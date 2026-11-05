# Object ids and element location

Every chare array element has a 64-bit id, `ck::ObjID` (`src/ck-core/objid.h`).
The runtime keys message delivery, the location tables and checkpoints on it.
An id names the element's array (the collection number) and the element within
the array (the payload); it does not name a PE. The home PE of an element, which
holds the authoritative record of where the element is, is computed from the id
or from the index, so ids survive migration, checkpoint and restart with a
different number of processes.

This chapter describes the standard layout and the location machinery of the
`recharm` line after the 64-bit redesign (charm #3994). The wide layout, which
adds a second id layout for programs with very many arrays, is PR 1d (#4050) and
is described in its own section below. Reasons, alternatives and measurements
are in `doc/objid64-design.md`, `doc/objid64-analysis.md` and
`doc/objid64-perf.md`.

Terminology: a *process* is an OS process, what Converse calls a node and counts
with `CkNumNodes()`; a *PE* is a worker thread, counted by `CkNumPes()`.

## 1. The id

    bit   63 62 61 | 60                      49 | 48                          0
         +---------+----------------------------+------------------------------+
         |  0 0 0  |   COLLECTION   (C = 12)    |      PAYLOAD   (64-3-C = 49) |  standard layout
         +---------+----------------------------+------------------------------+

    bit   63 62 61 | 60                      37 | 36            0
         +---------+----------------------------+----------------+
         |  0 0 1  |   COLLECTION   (24)        |  PAYLOAD (37)  |  wide layout (PR 1d)
         +---------+----------------------------+----------------+

    PAYLOAD of an array with bounds (packed kind, either layout):
         +----------------------------------------------------------------------+
         |              the element's index, packed into the field              |
         +----------------------------------------------------------------------+

    PAYLOAD of an array without bounds (hashed kind, standard layout only):
         +-----------------------+----------------------------------------------+
         |   HOME KEY (H bits)   |        UNIQUE NUMBER (49 - H bits)           |
         +-----------------------+----------------------------------------------+

**Tag.** Three bits name the layout: `000` standard, `001` wide. The other six
values are reserved; they were set aside in the original header for a handle
that could name other kinds of objects, and nothing writes or reads them.

**Collection.** The number of the chare array. PE 0 assigns it in
`CkCreateArray` from a counter, densely and never reused: standard numbers run
from 1 to 2^C - 1, and C is `CMK_OBJID_COLLECTION_BITS` (default 12, a build
flag). Before the redesign this field held the array's `CkGroupID`, which
counted every group in the program; now it counts arrays. An array bound to
another with `bindTo` has its own number but shares the other array's location
manager and layout.

**Payload.** What the payload means is decided per array, by its `CkLocMgr`,
from whether the array has bounds:

- *Packed kind.* An array whose bounds fit in the payload stores the element's
  index itself, packed by `FixedArrayIndexCompressor` (`ckarrayindex.h`). Every
  `CkArrayOptions` constructor that takes a size sets the bounds to that size, so
  almost every array is packed kind. A 3D array of 65,536^3 elements needs 48
  bits.
- *Hashed kind.* An array without usable bounds (an empty `CkArrayOptions`, or
  bounds wider than the payload) mints a payload per element at insertion: a home
  key, which is a hash of the index, followed by a number that is unique within
  the array. The key width H is `ceil(log2(processes * F))`, at most 24, where F
  is the expand factor (`+objid_expand`, default 8); it is computed at the first
  launch, stored in the checkpoint and reused by every restart, because keys are
  embedded in live ids. F is headroom for shrink/expand: a restart can grow to F
  times the launch-time process count and every process still owns home keys.

`ObjID` itself has no kind bit. The accessors are `getCollectionNumber()`,
`getElementID()` (the payload, which the location tables key on) and
`getID()` (the whole word; two ids name the same element iff they are equal).

## 2. The home of an element

The home PE holds the directory entry for an element: where it currently is.
One function computes it from either the index or the id, and both routes give
the same answer by construction:

- Packed kind: `homePe(index)` of the array's map, exactly as before the
  redesign; from an id, decompress first.
- Hashed kind: the home process is `key % CkNumNodes()`, and the PE within that
  process is `CkNodeFirst(process) + (key / CkNumNodes()) % CkNodeSize(process)`,
  so that a process's homes are spread over its PEs. The map's `homePe` is not
  consulted for hashed-kind arrays; its `procNum` still decides where elements
  are placed. The key is `ck::indexHashKey(index)`, a splitmix64 mix of the
  index words, a pure function of the index so every PE and every restart agrees.

No PE number is stored anywhere in an id, which is what makes G2 of the design
hold: a home is always defined under the current process count. If a restart has
more than 2^H processes, homes still exist (the modulo is always defined); only
the directory spread is limited to 2^H processes, and the runtime prints a
warning.

## 3. Location tables and delivery

The record table and the buffered messages are per PE; the location table, the
index table and the retired ids are per process since objid PR 3, shared by all
PEs of the process without a lock on the lookup path. The shared tables are the
chapter [process-location-cache.md](process-location-cache.md); this section
gives the protocol they serve.

| table | key | value | owner |
|---|---|---|---|
| `CkLocMgr::hash` | id | `CkLocRec*` (index, LB handle, timing) | the PE that holds the element |
| `ck::objid::LocTable` (`locTable()`) | full id | {pe, epoch, waiter bits} | every process that has heard where the element is; authoritative at the home |
| `ck::objid::IndexTable` (`indexTable()`) | (collection, index) | full id | hashed kind only: the process that created or holds the element, and the home's |
| `ck::objid::RetiredIds` (`retiredIds()`) | full id | index | hashed kind only: ids that no longer name a live element: an id this process rebound to a newer one, and, at the home, every dead element's id; any PE re-addresses messages sent to such an id |
| `CkArray::bufferedIDMsgs` and siblings | id or index | messages waiting for a location | the sending PE |

Delivery by id: a message carries the element id. The sender looks the id up in
its process's location table; a hit sends the message to that PE, a miss sends
it to the home. A PE that receives a message for an element it no longer holds forwards it
along its own cache entry, and the forwarded delivery teaches the original
sender the current location (the sender-repair rule), so a stale cache costs one
extra hop once. Delivery never needs the index on the sender's PE; the index
travels in the message only for insertion and demand creation.

Delivery by index (a proxy indexed by `CkArrayIndex`): packed kind compresses
the index to the id locally and proceeds as above. Hashed kind needs the id of an
element its process has never heard of, so it buffers the message and asks the
home (`requestLocation(index)`); the home answers with the id and location from
its index table, and the sender's process records both.

Migration: the element's record moves with it; the new PE registers the location
at the home (an epoch counter orders updates from the same element, so a late
message from an earlier migration cannot overwrite a newer location). The load
balancer's global update path (`CMK_GLOBAL_LOCATION_UPDATE`, off in every build)
finds the location manager from the migration record's object-manager id, which
is the manager's group id, and the element from the id's payload.

Deletion: the home drops the directory entry but keeps the index-to-id binding
and records the dead id in `retiredIds` (`CkLocMgr::noteDead`, from `reclaim` when
the element died on the home and from `reclaimRemote` otherwise); a message that
still reaches a PE by id for a deleted element is handed to the home, which
recovers the index from that table, in constant time, and holds the message by
index until an element of that index exists again (or creates one, if the message
asks for demand creation). The other processes keep the dead element's location,
and one of them may be told to create it again. A demand creation the home
approved (`CkArray::demandCreateElement`) is created on the requesting PE without
looking at that entry: the home grants it only when its table has no location
for the id, and bound arrays share the id, so no sibling is alive.
`CkArray::insertElement` forwards an explicit insertion to the PE the entry names
only when the location manager serves bound arrays
(`CkLocMgr::servesBoundArrays`), where the entry is a live sibling's; when every
sibling has died the entry is stale and the insertion is misplaced (#4031).

Re-creation under a new id: an element inserted without a PE argument lands on
the map's PE, which for the hashed kind is not its home, so a deleted index
re-created on a PE that never saw it gets a fresh unique number. PEs that cached
the old id keep sending to it. Any PE that rebinds an index to a different id
records the old id in `retiredIds` too; a PE that recovers an index from the
table and finds it bound to a newer id re-addresses the message to that id
(`CkArray::handleUnknownByID`), and the home also sends the original sender the
new binding by index. Messages a PE held under the old id while it asked the home
are re-addressed by that PE when the answer, by index, names the new id
(`CkArray::releaseRetiredMsgs`). The table grows like the index-to-id table and
is never pruned.

## 4. Unique numbers for the hashed kind

Each process holds a tranche of unique numbers per hashed-kind array, so local
insertion mints an id synchronously. The initial tranche is half of the unique
space divided by the launch-time process count; the other half is the pool of an
allocator on PE 0, which hands out refills on request and serves every process
after a restart, when the launch-time shares are already spent. An insertion that
finds its tranche empty is deferred: the message is re-sent once the refill
arrives. `+objid_tranche_log2` caps the tranche size (used by tests to force
refills).

## 5. Checkpoint and restart

The checkpoint stores, before the array data, a marker and the per-run layout:
H, the unique width, F, the tranche cap and the launch-time process count. A
checkpoint from before the redesign has no marker and is refused at the marker.
(PR 1d adds C and the wide width to the record, so that a build with another C
refuses the checkpoint with a message naming `CMK_OBJID_COLLECTION_BITS`;
before that, such a restore is not detected.) Element ids are restored
unchanged, including under a different process count. Directories are rebuilt
from the restored elements, not restored: each element registers at its home,
which the new process count defines. With PR 1d, each `CkLocMgr` and `CkArray`
pups its collection number and kind, and PE 0 moves its counters past every
restored number before any new array is created.

## 6. Limits and their diagnostics

Every limit aborts, or warns, with a message that names what to change:

| limit | what happens |
|---|---|
| 2^C - 1 arrays (4,095 at C = 12) | at half the range PE 0 prints a suggestion once (naming `setWideCollection`); at the limit `CkCreateArray` aborts naming the option and the build flag |
| packed index wider than the payload | the array silently becomes hashed kind; a sized array without a compressor is a sign of bounds wider than 49 bits |
| unique space too small (U < 16 at startup, or a tranche request that cannot be served) | abort naming `+objid_expand` and `setBounds` |
| more than 2^H processes at restart | warning; homes still defined |
| location or index table full | grows by a segment four times larger; `+objid_table_log2` sets the initial size, `+objid_diag` prints the sizes and counters per process at exit |

## 7. The wide layout (PR 1d, #4050)

A program that creates very many small arrays (AMPI: one per communicator)
exhausts 4,095 collection numbers. `CkArrayOptions::setWideCollection()` gives an
array the wide layout: tag `001`, a 24-bit collection number from a second
counter (standard arrays keep the low range, so a wide array never pushes a
standard array out of numbers), and a 37-bit payload that must be a packed
index; a wide array without bounds that pack into 37 bits aborts at creation.
Only the decoder of the collection field (the global location update) needs the
tag; delivery keys on the whole id word. The checkpoint records the wide width
next to C. Collection numbers are not reused when an array is destroyed, so a
library that creates an array per operation takes the wide layout: TCharm (one
array per AMPI communicator) and CkIO (one per write session, `WriteSession`,
and one per read session, `BufferChares`).

## 8. Not covered: singleton chares and groups

Singleton chares have no `ObjID`. They are addressed by `CkChareID`, a PE number
plus an object pointer (`charm.h`), or in builds without `CMK_CHARE_USE_PTR` an
index into the per-PE chare table so that checkpoints can re-create them. They
do not migrate and have no location management. Groups and nodegroups are
addressed by `CkGroupID` alone; no `ObjID` is ever minted for them.

## 9. Where in the source

| what | where |
|---|---|
| id layout, accessors | `src/ck-core/objid.h` |
| index packing | `src/ck-core/ckarrayindex.h`, `FixedArrayIndexCompressor` |
| per-run layout (H, F), hash key, tranche allocator, location manager, cache, checkpoint of the layout | `src/ck-core/cklocation.h`, `src/ck-core/cklocation.C` (namespace `ck::objid`, class `CkLocMgr`, class `CkLocCache`) |
| process-level location table, index table, retired ids | `src/ck-core/cklocnode.h`, `src/ck-core/cklocnode.C` (chapter [process-location-cache.md](process-location-cache.md)) |
| collection numbers, delivery by id, buffering | `src/ck-core/ckarray.C` (`CkCreateArray`, `CkArray::deliver*`) |
| array options (bounds, wide) | `src/ck-core/ckarrayoptions.h` |
| tests | `tests/charm++/objid_bound`, `objid_home`, `objid_insert`, `objid_sections`, `objid_tranche`, `objid_wide`, `objid_migrate6`, `objid_loctable`, `unbounded_restart`, `unbounded_migration` |
