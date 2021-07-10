#ifndef  TRACE__AUTOPERF__H__
#define  TRACE__AUTOPERF__H__
#define _VERBOSE_H

#include "picstreenode.h"
#include "picsdecisiontree.h"
#include "picsautoperfAPI.h"
#include <errno.h>
#include "charm++.h"
#include "trace.h"
#include "envelope.h"
#include "register.h"
#include "trace-common.h"
#include "TraceAutoPerf.decl.h"
#include "trace-projections.h"
#include <vector>
#include <map>
#include <list>
#include <fstream>
#include <iostream>
#include <utility>


CkpvExtern(int, numOfPhases);
class SavedPerfDatabase;
CkpvExtern(SavedPerfDatabase*, perfDatabase);
CkpvExtern(DecisionTree*, learnTree);
CkpvExtern(int, perfGoal);

//scalable tree analysis
CkpvExtern(int, myParent);
CkpvExtern(int, myInterGroupParent);
CkpvExtern(int, numChildren);


extern CkGroupID traceAutoPerfGID;
extern CProxy_TraceAutoPerfBOC autoPerfProxy;
extern int treeBranchFactor;
extern int numGroups;
extern int treeGroupSize;

/*
 * raw performance summary data
 */
class PerfData {
public:
  double data[NUM_NODES];
  double timeStep;
  double energy;
  double utilPercentage;
  double overheadPercentage;
  double idlePercentage;
  double userMetrics;

  PerfData() {}

  PerfData(double step, double util, double idle, double overhead)
  {
    timeStep = step;
    idlePercentage = idle;
    overheadPercentage = overhead;
    utilPercentage = util;
  }

  void copy(PerfData *src)
  {
    timeStep = src->timeStep;
    energy = src->energy;
    utilPercentage = src->utilPercentage;
    overheadPercentage = src->overheadPercentage;
    idlePercentage = src->idlePercentage;
    userMetrics = src->userMetrics;
  }

  void printMe(FILE *fp, const char *str) {
    for(int i=0; i<NUM_NODES; i++)
    {
      if (i == AVG_IdlePercentage || i == AVG_OverheadPercentage ||
          i==AVG_UtilizationPercentage || i==AVG_AppPercentage ||
          i == MAX_IdlePercentage || i == MAX_OverheadPercentage ||
          i == MAX_UtilizationPercentage || i == MAX_AppPercentage ||
          i == MIN_IdlePercentage || i == MIN_OverheadPercentage ||
          i == MIN_UtilizationPercentage)
        fprintf(fp, "%d %s %.1f\n", i, FieldName[i], 100*data[i]);
      else
        fprintf(fp, "%d %s %f\n", i, FieldName[i], data[i]);
    }
  }
};

/*
 * a set of history performance summary data
 */
template <class DataType> class Database{
private:
  std::vector<DataType> array;
  int curIdx;
  int prevIdx;

public:
  Database() {
    prevIdx = curIdx = -1;
    array.resize(10);
    for(int i=0; i<array.size(); i++)
      array[i] = NULL;
  }

  Database(int s) {
    prevIdx = curIdx = -1;
    array.resize(s);
  }

  DataType add(DataType source) {
    DataType oldData;
    prevIdx = curIdx;
    curIdx = (curIdx+1)%array.size();
    oldData = array[curIdx];
    array[curIdx] = source;
    if(prevIdx == -1) {
      prevIdx = 0;
    }
    return oldData;
  }

  DataType getCurrent() {
    return array[curIdx];
  }

  DataType getPrevious() {
    return array[prevIdx];
  }

  //relative position index
  DataType getData(int index) {
    int i = (curIdx+index+array.size())%array.size();
    return array[i];
  }

};

CkpvExtern(Database<CkReductionMsg*>*, summaryPerfDatabase);
#define ENTRIES_SAVED       10
class SavedPerfDatabase {
private:
  PerfData    *perfList[ENTRIES_SAVED];
  PerfData    *best, *secondbest;
  int         currentPhase;
  double      startTimer;
  int         curIdx; //current available
  int         prevIdx;
public:

  SavedPerfDatabase(void) ;
  ~SavedPerfDatabase(void);
  void advanceStep(void);
  PerfData* getCurrentPerfData(void);
  PerfData* getPrevPerfData(void);

  void setUserDefinedMetrics(double v) { perfList[curIdx]->userMetrics = v; }
  void setPhase(int phaseId) { currentPhase = phaseId; }
  void endCurrent(void) ;
  void copyData(PerfData *source, int num);   //copy data from source
  void setData(PerfData *source);
  bool timeStepLonger() { return true;}
  double getCurrentTimestepDuration() { return perfList[curIdx]->timeStep; }
  double getTimestepRatio() { return perfList[curIdx]->timeStep/perfList[prevIdx]->timeStep; }
  double getUtilRatio() { return perfList[curIdx]->utilPercentage/perfList[prevIdx]->utilPercentage; }
  double getEnergyRatio() { return 0; }
  double getCurrentIdlePercentage() { return perfList[curIdx]->idlePercentage; }
  double getPreviousIdlePercentage() { return perfList[prevIdx]->idlePercentage; }
  double getIdleRatio() { return  perfList[curIdx]->idlePercentage/perfList[prevIdx]->idlePercentage; }
  double getCurrentOverheadPercentage() { return perfList[curIdx]->overheadPercentage; }
  double getPreviousOverheadPercentage() { return perfList[prevIdx]->overheadPercentage; }
  double getOverheadRatio() { return perfList[curIdx]->overheadPercentage/perfList[prevIdx]->overheadPercentage; }
};

class TraceAutoPerfInit : public Chare {
public:
  TraceAutoPerfInit(CkArgMsg*);
  TraceAutoPerfInit(CkMigrateMessage *m):Chare(m) {}
};

/*
 * class to perform collection, analysis
 */
class TraceAutoPerfBOC : public CBase_TraceAutoPerfBOC {
private:
  int         numPesCollection;
  int         recvChildren;
  int         recvGroups;
  CkReductionMsg *redMsg;
  int         numPesInGroup;

  int         picsStep;
  bool        isBest;
  double      bestTimeStep;
  double      currentTimeStep;

  int         lastAnalyzeStep;
  int         currentAppStep;
  int         analyzeStep;
  double      endStepTimer;
  double      lastCriticalPathLength;
  double      lastAnalyzeTimer;
  LBManager  *theLbdb;
  std::vector<IntDoubleMap> solutions;
  std::vector<Condition*>    perfProblems;
  std::vector<int>            problemProcList;
  DecisionTree* priorityTree;
  DecisionTree* fuzzyTree;

  int     recvGroupCnt;
  double  bestMetrics;
  int     bestSource;

  double startLdbTimer;
  double endLdbTimer;

public:
  TraceAutoPerfBOC() ;
  TraceAutoPerfBOC(CkMigrateMessage *m) : CBase_TraceAutoPerfBOC(m) {};
  ~TraceAutoPerfBOC();

  void pup(PUP::er &p) {
    CBase_TraceAutoPerfBOC::pup(p);
  }
  void registerPerfGoal(int goalIndex);
  void setUserDefinedGoal(double value);
  void setAutoPerfDoneCallback(CkCallback cb); 
  static void staticAtSync(void *data);

  void resume();
  void resume(CkCallback cb);
  void startPhase(int phaseId);
  void endPhase();
  void startStep();
  void endStep(bool fromGlobal, int pe, int incSteps);
  void endPhaseAndStep(bool fromGlobal, int pe);
  void endStepResumeCb(bool fromGlobal, int pe, CkCallback cb);
  void getPerfData(int reductionPE, CkCallback cb);
  void run(bool fromGlobal, int fromPE);
  void setCbAndRun(bool fromGlobal, int fromPE, CkCallback cb) ;
  void PICS_markLDBStart(int appStep) ;
  void PICS_markLDBEnd() ;

  void setNumOfPhases(int num, const char names[], CkCallback cb);
  void setProjectionsOutput();
  void recvGlobalSummary(CkReductionMsg *msg);

  void tuneDone();

  //scalable analysis, global decision making
  void globalDecision(double metrics, int source);
  void analyzeAndTune();
  void startTimeNextStep();
  void gatherSummary(CkReductionMsg *msg);
  void globalPerfAnalyze(CkReductionMsg *msg);

  void formatPerfData(PerfData *data, int step, int phase);
  void analyzePerfData(PerfData *data, int step, int phase);

  void comparePerfData(PerfData *prevData, PerfData *data, int step, int phase);

  double getModelNetworkTime(int msgs, long bytes) ;
  
  inline bool isCurrentBest() {
    return isBest;
  }

  inline void setCurrentBest(bool b ) {
    isBest = b;
  }

  inline double getCurrentBestRatio() {
    return currentTimeStep/bestTimeStep;
  } 
};

class ObjIdentifier {
public:
  void *objPtr;

  ObjIdentifier(void *p) {
    objPtr = p;
  }

  ObjIdentifier(int a, int i, void *p) {
    objPtr = p;
  }
};

class ObjInfo 
{
public:
  double executeTime;
  long msgCount;
  long msgSize;

  ObjInfo(double e, long mc, long ms) {
    executeTime = e;
    msgCount = mc;
    msgSize = ms;
  }
};

class compare {
public:
  bool operator () (const void *x, const void *y) const {
    return (x < y);
  }
};

typedef std::map<void*, ObjInfo*, compare> ObjectLoadMap_t;


void setCollectionMode(int m) ;
void setEvaluationMode(int m) ;
void setConfigMode(int m) ;

#endif

