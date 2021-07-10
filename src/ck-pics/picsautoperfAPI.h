#ifndef __AUTOPERFAPI__H__
#define __AUTOPERFAPI__H__

#include "picsdefs.h"
#include "charm++.h"
#ifdef __cplusplus 
    extern "C" {
#endif

struct PicsConfig {
  bool fromGlobal = true;
  int numPhases = PICS_INVALID;
  int collectionMode = PICS_INVALID;
  int evaluationMode = PICS_INVALID;
  int warmupSteps = PICS_INVALID;
  int pauseSteps = PICS_INVALID;
  char **phaseNames = nullptr;
};

void PICS_registerAutoPerfDone(CkCallback cb, int frameworkShouldAdvancePhase);

void PICS_startStep(bool fromGlobal);
void PICS_endStep(bool fromGlobal);
void PICS_endStepInc(bool fromGlobal, int incSteps);
void PICS_endStepResumeCb(bool fromGlobal,  CkCallback cb);
void PICS_startPhase(bool fromGlobal, int phaseId);
void PICS_endPhase(bool fromGlobal);

void PICS_localAutoPerfRun( );

void PICS_autoPerfRun();
void PICS_autoPerfRunResumeCb(CkCallback cb);

void PICS_SetAutoTimer();

void PICS_markLDBStart(int appStep);

void PICS_markLDBEnd();

void PICS_configure(PicsConfig config, CkCallback cb);

#ifdef __cplusplus 
    }
#endif
#endif
