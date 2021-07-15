/**
 * \addtogroup CkPerf
*/
/*@{*/

// http://www.scl.ameslab.gov/Projects/Rabbit/
// http://www.support.compaq.com/nttools

#include "charm++.h"

#ifdef CMK_ORIGIN2000
#include "trace-counter.h"
#include "ck.h"
#include <inttypes.h>
#include <values.h>

#define DEBUGF(x) // CmiPrintf x
#define VER 1.0

// for performance monitoring
extern "C" int start_counters(int e0, int e1);
extern "C" int read_counters(int e0, long long *c0, int e1, long long *c1);

// a message will be composed of 2*numCounters + NUM_EXTRA_PERF
// reduction[0-(numStats-1)] correspond to counters 0-(numStats-1)
// the next is phase (only write if CmiMyPe() == 0)
// the next is the idle time (if any calculated)
// the next four are (respectively) flops/s, l1 miss%, l2 miss%, tlb miss%
// write -1 if not able to calculate
static const int NUM_EXTRA_PERF = 6;

CkpvStaticDeclare(Trace*, _trace);
CpvStaticDeclare(CountLogPool*, _logPool);
CpvStaticDeclare(char*,  _logName);
CpvStaticDeclare(char**, _counterNames);
CpvStaticDeclare(char**, _counterDesc);
CpvStaticDeclare(int,    _numCounters);
CpvStaticDeclare(int,    _reductionID);
static int _numEvents = 0;
CpvDeclare(double, version);

// a rudimentary reduction to print out the performance results across the run
CmiHandler StatTableReduction(char* msg)
{
  DEBUGF(("StatTableReduction called\n", CmiMyPe(), CmiNumPes()));
  static double* reduce = NULL;
  static int numReduce = 0;
  int numCounters = CpvAccess(_numCounters);
  int size = 2*CpvAccess(_numCounters)+NUM_EXTRA_PERF;
  int i;
  if (reduce == NULL) {
    // allocate
    reduce = new double[size];
    for (i=0; i<size; i++) { reduce[i] = 0.0; }
    DEBUGF(("  allocated reduce numCounters %d size %d\n", 
	    numCounters, size));
  }

  // see above for the feilds of this message
  double* msgResults = (double *)(msg+ALIGN8(CmiMsgHeaderSizeBytes));
  for (i=0; i<size; i++) { 
    reduce[i] += msgResults[i]; 
  }
  
  char** counterNames = CpvAccess(_counterNames);
  numReduce++;
  DEBUGF(("      numReduce %d numPes %d\n", numReduce, CmiNumPes()));
  int phase = reduce[2*numCounters];
  if (numReduce >= CmiNumPes()) {
    // finished with reduction, print out results
    numReduce = 0;
    for (i=0; i<numCounters; i++) {
      if (reduce[2*i+1]>0.0) { // is time > 0?
	if (phase >= 0) {
	  CmiPrintf("PHASE %d %s totalCount %f totalTime (us) %f\n" 
		    "PHASE %d %s count/proc %f avgTime (us)/phase %f\n",
		    phase, counterNames[i], reduce[2*i], reduce[2*i+1]*1e6,
		    phase, counterNames[i], reduce[2*i]/CmiNumPes(), 
		    reduce[2*i+1]*1e6/CmiNumPes());
	}
	else {
	  CmiPrintf("%s totalCount %f totalTime (us) %f\n" 
		    "%s count/proc %f avgTime (us)/phase %f\n",
		    counterNames[i], reduce[2*i], reduce[2*i+1]*1e6,
		    counterNames[i], reduce[2*i]/CmiNumPes(), 
		    reduce[2*i+1]*1e6/CmiNumPes());
	}
      }
    }
    if (phase >= 0) {
      CmiPrintf("PHASE %d totalIdleTime (us) %f avgIdleTime (us)/phase %f\n",
		phase, reduce[2*numCounters+1]*1e6, 
		reduce[2*numCounters+1]*1e6/CmiNumPes());
    }
    else {
      CmiPrintf("totalIdleTime (us) %f avgIdleTime (us)/phase %f\n",
		reduce[2*numCounters+1]*1e6, 
		reduce[2*numCounters+1]*1e6/CmiNumPes());
    }
    if (reduce[2*numCounters+2] > 0.0) {
      // we have flops
      if (phase >= 0) {
	CmiPrintf("PHASE %d flops/s %f flops/s/PE %f\n",
		  phase, reduce[2*numCounters+2], 
		  reduce[2*numCounters+2]/CmiNumPes());
      }
      else {
	CmiPrintf("flops/s %f flops/s/PE %f\n",
		  reduce[2*numCounters+2], 
		  reduce[2*numCounters+2]/CmiNumPes());
      }
    }
    char* missRate = NULL;
    for (i=0; i<3; i++) {
      switch (i) {
      case 0: missRate = "l1 avg miss rate (%)";  break;
      case 1: missRate = "l2 avg miss rate (%)";  break;
      case 2: missRate = "tlb avg miss rate (%)";  break;
      }
      if (reduce[2*numCounters+3+i] >= 0.0) {
	if (phase >= 0) {
	  CmiPrintf("PHASE %d %s %f\n", 
		    phase, missRate, reduce[2*numCounters+3+i]/CmiNumPes()*100);
	}
	else {
	  CmiPrintf("%s %f\n", 
		    missRate, reduce[2*numCounters+3+i]/CmiNumPes()*100);
	}
      }
    }

    // clean up
    delete [] reduce;
    reduce = NULL;
  }
  CmiFree(msg);
}

//! The following is the list of arguments that can be passed to 
//!   the +counter{1|2} command line arguments.
//! To add or change, change NUM_COUNTER_ARGS and follow the examples
//! Use three constructor arguments:
//!   1) Code (for SGI libperfex) associated with counter.
//!   2) String to be entered on the command line.
//!   3) String that is the description of the counter.
//! All NUM_COUNTER_ARGS are automatically registered via
//!   TraceCounter::TraceCounter() definition.
static const int NUM_COUNTER_ARGS = 32;
static TraceCounter::CounterArg COUNTER_ARG[NUM_COUNTER_ARGS] = 
{ TraceCounter::CounterArg( 0, "CYCLES0",      "Cycles (also see code 16)"),
  TraceCounter::CounterArg( 1, "INSTR",        "Issued instructions"),
  TraceCounter::CounterArg( 2, "LOAD",         "Issued loads"),
  TraceCounter::CounterArg( 3, "STORE",        "Issued stores"),
  TraceCounter::CounterArg( 4, "STORE_COND",   "Issued store conditionals"),
  TraceCounter::CounterArg( 5, "FAIL_COND",    "Failed store conditionals"),
  TraceCounter::CounterArg( 6, "DECODE_BR",    "Decoded branches.  (This changes meaning in 3.x versions of R10000.  It becomes resolved branches)"),
  TraceCounter::CounterArg( 7, "QUADWORDS2",   "Quadwords written back from secondary cache"),
  TraceCounter::CounterArg( 8, "CACHE_ER2",    "Correctable secondary cache data array ECC errors"),
  TraceCounter::CounterArg( 9, "L1_IMISS",     "Primary (L1) instruction cache misses"),
  TraceCounter::CounterArg(10, "L2_IMISS",     "Secondary (L2) instruction cache misses"),
  TraceCounter::CounterArg(11, "INSTRMISPR",   "Instruction misprediction from secondary cache way prediction table"),
  TraceCounter::CounterArg(12, "EXT_INTERV",   "External interventions"),
  TraceCounter::CounterArg(13, "EXT_INVAL",    "External invalidations"),
  TraceCounter::CounterArg(14, "VIRT_COHER",   "Virtual coherency conditions.  (This changes meaning in 3.x versions of R10000.  It becomes ALU/FPU forward progress cycles.  On the R12000, this counter is always 0)."),
  TraceCounter::CounterArg(15, "GR_INSTR15",   "Graduated instructions (also see code 17)"),
  TraceCounter::CounterArg(16, "CYCLES16",     "Cycles (also see code 0)"),
  TraceCounter::CounterArg(17, "GR_INSTR17",   "Graduated instructions (also see code 15)"),
  TraceCounter::CounterArg(18, "GR_LOAD" ,     "Graduated loads"),
  TraceCounter::CounterArg(19, "GR_STORE",     "Graduated stores"),
  TraceCounter::CounterArg(20, "GR_ST_COND",   "Graduated store conditionals"),
  TraceCounter::CounterArg(21, "GR_FLOPS",     "Graduated floating point instructions"),
  TraceCounter::CounterArg(22, "QUADWORDS1",   "Quadwords written back from primary data cache"),
  TraceCounter::CounterArg(23, "TLB_MISS",     "TLB misses"),
  TraceCounter::CounterArg(24, "MIS_BR",       "Mispredicted branches"),
  TraceCounter::CounterArg(25, "L1_DMISS",     "Primary (L1) data cache misses"),
  TraceCounter::CounterArg(26, "L2_DMISS",     "Primary (L2) data cache misses"),
  TraceCounter::CounterArg(27, "DATA_MIS",     "Data misprediction from secondary cache way predicition table"),
  TraceCounter::CounterArg(28, "EXT_INTERV2",  "External intervention hits in secondary cache (L2)"),
  TraceCounter::CounterArg(29, "EXT_INVAL2",   "External invalidation hits in secondary cache"),
  TraceCounter::CounterArg(30, "CLEAN_ST_PRE", "Store/prefetch exclusive to clean block in secondary cache"),
  TraceCounter::CounterArg(31, "SHARE_ST_PRE", "Store/prefetch exclusive to shared block in secondary cache") 
};

// this is called by the Charm++ runtime system
void _createTracecounter(char **argv)
{
  DEBUGF(("%d/%d DEBUG: createTraceCounter\n", CmiMyPe(), CmiNumPes()));
  CkpvInitialize(Trace*, _trace);
  TraceCounter* tc = new TraceCounter();  _MEMCHECK(tc);
  tc->traceInit(argv);
  CkpvAccess(_trace) = tc;
  CkpvAccess(_traces)->addTrace(CkpvAccess(_trace));
}

// constructor
StatTable::StatTable():
  stats_(NULL), numStats_(0)
{
  DEBUGF(("%d/%d DEBUG: StatTable::StatTable %08x size %d\n", 
          CmiMyPe(), CmiNumPes(), this));

  DEBUGF(("%d/%d registering reductionID\n", CmiMyPe(), CmiNumPes()));
  CpvAccess(_reductionID) = CmiRegisterHandler((CmiHandler)StatTableReduction); 
}

// destructor
StatTable::~StatTable() { delete [] stats_; }

// initialize the stat table internals
void StatTable::init(int argc)
{
  char** counterNames = CpvAccess(_counterNames);
  char** counterDesc = CpvAccess(_counterDesc);

  if (argc > numStats_) {
    delete [] stats_;
    stats_ = new Statistics[argc];  _MEMCHECK(stats_);
    numStats_ = argc;
  }
  for (int i=0; i<argc; i++) { 
    DEBUGF(("%d/%d DEBUG:   %d name %s\n     desc %s\n", 
	    CmiMyPe(), CmiNumPes(), i, name[i], desc[i]));
    stats_[i].name = counterNames[i]; 
    stats_[i].desc = counterDesc[i];
  }
  clear();
}

//! one entry is called for 'time' seconds, value is counter reading
void StatTable::setEp(int epidx, int stat, long long value, double time) 
{
  // CmiPrintf("StatTable::setEp %08x %d %d %d %f\n", 
  //           this, epidx, stat, value, time);

  CkAssert(epidx<MAX_ENTRIES);
  CkAssert(stat<numStats_ && stat>=0);
  
  int numCalled = stats_[stat].numCalled[epidx];
  double avg = stats_[stat].avgCount[epidx];
  double stdDev = stats_[stat].stdDevCount[epidx];
  stats_[stat].numCalled[epidx]++;
  stats_[stat].avgCount[epidx] = (avg * numCalled + value) / (numCalled + 1);
  // note that the stddev calculation is not quite correct, but it's
  // almost correct and over time will converge with true value
  avg = stats_[stat].avgCount[epidx];
  stats_[stat].stdDevCount[epidx] = 
    (stdDev * numCalled + (value-avg)*(value-avg)) / (numCalled+1);
  // CmiPrintf("variance %f avg %f value %ld calcVariance %f\n", 
  // stdDev, avg, value, stats_[stat].stdDevCount[epidx]);
  stats_[stat].totTime[epidx] += time;
  if (stats_[stat].maxCount[epidx] < value) {
    stats_[stat].maxCount[epidx] = value;
  }
  if (stats_[stat].minCount[epidx] > value) {
    stats_[stat].minCount[epidx] = value;
  }
}

//! write three lines for each stat:
//!   1. number of calls for each entry
//!   2. average count for each entry
//!   3. total time in us spent for each entry
void StatTable::write(FILE* fp) 
{
  DEBUGF(("%d/%d DEBUG: Writing StatTable\n", CmiMyPe(), CmiNumPes()));
  int i, j;
  int _numEntries=_entryTable.size();
  for (i=0; i<numStats_; i++) {
    // FAKE OUT AND WRITE AN OVERVIEW AS LAST ENTRY
    // write description of the entry
    fprintf(fp, "[%s {%s}]\n", stats_[i].name, stats_[i].desc);
    // write number of calls for each entry
    fprintf(fp, "[%s num_called] ", stats_[i].name);
    for (j=0; j<_numEntries+1; j++) { 
      fprintf(fp, "%d ", stats_[i].numCalled[j]); 
    }
    fprintf(fp, "\n");
    // write average count for each 
    fprintf(fp, "[%s avg_count] ", stats_[i].name);
    for (j=0; j<_numEntries+1; j++) { 
      fprintf(fp, "%f ", stats_[i].avgCount[j]); 
    }
    fprintf(fp, "\n");
    // write standard deviation of count for each 
    fprintf(fp, "[%s std_dev_count] ", stats_[i].name);
    for (j=0; j<_numEntries+1; j++) { 
      fprintf(fp, "%f ", sqrt(stats_[i].stdDevCount[j])); 
    }
    fprintf(fp, "\n");
    // write total time in us spent for each entry
    fprintf(fp, "[%s total_time(us)] ", stats_[i].name);
    for (j=0; j<_numEntries+1; j++) {
      fprintf(fp, "%f ", stats_[i].totTime[j]*1e6);
    }
    fprintf(fp, "\n");
    // write max count for each entry
    fprintf(fp, "[%s max_count] ", stats_[i].name);
    for (j=0; j<_numEntries+1; j++) { 
      fprintf(fp, "%ld ", stats_[i].maxCount[j]); 
    }
    fprintf(fp, "\n");
    // write min count for each entry
    fprintf(fp, "[%s min_count] ", stats_[i].name);
    for (j=0; j<_numEntries+1; j++) { 
      if (stats_[i].minCount[j] == MAXLONGLONG) { stats_[i].minCount[j] = 0; }
      fprintf(fp, "%ld ", stats_[i].minCount[j]); 
    }
    fprintf(fp, "\n");
  }
  DEBUGF(("%d/%d DEBUG: Finished writing StatTable\n", CmiMyPe(), CmiNumPes()));
}

//! set all of internals to null
void StatTable::clear() 
{
  for (int i=0; i<numStats_; i++) {
    for (int j=0; j<MAX_ENTRIES; j++) {
      stats_[i].numCalled[j] = 0;
      stats_[i].avgCount[j] = 0.0;
      stats_[i].stdDevCount[j] = 0.0;
      stats_[i].totTime[j] = 0.0;
      stats_[i].maxCount[j] = 0.0;
      stats_[i].minCount[j] = MAXLONGLONG;
    }
  }
}

//! do a reduction across processors to calculate the total count for
//! each count, and if the count has flops, etc, then calc the 
//! the flops/s, etc...
void StatTable::doReduction(int phase, double idleTime) {
  DEBUGF(("%d/%d DEBUG: StatTable::doReduction()\n",
	  CmiMyPe(), CmiNumPes(), this));
  // see above (NUM_EXTRA_PERF) for the fields in the message
  int msgSize = 
    ALIGN8(CmiMsgHeaderSizeBytes)+
    sizeof(double)*(2*numStats_+NUM_EXTRA_PERF);
  char *msg = (char *)CmiAlloc(msgSize);
  double* reduction = (double*)(msg+ALIGN8(CmiMsgHeaderSizeBytes));
  // calculate flops/s, l1%, l2%, tlb% if it's there
  char** counterNames = CpvAccess(_counterNames);
  int GR_FLOPS = -1;  double flopsRate = -1.0;  
  int LOAD     = -1;  double loadRate = -1.0;
  int STORE    = -1;  double storeRate = -1.0;
  int L1_DMISS = -1;  double l1Rate = -1.0;
  int L2_DMISS = -1;  double l2Rate = -1.0;
  int TLB_MISS = -1;  double tlbRate = -1.0;
  int i, j;
  for (i=0; i<2*numStats_+NUM_EXTRA_PERF; i++) { reduction[i] = 0.0; }
  for (i=0; i<numStats_; i++) {
    for (int j=0; j<MAX_ENTRIES; j++) { 
      reduction[2*i] += stats_[i].numCalled[j]*stats_[i].avgCount[j]; 
      reduction[2*i+1] += stats_[i].totTime[j];
    }
    if (strcmp(counterNames[i], "GR_FLOPS")==0) { GR_FLOPS = i; }
    else if (strcmp(counterNames[i], "LOAD")==0) { LOAD = i; }
    else if (strcmp(counterNames[i], "STORE")==0) { STORE = i; }
    else if (strcmp(counterNames[i], "L1_DMISS")==0) { L1_DMISS = i; }
    else if (strcmp(counterNames[i], "L2_DMISS")==0) { L2_DMISS = i; }
    else if (strcmp(counterNames[i], "TLB_MISS")==0) { TLB_MISS = i; }
  }
  if (CmiMyPe()==0) { reduction[2*numStats_] = phase; }
  reduction[2*numStats_+1] = idleTime;  
  // -1 for the rest of the calc values
  reduction[2*numStats_+2] = -1.0;
  reduction[2*numStats_+3] = -1.0;
  reduction[2*numStats_+4] = -1.0;
  reduction[2*numStats_+5] = -1.0;
  // calculate flops/s, l1%, l2%, tlb% if it's there
  double* rate = NULL;
  int  index;
  for (i=0; i<6; i++) {
    switch (i) {
    case 0: rate = &flopsRate; index = GR_FLOPS;   break;
    case 1: rate = &loadRate;  index = LOAD;       break;
    case 2: rate = &storeRate; index = STORE;      break;
    case 3: rate = &l1Rate;    index = L1_DMISS;   break;
    case 4: rate = &l2Rate;    index = L2_DMISS;   break; 
    case 5: rate = &tlbRate;   index = TLB_MISS;   break;
    }
    if (index >= 0 && reduction[2*index+1] > 0.0) { 
      // if we have the counter AND it's times were non-zero
      *rate = reduction[2*index]/reduction[2*index+1]; 
    }
  }
  // store rates if there
  if (GR_FLOPS >= 0) { reduction[2*numStats_+2] = flopsRate; }
  if (LOAD >= 0 && STORE >= 0) {
    double memRate = loadRate + storeRate;
    if (L1_DMISS >= 0 & memRate > 0) { 
      reduction[2*numStats_+3] = l1Rate / memRate; 
    }
    if (L2_DMISS >= 0 & memRate > 0) { 
      reduction[2*numStats_+4] = l2Rate / memRate; 
    }
    if (TLB_MISS >= 0 & memRate > 0) { 
      reduction[2*numStats_+5] = tlbRate / memRate; 
    }
  }

  // send the data
  CmiSetHandler(msg, (int)CpvAccess(_reductionID));
  int handlerID = CmiGetHandler(msg);
  DEBUGF(("%d/%d handlerID %d reductionID %d\n", 
	  CmiMyPe(), CmiNumPes(), handlerID, CpvAccess(_reductionID)));
  CmiSyncSendAndFree(0, msgSize, msg);
}

CountLogPool::CountLogPool():
  stats_     (),
  lastPhase_ (-1)
{
  DEBUGF(("%d/%d DEBUG: CountLogPool::CountLogPool() %08x\n", 
          CmiMyPe(), CmiNumPes(), this));
}

// open file, if phase is -1, don't add the phase string
FILE* CountLogPool::openFile(int phase) {
  // technically, the sprintf into pestr & phasestr are unnecessary,
  // can just make a limit and check for that

  DEBUGF(("%d CountLogPool::openFile(%d)\n", CmiMyPe(), phase));
  const static int strSize = 10;
  char pestr[strSize+1];
  char phasestr[strSize+1];
  snprintf(pestr, strSize, "%d", CmiMyPe());
  pestr[strSize] = '\0';
  int len = strlen(CpvAccess(_logName)) + strlen("phase.count.") + 2*strSize + 1;
  char* fname = new char[len+1];  _MEMCHECK(fname);
  if (phase >= 0) { 
    snprintf(phasestr, strSize, "%d", phase);
    phasestr[strSize] = '\0';
    sprintf(fname, "%s.phase%s.%s.count", CpvAccess(_logName), phasestr, pestr); 
  }
  else { sprintf(fname, "%s.%s.count", CpvAccess(_logName), pestr); }
  FILE* fp = NULL;
  DEBUGF(("%d/%d DEBUG: TRACE: %s:%d\n", CmiMyPe(), CmiNumPes(), fname, errno));
  do {
    fp = fopen(fname, "w+");
  } while (!fp && errno == EINTR);
  delete[] fname;
  if(!fp) {
    CmiAbort("Cannot open Summary Trace File for writing...\n");
  }
  return fp;
}

void CountLogPool::write(int phase) 
{
  if (phase >= 0) { lastPhase_ = phase; }
  if (phase < 0 && lastPhase_ >= 0) { lastPhase_++;  phase = lastPhase_; }

  int _numEntries=_entryTable.size();
  FILE* fp = (phase==-1) ? openFile() : openFile(phase); 
  fprintf(fp, "ver:%3.1f %d/%d ep:%d counters:%d\n", 
	  CpvAccess(version), CmiMyPe(), CmiNumPes(), _numEntries+1, 
	  stats_.numStats());
  stats_.write(fp);
  fclose(fp);
}

void CountLogPool::writeSts(int phase)
{
  // technically, the sprintf into phasestr is unnecessary,
  // can just make a limit and check for that

  if (phase >= 0) { lastPhase_ = phase; }
  DEBUGF(("%d CountLogPool::writeSts(%d)\n", CmiMyPe(), phase));

  const static int strSize = 10;
  char phasestr[strSize+1];
  int _numEntries=_entryTable.size();
  // add strSize for phase number
  char *fname = 
    new char[strlen(CpvAccess(_logName))+strlen(".count.sts")+strSize];
  _MEMCHECK(fname);
  if (phase < 0 && lastPhase_ >= 0) { phase = lastPhase_; }
  if (phase >= 0) { 
    snprintf(phasestr, strSize, "%d", phase);
    phasestr[strSize] = '\0';
    sprintf(fname, "%s.phase%s.count.sts", CpvAccess(_logName), phasestr); 
  } 
  else { sprintf(fname, "%s.count.sts", CpvAccess(_logName)); }
  FILE *sts = fopen(fname, "w+");
  // DEBUGF(("%d/%d DEBUG: File: %s \n", CmiMyPe(), CmiNumPes(), fname));
  if(sts==0)
    CmiAbort("Cannot open summary sts file for writing.\n");
  CmiPrintf("WRITING FILE=%s\n", fname); 
  delete[] fname;
  fprintf(sts, "MACHINE %s\n",CMK_MACHINE_NAME);
  fprintf(sts, "PROCESSORS %d\n", CmiNumPes());
  fprintf(sts, "TOTAL_CHARES %d\n", _chareTable.size());
  fprintf(sts, "TOTAL_EPS %d\n", _numEntries+1);  // make extra overview
  fprintf(sts, "TOTAL_MSGS %d\n", _msgTable.size());
  fprintf(sts, "TOTAL_PSEUDOS %d\n", 0);
  fprintf(sts, "TOTAL_EVENTS %d\n", _numEvents);
  int i;
  for(i=0;i<_chareTable.size();i++)
    fprintf(sts, "CHARE %d %s\n", i, _chareTable[i]->name);
  for(i=0;i<_numEntries;i++)
    fprintf(sts, "ENTRY CHARE %d %s %d %d\n", i, _entryTable[i]->name,
                 _entryTable[i]->chareIdx, _entryTable[i]->msgIdx);
  // fake out, make OVERVIEW
  fprintf(sts, "ENTRY CHARE %d OVERVIEW -1 -1\n", _numEntries);
  for(i=0;i<_msgTable.size();i++)
    fprintf(sts, "MESSAGE %d %ld\n", i, _msgTable[i]->size);
  for(i=0;i<_numEvents;i++)
    fprintf(sts, "EVENT %d Event%d\n", i, i);
  fprintf(sts, "END\n");
  fclose(sts);
}

void CountLogPool::setEp(int epidx, 
			 int index1, long long count1, 
			 int index2, long long count2, 
			 double time) 
{
  // DEBUGF(("%d/%d DEBUG: CountLogPool::setEp %08x %d %d %d %f\n", 
  //        CmiMyPe(), CmiNumPes(), this, epidx, count1, count2, time));

  if (epidx >= MAX_ENTRIES) {
    CmiAbort("CountLogPool::setEp too many entry points!\n");
  }
  stats_.setEp(epidx, index1, count1, time);
  stats_.setEp(epidx, index2, count2, time);
}

//! constructor
TraceCounter::TraceCounter() :
  // comand line processing
  firstArg_      (NULL),
  lastArg_       (NULL),
  argStrSize_    (0),
  commandLine_   (NULL),
  commandLineSz_ (0),
  counter1_      (NULL),
  counter2_      (NULL),
  counter1Sz_    (0),
  counter2Sz_    (0),
  // result of different command line opts
  overview_      (false),
  switchRandom_  (false),
  switchByPhase_ (false),
  noLog_         (false),
  writeByPhase_  (false),
  // store between start/stop of counter read
  execEP_        (-1),
  startEP_       (0.0),
  genStart_      (-1),
  // store state
  idleTime_      (0.0),
  phase_         (-1),
  reductionPhase_(-1),
  traceOn_       (false),
  status_        (IDLE),
  dirty_         (false)
{
  for (int i=0; i<NUM_COUNTER_ARGS; i++) { registerArg(&COUNTER_ARG[i]); }
}

//! destructor
TraceCounter::~TraceCounter() { 
  delete [] commandLine_;
  traceClose(); 
}

//! process command line arguments!
void TraceCounter::traceInit(char **argv)
{
  CpvInitialize(CountLogPool*, _logPool);
  CpvInitialize(char*, _logName);
  CpvInitialize(double, version);
  CpvInitialize(char**, _counterNames);
  CpvInitialize(char**, _counterDesc);
  CpvInitialize(int,    _numCounters);
  CpvInitialize(int, _reductionID);

  CpvAccess(_logName) = (char *) malloc(strlen(argv[0])+1);
  _MEMCHECK(CpvAccess(_logName));
  strcpy(CpvAccess(_logName), argv[0]);
  CpvAccess(version) = VER;

  int i;
  // parse command line args
  char* counters = NULL;
  commandLine_ = NULL;
  bool badArg = false;
  int numCounters = 0;
  if (CmiGetArgStringDesc(argv, "+counters", &counters, "Measure these performance counters")) {
    if (CmiMyPe()==0) { CmiPrintf("Counters: %s\n", counters); }
    int offset = 0;
    int limit = strlen(counters);
    char* ptr = counters;
    while (offset < limit && 
	   (ptr = strtok(&counters[offset], ",")) != NULL) 
    { 
      offset += strlen(ptr)+1;
      ptr = &ptr[strlen(ptr)+1];
      numCounters++; 
    }
    if (CmiMyPe()==0) { 
      CmiPrintf("There are %d counters\n", numCounters); 
    }
    commandLine_ = new CounterArg[numCounters];
    ptr = counters;
    for (i=0; i<numCounters; i++) {
      commandLine_[i].arg = ptr;
      if (!matchArg(&commandLine_[i])) { 
	if (CmiMyPe()==0) { CmiPrintf("Bad arg: [%s]\n", ptr); }
	badArg = true; 
      }
      ptr = &ptr[strlen(ptr)+1];
    }
  }
  commandLineSz_ = numCounters;

  // check to see if args are valid, output if not
  if (badArg || CmiGetArgFlagDesc(argv, "+count-help", "List available performance counters")) {
    if (CmiMyPe() == 0) { printHelp(); }
    ConverseExit();  return;
  }
  else if (counters == NULL) {
    if (CmiMyPe() == 0) { usage(); }
    ConverseExit();  return;
  }

  // get optional command line args
  overview_      = CmiGetArgFlag(argv, "+count-overview");  
  switchRandom_  = CmiGetArgFlag(argv, "+count-switchrandom");  
  switchByPhase_ = CmiGetArgFlag(argv, "+count-switchbyphase");
  noLog_         = CmiGetArgFlag(argv, "+count-nolog");
  writeByPhase_  = CmiGetArgFlag(argv, "+count-writebyphase");
  char* logName  = NULL;
  if (CmiGetArgString(argv, "+count-logname", &logName)) {
    CpvAccess(_logName) = logName;
    if (noLog_) {
      if (CkMyPe()==0) {
	CmiPrintf("+count-logname and +count-nolog are MUTUALLY EXCLUSIVE\n");
	usage();
	CmiAbort("");
      }
    }
  }
  if (switchByPhase_ && overview_) {
    if (CkMyPe()==0) {
      CmiPrintf(
	"+count-switchbyphase and +count-overview are MUTUALLY EXCLUSIVE\n"
	"+count-overview automatically switches by phase.\n");
      usage();
      CmiAbort("");
    }
  }
  if (writeByPhase_ && noLog_) {
    if (CkMyPe()==0) {
      CmiPrintf("+count-writebyphase and +count-nolog are MUTUALLY EXCLUSIVE\n");
      usage();
      CmiAbort("");
    }
  }

  // parse through commandLine_, figure out which belongs on which list (1 vs 2)
  CounterArg* last1 = NULL;
  CounterArg* last2 = NULL;
  CounterArg* tmp = NULL;
  counter1Sz_ = counter2Sz_ = 0;
  for (i=0; i<commandLineSz_; i++) {
    tmp = &commandLine_[i];
    if (tmp->code < NUM_COUNTER_ARGS/2) {
      if (counter1_ == NULL) { counter1_ = tmp;  last1 = counter1_; }
      else { last1->next = tmp;  last1 = tmp; }
      counter1Sz_++;
    }
    else {
      if (counter2_ == NULL) { counter2_ = tmp;  last2 = counter2_; }
      else { last2->next = tmp;  last2 = tmp; }
      counter2Sz_++;
    }
  }
  if (counter1_ == NULL) {
    printHelp();
    if (CmiMyPe()==0) {
      CmiPrintf("\nMust specify some counters with code < %d\n", 
		NUM_COUNTER_ARGS/2);
    }
    ConverseExit();
  }
  if (counter2_ == NULL) {
    printHelp();
    if (CmiMyPe()==0) {
      CmiPrintf("\nMust specify some counters with code >= %d\n", 
		NUM_COUNTER_ARGS/2);
    }
    ConverseExit();
  }
  last1->next = counter1_;
  last2->next = counter2_;

  // all args valid, now set up logging
  if (CmiMyPe() == 0) {
    CmiPrintf("Running with tracemode=counter and args:\n");
    // print out counter1 set
    tmp = counter1_;
    i = 0;
    do {
      CmiPrintf("  <counter1-%d>=%d %s %s\n", i, tmp->code, tmp->arg, tmp->desc);
      tmp = tmp->next;
      i++;
    } while (tmp != counter1_);
    // print out counter2 set
    tmp = counter2_;
    i = 0;
    do {
      CmiPrintf("  <counter2-%d>=%d %s %s\n", i, tmp->code, tmp->arg, tmp->desc);
      tmp = tmp->next;
      i++;
    } while (tmp != counter2_);

    CmiPrintf(
      "+count-overview %d\n+count-switchrandom %d\n"
      "+count-switchbyphase %d\n+count-nolog %d\n"
      "+count-logname %s\n+count-writebyphase %d\n",
      overview_, switchRandom_, switchByPhase_, noLog_, 
      logName, writeByPhase_);
  }

  // DEBUGF(("    DEBUG: Counter1=%d Counter2=%d\n", counter1_, counter2_));
  CpvAccess(_logPool) = new CountLogPool();

  // allocate names so can do reduction/analysis on the fly
  char** counterNames = new char*[counter1Sz_+counter2Sz_];
  char** counterDesc = new char*[counter1Sz_+counter2Sz_];
  tmp = counter1_;
  for (i=0; i<counter1Sz_; i++) {
    tmp->index = i;
    counterNames[i] = tmp->arg; 
    counterDesc[i] = tmp->desc;
    tmp = tmp->next;
  }
  tmp = counter2_;
  for (i=0; i<counter2Sz_; i++) {
    tmp->index = counter1Sz_+i;
    counterNames[counter1Sz_+i] = tmp->arg; 
    counterDesc[counter1Sz_+i] = tmp->desc;
    tmp = tmp->next;
  }
  CpvAccess(_counterNames) = counterNames;
  CpvAccess(_counterDesc) = counterDesc;
  CpvAccess(_numCounters) = numCounters;
  // don't erase counterNames or counterDesc, 
  // the reduction client will do it on the final reduction

  _MEMCHECK(CpvAccess(_logPool));
  CpvAccess(_logPool)->init(numCounters);
  DEBUGF(("%d/%d DEBUG: Created _logPool at %08x\n", 
          CmiMyPe(), CmiNumPes(), CpvAccess(_logPool)));
}

//! turn trace on/off, note that charm will automatically call traceBegin()
//! at the beginning of every run unless the command line option "+traceoff"
//! is specified
void TraceCounter::traceBegin() {
  DEBUGF(("%d/%d traceBegin called\n", CmiMyPe(), CmiNumPes()));
  if (traceOn_) { 
      static bool print = true;
      if (print) {
	print = false;
	if (CmiMyPe()==0) {
	  CmiPrintf("%d/%d WARN: traceBegin called but trace already on!\n"
		    "            Sure you didn't mean to use +traceoff?\n",
		    CmiMyPe(), CmiNumPes());
	}
    } 
  }
  else {
    if (overview_) { beginOverview(); }
    idleTime_ = 0.0;
    if (writeByPhase_) { phase_++; }
    traceOn_ = true;
  }
}

//! turn trace on/off, note that charm will automatically call traceBegin()
//! at the beginning of every run unless the command line option "+traceoff"
//! is specified
void TraceCounter::traceEnd() {
  DEBUGF(("%d/%d traceEnd called\n", CmiMyPe(), CmiNumPes()));
  if (!traceOn_) { 
    static bool print = true;
    if (print) {
      print = false;
      if (CmiMyPe()==0) {
	CmiPrintf("%d/%d WARN: traceEnd called but trace not on!\n"
		  "            Sure you didn't mean to use +traceoff?\n",
		  CmiMyPe(), CmiNumPes());
      }
    }
  }
  else {
    traceOn_ = false;
    dirty_ = false;
    if (overview_) { endOverview(); }  // overview switches counters automatic
    else if (switchByPhase_) { switchCounters(); };

    if (!noLog_ && writeByPhase_) {
      if (CmiMyPe()==0) { CpvAccess(_logPool)->writeSts(phase_); }
      CpvAccess(_logPool)->write(phase_); 
    }
    reductionPhase_++;
    CpvAccess(_logPool)->doReduction(reductionPhase_, idleTime_); 
    if (writeByPhase_) {
      idleTime_ = 0.0;
      CpvAccess(_logPool)->clearEps(); 
    }
    // setTrace must go after the writes otherwise the writes won't go through
    DEBUGF(("%d/%d DEBUG: Created _logPool at %08x\n", 
	    CmiMyPe(), CmiNumPes(), CpvAccess(_logPool)));
  }
}

//! begin/end execution of a Charm++ entry point
//! NOTE: begin/endPack and begin/endUnpack can be called in between
//!       a beginExecute and its corresponding endExecute.
void TraceCounter::beginExecute(envelope *e)
{
  // no message means thread execution
  if (e==NULL) { beginExecute(-1,-1,CtvAccess(CthEP),-1); }
  else { beginExecute(-1,-1,e->getEpIdx(),-1); }  
}

//! begin/end execution of a Charm++ entry point
//! NOTE: begin/endPack and begin/endUnpack can be called in between
//!       a beginExecute and its corresponding endExecute.
void TraceCounter::beginExecute
(
  int event,
  int msgType,
  int ep,
  int srcPe, 
  int mlen,
  CmiObjId *idx
)
{
  DEBUGF(("%d/%d DEBUG: beginExecute EP %d\n", CmiMyPe(), CmiNumPes(), ep));

  if (!traceOn_ || overview_) { return; }

  execEP_=ep;

  if (status_!= IDLE) {
    static bool print = true;
    if (print) {
      print = false;
      if (CmiMyPe()==0) { 
	CmiPrintf("WARN: %d beginExecute called when status not IDLE!\n", 
		  CmiMyPe());
      }
    }
    return;
  }
  else { status_=WORKING; }

  if ((genStart_=start_counters(counter1_->code, counter2_->code))<0)
  {
    CmiPrintf("genStart=%d counter1=%d counter2=%d\n", 
              genStart_, counter1_->code, counter2_->code);
    CmiAbort("ERROR: start_counters() in beginExecute\n");
  }

  startEP_=TraceTimer();
  DEBUGF(("%d/%d DEBUG:   beginExecute EP %d genStart %d\n", 
          CmiMyPe(), CmiNumPes(), ep, genStart_));
}

//! begin/end execution of a Charm++ entry point
//! NOTE: begin/endPack and begin/endUnpack can be called in between
//!       a beginExecute and its corresponding endExecute.
void TraceCounter::endExecute()
{
  DEBUGF(("%d/%d DEBUG: endExecute EP %d genStart_ %d\n", 
          CmiMyPe(), CmiNumPes(), execEP_, genStart_));

  if (!traceOn_ || overview_) { return; }

  if (status_!=WORKING) {
    static bool print = true;
    if (print) {
      print = false;
      if (CmiMyPe()==0) {
	CmiPrintf("WARN: %d endExecute called when status not WORKING!\n", 
		  CmiMyPe());
      }
    }
    return;
  }
  else { status_=IDLE; }

  double t = TraceTimer();

  long long value1 = 0, value2 = 0;
  int genRead;
  if ((genRead=read_counters(counter1_->code, &value1, counter2_->code, &value2)) < 0 ||
      genRead != genStart_)
  {
    CmiPrintf("genRead %d genStart_ %d counter1 %ld counter2 %ld\n",
	      genRead, genStart_, value1, value2);
    traceClose();
    CmiAbort("ERROR: read_counters() in endExecute\n");
  }

  DEBUGF((
    "%d/%d DEBUG:   endExecute genRead %d Time %f counter1 %d counter2 %ld\n", 
    CmiMyPe(), CmiNumPes(), genRead, t-startEP_, value1, value2));
  if (execEP_ != -1) { 
    dirty_ = true;
    CpvAccess(_logPool)->setEp(execEP_, counter1_->index, value1, 
			       counter2_->index, value2, t-startEP_); 
    if (!switchByPhase_) { switchCounters(); }
  }
}

//! begin/end idle time for this pe
void TraceCounter::beginIdle(double curWallTime) {
  if (traceOn_) { startIdle_ = TraceTimer(curWallTime); } 
}

//! begin/end idle time for this pe
void TraceCounter::endIdle(double curWallTime) {
  if (traceOn_) { idleTime_ += TraceTimer(curWallTime)-startIdle_; }
}

//! begin/end the process of packing a message (to send)
void TraceCounter::beginPack() 
{ 
  DEBUGF(("%d/%d DEBUG: beginPack\n", CmiMyPe(), CmiNumPes()));

  // beginPack/endPack can get called between beginExecute/endExecute 
  // and can't have nested counter reads on certain architectures and on
  // on those architectures the time to call stop/start_counters can be
  // expensive
}

//! begin/end the process of packing a message (to send)
void TraceCounter::endPack() {
  DEBUGF(("%d/%d DEBUG: endPack\n", CmiMyPe(), CmiNumPes()));

  // beginPack/endPack can get called between beginExecute/endExecute 
  // and can't have nested counter reads on certain architectures and on
  // on those architectures the time to call stop/start_counters can be
  // expensive
}

//! begin/end the process of unpacking a message (can occur before calling
//! a entry point or during an entry point when 
void TraceCounter::beginUnpack() { 
  DEBUGF(("%d/%d DEBUG: beginUnpack\n", CmiMyPe(), CmiNumPes()));

  // beginUnpack/endUnpack can get called between beginExecute/endExecute 
  // and can't have nested counter reads on certain architectures and on
  // on those architectures the time to call stop/start_counters can be
  // expensive
}

//! begin/end the process of unpacking a message (can occur before calling
//! a entry point or during an entry point when 
void TraceCounter::endUnpack() {
  DEBUGF(("%d/%d DEBUG: endUnpack\n", CmiMyPe(), CmiNumPes()));
  
  // beginUnpack/endUnpack can get called between beginExecute/endExecute 
  // and can't have nested counter reads on certain architectures and on
  // on those architectures the time to call stop/start_counters can be
  // expensive
}

//! begin/end of execution
void TraceCounter::beginComputation()
{
}

//! end computation, do a reduction here in hopes that it finishes before 
//! traceClose called and the program exits
void TraceCounter::endComputation() {
  CpvAccess(_logPool)->doReduction(-1, idleTime_); 
}

//! clear all data collected for entry points
void TraceCounter::traceClearEps() {
  CpvAccess(_logPool)->clearEps();
}

//! write the summary sts file for this trace
void TraceCounter::traceWriteSts() {
  if (traceOn_ && !noLog_) {
    if (CmiMyPe()==0) { CpvAccess(_logPool)->writeSts(); }
  }
}

//! do any clean-up necessary for tracing
void TraceCounter::traceClose()
{
  if (dirty_) {
    if (overview_) { endOverview(); }
    else {
      if (!noLog_) {
	CpvAccess(_logPool)->write();
	if(CmiMyPe()==0) { 
	  CpvAccess(_logPool)->writeSts(); 
	  CmiPrintf("TraceCounter dirty, writing results\n");
	}
      }
    }
    dirty_ = false;
  }
  if (CpvAccess(_logPool)!=NULL) { 
    CkpvAccess(_trace)->endComputation();
    delete CpvAccess(_logPool);
    CpvAccess(_logPool) = NULL;
  }
}

//! start/stop the overall counting ov eps (don't write to logCount, 
//! just print to screen
void TraceCounter::beginOverview()
{
  DEBUGF(("%d/%d DEBUG:   beginOverview\n", 
          CmiMyPe(), CmiNumPes()));
  if ((genStart_=start_counters(counter1_->code, counter2_->code))<0)
  {
    CmiPrintf("genStart=%d counter1=%d counter2=%d\n", 
              genStart_, counter1_->code, counter2_->code);
    CmiAbort("ERROR: start_counters() in beginOverview\n");
  }
  startEP_=TraceTimer();
  DEBUGF(("%d/%d DEBUG:   beginOverview\n", CmiMyPe(), CmiNumPes()));
  dirty_ = true;
}

void TraceCounter::endOverview()
{
  DEBUGF(("%d/%d DEBUG:   endOverview\n", CmiMyPe(), CmiNumPes()));
 
  double t = TraceTimer();
  int _numEntries=_entryTable.size();

  long long value1 = 0, value2 = 0;
  int genRead;
  if ((genRead=read_counters(counter1_->code, &value1, counter2_->code, &value2)) < 0 ||
      genRead != genStart_)
  {
    CmiPrintf("genRead %d genStart_ %d counter1 %ld counter2 %ld\n",
	      genRead, genStart_, value1, value2);
    traceClose();
    CmiAbort("ERROR: read_counters() in endOverview\n");
  }

  DEBUGF((
    "%d/%d DEBUG:   endOverview genRead %d Time %f counter1 %ld counter2 %ld\n", 
    CmiMyPe(), CmiNumPes(), genRead, t-startEP_, value1, value2));
  dirty_ = false;

  CpvAccess(_logPool)->setEp(_numEntries, 
			     counter1_->index, value1, 
			     counter2_->index, value2, 
			     t-startEP_); 
  DEBUGF((
    "%d/%d OVERVIEW phase%d Time(us) %f %s %ld %s %ld Idle(us) %f"
    " (overflow? MAX=%ld)\n",
    CmiMyPe(), CmiNumPes(), phase_, (t-startEP_)*1e6, counter1_->arg, value1, 
    counter2_->arg, value2, idleTime_*1e6, MAXLONGLONG));
  // this is phase boundary anyway, so switch counters
  switchCounters(); 
}

//! switch counters by whatever switching strategy 
void TraceCounter::switchCounters()
{
  static bool first = true;
  if (switchRandom_) {
    int i;
    if (first) { first = false;  srand(TraceTimer()*1e6); }
    int counter1Change = 
      (int) (rand() / (double) 0x7FFF * counter1Sz_ + 0.5);  // 2^15-1
    int counter2Change = 
      (int) (rand() / (double) 0x7FFF * counter2Sz_ + 0.5);  // 2^15-1
    if (CmiMyPe()==0) {
      DEBUGF(("change %d/%d %d/%d\n", 
	      counter1Change, counter1Sz_, 
	      counter2Change, counter2Sz_));
    }
    if (counter1Change > counter1Sz_) { counter1Change = counter1Sz_; }
    if (counter2Change > counter2Sz_) { counter2Change = counter2Sz_; }
    for (i=0; i<counter1Change; i++) { counter1_ = counter1_->next; }
    for (i=0; i<counter2Change; i++) { counter2_ = counter2_->next; }
  }
  else {
    counter1_ = counter1_->next;
    counter2_ = counter2_->next;
  }
  if (CmiMyPe()==0) {
    DEBUGF(("%d/%d New counters are %s %s\n", 
	    CmiMyPe(), CmiNumPes(), counter1_->arg, counter2_->arg));
  }
}

//! add the argument parameters to the linked list of args choices
void TraceCounter::registerArg(CounterArg* arg)
{
  if (firstArg_ == NULL) {
    firstArg_ = lastArg_ = arg;
    argStrSize_ = strlen(arg->arg);
  }
  else { 
    // check to see if any redundancy 
    CounterArg* check = firstArg_;
    while (check != NULL) {
      if (strcmp(check->arg, arg->arg)==0 || check->code == arg->code) {
	if (CmiMyPe()==0) { 
	  CmiPrintf("Two args with same name %s or code %d\n", 
		    arg->arg, arg->code); 
	}
	CmiAbort("TraceCounter::registerArg()\n");
      }
      check = check->next;
    }

    lastArg_->next = arg;
    lastArg_ = arg;
    int len = strlen(arg->arg);
    if (len > argStrSize_) { argStrSize_ = len; }
  }
}

//! see if the arg (str or code) matches any in the linked list of choices
//! and sets arg->code to the SGI code
//! return true if arg matches, false otherwise
bool TraceCounter::matchArg(CounterArg* arg)
{
  bool match = false;                // will be set to true if arg matches
  CounterArg* matchArg = firstArg_;  // traverse linked list
  int matchCode = atoi(arg->arg);    // in case user specs num on commline
  if (matchCode == 0) {
    if (arg->arg[0] != '0' || arg->arg[1] != '\0') { matchCode = -1; }
  }
  // DEBUGF(("%d/%d DEBUG: Matching %s or %d\n", CmiMyPe(), CmiNumPes(), arg->arg, matchCode));
  while (matchArg != NULL && !match) {
    // DEBUGF(("%d/%d DEBUG:   Examining %d %s\n", CmiMyPe(), CmiNumPes(), matchArg->code, matchArg->arg));
    if (strcmp(matchArg->arg, arg->arg)==0) {
      match = true;
      arg->code = matchArg->code;
      arg->desc = matchArg->desc;
    }
    else if (matchArg->code == matchCode) {
      match = true;
      arg->code = matchArg->code;
      arg->arg = matchArg->arg;
      arg->desc = matchArg->desc;
    }
    matchArg = matchArg->next;
  }
  // DEBUGF(("%d/%d DEBUG: Match = %d\n", CmiMyPe(), CmiNumPes(), match));
  return match;
}

//! print out usage argument
void TraceCounter::usage() {
  CmiPrintf(
    "ERROR: You've linked with '-tracemode counter', so you must specify\n"
    "       the +counters <counters> option followed by any of the \n"
    "       following optional command line options.\n"
    "\n"
    "REQUIRED: +counters <counter>\n"
    "\n"
    "  +counters <counters>: Where <counters> is comma delimited list\n"
    "                        of valid counters.  Type '+count-help' to\n"
    "                        get a list of valid counters.\n"
    "\n"
    "OPTIONAL: [+count-overview] [+count-switchrandom] [+switchbyphase]\n"
    "\n"
    "  +count-overview:      Collect counter values between start/stop\n"
    "                        of the program (or traceBegin/traceEnd if\n"
    "                        user marked events are on [see Performance\n"
    "                        Counter section of the Charm++ manual]).\n"
    "                        Normal operation collects counter values\n"
    "                        between the stop/start of Charm++ entry\n"
    "                        points.\n"
    "  +count-switchrandom:  Counters will switch randomly between\n"
    "                        each event instead of in the order\n"
    "                        specified by the <counters> arg.\n"
    "  +count-switchbyphase: Counters will switch not every EP call,\n"
    "                        but only in between phases (between each\n"
    "                        traceBegin/traceEnd call).\n"
    "  +count-nolog:         Don't write any log files.\n"
    "\n"
    "See the Performance Counter section of the Charm++ manual for\n"
    "examples of different options.\n"
    "\n");
}

//! print out all arguments in the linked-list of choices
void TraceCounter::printHelp()
{
  CmiPrintf(
    "Specify one of the following (code or str) after +counters:\n\n"
    "  code  str\n"
    "  ----  ---\n");

  // create a format so that all the str line up 
  char format[64];
  snprintf(format, 64, "    %%2d  %%-%ds  %%s\n", argStrSize_);

  CounterArg* help = firstArg_;
  while (help != NULL) {
    CmiPrintf(format, help->code, help->arg, help->desc);
    help = help->next;
  }
}

#else   /* not implemented */
void _createTracecounter(char **argv)
{
}
#endif // CMK_ORIGIN2000

/*@}*/
