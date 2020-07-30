#ifndef GREEDY_H
#define GREEDY_H

#include "TreeStrategyBase.h"
#include "pheap.h"

#include <algorithm>
#include <queue>
#include <vector>

namespace TreeStrategy
{
template <typename O, typename P, typename S>
class Greedy : public Strategy<O, P, S>
{
 public:
  void solve(std::vector<O>& objs, std::vector<P>& procs, S& solution, bool objsSorted)
  {
    // Sorts by maxload in vector
    if (!objsSorted) std::sort(objs.begin(), objs.end(), CmpLoadGreater<O>());

    std::vector<ProcHeap<P>> heaps;
    for (int i = 0; i < O::dimension; i++)
    {
      heaps.push_back(ProcHeap<P>(procs, i));
    }

    for (const auto& o : objs)
    {
      int maxdimension = 0;
      CkPrintf("Obj %d, dimension %d\n", o.id, O::dimension);
      for (int i = 0; i < O::dimension; i++)
      {
        if (o.load[i] > o.load[maxdimension])
          maxdimension = i;
        CkPrintf(" %f", o.load[i]);
      }
      CkPrintf("\n");
      P p = heaps[maxdimension].top();
      solution.assign(o, p);  // update solution (assumes solution updates processor load)
      for (int i = 0; i < O::dimension; i++)
      {
        heaps[i].remove(p);
        heaps[i].push(p);
      }
      CkPrintf("Obj %d going to PE %d\n", o.id, p.id);
    }
  }
};

template <typename P, typename S>
class Greedy<Obj<1>, P, S> : public Strategy<Obj<1>, P, S>
{
public:
  void solve(std::vector<Obj<1>>& objs, std::vector<P>& procs, S& solution,
             bool objsSorted)
  {
    if (!objsSorted) std::sort(objs.begin(), objs.end(), CmpLoadGreater<Obj<1>>());
    std::priority_queue<P, std::vector<P>, CmpLoadGreater<P>> procHeap(
        CmpLoadGreater<P>(), procs);

    for (const auto& o : objs)
    {
      P p = procHeap.top();
      procHeap.pop();
      solution.assign(o, p);  // update solution (assumes solution updates processor load)
      procHeap.push(p);
    }
  }
};

template <typename O, typename P>
struct GreedySolution
{
  inline void assign(const O& o, P& p)
  {
    ptr(p)->assign(o);
    maxload = std::max(maxload, ptr(p)->getLoad());
  }
  float maxload = 0;
};

// NOTE: this will modify order of objects in 'objs' if objsSorted is false
template <typename O, typename P>
float calcGreedyMaxload(std::vector<O>& objs, std::vector<P>& procs, bool objsSorted)
{
  GreedySolution<O, P> greedy_sol;
  Greedy<O, P, GreedySolution<O, P>> greedy;
  greedy.solve(objs, procs, greedy_sol, objsSorted);
  // greedy will modify my copy of processors only if they are passed as pointers
  if (std::is_pointer<P>::value)
    for (auto& p : procs) ptr(p)->resetLoad();
  return greedy_sol.maxload;
}

template <typename O, typename P, typename S>
class GreedyRefine : public Strategy<O, P, S>
{
 public:
  GreedyRefine(json& config)
  {
    const auto& option = config.find("tolerance");
    if (option != config.end())
    {
      tolerance = *option;
    }
  }

  void solve(std::vector<O>& objs, std::vector<P>& procs, S& solution, bool objsSorted)
  {
    float M = calcGreedyMaxload(objs, procs, objsSorted);
    if (CkMyPe() == 0 && _lb_args.debug() > 0)
      CkPrintf("[%d] GreedyRefine: greedy maxload is %f, tolerance set to %f\n", CkMyPe(),
               M, tolerance);

    M *= tolerance;

    // need custom heap that allows removal of elements from any position
    ProcHeap<P> procHeap(procs);
    P p;
    for (const auto& o : objs)
    {
      // TODO improve the case where the proc is not in my list of processors (because
      // it belongs to a foreing domain). procHeap API should return an error?
      P& oldPe = procHeap.getProc(ptr(o)->oldPe);
      if ((oldPe.id >= 0) && (oldPe.getLoad() + ptr(o)->getLoad() <= M))
        p = oldPe;
      else
        p = procHeap.top();
      procHeap.remove(p);
      solution.assign(o, p);
      procHeap.push(p);
      M = std::max(M, ptr(p)->getLoad());
    }
  }

 private:
  float tolerance = 1;  // tolerance above greedy maxload (not average load!)
};

}  // namespace TreeStrategy

#endif /* GREEDY_H */
