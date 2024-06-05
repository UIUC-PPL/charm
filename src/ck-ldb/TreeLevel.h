// Author: Juan Galvez <jjgalvez@illinois.edu>

#ifndef TREELEVEL_H
#define TREELEVEL_H

#include "TopoManager.h"
#include "TreeLB.h"
#include "TreeStrategyBase.h"
#include "charm.h"

#include <algorithm>
#include <cmath>
#include <limits>  // std::numeric_limits
#include <vector>

#define FLOAT_TO_INT_MULT 10000

// ----------------------- msgs -----------------------

#include "TreeLevel.decl.h"

class LLBMigrateMsg : public TreeLBMessage, public CMessage_LLBMigrateMsg
{
 public:
  // NOTE: currently this message allocates space for all PEs, even if it doesn't include
  // info for all of them
  int n_moves;        // number of moves
  int* num_incoming;  // pe -> number of objects incoming
  int* obj_start;     // pe -> idx to first object in to_pes
  int* to_pes;        // obj -> to_pe
};

class LBStatsMsg_1 : public TreeLBMessage, public CMessage_LBStatsMsg_1
{
 public:
  unsigned int nObjs;  // num objs in this msg
  unsigned int nPes;  // num pes in this msg
  size_t posDimension; // dimension of entries in positions

  int* pe_ids;              // IDs of the pes in this msg
  float* bgloads;           // bgloads[i] is background load of i-th pe in this msg
  float* speeds;            // speeds[i] is speed of i-th pe
  unsigned int* obj_start;  // obj_start[i] points to where loads of objects of i-th pe
                            // start in this msg (array oloads)

  float* oloads;  // array of obj loads (grouped by pe), i-th obj in the array is
                  // considered to have ID i
  float* positions; // array of object positions, if needed; indexing is same as oloads

  static TreeLBMessage* merge(std::vector<TreeLBMessage*>& msgs)
  {
    // TODO ideally have option of sorting objects

    bool rateAware = false;
    LBStatsMsg_1* mm = (LBStatsMsg_1*)msgs[0];
    if ((void*)mm->speeds != (void*)mm->obj_start) rateAware = true;

    // could pass n and m as parameters to this method, but don't think it would really
    // matter
    unsigned int nObjs = 0;
    unsigned int nPes = 0;
    size_t minPosDimension = std::numeric_limits<size_t>::max();
    size_t maxPosDimension = std::numeric_limits<size_t>::min();

    for (int i = 0; i < msgs.size(); i++)
    {
      LBStatsMsg_1* msg = (LBStatsMsg_1*)msgs[i];
      nObjs += msg->nObjs;
      nPes += msg->nPes;
      minPosDimension = std::min(minPosDimension, msg->posDimension);
      maxPosDimension = std::max(maxPosDimension, msg->posDimension);
    }
    CkAssertMsg(msgs.empty() || minPosDimension == maxPosDimension,
                "Position of every object for LB must be of same dimension");
    const size_t posDimension = minPosDimension;
    LBStatsMsg_1* newMsg;
    if (rateAware)
      newMsg = new (nPes, nPes, nPes, nPes + 1, nObjs, nObjs * posDimension) LBStatsMsg_1;
    else
      newMsg = new (nPes, nPes, 0, nPes + 1, nObjs, nObjs * posDimension) LBStatsMsg_1;
    newMsg->nObjs = nObjs;
    newMsg->nPes = nPes;
    newMsg->posDimension = posDimension;
    int pe_cnt = 0;
    int obj_cnt = 0;
    for (int i = 0; i < msgs.size(); i++)
    {
      LBStatsMsg_1* msg = (LBStatsMsg_1*)msgs[i];
      const int msg_npes = msg->nPes;
      memcpy(newMsg->pe_ids + pe_cnt, msg->pe_ids, sizeof(int) * msg_npes);
      memcpy(newMsg->bgloads + pe_cnt, msg->bgloads, sizeof(float) * msg_npes);
      if (rateAware)
        memcpy(newMsg->speeds + pe_cnt, msg->speeds, sizeof(float) * msg_npes);
      // memcpy(newMsg->obj_start + pe_cnt, msg->obj_start, sizeof(int)*msg_npes);
      for (int j = 0; j < msg_npes; j++)
        newMsg->obj_start[pe_cnt + j] = msg->obj_start[j] + obj_cnt;
      memcpy(newMsg->oloads + obj_cnt, msg->oloads, sizeof(float) * (msg->nObjs));
      memcpy(newMsg->positions + obj_cnt * posDimension, msg->positions,
             sizeof(float) * msg->nObjs * posDimension);
      obj_cnt += msg->nObjs;
      pe_cnt += msg_npes;
    }
    newMsg->obj_start[pe_cnt] = obj_cnt;

    return newMsg;
  }

  template <typename O, typename P>
  static float fill(std::vector<TreeLBMessage*> msgs, std::vector<O>& objs,
                    std::vector<P>& procs, LLBMigrateMsg* migMsg,
                    std::vector<int>& obj_local_ids)
  {
    int pe_cnt = 0;
    int obj_cnt = 0;
    float total_load = 0;
    for (int i = 0; i < msgs.size(); i++)
    {
      LBStatsMsg_1* msg = (LBStatsMsg_1*)msgs[i];
      for (int j = 0; j < msg->nPes; j++)
      {
        int pe = msg->pe_ids[j];
        CkAssert(pe >= 0 && pe < CkNumPes());
        procs[pe_cnt].populate(pe, msg->bgloads + j, msg->speeds + j);
        procs[pe_cnt++].resetLoad();
        migMsg->obj_start[pe] = obj_cnt;
        int local_id = 0;
        for (int k = msg->obj_start[j]; k < msg->obj_start[j + 1];
             k++, obj_cnt++, local_id++)
        {
          objs[obj_cnt].populate(obj_cnt, msg->oloads + k, pe);
          if (msg->posDimension > 0)
            objs[obj_cnt].setPosition(
              std::vector<float>(msg->positions + k * msg->posDimension,
                                 msg->positions + k * msg->posDimension + msg->posDimension));

          total_load += objs[obj_cnt].getLoad();
          migMsg->to_pes[obj_cnt] = pe;
          // if obj_local_ids.size() > 0:
          obj_local_ids[obj_cnt] = local_id;
        }
      }
    }
    CkAssert(obj_cnt == objs.size());
    CkAssert(pe_cnt == procs.size());
    return total_load;
  }
};

class SubtreeLoadMsg : public TreeLBMessage, public CMessage_SubtreeLoadMsg
{
 public:
  int pe;
  float load;
};

class SubtreeMigrateDecisionMsg : public TreeLBMessage,
                                  public CMessage_SubtreeMigrateDecisionMsg
{
 public:
  int num_moves;
  int* src_groups;
  int* dest_groups;
  int* loads;
};

class TokenListMsg : public TreeLBMessage, public CMessage_TokenListMsg
{
 public:
  // int dest;
  int load;
  int num_tokens;
  int* local_ids;
  int* oldPes;
  float* loads;
};

#include "TreeLevel.def.h"

// ----------------------- StrategyWrapper -----------------------

class IStrategyWrapper
{
 public:
  virtual ~IStrategyWrapper() {}

  virtual float prepStrategy(unsigned int nobjs, unsigned int nprocs,
                             std::vector<TreeLBMessage*>& msgs,
                             LLBMigrateMsg* mig_msg) = 0;

  virtual void runStrategy(LLBMigrateMsg* migMsg, IDM* idm = nullptr) = 0;

  virtual void removeObj(int& local_id, int& oldPe, float& load) = 0;

  virtual void addForeignObject(int local_id, int oldPe, float load) = 0;
};

// This wrapper allocates mem for objects and processors. to the lb algorithm,
// it passes vectors of objects and processors
template <typename O, typename P>
class StrategyWrapper : public IStrategyWrapper
{
 public:
  // TODO make a separate Solution class to deal with IDM scenario?
  class Solution
  {
   public:
    Solution(int& nmoves, int* num_incoming, int* loc, int foreign_obj_id_start,
             std::vector<int>& obj_local_ids)
        : n_moves(nmoves),
          num_incoming(num_incoming),
          loc(loc),
          obj_local_ids(obj_local_ids),
          foreign_obj_id_start(foreign_obj_id_start)
    {
    }

    inline void assign(const O* o, P* p)
    {
#if DEBUG__TREE_LB_L3
      CkPrintf("[%d] Moving object %d from processor %d to %d foreign_obj_id_start=%d\n",
               CkMyPe(), o->id, o->oldPe, p->id, foreign_obj_id_start);
#endif
#if CMK_ERROR_CHECKING
      CkAssert(p->id >= 0 && p->id < CkNumPes());
      CkAssert(procMap[p->id] >= 0);
      CkAssert(o->id >= 0 && o->id < num_objs);
      CkAssert(o->oldPe >= 0 && o->oldPe < CkNumPes());
      CkAssert(objs_assigned.count(o->id) ==
               0);  // check that object hasn't been assigned already
#endif
      p->assign(o);
      if (o->oldPe != p->id)
      {
        n_moves += 1;
        num_incoming[p->id] += 1;
      }
      if (o->id < foreign_obj_id_start)
      {
        // this object is in my subtree
        loc[o->id] = p->id;
      }
      else
      {
        (*idm)[o->oldPe].emplace_back(obj_local_ids[o->id], p->id);
      }
    }

    inline void assign(const O& o, P& p) { assign(&o, &p); }

    void setIDM(IDM* idm) { this->idm = idm; }

#if CMK_ERROR_CHECKING
    void setErrorChecking(std::vector<O>& objs, std::vector<P>& procs)
    {
      num_objs = objs.size();
      procMap.resize(CkNumPes(), -1);
      for (int i = 0; i < procs.size(); i++) procMap[procs[i].id] = i;
    }
#endif

   private:
    friend StrategyWrapper;
    int& n_moves;
    int* num_incoming;
    int* loc;  // store solution of strategy here: loc[i] = newPe for object i
    std::vector<int>& obj_local_ids;
    const int foreign_obj_id_start;
    IDM* idm = nullptr;
#if CMK_ERROR_CHECKING
    size_t num_objs;
    std::vector<int> procMap;
    std::unordered_set<unsigned int> objs_assigned;
#endif
  };

  StrategyWrapper(TreeStrategy::Strategy<O, P, Solution>* _strategy, const std::string& _strategy_name, bool _isTreeRoot, json& config)
  {
    strategy_name = _strategy_name;
    isTreeRoot = _isTreeRoot;
    strategy = _strategy;
  }

  virtual ~StrategyWrapper()
  {
    delete strategy;
    delete sol;
  }

  float prepStrategy(unsigned int nobjs, unsigned int nprocs,
                     std::vector<TreeLBMessage*>& msgs, LLBMigrateMsg* migMsg)
  {
    CkAssert(foreign_objs.size() == 0 && sol == nullptr);
    objs.resize(nobjs);
    procs.resize(nprocs);
    // if (subtree_migrations)
    obj_local_ids.resize(nobjs);
    foreign_obj_id = nobjs;
    sol = new Solution(migMsg->n_moves, migMsg->num_incoming, migMsg->to_pes,
                       foreign_obj_id, obj_local_ids);
    return LBStatsMsg_1::fill(msgs, objs, procs, migMsg, obj_local_ids);
  }

  // remove object because it is moving to a different subtree
  void removeObj(int& local_id, int& oldPe, float& load)
  {
    O& o = objs.back();
    local_id = obj_local_ids[o.id];
    oldPe = o.oldPe;
    load = o.getLoad();
    // indicating that the object will migrate but not yet known where
    sol->loc[o.id] = -1;
    objs.pop_back();
  }

  void addForeignObject(int local_id, int oldPe, float load)
  {
    CkAssert(oldPe >= 0 && oldPe < CkNumPes());
    foreign_objs.emplace_back();
    foreign_objs.back().populate(foreign_obj_id++, &load, oldPe);
    obj_local_ids.push_back(local_id);
    CkAssert(obj_local_ids.size() == foreign_obj_id);
  }

  void runStrategy(LLBMigrateMsg* migMsg, IDM* idm = nullptr)
  {
    CkAssert(sol != nullptr);
    sol->setIDM(idm);

    if (foreign_objs.size() > 0)
    {
      objs.insert(objs.end(), foreign_objs.begin(), foreign_objs.end());
      foreign_objs.clear();
    }

    std::vector<int> procMap;
#if CMK_ERROR_CHECKING
    {
#else
    if ((CkMyPe() == 0 || isTreeRoot) && _lb_args.debug() > 0)
    {
#endif
      procMap.resize(CkNumPes(), -1);
      for (int i = 0; i < procs.size(); i++) procMap[procs[i].id] = i;
      if ((CkMyPe() == 0 || isTreeRoot) && _lb_args.debug() > 0)
        CkPrintf("[%d] num_procs=%zu num_objs=%zu\n", CkMyPe(), procs.size(),
                 objs.size());
      if (objs.size() > 0)
      {
        float objMinLoad = std::numeric_limits<float>::max();
        float objMaxLoad = 0;
        float objTotalLoad = 0;
        for (const auto& o : objs)
        {
          float oload = o.getLoad();
          objMinLoad = std::min(objMinLoad, oload);
          objMaxLoad = std::max(objMaxLoad, oload);
          objTotalLoad += oload;
          if (o.id < sol->foreign_obj_id_start)
          {
            CkAssert(procMap[o.oldPe] >= 0);
            procs[procMap[o.oldPe]].assign(o);
          }
        }
#if CMK_ERROR_CHECKING
        if ((CkMyPe() == 0 || isTreeRoot) && _lb_args.debug() > 0)
#endif
          CkPrintf("[%d] obj loads: min=%f mean=%f max=%f\n", CkMyPe(), objMinLoad,
                   objTotalLoad / objs.size(), objMaxLoad);
      }

      float procMinLoad = std::numeric_limits<float>::max();
      float procMaxLoad = 0;
      float procTotalLoad = 0;
      for (const auto& p : procs)
      {
        float pload = p.getLoad();
        procMinLoad = std::min(procMinLoad, pload);
        procMaxLoad = std::max(procMaxLoad, pload);
        procTotalLoad += pload;
      }
#if CMK_ERROR_CHECKING
      if ((CkMyPe() == 0 || isTreeRoot) && _lb_args.debug() > 0)
#endif
        CkPrintf("[%d] proc loads: min=%f mean=%f max=%f\n", CkMyPe(), procMinLoad,
                 procTotalLoad / procs.size(), procMaxLoad);
      if (objs.size() > 0)
        for (auto& p : procs) p.resetLoad();
    }

#if CMK_ERROR_CHECKING
    sol->setErrorChecking(objs, procs);
#endif

    double t0 = CkWallTimer();
    strategy->solve(objs, procs, *sol, false);

#if CMK_ERROR_CHECKING
    {
#else
    if ((CkMyPe() == 0 || isTreeRoot) && _lb_args.debug() > 0)
    {
#endif
      double strategy_time = CkWallTimer() - t0;
      TopoManager* tmgr = TopoManager::getTopoManager();
      float maxLoad = 0;
      unsigned int migrations_sum_hops = 0;
      for (auto& p : procs) p.resetLoad();
      for (const auto& o : objs)
      {
        int dest = -1;
        if (o.id < sol->foreign_obj_id_start)
        {
          dest = migMsg->to_pes[o.id];
        }
        else
        {
          for (auto& idm_move : (*idm)[o.oldPe])
          {
            if (idm_move.first == obj_local_ids[o.id])
            {
              dest = idm_move.second;
              break;
            }
          }
        }
        if (dest == -1)
        {
          if (o.id >= sol->foreign_obj_id_start)
            CkPrintf("[%d] Error: strategy %s might not support foreign objects\n",
                     CkMyPe(), strategy_name.c_str());
          CkAbort("Object was not assigned to any processor\n");
        }
        if (procMap[dest] < 0) CkAbort("Strategy assigned object to invalid processor\n");
        if (dest != o.oldPe)
          migrations_sum_hops += tmgr->getHopsBetweenRanks(o.oldPe, dest);
        P& p = procs[procMap[dest]];
        p.assign(o);
        maxLoad = std::max(maxLoad, p.getLoad());
      }
#if CMK_ERROR_CHECKING
      if ((CkMyPe() == 0 || isTreeRoot) && _lb_args.debug() > 0)
#endif
      {
        CkPrintf(
            "[%d] strategy %s time=%f secs, maxLoad after strategy=%f, num_migrations=%d "
            "migrations_sum_hops=%u\n",
            CkMyPe(), strategy_name.c_str(), strategy_time, maxLoad, migMsg->n_moves,
            migrations_sum_hops);
      }
    }

    delete sol;
    sol = nullptr;
  }

 private:
  std::string strategy_name;
  bool isTreeRoot;
  std::vector<O> objs;
  std::vector<P> procs;
  Solution* sol = nullptr;
  std::vector<int> obj_local_ids;
  std::vector<O> foreign_objs;
  int foreign_obj_id;
  TreeStrategy::Strategy<O, P, Solution>* strategy;
};

#include "TreeStrategyFactory.h"

template <int N>
IStrategyWrapper* CreateStrategyWrapper(const std::string& strategy_name, bool isTreeRoot,
                                        json& config, bool rateAware)
{
  if (rateAware)
    return TreeStrategy::Factory::CreateStrategyWrapper<N, true>(
      strategy_name, isTreeRoot, config[strategy_name]);
  else
    return TreeStrategy::Factory::CreateStrategyWrapper<N, false>(
      strategy_name, isTreeRoot, config[strategy_name]);
}


// --------------------------------------------------------------

// ---------------- RootLevel ----------------

class RootLevel : public LevelLogic
{
 public:
  RootLevel(int _num_groups = -1) : num_groups(_num_groups) {}

  virtual ~RootLevel()
  {
    for (auto w : wrappers) delete w;
  }

  /**
   * mode 0: receive obj stats
   * mode 1: receive aggregated group load
   */
  virtual void configure(bool rateAware, std::vector<std::string> strategies,
                         json& config, bool repeat_strategies = false,
                         bool token_passing = true)
  {
    using namespace TreeStrategy;
    for (auto w : wrappers) delete w;
    wrappers.clear();
    if (num_groups == -1)
    {
      current_strategy = 0;
      for (const std::string& strategy_name : strategies)
      {
        wrappers.push_back(CreateStrategyWrapper<1>(strategy_name, true, config[strategy_name], rateAware));
      }
      this->repeat_strategies = repeat_strategies;
    }
    else
    {
      group_strategy_dummy = !token_passing;
    }
  }

  virtual void depositStats(TreeLBMessage* stats)
  {
    stats_msgs.push_back(stats);
    if (num_groups > 0)
    {
      total_load += ((SubtreeLoadMsg*)stats)->load;
    }
    else
    {
      nPes += ((LBStatsMsg_1*)stats)->nPes;
      nObjs += ((LBStatsMsg_1*)stats)->nObjs;
    }
  }

  TreeLBMessage* loadBalance(IDM& idm)
  {
#if DEBUG__TREE_LB_L1
    // print('[' + str(charm.myPe()) + ']', self.__class__, 'loadBalance')
#endif

    const int num_children = stats_msgs.size();
    CkAssert(num_children > 0);
#if DEBUG__TREE_LB_L1
    CkPrintf("[%d] RootLevel::loadBalance, num_children=%d nPes=%d nObjs=%d\n", CkMyPe(),
             num_children, nPes, nObjs);
#endif

    if (num_groups == -1)
    {
      // msg has object loads
      CkAssert(wrappers.size() > current_strategy);
      IStrategyWrapper* wrapper = wrappers[current_strategy];
      CkAssert(wrapper != nullptr);
      CkAssert(nPes == CkNumPes());
      LLBMigrateMsg* migMsg = new (nPes, nPes, nObjs, 0) LLBMigrateMsg;
      migMsg->n_moves = 0;
      std::fill(migMsg->num_incoming, migMsg->num_incoming + nPes, 0);

#if DEBUG__TREE_LB_L1
      double t0 = CkWallTimer();
#endif
      wrapper->prepStrategy(nObjs, nPes, stats_msgs, migMsg);
      wrapper->runStrategy(migMsg);
      if (current_strategy == wrappers.size() - 1)
      {
        if (repeat_strategies) current_strategy = 0;
      }
      else
      {
        current_strategy++;
      }
#if DEBUG__TREE_LB_L1
      CkPrintf("[%d] RootLevel::loadBalance - strategy took %f secs\n", CkMyPe(),
               CkWallTimer() - t0);
#endif
      // need to cast pointer to ensure delete of CMessage_LBStatsMsg_1 is called
      for (auto msg : stats_msgs) delete (LBStatsMsg_1*)msg;
      stats_msgs.clear();
      nPes = nObjs = 0;
      return migMsg;
    }
    else
    {
      CkAssert(num_groups >= 1);
      CkAssert(wrappers.size() == 0);
      if (_lb_args.debug() > 0)
        CkPrintf("[%d] ROOT: RECEIVED STATS, Total load is %f\n", CkMyPe(), total_load);

      std::vector<GroupMigration> solution;
      if (num_groups > 1 && !group_strategy_dummy)
      {
        std::vector<std::pair<int, float>> underloaded;
        std::vector<std::pair<int, float>> overloaded;
        underloaded.reserve(num_groups);
        overloaded.reserve(num_groups);
        float avg_grp_load = total_load / num_groups;
        float epsilon = avg_grp_load * 0.02;
        for (auto* sm : stats_msgs)
        {
          SubtreeLoadMsg* msg = (SubtreeLoadMsg*)sm;
          int& grp = msg->pe;
          float& load = msg->load;
          //#if DEBUG__TREE_LB_L1
          if (_lb_args.debug() > 1)
            CkPrintf("[%d] PE %d load = %f\n", CkMyPe(), grp, load);
          //#endif
          if (load < avg_grp_load - epsilon)
          {
            underloaded.emplace_back(grp, load);
          }
          else if (load > avg_grp_load + epsilon)
          {
            overloaded.emplace_back(grp, load);
          }
          delete msg;
        }
        stats_msgs.clear();
        size_t underloaded_idx = 0;
        for (auto& ov : overloaded)
        {
          int& g1 = ov.first;
          float& l1 = ov.second;
          while ((l1 - avg_grp_load > epsilon) && (underloaded_idx < underloaded.size()))
          {
            int& g2 = underloaded[underloaded_idx].first;
            float& l2 = underloaded[underloaded_idx].second;
            float transfer = std::min(l1 - avg_grp_load, avg_grp_load - l2);
            solution.emplace_back(g1, g2, roundf(FLOAT_TO_INT_MULT * transfer));
            //#if DEBUG__TREE_LB_L1
            if (_lb_args.debug() > 0)
              CkPrintf("[%d] Root: moving %f load from %d to %d\n", CkMyPe(), transfer,
                       g1, g2);
            //#endif
            l2 += transfer;
            /// underloaded[underloaded_idx].second += transfer;
            if (l2 >= avg_grp_load - epsilon) underloaded_idx += 1;
            l1 -= transfer;
          }
        }
      }
      else
      {
        for (auto* sm : stats_msgs) delete (SubtreeLoadMsg*)sm;
        stats_msgs.clear();
      }

      total_load = 0.0;

      int nmoves = int(solution.size());
      SubtreeMigrateDecisionMsg* migMsg =
          new (nmoves, nmoves, nmoves, 0) SubtreeMigrateDecisionMsg;
      migMsg->num_moves = nmoves;
      for (int i = 0; i < solution.size(); i++)
      {
        auto& mig = solution[i];
        migMsg->src_groups[i] = mig.src_group;
        migMsg->dest_groups[i] = mig.dst_group;
        migMsg->loads[i] = mig.load;
      }
      return migMsg;
    }
  }

 protected:
  struct GroupMigration
  {
    GroupMigration(int src, int dst, float _load)
        : src_group(src), dst_group(dst), load(_load)
    {
    }
    int src_group;
    int dst_group;
    int load;
  };

  int num_groups;
  bool repeat_strategies;
  size_t current_strategy = 0;
  bool group_strategy_dummy = false;  // if true, don't balance load between groups
  unsigned int nPes = 0;  // total number of processors in msgs I am processing
  unsigned int nObjs = 0;  // total number of objects in msgs I am processing
  float total_load = 0;
  std::vector<IStrategyWrapper*> wrappers;
};

// ---------------- NodeSetLevel ----------------

class NodeSetLevel : public LevelLogic
{
 public:
  NodeSetLevel(LBManager* _lbmgr, std::vector<int>& _pes) : lbmgr(_lbmgr), pes(_pes) {}

  virtual ~NodeSetLevel()
  {
    for (auto w : wrappers) delete w;
  }

  virtual void configure(bool rateAware, std::vector<std::string> strategies,
                         json& config, bool repeat_strategies = false,
                         int _cutoff_freq = 1)
  {
    using namespace TreeStrategy;
    for (auto w : wrappers) delete w;
    wrappers.clear();
    current_strategy = 0;
    for (const std::string& strategy_name : strategies)
    {
      wrappers.push_back(CreateStrategyWrapper<1>(strategy_name, false, config, rateAware));
    }
    this->repeat_strategies = repeat_strategies;;
    cutoff_freq = _cutoff_freq;
    CkAssert(cutoff_freq > 0);
  }

  virtual void depositStats(TreeLBMessage* stats)
  {
    stats_msgs.push_back(stats);
    nPes += ((LBStatsMsg_1*)stats)->nPes;
    nObjs += ((LBStatsMsg_1*)stats)->nObjs;
  }

  virtual bool cutoff() { return (lbmgr->step() + 1) % cutoff_freq != 0; }

  virtual TreeLBMessage* mergeStats()
  {
    CkAssert(wrappers.size() > current_strategy);
    IStrategyWrapper* wrapper = wrappers[current_strategy];
    CkAssert(wrapper != nullptr);

    num_children = stats_msgs.size();
    CkAssert(num_children > 0);
#if DEBUG__TREE_LB_L2
    CkPrintf("[%d] NodeSetLevel::mergeStats, num_children=%d nPes=%d nObjs=%d\n",
             CkMyPe(), num_children, nPes, nObjs);
#endif

    CkAssert(migMsg == nullptr);
    int total_npes = CkNumPes();
    migMsg = new (total_npes, total_npes, nObjs, 0) LLBMigrateMsg;
    migMsg->n_moves = 0;
    std::fill(migMsg->num_incoming, migMsg->num_incoming + total_npes, 0);

    float subtree_load = wrapper->prepStrategy(nObjs, nPes, stats_msgs, migMsg);
    // need to cast pointer to ensure delete of CMessage_LBStatsMsg_1 is called
    for (auto msg : stats_msgs) delete (LBStatsMsg_1*)msg;
    stats_msgs.clear();
    nPes = nObjs = 0;

    SubtreeLoadMsg* newMsg = new SubtreeLoadMsg;
    newMsg->pe = CkMyPe();
    newMsg->load = subtree_load;
    return newMsg;
  }

  virtual void processDecision(TreeLBMessage* decision, int& incoming, int& outgoing)
  {
    SubtreeMigrateDecisionMsg* d = (SubtreeMigrateDecisionMsg*)decision;
    incoming = outgoing = 0;
    for (int i = 0; i < d->num_moves; i++)
    {
      int& src_group = d->src_groups[i];
      int& dest_group = d->dest_groups[i];
      int& load = d->loads[i];
      if (src_group == CkMyPe())
        outgoing += load;
      else if (dest_group == CkMyPe())
        incoming += load;
      CkAssert(src_group != dest_group);
    }
#if DEBUG__TREE_LEVELS_L2
    CkPrintf("[%d] NodeSetLevel: incoming=%d outgoing=%d\n", CkMyPe(), incoming,
             outgoing);
#else
    if (CkMyPe() == 0 && _lb_args.debug() > 1)
      CkPrintf("[%d] NodeSetLevel: incoming=%d outgoing=%d\n", CkMyPe(), incoming,
               outgoing);
#endif
  }

  virtual bool makesTokens() { return true; }

  virtual int getTokenSets(TreeLBMessage* transferMsg,
                           std::vector<TreeLBMessage*>& token_sets,
                           std::vector<int>& destinations)
  {
    IStrategyWrapper* wrapper = wrappers[current_strategy];

    // tokens will be list of local object id, obj load, and current PE

    // TODO need a good algorithm to find a subset of objects whose aggregate load
    // closely matches the load that is supposed to be sent to each destination subtree.
    // this is NOT efficient
    SubtreeMigrateDecisionMsg* d = (SubtreeMigrateDecisionMsg*)transferMsg;
    int outgoing_nominal_load = 0;
    std::vector<Token> tokens;
    for (int i = 0; i < d->num_moves; i++)
    {
      int& src = d->src_groups[i];
      if (src == CkMyPe())
      {
        int& dest = d->dest_groups[i];
        float load = float(d->loads[i]) / FLOAT_TO_INT_MULT;
        int nominal_load = d->loads[i];
        outgoing_nominal_load += nominal_load;
        tokens.clear();
#if DEBUG__TREE_LB_L1
        CkPrintf("[%d] NodeSetLevel: I have to transfer %f load to %d\n", CkMyPe(), load,
                 dest);
#endif
        destinations.push_back(dest);
        float transferred = 0;
        int local_id, oldPe;
        float oload;
        while (transferred < load)
        {
          wrapper->removeObj(local_id, oldPe, oload);
          transferred += oload;
          tokens.emplace_back(local_id, oldPe, oload);
#if DEBUG__TREE_LB_L2
          CkPrintf("[%d] Sending obj with local_obj_id=%d oldPe=%d load=%f TO %d\n",
                   CkMyPe(), local_id, oldPe, oload, dest);
#endif
        }
        int ntokens = tokens.size();
        TokenListMsg* token_set_msg = new (ntokens, ntokens, ntokens, 0) TokenListMsg;
        token_set_msg->load = nominal_load;
        token_set_msg->num_tokens = ntokens;
        for (int j = 0; j < ntokens; j++)
        {
          auto& token = tokens[j];
          token_set_msg->local_ids[j] = token.obj_local_id;
          token_set_msg->oldPes[j] = token.oldPe;
          token_set_msg->loads[j] = token.load;
        }
        token_sets.push_back(token_set_msg);
      }
    }
    return outgoing_nominal_load;
  }

  virtual int tokensReceived(TreeLBMessage* msg)
  {
    IStrategyWrapper* wrapper = wrappers[current_strategy];
    TokenListMsg* token_set = (TokenListMsg*)msg;
    for (int i = 0; i < token_set->num_tokens; i++)
    {
#if DEBUG__TREE_LB_L2
      CkPrintf("[%d] Adding object with local_id=%d from oldPe=%d with load %f\n",
               CkMyPe(), token_set->local_ids[i], token_set->oldPes[i],
               token_set->loads[i]);
#endif
      wrapper->addForeignObject(token_set->local_ids[i], token_set->oldPes[i],
                                token_set->loads[i]);
    }
    int load = token_set->load;
#if DEBUG__TREE_LB_L2
    CkPrintf("[%d] Total nominal load in token set is %d\n", CkMyPe(), load);
#endif
    delete token_set;
    return load;
  }

  virtual TreeLBMessage* loadBalance(IDM& idm)
  {
    CkAssert(wrappers.size() > current_strategy);
    IStrategyWrapper* wrapper = wrappers[current_strategy];
    CkAssert(wrapper != nullptr);

    if (cutoff())
    {
      num_children = stats_msgs.size();
      CkAssert(num_children > 0);
#if DEBUG__TREE_LB_L2
      CkPrintf(
          "[%d] NodeSetLevel::loadBalance (w cutoff), num_children=%d nPes=%d nObjs=%d\n",
          CkMyPe(), num_children, nPes, nObjs);
#endif

      CkAssert(migMsg == nullptr);
      int total_npes = CkNumPes();
      migMsg = new (total_npes, total_npes, nObjs, 0) LLBMigrateMsg;
      migMsg->n_moves = 0;
      std::fill(migMsg->num_incoming, migMsg->num_incoming + total_npes, 0);

      wrapper->prepStrategy(nObjs, nPes, stats_msgs, migMsg);
      // need to cast pointer to ensure delete of CMessage_LBStatsMsg_1 is called
      for (auto msg : stats_msgs) delete (LBStatsMsg_1*)msg;
      stats_msgs.clear();
      nPes = nObjs = 0;
    }
    wrapper->runStrategy(migMsg, &idm);
    if (current_strategy == wrappers.size() - 1)
    {
      if (repeat_strategies) current_strategy = 0;
    }
    else
    {
      current_strategy++;
    }
    TreeLBMessage* decision = migMsg;
    migMsg = nullptr;
    return decision;
  }

 protected:
  struct Token
  {
    Token(int obj_local_id, int oldPe, float load)
        : obj_local_id(obj_local_id), oldPe(oldPe), load(load)
    {
    }
    int obj_local_id;
    int oldPe;
    float load;
  };

  LBManager* lbmgr;
  bool repeat_strategies;
  size_t current_strategy = 0;
  std::vector<IStrategyWrapper*> wrappers;
  LLBMigrateMsg* migMsg = nullptr;
  std::vector<int> pes;
  unsigned int num_children = 0;
  unsigned int nPes =
      0;  // total number of processors in msgs I am processing (from my subtree)
  unsigned int nObjs =
      0;  // total number of objects in msgs I am processing (from my subtree)
  int cutoff_freq = 0;
};

// ---------------- NodeLevel ----------------

class NodeLevel : public LevelLogic
{
 public:
  NodeLevel(LBManager* _lbmgr, std::vector<int>& _pes) : lbmgr(_lbmgr), pes(_pes) {}

  virtual ~NodeLevel()
  {
    for (auto w : wrappers) delete w;
  }

  virtual void configure(bool rateAware, std::vector<std::string> strategies,
                         json& config, bool repeat_strategies = false,
                         int _cutoff_freq = 1)
  {
    using namespace TreeStrategy;
    for (auto w : wrappers) delete w;
    wrappers.clear();
    current_strategy = 0;
    for (const std::string& strategy_name : strategies)
    {
      wrappers.push_back(CreateStrategyWrapper<1>(strategy_name, false, config[strategy_name], rateAware));
    }
    this->repeat_strategies = repeat_strategies;
    cutoff_freq = _cutoff_freq;
    CkAssert(cutoff_freq > 0);
  }

  virtual bool cutoff() { return (lbmgr->step() + 1) % cutoff_freq != 0; }

  virtual TreeLBMessage* mergeStats()
  {
    // send obj loads up
    TreeLBMessage* newMsg = LBStatsMsg_1::merge(stats_msgs);
    // need to cast pointer to ensure delete of CMessage_LBStatsMsg_1 is called
    for (auto m : stats_msgs) delete (LBStatsMsg_1*)m;
    stats_msgs.clear();
    return newMsg;
  }

  virtual void processDecision(TreeLBMessage* decision, int& incoming, int& outgoing)
  {
    // will just forward the decision from the root
    CkReferenceMsg(decision); // Add a reference since caller deletes this message
    this->decision = decision;
    incoming = outgoing = 0;
  }

  virtual TreeLBMessage* loadBalance(IDM& idm)
  {
    if (cutoff())
    {
      return withinNodeLoadBalance();
    }
    else
    {
      // just forward decision from root to children
      return decision;
    }
  }

 protected:
  LLBMigrateMsg* withinNodeLoadBalance()
  {
    CkAssert(wrappers.size() > current_strategy);
    IStrategyWrapper* wrapper = wrappers[current_strategy];
    CkAssert(wrapper != nullptr);
    CkAssert(pes.size() > 0);

    unsigned int nObjs = 0;
    unsigned int nPes = 0;
    for (auto msg : stats_msgs)
    {
      nObjs += ((LBStatsMsg_1*)msg)->nObjs;
      nPes += ((LBStatsMsg_1*)msg)->nPes;
    }
    CkAssert(nPes == pes.size());
#if DEBUG__TREE_LB_L1
    if (CkMyPe() == 0)
      CkPrintf("[%d] NodeLevel::withinNodeLoadBalance - nPes=%d nObjs=%d\n", CkMyPe(),
               nPes, nObjs);
#endif

    int total_npes = CkNumPes();
    LLBMigrateMsg* migMsg = new (total_npes, total_npes, nObjs, 0) LLBMigrateMsg;
    migMsg->n_moves = 0;
    std::fill(migMsg->num_incoming, migMsg->num_incoming + total_npes, 0);

#if DEBUG__TREE_LB_L2
    double t0 = CkWallTimer();
#endif
    wrapper->prepStrategy(nObjs, nPes, stats_msgs, migMsg);
    wrapper->runStrategy(migMsg);
#if DEBUG__TREE_LB_L2
    CkPrintf("[%d] NodeLevel::withinNodeLoadBalance - strategy took %f secs\n", CkMyPe(),
             CkWallTimer() - t0);
#endif
    if (current_strategy == wrappers.size() - 1)
    {
      if (repeat_strategies) current_strategy = 0;
    }
    else
    {
      current_strategy++;
    }
    // need to cast pointer to ensure delete of CMessage_LBStatsMsg_1 is called
    for (auto msg : stats_msgs) delete (LBStatsMsg_1*)msg;
    stats_msgs.clear();
    return migMsg;
  }

  LBManager* lbmgr;
  bool repeat_strategies;
  size_t current_strategy = 0;
  std::vector<IStrategyWrapper*> wrappers;
  TreeLBMessage* decision = nullptr;
  std::vector<int> pes;
  int cutoff_freq = 0;
};

// ---------------- PELevel ----------------

class PELevel : public LevelLogic
{
 public:
  struct LDObjLoadGreater
  {
    inline bool operator()(const LDObjData& o1, const LDObjData& o2) const
    {
      return (o1.wallTime > o2.wallTime);
    }
  };

  PELevel(LBManager* _lbmgr) : lbmgr(_lbmgr), rateAware(_lb_args.testPeSpeed()) {}

  virtual ~PELevel() {}

  virtual TreeLBMessage* getStats()
  {
    const int mype = CkMyPe();
    int nobjs = lbmgr->GetObjDataSz();
    std::vector<LDObjData> allLocalObjs(nobjs);
    if (nobjs > 0) lbmgr->GetObjData(allLocalObjs.data());  // populate allLocalObjs
    myObjs.clear();
    LBRealType nonMigratableLoad = 0;
    size_t minPosDimension = std::numeric_limits<size_t>::max();
    size_t maxPosDimension = std::numeric_limits<size_t>::min();
    for (int i = 0; i < nobjs; i++)
    {
      if (allLocalObjs[i].migratable)
      {
        myObjs.emplace_back(allLocalObjs[i]);
        minPosDimension = std::min(minPosDimension, allLocalObjs[i].position.size());
        maxPosDimension = std::max(maxPosDimension, allLocalObjs[i].position.size());
      }
      else
      {
        nonMigratableLoad += allLocalObjs[i].wallTime;
      }
    }
    nobjs = myObjs.size();
    CkAssertMsg(nobjs == 0 || minPosDimension == maxPosDimension,
                "Position of every object for LB must be of same dimension!");
    const size_t posDimension = (nobjs == 0) ? 0 : minPosDimension;
    // TODO verify that non-migratable objects are not added to msg and are only counted
    // as background load

#if DEBUG__TREE_LB_L3
    float total_obj_load = 0;
    for (int i = 0; i < nobjs; i++) total_obj_load += myObjs[i].wallTime;
    CkPrintf("[%d] PELevel::getStats, myObjs=%d, aggregate_obj_load=%f\n", mype,
             int(myObjs.size()), total_obj_load);
#endif

    // TODO sending comm info: only send if needed by an active strategy:
    // this could be tricky for trees with more than 2 levels and  each level is
    // cycling through multiple strategies

    // std::sort(myObjs.begin(), myObjs.end(), PELevel::LDObjLoadGreater());  // sort
    // descending order of load

    // allocate and populate stats msg
    LBStatsMsg_1* msg;
    if (rateAware)
    {
      msg = new (1, 1, 1, 2, nobjs, nobjs * posDimension) LBStatsMsg_1;
      msg->speeds[0] = float(lbmgr->ProcessorSpeed());
    }
    else
      msg = new (1, 1, 0, 2, nobjs, nobjs * posDimension) LBStatsMsg_1;
    msg->nObjs = nobjs;
    msg->nPes = 1;
    msg->posDimension = posDimension;
    msg->pe_ids[0] = mype;
    msg->obj_start[0] = 0;
    msg->obj_start[1] = nobjs;
    for (int i = 0; i < nobjs; i++)
    {
      // If rateAware, convert object loads by multiplying by processor speed
      // Note this conversion isn't done for bgloads because they never leave the PE
      if (rateAware)
        msg->oloads[i] = float(myObjs[i].wallTime) * msg->speeds[0];
      else
        msg->oloads[i] = float(myObjs[i].wallTime);
      if (posDimension > 0)
      {
        for (int j = 0; j < posDimension; j++)
          msg->positions[i * posDimension + j] = myObjs[i].position[j];
      }
    }

    LBRealType t1, t2, t3, bg_walltime;
#if CMK_LB_CPUTIMER
    LBRealType t4;
    lbmgr->GetTime(&t1, &t2, &t3, &bg_walltime, &t4);
#else
    lbmgr->GetTime(&t1, &t2, &t3, &bg_walltime, &bg_walltime);
#endif
    bg_walltime += nonMigratableLoad;
    if (_lb_args.ignoreBgLoad())  // TODO I think the LBDatabase should return
                                  // bg_walltime=0 if ignoreBGLoad=True
      msg->bgloads[0] = 0;
    else
      msg->bgloads[0] = float(bg_walltime);
    // fprintf(stderr, "[%d] my bgload is %f %f\n", mype, msg->bgloads[0], bg_walltime);

    return msg;
  }

  virtual void processDecision(TreeLBMessage* decision_msg, int& incoming, int& outgoing)
  {
    const int mype = CkMyPe();
    LLBMigrateMsg* decision = (LLBMigrateMsg*)decision_msg;
    incoming = decision->num_incoming[mype];
    CkAssert(incoming >= 0);
    outgoing = 0;
    int obj_start = decision->obj_start[mype];
    int obj_end = obj_start + int(myObjs.size());
    int j = 0;
    for (int i = obj_start; i < obj_end; i++, j++)
    {
      int dest = decision->to_pes[i];
      if (dest != mype)
      {
        if (dest >= 0)
        {
#if DEBUG__TREE_LB_L3
          CkPrintf("[%d] (processDecision) My obj %d (abs=%d) moving to %d\n", CkMyPe(),
                   j, i, dest);
#endif
          if (lbmgr->Migrate(myObjs[j].handle, dest) == 0)
          {
            CkAbort("PELevel: Migrate call returned 0\n");
            // missed.push_back(dest);
          }
        }
        else
        {
          // dest can be < 0, this can happen in some trees, if moving objects
          // between subtrees and I don't yet know the final destination PE
          outgoing += 1;
        }
      }
    }
#if DEBUG__TREE_LB_L2
    CkPrintf("[%d] PELevel::processDecision, incoming=%d outgoing=%d\n", CkMyPe(),
             incoming, outgoing);
#endif
  }

  int migrateObjects(const std::vector<std::pair<int, int>>& mig_order)
  {
    for (auto& move : mig_order)
    {
      int obj_local_id = move.first;
      int toPe = move.second;
#if DEBUG__TREE_LB_L3
      CkPrintf("[%d] (migrateObjects) migrating object with local ID %d to PE %d\n",
               CkMyPe(), obj_local_id, toPe);
#endif
      // import random
      // toPe = random.randint(0, charm.numPes() - 1)  # this is to verify that
      // verification framework works :)
      if (lbmgr->Migrate(myObjs[obj_local_id].handle, toPe) == 0)
      {
        CkAbort("PELevel: Migrate call returned 0\n");
      }
    }
    return mig_order.size();
  }

 protected:
  LBManager* lbmgr;
  bool rateAware;
  std::vector<LDObjData> myObjs;
};

// ---------------- MsgAggregator ----------------

/**
 * Currently only understands one msg type (LBStatsMsg_1) but could be made to
 * understand multiple
 */
class MsgAggregator : public LevelLogic
{
 public:
  MsgAggregator() {}

  virtual ~MsgAggregator() {}

  virtual TreeLBMessage* mergeStats()
  {
    TreeLBMessage* newMsg = LBStatsMsg_1::merge(stats_msgs);
    // need to cast pointer to ensure delete of CMessage_LBStatsMsg_1 is called
    for (auto m : stats_msgs) delete (LBStatsMsg_1*)m;
    stats_msgs.clear();
    return newMsg;
  }

  virtual std::vector<TreeLBMessage*> splitDecision(TreeLBMessage* decision,
                                                    std::vector<int>& children)
  {
    const int myNode = CkMyNode();
    std::vector<TreeLBMessage*> decisions(children.size() + 1);

    // Avoid allocating a new message until we get to a non-local child
    TreeLBMessage* remoteMsg = nullptr;

    // The first element is always for the local PE, the caller must delete it manually if
    // it is not used
    decisions[0] = decision;
    for (int i = 1; i < decisions.size(); ++i)
    {
      if (CkNodeOf(children[i-1]) == myNode)
      {
        CkReferenceMsg(decision);
        decisions[i] = decision;
      }
      // Use separate message for messages outside the process because it will get packed
      // and mess up local uses of the message
      else
      {
        if (remoteMsg == nullptr)
        {
          remoteMsg = (TreeLBMessage*)CkCopyMsg((void**)&decision);
        }
        else
        {
          CkReferenceMsg(remoteMsg);
        }
        decisions[i] = remoteMsg;
      }
    }

    return decisions;
  }
};

#endif /* TREELEVEL_H */
