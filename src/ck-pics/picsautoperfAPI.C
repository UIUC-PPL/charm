#include "picsdefs.h"
#include "picsdefscpp.h"
#include "picsautoperf.h"
#include "picsautoperfAPI.h"
#include "picsautoperfAPIC.h"
#define PERF_FREQUENCY 1
#define   CP_PERIOD  100

extern int user_call;
extern int WARMUP_STEP;
extern int PAUSE_STEP;
CkpvDeclare(int, currentStep);
CkpvDeclare(int, availAnalyzeCP);
CksvDeclare(int, availAnalyzeNodeCP);
CkpvDeclare(int, hasPendingAnalysis);
CkpvDeclare(CkCallback, callBackAutoPerfDone);

void PICS_registerAutoPerfDone(CkCallback cb, int frameworkShouldAdvancePhase){
  CkAssert(CkMyPe() == 0);
  autoPerfProxy.setAutoPerfDoneCallback(cb);
}

void PICS_startPhase( bool fromGlobal, int phaseId)
{
  if(fromGlobal)
    autoPerfProxy.startPhase(phaseId);
  else
    autoPerfProxy.ckLocalBranch()->startPhase(phaseId);
}

void PICS_endPhase( bool fromGlobal)
{
  if(fromGlobal)
    autoPerfProxy.endPhase();
  else
    autoPerfProxy.ckLocalBranch()->endPhase();
}

void PICS_startStep(bool fromGlobal)
{
  user_call = 1; //Sets call flag to 1 whenever this is called by the user
  if(fromGlobal)
    autoPerfProxy.startStep();
  else
    autoPerfProxy.ckLocalBranch()->startStep();
}

void PICS_endStep(bool fromGlobal )
{
  user_call = 1;
  if(fromGlobal)
    autoPerfProxy.endStep(fromGlobal, CkMyPe(), 1);
  else
    autoPerfProxy.ckLocalBranch()->endStep(fromGlobal, CkMyPe(), 1);
}

void PICS_endStepInc(bool fromGlobal, int incSteps  )
{
  if(fromGlobal)
    autoPerfProxy.endStep(fromGlobal, CkMyPe(), incSteps);
  else
    autoPerfProxy.ckLocalBranch()->endStep(fromGlobal, CkMyPe(), incSteps);
}



void PICS_endStepResumeCb( bool fromGlobal, CkCallback cb)
{
  if(fromGlobal) {
    autoPerfProxy.endStepResumeCb(true, CkMyPe(), cb);
  }
  else
  {
    autoPerfProxy.ckLocalBranch()->endStepResumeCb(false, CkMyPe(), cb);
  }
}

void PICS_autoPerfRun( )
{
  autoPerfProxy.run(true, CkMyPe());
}

void PICS_autoPerfRunResumeCb(CkCallback cb )
{
  autoPerfProxy.setCbAndRun(true, CkMyPe(), cb);
}

void PICS_localAutoPerfRun( )
{
  autoPerfProxy.ckLocalBranch()->run(false,CkMyPe());
}

//called by PE0
void startAnalysis()
{
  autoPerfProxy.endPhaseAndStep(true, CkMyPe());
}

void PICS_SetAutoTimer(){
  CcdCallFnAfterOnPE((CcdVoidFn)startAnalysis, NULL, 100, CkMyPe());
}

void startAnalysisonIdle()
{
  if (traceAutoPerfGID.idx !=0 && ((CkGroupID)autoPerfProxy).idx != 0 &&
      CksvAccess(availAnalyzeNodeCP) == 1 &&
      CkpvAccess(hasPendingAnalysis) == 0 )
  {
    CksvAccess(availAnalyzeNodeCP) = 0;
    CcdCallFnAfterOnPE((CcdVoidFn)autoPerfReset, NULL, CP_PERIOD, CkMyPe());
  }
}

void autoPerfReset()
{
  CksvAccess(availAnalyzeNodeCP) = 1;
}

void setNoPendingAnalysis()
{
  CkpvAccess(hasPendingAnalysis) = 0;
}

void registerPerfGoal(int goalIndex) 
{
  autoPerfProxy.registerPerfGoal(goalIndex);
}

void setUserDefinedGoal(double value)
{
  autoPerfProxy.setUserDefinedGoal(value);
}

void PICS_markLDBStart(int appStep) {
  autoPerfProxy.PICS_markLDBStart(appStep);
}

void PICS_markLDBEnd(){
  autoPerfProxy.PICS_markLDBEnd();
}

void PICS_configure(PicsConfig config, CkCallback cb) {
  if (config.warmupSteps != PICS_INVALID)
    WARMUP_STEP = config.warmupSteps;
  if (config.pauseSteps != PICS_INVALID)
    PAUSE_STEP = config.pauseSteps;
  if (config.collectionMode != PICS_INVALID)
    setCollectionMode(config.collectionMode);
  if (config.evaluationMode != PICS_INVALID)
    setEvaluationMode(config.evaluationMode);

  if (config.numPhases != PICS_INVALID) {
    std::vector<char> seqNames(config.numPhases*40);
    for(int i = 0; i < config.numPhases; i++) {
      strcpy(&seqNames[0]+i*40, config.phaseNames[i]);
    }
    if(config.fromGlobal)
      autoPerfProxy.setNumOfPhases(config.numPhases, &seqNames[0], cb);
    else
      autoPerfProxy.ckLocalBranch()->setNumOfPhases(config.numPhases, &seqNames[0], cb);
  } else {
    char name[40] = "default";
    if(config.fromGlobal)
      autoPerfProxy.setNumOfPhases(1, &name[0], cb);
    else
      autoPerfProxy.ckLocalBranch()->setNumOfPhases(1, &name[0], cb);
  }
}
