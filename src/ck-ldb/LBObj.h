/**
 * \addtogroup CkLdb
*/
/*@{*/

#ifndef LBOBJ_H
#define LBOBJ_H

#include "lbdb.h"

class LBDatabase;

class LBObj
{
friend class LBDatabase;

public:
  LBObj(const LDObjHandle &_h, void *usr_ptr = NULL, bool _migratable=true, bool _asyncArrival = false) {
    data.handle = _h;
    data.migratable = _migratable;
    data.asyncArrival = _asyncArrival;
    // Unknown until the application declares them (setPupSize/setGPUPupSize);
    // none of the examples does. Left uninitialised these reached the cost
    // model as garbage -- 10^16 bytes per object, a migration priced at 10^8
    // seconds -- and every priced decision refused to move anything.
    data.pupSize = 0;
#if CMK_GPU
    data.gpuPupSize = 0;
#endif
    // Not replicable until the application declares it (setLBReplica).
    data.replicaGroup = -1;
    data.replicaRank = 0;
    data.replicaCount = 1;
    data.replicaMax = 1;
    data.replicaExtraBytes = 0;
    data.replicaCostNode = data.replicaCostRemote = 0.0;
    Clear();
    localUserData = usr_ptr;
  }

  ~LBObj() { };

  void Clear(void);
  bool joinedStep = false;
  // When this object's measurement window last opened, or -1 while it is
  // closed. Its open time accumulates into data.windowTime (see there).
  double windowOpenedAt = -1.0;
  // The span this interval's load is extrapolated over: from the object's
  // first activity after ClearLoads (activeSince, set by StartTimer) to its
  // join (closedAt), or to now while it is still open. Under +LBAsync an
  // object keeps computing through the lag with its window closed, so the
  // span is the whole interval and the window a fraction of it; under the
  // barrier it joins and then waits idle, so span and window coincide and
  // nothing is scaled. Wall time since ClearLoads would count that idle wait
  // as unmeasured work and inflate exactly the light objects (leanmd sync
  // 903 -> 994 ms/step, job 22173198).
  double activeSince = -1.0;
  double closedAt = -1.0;

  void IncrementTime(LBRealType walltime, LBRealType cputime);
  void IncrementGPUTime(LBRealType walltime);

  // Has this object joined the current load balancing step? While it has, its
  // work is no longer billed to it: the strategy is about to read (or has
  // already read) the load it accumulated up to the join, and anything after
  // that belongs to the next round. Under the unsplit barrier this is never
  // true for long, because a joined element parks and does no work. Under
  // +LBAsync it keeps running, and charging that work inflated the measured
  // load of exactly the elements that reached the barrier first.
  //
  // Per object, deliberately: the PE-wide switch stops measuring elements that
  // have NOT joined yet, which biases the late ones the other way.
  inline void setJoinedStep(bool v) {
    if (v && !joinedStep) {            // closing: bank the time it was open
      const double now = CkWallTimer();
      if (windowOpenedAt >= 0.0) data.windowTime += now - windowOpenedAt;
      windowOpenedAt = -1.0;
      closedAt = now;
    } else if (!v && joinedStep) {     // opening
      windowOpenedAt = CkWallTimer();
      closedAt = -1.0;
      declaredWhileJoined = false;
    }
    joinedStep = v;
  }
  inline bool hasJoinedStep(void) const { return joinedStep; }
  // The window time so far this interval, the open tail included.
  inline double windowTime() const {
    return data.windowTime + (windowOpenedAt >= 0.0 ? CkWallTimer() - windowOpenedAt : 0.0);
  }
  // A new interval starts: nothing banked, and a window that is open restarts
  // its clock here, where the interval's own clock restarts (ClearLoads).
  inline void clearWindow(double now) {
    data.windowTime = 0.0;
    if (windowOpenedAt >= 0.0) windowOpenedAt = now;
    activeSince = -1.0;
    closedAt = -1.0;
  }
  // The span this interval's sample stands for; see activeSince.
  inline double activeSpan() const {
    if (activeSince < 0.0) return 0.0;
    const double end = closedAt >= 0.0 ? closedAt : CkWallTimer();
    return end > activeSince ? end - activeSince : 0.0;
  }
  // activeSpan / window: the factor that turns this object's sampled load and
  // traffic into a rate over the time it was actually active. 1 when the
  // window covers the span; capped, since a window seen for a tenth of the
  // span supports no finer an extrapolation than that.
  static constexpr double kMaxSampleScale = 10.0;
  inline double sampleScale() const {
    const double w = windowTime();
    const double span = activeSpan();
    if (w <= 0.0 || span <= w) return 1.0;
    const double f = span / w;
    return f > kMaxSampleScale ? kMaxSampleScale : f;
  }

  // The application declared this object's device load for the current
  // interval (EstObjGPULoad), so the CUPTI attribution must not replace it.
  // An application that declares knows its cost before the work runs -- moe
  // prices an expert at its token count times a measured rate -- and the
  // attribution can be far off it: measured on moe, CUPTI credited the
  // experts with 17% of the interval while the devices were 68-77% busy.
  // Cleared with the loads at each balancing step, so an object that stops
  // declaring goes back to being measured.
  bool gpuDeclared = false;
  inline void setGPUDeclared(bool v) { gpuDeclared = v; }
  inline bool hasGPUDeclared(void) const { return gpuDeclared; }
  // A declaration arrived while the window was closed (LBDatabase::EstObjGPULoad); cleared when it reopens.
  bool declaredWhileJoined = false;

  inline void StartTimer(void) {
    startWTime = CkWallTimer();
    if (activeSince < 0.0) activeSince = startWTime;   // first activity this interval
#if CMK_LB_CPUTIMER
    startCTime = CkCpuTimer();
#endif
  }

  inline void StopTimer(LBRealType* walltime, LBRealType* cputime) {
    if (startWTime >= 0.0) {	// in case startOn in middle of entry
      const double endWTime = CkWallTimer();
      *walltime = endWTime - startWTime;
#if CMK_LB_CPUTIMER
      const double endCTime = CkCpuTimer();
      *cputime = endCTime - startCTime;
#else
      *cputime = *walltime;
#endif
	  }
    else {
      *walltime = *cputime = 0.0;
    }
  }

  inline void getTime(LBRealType *w, LBRealType *c) {
    *w = data.wallTime;
#if CMK_LB_CPUTIMER
    *c = data.cpuTime;
#else
    *c = *w;
#endif
  }

  inline void getGPUTime(LBRealType *w) {
  #if CMK_GPU
    *w = data.gpuTime;
  #else
    CmiAbort("LBObj::getGPUTime called but CMK_GPU is not set");
  #endif
  }

#if CMK_GPU
  // The host time this object spent inside the CUDA driver is not host work:
  // it comes out of wallTime, what the per-PE dimension balances, and its
  // share of the process's driver busy time goes in as the launch load.
  // Returns what came out, which is at most what was there.
  inline LBRealType takeApiTime(LBRealType raw) {
    const LBRealType taken = (raw < data.wallTime) ? raw : data.wallTime;
    data.wallTime -= taken;
#if CMK_LB_CPUTIMER
    data.cpuTime = (data.cpuTime > taken) ? data.cpuTime - taken : 0;
#endif
    return taken;
  }
  inline void setDriverTiming(LBRealType t) { data.driverTime = t; }
#endif

  inline void setTiming(LBRealType cputime)
  {
    data.wallTime = cputime;
#if CMK_LB_CPUTIMER
    data.cpuTime = cputime;
#endif
  }

  // Object position, for geometric load balancers (DiffusionLB's centroid
  // metric). Empty unless the application calls CkMigratable::setObjPosition,
  // so every reader has to cope with a zero-length vector.
  inline void setPosition(const std::vector<LBRealType>& pos)
  {
    data.position = pos;
  }

  inline const std::vector<LBRealType>& getPosition()
  {
    return data.position;
  }

  // Previous home and the step that moved the object away from it; see
  // LDObjData::prevPe.
  inline void setPrev(int pe, int stepNo)
  {
    data.prevPe = pe;
    data.prevStep = stepNo;
  }
  inline void getPrev(int& pe, int& stepNo) const
  {
    pe = data.prevPe;
    stepNo = data.prevStep;
  }

  // Replication declaration; see LDObjData::replicaGroup.
  inline void setReplica(int group, int rank, int count, int maxCount, size_t extraBytes,
                         double costNode = 0.0, double costRemote = 0.0)
  {
    data.replicaGroup = group;
    data.replicaRank = rank;
    data.replicaCount = count;
    data.replicaMax = maxCount;
    data.replicaExtraBytes = extraBytes;
    data.replicaCostNode = costNode;
    data.replicaCostRemote = costRemote;
  }
  inline void getReplica(int& group, int& rank, int& count, int& maxCount, size_t& extraBytes) const
  {
    group = data.replicaGroup;
    rank = data.replicaRank;
    count = data.replicaCount;
    maxCount = data.replicaMax;
    extraBytes = data.replicaExtraBytes;
  }

  inline void setGPUTiming(LBRealType gputime)
  {
  #if CMK_GPU
    data.gpuTime = gputime;
  #else
    CmiAbort("LBObj::setGPUTiming called but CMK_GPU is not set");
  #endif
  }

#if CMK_GPU
  inline void setGPUCosts(const GpuObjectEpochCosts &costs) { data.gpuCosts = costs; }
  inline void clearGPUCosts() { data.gpuCosts.clear(); }
  inline const GpuObjectEpochCosts &getGPUCosts() const { return data.gpuCosts; }
#endif

  inline LDOMHandle &parentOM() { return data.handle.omhandle; }
  inline const LDObjHandle &GetLDObjHandle() const { return data.handle; }
  inline void SetMigratable(bool mig) { data.migratable = mig; }
  inline void setPupSize(size_t obj_pup_size) {
    data.pupSize = pup_encodeSize(obj_pup_size);
  }
  inline void setGPUPupSize(size_t obj_gpu_pup_size){
  #if CMK_GPU
    data.gpuPupSize = obj_gpu_pup_size;
  #else
    CmiAbort("LBObj::setGPUPupSize called but CMK_GPU is not set");
  #endif
  }
  
  inline void UseAsyncMigrate(bool async) { data.asyncArrival = async; }
  inline LDObjData &ObjData() { return data; };
  inline void lastKnownLoad(LBRealType *w, LBRealType *c) {
    *w = lastWallTime;
#if CMK_LB_CPUTIMER
    *c = lastCpuTime;
#else
    *c = *w;
#endif
  }
  // The GPU load the last balancing step read for this object (set at
  // ClearLoads, when the interval's figure is complete); 0 before the first.
  inline void lastKnownGPULoad(LBRealType *g) { *g = lastGPUTime; }
  inline void *getLocalUserData() { return  localUserData; }
#if CMK_LB_USER_DATA
  inline void *getDBUserData(int idx) { return  data.getUserData(idx); }
#endif
private:

  void *localUserData;               // local user data, not in database
//  LDOMHandle parentOM;
//  LDObjHandle myhandle;
  LDObjData data;
//  bool registered;
  double startWTime;             // needs double precision
  LBRealType lastWallTime;
  LBRealType lastGPUTime = 0.0;
#if CMK_LB_CPUTIMER
  double startCTime;
  LBRealType lastCpuTime;
#endif
//  bool migratable;   // temp
};

#endif

/*@}*/
