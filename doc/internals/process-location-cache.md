# The process-level location table

Status: implemented by objid PR 3 (designed 2026-10-31, code 2026-11-02; Kale,
Claude). This chapter describes the code; section 9 lists what the first
version leaves out of the design and why. Reasons and alternatives:
`doc/objid64-design.md` section 7 and the decision log there.

Before PR 3 every PE kept its own copy of where elements are (`CkLocCache::locMap`),
its own index-to-id bindings for hashed-kind arrays (`CkLocMgr::idx2id`), and
asked the home PE on its own behalf. On a reconverse process with 64 PEs the same
element was looked up, requested and cached up to 64 times, every location update
that had to reach "the process" was 64 messages, and a send to an element one
core away could go to the home and back. PR 3 replaced the per-PE copies with
one table per process that all PEs read without taking a lock, and sends one
location request per process. Delivery stays per PE: a message is still handed
to the PE that owns the element, the element table and its record stay with
that PE, and buffered messages stay on the PE that buffered them.

```
   process (reconverse node), P PEs

   per PE, as today                     shared by the P PEs (new)
   +-----------------------------+      +--------------------------------------+
   | ArrayElement* by id         |      | location table   id -> {pe, epoch,   |
   |   (CkpvAccess(array_objs),  |      |                        state, waiters}|
   |    CkArray::localElemVec)   |      | index table      (coll, idx) -> id   |
   | CkLocRec by id (CkLocMgr::  |      |                  hashed kind only    |
   |   hash): index, LB handle   |      | pending index requests (coll, idx) ->|
   | buffered messages           |      |                  requesting processes|
   |   (bufferedIDMsgs, ...)     |      | tranche cursor per hashed array      |
   | LB database, reductions     |      +--------------------------------------+
   +-----------------------------+        lock-free reads; single-word CAS writes
```

## 1. What is shared and what stays per PE

| table | key | value | today | PR 3 |
|---|---|---|---|---|
| element pointers (`CkpvAccess(array_objs)`, `CkArray::localElemVec`) | id | `ArrayElement*` | per PE | per PE |
| `CkLocMgr::hash` | id | `CkLocRec*` (index, LB handle, timing) | per PE | per PE |
| `CkLocCache::locMap` | id | {pe, epoch} | per PE | **per process**: the location table |
| `CkLocMgr::idx2id` | index | id | per PE, hashed kind | **per process**: the index table |
| `CkLocMgr::retiredIds` | id | index | per PE, hashed kind | **per process**: `ck::objid::RetiredIds`, a map under a mutex, read on the miss path only (section 2.2) |
| `CkLocMgr::bufferedLocationRequests` | index | requesting PEs | per PE | per PE at the home (section 2.3) |
| `CkArray::bufferedIDMsgs`, `bufferedIndexMsgs`, `bufferedCreationMsgs` | id or index | messages | per PE | per PE, plus a waiter set in the location table |
| tranche cursor (hashed kind) | array | next unique number | per process already (object-ids.md section 4) | unchanged |
| LB database, reduction managers, `CkMigratable_initInfo` | | | per PE | per PE |

Why delivery stays per PE: every delivered message does one lookup of the
recipient id on the owning PE (`ck.C`, `_processArrayEltMsg`, then
`CkArray::recvMsg` and `lookup(id)`). That lookup is in thread-local memory and
unsynchronized. The shared table is consulted only on a miss, which is the order
`recvMsg` has today. The record (`CkLocRec`) carries load-balancing state that
only the owning PE touches. Messages are PE-owned memory and are sent from the PE
that buffered them.

Why one table per process rather than one per array: the key is the full 64-bit
id (collection number included), so one table serves every array and every
bound-array family; the table is sized once per process and its memory is
bounded by the number of distinct elements the process has addressed, not by
that number times the number of PEs.

## 2. The tables

### 2.1 Location table

Open-addressing hash table with linear probing, power-of-two capacity, two
64-bit words per slot plus a waiter word:

```
   slot:  key      atomic<uint64>   full id (ObjID::getID()); 0 = empty
          value    atomic<uint64>   epoch:31 | state:1 | pe+1:32 ; 0 = no location
          waiters  atomic<uint64>   bit r set: rank r of this process holds
                                    messages for this id (section 3.3)
```

The id is never 0 (collection numbers start at 1), so 0 marks an empty slot.
`pe+1` makes a stored PE 0 distinguishable from "no location": a value word of 0
means the key is known but has no location, which is how a death is recorded
(section 3.5). The state bit is reserved for an element that is arriving at
this process but has not landed yet (section 3.4, not used by the first
version).

Reads are lock-free and wait-free: hash the id, probe, load the key with acquire
ordering, compare, load the value. A reader never waits for a writer. The hot
send path (`whichPe`) is this read and nothing else.

Writes are single-word compare-and-swap operations, never a lock:

- insert a key: CAS the key word from 0 to the id. A writer that loses the race
  finds the same id in the slot and continues with the value.
- store a location (`LocTable::store`, W1 and W3): the writer holds the element,
  so the store is unconditional, as the per-PE insert was.
- update a location (`LocTable::update`, W4): load the value; if the new epoch
  is not greater than the stored epoch, stop; else CAS the value word. A failed
  CAS re-reads and repeats the epoch test, so "newer epoch wins" holds under any
  interleaving (this is `CkLocCache::updateLocation`'s compare, made atomic).
- record an emigration (`LocTable::recordEmigration`, W2): CAS {self, e} to
  {destination, e+1}; the entry must name the writer, which the CAS loop checks.
- clear (`LocTable::clear`, W5): CAS the value word to 0 while it still names
  the PE the element died on, at the epoch it died with; the key and the waiter
  word stay (section 3.5).
- register or take waiters: fetch_or and exchange on the waiter word. A store or
  update that gives the key a location takes the waiter bits and returns them to
  the caller, which sends the wakes (section 3.3).

Keys never move and never change once published, so a key that is in the table
is found by a plain lock-free probe, and only a miss enters the insert protocol
below. Growth adds a segment: when a segment's load passes 1/2 (a miss with
linear probing then costs about 2.5 slots on average, against 32 at 7/8, and
every lookup of a key in an older segment misses in the head first), a writer
allocates a segment four times as large and publishes it as the new head. Readers and writers probe the newest segment
first and stop at the first hit; a writer that finds no copy anywhere inserts
into the head segment. A key must not end up in two segments: updates and
waiter bits would then split between the copies, and a wake or an epoch could
be lost (objid_loctable phase 3 caught this on oversubscribed CI runners, where
a writer's search missed, the head moved, and its insert landed in the old
segment). So the search and the insert of a writer happen while the writer is
counted as active in the head segment, and a grower first seals the segment,
then waits until no writer is active in it, then publishes the bigger one. A
writer that finds the segment sealed after counting itself waits for the new
head and starts over there. Reads never touch the counter or the seal. Segments
are never freed during the run.

Eviction is not in the first version. The table grows with the number of
distinct elements the process has addressed, which is what each PE's cache does
today, divided by the number of PEs. The bounded-cache policy of the design
document (section 7.7: authoritative entries are never evicted, cached ones are
dropped by generation) needs a grace period before a retired segment's memory
can be reused, and is deferred until the first version is measured.

### 2.2 Index table (hashed kind only)

Same structure, keyed by (collection number, index). The index is up to
`CK_ARRAYINDEX_MAXLEN` words plus its dimension, so a slot holds the key words,
a 64-bit hash word that doubles as the empty marker, and the id as the value
word. A writer claims the slot by CAS on the hash word, then stores the index
words, then publishes the id with release ordering; a reader loads the id with
acquire ordering, and compares the index words only after it has seen a
non-zero id, so it never reads a half-written key. Index words are immutable
once published; the id word may change.

The id word holds the full id (collection number included): an element id alone
can be 0, which is the "not yet published" marker. A writer that finds the slot
claimed but unpublished waits for the publication, because the claim may be its
own key; a reader treats such a slot as "no binding yet" and goes the slow way
(a request to the home), which is correct and rare.

The id word changes when an index is rebound: an element deleted and inserted
again in a process that never saw it gets a fresh unique number (object-ids.md
section 3). The CAS that rebinds reports the old id, which the writer retires
(`CkLocMgr::retireId`): it goes into `ck::objid::RetiredIds`, one map per
process, under a mutex, read only when a message by id finds no element
(`CkLocMgr::recoverIndex`), never on a send. The home records every dead
element's id there as well (`noteDead`), as it did per PE. Any PE of the
process that holds a message for the old id re-addresses it to the current id
(`CkArray::handleUnknownByID`, and `bufferForLocation` before it buffers), and
the writer that retired the id wakes the ranks waiting on it (section 3.3).

Within a process a rebinding is therefore visible to every PE at once, so a PE
of the process that re-creates an index its process has heard of reuses the
existing id; only a process that never addressed the index mints a new one.
`tests/charm++/objid_insert` phase 7 checks both outcomes.

Packed-kind arrays have no index table: compress and decompress are pure
functions of the index and the array's bounds.

### 2.3 Pending index requests

Requests by index that the home cannot answer yet (the element does not exist)
are parked at the home PE in `CkLocMgr::bufferedLocationRequests`, keyed by
index, as before: the request is addressed to the home PE and the answer comes
from the same PE when the element registers there (`CkLocMgr::updateLocation`),
so a per-PE table is consistent and nothing is shared. The design's shared
pending table is not needed while requests are addressed to PEs (section 9).

## 3. Protocols

### 3.1 Who writes, and when

| event | writer | writes |
|---|---|---|
| creation (W1) | the creating PE | location {self, epoch 0}; hashed kind: index binding |
| emigration (W2) | the source PE, before the migration message is sent | location {destination, epoch+1}: a forwarding entry, kept |
| immigration (W3) | the destination PE | location {self, epoch from the message} |
| update from another PE or process (W4) | the PE that receives the update or the reply | location, by epoch compare; hashed kind: index binding |
| death (W5) | the owning PE, and the home PE that handles the reclaim | value 0 (no location) while the entry still names the dying PE and its epoch; waiter bits kept; hashed kind: index binding kept, id retired |

Every write is one CAS or one store; the writer sends messages (wake-ups,
replies) after it, never while holding anything. W2 is written before the
migration message exists: a destination PE in the same process can dequeue the
message and write W3 before the source PE returns from the send, and the W2
CAS checks that the entry still names the source (the first test run of the
table found this order wrong). Intra-process migration from PE b to PE c is
then W2 {c, e+1} by b and the same value by c's W3, in that order.

### 3.2 Delivery by id and the repair rule

Unchanged in shape (object-ids.md section 3): a sender reads the location table;
a hit sends to that PE, a miss sends to the home. A PE that receives a message
for an element it does not hold reads the table and forwards along the entry;
the forwarded delivery teaches the sender (`CkLocMgr::multiHop`, after one
forward). What changes is the unit the repair is addressed to: the repair goes
to the sender's **process** and is written once into its table, so every PE of
that process is corrected by one message. Likewise a send from any PE of the
process to an element held by another PE of the same process finds the entry
written by W1 or W3 and pushes the message to that PE (`CmiPushPE`) without
leaving the process; today that send goes to the home and back when the sending
PE has never heard of the element.

### 3.3 One request per process, one reply per process

A PE that misses on an id it must deliver to (a buffered send, demand creation)
registers as a waiter (`CkLocCache::requestLocation`): it sets its rank bit in
the slot's waiter word with fetch_or, reads the value again, and, if the word
was 0 before and the value is still empty, sends the request. A request is
therefore sent once per process per unresolved element, by the first PE to
miss. The request goes to the home PE and the reply to the requesting PE, as
group entry methods of `CkLocCache` (the design's nodegroup is not needed for
this; section 9); the reply writes the location (W4), which takes the waiter
bits, and the writer sends each rank that had a bit set a small wake message
(`CkLocCache::wake`, one message per rank, which reconverse delivers as a push
onto that PE's queue); each woken rank flushes its own buffered messages for
that id (`CkArray::sendBufferedMsgs`). When the home is in the requesting PE's
own process no request is sent: the home's table is this table, and the write
that gives the element a location, by the creating PE or by the home PE on an
update from elsewhere, wakes the waiters. Registration and fill cannot miss
each other: a PE registers after its own read missed, and the filler takes the
waiter bits after its CAS succeeded; a PE whose registration lands after the
fill reads the table again before buffering and finds the entry, and then
sends instead of buffering (`requestLocation` returns the PE) and removes its
own bit again, so no bit outlives the fill and a later miss on the same key is
"first" again. A wake to a rank that has nothing buffered is a no-op.

The rule every path must keep: a message a PE holds is either under an id
whose waiter word has the PE's bit, or under an index with a request out to
the home. The wake by id is what releases the first kind; the home's reply by
index releases the second. Four places hand a message from one kind to the
other or could drop it, and each is written to keep the rule:

- A message to an index this process has an id for is held under the id, never
  by index (`CkArray::bufferForLocation`); messages held by index before the
  process learned the id join the id's queue when the next send finds the id.
  Only an index with no id in the process is held by index, and that send asks
  the home (`CkLocMgr::requestLocation(idx)`), whose reply by index releases
  it. The reply releases what is held under the id too.
- A retired id (section 2.2) never gets a location, so the PE that rebinds an
  index also takes the waiter bits of the old id's slot and wakes those ranks.
  The woken PE recovers the index, looks up the current id and re-addresses
  what it holds (`CkLocMgr::releaseRetired`, `CkArray::releaseRetiredMsgs`).
  The rebinding PE binds before it writes the new location, so a woken rank
  can find the new id without a location yet: it then holds the messages under
  the new id and registers as its waiter, and the write releases them. The
  rebinding PE itself does the same for what it holds (immigration, restore
  and resume rebind before they create the element).
- A wake can find the entry cleared again (the element died between the fill
  and the wake). The fill took the rank's bit, so the rank registers again
  while it still holds messages (`CkLocCache::wake`), and the next store, by
  the re-creation, wakes it.
- Death (W5) clears the value but leaves the waiter word: ranks that registered
  while the location was unknown are woken by the next store, and their request
  to the home, if one went out, is held there until the element exists again.

The sender-repair rule (section 3.2) skips a sender in the delivering PE's own
process, since that sender reads the table the landing wrote
(`CkLocMgr::multiHop`). Where a PE reports a move or a location, it passes the
entry it wrote (`informHome`, the `CMK_GLOBAL_LOCATION_UPDATE` broadcast,
`requestLocation` by index) rather than re-reading the table, which another PE
of the process may have changed in between; `requestLocation(id)` returns the
PE it saw for the same reason.

With more than 64 PEs in a process, bit r covers ranks r, r+64, ...; a wake to
the group is delivered to each rank in it, and the spare wakes are no-ops.
Correctness does not depend on the process size: the first PE to set any bit
still sends the one request, and a wake to a rank that has nothing buffered
costs one local message. Processes above 64 PEs are realistic (one process per
node on Frontier is 56 or 64, Aurora 104, El Capitan 96, Perlmutter CPU nodes
128), so the alternative was considered: k = ceil(ranks/64) waiter words per
slot, with the stride computed at init. With k words the condition "send if the
word was 0 before" is no longer one atomic check, because two PEs setting bits
in different words each see their own word go from 0 to nonzero; it needs a
separate request-sent flag, set by fetch_or after registering, with only the PE
that flips it sending, and the fill clears k words plus the flag. Neither
version touches the lookup path. The first version keeps the single word and
the fold; the cost is at most one spare wake per rank per fill at 128 PEs, and
fills happen once per process per unknown element. Switch to k words if a
profile of a 128-PE process shows the spare wakes.

Home-side bookkeeping is unchanged: `informHome` after creation or emigration
is one message to the home PE, which also binds the index there (hashed kind)
and answers the requests it holds by index. Addressing it to the home process
is part of the later step in section 3.7.

### 3.4 An element in flight (designed; not in the first version)

A message can reach the destination PE of a migration before the element has
landed. The destination PE's table still names the source PE, which forwards
straight back, and the message bounces until the landing (Aditya's branch
measured 234k forwards for 1,350 moves in one balancing step and added per-PE
parking of such messages, commit 54ff0156c on rate-aware-gpu-lb). The design
has the destination process mark the entry as arriving when it learns of the
move (from the balancer's decision, or from the migration message's first
fragment on branches that stream payloads): value {destination PE, epoch+1,
state = arriving}. The first version does not write this state, because on
recharm nothing learns of a move before the migration message lands: the
balancer's global update is compiled out and payloads are not split. The state
bit is reserved in the value word, and the device-payload split of Aditya's
branch is where the first writer belongs. A PE of that process that reads an arriving entry parks the
message in its own buffered queue and registers as a waiter; the landing (W3)
clears the state and wakes the waiters. No message leaves the process while the
element is on its way in. The source process's forwarding entry (W2) is written
at the same time, so a message that still reaches the source is forwarded once,
to the destination, and parked there.

Order across a move is not preserved, today or in PR 3: a message the sender
issues after it learns the new PE can reach the element before an older message
still queued on the source PE and forwarded from there. Charm++ does not promise
order here. PR 3 makes the sender switch sooner (its process's table is written
by the first repair, or by the source PE itself when both are in one process),
so this window is exercised more often; the test in section 7 covers it.

### 3.5 Death

The owning PE clears the entry: value 0, key and waiter bits kept (W5); the
home does the same when told (`reclaimRemote`, which carries the PE and the
epoch the element died with). Both clears are conditional on the entry still
naming that PE and epoch, because the entry is the process's: an index can be
re-created under the same id (packed kind, or hashed with the id reused in the
process) before a late death notice arrives, and the live entry must stay. The
remaining gap is a re-creation on the same PE at the same epoch (epoch 0, never
migrated) with the notice still in flight; closing it needs the creation flag
of the tombstone design below. The home keeps the index binding and records
the dead id as retired (section 2.2). Keys are never reclaimed in the first
version, which matches the unpruned per-PE `retiredIds` before it.

A creation stores epoch 0 unconditionally (W1), also over an entry that still
holds the epoch of the element's earlier life; the per-PE assert that epochs
never went backwards is gone with the per-PE cache.

The design's tombstone with a death epoch, which would reject a late update
from before the death, is not written. A packed-kind element re-created under
the same id starts again at epoch 0, so an epoch-carrying tombstone would also
reject the re-creation's own updates at the home and at every process that saw
the death; telling them apart needs a creation flag on the home update, which
is left for a later version (section 9). The race it would close, an update
from before the death reviving a dead entry, exists in the per-PE code as well
and is healed by the repair rule like any stale entry.

### 3.6 Device zero-copy sends to an element that moved

A device send picks its transfer mode on the sender from the destination PE it
believes in (`CkRdmaDeviceOnSender`, called from generated code with
`lastKnown(index)`): same process means the receiver may read the sender's
device pointer directly, same host means an IPC handle, anything else means the
network. Today the receiver aborts when the process it runs in is not the one
the sender assumed (`ckrdmadevice.C`, "Destination process does not match the
one the sender determined. Please enable CMK_GLOBAL_LOCATION_UPDATE"), and the
advice is stale: no arch header defines that flag. The reason the question is
hard is that the gap between preparing the payload and consuming it is one no
table can close, since the element can move in between.

PR 3 settles the contract in two parts. First, the sender's belief comes from
the process table, so "is the target in my process" is exact at the moment of
the read, and an arriving entry (section 3.4) counts as "not here yet" and
selects the staged path. Second, a device-send message that is **forwarded** out
of the process is repaired by the forwarder, which is the process that owns the
source buffer and can re-describe it: for a destination on the same host it
exports the buffer and rewrites the descriptor as an IPC one; for another host
it registers the buffer and rewrites the descriptor as a network get; with one
PE per process the forward is bounced back to the source PE, which does the
same. A receiver never aborts on a process mismatch: a mismatch on a message
with zero hops is a protocol error and aborts with the actual facts; a mismatch
on a forwarded message means the forwarder did not repair it, and the receiver
asks for the bytes again (the existing restage path). This is the protocol
Aditya's branch implements and has run on Frontier (commits d1b83b4ec,
c0741cb49 and f941fd851 on rate-aware-gpu-lb); PR 3 takes the contract and the
abort text, not the device code, which lands with his branch.

### 3.7 Moves within a process (a later step, after the first version)

Proposed by Kale 2026-10-31. Once the table is per process, a move from PE b to
PE c of the same process need not be reported to the home: the home's entry
still names a PE of the right process, and that PE's table forwards to c with
one queue push. The home's invariant weakens from "knows the element's PE" to
"knows the element's process", which is all a remote sender needs to reach the
table that knows the PE. This removes one home update per intra-process move;
under node-level balancing most moves are of this kind, so it is the bulk of the
balancing-step location traffic. The repair rule of section 3.2 is unchanged:
the first message from a remote process that is forwarded inside the
destination process still teaches that process the PE, so later sends from it
go to c directly. A sender in the same process needs no repair at all, since
the write at emigration (W2) is in the table it reads.

Two conditions make this sound. First, a forward inside a process must not
count as a hop. Today's rule in `CkArray::recvMsg` sends a message to the home
after its second hop rather than follow a chain of stale entries; with the home
no longer told about intra-process moves, a message bounced to the home could
be sent back to the same process and forwarded inside it again, and the
hop-count rule would alternate between the home and that process. If only
cross-process forwards count, the rule keeps its meaning: after two
cross-process misses the message goes to the home, and the home's entry always
names the right process because cross-process moves are still reported.
Second, the epoch argument holds without the home's knowledge of the local
move: the local table holds {c, e+1} while the home holds {b, e}; a later
cross-process move writes {d, e+2} both locally and at the home, which the
home accepts by epoch compare, and a remote process holding a repaired
{c, e+1} rejects a stale {b, e} answer from the home as older. Deaths
(section 3.5) are still reported to the home, since the home answers requests
for the index.

Deferred from the first version so that PR 3 can be measured against today's
traffic with the home's knowledge unchanged; the restart-traffic measurement
planned after PR 3 gives the before and after. The device contract (section
3.6) needs one check with Aditya for this step: a device send addressed to b
and forwarded to c stays in the same process, so the transfer mode chosen for
b (same-process IPC or direct pointer) is the one c needs as well.

## 4. Objects and messages

One location table, one index table and one retired-id map per process
(`ck::objid::locTable()`, `indexTable()`, `retiredIds()` in
`src/ck-core/cklocnode.h`), created on first use after the command line is
parsed and never destroyed; no reference counting, since every array shares
them. Their keys are full ids (collection number included) or (collection
number, index), so one set serves every array and every bound-array family.

The messaging side keeps the per-array group `CkLocCache`, which no longer holds
a map: it carries the manager pointer (for the home and the table key), the
per-PE listeners (the array's `sendBufferedMsgs`), and the entry methods
`requestLocation(id, peToTell)`, `updateLocation(entry)` and `wake(id)`.
`CkLocMgr` stays a per-PE group branch per array and keeps its record table,
its compressor and bounds, its map, and its buffered index requests.

Non-SMP (one PE per process) runs the same code with uncontended atomics; there
is no second code path.

## 5. Checkpoint, restart, shrink and expand

Nothing in the shared tables is checkpointed. Elements register with their homes
when restored (`CkLocMgr::restore` and `resume`), which rebuilds every location
entry and every index binding on the new process count, as the per-PE caches
are rebuilt today (`CkLocCache::pup` is empty for this reason). The tranche
cursor keeps its existing treatment (object-ids.md section 5).

## 6. Limits and diagnostics

- Table capacity: initial size from `+objid_table_log2` (default 2^16 slots for
  each table), growth by segments of four times the size, started when a
  segment passes 1/2 full. Memory is 24 bytes per location slot and 40 bytes
  per index slot at `CK_ARRAYINDEX_MAXLEN` 3.
- The index table stores `CK_ARRAYINDEX_MAXLEN` words per index, so an index
  wider than that cannot exist, as before.
- `+objid_diag` (new) counts table events and prints one line per process at
  exit: slots, segments and used slots of both tables, retired ids; location
  misses, and misses by index split into "no id for this index" and "id known,
  location unknown" (the opt-in counters of Aditya's commit e417584dd); requests
  sent and requests skipped because another PE of the process had asked; fills
  that took waiter bits and wakes sent; repairs skipped for a sender in the same
  process; growths. The counters are relaxed atomics touched only when the flag
  is set. The same commit's per-component message on the index compressor's
  overflow assertion is in `FixedArrayIndexCompressor::compress`.

## 7. Tests

Existing: `tests/charm++/anytime_migration`, `anytime_bcastred`,
`atsync_reduction`, `unbounded_migration`, `unbounded_restart`, `objid_insert`,
`objid_tranche`, `objid_home`, `dynamic_insertion_deletion`, `demand_creation`,
`megatest`, run in one, two and four processes. New with PR 3:

- `tests/charm++/objid_migrate6`: a 6-component index (`CkArrayIndex6D`) that
  migrates, which the tree lacked (every migrating test used a small bounded
  array; every unbounded or custom-index example never migrated). Hashed kind
  without bounds, packed kind with `-b`; elements inserted from every PE;
  `migrateMe` rounds, or `AtSync` under a balancer (`-lb`), or a stream of
  messages to one element moving between the PEs of its process (`-stream`,
  the intra-process case of section 3.4), checking that every message is
  delivered once (counts, not order). `+objid_table_log2 4` forces growth.
- `tests/charm++/objid_loctable`: the tables themselves, on private instances,
  under every PE of the process at once: concurrent updates never show an
  older epoch; a 16-slot table grows under contention without losing a key;
  exactly one PE per process is told it registered first as a waiter and a
  fill takes every PE's bit once; concurrent binds of one index report "no
  previous binding" exactly once per process, a rebinding reports the old id,
  and lookups afterwards give the new one.
- `objid_insert` phase 7 now distinguishes a fresh PE whose process never
  addressed the index (three or more processes: a new id) from one whose
  process did (the old id is reused).

## 8. Where in the source

| what | where |
|---|---|
| location table, index table, retired ids, `+objid_diag` report | `src/ck-core/cklocnode.h`, `cklocnode.C` (namespace `ck::objid`: `LocTable`, `IndexTable`, `RetiredIds`) |
| `CkLocCache` (messages, listeners, wakes) and `CkLocMgr` (bindings through the tables, `retireId`, `releaseRetired`, W2 before the send) | `src/ck-core/cklocation.h`, `cklocation.C`, `cklocation.ci` |
| request-or-send on a miss, re-addressing under a rebound id, buffered-message flush | `src/ck-core/ckarray.C` (`handleUnknownByID`, `bufferForLocation`, `sendBufferedMsgs`, `releaseRetiredMsgs`) |
| `+objid_table_log2`, `+objid_diag` | `src/ck-core/init.C` |
| compressor overflow message | `src/ck-core/ckarrayindex.h` |
| device-send repair contract (section 3.6) | `src/ck-core/ckrdmadevice.C`, with Aditya's branch |

## 9. What the first version leaves out

- The arriving state (section 3.4): nothing on recharm learns of a move before
  the migration message lands; the bit is reserved.
- Epoch-carrying tombstones (section 3.5): a re-created packed-kind element
  restarts at epoch 0, so they need a creation flag on the home update first.
- Messages addressed to processes (the design's `CkLocNode` nodegroup, section
  4): requests go to the home PE and replies to the requesting PE, one per
  process; `informHome` goes to the home PE. The traffic saving is one request
  per process already; addressing the home process is the step in section 3.7,
  which also removes the home update for moves within a process.
- A shared table of pending index requests (section 2.3): not needed while
  requests are addressed to PEs.
- Eviction (design document section 7.7): deferred until this version is
  measured. Entries, index bindings and retired ids are never removed, so a
  program that creates and deletes many elements or arrays grows the tables
  for the whole run (a per-PE map grew the same way, once per PE).
- A late death notice for an element re-created on the same PE at epoch 0
  (section 3.5): the conditional clear cannot tell the two lives apart; the
  creation flag of the tombstone design would.
- Above 64 ranks per process, a rank that drops its own waiter bit after it
  found the location also drops the bit of the ranks it is folded with (r+64,
  ...). A folded rank loses its wake only if it registered after a fill and a
  death of the same id in between; the k-word alternative in section 3.3
  removes the fold.
- Under `CMK_GLOBAL_LOCATION_UPDATE`, rank 0 of each process applies the
  balancer's announced move to the shared table, and no rank does in the
  source's process, where the emigrating PE records it. That build mode has
  no CI job; the rule is reasoned from the code, not tested.
