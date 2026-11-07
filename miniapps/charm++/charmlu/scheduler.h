/**
 * BlockScheduler
 *
 * The block scheduler maintains a list of the blocks assigned to its
 * processor, and tracks what step they have reached. Within the bounds of the
 * memory threshold, it requests blocks from remote processors that are needed
 * for local triangular solves and trailing updates. To eliminate the
 * possibility of deadlock, the order in which operations are executed, and
 * hence remote blocks requested, must be carefully selected.
 *
 * Instead for forcing updates to execute in step-order, the order that blocks
 * can execute is selected based on their dependencies on other blocks. The
 * modeling of these dependencies and the demonstration that this leads to a
 * deadlock-free order is shown in a technical report, "Exploring Partial
 * Synchrony in an Asynchronous Environment Using Dense LU".
 */

#ifndef LU_SCHEDULER_H
#define LU_SCHEDULER_H

#include "messages.h"
#include "luConfig.h"
#include "lu.decl.h"
#include <list>
#include <map>
#include <utility>
#include <algorithm>
#include <utility>

class Panel;

/**
 * The current state of a block that the BlockScheduler is tracking. Depending
 * on its state (whether the dependencies have been fulfilled) it will either
 * be planned by the scheduler or it must wait for other blocks to be planned
 * before it.
 */
struct BlockState {
  // Index of block
  int ix, iy;
  // Which panels this block depends on for the next update are not yet planned? [0..3]?
  std::list<Panel*> pendingDependencies;
  // Count of trailing updates completed and planned
  int updatesCompleted, updatesPlanned;
  // Have pivots been completed for this step?
  bool pivotsDone;

  BlockState(CkIndex2D index)
    : ix(index.x), iy(index.y), pendingDependencies(0),
      updatesCompleted(0), updatesPlanned(0),
      pivotsDone(false)
  { }

  bool operator==(const BlockState &rhs) {
    return ix == rhs.ix && iy == rhs.iy;
  }
};

typedef std::list<BlockState> StateList;

/**
 * Holds all the pertinent state for a trailing update. Note that L and U will
 * be shared between various Update structures
 */
struct Update {
  // The block that this update is targeting
  BlockState *target;
  // Whether this trailing update has been triggered or not (it enqueues a
  // message locally so it may not execute immediately)
  bool triggered;
  // The step number for this trailing update
  int t;
  // The L and U data pointers
  double *L, *U;

  // Try to deliver the data to this update, using the src X and Y coordinate
  // to determine whether it's a L or U block
  void tryDeliver(int srcx, int srcy, double *data) {
    if (srcx == target->ix && srcy == t)
      L = data;
    else if (srcy == target->iy && srcx == t)
      U = data;
  }

  // If this condition holds, this is a triangular solve
  bool isComputeU() { return target->ix == t; }
  // The Update is ready if L has arrived and it's a triangular solve or L and
  // U have arrived
  bool ready() { return L && (U || isComputeU()) && !triggered; }

  Update(BlockState *target_, int step)
    : target(target_), triggered(false), t(step), L(NULL), U(NULL) { }

  bool operator==(const Update &rhs) {
    return target == rhs.target && t == rhs.t && L == rhs.L && U == rhs.U;
  }
};

/**
 * A panel encapsulates a list of blocks that have the property of an equal
 * number of updates left to plan, before they are released
 */
struct Panel {
  std::list<StateList::iterator> dependents;
  int updatesLeftToPlan;

  void addDependent(StateList::iterator block) { dependents.push_back(block); }
  Panel() : updatesLeftToPlan(0) { }
};

/**
 * A ComputeU is a triangular solve for a block on a certain iteration
 */
struct ComputeU {
  int x, y, t;
  ComputeU(int x_, int y_, int t_) :
    x(x_), y(y_), t(t_) {}
};

// Estimate of the memory size in bytes of using SDAG, due to the various
// structures that it utilizes for managing dependencies. This is used to
// calculate an upper bound on number of updates that can concurrently be
// planned.
const int sdagOverheadPerBlock = 3760;

/**
 * The BlockScheduler that tracks the dependencies of blocks, requests blocks,
 * and thereby manages memory consumption on a particular processor.
 */
class BlockScheduler : public CBase_BlockScheduler {
public:
  BlockScheduler(CProxy_LUBlk luArr_, LUConfig config, CProxy_LUMgr mgr_);
  BlockScheduler(CkMigrateMessage *m) { }

  // All LUBlk chares that live on the same processor, must register with the
  // BlockScheduler at startup
  void registerBlock(CkIndex2D index);
  // Reduction to indicate that all chares have registered
  void allRegistered(CkReductionMsg *m);
  // Method for printing the calculated block limit, given a memory limit
  void printBlockLimit();
  // Indicate to the scheduler that an update is complete, allowing it to
  // decrease a reference count on the L and U blocks
  void updateDone(intptr_t update_ptr);
  // Indicate to the scheduler that factorization is complete on a block
  void factorizationDone(CkIndex2D index);
  // Invoked from a multicast of a block to deliver L or U data for an Update
  void deliverBlock(blkMsg *m);
  // Indicate that a block has begun active panel work
  void startedActivePanel();
  // Should the schedule execute trailing updates? If a block is executing
  // active panel work, trailing updates are turned off to reduce interference
  bool shouldExecute();
  // Schedule a block to be sent out. Do not sent out immediately so requests
  // can be agglomerated
  void scheduleSend(blkMsg *m, bool onActive);
  void scheduleSend(CkIndex2D index, bool onActive);
  // If a trailing update was enqueued in the Charm++ local queue, but the
  // processor began active panel work and thereby stopped the triggering of
  // updates, this method updates the count that the scheduler keeps track of
  // for pending triggered blocks
  void updateUntriggered();
  // Pump out multicast messages. Use the data and propagate them to the next
  // recipient
  void pumpMessages();
  // If an active column is finished, release dependencies for it
  void releaseActiveColumn(const int y, const int t);
  // Output statistics on maximum memory used, etc.
  void outputStats();

private:
  LUMgr *mgr;
  StateList localBlocks, doneBlocks;

  std::map<int, Panel> panels, Upanels;
  std::list<Update> plannedUpdates;
  CProxy_LUBlk luArr;
  // The memory-enforced limit on the number of blocks this processor can hold
  int blockLimit;
  // Booleans to prevent re-entrancy bugs
  bool inProgress, inPumpMessages;
  // The number of blocks that are in the active panel
  int numActive;
  // The number of blocks that have yet to be triggered
  int pendingTriggered;
  // Should this block scheduler reverse the order of multicast message sends
  // (this is an ordering optimization)
  bool reverseSends;

  // Sends of blocks that are scheduled and ones that are in flight (since a
  // send is asynchronous, they stay here until it is safe to re-send)
  std::list<blkMsg *> scheduledSends, sendsInFlight;

  // A block of data that is currently wanted by the scheduler. Includes a list
  // of pointers to Update objects for reference-counting
  struct wantedBlock {
    std::list<Update *> refs;
    blkMsg* m;
    double *data;
    bool isSending;
    wantedBlock() : m(NULL), data(NULL), isSending(false) {}
  };

  // Actually send the multicast to the next set of recipients
  void propagateBlkMsg(blkMsg *msg);
  // Invariant: All of these blocks are allocated and requested
  std::map<std::pair<int, int>, wantedBlock> wantedBlocks;
  // Drop a reference to a certain block because it has been consumed
  void dropRef(int srcx, int srcy, Update *update);

  // Blocks that are wanted locally and hence do not count against the memory
  // threshold or block limit
  std::map<std::pair<int, int>, std::list<Update*> > localWantedBlocks;

  // Plan an update that has its dependencies fulfilled
  void planUpdate(StateList::iterator target);
  // Make progress by planning updates, triggering updates, and pumping
  // messages out
  void progress();
  // Request a block locally or remotely and update the wanted list
  void getBlock(int srcx, int srcy, double *&data, Update *update);
  // Potentially pick something from the ready list to trigger an update
  void runUpdate(std::list<Update>::iterator update);

  // Update a panel's state based on a dependency being fulfilled
  template <typename K>
  void updatePanel(std::map<K, Panel> &panels, K index);
  // Add a new dependence for a panel for the next step
  template <typename K>
  void addDependence(std::map<K, Panel> &panels, K index, StateList::iterator block);

  // Memory usage instrumentation
  // Memory occupied before factorization starts (kilobytes)
  int baseMemory;
  // Maximum memory usage seen (kilobytes)
  int maxMemory, maxMemoryIncreases, maxMemoryStep;
};

#endif
