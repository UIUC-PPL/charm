# The process-level location table

Status: design for objid PR 3, written 2026-10-31 (Kale, Claude). Not
implemented. When PR 3 lands this chapter becomes the description of the
code and the status line goes; until then the current code is what
[object-ids.md](object-ids.md) section 3 describes: every table per PE.
Reasons and alternatives: `doc/objid64-design.md` section 7 and the decision
log there.

Today every PE keeps its own copy of where elements are (`CkLocCache::locMap`),
its own index-to-id bindings for hashed-kind arrays (`CkLocMgr::idx2id`), and
asks the home PE on its own behalf. On a reconverse process with 64 PEs the same
element is looked up, requested and cached up to 64 times, every location update
that must reach "the process" is 64 messages, and a send to an element one core
away can go to the home and back. PR 3 replaces the per-PE copies with one table
per process that all PEs read without taking a lock, and routes location
traffic to processes instead of PEs. Delivery stays per PE: a message is still
handed to the PE that owns the element, the element table and its record stay
with that PE, and buffered messages stay on the PE that buffered them.

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
| `CkLocMgr::retiredIds` | id | index | per PE, hashed kind | **per process**: folded into the index table (section 2.2) |
| `CkLocMgr::bufferedLocationRequests` | index | requesting PEs | per PE | **per process**: requesting processes |
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
`pe+1` makes a stored PE 0 distinguishable from "no location"; `pe+1 == 0` with
a non-zero epoch is a tombstone (the element died, section 3.5). The state bit
marks an element that is arriving at this process but has not landed yet
(section 3.4).

Reads are lock-free and wait-free: hash the id, probe, load the key with acquire
ordering, compare, load the value. A reader never waits for a writer. The hot
send path (`whichPe`) is this read and nothing else.

Writes are single-word compare-and-swap operations, never a lock:

- insert a key: CAS the key word from 0 to the id. A writer that loses the race
  finds the same id in the slot and continues with the value.
- update a location: load the value; if the new epoch is not greater than the
  stored epoch, stop; else CAS the value word. A failed CAS re-reads and repeats
  the epoch test, so "newer epoch wins" holds under any interleaving (this is
  `CkLocCache::updateLocation`'s compare, made atomic).
- register or clear waiters: fetch_or and exchange on the waiter word.

Keys never move and never change once published. Growth adds a segment: when a
segment's load passes 7/8, a writer allocates a segment four times as large and
publishes it with an atomic exchange of the head pointer. Readers and writers
probe the newest segment first and stop at the first hit; a writer that finds
no copy anywhere inserts into the head segment. A duplicate across two segments
can arise only when a check-then-insert straddles a head change; from then on
every update goes to the copy in the newer segment, and at most the one update
that raced the growth is invisible until the next one. That is the same
staleness a per-PE cache has today after any migration, and the same repair
fixes it (section 3.2). Segments are never freed during the run.

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

The id word changes when an index is rebound: an element deleted and inserted
again on a PE that never saw it gets a fresh unique number (object-ids.md
section 3). The old id is then retired: the location table keeps its slot as a
tombstone whose value names the index table slot, which is what
`CkLocMgr::retiredIds` holds per PE today. Any PE of the process that holds a
message for the old id re-addresses it to the current id (today's
`CkArray::handleUnknownByID`), and the home sends the original sender the new
binding by index, as today.

Packed-kind arrays have no index table: compress and decompress are pure
functions of the index and the array's bounds.

### 2.3 Pending index requests

Requests by index that the home process cannot answer yet (the element does not
exist) are parked in a small table keyed by (collection, index) whose value is a
list of requesting processes. It is written only on the home process, by the
PE that dequeued the request, under a narrow per-slot claim (CAS on the slot's
hash word, as in the index table); it is not on any send path.

## 3. Protocols

### 3.1 Who writes, and when

| event | writer | writes |
|---|---|---|
| creation (W1) | the creating PE | location {self, epoch 0}; hashed kind: index binding |
| emigration (W2) | the source PE | location {destination, epoch+1}: a forwarding entry, kept |
| immigration (W3) | the destination PE | location {self, epoch from the message}; clears the arriving state |
| update from another process (W4) | whichever PE dequeues it | location, by epoch compare; hashed kind: index binding |
| death (W5) | the owning PE, and the home's PE that handles the reclaim | tombstone {pe -1, death epoch}; hashed kind: index binding kept |

Every write is one CAS; the writer sends messages (wake-ups, replies) after the
CAS, never while holding anything. Intra-process migration from PE b to PE c is
two writes of the same value {c, e+1} in either order; both are epoch-compared,
so the order does not matter. Today's `recordEmigration` increments in place and
asserts the entry still names the source PE, which is only safe because nothing
is concurrent per PE.

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
registers as a waiter: it sets its rank bit in the slot's waiter word with
fetch_or and, if the word was 0 before, sends the request. A request is
therefore sent once per process per unresolved element, by the first PE to
miss. The request is addressed to the home process (`CmiSyncNodeSend`, dequeued
by whichever PE of the home process takes it from the node queue); the home
answers from its table, so the home never needs the element's own PE to be the
one that answers. The reply is addressed to the requesting process; the PE that
dequeues it writes the location (W4), exchanges the waiter word to 0 and sends
each rank that had a bit set a small wake message (`CmiPushPE`); each woken rank
flushes its own buffered messages for that id (`CkArray::sendBufferedMsgs`).
Registration and fill cannot miss each other: a PE registers after its own read
missed, and the filler takes the waiter bits after its CAS succeeded; a PE whose
registration lands after the fill reads the table again before buffering and
finds the entry. A wake to a rank that has nothing buffered is a no-op.

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

Home-side bookkeeping moves with it: `informHome` after creation or emigration
is one message to the home process, not the home PE; a home process holds the
directory entries of every element whose home PE is one of its PEs, and any of
its PEs can answer. The global location update of the load balancer
(`CMK_GLOBAL_LOCATION_UPDATE`, compiled out in every build on recharm) would
become one message per process if it is ever turned on.

### 3.4 An element in flight

A message can reach the destination PE of a migration before the element has
landed. Today the destination PE's cache still names the source PE, which
forwards straight back, and the message bounces until the landing (Aditya's
branch measured 234k forwards for 1,350 moves in one balancing step and added
per-PE parking of such messages, commit 54ff0156c on rate-aware-gpu-lb). In
PR 3 the destination process marks the entry as arriving when it learns of the
move (from the balancer's decision, or from the migration message's first
fragment on branches that stream payloads): value {destination PE, epoch+1,
state = arriving}. A PE of that process that reads an arriving entry parks the
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

The owning PE writes a tombstone {pe -1, death epoch} instead of erasing, so a
late update with an older epoch is rejected by the epoch compare and cannot
re-create a ghost entry (today `locMap[id]` is written unconditionally and the
entry is erased, which lets an older update in flight revive it). The home keeps
the index binding, as today, and records the retired id (section 2.2).
Tombstones are never reclaimed in the first version, which matches today's
unpruned `retiredIds`.

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

One table object per process, created at process start before any array
exists (`initnode` of the ck module) and never destroyed; no reference
counting, since every array shares it. The messaging side is one nodegroup,
`CkLocNode`, with entry methods that run on whichever PE of the process dequeues
them: `requestLocation(collection, id, requestingProcess)`,
`requestLocation(collection, index, requestingProcess)`,
`updateLocation(collection, entry)`, `updateLocation(collection, index, entry)`,
`repair(collection, entry)`. `CkLocCache` as a per-array group disappears; its
listeners (the array's `sendBufferedMsgs`) are called on the woken PE by the
wake handler. `CkLocMgr` stays a per-PE group branch per array and keeps its
record table, its compressor and bounds, and its map.

Non-SMP (one PE per process) runs the same code with uncontended atomics; there
is no second code path.

## 5. Checkpoint, restart, shrink and expand

Nothing in the shared tables is checkpointed. Elements register with their homes
when restored (`CkLocMgr::restore` and `resume`), which rebuilds every location
entry and every index binding on the new process count, as the per-PE caches
are rebuilt today (`CkLocCache::pup` is empty for this reason). The tranche
cursor keeps its existing treatment (object-ids.md section 5).

## 6. Limits and diagnostics

- Table capacity: initial size from `+objid_table_log2` (default 2^16 slots per
  table), growth by segments of four times the size; the number of segments and
  the slot count are printed with the existing `+objid_diag` summary at exit.
- A hashed-kind index wider than `CK_ARRAYINDEX_MAXLEN` words cannot be stored
  and is refused at insertion, as today.
- Counters, printed with `+objid_diag`: location misses split into "no id for
  this index" and "id known, location unknown"; requests sent and replies
  received per process; wakes sent and wakes that found nothing buffered;
  messages parked for an arriving element; repairs received. (The first two are
  the opt-in counters from Aditya's commit e417584dd, which also added a
  per-component message to the index compressor's overflow assertion; both come
  in with PR 3.)

## 7. Tests

Existing: `tests/charm++/anytime_migration`, `anytime_bcastred`,
`atsync_reduction`, `unbounded_migration`, `unbounded_restart`, `objid_insert`
(two processes), `megatest`. New with PR 3: a test that combines a
non-compressible index with migration, which the tree lacks (every migrating
test uses a small bounded array; every unbounded or custom-index example never
migrates): a custom 6-component index with `-DCK_ARRAYINDEX_MAXLEN=6`, dynamic
insertion, `migrateMe` rounds and a balancer run, checked on 1, 2 and 4
processes. A concurrency test for the table itself: all PEs of a process send
to the same 1,000 elements while they migrate within and out of the process,
with the counters above asserting one request per process per element. The
intra-process case of section 3.4: `migrateMe` between two PEs of one process
while a sender in the same process streams messages to the element, checking
that every message is delivered once (counts, not order) and that no request or
repair message leaves the process.

## 8. Where in the source (planned)

| what | where |
|---|---|
| location table, index table, pending requests | `src/ck-core/cklocnode.h`, `cklocnode.C` (new; namespace `ck::objid`) |
| `CkLocNode` nodegroup and its `.ci` | `src/ck-core/cklocnode.ci` (new) |
| the per-PE manager, now without `CkLocCache` | `src/ck-core/cklocation.h`, `cklocation.C` |
| waiter wake and buffered-message flush | `src/ck-core/ckarray.C` (`CkArray::sendBufferedMsgs`) |
| device-send repair contract | `src/ck-core/ckrdmadevice.C` |
