/* Generalized Chare Arrays

These classes implement Chare Arrays.
These are dynamic (i.e. allowing insertion
and deletion) collections of ordinary Chares
indexed by arbitrary runs of bytes.

The general structure is:

CkArray is the "array manager" Group, or BOC--
it creates, keeps track of, and cares for all the
array elements on this PE (i.e.. "local" elements).
It does so using a hashtable of CkArrayRec objects--
there's an entry for each local, home-here, and recently
communicated remote array elements.

CkArrayElement is the type of the array
elements (a subclass of Chare).

CkArrayIndex is an arbitrary run of bytes,
used to index into the CkArray hashtable.

Converted from 1-D arrays 2/27/2000 by
Orion Sky Lawlor, olawlor@acm.org
*/
#ifndef __CKARRAY_H
#define __CKARRAY_H

#include "ckarrayindex.h"
#include "cklocation.h"
#include "ckmemcheckpoint.h"  // for CkArrayCheckPTReqMessage
#include "ckmulticast.h"

/***********************************************************
        Utility defines, includes, etc.
*/
extern void _registerCkArray(void);

/** This flag is true when in the system there is anytime migration, false when
 *  the user code guarantees that no migration happens except during load balancing
 *  (in which case it can only happen between AtSync and ResumeFromSync). */
extern bool _isAnytimeMigration;

/** This flag is true when users are sure there is at least one charm array element
 *  per processor. In such case, when doing reduction on the array, the children
 *  don't need to be notified that reduction starts
 */
extern bool _isNotifyChildInRed;

/**
\addtogroup CkArray
\brief Migratable Chare Arrays: user-visible classes.

All these classes are defined in ckarray.C.
*/
/*@{*/
/*@}*/

// This simple arrayListener just prints each event to stdout:
class CkVerboseListener : public CkArrayListener
{
public:
  CkVerboseListener(void);
  CkVerboseListener(CkMigrateMessage* m) : CkArrayListener(m) {}
  PUPable_decl(CkVerboseListener);

  virtual void ckRegister(CkArray* arrMgr, int dataOffset_);
  virtual void ckBeginInserting(void);
  virtual void ckEndInserting(void);

  virtual void ckElementStamp(int* eltInfo);
  virtual void ckElementCreating(ArrayElement* elt);
  virtual bool ckElementCreated(ArrayElement* elt);
  virtual void ckElementDied(ArrayElement* elt);

  virtual void ckElementLeaving(ArrayElement* elt);
  virtual bool ckElementArriving(ArrayElement* elt);
};

/**
\addtogroup CkArray
*/
/*@{*/

/*********************** Proxy Support ************************/
// Needed by CBase_ArrayElement
class ArrayBase
{
};
/*forward*/ class ArrayElement;

/**
 * This class is a wrapper around a CkArrayIndex and ArrayID,
 *  used by array element proxies.  This makes the translator's
 *  job simpler, and the translated code smaller.
 */
class CProxy_ArrayBase : public CProxy
{
private:
  CkArrayID _aid;

public:
  CProxy_ArrayBase()
  {
#if CMK_ERROR_CHECKING
    _aid.setZero();
#endif
  }
  CProxy_ArrayBase(const CkArrayID& aid, CK_DELCTOR_PARAM)
      : CProxy(CK_DELCTOR_ARGS), _aid(aid)
  {
  }
  CProxy_ArrayBase(const CkArrayID& aid) : CProxy(), _aid(aid) {}
  CProxy_ArrayBase(const ArrayElement* e);
  CProxy_ArrayBase(const CProxy_ArrayBase& cs) : CProxy(cs), _aid(cs.ckGetArrayID()) {}

  bool operator==(const CProxy_ArrayBase& other)
  {
    return ckGetArrayID() == other.ckGetArrayID();
  }

#if CMK_ERROR_CHECKING
  inline void ckCheck(void) const
  {  // Make sure this proxy has a value
    if (_aid.isZero())
      CkAbort("Error! This array proxy has not been initialized!");
  }
#else
  inline void ckCheck(void) const {}
#endif

  static CkArrayID ckCreateEmptyArray(CkArrayOptions opts);
  static void ckCreateEmptyArrayAsync(CkCallback cb, CkArrayOptions opts);
  static CkArrayID ckCreateArray(CkArrayMessage* m, int ctor, const CkArrayOptions& opts);

  void ckInsertIdx(CkArrayMessage* m, int ctor, int onPe, const CkArrayIndex& idx);
  void ckBroadcast(CkArrayMessage* m, int ep, int opts = 0) const;
  CkArrayID ckGetArrayID(void) const { return _aid; }
  CkArray* ckLocalBranch(void) const { return _aid.ckLocalBranch(); }
  CkArray* ckLocalBranchOther(int rank) const { return _aid.ckLocalBranchOther(rank); }
  CkLocMgr* ckLocMgr(void) const;
  inline operator CkArrayID() const { return ckGetArrayID(); }
  unsigned int numLocalElements() const { return ckLocMgr()->numLocalElements(); }

  void doneInserting(void);
  void beginInserting(void);

  CK_REDUCTION_CLIENT_DECL

  void pup(PUP::er& p);
};

class CProxyElement_ArrayBase : public CProxy_ArrayBase
{
private:
  CkArrayIndex _idx;  //<- our element's array index
public:
  CProxyElement_ArrayBase() {}
  CProxyElement_ArrayBase(const CkArrayID& aid, const CkArrayIndex& idx, CK_DELCTOR_PARAM)
      : CProxy_ArrayBase(aid, CK_DELCTOR_ARGS), _idx(idx)
  {
  }
  CProxyElement_ArrayBase(const CkArrayID& aid, const CkArrayIndex& idx)
      : CProxy_ArrayBase(aid), _idx(idx)
  {
  }
  CProxyElement_ArrayBase(const CProxyElement_ArrayBase& other)
      : CProxy_ArrayBase(other.ckGetArrayID()), _idx(other.ckGetIndex())
  {
  }
  CProxyElement_ArrayBase(const ArrayElement* e);

  CProxyElement_ArrayBase& operator=(const CProxyElement_ArrayBase& other)
  {
    new (this) CProxyElement_ArrayBase(other);
    return *this;
  }

  bool operator==(const CProxyElement_ArrayBase& other)
  {
    return ckGetArrayID() == other.ckGetArrayID() && ckGetIndex() == other.ckGetIndex();
  }

  void ckInsert(CkArrayMessage* m, int ctor, int onPe);
  void ckSend(CkArrayMessage* m, int ep, int opts = 0) const;
  //      static void ckSendWrapper(void *me, void *m, int ep, int opts = 0);
  static void ckSendWrapper(CkArrayID _aid, CkArrayIndex _idx, void* m, int ep, int opts);
  void* ckSendSync(CkArrayMessage* m, int ep) const;
  const CkArrayIndex& ckGetIndex() const { return _idx; }

  ArrayElement* ckLocal(void) const;
  void pup(PUP::er& p);
};

#define _AUTO_DELEGATE_MCASTMGR_ON_ 1

class CProxySection_ArrayBase : public CProxy_ArrayBase
{
private:
  std::vector<CkSectionID> _sid;

public:
  CProxySection_ArrayBase() = default;
  CProxySection_ArrayBase(const CkArrayID& aid, const CkArrayIndex* elems,
                          const int nElems, int factor = USE_DEFAULT_BRANCH_FACTOR);
  CProxySection_ArrayBase(const CkArrayID& aid, const std::vector<CkArrayIndex>& elems,
                          int factor = USE_DEFAULT_BRANCH_FACTOR);
  CProxySection_ArrayBase(const CkArrayID& aid, const CkArrayIndex* elems,
                          const int nElems, CK_DELCTOR_PARAM)
      : CProxy_ArrayBase(aid, CK_DELCTOR_ARGS)
  {
    _sid.emplace_back(aid, elems, nElems);
  }
  CProxySection_ArrayBase(const CkArrayID& aid, const std::vector<CkArrayIndex>& elems,
                          CK_DELCTOR_PARAM)
      : CProxy_ArrayBase(aid, CK_DELCTOR_ARGS)
  {
    _sid.emplace_back(aid, elems);
  }
  CProxySection_ArrayBase(const CkSectionID& sid)
      : CProxy_ArrayBase(sid._cookie.get_aid())
  {
    _sid.emplace_back(sid);
  }
  CProxySection_ArrayBase(const CkSectionID& sid, CK_DELCTOR_PARAM)
      : CProxy_ArrayBase(sid._cookie.get_aid(), CK_DELCTOR_ARGS)
  {
    _sid.emplace_back(sid);
  }
  CProxySection_ArrayBase(const CProxySection_ArrayBase& cs) : CProxy_ArrayBase(cs)
  {
    _sid.resize(cs._sid.size());
    for (size_t i = 0; i < _sid.size(); ++i)
    {
      _sid[i] = cs._sid[i];
    }
  }
  CProxySection_ArrayBase(const CProxySection_ArrayBase& cs, CK_DELCTOR_PARAM)
      : CProxy_ArrayBase(cs.ckGetArrayID(), CK_DELCTOR_ARGS)
  {
    _sid.resize(cs._sid.size());
    for (size_t i = 0; i < _sid.size(); ++i)
    {
      _sid[i] = cs._sid[i];
    }
  }
  CProxySection_ArrayBase(const int n, const CkArrayID* aid,
                          CkArrayIndex const* const* elems, const int* nElems,
                          int factor = USE_DEFAULT_BRANCH_FACTOR);
  CProxySection_ArrayBase(const std::vector<CkArrayID>& aid,
                          const std::vector<std::vector<CkArrayIndex> >& elems,
                          int factor = USE_DEFAULT_BRANCH_FACTOR);
  CProxySection_ArrayBase(const int n, const CkArrayID* aid,
                          CkArrayIndex const* const* elems, const int* nElems,
                          CK_DELCTOR_PARAM)
      : CProxy_ArrayBase(aid[0], CK_DELCTOR_ARGS)
  {
    _sid.resize(n);
    for (size_t i = 0; i < _sid.size(); ++i)
    {
      _sid[i] = CkSectionID(aid[i], elems[i], nElems[i]);
    }
  }
  CProxySection_ArrayBase(const std::vector<CkArrayID>& aid,
                          const std::vector<std::vector<CkArrayIndex> >& elems,
                          CK_DELCTOR_PARAM)
      : CProxy_ArrayBase(aid[0], CK_DELCTOR_ARGS)
  {
    _sid.resize(aid.size());
    for (size_t i = 0; i < _sid.size(); ++i)
    {
      _sid[i] = CkSectionID(aid[i], elems[i]);
    }
  }

  ~CProxySection_ArrayBase() = default;

  CProxySection_ArrayBase& operator=(const CProxySection_ArrayBase& cs)
  {
    CProxy_ArrayBase::operator=(cs);
    _sid.resize(cs._sid.size());
    for (size_t i = 0; i < _sid.size(); ++i)
    {
      _sid[i] = cs._sid[i];
    }
    return *this;
  }

  void ckAutoDelegate(int opts = 1);
  using CProxy_ArrayBase::setReductionClient;  // compilation error o/w
  void setReductionClient(CkCallback* cb);
  void resetSection();

  void ckSectionDelegate(CkDelegateMgr* d, int opts = 1)
  {
    ckDelegate(d);
    if (opts == 1)
      d->initDelegateMgr(this);
  }
  //	void ckInsert(CkArrayMessage *m,int ctor,int onPe);
  void ckSend(CkArrayMessage* m, int ep, int opts = 0);

  //	ArrayElement *ckLocal(void) const;
  inline int ckGetNumSubSections() const { return _sid.size(); }
  inline CkSectionInfo& ckGetSectionInfo() { return _sid[0]._cookie; }
  inline CkSectionID* ckGetSectionIDs() { return _sid.data(); }
  inline CkSectionID& ckGetSectionID() { return _sid[0]; }
  inline CkSectionID& ckGetSectionID(int i) { return _sid[i]; }
  inline CkArrayID ckGetArrayIDn(int i) const { return _sid[i]._cookie.get_aid(); }
  inline CkArrayIndex* ckGetArrayElements() const
  {
    return const_cast<CkArrayIndex*>(_sid[0]._elems.data());
  }
  inline CkArrayIndex* ckGetArrayElements(int i) const
  {
    return const_cast<CkArrayIndex*>(_sid[i]._elems.data());
  }
  inline int ckGetNumElements() const { return _sid[0]._elems.size(); }
  inline int ckGetNumElements(int i) const { return _sid[i]._elems.size(); }
  inline int ckGetBfactor() const { return _sid[0].bfactor; }
  void pup(PUP::er& p);
};

// Simple C-like API:
void CkSetMsgArrayIfNotThere(void* msg, CkArray_IfNotThere policy = CkArray_IfNotThere_buffer);
void CkSendMsgArray(int entryIndex, void* msg, CkArrayID aID, const CkArrayIndex& idx,
                    int opts = 0);
void CkSendMsgArrayInline(int entryIndex, void* msg, CkArrayID aID,
                          const CkArrayIndex& idx, int opts = 0);
// A send issued on a PE whose copy of the array manager does not exist yet
// (its creation broadcast is still in flight) is held and re-issued when
// CkCreateLocalGroup creates that manager. doc/internals/array-send-order.md
void _ckArrayFlushDeferredSends(CkGroupID mgr);

void CkBroadcastMsgArray(int entryIndex, void* msg, CkArrayID aID, int opts = 0);
void CkBroadcastMsgSection(int entryIndex, void* msg, CkSectionID sID, int opts = 0);
/************************ Array Element *********************/
/**
 *An array element is a chare that lives inside the array.
 *Unlike regular chares, array elements can migrate from one
 *Pe to another.  Each element has a unique index.
 */

class ArrayElement : public CkMigratable
{
  friend class CkArray;

  friend class CkArrayListener;
  int numInitialElements;  // Number of elements created by ckNew(numElements)
  void initBasics(void);
#ifdef _PIPELINED_ALLREDUCE_
  AllreduceMgr* allredMgr;  // for allreduce
#endif
public:
  ArrayElement(void);
  ArrayElement(CkMigrateMessage* m);
  virtual ~ArrayElement();

  /// Pack/unpack routine (called before and after migration)
  void pup(PUP::er& p);

  // Overridden functions:
  /// Called by the system just before and after migration to another processor:
  virtual void ckAboutToMigrate(void);
  virtual void ckJustMigrated(void);

  virtual void ckJustRestored(void);

  virtual void ckDestroy(void);
  virtual char* ckDebugChareName(void);
  virtual int ckDebugChareID(char*, int);

  void ckEmigrate(int toPe)
  {
    if(!_isAnytimeMigration)
    {
      CkPrintf("Charm++> WARNING: Attempted anytime migration is disabled.\n");
    }
    ckMigrate(toPe);
  }

#ifdef _PIPELINED_ALLREDUCE_
  void contribute2(CkArrayIndex myIndex, int dataSize, const void* data,
                   CkReduction::reducerType type, const CkCallback& cb,
                   CMK_REFNUM_TYPE userFlag = (CMK_REFNUM_TYPE)-1);
  void contribute2(int dataSize, const void* data, CkReduction::reducerType type,
                   CMK_REFNUM_TYPE userFlag = (CMK_REFNUM_TYPE)-1);
  void contribute2(int dataSize, const void* data, CkReduction::reducerType type,
                   const CkCallback& cb, CMK_REFNUM_TYPE userFlag = (CMK_REFNUM_TYPE)-1);
  void contribute2(CkReductionMsg* msg);
  void contribute2(const CkCallback& cb, CMK_REFNUM_TYPE userFlag = (CMK_REFNUM_TYPE)-1);
  void contribute2(CMK_REFNUM_TYPE userFlag = (CMK_REFNUM_TYPE)-1);
#else
  CK_REDUCTION_CONTRIBUTE_METHODS_DECL
#endif
  // for _PIPELINED_ALLREDUCE_, assembler entry method
  inline void defrag(CkReductionMsg* msg);
  inline const CkArrayID& ckGetArrayID(void) const { return thisArrayID; }
  inline ck::ObjID ckGetID(void) const;  // defined after CkArray below

  inline int ckGetArraySize(void) const { return numInitialElements; }

  int getRedNo(void) const;

protected:
  CkArray* thisArray;     // My source array
  CkArrayID thisArrayID;  // My source array's ID

  // More verbose form of abort
  CMK_NORETURN
#if defined __GNUC__ || defined __clang__
  __attribute__((format(printf, 2, 3)))
#endif
  virtual void
  CkAbort(const char* format, ...) const;

private:
  // Array implementation methods:
  int listenerData[CK_ARRAYLISTENER_MAXLEN];

#if CMK_MEM_CHECKPOINT
  friend class CkMemCheckPT;
  friend class CkLocMgr;

protected:
  int budPEs[2];

private:
  void init_checkpt();
#endif
public:
  void inmem_checkpoint(CkArrayCheckPTReqMessage* m);
  void recvBroadcast(CkMessage*);

#if CMK_GRID_QUEUE_AVAILABLE
public:
  int grid_queue_interval;
  int grid_queue_threshold;
  int msg_count;
  int msg_count_grid;
  int border_flag;
#endif
};
inline int* CkArrayListener::ckGetData(ArrayElement* el) const
{
  return &el->listenerData[dataOffset];
}

/**An ArrayElementT is a utility class where you are
 * constrained to a "thisIndex" of some fixed-sized type T.
 */
template <class T>
class ArrayElementT : public ArrayElement
{
public:
  using array_index_t = T;

  ArrayElementT(void) : thisIndex(*(const T*)thisIndexMax.data()) {}
#ifdef _PIPELINED_ALLREDUCE_
  void contribute(int dataSize, const void* data, CkReduction::reducerType type,
                  CMK_REFNUM_TYPE userFlag = (CMK_REFNUM_TYPE)-1)
  {
    contribute2(dataSize, data, type, userFlag);
  }
  void contribute(int dataSize, const void* data, CkReduction::reducerType type,
                  const CkCallback& cb, CMK_REFNUM_TYPE userFlag = (CMK_REFNUM_TYPE)-1)
  {
    contribute2((CkArrayIndex)(thisIndex), dataSize, data, type, cb, userFlag);
  }
  void contribute(CkReductionMsg* msg) { contribute2(msg); }
  void contribute(const CkCallback& cb, CMK_REFNUM_TYPE userFlag = (CMK_REFNUM_TYPE)-1)
  {
    contribute2(cb, userFlag);
  }
  void contribute(CMK_REFNUM_TYPE userFlag = (CMK_REFNUM_TYPE)-1)
  {
    contribute2(userFlag);
  }
#endif
  ArrayElementT(CkMigrateMessage* msg)
      : ArrayElement(msg), thisIndex(*(const T*)thisIndexMax.data())
  {
  }

  const T thisIndex;  /// Object array index
};

typedef ArrayElementT<CkIndex1D> ArrayElement1D;
typedef ArrayElementT<CkIndex2D> ArrayElement2D;
typedef ArrayElementT<CkIndex3D> ArrayElement3D;
typedef ArrayElementT<CkIndex4D> ArrayElement4D;
typedef ArrayElementT<CkIndex5D> ArrayElement5D;
typedef ArrayElementT<CkIndex6D> ArrayElement6D;
typedef ArrayElementT<CkIndexMax> ArrayElementMax;

#if CMK_CHARM4PY

extern void (*ArrayMsgRecvExtCallback)(int, int, int*, int, int, char*, int);
extern int (*ArrayElemLeaveExt)(int, int, int*, char**, int);
extern void (*ArrayElemJoinExt)(int, int, int*, int, char*, int);
extern void (*ArrayResumeFromSyncExtCallback)(int, int, int*);

class ArrayElemExt : public ArrayElement
{
private:
  int ctorEpIdx;

public:
  ArrayElemExt(void* impl_msg);
  ArrayElemExt(CkMigrateMessage* m) { delete m; }

  static void __ArrayElemExt(void* impl_msg, void* impl_obj_void)
  {
    new (impl_obj_void) ArrayElemExt(impl_msg);
  }

  static void __entryMethod(void* impl_msg, void* impl_obj_void)
  {
    // fprintf(stderr, "ArrayElemExt:: Entry method invoked\n");
    ArrayElemExt* e = static_cast<ArrayElemExt*>(impl_obj_void);
    CkMarshallMsg* impl_msg_typed = (CkMarshallMsg*)impl_msg;
    char* impl_buf = impl_msg_typed->msgBuf;
    PUP::fromMem implP(impl_buf);
    int msgSize;
    implP | msgSize;
    int ep;
    implP | ep;
    int dcopy_start;
    implP | dcopy_start;
    ArrayMsgRecvExtCallback(((CkGroupID)e->thisArrayID).idx,
                            int(e->thisIndexMax.getDimension()), e->thisIndexMax.data(),
                            ep, msgSize, impl_buf + (3 * sizeof(int)), dcopy_start);
  }

  static void __AtSyncEntryMethod(void* impl_msg, void* impl_obj_void)
  {
    ArrayElemExt* e = static_cast<ArrayElemExt*>(impl_obj_void);
    // printf("ArrayElementExt:: calling AtSync elem->usesAtSync=%d\n", e->usesAtSync);
    e->AtSync();
    if (UsrToEnv(impl_msg)->isVarSysMsg() == 0)
      CkFreeSysMsg(impl_msg);
    else
      CkFreeMsg(impl_msg);
  }

  static void __CkMigrateMessage(void* impl_msg, void* impl_obj_void)
  {
    // printf("ArrayElemExt:: Migration constructor invoked\n");
    call_migration_constructor<ArrayElemExt> c = impl_obj_void;
    c((CkMigrateMessage*)impl_msg);
  }

  // NOTE this assumes that calls to pup are due to array element migration
  // not sure if it's always going to be the case
  void pup(PUP::er& p)
  {
    // because this is not generated code, looks like I need to make explicit call
    // to parent pup method, otherwise fields in parent class like usesAtSync will
    // not be pupped!
    ArrayElement::pup(p);
    int nDims = thisIndexMax.getDimension();
    int aid = ((CkGroupID)thisArrayID).idx;
    int data_size;
    if (!p.isUnpacking())
    {  // packing or sizing
      char* msg;
      data_size = ArrayElemLeaveExt(aid, nDims, thisIndexMax.data(), &msg, p.isSizing());
      p | data_size;
      p | ctorEpIdx;
      p(msg, data_size);
    }
    else
    {
      p | data_size;
      p | ctorEpIdx;
      PUP::fromMem* p_mem = (PUP::fromMem*)&p;
      ArrayElemJoinExt(aid, nDims, thisIndexMax.data(), ctorEpIdx,
                       p_mem->get_current_pointer(), data_size);
      p_mem->advance(data_size);
    }
  }

  void ResumeFromSync()
  {
    if (!usesAtSync)
    {
      // not sure in which cases it is useful to receive resumeFromSync if
      // usesAtSync=false, but for now I'm disabling it because it is
      // unnecessary overhead. In non-lb scenarios with NullLB, every LBPeriod
      // (which is 0.5 s by default), the lb infrastructure calls atsync and
      // resumefromsync on every chare array element, even if usesAtSync=false.
      // that part of the lb infrastructure should be fixed first.
      return;
    }
    ArrayResumeFromSyncExtCallback(((CkGroupID)thisArrayID).idx,
                                   int(thisIndexMax.getDimension()), thisIndexMax.data());
  }
};

#endif

/*@}*/

/*********************** Array Manager BOC *******************/
/**
\addtogroup CkArrayImpl
*/
/*@{*/

#include "CkArray.decl.h"

void CkSendAsyncCreateArray(int ctor, CkCallback cb, CkArrayOptions opts, void* ctorMsg);

class CkArrayCreatedMsg : public CMessage_CkArrayCreatedMsg
{
public:
  CkArrayID aid;
  CkArrayCreatedMsg(CkArrayID _aid) : aid(_aid) {}
};

class CkArrayBroadcaster;
class CkArrayReducer;

void _ckArrayInit(void);

// Wrapper class to hold a message pointer
// Used in ZC Bcast when root node is non-zero
class MsgPointerWrapper
{
public:
  void* msg;
  int ep;
  int opts;
  unsigned int epoch;
  void pup(PUP::er& p)
  {
    pup_pointer(&p, &msg);
    p | ep;
    p | opts;
    p | epoch;
  }
};

class CkArray : public CkReductionMgr
{
  friend class ArrayElement;
  friend class CkArrayBroadcaster;
  friend class CProxy_ArrayBase;
  friend class CProxyElement_ArrayBase;
  friend class CkLocMgr;

  using IDMsgBuffer = std::unordered_map<CmiUInt8, std::vector<CkArrayMessage*> >;
  using IndexMsgBuffer
      = std::unordered_map<CkArrayIndex, std::vector<CkArrayMessage*>, IndexHasher>;
  IDMsgBuffer bufferedIDMsgs;
  IndexMsgBuffer bufferedIndexMsgs;
  // We need a separate buffer for demand creation messages because it also serves as an
  // indicator of whether a demand creation request has already been sent.
  IndexMsgBuffer bufferedCreationMsgs;
  // Home side: indices whose demand creation was approved but whose element has
  // not registered yet. A second request in that window is not approved again
  // (its sender's location request is answered once the element exists).
  std::unordered_set<CkArrayIndex, IndexHasher> pendingDemandCreations;

  CkMagicNumber<ArrayElement> magic;  // To detect heap corruption
  CkLocMgr* locMgr;
  CkGroupID locMgrID;
  // This array's collection number and id layout (objid.h), assigned on PE 0 by
  // CkCreateArray; the layout is the location manager's (a bound array shares it).
  CmiUInt8 collectionNumber;
  bool wideCollection;
  CkGroupID mCastMgrID;
  bool sectionAutoDelegate;
  CkCallback initCallback;
  CProxy_CkArray thisProxy;
  // Separate mapping and storing the element pointers to speed iteration in broadcast
  std::unordered_map<CmiUInt8, unsigned int> localElems;
  std::vector<CkMigratable*> localElemVec;

  UShort recvBroadcastEpIdx;

private:
  bool stableLocations;

public:
  // Array Creation:
  CkArray(CkArrayOptions&& c, CkMarshalledMessage&& initMsg);
  CkArray(CkMigrateMessage* m);
  ~CkArray();
  CkGroupID& getmCastMgr(void) { return mCastMgrID; }
  bool isSectionAutoDelegated(void) { return sectionAutoDelegate; }

  UShort& getRecvBroadcastEpIdx(void) { return recvBroadcastEpIdx; }

  // Access & information routines
  inline CkLocMgr* getLocMgr(void) { return locMgr; }
  inline CmiUInt8 getCollectionNumber(void) const { return collectionNumber; }
  inline bool isWideCollection(void) const { return wideCollection; }
  /// The id of this array's element with the given payload
  inline ck::ObjID makeID(CmiUInt8 eid) const { return ck::ObjID(collectionNumber, eid, wideCollection); }
  inline const CkArrayIndex& getNumInitial(void) const { return numInitial; }
  inline int homePe(const CkArrayIndex& idx) const { return locMgr->homePe(idx); }
  inline int procNum(const CkArrayIndex& idx) const { return locMgr->procNum(idx); }

  /// Return the last known processor for this array index.
  /// Valid for any possible array index.
  inline int lastKnown(const CkArrayIndex& idx) const
  {
    int pe = locMgr->whichPe(idx);
    return pe == -1 ? homePe(idx) : pe;
  }

  // Called by the runtime system to deliver an array message to this array
  void deliver(CkArrayMessage* m, CkDeliver_t type)
  {
    recvMsg(m, m->array_element_id(), type);
  }

  // Methods for sending and receiving messages for array elements
  // As a message moves through the system, it will either be sent, buffered, trigger
  // demand creation, or invoked based on current conditions.
  void sendMsg(CkArrayMessage* msg, const CkArrayIndex& idx, CkDeliver_t type,
               int opts = 0);
  // Receive a msg which just arrived and needs to be delivered or forwarded.
  void recvMsg(CkArrayMessage* msg, CmiUInt8 id, CkDeliver_t type, int opts = 0);

  void recordSend(const CmiUInt8 id, const unsigned int bytes, int pe, const int opts = 0);

private:
  // These three methods are directly called by sendMsg and recvMsg
  void sendToPe(CkArrayMessage* msg, int pe, CkDeliver_t type, int opts = 0);
  void deliverToElement(CkArrayMessage* msg, ArrayElement* elem);
  void handleUnknown(CkArrayMessage* msg, const CkArrayIndex& idx, CkDeliver_t type,
                     int opts = 0);
  // For an element this PE has never seen: routes purely by id (its home is
  // computable from the id), never needing the index.
  void handleUnknownByID(CkArrayMessage* msg, CmiUInt8 id, CkDeliver_t type,
                         int opts = 0);

  // If we don't want to send the message, we will buffer the messages and send either a
  // location request or a demand creation request.
  void bufferForLocation(CkArrayMessage* msg, const CkArrayIndex& idx);
  void bufferForCreation(CkArrayMessage* msg, const CkArrayIndex& idx);

  void sendBufferedMsgs(CmiUInt8, int pe);
  void sendBufferedMsgs(const CkArrayIndex& idx, CmiUInt8 id, int pe);
  /// Messages held for oldId, whose index is now bound to newId: re-address and
  /// send to pe, or, with pe == -1, hold under newId until its location arrives
  /// (CkLocMgr::releaseRetired, hashed kind).
  void releaseRetiredMsgs(CmiUInt8 oldId, CmiUInt8 newId, int pe);
  bool hasBufferedMsgs(CmiUInt8 id) const
  {
    return bufferedIDMsgs.find(id) != bufferedIDMsgs.end();
  }

public:
  // TODO: Make sure the demand creation pipeline still obeys message delivery type?
  void demandCreateForMsg(CkArrayMessage* msg, const CkArrayIndex& idx);
  void requestDemandCreation(const CkArrayIndex& idx, int ctor, int pe);
  void demandCreateElement(const CkArrayIndex& idx, int ctor);

  /// Fetch a local element via its ID (return NULL if not local)
  inline ArrayElement* lookup(const CmiUInt8 id)
  {
    return (ArrayElement*)getEltFromArrMgr(id);
  }
  /// Fetch a local element via its index (return NULL if not local)
  inline ArrayElement* lookup(const CkArrayIndex& idx)
  {
    CmiUInt8 id;
    if (locMgr->lookupID(idx, id))
    {
      return (ArrayElement*)getEltFromArrMgr(id);
    }
    else
    {
      return NULL;
    }
  }

 inline size_t getNumLocalElems() {
    return localElemVec.size();
  }

  inline unsigned int getEltLocalIndex(const CmiUInt8 id) {
    const auto itr = localElems.find(id);
    return ( itr == localElems.end() ? -1 : itr->second);
  }

  virtual CkMigratable* getEltFromArrMgr(const CmiUInt8 id)
  {
    const auto itr = localElems.find(id);
    return (itr == localElems.end() ? NULL : localElemVec[itr->second]);
  }
  virtual void putEltInArrMgr(const CmiUInt8 id, CkMigratable* elt)
  {
    localElems[id] = localElemVec.size();
    localElemVec.push_back(elt);
  }
  virtual void eraseEltFromArrMgr(const CmiUInt8 id)
  {
    auto itr = localElems.find(id);
    if (itr != localElems.end())
    {
      unsigned int offset = itr->second;
      localElems.erase(itr);
      // Do not delete the CkMigratable itself (unlike in deleteElt)

      if (offset != localElemVec.size() - 1)
      {
        CkMigratable* moved = localElemVec[localElemVec.size() - 1];
        localElemVec[offset] = moved;
        localElems[moved->ckGetID()] = offset;
      }

      localElemVec.pop_back();
    }
  }

  void deleteElt(const CmiUInt8 id)
  {
    auto itr = localElems.find(id);
    if (itr != localElems.end())
    {
      unsigned int offset = itr->second;
      localElems.erase(itr);
      delete localElemVec[offset];

      if (offset != localElemVec.size() - 1)
      {
        CkMigratable* moved = localElemVec[localElemVec.size() - 1];
        localElemVec[offset] = moved;
        localElems[moved->ckGetID()] = offset;
      }

      localElemVec.pop_back();
    }
  }

  // Creation:
  /// Create-after-migrate:
  /// Create an uninitialized element after migration
  ///  The element's constructor will be called immediately after.
  virtual CkMigratable* allocateMigrated(int elChareType, CkElementCreation_t type);
  void stampListenerData(CkMigratable* elt);

  /// Prepare creation message:
  void prepareCtorMsg(CkMessage* m, int listenerData[CK_ARRAYLISTENER_MAXLEN]);

  int findInitialHostPe(const CkArrayIndex& idx, int proposedPe);

  /// Create initial array elements:
  virtual void insertInitial(const CkArrayIndex& idx, void* ctorMsg);
  virtual void doneInserting(void);
  virtual void beginInserting(void);
  void remoteDoneInserting(void);
  void remoteBeginInserting(void);

  /// Create manually:
  bool insertElement(CkArrayMessage*, const CkArrayIndex& idx,
                     int listenerData[CK_ARRAYLISTENER_MAXLEN]);
  void insertElement(CkMarshalledMessage&&, const CkArrayIndex& idx,
                     int listenerData[CK_ARRAYLISTENER_MAXLEN]);

  /// Broadcast communication:
  void sendBroadcast(CkMessage* msg);
  void recvBroadcast(CkMessage* msg);
  void sendExpeditedBroadcast(CkMessage* msg);
  void recvExpeditedBroadcast(CkMessage* msg) { recvBroadcast(msg); }
  void recvBroadcastViaTree(CkMessage* msg);
  void recvNoKeepBroadcast(CkMessage* msg) { recvBroadcast(msg); }
  void sendNoKeepBroadcast(CkMessage* msg);
  void recvNoKeepExpeditedBroadcast(CkMessage* msg) { recvBroadcast(msg); }
  void sendNoKeepExpeditedBroadcast(CkMessage* msg);

  void sendZCBroadcast(MsgPointerWrapper w);

  /// Whole array destruction, including all elements and the group itself
  void ckDestroy();

  void pup(PUP::er& p);
  void ckJustMigrated(void) { doneInserting(); }

  virtual bool isArrMgr(void) { return true; }

private:
  CkArrayIndex numInitial;  /// Number of initial array elements
  bool isInserting;         /// Are we currently inserting elements?
  int numPesInited;

  /// Allocate space for a new array element
  ArrayElement* allocate(int elChareType, CkMessage* msg, bool fromMigration,
                         int* listenerData);

  // Spring cleaning
  void springCleaning(void);
  static void staticSpringCleaning(void* forWhom);
  void setupSpringCleaning();
  int springCleaningCcd;

  // ArrayListeners:
  CkPupAblePtrVec<CkArrayListener> listeners;
  int listenerDataOffset;

public:
  void addListener(CkArrayListener* l)
  {
    l->ckRegister(this, listenerDataOffset);
    listenerDataOffset += l->ckGetLen();
    listeners.push_back(l);
    if (listenerDataOffset > CK_ARRAYLISTENER_MAXLEN)
      CkAbort(
          "Too much array listener data!\n"
          "You'll have to either use fewer array listeners, or increase the "
          "compile-time\n"
          "constant CK_ARRAYLISTENER_MAXLEN!\n");
  }

  void incrementBcastNoAndSendBack(int srcPe, MsgPointerWrapper w);

private:
  CkArrayReducer* reducer;          // Read-only copy of default reducer
  CkArrayBroadcaster* broadcaster;  // Read-only copy of default broadcaster
public:
  CkArrayBroadcaster* getBroadcaster() { return broadcaster; }
  void flushStates();
  void forwardZCMsgToOtherElems(envelope *env);
  void forwardZCMsgToSpecificElem(envelope *env, CkMigratable *elem);

  static bool isIrreducible() { return true; }
};

inline ck::ObjID ArrayElement::ckGetID(void) const { return thisArray->makeID(myRec->getID()); }

// Maintain old name of former parent class for backwards API compatibility
// with usage in maps' populateInitial()
typedef CkArray CkArrMgr;

/*@}*/

/// This arrayListener is in charge of delivering broadcasts to the array.
class CkArrayBroadcaster : public CkArrayListener
{
  inline int& getData(ArrayElement* el) { return *ckGetData(el); }

public:
  CkArrayBroadcaster(bool _stableLocations, bool _broadcastViaScheduler);
  CkArrayBroadcaster(CkMigrateMessage* m);
  virtual void pup(PUP::er& p);
  PUPable_decl(CkArrayBroadcaster);

  // When a new array element is inserted, it should only receive future broadcasts, so
  // set the object's epoch to 1 more than the max epoch we've seen so far
  virtual void ckElementStamp(int* eltInfo) { *eltInfo = storedBcasts.getMaxEpoch() + 1; }

  /// Element was just created on this processor
  /// Return false if the element migrated away or deleted itself.
  virtual bool ckElementCreated(ArrayElement* elt)
  {
    const bool stays = bringUpToDate(elt);
    releaseDelivered();
    return stays;
  }

  /// Element just arrived on this processor (so just called pup)
  /// Return false if the element migrated away or deleted itself.
  virtual bool ckElementArriving(ArrayElement* elt)
  {
    const bool stays = bringUpToDate(elt);
    releaseDelivered();
    return stays;
  }

  void setOwner(CkArray* array) { owner = array; }

  // Stable locations: frees the stored broadcasts every local element has
  // received. Does nothing for an array whose elements may move at any time.
  void releaseDelivered();

  // The message of this epoch is in use by a zero-copy transfer until the
  // ALL_DONE delivery, which frees it
  void holdBcast(const int epoch) { storedBcasts.hold(epoch); }
  // The stored original was handed to an element and freed on delivery
  void consumeBcast(const int epoch) { storedBcasts.consume(epoch); }
  // True if el expects this broadcast and every other local element has it,
  // so el may take the original
  bool isFinalRecipient(const std::vector<CkMigratable*>& elements, ArrayElement* el,
                        const int epoch);

  void ingestIncoming(CkArrayMessage* msg);

  bool attemptDelivery(CkArrayMessage* bcast, ArrayElement* el, bool doFree);
  bool performDelivery(CkArrayMessage* bcast, ArrayElement* el, bool doFree);
#if CMK_CHARM4PY
  void deliverAndUpdate(CkArrayMessage* bcast, std::vector<CkMigratable*>& elements,
                        int arrayId);
  bool performDelivery(CkArrayMessage* bcast, std::vector<CkMigratable*>& elements,
                       int arrayId);
#endif

  void springCleaning(void);

  void flushState();

private:
  // A circular queue designed to store incoming broadcasts. Notably,
  // it does not act like a FIFO, instead ordering and storing
  // broadcasts by their stamped epoch.
  class BcastQueue
  {
  private:
    // One slot per epoch from headEpoch up. A slot whose broadcast has arrived
    // is seen; its message is null again once an element took the original
    // (consumed). A held slot's message is still in use by a zero-copy
    // transfer and is not freed until the ALL_DONE delivery consumes it.
    struct Slot
    {
      CkArrayMessage* msg = nullptr;
      bool seen = false;
      bool held = false;
    };
    std::vector<Slot> storage{16};
    int headEpoch = 0;
    size_t headIndex = 0;
    size_t mask = 0x0F;
    int curMaxEpoch = -1;
    int oldMaxEpoch = -1;  // Max epoch at last spring cleaning

    int getOffset(const int epoch) const { return epoch - headEpoch; }
    bool inRange(const int epoch) const
    {
      const int offset = getOffset(epoch);
      return offset >= 0 && offset < storage.size();
    }
    Slot& slot(const int epoch) { return storage[(headIndex + getOffset(epoch)) & mask]; }
    const Slot& slot(const int epoch) const
    {
      return storage[(headIndex + getOffset(epoch)) & mask];
    }

  public:
    BcastQueue() = default;
    BcastQueue(CkMigrateMessage* m) {}
    ~BcastQueue()
    {
      for (Slot& s : storage)
        if (s.msg != nullptr)
          delete s.msg;
    }

    // Inserts msg into the slot corresponding to epoch
    void insert(CkArrayMessage* const msg, const int epoch)
    {
      const int offset = getOffset(epoch);
      if (offset < 0)
        CkAbort("Array broadcast %d arrived after the broadcast store released it (head %d)",
                epoch, headEpoch);
      if (offset >= storage.size())
      {
        // Size of storage is always a power of 2 to ease masking
        const size_t oldSize = storage.size();
        size_t newSize = oldSize * 2;
        while (offset >= newSize) newSize *= 2;
        storage.resize(newSize);
        mask = newSize - 1;
        if (headIndex != 0)
        {
          // Shuffle things around so that the queue starts at index 0
          std::move(storage.begin(), storage.begin() + headIndex,
                    storage.begin() + oldSize);
          std::move(storage.begin() + headIndex, storage.begin() + oldSize,
                    storage.begin());
          std::move(storage.begin() + oldSize, storage.begin() + oldSize + headIndex,
                    storage.begin() + oldSize - headIndex);
          std::fill(storage.begin() + oldSize, storage.begin() + oldSize + headIndex,
                    Slot());
          headIndex = 0;
        }
      }
      Slot& s = slot(epoch);
      CkAssert(s.msg == nullptr && !s.seen);
      s.msg = msg;
      s.seen = true;

      curMaxEpoch = std::max(epoch, curMaxEpoch);
    }

    bool hasBcast(const int epoch) const
    {
      return inRange(epoch) && slot(epoch).msg != nullptr;
    }

    CkArrayMessage* getBcast(const int epoch) const
    {
      CkAssert(hasBcast(epoch));
      return slot(epoch).msg;
    }

    int getMaxEpoch() const { return curMaxEpoch; }
    int getHeadEpoch() const { return headEpoch; }

    // The message is in use by a zero-copy transfer: keep it until consumed
    void hold(const int epoch)
    {
      CkAssert(hasBcast(epoch));
      slot(epoch).held = true;
    }

    // An element took the original message and it was freed on delivery
    void consume(const int epoch)
    {
      CkAssert(hasBcast(epoch));
      Slot& s = slot(epoch);
      s.msg = nullptr;
      s.held = false;
    }

    // Frees the stored broadcasts below the given epoch, stopping at the first
    // one not received yet (a broadcast still on its way may be older than a
    // stored one) or still held by a zero-copy transfer
    void releaseBelow(const int epoch)
    {
      while (headEpoch < epoch)
      {
        Slot& s = slot(headEpoch);  // offset 0: always within storage
        if (!s.seen || s.held)
          break;
        if (s.msg != nullptr)
          delete s.msg;
        s = Slot();
        headIndex = (headIndex + 1) & mask;
        headEpoch++;
      }
    }

    // Frees the broadcasts older than the newest one seen at the previous pass
    void springCleaning()
    {
      releaseBelow(oldMaxEpoch);
      oldMaxEpoch = std::max(oldMaxEpoch, curMaxEpoch);
    }

    void clear()
    {
      for (Slot& s : storage)
        if (s.msg != nullptr)
          delete s.msg;
      storage.clear();
      storage.resize(16);
      headIndex = 0;
      mask = 0x0F;
      curMaxEpoch = 0;
      oldMaxEpoch = 0;
    }

    void pup(PUP::er& p)
    {
      p | headEpoch;
      p | curMaxEpoch;
      if (p.isUnpacking())
      {
        // The messages are not saved, and the epochs below the newest one
        // seen do not arrive again after a restart: start the head past them
        headEpoch = std::max(headEpoch, curMaxEpoch + 1);
        oldMaxEpoch = curMaxEpoch;
      }
    }
  };

  int bcastSendEpoch = 0;
  bool stableLocations;
  bool broadcastViaScheduler;
  CkArray* owner = nullptr;  // the array whose local elements this serves
  // This queue stores old broadcasts (in case a migrant arrives
  // and needs to be brought up to date)
  BcastQueue storedBcasts;

  bool bringUpToDate(ArrayElement* el);

public:
  int incBcastSendEpoch() { return bcastSendEpoch++; }
};

#endif
