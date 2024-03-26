/*
Location manager: keeps track of an indexed set of migratable
objects.  Used by the array manager to locate array elements,
interact with the load balancer, and perform migrations.

Does not handle reductions (see ckreduction.h), broadcasts,
array proxies, or the details of element creation (see ckarray.h).
*/
#ifndef __CKLOCATION_H
#define __CKLOCATION_H

#include <unordered_map>
struct IndexHasher
{
public:
  size_t operator()(const CkArrayIndex& idx) const
  {
    return std::hash<unsigned int>()(idx.hash());
  }
};

struct ArrayIDHasher
{
public:
  size_t operator()(const CkArrayID& aid) const
  {
    return std::hash<int>()(((CkGroupID)aid).idx);
  }
};

/*********************** Array Messages ************************/
class CkArrayMessage : public CkMessage
{
public:
  // These routines are implementation utilities
  CmiUInt8 array_element_id(void);
  unsigned short& array_ep(void);
  unsigned short& array_ep_bcast(void);
  unsigned char& array_hops(void);
  unsigned int array_getSrcPe(void);
  unsigned int array_ifNotThere(void);
  void array_setIfNotThere(unsigned int);

  // This allows us to delete bare CkArrayMessages
  void operator delete(void* p) { CkFreeMsg(p); }
};

/* Utility */
//#if CMK_LBDB_ON
#include "LBManager.h"
#include "MetaBalancer.h"
//#endif

// Forward declarations
class CkArray;
class ArrayElement;

/// How to do a message delivery:
typedef enum : uint8_t
{
  CkDeliver_queue = 0,  // Deliver via the scheduler's queue
  CkDeliver_inline = 1  // Deliver via a regular call
} CkDeliver_t;
PUPbytes(CkDeliver_t)

    class CkArrayOptions;

// This is the entry in the location table, which stores the PE and epoch number of the
// latest location update.
// TODO: If we can template CkLocCache on this type, we could store more useful
// information for specific entities being managed. ie: for arrays it could store
// indices as well. Then one lookup would get you everything you need, rather than looking
// up in a bunch of tables.
// TODO: Is int the right type for pe and epoch?
struct CkLocEntry {
  CmiUInt8 id = 0;
  int pe = -1;
  int epoch = -1;

  const static CkLocEntry nullEntry;
};
PUPbytes(CkLocEntry);

#include "CkLocation.decl.h"

/************************** Array Messages ****************************/
/**
 *  This is the message type used to actually send a migrating array element.
 */

class CkArrayElementMigrateMessage : public CMessage_CkArrayElementMigrateMessage
{
public:
  CkArrayElementMigrateMessage(CkArrayIndex idx_, CmiUInt8 id_, bool ignoreArrival_,
                               int length_, int nManagers_, int epoch_)
      : idx(idx_),
        id(id_),
        ignoreArrival(ignoreArrival_),
        length(length_),
        nManagers(nManagers_),
        epoch(epoch_)
  {
  }

  CkArrayIndex idx;    // Array index that is migrating
  CmiUInt8 id;         // ID of the elements with this index in this collection
  bool ignoreArrival;  // if to inform LB of arrival
  int length;          // Size in bytes of the packed data
  int nManagers;       // Number of associated array managers
  int epoch;
  char* packData;
};

/******************* Map object ******************/

extern CkGroupID _defaultArrayMapID;
extern CkGroupID _fastArrayMapID;
class CkLocMgr;
class CkArray;

/**
\addtogroup CkArray
*/
/*@{*/

#include "ckarrayoptions.h"

/** The "map" is used by the array manager to map an array index to
 * a home processor number.
 */
class CkArrayMap : public IrrGroup  // : public CkGroupReadyCallback
{
public:
  CkArrayMap(void);
  CkArrayMap(CkMigrateMessage* m) : IrrGroup(m) {}
  virtual ~CkArrayMap();
  virtual int registerArray(const CkArrayIndex& numElements, CkArrayID aid);
  virtual void unregisterArray(int idx);
  virtual void storeCkArrayOpts(CkArrayOptions options);
  virtual void populateInitial(int arrayHdl, CkArrayOptions& options, void* ctorMsg,
                               CkArray* mgr);
  virtual int procNum(int arrayHdl, const CkArrayIndex& element) = 0;
  virtual int homePe(int arrayHdl, const CkArrayIndex& element)
  {
    return procNum(arrayHdl, element);
  }

  virtual void pup(PUP::er& p);

  CkArrayOptions storeOpts;
  std::unordered_map<int, bool> dynamicIns;
};

#if CMK_CHARM4PY

extern int (*ArrayMapProcNumExtCallback)(int, int, const int*);

class ArrayMapExt : public CkArrayMap
{
public:
  ArrayMapExt(void* impl_msg);

  static void __ArrayMapExt(void* impl_msg, void* impl_obj_void)
  {
    new (impl_obj_void) ArrayMapExt(impl_msg);
  }

  static void __entryMethod(void* impl_msg, void* impl_obj_void)
  {
    // fprintf(stderr, "ArrayMapExt:: entry method invoked\n");
    ArrayMapExt* obj = static_cast<ArrayMapExt*>(impl_obj_void);
    char* impl_buf = ck::get_message_buffer((ck::marshall_msg)impl_msg);
    PUP::fromMem implP(impl_buf);
    int msgSize;
    implP | msgSize;
    int ep;
    implP | ep;
    int dcopy_start;
    implP | dcopy_start;
    GroupMsgRecvExtCallback(obj->thisgroup.idx, ep, msgSize, impl_buf + (3 * sizeof(int)),
                            dcopy_start);
  }

  int procNum(int arrayHdl, const CkArrayIndex& element)
  {
    return ArrayMapProcNumExtCallback(thisgroup.idx, element.getDimension(),
                                      element.data());
    // fprintf(stderr, "[%d] ArrayMapExt - procNum is %d\n", CkMyPe(), pe);
  }
};

#endif

/*@}*/

/**
\addtogroup CkArrayImpl
\brief Migratable Chare Arrays: Implementation classes.
*/
/*@{*/
static inline CkGroupID CkCreatePropMap(void) { return CProxy_PropMap::ckNew(); }

extern void _propMapInit(void);
extern void _CkMigratable_initInfoInit(void);

#include "cklocrec.h"

/*********************** CkMigratable ******************************/
/** This is the superclass of all migratable parallel objects.
 *  Currently, that's just array elements.
 */
#if CMK_OUT_OF_CORE
#  include "conv-ooc.h"
extern CooPrefetchManager CkArrayElementPrefetcher;
// If this flag is set, this creation/deletion is just
//   a "fake" constructor/destructor call for prefetching.
CkpvExtern(int, CkSaveRestorePrefetch);
#endif

#include "ckmigratable.h"

/********************** CkLocMgr ********************/
/// A tiny class for detecting heap corruption
class CkMagicNumber_impl
{
protected:
  int magic;
  void badMagicNumber(int expected, const char* file, int line, void* obj) const;
  CkMagicNumber_impl(int m);
};
template <class T>
class CkMagicNumber : public CkMagicNumber_impl
{
  enum
  {
    good = sizeof(T) ^ 0x7EDC0000
  };

public:
  CkMagicNumber(void) : CkMagicNumber_impl(good) {}
  inline void check(const char* file, int line, void* obj) const
  {
    if (magic != good)
      badMagicNumber(good, file, line, obj);
  }
#if CMK_ERROR_CHECKING
#  define CK_MAGICNUMBER_CHECK magic.check(__FILE__, __LINE__, this);
#else
#  define CK_MAGICNUMBER_CHECK /*empty, for speed*/
#endif
};

/**
 * The "data" class passed to a CkLocIterator, which refers to a bound
 * glob of array elements.
 * This is a transient class-- do not attempt to store it or send
 * it across processors.
 */

class CkLocation
{
  CkLocMgr* mgr;
  CkLocRec* rec;

public:
  CkLocation(CkLocMgr* mgr_, CkLocRec* rec_);

  /// Find our location manager
  inline CkLocMgr* getManager(void) const { return mgr; }

  /// Find the local record that refers to this element
  inline CkLocRec* getLocalRecord(void) const { return rec; }

  /// Look up and return the array index of this location.
  const CkArrayIndex& getIndex(void) const;
  CmiUInt8 getID() const;

  void destroyAll();

  /// Pup all the array elements at this location.
  void pup(PUP::er& p);
};

/**
 * This interface describes the destination for an iterator over
 * the locations in an array.
 */
class CkLocIterator
{
public:
  virtual ~CkLocIterator();

  /// This location is part of the calling location manager.
  virtual void addLocation(CkLocation& loc) = 0;
};

enum CkElementCreation_t : uint8_t
{
  CkElementCreation_migrate = 2,  // Create object for normal migration arrival
  CkElementCreation_resume = 3,   // Create object after checkpoint
  CkElementCreation_restore = 4   // Create object after checkpoint, skip listeners
};

// Returns rank 0 for a pe for drone mode
#if CMK_DRONE_MODE
#  define CMK_RANK_0(pe) (CkNodeOf(pe) * CkNodeSize(0))
#else
#  define CMK_RANK_0(pe) pe
#endif

class CkMigratable_initInfo
{
public:
  CkLocRec* locRec;
  int chareType;
  bool forPrefetch; /* If true, this creation is only a prefetch restore-from-disk.*/
};
CkpvExtern(CkMigratable_initInfo, mig_initInfo);

class CkLocCache : public CBase_CkLocCache
{
private:
  // Map of ID to PE
  using LocationMap = std::unordered_map<CmiUInt8, CkLocEntry>;
  LocationMap locMap;

  using Listener = std::function<void(CmiUInt8, int)>;
  std::list<Listener> listeners;

public:
  CkLocCache() = default;
  CkLocCache(CkMigrateMessage* m) : CBase_CkLocCache(m) {}
  ~CkLocCache() = default;
  void pup(PUP::er& p);

  void requestLocation(CmiUInt8 id);

  // Entry methods for updating location tables across PEs
  void requestLocation(CmiUInt8 id, int peToTell);
  void updateLocation(const CkLocEntry& e);

  // Update the location table when an element migrates away
  void recordEmigration(CmiUInt8 id, int pe);

  // Query the local location table
  const CkLocEntry& getLocationEntry(CmiUInt8 id) const
  {
    LocationMap::const_iterator itr = locMap.find(id);
    return itr != locMap.end() ? itr->second : CkLocEntry::nullEntry;
  }
  int getPe(const CmiUInt8 id) const { return getLocationEntry(id).pe; }
  int getEpoch(const CmiUInt8 id) const { return getLocationEntry(id).epoch; }
  int homePe(const CmiUInt8 id) const { return ck::ObjID(id).getHomeID(); }

  // Insertion and removal
  void insert(CmiUInt8 id, int epoch = 0);
  void erase(CmiUInt8 id) { locMap.erase(id); }

  void addListener(Listener l) { listeners.push_back(l); }
  void notifyListeners(CmiUInt8 id, int pe) const
  {
    for (const Listener& l : listeners)
    {
      l(id, pe);
    }
  }
};

/**
 * A group which manages the location of an indexed set of
 * migratable objects.  Knows about insertions, deletions,
 * home processors, migration, and message forwarding.
 */
class CkLocMgr : public IrrGroup
{
private:
  CkMagicNumber<CkMigratable> magic;  // To detect heap corruption

  friend class CkLocation;  // so it can call pupElementsFor
  friend class ArrayElement;
  friend class MemElementPacker;

  using ArrayIdMap = std::unordered_map<CkArrayID, CkArray*, ArrayIDHasher>;
  using MsgBuffer = std::unordered_map<CmiUInt8, std::vector<CkArrayMessage*> >;
  using LocationRequestBuffer =
      std::unordered_map<CkArrayIndex, std::vector<int>, IndexHasher>;
  using IdxIdMap = std::unordered_map<CkArrayIndex, CmiUInt8, IndexHasher>;
  using LocRecHash = std::unordered_map<CmiUInt8, CkLocRec*>;
  using ElemMap = std::unordered_map<CmiUInt8, CkMigratable*>;

  using LocationListener = std::function<void(CmiUInt8, int)>;
  using IndexListener = std::function<void(const CkArrayIndex&, CmiUInt8, int)>;

  // Map of an index to a location record, which only exists for local elements
  LocRecHash hash;

  // Mapping data for initial/default object placement
  CkGroupID mapID;
  int mapHandle;
  CkArrayMap* map;

  // Information for the underlying location cache mapping ID to PE
  CkGroupID cacheID;
  CkLocCache* cache;

  // Map array of CkArrayID to CkArray* for all arrays managed by this CkLocMgr
  ArrayIdMap managers;

  // Listeners which need to be updated when there is new information about either index
  // to location binding, or index to ID binding.
  std::list<IndexListener> listeners;

  // Requests for locations of an index which did not have an ID at the time of the
  // request, so the request had to be buffered.
  LocationRequestBuffer bufferedLocationRequests;

  // Immigration messages which are waiting for all array managers to be ready
  std::list<CkArrayElementMigrateMessage*> pendingImmigrate;

  // The mapping of index to ID is either done via compression or an explicit map,
  // depending on if the bounds of this array are compressible into a 64bit ID.
  CkArrayIndex bounds;
  ck::ArrayIndexCompressor* compressor;
  IdxIdMap idx2id;    // Explicit map for non-compressible case
  CmiUInt8 idCounter; // Counter for creating new IDs in the non-compressible case

  /// This flag is set while we delete an old copy of a migrator
  bool duringMigration;
  /// This flag is set while we are deleting location manager
  bool duringDestruction;

  // Internal interface:
  void pupElementsFor(PUP::er& p, CkLocRec* rec, CkElementCreation_t type,
                      bool rebuild = false);

  bool checkInBounds(const CkArrayIndex& idx) const;

  // Get a new ID based on the ID generation scheme
  CmiUInt8 getNewObjectID(const CkArrayIndex& idx);

  // Insert the ID into the idx2id table
  inline void insertID(const CkArrayIndex& idx, const CmiUInt8 id)
  {
    if (compressor)
      return;
    idx2id[idx] = id;
  }

  // Insert elements after migration
  // TODO: This probably needs some work and can be cleaned up to more closely match
  // the normal creation/insertion path
  bool addElementToRec(CkLocRec* rec, CkArray* m, CkMigratable* elt, int ctorIdx,
                       void* ctorMsg);
  // Add given location record to the table, replacing the existing record
  void insertRec(CkLocRec* rec, const CmiUInt8& id);
  // Remove this entry from the table (does not delete record)
  void removeFromTable(const CmiUInt8 id);
  // Look up location record in the table, return NULL if not there.
  CkLocRec* elementNrec(const CmiUInt8 id) const;

  // Call this member function on each element of this location:
  typedef void (CkMigratable::*CkMigratable_voidfn_t)(void);
  typedef void (CkMigratable::*CkMigratable_voidfn_arg_t)(void*);
  void callMethod(CkLocRec* rec, CkMigratable_voidfn_arg_t fn, void*);
  void callMethod(CkLocRec* rec, CkMigratable_voidfn_t fn);

#if CMK_LBDB_ON
  CkGroupID lbmgrID;
  CkGroupID metalbID;
  CkSyncBarrier* syncBarrier;
  LBManager* lbmgr;
  MetaBalancer* the_metalb;
  LDBarrierReceiver lbBarrierBeginReceiver;
  LDBarrierReceiver lbBarrierEndReceiver;
  static void staticDummyResumeFromSync(void* data);
  void dummyResumeFromSync(void);
  static void staticRecvAtSync(void* data);
  void recvAtSync(void);
  LDOMHandle myLBHandle;
  void initLB(CkGroupID lbmgrID, CkGroupID metalbID);
#endif

  CProxy_CkLocMgr thisProxy;
  CProxyElement_CkLocMgr thislocalproxy;

public:
  MsgBuffer bufferedActiveRgetMsgs;

  CkLocMgr(CkArrayOptions opts);
  CkLocMgr(CkMigrateMessage* m);
  ~CkLocMgr();

  // TODO: This is currently only needed for checkpointing, but should be removed
  bool isLocMgr(void) { return true; }

  CkGroupID getLocationCache() const { return cacheID; }

  CkLocRec* registerNewElement(const CkArrayIndex& idx);

  // Interface used by external users:
  /// Home mapping
  int homePe(const CkArrayIndex& idx) const
  {
    return CMK_RANK_0(map->homePe(mapHandle, idx));
  }
  int homePe(const CmiUInt8 id) const
  {
    return CMK_RANK_0(id >> CMK_OBJID_ELEMENT_BITS);
  }
  int procNum(const CkArrayIndex& idx) const
  {
    return CMK_RANK_0(map->procNum(mapHandle, idx));
  }
  bool isHome(const CkArrayIndex& idx) const { return homePe(idx) == CkMyPe(); }
  int whichPe(const CmiUInt8 id) const { return cache->getPe(id); }
  int whichPe(const CkArrayIndex& idx) const
  {
    CmiUInt8 id;
    if (!lookupID(idx, id))
      return -1;
    return cache->getPe(id);
  }

  CmiUInt8 lookupID(const CkArrayIndex& idx) const
  {
    CkAssert(checkInBounds(idx));
    if (compressor)
    {
      const CmiUInt8 home = homePe(idx);
      CmiAssertMsg(
          home <= (ck::ObjID::masks::HOME_MASK >> ck::ObjID::bits::ELEMENT_BITS),
          "home is too big! (home: %" PRIx64 ", max: %" PRIx64 ")", home,
          (CmiUInt8)(ck::ObjID::masks::HOME_MASK >> ck::ObjID::bits::ELEMENT_BITS));
      const CmiUInt8 id = (home << CMK_OBJID_ELEMENT_BITS) + compressor->compress(idx);
      CmiAssertMsg(
          id <= (ck::ObjID::masks::HOME_MASK | ck::ObjID::masks::ELEMENT_MASK),
          "id is too big! (id: %" PRIx64 ", max: %" PRIx64 ")", id,
          (CmiUInt8)(ck::ObjID::masks::HOME_MASK | ck::ObjID::masks::ELEMENT_MASK));
      return id;
    }
    else
    {
      IdxIdMap::const_iterator itr = idx2id.find(idx);
      CkAssert(itr != idx2id.end());
      return itr->second;
    }
  }

  // TODO: This should be better
  bool lookupID(const CkArrayIndex& idx, CmiUInt8& id) const
  {
    CkAssert(checkInBounds(idx));
    if (compressor)
    {
      const CmiUInt8 home = homePe(idx);
      CmiAssertMsg(
          home <= (ck::ObjID::masks::HOME_MASK >> ck::ObjID::bits::ELEMENT_BITS),
          "home is too big! (home: %" PRIx64 ", max: %" PRIx64 ")", home,
          (CmiUInt8)(ck::ObjID::masks::HOME_MASK >> ck::ObjID::bits::ELEMENT_BITS));
      id = (home << CMK_OBJID_ELEMENT_BITS) + compressor->compress(idx);
      CmiAssertMsg(
          id <= (ck::ObjID::masks::HOME_MASK | ck::ObjID::masks::ELEMENT_MASK),
          "id is too big! (id: %" PRIx64 ", max: %" PRIx64 ")", id,
          (CmiUInt8)(ck::ObjID::masks::HOME_MASK | ck::ObjID::masks::ELEMENT_MASK));
      return true;
    }
    else
    {
      IdxIdMap::const_iterator itr = idx2id.find(idx);
      if (itr == idx2id.end())
      {
        return false;
      }
      else
      {
        id = itr->second;
        return true;
      }
    }
  }

  CkArrayIndex lookupIdx(const CmiUInt8& id) const
  {
    CkLocRec* rec = nullptr;
    if (compressor)
    {
      return compressor->decompress(id);
    }
    else if ((rec = elementNrec(id)))
    {
      return rec->getIndex();
    }
    else
    {
      IdxIdMap::const_iterator itr;
      for (itr = idx2id.begin(); itr != idx2id.end(); itr++)
      {
        if (itr->second == id)
          break;
      }
      CkAssert(itr != idx2id.end());
      return itr->first;
    }
  }

  int getMapHandle() const { return mapHandle; }
  CkGroupID getMap() const { return mapID; }

  void addLocationListener(LocationListener l) { cache->addListener(l); }
  void addIndexListener(IndexListener l) { listeners.push_back(l); }
  void notifyListeners(const CkArrayIndex& idx, CmiUInt8 id, int pe) const
  {
    for (const IndexListener& l : listeners)
    {
      l(idx, id, pe);
    }
  }

  /// Return true if this array element lives on another processor
  bool isRemote(const CkArrayIndex& idx, int* onPe) const;
  bool isRemote(const CmiUInt8 id) const {
    return elementNrec(id) == nullptr;
  }

  void setDuringDestruction(bool _duringDestruction);

  /// Pass each of our locations (each separate array index) to this destination.
  void iterate(CkLocIterator& dest);

  /// Insert and unpack this array element from this checkpoint (e.g., from
  /// CkLocation::pup), skip listeners
  void restore(const CkArrayIndex& idx, CmiUInt8 id, PUP::er& p);
  /// Insert and unpack this array element from this checkpoint (e.g., from
  /// CkLocation::pup)
  void resume(const CkArrayIndex& idx, CmiUInt8 id, PUP::er& p, bool notify = true,
              bool = false);

  // Interface used by array manager and proxies
  /// Add a new local array manager to our list.
  void addManager(CkArrayID aid, CkArray* mgr);
  void deleteManager(CkArrayID aid, CkArray* mgr);

  // Deliver buffered msgs that were buffered because of active rdma gets
  void deliverAnyBufferedRdmaMsgs(CmiUInt8);

  // Take all those actions that were waiting for the rgets launched from pup_buffer to
  // complete These actions include: calling ckJustMigrated, calling ResumeFromSync and
  // delivering any buffered messages that were sent for the element (which was still
  // carrying out rgets)
  void processAfterActiveRgetsCompleted(CmiUInt8 id);

  // Create a new local record at this array index.
  CkLocRec* createLocal(const CkArrayIndex& idx, bool forMigration, bool ignoreArrival,
                        bool notifyHome, int epoch = 0);

  /// Done inserting elements for now
  void doneInserting(void);
  void startInserting(void);

  // How many elements of each associated array are local to this PE?
  // If this returns n, and there are k associated arrays, that
  // means k*n elements are living here
  unsigned int numLocalElements();

  // Map stores the CkMigratable elements that have active Rgets
  // ResumeFromSync is not called for these elements until the Rgets have completed
  ElemMap toBeResumeFromSynced;

  // Advisories:
  /// This index now lives on the given processor-- update local records
  void informHome(const CkArrayIndex& idx, int nowOnPe);

  /// This message took several hops to reach us-- fix it
  void multiHop(CkArrayMessage* m);

  // Interface used by CkLocRec
  // Migrate us to another processor
  void emigrate(CkLocRec* rec, int toPe);
  void informLBPeriod(CkLocRec* rec, int lb_ideal_period);
  void metaLBCallLB(CkLocRec* rec);

#if CMK_LBDB_ON
  LBManager* getLBMgr(void) const { return lbmgr; }
  MetaBalancer* getMetaBalancer(void) const { return the_metalb; }
  const LDOMHandle& getOMHandle(void) const { return myLBHandle; }
#endif

  // This index will no longer be used-- delete the associated elements
  void reclaim(CkLocRec* rec);

  // Communication:
  void immigrate(CkArrayElementMigrateMessage* msg);
  void requestLocation(CmiUInt8 id);
  void requestLocation(const CkArrayIndex& idx);
  bool requestLocation(const CkArrayIndex& idx, int peToTell);
  void updateLocation(const CkArrayIndex& idx, const CkLocEntry& e);
  void reclaimRemote(const CkArrayIndex& idx, int deletedOnPe);
  void dummyAtSync(void);

  /// return a list of migratables in this local record
  void migratableList(CkLocRec* rec, std::vector<CkMigratable*>& list);

  void flushAllRecs(void);
  void flushLocalRecs(void);
  void pup(PUP::er& p);
};

/// check the command line arguments to determine if we can use ConfigurableRRMap
bool haveConfigurableRRMap();

/*@}*/

#endif /*def(thisHeader)*/
