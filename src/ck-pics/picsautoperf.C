#include  <stdlib.h>
#include <stdio.h>

#ifndef __STDC_FORMAT_MACROS
# define __STDC_FORMAT_MACROS
#endif
#ifndef __STDC_LIMIT_MACROS
# define __STDC_LIMIT_MACROS
#endif
#include <inttypes.h>
#include "charm++.h"
#include "pathHistory.h"
#include "TopoManager.h"
#include "picsdefs.h"
#include "picsdefscpp.h"
#include "TraceAutoPerf.decl.h"
#include "picsautoperf.h"
#include <algorithm>
#include <math.h>
#include "trace-perf.h"

#include <iterator>

#define PICS_CODE  15848
#define TRACE_START(id)
#define TRACE_END(step, id)

#define TRIGGER_PERF_IDLE_PERCENTAGE 0.1 

int user_call = 0;
int WARMUP_STEP;
int PAUSE_STEP;
#define   CP_PERIOD  200

#define TIMESTEP_RATIO_THRESHOLD 0

#define DEBUG_PRINT(x) 

#define NumOfSetConfigs   1

//ldb related quick hack
CkpvDeclare(double, timeForLdb);
CkpvDeclare(double, timeBeforeLdb);
CkpvDeclare(double, currentTimeStep);
CkpvDeclare(int, cntAfterLdb);
//scalable tree analysis
CkpvDeclare(int, myParent);
CkpvDeclare(int, myInterGroupParent);
CkpvDeclare(int, numChildren);

#if USE_MIRROR
extern CProxy_MirrorUpdate MirrorProxy;
#endif
CkpvDeclare(int, numOfPhases);
CkpvDeclare(std::vector<const char*>, phaseNames);
CkpvExtern(bool, dumpData);
CkpvDeclare(bool,   isExit);
CkpvDeclare(SavedPerfDatabase*, perfDatabase);
CkpvDeclare(Database<CkReductionMsg*>*, summaryPerfDatabase);
CkpvDeclare(DecisionTree*, learnTree);
CkpvExtern(int, availAnalyzeCP);
CkpvExtern(int, hasPendingAnalysis);
CkpvExtern(CkCallback, callBackAutoPerfDone);
CkGroupID traceAutoPerfGID;
CProxy_TraceAutoPerfBOC autoPerfProxy;
extern void setNoPendingAnalysis();
extern void startAnalysisonIdle();
extern void startAnalysis();
extern void autoPerfReset();


int PICS_collection_mode;
int PICS_evaluation_mode;

bool isPeriodicalAnalysis;
int treeGroupSize;
int numGroups;
int treeBranchFactor;
bool isIdleAnalysis;
bool isPerfDumpOn;
CkpvDeclare(FILE*, fpSummary);


SavedPerfDatabase::SavedPerfDatabase() {
  best = new PerfData();
  secondbest = new PerfData();
  prevIdx = curIdx = -1;
  for(int i=0; i<ENTRIES_SAVED; i++)
    perfList[i] = NULL;
}

SavedPerfDatabase::~SavedPerfDatabase() {
  for(int i=0; i<ENTRIES_SAVED; i++) {
    if(perfList[i] != NULL)
      free (perfList[i]);
  }
}

void SavedPerfDatabase::advanceStep() {
  startTimer = CkWallTimer();
  prevIdx = curIdx < 0 ? 0: curIdx;
  curIdx = (curIdx+1)%ENTRIES_SAVED;
  if(perfList[curIdx] == NULL) {
    int nbytes = sizeof(PerfData) * CkpvAccess(numOfPhases) * PERIOD_PERF;
    perfList[curIdx] = (PerfData*) malloc(nbytes);
    memset(perfList[curIdx], 0, nbytes);
  }
}

void SavedPerfDatabase::endCurrent( ) {
  perfList[curIdx]->timeStep = CkWallTimer() - startTimer ;
}

PerfData* SavedPerfDatabase::getCurrentPerfData(){
  if(curIdx<0) curIdx = 0;
  return perfList[curIdx];
}

PerfData* SavedPerfDatabase::getPrevPerfData(){
  return perfList[prevIdx];
}

void SavedPerfDatabase::copyData(PerfData *source, int num) {
  memcpy(perfList[curIdx], source, num * sizeof(PerfData));
}

void SavedPerfDatabase::setData(PerfData *source) {
  perfList[curIdx] = source;
}


/*
 * Reduction operation from diff PEs
 * Uses number of operations of MAX, MIN, AVG to iterate
 */
void combinePerfData(PerfData *ret, PerfData *source) {
  int k;
  CkAssert(ret!=NULL);
  CkAssert(source!=NULL);
  for(k=0; k<NUM_AVG; k++) {
    ret->data[k] += source->data[k];
  }
  if(ret->data[MAX_EntryMethodDuration] < source->data[MAX_EntryMethodDuration])
    ret->data[MaxEntryPE] = source->data[MaxEntryPE];
  for(;k<NUM_AVG+NUM_MAX; k++) {
    if(ret->data[k] < source->data[k]){
      ret->data[k] = source->data[k];
      k++;
      ret->data[k] = source->data[k];
    }
    else
    {
        k++;
    }
  }
  for(;k<NUM_AVG+NUM_MAX+NUM_MIN; k++) {
    ret->data[k] = std::min(ret->data[k], source->data[k]);
  }
}

void TraceAutoPerfBOC::gatherSummary(CkReductionMsg *msg){
  recvChildren++;
  PerfData *myCurrent;
  double *data;
  if(redMsg==NULL)
  {
    redMsg = msg;
  }else
  {
    PerfData *fromChild = (PerfData*)msg->getData();
    myCurrent = (PerfData*)redMsg->getData();
    combinePerfData(myCurrent, fromChild);
    delete msg;
  }
  if(recvChildren == CkpvAccess(numChildren)+1) {
    if(CkpvAccess(myParent) == -1)
    {
     autoPerfProxy[CkMyPe()].globalPerfAnalyze(redMsg);
     redMsg = NULL;
    }
    else{
      autoPerfProxy[CkpvAccess(myParent)].gatherSummary(redMsg);
      redMsg = NULL;
    }
    recvChildren = 0;
  }
}

CkReduction::reducerType PerfDataReductionType;

CkReductionMsg *PerfDataReduction(int nMsg,CkReductionMsg **msgs){
  PerfData *ret;
  int k;
  for(int j=0; j<CkpvAccess(numOfPhases)*PERIOD_PERF; j++) {
    if(nMsg > 0){
      ret=(PerfData*)(msgs[0]->getData())+j;
    }
    for (int i=1;i<nMsg;i++) {
      PerfData *m=(PerfData*)(msgs[i]->getData())+j;
      combinePerfData(ret, m);
    }
  }  
  ret=(PerfData*)msgs[0]->getData();
  CkReductionMsg *msg= CkReductionMsg::buildNew(sizeof(PerfData)*CkpvAccess(numOfPhases)*PERIOD_PERF,ret); 
  return msg;
}

void  TraceAutoPerfBOC::staticAtSync(void *data) {
  TraceAutoPerfBOC *me;
  char *str = NULL;
  if(data == NULL)
  {
    me = autoPerfProxy.ckLocalBranch(); 
  }else
    me = (TraceAutoPerfBOC*)(data);
}

void TraceAutoPerfBOC::startPhase(int phaseId) {
  TraceAutoPerf *t = localAutoPerfTracingInstance();
  CkpvAccess(perfDatabase)->setPhase(phaseId);
  t->startPhase(picsStep%PERIOD_PERF, phaseId);
}

void TraceAutoPerfBOC::endPhase() {
  TraceAutoPerf *t = localAutoPerfTracingInstance();
  t->endPhase();
}

void TraceAutoPerfBOC::startStep() {
  TraceAutoPerf *t = localAutoPerfTracingInstance();
  
  if(user_call == 1){ /* Resets the data to the initial values */
    t->resetAll();
  }

  if(picsStep % PERIOD_PERF == 0) //start of next analysis
  {
    t->startStep(true);
  }
  else
    t->startStep(false);
}

void TraceAutoPerfBOC::endStep(bool fromGlobal, int fromPE, int incSteps) {
  endStepTimer = CkWallTimer();
  TraceAutoPerf *t = localAutoPerfTracingInstance();
  currentAppStep += incSteps;
  picsStep++;
  if(picsStep % PERIOD_PERF == 0 ) {
    t->endStep(true);
  }
  else
  {
    t->endStep(false);
  }
}

void TraceAutoPerfBOC::endStepResumeCb(bool fromGlobal, int fromPE, CkCallback cb) {
  endStepTimer = CkWallTimer();
  TraceAutoPerf *t = localAutoPerfTracingInstance();
  if(picsStep % PERIOD_PERF == 0 ) {
    t->endStep(true);
  }
  else
  {
    t->endStep(false);
  }
  currentAppStep++;
  setAutoPerfDoneCallback(cb);
  run(fromGlobal, fromPE); 
}

void TraceAutoPerfBOC::endPhaseAndStep(bool fromGlobal, int fromPE) {
  endStepTimer = CkWallTimer();
  TraceAutoPerf *t = localAutoPerfTracingInstance();
  t->endPhase();
  currentAppStep++;
  picsStep++;
  if(picsStep % PERIOD_PERF == 0 ) {
    t->endStep(true);
    getPerfData(0, CkCallback::ignore );
  }
  else
  {
    t->endStep(false);
  }
  startStep();
  startPhase(0);
}

void TraceAutoPerfBOC::startTimeNextStep(){
  CcdCallFnAfterOnPE((CcdVoidFn)startAnalysis, NULL, CP_PERIOD, CkMyPe());
}

void TraceAutoPerfBOC::resume( CkCallback cb) {
  cb.send();
}
void TraceAutoPerfBOC::resume( ) {
  CkpvAccess(callBackAutoPerfDone).send();
}

void TraceAutoPerfBOC::run(bool fromGlobal, int fromPE)
{
  TraceAutoPerf *t = localAutoPerfTracingInstance();
  if(picsStep % PERIOD_PERF == 0 ) {
    getPerfData(0, CkCallback::ignore );
  }
  else
  {
    if(fromGlobal && CkMyPe() == fromPE)
    {
      resume();
    }
    else if (!fromGlobal)
    {
      resume( CkpvAccess(callBackAutoPerfDone));
    }
  }
}

void TraceAutoPerfBOC::PICS_markLDBStart(int appStep) {
  startLdbTimer = CkWallTimer(); 
}

void TraceAutoPerfBOC::PICS_markLDBEnd() {
  endLdbTimer = CkWallTimer();
  CkpvAccess(timeForLdb) = endLdbTimer - startLdbTimer;
  CkpvAccess(timeBeforeLdb) = currentTimeStep;
  CkpvAccess(cntAfterLdb) = -1;
}


void TraceAutoPerfBOC::registerPerfGoal(int goalIndex) {
  //CkpvAccess(perfGoal) = goalIndex;
}

void TraceAutoPerfBOC::setUserDefinedGoal(double value) { }

void TraceAutoPerfBOC::setNumOfPhases(int num, const char names[]) {
  CkpvAccess(numOfPhases) = num;
  CkpvAccess(phaseNames).clear();
  CkpvAccess(phaseNames).resize(num);
  for(int i=0; i<num; i++)
  {
    char *name = (char*)malloc(40);
    strcpy(name, names + i*40);
    CkpvAccess(phaseNames)[i] = name;
  }
}

// set the call back function, which is invoked after auto perf is done
void TraceAutoPerfBOC::setAutoPerfDoneCallback(CkCallback cb) {
  CkpvAccess(callBackAutoPerfDone) = cb;
}

void TraceAutoPerfBOC::setCbAndRun(bool fromGlobal, int fromPE, CkCallback cb) {
  CkpvAccess(callBackAutoPerfDone) = cb;
  run(fromGlobal, fromPE); 
}

void TraceAutoPerfBOC::formatPerfData(PerfData *perfdata, int subStep, int phaseID) {
  double *data = perfdata->data;
  int numpes = numPesInGroup;
  int steps = currentAppStep-lastAnalyzeStep;
  double totaltime = data[AVG_TotalTime];

  //derive metrics from raw performance data
  data[AVG_UtilizationPercentage] /= numpes;
  data[AVG_IdlePercentage] /= numpes;
  data[AVG_OverheadPercentage] /= numpes;
  data[AVG_NumObjectsPerPE] /= numpes;
  data[AVG_NumMsgRecvPerPE] /= numpes;
  data[AVG_BytesMsgRecvPerPE] /= numpes;
  data[AVG_EntryMethodDuration] /= data[AVG_NumInvocations];
  data[AVG_LoadPerPE] = data[AVG_UtilizationPercentage] * totaltime;
  data[AVG_CacheMissRate] = data[AVG_CacheMissRate]/numpes;

  data[AVG_NumMsgsPerObject] /= (data[AVG_NumObjectsPerPE]*numpes);
  data[AVG_BytesPerObject] /= (data[AVG_NumObjectsPerPE]*numpes);
  data[AVG_LoadPerObject] /= (data[AVG_NumObjectsPerPE]*numpes);
  data[AVG_BytesPerMsg] = data[AVG_BytesPerObject]/data[AVG_NumMsgsPerObject];

  data[AVG_NumMsgPerPE] = data[AVG_NumMsgsPerObject]/numpes;
  data[AVG_BytesPerPE] = data[AVG_BytesPerObject]/numpes;
  data[MAX_LoadPerPE] = data[MAX_UtilizationPercentage]*totaltime;
  data[MIN_LoadPerPE] = data[MIN_UtilizationPercentage]*totaltime;
  data[AVG_NumInvocations] = data[AVG_NumInvocations]/numpes;

    //TODO
  data[AVG_EntryMethodDuration_1] /= data[AVG_NumInvocations_1];
  data[AVG_EntryMethodDuration_2] /= data[AVG_NumInvocations_2];
  data[AVG_NumInvocations_1] = data[AVG_NumInvocations_1]/numpes;
  data[AVG_NumInvocations_2] = data[AVG_NumInvocations_2]/numpes;

  CkPrintf("\nPICS Data: PEs in group: %d\nIDLE: %.2f%%\nOVERHEAD: %.2f%%\nUTIL: %.2f%%\nAVG_ENTRY_DURATION: %fs\n", numpes, data[AVG_IdlePercentage]*100, data[AVG_OverheadPercentage]*100, data[AVG_UtilizationPercentage]*100, data[AVG_EntryMethodDuration]);
}

void TraceAutoPerfBOC::getPerfData(int reductionPE, CkCallback cb) {
  TraceAutoPerf *t = localAutoPerfTracingInstance();
  if(t->getTraceOn()) {
    if(treeBranchFactor < 0) {
      PerfData *data = CkpvAccess(perfDatabase)->getCurrentPerfData();
      CkCallback cb1(CkIndex_TraceAutoPerfBOC::globalPerfAnalyze(NULL), thisProxy[reductionPE]);
      contribute(sizeof(PerfData)*CkpvAccess(numOfPhases)*PERIOD_PERF,data, PerfDataReductionType, cb1);
      }
    else 
    {
      PerfData *data = CkpvAccess(perfDatabase)->getCurrentPerfData();
      CkReductionMsg *redMsgP = CkReductionMsg::buildNew(sizeof(PerfData)*CkpvAccess(numOfPhases)*PERIOD_PERF, data);
      if(CkpvAccess(myParent) != -1 && CkpvAccess(numChildren) == 0)  //leaves of the tree, partial collection
      {
        autoPerfProxy[CkpvAccess(myParent)].gatherSummary(redMsgP);
      }
      else{
        gatherSummary(redMsgP);
      }
    }
  }
}


/**
 * perf data from all processors within a group is collected at the root of that
 * group and the data is output to a file.
 */
void TraceAutoPerfBOC::globalPerfAnalyze(CkReductionMsg *msg )
{
  double now = CkWallTimer();
  double timestep = now-lastAnalyzeTimer;
  double totaltimestep = now-endStepTimer;
  lastAnalyzeTimer = now;
  CkpvAccess(cntAfterLdb)++;
  int numpes = numPesInGroup;

  analyzeStep++;
  PerfData *data=(PerfData*) msg->getData();
  if(CkpvAccess(isExit) || analyzeStep<= WARMUP_STEP || analyzeStep >= PAUSE_STEP) {
    autoPerfProxy[CkpvAccess(myInterGroupParent)].tuneDone();
  }
  if(analyzeStep<= WARMUP_STEP || analyzeStep >= PAUSE_STEP){
    if(isPeriodicalAnalysis && CkMyPe()== 0)
      CcdCallFnAfterOnPE((CcdVoidFn)startAnalysis, NULL, CP_PERIOD, CkMyPe());
    if(analyzeStep < WARMUP_STEP){
      delete msg;
    }
    else 
    {
      for(int j=0; j<CkpvAccess(numOfPhases)*PERIOD_PERF; j++)
      {
        formatPerfData(data, j/CkpvAccess(numOfPhases), j%CkpvAccess(numOfPhases));
        data++;
      }
      CkpvAccess(summaryPerfDatabase)->add(msg);
    }
    lastAnalyzeStep = currentAppStep;
    return;
  }

  TRACE_START(PICS_CODE);
  fprintf(CkpvAccess(fpSummary), "NEWITER %d %d %d %" PRIu64 " %d\n", analyzeStep, CkMyPe(), CkpvAccess(numOfPhases)*PERIOD_PERF, (CMK_TYPEDEF_UINT8)(CkWallTimer()*1000000), currentAppStep); 
  for(int j=0; j<CkpvAccess(numOfPhases)*PERIOD_PERF; j++)
  {
    formatPerfData(data, j/CkpvAccess(numOfPhases), j%CkpvAccess(numOfPhases));
  }
  //autoTunerProxy.ckLocalBranch()->printCPToFile(CkpvAccess(fpSummary));
  data=(PerfData*) msg->getData();
  //save results to database TODO
  if(bestTimeStep == -1 || bestTimeStep > timestep)
  {
    isBest = true;
    bestTimeStep = timestep;
  }
  else
    isBest = false;
  currentTimeStep = data->timeStep = timestep/(currentAppStep-lastAnalyzeStep);
  if(CkpvAccess(cntAfterLdb) == 1)
    CkpvAccess(currentTimeStep) = currentTimeStep;
  lastAnalyzeStep = currentAppStep;
  CkReductionMsg *oldData = CkpvAccess(summaryPerfDatabase)->add(msg);
  if(oldData != NULL) {
    delete oldData;
  }

  if( analyzeStep%NumOfSetConfigs == 0) {
    //pack results and reduce to PE0 and decide group with best performance metrics to choose best, average utilization percentage
    autoPerfProxy[CkpvAccess(myInterGroupParent)].globalDecision(data->data[AVG_UtilizationPercentage], CkMyPe());
  }
  
  TRACE_END(currentAppStep, PICS_CODE);
}

void TraceAutoPerfBOC::globalDecision(double metrics, int source) {

  if(recvGroupCnt == 0){
    bestMetrics = metrics;
    bestSource = source;
  }
  else if(bestMetrics < metrics)  //higher means better
  {
    bestMetrics = metrics;
    bestSource = source;
  }
  recvGroupCnt++;
  if(recvGroupCnt < numGroups && PICS_collection_mode==FULL )
    return;

  recvGroupCnt = 0;
  autoPerfProxy[bestSource].analyzeAndTune();
  if(isPeriodicalAnalysis)
    autoPerfProxy[0].startTimeNextStep();
}

void TraceAutoPerfBOC::analyzeAndTune(){
  problemProcList.clear();
  solutions[0].clear();
  solutions[1].clear();
  perfProblems.clear();

  CkReductionMsg *msg = CkpvAccess(summaryPerfDatabase)->getCurrent();
  PerfData *data=(PerfData*) msg->getData();
  CkReductionMsg *prevMsg = CkpvAccess(summaryPerfDatabase)->getData(0);
  PerfData *prevSummaryData = (PerfData*)(prevMsg->getData());
  for(int j=0; j<CkpvAccess(numOfPhases)*PERIOD_PERF; j++)
  {
    analyzePerfData(data, j/CkpvAccess(numOfPhases), j%CkpvAccess(numOfPhases));
    comparePerfData(prevSummaryData, data, j/CkpvAccess(numOfPhases), j%CkpvAccess(numOfPhases));
    prevSummaryData++;
    data++;
  }
  //combine all solutions in one map, lower priority first and then higher priority
  int numOfSets;
  if(PICS_collection_mode == PARTIAL)
    numOfSets = 1;
  else
    numOfSets = numGroups;
  autoPerfProxy[CkpvAccess(myInterGroupParent)].tuneDone();
}

void TraceAutoPerfBOC::analyzePerfData(PerfData *perfdata, int subStep, int phaseID) {
  double *data = perfdata->data;
  std::vector<Condition*> problems;
  problems.clear();
  (priorityTree)->DFS(data, solutions, 0, problems, CkpvAccess(fpSummary));
  (fuzzyTree)->DFS(data, solutions, 1, problems, CkpvAccess(fpSummary));
  std::copy(problems.begin(), problems.end(), std::inserter(perfProblems, perfProblems.begin()));
}

void TraceAutoPerfBOC::comparePerfData(PerfData *prevData, PerfData *perfData, int subStep, int phaseID) {
  //compare data of this step with previous step, phase by phase compare
  double *current = perfData->data;
  double *prev = prevData->data;
  double *ratios = new double[NUM_NODES];
  for(int i=0; i<NUM_NODES; i++)
  {
    if(prev[i] != 0)
      ratios[i] = current[i]/prev[i];
    else
      ratios[i] = 0;
  }
}

void TraceAutoPerfBOC::tuneDone() {
  recvGroups++;
  if(recvGroups == numGroups)
  {
    recvGroups=0;
    if(CkpvAccess(isExit))
      CkContinueExit();
    else
    {
      resume();
    }
  }
}

void TraceAutoPerfBOC::recvGlobalSummary(CkReductionMsg *msg)
{
}

double TraceAutoPerfBOC::getModelNetworkTime(int msgs, long bytes){
  // alpha + B* beta model
  double alpha = 0.000002; //2us for latency
  double beta =  0.00000025; //per byte time, 4GBytes/sec
  return msgs* alpha + beta * bytes; 
}

void TraceAutoPerfBOC::setProjectionsOutput() {
  CkpvAccess(dumpData) = true;
}

TraceAutoPerfBOC::TraceAutoPerfBOC() {
  picsStep = 0;
  lastAnalyzeStep = 0;
  lastCriticalPathLength = 0;
  currentAppStep = 0;
  analyzeStep = 0;
  isBest = false;
  bestTimeStep = -1;
  currentTimeStep = -1;
  priorityTree = new DecisionTree();
  priorityTree->build("tree.txt");
  fuzzyTree = new DecisionTree();
  fuzzyTree->build("fuzzytree.txt");
  numGroups = 1;
  recvGroups = 0;
  numPesInGroup = CkNumPes();
  recvChildren = 0;
  redMsg = NULL;
  solutions.resize(2);
  //scalable tree structure analysis
  if(treeBranchFactor > 0) {
    int treeGroupID = CkMyPe()/treeGroupSize;
    int idInTree = CkMyPe()%treeGroupSize;
    int start = treeGroupID * treeGroupSize;
    int upperBoundPE= (treeGroupID+1) * treeGroupSize;
    int upperBound;
    int child;

    recvChildren = 0;
    CkpvAccess(numChildren) = 0;
    numGroups = (CkNumPes()-1)/treeGroupSize+1;
    if(idInTree == 0)
      CkpvAccess(myParent) = -1;
    else
    {
      CkpvAccess(myParent) = (idInTree-1)/treeBranchFactor + start;
    }
    for(int i=0; i<treeBranchFactor; i++)
    {
      child = idInTree*treeBranchFactor+1+i+start;
      if(child < upperBoundPE && child<CkNumPes())
        CkpvAccess(numChildren)++;
    }
    if(upperBoundPE <= CkNumPes())
      numPesInGroup = treeGroupSize;
    else
      numPesInGroup = CkNumPes() - start; 
  }
  else{
    if(CkMyPe()==0)
      CkpvAccess(myParent) = -1;
    else
      CkpvAccess(myParent) = 0;
  }
  CkpvAccess(myInterGroupParent) = 0;
  recvGroupCnt = 0;
  TraceAutoPerf *t = localAutoPerfTracingInstance();
  if(PICS_collection_mode == PARTIAL)
  {
    numPesCollection = CkNumPes()>numPesInGroup?numPesInGroup:CkNumPes();
  }
  else
    numPesCollection = CkNumPes();

  if(CkMyPe() >= numPesCollection)
  {
    t->setTraceOn(false);
  }else
  {
    t->setTraceOn(true);
  }

  if((isPeriodicalAnalysis))
  {
    setNumOfPhases(1, "Default");
    startStep();
    startPhase(0);
    if(CkMyPe() == 0)
    {
      CcdCallFnAfterOnPE((CcdVoidFn)startAnalysis, NULL, 100, CkMyPe());
    }
  }
  //--------- Projections output
  if(CkpvAccess(myParent)==-1){
    char filename[50];
    sprintf(filename, "output.%d.pics", CkMyPe());
    if(CkMyPe()==0)
      CkpvAccess(fpSummary) = fopen(filename, "w+");
    else if(PICS_collection_mode == FULL)
      CkpvAccess(fpSummary) = fopen(filename, "w+");
  }
}

TraceAutoPerfBOC::~TraceAutoPerfBOC() { }

/**
 * Perf report initialization
 */
TraceAutoPerfInit::TraceAutoPerfInit(CkArgMsg* args)
{
  CkPrintf("PICS> Enabled PICS autoPerf\n");
  char **argv = args->argv;
  isPeriodicalAnalysis = CmiGetArgFlagDesc(argv,"+auto-pics","start performance analysis periodically");
  isIdleAnalysis = CmiGetArgFlagDesc(argv,"+idleAnalysis","start performance analysis when idle");
  if(isIdleAnalysis){
    CcdCallOnConditionKeep(CcdPROCESSOR_STILL_IDLE,(CcdVoidFn)startAnalysisonIdle, NULL);
    CcdCallFnAfterOnPE((CcdVoidFn)autoPerfReset, NULL, 10, CmiMyPe());
  }
  isPerfDumpOn = true; 
  CkpvAccess(fpSummary) = NULL;
  if(CmiGetArgIntDesc(argv,"+picsGroupSize", &treeGroupSize,"number of processors within a PICS group ")) {
    treeBranchFactor = 2;
    CkPrintf("PICS> Set scalable tree branch factor %d  group is %d\n", treeBranchFactor, treeGroupSize);
  }
  else
  {
    treeGroupSize = CkNumPes();
    treeBranchFactor = 2;
  }

  if(CmiGetArgIntDesc(argv,"+picsCollectionMode", &PICS_collection_mode, "Collection mode (0 full, 1 partial")) {
    CkPrintf("PICS> Set scalable collection mode %d\n", PICS_collection_mode);
  }else{
    PICS_collection_mode = FULL;
  }

  if(CmiGetArgIntDesc(argv,"+picsEvaluationMode", &PICS_evaluation_mode, "Evaluation mode (0 SEQ, 1 PARALLEL")) {
    CkPrintf("PICS> Set scalable evaluation mode %d\n", PICS_evaluation_mode);
  }else
  {
    PICS_evaluation_mode = SEQUENTIAL;
  }

  traceAutoPerfGID = autoPerfProxy = CProxy_TraceAutoPerfBOC::ckNew();
  /* Starts a new phase without user call */
  autoPerfProxy.startStep();
  autoPerfProxy.startPhase(0);
  autoPerfProxy.setNumOfPhases(1, "program");
}

extern "C" void traceAutoPerfExitFunction() {
  if (autoPerfProxy.ckGetGroupID().isZero()) {
    CkContinueExit();
    return;
  }

  /* Starts copying of data */
  if(user_call == 0){  // Do not call them by default if the user is calling them
    autoPerfProxy.endPhase();
    autoPerfProxy.endStepResumeCb(true, CkMyPe(), CkCallbackResumeThread());
  }

  CkpvAccess(isExit) = true;
  autoPerfProxy.getPerfData(0, CkCallback::ignore );
}

void _initTraceAutoPerfNode()
{
  PerfDataReductionType = CkReduction::addReducer(PerfDataReduction, false, "PerfDataReduction");
}

void _initTraceAutoPerfBOC()
{
  WARMUP_STEP = 0;
  PAUSE_STEP = 1000;
  CkpvInitialize(int, hasPendingAnalysis);
  CkpvAccess(hasPendingAnalysis) = 0;
  CkpvInitialize(CkCallback, callBackAutoPerfDone);
  CkpvAccess(callBackAutoPerfDone) = CkCallback::ignore; 
  CkpvInitialize(bool,   isExit);
  CkpvAccess(isExit) = false;
//  CkpvInitialize(int, perfGoal);
//  CkpvAccess(perfGoal) = BestTimeStep;
  CkpvInitialize(int, myParent);
  CkpvAccess(myParent) = -1;
  CkpvInitialize(int, myInterGroupParent);
  CkpvAccess(myInterGroupParent) = -1;
  CkpvInitialize(int, numChildren);
  CkpvAccess(numChildren) = -1;
  CkpvInitialize(int, numOfPhases);
  CkpvAccess(numOfPhases) = 1;
  CkpvInitialize(std::vector<const char*>, phaseNames);
  CkpvAccess(phaseNames).resize(1);
  CkpvAccess(phaseNames)[0] = "default";
  isPeriodicalAnalysis = false;
  CkpvInitialize(double, timeForLdb);
  CkpvAccess(timeForLdb) = 0;
  CkpvInitialize(double, timeBeforeLdb);
  CkpvAccess(timeBeforeLdb) = -1;
  CkpvInitialize(double, currentTimeStep);
  CkpvAccess(currentTimeStep) = -1;
  CkpvInitialize(int, cntAfterLdb);
  CkpvAccess(cntAfterLdb) = 4;
  CkpvInitialize(FILE*, fpSummary);
  CkpvAccess(fpSummary) = NULL;
    if (CkMyRank() == 0) {
      registerExitFn(traceAutoPerfExitFunction);
    }
    CkpvInitialize(SavedPerfDatabase*, perfDatabase);
    CkpvAccess(perfDatabase) = new SavedPerfDatabase();
    CkpvInitialize(Database<CkReductionMsg*>*, summaryPerfDatabase);
    CkpvAccess(summaryPerfDatabase) = new Database<CkReductionMsg*>();
    CkpvInitialize(DecisionTree*, learnTree);
    CkpvAccess(learnTree) = new DecisionTree();
}

//------------ C function ----------
void setCollectionMode(int m) {
  PICS_collection_mode = m;
}

void setEvaluationMode(int m) {
  PICS_evaluation_mode = m;
}

#include "TraceAutoPerf.def.h"

