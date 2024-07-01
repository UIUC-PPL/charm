/*****************************************************************************
 * A few useful built-in CCS handlers.
 *****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <limits.h>

#include "converse.h"
#include "ckhashtable.h"
#include "pup.h"
#include "pup_toNetwork.h"
#include "debug-conv++.h"
#include "conv-ccs.h"
#include "sockRoutines.h"
#include "queueing.h"
#include "ccs-builtins.h"

#ifdef __MINGW_H
#include "process.h"
#endif

#if CMK_CCS_AVAILABLE

void ccs_getinfo(char *msg);

/**********************************************
  "ccs_killport"-- takes one 4-byte big-endian port number
    Register a "client kill port".  When this program exits,
    it will connect to this TCP port and write "die\n\0" it.
*/

typedef struct killPortStruct{
  skt_ip_t ip;
  unsigned int port;
  struct killPortStruct *next;
} killPortStruct;
/*Only 1 kill list per node-- no Cpv needed*/
static killPortStruct *killList=NULL;

static void ccs_killport(char *msg)
{
  killPortStruct *oldList=killList;
  int port=ChMessageInt(*(ChMessageInt_t *)(msg+CmiReservedHeaderSize));
  skt_ip_t ip;
  unsigned int connPort;
  CcsCallerId(&ip,&connPort);
  killList=(killPortStruct *)malloc(sizeof(killPortStruct));
  killList->ip=ip;
  killList->port=port;
  killList->next=oldList;
  CmiFree(msg);
}
/*Send any registered clients kill messages before we exit*/
static int noMoreErrors(SOCKET skt, int c, const char *m) {return -1;}
void CcsImpl_kill(void)
{
  skt_set_abort(noMoreErrors);
  while (killList!=NULL)
  {
    SOCKET fd=skt_connect(killList->ip,killList->port,20);
    if (fd!=INVALID_SOCKET) {
      skt_sendN(fd,"die\n",strlen("die\n")+1);
      skt_close(fd);
    }
    killList=killList->next;
  }
}

/**********************************************
  "ccs_killpe"-- kills the executing processor
    Used for fault-tolerance testing: terminate the processor.
*/

#include <signal.h>

static void ccs_killpe(char *msg) {
#if CMK_HAS_KILL
  kill(getpid(), 9);
#else
  CmiAbort("ccs_killpe() not supported!");
#endif
}

/*************************************************
List interface:
   This lets different parts of a Charm++ program register 
"list retrieval functions", which expose some aspect of a
running program.  Example lists include: all readonly globals, 
messages waiting in a queue, or all extant array elements.

  "ccs_list_len" (4-byte character count, null-terminated ASCII string)
Return the number of items currently in the list at the given path.

  "ccs_list_items.bin" (4-byte start, 4-byte end+1,4-byte extra
data count n, n-byte extra data, 4-byte character count c, c-byte
ASCII request string [without NULL terminator])
Return the given items (from start to end) of the given queue, 
formatted as raw network binary data.

  "ccs_list_items.fmt" [parameters as for .bin]
Return the given items, formatted as tagged network binary data.

  "ccs_list_items.txt" [parameters as for .bin]
Return the given items, formatted as ASCII text.
*/

static void CpdListBoundsCheck(CpdListAccessor *l,int &lo,int &hi)
{
  if (l->checkBoundary()) {
    int len=l->getLength();
    if (lo<0) lo=0;
    if (hi>len) hi=len;
  }
}

typedef CkHashtableTslow<const char *,CpdListAccessor *> CpdListTable_t;
CpvStaticDeclare(CpdListTable_t *,cpdListTable);

/**
  Return the list at this (null-terminated ASCII) path.
*/
static CpdListAccessor *CpdListLookup(const char *path)
{
  CpdListAccessor *acc=CpvAccess(cpdListTable)->get(path);
  if (acc==NULL) {
    CmiError("CpdListAccessor> Unrecognized list path '%s'\n",path);
    return NULL;
  }
  return acc;
}

/**
  Return a CpdListAccessor, given a network string containing the 
  list path.  A network string is a big-endian 32-bit "length" 
  field, followed by a null-terminated ASCII string of that length.
*/
static CpdListAccessor *CpdListLookup(const ChMessageInt_t *lenAndPath)
{
  static const int CpdListMaxLen=80;
  int len=ChMessageInt(lenAndPath[0]);
  const char *path=(const char *)(lenAndPath+1);
  char pathBuf[CpdListMaxLen+1]; //Temporary null-termination buffer
  if ((len<0) || (len>CpdListMaxLen)) {
    CmiError("CpdListAccessor> Invalid list path length %d!\n",len);
    return NULL; //Character count is invalid
  }
  strncpy(pathBuf,path,len);
  pathBuf[len]=0; //Ensure string is null-terminated
  return CpdListLookup(pathBuf);
}

//CCS External access routines:

//Get the length of the given list:
static void CpdList_ccs_list_len(char *msg)
{
  const ChMessageInt_t *req=(const ChMessageInt_t *)(msg+CmiReservedHeaderSize);
  CpdListAccessor *acc=CpdListLookup(req);
  if (acc!=NULL) {
    ChMessageInt_t reply=ChMessageInt_new(acc->getLength());
    CcsSendReply(sizeof(reply),(void *)&reply);
  }
  CmiFree(msg);
}

//Read a list contents request header:
//  first item to send, 4-byte network integer
//  last item+1 to send, 4-byte network integer
//  extra data length, 4-byte network integer
//  extra data, list-defined bytes
//  list path length, 4-byte network integer (character count)
//  list path name, null-terminated ASCII
static CpdListAccessor *CpdListHeader_ccs_list_items(char *msg,
	     CpdListItemsRequest &h)
{
  int msgLen=CmiSize((void *)msg)-CmiReservedHeaderSize;
  CpdListAccessor *ret=NULL;
  const ChMessageInt_t *req=(const ChMessageInt_t *)(msg+CmiReservedHeaderSize);
  h.lo=ChMessageInt(req[0]); // first item to send
  h.hi=ChMessageInt(req[1]); // last item to send+1
  h.extraLen=ChMessageInt(req[2]); // extra data length
  if (h.extraLen>=0 
  && ((int)(3*sizeof(ChMessageInt_t)+h.extraLen))<msgLen) {
    h.extra=(void *)(req+3);  // extra data
    ret=CpdListLookup((ChMessageInt_t *)(h.extraLen+(char *)h.extra));
    if (ret!=NULL) CpdListBoundsCheck(ret,h.lo,h.hi);
  }
  return ret;
}


// Pup this cpd list's items under this request.
static void pupCpd(PUP::er &p, CpdListAccessor *acc, CpdListItemsRequest &req)
{
      p.syncComment(PUP::sync_begin_array,"CpdList");
      acc->pup(p,req);
      p.syncComment(PUP::sync_end_array);
}

static void CpdList_ccs_list_items_txt(char *msg)
{
  CpdListItemsRequest req;
  CpdListAccessor *acc=CpdListHeader_ccs_list_items(msg,req);
  if(acc == NULL) CmiPrintf("ccs-builtins> Null Accessor--bad list name (txt)\n");
  if (acc!=NULL) {
    int bufLen;
    { 
      PUP::sizerText p; pupCpd(p,acc,req); bufLen=p.size(); 
    }
    char *buf=new char[bufLen];
    { 
      PUP::toText p(buf, bufLen); pupCpd(p,acc,req);
      if (p.size()!=bufLen)
	CmiError("ERROR! Sizing/packing length mismatch for %s list pup function!\n",
		acc->getPath());
    }
    CcsSendReply(bufLen,(void *)buf);
    delete[] buf;
  }
  CmiFree(msg);
}

static void CpdList_ccs_list_items_set(char *msg)
{
  CpdListItemsRequest req;
  CpdListAccessor *acc=CpdListHeader_ccs_list_items(msg,req);
  if(acc == NULL) CmiPrintf("ccs-builtins> Null Accessor--bad list name (set)\n");
  else {
    PUP_toNetwork_unpack p(req.extra);
    pupCpd(p,acc,req);
    if (p.size()!=req.extraLen)
	CmiPrintf("Size mismatch during ccs_list_items.set: client sent %d bytes, but %zu bytes used!\n",
		req.extraLen,p.size());
  }
  CmiFree(msg);
}

/** gather information about the machine we're currently running on */
void CpdMachineArchitecture(char *msg) {
  char reply[8]; // where we store our reply
  reply[0]=CHARMDEBUG_MAJOR;
  reply[1]=CHARMDEBUG_MINOR;
  // decide if we are 32 bit (1) or 64 bit (2)
  reply[2] = 0;
  if (sizeof(char*) == 4) reply[2] = 1;
  else if (sizeof(char*) == 8) reply[2] = 2;
  // decide if we are little endian (1) or big endian (2)
  reply[3] = 0;
  int value = 1;
  char firstByte = *((char*)&value);
  if (firstByte == 1) reply[3] = 1;
  else reply[3] = 2;
  // get the size of an "int"
  reply[4] = sizeof(int);
  // get the size of an "long"
  reply[5] = sizeof(long);
#if CMK_LONG_LONG_DEFINED
  // get the size of an "long long"
  reply[6] = sizeof(long long);
#else
  // Per Filippo, the debugger will be fine with this. It should never
  // come up, since configure didn't detect support for `long long` on
  // the machine.
  reply[6] = 0;
#endif
  // get the size of an "bool"
  reply[7] = sizeof(bool);
  CcsSendReply(8, (void*)reply);
  CmiFree(msg);
}

static void CpdList_ccs_list_items_fmt(char *msg)
{
  CpdListItemsRequest req;
  CpdListAccessor *acc=CpdListHeader_ccs_list_items(msg,req);
  if (acc!=NULL) {
    int bufLen;
    { 
      PUP_toNetwork_sizer ps;
      PUP_fmt p(ps); 
      pupCpd(p,acc,req);
      bufLen=ps.size(); 
    }
    char *buf=new char[bufLen];
    { 
      PUP_toNetwork_pack pp(buf); 
      PUP_fmt p(pp);
      pupCpd(p,acc,req);
      if (pp.size()!=bufLen)
	CmiError("ERROR! Sizing/packing length mismatch for %s list pup function (%d sizing, %zu packing)\n",
		acc->getPath(),bufLen,pp.size());
    }
    CcsSendReply(bufLen,(void *)buf);
    delete[] buf;
  }
  CmiFree(msg);
}




//Introspection object-- extract a list of CpdLists!
class CpdList_introspect : public CpdListAccessor {
  CpdListTable_t *tab;
public:
  CpdList_introspect(CpdListTable_t *tab_) :tab(tab_) { }
  virtual const char *getPath(void) const { return "converse/lists";}
  virtual size_t getLength(void) const {
    size_t len=0;
    CkHashtableIterator *it=tab->iterator();
    while (NULL!=it->next()) len++;
    delete it;
    return len;
  }
  virtual void pup(PUP::er &p,CpdListItemsRequest &req) {
    CkHashtableIterator *it=tab->iterator();
    void *objp;
    int curObj=0;
    while (NULL!=(objp=it->next())) {
      if (curObj>=req.lo && curObj<req.hi) {
	CpdListAccessor *acc=*(CpdListAccessor **)objp;
	char *pathName=(char *)acc->getPath();
	beginItem(p,curObj);
        p.comment("name");
	p(pathName,strlen(pathName));
      }
      curObj++;
    }
  }
};




#endif /*CMK_CCS_AVAILABLE*/
/*We have to include these virtual functions, even when CCS is
disabled, to avoid bizarre link-time errors.*/

// C++ and C client API
void CpdListRegister(CpdListAccessor *acc)
#if CMK_CCS_AVAILABLE
{
  CpvAccess(cpdListTable)->put(acc->getPath())=acc;
}
#else
{ }
#endif

void CpdListRegister_c(const char *path,
            CpdListLengthFn_c len,void *lenParam,
            CpdListItemsFn_c items,void *itemsParam,int checkBoundary)
#if CMK_CCS_AVAILABLE
{
  CpdListRegister(new CpdListAccessor_c(path,
	     len,lenParam,items,itemsParam,checkBoundary!=0?true:false));
}
#else
{ }
#endif

#if CMK_CCS_AVAILABLE


// Initialization	

static void CpdListInit(void) {
  CpvInitialize(CpdListTable_t *,cpdListTable);
  CpvAccess(cpdListTable)=new CpdListTable_t(31,0.5,
	      CkHashFunction_string,CkHashCompare_string);
  CpdListRegister(new CpdList_introspect(CpvAccess(cpdListTable)));

  CcsRegisterHandler("ccs_list_len",(CmiHandler)CpdList_ccs_list_len);
  CcsRegisterHandler("ccs_list_items.txt",(CmiHandler)CpdList_ccs_list_items_txt);
  CcsRegisterHandler("ccs_list_items.fmt",(CmiHandler)CpdList_ccs_list_items_fmt);
  CcsRegisterHandler("ccs_list_items.set",(CmiHandler)CpdList_ccs_list_items_set);
  CcsRegisterHandler("debug/converse/arch",(CmiHandler)CpdMachineArchitecture);
}

#if CMK_WEB_MODE
/******************************************************
Web performance monitoring interface:
	Clients will register for performance data with 
processor 0 by calling the "perf_monitor" CCS request.  
Every WEB_INTERVAL (few seconds), this code
calls all registered web performance functions on all processors.  
The resulting integers are sent back to the client as a 
CCS reply.  

The current reply format is ASCII and rather nasty:
it's the string "perf" followed by space-separated list
of the performance functions for each processor.  By default,
only two performance functions are registered: the current 
processor utilization, in percent; and the current queue length.

The actual call sequence is:
CCS Client->CWebHandler->...  (processor 0)
  ...->CWeb_Collect->... (all processors)
...->CWeb_Reduce->CWeb_Deliver (processor 0 again)
*/

#if 0
#  define WEBDEBUG(x) CmiPrintf x
#else
#  define WEBDEBUG(x) /*empty*/
#endif

#define WEB_INTERVAL 1000 /*Time, in milliseconds, between performance snapshots*/
#define MAXFNS 20 /*Largest number of performance functions to expect*/

typedef struct {
	char hdr[CmiReservedHeaderSize];
	int fromPE;/*Source processor*/
	int perfData[MAXFNS];/*Performance numbers*/
} CWeb_CollectedData;

/*This needs to be made into a list of registered clients*/
static int hasApplet=0;
static CcsDelayedReply appletReply;

typedef int (*CWebFunction)(void);
static CWebFunction CWebPerformanceFunctionArray[MAXFNS];
static int CWebNoOfFns;
static int CWeb_ReduceIndex;
static int CWeb_CollectIndex;

/*Deliver the reduced web performance data to the waiting client:
*/
static int collectedCount;
static CWeb_CollectedData **collectedValues;

static void CWeb_Deliver(void)
{
  int i,j;

  if (hasApplet) {
    WEBDEBUG(("CWeb_Deliver to applet\n"));
    /*Send the performance data off to the applet*/
    int reply_len = 6+14+CmiNumPes()*CWebNoOfFns;
    char *reply=(char *)malloc(reply_len);
    snprintf(reply,reply_len,"perf");
  
    for(i=0; i<CmiNumPes(); i++){
      for (j=0;j<CWebNoOfFns;j++)
      {
        char buf[20];
        snprintf(buf,sizeof(buf)," %d",collectedValues[i]->perfData[j]);
        strcat(reply,buf);
      }
    }
    CcsSendDelayedReply(appletReply,strlen(reply) + 1, reply);
    free(reply);
    hasApplet=0;
  }
  else
    WEBDEBUG(("CWeb_Deliver (NO APPLET)\n"));
  
  /* Free saved performance data */
  for(i = 0; i < CmiNumPes(); i++){
    CmiFree(collectedValues[i]);
    collectedValues[i] = 0;
  }
  collectedCount = 0;
}

/*On PE 0, this handler accumulates all the performace data
*/
static void CWeb_Reduce(void *msg){
  CWeb_CollectedData *cur,*prev;
  int src;
  if(CmiMyPe() != 0){
    CmiAbort("CWeb performance data sent to wrong processor...\n");
  }
  WEBDEBUG(("CWeb_Reduce"));
  cur=(CWeb_CollectedData *)msg;
  src=cur->fromPE;
  prev = collectedValues[src]; /* Previous value, ideally 0 */
  collectedValues[src] = cur;
  if(prev == 0) collectedCount++;
  else CmiFree(prev); /*<- caused by out-of-order perf. data delivery*/

  if(collectedCount == CmiNumPes()){
    CWeb_Deliver();
  }
}

/*On each PE, this handler collects the performance data
and sends it to PE 0.
*/
static void CWeb_Collect(void)
{
  CWeb_CollectedData *msg;
  int i;

  WEBDEBUG(("CWeb_Collect on %d\n",CmiMyPe()));
  msg = (CWeb_CollectedData *)CmiAlloc(sizeof(CWeb_CollectedData));
  msg->fromPE = CmiMyPe();
  
  /* Evaluate each performance function*/
  for(i = 0; i < CWebNoOfFns; i++)
    msg->perfData[i] = CWebPerformanceFunctionArray[i] ();

  /* Send result off to node 0 */  
  CmiSetHandler(msg, CWeb_ReduceIndex);
  CmiSyncSendAndFree(0, sizeof(CWeb_CollectedData), msg);

  /* Re-call this function after a delay */
  CcdCallFnAfter((CcdVoidFn)CWeb_Collect, 0, WEB_INTERVAL);
}

void CWebPerformanceRegisterFunction(CWebFunction fn)
{
  if (CmiMyRank()!=0) return; /* Should only register from rank 0 */
  if (CWebNoOfFns>=MAXFNS) CmiAbort("Registered too many CWebPerformance functions!");
  CWebPerformanceFunctionArray[CWebNoOfFns] = fn;
  CWebNoOfFns++;
}

/*This is called on PE 0 by clients that wish
to receive performance data.
*/
static void CWebHandler(void){
  if(CcsIsRemoteRequest()) {
    static int startedCollection=0;
    
    WEBDEBUG(("CWebHandler request on %d\n",CmiMyPe()));    
    hasApplet=1;
    appletReply=CcsDelayReply();
    
    if(startedCollection == 0){
      WEBDEBUG(("Starting data collection on every processor\n"));    
      int i;
      startedCollection=1;
      collectedCount=0;
      collectedValues = (CWeb_CollectedData **)malloc(sizeof(void *) * CmiNumPes());
      for(i = 0; i < CmiNumPes(); i++)
        collectedValues[i] = 0;
      
      /*Start collecting data on each processor*/
      for(i = 0; i < CmiNumPes(); i++){
        char *msg = (char *)CmiAlloc(CmiReservedHeaderSize);
        CmiSetHandler(msg, CWeb_CollectIndex);
        CmiSyncSendAndFree(i, CmiReservedHeaderSize,msg);
      }
    }
  }
}

/** This "usage" section keeps track of percent of wall clock time
spent actually processing messages on each processor.   
It's a simple performance measure collected by the CWeb framework.
**/
struct CWebModeStats {
public:
	double beginTime; ///< Start of last collection interval
	double startTime; ///< Start of last busy time
	double usedTime; ///< Total busy time in last collection interval
	int PROCESSING; ///< If 1, processor is busy
};
CpvStaticDeclare(CWebModeStats *,cwebStats);

/* Called to begin a collection interval
*/
static void usageReset(CWebModeStats *stats, double curWallTime)
{
   stats->beginTime=curWallTime;
   stats->usedTime = 0.;
}

/* Called when processor becomes busy
*/
static void usageStart(CWebModeStats *stats)
{
   stats->startTime  = CmiWallTimer();
   stats->PROCESSING = 1;
}

/* Called when processor becomes idle
*/
static void usageStop(CWebModeStats *stats)
{
   stats->usedTime   += CmiWallTimer() - stats->startTime;
   stats->PROCESSING = 0;
}

/* Call this when the program is started
 -> Whenever traceModuleInit would be called
 -> -> see conv-core/convcore.C
*/
static void initUsage()
{
   CpvInitialize(CWebModeStats *, cwebStats);
   CWebModeStats *stats=new CWebModeStats;
   CpvAccess(cwebStats)=stats;
   usageReset(stats, CmiWallTimer());
   usageStart(stats);
   CcdCallOnConditionKeep(CcdPROCESSOR_BEGIN_BUSY,(CcdCondFn)usageStart,stats);
   CcdCallOnConditionKeep(CcdPROCESSOR_BEGIN_IDLE,(CcdCondFn)usageStop,stats);
}

static int getUsage(void)
{
   int usage = 0;
   double time      = CmiWallTimer();
   CWebModeStats *stats=CpvAccess(cwebStats);
   double totalTime = time - stats->beginTime;

   if(stats->PROCESSING)
   { /* Lock in current CPU usage */
      usageStop(stats); usageStart(stats);
   }
   if(totalTime > 0.)
      usage = (int)(0.5 + 100 *stats->usedTime/totalTime);
   usageReset(stats,time);

   return usage;
}

static int getSchedQlen(void)
{
  return(CqsLength((Queue)CpvAccess(CsdSchedQueue)));
}

#endif /*CMK_WEB_MODE*/

#if ! CMK_WEB_MODE
static void CWeb_Invalid(void)
{
  CmiAbort("Invalid web mode handler invoked!\n");
}
#endif

void CWebInit(void)
{
#if CMK_WEB_MODE
  CcsRegisterHandler("perf_monitor", (CmiHandler)CWebHandler);
  
  CmiAssignOnce(&CWeb_CollectIndex, CmiRegisterHandler((CmiHandler)CWeb_Collect));
  CmiAssignOnce(&CWeb_ReduceIndex, CmiRegisterHandler((CmiHandler)CWeb_Reduce));
  
  initUsage();
  CWebPerformanceRegisterFunction(getUsage);
  CWebPerformanceRegisterFunction(getSchedQlen);
#else
  /* always maintain the consistent CmiHandler table */
  /* which is good for heterogeneous clusters */
  CmiRegisterHandler((CmiHandler)CWeb_Invalid);
  CmiRegisterHandler((CmiHandler)CWeb_Invalid);
#endif
}


void CcsBuiltinsInit(char **argv)
{
  CcsRegisterHandler("ccs_getinfo",(CmiHandler)ccs_getinfo);
  CcsRegisterHandler("ccs_killport",(CmiHandler)ccs_killport);
  CcsRegisterHandler("ccs_killpe",(CmiHandler)ccs_killpe);
  CWebInit();
  CpdListInit();
}


#endif /*CMK_CCS_AVAILABLE*/

void PUP_fmt::fieldHeader(typeCode_t typeCode,int nItems) {
    // Compute and write intro byte:
    lengthLen_t ll;
    if (nItems==1) ll=lengthLen_single;
    else if (nItems<256) ll=lengthLen_byte;
    else ll=lengthLen_int;
    // CmiPrintf("Intro byte: l=%d t=%d\n",(int)ll,(int)typeCode);
    byte intro=(((int)ll)<<4)+(int)typeCode;
    p(intro);
    // Compute and write length:
    switch(ll) {
    case lengthLen_single: break; // Single item
    case lengthLen_byte: {
        byte l=nItems;
        p(l);
        } break;
    case lengthLen_int: {
        p(nItems); 
        } break;
    case lengthLen_long: CmiAbort("Should not have reached here!"); break;
    };
}

void PUP_fmt::comment(const char *message) {
	size_t nItems=strlen(message);
	fieldHeader(typeCode_comment,nItems);
	p((char *)message,nItems);
}
void PUP_fmt::synchronize(unsigned int m) {
	fieldHeader(typeCode_sync,1);
	p(m);
}

void PUP_fmt::pup_buffer_async(void *&ptr,size_t n,size_t itemSize,PUP::dataType t) {
  bytes(ptr, n, itemSize, t);
}

void PUP_fmt::pup_buffer_async(void *&ptr,size_t n, size_t itemSize, PUP::dataType t, std::function<void *(size_t)> allocate, std::function<void (void *)> deallocate){
  bytes(ptr, n, itemSize, t);
}

void PUP_fmt::bytes(void *ptr,size_t n,size_t itemSize,PUP::dataType t) {
	if(itemSize > INT_MAX || n > INT_MAX || itemSize*n > INT_MAX)
		CmiAbort("Ccs does not support messages greater than INT_MAX...\n");
	switch(t) {
	case PUP::Tchar:
	case PUP::Tuchar:
	case PUP::Tbyte:
		fieldHeader(typeCode_byte,n);
		p.bytes(ptr,n,itemSize,t);
		break;
	case PUP::Tshort: case PUP::Tint:
	case PUP::Tushort: case PUP::Tuint:
	case PUP::Tbool:
		fieldHeader(typeCode_int,n);
		p.bytes(ptr,n,itemSize,t);
		break;
	// treat "long" and "pointer" as 8-bytes, in conformity with pup_toNetwork.C
	case PUP::Tlong: case PUP::Tlonglong:
	case PUP::Tulong: case PUP::Tulonglong:
		fieldHeader(typeCode_long,n);
		p.bytes(ptr,n,itemSize,t);
		break;
	case PUP::Tfloat:
		fieldHeader(typeCode_float,n);
		p.bytes(ptr,n,itemSize,t);
		break;
	case PUP::Tdouble: case PUP::Tlongdouble:
		fieldHeader(typeCode_double,n);
		p.bytes(ptr,n,itemSize,t);
		break;
    case PUP::Tpointer:
        fieldHeader(typeCode_pointer,n);
        p.bytes(ptr,n,itemSize,t);
        break;
	default: CmiAbort("Unrecognized type code in PUP_fmt::bytes");
	};
}


