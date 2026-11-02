/**
 * \addtogroup CkLdb
*/
/*@{*/

/**
 * ShedLB: threshold refinement that minimises MIGRATED BYTES.
 *
 * The objective is not "balance" and not "few moves" but the one that
 * actually costs time when objects carry device state: move as few bytes as
 * possible subject to no bin exceeding (1 + eps) times the mean load.
 *
 * Every bin above the threshold sheds down to it and nothing else is touched.
 * A bin picks what to shed locally, taking objects in descending load per
 * byte, so it clears its excess for the fewest bytes. The shed objects form a
 * pool, which is placed on bins under the threshold by best fit on load.
 *
 * Two properties follow, and they are why this exists alongside
 * GreedyRefineCentralLB:
 *
 *  - Any algorithm that brings every bin under T must move at least the total
 *    excess above T, because that load has nowhere else to live. This one
 *    moves at most that excess plus, in the worst case, one object per
 *    overloaded bin, since the last object shed can overshoot. So it is
 *    optimal on migrated volume up to a granularity term that does not grow
 *    with the number of objects. A repacking strategy has no such bound: it
 *    re-derives the whole assignment and its move count is whatever the
 *    packing order happens to produce.
 *
 *  - The shed decision is local to each overloaded bin. Only the pool needs a
 *    global view, and the pool is bounded by the excess, not by the object
 *    count. That is what makes the algorithm a candidate for a distributed
 *    implementation later; this one is central, so it does not yet realise
 *    that, but the structure is kept explicit so it can be lifted.
 *
 * Under CUDA a bin is a GPU (the set of available PEs sharing a
 * gpu_device_id) and the balanced quantity is device time, with host time
 * deciding only which PE inside the group runs an object. Otherwise a bin is
 * a PE and the quantity is host time.
 *
 * eps is +LBShedEps, default 0.10 (LBManager.h says why). It cannot usefully
 * go below the largest object's share of the mean: with indivisible objects
 * no assignment beats the largest one.
 *
 * For objects the application has declared replicable (LDObjData::
 * replicaGroup, CkMigratable::setLBReplica) that limit is lifted: before
 * shedding, every group whose total load L_g exceeds T is asked for
 * k = ceil(L_g / T) copies (up to its maximum), each carrying L_g / k, and a
 * group whose load would fit in one fewer copy with margin gives one back.
 * A copy costs the object's bytes at its destination like a move does, and
 * copies of one group never share a bin. The decisions leave as LDStats::
 * clones / retires and reach the object as ckClone / ckRetire.
*/

#ifndef _SHED_LB_H_
#define _SHED_LB_H_

#include "CentralLB.h"
#include "ShedLB.decl.h"

void CreateShedLB();
BaseLB* AllocateShedLB();

class ShedLB : public CBase_ShedLB
{
public:
  ShedLB(const CkLBOptions&);
  ShedLB(CkMigrateMessage* m);
  void work(LDStats* stats);

private:
  bool QueryBalanceNow(int step) { return true; }

  // One balancing bin: a GPU under CUDA, otherwise a PE.
  struct Bin
  {
    double load = 0.0;         // what is being balanced
    std::vector<int> pes;      // PEs that belong to it
  };

  // An object considered for shedding.
  struct Cand
  {
    int idx;                   // index into stats->objData
    double load;
    double bytes;              // what one migration of it stages
    int bin;                   // the bin it is on now
  };
};

#endif /* _SHED_LB_H_ */

/*@}*/
