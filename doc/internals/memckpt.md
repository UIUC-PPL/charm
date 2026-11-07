# Checkpoint, restart, shrink/expand

This chapter will cover the three ways the runtime saves and restores the
state of a program: the double in-memory checkpoint, the disk checkpoint
(`CkStartCheckpoint`, `+restart`), and shrink/expand. Only the first is
written so far.

## The double in-memory checkpoint

Each PE packs all its array elements into one message, keeps one copy and
sends the other to a buddy PE in the next process; each PE also packs its
process-level data (readonly variables, mainchares, singleton chares,
nodegroups) and sends it to the same buddy. A rollback destroys every array
element and recreates it from the PE's own copy, without any process
failing. The code is in `src/ck-core/ckmemcheckpoint.{C,h,ci}`; user-facing
behavior is in the manual, section "Online Fault Tolerance".

### Build option

The protocol is compiled only when `CMK_MEM_CHECKPOINT` is defined, by the
`syncft` (or `memft`) build option: `src/arch/common/conv-mach-syncft.h`
defines it, and `conv-mach-opt.h` includes that file when the option is
given (`CMakeLists.txt`, the `foreach(opt ...)` loop). Several sites test
the macro with `#ifdef`, so it is never defined as 0. Makefiles see it as
`CMK_MEM_CHECKPOINT:=1` in `conv-mach-opt.mak`. Without the option the
group and its entry methods still exist, and `CkStartMemCheckpoint` sends
its callback at once with a warning.

The option changes code outside this file, which is why it is off by
default: `ArrayElement::ArrayElement` calls `init_checkpt`, which sends two
`createEntry` messages per element; `CentralLB.C` collects statistics
point to point instead of by reduction; `CkReductionMgr` disables the
inactive-PE optimization; `CkLocMgr::emigrate` counts migrations
(`CkMemCheckPTNoteMigration`).

The macro `__FAULT__` stays off on reconverse. It selects the classic
dead-process restart (below), which needs a restart phase in the message
header (`CmiGetRestartPhase`) and a crash hook from the machine layer
(`notify_crash_fn`); reconverse has neither, so the Converse handlers of
that path, from `restartBeginHandler` to `CkMemRestart`, are compiled only
with `__FAULT__`.

### Data structures

- `CkMemCheckPT`, a group, one branch per PE (`ckCheckPTGroupID`, created
  by the mainchare `CkMemCheckPTInit`). Its `chkpTable[2]` are
  `CkCheckPTEntry` objects: `chkpTable[0]` holds this PE's own array
  copies, `chkpTable[1]` the copies received from the PE whose buddy this
  PE is. Each entry has two slots, indexed by the per-PE
  `CpvAccess(chkpPointer)`: the slot it names is the last completed
  checkpoint, the other is the one being written.
- `CpvAccess(procChkptBuf)[2]`: the buddy's process-level data, two slots
  indexed the same way.
- Per-PE counters on the group: `recvCount` (array copies stored for the
  current checkpoint), `peCount` (on the starter, reductions received),
  `completedCheckpoints`, and `rollbackPending` (PE 0).
- Process-wide statics shared by the PEs of a process: `cpCallback` (the
  user's callback), `inCheckpointing`, `inRestarting`. `CkInRestarting()`
  reads `inRestarting`; the location manager, the reduction manager and
  AMPI consult it while elements are being destroyed and restored.
- `CpvAccess(migrationsSinceCheckpoint)`: array elements that left this PE
  since its last checkpoint pack.

The `ckTable` of `CkCheckPTInfo` entries filled by `createEntry` belongs to
the per-element variant (`CMK_CHKP_ALL` 0), which is not compiled; with
`CMK_CHKP_ALL` 1 it only feeds assertions.

### Buddy PE

`ChkptOnPe(pe) = (pe + CmiMyNodeSize()) % CkNumPes()`: the PE of the same
rank in the next process, wrapping around. With one process the buddy is
the PE itself, so both copies are in the same process; PE 0 prints
`CharmFT> one node: a process failure cannot be survived; checkpoint and
rollback work` at the first checkpoint (`doItNow`). `BuddyPE`, the next PE
that has not failed, is used only for the `createEntry` registration and
returns the PE itself when there is one PE.

### Checkpoint flow

1. `CkStartMemCheckpoint(cb)`, on any PE, broadcasts `doItNow(starter, cb)`.
2. `doItNow` on every PE stores `cb` and the starter, sets
   `inCheckpointing`, then calls `startArrayCheckpoint` and `sendProcData`.
3. `startArrayCheckpoint` packs every local element of every location
   manager into one `CkArrayCheckPTMessage` (`pupAllElements`: count, then
   per element its location manager, index, id and
   `pupElementsFor(..., CkElementCreation_migrate)`, the same pup as a
   migration), sends a copy to `ChkptOnPe(me)` (`recvArrayCheckpoint`),
   stores the original in `chkpTable[0]` at the slot being written, and
   resets the migration counter.
4. `recvArrayCheckpoint` stores the buddy's copy (`chkpTable[1]`, or
   `chkpTable[0]` when the copy is the PE's own, as with one process).
   `arrayCopyStored` counts the two copies of the PE; the second, in either
   order, contributes to the `cpFinish` reduction at the starter. The
   buddy's copy can arrive before the PE has run `doItNow`.
5. `sendProcData` packs `_handleProcData` (readonlys, mainchares on PE 0,
   singleton chares, nodegroups on rank 0; groups are not saved) and sends
   it to the buddy; `recvProcData` stores it in `procChkptBuf` and
   contributes to `cpFinish`.
6. `cpFinish` on the starter waits for both reductions, prints the time
   and broadcasts `report`.
7. `report` on every PE clears `inCheckpointing`, counts the checkpoint,
   flips `chkpPointer` (the new checkpoint becomes the last completed one)
   and contributes to `cpDone`, which sends `cb` from the starter. The
   callback therefore runs after every PE has the checkpoint marked
   complete.

### The restart chain

The group entries below restore the elements. Each step ends in a barrier
or reduction over the group, so every PE finishes one step before any PE
starts the next. `diePe` is the failed PE, or -1 for a rollback.

1. `restart(diePe)`: sets `thisFailedPe` and `inRestarting`, marks a failed
   PE unavailable to the load balancer (`resetLB`, skipped when there is
   none), calls `startInserting` on every location manager.
2. `removeArrayElements`: `flushLocalRecs` on every location manager
   destroys every local element. While `CkInRestarting()` is true, the
   destructor path does not tell the home PE (`CkLocMgr::reclaim`).
3. `resetReductionMgr`: `flushStates` and `ckJustMigrated` on every group.
   Reduction managers restart at reduction 0 (`CkReductionMgr::flushStates`,
   `Group::flushStates`); array managers zero their contributor counts
   (`CkArray::flushStates`) and empty their broadcast stores
   (`CkArrayBroadcaster::flushState`).
4. `recoverBuddies`: the PE holding the failed PE's copy sends it to the
   replacement (`recoverEntry`). With no failed PE nothing is sent.
5. `recoverArrayElements`: every PE takes a copy of its own last completed
   checkpoint (`chkpTable[0]`, slot `chkpPointer`) and recreates each
   element (`recoverAll` -> `CkLocMgr::resume(idx, id, p, notify=false,
   rebuild=true)`). `rebuild` resets the element's reduction numbers to 0
   and restamps its broadcast epoch (array-broadcast-migration.md); the
   location manager registers the element as local without telling its
   home PE. Then `doneInserting`.
6. `finishUp`: clears `inRestarting` (here, after the reduction, because
   the flag is shared by the PEs of a process and must stay set until all
   of them have resumed their elements), and the failed PE, or PE 0 for a
   rollback, sends `cpCallback`.

### Rollback

`CkRollbackMemCheckpoint()`, on any PE, returns at once if a rollback or
restart is under way, aborts if a checkpoint is in progress
(`inCheckpointing`) or none has completed, and otherwise sends
`requestRollback` to PE 0. PE 0 accepts one request at a time
(`rollbackPending`, cleared in `finishUp`) and calls `CkStartQD`; at
quiescence it broadcasts `rollback`. Waiting for quiescence replaces the
restart phase of the classic path: nothing is in flight when elements are
destroyed, so no message for a destroyed element can reach its restored
copy. Every PE then checks the restrictions before anything is destroyed
and calls `restart(-1)`.

Differences from a restart after a failure: no PE is marked failed, no
copy moves between PEs, every PE restores its own copy on the PE that took
it, PE 0 delivers the callback, and the process-level data in
`procChkptBuf` is not used (groups, mainchares, readonlys and nodegroups
keep their current state). The stored copy is copied, not consumed, so the
same checkpoint can be rolled back to again.

### The migration restriction

The restore recreates each element on the PE that packed it, and
`resume(..., notify=false)` does not update the home PE or other PEs'
location tables. An element that migrated after the checkpoint is named in
those tables (with a newer epoch, process-location-cache.md) on the PE it
moved to, so after the rollback messages for it would go to a PE that no
longer has it. Reconciling the tables is not done. Instead
`CkLocMgr::emigrate` counts each departure
(`CkMemCheckPTNoteMigration`), the pack in `startArrayCheckpoint` resets
the count, and `rollback` aborts on any PE whose count is nonzero:
`CharmFT> rollback refused: N array elements migrated on PE p since the
last in-memory checkpoint; load balancing between a checkpoint and its
rollback is not supported`.

### Restart after a process failure (classic only)

The dead-process path, compiled only with `__FAULT__`, needed three things
from below Charm++:

- Failure detection: the classic `netlrts` layer noticed a closed socket
  and called `notify_crash_fn`; the MPI layer had buddies ping each other
  (`pingBuddy`, `pingCheckHandler`).
- A replacement process: `charmrun` started one with
  `+restartaftercrash <phase>`, which ran `CkMemRestart` instead of the
  mainchares; the MPI layer kept spare ranks (`+wp`).
- Discarding stale messages: a restart phase in every message header, which
  `notifyHandler` advanced and the charm handlers checked
  (`_discard_charm_message`).

Reconverse has none of them: processes are started by `lcrun` or the batch
launcher, which neither replaces a dead process nor reports one, LCI does
not detect failures, and the message header has no phase field. A process
failure therefore ends the job. Failure injection (`+killFile`) is parsed
only with `__FAULT__`.

### Where in the source

- src/ck-core/ckmemcheckpoint.C: `CkStartMemCheckpoint`, `doItNow`,
  `startArrayCheckpoint`, `arrayCopyStored`, `recvArrayCheckpoint`,
  `sendProcData`, `recvProcData`, `cpFinish`, `report`, `cpDone`;
  `CkRollbackMemCheckpoint`, `requestRollback`, `rollback`,
  `CkMemCheckPTNoteMigration`; the chain `restart`, `removeArrayElements`,
  `resetReductionMgr`, `recoverBuddies`, `recoverArrayElements`,
  `recoverAll`, `finishUp`; `ChkptOnPe`, `BuddyPE`.
- src/ck-core/ckmemcheckpoint.h: `CkCheckPTEntry`, `CkMemCheckPT`.
- src/ck-core/cklocation.C: `CkLocMgr::emigrate` (migration count),
  `CkLocMgr::resume` and `pupElementsFor` with `rebuild`,
  `CkLocMgr::reclaim` (no home notice while restarting).
- src/ck-core/ckarray.C: `CkArray::flushStates`,
  `CkArrayBroadcaster::flushState`, `CkArray::restampBroadcast`.
- src/ck-core/ckreduction.C: `CkReductionMgr::flushStates`, the
  `CkInRestarting` checks in `contributorDied` and `contributorArriving`.
- src/arch/common/conv-mach-syncft.{h,sh}, CMakeLists.txt, buildcmake: the
  build option.
- tests/charm++/memckpt_rollback: checkpoint, rollback (once and with
  `-rollbacks N`), and the migration refusal (`testrestriction`).
