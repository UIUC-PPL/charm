#include <stdlib.h>
#include "charm++.h"
#include "trace-Tau.h"
#include "trace-TauBOC.h"
#include "trace-common.h"
#include "TAU.h"
//#include "tau_selective.cpp"
#include "map"
#include "stack"
#include <string>
using namespace std;

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

/*#ifndef PROFILING_ON
void TAU_PROFILER_CREATE(void *p, char *n, char *s, taugroup_t t) {
dprintf("---> tau
create profiler: %s \n", s); }

void TAU_PROFILER_STOP(void *p) { dprintf("---> tau
stop profiler"); }

void TAU_PROFILER_START(void *p) { dprintf("---> tau
start profiler"); }

void TAU_PROFILE_SET_NODE(int i) { dprintf("---> tau
set node"); }
#endif
*/

#ifdef DEBUG_PROF
#define dprintf printf
#else // DEBUG_PROF 
#define dprintf if (0) printf
#endif

extern bool processFileForInstrumentation(const string& file_name);
extern void printExcludeList();
extern bool instrumentEntity(const string& function_name);
extern int processInstrumentationRequests(char *fname);

// Charm++ "processor"(user thread)-private global variable
CkpvStaticDeclare(TraceTau*, _trace);

// This global variable is required for any post-execution 
// parallel analysis or activities the trace module might wish to perform.
CkGroupID traceTauGID;

/**
   For each TraceFoo module, _createTraceFoo() must be defined.
   This function is called in _createTraces() generated in moduleInit.C
*/

void *idle, *comp;
//char *name = "default";
bool profile = true, snapshotProfiling = false;

//map<const int, void*> events;
void* events[5000];
stack<void*> eventStack;
int EX_VALUE = 0;
void *EXCLUDED = &EX_VALUE;
void startEntryEvent(int id)
{
  dprintf("---------> starting Entry Event with id: %d\n", id);

  if ((id == -1) || (events[id] == NULL))
    {
      dprintf("-------> create event with id: %d\n", id);
      //sprintf(name, "Event %d", id);
      if (id == -1)
	{ /*
	    char *name = "dummy_thread_ep";
	    dprintf(" ------> creating event: %s\n", name);
	    TAU_PROFILER_CREATE(events[id], name, "", TAU_DEFAULT);
	    dprintf("timer created.\n");
	    eventStack.push(events[id]);
	    dprintf(" ------> starting event: %s\n", (char*) name);
	    TAU_PROFILER_START(eventStack.top());*/
	  //exclude dummy event
	  dprintf("------> excluding dummy function");
	  eventStack.push(EXCLUDED);
	}
      else
	{
	  //string check("doFFT(RSFFTMsg* impl_msg)");
	  //string name_s(_entryTable[id]->name);
	  //printf("checking name4: %s", _entryTable[id]->name);
	  //if (check.compare(name_s) != 0)
	  //{
	  char name [500];
	  sprintf(name, "%s::%s::%d", _chareTable[_entryTable[id]->chareIdx]->name,
		  _entryTable[id]->name, id);
	  //should this fuction be excluded from instrumentation?
	  if (!instrumentEntity(name))
	    {
	      //exclude function.
	      dprintf("------> excluding function %s\n", name);
	      events[id] = EXCLUDED;
	      eventStack.push(events[id]);
	    }
	  else
	    {
	      dprintf(" ------> creating event: %s\n", name);
	      TAU_PROFILER_CREATE(events[id], name, "", TAU_DEFAULT);
	      dprintf("timer created.\n");
	      eventStack.push(events[id]);
	      dprintf("starting event\n");
	      dprintf(" ------> starting event: %s\n", (char*) name);
	      TAU_PROFILER_START(eventStack.top());
	    }
	  dprintf("done.\n");
	}
    }
  else
    {
      eventStack.push(events[id]);
      if (events[id] != EXCLUDED)
	{
	  TAU_PROFILER_START(eventStack.top());
	}
    }
}

void stopEntryEvent()
{
  dprintf("stop timer...\n");
  if (eventStack.top() != EXCLUDED)
	{
	  TAU_PROFILER_STOP(eventStack.top());
	}
  eventStack.pop();
}


void _createTraceTau(char **argv)
{
  //TAU_INIT(1, argv);
  memset(events, 0, sizeof(void *)*5000);
  //CkPrintf("NEWEST VERSION");
  dprintf("arguments:\n");
  dprintf("[0] = %s, ", argv[0]);
  dprintf("[1] = %s, ", argv[1]);
  dprintf("[2] = %s, ", argv[2]);
  dprintf("\n");
  string disable = "disable-profiling";
  if (argv[1] == NULL) { profile = true; }
  else if (argv[1] == disable) { profile = false; }
  if (not CkpvAccess(traceOn)) { 
    dprintf("traceoff selected using snapshot profiling.\n");
    snapshotProfiling = true; 
  }

  CkpvInitialize(TraceTau*, _trace);
  CkpvAccess(_trace) = new TraceTau(argv);
  CkpvAccess(_traces)->addTrace(CkpvAccess(_trace));
}

TraceTau::TraceTau(char **argv)
{
  if (CkpvAccess(traceOnPe) == 0) return;
  
  // Process runtime arguments intended for the module
  CmiGetArgIntDesc(argv,"+TauPar0", &par0, "Fake integer parameter 0");
  CmiGetArgDoubleDesc(argv,"+TauPar1", &par1, "Fake double parameter 1");
  //TAU_REGISTER_THREAD();
  if (profile)
    {
      if (strcmp(CkpvAccess(selective), ""))
	{
	  //printf("select file: %s\n", CkpvAccess(selective));
	  //processFileForInstrumentation(CkpvAccess(selective));
	  processInstrumentationRequests(CkpvAccess(selective));
	  printExcludeList();
	  if (!instrumentEntity("Main::done(void)::99"))
	    {
	      dprintf("selective file working...\n");
	    }
	  else
	    dprintf("selective flile not working...\n");
	}
      
      TAU_PROFILER_CREATE(idle, "Idle", "", TAU_DEFAULT);
      //TAU_PROFILER_CREATE(entry,name,"", TAU_DEFAULT);
      dprintf("before %p\n", comp);
      TAU_PROFILER_CREATE(comp, "Main", "", TAU_DEFAULT);
      dprintf("after %p\n", comp);
      
      //Need to add an entry timer to the top of the stack because
      //traceTauExitFunction calls CkExit() which calls endExecute
      eventStack.push(EXCLUDED);
    }
  else 
    {
      dprintf("--> [TAU] creating timers...\n");
    }
}

void TraceTau::userEvent(int eventID) 
{
  dprintf("[%d] User Point Event id %d encountered\n", CkMyPe(), eventID);
}

void TraceTau::userBracketEvent(int eventID, double bt, double et) {
  dprintf("[%d] User Bracket Event id %d encountered\n", CkMyPe(), eventID);
}

void TraceTau::creation(envelope *, int epIdx, int num) {
  dprintf("[%d] Point-to-Point Message for Entry Method id %d sent\n",
	  CkMyPe(), epIdx);
}

void TraceTau::creationMulticast(envelope *, int epIdx, int num, 
				 const int *pelist) {
  dprintf("[%d] Multicast Message for Entry Method id %d sent to %d pes\n",
	  CkMyPe(), epIdx, num);
}

void TraceTau::creationDone(int num) {
  dprintf("[%d] Last initiated send completes\n", CkMyPe());
}

void TraceTau::messageRecv(char *env, int pe) {
  dprintf("[%d] Message from pe %d received by scheduler\n", 
	  CkMyPe(), pe);
}

void TraceTau::beginExecute(CmiObjId *tid)
{
  // CmiObjId is a 4-integer tuple uniquely identifying a migratable
  // Charm++ object. Note that there are other non-migratable Charm++
  // objects that CmiObjId will not identify.
  dprintf("[%d] Entry Method invoked using object id\n", CkMyPe());
  if (profile) {
    startEntryEvent(-1);
  }
  else
    {
      dprintf("--> [TAU] starting entry timer...\n");
    }
}

void TraceTau::beginExecute(envelope *e)
{
  // no message means thread execution
  if (e == NULL) {
    dprintf("[%d] Entry Method invoked via thread id %d\n", CkMyPe(),
	    CtvAccess(CthEP));
    if (profile) {
      startEntryEvent(-1);
    }
    else
      {
	dprintf("--> [TAU] starting entry timer...\n");
      }
    // Below is what is found in trace-summary.
    // beginExecute(-1,-1,_threadEP,-1);
  } else {
    dprintf("[%d] Entry Method %d invoked via message envelope\n", 
	    CkMyPe(), e->getEpIdx());
    if (profile) {
      startEntryEvent(e->getEpIdx());
    }
    else
      {
	dprintf("--> [TAU] starting entry timer...\n");
      }
    // Below is what is found in trace-summary.
    // beginExecute(-1,-1,e->getEpIdx(),-1);
  }
}

void TraceTau::beginExecute(int event,int msgType,int ep,int srcPe, 
			    int mlen, CmiObjId *idx)
{
  dprintf("[%d] Entry Method %d invoked by parameters\n", CkMyPe(),
	  ep);
  if (profile) {
    startEntryEvent(ep);
  }
  else
    {
      dprintf("--> [TAU] starting entry timer...\n");
    }
}

void TraceTau::endExecute(void)
{
  if (profile) {
    stopEntryEvent();
  }
  else
    {
      dprintf("--> [TAU] stoping entry timer...\n");
    }
  dprintf("[%d] Previously executing Entry Method completes\n", CkMyPe());
}

void TraceTau::beginIdle(double curWallTime) {
  dprintf("[%d] Scheduler has no useful user-work\n", CkMyPe());
  if (profile) {
    TAU_PROFILER_START(idle);
  }
  else
    {
      dprintf("--> [TAU] starting idle timer...\n");
    }
}

void TraceTau::endIdle(double curWallTime) {
  if (profile) {
    TAU_PROFILER_STOP(idle);
  }
  else
    {
      dprintf("--> [TAU] stopping idle timer...\n");
    }
  dprintf("[%d] Scheduler now has useful user-work\n", CkMyPe());
}

void TraceTau::beginComputation(void)
{
  dprintf("[%d] Computation Begins\n", CkMyPe());
  //TAU_DISABLE_ALL_GROUPS();
  // Code Below shows what trace-summary would do.
  // initialze arrays because now the number of entries is known.
  // _logPool->initMem();
}

void TraceTau::endComputation(void)
{
  dprintf("[%d] Computation Ends\n", CkMyPe());
}

void TraceTau::traceBegin(void)
{
  dprintf("[%d] >>>>>> Tracing Begins\n", CkMyPe());
  if (profile) {
    dprintf("ptr: %p\n", comp);
      TAU_DB_PURGE();
      TAU_ENABLE_ALL_GROUPS();
      TAU_PROFILER_START(comp);
  }
  else
    {
      dprintf("--> [TAU] starting computation timer...\n");
    }
}

void TraceTau::traceEnd(void)
{
  dprintf("[%d] >>>>>> Tracing Ends\n", CkMyPe());
  if (profile){
    dprintf("ptr: %p\n", comp);
      //TAU_PROFILER_STOP(comp);
    TAU_PROFILE_EXIT("tracing complete.");
    TAU_DISABLE_ALL_GROUPS();
  }
  else
    {
      dprintf("--> [TAU] stopping computation timer and writing profiles\n");
    }
  dprintf("[%d] Computation Ends\n", CkMyPe());
}

void TraceTau::malloc(void *where, int size, void **stack, int stackSize)
{
  dprintf("[%d] Memory allocation of size %d occurred\n", CkMyPe(), size);
}

void TraceTau::free(void *where, int size) {
  dprintf("[%d] %d-byte Memory block freed\n", CkMyPe(), size);
}

void TraceTau::traceClose(void)
{
  dprintf("traceClose called.\n");
  CkpvAccess(_trace)->endComputation();
  CkpvAccess(_trace)->traceEnd();
  //TAU_PROFILE_EXIT("closing trace...");
  //dprintf(" [%d] Exit called \n", CkMyPe());
  //TAU_PROFILE_EXIT("exiting...");
  // remove myself from traceArray so that no tracing will be called.
  CkpvAccess(_traces)->removeTrace(this);
}

extern "C" void traceTauExitFunction() {
  dprintf("traceTauExitFunction called.\n");
  //TAU_PROFILE_EXIT("exiting...");
  //TAU_PROFILE_EXIT("done");
  //eventStack.push(NULL);
  CkContinueExit();
}

// Initialization of the parallel trace module.
void initTraceTauBOC() {
  //void *main;
  dprintf("tracetauboc setting node %d\n", CmiMyPe());
  if (profile) {
    TAU_PROFILE_SET_NODE(CmiMyPe());
  }
  else
    {
      dprintf("---> [TAU] settting node.\n");
    }
  //TAU_PROFILER_CREATE(main, "main", "", TAU_DEFAULT);
  //TAU_PROFILER_START(main);
  if (CkMyRank() == 0) {
    registerExitFn(traceTauExitFunction);
  }
}
  
#include "TraceTau.def.h"
