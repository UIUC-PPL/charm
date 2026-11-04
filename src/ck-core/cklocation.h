/*
Location manager: keeps track of an indexed set of migratable
objects.  Used by the array manager to locate array elements,
interact with the load balancer, and perform migrations.

Does not handle reductions (see ckreduction.h), broadcasts,
array proxies, or the details of element creation (see ckarray.h).
*/
#ifndef __CKLOCATION_H
#define __CKLOCATION_H

#include <deque>
#include <map>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <atomic>
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

/**
 *  Intra-process (same CmiNode / SMP mode) migration message. Instead of
 *  packing the chare, the source PE transfers ownership of the live
 *  CkMigratable objects to the destination PE via raw pointers.
 */
class CkArrayElementIntraProcessMigrateMessage
  : public CMessage_CkArrayElementIntraProcessMigrateMessage
{
public:
  CkArrayElementIntraProcessMigrateMessage(CkArrayIndex idx_, CmiUInt8 id_,
                                           bool ignoreArrival_, int nManagers_,
                                           int locEpoch_, int barrierEpoch_)
      : idx(idx_),
        id(id_),
        ignoreArrival(ignoreArrival_),
        nManagers(nManagers_),
        locEpoch(locEpoch_),
        barrierEpoch(barrierEpoch_)
  {
  }

  CkArrayIndex idx;
  CmiUInt8 id;
  bool ignoreArrival;
  int nManagers;
  int locEpoch;      // location cache epoch (for createLocal)
  int barrierEpoch;  // AtSync sync-barrier epoch (for ckFinishConstruction)
  // Move-ledger tag: PE whose LBManager tracks this move (-1 for a move no
  // balancer step is waiting on), and the step to ack it under. The
  // destination retires the entry once the element has registered.
  int lbLedgerSrcPe = -1;
  int lbLedgerStep = -1;
  CkGroupID* aids;
  uintptr_t* eltPtrs;
};

class CkArrayElementMigrateMessage : public CMessage_CkArrayElementMigrateMessage
{
public:
  CkArrayElementMigrateMessage(CkArrayIndex idx_, CmiUInt8 id_, bool ignoreArrival_,
                               int length_, int nManagers_, int epoch_, bool hasGPUMsg_ = false,
                               int nGpuBufs_ = 0)
      : idx(idx_),
        id(id_),
        ignoreArrival(ignoreArrival_),
        length(length_),
        nManagers(nManagers_),
        hasGPUMsg(hasGPUMsg_),
        epoch(epoch_),
        nGpuBufs(nGpuBufs_)
  {
  }

  CkArrayIndex idx;    // Array index that is migrating
  CmiUInt8 id;         // ID of the elements with this index in this collection
  bool ignoreArrival;  // if to inform LB of arrival
  bool hasGPUMsg;      // a device payload travels alongside, on its own device-zerocopy send
  int length;          // Size in bytes of the packed data
  int nManagers;       // Number of associated array managers
  int epoch;
  // Per-buffer sizes of the packed device stream, in pack order -- the
  // manifest the unpack asserts against, element-wise. Zero-length for
  // migrations whose device payload was not packed by PUP::toMem (the
  // by-handle path records no manifest).
  int nGpuBufs;
  // Move-ledger tag: PE whose LBManager tracks this move (-1 for a move no
  // balancer step is waiting on), and the step to ack it under. The
  // destination retires the entry once the element has registered.
  int lbLedgerSrcPe = -1;
  int lbLedgerStep = -1;
  // Device STATE but no device PAYLOAD: nothing had to be copied out of the
  // element (gpuBufSize 0), yet its unpack allocates its buffers again here,
  // so it owes the admission gate a landing just as a payload does. Set by
  // the source, which also sends the request; the destination parks this
  // message until the gate has reserved those blocks (CkLocMgr::immigrate).
  bool needsLanding = false;
  // The element's Stable device buffers (PUP::DeviceLife::Stable) were shipped
  // at the balancer's decision as a separate payload (immigrateStable) and
  // are not in this message's device stream: their manifest entries carry
  // DEVICE_MANIFEST_STABLE, and the unpack rebinds them into that landing.
  bool hasStable = false;
  int stableGen = 0;
  int stableBytes = 0;
  int stableCount = 0;
  char* packData;
  size_t* gpuManifest;
};

// What the decision-time snapshot of an element's Stable device buffers
// amounts to, on the source, from the snapshot to the pack that consumes it.
struct CkStableSnapshot {
  int toPe;
  int gen;
  void* block;                      // the copy (a pool block), freed at the destination's ack
  size_t bytes;                     // the stable region's extent
  size_t count;                     // Stable buffers in it
  std::vector<size_t> manifest;     // their sizes, in walk order
  unsigned long long* hashes;       // device: count words at the snapshot, count at the pack, one flag; or null
  void* done;                       // cudaEvent_t after the snapshot's copies and hashes: the pack's compare waits on it
  double takenAt;
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
    CkMarshallMsg* impl_msg_typed = (CkMarshallMsg*)impl_msg;
    char* impl_buf = impl_msg_typed->msgBuf;
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

#if CMK_GPU
/// A device migration payload staged on the source, waiting for the
/// destination to read it. The buffer is a copy of the element's device state,
/// not the state itself, so the element can be destroyed as soon as the pack
/// finishes; only this buffer has to outlive the transfer.
class GPUMigrateData
{
public:
  int toPe;
  size_t size;
  void* data;
  // The stream the pack copies were issued on. The payload send stages from
  // `data` on this stream, which orders it behind those copies on the device
  // -- the ordering emigrate used to buy by waiting on the host.
  void* stream;
  // The element's resident device footprint at the source, read before the
  // pack, as the blocks that make it up ((size, count) pairs, flattened):
  // what its unpack will allocate at the destination on top of the landing
  // arena. The admission gate allocates exactly these before it grants.
  size_t footprint;
  std::vector<CmiUInt8> blocks;
  double dispatchedAt = 0.0;   // when the payload send was issued (the window tally)
  double requestedAt = 0.0;    // when its landing was requested (grant-wait)

  GPUMigrateData()
      : toPe(-1), size(0), data(nullptr), stream(nullptr), footprint(0) {}
  GPUMigrateData(int toPe_, int size_, void* data_, void* stream_ = nullptr,
                 size_t footprint_ = 0, std::vector<CmiUInt8> blocks_ = {})
      : toPe(toPe_), size(size_), data(data_), stream(stream_),
        footprint(footprint_), blocks(std::move(blocks_)) {}
};
#endif

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

// CHARM_ZC_STATS: split whichPe() misses into "no id for this index" versus
// "id known, location unknown". They need completely different fixes, and the
// send path cannot tell them apart from a -1.
void ckLocNoteWhichPeMiss(bool noId);

namespace ck {
namespace objid {
/**
 * Per-run layout of the element payload of an array whose index is not packed into
 * the id ("hashed kind"): the top keyBits are a hash key of the index, and the home
 * of the element is (key % CkNumNodes()) on any PE, from the id or from the index
 * alike; the low uniqueBits are a number unique within the array, handed out in
 * tranches. One copy per process, set at startup by rank 0 (initLayout) from the
 * process count and the +objid_expand factor; a checkpoint restores the launch-time
 * layout instead (CkPupROData), because keys live inside ids that survive the
 * restart.
 */
struct Layout
{
  int keyBits = 0;
  int uniqueBits = 0;
  int expandFactor = 8;
  int nodesAtLaunch = 0;
  int trancheLog2Cap = 0;  // +objid_tranche_log2: cap on tranche sizes (testing); 0 = none
  void pup(PUP::er& p)
  {
    p | keyBits;
    p | uniqueBits;
    p | expandFactor;
    p | nodesAtLaunch;
    p | trancheLog2Cap;
  }
};
/// Called once per process (rank 0) before any chare array exists.
void initLayout(int expandFactor, int trancheLog2Cap);
const Layout& getLayout();
/// Checkpoint the layout with the readonlies; on restore it replaces the
/// startup layout of this process (called by CkPupROData).
void pupLayout(PUP::er& p);
/// Hash key of an index, in [0, 2^keyBits): a pure function of the index bytes.
CmiUInt8 indexHashKey(const CkArrayIndex& idx);

/// Collection numbers (objid.h): the next one for a new array, standard or wide.
/// PE 0 only (CkCreateArray). Prints the wide-collection suggestion once when the
/// standard range is half used, aborts when either range is exhausted.
CmiUInt8 nextCollectionNumber(bool wide);
/// A restart restores arrays with their numbers: keep the counters above them.
void noteRestoredCollection(CmiUInt8 number, bool wide);

/// Size (log2) of the first tranche the allocator grants a process that has none
/// (after a restart), unless 1/64 of its cap (region / (8 * processes)) is smaller:
/// then that, so a restart on which every process inserts costs the pool at most
/// 1/512 of itself at any process count. Grants double from the first one up to the
/// cap (CkLocMgr::requestTranche).
constexpr int FIRST_GRANT_LOG2 = 16;

/// A process's current tranche of unique numbers for one array: [cursor, end),
/// plus the spare tranche the allocator granted for when it runs out. The fast path
/// (CkLocMgr::mintUnique) touches only the three atomics; everything else is read
/// and written under _nodeLock.
struct Tranche
{
  std::atomic<CmiUInt8> cursor{0};
  std::atomic<CmiUInt8> end{0};
  std::atomic<CmiUInt8> refillAt{~(CmiUInt8)0};  // cursor value at which to ask for the next
  CmiUInt8 len = 0;                            // length of the current tranche
  CmiUInt8 nextBase = 0, nextLen = 0;          // the spare, installed by grantTranche
  bool haveNext = false;
  bool requested = false;                      // a requestTranche is in flight
  std::vector<int> waitingPes;                 // PEs holding deferred insertions
};
}  // namespace objid
}  // namespace ck

class CkLocMgr;

class CkLocCache : public CBase_CkLocCache
{
private:
  // Map of ID to PE
  using LocationMap = std::unordered_map<CmiUInt8, CkLocEntry>;
  LocationMap locMap;

  using Listener = std::function<void(CmiUInt8, int)>;
  std::list<Listener> listeners;

  // Elements this PE has already asked the home PE about, so a stream of sends
  // to an element whose location is unknown costs one request rather than one
  // per message.
  std::unordered_set<CmiUInt8> pendingLocReqs;

  // The location manager this cache serves (created one-to-one with it; bound arrays
  // share both). Needed to compute the home of an id, which is not stored in the id.
  CkLocMgr* mgr = nullptr;

public:
  CkLocCache() = default;
  CkLocCache(CkMigrateMessage* m) : CBase_CkLocCache(m) {}
  ~CkLocCache() = default;
  void pup(PUP::er& p);

  void setManager(CkLocMgr* m) { mgr = m; }

  void requestLocation(CmiUInt8 id);
  // Ask the home PE where an element lives, at most once until the answer
  // arrives. Delivery forwards an unresolvable message via the home PE without
  // ever learning the location, so without this a sender keeps paying the
  // detour -- and, for a device-zerocopy send, keeps choosing its transfer mode
  // for an unknown destination.
  void requestLocationOnce(CmiUInt8 id)
  {
    // CHARM_NO_LOCREQ restores the old behaviour (forward via home forever,
    // never learn) so the trade can be measured: learning costs a request per
    // sender/element pair, and migration invalidates what was learned.
    static const bool disabled = (getenv("CHARM_NO_LOCREQ") != nullptr);
    if (disabled) return;
    if (locMap.find(id) != locMap.end()) return;
    if (!pendingLocReqs.insert(id).second) return;
    requestLocation(id);
  }

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
  int homePe(const CmiUInt8 id) const;  // via the manager; defined after CkLocMgr

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

  // A migration with device state travels as two independent sends: the host
  // message through immigrate(), and the device payload through immigrateGPU()
  // on the device-zerocopy path. Neither ordering is guaranteed, so whichever
  // arrives first waits here for the other.
  std::unordered_map<CmiUInt8, GPUMigrateData> sendGPUBuffers;          // source: payloads awaiting the ack
  std::unordered_map<CmiUInt8, CkArrayElementMigrateMessage*> bufferedHostMigrateMsgs;
  std::unordered_map<CmiUInt8, void*> bufferedDeviceMigrateMsgs;
  std::unordered_map<CmiUInt8, void*> receivedDeviceMsgs;

  // Chares whose migration into this PE has begun but not finished: the source
  // has released them and this PE has not constructed them yet, so they exist
  // on no PE at all. Every location cache already names this PE as their home
  // for the duration, so a message that arrives in that window would otherwise
  // be handed to the scheduler, come straight back, and spin forever. See
  // CkArray::sendToPe.
  std::unordered_set<CmiUInt8> inFlightImmigrations;

  // The mapping of index to ID is either done via compression or an explicit map,
  // depending on if the bounds of this array are compressible into a 64bit ID.
  CkArrayIndex bounds;
  ck::ArrayIndexCompressor* compressor;
  // Id layout of this array (objid.h): wide-collection or standard, the collection
  // number of the array that created this manager (bound arrays have their own
  // numbers but share the layout), and the payload width that follows from the kind.
  bool wideCollection;
  CmiUInt8 collectionNumber;
  int payloadBits;
  IdxIdMap idx2id;    // Explicit map for non-compressible case
  // Hashed kind: ids that no longer name a live element -> index: an id this PE
  // rebound to a newer one (insertID) and, at the home, every dead element's id
  // (noteDead). Never pruned, like idx2id. doc/internals/object-ids.md, section 3.
  std::unordered_map<CmiUInt8, CkArrayIndex> retiredIds;
  // Non-compressible case: this process's tranche of unique numbers for this array,
  // shared by all its PEs (see ck::objid::Tranche); resolved on first use.
  ck::objid::Tranche* tranche;
  ck::objid::Tranche* getTranche();
  // Unpacked from a checkpoint: this process's share of the initial region is spent
  // (the restored ids came from it), so the first tranche comes from the allocator.
  // An array created after a restart is not restored and takes its share as usual.
  bool restoredFromCheckpoint;

  /// This flag is set while we delete an old copy of a migrator
  bool duringMigration;
  /// This flag is set while we are deleting location manager
  bool duringDestruction;

  // Internal interface:
  void pupElementsFor(PUP::er& p, CkLocRec* rec, CkElementCreation_t type,
                      bool rebuild = false);

  bool checkInBounds(const CkArrayIndex& idx) const;

  // Get a new ID based on the ID generation scheme. False when the hashed kind has
  // no unique number to give right now (tranche exhausted, refill not yet here).
  bool tryNewObjectID(const CkArrayIndex& idx, CmiUInt8& id);
  // Take the next unique number from this process's tranche; false if exhausted,
  // in which case a refill has been requested and this PE is registered to be woken.
  bool mintUnique(CmiUInt8& u);
  void requestTrancheFromAllocator(int wantLog2);
  // Allocator state (PE 0's branch is the allocator; other branches carry a copy
  // that is never used): [allocNext, allocEnd) is the unhanded part of the pool.
  CmiUInt8 allocNext, allocEnd;
  struct DeferredInsertion
  {
    CkGroupID mgr;  // looked up at resume: the array may have been destroyed meanwhile
    CkArrayMessage* msg;
    CkArrayIndex idx;
    int listenerData[CK_ARRAYLISTENER_MAXLEN];
  };
  std::vector<DeferredInsertion> deferredInsertions;  // this PE's, waiting for a tranche

  // Insert the ID into the idx2id table
  inline void insertID(const CkArrayIndex& idx, const CmiUInt8 id)
  {
    if (compressor)
      return;
    auto ins = idx2id.emplace(idx, id);
    if (!ins.second && ins.first->second != id)
    {
      retiredIds[ins.first->second] = idx;
      ins.first->second = id;
    }
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

  // Residency table maintenance, driven from insertRec/removeFromTable so that
  // every arrival and departure inside this process passes through it.
  CmiUInt8 residentKey(CmiUInt8 id) const;
  void noteResident(CmiUInt8 id);
  void clearResident(CmiUInt8 id);
  // Look up location record in the table, return NULL if not there.
  CkLocRec* elementNrec(const CmiUInt8 id) const;
  // Device corrections need to find the receiving element to stand its
  // migration down; see CkNoteDeviceRecvDeferred in cklocation.C.
  friend void CkNoteDeviceRecvDeferred(CkGroupID aid, CmiUInt8 id);
  friend void CkNoteDeviceRecvComplete(CkGroupID aid, CmiUInt8 id);
  // The element a device zerocopy receive is addressed to, or NULL: lets the
  // registration cache retire receive-side entries when the element migrates.
  friend CkLocRec* CkFindDeviceRecvElement(CkGroupID aid, CmiUInt8 id);
  friend void _deferredMigrateHandler(void* arg);
  // The balancer's replication decisions reach the element(s) at a location
  // through callMethod (CkLocRec::staticClone / staticRetire).
  friend class CkLocRec;

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

  bool isWideCollection() const { return wideCollection; }
  CmiUInt8 getCollectionNumber() const { return collectionNumber; }
  int getPayloadBits() const { return payloadBits; }
  /// The id of an element of this manager's primary array, from its payload (used
  /// for load balancer object ids; CkArray::makeID gives the per-array id).
  ck::ObjID makeID(CmiUInt8 eid) const { return ck::ObjID(collectionNumber, eid, wideCollection); }

  CkLocRec* registerNewElement(const CkArrayIndex& idx);

  // Interface used by external users:
  /// Home mapping. One home per element, computable from the index or from the id
  /// alike, on any PE, under the current process count. Packed-index arrays use the
  /// array map, as before; hashed-kind arrays hash the index to a process.
  /// The key selects the process (key mod processes); the quotient, which is
  /// independent of the remainder, selects the rank within it, so a process's homes
  /// spread over its PEs instead of all sitting on rank 0 (in a single process they
  /// spread over every PE). Both are recomputed under the current topology, so a
  /// restart with any process or PE count rebuilds the directories at the new homes;
  /// nothing about the home is stored in a checkpoint. More +objid_expand means more
  /// keys per process and therefore a more even spread over its ranks.
  static int hashedHomePe(CmiUInt8 key)
  {
    const CmiUInt8 nodes = (CmiUInt8)CkNumNodes();
    const int node = (int)(key % nodes);
    const int rank = (int)((key / nodes) % (CmiUInt8)CkNodeSize(node));
    return CMK_RANK_0(CkNodeFirst(node) + rank);
  }
  int homePe(const CkArrayIndex& idx) const
  {
    if (compressor)
      return CMK_RANK_0(map->homePe(mapHandle, idx));
    return hashedHomePe(ck::objid::indexHashKey(idx));
  }
  int homePe(const CmiUInt8 id) const
  {
    if (compressor)
      return homePe(compressor->decompress(id));
    return hashedHomePe(id >> ck::objid::getLayout().uniqueBits);
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
    {
      ckLocNoteWhichPeMiss(true);   // never learned this index's element id
      return -1;
    }
    const int pe = cache->getPe(id);
    if (pe == -1) ckLocNoteWhichPeMiss(false);  // knew the id, not the location
    return pe;
  }

  CmiUInt8 lookupID(const CkArrayIndex& idx) const
  {
    CkAssert(checkInBounds(idx));
    if (compressor)
    {
      return compressor->compress(idx);
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
      id = compressor->compress(idx);
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

  /// id -> index, but report failure instead of asserting when this PE has
  /// never heard of the element.
  ///
  /// lookupIdx below can only resolve an id that this PE already knows: with
  /// no index compressor it needs either a local record or an idx2id entry.
  /// A PE that is neither the source nor the destination of a migration, and
  /// has never talked to the element, has neither -- so a broadcast location
  /// update (CMK_GLOBAL_LOCATION_UPDATE) reaches PEs that legitimately cannot
  /// name the index. Those PEs have nothing to refresh and should skip it.
  bool tryLookupIdx(const CmiUInt8& id, CkArrayIndex& out) const
  {
    CkLocRec* rec = nullptr;
    if (compressor)
    {
      out = compressor->decompress(id);
      return true;
    }
    else if ((rec = elementNrec(id)))
    {
      out = rec->getIndex();
      return true;
    }
    for (IdxIdMap::const_iterator itr = idx2id.begin(); itr != idx2id.end(); itr++)
    {
      if (itr->second == id)
      {
        out = itr->first;
        return true;
      }
    }
    return false;
  }

  /// id -> index. Only for a packed-index array (decode) or an element that lives
  /// here (its record). No delivery path may call this for an element this PE has
  /// never seen: delivery by id routes through homePe(id) and never needs the index.
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
    CkAbort("CkLocMgr::lookupIdx: the index of element id %" PRIx64
            " (location manager %d) is not known on PE %d\n",
            id, thisgroup.idx, CkMyPe());
    return CkArrayIndex();
  }

  /// id -> index where it can be had without a message: the compressor, or a local
  /// record, or an id that died or was rebound here (retiredIds; at the home that
  /// covers every element of its that ever died). Returns false when none applies.
  bool recoverIndex(CmiUInt8 id, CkArrayIndex& idx) const;

  /// Ask the home of idx for its location by index, whether or not this PE already
  /// holds an id for it. The home buffers the request until the element exists, so
  /// this is the request to pair with a demand-creation request.
  void requestLocationAtHome(const CkArrayIndex& idx)
  {
    const int home = homePe(idx);
    if (home != CkMyPe()) thisProxy[home].requestLocation(idx, CkMyPe());
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
  // Fast path for intra-process (same CmiNode) migration: transfer live
  // chare objects to the destination PE without packing. Returns false if
  // the fast path is not applicable and the caller should fall through to
  // the packed emigrate path.
  bool emigrateIntraProcess(CkLocRec* rec, int toPe);
  void immigrateIntraProcess(CkArrayElementIntraProcessMigrateMessage* msg);

  // See inFlightImmigrations. Registration happens wherever this PE first
  // learns an element is on its way but cannot yet construct it; the entry is
  // dropped in createLocal, the one point every arrival passes through.
  // Process residency table. Authoritative for "is this element in my process,
  // and on which PE" -- unlike the per-PE location cache, which only knows what
  // it last heard. See CkResidentTable in cklocation.C for why that difference
  // matters to device zerocopy.
  int residentOwnerPe(CmiUInt8 id) const;
  static size_t numResidentInProcess();

  // Destination PE a device zerocopy send should decide its transfer mode
  // from. Consults the residency table rather than the location cache alone.
  int deviceSendDestPe(const CkArrayIndex& idx);

  void noteImmigrationInFlight(CmiUInt8 id);
  // Public id lookup for the runtime's device-work accounting (ckrdmadevice.C).
  CkLocRec* recForDeviceWork(CmiUInt8 id) const { return elementNrec(id); }
  bool isImmigrationInFlight(CmiUInt8 id) const
  {
    return inFlightImmigrations.find(id) != inFlightImmigrations.end();
  }
  size_t numImmigrationsInFlight() const { return inFlightImmigrations.size(); }

#if CMK_GPU
  // Send a staged device payload to its destination. Runs as its own entry
  // method rather than inline in emigrate so the send is attributed to the
  // runtime and not to whichever element last ran on this PE.
  void sendGPUMsg(CmiUInt8 id);
  void immigrateGPU(CmiUInt8& id, int& size, char* &data, int& srcPe, CkDeviceBufferPost* post);
  void immigrateGPU(CmiUInt8 id, int size, char* data, int srcPe);
  void finishGPUSend(CmiUInt8 id);

  // The landing admission gate (+gpupool; see ckLandingAdmissible in
  // cklocation.C). The source asks before sending a payload, naming the
  // blocks its element holds; the destination grants once it has taken the
  // landing arena AND every one of those blocks from the pool without
  // growing it, above the floor of payloads this process still owes. The
  // blocks are the landing set: the element's unpack is served from it
  // (hapiPreallocBegin), and whatever it leaves is freed after the unpack.
  void requestLanding(CmiUInt8 id, int size, int srcPe, int n, CmiUInt8* blocks);
  void landingGranted(CmiUInt8 id);
  void admitLandings();
  void admitLandingsLoop();
  bool admittingLandings = false;     // admitLandings is not re-entrant
  bool admitLandingsAgain = false;    // a nested call asks for another round
  void releaseLandingSet(CmiUInt8 id);
  struct DeferredLanding {
    CmiUInt8 id;
    int size;
    int srcPe;
    size_t footprint;                 // sum of blocks
    std::vector<CmiUInt8> blocks;     // (size, count) pairs, flattened
    double since;
    bool reported = false;
    bool warned = false;
  };
  std::deque<DeferredLanding> deferredLandings;
  std::unordered_map<CmiUInt8, char*> grantedLandings;
  // The landing set per granted landing, from the grant to the unpack.
  std::unordered_map<CmiUInt8, std::multimap<size_t, void*>> landingSets;
  bool landingRetryScheduled = false;
  long landingDeferrals = 0;
  // The migration window's queue looks its moves up again when a place frees.
  CkLocRec* queuedMigrationRec(CmiUInt8 id) const { return elementNrec(id); }

  // Stable device buffers shipped at the balancer's decision (PUP::DeviceLife::
  // Stable). The source copies them into a pool block when the decision
  // arrives (recvMigrate), sends that block as a zerocopy payload while the
  // element keeps stepping, and the pack skips them; the destination keeps the
  // landed block until the element's host message arrives and rebinds the
  // buffers into it. A move that goes elsewhere after all discards it.
  void stableSnapshot(CkLocRec* rec, int toPe);
  void stableDiscard(CkLocRec* rec);
  void immigrateStable(CmiUInt8& id, int& size, char*& data, int& srcPe, int& gen, CkDeviceBufferPost* post);
  void immigrateStable(CmiUInt8 id, int size, char* data, int srcPe, int gen);
  void finishStableSend(CmiUInt8 id, int gen);
  void discardStable(CmiUInt8 id, int gen);
  struct StableSend { void* block; size_t bytes; int toPe; int gen; };
  std::map<std::pair<CmiUInt8, int>, StableSend> stableSends;   // source: the block, until the destination's ack
  struct StableLanding { char* block; int size; int gen; };
  std::unordered_map<CmiUInt8, StableLanding> stableLandings;  // destination: landed, waiting for the element
  std::map<std::pair<CmiUInt8, int>, char*> receivedStableMsgs; // destination: posted, landing
  std::unordered_map<CmiUInt8, int> stableDiscarded;            // destination: discard that generation when it lands
  std::unordered_map<CmiUInt8, double> payloadLandedAt;        // destination: the window tally's landing stamp
  std::unordered_map<CmiUInt8, double> payloadArrivedAt;       // destination: the envelope's arrival (rx-land)
private:
  void dispatchGPUMsg(CmiUInt8 id);
public:
#endif
  // The element left: tell the PEs that sent to it lately (CkLocRec::recentSenders)
  // where it is now, so their next message goes there directly.
  void informSenders(const CkArrayIndex& idx, CmiUInt8 id, const int* pes, int n, int toPe);
  void requestLocation(CmiUInt8 id);
  // See CkLocCache::requestLocationOnce.
  void requestLocationOnce(CmiUInt8 id) { cache->requestLocationOnce(id); }
  void requestLocation(const CkArrayIndex& idx);
  bool requestLocation(const CkArrayIndex& idx, int peToTell);
  void updateLocation(const CkArrayIndex& idx, const CkLocEntry& e);
#if CMK_LBDB_ON && CMK_GLOBAL_LOCATION_UPDATE
  /// A migration the load balancer decided (UpdateLocation): record, by id, that the
  /// element is about to live on pe, before the move is acted on.
  void updateLocationFromLB(CmiUInt8 id, int pe);
#endif
  void reclaimRemote(const CkArrayIndex& idx, int deletedOnPe);
  void noteDead(const CkArrayIndex& idx, CmiUInt8 id);

  // Tranche allocator (design section 3.1). The allocator is this group's branch on
  // PE 0; it hands out tranches of the upper half of the unique space, bottom up.
  void requestTranche(int node, int wantLog2);           // on PE 0
  void grantTranche(CmiUInt8 base, CmiUInt8 len);        // on rank 0 of the requesting process
  void resumeDeferredInsertions();                       // on a PE that deferred insertions
  // Hold an insertion until this process has a tranche again (see mintUnique).
  void deferInsertion(CkArray* mgr, CkArrayMessage* msg, const CkArrayIndex& idx,
                      const int listenerData[CK_ARRAYLISTENER_MAXLEN]);
  void dummyAtSync(void);

  /// return a list of migratables in this local record
  void migratableList(CkLocRec* rec, std::vector<CkMigratable*>& list);

  void flushAllRecs(void);
  void flushLocalRecs(void);
  void pup(PUP::er& p);
};

/// check the command line arguments to determine if we can use ConfigurableRRMap
bool haveConfigurableRRMap();

// The move window tally (CHARM_LB_MIGSTATS; MigStats in cklocation.C): the
// channel protocol reports its two waits here, 0 = a sender held from the
// closing to the next resolution, 1 = an owner from its closing to the ack.
bool ckMigStatsOn();
void ckMigStatChannel(int kind, double seconds);

/*@}*/

inline int CkLocCache::homePe(const CmiUInt8 id) const
{
  CkAssert(mgr != nullptr);
  return mgr->homePe(id);
}

#endif /*def(thisHeader)*/
