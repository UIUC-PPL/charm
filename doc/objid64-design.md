# 64-bit object id redesign: detailed design (charm #3994)

Status: draft 1, for discussion on the `objid-redesign` branch. Merging is deferred
until the reviewed-with-reconverse line is stable with users and the test suite is
more comprehensive; PR 0 of section 9 can land independently.

Draft 1, 2026-09-29 (Kale + Claude). Line references: reviewed-with-reconverse at
ebdc49c66. Companion analysis (why): doc/objid64-analysis.md. Decisions fixed by Kale
on 2026-09-29: stay at 64 bits; C = 12 build-time (AMPI may need 21 or 24); the
hashed payload is home key (width set per run from the process count and an
expand factor) plus a unique number handed out in tranches, no creator field; explicit bounds only, warn otherwise;
staged PRs; chares stay bound to individual PEs; drone mode is orthogonal.
Section 13 is the decision log; the main text describes the design as it stands.

Terminology: a *process* is an OS process, what Converse calls a node and counts
with CkNumNodes(); a *PE* is a worker thread, counted by CkNumPes(). Nothing in
this document is keyed on PEs except the per-PE tables named as such.

## 0. Goals

G1 A compressible (bounded, packable) index uses 49 payload bits instead of 16.
G2 One home per element, computable from the id alone or from the index alone,
   on any PE, under the current process count; no PE number stored in the id.
G3 Delivery by id never needs the index off the source PE; no linear reverse scan.
G4 Ids are stable across checkpoint/restart including shrink/expand; directories
   are rebuilt, not restored.
G5 Local (same-PE) insertion stays synchronous; creation path unchanged in shape.
G6 Directory and location cache become per process (PR 3); per-PE tables hold only
   id -> object pointer.
G7 Every capacity limit aborts with a message naming the build flag to raise.

## 1. Id layout (src/ck-core/objid.h)

    bit   63 62 61 | 60                      49 | 48                          0
         +---------+----------------------------+------------------------------+
         |  0 0 0  |   COLLECTION   (C = 12)    |      PAYLOAD   (64-3-C = 49) |  standard layout
         +---------+----------------------------+------------------------------+

    bit   63 62 61 | 60                      37 | 36            0
         +---------+----------------------------+----------------+
         |  0 0 1  |   COLLECTION   (24)        |  PAYLOAD (37)  |  wide layout (PR 1d, section 12)
         +---------+----------------------------+----------------+

    PAYLOAD of an array with bounds (packed kind, either layout; section 2):
         +----------------------------------------------------------------------+
         |              the element's index, packed into the field              |
         +----------------------------------------------------------------------+

    PAYLOAD of an array without bounds (hashed kind, standard layout only; section 3):
         +-----------------------+----------------------------------------------+
         |   HOME KEY (H bits)   |        UNIQUE NUMBER (49 - H bits)           |
         +-----------------------+----------------------------------------------+

Field by field:

    bit 63..61  TYPE_TAG        3   000 = standard layout, 001 = wide layout (PR 1d, section 12)
    bit 60..49  COLLECTION      C   default 12   -DCMK_OBJID_COLLECTION_BITS (build time)
    bit 48..0   PAYLOAD         64-3-C = 49

    wide layout (tag 001, opt-in per array with CkArrayOptions::setWideCollection()):
    bit 60..37  COLLECTION      24  fixed
    bit 36..0   PAYLOAD         37  packed index only (bounds required)

    PAYLOAD, packed-index kind (array has a compressor):
        the packed index, up to 49 bits (FixedArrayIndexCompressor budget)

    PAYLOAD, hashed kind (no compressor):        widths fixed PER RUN, not per build
        top H bits   HOMEKEY   H = ceil(log2(processes * expandFactor)), at most 24
        low U bits   UNIQUE    U = 49 - H; a number unique within the collection,
                               handed out in tranches (section 3.1); no creator field

H is computed at first launch from the process count and the expand factor,
stored in the checkpoint, and reused by every restart, because keys are embedded
in live ids. The expand factor (+objid_expand F, default 8) is headroom for
shrink/expand: the key space holds at least F times the launch-time process
count, so a restart can grow to that many processes and every process still owns
home keys. It has nothing to do with PEs per process, although since the
directory PE is chosen from the key within the home process (3.3), the keys per
process, about F, also bound how many PEs of a process serve as homes. If a
restart has more than 2^H processes it still works (home = key % processes is
always defined); only the directory spread is limited to 2^H processes, and the
runtime prints a warning. The code floors H at 1, which only a single process
with +objid_expand 1 would reach.

CMK_OBJID_HOME_BITS is removed. Build-time static_asserts: 1 <= C <= 40. Runtime
checks at startup and restart: H >= 1, U >= 16.

The three TYPE_TAG bits stay reserved: they were meant for a universal object
handle (analysis section 7). One value is used: tag 001 marks the wide layout,
which a decoder must know before it can find the collection (section 12). The
other six values stay free; they are not reclaimed for capacity.

Which kind a payload is comes from the collection: every PE's CkLocMgr branch knows
whether its array has a compressor. ObjID itself does not need a kind bit.

Capacities at the default C = 12: 4,096 collection ids (each chare array consumes
2-3: CkArray + CkLocMgr + CkLocCache, plus map/mcast if created); packed index up
to 2^49 (1D 5.6e14; 3D 65536^3 needs 48). Hashed-kind capacity per array per run:

| processes | expand 8 -> H | U | ids per array | initial tranche per process |
|---|---|---|---|---|
| 2 | 4 | 45 | 3.5e13 | 8.8e12 |
| 64 | 9 | 40 | 1.1e12 | 8.6e9 |
| 1,024 | 13 | 36 | 6.9e10 | 3.4e7 |
| 18,816 (Frontier, 2/node) | 18 | 31 | 2.1e9 | 32,768 |
| same, expand 2 | 16 | 33 | 8.6e9 | 131,072 |

The initial tranche is half of the unique space divided by the process count
(section 3.1); the other half is the allocator's pool for refills and for every
process after a restart. The small-process validation case is
solved: at 2 processes an array can hold trillions of elements. At full Frontier
scale the initial tranche is small, and its size is governed by the expand factor,
so the factor is a command-line choice, not a build constant.

The COLLECTION field holds a collection number, not a group id (since PR 1d; before
it the CkArray's CkGroupID, which counted every group in the program, so 4,096 was
reached after ~1,500 arrays). Numbers are dense, assigned per CkArray on PE 0 in
CkCreateArray from two counters (standard, wide; section 12.3), and are not
recycled: a program that creates arrays in a loop reaches 4,095 standard arrays and
aborts with a message naming setWideCollection and the build flag (12.4).

AMPI (when supported): --ampi-only currently sets C = 29 (buildcmake:619).
AMPI's arrays are not bounded today (no setBounds anywhere in
ampi.C or tcharm.C; createNewChildAmpiSync uses setNumInitial(0)), so under this
design every AMPI array is hashed-kind, and a C = 29 build has a 32-bit payload that
fails the U >= 16 startup check beyond 8,192 processes. Therefore
(section 12) AMPI arrays get bounds, and a tag-marked wide-collection kind (tag
001, 24 collection bits, 37 payload bits, opt-in per array) replaces the C = 29
build; implemented as objid PR 1d (2026-10-17).

ObjID API (as of PR 1d): the constructor takes (collection number, payload, wide);
isWide() reads the tag; getCollectionNumber() and getElementID() shift by the
layout's widths (one predictable branch); getElementID() is what
CkArrayMessage::array_element_id() and the location tables key on; getID() is the
whole word, tag included (delivery keys the per-PE object map on it). getHomeID()
is deleted (its only reader was CkLocCache::homePe, cklocation.h:378). New: a process-wide `ck::objid::Layout {H, U}` set at startup
or restored at restart; helpers hashKey(payload) = payload >> U,
unique(payload) = payload & ((1<<U)-1), makeHashed(key, unique). Constructor abort
messages updated to name the payload split.

## 2. Packed-index kind (arrays with bounds)

Unchanged mechanism, wider budget:
- FixedArrayIndexCompressor::make (ckarrayindex.h:367): budget test becomes
  `sum > ck::ObjID::bits::PAYLOAD_BITS`. compress() bound check likewise. (1d: the
  budget is a parameter, the array's payload width: 49 for the standard layout, 37
  for a wide-collection array; CkLocMgr passes its own.)
- CkLocMgr::lookupID (cklocation.h:572, 597): `id = compressor->compress(idx)`.
  The home is no longer added; the two CmiAssertMsg on home/id size go.
- lookupIdx for this kind = decompress (unchanged).
- Home: homeProcess(idx) = CkNodeOf(map->homePe(mapHandle, idx)); homeProcess(id) =
  homeProcess(decompress(id)). Directory PE (PR 1) = map->homePe(idx) exactly as today,
  so compressible arrays see no behavioural change in PR 1 beyond the wider budget --
  except the sender-repair rule of 4.3, which applies to both kinds: a forwarded delivery
  now teaches the sender the location (measured in doc/objid64-perf.md).
- checkInBounds unchanged.
- Warning (decision: no bounds inference): in CkLocMgr's constructor, if
  `compressor == nullptr` and `opts.numInitial` or `opts.end` is non-empty, print
  once per array on PE 0: "Array <gid> was sized (<dims>) but has no bounds; ids
  use the hashed scheme. Call CkArrayOptions::setBounds(...) if the index space is
  fixed." Also when bounds exist but exceed the budget: "bounds need <sum> bits,
  budget is <PAYLOAD_BITS>". Both are CkPrintf, not aborts.

## 3. Hashed kind (arrays without a usable compressor)

### 3.1 Minting (creator side, synchronous; tranches)

Each process holds, per collection, a current tranche [cur, end) with an atomic
cursor, and a slot for the next tranche. getNewObjectID(idx) (cklocation.C:2642),
non-compressible branch:

    key = ck::indexHashKey(idx)                    // section 3.2, H bits
    u   = tranche.cursor.fetch_add(1)              // any PE of the process
    if (u >= tranche.end) -> swap in the next tranche if it has arrived (CAS on the
                              slot), retry; else fall back (below)
    if (u - tranche.cur >= tranche.len/2 and no request in flight) -> request next
    id  = ObjID::makeHashed(key, u)
    insertID(idx, id)

Tranche state lives in a process-shared table (CksvAccess) keyed by CkLocMgr gid,
created under a CmiNodeLock when the CkLocMgr branch is constructed. The per-PE
`idCounter` member and its pup (cklocation.C:2454, 2515) are removed.

Unique space [0, 2^U) per collection:
- Initial region [0, 2^(U-1)): at first launch process p owns [p*S, (p+1)*S) with
  S = 2^(U-1-ceil(log2 processes)); no message needed, so populateInitial and any
  insertion in the array's constructor phase mint immediately on every process.
- Allocator region [2^(U-1), 2^U): handed out top-down by the allocator, which is
  the CkLocMgr branch on PE 0 (its cursor `trancheTop` is pupped there; PE 0's
  group copy is what shrink/expand restarts restore, ckcheckpoint.C:879, so the
  allocator state survives every restart consistently). Entry methods:
  `requestTranche(int proc, int wantLog2)` on PE 0, reply `grantTranche(base, len)`
  to rank 0 of proc, which installs it in the next-tranche slot. Grant size:
  max(S, requested), doubling on each successive request from the same process,
  capped so the region is not exhausted by one requester.
- After ANY restart every process discards its old tranche and takes its first one
  from the allocator: rank 0 of each process sends requestTranche during
  CkPupArrayElementsData unpacking, and the restart path waits for all grants
  before _initDone releases user code (restart already barriers there).

Refill: requested when half of the current tranche is used, so a process has half a
tranche of headroom while the request is in flight. Exhaustion before the grant
arrives (a bulk insertion loop of more than S/2 elements without returning to the
scheduler, at large scale) falls back to deferred insertion: the creation is queued
in the process's pending list and completed by grantTranche. Only a local `[inline]`
insert observes this (the element is not there when insert returns); the runtime
prints a one-time warning naming +objid_expand and the tranche size. Remote
insertions are asynchronous anyway.

Possible future mitigation (Kale, 18:22): block the inserting call until the next
tranche arrives, by running the scheduler re-entrantly or by making the allocator
reply through a path the blocked PE can poll. Complicated (re-entrancy inside an
entry method, and the allocator process may itself be inside a long entry method);
not part of this design. The deferred-insert fallback stays the v1 behaviour.

Uniqueness: u is unique within the collection by construction (disjoint tranches).
Two different indices may share a key; the key takes no part in uniqueness.

Implementation notes (PR 1b, 2026-09-30), where the code departs from or refines the
text above:
- Deferred insertion is a re-send, not a queue of half-built elements: when minting
  fails, CkLocMgr::registerNewElement returns null and CkArray::insertElement hands
  the constructor message and index to CkLocMgr::deferInsertion on that PE;
  grantTranche (rank 0 of the process) wakes every PE that deferred
  (resumeDeferredInsertions), and the PE re-runs insertElement. Demand creation and
  remote insertion go through the same entry, so they are covered.
- The fast path is lock-free: read `end` (acquire), then fetch_add the cursor; a
  tranche is installed by storing the cursor first and `end` (release) second, so a
  number taken against a stale cursor fails the bound test and is wasted, never
  reused. The slow path (swap in the spare, request, register as waiter) holds
  _nodeLock. At most one spare is held; the request goes out when half of the
  current tranche is used, and again whenever a process is exhausted.
- The allocator hands out the pool bottom up in powers of two: the requester asks
  for twice its current tranche, or for a first grant when it has none; PE 0 clamps
  to a floor and a cap. The cap is region / (8 * processes). The floor, which is the
  first grant after a restart, is 2^16 (FIRST_GRANT_LOG2) or cap / 64, whichever is
  smaller, so a restart on which every process inserts costs the pool at most
  processes * cap / 64 = region / 512 at any process count. A zero-length grant means
  the pool is spent and the requester aborts with a message naming setBounds,
  +objid_expand and CMK_OBJID_COLLECTION_BITS. (Until 2026-10-10 the floor was the
  launch share, which is 8x the cap, so every grant was the cap and each restart in
  a chain of restarts spent 1/8 of the pool: the ninth aborted. Until 2026-10-12 the
  floor was a flat 2^16, which equals the cap from 8192 processes up at the default
  expand factor, with the same result there. Aditya's reviews of #4021.) A later
  improvement, not done: at restart, move the allocator cursor down to just above the
  highest allocator-region number held by a restored element (one max-reduction), so
  repeated restarts cost only what is actually in use.
- Restart does not wait for grants, and does not request any: a restored CkLocMgr
  (restoredFromCheckpoint, set in its pup) starts with an empty tranche, and the
  first insertion on a process asks the allocator for its first grant and is deferred
  until the grant arrives. So after every restart the first insertion on each process
  is asynchronous: a local [inline] insert returns before the element exists and
  ckLocal() is null until the grant lands (a one-time note says so). This is a
  deliberate trade against goal G5, confined to restarts; the manual (PR 2) must say
  so. A process that never inserts after the restart costs the
  pool nothing. Per-process cursors are not checkpointed at all; only the allocator
  cursor is. An array created after a restart is not restored, so it takes its share
  of its own (untouched) initial region as at launch, split by the current process
  count rather than the launch count (after an expand the launch share would run
  into the allocator's half).
- `+objid_tranche_log2 N` caps every tranche at 2^N numbers so tests can see refills
  and deferral at small scale; it is pupped with the layout.

### 3.2 Index hash key

    ck::indexHashKey(const CkArrayIndex& idx) -> uint32 in [0, 2^H)
        h = splitmix64 mix over the words (nInts, dimension, index[0..nInts-1]);
        return h >> (64 - H);          // H from ck::objid::Layout

A real mixer, not CkArrayIndex::hash() (ckarrayindex.h:145, a sum of rotated words
that clusters on structured indices). Must be a pure function of the index bytes;
identical on every PE and across restarts (no PE count, no map state).

Optional map hook (later, not PR 1): `virtual uint32 CkArrayMap::homeKey(int
arrayHdl, const CkArrayIndex&)` defaulting to indexHashKey, for maps that want the
directory co-located with a placement pattern. The runtime requires only that it
is a pure function of the index and returns < 2^H.

### 3.3 Home

    homeProcess(id)  = ObjID::hashKey(payload) % CkNumNodes()
    homeProcess(idx) = indexHashKey(idx)        % CkNumNodes()      // identical by construction
    homePe(x)        = CkNodeFirst(homeProcess(x)) + (key / CkNumNodes()) % CkNodeSize(homeProcess(x))
                       // the quotient spreads a process's homes over its ranks
    PR 3: the process's shared table; requests go to the process (node queue)

CkArrayMap::homePe is NOT consulted for hashed-kind arrays (it defaulted to procNum;
only CldMap overrides it in the tree). procNum is still used for placement
(findInitialHostPe, populateInitial). This is the one user-visible semantic change
for hashed-kind arrays: the directory PE is a hashed PE of a hashed process rather
than procNum(idx). The cost profile is unchanged: a source without an id already round-
trips to the home before the first send (bufferForLocation -> requestLocation(idx)).
The rank chosen from the key's quotient spreads a process's homes over its ranks
without a shared table, so a single-process run does not concentrate every
hashed-kind home on PE 0 (rank 0 as the directory PE cost 1.62x on unbounded cold
sends, doc/objid64-perf.md). Nothing about the home is checkpointed, so the choice
of rank is invisible to restart. The spread is limited by keys per process (2^H / processes): with the default
expand factor there are 8 to 16 keys per process, so at most that many ranks of a
process serve as homes; +objid_expand raises it.

### 3.4 Reverse lookup

lookupIdx(id) keeps two branches: compressor->decompress, or a local record
(elementNrec(id)->getIndex()). The idx2id scan and the bare CkAssert (cklocation.h:640-
648) are deleted; if neither branch applies it aborts with "index of id %llx is not
known on PE %d" -- and no delivery path may call it (section 4). Remaining callers:
deliverInline's bound-sibling demand creation (ckarray.C:1959; the sibling's record
is local, so the record branch serves) and the LB UpdateLocation path (rewritten,
section 4.4).

No scan survives. CkLocMgr::recoverIndex(id, idx) answers from the compressor,
a local record, or the PE's table of retired ids (below), all constant time, and
returns false otherwise. Demand creation needs the index (the home approves by
index; createhere creates at the original sender), and a message can reach a PE
by id for an element that has no record there: a deleted element whose id and
stale location the sender still held. The home can always recover such an index,
because it records every death of an element it is home for; a non-home PE that
cannot hands the message to the home.

Re-creation under a new id. An element inserted without a PE argument lands on
the map's PE, which for the hashed kind is not its home (section 3.3), so a deleted
index re-created on a PE that never saw it gets a fresh unique number, and every
PE that cached the old id (the old element's host, any earlier sender) keeps
sending to an id that no element has. Each PE keeps a table `retiredIds` of ids
that no longer name a live element, by index: an id the PE rebound to a different
id (CkLocMgr::insertID), and, at the home, the id of every element of its that
died (CkLocMgr::noteDead, from reclaim for a local death and reclaimRemote for a
remote one); the idx -> id binding itself is kept, as before. A message for such
an id, wherever it lands (CkArray::handleUnknownByID), is re-addressed to the
index's current id when there is a newer one and forwarded; the home also sends
the original sender the new binding by index, so its later sends go direct. At
the home, if the index has no element at all, the message waits by index, and the
index listener releases it whichever id the next element of that index gets. A
location request by id for a retired id (CkLocCache::requestLocation, a large
message held where it landed) is answered the same way, by index; the asking PE's
updateLocation then re-addresses the messages it held under the old id to the new
one (CkArray::releaseRetiredMsgs) and sends them to the new location directly.
The table grows like idx2id and like it is never pruned (no PE knows which others
still hold an old id). Found 2026-10-18 by tests/charm++/amr_1d_simple at 4, 7
and 8 PEs, where the map's PE and the home of the re-created index differ;
tests/charm++/objid_insert phase 7 reproduces it deterministically, with and
without +messageBufferingThreshold 0.

## 4. Location manager and delivery

### 4.1 One home function

    int CkLocMgr::homePe(const CkArrayIndex& idx) const   // exists; semantics per kind
    int CkLocMgr::homePe(CmiUInt8 id) const               // exists but dead; becomes live:
        compressor ? homePe(compressor->decompress(id))
                   : hashedHomePe(ObjID::hashKey(id))        // 3.3: process from the
                                                            // remainder, rank from the quotient

CkLocCache gets a back-pointer `CkLocMgr* mgr` (set by CkLocMgr's constructor and by
its pup; the cache is created first, ckarray.C:653-660; bound arrays share both).
CkLocCache::homePe(id) (cklocation.h:378) becomes `mgr->homePe(id)`; the ObjID home
bits it read no longer exist. CkLocCache::requestLocation(id) (cklocation.C:2394) is
otherwise unchanged. CMK_RANK_0 wrappers are dropped (drone mode orthogonal).

### 4.2 Registration (unchanged protocol, now consistent)

createLocal -> informHome(idx, pe) -> homePe(idx); emigrate -> informHome;
reclaim -> reclaimRemote at homePe(idx); updateLocation(idx, entry) at the home
binds idx -> id (insertID) and id -> location (cache). Because homePe(id) ==
homePe(idx) for both kinds, no second registration exists and the creator holds
no special entry. reclaimRemote keeps the idx -> id binding (as today, so late
messages are not stranded) and erases the location.

### 4.2a Epochs

The epoch mechanism carries over unchanged in meaning: it is a per-element migration
counter, not a property of the home. It lives in the element's location entry at its
current PE, travels in the migrate message as getEpoch(id)+1 (cklocation.C:3074), is
installed at the destination by cache->insert (:2440), and every update that reports
a migration carries the epoch of that migration; a receiver applies an update only if
its epoch is newer (:2421). Migrations of one element are serialized by the element
itself, so its epochs are totally ordered, and two informs racing to the home from
consecutive migrations resolve to the later one whatever their arrival order. None
of this depends on which PE the home is, so PR 1 keeps the code as is.

Three places need care:
- PR 3, same-process migration (section 7.3 R3): today the source runs
  recordEmigration (pe = dest, epoch++, with an assert that pe == self) AFTER sending
  the migrate message. With a shared table the destination thread can process the
  message first, so the increment-in-place and its assert are wrong. recordEmigration
  becomes an epoch-tagged compare-write {dest, e+1}, idempotent with the
  destination's insert of the same value; no in-place increment anywhere.
- CMK_GLOBAL_LOCATION_UPDATE (section 4.4): today UpdateLocation on every PE
  fabricates the epoch as its OWN cached epoch + 1 (cklocation.C:106), which is not
  the element's epoch; a bystander with a stale cache stores a low epoch and a later
  older reply can overwrite the newer location. The redesigned update must carry
  the true epoch from the emigrating PE (it has it: the same value put in the
  migrate message), as the !CMK_LBDB_ON branch at :3133 already does.
- Restart: entries are rebuilt with epoch 0 (createLocal's default) on every PE,
  which is sound because every table is empty at that point; no pre-restart epoch
  survives anywhere.
- Aditya's intra-process live-pointer fast path must bump the epoch exactly as the
  packed path does when it is cherry-picked (one compare-write of {dest, e+1}).

### 4.3 Delivery by id (ckarray.C)

recvMsg (ckarray.C:1852) cache-miss branch: replace `lookupIdx(id); handleUnknown(idx)`
with `handleUnknownByID(msg, id, type, opts)` (Aditya's e417584dd function, folded in
verbatim except that homePe(id) is the section 4.1 function). Adopt
requestLocationOnce with its pending set keyed by array_element_id(). Enable the
commented-out rule at ckarray.C:1885: after more than one hop with a stale cache
entry, forward to homePe(id) rather than chase the chain. sendMsg / handleUnknown /
bufferForLocation on the source PE keep their index-keyed shape.

Multi-hop repair (multiHop -> cache->requestLocation(id, srcPe)): now fired by the
fast delivery path for ANY forwarded message (hops >= 1; recvMsg counts a hop per
forwarding PE, a direct delivery arrives with 0), so a sender learns the location from
the first forwarded delivery, cold or one-hop stale; no explicit requestLocationOnce
is needed for this (numbers in doc/objid64-perf.md).

### 4.4 Load balancer global update (CMK_GLOBAL_LOCATION_UPDATE)

UpdateLocation (cklocation.C:90) becomes id-keyed: cache->updateLocation(entry); if a
local record or the compressor yields the index, also fire the index listeners.
Never calls lookupIdx on a PE that has no record. The entry's epoch must be the
element's true epoch, sent by the emigrating PE (section 4.2a), not each
receiver's cached epoch + 1 as today (:106).

As implemented: the
synchronous update stays, through CkLocMgr::updateLocationFromLB(id, pe), so every
PE but the source addresses the element at its destination before the move is acted
on (the CentralLB invariant). The balancer's decision
(MigrateInfo) does not carry the element's epoch, so the entry is counted from the
receiver's cache as before; that count never exceeds the true epoch (both advance by
one per migration, and a bystander's starts no higher), so the destination's insert
still applies over it. The emigrating PE then broadcasts the entry with the true
epoch (CkLocMgr::emigrate), which closes the stale-reply window of the fabricated
count. Cost: one group broadcast per migration, in this build mode only. Carrying
the true epoch in MigrateInfo (through LDObjData) would remove the broadcast; left
for later.

### 4.5 Envelope

Unchanged in PR 1-3. The envelope's array union still carries id (payload plus
collection), the array gid, hop count and ifNotThere. (The gid duplicates the
collection bits and ifNotThere is an entry-method property; reclaiming those bytes
is noted for later, not needed at 64 bits.)

## 5. Creation, migration, deletion

- Dynamic insertion (ckInsertIdx -> findInitialHostPe -> insertElement on the host):
  unchanged. The host mints (section 3.1) or computes (section 2) the id, builds the
  record, cache entry and element table, runs the constructor, informs the home.
  `[inline]` local insertion stays synchronous.
- Demand creation: unchanged (approval through homePe(idx), which is the same PE
  the id-keyed path will consult).
- Migration: CkArrayElementMigrateMessage carries idx and id (cklocation.h:94);
  immigrate binds idx -> id before createLocal (cklocation.C:3198). Unchanged. The
  intra-process fast path on Aditya's branch (emigrateIntraProcess) is compatible:
  it carries the same fields.
- Deletion: reclaim -> reclaimRemote at homePe(idx). Unchanged.

## 6. Checkpoint, restart, shrink/expand

Ids never change: packed-kind ids are functions of the index; hashed-kind ids are
restored verbatim (restore/resume already call insertID(idx, id), cklocation.C:3320,
3332). Section cookies (ckmulticast ObjKeyList) therefore stay valid.

Layout persists: `ck::objid::Layout {H, U}` is written by PE 0 with the readonly/
group data and restored before any locmgr is unpacked; a restart never recomputes H.
If the new process count > 2^H the runtime warns that directory spread is limited.

Directories are rebuilt, not restored: resume/restore call createLocal with
notifyHome = true on every restart path, so each restored element registers at its
home under the NEW process count. The __FAULT__ branch of CkLocCache::pup
(cklocation.C:2347-2392) that ships remote entries is removed (it encoded the old
PE count).

Tranches: the allocator cursor `trancheTop` is part of the PE 0 CkLocMgr branch's
pup, so it is restored on every restart path (same count or shrink/expand). Every
process discards its pre-restart tranche and obtains a fresh one from the allocator
at its first insertion after the restart (section 3.1). The initial region is considered fully
consumed after the first launch; the allocator never hands out from it. This
replaces the per-PE `p | idCounter`, which under shrink/expand is restored from
PE 0's copy on every PE and can collide.

Startup/restart checks: U >= 16 (else abort naming +objid_expand and
CMK_OBJID_COLLECTION_BITS); allocator region not exhausted (abort with the same
message when a grant cannot be made).

## 7. Process-level directory and cache (PR 3)

This section analyzes which tables stay per PE, and how
the shared table is kept race-free.

The design as it will be implemented is `doc/internals/process-location-cache.md`
(2026-10-31). It keeps 7.1 (what is shared), 7.3's race analysis, 7.6's
guarantees S1-S5 and 7.7's eviction classification, and replaces the shard
locks of 7.2 with a lock-free lookup path: open-addressing tables whose slot
words are read with atomic loads and written with single-word compare-and-swap,
a waiter bitmask per entry in place of the waiter map, an arriving state for an
element in flight, and location messages addressed to processes. Where this
section and that chapter differ, the chapter holds.

### 7.1 Tables today (all per PE) and where they go

| table | keyed by | holds | today | PR 3 |
|---|---|---|---|---|
| CkArray element table (getEltFromArrMgr) | id | ArrayElement* | per PE, per array | **stays per PE** |
| CkpvAccess(array_objs) (ck.C:1331) | full id | ArrayElement* | per PE | **stays per PE** |
| CkLocMgr::hash | id | CkLocRec* (index, LB handle, timing) | per PE | **stays per PE** |
| CkLocCache::locMap | id | {pe, epoch} | per PE | **per process** |
| CkLocMgr::idx2id | idx | id | per PE | **per process**, plus a per-PE read cache |
| CkLocMgr::bufferedLocationRequests | idx | requesting PEs | per PE | **per process** |
| CkArray::bufferedIDMsgs / IndexMsgs / CreationMsgs | id / idx | messages | per PE | stay per PE, plus a per-process waiter set |
| tranche cursor (3.1) | gid | atomic | -- | per process |

**A separate PE-level id -> object pointer map is needed, and it is the hot path.**
Every delivered message does one lookup of the recipient id on the owning PE
(ck.C:1331, then CkArray::recvMsg -> lookup(id)). A thread-local table is read with
no synchronization at all. Putting the pointer into the shared table would make
every delivery take a shard lock or at least an atomic read of a cache line that
other cores write, on a table 64 PEs share; that is measurable at millions of
messages per second per node and buys nothing, since the pointer is only usable on
the owning PE anyway. So: delivery consults the per-PE pointer table FIRST and
touches the shared table only on a miss (element not on this PE), exactly the
order recvMsg has today. CkLocRec stays per PE with the element for the same
reason (it carries the LB handle and timing state that only the owning PE uses).

### 7.2 The shared table

Follow-up charm #4040: a per-array "full-length" hashed kind
(49 unique bits, no home key, id home = hash(id), index home holding only the
index -> id binding) is a split of this table into an index table and an id table,
and lands with or after PR 3, not before.

One object per process per CkLocMgr, reached through a CksvAccess map keyed by
locmgr gid. Created by whichever rank constructs its CkLocMgr branch first
(double-checked under a CmiNodeLock, since group creation runs on all ranks
concurrently); refcounted by ranks; freed when the last rank's branch is deleted.

    struct SharedLoc {
      struct Shard {
        CmiNodeLock lock;                                  // pthread mutex on reconverse
        std::unordered_map<CmiUInt8, Entry> loc;           // id -> {pe, epoch}
        std::unordered_map<CkArrayIndex, CmiUInt8> idx2id; // hashed kind only
        std::unordered_map<CmiUInt8, uint64_t> waiters;    // id -> bitmask of ranks
        std::unordered_map<CkArrayIndex, std::vector<int>> pendingIdxRequests;
      } shards[NSHARDS];                                   // NSHARDS = 256, by id hash
      Shard& of(CmiUInt8 id); Shard& of(const CkArrayIndex&);
    };

Every operation takes exactly one shard lock for its duration, and does its
read-compare-write inside it. No operation holds two shard locks (the idx2id and
loc entries for one element live in the shard chosen by the id, and index-keyed
operations first map idx -> id under the idx shard, then release, then take the id
shard; a stale idx -> id binding is impossible because bindings never change).
Non-SMP: one rank, locks uncontended; identical code.

### 7.3 Writers, readers, and each race

Writers of a location entry: W1 creation (owning PE), W2 emigration (source PE:
pe = dest, epoch+1), W3 immigration (dest PE: pe = self, epoch from the message),
W4 update from a remote (home inform, multi-hop repair, LB global update, request
reply), W5 erase on death (reclaim / reclaimRemote). Readers: whichPe(id) on every
send-side miss from any rank; the home answering a request from any rank.

R1 Two W4 updates racing with different epochs. Resolved by the epoch compare being
   a locked read-compare-write ("newer epoch wins"); today's per-PE code compares
   without a lock because nothing was concurrent. Same-epoch updates are idempotent.
R2 A reader sees an entry that is about to change (element migrating). Benign and
   identical to today's stale cache: the message reaches the old PE, misses the
   per-PE pointer table, consults the shared table (now updated), forwards; the
   multi-hop repair (multiHop) then corrects the sender. No new failure mode.
R3 Intra-process migration b -> c. W2 on b and W3 on c write the same value
   {c, e+1}; order does not matter ONLY because both are epoch-tagged compare-writes:
   today's recordEmigration increments in place and asserts pe == self after the
   send, which fails if c's thread processes the migrate message first (section
   4.2a). With Aditya's live-pointer fast path (emigrateIntraProcess on his branch)
   it is one write. Messages in b's queue for the element after b's W2 take the R2
   path to c: one extra local hop, no loss.
R4 Insert or erase concurrent with a read. The map structure is only ever touched
   under the shard lock, so a reader never sees a torn bucket. (A lock-free read of
   the entry value is possible later because {pe, epoch} packs into one 64-bit
   word; not in v1 -- measure first.)
R5 Death vs a late update. W5 erases; a late W4 with an older epoch would re-create
   a ghost entry, because updateLocation today writes `locMap[id]` unconditionally
   (cklocation.C:2420). This exists per PE already. PR 3 keeps a tombstone
   {pe = -1, epoch = death epoch} so an older update is rejected by the epoch
   compare; tombstones are dropped when the idx -> id binding is dropped (never, as
   today, or by a later reclaim policy).
R6 Waiter registration vs fill. Rank b buffers a message for id and must not miss
   a fill that lands "between" its check and its registration. Both happen inside
   one critical section on the id's shard: {look up entry; if absent, set bit b in
   waiters[id]}. The filler, in its own critical section, writes the entry and
   takes the waiter bits out. It then sends one local wake message per waiting rank
   (CmiSyncSend to that PE; the filler's own rank flushes inline). A wake that
   arrives after the rank already flushed is a harmless no-op.
R7 idx2id for hashed-kind arrays is on the send path of index-addressed sends. A
   per-PE thread-local read cache (idx -> id, filled on miss from the shared shard)
   keeps the hot path unsynchronized; bindings never change, so the cache never
   goes stale. Memory: only indices this PE has sent to.
R8 Tranche cursor: fetch_add; next-tranche slot: compare-and-swap (3.1).
R9 Home requests are delivered to the process (node queue) and answered by whichever
   rank dequeues them, reading the shard. If the home has no entry yet, the request
   is parked in pendingIdxRequests / waiters of the shard (today's per-PE code has a
   TODO and drops such requests, cklocation.C:2406); the rank that later performs
   W1/W3/W4 for that id answers them from inside the same critical section's
   collected list (sends happen after the lock is released).
R10 Group-creation ordering: a request can arrive at the home process before its
    CkLocMgr branches exist. Today's lookupGroupAndBufferIfNotThere handles the
    per-PE case; node-level requests use the same buffering keyed by gid at the
    node queue level (charm's nodegroup creation path already does this).

### 7.4 What must not be shared

The LB database (per PE, registers CkLocRec handles), the reduction managers, the
per-PE buffered message queues (messages are PE-owned memory and are sent from the
PE that buffered them), and the CkMigratable_initInfo per-PE construction state.

### 7.5 Cost expectations and what to measure

Send-side miss: one shard lock per miss instead of a message per PE-miss; cache
fills once per process. Delivery hot path: unchanged (per-PE table). Migration:
one shared write per migration per process touched instead of per PE. Measure on a
64-PE node: directory requests per second before/after, whichPe latency under
contention (all ranks sending to the same 1,000 elements), and a migration-heavy
run (megatest 4x8 x3, anytime_migration) for correctness under R1-R6.

### 7.6 Shortcuts the shared table must provide

The point of the shared table is that a PE never asks the home about anything it
could learn from its own process. Stated as guarantees, so PR 3 can be tested
against them:

S1 **Resident elements resolve locally, always.** Every element currently in the
   process has its idx -> id binding (hashed kind) and its id -> {pe, epoch} entry
   in the shared shards, written by the PE that created it (W1) or received it
   (W3) before that PE runs any entry method of it. So a send by index from any PE
   of the process to an element on another PE of the process costs: per-PE read
   cache miss, one shard read for idx -> id, one shard read for id -> pe, then
   CmiPushPE to that rank. No message leaves the process and the home is not
   consulted. Today the same send on a PE that has never seen the index goes to
   the home and back (bufferForLocation -> requestLocation(idx)), even when the
   element is one core away; objid_insert's cold-send phase measures exactly this.
   The packed kind needs no idx2id at all (compress is a pure function), so S1
   for it is the location entry alone.

S2 **An element that left the process leaves a forwarding entry.** Emigration (W2)
   writes {dest, epoch+1}; it is never erased by departure, only by death (W5,
   as a tombstone) or by a newer location. A PE of the old process that still
   addresses the element pays one hop to dest, which repairs the sender (multiHop)
   -- no trip to the home. Intra-process migration (R3) is the degenerate case:
   the forwarding entry is the final location.

S3 **One request per process, not per PE.** When no PE of the process knows the
   element, the first PE to miss sends the request and registers as a waiter; the
   others find the pending request in the shard (R6) and only register. The reply
   fills the shard once and wakes every waiting rank.

S4 **The home answers from the shard, on any rank.** A request arriving at the home
   process is answered by whichever PE dequeues it (R9); the index -> id binding
   and the location are both in the shard, so the home never needs the element's
   own PE to be the one that answers.

S5 **Same-PE delivery stays as it is.** sendMsg on the owning PE finds the element
   in the per-PE pointer table and delivers inline; the shared table is not
   touched. Chares remain PE-bound; S1 shortens the lookup, not the queue hop.

What S1 needs that the per-PE design does not have: the per-PE idx2id of the
hashed kind is today filled only by local creation, immigration and home replies;
in PR 3 the shared idx2id is filled by every creation and immigration on any rank
of the process. The per-PE read cache in front of it (R7) is fill-on-miss and
never invalidated, which is sound because a binding never changes.

### 7.7 Bounded caches

Every fill-only table above grows with the number of distinct elements a PE or a
process has ever addressed, and a long-running program that keeps migrating and
keeps sending to new elements never stops adding entries. Today's per-PE
CkLocCache::locMap already has this property; the redesign must not make it worse
and should fix it while the tables are being rebuilt.

Classify entries as authoritative or cached. Authoritative entries are never
evicted: the id -> location and idx -> id entries of elements RESIDENT in the process
(S1 depends on them), the home-directory entries for elements whose home is this
process (the home must always answer), tranche state, and the per-PE element pointer
and record tables. Everything else is a cache that can be refetched from the home at
the cost of one request: locations of remote elements learned from replies, repairs
and forwarding entries left by departed elements, tombstones, and the whole per-PE
idx -> id read cache (R7).

Policy, cheap on the hot path: a size cap per shard for cached entries and a cap for
the per-PE read cache; when a cap is hit, drop the cached entries of that shard (or
the whole read cache) in one sweep rather than keeping LRU state per entry. A
two-generation variant (entries carry the generation they were filled in; a sweep
drops the older generation) halves the refetch burst after a sweep at the cost of
one byte per entry. Caps default to a few hundred thousand entries per process and
are runtime options (+objid_cache_cap), with a counter of sweeps in the diagnostics.

What eviction may cost: a later send to an evicted element pays one request to the
home (hashed kind) or one forwarded delivery (packed kind), which the repair rule of
4.3 then fixes again. Dropping a forwarding entry or a tombstone is safe: a message
for the element goes to the home instead of along the chain; a late, older location
update that would have been rejected by a tombstone re-creates a cached entry that
is wrong only until the next forwarded delivery repairs it, and that entry is itself
evictable. Authoritative entries are never in that position.

Measure before choosing the per-PE read cache at all: if one shard read per
index-addressed send is cheap enough on a 64-PE node, the read cache can be dropped
and only the shared table needs the cap.

### 7.8 The re-creation rules after the TLA+ check: cost, contract, options (2026-11-03)

Kale's question after the check of PR #4123: the location protocol has grown
(chapter section 3.0 lists five rules for deletion and re-creation that the
first version did not have); is a simplification being missed, does the
machinery cost the program that never deletes, and should the full protocol be
an option an array declares? This section answers from the code as merged
(2f144b0e7), the chapter, and the model (`doc/internals/tla/process-location/`,
configurations `*_norec*`).

**Where the complexity is.** Entirely in deletion followed by re-insertion of
the same index while messages about the earlier life are still in flight. The
protocol for a program that never deletes is the first design: the table, the
epoch compare, the waiter bits, one request per process (chapter 3.0, "why this
is enough without deaths"). Of the five rules, two cost nothing and are
protective for every array: the mark on values written from messages with
"own writes stay" (rule 3), and the drop of replies and repairs by id in the
home's process (rule 4, which removes writes). Rule 5 (clear by match) is one
comparison. The cost is in rules 1 and 2: the life tag and the home's record.

**What they cost a program that never deletes.**

| item | cost | where |
|---|---|---|
| extra messages | none | the record is updated from messages the protocol already sends |
| message size | 8 bytes on each home update (`informHome`), reply by index, migration message and death notice | `CkLocEntry::life`, `CkArrayElementMigrateMessage::life` |
| home CPU | one hash-map lookup or insert and a scan of a one-element vector per home update | `CkLocMgr::homeNoteLocation` |
| home memory | about 100 bytes per element, at its home PE only (an `unordered_map` node holding a one-entry `std::vector<CkLocEntry>`); for comparison, a table slot is 24 bytes at a load of at most one half, in every process that addressed the element, and a `CkLocRec` plus the element itself is hundreds of bytes | `CkLocMgr::homeLives` |
| growth | the dead-tag set grows with deletions and is never pruned (chapter section 9); without deletions it is empty | `CkLocMgr::deadLives` |
| holder memory | 8 bytes per element | `CkLocRec::life` |

So there is no parallel messaging overhead in the base scenario. The argument
for separate code paths on messaging grounds does not hold; what remains is
memory at the home and a second per-element structure beside the table.

**Is the record the minimum for the no-contract setting?** Yes, with the
network as it is. Reconverse delivers in any order, a life's messages come from
whichever PEs it visited, and two lives are minted on different processes, so
no tag carried by a message orders two lives, and the home cannot tell, when a
creation update at epoch 0 meets an entry at a higher epoch, whether it is a
stale update of the same life or the first word of a new one. One scalar "life
of the entry" at the home is not enough: an old life's stale update can arrive
after the new life's update and before the old life's notice, and the home must
still remember the new life's location when the notice clears the entry. The
set of lives not known dead, with one location each, is that memory; in
practice it holds one entry, two during a race. The alternative of deciding at
the home by asking the PE the entry names ("do you hold x at epoch e?") would
replace tags and record by a probe message on each ambiguous update and each
notice; it keeps no per-element state but adds a round trip per deletion and a
pending list, and has not been modeled. Not pursued.

**The contract alternative.** The question is different if a program promises
that an index deleted is inserted again only after a synchronization that
follows the deletion (a reduction or quiescence after the `ckDestroy` calls;
re-creating the whole array also satisfies it), so that no runtime message
about the old life is in flight when the new life is created. The model has
this as `Sync`, and two reduced protocols: `Rec = FALSE` (no record, no tags:
the compare-and-clear on a notice plus the two free rules) and `DeadSet` (tags
kept; the home keeps only the set of dead lives, drops their updates, and clears
its entry unconditionally on a notice; no per-life location, no reinstall).

| configuration | protocol | contract | checks | result |
|---|---|---|---|---|
| `MC_death_safety_norec` | no record | no | `Inv`: holder's entry exact, held messages covered, requests outstanding | pass, 1,877,252 states |
| `MC_death_liveness_norec_noinv` | no record | no | delivery, home convergence, quiescence | violated (the race of finding 3) |
| `MC_death_3proc_norec` | no record | no | delivery, quiescence | violated |
| `MC_death_safety_norec_sync` | no record | yes | `Inv` | pass, 206,549 |
| `MC_death_stale_norec_sync` | no record | yes | stale and settled forms of the holder's entry | pass, 206,549 |
| `MC_death_liveness_norec_sync` | no record | yes | `Inv` + the three liveness properties | **violated**: finding 8 |
| `MC_death_3proc_norec_sync` | no record | yes | delivery, quiescence | **violated**: finding 8, a message bounces for ever |
| `MC_death_only` (code as written, no re-creation at all) | | | the network drains | **violated**: finding 8 |
| `MC_death_only_fix` (merged code) | record | | the network drains | pass, 91,558 |
| `MC_death_safety_deadset_sync` | dead set | yes | `Inv` | pass, 168,346 |
| `MC_death_stale_deadset_sync` | dead set | yes | stale and settled forms | pass, 168,346 |
| `MC_death_liveness_deadset_sync` | dead set | yes | `Inv` + the three liveness properties | pass, 168,346 |
| `MC_death_3proc_deadset_sync` | dead set | yes | delivery, quiescence | pass, 40,036 |
| `MC_death_3proc_deadset`, `MC_death_liveness_deadset_noinv` | dead set | no | liveness | violated (finding 3: the unconditional clear removes the new life's entry) |

Three facts come out of this:

- The safety properties hold without the record and without the contract: the
  two free rules alone keep the holder's process entry exact, so the move's CAS
  cannot abort and same-process sends go to the right PE. The record is needed
  for liveness only.
- The contract alone does not make the no-record protocol live, because of an
  ordering that has nothing to do with re-insertion (finding 8 in the model's
  README, `trace_dead_bounce.txt`): the element dies, its death notice reaches
  the home before one of the life's own earlier home updates (its creation or
  a move), the notice finds nothing to clear, and the late update revives the
  home's entry naming the dead PE. A message sent to the element afterwards
  goes to the home, which forwards it to the dead PE, whose process has no
  entry and sends it to the home: it bounces for ever. A later re-creation does
  not correct the home, since the new life's epoch 0 loses the compare. The
  per-PE code of recharm has the same ordering with the same outcome (the home
  PE's cache is erased by the notice and revived by the update); 339753ce0 is
  the first version without it. Closing it needs the home to remember that the
  life is dead when a late update of that life arrives, and to recognize the
  life: the tag and the dead set are the minimum.
- With the tag and the dead set, the contract is enough: the per-life location
  record and the reinstall step exist only for the no-contract race (a new
  life's creation update reaching the home before the old life's notice,
  finding 3), which the contract excludes. Without the contract the dead-set
  protocol loses that message.

**Options.**

- *A. Keep the full protocol for every array* (the code as merged). No contract
  to document; the cost in the table above. Most code, simplest rule for users:
  delete and re-insert whenever you like.
- *B. The per-life record as an array option.* `CkArrayOptions::setReinsertion(bool)`,
  beside `setAnytimeMigration` and `setStaticInsertion`: an array that may
  re-insert a deleted index without a synchronization in between declares it,
  and only its `CkLocMgr` keeps `homeLives` and runs `homeReinstall`; every
  array keeps the tags and the dead set (finding 8 needs them regardless), and
  clears by match or unconditionally depending on the flag. The default is the
  dead-set protocol with the contract in the manual. Saves the per-element home
  memory for every other array; adds a flag and a branch, so more code than A,
  not less.
- *C. Remove the per-life record, adopt the contract.* Deletes `homeLives`,
  `homeReinstall`, the older-than-record rule and the clear by match (about 80
  lines and one concept, the record), keeps the tags, the dead set and the two
  free rules. Model-checked as `DeadSet` with `Sync`. Returns to a lost message
  (a silent hang) when a program breaks the contract, as recharm does today in
  that race, with the abort of finding 5 and the own-entry corruption of
  finding 6 closed. The contract goes in the manual next to `ckDestroy`.

In every option the dead set grows with the deletions and is never pruned
(chapter section 9); pruning needs a bound on how late a message of a dead life
can arrive, which the contract would supply (prune at the synchronization) but
the runtime cannot see.

**A weaker contract that does not work.** Kale asked whether "the program
processes every message to the element before deleting it" would do instead of
quiescence. It does not: it orders application messages against the deletion,
while the race is between two runtime messages, the old life's death notice and
the new life's creation update, which travel from different PEs to the home.
Modeled as `AppSync` (`MC_death_3proc_deadset_appsync`,
`MC_death_liveness_deadset_noinv_appsync`): the dead-set protocol still loses a
message in 12 steps (`trace_appsync_lost_message_3proc.txt`). The condition is
still what makes a message to a dead element an error of the program rather
than something the runtime must define; no discard rule follows from it, since
a message held at the home for an id without location may be for the next life
and is released by that life's update.

**Decision (Kale, 2026-11-03): A**, the protocol as merged and checked, for
every array. The per-array version was judged to make the runtime harder to
read for the maintainer (two protocols, two sets of tests and configurations)
while only the application programmer gains a simpler model. **C is kept ready
as the fallback** if shrink/expand, heterogeneous nodes (processes with
different PE counts) or another feature make the general protocol too complex
to carry; its contract is the quiescence paragraph above (the weaker condition
is not enough), and its model configurations (`DeadSet` with `Sync`) stay in
the model folder so it can be rechecked against whatever the spec has become.
Switching to C is: delete `homeLives`, `homeReinstall`, the older-than-record
rule and the clear by match in `homeNoteDeath` (clear unconditionally); keep
the tags, `deadLives` and the two free rules; add the manual paragraph next to
`ckDestroy`; rewrite `objid_recreate`'s finding-3 scenario to expect the held
message; update chapter sections 3.0 and 3.5. Neither shrink/expand nor
heterogeneous processes change the rules of A as far as this analysis can see:
the tables are per process and are rebuilt on restart from the re-creations
(chapter section 5), a restart is a synchronization, so no message of an
earlier life survives it and the dead set starts empty; and nothing in the
protocol assumes equal PE counts per process (`CkNodeOf`, the per-process rank
in the waiter word).

## 8. Diagnostics and limits (PR 0, can land first)

- Abort in getNewObjectID when the per-PE counter would exceed the element mask
  (today: silent overflow into the home field at 65,535 per PE). PR 0 only; PR 1
  replaces the counter with tranches.
- The no-compressor note (section 2) at array creation.
- Existing collection-overflow abort (objid.h:56) keeps working; message updated.
- PR 1 adds: U >= 16 check; allocator exhaustion abort; the one-time deferred-
  insertion warning; the directory-spread warning after a large expand.

## 9. PR sequence

PR 0  Diagnostics (section 8, minus the process check) + coverage test: a migrating
      hashed-kind array (6D custom index with -DCK_ARRAYINDEX_MAXLEN=6, or
      tests/charm++/anytime_migration made unbounded), run with a balancer and with
      anytime migration; also under checkpoint/restart with a different PE count.
      Independent of the design; useful today.
PR 1  Layout (section 1), packed budget (2), hashed minting with per-process
      tranches and the PE-0 allocator (3.1-3.2), home function and cache
      back-pointer (4.1), delivery by id with e417584dd folded in (4.3), LB update
      by id (4.4), lookupIdx scan removed (3.4), persisted layout, allocator cursor
      and directory rebuild on restart (6). Home PE = rank 0
      of the hashed process for hashed-kind arrays; per-PE caches unchanged.
      Landed as PR 1a (#4017, layout/home/delivery, per-process initial tranche
      only) and PR 1b (allocator, refills, deferred insertion, restart with any
      process count).
      Tests: PR 0's test, bcastred, megatest, pingpong, ckpt tests (tests/charm++/
      chkpt, shrink/expand example), sections test after restart.
PR 2  Manual: id layout, build flags, what "compressible" means, the sizing note,
      shrink/expand guarantees.
PR 3  Process-level directory and cache (section 7). Measure: directory request
      counts and location-miss latency on a 64-PE node before/after.
PR 4  Cleanup: delete CMK_OBJID_HOME_BITS remnants, CMK_RANK_0, __FAULT__ cache
      pup, tryLookupIdx variants; optional CkArrayMap::homeKey hook.
PR 5  (optional, later) Id-carrying CkCallback variant. CkCallback's array form
      stores the index (ckcallback.h:121), which predates the object id; invoking
      it costs an index -> id step at the sender, and for a hashed-kind element the
      sending PE has never dealt with, a round trip to the home. A variant built
      where the id is known (the element itself, or a PE holding its record) sends
      by id directly; it is the first user of a kind field in the tag bits if
      callbacks to chares and groups are ever folded into the same handle.

### 9.1 Merge gate into the reviewed line

The redesign is not merged into reviewed-with-reconverse all at once. The cut is
PR 1 as a whole: 1a (#4017, merged into objid-redesign 2026-10-09), its fixes
(#4028, from Aditya's review of 1a), and 1b (#4021), together. 1a alone leaves
hashed-kind arrays with the initial tranche only, an abort when it runs out, and an
abort on a restart with a different process count; 1b removes all three, so 1a is
never merged to the reviewed line without 1b. The gate:

1. #4028 approved by Aditya and merged into objid-redesign.
2. #4021 rebased onto that, its tests (including the restart ones) passing, merged
   into objid-redesign.
3. ChaNGa, which creates a ~262k-element bounded array and today sees the PR 0
   "needs 17 bits ... hold 16" note, rebuilt and run against objid-redesign by
   Kale or Ritvik (Tom Quinn's schedule is not to be depended on): the note gone,
   the run unchanged.
4. A full sweep, because the id layout is a deep change: every test under
   tests/charm++ and tests/converse, every example under examples/charm++ with a
   test target, the benchmarks we run (pingpong, jacobi), the ten PPL mini-apps
   listed at https://charmplusplus.org/miniApps/ (LeanMD, AMR, Barnes-Hut, DenseLU,
   HPCCG, Kripke, TS, FFT, RA, EP Stream; repositories under github.com/UIUC-PPL),
   and the applications paratreet2 and ChaNGa, each in at least single-process multi-PE and
   multi-process configurations, with a balancer where the program supports one,
   and checkpoint/restart where it does. On the Mac and on one Linux cluster
   (Anvil). CI's subset is not enough for this merge.

Gate met 2026-10-17: items 1 and 2 done (#4028 merged 2026-10-09; #4021, carrying
1b and 1c, merged 2026-10-17); items 3 and 4 recorded in objid64-gate-2026-10-16.md
(ChaNGa at 262,144 pieces on Anvil, 1 to 2 nodes, energies and snapshots identical
to the baseline; the sweep's one real defect became #4044, merged). #4015 left draft
the same day.

Then #4015 (objid-redesign -> reviewed-with-reconverse) leaves draft and merges.
PR 2 (manual) follows as a small PR straight to the reviewed line. PR 3 and PR 4
are their own PRs against the reviewed line after PR 1 is in, not passengers on
objid-redesign. A checkpoint written before PR 1 cannot be restored after it (id
layout and checkpoint format change; pupLayout's marker makes that a clear abort):
the release notes must say so.

## 10. Test matrix

| test | kind | exercises |
|---|---|---|
| pingpong, megatest, hello/1darray (fixed) | packed | wider budget, unchanged paths |
| new: 6D custom-index migration (PR 0) | hashed | minting, homePe(id), delivery by id, LB |
| anytime_bcastred | packed | multi-hop repair |
| chkpt + restart same P | both | id stability, counter restore |
| shrink/expand example, P -> P/2 and P -> 2P | both | directory rebuild, counter file, key % newN |
| sections (ckmulticast) across restart | both | stable cookies |
| tiny layout (+objid_expand huge, small U; C=4 build) | hashed | every abort, deferred-insert fallback, allocator refill |
| startupTest, hello/4darray, leanmd | hashed | the sizing note, no behaviour change |

## 11. Open questions for Aditya

Q1 Which shrink/expand path is in use on rate-aware-gpu-lb: the ckcheckpoint.C
   CkResumeRestartMain route (+shrinkexpand, Cmi_myoldpe) or something newer? Does
   the LB database survive a restart (ids held across it)?
Q2 The AMR index: custom CkArrayIndexT with how many ints (CK_ARRAYINDEX_MAXLEN)?
   (The initial insertion burst is not large and fits the
   initial tranche; it is the refinements afterwards that add chares, and those run
   between steps, where a tranche refill has time to land. So the deferred-insert
   fallback of 3.1 should stay unreached in AMR; the index width is still open.)
Q3 Does any device or LB code read ObjID home bits or assume home == creator PE?
   (Audit on the reviewed line says no; his branch adds handleUnknownByID and
   emigrateIntraProcess, both id-keyed.)
Q4 (withdrawn.) The question asked about ids stored across LB
   steps; ids never change within a run, today or in this design, so that part was
   moot. The only case where an id changes today is a restart with a different PE
   count for a compressible array (analysis D4), and this design removes it. Nothing
   in application state needs auditing.
Q5 Is rank 0 of the home process as the interim directory PE (PR 1) acceptable for
   his runs, or does he need PR 3 before the AMR work can use this?

## 12. Wide-collection kind: collection capacity for AMPI and other many-array users

Status: decided 2026-10-16 (Kale); implemented 2026-10-17 on branch objid-pr1d-wide
(PR 1d, into objid-redesign after PR 1 #4015; target: before beta1). The AMPI-side
items (12.5) are scheduled by Kale with the AMPI regression work, not here.
Implementation notes are marked "(1d)" below where the code settled a detail.

### 12.1 Facts the design rests on

1. The COLLECTION field holds the CkGroupID index of the array's CkLocMgr
   (cklocation.C:99 names it locMgrGid). It is the ordinary group id: one counter
   on PE 0 (`_numGroups`, ck.C:993) shared by every group and every array, and
   arrays are always created on PE 0 (CkCreateArray asserts CkMyPe() == 0,
   ckarray.C:649; ckNew from another PE is forwarded). Each array consumes about
   three ids (CkArray, CkLocMgr, CkLocCache; map and mcast managers when made), so
   C = 12 is ~1,300 arrays, not 4,096.
2. Only the id's owner decodes the collection field from the id. Delivery never
   does: the envelope carries the CkArray gid separately (`getArrayMgr()`, used in
   ck.C:1336 and cklocation.C:1620), and the location tables key on the payload.
   The decoders are UpdateLocation under CMK_GLOBAL_LOCATION_UPDATE
   (cklocation.C:99), the LB migration records, and checkpoint restore. A change
   to how the collection field is read therefore costs nothing on the send or
   receive path.
3. AMPI creates one chare array per communicator, and per-rank communicators are
   inherent in MPI, not an artifact of one call site: MPI_COMM_SELF (createCommSelf,
   ampi.C:1164, one array per rank via MPI_Comm_split with color = own rank),
   MPI_Comm_dup of SELF (PETSc does one per rank in PetscInitialize), the
   communicators behind neighborhood collectives and Cartesian/graph topologies,
   and user splits with many colors. V virtual ranks therefore cost O(V)
   collections, times the number of such communicators per rank. Special-casing
   any one of them on the AMPI side does not change the order,
   so the runtime must hold millions of collections, and the current answer, the
   --ampi-only build with C = 29, is a per-build layout that nothing else uses.
4. AMPI's arrays carry no bounds: no setBounds call exists in ampi.C or tcharm.C,
   and createNewChildAmpiSync (ampi.C:2258) uses setNumInitial(0). Under this
   design they are all hashed-kind. Whatever the collection width, that is wrong
   for them: with 37 payload bits (12.2) the hashed kind leaves U = 37 - H unique
   bits, 20 at 8,192 processes, and the startup check (section 3.1) would fail
   first at Frontier scale. Their index is a rank in the parent communicator, so
   bounds = communicator size always holds; see 12.5.
5. The three TYPE_TAG bits are free (section 1). #4040's full-length kind needs no
   tag (its kind is known from the collection, like the hashed kind). A tag is only
   needed where the decoder must know something BEFORE it can look the collection
   up, which is exactly the case for a per-array collection width.
6. Charm++ sections (cksection.h, CkMulticast) are the runtime's own subset
   mechanism and consume no collection ids: a section is identified by the array's
   gid plus a cookie (creator PE, pointer to that PE's mCastEntry, reduction
   number) and carries an explicit element-index list; it exists for multicast and
   reduction only and is delivered by delegation, not by element id. So a section
   is what an MPI communicator's collective side maps to, but not its point-to-point
   side (a rank in a communicator is a different array element today). AMPI could
   in principle represent communicators as sections over one world array, which
   would remove the O(V) arrays; that is an AMPI redesign and is not pursued. The
   wide kind is cheaper and is useful to any library that makes many small arrays.

### 12.2 The layout: a tag-marked wide-collection kind

Per array, chosen at creation by `CkArrayOptions::setWideCollection()`:

    tag 000 (standard)   bit 60..49 COLLECTION C = 12     bit 48..0  PAYLOAD 49
    tag 001 (wide)       bit 60..37 COLLECTION W = 24     bit 36..0  PAYLOAD 37

The standard layout is bit-identical to today's, so existing ids and 9.0-beta
checkpoints are untouched, and an application that never opts in sees no change.
The wide layout is for arrays that are many and small. Its 37-bit payload covers
the packed kind up to 2^37 elements, which includes any world-size array; for the
hashed kind it leaves too few unique bits at scale (12.1 item 4), so the wide kind
REQUIRES bounds and aborts at creation without them, with a message naming
setBounds. W = 24 is 16.7 million collections: a million ranks with 16
per-rank communicators each. The practical ceiling is lower and is not the id:
every array still instantiates a CkLocMgr and CkArray branch on every PE, so a
million arrays cost on the order of a gigabyte per PE. That cost exists today in
AMPI and is independent of this design.

Decode: `getCollectionID()` reads the tag, then shifts by 49 or 37; one predictable
branch at the decode sites of 12.1 item 2. The hashed kind's per-collection
state (section 3) and the packed compressor read the payload width from the
CkLocMgr, which knows its own kind, so they do not need the tag.

Tag values used: 1 of 8; #4040 needs none. Interaction with #4040: orthogonal. Wide
is a collection-field property; the payload kind (packed, hashed, full-length)
stays a collection property. Wide + full-length would mean 37 unique bits; the
option rejects the combination.

### 12.3 Collection numbers instead of group ids

With the wide kind the field no longer holds a CkGroupID index. It holds a dense
"collection number" assigned on PE 0 in CkCreateArray (ckarray.C:647), from two
counters: standard numbers in [1, 2^12) and wide numbers in [2^12, 2^24), so a
program that makes 100,000 wide arrays never pushes a standard array past 4,095.
The number travels in the CkLocMgr constructor message and is registered in every
PE's vector of CkLocMgr pointers when the branch is created; the decoders of 12.1
item 2 index that vector instead of the group hash table, which is faster than
today. Group ids are still consumed by every array as before, but cap nothing.

(1d) The number is per CkArray, not per CkLocMgr: arrays bound together (bindTo)
share a location manager but need distinct recipient ids, because delivery keys the
per-PE object map on the whole id word. Each CkArray takes one number from the
counter of its layout (a bound array takes the layout of the manager it binds to,
whatever its own option says); the CkLocMgr records the number of the array that
created it and uses it for load-balancer object ids (CkLocMgr::makeID, also in
CkArray::recordSend, so a bound array's traffic is charged to the LB object its
elements registered under). UpdateLocation (CMK_GLOBAL_LOCATION_UPDATE) finds the
manager from the migration record's object-manager id, which is the CkLocMgr's
group id; there is no registry of managers by number. In a build with C >= 24
(the --ampi-only C = 29 build) the wide range starts at 1 and the two kinds are told
apart by the tag alone. On a restart every CkArray and CkLocMgr pup re-registers its
number and PE 0 moves the counters past every restored number
(ck::objid::noteRestoredCollection), so arrays created after the restart get fresh
numbers; the counters themselves are not checkpointed.

Standard arrays gain from this alone: the field counts arrays, not groups, so
C = 12 holds 4,096 arrays (3x today) with no layout change. This part belongs in
PR 3's rewrite of the per-PE tables (section 7.1) if 1d has not landed first.

### 12.4 Opt-in, with a creation-time suggestion

Wide is opt-in: automatic selection would change the ids of
every existing bounded array and the checkpoint format for every application, for
a problem only many-array libraries have. The runtime points the user at it:

- When the standard counter passes half its range (2,048 arrays), PE 0 prints
  once: "N chare arrays created; a program that creates many small bounded arrays
  can use CkArrayOptions::setWideCollection() (16.7 million collections) instead
  of rebuilding with -DCMK_OBJID_COLLECTION_BITS". Printed once per run, not per
  array, so it costs nothing in the common case.
- At 4,096 the existing abort in objid.h names the same option, replacing its
  current advice to rebuild with a larger C.
- Creating a wide array without bounds aborts with the setBounds message (12.2).

### 12.5 AMPI side

1. Bounds on every AMPI array (mandatory): `opts.setBounds(CkArrayIndex1D(size))`
   in createNewChildAmpiSync and in the world/parent creation in ampiInit, size =
   the communicator's rank count. A dozen lines. Makes AMPI arrays packed-kind and
   retires the hashed-kind startup check and tranche machinery for them.
2. `opts.setWideCollection()` at the same call sites.
3. Drop the `-DCMK_OBJID_COLLECTION_BITS=29` line from buildcmake's --ampi-only
   block once 1 + 2 are in; the special build is then unnecessary.

What PR #4118 found (2026-11-03): options passed at the AMPI creation sites
(createNewChildAmpiSync, ampiInit) are ignored. Every AMPI array is created with
bindTo() on the TCharm array, and CkCreateArray takes the id layout from the
location manager the array is bound to, not from the bound array's own
CkArrayOptions; bounds are likewise read only when a location manager is
constructed. The layout is therefore chosen once, by `setWideCollection()` on the
TCharm array in TCHARM_Build_threads (tcharm.C), whose bounds are already its
element count, and every AMPI array bound to it (ampiParent, MPI_COMM_WORLD's
array, each later communicator's array) inherits that layout. The bound arrays
are indexed by world rank, not communicator rank, so the world-size bound covers
every communicator and items 1 and 2 need no per-communicator size.

Scheduling is Kale's, alongside the AMPI regression work; items 1 and 2 are
independent of each other in the sense that 1 is correct and useful on its own,
while 2 needs PR 1d. This session does not edit src/libs/ck-libs/ampi or buildcmake.

### 12.6 Checkpoint and restart

CkLocMgr pups its kind flag, collection number and payload width next to `bounds`
(cklocation.C:2616), and CkArray pups its number and flag; stored ids carry the
tag, so restore decodes them with the right width as long as the restoring build
has the same W. pupLayout (cklocation.C:2363) records C and W after the marker and
aborts on a mismatch naming -DCMK_OBJID_COLLECTION_BITS (1d: before this, a restore
by a build with another C was not detected at all). Restore re-registers each CkLocMgr under its stored number, so the
vectors of 12.3 are rebuilt exactly; shrink/expand (section 6) is unaffected
because the number is per collection, not per PE.

### 12.7 Scope of PR 1d (as implemented 2026-10-17)

objid.h (two layouts, tag-aware accessors, the constructor takes a number, a
payload and the kind; the two hot-path aborts became CmiAsserts, since numbers are
in range by construction and payloads by the compressor or the minting); the 6
construction sites (ckarray.C x4, ckarray.h ckGetID, cklocation.C LB id) go through
CkArray::makeID / CkLocMgr::makeID; FixedArrayIndexCompressor::make takes the
payload width; the counters and noteRestoredCollection in
cklocation.C's ck::objid namespace; CkArrayOptions (setWideCollection,
setCollection, pup); CkLocMgr and CkArray constructors and pups; the creation-time
suggestion at 2,048 and the aborts at 4,096 standard / 2^24 wide / wide without
bounds / wide bounds over 37 bits; pupLayout's width check; the objid_wide test
(thousands of wide arrays plus standard ones: id decode and density, point-to-point
by index across arrays, migration with and without a balancer, checkpoint and
restart at the same and a smaller PE count with ids compared and counters checked,
the four abort and suggestion cases by grep). Sections 1, 2 and 12 updated. One day
of code; the gate is the objid test set, chkpt, megatest and pingpong on the Mac
(prod and Debug builds) and Anvil.

## 13. Decision log

- 2026-09-29 (Kale): hashed payload is home key plus a unique number in tranches, replacing a creator | counter split of the unique number; section 1.
- 2026-09-29 (Kale): the three TYPE_TAG bits stay reserved and unused, not reclaimed for capacity; section 1.
- 2026-09-29 (Kale): epochs carry over unchanged in meaning; section 4.2a added.
- 2026-09-29 (Kale): AMR's initial insertion burst fits the initial tranche (Q2 answered); Q4 withdrawn as moot; section 11.
- 2026-09-29 (Kale): per-PE versus per-process table analysis; section 7 introduction added.
- 2026-09-30 (Kale): shortcuts S1 onward that the shared table must provide; section 7.6 added.
- 2026-10-01 (Kale): bounded caches for fill-only tables; section 7.7 added.
- 2026-10-01 (PR 1b): multi-hop repair fires on any forwarded message, replacing the explicit requestLocationOnce of the first 1a draft; section 4.3.
- 2026-10-09 (PR 1a fixes, Aditya's review of #4017): CkLocMgr::recoverIndex added as the one scan off the delivery path; section 3.4.
- 2026-10-09 (PR 1a fixes, Aditya's review of #4017): synchronous LB location update kept through updateLocationFromLB, with a true-epoch broadcast from the emigrating PE; section 4.4.
- 2026-10-09 (Kale): merge gate into the reviewed line is PR 1 as a whole; section 9.1 added.
- 2026-10-12: directory PE chosen from the key's quotient within the home process, replacing rank 0 of the process (1.62x on unbounded cold sends); section 3.3.
- 2026-10-13: per-array full-length hashed kind filed as charm #4040, to land with or after PR 3; section 7.2.
- 2026-10-15: AMPI's arrays found unbounded (hashed-kind under this design), correcting the assumption that the C = 29 build suffices; section 1.
- 2026-10-16 (Kale): AMPI arrays get bounds and a tag-marked wide-collection kind replaces the C = 29 build, as objid PR 1d; sections 1 and 12.
- 2026-10-16 (Kale): wide collection is opt-in per array, not selected automatically; section 12.4.
- 2026-10-17 (PR 1d): tag value 001 marks the wide layout; the other six tag values stay free; sections 1 and 12.
- 2026-10-18 (fix after #4015): a deleted index re-created on a PE that never saw it gets a new id; the home keeps a retired-id table and re-addresses messages sent to the old id; section 3.4.
- 2026-10-19 (#4057 review, Aditya): the home records every dead element's id in the retired-id table, so the index scan is gone; a PE that releases messages held under an old id re-addresses them itself; objid_insert phase 7; section 3.4.
- 2026-10-19 (#4058 review, Ritvik, fixed forward after the merge): the LB comm record of a bound array uses the manager's id; the ObjID constructor's range check is always on; checkpoint layout marker OBJ2; the per-PE registry of managers is gone (UpdateLocation uses the record's object-manager id); objid_wide's grep cases run through testrun; advice text follows the layout widths; section 12.

- 2026-10-31: PR 3 lookup path is lock-free, not shard-locked (Kale's requirement: never a
  coarse lock on the lookup path). Reads are atomic loads on open-addressing tables, writes
  single-word CAS with the epoch compare, waiters a bitmask per entry, growth by chained
  segments without moving keys. Eviction (7.7) deferred to a second version. The two residue
  items of Aditya's e417584dd (compressor overflow message, miss counters) ride with PR 3.
  Design chapter: doc/internals/process-location-cache.md.
- 2026-11-02 (PR 3 implemented): location table, index table and retired ids per process
  (src/ck-core/cklocnode.{h,C}); one location request per process via waiter bits; the
  emigration is recorded before the migration message is sent. Left out of the first
  version, with reasons in the chapter's section 9: the arriving state, epoch-carrying
  tombstones, process-addressed messages (the CkLocNode nodegroup), eviction. Kale's
  proposal of 2026-10-31, no home update for a move within a process, is section 3.7 of
  the chapter and a later step.
- 2026-11-03 (TLA+ check of PR 3, #4123): five rules for deletion and re-creation under the
  same id (life tags, the home's record of lives, own writes stay, by-id drop in the home's
  process, clear by match), each from a model counterexample; model and traces in
  doc/internals/tla/process-location. Section 7.8 added: the rules cost no messages in the
  base scenario, about 100 bytes per element at the home; the record is needed only for
  liveness in the race and only without a synchronization between deletion and
  re-insertion; finding 8 (a death notice outrun by the life's own earlier update revives the
  home's entry and a message then bounces between the home and the dead PE for ever, in
  recharm's per-PE code too, no re-insertion needed) makes the tag and the dead set necessary
  in every variant; options A (keep), B (array option), C (remove the per-life record, adopt
  the contract). Decided A (Kale, 2026-11-03), C kept as the documented fallback with the
  quiescence contract; the weaker "all messages to the element processed before deletion"
  condition was modeled (`AppSync`) and shown insufficient.
- 2026-11-04 (#4123 review, Ritvik): whether a location request is out is a flag of its own
  in the table slot, set by the rank whose re-read found nothing and cleared by the fill,
  instead of "the waiter word was 0 before": a fill between two misses and their
  registrations followed by a death left a process holding messages with no request out.
  The model's atomic send step hid the ordering. Chapter section 3.3; model README finding 9.
- 2026-11-04 (#4123 review, Ritvik): every received update clears the request flag, whether
  or not it writes. A claim that came after a fill the rank's re-read had missed sent a
  request whose reply wrote nothing, which left the flag set with no request out. Chapter
  section 3.3; `objid_loctable` phase 10.
- 2026-11-04 (Kale): PR 3 (#4123) retargeted from objid-redesign to recharm, with recharm
  merged into it first. objid-redesign carried nothing beyond recharm except PR 3, so the
  separate "part 3" PR of section 9.1 would have been a second review of the same change.
- 2026-11-04: #4123 squash-merged into recharm as 4cf20af1c (Ritvik's approval). The three
  parts of the redesign are on the reviewed line; objid-redesign carries nothing beyond it.
- 2026-11-04 (#4031, defect 1): `CkArray::insertElement` forwards an insertion to the PE the
  index's entry names only when the location manager serves bound arrays
  (`CkLocMgr::servesBoundArrays`). For a single array such an entry is a dead element's
  (the other processes keep it after a death, section 3.5 of the process-location chapter),
  and a `createhere` re-creation followed it to the PE the element died on. `objid_insert`
  phase 6 now re-creates half of the destroyed elements by `createhere` from a PE outside
  the home's process. Review (Ritvik): bound arrays had it too once every sibling died, so
  a demand creation the home approved does not follow the entry at all
  (`insertElement(..., homeApproved)`): the home grants it only with no location for the id,
  which bound arrays share. Test `objid_boundrecreate` (two processes). Left open: an
  explicit `insert(pe)` into a bound array whose siblings all died still follows the stale
  entry, since insertions do not go through the home. Defect 2 of #4031 (a re-created life's
  updates rejected by processes holding the old life's higher epoch; a detour, not a
  misdelivery) stays open.
- 2026-11-05 (#4164, zachetienne): `IndexTable::bind` hung on two distinct keys with one
  64-bit hash: the claim word is the hash, and the insertion loop treated a claim equal to its
  hash as its own key still being published, without comparing the key words that `probe`
  compares. Fixed by comparing them after publication. The chance pair needs about n^2/2^65
  bindings in one process's table; the reporter inverted the hash to make one. Chapter
  section 2.2; `objid_loctable` phases 11 and 12.
- 2026-11-05 (#4149, zachetienne): CkIO's per-session arrays take the wide layout
  (`setWideCollection()`, unconditionally, as TCharm does; 16.7 million numbers are enough
  in every build, Kale). 12 collection
  bits counting arrays only is a lifetime limit of 4,095 arrays, down from about 500,000
  on classic (21 bits on the group id, four groups per array), and a library that creates
  an array per operation
  reaches it. Reusing numbers on destroy stays out: a late message, a stale location entry
  or a section cookie of the destroyed array would reach the new one. Test `io_sessions`,
  in the CI tier.
