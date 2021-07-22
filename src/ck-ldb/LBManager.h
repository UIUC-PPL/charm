/**
 * \addtogroup CkLdb
 */
/*@{*/

#ifndef LBMANAGER_H
#define LBMANAGER_H

#include "LBDatabase.h"
#include "json_fwd.hpp"
using json = nlohmann::json;

#define LB_FORMAT_VERSION 3

#define LB_MANAGER_VERSION 1

class MetaBalancer;
extern int _lb_version;

// command line options
class CkLBArgs
{
 private:
  double _autoLbPeriod;  // in seconds
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
  int _lb_central_pe;      // processor number for centralized strategy
  int _lb_maxDistPhases;   // Specifies the max number of LB phases in DistributedLB
  double _lb_targetRatio;  // Specifies the target load ratio for LBs that aim for a
                           // particular load ratio
  bool _lb_metaLbOn;
  char* _lb_metaLbModelDir;
  char* _lb_treeLBFile = (char*)"treelb.json";

 public:
  CkLBArgs()
  {
    _autoLbPeriod = -1.0;  // off by default
    _lb_debug = 0;
    _lb_ignoreBgLoad = _lb_syncResume = _lb_useCpuTime = false;
    _lb_printsummary = _lb_migObjOnly = false;
    _lb_statson = _lb_traceComm = true;
    _lb_loop = false;
    _lb_central_pe = 0;
    _lb_maxDistPhases = 10;
    _lb_targetRatio = 1.05;
    _lb_metaLbOn = false;
    _lb_metaLbModelDir = nullptr;
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
  inline int& central_pe() { return _lb_central_pe; }
  inline double& alpha() { return _lb_alpha; }
  inline double& beta() { return _lb_beta; }
  inline int& maxDistPhases() { return _lb_maxDistPhases; }
  inline double& targetRatio() { return _lb_targetRatio; }
  inline bool& metaLbOn() { return _lb_metaLbOn; }
  inline char*& metaLbModelDir() { return _lb_metaLbModelDir; }
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

// used in constructor of all load balancers
class CkLBOptions
{
 private:
  int seqno;  // for centralized lb, the seqno
  const char* legacyName;
 public:
  CkLBOptions() : seqno(-1), legacyName(nullptr) {}
  CkLBOptions(int s) : seqno(s), legacyName(nullptr) {}
  CkLBOptions(int s, const char* legacyName) : seqno(s), legacyName(legacyName) {}
  int getSeqNo() const { return seqno; }
  const char* getLegacyName() const { return legacyName; }
};
PUPbytes(CkLBOptions)

#include "LBManager.decl.h"

    extern CkGroupID _lbmgr;

CkpvExtern(bool, lbmanagerInited);

// LB options, mostly controled by user parameter
extern char* _lbtopo;

typedef void (*LBCreateFn)(const CkLBOptions&);
typedef BaseLB* (*LBAllocFn)();
void LBDefaultCreate(LBCreateFn f);

void LBRegisterBalancer(std::string, LBCreateFn, LBAllocFn, std::string, bool shown = true);

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

  bool useBarrier;            // use barrier or not

  PredictCB* predictCBFn;

  int startLBFn_count;

 public:
  int chare_count;

  LBManager(void) { init(); }
  LBManager(CkMigrateMessage* m) : CBase_LBManager(m) { init(); }
  ~LBManager() { delete lbdb_obj; }

 private:
  void init();
  void InvokeLB();

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
  void UseAsyncMigrate(LDObjHandle h, bool flag) { lbdb_obj->UseAsyncMigrate(h, flag); };
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
  void* GetObjUserData(LDObjHandle& h) { return lbdb_obj->GetObjUserData(h); }
  void MetaLBCallLBOnChares() { lbdb_obj->MetaLBCallLBOnChares(); }
  void MetaLBResumeWaitingChares(int lb_period)
  {
    lbdb_obj->MetaLBResumeWaitingChares(lb_period);
  }
  void ClearLoads(void) { lbdb_obj->ClearLoads(); }
  const LDObjHandle& GetObjHandle(int idx) { return lbdb_obj->GetObjHandle(idx); }
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
  LDObjHandle RegisterObj(LDOMHandle omh, CmiUInt8 id, void* userPtr, int migratable)
  {
    return lbdb_obj->RegisterObj(omh, id, userPtr, migratable);
  }
  void UnregisterObj(LDObjHandle h) { lbdb_obj->UnregisterObj(h); }
  void EstObjLoad(const LDObjHandle& h, double cpuload)
  {
    lbdb_obj->EstObjLoad(h, cpuload);
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
  void ResumeClients();
  static int ProcessorSpeed();
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
