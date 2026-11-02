/**
 * \addtogroup CkLdb
 */
/*@{*/

#ifndef LBMANAGER_H
#define LBMANAGER_H

#include <cassert>
#include <unordered_map>

#include "LBDatabase.h"
#include "conv-ccs.h"   // CcsDelayedReply
#include <deque>
#include "json_fwd.hpp"
using json = nlohmann::json;

// 4 adds GpuObjectEpochCosts to LDObjData and GpuDeviceDescriptor to ProcStats.
// 5 adds the device pool's arena quantum and the IPC slot budget to ProcStats.
// 8 adds the replication declaration (replicaGroup ...) to LDObjData.
// 9 adds a copy's recurring exchange cost (replicaCostNode / replicaCostRemote).
#define LB_FORMAT_VERSION 9

#define LB_MANAGER_VERSION 1

class MetaBalancer;
extern int _lb_version;

// command line options
class CkLBArgs
{
 private:
  double _autoLbPeriod;  // in seconds
  // GreedyRefineCentralGPULB: how far above plain greedy's max load the
  // refinement may settle in exchange for fewer migrations (1.1 = 10% higher,
  // the default). 0 or below asks for the parameter search instead: every PE
  // builds a candidate from a different (A,B) pair and the best is chosen,
  // at the cost of a reduction per balancing step.
  double _lb_greedyRefineTolerance;
  double _lb_alpha;      // per message send overhead
  double _lb_beta;       // per byte send overhead
  int _lb_debug;         // 1 or greater
  bool _lb_printsummary;  // print summary
  bool _lb_loop;          // use multiple load balancers in loop
  bool _lb_ignoreBgLoad;
  bool _lb_migObjOnly;  // only consider migratable objs
  bool _lb_syncResume;
  bool _lb_samePeSpeed;     // ignore cpu speed
  bool _lb_testPeSpeed;     // test cpu speed
  bool _lb_useCpuTime;      // use cpu instead of wallclock time
  bool _lb_statson;         // stats collection
  bool _lb_traceComm;       // stats collection for comm
  bool _lb_diffcomm;        // DiffusionLB: use the communication graph
  // Path to the transfer-cost table written by the lbcalib benchmark. Empty
  // means no cost model: DiffusionLB accepts every quota-driven move, which is
  // the historical behaviour.
  const char* _lb_costConfig;
  bool _lb_noMST;           // DiffusionLB: skip MST neighbor construction
  int _lb_diffnumnbors;     // DiffusionLB: number of diffusion neighbors
  double _lb_diffbeta;      // DiffusionLB: second-order diffusion momentum (1.0 = off)
  // Which load dimension the one-dimensional balancers (MetisLB's cross level,
  // DiffusionLB's across-node phase, every ckgraph consumer) balance. Neither
  // flag: the dimension that binds, measured per step (LBLoadDim.h).
  bool _lb_diffgpudim;      // force device occupancy
  bool _lb_diffhostdim;     // force host time
  // DiffusionLB decision floors. An imbalance below _lb_diffMinImbalance (a
  // fraction of the mean) is treated as measurement noise and left alone, at
  // both the across-node and the within-node level. _lb_diffMaxMoveFrac caps
  // the share of a node's migratable LOAD that may leave it across nodes in
  // one step; 1.0, the default, applies no cap and lets the pseudo rounds'
  // own obligation govern. A cap below 1 is spent on one neighbour at a time
  // (selection stays with a neighbour until it can take nothing more), so a
  // node far above its fair share exhausts the budget on its first
  // destination and never reaches the rest: measured on sph2d, 0.25 left
  // node 0 shedding 2.23 of the 6.16 it owed and cost 13 ms/step.
  // ShedLB: no bin may exceed (1 + _lb_shedEps) times the mean load. Below
  // the largest object's share of the mean it cannot be met at all, because
  // an indivisible object sets a floor on any assignment's maximum.
  //
  // The default is not "as tight as possible". A target tighter than the rate
  // at which load drifts buys precision the next step destroys and pays for it
  // in migrations: on the moe example at Llama size, 0.02 moved 19 objects for
  // 549 ms and 0.10 moved 7 for 490 ms, with 0.20 losing the balance again at
  // 538 ms. Match it to how far load moves between balancing steps.
  double _lb_shedEps;
  double _lb_diffMinImbalance;
  double _lb_diffMaxMoveFrac;
  // The alpha (LBLoadDim.h) at or above which BOTH load dimensions count:
  // DiffusionLB then diffuses step time and MetisLB carries the second
  // dimension as a partition constraint. Above 1 never; 0 always.
  double _lb_loadVectorAbove;
  bool _lb_gpuScaling;      // Learn destination-dependent per-kernel GPU costs
  double _lb_gpuScalingAlphaMin;  // EWMA floor; 0 selects a stationary mean
  int _lb_gpuScalingMinSamples;   // Samples needed before a factor is calibrated
  int _lb_gpuScalingMaxComponents;  // Modeled kernel buckets kept per object/epoch
  char* _lb_gpuRateFile;           // Optional GPU-type peak-rate overrides
  int _lb_central_pe;      // processor number for centralized strategy
  int _lb_maxDistPhases;   // Specifies the max number of LB phases in DistributedLB
  double _lb_targetRatio;  // Specifies the target load ratio for LBs that aim for a
                           // particular load ratio
  bool _lb_async;              // AtSyncStart returns without waiting for the step
  bool _lb_asyncFromFlag;      // +LBAsync was on the command line (so an application's request changes nothing)
  bool _lb_peerDecision;       // Scatter the LB decision instead of broadcasting it
  bool _lb_metaLbOn;
  bool _lb_metaLbGpuTrigger;   // MetaLB: trigger on per-device GPU imbalance
  double _lb_metaLbGpuTol;     // MetaLB: max/avg device load that triggers
  char* _lb_metaLbModelDir;
  char* _lb_treeLBFile = (char*)"treelb.json";
  int _lb_percentMovesAllowed;  // for GreedyRefineCentralLB, as percentage of chares that can be moved

 public:
  CkLBArgs()
  {
    _autoLbPeriod = -1.0;  // off by default
    _lb_debug = 0;
    _lb_ignoreBgLoad = _lb_syncResume = _lb_useCpuTime = false;
    _lb_printsummary = _lb_migObjOnly = false;
    _lb_statson = true;
    _lb_traceComm = false;
    _lb_diffcomm = false;
    _lb_costConfig = NULL;
    _lb_noMST = false;
    _lb_diffnumnbors = 1;
    _lb_diffbeta = 1.0;
    _lb_diffgpudim = false;
    _lb_diffhostdim = false;
    _lb_shedEps = 0.10;
    _lb_diffMinImbalance = 0.10;   // DiffusionLB's floor: 0.03 made it over-move on sph2d's moving load (1.86 vs 1.66 ms/step, 2026-10-18); MetisLB has its own floor
    _lb_diffMaxMoveFrac = 1.0;
    _lb_loadVectorAbove = 0.5;
    _lb_gpuScaling = false;
    _lb_gpuScalingAlphaMin = 0.0;
    _lb_gpuScalingMinSamples = 1;
    _lb_gpuScalingMaxComponents = 64;
    _lb_gpuRateFile = nullptr;
    _lb_loop = false;
    _lb_central_pe = 0;
    _lb_maxDistPhases = 10;
    _lb_targetRatio = 1.05;
    _lb_async = false;
    _lb_asyncFromFlag = false;
    _lb_peerDecision = false;
    _lb_metaLbOn = false;
    _lb_metaLbGpuTrigger = false;
    _lb_metaLbGpuTol = 1.1;
    _lb_metaLbModelDir = nullptr;
    _lb_percentMovesAllowed = 100;
    _lb_greedyRefineTolerance = 1.1;  // 10% over greedy; <=0 searches instead
  }
  inline char*& treeLBFile() { return _lb_treeLBFile; }
  inline double& lbperiod() { return _autoLbPeriod; }
  inline int& debug() { return _lb_debug; }
  inline bool& printSummary() { return _lb_printsummary; }
  inline int& lbversion() { return _lb_version; }
  inline bool& loop() { return _lb_loop; }
  inline bool& ignoreBgLoad() { return _lb_ignoreBgLoad; }
  inline bool& migObjOnly() { return _lb_migObjOnly; }
  inline bool& syncResume() { return _lb_syncResume; }
  inline bool& samePeSpeed() { return _lb_samePeSpeed; }
  inline bool& testPeSpeed() { return _lb_testPeSpeed; }
  inline bool& useCpuTime() { return _lb_useCpuTime; }
  inline bool& statsOn() { return _lb_statson; }
  inline bool& traceComm() { return _lb_traceComm; }
  inline bool& diffusionCommOn() { return _lb_diffcomm; }
  inline const char*& costConfig() { return _lb_costConfig; }
  inline bool& noMST() { return _lb_noMST; }
  inline int& diffusionNumNbors() { return _lb_diffnumnbors; }
  inline double& diffusionBeta() { return _lb_diffbeta; }
  inline bool& diffusionGpuDim() { return _lb_diffgpudim; }
  inline bool& diffusionHostDim() { return _lb_diffhostdim; }
  inline double& shedEps() { return _lb_shedEps; }
  inline double& diffusionMinImbalance() { return _lb_diffMinImbalance; }
  inline double& diffusionMaxMoveFrac() { return _lb_diffMaxMoveFrac; }
  inline double& loadVectorAbove() { return _lb_loadVectorAbove; }
  inline bool& gpuScaling() { return _lb_gpuScaling; }
  inline double& gpuScalingAlphaMin() { return _lb_gpuScalingAlphaMin; }
  inline int& gpuScalingMinSamples() { return _lb_gpuScalingMinSamples; }
  inline int& gpuScalingMaxComponents() { return _lb_gpuScalingMaxComponents; }
  inline char*& gpuRateFile() { return _lb_gpuRateFile; }
  inline int& central_pe() { return _lb_central_pe; }
  inline double& alpha() { return _lb_alpha; }
  inline double& greedyRefineTolerance() { return _lb_greedyRefineTolerance; }
  inline double& beta() { return _lb_beta; }
  inline int& maxDistPhases() { return _lb_maxDistPhases; }
  inline double& targetRatio() { return _lb_targetRatio; }
  inline bool& lbAsync() { return _lb_async; }
  inline bool& asyncFromFlag() { return _lb_asyncFromFlag; }
  inline bool& lbPeerDecision() { return _lb_peerDecision; }
  inline bool& metaLbOn() { return _lb_metaLbOn; }
  inline bool& metaLbGpuTrigger() { return _lb_metaLbGpuTrigger; }
  inline double& metaLbGpuTolerance() { return _lb_metaLbGpuTol; }
  inline char*& metaLbModelDir() { return _lb_metaLbModelDir; }
  inline int& percentMovesAllowed() { return _lb_percentMovesAllowed; }
};

extern CkLBArgs _lb_args;

extern bool _lb_predict;
extern int _lb_predict_delay;
extern int _lb_predict_window;
extern bool _lb_psizer_on;
#ifndef PREDICT_DEBUG
#define PREDICT_DEBUG 0  // 0 = No debug, 1 = Debug info on
#endif
#define PredictorPrintf \
  if (PREDICT_DEBUG) CmiPrintf

extern void realloc(char*);

// used in constructor of all load balancers
class CkLBOptions
{
 private:
  int seqno;  // for centralized lb, the seqno
  std::string legacyName;
 public:
  CkLBOptions() : seqno(-1), legacyName() {}
  CkLBOptions(int s) : seqno(s), legacyName() {}
  CkLBOptions(int s, const char* legacyName) : seqno(s), legacyName(legacyName ? legacyName : "") {}
  int getSeqNo() const { return seqno; }
  bool hasLegacyName() const { return !legacyName.empty(); }
  const char* getLegacyName() const {
    assert(hasLegacyName());
    return legacyName.c_str();
  }

  void pup(PUP::er& p)
  {
    p | seqno;
    p | legacyName;
  }
};

#include "LBManager.decl.h"

    extern CkGroupID _lbmgr;
// An application that implements the split barrier (AtSyncStart / AtSyncWait)
// turns asynchronous balancing on from its main chare with this, instead of
// requiring +LBAsync on the command line. Broadcast to every PE; it must be
// called before the first balancing step.
void CkLBSetAsync(bool on);

CkpvExtern(bool, lbmanagerInited);

// Sequence number of the most recent load balancing step MetaBalancer has asked
// for on this PE, or 0 if none. Monotonic, never cleared: a chare joins a step
// when this rises above the sequence it last joined, which makes the check in
// AtSyncStart a single read against a PE-private int and lets it run on every
// application iteration.
CkpvExtern(int, _lbStepRequested);

// The value of CkMigratable::lbIterNo at which the requested step is to be
// joined, agreed across all PEs before the request is broadcast. Chares join at
// this count and not before, so every chare enters the step at the same
// application iteration.
CkpvExtern(int, _lbStepPeriod);

// The floor broadcast when the vote opens, before the count is agreed. No chare
// runs past this, which is what makes the agreed count authoritative rather
// than a race the broadcast has to win: the furthest any chare can be when the
// count is settled is one call past what it reported, and the agreed count is
// one past the furthest report.
CkpvExtern(int, _lbStepTentative);

// LB options, mostly controled by user parameter
extern char* _lbtopo;

typedef void (*LBCreateFn)(const CkLBOptions&);
typedef BaseLB* (*LBAllocFn)();
void LBDefaultCreate(LBCreateFn f);

void LBRegisterBalancer(std::string, LBCreateFn, LBAllocFn, std::string, bool shown = true);
bool LBHasBalancersRegistered();

template <typename T>
void LBRegisterBalancer(std::string name, std::string description, bool shown = true)
{
  LBRegisterBalancer(
      name, [](const CkLBOptions& opts) { T::proxy_t::ckNew(opts); },
      []() -> BaseLB* { return new T(static_cast<CkMigrateMessage*>(nullptr)); },
      description, shown);
}

void _LBMgrInit();

// main chare
class LBMgrInit : public Chare
{
 public:
  LBMgrInit(CkArgMsg*);
  LBMgrInit(CkMigrateMessage* m) : Chare(m) {}
};

// class which implement a virtual function for the FuturePredictor
class LBPredictorFunction
{
 public:
  virtual ~LBPredictorFunction() {}
  int num_params;

  virtual void initialize_params(double* x)
  {
    double normall = 1.0 / pow((double)2, (double)31);
    for (int i = 0; i < num_params; ++i) x[i] = rand() * normall;
  }

  virtual double predict(double x, double* params) = 0;
  virtual void print(double* params) { PredictorPrintf("LB: unknown model\n"); };
  virtual void function(double x, double* param, double& y, double* dyda) = 0;
};

// a default implementation for a FuturePredictor function
class DefaultFunction : public LBPredictorFunction
{
 public:
  // constructor
  DefaultFunction() { num_params = 6; };

  // compute the prediction function for the variable x with parameters param
  double predict(double x, double* param)
  {
    return (param[0] + param[1] * x + param[2] * x * x +
            param[3] * sin(param[4] * (x + param[5])));
  }

  void print(double* param)
  {
    PredictorPrintf("LB: %f + %fx + %fx^2 + %fsin%f(x+%f)\n", param[0], param[1],
                    param[2], param[3], param[4], param[5]);
  }

  // compute the prediction function and its derivatives respect to the parameters
  void function(double x, double* param, double& y, double* dyda)
  {
    double tmp;

    y = predict(x, param);

    dyda[0] = 1;
    dyda[1] = x;
    dyda[2] = x * x;
    tmp = param[4] * (x + param[5]);
    dyda[3] = sin(tmp);
    dyda[4] = param[3] * (x + param[5]) * cos(tmp);
    dyda[5] = param[3] * param[4] * cos(tmp);
  }
};

class LBManager : public CBase_LBManager
{
 private:
  struct StartLBCB
  {
    std::function<void()> fn;
    bool on;
  };

  struct MigrationDoneCB
  {
    std::function<void()> fn;
  };

  struct PredictCB
  {
    std::function<void(LBPredictorFunction* model)> on;
    std::function<void(LBPredictorFunction* model, int win)> onWin;
    std::function<void()> off;
    std::function<void(LBPredictorFunction* model)> change;
  };

  LBDatabase* lbdb_obj;

  // CCS handler "LBObjLoads": pending reply tokens and whether a collection
  // round is in flight (one round serves every request that arrived meanwhile)
  std::deque<CcsDelayedReply> objLoadsReplies;
  bool objLoadsCollecting;

  bool useBarrier;            // use barrier or not

  PredictCB* predictCBFn;

  int startLBFn_count;

  int resume_epoch = 0;

  char* reallocBuffer;

 public:
  int chare_count;
  bool lb_in_progress;

  // Number of load balancing steps whose clients this PE has resumed. Under
  // +LBSyncResume -- which +LBAsync forces on -- the resume is driven by a
  // global reduction, so this advances in lockstep everywhere and is
  // comparable across PEs. An element parked in AtSyncWait() records the value
  // it is waiting past, which is how one that gets migrated by the very step it
  // is waiting on can tell, on arrival, that its destination already resumed.
  int resumeEpoch() const { return resume_epoch; }


  LBManager(void) { init(); }
  LBManager(CkMigrateMessage* m) : CBase_LBManager(m) { init(); }
  ~LBManager() { delete lbdb_obj; }

 private:
  void init();
  void InvokeLB();

  // Move-ledger state (see the ledger* methods below). Transient within one
  // balancer step; deliberately not pupped.
  std::unordered_map<CmiUInt8, int> ledgerEntries;  // element id -> dest PE
  bool ledgerRecording = false;  // inside the decision loop
  bool ledgerActive = false;     // open, or closed and draining
  bool ledgerClosed = false;     // all moves issued; fire when empty
  int ledgerStep = -1;
  double ledgerOpenTime = 0.0;
  double ledgerLastWarn = 0.0;
  std::function<void()> ledgerDoneFn;
  void maybeFireLedger();
  static void ledgerWatch(void* arg);

 public:
  LBDatabase* getLBDB() { return lbdb_obj; }
  inline static LBManager* Object()
  {
    return CkpvAccess(lbmanagerInited) ? (LBManager*)CkLocalBranch(_lbmgr) : NULL;
  }

  static void initnodeFn(void);

  void pup(PUP::er& p);

  void configureTreeLB(const char* json_str);
  void configureTreeLB(json& config);

  void bufferRealloc(char* bitmap)
  {
    int size = CkNumPes() + 2 * sizeof(int);
    reallocBuffer = (char*)malloc(size);
    memcpy(reallocBuffer, bitmap, size);
  }

  void callRealloc()
  {
    if (reallocBuffer != nullptr)
    {
      realloc(reallocBuffer);
      reallocBuffer = nullptr;
    }
  }

  /*
   * Calls from object managers to load database
   */
  //  LDOMHandle RegisterOM(LDOMid userID, void *userptr, LDCallbacks cb);

  static void periodicLB(void*);
  void setTimer();

  LDOMHandle RegisterOM(LDOMid userID, void* userptr, LDCallbacks cb)
  {
    return lbdb_obj->RegisterOM(userID, userptr, cb);
  }
  int Migrate(LDObjHandle h, int dest) { return lbdb_obj->Migrate(h, dest); }
  // Replication decisions (see LDObjData::replicaGroup); 0 when the handle no
  // longer names the object the decision was made for.
  int Clone(LDObjHandle h, int dest, int rank) { return lbdb_obj->Clone(h, dest, rank); }
  int Retire(LDObjHandle h) { return lbdb_obj->Retire(h); }
  int CloneNotice(LDObjHandle h, int dest, int rank) { return lbdb_obj->CloneNotice(h, dest, rank); }
  int RetireNotice(LDObjHandle h, int rank) { return lbdb_obj->RetireNotice(h, rank); }

  // ---- Migration move ledger --------------------------------------------
  // One entry per move this PE must execute in the current balancer step,
  // keyed by the element id both sides know. Opened around the strategy's
  // decision loop (DistBaseLB::ProcessMigrationDecision); recorded from
  // CkLocRec::recvMigrate, the one point every LB-ordered move passes
  // through however the async machinery later defers it; retired by the
  // destination once the element has registered there. The PE's share of the
  // step's resume is held until the ledger drains, so the barrier waits on
  // the identity of every move -- deferred ones included -- instead of on
  // anonymous arrival counts that cannot detect a duplicate or name a
  // straggler.
  void ledgerOpen(int step);
  // Returns the step to tag the element's record with, or -1 when no ledger
  // is recording (a non-ledgered balancer path, or an app-driven move).
  int ledgerRecord(CmiUInt8 id, int toPe);
  // All moves are issued; run `done` once every entry has been retired
  // (immediately when the ledger is already empty).
  void ledgerClose(std::function<void()> done);
  // Entry method: the destination's ack that the element registered.
  void ledgerRetire(CmiUInt8 id, int step);
  // A recorded move that will never execute (the element was destroyed
  // before it left). Without this the step would wait forever.
  void ledgerRetireLocal(CmiUInt8 id, const char* why);
  void UnregisterOM(LDOMHandle omh) { lbdb_obj->UnregisterOM(omh); }
  void RegisteringObjects(LDOMHandle omh) { lbdb_obj->RegisteringObjects(omh); }
  void DoneRegisteringObjects(LDOMHandle omh)
  {
    lbdb_obj->DoneRegisteringObjects(omh);
  }
  void ObjectStart(const LDObjHandle& h) { lbdb_obj->ObjectStart(h); }
  void ObjectStop(const LDObjHandle& h) { lbdb_obj->ObjectStop(h); }
  void NonMigratable(LDObjHandle h) { lbdb_obj->NonMigratable(h); }
  void Migratable(LDObjHandle h) { lbdb_obj->Migratable(h); }
  void setPupSize(LDObjHandle h, size_t pup_size) { lbdb_obj->setPupSize(h, pup_size); }
  void setGPUPupSize(LDObjHandle h, size_t gpu_pup_size) { lbdb_obj->setGPUPupSize(h, gpu_pup_size); }
  void UseAsyncMigrate(LDObjHandle h, bool flag) { lbdb_obj->UseAsyncMigrate(h, flag); };
  // Live per-object load snapshot over CCS (handler "LBObjLoads" on PE 0):
  // every PE appends one record, the reply is int32 numPes followed by the
  // concatenated records; see collectObjLoads for the layout.
  void objLoadsRequest(const CcsDelayedReply& token);
  void collectObjLoads();
  void objLoadsCollected(CkReductionMsg* m);
  int GetObjDataSz(void) { return lbdb_obj->GetObjDataSz(); }
  int GetCommDataSz(void) { return lbdb_obj->GetCommDataSz(); }
  void GetObjData(LDObjData* data) { lbdb_obj->GetObjData(data); }
  void GetCommData(LDCommData* data) { lbdb_obj->GetCommData(data); }
  void GetCommInfo(int& bytes, int& msgs, int& withinbytes, int& outsidebytes,
                   int& num_nghbors, int& hops, int& hopbytes)
  {
    lbdb_obj->GetCommInfo(bytes, msgs, withinbytes, outsidebytes, num_nghbors, hops,
                          hopbytes);
  }
  void CollectStatsOn(void) { lbdb_obj->CollectStatsOn(); }
  void CollectStatsOff(void) { lbdb_obj->CollectStatsOff(); }
  bool StatsOn(void) { return lbdb_obj->StatsOn(); }
  void ObjTime(LDObjHandle h, double walltime, double cputime)
  {
    lbdb_obj->ObjTime(h, walltime, cputime);
  }
  void GetObjLoad(LDObjHandle& h, LBRealType& walltime, LBRealType& cputime)
  {
    lbdb_obj->GetObjLoad(h, walltime, cputime);
  };
  // The loads the last balancing step read for an object; the current window's
  // accumulators above are cleared when a step completes, these are not.
  void QueryKnownObjLoad(LDObjHandle& h, LBRealType& walltime, LBRealType& cputime)
  {
    lbdb_obj->QueryKnownObjLoad(h, walltime, cputime);
  }
  void QueryKnownObjGPULoad(LDObjHandle& h, LBRealType& gputime)
  {
    lbdb_obj->QueryKnownObjGPULoad(h, gputime);
  }
   void GetObjGPULoad(LDObjHandle& h, LBRealType& gputime)
  {
    lbdb_obj->GetObjGPULoad(h, gputime);
  };
  LBRealType GetTotalObjGPULoad(void) { return lbdb_obj->GetTotalObjGPULoad(); }
#if CMK_GPU
  void SetObjDriverLoad(
      const std::unordered_map<LDObjKey, double, LDObjKeyHash> &apiRaw, double scale)
  {
    lbdb_obj->SetObjDriverLoad(apiRaw, scale);
  }
#endif
  void SetObjGPULoad(
      const std::unordered_map<LDObjKey, double, LDObjKeyHash> &id_loadMap)
  {
    lbdb_obj->SetObjGPULoad(id_loadMap);
  }
#if CMK_GPU
  void SetObjGPUCosts(
      const std::unordered_map<LDObjKey, GpuObjectEpochCosts, LDObjKeyHash>
          &id_costMap)
  {
    lbdb_obj->SetObjGPUCosts(id_costMap);
  }
#endif
  void SetObjPosition(LDObjHandle h, const std::vector<LBRealType>& pos)
  {
    lbdb_obj->ObjPosition(h, pos);
  }
  const std::vector<LBRealType>& GetObjPosition(LDObjHandle h)
  {
    return lbdb_obj->GetObjPosition(h);
  }
  // Where an object was before its last migration, and the balancing step that
  // moved it; see LDObjData::prevPe.
  void SetObjPrev(LDObjHandle h, int pe, int stepNo) { lbdb_obj->ObjPrev(h, pe, stepNo); }
  void GetObjPrev(LDObjHandle h, int& pe, int& stepNo) { lbdb_obj->GetObjPrev(h, pe, stepNo); }
  // Replication declaration; see LDObjData::replicaGroup.
  void SetObjReplica(LDObjHandle h, int group, int rank, int count, int maxCount, size_t extraBytes,
                     double costNode = 0.0, double costRemote = 0.0)
  {
    lbdb_obj->ObjReplica(h, group, rank, count, maxCount, extraBytes, costNode, costRemote);
  }
  void GetObjReplica(LDObjHandle h, int& group, int& rank, int& count, int& maxCount, size_t& extraBytes)
  {
    lbdb_obj->GetObjReplica(h, group, rank, count, maxCount, extraBytes);
  }
  void* GetObjUserData(LDObjHandle& h) { return lbdb_obj->GetObjUserData(h); }
  void MetaLBCallLBOnChares() { lbdb_obj->MetaLBCallLBOnChares(); }
  void MetaLBResumeWaitingChares(int lb_period)
  {
    lbdb_obj->MetaLBResumeWaitingChares(lb_period);
  }
  void ClearLoads(void) { lbdb_obj->ClearLoads(); }
  const LDObjHandle& GetObjHandle(int idx) { return lbdb_obj->GetObjHandle(idx); }
  bool LiveObjHandle(int idx, CmiUInt8 id, LDObjHandle& h) { return lbdb_obj->LiveObjHandle(idx, id, h); }
  void IdleTime(LBRealType* walltime) { lbdb_obj->IdleTime(walltime); }
  void TotalTime(LBRealType* walltime, LBRealType* cputime)
  {
    lbdb_obj->TotalTime(walltime, cputime);
  }

  void GetTime(LBRealType* total_walltime, LBRealType* total_cputime,
               LBRealType* idletime, LBRealType* bg_walltime, LBRealType* bg_cputime)
  {
    lbdb_obj->GetTime(total_walltime, total_cputime, idletime, bg_walltime, bg_cputime);
  }
  void GetGPUBGTime(LBRealType* bg_gputime)
  {
    lbdb_obj->GetGPUBGTime(bg_gputime);
  }
  LDObjHandle RegisterObj(LDOMHandle omh, CmiUInt8 id, void* userPtr, int migratable)
  {
    return lbdb_obj->RegisterObj(omh, id, userPtr, migratable);
  }
  void UnregisterObj(LDObjHandle h) { lbdb_obj->UnregisterObj(h); }
  void EstObjLoad(const LDObjHandle& h, double cpuload)
  {
    lbdb_obj->EstObjLoad(h, cpuload);
  }
  void SetObjJoinedStep(const LDObjHandle& h, bool joined)
  {
    lbdb_obj->SetObjJoinedStep(h, joined);
  }
  void EstObjGPULoad(const LDObjHandle& h, double gputime)
  {
    lbdb_obj->EstObjGPULoad(h, gputime);
  }
  void BackgroundLoad(LBRealType* walltime, LBRealType* cputime)
  {
    lbdb_obj->BackgroundLoad(walltime, cputime);
  }
  void Send(const LDOMHandle& destOM, const CmiUInt8& destID, unsigned int bytes,
            int destObjProc, int force = 0)
  {
    lbdb_obj->Send(destOM, destID, bytes, destObjProc, force);
  }
  void MulticastSend(const LDOMHandle& _om, CmiUInt8* _ids, int _n, unsigned int _b,
                     int _nMsgs = 1)
  {
    lbdb_obj->MulticastSend(_om, _ids, _n, _b, _nMsgs);
  }
  int useMem()
  {
    int size = sizeof(LBManager);
    size += startLBFnList.size() * sizeof(StartLBCB);
    size += migrationDoneCBList.size() * sizeof(MigrationDoneCB);
    size += lbdb_obj->useMem();
    return size;
  }

#if CMK_LB_USER_DATA
  inline void* GetDBObjUserData(LDObjHandle& h, int idx)
  {
    return lbdb_obj->LbObj(h)->getDBUserData(idx);
  }
#endif

  void ResetAdaptive();

  void DumpDatabase(void);

  void reset();

  /*
   * Calls from load balancer to load database
   */
  template <typename T>
  inline int AddStartLBFn(T* obj, void (T::*method)(void))
  {
    return AddStartLBFn(std::bind(method, obj));
  }
  int AddStartLBFn(std::function<void()> fn);
  void TurnOnStartLBFn(int handle) { startLBFnList[handle]->on = 1; }
  void TurnOffStartLBFn(int handle) { startLBFnList[handle]->on = 0; }
  void RemoveStartLBFn(int handle);

  void StartLB();

  template <typename T>
  inline int AddMigrationDoneFn(T* obj, void (T::*method)(void))
  {
    return AddMigrationDoneFn(std::bind(method, obj));
  }
  int AddMigrationDoneFn(std::function<void()> fn);
  void RemoveMigrationDoneFn(int handle);
  void MigrationDone();
  // How many decisions (moves, copies, retirements, job-wide) the last
  // balancing round made, known on every PE before its MigrationDone callbacks
  // run; -1 before the first round. An application that holds work back
  // around a round for the sake of migrations can let it go when this is 0.
  void SetLastRoundDecisions(int n) { lastRoundDecisions = n; }
  int LastRoundDecisions() const { return lastRoundDecisions; }

 public:
  inline void TurnManualLBOn() { useBarrier = false; }
  inline void TurnManualLBOff() { useBarrier = true; }

  inline void PredictorOn(LBPredictorFunction* model)
  {
    if (predictCBFn != NULL)
      predictCBFn->on(model);
    else
      CmiPrintf("Predictor not supported in this load balancer\n");
  }
  inline void PredictorOn(LBPredictorFunction* model, int wind)
  {
    if (predictCBFn != NULL)
      predictCBFn->onWin(model, wind);
    else
      CmiPrintf("Predictor not supported in this load balancer\n");
  }
  inline void PredictorOff()
  {
    if (predictCBFn != NULL)
      predictCBFn->off();
    else
      CmiPrintf("Predictor not supported in this load balancer\n");
  }
  inline void ChangePredictor(LBPredictorFunction* model)
  {
    if (predictCBFn != NULL)
      predictCBFn->change(model);
    else
      CmiPrintf("Predictor not supported in this load balancer");
  }

  template <typename T>
  void SetupPredictor(T* data,
                      void (T::*on)(LBPredictorFunction*),
                      void (T::*onWin)(LBPredictorFunction*, int),
                      void (T::*off)(void),
                      void (T::*change)(LBPredictorFunction*))
  {
    if (predictCBFn == nullptr) predictCBFn = new PredictCB;
    predictCBFn->on = [=](LBPredictorFunction* fn) { (data->*on)(fn); };
    predictCBFn->onWin = [=](LBPredictorFunction* fn, int win) { (data->*onWin)(fn, win); };
    predictCBFn->off = [=]() { (data->*off)(); };
    predictCBFn->change = [=](LBPredictorFunction* fn) { (data->*change)(fn); };
  }

  inline int CollectingCommStats(void)
  {
    return lbdb_obj->CollectingStats() && _lb_args.traceComm();
  };

  void Migrated(LDObjHandle h, int waitBarrier = 1);

  LDBarrierClient AddLocalBarrierClient(Chare* obj, std::function<void()> fn);
  template <typename T>
  inline LDBarrierClient AddLocalBarrierClient(T* obj, void (T::*method)(void))
  {
    return AddLocalBarrierClient((Chare*)obj, std::bind(method, obj));
  }

  void RemoveLocalBarrierClient(LDBarrierClient h);

  LDBarrierReceiver AddLocalBarrierReceiver(std::function<void()> fn);
  template <typename T>
  inline LDBarrierReceiver AddLocalBarrierReceiver(T* obj, void (T::*method)(void))
  {
    return AddLocalBarrierReceiver(std::bind(method, obj));
  }
  void RemoveLocalBarrierReceiver(LDBarrierReceiver h);
  void AtLocalBarrier(LDBarrierClient _n_c);
  void TurnOnBarrierReceiver(LDBarrierReceiver h);
  void TurnOffBarrierReceiver(LDBarrierReceiver h);

  void LocalBarrierOn(void);
  void LocalBarrierOff(void);
  void setAsyncMode(bool on);   // entry: see CkLBSetAsync
  void ResumeClients();
  static int ProcessorSpeed();
  static int ProcessorGPUSpeed();
  static void SetLBPeriod(double period)
  {
    _lb_args.lbperiod() = period;
    // If the manager has been initialized, then start the timer
    if (auto* const obj = LBManager::Object()) obj->setTimer();
  }
  static double GetLBPeriod() { return _lb_args.lbperiod(); }

  void SetMigrationCost(double cost);
  void SetStrategyCost(double cost);
  void UpdateDataAfterLB(double mLoad, double mCpuLoad, double avgLoad);

 private:
  int mystep;
  static std::vector<char> avail_vector; // processor bit vector
  static bool avail_vector_set;
  int new_ld_balancer;  // for Node 0
  MetaBalancer* metabalancer;
  int currentLBIndex;
  bool isPeriodicQueued = false;

 public:
  std::vector<BaseLB*> loadbalancers;

  std::vector<StartLBCB*> startLBFnList;
  std::vector<MigrationDoneCB*> migrationDoneCBList;
  int lastRoundDecisions = -1;

 public:
  BaseLB** getLoadBalancers() { return loadbalancers.data(); }
  int getNLoadBalancers() { return loadbalancers.size(); }

 public:
  static bool manualOn;

 public:
  const char *availVector() const { return avail_vector.data(); }
  void get_avail_vector(char * bitmap) const;
  void get_avail_vector(std::vector<char> & bitmap) const { bitmap = avail_vector; }
  void set_avail_vector(const char * bitmap, int new_ld = -1);
  void set_avail_vector(const std::vector<char> & bitmap, int new_ld = -1);
  int& new_lbbalancer() { return new_ld_balancer; }

  struct LastLBInfo
  {
    LBRealType* expectedLoad;
    LastLBInfo();
  };
  LastLBInfo lastLBInfo;
  inline LBRealType myExpectedLoad() { return lastLBInfo.expectedLoad[CkMyPe()]; }
  inline LBRealType* expectedLoad() { return lastLBInfo.expectedLoad; }

  int getLoadbalancerTicket();
  void addLoadbalancer(BaseLB* lb, int seq);
  void nextLoadbalancer(int seq);
  void switchLoadbalancer(int switchFrom, int switchTo);
  const char* loadbalancer(int seq);

  inline int step() { return mystep; }
  int lbEventsDone;   // balancing events completed here (see nextLoadbalancer)
  inline void incStep() { mystep++; }
};

void TurnManualLBOn();
void TurnManualLBOff();

void LBTurnPredictorOn(LBPredictorFunction* model);
void LBTurnPredictorOn(LBPredictorFunction* model, int wind);
void LBTurnPredictorOff();
void LBChangePredictor(LBPredictorFunction* model);

// This alias remains for backwards compatibility
CMK_DEPRECATED_MSG("Use LBManager::SetLBPeriod instead of LBSetPeriod")
void LBSetPeriod(double period);

#if CMK_LB_USER_DATA
int LBRegisterObjUserData(int size);
#endif

CLINKAGE void LBTurnInstrumentOn();
CLINKAGE void LBTurnInstrumentOff();
void LBTurnCommOn();
void LBTurnCommOff();
void LBClearLoads();

inline LBManager* LBManagerObj() { return LBManager::Object(); }

inline void CkStartLB() { LBManager::Object()->StartLB(); }

inline void get_avail_vector(std::vector<char> & bitmap) { LBManagerObj()->get_avail_vector(bitmap); }

inline void set_avail_vector(const std::vector<char> & bitmap) { LBManagerObj()->set_avail_vector(bitmap); }

//  a helper class to suspend/resume load instrumentation when calling into
//  runtime apis

class SystemLoad
{
  const LDObjHandle* objHandle;
  LBManager* lbmgr;

 public:
  SystemLoad();

  ~SystemLoad()
  {
    if (objHandle) lbmgr->ObjectStart(*objHandle);
  }
};

#define CK_RUNTIME_API SystemLoad load_entry;

#endif /* LDATABASE_H */

/*@}*/
