#ifndef __AUTOPERFAPI__H__
#define __AUTOPERFAPI__H__

#include "picsdefs.h"
#include "charm++.h"
#ifdef __cplusplus 
    extern "C" {
#endif

struct PicsConfig {
  bool fromGlobal;
  int numPhases;
  int collectionMode;
  int evaluationMode;
  int warmupSteps;
  int pauseSteps;
  char **phaseNames;
  PicsConfig() {
    fromGlobal = true;
    numPhases = PICS_INVALID;
    collectionMode = PICS_INVALID;
    evaluationMode = PICS_INVALID;
    warmupSteps = PICS_INVALID;
    pauseSteps = PICS_INVALID;
    phaseNames = nullptr;
  }
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
