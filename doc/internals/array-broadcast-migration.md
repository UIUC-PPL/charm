# Array broadcasts and migrating elements

A broadcast to a chare array is one message per PE, delivered by that PE's
branch of the array manager (`CkArray::recvBroadcast`, src/ck-core/ckarray.C)
to every element that is local when the message is processed. Elements move
between PEs, by `migrateMe` or at a load balancing step, while broadcasts are
in flight. The `CkArrayBroadcaster` listener (ckarray.C, ckarray.h) makes
every element receive every broadcast exactly once regardless.

## Epochs

PE 0 serializes the array's broadcasts (`CkArray::sendBroadcast`): each gets
the next epoch number before it is sent to all PEs. Each element carries the
epoch of the next broadcast it expects, in the broadcaster's listener data
(`bcastNo`, pupped with the element). A new element is stamped with the
highest epoch its PE has seen plus one (`ckElementStamp`), so it receives
only broadcasts sent after its creation.

## Delivery

On arrival a broadcast is stored in the PE's `storedBcasts` queue, indexed by
epoch (`ingestIncoming`), then offered to every local element
(`attemptDelivery`). An element whose expected epoch is lower has missed an
earlier broadcast and is skipped for now; one whose epoch is higher has
already received this broadcast elsewhere and is skipped for good. An element
that takes the broadcast then also takes every stored broadcast that follows
in sequence (`bringUpToDate`).

When an element arrives on a PE, after its pup, the broadcaster runs the same
catch-up (`ckElementArriving`). This covers both orders in which a broadcast
and a migration can meet: a broadcast that reached the destination PE before
the element is in the store and is delivered on arrival; one that reached the
source PE before the element left was delivered there, and its epoch tells
the destination to skip it.

An element whose pup used `pup_buffer` and arrived from another process is
in the local element table before its zero-copy reads complete, and its
`ckJustMigrated` runs only when they have (`CkLocMgr::immigrateWithDevice`,
`processAfterActiveRgetsCompleted`). While the reads are pending the element
is listed in the location manager's `bufferedActiveRgetMsgs`, where ordinary
messages to it wait. `attemptDelivery` skips such an element without
advancing its epoch, so the broadcast stays in the store and the catch-up on
`ckJustMigrated` delivers it.

The store (`BcastQueue`, ckarray.h) keeps one slot per epoch from its head
upward. A slot records whether its broadcast has arrived (seen), the message
while the store still owns it, and whether the message is held by a zero-copy
transfer. Freeing always starts at the head and stops at the first epoch not
seen yet, because a broadcast still on its way may be older than a stored one
(the next section says why), and at a held slot. Spring cleaning
(`CkArrayBroadcaster::springCleaning`, on the array's periodic timer) frees
up to the newest epoch seen at the previous pass under that rule, so a
broadcast is kept for one to two minutes after it arrived.

A checkpoint saves the head and the newest epoch seen, not the messages
(`BcastQueue::pup`). On restart the head starts past the newest epoch seen,
because the epochs below it do not arrive again and a head waiting for one of
them would never move, so every later broadcast would stay stored. The
elements restored on the PE expect that same epoch or a later one.

An in-memory rollback (memckpt.md) restores elements in the same processes,
whose array managers have kept counting. After the rollback's quiescence
every stored broadcast has reached every local element, so
`CkArrayBroadcaster::flushState` empties the store and moves its head past
the newest epoch seen, keeping that epoch (`BcastQueue::clear`), and each
restored element is restamped to expect the next one
(`CkArray::restampBroadcast`, called from `CkLocMgr::pupElementsFor` with
`rebuild`). The epoch saved in the element's checkpoint is discarded: the
broadcasts it would ask for were delivered before the rollback and are not
sent again.

A zero-copy broadcast (`CMK_ZC_BCAST_RECV_MSG`) is delivered first to one
local element, which posts its buffers; the transfer then runs on the same
message, the other local elements receive it as `CMK_ZC_BCAST_RECV_DONE_MSG`
(`CkArray::forwardZCMsgToOtherElems`), and the first element receives it once
more as `CMK_ZC_BCAST_RECV_ALL_DONE_MSG`, which frees it. The store holds the
slot from the first delivery (`holdBcast`) and marks it consumed at the last
(`consumeBcast`): the message is gone, but the epoch counts as seen, so the
head can move past it. (Reconverse completes a zero-copy broadcast's transfer
on the sending side and forwards the payload inline as
`CMK_ZC_SEND_DONE_MSG`, so the receive path above is not taken on it; the
rule is kept for layers that forward the broadcast down a spanning tree.)

## Order

The epochs also order broadcasts. A PE processes broadcasts in whatever order
the machine layer delivers them, and on reconverse two broadcasts from one PE
can arrive in either order: a group broadcast travels within a process as one
node-level message, which whichever PE is free takes from the process's node
queue and fans out to the PEs, so two broadcasts taken by two PEs are fanned
out concurrently. The element that expects epoch n is skipped for epoch n+1,
which waits in the store, and takes both in sequence when n arrives. Every
element therefore receives the array's broadcasts in one order, the order in
which PE 0 stamped them, in both modes below.

That consistency is the whole guarantee. A broadcast issued on another PE
reaches PE 0 as a point-to-point message to the array manager's branch
(`sendBroadcast`), and two such messages can be reordered on the way like
any other pair of messages, so two broadcasts issued by one PE may be stamped
in either order. The reduction results checked by tests/charm++/
atsync_reduction `-o` are issued on PE 0, where the reductions complete, so
they are stamped in completion order; that is a property of the test, not a
promise.

## Arrays with stable locations

`CkArray::stableLocations` is set when both hold at array creation:

- the array was created with static insertion (`CkArrayOptions::
  setStaticInsertion(true)`, the default for a `ckNew` with a nonzero
  initial size);
- anytime migration is off (`CkArrayOptions::setAnytimeMigration(false)`, or
  `+noAnytimeMigration` on the command line, which makes that the default).

These flags are the program's promise that no broadcast to the array is in
flight while an element moves. Elements of such an array may still migrate at
a load balancing step, so the promise covers that step: every broadcast,
including the result of a reduction whose callback broadcasts to the array,
must have been delivered everywhere before the elements call `AtSync`. A
broadcast that reaches the source PE after an element left and the destination
before it arrived is lost to that element; in the other order the element
receives it twice.

Under the promise no element will ever need a broadcast that every element on
its PE has received, so such an array keeps no spring cleaning timer and
instead frees the stored broadcasts after every delivery path: at the end of
`recvBroadcast`, after an arriving or newly created element has caught up,
and after the zero-copy forwarding (`CkArrayBroadcaster::releaseDelivered`).
It frees every stored broadcast older than the lowest expected epoch among
the PE's elements, under the same stopping rule as spring cleaning. A
broadcast is then held only while an earlier one is still on its way, and the
ordering above holds. When the last local element to receive a plain
broadcast is the one the store would free right afterwards, it takes the
original message instead of a copy (`isFinalRecipient`), so an array with one
element per PE receives broadcasts without a copy, as it did before
broadcasts were stored for it. Everything else, storing, the epoch check and
the catch-up, is the same code in both modes.

If the promise is broken, an element arrives expecting an epoch below the
head of the store on its new PE; it could never catch up and would receive
no further broadcast. `bringUpToDate` aborts in that case and names the
cause.

In the tree, TCharm, AMPI and CkIO sessions create their arrays with these
flags. AMPI keeps the promise by refusing `AMPI_Migrate` with a non-blocking
collective outstanding (`checkNoCollectiveOutstanding`, ampi.C), because the
results of `MPI_Iallreduce`, `MPI_Ibarrier` and the others are broadcasts to
the ampi array. With `+ampiMigrateWithCollectives`, or
`+ampiEnableMigrateToPe` which implies it, AMPI creates its arrays with
anytime migration on and allows collectives to be outstanding across a
migration, at the memory cost of broadcasts kept until spring cleaning.

## Where in the source

- src/ck-core/ckarray.C: `CkArray::CkArray` (sets `stableLocations`),
  `CkArray::sendBroadcast`, `CkArray::recvBroadcast`,
  `CkArrayBroadcaster::ingestIncoming`, `attemptDelivery`,
  `bringUpToDate`, `releaseDelivered`, `springCleaning`.
- src/ck-core/ckarray.h: `BcastQueue` (`releaseBelow`, `hold`, `consume`, `pup`, `clear`).
- src/ck-core/ckarray.C: `CkArrayBroadcaster::flushState`, `CkArray::restampBroadcast`
  (in-memory rollback).
- src/ck-core/ckarray.h: `CkArrayBroadcaster` (`ckElementStamp`,
  `ckElementArriving`), `CkArray::stableLocations`.
- src/ck-core/init.C: `+noAnytimeMigration`.
- src/libs/ck-libs/ampi/ampi.C: `isMigrateWithCollsEnabled`,
  `checkNoCollectiveOutstanding`, the `setAnytimeMigration` calls.
- tests/charm++/bcast_zcpup: broadcasts while elements with `pup_buffer`
  data migrate to another process; the handler checks that `ckJustMigrated`
  ran first.
- tests/charm++/atsync_reduction: reductions in flight across an AtSync
  balancing step; with `-s -d`, an array with stable locations keeping the
  promise; `-o` checks the delivery order; `-z` starts each step with a
  zero-copy broadcast.
- contrib/reconverse/README.md, "Messages to one destination can be
  reordered".
