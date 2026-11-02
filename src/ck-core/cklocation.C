/** \file cklocation.C
 *  \addtogroup CkArrayImpl
 *
 *  The location manager keeps track of an indexed set of migratable objects.
 *  It is used by the array manager to locate array elements, interact with the
 *  load balancer, and perform migrations.
 *
 *  Orion Sky Lawlor, olawlor@acm.org 9/29/2001
 */

#include "TopoManager.h"
#include "charm++.h"
#include "ck.h"
#if CMK_GPU
void hapiCuptiObjectJoinedStep(const LDObjHandle&);
void hapiCuptiObjectResumed(const LDObjHandle&);
#endif
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <unistd.h>
#include "cksyncbarrier.h"
#include "hilbert.h"
#include "partitioning_strategies.h"
#include "pup_stl.h"
#include "register.h"
#include "trace.h"
#include <algorithm>
#include <limits>
#include <sstream>
#include <stdarg.h>
#include <vector>

#if CMK_GPU

#include "hapi.h"

// A stream synchronize that also covers work this PE has handed to a submitter
// thread (+gpusubmit): it has to be in the driver before the wait means anything.
static inline hapiError_t ckSubmitDrainThenSync(hapiStream_t stream) {
  hapiSubmitDrain();
  return hapiStreamSynchronize(stream);
}
#include "gpumanager.h"
#include "LBMigrateWindow.h"

CsvExtern(GPUManager, gpu_manager);

#endif // CMK_GPU

#if CMK_LBDB_ON
#  include "LBManager.h"
#  include "MetaBalancer.h"
#  if CMK_GLOBAL_LOCATION_UPDATE
#    include "BaseLB.h"
#    include "init.h"
#  endif
CkpvExtern(int, _lb_obj_index);
CkpvExtern(int, _lb_obj_rebuild_index);  // for lbdb user data for obj index
#endif                           // CMK_LBDB_ON

CpvExtern(std::vector<NcpyOperationInfo*>, newZCPupGets);  // used for ZC Pup
#ifndef CMK_CHARE_USE_PTR
CkpvExtern(int, currentChareIdx);
#endif

#if CMK_GRID_QUEUE_AVAILABLE
CpvExtern(void*, CkGridObject);
#endif

// Diagnostics, all off by default, all environment variables:
//   CHARM_DEBUG_MIGRATE=1   every step of a move: decision, kick, pack (with
//                           stage timers), payload dispatch/post/receipt,
//                           landing, parked/staged messages, forwards.
//   CHARM_PRINTF_TS=1       a wall-clock prefix on every printed line, so the
//                           prints of all PEs and processes lie on one timeline.
//   +LBDebug 1              per-PE host load the balancer received ([PELOAD]).
static inline bool migDbg() {
  static const bool on = (getenv("CHARM_DEBUG_MIGRATE") != nullptr);
  return on;
}
// CHARM_MIG_TRACE=1: one line per hop of a device migration, with the wall clock (CLOCK_REALTIME, comparable
// across the job's nodes to ~1 ms), for an offline per-hop breakdown of the ship and grant windows.
static inline bool migTrace() { static const bool on = (getenv("CHARM_MIG_TRACE") != nullptr); return on; }
static inline double migNow() { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }
#define MIGT(stage, id, ptr) do { if (migTrace()) CmiPrintf("[MIGT %d] %s id=%llu ptr=%p t=%.6f\n", CkMyPe(), stage, (unsigned long long)(id), (const void*)(ptr), migNow()); } while (0)

#if CMK_GPU
// The landing admission gate (+gpupool). The memory contract plans each
// balancer step so that every batch fits the devices; this is the runtime half
// that keeps its assumption true. A staged arrival asks its destination for a
// landing before its payload is sent, and the destination grants it only if
// the pool can hand out the landing arena AND the state the element's unpack
// will allocate, and still cover every payload this process has yet to pack
// for departures the balancer ordered -- the floor. Packs are never gated and
// never wait on anything remote, so an arrival that is refused waits only on
// local progress: its own process's packs, a payload ack, or a free. A
// deferred arrival costs the destination nothing; its payload stays in the
// sender's pool block.
//
// The arrival's full cost is the payload block plus the element's resident
// footprint at the source, read before the pack as the blocks that make it up
// (GPUMigrateData::blocks). The gate reserves by ALLOCATING: it takes the
// landing arena and every one of those blocks from the pool without growing
// it, or defers. Byte sums are not enough under a buddy allocator -- a pool
// with 50 MB "free" on each of four ranks admitted arrivals by bytes and every
// rank still grew an 8 GB arena (job 22191010), because the 8 MB landings
// split the 16 MB blocks the arrivals' state needed. The blocks taken are the
// landing set: the element's unpack is served from it (hapiPreallocBegin), so
// it allocates nothing new; what it leaves -- everything, for an element that
// rebinds its buffers into the arena instead -- is freed after the unpack.
// Gating on the payload alone had admitted arrivals into memory that was not
// there, and under +LBAsync -- where departures stay resident until their own
// pack point -- grew the pool by whole arenas.
//
// Under the pool, payloads, landing arenas and chare state are all blocks of
// one pool. Without the floor an arrival could take the bytes a departure
// needs to pack, and that departure's destination could be waiting on an
// arrival of its own: the cycle a separate staging reserve used to rule out.
//
// The gate reads what the pool's arenas have free NOW, exactly as the batch
// planner did (LBMemoryContract.h): growing the pool is not credited, since
// growth is a whole arena or nothing and at 2+ nodes every arena is
// registered with the fabric, which has a ceiling (hapiDevPoolNewArenaLocked).
static std::atomic<size_t> ck_pending_pack_bytes{0};

static inline bool ckAdmissionGateOn() { return CkDevicePoolOn(); }

static inline size_t ckPoolBlock(size_t bytes)
{
  if (bytes == 0) return 0;
  size_t b = 4;
  while (b < bytes) b <<= 1;
  return b;
}

// The payload block a move of rec to toPe will pack: none for a same-process
// move onto the same device, which is a handoff.
static size_t ckStagedBytes(CkLocRec* rec, int toPe)
{
  if (CmiNodeOf(toPe) == CkMyNode() &&
      hapiDeviceForPe(toPe) == hapiDeviceForPe(CkMyPe()))
    return 0;
  return ckPoolBlock(rec->lbGpuPupSize);
}

// A move the balancer ordered is pending a pack from now until emigrate packs
// it or hands the element over without packing. Only moves that will stage
// count.
static void ckNotePendingPack(CkLocRec* rec, int toPe)
{
  if (!ckAdmissionGateOn()) return;
  const size_t bytes = ckStagedBytes(rec, toPe);
  if (rec->pendingPackBytes > 0) ck_pending_pack_bytes -= rec->pendingPackBytes;
  rec->pendingPackBytes = bytes;
  if (bytes > 0) ck_pending_pack_bytes += bytes;
}

static void ckClearPendingPack(CkLocRec* rec)
{
  if (rec->pendingPackBytes == 0) return;
  ck_pending_pack_bytes -= rec->pendingPackBytes;
  rec->pendingPackBytes = 0;
}

// Whether a landing of `size` payload bytes, for an element whose state is
// `footprint` bytes, could be taken from the pool now, by bytes: the pool's
// free bytes in the arenas it has, less the floor of pending packs. Earlier
// grants are already allocated, so they are already out of the free bytes.
// This is the cheap pre-check; the grant itself allocates (admitLandings).
static bool ckLandingAdmissible(size_t size, size_t footprint, size_t* reachOut,
                                size_t* floorOut)
{
  size_t devFree = 0, poolFree = 0, arena = 0;
  int slots = 0;
  hapiLBDeviceMemory(&devFree, &poolFree, &arena, &slots);
  const size_t reach = poolFree;
  const size_t floor = ck_pending_pack_bytes.load();
  *reachOut = reach;
  *floorOut = floor;
  return reach >= ckPoolBlock(size) + footprint + floor;
}

// Take the landing arena and the element's blocks from the pool, without
// growing it unless `grow`; on any miss, give everything back and report
// failure. Largest blocks first, which is the order a buddy allocator fits
// best.
static bool ckTakeLandingSet(int size, const std::vector<CmiUInt8>& blocks,
                             char** arenaOut, std::multimap<size_t, void*>& set,
                             bool grow = false)
{
  std::vector<std::pair<size_t, int>> want;
  for (size_t k = 0; k + 1 < blocks.size(); k += 2)
    want.emplace_back((size_t)blocks[k], (int)blocks[k + 1]);
  std::sort(want.begin(), want.end(),
            [](const std::pair<size_t, int>& a, const std::pair<size_t, int>& b) {
              return a.first > b.first;
            });
  hapiFootprintBegin(nullptr);   // nobody's until the unpack takes them
  bool ok = true;
  for (const auto& w : want) {
    for (int c = 0; ok && c < w.second; c++) {
      void* p = grow ? CkDeviceMalloc(w.first) : CkDeviceMallocNoGrow(w.first);
      if (p == nullptr) ok = false;
      else set.emplace(w.first, p);
    }
    if (!ok) break;
  }
  char* arena = nullptr;
  if (ok && size > 0) {
    arena = (char*)(grow ? CkDeviceMalloc((size_t)size) : CkDeviceMallocNoGrow((size_t)size));
    if (arena == nullptr) ok = false;
  }
  hapiFootprintEnd();
  if (!ok) {
    for (auto& e : set) CkDeviceFree(e.second);
    set.clear();
    if (arena) CkDeviceFree(arena);
    return false;
  }
  *arenaOut = arena;
  return true;
}

// Under the vmm pool a block never crosses a chunk boundary, so the free bytes
// a landing was admitted on can sit in holes too small for the element's
// largest block (job 23199057: 3451 MB free, largest hole 160 MB, 302 MB
// slabs). Waiting for a free is then waiting on this device's departures in a
// later batch, and the next batch is released only when this one completes:
// the landing sat out the whole timeout and then grew the pool after all. A
// vmm chunk is mapped and registered on its own, with no per-process ceiling,
// so the landing maps what it is short of instead, if the device has the
// memory: the set rounded up to whole chunks, and one chunk more.
static bool ckTakeLandingSetGrowingVmm(int size, size_t footprint,
                                       const std::vector<CmiUInt8>& blocks,
                                       char** arenaOut, std::multimap<size_t, void*>& set)
{
  if (!hapiDevPoolIsVmm()) return false;
  size_t devFree = 0, poolFree = 0, chunk = 0;
  int slots = 0;
  hapiLBDeviceMemory(&devFree, &poolFree, &chunk, &slots);
  if (chunk == 0) return false;
  const size_t need = ckPoolBlock(size) + footprint;
  if (devFree < (need + chunk - 1) / chunk * chunk + chunk) return false;
  return ckTakeLandingSet(size, blocks, arenaOut, set, /*grow=*/true);
}

// The migration window (+gpupool): pacing, not the safety argument. The batch
// planner and the admission gate are what keep a step inside device memory;
// this bounds how much of a step one PE has in flight at once, so a balancer
// that issues a whole step together (DiffusionLB, or a central step the planner
// kept in one batch) does not hold every payload block and every IPC slot of
// the step at the same moment. A staged move takes a place when recvMigrate
// sees it, if this PE has fewer moves in flight than its IPC slot budget and
// one more payload keeps its in-flight bytes within the byte bound; otherwise it
// queues in order. The place is given back by the payload's ack
// (finishGPUSend), or as soon as the move turns out not to stage. The byte
// bound per PE is min(256 MB, a quarter of the pool's capacity shared by
// the device's PEs) -- CHARM_LB_MIGRATE_WINDOW_MB moves the 256 -- and the
// balancer reserves exactly that per PE for landings in flight (L_g): the
// rule lives in LBMigrateWindow.h, so the two are one number.
//
// CHARM_LB_MIGRATE_WINDOW (moves) and CHARM_LB_MIGRATE_WINDOW_MB override the
// two bounds.
struct CkMigrateWindow
{
  struct Entry { size_t bytes; bool packed; };
  std::unordered_map<CmiUInt8, Entry> inflight;  // by element id
  size_t bytes = 0;
  struct Queued { CkLocMgr* mgr; CmiUInt8 id; int toPe; };
  std::deque<Queued> queue;
  bool pumpScheduled = false;
};
static thread_local CkMigrateWindow ck_window;  // one per PE

static inline bool ckWindowOn() { return CkDevicePoolOn(); }

static void ckWindowLimits(int* moves, size_t* bytes)
{
  static thread_local int m = -1;
  if (m < 0) {
    size_t devFree = 0, poolFree = 0, arena = 0;
    int slots = 0;
    hapiLBDeviceMemory(&devFree, &poolFree, &arena, &slots);
    const char* em = getenv("CHARM_LB_MIGRATE_WINDOW");
    m = em ? atoi(em) : slots;
  }
  // The byte bound follows the pool: the balancer reserved at most a quarter
  // of the pool's capacity on the device for landings in flight (L_g,
  // LBMigrateWindow.h), so each PE's window is its share of that, never more
  // than the configured 256 MB. Capacity, not the bytes free now: that is the
  // number the balancer reserved from, and the two must be one.
  size_t poolFree = 0;
  int pes = 1;
  hapiLBDevicePool(&poolFree, &pes);
  const size_t capacity = hapiLBDevicePoolCapacity();
  *moves = m;
  *bytes = lbLandingWindowBytes(capacity > poolFree ? capacity : poolFree, (size_t)pes);
}

// Takes a place for a move of `bytes` if there is one. The first move always
// goes, so a payload larger than the byte bound still moves, alone.
static bool ckWindowTake(CmiUInt8 id, size_t bytes)
{
  int maxMoves;
  size_t maxBytes;
  ckWindowLimits(&maxMoves, &maxBytes);
  const bool empty = ck_window.inflight.empty();
  if (!empty && maxMoves > 0 && (int)ck_window.inflight.size() >= maxMoves) return false;
  if (!empty && ck_window.bytes + bytes > maxBytes) return false;
  ck_window.inflight[id] = CkMigrateWindow::Entry{bytes, false};
  ck_window.bytes += bytes;
  return true;
}

static void ckWindowPump(void*, double);

static void ckWindowGiveBack(CmiUInt8 id, bool onlyIfUnpacked)
{
  auto it = ck_window.inflight.find(id);
  if (it == ck_window.inflight.end()) return;
  if (onlyIfUnpacked && it->second.packed) return;
  ck_window.bytes -= it->second.bytes;
  const size_t bytes = it->second.bytes;
  ck_window.inflight.erase(it);
  if (!ck_window.queue.empty() && !ck_window.pumpScheduled) {
    ck_window.pumpScheduled = true;
    CcdCallFnAfter(ckWindowPump, nullptr, 0.0);  // not from inside emigrate
  }
}
#endif

// Process-wide table of which PE inside this process currently owns each
// element, keyed by the globally unique (array id, element id) pair.
//
// The per-PE location cache cannot answer this question reliably. It names a PE
// from whatever it last heard, so it can still claim an element is in this
// process after the element left -- if it left from a *different* PE of the
// process, this PE has heard nothing. Every arrival and departure inside the
// process runs through CkLocMgr::insertRec and removeFromTable, so this table
// cannot be stale about residency.
//
// That is exactly what a device zerocopy sender needs: "same process" is "same
// GPU", and it decides whether the receiver may read the sender's device
// pointer directly (MEMCPY) or the payload has to be staged. Getting it wrong
// in the optimistic direction hands the receiver a pointer into an address
// space it cannot read. A function-local static rather than a Csv: one copy per
// process image is precisely the scope wanted, and C++11 guarantees the
// initialization is thread safe without any runtime plumbing.
namespace {
struct CkResidentTable {
  std::mutex lock;
  std::unordered_map<CmiUInt8, int> owner;
};
CkResidentTable& residentTable() {
  static CkResidentTable table;
  return table;
}
}  // namespace

#define ARRAY_DEBUG_OUTPUT 0

#if ARRAY_DEBUG_OUTPUT
#  define DEB(x) CkPrintf x   // General debug messages
#  define DEBI(x) CkPrintf x  // Index debug messages
#  define DEBC(x) CkPrintf x  // Construction debug messages
#  define DEBS(x) CkPrintf x  // Send/recv/broadcast debug messages
#  define DEBM(x) CkPrintf x  // Migration debug messages
#  define DEBL(x) CkPrintf x  // Load balancing debug messages
#  define DEBN(x) CkPrintf x  // Location debug messages
#  define DEBB(x) CkPrintf x  // Broadcast debug messages
#  define AA "LocMgr on %d: "
#  define AB , CkMyPe()
#  define DEBUG(x) CkPrintf x
#  define DEBAD(x) CkPrintf x
#else
#  define DEB(X)   /*CkPrintf x*/
#  define DEBI(X)  /*CkPrintf x*/
#  define DEBC(X)  /*CkPrintf x*/
#  define DEBS(x)  /*CkPrintf x*/
#  define DEBM(X)  /*CkPrintf x*/
#  define DEBL(X)  /*CkPrintf x*/
#  define DEBN(x)  /*CkPrintf x*/
#  define DEBB(x)  /*CkPrintf x*/
#  define str(x)   /**/
#  define DEBUG(x) /**/
#  define DEBAD(x) /*CkPrintf x*/
#endif

// whether to use block mapping in the SMP node level
bool useNodeBlkMapping;

/// Message size above which the runtime will buffer messages directed at
/// unlocated array elements
int _messageBufferingThreshold;

#if CMK_LBDB_ON

#  if CMK_GLOBAL_LOCATION_UPDATE
// Called by the load balancer on every PE but the source, for each migration it
// decided, before the move is acted on (CentralLB::ReceiveMigration): from here on
// this PE addresses the element at its destination, so no message takes an extra
// hop (which CkLocMgr::multiHop asserts against in this mode). Keyed by id: the
// index is not recoverable on a PE that has never seen a hashed-kind element, and
// is not needed.
void UpdateLocation(MigrateInfo& migData)
{
  // The record's object manager is the element's CkLocMgr (initLB registers thisgroup).
  CkLocMgr* mgr = static_cast<CkLocMgr*>(CkLocalBranch(migData.obj.omID().id));
  if (mgr == nullptr)
    return;
  const ck::ObjID oid(migData.obj.id);
  mgr->updateLocationFromLB(oid.getElementID(), migData.to_pe);
}
#  endif

#endif

/*********************** Array Messages ************************/
CmiUInt8 CkArrayMessage::array_element_id(void)
{
  return ck::ObjID(UsrToEnv((void*)this)->getRecipientID()).getElementID();
}
unsigned short& CkArrayMessage::array_ep(void)
{
  return UsrToEnv((void*)this)->getsetArrayEp();
}
unsigned short& CkArrayMessage::array_ep_bcast(void)
{
  return UsrToEnv((void*)this)->getsetArrayBcastEp();
}
// A small self-message carrying the identity of an element whose deferred
// migration is ready to start. Identity, not the CkLocRec pointer: the record
// can die between enqueue and handling. Used by both the send-completion
// release path and the AtSyncWait park.
struct DeferredMigrateMsg
{
  char header[CmiMsgHeaderSizeBytes];
  CkGroupID mgrGid;
  CmiUInt8 id;
  int toPe;
};

int _deferredMigrateHandlerIdx;
extern "C" void _deferredMigrateHandler(void* arg);
// A kick -- "move this element now" -- is a plain FIFO push, so it runs after
// the work already queued on this PE. It is a message, not an inline call: the
// count reaches zero inside the element's own entry, inside HAPI's event poll,
// or inside the IPC pump, none of which may have the element deleted under it.
//
// An urgent (queue-jumping) kick was tried and reverted. It does land sooner --
// first-kick success 28-46% -> near one, payload p50 0.5 s -> prompt -- but the
// element then migrates ahead of the messages already queued for it, and every
// one of those must be parked and forwarded to the new home. Measured on leanmd
// (2026-09-15, 5 runs each, with all three LB-step fixes in): urgent 123.6 s
// mean (117.7-127.7), FIFO 119.1 s (115.2-123.1), against sync 121.4 s. FIFO is
// the only async arm that beats sync, so the kick waits its turn.
// urgent: the count reached zero by a channel's closing ack (Frontier 2026-10-22). The owner's senders are
// held from the closing to the new resolution, and the FIFO kick waited ~40 ms behind the PE's queue while
// the acks took 3 ms -- the whole of the stand-down. The zero-copy path keeps the FIFO kick (above).
static void ckPostKick(DeferredMigrateMsg* m, bool urgent = false)
{
  CmiSetHandler(m, _deferredMigrateHandlerIdx);
  if (urgent) CmiPushPEUrgent(CmiMyRank(), m);
  else CmiPushPE(CmiMyRank(), m);
}

unsigned char& CkArrayMessage::array_hops(void)
{
  return UsrToEnv((void*)this)->getsetArrayHops();
}
unsigned int CkArrayMessage::array_getSrcPe(void)
{
  return UsrToEnv((void*)this)->getsetArraySrcPe();
}
unsigned int CkArrayMessage::array_ifNotThere(void)
{
  return UsrToEnv((void*)this)->getArrayIfNotThere();
}
void CkArrayMessage::array_setIfNotThere(unsigned int i)
{
  UsrToEnv((void*)this)->setArrayIfNotThere(i);
}

/*********************** Array Map ******************
Given an array element index, an array map tells us
the index's "home" Pe.  This is the Pe the element will
be created on, and also where messages to this element will
be forwarded by default.
*/

CkArrayMap::CkArrayMap(void) {}
CkArrayMap::~CkArrayMap() {}
int CkArrayMap::registerArray(const CkArrayIndex& numElements, CkArrayID aid)
{
  return 0;
}
void CkArrayMap::unregisterArray(int idx) {}

#define CKARRAYMAP_POPULATE_INITIAL(POPULATE_CONDITION)                                  \
  int i;                                                                                 \
  int index[6];                                                                          \
  int start_data[6], end_data[6], step_data[6];                                          \
  for (int d = 0; d < 6; d++)                                                            \
  {                                                                                      \
    start_data[d] = 0;                                                                   \
    end_data[d] = step_data[d] = 1;                                                      \
    if (end.dimension >= 4 && d < end.dimension)                                         \
    {                                                                                    \
      start_data[d] = ((short int*)start.data())[d];                                     \
      end_data[d] = ((short int*)end.data())[d];                                         \
      step_data[d] = ((short int*)step.data())[d];                                       \
    }                                                                                    \
    else if (d < end.dimension)                                                          \
    {                                                                                    \
      start_data[d] = start.data()[d];                                                   \
      end_data[d] = end.data()[d];                                                       \
      step_data[d] = step.data()[d];                                                     \
    }                                                                                    \
  }                                                                                      \
                                                                                         \
  for (index[0] = start_data[0]; index[0] < end_data[0]; index[0] += step_data[0])       \
  {                                                                                      \
    for (index[1] = start_data[1]; index[1] < end_data[1]; index[1] += step_data[1])     \
    {                                                                                    \
      for (index[2] = start_data[2]; index[2] < end_data[2]; index[2] += step_data[2])   \
      {                                                                                  \
        for (index[3] = start_data[3]; index[3] < end_data[3]; index[3] += step_data[3]) \
        {                                                                                \
          for (index[4] = start_data[4]; index[4] < end_data[4];                         \
               index[4] += step_data[4])                                                 \
          {                                                                              \
            for (index[5] = start_data[5]; index[5] < end_data[5];                       \
                 index[5] += step_data[5])                                               \
            {                                                                            \
              if (end.dimension == 1)                                                    \
              {                                                                          \
                i = index[0];                                                            \
                CkArrayIndex1D idx(index[0]);                                            \
                if (POPULATE_CONDITION)                                                  \
                {                                                                        \
                  mgr->insertInitial(idx, CkCopyMsg(&ctorMsg));                          \
                }                                                                        \
              }                                                                          \
              else if (end.dimension == 2)                                               \
              {                                                                          \
                i = index[0] * end_data[1] + index[1];                                   \
                CkArrayIndex2D idx(index[0], index[1]);                                  \
                if (POPULATE_CONDITION)                                                  \
                {                                                                        \
                  mgr->insertInitial(idx, CkCopyMsg(&ctorMsg));                          \
                }                                                                        \
              }                                                                          \
              else if (end.dimension == 3)                                               \
              {                                                                          \
                i = (index[0] * end_data[1] + index[1]) * end_data[2] + index[2];        \
                CkArrayIndex3D idx(index[0], index[1], index[2]);                        \
                if (POPULATE_CONDITION)                                                  \
                {                                                                        \
                  mgr->insertInitial(idx, CkCopyMsg(&ctorMsg));                          \
                }                                                                        \
              }                                                                          \
              else if (end.dimension == 4)                                               \
              {                                                                          \
                i = ((index[0] * end_data[1] + index[1]) * end_data[2] + index[2]) *     \
                        end_data[3] +                                                    \
                    index[3];                                                            \
                CkArrayIndex4D idx(index[0], index[1], index[2], index[3]);              \
                if (POPULATE_CONDITION)                                                  \
                {                                                                        \
                  mgr->insertInitial(idx, CkCopyMsg(&ctorMsg));                          \
                }                                                                        \
              }                                                                          \
              else if (end.dimension == 5)                                               \
              {                                                                          \
                i = (((index[0] * end_data[1] + index[1]) * end_data[2] + index[2]) *    \
                         end_data[3] +                                                   \
                     index[3]) *                                                         \
                        end_data[4] +                                                    \
                    index[4];                                                            \
                CkArrayIndex5D idx(index[0], index[1], index[2], index[3], index[4]);    \
                if (POPULATE_CONDITION)                                                  \
                {                                                                        \
                  mgr->insertInitial(idx, CkCopyMsg(&ctorMsg));                          \
                }                                                                        \
              }                                                                          \
              else if (end.dimension == 6)                                               \
              {                                                                          \
                i = ((((index[0] * end_data[1] + index[1]) * end_data[2] + index[2]) *   \
                          end_data[3] +                                                  \
                      index[3]) *                                                        \
                         end_data[4] +                                                   \
                     index[4]) *                                                         \
                        end_data[5] +                                                    \
                    index[5];                                                            \
                CkArrayIndex6D idx(index[0], index[1], index[2], index[3], index[4],     \
                                   index[5]);                                            \
                if (POPULATE_CONDITION)                                                  \
                {                                                                        \
                  mgr->insertInitial(idx, CkCopyMsg(&ctorMsg));                          \
                }                                                                        \
              }                                                                          \
            }                                                                            \
          }                                                                              \
        }                                                                                \
      }                                                                                  \
    }                                                                                    \
  }

void CkArrayMap::populateInitial(int arrayHdl, CkArrayOptions& options, void* ctorMsg,
                                 CkArray* mgr)
{
  CkArrayIndex start = options.getStart();
  CkArrayIndex end = options.getEnd();
  CkArrayIndex step = options.getStep();
  if (end.nInts == 0)
  {
    CkFreeMsg(ctorMsg);
    return;
  }
  int thisPe = CkMyPe();
  /* The CkArrayIndex is supposed to have at most 3 dimensions, which
     means that all the fields are ints, and numElements.nInts represents
     how many of them are used */
  CKARRAYMAP_POPULATE_INITIAL(CMK_RANK_0(procNum(arrayHdl, idx)) == thisPe);

  CkFreeMsg(ctorMsg);
}

void CkArrayMap::storeCkArrayOpts(CkArrayOptions options)
{
  // options will not be used on demand_creation arrays
  storeOpts = options;
}

void CkArrayMap::pup(PUP::er& p)
{
  p | storeOpts;
  p | dynamicIns;
}

CkGroupID _defaultArrayMapID;
CkGroupID _fastArrayMapID;

class RRMap : public CkArrayMap
{
private:
  CkArrayIndex maxIndex;
  uint64_t products[2 * CK_ARRAYINDEX_MAXLEN];
  bool productsInit;

public:
  RRMap(void)
  {
    DEBC((AA "Creating RRMap\n" AB));
    productsInit = false;
  }
  RRMap(CkMigrateMessage* m) : CkArrayMap(m) {}

  void indexInit()
  {
    productsInit = true;
    maxIndex = storeOpts.getEnd();
    products[maxIndex.dimension - 1] = 1;
    if (maxIndex.dimension <= CK_ARRAYINDEX_MAXLEN)
    {
      for (int dim = maxIndex.dimension - 2; dim >= 0; dim--)
      {
        products[dim] = products[dim + 1] * maxIndex.index[dim + 1];
      }
    }
    else
    {
      for (int dim = maxIndex.dimension - 2; dim >= 0; dim--)
      {
        products[dim] = products[dim + 1] * maxIndex.indexShorts[dim + 1];
      }
    }
  }  // End of indexInit

  int procNum(int arrayHdl, const CkArrayIndex& i)
  {
    if (i.dimension == 1)
    {
      // Map 1D integer indices in simple round-robin fashion
      int ans = (i.data()[0]) % CkNumPes();
      return ans;
    }
    else
    {
      if (dynamicIns.find(arrayHdl) != dynamicIns.end())
      {
        // Finding indicates that current array uses dynamic insertion
        // Map other indices based on their hash code, mod a big prime.
        unsigned int hash = (i.hash() + 739) % 1280107;
        int ans = (hash % CkNumPes());
        return ans;
      }
      else
      {
        if (!productsInit)
        {
          indexInit();
        }

        int indexOffset = 0;
        if (i.dimension <= CK_ARRAYINDEX_MAXLEN)
        {
          for (int dim = i.dimension - 1; dim >= 0; dim--)
          {
            indexOffset += (i.index[dim] * products[dim]);
          }
        }
        else
        {
          for (int dim = maxIndex.dimension - 1; dim >= 0; dim--)
          {
            indexOffset += (i.indexShorts[dim] * products[dim]);
          }
        }
        return indexOffset % CkNumPes();
      }
    }
  }

  void pup(PUP::er& p)
  {
    CkArrayMap::pup(p);
    p | maxIndex;
    p | productsInit;
    PUParray(p, products, 2 * CK_ARRAYINDEX_MAXLEN);
  }
};

/**
 * Class used to store the dimensions of the array and precalculate numChares,
 * binSize and other values for the DefaultArrayMap -- ASB
 */
class arrayMapInfo
{
public:
  CkArrayIndex _nelems;
  int _binSizeFloor; /* floor of numChares/numPes */
  int _binSizeCeil;  /* ceiling of numChares/numPes */
  int _numChares;    /* initial total number of chares */
  int _remChares;    /* numChares % numPes -- equals the number of
                        processors in the first set */
  int _numFirstSet;  /* _remChares X (_binSize + 1) -- number of
                        chares in the first set */

  int _nBinSizeFloor; /* floor of numChares/numNodes */
  int _nRemChares;    /* numChares % numNodes -- equals the number of
                         nodes in the first set */
  int _nNumFirstSet;  /* _remChares X (_binSize + 1) -- number of
                         chares in the first set of nodes */

  /** All processors are divided into two sets. Processors in the first set
   *  have one chare more than the processors in the second set. */

  arrayMapInfo(void) {}

  arrayMapInfo(const CkArrayIndex& n) : _nelems(n), _numChares(0) { compute_binsize(); }

  ~arrayMapInfo() {}

  void compute_binsize()
  {
    int numPes = CkNumPes();
    // Now assuming homogenous nodes where each node has the same number of PEs
    int numNodes = CkNumNodes();

    if (_nelems.dimension == 1)
    {
      _numChares = _nelems.data()[0];
    }
    else if (_nelems.dimension == 2)
    {
      _numChares = _nelems.data()[0] * _nelems.data()[1];
    }
    else if (_nelems.dimension == 3)
    {
      _numChares = _nelems.data()[0] * _nelems.data()[1] * _nelems.data()[2];
    }
    else if (_nelems.dimension == 4)
    {
      _numChares =
          (int)(((short int*)_nelems.data())[0] * ((short int*)_nelems.data())[1] *
                ((short int*)_nelems.data())[2] * ((short int*)_nelems.data())[3]);
    }
    else if (_nelems.dimension == 5)
    {
      _numChares =
          (int)(((short int*)_nelems.data())[0] * ((short int*)_nelems.data())[1] *
                ((short int*)_nelems.data())[2] * ((short int*)_nelems.data())[3] *
                ((short int*)_nelems.data())[4]);
    }
    else if (_nelems.dimension == 6)
    {
      _numChares =
          (int)(((short int*)_nelems.data())[0] * ((short int*)_nelems.data())[1] *
                ((short int*)_nelems.data())[2] * ((short int*)_nelems.data())[3] *
                ((short int*)_nelems.data())[4] * ((short int*)_nelems.data())[5]);
    }

    _remChares = _numChares % numPes;
    _binSizeFloor = (int)floor((double)_numChares / (double)numPes);
    _binSizeCeil = (int)ceil((double)_numChares / (double)numPes);
    _numFirstSet = _remChares * (_binSizeFloor + 1);

    _nRemChares = _numChares % numNodes;
    _nBinSizeFloor = _numChares / numNodes;
    _nNumFirstSet = _nRemChares * (_nBinSizeFloor + 1);
  }

  void pup(PUP::er& p)
  {
    p | _nelems;
    p | _binSizeFloor;
    p | _binSizeCeil;
    p | _numChares;
    p | _remChares;
    p | _numFirstSet;
    p | _nBinSizeFloor;
    p | _nRemChares;
    p | _nNumFirstSet;
  }
};

/**
 * The default map object -- This does blocked mapping in the general case and
 * calls the round-robin procNum for the dynamic insertion case -- ASB
 */
class DefaultArrayMap : public RRMap
{
public:
  /** This array stores information about different chare arrays in a Charm
   *  program (dimensions, binsize, numChares etc ... ) */
  CkPupPtrVec<arrayMapInfo> amaps;

public:
  DefaultArrayMap(void) { DEBC((AA "Creating DefaultArrayMap\n" AB)); }

  DefaultArrayMap(CkMigrateMessage* m) : RRMap(m) {}

  int registerArray(const CkArrayIndex& numElements, CkArrayID aid)
  {
    int idx = amaps.size();
    amaps.resize(idx + 1);
    amaps[idx] = new arrayMapInfo(numElements);
    return idx;
  }

  void unregisterArray(int idx)
  {
    delete amaps[idx];
    amaps[idx] = NULL;
  }

  int procNum(int arrayHdl, const CkArrayIndex& i)
  {
    int flati;
    auto& info = amaps[arrayHdl];
    if (info->_nelems.dimension == 0)
    {
      dynamicIns[arrayHdl] = true;
      return RRMap::procNum(arrayHdl, i);
    }

    if (i.dimension == 1)
    {
      flati = i.data()[0];
    }
    else if (i.dimension == 2)
    {
      flati = i.data()[0] * info->_nelems.data()[1] + i.data()[1];
    }
    else if (i.dimension == 3)
    {
      flati = (i.data()[0] * info->_nelems.data()[1] + i.data()[1]) *
                  info->_nelems.data()[2] +
              i.data()[2];
    }
    else if (i.dimension == 4)
    {
      flati = (int)(((((short int*)i.data())[0] *
                          ((short int*)info->_nelems.data())[1] +
                      ((short int*)i.data())[1]) *
                         ((short int*)info->_nelems.data())[2] +
                     ((short int*)i.data())[2]) *
                        ((short int*)info->_nelems.data())[3] +
                    ((short int*)i.data())[3]);
    }
    else if (i.dimension == 5)
    {
      flati = (int)((((((short int*)i.data())[0] *
                           ((short int*)info->_nelems.data())[1] +
                       ((short int*)i.data())[1]) *
                          ((short int*)info->_nelems.data())[2] +
                      ((short int*)i.data())[2]) *
                         ((short int*)info->_nelems.data())[3] +
                     ((short int*)i.data())[3]) *
                        ((short int*)info->_nelems.data())[4] +
                    ((short int*)i.data())[4]);
    }
    else if (i.dimension == 6)
    {
      flati = (int)(((((((short int*)i.data())[0] *
                            ((short int*)info->_nelems.data())[1] +
                        ((short int*)i.data())[1]) *
                           ((short int*)info->_nelems.data())[2] +
                       ((short int*)i.data())[2]) *
                          ((short int*)info->_nelems.data())[3] +
                      ((short int*)i.data())[3]) *
                         ((short int*)info->_nelems.data())[4] +
                     ((short int*)i.data())[4]) *
                        ((short int*)info->_nelems.data())[5] +
                    ((short int*)i.data())[5]);
    }
#if CMK_ERROR_CHECKING
    else
    {
      CkAbort("CkArrayIndex has more than 6 dimensions!");
    }
#endif

    if (useNodeBlkMapping)
    {
      if (flati < info->_numChares)
      {
        int numCharesOnNode = info->_nBinSizeFloor;
        int startNodeID, offsetInNode;
        if (flati < info->_nNumFirstSet)
        {
          numCharesOnNode++;
          startNodeID = flati / numCharesOnNode;
          offsetInNode = flati % numCharesOnNode;
        }
        else
        {
          startNodeID = info->_nRemChares +
                        (flati - info->_nNumFirstSet) / numCharesOnNode;
          offsetInNode = (flati - info->_nNumFirstSet) % numCharesOnNode;
        }
        int nodeSize = CkMyNodeSize();  // assuming every node has same number of PEs
        int elemsPerPE = numCharesOnNode / nodeSize;
        int remElems = numCharesOnNode % nodeSize;
        int firstSetPEs = remElems * (elemsPerPE + 1);
        if (offsetInNode < firstSetPEs)
        {
          return CkNodeFirst(startNodeID) + offsetInNode / (elemsPerPE + 1);
        }
        else
        {
          return CkNodeFirst(startNodeID) + remElems +
                 (offsetInNode - firstSetPEs) / elemsPerPE;
        }
      }
      else
        return (flati % CkNumPes());
    }
    // regular PE-based block mapping
    if (flati < info->_numFirstSet)
      return (flati / (info->_binSizeFloor + 1));
    else if (flati < info->_numChares)
      return (info->_remChares +
              (flati - info->_numFirstSet) / (info->_binSizeFloor));
    else
      return (flati % CkNumPes());
  }

  void pup(PUP::er& p)
  {
    RRMap::pup(p);
    int npes = CkNumPes();
    p | npes;
    p | amaps;
    if (p.isUnpacking() && npes != CkNumPes())
    {  // binSize needs update
      for (int i = 0; i < amaps.size(); i++)
        if (amaps[i])
          amaps[i]->compute_binsize();
    }
  }
};

/**
 *  A fast map for chare arrays which do static insertions and promise NOT
 *  to do late insertions -- ASB
 */
class FastArrayMap : public DefaultArrayMap
{
public:
  FastArrayMap(void) { DEBC((AA "Creating FastArrayMap\n" AB)); }

  FastArrayMap(CkMigrateMessage* m) : DefaultArrayMap(m) {}

  int registerArray(const CkArrayIndex& numElements, CkArrayID aid)
  {
    int idx;
    idx = DefaultArrayMap::registerArray(numElements, aid);

    return idx;
  }

  int procNum(int arrayHdl, const CkArrayIndex& i)
  {
    int flati = 0;
    if (amaps[arrayHdl]->_nelems.dimension == 0)
    {
      return RRMap::procNum(arrayHdl, i);
    }

    if (i.dimension == 1)
    {
      flati = i.data()[0];
    }
    else if (i.dimension == 2)
    {
      flati = i.data()[0] * amaps[arrayHdl]->_nelems.data()[1] + i.data()[1];
    }
    else if (i.dimension == 3)
    {
      flati = (i.data()[0] * amaps[arrayHdl]->_nelems.data()[1] + i.data()[1]) *
                  amaps[arrayHdl]->_nelems.data()[2] +
              i.data()[2];
    }
    else if (i.dimension == 4)
    {
      flati = (int)(((((short int*)i.data())[0] *
                          ((short int*)amaps[arrayHdl]->_nelems.data())[1] +
                      ((short int*)i.data())[1]) *
                         ((short int*)amaps[arrayHdl]->_nelems.data())[2] +
                     ((short int*)i.data())[2]) *
                        ((short int*)amaps[arrayHdl]->_nelems.data())[3] +
                    ((short int*)i.data())[3]);
    }
    else if (i.dimension == 5)
    {
      flati = (int)((((((short int*)i.data())[0] *
                           ((short int*)amaps[arrayHdl]->_nelems.data())[1] +
                       ((short int*)i.data())[1]) *
                          ((short int*)amaps[arrayHdl]->_nelems.data())[2] +
                      ((short int*)i.data())[2]) *
                         ((short int*)amaps[arrayHdl]->_nelems.data())[3] +
                     ((short int*)i.data())[3]) *
                        ((short int*)amaps[arrayHdl]->_nelems.data())[4] +
                    ((short int*)i.data())[4]);
    }
    else if (i.dimension == 6)
    {
      flati = (int)(((((((short int*)i.data())[0] *
                            ((short int*)amaps[arrayHdl]->_nelems.data())[1] +
                        ((short int*)i.data())[1]) *
                           ((short int*)amaps[arrayHdl]->_nelems.data())[2] +
                       ((short int*)i.data())[2]) *
                          ((short int*)amaps[arrayHdl]->_nelems.data())[3] +
                      ((short int*)i.data())[3]) *
                         ((short int*)amaps[arrayHdl]->_nelems.data())[4] +
                     ((short int*)i.data())[4]) *
                        ((short int*)amaps[arrayHdl]->_nelems.data())[5] +
                    ((short int*)i.data())[5]);
    }
#if CMK_ERROR_CHECKING
    else
    {
      CkAbort("CkArrayIndex has more than 6 dimensions!");
    }
#endif

    /** binSize used in DefaultArrayMap is the floor of numChares/numPes
     *  but for this FastArrayMap, we need the ceiling */
    return (flati / amaps[arrayHdl]->_binSizeCeil);
  }

  void pup(PUP::er& p) { DefaultArrayMap::pup(p); }
};

/* *
 * Hilbert map object -- This does hilbert mapping.
 * Convert array indices into 1D fashion according to their Hilbert filling curve
 */

typedef struct
{
  int intIndex;
  std::vector<int> coords;
} hilbert_pair;

bool operator==(hilbert_pair p1, hilbert_pair p2) { return p1.intIndex == p2.intIndex; }

bool myCompare(hilbert_pair p1, hilbert_pair p2) { return p1.intIndex < p2.intIndex; }

class HilbertArrayMap : public DefaultArrayMap
{
  std::vector<int> allpairs;
  std::vector<int> procList;

public:
  HilbertArrayMap(void)
  {
    procList.resize(CkNumPes());
    getHilbertList(procList.data());
    DEBC((AA "Creating HilbertArrayMap\n" AB));
  }

  HilbertArrayMap(CkMigrateMessage* m) : DefaultArrayMap(m) {}

  ~HilbertArrayMap() {}

  int registerArray(const CkArrayIndex& i, CkArrayID aid)
  {
    int idx;
    idx = DefaultArrayMap::registerArray(i, aid);

    if (i.dimension == 1)
    {
      // CkPrintf("1D %d\n", amaps[idx]->_nelems.data()[0]);
    }
    else if (i.dimension == 2)
    {
      // CkPrintf("2D %d:%d\n", amaps[idx]->_nelems.data()[0],
      // amaps[idx]->_nelems.data()[1]);
      const int dims = 2;
      int nDim0 = amaps[idx]->_nelems.data()[0];
      int nDim1 = amaps[idx]->_nelems.data()[1];
      int index;
      int counter = 0;
      std::vector<int> coords;
      allpairs.resize((size_t)nDim0 * nDim1);
      coords.resize(dims);
      for (int i = 0; i < nDim0; i++)
        for (int j = 0; j < nDim1; j++)
        {
          coords[0] = i;
          coords[1] = j;
          index = Hilbert_to_int(coords, dims);
          // CkPrintf("(%d:%d)----------> %d \n", i, j, index);
          allpairs[counter] = index;
          counter++;
        }
    }
    else if (i.dimension == 3)
    {
      // CkPrintf("3D %d:%d:%d\n", amaps[idx]->_nelems.data()[0],
      // amaps[idx]->_nelems.data()[1],
      //        amaps[idx]->_nelems.data()[2]);
      const int dims = 3;
      int nDim0 = amaps[idx]->_nelems.data()[0];
      int nDim1 = amaps[idx]->_nelems.data()[1];
      int nDim2 = amaps[idx]->_nelems.data()[2];
      int index;
      int counter = 0;
      std::vector<int> coords;
      allpairs.resize((size_t)nDim0 * nDim1 * nDim2);
      coords.resize(dims);
      for (int i = 0; i < nDim0; i++)
        for (int j = 0; j < nDim1; j++)
          for (int k = 0; k < nDim2; k++)
          {
            coords[0] = i;
            coords[1] = j;
            coords[2] = k;
            index = Hilbert_to_int(coords, dims);
            allpairs[counter] = index;
            counter++;
          }
    }
    else if (i.dimension == 4)
    {
      // CkPrintf("4D %hd:%hd:%hd:%hd\n", ((short int*)amaps[idx]->_nelems.data())[0],
      //        ((short int*)amaps[idx]->_nelems.data())[1], ((short
      //        int*)amaps[idx]->_nelems.data())[2],
      //        ((short int*)amaps[idx]->_nelems.data())[3]);
      const int dims = 4;
      int nDim[dims];
      for (int k = 0; k < dims; k++)
      {
        nDim[k] = (int)((short int*)amaps[idx]->_nelems.data())[k];
      }
      int index;
      int counter = 0;
      std::vector<int> coords;
      allpairs.resize((size_t)nDim[0] * nDim[1] * nDim[2] * nDim[3]);
      coords.resize(dims);
      for (int i = 0; i < nDim[0]; i++)
        for (int j = 0; j < nDim[1]; j++)
          for (int k = 0; k < nDim[2]; k++)
            for (int x = 0; x < nDim[3]; x++)
            {
              coords[0] = i;
              coords[1] = j;
              coords[2] = k;
              coords[3] = x;
              index = Hilbert_to_int(coords, dims);
              allpairs[counter] = index;
              counter++;
            }
    }
    else if (i.dimension == 5)
    {
      // CkPrintf("5D %hd:%hd:%hd:%hd:%hd\n", ((short int*)amaps[idx]->_nelems.data())[0],
      //        ((short int*)amaps[idx]->_nelems.data())[1], ((short
      //        int*)amaps[idx]->_nelems.data())[2],
      //        ((short int*)amaps[idx]->_nelems.data())[3], ((short
      //        int*)amaps[idx]->_nelems.data())[4]);
      const int dims = 5;
      int nDim[dims];
      for (int k = 0; k < dims; k++)
      {
        nDim[k] = (int)((short int*)amaps[idx]->_nelems.data())[k];
      }
      int index;
      int counter = 0;
      std::vector<int> coords;
      allpairs.resize((size_t)nDim[0] * nDim[1] * nDim[2] * nDim[3] * nDim[4]);
      coords.resize(dims);
      for (int i = 0; i < nDim[0]; i++)
        for (int j = 0; j < nDim[1]; j++)
          for (int k = 0; k < nDim[2]; k++)
            for (int x = 0; x < nDim[3]; x++)
              for (int y = 0; y < nDim[4]; y++)
              {
                coords[0] = i;
                coords[1] = j;
                coords[2] = k;
                coords[3] = x;
                coords[4] = y;
                index = Hilbert_to_int(coords, dims);
                allpairs[counter] = index;
                counter++;
              }
    }
    else if (i.dimension == 6)
    {
      // CkPrintf("6D %hd:%hd:%hd:%hd:%hd:%hd\n", ((short
      // int*)amaps[idx]->_nelems.data())[0],
      //        ((short int*)amaps[idx]->_nelems.data())[1], ((short
      //        int*)amaps[idx]->_nelems.data())[2],
      //        ((short int*)amaps[idx]->_nelems.data())[3], ((short
      //        int*)amaps[idx]->_nelems.data())[4],
      //        ((short int*)amaps[idx]->_nelems.data())[5]);
      const int dims = 6;
      int nDim[dims];
      for (int k = 0; k < dims; k++)
      {
        nDim[k] = (int)((short int*)amaps[idx]->_nelems.data())[k];
      }
      int index;
      int counter = 0;
      std::vector<int> coords;
      allpairs.resize((size_t)nDim[0] * nDim[1] * nDim[2] * nDim[3] * nDim[4] * nDim[5]);
      coords.resize(dims);
      for (int i = 0; i < nDim[0]; i++)
        for (int j = 0; j < nDim[1]; j++)
          for (int k = 0; k < nDim[2]; k++)
            for (int x = 0; x < nDim[3]; x++)
              for (int y = 0; y < nDim[4]; y++)
                for (int z = 0; z < nDim[5]; z++)
                {
                  coords[0] = i;
                  coords[1] = j;
                  coords[2] = k;
                  coords[3] = x;
                  coords[4] = y;
                  coords[5] = y;
                  index = Hilbert_to_int(coords, dims);
                  allpairs[counter] = index;
                  counter++;
                }
    }
    return idx;
  }

  int procNum(int arrayHdl, const CkArrayIndex& i)
  {
    int flati = 0;
    int myInt;
    if (amaps[arrayHdl]->_nelems.dimension == 0)
    {
      return RRMap::procNum(arrayHdl, i);
    }
    if (i.dimension == 1)
    {
      flati = i.data()[0];
    }
    else if (i.dimension == 2)
    {
      int nDim1 = amaps[arrayHdl]->_nelems.data()[1];
      myInt = i.data()[0] * nDim1 + i.data()[1];
      flati = allpairs[myInt];
    }
    else if (i.dimension == 3)
    {
      hilbert_pair mypair;
      mypair.coords.resize(3);
      int nDim[2];
      for (int j = 0; j < 2; j++)
      {
        nDim[j] = amaps[arrayHdl]->_nelems.data()[j + 1];
      }
      myInt = i.data()[0] * nDim[0] * nDim[1] + i.data()[1] * nDim[1] + i.data()[2];
      flati = allpairs[myInt];
    }
    else if (i.dimension == 4)
    {
      hilbert_pair mypair;
      mypair.coords.resize(4);
      short int nDim[3];
      for (int j = 0; j < 3; j++)
      {
        nDim[j] = ((short int*)amaps[arrayHdl]->_nelems.data())[j + 1];
      }
      myInt = (int)(((short int*)i.data())[0] * nDim[0] * nDim[1] * nDim[2] +
                    ((short int*)i.data())[1] * nDim[1] * nDim[2] +
                    ((short int*)i.data())[2] * nDim[2] + ((short int*)i.data())[3]);
      flati = allpairs[myInt];
    }
    else if (i.dimension == 5)
    {
      hilbert_pair mypair;
      mypair.coords.resize(5);
      short int nDim[4];
      for (int j = 0; j < 4; j++)
      {
        nDim[j] = ((short int*)amaps[arrayHdl]->_nelems.data())[j + 1];
      }
      myInt = (int)(((short int*)i.data())[0] * nDim[0] * nDim[1] * nDim[2] * nDim[3] +
                    ((short int*)i.data())[1] * nDim[1] * nDim[2] * nDim[3] +
                    ((short int*)i.data())[2] * nDim[2] * nDim[3] +
                    ((short int*)i.data())[3] * nDim[3] + ((short int*)i.data())[4]);
      flati = allpairs[myInt];
    }
    else if (i.dimension == 6)
    {
      hilbert_pair mypair;
      mypair.coords.resize(6);
      short int nDim[5];
      for (int j = 0; j < 5; j++)
      {
        nDim[j] = ((short int*)amaps[arrayHdl]->_nelems.data())[j + 1];
      }
      myInt = (int)(((short int*)i.data())[0] * nDim[0] * nDim[1] * nDim[2] * nDim[3] *
                        nDim[4] +
                    ((short int*)i.data())[1] * nDim[1] * nDim[2] * nDim[3] * nDim[4] +
                    ((short int*)i.data())[2] * nDim[2] * nDim[3] * nDim[4] +
                    ((short int*)i.data())[3] * nDim[3] * nDim[4] +
                    ((short int*)i.data())[4] * nDim[4] + ((short int*)i.data())[5]);
      flati = allpairs[myInt];
    }
#if CMK_ERROR_CHECKING
    else
    {
      CkAbort("CkArrayIndex has more than 6 dimensions!");
    }
#endif

    /** binSize used in DefaultArrayMap is the floor of numChares/numPes
     *  but for this FastArrayMap, we need the ceiling */
    int block = flati / amaps[arrayHdl]->_binSizeCeil;
    // for(int i=0; i<CkNumPes(); i++)
    //    CkPrintf("(%d:%d) ", i, procList[i]);
    // CkPrintf("\n");
    // CkPrintf("block [%d:%d]\n", block, procList[block]);
    return procList[block];
  }

  void pup(PUP::er& p) { DefaultArrayMap::pup(p); }
};

/**
 * This map can be used for topology aware mapping when the mapping is provided
 * through a file -- ASB
 */
class ReadFileMap : public DefaultArrayMap
{
private:
  std::vector<int> mapping;

public:
  ReadFileMap(void) { DEBC((AA "Creating ReadFileMap\n" AB)); }

  ReadFileMap(CkMigrateMessage* m) : DefaultArrayMap(m) {}

  int registerArray(const CkArrayIndex& numElements, CkArrayID aid)
  {
    int idx;
    idx = DefaultArrayMap::registerArray(numElements, aid);

    if (mapping.size() == 0)
    {
      int numChares;

      if (amaps[idx]->_nelems.dimension == 1)
      {
        numChares = amaps[idx]->_nelems.data()[0];
      }
      else if (amaps[idx]->_nelems.dimension == 2)
      {
        numChares = amaps[idx]->_nelems.data()[0] * amaps[idx]->_nelems.data()[1];
      }
      else if (amaps[idx]->_nelems.dimension == 3)
      {
        numChares = amaps[idx]->_nelems.data()[0] * amaps[idx]->_nelems.data()[1] *
                    amaps[idx]->_nelems.data()[2];
      }
      else if (amaps[idx]->_nelems.dimension == 4)
      {
        numChares = (int)(((short int*)amaps[idx]->_nelems.data())[0] *
                          ((short int*)amaps[idx]->_nelems.data())[1] *
                          ((short int*)amaps[idx]->_nelems.data())[2] *
                          ((short int*)amaps[idx]->_nelems.data())[3]);
      }
      else if (amaps[idx]->_nelems.dimension == 5)
      {
        numChares = (int)(((short int*)amaps[idx]->_nelems.data())[0] *
                          ((short int*)amaps[idx]->_nelems.data())[1] *
                          ((short int*)amaps[idx]->_nelems.data())[2] *
                          ((short int*)amaps[idx]->_nelems.data())[3] *
                          ((short int*)amaps[idx]->_nelems.data())[4]);
      }
      else if (amaps[idx]->_nelems.dimension == 6)
      {
        numChares = (int)(((short int*)amaps[idx]->_nelems.data())[0] *
                          ((short int*)amaps[idx]->_nelems.data())[1] *
                          ((short int*)amaps[idx]->_nelems.data())[2] *
                          ((short int*)amaps[idx]->_nelems.data())[3] *
                          ((short int*)amaps[idx]->_nelems.data())[4] *
                          ((short int*)amaps[idx]->_nelems.data())[5]);
      }
      else
      {
        CkAbort("CkArrayIndex has more than 6 dimension!");
      }

      mapping.resize(numChares);
      FILE* mapf = fopen("mapfile", "r");
      TopoManager tmgr;
      int x, y, z, t;

      for (int i = 0; i < numChares; i++)
      {
        if (fscanf(mapf, "%d %d %d %d", &x, &y, &z, &t) != 4)
        {
          CkAbort("ReadFileMap> reading from mapfile failed!");
        }
        mapping[i] = tmgr.coordinatesToRank(x, y, z, t);
      }
      fclose(mapf);
    }

    return idx;
  }

  int procNum(int arrayHdl, const CkArrayIndex& i)
  {
    int flati;

    if (i.dimension == 1)
    {
      flati = i.data()[0];
    }
    else if (i.dimension == 2)
    {
      flati = i.data()[0] * amaps[arrayHdl]->_nelems.data()[1] + i.data()[1];
    }
    else if (i.dimension == 3)
    {
      flati = (i.data()[0] * amaps[arrayHdl]->_nelems.data()[1] + i.data()[1]) *
                  amaps[arrayHdl]->_nelems.data()[2] +
              i.data()[2];
    }
    else if (i.dimension == 4)
    {
      flati = (int)(((((short int*)i.data())[0] *
                          ((short int*)amaps[arrayHdl]->_nelems.data())[1] +
                      ((short int*)i.data())[1]) *
                         ((short int*)amaps[arrayHdl]->_nelems.data())[2] +
                     ((short int*)i.data())[2]) *
                        ((short int*)amaps[arrayHdl]->_nelems.data())[3] +
                    ((short int*)i.data())[3]);
    }
    else if (i.dimension == 5)
    {
      flati = (int)((((((short int*)i.data())[0] *
                           ((short int*)amaps[arrayHdl]->_nelems.data())[1] +
                       ((short int*)i.data())[1]) *
                          ((short int*)amaps[arrayHdl]->_nelems.data())[2] +
                      ((short int*)i.data())[2]) *
                         ((short int*)amaps[arrayHdl]->_nelems.data())[3] +
                     ((short int*)i.data())[3]) *
                        ((short int*)amaps[arrayHdl]->_nelems.data())[4] +
                    ((short int*)i.data())[4]);
    }
    else if (i.dimension == 6)
    {
      flati = (int)(((((((short int*)i.data())[0] *
                            ((short int*)amaps[arrayHdl]->_nelems.data())[1] +
                        ((short int*)i.data())[1]) *
                           ((short int*)amaps[arrayHdl]->_nelems.data())[2] +
                       ((short int*)i.data())[2]) *
                          ((short int*)amaps[arrayHdl]->_nelems.data())[3] +
                      ((short int*)i.data())[3]) *
                         ((short int*)amaps[arrayHdl]->_nelems.data())[4] +
                     ((short int*)i.data())[4]) *
                        ((short int*)amaps[arrayHdl]->_nelems.data())[5] +
                    ((short int*)i.data())[5]);
    }
    else
    {
      CkAbort("CkArrayIndex has more than 6 dimensions!");
    }

    return mapping[flati];
  }

  void pup(PUP::er& p)
  {
    DefaultArrayMap::pup(p);
    p | mapping;
  }
};

/**
 * This map can be used for a simple (non-topology-aware) mapping of a 1D array when the
 * mapping is provided through a file
 */
class Simple1DFileMap : public DefaultArrayMap
{
private:
  std::vector<int> mapping;

public:
  Simple1DFileMap(void) {
    DEBC((AA "Creating Simple1DFileMap\n" AB));
  }

  Simple1DFileMap(CkMigrateMessage *m) : DefaultArrayMap(m){}

  int registerArray(const CkArrayIndex& numElements, CkArrayID aid)
  {
    int idx = DefaultArrayMap::registerArray(numElements, aid);

    if(mapping.size() == 0) {
      int numChares;

      if (amaps[idx]->_nelems.dimension == 1) {
        numChares = amaps[idx]->_nelems.data()[0];
      } else {
        CkAbort("CkArrayIndex has more than 1 dimension for a Simple1DFileMap!");
      }

      mapping.resize(numChares);
      FILE *mapf = fopen("mapfile", "r");
      if (mapf == NULL) {
        CkAbort("Simple1DFileMap failed to open file named 'mapfile'!");
      }
      int pe;

      for(int i=0; i<numChares; i++) {
        if (fscanf(mapf, "%d\n", &pe) != 1) {
          CkAbort("Simple1DFileMap> reading from mapfile failed! Expected one int per line, one line per chare array element...");
        }
        mapping[i] = pe;
      }
      fclose(mapf);
    }

    return idx;
  }

  int procNum(int arrayHdl, const CkArrayIndex &i) {
    int flati;

    if (i.dimension == 1) {
      flati = i.data()[0];
    } else {
      CkAbort("CkArrayIndex has more than 1 dimension for a 1D map!");
    }

    return mapping[flati];
  }

  void pup(PUP::er& p){
    DefaultArrayMap::pup(p);
    p|mapping;
  }
};

// This is currently  here for backwards compatibility
class BlockMap : public DefaultArrayMap
{
public:
  BlockMap() {}
  BlockMap(CkMigrateMessage* m) : DefaultArrayMap(m) {}
};

/**
 * map object-- use seed load balancer.
 */
class CldMap : public CkArrayMap
{
public:
  CldMap(void) { DEBC((AA "Creating CldMap\n" AB)); }
  CldMap(CkMigrateMessage* m) : CkArrayMap(m) {}
  int homePe(int /*arrayHdl*/, const CkArrayIndex& i)
  {
    if (i.dimension == 1)
    {
      // Map 1D integer indices in simple round-robin fashion
      return (i.data()[0]) % CkNumPes();
    }
    else
    {
      // Map other indices based on their hash code, mod a big prime.
      unsigned int hash = (i.hash() + 739) % 1280107;
      return (hash % CkNumPes());
    }
  }
  int procNum(int arrayHdl, const CkArrayIndex& i)
  {
    return CLD_ANYWHERE;  // -1
  }
  void populateInitial(int arrayHdl, CkArrayOptions& options, void* ctorMsg, CkArray* mgr)
  {
    CkArrayIndex start = options.getStart();
    CkArrayIndex end = options.getEnd();
    CkArrayIndex step = options.getStep();
    if (end.dimension == 0)
    {
      CkFreeMsg(ctorMsg);
      return;
    }
    int thisPe = CkMyPe();
    int numPes = CkNumPes();

    CKARRAYMAP_POPULATE_INITIAL(i % numPes == thisPe);
    CkFreeMsg(ctorMsg);
  }
};

/// A class responsible for parsing the command line arguments for the PE
/// to extract the format string passed in with +ConfigurableRRMap
class ConfigurableRRMapLoader
{
public:
  std::vector<int> locations;
  int objs_per_block;
  int PE_per_block;

  /// labels for states used when parsing the ConfigurableRRMap from ARGV
  enum ConfigurableRRMapLoadStatus : uint8_t
  {
    not_loaded,
    loaded_found,
    loaded_not_found
  };

  enum ConfigurableRRMapLoadStatus state;

  ConfigurableRRMapLoader()
  {
    state = not_loaded;
    objs_per_block = 0;
    PE_per_block = 0;
  }

  /// load configuration if possible, and return whether a valid configuration exists
  bool haveConfiguration()
  {
    if (state == not_loaded)
    {
      DEBUG(("[%d] loading ConfigurableRRMap configuration\n", CkMyPe()));
      char** argv = CkGetArgv();
      char* configuration = NULL;
      bool found = CmiGetArgString(argv, "+ConfigurableRRMap", &configuration);
      if (!found)
      {
        DEBUG(("Couldn't find +ConfigurableRRMap command line argument\n"));
        state = loaded_not_found;
        return false;
      }
      else
      {
        DEBUG(("Found +ConfigurableRRMap command line argument in %p=\"%s\"\n",
               configuration, configuration));

        std::istringstream instream(configuration);
        CkAssert(instream.good());

        // Example line:
        // 10 8 0 1 2 3 4 5 6 7 7 7 7
        // Map 10 objects to 8 PEs, with each object's index among the 8 PEs.

        // extract first integer
        instream >> objs_per_block >> PE_per_block;
        CkAssert(instream.good());
        CkAssert(objs_per_block > 0);
        CkAssert(PE_per_block > 0);
        locations.resize(objs_per_block);
        for (int i = 0; i < objs_per_block; i++)
        {
          locations[i] = 0;
          CkAssert(instream.good());
          instream >> locations[i];
          CkAssert(locations[i] < PE_per_block);
        }
        state = loaded_found;
        return true;
      }
    }
    else
    {
      DEBUG(("[%d] ConfigurableRRMap has already been loaded\n", CkMyPe()));
      return state == loaded_found;
    }
  }
};

CkpvDeclare(ConfigurableRRMapLoader, myConfigRRMapState);

void _initConfigurableRRMap()
{
  CkpvInitialize(ConfigurableRRMapLoader, myConfigRRMapState);
}

/// Try to load the command line arguments for ConfigurableRRMap
bool haveConfigurableRRMap()
{
  DEBUG(("haveConfigurableRRMap()\n"));
  ConfigurableRRMapLoader& loader = CkpvAccess(myConfigRRMapState);
  return loader.haveConfiguration();
}

class ConfigurableRRMap : public RRMap
{
public:
  ConfigurableRRMap(void) { DEBC((AA "Creating ConfigurableRRMap\n" AB)); }
  ConfigurableRRMap(CkMigrateMessage* m) : RRMap(m) {}

  void populateInitial(int arrayHdl, CkArrayOptions& options, void* ctorMsg, CkArray* mgr)
  {
    CkArrayIndex end = options.getEnd();
    // Try to load the configuration from command line argument
    CkAssert(haveConfigurableRRMap());
    ConfigurableRRMapLoader& loader = CkpvAccess(myConfigRRMapState);
    if (end.dimension == 0)
    {
      CkFreeMsg(ctorMsg);
      return;
    }
    int thisPe = CkMyPe();
    int maxIndex = end.data()[0];
    DEBUG(("[%d] ConfigurableRRMap: index=%d,%d,%d\n", CkMyPe(), (int)end.data()[0],
           (int)end.data()[1], (int)end.data()[2]));

    if (end.dimension != 1)
    {
      CkAbort("ConfigurableRRMap only supports dimension 1!");
    }

    for (int index = 0; index < maxIndex; index++)
    {
      CkArrayIndex1D idx(index);

      int cyclic_block = index / loader.objs_per_block;
      int cyclic_local = index % loader.objs_per_block;
      int l = loader.locations[cyclic_local];
      int PE = (cyclic_block * loader.PE_per_block + l) % CkNumPes();

      DEBUG(("[%d] ConfigurableRRMap: index=%d is located on PE %d l=%d\n", CkMyPe(),
             (int)index, (int)PE, l));

      if (PE == thisPe)
        mgr->insertInitial(idx, CkCopyMsg(&ctorMsg));
    }
    //        CKARRAYMAP_POPULATE_INITIAL(PE == thisPe);

    CkFreeMsg(ctorMsg);
  }
};

CkpvStaticDeclare(double*, rem);

class arrInfo
{
private:
  CkArrayIndex _nelems;
  std::vector<int> _map;

public:
  arrInfo() {}
  arrInfo(const CkArrayIndex& n, int* _speeds)
      : _nelems(n), _map(_nelems.getCombinedCount())
  {
    distrib(_speeds);
  }
  ~arrInfo() {}
  int getMap(const CkArrayIndex& i);
  void distrib(int* speeds);
  void pup(PUP::er& p)
  {
    p | _nelems;
    p | _map;
  }
};

static int cmp(const void* first, const void* second)
{
  int fi = *((const int*)first);
  int si = *((const int*)second);
  return ((CkpvAccess(rem)[fi] == CkpvAccess(rem)[si])
              ? 0
              : ((CkpvAccess(rem)[fi] < CkpvAccess(rem)[si]) ? 1 : (-1)));
}

void arrInfo::distrib(int* speeds)
{
  int _nelemsCount = _nelems.getCombinedCount();
  double total = 0.0;
  int npes = CkNumPes();
  int i, j, k;
  for (i = 0; i < npes; i++) total += (double)speeds[i];
  std::vector<double> nspeeds(npes);
  for (i = 0; i < npes; i++) nspeeds[i] = (double)speeds[i] / total;
  std::vector<int> cp(npes);
  for (i = 0; i < npes; i++) cp[i] = (int)(nspeeds[i] * _nelemsCount);
  int nr = 0;
  for (i = 0; i < npes; i++) nr += cp[i];
  nr = _nelemsCount - nr;
  if (nr != 0)
  {
    CkpvAccess(rem) = new double[npes];
    for (i = 0; i < npes; i++)
      CkpvAccess(rem)[i] = (double)_nelemsCount * nspeeds[i] - cp[i];
    std::vector<int> pes(npes);
    for (i = 0; i < npes; i++) pes[i] = i;
    qsort(pes.data(), npes, sizeof(int), cmp);
    for (i = 0; i < nr; i++) cp[pes[i]]++;
    delete[] CkpvAccess(rem);
  }
  k = 0;
  for (i = 0; i < npes; i++)
  {
    for (j = 0; j < cp[i]; j++) _map[k++] = i;
  }
}

int arrInfo::getMap(const CkArrayIndex& i)
{
  if (i.dimension == 1)
    return _map[i.data()[0]];
  else
    return _map[((i.hash() + 739) % 1280107) % _nelems.getCombinedCount()];
}

// Speeds maps processor number to "speed" (some sort of iterations per second counter)
// It is initialized by processor 0.
static int* globalSpeeds;

#if CMK_USE_PROP_MAP
typedef struct _speedmsg
{
  char hdr[CmiMsgHeaderSizeBytes];
  int node;
  int speed;
} speedMsg;

static void _speedHdlr(void* m)
{
  speedMsg* msg = (speedMsg*)m;
  if (CmiMyRank() == 0)
    for (int pe = 0; pe < CmiNodeSize(msg->node); pe++)
      globalSpeeds[CmiNodeFirst(msg->node) + pe] = msg->speed;
  CmiFree(m);
}

// initnode call
void _propMapInit(void)
{
  globalSpeeds = new int[CkNumPes()];
  int hdlr = CkRegisterHandler(_speedHdlr);
  CmiPrintf("[%d]Measuring processor speed for prop. mapping...\n", CkMyPe());
  int s = LBManager::ProcessorSpeed();
  speedMsg msg;
  CmiSetHandler(&msg, hdlr);
  msg.node = CkMyNode();
  msg.speed = s;
  CmiSyncBroadcastAllAndFree(sizeof(msg), &msg);
  for (int i = 0; i < CkNumNodes(); i++) CmiDeliverSpecificMsg(hdlr);
}
#else
void _propMapInit(void)
{
  globalSpeeds = new int[CkNumPes()];
  int i;
  for (i = 0; i < CkNumPes(); i++) globalSpeeds[i] = 1;
}
#endif
/**
 * A proportional map object-- tries to map more objects to
 * faster processors and fewer to slower processors.  Also
 * attempts to ensure good locality by mapping nearby elements
 * together.
 */
class PropMap : public CkArrayMap
{
private:
  CkPupPtrVec<arrInfo> arrs;

public:
  PropMap(void)
  {
    CkpvInitialize(double*, rem);
    DEBC((AA "Creating PropMap\n" AB));
  }
  PropMap(CkMigrateMessage* m) {}
  int registerArray(const CkArrayIndex& numElements, CkArrayID aid)
  {
    int idx = arrs.size();
    arrs.resize(idx + 1);
    arrs[idx] = new arrInfo(numElements, globalSpeeds);
    return idx;
  }
  void unregisterArray(int idx) { arrs[idx].destroy(); }
  int procNum(int arrayHdl, const CkArrayIndex& i) { return arrs[arrayHdl]->getMap(i); }
  void pup(PUP::er& p)
  {
    int oldNumPes = -1;
    if (p.isPacking())
    {
      oldNumPes = CkNumPes();
    }
    p | oldNumPes;
    p | arrs;
    if (p.isUnpacking() && oldNumPes != CkNumPes())
    {
      for (int idx = 0; idx < arrs.length(); ++idx)
      {
        arrs[idx]->distrib(globalSpeeds);
      }
    }
  }
};

class CkMapsInit : public Chare
{
public:
  CkMapsInit(CkArgMsg* msg)
  {
    //_defaultArrayMapID = CProxy_HilbertArrayMap::ckNew();
    _defaultArrayMapID = CProxy_DefaultArrayMap::ckNew();
    _fastArrayMapID = CProxy_FastArrayMap::ckNew();
    delete msg;
  }

  CkMapsInit(CkMigrateMessage* m) {}
};

// given an envelope of a Charm msg, find the recipient object pointer
CkMigratable* CkArrayMessageObjectPtr(envelope* env)
{
  if (env->getMsgtype() != ForArrayEltMsg)
    return NULL;  // not an array msg

  ///@todo: Delegate this to the array manager which can then deal with ForArrayEltMsg
  CkArray* mgr = CProxy_CkArray(env->getArrayMgr()).ckLocalBranch();
  return mgr ? mgr->lookup(ck::ObjID(env->getRecipientID()).getElementID()) : NULL;
}

/****************************** Out-of-Core support ********************/

#if CMK_OUT_OF_CORE
CooPrefetchManager CkArrayElementPrefetcher;
CkpvDeclare(int, CkSaveRestorePrefetch);

/**
 * Return the out-of-core objid (from CooRegisterObject)
 * that this Converse message will access.  If the message
 * will not access an object, return -1.
 */
int CkArrayPrefetch_msg2ObjId(void* msg)
{
  envelope* env = (envelope*)msg;
  CkMigratable* elt = CkArrayMessageObjectPtr(env);
  return elt ? elt->prefetchObjID : -1;
}

/**
 * Write this object (registered with RegisterObject)
 * to this writable file.
 */
void CkArrayPrefetch_writeToSwap(FILE* swapfile, void* objptr)
{
  CkMigratable* elt = (CkMigratable*)objptr;

  // Save the element's data to disk:
  PUP::toDisk p(swapfile);
  elt->virtual_pup(p);

  // Call the element's destructor in-place (so pointer doesn't change)
  CkpvAccess(CkSaveRestorePrefetch) = 1;
  elt->~CkMigratable();  //< because destructor is virtual, destroys user class too.
  CkpvAccess(CkSaveRestorePrefetch) = 0;
}

/**
 * Read this object (registered with RegisterObject)
 * from this readable file.
 */
void CkArrayPrefetch_readFromSwap(FILE* swapfile, void* objptr)
{
  CkMigratable* elt = (CkMigratable*)objptr;
  // Call the element's migration constructor in-place
  CkpvAccess(CkSaveRestorePrefetch) = 1;
  int ctorIdx = _chareTable[elt->thisChareType]->migCtor;
  elt->myRec->invokeEntry(elt, (CkMigrateMessage*)0, ctorIdx, true);
  CkpvAccess(CkSaveRestorePrefetch) = 0;

  // Restore the element's data from disk:
  PUP::fromDisk p(swapfile);
  elt->virtual_pup(p);
}

static void _CkMigratable_prefetchInit(void)
{
  CkpvExtern(int, CkSaveRestorePrefetch);
  CkpvAccess(CkSaveRestorePrefetch) = 0;
  CkArrayElementPrefetcher.msg2ObjId = CkArrayPrefetch_msg2ObjId;
  CkArrayElementPrefetcher.writeToSwap = CkArrayPrefetch_writeToSwap;
  CkArrayElementPrefetcher.readFromSwap = CkArrayPrefetch_readFromSwap;
  CooRegisterManager(&CkArrayElementPrefetcher, _charmHandlerIdx);
}
#endif

/****************************** CkMigratable ***************************/
/**
 * This tiny class is used to convey information to the
 * newly created CkMigratable object when its constructor is called.
 */

CkpvDeclare(CkMigratable_initInfo, mig_initInfo);

void _CkMigratable_initInfoInit(void)
{
  CkpvInitialize(CkMigratable_initInfo, mig_initInfo);
#if CMK_OUT_OF_CORE
  _CkMigratable_prefetchInit();
#endif
}

void CkMigratable::commonInit(void)
{
  CkMigratable_initInfo& i = CkpvAccess(mig_initInfo);
#if CMK_OUT_OF_CORE
  isInCore = true;
  if (CkpvAccess(CkSaveRestorePrefetch))
    return;            /* Just restoring from disk--don't touch object */
  prefetchObjID = -1;  // Unregistered
#endif
  myRec = i.locRec;
  thisIndexMax = myRec->getIndex();
  thisChareType = i.chareType;
  usesAtSync = false;
  usesAutoMeasure = true;
  barrierRegistered = false;

  local_state = OFF;
  prev_load = 0.0;
  can_reset = false;
  lbStepSeen = 0;
  lbIterNo = 0;
  lbStepBlocked = false;
  metaLBStreamJoinedPe = -1;
  lbStepPending = false;
  lbMeasurementClosed = false;
  waitParked = false;
  // (CkLocRec::deviceRecvParked default-initializes to false; myRec is not
  // necessarily wired yet in this init path, so it is not touched here.)
  lbWaitEpoch = 0;

#if CMK_LBDB_ON
  // Join whatever step this PE has already been asked for rather than trying to
  // start it: a chare created (or migrated in) mid-step would otherwise call
  // atBarrier for an epoch whose local barrier has already fired.
  lbStepSeen = CkpvAccess(_lbStepRequested);
  if (_lb_args.metaLbOn())
  {
    // Deliberately not joining MetaBalancer's sample stream here. Joining means
    // recording which sample index this element will first contribute to, and
    // pre-crediting the open buckets it will therefore skip -- both of which are
    // only true of the instant they are done. Once elements can migrate while
    // their new PE is still sampling, which is what +LBAsync allows, that PE can
    // close buckets between this constructor and this element's first
    // AtSyncSample, and the recorded index is then already in the past. The join
    // happens on the first sample instead, in sampleMetaLBLoad, where the
    // instant it describes is the instant it is used.
    atsync_iteration = -1;
  }
#endif
}

CkMigratable::CkMigratable(void)
{
  DEBC((AA "In CkMigratable constructor\n" AB));
  commonInit();
}
CkMigratable::CkMigratable(CkMigrateMessage* m) : Chare(m) { commonInit(); }

int CkMigratable::ckGetChareType(void) const { return thisChareType; }

void CkMigratable::lbSetMeasurementClosed(bool closed)
{
  lbMeasurementClosed = closed;
#if CMK_LBDB_ON
  myRec->getLBMgr()->SetObjJoinedStep(myRec->getLdHandle(), closed);
#if CMK_GPU
  // This set is PE-local. An arrival must overwrite both a missing exclusion
  // on a new PE and a stale exclusion when returning to a previous PE.
  if (closed)
    hapiCuptiObjectJoinedStep(myRec->getLdHandle());
  else
    hapiCuptiObjectResumed(myRec->getLdHandle());
#endif
#endif
}

void CkMigratable::pup(PUP::er& p)
{
  DEBM((AA "In CkMigratable::pup %s\n" AB, idx2str(thisIndexMax)));
  Chare::pup(p);
  p | thisIndexMax;
  p | usesAtSync;
  p | can_reset;
  p | usesAutoMeasure;
  p | objRebuildSeconds;

#if CMK_LBDB_ON
  bool readyMigrate = false;
  if (p.isPacking())
    readyMigrate = myRec->isReadyMigrate();
  p | readyMigrate;
  if (p.isUnpacking())
    myRec->ReadyMigrate(readyMigrate);
#endif

  int epoch = -1;
  // Only pup epoch when migrating. Will result in problems if pup'd when
  // checkpointing since it will not match the value of the freshly inited barrier
  // upon restart
  if (usesAtSync && p.isMigration())
  {
    if (p.isPacking())
    {
      epoch = (*ldBarrierHandle)->epoch;
    }
    p | epoch;
    // +LBAsync wait state. An element can be parked in AtSyncWait() at the
    // moment the step it is waiting on moves it, and the destination has to be
    // able to finish that wait -- see lbCheckWaitRelease. Not checkpointed, for
    // the same reason the barrier epoch is not: it describes a step in flight.
    p | lbStepPending;
    p | lbMeasurementClosed;
    p | waitParked;
    p | lbWaitEpoch;
    p | lbStepSeen;
    p | lbIterNo;
    p | lbStepBlocked;
    if (p.isUnpacking()) lbSetMeasurementClosed(lbMeasurementClosed);
  }

#if CMK_LBDB_ON
  // The position travels with the object. It is registered by the application
  // against the LB database entry on the source PE, and that entry does not
  // migrate -- without carrying it here, an object arrives with an empty
  // position and drops out of the centroid the geometric metric computes.
  {
    std::vector<LBRealType> position;
    if (!p.isUnpacking())
      position = myRec->getObjPosition();
    p | position;
    if (p.isUnpacking())
      myRec->setObjPosition(position);
  }
  // Where this element is coming from and which balancing step moved it, so
  // the balancer can take a step back (see LDObjData::prevPe). On a migration
  // the source is this PE and the step is the one in progress; a checkpoint
  // carries whatever the element last recorded, which is harmless.
  {
    int prevPe = -1, prevStep = -1;
    if (p.isPacking())
    {
      if (p.isMigration())
      {
        prevPe = CkMyPe();
        prevStep = myRec->lbStep();
      }
      else
        myRec->getObjPrev(prevPe, prevStep);
    }
    p | prevPe;
    p | prevStep;
    if (p.isUnpacking())
      myRec->setObjPrev(prevPe, prevStep);
  }
  // The replication declaration travels with the element and is declared
  // again to the database of the PE it arrives on, like the position.
  p | lbReplicaGroup;
  p | lbReplicaRank;
  p | lbReplicaCount;
  p | lbReplicaMax;
  p | lbReplicaExtraBytes;
  p | lbReplicaCostNode;
  p | lbReplicaCostRemote;
  if (p.isUnpacking() && lbReplicaGroup >= 0)
    myRec->setObjReplica(lbReplicaGroup, lbReplicaRank, lbReplicaCount, lbReplicaMax,
                         lbReplicaExtraBytes, lbReplicaCostNode, lbReplicaCostRemote);
#endif

  if (p.isUnpacking())
    ckFinishConstruction(epoch);
}

void CkMigratable::ckDestroy(void) {}
void CkMigratable::ckAboutToMigrate(void) {}
void CkMigratable::ckClone(int toPe, int rank)
{
  if (lbReplicable())
    CkAbort("the balancer asked for a copy of this element (replica %d on PE %d) but it does "
            "not implement ckClone; setLBReplica declared it replicable", rank, toPe);
}
void CkMigratable::ckRetire(void)
{
  if (lbReplicable())
    CkAbort("the balancer asked this element to retire a copy but it does not implement "
            "ckRetire; setLBReplica declared it replicable");
}

#if CMK_LBDB_ON
void CkMigratable::ckSuspendBarrierForDeferredDestroy()
{
  // Remove this chare from the AtSync barrier so it is not woken by the next
  // ResumeClients. myRec stays alive; CkLocMgr still holds the chare and
  // deletes it when the migration ack arrives.
  if (usesAtSync && barrierRegistered)
  {
    myRec->getSyncBarrier()->removeClient(ldBarrierHandle);
    barrierRegistered = false;
  }
}
#endif

int CkMigratable::ckPrepareIntraProcessMigrate()
{
#if CMK_GPU && CMK_GPU_COMM
  // Reallocates the element's device buffers just as the cross-PE path does,
  // so cached registrations for them have to go too.
  CkRdmaDeviceDropRegistrations(myRec);
#endif
  int epoch = -1;
  if (usesAtSync && barrierRegistered)
  {
    epoch = (*ldBarrierHandle)->epoch;
    myRec->getSyncBarrier()->removeClient(ldBarrierHandle);
    barrierRegistered = false;
  }
#if CMK_LBDB_ON
  // This element is leaving this PE's MetaBalancer sample stream without being
  // destroyed, and ~CkMigratable -- the only other place that decrements a
  // stream -- therefore never runs for this PE. Without this the stream goes on
  // waiting for a contribution that has become somebody else's, which surfaces
  // on the far side of the arithmetic as "Abort!!! Received more contribution".
  // Pairs with the credit the destination makes in sampleMetaLBLoad when it
  // sees the element arrive on a PE whose stream it has not credited.
  if (_lb_args.metaLbOn() && atsync_iteration != -1)
    myRec->getMetaBalancer()->AdjustCountForDeadContributor(atsync_iteration);
#endif
  myRec = nullptr;
  return epoch;
}

void CkMigratable::ckFinalizeIntraProcessMigrate(CkLocRec* newRec, int epoch)
{
  myRec = newRec;
  lbSetMeasurementClosed(lbMeasurementClosed);
  ckFinishConstruction(epoch);
}
void CkMigratable::ckJustMigrated(void) {}
void CkMigratable::ckJustRestored(void) {}

CkMigratable::~CkMigratable()
{
  DEBC((AA "In CkMigratable::~CkMigratable %s\n" AB, idx2str(thisIndexMax)));
#if CMK_OUT_OF_CORE
  isInCore = false;
  if (CkpvAccess(CkSaveRestorePrefetch))
    return; /* Just saving to disk--don't deregister anything. */
  /* We're really leaving or dying-- unregister from the ooc system*/
  if (prefetchObjID != -1)
  {
    CooDeregisterObject(prefetchObjID);
    prefetchObjID = -1;
  }
#endif
#if CMK_LBDB_ON
  if (barrierRegistered)
  {
    DEBL((AA "Removing barrier for element %s\n" AB, idx2str(thisIndexMax)));
    if (usesAtSync)
      myRec->getSyncBarrier()->removeClient(ldBarrierHandle);
  }

  // atsync_iteration == -1 means this element never joined the sample stream,
  // so no bucket is waiting on it and there is nothing to adjust.
  if (_lb_args.metaLbOn() && atsync_iteration != -1)
  {
    myRec->getMetaBalancer()->AdjustCountForDeadContributor(atsync_iteration);
  }
#endif
  myRec->destroy(); /* Attempt to delete myRec if it's no longer in use */
  // To detect use-after-delete
  thisIndexMax.nInts = 0;
  thisIndexMax.dimension = 0;
}

void CkMigratable::CkAbort(const char* format, ...) const
{
  char newmsg[256];
  va_list args;
  va_start(args, format);
  vsnprintf(newmsg, sizeof(newmsg), format, args);
  va_end(args);

  ::CkAbort("CkMigratable '%s' aborting: %s", _chareTable[thisChareType]->name, newmsg);
}

void CkMigratable::ResumeFromSync(void) {}

void CkMigratable::UserSetLBLoad()
{
  CkAbort("::UserSetLBLoad() not defined for this array element!\n");
}

#if CMK_LBDB_ON  // For load balancing:
// user can call this helper function to set obj load (for model-based lb)
void CkMigratable::setObjTime(double cputime) { 
  myRec->setObjTime(cputime); }
void CkMigratable::setObjPosition(const std::vector<LBRealType>& pos) {
  myRec->setObjPosition(pos); }
double CkMigratable::getObjTime() { return myRec->getObjTime(); }
void CkMigratable::setObjGPUTime(double gputime) {
  myRec->setObjGPUTime(gputime);
}
double CkMigratable::getObjGPUTime() { return myRec->getObjGPUTime(); }
double CkMigratable::getObjLastTime() { return myRec->getObjLastTime(); }
double CkMigratable::getObjLastGPUTime() { return myRec->getObjLastGPUTime(); }

#  if CMK_LB_USER_DATA
/**
 * Use this method to set user specified data to the lbdatabase.
 *
 * Eg usage:
 * In the application code,
 *   void *data = getObjUserData(CkpvAccess(_lb_obj_index));
 *   *(int *) data = val;
 *
 * In the loadbalancer or wherever this data is used do
 *   for (int i = 0; i < stats->n_objs; i++ ) {
 *     LDObjData &odata = stats->objData[i];
 *     int* udata = (int *) odata.getUserData(CkpvAccess(_lb_obj_index));
 *   }
 *
 * For a complete example look at tests/charm++/load_balancing/lb_userdata_test/
 */
void* CkMigratable::getObjUserData(int idx) { return myRec->getObjUserData(idx); }
#  endif

void CkMigratable::clearMetaLBData()
{
  //  if (can_reset) {
  local_state = OFF;
  // The GPU trigger's sample stream runs free across load balancing steps:
  // every PE completes every sample index in order, which is what keeps their
  // contributions to MetaBalancer's reduction lined up. Resetting the counter
  // here would make this chare re-contribute indices its PE has already closed.
  // prev_load still resets, so the sample that straddles ClearLoads measures
  // from zero rather than going negative.
  if (!_lb_args.metaLbGpuTrigger()) atsync_iteration = -1;
  prev_load = 0.0;
  can_reset = false;
  //  }
}

void CkMigratable::recvLBPeriod(void* data)
{
  if (_lb_args.metaLbGpuTrigger())
  {
    // The GPU trigger schedules on lbIterNo, not on the wall-time path's
    // extrapolated period, so none of the state machine below applies. This is
    // the agreed count arriving: a chare stopped at the tentative floor either
    // joins here, or is let go to keep iterating until it reaches the count.
    if (!lbStepBlocked) return;
    lbStepBlocked = false;
    const int agreed = *((int*)data);
    if (lbIterNo >= agreed)
    {
      lbJoinStep(1);
      // Under +LBAsync joining never stops a chare, so one that was stopped at
      // the tentative floor has to be handed back to the application here --
      // its AtSyncStart() already returned, so nothing else will do it.
      if (_lb_args.lbAsync()) ResumeFromSync();
    }
    else
    {
      ResumeFromSync();
    }
    return;
  }
  if (atsync_iteration < 0)
  {
    return;
  }
  int lb_period = *((int*)data);
  DEBAD(("\t[obj %s] Received the LB Period %d current iter %d state %d on PE %d\n",
         idx2str(thisIndexMax), lb_period, atsync_iteration, local_state, CkMyPe()));

  bool is_tentative;
  if (local_state == LOAD_BALANCE)
  {
    CkAssert(lb_period == myRec->getMetaBalancer()->getPredictedLBPeriod(is_tentative));
    return;
  }

  if (local_state == PAUSE)
  {
    if (atsync_iteration < lb_period)
    {
      local_state = DECIDED;
      ResumeFromSync();
      return;
    }
    local_state = LOAD_BALANCE;

    can_reset = true;
    return;
  }
  local_state = DECIDED;
}

void CkMigratable::metaLBCallLB()
{
  if (usesAtSync)
    myRec->getSyncBarrier()->atBarrier(ldBarrierHandle);
}

void CkMigratable::ckFinishConstruction(int epoch)
{
  //	if ((!usesAtSync) || barrierRegistered) return;
  if (usesAtSync && _lb_args.lbperiod() != -1.0)
    CkAbort("You must use AtSync or Periodic LB separately!\n");

  myRec->setMeasure(usesAutoMeasure);
  if (barrierRegistered)
    return;
  DEBL((AA "Registering barrier client for %s\n" AB, idx2str(thisIndexMax)));
  if (usesAtSync)
  {
    ldBarrierHandle = myRec->getSyncBarrier()->addClient(
        this, [=]() { this->ResumeFromSyncHelper(); }, epoch);
  }
  barrierRegistered = true;
}

// The unsplit call: the two halves, back to back. That is all "stop here until
// the step is over" means, and there is no separate mechanism behind it any
// more. There used to be -- a parallel forty-line implementation with its own
// MetaBalancer state machine -- which meant two ways to join a balancing step,
// and every fix had to be made twice or, in practice, only to the one being
// worked on.
//
// New code should call the halves directly and put its own work between them.
// Written that way it is correct with or without +LBAsync; the flag only
// decides whether the work in between actually overlaps the step.
void CkMigratable::AtSync(int waitForMigration)
{
  // Blocked means the element is stopped and must not run -- so it must not
  // wait either. It is handed back through ResumeFromSync, which is exactly
  // what an unsplit caller is already waiting for.
  if (AtSyncStart(waitForMigration) == AtSyncStatus::Blocked) return;
  AtSyncWait();
}

// Measure what the load balancer needs to know about this chare: its load, its
// serialized size, and -- only when a step is actually starting -- the device
// footprint the memory contract prices migration with. A sample skips that last
// part: it is a second full pup traversal, and nothing reads the result until a
// step runs.
void CkMigratable::recordLBSizes(bool forStep)
{
  // model-based load balancing, ask user to provide cpu load
  if (usesAutoMeasure == false)
    UserSetLBLoad();

  #ifdef CMK_GPU
  if (forStep) {
  PUP::sizer ps(PUP::er::IS_MIGRATION);
  // Sized as the pack ships it: Stable buffers go at the decision, so a move
  // is priced on what travels at the pack.
  ps.stableSkip = true;
  this->virtual_pup(ps);
  // printf("[%d] gpu pup size %ld\n",CkMyPe(), ps.gpu_size() );
  setGPUPupSize(ps.gpu_size());
  #if CMK_LB_USER_DATA
  // Per-chare device footprint, written into the LB user-data slot the
  // memory-aware strategies read: its hapiMalloc and device pool blocks, and
  // the migration arena it landed in. Floored at the serialized size so an
  // unattributed footprint (allocations made by other code on its behalf)
  // can never read as "free to move".
  {
    const int udIdx = CkpvAccess(_lb_obj_index);
    void* ud = (udIdx >= 0) ? getObjUserData(udIdx) : NULL;
    size_t footprint = 0;
    if (ud != NULL) {
      footprint = hapiCurrentObjectFootprint();
      const size_t staged = ps.gpu_size() + ps.gpu_stable_size();
      if (footprint < staged) footprint = staged;
      *(size_t*)ud = footprint;
    }
    // The rebuild cost the element declared, into its slot (the entry is
    // this PE's; a moved element writes it again at its next step here).
    const int rbIdx = CkpvAccess(_lb_obj_rebuild_index);
    void* rb = (rbIdx >= 0) ? getObjUserData(rbIdx) : NULL;
    if (rb != NULL) *(double*)rb = objRebuildSeconds;
  }
  #endif
  }
  #endif
  if (_lb_psizer_on || _lb_args.metaLbOn())
  {
    PUP::sizer ps(PUP::er::IS_MIGRATION);
    this->virtual_pup(ps);
    if (_lb_psizer_on)
    {
      setPupSize(ps.size());
    }
    //TODO: check if this is correct after gpuPUP size
    if (_lb_args.metaLbOn())
      myRec->getMetaBalancer()->SetCharePupSize(ps.size());
  }
}

// Fold this chare's load since its last sample into the local MetaBalancer.
// Once every chare on this PE has done so for a sample index, the PE contributes
// it to a background reduction. Nothing here is collective and nothing blocks.
void CkMigratable::sampleMetaLBLoad()
{
  if (atsync_iteration == -1)
  {
    can_reset = false;
    local_state = OFF;
    prev_load = 0.0;
    // Join the stream where this PE is right now. AdjustCountForNewContributor
    // credits the buckets still open here, which are exactly the ones this
    // element is skipping past -- so they can complete without it, and the one
    // it is about to open is the first it owes.
    atsync_iteration = myRec->getMetaBalancer()->get_iteration();
    myRec->getMetaBalancer()->AdjustCountForNewContributor(atsync_iteration);
    metaLBStreamJoinedPe = CkMyPe();
  }
  else if (metaLBStreamJoinedPe != CkMyPe())
  {
    // Arrived from another PE carrying that PE's sample index. Streams advance
    // per PE, so the index it brings can already be behind this one -- and then
    // the credit below, which only covers finished+1 through atsync_iteration,
    // cannot reach the bucket it is about to contribute to. It contributes to a
    // bucket this PE has already closed and trips the guard further down.
    // Join where this PE actually is, exactly as a first-time contributor does.
    const int here = myRec->getMetaBalancer()->get_iteration();
    if (atsync_iteration < here) atsync_iteration = here;
    // Arrived by migration, carrying its own atsync_iteration, so the branch
    // above did not run. This PE's open buckets still have to be credited for a
    // contributor they were not counting, exactly as for a new chare -- without
    // it this PE eventually sees more contributions to an index than it has
    // objects, and aborts.
    myRec->getMetaBalancer()->AdjustCountForNewContributor(atsync_iteration);
    metaLBStreamJoinedPe = CkMyPe();
  }

  atsync_iteration++;
  // CkPrintf("[pe %s] atsync_iter %d && predicted period %d state: %d\n",
  //    idx2str(thisIndexMax), atsync_iteration,
  //    myRec->getMetaBalancer()->getPredictedLBPeriod(), local_state);
  double tmp = prev_load;
  prev_load = myRec->getObjTime();
  double current_load = prev_load - tmp;

  // If the load for the chares are based on certain model, then set the
  // current_load to be whatever is the obj load.
  if (!usesAutoMeasure)
  {
    current_load = myRec->getObjTime();
  }

  // A step's ClearLoads can land inside a sample window, restarting the object
  // timer below the reading this chare last took.
  if (current_load < 0.0) current_load = 0.0;

  if (atsync_iteration <= myRec->getMetaBalancer()->get_finished_iteration())
  {
    CkPrintf("[%d:%s] Error!! Contributing to iter %d < current iter %d\n", CkMyPe(),
             idx2str(thisIndexMax), atsync_iteration,
             myRec->getMetaBalancer()->get_finished_iteration());
    CkAbort("Not contributing to the right iteration\n");
  }

  if (atsync_iteration != 0)
  {
    myRec->getMetaBalancer()->AddLoad(atsync_iteration, current_load);
  }
}

// Contribute load to MetaBalancer without going anywhere near the barrier. Call
// this every few iterations: it is the expensive half (on CUDA it flushes and
// normalizes the GPU counters once per sample per process), and MetaBalancer
// only needs a periodic reading to spot an imbalance.
void CkMigratable::AtSyncSample()
{
  if (!usesAtSync)
    CkAbort(
        "You must set usesAtSync=true in your array element constructor to use "
        "AtSyncSample!\n");
  // No balancer is listening, or MetaBalancer is off and nothing consumes the
  // sample. Either way this is a no-op, so applications can call it blind.
  if (!_lb_args.metaLbOn() || !myRec->getSyncBarrier()->hasReceivers()) return;

  ckFinishConstruction();
  recordLBSizes(false);
  sampleMetaLBLoad();
  // This chare's own window closes in lbJoinStep (SetObjJoinedStep), which is
  // per object. No LBTurnInstrumentOff() here: the PE-wide flag is closed once
  // per LB step where the strategy reads the loads (DistBaseLB::barrierDone,
  // CentralLB::InvokeLB) and reopened at ClearLoads. See the note there.
}

bool CkMigratable::AtSyncPending() const
{
  if (!usesAtSync || !myRec->getSyncBarrier()->hasReceivers()) return false;
  // Without MetaBalancer every AtSyncStart starts a step, so one is always due.
  if (!_lb_args.metaLbOn()) return true;
  // A step is agreed, and this chare's next AtSyncStart is the one it was
  // agreed for. Both halves matter: a step can be requested many iterations
  // before the count it is to be joined at.
  if (CkpvAccess(_lbStepRequested) <= lbStepSeen) return false;
  const int agreed = CkpvAccess(_lbStepPeriod);
  return agreed > 0 && lbIterNo + 1 >= agreed;
}

// Start a load balancing step if one has been requested since this chare last
// joined one. Cheap enough to call every iteration -- which is the point: a
// step begins one iteration after MetaBalancer sees the imbalance, however
// rarely AtSyncSample runs.
//
// Without +LBAsync this behaves like AtSync: the element joins the barrier and
// waits for ResumeFromSync, and when no step is due it resumes inline. With
// +LBAsync it never resumes -- it returns to its caller, which keeps running
// while the strategy and the migrations proceed, and AtSyncWait() is where the
// element finds out the step is over.
CkMigratable::AtSyncStatus CkMigratable::AtSyncStart(int waitForMigration)
{
  if (!usesAtSync)
    CkAbort(
        "You must set usesAtSync=true in your array element constructor to use "
        "AtSyncStart!\n");
  // Nothing resumes the element from this half, in either mode: AtSyncWait() is
  // the single point a step is handed back, and it resumes inline when there
  // was no step to wait for. That is what lets one code path serve both.
  if (!myRec->getSyncBarrier()->hasReceivers()) return AtSyncStatus::Continue;

  // One step at a time, per element. Joining again would pass the barrier
  // twice for one element and could fire the next step's barrier before this
  // step's migrations had even been released.
  if (lbStepPending)
    CkAbort(
        "AtSyncStart() called while the load balancing step this element started "
        "is still in flight. Every AtSyncStart() must be followed by an "
        "AtSyncWait() before the next one.\n");

  // MetaBalancer's wall-time path. It picks a step from a predicted period
  // rather than an agreed count, and it joins the barrier separately through
  // metaLBCallLB() once the decision reaches the element -- so this half only
  // records where the element is, never joins, and the element is stopped until
  // that decision arrives. Kept verbatim from the unsplit AtSync, which was the
  // only caller that ever ran it.
  if (_lb_args.metaLbOn() && !_lb_args.metaLbGpuTrigger())
  {
    myRec->AsyncMigrate(!waitForMigration);
    if (waitForMigration) ReadyMigrate(true);
    ckFinishConstruction();
    recordLBSizes(true);
    sampleMetaLBLoad();

    bool is_tentative;
    if (atsync_iteration <
        myRec->getMetaBalancer()->getPredictedLBPeriod(is_tentative))
      return AtSyncStatus::Continue;

    if (!is_tentative && local_state == DECIDED)
    {
      local_state = LOAD_BALANCE;
      can_reset = true;
    }
    else
    {
      local_state = PAUSE;
    }
    return AtSyncStatus::Blocked;
  }

  // With MetaBalancer off there is nothing to decide for us, so every call
  // starts a step and the application's own cadence is the trigger.
  if (_lb_args.metaLbOn())
  {
    // The application calls this on a fixed cadence, so this counter is the
    // clock every chare agrees on. It has to advance on every call, including
    // the calls with no step to start, or the counts drift apart.
    ++lbIterNo;
    myRec->getMetaBalancer()->reportStartNo(lbIterNo);

    if (CkpvAccess(_lbStepRequested) <= lbStepSeen)
    {
        return AtSyncStatus::Continue;
    }

    const int agreed = CkpvAccess(_lbStepPeriod);
    if (agreed == 0)
    {
      // A step has been called for but its count is still being agreed. Run up
      // to the tentative floor and stop there. Stopping is the whole mechanism:
      // it bounds how far any chare can get before the count is settled, so the
      // agreed count is one no chare has passed. Relying instead on the final
      // broadcast outrunning the application would make correctness a race.
      if (lbIterNo < CkpvAccess(_lbStepTentative))
      {
            return AtSyncStatus::Continue;
      }
      lbStepBlocked = true;
      if (migDbg())
        CkPrintf("[LBSTEP] pe=%d elem=%s BLOCK iter=%d tentative=%d\n", CkMyPe(),
                 idx2str(thisIndexMax), lbIterNo, CkpvAccess(_lbStepTentative));
      return AtSyncStatus::Blocked;
    }

    if (lbIterNo < agreed)
    {
        return AtSyncStatus::Continue;
    }
  }

  lbJoinStep(waitForMigration);
  return AtSyncStatus::Started;
}

// The tail of a step join, shared by AtSyncStart and the release that happens
// when a blocked chare learns the agreed count.
void CkMigratable::lbJoinStep(int waitForMigration)
{
  if (_lb_args.metaLbOn()) lbStepSeen = CkpvAccess(_lbStepRequested);

  myRec->AsyncMigrate(!waitForMigration);
  // Migratable straight away, in both modes. An element that keeps running
  // under +LBAsync can still be moved at any entry-method boundary, because
  // making its own state safe to pack is its pup's job -- an element with
  // device state settles its streams on the packing pass, which is the only
  // code that knows which streams those are. Holding the move until AtSyncWait
  // instead would mean an element never overlaps its own migration with its own
  // iterations, which is most of the point of the split.
  if (waitForMigration)
    ReadyMigrate(true);
  ckFinishConstruction();
  DEBL((AA "Element %s starting LB step %d\n" AB, idx2str(thisIndexMax), lbStepSeen));
  recordLBSizes(true);

  // This element's measurement window closes HERE, at its own join -- not at
  // the barrier, and not PE-wide. Under +LBAsync it keeps running from this
  // point until AtSyncWait releases it, and that work belongs to the next
  // round: billing it here inflated the measured load of whichever elements
  // reached the barrier earliest, non-uniformly, so the strategy saw a
  // different imbalance than an identical sync run. Reopened in
  // ResumeFromSync.
  lbSetMeasurementClosed(true);

  local_state = LOAD_BALANCE;
  can_reset = true;
  lbStepPending = true;
  lbWaitEpoch = myRec->getLBMgr()->resumeEpoch();
  if (migDbg())
    CkPrintf("[LBSTEP] pe=%d elem=%s JOIN seq=%d iter=%d epoch=%d\n", CkMyPe(),
             idx2str(thisIndexMax), lbStepSeen, lbIterNo, lbWaitEpoch);
  myRec->getSyncBarrier()->atBarrier(ldBarrierHandle);
}

// The blocking half of the split. See the header for the contract.


// Parks at AtSyncWait on this PE (the step had not finished when the element arrived): a parked element's
// measurement window reopens only when the step finishes, so its sample shrinks by the park. Counted per PE for
// the LBDB-span line (LBDatabase::GetObjData).
static thread_local int s_lbParks = 0;
int ckLbParksSinceLast() { const int n = s_lbParks; s_lbParks = 0; return n; }
void CkMigratable::AtSyncWait()
{
  if (!usesAtSync)
    CkAbort(
        "You must set usesAtSync=true in your array element constructor to use "
        "AtSyncWait!\n");
  if (!lbStepPending)
  {
    // The step already finished, so the wait is where the window opens.
    // This path bypasses ResumeFromSyncHelper(), so it must reopen this
    // element's own measurement window itself -- and it is the path always
    // taken at a long -lblag, where the step completes long before the element
    // parks. Missing it left the element marked joined for the rest of the run:
    // its load decayed to zero and every node handed the strategy an all-zero
    // load vector.
    lbSetMeasurementClosed(false);
    ResumeFromSync();
    return;
  }
  waitParked = true;
  s_lbParks++;
  myRec->deviceRecvParked = true;
  // The park is by definition a point the application has declared safe, so
  // re-assert readiness here. An application that narrows its safe window with
  // ReadyMigrate(false) -- which it must, if its pup is only valid at certain
  // points -- would otherwise leave a move buffered in nextPe with nothing left
  // to run and fire it. invokeEntry calls checkBufferedMigration when this
  // element's entry method returns, which is where such a move departs.
  myRec->ReadyMigrate(true);
  if (migDbg())
    CkPrintf("[PARK %d] elem %s pending=%d count=%d\n", CkMyPe(),
             idx2str(thisIndexMax), myRec->pendingMigrateTo,
             myRec->outstandingDeviceSends);
  // A migration decided while this element was still running was deferred to
  // this park. Enqueued, not driven inline: this is the element's own entry
  // method, and emigrate deletes the object under it.
  if (myRec->pendingMigrateTo != -1 && myRec->outstandingDeviceSends == 0)
  {
    const int toPe = myRec->pendingMigrateTo;
    myRec->pendingMigrateTo = -1;
    DeferredMigrateMsg* m =
        (DeferredMigrateMsg*)CmiAlloc(sizeof(DeferredMigrateMsg));
    m->mgrGid = myRec->getLocMgr()->ckGetGroupID();
    m->id = myRec->getID();
    m->toPe = toPe;
    ckPostKick(m);
  }
}

// An element that parked in AtSyncWait() and was then moved by the very step it
// was waiting on can land on its destination after that PE resumed its clients,
// so nothing there would ever call it back. Under +LBSyncResume, which +LBAsync
// forces on, the resume epoch advances globally in step, so comparing the
// destination's against the one the element parked at settles it.
void CkMigratable::lbCheckWaitRelease()
{
  if (!_lb_args.lbAsync() || !lbStepPending) return;
  if (myRec->getLBMgr()->resumeEpoch() <= lbWaitEpoch) return;

  lbStepPending = false;
  if (!waitParked) return;
  waitParked = false;
  myRec->deviceRecvParked = false;
  CkDeviceRecvAdmissionReplay(myRec->getID());
  // With the move ledger, the resume cannot complete before every migrated
  // element has registered at its destination, so landing on an
  // already-resumed PE should be impossible. This stays as a safety net, but
  // if it fires the ledger has a hole -- say so.
  CkPrintf("[%d] WARNING: element %s released by the arrival epoch check: it "
           "landed after its step's resume. The move ledger should have made "
           "this impossible -- investigate.\n",
           CkMyPe(), idx2str(thisIndexMax));
  DEBL((AA "Element %s released after migrating into a resumed PE\n" AB,
        idx2str(thisIndexMax)));
  lbSetMeasurementClosed(false);
  ResumeFromSync();
}

void CkMigratable::ReadyMigrate(bool ready) { myRec->ReadyMigrate(ready); }

void CkMigratable::ResumeFromSyncHelper()
{
  DEBL((AA "Element %s resuming from sync\n" AB, idx2str(thisIndexMax)));

  if (_lb_args.metaLbOn())
  {
    clearMetaLBData();
  }

  // The step this element started is over, in either mode -- AtSyncStart()
  // refuses to begin another while this is set.
  if (migDbg())
    CkPrintf("[LBSTEP] pe=%d elem=%s RESUME seen=%d epoch=%d\n", CkMyPe(),
             idx2str(thisIndexMax), lbStepSeen, myRec->getLBMgr()->resumeEpoch());
  lbStepPending = false;

  if (waitParked)
  {
    waitParked = false;
    myRec->deviceRecvParked = false;
    CkDeviceRecvAdmissionReplay(myRec->getID());
  }
  else if (_lb_args.lbAsync())
  {
    // Async, and it never parked: the element kept running, so there is nothing
    // to hand back here. It finds the step already over in AtSyncWait().
    return;
  }
  // Released from the wait -- reopen THIS element's window. The PE-wide flag is
  // not this element's to flip; see the note at CentralLB/DistBaseLB ClearLoads.
  // Reopen this element's own window; see lbJoinStep.
  lbSetMeasurementClosed(false);

  CkLocMgr* localLocMgr = myRec->getLocMgr();
  auto iter = localLocMgr->bufferedActiveRgetMsgs.find(ckGetID());
  if (iter != localLocMgr->bufferedActiveRgetMsgs.end())
  {
    localLocMgr->toBeResumeFromSynced.emplace(ckGetID(), this);
  }
  else
  {
    ResumeFromSync();
  }
}

void CkMigratable::setMigratable(int migratable) { myRec->setMigratable(migratable); }

void CkMigratable::setPupSize(size_t obj_pup_size) { myRec->setPupSize(obj_pup_size); }

void CkMigratable::setGPUPupSize(size_t obj_gpu_pup_size) { myRec->setGPUPupSize(obj_gpu_pup_size); }

void CkMigratable::setLBReplica(int group, int rank, int count, int maxCount, size_t extraBytes,
                                double costNode, double costRemote)
{
  lbReplicaGroup = group;
  lbReplicaRank = rank;
  lbReplicaCount = count;
  lbReplicaMax = maxCount;
  lbReplicaExtraBytes = extraBytes;
  lbReplicaCostNode = costNode;
  lbReplicaCostRemote = costRemote;
  myRec->setObjReplica(group, rank, count, maxCount, extraBytes, costNode, costRemote);
}


void CkMigratable::CkAddThreadListeners(CthThread tid, void* msg)
{
  Chare::CkAddThreadListeners(tid, msg);  // for trace
  CthSetThreadID(tid, thisIndexMax.data()[0], thisIndexMax.data()[1],
                 thisIndexMax.data()[2]);
}
#else
void CkMigratable::setObjTime(double cputime) {}
void CkMigratable::setObjPosition(const std::vector<LBRealType>& pos) {}
double CkMigratable::getObjTime() { return 0.0; }
void CkMigratable::setObjGPUTime(double gputime) {}
double CkMigratable::getObjGPUTime() { return 0.0; }
double CkMigratable::getObjLastTime() { return 0.0; }
double CkMigratable::getObjLastGPUTime() { return 0.0; }

#  if CMK_LB_USER_DATA
void* CkMigratable::getObjUserData(int idx) { return NULL; }
#  endif

/* no load balancer: need dummy implementations to prevent link error */
void CkMigratable::CkAddThreadListeners(CthThread tid, void* msg) {}
#endif

/************************** Location Records: *********************************/

/*----------------- Local:
Matches up the array index with the local index, an
interfaces with the load balancer on behalf of the
represented array elements.
*/
CkLocRec::CkLocRec(CkLocMgr* mgr, bool fromMigration, bool ignoreArrival,
                   const CkArrayIndex& idx_, CmiUInt8 id_)
    : myLocMgr(mgr), idx(idx_), id(id_), deletedMarker(NULL), running(false)
{
#if CMK_LBDB_ON
  DEBL((AA "Registering element %s with load balancer\n" AB, idx2str(idx)));
  nextPe = -1;
  asyncMigrate = false;
  readyMigrate = true;
  enable_measure = true;
  syncBarrier = CkSyncBarrier::object();
  lbmgr = mgr->getLBMgr();
  if (_lb_args.metaLbOn())
    the_metalb = mgr->getMetaBalancer();
#  if CMK_GLOBAL_LOCATION_UPDATE
  id_ = mgr->makeID(id_).getID();
#  endif
  ldHandle = lbmgr->RegisterObj(mgr->getOMHandle(), id_, (void*)this, 1);
  if (fromMigration)
  {
    DEBL((AA "Element %s migrated in\n" AB, idx2str(idx)));
    // An asynchronous arrival is one the load balancer does not hold its
    // migration barrier for, but it must still be counted. CentralLB tallies
    // these separately in future_migrates_completed, and CheckMigrationComplete
    // withholds its second token -- and with it DoneRegisteringObjects, and so
    // the next LB step -- until every one of them has landed. Skipping the call
    // entirely left future_migrates_completed at zero against a non-zero
    // future_migrates_expected, and the step after an async migration hung.
    lbmgr->Migrated(ldHandle, !ignoreArrival);
  }
#endif
}
CkLocRec::~CkLocRec()
{
  if (deletedMarker != NULL)
    *deletedMarker = true;
#if CMK_LBDB_ON
  // A move recorded in a step's ledger that will now never execute: the
  // element is being destroyed without having emigrated (emigrate clears the
  // tag when it consumes it). Retire locally or the step never resumes.
  if (lbLedgerStep >= 0) lbmgr->ledgerRetireLocal(getID(), "element destroyed");
#if CMK_GPU
  ckClearPendingPack(this);
  // Destroyed without having packed: its window place goes back. A packed
  // payload keeps its place until the ack, after this record is gone.
  ckWindowGiveBack(getID(), /*onlyIfUnpacked=*/true);
  if (stable != nullptr) myLocMgr->stableDiscard(this);
#endif
  stopTiming();
  DEBL((AA "Unregistering element %s from load balancer\n" AB, idx2str(idx)));
  lbmgr->UnregisterObj(ldHandle);
#endif
}
void CkLocRec::migrateMe(int toPe)  // Leaving this processor
{
  // This will pack us up, send us off, and delete us
  //	printf("[%d] migrating migrateMe to %d \n",CkMyPe(),toPe);
  myLocMgr->emigrate(this, toPe);
}

#if CMK_LBDB_ON
CkpvDeclare(CkLocRec*, _currentLocRec);


// A device receive deferred for correction resumes against the destination
// buffer captured before the request round trip. If the element migrates in
// that window its buffers are freed, and the correction writes into memory that
// has since been handed to another chare -- silent corruption -- while the
// message is enqueued to the PE the element has left. Count these against the
// element exactly as an outstanding send is counted, so emigrate stands down
// until the correction lands.
void CkNoteDeviceRecvDeferred(CkGroupID aid, CmiUInt8 id)
{
  if (aid.idx == -1) return;   // not an array element; nothing to stand down
  CkArray* arr = (CkArray*)CkLocalBranch(aid);
  CkLocMgr* mgr = arr ? arr->getLocMgr() : NULL;
  // The rec table is keyed by the bare element id; `id` arrives as the
  // envelope's packed ObjID (collection|home|element), which never matches it
  // as-is. Looking up the packed value here made this stand-down a silent
  // no-op for every element -- migration proceeded with corrections in
  // flight, and the correction then wrote into the departed element's freed
  // buffers.
  CkLocRec* rec = mgr ? mgr->elementNrec(ck::ObjID(id).getElementID()) : NULL;
  if (rec == NULL) return;   // already gone; nothing left here to hold
  rec->noteDeviceSendPosted();
}

void CkNoteDeviceRecvComplete(CkGroupID aid, CmiUInt8 id)
{
  if (aid.idx == -1) return;   // not an array element; nothing to stand down
  CkArray* arr = (CkArray*)CkLocalBranch(aid);
  CkLocMgr* mgr = arr ? arr->getLocMgr() : NULL;
  CkLocRec* rec = mgr ? mgr->elementNrec(ck::ObjID(id).getElementID()) : NULL;
  if (rec == NULL) return;   // departed; the stand-down it held died with it
  rec->noteDeviceSendDone();
}

// Same resolution, exported as a lookup: the receive path of the device
// registration cache (ckrdmadevice.C) attributes a destination-buffer
// registration to the element that posted it, so the entry is retired when
// that element migrates and its buffers are freed.
CkLocRec* CkFindDeviceRecvElement(CkGroupID aid, CmiUInt8 id)
{
  if (aid.idx == -1) return NULL;
  CkArray* arr = (CkArray*)CkLocalBranch(aid);
  CkLocMgr* mgr = arr ? arr->getLocMgr() : NULL;
  return mgr ? mgr->elementNrec(ck::ObjID(id).getElementID()) : NULL;
}



// Releasing the last outstanding send is what lets a migration that was waiting
// on it proceed. Deferring rather than blocking matters: an inter-node send
// completes only when the receiver acknowledges, so spinning here would deadlock
// against a receiver that is itself mid-migration.
// (DeferredMigrateMsg and its handler index are declared near the top of
// this file; the handler body lives beside noteDeviceSendDone below.)

// Under the split barrier a move used to wait for the element to park in
// AtSyncWait. That meant an element never overlapped its own migration with its
// own iterations -- most of the point of the split, per lbJoinStep.
//
// The park is not what makes a move safe. outstandingDeviceSends is: it counts
// both directions (sends still sourcing from this element's buffers, and
// receives deferred into them), and zero means nothing is touching memory the
// move is about to free. What the park adds is a guarantee the count REACHES
// zero -- admission control bounces new device receives for a parked element
// with a move pending, so it cannot be fed faster than it drains.
//
// So take the move at any device-quiet moment and keep the park as the fallback
// for an element that never goes quiet while running. Strictly more
// opportunities to move, under the same safety condition, and admission control
// is untouched: freezing a RUNNING element's receives is what deadlocked pic2d
// (see 9ddd1d058), and that is not what this changes. An element that moves
// mid-window carries its wait state -- lbStepPending, waitParked, lbWaitEpoch --
// in CkMigratable::pup, calls AtSyncWait() on its new PE, and finds the step
// already over there, which is the case that path already handles.

extern "C" void _deferredMigrateHandler(void* arg)
{
  DeferredMigrateMsg* m = (DeferredMigrateMsg*)arg;
  CkLocMgr* mgr = (CkLocMgr*)CkLocalBranch(m->mgrGid);
  CkLocRec* rec = mgr ? mgr->elementNrec(m->id) : NULL;
  if (rec != NULL) {
    bool movable = (rec->outstandingDeviceSends == 0);
#if CMK_LBDB_ON
    // The application's safe-to-pack window still governs. This kick is a
    // runtime-side zero crossing, not a point the application declared safe,
    // and emigrating from here would walk straight past the readyMigrate gate
    // that recvMigrate honours -- packing an element whose pup is only valid
    // between steps, mid-step. Hand the move back through recvMigrate instead:
    // it re-buffers into nextPe, and checkBufferedMigration takes it the moment
    // the application reopens the window. (ledgerRecord keeps one entry per
    // element per step, so re-entering is idempotent.)
    if (!rec->isReadyMigrate())
    {
      if (getenv("CHARM_DEBUG_MIGRATE"))
        CmiPrintf("[KICK-HOLD %d] id=%llu not ready to pack; re-buffering for "
                  "toPe=%d\n", CkMyPe(), (unsigned long long)m->id, m->toPe);
      // The application has to run on to its safe-to-pack window: let its
      // receives through again.
      if (rec->migrateKickHeld) {
        rec->migrateKickHeld = false;
        CkDeviceRecvAdmissionReplay(m->id);
      }
      rec->recvMigrate(m->toPe);
      CmiFree(m);
      return;
    }
#endif
    if (getenv("CHARM_DEBUG_MIGRATE"))
      CmiPrintf("[KICK %d] id=%llu movable=%d held=%d toPe=%d\n", CkMyPe(),
                (unsigned long long)m->id, (int)movable, (int)rec->migrateKickHeld, m->toPe);
    if (movable) {
      mgr->emigrate(rec, m->toPe);  // does not return to this record
    } else if (rec->pendingMigrateTo == -1) {
      // New transfers started between the zero crossing that enqueued this
      // kick and now. Re-arm rather than drop: the next zero crossing kicks
      // again, and dropping here would lose the migration outright -- the
      // balancer's resume barrier then waits forever for a move that no one
      // is going to make.
      rec->pendingMigrateTo = m->toPe;
    }
  }
  // An element that moved or died since the enqueue made this moot.
  CmiFree(m);
}

void CkLocRec::noteDeviceSendDone(bool urgentKick)
{
  if (outstandingDeviceSends > 0) outstandingDeviceSends--;
  if (outstandingDeviceSends == 0 && pendingMigrateTo != -1)
  {
    const int toPe = pendingMigrateTo;
    pendingMigrateTo = -1;
    // Enqueued, never called inline. The release that brought the count to
    // zero can run inside the very handler that has just enqueued this
    // element's completion message: migrating here packs the element before
    // that message is consumed, and the message is then forwarded to the new
    // process carrying pointers into this one. Going through the scheduler
    // puts the migration behind everything already enqueued -- including that
    // message -- at the cost of one local hop.
    DeferredMigrateMsg* m = (DeferredMigrateMsg*)CmiAlloc(sizeof(DeferredMigrateMsg));
    m->mgrGid = myLocMgr->ckGetGroupID();
    // Hold new device receives until the kick runs (see migrateKickHeld).
    migrateKickHeld = true;
    m->id = getID();
    m->toPe = toPe;
    ckPostKick(m, urgentKick);
  }
}

void CkLocRec::startTiming(int ignore_running)
{
  if (!ignore_running)
    running = true;
  CkpvAccess(_currentLocRec) = this;
  DEBL((AA "Start timing for %s at %.3fs {\n" AB, idx2str(idx), CkWallTimer()));
  if (enable_measure)
    lbmgr->ObjectStart(ldHandle);
}
void CkLocRec::stopTiming(int ignore_running)
{
  DEBL((AA "} Stop timing for %s at %.3fs\n" AB, idx2str(idx), CkWallTimer()));
  if ((ignore_running || running) && enable_measure)
    lbmgr->ObjectStop(ldHandle);
  if (!ignore_running) {
    running = false;
    if (CkpvAccess(_currentLocRec) == this) CkpvAccess(_currentLocRec) = NULL;
  }
}
void CkLocRec::setObjTime(double cputime) { 
  lbmgr->EstObjLoad(ldHandle, cputime); }
void CkLocRec::setObjPosition(const std::vector<LBRealType>& pos) {
  lbmgr->SetObjPosition(ldHandle, pos); }
void CkLocRec::setObjPrev(int pe, int stepNo) {
  lbmgr->SetObjPrev(ldHandle, pe, stepNo); }
void CkLocRec::getObjPrev(int& pe, int& stepNo) {
  lbmgr->GetObjPrev(ldHandle, pe, stepNo); }
int CkLocRec::lbStep() { return lbmgr->step(); }
double CkLocRec::getObjTime()
{
  LBRealType walltime, cputime;
  lbmgr->GetObjLoad(ldHandle, walltime, cputime);
  return walltime;
}
const std::vector<LBRealType>& CkLocRec::getObjPosition()
{
  return lbmgr->GetObjPosition(ldHandle);
}
void CkLocRec::setObjGPUTime(double gputime) {
  lbmgr->EstObjGPULoad(ldHandle, gputime);
}
double CkLocRec::getObjGPUTime()
{
  LBRealType gputime;
  lbmgr->GetObjGPULoad(ldHandle, gputime);
  return gputime;
}
double CkLocRec::getObjLastTime()
{
  LBRealType walltime, cputime;
  lbmgr->QueryKnownObjLoad(ldHandle, walltime, cputime);
  return walltime;
}
double CkLocRec::getObjLastGPUTime()
{
  LBRealType gputime;
  lbmgr->QueryKnownObjGPULoad(ldHandle, gputime);
  return gputime;
}
#  if CMK_LB_USER_DATA
void* CkLocRec::getObjUserData(int idx) { return lbmgr->GetDBObjUserData(ldHandle, idx); }
#  endif
#endif

// Attempt to destroy this record. If the location manager is done with the
// record (because all array elements were destroyed) then it will be deleted.
void CkLocRec::destroy(void) { myLocMgr->reclaim(this); }

/****************************************************************************/

bool CkLocRec::invokeEntry(CkMigratable* obj, void* msg, int epIdx, bool doFree)
{
  DEBS((AA "   Invoking entry %d on element %s\n" AB, epIdx, idx2str(idx)));
  bool isDeleted = false;  // Enables us to detect deletion during processing
  deletedMarker = &isDeleted;

#if CMK_TRACE_ENABLED
  if (msg)
  { /* Tracing: */
    envelope* env = UsrToEnv(msg);
    //	CkPrintf("ckLocation.C beginExecuteDetailed %d %d
    //\n",env->getEvent(),env->getsetArraySrcPe());
    if (_entryTable[epIdx]->traceEnabled)
    {
      CmiObjId projID = idx.getProjectionID();
      _TRACE_SET_OBJECT(myLocMgr->ckGetGroupID().idx, id);
      _TRACE_BEGIN_EXECUTE_DETAILED(env->getEvent(), ForChareMsg, epIdx, env->getSrcPe(),
                                    env->getTotalsize(), &projID, obj);
      if (_entryTable[epIdx]->appWork)
        _TRACE_BEGIN_APPWORK();
    }
  }
#endif

  if (doFree)
    CkDeliverMessageFree(epIdx, msg, obj);
  else /* !doFree */
    CkDeliverMessageReadonly(epIdx, msg, obj);

#if CMK_TRACE_ENABLED
  if (msg)
  { /* Tracing: */
    if (_entryTable[epIdx]->traceEnabled)
    {
      if (_entryTable[epIdx]->appWork)
        _TRACE_END_APPWORK();
      _TRACE_END_EXECUTE();
      _TRACE_CLEAR_OBJECT();
    }
  }
#endif
#if CMK_LBDB_ON
  if (!isDeleted)
    checkBufferedMigration();  // check if should migrate
#endif
  if (isDeleted)
    return false;  // We were deleted
  deletedMarker = NULL;
  return true;
}

#if CMK_LBDB_ON

void CkLocRec::staticMetaLBResumeWaitingChares(LDObjHandle h, int lb_ideal_period)
{
  CkLocRec* el = (CkLocRec*)(LBManager::Object()->GetObjUserData(h));
  DEBL((AA "MetaBalancer wants to resume waiting chare %s\n" AB, idx2str(el->idx)));
  el->myLocMgr->informLBPeriod(el, lb_ideal_period);
}

void CkLocRec::staticMetaLBCallLBOnChares(LDObjHandle h)
{
  CkLocRec* el = (CkLocRec*)(LBManager::Object()->GetObjUserData(h));
  DEBL((AA "MetaBalancer wants to call LoadBalance on chare %s\n" AB, idx2str(el->idx)));
  el->myLocMgr->metaLBCallLB(el);
}

void CkLocRec::staticMigrate(LDObjHandle h, int dest)
{
  CkLocRec* el = (CkLocRec*)(LBManager::Object()->GetObjUserData(h));
  DEBL((AA "Load balancer wants to migrate %s to %d\n" AB, idx2str(el->idx), dest));
  el->recvMigrate(dest);
}

// The balancer's replication decisions go straight to the element(s) at this
// location; the element chooses when to act (CkMigratable::ckClone).
struct CkLocCloneArgs { int toPe; int rank; };
void CkMigratable::ckCloneFromDecision(void* data)
{
  const CkLocCloneArgs* a = (const CkLocCloneArgs*)data;
  ckClone(a->toPe, a->rank);
}
void CkLocRec::staticClone(LDObjHandle h, int dest, int rank)
{
  CkLocRec* el = (CkLocRec*)(LBManager::Object()->GetObjUserData(h));
  DEBL((AA "Load balancer wants a copy of %s on %d as replica %d\n" AB, idx2str(el->idx), dest, rank));
  CkLocCloneArgs args = {dest, rank};
  el->myLocMgr->callMethod(el, &CkMigratable::ckCloneFromDecision, (void*)&args);
}
void CkMigratable::ckCloneNoticeFromDecision(void* data)
{
  const CkLocCloneArgs* a = (const CkLocCloneArgs*)data;
  ckCloneNotice(a->toPe, a->rank);
}
void CkMigratable::ckRetireNoticeFromDecision(void* data)
{
  const CkLocCloneArgs* a = (const CkLocCloneArgs*)data;
  ckRetireNotice(a->rank);
}
void CkLocRec::staticCloneNotice(LDObjHandle h, int dest, int rank)
{
  CkLocRec* el = (CkLocRec*)(LBManager::Object()->GetObjUserData(h));
  CkLocCloneArgs args = {dest, rank};
  el->myLocMgr->callMethod(el, &CkMigratable::ckCloneNoticeFromDecision, (void*)&args);
}
void CkLocRec::staticRetireNotice(LDObjHandle h, int rank)
{
  CkLocRec* el = (CkLocRec*)(LBManager::Object()->GetObjUserData(h));
  CkLocCloneArgs args = {-1, rank};
  el->myLocMgr->callMethod(el, &CkMigratable::ckRetireNoticeFromDecision, (void*)&args);
}
void CkLocRec::staticRetire(LDObjHandle h)
{
  CkLocRec* el = (CkLocRec*)(LBManager::Object()->GetObjUserData(h));
  DEBL((AA "Load balancer wants %s retired\n" AB, idx2str(el->idx)));
  if (_lb_args.debug() > 1)
    CkPrintf("[%d] CkLocRec: retire decision reaches element %s\n", CkMyPe(), idx2str(el->idx));
  el->myLocMgr->callMethod(el, &CkMigratable::ckRetire);
}
void CkLocRec::setObjReplica(int group, int rank, int count, int maxCount, size_t extraBytes,
                             double costNode, double costRemote)
{
  lbmgr->SetObjReplica(ldHandle, group, rank, count, maxCount, extraBytes, costNode, costRemote);
}

#if CMK_LBDB_ON
// Under +LBAsync a balancer's moves are not executed inline.
//
// ProcessMigrationDecision hands every move of a step to recvMigrate in one
// loop, and each one used to pack, send and destroy its element right there --
// all of a PE's moves back to back on the scheduler thread, with the
// application's messages waiting behind them. With the application parked,
// as it is under the unsplit barrier, that costs nothing extra. Under the
// split barrier it is the one part of the step that never overlapped: a
// leanmd step that moves ~750 Computes stopped every PE for 50-130 ms.
//
// So the move is queued instead, on this PE's own scheduler queue and BELOW
// default priority. Application messages already queued run first, and any
// that arrive while the moves drain go ahead of the moves still waiting, so
// the application sees at most one emigrate of extra latency at a time and
// the moves themselves fill the gaps where the PE would otherwise have been
// waiting on the device. If the PE never goes quiet, its elements reach the
// park, stop generating work, and the moves run then -- which is exactly when
// they used to run. The move ledger holds the step's resume until every move
// has landed, whichever way it was scheduled, so nothing about completion
// changes.
//
// The kick now goes to the urgent queue (ckPostKick), and a busy element gets
// no kick at all until its zero crossing; see there.
static bool lbMigrateDeferred() { return _lb_args.lbAsync(); }

static void ckPostDeferredMigrate(CkLocMgr* mgr, CmiUInt8 id, int toPe)
{
  DeferredMigrateMsg* m = (DeferredMigrateMsg*)CmiAlloc(sizeof(DeferredMigrateMsg));
  m->mgrGid = mgr->ckGetGroupID();
  m->id = id;
  m->toPe = toPe;
  ckPostKick(m);
}
#endif

void CkLocRec::recvMigrate(int toPe)
{
#if CMK_LBDB_ON
  if (migDecidedAt == 0.0) migDecidedAt = CkWallTimer();
#if CMK_GPU
  // Its Stable device buffers go now, while it keeps stepping; the pack
  // skips them (PUP::DeviceLife::Stable). Only when the move is deferred:
  // a synchronous move packs right here.
  if (lbMigrateDeferred()) myLocMgr->stableSnapshot(this, toPe);
#endif
  // The one point every LB-ordered move passes through, before any deferral
  // (readyMigrate buffering, outstanding-send stand-down, park). If a balancer
  // step is recording its decisions, enter this move in its ledger and tag the
  // record so emigrate can tell the destination whom to ack.
  const int step = lbmgr->ledgerRecord(getID(), toPe);
  if (step >= 0) lbLedgerStep = step;
#if CMK_GPU
  // Its payload is owed from here: the admission gate keeps it free.
  ckNotePendingPack(this, toPe);
  // And it waits its turn in this PE's migration window, if it stages. A move
  // already holding a place (this is a re-entry) goes straight on.
  if (ckWindowOn() && ck_window.inflight.find(getID()) == ck_window.inflight.end())
  {
    const size_t bytes = ckStagedBytes(this, toPe);
    if (bytes > 0 && (!ck_window.queue.empty() || !ckWindowTake(getID(), bytes)))
    {
      CkMigrateWindow::Queued q;
      q.mgr = myLocMgr;
      q.id = getID();
      q.toPe = toPe;
      ck_window.queue.push_back(q);
      return;
    }
  }
#endif
#endif
  // we are in the mode of delaying actual migration
  // till readyMigrate()
  if (readyMigrate)
  {
#if CMK_LBDB_ON
    // Queued below the application (see lbMigrateDeferred). The handler
    // re-checks readiness and the outstanding-send count when it runs, and
    // re-buffers into nextPe through this function if the window has closed.
    if (lbMigrateDeferred())
    {
      if (outstandingDeviceSends > 0)
      {
        // Busy: a kick now would only find it busy and re-arm (1225 of 3407
        // kicks in one run did exactly that). Record the move; the zero
        // crossing in noteDeviceSendDone posts the kick, with the hold.
        pendingMigrateTo = toPe;
        return;
      }
      // Quiet now: hold new device receives until the kick runs, so it finds
      // the element still quiet (see migrateKickHeld).
      migrateKickHeld = true;
      ckPostDeferredMigrate(myLocMgr, getID(), toPe);
      return;
    }
#endif
    migrateMe(toPe);
  }
  else
    nextPe = toPe;
}

#if CMK_GPU
// Starts queued moves, in order, while the window has places for them.
static void ckWindowPump(void*, double)
{
  ck_window.pumpScheduled = false;
  while (!ck_window.queue.empty())
  {
    const CkMigrateWindow::Queued q = ck_window.queue.front();
    CkLocRec* rec = q.mgr->queuedMigrationRec(q.id);
    if (rec == nullptr)  // destroyed while it waited; its ledger entry retired
    {
      ck_window.queue.pop_front();
      continue;
    }
    if (!ckWindowTake(q.id, ckStagedBytes(rec, q.toPe))) return;
    ck_window.queue.pop_front();
    rec->recvMigrate(q.toPe);  // holds a place now, so it goes on from here
  }
}
#endif

void CkLocRec::AsyncMigrate(bool use)
{
  asyncMigrate = use;
  lbmgr->UseAsyncMigrate(ldHandle, use);
}

bool CkLocRec::checkBufferedMigration()
{
  // we don't migrate in user's code when calling ReadyMigrate(true)
  // we postphone the action to here until we exit from the user code.
  if (readyMigrate && nextPe != -1)
  {
    int toPe = nextPe;
    nextPe = -1;
    // don't migrate inside the object call
    migrateMe(toPe);
    // don't do anything
    return true;
  }
  return false;
}

int CkLocRec::MigrateToPe()
{
  int pe = nextPe;
  nextPe = -1;
  return pe;
}

void CkLocRec::setMigratable(int migratable)
{
  if (migratable)
    lbmgr->Migratable(ldHandle);
  else
    lbmgr->NonMigratable(ldHandle);
}

void CkLocRec::setPupSize(size_t obj_pup_size)
{
  lbmgr->setPupSize(ldHandle, obj_pup_size);
}

void CkLocRec::setGPUPupSize(size_t obj_gpu_pup_size)
{
  lbGpuPupSize = obj_gpu_pup_size;
  lbmgr->setGPUPupSize(ldHandle, obj_gpu_pup_size);
}

#endif

// Call ckDestroy for each record, which deletes the record, and ~CkLocRec()
// removes it from the hash table, which would invalidate an iterator.
void CkLocMgr::flushLocalRecs(void)
{
  while (hash.size())
  {
    CkLocRec* rec = hash.begin()->second;
    callMethod(rec, &CkMigratable::ckDestroy);
  }
}

// All records are local records after the 64bit ID update
void CkLocMgr::flushAllRecs(void) { flushLocalRecs(); }

/*************************** Object id layout and tranches **************************/
// One copy per process; written by rank 0 at startup and by the checkpoint restore,
// read by every PE after the node barriers that follow both.
CksvDeclare(ck::objid::Layout, _objidLayout);

namespace ck {
namespace objid {

static int ceilLog2(CmiUInt8 n)
{
  int b = 0;
  while (((CmiUInt8)1 << b) < n) ++b;
  return b;
}

const Layout& getLayout() { return CksvAccess(_objidLayout); }

// Precedes the layout in a checkpoint, so that a checkpoint written without one
// (by a build before the 64-bit object id redesign) or with an older record
// ("OBJ1", part 1, without the collection widths) fails here, not by misreading
// what follows.
static const int LAYOUT_MARKER = 0x4F424A32;  // "OBJ2"

void pupLayout(PUP::er& p)
{
  int marker = LAYOUT_MARKER;
  p | marker;
  if (p.isUnpacking() && marker != LAYOUT_MARKER)
    CkAbort("Checkpoint has no current object id layout record (written by a Charm++"
            " build before the 64-bit object id redesign or its part 2, or corrupt);"
            " this build cannot restore it.\n");
  // The field widths of both layouts: ids restored from the checkpoint were cut
  // with them, so a build with other widths must not misdecode them.
  int collectionBits = ObjID::bits::COLLECTION_BITS;
  int wideCollectionBits = ObjID::bits::WIDE_COLLECTION_BITS;
  p | collectionBits;
  p | wideCollectionBits;
  if (p.isUnpacking() && (collectionBits != ObjID::bits::COLLECTION_BITS ||
                          wideCollectionBits != ObjID::bits::WIDE_COLLECTION_BITS))
    CkAbort("Checkpoint was written by a Charm++ build with %d collection bits in"
            " element ids (%d wide); this build has %d (%d wide). Restore it with a"
            " build of the same -DCMK_OBJID_COLLECTION_BITS.\n",
            collectionBits, wideCollectionBits, (int)ObjID::bits::COLLECTION_BITS,
            (int)ObjID::bits::WIDE_COLLECTION_BITS);
  if (p.isUnpacking())
  {
    Layout restored;
    restored.pup(p);
    CmiLock(CksvAccess(_nodeLock));
    CksvAccess(_objidLayout) = restored;
    CmiUnlock(CksvAccess(_nodeLock));
  }
  else
  {
    Layout current = getLayout();
    current.pup(p);
  }
}

void initLayout(int expandFactor, int trancheLog2Cap)
{
  CksvInitialize(Layout, _objidLayout);
  Layout& l = CksvAccess(_objidLayout);
  if (expandFactor < 1) expandFactor = 1;
  l.expandFactor = expandFactor;
  l.trancheLog2Cap = trancheLog2Cap > 0 ? trancheLog2Cap : 0;
  l.nodesAtLaunch = CkNumNodes();
  int keyBits = ceilLog2((CmiUInt8)CkNumNodes() * (CmiUInt8)expandFactor);
  if (keyBits < 1) keyBits = 1;
  if (keyBits > 24) keyBits = 24;
  l.keyBits = keyBits;
  l.uniqueBits = ObjID::bits::PAYLOAD_BITS - keyBits;
  // A layout too narrow for hashed ids is rejected when the first hashed-kind array
  // asks for a tranche (CkLocMgr::getTranche), not here: a run with only packed
  // arrays never needs the unique bits.
}

static inline CmiUInt8 mix64(CmiUInt8 z)
{
  // splitmix64 finalizer
  z += 0x9e3779b97f4a7c15ULL;
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  return z ^ (z >> 31);
}

CmiUInt8 indexHashKey(const CkArrayIndex& idx)
{
  CmiUInt8 h = 0x243f6a8885a308d3ULL ^ ((CmiUInt8)idx.nInts << 8) ^ (CmiUInt8)idx.dimension;
  for (int i = 0; i < idx.nInts; ++i)
    h = mix64(h ^ (CmiUInt8)(unsigned int)idx.index[i]);
  h = mix64(h);
  const int keyBits = CksvAccess(_objidLayout).keyBits;
  return h >> (64 - keyBits);
}

// Collection numbers are handed out on PE 0 only (CkCreateArray runs there), so
// plain statics suffice; a restart bumps them past every restored number.
static CmiUInt8 _nextStandardCollection = 1;
static CmiUInt8 _nextWideCollection = ObjID::limits::WIDE_FIRST;
static bool _wideSuggestionPrinted = false;

CmiUInt8 nextCollectionNumber(bool wide)
{
  CkAssert(CkMyPe() == 0);
  if (wide)
  {
    if (_nextWideCollection >= ObjID::limits::WIDE_LIMIT)
      CkAbort("Charm++ ran out of wide-collection numbers: %" PRIu64 " chare arrays with"
              " setWideCollection() exist (the limit with %d collection bits).\n",
              (CmiUInt8)ObjID::limits::WIDE_LIMIT - ObjID::limits::WIDE_FIRST,
              (int)ObjID::bits::WIDE_COLLECTION_BITS);
    return _nextWideCollection++;
  }
  // The wide layout is the way out only where it has the larger range (not in a
  // -DCMK_OBJID_COLLECTION_BITS=29 build).
  const bool wideHasMore = (CmiUInt8)ObjID::limits::WIDE_LIMIT - ObjID::limits::WIDE_FIRST >
                           (CmiUInt8)ObjID::limits::STANDARD_LIMIT - 1;
  if (_nextStandardCollection >= ObjID::limits::STANDARD_LIMIT)
    CkAbort("Charm++ ran out of collection numbers: %" PRIu64 " chare arrays exist (the"
            " limit with %d collection bits). %sRebuild with"
            " -DCMK_OBJID_COLLECTION_BITS=N.\n",
            (CmiUInt8)ObjID::limits::STANDARD_LIMIT - 1, (int)ObjID::bits::COLLECTION_BITS,
            wideHasMore ? "Create small bounded arrays with CkArrayOptions::setWideCollection(), or " : "");
  if (wideHasMore && !_wideSuggestionPrinted &&
      _nextStandardCollection >= ObjID::limits::STANDARD_LIMIT / 2)
  {
    _wideSuggestionPrinted = true;
    CkPrintf("Charm++> %" PRIu64 " chare arrays created, half of the %" PRIu64 " that element"
             " ids can name with %d collection bits. A program that creates many small"
             " bounded arrays can create them with CkArrayOptions::setWideCollection()"
             " (%" PRIu64 " collections) instead of rebuilding with"
             " -DCMK_OBJID_COLLECTION_BITS.\n",
             _nextStandardCollection, (CmiUInt8)ObjID::limits::STANDARD_LIMIT - 1,
             (int)ObjID::bits::COLLECTION_BITS,
             (CmiUInt8)ObjID::limits::WIDE_LIMIT - ObjID::limits::WIDE_FIRST);
  }
  return _nextStandardCollection++;
}

void noteRestoredCollection(CmiUInt8 number, bool wide)
{
  if (CkMyPe() != 0) return;
  CmiUInt8& next = wide ? _nextWideCollection : _nextStandardCollection;
  if (number >= next) next = number + 1;
}

}  // namespace objid
}  // namespace ck

// One tranche per process per location manager, shared by the process's PEs.
using TrancheTable = std::unordered_map<int, ck::objid::Tranche*>;
CksvDeclare(TrancheTable*, _objidTranches);

static int floorLog2(CmiUInt8 n)
{
  int b = -1;
  while (n) { ++b; n >>= 1; }
  return b < 0 ? 0 : b;
}

// Size of a process's initial tranche: the lower half of the unique space split
// evenly among the processes present at launch. The upper half is the allocator's
// pool for refills and for every process after a restart. Divided by the current
// process count, not the launch count: an array created after an expand splits its
// (untouched) initial region among the processes that exist now, and a larger launch
// share would run into the allocator's half. +objid_tranche_log2 caps it (and every
// grant) for tests that need to see refills.
static CmiUInt8 initialTrancheSize(const ck::objid::Layout& l)
{
  int shareBits = l.uniqueBits - 1 - ck::objid::ceilLog2((CmiUInt8)CkNumNodes());
  if (l.trancheLog2Cap > 0 && shareBits > l.trancheLog2Cap) shareBits = l.trancheLog2Cap;
  return shareBits > 0 ? ((CmiUInt8)1 << shareBits) : 1;
}

ck::objid::Tranche* CkLocMgr::getTranche()
{
  if (tranche) return tranche;
  CmiLock(CksvAccess(_nodeLock));
  TrancheTable*& table = CksvAccess(_objidTranches);
  if (table == nullptr) table = new TrancheTable();
  auto itr = table->find(thisgroup.idx);
  if (itr == table->end())
  {
    const ck::objid::Layout& l = ck::objid::getLayout();
    if (l.uniqueBits < 16)
      CkAbort("Object id layout: %d processes x expand factor %d need %d home key bits,"
              " leaving %d unique bits of the %d-bit payload (minimum 16) for chare"
              " array %d, whose index is not packed into ids. Lower +objid_expand,"
              " give the array bounds, or rebuild with fewer"
              " -DCMK_OBJID_COLLECTION_BITS.\n",
              l.nodesAtLaunch, l.expandFactor, l.keyBits, l.uniqueBits,
              (int)ck::ObjID::bits::PAYLOAD_BITS, thisgroup.idx);
    ck::objid::Tranche* t = new ck::objid::Tranche();
    if (!restoredFromCheckpoint)
    {
      // A new array: this process's share of the initial region, no message needed.
      const CmiUInt8 size = initialTrancheSize(l);
      const CmiUInt8 begin = (CmiUInt8)CkMyNode() * size;
      t->len = size;
      t->cursor.store(begin, std::memory_order_relaxed);
      t->end.store(begin + size, std::memory_order_release);
      t->refillAt.store(begin + size / 2, std::memory_order_relaxed);
    }
    // Restored from a checkpoint: the initial region is spent (the restored ids came
    // from it). Start empty; the first insertion on this process asks the allocator
    // (mintUnique) and is deferred until the grant arrives. Asking here, for every
    // process at every restart, would spend a grant per process per restart whether
    // or not anything is ever inserted.
    itr = table->emplace(thisgroup.idx, t).first;
  }
  tranche = itr->second;
  CmiUnlock(CksvAccess(_nodeLock));
  return tranche;
}

void CkLocMgr::requestTrancheFromAllocator(int wantLog2)
{
  thisProxy[0].requestTranche(CkMyNode(), wantLog2);
}

bool CkLocMgr::mintUnique(CmiUInt8& u)
{
  ck::objid::Tranche* t = getTranche();
  CmiNodeLock lock = CksvAccess(_nodeLock);
  for (;;)
  {
    // Read end before bumping the cursor: a tranche is installed by storing the
    // cursor first and the end (release) second, so a number taken after an end we
    // saw is in that tranche, and a number from an older cursor fails the test and
    // is merely wasted, never reused.
    const CmiUInt8 e = t->end.load(std::memory_order_acquire);
    u = t->cursor.fetch_add(1, std::memory_order_relaxed);
    if (u < e)
    {
      if (u >= t->refillAt.load(std::memory_order_relaxed))
      {
        // Half of the tranche is used: ask for the next one, once.
        bool send = false;
        int want = 0;
        CmiLock(lock);
        if (!t->haveNext && !t->requested)
        {
          t->requested = true;
          send = true;
          want = floorLog2(t->len) + 1;
        }
        t->refillAt.store(~(CmiUInt8)0, std::memory_order_relaxed);
        CmiUnlock(lock);
        if (send) requestTrancheFromAllocator(want);
      }
      return true;
    }
    // Exhausted. Under the lock: swap in the spare if it is here, else make sure a
    // request is in flight and register this PE to be woken by the grant.
    CmiLock(lock);
    if (t->cursor.load(std::memory_order_relaxed) < t->end.load(std::memory_order_relaxed))
    {
      CmiUnlock(lock);  // another PE installed the spare meanwhile
      continue;
    }
    if (t->haveNext)
    {
      t->len = t->nextLen;
      t->haveNext = false;
      t->cursor.store(t->nextBase, std::memory_order_relaxed);
      t->end.store(t->nextBase + t->nextLen, std::memory_order_release);
      t->refillAt.store(t->nextBase + t->nextLen / 2, std::memory_order_relaxed);
      CmiUnlock(lock);
      continue;
    }
    bool send = false;
    int want = 0;
    if (!t->requested)
    {
      t->requested = true;
      send = true;
      // Twice the current tranche; a process with none yet (after a restart) asks
      // for 0, which the allocator reads as "the first grant", whose size it sets
      // from the pool and the process count (requestTranche), and doubles from there.
      want = t->len ? floorLog2(t->len) + 1 : 0;
    }
    if (std::find(t->waitingPes.begin(), t->waitingPes.end(), CkMyPe()) == t->waitingPes.end())
      t->waitingPes.push_back(CkMyPe());
    CmiUnlock(lock);
    if (send) requestTrancheFromAllocator(want);
    return false;
  }
}

// PE 0: hand out the next tranche. Sizes are powers of two: what the requester asks
// for (twice its current tranche), at least a floor and at most a cap that leaves
// every process several more grants. The floor is the first grant after a restart
// (the requester asks for 0): 2^FIRST_GRANT_LOG2, or 1/64 of the cap when that is
// smaller, so that a restart on which every process inserts costs the pool at most
// 1/512 of itself (processes x cap/64 = region / 512) however many processes there
// are. The floor must stay well below the cap, or the doubling never takes effect
// and every grant is a fixed fraction of the pool: with the launch share as the
// floor a restart cost 1/8 of the pool and the ninth restart in a chain aborted;
// with a flat 2^16 the same happened from 8192 processes up, where the cap is 2^16
// (Aditya's reviews of #4021). +objid_tranche_log2 caps both for tests.
void CkLocMgr::requestTranche(int node, int wantLog2)
{
  CkAssert(CkMyPe() == 0);
  const ck::objid::Layout& l = ck::objid::getLayout();
  const CmiUInt8 region = (CmiUInt8)1 << (l.uniqueBits - 1);
  int capLog2 = floorLog2(region / (8 * (CmiUInt8)CkNumNodes()));
  if (capLog2 < 0) capLog2 = 0;
  int minLog2 = ck::objid::FIRST_GRANT_LOG2;
  if (minLog2 > capLog2 - 6) minLog2 = capLog2 - 6;
  if (minLog2 < 0) minLog2 = 0;
  if (l.trancheLog2Cap > 0 && capLog2 > l.trancheLog2Cap) capLog2 = l.trancheLog2Cap;
  if (minLog2 > capLog2) minLog2 = capLog2;
  int log2 = wantLog2;
  if (log2 < minLog2) log2 = minLog2;
  if (log2 > capLog2) log2 = capLog2;
  CmiUInt8 len = (CmiUInt8)1 << log2;
  if (allocNext + len > allocEnd) len = allocEnd - allocNext;
  const CmiUInt8 base = allocNext;
  allocNext += len;
  thisProxy[CkNodeFirst(node)].grantTranche(base, len);
}

// Rank 0 of the requesting process: install the spare and wake the PEs that
// deferred insertions while waiting for it.
void CkLocMgr::grantTranche(CmiUInt8 base, CmiUInt8 len)
{
  ck::objid::Tranche* t = getTranche();
  CmiNodeLock lock = CksvAccess(_nodeLock);
  CmiLock(lock);
  if (len == 0)
  {
    CmiUnlock(lock);
    const ck::objid::Layout& l = ck::objid::getLayout();
    CkAbort("Chare array (location manager %d) has used all 2^%d unique element ids"
            " available to it in this run (its index is not packed into ids). Give the"
            " array bounds (CkArrayOptions::setBounds), lower +objid_expand, or rebuild"
            " with fewer -DCMK_OBJID_COLLECTION_BITS.\n",
            thisgroup.idx, l.uniqueBits);
  }
  CkAssert(!t->haveNext);
  CkAssert(base >= t->end.load(std::memory_order_relaxed));
  t->nextBase = base;
  t->nextLen = len;
  t->haveNext = true;
  t->requested = false;
  std::vector<int> wake;
  wake.swap(t->waitingPes);
  CmiUnlock(lock);
  for (int pe : wake) thisProxy[pe].resumeDeferredInsertions();
}

void CkLocMgr::deferInsertion(CkArray* mgr, CkArrayMessage* msg, const CkArrayIndex& idx,
                              const int listenerData[CK_ARRAYLISTENER_MAXLEN])
{
  // Once per process (a static shared by its PEs); informational.
  static std::atomic<bool> warned{false};
  if (!warned.exchange(true))
  {
    const ck::objid::Layout& l = ck::objid::getLayout();
    ck::objid::Tranche* t = getTranche();
    CmiLock(CksvAccess(_nodeLock));
    const CmiUInt8 len = t->len;
    CmiUnlock(CksvAccess(_nodeLock));
    if (len == 0)
      CkPrintf("Charm++> Note: PE %d deferred an insertion into chare array (location"
               " manager %d): this process's first tranche of element ids after the"
               " restart has not arrived yet. The element is created when it does;"
               " until then ckLocal() is null even for a local insert.\n",
               CkMyPe(), thisgroup.idx);
    else
      CkPrintf("Charm++> Note: PE %d deferred an insertion into chare array (location"
               " manager %d): this process used its tranche of element ids before the"
               " next one arrived. The element is created when it does; until then"
               " ckLocal() is null even for a local insert. Lower +objid_expand (%d) for"
               " larger tranches (initial tranche: %" PRIu64 " ids).\n",
               CkMyPe(), thisgroup.idx, l.expandFactor, initialTrancheSize(l));
  }
  DeferredInsertion d;
  d.mgr = mgr->ckGetGroupID();
  d.msg = msg;
  d.idx = idx;
  for (int i = 0; i < CK_ARRAYLISTENER_MAXLEN; ++i) d.listenerData[i] = listenerData[i];
  deferredInsertions.push_back(d);
}

void CkLocMgr::resumeDeferredInsertions()
{
  std::vector<DeferredInsertion> work;
  work.swap(deferredInsertions);
  for (DeferredInsertion& d : work)
  {
    CkArray* mgr = static_cast<CkArray*>(CkLocalBranch(d.mgr));
    if (mgr == nullptr)
    {
      // The array was destroyed while the insertion waited for a tranche.
      CkFreeMsg(d.msg);
      continue;
    }
    mgr->insertElement(d.msg, d.idx, d.listenerData);
  }
}

/*************************** LocCache **************************/
const CkLocEntry CkLocEntry::nullEntry = CkLocEntry();
void CkLocCache::pup(PUP::er& p)
{
  // Nothing to checkpoint: every location entry is rebuilt after a restart by the
  // restored elements registering with their homes (CkLocMgr::restore/resume), and
  // the manager back-pointer is re-set by CkLocMgr::pup. (The former __FAULT__
  // branch shipped the home's remote entries, encoded against the old PE count.)
}

// CHARM_LB_MIGSTATS: how much wall time each migration path actually consumes,
// and how many objects take it. Separates the live-pointer handoff used inside a
// process from the pack/unpack path used across processes.
namespace {
struct MigStats {
  std::atomic<double> intraSecs{0.0}, packedSecs{0.0};
  std::atomic<long> intraN{0}, packedN{0};
  // Send side. The receive side was already timed; this is the half that packs
  // the element and then destroys it, and an element with device state frees
  // that state in its destructor. cudaFree synchronises the device, so this can
  // cost far more than the bytes involved suggest -- and it runs on the
  // scheduler thread, so the whole PE stops for it.
  std::atomic<double> emigSecs{0.0};
  std::atomic<long> emigN{0};
  // The move window, stage by stage: how long an element's partners wait on
  // each part of a move. Seconds, per process; mean and max at exit.
  struct Stage {
    std::atomic<double> sum{0.0}, max{0.0};
    std::atomic<long> n{0};
    void add(double d) {
      double cur = sum.load();
      while (!sum.compare_exchange_weak(cur, cur + d)) {}
      double m = max.load();
      while (d > m && !max.compare_exchange_weak(m, d)) {}
      n.fetch_add(1, std::memory_order_relaxed);
    }
    void print(const char* name) const {
      const long k = n.load();
      if (k == 0) return;
      fprintf(stderr, "[mig-window] pid=%d %-12s n=%-6ld mean=%9.3f ms  max=%9.3f ms\n",
              (int)getpid(), name, k, 1e3 * sum.load() / k, 1e3 * max.load());
    }
  };
  Stage decideToTry;   // the balancer's decision -> emigrate's first attempt
  Stage standDown;     // that attempt -> the pack (outstanding sends, channel closings acked)
  Stage pack;          // pack copies enqueued -> complete on the device
  Stage ship;          // payload dispatched -> the destination's ack
  Stage land;          // destination: payload landed -> element unpacked
  Stage settle;        // destination: unpacked -> ckJustMigrated returned
  Stage chanHold;      // channel sender: closing received -> the new resolution applied
  Stage chanClose;     // channel owner: closing sent -> its ack
  Stage shipWait;      // payload: dispatched -> its message leaves (the pack copies' producer wait)
  Stage grantWait;     // payload: landing requested -> granted and dispatched (the admission gate, two hops)
  Stage rxLand;        // destination: payload envelope arrived (landing buffer taken) -> its entry runs after the rget
  Stage sdOwnInflight; // at the first deferral: the element's own puts in flight (a count, not seconds)
  Stage sdOwnPending;  //   its puts parked for a credit
  Stage sdAcks;        //   its owned channels' closings not yet acked
  Stage snapshotLead;  // Stable snapshot taken -> the pack that skipped it
  std::atomic<long> snapshots{0}, snapshotBytes{0}, stableChecks{0}, stableChecksSkipped{0};
  ~MigStats() {
    if (intraN.load() + packedN.load() + emigN.load() == 0) return;
    fprintf(stderr, "[mig-stats] pid=%d intraproc n=%ld time=%.3fs | packed n=%ld "
                    "time=%.3fs | emigrate n=%ld time=%.3fs\n",
            (int)getpid(), intraN.load(), intraSecs.load(),
            packedN.load(), packedSecs.load(),
            emigN.load(), emigSecs.load());
    decideToTry.print("decide->try");
    standDown.print("stand-down");
    pack.print("pack");
    ship.print("ship");
    land.print("land");
    settle.print("settle");
    chanHold.print("chan-hold");
    chanClose.print("chan-close");
    shipWait.print("ship-wait");
    grantWait.print("grant-wait");
    rxLand.print("rx-land");
    sdOwnInflight.print("sd-own-inflight");
    sdOwnPending.print("sd-own-pending");
    sdAcks.print("sd-acks");
    snapshotLead.print("snapshot");
    if (snapshots.load() > 0)
      fprintf(stderr, "[mig-window] pid=%d stable snapshots %ld (%.1f MB), checks %ld, skipped %ld\n",
              (int)getpid(), snapshots.load(), snapshotBytes.load() / 1048576.0,
              stableChecks.load(), stableChecksSkipped.load());
    fflush(stderr);
  }
};
MigStats g_mig;
inline bool migStatsOn() {
  static const bool on = (getenv("CHARM_LB_MIGSTATS") != nullptr);
  return on;
}
}  // namespace
// The channel protocol's two waits, from ckrdmadevice.C: 0 = a sender held
// from the closing to the next resolution, 1 = an owner from its closing to
// the ack.
bool ckMigStatsOn() { return migStatsOn(); }
void ckMigStatChannel(int kind, double seconds)
{
  if (kind == 0) g_mig.chanHold.add(seconds);
  else if (kind == 1) g_mig.chanClose.add(seconds);
  else g_mig.shipWait.add(seconds);
}
namespace {
struct MigTimer {
  double t0; bool on; std::atomic<double>* acc; std::atomic<long>* cnt;
  MigTimer(std::atomic<double>* a, std::atomic<long>* c)
      : t0(0), on(migStatsOn()), acc(a), cnt(c) { if (on) t0 = CkWallTimer(); }
  ~MigTimer() {
    if (!on) return;
    double d = CkWallTimer() - t0, cur = acc->load();
    while (!acc->compare_exchange_weak(cur, cur + d)) {}
    cnt->fetch_add(1, std::memory_order_relaxed);
  }
};
}  // namespace

void ckLocNoteWhichPeMiss(bool noId)
{
  static const bool on = (getenv("CHARM_ZC_STATS") != nullptr);
  if (!on) return;
  static std::atomic<long> noIdCount{0}, noLocCount{0};
  static struct Reporter {
    ~Reporter() {
      const long a = noIdCount.load(), b = noLocCount.load();
      if (a + b == 0) return;
      fprintf(stderr, "[whichPe-miss] pid=%d no_id=%ld no_location=%ld\n",
              (int)getpid(), a, b);
      fflush(stderr);
    }
  } reporter;
  if (noId) noIdCount.fetch_add(1, std::memory_order_relaxed);
  else noLocCount.fetch_add(1, std::memory_order_relaxed);
}

void CkLocCache::requestLocation(CmiUInt8 id)
{
  int home = homePe(id);
  if (home != CkMyPe())
  {
    thisProxy[home].requestLocation(id, CkMyPe());
  }
}

void CkLocCache::requestLocation(CmiUInt8 id, const int peToTell)
{
  if (peToTell == CkMyPe()) return;

  LocationMap::const_iterator itr = locMap.find(id);
  if (itr != locMap.end())
  {
    thisProxy[peToTell].updateLocation(itr->second);
    return;
  }
  // No location: the element died or was re-created under a newer id. Answer by
  // index where the index is recoverable (always at the home); else drop, as before.
  CkArrayIndex idx;
  if (mgr && mgr->recoverIndex(id, idx))
    mgr->requestLocation(idx, peToTell);
}

void CkLocCache::updateLocation(const CkLocEntry& newEntry)
{
  // printf("[%d] updateLocation: id=%llu pe=%d epoch=%d\n", CmiMyPe(), newEntry.id, newEntry.pe, newEntry.epoch);
  CkAssert(newEntry.pe != -1);
  // The answer is in; a later miss on this element may ask again.
  pendingLocReqs.erase(newEntry.id);
  CkLocEntry& oldEntry = locMap[newEntry.id];
  // printf("[%d] updateLocation: oldEntry.epoch=%d\n", CmiMyPe(), oldEntry.epoch);
  if (newEntry.epoch > oldEntry.epoch)
  {
    oldEntry = newEntry;
    notifyListeners(newEntry.id, newEntry.pe);
  }
}

void CkLocCache::recordEmigration(CmiUInt8 id, int pe)
{
  LocationMap::iterator itr = locMap.find(id);

  // printf("[%d] recordEmigration: id=%llu itr->second.pe=%d pe=%d\n", CmiMyPe(), id, itr->second.pe, pe);

  CkAssert(itr != locMap.end());
  CkAssert(itr->second.pe == CkMyPe());

  itr->second.pe = pe;
  itr->second.epoch++;
}


void CkLocCache::insert(CmiUInt8 id, int epoch)
{
  CkLocEntry& e = locMap[id];
  // TODO: This should be > probably, but demand creation needs some fixing up
  CkAssert(epoch >= e.epoch);
  e.id = id;
  e.pe = CkMyPe();
  e.epoch = epoch;
  notifyListeners(e.id, e.pe);
}

/*************************** LocMgr: CREATION *****************************/
CkLocMgr::CkLocMgr(CkArrayOptions opts)
    : bounds(opts.getBounds()),
      wideCollection(opts.isWideCollection()),
      collectionNumber(opts.getCollectionNumber()),
      payloadBits(opts.isWideCollection() ? (int)ck::ObjID::bits::WIDE_PAYLOAD_BITS
                                          : (int)ck::ObjID::bits::PAYLOAD_BITS),
      tranche(nullptr),
      restoredFromCheckpoint(false),
      allocNext(0),
      allocEnd(0),
      thisProxy(thisgroup),
      thislocalproxy(thisgroup, CkMyPe())
{
  DEBC((AA "Creating new location manager %d\n" AB, thisgroup.idx));

  duringMigration = false;
  duringDestruction = false;

  // Register with the map object
  mapID = opts.getMap();
  map = static_cast<CkArrayMap*>(CkLocalBranch(mapID));
  if (map == nullptr)
    CkAbort("ERROR! Local branch of array map is NULL!");
  // TODO: Should this be registered here, CkArray, CkLocMgr, or none?
  mapHandle = map->registerArray(opts.getEnd(), thisgroup);

  cacheID = opts.getLocationCache();
  cache = static_cast<CkLocCache*>(CkLocalBranch(cacheID));
  if (cache == nullptr)
    CkAbort("ERROR! Local branch of location cache is NULL!\n");
  cache->setManager(this);

  // Figure out the mapping from indices to object IDs if one is possible
  compressor = ck::FixedArrayIndexCompressor::make(bounds, payloadBits);
  if (wideCollection && compressor == nullptr)
  {
    // The wide layout's 37-bit payload is too narrow for hashed ids at scale
    // (design 12.2), so a wide array must pack its index.
    const unsigned int need = ck::FixedArrayIndexCompressor::bitsNeeded(bounds);
    if (need == 0)
      CkAbort("Chare array (location manager %d) has setWideCollection() but no bounds."
              " A wide-collection array packs its index into element ids, so it needs"
              " CkArrayOptions::setBounds(...) with its full index space.\n",
              thisgroup.idx);
    // The standard layout is the alternative only where its payload is wider
    // (not in a -DCMK_OBJID_COLLECTION_BITS=29 build).
    CkAbort("Chare array (location manager %d) has setWideCollection() and bounds that"
            " need %u bits to pack its index, but a wide-collection id holds %d. Use"
            " smaller bounds%s.\n",
            thisgroup.idx, need, payloadBits,
            (int)ck::ObjID::bits::PAYLOAD_BITS > payloadBits
                ? ", or the standard layout (wider payload) without setWideCollection()"
                : "");
  }
  if (compressor == nullptr)
  {
    // The allocator's pool: the upper half of the unique space. Only PE 0's branch
    // hands it out, but every branch starts with the same value so a shrink/expand
    // restart, which restores PE 0's copy everywhere, changes nothing.
    const int U = ck::objid::getLayout().uniqueBits;
    allocNext = (CmiUInt8)1 << (U - 1);
    allocEnd = (CmiUInt8)1 << U;
  }
  if (compressor == nullptr && CkMyPe() == 0)
  {
    // Say so once: without a compressor the array pays for an index<->id map and
    // hashed ids handed out in tranches. The sized CkArrayOptions constructors set
    // bounds, but setNumInitial/setEnd do not, so this is easy to get by accident.
    const unsigned int need = ck::FixedArrayIndexCompressor::bitsNeeded(bounds);
    if (need > 0)
      CkPrintf("Charm++> Note: chare array (location manager %d) has bounds that need %u"
               " bits to pack its index, but element ids hold %d; elements use hashed"
               " ids and an index map.\n",
               thisgroup.idx, need, payloadBits);
    else if (opts.getNumInitial().dimension > 0 || opts.getEnd().dimension > 0)
      CkPrintf("Charm++> Note: chare array (location manager %d) was given an initial"
               " size but no bounds, so its index is not packed into element ids;"
               " elements use hashed ids and an index map. Call"
               " CkArrayOptions::setBounds(...) if the index space is fixed.\n",
               thisgroup.idx);
  }

  // Find and register with the load balancer
#if CMK_LBDB_ON
  lbmgrID = _lbmgr;
  metalbID = _metalb;
  initLB(lbmgrID, metalbID);
#endif
}

CkLocMgr::CkLocMgr(CkMigrateMessage* m)
    : IrrGroup(m), thisProxy(thisgroup), thislocalproxy(thisgroup, CkMyPe())
{
  duringMigration = false;
  duringDestruction = false;
  wideCollection = false;  // set by pup
  collectionNumber = 0;
}

CkLocMgr::~CkLocMgr()
{
#if CMK_LBDB_ON
  syncBarrier->removeBeginReceiver(lbBarrierBeginReceiver);
  syncBarrier->removeEndReceiver(lbBarrierEndReceiver);
  lbmgr->UnregisterOM(myLBHandle);
#endif
  map->unregisterArray(mapHandle);
}

void CkLocMgr::pup(PUP::er& p)
{
  if (p.isPacking() && pendingImmigrate.size() > 0)
  {
    CkAbort(
        "Attempting to pup location manager with buffered migration messages."
        " Likely cause is checkpointing before array creation has fully completed\n");
  }
  IrrGroup::pup(p);
  p | mapID;
  p | mapHandle;
  p | cacheID;
  p | bounds;
  p | wideCollection;
  p | collectionNumber;
  p | payloadBits;
  // The allocator's state (meaningful on PE 0, whose copy every PE restores after a
  // shrink/expand). Per-process tranches are not checkpointed: after any restart a
  // process starts empty and takes its first tranche from the allocator, so ids
  // minted after the checkpoint can never collide with ids minted after the restart.
  p | allocNext;
  p | allocEnd;
  if (p.isPacking() && !deferredInsertions.empty())
    CkAbort("Checkpointing chare array (location manager %d) on PE %d while %d insertions"
            " are waiting for a tranche of element ids; they would be lost. Checkpoint"
            " after the insertions complete (quiescence), or lower +objid_expand.\n",
            thisgroup.idx, CkMyPe(), (int)deferredInsertions.size());
  if (p.isUnpacking())
  {
    thisProxy = thisgroup;
    CProxyElement_CkLocMgr newlocalproxy(thisgroup, CkMyPe());
    thislocalproxy = newlocalproxy;

    // Register with the map object
    map = static_cast<CkArrayMap*>(CkLocalBranch(mapID));
    if (map == nullptr)
      CkAbort("ERROR! Local branch of array map is NULL!");

    // Register with the cache object
    cache = static_cast<CkLocCache*>(CkLocalBranch(cacheID));
    if (cache == nullptr)
      CkAbort("ERROR! Local branch of location cache is NULL!");
    cache->setManager(this);

    compressor = ck::FixedArrayIndexCompressor::make(bounds, payloadBits);
    ck::objid::noteRestoredCollection(collectionNumber, wideCollection);
    tranche = nullptr;
    // A location manager is only ever unpacked at a restart. The process's first
    // tranche is requested by the first insertion that needs one (getTranche).
    restoredFromCheckpoint = true;
  }

#if CMK_LBDB_ON
  p | lbmgrID;
  p | metalbID;
  if (p.isUnpacking())
  {
    initLB(lbmgrID, metalbID);
  }
#endif

  // Delay doneInserting when it is unpacking during restart to prevent load balancing
  // from kicking in.
  if (p.isUnpacking() && !CkInRestarting())
  {
    doneInserting();
  }
}

/// Add a new local array manager to our list.
void CkLocMgr::addManager(CkArrayID id, CkArray* mgr)
{
  CK_MAGICNUMBER_CHECK
  DEBC((AA "Adding new array manager\n" AB));
  managers[id] = mgr;
  auto i = pendingImmigrate.begin();
  while (i != pendingImmigrate.end())
  {
    auto msg = *i;
    if (msg->nManagers <= managers.size())
    {
      i = pendingImmigrate.erase(i);
      immigrate(msg);
    }
    else
    {
      i++;
    }
  }
}

void CkLocMgr::deleteManager(CkArrayID id, CkArray* mgr)
{
  CkAssert(managers[id] == mgr);
  managers.erase(id);
  if (managers.empty())
    delete this;
}

// Tell this element's home processor it now lives "there"
void CkLocMgr::informHome(const CkArrayIndex& idx, int nowOnPe)
{
  int home = homePe(idx);
  // TODO: If home == CkMyPe() should we call update locally?
  if (home != CkMyPe() && home != nowOnPe)
  {
    // TODO: This may not need to be an idx update. Pretty sure the home will always
    // know the idx to id mapping.
    CmiUInt8 id = lookupID(idx);
    thisProxy[home].updateLocation(idx, cache->getLocationEntry(id));
  }
}

void CkLocMgr::noteImmigrationInFlight(CmiUInt8 id)
{
  if (inFlightImmigrations.insert(id).second && migDbg())
  {
    CkPrintf("[MIG %d] in-flight immigration id=%llu (%zu outstanding)\n", CkMyPe(),
             (unsigned long long)id, inFlightImmigrations.size());
    fflush(stdout);
  }
}

CkLocRec* CkLocMgr::createLocal(const CkArrayIndex& idx, bool forMigration,
                                bool ignoreArrival, bool notifyHome, int epoch)
{
  DEBC((AA "Adding new record for element %s\n" AB, idx2str(idx)));
  CmiUInt8 id = lookupID(idx);

  CkLocRec* rec = new CkLocRec(this, forMigration, ignoreArrival, idx, id);
  insertRec(rec, id);
  // The element is locally present from here on, so anything parked against it
  // can be released. This must precede updateLocation: that fires the location
  // listeners, and CkArray's listener replays bufferedIDMsgs through sendToPe,
  // which would park them right back if the id still read as in flight.
  if (inFlightImmigrations.erase(id) > 0 && migDbg())
  {
    CkPrintf("[MIG %d] immigration landed id=%llu (%zu outstanding)\n", CkMyPe(),
             (unsigned long long)id, inFlightImmigrations.size());
    fflush(stdout);
  }
  cache->insert(id, epoch);
  updateLocation(idx, cache->getLocationEntry(id));

  if (notifyHome)
  {
    informHome(idx, CkMyPe());
  }
  return rec;
}

// Used to handle messages that were buffered because of active rgets in progress
void CkLocMgr::processAfterActiveRgetsCompleted(CmiUInt8 id)
{
  // Call ckJustMigrated
  CkLocRec* myLocRec = elementNrec(id);
  callMethod(myLocRec, &CkMigratable::ckJustMigrated);
  callMethod(myLocRec, &CkMigratable::lbCheckWaitRelease);

  // Call ResumeFromSync on elements that were waiting for rgets
  auto iter2 = toBeResumeFromSynced.find(id);
  if (iter2 != toBeResumeFromSynced.end())
  {
    iter2->second->ResumeFromSync();
    toBeResumeFromSynced.erase(iter2);
  }

  // Deliver buffered messages to the elements that were waiting on rgets
  auto iter = bufferedActiveRgetMsgs.find(id);
  if (iter != bufferedActiveRgetMsgs.end())
  {
    std::vector<CkArrayMessage*> bufferedMsgs = iter->second;
    bufferedActiveRgetMsgs.erase(iter);
    for (auto msg : bufferedMsgs)
    {
      CmiHandleMessage(UsrToEnv(msg));
    }
  }
}

bool CkLocMgr::tryNewObjectID(const CkArrayIndex& idx, CmiUInt8& id)
{
  if (!lookupID(idx, id))
  {
    // Hashed kind: the top bits are the index's hash key (so the home is
    // computable from the id), the low bits a number unique within the array,
    // taken from this process's tranche.
    CmiUInt8 u;
    if (!mintUnique(u)) return false;
    const ck::objid::Layout& l = ck::objid::getLayout();
    id = (ck::objid::indexHashKey(idx) << l.uniqueBits) | u;
    insertID(idx, id);
  }
  return true;
}

CkLocRec* CkLocMgr::registerNewElement(const CkArrayIndex& idx)
{
  CmiUInt8 id;
  if (!tryNewObjectID(idx, id)) return nullptr;
  CkLocRec* rec = elementNrec(id);
  if (rec == nullptr)
  {
    // TODO: This is going to end up needlessly calling getNewObjectID(...) again
    rec = createLocal(idx, false, false, true);
  }

  return rec;
}

bool CkLocMgr::addElementToRec(CkLocRec* rec, CkArray* mgr, CkMigratable* elt,
                               int ctorIdx, void* ctorMsg)
{
  // Insert the new element into its manager's local list
  CmiUInt8 id = lookupID(rec->getIndex());
  if (mgr->getEltFromArrMgr(id))
  {
    CkAbort("Cannot insert array element twice!");
  }
  mgr->putEltInArrMgr(id, elt);  // Local element table

  // Call the element's constructor
  DEBC((AA "Constructing element %s of array\n" AB, idx2str(rec->getIndex())));
  CkMigratable_initInfo& i = CkpvAccess(mig_initInfo);
  i.locRec = rec;
  i.chareType = _entryTable[ctorIdx]->chareIdx;

#ifndef CMK_CHARE_USE_PTR
  int callingChareIdx = CkpvAccess(currentChareIdx);
  CkpvAccess(currentChareIdx) = -1;
#endif

  if (!rec->invokeEntry(elt, ctorMsg, ctorIdx, true))
    return false;

#ifndef CMK_CHARE_USE_PTR
  CkpvAccess(currentChareIdx) = callingChareIdx;
#endif

#if CMK_OUT_OF_CORE
  /* Register new element with out-of-core */
  PUP::sizer p_getSize;
  elt->virtual_pup(p_getSize);
  elt->prefetchObjID =
      CooRegisterObject(&CkArrayElementPrefetcher, p_getSize.size(), elt);
#endif

  return true;
}

// TODO: This might be not needed
void CkLocMgr::requestLocation(CmiUInt8 id)
{
  cache->requestLocation(id);
}

void CkLocMgr::requestLocation(const CkArrayIndex& idx)
{
  CmiUInt8 id;
  if (lookupID(idx, id))
  {
    requestLocation(id);
  }
  else
  {
    int home = homePe(idx);
    if (home != CkMyPe())
    {
      thisProxy[home].requestLocation(idx, CkMyPe());
    }
  }
}

bool CkLocMgr::requestLocation(const CkArrayIndex& idx, const int peToTell)
{
  CkAssert(peToTell != CkMyPe());

  CmiUInt8 id;
  if (lookupID(idx, id) && cache->getPe(id) != -1)
  {
    // We know the id and where it lives: answer now.
    thisProxy[peToTell].updateLocation(idx, cache->getLocationEntry(id));
    return true;
  }
  // Either the index has no id yet (hashed kind: the element was never created), or
  // the id is known but its location is not (packed kind, whose ids exist before
  // the element does; or an element deleted since, whose idx -> id binding is
  // kept). Answering with the null entry would teach peToTell a location of -1 and
  // an id of 0, so hold the request: updateLocation answers it once the element
  // registers here.
  DEBN(("%d Buffering ID/location req for %s\n", CkMyPe(), idx2str(idx)));
  bufferedLocationRequests[idx].push_back(peToTell);
  return false;
}

bool CkLocMgr::recoverIndex(CmiUInt8 id, CkArrayIndex& idx) const
{
  if (compressor)
  {
    idx = compressor->decompress(id);
    return true;
  }
  if (CkLocRec* rec = elementNrec(id))
  {
    idx = rec->getIndex();
    return true;
  }
  // Hashed kind: an id that died or was rebound here (see retiredIds).
  auto r = retiredIds.find(id);
  if (r != retiredIds.end())
  {
    idx = r->second;
    return true;
  }
  return false;
}

#if CMK_LBDB_ON && CMK_GLOBAL_LOCATION_UPDATE
void CkLocMgr::updateLocationFromLB(CmiUInt8 id, int pe)
{
  CkLocEntry e;
  e.id = id;
  e.pe = pe;
  // The balancer's decision does not carry the element's epoch. Count this
  // migration from what this PE last heard (a bystander that never heard of the
  // element starts at 0): that is at most the element's true epoch after the move
  // (design section 4.2a), so the destination's insert and the emigrating PE's
  // broadcast, which carry the true epoch, still apply over this entry.
  e.epoch = cache->getEpoch(id) + 1;
  CkArrayIndex idx;
  if (recoverIndex(id, idx))
    updateLocation(idx, e);  // also binds idx -> id and fires the index listeners
  else
    cache->updateLocation(e);
}
#endif

void CkLocMgr::updateLocation(const CkArrayIndex& idx, const CkLocEntry& e)
{
  CkAssert(e.pe != -1);
  // Set the mapping from idx to id, noting an older id this PE had bound to it
  CmiUInt8 retired = 0;
  bool hasRetired = false;
  if (!compressor)
  {
    auto itr = idx2id.find(idx);
    if (itr != idx2id.end() && itr->second != e.id)
    {
      retired = itr->second;
      hasRetired = true;
    }
  }
  insertID(idx, e.id);

  // Update the location information
  cache->updateLocation(e);

  // Messages held under the retired id: re-address to e.id here, where both are known.
  if (hasRetired)
    for (auto& m : managers) m.second->releaseRetiredMsgs(retired, e.id, e.pe);

  // Any location requests that we had to buffer because we didn't know how the index
  // mapped to the id can now be replied to.
  auto itr = bufferedLocationRequests.find(idx);
  if (itr != bufferedLocationRequests.end())
  {
    for (int pe : itr->second)
    {
      DEBN(("%d Replying to buffered ID/location req to pe %d\n", CkMyPe(), pe));
      if (pe != CkMyPe())
        thisProxy[pe].updateLocation(idx, e);
    }
    bufferedLocationRequests.erase(itr);
  }

  notifyListeners(idx, e.id, e.pe);
}

/*************************** LocMgr: DELETION *****************************/
// This index may no longer be used -- check if any of our managers are still
// using it, and if not delete it and clean up all traces of it on other PEs.
void CkLocMgr::reclaim(CkLocRec* rec)
{
  CK_MAGICNUMBER_CHECK
  // Return early if the record is still in use by any of our arrays
  for (auto itr = managers.begin(); itr != managers.end(); ++itr)
  {
    if (itr->second->lookup(rec->getID()))
      return;
  }
  removeFromTable(rec->getID());

  DEBC((AA "Destroying record for element %s\n" AB, idx2str(rec->getIndex())));
  if (!duringMigration)
  {  // This is a local element dying a natural death
    int home = homePe(rec->getIndex());
    if (home == CkMyPe())
      noteDead(rec->getIndex(), rec->getID());
    else
#if CMK_MEM_CHECKPOINT
      if (!CkInRestarting())  // all array elements are removed anyway
#endif
        if (!duringDestruction)
          thisProxy[home].reclaimRemote(rec->getIndex(), CkMyPe());
  }
  delete rec;
}

// Home, hashed kind: an element of ours died (here, or on deletedOnPe). Record
// id -> idx so recoverIndex needs no scan; the idx -> id binding itself is kept.
void CkLocMgr::noteDead(const CkArrayIndex& idx, CmiUInt8 id)
{
  if (!compressor) retiredIds[id] = idx;
}

// TODO: Rename to something more basic. This is just clearing entries from the
// cache. Will be called by reclaim remote, but maybe other things as well.
void CkLocMgr::reclaimRemote(const CkArrayIndex& idx, int deletedOnPe)
{
  DEBC((AA "Our element %s died on PE %d\n" AB, idx2str(idx), deletedOnPe));

  CmiUInt8 id;
  if (!lookupID(idx, id))
    CkAbort("Cannot find ID for the given index\n");

  // Delete the ID from location caching. Do not delete the idx to id mapping because
  // that remains constant and we don't want messages to end up stranded because they
  // can't figure out their index.
  noteDead(idx, id);
  cache->erase(id);
}

void CkLocMgr::removeFromTable(const CmiUInt8 id)
{
#if CMK_ERROR_CHECKING
  // Make sure it's actually in the table before we delete it
  if (NULL == elementNrec(id))
    CkAbort("CkLocMgr::removeFromTable called on invalid index!");
#endif
  hash.erase(id);
  clearResident(id);
  // Don't erase this during migration because the entry will be updated to reflect
  // the new location by calling recordEmigration
  if (!duringMigration)
    cache->erase(id);
#if CMK_ERROR_CHECKING
  // Make sure it's really gone
  if (NULL != elementNrec(id))
    CkAbort("CkLocMgr::removeFromTable called, but element still there!");
#endif
}

// This message took several hops to reach us-- fix it
void CkLocMgr::multiHop(CkArrayMessage* msg)
{
  CK_MAGICNUMBER_CHECK
  int srcPe = msg->array_getSrcPe();
  if (srcPe == CkMyPe())
    DEB((AA "Odd routing: local element %" PRIu64 " is %d hops away!\n" AB,
         msg->array_element_id(), msg->array_hops()));
  else
  {  // Send a routing message letting original sender know new element location
    DEBS((AA "Sending update back to %d for element %" PRIu64 "\n" AB, srcPe,
          msg->array_element_id()));
    // Under CMK_GLOBAL_LOCATION_UPDATE a forward is routine, not a chain to assert
    // on: a cold send through the home (for an element that does not live at its
    // home), a demand creation, a send that raced an announced move, or a stale entry
    // after a move. Only balancers derived from CentralLB announce a move to every
    // PE before acting on it (UpdateLocation); the TreeLB-based ones (GreedyLB,
    // RefineLB, RotateLB, ...) are announced by the emigrate broadcast, which can
    // land after the program has resumed. All of these are repaired the same way.
    cache->requestLocation(msg->array_element_id(), srcPe);
  }
}

bool CkLocMgr::checkInBounds(const CkArrayIndex& idx) const
{
  if (bounds.nInts > 0)
  {
    CkAssert(idx.dimension == bounds.dimension);
    bool shorts = idx.dimension > 3;

    for (int i = 0; i < idx.dimension; ++i)
    {
      unsigned int thisDim = shorts ? idx.indexShorts[i] : idx.index[i];
      unsigned int thatDim = shorts ? bounds.indexShorts[i] : bounds.index[i];
      if (thisDim >= thatDim) return false;
    }
  }
  return true;
}

/************************** LocMgr: ITERATOR *************************/
CkLocation::CkLocation(CkLocMgr* mgr_, CkLocRec* rec_) : mgr(mgr_), rec(rec_) {}

const CkArrayIndex& CkLocation::getIndex(void) const { return rec->getIndex(); }

CmiUInt8 CkLocation::getID() const { return rec->getID(); }

void CkLocation::destroyAll() { mgr->callMethod(rec, &CkMigratable::ckDestroy); }

void CkLocation::pup(PUP::er& p)
{
  mgr->pupElementsFor(p, rec, CkElementCreation_migrate);
}

CkLocIterator::~CkLocIterator() {}

/// Iterate over our local elements:
void CkLocMgr::iterate(CkLocIterator& dest)
{
  // Poke through the hash table for local ArrayRecs.
  for (LocRecHash::iterator it = hash.begin(); it != hash.end(); it++)
  {
    CkLocation loc(this, it->second);
    dest.addLocation(loc);
  }
}

/************************** LocMgr: MIGRATION *************************/
void CkLocMgr::pupElementsFor(PUP::er& p, CkLocRec* rec, CkElementCreation_t type,
                              bool rebuild)
{
  p.comment("-------- Array Location --------");

  // First pup the element types
  // (A separate loop so ckLocal works even in element pup routines)
  for (auto itr = managers.begin(); itr != managers.end(); ++itr)
  {
    int elCType;
    CkArray* arr = itr->second;
    if (!p.isUnpacking())
    {  // Need to find the element's existing type
      CkMigratable* elt = arr->getEltFromArrMgr(rec->getID());
      if (elt)
        elCType = elt->ckGetChareType();
      else
        elCType = -1;  // Element hasn't been created
    }
    p(elCType);
    if (p.isUnpacking() && elCType != -1)
    {
      // Create the element
      CkMigratable* elt = arr->allocateMigrated(elCType, type);
      int migCtorIdx = _chareTable[elCType]->getMigCtor();
      // Insert into our tables and call migration constructor
      if (!addElementToRec(rec, arr, elt, migCtorIdx, NULL))
        return;
      if (type == CkElementCreation_resume)
      {  // HACK: Re-stamp elements on checkpoint resume--
        //  this restores, e.g., reduction manager's gcount
        arr->stampListenerData(elt);
      }
    }
  }
  // Next pup the element data
  for (auto itr = managers.begin(); itr != managers.end(); ++itr)
  {
    CkMigratable* elt = itr->second->getEltFromArrMgr(rec->getID());
    if (elt != NULL)
    {
      elt->virtual_pup(p);
#if CMK_ERROR_CHECKING
      if (p.isUnpacking())
        elt->sanitycheck();
#endif
    }
  }
#if CMK_MEM_CHECKPOINT
  if (rebuild)
  {
    ArrayElement* elt;
    std::vector<CkMigratable*> list;
    migratableList(rec, list);
    CmiAssert(!list.empty());
    for (int l = 0; l < list.size(); l++)
    {
      //    reset, may not needed now
      // for now.
      for (int i = 0; i < CK_ARRAYLISTENER_MAXLEN; i++)
      {
        ArrayElement* elt = (ArrayElement*)list[l];
        contributorInfo* c = (contributorInfo*)&elt->listenerData[i];
        if (c)
          c->redNo = 0;
      }
    }
  }
#endif
}

/// Call this member function on each element of this location:
void CkLocMgr::callMethod(CkLocRec* rec, CkMigratable_voidfn_t fn)
{
  for (auto itr = managers.begin(); itr != managers.end(); ++itr)
  {
    CkMigratable* el = itr->second->getEltFromArrMgr(rec->getID());
    if (el)
      (el->*fn)();
  }
}

/// Call this member function on each element of this location:
void CkLocMgr::callMethod(CkLocRec* rec, CkMigratable_voidfn_arg_t fn, void* data)
{
  for (auto itr = managers.begin(); itr != managers.end(); ++itr)
  {
    CkMigratable* el = itr->second->getEltFromArrMgr(rec->getID());
    if (el)
      (el->*fn)(data);
  }
}

/// return a list of migratables in this local record
void CkLocMgr::migratableList(CkLocRec* rec, std::vector<CkMigratable*>& list)
{
  for (auto itr = managers.begin(); itr != managers.end(); ++itr)
  {
    CkMigratable* elt = itr->second->getEltFromArrMgr(rec->getID());
    if (elt)
      list.push_back(elt);
  }
}

bool did_inter_node_gpudirect_rdma(int srcPe, int dstPe) {
  CmiEnforce((srcPe >= 0) && (srcPe <= CmiNumPes()));
  CmiEnforce((dstPe >= 0) && (dstPe <= CmiNumPes()));

  if (CmiNodeOf(srcPe) == CmiNodeOf(dstPe)) {
    return false;
  } else if (CmiPeOnSamePhysicalNode(srcPe, dstPe)) {
    return false;
  } else {
    return true;
  }
}

#if CMK_GPU
void CkLocMgr::sendGPUMsg(CmiUInt8 id)
{
  if (ckAdmissionGateOn()) {
    // Ask for the landing first (see ckLandingAdmissible). The payload waits
    // in its pool block until the destination grants it.
    GPUMigrateData& gpuData = sendGPUBuffers[id];
    if (migStatsOn()) gpuData.requestedAt = CkWallTimer();
    MIGT("req-send", id, nullptr);
    thisProxy[gpuData.toPe].requestLanding(id, gpuData.size, CkMyPe(),
                                           (int)gpuData.blocks.size(),
                                           gpuData.blocks.empty() ? nullptr
                                               : const_cast<CmiUInt8*>(gpuData.blocks.data()));
    return;
  }
  dispatchGPUMsg(id);
}

void CkLocMgr::landingGranted(CmiUInt8 id)
{
  if (sendGPUBuffers.find(id) == sendGPUBuffers.end())
    CkAbort("PE %d: landing granted for migration %llu, which has no payload here",
            CkMyPe(), (unsigned long long)id);
  dispatchGPUMsg(id);
}

// Destination: grant the landing now, or queue it until the pool can take it
// above the floor.
// Which caller is inside admitLandings, for the re-entry report.
static thread_local const char* ck_admit_from = "?";
static thread_local const char* ck_admit_outer = "?";
void CkLocMgr::requestLanding(CmiUInt8 id, int size, int srcPe, int n, CmiUInt8* blocks)
{
  MIGT("req-recv", id, nullptr);
  DeferredLanding d;
  d.id = id;
  d.size = size;
  d.srcPe = srcPe;
  d.blocks.assign(blocks, blocks + n);
  d.footprint = 0;
  for (int k = 0; k + 1 < n; k += 2) d.footprint += (size_t)blocks[k] * (size_t)blocks[k + 1];
  d.since = CkWallTimer();
  deferredLandings.push_back(d);
  ck_admit_from = "requestLanding";
  admitLandings();
}

// The granted element has unpacked, served from its landing set. Whatever it
// did not take -- everything, for an element that rebound its buffers into
// the arena -- goes back to the pool, where a deferred landing may fit now.
void CkLocMgr::releaseLandingSet(CmiUInt8 id)
{
  auto it = landingSets.find(id);
  if (it == landingSets.end()) return;
  size_t left = 0;
  for (auto& e : it->second) CkDeviceFree(e.second);
  landingSets.erase(it);
  ck_admit_from = "releaseLandingSet";
  if (!deferredLandings.empty()) admitLandings();
}

// Not re-entrant: the loop holds a reference into deferredLandings while it
// takes memory and sends the grant, and a nested call -- a free that admits,
// a pool release hook -- would pop that entry from under it: a double grant
// ("landing granted for migration N, which has no payload here" at the
// source) and a double destroy (free(): invalid pointer; job 22220495, the
// first cross-node landings under the vmm pool). A nested call only asks the
// outer loop for another round.
void CkLocMgr::admitLandings()
{
  if (admittingLandings) {
    admitLandingsAgain = true;
    // Not expected any more (enqueueNcpyMessage no longer runs a completed
    // zero-copy message inline); say so once per PE if it happens.
    static thread_local bool said = false;
    if (!said) {
      said = true;
      CkPrintf("[%d] WARNING: admitLandings re-entered from %s inside %s (%zu deferred): a "
               "message ran inside another entry method; deferring the round\n",
               CkMyPe(), ck_admit_from, ck_admit_outer, deferredLandings.size());
    }
    return;
  }
  admittingLandings = true;
  ck_admit_outer = ck_admit_from;
  do {
    admitLandingsAgain = false;
    admitLandingsLoop();
  } while (admitLandingsAgain && !deferredLandings.empty());
  admittingLandings = false;
}

void CkLocMgr::admitLandingsLoop()
{
  // Strictly in arrival order: a large arrival is not starved by smaller ones
  // taking the bytes it waits for.
  while (!deferredLandings.empty()) {
    DeferredLanding& d = deferredLandings.front();
    size_t reach = 0, floor = 0;
    // A landing that has waited this long is past anything the plan accounted
    // for -- memory another allocation took, or a cycle of payloads each
    // waiting on the other's landing. Take it anyway: the allocation either
    // succeeds or fails the way it did before the gate existed, rather than
    // hanging the job.
    static const double timeout = 120.0;
    const bool expired = (CkWallTimer() - d.since) > timeout;
    if (expired && !d.warned)
      CkPrintf("[%d] WARNING: admitting a %d-byte migration landing (+%zu bytes of "
               "element state) after %.0f s without the admission gate's guarantee\n",
               CkMyPe(), d.size, d.footprint, timeout);
    if (!expired &&
        !ckLandingAdmissible((size_t)d.size, d.footprint, &reach, &floor)) {
      const double waited = CkWallTimer() - d.since;
      if (!d.reported && landingDeferrals++ == 0)
        CkPrintf("[%d] memory contract: a migration landing waits for device memory "
                 "(admission gate)\n", CkMyPe());
      d.reported = true;
      if (waited > 30.0 && !d.warned) {
        d.warned = true;
        CkPrintf("[%d] WARNING: a %d-byte migration landing (+%zu bytes of element "
                 "state) has waited %.0f s for device memory (pool free %zu, pending "
                 "packs and reserved landings %zu). The balancer planned this step "
                 "against more memory than the pool has now.\n",
                 CkMyPe(), d.size, d.footprint, waited, reach, floor);
      }
      if (!landingRetryScheduled) {
        landingRetryScheduled = true;
        CcdCallFnAfter([](void* mgr, double) {
          CkLocMgr* m = (CkLocMgr*)mgr;
          m->landingRetryScheduled = false;
          ck_admit_from = "retry";
          m->admitLandings();
        }, this, 2.0);
      }
      return;
    }
    // Admissible by bytes: now take the arena and the element's blocks for
    // real, without growing the pool. By shape they may still not fit --
    // that is the case the byte check cannot see -- and then a vmm pool maps
    // what it is short of (ckTakeLandingSetGrowingVmm); otherwise this waits
    // like any other deferral, for a free.
    char* arena = nullptr;
    std::multimap<size_t, void*> set;
    bool taken = expired || ckTakeLandingSet(d.size, d.blocks, &arena, set);
    if (!taken && ckTakeLandingSetGrowingVmm(d.size, d.footprint, d.blocks, &arena, set)) {
      taken = true;
      static thread_local int grown = 0;
      if (grown++ == 0)
        CkPrintf("[%d] memory contract: a %d-byte migration landing (+%zu bytes of element "
                 "state) fit the pool by bytes but not by shape; mapped more of the vmm pool "
                 "instead of waiting for a free\n", CkMyPe(), d.size, d.footprint);
    }
    if (!taken) {
      if (!d.reported && landingDeferrals++ == 0)
        CkPrintf("[%d] memory contract: a migration landing waits for device memory "
                 "(admission gate)\n", CkMyPe());
      d.reported = true;
      if (!landingRetryScheduled) {
        landingRetryScheduled = true;
        CcdCallFnAfter([](void* mgr, double) {
          CkLocMgr* m = (CkLocMgr*)mgr;
          m->landingRetryScheduled = false;
          ck_admit_from = "retry";
          m->admitLandings();
        }, this, 2.0);
      }
      return;
    }
    if (expired && d.size > 0) {
      // Past the timeout: the arena the old way, growing if it must; the
      // element's unpack then allocates for itself. (A landing without a
      // payload has no arena to take: it is simply let through.)
      hapiFootprintBegin(nullptr);
      arena = (char*)CkDeviceMalloc((size_t)d.size);
      hapiFootprintEnd();
      if (arena == nullptr)
        CkAbort("PE %d: device pool could not provide a %d-byte migration arena",
                CkMyPe(), d.size);
    }
    grantedLandings[d.id] = arena;
    if (!set.empty()) landingSets[d.id] = std::move(set);
    if (d.size == 0) {
      // Nothing to pull: this landing only reserves the arriving element's
      // blocks, and the element's own message is what waits for it, parked
      // here. Release it THROUGH THE SCHEDULER: immigrate() ends in
      // releaseLandingSet, which admits, and running it inside this loop is
      // the re-entry that once double-granted and double-destroyed a landing.
      // If the grant beat the message, immigrate() finds the id in
      // grantedLandings on arrival and goes straight ahead.
      auto h = bufferedHostMigrateMsgs.find(d.id);
      if (h != bufferedHostMigrateMsgs.end()) {
        CkArrayElementMigrateMessage* m = h->second;
        bufferedHostMigrateMsgs.erase(h);
        thisProxy[CkMyPe()].immigrate(m);
      }
    } else {
      MIGT("grant-send", d.id, nullptr);
      thisProxy[d.srcPe].landingGranted(d.id);
    }
    deferredLandings.pop_front();
  }
}

void CkLocMgr::dispatchGPUMsg(CmiUInt8 id)
{
  if (migDbg())
    CkPrintf("[GPUSEND %d] dispatch id=%llu\n", CkMyPe(), (unsigned long long)id);
  MIGT("dispatch", id, nullptr);
  if (migStatsOn()) {
    sendGPUBuffers[id].dispatchedAt = CkWallTimer();
    if (sendGPUBuffers[id].requestedAt > 0.0) g_mig.grantWait.add(sendGPUBuffers[id].dispatchedAt - sendGPUBuffers[id].requestedAt);
  }
  auto gpuData = sendGPUBuffers[id];

  // No completion callback and no blocking wait. The previous
  // CkCallbackResumeThread parked this (threaded) entry's thread until the
  // whole remote transfer completed, once per migrating chare -- serializing
  // cross-node migrations behind their own transfers. The source pool block's
  // lifetime is instead handled per transport: a same-node staged send
  // registers the block with its IPC event and reclaimCompletedIpcEvents
  // frees it once the receiver has read it; an inter-node send is acked
  // explicitly -- the receiver's immigrateGPU entry runs only after the rget
  // has landed, and its finishGPUSend(id) releases the block here.
  // This send belongs to the RUNTIME, not to whatever element last ran on
  // this PE: _currentLocRec can still name one here, and attributing the
  // payload send to it corrupts that element's migration interlock -- and,
  // with the registration cache on, registers the LB-buffer block against an
  // owner whose own migration then deregisters it mid-transfer, wedging this
  // immigration permanently ("in-flight immigration" that never lands).
  {
    CkLocRec* saved_rec = CkpvAccess(_currentLocRec);
    if (saved_rec != NULL && getenv("CHARM_DEBUG_MIGRATE"))
      CkPrintf("[GPUSEND %d] stale _currentLocRec at payload send (id=%llu)\n",
               CkMyPe(), (unsigned long long)id);
    CkpvAccess(_currentLocRec) = NULL;
    CkRdmaDeviceMarkMigrationPayload(true);
    // Always tagged: a stream-less CkDeviceBuffer now means "complete at send
    // time", and a payload packed with no migration stream was copied on the
    // null stream, which the per-thread stream orders behind.
    thisProxy[gpuData.toPe].immigrateGPU(id, gpuData.size,
      CkDeviceBuffer(gpuData.data, gpuData.size,
                     gpuData.stream ? (hapiStream_t)gpuData.stream : hapiStreamPerThread), CkMyPe());
    CkRdmaDeviceMarkMigrationPayload(false);
    if (migDbg())
      CkPrintf("[GPUSENT %d] id=%llu\n", CkMyPe(), (unsigned long long)id);
    CkpvAccess(_currentLocRec) = saved_rec;
  }

  // Who releases the staged block depends on how the send actually travelled.
  //
  // Same node, different process: the send goes through CUDA IPC, which
  // registers the block against an IPC event, and reclaimCompletedIpcEvents
  // frees it once the receiver sets dst_flag. Hand it over.
  //
  // Same process: the send is a plain device-to-device memcpy. Nothing
  // registers the block with an event, so the reclaim scan can never see it --
  // measured, 213 allocations produced zero reclaims, which exhausted the load
  // balance buffer after two steps at 128MB. Keep the entry and let the
  // receiver's ack free it, as inter-node already does.
  // Under +gpupool the payload is a pool block the IPC reclaim knows nothing
  // about, so the entry stays until the receiver's ack in every case.
  const bool same_process = (CmiNodeOf(CkMyPe()) == CmiNodeOf(gpuData.toPe));
  if (!did_inter_node_gpudirect_rdma(CkMyPe(), gpuData.toPe) && !same_process &&
      !CkDevicePoolOn()) {
    sendGPUBuffers.erase(id);
  }
  // Otherwise the entry stays in sendGPUBuffers until finishGPUSend(id).
}

// Inter-node ack: the receiver's payload has fully arrived, so the packed
// source block this migration was holding can be released.
void CkLocMgr::finishGPUSend(CmiUInt8 id)
{
  MIGT("ack-recv", id, nullptr);
  auto it = sendGPUBuffers.find(id);
  if (it == sendGPUBuffers.end()) return;
  if (migStatsOn() && it->second.dispatchedAt > 0.0)
    g_mig.ship.add(CkWallTimer() - it->second.dispatchedAt);

  GPUManager& csv_gpu_manager = CsvAccess(gpu_manager);
  if (CkDevicePoolOn()) {
    CkDeviceFree(it->second.data);   // the payload was a pool block
    sendGPUBuffers.erase(it);
    ckWindowGiveBack(id, /*onlyIfUnpacked=*/false);
    ck_admit_from = "finishGPUSend";
    if (!deferredLandings.empty()) admitLandings();  // those bytes may be enough
    return;
  } else if (csv_gpu_manager.use_shm) {
    DeviceManager* dm = csv_gpu_manager.device_map[CkMyPe()];
#if CMK_SMP
    CmiLock(dm->lock);
#endif
    dm->free_comm_buffer((size_t)((char*)it->second.data - (char*)dm->comm_buffer->base_ptr));
#if CMK_SMP
    CmiUnlock(dm->lock);
#endif
    // Whoever takes this block next waits on this, instead of the host waiting
    // for the whole device. Staging rides the legacy default stream.
    CkRdmaDeviceNoteLbBufferFreed(dm, (hapiStream_t)0);
  }
  sendGPUBuffers.erase(it);
}
#endif

/// Migrate this local element away to another processor.
// Fast path for intra-process (same CmiNode) migration in SMP mode.
// Transfers ownership of the live CkMigratable objects to the destination PE
// without pup-based serialization. Must be called after ckAboutToMigrate and
// only when there is no GPU-resident data.
bool CkLocMgr::emigrateIntraProcess(CkLocRec* rec, int toPe)
{
  CkArrayIndex idx = rec->getIndex();
  CmiUInt8 id = rec->getID();
  int senderPes[CkLocRec::RECENT_SENDERS];
  const int nSenderPes = rec->nRecentSenders;
  for (int i = 0; i < nSenderPes; i++) senderPes[i] = rec->recentSenders[i];
#if CMK_GPU
  if (rec->stable != nullptr) stableDiscard(rec);   // a handoff packs nothing
#endif

  // Gather (aid, elt) pairs from each manager and detach each chare from
  // this PE's sync barrier, capturing the barrier epoch.
  std::vector<CkGroupID> aids;
  std::vector<uintptr_t> eltPtrs;
  aids.reserve(managers.size());
  eltPtrs.reserve(managers.size());
  int barrierEpoch = -1;

  for (auto itr = managers.begin(); itr != managers.end(); ++itr)
  {
    CkMigratable* elt = itr->second->getEltFromArrMgr(id);
    if (!elt) continue;
    int ep = elt->ckPrepareIntraProcessMigrate();
    if (ep != -1) barrierEpoch = ep;
    aids.push_back(itr->first);
    eltPtrs.push_back(reinterpret_cast<uintptr_t>(elt));
  }

  int nElts = (int)aids.size();
  int locEpoch = cache->getEpoch(id) + 1;

  CkArrayElementIntraProcessMigrateMessage* msg =
      new (nElts, nElts) CkArrayElementIntraProcessMigrateMessage(
          idx, id,
#if CMK_LBDB_ON
          rec->isAsyncMigrate(),
#else
          false,
#endif
          nElts, locEpoch, barrierEpoch);
  for (int i = 0; i < nElts; i++)
  {
    msg->aids[i] = aids[i];
    msg->eltPtrs[i] = eltPtrs[i];
  }
#if CMK_LBDB_ON
  // Ledger-tracked move: the destination acks this PE's LBManager on
  // registration. Consume the tag so the destructor below does not retire it.
  msg->lbLedgerSrcPe = (rec->lbLedgerStep >= 0) ? CkMyPe() : -1;
  msg->lbLedgerStep = rec->lbLedgerStep;
  rec->lbLedgerStep = -1;
#endif

  DEBM((AA "Intra-process migrating index %s to %d\n" AB, idx2str(idx), toPe));

#if CMK_GPU && CMK_GPU_COMM
  // The element's persistent channels move with it unpacked: their entries in
  // this PE's tables go now, and are made again on the destination.
  CkRdmaDeviceChannelsLeaving(rec);
#endif
  thisProxy[toPe].immigrateIntraProcess(msg);

  // Detach from this PE's array managers and location tables. Do NOT delete
  // the CkMigratable objects -- they now live on the destination PE.
  duringMigration = true;
  for (auto itr = managers.begin(); itr != managers.end(); ++itr)
  {
    itr->second->eraseEltFromArrMgr(id);
  }
  removeFromTable(id);
  delete rec;
  duringMigration = false;

  cache->recordEmigration(id, toPe);
  informHome(idx, toPe);
  informSenders(idx, id, senderPes, nSenderPes, toPe);
  // Anything admission control held for this element can now go back through
  // delivery, which will forward it to the new home.
  CkDeviceRecvAdmissionReplay(id);

#if !CMK_LBDB_ON && CMK_GLOBAL_LOCATION_UPDATE
  CmiPrintf((AA "Global location update. idx %s "
           "assigned to %d \n" AB,
        idx2str(idx), toPe));
  thisProxy.updateLocation(id, toPe);
#endif

  CK_MAGICNUMBER_CHECK
  return true;
}

// Receive side of the intra-process migration fast path.
void CkLocMgr::immigrateIntraProcess(CkArrayElementIntraProcessMigrateMessage* msg)
{
  MigTimer _t(&g_mig.intraSecs, &g_mig.intraN);
  const CkArrayIndex& idx = msg->idx;

  if (msg->nManagers > (int)managers.size())
  {
    // Some array managers haven't registered yet. This should be rare during
    // an AtSync-driven migration; fall back to the existing pendingImmigrate
    // list is not yet implemented, so abort loudly to catch it in testing.
    CkAbort("Intra-process immigrate: managers not yet registered on destination PE\n");
  }

  insertID(idx, msg->id);

  CkLocRec* rec = elementNrec(msg->id);
  if (rec == nullptr)
    rec = createLocal(idx, true /* fromMigration */, msg->ignoreArrival,
                      false /* home told on departure */, msg->locEpoch);

  for (int i = 0; i < msg->nManagers; i++)
  {
    auto mit = managers.find(msg->aids[i]);
    if (mit == managers.end())
      CkAbort("Intra-process immigrate: unknown array manager\n");
    CkMigratable* elt = reinterpret_cast<CkMigratable*>(msg->eltPtrs[i]);
    mit->second->putEltInArrMgr(msg->id, elt);
    elt->ckFinalizeIntraProcessMigrate(rec, msg->barrierEpoch);
  }
#if CMK_GPU && CMK_GPU_COMM
  // Its persistent channels register on this PE before any entry method runs.
  CkRdmaDeviceChannelsArrived(rec);
#endif

#if CMK_LBDB_ON
  // The element is registered here: retire its entry in the source's move
  // ledger. This is what holds the source's share of the step's resume.
  if (msg->lbLedgerSrcPe >= 0)
    CProxy_LBManager(_lbmgr)[msg->lbLedgerSrcPe].ledgerRetire(msg->id,
                                                              msg->lbLedgerStep);
#endif

  callMethod(rec, &CkMigratable::ckJustMigrated);
  callMethod(rec, &CkMigratable::lbCheckWaitRelease);

  delete msg;
}

#if CMK_GPU
// Defined with the stable-snapshot code below immigrateGPU.
static hapiStream_t ckMigrationStreamLocked(DeviceManager* dm);
namespace { void ckStableCheckIssue(CkStableSnapshot* snap, CmiUInt8 id, hapiStream_t s); }
#endif

void CkLocMgr::emigrate(CkLocRec* rec, int toPe)
{
  CK_MAGICNUMBER_CHECK
  if (toPe == CkMyPe())
    return;  // You're already there!
  // CHARM_DEBUG_MIGRATE: stage clock for the EMIG line below.
  const double tE0 = CmiWallTimer();
  double tE1 = tE0, tE2 = tE0, tE3 = tE0, tE4 = tE0, tE5 = tE0;
  if (rec->migFirstTryAt == 0.0) rec->migFirstTryAt = tE0;

  // Do not move an element while the runtime is still reading buffers it handed
  // to a zerocopy send: migration frees and reallocates exactly those buffers.
  // Stand the migration down and let noteDeviceSendDone re-drive it when the
  // last transfer completes.
  // Under the split barrier this used to additionally require the element to be
  // parked. It does not: the count above is the safety condition, and the park
  // only guarantees it is reached. An element that never goes quiet while
  // running still moves at its park, which is where AtSyncWait re-drives this.
#if CMK_GPU && CMK_GPU_COMM
  // Persistent channels this element receives on: their senders are told the
  // channel is closing, and each awaited ack counts as an outstanding device
  // send below, so no put lands in a buffer the pack is about to free.
  CkRdmaDeviceChannelsBeforeMigrate(rec);
#endif
  if (rec->outstandingDeviceSends > 0)
  {
    // CHARM_LB_MIGSTATS: what the first deferral waits for -- the element's own puts (in flight on the
    // device or parked for a credit) or its owned channels' unacked closings (counts, in the ms column).
#if CMK_GPU && CMK_GPU_COMM
    if (migStatsOn() && rec->migFirstTryAt == tE0) {
      int own_inflight = 0, own_pending = 0, acks = 0;
      CkRdmaDeviceChannelsStandDownWhy(rec, &own_inflight, &own_pending, &acks);
      g_mig.sdOwnInflight.add(own_inflight * 1e-3); g_mig.sdOwnPending.add(own_pending * 1e-3); g_mig.sdAcks.add(acks * 1e-3);
    }
#endif
    if (getenv("CHARM_DEBUG_MIGRATE"))
      CmiPrintf("[DEFER-MIG %d] elem %s id=%llu count=%d parked=%d toPe=%d\n", CkMyPe(),
                idx2str(rec->getIndex()), (unsigned long long)rec->getID(),
                rec->outstandingDeviceSends, (int)rec->deviceRecvParked, toPe);
    rec->pendingMigrateTo = toPe;
    return;
  }

  // Started only past the stand-down above, so the count is actual emigrations
  // rather than deferral attempts.
  MigTimer _te(&g_mig.emigSecs, &g_mig.emigN);
  if (migStatsOn()) {
    if (rec->migDecidedAt > 0.0) g_mig.decideToTry.add(rec->migFirstTryAt - rec->migDecidedAt);
    g_mig.standDown.add(tE0 - rec->migFirstTryAt);
  }
  // Whom to tell where it went: copied now, the record dies with the pack.
  int senderPes[CkLocRec::RECENT_SENDERS];
  const int nSenderPes = rec->nRecentSenders;
  for (int i = 0; i < nSenderPes; i++) senderPes[i] = rec->recentSenders[i];
#if CMK_GPU
  // What the element holds on the device now, before the pack frees it, as
  // blocks: the destination's admission gate takes exactly these on top of
  // the landing arena, since the unpack allocates them again there.
  std::vector<CmiUInt8> emigBlocks;
  size_t emigFootprint = 0;
  {
    std::vector<std::pair<size_t, int>> blocks;
    hapiObjectBlocks(rec, blocks);
    for (const auto& b : blocks) {
      emigBlocks.push_back((CmiUInt8)b.first);
      emigBlocks.push_back((CmiUInt8)b.second);
      emigFootprint += b.first * (size_t)b.second;
    }
  }
#endif

#if CMK_GPU && CMK_GPU_COMM
  // Those same buffers are about to be freed, so any registration cached for
  // them dies with them. Safe here and only here: the check above is what
  // guarantees nothing is reading them.
  CkRdmaDeviceDropRegistrations(rec);
#endif

  CkArrayIndex idx = rec->getIndex();
  CmiUInt8 id = rec->getID();

#if CMK_OUT_OF_CORE
  /* Load in any elements that are out-of-core */
  for (auto itr = managers.begin(); itr != managers.end(); ++itr)
  {
    CkMigratable* el = itr->second->getEltFromArrMgr(rec->getID());
    if (el)
      if (!el->isInCore)
        CooBringIn(el->prefetchObjID);
  }
#endif

  // Let all the elements know we're leaving
  callMethod(rec, &CkMigratable::ckAboutToMigrate);

  // First pass: find size of migration message
  size_t bufSize, gpuBufSize;
  PUP::sizer p(PUP::er::IS_MIGRATION);
#if CMK_GPU
  // Stable buffers shipped at the decision (recvMigrate -> stableSnapshot)
  // travel in their own payload: the pack walks past them. A snapshot for
  // another destination is a leftover of a decision that changed.
  if (rec->stable != nullptr && rec->stable->toPe != toPe) stableDiscard(rec);
  const bool useStable = (rec->stable != nullptr);
  p.stableSkip = useStable;
#endif
  pupElementsFor(p, rec, CkElementCreation_migrate);
  bufSize = p.size();
  tE1 = CmiWallTimer();

  gpuBufSize = 0;
  size_t nGpuBufs = 0;
  size_t gpuStableBytes = 0;
#if CMK_GPU
  gpuBufSize = p.gpu_size();
  nGpuBufs = p.gpu_buf_count();
  gpuStableBytes = p.gpu_stable_size();
  if (gpuBufSize == 0) {  // nothing to pack after all
    ckClearPendingPack(rec);
    ckWindowGiveBack(id, /*onlyIfUnpacked=*/false);
  }
#endif

  // Fast path: for intra-process (same CmiNode) migration, transfer ownership
  // of the live chare objects to the destination PE without packing/copying.
  // GPU-resident data is only carried along when the destination PE is mapped
  // to the same physical GPU as the source, because raw device pointers are
  // only directly usable in that case.
  if (CmiNodeOf(CkMyPe()) == CmiNodeOf(toPe))
  {
    // Device state of either lifetime makes the device matter.
    bool sameGpuRequirementMet = (gpuBufSize + gpuStableBytes == 0);
#if CMK_GPU
    if (gpuBufSize + gpuStableBytes > 0)
      sameGpuRequirementMet = (hapiDeviceForPe(CkMyPe()) == hapiDeviceForPe(toPe));
#endif
#if CMK_GPU
    // A handoff packs nothing. Released before the call, which consumes rec.
    CkLocRec* handedOff = sameGpuRequirementMet ? rec : nullptr;
    size_t owed = handedOff ? handedOff->pendingPackBytes : 0;
    if (handedOff) ckClearPendingPack(handedOff);
#endif
    if (sameGpuRequirementMet && emigrateIntraProcess(rec, toPe))
    {
#if CMK_GPU
      ckWindowGiveBack(id, /*onlyIfUnpacked=*/false);
#endif
      return;
    }
#if CMK_GPU
    if (handedOff && owed > 0) {  // not handed over after all: it will pack
      handedOff->pendingPackBytes = owed;
      ck_pending_pack_bytes += owed;
    }
#endif
  }

#if CMK_GPU && CMK_GPU_COMM
  // The channels' exports and registrations go with the buffers the pack is
  // about to free; the element reopens them on its new buffers in its pup. A
  // handoff above kept them (same process, same buffer) and moved only the
  // per-PE registrations.
  CkRdmaDeviceChannelsPack(rec);
#endif

  // Anything the intra-process handoff above did not take stages from here --
  // including a move within this process onto a different GPU. See the note on
  // migration transports below for why, and what supporting that case directly
  // would require.

#if CMK_ERROR_CHECKING
  if (bufSize > std::numeric_limits<int>::max())
  {
    CkAbort("Cannot migrate an object with size greater than %d bytes!\n",
            std::numeric_limits<int>::max());
  }
#endif


  void* gpuMsg = nullptr;
#if CMK_GPU
  hapiStream_t migStream = NULL;
#endif
  // Allocate and pack into message
  CkArrayElementMigrateMessage* msg =
      new (bufSize, nGpuBufs, 0) CkArrayElementMigrateMessage(idx, id,
#if CMK_LBDB_ON
                                                    rec->isAsyncMigrate(),
#else
                                                    false,
#endif
                                                    bufSize, managers.size(),
                                                    cache->getEpoch(id) + 1,
                                                    gpuBufSize > 0,
                                                    (int)nGpuBufs);
#if CMK_LBDB_ON
  // Ledger-tracked move: the destination acks this PE's LBManager on
  // registration. Consume the tag so the destructor does not retire it.
  msg->lbLedgerSrcPe = (rec->lbLedgerStep >= 0) ? CkMyPe() : -1;
  msg->lbLedgerStep = rec->lbLedgerStep;
  rec->lbLedgerStep = -1;
#endif

#if CMK_GPU
  // The admission gate hung entirely off gpuBufSize > 0, so an element with
  // device state but nothing to copy out skipped it and landed with no
  // reservation: its unpack allocated from the arenas with growth allowed,
  // behind the gate's back. sph2d is the case in point -- part_capacity is a
  // readonly, so a patch holding no particles still allocates 74 MB on arrival,
  // and those are most of what a step moves. Such an element takes the same
  // road as a payload-bearing one, less the payload.
  const bool needsLanding =
      ckAdmissionGateOn() && gpuBufSize == 0 && !emigBlocks.empty();
  msg->needsLanding = needsLanding;
#endif

  {
#if CMK_GPU
    if (gpuBufSize > 0) {
      GPUManager& csv_gpu_manager = CsvAccess(gpu_manager);
      if(csv_gpu_manager.use_shm) {
        DeviceManager* dm = csv_gpu_manager.device_map[CkMyPe()];
        if (CkDevicePoolOn()) {
          // The packed payload is a pool block: exported through its arena's
          // handle like any other pool buffer, sent direct, and returned to
          // the pool by finishGPUSend on the receiver's ack. The runtime's,
          // not the departing element's, whatever is running.
          hapiFootprintBegin(nullptr);
          gpuMsg = (char*)CkDeviceMalloc(gpuBufSize);
          hapiFootprintEnd();
          if (gpuMsg == nullptr)
            CkAbort("PE %d: device pool could not provide a %zu-byte migration "
                    "payload (+gpupoolsize)", CkMyPe(), gpuBufSize);
          // Packed: the floor no longer needs to keep these bytes free.
          ckClearPendingPack(rec);
          {
            auto w = ck_window.inflight.find(id);  // the ack gives it back now
            if (w != ck_window.inflight.end()) w->second.packed = true;
          }
        } else {
          // Takes dm->lock itself, and reclaims retired IPC slots before giving
          // up -- this region is only ever freed by that scan.
          gpuMsg = CkRdmaDeviceAllocLbBuffer(dm, gpuBufSize);
          if (gpuMsg == nullptr) {
            CkAbort("PE %d, device %d: Not enough memory on device Load balance "
                    "buffer (%zu free). This buffer stages device state pupped "
                    "with pup_buffer_device and is 0 by default -- size it with "
                    "+gpulbbuffer <MB>.",
                CkMyPe(), dm->global_index, dm->get_lb_buffer_free_size());
          }
        }
#if CMK_SMP
        CmiLock(dm->lock);
#endif
        // Pack copies go on one of the device's migration streams, not the
        // null stream (see DeviceManager::migration_streams). Lazy creation
        // is serialized by this lock.
        migStream = ckMigrationStreamLocked(dm);
#if CMK_SMP
        CmiUnlock(dm->lock);
#endif
      }
    }
#endif
    tE2 = CmiWallTimer();
    PUP::toMem p(msg->packData, gpuMsg, PUP::er::IS_MIGRATION);
#if CMK_GPU
    p.gpuStream = (void*)migStream;
    p.deviceManifestOut = msg->gpuManifest;
    p.deviceManifestCap = nGpuBufs;
    p.stableSkip = useStable;
    // Hashed again here, on the pack stream, for the check against the
    // snapshot's hashes (ckStableCheckIssue below).
    p.stableHashOut = (useStable && rec->stable->hashes != nullptr && migStream != NULL)
                          ? rec->stable->hashes + rec->stable->count : nullptr;
#endif
    p.becomeDeleting();
    pupElementsFor(p, rec, CkElementCreation_migrate);
    tE3 = CmiWallTimer();
    if (p.size() != bufSize)
    {
      CkError(
          "ERROR! Array element claimed it was %zu bytes to a "
          "sizing PUP::er, but copied %zu bytes into the packing PUP::er!\n",
          bufSize, p.size());
      CkAbort("Array element's pup routine has a direction mismatch.\n");
    }
#if CMK_GPU
    // The same check for the device stream. Both totals matter and neither is
    // implied by the host size: gpu_size() sizes the device region the pack
    // copies into, and gpu_buf_count() sizes msg->gpuManifest, a host array
    // inside this message. Under-counting either one is a buffer overflow --
    // of device memory, or of the heap -- that shows up much later as
    // unrelated corruption, so catch it where the mismatch actually is.
    // Overflow only: a pass that packs *less* than it sized wastes room but
    // corrupts nothing, and the sizer may legitimately round up trailing
    // alignment. Packing more is the bug worth aborting on.
    if (p.gpu_size() > gpuBufSize || p.deviceManifestIdx > nGpuBufs)
    {
      CkError(
          "ERROR! Array element claimed %zu device bytes in %zu buffers to a "
          "sizing PUP::er, but packed %zu bytes in %zu buffers!\n",
          gpuBufSize, nGpuBufs, p.gpu_size(), p.deviceManifestIdx);
      CkAbort("Array element's pup routine has a device direction mismatch.\n");
    }
    if (useStable) {
      CkStableSnapshot* snap = rec->stable;
      if (p.stableHashOut != nullptr && p.stableHashIdx != snap->count)
        CkAbort("Array element's pup routine declared %zu Stable device buffers at the "
                "balancer's decision but %zu at the pack.\n", snap->count, p.stableHashIdx);
      // The verdict comes by callback once the device has compared the hashes.
      if (p.stableHashOut != nullptr) ckStableCheckIssue(snap, id, migStream);
      else if (snap->hashes != nullptr) CkDeviceFree(snap->hashes);
      if (snap->done != nullptr) hapiCheck(hapiEventDestroy((hapiEvent_t)snap->done));
      msg->hasStable = true;
      msg->stableGen = snap->gen;
      msg->stableBytes = (int)snap->bytes;
      msg->stableCount = (int)snap->count;
      if (migStatsOn()) g_mig.snapshotLead.add(tE2 - snap->takenAt);
      delete snap;
      rec->stable = nullptr;
    }
    if (migStatsOn() && migStream != NULL) {
      struct PackStat { double t0; static void fn(void* a, void*) {
        PackStat* st = (PackStat*)a; g_mig.pack.add(CkWallTimer() - st->t0); delete st; } };
      hapiAddCallback(migStream, CkCallback(PackStat::fn, (void*)new PackStat{tE2}));
    }
#endif
  }

  DEBM((AA "Migrated index size %s to %d \n" AB, idx2str(idx), toPe));

#if CMK_GPU
  // Complete the pack copies before destroying elements. On the migration
  // stream this waits for exactly those copies instead of the whole device;
  // the device-wide fallback covers the no-pool path, whose packing still
  // used the null stream (and cudaMemcpy(D2D) is async in CUDA 12.x).
  if (gpuBufSize > 0) {
    if (migStream != NULL) {
      // The wait covered two things: the sources must not be freed under the
      // copies, and the payload send must not stage before they finish. When
      // every packed buffer came from the device pool, both are ordered on the
      // device instead -- the pool parks a freed block until the reads it was
      // told about retire, and the send below is tagged with this stream -- so
      // the host does not have to stop. A buffer from anywhere else has no
      // such ordering, and the wait stays exactly as it was.
      if (!p.deviceAllPool) {
        const double tw0 = CmiWallTimer();
        hapiCheck(ckSubmitDrainThenSync(migStream));
        if (migDbg())
          CkPrintf("[EMIG %d] id=%llu pack wait %.3f ms (non-pool source)\n",
                   CkMyPe(), (unsigned long long)id,
                   (CmiWallTimer() - tw0) * 1e3);
      }
    } else {
      hapiSubmitDrain();   // cover copies still in a submitter ring
      hapiDeviceSynchronize();
    }
  }
#endif

  thisProxy[toPe].immigrate(msg);
  tE4 = CmiWallTimer();

  duringMigration = true;
  for (auto itr = managers.begin(); itr != managers.end(); ++itr)
  {
    itr->second->deleteElt(id);
  }
  duringMigration = false;

  tE5 = CmiWallTimer();
  cache->recordEmigration(id, toPe);
  informHome(idx, toPe);
  informSenders(idx, id, senderPes, nSenderPes, toPe);
#if CMK_GPU
  if (gpuBufSize > 0)
  {
    sendGPUBuffers[id] = GPUMigrateData(toPe, gpuBufSize, gpuMsg, (void*)migStream,
                                        emigFootprint, std::move(emigBlocks));
    if (migDbg() && migStream != NULL) {
      // when the pack copies of this element have completed on the migration stream
      struct PackDone { static void fn(void* p, void*) {
        CmiPrintf("[PACKDONE %d] id=%llu\n", CkMyPe(), (unsigned long long)(uintptr_t)p); } };
      hapiAddCallback((hapiStream_t)migStream, CkCallback(PackDone::fn, (void*)(uintptr_t)id));
    }
    thisProxy[CkMyPe()].sendGPUMsg(id);
  }
  else if (needsLanding)
  {
    // Size 0 says "reserve the blocks, there is nothing to pull". No payload
    // means no sendGPUBuffers entry and no landingGranted round trip: the
    // destination grants to itself and releases the element's own message.
    thisProxy[toPe].requestLanding(id, 0, CkMyPe(), (int)emigBlocks.size(),
                                   emigBlocks.data());
  }
  if (getenv("CHARM_DEBUG_MIGRATE"))
    CmiPrintf("[EMIG %d] id=%llu gpuBufSize=%zu toPe=%d stages_ms sizer=%.3f alloc=%.3f "
              "pack=%.3f send=%.3f delete=%.3f tail=%.3f\n", CkMyPe(),
              (unsigned long long)id, (size_t)gpuBufSize, toPe,
              (tE1 - tE0) * 1e3, (tE2 - tE1) * 1e3, (tE3 - tE2) * 1e3,
              (tE4 - tE3) * 1e3, (tE5 - tE4) * 1e3, (CmiWallTimer() - tE5) * 1e3);
#endif
  // Anything admission control held for this element goes back through
  // delivery, which forwards it to the new home.
  //
  // This runs AFTER the GPU payload is dispatched, and the order is load
  // bearing. Both the replay and sendGPUMsg go onto this PE's own queue,
  // which the scheduler drains ahead of the network. Replaying first puts
  // every message the element buffered while parked in front of the send
  // that ships its device buffers, so the destination waits forever for a
  // payload that is stuck behind traffic addressed to the element that
  // already left -- the migration never completes, its PE never reports in,
  // and the balancer's resume reduction hangs with it.
  CkDeviceRecvAdmissionReplay(id);

#if CMK_GLOBAL_LOCATION_UPDATE
  // Every PE already recorded this move before it was acted on (UpdateLocation,
  // from the balancer's decision), with an epoch counted from its own cache. This
  // broadcast carries the element's true epoch (the entry recordEmigration just
  // advanced), so a bystander whose count fell short cannot have the entry
  // overwritten by an older reply still in flight. The home was told above and
  // ignores the duplicate by epoch. Cost: one group broadcast per migration, in
  // this build mode only.
  DEBM((AA "Global location update. idx %s assigned to %d \n" AB, idx2str(idx), toPe));
  thisProxy.updateLocation(idx, cache->getLocationEntry(id));
#endif

  CK_MAGICNUMBER_CHECK
}

#if CMK_LBDB_ON
void CkLocMgr::informLBPeriod(CkLocRec* rec, int lb_ideal_period)
{
  callMethod(rec, &CkMigratable::recvLBPeriod, (void*)&lb_ideal_period);
}

void CkLocMgr::metaLBCallLB(CkLocRec* rec)
{
  callMethod(rec, &CkMigratable::metaLBCallLB);
}
#endif

// Device migration transports: staged, plus a same-process pointer handoff.
//
// emigrateIntraProcess hands the live chare over when source and destination
// PEs are in one process AND on one GPU. Everything else stages: the chare's
// device state is packed into a pool block, the chare is destroyed at pack, and
// the block travels as an ordinary device zerocopy payload.
//
// Same process but a different GPU therefore stages too, at the cost of one
// device-to-device copy. Supporting it directly -- the destination reading the
// source's buffers where they lie -- was tried and removed, because it is not
// merely a pointer handoff: a cross-GPU read is asynchronous, so the source
// chare has to outlive it. Reinstating it needs all of:
//
//   - a pack that records the chare's device buffers as (ptr,size) instead of
//     copying them (a collector hook on PUP::er),
//   - a message carrying those handles to the destination,
//   - a deferred-destruction table on the source, since the element must not be
//     deleted at emigrate,
//   - an ack from the destination once its copies retire, to drive that table,
//   - an explicit erase from CkpvAccess(array_objs) at emigrate: ~ArrayElement
//     normally does it, and an undestroyed element would otherwise keep
//     receiving messages through the PE-level fast path, and
//   - accounting in the LB memory contract, which must treat the move as a held
//     transport debiting both ends -- see heldTransport(), whose held/staged
//     line has to keep matching whatever this file routes.
//
// The reason it is not worth it by default is the last two: holding device
// memory until a peer acks is unbounded in the number of concurrent migrations,
// while staging frees the source at pack and keeps a step's footprint inside a
// reserve the balancer can plan against.

#if CMK_GPU
// Arena mode (CHARM_MIGRATE_ARENA): the staged payload lands in one ordinary
// device allocation that becomes the arriving chare's permanent storage --
// pup_buffer_device rebinds pointers into it at unpack, and nothing is
// copied. The arena needs only the payload's size, which this message
// carries, so it works with either arrival order of host message and
// payload. Off by default while experimental.
// +gpupool implies it: the landing arena is the chare's storage.
static inline bool ckMigrateArenaMode()
{
  static const bool on = (getenv("CHARM_MIGRATE_ARENA") != nullptr) || CkDevicePoolOn();
  return on;
}
// Whether a migration arena comes from the device pool (CkDeviceMalloc) and goes
// back to it, instead of hapiMalloc/hapiFree: under +gpupool. The pool is
// optional, and without it this path keeps working exactly as before.
static inline bool ckMigrateArenaFromPool() { return CkDevicePoolOn(); }

// Per-PE non-blocking stream for migration payload landings in arena mode.
static hapiStream_t ckLandingStream()
{
  static thread_local hapiStream_t s = nullptr;
  if (s == nullptr) hapiCheck(hapiStreamCreateWithFlags(&s, hapiStreamNonBlocking));
  return s;
}

void CkLocMgr::immigrateGPU(CmiUInt8& id, int& size, char* &data, int& srcPe, CkDeviceBufferPost* post)
{
  // Only set on the pooled path; arena mode allocates fresh memory that
  // cannot alias anything, so there is nothing to gate against.
  void* dm_for_gate = NULL;
  //CkPrintf("PE %d allocating GPU memory size %d for id %llu\n", CkMyPe(), size, id);
  GPUManager& csv_gpu_manager = CsvAccess(gpu_manager);
  if (ckMigrateArenaMode()) {
    // The arena: the chare's storage, not a landing block, so it comes from
    // ordinary device memory -- pool memory is transient by contract.
    // Under +gpupool the arena comes from the device pool (host-side buddy
    // allocation, no cudaMalloc and later no cudaFree) rather than a fresh
    // device allocation; without the pool it is the allocation it always was.
    data = nullptr;
    // Charged to nobody here; immigrate charges it to the element it becomes
    // storage for, once that element exists.
    auto granted = grantedLandings.find(id);
    hapiFootprintBegin(nullptr);
    if (granted != grantedLandings.end()) {
      data = granted->second;  // taken when the admission gate granted it
      grantedLandings.erase(granted);
    } else if (ckMigrateArenaFromPool()) {
      data = (char*)CkDeviceMalloc(size);
      if (data == nullptr)
        CkAbort("PE %d: device pool could not provide a %d-byte migration arena",
                CkMyPe(), size);
    } else {
      hapiCheck(hapiMalloc((void**)&data, size));
    }
    hapiFootprintEnd();
  } else if(csv_gpu_manager.use_shm) {
    DeviceManager* dm = csv_gpu_manager.device_map[CkMyPe()];
    dm_for_gate = dm;
    data = (char*)CkRdmaDeviceAllocLbBuffer(dm, size);
    if (data == nullptr) {
      CkAbort("PE %d, device %d: Not enough memory on device Load balance "
              "buffer (%zu free). This buffer stages device state pupped with "
              "pup_buffer_device and is 0 by default -- size it with "
              "+gpulbbuffer <MB>.",
          CkMyPe(), dm->global_index, dm->get_lb_buffer_free_size());
    }
  }
  // What has to be ordered here, and what does not.
  //
  // This is the post function: it runs before the landing copy is issued.
  // The copy goes on the legacy default stream (set below), and the runtime
  // invokes the receiving entry method as a callback on that same stream, so
  // by the time the payload is unpacked -- and long before the chare's first
  // kernel on its own, non-blocking stream -- the write has completed. Nothing
  // downstream needs a wait for the data to arrive; the callback is that wait.
  //
  // What can go wrong is the opposite direction: writing the landing block
  // while something is still reading it. That is only possible when the block
  // is recycled, which only the load-balancing region of the comm buffer does.
  // Every path that returns a block there already retires the device work
  // against it (the IPC reclaim queries the transfer's event, finishGPUSend
  // waits for the receiver's ack) except the free after unpack, whose reads
  // ride the migration stream -- so CkRdmaDeviceNoteLbBufferFreed records an
  // event on that stream at the free, and the gate below makes the landing
  // stream wait on exactly those events. The host wait is the older, stronger
  // form of the same thing: stop the host until the legacy stream drains,
  // which also drains every other PE's landing in the process.
  //
  // Arena mode has no recycled block at all. The arena is fresh device memory,
  // or a pool block the pool would not hand out until the reads it was told
  // about had retired. So there is nothing to order and no wait of either kind
  // (the gate has no device manager to look up, and returns at once).
  // Measured on 4 A40s, arena mode, -lblag 8, direct IPC, 32 paired runs
  // alternating host wait and no wait: 32 of 32 exact each way.
  //
  // CHARM_LB_POOL_EVENT_GATE is the recycled-block gate's opt-in; without it
  // the recycled-block path keeps the host wait, which is the behaviour that
  // has never failed.
  //
  // Note for anyone debugging in this window: added synchronization hides the
  // failure, so probes placed inside it are worthless. Use failure-path-only
  // probes.
  static const bool poolEventGate =
      (getenv("CHARM_LB_POOL_EVENT_GATE") != nullptr);
  // Arena mode lands on a per-PE non-blocking stream. Stream 0 is one per
  // process and FIFO, so every landing of a burst -- ~40 per PE, 8 PEs --
  // queued behind every earlier one, each also waiting on its sender's
  // cross-process event: 245 ms median from dispatch to receipt on leanmd.
  // Arena mode has nothing to order (see above); the recycled-block modes
  // keep stream 0 and their gate.
  hapiStream_t landing = (hapiStream_t)0;
  if (dm_for_gate != NULL && !poolEventGate)
    hapiCheck(hapiStreamSynchronize((hapiStream_t)0));
  else if (dm_for_gate != NULL)
    CkRdmaDeviceGateLbBuffer(dm_for_gate, (hapiStream_t)0);
  else
    landing = ckLandingStream();
  receivedDeviceMsgs[id] = data;
  post[0].hapi_stream = landing;
  if (migDbg())
    CkPrintf("[GPUPOST %d] id=%llu size=%d srcPe=%d\n", CkMyPe(),
             (unsigned long long)id, size, srcPe);
  if (migStatsOn()) payloadArrivedAt[id] = CkWallTimer();   // the envelope is here; the rget follows
  MIGT("arrive", id, data);
}



void CkLocMgr::immigrateGPU(CmiUInt8 id, int size, char* data, int srcPe)
{
  MIGT("entry", id, data);
  if (migStatsOn()) {
    auto ra = payloadArrivedAt.find(id);
    if (ra != payloadArrivedAt.end()) { g_mig.rxLand.add(CkWallTimer() - ra->second); payloadArrivedAt.erase(ra); }
  }
  if (migDbg())
    CkPrintf("[GPURECV %d] id=%llu size=%d srcPe=%d\n", CkMyPe(),
             (unsigned long long)id, size, srcPe);
  // This entry runs only after the device payload has fully landed, so an
  // inter-node sender's packed source block is safe to release now. Same-node
  // sends release theirs through the IPC event reclaim instead.
  // Ack for the two cases where the sender still holds the staged block:
  // inter-node, and same-process, whose memcpy send registers no IPC event so
  // nothing else would ever free it.
  // Under +gpupool every payload is a pool block released by this ack, in
  // all three cases: the direct same-node send claims an event slot but the
  // reclaim frees nothing for it.
  if (srcPe >= 0 && (CkDevicePoolOn() ||
                     did_inter_node_gpudirect_rdma(srcPe, CkMyPe()) ||
                     CmiNodeOf(srcPe) == CmiNodeOf(CkMyPe())))
    thisProxy[srcPe].finishGPUSend(id);

  void* dataPtr = receivedDeviceMsgs[id];
  receivedDeviceMsgs.erase(id);
  bufferedDeviceMigrateMsgs[id] = dataPtr;
  if (migStatsOn()) payloadLandedAt[id] = CkWallTimer();
  // Taken out of the buffer BEFORE immigrate runs: it may park the message
  // again (for a stable landing still on its way), and an erase afterwards
  // would drop that.
  auto h = bufferedHostMigrateMsgs.find(id);
  if (h != bufferedHostMigrateMsgs.end())
  {
    CkArrayElementMigrateMessage* m = h->second;
    bufferedHostMigrateMsgs.erase(h);
    immigrate(m);
  }
}

// ---- Stable device buffers shipped at the balancer's decision ----
//
// recvMigrate calls stableSnapshot when a deferred move is ordered: a sizing
// walk of the element finds its Stable buffers (PUP::DeviceLife::Stable),
// a snapshot walk copies them into one pool block laid out as the unpack's
// stable region, hashes each, and the block goes to the destination as a
// zerocopy payload tagged with the copy stream. The element keeps stepping.
// At the pack, emigrate walks past those buffers (manifest entries only).
// Under CHARM_DEBUG_MIGRATE each is hashed at both points and the device
// compares; a change aborts the run naming the buffer (the application broke
// its promise). Otherwise nothing is hashed. The destination keeps the landed
// block until the element's host message arrives and rebinds the buffers into
// it, as the main arena is rebound.

// A migration stream of this PE's device, or NULL when the shared-memory
// device path (and with it the pool) is off.
static hapiStream_t ckMigrationStreamLocked(DeviceManager* dm)
{
  const int i = dm->migration_stream_next;
  dm->migration_stream_next = (i + 1) % DeviceManager::MIGRATION_STREAMS;
  if (dm->migration_streams[i] == NULL) hapiCheck(hapiStreamCreate(&dm->migration_streams[i]));
  if (dm->migration_stream == NULL) dm->migration_stream = dm->migration_streams[0];
  return dm->migration_streams[i];
}
static hapiStream_t ckMigrationStream()
{
  GPUManager& g = CsvAccess(gpu_manager);
  if (!g.use_shm) return NULL;
  DeviceManager* dm = g.device_map[CkMyPe()];
  if (dm == NULL) return NULL;
#if CMK_SMP
  CmiLock(dm->lock);
#endif
  hapiStream_t s = ckMigrationStreamLocked(dm);
#if CMK_SMP
  CmiUnlock(dm->lock);
#endif
  return s;
}

// The check's verdict ring: pinned words the device compare writes through,
// one per check in flight on this PE, read by the stream callback.
namespace {
enum { CK_STABLE_FLAGS = 1024 };
struct CkStableFlagRing {
  unsigned long long* flags = nullptr;
  char busy[CK_STABLE_FLAGS];
  int next = 0;
};
thread_local CkStableFlagRing ck_stable_ring;
struct CkStableCheck {
  unsigned long long* hashes;
  size_t count;
  CmiUInt8 id;
  int slot;
};
void ckStableCheckDone(void* arg, void*)
{
  CkStableCheck* c = (CkStableCheck*)arg;
  const unsigned long long flag = ck_stable_ring.flags[c->slot];
  ck_stable_ring.busy[c->slot] = 0;
  CkDeviceFree(c->hashes);
  if (flag != ~0ULL)
    CkAbort("PE %d: element %llu declared device buffer #%llu of its %zu Stable buffers as "
            "PUP::DeviceLife::Stable, but it changed between the balancer's decision and the "
            "pack; the snapshot shipped at the decision is not the state that left.",
            CkMyPe(), (unsigned long long)c->id, (unsigned long long)flag, c->count);
  delete c;
}
void ckStableCheckIssue(CkStableSnapshot* snap, CmiUInt8 id, hapiStream_t s)
{
  CkStableFlagRing& r = ck_stable_ring;
  if (r.flags == nullptr) {
    hapiCheck(hapiMallocHost((void**)&r.flags, CK_STABLE_FLAGS * sizeof(unsigned long long)));
    memset(r.busy, 0, sizeof(r.busy));
  }
  int slot = -1;
  for (int k = 0; k < CK_STABLE_FLAGS; k++) {
    const int i = (r.next + k) % CK_STABLE_FLAGS;
    if (!r.busy[i]) { slot = i; break; }
  }
  if (slot < 0) {   // every slot has a verdict pending: this one goes unchecked
    g_mig.stableChecksSkipped.fetch_add(1, std::memory_order_relaxed);
    CkDeviceFree(snap->hashes);
    if (snap->done != nullptr) { hapiCheck(hapiEventDestroy((hapiEvent_t)snap->done)); snap->done = nullptr; }
    return;
  }
  r.next = (slot + 1) % CK_STABLE_FLAGS;
  r.busy[slot] = 1;
  unsigned long long* flag = snap->hashes + 2 * snap->count;
  if (snap->done != nullptr) {
    hapiCheck(hapiStreamWaitEvent((hapiStream_t)s, (hapiEvent_t)snap->done, 0));
    hapiCheck(hapiEventDestroy((hapiEvent_t)snap->done));   // the wait captured it
    snap->done = nullptr;
  }
  hapiHashCompareAsync(snap->hashes, snap->hashes + snap->count, snap->count, flag, s);
  hapiCheck(hapiMemcpyAsync(&r.flags[slot], flag, sizeof(unsigned long long),
                            hapiMemcpyDeviceToHost, (hapiStream_t)s));
  CkStableCheck* c = new CkStableCheck{snap->hashes, snap->count, id, slot};
  g_mig.stableChecks.fetch_add(1, std::memory_order_relaxed);
  hapiAddCallback(s, CkCallback(ckStableCheckDone, (void*)c));
}
}  // namespace

void CkLocMgr::stableSnapshot(CkLocRec* rec, int toPe)
{
  if (toPe == CkMyPe()) return;
  if (!ckMigrateArenaMode() || !CkDevicePoolOn()) return;   // the rebind needs the arena, the block the pool
  // A handoff packs nothing: same process, same device.
  if (CmiNodeOf(CkMyPe()) == CmiNodeOf(toPe) &&
      hapiDeviceForPe(CkMyPe()) == hapiDeviceForPe(toPe)) return;
  if (rec->stable != nullptr) {
    if (rec->stable->toPe == toPe) return;   // the same move, re-entered
    stableDiscard(rec);
  }
  // What is Stable, and how much: a sizing walk, which the element sees as
  // the sizing pass it already sees at every balancing step.
  PUP::sizer sz(PUP::er::IS_MIGRATION);
  pupElementsFor(sz, rec, CkElementCreation_migrate);
  const size_t bytes = sz.gpu_stable_size();
  const size_t count = sz.gpu_stable_count();
  if (bytes == 0) return;
  hapiStream_t s = ckMigrationStream();
  if (s == NULL) return;
  hapiFootprintBegin(nullptr);   // the runtime's, until the destination charges it to the element
  void* block = CkDeviceMalloc(bytes);
  // The promise is checked only under CHARM_DEBUG_MIGRATE: each Stable buffer
  // hashed here and again at the pack, compared on the device, a change
  // aborting the run. Off, nothing is hashed.
  unsigned long long* hashes = migDbg()
      ? (unsigned long long*)CkDeviceMalloc((2 * count + 1) * sizeof(unsigned long long))
      : nullptr;
  hapiFootprintEnd();
  if (block == nullptr) {   // no room now: the pack ships them as it always did
    if (hashes != nullptr) CkDeviceFree(hashes);
    return;
  }
  CkStableSnapshot* snap = new CkStableSnapshot;
  snap->toPe = toPe;
  snap->gen = ++rec->stableGen;
  snap->block = block;
  snap->bytes = bytes;
  snap->count = count;
  snap->manifest.resize(count);
  snap->hashes = hashes;
  snap->done = nullptr;
  snap->takenAt = CkWallTimer();
  PUP::snapshot p(block, PUP::er::IS_MIGRATION);
  p.gpuStream = (void*)s;
  p.manifestOut = snap->manifest.data();
  p.manifestCap = count;
  p.hashOut = hashes;   // null unless debugging
  pupElementsFor(p, rec, CkElementCreation_migrate);
  if (p.gpu_copied() > bytes || p.manifestIdx != count)
    CkAbort("PE %d: element %llu sized %zu Stable device bytes in %zu buffers but its snapshot "
            "walk copied %zu bytes in %zu buffers (pup direction mismatch)",
            CkMyPe(), (unsigned long long)rec->getID(), bytes, count, p.gpu_copied(), p.manifestIdx);
  // A source outside the pool has no stream-ordered free: its copies must be
  // done before anything can free it. Rare (the pool is the backend here).
  if (!p.deviceAllPool) hapiCheck(hapiStreamSynchronize((hapiStream_t)s));
  // The pack's compare runs on whichever migration stream the pack takes:
  // it waits on this event for the snapshot's hashes.
  if (hashes != nullptr) {
    hapiEvent_t ev = nullptr;
    hapiCheck(hapiEventCreateWithFlags(&ev, hapiEventDisableTiming));
    hapiCheck(hapiEventRecord(ev, (hapiStream_t)s));
    snap->done = (void*)ev;
  }
  rec->stable = snap;
  stableSends[std::make_pair(rec->getID(), snap->gen)] = StableSend{block, bytes, toPe, snap->gen};
  g_mig.snapshots.fetch_add(1, std::memory_order_relaxed);
  g_mig.snapshotBytes.fetch_add((long)bytes, std::memory_order_relaxed);
  if (migDbg())
    CkPrintf("[STABLE %d] id=%llu gen %d: %zu bytes in %zu buffers snapshotted for pe %d\n",
             CkMyPe(), (unsigned long long)rec->getID(), snap->gen, bytes, count, toPe);
  // Shipped as the migration payload is: the runtime's send, not the
  // element's, tagged with the copy stream so it waits for the copies.
  {
    CkLocRec* saved_rec = CkpvAccess(_currentLocRec);
    CkpvAccess(_currentLocRec) = NULL;
    CkRdmaDeviceMarkMigrationPayload(true);
    thisProxy[toPe].immigrateStable(rec->getID(), (int)bytes, CkDeviceBuffer(block, bytes, s),
                                    CkMyPe(), snap->gen);
    CkRdmaDeviceMarkMigrationPayload(false);
    CkpvAccess(_currentLocRec) = saved_rec;
  }
}

// The snapshot is not going to be used by the move it was taken for: the
// destination frees its landing (when it has landed), the source block goes
// back at the destination's ack as usual.
void CkLocMgr::stableDiscard(CkLocRec* rec)
{
  CkStableSnapshot* snap = rec->stable;
  if (snap == nullptr) return;
  rec->stable = nullptr;
  if (migDbg())
    CkPrintf("[STABLE %d] id=%llu gen %d: snapshot for pe %d discarded\n", CkMyPe(),
             (unsigned long long)rec->getID(), snap->gen, snap->toPe);
  thisProxy[snap->toPe].discardStable(rec->getID(), snap->gen);
  if (snap->hashes != nullptr) CkDeviceFree(snap->hashes);
  if (snap->done != nullptr) hapiCheck(hapiEventDestroy((hapiEvent_t)snap->done));
  delete snap;
}

// Destination, post function: the landing block for the stable region.
void CkLocMgr::immigrateStable(CmiUInt8& id, int& size, char*& data, int& srcPe, int& gen,
                               CkDeviceBufferPost* post)
{
  (void)srcPe;
  hapiFootprintBegin(nullptr);   // charged to the element once it is here
  data = (char*)CkDeviceMalloc((size_t)size);
  hapiFootprintEnd();
  if (data == nullptr)
    CkAbort("PE %d: device pool could not provide a %d-byte stable landing for element %llu",
            CkMyPe(), size, (unsigned long long)id);
  receivedStableMsgs[std::make_pair(id, gen)] = data;
  post[0].hapi_stream = ckLandingStream();
}

// Destination: the stable region has landed.
void CkLocMgr::immigrateStable(CmiUInt8 id, int size, char* data, int srcPe, int gen)
{
  (void)data;
  thisProxy[srcPe].finishStableSend(id, gen);   // the source block may go back
  char* block = receivedStableMsgs[std::make_pair(id, gen)];
  receivedStableMsgs.erase(std::make_pair(id, gen));
  auto d = stableDiscarded.find(id);
  if (d != stableDiscarded.end() && d->second == gen) {
    stableDiscarded.erase(d);
    CkDeviceFree(block);
    return;
  }
  auto old = stableLandings.find(id);
  if (old != stableLandings.end()) {   // an older generation nobody claimed
    CkDeviceFree(old->second.block);
    stableLandings.erase(old);
  }
  stableLandings[id] = StableLanding{block, size, gen};
  if (migDbg())
    CkPrintf("[STABLE %d] id=%llu gen %d: %d bytes landed from pe %d\n", CkMyPe(),
             (unsigned long long)id, gen, size, srcPe);
  auto h = bufferedHostMigrateMsgs.find(id);
  if (h != bufferedHostMigrateMsgs.end() && h->second->hasStable && h->second->stableGen == gen)
  {
    CkArrayElementMigrateMessage* m = h->second;
    bufferedHostMigrateMsgs.erase(h);
    immigrate(m);
  }
}

void CkLocMgr::finishStableSend(CmiUInt8 id, int gen)
{
  auto it = stableSends.find(std::make_pair(id, gen));
  if (it == stableSends.end()) return;
  CkDeviceFree(it->second.block);
  stableSends.erase(it);
}

void CkLocMgr::discardStable(CmiUInt8 id, int gen)
{
  auto it = stableLandings.find(id);
  if (it != stableLandings.end() && it->second.gen == gen) {
    CkDeviceFree(it->second.block);
    stableLandings.erase(it);
    return;
  }
  stableDiscarded[id] = gen;   // not landed yet: freed when it does
}
#endif

// The element left this PE for toPe: its recent senders learn so directly,
// instead of sending to this PE once more and taking the forward and the
// repair. The home PE and the destination already know.
void CkLocMgr::informSenders(const CkArrayIndex& idx, CmiUInt8 id, const int* pes, int n, int toPe)
{
  if (n == 0) return;
  const int home = homePe(idx);
  const CkLocEntry e = cache->getLocationEntry(id);
  for (int i = 0; i < n; i++)
  {
    const int pe = pes[i];
    if (pe == CkMyPe() || pe == toPe || pe == home) continue;
    thisProxy[pe].updateLocation(idx, e);
  }
}

/**
  Migrating array element is arriving on this processor.
*/
void CkLocMgr::immigrate(CkArrayElementMigrateMessage* msg)
{
  MigTimer _t(&g_mig.packedSecs, &g_mig.packedN);
  void* gpuMsg = nullptr;
  if (msg->hasGPUMsg)
  {
    auto it = bufferedDeviceMigrateMsgs.find(msg->id);

    if (it == bufferedDeviceMigrateMsgs.end())
    {
      // Host state is here but the device pull is not. Park the message and
      // record that the element is mid-flight into this PE.
      bufferedHostMigrateMsgs[msg->id] = msg;
      noteImmigrationInFlight(msg->id);
      return;
    }
    gpuMsg = it->second;
  }
#if CMK_GPU
  else if (msg->needsLanding && grantedLandings.find(msg->id) == grantedLandings.end())
  {
    // Device state but no payload: nothing else holds this message back, so
    // park it until the gate has reserved the element's blocks; admitLandings
    // releases it. The admission timeout still admits without a guarantee if
    // the pool never frees, so this cannot wedge a run.
    bufferedHostMigrateMsgs[msg->id] = msg;
    noteImmigrationInFlight(msg->id);
    return;
  }
  if (msg->hasStable)
  {
    // The stable region was shipped at the decision; it lands on its own.
    auto sl = stableLandings.find(msg->id);
    if (sl == stableLandings.end() || sl->second.gen != msg->stableGen)
    {
      bufferedHostMigrateMsgs[msg->id] = msg;
      noteImmigrationInFlight(msg->id);
      return;
    }
  }
#endif

  const CkArrayIndex& idx = msg->idx;

  PUP::fromMem p(msg->packData, gpuMsg, PUP::er::IS_MIGRATION);
#if CMK_GPU
  // Sender-recorded device manifest: every DEVICE-mode unpack call is checked
  // against it (see PUP::fromMem::deviceManifestIn). nGpuBufs is 0 for
  // migrations whose payload was not packed by toMem (the by-handle path).
  if (msg->hasGPUMsg && msg->nGpuBufs > 0) {
    p.deviceManifestIn = msg->gpuManifest;
    p.deviceManifestCount = (size_t)msg->nGpuBufs;
  }
  // Arena mode applies only to staged payloads (nGpuBufs > 0 identifies
  // them; a by-handle payload landed in a pool block regardless of the env
  // flag). pup_buffer_device calls then rebind into gpuMsg instead of
  // copying out of it.
  const bool gpuMsgIsArena =
      ckMigrateArenaMode() && msg->hasGPUMsg && msg->nGpuBufs > 0 &&
      gpuMsg != nullptr;
  if (gpuMsgIsArena) p.deviceRebind = true;
  // The stable region: its buffers rebind into the landed block as the main
  // arena's do (a snapshot is only taken in arena mode).
  if (msg->hasStable) {
    p.stableOrigBuf = p.stableBuf = (unsigned char*)stableLandings[msg->id].block;
    p.deviceRebind = true;
  }
  // Pooled arena: the unpack copies go on a per-PE unpack stream. The payload
  // has landed (this entry runs behind the landing's callback), so the copies
  // depend on nothing on the default stream -- and waiting there afterwards
  // meant waiting behind every OTHER payload still landing on it. With 128 MB
  // experts at PCIe rates (moe) that was ~50 ms per arrival with the host
  // blocked and nothing of this element's in flight.
  static thread_local hapiStream_t unpackStream = NULL;
  if (gpuMsgIsArena) {
    if (unpackStream == NULL) hapiCheck(hapiStreamCreate(&unpackStream));
    p.gpuStream = (void*)unpackStream;
  }
#endif

  if (msg->nManagers < managers.size())
    CkAbort("Array element arrived from location with fewer managers!\n");
  if (msg->nManagers > managers.size())
  {
    // Some array managers haven't registered yet -- buffer the message
    DEBM((AA "Buffering %s immigrate msg waiting for array registration\n" AB,
          idx2str(idx)));
    pendingImmigrate.push_back(msg);
    noteImmigrationInFlight(msg->id);
    return;
  }

  if (msg->hasGPUMsg)
    bufferedDeviceMigrateMsgs.erase(msg->id);

  insertID(idx, msg->id);

  // Create a record for this element
  CkLocRec* rec = elementNrec(msg->id);
  if (rec == nullptr)
      rec = createLocal(idx, true, msg->ignoreArrival, false /* home told on departure */, msg->epoch);

  CmiAssert(CpvAccess(newZCPupGets).empty());  // Ensure that vector is empty
  // Create the new elements as we unpack the message. What the unpack
  // allocates is the arriving element's, though its entry method is not what
  // is running.
#if CMK_GPU
  hapiFootprintBegin(rec);
  {
    auto ls = landingSets.find(msg->id);   // the gate's landing set, if any
    hapiPreallocBegin(ls != landingSets.end() ? &ls->second : nullptr);
  }
#endif
  pupElementsFor(p, rec, CkElementCreation_migrate);
#if CMK_GPU
  hapiPreallocEnd();
  hapiFootprintEnd();
  // A payload landing's grant is retired when immigrateGPU takes its arena; a
  // landing without a payload has no such moment, so it is retired here, once
  // the element it was reserved for has unpacked. Before releaseLandingSet,
  // which may admit the next landing.
  if (msg->needsLanding) grantedLandings.erase(msg->id);
  releaseLandingSet(msg->id);   // what the unpack did not take goes back
#endif
#if CMK_GPU
  // The unpack's copies must complete before the element runs, and the wait
  // also gates the free of gpuMsg below. But it does not need to be a
  // device-wide barrier, which is what the note here has always said and what
  // the code did anyway: once per arriving object, and with ~750 arriving at
  // leanmd's first balancing step that was the bulk of the first step after it.
  //
  // Two cases, neither of which needs one. In arena mode pup_buffer_device
  // rebinds the pointers instead of copying, so the unpack enqueued no device
  // work at all -- and the payload's own arrival was already synchronized in
  // immigrateGPU -- so there is nothing to wait for and the arena is retained
  // rather than freed. Otherwise the copies are plain cudaMemcpy on the
  // default stream, so waiting on that stream is exactly the wait that was
  // wanted.
  const bool deviceRebound = gpuMsgIsArena && p.deviceReboundCount > 0;
  if (gpuMsg != nullptr && !deviceRebound) {
    const double tw0 = CmiWallTimer();
    if (p.gpuStream != nullptr)
      hapiCheck(ckSubmitDrainThenSync((hapiStream_t)p.gpuStream));
    else
      hapiCheck(hapiStreamSynchronize((hapiStream_t)0));
    if (migDbg())
      CkPrintf("[IMMIG %d] id=%llu unpack wait %.3f ms (%s)\n", CkMyPe(),
               (unsigned long long)msg->id, (CmiWallTimer() - tw0) * 1e3,
               p.gpuStream ? "unpack stream" : "default stream");
  }
#endif

#if CMK_GPU
  GPUManager& csv_gpu_manager = CsvAccess(gpu_manager);
  // Only elements that actually carried device state allocated a comm-buffer
  // block above (gpuMsg stays null when msg->hasGPUMsg is false). Freeing
  // unconditionally handed the allocator base_ptr + (0 - base_ptr) == nullptr
  // and aborted, so migrating any element without GPU state was fatal.
  if (gpuMsgIsArena) {
    if (p.deviceReboundCount > 0) {
      // The chare's rebound buffers live in the arena: keep it, and let
      // hapiFreeMigratable release it when the last one is freed. Extent is
      // the packed stream's aligned extent, recomputed from the manifest.
      size_t extent = 0;
      for (int i = 0; i < msg->nGpuBufs; ++i) {
        if (msg->gpuManifest[i] & PUP::DEVICE_MANIFEST_STABLE) continue;   // the stable region's
        extent = PUP::alignDeviceOffset(extent) + msg->gpuManifest[i];
      }
      // If the arena came from the device pool (immigrateGPU), it goes back
      // there when the last rebound buffer is released -- a buddy free, not a
      // cudaFree. Otherwise hapiFree, as before.
      hapiArenaRegister(gpuMsg, extent, p.deviceReboundCount,
                        ckMigrateArenaFromPool() ? CkDeviceFree : nullptr);
      // The arena is this element's state now: charge it, so its footprint on
      // this PE is what it holds rather than 0. Released by the free above.
      hapiFootprintCharge(gpuMsg, extent, rec);
    } else {
      // Nothing rebound: the arena served as a plain landing area for a
      // legacy pup, and its contents were copied out.
      if (ckMigrateArenaFromPool()) CkDeviceFree(gpuMsg);
      else hapiCheck(hapiFree(gpuMsg));
    }
  } else if(csv_gpu_manager.use_shm && gpuMsg != nullptr) {
    DeviceManager* dm = csv_gpu_manager.device_map[CkMyPe()];
#if CMK_SMP
    CmiLock(dm->lock);
#endif
  dm->free_comm_buffer((size_t)((char*)gpuMsg - (char*)dm->comm_buffer->base_ptr));
#if CMK_SMP
    CmiUnlock(dm->lock);
#endif
  // The landing write into this block and the unpack copies out of it are both
  // on the legacy default stream; record there so the next taker can order
  // itself behind them without the host waiting.
  CkRdmaDeviceNoteLbBufferFreed(dm, (hapiStream_t)0);
  }
  // The stable region: the element's storage now, like the arena, released
  // when its last rebound buffer goes.
  if (msg->hasStable) {
    auto sl = stableLandings.find(msg->id);
    if (sl != stableLandings.end()) {
      char* block = sl->second.block;
      stableLandings.erase(sl);
      if (p.stableReboundCount > 0) {
        size_t extent = 0;
        for (int i = 0; i < msg->nGpuBufs; ++i) {
          if (!(msg->gpuManifest[i] & PUP::DEVICE_MANIFEST_STABLE)) continue;
          extent = PUP::alignDeviceOffset(extent) + (msg->gpuManifest[i] & ~PUP::DEVICE_MANIFEST_STABLE);
        }
        hapiArenaRegister(block, extent, p.stableReboundCount, CkDeviceFree);
        hapiFootprintCharge(block, extent, rec);
      } else {
        CkDeviceFree(block);
      }
    }
  }
  if (migStatsOn()) {
    auto la = payloadLandedAt.find(msg->id);
    if (la != payloadLandedAt.end()) {
      g_mig.land.add(CkWallTimer() - la->second);
      payloadLandedAt.erase(la);
    }
  }
#endif

  bool zcRgetsActive = !CpvAccess(newZCPupGets).empty();
  if (zcRgetsActive)
  {
    // newZCPupGets is not empty, rgets need to be launched
    // newZCPupGets is populated with NcpyOperationInfo during pupElementsFor by
    // pup_buffer calls that require Rgets Issue Rgets using the populated newZCPupGets
    // vector
    zcPupIssueRgets(msg->id, this);
  }
  CpvAccess(newZCPupGets).clear();  // Clear this to reuse the vector
#if CMK_GPU
  // The element-wise check catches wrong sizes; this catches an unpack that
  // asked for fewer device buffers than were packed -- also a direction
  // mismatch, just one the per-call check cannot see.
  if (p.deviceManifestCount > 0 && p.deviceManifestIdx != p.deviceManifestCount)
    CkAbort("Device pup direction mismatch: sender packed %zu device buffers "
            "but unpack consumed only %zu",
            p.deviceManifestCount, p.deviceManifestIdx);
#endif
  if (p.size() != msg->length)
  {
    CkError(
        "ERROR! Array element claimed it was %d bytes to a"
        "packing PUP::er, but %zu bytes in the unpacking PUP::er!\n",
        msg->length, p.size());
    CkError("(I have %zu managers; it claims %d managers)\n", managers.size(),
            msg->nManagers);

    CkAbort("Array element's pup routine has a direction mismatch.\n");
  }

#if CMK_LBDB_ON
  // The element is registered here: retire its entry in the source's move
  // ledger. This is what holds the source's share of the step's resume.
  // Deliberately before the zcRgetsActive branch -- registration, not host
  // buffer completion, is the barrier's contract.
  if (msg->lbLedgerSrcPe >= 0)
    CProxy_LBManager(_lbmgr)[msg->lbLedgerSrcPe].ledgerRetire(msg->id,
                                                              msg->lbLedgerStep);
#endif

  if (migDbg())
    CkPrintf("[JUSTMIG %d] elem %s zcRgetsActive=%d\n", CkMyPe(),
             idx2str(idx), (int)zcRgetsActive);
  if (!zcRgetsActive)
  {
    // Let all the elements know we've arrived
    const double tj0 = migStatsOn() ? CkWallTimer() : 0.0;
    callMethod(rec, &CkMigratable::ckJustMigrated);
    callMethod(rec, &CkMigratable::lbCheckWaitRelease);
    if (tj0 != 0.0) g_mig.settle.add(CkWallTimer() - tj0);
  }

  delete msg;
}

void CkLocMgr::restore(const CkArrayIndex& idx, CmiUInt8 id, PUP::er& p)
{
  insertID(idx, id);

  // Directories are rebuilt from the restored elements, so tell the home.
  CkLocRec* rec = createLocal(idx, false, false, true);

  // Create the new elements as we unpack the message
  pupElementsFor(p, rec, CkElementCreation_restore);

  callMethod(rec, &CkMigratable::ckJustRestored);
}

/// Insert and unpack this array element from this checkpoint (e.g., from CkLocation::pup)
void CkLocMgr::resume(const CkArrayIndex& idx, CmiUInt8 id, PUP::er& p, bool notify,
                      bool rebuild)
{
  insertID(idx, id);

  CkLocRec* rec = createLocal(idx, false, false, notify /* home doesn't know yet */);

  // Create the new elements as we unpack the message
  pupElementsFor(p, rec, CkElementCreation_resume, rebuild);

  callMethod(rec, &CkMigratable::ckJustMigrated);
}

/********************* LocMgr: UTILITY ****************/
void CkMagicNumber_impl::badMagicNumber(int expected, const char* file, int line,
                                        void* obj) const
{
  CkError(
      "FAILURE on pe %d, %s:%d> Expected %p's magic number "
      "to be 0x%08x; but found 0x%08x!\n",
      CkMyPe(), file, line, obj, expected, magic);
  CkAbort(
      "Bad magic number detected!  This implies either\n"
      "the heap or a message was corrupted!\n");
}
CkMagicNumber_impl::CkMagicNumber_impl(int m) : magic(m) {}

/// Return true if this array element lives on another processor
bool CkLocMgr::isRemote(const CkArrayIndex& idx, int* onPe) const
{
  int pe = whichPe(idx);
  /* not definitely a remote element */
  if (pe == -1 || pe == CkMyPe())
    return false;
  // element is indeed remote
  *onPe = pe;
  return true;
}

static const char* rec2str[] = {
    "base (INVALID)",  // Base class (invalid type)
    "local",           // Array element that lives on this Pe
};

// If we are deleting our last array manager set duringDestruction to true to
// avoid sending out unneeded reclaimRemote messages.
void CkLocMgr::setDuringDestruction(bool _duringDestruction)
{
  duringDestruction = (_duringDestruction && managers.size() == 1);
}

// Add given element array record at idx, replacing the existing record
void CkLocMgr::insertRec(CkLocRec* rec, const CmiUInt8& id)
{
  CkLocRec* old_rec = elementNrec(id);
  hash[id] = rec;
  delete old_rec;
  noteResident(id);
}

// --- process residency table (see CkResidentTable above) ---

// Key an element by array as well as index: two arrays hand out the same ids.
CmiUInt8 CkLocMgr::residentKey(CmiUInt8 id) const
{
  return makeID(id).getID();
}

void CkLocMgr::noteResident(CmiUInt8 id)
{
  const CmiUInt8 key = residentKey(id);
  CkResidentTable& t = residentTable();
  std::lock_guard<std::mutex> lk(t.lock);
  auto it = t.owner.find(key);
  if (it != t.owner.end() && it->second != CkMyPe() && migDbg())
  {
    // Not fatal: an intra-process handoff can legitimately publish the arrival
    // before the source has retired its entry. Worth seeing, though, because a
    // persistent one means a departure path is not calling clearResident.
    CkPrintf("[MIG %d] resident table: id=%llu was owned by %d\n", CkMyPe(),
             (unsigned long long)id, it->second);
    fflush(stdout);
  }
  t.owner[key] = CkMyPe();
}

void CkLocMgr::clearResident(CmiUInt8 id)
{
  const CmiUInt8 key = residentKey(id);
  CkResidentTable& t = residentTable();
  std::lock_guard<std::mutex> lk(t.lock);
  auto it = t.owner.find(key);
  // Only retire our own entry. If another PE of this process has already
  // claimed the element, that claim is the newer truth and this is the losing
  // half of an intra-process handoff.
  if (it != t.owner.end() && it->second == CkMyPe()) t.owner.erase(it);
}

int CkLocMgr::residentOwnerPe(CmiUInt8 id) const
{
  const CmiUInt8 key = residentKey(id);
  CkResidentTable& t = residentTable();
  std::lock_guard<std::mutex> lk(t.lock);
  auto it = t.owner.find(key);
  return (it == t.owner.end()) ? -1 : it->second;
}

// Where a device zerocopy send should believe its target is.
//
// The transfer mode turns on one question -- is the target in my process, and
// therefore on my GPU -- and the location cache cannot answer it safely. It can
// still name a PE of this process for an element that has left, if the element
// left from another PE here; acting on that picks MEMCPY, stages nothing, and
// hands the receiver a device pointer belonging to an address space it cannot
// read. The residency table cannot be wrong about that, so it decides.
//
// Strictly more conservative than the cache alone: it only ever turns a MEMCPY
// decision into a staged one, never the reverse.
int CkLocMgr::deviceSendDestPe(const CkArrayIndex& idx)
{
  const int cached = whichPe(idx);

  CmiUInt8 id;
  if (!lookupID(idx, id)) return cached;  // index not known here at all

  const int resident = residentOwnerPe(id);
  if (resident != -1) return resident;  // in this process, exactly here

  // Not in this process. A cached PE that says otherwise is stale; report the
  // location as unconfirmed instead, which is what CkRdmaDeviceOnSender reads
  // as "stage this, I cannot promise the destination".
  if (cached != -1 && CmiNodeOf(cached) == CmiNodeOf(CkMyPe())) return -1;
  return cached;
}

size_t CkLocMgr::numResidentInProcess()
{
  CkResidentTable& t = residentTable();
  std::lock_guard<std::mutex> lk(t.lock);
  return t.owner.size();
}

// Call this on an unrecognized array index
static void abort_out_of_bounds(const CkArrayIndex& idx)
{
  CkPrintf("ERROR! Unknown array index: %s\n", idx2str(idx));
  CkAbort("Array index out of bounds\n");
}

// Look up array element in hash table.  Return NULL if not there.
CkLocRec* CkLocMgr::elementNrec(const CmiUInt8 id) const
{
  LocRecHash::const_iterator it = hash.find(id);
  return it == hash.end() ? NULL : it->second;
}

struct LocalElementCounter : public CkLocIterator
{
  unsigned int count;
  LocalElementCounter() : count(0) {}
  void addLocation(CkLocation& loc) { ++count; }
};

unsigned int CkLocMgr::numLocalElements()
{
  LocalElementCounter c;
  iterate(c);
  return c.count;
}

/********************* LocMgr: LOAD BALANCE ****************/

#if !CMK_LBDB_ON
// Empty versions of all load balancer calls
void CkLocMgr::startInserting(void) {}
void CkLocMgr::doneInserting(void) {}
#endif

#if CMK_LBDB_ON
void CkLocMgr::initLB(CkGroupID lbmgrID_, CkGroupID metalbID_)
{  // Find and register with the load balancer
  lbmgr = (LBManager*)CkLocalBranch(lbmgrID_);
  if (lbmgr == nullptr)
    CkAbort("LBManager not yet created?\n");
  DEBL((AA "Connected to load balancer %p\n" AB, lbmgr));
  if (_lb_args.metaLbOn())
  {
    the_metalb = (MetaBalancer*)CkLocalBranch(metalbID_);
    if (the_metalb == 0)
      CkAbort("MetaBalancer not yet created?\n");
  }
  syncBarrier = CkSyncBarrier::object();
  if (syncBarrier == nullptr)
    CkAbort("CkSyncBarrier not yet created?\n");
  // Register myself as an object manager
  LDOMid myId;
  myId.id = thisgroup;
  LDCallbacks myCallbacks;
  myCallbacks.migrate = (LDMigrateFn)CkLocRec::staticMigrate;
  myCallbacks.clone = (LDCloneFn)CkLocRec::staticClone;
  myCallbacks.retire = (LDRetireFn)CkLocRec::staticRetire;
  myCallbacks.cloneNotice = (LDCloneNoticeFn)CkLocRec::staticCloneNotice;
  myCallbacks.retireNotice = (LDRetireNoticeFn)CkLocRec::staticRetireNotice;
  myCallbacks.setStats = NULL;
  myCallbacks.queryEstLoad = NULL;
  myCallbacks.metaLBResumeWaitingChares =
      (LDMetaLBResumeWaitingCharesFn)CkLocRec::staticMetaLBResumeWaitingChares;
  myCallbacks.metaLBCallLBOnChares =
      (LDMetaLBCallLBOnCharesFn)CkLocRec::staticMetaLBCallLBOnChares;
  myLBHandle = lbmgr->RegisterOM(myId, this, myCallbacks);

  // Tell the lbdb that I'm registering objects
  lbmgr->RegisteringObjects(myLBHandle);

  // Set up callbacks for this LocMgr to call Registering/DoneRegistering during
  // each AtSync.
  lbBarrierBeginReceiver = syncBarrier->addBeginReceiver([=]() {
    DEBL((AA "CkLocMgr AtSync Receiver called\n" AB));
    lbmgr->RegisteringObjects(myLBHandle);
  });
  lbBarrierEndReceiver =
      syncBarrier->addEndReceiver([=]() { lbmgr->DoneRegisteringObjects(myLBHandle); });
}

void CkLocMgr::startInserting(void) { lbmgr->RegisteringObjects(myLBHandle); }
void CkLocMgr::doneInserting(void) { lbmgr->DoneRegisteringObjects(myLBHandle); }
#endif

#include "CkLocation.def.h"
