# Replication as a load-balancing decision

A placement balancer moves objects between processors; it cannot bring the
maximum load below the heaviest object. This chapter describes the extension
that lets a balancer change how many copies of an object exist: the
declaration an object makes, the two decision kinds a strategy can emit
beside its moves, how they reach the object, how the memory contract
accounts for them, and what ShedLB does with them. The reasons and the
measurements are in the pull request that introduced it and in the MoE
example's README (`examples/charm++/cuda/gpudirect/moe`).

The division of responsibility is the one migration already has. The
application decides *whether* an object type can be replicated and *how*
the copies are made and kept consistent, the way it decides whether an
object is migratable and provides its `pup`. The balancer decides *when*,
*which* object, *how many* copies and *where*, jointly with its moves. An
object that never declares itself replicable is never replicated, and no
other application is affected.

## The declaration

`LDObjData` (`src/ck-ldb/lbdb.h`) carries seven fields per object:
`replicaGroup`, `replicaRank`, `replicaCount`, `replicaMax`,
`replicaExtraBytes`, `replicaCostNode`, `replicaCostRemote`. `replicaGroup
< 0` means not replicable, which the `LBObj` constructor sets. Objects
sharing a group are the copies of one logical object; `replicaRank` is the
copy's index, `replicaCount` how many copies exist, `replicaMax` how many
the application can keep consistent, `replicaExtraBytes` what one copy
holds on its device beyond the object's own footprint (the buffers the
copies exchange), and the two costs what one more copy adds to each copy's
load, per balancing interval, for its exchange with that one peer on the
same node or on another node (zero when the copies exchange nothing, as
when serving). The fields are pupped with the stats (format version 8, the
costs 9) and so appear in every `LDStats::objData` entry a strategy reads.

An element declares them with `CkMigratable::setLBReplica(group, rank,
count, maxCount, extraBytes, costNode, costRemote)` (`src/ck-core/cklocation.C`), through
`CkLocRec::setObjReplica` and `LBManager::SetObjReplica` into its `LBObj`.
The values are also members of `CkMigratable`, pupped with the element and
declared again to the database of the PE it arrives on, since the `LBObj`
is per PE. The application calls it before every `AtSync`, as it does
`setObjGPUTime`.

## Decision kinds

A decision record, `MigrateInfo` (`src/ck-ldb/BaseLB.h`) and its scatter
form `MigrateDecision`, carries `kind` (`LBDecisionKind`: move, clone,
retire) and `rank`. A clone asks the object on `from_pe` to make a copy of
itself on `to_pe` with replica rank `rank`; a retire asks the copy on
`from_pe` to go away. Every producer of records sets the kind; `MigrateInfo`
constructs as a move, and the one producer that fills a varsize message's
records in place (`DistBaseLB::BroadcastSingleLocationUpdate`) sets it
explicitly.

A central strategy does not build records; it writes `stats->to_proc` and
`CentralLB::createMigrateMsg` derives the moves. Replication decisions have
their own storage beside it: `LDStats::clones` and `LDStats::retires`, each
an `LBAction {obj, fromPe, toPe, rank}` with `obj` the stats index of the
copy addressed (the source of a clone, the copy a retire removes).
`createMigrateMsg` appends them after the moves. The batch planner, which
rebuilds one message per batch from `to_proc`, puts them all in the last
batch: they stage nothing, and the application executes them once the step
has resumed.

Everything that counts arrivals skips them: the tallies of
`InitiateScatter`, the `migrates_expected` counting in
`ProcessReceiveMigration` and `ProcessMigrationDecision`,
`getPredictedLoadWithMsg`. A copy made through array insertion never calls
`LBManager::Migrated`, so counting it would hang the step.

## Delivery

On the PE named by `from_pe`, `ProcessReceiveMigration` (broadcast) or
`ProcessMigrationDecision` (scatter) calls
`CentralLB::executeReplicaDecision`, which calls `LBManager::Clone` or
`LBManager::Retire`. `LBDatabase::Clone/Retire` check that the handle's slot
still holds the object with the handle's id (slots are reused when objects
die; `Migrate` tolerates an empty slot but does not make this check) and
hand the decision to the object manager's callbacks, `LDCallbacks::clone`
and `LDCallbacks::retire` (`lbdb.h`). The callback table is filled at two
sites, `CkLocMgr::initLB` and `IrrGroup::ckRegisterWithLB`; the group
version aborts, since a group branch cannot be replicated. The array version
is `CkLocRec::staticClone/staticRetire`, which call `CkMigratable::ckClone(int
toPe, int rank)` and `ckRetire()` on every element bound to the location.
The defaults abort for an element that declared itself replicable and do
nothing for any other.

The decision is delivered at once, with no deferral: under the split
barrier it can arrive at any point of the element's step. The element
records it and acts at a point of its own choosing. The MoE example acts in
the step the balancer released it in (after `AtSync`, or after `AtSyncWait`
under the split barrier), after that step's weight update, so that by then
every PE's barrier for the round has fired and the round's migrations have
been released: a copy inserted then registers into the next round's epoch
(`cksyncbarrier.C`, `addClient`) and no decision of the current round can
still arrive. This keeps the five places that hold a deferred move behind
`CkLocRec::recvMigrate` unchanged.

Two facts about the runtime an implementation of `ckClone`/`ckRetire`
relies on:

- A copy is a new array element. A 2-D array created as `(n, 1)` has no
  bit in its element id for the second index; it needs
  `CkArrayOptions::setBounds(n, 2)` or `(e, 1)` silently takes another
  element's id. Insertion after creation needs no `beginInserting` /
  `doneInserting`, and calling them mid-run would stall the array's
  reductions and the LB barrier. A message sent to the new element before
  it exists is buffered and delivered after insertion. A freshly inserted
  element can receive the round's `ResumeFromSync` under the unsplit
  barrier; it should ignore a resume it did not ask for.
- Destruction waits for no device work and closes no channel. A retiring
  copy must finish its sends, have its senders drop their channel copies
  (destroy them: an assignment over a `CkDeviceChannel` leaves its
  registration behind), `close()` its own, and then `ckDestroy()`, from an
  entry method other than the one that decided it.

## Memory accounting

A clone is new device state at the destination with no departure anywhere.
`MemoryLedger::feasibleArrival(bytes, pe)` and `commitArrival` (`src/ck-ldb/
LBMemoryContract.h`) debit `footprint + replicaExtraBytes` at the
destination's device; the source's device is debited `replicaExtraBytes`
when the object was alone. `ContractVerifier::verifyAndRepair` takes the
clone list beside the moves, accounts them the same way, ranks them with
the other arrivals by load per byte (a copy carrying the group's load split
one way further), and removes a refused clone from the list. A retire frees
bytes, but the credit is not spent in the same step (the two-phase rule):
the copy is still there when the step's moves execute.

## ShedLB's group pass

`ShedLB::work` (`src/ck-ldb/ShedLB.C`) runs a group pass before shedding.
For every group of replicable objects, with `L_g` the sum of its copies'
loads, `k` their count and `K` the declared maximum: the target is
`k* = min(K, max(1, ceil(L_g / T)))` with `T = (1 + eps) * mean`.

- `k* > k`: one clone record. The source is the copy on the fullest bin.
  The destination is the fullest bin that still clears `T` with the copy's
  share `L_g / k*`, else the emptiest, never a bin the group is already
  on, and only where `feasibleArrival` says the copy fits (and the source's
  device the extra). Every copy is priced as its share `L_g / k*` plus its
  exchanges: for each other copy, the declared `replicaCostNode` or
  `replicaCostRemote` by whether the two sit on the same node (`CkNodeOf`).
  The measured loads leave the exchange out, so the group pass first adds
  each existing copy's exchanges to its bin. The copy is made only if the
  step's critical path, the fullest bin anywhere with those loads, drops by
  more than `eps * mean`. A copy is paid for every step, so a copy that
  relieves a bin other than the fullest, or leaves another bin as full as
  the one it relieves, buys nothing: eight experts on eight GPUs, each 0.7
  to 1.4 times the mean, got two copies that lowered nothing (+29% in bf16
  training, +35% in bf16 serving); on Mixtral-8x7B's real routing (hottest
  expert 1.27 times the mean, the next 1.18) a copy that lowered its own
  bin but not the fullest cost +15% in training and, where the copies
  exchange nothing, gained 10% in serving: the declared cost is what tells
  the two apart. The declined copy is printed with its exchange and the two
  loads. Every copy's load becomes the share; the staged bytes of the
  objective grow by the object's bytes.
- `k > 1`: one retire record for the highest-ranked copy (so an
  application that partitions its work over ranks `0..k-1` contiguously
  keeps a contiguous partition), when the group fits fewer copies,
  `L_g / (k - 1) <= T (1 - eps)`, or when the exchanges cost the critical
  path more than the sharing saves it (the fullest bin without the copy
  and its exchanges is lower by more than `eps * mean`). Rank 0 is never
  retired: it keeps the object's identity for the application. The
  remaining copies' loads become `L_g / (k - 1)` plus their exchanges. The
  margin keeps the two decisions from alternating on a load near `T`.

Clone sources and retiring copies are left out of the shedding that
follows, and a copy is never placed on a bin that holds another copy of its
group. The pass runs before the early return on "already within the
threshold", so a group can shrink when nothing else needs to move. One line
is printed per decision (`CharmLB> ShedLB: replicate ...` / `retire ...`).

## What an application implements

The MoE example's experts are the reference implementation: a 2-D array
`(expert, copy)`; copy `r` serves the dispatchers with `d % 2 == r`; each
copy computes its partition's gradient into its own accumulator, sends it
to the other copy as a zero-copy device message landing in a parity
double-buffer, adds the two in a fixed order and applies the same update,
so the copies' weights stay identical. An unreplicated expert computes the
two partitions separately and adds them the same way, which is what keeps
the checksums identical whether or not the balancer replicated. `ckClone`
inserts `(e, 1)`, sends it the weights behind the step's update on the
device and serves only its partition from the next step; `ckRetire` is
announced in the step's outputs and in the partial, after which the
dispatchers drop their channels and the copy closes and destroys itself.
The test `tests/charm++/load_balancing/replica_test` does the same on the
host with a running sum.

What a copy costs the application is the consistency protocol, and that
sets where the balancer's decision pays. A copy of a training expert
exchanges its gradient partial every step, two weight-sized device messages
each way, and applies the update twice; that is the `replicaExtraBytes` it
declares and, at a shape where the step is short against the exchange, a
copy can cost more than the load it halves (the MoE README's tables of
2026-10-31: a win in fp32, a loss in bf16 at the Mixtral shape). A copy of
a serving expert, forward only, exchanges nothing after its one state send,
declares no extra bytes, and pays off wherever the balancer finds an expert
hotter than a GPU's share. The balancer sees the difference only through
the declaration and the measured load; the policy is unchanged.

## Where in the source

- `src/ck-ldb/lbdb.h`: `LDObjData` replica fields, `LDCallbacks::clone/retire`.
- `src/ck-ldb/LBObj.h`, `LBDatabase.h/.C`, `LBManager.h`, `LBOM.h`: the
  setter chain and `Clone`/`Retire` with the id check.
- `src/ck-ldb/BaseLB.h`: `LBDecisionKind`, `LBAction`, `MigrateInfo::kind`,
  `LDStats::clones/retires`.
- `src/ck-ldb/CentralLB.C`: `createMigrateMsg`, the batch planner's last
  batch, `executeReplicaDecision`, the skips in the arrival tallies.
- `src/ck-ldb/LBMemoryContract.h`: `feasibleArrival`, the verifier's clones.
- `src/ck-ldb/ShedLB.C`: the group pass.
- `src/ck-ldb/LBSimulation.C`: decision printing.
- `src/ck-core/ckmigratable.h`, `cklocation.C`, `cklocrec.h`, `ck.C`:
  `setLBReplica`, `ckClone`/`ckRetire`, the two callback registrations.

## A round that decides nothing

An application that holds work back around a balancing round for the sake
of migrations (the MoE example sends no tokens ahead across the round's
window, so a migration finds no later step's slab landed or in flight) pays
for that even when the round moves nothing. Every PE learns how many
decisions the round made, job-wide, before its `MigrationDone` callbacks
run: `CentralLB` records the migrate message's count (`LBMigrateMsg::n_moves`
on the broadcast path; `LBScatterMsg::totalDecisions`, carried down the
scatter tree, on the scatter path) in `LBManager::SetLastRoundDecisions`,
and `LBManager::LastRoundDecisions()` returns it (-1 before the first round).
Clones and retires are in the count, since they are entries of the same
list. The example's dispatcher registers an `AddMigrationDoneFn` callback
and, on a count of 0, lets the window's remaining steps send ahead
(`Dispatcher::roundDone`, `sendAhead`); the flag is reset at every balancing
step. On OLMoE's routing, where the balancer had nothing to decide, the two
rounds in a 21-step window had cost 10% of the training step.

## Copies beyond two

The two-copy protocol (two partitions by parity, one peer, one exchange)
generalises to `k <= K` (K = 4) as follows, keeping the bits
placement-independent (the example implements this; `-R` up to 4):

- Four fixed dispatcher partitions, `p = d % 4`, whatever `k` is. Copy `r`
  of `k` serves the contiguous block of partitions `[floor(4r/k),
  floor(4(r+1)/k))`: for `k = 2` the blocks are `{0,1}`, `{2,3}`; for `k = 3`
  `{0}`, `{1}`, `{2,3}`; for `k = 4` one each. Every block is a single
  partition or an aligned pair, which is what the tree needs.
- The step's gradient is always the fixed tree `((P0 + P1) + (P2 + P3))`
  over the four partitions' accumulators: each pair add is an explicit
  kernel, the root add is the one inside the SGD kernel. An unreplicated
  expert computes the four leaves itself (three accumulator pairs of lane
  scratch, folding as it goes); a copy computes the leaves of its block,
  folds them to its block sum, sends the block sum to every other copy and
  receives theirs, then assembles the two tree nodes from block sums and its
  own leaves. Since a block is a leaf or an aligned pair, every copy ends
  with the same two node values and applies the same update: the bits of
  `k = 1 .. 4` agree.
- Exchange buffers per copy: two accumulator pairs of its own and
  `(K_app - 1) x 2` landing pairs (per peer slot and step parity), where
  `K_app` is the application's `-R`; `replicaExtraBytes` follows.
- A clone's rank is `k` (the next unused) and a retire takes the highest
  rank (ShedLB does both), so the block mapping only ever splits the last
  block or merges the last two. Every copy must announce the same count for
  the same step, so the decision reaches all of them before the round
  releases any: besides the clone or retire record for the copy concerned,
  ShedLB emits a notice record (`LDStats::cloneNotices` /
  `retireNotices`, kinds `LB_DECISION_CLONE_NOTICE` / `RETIRE_NOTICE`) for
  each other member, delivered by the same path to
  `CkMigratable::ckCloneNotice(toPe, rank)` / `ckRetireNotice(rank)`
  (virtuals that do nothing by default). The notices of a clone the memory
  verifier refused are dropped with it. The copies act at the end of the
  step the round released them in (the split barrier releases every copy
  in the same step), the new copy starts or the retired one ends at the
  next. Dispatchers route by the mapping and wait for the channels of a
  copy they have not sent to before; a copy opens channels for the
  dispatchers it newly serves when a change takes effect (`late` sets, which
  the dispatcher counts as arrivals), and never drops a channel except when
  retiring.
