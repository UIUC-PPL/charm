/**
\addtogroup Ck

These routines implement a basic remote-method-invocation system
consisting of chares and groups.  There is no migration. All
the bindings are written to the C language, although most
clients, including the rest of Charm++, are actually C++.
*/
#include "ck.h"
#include "trace.h"
#include "queueing.h"

#include "pathHistory.h"

#if CMK_LBDB_ON
#include "LBManager.h"
#endif // CMK_LBDB_ON

#ifndef CMK_CHARE_USE_PTR
#include <map>
CkpvDeclare(std::vector<void *>, chare_objs);
CkpvDeclare(std::vector<int>, chare_types);
CkpvDeclare(std::vector<VidBlock *>, vidblocks);
CksvExtern(ObjNumRdmaOpsMap, pendingZCOps);

typedef std::map<int, CkChareID>  Vidblockmap;
CkpvDeclare(Vidblockmap, vmap);      // remote VidBlock to notify upon deletion
CkpvDeclare(int, currentChareIdx);
#endif

// Map of array IDs to array elements for fast message delivery
CkpvDeclare(ArrayObjMap, array_objs);

#define CK_MSG_SKIP_OR_IMM    (CK_MSG_EXPEDITED | CK_MSG_IMMEDIATE)

VidBlock::VidBlock() { state = UNFILLED; msgQ = new PtrQ(); _MEMCHECK(msgQ); }

int CMessage_CkMessage::__idx=-1;
int CMessage_CkArgMsg::__idx=0;
int CkIndex_Chare::__idx;
int CkIndex_Group::__idx;
int CkIndex_ArrayBase::__idx=-1;

extern int _defaultObjectQ;

void _initChareTables()
{
#ifndef CMK_CHARE_USE_PTR
          /* chare and vidblock table */
  CkpvInitialize(std::vector<void *>, chare_objs);
  CkpvInitialize(std::vector<int>, chare_types);
  CkpvInitialize(std::vector<VidBlock *>, vidblocks);
  CkpvInitialize(Vidblockmap, vmap);
  CkpvInitialize(int, currentChareIdx);
  CkpvAccess(currentChareIdx) = -1;
#endif

  CkpvInitialize(ArrayObjMap, array_objs);
}

//Charm++ virtual functions: declaring these here results in a smaller executable
Chare::Chare(void) {
  thishandle.onPE=CkMyPe();
  thishandle.objPtr=this;
#if CMK_ERROR_CHECKING
  magic = CHARE_MAGIC;
#endif
#ifndef CMK_CHARE_USE_PTR
     // for plain chare, objPtr is actually the index to chare obj table
  if (CkpvAccess(currentChareIdx) >= 0) {
    thishandle.objPtr=(void*)(CmiIntPtr)CkpvAccess(currentChareIdx);
  }
  chareIdx = CkpvAccess(currentChareIdx);
#endif
#if CMK_OBJECT_QUEUE_AVAILABLE
  if (_defaultObjectQ)  CkEnableObjQ();
#endif
}

Chare::Chare(CkMigrateMessage* m) {
  thishandle.onPE=CkMyPe();
  thishandle.objPtr=this;
#if CMK_ERROR_CHECKING
  magic = 0;
#endif


#if CMK_OBJECT_QUEUE_AVAILABLE
  if (_defaultObjectQ)  CkEnableObjQ();
#endif
}

void Chare::CkEnableObjQ()
{
#if CMK_OBJECT_QUEUE_AVAILABLE
  objQ.create();
#endif
}

Chare::~Chare() {
#ifndef CMK_CHARE_USE_PTR
/*
  if (chareIdx >= 0 && chareIdx < CpvAccess(chare_objs).size() && CpvAccess(chare_objs)[chareIdx] == this) 
*/
  if (chareIdx != -1)
  {
    CmiAssert(CkpvAccess(chare_objs)[chareIdx] == this);
    CkpvAccess(chare_objs)[chareIdx] = NULL;
    Vidblockmap::iterator iter = CkpvAccess(vmap).find(chareIdx);
    if (iter != CkpvAccess(vmap).end()) {
      CkChareID *pCid = (CkChareID *)
        _allocMsg(DeleteVidMsg, sizeof(CkChareID));
      int srcPe = iter->second.onPE;
      *pCid = iter->second;
      envelope *ret = UsrToEnv(pCid);
      ret->setVidPtr(iter->second.objPtr);
      ret->setSrcPe(CkMyPe());
      CmiSetHandler(ret, _charmHandlerIdx);
      CmiSyncSendAndFree(srcPe, ret->getTotalsize(), (char *)ret);
      CpvAccess(_qd)->create();
      CkpvAccess(vmap).erase(iter);
    }
  }
#endif
}

void Chare::pup(PUP::er &p)
{
  p(thishandle.onPE);
  thishandle.objPtr=(void *)this;
#ifndef CMK_CHARE_USE_PTR
  p(chareIdx);
  if (chareIdx != -1) thishandle.objPtr=(void*)(CmiIntPtr)chareIdx;
#endif
#if CMK_ERROR_CHECKING
  p(magic);
#endif
}

int Chare::ckGetChareType() const {
  return -3;
}
char *Chare::ckDebugChareName(void) {
  char buf[100];
  sprintf(buf,"Chare on pe %d at %p",CkMyPe(),(void*)this);
  return strdup(buf);
}
int Chare::ckDebugChareID(char *str, int limit) {
  // pure chares for now do not have a valid ID
  str[0] = 0;
  return 1;
}
void Chare::ckDebugPup(PUP::er &p) {
  pup(p);
}

/// This method is called before starting a [threaded] entry method.
void Chare::CkAddThreadListeners(CthThread th, void *msg) {
  CthSetThreadID(th, thishandle.onPE, (int)(((char *)thishandle.objPtr)-(char *)0), 0);
  traceAddThreadListeners(th, UsrToEnv(msg));
}

void CkMessage::ckDebugPup(PUP::er &p,void *msg) {
  p.comment("Bytes");
  int ts=UsrToEnv(msg)->getTotalsize();
  int msgLen=ts-sizeof(envelope);
  if (msgLen>0)
    p((char*)msg,msgLen);
}

IrrGroup::IrrGroup(void) {
  thisgroup = CkpvAccess(_currentGroup);
}

IrrGroup::~IrrGroup() {
  // remove the object pointer
  if (CkpvAccess(_destroyingNodeGroup)) {
    CmiImmediateLock(CksvAccess(_nodeGroupTableImmLock));
    CksvAccess(_nodeGroupTable)->find(thisgroup).setObj(NULL);
    CmiImmediateUnlock(CksvAccess(_nodeGroupTableImmLock));
    CkpvAccess(_destroyingNodeGroup) = false;
  } else {
    CmiImmediateLock(CkpvAccess(_groupTableImmLock));
    CkpvAccess(_groupTable)->find(thisgroup).setObj(NULL);
    CmiImmediateUnlock(CkpvAccess(_groupTableImmLock));
  }
}

void IrrGroup::pup(PUP::er &p)
{
  Chare::pup(p);
  p|thisgroup;
}

int IrrGroup::ckGetChareType() const {
  return CkpvAccess(_groupTable)->find(thisgroup).getcIdx();
}

int IrrGroup::ckDebugChareID(char *str, int limit) {
  if (limit<5) return -1;
  str[0] = 1;
  *((int*)&str[1]) = thisgroup.idx;
  return 5;
}

char *IrrGroup::ckDebugChareName() {
  return strdup(_chareTable[ckGetChareType()]->name);
}

void IrrGroup::ckJustMigrated(void)
{
}

void IrrGroup::CkAddThreadListeners(CthThread tid, void *msg) {
  /* FIXME: **CW** not entirely sure what we should do here yet */
}

void Group::CkAddThreadListeners(CthThread th, void *msg) {
  Chare::CkAddThreadListeners(th, msg);
  CthSetThreadID(th, thisgroup.idx, 0, 0);
}

void Group::pup(PUP::er &p)
{
  CkReductionMgr::pup(p);
  p|reductionInfo;
}

/**** Delegation Manager Group */
CkDelegateMgr::~CkDelegateMgr() { }

//Default delegator implementation: do not delegate-- send directly
void CkDelegateMgr::ChareSend(CkDelegateData *pd,int ep,void *m,const CkChareID *c,int onPE)
  { CkSendMsg(ep,m,c); }
void CkDelegateMgr::GroupSend(CkDelegateData *pd,int ep,void *m,int onPE,CkGroupID g)
  { CkSendMsgBranch(ep,m,onPE,g); }
void CkDelegateMgr::GroupBroadcast(CkDelegateData *pd,int ep,void *m,CkGroupID g)
  { CkBroadcastMsgBranch(ep,m,g); }
void CkDelegateMgr::GroupSectionSend(CkDelegateData *pd,int ep,void *m,int nsid,CkSectionID *s)
  { CkSendMsgBranchMulti(ep,m,s->_cookie.get_aid(),s->pelist.size(),s->pelist.data()); }
void CkDelegateMgr::NodeGroupSend(CkDelegateData *pd,int ep,void *m,int onNode,CkNodeGroupID g)
  { CkSendMsgNodeBranch(ep,m,onNode,g); }
void CkDelegateMgr::NodeGroupBroadcast(CkDelegateData *pd,int ep,void *m,CkNodeGroupID g)
  { CkBroadcastMsgNodeBranch(ep,m,g); }
void CkDelegateMgr::NodeGroupSectionSend(CkDelegateData *pd,int ep,void *m,int nsid,CkSectionID *s)
  { CkSendMsgNodeBranchMulti(ep,m,s->_cookie.get_aid(),s->pelist.size(),s->pelist.data()); }
void CkDelegateMgr::ArrayCreate(CkDelegateData *pd,int ep,void *m,const CkArrayIndex &idx,int onPE,CkArrayID a)
{
	CProxyElement_ArrayBase ap(a,idx);
	ap.ckInsert((CkArrayMessage *)m,ep,onPE);
}
void CkDelegateMgr::ArraySend(CkDelegateData *pd,int ep,void *m,const CkArrayIndex &idx,CkArrayID a)
{
	CProxyElement_ArrayBase ap(a,idx);
	ap.ckSend((CkArrayMessage *)m,ep);
}
void CkDelegateMgr::ArrayBroadcast(CkDelegateData *pd,int ep,void *m,CkArrayID a)
{
	CProxy_ArrayBase ap(a);
	ap.ckBroadcast((CkArrayMessage *)m,ep);
}

void CkDelegateMgr::ArraySectionSend(CkDelegateData *pd,int ep,void *m, int nsid,CkSectionID *s, int opts)
{
	CmiAbort("ArraySectionSend is not implemented!\n");
/*
	CProxyElement_ArrayBase ap(a,idx);
	ap.ckSend((CkArrayMessage *)m,ep);
*/
}

/*** Proxy <-> delegator communication */
CkDelegateData::~CkDelegateData() {}

CkDelegateData *CkDelegateMgr::DelegatePointerPup(PUP::er &p,CkDelegateData *pd) {
  return pd; // default implementation ignores pup call
}

/** FIXME: make a "CkReferenceHandle<CkDelegateData>" class to avoid
   this tricky manual reference counting business... */

void CProxy::ckDelegate(CkDelegateMgr *dTo,CkDelegateData *dPtr) {
 	if (dPtr) dPtr->ref();
	ckUndelegate();
	delegatedMgr = dTo;
	delegatedPtr = dPtr;
        delegatedGroupId = delegatedMgr->CkGetGroupID();
        isNodeGroup = delegatedMgr->isNodeGroup();
}
void CProxy::ckUndelegate(void) {
	delegatedMgr=NULL;
        delegatedGroupId.setZero();
	if (delegatedPtr) delegatedPtr->unref();
	delegatedPtr=NULL;
}

/// Copy constructor
CProxy::CProxy(const CProxy &src)
  :delegatedMgr(src.delegatedMgr), delegatedGroupId(src.delegatedGroupId), 
   isNodeGroup(src.isNodeGroup) {
    delegatedPtr = NULL;
    if(delegatedMgr != NULL && src.delegatedPtr != NULL) {
        delegatedPtr = src.delegatedMgr->ckCopyDelegateData(src.delegatedPtr);
    }
}

/// Assignment operator
CProxy& CProxy::operator=(const CProxy &src) {
	CkDelegateData *oldPtr=delegatedPtr;
	ckUndelegate();
	delegatedMgr=src.delegatedMgr;
        delegatedGroupId = src.delegatedGroupId; 
        isNodeGroup = src.isNodeGroup;

        if(delegatedMgr != NULL && src.delegatedPtr != NULL)
            delegatedPtr = delegatedMgr->ckCopyDelegateData(src.delegatedPtr);
        else
            delegatedPtr = NULL;

        // subtle: do unref *after* ref, because it's possible oldPtr == delegatedPtr
	if (oldPtr) oldPtr->unref();
	return *this;
}

void CProxy::pup(PUP::er &p) {
  if (!p.isUnpacking()) {
    if (ckDelegatedTo() != NULL) {
      delegatedGroupId = delegatedMgr->CkGetGroupID();
      isNodeGroup = delegatedMgr->isNodeGroup();
    }
  }
  p|delegatedGroupId;
  if (!delegatedGroupId.isZero()) {
    p|isNodeGroup;
    if (p.isUnpacking()) {
      delegatedMgr = ckDelegatedTo(); 
    }

    int migCtor = 0, cIdx; 
    if (!p.isUnpacking()) {
      if (isNodeGroup) {
        CmiImmediateLock(CksvAccess(_nodeGroupTableImmLock));
        cIdx = CksvAccess(_nodeGroupTable)->find(delegatedGroupId).getcIdx(); 
        migCtor = _chareTable[cIdx]->migCtor; 
        CmiImmediateUnlock(CksvAccess(_nodeGroupTableImmLock));
      }
      else  {
        CmiImmediateLock(CkpvAccess(_groupTableImmLock));
        cIdx = CkpvAccess(_groupTable)->find(delegatedGroupId).getcIdx();
        migCtor = _chareTable[cIdx]->migCtor; 
        CmiImmediateUnlock(CkpvAccess(_groupTableImmLock));
      }         
    }

    p|migCtor;

    // if delegated manager has not been created, construct a dummy
    // object on which to call DelegatePointerPup
    if (delegatedMgr == NULL) {

      // create a dummy object for calling DelegatePointerPup
      int objId = _entryTable[migCtor]->chareIdx; 
      size_t objSize = _chareTable[objId]->size;
      void *obj = malloc(objSize); 
      _entryTable[migCtor]->call(NULL, obj); 
      delegatedPtr = static_cast<CkDelegateMgr *> (obj)
        ->DelegatePointerPup(p, delegatedPtr);           
      free(obj);

    }
    else {

      // delegated manager has been created, so we can use it
      delegatedPtr = delegatedMgr->DelegatePointerPup(p,delegatedPtr);

    }

    if (p.isUnpacking() && delegatedPtr) {
      delegatedPtr->ref();
    }
  }
}

/**** Array sections */
#define CKSECTIONID_CONSTRUCTOR_DEF(index) \
CkSectionID::CkSectionID(const CkArrayID &aid, const CkArrayIndex##index *elems, const int nElems, int factor): bfactor(factor) { \
  _elems.assign(elems, elems+nElems);  \
  _cookie.get_aid() = aid;	\
  _cookie.get_pe() = CkMyPe();	\
} \
CkSectionID::CkSectionID(const CkArrayID &aid, const std::vector<CkArrayIndex##index> &elems, int factor): bfactor(factor) { \
  _elems.resize(elems.size()); \
  for (int i=0; i<_elems.size(); ++i) { \
    _elems[i] = static_cast<CkArrayIndex>(elems[i]); \
  } \
  _cookie.get_aid() = aid;	\
  _cookie.get_pe() = CkMyPe();	\
} \

CKSECTIONID_CONSTRUCTOR_DEF(1D)
CKSECTIONID_CONSTRUCTOR_DEF(2D)
CKSECTIONID_CONSTRUCTOR_DEF(3D)
CKSECTIONID_CONSTRUCTOR_DEF(4D)
CKSECTIONID_CONSTRUCTOR_DEF(5D)
CKSECTIONID_CONSTRUCTOR_DEF(6D)
CKSECTIONID_CONSTRUCTOR_DEF(Max)

CkSectionID::CkSectionID(const CkGroupID &gid, const int *_pelist, const int _npes, int factor): bfactor(factor) {
  _cookie.get_aid() = gid;
  pelist.assign(_pelist, _pelist+_npes);
}

CkSectionID::CkSectionID(const CkGroupID &gid, const std::vector<int>& _pelist, int factor): pelist(_pelist), bfactor(factor) {
  _cookie.get_aid() = gid;
}

CkSectionID::CkSectionID(const CkSectionID &sid) {
  _cookie = sid._cookie;
  pelist = sid.pelist;
  _elems = sid._elems;
  bfactor = sid.bfactor;
}

void CkSectionID::operator=(const CkSectionID &sid) {
  _cookie = sid._cookie;
  pelist = sid.pelist;
  _elems = sid._elems;
  bfactor = sid.bfactor;
}

void CkSectionID::pup(PUP::er &p) {
  p | _cookie;
  p | pelist;
  p | _elems;
  p | bfactor;
}

/**** Tiny random API routines */

#if CMK_CUDA
void CUDACallbackManager(void *fn, void *msg) {
  if (fn) {
    ((CkCallback*)fn)->send(msg);
  }
}
#endif

void QdCreate(int n) {
  CpvAccess(_qd)->create(n);
}

void QdProcess(int n) {
  CpvAccess(_qd)->process(n);
}

void CkSetRefNum(void *msg, CMK_REFNUM_TYPE ref)
{
  UsrToEnv(msg)->setRef(ref);
}

CMK_REFNUM_TYPE CkGetRefNum(void *msg)
{
  return UsrToEnv(msg)->getRef();
}

int CkGetSrcPe(void *msg)
{
  return UsrToEnv(msg)->getSrcPe();
}

int CkGetSrcNode(void *msg)
{
  return CmiNodeOf(CkGetSrcPe(msg));
}

void *CkLocalBranch(CkGroupID gID) {
  return _localBranch(gID);
}

// Similar to CkLocalBranch, but should be used from non-PE-local, but node-local PE
// Ensure thread safety while using this function as it is accessing a non-PE-local group
void *CkLocalBranchOther(CkGroupID gID, int rank) {
  return _localBranchOther(gID, rank);
}
static inline void _processBocBcastMsg(CkCoreState* ck, envelope* env);

static
void *_ckLocalNodeBranch(CkGroupID groupID) {
  CmiImmediateLock(CksvAccess(_nodeGroupTableImmLock));
  auto &entry = CksvAccess(_nodeGroupTable)->find(groupID);
  void *retval = entry.isReady() ? entry.getObj() : nullptr;
  CmiImmediateUnlock(CksvAccess(_nodeGroupTableImmLock));
  return retval;
}

void *CkLocalNodeBranch(CkGroupID groupID)
{
  void *retval;
  // we are called in a constructor
  if (CkpvAccess(_currentNodeGroupObj) && CkpvAccess(_currentGroup) == groupID)
    return CkpvAccess(_currentNodeGroupObj);
  while (NULL== (retval=_ckLocalNodeBranch(groupID)))
  { // Nodegroup hasn't finished being created yet-- schedule...
    CsdScheduler(0);
  }
  return retval;
}

void *CkLocalChare(const CkChareID *pCid)
{
	int pe=pCid->onPE;
	if (pe<0) { //A virtual chare ID
		if (pe!=(-(CkMyPe()+1)))
			return NULL;//VID block not on this PE
#ifdef CMK_CHARE_USE_PTR
		VidBlock *v=(VidBlock *)pCid->objPtr;
#else
		VidBlock *v=CkpvAccess(vidblocks)[(CmiIntPtr)pCid->objPtr];
#endif
		return v->getLocalChareObj();
	}
	else
	{ //An ordinary chare ID
		if (pe!=CkMyPe())
			return NULL;//Chare not on this PE
#ifdef CMK_CHARE_USE_PTR
		return pCid->objPtr;
#else
		return CkpvAccess(chare_objs)[(CmiIntPtr)pCid->objPtr];
#endif
	}
}

CkpvDeclare(char**,Ck_argv);

char **CkGetArgv(void) {
	return CkpvAccess(Ck_argv);
}
int CkGetArgc(void) {
	return CmiGetArgc(CkpvAccess(Ck_argv));
}

/******************** Basic support *****************/
void CkDeliverMessageFree(int epIdx,void *msg,void *obj)
{
#if CMK_CHARMDEBUG
  CpdBeforeEp(epIdx, obj, msg);
#endif    
  const auto msgtype = (msg == NULL) ? LAST_CK_ENVELOPE_TYPE : UsrToEnv(msg)->getMsgtype();
  _entryTable[epIdx]->call(msg, obj);
#if CMK_CHARMDEBUG
  CpdAfterEp(epIdx);
#endif
  if (_entryTable[epIdx]->noKeep && msgtype != LAST_CK_ENVELOPE_TYPE)
  { /* Method doesn't keep/delete the message, so we have to: */
    if (msgtype != ArrayBcastFwdMsg) {
      _msgTable[_entryTable[epIdx]->msgIdx]->dealloc(msg);
    } else if (_entryTable[epIdx]->msgIdx != -1) {
      // For array broadcasts, the first time they show up here the msgIdx is
      // -1, and they are just messages for CkArray::recvBroadcastNoKeep, so we
      // don't want to delete them yet.
      _msgTable[_entryTable[epIdx]->msgIdx]->dealloc(msg);
    }
  }
}
void CkDeliverMessageReadonly(int epIdx,const void *msg,void *obj)
{
  void *deliverMsg;
  if (_entryTable[epIdx]->noKeep)
  { /* Deliver a read-only copy of the message */
    deliverMsg=(void *)msg;
  } else
  { /* Method needs a copy of the message to keep/delete */
    void *oldMsg=(void *)msg;
    deliverMsg=CkCopyMsg(&oldMsg);
#if CMK_ERROR_CHECKING
    if (oldMsg!=msg)
      CkAbort("CkDeliverMessageReadonly: message pack/unpack changed message pointer!");
#endif
  }
#if CMK_CHARMDEBUG
  CpdBeforeEp(epIdx, obj, (void*)msg);
#endif
  _entryTable[epIdx]->call(deliverMsg, obj);
#if CMK_CHARMDEBUG
  CpdAfterEp(epIdx);
#endif
}

static inline void _invokeEntryNoTrace(int epIdx,envelope *env,void *obj)
{
  void *msg = EnvToUsr(env);
  _SET_USED(env, 0);
  if(CMI_ZC_MSGTYPE(UsrToEnv(msg)) == CMK_ZC_P2P_RECV_MSG ||
     CMI_ZC_MSGTYPE(UsrToEnv(msg)) == CMK_ZC_BCAST_RECV_MSG ||
     CMI_ZC_MSGTYPE(UsrToEnv(msg)) == CMK_ZC_BCAST_RECV_DONE_MSG)
    CkDeliverMessageReadonly(epIdx,msg,obj); // Do not free a P2P_RECV_MSG or BCAST_RECV_MSG or a BCAST_RECV_DONE_MSG
  else
    CkDeliverMessageFree(epIdx,msg,obj);
}

static inline void _invokeEntry(int epIdx,envelope *env,void *obj)
{

#if CMK_TRACE_ENABLED 
  if (_entryTable[epIdx]->traceEnabled) {
    _TRACE_BEGIN_EXECUTE(env, obj);
    if(_entryTable[epIdx]->appWork)
        _TRACE_BEGIN_APPWORK();
    _invokeEntryNoTrace(epIdx,env,obj);
    if(_entryTable[epIdx]->appWork)
        _TRACE_END_APPWORK();
    _TRACE_END_EXECUTE();
  }
  else
#endif
    _invokeEntryNoTrace(epIdx,env,obj);

}

/********************* Creation ********************/

void CkCreateChare(int cIdx, int eIdx, void *msg, CkChareID *pCid, int destPE)
{
  CkAssert(cIdx == _entryTable[eIdx]->chareIdx);
  envelope *env = UsrToEnv(msg);
  _CHECK_USED(env);
  if(pCid == 0) {
    env->setMsgtype(NewChareMsg);
  } else {
    pCid->onPE = (-(CkMyPe()+1));
    //  pCid->magic = _GETIDX(cIdx);
    pCid->objPtr = (void *) new VidBlock();
    _MEMCHECK(pCid->objPtr);
    env->setMsgtype(NewVChareMsg);
    env->setVidPtr(pCid->objPtr);
#ifndef CMK_CHARE_USE_PTR
    CkpvAccess(vidblocks).push_back((VidBlock*)pCid->objPtr);
    int idx = CkpvAccess(vidblocks).size()-1;
    pCid->objPtr = (void *)(CmiIntPtr)idx;
    env->setVidPtr((void *)(CmiIntPtr)idx);
#endif
  }
  env->setEpIdx(eIdx);
  env->setByPe(CkMyPe());
  env->setSrcPe(CkMyPe());
  CmiSetHandler(env, _charmHandlerIdx);
  _TRACE_CREATION_1(env);
  CpvAccess(_qd)->create();
  _STATS_RECORD_CREATE_CHARE_1();
  _SET_USED(env, 1);
  if(destPE == CK_PE_ANY)
    env->setForAnyPE(1);
  else
    env->setForAnyPE(0);
  _CldEnqueue(destPE, env, _infoIdx);
  _TRACE_CREATION_DONE(1);
}

inline void CkReadyEntry(TableEntry &entry, bool nodeLevel) {
  // the ready flag is set first to expedite the unblocking of
  // node-level peers (that depend on the current [node]group)
  entry.setReady();

  auto *ptrq = entry.getPending();
  if (ptrq) {
    void *pending = nullptr;
    while (nullptr != (pending = ptrq->deq())) {
      if (nodeLevel) {
        _CldNodeEnqueue(CkMyNode(), pending, _infoIdx);
      } else {
        CsdEnqueueGeneral(pending, CQS_QUEUEING_FIFO, 0, 0);
      }
    }
    entry.clearPending();
  }
}

void CkCreateLocalGroup(CkGroupID groupID, int epIdx, envelope *env)
{
  int gIdx = _entryTable[epIdx]->chareIdx;
  void *obj = malloc(_chareTable[gIdx]->size);
  _MEMCHECK(obj);
  setMemoryTypeChare(obj);

  // this enables groups to access themselves via
  // ckLocalBranch during their construction (nodegroups
  // use _currentNodeGroupObj to achieve this)
  CmiImmediateLock(CkpvAccess(_groupTableImmLock));
  CkpvAccess(_groupTable)->find(groupID).setObj(obj);
  CkpvAccess(_groupTable)->find(groupID).setcIdx(gIdx);
  CkpvAccess(_groupIDTable)->push_back(groupID);
  CmiImmediateUnlock(CkpvAccess(_groupTableImmLock));

  CkpvAccess(_currentGroup) = groupID;
  CkpvAccess(_currentGroupRednMgr) = env->getRednMgr();

#ifndef CMK_CHARE_USE_PTR
  int callingChareIdx = CkpvAccess(currentChareIdx);
  CkpvAccess(currentChareIdx) = -1;
#endif

  _invokeEntryNoTrace(epIdx,env,obj); /* can't trace groups: would cause nested begin's */

#ifndef CMK_CHARE_USE_PTR
  CkpvAccess(currentChareIdx) = callingChareIdx;
#endif

  // this enables other PEs to access this object via
  // ckLocalBranchOther, and schedules pending dependencies
  CmiImmediateLock(CkpvAccess(_groupTableImmLock));
  CkReadyEntry(CkpvAccess(_groupTable)->find(groupID), false);
  CmiImmediateUnlock(CkpvAccess(_groupTableImmLock));

  _STATS_RECORD_PROCESS_GROUP_1();
}

void CkCreateLocalNodeGroup(CkGroupID groupID, int epIdx, envelope *env)
{
  int gIdx = _entryTable[epIdx]->chareIdx;
  size_t objSize=_chareTable[gIdx]->size;
  void *obj = malloc(objSize);
  _MEMCHECK(obj);
  setMemoryTypeChare(obj);

  CkpvAccess(_currentGroup) = groupID;

// Now that the NodeGroup is created, add it to the table.
//  NodeGroups can be accessed by multiple processors, so
//  this is in the opposite order from groups - invoking the constructor
//  before registering it.
// User may call CkLocalNodeBranch() inside the nodegroup constructor
//  store nodegroup into _currentNodeGroupObj
  CkpvAccess(_currentNodeGroupObj) = obj;

#ifndef CMK_CHARE_USE_PTR
  int callingChareIdx = CkpvAccess(currentChareIdx);
  CkpvAccess(currentChareIdx) = -1;
#endif

  _invokeEntryNoTrace(epIdx,env,obj);

#ifndef CMK_CHARE_USE_PTR
  CkpvAccess(currentChareIdx) = callingChareIdx;
#endif

  CkpvAccess(_currentNodeGroupObj) = NULL;

  // nodegroups use `_currentNodeGroupObj` so they do not
  // need a table entry for objects to access themselves
  // during creation. therefore, everything can be
  // consolidated into one critical section
  CmiImmediateLock(CksvAccess(_nodeGroupTableImmLock));
  CksvAccess(_nodeGroupTable)->find(groupID).setObj(obj);
  CksvAccess(_nodeGroupTable)->find(groupID).setcIdx(gIdx);
  CksvAccess(_nodeGroupIDTable).push_back(groupID);
  CkReadyEntry(CksvAccess(_nodeGroupTable)->find(groupID), true);
  CmiImmediateUnlock(CksvAccess(_nodeGroupTableImmLock));

  _STATS_RECORD_PROCESS_NODE_GROUP_1();
}

void _createGroup(CkGroupID groupID, envelope *env)
{
  _CHECK_USED(env);
  _SET_USED(env, 1);
  int epIdx = env->getEpIdx();
  int gIdx = _entryTable[epIdx]->chareIdx;
  env->setGroupNum(groupID);
  env->setSrcPe(CkMyPe());
  env->setGroupEpoch(CkpvAccess(_charmEpoch));

  if(CkNumPes()>1) {
    CkPackMessage(&env);
    CmiSetHandler(env, _bocHandlerIdx);
    _numInitMsgs++;
    CmiSyncBroadcast(env->getTotalsize(), (char *)env);
    CpvAccess(_qd)->create(CkNumPes()-1);
    CkUnpackMessage(&env);
  }
  _STATS_RECORD_CREATE_GROUP_1();
  CkCreateLocalGroup(groupID, epIdx, env);
}

void _createNodeGroup(CkGroupID groupID, envelope *env)
{
  _CHECK_USED(env);
  _SET_USED(env, 1);
  int epIdx = env->getEpIdx();
  env->setGroupNum(groupID);
  env->setSrcPe(CkMyPe());
  env->setGroupEpoch(CkpvAccess(_charmEpoch));
  if(CkNumNodes()>1) {
    CkPackMessage(&env);
    CmiSetHandler(env, _bocHandlerIdx);
    _numInitMsgs++;
    if (CkpvAccess(_charmEpoch)==0) CksvAccess(_numInitNodeMsgs)++;
    CmiSyncNodeBroadcast(env->getTotalsize(), (char *)env);
    CpvAccess(_qd)->create(CkNumNodes()-1);
    CkUnpackMessage(&env);
  }
  _STATS_RECORD_CREATE_NODE_GROUP_1();
  CkCreateLocalNodeGroup(groupID, epIdx, env);
}

// new _groupCreate

static CkGroupID _groupCreate(envelope *env)
{
  CkGroupID groupNum;

  // check CkMyPe(). if it is 0 then idx is _numGroups++
  // if not, then something else...
  if(CkMyPe() == 0)
     groupNum.idx = CkpvAccess(_numGroups)++;
  else
     groupNum.idx = _getGroupIdx(CkNumPes(),CkMyPe(),CkpvAccess(_numGroups)++);
  _createGroup(groupNum, env);
  return groupNum;
}

// new _nodeGroupCreate
static CkGroupID _nodeGroupCreate(envelope *env)
{
  CkGroupID groupNum;
  CmiImmediateLock(CksvAccess(_nodeGroupTableImmLock));                // change for proc 0 and other processors
  if(CkMyNode() == 0)				// should this be CkMyPe() or CkMyNode()?
          groupNum.idx = CksvAccess(_numNodeGroups)++;
   else
          groupNum.idx = _getGroupIdx(CkNumNodes(),CkMyNode(),CksvAccess(_numNodeGroups)++);
  CmiImmediateUnlock(CksvAccess(_nodeGroupTableImmLock));
  _createNodeGroup(groupNum, env);
  return groupNum;
}

/**** generate the group idx when group is creator pe is not pe0
 **** the 32 bit index has msb set to 1 (+ve indices are used by proc 0)
 **** remaining bits contain the group creator processor number and
 **** the idx number which starts from 1(_numGroups or _numNodeGroups) on each proc ****/

int _getGroupIdx(int numNodes,int myNode,int numGroups)
{
        int idx;
        int x = (int)ceil(log((double)numNodes)/log((double)2));// number of bits needed to store node number
        int n = 32 - (x+1);                                     // number of bits remaining for the index
        idx = (myNode<<n) + numGroups;                          // add number of processors, shift by the no. of bits needed,
                                                                // then add the next available index
	// of course this won't work when int is 8 bytes long on T3E
        //idx |= 0x80000000;                                      // set the most significant bit to 1
	idx = - idx;
								// if int is not 32 bits, wouldn't this be wrong?
        return idx;
}

CkGroupID CkCreateGroup(int cIdx, int eIdx, void *msg)
{
  CkAssert(cIdx == _entryTable[eIdx]->chareIdx);
  envelope *env = UsrToEnv(msg);
  env->setMsgtype(BocInitMsg);
  env->setEpIdx(eIdx);
  env->setSrcPe(CkMyPe());
  _TRACE_CREATION_N(env, CkNumPes());
  CkGroupID gid = _groupCreate(env);
  _TRACE_CREATION_DONE(1);
  return gid;
}

CkGroupID CkCreateNodeGroup(int cIdx, int eIdx, void *msg)
{
  CkAssert(cIdx == _entryTable[eIdx]->chareIdx);
  envelope *env = UsrToEnv(msg);
  env->setMsgtype(NodeBocInitMsg);
  env->setEpIdx(eIdx);
  env->setSrcPe(CkMyPe());
  _TRACE_CREATION_N(env, CkNumNodes());
  CkGroupID gid = _nodeGroupCreate(env);
  _TRACE_CREATION_DONE(1);
  return gid;
}

static inline void *_allocNewChare(envelope *env, int &idx)
{
  int chareIdx = _entryTable[env->getEpIdx()]->chareIdx;
  void *tmp=malloc(_chareTable[chareIdx]->size);
  _MEMCHECK(tmp);
#ifndef CMK_CHARE_USE_PTR
  CkpvAccess(chare_objs).push_back(tmp);
  CkpvAccess(chare_types).push_back(chareIdx);
  idx = CkpvAccess(chare_objs).size()-1;
#endif
  setMemoryTypeChare(tmp);
  return tmp;
}

// Method returns true if one or more group dependencies are unsatisfied
inline bool isGroupDepUnsatisfied(const CkCoreState *ck, const envelope *env) {
  int groupDepNum = env->getGroupDepNum();
  if(groupDepNum != 0) {
    CkGroupID *groupDepPtr = (CkGroupID *)(env->getGroupDepPtr());
    for(int i=0;i<groupDepNum;i++) {
      CkGroupID depID = groupDepPtr[i];
      if (!depID.isZero() && !_lookupGroupAndBufferIfNotThere(ck, env, depID)) {
        return true;
      }
    }
  }
  return false;
}

static void _processNewChareMsg(CkCoreState *ck,envelope *env)
{
  if(isGroupDepUnsatisfied(ck, env))
    return;
  if(ck)
    ck->process(); // ck->process() updates mProcessed count used in QD
  int idx;
  void *obj = _allocNewChare(env, idx);
#ifndef CMK_CHARE_USE_PTR
  CkpvAccess(currentChareIdx) = idx;
#endif
  _invokeEntry(env->getEpIdx(),env,obj);
  if(ck)
    _STATS_RECORD_PROCESS_CHARE_1();
}

void CkCreateLocalChare(int epIdx, envelope *env)
{
  env->setEpIdx(epIdx);
  _processNewChareMsg(NULL, env);
}

static void _processNewVChareMsg(CkCoreState *ck,envelope *env)
{
  if(isGroupDepUnsatisfied(ck, env))
    return;
  ck->process(); // ck->process() updates mProcessed count used in QD
  int idx;
  void *obj = _allocNewChare(env, idx);
  CkChareID *pCid = (CkChareID *)
      _allocMsg(FillVidMsg, sizeof(CkChareID));
  pCid->onPE = CkMyPe();
#ifndef CMK_CHARE_USE_PTR
  pCid->objPtr = (void*)(CmiIntPtr)idx;
#else
  pCid->objPtr = obj;
#endif
  // pCid->magic = _GETIDX(_entryTable[env->getEpIdx()]->chareIdx);
  envelope *ret = UsrToEnv(pCid);
  ret->setVidPtr(env->getVidPtr());
  int srcPe = env->getByPe();
  ret->setSrcPe(CkMyPe());
  CmiSetHandler(ret, _charmHandlerIdx);
  CmiSyncSendAndFree(srcPe, ret->getTotalsize(), (char *)ret);
#ifndef CMK_CHARE_USE_PTR
  // register the remote vidblock for deletion when chare is deleted
  CkChareID vid;
  vid.onPE = srcPe;
  vid.objPtr = env->getVidPtr();
  CkpvAccess(vmap)[idx] = vid;    
#endif
  CpvAccess(_qd)->create();
#ifndef CMK_CHARE_USE_PTR
  CkpvAccess(currentChareIdx) = idx;
#endif
  _invokeEntry(env->getEpIdx(),env,obj);
  _STATS_RECORD_PROCESS_CHARE_1();
}

/************** Receive: Chares *************/

static inline void _processForPlainChareMsg(CkCoreState *ck,envelope *env)
{
  if(isGroupDepUnsatisfied(ck, env))
    return;
  ck->process(); // ck->process() updates mProcessed count used in QD
  int epIdx = env->getEpIdx();
  int mainIdx = _chareTable[_entryTable[epIdx]->chareIdx]->mainChareType();
  void *obj;
  if (mainIdx != -1)  {           // mainchare
    CmiAssert(CkMyPe()==0);
    obj = _mainTable[mainIdx]->getObj();
  }
  else {
#ifndef CMK_CHARE_USE_PTR
    if (_chareTable[_entryTable[epIdx]->chareIdx]->chareType == TypeChare)
      obj = CkpvAccess(chare_objs)[(CmiIntPtr)env->getObjPtr()];
    else
      obj = env->getObjPtr();
#else
    obj = env->getObjPtr();
#endif
  }
  _invokeEntry(epIdx,env,obj);
  _STATS_RECORD_PROCESS_MSG_1();
}

static inline void _processFillVidMsg(CkCoreState *ck,envelope *env)
{
  ck->process(); // ck->process() updates mProcessed count used in QD
#ifndef CMK_CHARE_USE_PTR
  VidBlock *vptr = CkpvAccess(vidblocks)[(CmiIntPtr)env->getVidPtr()];
#else
  VidBlock *vptr = (VidBlock *) env->getVidPtr();
  _CHECK_VALID(vptr, "FillVidMsg: Not a valid VIdPtr\n");
#endif
  CkChareID *pcid = (CkChareID *) EnvToUsr(env);
  _CHECK_VALID(pcid, "FillVidMsg: Not a valid pCid\n");
  if (vptr) vptr->fill(pcid->onPE, pcid->objPtr);
  CmiFree(env);
}

static inline void _processForVidMsg(CkCoreState *ck,envelope *env)
{
  ck->process(); // ck->process() updates mProcessed count used in QD
#ifndef CMK_CHARE_USE_PTR
  VidBlock *vptr = CkpvAccess(vidblocks)[(CmiIntPtr)env->getVidPtr()];
#else
  VidBlock *vptr = (VidBlock *) env->getVidPtr();
  _CHECK_VALID(vptr, "ForVidMsg: Not a valid VIdPtr\n");
#endif
  _SET_USED(env, 1);
  vptr->send(env);
}

static inline void _processDeleteVidMsg(CkCoreState *ck,envelope *env)
{
  ck->process(); // ck->process() updates mProcessed count used in QD
#ifndef CMK_CHARE_USE_PTR
  VidBlock *vptr = CkpvAccess(vidblocks)[(CmiIntPtr)env->getVidPtr()];
  delete vptr;
  CkpvAccess(vidblocks)[(CmiIntPtr)env->getVidPtr()] = NULL;
#endif
  CmiFree(env);
}

/************** Receive: Groups ****************/

/**
 Return a pointer to the local BOC of "groupID".
 The message "env" passed in has some known dependency on this groupID
 (either it is to be delivered to this BOC, or it depends on this BOC being there).
 Therefore, if the return value is NULL, this function buffers the message so that
 it will be re-sent (by CkCreateLocalBranch) when this groupID is eventually constructed.
 The message passed in must have its handlers correctly set so that it can be
 scheduled again.
*/
static inline IrrGroup *_lookupGroupAndBufferIfNotThere(const CkCoreState *ck, const envelope *env, const CkGroupID &groupID)
{

	CmiImmediateLock(CkpvAccess(_groupTableImmLock));
	IrrGroup *obj = ck->localBranch(groupID);
	if (obj==NULL) { /* groupmember not yet created: stash message */
		ck->getGroupTable()->find(groupID).enqMsg((envelope *)env);
	}
	CmiImmediateUnlock(CkpvAccess(_groupTableImmLock));
	return obj;
}

IrrGroup *lookupGroupAndBufferIfNotThere(CkCoreState *ck,envelope *env,const CkGroupID &groupID)
{
  return _lookupGroupAndBufferIfNotThere(ck, env, groupID);
}

static inline void _deliverForBocMsg(CkCoreState *ck,int epIdx,envelope *env,IrrGroup *obj)
{
#if CMK_LBDB_ON
  // if there is a running obj being measured, stop it temporarily
  LDObjHandle objHandle;
  int objstopped = 0;
  LBManager *the_lbmgr = (LBManager *)CkLocalBranch(_lbmgr);
  if (the_lbmgr->RunningObject(&objHandle)) {
    objstopped = 1;
    the_lbmgr->ObjectStop(objHandle);
  }
#endif

#if CMK_SMP
  unsigned short int msgType = CMI_ZC_MSGTYPE(env); // store msgType as msg could be freed
#endif

  _invokeEntry(epIdx,env,obj);

#if CMK_SMP
  if(msgType == CMK_ZC_BCAST_RECV_DONE_MSG) {
    updatePeerCounterAndPush(env);
  }
#endif

#if CMK_LBDB_ON
  if (objstopped) the_lbmgr->ObjectStart(objHandle);
#endif
  _STATS_RECORD_PROCESS_BRANCH_1();
}

static inline void _processForBocMsg(CkCoreState *ck,envelope *env)
{
  if(isGroupDepUnsatisfied(ck, env))
    return;
  CkGroupID groupID =  env->getGroupNum();
  IrrGroup *obj = _lookupGroupAndBufferIfNotThere(ck,env,env->getGroupNum());
  if(obj) {
    ck->process(); // ck->process() updates mProcessed count used in QD
    _deliverForBocMsg(ck,env->getEpIdx(),env,obj);
  }
}

IrrGroup* _getCkLocalBranchFromGroupID(CkGroupID &gID) {
  CkCoreState *ck = CkpvAccess(_coreState);
  CmiImmediateLock(CkpvAccess(_groupTableImmLock));
  IrrGroup *obj = ck->localBranch(gID);
  CmiImmediateUnlock(CkpvAccess(_groupTableImmLock));
  return obj;
}

static inline void _deliverForNodeBocMsg(CkCoreState *ck,int epIdx, envelope *env,void *obj)
{
  env->setEpIdx(epIdx);
  _invokeEntry(epIdx,env,obj);
  _STATS_RECORD_PROCESS_NODE_BRANCH_1();
}

static inline void _processForNodeBocMsg(CkCoreState *ck,envelope *env)
{
  if(isGroupDepUnsatisfied(ck, env))
    return;
  CkGroupID groupID = env->getGroupNum();
  void *obj;

  CmiImmediateLock(CksvAccess(_nodeGroupTableImmLock));
  obj = CksvAccess(_nodeGroupTable)->find(groupID).getObj();
  if(!obj) { // groupmember not yet created
#if CMK_IMMEDIATE_MSG
    if (CmiIsImmediate(env)) {
      //CmiDelayImmediate();        /* buffer immediate message */
      CmiResetImmediate(env);        // note: this may not be SIG IO safe !
    }
#endif
    CksvAccess(_nodeGroupTable)->find(groupID).enqMsg(env);
    CmiImmediateUnlock(CksvAccess(_nodeGroupTableImmLock));
    return;
  }
  CmiImmediateUnlock(CksvAccess(_nodeGroupTableImmLock));
  ck->process(); // ck->process() updates mProcessed count used in QD
  _invokeEntry(env->getEpIdx(),env,obj);
  _STATS_RECORD_PROCESS_NODE_BRANCH_1();
}

void _processBocInitMsg(CkCoreState *ck,envelope *env)
{
  if(isGroupDepUnsatisfied(ck, env))
    return;
  CkGroupID groupID = env->getGroupNum();
  int epIdx = env->getEpIdx();
  ck->process(); // ck->process() updates mProcessed count used in QD
  CkCreateLocalGroup(groupID, epIdx, env);
}

void _processNodeBocInitMsg(CkCoreState *ck,envelope *env)
{
  if(isGroupDepUnsatisfied(ck, env))
    return;
  ck->process(); // ck->process() updates mProcessed count used in QD
  CkGroupID groupID = env->getGroupNum();
  int epIdx = env->getEpIdx();
  CkCreateLocalNodeGroup(groupID, epIdx, env);
}

/************** Receive: Arrays *************/
static void _processArrayEltMsg(CkCoreState *ck,envelope *env) {
  ArrayObjMap& object_map = CkpvAccess(array_objs);
  auto iter = object_map.find(env->getRecipientID());
  if (iter != object_map.end()) {

    CkArrayMessage* msg = (CkArrayMessage*)EnvToUsr(env);

    CkLocMgr *localLocMgr  = CProxy_ArrayBase(env->getArrayMgr()).ckLocMgr();

    // Check if the array element has active Rgets (because of ZC Pup)
    auto iter2 = localLocMgr->bufferedActiveRgetMsgs.find(msg->array_element_id());

    if(iter2 != localLocMgr->bufferedActiveRgetMsgs.end()) {
      // array element has active rgets
      iter2->second.push_back(msg); // Buffer msg for now and handle it after rgets complete
      return;
    }

    // First see if we already have a direct pointer to the object
    _SET_USED(env, 0);
    ck->process(); // ck->process() updates mProcessed count used in QD
    int opts = 0;
    if (msg->array_hops()>1) {
      CProxy_ArrayBase(env->getArrayMgr()).ckLocMgr()->multiHop(msg);
    }
    bool doFree = true;
    if(CMI_ZC_MSGTYPE(env) == CMK_ZC_P2P_RECV_MSG) // Do not free a P2P_RECV_MSG
      doFree = false;
    iter->second->ckInvokeEntry(env->getEpIdx(), msg, doFree);
  } else {
    // Otherwise fallback to delivery through the array manager
    CkArray *mgr=(CkArray *)_lookupGroupAndBufferIfNotThere(ck,env,env->getArrayMgr());
    if (mgr) {
      _SET_USED(env, 0);
      ck->process(); // ck->process() updates mProcessed count used in QD
      mgr->deliver((CkArrayMessage *)EnvToUsr(env), CkDeliver_inline);
    }
  }
}

// Debugging support:
#define TELLMSGTYPE(x) //x

/**
 * This is the main converse-level handler used by all of Charm++.
 *
 * \addtogroup CriticalPathFramework
 */
void _processHandler(void *converseMsg,CkCoreState *ck)
{
  envelope *env = (envelope *) converseMsg;

  MESSAGE_PHASE_CHECK(env);

  if(CMI_ZC_MSGTYPE(env) == CMK_ZC_P2P_SEND_MSG || CMI_ZC_MSGTYPE(env) == CMK_ZC_BCAST_SEND_MSG){
    envelope *prevEnv = env;

    // Determine mode depending on the message
    ncpyEmApiMode mode = (CMI_ZC_MSGTYPE(env) == CMK_ZC_BCAST_SEND_MSG) ? ncpyEmApiMode::BCAST_SEND : ncpyEmApiMode::P2P_SEND;

    env = CkRdmaIssueRgets(env, mode, prevEnv);

    if(env) {
      // memcpyGet or cmaGet completed, env contains the payload and will be enqueued

      // Free prevEnv
      CkFreeMsg(EnvToUsr(prevEnv));
    } else {
      // async rdma call in place, asynchronous return and ack handling
      return;
    }
  }

//#if CMK_RECORD_REPLAY
  if (ck->watcher!=NULL) {
    if (!ck->watcher->processMessage(&env,ck)) return;
  }
//#endif
#if USE_CRITICAL_PATH_HEADER_ARRAY
  CK_CRITICALPATH_START(env)
#endif

  switch(env->getMsgtype()) {
// Group support
    case BocInitMsg : // Group creation message
      TELLMSGTYPE(CkPrintf("proc[%d]: _processHandler with msg type: BocInitMsg\n", CkMyPe());)
      // QD processing moved inside _processBocInitMsg because it is conditional
      //ck->process(); 
      if(env->isPacked()) CkUnpackMessage(&env);
      _processBocInitMsg(ck,env);
      break;
    case NodeBocInitMsg : // Nodegroup creation message
      TELLMSGTYPE(CkPrintf("proc[%d]: _processHandler with msg type: NodeBocInitMsg\n", CkMyPe());)
      if(env->isPacked()) CkUnpackMessage(&env);
      _processNodeBocInitMsg(ck,env);
      break;
    case ForBocMsg : // Group entry method message (non creation)
    case ArrayBcastFwdMsg :
      TELLMSGTYPE(CkPrintf("proc[%d]: _processHandler with msg type: ForBocMsg\n", CkMyPe());)
      // QD processing moved inside _processForBocMsg because it is conditional
      if(env->isPacked()) CkUnpackMessage(&env);
      _processForBocMsg(ck,env);
      // stats record moved inside _processForBocMsg because it is conditional
      break;
    case ForNodeBocMsg : // Nodegroup entry method message (non creation)
      TELLMSGTYPE(CkPrintf("proc[%d]: _processHandler with msg type: ForNodeBocMsg\n", CkMyPe());)
      // QD processing moved to _processForNodeBocMsg because it is conditional
      if(env->isPacked()) CkUnpackMessage(&env);
      _processForNodeBocMsg(ck,env);
      // stats record moved to _processForNodeBocMsg because it is conditional
      break;
    case BocBcastMsg : // Broadcast that needs to be forwarded
    case ArrayBcastMsg :
      if (env->isPacked()) CkUnpackMessage(&env);
      _processBocBcastMsg(ck,env);
      break;

// Array support
    case ForArrayEltMsg: // Array element entry method message
      TELLMSGTYPE(CkPrintf("proc[%d]: _processHandler with msg type: ForArrayEltMsg\n", CkMyPe());)
      if(env->isPacked()) CkUnpackMessage(&env);
      _processArrayEltMsg(ck,env);
      break;

// Chare support
    case NewChareMsg : // Singleton chare creation message
      TELLMSGTYPE(CkPrintf("proc[%d]: _processHandler with msg type: NewChareMsg\n", CkMyPe());)
      if(env->isPacked()) CkUnpackMessage(&env);
      _processNewChareMsg(ck,env);
      break;
    case NewVChareMsg : // Singleton virtual chare creation message
      TELLMSGTYPE(CkPrintf("proc[%d]: _processHandler with msg type: NewVChareMsg\n", CkMyPe());)
      if(env->isPacked()) CkUnpackMessage(&env);
      _processNewVChareMsg(ck,env);
      break;
    case ForChareMsg : // Singeton chare entry method message (non creation)
      TELLMSGTYPE(CkPrintf("proc[%d]: _processHandler with msg type: ForChareMsg\n", CkMyPe());)
      if(env->isPacked()) CkUnpackMessage(&env);
      _processForPlainChareMsg(ck,env);
      break;
    case ForVidMsg   : // Singleton virtual chare entry method message (non creation)
      TELLMSGTYPE(CkPrintf("proc[%d]: _processHandler with msg type: ForVidMsg\n", CkMyPe());)
      _processForVidMsg(ck,env);
      break;
    case FillVidMsg  : // Message sent back from the real chare PE to the virtual chare PE to
                       // fill the VidBlock (called when the real chare is constructed)
      TELLMSGTYPE(CkPrintf("proc[%d]: _processHandler with msg type: FillVidMsg\n", CkMyPe());)
      _processFillVidMsg(ck,env);
      break;
    case DeleteVidMsg  : // Message sent back from the real chare PE to the virtual chare PE to
                         // delete the Vidblock (called when the real chare is deleted by the destructor)
      TELLMSGTYPE(CkPrintf("proc[%d]: _processHandler with msg type: DeleteVidMsg\n", CkMyPe());)
      _processDeleteVidMsg(ck,env);
      break;

    default:
      CmiAbort("Fatal Charm++ Error> Unknown msg-type in _processHandler.\n");
  }


#if USE_CRITICAL_PATH_HEADER_ARRAY
  CK_CRITICALPATH_END()
#endif

}


/******************** Message Send **********************/

void _infoFn(void *converseMsg, CldPackFn *pfn, int *len,
             int *queueing, int *priobits, unsigned int **prioptr)
{
  envelope *env = (envelope *)converseMsg;
  *pfn = (CldPackFn)CkPackMessage;
  *len = env->getTotalsize();
  *queueing = env->getQueueing();
  *priobits = env->getPriobits();
  *prioptr = (unsigned int *) env->getPrioPtr();
}

void CkPackMessage(envelope **pEnv)
{
  envelope *env = *pEnv;
  if(!env->isPacked() && _msgTable[env->getMsgIdx()]->pack) {
    void *msg = EnvToUsr(env);
    short int zcMsgType = CMI_ZC_MSGTYPE(env);
    if(zcMsgType == CMK_ZC_SEND_DONE_MSG) // Store buffer pointers as offsets
      CkPackRdmaPtrs(((CkMarshallMsg *)msg)->msgBuf);
    _TRACE_BEGIN_PACK();
    msg = _msgTable[env->getMsgIdx()]->pack(msg);
    _TRACE_END_PACK();
    env=UsrToEnv(msg);
    env->setPacked(1);
    *pEnv = env;
  }
}

void CkUnpackMessage(envelope **pEnv)
{
  envelope *env = *pEnv;
  int msgIdx = env->getMsgIdx();
  if(env->isPacked()) {
    void *msg = EnvToUsr(env);
    _TRACE_BEGIN_UNPACK();
    msg = _msgTable[msgIdx]->unpack(msg);
    _TRACE_END_UNPACK();
    env=UsrToEnv(msg);
    env->setPacked(0);
    short int zcMsgType = CMI_ZC_MSGTYPE(env);
    if(zcMsgType == CMK_ZC_SEND_DONE_MSG) // Convert offsets back into buffer pointers
      CkUnpackRdmaPtrs(((CkMarshallMsg *)msg)->msgBuf);
    *pEnv = env;
  }
}

//There's no reason for most messages to go through the Cld--
// the PE can never be CLD_ANYWHERE; wasting _infoFn calls.
// Thus these accellerated versions of the Cld calls.
#if CMK_OBJECT_QUEUE_AVAILABLE
static int index_objectQHandler;
#endif
int index_tokenHandler;
int index_skipCldHandler;

void _skipCldHandler(void *converseMsg)
{
  envelope *env = (envelope *)(converseMsg);
  CmiSetHandler(converseMsg, CmiGetXHandler(converseMsg));
#if CMK_GRID_QUEUE_AVAILABLE
  if (CmiGridQueueLookupMsg ((char *) converseMsg)) {
    CqsEnqueueGeneral ((Queue) CpvAccess (CsdGridQueue),
		       env, env->getQueueing (), env->getPriobits (),
		       (unsigned int *) env->getPrioPtr ());
  } else {
    CqsEnqueueGeneral ((Queue) CpvAccess (CsdSchedQueue),
		       env, env->getQueueing (), env->getPriobits (),
		       (unsigned int *) env->getPrioPtr ());
  }
#else
  CqsEnqueueGeneral((Queue)CpvAccess(CsdSchedQueue),
  	env, env->getQueueing(),env->getPriobits(),
  	(unsigned int *)env->getPrioPtr());
#endif
}


//static void _skipCldEnqueue(int pe,envelope *env, int infoFn)
// Made non-static to be used by ckmessagelogging
void _skipCldEnqueue(int pe,envelope *env, int infoFn)
{
#if CMK_CHARMDEBUG
  if (!ConverseDeliver(pe)) {
    CmiFree(env);
    return;
  }
#endif

  // Store source information to handle acknowledgements on completion
  if (CMI_IS_ZC(env)) {
    CkRdmaPrepareZCMsg(env, CkNodeOf(pe));
  }

  if (pe == CkMyPe() && !CmiImmIsRunning()) {
#if CMK_OBJECT_QUEUE_AVAILABLE
    Chare *obj = CkFindObjectPtr(env);
    if (obj && obj->CkGetObjQueue().queue()) {
      _enqObjQueue(obj, env);
    }
    else
#endif
    {
      CqsEnqueueGeneral((Queue)CpvAccess(CsdSchedQueue),
          env, env->getQueueing(),env->getPriobits(),
          (unsigned int *)env->getPrioPtr());
    }
#if CMK_PERSISTENT_COMM
    CmiPersistentOneSend();
#endif
  } else {
    if (pe < 0 || CmiNodeOf(pe) != CmiMyNode()) {
      CkPackMessage(&env);
    }
    int len=env->getTotalsize();
    CmiSetXHandler(env,CmiGetHandler(env));
#if CMK_OBJECT_QUEUE_AVAILABLE
    CmiSetHandler(env,index_objectQHandler);
#else
    CmiSetHandler(env,index_skipCldHandler);
#endif
    CmiSetInfo(env,infoFn);
    if (pe==CLD_BROADCAST) {
      CmiSyncNodeBroadcastAndFree(len, (char *)env);
    } else if (pe==CLD_BROADCAST_ALL) {
      CmiSyncNodeBroadcastAllAndFree(len, (char *)env);
    } else {
      CmiSyncSendAndFree(pe, len, (char *)env);
    }
  }
}


// by pass Charm++ priority queue, send as Converse message
static void _noCldEnqueueMulti(int npes, const int *pes, envelope *env)
{
#if CMK_CHARMDEBUG
  if (!ConverseDeliver(-1)) {
    CmiFree(env);
    return;
  }
#endif
  CkPackMessage(&env);
  int len=env->getTotalsize();
  CmiSyncListSendAndFree(npes, pes, len, (char *)env);
}

static void _noCldEnqueue(int pe, envelope *env)
{
/*
  if (pe == CkMyPe()) {
    CmiHandleMessage(env);
  } else
*/
#if CMK_CHARMDEBUG
  if (!ConverseDeliver(pe)) {
    CmiFree(env);
    return;
  }
#endif

  // Store source information to handle acknowledgements on completion
  if(CMI_IS_ZC(env))
    CkRdmaPrepareZCMsg(env, CkNodeOf(pe));

  CkPackMessage(&env);
  int len=env->getTotalsize();
  if (pe==CLD_BROADCAST) { CmiSyncNodeBroadcastAndFree(len, (char *)env); }
  else if (pe==CLD_BROADCAST_ALL) { CmiSyncNodeBroadcastAllAndFree(len, (char *)env); }
  else CmiSyncSendAndFree(pe, len, (char *)env);
}

//static void _noCldNodeEnqueue(int node, envelope *env)
//Made non-static to be used by ckmessagelogging
void _noCldNodeEnqueue(int node, envelope *env)
{
/*
  if (node == CkMyNode()) {
    CmiHandleMessage(env);
  } else {
*/
#if CMK_CHARMDEBUG
  if (!ConverseDeliver(node)) {
    CmiFree(env);
    return;
  }
#endif

  // Store source information to handle acknowledgements on completion
  if(CMI_IS_ZC(env))
    CkRdmaPrepareZCMsg(env, node);

  CkPackMessage(&env);
  int len=env->getTotalsize();
  if (node==CLD_BROADCAST) { 
	CmiSyncNodeBroadcastAndFree(len, (char *)env); 
}
  else if (node==CLD_BROADCAST_ALL) { 
		CmiSyncNodeBroadcastAllAndFree(len, (char *)env); 

}
  else {
	CmiSyncNodeSendAndFree(node, len, (char *)env);
  }
}

#if CMK_REPLAYSYSTEM && !CMK_TRACE_ENABLED
#error "Building with Record/Replay support requires tracing support!"
#endif

static inline int _prepareMsg(int eIdx,void *msg,const CkChareID *pCid)
{
  envelope *env = UsrToEnv(msg);
  _CHECK_USED(env);
  _SET_USED(env, 1);
#if CMK_REPLAYSYSTEM
  setEventID(env);
#endif
  env->setMsgtype(ForChareMsg);
  env->setEpIdx(eIdx);
  env->setSrcPe(CkMyPe());

#if USE_CRITICAL_PATH_HEADER_ARRAY
  CK_CRITICALPATH_SEND(env)
  //CK_AUTOMATE_PRIORITY(env)
#endif
#if CMK_CHARMDEBUG
  setMemoryOwnedBy(((char*)env)-sizeof(CmiChunkHeader), 0);
#endif
#if CMK_OBJECT_QUEUE_AVAILABLE
  CmiSetHandler(env, index_objectQHandler);
#else
  CmiSetHandler(env, _charmHandlerIdx);
#endif
  if (pCid->onPE < 0) { //Virtual chare ID (VID)
    int pe = -(pCid->onPE+1);
    if(pe==CkMyPe()) {
#ifndef CMK_CHARE_USE_PTR
      VidBlock *vblk = CkpvAccess(vidblocks)[(CmiIntPtr)pCid->objPtr];
#else
      VidBlock *vblk = (VidBlock *) pCid->objPtr;
#endif
      void *objPtr;
      if (NULL!=(objPtr=vblk->getLocalChare()))
      { //A ready local chare
	env->setObjPtr(objPtr);
	return pe;
      }
      else { //The vidblock is not ready-- forget it
        vblk->send(env);
        return -1;
      }
    } else { //Valid vidblock for another PE:
      env->setMsgtype(ForVidMsg);
      env->setVidPtr(pCid->objPtr);
      return pe;
    }
  }
  else {
    env->setObjPtr(pCid->objPtr);
    return pCid->onPE;
  }
}

static inline int _prepareImmediateMsg(int eIdx,void *msg,const CkChareID *pCid)
{
  int destPE = _prepareMsg(eIdx, msg, pCid);
  if (destPE != -1) {
    envelope *env = UsrToEnv(msg);
    //criticalPath_send(env);
#if USE_CRITICAL_PATH_HEADER_ARRAY
    CK_CRITICALPATH_SEND(env)
    //CK_AUTOMATE_PRIORITY(env)
#endif
    CmiBecomeImmediate(env);
  }
  return destPE;
}

void CkSendMsg(int entryIdx, void *msg,const CkChareID *pCid, int opts)
{
  if (opts & CK_MSG_INLINE) {
    CkSendMsgInline(entryIdx, msg, pCid, opts);
    return;
  }
  envelope *env = UsrToEnv(msg);
#if CMK_ERROR_CHECKING
  //Allow rdma metadata messages marked as immediate to go through
  if (opts & CK_MSG_IMMEDIATE)
    if (CMI_ZC_MSGTYPE(env) == CMK_REG_NO_ZC_MSG)
      CmiAbort("Immediate message is not allowed in Chare!");
#endif
  int destPE=_prepareMsg(entryIdx,msg,pCid);
  // Before it traced the creation only if destPE!=-1 (i.e it did not when the
  // VidBlock was not yet filled). The problem is that the creation was never
  // traced later when the VidBlock was filled. One solution is to trace the
  // creation here, the other to trace it in VidBlock->msgDeliver().
  _TRACE_CREATION_1(env);
  if (destPE!=-1) {
    CpvAccess(_qd)->create();
    if (opts & CK_MSG_SKIP_OR_IMM)
      _noCldEnqueue(destPE, env);
    else
      _CldEnqueue(destPE, env, _infoIdx);
  }
  _TRACE_CREATION_DONE(1);
}

void CkSendMsgInline(int entryIndex, void *msg, const CkChareID *pCid, int opts)
{
  if (pCid->onPE==CkMyPe())
  {
#if CMK_CHARMDEBUG
    //Just in case we need to breakpoint or use the envelope in some way
    _prepareMsg(entryIndex,msg,pCid);
#endif
		//Just directly call the chare (skip QD handling & scheduler)
    envelope *env = UsrToEnv(msg);
    if (env->isPacked()) CkUnpackMessage(&env);
    _STATS_RECORD_PROCESS_MSG_1();
    _invokeEntryNoTrace(entryIndex,env,pCid->objPtr);
  }
  else {
    //No way to inline a cross-processor message:
    CkSendMsg(entryIndex, msg, pCid, opts & (~CK_MSG_INLINE));
  }
}

static inline envelope *_prepareMsgBranch(int eIdx,void *msg,CkGroupID gID,int type)
{
  envelope *env = UsrToEnv(msg);
  /*#if CMK_ERROR_CHECKING
  CkNodeGroupID nodeRedMgr;
#endif
  */
  _CHECK_USED(env);
  _SET_USED(env, 1);
#if CMK_REPLAYSYSTEM
  setEventID(env);
#endif
  env->setMsgtype(type);
  env->setEpIdx(eIdx);
  env->setGroupNum(gID);
  env->setSrcPe(CkMyPe());

  CMI_MSG_NOKEEP(env) = _entryTable[eIdx]->noKeep;
  /*
#if CMK_ERROR_CHECKING
  nodeRedMgr.setZero();
  env->setRednMgr(nodeRedMgr);
#endif
*/
  //criticalPath_send(env);
#if USE_CRITICAL_PATH_HEADER_ARRAY
  CK_CRITICALPATH_SEND(env)
  //CK_AUTOMATE_PRIORITY(env)
#endif
#if CMK_CHARMDEBUG
  setMemoryOwnedBy(((char*)env)-sizeof(CmiChunkHeader), 0);
#endif
  CmiSetHandler(env, _charmHandlerIdx);
  return env;
}

static inline envelope *_prepareImmediateMsgBranch(int eIdx,void *msg,CkGroupID gID,int type)
{
  envelope *env = _prepareMsgBranch(eIdx, msg, gID, type);
#if USE_CRITICAL_PATH_HEADER_ARRAY
  CK_CRITICALPATH_SEND(env)
  //CK_AUTOMATE_PRIORITY(env)
#endif
  CmiBecomeImmediate(env);
  return env;
}

static inline void _sendMsgBranch(int eIdx, void *msg, CkGroupID gID,
                  int pe=CLD_BROADCAST_ALL, int opts = 0)
{
  int numPes;
  envelope *env;
    if (opts & CK_MSG_IMMEDIATE) {
        env = _prepareImmediateMsgBranch(eIdx,msg,gID,ForBocMsg);
    }else
    {
        if (pe == CLD_BROADCAST || pe == CLD_BROADCAST_ALL)
          if (UsrToEnv(msg)->getMsgtype() == ArrayBcastMsg) {
            env = _prepareMsgBranch(eIdx,msg,gID,ArrayBcastMsg);
          } else {
            env = _prepareMsgBranch(eIdx,msg,gID,BocBcastMsg);
          }
        else
          env = _prepareMsgBranch(eIdx,msg,gID,ForBocMsg);
    }

  _TRACE_ONLY(numPes = (pe==CLD_BROADCAST_ALL?CkNumPes():1));
  _TRACE_CREATION_N(env, numPes);
  if (opts & CK_MSG_SKIP_OR_IMM)
    _noCldEnqueue(pe, env);
  else
    _skipCldEnqueue(pe, env, _infoIdx);
  _TRACE_CREATION_DONE(1);
}

static inline void _sendMsgBranchWithinNode(int eIdx, void *msg, CkGroupID gID)
{
  envelope* env;
  if (UsrToEnv(msg)->getMsgtype() == ArrayBcastMsg) {
    env = _prepareMsgBranch(eIdx,msg,gID,ArrayBcastFwdMsg);
  } else if (UsrToEnv(msg)->getMsgtype() == BocBcastMsg
      || UsrToEnv(msg)->getMsgtype() == ForNodeBocMsg) {
    // TODO: Does this make sense when its a ForNode? Should ForNode even hit?
    env = _prepareMsgBranch(eIdx,msg,gID,ForBocMsg);
  } else {
    CkAbort("Bad message type %i\n", UsrToEnv(msg)->getMsgtype());
  }
  _TRACE_CREATION_N(env, CmiMyNodeSize());
  _CldEnqueueWithinNode(env, _infoIdx);
  _TRACE_CREATION_DONE(1);  // since it only creates one creation event.
}

static inline void _processBocBcastMsg(CkCoreState* ck, envelope* env) {
  _SET_USED(env, 0);
  _sendMsgBranchWithinNode(env->getEpIdx(), EnvToUsr(env), env->getGroupNum());
}

static inline void _sendMsgBranchMulti(int eIdx, void *msg, CkGroupID gID,
                           int npes, const int *pes)
{
  envelope *env = _prepareMsgBranch(eIdx,msg,gID,ForBocMsg);
  _TRACE_CREATION_MULTICAST(env, npes, pes);
  _CldEnqueueMulti(npes, pes, env, _infoIdx);
  _TRACE_CREATION_DONE(1); 	// since it only creates one creation event.
}

void CkSendMsgBranchImmediate(int eIdx, void *msg, int destPE, CkGroupID gID)
{
#if CMK_IMMEDIATE_MSG && ! CMK_SMP
  if (destPE==CkMyPe())
  {
    CkSendMsgBranchInline(eIdx, msg, destPE, gID);
    return;
  }
  //Can't inline-- send the usual way
  envelope *env = UsrToEnv(msg);
  int numPes;
  _TRACE_ONLY(numPes = (destPE==CLD_BROADCAST_ALL?CkNumPes():1));
  env = _prepareImmediateMsgBranch(eIdx,msg,gID,ForBocMsg);
  _TRACE_CREATION_N(env, numPes);
  _noCldEnqueue(destPE, env);
  _STATS_RECORD_SEND_BRANCH_1();
  CkpvAccess(_coreState)->create();
  _TRACE_CREATION_DONE(1);
#else
  // no support for immediate message, send inline
  CkSendMsgBranchInline(eIdx, msg, destPE, gID);
#endif
}

void CkSendMsgBranchInline(int eIdx, void *msg, int destPE, CkGroupID gID, int opts)
{
  if (destPE==CkMyPe())
  {
    IrrGroup *obj=(IrrGroup *)_localBranch(gID);
    if (obj!=NULL)
    { //Just directly call the group:
#if CMK_ERROR_CHECKING
      envelope *env=_prepareMsgBranch(eIdx,msg,gID,ForBocMsg);
#else
      envelope *env=UsrToEnv(msg);
#endif
      _deliverForBocMsg(CkpvAccess(_coreState),eIdx,env,obj);
      return;
    }
  }
  //Can't inline-- send the usual way, clear CK_MSG_INLINE
  CkSendMsgBranch(eIdx, msg, destPE, gID, opts & (~CK_MSG_INLINE));
}

void CkSendMsgBranch(int eIdx, void *msg, int pe, CkGroupID gID, int opts)
{
  if (opts & CK_MSG_INLINE) {
    CkSendMsgBranchInline(eIdx, msg, pe, gID, opts);
    return;
  }
  envelope *env=UsrToEnv(msg);
  //Allow rdma metadata messages marked as immediate to go through
  if (opts & CK_MSG_IMMEDIATE) {
    if (CMI_ZC_MSGTYPE(env) == CMK_REG_NO_ZC_MSG)
    {
      CkSendMsgBranchImmediate(eIdx,msg,pe,gID);
      return;
    }
  }
  _sendMsgBranch(eIdx, msg, gID, pe, opts);
  _STATS_RECORD_SEND_BRANCH_1();
  CkpvAccess(_coreState)->create();
}

void CkSendMsgBranchMultiImmediate(int eIdx,void *msg,CkGroupID gID,int npes,const int *pes)
{
#if CMK_IMMEDIATE_MSG && ! CMK_SMP
  envelope *env = _prepareImmediateMsgBranch(eIdx,msg,gID,ForBocMsg);
  _TRACE_CREATION_MULTICAST(env, npes, pes);
  _noCldEnqueueMulti(npes, pes, env);
  _TRACE_CREATION_DONE(1);      // since it only creates one creation event.
#else
  _sendMsgBranchMulti(eIdx, msg, gID, npes, pes);
  CpvAccess(_qd)->create(-npes);
#endif
  _STATS_RECORD_SEND_BRANCH_N(npes);
  CpvAccess(_qd)->create(npes);
}

void CkSendMsgBranchMulti(int eIdx,void *msg,CkGroupID gID,int npes,const int *pes, int opts)
{
  if (opts & CK_MSG_IMMEDIATE) {
    CkSendMsgBranchMultiImmediate(eIdx,msg,gID,npes,pes);
    return;
  }
    // normal mesg
  _sendMsgBranchMulti(eIdx, msg, gID, npes, pes);
  _STATS_RECORD_SEND_BRANCH_N(npes);
  CpvAccess(_qd)->create(npes);
}

void CkSendMsgBranchGroup(int eIdx,void *msg,CkGroupID gID,CmiGroup grp, int opts)
{
  int npes;
  int *pes;
  if (opts & CK_MSG_IMMEDIATE) {
    CmiAbort("CkSendMsgBranchGroup: immediate messages not supported!");
    return;
  }
    // normal mesg
  envelope *env = _prepareMsgBranch(eIdx,msg,gID,ForBocMsg);
  CmiLookupGroup(grp, &npes, &pes);
  _TRACE_CREATION_MULTICAST(env, npes, pes);
  _CldEnqueueGroup(grp, env, _infoIdx);
  _TRACE_CREATION_DONE(1); 	// since it only creates one creation event.
  _STATS_RECORD_SEND_BRANCH_N(npes);
  CpvAccess(_qd)->create(npes);
}

void CkBroadcastWithinNode(int eIdx, void *msg, CkGroupID gID, int opts)
{
  _sendMsgBranchWithinNode(eIdx, msg, gID);
  _STATS_RECORD_SEND_BRANCH_N(CmiMyNodeSize());
  CpvAccess(_qd)->create(CmiMyNodeSize());
}

void CkBroadcastMsgBranch(int eIdx, void *msg, CkGroupID gID, int opts)
{
  _sendMsgBranch(eIdx, msg, gID, CLD_BROADCAST_ALL, opts);
  _STATS_RECORD_SEND_BRANCH_N(CkNumPes());
  CpvAccess(_qd)->create(CkNumPes());
}

static inline void _sendMsgNodeBranch(int eIdx, void *msg, CkGroupID gID,
                int node=CLD_BROADCAST_ALL, int opts=0)
{
    int numPes;
    envelope *env;
    if (opts & CK_MSG_IMMEDIATE) {
        env = _prepareImmediateMsgBranch(eIdx,msg,gID,ForNodeBocMsg);
    }else
    {
        env = _prepareMsgBranch(eIdx,msg,gID,ForNodeBocMsg);
    }
  numPes = (node==CLD_BROADCAST_ALL?CkNumNodes():1);
  _TRACE_CREATION_N(env, numPes);
  if (opts & CK_MSG_SKIP_OR_IMM) {
    _noCldNodeEnqueue(node, env);
  }
  else
    _CldNodeEnqueue(node, env, _infoIdx);
  _TRACE_CREATION_DONE(1);
}

static inline void _sendMsgNodeBranchMulti(int eIdx, void *msg, CkGroupID gID,
                           int npes, const int *nodes)
{
  envelope *env = _prepareMsgBranch(eIdx,msg,gID,ForNodeBocMsg);
  _TRACE_CREATION_N(env, npes);
  for (int i=0; i<npes; i++) {
    _CldNodeEnqueue(nodes[i], env, _infoIdx);
  }
  _TRACE_CREATION_DONE(1);  // since it only creates one creation event.
}

void CkSendMsgNodeBranchImmediate(int eIdx, void *msg, int node, CkGroupID gID)
{
#if CMK_IMMEDIATE_MSG
  if (node==CkMyNode())
  {
    CkSendMsgNodeBranchInline(eIdx, msg, node, gID);
    return;
  }
  //Can't inline-- send the usual way
  envelope *env = UsrToEnv(msg);
  int numPes;
  _TRACE_ONLY(numPes = (node==CLD_BROADCAST_ALL?CkNumNodes():1));
  env = _prepareImmediateMsgBranch(eIdx,msg,gID,ForNodeBocMsg);
  _TRACE_CREATION_N(env, numPes);
  _noCldNodeEnqueue(node, env);
  _STATS_RECORD_SEND_BRANCH_1();
  CkpvAccess(_coreState)->create();
  _TRACE_CREATION_DONE(1);
#else
  // no support for immediate message, send inline
  CkSendMsgNodeBranchInline(eIdx, msg, node, gID);
#endif
}

void CkSendMsgNodeBranchInline(int eIdx, void *msg, int node, CkGroupID gID, int opts)
{
  if (node==CkMyNode()) {
    if (CMI_ZC_MSGTYPE(UsrToEnv(msg)) == CMK_REG_NO_ZC_MSG)
    {
      CmiImmediateLock(CksvAccess(_nodeGroupTableImmLock));
      void *obj = CksvAccess(_nodeGroupTable)->find(gID).getObj();
      CmiImmediateUnlock(CksvAccess(_nodeGroupTableImmLock));
      if (obj!=NULL)
      { //Just directly call the group:
#if CMK_ERROR_CHECKING
        envelope *env=_prepareMsgBranch(eIdx,msg,gID,ForNodeBocMsg);
#else
        envelope *env=UsrToEnv(msg);
#endif
        _deliverForNodeBocMsg(CkpvAccess(_coreState),eIdx,env,obj);
        return;
      }
    }
  }
  //Can't inline-- send the usual way
  CkSendMsgNodeBranch(eIdx, msg, node, gID, opts & ~(CK_MSG_INLINE));
}

void CkSendMsgNodeBranch(int eIdx, void *msg, int node, CkGroupID gID, int opts)
{
  if (opts & CK_MSG_INLINE) {
    CkSendMsgNodeBranchInline(eIdx, msg, node, gID, opts);
    return;
  }
  if (opts & CK_MSG_IMMEDIATE) {
    CkSendMsgNodeBranchImmediate(eIdx, msg, node, gID);
    return;
  }
  _sendMsgNodeBranch(eIdx, msg, gID, node, opts);
  _STATS_RECORD_SEND_NODE_BRANCH_1();
  CkpvAccess(_coreState)->create();
}

void CkSendMsgNodeBranchMultiImmediate(int eIdx,void *msg,CkGroupID gID,int npes,const int *nodes)
{
#if CMK_IMMEDIATE_MSG && ! CMK_SMP
  envelope *env = _prepareImmediateMsgBranch(eIdx,msg,gID,ForNodeBocMsg);
  _noCldEnqueueMulti(npes, nodes, env);
#else
  _sendMsgNodeBranchMulti(eIdx, msg, gID, npes, nodes);
  CpvAccess(_qd)->create(-npes);
#endif
  _STATS_RECORD_SEND_NODE_BRANCH_N(npes);
  CpvAccess(_qd)->create(npes);
}

void CkSendMsgNodeBranchMulti(int eIdx,void *msg,CkGroupID gID,int npes,const int *nodes, int opts)
{
  if (opts & CK_MSG_IMMEDIATE) {
    CkSendMsgNodeBranchMultiImmediate(eIdx,msg,gID,npes,nodes);
    return;
  }
    // normal mesg
  _sendMsgNodeBranchMulti(eIdx, msg, gID, npes, nodes);
  _STATS_RECORD_SEND_NODE_BRANCH_N(npes);
  CpvAccess(_qd)->create(npes);
}

void CkBroadcastMsgNodeBranch(int eIdx, void *msg, CkGroupID gID, int opts)
{
  _sendMsgNodeBranch(eIdx, msg, gID, CLD_BROADCAST_ALL, opts);
  _STATS_RECORD_SEND_NODE_BRANCH_N(CkNumNodes());
  CpvAccess(_qd)->create(CkNumNodes());
}

//Needed by delegation manager:
int CkChareMsgPrep(int eIdx, void *msg,const CkChareID *pCid)
{ return _prepareMsg(eIdx,msg,pCid); }
void CkGroupMsgPrep(int eIdx, void *msg, CkGroupID gID)
{ _prepareMsgBranch(eIdx,msg,gID,ForBocMsg); }
void CkNodeGroupMsgPrep(int eIdx, void *msg, CkGroupID gID)
{ _prepareMsgBranch(eIdx,msg,gID,ForNodeBocMsg); }

void _ckModuleInit(void) {
	CmiAssignOnce(&index_skipCldHandler, CkRegisterHandler(_skipCldHandler));
#if CMK_OBJECT_QUEUE_AVAILABLE
	CmiAssignOnce(&index_objectQHandler, CkRegisterHandler(_ObjectQHandler));
#endif
	CmiAssignOnce(&index_tokenHandler, CkRegisterHandler(_TokenHandler));
	CkpvInitialize(TokenPool*, _tokenPool);
	CkpvAccess(_tokenPool) = new TokenPool;
}


/************** Send: Arrays *************/

static void _prepareOutgoingArrayMsg(envelope *env,int type)
{
  _CHECK_USED(env);
  _SET_USED(env, 1);
  env->setMsgtype(type);
#if CMK_CHARMDEBUG
  setMemoryOwnedBy(((char*)env)-sizeof(CmiChunkHeader), 0);
#endif
  CmiSetHandler(env, _charmHandlerIdx);
  CpvAccess(_qd)->create();
}

void CkArrayManagerDeliver(int pe,void *msg, int opts) {
  envelope *env = UsrToEnv(msg);
  _prepareOutgoingArrayMsg(env,ForArrayEltMsg);
  if (opts & CK_MSG_IMMEDIATE)
    CmiBecomeImmediate(env);
  if (opts & CK_MSG_SKIP_OR_IMM)
    _noCldEnqueue(pe, env);
  else
    _skipCldEnqueue(pe, env, _infoIdx);
}

class ElementDestroyer : public CkLocIterator {
private:
        CkLocMgr *locMgr;
public:
        ElementDestroyer(CkLocMgr* mgr_):locMgr(mgr_){};
        void addLocation(CkLocation &loc) {
	  loc.destroyAll();
        }
};

void CkDeleteChares() {
  int i;
  int numGroups = CkpvAccess(_groupIDTable)->size();

  // delete all plain chares
#ifndef CMK_CHARE_USE_PTR
  for (i=0; i<CkpvAccess(chare_objs).size(); i++) {
	Chare *obj = (Chare*)CkpvAccess(chare_objs)[i];
	delete obj;
	CkpvAccess(chare_objs)[i] = NULL;
  }
  for (i=0; i<CkpvAccess(vidblocks).size(); i++) {
	VidBlock *obj = CkpvAccess(vidblocks)[i];
	delete obj;
	CkpvAccess(vidblocks)[i] = NULL;
  }
#endif

  // delete all array elements
  for(i=0;i<numGroups;i++) {
    IrrGroup *obj = CkpvAccess(_groupTable)->find((*CkpvAccess(_groupIDTable))[i]).getObj();
    if(obj && obj->isLocMgr())  {
      CkLocMgr *mgr = (CkLocMgr*)obj;
      ElementDestroyer destroyer(mgr);
      mgr->iterate(destroyer);
    }
  }

  // delete all groups
  CmiImmediateLock(CkpvAccess(_groupTableImmLock));
  for(i=0;i<numGroups;i++) {
    CkGroupID gID = (*CkpvAccess(_groupIDTable))[i];
    IrrGroup *obj = CkpvAccess(_groupTable)->find(gID).getObj();
    if (obj) delete obj;
  }
  CmiImmediateUnlock(CkpvAccess(_groupTableImmLock));

  // delete all node groups
  if (CkMyRank() == 0) {
    int numNodeGroups = CksvAccess(_nodeGroupIDTable).size();
    for(i=0;i<numNodeGroups;i++) {
      CkGroupID gID = CksvAccess(_nodeGroupIDTable)[i];
      IrrGroup *obj = CksvAccess(_nodeGroupTable)->find(gID).getObj();
      if (obj) delete obj;
    }
  }
}


//------------------- External client support (e.g. Charm4py) ----------------
#if CMK_CHARM4PY

static std::vector< std::vector<char> > ext_args;
static std::vector<char*> ext_argv;

// This is just a wrapper for ConverseInit that copies command-line args into a private
// buffer.
// To be called from external clients like charm4py. This wrapper avoids issues with
// ctypes and cffi.
void StartCharmExt(int argc, char **argv) {
#if !defined(_WIN32) && !NODE_0_IS_CONVHOST
  // only do this in net layers if using charmrun, so that output of process 0
  // doesn't get duplicated
  char *ns = getenv("NETSTART");
  if (ns != 0) {
    int fd;
    if (-1 != (fd = open("/dev/null", O_RDWR))) {
      dup2(fd, 0);
      dup2(fd, 1);
      dup2(fd, 2);
    }
  }
#endif
  ext_args.resize(argc);
  ext_argv.resize(argc + 1, NULL);
  for (int i=0; i < argc; i++) {
    ext_args[i].resize(strlen(argv[i]) + 1);
    strcpy(ext_args[i].data(), argv[i]);
    ext_argv[i] = ext_args[i].data();
  }
  ConverseInit(argc, ext_argv.data(), _initCharm, 0, 0);
}

void (*CkRegisterMainModuleCallback)() = NULL;
void registerCkRegisterMainModuleCallback(void (*cb)()) {
  CkRegisterMainModuleCallback = cb;
}

void (*MainchareCtorExtCallback)(int, void*, int, int, char **) = NULL;
void registerMainchareCtorExtCallback(void (*cb)(int, void*, int, int, char **)) {
  MainchareCtorExtCallback = cb;
}

void (*ReadOnlyRecvExtCallback)(int, char*) = NULL;
void registerReadOnlyRecvExtCallback(void (*cb)(int, char*)) {
  ReadOnlyRecvExtCallback = cb;
}

void* ReadOnlyExt::ro_data = NULL;
size_t ReadOnlyExt::data_size = 0;

void (*ChareMsgRecvExtCallback)(int, void*, int, int, char *, int) = NULL;
void registerChareMsgRecvExtCallback(void (*cb)(int, void*, int, int, char *, int)) {
  ChareMsgRecvExtCallback = cb;
}

void (*GroupMsgRecvExtCallback)(int, int, int, char *, int) = NULL;
void registerGroupMsgRecvExtCallback(void (*cb)(int, int, int, char *, int)) {
  GroupMsgRecvExtCallback = cb;
}

void (*ArrayMsgRecvExtCallback)(int, int, int *, int, int, char *, int) = NULL;
void registerArrayMsgRecvExtCallback(void (*cb)(int, int, int *, int, int, char *, int)) {
  ArrayMsgRecvExtCallback = cb;
}

void (*ArrayBcastRecvExtCallback)(int, int, int, int, int *, int, int, char *, int) = NULL;
void registerArrayBcastRecvExtCallback(void (*cb)(int, int, int, int, int *, int, int, char *, int)) {
  ArrayBcastRecvExtCallback = cb;
}

int (*ArrayElemLeaveExt)(int, int, int *, char**, int) = NULL;
void registerArrayElemLeaveExtCallback(int (*cb)(int, int, int *, char**, int)) {
  ArrayElemLeaveExt = cb;
}

void (*ArrayElemJoinExt)(int, int, int *, int, char*, int) = NULL;
void registerArrayElemJoinExtCallback(void (*cb)(int, int, int *, int, char*, int)) {
  ArrayElemJoinExt = cb;
}

void (*ArrayResumeFromSyncExtCallback)(int, int, int *) = NULL;
void registerArrayResumeFromSyncExtCallback(void (*cb)(int, int, int *)) {
  ArrayResumeFromSyncExtCallback = cb;
}

void (*CreateCallbackMsgExt)(void*, int, int, int, int *, char**, int*) = NULL;
void registerCreateCallbackMsgExtCallback(void (*cb)(void*, int, int, int, int *, char**, int*)) {
  CreateCallbackMsgExt = cb;
}

int (*PyReductionExt)(char**, int*, int, char**) = NULL;
void registerPyReductionExtCallback(int (*cb)(char**, int*, int, char**)) {
    PyReductionExt = cb;
}

int (*ArrayMapProcNumExtCallback)(int, int, const int *) = NULL;
void registerArrayMapProcNumExtCallback(int (*cb)(int, int, const int *)) {
  ArrayMapProcNumExtCallback = cb;
}

int CkMyPeHook() { return CkMyPe(); }
int CkNumPesHook() { return CkNumPes(); }

void ReadOnlyExt::setData(void *msg, size_t msgSize) {
  ro_data = malloc(msgSize);
  memcpy(ro_data, msg, msgSize);
  data_size = msgSize;
}

void ReadOnlyExt::_roPup(void *pup_er) {
  PUP::er &p=*(PUP::er *)pup_er;
  if (!p.isUnpacking()) {
    //printf("[%d] Sizing/packing data, ro_data=%p, data_size=%d\n", CkMyPe(), ro_data, int(data_size));
    p | data_size;
    p((char*)ro_data, data_size);
  } else {
    CkAssert(CkMyPe() != 0);
    CkAssert(ro_data == NULL);
    PUP::fromMem &p_mem = *(PUP::fromMem *)pup_er;
    p_mem | data_size;
    //printf("[%d] Unpacking ro, size of data to unpack is %d\n", CkMyPe(), int(data_size));
    ReadOnlyRecvExtCallback(int(data_size), p_mem.get_current_pointer());
    p_mem.advance(data_size);
  }
}

CkpvExtern(int, _currentChareType);

MainchareExt::MainchareExt(CkArgMsg *m) {
  int cIdx = CkpvAccess(_currentChareType);
  //printf("Constructor of MainchareExt, chareId=(%d,%p), chareIdx=%d\n", thishandle.onPE, thishandle.objPtr, cIdx);
  int ctorEpIdx =  _mainTable[_chareTable[cIdx]->mainChareType()]->entryIdx;
  MainchareCtorExtCallback(thishandle.onPE, thishandle.objPtr, ctorEpIdx, m->argc, m->argv);
  delete m;
}

GroupExt::GroupExt(void *impl_msg) {
  //printf("Constructor of GroupExt, gid=%d\n", thisgroup.idx);
  //int chareIdx = CkpvAccess(_groupTable)->find(thisgroup).getcIdx();
  int chareIdx = ckGetChareType();
  int ctorEpIdx = _chareTable[chareIdx]->getDefaultCtor();
  CkMarshallMsg *impl_msg_typed = (CkMarshallMsg *)impl_msg;
  char *impl_buf = impl_msg_typed->msgBuf;
  PUP::fromMem implP(impl_buf);
  int msgSize; implP|msgSize;
  int dcopy_start; implP|dcopy_start;
  GroupMsgRecvExtCallback(thisgroup.idx, ctorEpIdx, msgSize, impl_buf+(2*sizeof(int)),
                          dcopy_start);
}

ArrayMapExt::ArrayMapExt(void *impl_msg) {
  //printf("Constructor of ArrayMapExt, gid=%d\n", thisgroup.idx);
  int chareIdx = ckGetChareType();
  int ctorEpIdx = _chareTable[chareIdx]->getDefaultCtor();
  CkMarshallMsg *impl_msg_typed = (CkMarshallMsg *)impl_msg;
  char *impl_buf = impl_msg_typed->msgBuf;
  PUP::fromMem implP(impl_buf);
  int msgSize; implP|msgSize;
  int dcopy_start; implP|dcopy_start;
  GroupMsgRecvExtCallback(thisgroup.idx, ctorEpIdx, msgSize, impl_buf+(2*sizeof(int)),
                          dcopy_start);
}

// TODO options
int CkCreateGroupExt(int cIdx, int eIdx, int num_bufs, char **bufs, int *buf_sizes) {
  //static_cast<void>(impl_e_opts);
  CkAssert(num_bufs >= 1);
  int totalSize = 0;
  for (int i=0; i < num_bufs; i++) totalSize += buf_sizes[i];
  int marshall_msg_size = (sizeof(char)*totalSize + sizeof(int)*2);
  CkMarshallMsg *impl_msg = CkAllocateMarshallMsg(marshall_msg_size, NULL);
  PUP::toMem implP((void *)impl_msg->msgBuf);
  implP|totalSize;
  implP|buf_sizes[0];
  for (int i=0; i < num_bufs; i++) implP(bufs[i], buf_sizes[i]);
  UsrToEnv(impl_msg)->setMsgtype(BocInitMsg);
  //if (impl_e_opts)
  //  UsrToEnv(impl_msg)->setGroupDep(impl_e_opts->getGroupDepID());
  CkGroupID gId = CkCreateGroup(cIdx, eIdx, impl_msg);
  return gId.idx;
}

// TODO options
int CkCreateArrayExt(int cIdx, int ndims, int *dims, int eIdx, int num_bufs,
                     char **bufs, int *buf_sizes, int map_gid, char useAtSync) {
  //static_cast<void>(impl_e_opts);
  CkAssert(num_bufs >= 1);
  int totalSize = 0;
  for (int i=0; i < num_bufs; i++) totalSize += buf_sizes[i];
  int marshall_msg_size = (sizeof(char)*totalSize + sizeof(int)*2 + sizeof(char));
  CkMarshallMsg *impl_msg = CkAllocateMarshallMsg(marshall_msg_size, NULL);
  PUP::toMem implP((void *)impl_msg->msgBuf);
  implP|useAtSync;
  implP|totalSize;
  implP|buf_sizes[0];
  for (int i=0; i < num_bufs; i++) implP(bufs[i], buf_sizes[i]);
  CkArrayOptions opts;
  if (ndims != -1)
    opts = CkArrayOptions(ndims, dims);
  if (map_gid >= 0) {
    CkGroupID map_gId;
    map_gId.idx = map_gid;
    opts.setMap(CProxy_Group(map_gId));
  }
  UsrToEnv(impl_msg)->setMsgtype(ArrayEltInitMsg);
  //CkArrayID gId = ckCreateArray((CkArrayMessage *)impl_msg, eIdx, opts);
  CkGroupID gId = CProxyElement_ArrayElement::ckCreateArray((CkArrayMessage *)impl_msg, eIdx, opts);
  return gId.idx;
}

// TODO options
void CkInsertArrayExt(int aid, int ndims, int *index, int epIdx, int onPE, int num_bufs,
                      char **bufs, int *buf_sizes, char useAtSync) {
  CkAssert(num_bufs >= 1);
  int totalSize = 0;
  for (int i=0; i < num_bufs; i++) totalSize += buf_sizes[i];
  int marshall_msg_size = (sizeof(char)*totalSize + sizeof(int)*2 + sizeof(char));
  CkMarshallMsg *impl_msg = CkAllocateMarshallMsg(marshall_msg_size, NULL);
  PUP::toMem implP((void *)impl_msg->msgBuf);
  implP|useAtSync;
  implP|totalSize;
  implP|buf_sizes[0];
  for (int i=0; i < num_bufs; i++) implP(bufs[i], buf_sizes[i]);

  UsrToEnv(impl_msg)->setMsgtype(ArrayEltInitMsg);
  CkArrayIndex newIdx(ndims, index);
  CkGroupID gId;
  gId.idx = aid;
  CProxy_ArrayBase(gId).ckInsertIdx((CkArrayMessage *)impl_msg, epIdx, onPE, newIdx);
}

void CkMigrateExt(int aid, int ndims, int *index, int toPe) {
  //printf("[charm] CkMigrateMeExt called with aid: %d, ndims: %d, index: %d, toPe: %d\n",
        //aid, ndims, *index, toPe);
  CkGroupID gId;
  gId.idx = aid;
  CkArrayIndex arrayIndex(ndims, index);
  CProxyElement_ArrayBase arrayProxy = CProxyElement_ArrayBase(gId, arrayIndex);
  ArrayElement* arrayElement = arrayProxy.ckLocal();
  CkAssert(arrayElement != NULL);
  arrayElement->ckMigrate(toPe);
}

void CkArrayDoneInsertingExt(int aid) {
  CkGroupID gId;
  gId.idx = aid;
  CProxy_ArrayBase(gId).doneInserting();
}

int CkGroupGetReductionNumber(int g_id) {
  CkGroupID gId;
  gId.idx = g_id;
  return ((Group*)CkLocalBranch(gId))->getRedNo();
}

int CkArrayGetReductionNumber(int aid, int ndims, int *index) {
  CkGroupID gId;
  gId.idx = aid;
  CkArrayIndex arrayIndex(ndims, index);
  CProxyElement_ArrayBase arrayProxy = CProxyElement_ArrayBase(gId, arrayIndex);
  ArrayElement* arrayElement = arrayProxy.ckLocal();
  CkAssert(arrayElement != NULL);
  return arrayElement->getRedNo();
}

void CkSetMigratable(int aid, int ndims, int *index, char migratable) {
  CkGroupID gId;
  gId.idx = aid;
  CkArrayIndex arrayIndex(ndims, index);
  CProxyElement_ArrayBase arrayProxy = CProxyElement_ArrayBase(gId, arrayIndex);
  ArrayElement* arrayElement = arrayProxy.ckLocal();
  CkAssert(arrayElement != NULL);
  arrayElement->setMigratable(migratable);
}

void CkStartQDExt_ChareCallback(int onPE, void* objPtr, int epIdx, int fid)
{
  CkStartQD(CkCallback(onPE, objPtr, epIdx, (CMK_REFNUM_TYPE)fid));
}

void CkStartQDExt_GroupCallback(int gid, int pe, int epIdx, int fid)
{
  CkStartQD(CkCallback(gid, pe, epIdx, (CMK_REFNUM_TYPE)fid));
}

void CkStartQDExt_ArrayCallback(int aid, int* idx, int ndims, int epIdx, int fid)
{
  CkStartQD(CkCallback(aid, idx, ndims, epIdx, (CMK_REFNUM_TYPE)fid));
}

void CkStartQDExt_SectionCallback(int sid_pe, int sid_cnt, int rootPE, int ep)
{
  CkStartQD(CkCallback(sid_pe, sid_cnt, rootPE, ep));
}

void CkChareExtSend(int onPE, void *objPtr, int epIdx, char *msg, int msgSize) {
  int marshall_msg_size = (sizeof(char)*msgSize + 3*sizeof(int));
  CkMarshallMsg *impl_msg = CkAllocateMarshallMsg(marshall_msg_size, NULL);
  PUP::toMem implP((void *)impl_msg->msgBuf);
  implP|msgSize;
  implP|epIdx;
  int d=0; implP|d;
  implP(msg, msgSize);
  CkChareID chareID;
  chareID.onPE = onPE;
  chareID.objPtr = objPtr;

  CkSendMsg(epIdx, impl_msg, &chareID);
}

void CkChareExtSend_multi(int onPE, void *objPtr, int epIdx, int num_bufs, char **bufs, int *buf_sizes) {
  CkAssert(num_bufs >= 1);
  int totalSize = 0;
  for (int i=0; i < num_bufs; i++) totalSize += buf_sizes[i];
  int marshall_msg_size = (sizeof(char)*totalSize + 3*sizeof(int));
  CkMarshallMsg *impl_msg = CkAllocateMarshallMsg(marshall_msg_size, NULL);
  PUP::toMem implP((void *)impl_msg->msgBuf);
  implP | totalSize;
  implP | epIdx;
  implP | buf_sizes[0];
  for (int i=0; i < num_bufs; i++) implP(bufs[i], buf_sizes[i]);
  CkChareID chareID;
  chareID.onPE = onPE;
  chareID.objPtr = objPtr;

  CkSendMsg(epIdx, impl_msg, &chareID);
}

void CkGroupExtSend(int gid, int npes, const int *pes, int epIdx, char *msg, int msgSize) {
  int marshall_msg_size = (sizeof(char)*msgSize + 3*sizeof(int));
  CkMarshallMsg *impl_msg = CkAllocateMarshallMsg(marshall_msg_size, NULL);
  PUP::toMem implP((void *)impl_msg->msgBuf);
  implP|msgSize;
  implP|epIdx;
  int d=0; implP|d;
  implP(msg, msgSize);
  CkGroupID gId;
  gId.idx = gid;

  if (pes[0] == -1)
    CkBroadcastMsgBranch(epIdx, impl_msg, gId, 0);
  else if (npes == 1)
    CkSendMsgBranch(epIdx, impl_msg, pes[0], gId, 0);
  else
    CkSendMsgBranchMulti(epIdx, impl_msg, gId, npes, pes, 0);
}

void CkGroupExtSend_multi(int gid, int npes, const int *pes, int epIdx, int num_bufs, char **bufs, int *buf_sizes) {
  CkAssert(num_bufs >= 1);
  int totalSize = 0;
  for (int i=0; i < num_bufs; i++) totalSize += buf_sizes[i];
  int marshall_msg_size = (sizeof(char)*totalSize + 3*sizeof(int));
  CkMarshallMsg *impl_msg = CkAllocateMarshallMsg(marshall_msg_size, NULL);
  PUP::toMem implP((void *)impl_msg->msgBuf);
  implP | totalSize;
  implP | epIdx;
  implP | buf_sizes[0];
  for (int i=0; i < num_bufs; i++) implP(bufs[i], buf_sizes[i]);
  CkGroupID gId;
  gId.idx = gid;

  if (pes[0] == -1)
    CkBroadcastMsgBranch(epIdx, impl_msg, gId, 0);
  else if (npes == 1)
    CkSendMsgBranch(epIdx, impl_msg, pes[0], gId, 0);
  else
    CkSendMsgBranchMulti(epIdx, impl_msg, gId, npes, pes, 0);
}

void CkForwardMulticastMsg(int _gid, int num_children, const int *children) {
  CkGroupID gid;
  gid.idx = _gid;
  ((SectionManagerExt*)CkLocalBranch(gid))->forwardMulticastMsg(num_children, children);
}

void CkArrayExtSend(int aid, int *idx, int ndims, int epIdx, char *msg, int msgSize) {
  int marshall_msg_size = (sizeof(char)*msgSize + 3*sizeof(int));
  CkMarshallMsg *impl_msg = CkAllocateMarshallMsg(marshall_msg_size, NULL);
  PUP::toMem implP((void *)impl_msg->msgBuf);
  implP|msgSize;
  implP|epIdx;
  int d=0; implP|d;
  implP(msg, msgSize);
  UsrToEnv(impl_msg)->setMsgtype(ForArrayEltMsg);
  CkArrayMessage *impl_amsg=(CkArrayMessage *)impl_msg;
  impl_amsg->array_setIfNotThere(CkArray_IfNotThere_buffer);
  CkGroupID gId;
  gId.idx = aid;
  if (ndims > 0) {
    CkArrayIndex arrIndex(ndims, idx);
    // TODO is there a better function for this?
    CProxyElement_ArrayBase::ckSendWrapper(gId, arrIndex, impl_amsg, epIdx, 0);
  } else { // broadcast
    CkBroadcastMsgArray(epIdx, impl_amsg, gId, 0);
  }
}

void CkArrayExtSend_multi(int aid, int *idx, int ndims, int epIdx, int num_bufs, char **bufs, int *buf_sizes) {
  CkAssert(num_bufs >= 1);
  int totalSize = 0;
  for (int i=0; i < num_bufs; i++) totalSize += buf_sizes[i];
  int marshall_msg_size = (sizeof(char)*totalSize + 3*sizeof(int));
  CkMarshallMsg *impl_msg = CkAllocateMarshallMsg(marshall_msg_size, NULL);
  PUP::toMem implP((void *)impl_msg->msgBuf);
  implP | totalSize;
  implP | epIdx;
  implP | buf_sizes[0];
  for (int i=0; i < num_bufs; i++) implP(bufs[i], buf_sizes[i]);
  UsrToEnv(impl_msg)->setMsgtype(ForArrayEltMsg);
  CkArrayMessage *impl_amsg=(CkArrayMessage *)impl_msg;
  impl_amsg->array_setIfNotThere(CkArray_IfNotThere_buffer);
  CkGroupID gId;
  gId.idx = aid;
  if (ndims > 0) {
    CkArrayIndex arrIndex(ndims, idx);
    // TODO is there a better function for this?
    CProxyElement_ArrayBase::ckSendWrapper(gId, arrIndex, impl_amsg, epIdx, 0);
  } else { // broadcast
    CkBroadcastMsgArray(epIdx, impl_amsg, gId, 0);
  }
}

#endif

//------------------- Message Watcher (record/replay) ----------------

#include "crc32.h"

CkpvDeclare(int, envelopeEventID);
int _recplay_crc = 0;
int _recplay_checksum = 0;
int _recplay_logsize = 1024*1024;

//#define REPLAYDEBUG(args) ckout<<"["<<CkMyPe()<<"] "<< args <<endl;
#define REPLAYDEBUG(args) /* empty */

CkMessageWatcher::~CkMessageWatcher() { if (next!=NULL) delete next;}

#include "trace-common.h" /* For traceRoot and traceRootBaseLength */
#include "BaseLB.h" /* For LBMigrateMsg message */

#if CMK_REPLAYSYSTEM
static FILE *openReplayFile(const char *prefix, const char *suffix, const char *permissions) {
  std::string fName = CkpvAccess(traceRoot);
  fName += prefix;
  fName += std::to_string(CkMyPe());
  fName += suffix;
  FILE *f = fopen(fName.c_str(), permissions);
  REPLAYDEBUG("openReplayfile " << fName.c_str());
  if (f==NULL) {
    CkPrintf("[%d] Could not open replay file '%s' with permissions '%s'\n",
             CkMyPe(), fName.c_str(), permissions);
    CkAbort("openReplayFile> Could not open replay file");
  }
  return f;
}

class CkMessageRecorder : public CkMessageWatcher {
  unsigned int curpos;
  bool firstOpen;
  std::vector<char> buffer;
public:
  CkMessageRecorder(FILE *f_): curpos(0), firstOpen(true), buffer(_recplay_logsize) { f=f_; }
  ~CkMessageRecorder() {
    flushLog(0);
    fprintf(f,"-1 -1 -1 ");
    fclose(f);
#if 0
    FILE *stsfp = fopen("sts", "w");
    void traceWriteSTS(FILE *stsfp,int nUserEvents);
    traceWriteSTS(stsfp, 0);
    fclose(stsfp);
#endif
    CkPrintf("[%d] closing log at %f.\n", CkMyPe(), CmiWallTimer());
  }

private:
  void flushLog(int verbose=1) {
    if (verbose) CkPrintf("[%d] flushing log\n", CkMyPe());
    fprintf(f, "%s", buffer.data());
    curpos=0;
  }
  virtual bool process(envelope **envptr,CkCoreState *ck) {
    if ((*envptr)->getEvent()) {
      bool wasPacked = (*envptr)->isPacked();
      if (!wasPacked) CkPackMessage(envptr);
      envelope *env = *envptr;
      unsigned int crc1=0, crc2=0;
      if (_recplay_crc) {
        //unsigned int crc = crc32_initial(((unsigned char*)env)+CmiMsgHeaderSizeBytes, env->getTotalsize()-CmiMsgHeaderSizeBytes);
        crc1 = crc32_initial(((unsigned char*)env)+CmiMsgHeaderSizeBytes, sizeof(*env)-CmiMsgHeaderSizeBytes);
        crc2 = crc32_initial(((unsigned char*)env)+sizeof(*env), env->getTotalsize()-sizeof(*env));
      } else if (_recplay_checksum) {
        crc1 = checksum_initial(((unsigned char*)env)+CmiMsgHeaderSizeBytes, sizeof(*env)-CmiMsgHeaderSizeBytes);
        crc2 = checksum_initial(((unsigned char*)env)+sizeof(*env), env->getTotalsize()-sizeof(*env));
      }
      curpos+=sprintf(&buffer[curpos],"%d %d %d %d %x %x %d\n",env->getSrcPe(),env->getTotalsize(),env->getEvent(), env->getMsgtype()==NodeBocInitMsg || env->getMsgtype()==ForNodeBocMsg, crc1, crc2, env->getEpIdx());
      if (curpos > _recplay_logsize-128) flushLog();
      if (!wasPacked) CkUnpackMessage(envptr);
    }
    return true;
  }
  virtual bool process(CthThreadToken *token,CkCoreState *ck) {
    curpos+=sprintf(&buffer[curpos], "%d %d %d\n",CkMyPe(), -2, token->serialNo);
    if (curpos > _recplay_logsize-128) flushLog();
    return true;
  }
  
  virtual bool process(LBMigrateMsg **msg,CkCoreState *ck) {
    FILE *f;
    if (firstOpen) f = openReplayFile("ckreplay_",".lb","w");
    else f = openReplayFile("ckreplay_",".lb","a");
    firstOpen = false;
    if (f != NULL) {
      PUP::toDisk p(f);
      p | (*msg)->n_moves; // Need to store to be able to reload the message during replay
      (*msg)->pup(p);
      fclose(f);
    }
    return true;
  }
};

class CkMessageDetailRecorder : public CkMessageWatcher {
public:
  CkMessageDetailRecorder(FILE *f_) {
    f=f_;
    /* The file starts with "x 0" if it is little endian, "0 x" if big endian.
     * The value of 'x' is the pointer size.
     */
    CmiUInt2 little = sizeof(void*);
    fwrite(&little, 2, 1, f);
  }
  ~CkMessageDetailRecorder() {fclose(f);}
private:
  virtual bool process(envelope **envptr, CkCoreState *ck) {
    bool wasPacked = (*envptr)->isPacked();
    if (!wasPacked) CkPackMessage(envptr);
    envelope *env = *envptr;
    CmiUInt4 size = env->getTotalsize();
    fwrite(&size, 4, 1, f);
    fwrite(env, env->getTotalsize(), 1, f);
    if (!wasPacked) CkUnpackMessage(envptr);
    return true;
  }
};

void CkMessageReplayQuiescence(void *rep, double time);
void CkMessageDetailReplayDone(void *rep, double time);

class CkMessageReplay : public CkMessageWatcher {
  int counter;
	int nextPE, nextSize, nextEvent, nexttype; //Properties of next message we need:
	int nextEP;
	unsigned int crc1, crc2;
	FILE *lbFile;
	/// Read the next message we need from the file:
	void getNext(void) {
	  if (3!=fscanf(f,"%d%d%d", &nextPE,&nextSize,&nextEvent)) CkAbort("CkMessageReplay> Syntax error reading replay file");
	  if (nextSize > 0) {
	    // We are reading a regular message
	    if (4!=fscanf(f,"%d%x%x%d", &nexttype,&crc1,&crc2,&nextEP)) {
	      CkAbort("CkMessageReplay> Syntax error reading replay file");
	    }
            REPLAYDEBUG("getNext: "<<nextPE<<" " << nextSize << " " << nextEvent)
	  } else if (nextSize == -2) {
	    // We are reading a special message (right now only thread awaken)
	    // Nothing to do since we have already read all info
            REPLAYDEBUG("getNext: "<<nextPE<<" " << nextSize << " " << nextEvent)
	  } else if (nextPE!=-1 || nextSize!=-1 || nextEvent!=-1) {
	    CkPrintf("Read from file item %d %d %d\n",nextPE,nextSize,nextEvent);
	    CkAbort("CkMessageReplay> Unrecognized input");
	  }
	    /*
		if (6!=fscanf(f,"%d%d%d%d%x%x", &nextPE,&nextSize,&nextEvent,&nexttype,&crc1,&crc2)) {
			CkAbort("CkMessageReplay> Syntax error reading replay file");
			nextPE=nextSize=nextEvent=nexttype=-1; //No destructor->record file just ends in the middle!
		}
		*/
		counter++;
	}
	/// If this is the next message we need, advance and return true.
	bool isNext(envelope *env) {
		if (nextPE!=env->getSrcPe()) return false;
		if (nextEvent!=env->getEvent()) return false;
		if (nextSize<0) return false; // not waiting for a regular message
#if 1
		if (nextEP != env->getEpIdx()) {
			CkPrintf("[%d] CkMessageReplay> Message EP changed during replay org: [%d %d %d %d] got: [%d %d %d %d]\n", CkMyPe(), nextPE, nextSize, nextEvent, nextEP, env->getSrcPe(), env->getTotalsize(), env->getEvent(), env->getEpIdx());
			return false;
		}
#endif
		if (nextSize!=env->getTotalsize())
                {
			CkPrintf("[%d] CkMessageReplay> Message size changed during replay org: [%d %d %d %d] got: [%d %d %d %d]\n", CkMyPe(), nextPE, nextSize, nextEvent, nextEP, env->getSrcPe(), env->getTotalsize(), env->getEvent(), env->getEpIdx());
                        return false;
                }
		if (_recplay_crc || _recplay_checksum) {
		  bool wasPacked = env->isPacked();
		  if (!wasPacked) CkPackMessage(&env);
		  if (_recplay_crc) {
		    //unsigned int crcnew = crc32_initial(((unsigned char*)env)+CmiMsgHeaderSizeBytes, env->getTotalsize()-CmiMsgHeaderSizeBytes);
		    unsigned int crcnew1 = crc32_initial(((unsigned char*)env)+CmiMsgHeaderSizeBytes, sizeof(*env)-CmiMsgHeaderSizeBytes);
		    unsigned int crcnew2 = crc32_initial(((unsigned char*)env)+sizeof(*env), env->getTotalsize()-sizeof(*env));
		    if (crcnew1 != crc1) {
		      CkPrintf("CkMessageReplay %d> Envelope CRC changed during replay org: [0x%x] got: [0x%x]\n",CkMyPe(),crc1,crcnew1);
		    }
		    if (crcnew2 != crc2) {
		      CkPrintf("CkMessageReplay %d> Message CRC changed during replay org: [0x%x] got: [0x%x]\n",CkMyPe(),crc2,crcnew2);
		    }
		  } else if (_recplay_checksum) {
            unsigned int crcnew1 = checksum_initial(((unsigned char*)env)+CmiMsgHeaderSizeBytes, sizeof(*env)-CmiMsgHeaderSizeBytes);
            unsigned int crcnew2 = checksum_initial(((unsigned char*)env)+sizeof(*env), env->getTotalsize()-sizeof(*env));
            if (crcnew1 != crc1) {
              CkPrintf("CkMessageReplay %d> Envelope Checksum changed during replay org: [0x%x] got: [0x%x]\n",CkMyPe(),crc1,crcnew1);
            }
            if (crcnew2 != crc2) {
              CkPrintf("CkMessageReplay %d> Message Checksum changed during replay org: [0x%x] got: [0x%x]\n",CkMyPe(),crc2,crcnew2);
            }		    
		  }
		  if (!wasPacked) CkUnpackMessage(&env);
		}
		return true;
	}
	bool isNext(CthThreadToken *token) {
	  if (nextPE==CkMyPe() && nextSize==-2 && nextEvent==token->serialNo) return true;
	  return false;
	}

	/// This is a (short) list of messages we aren't yet ready for:
	CkQ<envelope *> delayedMessages;
	/// This is a (short) list of tokens (i.e messages that awake user-threads) we aren't yet ready for:
	CkQ<CthThreadToken *> delayedTokens;

	/// Try to flush out any delayed messages
	void flush(void) {
	  if (nextSize>0) {
		int len=delayedMessages.length();
		for (int i=0;i<len;i++) {
			envelope *env=delayedMessages.deq();
			if (isNext(env)) { /* this is the next message: process it */
				REPLAYDEBUG("Dequeueing message: "<<env->getSrcPe()<<" "<<env->getTotalsize()<<" "<<env->getEvent())
				CsdEnqueueLifo((void*)env); // Make it at the beginning since this is the one we want next
				return;
			}
			else /* Not ready yet-- put it back in the
				queue */
			  {
				REPLAYDEBUG("requeueing delayed message: "<<env->getSrcPe()<<" "<<env->getTotalsize()<<" "<<env->getEvent()<<" ep:"<<env->getEpIdx())
				delayedMessages.enq(env);
			  }
		}
	  } else if (nextSize==-2) {
	    int len=delayedTokens.length();
	    for (int i=0;i<len;++i) {
	      CthThreadToken *token=delayedTokens.deq();
	      if (isNext(token)) {
            REPLAYDEBUG("Dequeueing token: "<<token->serialNo)
	        CsdEnqueueLifo((void*)token);
	        return;
	      } else {
            REPLAYDEBUG("requeueing delayed token: "<<token->serialNo)
	        delayedTokens.enq(token);
	      }
	    }
	  }
	}

public:
	CkMessageReplay(FILE *f_) : lbFile(NULL) {
	  counter=0;
	  f=f_;
	  getNext();
	  REPLAYDEBUG("Constructing ckMessageReplay: "<< nextPE <<" "<< nextSize <<" "<<nextEvent);
#if CMI_QD
	  if (CkMyPe()==0) CmiStartQD(CkMessageReplayQuiescence, this);
#endif
	}
	~CkMessageReplay() {fclose(f);}

private:
	virtual bool process(envelope **envptr,CkCoreState *ck) {
          bool wasPacked = (*envptr)->isPacked();
          if (!wasPacked) CkPackMessage(envptr);
          envelope *env = *envptr;
	  //CkAssert(*(int*)env == 0x34567890);
	  REPLAYDEBUG("ProcessMessage message: "<<env->getSrcPe()<<" "<<env->getTotalsize()<<" "<<env->getEvent() <<" " <<env->getMsgtype() <<" " <<env->getMsgIdx() << " ep:" << env->getEpIdx());
                if (env->getEvent() == 0) return true;
		if (isNext(env)) { /* This is the message we were expecting */
			REPLAYDEBUG("Executing message: "<<env->getSrcPe()<<" "<<env->getTotalsize()<<" "<<env->getEvent())
			getNext(); /* Advance over this message */
			flush(); /* try to process queued-up stuff */
    			if (!wasPacked) CkUnpackMessage(envptr);
			return true;
		}
#if CMK_SMP
                else if (env->getMsgtype()==NodeBocInitMsg || env->getMsgtype()==ForNodeBocMsg) {
                         // try next rank, we can't just buffer the msg and left
                         // we need to keep unprocessed msg on the fly
                        int nextpe = CkMyPe()+1;
                        if (nextpe == CkNodeFirst(CkMyNode())+CkMyNodeSize())
                        nextpe = CkNodeFirst(CkMyNode());
                        CmiSyncSendAndFree(nextpe,env->getTotalsize(),(char *)env);
                        return false;
                }
#endif
		else /*!isNext(env) */ {
			REPLAYDEBUG("Queueing message: "<<env->getSrcPe()<<" "<<env->getTotalsize()<<" "<<env->getEvent()<<" "<<env->getEpIdx()
				<<" because we wanted "<<nextPE<<" "<<nextSize<<" "<<nextEvent << " " << nextEP)
			delayedMessages.enq(env);
                        flush();
			return false;
		}
	}
	virtual bool process(CthThreadToken *token, CkCoreState *ck) {
      REPLAYDEBUG("ProcessToken token: "<<token->serialNo);
	  if (isNext(token)) {
        REPLAYDEBUG("Executing token: "<<token->serialNo)
	    getNext();
	    flush();
	    return true;
	  } else {
        REPLAYDEBUG("Queueing token: "<<token->serialNo
            <<" because we wanted "<<nextPE<<" "<<nextSize<<" "<<nextEvent)
	    delayedTokens.enq(token);
	    return false;
	  }
	}

	virtual bool process(LBMigrateMsg **msg,CkCoreState *ck) {
	  if (lbFile == NULL) lbFile = openReplayFile("ckreplay_",".lb","r");
	  if (lbFile != NULL) {
	    int num_moves = 0;
        PUP::fromDisk p(lbFile);
	    p | num_moves;
	    if (num_moves != (*msg)->n_moves) {
	      delete *msg;
	      *msg = new (num_moves,CkNumPes(),CkNumPes(),0) LBMigrateMsg;
	    }
	    (*msg)->pup(p);
	  }
	  return true;
	}
};

class CkMessageDetailReplay : public CkMessageWatcher {
  void *getNext() {
    CmiUInt4 size; size_t nread;
    if ((nread=fread(&size, 4, 1, f)) < 1) {
      if (feof(f)) return NULL;
      CkAbort("Broken record file (metadata) got %zu\n",nread);
    }
    void *env = CmiAlloc(size);
    long tell = ftell(f);
    if ((nread=fread(env, size, 1, f)) < 1) {
      CkAbort("Broken record file (data) expecting %d, got %zu (file position %ld)\n",size,nread,tell);
    }
    //*(int*)env = 0x34567890; // set first integer as magic
    return env;
  }
public:
  double starttime;
  CkMessageDetailReplay(FILE *f_) {
    f=f_;
    starttime=CkWallTimer();
    /* This must match what CkMessageDetailRecorder did */
    CmiUInt2 little;
    fread(&little, 2, 1, f);
    if (little != sizeof(void*)) {
      CkAbort("Replaying on a different architecture from which recording was done!");
    }

    CsdEnqueue(getNext());

    CcdCallOnCondition(CcdPROCESSOR_STILL_IDLE, (CcdVoidFn)CkMessageDetailReplayDone, (void*)this);
  }
  virtual bool process(envelope **env,CkCoreState *ck) {
    void *msg = getNext();
    if (msg != NULL) CsdEnqueue(msg);
    return true;
  }
};

void CkMessageReplayQuiescence(void *rep, double time) {
  CkPrintf("[%d] Quiescence detected\n",CkMyPe());
  CkMessageReplay *replay = (CkMessageReplay*)rep;
  //CmiStartQD(CkMessageReplayQuiescence, replay);
}

void CkMessageDetailReplayDone(void *rep, double time) {
  CkMessageDetailReplay *replay = (CkMessageDetailReplay *)rep;
  CkPrintf("[%d] Detailed replay finished after %f seconds. Exiting.\n",CkMyPe(),CkWallTimer()-replay->starttime);
  ConverseExit();
}
#endif

static bool CpdExecuteThreadResume(CthThreadToken *token) {
  CkCoreState *ck = CkpvAccess(_coreState);
  if (ck->watcher!=NULL) {
    return ck->watcher->processThread(token,ck);
  }
  return true;
}

CpvExtern(int, CthResumeNormalThreadIdx);
void CthResumeNormalThreadDebug(CthThreadToken* token)
{
  CthThread t = token->thread;

  if(t == NULL){
    free(token);
    return;
  }
#if CMK_TRACE_ENABLED
#if ! CMK_TRACE_IN_CHARM
  if(CpvAccess(traceOn))
    CthTraceResume(t);
/*    if(CpvAccess(_traceCoreOn)) 
            resumeTraceCore();*/
#endif
#endif
#if CMK_OMP
  CthSetPrev(t, CthSelf());
#endif
  /* For Record/Replay debugging: need to notify the upper layer that we are resuming a thread */
  if (CpdExecuteThreadResume(token)) {
    CthResume(t);
  }
#if CMK_OMP
  CthScheduledDecrement();
  CthSetPrev(CthSelf(), 0);
#endif
}

void CpdHandleLBMessage(LBMigrateMsg **msg) {
  CkCoreState *ck = CkpvAccess(_coreState);
  if (ck->watcher!=NULL) {
    ck->watcher->processLBMessage(msg, ck);
  }
}


#include "ckliststring.h"
void CkMessageWatcherInit(char **argv,CkCoreState *ck) {
    CmiArgGroup("Charm++","Record/Replay");
    bool forceReplay = false;
    char *procs = NULL;
    _replaySystem = 0;
    if (CmiGetArgFlagDesc(argv,"+recplay-crc","Enable CRC32 checksum for message record-replay")) {
      if(CmiMyRank() == 0) _recplay_crc = 1;
    }
    if (CmiGetArgFlagDesc(argv,"+recplay-xor","Enable simple XOR checksum for message record-replay")) {
      if(CmiMyRank() == 0) _recplay_checksum = 1;
    }
    int tmplogsize;
    if(CmiGetArgIntDesc(argv,"+recplay-logsize",&tmplogsize,"Specify the size of the buffer used by the message recorder"))
      {
	if(CmiMyRank() == 0) _recplay_logsize = tmplogsize;
      }
    REPLAYDEBUG("CkMessageWatcherInit ");
    if (CmiGetArgStringDesc(argv,"+record-detail",&procs,"Record full message content for the specified processors")) {
#if CMK_REPLAYSYSTEM
        CkListString list(procs);
        if (list.includes(CkMyPe())) {
          CkPrintf("Charm++> Recording full detail for processor %d\n",CkMyPe());
          CpdSetInitializeMemory(1);
          ck->addWatcher(new CkMessageDetailRecorder(openReplayFile("ckreplay_",".detail","w")));
        }
#else
        CkAbort("Option `+record-detail' requires that record-replay support be enabled at configure time (--enable-replay)");
#endif
    }
    if (CmiGetArgFlagDesc(argv,"+record","Record message processing order")) {
#if CMK_REPLAYSYSTEM
      if (CkMyPe() == 0) {
        CmiPrintf("Charm++> record mode.\n");
        if (!CmiMemoryIs(CMI_MEMORY_IS_CHARMDEBUG)) {
          CmiPrintf("Charm++> Warning: disabling recording for message integrity detection (requires linking with -memory charmdebug)\n");
          _recplay_crc = _recplay_checksum = 0;
        }
      }
      CpdSetInitializeMemory(1);
      CmiNumberHandler(CpvAccess(CthResumeNormalThreadIdx), (CmiHandler)CthResumeNormalThreadDebug);
      ck->addWatcher(new CkMessageRecorder(openReplayFile("ckreplay_",".log","w")));
#else
      CkAbort("Option `+record' requires that record-replay support be enabled at configure time (--enable-replay)");
#endif
    }
	if (CmiGetArgStringDesc(argv,"+replay-detail",&procs,"Replay the specified processors from recorded message content")) {
#if CMK_REPLAYSYSTEM
	    forceReplay = true;
	    CpdSetInitializeMemory(1);
	    // Set the parameters of the processor
#if CMK_SHARED_VARS_UNAVAILABLE
	    _Cmi_mype = atoi(procs);
	    while (procs[0]!='/') procs++;
	    procs++;
	    _Cmi_numpes = atoi(procs);
#else
	    CkAbort("+replay-detail available only for non-SMP build");
#endif
	    _replaySystem = 1;
	    ck->addWatcher(new CkMessageDetailReplay(openReplayFile("ckreplay_",".detail","r")));
#else
          CkAbort("Option `+replay-detail' requires that record-replay support be enabled at configure time (--enable-replay)");
#endif
	}
	if (CmiGetArgFlagDesc(argv,"+replay","Replay recorded message stream") || forceReplay) {
#if CMK_REPLAYSYSTEM
	  if (CkMyPe() == 0)  {
	    CmiPrintf("Charm++> replay mode.\n");
	    if (!CmiMemoryIs(CMI_MEMORY_IS_CHARMDEBUG)) {
	      CmiPrintf("Charm++> Warning: disabling message integrity detection during replay (requires linking with -memory charmdebug)\n");
	      _recplay_crc = _recplay_checksum = 0;
	    }
	  }
	  CpdSetInitializeMemory(1);
	  CmiNumberHandler(CpvAccess(CthResumeNormalThreadIdx), (CmiHandler)CthResumeNormalThreadDebug);
	  ck->addWatcher(new CkMessageReplay(openReplayFile("ckreplay_",".log","r")));
#else
          CkAbort("Option `+replay' requires that record-replay support be enabled at configure time (--enable-replay)");
#endif
	}
	if (_recplay_crc && _recplay_checksum) {
	  CmiAbort("Both +recplay-crc and +recplay-checksum options specified, only one allowed.");
	}
}

int CkMessageToEpIdx(void* msg) {
  envelope* env = UsrToEnv(msg);
  int type = env->getMsgtype();
  if (type == ArrayBcastMsg || type == ArrayBcastFwdMsg)
    return env->getsetArrayBcastEp();
  else
    return env->getEpIdx();
}

int getCharmEnvelopeSize() {
  return sizeof(envelope);
}

/// Best-effort guess at whether @arg msg points at a charm envelope
int isCharmEnvelope(void *msg) {
    envelope *e = (envelope *)msg;
    if (SIZEFIELD(msg) < sizeof(envelope)) return 0;
    if (SIZEFIELD(msg) < e->getTotalsize()) return 0;
    if (e->getTotalsize() < sizeof(envelope)) return 0;
    if (e->getEpIdx()<=0 || e->getEpIdx()>=_entryTable.size()) return 0;
#if CMK_SMP
    if (e->getSrcPe()>=CkNumPes()+CkNumNodes()) return 0;
#else
    if (e->getSrcPe()>=CkNumPes()) return 0;
#endif
    if (e->getMsgtype()<=0 || e->getMsgtype()>=LAST_CK_ENVELOPE_TYPE) return 0;
    return 1;
}

#include "CkMarshall.def.h"
