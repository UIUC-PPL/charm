/**
 * BlockScheduler
 *
 * Plan on a set of trailing updates up to the block (U or L) limit, unless the
 * block is local or shared by another trailing update. A block is eligible to
 * be planned if its necessary dependencies on other blocks have been
 * fulfilled, by that block having been previously planned. To simplify the
 * dependencies the block are split into regions (called panels in the code) to
 * signify that they have the same number of pending dependencies because they
 * are in the same region. This is explained further in a technical report,
 * "Exploring Partial Synchrony in an Asynchronous Environment Using Dense LU".
 */

#include "scheduler.h"
#include "lu.decl.h"
#include "messages.h"
#include "lu.h"
#include <algorithm>
using std::min;
using std::find;
#include <utility>
using std::pair;
using std::make_pair;
#include "register.h"
using std::list;
using std::map;

// The limit on the number of concurrent outgoing sends
#ifndef SEND_LIM
  #error Please define some value for the macro SEND_LIM appropriate to the machine you are running on
#endif
static const int SEND_LIMIT = SEND_LIM;

inline bool operator==(const CkIndex2D &l, const CkIndex2D &r) {
  return l.x == r.x && l.y == r.y;
}
pair<int, int> make_pair(CkIndex2D index) {
  return make_pair(index.x, index.y);
}

BlockScheduler::BlockScheduler(CProxy_LUBlk luArr_, LUConfig config, CProxy_LUMgr mgr_)
  : luArr(luArr_), mgr(mgr_.ckLocalBranch()), inProgress(false), inPumpMessages(false), numActive(0)
  , pendingTriggered(0), reverseSends(CkMyPe() % 2 == 0)
  , maxMemory(0), maxMemoryIncreases(0), maxMemoryStep(-1) {
  // Calculate the block limit based on the memory threshold
  blockLimit = config.memThreshold * 1024 * 1024 /
    (config.blockSize * (config.blockSize + 1) * sizeof(double) + sizeof(LUBlk) + sdagOverheadPerBlock);
  contribute(CkCallback(CkIndex_LUBlk::schedulerReady(NULL), luArr));
}

void BlockScheduler::scheduleSend(blkMsg *msg, bool onActive) {
  list<blkMsg *>::iterator iter = find(scheduledSends.begin(), scheduledSends.end(), msg);
  if (iter == scheduledSends.end()) {
    scheduledSends.push_back(msg);
  }
  pumpMessages();
}

void BlockScheduler::releaseActiveColumn(const int y, const int t) {
  for (StateList::iterator iter = localBlocks.begin();
       iter != localBlocks.end(); ++iter) {
    if (iter->iy == y && t >= iter->updatesPlanned &&
        iter->pendingDependencies.size() > 0) {
      for (list<Panel*>::iterator depPanels = iter->pendingDependencies.begin();
           depPanels != iter->pendingDependencies.end(); ++depPanels) {
        Panel &panel = **depPanels;
        panel.dependents.remove(iter);
      }
      iter->pendingDependencies.clear();
    }
  }
}

void BlockScheduler::scheduleSend(CkIndex2D index, bool onActive) {
  blkMsg *msg = luArr(index).ckLocal()->LUmsg;
  scheduleSend(msg, onActive);
}

// If the processor is idle, have Converse call this function to pump message
// out to ensure progress is made
void pumpOnIdle(void *s) {
  BlockScheduler *scheduler = (BlockScheduler *)s;
  scheduler->pumpMessages();
}

void BlockScheduler::pumpMessages() {
  if (inPumpMessages) return;
  inPumpMessages = true;

  size_t curMemory = CmiMemoryUsage()/1024;
  if (curMemory > maxMemory) {
    maxMemory = curMemory;
    maxMemoryIncreases++;
    if (plannedUpdates.size() > 0)
      maxMemoryStep = plannedUpdates.front().t;
  }

  for (list<blkMsg*>::iterator iter = sendsInFlight.begin();
       iter != sendsInFlight.end(); ++iter) {
    blkMsg *msg = *iter;
    int ref = REFFIELD(UsrToEnv(msg));
    CkAssert(ref > 0 && ref <= 2);
    if (ref == 1) {
      if (!msg->firstHalfSent) {
        msg->firstHalfSent = true;
        propagateBlkMsg(msg);
      } else {
        map<pair<int, int>, wantedBlock>::iterator blockIter =
          wantedBlocks.find(make_pair(msg->src));
        if (blockIter != wantedBlocks.end()) {
          wantedBlock &block = blockIter->second;
          CkAssert(block.isSending);
          block.isSending = false;
          if (block.refs.size() == 0)  {
            delete msg;
            wantedBlocks.erase(blockIter);
            progress();
          }
        } else {
          // If local, needs to be set back to false, so resetMessage gets called
          msg->firstHalfSent = false;
        }
        iter = sendsInFlight.erase(iter);
      }
    }
  }

  for (list<blkMsg*>::iterator iter = scheduledSends.begin();
       iter != scheduledSends.end() && sendsInFlight.size() < SEND_LIMIT;
       ++iter) {
    if (find(sendsInFlight.begin(), sendsInFlight.end(), *iter) ==
        sendsInFlight.end()) {
      sendsInFlight.push_back(*iter);
      propagateBlkMsg(*iter);
      iter = scheduledSends.erase(iter);
    }
  }

  if (sendsInFlight.size() != 0 || scheduledSends.size() != 0) {
    CcdCallOnCondition(CcdPROCESSOR_STILL_IDLE, pumpOnIdle, this);
  }

  inPumpMessages = false;
}

void BlockScheduler::printBlockLimit() {
  CkPrintf("%d: block limit = %d\n", CkMyPe(), blockLimit);
}

// When a LUBlk registers, initialize the dependencies based on the presence of
// this block
void BlockScheduler::registerBlock(CkIndex2D index) {
  blockLimit--;

  if (index.y != 0) {
    localBlocks.push_back(BlockState(index));
    if (index.x != 0) {
      for (int i = 1; i < min(index.x, index.y) + 1; i++)
        Upanels[i].updatesLeftToPlan++;
    }
    if (index.y > index.x)
      panels[index.x].updatesLeftToPlan++;
  }

  if (blockLimit < 2)
    CkAbort("Too little space to plan even one trailing update");
}

void printMemory(void *time, void *msg) {
  int *s = (int*) ((CkReductionMsg *)msg)->getData();
  int mem = s[0];
  CkPrintf("%s memory usage: %zu KiB, additional stats: ", time, mem);
  for (int i = 1; i < ((CkReductionMsg*)msg)->getLength() / sizeof(int); i++)
    CkPrintf("%zd ", s[i]);
  CkPrintf("\n");
  delete (CkReductionMsg*)msg;
}

void BlockScheduler::outputStats() {
  int stats[3];
  stats[0] = maxMemory;
  stats[1] = maxMemoryIncreases;
  stats[2] = maxMemoryStep;
  contribute(3 * sizeof(int), stats, CkReduction::max_int,
	     CkCallback(&printMemory, const_cast<char*>("Peak")));
}

void BlockScheduler::allRegistered(CkReductionMsg *m) {
  delete m;

  baseMemory = CmiMemoryUsage()/1024;
  contribute(sizeof(int), &baseMemory, CkReduction::max_int,
	     CkCallback(&printMemory, const_cast<char*>("Base")));

  // Add dependence for the first iteration if this is needed
  for (StateList::iterator iter = localBlocks.begin();
       iter != localBlocks.end(); ++iter)
    if (iter->ix != 0)
      addDependence(panels, 0, iter);

  progress();
}

void BlockScheduler::startedActivePanel() {
  numActive++;
}

template <typename K>
void BlockScheduler::updatePanel(map<K, Panel> &panels, K index) {
  typename map<K, Panel>::iterator iter = panels.find(index);
  CkAssert(iter != panels.end());
  Panel &panel = iter->second;

  panel.updatesLeftToPlan--;
  if(panel.updatesLeftToPlan == 0) {
    for (list<StateList::iterator>::iterator i = panel.dependents.begin();
         i != panel.dependents.end(); ++i) {
      (*i)->pendingDependencies.remove(&panel);
    }

    panels.erase(iter);
  }
}

template <typename K>
void BlockScheduler::addDependence(map<K, Panel> &panels, K index,
				   StateList::iterator block) {
  typename map<K, Panel>::iterator panel = panels.find(index);
  if (panel != panels.end()) {
    block->pendingDependencies.push_back(&panel->second);
    panel->second.addDependent(block);
  }
}

void BlockScheduler::planUpdate(StateList::iterator target) {
  CkAssert(target->pendingDependencies.size() == 0);

  int t = target->updatesPlanned++;

  plannedUpdates.push_back(Update(&*target, t));
  Update &update = plannedUpdates.back();

  if (update.isComputeU()) {
    CkAssert(t == min(target->ix, target->iy));

    if (target->iy == target->ix + 1)
      plannedUpdates.pop_back();
    else
      getBlock(target->ix, target->ix, update.L, &update);

    updatePanel(panels, t);

    doneBlocks.splice(doneBlocks.end(), localBlocks, target);
    return;
  }

  getBlock(target->ix, t, update.L, &update);
  getBlock(t, target->iy, update.U, &update);

  if (target->updatesPlanned < min(target->ix, target->iy)) {
    addDependence(panels, t+1, target);
    addDependence(Upanels, t+1, target);
  } else if (target->iy > target->ix) {
    CkAssert(target->updatesPlanned == min(target->ix, target->iy));
    addDependence(Upanels, t+1, target);
  } else
    doneBlocks.splice(doneBlocks.end(), localBlocks, target);

  updatePanel(Upanels, t+1);
}

void BlockScheduler::getBlock(int srcx, int srcy, double *&data,
			      Update *update) {
  LUBlk *local = luArr(srcx, srcy).ckLocal();
  if (local) {
    if (local->factored)
      data = local->accessLocalBlock();
    else
      localWantedBlocks[make_pair(srcx, srcy)].push_back(update);
    return;
  }

  pair<int, int> src = make_pair(srcx, srcy);
  wantedBlock &block = wantedBlocks[src];

  block.refs.push_back(update);

  if (block.refs.size() == 1 && !block.isSending) {
    // First reference to this block, so ask for it
    CkEntryOptions opts;
    luArr(src.first, src.second).requestBlock(CkMyPe(), update->target->ix, update->target->iy,
                                              &(mgr->setPrio(GET_BLOCK, opts)));
  }

  if (block.m)
    update->tryDeliver(srcx, srcy, block.data);
}

void BlockScheduler::deliverBlock(blkMsg *m) {
  CkAssert(wantedBlocks.find(make_pair(m->src)) != wantedBlocks.end());
  wantedBlock &block = wantedBlocks[make_pair(m->src)];
  block.m = m;
  block.data = m->data;

  for (list<Update*>::iterator update = block.refs.begin();
       update != block.refs.end(); ++update)
    (*update)->tryDeliver(m->src.x, m->src.y, m->data);

  CkAssert(m->npes_receiver >= 1);
  CkAssert(CkMyPe() == m->pes[m->offset]);

  // This processor is no longer part of the set
  m->offset++;
  m->npes_receiver--;
  m->firstHalfSent = false;
  if (m->npes_receiver >= 1) {
    scheduleSend(m, false);
    block.isSending = true;
  }

  progress();
}

void BlockScheduler::propagateBlkMsg(blkMsg *m) {
  envelope *env = UsrToEnv(m);
  _SET_USED(env, 0);
  if (env->isPacked()) {
    unsigned char msgidx = env->getMsgIdx();
    if(_msgTable[msgidx]->unpack) {
      m = (blkMsg*)_msgTable[msgidx]->unpack(m);
      UsrToEnv(m)->setPacked(0);
    }
  }

  if (!m->firstHalfSent) {
    LUBlk *block = luArr[m->src].ckLocal();
    if (block != NULL) {
      block->resetMessage(reverseSends);
      reverseSends = !reverseSends;
    }
    m->npes_sender = m->npes_receiver;
    m->npes_receiver = (m->npes_receiver+1) / 2;
  } else {
    int secondHalf = m->npes_sender - m->npes_receiver;
    CkAssert(secondHalf >= 0);
    m->offset += m->npes_receiver;
    m->npes_receiver = secondHalf;
  }

  if (m->npes_receiver >= 1) {
    takeRef(m);
    thisProxy[m->pes[m->offset]].deliverBlock(m);
  }
}

void BlockScheduler::dropRef(int srcx, int srcy, Update *update) {
  map<pair<int, int>, wantedBlock>::iterator input =
    wantedBlocks.find(make_pair(srcx, srcy));

  // Ignore local blocks used as inputs
  if (input == wantedBlocks.end())
    return;

  wantedBlock &block = input->second;
  block.refs.remove(update);
  if (block.refs.size() == 0 && !block.isSending) {
    delete block.m;
    wantedBlocks.erase(input);
  }
}

void BlockScheduler::runUpdate(list<Update>::iterator iter) {
  Update &update = *iter;
  CkAssert(update.ready());
  int tx = update.target->ix, ty = update.target->iy;
  update.triggered = true;
  pendingTriggered++;

  CkEntryOptions opts;
  intptr_t update_ptr = (intptr_t)&update;

  if (update.isComputeU()) {
   luArr(tx, ty).processComputeU(update_ptr,
                                 &(mgr->setPrio(PROCESS_COMPUTE_U, opts)));
  } else {
    int t = update.t;
    luArr(tx, ty).processTrailingUpdate(t, update_ptr,
                                        &(mgr->setPrio(PROCESS_TRAILING_UPDATE,
                                                       opts, ty, tx, t)));
  }
}

void BlockScheduler::updateDone(intptr_t update_ptr) {
  Update &update = *(Update *)update_ptr;
  int tx = update.target->ix, ty = update.target->iy;

  dropRef(tx, update.t, &update);
  if (!update.isComputeU())
    dropRef(update.t, ty, &update);

  update.target->updatesCompleted++;
  pendingTriggered--;
  CkAssert(pendingTriggered >= 0);

  plannedUpdates.erase(find(plannedUpdates.begin(), plannedUpdates.end(), update));

  progress();
}

void BlockScheduler::updateUntriggered() {
  pendingTriggered--;
  if (pendingTriggered == 0) {
    pumpMessages();
  }
}

bool BlockScheduler::shouldExecute() {
  return numActive == 0;
}

void BlockScheduler::factorizationDone(CkIndex2D index) {
  if (index.x >= index.y)
    numActive--;

  map<pair<int, int>, list<Update*> >::iterator wanters =
    localWantedBlocks.find(make_pair(index.x, index.y));
  if (wanters != localWantedBlocks.end()) {
    list<Update*> &wantList = wanters->second;
    CkAssert(luArr(index).ckLocal());

    for (list<Update*>::iterator wanter = wantList.begin();
	 wanter != wantList.end(); ++wanter)
      (*wanter)->tryDeliver(index.x, index.y, luArr(index).ckLocal()->accessLocalBlock());

    localWantedBlocks.erase(wanters);
  }

  progress();
}

bool eligibilityYOrder(const BlockState& block1, const BlockState& block2) {
  if (block1.pendingDependencies.size() != block2.pendingDependencies.size())
    return block1.pendingDependencies.size() < block2.pendingDependencies.size();
  else
    return block1.iy < block2.iy;
}

void BlockScheduler::progress() {
  // Prevent re-entrance
  if (inProgress) return;
  inProgress = true;

  bool stateModified;

  do {
    stateModified = false;
    bool plannedAnything = true;
    while (wantedBlocks.size() < blockLimit && plannedAnything) {
      plannedAnything = false;
      if (localBlocks.size() > 0) {
        localBlocks.sort(eligibilityYOrder);

	CkAssert(localBlocks.front().pendingDependencies.size() == 0);
	planUpdate(localBlocks.begin());
	plannedAnything = true;
      }
    }

    if (numActive == 0) {
      // Start triangular solves and trailing updates
      for (list<Update>::iterator update = plannedUpdates.begin();
	   update != plannedUpdates.end(); ++update) {
	if (update->ready()) {
          runUpdate(update);
          stateModified = true;
          break;
        }
      }
    }
  } while (stateModified);

  pumpMessages();
  inProgress = false;
}

