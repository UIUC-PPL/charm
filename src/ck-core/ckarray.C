/**
\file
\addtogroup CkArray

An Array is a collection of array elements (Chares) which
can be indexed by an arbitary run of bytes (a CkArrayIndex).
Elements can be inserted or removed from the array,
or migrated between processors.  Arrays are integrated with
the run-time load balancer.
Elements can also receive broadcasts and participate in
reductions.

Here's a list, valid in 2003/12, of all the different 
code paths used to create array elements:

1.) Initial inserts: all at once
CProxy_foo::ckNew(msg,n);
 CProxy_ArrayBase::ckCreateArray
  CkArray::CkArray
   CkLocMgr::populateInitial(numInitial) -> CkArrayMap::populateInitial(numInitial)
    for (idx=...)
     if (map->procNum(idx)==thisPe) 
      CkArray::insertInitial
       CkArray::prepareCtorMsg
       CkArray::insertElement
    // OR map-specific insertion logic

2.) Initial inserts: one at a time
fooProxy[idx].insert(msg,n);
 CProxy_ArrayBase::ckInsertIdx
  CkArray::prepareCtorMsg
  CkArray::insertElement

3.) Demand creation (receive side)
CkLocMgr::deliver
 CkLocMgr::deliverUnknown
  CkLocMgr::demandCreateElement
   CkArray::demandCreateElement
    CkArray::prepareCtorMsg
    CkArray::insertElement

4.) Migration (receive side)
CkLocMgr::migrateIncoming
 CkLocMgr::pupElementsFor
  CkArray::allocateMigrated



Converted from 1-D arrays 2/27/2000 by
Orion Sky Lawlor, olawlor@acm.org
*/
#include "charm++.h"
#include "register.h"
#include "ck.h"
#include "pathHistory.h"
#include "ckarray.h"
#include <stdarg.h>

bool _isAnytimeMigration;
bool _isStaticInsertion;
bool _isNotifyChildInRed;

#define ARRAY_DEBUG_OUTPUT 0

#if ARRAY_DEBUG_OUTPUT 
#   define DEB(x) CkPrintf x  //General debug messages
#   define DEBI(x) CkPrintf x  //Index debug messages
#   define DEBC(x) CkPrintf x  //Construction debug messages
#   define DEBS(x) CkPrintf x  //Send/recv/broadcast debug messages
#   define DEBM(x) CkPrintf x  //Migration debug messages
#   define DEBL(x) CkPrintf x  //Load balancing debug messages
#   define DEBK(x) CkPrintf x  //Spring Cleaning debug messages
#   define DEBB(x) CkPrintf x  //Broadcast debug messages
#   define AA "ArrayBOC on %d: "
#   define AB ,CkMyPe()
#   define DEBUG(x) x
#else
#   define DEB(X) /*CkPrintf x*/
#   define DEBI(X) /*CkPrintf x*/
#   define DEBC(X) /*CkPrintf x*/
#   define DEBS(x) /*CkPrintf x*/
#   define DEBM(X) /*CkPrintf x*/
#   define DEBL(X) /*CkPrintf x*/
#   define DEBK(x) /*CkPrintf x*/
#   define DEBB(x) /*CkPrintf x*/
#   define str(x) /**/
#   define DEBUG(x)
#endif

///This arrayListener is in charge of performing reductions on the array.
class CkArrayReducer : public CkArrayListener {
  CkGroupID mgrID;
  CkReductionMgr *mgr;
  typedef  contributorInfo *I;
  inline contributorInfo *getData(ArrayElement *el)
    {return (I)ckGetData(el);}
public:
  /// Attach this array to this CkReductionMgr
  CkArrayReducer(CkGroupID mgrID_);
  CkArrayReducer(CkMigrateMessage *m);
  virtual void pup(PUP::er &p);
  virtual ~CkArrayReducer();
  PUPable_decl(CkArrayReducer);

  void ckBeginInserting(void) {mgr->creatingContributors();}
  void ckEndInserting(void) {mgr->doneCreatingContributors();}

  void ckElementStamp(int *eltInfo) {mgr->contributorStamped((I)eltInfo);}

  void ckElementCreating(ArrayElement *elt)
    {mgr->contributorCreated(getData(elt));}
  void ckElementDied(ArrayElement *elt)
    {mgr->contributorDied(getData(elt));}

  void ckElementLeaving(ArrayElement *elt)
    {mgr->contributorLeaving(getData(elt));}
  bool ckElementArriving(ArrayElement *elt)
    {mgr->contributorArriving(getData(elt)); return true; }
};

/*
void 
CProxyElement_ArrayBase::ckSendWrapper(void *me, void *m, int ep, int opts){
       ((CProxyElement_ArrayBase*)me)->ckSend((CkArrayMessage*)m,ep,opts);
}
*/
void
CProxyElement_ArrayBase::ckSendWrapper(CkArrayID _aid, CkArrayIndex _idx, void *m, int ep, int opts) {
	CProxyElement_ArrayBase me = CProxyElement_ArrayBase(_aid,_idx);
	((CProxyElement_ArrayBase)me).ckSend((CkArrayMessage*)m,ep,opts);
}

/*********************** CkVerboseListener ******************/
#define VL_PRINT ckout<<"VerboseListener on PE "<<CkMyPe()<<" > "

CkVerboseListener::CkVerboseListener(void)
  :CkArrayListener(0)
{
  VL_PRINT<<"INIT  Creating listener"<<endl;
}

void CkVerboseListener::ckRegister(CkArray *arrMgr,int dataOffset_)
{
  CkArrayListener::ckRegister(arrMgr,dataOffset_);
  VL_PRINT<<"INIT  Registering array manager at offset "<<dataOffset_<<endl;
}
void CkVerboseListener::ckBeginInserting(void)
{
  VL_PRINT<<"INIT  Begin inserting elements"<<endl;
}
void CkVerboseListener::ckEndInserting(void)
{
  VL_PRINT<<"INIT  Done inserting elements"<<endl;
}

void CkVerboseListener::ckElementStamp(int *eltInfo)
{
  VL_PRINT<<"LIFE  Stamping element"<<endl;
}
void CkVerboseListener::ckElementCreating(ArrayElement *elt)
{
  VL_PRINT<<"LIFE  About to create element "<<idx2str(elt)<<endl;
}
bool CkVerboseListener::ckElementCreated(ArrayElement *elt)
{
  VL_PRINT<<"LIFE  Created element "<<idx2str(elt)<<endl;
  return true;
}
void CkVerboseListener::ckElementDied(ArrayElement *elt)
{
  VL_PRINT<<"LIFE  Deleting element "<<idx2str(elt)<<endl;
}

void CkVerboseListener::ckElementLeaving(ArrayElement *elt)
{
  VL_PRINT<<"MIG  Leaving: element "<<idx2str(elt)<<endl;
}
bool CkVerboseListener::ckElementArriving(ArrayElement *elt)
{
  VL_PRINT<<"MIG  Arriving: element "<<idx2str(elt)<<endl;
  return true;
}

//Iterate over the CkArrayListeners in this vector, calling "inside" each time.
#define CK_ARRAYLISTENER_LOOP(listVec,inside) \
  do { \
	int lIdx,lMax=listVec.size();\
	for (lIdx=0;lIdx<lMax;lIdx++) { \
		CkArrayListener *l=listVec[lIdx];\
		inside;\
	}\
  } while(0)

/************************* ArrayElement *******************/
class ArrayElement_initInfo {
public:
  CkArray *thisArray;
  CkArrayID thisArrayID;
  CkArrayIndex numInitial;
  int listenerData[CK_ARRAYLISTENER_MAXLEN];
  bool fromMigration;
};

CkpvStaticDeclare(ArrayElement_initInfo,initInfo);

void ArrayElement::initBasics(void)
{
#if CMK_OUT_OF_CORE
  if (CkpvAccess(CkSaveRestorePrefetch)) 
    return; /* Just restoring from disk--don't try to set up anything. */
#endif
#if CMK_GRID_QUEUE_AVAILABLE
	grid_queue_interval = 0;
	grid_queue_threshold = 0;
	msg_count = 0;
	msg_count_grid = 0;
	border_flag = 0;

	grid_queue_interval = CmiGridQueueGetInterval ();
	grid_queue_threshold = CmiGridQueueGetThreshold ();
#endif
  ArrayElement_initInfo &info=CkpvAccess(initInfo);
  thisArray=info.thisArray;
  thisArrayID=info.thisArrayID;
  numInitialElements=info.numInitial.getCombinedCount();
  memcpy(listenerData,info.listenerData,sizeof(listenerData));
  if (!info.fromMigration) {
    CK_ARRAYLISTENER_LOOP(thisArray->listeners,
			  l->ckElementCreating(this));
  }
#ifdef _PIPELINED_ALLREDUCE_
	allredMgr = NULL;
#endif
  DEBC((AA "Inserting %llu into PE level hashtable\n" AB, ckGetID().getID()));
  CkpvAccess(array_objs)[ckGetID().getID()] = this;
}

ArrayElement::ArrayElement(void) 
{
	initBasics();
#if CMK_MEM_CHECKPOINT
        init_checkpt();
#endif
}

ArrayElement::ArrayElement(CkMigrateMessage *m) : CkMigratable(m)
{
	initBasics();
}

//Called by the system just before and after migration to another processor:  
void ArrayElement::ckAboutToMigrate(void) {
	CK_ARRAYLISTENER_LOOP(thisArray->listeners,
				l->ckElementLeaving(this));
	CkMigratable::ckAboutToMigrate();
}
void ArrayElement::ckJustMigrated(void) {
	CkMigratable::ckJustMigrated();
	CK_ARRAYLISTENER_LOOP(thisArray->listeners,
	      if (!l->ckElementArriving(this)) return;);
}

void ArrayElement::ckJustRestored(void) {
    CkMigratable::ckJustRestored();
    //empty for out-of-core emulation
}

#ifdef _PIPELINED_ALLREDUCE_
void ArrayElement::contribute2(int dataSize,const void *data,CkReduction::reducerType type,
					CMK_REFNUM_TYPE userFlag)
{
	CkReductionMsg *msg=CkReductionMsg::buildNew(dataSize,data,type);
	msg->setUserFlag(userFlag);
	msg->setMigratableContributor(true);
	thisArray->contribute(&*(contributorInfo *)&listenerData[thisArray->reducer->ckGetOffset()],msg);
}
void ArrayElement::contribute2(int dataSize,const void *data,CkReduction::reducerType type,
					const CkCallback &cb,CMK_REFNUM_TYPE userFlag)
{
	CkReductionMsg *msg=CkReductionMsg::buildNew(dataSize,data,type);
	msg->setUserFlag(userFlag);
	msg->setCallback(cb);
	msg->setMigratableContributor(true);
	thisArray->contribute(&*(contributorInfo *)&listenerData[thisArray->reducer->ckGetOffset()],msg);
}
void ArrayElement::contribute2(CkReductionMsg *msg) 
{
	msg->setMigratableContributor(true);
	thisArray->contribute(&*(contributorInfo *)&listenerData[thisArray->reducer->ckGetOffset()],msg);
}
void ArrayElement::contribute2(const CkCallback &cb,CMK_REFNUM_TYPE userFlag)
{
    CkReductionMsg *msg=CkReductionMsg::buildNew(0,NULL,CkReduction::nop);
    msg->setUserFlag(userFlag);
    msg->setCallback(cb);
    msg->setMigratableContributor(true);
    thisArray->contribute(&*(contributorInfo *)&listenerData[thisArray->reducer->ckGetOffset()],msg);
}
void ArrayElement::contribute2(CMK_REFNUM_TYPE userFlag)
{
    CkReductionMsg *msg=CkReductionMsg::buildNew(0,NULL,CkReduction::nop);
    msg->setUserFlag(userFlag);
    msg->setMigratableContributor(true);
    thisArray->contribute(&*(contributorInfo *)&listenerData[thisArray->reducer->ckGetOffset()],msg);
}

void ArrayElement::contribute2(CkArrayIndex myIndex, int dataSize,const void *data,CkReduction::reducerType type,
							  const CkCallback &cb,CMK_REFNUM_TYPE userFlag)
{
	// if it is a broadcast to myself and size is large
	if(cb.type==CkCallback::bcastArray && cb.d.array.id==thisArrayID && dataSize>FRAG_THRESHOLD) 
	{
		if (!allredMgr) {
			allredMgr = new AllreduceMgr();
		}
		// number of fragments
		int fragNo = dataSize/FRAG_SIZE;
		int size = FRAG_SIZE;
		// for each fragment
		for (int i=0; i<fragNo; i++) {
			// callback to defragmentor
			CkCallback defrag_cb(CkIndex_ArrayElement::defrag(NULL), thisArrayID);
			if ((0 != i) && ((fragNo-1) == i) && (0 != dataSize%FRAG_SIZE)) {
				size = dataSize%FRAG_SIZE;
			}
			CkReductionMsg *msg = CkReductionMsg::buildNew(size, (char*)data+i*FRAG_SIZE);
			// initialize the new msg
			msg->reducer            = type;
			msg->nFrags             = fragNo;
			msg->fragNo             = i;
			msg->callback           = defrag_cb;
			msg->userFlag           = userFlag;
			allredMgr->cb		= cb;
			allredMgr->cb.type	= CkCallback::sendArray;
			allredMgr->cb.d.array.idx = myIndex;
			contribute2(msg);
		}
		return;
	}
	CkReductionMsg *msg=CkReductionMsg::buildNew(dataSize,data,type);
	msg->setUserFlag(userFlag);
	msg->setCallback(cb);
	msg->setMigratableContributor(true);
	thisArray->contribute(&*(contributorInfo *)&listenerData[thisArray->reducer->ckGetOffset()],msg);
}


#else
CK_REDUCTION_CONTRIBUTE_METHODS_DEF(ArrayElement,thisArray,
   *(contributorInfo *)&listenerData[thisArray->reducer->ckGetOffset()],true)
#endif
// _PIPELINED_ALLREDUCE_
void ArrayElement::defrag(CkReductionMsg *msg)
{
//	CkPrintf("in defrag\n");
#ifdef _PIPELINED_ALLREDUCE_
	allredMgr->allreduce_recieve(msg);
#endif
}

int ArrayElement::getRedNo(void) const
{
  return ((contributorInfo *)&listenerData[thisArray->reducer->ckGetOffset()])->redNo;
}

// Remote method: This removes the array element from its array manager which
// also calls delete on this element. The superclass destructor then handles
// cleanup of the associated location record from CkLocMgr.
void ArrayElement::ckDestroy(void)
{
	    CK_ARRAYLISTENER_LOOP(thisArray->listeners,
			   l->ckElementDied(this));
	thisArray->deleteElt(CkMigratable::ckGetID());
}

//Destructor (virtual)
ArrayElement::~ArrayElement()
{
#if CMK_OUT_OF_CORE
  if (CkpvAccess(CkSaveRestorePrefetch)) 
    return; /* Just saving to disk--don't trash anything. */
#endif
  // Erase from PE level hashtable for quick receives
  DEBC((AA "Removing %llu from PE level hashtable\n" AB, ckGetID().getID()));
  CkpvAccess(array_objs).erase(ckGetID().getID());
  //To detect use-after-delete: 
  thisArray=(CkArray *)(intptr_t)0xDEADa7a1;
}

void ArrayElement::pup(PUP::er &p)
{
  DEBM((AA "  ArrayElement::pup()\n" AB));
  CkMigratable::pup(p);
  thisArrayID.pup(p);
  if (p.isUnpacking())
  	thisArray=thisArrayID.ckLocalBranch();
  p(listenerData,CK_ARRAYLISTENER_MAXLEN);
#if CMK_MEM_CHECKPOINT
  p(budPEs, 2);
#endif
  p.syncComment(PUP::sync_last_system,"ArrayElement");
#if CMK_GRID_QUEUE_AVAILABLE
  p|grid_queue_interval;
  p|grid_queue_threshold;
  p|msg_count;
  p|msg_count_grid;
  p|border_flag;
  if (p.isUnpacking ()) {
    msg_count = 0;
    msg_count_grid = 0;
    border_flag = 0;
  }
#endif
}

char *ArrayElement::ckDebugChareName(void) {
	char buf[200];
	const char *className=_chareTable[ckGetChareType()]->name;
	const int *d=thisIndexMax.data();
	const short int *s=(const short int*)d;
	switch (thisIndexMax.dimension) {
	case 0:	sprintf(buf,"%s",className); break;
	case 1: sprintf(buf,"%s[%d]",className,d[0]); break;
	case 2: sprintf(buf,"%s(%d,%d)",className,d[0],d[1]); break;
	case 3: sprintf(buf,"%s(%d,%d,%d)",className,d[0],d[1],d[2]); break;
    case 4: sprintf(buf,"%s(%hd,%hd,%hd,%hd)",className,s[0],s[1],s[2],s[3]); break;
    case 5: sprintf(buf,"%s(%hd,%hd,%hd,%hd,%hd)",className,s[0],s[1],s[2],s[3],s[4]); break;
    case 6: sprintf(buf,"%s(%hd,%hd,%hd,%hd,%hd,%hd)",className,s[0],s[1],s[2],s[3],s[4],s[5]); break;
	default: sprintf(buf,"%s(%d,%d,%d,%d..)",className,d[0],d[1],d[2],d[3]); break;
	};
	return strdup(buf);
}

int ArrayElement::ckDebugChareID(char *str, int limit) {
  if (limit<21) return -1;
  str[0] = 2;
  *((int*)&str[1]) = ((CkGroupID)thisArrayID).idx;
  *((CkArrayIndex*)&str[5]) = thisIndexMax;
  return 21;
}

/// A more verbose form of abort
void ArrayElement::CkAbort(const char *format, ...) const
{
	char newmsg[256];
	va_list args;
	va_start(args, format);
	vsnprintf(newmsg, sizeof(newmsg), format, args);
	va_end(args);

	CkMigratable::CkAbort("[%d] Array element at index %s aborting:\n%s", CkMyPe(), idx2str(thisIndexMax), newmsg);
}

void ArrayElement::recvBroadcast(CkMessage *m){
}

#if CMK_CHARM4PY

ArrayElemExt::ArrayElemExt(void *impl_msg)
{
  int chareIdx = ckGetChareType();
  ctorEpIdx = _chareTable[chareIdx]->getDefaultCtor();
  //printf("Constructor of ArrayElemExt, aid=%d, chareIdx=%d, ctorEpIdx=%d\n", ((CkGroupID)thisArrayID).idx, chareIdx, ctorEpIdx);
  char *impl_buf = ck::get_message_buffer((ck::marshall_msg)impl_msg);
  PUP::fromMem implP(impl_buf);
  implP|usesAtSync;
  int msgSize; implP|msgSize;
  int dcopy_start; implP|dcopy_start;

  ArrayMsgRecvExtCallback(((CkGroupID)thisArrayID).idx, int(thisIndexMax.getDimension()),
                          thisIndexMax.data(), ctorEpIdx,
                          msgSize, impl_buf+(2*sizeof(int))+sizeof(char), dcopy_start);
}

#endif

/*********************** Spring Cleaning *****************
Periodically (every minute or so) remove expired broadcasts
from the queue.

This does not get called for arrays with stable locations (all
insertions done at creation, migration only at discrete points).
*/

inline void CkArray::springCleaning(void)
{
  DEBK((AA "Starting spring cleaning\n" AB));
  broadcaster->springCleaning();
  setupSpringCleaning();
}

void CkArray::staticSpringCleaning(void *forArray,double curWallTime) {
	((CkArray *)forArray)->springCleaning();
}

void CkArray::setupSpringCleaning() {
 // set up broadcast cleaner
 if (!stableLocations)
      springCleaningCcd = CcdCallOnCondition(CcdPERIODIC_1minute,
                                             staticSpringCleaning, (void *)this);
}

/********************* Little CkArray Utilities ******************/

CProxy_ArrayBase::CProxy_ArrayBase(const ArrayElement *e)
	:CProxy(), _aid(e->ckGetArrayID())
	{}
CProxyElement_ArrayBase::CProxyElement_ArrayBase(const ArrayElement *e)
	:CProxy_ArrayBase(e), _idx(e->ckGetArrayIndex())
	{}

CProxySection_ArrayBase::CProxySection_ArrayBase(const CkArrayID &aid, const CkArrayIndex *elems, const int nElems, int factor) :CProxy_ArrayBase(aid) {
    _sid.emplace_back(aid, elems, nElems, factor);
}

CProxySection_ArrayBase::CProxySection_ArrayBase(const CkArrayID &aid, const std::vector<CkArrayIndex> &elems, int factor) :CProxy_ArrayBase(aid) {
    _sid.emplace_back(aid, elems, factor);
}

CProxySection_ArrayBase::CProxySection_ArrayBase(const int n, const CkArrayID *aid, CkArrayIndex const * const *elems, const int *nElems, int factor) :CProxy_ArrayBase(aid[0]) {
    _sid.resize(n);
    for (int i=0; i<_sid.size(); i++) {
      _sid[i] = CkSectionID(aid[i], elems[i], nElems[i], factor);
    }
}

CProxySection_ArrayBase::CProxySection_ArrayBase(const std::vector<CkArrayID> &aid, const std::vector<std::vector<CkArrayIndex> > &elems, int factor) :CProxy_ArrayBase(aid[0]) {
    _sid.resize(aid.size());
    for (int i=0; i<_sid.size(); i++) {
      _sid[i] = CkSectionID(aid[i], elems[i], factor);
    }
}


void CProxySection_ArrayBase::ckAutoDelegate(int opts){
    if(_sid.empty())
      CmiAbort("Auto Delegation before setting up CkSectionID\n");
    CkArray *ckarr = CProxy_CkArray(_sid[0].get_aid()).ckLocalBranch();
    if(ckarr->isSectionAutoDelegated()){
    	CkMulticastMgr *mCastGrp = CProxy_CkMulticastMgr(ckarr->getmCastMgr()).ckLocalBranch();
    	ckSectionDelegate(mCastGrp, opts);
    }
}


void CProxySection_ArrayBase::setReductionClient(CkCallback *cb) {
    if(_sid.empty())
      CmiAbort("setReductionClient before setting up CkSectionID\n");
    CkArray *ckarr = CProxy_CkArray(_sid[0].get_aid()).ckLocalBranch();
    if(ckarr->isSectionAutoDelegated()){
      CkMulticastMgr *mCastGrp = CProxy_CkMulticastMgr(ckarr->getmCastMgr()).ckLocalBranch();
      mCastGrp->setReductionClient(*this, cb);
    }
    else{
      CmiAbort("setReductionClient called on section without autoDelegate");
    }
}


void CProxySection_ArrayBase::resetSection(){
    if(_sid.empty())
      CmiAbort("resetSection before setting up CkSectionID\n");
    CkArray *ckarr = CProxy_CkArray(_sid[0].get_aid()).ckLocalBranch();
    if(ckarr->isSectionAutoDelegated()){
      CkMulticastMgr *mCastGrp = CProxy_CkMulticastMgr(ckarr->getmCastMgr()).ckLocalBranch();
      mCastGrp->resetSection(*this);
    }
    else{
      CmiAbort("resetSection called on section without autoDelegate");
    }
}

CkLocMgr *CProxy_ArrayBase::ckLocMgr(void) const
	{return ckLocalBranch()->getLocMgr(); }

CK_REDUCTION_CLIENT_DEF(CProxy_ArrayBase,ckLocalBranch())

static CkArrayID CkCreateArray(CkArrayMessage *m, int ctor, CkArrayOptions opts)
{
  CkAssert(CkMyPe() == 0);

  CkGroupID locMgr = opts.getLocationManager();
  if (locMgr.isZero())
  { //Create a new location manager
    CkEntryOptions  e_opts;
    e_opts.setGroupDepID(opts.getMap());       // group creation dependence
    locMgr = CProxy_CkLocMgr::ckNew(opts, &e_opts);
    opts.setLocationManager(locMgr);
  }
  CkGroupID mCastMgr = opts.getMcastManager();
  if (opts.isSectionAutoDelegated() && mCastMgr.isZero())
  { //Create a new multicast manager
    CkEntryOptions  e_opts;
    e_opts.setGroupDepID(locMgr);       // group creation dependence
    //call with default parameters, since the last parameter has to be e_opts
    mCastMgr = CProxy_CkMulticastMgr::ckNew(2, 8192, 8192, &e_opts);
    opts.setMcastManager(mCastMgr);
  }
  //Create the array manager
  m->array_ep()=ctor;
  CkMarshalledMessage marsh(m);
  CkEntryOptions  e_opts;
  e_opts.setGroupDepID(locMgr);       // group creation dependence
  if(opts.isSectionAutoDelegated())
  {
    e_opts.setGroupDepID(mCastMgr);
  }

  // Add user defined group dependencies
  envelope *env = UsrToEnv(m);
  for(int i=0; i<env->getGroupDepNum(); i++) {
    e_opts.addGroupDepID(env->getGroupDep(i));
  }
  CkGroupID ag=CProxy_CkArray::ckNew(opts,marsh,&e_opts);
  return (CkArrayID)ag;
}

CkArrayID CProxy_ArrayBase::ckCreateArray(CkArrayMessage *m,int ctor,
					  const CkArrayOptions &opts)
{
  return CkCreateArray(m, ctor, opts);
}

CkArrayID CProxy_ArrayBase::ckCreateEmptyArray(CkArrayOptions opts)
{
  return ckCreateArray((CkArrayMessage *)CkAllocSysMsg(),0,opts);
}

void CProxy_ArrayBase::ckCreateEmptyArrayAsync(CkCallback cb, CkArrayOptions opts)
{
  CkSendAsyncCreateArray(0, cb, opts, (CkArrayMessage *)CkAllocSysMsg());
}

extern IrrGroup *lookupGroupAndBufferIfNotThere(CkCoreState *ck,envelope *env,const CkGroupID &groupID);

struct CkInsertIdxMsg {
  char core[CmiReservedHeaderSize];
  CkArrayIndex idx;
  CkArrayMessage *m;
  int ctor;
  int onPe;
  CkArrayID _aid;
};

static int ckinsertIdxHdl;

void ckinsertIdxFunc(void *m)
{
  CkInsertIdxMsg *msg = (CkInsertIdxMsg *)m;
  CProxy_ArrayBase   ca(msg->_aid);
  ca.ckInsertIdx(msg->m, msg->ctor, msg->onPe, msg->idx);
  CmiFree(msg);
}

void CProxy_ArrayBase::ckInsertIdx(CkArrayMessage *m,int ctor,int proposedPe,
	const CkArrayIndex &idx)
{
  if (m==NULL) m=(CkArrayMessage *)CkAllocSysMsg();
  m->array_ep()=ctor;
  CkArray *ca = ckLocalBranch();
  if (ca == NULL) {
      CkInsertIdxMsg *msg = (CkInsertIdxMsg *)CmiAlloc(sizeof(CkInsertIdxMsg));
      msg->idx = idx;
      msg->m = m;
      msg->ctor = ctor;
      msg->onPe = proposedPe;
      msg->_aid = _aid;
      CmiSetHandler(msg, ckinsertIdxHdl);
      ca = (CkArray *)lookupGroupAndBufferIfNotThere(CkpvAccess(_coreState), (envelope*)msg,_aid);
      CkAssert (ca == NULL);
      return;
  }

  int hostPe = ca->findInitialHostPe(idx, proposedPe);

  int listenerData[CK_ARRAYLISTENER_MAXLEN];
  ca->prepareCtorMsg(m, listenerData);
  if (ckIsDelegated()) {
    ckDelegatedTo()->ArrayCreate(ckDelegatedPtr(),ctor,m,idx,hostPe,_aid);
    return;
  }
  
  DEBC((AA "Proxy inserting element %s on Pe %d\n" AB,idx2str(idx),hostPe));
  CProxy_CkArray(_aid)[hostPe].insertElement(m, idx, listenerData);
}

void CProxyElement_ArrayBase::ckInsert(CkArrayMessage *m,int ctorIndex,int onPe)
{
  ckInsertIdx(m,ctorIndex,onPe,_idx);
}

ArrayElement *CProxyElement_ArrayBase::ckLocal(void) const
{
  return ckLocalBranch()->lookup(_idx);
}

//pack-unpack method for CProxy_ArrayBase
void CProxy_ArrayBase::pup(PUP::er &p)
{
  CProxy::pup(p);
  _aid.pup(p);
}
void CProxyElement_ArrayBase::pup(PUP::er &p)
{
  CProxy_ArrayBase::pup(p);
  p|_idx;
}

void CProxySection_ArrayBase::pup(PUP::er &p)
{
  CProxy_ArrayBase::pup(p);
  p | _sid;
}

/*
 * Message type and code to create new chare arrays asynchronously.
 * Post-startup, whatever non-0 PE calls for the creation of an array will pack
 * up all of the arguments and send them to PE 0. PE 0 will then run the normal
 * creation process and send the array ID to the provided callback. This
 * ensures that up to the limit of available bits, array IDs can be represented
 * as part of a compound fixed-size ID for their elements.
 */
struct CkCreateArrayAsyncMsg : public CMessage_CkCreateArrayAsyncMsg {
  int ctor;
  CkCallback cb;
  CkArrayOptions opts;
  char *ctorPayload;

  CkCreateArrayAsyncMsg(int ctor_, CkCallback cb_, CkArrayOptions opts_)
    : ctor(ctor_), cb(cb_), opts(opts_)
  { }
};

static int ckArrayCreationHdl = 0;

void CkSendAsyncCreateArray(int ctor, CkCallback cb, CkArrayOptions opts, void *ctorMsg)
{
  CkAssert(ctorMsg);
  UsrToEnv(ctorMsg)->setMsgtype(ArrayEltInitMsg);
  PUP::sizer ps;
  CkPupMessage(ps, &ctorMsg);
  CkCreateArrayAsyncMsg *msg = new (ps.size()) CkCreateArrayAsyncMsg(ctor, cb, opts);
  PUP::toMem p(msg->ctorPayload);
  CkPupMessage(p, &ctorMsg);
  CkFreeMsg(ctorMsg);
  envelope *env = UsrToEnv(msg);
  CmiSetHandler(env, ckArrayCreationHdl);
  CkPackMessage(&env);
  CmiSyncSendAndFree(0, env->getTotalsize(), (char*)env);
}

static void CkCreateArrayAsync(void *vmsg)
{
  envelope *venv = static_cast<envelope*>(vmsg);
  CkUnpackMessage(&venv);
  CkCreateArrayAsyncMsg *msg = static_cast<CkCreateArrayAsyncMsg*>(EnvToUsr(venv));

  // Unpack arguments
  PUP::fromMem p(msg->ctorPayload);
  void *vm;
  CkPupMessage(p, &vm);
  CkArrayMessage *m = static_cast<CkArrayMessage*>(vm);

  CkArrayID aid = CkCreateArray(m, msg->ctor, msg->opts);

  // Does the caller care about the constructed array ID?
  if (!msg->cb.isInvalid())
    msg->cb.send(new CkArrayCreatedMsg(aid));
  delete msg;
}

/*********************** CkArray Creation *************************/
void _ckArrayInit(void)
{
  CkpvInitialize(ArrayElement_initInfo,initInfo);
  CkDisableTracing(CkIndex_CkArray::insertElement(0, CkArrayIndex(), 0));
  CkDisableTracing(CkIndex_CkArray::recvBroadcast(0));
    // disable because broadcast listener may deliver broadcast message
  CkDisableTracing(CkIndex_CkLocMgr::immigrate(0));
  // by default anytime migration is allowed
  CmiAssignOnce(&ckinsertIdxHdl, CkRegisterHandler(ckinsertIdxFunc));
  CmiAssignOnce(&ckArrayCreationHdl, CkRegisterHandler(CkCreateArrayAsync));
}

CkArray::CkArray(CkArrayOptions &&opts,
		 CkMarshalledMessage &&initMsg)
  : locMgr(CProxy_CkLocMgr::ckLocalBranch(opts.getLocationManager())),
    locMgrID(opts.getLocationManager()),
    mCastMgrID(opts.getMcastManager()),
    sectionAutoDelegate(opts.isSectionAutoDelegated()),
    initCallback(opts.getInitCallback()),
    thisProxy(thisgroup),
    stableLocations(opts.staticInsertion && !opts.anytimeMigration),
    numInitial(opts.getNumInitial()), isInserting(true), numPesInited(0)
{
  // Register with our location manager
  locMgr->addManager(thisgroup,this);

  setupSpringCleaning();
  
  //set the field in one my parent class (CkReductionMgr)
  if(opts.disableNotifyChildInRed)
	  disableNotifyChildrenStart = true; 
  
  //Find, register, and initialize the arrayListeners
  listenerDataOffset=0;
  broadcaster=new CkArrayBroadcaster(stableLocations, opts.broadcastViaScheduler);
  addListener(broadcaster);
  reducer=new CkArrayReducer(thisgroup);
  addListener(reducer);

  // COMLIB HACK
  //calistener = new ComlibArrayListener();
  //addListener(calistener,dataOffset);

  int lNo,nL=opts.getListeners(); //User-added listeners
  for (lNo=0;lNo<nL;lNo++) addListener(opts.getListener(lNo));

  for (int l=0;l<listeners.size();l++) listeners[l]->ckBeginInserting();

  ///Set up initial elements (if any)
  locMgr->populateInitial(opts,initMsg.getMessage(),this);
  if (opts.staticInsertion)
    initDone();



  if (opts.reductionClient.type != CkCallback::invalid && CkMyPe() == 0)
      ckSetReductionClient(&opts.reductionClient);
}

CkArray::CkArray(CkMigrateMessage *m)
	:CkReductionMgr(m), thisProxy(thisgroup)
{
  locMgr=NULL;
  isInserting=true;
}

CkArray::~CkArray()
{
  if (!stableLocations)
    CcdCancelCallOnCondition(CcdPERIODIC_1minute, springCleaningCcd);
}

#if CMK_ERROR_CHECKING
inline void testPup(PUP::er &p,int shouldBe) {
  int a=shouldBe;
  p|a;
  if (a!=shouldBe)
    CkAbort("PUP direction mismatch!");
}
#else
inline void testPup(PUP::er &p,int shouldBe) {}
#endif

void CkArray::pup(PUP::er &p){
	CkReductionMgr::pup(p);
	p|numInitial;
	p|locMgrID;
	p|mCastMgrID;
	p|sectionAutoDelegate;
	p|initCallback;
	p|listeners;
	p|listenerDataOffset;
        p|stableLocations;
	p|numPesInited;
	testPup(p,1234);
	if(p.isUnpacking()){
		thisProxy=thisgroup;
		locMgr = CProxy_CkLocMgr::ckLocalBranch(locMgrID);
		locMgr->addManager(thisgroup,this);
		/// Restore our default listeners:
		broadcaster=(CkArrayBroadcaster *)(CkArrayListener *)(listeners[0]);
		reducer=(CkArrayReducer *)(CkArrayListener *)(listeners[1]);
                setupSpringCleaning();
	}
}

#define CK_ARRAYLISTENER_STAMP_LOOP(listenerData) do {\
  int dataOffset=0; \
  for (int lNo=0;lNo<listeners.size();lNo++) { \
    CkArrayListener *l=listeners[lNo]; \
    l->ckElementStamp(&listenerData[dataOffset]); \
    dataOffset+=l->ckGetLen(); \
  } \
} while (0)

//Called on send side to prepare array constructor message
void CkArray::prepareCtorMsg(CkMessage *m, int *listenerData)
{
  envelope *env=UsrToEnv((void *)m);
  env->setMsgtype(ArrayEltInitMsg);
  CK_ARRAYLISTENER_STAMP_LOOP(listenerData);
}

int CkArray::findInitialHostPe(const CkArrayIndex &idx, int proposedPe)
{
  int hostPe = locMgr->whichPE(idx);

  if (hostPe == -1 && proposedPe == -1)
    return procNum(idx);
  if (hostPe == -1)
    return proposedPe;
  if (proposedPe == -1)
    return hostPe;
  if (hostPe == proposedPe)
    return hostPe;

  CkAbort("hostPe for a bound element disagrees with an explicit proposedPe");
  return -1;
}

void CkArray::stampListenerData(CkMigratable *elt)
{
  ArrayElement *elt2 = (ArrayElement *)elt;
  CK_ARRAYLISTENER_STAMP_LOOP(elt2->listenerData);
}

CkMigratable *CkArray::allocateMigrated(int elChareType, CkElementCreation_t type)
{
	ArrayElement *ret=allocate(elChareType, NULL, true, NULL);
	return ret;
}

ArrayElement *CkArray::allocate(int elChareType, CkMessage *msg, bool fromMigration, int *listenerData)
{
	//Stash the element's initialization information in the global "initInfo"
	ArrayElement_initInfo &init=CkpvAccess(initInfo);
	init.numInitial=numInitial;
	init.thisArray=this;
	init.thisArrayID=thisgroup;
	if (listenerData) /*Have to *copy* data because msg will be deleted*/
	  memcpy(init.listenerData, listenerData, sizeof(init.listenerData));
	init.fromMigration=fromMigration;
	
	//Build the element
	size_t elSize=_chareTable[elChareType]->size;
	ArrayElement *elem = (ArrayElement *)malloc(elSize);
	if (elem!=NULL) setMemoryTypeChare(elem);
	return elem;
}

void CkArray::insertElement(CkMarshalledMessage &&m, const CkArrayIndex &idx, int listenerData[CK_ARRAYLISTENER_MAXLEN])
{
  insertElement((CkArrayMessage*)m.getMessage(), idx, listenerData);
}

/// This method is called by ck.C or the user to add an element.
bool CkArray::insertElement(CkArrayMessage *me, const CkArrayIndex &idx, int listenerData[CK_ARRAYLISTENER_MAXLEN])
{
  CK_MAGICNUMBER_CHECK
  int onPe;
  if (locMgr->isRemote(idx,&onPe)) 
  { /* element's sibling lives somewhere else, so insert there */
    thisProxy[onPe].insertElement(me, idx, listenerData);
    return false;
  }
  int ctorIdx = me->array_ep();
  int chareType=_entryTable[ctorIdx]->chareIdx;
  ArrayElement *elt=allocate(chareType, me, false, listenerData);
  if (!locMgr->addElement(thisgroup, idx, elt, ctorIdx, (void *)me)) return false;
  CK_ARRAYLISTENER_LOOP(listeners,
      if (!l->ckElementCreated(elt)) return false;);
  return true;
}

void CkArray::initDone(void) {
  if (initCallback.isInvalid())
    return;

  numPesInited++;
  DEBC(("PE %d initDone, numPesInited %d, treeKids %d, parent %d\n",
       CkMyPe(), numPesInited, treeKids(), treeParent()));

  // Re-use the spanning tree for reductions over the array elements
  // The "+1" is for the PE itself
  if (numPesInited == treeKids() + 1) {
    if (hasParent())
      thisProxy[treeParent()].initDone();
    else
      initCallback.send(CkReductionMsg::buildNew(0, NULL));
  }
}

void CProxy_ArrayBase::doneInserting(void)
{
  DEBC((AA "Broadcasting a doneInserting request\n" AB));
  //Broadcast a DoneInserting
  CProxy_CkArray(_aid).remoteDoneInserting();
}

void CProxy_ArrayBase::beginInserting(void)
{
  DEBC((AA "Broadcasting a beginInserting request\n" AB));
  CProxy_CkArray(_aid).remoteBeginInserting();
}

void CkArray::doneInserting(void)
{
  thisProxy[CkMyPe()].remoteDoneInserting();
}

void CkArray::beginInserting(void)
{
  thisProxy[CkMyPe()].remoteBeginInserting();
}

/// This is called on every processor after the last array insertion.
void CkArray::remoteDoneInserting(void)
{
  CK_MAGICNUMBER_CHECK
  if (isInserting) {
    isInserting=false;
    DEBC((AA "Done inserting objects\n" AB));
    for (int l=0;l<listeners.size();l++) listeners[l]->ckEndInserting();
    locMgr->doneInserting();
    initDone();
  }
}

void CkArray::remoteBeginInserting(void)
{
  CK_MAGICNUMBER_CHECK;

  if (!isInserting) {
    isInserting = true;
    DEBC((AA "Begin inserting objects\n" AB));
    for (int l=0;l<listeners.size();l++) listeners[l]->ckBeginInserting();
    locMgr->startInserting();
  }
}

void CkArray::demandCreateElement(const CkArrayIndex &idx, int ctor, CkDeliver_t type)
{
	CkArrayMessage *m=(CkArrayMessage *)CkAllocSysMsg();
        envelope *env = UsrToEnv(m);
        env->setMsgtype(ArrayEltInitMsg);
        env->setArrayMgr(thisgroup);
        int listenerData[CK_ARRAYLISTENER_MAXLEN];
	prepareCtorMsg(m, listenerData);
	m->array_ep()=ctor;

        DEBC((AA "Demand-creating %s\n" AB,idx2str(idx)));
        insertElement(m, idx, listenerData);
}

void CkArray::insertInitial(const CkArrayIndex &idx,void *ctorMsg)
{
	CkArrayMessage *m=(CkArrayMessage *)ctorMsg;
        int listenerData[CK_ARRAYLISTENER_MAXLEN];
	prepareCtorMsg(m, listenerData);
        insertElement(m, idx, listenerData);
}

/********************* CkArray Messaging ******************/
/// Fill out a message's array fields before sending it
inline void msg_prepareSend(CkArrayMessage *msg, int ep,CkArrayID aid)
{
	envelope *env=UsrToEnv((void *)msg);
        env->setMsgtype(ForArrayEltMsg);
	env->setArrayMgr(aid);
	env->getsetArraySrcPe()=CkMyPe();
        env->setRecipientID(ck::ObjID(0));
#if CMK_SMP_TRACE_COMMTHREAD
        env->setSrcPe(CkMyPe());
#endif
	env->setEpIdx(ep);
	env->getsetArrayHops()=0;
#ifdef USE_CRITICAL_PATH_HEADER_ARRAY
	criticalPath_send(env);
	automaticallySetMessagePriority(env);
#endif
}

void CProxyElement_ArrayBase::ckSend(CkArrayMessage *msg, int ep, int opts) const
{
#if CMK_ERROR_CHECKING
	//Check our array index for validity
	if (_idx.nInts<0) CkAbort("Array index length is negative!\n");
	if (_idx.nInts>CK_ARRAYINDEX_MAXLEN)
		CkAbort("Array index length (nInts) is too long-- did you "
			"use bytes instead of integers?\n");
#endif
	msg_prepareSend(msg, ep, ckGetArrayID());
	if (ckIsDelegated()) //Just call our delegateMgr
	  ckDelegatedTo()->ArraySend(ckDelegatedPtr(),ep,msg,_idx,ckGetArrayID());
	else 
	{ //Usual case: a direct send
	  CkArray *localbranch = ckLocalBranch();
	  if (localbranch == NULL) { // array not created yet
	    CkAbort("Cannot send a message from an array without a local branch");
	  }
	  else {
	    if (opts & CK_MSG_INLINE)
	      localbranch->deliver(msg, _idx, CkDeliver_inline, opts & (~CK_MSG_INLINE));
	    else
	      localbranch->deliver(msg, _idx, CkDeliver_queue, opts);
	  }
	}
}

void *CProxyElement_ArrayBase::ckSendSync(CkArrayMessage *msg, int ep) const
{
	CkFutureID f=CkCreateAttachedFuture(msg);
	ckSend(msg,ep);
	return CkWaitReleaseFuture(f);
}

void CkBroadcastMsgSection(int entryIndex, void *msg, CkSectionID sID, int opts     )
{
	CProxySection_ArrayBase sp(sID);
	sp.ckSend((CkArrayMessage *)msg,entryIndex,opts);
}

void CProxySection_ArrayBase::ckSend(CkArrayMessage *msg, int ep, int opts)
{
	if (ckIsDelegated()) //Just call our delegateMgr
	  ckDelegatedTo()->ArraySectionSend(ckDelegatedPtr(), ep, msg, _sid.size(), _sid.data(), opts);
	else {
	  // send through all
	  for (int k=0; k<_sid.size(); ++k) {
	    for (int i=0; i< _sid[k]._elems.size()-1; i++) {
	      CProxyElement_ArrayBase ap(_sid[k]._cookie.get_aid(), _sid[k]._elems[i]);
	      void *newMsg=CkCopyMsg((void **)&msg);
	      ap.ckSend((CkArrayMessage *)newMsg,ep,opts);
	    }
	    if (!_sid[k]._elems.empty()) {
	      void *newMsg= (k<_sid.size()-1) ? CkCopyMsg((void **)&msg) : msg;
	      CProxyElement_ArrayBase ap(_sid[k]._cookie.get_aid(), _sid[k]._elems[_sid[k]._elems.size()-1]);
	      ap.ckSend((CkArrayMessage *)newMsg,ep,opts);
	    }
	  }
	}
}

void CkSetMsgArrayIfNotThere(void *msg) {
  envelope *env = UsrToEnv((void *)msg);
  env->setMsgtype(ForArrayEltMsg);
  CkArrayMessage *m = (CkArrayMessage *)msg;
  m->array_setIfNotThere(CkArray_IfNotThere_buffer);
}

void CkSendMsgArray(int entryIndex, void *msg, CkArrayID aID, const CkArrayIndex &idx, int opts)
{
  CkArrayMessage *m=(CkArrayMessage *)msg;
  msg_prepareSend(m,entryIndex,aID);
  CkArray *a=(CkArray *)_localBranch(aID);
  if (a == NULL)
    CkAbort("Cannot receive a message for an array without a local branch");
  else
    a->deliver(m, idx, CkDeliver_queue, opts);
}

void CkSendMsgArrayInline(int entryIndex, void *msg, CkArrayID aID, const CkArrayIndex &idx, int opts)
{
  CkArrayMessage *m=(CkArrayMessage *)msg;
  msg_prepareSend(m,entryIndex,aID);
  CkArray *a=(CkArray *)_localBranch(aID);
  int oldStatus = CkDisableTracing(entryIndex);     // avoid nested tracing
  a->deliver(m, idx, CkDeliver_inline, opts);
  if (oldStatus) CkEnableTracing(entryIndex);
}


/*********************** CkArray Reduction *******************/
CkArrayReducer::CkArrayReducer(CkGroupID mgrID_)
  :CkArrayListener(sizeof(contributorInfo)/sizeof(int)),
   mgrID(mgrID_)
{
  mgr=CProxy_CkReductionMgr(mgrID).ckLocalBranch();
}
CkArrayReducer::CkArrayReducer(CkMigrateMessage *m)
  :CkArrayListener(m)
{
  mgr=NULL;
}
void CkArrayReducer::pup(PUP::er &p) {
  CkArrayListener::pup(p);
  p|mgrID;
  if (p.isUnpacking())
    mgr=CProxy_CkReductionMgr(mgrID).ckLocalBranch();
}
CkArrayReducer::~CkArrayReducer() {}

/*********************** CkArray Broadcast ******************/

CkArrayBroadcaster::CkArrayBroadcaster(bool stableLocations_, bool broadcastViaScheduler_)
    :CkArrayListener(1), //Each array element carries a broadcast number
     oldBcastNo(0), stableLocations(stableLocations_), broadcastViaScheduler(broadcastViaScheduler_)
{
  setBcastNo(0);
}

CkArrayBroadcaster::CkArrayBroadcaster(CkMigrateMessage *m)
    :CkArrayListener(m), oldBcastNo(-1), broadcastViaScheduler(false)
{
  setBcastNo(-1);
}

void CkArrayBroadcaster::pup(PUP::er &p) {
  CkArrayListener::pup(p);
  /* Assumption: no migrants during checkpoint, so no need to
     save old broadcasts. */
  int bcastNum = getBcastNo();
  p|bcastNum;
  p|stableLocations;
  p|broadcastViaScheduler;
  if (p.isUnpacking()) {
    setBcastNo(bcastNum);
    oldBcastNo=bcastNum; /* because we threw away oldBcasts */
  }
}

CkArrayBroadcaster::~CkArrayBroadcaster()
{
  CkArrayMessage *msg;
  while (NULL!=(msg=oldBcasts.deq())) delete msg;
}

int CkArrayBroadcaster::incrementBcastNo() {
  return 1 + incBcastNo();
}

void CkArrayBroadcaster::incoming(CkArrayMessage *msg)
{

  if((CMI_ZC_MSGTYPE(UsrToEnv(msg)) == CMK_ZC_BCAST_SEND_MSG ||
      CMI_ZC_MSGTYPE(UsrToEnv(msg)) == CMK_ZC_BCAST_RECV_MSG) &&
      getRootNode(UsrToEnv(msg)) != 0 &&
      CkMyPe() == 0) {
    // Do not increment as it has already been incremented
  } else
  {
    incBcastNo();
  }

  DEBB((AA "Received broadcast %d\n" AB,getBcastNo()));

  if (stableLocations)
    return;

  CmiMemoryMarkBlock(((char *)UsrToEnv(msg))-sizeof(CmiChunkHeader));
  oldBcasts.enq((CkArrayMessage *)msg);//Stash the message for later use
}

/// Deliver a copy of the given broadcast to the given local element
bool CkArrayBroadcaster::deliver(CkArrayMessage *bcast, ArrayElement *el,
				    bool doFree)
{
  int &elBcastNo=getData(el);
  // if this array element already received this message, skip it
  if (elBcastNo >= getBcastNo()) return false;
  elBcastNo++;
  DEBB((AA "Delivering broadcast %d to element %s\n" AB,elBcastNo,idx2str(el)));

  CkAssert(UsrToEnv(bcast)->getMsgtype() == ArrayBcastFwdMsg);

  if (!broadcastViaScheduler)
    return el->ckInvokeEntry(bcast->array_ep_bcast(), bcast, doFree);
  else {
    if (!doFree) {
      CkArrayMessage *newMsg = (CkArrayMessage *)CkCopyMsg((void **)&bcast);
      bcast = newMsg;
    }
    envelope *env = UsrToEnv(bcast);
    env->setRecipientID(el->ckGetID());
    CkArrayManagerDeliver(CkMyPe(), bcast, 0);
    return true;
  }
}

#if CMK_CHARM4PY

extern void (*ArrayBcastRecvExtCallback)(int, int, int, int, int *, int, int, char *, int);

void CkArrayBroadcaster::deliver(CkArrayMessage *bcast,
                                 std::vector<CkMigratable*> &elements,
                                 int arrayId, bool doFree)
{
  if (elements.size() == 0)
    return;
  CkAssert(UsrToEnv(bcast)->getMsgtype() == ArrayBcastFwdMsg);

  ArrayElement *el = (ArrayElement*)elements[0];
  // get number of dimensions and number of ints used by CkArrayIndex of this array
  const int numDim = el->thisIndexMax.getDimension();
  const int numInts = el->thisIndexMax.nInts;
  // store array index data of elements that are going to receive the broadcast, to pass to Charm4py
  std::vector<int> validIndexes(elements.size() * numInts);
  int numValidElements = 0;
  int j = 0;
  for (CkMigratable *m : elements) {
    ArrayElement *el = (ArrayElement*)m;
    int &elBcastNo = getData(el);
    // if this array element already received this message, skip it
    if (elBcastNo >= bcastNo) continue;
    elBcastNo++;
    DEBB((AA "Delivering broadcast %d to element %s\n" AB,elBcastNo,idx2str(el)));
    int *index = el->thisIndexMax.data();
    for (int i=0; i < numInts; i++) validIndexes[j++] = index[i];
    numValidElements++;
  }
  char *msg_buf = ck::get_message_buffer((ck::marshall_msg)bcast);
  PUP::fromMem implP(msg_buf);
  int msgSize; implP|msgSize;
  int ep; implP|ep;
  int dcopy_start; implP|dcopy_start;
  ArrayBcastRecvExtCallback(arrayId, numDim, numInts, numValidElements, validIndexes.data(),
                            ep, msgSize, msg_buf+(3*sizeof(int)), dcopy_start);
  if (doFree)
    delete bcast;
}

#endif

/// Deliver all needed broadcasts to the given local element
bool CkArrayBroadcaster::bringUpToDate(ArrayElement *el)
{
  if (stableLocations) return true;
  int &elBcastNo=getData(el);
  int bcastNum = getBcastNo();
  if (elBcastNo<bcastNum)
  {//This element needs some broadcasts-- it must have
   //been migrating during the broadcast.
    int i,nDeliver=bcastNum-elBcastNo;
    DEBM((AA "Migrator %s missed %d broadcasts--\n" AB,idx2str(el),nDeliver));

    //Skip the old junk at the front of the bcast queue
    for (i=oldBcasts.length()-1;i>=nDeliver;i--)
      oldBcasts.enq(oldBcasts.deq());

    //Deliver the newest messages, in old-to-new order
    for (i=nDeliver-1;i>=0;i--)
    {
      CkArrayMessage *msg=oldBcasts.deq();
		if(msg == NULL)
        	continue;
      oldBcasts.enq(msg);
      if (!deliver(msg, el, false))
	return false; //Element migrated away
    }
  }
  //Otherwise, the element survived
  return true;
}


void CkArrayBroadcaster::springCleaning(void)
{
  int bcastNum = getBcastNo();
  //Remove old broadcast messages
  int nDelete=oldBcasts.length()-(bcastNum-oldBcastNo);
  if (nDelete>0) {
    DEBK((AA "Cleaning out %d old broadcasts\n" AB,nDelete));
    for (int i=0;i<nDelete;i++)
      delete oldBcasts.deq();
  }
  oldBcastNo=bcastNum;
}

void CkArrayBroadcaster::flushState() 
{ 
  setBcastNo(0);
  oldBcastNo = 0; 
  CkArrayMessage *msg;
  while (NULL!=(msg=oldBcasts.deq())) delete msg;
}

void CkBroadcastMsgArray(int entryIndex, void *msg, CkArrayID aID, int opts)
{
	CProxy_ArrayBase ap(aID);
	ap.ckBroadcast((CkArrayMessage *)msg,entryIndex,opts);
}

void CProxy_ArrayBase::ckBroadcast(CkArrayMessage* msg, int ep, int opts) const
{
  envelope* env = UsrToEnv(msg);
  env->setMsgtype(ArrayBcastMsg);
  msg->array_ep_bcast() = ep;
  if (ckIsDelegated()) {
    //Just call our delegateMgr
    ckDelegatedTo()->ArrayBroadcast(ckDelegatedPtr(), ep, msg, _aid);
  } else {
    //Broadcast message via serializer node
    _TRACE_CREATION_DETAILED(UsrToEnv(msg), ep);
    static constexpr int serializer = 0;
    int skipsched = opts & CK_MSG_EXPEDITED;
    if (CkMyPe() == serializer) {
      DEBB((AA "Sending array broadcast\n" AB));
      if (skipsched && _entryTable[ep]->noKeep) {
        CProxy_CkArray(_aid).recvNoKeepExpeditedBroadcast(msg);
      } else if (skipsched) {
        CProxy_CkArray(_aid).recvExpeditedBroadcast(msg);
      } else if (_entryTable[ep]->noKeep) {
        CProxy_CkArray(_aid).recvNoKeepBroadcast(msg);
      } else {
        CProxy_CkArray(_aid).recvBroadcast(msg);
      }
    } else {
      DEBB((AA "Forwarding array broadcast to serializer node %d\n" AB,
          serializer));
      CProxy_CkArray ap(_aid);
      if (CMI_ZC_MSGTYPE(env) == CMK_ZC_BCAST_SEND_MSG ||
          CMI_ZC_MSGTYPE(env) == CMK_ZC_BCAST_RECV_MSG) {
        // ZC Bcast is implemented on non-zero root nodes by sending a small
        // message to Node 0 (through comm thread) to increment bcastNo on PE 0
        // i.e. the serializerPe (implemented as an atomic). After
        // incrementing, an ack message is sent back to this PE (which is the
        // root node pe) to perform a broadcast
        MsgPointerWrapper w;
        w.msg = msg;
        w.ep = ep;
        w.opts = opts;
        ap[serializer].incrementBcastNoAndSendBack(CkMyPe(), w);
      } else
      {
        // Regular Bcast (non ZC) is implemented on non-zero root nodes by
        // forwarding the message to PE 0 and then having PE 0 perform the
        // broadcast rooted at Node 0. This is done to ensure single delivery
        // (and avoid multiple delivery or no delivery of the message) when
        // load balancing occurs during a broadcast
        DEBB((AA "Forwarding array broadcast to serializer node %d\n" AB,
            serializer));
        if (skipsched && _entryTable[ep]->noKeep) {
          ap[serializer].sendNoKeepExpeditedBroadcast(msg);
        } else if (skipsched) {
          ap[serializer].sendExpeditedBroadcast(msg);
        } else if (_entryTable[ep]->noKeep) {
          ap[serializer].sendNoKeepBroadcast(msg);
        } else {
          ap[serializer].sendBroadcast(msg);
        }
      }
    }
  }
}

void CkArray::incrementBcastNoAndSendBack(int srcPe, MsgPointerWrapper w){
    // increment bcastNo
    broadcaster->incrementBcastNo();

    // Send back to CkArray on that index
    thisProxy[srcPe].sendZCBroadcast(w);
  }

void CkArray::incrementBcastNo(){
  int zcBcastNo = broadcaster->incrementBcastNo();
}

void CkArray::sendZCBroadcast(MsgPointerWrapper w) {
  int skipsched = w.opts & CK_MSG_EXPEDITED;
  int nokeep = _entryTable[w.ep]->noKeep;
  if (skipsched && nokeep) {
    thisProxy.recvNoKeepExpeditedBroadcast((CkArrayMessage *)(w.msg));
  } else if (skipsched) {
    thisProxy.recvExpeditedBroadcast((CkArrayMessage *)(w.msg));
  } else if (nokeep) {
    thisProxy.recvNoKeepBroadcast((CkArrayMessage *)(w.msg));
  } else {
    thisProxy.recvBroadcast((CkArrayMessage *)(w.msg));
  }
}

/// Reflect a broadcast off this Pe:
void CkArray::sendBroadcast(CkMessage* msg) {
  CK_MAGICNUMBER_CHECK
  static constexpr int serializer = 0;
  // TODO: is this recheck necessary? If so, it's necessary in the others too
  if (CkMyPe() == serializer) {
    //Broadcast the message to all processors
    UsrToEnv(msg)->setMsgtype(ArrayBcastMsg);
    thisProxy.recvBroadcast(msg);
  } else {
    thisProxy[serializer].sendBroadcast(msg);
  }
}

void CkArray::sendNoKeepExpeditedBroadcast(CkMessage* msg) {
  CK_MAGICNUMBER_CHECK
  //Broadcast the message to all processors
  UsrToEnv(msg)->setMsgtype(ArrayBcastMsg);
  thisProxy.recvNoKeepExpeditedBroadcast(msg);
}

void CkArray::sendExpeditedBroadcast(CkMessage* msg) {
  CK_MAGICNUMBER_CHECK
  //Broadcast the message to all processors
  UsrToEnv(msg)->setMsgtype(ArrayBcastMsg);
  thisProxy.recvExpeditedBroadcast(msg);
}

void CkArray::sendNoKeepBroadcast(CkMessage* msg) {
  CK_MAGICNUMBER_CHECK
  //Broadcast the message to all processors
  UsrToEnv(msg)->setMsgtype(ArrayBcastMsg);
  thisProxy.recvNoKeepBroadcast(msg);
}

void CkArray::recvBroadcastViaTree(CkMessage *msg){
}


/// Increment broadcast count; deliver to all local elements
void CkArray::recvBroadcast(CkMessage* m) {
  CK_MAGICNUMBER_CHECK
  CkArrayMessage* msg = (CkArrayMessage *)m;
  envelope* env = UsrToEnv(msg);

  broadcaster->incoming(msg);

  int len = localElemVec.size();
  // extract this field here so we can still check it even if msg is freed
  const auto zc_msgtype = CMI_ZC_MSGTYPE(env);

  if (zc_msgtype == CMK_ZC_BCAST_RECV_ALL_DONE_MSG && len > 0 ) {
    // Message contains pointers to the posted buffer, which contains the data
    // received
    // All operations done, already consumed by other array elements, now
    // deliver to the first element

    bool doFree = true; // free it since all ops are done
    broadcaster->deliver(msg, (ArrayElement*)localElemVec[0], doFree);
  } else if (zc_msgtype == CMK_ZC_BCAST_RECV_MSG && len > 0 ) {


    // Message is used by the receiver to post the receiver buffer
    // Initial metadata message, send only to the first element, other elements
    // are sent CMK_ZC_BCAST_RECV_DONE_MSG after rget completion

    // do not free since msg will be reused to send buffers to peers, msg will
    // be finally freed by the first element in the
    // CMK_ZC_BCAST_RECV_ALL_DONE_MSG branch
    bool doFree = false;
    broadcaster->deliver(msg, (ArrayElement*)localElemVec[0], doFree);
  } else
  {
#if CMK_CHARM4PY
    broadcaster->deliver(msg, localElemVec, thisgroup.idx, stableLocations);
#else
    for (unsigned int i = 0; i < len; ++i) {
      bool doFree = false;
      if (stableLocations && i == len-1) doFree = true;
      // Do not free if CMK_ZC_BCAST_RECV_DONE_MSG, since it'll be freed by the
      // first element during CMK_ZC_BCAST_ALL_DONE_MSG
      if (zc_msgtype == CMK_ZC_BCAST_RECV_DONE_MSG) doFree = false;
      CmiAssert(i < localElemVec.size());
      broadcaster->deliver(msg, (ArrayElement*)localElemVec[i], doFree);
    }

#endif // CMK_CHARM4PY
  }

  // CkArrayBroadcaster doesn't have msg buffered, and there was no last
  // delivery to transfer ownership
  if (stableLocations && len == 0) {
    delete msg;
  }
}

void CkArray::forwardZCMsgToOtherElems(envelope *env) {

  CMI_ZC_MSGTYPE(env) = CMK_ZC_BCAST_RECV_DONE_MSG;

  int len = localElemVec.size();

  for (unsigned int i = 1; i < len; ++i) { // Send to all elements except the first element
    bool doFree = false;
    if (stableLocations && i == len-1 && CMI_ZC_MSGTYPE(env)!=CMK_ZC_BCAST_RECV_DONE_MSG) doFree = true;
    broadcaster->deliver((CkArrayMessage *)EnvToUsr(env), (ArrayElement*)localElemVec[i], doFree);
  }
}

void CkArray::flushStates() {
  CkReductionMgr::flushStates();
  // For chare arrays, and for chare arrays alone, the global and local
  // element counters in the reduction manager need to be reset to 0.
  // This is because all array elements are recreated during recovery
  // and will reregister, pushing the counts back to the correct levels.
  // For groups, the counters are set to 1 in the Group constructor.
  // However, since groups are not recreated during recovery, setting them
  // to zero in Group::flushStates() would not be followed by an increment
  // to 1 because the constructor will not be invoked.
  // Hence, these counters are reset only for chare arrays.
  resetCountersWhenFlushingStates();
  CK_ARRAYLISTENER_LOOP(listeners, l->flushState());
}

void CkArray::ckDestroy() {
  isDestroying = true;
  // Set the duringDestruction flag in the location manager. This is used to
  // indicate that the location manager is going to be destroyed so don't need
  // to send messages to remote PEs with reclaimRemote messages.
  locMgr->setDuringDestruction(true);

  // ckDestroy deletes the CkMigratable, which also removes it from this vector
  while (localElemVec.size()) {
    localElemVec.front()->ckDestroy();
  }

  locMgr->deleteManager(CkGroupID(thisProxy), this);
  if (!mCastMgrID.isZero()) {
    delete _localBranch(mCastMgrID);
    mCastMgrID.setZero();
  }
  delete this;
}

#include "CkArray.def.h"


