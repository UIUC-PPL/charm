#ifndef __TRACE_PERF__H__
#define __TRACE_PERF__H__
#include "charm++.h"
#include "TopoManager.h"
#include "envelope.h"
#include "trace-common.h"
#include "picsdefs.h"
#include "picsdefscpp.h"
#include "picsautoperf.h"
#include <map>

#define COMPRESS_EVENT_NO         392
#define DECOMPRESS_EVENT_NO       393

class TraceAutoPerf : public Trace {

  bool isTraceOn;

  TopoManager tmgr;

  ObjectLoadMap_t objectLoads;
#if CMK_HAS_COUNTER_PAPI
  LONG_LONG_PAPI previous_papiValues[NUMPAPIEVENTS];
  LONG_LONG_PAPI papiValues[NUMPAPIEVENTS];
#endif
  double  lastBeginExecuteTime;
  int     lastbeginMessageSize;
  int     lastEvent;
  /** The start of the idle region */
  double  lastBeginIdle;
  int     numNewObjects;

  /** Amount of time spent so far in untraced regions */
  double totalUntracedTime;

  /** When tracing was suspended (0 if not currently suspended) */
  double whenStoppedTracing;

  /** The amount of time spent executing entry methods since we last reset the counters */
  double totalEntryMethodTime;
  double totalEntryMethodTime_1;
  double totalEntryMethodTime_2;

  double appWorkStartTimer;
  /** the amount of application useful work, need app knowledge */
  double totalAppTime;
  double tuneOverheadTotalTime;

  double startTimer;

  double tuneOverheadStartTimer;

  /** The amount of time spent idle since we last reset the counters */
  double totalIdleTime;

  /* * maximum excution time of a single entry method */
  double maxEntryTime;
  double maxEntryTime_1;
  double maxEntryTime_2;
  int    maxEntryIdx;
  int    maxEntryIdx_1;
  int    maxEntryIdx_2;

  /*  maximum execution time of a single object  */
  /*  obj load map */
  void *currentObject;

  int currentEP;

  int currentAID;
  int currentIDX;

  /** The highest seen memory usage  since we last reset the counters */
  double memUsage;

  /** The number of entry method invocations since we last reset the counters */
  long totalEntryMethodInvocations;
  long totalEntryMethodInvocations_1;
  long totalEntryMethodInvocations_2;

  /** The time we last rest the counters */
  double lastResetTime;

  double phaseEndTime;

  /* * summary data */
  PerfData *currentSummary;
  PerfData *currentTraceData;

  int currentGroupID;
  CkArrayIndex currentIndex;

  // In some programs like Changa, entry methods may be nested, and hence we only want to consider the outermost one
  int nesting_level;

public:
  TraceAutoPerf(char **argv);

  //begin/end tracing
  void traceBegin(void);
  void traceEnd(void);


  // a user event has just occured
  void userEvent(int eventID);
  // a pair of begin/end user event has just occured
  void userBracketEvent(int eventID, double bt, double et);
  void beginAppWork();
  void endAppWork();
  void countNewChare();

  void beginTuneOverhead();
  void endTuneOverhead();
  // "creation" of message(s) - message Sends
  void creation(envelope *, int epIdx, int num=1);
  void creationMulticast(envelope *, int epIdx, int num=1, const int *pelist=NULL);
  void creationDone(int num=1);

  void messageRecv(void *env, int pe);
  void messageSend(void *env, int pe, int size);

  void beginExecute(envelope *, void*);
  void beginExecute(CmiObjId *tid);

  void beginExecute(
    envelope* env,
    int event,   // event type defined in trace-common.h
    int msgType, // message type
    int ep,      // Charm++ entry point id
    int srcPe,   // Which PE originated the call
    int ml,      // message size
    CmiObjId* idx);    // index

  void beginExecute(
    int event,   // event type defined in trace-common.h
    int msgType, // message type
    int ep,      // Charm++ entry point id
    int srcPe,   // Which PE originated the call
    int ml,      // message size
    CmiObjId* idx,
    void* obj);    // index
  void endExecute(void);

  // begin/end idle time for this pe
  void beginIdle(double curWallTime);
  void endIdle(double curWallTime);

  // begin/end of execution
  void beginComputation(void);
  void endComputation(void);

  /* Memory tracing */
  void malloc(void *where, int size, void **stack, int stackSize);
  void free(void *where, int size);

  // do any clean-up necessary for tracing
  void traceClose();

  // ==================================================================
  /** reset the idle time and entry method execution time accumulators */
  void resetTimings();
  /** Reset the idle, overhead, and memory measurements */
  void resetAll();
  void endPhase();
  void startPhase(int step, int id);
  void startStep(bool analysis);
  void endStep(bool analysis);

  /** Fraction of the time spent idle since resetting the counters */

  inline double idleRatio(){
    if(lastEvent == BEGIN_IDLE)
      totalIdleTime += (CkWallTimer() - lastBeginIdle);
    return (totalIdleTime) / totalTraceTime();
  }

  inline double idleTime()
  {
    if(lastEvent == BEGIN_IDLE)
      totalIdleTime += (CkWallTimer() - lastBeginIdle);
    return totalIdleTime;
  }

  inline double untracedTime(){
    if(whenStoppedTracing <= 0){
      return totalUntracedTime;
    } else {
      return totalUntracedTime + (phaseEndTime -whenStoppedTracing);
    }
  }

  inline double totalTraceTime()
  {
    return CkWallTimer() - startTimer;
  }
  /** Fraction of time spent as overhead since resetting the counters */
  inline double overheadRatio(){
    double t = totalTraceTime();
    return (t - totalIdleTime - totalEntryMethodTime)/t;
  }

  inline double overheadTime(){
    double t = totalTraceTime();
    return (t - totalIdleTime - totalEntryMethodTime);
  }

  inline double utilRatio() {
    double inprogress_time = 0.0;
    if(lastEvent == BEGIN_PROCESSING)
      inprogress_time = (CkWallTimer() - lastBeginExecuteTime);
    return (totalEntryMethodTime + inprogress_time)/ totalTraceTime();
  }

  inline double utilTime() {
    double inprogress_time = 0.0;
    if(lastEvent == BEGIN_PROCESSING)
      inprogress_time = (CkWallTimer() - lastBeginExecuteTime);
    return (totalEntryMethodTime + inprogress_time);
  }

  inline double appRatio() {
    return totalAppTime/ totalTraceTime();
  }

  inline double appTime() {
    return totalAppTime;
  }
  /** Highest memory usage (in MB) value we've seen since resetting the counters */
  inline double memoryUsageMB(){
    return ((double)memUsage) / 1024.0 / 1024.0;
  }

  /** Determine the average grain size since last reset of counters */
  inline double grainSize(){
    return (double)totalEntryMethodTime / totalEntryMethodInvocations;
  }

  inline double maxGrainSize() {
    return maxEntryTime;
  }

  void summarizeObjectInfo(double &maxtime, double &totaltime, double &mintime, double &maxMsgCount,
                           double &totalMsgCount, double &maxMsgSize, double &totalMsgSize,
                           double &numObjs, double &maxBytesPerMsg, double &minBytesPerMsg) ;


  inline long numInvocations() {
    return totalEntryMethodInvocations;
  }

#if CMK_HAS_COUNTER_PAPI
  inline void readPAPI()
  {
    if (PAPI_read(CkpvAccess(papiEventSet), CkpvAccess(papiValues)) != PAPI_OK) {
      CmiAbort("PAPI failed to read at begin execute!\n");
    }
  }
#endif

  PerfData* getSummary();
  void printSummary();

  void setTraceOn(bool b) {
    isTraceOn = b;
  }

  bool getTraceOn() { return isTraceOn;}
};


TraceAutoPerf* localAutoPerfTracingInstance();

#endif
