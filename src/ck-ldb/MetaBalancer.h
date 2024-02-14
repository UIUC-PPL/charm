/**
* Author: Harshitha Menon
* Date: 2012
*
* Meta-Balancer is for automating the load balancing decisions based on the
* application characteristics. The decision of when to call the load balancer is
* handled by the MetaBalancer if +MetaLB flag is set when launching the
* application. AtSync should be called very often (every couple of iterations).
*
* Meta-Balancer is not aware of the application iteration so it depends on
* AtSync calls to count the iterations. At every AtSync call, if +MetaLB is
* set, the chare sends its object load to the MetaBalancer on its local
* processor and resumes its work. Once all the chares residing on the processor
* has contributed their load information, this information is collected at the
* central processor(root) using reduction. The root calculates the ideal period
* based on linear extrapolation and informs a tentative lb period to all the
* processors via broadcast. The Meta-Balancer residing on each processor then
* informs the root about the maximum iteration of any chare on their processor.
* The root then informs the final lb period, which is the max of calculated and
* max. Meanwhile, chares can be in various states. Chare goes to LOAD_BALANCE
* state when it enters load balancing phase. It goes to PAUSE state when the
* chare has reached the tentative period and is waiting for the final period to
* be announced.
*
* To handle the case of no objects on a particular processor, a timer call is
* set which checks for the number of objects and if found to be == 0,
* contributes to the reduction which collects minimum statistics.
*/

#ifndef METABALANCER_H
#define METABALANCER_H

#include "LBManager.h"
#include "RandomForestModel.h"
#include "fastforest.h"
#include <vector>

#include "MetaBalancer.decl.h"

using namespace rfmodel;

extern CkGroupID _metalb;
extern CkGroupID _metalbred;

CkpvExtern(int, metalbInited);

void _MetaLBInit();

// main chare
class MetaLBInit : public Chare {
  public:
    MetaLBInit(CkArgMsg*);
    MetaLBInit(CkMigrateMessage *m):Chare(m) {}
};

enum metalb_stats_types{
  ITER_NO,
  NUM_PROCS,
  TOTAL_LOAD,
  MAX_LOAD,
  IDLE_TIME,
  UTILIZATION,
  TOTAL_LOAD_W_BG,
  MAX_LOAD_W_BG,
  TOTAL_KBYTES,
  TOTAL_KMSGS,
  WITHIN_PE_KBYTES,
  OUTSIDE_PE_KBYTES,
  SUM_COMM_NEIGHBORS,
  MAX_COMM_NEIGHBORS,
  SUM_OBJ_COUNT,
  MAX_OBJ_COUNT,
  SUM_OBJ_LOAD,
  MAX_OBJ_LOAD,
  SUM_HOPS,
  SUM_HOP_KBYTES,
  LOAD_STDEV2,
  MAX_UTIL,
  MIN_LOAD,
  MIN_BG,
  MIN_OBJ_LOAD,
  MAX_ITER_TIME,
  LOAD_SKEWNESS,
  LOAD_KURTOSIS,
  TOTAL_OVERLOADED_PES,
};

class MetaBalancer : public CBase_MetaBalancer {
public:
 MetaBalancer(void) : rFmodel(NULL) { init(); }
 MetaBalancer(CkMigrateMessage* m) : CBase_MetaBalancer(m) { init(); }
 ~MetaBalancer() {
   if (CkMyPe() == 0) {
     delete rFmodel;
   }
 }

private:
  void init();
  MetaBalancerRedn* metaRdnGroup;

public:
  inline static MetaBalancer * Object() {
    return CkpvAccess(metalbInited)?(MetaBalancer *)CkLocalBranch(_metalb):NULL;
  }

  static void initnodeFn(void);

  void pup(PUP::er& p);

  void ResumeClients();

  void ResetAdaptive();
  int get_iteration();
  int get_finished_iteration();
  void ContributeStats(int iteration);
  void AdjustCountForDeadContributor(int iteration);
  void AdjustCountForNewContributor(int iteration);
  bool AddLoad(int iteration, double load);
  void SetCharePupSize(size_t psize);
  void ReceiveMinStats(double *load, int n);
  void TriggerSoon(int iteration_no, double imbalance_ratio, double tolerate_imb);
  void LoadBalanceDecision(int, int);
  void LoadBalanceDecisionFinal(int, int);
  void MetaLBCallLBOnChares();
  void MetaLBSetLBOnChares(int switchFrom, int switchTo);
  void ReceiveIterationNo(int); // Receives the current iter no
  static void periodicCall(void *ad);
  static void checkForNoObj(void *ad);
  void HandleAdaptiveNoObj();
  void RegisterNoObjCallback(int index);
  void TriggerAdaptiveReduction();
  void PreviousAvgLoad(double avg);

  bool generatePlan(int& period, double& ratio_at_t);
  bool getLineEq(double new_load_percent, double& aslope, double& ac,
      double& mslope, double& mc);
  bool getPeriodForLinear(double a, double b, double c, int& period);
  bool getPeriodForStrategy(double new_load, double overhead_percent,
      int& period, double& ratio_at_t);
  int getPredictedLBPeriod(bool& is_tentative);

  bool isStrategyComm();

  void UpdateAfterLBData(int is_lb_refine, double lb_max, double lb_avg, double
      local_comm, double remote_comm);

  void UpdateAfterLBData(double max_load, double max_cpu, double avg_load);
  void UpdateAfterLBComm(double alpha_beta_cost);
  void GetPrevLBData(int& lb_type, double& lb_max_avg_ratio, double&
      local_remote_comm_ratio);
  void GetLBDataForLB(int lb_type, double& lb_max_avg_ratio, double&
      local_remote_comm_ratio);

  void SetMigrationCost(double lb_migration_cost);
  void SetStrategyCost(double lb_strategy_cost);

private:
  LBManager* lbmanager;
  std::vector<double> total_load_vec;
  // Keeps track of how many local chares contributed
  std::vector<int> total_count_vec;
  std::vector<double> max_load_vec;
  std::vector<double> min_load_vec;
  std::vector<int> lbdb_no_obj_callback;

  double prev_idle;
  double alpha_beta_cost_to_load;
  int is_prev_lb_refine;
  int prev_bytes, prev_msgs;
  int prev_outsidepemsgs, prev_outsidepebytes;
  int prev_hops, prev_hopbytes;
  bool print_iteration_time;
  double prev_iteration_time;
  double prev_avg_load;
  int chare_pup_size;
  double pe_ld_kurtosis;
  double pe_ld_skewness;
  int total_ovld_pes;
  int current_balancer;
  ForestModel* rFmodel;
  fastforest::FastForest xgboost;

  struct AdaptiveData {
    double iteration;
    double max_load;
    double avg_load;
    double min_utilization;
    double avg_utilization;
    double total_bytes;
    double total_msgs;
  };

  struct AdaptiveMetaBalancer {
    std::vector<AdaptiveData> history_data;
    int lb_iter_no;
  } adaptive_lbdb;

  struct AdaptiveLBInfo {
    AdaptiveLBInfo() {
      max_avg_ratio = 1;
      remote_local_ratio = 1;
    }
    double max_avg_ratio;
    double remote_local_ratio;
  };

  // TODO: Separate out the datastructure required by just the central and on all
  // processors
  struct AdaptiveLBStructure {
    int tentative_period;
    int final_lb_period;
    // This is based on the linear extrapolation
    int lb_calculated_period;
    // Current maximum iteration no of any chare on this processor
    int lb_iteration_no;
    // This corresponds to the last iteration that was contributed
    int finished_iteration_no;
    // This is set when all the processor sends the maximum iteration no
    int global_max_iter_no;
    // This keeps track of what was the max iteration no we had previously
    // received. TODO: Mostly global_max_iter_no should be sufficied.
    int tentative_max_iter_no;
    // true indicates it is in Inform->ReceiveMaxIter->FinalLBPeriod stage.
    bool in_progress;
    double lb_strategy_cost;
    double lb_migration_cost;
    bool doCommStrategy;
    int lb_msg_send_no;
    int lb_msg_recv_no;
    // Total AtSync calls from all the chares residing on the processor
    int total_syncs_called;
    int last_lb_type;
    AdaptiveLBInfo greedy_info;
    AdaptiveLBInfo refine_info;
    AdaptiveLBInfo comm_info;
    AdaptiveLBInfo comm_refine_info;
    AdaptiveLBInfo info_first_iter;
  } adaptive_struct;

public:
  bool lb_in_progress;
  bool ignore_periodic;
};

class MetaBalancerRedn : public CBase_MetaBalancerRedn {
  public:
    MetaBalancerRedn(void) {init();}
    MetaBalancerRedn(CkMigrateMessage *m) : CBase_MetaBalancerRedn(m)  {init();}
    ~MetaBalancerRedn()  {}
    void pup(PUP::er& p);
    void ReceiveIterNo(int max_iter);
    void getMaxIter(int);

  private:
    MetaBalancer* metabalancer;
    void init();
};

inline MetaBalancer* MetaBalancerObj() { return MetaBalancer::Object(); }

#endif /* LDATABASE_H */

/*@}*/
