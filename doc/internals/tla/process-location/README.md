# Process-level location table (charm PR #4123) — TLA+ model

Model of the location-management protocol of the process-level location table
(charmplusplus/charm PR #4123, "Object id PR 3: process-level location and
index tables"). The design chapter is
[process-location-cache.md](../../process-location-cache.md); this directory holds the model that checked
its protocols, the configurations that were run, and the counterexamples that
led to the rules of chapter section 3.5. It is kept as a worked example of
checking a runtime protocol with TLA+: the source-mapping table below ties
every model action to the functions in `src/ck-core/`, so the model can be
updated when they change and the configurations rerun.

Two revisions of the code are modeled, selected by the constant `Fix`:

- `Fix = FALSE`: the code at `cc84660f1` (2026-11-03 morning), before the
  deletion findings below were fixed.
- `Fix = TRUE`: the code after the fix, commit `339753ce0` (life tags, home
  record) and `2f144b0e7` (owner-write protection, by-id drop, older-than-
  record drop), both of 2026-11-03.

Files:

- `ProcLoc.tla` — the specification.
- `MC_*.cfg` — TLC configurations (see "Results"); `MC_*_fix.cfg` set
  `Fix = TRUE`, the others `Fix = FALSE`. Two more constants: `Rec` (with
  `Fix`: the home keeps its record of lives; `FALSE` leaves only the two free
  rules, the `MC_*_norec*.cfg` configurations), `DeadSet` (with `Fix` and not
  `Rec`: tags and the home's set of dead lives, no per-life location, the
  notice clears unconditionally; `MC_*_deadset*.cfg`), `Sync` (`TRUE`: a
  re-creation happens only after every runtime message of the earlier life has
  been delivered, the contract of the design document's section 7.8; the
  `*_sync` configurations) and `AppSync` (`TRUE`: the element is deleted only
  when no application message to it is in flight or held, the weaker condition
  of section 7.8; `*_appsync` configurations).
- `trace_*.txt` — the counterexamples of the Findings section, cut from the
  TLC output (one state per block). The full logs are not kept here; the
  Results tables give their state counts and times.

TLC is `tla2tools.jar` from https://github.com/tlaplus/tlaplus/releases (TLC
2.20 was used, with a Java 17 runtime). Run one check with:

    java -XX:+UseParallelGC -Xmx3g -cp tla2tools.jar tlc2.TLC -deadlock -workers 3 -config MC_safety.cfg ProcLoc.tla

(`-deadlock` because a quiescent system is a legitimate final state.) Parse
only: `java -cp tla2tools.jar tla2sany.SANY ProcLoc.tla`. TLC writes
checkpoints under `states/`; delete them after a run (a day of runs left
26 GB).

## What is modeled

Two processes of two PEs each (constants `Procs = {0,1}`, `PPN = 2`; PE p is in
process p ÷ 2), plus runs with three processes of one PE for finding 3. One
element of a **packed-kind** array: its index packs into its id, so `lookupID`
always succeeds, `recoverIndex` never fails, and the id is never rebound (no
index table, no retired ids). The element does not exist at the start: it is
created during the run on any PE (`Create`), migrates within and across
processes (`Emigrate`, bounded by `MaxMig`), and, with `MaxDel > 0`, dies
(`Delete`) and is created again under the same id at epoch 0 (bounded by
`MaxCreate`). Point-to-point messages with the default *buffer* if-not-there
policy, each small or large (large: held where it cannot be delivered instead
of being sent to the home). The network delivers in any order (reconverse makes
no ordering promise, `contrib/reconverse/README.md`).

### Concurrency

The PEs of a process share one table entry per element: a value word (location
`pe`, epoch `ep`, and with `Fix` the `own` mark, see below) and a waiter word
(`w`, modeled as a set of PEs; the 64-bit fold is not modeled). A handler is
atomic only against the other handlers of its own PE; the PEs of a process
interleave at every table access. The one handler whose table accesses
interleave in a way that matters is `CkLocCache::requestLocation(id)`: register
the waiter bit, then re-read the value. It is split into two steps with a
per-PE program counter `pc`; while `pc[p]` is not idle no other handler runs on
p (its own handlers are serial), while the other PEs run freely. Store/update
plus the take of the waiter bits are one step: the writer's CAS and its
exchange are two atomics, and a registration between them only earns a spare
wake. Emigration's "record, then send" is one step: the migration message is
received in a later step anyway, so nobody can observe the gap.

### Lives

Each creation is a life, numbered by the `creates` counter (the code mints
`CkLocEntry::life` from a process-wide counter and the process number). Home
updates (`upd` with `idx = TRUE`) and death notices (`dead`) carry the life of
the element they describe; replies and repairs by id (`upd` with `idx = FALSE`)
carry `life = 0`, as in the code, which has no life in the table. The migration
message carries the current life implicitly. With `Fix = FALSE` the fields are
carried but ignored.

### Source mapping

| model action | source |
|---|---|
| `Send` | `ckarray.C` `CkArray::sendMsg` (packed: id known; `whichPe` reads the process table), `handleUnknown` (small, not home: send to the home), `bufferForLocation` (hold under the id) |
| `Hold` / `Reread` | `CkArray::bufferForLocation` → `CkLocCache::requestLocation(id)`: `LocTable::registerWaiter` (first = word was 0), re-read; found: `unregisterWaiter`, send; not found: the first registrant sends `requestLocation(id, me)` to the home unless the home is in this process; then push to `bufferedIDMsgs` |
| `RecvApp` deliver | `CkArray::recvMsg` (hops++), `deliverToElement`, `CkLocMgr::multiHop`: no repair for a sender in this process; else `CkLocCache::requestLocation(id, srcPe)` run locally → update by id to the sender. With the entry empty the request is held on this PE, which is not the home, and never answered: modeled as no repair |
| `RecvApp` forward | `CkArray::recvMsg`: follow the process table; after two hops send to the home, except at the home |
| `RecvApp` no entry | `CkArray::handleUnknownByID`: at the home hold (`bufferForLocation`, id path); small and not home → send home; large → hold |
| `Create` | `CkLocMgr::createLocal(idx, ..., epoch 0)`: `CkLocCache::insert` = `LocTable::store` (W1) + `wakeRanks`; `updateLocation(idx, e)` locally (flush own buffer, answer `bufferedLocationRequests` at the home; Fix: record the life when this PE is the home); `informHome(idx, e)` unless the home is this PE |
| `Emigrate` | `CkLocMgr::emigrate`: `recordEmigration` (W2; the entry must name this PE, else `CmiAbort`), migration message with the new epoch, `informHome(idx, {id, toPe, newEpoch})` unless the home is source or destination (Fix: recorded on the spot when the home is the source) |
| `RecvImm` | `CkLocMgr::immigrate` → `createLocal(notifyHome = false)`: `store` (W3) + wakes; local `updateLocation(idx, e)` (Fix: recorded when the destination is the home) |
| `RecvUpd`, `idx = TRUE` | `CkLocMgr::updateLocation(idx, e)` (informHome, replies by index). Fix: at the home `homeNoteLocation` first (below). Then `CkLocCache::applyLocation(e)` = `LocTable::update` (W4, epoch compare; Fix: `received = true`, never over an own write) + wakes if written; then `notifyListeners(idx)` **unconditionally** (flush toward e.pe) and, at the home, answer `bufferedLocationRequests` |
| `RecvUpd`, `idx = FALSE` | `CkLocCache::updateLocation(e)` (reply to a request by id, sender repair). Fix: dropped in the home's process; else W4 + wakes; flush only if written |
| `RecvReq` | `CkLocCache::requestLocation(id, peToTell)` at the home: answer by id from the entry, or hold the request (`CkLocMgr::requestLocation(idx, peToTell)`, `bufferedLocationRequests`) |
| `RecvWake` | `CkLocCache::wake(id)`: location known → flush; else (not a retired id here) re-register while messages are held |
| `Delete` | `CkLocMgr::reclaim` after `ckDestroy`: read the epoch, `removeFromTable` → `clear` while the entry names this PE (waiter bits kept), `reclaimRemote(idx, id, pe, epoch, life)` to the home unless this PE is the home (Fix: then `homeNoteDeath` on the spot) |
| `RecvDead` | `CkLocMgr::reclaimRemote` at the home: `cache->erase(id, deletedOnPe, epoch)`, a compare-and-clear. Fix: `homeNoteDeath` (below) |

Abstractions: `hops` saturates at 2 (the code only tests `hops > 1`); identical
update, request and wake messages in flight are merged (all idempotent); the
per-message counter `b` counts forwards, for the probe only.

Not modeled: the hashed kind (index table, rebinding, retired ids, the
`releaseRetired` paths), demand creation, `CMK_GLOBAL_LOCATION_UPDATE`,
zero-copy, the GPU half, inline sends, more than 64 PEs per process, table
growth (one key), eviction, checkpoint/restart.

### The fix (`Fix = TRUE`)

Four rules, all in chapter section 3.5. Model names in parentheses.

1. **Life tags.** Entries, migration messages, home updates and death notices
   carry the life (above).
2. **The home's record of lives** (`hl`, `dead`; code `homeLives`,
   `deadLives`). The home keeps the latest location it has seen for each life
   not known dead. An update by index whose life is known dead, or whose epoch
   is below the record of its life (`Stale`), is dropped after clearing the
   entry while it still names that update's location (the home may have missed
   the update when the notice came; or the update is a state the life has
   left, and would revive a cleared entry). A notice adds the life to `dead`,
   forgets its record, and clears the entry while it names where the life died
   or the location recorded for that life. After any clear, if the entry is
   empty and another life is recorded, that life is installed through the
   update path (`Install`; code `homeReinstall` → `updateLocation(idx, e)`),
   which flushes the home's held messages and answers held requests.
3. **Owner-write protection** (`own`; code `byMessage` bit in the value word).
   A value written from a received update (W4) is marked; what a PE of this
   process wrote about itself (W1, W2, W3: unmarked, naming a PE of the
   process; `OwnWrite`) is never replaced by a received update (`Written`) nor
   cleared by the home's clears on a notice or a stale update. A received
   update naming a local PE is still applied to an empty or marked entry, so a
   reply about an element landing in this process keeps flushing.
4. **By-id drop in the home's process.** A reply or repair by id (life 0)
   arriving at any PE of the home's process is dropped: the home's by-index
   path and the process's own writes keep that entry.

Rules 1 and 2 are the record (`Rec`); rules 3 and 4 cost no messages and no
state and are kept in every variant. `Rec = FALSE` with `Fix = TRUE` models
the code without the record and the tags (design document section 7.8, option
C), and `Sync = TRUE` adds the contract that a re-creation waits for the
earlier life's runtime messages to be delivered (`Create` is enabled only when
no update, notice, request or wake is in flight).

## Properties

Safety (`Inv`, plus the action property `TableMonotone`):

- `ExactlyOnce` — every sent message is, at every moment, in exactly one place:
  in flight, held in a buffer, held by a PE mid-request, or delivered once.
- `OneElement` — the element is on one PE or inside one migration message.
- `HolderEntryExact` — the holder's **process** entry names the holder at the
  element's epoch. This is the condition `LocTable::recordEmigration` aborts
  on, and what a same-process sender relies on to send direct.
- `EpochOK` — no process entry is ahead of the element (without deaths).
- `WaiterCoverage` — a PE that holds messages has its waiter bit set, or a
  wake, an update or the element itself is on its way to it, or it is
  mid-request. (The chapter's section 3.3 rule, for the id-held half.)
- `RequestOutstanding` — a process that waits with no location, with the home
  elsewhere, has a request in flight or held at the home, or a reply coming,
  or its first registrant has not yet sent it.
- `TableMonotone` — a process entry's epoch never moves back (without deaths).

Weaker forms of `HolderEntryExact`, used to see past finding 1 (TLC stops at
the first violation, so a second ordering under the same invariant stays
hidden until the first is excluded):

- `HolderEntryNotStale` — the holder's process entry, when it has a location,
  is the holder's (an empty entry, finding 1, passes).
- `StaleOnlyBeforeNotice` — `HolderEntryNotStale` once no death notice is in
  flight.
- `SettledHolderExact` — `HolderEntryExact` once no notice, home update or
  migration message is in flight.

Liveness (weak fairness on receiving each message in flight, on finishing a
split request, and on creation):

- `EventuallyDelivered` — every sent message is delivered.
- `HomeConverges` — the home's process entry ends up exact.
- `Quiesces` — the network, all buffers and all split requests drain.
- `NetDrains` — the network alone drains (used when a message to an element
  that died and is not re-created is expected to end up held at the home).

## Results

All runs on the final `ProcLoc.tla` except where the "distinct states" column
notes otherwise. Two processes of two PEs unless noted; TLC 2.20, 1 to 4
workers, Apple M-series laptop. "to the hit" counts states explored before the
first violation.

### Code as written (`Fix = FALSE`)

| config | Home | MaxMig | msgs | MaxDel | checks | distinct states | time | result |
|---|---|---|---|---|---|---|---|---|
| `MC_safety_final` | 1 | 2 | 2 | 0 | `Inv`, `TableMonotone` | 56,283,390 | 23m56s | pass |
| `MC_safety_home0` | 0 | 2 | 2 | 0 | `Inv`, `TableMonotone` | 56,283,390 | 24m31s | pass |
| `MC_safety` | 1 | 2 | 2 | 0 | `Inv`, `TableMonotone` | 56,283,390 (rerun with the life fields) | 27m13s | pass |
| `MC_safety_1mig` | 1 | 1 | 2 | 0 | `Inv`, `TableMonotone` | 3,847,407 | 1m36s | pass |
| `MC_liveness` | 1 | 1 | 1 | 0 | `Inv` + 3 liveness properties | 17,077 | 12s | pass |
| `MC_liveness_2mig` | 1 | 2 | 1 | 0 | `Inv` + 3 liveness properties | 160,855 | 2m56s | pass |
| `MC_probe_bounce` | 1 | 1 | 1 | 0 | `Inv`, `BoundedForwarding` (probe) | 4,525 to the hit | 0s | probe hit in 9 steps: finding 4 |
| `MC_death_safety` | 1 | 1 | 1 | 1 | `Inv`, `TableMonotone` | 778 to the hit | 0s | `Inv` violated in 6 steps: finding 1 |
| `MC_death_liveness` | 1 | 1 | 1 | 1 | `Inv` + 3 liveness properties | 680 to the hit | 0s | same 6-step trace |
| `MC_death_safety_2msg` | 1 | 1 | 2 | 1 | `Inv`, `TableMonotone` | 6,656 to the hit | 0s | same 6-step trace |
| `MC_death_stale` | 1 | 1 | 1 | 1 | `TypeOK`, the three weaker forms | 4,498 to the hit | 0s | `SettledHolderExact` violated: finding 1 in its settled form |
| `MC_death_settled` | 1 | 1 | 1 | 1 | `TypeOK`, `StaleOnlyBeforeNotice`, `SettledHolderExact` | 4,657 to the hit | 0s | same |
| `MC_death_notstale` | 1 | 1 | 1 | 1 | `TypeOK`, `HolderEntryNotStale` | 11,194 to the hit | 1s | violated in 7 steps: finding 5 |
| `MC_death_liveness_noinv` | 1 | 1 | 1 | 1 | 3 liveness properties | 101,985 | 1m03s | `HomeConverges` violated in 9 steps: finding 2 |
| `MC_death_3proc` (3 procs x 1 PE) | 0 | 1 | 1 | 1 | `EventuallyDelivered`, `Quiesces` | 141,296 | 1m03s | `EventuallyDelivered` violated in 13 steps: finding 3 |

### With the fix (`Fix = TRUE`)

| config | Home | MaxMig | msgs | MaxDel | checks | distinct states | time | result |
|---|---|---|---|---|---|---|---|---|
| `MC_safety_1mig_fix` | 1 | 1 | 2 | 0 | `Inv`, `TableMonotone` | 3,946,417 | 1m38s | pass |
| `MC_liveness_fix` | 1 | 1 | 1 | 0 | `Inv` + 3 liveness properties | 17,758 | 11s | pass |
| `MC_death_safety_fix` | 1 | 1 | 1 | 1 | `Inv`, `TableMonotone` | 1,541,300 | 29s | pass |
| `MC_death_stale_fix` | 1 | 1 | 1 | 1 | `TypeOK`, the three weaker forms | 1,541,300 | 27s | pass |
| `MC_death_settled_fix` | 1 | 1 | 1 | 1 | `TypeOK`, `StaleOnlyBeforeNotice`, `SettledHolderExact` | 1,541,300 | 27s | pass |
| `MC_death_liveness_fix` | 1 | 1 | 1 | 1 | `Inv` + 3 liveness properties | 1,541,300 | 31m56s | pass |
| `MC_death_liveness_fix_noinv` | 1 | 1 | 1 | 1 | 3 liveness properties | 1,541,300 | 31m22s | pass |
| `MC_death_3proc_fix` (3 procs x 1 PE) | 0 | 1 | 1 | 1 | `EventuallyDelivered`, `Quiesces` | 210,446 | 2m06s | pass |
| `MC_death_safety_2msg_fix` | 1 | 1 | 2 | 1 | `Inv`, `TableMonotone` | running (started 16:21, 2026-11-03; 2.3M states after 1 min) | | result to be appended to this line |

### Reduced protocols, with and without the contract (`Rec = FALSE`)

Run for the design document's section 7.8 (is the record needed, and what does
a synchronization between deletion and re-insertion buy). "no record": only
the two free rules (own writes stay, by-id drop). "dead set": tags and the set
of dead lives, unconditional clear on a notice, no per-life location.

| config | protocol | Sync | checks | distinct states | time | result |
|---|---|---|---|---|---|---|
| `MC_death_safety_norec` | no record | no | `Inv`, `TableMonotone` | 1,877,252 | 54s | pass |
| `MC_death_liveness_norec_noinv` | no record | no | 3 liveness properties | 118,831 | 1m03s | violated (finding 3's race) |
| `MC_death_3proc_norec` (3 x 1) | no record | no | `EventuallyDelivered`, `Quiesces` | 127,617 | 1m03s | violated |
| `MC_death_safety_norec_sync` | no record | yes | `Inv`, `TableMonotone` | 206,549 | 4s | pass |
| `MC_death_stale_norec_sync` | no record | yes | the three weaker forms | 206,549 | 4s | pass |
| `MC_death_liveness_norec_sync` | no record | yes | `Inv` + 3 liveness properties | 106,918 | 1m06s | `HomeConverges` violated in 12 steps: finding 8 |
| `MC_death_3proc_norec_sync` (3 x 1) | no record | yes | `EventuallyDelivered`, `Quiesces` | 50,136 | 20s | violated in 22 steps: finding 8 |
| `MC_death_only` (`Fix = FALSE`, MaxCreate 1) | code as written | | `NetDrains` | 128,235 | 35s | violated in 13 steps: finding 8 |
| `MC_death_only_fix` (MaxCreate 1) | record | | `NetDrains` | 91,558 | 26s | pass |
| `MC_death_safety_deadset_sync` | dead set | yes | `Inv`, `TableMonotone` | 168,346 | 3s | pass |
| `MC_death_stale_deadset_sync` | dead set | yes | the three weaker forms | 168,346 | 3s | pass |
| `MC_death_liveness_deadset_sync` | dead set | yes | `Inv` + 3 liveness properties | 168,346 | 2m09s | pass |
| `MC_death_3proc_deadset_sync` (3 x 1) | dead set | yes | `EventuallyDelivered`, `Quiesces` | 40,036 | 17s | pass |
| `MC_death_3proc_deadset` (3 x 1) | dead set | no | `EventuallyDelivered`, `Quiesces` | 144,054 | 1m03s | violated in 14 steps: finding 3 (`trace_deadset_nosync_3proc.txt`) |
| `MC_death_liveness_deadset_noinv` | dead set | no | 3 liveness properties | 124,974 | 1m04s | violated |
| `MC_death_3proc_deadset_appsync` (3 x 1) | dead set | AppSync only | `EventuallyDelivered`, `Quiesces` | 98,194 | 1m03s | violated in 12 steps (`trace_appsync_lost_message_3proc.txt`): the weaker condition does not order the notice against the new life's update |
| `MC_death_liveness_deadset_noinv_appsync` | dead set | AppSync only | 3 liveness properties | 96,305 | 1m03s | violated |

The no-death configurations have more states with `Fix` because the `own`
mark distinguishes entries that were equal before; the property set is the
same and still passes. An earlier oracle (`LifeTag`, a life on every entry
and an "update from a newer life always wins" rule, not implementable as such
because the table carries no life) passed the same death configurations and
showed that findings 1 to 3 share one cause; it was replaced by the `Fix`
model of the code actually written.

Traces are saved as `trace_*.txt` (TLC output, one state per block).

## Mutation checks

Each mutation (`Mutation` constant, with `Fix = FALSE`) is a deliberate
protocol error; each is caught.

| `Mutation` | protocol error | source step removed | caught by (`MC_mutN`, with `Inv`) | states, steps | liveness-only run (`MC_mutN_live`) |
|---|---|---|---|---|---|
| 1 | a move is not recorded in the source's process table | `CkLocMgr::emigrate` → `LocTable::recordEmigration` (W2) | `HomeConverges`: home = source, so no home update is sent either, and the home's entry keeps naming the source at the old epoch | 6,702, 7 | same run |
| 2 | a PE that holds a message does not register as a waiter | `CkLocCache::requestLocation(id)` → `LocTable::registerWaiter` | `WaiterCoverage`: a held message with no bit and nothing on the way | 22, 3 | `EventuallyDelivered`, 9 steps |
| 3 | a store takes the waiter bits but wakes nobody | `CkLocCache::insert` / `updateLocation` → `wakeRanks` | `WaiterCoverage`: the creation fills the entry and clears the bits, the holding PE is never told | 183, 4 | `EventuallyDelivered`, 9 steps |
| 4 | the first registrant sends no request to the home | `CkLocCache::requestLocation(id)` → `CkLocMgr::requestLocation(id, me)` | `RequestOutstanding`: a waiting process with no request in flight or held | 77, 3 | `EventuallyDelivered`, 7 steps |

Traces: `trace_mut1.txt` to `trace_mut4.txt`.

## Findings

The growth, waiter and request protocol of the chapter holds in every
configuration without deaths. All findings involve deletion or a message that
reaches a destination before the element lands. Findings 1 to 3 and 5 are in
the base branch too in some form (per-PE `CkLocCache::updateLocation` has the
same strict epoch compare, `objid-redesign` cklocation.C:2786, and its
`reclaimRemote` erased unconditionally); 5 and 6 reach the live element's own
entry only with the shared table. Steps name code terms; PE p is in process
p ÷ 2 (two-process runs) or p (three-process runs). Findings 1, 2, 3, 5, 6, 7
and 8 are closed by the fix (the `_fix` rows above); 4 is not a defect.

Code coverage of the same orderings: `tests/charm++/objid_recreate` drives
the home with hand-built updates and notices (seven scenarios, 166 checks,
chapter section 7) and fails on the code as written.

### 1. Late death notice clears the re-created entry

`MC_death_safety`, `trace_death_residual.txt`, Home = PE 1 (process 0).
Was the chapter's known residual (old sections 3.5 and 9).

1. A PE with no location sends message 1 to the home.
2. `createLocal` on PE 0, epoch 0: process 0 entry {PE 0, 0}; `informHome` sends {PE 0, 0} to PE 1.
3. `reclaim` on PE 0: `clear` empties the entry; `reclaimRemote(idx, 0, 0)` to PE 1.
4. `createLocal` on PE 0 again, epoch 0: entry {PE 0, 0}; a second `informHome`.
5. PE 1 runs `reclaimRemote`: `erase(id, 0, 0)` matches the live entry and clears it.

`HolderEntryExact` fails: the element is on PE 0 and its process has no entry.
The window closes when either home update lands (the entry is empty, so epoch 0
is written). Inside the window, `emigrate` of this element hits the `CmiAbort`
in `recordEmigration`, and messages sent from process 0 go to the home and are
held there. The violation needs the home in the holder's process; with the home
elsewhere only the home's view is empty for the window.

Fix: rule 3. PE 0's second `store` is an own write of process 0; the home's
clear skips it.

### 2. A stale creation update revives the entry at epoch 0 and the new life cannot overwrite it

`MC_death_liveness_noinv`, `trace_stale_create_update.txt`, Home = PE 1 (process 0).

1. `createLocal` on PE 3 (process 1), epoch 0: process 1 entry {PE 3, 0}; `informHome` {PE 3, 0} to PE 1, delayed.
2. `emigrate` PE 3 → PE 1: process 1 entry {PE 1, 1}; migration message to PE 1; no home update (the home is the destination).
3. PE 1 `immigrate`: process 0 entry {PE 1, 1}.
4. `reclaim` on PE 1 (the home): `clear` empties the process 0 entry; no notice.
5. `createLocal` on PE 2 (process 1), epoch 0: process 1 entry {PE 2, 0}; `informHome` {PE 2, 0} to PE 1.
6. PE 1 receives the delayed update from step 1: the entry is empty, `LocTable::update` writes {PE 3, 0}.
7. PE 1 receives the update from step 5: `epoch <= c.epoch()` (cklocnode.C:189, 0 <= 0), not written.

`HomeConverges` fails: the home names PE 3 at epoch 0 while the element is on
PE 2 at epoch 0, and no later message corrects it. Messages from process 0 go
home, to PE 3, and are forwarded by process 1's table to PE 2 (two extra hops);
the sender repair from `multiHop` carries {PE 2, 0} and is rejected the same way.
The state lasts until the element migrates (epoch 1 wins). The old chapter
section 3.5 said such a revived entry "is healed by the repair rule like any
stale entry"; that held only when the new life had a higher epoch than the
stale update.

Fix: rule 2. The home records the death of life 1 in step 4 (`homeNoteDeath`
on the spot) and drops the update of step 6 as a dead life's; the update of
step 5 is then written.

### 3. A message is held forever: the new life's creation update loses to a stale higher epoch, then the death notice clears the home

`MC_death_3proc`, `trace_lost_message_3proc.txt`, three processes of one PE,
Home = PE 0. Needs three processes: the new life must be in a process other
than the home's (else `store` writes unconditionally) and other than the dead
PE's (else that process's table forwards correctly).

1. `createLocal` on PE 0 (the home), epoch 0: process 0 entry {PE 0, 0}.
2. `emigrate` PE 0 → PE 1: process 0 entry {PE 1, 1}; migration message; no home update (home is the source).
3. PE 1 `immigrate`: process 1 entry {PE 1, 1}.
4. A PE whose table names PE 1 sends a large message 1 to PE 1.
5. `reclaim` on PE 1: process 1 entry cleared; `reclaimRemote(idx, 1, 1)` to PE 0.
6. `createLocal` on PE 2, epoch 0: process 2 entry {PE 2, 0}; `informHome` {PE 2, 0} to PE 0.
7. PE 0 receives the update first: entry {PE 1, 1}, `0 <= 1`, not written.
8. PE 0 receives the death notice: `erase(id, 1, 1)` matches, entry cleared. No further update will come.
9. Message 1 arrives at PE 1: no element, no entry; `handleUnknownByID` holds a large message (`bufferForLocation`).
10. PE 1 `requestLocation(id)`: registers, re-reads nothing, first registrant sends `requestLocation(id, 1)` to the home.
11. PE 0 `requestLocation(id, 1)`: no location, the request joins `bufferedLocationRequests`.

`EventuallyDelivered` fails: message 1 sits in `bufferedIDMsgs` on PE 1 and its
request at the home, while the element lives on PE 2; quiescence is never
reached. A small message takes the same path via the home (held there by
`bufferForLocation`). The state lasts until the element on PE 2 migrates. The
pre-PR code has the same compare and an unconditional erase, so the same
ordering loses the message there. A one-bit "creation flag" cannot order the
two creation updates of finding 2, so the fix needs a per-life record that
outlives the notice.

Fix: rule 2. Step 7 records life 2 at {PE 2, 0}; the notice of step 8 clears
the entry of life 1 and reinstalls life 2, releasing the held request and
messages.

### 4. Bounce probe: the destination PE names itself before the element lands

`MC_probe_bounce`, `trace_bounce.txt`, Home = PE 1 (process 0). The probe
`BoundedForwarding` (forward counter below `BCap = 7`) is not a correctness
property; it reports a forwarding loop. The liveness runs pass, so the loop
ends when the migration message is processed.

1. A PE with no location sends message 1 to the home PE 1.
2. `createLocal` on PE 0, epoch 0: process 0 entry {PE 0, 0}.
3. `emigrate` PE 0 → PE 1: `recordEmigration` writes {PE 1, 1} into the shared process 0 entry; migration message to PE 1 in flight.
4. Message 1 reaches PE 1: no element; `whichPe` reads the shared entry and returns PE 1 itself; `sendToPe(msg, 1, CkDeliver_queue)` enqueues it locally.
5-9. Each pass: `hops > 1`, this PE is the home so it follows its own entry, to itself again.

In the per-PE code the destination did not know the move until `immigrate`, and
such a message bounced over the network between the home and the source
(chapter section 3.4). With the shared table every PE of the destination's
process, the destination included, sees the move as soon as the source records
it. The loop is local and ends when the scheduler reaches the migration
message; the inline variant (`sendToPe` with `CkDeliver_inline`, element
absent, buffer policy) pushes the message into `bufferedIDMsgs` directly and
the landing's local `updateLocation` flushes it. No fix is required; the
arriving state of section 3.4 or Aditya's parking would remove the requeues.

### 5. A stale move update of a dead life overwrites the re-created element's own entry

`MC_death_notstale`, `trace_stale_over_new_life.txt`, Home = PE 1 (process 0).
Reported first by an independent model of PR #4123 built in a separate session
(with the re-creation on PE 2 and the dead life on PE 3). This model had the ordering in its state space from the start but did not
report it: it violates `HolderEntryExact`, and TLC stops at finding 1, which
violates the same invariant in fewer steps. `HolderEntryNotStale` excludes
finding 1 (an empty entry) and surfaces it in 7 steps.

1. `createLocal` on PE 0, epoch 0: process 0 entry {PE 0, 0}; `informHome` {PE 0, 0} to PE 1.
2. `emigrate` PE 0 → PE 2 (process 1): process 0 entry {PE 2, 1}; migration message; `informHome` {PE 2, 1} to PE 1, delayed.
3. PE 2 `immigrate`: process 1 entry {PE 2, 1}.
4. `reclaim` on PE 2: process 1 entry cleared; `reclaimRemote(idx, 2, 1)` to PE 1.
5. `createLocal` on PE 0 again, epoch 0: `store` writes process 0's entry {PE 0, 0}; `informHome` {PE 0, 0} to PE 1.
6. PE 1 receives the delayed update of step 2: `LocTable::update`, 1 > 0, written. Process 0's entry, the live element's own, is {PE 2, 1}.

`HolderEntryNotStale` fails: the element is on PE 0 and its own process names
the dead PE 2. Sends from process 0 go to PE 2, which forwards them to the
home (two hops) and from there to PE 0 once the notice and the creation update
have landed. PE 0's next `emigrate` before that hits
`CmiAbort("LocTable::recordEmigration: the entry of an element leaving PE 0
names PE 2 ...")` (cklocnode.C), in production builds too. New with the shared
table: with per-PE caches step 6 changed only PE 1's private copy.

Fix: rule 3. The entry of step 5 is an own write of process 0; the update of
step 6 is not applied to it (it is recorded at the home, where the notice of
step 4 later drops life 1 from the record).

### 6. A reply or repair by id of a dead life reaches the home's process

Found while checking the fix, on intermediate revisions of the spec
(`MC_death_liveness_fix_noinv`, 11 steps); the traces were overwritten by the
passing reruns and are not kept. Two forms:

- **Over the new life's own entry.** As finding 5, with the stale message a
  sender repair from `multiHop` (or a reply to `requestLocation(id, pe)`)
  carrying {dead PE, 1} instead of a home update. Rule 3 alone covers it the
  same way; rule 2 cannot, since replies and repairs by id carry no life.
- **Reviving an empty entry.** The dead life's repair {dead PE, epoch} arrives
  at a PE of the home's process after the notice has cleared the entry: the
  entry is empty, so the epoch compare writes it (not an own write, so rule 3
  does not apply), and the new life's creation update at epoch 0 then loses to
  it for good, as in finding 2, with no life to drop it by.

Fix: rule 4. Replies and repairs by id are dropped in the home's process; the
home's by-index path (which carries lives) and the process's own writes keep
that entry. Outside the home's process such a message is applied by the epoch
compare as any stale entry is, and the next delivery's repair or the element's
next move corrects it (chapter section 9).

### 7. A creation update older than the home's record revives a cleared entry

Found while checking the fix (`MC_death_liveness_fix`, 11 steps, trace not
kept; the ordering below is reconstructed from it). Home = PE 1 (process 0).
With rules 1 to 4 and a record that only kept the latest epoch per life:

1. `createLocal` on PE 3 (process 1), epoch 0; `informHome` {PE 3, 0} to PE 1, delayed.
2. `emigrate` PE 3 → PE 0 (process 0); `informHome` {PE 0, 1} to PE 1, which arrives: life 1 recorded at {PE 0, 1}, entry {PE 0, 1} written.
3. PE 0 `immigrate`: process 0 entry {PE 0, 1} (own write).
4. `reclaim` on PE 0: process 0 entry cleared (W5); `reclaimRemote(idx, 0, 1)` to PE 1, delayed.
5. `createLocal` on PE 2 (process 1), epoch 0; `informHome` {PE 2, 0} to PE 1.
6. PE 1 receives the delayed update of step 1: life 1 is not yet known dead, its record stands at epoch 1, the update is ignored by the record but applied to the table: the entry is empty, so {PE 3, 0} is written (marked).
7. PE 1 receives the update of step 5: 0 <= 0, not written (recorded for life 2).
8. PE 1 receives the notice of step 4: it clears the entry while it names {PE 0, 1} or the record of life 1, {PE 0, 1}; the entry is {PE 3, 0}, so nothing is cleared, and no reinstall happens because the entry is not empty.

`HomeConverges` fails: the home names PE 3 for good. Rule 2's older-than-
record clause (`Stale`) drops the update of step 6 before it reaches the
table; `MC_death_liveness_fix` and `MC_death_liveness_fix_noinv` pass with
it (1,541,300 states each).

### 8. A death notice outrun by the life's own earlier update: a message bounces for ever

`MC_death_only`, `trace_dead_bounce.txt`, Home = PE 1 (process 0), no
re-creation (`MaxCreate = 1`); also the reason the reduced protocols fail
under the contract (`trace_notice_before_updates_2proc.txt`,
`trace_notice_before_updates_3proc.txt`). Found while checking those variants.

1. `createLocal` on PE 0, epoch 0: process 0 entry {PE 0, 0}; `informHome` {PE 0, 0} to PE 1, delayed.
2. A PE sends message 1 to PE 0.
3. `reclaim` on PE 0: process 0 entry cleared (W5); `reclaimRemote(idx, 0, 0)` to PE 1.
4. PE 1 receives the notice first: the entry is empty, nothing to clear.
5. PE 1 receives the update of step 1: the entry is empty, {PE 0, 0} is written.
6. Message 1 reaches PE 0: no element; the entry names PE 0 itself, so it is requeued; on the second pass (`hops > 1`) it goes to the home.
7. PE 1 follows its entry to PE 0, PE 0 sends it to the home, and so on.

`NetDrains` fails: the message circulates between the home and the dead PE and
never comes to rest. With the home in another process the same ordering is
produced by the network between two processes. No re-creation is involved, so
no synchronization between deletion and re-insertion prevents it. The per-PE
code of recharm has the same ordering: the home PE's cache entry is erased by
the notice and revived by the late update, and a message then bounces between
the home and the dead PE. The compare-and-clear on the notice, which was added
to protect a re-creation, is what lets the stale update through here (an
unconditional clear would also have missed it, since the entry was empty when
the notice came).

Fix: rule 2's dead set. The notice records the life as dead and the late update
is dropped. This is the one part of the record every variant needs: the tag,
to recognize the life, and the set of dead lives; the per-life location and the
reinstall are needed only without the contract (finding 3).
