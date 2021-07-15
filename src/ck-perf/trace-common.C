/**
 * \addtogroup CkPerf
*/
/*@{*/

#include <sys/stat.h>
#include <sys/types.h>

#include <ctime>        // std::time_t, std::gmtime
#include <chrono>       // std::chrono::system_clock

#include "charm.h"
#include "middle.h"
#include "cklists.h"
#include "ckliststring.h"

#include "trace.h"
#include "trace-common.h"
#include "allEvents.h"          //projector
#include "register.h" // for _entryTable

// To get username
#if defined(_WIN32) || defined(_WIN64)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <Lmcons.h>
#else
#include <pwd.h>
#endif

// To get hostname
#if defined(_WIN32) || defined(_WIN64)
#  include <Winsock2.h>
#else
#  include <unistd.h>
#  include <limits.h> // For HOST_NAME_MAX
#endif

#ifndef HOST_NAME_MAX
// Apple seems to not adhere to POSIX and defines this non-standard variable
// instead of HOST_NAME_MAX
#  ifdef MAXHOSTNAMELEN
#    define HOST_NAME_MAX MAXHOSTNAMELEN
// Windows docs say 256 bytes (so 255 + 1 added at use) will always be sufficient
#  else
#    define HOST_NAME_MAX 255
#  endif
#endif

CpvExtern(int, _traceCoreOn);   // projector

#if ! CMK_TRACE_ENABLED
static int warned = 0;
#define OPTIMIZE_WARNING if (!warned) { warned=1;  CmiPrintf("\n\n!!!! Warning: tracing not available without CMK_TRACE_ENABLED!\n");  return;  }
#else
#define OPTIMIZE_WARNING /*empty*/
#endif

#define DEBUGF(x)          // CmiPrintf x

CkpvDeclare(TraceArray*, _traces);		// lists of all trace modules

CkpvDeclare(bool,   dumpData);
CkpvDeclare(double, traceInitTime);
CkpvDeclare(double, traceInitCpuTime);
CpvDeclare(int, traceOn);
CkpvDeclare(int, traceOnPe);
CkpvDeclare(char*, traceRoot);
CkpvDeclare(char*, partitionRoot);
CkpvDeclare(int, traceRootBaseLength);
CkpvDeclare(char*, selective);
CkpvDeclare(bool, verbose);
bool outlierAutomatic;
bool findOutliers;
int numKSeeds;
int peNumKeep;
bool outlierUsePhases;
double entryThreshold;

typedef void (*mTFP)();                   // function pointer for
CpvStaticDeclare(mTFP, machineTraceFuncPtr);    // machine user event
                                          // registration

int _threadMsg, _threadChare, _threadEP;
int _packMsg, _packChare, _packEP;
int _unpackMsg, _unpackChare, _unpackEP;
int _sdagMsg, _sdagChare, _sdagEP;

CtvDeclare(int, curThreadEvent);
CpvDeclare(int, curPeEvent);

double TraceTimerCommon(){return TRACE_TIMER() - CkpvAccess(traceInitTime);}
#if CMK_TRACE_ENABLED
void CthSetEventInfo(CthThread t, int event, int srcPE);
#endif
/// decide parameters from command line
static void traceCommonInit(char **argv)
{
  CmiArgGroup("Charm++","Tracing");
  DEBUGF(("[%d] in traceCommonInit.\n", CkMyPe()));
  CkpvInitialize(double, traceInitTime);
  CkpvAccess(traceInitTime) = CmiStartTimer();
  CkpvInitialize(bool, dumpData);
  CkpvAccess(dumpData) = true;
  CkpvInitialize(double, traceInitCpuTime);
  CkpvAccess(traceInitCpuTime) = TRACE_CPUTIMER();
  CpvInitialize(int, traceOn);
  CpvAccess(traceOn) = 0;
  CpvInitialize(int, _traceCoreOn); //projector
  CpvAccess(_traceCoreOn)=0; //projector
  CpvInitialize(mTFP, machineTraceFuncPtr);
  CpvAccess(machineTraceFuncPtr) = NULL;
  CkpvInitialize(int, traceOnPe);
  CkpvAccess(traceOnPe) = 1;
  CkpvInitialize(bool, verbose);
  if (CmiGetArgFlag(argv, "+traceWarn")) {
    CkpvAccess(verbose) = true;
  } else {
    CkpvAccess(verbose) = false;
  }

  char *root=NULL;
  char *temproot;
  char *temproot2;
  CkpvInitialize(char*, traceRoot);
  CkpvInitialize(char*, partitionRoot);
  CkpvInitialize(int, traceRootBaseLength);
  /* Ctv variable to store Cthread Local event ID for Cthread tracing */
  CtvInitialize(int,curThreadEvent);
  CtvAccess(curThreadEvent)=0;
  /* Cpv variable to store current PE event ID for [local] and [inline] method tracing */
  CpvInitialize(int, curPeEvent);
  CpvAccess(curPeEvent)=0;

  char subdir[20];
  if(CmiNumPartitions() > 1) {
    sprintf(subdir, "prj.part%d%s", CmiMyPartition(), PATHSEPSTR);
  } else {
    subdir[0]='\0';
  }

  if (CmiGetArgStringDesc(argv, "+traceroot", &temproot, "Directory to write trace files to")) {
    int i;
    // Trying to decide if the traceroot path is absolute or not. If it is not
    // then create an absolute pathname for it.
    if (temproot[0] != PATHSEP) {
      temproot2 = GETCWD(NULL,0);
      root = (char *)malloc(strlen(temproot2)+1+strlen(temproot)+1);
      strcpy(root, temproot2);
      strcat(root, PATHSEPSTR);
      strcat(root, temproot);
    } else {
      root = (char *)malloc(strlen(temproot)+1);
      strcpy(root,temproot);
    }
    for (i=strlen(argv[0])-1; i>=0; i--) if (argv[0][i] == PATHSEP) break;
    i++;
    CkpvAccess(traceRootBaseLength) = strlen(root)+1;
    CkpvAccess(traceRoot) = (char *)malloc(strlen(argv[0]+i) + strlen(root) + 2 +strlen(subdir));    _MEMCHECK(CkpvAccess(traceRoot));
    CkpvAccess(partitionRoot) = (char *)malloc(strlen(argv[0]+i) + strlen(root) + 2 +strlen(subdir));    _MEMCHECK(CkpvAccess(partitionRoot));
    strcpy(CkpvAccess(traceRoot), root);
    strcat(CkpvAccess(traceRoot), PATHSEPSTR);
    strcat(CkpvAccess(traceRoot), subdir);
    strcpy(CkpvAccess(partitionRoot),CkpvAccess(traceRoot));
    strcat(CkpvAccess(traceRoot), argv[0]+i);
  }
  else {
    CkpvAccess(traceRoot) = (char *) malloc(strlen(argv[0])+1 +strlen(subdir));
    _MEMCHECK(CkpvAccess(traceRoot));
    CkpvAccess(partitionRoot) = (char *) malloc(strlen(argv[0])+1 +strlen(subdir));
    _MEMCHECK(CkpvAccess(partitionRoot));
    strcpy(CkpvAccess(traceRoot), subdir);
    strcpy(CkpvAccess(partitionRoot),CkpvAccess(traceRoot));
    strcat(CkpvAccess(traceRoot), argv[0]);
  }
  CkpvAccess(traceRootBaseLength)  +=  strlen(subdir);
	/* added for TAU trace module. */
  char *cwd;
  CkpvInitialize(char*, selective);
  if (CmiGetArgStringDesc(argv, "+selective", &temproot, "TAU's selective instrumentation file")) {
    // Trying to decide if the traceroot path is absolute or not. If it is not
    // then create an absolute pathname for it.
    if (temproot[0] != PATHSEP) {
      cwd = GETCWD(NULL,0);
      root = (char *)malloc(strlen(cwd)+strlen(temproot)+2);
      strcpy(root, cwd);
      strcat(root, PATHSEPSTR);
      strcat(root, temproot);
    } else {
      root = (char *)malloc(strlen(temproot)+1);
      strcpy(root,temproot);
    }
    CkpvAccess(selective) = (char *) malloc(strlen(root)+1);
    _MEMCHECK(CkpvAccess(selective));
    strcpy(CkpvAccess(selective), root);
    if (CkMyPe() == 0) 
      CmiPrintf("Trace: selective: %s\n", CkpvAccess(selective));
  }
  else {
    CkpvAccess(selective) = (char *) malloc(3);
    _MEMCHECK(CkpvAccess(selective));
    strcpy(CkpvAccess(selective), "");
  }

  outlierAutomatic = true;
  findOutliers = false;
  numKSeeds = 10;
  peNumKeep = CkNumPes();
  outlierUsePhases = false;
  entryThreshold = 0.0;
  //For KMeans
  if (outlierAutomatic) {
    CmiGetArgIntDesc(argv, "+outlierNumSeeds", &numKSeeds,
		     "Number of cluster seeds to apply at outlier analysis.");
    CmiGetArgIntDesc(argv, "+outlierPeNumKeep", 
		     &peNumKeep, "Number of Processors to retain data");
    CmiGetArgDoubleDesc(argv, "+outlierEpThresh", &entryThreshold,
			"Minimum significance of entry points to be considered for clustering (%).");
    findOutliers =
      CmiGetArgFlagDesc(argv,"+outlier", "Find Outliers.");
    outlierUsePhases = 
      CmiGetArgFlagDesc(argv,"+outlierUsePhases",
			"Apply automatic outlier analysis to any available phases.");
    if (outlierUsePhases) {
      // if the user wants to use an outlier feature, it is assumed outlier
      //    analysis is desired.
      findOutliers = true;
    }
    if(root)
        free(root);
  }

  
  
  if(CkMyRank()==0) {
    _threadMsg = CkRegisterMsg("dummy_thread_msg", 0, 0, 0, 0);
    _threadChare = CkRegisterChare("dummy_thread_chare", 0, TypeInvalid);
    CkRegisterChareInCharm(_threadChare);
    _threadEP = CkRegisterEp("dummy_thread_ep", 0, _threadMsg,_threadChare, 0+CK_EP_INTRINSIC);

    _packMsg = CkRegisterMsg("dummy_pack_msg", 0, 0, 0, 0);
    _packChare = CkRegisterChare("dummy_pack_chare", 0, TypeInvalid);
    CkRegisterChareInCharm(_packChare);
    _packEP = CkRegisterEp("dummy_pack_ep", 0, _packMsg,_packChare, 0+CK_EP_INTRINSIC);

    _unpackMsg = CkRegisterMsg("dummy_unpack_msg", 0, 0, 0, 0);
    _unpackChare = CkRegisterChare("dummy_unpack_chare", 0, TypeInvalid);
    CkRegisterChareInCharm(_unpackChare);
    _unpackEP = CkRegisterEp("dummy_unpack_ep", 0, _unpackMsg,_unpackChare, 0+CK_EP_INTRINSIC);

    _sdagMsg = CkRegisterMsg("sdag_msg", 0, 0, 0, 0);
    _sdagChare = CkRegisterChare("SDAG", 0, TypeInvalid);
    CkRegisterChareInCharm(_sdagChare);
    _sdagEP = CkRegisterEp("SDAG_RTS", 0, _sdagMsg, _sdagChare, 0+CK_EP_INTRINSIC);
  }
}

extern char** Cmi_argvcopy;
extern const char* const CmiCommitID;

/** Write out the common parts of the .sts file. */
void traceWriteSTS(FILE *stsfp,int nUserEvents) {
  fprintf(stsfp, "MACHINE \"%s\"\n",CMK_MACHINE_NAME);
#if CMK_SMP_TRACE_COMMTHREAD && CMK_SMP && !CMK_SMP_NO_COMMTHD
  //Assuming there's only 1 comm thread now! --Chao Mei
  //considering the extra comm thread per node
  fprintf(stsfp, "PROCESSORS %d\n", CkNumPes()+CkNumNodes());
#else
  fprintf(stsfp, "PROCESSORS %d\n", CkNumPes());
#endif
#ifdef CMK_SMP
  fprintf(stsfp, "SMPMODE %d %d\n", CkMyNodeSize(), CkNumNodes());
#endif

#if defined(_WIN32) || defined(_WIN64)
  TCHAR username[UNLEN + 1];
  DWORD size = UNLEN + 1;
  if (GetUserName(username, &size))
    fprintf(stsfp, "USERNAME \"%s\"\n", username);
#else
  const struct passwd* pw = getpwuid(getuid());
  if (pw != nullptr)
    fprintf(stsfp, "USERNAME \"%s\"\n", pw->pw_name);
#endif

  // Add 1 for null terminator
  char hostname[HOST_NAME_MAX + 1];
  if(gethostname(hostname, HOST_NAME_MAX + 1) == 0)
    fprintf(stsfp, "HOSTNAME \"%s\"\n", hostname);

  fprintf(stsfp, "COMMANDLINE \"");
  int index = 0;
  while (Cmi_argvcopy[index] != nullptr)
  {
    if (index > 0)
      fprintf(stsfp, " ");
    fprintf(stsfp, "%s", Cmi_argvcopy[index]);
    index++;
  }
  fprintf(stsfp, "\"\n");

  // write timestamp in ISO 8601 format
  using std::chrono::system_clock;
  const time_t now = system_clock::to_time_t(system_clock::now());
  struct tm currentTime;
#if defined(_WIN32) || defined(_WIN64)
  gmtime_s(&currentTime, &now);
#else
  gmtime_r(&now, &currentTime);
#endif
  char timeBuffer[sizeof("YYYY-mm-ddTHH:MM:SSZ")];
  strftime(timeBuffer, sizeof(timeBuffer), "%FT%TZ", &currentTime);
  fprintf(stsfp, "TIMESTAMP %s\n", timeBuffer);

  fprintf(stsfp, "CHARMVERSION %s\n", CmiCommitID);
  fprintf(stsfp, "TOTAL_CHARES %d\n", (int)_chareTable.size());
  fprintf(stsfp, "TOTAL_EPS %d\n", (int)_entryTable.size());
  fprintf(stsfp, "TOTAL_MSGS %d\n", (int)_msgTable.size());
  fprintf(stsfp, "TOTAL_PSEUDOS %d\n", (int)0);
  fprintf(stsfp, "TOTAL_EVENTS %d\n", (int)nUserEvents);
  size_t i;
  for(i=0;i<_chareTable.size();i++)
    fprintf(stsfp, "CHARE %d \"%s\" %d\n", (int)i, _chareTable[i]->name, _chareTable[i]->ndims);
  for(i=0;i<_entryTable.size();i++)
    fprintf(stsfp, "ENTRY CHARE %d \"%s\" %d %d\n", (int)i, _entryTable[i]->name,
                 (int)_entryTable[i]->chareIdx, (int)_entryTable[i]->msgIdx);
  for(i=0;i<_msgTable.size();i++)
    fprintf(stsfp, "MESSAGE %d %u\n", (int)i, (int)_msgTable[i]->size);
}

void traceCommonBeginIdle(void *proj,double curWallTime)
{
  ((TraceArray *)proj)->beginIdle(curWallTime);
}
 
void traceCommonEndIdle(void *proj,double curWallTime)
{
  ((TraceArray *)proj)->endIdle(curWallTime);
}

void TraceArray::traceBegin() {
  if (n==0) return; // No tracing modules registered.
#if ! CMK_TRACE_IN_CHARM
  cancel_beginIdle = CcdCallOnConditionKeep(CcdPROCESSOR_BEGIN_IDLE,(CcdVoidFn)traceCommonBeginIdle,this);
  cancel_endIdle = CcdCallOnConditionKeep(CcdPROCESSOR_BEGIN_BUSY,(CcdVoidFn)traceCommonEndIdle,this);
#endif
  ALLDO(traceBegin());
}

void TraceArray::traceBeginOnCommThread() {
#if CMK_SMP_TRACE_COMMTHREAD
  if (n==0) return; // No tracing modules registered.
/*#if ! CMK_TRACE_IN_CHARM	
  cancel_beginIdle = CcdCallOnConditionKeep(CcdPROCESSOR_BEGIN_IDLE,(CcdVoidFn)traceCommonBeginIdle,this);
  cancel_endIdle = CcdCallOnConditionKeep(CcdPROCESSOR_BEGIN_BUSY,(CcdVoidFn)traceCommonEndIdle,this);
#endif*/
  ALLDO(traceBeginOnCommThread());
#endif
}

void TraceArray::traceEnd() {
  if (n==0) return; // No tracing modules registered.
  ALLDO(traceEnd());
#if ! CMK_TRACE_IN_CHARM
  CcdCancelCallOnConditionKeep(CcdPROCESSOR_BEGIN_IDLE, cancel_beginIdle);
  CcdCancelCallOnConditionKeep(CcdPROCESSOR_BEGIN_BUSY, cancel_endIdle);
#endif
}

void TraceArray::traceEndOnCommThread() {
#if CMK_SMP_TRACE_COMMTHREAD
  if (n==0) return; // No tracing modules registered.
  ALLDO(traceEndOnCommThread());
/*#if ! CMK_TRACE_IN_CHARM
  CcdCancelCallOnConditionKeep(CcdPROCESSOR_BEGIN_IDLE, cancel_beginIdle);
  CcdCancelCallOnConditionKeep(CcdPROCESSOR_BEGIN_BUSY, cancel_endIdle);
#endif*/
#endif
}

#if CMK_MULTICORE
extern int Cmi_commthread;
#endif

/*Install the beginIdle/endIdle condition handlers.*/
void traceBegin(void) {
#if CMK_TRACE_ENABLED
  DEBUGF(("[%d] traceBegin called with %d at %f\n", CkMyPe(), CpvAccess(traceOn), TraceTimer()));
  
#if CMK_SMP_TRACE_COMMTHREAD
  //the first core of this node controls the condition of comm thread
#if CMK_MULTICORE
  if (Cmi_commthread)
#endif
  if(CmiMyRank()==0){
	if(CpvAccessOther(traceOn, CmiMyNodeSize())!=1){
		CkpvAccessOther(_traces, CmiMyNodeSize())->traceBeginOnCommThread();		
		CpvAccessOther(traceOn, CmiMyNodeSize()) = 1;
	}
  }
#endif
  if (CpvAccess(traceOn)==1) return;
  CkpvAccess(_traces)->traceBegin();
  CpvAccess(traceOn) = 1;
#endif
}

/*Cancel the beginIdle/endIdle condition handlers.*/
void traceEnd(void) {
#if CMK_TRACE_ENABLED
  DEBUGF(("[%d] traceEnd called with %d at %f\n", CkMyPe(), CpvAccess(traceOn), TraceTimer()));

#if CMK_SMP_TRACE_COMMTHREAD
//the first core of this node controls the condition of comm thread
#if CMK_MULTICORE
  if (Cmi_commthread)
#endif
  if(CmiMyRank()==0){
	if(CkpvAccessOther(traceOn, CmiMyNodeSize())!=0){
		CkpvAccessOther(_traces, CmiMyNodeSize())->traceEndOnCommThread();
		CkpvAccessOther(traceOn, CmiMyNodeSize()) = 0;
	}
}
#endif
	
	
  if (CpvAccess(traceOn)==0) return;
  if (CkpvAccess(_traces) == NULL) {
    CmiPrintf("Warning: did you mix compilation with and without -DCMK_TRACE_ENABLED? \n");
  }
  CkpvAccess(_traces)->traceEnd();
  CpvAccess(traceOn) = 0;
#endif
}

void traceBeginComm(void) {
#if CMK_TRACE_ENABLED && CMK_SMP_TRACE_COMMTHREAD
#if CMK_MULTICORE
  if (Cmi_commthread)
#endif
    if (CmiMyRank() == 0) {
      if (CkpvAccessOther(traceOn, CmiMyNodeSize()) != 1) {
        CkpvAccessOther(_traces, CmiMyNodeSize())->traceBeginOnCommThread();
        CkpvAccessOther(traceOn, CmiMyNodeSize()) = 1;
      }
    }
#endif
}

void traceEndComm(void) {
#if CMK_TRACE_ENABLED && CMK_SMP_TRACE_COMMTHREAD
#if CMK_MULTICORE
  if (Cmi_commthread)
#endif
    if (CmiMyRank() == 0) {
      if (CkpvAccessOther(traceOn, CmiMyNodeSize()) != 0) {
        CkpvAccessOther(_traces, CmiMyNodeSize())->traceEndOnCommThread();
        CkpvAccessOther(traceOn, CmiMyNodeSize()) = 0;
      }
    }
#endif
}

static int checkTraceOnPe(char **argv)
{
  int traceOnPE = 1;
  char *procs = NULL;
  if (CmiGetArgStringDesc(argv, "+traceprocessors", &procs, "A list of processors to trace, e.g. 0,10,20-30"))
  {
    CkListString procList(procs);
    traceOnPE = procList.includes(CkMyPe());
  }

  if (CmiGetArgFlagDesc(argv, "+traceselective", " Whether only dump data for PEs based on perfReport"))
  {
      if(CkMyPe() !=0)
          CkpvAccess(dumpData) = false;
  }

  // must include pe 0, otherwise sts file is not generated
  if (CkMyPe()==0) traceOnPE = 1;
#if !CMK_TRACE_IN_CHARM
#if !CMK_SMP_TRACE_COMMTHREAD
  /* skip communication thread */
  traceOnPE = traceOnPE && (CkMyRank() != CkMyNodeSize());
#endif
#endif
  return traceOnPE;
}

/// defined in moduleInit.C
void _createTraces(char **argv);


bool enableCPTracing; // A global variable specifying whether or not the control point tracing module should be active in the run
extern void _registerTraceControlPoints();
extern void _createTracecontrolPoints(char **argv);


/**
    traceInit: 		called at Converse level
    traceCharmInit:	called at Charm++ level
*/
/// initialize trace framework, also create the trace module(s).
static inline void _traceInit(char **argv) 
{
  CkpvInitialize(TraceArray *, _traces);
  CkpvAccess(_traces) = new TraceArray;

  // common init
  traceCommonInit(argv);

  // check if trace is turned on/off for this pe
  CkpvAccess(traceOnPe) = checkTraceOnPe(argv);

#if !CMK_CHARM4PY
  // defined in moduleInit.C
  _createTraces(argv);
#endif

  // Now setup the control point tracing module if desired. It is always compiled/linked in, but is not always enabled
  // FIXME: make sure it is safe to use argv in SMP version 
  // because CmiGetArgFlagDesc is destructive and this is called on all PEs.
  if( CmiGetArgFlagDesc(argv,"+CPEnableMeasurements","Enable recording of measurements for Control Points") ){
    enableCPTracing = true;
    _createTracecontrolPoints(argv);   
  } else {
    enableCPTracing = false;
  }
  

  // set trace on/off
  CkpvAccess(_traces)->setTraceOnPE(CkpvAccess(traceOnPe));

#if CMK_SMP_TRACE_COMMTHREAD
/**
 * In traceBegin(), CkpvAccessOther will be used which means
 * this core needs to access to some cpv variable on another 
 * core in the same memory address space. It's possible the
 * variable on the other core has not been initialized, which
 * implies the CpvAcessOther will cause a bad memory access.
 * Therefore, we need a barrier here for the traceCommonInit to
 * finish here. -Chao Mei
 */
   CmiBarrier();
#endif

  if (CkpvAccess(_traces)->length()) {
    if (CkMyPe() == 0) 
      CmiPrintf("Trace: traceroot: %s\n", CkpvAccess(traceRoot));
    if (!CmiGetArgFlagDesc(argv,"+traceoff","Disable tracing"))
      traceBegin();
  }
}

/// Converse version
void traceInit(char **argv) 
{
#if ! CMK_TRACE_IN_CHARM
  _traceInit(argv);
  initTraceCore(argv);
#endif
}

/// Charm++ version
void traceCharmInit(char **argv) 
{
#if CMK_TRACE_IN_CHARM
  _traceInit(argv);
#endif
}

// CMK_TRACE_ENABLED is already guarded in convcore.C
void traceMessageRecv(char *msg, int pe)
{
#if ! CMK_TRACE_IN_CHARM
  CkpvAccessOther(_traces, CmiRankOf(pe))->messageRecv(msg, pe);
#endif
}


void traceBeginIdle()
{
    _TRACE_ONLY(CkpvAccess(_traces)->beginIdle(CmiWallTimer()));
}


void traceEndIdle()
{
    _TRACE_ONLY(CkpvAccess(_traces)->endIdle(CmiWallTimer()));
}

// CMK_TRACE_ENABLED is already guarded in convcore.C
void traceResume(int eventID, int srcPE, int ep, CmiObjId *tid)
{
    _TRACE_BEGIN_EXECUTE_DETAILED(eventID, ForChareMsg, ep, srcPE, 0, NULL, tid);
    if(CpvAccess(_traceCoreOn))
	    resumeTraceCore();
}

void traceSuspend(void)
{
  _TRACE_ONLY(CkpvAccess(_traces)->endExecute());
}

void traceAwaken(CthThread t)
{
  CkpvAccess(_traces)->creation(0, _threadEP);
#if CMK_TRACE_ENABLED
  CthSetEventInfo(t, CtvAccess(curThreadEvent), CkMyPe());
#endif
}

void traceUserEvent(int e)
{
#if CMK_TRACE_ENABLED
  if (CpvAccess(traceOn))
    CkpvAccess(_traces)->userEvent(e);
#endif
}


void beginAppWork()
{
#if CMK_TRACE_ENABLED
    if (CpvAccess(traceOn) && CkpvAccess(_traces))
    {
        CkpvAccess(_traces)->beginAppWork();
    }
#endif
}


void endAppWork()
{
#if CMK_TRACE_ENABLED
    if (CpvAccess(traceOn) && CkpvAccess(_traces))
    {
        CkpvAccess(_traces)->endAppWork();
    }
#endif
}

void countNewChare()
{
#if CMK_TRACE_ENABLED
    if (CpvAccess(traceOn) && CkpvAccess(_traces))
    {
        CkpvAccess(_traces)->countNewChare();
    }
#endif
}


void beginTuneOverhead()
{
#if CMK_TRACE_ENABLED
    if (CpvAccess(traceOn) && CkpvAccess(_traces))
    {
        CkpvAccess(_traces)->beginTuneOverhead();
    }
#endif
}

void endTuneOverhead()
{
#if CMK_TRACE_ENABLED
    if (CpvAccess(traceOn) && CkpvAccess(_traces))
    {
        CkpvAccess(_traces)->endTuneOverhead();
    }
#endif
}

void traceUserBracketEvent(int e, double beginT, double endT)
{
#if CMK_TRACE_ENABLED
  if (CpvAccess(traceOn) && CkpvAccess(_traces))
    CkpvAccess(_traces)->userBracketEvent(e, beginT, endT);
#endif
}

// trace a UserBracketEvent that is coming from a "nested" thread, e.g. a virtual AMPI rank
void traceUserBracketEventNestedID(int e, double beginT, double endT, int nestedID)
{
#if CMK_TRACE_ENABLED
  if (CpvAccess(traceOn) && CkpvAccess(_traces))
    CkpvAccess(_traces)->userBracketEvent(e, beginT, endT, nestedID);
#endif
}

void traceBeginUserBracketEvent(int e)
{
#if CMK_TRACE_ENABLED
  if (CpvAccess(traceOn) && CkpvAccess(_traces))
    CkpvAccess(_traces)->beginUserBracketEvent(e);
#endif
}

void traceBeginUserBracketEventNestedID(int e, int nestedID)
{
#if CMK_TRACE_ENABLED
  if (CpvAccess(traceOn) && CkpvAccess(_traces))
    CkpvAccess(_traces)->beginUserBracketEvent(e, nestedID);
#endif
}

void traceEndUserBracketEvent(int e)
{
#if CMK_TRACE_ENABLED
  if (CpvAccess(traceOn) && CkpvAccess(_traces))
    CkpvAccess(_traces)->endUserBracketEvent(e);
#endif
}

void traceEndUserBracketEventNestedID(int e, int nestedID)
{
#if CMK_TRACE_ENABLED
  if (CpvAccess(traceOn) && CkpvAccess(_traces))
    CkpvAccess(_traces)->endUserBracketEvent(e, nestedID);
#endif
}

//common version of User Stat Functions
int traceRegisterUserStat(const char*x, int e)
{
#if CMK_TRACE_ENABLED
  return CkpvAccess(_traces)->traceRegisterUserStat(x, e);
#else
  return 0;
#endif
}

void updateStatPair(int e, double stat, double time)
{
#if CMK_TRACE_ENABLED
  if (CpvAccess(traceOn) && CkpvAccess(_traces))
    CkpvAccess(_traces)->updateStatPair(e, stat, time);
#endif
}

void updateStat(int e, double stat)
{
#if CMK_TRACE_ENABLED
  if (CpvAccess(traceOn) && CkpvAccess(_traces))
    CkpvAccess(_traces)->updateStat(e, stat);
#endif
}

void traceUserSuppliedData(int d)
{
#if CMK_TRACE_ENABLED
  if (CpvAccess(traceOn) && CkpvAccess(_traces))
    CkpvAccess(_traces)->userSuppliedData(d);
#endif
}

void traceUserSuppliedNote(const char * note)
{
#if CMK_TRACE_ENABLED
  if (CpvAccess(traceOn) && CkpvAccess(_traces))
    CkpvAccess(_traces)->userSuppliedNote(note);
#endif
}


void traceUserSuppliedBracketedNote(const char *note, int eventID, double bt, double et)
{
  //CkPrintf("traceUserSuppliedBracketedNote(const char *note, int eventID, double bt, double et)\n");
#if CMK_TRACE_ENABLED
  if (CpvAccess(traceOn) && CkpvAccess(_traces))
    CkpvAccess(_traces)->userSuppliedBracketedNote(note, eventID, bt, et);
#endif
}


void traceMemoryUsage()
{
#if CMK_TRACE_ENABLED
  double d = CmiMemoryUsage()*1.0;

  if (CpvAccess(traceOn) && CkpvAccess(_traces))
    CkpvAccess(_traces)->memoryUsage(d);
#endif
}

void tracePhaseEnd()
{
  _TRACE_ONLY(CkpvAccess(_traces)->endPhase());
}

void registerMachineUserEventsFunction(void (*eventRegistrationFunc)()) {
  CmiAssert(CpvInitialized(machineTraceFuncPtr));
  CpvAccess(machineTraceFuncPtr) = eventRegistrationFunc;
}

void (*registerMachineUserEvents())() {
  CmiAssert(CpvInitialized(machineTraceFuncPtr));
  if (CpvAccess(machineTraceFuncPtr) != NULL) {
    return CpvAccess(machineTraceFuncPtr);
  } else {
    return NULL;
  }
}

int traceRegisterUserEvent(const char*x, int e)
{
#if CMK_TRACE_ENABLED
  return CkpvAccess(_traces)->traceRegisterUserEvent(x, e);
#else
  return 0;
#endif
}

void traceClearEps(void)
{
  OPTIMIZE_WARNING
  CkpvAccess(_traces)->traceClearEps();
}

void traceWriteSts(void)
{
  OPTIMIZE_WARNING
  CkpvAccess(_traces)->traceWriteSts();
}

void traceFlushLog(void)
{
  OPTIMIZE_WARNING
  CkpvAccess(_traces)->traceFlushLog();
}

/**
    traceClose: 	this function is called at Converse
    traceCharmClose:	called at Charm++ level
*/
void traceClose(void)
{
  OPTIMIZE_WARNING
  CkpvAccess(_traces)->traceClose();
}

void traceCharmClose(void)
{
}

/* **CW** This is the API called from user code to support CCS operations 
   if supported by the underlying trace module.
 */
void traceEnableCCS(void)
{
  OPTIMIZE_WARNING
  CkpvAccess(_traces)->traceEnableCCS();  
}

/* **CW** Support for thread listeners. This makes a call to each
   trace module which must support the call.
*/
void traceAddThreadListeners(CthThread tid, envelope *e) {
  _TRACE_ONLY(CkpvAccess(_traces)->traceAddThreadListeners(tid, e));
}

#if 1
// helper functions
extern int _charmHandlerIdx;
class CkCoreState;
extern void _processHandler(void *, CkCoreState*);
int isCharmEnvelope(void *msg);
int CkIsCharmMessage(char *msg)
{
//CmiPrintf("[%d] CkIsCharmMessage: %d %p %d %p\n", CkMyPe(),CmiGetHandler(msg), CmiGetHandlerFunction(msg), _charmHandlerIdx, _processHandler);
  if ((CmiGetHandler(msg) == _charmHandlerIdx) &&
         (CmiGetHandlerFunction(msg) == (CmiHandlerEx)_processHandler))
    return 1;
  if (CmiGetXHandler(msg) == _charmHandlerIdx) return isCharmEnvelope(msg);
  return 0;
}
#endif

// return 1 if any one of tracing modules is linked.
int  traceAvailable()
{
#if ! CMK_TRACE_ENABLED
  return 0;
#else
  return CkpvAccess(_traces)->length()>0;
#endif
}

double CmiTraceTimer()
{
  return TraceTimer();
}

void TraceArray::creation(envelope *env, int ep, int num)
{ 
    if (_entryTable[ep]->traceEnabled)
        ALLDO(creation(env, ep, num));
}

void TraceArray::creationMulticast(envelope *env, int ep, int num,
				   const int *pelist)
{
  if (_entryTable[ep]->traceEnabled)
    ALLDO(creationMulticast(env, ep, num, pelist));
}

#if CMK_SMP_TRACE_COMMTHREAD
int traceBeginCommOp(char *msg){
#if CMK_TRACE_ENABLED
  if (CpvAccess(traceOn) && CkpvAccess(_traces) && CkIsCharmMessage(msg)) {
    CkpvAccess(_traces)->beginExecute(msg);
    return 1;
  }
#endif
  return 0;
}

void traceEndCommOp(char *msg){
#if CMK_TRACE_ENABLED
  if (CpvAccess(traceOn) && CkpvAccess(_traces) && CkIsCharmMessage(msg))
    CkpvAccess(_traces)->endExecute(msg);
#endif
}

void traceSendMsgComm(char *msg){
#if CMK_TRACE_ENABLED
  if (CpvAccess(traceOn) && CkpvAccess(_traces) && CkIsCharmMessage(msg))
    CkpvAccess(_traces)->creation(msg);
#endif
}

void traceCommSetMsgID(char *msg){
#if CMK_TRACE_ENABLED
  if (CpvAccess(traceOn) && CkpvAccess(_traces) && CkIsCharmMessage(msg))
    CkpvAccess(_traces)->traceCommSetMsgID(msg);
#endif
}

#endif

void traceGetMsgID(char *msg, int *pe, int *event)
{
#if CMK_TRACE_ENABLED
  if (CpvAccess(traceOn) && CkpvAccess(_traces) && CkIsCharmMessage(msg))
    CkpvAccess(_traces)->traceGetMsgID(msg, pe, event);
#endif
}

void traceSetMsgID(char *msg, int pe, int event)
{
#if CMK_TRACE_ENABLED
  if (CpvAccess(traceOn) && CkpvAccess(_traces) && CkIsCharmMessage(msg))
    CkpvAccess(_traces)->traceSetMsgID(msg, pe, event);
#endif
}


void traceChangeLastTimestamp(double ts){
#if CMK_TRACE_ENABLED
  if (CpvAccess(traceOn) && CkpvAccess(_traces))
    CkpvAccess(_traces)->changeLastEntryTimestamp(ts);
#endif
}

#if CMK_HAS_COUNTER_PAPI
CkpvDeclare(int, papiEventSet);
CkpvDeclare(LONG_LONG_PAPI*, papiValues);
CkpvDeclare(int, papiStarted);
CkpvDeclare(int, papiStopped);
CkpvDeclare(int*, papiEvents);
CkpvDeclare(int, numEvents);
#endif // CMK_HAS_COUNTER_PAPI

#if CMK_HAS_COUNTER_PAPI
void initPAPI() {
#if CMK_HAS_COUNTER_PAPI
  // We initialize and create the event sets for use with PAPI here.
  int papiRetValue;
  if(CkMyRank()==0){
      papiRetValue = PAPI_is_initialized();
      if(papiRetValue != PAPI_NOT_INITED)
          return;
    papiRetValue = PAPI_library_init(PAPI_VER_CURRENT);
    if (papiRetValue != PAPI_VER_CURRENT) {
      CmiAbort("PAPI Library initialization failure!\n");
    }
#if CMK_SMP
    if(PAPI_thread_init(pthread_self) != PAPI_OK){
      CmiAbort("PAPI could not be initialized in SMP mode!\n");
    }
#endif
  }
  CkpvInitialize(int, papiStarted);
  CkpvAccess(papiStarted) = 0;
  CkpvInitialize(int, papiStopped);
  CkpvAccess(papiStopped) = 0;

#if CMK_SMP
  //PAPI_thread_init has to finish before calling PAPI_create_eventset
  #if CMK_SMP_TRACE_COMMTHREAD
      CmiNodeAllBarrier();
  #else
      CmiNodeBarrier();
  #endif
#endif
  // PAPI 3 mandates the initialization of the set to PAPI_NULL
  CkpvInitialize(int, papiEventSet); 
  CkpvAccess(papiEventSet) = PAPI_NULL; 
  if (PAPI_create_eventset(&CkpvAccess(papiEventSet)) != PAPI_OK) {
    CmiAbort("PAPI failed to create event set!\n");
  }
  CkpvInitialize(int*, papiEvents);
  CkpvInitialize(int, numEvents);
#ifdef USE_SPP_PAPI
  CkpvAccess(numEvents) = NUMPAPIEVENTS;
  CkpvAccess(papiEvents) = new int[CkpvAccess(numEvents)];
  //  CmiPrintf("Using SPP counters for PAPI\n");
  if(PAPI_query_event(PAPI_FP_OPS)==PAPI_OK) {
    CkpvAccess(papiEvents)[0] = PAPI_FP_OPS;
  }else{
    if(CmiMyPe()==0){
      CmiAbort("WARNING: PAPI_FP_OPS doesn't exist on this platform!");
    }
  }
  if(PAPI_query_event(PAPI_TOT_INS)==PAPI_OK) {
    CkpvAccess(papiEvents)[1] = PAPI_TOT_INS;
  }else{
    CmiAbort("WARNING: PAPI_TOT_INS doesn't exist on this platform!");
  }
  int EventCode;
  int ret;
  ret=PAPI_event_name_to_code("perf::PERF_COUNT_HW_CACHE_LL:MISS",&EventCode);
  if(PAPI_query_event(EventCode)==PAPI_OK) {
    CkpvAccess(papiEvents)[2] = EventCode;
  }else{
    CmiAbort("WARNING: perf::PERF_COUNT_HW_CACHE_LL:MISS doesn't exist on this platform!");
  }
  ret=PAPI_event_name_to_code("DATA_PREFETCHER:ALL",&EventCode);
  if(PAPI_query_event(EventCode)==PAPI_OK) {
    CkpvAccess(papiEvents)[3] = EventCode;
  }else{
    CmiAbort("WARNING: DATA_PREFETCHER:ALL doesn't exist on this platform!");
  }
  if(PAPI_query_event(PAPI_L1_DCA)==PAPI_OK) {
    CkpvAccess(papiEvents)[4] = PAPI_L1_DCA;
  }else{
    CmiAbort("WARNING: PAPI_L1_DCA doesn't exist on this platform!");
  }
  if(PAPI_query_event(PAPI_TOT_CYC)==PAPI_OK) {
    CkpvAccess(papiEvents)[5] = PAPI_TOT_CYC;
  }else{
    CmiAbort("WARNING: PAPI_TOT_CYC doesn't exist on this platform!");
  }
#else
  CkpvAccess(numEvents) = NUMPAPIEVENTS;
  CkpvAccess(papiEvents) = new int[CkpvAccess(numEvents)];
  if (PAPI_query_event(PAPI_L1_TCM) == PAPI_OK && PAPI_query_event(PAPI_L1_TCA) == PAPI_OK) {
    CkpvAccess(papiEvents)[0] = PAPI_L1_TCM;
    CkpvAccess(papiEvents)[1] = PAPI_L1_TCA;
  } else if (PAPI_query_event(PAPI_L2_TCM) == PAPI_OK && PAPI_query_event(PAPI_L2_TCA) == PAPI_OK) {
    CkpvAccess(papiEvents)[0] = PAPI_L2_TCM;
    CkpvAccess(papiEvents)[1] = PAPI_L2_TCA;
  } else if (PAPI_query_event(PAPI_L3_TCM) == PAPI_OK && PAPI_query_event(PAPI_L3_TCA) == PAPI_OK) {
    CkpvAccess(papiEvents)[0] = PAPI_L3_TCM;
    CkpvAccess(papiEvents)[1] = PAPI_L3_TCA;
  } else {
    CmiAbort("PAPI: no cache miss/access events supported on any level!\n");
  }
#endif
  papiRetValue = PAPI_add_events(CkpvAccess(papiEventSet), CkpvAccess(papiEvents), CkpvAccess(numEvents));
  if (papiRetValue < 0) {
    if (papiRetValue == PAPI_ECNFLCT) {
      CmiAbort("PAPI events conflict! Please re-assign event types!\n");
    } else {
      char error_str[PAPI_MAX_STR_LEN];
      //PAPI_perror(error_str);
      //PAPI_perror(papiRetValue,error_str,PAPI_MAX_STR_LEN);
      CmiAbort("PAPI failed to add designated events!\n");
    }
  }
  if(CkMyPe()==0)
    {
      CmiPrintf("Registered %d PAPI counters:",CkpvAccess(numEvents));
      char nameBuf[PAPI_MAX_STR_LEN];
      for(int i=0;i<CkpvAccess(numEvents);i++)
	{
	  PAPI_event_code_to_name(CkpvAccess(papiEvents)[i], nameBuf);
	  CmiPrintf("%s ",nameBuf);
	}
      CmiPrintf("\n");
    }
  CkpvInitialize(LONG_LONG_PAPI*, papiValues);
  CkpvAccess(papiValues) = (LONG_LONG_PAPI*)malloc(CkpvAccess(numEvents)*sizeof(LONG_LONG_PAPI));
  memset(CkpvAccess(papiValues), 0, CkpvAccess(numEvents)*sizeof(LONG_LONG_PAPI));
#endif
}
#endif

void traceSend(void *env, int pe, int size)
{
#if CMK_TRACE_ENABLED
  if (CpvAccess(traceOn) && CkpvAccess(_traces))
      CkpvAccess(_traces)->messageSend(env, pe, size);
#endif
}

void traceRecv(void *env , int size)
{
#if CMK_TRACE_ENABLED
  if (CpvAccess(traceOn) && CkpvAccess(_traces))
      CkpvAccess(_traces)->messageRecv(env, size);
#endif
}

/*@}*/
