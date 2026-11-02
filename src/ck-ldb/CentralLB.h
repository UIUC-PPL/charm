/**
 * \addtogroup CkLdb
*/
/*@{*/

#ifndef CENTRALLB_H
#define CENTRALLB_H

#include "BaseLB.h"
#include "CentralLB.decl.h"

#include <vector>
#include <unordered_map>
#include "pup_stl.h"
#include "manager.h"
#include "ckcheckpoint.h"
extern CkGroupID loadbalancer;

void CreateCentralLB();

class CLBStatsMsg;
class LBSimulation;

/// for backward compatibility
typedef LBMigrateMsg  CLBMigrateMsg;

class LBInfo
{
public:
  LBRealType *peLoads; 	// total load: object + background
  LBRealType *objLoads; 	// total obj load
  LBRealType *comLoads; 	// total comm load
  LBRealType *bgLoads; 	// background load
  int    numPes;
  int    msgCount;	// total non-local communication
  CmiUInt8  msgBytes;	// total non-local communication
  LBRealType minObjLoad, maxObjLoad;
  LBInfo(): peLoads(NULL), objLoads(NULL), comLoads(NULL), 
            bgLoads(NULL), numPes(0), msgCount(0),
            msgBytes(0), minObjLoad(0.0), maxObjLoad(0.0) {}
  LBInfo(LBRealType *pl, int count): peLoads(pl), objLoads(NULL), 
            comLoads(NULL), bgLoads(NULL), numPes(count), msgCount(0),
            msgBytes(0), minObjLoad(0.0), maxObjLoad(0.0) {}
  LBInfo(int count);
  ~LBInfo();
  void getInfo(BaseLB::LDStats* stats, int count, int considerComm);
  void clear();
  void print();
  void getSummary(LBRealType &maxLoad, LBRealType &maxCpuLoad, LBRealType &totalLoad);
};

/** added by Abhinav
 * class for computing the parent and children of a processor 
 */
class SpanningTree
{
	public:
		int arity;
		int parent;
		int numChildren;
		SpanningTree();
		void calcParent(int n);
		void calcNumChildren(int n);
};

class CentralLB : public CBase_CentralLB
{
private:
  CLBStatsMsg *statsMsg;
  int count_msgs;
  void initLB(const CkLBOptions &);
public:
  CkMarshalledCLBStatsMessage bufMsg;
  SpanningTree st;
  CentralLB(const CkLBOptions & opt) : CBase_CentralLB(opt), concurrent(false) { initLB(opt);
#if CMK_SHRINK_EXPAND
		manager_init();
#endif
  }
  CentralLB(CkMigrateMessage *m) : CBase_CentralLB(m), concurrent(false) {
#if CMK_SHRINK_EXPAND
		manager_init();
#endif
  }
#if defined(TEMP_LDB) 
	float getTemp(int);
	  FILE* logFD;
	int physicalCoresPerNode;
	int logicalCoresPerNode,numSockets;
	int logicalCoresPerChip;
#endif

  virtual ~CentralLB();

  void pup(PUP::er &p);

  void SetPESpeed(int);
  int GetPESpeed();
  inline void setConcurrent(bool c) { concurrent = c; }

  void CallLB();
  // Second half of CallLB on a CUDA build: runs once the process's per-object
  // GPU loads for this round exist.
  void gpuLoadsReady();
  void InvokeLB(); // Everything is at the PE barrier
  void ProcessAtSync(void); // Receive a message from AtSync to avoid
                            // making projections output look funny
  void SendStats();
  void ReceiveCounts(int *counts, int n);
  void ReceiveStats(CkMarshalledCLBStatsMessage &&msg);	// Receive stats on PE 0
  void ReceiveStatsViaTree(CkMarshalledCLBStatsMessage &&msg); // Receive stats using a tree structure  
  void ReceiveStatsFromRoot(CkMarshalledCLBStatsMessage &&msg);
  
  void depositData(CLBStatsMsg *m);
  void LoadBalance(void); 
  // Strategy() in two halves, so CHARM_LB_STRATEGY_THREAD (CentralLB.C) can run the compute on a
  // helper thread while the decision PE keeps delivering its objects' messages:
  // Strategy(stats) == strategyCompute(stats) + strategyFinish(stats).
  void strategyCompute(LDStats* stats);            // work(): thread-safe for MetisLB
  LBMigrateMsg* strategyFinish(LDStats* stats);    // verifier, migrate message: on the PE
  void strategyFinishOffThread(LDStats* stats);    // the helper's completion, from a scheduler poll
  void t_LoadBalance(void); 
  void ApplyDecision(void);
  void ResumeClients(int);                      // Resuming clients needs

  void ResumeClients(); // to be resumed via message
  void InitiateScatter(LBMigrateMsg *msg);
  void ScatterMigrationResults(LBScatterMsg *);
  void ReceiveMigration(LBScatterMsg *);
  void ReceiveMigration(LBMigrateMsg *); 	// Receive migration data
  void ProcessMigrationDecision();
  void ProcessReceiveMigration();
  void ReleaseNextBatch();
  void MissMigrate(int waitForBarrier);

  //Shrink-Expand related functions
  void CheckForLB();
  void CheckForRealloc ();
  void ResumeFromReallocCheckpoint();
  void MigrationDoneImpl (int );
  void WillIbekilled(std::vector<char> avail, int);
  void StartCleanup();

  // manual start load balancing
  inline void StartLB() { thisProxy.ProcessAtSync(); }

  // Migrated-element callback
  void Migrated(int waitBarrier=1);

  void MigrationDone(int balancing);  // Call when migration is complete
  void CheckMigrationComplete();      // Call when all migration is complete

  // IMPLEMENTATION FOR FUTURE PREDICTOR
  void FuturePredictor(LDStats* stats);

  struct FutureModel {
    int cur_stats;   // number of statistics currently present
    int start_stats; // next stat to be written
    std::vector<LDStats> collection;
    LBPredictorFunction *predictor;
    std::vector<std::vector<double>> parameters;
    std::vector<bool> model_valid;

    FutureModel() : FutureModel(0, new DefaultFunction()) {}

    FutureModel(int n) : FutureModel(n, new DefaultFunction()) {}

    FutureModel(int n, LBPredictorFunction* myfunc)
        : cur_stats(0), start_stats(0), collection(n), predictor(myfunc)
    {
    }

    ~FutureModel() {
      delete predictor;
    }

    void changePredictor(LBPredictorFunction* new_predictor)
    {
      // gain control of the provided predictor
      delete predictor;
      predictor = new_predictor;

      for (auto& param_list : parameters)
      {
        param_list.resize(new_predictor->num_params);
      }
      model_valid.assign(model_valid.size(), false);
    }
  };

  // create new predictor, if one already existing, delete it first
  // if "pred" == 0 then the default function is used
  void predictorOn(LBPredictorFunction *pred) {
    predictorOn(pred, _lb_predict_window);
  }
  void predictorOn(LBPredictorFunction *pred, int window_size) {
    if (predicted_model) {
      PredictorPrintf("Predictor already allocated");
    } else {
      _lb_predict_window = window_size;
      if (pred) predicted_model = new FutureModel(window_size, pred);
      else predicted_model = new FutureModel(window_size);
      _lb_predict = true;
    }
    PredictorPrintf("Predictor turned on, window size %d\n",window_size);
  }

  // deallocate the predictor
  void predictorOff() {
    if (predicted_model) delete predicted_model;
    predicted_model = 0;
    _lb_predict = false;
    PredictorPrintf("Predictor turned off\n");
  }

  // change the function of the predictor, at runtime
  // it will do nothing if it does not exist
  void changePredictor(LBPredictorFunction *new_predictor) {
    if (predicted_model) {
      predicted_model->changePredictor(new_predictor);
      PredictorPrintf("Predictor model changed\n");
    }
  }
  // END IMPLEMENTATION FOR FUTURE PREDICTOR

  LBMigrateMsg* callStrategy(LDStats* stats,int count){
    return Strategy(stats);
  };

  int cur_ld_balancer;

  void readStatsMsgs(const char* filename);
  void writeStatsMsgs(const char* filename);

  void removeCommDataOfDeletedObjs(LDStats* stats);
  void preprocess(LDStats* stats);
  virtual LBMigrateMsg* Strategy(LDStats* stats);
  virtual void work(LDStats* stats);
	virtual void changeFreq(int n);
  virtual LBMigrateMsg * createMigrateMsg(LDStats* stats);
  virtual LBMigrateMsg * extractMigrateMsg(LBMigrateMsg *m, int p);
  // Hands a Clone / Retire record (LBDecisionKind) addressed to this PE to its
  // object; see LDObjData::replicaGroup.
  void executeReplicaDecision(const LDObjHandle& h, unsigned char kind, int toPe, int rank);

  // Not to be used -- maintained for legacy applications
  virtual LBMigrateMsg* Strategy(LDStats* stats, int nprocs) {
    return Strategy(stats);
  }

#if CMK_GPU
  // Destination-dependent GPU cost prediction, shared by every centralized
  // strategy. Lives here rather than in one balancer so the estimator can be
  // developed and judged independently of whichever policy consumes it.
  const GpuScalingModel& gpuScalingModel() const { return gpuScalingModel_; }

  // Predicted whole-device GPU seconds for `obj` if it ran on `destination`.
  // Falls back to the measured scalar gpuTime when the model cannot price the
  // pair, so a strategy always gets a usable number.
  double predictGpuCost(const LDObjData& obj, const ProcStats& destination) const;
#endif

protected:
  virtual bool QueryBalanceNow(int) { return true; };  
  virtual bool QueryDumpData() { return false; };  
  virtual void LoadbalanceDone(int balancing) {}

  void simulationRead();
  void simulationWrite();
  void findSimResults(LDStats* stats, int count, 
                      LBMigrateMsg* msg, LBSimulation* simResults);
  void removeNonMigratable(LDStats* statsDataList, int count);
  void loadbalance_with_thread() { use_thread = true; }

  bool concurrent;
  bool strategyOffThreadOk;   // a derived balancer whose work() reads PE-local state sets this false in its ctor
private:  
  int myspeed;
  int stats_msg_count;
  CLBStatsMsg **statsMsgsList;
  LDStats *statsData;
  int migrates_completed;
  int migrates_expected;
  int future_migrates_completed;
  int future_migrates_expected;
  int lbdone;
  double start_lb_time;
  double strat_start_time;
  LBMigrateMsg   *storedMigrateMsg;
  LBScatterMsg   *storedScatterMsg;
  bool  reduction_started;
  bool  use_thread;

  // Batched migration execution (memory contract I-batch). The decision PE
  // stashes one message per batch and releases them sequentially; every PE
  // tracks which batch it is executing so MigrationDone can distinguish a
  // batch boundary (reset counters, contribute to ReleaseNextBatch) from the
  // end of the step (resume clients).
  std::vector<LBMigrateMsg*> lbBatchMsgs;
  int lbNextBatch = 0;
  int lbCurBatch = 0;
  int lbCurNBatches = 1;
  void sendDecision(LBMigrateMsg* msg);

  FutureModel *predicted_model;

#if CMK_GPU
  GpuScalingModel gpuScalingModel_;
  uint64_t gpuScalingEpoch_;
  // Last epoch's shadow predictions, checked against what actually happened
  // once the objects have run again. Keyed by full LB identity so a migration
  // does not lose the pairing.
  std::unordered_map<LDObjKey, GpuShadowPrediction, LDObjKeyHash> gpuShadow_;
  GpuPredictionAccuracy gpuAccuracy_;

  // Register device types, pick a reference, learn from this epoch's summaries,
  // then score last epoch's predictions. Runs after the statistics are complete
  // and before Strategy, and never influences placement.
  void updateGpuScalingModel(LDStats* stats);
  void scoreGpuShadowPredictions(LDStats* stats);
  void reportGpuScalingModel(LDStats* stats) const;
#endif

  void BuildStatsMsg();
  void buildStats();
  void printStrategyStats(LBMigrateMsg *msg);

public:
  int useMem();
};


// CLBStatsMsg is not directly sent in the entry function
// CkMarshalledCLBStatsMessage is used instead to use the pup defined here.
//class CLBStatsMsg: public CMessage_CLBStatsMsg {
class CLBStatsMsg {
public:
#if defined(TEMP_LDB)
	float pe_temp;
#endif

  int from_pe;
  int pe_speed;
#if CMK_GPU
  size_t gpu_mem_remaining;
  size_t pool_buff_mem_remaining;
  size_t gpu_pool_arena_bytes;
  size_t gpu_pool_capacity_bytes;
  int gpu_ipc_slots;
  uint64_t gpu_device_id;
  int gpu_total_sms;
  GpuDeviceDescriptor gpu_descriptor;
#endif
  LBRealType total_walltime;
  LBRealType idletime;
  LBRealType bg_walltime;
#if CMK_LB_CPUTIMER
  LBRealType total_cputime;
  LBRealType bg_cputime;
#endif
  std::vector<LDObjData> objData;
  std::vector<LDCommData> commData;

  std::vector<char> avail_vector;
  int next_lb;

public:
  CLBStatsMsg(int osz, int csz);
  CLBStatsMsg(): from_pe(0), pe_speed(0),
#if CMK_GPU
		 gpu_device_id(-1), gpu_total_sms(0),
		 gpu_mem_remaining(0), pool_buff_mem_remaining(0),
		 gpu_pool_arena_bytes(0), gpu_pool_capacity_bytes(0), gpu_ipc_slots(0),
#endif
		 total_walltime(0.0), idletime(0.0),
		 bg_walltime(0.0),
#if defined(TEMP_LDB)
		pe_temp(1.0),
#endif

#if CMK_LB_CPUTIMER
		 total_cputime(0.0), bg_cputime(0.0),
#endif
		 next_lb(0) {}
  ~CLBStatsMsg();
  void pup(PUP::er &p);
}; 


// compute load distribution info
void getLoadInfo(BaseLB::LDStats* stats, int count, LBInfo &info, int considerComm);

#endif /* CENTRALLB_H */

/*@}*/


