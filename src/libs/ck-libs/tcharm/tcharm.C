/*
Threaded Charm++ "Framework Framework"

Orion Sky Lawlor, olawlor@acm.org, 11/19/2001
 */
#include "tcharm_impl.h"
#include "tcharm.h"
#include <ctype.h>
#include "memory-isomalloc.h"

CtvDeclare(TCharm *,_curTCharm);

static int nChunks;

class TCharmTraceLibList {
	enum {maxLibs=20,maxLibNameLen=15};
	//List of libraries we want to trace:
	int curLibs;
	char libNames[maxLibs][maxLibNameLen];
	int checkIfTracing(const char *lib) const
	{
		for (int i=0;i<curLibs;i++) 
			if (0==strcmp(lib,libNames[i]))
				return 1;
		return 0;
	}
public:
	TCharmTraceLibList() {curLibs=0;}
	void addTracing(const char *lib) 
	{ //We want to trace this library-- add its name to the list.
		CkPrintf("TCHARM> Will trace calls to library %s\n",lib);
		int i;
		for (i=0;0!=*lib;i++,lib++)
			libNames[curLibs][i]=tolower(*lib);
		libNames[curLibs][i]=0;
		// if already tracing, skip
		if (checkIfTracing(libNames[curLibs])) return;
		curLibs++;
	}
	inline bool isTracing(const char *lib) const {
		if (curLibs==0) return 0; //Common case
		else return checkIfTracing(lib);
	}
};
static TCharmTraceLibList tcharm_tracelibs;
static bool tcharm_nomig=false;
bool tcharm_nothreads=false;
#ifndef TCHARM_STACKSIZE_DEFAULT
#define TCHARM_STACKSIZE_DEFAULT 1048576 /*Default stack size is 1MB*/
#endif
static int tcharm_stacksize=TCHARM_STACKSIZE_DEFAULT;
static bool tcharm_initted=false;
CkpvDeclare(bool, mapCreated);
static CkGroupID mapID;
static char* mapping = NULL;

#if CMK_TRACE_ENABLED
CsvDeclare(funcmap*, tcharm_funcmap);
#endif

// circumstances that need special handling so that node setup runs on pthread 0:
#define TCHARM_NODESETUP_COMMTHD (CMK_CONVERSE_MPI && CMK_SMP && !CMK_SMP_NO_COMMTHD)

void TCharm::nodeInit()
{
  static bool tcharm_nodeinit_has_been_called;
  if (tcharm_nodeinit_has_been_called)
    return;
  tcharm_nodeinit_has_been_called = true;

#if CMK_TRACE_ENABLED
  if (CsvAccess(tcharm_funcmap) == NULL) {
    CsvInitialize(funcmap*, tcharm_funcmap);
    CsvAccess(tcharm_funcmap) = new funcmap();
  }
#endif

  char **argv = CkGetArgv();
  nChunks = CkNumPes();
  CmiGetArgIntDesc(argv, "-vp", &nChunks, "Set the total number of virtual processors");
  CmiGetArgIntDesc(argv, "+vp", &nChunks, nullptr);

#if !TCHARM_NODESETUP_COMMTHD
  TCHARM_Node_Setup(nChunks);
#endif
}

void TCharm::procInit()
{
#if TCHARM_NODESETUP_COMMTHD
  if (CmiInCommThread())
    TCHARM_Node_Setup(nChunks);

  CmiNodeAllBarrier();
#endif

  CtvInitialize(TCharm *,_curTCharm);
  CtvAccess(_curTCharm)=NULL;
  tcharm_initted=true;
  CtgInit();

  CkpvInitialize(bool, mapCreated);
  CkpvAccess(mapCreated) = false;

  // called on every pe to eat these arguments
  char **argv=CkGetArgv();
  tcharm_nomig=CmiGetArgFlagDesc(argv,"+tcharm_nomig","Disable migration support (debugging)");
  tcharm_nothreads=CmiGetArgFlagDesc(argv,"+tcharm_nothread","Disable thread support (debugging)");
  tcharm_nothreads=(CmiGetArgFlagDesc(argv,"+tcharm_nothreads",NULL)) ? true : tcharm_nothreads;
  char *traceLibName=NULL;
  while (CmiGetArgStringDesc(argv,"+tcharm_trace",&traceLibName,"Print each call to this library"))
      tcharm_tracelibs.addTracing(traceLibName);
  // CmiGetArgIntDesc(argv,"+tcharm_stacksize",&tcharm_stacksize,"Set the thread stack size (default 1MB)");
  char *str;
  if (CmiGetArgStringDesc(argv,"+tcharm_stacksize",&str,"Set the thread stack size (default 1MB)"))  {
    if (strpbrk(str,"M")) {
      sscanf(str, "%dM", &tcharm_stacksize);
      tcharm_stacksize *= 1024*1024;
    }
    else if (strpbrk(str,"K")) {
      sscanf(str, "%dK", &tcharm_stacksize);
      tcharm_stacksize *= 1024;
    }
    else {
      sscanf(str, "%d", &tcharm_stacksize);
    }
    if (CkMyPe() == 0)
      CkPrintf("TCharm> stack size is set to %d.\n", tcharm_stacksize);
  }
  if (CkMyRank() != 0) { // rank 0 eats "+vp<N>" and "-vp<N>" in nodeInit
    int ignored;
    CmiGetArgIntDesc(argv, "-vp", &ignored, nullptr);
    CmiGetArgIntDesc(argv, "+vp", &ignored, nullptr);
  }
  if (CkMyPe()==0) { // Echo various debugging options:
    if (tcharm_nomig) CmiPrintf("TCHARM> Disabling migration support, for debugging\n");
    if (tcharm_nothreads) CmiPrintf("TCHARM> Disabling thread support, for debugging\n");
  }
  if (!CkpvAccess(mapCreated)) {
    if (0!=CmiGetArgString(argv, "+mapping", &mapping)){
    }
    CkpvAccess(mapCreated)=true;
  }
}

void TCHARM_Api_trace(const char *routineName,const char *libraryName) noexcept
{
	if (!tcharm_tracelibs.isTracing(libraryName)) return;
	TCharm *tc=CtvAccess(_curTCharm);
	char where[100];
	if (tc==NULL) sprintf(where,"[serial context on %d]",CkMyPe());
	else sprintf(where,"[%p> vp %d, p %d]",(void *)tc,tc->getElement(),CkMyPe());
	CmiPrintf("%s Called routine %s\n",where,routineName);
	CmiPrintStackTrace(1);
	CmiPrintf("\n");
}

// register thread start functions to get a function handler
// this is portable across heterogeneous platforms, or on machines with
// random stack/function pointer

static CkVec<TCHARM_Thread_data_start_fn> threadFnTable;

int TCHARM_Register_thread_function(TCHARM_Thread_data_start_fn fn)
{
  int idx = threadFnTable.size();
  threadFnTable.push_back(fn);
  return idx+1;                     // make 0 invalid number
}

TCHARM_Thread_data_start_fn getTCharmThreadFunction(int idx)
{
  CmiAssert(idx > 0);
  return threadFnTable[idx-1];
}

static void startTCharmThread(TCharmInitMsg *msg)
{
	DBGX("thread started");
	TCharm::activateThread();
       TCHARM_Thread_data_start_fn threadFn = getTCharmThreadFunction(msg->threadFn);
	threadFn(msg->data);
	TCharm::deactivateThread();
	CtvAccess(_curTCharm)->done(0);
}

TCharm::TCharm(TCharmInitMsg *initMsg_)
{
  initMsg=initMsg_;
  initMsg->opts.sanityCheck();
  timeOffset=0.0;
  if (tcharm_nothreads)
  { //Don't even make a new thread-- just use main thread
    tid=CthSelf();
  }
  else /*Create a thread normally*/
  {
    if (tcharm_nomig) { /*Nonmigratable version, for debugging*/
      tid=CthCreate((CthVoidFn)startTCharmThread,initMsg,initMsg->opts.stackSize);
      TCHARM_Element_Setup(thisIndex, initMsg->numElements, CmiIsomallocContext{});
    } else {
      // add one to numElements so that pieglobals can have some scratch space
      CmiIsomallocContext heapContext = CmiIsomallocContextCreate(thisIndex, initMsg->numElements+1);
      tid = CthCreateMigratable((CthVoidFn)startTCharmThread,initMsg,initMsg->opts.stackSize, heapContext);
      TCHARM_Element_Setup(thisIndex, initMsg->numElements, heapContext);
      if (heapContext.opaque != nullptr)
        CmiIsomallocContextEnableRandomAccess(heapContext);
    }
  }
  CtvAccessOther(tid,_curTCharm)=this;
  asyncMigrate = false;
  isStopped=true;
  exitWhenDone=initMsg->opts.exitWhenDone;
  isSelfDone = false;
  threadInfo.tProxy=CProxy_TCharm(thisArrayID);
  threadInfo.thisElement=thisIndex;
  threadInfo.numElements=initMsg->numElements;
  nUd=0;
  usesAtSync=true;
  run();
}

TCharm::TCharm(CkMigrateMessage *msg)
	:CBase_TCharm(msg)
{
  initMsg=NULL;
  tid=NULL;
  threadInfo.tProxy=CProxy_TCharm(thisArrayID);
}

void checkPupMismatch(PUP::er &p,int expected,const char *where)
{
	int v=expected;
	p|v;
	if (v!=expected) {
		CkError("FATAL ERROR> Mismatch %s pup routine\n",where);
		CkAbort("FATAL ERROR: Pup direction mismatch");
	}
}

void TCharm::pup(PUP::er &p) {
  checkPupMismatch(p,5134,"before TCHARM");
  p(isStopped); p(exitWhenDone); p(isSelfDone); p(asyncMigrate);
  p(threadInfo.thisElement);
  p(threadInfo.numElements);
  p | resumeAfterMigrationCallback;

  if (sema.size()>0){
  	CkAbort("TCharm::pup> Cannot migrate with unconsumed semaphores!\n");
  }

  DBG("Packing thread");
#if CMK_ERROR_CHECKING
  if (!p.isSizing() && !isStopped && !CmiMemoryIs(CMI_MEMORY_IS_ISOMALLOC)){
	CkAbort("Cannot pup a running thread.  You must suspend before migrating.\n");
  }	
  if (tcharm_nomig && !p.isSizing()) CkAbort("Cannot migrate with the +tcharm_nomig option!\n");
#endif

  //This seekBlock allows us to reorder the packing/unpacking--
  // This is needed because the userData depends on the thread's stack
  // and heap data both at pack and unpack time.
  PUP::seekBlock s(p,2);
  
  if (p.isUnpacking())
  {//In this case, unpack the thread & heap before the user data
    s.seek(1);
    pupThread(p);
    //Restart our clock: set it up so packTime==CkWallTimer+timeOffset
    double packTime;
    p(packTime);
    timeOffset=packTime-CkWallTimer();
  }
  
//Pack all user data
  // Set up TCHARM context for use during user's pup routines:
  if(isStopped) {
    CtvAccess(_curTCharm)=this;
    activateThread();
  }

  s.seek(0);
  checkPupMismatch(p,5135,"before TCHARM user data");
  p(nUd);
  for(int i=0;i<nUd;i++) {
    if (p.isUnpacking()) ud[i].update(tid);
    ud[i].pup(p);
  }
  checkPupMismatch(p,5137,"after TCHARM_Register user data");

  if(isStopped) {
    if (CmiMemoryIs(CMI_MEMORY_IS_ISOMALLOC))
      deactivateThread();
  }
  p|sud;           //  sud vector block can not be in isomalloc
  checkPupMismatch(p,5138,"after TCHARM_Global user data");
  
  // Tear down TCHARM context after calling user pup routines
  if(isStopped) {
    if (!CmiMemoryIs(CMI_MEMORY_IS_ISOMALLOC))
      deactivateThread();
    CtvAccess(_curTCharm)=NULL;
  }

  if (!p.isUnpacking())
  {//In this case, pack the thread & heap after the user data
    s.seek(1);
    pupThread(p);
    //Stop our clock:
    double packTime=CkWallTimer()+timeOffset;
    p(packTime);
  }
  
  s.endBlock(); //End of seeking block
  checkPupMismatch(p,5140,"after TCHARM");
}

// Pup our thread and related data
void TCharm::pupThread(PUP::er &pc) {
    pup_er p=(pup_er)&pc;
    checkPupMismatch(pc,5138,"before TCHARM thread"); 

    tid = CthPup(p, tid);
    if (pc.isUnpacking()) {
      CtvAccessOther(tid,_curTCharm)=this;
    }
    checkPupMismatch(pc,5139,"after TCHARM thread");
}

//Pup one group of user data
void TCharm::UserData::pup(PUP::er &p)
{
  pup_er pext=(pup_er)(&p);
  p(mode);
  switch(mode) {
  case 'c': { /* C mode: userdata is on the stack, so keep address */
//     p((char*)&data,sizeof(data));
     p(pos);
     //FIXME: function pointers may not be valid across processors
     p((char*)&cfn, sizeof(TCHARM_Pup_fn));
     char *data = CthPointer(t, pos);
     if (cfn) cfn(pext,data);
     } break;
  case 'g': { /* Global mode: zero out userdata on arrival */
     if (CmiMemoryIs(CMI_MEMORY_IS_ISOMALLOC))
     {
        // keep the pointer value if using isomalloc, no need to use pup
       p(pos);
     }
     else if (p.isUnpacking())      //  zero out userdata on arrival
       pos=0;

       //FIXME: function pointers may not be valid across processors
     p((char*)&gfn, sizeof(TCHARM_Pup_global_fn));
     if (gfn) gfn(pext);
     } break;
  default:
     break;
  };
}

TCharm::~TCharm()
{
  CthFree(tid);
  delete initMsg;
}

CMI_WARN_UNUSED_RESULT TCharm * TCharm::migrateTo(int destPE) noexcept {
	if (destPE==CkMyPe()) return this;
	if (CthMigratable() == 0) {
	    CkPrintf("Warning> thread migration is not supported!\n");
            return this;
        }
	asyncMigrate = true;
	// Make sure migrateMe gets called *after* we suspend:
	thisProxy[thisIndex].ckEmigrate(destPE);
	return suspend();
}
void TCharm::ckJustMigrated() {
	ArrayElement::ckJustMigrated();
    if (asyncMigrate) {
        asyncMigrate = false;
        resume();
    }
}

void TCharm::ckJustRestored() {
	ArrayElement::ckJustRestored();
}

// If a Tcharm object is about to migrate it should be suspended first
void TCharm::ckAboutToMigrate(){
	ArrayElement::ckAboutToMigrate();
	isStopped = true;
}

// clear the data before restarting from disk
void TCharm::clear()
{
  CthFree(tid);
  delete initMsg;
}

//Register user data to be packed with the thread
int TCharm::add(const TCharm::UserData &d) noexcept
{
  if (nUd>=maxUserData)
    CkAbort("TCharm: Registered too many user data fields!\n");

  // disable use of pup_buffer which conflicts with pup routines
  CthThread th = getThread();
  auto ctx = CmiIsomallocGetThreadContext(th);
  if (ctx.opaque != nullptr)
    CmiIsomallocEnableRDMA(ctx, 0);

  int nu=nUd++;
  ud[nu]=d;
  return nu;
}
void *TCharm::lookupUserData(int i) noexcept {
	if (i<0 || i>=nUd)
		CkAbort("Bad user data index passed to TCharmGetUserdata!\n");
	return ud[i].getData();
}

//Start the thread running
void TCharm::run() noexcept
{
  DBG("TCharm::run()");
  if (tcharm_nothreads) {/*Call user routine directly*/
	  startTCharmThread(initMsg);
  } 
  else /* start the thread as usual */
  	  start();
}

//Go to sync, block, possibly migrate, and then resume
CMI_WARN_UNUSED_RESULT TCharm * TCharm::migrate() noexcept
{
#if CMK_LBDB_ON
  DBG("going to sync");
  AtSync();
  return stop();
#else
  DBG("skipping sync, because there is no load balancer");
  return this;
#endif
}

//calls atsync with async mode
CMI_WARN_UNUSED_RESULT TCharm * TCharm::async_migrate() noexcept
{
#if CMK_LBDB_ON
  DBG("going to sync at async mode");
  ReadyMigrate(false);
  asyncMigrate = true;
  AtSync(0);
  return schedule();
#else
  DBG("skipping sync, because there is no load balancer");
  return this;
#endif
}

/*
Note:
 thread can only migrate at the point when this is called
*/
CMI_WARN_UNUSED_RESULT TCharm * TCharm::allow_migrate()
{
#if CMK_LBDB_ON
  int nextPe = MigrateToPe();
  if (nextPe != -1) {
    return migrateTo(nextPe);
  }
#else
  DBG("skipping sync, because there is no load balancer");
#endif
  return this;
}

//Resume from sync: start the thread again
void TCharm::ResumeFromSync()
{
  DBG("thread resuming from sync");

  CthThread th = getThread();
  auto ctx = CmiIsomallocGetThreadContext(th);
  CmiIsomallocContextJustMigrated(ctx);

  if (resumeAfterMigrationCallback.isInvalid())
    start();
  else
    resumeAfterMigrationCallback.send();
}


/****** TcharmClient ******/
void TCharmClient1D::ckJustMigrated() {
  ArrayElement1D::ckJustMigrated();
  findThread();
  tcharmClientInit();
}

void TCharmClient1D::pup(PUP::er &p) {
  ArrayElement1D::pup(p);
  p|threadProxy;
}

CkArrayID TCHARM_Get_threads() {
	TCHARMAPI("TCHARM_Get_threads");
	return TCharm::get()->getProxy();
}

/************* Startup/Shutdown Coordination Support ************/

//Called when we want to go to a barrier
CMI_WARN_UNUSED_RESULT TCharm * TCharm::barrier() noexcept {
	//Contribute to a synchronizing reduction
	contribute(CkCallback(CkReductionTarget(TCharm, atBarrier), thisProxy[0]));
	TCharm * dis = stop();
	return dis;
}

//Called when we've reached the barrier
void TCharm::atBarrier(){
	DBGX("clients all at barrier");
	thisProxy.start(); //Just restart everybody
}

//Called when the thread is done running
void TCharm::done(int exitcode) noexcept {
	//CmiPrintStackTrace(0);
	DBG("TCharm thread "<<thisIndex<<" done")
	if (exitWhenDone) {
		//Contribute to a synchronizing reduction
		CkReductionMsg *exitmsg = CkReductionMsg::buildNew(sizeof(int), &exitcode, CkReduction::max_int);
		CkCallback cb(CkIndex_TCharm::atExit(NULL), CkArrayIndex1D(0), thisProxy);
		exitmsg->setCallback(cb);
		contribute(exitmsg);
	}
	isSelfDone = true;
	TCharm * unused = stop();
}
//Called when all threads are done running
void TCharm::atExit(CkReductionMsg* msg) noexcept {
	DBGX("TCharm::atExit1> exiting");
	//thisProxy.unsetFlags();
	int exitcode = *(int*)msg->getData();

	// NOTE: We use an explicit message rather than a [reductiontarget] entry method
	//       here so that we can delete the msg explicitly *before* calling CkExit(),
	//       otherwise the underlying message is leaked (and valgrind reports it).
	delete msg;

	CkExit(exitcode);
	//CkPrintf("After CkExit()!!!!!!!\n");
}

/************* Setup **************/

//Globals used to control setup process
static TCHARM_Fallback_setup_fn g_fallbackSetup=NULL;
void TCHARM_Set_fallback_setup(TCHARM_Fallback_setup_fn f)
{
	g_fallbackSetup=f;
}
void TCHARM_Call_fallback_setup() {
	if (g_fallbackSetup) 
		(g_fallbackSetup)();
	else
		CkAbort("TCHARM: Unexpected fallback setup--missing TCHARM_User_setup routine?");
}

/************** User API ***************/
/**********************************
Callable from UserSetup:
*/

// Read the command line to figure out how many threads to create:
CLINKAGE int TCHARM_Get_num_chunks()
{
	TCHARMAPI("TCHARM_Get_num_chunks");
	if (CkMyPe()!=0) CkAbort("TCHARM_Get_num_chunks should only be called on PE 0 during setup!");
	return nChunks;
}
FLINKAGE int FTN_NAME(TCHARM_GET_NUM_CHUNKS,tcharm_get_num_chunks)()
{
	return TCHARM_Get_num_chunks();
}

// Fill out the default thread options:
TCHARM_Thread_options::TCHARM_Thread_options(int doDefault)
{
	stackSize=0; /* default stacksize */
	exitWhenDone=false; /* don't exit when done by default. */
}
void TCHARM_Thread_options::sanityCheck() {
	if (stackSize<=0) stackSize=tcharm_stacksize;
}


TCHARM_Thread_options g_tcharmOptions(1);

/*Set the size of the thread stack*/
CLINKAGE void TCHARM_Set_stack_size(int newStackSize)
{
	TCHARMAPI("TCHARM_Set_stack_size");
	g_tcharmOptions.stackSize=newStackSize;
}
FLINKAGE void FTN_NAME(TCHARM_SET_STACK_SIZE,tcharm_set_stack_size)
	(int *newSize)
{ TCHARM_Set_stack_size(*newSize); }

CLINKAGE void TCHARM_Set_exit() { g_tcharmOptions.exitWhenDone=true; }

/*Create a new array of threads, which will be bound to by subsequent libraries*/
CLINKAGE void TCHARM_Create(int nThreads,
			int threadFn)
{
	TCHARMAPI("TCHARM_Create");
	TCHARM_Create_data(nThreads,
			 threadFn,NULL,0);
}
FLINKAGE void FTN_NAME(TCHARM_CREATE,tcharm_create)
	(int *nThreads,int threadFn)
{ TCHARM_Create(*nThreads,threadFn); }

static CProxy_TCharm TCHARM_Build_threads(TCharmInitMsg *msg);

/*As above, but pass along (arbitrary) data to threads*/
CLINKAGE void TCHARM_Create_data(int nThreads,
		  int threadFn,
		  void *threadData,int threadDataLen)
{
	TCHARMAPI("TCHARM_Create_data");
	TCharmInitMsg *msg=new (threadDataLen,0) TCharmInitMsg(
		threadFn,g_tcharmOptions);
	msg->numElements=nThreads;
	memcpy(msg->data,threadData,threadDataLen);
	TCHARM_Build_threads(msg);
	
	// Reset the thread options:
	g_tcharmOptions=TCHARM_Thread_options(1);
}

FLINKAGE void FTN_NAME(TCHARM_CREATE_DATA,tcharm_create_data)
	(int *nThreads,
		  int threadFn,
		  void *threadData,int *threadDataLen)
{ TCHARM_Create_data(*nThreads,threadFn,threadData,*threadDataLen); }

static CProxy_TCharm TCHARM_Build_threads(TCharmInitMsg *msg)
{
  CkArrayOptions opts(msg->numElements);
  CkAssert(CkpvAccess(mapCreated)==true);

  if(haveConfigurableRRMap()){
    CkPrintf("TCharm> using ConfigurableRRMap\n");
    mapID=CProxy_ConfigurableRRMap::ckNew();
    opts.setMap(mapID);
  } else if(mapping==NULL){
    /* do nothing: use the default map */
  } else if(0 == strcmp(mapping,"BLOCK_MAP")) {
    CkPrintf("TCharm> using BLOCK_MAP\n");
    mapID = CProxy_BlockMap::ckNew();
    opts.setMap(mapID);
  } else if(0 == strcmp(mapping,"RR_MAP")) {
    CkPrintf("TCharm> using RR_MAP\n");
    mapID = CProxy_RRMap::ckNew();
    opts.setMap(mapID);
  } else if(0 == strcmp(mapping,"MAPFILE")) {
    CkPrintf("TCharm> reading map from mapfile\n");
    mapID = CProxy_Simple1DFileMap::ckNew();
    opts.setMap(mapID);
  } else if(0 == strcmp(mapping,"TOPO_MAPFILE")) {
    CkPrintf("TCharm> reading topo map from mapfile\n");
    mapID = CProxy_ReadFileMap::ckNew();
    opts.setMap(mapID);
  } else if(0 == strcmp(mapping,"PROP_MAP")) {
    CkPrintf("TCharm> using PROP_MAP\n");
    mapID = CProxy_PropMap::ckNew();
    opts.setMap(mapID);
  }
  opts.setStaticInsertion(true);
  opts.setAnytimeMigration(false);
  opts.setSectionAutoDelegate(false);
  return CProxy_TCharm::ckNew(msg,opts);
}

// Helper used when creating a new array bound to the TCHARM threads:
CkArrayOptions TCHARM_Attach_start(CkArrayID *retTCharmArray,int *retNumElts)
{
	TCharm *tc=TCharm::get();
	if (!tc)
		CkAbort("You must call TCHARM initialization routines from a TCHARM thread!");
	int nElts=tc->getNumElements();
      
        //CmiPrintf("TCHARM Elements = %d\n", nElts);  
      
	if (retNumElts!=NULL) *retNumElts=nElts;
	*retTCharmArray=tc->getProxy();
	CkArrayOptions opts(nElts);
	opts.bindTo(tc->getProxy());
	return opts;
}

void TCHARM_Suspend() {
	TCharm * unused = TCharm::get()->suspend();
}

/***********************************
Callable from worker thread
*/
CLINKAGE int TCHARM_Element()
{ 
	TCHARMAPI("TCHARM_Element");
	return TCharm::get()->getElement();
}
CLINKAGE int TCHARM_Num_elements()
{ 
	TCHARMAPI("TCHARM_Num_elements");
	return TCharm::get()->getNumElements();
}

FLINKAGE int FTN_NAME(TCHARM_ELEMENT,tcharm_element)()
{ return TCHARM_Element();}
FLINKAGE int FTN_NAME(TCHARM_NUM_ELEMENTS,tcharm_num_elements)()
{ return TCHARM_Num_elements();}

//Make sure this address will migrate with us when we move:
static void checkAddress(void *data)
{
	if (tcharm_nomig||tcharm_nothreads) return; //Stack is not isomalloc'd
	if (CmiThreadIs(CMI_THREAD_IS_ALIAS)||CmiThreadIs(CMI_THREAD_IS_STACKCOPY)) return; // memory alias thread
	if (CmiIsomallocEnabled()) {
          if (!CmiIsomallocInRange(data))
	    CkAbort("The UserData you register must be allocated on the stack!\n");
        }
        else {
	  if(CkMyPe() == 0)
	    CkPrintf("Warning> checkAddress failed because isomalloc not supported.\n");
	}
}

/* Old "register"-based userdata: */
CLINKAGE int TCHARM_Register(void *data,TCHARM_Pup_fn pfn)
{ 
	TCHARMAPI("TCHARM_Register");
	checkAddress(data);
	return TCharm::get()->add(TCharm::UserData(pfn,TCharm::get()->getThread(),data));
}
FLINKAGE int FTN_NAME(TCHARM_REGISTER,tcharm_register)
	(void *data,TCHARM_Pup_fn pfn)
{ 
	TCHARMAPI("TCHARM_Register");
	checkAddress(data);
	return TCharm::get()->add(TCharm::UserData(pfn,TCharm::get()->getThread(),data));
}

CLINKAGE void *TCHARM_Get_userdata(int id)
{
	TCHARMAPI("TCHARM_Get_userdata");
	return TCharm::get()->lookupUserData(id);
}
FLINKAGE void *FTN_NAME(TCHARM_GET_USERDATA,tcharm_get_userdata)(int *id)
{ return TCHARM_Get_userdata(*id); }

/* New hardcoded-ID userdata: */
CLINKAGE void TCHARM_Set_global(int globalID,void *new_value,TCHARM_Pup_global_fn pup_or_NULL)
{
	TCHARMAPI("TCHARM_Set_global");
	TCharm *tc=TCharm::get();
	if (tc->sud.length()<=globalID)
	{ //We don't have room for this ID yet: make room
		int newLen=2*globalID;
		tc->sud.resize(newLen);
	}
	tc->sud[globalID]=TCharm::UserData(pup_or_NULL,tc->getThread(),new_value);
}
CLINKAGE void *TCHARM_Get_global(int globalID)
{
	//Skip TCHARMAPI("TCHARM_Get_global") because there's no dynamic allocation here,
	// and this routine should be as fast as possible.
	CkVec<TCharm::UserData> &v=TCharm::get()->sud;
	if (v.length()<=globalID) return NULL; //Uninitialized global
	return v[globalID].getData();
}

CLINKAGE void TCHARM_Migrate()
{
	TCHARMAPI("TCHARM_Migrate");
	if (CthMigratable() == 0) {
	  if(CkMyPe() == 0)
	    CkPrintf("Warning> thread migration is not supported!\n");
          return;
        }
	TCharm * unused = TCharm::get()->migrate();
}
FORTRAN_AS_C(TCHARM_MIGRATE,TCHARM_Migrate,tcharm_migrate,(void),())

CLINKAGE void TCHARM_Async_Migrate()
{
	TCHARMAPI("TCHARM_Async_Migrate");
	TCharm * unused = TCharm::get()->async_migrate();
}
FORTRAN_AS_C(TCHARM_ASYNC_MIGRATE,TCHARM_Async_Migrate,tcharm_async_migrate,(void),())

CLINKAGE void TCHARM_Allow_Migrate()
{
	TCHARMAPI("TCHARM_Allow_Migrate");
	TCharm * unused = TCharm::get()->allow_migrate();
}
FORTRAN_AS_C(TCHARM_ALLOW_MIGRATE,TCHARM_Allow_Migrate,tcharm_allow_migrate,(void),())

CLINKAGE void TCHARM_Migrate_to(int destPE)
{
	TCHARMAPI("TCHARM_Migrate_to");
	TCharm * unused = TCharm::get()->migrateTo(destPE);
}

FORTRAN_AS_C(TCHARM_MIGRATE_TO,TCHARM_Migrate_to,tcharm_migrate_to,
	(int *destPE),(*destPE))

CLINKAGE void TCHARM_Yield()
{
	TCHARMAPI("TCHARM_Yield");
	TCharm * unused = TCharm::get()->schedule();
}
FORTRAN_AS_C(TCHARM_YIELD,TCHARM_Yield,tcharm_yield,(void),())

CLINKAGE void TCHARM_Barrier()
{
	TCHARMAPI("TCHARM_Barrier");
	TCharm * unused = TCharm::get()->barrier();
}
FORTRAN_AS_C(TCHARM_BARRIER,TCHARM_Barrier,tcharm_barrier,(void),())

CLINKAGE void TCHARM_Done(int exitcode)
{
	TCHARMAPI("TCHARM_Done");
	TCharm *c=TCharm::getNULL();
	if (!c) CkExit(exitcode);
	else c->done(exitcode);
}
FORTRAN_AS_C(TCHARM_DONE,TCHARM_Done,tcharm_done,(int *exitcode),(*exitcode))


CLINKAGE double TCHARM_Wall_timer()
{
  TCHARMAPI("TCHARM_Wall_timer");
  TCharm *c=TCharm::getNULL();
  if(!c) return CkWallTimer();
  else { //Have to apply current thread's time offset
    return CkWallTimer()+c->getTimeOffset();
  }
}

#if 1
/*Include Fortran-style "iargc" and "getarg" routines.
These are needed to get access to the command-line arguments from Fortran.
*/
FLINKAGE int FTN_NAME(TCHARM_IARGC,tcharm_iargc)() {
  TCHARMAPI("tcharm_iargc");
  return CkGetArgc()-1;
}

FLINKAGE void FTN_NAME(TCHARM_GETARG,tcharm_getarg)
	(int *i_p,char *dest,int destLen)
{
  TCHARMAPI("tcharm_getarg");
  int i=*i_p;
  if (i<0) CkAbort("tcharm_getarg called with negative argument!");
  if (i>=CkGetArgc()) CkAbort("tcharm_getarg called with argument > iargc!");
  const char *src=CkGetArgv()[i];
  strcpy(dest,src);
  for (i=strlen(dest);i<destLen;i++) dest[i]=' ';
}

#endif

//These silly routines are used for serial startup:
extern void _initCharm(int argc, char **argv);
CLINKAGE void TCHARM_Init(int *argc,char ***argv) {
	if (!tcharm_initted) {
	  ConverseInit(*argc, *argv, (CmiStartFn) _initCharm,1,1);
	  _initCharm(*argc,*argv);
	}
}

FLINKAGE void FTN_NAME(TCHARM_INIT,tcharm_init)()
{
	int argc=1;
	const char *argv_sto[2]={"foo",NULL};
	char **argv=(char **)argv_sto;
	TCHARM_Init(&argc,&argv);
}

/***********************************
* TCHARM Semaphores:
* The idea is one side "puts", the other side "gets"; 
* but the calls can come in any order--
* if the "get" comes first, it blocks until the put.
* This makes a convenient, race-condition-free way to do
* onetime initializations.  
*/
/// Find this semaphore, or insert if there isn't one:
TCharm::TCharmSemaphore *TCharm::findSema(int id) {
	for (unsigned int s=0;s<sema.size();s++)
		if (sema[s].id==id) 
			return &sema[s];
	sema.push_back(TCharmSemaphore(id));
	return &sema[sema.size()-1];
}
/// Remove this semaphore from the list
void TCharm::freeSema(TCharmSemaphore *doomed) {
	int id=doomed->id;
	for (unsigned int s=0;s<sema.size();s++)
		if (sema[s].id==id) {
			sema[s]=sema[sema.length()-1];
			sema.length()--;
			return;
		}
	CkAbort("Tried to free nonexistent TCharm semaphore");
}

/// Block until this semaphore has data:
TCharm::TCharmSemaphore *TCharm::getSema(int id) {
	TCharmSemaphore *s=findSema(id);
	if (s->data==NULL) 
	{ //Semaphore isn't filled yet: wait until it is
		s->thread=CthSelf();
		TCharm * dis = suspend(); //Will be woken by semaPut
		// Semaphore may have moved-- find it again
		s = dis->findSema(id);
		if (s->data==NULL) CkAbort("TCharm::semaGet awoken too early!");
	}
	return s;
}

/// Store data at the semaphore "id".
///  The put can come before or after the get.
void TCharm::semaPut(int id,void *data) {
	TCharmSemaphore *s=findSema(id);
	if (s->data!=NULL) CkAbort("Duplicate calls to TCharm::semaPut!");
	s->data=data;
	DBG("semaPut "<<id<<" "<<data);
	if (s->thread!=NULL) {//Awaken the thread
		s->thread=NULL;
		resume();
	}
}

/// Retreive data from the semaphore "id".
///  Blocks if the data is not immediately available.
///  Consumes the data, so another put will be required for the next get.
void *TCharm::semaGet(int id) {
	TCharmSemaphore *s=getSema(id);
	void *ret=s->data;
	DBG("semaGet "<<id<<" "<<ret);
	// Now remove the semaphore from the list:
	freeSema(s);
	return ret;
}

/// Retreive data from the semaphore "id".
///  Blocks if the data is not immediately available.
void *TCharm::semaGets(int id) {
	TCharmSemaphore *s=getSema(id);
	return s->data;
}

/// Retreive data from the semaphore "id", or returns NULL.
void *TCharm::semaPeek(int id) {
	TCharmSemaphore *s=findSema(id);
	return s->data;
}

/****** System Call support ******/
/*
TCHARM_System exists to work around a bug where Linux ia32
glibc2.2.x with pthreads crashes at the fork() site when 
called from a user-levelthread. 

The fix is to call system from the main thread, by 
passing the request out of the thread to our array element 
before calling system().
*/

CLINKAGE int
TCHARM_System(const char *shell_command)
{
	return TCharm::get()->system(shell_command);
}
int TCharm::system(const char *cmd)
{
	int ret=-1778;
	callSystemStruct s;
	s.cmd=cmd;
	s.ret=&ret;
	thisProxy[thisIndex].callSystem(s);
	TCharm * unused = suspend();
	return ret;
}

void TCharm::callSystem(const callSystemStruct &s)
{
	*s.ret = ::system(s.cmd);
	resume();
}



#include "tcharm.def.h"
