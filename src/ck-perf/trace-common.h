/**
 * \addtogroup CkPerf
*/
/*@{*/

#ifndef __TRACE_COMMON_H__
#define __TRACE_COMMON_H__


#include <stdlib.h>
#if defined(_WIN32)
#include <direct.h>
#define CHDIR _chdir
#define GETCWD _getcwd
#define PATHSEP '\\'
#define PATHSEPSTR "\\"
#else
#include <unistd.h>
#define CHDIR chdir
#define GETCWD getcwd
#define PATHSEP '/'
#define PATHSEPSTR "/"
#endif


#define  CREATION           1
#define  BEGIN_PROCESSING   2
#define  END_PROCESSING     3
#define  ENQUEUE            4
#define  DEQUEUE            5
#define  BEGIN_COMPUTATION  6
#define  END_COMPUTATION    7
#define  BEGIN_INTERRUPT    8
#define  END_INTERRUPT      9
#define  MESSAGE_RECV       10
#define  BEGIN_TRACE        11
#define  END_TRACE          12
#define  USER_EVENT         13
#define  BEGIN_IDLE         14
#define  END_IDLE           15
#define  BEGIN_PACK         16
#define  END_PACK           17
#define  BEGIN_UNPACK       18
#define  END_UNPACK         19
#define  CREATION_BCAST     20

#define  CREATION_MULTICAST 21

/* Memory tracing */
#define  MEMORY_MALLOC      24
#define  MEMORY_FREE        25

/* Trace user supplied data */
#define USER_SUPPLIED       26

/* Trace memory usage */
#define MEMORY_USAGE_CURRENT       27

/* Trace user supplied note (text string)  */
#define USER_SUPPLIED_NOTE       28

/* Trace user supplied note (text string, with start, end times, and user event id)  */
#define USER_SUPPLIED_BRACKETED_NOTE       29

/* Support for Phases and time-partial logs */
#define END_PHASE           30
#define SURROGATE_BLOCK     31 /* inserted by cluster analysis only */

/* Custom User Stats*/
#define USER_STAT           32

#define BEGIN_USER_EVENT_PAIR  98
#define END_USER_EVENT_PAIR    99
#define  USER_EVENT_PAIR    100

CkpvExtern(CmiInt8, CtrLogBufSize);
CkpvExtern(char*, traceRoot);
CkpvExtern(char*, partitionRoot);
CkpvExtern(int, traceRootBaseLength);
CkpvExtern(bool, verbose);
CkpvExtern(double, traceInitTime);
CkpvExtern(double, traceInitCpuTime);

#define  TRACE_TIMER   CmiWallTimer
#define  TRACE_CPUTIMER   CmiCpuTimer
inline double TraceTimer() { return TRACE_TIMER() - CkpvAccess(traceInitTime); }
inline double TraceTimer(double t) { return t - CkpvAccess(traceInitTime); }
inline double TraceCpuTimer() { return TRACE_CPUTIMER() - CkpvAccess(traceInitCpuTime); }
inline double TraceCpuTimer(double t) { return t - CkpvAccess(traceInitCpuTime); }

double TraceTimerCommon(); //TraceTimer to be used in common lrts layers

#define TRACE_WARN(msg) if (CkpvAccess(verbose)) CmiPrintf(msg)

extern bool outlierAutomatic;
extern bool findOutliers;
extern int numKSeeds;
extern int peNumKeep;
extern bool outlierUsePhases;
extern double entryThreshold;

/** Tracing-specific registered Charm entities: */
extern int _threadMsg, _threadChare, _threadEP;
extern int _packMsg, _packChare, _packEP;
extern int _unpackMsg, _unpackChare, _unpackEP;
extern int _sdagMsg, _sdagChare, _sdagEP;

/** Write out the common parts of the .sts file. */
extern void traceWriteSTS(FILE *stsfp,int nUserEvents);
void (*registerMachineUserEvents())();

#if CMK_HAS_COUNTER_PAPI
#include <papi.h>
#ifdef USE_SPP_PAPI
#define NUMPAPIEVENTS 6
#else
#define NUMPAPIEVENTS 2
#endif
CkpvExtern(int, papiEventSet);
CkpvExtern(LONG_LONG_PAPI*, papiValues);
CkpvExtern(int, papiStarted);
CkpvExtern(int, papiStopped);
CkpvExtern(int*, papiEvents);
CkpvExtern(int, numEvents);
void initPAPI();
#endif

#endif

/*@}*/
