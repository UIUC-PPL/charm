#ifndef REFINE_H
#define REFINE_H

#include "greedy.h"
#include <limits>
#include <unordered_map>
#include <vector>

namespace TreeStrategy
{
inline float reldiff(float a, float b) { return std::max(a, b) / std::min(a, b); }

template <typename O, typename P>
struct RefineSolution
{
  RefineSolution(size_t num_objs) : obj_to_pe(num_objs, -1) {}
  inline void assign(const O& o, P& p)
  {
    p.assign(o);
    CkAssert(o.id >= 0 && o.id < obj_to_pe.size());
    obj_to_pe[o.id] = p.id;
  }
  std::vector<int> obj_to_pe;
};

template <typename O, typename P, typename S>
class RefineA : public Strategy<O, P, S>
{
 private:
  std::mt19937 rng;

 public:
  RefineA()
  {
    std::random_device rd;
    rng = std::mt19937(rd());
  }

  void solve(std::vector<O>& objs, std::vector<P>& procs, S& solution, bool objsSorted)
  {
    float M = calcGreedyMaxload(objs, procs, objsSorted);
    if (CkMyPe() == 0 && _lb_args.debug() > 0)
      CkPrintf("[%d] RefineA: greedy maxload is %f\n", CkMyPe(), M);

    std::uniform_int_distribution<int> uni(0, procs.size() - 1);

    std::vector<int> procMap(CkNumPes(), -1);  // real pe -> idx in procs
    for (int i = 0; i < procs.size(); i++) procMap[procs[i].id] = i;
    std::unordered_map<int, std::vector<O>> proc_objs0;  // real pe -> list of its objects
    RefineSolution<O, P> initialAssignment(objs.size());
    for (const auto& o : objs)
    {
      if (procMap[o.oldPe] < 0)
      {  // if object is foreign, initially assign to a random processor
        P& p = procs[uni(rng)];
        initialAssignment.assign(o, p);
        proc_objs0[p.id].push_back(o);
      }
      else
      {
        initialAssignment.assign(o, procs[procMap[o.oldPe]]);
        proc_objs0[o.oldPe].push_back(o);
      }
    }
    for (const auto& p : procs)
      std::sort(proc_objs0[p.id].begin(), proc_objs0[p.id].end(), CmpLoadGreater<O>());

    std::vector<RefineSolution<O, P>> solutions;
    float lower = M;
    float upper = lower * 1.5;
    float bestSolution = std::numeric_limits<float>::max();
    size_t bestSolutionIdx = -1;
    while (reldiff(lower, upper) > 1.01)
    {
      M = (lower + upper) / 2;

      solutions.emplace_back(initialAssignment);
      std::unordered_map<int, std::vector<O>> proc_objs(
          proc_objs0);  // real pe -> list of its objects
      std::vector<P> light_processors;
      std::priority_queue<P, std::vector<P>, CmpLoadLess<P>> heavy_processors;
      float light_maxload = 0;
      for (auto& p : procs)
      {
        if (p.getLoad() > M)
        {
          heavy_processors.push(p);
        }
        else
        {
          light_processors.push_back(p);
          light_maxload = std::max(light_maxload, p.getLoad());
        }
      }
      ProcHeap<P> lightH(light_processors);

      while (heavy_processors.size() > 0 && light_processors.size() > 0)
      {
        // select heaviest obj from heavy that fits in one of the light_processors
        P heavy = heavy_processors.top();
        std::vector<O>& heavy_objs = proc_objs[heavy.id];
        bool objFound = false;
        P& lightest = lightH.top();
        for (auto it = heavy_objs.begin(); it != heavy_objs.end(); it++)
        {
          O& o = *it;
          if (lightest.getLoad() + o.getLoad() <= M)
          {
            heavy_processors.pop();
            heavy.unassign(o);
            for (auto& light : light_processors)
            {
              if (light.getLoad() + o.getLoad() <= M)
              {
                solutions.back().assign(o, light);
                lightH.remove(light);
                lightH.push(light);
                light_maxload = std::max(light_maxload, light.getLoad());
                break;
              }
            }
            heavy_objs.erase(it);
            if (heavy.getLoad() > M)
            {
              heavy_processors.push(heavy);
            }
            else
            {
              light_processors.push_back(heavy);
              light_maxload = std::max(light_maxload, heavy.getLoad());
            }
            objFound = true;
            break;
          }
        }

        if (!objFound) break;
      }

      float cur_maxload;
      if (heavy_processors.size() > 0)
        cur_maxload = heavy_processors.top().getLoad();
      else
        cur_maxload = light_maxload;
      if (cur_maxload < bestSolution)
      {
        bestSolution = cur_maxload;
        bestSolutionIdx = solutions.size() - 1;
      }
      if (CkMyPe() == 0 && _lb_args.debug() > 1)
        CkPrintf("M=%f maxload=%f\n", M, cur_maxload);
      if (cur_maxload / M < 1.01)
        upper = M;
      else
        lower = M;
    }

    for (const auto& o : objs)
    {
      int dest = solutions[bestSolutionIdx].obj_to_pe[o.id];
      solution.assign(o, procs[procMap[dest]]);
    }
  }
};

template <typename O, typename P, typename S>
class RefineB : public Strategy<O, P, S>
{
 private:
  std::mt19937 rng;

 public:
  RefineB()
  {
    std::random_device rd;
    rng = std::mt19937(rd());
  }

  void solve(std::vector<O>& objs, std::vector<P>& procs, S& solution, bool objsSorted)
  {
    float M = calcGreedyMaxload(objs, procs, objsSorted);
    if (CkMyPe() == 0 && _lb_args.debug() > 0)
      CkPrintf("[%d] RefineB: greedy maxload is %f\n", CkMyPe(), M);

    std::uniform_int_distribution<int> uni(0, procs.size() - 1);

    std::vector<int> procMap(CkNumPes(), -1);  // real pe -> idx in procs
    for (int i = 0; i < procs.size(); i++) procMap[procs[i].id] = i;
    std::unordered_map<int, std::vector<O>> proc_objs0;  // real pe -> list of its objects
    RefineSolution<O, P> initialAssignment(objs.size());
    for (const auto& o : objs)
    {
      if (procMap[o.oldPe] < 0)
      {  // if object is foreign, initially assign to a random processor
        P& p = procs[uni(rng)];
        initialAssignment.assign(o, p);
        proc_objs0[p.id].push_back(o);
      }
      else
      {
        initialAssignment.assign(o, procs[procMap[o.oldPe]]);
        proc_objs0[o.oldPe].push_back(o);
      }
    }
    for (const auto& p : procs)
      std::sort(proc_objs0[p.id].begin(), proc_objs0[p.id].end(), CmpLoadGreater<O>());

    std::vector<RefineSolution<O, P>> solutions;
    float lower = M;
    float upper = lower * 1.5;
    float bestSolution = std::numeric_limits<float>::max();
    size_t bestSolutionIdx = -1;
    while (reldiff(lower, upper) > 1.01)
    {
      M = (lower + upper) / 2;

      solutions.emplace_back(initialAssignment);
      std::unordered_map<int, std::vector<O>> proc_objs(
          proc_objs0);  // real pe -> list of its objects
      std::priority_queue<P, std::vector<P>, CmpLoadGreater<P>> light_processors;
      std::priority_queue<P, std::vector<P>, CmpLoadLess<P>> heavy_processors;
      float light_maxload = 0;
      for (auto& p : procs)
      {
        if (p.getLoad() > M)
          heavy_processors.push(p);
        else
        {
          light_processors.push(p);
          light_maxload = std::max(light_maxload, p.getLoad());
        }
      }

      while (heavy_processors.size() > 0 && light_processors.size() > 0)
      {
        // select heaviest obj from heavy that fits in lightest processor
        P heavy = heavy_processors.top();
        P light = light_processors.top();
        heavy_processors.pop();
        light_processors.pop();
        bool objFound = false;
        std::vector<O>& heavy_objs = proc_objs[heavy.id];
        for (auto i = heavy_objs.begin(); i != heavy_objs.end(); i++)
        {
          O& o = *i;
          if (light.getLoad() + o.getLoad() <= M)
          {
            heavy.unassign(o);
            solutions.back().assign(o, light);
            light_maxload = std::max(light_maxload, light.getLoad());
            heavy_objs.erase(i);
            objFound = true;
            break;
          }
        }
        if (heavy.getLoad() > M)
        {
          heavy_processors.push(heavy);
        }
        else
        {
          light_processors.push(heavy);
          light_maxload = std::max(light_maxload, heavy.getLoad());
        }
        if (!objFound) break;
        if (light.getLoad() <= M) light_processors.push(light);
      }

      float cur_maxload;
      if (heavy_processors.size() > 0)
        cur_maxload = heavy_processors.top().getLoad();
      else
        cur_maxload = light_maxload;
      if (cur_maxload < bestSolution)
      {
        bestSolution = cur_maxload;
        bestSolutionIdx = solutions.size() - 1;
      }
      if (CkMyPe() == 0 && _lb_args.debug() > 1)
        CkPrintf("M=%f maxload=%f\n", M, cur_maxload);
      if (cur_maxload /  M < 1.01)
        upper = M;
      else
        lower = M;
    }

    for (const auto& o : objs)
    {
      int dest = solutions[bestSolutionIdx].obj_to_pe[o.id];
      solution.assign(o, procs[procMap[dest]]);
    }
  }
};

}  // namespace TreeStrategy

#endif /* REFINE_H */
