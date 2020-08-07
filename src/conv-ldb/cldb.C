
#include <stdlib.h>
#include "queueing.h"
#include "cldb.h"
#include <math.h>

typedef char *BitVector;

CpvDeclare(int, CldHandlerIndex);
CpvDeclare(int, CldNodeHandlerIndex);
CpvDeclare(BitVector, CldPEBitVector);
CpvDeclare(int, CldBalanceHandlerIndex);

CpvDeclare(int, CldRelocatedMessages);
CpvDeclare(int, CldLoadBalanceMessages);
CpvDeclare(int, CldMessageChunks);
CpvDeclare(int, CldLoadNotify);

CpvDeclare(CmiNodeLock, cldLock);

extern void LoadNotifyFn(int);

static char s_lbtopo_default[] = "torus_nd_5";
extern char *_lbtopo;
char *_lbtopo = s_lbtopo_default;

/* Estimator stuff.  Of any use? */
/*
CpvStaticDeclare(CldEstimatorTable, _estfns);
*/
void CldRegisterEstimator(CldEstimator fn)
{
  /*CpvAccess(_estfns).fns[CpvAccess(_estfns).count++] = fn;*/
}

/* 
int CldEstimate(void)
{
  CldEstimatorTable *estab = &(CpvAccess(_estfns));
  int i, load=0;
  for(i=0; i<estab->count; i++)
    load += (*(estab->fns[i]))();
  return load;
}

static int CsdEstimator(void)
{
  return CsdLength();
}
*/

CpvDeclare(int, CldLoadOffset);


int CldRegisterInfoFn(CldInfoFn fn)
{
  return CmiRegisterHandler((CmiHandler)fn);
}

int CldRegisterPackFn(CldPackFn fn)
{
  return CmiRegisterHandler((CmiHandler)fn);
}

/* CldSwitchHandler takes a message and a new handler number.  It
 * changes the handler number to the new handler number and move the
 * old to the Xhandler part of the header.  When the message gets
 * handled, the handler should call CldRestoreHandler to put the old
 * handler back.
 *
 * CldPutToken puts a message in the scheduler queue in such a way
 * that it can be retreived from the queue.  Once the message gets
 * handled, it can no longer be retreived.  CldGetToken removes a
 * message that was placed in the scheduler queue in this way.
 * CldCountTokens tells you how many tokens are currently retreivable.  
*/

void CldSwitchHandler(char *cmsg, int handler)
{
#if CMK_MEM_CHECKPOINT
  int old_phase = CmiGetRestartPhase(cmsg);
#endif
  CmiSetXHandler(cmsg, CmiGetHandler(cmsg));
  CmiSetHandler(cmsg, handler);
#if CMK_MEM_CHECKPOINT
  CmiGetRestartPhase(cmsg) = old_phase;
#endif
}

void CldRestoreHandler(char *cmsg)
{
#if CMK_MEM_CHECKPOINT
  int old_phase = CmiGetRestartPhase(cmsg);
#endif
  CmiSetHandler(cmsg, CmiGetXHandler(cmsg));
#if CMK_MEM_CHECKPOINT
  CmiGetRestartPhase(cmsg) = old_phase;
#endif
}

void Cldhandler(char *);
 
typedef struct CldToken_s {
  char msg_header[CmiMsgHeaderSizeBytes];
  char *msg;  /* if null, message already removed */
  struct CldToken_s *pred;
  struct CldToken_s *succ;
} *CldToken;

typedef struct CldProcInfo_s {
  int tokenhandleridx;
  int load; /* number of items in doubly-linked circle besides sentinel */
  CldToken sentinel;
} *CldProcInfo;

CpvDeclare(CldProcInfo, CldProc);

static void CldTokenHandler(CldToken tok)
{
  CldProcInfo proc = CpvAccess(CldProc);
  if (tok->msg) {
    tok->pred->succ = tok->succ;
    tok->succ->pred = tok->pred;
    proc->load --;
    CmiHandleMessage(tok->msg);
  }
  else 
    CpvAccess(CldLoadOffset)--;
  if (CpvAccess(CldLoadNotify))
    LoadNotifyFn(CpvAccess(CldProc)->load);
  CmiFree(tok);
}

int CldCountTokensRank(int rank)
{
  return CpvAccessOther(CldProc, rank)->load;
}

int CldCountTokens(void)
{
  return (CpvAccess(CldProc)->load);
}

int CldLoad(void)
{
  return (CsdLength() - CpvAccess(CldLoadOffset));
}

int CldLoadRank(int rank)
{
  int len, offset;
  /* CmiLock(CpvAccessOther(cldLock, rank));  */
  len = CqsLength((Queue)CpvAccessOther(CsdSchedQueue, rank));
     /* CldLoadOffset is the empty token counter */
  offset = CpvAccessOther(CldLoadOffset, rank);
  /* CmiUnlock(CpvAccessOther(cldLock, rank)); */
  return len - offset;
}

void CldPutToken(char *msg)
{
  CldProcInfo proc = CpvAccess(CldProc);
  CldInfoFn ifn = (CldInfoFn)CmiHandlerToFunction(CmiGetInfo(msg));
  CldToken tok;
  int queueing, priobits; unsigned int *prioptr;
  size_t len;
  CldPackFn pfn;

  CmiLock(CpvAccess(cldLock));
  tok = (CldToken)CmiAlloc(sizeof(struct CldToken_s));
  tok->msg = msg;

  /* add token to the doubly-linked circle */
  tok->pred = proc->sentinel->pred;
  tok->succ = proc->sentinel;
  tok->pred->succ = tok;
  tok->succ->pred = tok;
  proc->load ++;
  /* add token to the scheduler */
  CmiSetHandler(tok, proc->tokenhandleridx);
  ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
  /* not sigio or thread safe */
  CsdEnqueueGeneral(tok, queueing, priobits, prioptr);
  CmiUnlock(CpvAccess(cldLock));
}

void CldPutTokenPrio(char *msg)
{
  CldProcInfo proc = CpvAccess(CldProc);
  CldInfoFn ifn = (CldInfoFn)CmiHandlerToFunction(CmiGetInfo(msg));
  CldToken tok, ptr;
  int queueing, priobits; unsigned int *prioptr, ints;
  size_t len;
  CldPackFn pfn;

  ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
  ints = (priobits+CINTBITS-1)/CINTBITS;

  CmiLock(CpvAccess(cldLock));
  tok = (CldToken)CmiAlloc(sizeof(struct CldToken_s));
  tok->msg = msg;

  /* find the right place */
  ptr = proc->sentinel->succ;
  while (ptr!=proc->sentinel) {
    int queueing1, priobits1; unsigned int *prioptr1, ints1;
    size_t len1;
    CldPackFn pfn1;
    ifn(ptr->msg, &pfn1, &len1, &queueing1, &priobits1, &prioptr1);
    ints1 = (priobits1+CINTBITS-1)/CINTBITS;

    if (!CqsPrioGT_(ints, prioptr, ints1, prioptr1)) { break;}
    ptr = ptr->succ;
  }

  /* add token to the doubly-linked circle */
  tok->succ = ptr;
  tok->pred = ptr->pred;
  tok->pred->succ = tok;
  tok->succ->pred = tok;
  proc->load ++;
  /* add token to the scheduler */
  CmiSetHandler(tok, proc->tokenhandleridx);
  /* not sigio or thread safe */
  CsdEnqueueGeneral(tok, queueing, priobits, prioptr);
  CmiUnlock(CpvAccess(cldLock));
}


static 
#if CMK_C_INLINE
inline 
#endif
void * _CldGetTokenMsg(CldProcInfo proc)
{
  CldToken tok;
  void *msg;
  
  tok = proc->sentinel->succ;
  if (tok == proc->sentinel) {
    return NULL;
  }
  tok->pred->succ = tok->succ;
  tok->succ->pred = tok->pred;
  proc->load --;
  msg = tok->msg;
  tok->msg = 0;
  return msg;
}

void CldGetToken(char **msg)
{
  CldProcInfo proc = CpvAccess(CldProc);
  CmiNodeLock cldlock = CpvAccess(cldLock);
  CmiLock(cldlock);
  *msg = (char *)_CldGetTokenMsg(proc);
  if (*msg) CpvAccess(CldLoadOffset)++;
  CmiUnlock(cldlock);
}

/* called at node level */
/* get token from processor of rank pe */
static 
#if CMK_C_INLINE
inline 
#endif
void CldGetTokenFromRank(char **msg, int rank)
{
  CldProcInfo proc = CpvAccessOther(CldProc, rank);
  CmiNodeLock cldlock = CpvAccessOther(cldLock, rank);
  CmiLock(cldlock);
  *msg = (char *)_CldGetTokenMsg(proc);
  if (*msg) CpvAccessOther(CldLoadOffset, rank)++;
  CmiUnlock(cldlock);
}

static 
#if CMK_C_INLINE
inline 
#endif
void * _CldGetTokenMsgAt(CldProcInfo proc, CldToken tok)
{
  void *msg;
  
  if (tok == proc->sentinel) return NULL;
  tok->pred->succ = tok->succ;
  tok->succ->pred = tok->pred;
  proc->load --;
  msg = tok->msg;
  tok->msg = 0;
  return msg;
}

/* called at node level */
/* get token from processor of rank pe */
static 
#if CMK_C_INLINE
inline 
#endif
void CldGetTokenFromRankAt(char **msg, int rank, CldToken tok)
{
  CldProcInfo proc = CpvAccessOther(CldProc, rank);
  CmiNodeLock cldlock = CpvAccessOther(cldLock, rank);
  CmiLock(cldlock);
  *msg = (char *)_CldGetTokenMsgAt(proc, tok);
  if (*msg) CpvAccessOther(CldLoadOffset, rank)++;
  CmiUnlock(cldlock);
}

/* Bit Vector Stuff */

int CldPresentPE(int pe)
{
  return CpvAccess(CldPEBitVector)[pe];
}

void CldMoveAllSeedsAway(void)
{
  char *msg;
  int queueing, priobits, pe;
  size_t len;
  unsigned int *prioptr;
  CldInfoFn ifn;  CldPackFn pfn;

  CldGetToken(&msg);
  while (msg != 0) {
    ifn = (CldInfoFn)CmiHandlerToFunction(CmiGetInfo(msg));
    ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
    CldSwitchHandler(msg, CpvAccess(CldBalanceHandlerIndex));
    pe = (((CrnRand()+CmiMyPe())&0x7FFFFFFF)%CmiNumPes());
    while (!CldPresentPE(pe))
      pe = (((CrnRand()+CmiMyPe())&0x7FFFFFFF)%CmiNumPes());
    CmiSyncSendAndFree(pe, len, msg);
    CldGetToken(&msg);
  }
}

void CldSetPEBitVector(const char *newBV)
{
  int i;
  
  for (i=0; i<CmiNumPes(); i++)
    CpvAccess(CldPEBitVector)[i] = newBV[i];
  if (!CldPresentPE(CmiMyPe()))
    CldMoveAllSeedsAway();
}

/* End Bit Vector Stuff */

static int _cldb_cs = 0;

void CldModuleGeneralInit(char **argv)
{
  CldToken sentinel = (CldToken)CmiAlloc(sizeof(struct CldToken_s));
  CldProcInfo proc;
  int i;

  CpvInitialize(CldProcInfo, CldProc);
  CpvInitialize(int, CldLoadOffset);
  CpvAccess(CldLoadOffset) = 0;
  CpvInitialize(int, CldLoadNotify);
  CpvInitialize(BitVector, CldPEBitVector);
  CpvAccess(CldPEBitVector) = (char *)malloc(CmiNumPes()*sizeof(char));
  for (i=0; i<CmiNumPes(); i++)
    CpvAccess(CldPEBitVector)[i] = 1;
  CpvAccess(CldProc) = (CldProcInfo)CmiAlloc(sizeof(struct CldProcInfo_s));
  proc = CpvAccess(CldProc);
  proc->load = 0;
  proc->tokenhandleridx = CmiRegisterHandler((CmiHandler)CldTokenHandler);
  proc->sentinel = sentinel;
  sentinel->succ = sentinel;
  sentinel->pred = sentinel;

  /* lock to protect token queue for immediate message and smp */
  CpvInitialize(CmiNodeLock, cldLock);
  CpvAccess(cldLock) = CmiCreateLock();

  _cldb_cs = CmiGetArgFlagDesc(argv, "+cldb_cs", "Converse> Print seed load balancing statistics.");
  
  if (CmiMyPe() == 0) {
    const char *stra = CldGetStrategy();
    if (strcmp(stra, "rand") != 0) {
      CmiPrintf("Charm++> %s seed load balancer.\n", stra);
    }
  } 
}

/* function can be called in an immediate handler at node level
   rank specify the rank of processor for the node to represent
   This function can also send as immeidate messages
*/
void CldMultipleSend(int pe, int numToSend, int rank, int immed)
{
  char **msgs;
  int queueing, priobits, i, numSent, done=0, parcelSize;
  size_t len, *msgSizes;
  unsigned int *prioptr;
  CldInfoFn ifn;
  CldPackFn pfn;

  msgs = (char **)calloc(numToSend, sizeof(char *));
  msgSizes = (size_t *)calloc(numToSend, sizeof(size_t));

  while (!done) {
    numSent = 0;
    parcelSize = 0;
    for (i=0; i<numToSend; i++) {
      CldGetTokenFromRank(&msgs[i], rank);
      if (msgs[i] != 0) {
	done = 1;
	numSent++;
	ifn = (CldInfoFn)CmiHandlerToFunction(CmiGetInfo(msgs[i]));
	ifn(msgs[i], &pfn, &len, &queueing, &priobits, &prioptr);
	msgSizes[i] = len;
	parcelSize += len;
	CldSwitchHandler(msgs[i], CpvAccessOther(CldBalanceHandlerIndex, rank));
        if (immed) CmiBecomeImmediate(msgs[i]);
      }
      else {
	done = 1;
	break;
      }
      if (parcelSize > MAXMSGBFRSIZE) {
	if(i<numToSend-1)
	  done = 0;
	numToSend -= numSent;
	break;
      }
    }
    if (numSent > 1) {
      if (immed)
        CmiMultipleIsend(pe, numSent, msgSizes, msgs);
      else
        CmiMultipleSend(pe, numSent, msgSizes, msgs);
      for (i=0; i<numSent; i++)
	CmiFree(msgs[i]);
      CpvAccessOther(CldRelocatedMessages, rank) += numSent;
      CpvAccessOther(CldMessageChunks, rank)++;
    }
    else if (numSent == 1) {
      if (immed) CmiBecomeImmediate(msgs[0]);
      CmiSyncSendAndFree(pe, msgSizes[0], msgs[0]);
      CpvAccessOther(CldRelocatedMessages, rank)++;
      CpvAccessOther(CldMessageChunks, rank)++;
    }
  }
  free(msgs);
  free(msgSizes);
}

/* function can be called in an immediate handler at node level
   rank specify the rank of processor for the node to represent
   This function can also send as immeidate messages
*/
void CldMultipleSendPrio(int pe, int numToSend, int rank, int immed)
{
  char **msgs;
  int queueing, priobits, i;
  size_t len, *msgSizes;
  unsigned int *prioptr;
  CldInfoFn ifn;
  CldPackFn pfn;
  CldToken tok;
  CldProcInfo proc = CpvAccess(CldProc);
  int count = 0;

  if (numToSend ==0) return;
  msgs = (char **)calloc(numToSend, sizeof(char *));
  msgSizes = (size_t *)calloc(numToSend, sizeof(size_t));

  tok = proc->sentinel->succ;
  if (tok == proc->sentinel) {
    free(msgs);
    free(msgSizes);
    return;
  }
  tok = tok->succ;
  while (tok!=proc->sentinel) {
    tok = tok->succ;
    if (tok == proc->sentinel) break;
    CldGetTokenFromRankAt(&msgs[count], rank, tok->pred);
    if (msgs[count] != 0) {
	ifn = (CldInfoFn)CmiHandlerToFunction(CmiGetInfo(msgs[count]));
	ifn(msgs[count], &pfn, &len, &queueing, &priobits, &prioptr);
	msgSizes[count] = len;
	CldSwitchHandler(msgs[count], CpvAccessOther(CldBalanceHandlerIndex, rank));
        if (immed) CmiBecomeImmediate(msgs[count]);
        count ++;
    }
    tok = tok->succ;
  }
  if (count > 1) {
      if (immed)
        CmiMultipleIsend(pe, count, msgSizes, msgs);
      else
        CmiMultipleSend(pe, count, msgSizes, msgs);
      for (i=0; i<count; i++)
	CmiFree(msgs[i]);
      CpvAccessOther(CldRelocatedMessages, rank) += count;
      CpvAccessOther(CldMessageChunks, rank)++;
  }
  else if (count == 1) {
      if (immed) CmiBecomeImmediate(msgs[0]);
      CmiSyncSendAndFree(pe, msgSizes[0], msgs[0]);
      CpvAccessOther(CldRelocatedMessages, rank)++;
      CpvAccessOther(CldMessageChunks, rank)++;
  }
  free(msgs);
  free(msgSizes);
}

/* simple scheme - just send one by one. useful for multicore */
void CldSimpleMultipleSend(int pe, int numToSend, int rank)
{
  char *msg;
  int queueing, priobits, i, numSent, done=0;
  size_t len;
  unsigned int *prioptr;
  CldInfoFn ifn;
  CldPackFn pfn;

  if (numToSend == 0)
    return;

  numSent = 0;
  while (!done) {
    for (i=0; i<numToSend; i++) {
      CldGetTokenFromRank(&msg, rank);
      if (msg != 0) {
	done = 1;
	numToSend--;
	ifn = (CldInfoFn)CmiHandlerToFunction(CmiGetInfo(msg));
	ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
	CldSwitchHandler(msg, CpvAccessOther(CldBalanceHandlerIndex, rank));
        CmiSyncSendAndFree(pe, len, msg);
        if (numToSend == 0) done = 1;
      }
      else {
	done = 1;
	break;
      }
    }
  }
}

void seedBalancerExit(void)
{
  if (_cldb_cs)
    CmiPrintf("[%d] Relocate message number is %d\n", CmiMyPe(), CpvAccess(CldRelocatedMessages));
}
