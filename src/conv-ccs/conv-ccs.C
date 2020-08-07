#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

#include "converse.h"
#include "conv-ccs.h"
#include "ccs-server.h"
#include "sockRoutines.h"
#include "queueing.h"

#ifdef _WIN32
# include <io.h>
# define read _read
#endif

void CpdEndConditionalDeliver_master(void);

#if CMK_CCS_AVAILABLE

int CcsReply(CcsImplHeader *rep,int repLen,const void *repData);

/*****************************************************************************
 *
 * Converse Client-Server Functions
 *
 *****************************************************************************/
 
static void initHandlerRec(CcsHandlerRec *c,const char *name) {
  if (strlen(name)>=CCS_MAXHANDLER) 
  	CmiAbort("CCS handler names cannot exceed 32 characters");
  c->name=strdup(name);
  c->fn=NULL;
  c->fnOld=NULL;
  c->userPtr=NULL;
  c->mergeFn=NULL;
  c->nCalls=0;
}

static void callHandlerRec(CcsHandlerRec *c,int reqLen,const void *reqData) {
	c->nCalls++;
	if (c->fnOld) 
	{ /* Backward compatability version:
	    Pack user data into a converse message (cripes! why bother?);
	    user will delete the message. 
	  */
		char *cmsg = (char *) CmiAlloc(CmiReservedHeaderSize+reqLen);
		memcpy(cmsg+CmiReservedHeaderSize, reqData, reqLen);
		(c->fnOld)(cmsg);
	}
	else { /* Pass read-only copy of data straight to user */
		(c->fn)(c->userPtr, reqLen, reqData);
	}
}

/*Table maps handler name to CcsHandler object*/
CpvDeclare(CcsHandlerTable, ccsTab);

CpvStaticDeclare(CcsImplHeader*,ccsReq);/*Identifies CCS requestor (client)*/

void CcsRegisterHandler(const char *name, CmiHandler fn) {
  CcsHandlerRec cp;
  initHandlerRec(&cp,name);
  cp.fnOld=fn;
  *(CcsHandlerRec *)CkHashtablePut(CpvAccess(ccsTab),(void *)&cp.name)=cp;
}
void CcsRegisterHandlerFn(const char *name, CcsHandlerFn fn, void *ptr) {
  CcsHandlerRec cp;
  initHandlerRec(&cp,name);
  cp.fn=fn;
  cp.userPtr=ptr;
  *(CcsHandlerRec *)CkHashtablePut(CpvAccess(ccsTab),(void *)&cp.name)=cp;
}
CcsHandlerRec *CcsGetHandler(const char *name) {
  return (CcsHandlerRec *)CkHashtableGet(CpvAccess(ccsTab),(void *)&name);
}
void CcsSetMergeFn(const char *name, CmiReduceMergeFn newMerge) {
  CcsHandlerRec *rec=(CcsHandlerRec *)CkHashtableGet(CpvAccess(ccsTab),(void *)&name);
  if (rec==NULL) {
    CmiAbort("[%d] CCS: Unknown CCS handler name %s.\n",CmiMyPe(),name);
  }
  rec->mergeFn=newMerge;
  rec->redID=CmiGetGlobalReduction();
}

void * CcsMerge_concat(size_t *size,void *local,void **remote,int n) {
  CcsImplHeader *hdr;
  size_t total = *size;
  void *reply;
  char *ptr;
  int i;
  for (i=0; i<n; ++i) {
    hdr = (CcsImplHeader*)(((char*)remote[i])+CmiReservedHeaderSize);
    total += ChMessageInt(hdr->len);
  }
  reply = CmiAlloc(total);
  memcpy(reply, local, *size);
  ((CcsImplHeader*)(((char*)reply)+CmiReservedHeaderSize))->len = ChMessageInt_new(total-CmiReservedHeaderSize-sizeof(CcsImplHeader));
  CmiFree(local);
  ptr = ((char*)reply)+*size;
  for (i=0; i<n; ++i) {
    int len = ChMessageInt(((CcsImplHeader*)(((char*)remote[i])+CmiReservedHeaderSize))->len);
    memcpy(ptr, ((char*)remote[i])+CmiReservedHeaderSize+sizeof(CcsImplHeader), len);
    ptr += len;
  }
  *size = total;
  return reply;
}

#define SIMPLE_REDUCTION(name, dataType, loop) \
void * CcsMerge_##name(size_t *size,void *local,void **remote,int n) { \
  int i, m; \
  CcsImplHeader *hdrLocal = (CcsImplHeader*)(((char*)local)+CmiReservedHeaderSize); \
  size_t lenLocal = ChMessageInt(hdrLocal->len); \
  size_t nElem = lenLocal / sizeof(dataType); \
  dataType *ret = (dataType *) (hdrLocal+1); \
  CcsImplHeader *hdr; \
  for (m=0; m<n; ++m) { \
    size_t len; \
    dataType *value; \
    hdr = (CcsImplHeader*)(((char*)remote[m])+CmiReservedHeaderSize); \
    len = ChMessageInt(hdr->len); \
    value = (dataType *)(hdr+1); \
    CmiAssert(lenLocal == len); \
    for (i=0; i<nElem; ++i) loop; \
  } \
  return local; \
}

SIMPLE_REDUCTION(logical_and, int, ret[i]=(ret[i]&&value[i])?1:0)
SIMPLE_REDUCTION(logical_or, int, ret[i]=(ret[i]||value[i])?1:0)
SIMPLE_REDUCTION(bitvec_and, int, ret[i]&=value[i])
SIMPLE_REDUCTION(bitvec_or, int, ret[i]|=value[i])

/*Use this macro for reductions that have the same type for all inputs */
#define SIMPLE_POLYMORPH_REDUCTION(nameBase,loop) \
  SIMPLE_REDUCTION(nameBase##_int, int, loop) \
  SIMPLE_REDUCTION(nameBase##_float, float, loop) \
  SIMPLE_REDUCTION(nameBase##_double, double, loop)

SIMPLE_POLYMORPH_REDUCTION(sum, ret[i]+=value[i])
SIMPLE_POLYMORPH_REDUCTION(product, ret[i]*=value[i])
SIMPLE_POLYMORPH_REDUCTION(max, if (ret[i]<value[i]) ret[i]=value[i])
SIMPLE_POLYMORPH_REDUCTION(min, if (ret[i]>value[i]) ret[i]=value[i])

#undef SIMPLE_REDUCTION
#undef SIMPLE_POLYMORPH_REDUCTION

int CcsEnabled(void)
{
  return 1;
}

int CcsIsRemoteRequest(void)
{
  return CpvAccess(ccsReq)!=NULL;
}

void CcsCallerId(skt_ip_t *pip, unsigned int *pport)
{
  *pip = CpvAccess(ccsReq)->attr.ip;
  *pport = ChMessageInt(CpvAccess(ccsReq)->attr.port);
}

extern int rep_fw_handler_idx;
int rep_fw_handler_idx;

CcsDelayedReply CcsDelayReply(void)
{
  CcsDelayedReply ret;
  int len = sizeof(CcsImplHeader);
  if (ChMessageInt(CpvAccess(ccsReq)->pe) < -1)
    len += ChMessageInt(CpvAccess(ccsReq)->pe) * sizeof(int);
  ret.hdr = (CcsImplHeader*)malloc(len);
  memcpy(ret.hdr, CpvAccess(ccsReq), len);
  CpvAccess(ccsReq)=NULL;
  return ret;
}

void CcsSendReply(int replyLen, const void *replyData)
{
  if (CpvAccess(ccsReq)==NULL)
    CmiAbort("CcsSendReply: reply already sent!\n");
  CpvAccess(ccsReq)->len = ChMessageInt_new(1);
  CcsReply(CpvAccess(ccsReq),replyLen,replyData);
  CpvAccess(ccsReq) = NULL;
}

void CcsSendReplyNoError(int replyLen, const void *replyData) {
  if (CpvAccess(ccsReq)==NULL) return;
  CcsSendReply(replyLen, replyData);
}

void CcsSendDelayedReply(CcsDelayedReply d,int replyLen, const void *replyData)
{
  CcsImplHeader *h = d.hdr;
  h->len=ChMessageInt_new(1);
  CcsReply(h,replyLen,replyData);
  free(h);
}

void CcsNoReply(void)
{
  if (CpvAccess(ccsReq)==NULL) return;
  CpvAccess(ccsReq)->len = ChMessageInt_new(0);
  CcsReply(CpvAccess(ccsReq),0,NULL);
  CpvAccess(ccsReq) = NULL;
}

void CcsNoDelayedReply(CcsDelayedReply d)
{
  CcsImplHeader *h = d.hdr;
  h->len = ChMessageInt_new(0);
  CcsReply(h,0,NULL);
  free(h);
}


/**********************************
_CCS Implementation Routines:
  These do the request forwarding and
delivery.
***********************************/

/*CCS Bottleneck:
  Deliver the given message data to the given
CCS handler.
*/
void CcsHandleRequest(CcsImplHeader *hdr,const char *reqData)
{
  char *cmsg;
  int reqLen=ChMessageInt(hdr->len);
/*Look up handler's converse ID*/
  char *handlerStr=hdr->handler;
  CcsHandlerRec *fn=(CcsHandlerRec *)CkHashtableGet(CpvAccess(ccsTab),(void *)&handlerStr);
  //  CmiPrintf("[%d] CCS: CCS handler name '%s' requested.\n",CmiMyPe(), hdr->handler);
  if (fn==NULL) {
    CmiPrintf("[%d] CCS: Unknown CCS handler name '%s' requested. Ignoring...\n"
	      ,CmiMyPe(), hdr->handler);
    CpvAccess(ccsReq)=hdr;
    CcsSendReply(0,NULL); /*Send an empty reply to the possibly waiting client*/
    return;
 /*   CmiAbort("CCS: Unknown CCS handler name.\n");*/
  }

/* Call the handler */
  CpvAccess(ccsReq)=hdr;
#if CMK_CHARMDEBUG
  if (conditionalPipe[1]!=0 && _conditionalDelivery==0) {
    /* We are conditionally delivering, the message has been sent to the child, wait for its response */
    int bytes;
    if (4==read(conditionalPipe[0], &bytes, 4)) {
      char *buf = (char *)malloc(bytes);
      if (bytes == read(conditionalPipe[0], buf, bytes))
      {
        CcsSendReply(bytes,buf);
        free(buf);
      }
      else
      {
        free(buf);
        CpdEndConditionalDeliver_master();
      }
    } else {
      /* the pipe has been closed */
      CpdEndConditionalDeliver_master();
   }
  }
  else
#endif
  {
    callHandlerRec(fn,reqLen,reqData);
  
/*Check if a reply was sent*/
    if (CpvAccess(ccsReq)!=NULL)
      CcsSendReply(0,NULL);/*Send an empty reply if not*/
  }
}

#if ! NODE_0_IS_CONVHOST
/* The followings are necessary to prevent CCS requests to be processed before
 * CCS has been initialized. Really it matters only when NODE_0_IS_CONVHOST=0, but
 * it doesn't hurt having it in the other case as well */
static char **bufferedMessages = NULL;
static int CcsNumBufferedMsgs = 0;
#define CCS_MAX_NUM_BUFFERED_MSGS  100

void CcsBufferMessage(char *msg) {
  CmiPrintf("Buffering CCS message\n");
  CmiAssert(CcsNumBufferedMsgs < CCS_MAX_NUM_BUFFERED_MSGS);
  if (CcsNumBufferedMsgs < 0) CmiAbort("Why is a CCS message being buffered now???");
  if (bufferedMessages == NULL) bufferedMessages = (char **)malloc(sizeof(char*)*CCS_MAX_NUM_BUFFERED_MSGS);
  bufferedMessages[CcsNumBufferedMsgs] = msg;
  CcsNumBufferedMsgs ++;
}
#endif
  
/*Unpacks request message to call above routine*/
int _ccsHandlerIdx = 0;/*Converse handler index of routine req_fw_handler*/

void req_fw_handler(char *msg);

void CcsReleaseMessages(void) {
#if ! NODE_0_IS_CONVHOST
  if (CcsNumBufferedMsgs > 0) {
    int i;
    //CmiPrintf("CCS: %d messages released\n",CcsNumBufferedMsgs);
    for (i=0; i<CcsNumBufferedMsgs; ++i) {
      CmiSetHandler(bufferedMessages[i], _ccsHandlerIdx);
      CsdEnqueue(bufferedMessages[i]);
    }
    free(bufferedMessages);
    bufferedMessages = NULL;
    CcsNumBufferedMsgs = -1;
  }
#endif
}

/*Convert CCS header & message data into a converse message 
 addressed to handler*/
char *CcsImpl_ccs2converse(const CcsImplHeader *hdr,const void *data,int *ret_len)
{
  int reqLen=ChMessageInt(hdr->len);
  int destPE = ChMessageInt(hdr->pe);
  int len;
  char *msg;
  if (destPE < -1) reqLen -= destPE*sizeof(int);
  len=CmiReservedHeaderSize+sizeof(CcsImplHeader)+reqLen;
  msg=(char *)CmiAlloc(len);
  memcpy(msg+CmiReservedHeaderSize,hdr,sizeof(CcsImplHeader));
  memcpy(msg+CmiReservedHeaderSize+sizeof(CcsImplHeader),data,reqLen);
  if (ret_len!=NULL) *ret_len=len;
  if (_ccsHandlerIdx != 0) {
    CmiSetHandler(msg, _ccsHandlerIdx);
    return msg;
  } else {
#if NODE_0_IS_CONVHOST
    CmiAbort("Why do we need to buffer messages when node 0 is Convhost?");
#else
    CcsBufferMessage(msg);
#endif
    return NULL;
  }
}

/*Receives reply messages passed up from
converse to node 0.*/
static void rep_fw_handler(char *msg)
{
  int len;
  char *r=msg+CmiReservedHeaderSize;
  CcsImplHeader *hdr=(CcsImplHeader *)r; 
  r+=sizeof(CcsImplHeader);
  len=ChMessageInt(hdr->len);
  CcsImpl_reply(hdr,len,r);
  CmiFree(msg);
}

#if NODE_0_IS_CONVHOST
/************** NODE_0_IS_CONVHOST ***********
Non netlrts- versions of charm++ are run without a 
(real) conv-host program.  This is fine, except 
CCS clients connect via conv-host; so for CCS
on non-netlrts- versions of charm++, node 0 carries
out the CCS forwarding normally done in conv-host.

CCS works by listening to a TCP connection on a 
port-- the Ccs server socket.  A typical communcation
pattern is:

1.) Random program (CCS client) from the net
connects to the CCS server socket and sends
a CCS request.

2.) Node 0 forwards the request to the proper
PE as a regular converse message (built in CcsImpl_netReq)
for CcsHandleRequest.

3.) CcsHandleRequest looks up the user's pre-registered
CCS handler, and passes the user's handler the request data.

4.) The user's handler calls CcsSendReply with some
reply data; OR finishes without calling CcsSendReply,
in which case CcsHandleRequest does it.

5.) CcsSendReply forwards the reply back to node 0,
which sends the reply back to the original requestor,
on the (still-open) request socket.
 */

/**
Send a Ccs reply back to the requestor, down the given socket.
Since there is no conv-host, node 0 does all the CCS 
communication-- this means all requests come to node 0
and are forwarded out; all replies are forwarded back to node 0.

Note: on Net- versions, CcsImpl_reply is implemented in machine.C
*/
void CcsImpl_reply(CcsImplHeader *rep,int repLen,const void *repData)
{
  const int repPE=0;
  rep->len=ChMessageInt_new(repLen);
  if (CmiMyPe()==repPE) {
    /*Actually deliver reply data*/
    CcsServer_sendReply(rep,repLen,repData);
  } else {
    /*Forward data & socket # to the replyPE*/
    int len=CmiReservedHeaderSize+
           sizeof(CcsImplHeader)+repLen;
    char *msg = (char *)CmiAlloc(len);
    char *r=msg+CmiReservedHeaderSize;
    *(CcsImplHeader *)r=*rep; r+=sizeof(CcsImplHeader);
    memcpy(r,repData,repLen);
    CmiSetHandler(msg,rep_fw_handler_idx);
    CmiSyncSendAndFree(repPE,len,msg);
  }
}

/*No request will be sent through this socket.
Closes it.
*/
/*void CcsImpl_noReply(CcsImplHeader *hdr)
{
  int fd=ChMessageInt(hdr->replyFd);
  skt_close(fd);
}*/

/**
 * This is the entrance point of a CCS request into the server.
 * It is executed only on proc 0, and it forwards the request to the appropriate PE.
 */
void CcsImpl_netRequest(CcsImplHeader *hdr,const void *reqData)
{
  char *msg;
  int len,repPE=ChMessageInt(hdr->pe);
  if (repPE<=-CmiNumPes() || repPE>=CmiNumPes()) {
    /*Treat out of bound values as errors. Helps detecting bugs*/
    if (repPE==-CmiNumPes()) CmiPrintf("Invalid processor index in CCS request: are you trying to do a broadcast instead?");
    else CmiPrintf("Invalid processor index in CCS request.");
    CpvAccess(ccsReq)=hdr;
    CcsSendReply(0,NULL); /*Send an empty reply to the possibly waiting client*/
    return;
  }

  msg=CcsImpl_ccs2converse(hdr,reqData,&len);
  if (repPE >= 0) {
    /* The following %CmiNumPes() follows the assumption that in BigSim the mapping is round-robin */
    //CmiPrintf("CCS message received for %d\n",repPE);
    CmiSyncSendAndFree(repPE%CmiNumPes(),len,msg);
  } else if (repPE == -1) {
    /* Broadcast to all processors */
    //CmiPrintf("CCS broadcast received\n");
    CmiSyncSendAndFree(0,len,msg);
  } else {
    /* Multicast to -repPE processors, specified right at the beginning of reqData (as a list of pes) */
    int firstPE = ChMessageInt(*(ChMessageInt_t*)reqData);
    /* The following %CmiNumPes() follows the assumption that in BigSim the mapping is round-robin */
    //CmiPrintf("CCS multicast received\n");
    CmiSyncSendAndFree(firstPE%CmiNumPes(),len,msg);
  }
}

/*
We have to run a CCS server socket here on
node 0.  To keep the speed impact minimal,
we only probe for new connections (with CcsServerCheck)
occasionally.  
 */
#include <signal.h>
#include "ccs-server.C" /*Include implementation here in this case*/
#include "ccs-auth.C"

/*Check for ready Ccs messages:*/
void CcsServerCheck(void)
{
  while (1==skt_select1(CcsServer_fd(),0)) {
    CcsImplHeader hdr;
    void *data;
    /* printf("Got CCS connect...\n"); */
    if (CcsServer_recvRequest(&hdr,&data))
    {/*We got a network request*/
      /* printf("Got CCS request...\n"); */
      if (! check_stdio_header(&hdr)) {
        CcsImpl_netRequest(&hdr,data);
      }
      free(data);
    }
  }
}

#endif /*NODE_0_IS_CONVHOST*/

int _isCcsHandlerIdx(int hIdx) {
  if (hIdx==_ccsHandlerIdx) return 1;
  if (hIdx==rep_fw_handler_idx) return 1;
  return 0;
}

void CcsBuiltinsInit(char **argv);

int cmiArgDebugFlag; // Value is 0, unless reset in ConverseCommonInit
CpvDeclare(char *, displayArgument);
CpvCExtern(int, cpdSuspendStartup);
CpvDeclare(int, cpdSuspendStartup);

void CcsInit(char **argv)
{
  CpvInitialize(CkHashtable_c, ccsTab);
  CpvAccess(ccsTab) = CkCreateHashtable_string(sizeof(CcsHandlerRec),5);
  CpvInitialize(CcsImplHeader *, ccsReq);
  CpvAccess(ccsReq) = NULL;
  CmiAssignOnce(&_ccsHandlerIdx,CmiRegisterHandler((CmiHandler)req_fw_handler));
  CpvInitialize(char *, displayArgument);
  CpvInitialize(int, cpdSuspendStartup);
  CpvAccess(displayArgument) = NULL;
  CpvAccess(cpdSuspendStartup) = 0;
  
  CcsBuiltinsInit(argv);

  CmiAssignOnce(&rep_fw_handler_idx, CmiRegisterHandler((CmiHandler)rep_fw_handler));
#if NODE_0_IS_CONVHOST
#if ! CMK_CMIPRINTF_IS_A_BUILTIN
  CmiAssignOnce(&print_fw_handler_idx, CmiRegisterHandler((CmiHandler)print_fw_handler));
#endif
  {
   int ccs_serverPort=0;
   char *ccs_serverAuth=NULL;
   
   if (CmiGetArgFlagDesc(argv,"++server", "Create a CCS server port") | 
      CmiGetArgIntDesc(argv,"++server-port",&ccs_serverPort, "Listen on this TCP/IP port number") |
      CmiGetArgStringDesc(argv,"++server-auth",&ccs_serverAuth, "Use this CCS authentication file")) 
     if (CmiMyPe()==0)
    {/*Create and occasionally poll on a CCS server port*/
      CcsServer_new(NULL,&ccs_serverPort,ccs_serverAuth);
      CcdCallOnConditionKeep(CcdPERIODIC,(CcdVoidFn)CcsServerCheck,NULL);
    }
  }
#endif
  /* if in parallel debug mode i.e ++cpd, freeze */
  if (cmiArgDebugFlag)
  {
     if (CmiGetArgStringDesc(argv, "+DebugDisplay",&(CpvAccess(displayArgument)), "X display for gdb used only in cpd mode"))
     {
        if (CpvAccess(displayArgument) == NULL)
            CmiPrintf("WARNING> NULL parameter for +DebugDisplay\n***");
     }
     else if (CmiMyPe() == 0)
     {
            /* only one processor prints the warning */
            CmiPrintf("WARNING> x term for gdb needs to be specified as +DebugDisplay by debugger\n***\n");
     }

     if (CmiGetArgFlagDesc(argv, "+DebugSuspend", "Suspend execution at beginning of program")) {
#if CMK_SMP       
       if(!CmiInCommThread()) CpvAccess(cpdSuspendStartup) = 1;
#else
       CpvAccess(cpdSuspendStartup) = 1;
#endif       
       //       CmiPrintf("[%d] set SuspendStartup %d\n",CmiMyPe(), CpvAccess(cpdSuspendStartup));
     }
  }

  CcsReleaseMessages();
}

#endif /*CMK_CCS_AVAILABLE*/

