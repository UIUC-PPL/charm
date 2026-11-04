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
#include <unordered_map>
#include "ck.h"
#include "ckarray.h"
#include <climits>
#include "pathHistory.h"
#include "register.h"
#include <stdarg.h>

bool _isAnytimeMigration;
bool _isNotifyChildInRed;

#define ARRAY_DEBUG_OUTPUT 0

#if ARRAY_DEBUG_OUTPUT
#  define DEB(x) CkPrintf x   // General debug messages
#  define DEBI(x) CkPrintf x  // Index debug messages
#  define DEBC(x) CkPrintf x  // Construction debug messages
#  define DEBS(x) CkPrintf x  // Send/recv/broadcast debug messages
#  define DEBM(x) CkPrintf x  // Migration debug messages
#  define DEBL(x) CkPrintf x  // Load balancing debug messages
#  define DEBK(x) CkPrintf x  // Spring Cleaning debug messages
#  define DEBB(x) CkPrintf x  // Broadcast debug messages
#  define AA "ArrayBOC on %d: "
#  define AB , CkMyPe()
#  define DEBUG(x) x
#else
#  define DEB(X)  /*CkPrintf x*/
#  define DEBI(X) /*CkPrintf x*/
#  define DEBC(X) /*CkPrintf x*/
#  define DEBS(x) /*CkPrintf x*/
#  define DEBM(X) /*CkPrintf x*/
#  define DEBL(X) /*CkPrintf x*/
#  define DEBK(x) /*CkPrintf x*/
#  define DEBB(x) /*CkPrintf x*/
#  define str(x)  /**/
#  define DEBUG(x)
#endif

extern int _messageBufferingThreshold;

/// This arrayListener is in charge of performing reductions on the array.
class CkArrayReducer : public CkArrayListener
{
  CkGroupID mgrID;
  CkReductionMgr* mgr;
  typedef contributorInfo* I;
  inline contributorInfo* getData(ArrayElement* el) { return (I)ckGetData(el); }

public:
  /// Attach this array to this CkReductionMgr
  CkArrayReducer(CkGroupID mgrID_);
  CkArrayReducer(CkMigrateMessage* m);
  virtual void pup(PUP::er& p);
  virtual ~CkArrayReducer();
  PUPable_decl(CkArrayReducer);

  void ckBeginInserting(void) { mgr->creatingContributors(); }
  void ckEndInserting(void) { mgr->doneCreatingContributors(); }

  void ckElementStamp(int* eltInfo) { mgr->contributorStamped((I)eltInfo); }

  void ckElementCreating(ArrayElement* elt) { mgr->contributorCreated(getData(elt)); }
  void ckElementDied(ArrayElement* elt) { mgr->contributorDied(getData(elt)); }

  void ckElementLeaving(ArrayElement* elt) { mgr->contributorLeaving(getData(elt)); }
  bool ckElementArriving(ArrayElement* elt)
  {
    mgr->contributorArriving(getData(elt));
    return true;
  }
};

/*
void
CProxyElement_ArrayBase::ckSendWrapper(void *me, void *m, int ep, int opts){
       ((CProxyElement_ArrayBase*)me)->ckSend((CkArrayMessage*)m,ep,opts);
}
*/
void CProxyElement_ArrayBase::ckSendWrapper(CkArrayID _aid, CkArrayIndex _idx, void* m,
                                            int ep, int opts)
{
  CProxyElement_ArrayBase me = CProxyElement_ArrayBase(_aid, _idx);
  ((CProxyElement_ArrayBase)me).ckSend((CkArrayMessage*)m, ep, opts);
}

/*********************** CkVerboseListener ******************/
#define VL_PRINT ckout << "VerboseListener on PE " << CkMyPe() << " > "

CkVerboseListener::CkVerboseListener(void) : CkArrayListener(0)
{
  VL_PRINT << "INIT  Creating listener" << endl;
}

void CkVerboseListener::ckRegister(CkArray* arrMgr, int dataOffset_)
{
  CkArrayListener::ckRegister(arrMgr, dataOffset_);
  VL_PRINT << "INIT  Registering array manager at offset " << dataOffset_ << endl;
}
void CkVerboseListener::ckBeginInserting(void)
{
  VL_PRINT << "INIT  Begin inserting elements" << endl;
}
void CkVerboseListener::ckEndInserting(void)
{
  VL_PRINT << "INIT  Done inserting elements" << endl;
}

void CkVerboseListener::ckElementStamp(int* eltInfo)
{
  VL_PRINT << "LIFE  Stamping element" << endl;
}
void CkVerboseListener::ckElementCreating(ArrayElement* elt)
{
  VL_PRINT << "LIFE  About to create element " << idx2str(elt) << endl;
}
bool CkVerboseListener::ckElementCreated(ArrayElement* elt)
{
  VL_PRINT << "LIFE  Created element " << idx2str(elt) << endl;
  return true;
}
void CkVerboseListener::ckElementDied(ArrayElement* elt)
{
  VL_PRINT << "LIFE  Deleting element " << idx2str(elt) << endl;
}

void CkVerboseListener::ckElementLeaving(ArrayElement* elt)
{
  VL_PRINT << "MIG  Leaving: element " << idx2str(elt) << endl;
}
bool CkVerboseListener::ckElementArriving(ArrayElement* elt)
{
  VL_PRINT << "MIG  Arriving: element " << idx2str(elt) << endl;
  return true;
}

// Iterate over the CkArrayListeners in this vector, calling "inside" each time.
#define CK_ARRAYLISTENER_LOOP(listVec, inside) \
  do                                           \
  {                                            \
    int lIdx, lMax = listVec.size();           \
    for (lIdx = 0; lIdx < lMax; lIdx++)        \
    {                                          \
      CkArrayListener* l = listVec[lIdx];      \
      inside;                                  \
    }                                          \
  } while (0)

/************************* ArrayElement *******************/
class ArrayElement_initInfo
{
public:
  CkArray* thisArray;
  CkArrayID thisArrayID;
  CkArrayIndex numInitial;
  int listenerData[CK_ARRAYLISTENER_MAXLEN];
  bool fromMigration;
};

CkpvStaticDeclare(ArrayElement_initInfo, initInfo);

// Sends held until this PE creates the array's manager group, keyed by that
// group's idx. Nothing is counted for quiescence here: the manager's creation
// message, which is counted, is still unprocessed on this PE until the flush.
struct DeferredArraySend
{
  int entryIndex;
  void* msg;
  CkArrayIndex idx;
  int opts;
};
typedef std::unordered_map<int, std::vector<DeferredArraySend>> DeferredArraySends;
CkpvStaticDeclare(DeferredArraySends, deferredArraySends);

static void deferArraySend(int entryIndex, void* msg, CkArrayID aID,
                           const CkArrayIndex& idx, int opts)
{
  CkpvAccess(deferredArraySends)[((CkGroupID)aID).idx].push_back(
      DeferredArraySend{entryIndex, msg, idx, opts});
}

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

  grid_queue_interval = CmiGridQueueGetInterval();
  grid_queue_threshold = CmiGridQueueGetThreshold();
#endif
  ArrayElement_initInfo& info = CkpvAccess(initInfo);
  thisArray = info.thisArray;
  thisArrayID = info.thisArrayID;
  numInitialElements = info.numInitial.getCombinedCount();
  memcpy(listenerData, info.listenerData, sizeof(listenerData));
  if (!info.fromMigration)
  {
    CK_ARRAYLISTENER_LOOP(thisArray->listeners, l->ckElementCreating(this));
  }
#ifdef _PIPELINED_ALLREDUCE_
  allredMgr = NULL;
#endif
  DEBC((AA "Inserting %" PRIu64 " into PE level hashtable\n" AB, ckGetID().getID()));
  CkpvAccess(array_objs)[ckGetID().getID()] = this;
}

ArrayElement::ArrayElement(void)
{
  initBasics();
#if CMK_MEM_CHECKPOINT
  init_checkpt();
#endif
}

ArrayElement::ArrayElement(CkMigrateMessage* m) : CkMigratable(m) { initBasics(); }

// Called by the system just before and after migration to another processor:
void ArrayElement::ckAboutToMigrate(void)
{
  CK_ARRAYLISTENER_LOOP(thisArray->listeners, l->ckElementLeaving(this));
  CkMigratable::ckAboutToMigrate();
}
void ArrayElement::ckJustMigrated(void)
{
  CkMigratable::ckJustMigrated();
  CK_ARRAYLISTENER_LOOP(thisArray->listeners, if (!l->ckElementArriving(this)) return;);
}

void ArrayElement::ckJustRestored(void)
{
  CkMigratable::ckJustRestored();
  // empty for out-of-core emulation
}

#ifdef _PIPELINED_ALLREDUCE_
void ArrayElement::contribute2(int dataSize, const void* data,
                               CkReduction::reducerType type, CMK_REFNUM_TYPE userFlag)
{
  CkReductionMsg* msg = CkReductionMsg::buildNew(dataSize, data, type);
  msg->setUserFlag(userFlag);
  msg->setMigratableContributor(true);
  thisArray->contribute(
      &*(contributorInfo*)&listenerData[thisArray->reducer->ckGetOffset()], msg);
}
void ArrayElement::contribute2(int dataSize, const void* data,
                               CkReduction::reducerType type, const CkCallback& cb,
                               CMK_REFNUM_TYPE userFlag)
{
  CkReductionMsg* msg = CkReductionMsg::buildNew(dataSize, data, type);
  msg->setUserFlag(userFlag);
  msg->setCallback(cb);
  msg->setMigratableContributor(true);
  thisArray->contribute(
      &*(contributorInfo*)&listenerData[thisArray->reducer->ckGetOffset()], msg);
}
void ArrayElement::contribute2(CkReductionMsg* msg)
{
  msg->setMigratableContributor(true);
  thisArray->contribute(
      &*(contributorInfo*)&listenerData[thisArray->reducer->ckGetOffset()], msg);
}
void ArrayElement::contribute2(const CkCallback& cb, CMK_REFNUM_TYPE userFlag)
{
  CkReductionMsg* msg = CkReductionMsg::buildNew(0, NULL, CkReduction::nop);
  msg->setUserFlag(userFlag);
  msg->setCallback(cb);
  msg->setMigratableContributor(true);
  thisArray->contribute(
      &*(contributorInfo*)&listenerData[thisArray->reducer->ckGetOffset()], msg);
}
void ArrayElement::contribute2(CMK_REFNUM_TYPE userFlag)
{
  CkReductionMsg* msg = CkReductionMsg::buildNew(0, NULL, CkReduction::nop);
  msg->setUserFlag(userFlag);
  msg->setMigratableContributor(true);
  thisArray->contribute(
      &*(contributorInfo*)&listenerData[thisArray->reducer->ckGetOffset()], msg);
}

void ArrayElement::contribute2(CkArrayIndex myIndex, int dataSize, const void* data,
                               CkReduction::reducerType type, const CkCallback& cb,
                               CMK_REFNUM_TYPE userFlag)
{
  // if it is a broadcast to myself and size is large
  if (cb.type == CkCallback::bcastArray && cb.d.array.id == thisArrayID &&
      dataSize > FRAG_THRESHOLD)
  {
    if (!allredMgr)
    {
      allredMgr = new AllreduceMgr();
    }
    // number of fragments
    int fragNo = dataSize / FRAG_SIZE;
    int size = FRAG_SIZE;
    // for each fragment
    for (int i = 0; i < fragNo; i++)
    {
      // callback to defragmentor
      CkCallback defrag_cb(CkIndex_ArrayElement::defrag(NULL), thisArrayID);
      if ((0 != i) && ((fragNo - 1) == i) && (0 != dataSize % FRAG_SIZE))
      {
        size = dataSize % FRAG_SIZE;
      }
      CkReductionMsg* msg = CkReductionMsg::buildNew(size, (char*)data + i * FRAG_SIZE);
      // initialize the new msg
      msg->reducer = type;
      msg->nFrags = fragNo;
      msg->fragNo = i;
      msg->callback = defrag_cb;
      msg->userFlag = userFlag;
      allredMgr->cb = cb;
      allredMgr->cb.type = CkCallback::sendArray;
      allredMgr->cb.d.array.idx = myIndex;
      contribute2(msg);
    }
    return;
  }
  CkReductionMsg* msg = CkReductionMsg::buildNew(dataSize, data, type);
  msg->setUserFlag(userFlag);
  msg->setCallback(cb);
  msg->setMigratableContributor(true);
  thisArray->contribute(
      &*(contributorInfo*)&listenerData[thisArray->reducer->ckGetOffset()], msg);
}

#else
CK_REDUCTION_CONTRIBUTE_METHODS_DEF(
    ArrayElement, thisArray,
    *(contributorInfo*)&listenerData[thisArray->reducer->ckGetOffset()], true)
#endif
// _PIPELINED_ALLREDUCE_
void ArrayElement::defrag(CkReductionMsg* msg)
{
//	CkPrintf("in defrag\n");
#ifdef _PIPELINED_ALLREDUCE_
  allredMgr->allreduce_recieve(msg);
#endif
}

int ArrayElement::getRedNo(void) const
{
  return ((contributorInfo*)&listenerData[thisArray->reducer->ckGetOffset()])->redNo;
}

// Remote method: This removes the array element from its array manager which
// also calls delete on this element. The superclass destructor then handles
// cleanup of the associated location record from CkLocMgr.
void ArrayElement::ckDestroy(void)
{
  CK_ARRAYLISTENER_LOOP(thisArray->listeners, l->ckElementDied(this));
  thisArray->deleteElt(CkMigratable::ckGetID());
}

// Destructor (virtual)
ArrayElement::~ArrayElement()
{
#if CMK_OUT_OF_CORE
  if (CkpvAccess(CkSaveRestorePrefetch))
    return; /* Just saving to disk--don't trash anything. */
#endif
  // Erase from PE level hashtable for quick receives
  DEBC((AA "Removing %" PRIu64 " from PE level hashtable\n" AB, ckGetID().getID()));
  CkpvAccess(array_objs).erase(ckGetID().getID());
  // To detect use-after-delete:
  thisArray = (CkArray*)(intptr_t)0xDEADa7a1;
}

void ArrayElement::pup(PUP::er& p)
{
  DEBM((AA "  ArrayElement::pup()\n" AB));
  CkMigratable::pup(p);
  thisArrayID.pup(p);
  if (p.isUnpacking())
    thisArray = thisArrayID.ckLocalBranch();
  p(listenerData, CK_ARRAYLISTENER_MAXLEN);
#if CMK_MEM_CHECKPOINT
  p(budPEs, 2);
#endif
  p.syncComment(PUP::sync_last_system, "ArrayElement");
#if CMK_GRID_QUEUE_AVAILABLE
  p | grid_queue_interval;
  p | grid_queue_threshold;
  p | msg_count;
  p | msg_count_grid;
  p | border_flag;
  if (p.isUnpacking())
  {
    msg_count = 0;
    msg_count_grid = 0;
    border_flag = 0;
  }
#endif
}

char* ArrayElement::ckDebugChareName(void)
{
  char buf[200];
  const char* className = _chareTable[ckGetChareType()]->name;
  const int* d = thisIndexMax.data();
  const short int* s = (const short int*)d;
  switch (thisIndexMax.dimension)
  {
    case 0:
      snprintf(buf, sizeof(buf), "%s", className);
      break;
    case 1:
      snprintf(buf, sizeof(buf), "%s[%d]", className, d[0]);
      break;
    case 2:
      snprintf(buf, sizeof(buf), "%s(%d,%d)", className, d[0], d[1]);
      break;
    case 3:
      snprintf(buf, sizeof(buf), "%s(%d,%d,%d)", className, d[0], d[1], d[2]);
      break;
    case 4:
      snprintf(buf, sizeof(buf), "%s(%hd,%hd,%hd,%hd)", className, s[0], s[1], s[2], s[3]);
      break;
    case 5:
      snprintf(buf, sizeof(buf), "%s(%hd,%hd,%hd,%hd,%hd)", className, s[0], s[1], s[2], s[3], s[4]);
      break;
    case 6:
      snprintf(buf, sizeof(buf), "%s(%hd,%hd,%hd,%hd,%hd,%hd)", className, s[0], s[1], s[2], s[3], s[4],
              s[5]);
      break;
    default:
      snprintf(buf, sizeof(buf), "%s(%d,%d,%d,%d..)", className, d[0], d[1], d[2], d[3]);
      break;
  };
  return strdup(buf);
}

int ArrayElement::ckDebugChareID(char* str, int limit)
{
  if (limit < 21)
    return -1;
  str[0] = 2;
  *((int*)&str[1]) = ((CkGroupID)thisArrayID).idx;
  *((CkArrayIndex*)&str[5]) = thisIndexMax;
  return 21;
}

/// A more verbose form of abort
void ArrayElement::CkAbort(const char* format, ...) const
{
  char newmsg[256];
  va_list args;
  va_start(args, format);
  vsnprintf(newmsg, sizeof(newmsg), format, args);
  va_end(args);

  CkMigratable::CkAbort("[%d] Array element at index %s aborting:\n%s", CkMyPe(),
                        idx2str(thisIndexMax), newmsg);
}

void ArrayElement::recvBroadcast(CkMessage* m) {}

#if CMK_CHARM4PY

ArrayElemExt::ArrayElemExt(void* impl_msg)
{
  int chareIdx = ckGetChareType();
  ctorEpIdx = _chareTable[chareIdx]->getDefaultCtor();
  // printf("Constructor of ArrayElemExt, aid=%d, chareIdx=%d, ctorEpIdx=%d\n",
  // ((CkGroupID)thisArrayID).idx, chareIdx, ctorEpIdx);
  CkMarshallMsg* impl_msg_typed = (CkMarshallMsg*)impl_msg;
  char* impl_buf = impl_msg_typed->msgBuf;
  PUP::fromMem implP(impl_buf);
  implP | usesAtSync;
  int msgSize;
  implP | msgSize;
  int dcopy_start;
  implP | dcopy_start;

  ArrayMsgRecvExtCallback(((CkGroupID)thisArrayID).idx, int(thisIndexMax.getDimension()),
                          thisIndexMax.data(), ctorEpIdx, msgSize,
                          impl_buf + (2 * sizeof(int)) + sizeof(char), dcopy_start);
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

void CkArray::staticSpringCleaning(void* forArray)
{
  ((CkArray*)forArray)->springCleaning();
}

void CkArray::setupSpringCleaning()
{
  // set up broadcast cleaner
  if (!stableLocations)
    springCleaningCcd =
        CcdCallOnCondition(CcdPERIODIC_1minute, (CcdCondFn)CkArray::staticSpringCleaning, (void*)this);
}

/********************* Little CkArray Utilities ******************/

CProxy_ArrayBase::CProxy_ArrayBase(const ArrayElement* e)
    : CProxy(), _aid(e->ckGetArrayID())
{
}
CProxyElement_ArrayBase::CProxyElement_ArrayBase(const ArrayElement* e)
    : CProxy_ArrayBase(e), _idx(e->ckGetArrayIndex())
{
}

CProxySection_ArrayBase::CProxySection_ArrayBase(const CkArrayID& aid,
                                                 const CkArrayIndex* elems,
                                                 const int nElems, int factor)
    : CProxy_ArrayBase(aid)
{
  _sid.emplace_back(aid, elems, nElems, factor);
}

CProxySection_ArrayBase::CProxySection_ArrayBase(const CkArrayID& aid,
                                                 const std::vector<CkArrayIndex>& elems,
                                                 int factor)
    : CProxy_ArrayBase(aid)
{
  _sid.emplace_back(aid, elems, factor);
}

CProxySection_ArrayBase::CProxySection_ArrayBase(const int n, const CkArrayID* aid,
                                                 CkArrayIndex const* const* elems,
                                                 const int* nElems, int factor)
    : CProxy_ArrayBase(aid[0])
{
  _sid.resize(n);
  for (int i = 0; i < _sid.size(); i++)
  {
    _sid[i] = CkSectionID(aid[i], elems[i], nElems[i], factor);
  }
}

CProxySection_ArrayBase::CProxySection_ArrayBase(
    const std::vector<CkArrayID>& aid,
    const std::vector<std::vector<CkArrayIndex> >& elems, int factor)
    : CProxy_ArrayBase(aid[0])
{
  _sid.resize(aid.size());
  for (int i = 0; i < _sid.size(); i++)
  {
    _sid[i] = CkSectionID(aid[i], elems[i], factor);
  }
}

void CProxySection_ArrayBase::ckAutoDelegate(int opts)
{
  if (_sid.empty())
    CmiAbort("Auto Delegation before setting up CkSectionID\n");
  CkArray* ckarr = CProxy_CkArray(_sid[0].get_aid()).ckLocalBranch();
  if (ckarr->isSectionAutoDelegated())
  {
    CkMulticastMgr* mCastGrp =
        CProxy_CkMulticastMgr(ckarr->getmCastMgr()).ckLocalBranch();
    ckSectionDelegate(mCastGrp, opts);
  }
}

void CProxySection_ArrayBase::setReductionClient(CkCallback* cb)
{
  if (_sid.empty())
    CmiAbort("setReductionClient before setting up CkSectionID\n");
  CkArray* ckarr = CProxy_CkArray(_sid[0].get_aid()).ckLocalBranch();
  if (ckarr->isSectionAutoDelegated())
  {
    CkMulticastMgr* mCastGrp =
        CProxy_CkMulticastMgr(ckarr->getmCastMgr()).ckLocalBranch();
    mCastGrp->setReductionClient(*this, cb);
  }
  else
  {
    CmiAbort("setReductionClient called on section without autoDelegate");
  }
}

void CProxySection_ArrayBase::resetSection()
{
  if (_sid.empty())
    CmiAbort("resetSection before setting up CkSectionID\n");
  CkArray* ckarr = CProxy_CkArray(_sid[0].get_aid()).ckLocalBranch();
  if (ckarr->isSectionAutoDelegated())
  {
    CkMulticastMgr* mCastGrp =
        CProxy_CkMulticastMgr(ckarr->getmCastMgr()).ckLocalBranch();
    mCastGrp->resetSection(*this);
  }
  else
  {
    CmiAbort("resetSection called on section without autoDelegate");
  }
}

CkLocMgr* CProxy_ArrayBase::ckLocMgr(void) const { return ckLocalBranch()->getLocMgr(); }

CK_REDUCTION_CLIENT_DEF(CProxy_ArrayBase, ckLocalBranch())

static CkArrayID CkCreateArray(CkArrayMessage* m, int ctor, CkArrayOptions opts)
{
  CkAssert(CkMyPe() == 0);

  CkGroupID locMgr = opts.getLocationManager();
  // The id layout is the location manager's: an array bound to another (bindTo)
  // takes that array's layout whatever its own option says. Each array then gets
  // its own collection number from the counter of that layout.
  bool wide = opts.isWideCollection();
  if (!locMgr.isZero())
  {
    CkLocMgr* mgr = CProxy_CkLocMgr(locMgr).ckLocalBranch();
    if (mgr == nullptr)
      CkAbort("CkCreateArray: the location manager (group %d) this array binds to has"
              " no branch on PE 0 yet\n", locMgr.idx);
    wide = mgr->isWideCollection();
  }
  opts.setCollection(ck::objid::nextCollectionNumber(wide), wide);
  if (locMgr.isZero())
  {  // Create a new location manager
    CkGroupID locCache;
    CkEntryOptions locCacheOpts;
    locCacheOpts.setGroupDepID(opts.getMap());  // group creation dependence
    locCache = CProxy_CkLocCache::ckNew(&locCacheOpts);
    opts.setLocationCache(locCache);
    CkEntryOptions locMgrOpts;
    locMgrOpts.setGroupDepID(locCache);
    locMgr = CProxy_CkLocMgr::ckNew(opts, &locMgrOpts);
    opts.setLocationManager(locMgr);
  }
  CkGroupID mCastMgr = opts.getMcastManager();
  if (opts.isSectionAutoDelegated() && mCastMgr.isZero())
  {  // Create a new multicast manager
    CkEntryOptions e_opts;
    e_opts.setGroupDepID(locMgr);  // group creation dependence
    // call with default parameters, since the last parameter has to be e_opts
    mCastMgr = CProxy_CkMulticastMgr::ckNew(2, 8192, 8192, &e_opts);
    opts.setMcastManager(mCastMgr);
  }
  // Create the array manager
  m->array_ep() = ctor;
  CkMarshalledMessage marsh(m);
  CkEntryOptions e_opts;
  e_opts.setGroupDepID(locMgr);  // group creation dependence
  if (opts.isSectionAutoDelegated())
  {
    e_opts.setGroupDepID(mCastMgr);
  }

  // Add user defined group dependencies
  envelope* env = UsrToEnv(m);
  for (int i = 0; i < env->getGroupDepNum(); i++)
  {
    e_opts.addGroupDepID(env->getGroupDep(i));
  }
  CkGroupID ag = CProxy_CkArray::ckNew(opts, marsh, &e_opts);
  return (CkArrayID)ag;
}

CkArrayID CProxy_ArrayBase::ckCreateArray(CkArrayMessage* m, int ctor,
                                          const CkArrayOptions& opts)
{
  return CkCreateArray(m, ctor, opts);
}

CkArrayID CProxy_ArrayBase::ckCreateEmptyArray(CkArrayOptions opts)
{
  return ckCreateArray((CkArrayMessage*)CkAllocSysMsg(), 0, opts);
}

void CProxy_ArrayBase::ckCreateEmptyArrayAsync(CkCallback cb, CkArrayOptions opts)
{
  CkSendAsyncCreateArray(0, cb, opts, (CkArrayMessage*)CkAllocSysMsg());
}

extern IrrGroup* lookupGroupAndBufferIfNotThere(CkCoreState* ck, envelope* env,
                                                const CkGroupID& groupID);

struct CkInsertIdxMsg
{
  char core[CmiReservedHeaderSize];
  CkArrayIndex idx;
  CkArrayMessage* m;
  int ctor;
  int onPe;
  CkArrayID _aid;
};

static int ckinsertIdxHdl;

void ckinsertIdxFunc(void* m)
{
  CkInsertIdxMsg* msg = (CkInsertIdxMsg*)m;
  CProxy_ArrayBase ca(msg->_aid);
  ca.ckInsertIdx(msg->m, msg->ctor, msg->onPe, msg->idx);
  CmiFree(msg);
}

void CProxy_ArrayBase::ckInsertIdx(CkArrayMessage* m, int ctor, int proposedPe,
                                   const CkArrayIndex& idx)
{
  if (m == NULL)
    m = (CkArrayMessage*)CkAllocSysMsg();
  m->array_ep() = ctor;
  CkArray* ca = ckLocalBranch();
  if (ca == NULL)
  {
    CkInsertIdxMsg* msg = (CkInsertIdxMsg*)CmiAlloc(sizeof(CkInsertIdxMsg));
    msg->idx = idx;
    msg->m = m;
    msg->ctor = ctor;
    msg->onPe = proposedPe;
    msg->_aid = _aid;
    CmiSetHandler(msg, ckinsertIdxHdl);
    ca = (CkArray*)lookupGroupAndBufferIfNotThere(CkpvAccess(_coreState), (envelope*)msg,
                                                  _aid);
    CkAssert(ca == NULL);
    return;
  }

  int hostPe = ca->findInitialHostPe(idx, proposedPe);

  int listenerData[CK_ARRAYLISTENER_MAXLEN];
  ca->prepareCtorMsg(m, listenerData);
  if (ckIsDelegated())
  {
    ckDelegatedTo()->ArrayCreate(ckDelegatedPtr(), ctor, m, idx, hostPe, _aid);
    return;
  }

  DEBC((AA "Proxy inserting element %s on Pe %d\n" AB, idx2str(idx), hostPe));
  CProxy_CkArray(_aid)[hostPe].insertElement(m, idx, listenerData);
}

void CProxyElement_ArrayBase::ckInsert(CkArrayMessage* m, int ctorIndex, int onPe)
{
  ckInsertIdx(m, ctorIndex, onPe, _idx);
}

ArrayElement* CProxyElement_ArrayBase::ckLocal(void) const
{
  return ckLocalBranch()->lookup(_idx);
}

// pack-unpack method for CProxy_ArrayBase
void CProxy_ArrayBase::pup(PUP::er& p)
{
  CProxy::pup(p);
  _aid.pup(p);
}
void CProxyElement_ArrayBase::pup(PUP::er& p)
{
  CProxy_ArrayBase::pup(p);
  p | _idx;
}

void CProxySection_ArrayBase::pup(PUP::er& p)
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
class CkCreateArrayAsyncMsg : public CMessage_CkCreateArrayAsyncMsg
{
 public:
  int ctor;
  CkCallback cb;
  CkArrayOptions opts;
  char* ctorPayload;

  CkCreateArrayAsyncMsg(int ctor_, CkCallback cb_, CkArrayOptions opts_)
      : ctor(ctor_), cb(cb_), opts(opts_)
  {
  }
};

static int ckArrayCreationHdl = 0;

void CkSendAsyncCreateArray(int ctor, CkCallback cb, CkArrayOptions opts, void* ctorMsg)
{
  CkAssert(ctorMsg);
  UsrToEnv(ctorMsg)->setMsgtype(ArrayEltInitMsg);
  PUP::sizer ps;
  CkPupMessage(ps, &ctorMsg);
  CkCreateArrayAsyncMsg* msg = new (ps.size()) CkCreateArrayAsyncMsg(ctor, cb, opts);
  PUP::toMem p(msg->ctorPayload);
  CkPupMessage(p, &ctorMsg);
  CkFreeMsg(ctorMsg);
  envelope* env = UsrToEnv(msg);
  CmiSetHandler(env, ckArrayCreationHdl);
  CkPackMessage(&env);
  CmiSyncSendAndFree(0, env->getTotalsize(), (char*)env);
}

static void CkCreateArrayAsync(void* vmsg)
{
  envelope* venv = static_cast<envelope*>(vmsg);
  CkUnpackMessage(&venv);
  CkCreateArrayAsyncMsg* msg = static_cast<CkCreateArrayAsyncMsg*>(EnvToUsr(venv));

  // Unpack arguments
  PUP::fromMem p(msg->ctorPayload);
  void* vm;
  CkPupMessage(p, &vm);
  CkArrayMessage* m = static_cast<CkArrayMessage*>(vm);

  CkArrayID aid = CkCreateArray(m, msg->ctor, msg->opts);

  // Does the caller care about the constructed array ID?
  if (!msg->cb.isInvalid())
    msg->cb.send(new CkArrayCreatedMsg(aid));
  delete msg;
}

/*********************** CkArray Creation *************************/
void _ckArrayInit(void)
{
  CkpvInitialize(ArrayElement_initInfo, initInfo);
  CkpvInitialize(DeferredArraySends, deferredArraySends);
  CkDisableTracing(CkIndex_CkArray::insertElement(0, CkArrayIndex(), 0));
  CkDisableTracing(CkIndex_CkArray::recvBroadcast(0));
  // disable because broadcast listener may deliver broadcast message
  CkDisableTracing(CkIndex_CkLocMgr::immigrate(0));
  // by default anytime migration is allowed
  CmiAssignOnce(&ckinsertIdxHdl, CkRegisterHandler(ckinsertIdxFunc));
  CmiAssignOnce(&ckArrayCreationHdl, CkRegisterHandler(CkCreateArrayAsync));
}

CkArray::CkArray(CkArrayOptions&& opts, CkMarshalledMessage&& initMsg)
    : locMgr(CProxy_CkLocMgr::ckLocalBranch(opts.getLocationManager())),
      locMgrID(opts.getLocationManager()),
      collectionNumber(opts.getCollectionNumber()),
      wideCollection(opts.isWideCollection()),
      mCastMgrID(opts.getMcastManager()),
      sectionAutoDelegate(opts.isSectionAutoDelegated()),
      initCallback(opts.getInitCallback()),
      thisProxy(thisgroup),
      // The program promises no broadcast to the array is in flight while an
      // element moves, so a broadcast is freed once every local element has
      // it (doc/internals/array-broadcast-migration.md).
      stableLocations(opts.isStaticInsertion() && !opts.anytimeMigration),
      numInitial(opts.getNumInitial()),
      isInserting(true),
      numPesInited(0)
{
  CkAssert(wideCollection == locMgr->isWideCollection());
  // Register with our location manager
  locMgr->addManager(thisgroup, this);
  locMgr->addLocationListener([=](CmiUInt8 id, int pe) { this->sendBufferedMsgs(id, pe); });
  locMgr->addIndexListener([=](const CkArrayIndex& idx, CmiUInt8 id, int pe) { this->sendBufferedMsgs(idx, id, pe); });

  setupSpringCleaning();

  // set the field in one my parent class (CkReductionMgr)
  if (opts.disableNotifyChildInRed)
    disableNotifyChildrenStart = true;

  // Find, register, and initialize the arrayListeners
  listenerDataOffset = 0;
  broadcaster = new CkArrayBroadcaster(stableLocations, opts.broadcastViaScheduler);
  broadcaster->setOwner(this);
  addListener(broadcaster);
  reducer = new CkArrayReducer(thisgroup);
  addListener(reducer);

  // COMLIB HACK
  // calistener = new ComlibArrayListener();
  // addListener(calistener,dataOffset);

  int lNo, nL = opts.getListeners();  // User-added listeners
  for (lNo = 0; lNo < nL; lNo++) addListener(opts.getListener(lNo));

  for (int l = 0; l < listeners.size(); l++) listeners[l]->ckBeginInserting();

  /// Set up initial elements (if any)
  CkGroupID mapID = opts.getMap();
  CkArrayMap* map = (CkArrayMap*)CkLocalBranch(mapID);
  if (map == NULL)
    CkAbort("ERROR! Local branch of array map is NULL!");
  map->storeCkArrayOpts(opts);
  map->populateInitial(locMgr->getMapHandle(), opts, initMsg.getMessage(), this);
  if (opts.isStaticInsertion())
    remoteDoneInserting();

  if (opts.reductionClient.type != CkCallback::invalid && CkMyPe() == 0)
    ckSetReductionClient(&opts.reductionClient);
}

CkArray::CkArray(CkMigrateMessage* m) : CkReductionMgr(m), thisProxy(thisgroup)
{
  locMgr = NULL;
  isInserting = true;
}

CkArray::~CkArray()
{
  if (!stableLocations)
    CcdCancelCallOnCondition(CcdPERIODIC_1minute, springCleaningCcd);
}

#if CMK_ERROR_CHECKING
inline void testPup(PUP::er& p, int shouldBe)
{
  int a = shouldBe;
  p | a;
  if (a != shouldBe)
    CkAbort("PUP direction mismatch!");
}
#else
inline void testPup(PUP::er& p, int shouldBe) {}
#endif

void CkArray::pup(PUP::er& p)
{
  CkReductionMgr::pup(p);
  p | numInitial;
  p | locMgrID;
  p | collectionNumber;
  p | wideCollection;
  p | mCastMgrID;
  p | sectionAutoDelegate;
  p | initCallback;
  p | listeners;
  p | listenerDataOffset;
  p | stableLocations;
  p | numPesInited;
  testPup(p, 1234);
  if (p.isUnpacking())
  {
    thisProxy = thisgroup;
    locMgr = CProxy_CkLocMgr::ckLocalBranch(locMgrID);
    ck::objid::noteRestoredCollection(collectionNumber, wideCollection);
    locMgr->addManager(thisgroup, this);
    locMgr->addLocationListener([=](CmiUInt8 id, int pe) { this->sendBufferedMsgs(id, pe); });
    locMgr->addIndexListener([=](const CkArrayIndex& idx, CmiUInt8 id, int pe) { this->sendBufferedMsgs(idx, id, pe); });
    /// Restore our default listeners:
    broadcaster = (CkArrayBroadcaster*)(CkArrayListener*)(listeners[0]);
    broadcaster->setOwner(this);
    reducer = (CkArrayReducer*)(CkArrayListener*)(listeners[1]);
    setupSpringCleaning();
  }
}

#define CK_ARRAYLISTENER_STAMP_LOOP(listenerData)    \
  do                                                 \
  {                                                  \
    int dataOffset = 0;                              \
    for (int lNo = 0; lNo < listeners.size(); lNo++) \
    {                                                \
      CkArrayListener* l = listeners[lNo];           \
      l->ckElementStamp(&listenerData[dataOffset]);  \
      dataOffset += l->ckGetLen();                   \
    }                                                \
  } while (0)

// Called on send side to prepare array constructor message
void CkArray::prepareCtorMsg(CkMessage* m, int* listenerData)
{
  envelope* env = UsrToEnv((void*)m);
  env->setMsgtype(ArrayEltInitMsg);
  CK_ARRAYLISTENER_STAMP_LOOP(listenerData);
}

int CkArray::findInitialHostPe(const CkArrayIndex& idx, int proposedPe)
{
  int hostPe = locMgr->whichPe(idx);

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

void CkArray::stampListenerData(CkMigratable* elt)
{
  ArrayElement* elt2 = (ArrayElement*)elt;
  CK_ARRAYLISTENER_STAMP_LOOP(elt2->listenerData);
}

CkMigratable* CkArray::allocateMigrated(int elChareType, CkElementCreation_t type)
{
  ArrayElement* ret = allocate(elChareType, NULL, true, NULL);
  return ret;
}

ArrayElement* CkArray::allocate(int elChareType, CkMessage* msg, bool fromMigration,
                                int* listenerData)
{
  // Stash the element's initialization information in the global "initInfo"
  ArrayElement_initInfo& init = CkpvAccess(initInfo);
  init.numInitial = numInitial;
  init.thisArray = this;
  init.thisArrayID = thisgroup;
  if (listenerData) /*Have to *copy* data because msg will be deleted*/
    memcpy(init.listenerData, listenerData, sizeof(init.listenerData));
  init.fromMigration = fromMigration;

  // Build the element
  return (ArrayElement *)CkAllocateChare(elChareType);
}

void CkArray::insertElement(CkMarshalledMessage&& m, const CkArrayIndex& idx,
                            int listenerData[CK_ARRAYLISTENER_MAXLEN])
{
  insertElement((CkArrayMessage*)m.getMessage(), idx, listenerData);
}

/// This method is called by ck.C or the user to add an element.
bool CkArray::insertElement(CkArrayMessage* m, const CkArrayIndex& idx,
                            int listenerData[CK_ARRAYLISTENER_MAXLEN])
{
  CK_MAGICNUMBER_CHECK
  int onPe;
  // Bound arrays: the sibling already lives somewhere else, so insert there instead.
  // For a single array an entry naming another PE is a dead element's (charm #4031;
  // doc/internals/object-ids.md, section 3), and the element is created here.
  // TODO: What if it's remote but we don't know it? Creation is not necessarily routed
  // through home like demand creation is, which can cause problems.
  if (locMgr->servesBoundArrays() && locMgr->isRemote(idx, &onPe))
  {
    thisProxy[onPe].insertElement(m, idx, listenerData);
    return false;
  }

  // Register the new element with the location manager
  CkLocRec* rec = locMgr->registerNewElement(idx);
  if (rec == nullptr)
  {
    // Hashed-kind id and this process is out of unique numbers until the allocator's
    // next tranche arrives: the insertion completes then (the element does not exist
    // when this call returns, even for a local [inline] insert).
    locMgr->deferInsertion(this, m, idx, listenerData);
    return false;
  }
  CmiUInt8 id = rec->getID();

  // Make sure the element doesn't already exist
  CkAssertMsg(getEltFromArrMgr(id) == nullptr, "Cannot insert array element twice!");

  // Create the new element and insert it into the array manager
  int ctorIdx = m->array_ep();
  int chareType = _entryTable[ctorIdx]->chareIdx;
  ArrayElement* elt = allocate(chareType, m, false, listenerData);
  putEltInArrMgr(id, elt);

  // Set the constructor info for the new element
  CkMigratable_initInfo& i = CkpvAccess(mig_initInfo);
  i.locRec = rec;
  i.chareType = chareType;

  // Execute the constructor
  if (!rec->invokeEntry(elt, (void*)m, ctorIdx, true))
    return false;

  elt->ckFinishConstruction();

  CK_ARRAYLISTENER_LOOP(listeners, if (!l->ckElementCreated(elt)) return false;);
  // The initCallback will only be valid if it was set in CkArrayOptions and this is the
  // first wave of insertions.
  if (!initCallback.isInvalid()) elt->contribute(initCallback);

  // In the case where this is a sibling of an element that already existed on this PE,
  // we need to make sure we deliver any buffered messages.
  sendBufferedMsgs(id, CkMyPe());
  return true;
}

void CProxy_ArrayBase::doneInserting(void)
{
  DEBC((AA "Broadcasting a doneInserting request\n" AB));
  // Broadcast a DoneInserting
  CProxy_CkArray(_aid).remoteDoneInserting();
}

void CProxy_ArrayBase::beginInserting(void)
{
  DEBC((AA "Broadcasting a beginInserting request\n" AB));
  CProxy_CkArray(_aid).remoteBeginInserting();
}

void CkArray::doneInserting(void) { thisProxy[CkMyPe()].remoteDoneInserting(); }

void CkArray::beginInserting(void) { thisProxy[CkMyPe()].remoteBeginInserting(); }

/// This is called on every processor after the last array insertion.
void CkArray::remoteDoneInserting(void)
{
  CK_MAGICNUMBER_CHECK
  if (isInserting)
  {
    isInserting = false;
    DEBC((AA "Done inserting objects\n" AB));
    for (int l = 0; l < listeners.size(); l++) listeners[l]->ckEndInserting();
    locMgr->doneInserting();
  }
}

void CkArray::remoteBeginInserting(void)
{
  CK_MAGICNUMBER_CHECK;

  if (!isInserting)
  {
    // After the first wave of insertions, the init callback should not be used
    initCallback = CkCallback(CkCallback::invalid);
    isInserting = true;
    DEBC((AA "Begin inserting objects\n" AB));
    for (int l = 0; l < listeners.size(); l++) listeners[l]->ckBeginInserting();
    locMgr->startInserting();
  }
}

void CkArray::insertInitial(const CkArrayIndex& idx, void* ctorMsg)
{
  CkArrayMessage* m = (CkArrayMessage*)ctorMsg;
  int listenerData[CK_ARRAYLISTENER_MAXLEN];
  prepareCtorMsg(m, listenerData);
  insertElement(m, idx, listenerData);
}

/********************* CkArray Messaging ******************/
/// Fill out a message's array fields before sending it
inline void msg_prepareSend(CkArrayMessage* msg, int ep, CkArrayID aid)
{
  envelope* env = UsrToEnv((void*)msg);
  env->setMsgtype(ForArrayEltMsg);
  env->setArrayMgr(aid);
  env->getsetArraySrcPe() = CkMyPe();
  env->setRecipientID(ck::ObjID(0));
#if CMK_SMP_TRACE_COMMTHREAD
  env->setSrcPe(CkMyPe());
#endif
  env->setEpIdx(ep);
  env->getsetArrayHops() = 0;
#ifdef USE_CRITICAL_PATH_HEADER_ARRAY
  criticalPath_send(env);
  automaticallySetMessagePriority(env);
#endif
}

void CProxyElement_ArrayBase::ckSend(CkArrayMessage* msg, int ep, int opts) const
{
#if CMK_ERROR_CHECKING
  // Check our array index for validity
  if (_idx.nInts < 0)
    CkAbort("Array index length is negative!\n");
  if (_idx.nInts > CK_ARRAYINDEX_MAXLEN)
    CkAbort(
        "Array index length (nInts) is too long-- did you "
        "use bytes instead of integers?\n");
#endif
  msg_prepareSend(msg, ep, ckGetArrayID());
  if (ckIsDelegated())  // Just call our delegateMgr
    ckDelegatedTo()->ArraySend(ckDelegatedPtr(), ep, msg, _idx, ckGetArrayID());
  else
  {  // Usual case: a direct send
    CkArray* localbranch = ckLocalBranch();
    if (localbranch == NULL)
    {  // array manager not created on this PE yet: hold until it is
      deferArraySend(ep, msg, ckGetArrayID(), _idx, opts & (~CK_MSG_INLINE));
    }
    else
    {
      if (opts & CK_MSG_INLINE)
        localbranch->sendMsg(msg, _idx, CkDeliver_inline, opts & (~CK_MSG_INLINE));
      else
        localbranch->sendMsg(msg, _idx, CkDeliver_queue, opts);
    }
  }
}

void* CProxyElement_ArrayBase::ckSendSync(CkArrayMessage* msg, int ep) const
{
  CkFutureID f = CkCreateAttachedFuture(msg);
  ckSend(msg, ep);
  return CkWaitReleaseFuture(f);
}

void CkBroadcastMsgSection(int entryIndex, void* msg, CkSectionID sID, int opts)
{
  CProxySection_ArrayBase sp(sID);
  sp.ckSend((CkArrayMessage*)msg, entryIndex, opts);
}

void CProxySection_ArrayBase::ckSend(CkArrayMessage* msg, int ep, int opts)
{
  if (ckIsDelegated())  // Just call our delegateMgr
    ckDelegatedTo()->ArraySectionSend(ckDelegatedPtr(), ep, msg, _sid.size(), _sid.data(),
                                      opts);
  else
  {
    // send through all
    for (int k = 0; k < _sid.size(); ++k)
    {
      for (int i = 0; i < _sid[k]._elems.size() - 1; i++)
      {
        CProxyElement_ArrayBase ap(_sid[k]._cookie.get_aid(), _sid[k]._elems[i]);
        void* newMsg = CkCopyMsg((void**)&msg);
        ap.ckSend((CkArrayMessage*)newMsg, ep, opts);
      }
      if (!_sid[k]._elems.empty())
      {
        void* newMsg = (k < _sid.size() - 1) ? CkCopyMsg((void**)&msg) : msg;
        CProxyElement_ArrayBase ap(_sid[k]._cookie.get_aid(),
                                   _sid[k]._elems[_sid[k]._elems.size() - 1]);
        ap.ckSend((CkArrayMessage*)newMsg, ep, opts);
      }
    }
  }
}

void CkSetMsgArrayIfNotThere(void* msg, CkArray_IfNotThere policy)
{
  envelope* env = UsrToEnv((void*)msg);
  env->setMsgtype(ForArrayEltMsg);
  CkArrayMessage* m = (CkArrayMessage*)msg;
  m->array_setIfNotThere(policy);
}

void _ckArrayFlushDeferredSends(CkGroupID mgr)
{
  DeferredArraySends& table = CkpvAccess(deferredArraySends);
  auto it = table.find(mgr.idx);
  if (it == table.end())
    return;
  std::vector<DeferredArraySend> sends = std::move(it->second);
  table.erase(it);
  for (const DeferredArraySend& s : sends)
    CkSendMsgArray(s.entryIndex, s.msg, CkArrayID(mgr), s.idx, s.opts);
}

void CkSendMsgArray(int entryIndex, void* msg, CkArrayID aID, const CkArrayIndex& idx,
                    int opts)
{
  CkArrayMessage* m = (CkArrayMessage*)msg;
  msg_prepareSend(m, entryIndex, aID);
  CkArray* a = (CkArray*)_localBranch(aID);
  if (a == NULL)
    deferArraySend(entryIndex, msg, aID, idx, opts);
  else
    a->sendMsg(m, idx, CkDeliver_queue, opts);
}

void CkSendMsgArrayInline(int entryIndex, void* msg, CkArrayID aID,
                          const CkArrayIndex& idx, int opts)
{
  CkArrayMessage* m = (CkArrayMessage*)msg;
  msg_prepareSend(m, entryIndex, aID);
  CkArray* a = (CkArray*)_localBranch(aID);
  if (a == NULL)
  {
    // No manager yet, so no inline delivery: held and sent as a queued message.
    deferArraySend(entryIndex, msg, aID, idx, opts);
    return;
  }
  int oldStatus = CkDisableTracing(entryIndex);  // avoid nested tracing
  // TODO: Why does it not change opts to match inline delivery?
  a->sendMsg(m, idx, CkDeliver_inline, opts);
  if (oldStatus)
    CkEnableTracing(entryIndex);
}

/*********************** CkArray Reduction *******************/
CkArrayReducer::CkArrayReducer(CkGroupID mgrID_)
    : CkArrayListener(sizeof(contributorInfo) / sizeof(int)), mgrID(mgrID_)
{
  mgr = CProxy_CkReductionMgr(mgrID).ckLocalBranch();
}
CkArrayReducer::CkArrayReducer(CkMigrateMessage* m) : CkArrayListener(m) { mgr = NULL; }
void CkArrayReducer::pup(PUP::er& p)
{
  CkArrayListener::pup(p);
  p | mgrID;
  if (p.isUnpacking())
    mgr = CProxy_CkReductionMgr(mgrID).ckLocalBranch();
}
CkArrayReducer::~CkArrayReducer() {}

/*********************** CkArray Broadcast ******************/

CkArrayBroadcaster::CkArrayBroadcaster(bool stableLocations_, bool broadcastViaScheduler_)
    : CkArrayListener(1),  // Each array element carries a broadcast number
      stableLocations(stableLocations_),
      broadcastViaScheduler(broadcastViaScheduler_)
{
}

CkArrayBroadcaster::CkArrayBroadcaster(CkMigrateMessage* m)
    : CkArrayListener(m), broadcastViaScheduler(false)
{
}

void CkArrayBroadcaster::pup(PUP::er& p)
{
  CkArrayListener::pup(p);
  /* Assumption: no migrants during checkpoint, so no need to
     save old broadcasts. */
  p | stableLocations;
  p | broadcastViaScheduler;
  p | bcastSendEpoch;
  p | storedBcasts;
}

// Processes the incoming broadcast message and buffers it for future delivery
void CkArrayBroadcaster::ingestIncoming(CkArrayMessage* msg)
{
  const int bcastEpoch = UsrToEnv(msg)->getGroupEpoch();
  DEBB((AA "Received broadcast %d\n" AB, bcastEpoch));

  CmiMemoryMarkBlock(((char*)UsrToEnv(msg)) - sizeof(CmiChunkHeader));

  // If this is a ZC all done msg, then the message should already be in storedMsgs, so
  // check and return
  if (CMI_ZC_MSGTYPE(UsrToEnv(msg)) == CMK_ZC_BCAST_RECV_ALL_DONE_MSG)
  {
    CkAssert(storedBcasts.hasBcast(bcastEpoch));
    return;
  }

  storedBcasts.insert(msg, bcastEpoch);
}

// Attempt to deliver the given broadcast to the given element, continuing to deliver
// additional broadcasts to that element if possible
bool CkArrayBroadcaster::attemptDelivery(CkArrayMessage* bcast, ArrayElement* el,
                                         bool doFree)
{
  int& elBcastNo = getData(el);
  const int bcastEpoch = UsrToEnv(bcast)->getGroupEpoch();

  DEBB((AA "Attempting to deliver broadcast %d to element %s (el bcast#:%d)\n" AB,
        bcastEpoch, idx2str(el), elBcastNo));

  // If this is a ZC all done msg, then we should already have seen this epoch since we're
  // now at the last part of the ZC delivery, so don't increment the elBcastNo
  if (CMI_ZC_MSGTYPE(UsrToEnv(bcast)) == CMK_ZC_BCAST_RECV_ALL_DONE_MSG)
  {
    CkAssert(elBcastNo > bcastEpoch);
  }
  else
  {
    // if this array element already received this message, skip it
    // if this array element can move and hasn't reached this bcast epoch yet, also skip
    // it for now, it'll be delivered when it catches up
    if (elBcastNo != bcastEpoch)
      return false;
    elBcastNo++;
  }

  if (!performDelivery(bcast, el, doFree))
    return false;
  // Keep delivering broadcasts while we have the next one in sequence
  return bringUpToDate(el);
}

// Deliver all deliverable stored broadcasts to the given element
bool CkArrayBroadcaster::bringUpToDate(ArrayElement* el)
{
  int& elBcastNo = getData(el);
  // A stable-locations array frees a broadcast once every local element has it,
  // on the promise that no element moves while a broadcast is in flight
  // (doc/internals/array-broadcast-migration.md)
  if (stableLocations && elBcastNo < storedBcasts.getHeadEpoch())
    CkAbort("Array element %s arrived on PE %d expecting broadcast %d, which this PE "
            "freed (next kept: %d). An element of an array with static insertion and "
            "setAnytimeMigration(false) moved while a broadcast to the array was in "
            "flight, which such an array does not allow.\n",
            idx2str(el), CkMyPe(), elBcastNo, storedBcasts.getHeadEpoch());
  // Keep delivering broadcasts while we have the next one in sequence
  while (storedBcasts.hasBcast(elBcastNo))
  {
    CkArrayMessage* curBcast = storedBcasts.getBcast(elBcastNo);
    elBcastNo++;
    const bool curDoFree =
        CMI_ZC_MSGTYPE(UsrToEnv(curBcast)) == CMK_ZC_BCAST_RECV_ALL_DONE_MSG;
    const bool curStatus = performDelivery(curBcast, el, curDoFree);
    if (!curStatus)
      return false;
  }
  return true;
}

// Deliver broadcast to the given element without an epoch check
bool CkArrayBroadcaster::performDelivery(CkArrayMessage* bcast, ArrayElement* el,
                                         bool doFree)
{
  int& elBcastNo = getData(el);
  DEBB((AA "Delivering broadcast %d to element %s\n" AB, elBcastNo, idx2str(el)));

  CkAssert(UsrToEnv(bcast)->getMsgtype() == ArrayBcastFwdMsg);

  if (!broadcastViaScheduler)
    return el->ckInvokeEntry(bcast->array_ep_bcast(), bcast, doFree);
  else
  {
    envelope* env = UsrToEnv(bcast);
    if (!doFree)
    {
      CkArrayMessage* newMsg = env->isPacked()
                                   ? (CkArrayMessage*)CkCopyPackedMsg(bcast)
                                   : (CkArrayMessage*)CkCopyMsg((void**)&bcast);
      bcast = newMsg;
    }
    else if (env->isPacked())
    {
      CkUnpackMessage(&env);
      bcast = (CkArrayMessage*)EnvToUsr(env);
    }
    env = UsrToEnv(bcast);
    env->setRecipientID(el->ckGetID());
    CkArrayManagerDeliver(CkMyPe(), bcast, 0);
    return true;
  }
}

#if CMK_CHARM4PY

extern void (*ArrayBcastRecvExtCallback)(int, int, int, int, int*, int, int, char*, int);

void CkArrayBroadcaster::deliverAndUpdate(CkArrayMessage* bcast,
                                          std::vector<CkMigratable*>& elements,
                                          int arrayId)
{
  if (elements.empty())
    return;

  bool success = performDelivery(bcast, elements, arrayId);

  if (success)
  {
    int bcastEpoch = UsrToEnv(bcast)->getGroupEpoch() + 1;
    while (storedBcasts.hasBcast(bcastEpoch))
    {
      success = performDelivery(storedBcasts.getBcast(bcastEpoch), elements, arrayId);
      bcastEpoch++;
    }
  }
}

bool CkArrayBroadcaster::performDelivery(CkArrayMessage* bcast,
                                         std::vector<CkMigratable*>& elements,
                                         int arrayId)
{
  const int bcastEpoch = UsrToEnv(bcast)->getGroupEpoch();

  CkAssert(UsrToEnv(bcast)->getMsgtype() == ArrayBcastFwdMsg);

  ArrayElement* el = (ArrayElement*)elements[0];
  // get number of dimensions and number of ints used by CkArrayIndex of this array
  const int numDim = el->thisIndexMax.getDimension();
  const int numInts = el->thisIndexMax.nInts;
  // store array index data of elements that are going to receive the broadcast, to pass
  // to Charm4py
  std::vector<int> validIndexes(elements.size() * numInts);
  int numValidElements = 0;
  int j = 0;
  for (CkMigratable* m : elements)
  {
    ArrayElement* el = (ArrayElement*)m;
    int& elBcastNo = getData(el);
    // if this array element already received this message, skip it
    // if this array element can move and hasn't reached this bcast epoch yet, also skip
    // it for now, it'll be delivered when it catches up
    if (elBcastNo != bcastEpoch)
      continue;
    elBcastNo++;
    DEBB((AA "Delivering broadcast %d to element %s\n" AB, bcastEpoch, idx2str(el)));
    int* index = el->thisIndexMax.data();
    for (int i = 0; i < numInts; i++) validIndexes[j++] = index[i];
    numValidElements++;
  }

  if (numValidElements > 0)
  {
    char* msg_buf = ((CkMarshallMsg*)bcast)->msgBuf;
    PUP::fromMem implP(msg_buf);
    int msgSize;
    implP | msgSize;
    int ep;
    implP | ep;
    int dcopy_start;
    implP | dcopy_start;
    ArrayBcastRecvExtCallback(arrayId, numDim, numInts, numValidElements,
                              validIndexes.data(), ep, msgSize,
                              msg_buf + (3 * sizeof(int)), dcopy_start);
  }

  return numValidElements > 0;
}

#endif

void CkArrayBroadcaster::springCleaning(void) { storedBcasts.springCleaning(); }

// Stable locations: no element will ever need a broadcast that every local
// element has received, so free those now instead of at spring cleaning.
void CkArrayBroadcaster::releaseDelivered()
{
  if (!stableLocations)
    return;
  int below = INT_MAX;
  for (CkMigratable* m : owner->localElemVec)
    below = std::min(below, getData((ArrayElement*)m));
  storedBcasts.releaseBelow(below);
}

bool CkArrayBroadcaster::isFinalRecipient(const std::vector<CkMigratable*>& elements,
                                          ArrayElement* el, const int epoch)
{
  if (getData(el) != epoch)
    return false;
  for (CkMigratable* m : elements)
    if (m != el && getData((ArrayElement*)m) <= epoch)
      return false;
  return true;
}

void CkArrayBroadcaster::flushState() { storedBcasts.clear(); }

void CkBroadcastMsgArray(int entryIndex, void* msg, CkArrayID aID, int opts)
{
  CProxy_ArrayBase ap(aID);
  ap.ckBroadcast((CkArrayMessage*)msg, entryIndex, opts);
}

void CProxy_ArrayBase::ckBroadcast(CkArrayMessage* msg, int ep, int opts) const
{
  envelope* env = UsrToEnv(msg);
  env->setMsgtype(ArrayBcastMsg);
  msg->array_ep_bcast() = ep;
  if (ckIsDelegated())
  {
    // Just call our delegateMgr
    ckDelegatedTo()->ArrayBroadcast(ckDelegatedPtr(), ep, msg, _aid);
  }
  else
  {
    // Broadcast message via serializer node
    _TRACE_CREATION_DETAILED(UsrToEnv(msg), ep);
    static constexpr int serializer = 0;
    int skipsched = opts & CK_MSG_EXPEDITED;
    CProxy_CkArray ap(_aid);

    if (CkMyPe() != serializer)
    {
      DEBB((AA "Forwarding array broadcast to serializer node %d\n" AB, serializer));
      if (CMI_ZC_MSGTYPE(env) == CMK_ZC_BCAST_SEND_MSG ||
          CMI_ZC_MSGTYPE(env) == CMK_ZC_BCAST_RECV_MSG)
      {
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
        return;
      }
    }
    else
    {
      DEBB((AA "Sending array broadcast\n" AB));
    }
    // Regular Bcast (non ZC) is implemented on non-zero root nodes by
    // forwarding the message to PE 0 and then having PE 0 perform the
    // broadcast rooted at Node 0. This is done to ensure single delivery
    // (and avoid multiple delivery or no delivery of the message) when
    // load balancing occurs during a broadcast

    if (skipsched && _entryTable[ep]->noKeep)
    {
      ap[serializer].sendNoKeepExpeditedBroadcast(msg);
    }
    else if (skipsched)
    {
      ap[serializer].sendExpeditedBroadcast(msg);
    }
    else if (_entryTable[ep]->noKeep)
    {
      ap[serializer].sendNoKeepBroadcast(msg);
    }
    else
    {
      ap[serializer].sendBroadcast(msg);
    }
  }
}

void CkArray::incrementBcastNoAndSendBack(int srcPe, MsgPointerWrapper w)
{
  // increment bcastNo
  w.epoch = broadcaster->incBcastSendEpoch();
  // Send back to CkArray on that index
  thisProxy[srcPe].sendZCBroadcast(w);
}

void CkArray::sendZCBroadcast(MsgPointerWrapper w)
{
  int skipsched = w.opts & CK_MSG_EXPEDITED;
  int nokeep = _entryTable[w.ep]->noKeep;
  UsrToEnv(w.msg)->setGroupEpoch(w.epoch);
  if (skipsched && nokeep)
  {
    thisProxy.recvNoKeepExpeditedBroadcast((CkArrayMessage*)(w.msg));
  }
  else if (skipsched)
  {
    thisProxy.recvExpeditedBroadcast((CkArrayMessage*)(w.msg));
  }
  else if (nokeep)
  {
    thisProxy.recvNoKeepBroadcast((CkArrayMessage*)(w.msg));
  }
  else
  {
    thisProxy.recvBroadcast((CkArrayMessage*)(w.msg));
  }
}

/// Reflect a broadcast off this Pe:
void CkArray::sendBroadcast(CkMessage* msg)
{
  CK_MAGICNUMBER_CHECK
  static constexpr int serializer = 0;
  // TODO: is this recheck necessary? If so, it's necessary in the others too
  if (CkMyPe() == serializer)
  {
    // Broadcast the message to all processors
    UsrToEnv(msg)->setMsgtype(ArrayBcastMsg);
    UsrToEnv(msg)->setGroupEpoch(broadcaster->incBcastSendEpoch());
    thisProxy.recvBroadcast(msg);
  }
  else
  {
    thisProxy[serializer].sendBroadcast(msg);
  }
}

void CkArray::sendNoKeepExpeditedBroadcast(CkMessage* msg)
{
  CK_MAGICNUMBER_CHECK
  // Broadcast the message to all processors
  UsrToEnv(msg)->setMsgtype(ArrayBcastMsg);
  UsrToEnv(msg)->setGroupEpoch(broadcaster->incBcastSendEpoch());
  thisProxy.recvNoKeepExpeditedBroadcast(msg);
}

void CkArray::sendExpeditedBroadcast(CkMessage* msg)
{
  CK_MAGICNUMBER_CHECK
  // Broadcast the message to all processors
  UsrToEnv(msg)->setMsgtype(ArrayBcastMsg);
  UsrToEnv(msg)->setGroupEpoch(broadcaster->incBcastSendEpoch());
  thisProxy.recvExpeditedBroadcast(msg);
}

void CkArray::sendNoKeepBroadcast(CkMessage* msg)
{
  CK_MAGICNUMBER_CHECK
  // Broadcast the message to all processors
  UsrToEnv(msg)->setMsgtype(ArrayBcastMsg);
  UsrToEnv(msg)->setGroupEpoch(broadcaster->incBcastSendEpoch());
  thisProxy.recvNoKeepBroadcast(msg);
}

void CkArray::recvBroadcastViaTree(CkMessage* msg) {}

/// Increment broadcast count; deliver to all local elements
void CkArray::recvBroadcast(CkMessage* m)
{
  CK_MAGICNUMBER_CHECK
  CkArrayMessage* msg = (CkArrayMessage*)m;
  envelope* env = UsrToEnv(msg);

  // A keep entry gets its own copy per local element, and the broadcast is also
  // stored for elements that arrive later. Pack it once, here, and make every copy
  // from the packed bytes (CkCopyPackedMsg). Copying the unpacked message instead
  // packs and re-unpacks the source per copy (CkCopyMsg), which for a custom
  // pack/unpack pair replaces the source object while this loop and the stored
  // copy still hold the old pointer. Zerocopy broadcasts keep their pointers live
  // in the message, so they are left unpacked.
  if (CMI_ZC_MSGTYPE(env) == CMK_REG_NO_ZC_MSG && !env->isPacked() &&
      !_entryTable[msg->array_ep_bcast()]->noKeep)
  {
    CkPackMessage(&env);
    msg = (CkArrayMessage*)EnvToUsr(env);
  }

  // Process the incoming message, buffers if necessary
  broadcaster->ingestIncoming(msg);

  int len = localElemVec.size();
  // extract these fields here so we can still use them even if msg is freed
  const auto zc_msgtype = CMI_ZC_MSGTYPE(env);
  const int epoch = env->getGroupEpoch();

  // Attempt to actually deliver the broadcast
  if (zc_msgtype == CMK_ZC_BCAST_RECV_ALL_DONE_MSG && len > 0)
  {
    // Message contains pointers to the posted buffer, which contains the data
    // received
    // All operations done, already consumed by other array elements, now
    // deliver to the first element

    bool doFree = true;  // free it since all ops are done
    broadcaster->attemptDelivery(msg, (ArrayElement*)localElemVec[0], doFree);
    // The store held this message through the transfer; the delivery freed it
    broadcaster->consumeBcast(epoch);
  }
  else if (zc_msgtype == CMK_ZC_BCAST_RECV_MSG && len > 0)
  {
    // Message is used by the receiver to post the receiver buffer
    // Initial metadata message, send only to the first element, other elements
    // are sent CMK_ZC_BCAST_RECV_DONE_MSG after rget completion

    // do not free since msg will be reused to send buffers to peers, msg will
    // be finally freed by the first element in the
    // CMK_ZC_BCAST_RECV_ALL_DONE_MSG branch
    bool doFree = false;
    // The transfer keeps using this message until its ALL_DONE delivery
    broadcaster->holdBcast(epoch);
    broadcaster->attemptDelivery(msg, (ArrayElement*)localElemVec[0], doFree);
  }
  else
  {
#if CMK_CHARM4PY
    broadcaster->deliverAndUpdate(msg, localElemVec, thisgroup.idx);
#else
    // Do not free if CMK_ZC_BCAST_RECV_DONE_MSG, since it'll be freed by the
    // first element during CMK_ZC_BCAST_ALL_DONE_MSG
    if (zc_msgtype == CMK_ZC_BCAST_RECV_DONE_MSG) {
      updateTagArray(env, localElemVec.size());
    }
    // Deliver in reverse order in case the target method destroys and removes
    // the element from localElemVec
    for (int i = len - 1; i >= 0; --i)
    {
      bool doFree = false;
      // On a stable-locations array the store would free the message right
      // after this delivery, so the last element to receive it takes the
      // original instead of a copy. A CMK_ZC_BCAST_RECV_DONE_MSG is freed by
      // the first element at CMK_ZC_BCAST_RECV_ALL_DONE_MSG; a send-done
      // message carries its payload inline and is an ordinary message here.
      if (i == 0 && stableLocations &&
          (zc_msgtype == CMK_REG_NO_ZC_MSG || zc_msgtype == CMK_ZC_SEND_DONE_MSG) &&
          broadcaster->isFinalRecipient(localElemVec, (ArrayElement*)localElemVec[0], epoch))
        doFree = true;
      CmiAssert(i < localElemVec.size());
      broadcaster->attemptDelivery(msg, (ArrayElement*)localElemVec[i], doFree);
      if (doFree)
        broadcaster->consumeBcast(epoch);
    }

#endif  // CMK_CHARM4PY
  }

  broadcaster->releaseDelivered();
}

void CkArray::forwardZCMsgToOtherElems(envelope* env)
{
  CMI_ZC_MSGTYPE(env) = CMK_ZC_BCAST_RECV_DONE_MSG;

  int len = localElemVec.size();

  for (unsigned int i = 1; i < len; ++i)
  {  // Send to all elements except the first element
    bool doFree = false;
    broadcaster->attemptDelivery((CkArrayMessage*)EnvToUsr(env), (ArrayElement*)localElemVec[i],
                         doFree);
  }
  broadcaster->releaseDelivered();
}

void CkArray::forwardZCMsgToSpecificElem(envelope *env, CkMigratable *elem) {
  bool doFree = false;
  broadcaster->performDelivery((CkArrayMessage *)EnvToUsr(env), (ArrayElement*)elem, doFree);
}

void CkArray::flushStates()
{
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

void CkArray::ckDestroy()
{
  isDestroying = true;
  // Set the duringDestruction flag in the location manager. This is used to
  // indicate that the location manager is going to be destroyed so don't need
  // to send messages to remote PEs with reclaimRemote messages.
  locMgr->setDuringDestruction(true);

  // ckDestroy deletes the CkMigratable, which also removes it from this vector
  while (!localElemVec.empty())
  {
    localElemVec.back()->ckDestroy();
  }

  locMgr->deleteManager(CkGroupID(thisProxy), this);
  if (!mCastMgrID.isZero())
  {
    delete _localBranch(mCastMgrID);
    mCastMgrID.setZero();
  }
  delete this;
}

// We are trying to send a message to an element of this array. This is the origin of
// message sends, and is only called once and only on the source PE.
// If we know the ID and location of the element, then we send the message to that PE.
// If we don't know either the ID or the location, we call handleUnknown to either buffer
// the message, forward it, or trigger demand creation of the element.
void CkArray::sendMsg(CkArrayMessage* msg, const CkArrayIndex& idx, CkDeliver_t type,
                      int opts)
{
  envelope* env = UsrToEnv(msg);
  env->setMsgtype(ForArrayEltMsg);
  _TRACE_CREATION_DETAILED(env, msg->array_ep());

  CmiUInt8 id;
  if (locMgr->lookupID(idx, id))
  {
    // We know the ID, so fill in the rest of the envelope to allow for sending
    env->setRecipientID(makeID(id));
    int dest = locMgr->whichPe(id);
    if (dest != -1)
    {
      // We know the ID AND the location, so we can send the message as normal.
      sendToPe(msg, dest, type, opts);
    }
    else
    {
      // We know the ID but the location is unknown. This means the message can be sent,
      // but we don't know where to send it.
      handleUnknown(msg, idx, type, opts);
    }
  }
  else
  {
    // We don't know the ID, so set a sentinel ID to prevent the message from being sent
    // then handle the unknown (which will buffer or demand create).
    env->setRecipientID(0);
    handleUnknown(msg, idx, type, opts);
  }
}

// We have just received a message for an element of this array. If the element exists
// here, then deliver the message. If the element does not exist here, then we follow
// similar logic as sendMsg to forward the message, buffer it, or trigger demand creation.
void CkArray::recvMsg(CkArrayMessage* msg, CmiUInt8 id, CkDeliver_t type, int opts)
{
  msg->array_hops()++;

  // First, if this is the actual location of the element, just deliver the message
  // right away and completely avoid location management.
  ArrayElement* elem = lookup(id);
  if (elem)
  {
    deliverToElement(msg, elem);
  }
  else
  {
    // If the object is not here, figure out where we think it is and forward the message
    int pe = locMgr->whichPe(id);
    if (pe == -1)
    {
      // The element is unknown to us. If we are its home, then it just means it hasn't
      // been created yet (or has been deleted). If we are not the home this can still
      // occur if we knew the element but it has been deleted or our location cache has
      // been purged. Either way the index is not needed: the home is computable
      // from the id.
      handleUnknownByID(msg, id, type, opts);
    }
    else
    {
      // If we haven't found the element in two tries, send it back home rather than
      // chase a chain of stale location entries; the home always knows. (Sound now
      // that the home of an id and the home of its index are the same PE.) The home
      // itself must follow its own entry: redirecting there would send the message
      // to this PE again, forever.
      if (msg->array_hops() > 1)
      {
        const int home = locMgr->homePe(id);
        if (CkMyPe() != home) pe = home;
      }
      sendToPe(msg, pe, type, opts);
    }
  }
}

void CkArray::recordSend(const CmiUInt8 id, const unsigned int bytes, int pe, const int opts)
{
#if CMK_LBDB_ON
  if (!(opts & CK_MSG_LB_NOTRACE) && locMgr->getLBMgr()->CollectingCommStats())
  {
    // LB deals in IDs with collection information only when CMK_GLOBAL_LOCATION_UPDATE
    // is enabled, so add the group information if so.
#  if CMK_GLOBAL_LOCATION_UPDATE
    // The id the element registered with the LB (CkLocRec): the manager's, which
    // differs from this array's for a bound array.
    const CmiUInt8 lbObjId = locMgr->makeID(id).getID();
#  else
    const CmiUInt8 lbObjId = id;
#  endif
    locMgr->getLBMgr()->Send(locMgr->getOMHandle(), lbObjId, bytes, pe, 1);
  }
#endif
}

// Send the msg to the designated PE. This is a private entry method and only called by
// other methods in CkArray when the message is ready to send to the specified PE.
// If pe is this PE then we know the object is supposed to be located here, but it is
// possible that it has either been deleted or not been created yet.
void CkArray::sendToPe(CkArrayMessage* msg, int pe, CkDeliver_t type, int opts)
{
  // This method should only be called for a valid PE, and with a properly filled in msg.
  CkAssert(pe >= 0 && pe < CkNumPes());
  CkAssert(thisgroup == UsrToEnv(msg)->getArrayMgr());
  CkAssert(UsrToEnv(msg)->getRecipientID() != 0);

  // If the message is not for me, or is supposed to be queued, send it via the normal
  // queuing infrastructure
  if (pe != CkMyPe() || type == CkDeliver_queue)
  {
#if CMK_LBDB_ON
    // Track message sends if the LB framework is collecting comm stats
    if (msg->array_hops() == 0 && !(opts & CK_MSG_LB_NOTRACE) &&
        locMgr->getLBMgr()->CollectingCommStats())
    {
      recordSend(msg->array_element_id(), UsrToEnv(msg)->getTotalsize(), pe, opts);
    }
#endif
    CkArrayManagerDeliver(pe, msg, opts);
  }
  else
  {
    // If it is for me, and inline delivery, attempt to invoke the entry method
    // NOTE: We should only end up here when an inline entry method is called via callback
    // or when a buffered message is sent from this PE to this PE. Normal inline sends are
    // handled directly in the .ci file via generated code.
    CmiUInt8 id = msg->array_element_id();
    ArrayElement* elem = lookup(id);
    if (elem == nullptr)
    {
      // The element has not yet been created but the location is known. This means the
      // element is bound to a sibling that has already been created here.
      if (msg->array_ifNotThere() == CkArray_IfNotThere_buffer)
      {
        // Directly buffer. Since we are the location of a sibling, we don't need to make
        // a location request.
        bufferedIDMsgs[id].push_back(msg);
        return;
      }
      else
      {
        // Directly demand create. Since we are the location of a sibling, we don't need
        // to request permission from home.
        const CkArrayIndex& idx = locMgr->lookupIdx(id);
        CkAssert(!locMgr->isRemote(id));
        int chareType = _entryTable[msg->array_ep()]->chareIdx;
        int ctor = _chareTable[chareType]->getDefaultCtor();
        CkAssertMsg(ctor != -1,
            "Can't demand create an element with no default ctor in the .ci file\n");
        demandCreateElement(idx, ctor);
        // The element now exists here; the pointer looked up above predates it.
        elem = lookup(id);
        if (elem == nullptr)
          CkAbort("CkArray::deliverInline: demand creation of bound element id %" PRIx64
                  " on PE %d did not create it\n", id, CkMyPe());
      }
    }
#if CMK_LBDB_ON
    // This ensures that communication is tracked even for inline sends
    if (msg->array_hops() == 0 && !(opts & CK_MSG_LB_NOTRACE) &&
        locMgr->getLBMgr()->CollectingCommStats())
    {
      recordSend(msg->array_element_id(), UsrToEnv(msg)->getTotalsize(), pe, opts);
    }
#endif
    deliverToElement(msg, elem);
  }
}

// We have a message for the element. Invoke the appropriate entry method.
void CkArray::deliverToElement(CkArrayMessage* msg, ArrayElement* elem)
{
  CkAssert(elem);
  // The element already existed, or was literally just created via demand creation.
  // If the message has multiple hops, trigger location updates to the message source.
  if (msg->array_hops() > 1)
    locMgr->multiHop(msg);

  // Invoke the entry method
  elem->ckInvokeEntry(msg->array_ep(), (void*)msg, true);
}

// Handle a message for an element whose index this PE cannot name (recvMsg): the
// same choices as handleUnknown below, sourced entirely from the id. An id-addressed
// message buffers against bufferedIDMsgs anyway; bufferForLocation only needs the
// index for messages that carry no id.
void CkArray::handleUnknownByID(CkArrayMessage* msg, CmiUInt8 id, CkDeliver_t type,
                                int opts)
{
  envelope* env = UsrToEnv(msg);
  // Only id-addressed messages arrive here; without an id there is nothing to route on.
  CkAssert(env->getRecipientID() != 0);
  const bool isSmall = env->getTotalsize() < _messageBufferingThreshold;
  const int home = locMgr->homePe(id);
  const int ifNotThere = msg->array_ifNotThere();

  // Hashed kind, an id this PE knows as dead or rebound (retiredIds): re-address to
  // the index's current id if there is a newer one; at the home, teach the sender by
  // index, and hold a message for an index without an element until one registers.
  // doc/internals/object-ids.md, section 3.
  {
    CkArrayIndex idx;
    if (locMgr->recoverIndex(id, idx))
    {
      CmiUInt8 current;
      if (locMgr->lookupID(idx, current) && current != id)
      {
        env->setRecipientID(makeID(current));
        const int srcPe = msg->array_getSrcPe();
        if (CkMyPe() == home && srcPe != CkMyPe())
          locMgr->requestLocation(idx, srcPe);
        recvMsg(msg, current, type, opts);
        return;
      }
      if (CkMyPe() == home && ifNotThere == CkArray_IfNotThere_buffer)
      {
        env->setRecipientID(0);
        bufferForLocation(msg, idx);
        return;
      }
    }
  }

  if (ifNotThere != CkArray_IfNotThere_buffer)
  {
    // Demand creation. The index-keyed path knows how to get the element created
    // (at the home for createhome, at the original sender for createhere), and it
    // needs the index: recover it where this PE can. Otherwise only the home can
    // (it keeps idx -> id for every element it is home for), so hand the message
    // there. Parking it here would wait for a location that nothing will ever
    // report, since the element does not exist.
    CkArrayIndex idx;
    if (locMgr->recoverIndex(id, idx))
    {
      handleUnknown(msg, idx, type, opts);
      return;
    }
    if (CkMyPe() != home)
    {
      sendToPe(msg, home, type, opts);
      return;
    }
    CkAbort("CkArray::handleUnknownByID: demand creation requested for element id"
            " %" PRIx64 " of chare array %d, whose index its home PE %d cannot"
            " recover\n", id, thisgroup.idx, CkMyPe());
  }

  // Same forwarding rule as handleUnknown: hand a small message to the home, which
  // either knows the location or will learn it.
  if (isSmall && CkMyPe() != home)
  {
    // The delivering PE sends this PE the location (multiHop on a forwarded
    // message), so later sends go direct.
    sendToPe(msg, home, type, opts);
    return;
  }

  // Otherwise hold it here until the location manager resolves the id. The request
  // can find the location already written by another PE of this process.
  if (bufferedIDMsgs.find(id) == bufferedIDMsgs.end())
  {
    const int pe = locMgr->requestLocation(id);
    if (pe != -1)
    {
      sendToPe(msg, pe, type, opts);
      return;
    }
  }
  bufferedIDMsgs[id].push_back(msg);
}

// Handle a message to an unknown destination. If we at least know the ID, we have the
// option to send the message to the elements home. If we don't know that, the message
// must be buffered or trigger demand creation.
void CkArray::handleUnknown(CkArrayMessage* msg, const CkArrayIndex& idx,
                            CkDeliver_t type, int opts)
{
  envelope* env = UsrToEnv(msg);
  // TODO: Make sure this is actually a sentinel ID
  bool hasID = env->getRecipientID() != 0;
  bool isSmall = env->getTotalsize() < _messageBufferingThreshold;
  int home = locMgr->homePe(idx);
  if (msg->array_ifNotThere() == CkArray_IfNotThere_buffer)
  {
    if (isSmall && hasID && CkMyPe() != home)
    {
      // The delivering PE sends this PE the location (multiHop on a forwarded message).
      sendToPe(msg, home, type, opts);
    }
    else
    {
      bufferForLocation(msg, idx);
    }
  }
  else
  {
    // This is a message that utilizes demand creation
    if (isSmall && hasID && CkMyPe() != home &&
        msg->array_ifNotThere() != CkArray_IfNotThere_createhere)
    {
      // Send the message home where it will trigger demand creation, or get delivered to
      // the element if it already exists (either way the delivery repairs this PE's cache)
      sendToPe(msg, home, type, opts);
    }
    else
    {
      // Buffer the message here and query the home PE to see if demand creation is
      // required
      bufferForCreation(msg, idx);
    }
  }
}

// Buffer the message to be sent when we know the location, and ask for the location
// if no request is out yet. A message whose index this process has an id for is held
// under that id, where the process's waiter bits release it; only an index without
// an id is held by index, released by the home's reply by index.
void CkArray::bufferForLocation(CkArrayMessage* msg, const CkArrayIndex& idx)
{
  CmiUInt8 id = msg->array_element_id();
  // An id another process addressed may be one this process has rebound since
  // (hashed kind), and an index without an id may have one here: hold the message
  // under the index's current id.
  CmiUInt8 current;
  const bool hasId = locMgr->lookupID(idx, current);
  if (hasId && current != id)
  {
    UsrToEnv(msg)->setRecipientID(makeID(current));
    id = current;
  }
  if (!hasId && UsrToEnv(msg)->getRecipientID() == 0)
  {
    bufferedIndexMsgs[idx].push_back(msg);
    if (bufferedIndexMsgs[idx].size() == 1) locMgr->requestLocation(idx);
    return;
  }

  // Messages held by index before this process learned its id join the id's queue.
  const bool fresh = bufferedIDMsgs.find(id) == bufferedIDMsgs.end();
  auto byIndex = bufferedIndexMsgs.find(idx);
  if (byIndex != bufferedIndexMsgs.end())
  {
    for (CkArrayMessage* m : byIndex->second)
    {
      UsrToEnv(m)->setRecipientID(makeID(id));
      bufferedIDMsgs[id].push_back(m);
    }
    bufferedIndexMsgs.erase(byIndex);
  }
  if (fresh)
  {
    const int pe = locMgr->requestLocation(id);
    if (pe != -1)
    {
      if (bufferedIDMsgs.find(id) != bufferedIDMsgs.end()) sendBufferedMsgs(id, pe);
      sendToPe(msg, pe, CkDeliver_queue);
      return;
    }
  }
  bufferedIDMsgs[id].push_back(msg);
}

// Demand creation has 4 possible outcomes:
// 1. Object doesn't exist yet, and it's createhome -> Create the element at its home
// 2. Object doesn't exist yet, and it's createhere -> Check with home, then create here
// 3. Object doesn't exist yet, but a bound sibling does -> Create with the sibling
// 4. Object exists -> send the message as normal
// NOTE: The only PE that knows if the object has been created or not is home. Furthermore
// if a bound sibling has been created, it will appear as though the object exists until
// the message reaches the sibling. So until that point, the message will follow the
// normal send path.
// TODO: What if an element is created with a regular constructor call, but that creation
// message doesn't reach home until after a demand creation message reached home?  In that
// case the object would already exist and the constructor could not be called.  This may
// mean that demand creation is just not compatible with regular element creation, but if
// that's the case it should be disabled in the proxy or something. Although we do have
// multiple steps to constructing an object. Would invoking the constructor entry method
// after the element has already been created be incorrect? Probably.

// This is the method that actually does the creation of the local object. Inside of
// insertElement there is logic for forwarding the creation if the object has migrated.
// TODO: This and the other instances of creating an element can probably be combined
// TODO: Why is there a field in the message for what to do when the element is not there?
// Seems like sinces it's a property of the entry method, not the msg, we could just look
// it up and save space in the message.
// TODO: THe need for demand creation paths to need idx is flimsy in most cases. May be
// able to eliminate a bit more reliance on idx, which would trickle up to handle
// unknown stuff as well. The only time we have to rely on just idx is on the original
// sending PE.
void CkArray::demandCreateElement(const CkArrayIndex& idx, int ctor)
{
  CkArrayMessage* m = (CkArrayMessage*)CkAllocSysMsg();
  envelope* env = UsrToEnv(m);
  env->setMsgtype(ArrayEltInitMsg);
  env->setArrayMgr(thisgroup);
  int listenerData[CK_ARRAYLISTENER_MAXLEN];
  prepareCtorMsg(m, listenerData);
  m->array_ep() = ctor;

  DEBC((AA "Demand-creating %s\n" AB, idx2str(idx)));
  insertElement(m, idx, listenerData);
}

void CkArray::bufferForCreation(CkArrayMessage* msg, const CkArrayIndex& idx)
{
  // Only send a request if we haven't already requested
  if (bufferedCreationMsgs.find(idx) == bufferedCreationMsgs.end())
  {
    // Figure out the constructor to call
    int chareType = _entryTable[msg->array_ep()]->chareIdx;
    int ctor = _chareTable[chareType]->getDefaultCtor();
    CkAssertMsg(ctor != -1,
        "Can't demand create an element with no default ctor in the .ci file\n");

    // Figure out the pe we are requesting for
    int home = locMgr->homePe(idx);
    int pe = home;
    if (msg->array_ifNotThere() == CkArray_IfNotThere_createhere)
      pe = UsrToEnv(msg)->getsetArraySrcPe();

    // Send the request to the target PE
    thisProxy[home].requestDemandCreation(idx, ctor, pe);
    // The request carries no reply for the requester: the home creates the element
    // (or finds it exists) and, for createhome, tells nobody. Ask the home for the
    // location by index as well; it buffers that request until the element exists and
    // its answer is what flushes bufferedCreationMsgs here.
    locMgr->requestLocationAtHome(idx);
  }
  bufferedCreationMsgs[idx].push_back(msg);
}

// TODO: Need to make sure we use msg source PE for createhere cases where the message
// has moved around, but ended up on an unknown PE because the element was deleted or
// never created at some point?
void CkArray::requestDemandCreation(const CkArrayIndex& idx, int ctor, int pe)
{
  // Only the home PE can fulfill requests for demand creation
  CkAssert(locMgr->homePe(idx) == CkMyPe());

  CmiUInt8 id;
  if (!locMgr->lookupID(idx, id) || locMgr->whichPe(id) == -1)
  {
    // We (the home PE) do not know the elements location, therefore it (and its siblings)
    // do not exist. So we can approve the demand creation request -- once: until the
    // element registers here, further requests would create it again (a creation on
    // another PE has not reported yet, or one here is deferred for an element id).
    // The requester asked for the location as well and is told when it exists.
    if (!pendingDemandCreations.insert(idx).second) return;
    if (pe == CkMyPe())
    {
      // Directly create the element
      demandCreateElement(idx, ctor);
    }
    else
    {
      // Trigger creation at the request site
      thisProxy[pe].demandCreateElement(idx, ctor);
    }
  }
  else
  {
    // The object (or one of its siblings already exists, tell the requester.
    if (pe != CkMyPe())
    {
      // TODO: This API needs work
      // TODO: The requester and the pe to create on may be different
      locMgr->requestLocation(idx, pe);
    }
  }
}

void CkArray::sendBufferedMsgs(CmiUInt8 id, int pe)
{
  for (CkArrayMessage* msg : bufferedIDMsgs[id])
  {
    CkAssert(msg->array_element_id() == id);
    sendToPe(msg, pe, CkDeliver_queue);
  }
  bufferedIDMsgs.erase(id);

  CkAssert(bufferedIDMsgs.find(id) == bufferedIDMsgs.end());
}

void CkArray::releaseRetiredMsgs(CmiUInt8 oldId, CmiUInt8 newId, int pe)
{
  auto it = bufferedIDMsgs.find(oldId);
  if (it == bufferedIDMsgs.end()) return;
  for (CkArrayMessage* msg : it->second)
  {
    CkAssert(msg->array_element_id() == oldId);
    UsrToEnv(msg)->setRecipientID(makeID(newId));
    if (pe == -1)
      bufferedIDMsgs[newId].push_back(msg);
    else
      sendToPe(msg, pe, CkDeliver_queue);
  }
  bufferedIDMsgs.erase(oldId);
}

void CkArray::sendBufferedMsgs(const CkArrayIndex& idx, CmiUInt8 id, int pe)
{
  pendingDemandCreations.erase(idx);  // the element exists and has registered
  // TODO: This shouldn't be needed
  sendBufferedMsgs(id, pe);
  for (CkArrayMessage* msg : bufferedIndexMsgs[idx])
  {
    UsrToEnv(msg)->setRecipientID(makeID(id));
    // TODO: Is deliver_queue right?
    sendToPe(msg, pe, CkDeliver_queue);
  }
  bufferedIndexMsgs.erase(idx);

  for (CkArrayMessage* msg : bufferedCreationMsgs[idx])
  {
    UsrToEnv(msg)->setRecipientID(makeID(id));
    // TODO: Is deliver_queue right?
    sendToPe(msg, pe, CkDeliver_queue);
  }
  bufferedCreationMsgs.erase(idx);

  CkAssert(bufferedIDMsgs.find(id) == bufferedIDMsgs.end());
  CkAssert(bufferedIndexMsgs.find(idx) == bufferedIndexMsgs.end());
  CkAssert(bufferedCreationMsgs.find(idx) == bufferedCreationMsgs.end());
}

#include "CkArray.def.h"
