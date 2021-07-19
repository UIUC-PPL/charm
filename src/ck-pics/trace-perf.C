#include "trace-perf.h"
#include <stdlib.h>
CkpvStaticDeclare(TraceAutoPerf*, _trace);

TraceAutoPerf *localAutoPerfTracingInstance()
{
  return CkpvAccess(_trace);
}

TraceAutoPerf::TraceAutoPerf(char **argv) 
{
    currentSummary = currentTraceData = (PerfData*)::malloc(sizeof(PerfData) );
    memset(currentSummary, 0, sizeof(PerfData));
    resetAll();
    nesting_level = 0;
    whenStoppedTracing = 0;
#if CMK_HAS_COUNTER_PAPI
    initPAPI();
#endif
    if (CkpvAccess(traceOnPe) == 0) return;
}

void TraceAutoPerf::startStep(bool newAnalysis) {
  if(isTraceOn){
    if(newAnalysis) {
      CkpvAccess(perfDatabase)->advanceStep();
      currentSummary = currentTraceData = CkpvAccess(perfDatabase)->getCurrentPerfData();
    }
  }
}

void TraceAutoPerf::startPhase(int step, int phaseId) {
  if(isTraceOn){
    currentSummary = currentTraceData + step*CkpvAccess(numOfPhases) +  phaseId;
    resetAll(); 
  }
}

void TraceAutoPerf::endPhase() {
  if(isTraceOn){
    getSummary();
  }
}

void TraceAutoPerf::endStep( bool newAnalysis) {
  if(isTraceOn){
    if(newAnalysis)
      CkpvAccess(perfDatabase)->endCurrent();
  }
}

void TraceAutoPerf::resetTimings(){
}

void TraceAutoPerf::resetAll(){
  ObjectLoadMap_t::iterator  iter;
  double curTimer = CkWallTimer();
  totalIdleTime = 0.0;
  totalEntryMethodTime = 0.0;
  totalEntryMethodTime_1 = 0.0;
  totalEntryMethodTime_2 = 0.0;
  totalAppTime = 0.0;
  tuneOverheadTotalTime = 0.0;
  maxEntryTime = 0;
  maxEntryTime_1 = 0;
  maxEntryTime_2 = 0;
  totalEntryMethodInvocations = 0;
  totalEntryMethodInvocations_1 = 0;
  totalEntryMethodInvocations_2 = 0;
  startTimer = lastBeginIdle = lastBeginExecuteTime = lastResetTime = curTimer;
  totalUntracedTime = 0;
  numNewObjects = 0;
  objectLoads.clear();
  if(whenStoppedTracing !=0){
    whenStoppedTracing = curTimer;
  }
#if CMK_HAS_COUNTER_PAPI
  memcpy(previous_papiValues, papiValues, sizeof(LONG_LONG_PAPI)*NUMPAPIEVENTS);
#endif
}

void TraceAutoPerf::traceBegin(void){
  if(isTraceOn){
    if(whenStoppedTracing != 0)
      totalUntracedTime += (CkWallTimer() - whenStoppedTracing);
    whenStoppedTracing = 0;
  }
}

void TraceAutoPerf::traceEnd(void){
  if(isTraceOn){
    CkAssert(whenStoppedTracing == 0); // can't support nested traceEnds on one processor yet...
    whenStoppedTracing = CkWallTimer();
  }
}

void TraceAutoPerf::userEvent(int eventID) { }

void TraceAutoPerf::userBracketEvent(int eventID, double bt, double et) { 
  if(isTraceOn){
    if(eventID == DECOMPRESS_EVENT_NO || eventID == COMPRESS_EVENT_NO) 
    {
      currentSummary->data[AVG_CompressTime] += (et-bt);
    }
  }
}

void TraceAutoPerf::beginTuneOverhead()
{
  if(isTraceOn){
    tuneOverheadStartTimer = CkWallTimer(); 
  }
}

void TraceAutoPerf::endTuneOverhead()
{
  if(isTraceOn){
    tuneOverheadTotalTime += (CkWallTimer() - tuneOverheadStartTimer);
  }
}

void TraceAutoPerf::beginAppWork() 
{
  if(isTraceOn){
    appWorkStartTimer = CkWallTimer();
  }
}

void TraceAutoPerf::endAppWork() 
{
  if(isTraceOn){
    totalAppTime += (CkWallTimer() - appWorkStartTimer);
  }
}

void TraceAutoPerf::countNewChare() 
{
  if(isTraceOn){
    numNewObjects++;
  }
}

void TraceAutoPerf::creation(envelope *env, int epIdx, int num) { 
} 

void TraceAutoPerf::creationMulticast(envelope *, int epIdx, int num, const int *pelist) { }

void TraceAutoPerf::creationDone(int num) { }

void TraceAutoPerf::messageRecv(void *env, int size) {
  if(isTraceOn){
    currentSummary->data[AVG_NumMsgRecvPerPE]++;
    currentSummary->data[AVG_BytesMsgRecvPerPE] += size;
  }
}

void TraceAutoPerf::messageSend(void *env, int pe, int size) {
  if(isTraceOn){
    currentSummary->data[MAX_BytesSend] += size;
    currentSummary->data[MAX_BytesSendPE] = CkMyPe();
    currentSummary->data[MAX_NumMsgSend] += 1;
    currentSummary->data[MAX_NumMsgSendPE] = CkMyPe();
  }
}

void TraceAutoPerf::beginExecute(CmiObjId *tid)
{
  if(isTraceOn){
    lastBeginExecuteTime = CkWallTimer();
    lastEvent =  BEGIN_PROCESSING;
    lastbeginMessageSize = 0;
    currentObject = tid;
    currentEP = 0;
  }
}

void TraceAutoPerf::beginExecute(envelope *env, void *obj)
{
  if(isTraceOn){
    lastBeginExecuteTime = CkWallTimer();
    lastEvent =  BEGIN_PROCESSING;
    lastbeginMessageSize = env->getTotalsize();
    currentObject = obj;
    currentEP = env->getEpIdx();
#if USE_MIRROR
    if(_entryTable[currentEP]->mirror){
      currentAID = env->getArrayMgr().idx;
    }
#endif
  }
}

void TraceAutoPerf::beginExecute(envelope *env, int event,int msgType,int ep,
    int srcPe, int mlen, CmiObjId *idx)
{
  if(isTraceOn){
    lastbeginMessageSize = env->getTotalsize();
    lastBeginExecuteTime = CkWallTimer();
    lastEvent =  BEGIN_PROCESSING;
    currentEP = ep; 
#if USE_MIRROR
    if(_entryTable[currentEP]->mirror){
      currentAID = env->getArrayMgr().idx;
      currentIDX = env->getsetArrayIndex().getCombinedCount();
    }
#endif
  }
}

void TraceAutoPerf::beginExecute(int event,int msgType,int ep,int srcPe,
    int mlen, CmiObjId *idx, void *obj)
{
  if(isTraceOn){
    lastBeginExecuteTime = CkWallTimer();
    lastbeginMessageSize = mlen;
    lastEvent =  BEGIN_PROCESSING;
    currentObject = obj;
    currentEP = ep; 
  }
}

void TraceAutoPerf::endExecute(void)
{
  if(isTraceOn){
    double endTime = CkWallTimer() ;
    double executionTime = endTime - lastBeginExecuteTime;
    lastEvent =  -1;
    totalEntryMethodTime += executionTime;
    totalEntryMethodInvocations ++;
    if(executionTime > maxEntryTime) {
      maxEntryTime = executionTime;
      maxEntryIdx = currentEP;
    }
    
    {
      ObjectLoadMap_t::iterator  iter;
      iter = objectLoads.find(currentObject);
      if(iter == objectLoads.end())
      {
        ObjInfo  *myobjInfo = new ObjInfo(executionTime, 1, lastbeginMessageSize);
        objectLoads[currentObject] = myobjInfo;
      }else
      {
        iter->second->executeTime += executionTime;
        iter->second->msgCount += 1;
        iter->second->msgSize += lastbeginMessageSize;
      }
    } 
    currentObject = NULL;    
  }
}

void TraceAutoPerf::beginIdle(double curWallTime) {
  if(isTraceOn){
  lastBeginIdle =  curWallTime; 
  lastEvent =  BEGIN_IDLE;
  }
}

void TraceAutoPerf::endIdle(double curWallTime) {
  if(isTraceOn){
  double idleTime = curWallTime - lastBeginIdle;
  totalIdleTime += idleTime; 
  lastEvent =  -1;
  }
}

void TraceAutoPerf::beginComputation(void) {
  if(isTraceOn){
#if CMK_HAS_COUNTER_PAPI
  if(CkpvAccess(papiStarted) == 0)
  {
    if (PAPI_start(CkpvAccess(papiEventSet)) != PAPI_OK) {
      CmiAbort("PAPI failed to start designated counters!\n");
    }
    CkpvAccess(papiStarted) = 1;
  }
#endif
  }
}

void TraceAutoPerf::endComputation(void) { 
  if(isTraceOn){
#if CMK_HAS_COUNTER_PAPI
  // we stop the counters here. A silent failure is alright since we
  // are already at the end of the program.
  if(CkpvAccess(papiStopped) == 0) {
    if (PAPI_stop(CkpvAccess(papiEventSet), papiValues) != PAPI_OK) {
      CkPrintf("Warning: PAPI failed to stop correctly!\n");
    }
    CkpvAccess(papiStopped) = 1;
  }
#endif
  }
}

void TraceAutoPerf::malloc(void *where, int size, void **stack, int stackSize)
{
}

void TraceAutoPerf::free(void *where, int size) { }

void TraceAutoPerf::traceClose(void)
{
  if (CkpvAccess(_traces)) {
    CkpvAccess(_traces)->endComputation();
    CkpvAccess(_traces)->removeTrace(this);
  }
}


void TraceAutoPerf::printSummary() { }

void TraceAutoPerf::summarizeObjectInfo(double &maxtime, double &totaltime, double &mintime,
    double &maxMsgCount, double &totalMsgCount, double &maxMsgSize,
    double &totalMsgSize, double &numObjs, double &maxBytesPerMsg, double &minBytesPerMsg) {
  void *maximum = NULL;

  if(objectLoads.size() > 0) {
    mintime = objectLoads.begin()->second->executeTime;
    minBytesPerMsg = maxBytesPerMsg = (double)(objectLoads.begin()->second->msgSize)/(double)(objectLoads.begin()->second->msgCount);
  }
  for(ObjectLoadMap_t::iterator it= objectLoads.begin(); it!= objectLoads.end(); it++)
  {
    if(it -> second->executeTime < mintime) {
      mintime = it->second->executeTime;
    }
    if( it->second->executeTime > maxtime) {
      maxtime = it->second->executeTime;
    }
    totaltime += it->second->executeTime;

    if( it->second->msgCount > maxMsgCount) 
    {
      maxMsgCount = it->second->msgCount;
      maximum = it->first;
    }
    totalMsgCount += it->second->msgCount;

    if( it->second->msgSize > maxMsgSize) 
      maxMsgSize = it->second->msgSize;
    totalMsgSize += it->second->msgSize;
    numObjs++;

    double temp = (double)(it->second->msgSize)/(double)(it->second->msgCount);
    if(temp > maxBytesPerMsg) {
      maxBytesPerMsg = temp;
    }
    if(temp < minBytesPerMsg) {
      minBytesPerMsg = temp;
    }
  }
  numObjs += numNewObjects;
}

PerfData* TraceAutoPerf::getSummary() {
  if(isTraceOn){
  currentSummary->data[AVG_TotalTime] = totalTraceTime();
  currentSummary->data[MIN_IdlePercentage] = currentSummary->data[MAX_IdlePercentage] = currentSummary->data[AVG_IdlePercentage] = idleRatio();
  currentSummary->data[MIN_OverheadPercentage] = currentSummary->data[MAX_OverheadPercentage] = currentSummary->data[AVG_OverheadPercentage] = overheadRatio();
  currentSummary->data[MIN_UtilizationPercentage] = currentSummary->data[MAX_UtilizationPercentage] = currentSummary->data[AVG_UtilizationPercentage] = utilRatio();
  currentSummary->data[MAX_NumInvocations] = currentSummary->data[AVG_NumInvocations] = currentSummary->data[AVG_NumInvocations] = (double)totalEntryMethodInvocations;

  summarizeObjectInfo(currentSummary->data[MAX_LoadPerObject], currentSummary->data[AVG_LoadPerObject], currentSummary->data[MIN_LoadPerObject],
                      currentSummary->data[MAX_NumMsgsPerObject],  currentSummary->data[AVG_NumMsgsPerObject],
                      currentSummary->data[MAX_BytesPerObject], currentSummary->data[AVG_BytesPerObject], currentSummary->data[AVG_NumObjectsPerPE],
                      currentSummary->data[MAX_BytesPerMsg], currentSummary->data[MIN_BytesPerMsg]);
  currentSummary->data[AVG_NumMsgRecvPerPE] = currentSummary->data[AVG_NumMsgsPerObject];
  currentSummary->data[AVG_BytesMsgRecvPerPE] = currentSummary->data[AVG_BytesPerObject];
  currentSummary->data[MAX_LoadPerPE] = currentSummary->data[MIN_LoadPerPE] = utilRatio();
  currentSummary->data[AVG_EntryMethodDuration]= (double)totalEntryMethodTime;
  currentSummary->data[MAX_EntryMethodDuration]= maxEntryTime;
  currentSummary->data[MAX_EntryID]= maxEntryIdx;

  currentSummary->data[MIN_AppPercentage] = currentSummary->data[MAX_AppPercentage] = currentSummary->data[AVG_AppPercentage] = appTime();
  currentSummary->data[AVG_TuningOverhead] = tuneOverheadTotalTime;
  currentSummary->data[AVG_EntryMethodDuration_1]= (double)totalEntryMethodTime_1;
  currentSummary->data[AVG_EntryMethodDuration_2]= (double)totalEntryMethodTime_2;
  currentSummary->data[AVG_NumInvocations_1] = (double)totalEntryMethodInvocations_1;
  currentSummary->data[AVG_NumInvocations_2] = (double)totalEntryMethodInvocations_2;
  currentSummary->data[MAX_EntryMethodDuration_1]= maxEntryTime_1;
  currentSummary->data[MAX_EntryMethodDuration_2]= maxEntryTime_2;
  currentSummary->data[MAX_EntryID_1]= maxEntryIdx_1;
  currentSummary->data[MAX_EntryID_2]= maxEntryIdx_2;
#if CMK_HAS_COUNTER_PAPI
  readPAPI();
  if((papiValues)[1]-previous_papiValues[1] > 0)
    currentSummary->data[AVG_CacheMissRate] = ((papiValues)[0]-previous_papiValues[0]) / ((papiValues)[1]-previous_papiValues[1]);
#endif
  currentSummary->data[MAX_NumMsgRecv] = currentSummary->data[MIN_NumMsgRecv] = currentSummary->data[AVG_NumMsgRecvPerPE];
  currentSummary->data[MAX_BytesMsgRecv] = currentSummary->data[MIN_BytesMsgRecv] = currentSummary->data[AVG_BytesMsgRecvPerPE];
  currentSummary->data[MinIdlePE] = CkMyPe();
  currentSummary->data[MAX_IdlePE] = CkMyPe();
  currentSummary->data[MAX_OverheadPE] = CkMyPe();
  currentSummary->data[MAX_UtilPE] = CkMyPe();
  currentSummary->data[MAX_AppPE] = CkMyPe();
  currentSummary->data[MAX_NumInvocPE] = CkMyPe();
  currentSummary->data[MAX_LoadPE] = CkMyPe();
  currentSummary->data[MAX_ExternalBytePE] = CkMyPe();
  currentSummary->data[MAX_CPPE] = CkMyPe();
  currentSummary->data[MAX_NumMsgRecvPE] = CkMyPe();
  currentSummary->data[MAX_BytesMsgRecvPE] = CkMyPe();
  currentSummary->data[MAX_NumMsgSendPE] = CkMyPe();
  currentSummary->data[MAX_BytesSendPE] = CkMyPe();
  currentSummary->data[MaxEntryPE] = CkMyPe();
  }
  return currentSummary;
}

void _createTraceperfReport(char **argv)
{
  CkpvInitialize(TraceAutoPerf*, _trace);
  CkpvAccess(_trace) = new TraceAutoPerf(argv);
  CkpvAccess(_traces)->addTrace(CkpvAccess(_trace));
}
