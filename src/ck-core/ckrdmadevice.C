/*
 * Direct GPU Messaging
 *
 * Uses host-bypass mechanisms to directly transfer data between GPU devices.
 *
 * 1) Intra-process (intra-node): The sender sends a metadata message to the
 *    receiver containing the pointer to the source GPU buffer. There is
 *    no setup needed on the sender side. The receiver invokes device-to-device
 *    transfer from the source GPU buffer to the destination GPU buffer.
 *
 * 2) Inter-process (intra-node): The pointer to the source GPU buffer will be
 *    invalid on the receiver as it is a different process. Thus CUDA IPC is
 *    used to create a handle to the source GPU buffer, which can be opened on
 *    the receiver side to initiate the data transfer. To mitigate the overheads
 *    of creating and destroying IPC handles, the runtime first allocates a
 *    'device communication buffer' on each GPU device, creates IPC handles
 *    only for these buffers, and then exchanges the handles between processes
 *    on the same physical node. This means each process will have IPC handles
 *    for all device communication buffers on the same host (that are potentially
 *    managed by other processes) and can perform data transfers using these
 *    handles. Each GPU-GPU data transfer invovles requesting a block from the
 *    device communication buffer on the sender, copying the source GPU buffer
 *    to the allocated block, sending a metadata message to the receiver
 *    (that contains the offset of the allocated block), and performing a
 *    transfer from the block on the sender's device communication buffer to
 *    the destination GPU buffer. CUDA Events are used to enforce the correct
 *    ordering between these data transfers. Because multiple PEs can be mapped
 *    to the same GPU and hence concurrently request allocations from the same
 *    device communication buffer, a thread-safe allocator using the buddy
 *    allocation algorithm was implemented. The allocator first calls hapiMalloc
 *    to obtain a relatively large chunk of memory and then services allocation
 *    and deallocation requests from PEs that are mapped to its GPU device.
 *    The buddy algorithm was used to minimize the external fragmentation that
 *    could occur from concurrent manipulations of the device communication
 *    buffer.
 *
 * TODO
 * 3) Inter-node: This currently uses a simple host-staged mechanism to perform
 *    a device-to-host copy of the source GPU buffer to a message, which is sent
 *    to the receiver. The receiver then performs a host-to-device copy to the
 *    destination GPU buffer. This will be updated to use GPUDirect RDMA to
 *    directly performa true device-to-device transfer.
 */

#ifndef _WIN32
#include <pthread.h>
#endif
#include "envelope.h"
#include "charm++.h"
#include "ck.h"
#include "ckrdmadevice.h"
#include <deque>
#include <array>
#include "buddy_allocator.h"
#include <mutex>
#include <map>
#include <set>
#include <condition_variable>

#define CMK_GPU_COMM 1

#if CMK_GPU

CmiNcpyModeDevice findTransferModeDevice(int srcPe, int dstPe) {
  CmiEnforce((srcPe >= 0) && (srcPe <= CmiNumPes()));
  CmiEnforce((dstPe >= 0) && (dstPe <= CmiNumPes()));

  if (CmiNodeOf(srcPe) == CmiNodeOf(dstPe)) {
    // Same logical node
    return CmiNcpyModeDevice::MEMCPY;
  } else if (CmiPeOnSamePhysicalNode(srcPe, dstPe)) {
    // Different logical nodes, same physical node
    return CmiNcpyModeDevice::IPC;
  } else {
    // Different physical nodes, requires GPUDirect RDMA
    return CmiNcpyModeDevice::RDMA;
  }
}

#include <atomic>
#include <stdio.h>
#include <unordered_map>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <string>

#include "hapi.h"

// A stream synchronize that also covers work this PE has handed to a submitter
// thread (+gpusubmit): it has to be in the driver before the wait means anything.
static inline hapiError_t ckSubmitDrainThenSync(hapiStream_t stream) {
  hapiSubmitDrain();
  return hapiStreamSynchronize(stream);
}
#include "gpumanager.h"

CsvExtern(GPUManager, gpu_manager);
CpvExtern(int, my_device_id);
static inline hapi_ipc_event_shared* ipcSharedSlot(int device_idx, int event_idx);

// Defined further down, used by both receive completion handlers above it.
void zcRecordRecvTime(int slot, size_t bytes, double seconds);

// void CkRdmaDeviceRecvHandler(void* data)
// {
//   DeviceRdmaOp* op = (DeviceRdmaOp*)data;
//   DeviceRdmaInfo* info = op->info;

//   // Invoke source callbacks
//   if (op->src_cb) {
//     int rank;
//     CkCallback* cb = (CkCallback*)op->src_cb;
//     cb->send();
//     delete cb;
//   }

//   // Update counter (there may be multiple buffers in transit)
//   info->counter++;

//   // Check if all buffers have been received
//   // If so, invoke regular entry method
//   if (info->counter == info->n_ops) {
//     QdCreate(1);

//     enqueueNcpyMessage(op->dest_pe, info->msg);

//     // Free RDMA metadata
//     CmiFree(info);
//   }
// }

// ---- Device registration: pool arenas once, everything else cached --------
//
// Under +gpupool every device buffer the runtime sends or lands lives inside a
// pool arena: one cudaMalloc of CK_GPU_ARENA_MB that is never freed, whatever
// the pool carves out of it and hands back. A network registration of the
// whole arena is therefore valid for the life of the process, and one is all
// an arena ever gets: the first send or receive that touches an arena
// registers [base, base+extent) once, process-wide, and every descriptor for
// a buffer inside it is a copy of that registration with the buffer's own
// pointer and count. The LCI get addresses the remote buffer as a displacement
// from the registered region's base, so a sub-range needs nothing more. Nothing
// is ever deregistered, and nothing needs to be: the arena outlives every
// transfer. That is the whole point of having a pool on the network side --
// without it every inter-node device send was a real fi_mr_reg that nothing
// released, and at 64 PEs the NIC's registration table filled in ~50 steps.
//
// Buffers that are not the pool's (a run without +gpupool, or an application
// allocation outside it) keep the per-buffer path below.
//
// ---- Device registration cache (CHARM_DEVICE_MR_CACHE) --------------------
//
// The device path builds a fresh CmiNcpyBuffer for every send, and constructing
// one registers the buffer with the network. The application reuses the same
// device buffers every iteration, so nearly all of that is re-registration of
// memory that is already registered: 5318 registration calls resolving to 710
// distinct regions in one 60-iteration run. Nothing is ever deregistered
// either, which is why disabling the provider's own MR cache costs 3.3x -- with
// nothing absorbing the repeats, every one becomes a real fi_mr_reg.
//
// Keep the registration instead, keyed by the range it covers, and hand out
// copies that differ only in the per-transfer ref field.
//
// The hard part of any registration cache is knowing when the memory died, and
// this one claims to know exactly one case: an element's device buffers are
// freed when it migrates, and migration is mediated by the runtime, so entries
// are attributed to the element that registered them and dropped when it
// leaves. An application that frees and reallocates a device buffer in place is
// invisible here for the same reason it is invisible to the provider's cache --
// CUDA suballocates, so a free usually raises no address-space event at all.
// That is the gap, and it is why this is off unless CHARM_DEVICE_MR_CACHE is
// set.
//
// Both directions are cached. The send path attributes entries to
// _currentLocRec, the element issuing the send. The receive path runs before
// the entry method, so _currentLocRec is not yet set -- but the envelope names
// the element the receive is addressed to, and CkFindDeviceRecvElement
// resolves it to the same CkLocRec the migration paths retire. A receive with
// no element attached (a migration payload on a group entry) is not cached:
// its buffer is freshly allocated per transfer and freed after, exactly the
// staleness an unowned entry would hide.

struct DeviceMrEntry {
  CmiNcpyBuffer reg;   // registered once; copies differ only in ref and cnt
  CkLocRec* owner;     // whose migration retires this entry
};

// Keyed by pointer alone, not (ptr, cnt): the particle-exchange pattern sends
// from a fixed buffer with a different count every iteration, and a
// count-qualified key turns that into one fresh registration per distinct
// count -- the same unbounded growth this cache exists to stop. A registration
// covers [ptr, ptr+cnt); a request for fewer bytes reuses it with the copy's
// cnt shrunk to the request, and a request for more re-registers the larger
// extent in place.
typedef std::unordered_map<const void*, DeviceMrEntry> DeviceMrCache;

CkpvDeclare(DeviceMrCache*, device_mr_cache);

static bool deviceMrCacheEnabled()
{
  static const bool on = (getenv("CHARM_DEVICE_MR_CACHE") != nullptr);
  return on;
}

// A device receive pins its completion message to this process: the message
// carries pointers into the buffers the element posted here, and they stay
// referenced until the application has consumed the message -- which for an
// SDAG entry can be long after the transfers completed, since a message for a
// future round buffers in the dependency object. Each entry records the
// stand-downs to release when that message is finally freed, which is the
// one event that means nothing references it any more, wherever it happened.
struct DeviceRecvHold { int aid_idx; CmiUInt8 id; };
typedef std::unordered_map<void*, std::vector<DeviceRecvHold>> DeviceRecvHoldMap;
CkpvDeclare(DeviceRecvHoldMap*, device_recv_holds);

// Envelopes held back by admission control while their element is parked,
// keyed by the bare element id. Replayed -- re-injected into delivery --
// when the element unparks (it can consume again) or departs (delivery then
// forwards them to its new home, unpreprocessed and clean).
typedef std::unordered_map<CmiUInt8, std::vector<void*>> DeviceRecvAdmissionMap;
CkpvDeclare(DeviceRecvAdmissionMap*, device_recv_admission);

// Landing buffers of staged parked receives (CkRdmaDeviceStageParked), keyed
// by the landing pointer the rewritten descriptor now carries. `ready` is
// recorded on the staging stream behind the pull; the delivery waits on it.
struct StagedLanding {
  hapiEvent_t ready;
  size_t cnt;
};
typedef std::unordered_map<const void*, StagedLanding> StagedLandingMap;
// Stamped into comm_offset of a rewritten descriptor (unused when the protocol
// is NONE). A pointer alone does not identify a landing: `ptr` in an incoming
// message is an address in the SENDER's process, and every process lays its
// device arenas out the same way, so a foreign pointer can equal one of this
// PE's landing buffers. Matching on the pointer alone copied a landing buffer
// into a migration payload once in ~5 sph2d runs (2026-09-15, job 22084755).
static const size_t kStagedLandingMark = (size_t)0x53544147454431ULL;
static inline bool isStagedLandingDescriptor(const CkDeviceBuffer& b)
{
  return b.src_pe == CkMyPe() && b.ipc_protocol == CmiIpcProtocol::NONE &&
         b.device_idx == -1 && b.event_idx == -1 && b.sender_prepared &&
         b.comm_offset == kStagedLandingMark;
}
CkpvDeclare(StagedLandingMap*, staged_landings);
CkpvDeclare(hapiStream_t, staging_stream);
// Carries only the producer wait that an inter-node restage put has to observe
// before the NIC may read the source. Its own stream, so that wait is never
// queued behind an unrelated staging copy.
CkpvDeclare(hapiStream_t, restage_wait_stream);
CkpvDeclare(std::vector<hapiEvent_t>*, staging_events);
// The PE's receive stream: every device receive lands its copy on it, and
// nothing is ever issued on it that cannot run at once (DeviceRecvPending).
CkpvDeclare(hapiStream_t, device_recv_stream);
struct DeviceRecvPending;
CkpvDeclare(std::vector<DeviceRecvPending*>*, device_recv_pending);
// Events for the destination-order fallback when the flag ring is full.
CkpvDeclare(std::vector<hapiEvent_t>*, device_recv_events);
static void deviceRecvPendingPoll(void*);
// Landings waiting to be issued as one batch (see deviceRecvBatchAdd).
struct DeviceRecvBatch;
CkpvDeclare(DeviceRecvBatch*, device_recv_batch);
static void deviceRecvBatchPoll(void*);
static void deviceRecvBatchIdle(void*);

// Device work in flight on an element (hapiDeviceWorkBegin/End, see hapi.h):
// every HAPI callback registered inside an element's entry method raises its
// outstanding count until the callback fires. Without this a Compute went
// "quiet" the moment it consumed its positions, its force kernels still
// running, and 46% of kicks packed an element whose last kernel had not
// finished -- the pack copies then waited on the device and, on the shared
// migration stream, held every later pack behind them (128-190 ms median).
struct CkDeviceWorkToken {
  CkLocMgr* mgr;
  CmiUInt8 id;
};
static void* ckDeviceWorkBegin()
{
  CkLocRec* r = CkpvAccess(_currentLocRec);
  if (r == NULL) return nullptr;
  r->noteDeviceSendPosted();
  return new CkDeviceWorkToken{r->getLocMgr(), r->getID()};
}
static void ckDeviceWorkEnd(void* token)
{
  CkDeviceWorkToken* t = (CkDeviceWorkToken*)token;
  CkLocRec* r = t->mgr->recForDeviceWork(t->id);   // gone if it migrated or died meanwhile
  if (r) r->noteDeviceSendDone();
  delete t;
}

static void ckRegisterPoolSegments(void* base, size_t bytes, size_t unit, int dev);   // below, with the arena registrations

void CkRdmaDeviceRegistrationCacheInit()
{
  hapiDeviceWorkBegin = ckDeviceWorkBegin;
  hapiDeviceWorkEnd = ckDeviceWorkEnd;
  hapiDevPoolSegmentHook = ckRegisterPoolSegments;   // pool segments register at the mapping
  CkpvInitialize(DeviceMrCache*, device_mr_cache);
  CkpvAccess(device_mr_cache) = deviceMrCacheEnabled() ? new DeviceMrCache() : NULL;
  CkpvInitialize(DeviceRecvHoldMap*, device_recv_holds);
  CkpvAccess(device_recv_holds) = new DeviceRecvHoldMap();
  CkpvInitialize(DeviceRecvAdmissionMap*, device_recv_admission);
  CkpvAccess(device_recv_admission) = new DeviceRecvAdmissionMap();
  CkpvInitialize(StagedLandingMap*, staged_landings);
  CkpvAccess(staged_landings) = new StagedLandingMap();
  CkpvInitialize(hapiStream_t, staging_stream);
  CkpvAccess(staging_stream) = nullptr;
  CkpvInitialize(hapiStream_t, restage_wait_stream);
  CkpvAccess(restage_wait_stream) = nullptr;
  CkpvInitialize(std::vector<hapiEvent_t>*, staging_events);
  CkpvAccess(staging_events) = new std::vector<hapiEvent_t>();
  CkpvInitialize(hapiStream_t, device_recv_stream);
  CkpvAccess(device_recv_stream) = nullptr;
  CkpvInitialize(std::vector<DeviceRecvPending*>*, device_recv_pending);
  CkpvAccess(device_recv_pending) = new std::vector<DeviceRecvPending*>();
  CkpvInitialize(std::vector<hapiEvent_t>*, device_recv_events);
  CkpvAccess(device_recv_events) = new std::vector<hapiEvent_t>();
  CcdCallOnConditionKeep(CcdSCHEDLOOP, (CcdCondFn)deviceRecvPendingPoll, NULL);
  CkpvInitialize(DeviceRecvBatch*, device_recv_batch);
  CkpvAccess(device_recv_batch) = nullptr;
  // A batch flushes on age (checked every scheduler pass) and as soon as the
  // PE has nothing else to do.
  CcdCallOnConditionKeep(CcdSCHEDLOOP, (CcdCondFn)deviceRecvBatchPoll, NULL);
  CcdCallOnConditionKeep(CcdPROCESSOR_BEGIN_IDLE, (CcdCondFn)deviceRecvBatchIdle, NULL);
  CcdCallOnConditionKeep(CcdPROCESSOR_STILL_IDLE, (CcdCondFn)deviceRecvBatchIdle, NULL);
}

static hapiStream_t stagingStream()
{
  hapiStream_t& s = CkpvAccess(staging_stream);
  if (s == nullptr) hapiCheck(hapiStreamCreateWithFlags(&s, hapiStreamNonBlocking));
  return s;
}
static hapiStream_t restageWaitStream()
{
  hapiStream_t& s = CkpvAccess(restage_wait_stream);
  if (s == nullptr) hapiCheck(hapiStreamCreateWithFlags(&s, hapiStreamNonBlocking));
  return s;
}
static hapiEvent_t stagingEventTake()
{
  std::vector<hapiEvent_t>& pool = *CkpvAccess(staging_events);
  if (pool.empty()) {
    hapiEvent_t e;
    hapiCheck(hapiEventCreateWithFlags(&e, hapiEventDisableTiming));
    return e;
  }
  hapiEvent_t e = pool.back();
  pool.pop_back();
  return e;
}
static void stagingEventGive(hapiEvent_t e) { CkpvAccess(staging_events)->push_back(e); }

// Runs on the posting stream behind the copy out of a landing buffer: the
// buffer goes back to the pool (CkDeviceFree has no stream ordering of its
// own, so it must be called from here, not at issue) and the event to its pool.
struct StagedLandingFree {
  void* ptr;
  hapiEvent_t ready;
};
static void stagedLandingFreeFn(void* param, void*)
{
  StagedLandingFree* f = (StagedLandingFree*)param;
  CkDeviceFree(f->ptr);
  stagingEventGive(f->ready);
  delete f;
}
static inline bool stagedDbg()
{
  static const bool on = (getenv("CHARM_DEBUG_MIGRATE") != nullptr);
  return on;
}

// Called from CkFreeMsg for every message: release whatever this one held.
// The empty() check keeps the common case at one branch.
// Does the running element hold any unconsumed device-receive message? The
// application checks this before parking in AtSyncWait: a parked element runs
// no SDAG and can never consume, so parking un-quiet would deadlock the
// migration interlock.
bool CkDeviceRecvQuiet()
{
  CkLocRec* rec = CkpvAccess(_currentLocRec);
  return rec == NULL || rec->outstandingDeviceSends == 0;
}


void CkDeviceRecvAdmissionBuffer(CmiUInt8 id, void* env)
{
  (*CkpvAccess(device_recv_admission))[id].push_back(env);
  if (getenv("CHARM_DEBUG_MIGRATE"))
    CmiPrintf("[BUFFER %d] id=%llu depth=%zu\n", CkMyPe(),
              (unsigned long long)id,
              (*CkpvAccess(device_recv_admission))[id].size());
}

void CkDeviceRecvAdmissionReplay(CmiUInt8 id)
{
  DeviceRecvAdmissionMap* m = CkpvAccess(device_recv_admission);
  if (m == NULL || m->empty()) return;
  auto it = m->find(id);
  if (it == m->end()) return;
  std::vector<void*> envs;
  envs.swap(it->second);
  m->erase(it);
  if (envs.size() && getenv("CHARM_DEBUG_MIGRATE"))
    CmiPrintf("[REPLAY %d] id=%llu n=%zu\n", CkMyPe(),
              (unsigned long long)id, envs.size());
  for (size_t k = 0; k < envs.size(); k++)
    CmiPushPE(CmiMyRank(), envs[k]);
}

// Receives parked for a dependency still running, and the most parked at once.
static long g_recv_parked = 0, g_recv_parked_max = 0;

void CkRdmaDeviceMsgFreed(void* env)
{
  DeviceRecvHoldMap* holds = CkpvAccess(device_recv_holds);
  if (holds == NULL || holds->empty()) return;
  auto it = holds->find(env);
  if (it == holds->end()) return;
  // The map is keyed by envelope ADDRESS, and the allocator recycles
  // addresses. A held message freed through a path that does not run this
  // hook -- any raw CmiFree, of which the runtime has many -- leaves its
  // entry behind. The next envelope allocated at that address then releases
  // its predecessor's holds: an element with live transfers reads zero
  // outstanding sends, migrates, and frees the very buffers those transfers
  // are still landing in. That corrupts the heap on the source process and
  // strands every neighbour waiting on the ghosts it owed.
  //
  // A hold is only released against the message it was recorded for, so
  // confirm this envelope still names that recipient. A recycled address is
  // dropped, not released -- the element it names keeps its stand-down and
  // its migration waits for a real completion.
  envelope* e = (envelope*)env;
  const bool for_elt = (e->getMsgtype() == ForArrayEltMsg);
  for (size_t k = 0; k < it->second.size(); k++) {
    if (!for_elt || e->getRecipientID() != it->second[k].id) {
      static int stale_holds = 0;
      if (++stale_holds <= 8 && getenv("CHARM_DEBUG_MIGRATE"))
        CmiPrintf("[STALEHOLD %d] env=%p recycled: recorded id=%llu, now %s "
                  "id=%llu (n=%d)\n", CkMyPe(), env,
                  (unsigned long long)it->second[k].id,
                  for_elt ? "elt" : "non-elt",
                  (unsigned long long)(for_elt ? e->getRecipientID() : 0),
                  stale_holds);
      continue;
    }
    CkGroupID daid; daid.idx = it->second[k].aid_idx;
    CkNoteDeviceRecvComplete(daid, it->second[k].id);
  }
  holds->erase(it);
}

// One registration per pool segment, shared by every PE of the process: the
// arena under the buddy backend, the run of chunks covering the buffer under
// the vmm backend (hapiDevPoolSegmentOf). Keyed by (base, extent); created on
// first use; marked no-dereg so no completion path can ever release it.
static std::mutex g_deviceArenaRegLock;
static std::condition_variable g_deviceArenaRegCv;
static std::map<std::pair<uintptr_t, size_t>, CmiNcpyBuffer> g_deviceArenaRegs;
static std::set<std::pair<uintptr_t, size_t>> g_deviceArenaRegBusy;   // registrations in flight, lock released

// Registers [base, base+extent) with the fabric once for the process and returns its descriptor;
// true when this call did the registering (ms_out: how long it took). The fabric call runs with the
// lock RELEASED: it costs milliseconds per chunk and a process whose PEs first touch many chunks at
// once (a one-process-per-node layout at its Metis event: ~150 chunk runs per process, job
// 22856533) would otherwise register them one after another, every PE queued on this mutex.
// Concurrent registrations of DIFFERENT chunks proceed together; a second requester of a chunk
// already in flight waits for that one registration.
static bool registerPoolSegmentOnce(void* base, size_t extent, CmiNcpyBuffer* out, double* ms_out)
{
  std::unique_lock<std::mutex> lk(g_deviceArenaRegLock);
  const auto key = std::make_pair((uintptr_t)base, extent);
  auto it = g_deviceArenaRegs.find(key);
  if (it == g_deviceArenaRegs.end() && g_deviceArenaRegBusy.count(key)) {
    g_deviceArenaRegCv.wait(lk, [&] { return !g_deviceArenaRegBusy.count(key); });
    it = g_deviceArenaRegs.find(key);
  }
  bool registered = false;
  if (it == g_deviceArenaRegs.end()) {
    g_deviceArenaRegBusy.insert(key);
    lk.unlock();
    const double t0 = CmiWallTimer();
    CmiNcpyBuffer reg(base, extent, CMK_BUFFER_REG, CMK_BUFFER_NODEREG);  // the one registration
    if (ms_out) *ms_out = (CmiWallTimer() - t0) * 1.0e3;
    lk.lock();
    it = g_deviceArenaRegs.emplace(key, reg).first;
    g_deviceArenaRegBusy.erase(key);
    g_deviceArenaRegCv.notify_all();
    registered = true;
  }
  if (out) *out = it->second;
  return registered;
}

// hapiDevPoolSegmentHook: the pool has just mapped [base, base+bytes) in units of `unit` (a vmm chunk,
// or the whole buddy arena). Register every unit now, on the mapping thread with the pool lock
// released, so that no transfer later pays the 10 ms per chunk at its first use -- for the
// one-process-per-node layout that was ~150 registrations inside its first LB event (job 22856533).
// One node holds no off-node peer, and nothing is registered there, as before.
static void ckRegisterPoolSegments(void* base, size_t bytes, size_t unit, int dev)
{
  if (CmiNumPhysicalNodes() <= 1 || unit == 0) return;
  const double t0 = CmiWallTimer();
  size_t n = 0;
  double mx = 0.0;
  for (size_t off = 0; off < bytes; off += unit) {
    double ms = 0.0;
    if (registerPoolSegmentOnce((char*)base + off, std::min(unit, bytes - off), NULL, &ms)) {
      n++;
      if (ms > mx) mx = ms;
    }
  }
  if (n > 0)
    CmiPrintf("[%d] device pool: %zu %s of %zu MB on device %d registered for RDMA at the mapping "
              "(%.2f s, max %.1f ms each)\n", CmiMyPe(), n,
              hapiDevPoolIsVmm() ? "chunk runs" : "arenas", unit >> 20, dev, CmiWallTimer() - t0, mx);
}

static bool acquireArenaRegistration(const void* ptr, size_t cnt, CmiNcpyBuffer* out)
{
  if (!hapiDevPoolOn()) return false;
  void* base = NULL;
  size_t extent = 0;
  if (!hapiDevPoolSegmentOf(ptr, cnt, &base, &extent)) return false;

  double ms = 0.0;
  if (registerPoolSegmentOnce(base, extent, out, &ms))
    CmiPrintf("[%d] device pool: %s %p (%zu MB) registered once for RDMA (%.1f ms)\n",
              CmiMyPe(), hapiDevPoolIsVmm() ? "chunk run" : "arena", base, extent >> 20, ms);
  // The registration covers the arena; the transfer is the buffer.
  out->ptr = ptr;
  out->cnt = cnt;
  out->pe = CmiMyPe();
  return true;
}

// Registered descriptor for [ptr, ptr+cnt). A pool buffer gets its arena's
// one registration; otherwise registers on a miss, and without the cache
// behaves exactly as constructing one in place did.
static CmiNcpyBuffer acquireDeviceRegistration(const void* ptr, size_t cnt,
                                               CkLocRec* owner)
{
  CmiNcpyBuffer arena;
  if (acquireArenaRegistration(ptr, cnt, &arena)) return arena;

  DeviceMrCache* cache = CkpvAccess(device_mr_cache);
  if (cache == NULL) return CmiNcpyBuffer(ptr, cnt);

  auto it = cache->find(ptr);
  if (it == cache->end()) {
    DeviceMrEntry entry;
    entry.reg = CmiNcpyBuffer(ptr, cnt);   // the one real registration
    entry.owner = owner;
    it = cache->emplace(ptr, entry).first;
  } else {
    if (cnt > it->second.reg.cnt) {
      // A larger transfer from the same buffer: re-register the larger
      // extent, once. Counts only ever grow entries; they never multiply them.
      it->second.reg.deregisterMem();
      it->second.reg = CmiNcpyBuffer(ptr, cnt);
    }
    if (it->second.owner == NULL) {
      // First use came from a context with no element attached; adopt the
      // first owner that does appear, so the entry becomes retirable.
      it->second.owner = owner;
    }
  }
  CmiNcpyBuffer out = it->second.reg;
  // The registration spans at least [ptr, ptr+cnt); the transfer must still
  // be sized by the request, not by the registered extent.
  out.cnt = cnt;
  return out;
}

// Retire everything registered for an element. Called from the migration paths,
// where the element's device buffers are about to be freed.
void CkRdmaDeviceDropRegistrations(CkLocRec* owner)
{
  DeviceMrCache* cache = CkpvAccess(device_mr_cache);
  if (cache == NULL || owner == NULL) return;
  for (auto it = cache->begin(); it != cache->end();) {
    if (it->second.owner == owner) {
      it->second.reg.deregisterMem();
      it = cache->erase(it);
    } else {
      ++it;
    }
  }
}

struct LoopBackMsg {
  char header[CmiMsgHeaderSizeBytes];
  void* msg;
};

extern "C" {
  void* loopback_bridge(void* arg) {
    QdProcess(1);
    LoopBackMsg* recv_msg = (LoopBackMsg*)arg;
    CkRdmaDeviceRecvHandler(recv_msg->msg);
    CmiFree(recv_msg);
    return NULL;
  }
  
  int loopback_handler;
}

// Sender side of a correction put; defined with the rest of the protocol below.
static void notifyDeviceRestagePut(int dest_pe, void* dest_op);

// Neither completion handler below releases the migration stand-downs the
// message's receives took: those release when the completion message itself
// dies (CkRdmaDeviceMsgFreed), i.e. at consumption. Releasing at transfer
// completion is too early -- noteDeviceSendDone fires a pending migrateMe,
// the element packs and frees its buffers before the just-enqueued message
// is consumed, and the message is then forwarded to the new process carrying
// pointers into this one. That was the DiffusionLB-async failure: diffusion
// moves both ends of many send pairs per step, so the window was hit
// constantly.

// Zero-copy statistics, declared ahead of the completion handlers that record into them.
namespace {
inline bool zcStatsOnFast() {
  static const bool on = (getenv("CHARM_ZC_STATS") != nullptr);
  return on;
}
struct ZcModeStats {
  std::atomic<long> memcpy_n{0};
  std::atomic<long> ipc_n{0};
  std::atomic<long> other_n{0};
  // Completed receives, their bytes, and the wall time from posting the receive
  // to its last op completing -- per mode. Counts alone answer "did placement
  // move traffic to a slower transport"; they cannot answer "by how much", and
  // a cost model needs the second question. A pingpong benchmark cannot answer
  // it either: it measures a transfer alone on the machine, whereas what a
  // balancer needs is what the transfer costs amid all the others contending
  // for the same device, link and NIC. Only the application's own traffic has
  // that contention in it.
  //
  // Recorded per receive, not per op: the receive is the unit the application
  // waits on, and its ops complete concurrently.
  std::atomic<long> recv_n[3];
  std::atomic<long> recv_bytes[3];
  // Microseconds, as an integer so the accumulation stays a relaxed atomic add.
  std::atomic<long> recv_us[3];
  // Batched landings (deviceRecvBatchAdd): batches issued, ops they carried,
  // the largest, and how often the batched copy API refused and the batch fell
  // back to one copy per op (still one completion per batch).
  // A channel receiver's release(): the credit's allocation and its converse send,
  // split by whether the sender is on this host.
  std::atomic<long> credit_n[2]{{0},{0}};
  std::atomic<long> credit_alloc_us[2]{{0},{0}};
  std::atomic<long> credit_send_us[2]{{0},{0}};
  // A channel sender's put(): the host time from the call to its return, by
  // tier (0 same host, 1 other host, 2 unresolved -> queued), and how many
  // puts had to wait for a credit or a resolution; the owner's taken events.
  std::atomic<long> put_n[3]{{0},{0},{0}};
  std::atomic<long> put_us[3]{{0},{0},{0}};
  // Where a cross-host put's host time goes (issue): the source endpoint's open (export + registration),
  // the persistent put (context, op info, the stream callback), and the same-host batch flush.
  std::atomic<long> chan_open_n{0}, chan_open_us{0}, chan_rput_n{0}, chan_rput_us{0}, chan_flush_n{0}, chan_flush_us{0};
  // The scheduler-loop polls (every iteration): total and the longest single call.
  std::atomic<long> shmpoll_n{0}, shmpoll_us{0}, shmpoll_max{0}, recvpoll_n{0}, recvpoll_us{0}, recvpoll_max{0};
  std::atomic<long> put_pending{0};
  std::atomic<long> put_staged{0};    // puts staged and announced while the receiver was closed
  std::atomic<long> chan_batches{0};  // channel put batches flushed, their puts, the largest, batched-copy refusals
  std::atomic<long> chan_batch_ops{0};
  std::atomic<long> chan_batch_max{0};
  std::atomic<long> chan_batch_fallback{0};
  std::atomic<long> event_n{0};
  std::atomic<long> signal_n{0};   // notes that arrived as the data write's immediate
  std::atomic<long> batch_flushes{0};
  std::atomic<long> batch_ops{0};
  std::atomic<long> batch_max{0};
  std::atomic<long> batch_fallback{0};
  std::atomic<long> batch_us{0};   // host time inside the batched copy call (and its fallback)
  // Host time (us) the PEs of this process spent inside the per-message paths:
  // preparing a send (CkRdmaDeviceOnSender), admitting and issuing a receive
  // (CkRdmaDeviceIssueRgets), and completing ops (CkRdmaDeviceRecvHandler).
  // What a cross-process device message costs the host, as distinct from what
  // its bytes cost the device.
  std::atomic<long> host_send_us{0};
  std::atomic<long> host_send_n{0};
  std::atomic<long> host_recv_us{0};
  std::atomic<long> host_recv_n{0};
  std::atomic<long> host_done_us{0};
  std::atomic<long> host_done_n{0};

  ZcModeStats() {
    for (int i = 0; i < 3; i++) { recv_n[i] = 0; recv_bytes[i] = 0; recv_us[i] = 0; }
  }
  static const char* slotName(int s) {
    return (s == 0) ? "MEMCPY" : (s == 1) ? "IPC" : "OTHER";
  }
  ~ZcModeStats() {
    const long m = memcpy_n.load(), i = ipc_n.load(), o = other_n.load();
    const long total = m + i + o;
    fprintf(stderr, "[hapi-stats] pid=%d completion-flag writes %ld\n", (int)getpid(), hapiFlagWritesIssued());
    const long pn0 = put_n[0].load(), pn1 = put_n[1].load(), pn2 = put_n[2].load();
    if (pn0 + pn1 + pn2 > 0)
      fprintf(stderr, "[chan-stats] pid=%d puts: same-host %ld (%.2f us each), other-host %ld (%.2f us), "
                      "unresolved %ld (%.2f us); queued for a credit %ld, staged %ld; events taken %ld, notes as immediate data %ld\n",
              (int)getpid(), pn0, pn0 ? put_us[0].load() / (double)pn0 : 0.0, pn1, pn1 ? put_us[1].load() / (double)pn1 : 0.0,
              pn2, pn2 ? put_us[2].load() / (double)pn2 : 0.0, put_pending.load(), put_staged.load(), event_n.load(), signal_n.load());
      fprintf(stderr, "[chan-poll] pid=%d shm note/credit poll n=%ld (%.2f us, max %.2f ms); pending-receive poll n=%ld (%.2f us, max %.2f ms)\n",
              (int)getpid(), shmpoll_n.load(), shmpoll_n.load() ? shmpoll_us.load() / (double)shmpoll_n.load() : 0.0, shmpoll_max.load() / 1e3,
              recvpoll_n.load(), recvpoll_n.load() ? recvpoll_us.load() / (double)recvpoll_n.load() : 0.0, recvpoll_max.load() / 1e3);
      fprintf(stderr, "[chan-cost] pid=%d cross-host issue: open n=%ld (%.2f us), rput n=%ld (%.2f us); same-host batch flush n=%ld (%.2f us)\n",
              (int)getpid(), chan_open_n.load(), chan_open_n.load() ? chan_open_us.load() / (double)chan_open_n.load() : 0.0,
              chan_rput_n.load(), chan_rput_n.load() ? chan_rput_us.load() / (double)chan_rput_n.load() : 0.0,
              chan_flush_n.load(), chan_flush_n.load() ? chan_flush_us.load() / (double)chan_flush_n.load() : 0.0);
    if (chan_batches.load() > 0)
      fprintf(stderr, "[chan-stats] pid=%d put batches %ld for %ld puts (%.1f per batch, largest %ld), batched-copy refusals %ld\n",
              (int)getpid(), chan_batches.load(), chan_batch_ops.load(), chan_batch_ops.load() / (double)chan_batches.load(),
              chan_batch_max.load(), chan_batch_fallback.load());
    if (total == 0) return;
    fprintf(stderr, "[zc-stats] pid=%d MEMCPY=%ld IPC=%ld OTHER=%ld "
                    "(same-process %.1f%% of %ld)\n",
            (int)getpid(), m, i, o, 100.0 * m / total, total);
    for (int s = 0; s < 3; s++) {
      const long n = recv_n[s].load();
      if (n == 0) continue;
      const long us = recv_us[s].load(), by = recv_bytes[s].load();
      // mean us/receive and mean bytes/receive: fit alpha and beta across the
      // three rows and the result already carries the real contention.
      fprintf(stderr, "[zc-time] pid=%d %-6s recvs=%ld bytes=%ld total_us=%ld "
                      "mean_us=%.3f mean_bytes=%.1f\n",
              (int)getpid(), slotName(s), n, by, us, (double)us / n,
              (double)by / n);
    }
    if (host_send_n.load() + host_recv_n.load() > 0)
      fprintf(stderr, "[zc-host] pid=%d send: n=%ld total_ms=%.1f mean_us=%.1f | recv-issue: n=%ld "
                      "total_ms=%.1f mean_us=%.1f | complete: n=%ld total_ms=%.1f mean_us=%.1f\n",
              (int)getpid(), host_send_n.load(), host_send_us.load() / 1e3,
              host_send_n.load() ? (double)host_send_us.load() / host_send_n.load() : 0.0,
              host_recv_n.load(), host_recv_us.load() / 1e3,
              host_recv_n.load() ? (double)host_recv_us.load() / host_recv_n.load() : 0.0,
              host_done_n.load(), host_done_us.load() / 1e3,
              host_done_n.load() ? (double)host_done_us.load() / host_done_n.load() : 0.0);
    for (int k = 0; k < 2; k++) {
      const long cn = credit_n[k].load();
      if (cn > 0)
        fprintf(stderr, "[zc-host] pid=%d channel credit (%s): n=%ld alloc mean_us=%.2f send mean_us=%.2f\n",
                (int)getpid(), k ? "other host" : "same host", cn,
                (double)credit_alloc_us[k].load() / cn, (double)credit_send_us[k].load() / cn);
    }
    for (int k = 0; k < 3; k++) {
      const long pn = put_n[k].load();
      if (pn > 0)
        fprintf(stderr, "[zc-host] pid=%d channel put (%s): n=%ld host mean_us=%.2f\n", (int)getpid(),
                k == 0 ? "same host" : k == 1 ? "other host" : "unresolved, queued", pn,
                (double)put_us[k].load() / pn);
    }
    if (put_pending.load() > 0 || event_n.load() > 0)
      fprintf(stderr, "[zc-host] pid=%d channel puts queued for a credit or resolution: %ld; events taken: %ld; notes by immediate data: %ld\n",
              (int)getpid(), put_pending.load(), event_n.load(), signal_n.load());
    const long bf = batch_flushes.load();
    if (bf > 0)
      fprintf(stderr, "[zc-batch] pid=%d batches=%ld ops=%ld mean_ops=%.1f max_ops=%ld "
                      "fallbacks=%ld call_us_total=%ld call_us_mean=%.1f\n",
              (int)getpid(), bf, batch_ops.load(), (double)batch_ops.load() / bf,
              batch_max.load(), batch_fallback.load(), batch_us.load(), (double)batch_us.load() / bf);
    fflush(stderr);
  }
};
ZcModeStats zc_mode_stats;

// Adds the scope's wall time to one of the host counters above; free when the
// statistics are off (one cached env lookup).
struct ZcHostTimer {
  std::atomic<long>* us; std::atomic<long>* n; double t0;
  ZcHostTimer(std::atomic<long>& us_, std::atomic<long>& n_)
      : us(&us_), n(&n_), t0(zcStatsOnFast() ? CkWallTimer() : 0.0) {}
  ~ZcHostTimer() {
    if (t0 == 0.0) return;
    us->fetch_add((long)((CkWallTimer() - t0) * 1e6), std::memory_order_relaxed);
    n->fetch_add(1, std::memory_order_relaxed);
  }
};
}  // namespace

// A persistent endpoint's cross-host transfer: one-sided, its completion raised
// on the initiator process -- on whichever PE runs the network progress -- and
// handed back to the initiating PE, which fires both endpoints' callbacks. The
// op info is created when the transfer is requested and issued when its source
// is ready. Methods with the Direct API below.
// ---------------------------------------------------------------------------
// Cross-host puts: the arrival note rides on the data (RDMA write with
// immediate data) instead of following it as a message.
//
// LCI passes 16 bits of immediate data to the target: a slot in this
// process's signal table, allocated by the owner per resolution and carried
// to the sender in the Resolved reply. The per-put facts the receiver needs
// (sequence, bytes, the application's tag) do not fit, so the sender writes
// them into the slot's 16-byte header, a registered host region of this
// process, with an inject-sized put just before the data write. The two
// writes are not ordered on this fabric, so the owner PE waits for the
// header's sequence to advance before it builds the event. The completion
// arrives on whichever thread runs the network; it is pushed to the owner's
// PE. A tag or size that does not fit, or an exhausted table, falls back to
// the note message.
// ---------------------------------------------------------------------------
namespace {
constexpr int CHAN_SIGNAL_SLOTS = 65536;   // 16 immediate bits
struct ChanSignalHdr {
  std::atomic<uint64_t> seq;   // the put's sequence; a new value means the header is in
  uint32_t bytes;
  uint32_t tag;
};
struct ChanSignalEntry {
  std::atomic<bool> used;
  int pe;                      // the owner's PE when it replied: where the completion is pushed
  CkCallback arrived;
  uint64_t channel, generation, sender_token, last_seq;
  uint64_t final_seq;          // the sender's acked last put: the slot is freed once that one has been taken (0: none yet)
  int sender_pe;
};
struct ChanSignalMsg { char core[CmiMsgHeaderSizeBytes]; int slot; uint64_t size; int rank; };

std::mutex chan_signal_lock;              // table allocation, process-wide
ChanSignalEntry* chan_signal_table = nullptr;
// CHARM_DEBUG_SIGNAL: the cross-host signal path alone (slots, headers, notes, resolutions and closings),
// a few lines per put rather than the whole protocol trace of CHARM_DEBUG_MIGRATE.
static bool chanSignalDebug() { static const bool on = getenv("CHARM_DEBUG_SIGNAL") != nullptr || getenv("CHARM_DEBUG_MIGRATE") != nullptr; return on; }
ChanSignalHdr* chan_signal_hdr = nullptr; // the registered header region
CmiNcpyBuffer chan_signal_hdr_region;     // its registration, pupped into Resolved replies
std::vector<int> chan_signal_free;
std::once_flag chan_signal_once;
bool chan_signal_ready = false;
}  // namespace
int device_channel_signal_handler;
// Watchdog counters for the immediate-note path (any thread may count).
static std::atomic<long> chan_signal_dropped_retired{0};   // notes that found their slot retired (owner packed/closed)
static std::atomic<long> chan_signal_dropped_range{0};     // notes with a slot outside the table
static std::atomic<long> chan_signal_seq_jumps{0};         // headers taken whose seq skipped one or more puts
static std::atomic<long> chan_signal_seq_stale{0};         // headers whose seq went backwards (a reused slot)
static char chan_signal_last_drop[160] = "";
static char chan_signal_last_jump[200] = "";

// Per PE: a registered ring of header sources, one entry per put; an entry is
// reused 4096 puts later, long after its write has been read.
constexpr int CHAN_HDR_RING = 4096;
CkpvStaticDeclare(CmiNcpyBuffer*, device_channel_hdr_src);
CkpvStaticDeclare(unsigned, device_channel_hdr_next);
CkpvStaticDeclare(bool, device_channel_late_init_done);
void deviceChannelLateInit();

static void deviceChannelSignalFromNet(int rank, uint32_t tag, size_t size);
static void chanSignalInit() {
  const size_t hbytes = sizeof(ChanSignalHdr) * (size_t)CHAN_SIGNAL_SLOTS;
  void* h = nullptr;
  if (posix_memalign(&h, 4096, hbytes) != 0) CkAbort("CkDeviceChannel: header region allocation failed\n");
  memset(h, 0, hbytes);
  chan_signal_hdr = (ChanSignalHdr*)h;
  // Registered once and never deregistered: a completion must not send the
  // zero-copy API's deregistration message for it.
  chan_signal_hdr_region = CmiNcpyBuffer(h, hbytes, CMK_BUFFER_REG, CMK_BUFFER_NODEREG);
  chan_signal_table = new ChanSignalEntry[CHAN_SIGNAL_SLOTS];
  for (int i = 0; i < CHAN_SIGNAL_SLOTS; i++) { chan_signal_table[i].used.store(false); chan_signal_table[i].last_seq = 0; }
  chan_signal_free.reserve(CHAN_SIGNAL_SLOTS);
  for (int i = CHAN_SIGNAL_SLOTS - 1; i > 0; i--) chan_signal_free.push_back(i);   // slot 0 kept unused
  CmiSetRputSignalHandler(deviceChannelSignalFromNet);
  chan_signal_ready = true;
}

// Owner: a slot for the resolution it is about to grant; -1 when none is free.
static int chanSignalAlloc(const CkCallback& arrived, uint64_t channel, uint64_t generation,
                           int sender_pe, uint64_t sender_token) {
  if (!chan_signal_ready) return -1;
  std::lock_guard<std::mutex> lk(chan_signal_lock);
  if (chan_signal_free.empty()) return -1;
  const int slot = chan_signal_free.back(); chan_signal_free.pop_back();
  ChanSignalEntry& e = chan_signal_table[slot];
  e.pe = CkMyPe(); e.arrived = arrived; e.channel = channel; e.generation = generation;
  e.sender_pe = sender_pe; e.sender_token = sender_token;
  chan_signal_hdr[slot].seq.store(0, std::memory_order_release);   // nobody writes a retired slot; start clean
  chan_signal_hdr[slot].bytes = 0; chan_signal_hdr[slot].tag = 0;
  e.last_seq = 0;                                                  // a sender's sequences start at 1
  e.final_seq = 0;
  e.used.store(true, std::memory_order_release);
  if (chanSignalDebug())
    CmiPrintf("[CHAN-HDR %d] slot %d allocated to channel %llx gen %llu for pe %d token %llx\n", CkMyPe(), slot,
              (unsigned long long)channel, (unsigned long long)generation, sender_pe, (unsigned long long)sender_token);
  return slot;
}
static void chanSignalFree(int slot) {
  if (slot <= 0 || !chan_signal_ready) return;
  std::lock_guard<std::mutex> lk(chan_signal_lock);
  chan_signal_table[slot].used.store(false, std::memory_order_release);
  chan_signal_free.push_back(slot);
}
static std::atomic<long> chan_signal_release_now{0}, chan_signal_release_deferred{0}, chan_signal_release_late{0};
// The owner's resolution is closed and the sender acked with its last put's
// sequence: the slot goes back only once the bridge has taken that put (its
// note may still be in flight behind the ack); UINT64_MAX = a blind ack.
// Runs on the PE the slot was granted on (e.pe): the bridge's thread.
static void chanSignalRelease(int slot, uint64_t final_seq) {
  if (slot <= 0 || slot >= CHAN_SIGNAL_SLOTS || !chan_signal_ready) return;
  ChanSignalEntry& e = chan_signal_table[slot];
  if (!e.used.load(std::memory_order_acquire)) return;
  if (chanSignalDebug())
    CmiPrintf("[CHAN-HDR %d] release slot %d (channel %llx): final_seq %llu, last_seq %llu -> %s\n", CkMyPe(), slot,
              (unsigned long long)e.channel, (unsigned long long)final_seq, (unsigned long long)e.last_seq,
              (final_seq == UINT64_MAX || final_seq == 0 || e.last_seq >= final_seq) ? "freed now" : "deferred");
  if (final_seq == UINT64_MAX || final_seq == 0 || e.last_seq >= final_seq) {
    chan_signal_release_now.fetch_add(1, std::memory_order_relaxed);
    chanSignalFree(slot);
    return;
  }
  e.final_seq = final_seq;
  chan_signal_release_deferred.fetch_add(1, std::memory_order_relaxed);
}
struct DeviceChannelSlotRelease {
  char core[CmiMsgHeaderSizeBytes];
  int slot;
  uint64_t final_seq;
};
int device_channel_slotrelease_handler;
extern "C" void* device_channel_slotrelease_bridge(void* arg) {
  auto* m = (DeviceChannelSlotRelease*)arg;
  chanSignalRelease(m->slot, m->final_seq);
  CmiFree(m);
  return NULL;
}
// Always a message, even within the process: the release must run on the
// granting PE's thread, where the bridge reads and writes the entry.
static void deviceChannelSlotRelease(int pe, int slot, uint64_t final_seq) {
  if (pe < 0 || slot <= 0) return;
  auto* m = (DeviceChannelSlotRelease*)CmiAlloc(sizeof(DeviceChannelSlotRelease));
  m->slot = slot; m->final_seq = final_seq;
  CmiSetHandler(m, device_channel_slotrelease_handler);
  CmiSyncSendAndFree(pe, sizeof(DeviceChannelSlotRelease), (char*)m);
}

// The network's thread: push the completion to the owner's PE.
static void deviceChannelSignalFromNet(int rank, uint32_t tag, size_t size) {
  const int slot = (int)tag;
  if (slot <= 0 || slot >= CHAN_SIGNAL_SLOTS) { chan_signal_dropped_range.fetch_add(1, std::memory_order_relaxed); return; }
  ChanSignalEntry& e = chan_signal_table[slot];
  if (!e.used.load(std::memory_order_acquire)) {   // retired: the owner packed or closed while this note was in flight
    chan_signal_dropped_retired.fetch_add(1, std::memory_order_relaxed);
    snprintf(chan_signal_last_drop, sizeof(chan_signal_last_drop), "slot %d from rank %d, %zu bytes, owner pe %d channel %llx last_seq %llu",
             slot, rank, size, e.pe, (unsigned long long)e.channel, (unsigned long long)e.last_seq);
    return;
  }
  auto* m = (ChanSignalMsg*)CmiAlloc(sizeof(ChanSignalMsg));
  m->slot = slot; m->size = size; m->rank = rank;
  CmiSetHandler(m, device_channel_signal_handler);
  CmiPushPE(CmiRankOf(e.pe), m);
}

// The owner's PE: wait for the header, build the event the note would have carried.
extern "C" void* device_channel_signal_bridge(void* arg) {
  auto* m = (ChanSignalMsg*)arg;
  ChanSignalEntry& e = chan_signal_table[m->slot];
  if (e.used.load(std::memory_order_acquire)) {
    ChanSignalHdr& h = chan_signal_hdr[m->slot];
    const double t0 = CkWallTimer();
    uint64_t seq;
    if (chanSignalDebug())
      CmiPrintf("[CHAN-HDR %d] note for slot %d (channel %llx, from rank %d, %zu bytes): header seq %llu, last %llu\n", CkMyPe(),
                m->slot, (unsigned long long)e.channel, m->rank, m->size, (unsigned long long)h.seq.load(), (unsigned long long)e.last_seq);
    while ((seq = h.seq.load(std::memory_order_acquire)) == e.last_seq) {
      if (CkWallTimer() - t0 > 2.0) {
        for (int d = -2; d <= 2; d++) {
          const int sl = m->slot + d;
          if (sl <= 0 || sl >= CHAN_SIGNAL_SLOTS) continue;
          CmiPrintf("[CHAN-HDR %d]   slot %d: used %d channel %llx last_seq %llu final %llu header seq %llu bytes %u tag %u\n", CkMyPe(), sl,
                    (int)chan_signal_table[sl].used.load(), (unsigned long long)chan_signal_table[sl].channel,
                    (unsigned long long)chan_signal_table[sl].last_seq, (unsigned long long)chan_signal_table[sl].final_seq,
                    (unsigned long long)chan_signal_hdr[sl].seq.load(), chan_signal_hdr[sl].bytes, chan_signal_hdr[sl].tag);
        }
        CkAbort("CkDeviceChannel: the header of a put with signal never arrived (slot %d, channel %llx, seq still %llu, header region %p)\n",
                m->slot, (unsigned long long)e.channel, (unsigned long long)e.last_seq, (void*)chan_signal_hdr);
      }
    }
    if (e.last_seq != 0 && (seq < e.last_seq || seq > e.last_seq + 1)) {
      (seq < e.last_seq ? chan_signal_seq_stale : chan_signal_seq_jumps).fetch_add(1, std::memory_order_relaxed);
      snprintf(chan_signal_last_jump, sizeof(chan_signal_last_jump), "slot %d channel %llx: header seq %llu after %llu (%u bytes tag %u) on pe %d",
               m->slot, (unsigned long long)e.channel, (unsigned long long)seq, (unsigned long long)e.last_seq, h.bytes, h.tag, CkMyPe());
    }
    e.last_seq = seq;
    { static thread_local bool said = false; if (!said) { said = true; CmiPrintf("[CHAN-INIT %d] first put received as immediate data (slot %d, seq %llu, %u bytes, waited %.1f us for the header)\n", CkMyPe(), m->slot, (unsigned long long)seq, h.bytes, (CkWallTimer() - t0) * 1e6); } }
    CkDeviceChannelEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = CkDeviceChannelEvent::DATA;
    ev.channel = e.channel; ev.generation = e.generation; ev.seq = seq;
    ev.bytes = h.bytes; ev.tag = h.tag;
    ev.sender_pe = e.sender_pe; ev.sender_token = e.sender_token;
    if (m->size != 0 && m->size != (uint64_t)h.bytes) {
      static thread_local bool said = false;
      if (!said) { said = true; CmiPrintf("[CHAN %d] put with signal: completion length %llu, header says %u bytes (channel %llx); using the header\n", CkMyPe(), (unsigned long long)m->size, h.bytes, (unsigned long long)e.channel); }
    }
    CkDataMsg* note = CkDataMsg::buildNew((int)sizeof(ev), &ev);
    CkSetRefNum(note, (CMK_REFNUM_TYPE)ev.tag);
    e.arrived.send(note);
    if (zcStatsOnFast()) zc_mode_stats.signal_n.fetch_add(1, std::memory_order_relaxed);
    if (e.final_seq != 0 && seq >= e.final_seq) { chan_signal_release_late.fetch_add(1, std::memory_order_relaxed); chanSignalFree(m->slot); }
  }
  CmiFree(m);
  return NULL;
}

// Set by the channel's issue() for the put that follows; taken by
// CkDevicePersistent::put when it builds the transfer's context.
struct DeviceChannelSignalReq { int slot; CmiNcpyBuffer hdr; uint64_t seq; uint32_t bytes, tag; };
static thread_local DeviceChannelSignalReq* device_channel_signal_req = nullptr;

struct DevicePersistentRdmaCtx {
  int remaining;                  // one-sided operations of this transfer still to complete
  std::vector<NcpyOperationInfo*> infos;   // one per chunk (the first is also `info`)
  int pe;              // the initiating PE: completion is finished there
  CkCallback local_cb;
  void* local_msg;
  CkCallback remote_cb;
  void* remote_msg;
  NcpyOperationInfo* info;
  // A channel put whose note rides as immediate data: the slot, and the header
  // (sequence, bytes, tag) written just before the data. Set by
  // persistentRdmaBegin; -1 / null means a plain transfer.
  int signal_slot;
  NcpyOperationInfo* hdr_info;   // unused: the header goes by CmiRdmaPutRaw
  uint64_t hdr_seq;
  uint32_t hdr_bytes, hdr_tag;
  ChanSignalHdr* hdr_src;        // this put's entry in the PE's header ring (registered once)
  const void* hdr_src_layer;     // the ring's registration block
  int hdr_dst_pe;                // the owner process's header region: a PE of it, the slot's address,
  void* hdr_dst;                 // and a copy of the region's registration block
  const void* hdr_dst_layer;
  std::array<char, CMK_NOCOPY_DIRECT_BYTES> hdr_dst_layer_copy;
};
static void persistentRdmaDone(DevicePersistentRdmaCtx* ctx);

void CkRdmaDeviceRecvHandler(void* data)
{
  NcpyOperationInfo *ncpy_op_info = (NcpyOperationInfo *)data;

  if (ncpy_op_info->opMode == CMK_DEVICE_PERSISTENT_OP) {
    // A persistent endpoint's one-sided put or get. The completion is raised in
    // the initiator process but on whichever PE ran the network progress; the
    // sender's bookkeeping (its copy's token table, its element's outstanding
    // count) is per PE, so finish on the initiating PE, through the same
    // loopback the entry-method receives use.
    DevicePersistentRdmaCtx* ctx = (DevicePersistentRdmaCtx*)ncpy_op_info->refPtr;
    if (ctx->pe != CmiMyPe()) {
      int infoSize = ncpy_op_info->ncpyOpInfoSize;
      NcpyOperationInfo* copy = (NcpyOperationInfo*)CmiAlloc(infoSize);
      memcpy(copy, ncpy_op_info, infoSize);
      LoopBackMsg* conv_msg = (LoopBackMsg*)CmiAlloc(sizeof(LoopBackMsg));
      conv_msg->msg = copy;
      QdCreate(1);
      CmiSetHandler(conv_msg, loopback_handler);
      CmiPushPE(CmiRankOf(ctx->pe), conv_msg);
      return;
    }
    persistentRdmaDone(ctx);
    return;
  }

  if (ncpy_op_info->opMode == CMK_DEVICE_RESTAGE_PUT) {
    // A put raises its completion on the initiator only, and for a correction
    // the initiator is the sender. deviceRdmaOpInfo names the receiver's
    // DeviceRdmaOp, which is a pointer into that other process and must not be
    // followed here -- so tell the receiver the payload has landed and let it
    // resolve the op. destPe is a plain int in the ncpyOpInfo, so it is the one
    // piece of the destination that is safe to read from this side.
    notifyDeviceRestagePut(ncpy_op_info->destPe,
                           ncpy_op_info->deviceRdmaOpInfo);
    return;
  }

  DeviceRdmaOp* op = (DeviceRdmaOp*)(ncpy_op_info->deviceRdmaOpInfo);

  if(op->dest_pe != CmiMyPe()) {
        int infoSize = ncpy_op_info->ncpyOpInfoSize;
        NcpyOperationInfo* copy = (NcpyOperationInfo*)CmiAlloc(infoSize);
        memcpy(copy, ncpy_op_info, infoSize);

        LoopBackMsg* conv_msg = (LoopBackMsg*)CmiAlloc(sizeof(LoopBackMsg));
        conv_msg->msg = copy;

        QdCreate(1);
        CmiSetHandler(conv_msg, loopback_handler);
        CmiPushPE(CmiRankOf(op->dest_pe), conv_msg);
        return;
  }

  QdProcess(1);
  DeviceRdmaInfo* info = op->info;

  // Invoke source callbacks
  if (op->src_cb) {
    CkCallback* cb = (CkCallback*)op->src_cb;
    cb->send();
    delete cb;
  }

  // Cross-node tier, per op: this fires for every completed rget, whether or
  // not the receive it belongs to ever completes as a whole.
  if (op->rget_posted > 0.0) {
    zcRecordRecvTime(2, op->size, CkWallTimer() - op->rget_posted);
    op->rget_posted = 0.0;
  }

  // Update counter (there may be multiple buffers in transit)
  info->counter++;

  // Check if all buffers have been received
  // If so, invoke regular entry method
  if (info->counter == info->n_ops) {
    if (info->msg == nullptr) {  // a staged pull for a parked message: nothing to deliver
      CmiFree(info);
      return;
    }
    QdCreate(1);
    { static const bool mt = getenv("CHARM_MIG_TRACE") != nullptr;
      if (mt) { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        CmiPrintf("[MIGT %d] landed id=0 ptr=%p t=%.6f\n", CkMyPe(), (const void*)ncpy_op_info->destPtr, ts.tv_sec + ts.tv_nsec * 1e-9); } }

    enqueueNcpyMessage(op->dest_pe, info->msg);

    // Free RDMA metadata
    // CmiFree(info);
  }
}
static void deviceRecvWatchDrop(DeviceRdmaInfo* info);  // defined by the stall watch below
// Defined just below the zerocopy stats, which live in an anonymous namespace
// this declaration cannot name. The completion path above is where a receive's
// elapsed time becomes known, hence the forward declaration.

// Invoked when a GPU buffer arrives on the receiver
void CkRdmaDeviceRecvHandler(void* data, void* msg)
{
  ZcHostTimer zc_host_timer(zc_mode_stats.host_done_us, zc_mode_stats.host_done_n);
  DeviceRdmaOp* op = (DeviceRdmaOp*)data;
  DeviceRdmaInfo* info = op->info;

  // The copy out of the sender's IPC slot has landed (this runs behind it on
  // the receive stream), so the sender may take the slot back. Raised here,
  // at completion, rather than when the copy was issued: that is what lets
  // reclaimCompletedIpcEvents trust the flag alone and query no event.
  // The sender clears it when it reclaims the slot, so finding it already set
  // means a second receive is signalling the same (device, event) pair before
  // the first was retired -- the sender would then free a block another
  // transfer is still reading.
  if (op->ipc_event_idx >= 0) {
    hapi_ipc_event_shared* sh = ipcSharedSlot(op->ipc_device_idx, op->ipc_event_idx);
    if (sh->dst_flag.exchange(true, std::memory_order_acq_rel)) {
      CmiPrintf("[%d] IPC DUPLICATE dst_flag dev_idx=%d ev_idx=%d srcPe=%d cnt=%zu\n",
                CkMyPe(), op->ipc_device_idx, op->ipc_event_idx, op->src_pe, op->size);
      fflush(stdout);
    }
    op->ipc_event_idx = -1;
  }

  // Invoke source callbacks
  if (op->src_cb) {
    CkCallback* cb = (CkCallback*)op->src_cb;
    cb->send();
    delete cb;
  }

  // Cross-node tier, per op: this fires for every completed rget, whether or
  // not the receive it belongs to ever completes as a whole.
  if (op->rget_posted > 0.0) {
    zcRecordRecvTime(2, op->size, CkWallTimer() - op->rget_posted);
    op->rget_posted = 0.0;
  }

  // Update counter (there may be multiple buffers in transit)
  info->counter++;

  // Check if all buffers have been received
  // If so, invoke regular entry method
  if (info->counter == info->n_ops) {
    if (info->msg == nullptr) {
      // A staged pull for a parked message (CkRdmaDeviceStageParked): the
      // senders are complete, the payload sits in landing buffers, and the
      // message itself is delivered when its element lands.
      deviceRecvWatchDrop(info);
      CmiFree(info);
      return;
    }
    QdCreate(1);

    // The receive is complete here, which is the moment the application can
    // act on it, so this is the interval worth charging to the transport.
    if (info->zc_posted > 0.0)
      zcRecordRecvTime(info->zc_mode, info->zc_bytes,
                       CkWallTimer() - info->zc_posted);

    enqueueNcpyMessage(op->dest_pe, info->msg);

    // Free RDMA metadata
    deviceRecvWatchDrop(info);
    CmiFree(info);
  }
}

/****************************** Direct (Persistent) API ******************************/
//
// An endpoint is exported once by its owner and then read or written repeatedly
// by a peer, each transfer carrying nothing but the two endpoints. The three
// transports are the ones every device message uses, minus the per-message
// protocol: inside a process a stream memcpy; on the same host a device-to-
// device copy through the IPC import cache (the export comes from
// hapiIpcExportBuffer, so pool and vmm memory work, and the mapping is the
// cache's, shared with every other transfer into that region); across hosts a
// one-sided RDMA put or get on registrations taken at open(), issued and
// completed the way the correction put is (issueDeviceRestagePut).

void CkDevicePersistent::init() {
  pe = CkMyPe();
  cb_msg = nullptr;
  ipc_offset = 0;
  ipc_base = nullptr;
  ipc_exported = false;
  ipc_ptr = nullptr;
  ipc_open = false;
  rdma = CmiNcpyBuffer();
  rdma_registered = false;
  rdma_owned = false;
}

void CkDevicePersistent::open() {
  // Same-host peers in other processes read this buffer through the import
  // cache, so the export must be the one hapiIpcExportBuffer produces: a raw
  // cudaIpcGetMemHandle cannot name vmm pool memory, and a mapping opened
  // outside the cache collides with the one the cache holds for the region
  // (cudaErrorAlreadyMapped).
  if (!hapiIpcExportBuffer(ptr, &hapi_ipc_handle, &ipc_offset, &ipc_base)) {
    CkAbort("CkDevicePersistent::open: %p (%zu bytes) is not exportable device memory; "
            "managed or host memory cannot be a persistent endpoint\n", ptr, cnt);
  }
  ipc_exported = true;

  // Peers on other hosts address it by RDMA. Register here, once, so that no
  // transfer pays for it; under +gpupool this is the arena's one registration.
  if (CmiNumPhysicalNodes() > 1 && cnt > 0) {
    void* seg_base = nullptr;
    size_t seg_extent = 0;
    const bool pooled = hapiDevPoolOn() && hapiDevPoolSegmentOf(ptr, cnt, &seg_base, &seg_extent);
    rdma = acquireDeviceRegistration(ptr, cnt, nullptr);
    rdma_registered = true;
    // The arena's registration and a cached one outlive this endpoint; only a
    // registration made for it alone is this endpoint's to retire.
    rdma_owned = !pooled && CkpvAccess(device_mr_cache) == NULL;
  }
}

void CkDevicePersistent::close() {
  if (pe == CkMyPe()) {
    // The owner, about to free or reuse the buffer: drop the export (the cache
    // is keyed by allocation base and addresses are recycled) and an owned
    // registration.
    if (ipc_exported) hapiIpcInvalidateExport(ptr);
    if (rdma_registered && rdma_owned) rdma.deregisterMem();
    ipc_exported = false;
    rdma_registered = false;
    rdma_owned = false;
  }
  // A peer's copy forgets its view. The mapping belongs to the import cache,
  // which keeps it for every other transfer that reaches this region, so it is
  // not closed here.
  ipc_ptr = nullptr;
  ipc_open = false;
}

void CkDevicePersistent::set_msg(void* msg) {
  cb_msg = msg;
}

void CkDevicePersistent::pup(PUP::er& p) {
  p((char*)&ptr, sizeof(ptr));
  p|cnt;
  p|pe;
  p|cb;
  p((char*)&hapi_ipc_handle, sizeof(hapi_ipc_handle));
  p|ipc_offset;
  p((char*)&ipc_base, sizeof(ipc_base));
  p|ipc_exported;
  p|rdma;
  p|rdma_registered;
  if (p.isUnpacking()) {
    // Local state of the copy: nothing of the owner's travels.
    cb_msg = nullptr;
    hapi_stream = hapiStreamPerThread;
    ipc_ptr = nullptr;
    ipc_open = false;
    rdma_owned = false;
  }
}

// The peer's view of a same-host endpoint, through the import cache.
static void* persistentImport(CkDevicePersistent& e) {
  if (!e.ipc_open) {
    if (!e.ipc_exported) {
      CkAbort("CkDevicePersistent: the endpoint of PE %d was never opened; call open() "
              "before sending it to a peer\n", e.pe);
    }
    void* mapped = hapiIpcImportBuffer(e.hapi_ipc_handle, CmiNodeOf(e.pe), e.ipc_base,
                                       e.ipc_offset + e.cnt);
    if (mapped == nullptr) {
      CkAbort("CkDevicePersistent: PE %d could not import the endpoint of PE %d (%s)\n",
              CkMyPe(), e.pe, hapiIpcLastImportErrorName());
    }
    e.ipc_ptr = (char*)mapped + e.ipc_offset;
    e.ipc_open = true;
  }
  return e.ipc_ptr;
}

// A copy with a peer's imported buffer on one side. On HIP it goes through the batch API with one
// entry: hipMemcpyAsync on a pointer into a peer's imported vmm heap (hipMemImportFromShareableHandle +
// hipMemMap) crashes in ROCr's pointer lookup (Runtime::VMemoryPtrInfo, ROCm 7.14.1 on MI250X), whatever
// the kind, and hipMemcpyBatchAsync takes the same pointers (Frontier job 5833510; the two batch
// flushes below use it for every size for the same reason). CUDA keeps the plain copy.
static inline void deviceImportedCopyAsync(void* dst, const void* src, size_t bytes, hapiStream_t st) {
#if CMK_HIP
  hipMemcpyAttributes attr{};
  attr.srcAccessOrder = hipMemcpySrcAccessOrderStream;
  size_t attr_idx = 0, fail_idx = 0;
  void* d = dst; const void* s = src; size_t n = bytes;
  if (hipMemcpyBatchAsync(&d, const_cast<void**>(&s), &n, 1, &attr, &attr_idx, 1, &fail_idx, st) == hipSuccess) return;
  hapiGetLastError();
#endif
  hapiCheck(hapiMemcpyAsync(dst, src, bytes, hapiMemcpyDefault, st));
}

// Both endpoints' callbacks, behind the copy on the initiator's stream.
static void persistentStreamCallbacks(hapiStream_t stream, CkDevicePersistent& a,
                                      CkDevicePersistent& b) {
  if (a.cb.type != CkCallback::ignore) hapiAddCallback(stream, a.cb, a.cb_msg);
  if (b.cb.type != CkCallback::ignore) hapiAddCallback(stream, b.cb, b.cb_msg);
}

static void persistentRdmaDone(DevicePersistentRdmaCtx* ctx) {
  if (--ctx->remaining > 0) return;   // more chunks of this transfer in flight
  QdProcess(1);   // raised when the transfer was requested
  if (ctx->local_cb.type != CkCallback::ignore) ctx->local_cb.send(ctx->local_msg);
  if (ctx->remote_cb.type != CkCallback::ignore) ctx->remote_cb.send(ctx->remote_msg);
  delete ctx;
}

static DevicePersistentRdmaCtx* persistentRdmaBegin(CkDevicePersistent& local,
                                                    CkDevicePersistent& remote) {
  auto* ctx = new DevicePersistentRdmaCtx{1, {}, CkMyPe(), local.cb, local.cb_msg, remote.cb,
                                          remote.cb_msg, nullptr, -1, nullptr, 0, 0, 0, nullptr, nullptr, -1, nullptr, nullptr, {}};
  // Completions of one-sided device transfers all come through this handler.
  CmiSetDirectNcpyAckHandler(CkRdmaDeviceRecvHandler);
  QdCreate(1);   // released by persistentRdmaDone
  return ctx;
}

// The producer of the source has finished on the sender's stream: issue the put.
static void persistentRputReadyFn(void* param, void*) {
  auto* ctx = (DevicePersistentRdmaCtx*)param;
  if (ctx->signal_slot >= 0 && ctx->infos.empty()) {
    // The header, from this PE's registered scratch (inject-sized: copied at
    // the post, so the scratch is free again when CmiIssueRput returns), then
    // the data write whose immediate is the slot.
    ChanSignalHdr* h = ctx->hdr_src;   // this put's ring entry
    h->bytes = ctx->hdr_bytes; h->tag = ctx->hdr_tag;
    h->seq.store(ctx->hdr_seq, std::memory_order_release);
    { static thread_local bool said = false; if (!said) { said = true; CmiPrintf("[CHAN-INIT %d] first put with immediate data (slot %d, %u bytes)\n", CkMyPe(), ctx->signal_slot, ctx->hdr_bytes); } }
    if (chanSignalDebug())
      CmiPrintf("[CHAN-HDR %d] put seq %llu: header to slot %d at %p (process of pe %d), %u bytes tag %u\n", CkMyPe(),
                (unsigned long long)ctx->hdr_seq, ctx->signal_slot, ctx->hdr_dst, ctx->hdr_dst_pe, ctx->hdr_bytes, ctx->hdr_tag);
    CmiRdmaPutRaw(ctx->hdr_dst_pe, h, sizeof(ChanSignalHdr), ctx->hdr_src_layer, ctx->hdr_dst, ctx->hdr_dst_layer);
    CmiIssueRputSignal(ctx->info, (uint32_t)ctx->signal_slot);
    return;
  }
  if (ctx->infos.empty()) CmiIssueRput(ctx->info);
  else for (NcpyOperationInfo* op : ctx->infos) CmiIssueRput(op);
}

// A large one-sided put is issued as this many concurrent operations
// (+gpuputchunks, default 1 until measured): one RDMA write from device memory
// ran at ~11 GB/s against the zero-copy path's ~14 GB/s on two nodes.
static int persistentPutChunks() {
  static int n = -1;
  if (n < 0) {
    n = 1;
    CmiGetArgIntDesc(CkGetArgv(), "+gpuputchunks", &n,
                     "persistent device put: number of concurrent one-sided operations per transfer");
    if (n < 1) n = 1;
  }
  return n;
}

// The initiator's registration of its own buffer: the one taken at open(), or
// one taken now for a buffer that was never opened (a send buffer that no peer
// addresses need not be).
static CmiNcpyBuffer persistentLocalRegistration(CkDevicePersistent& e) {
  if (e.rdma_registered) return e.rdma;
  return acquireDeviceRegistration(e.ptr, e.cnt, nullptr);
}

CkDeviceStatus CkDevicePersistent::get(CkDevicePersistent& src) {
  // Check that the source buffer fits into the destination buffer
  if (cnt < src.cnt) {
    CkAbort("CkDevicePersistent::get: Destination buffer is smaller than source buffer\n");
  }

  CkNcpyModeDevice mode = findTransferModeDevice(src.pe, CkMyPe());

  if (mode == CkNcpyModeDevice::MEMCPY) {
    hapiMemcpyAsync((void*)ptr, src.ptr, src.cnt, hapiMemcpyDefault, hapi_stream);
    persistentStreamCallbacks(hapi_stream, src, *this);
  } else if (mode == CkNcpyModeDevice::IPC) {
    deviceImportedCopyAsync((void*)ptr, persistentImport(src), src.cnt, hapi_stream);
    persistentStreamCallbacks(hapi_stream, src, *this);
  } else {
    // One-sided read of the peer's registered buffer into this one. The peer's
    // data must be complete, as with the other two modes; the write into this
    // buffer is outside every stream, so the callbacks are what order kernels
    // that read it.
    if (!src.rdma_registered) {
      CkAbort("CkDevicePersistent::get: the endpoint of PE %d carries no RDMA registration; "
              "it must be open()ed in a job that spans several hosts\n", src.pe);
    }
    // The registrations span the buffers; the transfer is sized by the
    // endpoints' current counts (a caller may shrink cnt below the opened size).
    CmiNcpyBuffer dst_ncpy = persistentLocalRegistration(*this);
    dst_ncpy.cnt = src.cnt;
    CmiNcpyBuffer src_ncpy = src.rdma;
    src_ncpy.cnt = src.cnt;
    DevicePersistentRdmaCtx* ctx = persistentRdmaBegin(*this, src);
    ctx->info = dst_ncpy.createNcpyOpInfo(src_ncpy, dst_ncpy, /*ackSize=*/0, NULL, NULL,
                                          /*rootNode=*/-1, CMK_DEVICE_PERSISTENT_OP, ctx);
    CmiIssueRget(ctx->info);
  }

  return CkDeviceStatus::incomplete;
}

CkDeviceStatus CkDevicePersistent::put(CkDevicePersistent& dst) {
  // Check that the source buffer fits into the destination buffer
  if (dst.cnt < cnt) {
    CkAbort("CkDevicePersistent::put: Destination buffer is smaller than source buffer\n");
  }

  CkNcpyModeDevice mode = findTransferModeDevice(CkMyPe(), dst.pe);

  if (mode == CkNcpyModeDevice::MEMCPY) {
    hapiMemcpyAsync((void*)dst.ptr, ptr, cnt, hapiMemcpyDefault, hapi_stream);
    persistentStreamCallbacks(hapi_stream, *this, dst);
  } else if (mode == CkNcpyModeDevice::IPC) {
    deviceImportedCopyAsync(persistentImport(dst), ptr, cnt, hapi_stream);
    persistentStreamCallbacks(hapi_stream, *this, dst);
  } else {
    // One-sided write into the peer's registered buffer. The NIC reads the
    // source outside every CUDA stream, so the put is issued from a callback
    // behind the producing work on hapi_stream rather than blocking the PE on
    // it (the correction put's rule, deviceRestagePutReady).
    if (!dst.rdma_registered) {
      CkAbort("CkDevicePersistent::put: the endpoint of PE %d carries no RDMA registration; "
              "it must be open()ed in a job that spans several hosts\n", dst.pe);
    }
    CmiNcpyBuffer src_ncpy = persistentLocalRegistration(*this);
    src_ncpy.cnt = cnt;
    CmiNcpyBuffer dst_ncpy = dst.rdma;
    dst_ncpy.cnt = cnt;
    DevicePersistentRdmaCtx* ctx = persistentRdmaBegin(*this, dst);
    if (DeviceChannelSignalReq* rq = device_channel_signal_req) {
      // The channel asked for the note as immediate data: the header goes by
      // a raw put from this PE's ring to the owner process's header region.
      device_channel_signal_req = nullptr;
      ctx->signal_slot = rq->slot; ctx->hdr_seq = rq->seq; ctx->hdr_bytes = rq->bytes; ctx->hdr_tag = rq->tag;
      CmiNcpyBuffer* ring = CkpvAccess(device_channel_hdr_src);
      const unsigned slot_in_ring = CkpvAccess(device_channel_hdr_next)++ % CHAN_HDR_RING;
      ctx->hdr_src = (ChanSignalHdr*)ring->ptr + slot_in_ring;
      ctx->hdr_src_layer = ring->layerInfo;
      ctx->hdr_dst_pe = rq->hdr.pe;
      ctx->hdr_dst = (void*)((const char*)rq->hdr.ptr + (size_t)rq->slot * sizeof(ChanSignalHdr));
      memcpy(ctx->hdr_dst_layer_copy.data(), rq->hdr.layerInfo, CMK_NOCOPY_DIRECT_BYTES);
      ctx->hdr_dst_layer = ctx->hdr_dst_layer_copy.data();
      delete rq;
    }
    const int chunks = (ctx->signal_slot >= 0) ? 1
                     : (cnt >= (size_t)(64 * 1024) * persistentPutChunks()) ? persistentPutChunks() : 1;
    if (chunks == 1) {
      ctx->info = src_ncpy.createNcpyOpInfo(src_ncpy, dst_ncpy, /*ackSize=*/0, NULL, NULL,
                                            /*rootNode=*/-1, CMK_DEVICE_PERSISTENT_OP, ctx);
    } else {
      // Concurrent writes of consecutive ranges of the same registered buffers;
      // the completion of the last one completes the transfer.
      ctx->remaining = chunks;
      const size_t per = (cnt + chunks - 1) / chunks;
      for (int k = 0; k < chunks; k++) {
        const size_t off = (size_t)k * per;
        if (off >= cnt) { ctx->remaining--; continue; }
        CmiNcpyBuffer s = src_ncpy, d = dst_ncpy;
        s.ptr = (const void*)((const char*)src_ncpy.ptr + off);
        d.ptr = (const void*)((const char*)dst_ncpy.ptr + off);
        s.cnt = d.cnt = std::min(per, cnt - off);
        ctx->infos.push_back(s.createNcpyOpInfo(s, d, /*ackSize=*/0, NULL, NULL,
                                                /*rootNode=*/-1, CMK_DEVICE_PERSISTENT_OP, ctx));
      }
      ctx->info = ctx->infos.empty() ? NULL : ctx->infos[0];
    }
    hapiAddCallback(hapi_stream, CkCallback(persistentRputReadyFn, (void*)ctx));
  }

  return CkDeviceStatus::incomplete;
}

/****************************** Persistent channels ******************************/
//
// CkDeviceChannel wraps a receiver-owned CkDevicePersistent endpoint with what
// an exchange needs on top of the raw transfer: a notification the receiving
// element acts on (a CkDataMsg carrying a CkDeviceChannelEvent, sent to the
// `arrived` callback by the completion of each put); a credit that says when
// the buffer may be overwritten (release() on the receiver, one converse message
// to the sender's PE, matched to the sender's copy by a token in a per-PE
// table); and the migration protocol described at the class: a sender resolves
// the channel before its first put, the receiver's migration closes the channel
// at every resolved sender and waits for the acks, the receiver reopens at its
// new home and the senders re-resolve.
//
// Per-PE tables: senders (token -> holder copy) and owners (channel id -> owner
// object). A copy that is assigned over or destroyed moves or drops its entry.

int device_channel_credit_handler;
int device_channel_closing_handler;
int device_channel_closeack_handler;
int device_channel_resolved_handler;
int device_channel_done_handler;

struct DeviceChannelCredit {
  char core[CmiMsgHeaderSizeBytes];
  uint64_t token;
  uint64_t channel;
  uint64_t seq;
  uint64_t epoch;   // the resolution the credit belongs to
  int dst_pe;       // where it was addressed (a misdelivery is a transport fault, printed at once)
  int src_pe;
};
// Watchdog probe: a sender whose put waits for a credit asks its owner to
// print its own view of the channel.
struct DeviceChannelProbe {
  char core[CmiMsgHeaderSizeBytes];
  uint64_t channel;
  uint64_t token;
  uint64_t epoch;
  uint64_t seq;
  int pe;
  int owner_known;      // the owner element as the copy knows it
  uint64_t owner_id;
  CkGroupID owner_mgr;
};
// Each PE's last watchdog tick, by rank: a sibling that stops ticking is
// spinning or blocked, and a stuck PE never reports itself.
static double device_channel_last_tick[2048];
int device_channel_probe_handler;
// Credits that found no sender copy on this PE (the copy left): counted, the
// last one kept, printed by the watchdog.
static thread_local long device_channel_credits_dropped = 0;
static thread_local DeviceChannelCredit device_channel_last_dropped_credit;
// Credits a copy refused (not resolved, or another resolution's): counted, the last one kept.
static thread_local long device_channel_credits_refused = 0;
static thread_local long device_channel_resolved_older = 0;   // resolutions ignored: older than (or equal to) the one held
static char device_channel_last_resolved_older[200] = "";
static thread_local char device_channel_last_refused[200];
// Credits kept ahead of their resolution (see device_channel_early_credits): count and the last one.
static thread_local long device_channel_credits_early = 0;
static thread_local char device_channel_last_early[200];
// Resolved replies and closings that found no sender copy here: count and the last one.
static thread_local long device_channel_resolved_dropped = 0;
static thread_local char device_channel_last_resolved_dropped[200];
static thread_local long device_channel_closing_acked_blind = 0;
static thread_local char device_channel_last_closing_blind[200];
struct DeviceChannelClosing {
  char core[CmiMsgHeaderSizeBytes];
  uint64_t token;
  uint64_t channel;
  uint64_t generation;
  uint64_t epoch;   // the resolution being closed
  int owner_pe;
};
struct DeviceChannelCloseAck {
  char core[CmiMsgHeaderSizeBytes];
  uint64_t channel;
  uint64_t token;
  int pe;
  uint64_t last_seq;    // the sender's last issued put (UINT64_MAX: unknown, the copy had left)
  uint64_t last_bytes;  // its size
};
struct DeviceChannelDone {   // the receiver has read put `seq`: the sender's source is free
  char core[CmiMsgHeaderSizeBytes];
  uint64_t token;
  uint64_t channel;
  uint64_t seq;
};
struct DeviceChannelResolved {
  char core[CmiMsgHeaderSizeBytes];
  uint64_t token;
  uint64_t channel;
  uint64_t generation;
  uint64_t epoch;   // this resolution's number; its credits and closing carry it
  int owner_pe;
  int free;
  uint64_t taken_seq;   // the last put of this sender's in the buffer at the reply (free says whether it is still held)
  int desc_bytes;   // the pup of the receiver's endpoint follows the struct
  int dst_pe;       // addressed to; a misdelivery is printed at once
  int src_pe;
  int credit_pe;    // same-host sender: the owner's shared-memory area and slot for its credits
  int credit_slot;  // (-1: credits are messages)
  int signal_slot;  // cross-host sender: the slot its puts signal (-1: notes are messages)
  int hdr_bytes;    // the pup of the owner process's header region follows the endpoint's
};

static bool deviceChannelDebug() {
  static const bool on = (getenv("CHARM_DEBUG_MIGRATE") != NULL);
  return on;
}
// CHARM_MIG_TRACE=1: one wall-clock line per hop of a channel's closing->resolution (the hold), by channel and
// generation, for the same-node pairs an offline pass can compare (nodes' clocks differ by seconds).
static inline bool chanMigTrace() { static const bool on = (getenv("CHARM_MIG_TRACE") != nullptr); return on; }
#define MIGC(stage, ch, gen, pe) do { if (chanMigTrace()) { struct timespec ts_; clock_gettime(CLOCK_REALTIME, &ts_); \
  CmiPrintf("[MIGC %d] %s ch=%llx gen=%llu pe=%d t=%.6f\n", CkMyPe(), stage, (unsigned long long)(ch), (unsigned long long)(gen), (int)(pe), ts_.tv_sec + ts_.tv_nsec * 1e-9); } } while (0)

typedef std::unordered_map<uint64_t, CkDeviceChannel*> DeviceChannelTable;
CkpvStaticDeclare(DeviceChannelTable*, device_channel_senders);
CkpvStaticDeclare(DeviceChannelTable*, device_channel_owners);
CkpvStaticDeclare(uint64_t, device_channel_counter);
// Credits that arrived ahead of the resolution they belong to, by sender
// token: the owner replies to a resolve and releases the buffer afterwards,
// and the two messages can reach the sender in either order (two nodes,
// adaptive routing). A credit for a later epoch than the copy's current one
// can only be the owner's answer to this copy's outstanding resolve, so it is
// kept and applied when that resolution arrives. Leanmd 16x8x8 hung at the
// step after the Metis wave with exactly this: a moved Compute's forceOut
// resolved while its owner still held the previous put; the owner replied
// (free=0) and then released, and the credit overtook the reply.
struct DeviceChannelEarlyCredit { uint64_t epoch, seq; };
typedef std::unordered_map<uint64_t, std::vector<DeviceChannelEarlyCredit>> DeviceChannelEarlyCredits;
CkpvStaticDeclare(DeviceChannelEarlyCredits*, device_channel_early_credits);
// Staging buffers of puts made while the receiver's channel was closed
// (issueStaged), by (sender token, seq): freed by the receiver's DONE, which
// finds them here even if the sending element's channel copy is gone.
struct DeviceChannelStaged { void* buf; CkGroupID holder_mgr; CmiUInt8 holder_elem; bool holder_known; };
typedef std::map<std::pair<uint64_t, uint64_t>, DeviceChannelStaged> DeviceChannelStagedTable;
CkpvStaticDeclare(DeviceChannelStagedTable*, device_channel_staged);
// Channel puts batched per PE (2026-10-20): the puts a PE issues on one
// producing stream within one entry method become one batched copy with one
// completion flag (deviceChannelBatchFlush), as the receive path batches its
// landings (DeviceRecvBatch). Flushed when the stream changes, when full, at
// every scheduler iteration and at idle: a batch never outlives the entry
// method that opened it.
struct DeviceChannelBatchEntry { uint64_t token; uint64_t seq; CkCallback user; CkCallback note; void* note_msg; };
struct DeviceChannelBatch {
  hapiStream_t stream;
  bool in_entry;   // opened inside an element's entry method: CkCallstackPop flushes it; otherwise the scheduler loop does
  std::vector<void*> dsts;
  std::vector<const void*> srcs;
  std::vector<size_t> sizes;
  std::vector<DeviceChannelBatchEntry> entries;
};
CkpvStaticDeclare(DeviceChannelBatch*, device_channel_batch);
static const size_t kChanBatchMax = 256;
static thread_local bool device_channel_batch_ready = false;   // this PE's tables exist (CkRdmaDeviceChannelInit ran)
static void deviceChannelBatchFlush();
static void deviceChannelBatchPoll(void*);
// Channel events handled on this PE (puts, releases, resolves, credits,
// completions): the watchdog below reads it every 10 s.
CkpvStaticDeclare(uint64_t, device_channel_progress);
CkpvStaticDeclare(uint64_t, device_channel_progress_seen);
CkpvStaticDeclare(int, device_channel_quiet_ticks);
static inline void deviceChannelProgress() { ++CkpvAccess(device_channel_progress); }

// Process-wide: the channel objects (owned channels and sender copies) each
// element holds, so an element handed to another PE of this process without a
// pup can have its channels' per-PE registrations moved along. Elements are
// per PE but a handoff crosses PEs, hence the lock.
// An element is (location manager, id): ids repeat across arrays (a Cell and a
// Compute of leanmd share id values), so the manager is part of every key.
typedef std::pair<int, CmiUInt8> DeviceChannelElemKey;
typedef std::map<DeviceChannelElemKey, std::set<CkDeviceChannel*>> DeviceChannelElems;
static DeviceChannelElems g_device_channel_elems;
static std::mutex g_device_channel_elems_lock;
static DeviceChannelElemKey deviceChannelElemKeyOf(const CkLocRec* rec) {
  return DeviceChannelElemKey(rec->getLocMgr()->ckGetGroupID().idx, rec->getID());
}
static void deviceChannelElemAdd(CkGroupID mgr, CmiUInt8 elem, CkDeviceChannel* c) {
  std::lock_guard<std::mutex> g(g_device_channel_elems_lock);
  g_device_channel_elems[DeviceChannelElemKey(mgr.idx, elem)].insert(c);
}
static void deviceChannelElemDrop(CkGroupID mgr, CmiUInt8 elem, CkDeviceChannel* c) {
  std::lock_guard<std::mutex> g(g_device_channel_elems_lock);
  auto it = g_device_channel_elems.find(DeviceChannelElemKey(mgr.idx, elem));
  if (it == g_device_channel_elems.end()) return;
  it->second.erase(c);
  if (it->second.empty()) g_device_channel_elems.erase(it);
}
static std::vector<CkDeviceChannel*> deviceChannelElemList(const CkLocRec* rec) {
  std::lock_guard<std::mutex> g(g_device_channel_elems_lock);
  auto it = g_device_channel_elems.find(deviceChannelElemKeyOf(rec));
  if (it == g_device_channel_elems.end()) return {};
  return std::vector<CkDeviceChannel*>(it->second.begin(), it->second.end());
}

// Under CHARM_DEBUG_MIGRATE, every 10 s each PE prints the channel copies that
// are waiting on something: a sender with a put queued (for a credit or a
// resolve), a receiver holding a put or holding resolves for after a move.
static void deviceChannelStallScan(void*) {
  for (auto& kv : *CkpvAccess(device_channel_senders)) kv.second->describeStall("sender");
  for (auto& kv : *CkpvAccess(device_channel_owners)) kv.second->describeStall("owner");
}

// Always armed, silent while channels move: after two 10 s ticks without a
// channel event on this PE, the copies that are waiting on something are
// described -- a summary and a few examples -- and again once a minute. A
// healthy run never prints; a hang prints its own stall scan without the
// per-event prints of CHARM_DEBUG_MIGRATE, which change the timing.
// ---------------------------------------------------------------------------
// Same-host, other-process signalling through shared memory.
//
// A credit or an arrival note between two processes on one host was a
// transport message: an LCI post through the NIC's loopback, 2-30 us of the
// sending PE's time with retries under load, where the zero-copy path's
// same-host sends signal through flags in shared memory and post nothing.
// Here every PE of the host owns an area in one segment: credit slots the PE
// grants to same-host senders (a word the owner increments at release(), the
// sender reads), and a ring of arrival notes other PEs' completions write and
// this PE's scheduler-loop poll turns into the local event. Cross-host pairs
// and pairs inside one process keep their messages (the latter are a local
// queue push already).
//
// The segment's pages are zero when created, and zero is the empty state of
// everything in it, so no rendezvous is needed: each process opens-or-creates
// the file, sizes it (idempotent) and maps it. The name carries the host and
// the job, and is unlinked at exit.
// ---------------------------------------------------------------------------
namespace {
constexpr int CHAN_SHM_CREDITS = 8192;    // credit slots per PE area
constexpr int CHAN_SHM_NOTES = 8192;      // note ring entries per PE area
constexpr int CHAN_SHM_NOTE_MARGIN = 512; // tickets producers may hold between the check and the write

struct ChanShmCredit {
  std::atomic<uint64_t> channel;   // 0: free. Owner sets channel+epoch, sender checks both around its read.
  std::atomic<uint64_t> epoch;
  std::atomic<uint64_t> count;     // credits granted under that resolution
};
struct ChanShmNote {
  std::atomic<uint64_t> ready;     // ticket + 1 once the entry is complete
  CkDeviceChannelEvent ev;
  alignas(8) char cb[sizeof(CkCallback)];   // the owner's arrival callback, as the sender's copy holds it
};
struct ChanShmArea {
  ChanShmCredit credit[CHAN_SHM_CREDITS];
  std::atomic<uint64_t> note_head;  // producers' ticket counter
  std::atomic<uint64_t> note_tail;  // consumer's progress, published for the producers' overflow check
  char pad[64];
  ChanShmNote note[CHAN_SHM_NOTES];
};
// 64-bit atomics are lock-free on this target (x86-64); the words live in shared memory.

ChanShmArea* chan_shm_base = nullptr;   // per process
int chan_shm_areas = 0;                 // PEs on this host
std::string chan_shm_name;
std::once_flag chan_shm_once;

void chanShmUnlink() { if (!chan_shm_name.empty()) shm_unlink(chan_shm_name.c_str()); }

void chanShmMap() {
  const int host = CmiPhysicalNodeID(CkMyPe());
  chan_shm_areas = CmiNumPesOnPhysicalNode(host);
  const char* job = getenv("SLURM_JOB_ID");
  // Per job STEP, not per job: a run killed mid-way leaves the segment behind
  // with live ring and credit state, and the next step of the same job then
  // opened it (zero is the empty state; the leftover is not) and hung as soon
  // as its first credit went through shared memory (2026-10-20 20:31-20:54,
  // job 23067051: four "hangs" after a step was killed by hand).
  const char* step = getenv("SLURM_STEP_ID");
  chan_shm_name = "charm-chan-" + std::to_string(host) + "-" + (job ? std::string(job) : std::to_string((long)getppid())) +
                  (step ? "-" + std::string(step) : std::string());
  const size_t bytes = sizeof(ChanShmArea) * (size_t)chan_shm_areas;
  int fd = shm_open(chan_shm_name.c_str(), O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
  if (fd < 0) CkAbort("CkDeviceChannel: shm_open(%s) failed: %s\n", chan_shm_name.c_str(), strerror(errno));
  if (ftruncate(fd, (off_t)bytes) != 0) CkAbort("CkDeviceChannel: ftruncate(%s, %zu) failed: %s\n", chan_shm_name.c_str(), bytes, strerror(errno));
  void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  if (p == MAP_FAILED) CkAbort("CkDeviceChannel: mmap(%s, %zu) failed: %s\n", chan_shm_name.c_str(), bytes, strerror(errno));
  chan_shm_base = (ChanShmArea*)p;
  atexit(chanShmUnlink);
}

inline ChanShmArea& chanShmArea(int node_local_pe) { return chan_shm_base[node_local_pe]; }
inline int chanShmIndexOf(int pe) { return CmiPhysicalRank(pe); }
// A pair that signals through shared memory: same host, different process.
inline bool chanShmPair(int pe) {
  return chan_shm_base != nullptr && CmiPhysicalNodeID(pe) == CmiPhysicalNodeID(CkMyPe()) &&
         CmiNodeOf(pe) != CmiMyNode();
}

// Owner: a free slot in this PE's area, tagged for (channel, epoch), count 0.
int chanShmAllocCredit(uint64_t channel, uint64_t epoch) {
  static thread_local int next = 0;
  ChanShmArea& a = chanShmArea(chanShmIndexOf(CkMyPe()));
  for (int tries = 0; tries < CHAN_SHM_CREDITS; tries++) {
    const int i = next; next = (next + 1) % CHAN_SHM_CREDITS;
    uint64_t zero = 0;
    if (a.credit[i].channel.compare_exchange_strong(zero, channel, std::memory_order_acq_rel)) {
      a.credit[i].count.store(0, std::memory_order_relaxed);
      a.credit[i].epoch.store(epoch, std::memory_order_release);
      return i;
    }
  }
  return -1;   // none free: this resolution's credits go by message
}

// Sender: the credits granted so far under (channel, epoch), or -1 when the
// slot no longer carries that resolution (freed, or reused by another).
long chanShmReadCredit(int area_pe, int slot, uint64_t channel, uint64_t epoch) {
  ChanShmCredit& c = chanShmArea(area_pe).credit[slot];
  const uint64_t ch1 = c.channel.load(std::memory_order_acquire);
  const uint64_t ep1 = c.epoch.load(std::memory_order_acquire);
  const uint64_t n = c.count.load(std::memory_order_acquire);
  const uint64_t ep2 = c.epoch.load(std::memory_order_acquire);
  const uint64_t ch2 = c.channel.load(std::memory_order_acquire);
  if (ch1 != channel || ch2 != channel || ep1 != epoch || ep2 != epoch) return -1;
  return (long)n;
}

// Senders of this PE with a put queued behind a shared-memory credit.
CkpvStaticDeclare(std::vector<CkDeviceChannel*>*, device_channel_shm_waiters);

// The note a put's completion writes into the owner PE's ring, or sends as a
// message when the ring is full.
struct DeviceChannelShmNote {
  int area_pe;
  CkDeviceChannelEvent ev;
  CkCallback arrived;
};
void deviceChannelShmNoteFn(void* param, void*) {
  auto* d = (DeviceChannelShmNote*)param;
  ChanShmArea& a = chanShmArea(d->area_pe);
  const uint64_t head = a.note_head.load(std::memory_order_relaxed);
  const uint64_t tail = a.note_tail.load(std::memory_order_acquire);
  if (head - tail < (uint64_t)(CHAN_SHM_NOTES - CHAN_SHM_NOTE_MARGIN)) {
    const uint64_t ticket = a.note_head.fetch_add(1, std::memory_order_acq_rel);
    { static thread_local bool said = false; if (!said) { said = true; CmiPrintf("[CHAN-INIT %d] first note through shared memory (area %d)\n", CkMyPe(), d->area_pe); } }
    ChanShmNote& e = a.note[ticket % CHAN_SHM_NOTES];
    e.ev = d->ev;
    memcpy(e.cb, &d->arrived, sizeof(CkCallback));
    e.ready.store(ticket + 1, std::memory_order_release);
  } else {
    CkDataMsg* note = CkDataMsg::buildNew((int)sizeof(d->ev), &d->ev);
    CkSetRefNum(note, (CMK_REFNUM_TYPE)d->ev.tag);
    d->arrived.send(note);
  }
  delete d;
}

// This PE's scheduler-loop poll: notes other PEs wrote for it, and credits
// its queued puts wait for.
struct ZcPollTimer { double t0; std::atomic<long>* n; std::atomic<long>* us; std::atomic<long>* mx;
  ~ZcPollTimer() { if (t0 == 0.0) return; const long d = (long)((CkWallTimer() - t0) * 1e6); n->fetch_add(1, std::memory_order_relaxed); us->fetch_add(d, std::memory_order_relaxed);
    long m = mx->load(std::memory_order_relaxed); while (d > m && !mx->compare_exchange_weak(m, d, std::memory_order_relaxed)) {} } };
void deviceChannelShmPoll(void*) {
  ZcPollTimer zc_poll_timer{zcStatsOnFast() ? CkWallTimer() : 0.0, &zc_mode_stats.shmpoll_n, &zc_mode_stats.shmpoll_us, &zc_mode_stats.shmpoll_max};
  if (chan_shm_base == nullptr) return;
  ChanShmArea& a = chanShmArea(chanShmIndexOf(CkMyPe()));
  uint64_t tail = a.note_tail.load(std::memory_order_relaxed);
  if (tail != a.note_head.load(std::memory_order_acquire)) {
    int taken = 0;
    while (taken < 64) {
      ChanShmNote& e = a.note[tail % CHAN_SHM_NOTES];
      if (e.ready.load(std::memory_order_acquire) != tail + 1) break;   // not written yet
      CkDeviceChannelEvent ev = e.ev;
      CkCallback cb;
      memcpy(&cb, e.cb, sizeof(CkCallback));
      tail++;
      a.note_tail.store(tail, std::memory_order_release);
      CkDataMsg* note = CkDataMsg::buildNew((int)sizeof(ev), &ev);
      CkSetRefNum(note, (CMK_REFNUM_TYPE)ev.tag);
      cb.send(note);
      taken++;
    }
  }
  std::vector<CkDeviceChannel*>& w = *CkpvAccess(device_channel_shm_waiters);
  if (w.empty()) return;
  // pollShmCredit may issue the queued put, and issue() unlists the copy
  // (swap with the back, pop): walking the list itself while that happens
  // read past its end once it was empty -- a garbage pointer polled next
  // (segfault in pollShmCredit, 2026-10-22 14:12, c4_ach_5 step 150). Walk a
  // copy, then drop what no longer waits.
  std::vector<CkDeviceChannel*> walk(w);
  for (CkDeviceChannel* c : walk) c->pollShmCredit();
  for (size_t i = 0; i < w.size();) {
    if (!w[i]->putQueued()) { w[i] = w.back(); w.pop_back(); } else i++;
  }
}
void deviceChannelShmUnlist(CkDeviceChannel* c) {
  std::vector<CkDeviceChannel*>& w = *CkpvAccess(device_channel_shm_waiters);
  for (size_t i = 0; i < w.size(); i++) if (w[i] == c) { w[i] = w.back(); w.pop_back(); return; }
}
}  // namespace


// While the watchdog scans, describeStall tallies into this instead of
// printing every stuck copy (a PE can hold thousands).
struct DeviceChannelWatchTally {
  int s_inflight = 0, s_pending = 0, s_resolving = 0, s_closeack = 0;
  int o_waiting = 0, o_closing = 0, o_pulls = 0, shown = 0;
};
static thread_local DeviceChannelWatchTally* device_channel_watch_tally = nullptr;
static CkDeviceChannel* deviceChannelOwner(uint64_t channel);
struct DeviceChannelProbeCtx { int pe; uint64_t token, epoch, seq; };
static thread_local DeviceChannelProbeCtx* device_channel_probe_ctx = nullptr;

extern "C" void* device_channel_probe_bridge(void* arg) {
  auto* m = (DeviceChannelProbe*)arg;
  DeviceChannelProbeCtx ctx{m->pe, m->token, m->epoch, m->seq};
  device_channel_probe_ctx = &ctx;
  CkDeviceChannel* c = deviceChannelOwner(m->channel);
  if (c) c->describeStall("owner");
  int copies = 0;
  for (auto& kv : *CkpvAccess(device_channel_senders))
    if (kv.second->id() == m->channel) { copies++; kv.second->describeStall("sender-copy"); }
  if (!c) {
    // Not on this PE: any other PE of this process (a handoff moves the owner
    // within the process without a new resolution until it re-registers).
    for (int r = 0; r < CkMyNodeSize() && !c; r++) {
      if (r == CkMyRank()) continue;
      DeviceChannelTable* tab = CkpvAccessOther(device_channel_owners, r);
      if (!tab) continue;
      auto it = tab->find(m->channel);
      if (it != tab->end()) {
        c = it->second;
        CmiPrintf("[CHAN-PROBE %d] channel %llx asked by pe %d: the owner is registered on PE %d of this process, not here\n",
                  CkMyPe(), (unsigned long long)m->channel, m->pe, CkNodeFirst(CkMyNode()) + r);
        c->describeStall("owner-elsewhere");
      }
    }
  }
  device_channel_probe_ctx = nullptr;
  if (!c) {
    CmiPrintf("[CHAN-PROBE %d] channel %llx asked by pe %d token %llx: NO owner registered in this process (%d sender copies of it here)\n",
              CkMyPe(), (unsigned long long)m->channel, m->pe, (unsigned long long)m->token, copies);
    if (m->owner_known) {
      CkLocMgr* mgr = (CkLocMgr*)CkLocalBranch(m->owner_mgr);
      CkLocRec* rec = mgr ? mgr->recForDeviceWork(m->owner_id) : NULL;
      if (rec) {
        const CkArrayIndex& idx = rec->getIndex();
        CmiPrintf("[CHAN-PROBE %d] the owner element %llu IS here (index %d %d %d %d %d %d) but its channel %llx is not registered: arrived without reopen, or packed and not yet gone\n",
                  CkMyPe(), (unsigned long long)m->owner_id, idx.data()[0], idx.data()[1], idx.data()[2],
                  idx.data()[3], idx.data()[4], idx.data()[5], (unsigned long long)m->channel);
      } else {
        CmiPrintf("[CHAN-PROBE %d] the owner element %llu is not on this PE either (manager %d)\n",
                  CkMyPe(), (unsigned long long)m->owner_id, m->owner_mgr.idx);
      }
    }
  }
  CmiFree(m);
  return NULL;
}
extern "C" void hapiDumpEventQueues(int max_streams);   // hapi_impl.cpp: what this PE's poll waits on

static void deviceChannelWatch(void*) {
  const double wall = CkWallTimer();
  if (CkMyRank() < 2048) device_channel_last_tick[CkMyRank()] = wall;
  const uint64_t now = CkpvAccess(device_channel_progress);
  if (now != CkpvAccess(device_channel_progress_seen)) {
    CkpvAccess(device_channel_progress_seen) = now;
    CkpvAccess(device_channel_quiet_ticks) = 0;
    return;
  }
  const int q = ++CkpvAccess(device_channel_quiet_ticks);
  if (q < 2 || (q - 2) % 6 != 0) return;
  for (int r = 0; r < CkMyNodeSize() && r < 2048; r++) {
    const double t = device_channel_last_tick[r];
    if (r != CkMyRank() && t > 0.0 && wall - t > 25.0)
      CmiPrintf("[CHAN-WATCH %d] PE %d (rank %d of this process) has not run its watchdog for %.0f s: spinning or blocked\n",
                CkMyPe(), CkNodeFirst(CkMyNode()) + r, r, wall - t);
  }
  DeviceChannelWatchTally t;
  device_channel_watch_tally = &t;
  for (auto& kv : *CkpvAccess(device_channel_senders)) kv.second->describeStall("sender");
  for (auto& kv : *CkpvAccess(device_channel_owners)) kv.second->describeStall("owner");
  device_channel_watch_tally = nullptr;
  // The counters, from every quiet PE (a credit sent to the wrong place is
  // dropped where it lands, not where the sender waits).
  if (device_channel_credits_refused > 0)
    CmiPrintf("[CHAN-WATCH %d] credits refused by a copy here: %ld (last: %s)\n", CkMyPe(),
              device_channel_credits_refused, device_channel_last_refused);
  if (device_channel_resolved_older > 0)
    CmiPrintf("[CHAN-WATCH %d] resolutions ignored here as older than the one held: %ld (last: %s)\n", CkMyPe(),
              device_channel_resolved_older, device_channel_last_resolved_older);
  if (CkMyRank() == 0) {
    const long dr = chan_signal_dropped_retired.load(), dg = chan_signal_dropped_range.load();
    const long sj = chan_signal_seq_jumps.load(), ss = chan_signal_seq_stale.load();
    if (dr > 0 || dg > 0)
      CmiPrintf("[CHAN-WATCH %d] immediate notes dropped in this process: %ld at a retired slot, %ld out of range (last: %s)\n",
                CkMyPe(), dr, dg, chan_signal_last_drop);
    if (sj > 0 || ss > 0)
      CmiPrintf("[CHAN-WATCH %d] immediate headers in this process: %ld skipped a put, %ld went backwards (last: %s)\n",
                CkMyPe(), sj, ss, chan_signal_last_jump);
    const long rn = chan_signal_release_now.load(), rd = chan_signal_release_deferred.load(), rl = chan_signal_release_late.load();
    if (rn > 0 || rd > 0 || rl > 0)
      CmiPrintf("[CHAN-WATCH %d] signal slots released in this process: %ld at the ack, %ld deferred to the acked put, %ld of those freed by it\n",
                CkMyPe(), rn, rd, rl);
  }
  if (device_channel_credits_early > 0)
    CmiPrintf("[CHAN-WATCH %d] credits kept ahead of their resolution here: %ld, %zu still held (last: %s)\n", CkMyPe(),
              device_channel_credits_early, CkpvAccess(device_channel_early_credits)->size(), device_channel_last_early);
  if (device_channel_resolved_dropped > 0)
    CmiPrintf("[CHAN-WATCH %d] resolutions that found no sender copy here: %ld (last: %s)\n", CkMyPe(),
              device_channel_resolved_dropped, device_channel_last_resolved_dropped);
  if (device_channel_closing_acked_blind > 0)
    CmiPrintf("[CHAN-WATCH %d] closings acked for a copy no longer here: %ld (last: %s)\n", CkMyPe(),
              device_channel_closing_acked_blind, device_channel_last_closing_blind);
  if (device_channel_credits_dropped > 0) {
    const DeviceChannelCredit& d = device_channel_last_dropped_credit;
    CmiPrintf("[CHAN-WATCH %d] credits that found no sender copy here: %ld (last: channel %llx token %llx seq %llu epoch %llu from pe %d)\n",
              CkMyPe(), device_channel_credits_dropped, (unsigned long long)d.channel, (unsigned long long)d.token,
              (unsigned long long)d.seq, (unsigned long long)d.epoch, d.src_pe);
  }
  if (t.s_inflight + t.s_pending + t.s_resolving + t.s_closeack + t.o_waiting + t.o_closing + t.o_pulls == 0) return;
  if (t.s_inflight > 0) hapiDumpEventQueues(8);
  CmiPrintf("[CHAN-WATCH %d] no channel event for %d s (%llu handled): senders in_flight %d, pending %d, "
            "resolving %d, close_ack_due %d; owners holding resolves %d, closing %d, announces queued %d "
            "(%zu senders, %zu owners here)\n",
            CkMyPe(), q * 10, (unsigned long long)now, t.s_inflight, t.s_pending, t.s_resolving, t.s_closeack,
            t.o_waiting, t.o_closing, t.o_pulls, CkpvAccess(device_channel_senders)->size(),
            CkpvAccess(device_channel_owners)->size());
}

void CkRdmaDeviceChannelInit() {
  CkpvInitialize(DeviceChannelTable*, device_channel_senders);
  CkpvAccess(device_channel_senders) = new DeviceChannelTable();
  CkpvInitialize(DeviceChannelTable*, device_channel_owners);
  CkpvAccess(device_channel_owners) = new DeviceChannelTable();
  CkpvInitialize(uint64_t, device_channel_counter);
  CkpvAccess(device_channel_counter) = 0;
  CkpvInitialize(DeviceChannelEarlyCredits*, device_channel_early_credits);
  CkpvAccess(device_channel_early_credits) = new DeviceChannelEarlyCredits();
  CkpvInitialize(DeviceChannelStagedTable*, device_channel_staged);
  CkpvAccess(device_channel_staged) = new DeviceChannelStagedTable();
  CkpvInitialize(DeviceChannelBatch*, device_channel_batch);
  CkpvAccess(device_channel_batch) = nullptr;
  device_channel_batch_ready = true;
  CcdCallOnConditionKeep(CcdSCHEDLOOP, deviceChannelBatchPoll, NULL);
  CcdCallOnConditionKeep(CcdPROCESSOR_BEGIN_IDLE, deviceChannelBatchPoll, NULL);
  CcdCallOnConditionKeep(CcdPROCESSOR_STILL_IDLE, deviceChannelBatchPoll, NULL);
  CkpvInitialize(uint64_t, device_channel_progress);
  CkpvAccess(device_channel_progress) = 0;
  CkpvInitialize(uint64_t, device_channel_progress_seen);
  CkpvAccess(device_channel_progress_seen) = 0;
  CkpvInitialize(int, device_channel_quiet_ticks);
  CkpvAccess(device_channel_quiet_ticks) = 0;
  if (deviceChannelDebug()) CcdCallOnConditionKeep(CcdPERIODIC_10seconds, deviceChannelStallScan, NULL);
  CcdCallOnConditionKeep(CcdPERIODIC_10seconds, deviceChannelWatch, NULL);
  CkpvInitialize(std::vector<CkDeviceChannel*>*, device_channel_shm_waiters);
  CkpvAccess(device_channel_shm_waiters) = new std::vector<CkDeviceChannel*>();
  CkpvInitialize(CmiNcpyBuffer*, device_channel_hdr_src);
  CkpvAccess(device_channel_hdr_src) = nullptr;
  CkpvInitialize(unsigned, device_channel_hdr_next);
  CkpvAccess(device_channel_hdr_next) = 0;
  CkpvInitialize(bool, device_channel_late_init_done);
  CkpvAccess(device_channel_late_init_done) = false;
  // The host topology (which PEs share a host) is computed after this runs
  // (init.C: CmiInitCPUTopology comes later), so the shared-memory segment,
  // its poll and the signal table wait for the first use: deviceChannelLateInit.
}

// Per PE, at the first channel use, once the host topology is known: the
// shared-memory segment and its poll when several processes share this host,
// the signal table and this PE's header ring when there are several hosts.
void deviceChannelLateInit() {
  if (CkpvAccess(device_channel_late_init_done)) return;
  CkpvAccess(device_channel_late_init_done) = true;
  const int host = CmiPhysicalNodeID(CkMyPe());
  const bool several_procs = CmiNumPesOnPhysicalNode(host) > CkMyNodeSize();
  const bool several_hosts = CmiNumPhysicalNodes() > 1;
  if (several_procs) {
    std::call_once(chan_shm_once, chanShmMap);
    CcdCallOnConditionKeep(CcdSCHEDLOOP, deviceChannelShmPoll, NULL);
  }
  if (several_hosts) {
    std::call_once(chan_signal_once, chanSignalInit);
    void* sc = nullptr;
    const size_t ring_bytes = sizeof(ChanSignalHdr) * (size_t)CHAN_HDR_RING;
    if (posix_memalign(&sc, 4096, ring_bytes) != 0) CkAbort("CkDeviceChannel: header ring allocation failed\n");
    memset(sc, 0, ring_bytes);
    CkpvAccess(device_channel_hdr_src) = new CmiNcpyBuffer(sc, ring_bytes, CMK_BUFFER_REG, CMK_BUFFER_NODEREG);   // registered once per PE, never deregistered
  }
  if (CkMyRank() == 0)
    CmiPrintf("[CHAN-INIT %d] hosts=%d pes_on_host=%d pes_in_process=%d: shared memory %s, signal table %s\n",
              CkMyPe(), CmiNumPhysicalNodes(), CmiNumPesOnPhysicalNode(host), CkMyNodeSize(),
              several_procs ? "on" : "off", several_hosts ? "on" : "off");
}

// Unique on this PE and distinct from every other PE's: channel ids and sender
// tokens both come from it.
static uint64_t deviceChannelToken() {
  return ((uint64_t)CkMyPe() << 40) | (++CkpvAccess(device_channel_counter));
}

static CkDeviceChannel* deviceChannelSender(uint64_t token) {
  DeviceChannelTable& tab = *CkpvAccess(device_channel_senders);
  auto it = tab.find(token);
  return it == tab.end() ? NULL : it->second;
}

static CkDeviceChannel* deviceChannelOwner(uint64_t channel) {
  DeviceChannelTable& tab = *CkpvAccess(device_channel_owners);
  auto it = tab.find(channel);
  return it == tab.end() ? NULL : it->second;
}

// A protocol message: allocated, addressed to its handler, counted for
// quiescence (the handler releases the count).
template <class M>
static M* deviceChannelMsg(int handler, size_t extra = 0) {
  M* m = (M*)CmiAlloc(sizeof(M) + extra);
  CmiEnforce(m);
  CmiSetHandler(m, handler);
  QdCreate(1);
  return m;
}

extern "C" void* device_channel_credit_bridge(void* arg) {
  QdProcess(1);
  auto* m = (DeviceChannelCredit*)arg;
  if (m->dst_pe != CkMyPe())
    CmiPrintf("[CHAN-MISDELIVERED %d] credit for channel %llx token %llx epoch %llu from pe %d was addressed to pe %d\n",
              CkMyPe(), (unsigned long long)m->channel, (unsigned long long)m->token, (unsigned long long)m->epoch,
              m->src_pe, m->dst_pe);
  CkDeviceChannel* c = deviceChannelSender(m->token);
  // A copy that left this PE re-resolves before its next put and learns the
  // buffer's state then, so a credit that finds nobody is simply dropped.
  if (c) c->onCredit(m->seq, m->epoch);
  else { device_channel_credits_dropped++; device_channel_last_dropped_credit = *m; }
  CmiFree(m);
  return NULL;
}

extern "C" void* device_channel_closing_bridge(void* arg) {
  QdProcess(1);
  auto* m = (DeviceChannelClosing*)arg;
  CkDeviceChannel* c = deviceChannelSender(m->token);
  if (c) {
    c->onClosing(m->generation, m->owner_pe, m->epoch);
  } else {
    // The holder left this PE (its chare migrated or died), so nothing of it is
    // in flight from here: acknowledge for it, or the receiver never moves.
    auto* a = deviceChannelMsg<DeviceChannelCloseAck>(device_channel_closeack_handler);
    a->channel = m->channel;
    a->token = m->token;
    a->pe = CkMyPe();
    a->last_seq = UINT64_MAX;   // nobody here knows what that copy last put
    a->last_bytes = 0;
    CmiSyncSendAndFreeUrgent(m->owner_pe, sizeof(DeviceChannelCloseAck), (char*)a);
    device_channel_closing_acked_blind++;
    snprintf(device_channel_last_closing_blind, sizeof(device_channel_last_closing_blind),
             "channel %llx token %llx epoch %llu gen %llu from pe %d",
             (unsigned long long)m->channel, (unsigned long long)m->token, (unsigned long long)m->epoch,
             (unsigned long long)m->generation, m->owner_pe);
  }
  CmiFree(m);
  return NULL;
}

extern "C" void* device_channel_closeack_bridge(void* arg) {
  QdProcess(1);
  auto* m = (DeviceChannelCloseAck*)arg;
  CkDeviceChannel* c = deviceChannelOwner(m->channel);
  if (c) c->onCloseAck(m->pe, m->token, m->last_seq, m->last_bytes);
  CmiFree(m);
  return NULL;
}

extern "C" void* device_channel_done_bridge(void* arg) {
  QdProcess(1);
  auto* m = (DeviceChannelDone*)arg;
  DeviceChannelStagedTable& st = *CkpvAccess(device_channel_staged);
  auto it = st.find(std::make_pair(m->token, m->seq));
  if (it != st.end()) {
    // A staged put was read: its buffer goes back, the sending element is no
    // longer charged with it, and its channel copy (if still here) rebalances.
    const DeviceChannelStaged d = it->second;
    st.erase(it);
    if (d.buf) CkDeviceFree(d.buf);
    if (d.holder_known) {
      CkLocMgr* mgr = (CkLocMgr*)CkLocalBranch(d.holder_mgr);
      CkLocRec* rec = mgr ? mgr->recForDeviceWork(d.holder_elem) : NULL;
      if (rec) rec->noteDeviceSendDone();
    }
    CkDeviceChannel* c = deviceChannelSender(m->token);
    if (c) c->onStagedDone(m->seq);
  } else {
    CkDeviceChannel* c = deviceChannelSender(m->token);
    if (c) c->onDone(m->seq);
  }
  CmiFree(m);
  return NULL;
}

// The announce of a put, sent once the source is complete on the sender's
// stream (as the one-sided write was issued).
struct DeviceChannelAnnounce { CkCallback to; CkDataMsg* msg; };
static void deviceChannelAnnounceReadyFn(void* param, void*) {
  auto* a = (DeviceChannelAnnounce*)param;
  a->to.send(a->msg);
  delete a;
}
// The receiver's read of an announced put completed (runs on the receiver PE).
struct DeviceChannelPulled { uint64_t channel; };
static void deviceChannelPulledFn(void* param, void*) {
  auto* d = (DeviceChannelPulled*)param;
  CkDeviceChannel* c = deviceChannelOwner(d->channel);
  if (c) c->onPulled();
  else CmiPrintf("[CHAN %d] a read completed for channel %llx, which is no longer open here\n", CkMyPe(), (unsigned long long)d->channel);
  delete d;
}

extern "C" void* device_channel_resolved_bridge(void* arg) {
  QdProcess(1);
  auto* m = (DeviceChannelResolved*)arg;
  if (m->dst_pe != CkMyPe())
    CmiPrintf("[CHAN-MISDELIVERED %d] resolution %llu of channel %llx token %llx from pe %d was addressed to pe %d\n",
              CkMyPe(), (unsigned long long)m->epoch, (unsigned long long)m->channel, (unsigned long long)m->token,
              m->src_pe, m->dst_pe);
  CkDeviceChannel* c = deviceChannelSender(m->token);
  if (c) {
    CkDevicePersistent ep;
    PUP::fromMem p((char*)m + sizeof(DeviceChannelResolved));
    p | ep;
    CmiNcpyBuffer hdr;
    const bool with_hdr = (m->hdr_bytes > 0 && m->signal_slot > 0);
    if (m->hdr_bytes > 0) p | hdr;
    c->onResolved(ep, m->generation, m->owner_pe, m->free != 0, m->epoch, m->credit_pe, m->credit_slot,
                  m->signal_slot, with_hdr ? &hdr : NULL, m->taken_seq);
  } else {
    device_channel_resolved_dropped++;
    snprintf(device_channel_last_resolved_dropped, sizeof(device_channel_last_resolved_dropped),
             "channel %llx token %llx epoch %llu gen %llu free %d from pe %d",
             (unsigned long long)m->channel, (unsigned long long)m->token, (unsigned long long)m->epoch,
             (unsigned long long)m->generation, (int)m->free, m->src_pe);
    { static thread_local int said = 0; if (said++ < 8 || deviceChannelDebug()) CmiPrintf("[CHAN-DROP %d] resolution with no sender copy here: %s\n", CkMyPe(), device_channel_last_resolved_dropped); }
  }
  CmiFree(m);
  return NULL;
}

// A put's bytes on the sending element's edge to the receiver, for the
// balancers' communication graph (what CkArray::recordSend does for a message).
// The edge runs from the sending element (the LB manager's current object, as
// for a message send) to the receiving element, the channel's owner.
static void deviceChannelRecordSend(CkLocRec* rec, size_t bytes, int dest_pe, bool owner_known,
                                    CkGroupID owner_mgr, CmiUInt8 dest_elem) {
#if CMK_LBDB_ON
  // The destination key is (the OWNER's object manager, its element id), as
  // CkArray::recordSend builds it on the destination array: with the sender's
  // manager, a put from a Cell to a Compute of leanmd named the Cell of the
  // same id, and MetisLB saw a scrambled graph (7.5 GB external against 2.8).
  if (rec == NULL || !owner_known) return;
  CkLocMgr* ownerMgr = (CkLocMgr*)CkLocalBranch(owner_mgr);
  if (ownerMgr == NULL || ownerMgr->getLBMgr() == NULL || !ownerMgr->getLBMgr()->CollectingCommStats()) return;
#  if CMK_GLOBAL_LOCATION_UPDATE
  const CmiUInt8 lbObjId = ck::ObjID(ownerMgr->getGroupID(), dest_elem).getID();
#  else
  const CmiUInt8 lbObjId = dest_elem;
#  endif
  ownerMgr->getLBMgr()->Send(ownerMgr->getOMHandle(), lbObjId, (unsigned int)bytes, dest_pe, 1);
#endif
}

// Sender-side completion of a put: the source may be reused. Runs on the
// sender's PE (the copy's completion callback, or the RDMA ack). The copy is
// found by token, never by pointer: it may have been assigned elsewhere since.
struct DeviceChannelSrcDone {
  uint64_t token;
  uint64_t seq;
  CkCallback user;
};

static void deviceChannelSrcDoneFn(void* param, void* msg) {
  auto* d = (DeviceChannelSrcDone*)param;
  CkDeviceChannel* c = deviceChannelSender(d->token);
  if (c) c->onSrcDone(d->seq);
  if (d->user.type != CkCallback::ignore) d->user.send(msg);
  delete d;
}
// Channel puts batched (2026-10-20). Measured at leanmd 16x8x8, no LB, on the
// GPU that sets the step (rank 3, 10 steps): a put was its own copy (3.10 us
// on the busy copy engine) with two completion flags behind it (1.4 us memsets:
// source-done and the receiver's note), 918 ms of copy-engine time per 10
// steps against 512 ms for the same messages on the zero-copy path, whose
// landings go in batches of ~9 at 2.26 us each with one flag per batch -- the
// 2-3% steady-state gap. One flag per put (the two callbacks merged) took half
// of it. Now the puts a PE issues on one producing stream within one entry
// method (a cell's thirteen position puts) are one batched copy on that stream
// with one flag; its completion delivers every put's note and source-done.
static void deviceChannelBatchDoneFn(void* param, void* msg) {
  auto* b = (DeviceChannelBatch*)param;
  for (const DeviceChannelBatchEntry& e : b->entries) {
    CkDeviceChannel* c = deviceChannelSender(e.token);
    if (c) c->onSrcDone(e.seq);
    if (e.user.type != CkCallback::ignore) e.user.send(msg);
    e.note.send(e.note_msg);
  }
  delete b;
}

static void deviceChannelBatchFlush() {
  DeviceChannelBatch*& cur = CkpvAccess(device_channel_batch);
  if (cur == nullptr) return;
  struct FlushTimer { double t0; ~FlushTimer() { if (t0 != 0.0) { zc_mode_stats.chan_flush_n.fetch_add(1, std::memory_order_relaxed);
    zc_mode_stats.chan_flush_us.fetch_add((long)((CkWallTimer() - t0) * 1e6), std::memory_order_relaxed); } } } flush_timer{zcStatsOnFast() ? CkWallTimer() : 0.0};
  DeviceChannelBatch* b = cur;
  cur = nullptr;
  const size_t n = b->entries.size();
  if (zcStatsOnFast()) {
    zc_mode_stats.chan_batches.fetch_add(1, std::memory_order_relaxed);
    zc_mode_stats.chan_batch_ops.fetch_add((long)n, std::memory_order_relaxed);
    long m = zc_mode_stats.chan_batch_max.load(std::memory_order_relaxed);
    while (m < (long)n && !zc_mode_stats.chan_batch_max.compare_exchange_weak(m, (long)n, std::memory_order_relaxed)) {}
  }
  hapiStream_t st = b->stream;
  // The arrays belong to the batch, which lives until its completion handler
  // frees it, so the closure may run later (submit mode) and still read them.
  hapiSubmit(st, [=]() {
    hapiError_t err = hapiErrorNotSupported;
#if defined(CUDART_VERSION) && CUDART_VERSION >= 12080
    if (n > 1) {
      cudaMemcpyAttributes attr{};
      attr.srcAccessOrder = cudaMemcpySrcAccessOrderStream;
      size_t attr_idx = 0;
#if CUDART_VERSION >= 13000
      err = cudaMemcpyBatchAsync(b->dsts.data(), b->srcs.data(), b->sizes.data(), n, &attr, &attr_idx, 1, st);
#else
      size_t fail_idx = 0;
      err = cudaMemcpyBatchAsync(b->dsts.data(), b->srcs.data(), b->sizes.data(), n, &attr, &attr_idx, 1, &fail_idx, st);
#endif
    }
#elif CMK_HIP
    // hipMemcpyBatchAsync (ROCm 7.14): the 12.8 signature, with the failure index; its per-copy
    // attributes are not supported, one attribute for the batch is. A single copy goes through it
    // too: hipMemcpyAsync into a peer's imported vmm heap (hipMemImportFromShareableHandle +
    // hipMemMap) crashes in ROCr's pointer lookup (Runtime::VMemoryPtrInfo, ROCm 7.14.1 on MI250X),
    // whatever the kind; the batch API takes the same pointers (Frontier job 5833510).
    {
      hipMemcpyAttributes attr{};
      attr.srcAccessOrder = hipMemcpySrcAccessOrderStream;
      size_t attr_idx = 0, fail_idx = 0;
      err = hipMemcpyBatchAsync(b->dsts.data(), const_cast<void**>(b->srcs.data()), b->sizes.data(), n, &attr, &attr_idx, 1, &fail_idx, st);
      if (err != hipSuccess)
        fprintf(stderr, "[chan-batch] pe %d: hipMemcpyBatchAsync(n=%zu) failed at %zu: %s\n", CkMyPe(), n, fail_idx, hipGetErrorString(err));
    }
#endif
    if (err != hapiSuccess) {
      if (n > 1) {
        hapiGetLastError();
        if (zcStatsOnFast()) zc_mode_stats.chan_batch_fallback.fetch_add(1, std::memory_order_relaxed);
      }
      // cudaMemcpyDefault: a destination may sit on another device (a peer's
      // buffer through its IPC import, or a receiver moved between GPUs).
      for (size_t i = 0; i < n; i++)
        hapiCheck(hapiMemcpyAsync(b->dsts[i], b->srcs[i], b->sizes[i], hapiMemcpyDefault, st));
    }
  });
  hapiAddCallback(st, CkCallback(deviceChannelBatchDoneFn, (void*)b));
}

static void deviceChannelBatchAdd(hapiStream_t stream, void* dst, const void* src, size_t bytes,
                                  const DeviceChannelBatchEntry& e) {
  DeviceChannelBatch*& cur = CkpvAccess(device_channel_batch);
  if (cur != nullptr && cur->stream != stream) deviceChannelBatchFlush();   // one producing stream per batch
  if (cur == nullptr) {
    cur = new DeviceChannelBatch;
    cur->stream = stream;
    cur->in_entry = (CkpvAccess(_currentLocRec) != NULL);
    cur->dsts.reserve(16); cur->srcs.reserve(16); cur->sizes.reserve(16); cur->entries.reserve(16);
  }
  cur->dsts.push_back(dst);
  cur->srcs.push_back(src);
  cur->sizes.push_back(bytes);
  cur->entries.push_back(e);
  if (cur->entries.size() >= kChanBatchMax) deviceChannelBatchFlush();
}

// Every scheduler iteration and at idle: the batch of the entry method that
// just ran goes out before the next one runs.
static void deviceChannelBatchPoll(void*) { deviceChannelBatchFlush(); }

// From CkCallstackPop, before the entry method's CUPTI correlation is popped:
// the batch's copies are launched while the element that made the puts is
// still the attributed object, so the balancer's per-object device load keeps
// them (flushed from the scheduler loop they belonged to nobody, and at 8x4x4
// the balancer then moved 20-30% more objects and lost 12%). The scheduler-loop
// and idle flushes stay as the net for puts made outside an entry method.
// Only a batch opened inside an entry method: one opened from a converse
// handler (a credit or a resolution issuing a parked put) waits for the
// scheduler loop. The pop of the deferred-send bracket (deviceDeferredPublish)
// runs INSIDE hapiPollEvents, under its queue lock; flushing such a batch there
// re-took the lock in hapiAddCallback and hung every PE before step 1
// (job 23067051, 20:31).
void CkRdmaDeviceChannelBatchFlush() {
  if (!device_channel_batch_ready) return;
  DeviceChannelBatch* cur = CkpvAccess(device_channel_batch);
  if (cur != nullptr && cur->in_entry) deviceChannelBatchFlush();
}

CkDeviceChannel::CkDeviceChannel()
  : arrived(CkCallback(CkCallback::ignore)), control(CkCallback(CkCallback::ignore)),
    channel_id(0), generation(0), cap(0),
    is_owner(false), owner_id(0), owner_known(false), busy(false), taken_seq(0), landed_unseen(0), landed_seq(0), last_put_bytes(0), last_direct_seq(0), credit_slot_pe(-1), credit_slot(-1), credit_shm_pe(-1), credit_shm_slot(-1), credits_seen(0), shm_waiting(false), owner_signal_slot(-1), departed_signal_slot(-1), departed_signal_pe(-1), signal_slot(-1), sender_pe(-1), sender_token(0),
    sender_known(false), closing_gen(0), closing_sent(false), sender_closed(false), resolved_during_closing(false), epoch(0), closing_rec(NULL), sender_pulls(false), pulling(false), have_last(false),
    resolved(false), resolving(false), send_seq(0), credits(0), uncredited(0), pull_mode(false), closed_epoch(0), in_flight(0), staged_outstanding(0), acct_seq(0), occupant_credit_due(false),
    close_ack_due(false), close_ack_pe(-1), my_token(0), closing_at(0.0), sender_rec(NULL), holder_elem(0), holder_known(false) {
  pending.valid = false;
  memset(&last, 0, sizeof(last));
}

CkDeviceChannel::CkDeviceChannel(void* dst_, size_t capacity, const CkCallback& arrived_)
  : dst(dst_, capacity), arrived(arrived_), control(CkCallback(CkCallback::ignore)),
    channel_id(0), generation(0), cap(capacity),
    is_owner(false), owner_id(0), owner_known(false), busy(false), taken_seq(0), landed_unseen(0), landed_seq(0), last_put_bytes(0), last_direct_seq(0), credit_slot_pe(-1), credit_slot(-1), credit_shm_pe(-1), credit_shm_slot(-1), credits_seen(0), shm_waiting(false), owner_signal_slot(-1), departed_signal_slot(-1), departed_signal_pe(-1), signal_slot(-1), sender_pe(-1), sender_token(0),
    sender_known(false), closing_gen(0), closing_sent(false), sender_closed(false), resolved_during_closing(false), epoch(0), closing_rec(NULL), sender_pulls(false), pulling(false), have_last(false),
    resolved(false), resolving(false), send_seq(0), credits(0), uncredited(0), pull_mode(false), closed_epoch(0), in_flight(0), staged_outstanding(0), acct_seq(0), occupant_credit_due(false),
    close_ack_due(false), close_ack_pe(-1), my_token(0), closing_at(0.0), sender_rec(NULL), holder_elem(0), holder_known(false) {
  pending.valid = false;
  memset(&last, 0, sizeof(last));
}

// A copy takes over the original's table entries: the usual shape is
// `peer = param;` in an entry method, after which the parameter dies.
void CkDeviceChannel::takeOver(const CkDeviceChannel& o) {
  memcpy(hist, o.hist, sizeof(hist)); hist_n = o.hist_n;
  if (o.my_token != 0) (*CkpvAccess(device_channel_senders))[o.my_token] = this;
  if (o.is_owner && o.channel_id != 0) (*CkpvAccess(device_channel_owners))[o.channel_id] = this;
  if (o.is_owner && o.owner_known) deviceChannelElemAdd(o.owner_mgr, o.owner_id, this);
  if (o.holder_known) deviceChannelElemAdd(o.holder_mgr, o.holder_elem, this);
}

CkDeviceChannel::CkDeviceChannel(const CkDeviceChannel& o)
  : dst(o.dst), arrived(o.arrived), control(o.control), channel_id(o.channel_id), generation(o.generation),
    cap(o.cap), is_owner(o.is_owner), owner_id(o.owner_id), owner_known(o.owner_known), owner_mgr(o.owner_mgr), busy(o.busy), taken_seq(o.taken_seq), landed_unseen(o.landed_unseen), landed_seq(o.landed_seq), last_put_bytes(o.last_put_bytes), last_direct_seq(o.last_direct_seq),
    credit_slot_pe(o.credit_slot_pe), credit_slot(o.credit_slot), credit_shm_pe(o.credit_shm_pe), credit_shm_slot(o.credit_shm_slot), credits_seen(o.credits_seen), shm_waiting(false), owner_signal_slot(o.owner_signal_slot), departed_signal_slot(o.departed_signal_slot), departed_signal_pe(o.departed_signal_pe), signal_slot(o.signal_slot), signal_hdr(o.signal_hdr),
    sender_pe(o.sender_pe), sender_token(o.sender_token), sender_known(o.sender_known),
    closing_gen(o.closing_gen), closing_sent(o.closing_sent), sender_closed(o.sender_closed), resolved_during_closing(o.resolved_during_closing), epoch(o.epoch), closing_rec(o.closing_rec), sender_pulls(o.sender_pulls), pulls(o.pulls), pulling(o.pulling), pull_ev(o.pull_ev), pull_src(o.pull_src), waiting(o.waiting), last(o.last),
    have_last(o.have_last), resolved(o.resolved), resolving(o.resolving), send_seq(o.send_seq),
    credits(o.credits), uncredited(o.uncredited), pull_mode(o.pull_mode), done_cbs(o.done_cbs), closed_epoch(o.closed_epoch), in_flight(o.in_flight), staged_outstanding(o.staged_outstanding), acct_seq(o.acct_seq), occupant_credit_due(o.occupant_credit_due), direct_issued(o.direct_issued), dones_seen(o.dones_seen), close_ack_due(o.close_ack_due),
    close_ack_pe(o.close_ack_pe), my_token(o.my_token), closing_at(o.closing_at), sender_rec(o.sender_rec),
    holder_elem(o.holder_elem), holder_mgr(o.holder_mgr), holder_known(o.holder_known), pending(o.pending), src_ep(o.src_ep) {
  takeOver(o);
}

CkDeviceChannel& CkDeviceChannel::operator=(const CkDeviceChannel& o) {
  if (this == &o) return *this;
  dst = o.dst; arrived = o.arrived; control = o.control; channel_id = o.channel_id; generation = o.generation;
  cap = o.cap; is_owner = o.is_owner; owner_id = o.owner_id; owner_known = o.owner_known; owner_mgr = o.owner_mgr; busy = o.busy; taken_seq = o.taken_seq; landed_unseen = o.landed_unseen; landed_seq = o.landed_seq; last_put_bytes = o.last_put_bytes; last_direct_seq = o.last_direct_seq;
  credit_slot_pe = o.credit_slot_pe; credit_slot = o.credit_slot; credit_shm_pe = o.credit_shm_pe; credit_shm_slot = o.credit_shm_slot; credits_seen = o.credits_seen;
  owner_signal_slot = o.owner_signal_slot; departed_signal_slot = o.departed_signal_slot; departed_signal_pe = o.departed_signal_pe; signal_slot = o.signal_slot; signal_hdr = o.signal_hdr;
  sender_pe = o.sender_pe; sender_token = o.sender_token; sender_known = o.sender_known;
  closing_gen = o.closing_gen; closing_sent = o.closing_sent; sender_closed = o.sender_closed; resolved_during_closing = o.resolved_during_closing; epoch = o.epoch; closing_rec = o.closing_rec; sender_pulls = o.sender_pulls; pulls = o.pulls; pulling = o.pulling; pull_ev = o.pull_ev; pull_src = o.pull_src; waiting = o.waiting; last = o.last;
  have_last = o.have_last; resolved = o.resolved; resolving = o.resolving; send_seq = o.send_seq;
  credits = o.credits; uncredited = o.uncredited; pull_mode = o.pull_mode; done_cbs = o.done_cbs; closed_epoch = o.closed_epoch; in_flight = o.in_flight; staged_outstanding = o.staged_outstanding; acct_seq = o.acct_seq; occupant_credit_due = o.occupant_credit_due; direct_issued = o.direct_issued; dones_seen = o.dones_seen; close_ack_due = o.close_ack_due;
  close_ack_pe = o.close_ack_pe; my_token = o.my_token; closing_at = o.closing_at; sender_rec = o.sender_rec;
  holder_elem = o.holder_elem; holder_mgr = o.holder_mgr; holder_known = o.holder_known; pending = o.pending; src_ep = o.src_ep;
  takeOver(o);
  return *this;
}

CkDeviceChannel::~CkDeviceChannel() {
  if (shm_waiting) deviceChannelShmUnlist(this);
  if (is_owner) { retireCreditSlot(); retireSignalSlot(); }
  if (my_token != 0) {
    DeviceChannelTable* tab = CkpvAccess(device_channel_senders);
    auto it = tab->find(my_token);
    if (it != tab->end() && it->second == this) tab->erase(it);
    CkpvAccess(device_channel_early_credits)->erase(my_token);
  }
  if (is_owner && channel_id != 0) unregisterOwner();
  if (is_owner && owner_known) deviceChannelElemDrop(owner_mgr, owner_id, this);
  if (holder_known) deviceChannelElemDrop(holder_mgr, holder_elem, this);
}

void CkDeviceChannel::registerOwner() {
  (*CkpvAccess(device_channel_owners))[channel_id] = this;
}

void CkDeviceChannel::unregisterOwner() {
  DeviceChannelTable* tab = CkpvAccess(device_channel_owners);
  auto it = tab->find(channel_id);
  if (it != tab->end() && it->second == this) tab->erase(it);
}

void CkDeviceChannel::open() {
  if (dst.ptr == nullptr || dst.cnt == 0) {
    CkAbort("CkDeviceChannel::open: no destination buffer; construct the channel with one\n");
  }
  if (arrived.type == CkCallback::ignore) {
    CkAbort("CkDeviceChannel::open: no arrival callback; a channel needs an entry that learns of "
            "each put\n");
  }
  if (channel_id == 0) channel_id = deviceChannelToken();
  generation = 1;
  cap = dst.cnt;
  if (getenv("CHARM_DEBUG_MIGRATE"))
    CmiPrintf("[CHAN-OPEN %d] id=%llx owner_id=%llu rec=%p obj=%p\n", CkMyPe(),
              (unsigned long long)channel_id,
              (unsigned long long)(CkpvAccess(_currentLocRec) ? CkpvAccess(_currentLocRec)->getID() : 0),
              (void*)CkpvAccess(_currentLocRec), (void*)this);
  // The receiving element, for the migration hook. NULL outside an element's
  // entry method (a group, or a call from the main chare): the channel then has
  // no owner to stand down, which is right for a chare that never migrates.
  CkLocRec* rec = CkpvAccess(_currentLocRec);
  owner_id = rec ? rec->getID() : 0;
  owner_known = (rec != NULL);
  if (rec) owner_mgr = rec->getLocMgr()->ckGetGroupID();
  // Runtime control events (a sender's resolve) go to the element itself, not
  // through the application's entry: an SDAG `when arrived[step]` would hold
  // them forever. For an array element that is its ckDeviceChannelControl
  // entry; anything else (a group) gets them on `arrived`, where event() hands
  // the application NULL.
  if (rec && arrived.type == CkCallback::sendArray) {
    control = CkCallback(CkIndex_ArrayElement::ckDeviceChannelControl(NULL), rec->getIndex(),
                         CkArrayID(arrived.d.array.id));
  } else {
    control = arrived;
  }
  dst.open();
  is_owner = true;
  registerOwner();
  if (owner_known) deviceChannelElemAdd(owner_mgr, owner_id, this);
}

void CkDeviceChannel::reopen(void* new_dst) {
  if (channel_id == 0 || cap == 0) {
    CkAbort("CkDeviceChannel::reopen: the channel was never open()ed\n");
  }
  dst = CkDevicePersistent(new_dst, cap);
  generation++;
  MIGC("reopen-ch", channel_id, generation - 1, sender_pe);
  dst.open();
  is_owner = true;
  // Every sender of the previous home was closed; they re-resolve.
  sender_known = false;
  closing_gen = 0;
  closing_sent = false;
  resolved_during_closing = false;
  sender_closed = false;
  closing_rec = NULL;
  registerOwner();
  if (owner_known) deviceChannelElemAdd(owner_mgr, owner_id, this);
  // Senders that asked while the element was on its way; then the last
  // sender, closed before the move, gets the new resolution unasked (one
  // round trip less before its next put).
  const int last_pe = sender_pe; const uint64_t last_token = sender_token;
  const bool push = sender_known && sender_closed;
  std::vector<Waiting> w;
  w.swap(waiting);
  bool asked = false;
  for (const Waiting& x : w) { replyResolve(x.pe, x.token); if (x.pe == last_pe && x.token == last_token) asked = true; }
  if (push && !asked) replyResolve(last_pe, last_token);
  // Announces that travelled with the element: read into the new buffer.
  if (!busy && !pulling) startPull();
}

// Handoff, old PE: the object leaves with the element. Nothing is in flight
// (the puts counted as outstanding sends; the senders were closed and acked).
void CkDeviceChannel::leaveForHandoff() {
  note(15, send_seq, epoch, CkMyPe(), credit_shm_slot, credits);
  if (is_owner) unregisterOwner();   // the export stays: same process, same buffer
  if (my_token != 0) {
    DeviceChannelTable* tab = CkpvAccess(device_channel_senders);
    auto it = tab->find(my_token);
    if (it != tab->end() && it->second == this) tab->erase(it);
    CkpvAccess(device_channel_early_credits)->erase(my_token);
    my_token = 0;
  }
  // A sender copy re-resolves from its new PE: anything addressed to the old
  // PE's token (a credit, a reply) is dropped there and re-learned.
  if (shm_waiting) { deviceChannelShmUnlist(this); shm_waiting = false; }
  credit_shm_slot = -1; credit_shm_pe = -1; credits_seen = 0; signal_slot = -1;
  resolved = false;
  resolving = false;
  closed_epoch = 0;
  in_flight = 0;
  close_ack_due = false;
  sender_rec = NULL;
}

// Handoff, new PE: the owned channel is the same buffer under a new
// generation; its sender was closed and re-resolves.
void CkDeviceChannel::arriveFromHandoff() {
  note(16, send_seq, epoch, CkMyPe(), credit_shm_slot, credits);
  if (getenv("CHARM_DEBUG_MIGRATE") || chanSignalDebug())
    CmiPrintf("[CHAN-HANDOFF %d] arrive channel %llx owner=%d gen=%llu obj=%p\n", CkMyPe(),
              (unsigned long long)channel_id, (int)is_owner, (unsigned long long)generation, (void*)this);
  if (!is_owner) return;
  generation++;
  const int last_pe = sender_pe; const uint64_t last_token = sender_token;
  const bool push = sender_known;
  closing_gen = 0;
  closing_sent = false;
  resolved_during_closing = false;
  sender_closed = true;
  closing_rec = NULL;
  registerOwner();
  std::vector<Waiting> w;
  w.swap(waiting);
  bool asked = false;
  for (const Waiting& x : w) { replyResolve(x.pe, x.token); if (x.pe == last_pe && x.token == last_token) asked = true; }
  if (push && !asked) replyResolve(last_pe, last_token);
  if (!busy && !pulling) startPull();
}

void CkDeviceChannel::close() {
  if (!is_owner) return;
  retireCreditSlot();
  retireSignalSlot();
  dst.close();
  unregisterOwner();
  is_owner = false;
}

const CkDeviceChannelEvent* CkDeviceChannel::event(CkDataMsg* m) {
  if (m == NULL || m->getLength() < (int)sizeof(CkDeviceChannelEvent)) {
    CkAbort("CkDeviceChannel::event: the message is not a channel event (%d bytes, %zu expected)\n",
            m ? m->getLength() : -1, sizeof(CkDeviceChannelEvent));
  }
  CkDeviceChannelEvent ev;
  memcpy(&ev, m->getData(), sizeof(ev));
  if (ev.kind == CkDeviceChannelEvent::ANNOUNCE) {
    // The sender's registered source follows the event; this side reads it
    // when the buffer is free. The announce followed the element here if it
    // moved, and the source stays valid until this side says DONE.
    CkDevicePersistent src;
    PUP::fromMem p((const char*)m->getData() + sizeof(ev));
    p | src;
    onAnnounce(ev, src);
    return NULL;
  }
  if (ev.channel != channel_id) {
    CkAbort("CkDeviceChannel::event: event for channel %llx taken on channel %llx\n",
            (unsigned long long)ev.channel, (unsigned long long)channel_id);
  }
  if (ev.kind == CkDeviceChannelEvent::RESOLVE) {
    if (!is_owner) {
      // Packed and on the way to another PE: answered after reopen there.
      Waiting w; w.pe = ev.sender_pe; w.token = ev.sender_token;
      waiting.push_back(w);
      return NULL;
    }
    // Answered even while a migration is pending: the buffer is valid until the
    // pack, and holding the sender would hold the element's own step, which is
    // what it must finish before it can move (the split barrier packs between
    // steps). The sender is then closed at once, so the stand-down covers its
    // puts too; it re-resolves after the move as any sender does.
    replyResolve(ev.sender_pe, ev.sender_token);
    if (closing_gen == generation && closing_rec != NULL) closingForMigration(closing_rec);
    return NULL;
  }
  if (ev.generation != generation && deviceChannelDebug())
    CmiPrintf("[CHAN-STALE %d] channel %llx: put %llu into generation %llu taken at generation %llu (its bytes came with the pup)\n",
              CkMyPe(), (unsigned long long)channel_id, (unsigned long long)ev.seq,
              (unsigned long long)ev.generation, (unsigned long long)generation);
  last = ev;
  have_last = true;
  busy = true;
  taken_seq = ev.seq;
  note(10, ev.seq, epoch, ev.sender_pe, -1, (int)sender_known);
  landed_unseen = 0;
  landed_seq = 0;
  if (zcStatsOnFast()) zc_mode_stats.event_n.fetch_add(1, std::memory_order_relaxed);
  return &last;
}

// The current endpoint to a sender; that sender is the one credits and the
// closing go to from now on.
void CkDeviceChannel::replyResolve(int pe, uint64_t token) {
  deviceChannelProgress();
  deviceChannelLateInit();
  const bool sender_closed_before_reply = !sender_known || sender_closed;
  sender_pe = pe;
  sender_token = token;
  sender_known = true;
  sender_closed = false;
  sender_pulls = false;   // the tier may differ now; an announce sets it again
  // A closing on its way to the previous resolution may be acked after this
  // reply is out (the sender's request crossed its ack): the ack then closes
  // this resolution too, before the element may move.
  if (closing_sent) resolved_during_closing = true;
  epoch++;
  // The previous resolution's credit slot: a sender reads it only while that
  // resolution is current for it, and checks channel and epoch around the
  // read, so it can go back now.
  retireCreditSlot();
  if (chanShmPair(pe)) {
    credit_slot = chanShmAllocCredit(channel_id, epoch);
    credit_slot_pe = (credit_slot >= 0) ? chanShmIndexOf(CkMyPe()) : -1;
  }
  note(12, taken_seq, epoch, pe, credit_slot, (int)busy);
  MIGC("reply-ch", channel_id, generation, pe);
  // Another host: the sender's puts signal a slot of this process. The
  // previous slot goes back only once that resolution was closed and acked
  // (a sender still under it may put again); otherwise it waits for the ack.
  if (sender_closed_before_reply) retireSignalSlot();   // (already released by the ack; no-op)
  else stashSignalSlot();                                // the ack of the previous resolution releases it
  if (CmiPhysicalNodeID(pe) != CmiPhysicalNodeID(CkMyPe())) {
    owner_signal_slot = chanSignalAlloc(arrived, channel_id, generation, pe, token);
    static thread_local bool said = false;
    if (!said) { said = true; CmiPrintf("[CHAN-INIT %d] first cross-host reply: signal slot %d (table ready %d, free %zu)\n", CkMyPe(), owner_signal_slot, (int)chan_signal_ready, chan_signal_free.size()); }
  }
  if (deviceChannelDebug() || chanSignalDebug())
    CmiPrintf("[CHAN-RESOLVE %d] channel %llx gen %llu: resolution %llu to pe %d token %llx (free=%d, closing in flight=%d, pulling=%d taken_seq=%llu pulls=%zu, signal slot %d, header region %p)\n",
              CkMyPe(), (unsigned long long)channel_id, (unsigned long long)generation, (unsigned long long)epoch,
              pe, (unsigned long long)token, (int)!busy, (int)closing_sent, (int)pulling, (unsigned long long)taken_seq, pulls.size(),
              owner_signal_slot, (void*)chan_signal_hdr);
  PUP::sizer sz;
  sz | dst;
  const size_t n = sz.size();
  size_t hn = 0;
  if (owner_signal_slot > 0) { PUP::sizer hz; hz | chan_signal_hdr_region; hn = hz.size(); }
  auto* m = deviceChannelMsg<DeviceChannelResolved>(device_channel_resolved_handler, n + hn);
  m->token = token;
  m->channel = channel_id;
  m->generation = generation;
  m->owner_pe = CkMyPe();
  m->epoch = epoch;
  m->free = busy ? 0 : 1;
  // The last of the sender's puts to have entered the buffer: taken_seq once event() took it;
  // before that, a staged put read (pull_ev; onPulled set busy and sent its DONE) or a direct put
  // that landed before the pack and came with the pup (landed_seq, from the close ack). The
  // sender counts only puts after this one as still on their way.
  m->taken_seq = taken_seq;
  if (busy && !have_last) {
    // A direct put that landed before the pack (the close ack's last_seq) or a staged put read
    // but not yet taken; pull_ev is written by the first read only, so it is consulted only for
    // the latter (an owner that never read a staged put sent its uninitialised seq as taken_seq,
    // and the sender dropped the early credit it was holding: Frontier, 2026-10-22).
    if (landed_unseen > 0 && landed_unseen != SIZE_MAX) { if (landed_seq > m->taken_seq) m->taken_seq = landed_seq; }
    else if (pull_ev.seq > m->taken_seq) m->taken_seq = pull_ev.seq;
  }
  m->desc_bytes = (int)n;
  m->dst_pe = pe;
  m->src_pe = CkMyPe();
  m->credit_pe = credit_slot_pe;
  m->credit_slot = credit_slot;
  m->signal_slot = owner_signal_slot;
  m->hdr_bytes = (int)hn;
  PUP::toMem tm((char*)m + sizeof(DeviceChannelResolved));
  tm | dst;
  if (hn > 0) tm | chan_signal_hdr_region;
  CmiSyncSendAndFreeUrgent(pe, (int)(sizeof(DeviceChannelResolved) + n + hn), (char*)m);
}

void CkDeviceChannel::retireSignalSlot() {
  if (owner_signal_slot < 0) return;
  chanSignalFree(owner_signal_slot);
  owner_signal_slot = -1;
}
// The slot of a resolution whose closing ack has not arrived: kept, its
// release comes with the ack (onCloseAck). An older stash still pending is
// released blind (a second move before the first ack).
void CkDeviceChannel::stashSignalSlot() {
  if (owner_signal_slot <= 0) return;
  if (departed_signal_slot > 0) deviceChannelSlotRelease(departed_signal_pe, departed_signal_slot, UINT64_MAX);
  departed_signal_slot = owner_signal_slot;
  departed_signal_pe = CkMyPe();
  owner_signal_slot = -1;
}

void CkDeviceChannel::retireCreditSlot() {
  if (credit_slot < 0) return;
  ChanShmCredit& c = chanShmArea(credit_slot_pe).credit[credit_slot];
  c.epoch.store(0, std::memory_order_release);
  c.channel.store(0, std::memory_order_release);
  credit_slot = -1;
  credit_slot_pe = -1;
}

// Sender: credits granted through shared memory since the last look. A put
// queued behind one is issued here.
void CkDeviceChannel::pollShmCredit() {
  if (credit_shm_slot < 0 || !resolved) return;
  const long n = chanShmReadCredit(credit_shm_pe, credit_shm_slot, channel_id, epoch);
  if (n < 0 || (uint32_t)n <= credits_seen) return;
  const uint32_t fresh = (uint32_t)n - credits_seen;
  if (deviceChannelDebug() || chanSignalDebug())
    CmiPrintf("[CHAN-CREDIT-RX %d] channel %llx token %llx: %u shm credit(s) from slot %d of area %d under epoch %llu (count %ld seen %u): credits %d pending %d\n",
              CkMyPe(), (unsigned long long)channel_id, (unsigned long long)my_token, fresh, credit_shm_slot, credit_shm_pe,
              (unsigned long long)epoch, n, credits_seen, credits, (int)pending.valid);
  credits_seen = (uint32_t)n;
  note(6, 0, epoch, credit_shm_pe, credit_shm_slot, (int)fresh);
  deviceChannelProgress();
  for (uint32_t i = 0; i < fresh; i++) {
    credits++;
    if (uncredited > 0) uncredited--;
    creditedPut(0);   // the slot carries no sequence: credits come in put order
  }
  if (pending.valid && credits > 0) {
    Pending p = pending;
    pending.valid = false;
    credits--;
    uncredited++;
    issue(p.src, p.bytes, p.stream, p.done, p.tag);
  }
}

void CkDeviceChannel::release() {
  deviceChannelProgress();
  if (!have_last) {
    CkAbort("CkDeviceChannel::release: nothing to release; event() records the put first\n");
  }
  have_last = false;
  busy = false;
  // The next staged put, if one waits and is next in order.
  if (!pulls.empty()) startPull();
  // Every release credits the current sender, after a read staged put as after
  // a direct one: the sender debited that read at its DONE (onStagedDone), so
  // the two pair off and the balance stays one buffer. (The pull tier that let
  // a sender announce without credits is off: pull_mode is never set.)
  // No resolved sender (it re-resolves after our migration and learns the
  // buffer is free then): nothing to credit.
  if (!sender_known) return;
  note(11, last.seq, epoch, sender_pe, credit_slot, 0);
  if (deviceChannelDebug() || chanSignalDebug())
    CmiPrintf("[CHAN-CREDIT %d] channel %llx: release of put %llu credits pe %d token %llx under epoch %llu via %s (taken_seq %llu)\n",
              CkMyPe(), (unsigned long long)channel_id, (unsigned long long)last.seq, sender_pe, (unsigned long long)sender_token,
              (unsigned long long)epoch, credit_slot >= 0 ? "shm" : "msg", (unsigned long long)taken_seq);
  if (credit_slot >= 0) {
    // Same host, other process: one shared-memory increment, no message.
    { static thread_local bool said = false; if (!said) { said = true; CmiPrintf("[CHAN-INIT %d] first credit through shared memory (to pe %d)\n", CkMyPe(), sender_pe); } }
    chanShmArea(credit_slot_pe).credit[credit_slot].count.fetch_add(1, std::memory_order_acq_rel);
    if (deviceChannelDebug() || chanSignalDebug())
      CmiPrintf("[CHAN-CREDIT %d] channel %llx: release of put %llu credits pe %d token %llx under epoch %llu via shm slot %d of area %d (count now %u)\n",
                CkMyPe(), (unsigned long long)channel_id, (unsigned long long)last.seq, sender_pe, (unsigned long long)sender_token,
                (unsigned long long)epoch, credit_slot, credit_slot_pe, (unsigned)chanShmArea(credit_slot_pe).credit[credit_slot].count.load());
    if (zcStatsOnFast()) zc_mode_stats.credit_n[0].fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const double t0 = zcStatsOnFast() ? CkWallTimer() : 0.0;
  auto* m = deviceChannelMsg<DeviceChannelCredit>(device_channel_credit_handler);
  m->token = sender_token;
  m->channel = channel_id;
  m->seq = last.seq;
  m->epoch = epoch;
  m->dst_pe = sender_pe;
  m->src_pe = CkMyPe();
  const double t1 = (t0 != 0.0) ? CkWallTimer() : 0.0;
  CmiSyncSendAndFreeUrgent(sender_pe, sizeof(DeviceChannelCredit), (char*)m);
  if (t0 != 0.0) {
    const int k = (CmiPhysicalNodeID(sender_pe) == CmiPhysicalNodeID(CkMyPe())) ? 0 : 1;
    zc_mode_stats.credit_alloc_us[k].fetch_add((long)((t1 - t0) * 1e6), std::memory_order_relaxed);
    zc_mode_stats.credit_send_us[k].fetch_add((long)((CkWallTimer() - t1) * 1e6), std::memory_order_relaxed);
    zc_mode_stats.credit_n[k].fetch_add(1, std::memory_order_relaxed);
  }
}

// Migration of the receiving element (from CkLocMgr::emigrate): the resolved
// sender is told the channel is closing and the element is charged one
// outstanding device send until the ack. Once per generation.
void CkDeviceChannel::closingForMigration(CkLocRec* rec) {
  if (!is_owner) return;
  note(13, taken_seq, epoch, sender_pe, -1, (int)sender_known);
  if (!closing_sent) MIGC("closing-tx", channel_id, generation, sender_pe);
  // The pending migration is noted on every owned channel, resolved sender or
  // not: a sender that resolves while the element stands down (for another
  // channel's ack) is closed at once from event(), or its put could land after
  // the pack.
  closing_gen = generation;
  closing_rec = rec;
  if (!sender_known || sender_closed || closing_sent) return;
  closing_sent = true;
  resolved_during_closing = false;
  rec->noteDeviceSendPosted();
  if (ckMigStatsOn()) closing_at = CkWallTimer();
  if (deviceChannelDebug() || chanSignalDebug())
    CmiPrintf("[CHAN-CLOSE %d] channel %llx gen %llu: closing resolution %llu sent to pe %d token %llx; element %llu outstanding now %d\n",
              CkMyPe(), (unsigned long long)channel_id, (unsigned long long)generation, (unsigned long long)epoch, sender_pe,
              (unsigned long long)sender_token, (unsigned long long)rec->getID(), rec->outstandingDeviceSends);
  // The protocol's control messages (this closing, its ack, the resolution,
  // the credit, the DONE) go urgent: the destination serves them ahead of its
  // queue, so each hop costs the entry method running there, not the depth of
  // the queue (50-400 ms at 448 objects per PE).
  auto* m = deviceChannelMsg<DeviceChannelClosing>(device_channel_closing_handler);
  m->token = sender_token;
  m->channel = channel_id;
  m->generation = generation;
  m->epoch = epoch;
  m->owner_pe = CkMyPe();
  CmiSyncSendAndFreeUrgent(sender_pe, sizeof(DeviceChannelClosing), (char*)m);
}

void CkDeviceChannel::onCloseAck(int pe, uint64_t token, uint64_t last_seq, uint64_t last_bytes) {
  deviceChannelProgress();
  note(14, last_seq, epoch, pe, -1, (int)last_bytes);
  (void)pe; (void)token;
  // What the sender put last against what this side has taken: a put that
  // landed while the element was closing has its event still queued, and the
  // element's pup must carry those bytes (unseenBytes()). Not free until the
  // application takes and releases it.
  if (last_seq == UINT64_MAX) landed_unseen = SIZE_MAX;
  else if (last_seq > taken_seq) { landed_unseen = (size_t)last_bytes; landed_seq = last_seq; busy = true; }
  if (deviceChannelDebug() || chanSignalDebug())
    CmiPrintf("[CHAN-CLOSE %d] channel %llx: ack from pe %d token %llx (owner=%d closing_gen %llu gen %llu sent=%d closed=%d)\n",
              CkMyPe(), (unsigned long long)channel_id, pe, (unsigned long long)token, (int)is_owner,
              (unsigned long long)closing_gen, (unsigned long long)generation, (int)closing_sent, (int)sender_closed);
  // The slots of the closed resolution: whichever process granted them frees them once the acked
  // last put has been taken there (or now, for a blind ack). Both when both exist: a sender answered
  // twice after a move (the pushed reply and its own request, 10 us apart) leaves the first reply's
  // slot stashed and the second's current, and releasing only the stashed one left the current slot
  // referenced, to be freed blind by the next reply's retireSignalSlot while the acked put's note was
  // still on its way; the free list handed the same slot straight back and the allocation's reset
  // wiped that put's header (Frontier, 2026-10-22: "the header of a put with signal never arrived").
  if (departed_signal_slot > 0) { deviceChannelSlotRelease(departed_signal_pe, departed_signal_slot, last_seq); departed_signal_slot = -1; departed_signal_pe = -1; }
  if (owner_signal_slot > 0 && is_owner) { deviceChannelSlotRelease(CkMyPe(), owner_signal_slot, last_seq); owner_signal_slot = -1; }
  if (!is_owner || !closing_sent) return;
  closing_sent = false;
  if (closing_at > 0.0) { ckMigStatChannel(1, CkWallTimer() - closing_at); closing_at = 0.0; }
  retireCreditSlot();
  CkLocMgr* mgr = (CkLocMgr*)CkLocalBranch(owner_mgr);
  CkLocRec* rec = mgr ? mgr->recForDeviceWork(owner_id) : NULL;
  const bool reclose = resolved_during_closing;
  resolved_during_closing = false;
  // The sender resolved again while that closing was out (its request crossed
  // the ack): that resolution is closed too, posted before this ack's count-down
  // so the element's outstanding count never touches zero in between.
  if (reclose && rec) closingForMigration(rec);
  else sender_closed = true;
  // The element stands down on this PE until this ack.
  if (rec) rec->noteDeviceSendDone(/*urgentKick=*/true);   // the senders are held until this element lands
  else if (deviceChannelDebug())
    CmiPrintf("[CHAN-CLOSE %d] channel %llx: ack but element %llu not found here\n", CkMyPe(),
              (unsigned long long)channel_id, (unsigned long long)owner_id);
}

static const char* const chan_hist_names[] = {
  "put", "pend", "staged", "resolve-req", "resolved", "credit-msg", "credit-shm", "closing-rx", "closeack-tx",
  "srcdone", "taken", "release", "reply", "closing-tx", "closeack-rx", "handoff-leave", "handoff-arrive", "pack",
  "unpack", "done-rx", "early-credit", "stale-credit"};
void CkDeviceChannel::note(uint8_t kind, uint64_t seq, uint64_t epoch_, int pe, int slot, int n) {
  Hist& h = hist[hist_n++ % HIST_N];
  h.kind = kind; h.pe = pe; h.slot = slot; h.n = n; h.seq = seq; h.epoch = epoch_; h.t = CkWallTimer();
}
void CkDeviceChannel::printHist(const char* role) const {
  char buf[2600]; int off = 0;
  const uint32_t n = hist_n < (uint32_t)HIST_N ? hist_n : (uint32_t)HIST_N;
  const unsigned nnames = sizeof(chan_hist_names) / sizeof(*chan_hist_names);
  for (uint32_t i = 0; i < n; i++) {
    const Hist& h = hist[(hist_n - n + i) % HIST_N];
    off += snprintf(buf + off, sizeof(buf) - off, " [%s s%llu e%llu pe%d sl%d n%d @%.3f]",
                    h.kind < nnames ? chan_hist_names[h.kind] : "?", (unsigned long long)h.seq,
                    (unsigned long long)h.epoch, h.pe, h.slot, h.n, h.t);
    if (off >= (int)sizeof(buf) - 80) break;
  }
  long raw_cnt = -1; unsigned long long raw_ch = 0, raw_ep = 0; int w_pe = -1, w_slot = -1;
  if (!is_owner && credit_shm_slot >= 0) { w_pe = credit_shm_pe; w_slot = credit_shm_slot; }
  if (is_owner && credit_slot >= 0) { w_pe = credit_slot_pe; w_slot = credit_slot; }
  if (w_slot >= 0) {
    const ChanShmCredit& c = chanShmArea(w_pe).credit[w_slot];
    raw_ch = c.channel.load(std::memory_order_acquire); raw_ep = c.epoch.load(std::memory_order_acquire);
    raw_cnt = (long)c.count.load(std::memory_order_acquire);
  }
  CmiPrintf("[CHAN-HIST %d] %s channel %llx token %llx: credit word (area %d slot %d): channel %llx epoch %llu count %ld, seen %u, credits %d; last %u events:%s\n",
            CkMyPe(), role, (unsigned long long)channel_id, (unsigned long long)my_token, w_pe, w_slot, raw_ch, raw_ep, raw_cnt,
            credits_seen, credits, n, buf);
}

void CkDeviceChannel::describeStall(const char* role) const {
  if (const DeviceChannelProbeCtx* pc = device_channel_probe_ctx) {
    CmiPrintf("[CHAN-PROBE %d] %s channel %llx gen %llu, asked by pe %d token %llx (its epoch %llu, seq %llu): "
              "is_owner=%d busy=%d have_last=%d last(seq %llu tag %llu kind %d from pe %d token %llx) epoch=%llu "
              "sender_known=%d sender=%d/%llx sender_closed=%d closing_sent=%d closing_gen=%llu waiting=%zu pulls=%zu taken_seq=%llu unseen=%zu "
              "| as sender: resolved=%d credits=%d in_flight=%d pending=%d token=%llx\n",
              CkMyPe(), role, (unsigned long long)channel_id, (unsigned long long)generation, pc->pe,
              (unsigned long long)pc->token, (unsigned long long)pc->epoch, (unsigned long long)pc->seq,
              (int)is_owner, (int)busy, (int)have_last, (unsigned long long)last.seq, (unsigned long long)last.tag,
              (int)last.kind, last.sender_pe, (unsigned long long)last.sender_token, (unsigned long long)epoch,
              (int)sender_known, sender_pe, (unsigned long long)sender_token, (int)sender_closed, (int)closing_sent,
              (unsigned long long)closing_gen, waiting.size(), pulls.size(), (unsigned long long)taken_seq, landed_unseen,
              (int)resolved, credits, in_flight, (int)pending.valid, (unsigned long long)my_token);
    printHist(role);
    return;
  }
  const bool sender_stuck = pending.valid || resolving || in_flight > 0 || close_ack_due || staged_outstanding > 0;
  const bool owner_stuck = is_owner && (!waiting.empty() || closing_sent || !pulls.empty());
  if (!sender_stuck && !owner_stuck) return;
  if (device_channel_watch_tally && !is_owner && pending.valid && resolved && credits == 0 && in_flight == 0) {
    // Waiting for a credit that is not coming: ask the owner what it thinks.
    auto* m = deviceChannelMsg<DeviceChannelProbe>(device_channel_probe_handler);
    m->channel = channel_id; m->token = my_token; m->epoch = epoch; m->seq = send_seq; m->pe = CkMyPe();
    m->owner_known = owner_known ? 1 : 0; m->owner_id = owner_id; m->owner_mgr = owner_mgr;
    CmiSyncSendAndFree(dst.pe, sizeof(DeviceChannelProbe), (char*)m);
  }
  if (DeviceChannelWatchTally* t = device_channel_watch_tally) {
    t->s_inflight += in_flight > 0; t->s_pending += pending.valid; t->s_resolving += resolving;
    t->s_closeack += close_ack_due;
    if (is_owner) { t->o_waiting += !waiting.empty(); t->o_closing += closing_sent; t->o_pulls += !pulls.empty(); }
    if (t->shown >= 12) return;
    t->shown++;
  }
  // A put in flight: is its source stream still working, or did its
  // completion callback get lost?
  const char* stream_state = "-";
  if (in_flight > 0) {
    const hapiError_t q = hapiStreamQuery(src_ep.hapi_stream);
    stream_state = (q == hapiSuccess) ? "idle" : (q == hapiErrorNotReady) ? "busy" : hapiGetErrorName(q);
  }
  CmiPrintf("[CHAN-STALL %d] %s channel %llx gen %llu owner_pe %d | sender: resolved=%d resolving=%d credits=%d "
            "in_flight=%d(stream %s, tier %s) pending=%d(tag %llu) close_ack_due=%d token=%llx seq=%llu epoch=%llu closed=%llu pull=%d | owner: is_owner=%d busy=%d pulls=%zu pulling=%d "
            "sender_known=%d sender=%d/%llx closing_gen=%llu sent=%d closed=%d waiting=%zu have_last=%d\n",
            CkMyPe(), role, (unsigned long long)channel_id, (unsigned long long)generation, dst.pe,
            (int)resolved, (int)resolving, credits, in_flight, stream_state,
            !resolved ? "?" : findTransferModeDevice(CkMyPe(), dst.pe) == CkNcpyModeDevice::MEMCPY ? "memcpy"
                            : findTransferModeDevice(CkMyPe(), dst.pe) == CkNcpyModeDevice::IPC ? "ipc" : "rdma",
            (int)pending.valid,
            (unsigned long long)(pending.valid ? pending.tag : 0), (int)close_ack_due,
            (unsigned long long)my_token, (unsigned long long)send_seq, (unsigned long long)epoch,
            (unsigned long long)closed_epoch, (int)pull_mode, (int)is_owner, (int)busy, pulls.size(), (int)pulling,
            (int)sender_known, sender_pe, (unsigned long long)sender_token, (unsigned long long)closing_gen,
            (int)closing_sent, (int)sender_closed, waiting.size(), (int)have_last);
  if (staged_outstanding > 0 || !pulls.empty())
    CmiPrintf("[CHAN-STALL %d]   staged puts outstanding %d; queued announces %zu, head seq %llu, taken_seq %llu\n",
              CkMyPe(), staged_outstanding, pulls.size(),
              pulls.empty() ? 0ULL : (unsigned long long)pulls.front().ev.seq, (unsigned long long)taken_seq);
  printHist(role);
}

// Past the stand-down, before the pack: the buffer is about to go.
void CkDeviceChannel::packForMigration() {
  if (!is_owner) return;
  note(17, taken_seq, epoch, sender_pe, credit_slot, (int)busy);
  MIGC("pack-ch", channel_id, generation, sender_pe);
  retireCreditSlot();
  stashSignalSlot();
  dst.close();
  unregisterOwner();
  if (owner_known) deviceChannelElemDrop(owner_mgr, owner_id, this);
  is_owner = false;
  sender_known = false;
  closing_sent = false;
  resolved_during_closing = false;
  sender_closed = false;
  closing_rec = NULL;
}

CkDeviceStatus CkDeviceChannel::put(const void* src, size_t bytes, hapiStream_t stream,
                                    const CkCallback& src_done, uint64_t tag) {
  deviceChannelProgress();
  deviceChannelLateInit();
  struct PutTimer {   // CHARM_ZC_STATS: the host time of this call, by tier
    double t0; const CkDeviceChannel* c;
    ~PutTimer() {
      if (t0 == 0.0) return;
      const int k = !c->resolved ? 2 : (CmiPhysicalNodeID(c->dst.pe) == CmiPhysicalNodeID(CkMyPe()) ? 0 : 1);
      zc_mode_stats.put_n[k].fetch_add(1, std::memory_order_relaxed);
      zc_mode_stats.put_us[k].fetch_add((long)((CkWallTimer() - t0) * 1e6), std::memory_order_relaxed);
    }
  } put_timer{zcStatsOnFast() ? CkWallTimer() : 0.0, this};
  if (channel_id == 0) {
    CkAbort("CkDeviceChannel::put: the channel was never open()ed by its receiver\n");
  }
  if (bytes > cap) {
    CkAbort("CkDeviceChannel::put: %zu bytes into a channel of %zu\n", bytes, cap);
  }
  if (sender_rec == NULL) {
    sender_rec = CkpvAccess(_currentLocRec);
    if (sender_rec) {
      // A copy an element holds: found again if the element is handed to
      // another PE of this process.
      holder_elem = sender_rec->getID();
      holder_mgr = sender_rec->getLocMgr()->ckGetGroupID();
      holder_known = true;
      deviceChannelElemAdd(holder_mgr, holder_elem, this);
    }
  }
  // The put counts as an outstanding device send of the sending element from
  // now until its source is done, queued or not: the element does not migrate
  // with a put of its buffers in flight or waiting.
  if (sender_rec) sender_rec->noteDeviceSendPosted();
  // The balancers' communication edge, recorded here while the sending element
  // is the running object: a put issued later from a credit or resolve handler
  // would be charged to whatever object runs then.
  deviceChannelRecordSend(sender_rec, bytes, dst.pe, owner_known, owner_mgr, owner_id);
  // Closed by the receiver's migration, or still behind puts staged while it
  // was: the put goes as a staged announce, holding nothing here (issueStaged).
  if (deviceChannelDebug())
    CmiPrintf("[CHAN-PUT %d] channel %llx token %llx: put tag %llu: resolved %d resolving %d closed_epoch %llu credits %d staged %d pending %d\n",
              CkMyPe(), (unsigned long long)channel_id, (unsigned long long)my_token, (unsigned long long)tag, (int)resolved,
              (int)resolving, (unsigned long long)closed_epoch, credits, staged_outstanding, (int)pending.valid);
  if ((!resolved && closed_epoch > 0) || (resolved && staged_outstanding > 0)) {
    issueStaged(src, bytes, stream, src_done, tag);
    return CkDeviceStatus::incomplete;
  }
  if (resolved && !pull_mode && credits == 0 && credit_shm_slot >= 0) pollShmCredit();   // a credit may be waiting in shared memory
  if (resolved && (pull_mode || credits > 0)) {
    if (!pull_mode) { credits--; uncredited++; }
    issue(src, bytes, stream, src_done, tag);
  } else if (!pending.valid) {
    if (zcStatsOnFast()) zc_mode_stats.put_pending.fetch_add(1, std::memory_order_relaxed);
    pending.src = src;
    pending.bytes = bytes;
    pending.stream = stream;
    pending.done = src_done;
    pending.tag = tag;
    pending.valid = true;
    note(1, send_seq, epoch, dst.pe, credit_shm_slot, credits);
    if (resolved && credit_shm_slot >= 0 && !shm_waiting) {
      shm_waiting = true;
      CkpvAccess(device_channel_shm_waiters)->push_back(this);
    }
    if (!resolved && !resolving) requestResolve();
  } else {
    CkAbort("CkDeviceChannel::put: a put is already queued behind the credit for put %llu on "
            "channel %llx; the receiver's release() frees the buffer, a second channel overlaps\n",
            (unsigned long long)send_seq, (unsigned long long)channel_id);
  }
  return CkDeviceStatus::incomplete;
}

// From the owning element's pup on unpack: this channel arrived with it and is
// reopened later (ckJustMigrated, on the final buffer). In the table from now
// so that a resolve or an announce reaching the element first is queued here,
// not refused. A copy unpacked as an entry-method parameter must not call it
// (the parameter is a temporary; a table entry for it would dangle).
void CkDeviceChannel::arriving() {
  if (channel_id != 0 && !is_owner) registerOwner();
}

// A holder that moved asks for the receiver's endpoint now rather than at its
// next put (the round trip then overlaps the step instead of delaying it).
void CkDeviceChannel::resolve() {
  if (channel_id == 0 || resolved || resolving) return;
  requestResolve();
}

void CkDeviceChannel::requestResolve() {
  if (my_token == 0) {
    my_token = deviceChannelToken();
    (*CkpvAccess(device_channel_senders))[my_token] = this;
  }
  resolving = true;
  note(3, send_seq, epoch, dst.pe, -1, credits);
  CkDeviceChannelEvent ev;
  memset(&ev, 0, sizeof(ev));
  ev.kind = CkDeviceChannelEvent::RESOLVE;
  ev.channel = channel_id;
  ev.sender_pe = CkMyPe();
  ev.sender_token = my_token;
  control.send(CkDataMsg::buildNew((int)sizeof(ev), &ev));
}

void CkDeviceChannel::onResolved(const CkDevicePersistent& ep, uint64_t gen_, int owner_pe,
                                 bool free, uint64_t epoch_, int credit_pe, int credit_slot_,
                                 int signal_slot_, const CmiNcpyBuffer* signal_hdr_, uint64_t taken_seq_) {
  deviceChannelProgress();
  (void)owner_pe;
  // Messages between two PEs are not ordered: the closing of this resolution
  // may have come (and been acked) before the resolution itself. An endpoint
  // already closed is never used; the request made at the closing (or the next
  // put) brings the current one.
  if (epoch_ <= closed_epoch) {
    if (deviceChannelDebug())
      CmiPrintf("[CHAN-STALE %d] channel %llx: resolution %llu arrived after its closing was acked (closed %llu); dropped\n",
                CkMyPe(), (unsigned long long)channel_id, (unsigned long long)epoch_, (unsigned long long)closed_epoch);
    return;
  }
  // Two resolutions can be out to one copy at once (one answered from the
  // owner's waiting list at its arrival, one pushed right after), and messages
  // between PEs are not ordered: an older one arriving after a newer one must
  // not replace it -- the owner's credits are under the newest, and the copy
  // would wait for them under the old one forever (job 23024413, 23:09: the
  // sender at resolution 2, the owner releasing under 3). The same one twice
  // would double its credit.
  if (epoch != 0 && epoch_ <= epoch) {
    device_channel_resolved_older++;
    snprintf(device_channel_last_resolved_older, sizeof(device_channel_last_resolved_older),
             "channel %llx token %llx: resolution %llu after %llu (resolved %d, closed %llu)",
             (unsigned long long)channel_id, (unsigned long long)my_token, (unsigned long long)epoch_,
             (unsigned long long)epoch, (int)resolved, (unsigned long long)closed_epoch);
    { static thread_local int said = 0; if (said++ < 8 || deviceChannelDebug()) CmiPrintf("[CHAN-STALE %d] %s; ignored\n", CkMyPe(), device_channel_last_resolved_older); }
    return;
  }
  // The buffer's one credit, reconciled by sequence number rather than reset. The reply's free
  // flag describes the buffer as of the receiver's last take (taken_seq_), and the receiver's
  // DONEs, credits and the reply itself reach this copy in any order (two nodes, 2026-10-22: a
  // DONE sent before the reply delivered after it, a credit sent after both delivered first;
  // leanmd stalled at the step after a migration in half its runs). So: a direct put the
  // receiver had not taken at the reply will occupy the buffer (-1; its credit comes under this
  // resolution); a staged put it read after the reply does the same when its DONE is seen, here
  // if the DONE overtook the reply (dones_seen) or in onStagedDone otherwise; a release under
  // this resolution that overtook it (an early credit) is +1 when it frees a put taken after
  // the reply, or the put held at it. Everything up to taken_seq_ is the free flag's.
  acct_seq = taken_seq_;
  int c = free ? 1 : 0;
  occupant_credit_due = !free;
  {
    std::vector<uint64_t> keep;
    for (uint64_t sq : direct_issued) if (sq > acct_seq) { keep.push_back(sq); c--; }
    direct_issued.swap(keep);
  }
  for (uint64_t sq : dones_seen) if (sq > acct_seq) c--;
  dones_seen.clear();
  int early_credit = 0;
  if (my_token != 0) {
    DeviceChannelEarlyCredits& early = *CkpvAccess(device_channel_early_credits);
    auto it = early.find(my_token);
    if (it != early.end()) {
      std::vector<DeviceChannelEarlyCredit> keep;
      for (const DeviceChannelEarlyCredit& ec : it->second) {
        if (ec.epoch == epoch_ && (ec.seq > acct_seq || (ec.seq == acct_seq && !free))) {
          c++; early_credit++;
          if (ec.seq == acct_seq) occupant_credit_due = false;
        } else if (ec.epoch > epoch_) keep.push_back(ec);   // of a resolution still to come
      }
      if (keep.empty()) early.erase(it); else it->second.swap(keep);
    }
  }
  note(4, taken_seq_, epoch_, owner_pe, credit_slot_, c);
  MIGC("resolved-ch", channel_id, gen_, owner_pe);
  if (deviceChannelDebug() || chanSignalDebug())
    CmiPrintf("[CHAN-RESOLVED %d] channel %llx token %llx: resolution %llu applied (held %llu resolved %d closed %llu, free %d taken_seq %llu, early %d, direct puts in flight %zu, credits %d)\n",
              CkMyPe(), (unsigned long long)channel_id, (unsigned long long)my_token, (unsigned long long)epoch_,
              (unsigned long long)epoch, (int)resolved, (unsigned long long)closed_epoch, (int)free, (unsigned long long)acct_seq,
              early_credit, direct_issued.size(), c);
  if (closing_at > 0.0) { ckMigStatChannel(0, CkWallTimer() - closing_at); closing_at = 0.0; }
  epoch = epoch_;
  dst = ep;
  generation = gen_;
  resolved = true;
  resolving = false;
  credits = c;
  credit_shm_pe = credit_pe;
  credit_shm_slot = credit_slot_;
  credits_seen = 0;
  signal_slot = (signal_hdr_ != NULL) ? signal_slot_ : -1;
  if (signal_hdr_ != NULL) signal_hdr = *signal_hdr_;
  src_ep = CkDevicePersistent();   // the transport to the new home may differ
  // Another host: announce and let the receiver read (an RDMA read beat the
  // write at every size here); otherwise put and wait for credits.
  // The cross-host tier announces and lets the receiver read: the receiver's
  // get completes locally, so nothing follows the data on its path, and the
  // read beat the write at every size on two nodes (12.4 vs 19.0 us at 1 KB,
  // 253 vs 268 us at 4 MB). Its 2026-10-18 hangs were the cross-node message
  // reorder, fixed by the early credits; it completed on the fixed runtime.
  pull_mode = false;   // the write across hosts; its note is the write's immediate data
  (void)dst.pe;
  if (pending.valid && staged_outstanding == 0 && (pull_mode || credits > 0)) {
    Pending p = pending;
    pending.valid = false;
    if (!pull_mode) { credits--; uncredited++; }
    issue(p.src, p.bytes, p.stream, p.done, p.tag);
  } else if (pending.valid) {
    // Parked before this resolution (put() lists a copy for the per-PE
    // shared-memory credit poll only when it parks resolved): the credit comes
    // through the word, and the owner's release can precede this reply's
    // application (2026-10-22 13:53, two nodes: the owner replied busy at
    // 29.077, released at 29.079, the reply applied at 29.116, and nobody read
    // the word again -- a stall at step 151). List it, and read the word now.
    awaitShmCredit();
  }
}

// A put is queued behind a credit that comes through shared memory: list this
// copy for the per-PE poll and read the word once now.
void CkDeviceChannel::awaitShmCredit() {
  if (!pending.valid || !resolved || pull_mode || credit_shm_slot < 0) return;
  if (!shm_waiting) {
    shm_waiting = true;
    CkpvAccess(device_channel_shm_waiters)->push_back(this);
  }
  pollShmCredit();
}

void CkDeviceChannel::onClosing(uint64_t gen_, int owner_pe, uint64_t epoch_) {
  deviceChannelProgress();
  (void)gen_;
  if (deviceChannelDebug() || chanSignalDebug())
    CmiPrintf("[CHAN-CLOSE %d] channel %llx: closing of resolution %llu received (current %llu resolved %d in_flight %d pending %d)\n",
              CkMyPe(), (unsigned long long)channel_id, (unsigned long long)epoch_, (unsigned long long)epoch,
              (int)resolved, in_flight, (int)pending.valid);
  if (epoch_ > closed_epoch) closed_epoch = epoch_;
  close_ack_pe = owner_pe;
  note(7, send_seq, epoch_, owner_pe, -1, in_flight);
  MIGC("closing-rx", channel_id, gen_, owner_pe);
  if (ckMigStatsOn() && closing_at == 0.0 && !(resolved && epoch_ < epoch)) closing_at = CkWallTimer();
  if (resolved && epoch_ < epoch) {
    // A closing of a resolution this copy already left behind: nothing of it is
    // in flight, the owner only needs the ack (it closes the current one next).
    sendCloseAck();
    return;
  }
  resolved = false;
  resolving = false;
  uncredited = 0;   // the next resolution's free flag accounts for the buffer
  credit_shm_slot = -1; credit_shm_pe = -1;
  if (shm_waiting) { deviceChannelShmUnlist(this); shm_waiting = false; }
  signal_slot = -1;
  if (in_flight == 0) sendCloseAck();
  else close_ack_due = true;
  // A put parked here for a credit goes to the receiver's new home as a staged
  // announce now, instead of waiting for the next resolution.
  if (pending.valid) {
    Pending p = pending;
    pending.valid = false;
    issueStaged(p.src, p.bytes, p.stream, p.done, p.tag);
  }
}

void CkDeviceChannel::sendCloseAck() {
  close_ack_due = false;
  note(8, last_direct_seq, epoch, close_ack_pe, -1, in_flight);
  auto* m = deviceChannelMsg<DeviceChannelCloseAck>(device_channel_closeack_handler);
  m->channel = channel_id;
  m->token = my_token;
  m->pe = CkMyPe();
  // The last put WRITTEN into the buffer: the owner carries its bytes in the
  // pup if their event is still queued. A staged put was announced, not
  // written; reporting it would mark the buffer busy for nothing.
  m->last_seq = last_direct_seq;
  m->last_bytes = last_put_bytes;
  CmiSyncSendAndFreeUrgent(close_ack_pe, sizeof(DeviceChannelCloseAck), (char*)m);
}

void CkDeviceChannel::onSrcDone(uint64_t seq) {
  deviceChannelProgress();
  (void)seq;
  if (in_flight > 0) in_flight--;
  note(9, seq, epoch, -1, -1, in_flight);
  if (sender_rec) sender_rec->noteDeviceSendDone();
  if (close_ack_due && in_flight == 0) sendCloseAck();
}

// The receiver has read put `seq`: the source may be reused.
void CkDeviceChannel::onDone(uint64_t seq) {
  CkCallback cb(CkCallback::ignore);
  auto it = done_cbs.find(seq);
  if (it != done_cbs.end()) { cb = it->second; done_cbs.erase(it); }
  onSrcDone(seq);
  if (cb.type != CkCallback::ignore) cb.send(NULL);
}

// An announced put: read it now if the buffer is free, else after release().
void CkDeviceChannel::onAnnounce(const CkDeviceChannelEvent& ev, const CkDevicePersistent& src) {
  deviceChannelProgress();
  Queued q; q.ev = ev; q.src = src;
  pulls.push_back(q);
  if (is_owner && !busy && !pulling) startPull();
}

void CkDeviceChannel::startPull() {
  if (!is_owner || busy || pulling || pulls.empty()) return;
  // Puts are read in the order they were made: a direct put whose note is
  // still on its way is taken and released before a staged put that followed
  // it is read over the buffer.
  if (pulls.front().ev.seq != taken_seq + 1) return;
  Queued q = pulls.front();
  pulls.erase(pulls.begin());
  pulling = true;
  pull_ev = q.ev;
  pull_src = q.src;
  pull_src.cnt = q.ev.bytes;
  if (pull_src.cnt > cap) {
    CkAbort("CkDeviceChannel: announced put of %zu bytes into a channel of %zu\n", pull_src.cnt, cap);
  }
  // The read lands in this buffer outside every stream: the element must not
  // leave with it in flight.
  if (owner_known) {
    CkLocMgr* mgr = (CkLocMgr*)CkLocalBranch(owner_mgr);
    CkLocRec* rec = mgr ? mgr->recForDeviceWork(owner_id) : NULL;
    if (rec) rec->noteDeviceSendPosted();
  }
  if (pull_src.cnt == 0) { onPulled(); return; }   // a zero-byte put: the note alone
  dst.cb = CkCallback(deviceChannelPulledFn, (void*)new DeviceChannelPulled{channel_id});
  dst.cb_msg = nullptr;
  dst.get(pull_src);
}

// The read completed: the application's DATA event, and DONE to the sender.
void CkDeviceChannel::onPulled() {
  deviceChannelProgress();
  pulling = false;
  // The buffer holds unconsumed data from now until release(): the application
  // may take the DATA event much later (an SDAG waits for its tag), and the
  // next announce must not be read over it.
  busy = true;
  // Until event() takes it, these bytes are in the buffer and nowhere else: a
  // pack in between must carry them, and unseenBytes() is how the owner learns
  // that. Without this only a direct put acked at closing counted, and an
  // element that packed between this read and its DATA note left the bytes
  // behind (moe, async LB, cross-host announces: 2 experts read a stale slab
  // at step 13, job 23202493).
  landed_unseen = pull_ev.bytes;
  landed_seq = pull_ev.seq;
  if (owner_known) {
    CkLocMgr* mgr = (CkLocMgr*)CkLocalBranch(owner_mgr);
    CkLocRec* rec = mgr ? mgr->recForDeviceWork(owner_id) : NULL;
    if (rec) rec->noteDeviceSendDone();
  }
  if (deviceChannelDebug())
    CmiPrintf("[CHAN-PULLED %d] channel %llx: staged put %llu read from pe %d token %llx; DONE sent (epoch %llu taken_seq %llu)\n",
              CkMyPe(), (unsigned long long)channel_id, (unsigned long long)pull_ev.seq, pull_ev.sender_pe,
              (unsigned long long)pull_ev.sender_token, (unsigned long long)epoch, (unsigned long long)taken_seq);
  auto* d = deviceChannelMsg<DeviceChannelDone>(device_channel_done_handler);
  d->token = pull_ev.sender_token;
  d->channel = channel_id;
  d->seq = pull_ev.seq;
  CmiSyncSendAndFreeUrgent(pull_ev.sender_pe, sizeof(DeviceChannelDone), (char*)d);
  CkDeviceChannelEvent ev = pull_ev;
  ev.kind = CkDeviceChannelEvent::DATA;
  CkDataMsg* note = CkDataMsg::buildNew((int)sizeof(ev), &ev);
  CkSetRefNum(note, (CMK_REFNUM_TYPE)ev.tag);
  arrived.send(note);
}

void CkDeviceChannel::issue(const void* src, size_t bytes, hapiStream_t stream,
                            const CkCallback& src_done, uint64_t tag) {
  const uint64_t seq = ++send_seq;
  note(0, seq, epoch, dst.pe, signal_slot, credits);
  direct_issued.push_back(seq);
  last_put_bytes = bytes;
  last_direct_seq = seq;
  if (shm_waiting && !pending.valid) { deviceChannelShmUnlist(this); shm_waiting = false; }
  if (my_token == 0) {
    my_token = deviceChannelToken();
    (*CkpvAccess(device_channel_senders))[my_token] = this;
  }
  CkDeviceChannelEvent ev;
  memset(&ev, 0, sizeof(ev));
  ev.kind = CkDeviceChannelEvent::DATA;
  ev.channel = channel_id;
  ev.generation = generation;
  ev.seq = seq;
  ev.bytes = bytes;
  ev.sender_pe = CkMyPe();
  ev.sender_token = my_token;
  ev.tag = tag;
  // The receiver's notification, sent by the transfer's completion; its
  // reference number is the tag, for SDAG matching on the receiver.
  dst.cb = arrived;
  CkDataMsg* note = CkDataMsg::buildNew((int)sizeof(ev), &ev);
  CkSetRefNum(note, (CMK_REFNUM_TYPE)tag);
  // Nothing to move: the notification goes now and the source is done. The
  // receiver still releases, as for any put.
  if (bytes == 0) {
    in_flight++;
    arrived.send(note);
    deviceChannelSrcDoneFn(new DeviceChannelSrcDone{my_token, seq, src_done}, NULL);
    return;
  }
  if (pull_mode) {
    // The receiver reads: register the source once per buffer, keep the user's
    // source-done until the receiver's DONE, and announce when the producing
    // stream is complete. Several puts may be outstanding; the receiver queues
    // them and reads in order as its buffer frees.
    if (src_ep.ptr != src || src_ep.cnt != bytes) {
      src_ep = CkDevicePersistent(src, bytes, stream);
      src_ep.open();
    }
    done_cbs[seq] = src_done;
    in_flight++;
    ev.kind = CkDeviceChannelEvent::ANNOUNCE;
    PUP::sizer sz;
    sz | src_ep;
    std::vector<char> buf(sizeof(ev) + sz.size());
    memcpy(buf.data(), &ev, sizeof(ev));
    PUP::toMem tm(buf.data() + sizeof(ev));
    tm | src_ep;
    delete note;
    CkDataMsg* ann = CkDataMsg::buildNew((int)buf.size(), buf.data());
    hapiAddCallback(stream, CkCallback(deviceChannelAnnounceReadyFn,
                                       (void*)new DeviceChannelAnnounce{control, ann}));
    return;
  }
  if (chanShmPair(dst.pe)) {
    // Same host, other process: the completion writes the note into the owner
    // PE's ring instead of sending it.
    delete note;
    dst.cb = CkCallback(deviceChannelShmNoteFn, (void*)new DeviceChannelShmNote{chanShmIndexOf(dst.pe), ev, arrived});
    dst.cb_msg = nullptr;
  } else {
    dst.set_msg(note);
  }
  dst.cnt = bytes;   // the transfer; the registration spans the buffer
  // The source endpoint: kept across puts of the same buffer, so a cross-host
  // sender registers its source once.
  if (src_ep.ptr != src || src_ep.cnt != bytes) {
    const double t0 = zcStatsOnFast() ? CkWallTimer() : 0.0;
    src_ep = CkDevicePersistent(src, bytes, stream);
    if (CmiNumPhysicalNodes() > 1 &&
        findTransferModeDevice(CkMyPe(), dst.pe) == CkNcpyModeDevice::RDMA) {
      src_ep.open();
    }
    if (t0 != 0.0) { zc_mode_stats.chan_open_n.fetch_add(1, std::memory_order_relaxed); zc_mode_stats.chan_open_us.fetch_add((long)((CkWallTimer() - t0) * 1e6), std::memory_order_relaxed); }
  }
  src_ep.hapi_stream = stream;
  src_ep.cb = CkCallback(deviceChannelSrcDoneFn, (void*)new DeviceChannelSrcDone{my_token, seq, src_done});
  src_ep.cb_msg = nullptr;
  in_flight++;
  { static thread_local bool said = false;
    if (!said && CmiPhysicalNodeID(dst.pe) != CmiPhysicalNodeID(CkMyPe())) { said = true;
      CmiPrintf("[CHAN-INIT %d] first cross-host put: signal_slot=%d tag=%llu bytes=%zu hosts=%d tier=%d (2=rdma)\n", CkMyPe(),
                signal_slot, (unsigned long long)tag, bytes, CmiNumPhysicalNodes(), (int)findTransferModeDevice(CkMyPe(), dst.pe)); } }
  // Across hosts the note rides on the data write as immediate data (LCI
  // emulates it on cxi as the write followed by a zero-length message carrying
  // the immediate; the header with seq/bytes/tag goes ahead by a raw put into
  // the owner process's registered header region). Validated 2026-10-19 on
  // chantest, leanmd and jacobi3d; the constant exists to retest the message
  // note (false) against it.
  constexpr bool CHAN_IMMEDIATE_NOTES = true;
  if (CHAN_IMMEDIATE_NOTES && signal_slot > 0 && tag < 65536 && bytes < (size_t)UINT32_MAX &&
      CmiNumPhysicalNodes() > 1 && findTransferModeDevice(CkMyPe(), dst.pe) == CkNcpyModeDevice::RDMA) {
    // The note rides on the data write: no message after the data.
    dst.cb = CkCallback(CkCallback::ignore);
    dst.cb_msg = nullptr;
    device_channel_signal_req = new DeviceChannelSignalReq{signal_slot, signal_hdr, seq, (uint32_t)bytes, (uint32_t)tag};
  }
  // Stream tiers (same process, same host): into this PE's open batch on the
  // producing stream (deviceChannelBatchAdd): one batched copy, one flag, and
  // every put's note and source-done from its completion. Across hosts the
  // one-sided write keeps its own path; its completion handler fires both.
  const CkNcpyModeDevice tier = findTransferModeDevice(CkMyPe(), dst.pe);
  if (tier != CkNcpyModeDevice::RDMA && dst.cb.type != CkCallback::ignore) {
    void* dptr = (tier == CkNcpyModeDevice::MEMCPY) ? (void*)dst.ptr : persistentImport(dst);
    delete (DeviceChannelSrcDone*)src_ep.cb.d.cfn.param;
    src_ep.cb = CkCallback(CkCallback::ignore);
    deviceChannelBatchAdd(stream, dptr, src, bytes, DeviceChannelBatchEntry{my_token, seq, src_done, dst.cb, dst.cb_msg});
    dst.cb = CkCallback(CkCallback::ignore);
    dst.cb_msg = nullptr;
    return;
  }
  {
    const double t0 = zcStatsOnFast() ? CkWallTimer() : 0.0;
    src_ep.put(dst);
    if (t0 != 0.0) { zc_mode_stats.chan_rput_n.fetch_add(1, std::memory_order_relaxed); zc_mode_stats.chan_rput_us.fetch_add((long)((CkWallTimer() - t0) * 1e6), std::memory_order_relaxed); }
  }
}

// A put while the receiver's channel is closed (its element is migrating), or
// while earlier such puts are still unread: the bytes are copied to a staging
// buffer on the producing stream, the application's source is released as soon
// as that copy has read it (src_done), and the staging buffer is announced to
// the receiving element -- an element message, which the location manager
// parks and forwards, so it is waiting at the new home when the element
// reopens and reads it in put order (startPull). Nothing holds the sender; the
// sending element is charged one outstanding device send until the receiver's
// DONE frees the staging buffer (device_channel_done_bridge), so it does not
// move with one unread. Balance: the receiver's read takes the buffer as a
// direct put would and its release credits, so this side debits one credit at
// the DONE (onStagedDone) and the two pair off.
struct DeviceChannelStagedCopied { CkCallback user; };
static void deviceChannelStagedCopiedFn(void* param, void* msg) {
  auto* d = (DeviceChannelStagedCopied*)param;
  if (d->user.type != CkCallback::ignore) d->user.send(msg);
  delete d;
}
void* CkDeviceMallocNoGrow(size_t size);

void CkDeviceChannel::issueStaged(const void* src, size_t bytes, hapiStream_t stream,
                                  const CkCallback& src_done, uint64_t tag) {
  note(2, send_seq, epoch, dst.pe, -1, staged_outstanding);
  if (my_token == 0) {
    my_token = deviceChannelToken();
    (*CkpvAccess(device_channel_senders))[my_token] = this;
  }
  void* stage = nullptr;
  if (bytes > 0) {
    stage = CkDeviceMallocNoGrow(bytes);
    if (stage == nullptr) {
      // No pool memory for the copy: wait for the new home as before. The
      // parked put is issued directly once resolved and credited, and only
      // behind the staged ones (onStagedDone).
      if (pending.valid) {
        CkAbort("CkDeviceChannel::put: a put is already queued on channel %llx while its receiver migrates "
                "and the device pool has no %zu bytes to stage another\n", (unsigned long long)channel_id, bytes);
      }
      pending.src = src; pending.bytes = bytes; pending.stream = stream; pending.done = src_done; pending.tag = tag;
      pending.valid = true;
      if (!resolved && !resolving) requestResolve();
      return;
    }
  }
  const uint64_t seq = ++send_seq;   // in the one sequence with direct puts: the receiver reads in order
  CkDevicePersistent sep;
  if (bytes > 0) {
    hapiMemcpyAsync(stage, src, bytes, hapiMemcpyDeviceToDevice, stream);
    sep = CkDevicePersistent(stage, bytes, stream);
    sep.open();   // the receiver reads it: same process by copy, same host through the import cache, else by RDMA
  }
  // The application's source is free once the copy has read it.
  hapiAddCallback(stream, CkCallback(deviceChannelStagedCopiedFn, (void*)new DeviceChannelStagedCopied{src_done}));
  CkDeviceChannelEvent ev;
  memset(&ev, 0, sizeof(ev));
  ev.kind = CkDeviceChannelEvent::ANNOUNCE;
  ev.channel = channel_id;
  ev.generation = generation;
  ev.seq = seq;
  ev.bytes = bytes;
  ev.sender_pe = CkMyPe();
  ev.sender_token = my_token;
  ev.tag = tag;
  PUP::sizer sz;
  sz | sep;
  std::vector<char> buf(sizeof(ev) + sz.size());
  memcpy(buf.data(), &ev, sizeof(ev));
  PUP::toMem tm(buf.data() + sizeof(ev));
  tm | sep;
  CkDataMsg* ann = CkDataMsg::buildNew((int)buf.size(), buf.data());
  (*CkpvAccess(device_channel_staged))[std::make_pair(my_token, seq)] =
      DeviceChannelStaged{stage, holder_mgr, holder_elem, holder_known};
  staged_outstanding++;
  if (zcStatsOnFast()) zc_mode_stats.put_staged.fetch_add(1, std::memory_order_relaxed);
  if (deviceChannelDebug())
    CmiPrintf("[CHAN-STAGED %d] channel %llx token %llx: put %llu (%zu bytes, tag %llu) staged at %p; %d outstanding\n",
              CkMyPe(), (unsigned long long)channel_id, (unsigned long long)my_token, (unsigned long long)seq, bytes,
              (unsigned long long)tag, stage, staged_outstanding);
  // Announced once the copy is complete on the stream.
  hapiAddCallback(stream, CkCallback(deviceChannelAnnounceReadyFn, (void*)new DeviceChannelAnnounce{control, ann}));
  // The new home answers after reopen (and pushes to the last sender anyway).
  if (!resolved && !resolving) requestResolve();
}

// The receiver read a staged put (its DONE; the staging buffer is already
// freed by the handler): the read took the buffer as a direct put would, and
// the receiver's release of it will credit, so one credit is debited here.
void CkDeviceChannel::onStagedDone(uint64_t seq) {
  deviceChannelProgress();
  note(19, seq, epoch, -1, -1, staged_outstanding);
  if (deviceChannelDebug() || chanSignalDebug())
    CmiPrintf("[CHAN-DONE-RX %d] channel %llx token %llx: staged put %llu read by the owner: credits %d staged %d pending %d resolved %d epoch %llu\n",
              CkMyPe(), (unsigned long long)channel_id, (unsigned long long)my_token, (unsigned long long)seq, credits,
              staged_outstanding, (int)pending.valid, (int)resolved, (unsigned long long)epoch);
  if (staged_outstanding > 0) staged_outstanding--;
  // The read took the buffer as a direct put would, until the receiver releases it (a credit).
  // Unresolved: the next reply's free flag speaks for every put up to its taken_seq, so the
  // DONE is kept and reconciled there. Resolved: a put read after the reply is debited here;
  // one read before it was counted by its free flag.
  if (!resolved) dones_seen.push_back(seq);
  else if (seq > acct_seq) credits--;
  if (staged_outstanding == 0 && pending.valid && resolved && credits > 0) {
    Pending p = pending;
    pending.valid = false;
    credits--;
    uncredited++;
    issue(p.src, p.bytes, p.stream, p.done, p.tag);
  } else if (staged_outstanding == 0 && pending.valid) {
    awaitShmCredit();   // the parked put now waits for a credit that may come through shared memory
  }
}

// A credit under the current resolution frees one put: the one held at the reply first (not
// in direct_issued, which the reply's reconciliation pruned), then the oldest direct put. seq
// names it when the credit came as a message; 0 (shared memory) takes the put order.
void CkDeviceChannel::creditedPut(uint64_t seq) {
  if (occupant_credit_due && (seq == 0 || seq <= acct_seq)) { occupant_credit_due = false; return; }
  occupant_credit_due = false;
  if (direct_issued.empty()) return;
  if (seq != 0) {
    for (size_t i = 0; i < direct_issued.size(); i++)
      if (direct_issued[i] == seq) { direct_issued.erase(direct_issued.begin() + i); return; }
    if (seq < direct_issued.front()) return;   // a staged put's release: no direct put freed
  }
  direct_issued.erase(direct_issued.begin());
}

void CkDeviceChannel::onCredit(uint64_t seq, uint64_t epoch_) {
  deviceChannelProgress();
  note(5, seq, epoch_, -1, -1, credits);
  if (deviceChannelDebug() || chanSignalDebug())
    CmiPrintf("[CHAN-CREDIT-RX %d] channel %llx token %llx: credit for put %llu epoch %llu arrives: resolved %d resolving %d epoch %llu credits %d staged %d pending %d uncredited %d\n",
              CkMyPe(), (unsigned long long)channel_id, (unsigned long long)my_token, (unsigned long long)seq, (unsigned long long)epoch_,
              (int)resolved, (int)resolving, (unsigned long long)epoch, credits, staged_outstanding, (int)pending.valid, uncredited);
  if (pull_mode && resolved && epoch_ == epoch) return;   // the receiver reads when it is ready
  // A credit of another resolution (the owner released after it closed this
  // copy, or before it answered the current request) is already in the free
  // flag the next Resolved carries, or will be; counting it would allow two
  // puts into one buffer.
  if (epoch_ > epoch && my_token != 0) {
    // Ahead of its resolution (see device_channel_early_credits).
    (*CkpvAccess(device_channel_early_credits))[my_token].push_back(DeviceChannelEarlyCredit{epoch_, seq});
    device_channel_credits_early++;
    note(20, seq, epoch_, -1, -1, credits);
    snprintf(device_channel_last_early, sizeof(device_channel_last_early),
             "channel %llx put %llu of resolution %llu while current %llu resolved %d resolving %d pending %d",
             (unsigned long long)channel_id, (unsigned long long)seq, (unsigned long long)epoch_,
             (unsigned long long)epoch, (int)resolved, (int)resolving, (int)pending.valid);
    return;
  }
  if (!resolved || epoch_ != epoch) {
    device_channel_credits_refused++;
    note(21, seq, epoch_, -1, -1, credits);
    snprintf(device_channel_last_refused, sizeof(device_channel_last_refused),
             "channel %llx put %llu of resolution %llu while current %llu resolved %d resolving %d pending %d credits %d",
             (unsigned long long)channel_id, (unsigned long long)seq, (unsigned long long)epoch_,
             (unsigned long long)epoch, (int)resolved, (int)resolving, (int)pending.valid, credits);
    if (deviceChannelDebug() || chanSignalDebug())
      CmiPrintf("[CHAN-STALE %d] channel %llx: credit for put %llu of resolution %llu dropped (current %llu, resolved %d)\n",
                CkMyPe(), (unsigned long long)channel_id, (unsigned long long)seq, (unsigned long long)epoch_,
                (unsigned long long)epoch, (int)resolved);
    if (pending.valid && !resolved && !resolving) requestResolve();
    return;
  }
  credits++;
  if (uncredited > 0) uncredited--;
  creditedPut(seq);
  if (staged_outstanding > 0) return;   // the parked put follows the staged ones (onStagedDone)
  if (!pending.valid) return;
  Pending p = pending;
  pending.valid = false;
  credits--;
  uncredited++;
  issue(p.src, p.bytes, p.stream, p.done, p.tag);
}

void CkDeviceChannel::pup(PUP::er& p) {
  p|dst;
  p|arrived;
  p|control;
  p|channel_id;
  p|generation;
  p|cap;
  p|owner_id;
  p|owner_known;
  p|owner_mgr;
  p|busy;
  p|epoch;
  p|sender_known;
  p|sender_pe;
  p|sender_token;
  bool was_owner = is_owner;
  p|was_owner;
  // A receiver may move between event() and release(): the taken event goes
  // with it, so the release at the new home finds it (the credit then goes
  // nowhere, and the sender learns the buffer is free when it re-resolves).
  p|have_last;
  p((char*)&last, sizeof(last));
  p|send_seq;
  p|taken_seq;
  p|landed_unseen;
  p|landed_seq;
  p|last_put_bytes;
  p|last_direct_seq;
  p|departed_signal_slot;
  p|departed_signal_pe;
  // Announced puts not yet read travel with the element; the sources stay
  // valid until DONE, which the new home sends after reading them.
  int np = (int)pulls.size();
  p|np;
  if (p.isUnpacking()) pulls.resize(np);
  for (int i = 0; i < np; i++) { p((char*)&pulls[i].ev, sizeof(pulls[i].ev)); p|pulls[i].src; }
  int nw = (int)waiting.size();
  p|nw;
  if (p.isUnpacking()) waiting.resize(nw);
  for (int i = 0; i < nw; i++) { p|waiting[i].pe; p|waiting[i].token; }
  if (p.isUnpacking()) {
    // A copy, whichever side it lands on, starts unresolved and unregistered:
    // the receiver re-opens it (reopen) and senders re-resolve.
    is_owner = false;
    (void)was_owner;   // the owning element's pup says whether this copy arrived with it (arriving())
    // The last sender stays known (pupped above) so that reopen() can hand it
    // the new resolution without waiting for it to ask; it was closed before
    // the move.
    sender_closed = sender_known;
    closing_gen = 0;
    closing_sent = false;
    resolved_during_closing = false;
    closing_rec = NULL;
    resolved = false;
    resolving = false;
    credits = 0;
    uncredited = 0;
    pull_mode = false;
    done_cbs.clear();
    sender_pulls = false;
    pulling = false;
    closed_epoch = 0;   // a new token: nothing stale can reach this copy
    in_flight = 0;
    staged_outstanding = 0;   // a holder does not move with a staged put unread (it is an outstanding send)
    acct_seq = 0; occupant_credit_due = false; direct_issued.clear(); dones_seen.clear();
    close_ack_due = false;
    close_ack_pe = -1;
    my_token = 0;
    credit_shm_slot = -1; credit_shm_pe = -1; credits_seen = 0; shm_waiting = false;
    credit_slot = -1; credit_slot_pe = -1;
    signal_slot = -1; owner_signal_slot = -1;
    hist_n = 0; note(18, send_seq, epoch, CkMyPe());
    sender_rec = NULL;
    holder_elem = 0;
    holder_known = false;
    pending.valid = false;
    src_ep = CkDevicePersistent();
  }
}

// ArrayElement::ckDeviceChannelControl: a sender's resolve for one of this
// element's channels, delivered where the element lives.
void CkDeviceChannelControl(void* msg) {
  CkDataMsg* m = (CkDataMsg*)msg;
  if (m->getLength() < (int)sizeof(CkDeviceChannelEvent)) {
    CkAbort("CkDeviceChannelControl: not a channel event (%d bytes)\n", m->getLength());
  }
  CkDeviceChannelEvent ev;
  memcpy(&ev, m->getData(), sizeof(ev));
  CkDeviceChannel* c = deviceChannelOwner(ev.channel);
  if (c == NULL) {
    DeviceChannelTable& tab = *CkpvAccess(device_channel_owners);
    CmiPrintf("[CHAN-CONTROL %d] channel %llx kind=%d from pe %d token %llx; owners here: %zu entries:",
              CkMyPe(), (unsigned long long)ev.channel, ev.kind, ev.sender_pe,
              (unsigned long long)ev.sender_token, tab.size());
    for (auto& kv : tab) CmiPrintf(" %llx(elem %llu)", (unsigned long long)kv.first, (unsigned long long)kv.second->ownerElement());
    CmiPrintf("\n");
    CkAbort("CkDeviceChannel: a control event for channel %llx reached PE %d, where its element "
            "holds no channel of that id; an element's pup must carry its channels and reopen() them on the "
            "new buffers when it unpacks\n", (unsigned long long)ev.channel, CkMyPe());
  }
  c->event(m);
  delete m;
}

void CkRdmaDeviceChannelsBeforeMigrate(CkLocRec* rec) {
  DeviceChannelTable& tab = *CkpvAccess(device_channel_owners);
  if (tab.empty()) return;
  const CmiUInt8 id = rec->getID();
  const int mgr = rec->getLocMgr()->ckGetGroupID().idx;
  for (auto& kv : tab)
    if (kv.second->ownedBy(mgr, id)) kv.second->closingForMigration(rec);
}

// What an element's stand-down waits for, counted at its first deferral (CHARM_LB_MIGSTATS): its own puts
// in flight or staged (sender copies it holds), its puts parked for a credit, and its owned channels'
// closings not yet acked.
void CkRdmaDeviceChannelsStandDownWhy(CkLocRec* rec, int* own_inflight, int* own_pending, int* acks) {
  *own_inflight = *own_pending = *acks = 0;
  for (CkDeviceChannel* c : deviceChannelElemList(rec)) {
    if (c->is_owner) { if (c->closing_sent && !c->sender_closed) (*acks)++; }
    else { *own_inflight += c->in_flight + c->staged_outstanding; if (c->pending.valid) (*own_pending)++; }
  }
}

void CkRdmaDeviceChannelsLeaving(CkLocRec* rec) {
  std::vector<CkDeviceChannel*> cs = deviceChannelElemList(rec);
  if (deviceChannelDebug())
    CmiPrintf("[CHAN-HANDOFF %d] leave id=%llu channels=%zu\n", CkMyPe(),
              (unsigned long long)rec->getID(), cs.size());
  for (CkDeviceChannel* c : cs) c->leaveForHandoff();
}

void CkRdmaDeviceChannelsArrived(CkLocRec* rec) {
  std::vector<CkDeviceChannel*> cs = deviceChannelElemList(rec);
  if (deviceChannelDebug())
    CmiPrintf("[CHAN-HANDOFF %d] arrive id=%llu channels=%zu\n", CkMyPe(),
              (unsigned long long)rec->getID(), cs.size());
  for (CkDeviceChannel* c : cs) c->arriveFromHandoff();
}

void CkRdmaDeviceChannelsPack(CkLocRec* rec) {
  DeviceChannelTable& tab = *CkpvAccess(device_channel_owners);
  if (tab.empty()) return;
  const CmiUInt8 id = rec->getID();
  const int mgr = rec->getLocMgr()->ckGetGroupID().idx;
  std::vector<CkDeviceChannel*> owned;
  for (auto& kv : tab)
    if (kv.second->ownedBy(mgr, id)) owned.push_back(kv.second);
  for (CkDeviceChannel* c : owned) c->packForMigration();
}

/****************************** Recv Entry Method API ******************************/

// CmiNodeRankLocal / CmiMyNodeRankLocal: one definition, in gpumanager.h,
// shared with hapi_impl.cpp (both sides of a transfer index by them).

// Debug/validation knobs on the per-message IPC path, resolved once.
//
// getenv is a linear strncmp walk of the environment and a miss -- which is the
// normal case for these -- scans all of it: measured at 770ns with 93 variables
// and 950ns with 243, the sort of environment a batch scheduler hands out. The
// staged path reads this several times per message, so leaving it uncached
// spent microseconds per message doing nothing. Read it through this accessor;
// do not call getenv on this path.
static inline bool zcValidateOn() {
  static const bool on = (getenv("CHARM_ZC_VALIDATE") != nullptr);
  return on;
}

// Per-process tally of how device zerocopy receives actually resolved, enabled
// with CHARM_ZC_STATS=1. Says whether a placement decision (e.g. a
// communication-aware load balancer) turned cross-process IPC transfers into
// same-process device-to-device copies, which is the locality question that
// raw timings alone cannot answer. Counting is a relaxed atomic increment on a
// path that already issues a CUDA call, and the env lookup is cached, so an
// instrumented run stays representative.
namespace {

// Sender-side companion: how often the destination could not be resolved at
// send time. An unresolved destination cannot take the cheap same-process path,
// so a burst of these right after a migration is what turns the first
// post-migration step into a slow one.
struct ZcDestStats {
  std::atomic<long> confirmed{0};
  std::atomic<long> unconfirmed{0};
  ~ZcDestStats() {
    const long c = confirmed.load(), u = unconfirmed.load();
    if (c + u == 0) return;
    fprintf(stderr, "[zc-dest] pid=%d confirmed=%ld unconfirmed=%ld (%.2f%% unresolved)\n",
            (int)getpid(), c, u, 100.0 * u / (c + u));
    fflush(stderr);
  }
};
ZcDestStats zc_dest_stats;

inline void zcDestCount(bool confirmed) {
  static const bool on = (getenv("CHARM_ZC_STATS") != nullptr);
  if (!on) return;
  if (confirmed) zc_dest_stats.confirmed.fetch_add(1, std::memory_order_relaxed);
  else zc_dest_stats.unconfirmed.fetch_add(1, std::memory_order_relaxed);
}

// Slot a mode falls into, matching the count buckets above.
inline int zcModeSlot(CkNcpyModeDevice mode) {
  if (mode == CkNcpyModeDevice::MEMCPY) return 0;
  if (mode == CkNcpyModeDevice::IPC) return 1;
  return 2;
}

inline bool zcStatsOn() {
  static const bool on = (getenv("CHARM_ZC_STATS") != nullptr);
  return on;
}

// Called once per receive, when its last op completes.
inline void zcStatsTime(int slot, size_t bytes, double seconds) {
  if (!zcStatsOn()) return;
  if (slot < 0 || slot > 2) return;
  zc_mode_stats.recv_n[slot].fetch_add(1, std::memory_order_relaxed);
  zc_mode_stats.recv_bytes[slot].fetch_add((long)bytes, std::memory_order_relaxed);
  zc_mode_stats.recv_us[slot].fetch_add((long)(seconds * 1e6),
                                        std::memory_order_relaxed);
}

inline void zcStatsCount(CkNcpyModeDevice mode) {
  static const bool on = (getenv("CHARM_ZC_STATS") != nullptr);
  if (!on) return;
  switch (mode) {
    case CkNcpyModeDevice::MEMCPY: zc_mode_stats.memcpy_n.fetch_add(1, std::memory_order_relaxed); break;
    case CkNcpyModeDevice::IPC:    zc_mode_stats.ipc_n.fetch_add(1, std::memory_order_relaxed); break;
    default:                       zc_mode_stats.other_n.fetch_add(1, std::memory_order_relaxed); break;
  }
}
}  // namespace

// File-scope bridge to the tally above, for the completion path that runs
// earlier in this translation unit.
void zcRecordRecvTime(int slot, size_t bytes, double seconds)
{
  zcStatsTime(slot, bytes, seconds);
}

// Per-PE ring of events used only to order same-process transfers.
//
// A wait enqueued on an event captures the most recent record at enqueue time,
// and the sender always records before the message is sent, so the receiver's
// wait cannot run ahead of the record. Re-recording an event that some earlier
// receiver is still waiting on is therefore harmless: that wait already
// captured the earlier record. The ring only needs to be large enough that
// re-use does not add false dependencies, not for correctness.
static const int kMemcpyEventRing = 1024;
static hapiEvent_t* ckDeviceMemcpyEventRing() {
  static const int ring_size = kMemcpyEventRing;
  static thread_local hapiEvent_t* ring = nullptr;
  static thread_local int next = 0;
  if (ring == nullptr) {
    ring = new hapiEvent_t[ring_size];
    for (int i = 0; i < ring_size; i++) {
      if (hapiEventCreateWithFlags(&ring[i], hapiEventDisableTiming) != hapiSuccess) {
        delete[] ring;
        ring = nullptr;
        return (hapiEvent_t*)nullptr;
      }
    }
  }
  return ring;
}

void* ckDeviceRecordMemcpyEvent(hapiStream_t stream) {
  hapiSubmitDrain();   // recorded directly and QUERIED by the receiver: it must follow the queued work
  static const int ring_size = kMemcpyEventRing;
  static thread_local int next = 0;
  hapiEvent_t* ring = ckDeviceMemcpyEventRing();
  if (ring == nullptr) return NULL;
  hapiEvent_t ev = ring[next];
  next = (next + 1) % ring_size;
  if (hapiEventRecord(ev, stream) != hapiSuccess) {
    // Expected here: the event can belong to another device (see the
    // cross-device notes below), and this is a try-and-fall-back. Clear the
    // sticky error so it is not left for the next cudaPeekAtLastError in
    // application code, which would abort on a failure it did not cause and
    // which was already handled.
    hapiGetLastError();
    return NULL;
  }
  return (void*)ev;
}

// Invoked after post entry method
// The staged/direct CUDA IPC receive, lifted out of CkRdmaDeviceIssueRgets so
// that a transfer deferred by the migration-mismatch correction below can run
// exactly the same code when the sender's retransmit metadata arrives. Pure
// extraction: the caller still owns completion (the hapiAddCallback at the end
// of the loop), and every value this needs is passed in rather than closed over.
// Where the bytes of a direct or staged IPC receive are, as an address this
// process can read: the peer's communication buffer through the mapping opened
// at startup, or the peer's own allocation through its (cached) IPC import.
static const void* deviceIpcResolveSource(CkDeviceBuffer& source, CkDeviceBuffer& dest,
                                          int srcPe, CkNcpyModeDevice mode)
{
  GPUManager& csv_gpu_manager = CsvAccess(gpu_manager);
  const bool sender_exported =
      (source.device_idx != -1 && csv_gpu_manager.use_shm);
  const bool sender_direct =
      (sender_exported && source.ipc_protocol == CmiIpcProtocol::DIRECT);
  (void)sender_exported;
      // sender_exported already guarantees device_idx is a real index; guard the
      // upper bound only, since a corrupted index would index the pool out of
      // bounds and surface later as an asynchronous illegal access, far from here.
      if ((size_t)source.device_idx >= csv_gpu_manager.hapi_ipc_device_infos.size()) {
        CkAbort("CkRdmaDeviceIssueRgets: receive on PE %d from PE %d carries an "
                "out-of-range IPC device index %d (pool size %zu).",
                CkMyPe(), srcPe, source.device_idx,
                csv_gpu_manager.hapi_ipc_device_infos.size());
      }
      // Inter-process using shared memory optimizations
      // Use optimiziations with POSIX shared memory
      hapi_ipc_device_info& device_info =
        csv_gpu_manager.hapi_ipc_device_infos[source.device_idx];


      // 0. Resolve where the bytes actually are.
      //
      // Staged: in the peer's communication buffer, whose mapping every process
      // opened once at startup.
      //
      // Direct: in the peer's own allocation, which has to be mapped now --
      // unless this delivery turned out to be same-process after all, because
      // the target migrated between the send and its arrival. Then the sender's
      // pointer is directly readable and must be used: the driver does not let
      // a process open a handle it exported itself.
      void* imported_base = NULL;
      const void* src_addr;
      if (!sender_direct) {
        src_addr = (const void*)((char*)device_info.buffer + source.comm_offset);
      } else if (mode == CkNcpyModeDevice::MEMCPY) {
        src_addr = source.ptr;
      } else {
        imported_base = hapiIpcImportBuffer(source.ipc_handle,
                                            CmiNodeOf(srcPe), source.ipc_base,
                                            source.ipc_offset + (size_t)dest.cnt);
        if (imported_base == NULL) {
          // Name the CUDA error rather than listing the possibilities.
          // cudaErrorAlreadyMapped means this process still holds a mapping of
          // this memory that the import cache did not find; an invalid-handle
          // error means the exporter's allocation is genuinely gone, so the
          // sender released it while its handle was in flight.
          CkAbort("CkRdmaDeviceIssueRgets: receive on PE %d from PE %d could not "
                  "open the CUDA IPC handle for a %zu-byte direct transfer: "
                  "%s (%d).",
                  CkMyPe(), srcPe, (size_t)dest.cnt,
                  hapiIpcLastImportErrorName(), hapiIpcLastImportError());
        }
        src_addr = (const void*)((char*)imported_base + source.ipc_offset);
      }

      (void)imported_base;
      return src_addr;
}

// The one copy a direct transfer makes, unbatched: from the resolved peer
// address into the posted buffer, on the given stream. The ordinary receive
// path batches this (deviceRecvBatchAdd); the pull of a parked message's
// payload (CkRdmaDeviceStageParked) issues it alone on the staging stream.
static void deviceIpcReceive(CkDeviceBuffer& source, CkDeviceBuffer& dest,
                             hapiStream_t recv_stream, int srcPe,
                             CkNcpyModeDevice mode)
{
  const void* src_addr = deviceIpcResolveSource(source, dest, srcPe, mode);
  // No wait here: the caller issues this only once the sender's work is known
  // complete on the host, so nothing on recv_stream can block behind a sender.
  // The slot is released when this copy completes: the caller recorded it in
  // the op, and CkRdmaDeviceRecvHandler raises its dst_flag then.
  {
    void* const dp = (void*)dest.ptr; const size_t cnt = dest.cnt;
    hapiSubmit(recv_stream, [=]() { deviceImportedCopyAsync(dp, src_addr, cnt, recv_stream); });
  }
}


// Defined below, next to the reclaim scan it drives.
static bool acquireIpcSendSlot(DeviceManager* dm, int cpv_my_device_id,
                               bool is_lb_buffer, bool direct,
                               const void* src_ptr, size_t cnt,
                               void** out_buffer, int* out_event_idx,
                               bool wait = true);

/*************** Host-gated receive issue ***************/
//
// A device receive has two dependencies: the sender's producing work, and
// whatever ran on the posted stream before the post (the buffer's previous
// reader or writer). Putting either into a stream as a wait makes everything
// queued behind it wait too: on the consumer's compute stream that stalled
// other chares' kernels behind an unrelated sender; on a shared receive
// stream it would stall other receives. So neither goes on the GPU. The copy
// is issued only once both are known complete on the host, on the PE's one
// receive stream, where consequently nothing ever blocks. A receive whose
// dependencies are still running is parked and re-checked from the scheduler
// loop -- with plain loads where the sender or the post issued a pinned flag,
// and with an event query no more than once per kRecvQueryPeriod otherwise.

static hapiStream_t deviceRecvStream()
{
  hapiStream_t& s = CkpvAccess(device_recv_stream);
  if (s == nullptr) s = hapiAcquireRuntimeStream(/*highPriority=*/true);
  return s;
}

static hapiEvent_t deviceRecvEventTake()
{
  std::vector<hapiEvent_t>& pool = *CkpvAccess(device_recv_events);
  if (pool.empty()) {
    hapiEvent_t e;
    hapiCheck(hapiEventCreateWithFlags(&e, hapiEventDisableTiming));
    return e;
  }
  hapiEvent_t e = pool.back();
  pool.pop_back();
  return e;
}
static void deviceRecvEventGive(hapiEvent_t e)
{
  CkpvAccess(device_recv_events)->push_back(e);
}

static inline hapi_ipc_event_shared* ipcSharedSlot(int device_idx, int event_idx)
{
  GPUManager& gm = CsvAccess(gpu_manager);
  return (hapi_ipc_event_shared*)((char*)gm.shm_ptr
      + gm.shm_chunk_size * device_idx + sizeof(hapiIpcMemHandle_t)) + event_idx;
}

// The sender's half of the source condition for a cross-process send: once
// the work that produced this slot's payload has completed, raise src_ready
// in the shared slot so the receiving process sees it without a driver call.
// A HAPI callback on the producing stream; reclaimCompletedIpcEvents will not
// free the slot before it has fired.
static void ipcSrcReadyFn(void* param, void*)
{
  const uintptr_t packed = (uintptr_t)param;
  ipcSharedSlot((int)(packed >> 32), (int)(uint32_t)packed)
      ->src_ready.store(true, std::memory_order_release);
}
static void ipcPublishSrcReady(int device_idx, int event_idx, hapiStream_t stream)
{
  const uintptr_t packed = ((uintptr_t)(uint32_t)device_idx << 32) | (uint32_t)event_idx;
  hapiAddCallback(stream, CkCallback(ipcSrcReadyFn, (void*)packed));
}
// For a sender that settled its stream synchronously: ready now.
static void ipcPublishSrcReadyNow(int device_idx, int event_idx)
{
  ipcSharedSlot(device_idx, event_idx)->src_ready.store(true, std::memory_order_release);
}

// Decide at post time what the destination still has to wait for: nothing if
// the poster said the buffer is free or the posted stream is idle; else a
// flag on the posted stream, or an event when the ring has no slot. Kept in
// the op so a receive deferred for correction resumes with the same
// condition, taken before anything later was added to that stream.
static void deviceRecvNoteDestination(DeviceRdmaOp& op, const CkDeviceBufferPost& post)
{
  op.dst_flag_rank = -1;
  op.dst_flag_seq = 0;
  op.dst_event = NULL;
  if (post.buffer_free) return;
  hapiSubmitDrain();   // queued work on the posted stream must be in the driver before it is asked
  const hapiError_t q = hapiStreamQuery(post.hapi_stream);
  if (q == hapiSuccess) return;
  if (q != hapiErrorNotReady) { hapiCheck(q); return; }
  hapiGetLastError();   // NotReady is the answer, not an error to leave behind
  int rank = -1;
  uint32_t seq = 0;
  if (hapiFlagIssue(post.hapi_stream, &rank, &seq)) {
    op.dst_flag_rank = rank;
    op.dst_flag_seq = seq;
    return;
  }
  hapiEvent_t ev = deviceRecvEventTake();
  if (hapiEventRecord(ev, post.hapi_stream) == hapiSuccess) {
    op.dst_event = (void*)ev;
  } else {
    // Another device's stream (a chare that moved between GPUs): our event
    // cannot be recorded on it. Settle it instead; rare and bounded.
    hapiGetLastError();
    deviceRecvEventGive(ev);
    hapiCheck(ckSubmitDrainThenSync(post.hapi_stream));
  }
}

enum class DeviceRecvKind : char { Memcpy, Ipc, Staged };

struct DeviceRecvPending {
  DeviceRdmaOp* op = nullptr;
  CkDeviceBuffer source;
  CkDeviceBuffer dest;
  CkNcpyModeDevice mode = CkNcpyModeDevice::MEMCPY;
  int src_pe = -1;
  DeviceRecvKind kind = DeviceRecvKind::Memcpy;
  StagedLanding sl{};          // Staged only
  CkCallbackFn done = nullptr; // completion handler, called with op
  bool src_ready = false;
  bool dst_ready = false;
  double next_query = 0.0;     // earliest time for the next driver-call check
};

static const double kRecvQueryPeriod = 100e-6;

static inline bool deviceRecvEventDone(hapiEvent_t ev)
{
  const hapiError_t q = hapiEventQuery(ev);
  if (q == hapiSuccess) return true;
  if (q != hapiErrorNotReady) hapiCheck(q);
  hapiGetLastError();
  return false;
}

// Is the sender's producing work done? Plain loads first; a driver query only
// when may_query.
static bool deviceRecvSourceReady(const DeviceRecvPending& p, bool may_query)
{
  GPUManager& gm = CsvAccess(gpu_manager);
  switch (p.kind) {
  case DeviceRecvKind::Memcpy:
    if (p.source.ready_seq != 0)
      return hapiFlagLanded(p.source.ready_rank, p.source.ready_seq);
    if (p.source.memcpy_event == NULL) return true;   // the sender settled synchronously
    return may_query && deviceRecvEventDone((hapiEvent_t)p.source.memcpy_event);   // recorded after a drain, see ckDeviceRecordMemcpyEvent
  case DeviceRecvKind::Ipc:
    if (p.source.event_idx < 0 ||
        (size_t)p.source.device_idx >= gm.hapi_ipc_device_infos.size())
      return true;   // deviceIpcReceive reports a bad index
    if (ipcSharedSlot(p.source.device_idx, p.source.event_idx)
            ->src_ready.load(std::memory_order_acquire))
      return true;
    // Submit mode: the sender's event may still sit in ITS submitter's queue, and
    // an unrecorded event reads as complete. src_ready (a flag) is the only word.
    if (hapiSubmitOn()) return false;
    return may_query &&
        deviceRecvEventDone(gm.hapi_ipc_device_infos[p.source.device_idx]
                                .src_event_pool[p.source.event_idx]);
  case DeviceRecvKind::Staged:
    return may_query && deviceRecvEventDone(p.sl.ready);
  }
  return true;
}

// Has everything issued on the posted stream before the post completed?
static bool deviceRecvDestinationReady(DeviceRecvPending& p, bool may_query)
{
  DeviceRdmaOp& op = *p.op;
  if (op.dst_flag_seq != 0) return hapiFlagLanded(op.dst_flag_rank, op.dst_flag_seq);
  if (op.dst_event == NULL) return true;
  if (!may_query || !deviceRecvEventDone((hapiEvent_t)op.dst_event)) return false;
  deviceRecvEventGive((hapiEvent_t)op.dst_event);
  op.dst_event = NULL;
  return true;
}

// ---- batched landings -------------------------------------------------------
//
// A receive that is ready to land used to be its own copy on the PE's receive
// stream with its own completion behind it. A halo exchange lands hundreds of
// small payloads per PE in one burst, and the per-landing fixed costs -- the
// copy issue, the completion record, the poll that retires it -- came to more
// than the bytes: sph3d at 25M particles on 4 A40s lost ~10 ms per slab
// boundary per exchange, and two GPUs were slower than one. Landings are now
// gathered per PE and issued as ONE batched copy (cudaMemcpyBatchAsync, CUDA
// 12.8+; one copy per op on the same stream otherwise) with ONE completion
// behind it, whose handler completes every op of the batch exactly as the
// per-op handler did. The protocol per message -- post, readiness, slot
// release, delivery, source callback, migration interlocks -- is untouched;
// only how the bytes move changed.
//
// A batch flushes when it is full, when it has aged 500 microseconds, or as
// soon as the PE goes idle -- so a landing waits at most for the rest of
// the burst it arrived in, and never when there is nothing to wait for.
struct DeviceRecvBatchEntry {
  DeviceRdmaOp* op;
  CkCallbackFn done;
};
struct DeviceRecvBatch {
  std::vector<void*> dsts;
  std::vector<const void*> srcs;
  std::vector<size_t> sizes;
  std::vector<DeviceRecvBatchEntry> entries;
  double opened = 0.0;
};
static const size_t kRecvBatchMax = 256;

static int deviceRecvBatchUsec() { return 500; }

// Behind the batch's copies on the receive stream: every op of it has landed.
static void deviceRecvBatchDone(void* arg, void*)
{
  DeviceRecvBatch* b = (DeviceRecvBatch*)arg;
  for (const DeviceRecvBatchEntry& e : b->entries) e.done((void*)e.op, nullptr);
  delete b;
}

static void deviceRecvBatchFlush()
{
  DeviceRecvBatch*& cur = CkpvAccess(device_recv_batch);
  if (cur == nullptr) return;
  DeviceRecvBatch* b = cur;
  cur = nullptr;
  const size_t n = b->entries.size();
  if (zcStatsOn()) {
    zc_mode_stats.batch_flushes.fetch_add(1, std::memory_order_relaxed);
    zc_mode_stats.batch_ops.fetch_add((long)n, std::memory_order_relaxed);
    long m = zc_mode_stats.batch_max.load(std::memory_order_relaxed);
    while (m < (long)n &&
           !zc_mode_stats.batch_max.compare_exchange_weak(m, (long)n, std::memory_order_relaxed)) {}
  }
  hapiStream_t rs = deviceRecvStream();
  // The arrays belong to the batch, which lives until its completion handler
  // frees it, so the closure may run later (submit mode) and still read them.
  hapiSubmit(rs, [=]() {
    const double tb0 = zcStatsOnFast() ? CkWallTimer() : 0.0;
    hapiError_t err = hapiErrorNotSupported;
#if defined(CUDART_VERSION) && CUDART_VERSION >= 12080
    if (n > 1) {
      cudaMemcpyAttributes attr{};
      attr.srcAccessOrder = cudaMemcpySrcAccessOrderStream;
      size_t attr_idx = 0;
#if CUDART_VERSION >= 13000
      err = cudaMemcpyBatchAsync(b->dsts.data(), b->srcs.data(), b->sizes.data(), n,
                                 &attr, &attr_idx, 1, rs);
#else
      size_t fail_idx = 0;   // 12.8 and 12.9 report the first failed copy here
      err = cudaMemcpyBatchAsync(b->dsts.data(), b->srcs.data(), b->sizes.data(), n,
                                 &attr, &attr_idx, 1, &fail_idx, rs);
#endif
    }
#elif CMK_HIP
    {   // as above: one attribute for the batch, the failure index reported; every size, for the
        // single copy out of a peer's imported vmm heap (deviceChannelBatchFlush has the ROCr crash)
      hipMemcpyAttributes attr{};
      attr.srcAccessOrder = hipMemcpySrcAccessOrderStream;
      size_t attr_idx = 0, fail_idx = 0;
      err = hipMemcpyBatchAsync(b->dsts.data(), const_cast<void**>(b->srcs.data()), b->sizes.data(), n,
                                &attr, &attr_idx, 1, &fail_idx, rs);
      if (err != hipSuccess)
        fprintf(stderr, "[recv-batch] pe %d: hipMemcpyBatchAsync(n=%zu) failed at %zu: %s\n", CkMyPe(), n, fail_idx, hipGetErrorString(err));
    }
#endif
    if (err != hapiSuccess) {
      if (n > 1) {
        hapiGetLastError();   // the refusal is answered by the loop below
        if (zcStatsOn()) zc_mode_stats.batch_fallback.fetch_add(1, std::memory_order_relaxed);
      }
      // cudaMemcpyDefault: source and destination may sit on different devices
      // (a peer's memory through its IPC import, or a chare moved between GPUs).
      for (size_t i = 0; i < n; i++)
        hapiCheck(hapiMemcpyAsync(b->dsts[i], b->srcs[i], b->sizes[i], hapiMemcpyDefault, rs));
    }
    if (tb0 != 0.0)
      zc_mode_stats.batch_us.fetch_add((long)((CkWallTimer() - tb0) * 1e6), std::memory_order_relaxed);
  });
  hapiAddCallback(rs, CkCallback(deviceRecvBatchDone, (void*)b));
}

// A landing whose source and destination are both ready: into the open batch,
// or straight onto the stream with its own completion when batching is off.
static void deviceRecvBatchAdd(void* dst, const void* src, size_t bytes,
                               DeviceRdmaOp* op, CkCallbackFn done)
{
  if (deviceRecvBatchUsec() <= 0) {
    hapiStream_t rs = deviceRecvStream();
    hapiSubmitMemcpyAsync(dst, src, bytes, hapiMemcpyDefault, rs);
    hapiAddCallback(rs, CkCallback(done, (void*)op));
    return;
  }
  DeviceRecvBatch*& cur = CkpvAccess(device_recv_batch);
  if (cur == nullptr) {
    cur = new DeviceRecvBatch;
    cur->opened = CkWallTimer();
    cur->dsts.reserve(64); cur->srcs.reserve(64); cur->sizes.reserve(64); cur->entries.reserve(64);
  }
  cur->dsts.push_back(dst);
  cur->srcs.push_back(src);
  cur->sizes.push_back(bytes);
  cur->entries.push_back(DeviceRecvBatchEntry{op, done});
  if (cur->entries.size() >= kRecvBatchMax) deviceRecvBatchFlush();
}

static void deviceRecvBatchPoll(void*)
{
  DeviceRecvBatch* cur = CkpvAccess(device_recv_batch);
  if (cur == nullptr) return;
  if (CkWallTimer() - cur->opened >= deviceRecvBatchUsec() * 1e-6) deviceRecvBatchFlush();
}

static void deviceRecvBatchIdle(void*)
{
  if (CkpvAccess(device_recv_batch) != nullptr) deviceRecvBatchFlush();
}

static void deviceRecvIssue(DeviceRecvPending& p)
{
  GPUManager& csv_gpu_manager = CsvAccess(gpu_manager);
  hapiStream_t rs = deviceRecvStream();
  CkDeviceBuffer& source = p.source;
  CkDeviceBuffer& dest = p.dest;
  switch (p.kind) {
  case DeviceRecvKind::Staged: {
    // The pull's completion was the source condition; copy the landing into
    // the posted buffer and free it behind that copy.
    const StagedLanding& sl = p.sl;
    hapiSubmitMemcpyAsync((void*)dest.ptr, source.ptr, dest.cnt, hapiMemcpyDefault, rs);
    hapiAddCallback(rs, CkCallback(stagedLandingFreeFn,
                                   (void*)new StagedLandingFree{(void*)source.ptr, sl.ready}));
    if (stagedDbg())
      CmiPrintf("[STAGE-USE %d] land=%p cnt=%zu -> posted %p\n", CkMyPe(),
                source.ptr, (size_t)dest.cnt, (void*)dest.ptr);
    break;
  }
  case DeviceRecvKind::Memcpy: {
    // Same process: a device-to-device copy from the sender's buffer. The
    // sender's producing work is already complete (deviceRecvSourceReady),
    // so no stream wait precedes it.
    // A sender that staged IPC state for this transfer anyway -- an
    // unconfirmed destination that turned out to be this process, or a send
    // that left the process and came back -- holds a slot nothing else will
    // release. Its dst_flag is raised when this copy completes
    // (CkRdmaDeviceRecvHandler), exactly as on the IPC path.
    if (source.device_idx >= 0 && source.event_idx >= 0 && csv_gpu_manager.use_shm) {
      p.op->ipc_device_idx = source.device_idx;
      p.op->ipc_event_idx = source.event_idx;
    }
    // cudaMemcpyDefault, not DeviceToDevice: once load balancing has moved
    // chares between GPUs, the source and destination of a same-process
    // transfer can sit on different devices, and an explicit DeviceToDevice
    // kind is rejected for that pair. Default resolves the direction from the
    // pointers themselves and handles the peer case.
    deviceRecvBatchAdd((void*)dest.ptr, source.ptr, dest.cnt, p.op, p.done);
    return;
  }
  case DeviceRecvKind::Ipc:
    // The slot is released from the completion handler; naming it in the op
    // is what arms that.
    if (source.device_idx >= 0 && source.event_idx >= 0) {
      p.op->ipc_device_idx = source.device_idx;
      p.op->ipc_event_idx = source.event_idx;
    }
    deviceRecvBatchAdd((void*)dest.ptr, deviceIpcResolveSource(source, dest, p.src_pe, p.mode),
                       dest.cnt, p.op, p.done);
    return;
  }
  hapiAddCallback(rs, CkCallback(p.done, (void*)p.op));
}

// Issue now if both conditions hold, else park for deviceRecvPendingPoll.
static void deviceRecvTryIssue(DeviceRecvPending&& pending)
{
  pending.src_ready = deviceRecvSourceReady(pending, /*may_query=*/true);
  pending.dst_ready = deviceRecvDestinationReady(pending, /*may_query=*/true);
  if (pending.src_ready && pending.dst_ready) {
    deviceRecvIssue(pending);
    return;
  }
  pending.next_query = CkWallTimer() + kRecvQueryPeriod;
  std::vector<DeviceRecvPending*>& list = *CkpvAccess(device_recv_pending);
  list.push_back(new DeviceRecvPending(std::move(pending)));
  g_recv_parked++;
  if ((long)list.size() > g_recv_parked_max) g_recv_parked_max = (long)list.size();
}

static void deviceRecvPendingPoll(void*)
{
  ZcPollTimer zc_poll_timer{zcStatsOnFast() ? CkWallTimer() : 0.0, &zc_mode_stats.recvpoll_n, &zc_mode_stats.recvpoll_us, &zc_mode_stats.recvpoll_max};
  std::vector<DeviceRecvPending*>& list = *CkpvAccess(device_recv_pending);
  if (list.empty()) return;
  const double now = CkWallTimer();
  size_t kept = 0;
  for (size_t i = 0; i < list.size(); i++) {
    DeviceRecvPending* p = list[i];
    const bool may_query = (now >= p->next_query);
    if (may_query) p->next_query = now + kRecvQueryPeriod;
    if (!p->src_ready) p->src_ready = deviceRecvSourceReady(*p, may_query);
    if (!p->dst_ready) p->dst_ready = deviceRecvDestinationReady(*p, may_query);
    if (p->src_ready && p->dst_ready) {
      deviceRecvIssue(*p);
      delete p;
    } else {
      list[kept++] = p;
    }
  }
  list.resize(kept);
}

/*************** Forward-time repair of a memcpy-prepared payload ***************/
//
// CkRdmaDeviceOnSender prepares a send for a co-resident target as MEMCPY: a
// raw device pointer, no export, no IPC event. That is exact at the moment it
// is decided (the process-wide resident table is consulted), but the message
// is then queued, and if the balancer moves the target out of the process
// before the message is consumed, the location manager forwards it to a home
// that cannot read the pointer. Until now that cost a correction round trip
// (requestDeviceRestage): the new home asked the sender for the bytes again.
//
// The forward happens in the process that owns the source, and the source is
// still alive -- a memcpy send ships its completion callback with the message,
// so nothing has released the buffer -- so the payload can be re-prepared
// here as a first-time direct send would have been: export the source, claim
// an IPC event slot, record the event behind the memcpy event that marks the
// data as produced, and rewrite the descriptor in the message. The receiver
// then takes its ordinary direct path and no correction is requested. Costs
// nothing when nothing moves.
//
// A forward off the physical node is repaired the same way, as a first-time
// RDMA send would have been prepared: the bytes have to cross the network
// anyway, and the sender's side of that is a registered descriptor for the
// live source, which in pool mode is the arena's one registration. The
// receiver then takes its ordinary rget path. Before this the inter-node case
// fell back to the correction round trip -- a NACK, a put, and a
// notification -- through a path nothing else exercised.
//
// The descriptors sit at the front of every device-send message, right after
// their count (see the generated _call_ functions), and have one width in
// every protocol (CmiDeviceBuffer::pup), which is what makes the in-place
// rewrite possible without generated code.
// forwarder -> source PE: a message whose device payload only the source's
// process can re-prepare for the network, with the PE it must reach. The
// envelope follows the header verbatim.
struct DeviceForwardRedirect {
  char header[CmiMsgHeaderSizeBytes];
  int newPe;
  int envSize;
};
extern "C" { int device_forward_redirect_handler; }

// An inter-node forward repair re-registers the sender's live buffer so the
// new home can rget it, and the NIC reads that buffer outside every CUDA
// stream -- so, exactly as in issueDeviceRestagePut, the producing kernel has
// to have FINISHED before the descriptor is published, not merely be ordered
// ahead of it. cudaEventSynchronize bought that by stopping the PE until the
// GPU drained, once per repaired payload.
//
// Park the message behind the producer instead. The repair is re-entrant: a
// descriptor it has already rewritten is marked sender_prepared with
// device_idx -1, which the loop below skips as an RDMA descriptor, so resuming
// simply picks up where the wait interrupted it. Each pass repairs at least
// the descriptor it waited on, so the chain terminates.
struct DeviceForwardRepairCtx
{
  envelope* env;
  int newPe;
  int opts;
};

static void deviceForwardRepairReady(void* arg, void*)
{
  DeviceForwardRepairCtx* ctx = (DeviceForwardRepairCtx*)arg;
  // Resume. A further descriptor may park again, or the pass may now redirect;
  // in both cases something else owns the message and this pass must not
  // deliver it.
  if (CkRdmaDeviceRepairForward(ctx->env, ctx->newPe, ctx->opts) ==
      CkDeviceRepairResult::CallerDelivers)
    CkArrayManagerDeliver(ctx->newPe, EnvToUsr(ctx->env), ctx->opts);
  delete ctx;
}

// True when the producer has already finished and the caller may publish the
// descriptor now. False when the message has been parked behind it: the caller
// owns nothing further and must report the message consumed.
static bool forwardRepairReadyOrPark(envelope* env, int newPe, int opts,
                                     hapiEvent_t ev)
{
  if (ev == NULL) return true;
  // The overwhelmingly common case: the payload was produced long before its
  // target moved, so there is nothing to wait for and nothing to park.
  const hapiError_t q = hapiEventQuery(ev);
  if (q == hapiSuccess) return true;
  if (q != hapiErrorNotReady) {
    hapiCheck(q);   // a real failure: report it where it happened
    return true;
  }
  hapiCheck(hapiStreamWaitEvent(restageWaitStream(), ev, 0));
  DeviceForwardRepairCtx* ctx = new DeviceForwardRepairCtx{env, newPe, opts};
  // hapiAddCallback holds quiescence for the whole gap and delivers to this PE.
  hapiAddCallback(restageWaitStream(), CkCallback(deviceForwardRepairReady, ctx));
  return false;
}

// The same gate for a sender that marked its stream with a pinned flag and
// recorded no event (the normal same-process send). A flag lands without a
// callback, so the wait is a scheduler-loop poll; QdCreate holds quiescence
// across it exactly as hapiAddCallback does above.
struct DeviceForwardFlagWait
{
  DeviceForwardRepairCtx* ctx;
  int rank;
  uint32_t seq;
};

static void deviceForwardFlagPoll(void* arg)
{
  DeviceForwardFlagWait* w = (DeviceForwardFlagWait*)arg;
  if (!hapiFlagLanded(w->rank, w->seq)) {
    CcdCallOnCondition(CcdSCHEDLOOP, deviceForwardFlagPoll, w);
    return;
  }
  QdProcess(1);
  deviceForwardRepairReady(w->ctx, NULL);
  delete w;
}

static bool forwardRepairFlagReadyOrPark(envelope* env, int newPe, int opts,
                                         int rank, uint32_t seq)
{
  if (hapiFlagLanded(rank, seq)) return true;
  DeviceForwardRepairCtx* ctx = new DeviceForwardRepairCtx{env, newPe, opts};
  QdCreate(1);
  CcdCallOnCondition(CcdSCHEDLOOP, deviceForwardFlagPoll,
                     new DeviceForwardFlagWait{ctx, rank, seq});
  return false;
}

CkDeviceRepairResult CkRdmaDeviceRepairForward(envelope* env, int newPe, int opts) {
  if (!CMI_IS_ZC_DEVICE(env)) return CkDeviceRepairResult::CallerDelivers;
  if (env->getMsgtype() != ForArrayEltMsg) return CkDeviceRepairResult::CallerDelivers;
  if (CmiNodeOf(newPe) == CmiMyNode()) return CkDeviceRepairResult::CallerDelivers;       // still readable as is
  GPUManager& csv_gpu_manager = CsvAccess(gpu_manager);
  static const bool dbg = (getenv("CHARM_ZC_RESTAGE_DEBUG") != nullptr);

  if (!CmiPeOnSamePhysicalNode(newPe, CkMyPe())) {
    // Inter-node forward: re-prepare as an RDMA send.
    //
    // Only the source's own process can do that: the registration has to be
    // of the live buffer in the address space that owns it. A forward from
    // another process on the same physical node -- the common case with one
    // PE per process, where every cross-process forward is one -- is bounced
    // to the source PE over shared memory with the destination attached; it
    // repairs and delivers off-node from there. One extra same-node hop, in
    // place of the correction round trip nothing else exercised.
    int redirectTo = -1;
    char* buf = ((CkMarshallMsg*)EnvToUsr(env))->msgBuf;
    PUP::fromMem rd(buf);
    int n = 0;
    rd | n;
    for (int i = 0; i < n; i++) {
      const size_t off = rd.size();
      CkDeviceBuffer b;
      rd | b;
      const size_t width = rd.size() - off;
      // Empty: nothing to re-prepare, in any mode (CkRdmaDeviceOnSender).
      if (b.cnt == 0) continue;
      // Two kinds of descriptor can be repaired here, because in both the
      // sender's live buffer is the payload: the memcpy-prepared one (nothing
      // prepared at all) and the direct-IPC one (the live buffer exported by
      // handle, which means nothing off this node). A staged descriptor's
      // bytes are a copy in this node's comm buffer and keep the correction
      // path; so does anything whose source is not in this process.
      const bool direct_prepared =
          b.sender_prepared && b.ipc_protocol == CmiIpcProtocol::DIRECT &&
          b.device_idx >= 0 &&
          (size_t)b.device_idx < csv_gpu_manager.hapi_ipc_device_infos.size();
      // Already an RDMA descriptor (a first-hop inter-node send being
      // forwarded again): readable from anywhere, nothing to do.
      if (b.sender_prepared && b.device_idx < 0) continue;
      const char* skip = NULL;
      if (b.sender_prepared && !direct_prepared) skip = "staged, left to the correction path";
      else if (b.src_mpi_rank != CmiMyNode()) {
        if (CmiPeOnSamePhysicalNode(b.src_pe, CkMyPe())) { redirectTo = b.src_pe; break; }
        skip = "source on another physical node";
      }
      if (skip) {
        if (dbg) {
          CmiPrintf("[%d] ZC FORWARD-SKIP src=%p cnt=%zu srcPe=%d -> PE %d: %s (inter-node)\n",
                    CkMyPe(), b.ptr, (size_t)b.cnt, b.src_pe, newPe, skip);
          fflush(stdout);
        }
        continue;
      }
      // The NIC reads the source outside any CUDA stream, so the production
      // has to be complete, not merely ordered behind. The memcpy prepare marks
      // it with a pinned flag (ready_seq), or with memcpy_event when no flag
      // could be issued; the direct prepare recorded its slot's src event on
      // the producing stream. Neither flag nor event means the data was
      // complete at send time (no stream given, or the send blocked).
      if (direct_prepared) {
        hapi_ipc_device_info& info = csv_gpu_manager.hapi_ipc_device_infos[b.device_idx];
        if (!forwardRepairReadyOrPark(env, newPe, opts, info.src_event_pool[b.event_idx]))
          return CkDeviceRepairResult::Parked;
        // Retire the IPC slot the direct prepare claimed: the receiver it was
        // claimed for is on another node and will never set its flag. A direct
        // slot holds no block and its producer is complete (checked above), so
        // the flag alone lets the owning PE's reclaim scan take it back.
        hapi_ipc_event_shared* slot =
            (hapi_ipc_event_shared*)((char*)csv_gpu_manager.shm_ptr
                + csv_gpu_manager.shm_chunk_size * b.device_idx
                + sizeof(hapiIpcMemHandle_t)) + b.event_idx;
        slot->dst_flag.store(true, std::memory_order_release);
      } else if (b.ready_seq != 0) {
        if (!forwardRepairFlagReadyOrPark(env, newPe, opts, b.ready_rank, b.ready_seq))
          return CkDeviceRepairResult::Parked;
      } else if (b.memcpy_event != NULL) {
        if (!forwardRepairReadyOrPark(env, newPe, opts, (hapiEvent_t)b.memcpy_event))
          return CkDeviceRepairResult::Parked;
      }
      // The source is still owned by the sending element: a memcpy send ships
      // its completion callback with the message, and the receiver fires it
      // once the rget has landed, exactly as it would for a first-time RDMA
      // send. No owner is attributed here (the forwarding context is not the
      // sender); in pool mode the registration is the arena's anyway.
      b.lci_ncpy_buffer = acquireDeviceRegistration(b.ptr, b.cnt, NULL);
      b.sender_prepared = true;
      b.device_idx = -1;                              // not IPC-exported: the rget path
      b.event_idx = -1;
      b.comm_offset = 0;
      b.ipc_protocol = CmiIpcProtocol::NONE;
      b.dest_pe = newPe;
      b.dest_mpi_rank = CmiNodeOf(newPe);
      PUP::toMem wr(buf + off);
      wr | b;
      if (wr.size() != width)
        CkAbort("CkRdmaDeviceRepairForward: descriptor width changed (%zu -> %zu)",
                width, wr.size());
      csv_gpu_manager.ipc_forward_repairs.fetch_add(1, std::memory_order_relaxed);
      if (dbg) {
        CmiPrintf("[%d] ZC FORWARD-REPAIR src=%p cnt=%zu -> PE %d (rdma)\n",
                  CkMyPe(), b.ptr, (size_t)b.cnt, newPe);
        fflush(stdout);
      }
    }
    if (redirectTo < 0) return CkDeviceRepairResult::CallerDelivers;

    // Every descriptor of one message comes from one element, so one source
    // PE serves the whole message. Ship the envelope to it verbatim.
    CkPackMessage(&env);
    const int envSize = env->getTotalsize();
    DeviceForwardRedirect* r = (DeviceForwardRedirect*)CmiAlloc(
        sizeof(DeviceForwardRedirect) + envSize);
    CmiEnforce(r);
    r->newPe = newPe;
    r->envSize = envSize;
    memcpy((char*)r + sizeof(DeviceForwardRedirect), env, envSize);
    if (dbg) {
      CmiPrintf("[%d] ZC FORWARD-REDIRECT %d bytes -> source PE %d, for PE %d\n",
                CkMyPe(), envSize, redirectTo, newPe);
      fflush(stdout);
    }
    CmiFree(env);
    QdCreate(1);
    CmiSetHandler(r, device_forward_redirect_handler);
    CmiSyncSendAndFree(redirectTo, sizeof(DeviceForwardRedirect) + envSize, (char*)r);
    csv_gpu_manager.ipc_forward_repairs.fetch_add(1, std::memory_order_relaxed);
    return CkDeviceRepairResult::Redirected;
  }

  if (!csv_gpu_manager.use_shm || !hapiIpcUseDirect()) return CkDeviceRepairResult::CallerDelivers;
  auto dmit = csv_gpu_manager.device_map.find(CkMyPe());
  if (dmit == csv_gpu_manager.device_map.end()) return CkDeviceRepairResult::CallerDelivers;
  DeviceManager* dm = dmit->second;

  char* buf = ((CkMarshallMsg*)EnvToUsr(env))->msgBuf;
  PUP::fromMem rd(buf);
  int n = 0;
  rd | n;
  for (int i = 0; i < n; i++) {
    const size_t off = rd.size();
    CkDeviceBuffer b;
    rd | b;
    const size_t width = rd.size() - off;
    if (b.cnt == 0) continue;                        // empty: nothing to re-prepare
    if (b.sender_prepared) continue;                 // IPC-prepared: readable node-wide already
    const char* skip = NULL;
    auto srcit = csv_gpu_manager.device_map.find(b.src_pe);
    hapiIpcMemHandle_t handle;
    size_t export_offset = 0;
    void* export_base = NULL;
    if (b.src_mpi_rank != CmiMyNode()) skip = "source not in this process";
    else if (srcit == csv_gpu_manager.device_map.end() || srcit->second != dm)
      skip = "source on another device";
    else if (!hapiIpcExportBuffer(b.ptr, &handle, &export_offset, &export_base))
      skip = "export failed";
    if (skip) {
      if (dbg) {
        CmiPrintf("[%d] ZC FORWARD-SKIP src=%p cnt=%zu srcPe=%d -> PE %d: %s\n",
                  CkMyPe(), b.ptr, (size_t)b.cnt, b.src_pe, newPe, skip);
        fflush(stdout);
      }
      continue;                                      // left to the correction protocol
    }
    const int dev = CpvAccess(my_device_id);
    void* unused = NULL;
    int ev = -1;
    acquireIpcSendSlot(dm, dev, /*is_lb_buffer=*/false, /*direct=*/true,
                       b.ptr, b.cnt, &unused, &ev);
    // The receiver waits on this event before it reads. Order it behind the
    // memcpy event the send recorded on the producing stream (the receiver's
    // memcpy branch would have waited on that same event).
    if (b.memcpy_event != NULL)
      hapiCheck(hapiStreamWaitEvent(hapiStreamPerThread,
                                    (hapiEvent_t)b.memcpy_event, 0));
    const int device_idx =
        csv_gpu_manager.device_count * CmiMyNodeRankLocal() + dev;
    hapi_ipc_device_info& info = csv_gpu_manager.hapi_ipc_device_infos[device_idx];
    hapiCheck(hapiEventRecord(info.src_event_pool[ev], hapiStreamPerThread));
    ipcPublishSrcReady(device_idx, ev, hapiStreamPerThread);

    b.ipc_protocol = CmiIpcProtocol::DIRECT;
    b.ipc_handle = handle;
    b.ipc_offset = export_offset;
    b.ipc_base = export_base;
    b.device_idx = device_idx;
    b.event_idx = ev;
    b.comm_offset = 0;
    b.sender_prepared = true;
    b.dest_pe = newPe;
    b.dest_mpi_rank = CmiNodeOf(newPe);
    PUP::toMem wr(buf + off);
    wr | b;
    if (wr.size() != width)
      CkAbort("CkRdmaDeviceRepairForward: descriptor width changed (%zu -> %zu)",
              width, wr.size());
    csv_gpu_manager.ipc_forward_repairs.fetch_add(1, std::memory_order_relaxed);
    if (getenv("CHARM_ZC_RESTAGE_DEBUG") != nullptr) {
      CmiPrintf("[%d] ZC FORWARD-REPAIR src=%p cnt=%zu -> PE %d (direct, ev %d)\n",
                CkMyPe(), b.ptr, (size_t)b.cnt, newPe, ev);
      fflush(stdout);
    }
  }
  return CkDeviceRepairResult::CallerDelivers;
}

// Source PE: a forward bounced here because only this process can re-prepare
// its payload for the network. Repair in place -- the source is in this
// process now, so the inter-node branch above takes it -- and deliver to the
// PE the forwarder named. The descriptor is an RDMA one from here on, so any
// further forward needs no repair at all.
extern "C" void* device_forward_redirect_bridge(void* arg)
{
  QdProcess(1);
  DeviceForwardRedirect* r = (DeviceForwardRedirect*)arg;
  envelope* env = (envelope*)CmiAlloc(r->envSize);
  CmiEnforce(env);
  memcpy(env, (char*)r + sizeof(DeviceForwardRedirect), r->envSize);
  const int newPe = r->newPe;
  CmiFree(r);
  CkUnpackMessage(&env);
  // A second REDIRECT is fatal: this is already the source process, so there is
  // nowhere left to bounce it. A PARK is not -- the payload's producer simply
  // has not finished yet, and the callback that resumes the repair delivers it.
  // Collapsing the two is what aborted the async arm at the first parked
  // redirect, since every park read as a bounce.
  switch (CkRdmaDeviceRepairForward(env, newPe)) {
    case CkDeviceRepairResult::Redirected:
      CkAbort("[%d] device forward redirect bounced again: the source process "
              "could not repair its own payload", CkMyPe());
      break;
    case CkDeviceRepairResult::Parked:
      return NULL;   // deviceForwardRepairReady delivers it
    case CkDeviceRepairResult::CallerDelivers:
      break;
  }
  CkArrayManagerDeliver(newPe, EnvToUsr(env), 0);
  return NULL;
}

/*************** Migration-mismatch payload correction (NACK + retransmit) ***************/
//
// A device send picks its transfer mode from where the sender believes the
// target lives, but that only becomes true when the message lands. If the
// target migrates across a process boundary in between, the sender will have
// chosen a plain same-process memcpy and prepared nothing readable from
// anywhere else, while the message itself is routed to the target's new home
// perfectly well by the location manager.
//
// So only the payload is missing, not the message. The receiver detects the
// disagreement, defers that buffer's completion, and asks the sender to send
// the bytes again by a route this process can read. Previously this aborted
// (IPC) or issued an unregistered rdmaGet that surfaced as an LCI "Message too
// long" assert far from the cause (inter-node).
//
// Two disagreements reach here, and they differ in what the sender still owns:
//   sender_prepared false -- a plain same-process memcpy was chosen, so nothing
//                 was exported, but the chare's source buffer is still live: a
//                 memcpy send ships its completion callback to the receiver
//                 rather than firing it, so nobody has released that buffer.
//   staged, target off-node -- the sender did export, into its comm buffer, and
//                 a staged send fires its own callback as soon as that copy
//                 lands. The chare's buffer is free from that moment and may
//                 already hold the next iteration, so the staged block is the
//                 only copy that is still this transfer's payload. The request
//                 carries device_idx and comm_offset back for it; those are
//                 node-local, so they mean nothing where the request is built
//                 and are exact on the PE it is sent to.
//
// The sender's stale location cache needs no help from this protocol: the
// forwarded message took more than one hop, so CkArray::deliverToElement calls
// CkLocMgr::multiHop, which pushes the corrected entry back to the sender.
//
// Correction is cheap because it reuses each transport's existing direction:
//   same node  -- the sender stages into its comm buffer and returns the IPC
//                 metadata; the receiver pulls, as it does for any staged send,
//                 through the once-per-device mapping it already holds.
//   inter-node -- the sender registers its source and RDMA-puts straight into
//                 the descriptor the receiver registered for it, then sends a
//                 one-word notification, because an RDMA write raises its
//                 completion on the initiator and leaves the target unaware.
// Putting into an app buffer costs nothing extra inter-node, where the receiver
// registers that buffer on every receive anyway. Same-node it would mean a cold
// cuIpcOpenMemHandle, charged to the one transfer least able to afford it: a
// corrected payload has already taken the extra forwarding hops and waited out
// a request round trip before any of its bytes move. Not that the mapping would
// be wasted under DIRECT -- that protocol is chosen precisely because the
// application reuses its buffers, so later sends to the chare's new location
// would hit it --
// but it does not have to be opened here. The forwarding above repairs the
// sender's cache, so the next ordinary send opens it instead, off the critical
// path of a transfer that is already late. Corrections also come in a burst
// right after a load balancing step, which is the worst moment in the run to
// add hundreds of microseconds apiece.

struct DeviceRestageReq {          // receiver -> sender
  char header[CmiMsgHeaderSizeBytes];
  void* dest_op;                   // receiver's DeviceRdmaOp*, opaque here
  int dest_pe;
  const void* src_ptr;             // the sender's live source, if it still owns one
  bool src_staged;                 // true: read the staged block below instead
  size_t src_comm_offset;
  // What the source read below must be ordered behind. The original send
  // established this ordering and the correction has to re-establish it: the
  // payload is produced by the application's stream (a packing kernel, or the
  // staging copy), and the re-read runs on hapiStreamPerThread, which is
  // ordered against neither.
  const void* src_event;           // memcpy path: the sender's recorded event
  int src_event_idx;               // staged path: the sender's IPC event slot
  // memcpy path, the normal case: the pinned flag the sender issued instead of
  // an event (CmiDeviceBuffer::ready_seq). Read with a plain load on the PE
  // that issued it; seq 0 means none.
  int src_flag_rank;
  uint32_t src_flag_seq;
  size_t cnt;
  bool inter_node;
  CmiNcpyBuffer dest_ncpy;         // inter-node only: registered destination
};

struct DeviceRestageMeta {         // sender -> receiver, same node
  char header[CmiMsgHeaderSizeBytes];
  void* dest_op;
  int device_idx;
  int event_idx;
  size_t comm_offset;              // STAGED: where in the sender's comm buffer
  size_t cnt;
  // DIRECT (+gpupool): the sender's source allocation, exported in place.
  CmiIpcProtocol protocol;
  hapiIpcMemHandle_t ipc_handle;
  void* ipc_base;
  size_t ipc_offset;
};

struct DeviceRestagePutDone {      // sender -> receiver, inter-node put
  char header[CmiMsgHeaderSizeBytes];
  void* dest_op;                   // receiver's DeviceRdmaOp*
};

extern "C" {
  int device_restage_req_handler;
  int device_restage_meta_handler;
  int device_restage_put_done_handler;
}

// Sender side: the write has completed locally, and nothing on the target has
// been told, because an RDMA write raises no completion there. So tell it. This
// is the same shape as LCI's own rendezvous protocol, which posts its puts and
// then, from the local write completion, sends a FIN carrying the receiver's
// context pointer -- ordering the notification behind the payload by waiting
// for the write rather than by trusting the network to order two operations.
static void notifyDeviceRestagePut(int dest_pe, void* dest_op)
{
  auto* m = (DeviceRestagePutDone*)CmiAlloc(sizeof(DeviceRestagePutDone));
  CmiEnforce(m);
  m->dest_op = dest_op;
  // No QdCreate: the count raised before the put is still outstanding and is
  // handed to this message, so quiescence stays blocked across the whole
  // correction rather than reopening between the write and the notification.
  CmiSetHandler(m, device_restage_put_done_handler);
  CmiSyncSendAndFree(dest_pe, sizeof(DeviceRestagePutDone), (char*)m);
}

// Receiver side, inter-node put: the payload is already in the destination
// buffer, so the op only has to be resolved.
extern "C" void* device_restage_put_done_bridge(void* arg)
{
  QdProcess(1);
  auto* m = (DeviceRestagePutDone*)arg;
  // No stand-down release here: it happens when the completion message is
  // freed (CkRdmaDeviceMsgFreed), the same as an uncorrected receive, so a
  // corrected op must not release a second time.
  CkRdmaDeviceRecvHandler(m->dest_op, NULL);
  CmiFree(m);
  return NULL;
}

// Receiver side: defer this buffer and ask the sender to retransmit it.
// What the receiver saw in the descriptor when it asked for a correction;
// only read by the CHARM_ZC_RESTAGE_DEBUG print.
static thread_local bool src_prepared_dbg = false;
static thread_local int src_proto_dbg = 0;
static thread_local int src_dev_idx_dbg = -1;

// An inter-node restage put may only be issued once the kernel that produced
// the source has actually finished: the NIC reads that buffer outside every
// CUDA stream, so a stream wait -- which orders CUDA work against CUDA work --
// cannot hold it back, and a put issued early ships whatever the buffer held
// before. That ordering is required. Blocking the PE on it is not, and
// cudaEventSynchronize did exactly that: the PE stopped scheduling until the
// GPU drained, once per correction, in an LB step that makes hundreds of them.
//
// The wait is enqueued on this PE's restage stream instead, and the put issued
// from that stream's HAPI callback. hapiAddCallback delivers it to the PE that
// registered it (a converse message on light_cb_idx_), so the put is still
// issued by this PE, in the same order relative to its own work -- the PE just
// keeps launching and draining in the meantime. Distinct corrections are
// independent: each carries its own dest_op and its own registered landing
// buffer, and completion is resolved per op by the notification, so letting
// them overlap changes nothing about how any one of them resolves.
struct DeviceRestagePutCtx
{
  DeviceRestageReq* req;
  const void* src_ptr;
};

static void issueDeviceRestagePut(DeviceRestageReq* req, const void* src_ptr)
{
  // Put with notification. The NACK already carried the receiver's registered
  // landing buffer, so the payload goes straight there: one RDMA write from
  // the side that already owns the source registration.
  //
  // CMK_DEVICE_RESTAGE_PUT marks the operation so that its completion --
  // which a write raises on the initiator, here the sender -- is turned into
  // a notification to the target instead of being resolved locally. Without
  // that the receiver is never told, and the run stalls with the bytes
  // already in place and zero load balancing steps.
  CmiSetDirectNcpyAckHandler(CkRdmaDeviceRecvHandler);
  CmiNcpyBuffer src_ncpy(src_ptr, req->cnt);
  NcpyOperationInfo* info = src_ncpy.createNcpyOpInfo(
      src_ncpy, req->dest_ncpy, /*ackSize=*/0, NULL, NULL, /*rootNode=*/-1,
      CMK_DEVICE_RESTAGE_PUT, NULL);
  QdCreate(1);   // released by device_restage_put_done_bridge
  CmiIssueRput(info);
}

// The producer has finished; issue the put and retire the request that was
// held across the wait. Runs on the PE that enqueued the wait.
static void deviceRestagePutReady(void* arg, void*)
{
  DeviceRestagePutCtx* ctx = (DeviceRestagePutCtx*)arg;
  issueDeviceRestagePut(ctx->req, ctx->src_ptr);
  CmiFree(ctx->req);
  delete ctx;
}

static void requestDeviceRestage(int srcPe, void* dest_op, const void* src_ptr,
                                 CkGroupID dest_aid, CmiUInt8 dest_id,
                                 bool src_staged, size_t src_comm_offset,
                                 const void* src_event, int src_event_idx,
                                 int src_flag_rank, uint32_t src_flag_seq,
                                 size_t cnt, void* dest_ptr, size_t dest_cnt,
                                 bool inter_node)
{
  DeviceRestageReq* req = (DeviceRestageReq*)CmiAlloc(sizeof(DeviceRestageReq));
  CmiEnforce(req);
  req->dest_op = dest_op;
  req->dest_pe = CkMyPe();
  // Stand this element's migration down until the correction lands: the
  // destination below is captured now and written a round trip later.
  // (The stand-down for this op was already taken when its transfers were
  // issued -- every receive is counted now, deferred or not -- so taking
  // another here would need a second release.)
  req->src_ptr = src_ptr;
  // A staged send hands the bytes to the comm buffer and releases the chare's
  // own source buffer, so src_ptr is only trustworthy when the sender staged
  // nothing. Carry the staged location back too: comm_offset is node-local,
  // meaningless where this request is built, and exact on the PE it is being
  // sent to -- the one that staged it.
  //
  // Keyed on the protocol, not on device_idx: a DIRECT send sets device_idx
  // exactly as a staged one does but stages nothing, leaving comm_offset at 0.
  // Reading that would hand back the base of the comm buffer -- some other
  // transfer's bytes.
  req->src_staged = src_staged;
  req->src_comm_offset = src_comm_offset;
  req->src_event = src_event;
  req->src_event_idx = src_event_idx;
  req->src_flag_rank = src_flag_rank;
  req->src_flag_seq = src_flag_seq;
  req->cnt = cnt;
  req->inter_node = inter_node;
  if (inter_node) {
    // Register the landing buffer here and hand the sender its descriptor, so
    // the put can be addressed straight at it. The buffer also carries this
    // receive's DeviceRdmaOp, which rides along in the operation info and comes
    // back in the sender's notification as the name of what to resolve.
    req->dest_ncpy = CmiNcpyBuffer(dest_ptr, dest_cnt, dest_op);
  }
  // CHARM_ZC_RESTAGE_DEBUG: a pass on the migration paths only means something
  // if this path actually ran, so make it countable rather than inferred.
  static const bool restage_debug = (getenv("CHARM_ZC_RESTAGE_DEBUG") != nullptr);
  if (restage_debug) {
    static std::atomic<unsigned> n{0};
    CmiPrintf("[%d] ZC RESTAGE #%u srcPe=%d cnt=%zu id=%llu %s "
              "(sender_prepared=%d proto=%d dev_idx=%d src_staged=%d)\n", CkMyPe(),
              n.fetch_add(1, std::memory_order_relaxed) + 1, srcPe, cnt,
              (unsigned long long)dest_id,
              inter_node ? "inter-node put" : "same-node stage",
              (int)src_prepared_dbg, (int)src_proto_dbg, src_dev_idx_dbg,
              (int)src_staged);
    fflush(stdout);
  }

  // Balances the QdProcess in the bridge: the transfer is still outstanding
  // across the round trip, and quiescence must not fire in the middle of it.
  QdCreate(1);
  CmiSetHandler(req, device_restage_req_handler);
  CmiSyncSendAndFree(srcPe, sizeof(DeviceRestageReq), (char*)req);
}

static void deviceRestageFlagPoll(void* arg)
{
  device_restage_req_bridge(arg);
}

extern "C" void* device_restage_req_bridge(void* arg)
{
  DeviceRestageReq* req = (DeviceRestageReq*)arg;
  // The normal memcpy sender marked its stream with a pinned flag and recorded
  // no event; until it lands the bytes are not there to re-read. A flag has no
  // callback, so poll from the scheduler loop. The request's QdCreate keeps
  // holding quiescence until the pass below balances it.
  if (req->src_flag_seq != 0 &&
      !hapiFlagLanded(req->src_flag_rank, req->src_flag_seq)) {
    CcdCallOnCondition(CcdSCHEDLOOP, deviceRestageFlagPoll, req);
    return NULL;
  }
  QdProcess(1);
  GPUManager& csv_gpu_manager = CsvAccess(gpu_manager);

  // Read back whatever this PE still owns. A staged send copied the payload
  // into the comm buffer and let the chare reuse its own buffer, so where one
  // was staged the comm buffer holds the only copy that is still the payload
  // this transfer promised; src_ptr may already carry the next iteration.
  const void* src_ptr = req->src_ptr;
  if (req->src_staged && csv_gpu_manager.use_shm) {
    DeviceManager* src_dm = csv_gpu_manager.device_map[CkMyPe()];
    src_ptr = (const char*)src_dm->comm_buffer->base_ptr + req->src_comm_offset;
  }

  // What produced these bytes has not necessarily run yet. The original send
  // never read the source itself -- it recorded an event on the application's
  // stream and let the receiver wait on it -- so the payload is only ordered
  // behind the packing kernel (or the staging copy) through that event. The
  // re-read below runs on hapiStreamPerThread, which is ordered against neither
  // the application's stream nor anything else here, so without re-imposing it
  // the correction can copy a slice the kernel has not filled and ship the
  // previous contents of a buffer the application rotates. That lands as a
  // plausible but wrong ghost in one element, far from here.
  //
  // A null event on the unstaged path means the sender blocked at send time
  // (no event was free), so the data is already there.
  hapiEvent_t src_ready = NULL;
  bool src_needs_full_sync = false;
  if (req->src_staged && csv_gpu_manager.use_shm) {
    if (req->src_event_idx >= 0) {
      const int src_dev_idx = csv_gpu_manager.device_count * CmiMyNodeRankLocal()
                            + CpvAccess(my_device_id);
      src_ready = csv_gpu_manager.hapi_ipc_device_infos[src_dev_idx]
                      .src_event_pool[req->src_event_idx];
    } else {
      src_needs_full_sync = true;
    }
  } else if (req->src_event != NULL) {
    src_ready = (hapiEvent_t)req->src_event;
  }

  if (req->inter_node) {
    // See issueDeviceRestagePut: the production has to be COMPLETE before the
    // write is issued, not merely ordered behind it, because the NIC reads
    // outside every stream. Observe that without stopping the PE -- enqueue the
    // wait and issue the put from the stream's callback. hapiAddCallback holds
    // quiescence for the whole gap, so nothing can complete out from under it.
    if (src_ready) {
      hapiCheck(hapiStreamWaitEvent(restageWaitStream(), src_ready, 0));
      DeviceRestagePutCtx* ctx = new DeviceRestagePutCtx{req, src_ptr};
      hapiAddCallback(restageWaitStream(), CkCallback(deviceRestagePutReady, ctx));
      return NULL;  // ctx owns req until the callback issues the put
    }
    // No event to wait on, and the staged path means something produced these
    // bytes on a stream this PE can no longer name -- the sender found no free
    // IPC event slot. There is nothing to enqueue a wait against, so this one
    // case still drains the device. It fires only under slot exhaustion; the
    // fix for it is a free slot, not a different wait.
    if (src_needs_full_sync) { hapiSubmitDrain(); hapiCheck(hapiDeviceSynchronize()); }
    // Otherwise the sender blocked at send time (no event was free on the
    // unstaged path), so the bytes are already there.
    issueDeviceRestagePut(req, src_ptr);
  } else if (hapiDevPoolOn()) {
    // +gpupool: nothing to stage into, and no need. The only same-node
    // correction left is the memcpy-chosen send whose target moved to another
    // process, and the sender still owns that source. Export it and let the
    // receiver read it in place, exactly as a first-time direct send would.
    DeviceManager* dm = csv_gpu_manager.device_map[CkMyPe()];
    const int cpv_my_device_id = CpvAccess(my_device_id);
    hapiIpcMemHandle_t handle;
    size_t export_offset = 0;
    void* export_base = NULL;
    if (!hapiIpcExportBuffer(src_ptr, &handle, &export_offset, &export_base)) {
      CkAbort("PE %d: restage to PE %d could not export the %zu-byte source at "
              "%p for direct CUDA IPC (%s). Under +gpupool every device buffer "
              "sent must be cudaMalloc-backed.",
              CkMyPe(), req->dest_pe, req->cnt, src_ptr,
              hapiIpcLastImportErrorName());
    }
    void* unused = NULL;
    int event_idx = -1;
    acquireIpcSendSlot(dm, cpv_my_device_id, /*is_lb_buffer=*/false,
                       /*direct=*/true, src_ptr, req->cnt, &unused, &event_idx);
    // The receiver waits on this event before it reads; it has to sit behind
    // whatever produced the source, which src_ready marks.
    if (src_ready)
      hapiCheck(hapiStreamWaitEvent(hapiStreamPerThread, src_ready, 0));
    else if (src_needs_full_sync)
      { hapiSubmitDrain(); hapiCheck(hapiDeviceSynchronize()); }
    const int device_idx =
        csv_gpu_manager.device_count * CmiMyNodeRankLocal() + cpv_my_device_id;
    hapi_ipc_device_info& my_device_info =
        csv_gpu_manager.hapi_ipc_device_infos[device_idx];
    hapiCheck(hapiEventRecord(my_device_info.src_event_pool[event_idx],
                              hapiStreamPerThread));
    ipcPublishSrcReady(device_idx, event_idx, hapiStreamPerThread);

    DeviceRestageMeta* m = (DeviceRestageMeta*)CmiAlloc(sizeof(DeviceRestageMeta));
    CmiEnforce(m);
    m->dest_op = req->dest_op;
    m->device_idx = device_idx;
    m->event_idx = event_idx;
    m->comm_offset = 0;
    m->cnt = req->cnt;
    m->protocol = CmiIpcProtocol::DIRECT;
    m->ipc_handle = handle;
    m->ipc_base = export_base;
    m->ipc_offset = export_offset;
    QdCreate(1);
    CmiSetHandler(m, device_restage_meta_handler);
    CmiSyncSendAndFree(req->dest_pe, sizeof(DeviceRestageMeta), (char*)m);
  } else {
    DeviceManager* dm = csv_gpu_manager.device_map[CkMyPe()];
    const int cpv_my_device_id = CpvAccess(my_device_id);
    void* staged = NULL;
    int event_idx = -1;
    acquireIpcSendSlot(dm, cpv_my_device_id, /*is_lb_buffer=*/false,
                       /*direct=*/false, src_ptr, req->cnt, &staged,
                       &event_idx);
    if (src_ready)
      hapiCheck(hapiStreamWaitEvent(hapiStreamPerThread, src_ready, 0));
    else if (src_needs_full_sync)
      { hapiSubmitDrain(); hapiCheck(hapiDeviceSynchronize()); }

    hapiCheck(hapiMemcpyAsync(staged, src_ptr, req->cnt,
                              hapiMemcpyDefault, hapiStreamPerThread));

    const int device_idx =
        csv_gpu_manager.device_count * CmiMyNodeRankLocal() + cpv_my_device_id;
    hapi_ipc_device_info& my_device_info =
        csv_gpu_manager.hapi_ipc_device_infos[device_idx];
    hapiCheck(hapiEventRecord(my_device_info.src_event_pool[event_idx],
                              hapiStreamPerThread));
    ipcPublishSrcReady(device_idx, event_idx, hapiStreamPerThread);

    // Retire the ORIGINAL send's slot, now that its bytes have been re-read.
    //
    // The deferral could not do this. Where the original send staged, the comm
    // buffer holds the only copy of the payload still owed -- the chare's own
    // buffer may already carry the next iteration -- so the block has to stay
    // intact until the memcpy above has read it. Releasing at the deferral site
    // would hand the block back while it was still the source.
    //
    // Nothing released it afterwards either, so every correction burned one
    // event slot of the per-PE slice for the life of the run, and one comm
    // block with it. Single-digit correction counts kept that invisible until
    // unmappable direct sends started routing through here in volume, at which
    // point the pic2d repro needed +gpuipceventpool 2048 where 16 had done.
    //
    // The original slot's block, if it has one, was written by the sender's
    // own staging copy, whose completion its src_ready publication (a gate on
    // reclaim) attests; the failed delivery never read it. The flag alone
    // releases it.
    if (req->src_event_idx >= 0) {
      hapi_ipc_event_shared* orig_shared =
          (hapi_ipc_event_shared*)((char*)csv_gpu_manager.shm_ptr
              + csv_gpu_manager.shm_chunk_size * device_idx
              + sizeof(hapiIpcMemHandle_t)) + req->src_event_idx;
      orig_shared->dst_flag.store(true, std::memory_order_release);
    }

    // Deliberately no completion callback for the source buffer here. The
    // sender chose memcpy, so it shipped the real CkCallback to the receiver
    // rather than firing it itself as a staged send would; the receiver still
    // holds it in save_op.src_cb and fires it on completion. Firing it here as
    // well would release the application's buffer twice.
    DeviceRestageMeta* m = (DeviceRestageMeta*)CmiAlloc(sizeof(DeviceRestageMeta));
    CmiEnforce(m);
    m->dest_op = req->dest_op;
    m->device_idx = device_idx;
    m->event_idx = event_idx;
    m->comm_offset = (char*)staged - (char*)dm->comm_buffer->base_ptr;
    m->cnt = req->cnt;
    m->protocol = CmiIpcProtocol::STAGED;
    m->ipc_base = NULL;
    m->ipc_offset = 0;
    QdCreate(1);
    CmiSetHandler(m, device_restage_meta_handler);
    CmiSyncSendAndFree(req->dest_pe, sizeof(DeviceRestageMeta), (char*)m);
  }
  CmiFree(req);
  return NULL;
}

// Completion of a same-node corrected receive: the restaged bytes are now in
// the destination buffer. Runs on the destination PE via hapiAddCallback, the
// same context deviceSendReleaseFn already uses. No stand-down release here:
// it happens when the completion message is freed (CkRdmaDeviceMsgFreed), the
// same as an uncorrected receive, so a corrected op must not release twice.
static void deviceRestageRecvDone(void* data, void* msg)
{
  CkRdmaDeviceRecvHandler(data, msg);
}

// Receiver side, same node: the sender has staged the payload, so run the
// ordinary staged-IPC receive against the metadata it just sent.
extern "C" void* device_restage_meta_bridge(void* arg)
{
  QdProcess(1);
  DeviceRestageMeta* m = (DeviceRestageMeta*)arg;
  DeviceRdmaOp* op = (DeviceRdmaOp*)m->dest_op;
  // The stand-down taken at requestDeviceRestage is NOT lifted here. The
  // copy below has not even been issued yet: releasing the element now opens
  // a stream-latency window in which it can emigrate, pack its grid off
  // migration_stream -- which is not ordered against recv_stream -- and leave
  // with the pre-correction bytes while the copy lands in the buffer the
  // departed element abandoned (or in freed device memory). The inter-node
  // path may resolve on arrival of the put-done because the payload is
  // already in place; here that is only true once recv_stream has executed
  // the copy, so the note moves to the completion callback.

  CkDeviceBuffer source;
  source.ptr = NULL;                 // never dereferenced on a staged receive
  source.cnt = m->cnt;
  source.device_idx = m->device_idx;
  source.event_idx = m->event_idx;
  source.comm_offset = m->comm_offset;
  source.ipc_protocol = m->protocol;
  source.ipc_handle = m->ipc_handle;
  source.ipc_base = m->ipc_base;
  source.ipc_offset = m->ipc_offset;
  source.sender_prepared = true;

  CkDeviceBuffer dest(op->dest_ptr, op->size);

  // The same gate as a first-time receive: the retransmit's producer (the
  // sender's staging copy, or its original kernels for a direct export) and
  // the destination condition the original post recorded in the op.
  DeviceRecvPending pr;
  pr.op = op;
  pr.source = source;
  pr.dest = dest;
  pr.mode = CkNcpyModeDevice::IPC;
  pr.src_pe = op->src_pe;
  pr.kind = DeviceRecvKind::Ipc;
  pr.done = deviceRestageRecvDone;
  deviceRecvTryIssue(std::move(pr));

  CmiFree(m);
  return NULL;
}

// ---- Stalled-receive watchdog (CHARM_ZC_STALL_SECS) -----------------------
//
// A device receive releases its message only on counter == n_ops, so a single
// op that never completes wedges that message for good: the entry method never
// runs, the element never advances, and every PE ends up waiting at the load
// balancing barrier with no error anywhere. That is what every stall this code
// has produced looks like from the outside, and it is invisible to a debugger
// attached after the fact -- the stack just shows an idle scheduler.
//
// So make the receive say so itself. Every in-flight DeviceRdmaInfo is
// registered here with the time it was posted; once a second, anything older
// than the threshold is printed with the ops it is still waiting on. A stall
// then names the op that never completed -- which buffer, from which PE, and
// whether it was deferred for correction -- on whatever run happens to hit it,
// with nothing to attach and nothing to reproduce on demand.

struct DeviceRecvWatch {
  DeviceRdmaInfo* info;
  double posted;
  int numops;
  std::vector<int> src_pe;
  std::vector<size_t> size;
  std::vector<char> deferred;   // asked the sender to send it again
  // IPC identity of each op's transfer, when it went through the shm path
  // (-1 otherwise): lets the scan query the actual CUDA events of a stuck op
  // and say WHICH side's stream never fired -- src_event pending means the
  // sender's staging copy never executed; src done with dst pending means the
  // receiver's copy is parked behind something else on its stream. Slots are
  // only reclaimed after dst_event completes, so for a genuinely stuck op the
  // indices still name the live pair.
  std::vector<int> dev_idx;
  std::vector<int> ev_idx;
  bool reported;
};

CkpvDeclare(std::vector<DeviceRecvWatch>*, device_recv_watch);

static double deviceStallSecs()
{
  static const double s = []() {
    const char* e = getenv("CHARM_ZC_STALL_SECS");
    return e ? atof(e) : 0.0;   // 0 disables
  }();
  return s;
}

// Drop a resolved receive's watch entries before its DeviceRdmaInfo is freed.
// The scan otherwise reads the freed struct on its next pass -- observed as
// stall reports with garbage n_ops in the billions. Resolve and scan run on
// the same PE's scheduler, so erasing here fully closes the race.
static void deviceRecvWatchDrop(DeviceRdmaInfo* info)
{
  auto* w = CkpvAccess(device_recv_watch);
  if (w == NULL) return;
  for (auto it = w->begin(); it != w->end();) {
    if (it->info == info) it = w->erase(it);
    else ++it;
  }
}

static void deviceStallScan(void*, double)
{
  auto* w = CkpvAccess(device_recv_watch);
  if (w == NULL) return;
  const double now = CkWallTimer();
  const double limit = deviceStallSecs();
  for (auto it = w->begin(); it != w->end();) {
    if (it->info->counter >= it->info->n_ops) {
      it = w->erase(it);
      continue;
    }
    if (!it->reported && now - it->posted > limit) {
      CmiPrintf("[%d] ZC STALL: receive stuck %.0fs, %d of %d ops complete\n",
                CkMyPe(), now - it->posted, it->info->counter, it->info->n_ops);
      GPUManager& gm = CsvAccess(gpu_manager);
      for (int i = 0; i < it->numops; i++) {
        char ev_state[96] = "";
        const int di = it->dev_idx[i], ei = it->ev_idx[i];
        if (gm.use_shm && di >= 0 && ei >= 0 &&
            (size_t)di < gm.hapi_ipc_device_infos.size()) {
          hapi_ipc_device_info& dinfo = gm.hapi_ipc_device_infos[di];
          if ((size_t)ei < dinfo.src_event_pool.size()) {
            const bool src_done =
                (hapiEventQuery(dinfo.src_event_pool[ei]) == hapiSuccess);
            hapi_ipc_event_shared* sh =
                (hapi_ipc_event_shared*)((char*)gm.shm_ptr
                    + gm.shm_chunk_size * di + sizeof(hapiIpcMemHandle_t)) + ei;
            snprintf(ev_state, sizeof(ev_state),
                     " dev=%d ev=%d src_ev=%s src_ready=%d dst_done=%d", di, ei,
                     src_done ? "DONE" : "PENDING",
                     (int)sh->src_ready.load(std::memory_order_relaxed),
                     (int)sh->dst_flag.load(std::memory_order_relaxed));
          }
        }
        CmiPrintf("[%d]   op %d: srcPe=%d bytes=%zu %s%s\n", CkMyPe(), i,
                  it->src_pe[i], it->size[i],
                  it->deferred[i] ? "DEFERRED for correction" : "ordinary",
                  ev_state);
      }
      fflush(stdout);
      it->reported = true;
    }
    ++it;
  }
}

void CkRdmaDeviceStallWatchInit()
{
  CkpvInitialize(std::vector<DeviceRecvWatch>*, device_recv_watch);
  CkpvAccess(device_recv_watch) = NULL;
  if (deviceStallSecs() <= 0.0) return;
  CkpvAccess(device_recv_watch) = new std::vector<DeviceRecvWatch>();
  CcdCallOnConditionKeep(CcdPERIODIC_1second, (CcdCondFn)deviceStallScan, NULL);
}

void CkRdmaDeviceIssueRgets(envelope *env, int numops, void **arrPtrs, int *arrSizes, CkDeviceBufferPost *postStructs) {
  ZcHostTimer zc_host_timer(zc_mode_stats.host_recv_us, zc_mode_stats.host_recv_n);
  // The element every op of this message is addressed to, or NULL for a
  // message with no element behind it (a migration payload arriving on a
  // group entry -- asking such an envelope for an ArrayID aborts, hence the
  // guard). Resolved once here: admission control, the per-op migration
  // stand-down, and the receive-side registration cache all name the same
  // recipient.
  CkLocRec* recv_elt = (env->getMsgtype() == ForArrayEltMsg)
      ? CkFindDeviceRecvElement(env->getArrayMgr(), env->getRecipientID())
      : NULL;
#if CMK_LBDB_ON
  // Admission control against a departing element. Preprocessing this message
  // pins it to this process: the transfers land in buffers the element just
  // posted here, and the completion message carries pointers into this
  // address space. An element with a migration pending must therefore stop
  // admitting receives -- what it has in flight drains, its step stalls
  // waiting on the receives we hold back, the counter reaches zero, and it
  // leaves. Without this a continuously-receiving element never goes quiet
  // and the deferred migration starves, which wedges any balancer that waits
  // for its moves to finish. The requeued envelope is untouched (the bail is
  // before any mutation), so once the element departs, ordinary delivery
  // forwards it and the new host preprocesses it with local pointers.
  //
  // Only a parked element WITH A MIGRATION PENDING bounces, and both halves
  // of that condition are load bearing. An element with a migration pending
  // that is still running must keep admitting: it needs its current round's
  // receives to consume what it already accepted and to keep feeding its
  // neighbours, and it parks only once quiet -- freezing it at its first
  // quiet moment mid-run stalls its neighbours on missing ghosts and
  // deadlocks the step (measured, not hypothetical). A parked element with
  // no migration decision admits normally: buffering returns before the
  // transfer is issued, so the sender's completion callback never fires, and
  // an application that gates its step on "every zerocopy send I issued has
  // completed" (it must, since the next iteration repacks those buffers)
  // then hangs at that gate forever, never reaches its own park, and nothing
  // ever unparks and replays what was buffered. Measured: one element stuck
  // at its send drain, two neighbours holding 7 of 8 particle messages, and
  // sixty-one elements parked behind them.
  // The pendingMigrateTo condition used to be here too, so a device message
  // arriving at a PARKED element whose destination the strategy had not yet
  // chosen went down the normal delivery path. The element is parked, so its
  // `when` is not ready, and the SDAG buffers the message -- and if the
  // strategy then decides to move that element, _sdag_pup serialises a
  // buffered nocopydevice payload whose pointer is process-local. pic2d's own
  // pup comment states the invariant this violates: "no device-zerocopy
  // message ... is buffered here at migration time". The window is between the
  // park (cklocation.C, AtSyncWait sets deviceRecvParked) and the migration
  // decision (pendingMigrateTo, set when the strategy's move arrives), which
  // is why the wedge is nondeterministic and why it hits sync LB too -- AtSync
  // is AtSyncStart + AtSyncWait and parks just the same.
  //
  // Buffering for the whole park is safe now in a way it was not when this was
  // narrowed: that narrowing predates the switch from REQUEUE to BUFFER below.
  // A requeue spun on the local queue and starved the resume broadcast, so the
  // sender's completion never fired; the buffer path takes a reference and
  // replays on unpark or departure, so the sender completes at replay.
  if (recv_elt != NULL && (recv_elt->deviceRecvParked || recv_elt->migrateKickHeld)) {
    // Buffered, NOT requeued: a requeue spins on the scheduler's local
    // queue, which is popped ahead of the network -- so the resume
    // broadcast that would unpark this element starves behind its own
    // bounced messages, and the wedge sustains itself. Buffer the envelope
    // and replay it when the element unparks or departs.
    //
    // The delivery framework owns this envelope: every marshalled entry is
    // noKeep, so CkDeliverMessageFree frees the message the moment the
    // entry invocation this bail returns into completes -- the normal path
    // below survives that only by copying into new_env. Take a reference
    // so the buffered envelope stays alive until replay re-injects it;
    // redelivery then admits it (a copy is made and this reference is the
    // one the framework frees), bounces it again (another reference is
    // taken here), or forwards it to the element's new home (the send path
    // frees it after transmission).
    CmiReference((void*)env);
    CkDeviceRecvAdmissionBuffer(
        ck::ObjID(env->getRecipientID()).getElementID(), (void*)env);
    return;
  }
#endif
  // Change message header to invoke regular entry method
  CMI_ZC_MSGTYPE(env) = CMK_REG_NO_ZC_MSG;

  // Create a copy of this message for regular entry method invocation
  // FIXME: Reuse the old message instead of creating a new one
  void* old_msg = EnvToUsr(env);
  envelope* new_env = UsrToEnv(CkCopyMsg(&old_msg));

  // Retarget the copied message's device buffers to the buffers this receiver
  // posted. The transfers below land in arrPtrs[], but the copy still carries
  // the SENDER's CkDeviceBuffer::ptr, and the entry method delivered from it
  // reads that pointer as its data. Within one process the sender's pointer is
  // a valid local address holding the same bytes, so this went unnoticed;
  // across processes it names memory in another address space and the first
  // kernel touching it faults. Rewriting in place is safe because pupping a
  // CkDeviceBuffer is fixed-width -- only ptr changes value.
  {
    char* new_buf = ((CkMarshallMsg*)EnvToUsr(new_env))->msgBuf;
    PUP::fromMem walk(new_buf);
    int copy_numops;
    walk | copy_numops;
    for (int i = 0; i < numops && i < copy_numops; i++) {
      const size_t field_off = walk.size();
      CkDeviceBuffer db;
      walk | db;
      const size_t field_len = walk.size() - field_off;
      db.ptr = arrPtrs[i];
      PUP::toMem patch(new_buf + field_off);
      patch | db;
      // Rewriting in place is only sound if packing a CkDeviceBuffer produces
      // exactly as many bytes as unpacking consumed. If that ever stops being
      // true, this would silently overwrite the marshalled parameters that
      // follow (ref/dir/n) instead of just the pointer.
      if (patch.size() != field_len)
        CkAbort("CkRdmaDeviceIssueRgets: device buffer pup asymmetry "
                "(read %zu bytes, wrote %zu) -- in-place retarget is unsafe",
                field_len, patch.size());
    }
  }

  // Start unpacking marshalled message
  PUP::fromMem up((void *)((CkMarshallMsg *)EnvToUsr(env))->msgBuf);
  int received_numops;
  up|received_numops;
  CkAssert(numops == received_numops);

  CkDeviceBuffer source;

  // Machine layer does not support GPU-aware communication
  GPUManager& csv_gpu_manager = CsvAccess(gpu_manager);

  // Find which mode of transfer should be used
  // CmiPrintf("[%d] CkRdmaDeviceOnSender: src_pe=%d, dst_pe=%d\n", CkMyPe(), env->getSrcPe(), CkMyPe());
  CkNcpyModeDevice mode = findTransferModeDevice(env->getSrcPe(), CkMyPe());

  // Allocate and fill in metadata for this zerocopy operation
  void* rdma_data = CmiAlloc(sizeof(DeviceRdmaInfo) + sizeof(DeviceRdmaOp) * numops);
  CmiEnforce(rdma_data);
  DeviceRdmaInfo* rdma_info = (DeviceRdmaInfo*)rdma_data;
  rdma_info->n_ops = numops;
  // Timed tally: zero posted time means "not measuring", so the completion path
  // can tell an untimed receive from one that completed instantly.
  rdma_info->zc_posted = zcStatsOn() ? CkWallTimer() : 0.0;
  rdma_info->zc_bytes = 0;
  rdma_info->zc_mode = 2;
  rdma_info->counter = 0;
  rdma_info->msg = new_env;

  // Watchdog bookkeeping; no cost unless CHARM_ZC_STALL_SECS is set.
  DeviceRecvWatch* watch = NULL;
  if (CkpvAccess(device_recv_watch) != NULL) {
    CkpvAccess(device_recv_watch)->push_back(
        DeviceRecvWatch{rdma_info, CkWallTimer(), numops,
                        std::vector<int>(numops, -1),
                        std::vector<size_t>(numops, 0),
                        std::vector<char>(numops, 0),
                        std::vector<int>(numops, -1),
                        std::vector<int>(numops, -1), false});
    watch = &CkpvAccess(device_recv_watch)->back();
  }

  // Set when the last op of this receive is an empty one, which completes
  // here rather than on a stream callback: nothing else will come back to
  // release the metadata, so this function has to.
  bool completed_inline = false;

  for (int i = 0; i < numops; i++) {
    // Unpack source buffer from sender
    up|source;

    if (arrSizes[i] > source.cnt) {
      CkAbort("CkRdmaDeviceIssueRgets: posted data size is larger than source data size!");
    }

    // Store information about this buffer
    DeviceRdmaOp& save_op = *(DeviceRdmaOp*)((char*)rdma_data
        + sizeof(DeviceRdmaInfo) + sizeof(DeviceRdmaOp) * i);
    // Use the PE we are actually running on, not the one the sender recorded.
    // If the target chare migrated after the sender posted this transfer, the
    // sender's dest_pe is stale; the message itself has already been routed
    // here by the location manager, so this PE is the one that hosts the chare
    // and that posted arrPtrs below.
    save_op.dest_pe  = CkMyPe();
    save_op.dest_ptr = arrPtrs[i];
    save_op.size = (size_t)arrSizes[i];
    save_op.info = rdma_info;
    // rdma_data comes from CmiAlloc, so this is uninitialised storage: without
    // clearing it, a stale non-zero would be read as an rget timestamp and
    // charge a nonsense interval to the cross-node tier. Only the rget branch
    // sets it.
    save_op.rget_posted = 0.0;
    // DIAGNOSTIC: what buffer did this receive post, for which element, and in
    // which mode. Correlated against the application's own record of which
    // ghost it later reads out of that buffer, this shows whether the runtime
    // ever lands one message's payload in another message's posted buffer.
    save_op.src_pe = env->getSrcPe();
    // Only an array-element message names an element; the migration payload
    // arrives on a group entry, and asking it for an ArrayID aborts. Those
    // receives are never deferred for correction, so -1 simply means "no
    // element to stand down".
    if (env->getMsgtype() == ForArrayEltMsg) {
      save_op.dest_aid_idx = env->getArrayMgr().idx;
      save_op.dest_id = env->getRecipientID();
      // Stand the receiving element's migration down for EVERY in-flight
      // receive, not only the deferred ones. The transfers below land in the
      // buffers this element just posted, and the completion message carries
      // pointers into this process; an element that migrates mid-flight frees
      // those buffers under an active transfer, and the forwarded completion
      // then unpacks a pointer from an address space the new host cannot
      // read. The sender has held its side of every raw-buffer transfer since
      // the interlock was introduced; this is the receiving half.
      if (recv_elt != NULL) {
        recv_elt->noteDeviceSendPosted();
        // Released when the completion message is freed -- consumption, not
        // transfer completion. An SDAG entry can buffer that message for a
        // future round; migrating in that window ships it, still carrying
        // this process's pointers, to a host that cannot read them.
        (*CkpvAccess(device_recv_holds))[(void*)new_env].push_back(
            DeviceRecvHold{save_op.dest_aid_idx, save_op.dest_id});
      }
    } else {
      save_op.dest_aid_idx = -1;
      save_op.dest_id = 0;
    }

    if (watch) {
      watch->src_pe[i] = env->getSrcPe(); watch->size[i] = (size_t)arrSizes[i];
      watch->dev_idx[i] = source.device_idx; watch->ev_idx[i] = source.event_idx;
    }
    save_op.stream = (void*)postStructs[i].hapi_stream;
    save_op.src_cb = (source.cb.type != CkCallback::ignore) ? new CkCallback(source.cb) : nullptr;
    save_op.dst_cb = nullptr;

    // A mismatch here means the target chare migrated between the sender
    // posting this transfer and its arrival. That is expected and safe: the
    // transfer mode above is derived from CkMyPe(), the destination buffers
    // are the ones this PE posted, and the message reached this PE precisely
    // because the location manager knows the chare lives here now. Previously
    // this aborted, which made any migration concurrent with a GPU-direct send
    // fatal.
    if (source.dest_pe != CkMyPe() && _lb_args.debug() > 1) {
      CmiPrintf("[%d] CkRdmaDeviceIssueRgets: sender addressed PE %d (src PE %d); "
                "chare has since migrated here, retargeting.\n",
                CkMyPe(), source.dest_pe, env->getSrcPe());
    }

    // Empty payload: the sender prepared nothing and fired its callback
    // already (CkRdmaDeviceOnSender). Complete the op here, on the host, with
    // no stream wait, copy or event, and deliver once the message's last op
    // is in -- the same count the asynchronous completions drive. There is no
    // QdProcess to balance: nothing was created for this op.
    if (source.cnt == 0) {
      rdma_info->counter++;
      if (rdma_info->counter == rdma_info->n_ops) {
        QdCreate(1);
        enqueueNcpyMessage(CkMyPe(), new_env);
        // The counter can only reach n_ops on the last op there is: every
        // other op has already counted itself, and an op later in this loop
        // has not been unpacked yet. So no save_op below is still live, and
        // the metadata is freed after the loop rather than here only to keep
        // that obvious.
        completed_inline = true;
      }
      continue;
    }

    // The copy lands on the PE's receive stream, issued only once what it
    // depends on is complete (DeviceRecvPending); save_op.stream names it for
    // a transfer that resumes after a correction. What the destination waits
    // for is decided per branch below, before anything later reaches the
    // posted stream.
    save_op.stream = (void*)deviceRecvStream();
    save_op.dst_flag_rank = -1;
    save_op.dst_flag_seq = 0;
    save_op.dst_event = NULL;
    save_op.ipc_device_idx = -1;
    save_op.ipc_event_idx = -1;

    // Destination buffer (on this receiver)
    CkDeviceBuffer dest((const void *)arrPtrs[i], arrSizes[i]);

    // Perform data transfers.
    //
    // `mode` (computed above from the true, always-accurate delivery PEs) is
    // authoritative -- it is never a guess, unlike the sender's transfer_mode
    // decision (see CkRdmaDeviceOnSender), which can be made without knowing
    // the destination. CkRdmaDeviceOnSender's job is to make sure whatever
    // `mode` picks here finds the metadata it needs already staged: it always
    // stages IPC info when it isn't certain MEMCPY suffices and RDMA can be
    // ruled out, so an unconfirmed-destination send still leaves
    // source.device_idx valid for the IPC branch below.
    zcStatsCount(mode);
    // Every op of one receive comes from the same sender and so resolves the
    // same way; recording it per op just avoids a second mode computation.
    rdma_info->zc_bytes += (size_t)arrSizes[i];
    rdma_info->zc_mode = zcModeSlot(mode);

    // CHARM_ZC_VALIDATE: check both pointers before handing them to CUDA. An
    // illegal access raised by the copies below is asynchronous and sticky, so
    // it otherwise surfaces at some unrelated later call and says nothing about
    // which side was bad. Migration is what makes this ambiguous: the source
    // buffer can be freed by an object that migrated away, and the destination
    // is whatever the (possibly just-migrated) receiver posted.
    if (zcValidateOn()) {
      hapiPointerAttributes sattr{}, dattr{};
      const hapiError_t serr = hapiPointerGetAttributes(&sattr, source.ptr);
      const hapiError_t derr = hapiPointerGetAttributes(&dattr, (void*)dest.ptr);
      const bool sbad = (serr != hapiSuccess || sattr.type == hapiMemoryTypeUnregistered);
      const bool dbad = (derr != hapiSuccess || dattr.type == hapiMemoryTypeUnregistered);
      // A staged IPC receive reads the sender's comm buffer through an imported
      // handle, never source.ptr, so source.ptr being unmapped here is the normal
      // cross-process case and says nothing. Only report it when this receive
      // would actually dereference it.
      const bool src_will_be_read =
          !(source.device_idx != -1 && csv_gpu_manager.use_shm);
      if ((sbad && src_will_be_read) || dbad) {
        CmiPrintf("[%d] ZC VALIDATE FAIL mode=%d srcPe=%d src=%p(%s type=%d) "
                  "dst=%p(%s type=%d) cnt=%zu dev_idx=%d\n",
                  CkMyPe(), (int)mode, env->getSrcPe(),
                  source.ptr, sbad ? "BAD" : "ok", (int)sattr.type,
                  (void*)dest.ptr, dbad ? "BAD" : "ok", (int)dattr.type,
                  (size_t)dest.cnt, source.device_idx);
        fflush(stdout);
        hapiGetLastError();  // clear so the report is not itself sticky
      }
    }

    // Prefer whatever the sender actually prepared over the mode derived here.
    // The sender exported this buffer one of two ways -- staged into its
    // communication buffer, or named directly by an IPC handle -- and either
    // arrangement is what the metadata in hand describes, so honour it rather
    // than re-deciding from locality.
    //
    // A staged copy additionally holds the bytes outright, so it survives the
    // sending chare migrating away and freeing its source buffer. A direct one
    // does not: it reads the sender's live allocation, which is why such a send
    // must carry a completion callback the sender waits on before reusing or
    // freeing that buffer.
    // device_idx is node-local -- device_count * CmiMyNodeRankLocal() +
    // my_device_id -- so PE 1 on one node and PE 5 on another both produce 1.
    // Indexing this node's hapi_ipc_device_infos with a remote sender's index
    // resolves to an unrelated local staged block, and the receive silently
    // delivers some other chare's ghost. Measured: staged blocks (dev=1,off=0)
    // were referenced by receives naming srcPEs 1 and 5 in the same run.
    //
    // A sender only stages for a destination it believes is in another process
    // on its own node, so an export arriving from another physical node means
    // the target moved after the mode was chosen. Treat it as unprepared and
    // let the correction path re-fetch the payload by a route that works.
    {
      // A staged landing (CkRdmaDeviceStageParked): the payload is already on
      // this device behind the event the pull recorded. Copy it into the
      // posted buffer on the posting stream and free it behind that copy. The
      // sender was completed by the staging pull; the descriptor carries no
      // callback, so src_cb is null and nothing fires twice.
      StagedLandingMap& landings = *CkpvAccess(staged_landings);
      auto lit = isStagedLandingDescriptor(source) ? landings.find(source.ptr) : landings.end();
      if (lit != landings.end() && (size_t)dest.cnt > lit->second.cnt)
        CkAbort("CkRdmaDeviceIssueRgets: staged landing %p holds %zu bytes but %zu were posted",
                source.ptr, lit->second.cnt, (size_t)dest.cnt);
      if (lit != landings.end()) {
        const StagedLanding sl = lit->second;
        landings.erase(lit);
        if (save_op.src_cb) {  // the sender was completed by the staging pull
          delete (CkCallback*)save_op.src_cb;
          save_op.src_cb = nullptr;
        }
        // The pull's completion (sl.ready) is the source condition here;
        // deviceRecvIssue copies the landing into the posted buffer.
        deviceRecvNoteDestination(save_op, postStructs[i]);
        DeviceRecvPending pr;
        pr.op = &save_op;
        pr.source = source;
        pr.dest = dest;
        pr.mode = mode;
        pr.src_pe = env->getSrcPe();
        pr.kind = DeviceRecvKind::Staged;
        pr.sl = sl;
        pr.done = CkRdmaDeviceRecvHandler;
        deviceRecvTryIssue(std::move(pr));
        continue;
      }
    }
    const bool sender_exported =
        (source.ipc_protocol != CmiIpcProtocol::NONE &&
         source.device_idx != -1 && csv_gpu_manager.use_shm &&
         CmiPeOnSamePhysicalNode(env->getSrcPe(), CkMyPe()));
    const bool sender_direct =
        (sender_exported && source.ipc_protocol == CmiIpcProtocol::DIRECT);

    if (mode == CkNcpyModeDevice::MEMCPY && !sender_exported) {
      // Same process: the copy is issued by deviceRecvIssue once the sender's
      // producing work and the destination's earlier work are both complete.
      deviceRecvNoteDestination(save_op, postStructs[i]);
      DeviceRecvPending pr;
      pr.op = &save_op;
      pr.source = source;
      pr.dest = dest;
      pr.mode = mode;
      pr.src_pe = env->getSrcPe();
      pr.kind = DeviceRecvKind::Memcpy;
      pr.done = CkRdmaDeviceRecvHandler;
      deviceRecvTryIssue(std::move(pr));
    } else if (sender_exported) {
      deviceRecvNoteDestination(save_op, postStructs[i]);
      // A direct send can be unmappable here even though nothing is wrong with
      // it. One cudaIpcOpenMemHandle maps the whole suballocator region behind
      // an allocation, so a second allocation carved from that same region
      // cannot be opened at all -- the driver returns cudaErrorAlreadyMapped --
      // and no API says which held mapping already covers it.
      //
      // This is not a migration effect: it reproduces with load balancing
      // switched off entirely, around iteration 15 of pic2d.
      //
      // So take the route that exists for a payload this process cannot read,
      // the same correction the migration mismatch below uses. Importing here
      // rather than inside deviceIpcReceive is what makes that possible: the
      // mapping has to be known to have failed while the send is still
      // identifiable and the buffer can still be asked for again. A successful
      // import is cached, so the one inside deviceIpcReceive hits.
      if (sender_direct && mode != CkNcpyModeDevice::MEMCPY &&
          hapiIpcImportBuffer(source.ipc_handle, CmiNodeOf(env->getSrcPe()),
                              source.ipc_base,
                              source.ipc_offset + (size_t)dest.cnt) == NULL) {
        if (watch) watch->deferred[i] = 1;
        CkGroupID def_aid; def_aid.idx = save_op.dest_aid_idx;
        src_prepared_dbg = source.sender_prepared;
        src_proto_dbg = (int)source.ipc_protocol;
        src_dev_idx_dbg = source.device_idx;
        requestDeviceRestage(env->getSrcPe(), (void*)&save_op, source.ptr,
                             def_aid, save_op.dest_id,
                             source.ipc_protocol == CmiIpcProtocol::STAGED,
                             source.comm_offset,
                             source.memcpy_event, source.event_idx,
                             source.ready_rank, source.ready_seq,
                             (size_t)dest.cnt, arrPtrs[i], (size_t)arrSizes[i],
                             mode != CkNcpyModeDevice::IPC);
        continue;  // completion deferred until the retransmit lands
      }
      DeviceRecvPending pr;
      pr.op = &save_op;
      pr.source = source;
      pr.dest = dest;
      pr.mode = mode;
      pr.src_pe = env->getSrcPe();
      pr.kind = DeviceRecvKind::Ipc;
      pr.done = CkRdmaDeviceRecvHandler;
      deviceRecvTryIssue(std::move(pr));
    } else {
      // Nothing staged and not same-process. That is legitimate only for a
      // genuine cross-physical-node transfer, where the sender's else-branch
      // registered source.lci_ncpy_buffer for the rdmaGet below.
      //
      // IPC here means the sender resolved this destination to its own process
      // and staged nothing -- and then the target migrated to another process
      // before the message landed. The transfer mode is chosen at send time from
      // the location the sender knows; it only becomes true at delivery. The
      // source buffer lives in an address space this PE cannot read, and
      // source.lci_ncpy_buffer was never registered, so the rdmaGet below would
      // issue against an unregistered descriptor -- which surfaces as an LCI
      // "Message too long" assert or silent corruption, nowhere near the cause.
      //
      // Fail here, where the send is still identifiable, rather than there.
      // The sender prepared nothing readable from another address space, so
      // the target migrated across a process boundary after the mode was
      // chosen. Defer this buffer and ask for the payload again by a route
      // this process can read; see the protocol notes above. The message is
      // already where it belongs -- only the bytes are missing.
      if (!source.sender_prepared ||
          (source.device_idx != -1 &&
           !CmiPeOnSamePhysicalNode(env->getSrcPe(), CkMyPe()))) {
        if (watch) watch->deferred[i] = 1;
        deviceRecvNoteDestination(save_op, postStructs[i]);
        CkGroupID def_aid; def_aid.idx = save_op.dest_aid_idx;
        src_prepared_dbg = source.sender_prepared;
        src_proto_dbg = (int)source.ipc_protocol;
        src_dev_idx_dbg = source.device_idx;
        requestDeviceRestage(env->getSrcPe(), (void*)&save_op, source.ptr,
                             def_aid, save_op.dest_id,
                             source.ipc_protocol == CmiIpcProtocol::STAGED,
                             source.comm_offset,
                             source.memcpy_event, source.event_idx,
                             source.ready_rank, source.ready_seq,
                             (size_t)dest.cnt, arrPtrs[i], (size_t)arrSizes[i],
                             mode != CkNcpyModeDevice::IPC);
        continue;  // completion deferred until the retransmit lands
      }
      // CmiPrintf("it should never be called during intra node\n");
#if CMK_GPU_COMM
      // Machine layer supports GPU-aware communication
      QdCreate(1);
      CmiSetDirectNcpyAckHandler(CkRdmaDeviceRecvHandler);
      // The destination registration is cached against the element this
      // receive is addressed to, whose migration frees the posted buffer and
      // retires the entry. A receive with no element behind it (a migration
      // payload on a group entry) registers per-op as before -- unless the
      // buffer is a pool block, which takes its arena's one registration
      // whoever posted it.
      CmiNcpyBuffer lci_dest_ncpy_buffer;
      void* arena_base_unused = NULL;
      size_t arena_extent_unused = 0;
      const bool in_pool = hapiDevPoolOn() &&
          hapiDevPoolArenaOf(arrPtrs[i], &arena_base_unused, &arena_extent_unused);
      if (recv_elt != NULL || in_pool) {
        lci_dest_ncpy_buffer =
            acquireDeviceRegistration(arrPtrs[i], (size_t)arrSizes[i], recv_elt);
        // What the uncached 3-arg constructor's third argument sets: the
        // completion handler finds this operation's DeviceRdmaOp through it.
        lci_dest_ncpy_buffer.deviceRdmaOpInfo = (void*)(&save_op);
      } else {
        lci_dest_ncpy_buffer =
            CmiNcpyBuffer(arrPtrs[i], (size_t)arrSizes[i], (void*)(&save_op));
      }
      // Stamp the cross-node tier per op; see DeviceRdmaOp::rget_posted.
      save_op.rget_posted = zcStatsOn() ? CkWallTimer() : 0.0;
      lci_dest_ncpy_buffer.rdmaGet(source.lci_ncpy_buffer, 0, nullptr, nullptr);
      continue;
#else
      // Handle all other cases (basic inter-process and inter-node)
      // Transfer the received/unpacked data on host to the destination device buffer
      // FIXME: Print warning that this is slow?
      CkAssert(source.data_stored);
      hapiCheck(hapiMemcpyAsync((void*)dest.ptr, source.data, dest.cnt,
            hapiMemcpyHostToDevice, postStructs[i].hapi_stream));
      hapiAddCallback(postStructs[i].hapi_stream, CkCallback(CkRdmaDeviceRecvHandler, &save_op));
#endif
    }
  }

  // A receive whose every buffer was empty raises no completion: the device
  // was never touched, so no stream callback runs and CkRdmaDeviceRecvHandler,
  // which is what frees this for every other receive, is never reached. Left
  // undone it is one CmiAlloc per message for a stencil that sends to each
  // neighbour every step whether or not it has anything to say -- tens of
  // thousands of messages a second, and the process is killed for memory long
  // before the run ends.
  if (completed_inline) {
    deviceRecvWatchDrop(rdma_info);
    CmiFree(rdma_data);
  }
}

/*************** Staged receive for a message parked at its destination ***************/
//
// A device message that reaches the PE an element is migrating TO before the
// element has landed is parked there (ckarray.C, bufferedIDMsgs) until it
// lands. A direct device send keeps the SENDER's buffer live and its
// outstanding-send count raised until the receiver pulls, so a parked message
// pins its sender for the whole landing. When that sender must itself move,
// or this PE's landing gate is holding memory for that sender's pack, the wait
// is a cycle and nothing lands: with parking alone, every sph2d async run
// wedged at its first balancing step (2026-09-15).
//
// So the payload is pulled NOW, into landing buffers from the device pool,
// with the same per-protocol code the element's own receive would use, and the
// sender is completed the same way (its callback, its IPC slot flag). The
// parked message's descriptors are rewritten in place to name the landing
// buffers; when the element lands and the message is delivered,
// CkRdmaDeviceIssueRgets recognises a landing buffer, copies it into the
// element's posted buffer on the posting stream, and frees it behind the copy.
//
// Left parked as before, with the sender live: a source this PE cannot pull
// directly -- an unprepared sender, an IPC handle that does not import, any
// off-node source (the rget path completes by ack, not on a stream, and is not
// staged in this version) -- and a payload the pool cannot serve.
bool CkRdmaDeviceStageParked(envelope* env)
{
  if (!CMI_IS_ZC_DEVICE(env) || env->getMsgtype() != ForArrayEltMsg) return false;  // host message
  if (!CkDevicePoolOn()) return false;
  GPUManager& csv_gpu_manager = CsvAccess(gpu_manager);
  const int srcPe = env->getSrcPe();
  if (!CmiPeOnSamePhysicalNode(srcPe, CkMyPe())) {
    if (stagedDbg()) CmiPrintf("[STAGE-SKIP %d] source PE %d is off-node\n", CkMyPe(), srcPe);
    return false;
  }
  const CkNcpyModeDevice mode = findTransferModeDevice(srcPe, CkMyPe());
  char* buf = ((CkMarshallMsg*)EnvToUsr(env))->msgBuf;

  // Pass 1: every descriptor, and whether each is pullable from here.
  std::vector<CkDeviceBuffer> src;
  std::vector<size_t> off, width;
  {
    PUP::fromMem rd(buf);
    int n = 0;
    rd | n;
    for (int i = 0; i < n; i++) {
      const size_t o = rd.size();
      CkDeviceBuffer b;
      rd | b;
      src.push_back(b);
      off.push_back(o);
      width.push_back(rd.size() - o);
    }
  }
  StagedLandingMap& landings = *CkpvAccess(staged_landings);
  int live = 0;
  for (size_t i = 0; i < src.size(); i++) {
    const CkDeviceBuffer& b = src[i];
    if (b.cnt == 0) continue;
    if (isStagedLandingDescriptor(b)) {
      if (stagedDbg()) CmiPrintf("[STAGE-SKIP %d] already staged %p\n", CkMyPe(), b.ptr);
      return false;
    }
    const bool sender_exported =
        (b.ipc_protocol != CmiIpcProtocol::NONE && b.device_idx != -1 &&
         csv_gpu_manager.use_shm);
    const bool sender_direct =
        (sender_exported && b.ipc_protocol == CmiIpcProtocol::DIRECT);
    if (mode == CkNcpyModeDevice::MEMCPY && !sender_exported) { live++; continue; }
    if (sender_exported) {
      if (sender_direct && mode != CkNcpyModeDevice::MEMCPY &&
          hapiIpcImportBuffer(b.ipc_handle, CmiNodeOf(srcPe), b.ipc_base,
                              b.ipc_offset + (size_t)b.cnt) == NULL) {
        if (stagedDbg()) CmiPrintf("[STAGE-SKIP %d] IPC handle from PE %d did not import\n", CkMyPe(), srcPe);
        return false;
      }
      live++;
      continue;
    }
    // needs the rget / correction path: leave it to the element
    if (stagedDbg())
      CmiPrintf("[STAGE-SKIP %d] from PE %d: mode=%d proto=%d dev_idx=%d prepared=%d not pullable here\n",
                CkMyPe(), srcPe, (int)mode, (int)b.ipc_protocol, b.device_idx, (int)b.sender_prepared);
    return false;
  }
  if (live == 0) return false;

  // Pass 2: landing buffers, all or nothing.
  std::vector<void*> land(src.size(), nullptr);
  for (size_t i = 0; i < src.size(); i++) {
    if (src[i].cnt == 0) continue;
    land[i] = CkDeviceMalloc(src[i].cnt);
    if (land[i] == nullptr) {
      for (size_t j = 0; j < i; j++) if (land[j]) CkDeviceFree(land[j]);
      if (stagedDbg())
        CmiPrintf("[STAGE-SKIP %d] pool could not serve %zu bytes\n", CkMyPe(),
                  (size_t)src[i].cnt);
      return false;
    }
  }

  // Pass 3: pull, as CkRdmaDeviceIssueRgets would, into the landing buffers.
  const int numops = (int)src.size();
  void* rdma_data = CmiAlloc(sizeof(DeviceRdmaInfo) + sizeof(DeviceRdmaOp) * numops);
  CmiEnforce(rdma_data);
  DeviceRdmaInfo* info = (DeviceRdmaInfo*)rdma_data;
  info->n_ops = live;
  info->counter = 0;
  info->msg = nullptr;   // the mark of a staged pull: nothing to deliver at completion
  info->zc_posted = 0.0;
  info->zc_bytes = 0;
  info->zc_mode = zcModeSlot(mode);
  hapiStream_t st = stagingStream();
  size_t bytes = 0;
  for (int i = 0; i < numops; i++) {
    CkDeviceBuffer& source = src[i];
    if (source.cnt == 0) continue;
    DeviceRdmaOp& op = *(DeviceRdmaOp*)((char*)rdma_data
        + sizeof(DeviceRdmaInfo) + sizeof(DeviceRdmaOp) * i);
    op.stream = (void*)st;
    op.dest_ptr = land[i];
    op.size = (size_t)source.cnt;
    op.info = info;
    op.src_cb = (source.cb.type != CkCallback::ignore) ? new CkCallback(source.cb) : nullptr;
    op.dst_cb = nullptr;
    op.tag = 0;
    op.dest_pe = CkMyPe();
    op.dest_aid_idx = env->getArrayMgr().idx;
    op.dest_id = env->getRecipientID();
    op.src_pe = srcPe;
    op.src_mpi_rank = source.src_mpi_rank;
    op.dest_mpi_rank = CmiMyNode();
    op.dst_flag_rank = -1;
    op.dst_flag_seq = 0;
    op.dst_event = NULL;
    op.ipc_device_idx = -1;
    op.ipc_event_idx = -1;
    CkDeviceBuffer dest((const void*)land[i], source.cnt);
    bytes += (size_t)source.cnt;
    const bool sender_exported =
        (source.ipc_protocol != CmiIpcProtocol::NONE && source.device_idx != -1 &&
         csv_gpu_manager.use_shm);
    // The sender's slot, if it claimed one, is released when the pull
    // completes: CkRdmaDeviceRecvHandler raises its dst_flag.
    if (source.device_idx >= 0 && source.event_idx >= 0 && csv_gpu_manager.use_shm) {
      op.ipc_device_idx = source.device_idx;
      op.ipc_event_idx = source.event_idx;
    }
    if (mode == CkNcpyModeDevice::MEMCPY && !sender_exported) {
      if (source.memcpy_event != NULL)
        hapiCheck(hapiStreamWaitEvent(st, (hapiEvent_t)source.memcpy_event, 0));
      hapiCheck(hapiMemcpyAsync((void*)dest.ptr, source.ptr, dest.cnt, hapiMemcpyDefault, st));
    } else {
      deviceIpcReceive(source, dest, st, srcPe, mode);
    }
    hapiAddCallback(st, CkCallback(CkRdmaDeviceRecvHandler, &op));
    hapiEvent_t ready = stagingEventTake();
    hapiCheck(hapiEventRecord(ready, st));
    landings[land[i]] = StagedLanding{ready, (size_t)source.cnt};
  }

  // Pass 4: the descriptors now name the landing buffers, as a process-local
  // source with no callback (the sender's fired from the pull above).
  for (int i = 0; i < numops; i++) {
    if (src[i].cnt == 0) continue;
    CkDeviceBuffer nb = src[i];
    nb.ptr = land[i];
    nb.src_pe = CkMyPe();
    nb.src_mpi_rank = CmiMyNode();
    nb.dest_pe = CkMyPe();
    nb.dest_mpi_rank = CmiMyNode();
    nb.device_idx = -1;
    nb.event_idx = -1;
    nb.comm_offset = kStagedLandingMark;
    nb.ipc_protocol = CmiIpcProtocol::NONE;
    nb.ipc_offset = 0;
    nb.ipc_base = NULL;
    nb.memcpy_event = NULL;
    nb.ready_rank = -1;
    nb.ready_seq = 0;
    nb.sender_prepared = true;
    nb.data_stored = false;
    nb.data = NULL;
    // The sender's callback stays in the descriptor: a CkCallback pups a
    // type-dependent number of bytes, so replacing it would change the
    // width of an in-place rewrite. It fired once, from the staging pull
    // above; the landing branch in CkRdmaDeviceIssueRgets drops its copy.
    PUP::toMem wr(buf + off[i]);
    wr | nb;
    if (wr.size() != width[i])
      CkAbort("CkRdmaDeviceStageParked: descriptor width changed (%zu -> %zu)",
              width[i], wr.size());
  }
  if (stagedDbg())
    CmiPrintf("[STAGE %d] id=%llu from PE %d: %d op(s), %zu bytes into landing buffers\n",
              CkMyPe(), (unsigned long long)ck::ObjID(env->getRecipientID()).getElementID(),
              srcPe, live, bytes);
  return true;
}

// Unused, left for future reference
/*
int CkRdmaGetDestPEChare(int dest_pe, void* obj_ptr) {
  // Mechanism extracted from _prepareMsg() in ck.C
  if (dest_pe < 0) {
    int pe = -(dest_pe+1);
    if (pe == CkMyPe()) {
      VidBlock* vblk = CkpvAccess(vidblocks)[(CmiIntPtr)obj_ptr];
      void *objPtr = vblk->getLocalChare();
      dest_pe = objPtr ? pe : vblk->getActualID().onPE;
    } else {
      dest_pe = pe;
    }
  }

  return dest_pe;
}
*/

// Reclaim the IPC events in this PE's slice whose transfers have completed,
// releasing the device comm-buffer block each one was holding. Returns how many
// slots were freed.
//
// Split out of the old findFreeIpcEvent so a sender that finds either resource
// exhausted can keep re-running just this scan while it waits (see
// acquireIpcSendSlot). Nothing here depends on this PE's scheduler: an event
// becomes reclaimable when the *peer* process's receiver records its event and
// sets dst_flag in shared memory, so repeating the scan is what lets a
// momentarily-exhausted pool recover.
//
// max_to_free bounds the sweep: a sender that needs one event slot stops as
// soon as it has one, instead of paying a driver query for every busy slot in
// the slice. Pass 0 for an unbounded sweep, which is what a sender short of
// comm-buffer *bytes* wants -- it cannot know in advance how many blocks it
// must reclaim to fit its request.
//
// Index of this PE among the PEs sharing its device.
//
// The IPC event pool holds one slice per PE *sharing a device* -- its total is
// hapi_ipc_event_pool_size_pe * pes_per_device -- so a slice must be chosen by
// this index, not by the PE's rank in the process. The two differ as soon as a
// process drives more than one GPU: with 8 PEs over 4 devices, CkMyRank()
// reaches 7 and would index 1792 into a 512-entry pool, handing back an event
// that was never created and failing as 'invalid resource handle' when it is
// recorded.
//
// Derived from the device ids rather than from arithmetic on the rank, because
// Block and RoundRobin group PEs onto devices differently and each would
// otherwise need its own formula. Cached: the mapping is fixed after startup.
static int ipcEventPoolSlice(int cpv_my_device_id) {
  static thread_local int cached = -1;
  if (cached < 0) {
    int slice = 0;
    for (int k = 0; k < CkMyRank(); k++)
      if (CpvAccessOther(my_device_id, k) == cpv_my_device_id) slice++;
    cached = slice;
  }
  return cached;
}

// The caller must NOT hold dm->lock. The slice [pool_start, pool_start +
// pool_size) belongs to this PE alone -- pool_start is derived from this PE's
// index among those sharing its device,
// so ranks index disjoint elements -- and the only shared state here is the
// buddy allocator. So the scan runs lock-free and the lock is taken once, at
// the end, around the batch of frees.
static int reclaimCompletedIpcEvents(DeviceManager* dm, int cpv_my_device_id,
                                     int max_to_free) {
  GPUManager& csv_gpu_manager = CsvAccess(gpu_manager);
  int pool_size = csv_gpu_manager.hapi_ipc_event_pool_size_pe;
  int pool_start = ipcEventPoolSlice(cpv_my_device_id) * pool_size;
  const int my_device_index =
      csv_gpu_manager.device_count * CmiMyNodeRankLocal() + cpv_my_device_id;
  hapi_ipc_device_info& my_device_info = csv_gpu_manager.hapi_ipc_device_infos[my_device_index];
  hapi_ipc_event_shared* my_shm_events =
      (hapi_ipc_event_shared*)((char*)csv_gpu_manager.shm_ptr
          + csv_gpu_manager.shm_chunk_size * my_device_index
          + sizeof(hapiIpcMemHandle_t));

  // Offsets of blocks whose slot has been retired but whose memory has not been
  // handed back yet. Collected here so the whole scan stays outside dm->lock;
  // flushed when full so this stays a fixed-size stack buffer whatever
  // +gpuipceventpool is set to.
  constexpr int kFreeBatch = 64;
  size_t pending_free[kFreeBatch];
  int npending = 0;
  int nfreed = 0;

  auto flush = [&]() {
    if (npending == 0) return;
#if CMK_SMP
    CmiLock(dm->lock);
#endif
    for (int j = 0; j < npending; j++) dm->free_comm_buffer(pending_free[j]);
#if CMK_SMP
    CmiUnlock(dm->lock);
#endif
    npending = 0;
  };

  // Free IPC events that are complete
  for (int i = pool_start; i < pool_start + pool_size; i++) {
    int& event_flag = my_device_info.event_pool_flags[i];
    if (event_flag == 0) continue;  // slot is already free

    // Check in shared memory whether the receiver has invoked the memcpy from
    // the device comm buffer on the sender to the destination buffer. Acquire
    // pairs with the receiver's release store, and is what makes its
    // hapiEventRecord visible before the query below.
    // Raised by the receiver's completion handler once its copy out of this
    // slot has landed (or by a correction path that owns the slot): no event
    // to query.
    if (!my_shm_events[i].dst_flag.load(std::memory_order_acquire)) continue;
    // The publish callback behind this use's source event must have fired
    // too: freed before that, the slot could be claimed and named in a new
    // send, and the late callback would mark that use ready.
    if (!my_shm_events[i].src_ready.load(std::memory_order_acquire)) continue;

    // The receiver has invoked the memcpy, so the sender may query the event.
    // Event completion means the transfer from the source device comm buffer to
    // the destination buffer is done, so the allocated block can go back.
    if (event_flag == 1) {
      pending_free[npending++] = my_device_info.event_pool_buff_offsets[i];
      if (npending == kFreeBatch) flush();
    } else if (event_flag != 2) {
      // 2 is a DIRECT transfer: it never allocated a block, so there is
      // nothing to release beyond the event slot itself. Its completion
      // still matters -- it is what tells the sender the receiver has
      // finished reading its source buffer.
      CkAbort("IPC slot flagged complete but not in use");
    }

    // Mark event as free. Ordered after the offset is read above, so the slot
    // cannot be reclaimed and re-pointed before its old block is recorded.
    event_flag = 0;
    my_shm_events[i].dst_flag.store(false, std::memory_order_release);

    if (++nfreed == max_to_free) break;
  }

  flush();
  return nfreed;
}

// Claim a free IPC event from this PE's slice, or -1 if none is free.
//
// Two events are used per message:
// 1) Recorded by the sender after 'source buffer -> device comm buffer' hapiMemcpy.
//    Can be used by the sender to determine if the sender buffer is free for reuse.
//    It is also used by the receiver to create a dependency for the second hapiMemcpy
//    ('device comm buffer -> dest buffer')
// 2) Recorded by the receiver after 'device comm buffer -> dest buffer' hapiMemcpy.
//    It is used by the sender to determine when the allocated block on
//    device comm buffer and IPC events can be freed.
//
// Needs no lock: the slice is this PE's alone (see reclaimCompletedIpcEvents).
// flag_value distinguishes what the slot is holding: 1 for a staged transfer,
// whose comm-buffer block at comm_offset must be released when the slot
// retires, 2 for a direct one, which holds no block. A sentinel offset would
// not do -- 0 is a legal block offset.
static int claimFreeIpcEvent(int cpv_my_device_id, const size_t comm_offset,
                             int flag_value) {
  GPUManager& csv_gpu_manager = CsvAccess(gpu_manager);
  int pool_size = csv_gpu_manager.hapi_ipc_event_pool_size_pe;
  int pool_start = ipcEventPoolSlice(cpv_my_device_id) * pool_size;
  const int my_device_index =
      csv_gpu_manager.device_count * CmiMyNodeRankLocal() + cpv_my_device_id;
  hapi_ipc_device_info& my_device_info = csv_gpu_manager.hapi_ipc_device_infos[my_device_index];

  for (int i = pool_start; i < pool_start + pool_size; i++) {
    int& event_flag = my_device_info.event_pool_flags[i];
    size_t& buff_offset = my_device_info.event_pool_buff_offsets[i];
    if (event_flag == 0) {
      event_flag = flag_value;
      buff_offset = comm_offset;
      // Not ready until this use's publish callback says so. The receiver
      // reads it only after the message naming the slot, which follows this.
      if (csv_gpu_manager.use_shm)
        ipcSharedSlot(my_device_index, i)->src_ready.store(false, std::memory_order_release);
      return i;
    }
  }

  return -1;
}

// Acquire the two resources a cross-process IPC send needs -- a block of the
// device comm buffer and an IPC event pair -- waiting for in-flight transfers
// to release them rather than aborting when the pools are momentarily empty.
//
// Both pools are sized for steady state, but a burst can transiently want more
// than they hold. leanmd's first step is the case that exposed this: every cell
// contacts all 26 neighbours at once, and on a 32-PE run that asks for more
// concurrent transfers per PE than a 256-event slice provides. Enlarging the
// pool is not the fix -- every slot costs two cudaEventInterprocess events per
// PE sharing the device, so merely doubling the default already fails device
// allocation at init (hapi_impl.cpp, ipcEventPoolInit). The slots are genuinely
// transient, so the sender waits for one to come back instead.
//
// The wait makes progress only while peers keep running: a slot is released
// once the receiving process handles the transfer and sets dst_flag in shared
// memory. That happens independently of this PE -- the sends holding those
// slots are already in flight -- so the common case drains quickly. It cannot
// happen if every participating PE is parked in this loop simultaneously, so
// the wait is bounded and says so, rather than hanging. Re-entering the
// scheduler (CsdSchedulePoll) would break such a cycle, but is not safe here:
// this runs mid-marshalling inside the caller's entry method, and re-entry
// could deliver another message to the very chare that is partway through a
// send.
//
// A direct transfer needs only the event pair -- it reads the sender's own
// allocation, so there is no block to reserve -- and so cannot be held up by
// comm-buffer exhaustion at all. It still needs the events: src_event orders
// the receiver's copy after the kernels that produced the data, and dst_event
// is how the sender learns the receiver has finished reading a buffer it does
// not own.
//
// Reclaiming is a *failure* path, not part of every send. The scan costs a
// driver query per busy slot in the slice, so running it up front made every
// message pay for garbage collection it usually did not need -- and the cost
// grew with +gpuipceventpool, which is exactly the knob a send-heavy run turns
// up. The invariant that matters is only that no attempt reports exhaustion
// without having reclaimed first (reclaiming solely *after* a failed
// allocation, as the original code did, let comm-buffer exhaustion abort
// without ever running the scan that frees comm buffers). Trying first and
// reclaiming on failure preserves that: the slow path below always reclaims
// before looping, and the timeout can only be reached through it.
//
// wait == false: one try, one reclaim, one more try, then false instead of the
// loop. That is how the deferrable send path asks (ipcPrepareBuffers, the IPC
// slot queue below it): a PE that spins here receives nothing, and receiving
// is what releases every other PE's slots.
static double ipcSlotTimeoutSecs() { return 60.0; }

static bool acquireIpcSendSlot(DeviceManager* dm, int cpv_my_device_id,
                               bool is_lb_buffer, bool direct,
                               const void* src_ptr,
                               size_t cnt, void** out_buffer,
                               int* out_event_idx, bool wait) {
  const double timeout_s = ipcSlotTimeoutSecs();

  // The reclaim scan stays on the fast path (reclaim, then try): see the
  // discussion at CkRdmaDeviceAllocLbBuffer -- other allocators of this buffer
  // depend on the sweep running often, and the scan is cheap enough now that
  // dst_flag is a plain atomic that taking it off the send path buys little.

  double wait_start = 0.0;
  bool waiting = false;
  int attempts = 0;
  bool first = false;
  // Which resource came up short last time, so the next reclaim knows whether
  // one slot is enough (event pool drained) or it must sweep for bytes (comm
  // buffer drained). Always set by the attempt that precedes each reclaim, so
  // the initializer here is only to keep it defined.
  bool need_slot_only = false;

  for (;;) {
    if (!first) {
      // Bounded when the event pool is what ran dry: one freed slot is all this
      // send can use. Unbounded when it is bytes, since how many blocks must
      // come back to fit cnt is not knowable in advance.
      reclaimCompletedIpcEvents(dm, cpv_my_device_id, need_slot_only ? 1 : 0);
    }
    first = false;

    void* buf = nullptr;
    int ev = -1;

    if (direct) {
      // No block to reserve, so nothing to hand back if the event pool is
      // empty; only the events can hold this up.
      ev = claimFreeIpcEvent(cpv_my_device_id, 0, 2);
      need_slot_only = true;
    } else if (is_lb_buffer) {
      // Already in the comm buffer; nothing to allocate, so only the event pool
      // can hold this up.
      buf = const_cast<void*>(src_ptr);
      const size_t off = (char*)buf - (char*)dm->comm_buffer->base_ptr;
      ev = claimFreeIpcEvent(cpv_my_device_id, off, 1);
      if (ev == -1) buf = nullptr;
      need_slot_only = true;
    } else {
#if CMK_SMP
      CmiLock(dm->lock);
#endif
      buf = dm->alloc_comm_buffer(cnt);
#if CMK_SMP
      CmiUnlock(dm->lock);
#endif
      if (buf == nullptr) {
        need_slot_only = false;  // short of bytes
      } else {
        const size_t off = (char*)buf - (char*)dm->comm_buffer->base_ptr;
        ev = claimFreeIpcEvent(cpv_my_device_id, off, 1);
        if (ev == -1) {
          // Got a block but no event. Hand the block back before waiting:
          // holding half the pair while blocked lets two senders pin each
          // other's missing half indefinitely.
#if CMK_SMP
          CmiLock(dm->lock);
#endif
          dm->free_comm_buffer(off);
#if CMK_SMP
          CmiUnlock(dm->lock);
#endif
          buf = nullptr;
          need_slot_only = true;  // bytes were there, the slot was not
        }
      }
    }

    if (ev != -1) {
      *out_buffer = buf;
      *out_event_idx = ev;
      return true;
    }

    // A caller that can park its send instead: it has had a try and (on the
    // next pass) a reclaim; report the shortage rather than spin.
    if (!wait) {
      if (++attempts >= 2) return false;
      continue;
    }

    if (!waiting) {
      wait_start = CkWallTimer();
      waiting = true;
    } else if (CkWallTimer() - wait_start > timeout_s) {
      if (dm->comm_buffer == nullptr)   // +gpupool: slots are the only resource
        CkAbort("PE %d, device %d: no free CUDA IPC event slot after %.0fs. "
                "If every peer is blocked here too, the transfers that would "
                "release these slots cannot run; reduce concurrent device "
                "sends, or raise +gpuipceventpool.",
                CkMyPe(), dm->global_index, timeout_s);
      CkAbort("PE %d, device %d: no free CUDA IPC event/comm-buffer slot after "
              "%.0fs (comm buffer: %zu bytes free). If every peer is blocked "
              "here too, the transfers that would release these slots cannot "
              "run; reduce concurrent device sends, or raise +gpucommbuffer "
              "(raising +gpuipceventpool costs device memory at init).",
              CkMyPe(), dm->global_index, timeout_s,
              dm->get_comm_buffer_free_size());
    }

    // Back off so the peer processes that release these slots get the core,
    // and so this does not spin on the allocator lock that other PEs sharing
    // this device need.
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 50000;  // 50us
    nanosleep(&ts, nullptr);
  }
}

// Allocate from the device load-balance region, reclaiming and retrying once if
// the first attempt does not fit. See the declaration in ckrdmadevice.h.
//
// Blocks in this region are handed back only by reclaimCompletedIpcEvents: a
// migration's packed payload is released when the receiver acknowledges the
// staged transfer that carried it, through the same slot machinery an ordinary
// cross-process send uses. Nothing on the migration path calls
// acquireIpcSendSlot, so without this the scan would never run on its behalf.
//
// This used to work by accident. The scan sat unconditionally at the top of
// every acquireIpcSendSlot, so an application doing ordinary device sends swept
// the slice often enough that migration always found free blocks waiting.
// Taking that sweep off the send fast path removed the accident and made the
// dependency explicit: the reclaim now happens where the memory is actually
// needed, which is also better timed than sweeping on every unrelated send.
// One reusable event per (device, freeing stream). A block's previous work is
// always on some stream; recording there and waiting on it from the consumer
// is the exact ordering the host barrier was standing in for. Guarded by the
// device manager's own lock, which is what serializes the pool itself.
namespace {
// Keyed by the stream AND the device the entry was made on. A raw
// cudaStream_t is not a stable identity: when an element migrates away its
// stream is destroyed (or pooled), and the next stream created on this PE --
// or an element that arrives and takes the pooled handle -- can carry the very
// same handle value. Keying on the handle alone then returns the departed
// element's event, which was created on whatever device THAT PE had current.
// An event and a stream on different devices is cudaErrorInvalidResourceHandle,
// and because nothing here checks the result the error becomes this thread's
// sticky error and aborts whichever unrelated chare next peeks at it.
// Including the device in the key keeps the two apart; the gate below then
// only waits on events belonging to the consumer's own device, which is the
// only wait that is valid anyway.
struct LbRetireKey {
  hapiStream_t stream;
  int device;
  bool operator==(const LbRetireKey& o) const {
    return stream == o.stream && device == o.device;
  }
};
struct LbRetireKeyHash {
  size_t operator()(const LbRetireKey& k) const {
    return std::hash<void*>()((void*)k.stream) ^ (std::hash<int>()(k.device) << 1);
  }
};
typedef std::unordered_map<LbRetireKey, hapiEvent_t, LbRetireKeyHash> LbRetireEvents;
std::unordered_map<void*, LbRetireEvents> lb_retire_events;
// A std::mutex, not a CmiNodeLock created on first use: "if NULL, create" races
// when two PEs arrive together (see hapi_stream_pool_lock).
std::mutex lb_retire_lock;
}  // namespace


void CkRdmaDeviceNoteLbBufferFreed(void* dm_opaque, hapiStream_t usedBy) {
  if (dm_opaque == NULL) return;
  lb_retire_lock.lock();
  int dev = -1;
  hapiCheck(hapiGetDevice(&dev));
  LbRetireEvents& evs = lb_retire_events[dm_opaque];
  const LbRetireKey key{usedBy, dev};
  auto it = evs.find(key);
  if (it == evs.end()) {
    hapiEvent_t e;
    // Disable timing: this event is only ever waited on, and a timing-enabled
    // event costs more to record.
    hapiCheck(hapiEventCreateWithFlags(&e, hapiEventDisableTiming));
    it = evs.emplace(key, e).first;
  }
  // Re-recording overwrites the previous capture, which is correct: work on one
  // stream is ordered, so the latest record subsumes every earlier one.
  hapiCheck(hapiEventRecord(it->second, usedBy));
  lb_retire_lock.unlock();
}

void CkRdmaDeviceGateLbBuffer(void* dm_opaque, hapiStream_t consumer) {
  if (dm_opaque == NULL) return;
  lb_retire_lock.lock();
  auto dit = lb_retire_events.find(dm_opaque);
  if (dit != lb_retire_events.end()) {
    int dev = -1;
    hapiCheck(hapiGetDevice(&dev));
    for (auto& kv : dit->second) {
      if (kv.first.device != dev) continue;   // another device's event
      if (kv.first.stream == consumer) continue;  // same stream, already ordered
      hapiCheck(hapiStreamWaitEvent(consumer, kv.second, 0));
    }
  }
  lb_retire_lock.unlock();
}

// ---- Device pool: thin wrappers over hapi's (see hapi.h) ----------------------
void* CkDeviceMalloc(size_t size) {
  return hapiDevPoolMalloc(size, CpvAccess(my_device_id));
}
void* CkDeviceMallocNoGrow(size_t size) {
  return hapiDevPoolMallocNoGrow(size, CpvAccess(my_device_id));
}
void CkDeviceFree(void* ptr) { hapiDevPoolFree(ptr); }
bool CkDevicePoolOn() { return hapiDevPoolOn(); }

static thread_local bool ck_sending_migration_payload = false;
void CkRdmaDeviceMarkMigrationPayload(bool sending) {
  ck_sending_migration_payload = sending;
}

void* CkRdmaDeviceAllocLbBuffer(void* dm_opaque, size_t size) {
  DeviceManager* dm = (DeviceManager*)dm_opaque;

  auto attempt = [&]() {
#if CMK_SMP
    CmiLock(dm->lock);
#endif
    void* p = dm->alloc_comm_buffer(size, false);
#if CMK_SMP
    CmiUnlock(dm->lock);
#endif
    return p;
  };

  void* p = attempt();
  if (p != nullptr) return p;

  if (!CsvAccess(gpu_manager).use_shm) return nullptr;
  if (reclaimCompletedIpcEvents(dm, CpvAccess(my_device_id), 0) == 0) return nullptr;

  return attempt();
}

// Device payload of the zerocopy send currently being marshalled, in bytes.
//
// The load balancer's communication graph weights each edge by
// UsrToEnv(msg)->getTotalsize(), but a device zerocopy message carries only the
// CkDeviceBuffer descriptors in its envelope -- a few hundred bytes standing in
// for a transfer that is routinely megabytes. Weighted that way, GPU-to-GPU
// edges are effectively invisible to any communication-aware strategy.
//
// The real sizes are known here and nowhere later on the send path, so they are
// parked per PE and picked up by CkArray::sendToPe. The generated code between
// this call and that one is straight-line marshalling with no intervening entry
// method, so the value cannot be interleaved with another send on this PE.
// CkArray::sendToPe takes and clears it unconditionally, so a stale value
// cannot outlive one array send.
//
// Limitation: group and nodegroup device sends never reach CkArray::sendToPe,
// so their value is left for the next array send on that PE to discard. The LB
// only builds object-to-object edges from array elements, so this costs
// nothing today, but it is why the take-and-clear must stay unconditional.
static thread_local size_t _ck_pending_device_send_bytes = 0;

// How many of this PE's IPC event slots are currently claimed. A slot is
// released only when the receiving process signals it took the staged bytes,
// so a count that climbs across load balancing rounds and never comes back
// down means transfers staged before a migration are never being acknowledged.
int CkRdmaDeviceBusyIpcSlots() {
  GPUManager& gm = CsvAccess(gpu_manager);
  if (!gm.use_shm) return -1;
  const int pool_size = gm.hapi_ipc_event_pool_size_pe;
  const int pool_start = ipcEventPoolSlice(CpvAccess(my_device_id)) * pool_size;
  const int idx = gm.device_count * CmiMyNodeRankLocal() + CpvAccess(my_device_id);
  if (idx < 0 || (size_t)idx >= gm.hapi_ipc_device_infos.size()) return -1;
  hapi_ipc_device_info& info = gm.hapi_ipc_device_infos[idx];
  if ((size_t)(pool_start + pool_size) > info.event_pool_flags.size()) return -1;
  int busy = 0;
  for (int i = pool_start; i < pool_start + pool_size; i++)
    if (info.event_pool_flags[i] != 0) busy++;
  return busy;
}

size_t CkRdmaDeviceTakePendingSendBytes() {
  const size_t bytes = _ck_pending_device_send_bytes;
  _ck_pending_device_send_bytes = 0;
  return bytes;
}

// Releases the element that issued an inter-node zerocopy send once the runtime
// has finished reading its source buffer, then hands control to the callback the
// application attached. A callCFn callback records the PE that built it and
// CkCallback::send routes back there, so this runs on the sending PE.
struct DeviceSendRelease { CkLocRec* rec; CkCallback app_cb; };

static void deviceSendReleaseFn(void* param, void* msg)
{
  DeviceSendRelease* r = (DeviceSendRelease*)param;
  CkLocRec* rec = r->rec;
  CkCallback cb = r->app_cb;
  delete r;
  if (cb.type != CkCallback::ignore) cb.send(msg);
  else if (msg) CkFreeMsg(msg);
  if (rec) rec->noteDeviceSendDone();  // may start a deferred migration
}

// Only network/fallback sends need a host-side producer completion. Keep the
// descriptors by value: the generated proxy's CkDeviceBuffers are stack locals.
// Nothing in this state is sent over the wire.
struct CkDeviceDeferredSend {
  std::vector<CkDeviceBuffer> buffers;
  CkLocRec* owner;
  Chare* sourceObject;
  int sourcePe;
  size_t bytes = 0;
  size_t descriptorBytes = 0;
  size_t remaining = 0;
  void* msg = nullptr;
  std::function<void()> send;

  // A send parked for a CUDA IPC event slot (see the IPC slot queue below):
  // the destination its prepare is for, whether it is the runtime's own
  // migration payload, one event per buffer marking the producer's position
  // when the send was made, and when it was parked.
  bool slotWait = false;
  int destPe = -1;
  bool migrationPayload = false;
  std::vector<hapiEvent_t> producerEvents;
  double parkedAt = 0.0;
  bool warned = false;

  explicit CkDeviceDeferredSend(CkLocRec* rec)
      : owner(rec), sourceObject(rec ? CkActiveObj() : nullptr), sourcePe(CkMyPe()) {}
};

// What pass one of ipcPrepareBuffers decided and acquired for one buffer.
struct IpcPrep {
  bool live = false;
  bool is_lb_buffer = false;
  bool direct = false;
  hapiIpcMemHandle_t export_handle;
  size_t export_offset = 0;
  void* export_base = nullptr;
  void* block = nullptr;   // staged: the comm-buffer block
  int event_idx = -1;
};

// Hand back a slot (and block) that pass one acquired for a message that is
// not going to be prepared now. Nothing was published for it, so no receiver
// will ever raise its flag: it goes straight back, not through the reclaim.
static void releaseUnusedIpcSlot(DeviceManager* dm, hapi_ipc_device_info& info,
                                 const IpcPrep& p)
{
  if (p.event_idx < 0) return;
  info.event_pool_flags[p.event_idx] = 0;
  info.event_pool_buff_offsets[p.event_idx] = 0;
  if (!p.direct && !p.is_lb_buffer && p.block != nullptr) {
#if CMK_SMP
    CmiLock(dm->lock);
#endif
    dm->free_comm_buffer((size_t)((char*)p.block - (char*)dm->comm_buffer->base_ptr));
#if CMK_SMP
    CmiUnlock(dm->lock);
#endif
  }
}

// The sender side of a cross-process (CUDA IPC) send: for every live buffer
// of one message, its transport, a slot from this PE's slice, the staging
// copy if it stages, the migration hold if it is direct, and the event the
// receiver waits on. Two passes, so that a caller that may not block (wait ==
// false) learns of a slot shortage before anything has been copied, held or
// recorded, and can park the whole message: it returns false then, with
// every slot the message had already taken handed back. With wait set it
// waits for slots as acquireIpcSendSlot always has.
//
// `deferred` is the parked record when this is the retry from the slot
// queue. Its buffers then carry the producer position captured when the send
// was made, and everything in pass two goes on the PE's deferred-send stream
// behind that position, rather than on the application's stream.
static bool ipcPrepareBuffers(int dest_pe, int numops, CkDeviceBuffer** buffers,
                              bool wait, CkDeviceDeferredSend* deferred);

// ---- IPC slot queue ------------------------------------------------------------
//
// A cross-process device send holds one of this PE's CUDA IPC event slots
// until the receiver has read the payload. acquireIpcSendSlot waited for a
// free one by spinning, which is safe only while the PEs that release slots
// keep running -- and they release them by RECEIVING: a receiver claims no
// slot of its own, but its deviceIpcReceive is what raises dst_flag on the
// sender's. A PE spinning inside a send receives nothing. So once every PE
// was short at the same moment nothing could progress, and each aborted 60 s
// later with "no free CUDA IPC event slot". leanmd after MetisLB is the case:
// the repartition makes most of a step's device sends cross-process, several
// hundred per PE against a 256-slot slice, and all 32 PEs went short in the
// first step after the balance -- with every migration window already empty.
//
// A send made through the deferrable proxy (CkRdmaDeviceSendWhenReady) is
// therefore never blocked for a slot. When its slots are not there after one
// reclaim, or other sends are already waiting, it is parked here in order
// and retried from a timer callback, so this PE keeps receiving while it
// waits; that is what lets its peers' sends -- and, through them, its own --
// complete. The producer's position is captured at send time in a private
// event, and the deferred prepare orders the IPC event behind exactly that
// on a dedicated stream, not behind whatever the application queued on its
// own stream afterwards: a stream that goes on to wait for the receiver's
// reply would otherwise put that reply and this read in a cycle on the GPU.
// The sending element is held against migration from the moment it is
// parked, as it is for a deferred network send.
//
// The three-argument prepare (sync entries) and the correction paths keep the
// blocking acquire. The slot timeout (60 s) is a warning here, since the wait
// makes progress.
struct CkIpcSlotQueue {
  std::deque<CkDeviceDeferredSend*> queue;
  std::vector<hapiEvent_t> spareEvents;
  hapiStream_t stream = nullptr;   // where deferred prepares are ordered
  bool pumpScheduled = false;
  bool reported = false;
  size_t parked = 0;
  size_t maxDepth = 0;
};
static thread_local CkIpcSlotQueue ipc_slot_queue;

static hapiEvent_t ipcTakeProducerEvent()
{
  CkIpcSlotQueue& q = ipc_slot_queue;
  if (!q.spareEvents.empty()) {
    hapiEvent_t e = q.spareEvents.back();
    q.spareEvents.pop_back();
    return e;
  }
  hapiEvent_t e;
  hapiCheck(hapiEventCreateWithFlags(&e, hapiEventDisableTiming));
  return e;
}

// Non-blocking: the per-thread default stream synchronizes with the legacy
// one, and a landing or restage there has nothing to do with these sends.
static hapiStream_t ipcDeferredStream()
{
  CkIpcSlotQueue& q = ipc_slot_queue;
  if (q.stream == nullptr)
    hapiCheck(hapiStreamCreateWithFlags(&q.stream, hapiStreamNonBlocking));
  return q.stream;
}

static void ipcSlotPump(void*, double);

static void ipcSlotPumpSchedule(double msecs)
{
  if (ipc_slot_queue.pumpScheduled) return;
  ipc_slot_queue.pumpScheduled = true;
  CcdCallFnAfter(ipcSlotPump, nullptr, msecs);
}

// Builds the parked record for a message whose slots were not there. Every
// live buffer's producer position is captured now, and its element held.
static CkDeviceDeferredSend* ipcParkSend(int dest_pe, int numops,
                                         CkDeviceBuffer** buffers, size_t bytes)
{
  CkLocRec* rec = CkpvAccess(_currentLocRec);
  CkDeviceDeferredSend* p = new CkDeviceDeferredSend(rec);
  p->slotWait = true;
  p->destPe = dest_pe;
  p->migrationPayload = ck_sending_migration_payload;
  p->bytes = bytes;
  p->parkedAt = CkWallTimer();
  p->producerEvents.assign(numops, nullptr);
  for (int i = 0; i < numops; i++) {
    CkDeviceBuffer* b = buffers[i];
    if (b->cnt != 0) {
      hapiEvent_t e = ipcTakeProducerEvent();
      if (hapiEventRecord(e, b->hapi_stream) == hapiSuccess) {
        p->producerEvents[i] = e;
      } else {
        // The buffer's stream is on another device (see the cross-device
        // record in ipcPrepareBuffers): settle it here instead, as that does.
        hapiGetLastError();
        ipc_slot_queue.spareEvents.push_back(e);
        hapiCheck(ckSubmitDrainThenSync(b->hapi_stream));
      }
      // Held from now: the buffer must outlive the deferred prepare (staged)
      // or the receiver's read (direct). The deferred prepare runs as the
      // runtime and takes no hold of its own.
      if (rec) {
        rec->noteDeviceSendPosted();
        b->cb = CkCallback(deviceSendReleaseFn,
                           (void*)new DeviceSendRelease{rec, b->cb});
      }
    }
    p->buffers.push_back(*b);
  }
  return p;
}

// Rewrites the descriptor prefix of the held message from the (now prepared)
// buffers, sends it with the original entry's attribution restored, and frees
// the record. Shared by the producer-ready and slot-wait paths.
static void deviceDeferredPublish(CkDeviceDeferredSend* pending)
{
  // Device descriptors occupy a fixed-width prefix of the marshalled message.
  // Fill in the registrations without touching scalar/array arguments after it.
  PUP::sizer size;
  int numops = pending->buffers.size();
  size | numops;
  for (auto& buffer : pending->buffers) size | buffer;
  if (size.size() != pending->descriptorBytes)
    CkAbort("Deferred device send changed its descriptor prefix size");
  PUP::toMem pack(((CkMarshallMsg*)pending->msg)->msgBuf);
  pack | numops;
  for (auto& buffer : pending->buffers) pack | buffer;

  // The original entry method has returned. Restore its attribution just for
  // the send, so LB records an object-to-object edge and the actual device bytes.
  // The outstanding-send references keep this source object stationary/live.
  CkLocRec* savedRec = CkpvAccess(_currentLocRec);
  const size_t savedBytes = _ck_pending_device_send_bytes;
  CkpvAccess(_currentLocRec) = pending->owner;
  _ck_pending_device_send_bytes = pending->bytes;
  if (pending->sourceObject) CkCallstackPush(pending->sourceObject);
  if (pending->migrationPayload && pending->parkedAt > 0.0 && ckMigStatsOn())
    ckMigStatChannel(2, CkWallTimer() - pending->parkedAt);   // dispatch -> the payload message leaves
  pending->send();
  if (pending->sourceObject) CkCallstackPop(pending->sourceObject);
  _ck_pending_device_send_bytes = savedBytes;
  CkpvAccess(_currentLocRec) = savedRec;
  delete pending;
  // The buffer callbacks, now in the message, release the owner after the
  // network read completes. Producer readiness must not release those holds.
}

// Prepares and sends parked messages, in order, while their slots are there.
static void ipcSlotPump(void*, double)
{
  CkIpcSlotQueue& q = ipc_slot_queue;
  q.pumpScheduled = false;
  while (!q.queue.empty()) {
    CkDeviceDeferredSend* p = q.queue.front();
    std::vector<CkDeviceBuffer*> bufs(p->buffers.size());
    for (size_t i = 0; i < p->buffers.size(); i++) bufs[i] = &p->buffers[i];
    // As the runtime, not the element: the hold was taken when the send was
    // parked and must not be taken twice.
    CkLocRec* savedRec = CkpvAccess(_currentLocRec);
    CkpvAccess(_currentLocRec) = nullptr;
    const bool savedPayload = ck_sending_migration_payload;
    ck_sending_migration_payload = p->migrationPayload;
    const bool ok = ipcPrepareBuffers(p->destPe, (int)bufs.size(), bufs.data(),
                                      /*wait=*/false, p);
    ck_sending_migration_payload = savedPayload;
    CkpvAccess(_currentLocRec) = savedRec;
    if (!ok) {
      const double waited = CkWallTimer() - p->parkedAt;
      if (waited > ipcSlotTimeoutSecs() && !p->warned) {
        p->warned = true;
        CmiPrintf("[%d] WARNING: a %zu-byte device send to PE %d has waited "
                  "%.0f s for a CUDA IPC event slot (%zu parked behind it, %d "
                  "of this PE's slots busy). The receivers of this PE's "
                  "in-flight sends are not reading them.\n",
                  CkMyPe(), p->bytes, p->destPe, waited, q.queue.size() - 1,
                  CkRdmaDeviceBusyIpcSlots());
        fflush(stdout);
      }
      ipcSlotPumpSchedule(0.05);
      return;
    }
    q.queue.pop_front();
    for (hapiEvent_t e : p->producerEvents)
      if (e != nullptr) q.spareEvents.push_back(e);  // its wait is enqueued
    QdProcess(1);
    deviceDeferredPublish(p);
  }
}

static bool ipcPrepareBuffers(int dest_pe, int numops, CkDeviceBuffer** buffers,
                              bool wait, CkDeviceDeferredSend* deferred)
{
  GPUManager& csv_gpu_manager = CsvAccess(gpu_manager);
  const int cpv_my_device_id = CpvAccess(my_device_id);
  DeviceManager* dm = csv_gpu_manager.device_map[CkMyPe()];
  const int my_device_idx =
      csv_gpu_manager.device_count * CmiMyNodeRankLocal() + cpv_my_device_id;
  hapi_ipc_device_info& my_device_info =
      csv_gpu_manager.hapi_ipc_device_infos[my_device_idx];

  std::vector<IpcPrep> prep(numops);

  // Pass one: the transport of each live buffer, then its slot.
  for (int i = 0; i < numops; i++) {
    if (buffers[i]->cnt == 0) continue;  // empty: completed by the caller
    IpcPrep& p = prep[i];
    p.live = true;
    // Pool mode has no comm buffer, so nothing is "in the LB region".
    p.is_lb_buffer = dm->comm_buffer != nullptr &&
        ( (size_t)((char*)(buffers[i]->ptr) - (char*)(dm->comm_buffer->base_ptr)) < dm->comm_buffer->total_size );

    // Choose the transport for this buffer. Per buffer, not per message: one
    // entry method can legitimately carry payloads on both sides of the
    // threshold.
    //
    // A buffer that already lives in the comm buffer stays staged whatever
    // its size -- staging it costs nothing, since there is no copy to make,
    // and the receiver reads it through the mapping every peer already holds.
    // Otherwise the direct transport applies when the run asked for it,
    // unless the source allocation turns out not to be exportable, in which
    // case hapiIpcExportBuffer says so and this falls back to staging.
    if (!p.is_lb_buffer && hapiIpcUseDirect()) {
      p.direct = hapiIpcExportBuffer(buffers[i]->ptr, &p.export_handle,
                                     &p.export_offset, &p.export_base);
    }
    // Under +gpupool the export is the only route: there is no comm buffer
    // to fall back to. A buffer cudaIpcGetMemHandle will not name -- managed
    // memory, a host allocation, an address the driver does not own -- is a
    // contract violation there, not a slower path.
    if (!p.direct && !p.is_lb_buffer && hapiDevPoolOn()) {
      CkAbort("PE %d: a %zu-byte device buffer at %p could not be exported "
              "for direct CUDA IPC (%s). Under +gpupool every device buffer "
              "sent must be cudaMalloc-backed: from CkDeviceMalloc or "
              "hapiMalloc.",
              CkMyPe(), (size_t)buffers[i]->cnt, buffers[i]->ptr,
              hapiIpcLastImportErrorName());
    }

    // A zero-copy device send may not reuse or free its source buffer until
    // the CkDeviceBuffer's completion callback fires. That is the contract
    // whatever transport carries it, so a send without a callback is already
    // an application bug -- staging merely hides it, since the source happens
    // to be free once the staging copy retires on the sender's own stream.
    // Direct reads the sender's allocation itself, so the same bug shows up
    // as corrupted data instead. Report it once, where the send is still
    // identifiable, rather than let it surface there.
    // The runtime's own migration payload is the one exception: it is
    // released by the receiver's ack, not by a callback.
    if (p.direct && buffers[i]->cb.type == CkCallback::ignore &&
        !ck_sending_migration_payload) {
      static std::atomic<bool> warned{false};
      bool expected = false;
      if (warned.compare_exchange_strong(expected, true)) {
        CmiPrintf("[%d] WARNING: a %zu-byte device buffer is being sent over "
                  "direct CUDA IPC with no completion callback. Every "
                  "zero-copy device send needs one: attach a CkCallback to "
                  "the CkDeviceBuffer and leave the buffer alone until it "
                  "fires. Staging happens to tolerate a missing callback, so "
                  "forcing these sends back to staging would hide this rather "
                  "than fix it.\n",
                  CkMyPe(), (size_t)buffers[i]->cnt);
        fflush(stdout);
      }
    }
  }
  for (int i = 0; i < numops; i++) {
    IpcPrep& p = prep[i];
    if (!p.live) continue;
    // Waits for a slot if the pools are momentarily drained, or reports the
    // shortage when the caller can park instead; takes and releases dm->lock
    // itself.
    if (!acquireIpcSendSlot(dm, cpv_my_device_id, p.is_lb_buffer, p.direct,
                            buffers[i]->ptr, buffers[i]->cnt, &p.block,
                            &p.event_idx, wait)) {
      // Give back what this message had already taken: holding part of a
      // message's slots while parked would let two short PEs pin each
      // other's remainder.
      for (int j = 0; j < i; j++) releaseUnusedIpcSlot(dm, my_device_info, prep[j]);
      return false;
    }
  }

  // Pass two. A deferred prepare orders behind the producer position captured
  // when the send was made (see the IPC slot queue), on the deferred stream.
  if (deferred) {
    hapiStream_t ds = ipcDeferredStream();
    for (int i = 0; i < numops; i++) {
      if (!prep[i].live) continue;
      if (deferred->producerEvents[i] != nullptr)
        hapiCheck(hapiStreamWaitEvent(ds, deferred->producerEvents[i], 0));
      buffers[i]->hapi_stream = ds;
    }
  }
  for (int i = 0; i < numops; i++) {
    IpcPrep& p = prep[i];
    if (!p.live) continue;
    void* alloc_comm_buffer = p.block;
    const int acquired_event_idx = p.event_idx;
    const bool direct = p.direct;
    const bool is_lb_buffer = p.is_lb_buffer;
    if (direct) {
      buffers[i]->ipc_protocol = CmiIpcProtocol::DIRECT;
      buffers[i]->ipc_handle = p.export_handle;
      buffers[i]->ipc_offset = p.export_offset;
      buffers[i]->ipc_base = p.export_base;
      buffers[i]->comm_offset = 0;
      csv_gpu_manager.ipc_direct_sends.fetch_add(1, std::memory_order_relaxed);

      // Direct exports the application's live allocation and ships the peer a
      // handle to it, so the buffer has to outlive the peer's read -- exactly
      // the condition the send interlock exists for, and the same reason the
      // memcpy and inter-node paths register above and below. This path was
      // added later and never did, so emigrate saw no outstanding sends,
      // migrated the element, and freed the allocation whose handle was
      // already on the wire; the peer then opened a dead handle and aborted
      // with "could not open the CUDA IPC handle".
      //
      // Staged needs none of this: it copies into the comm buffer, so the
      // element's own buffer is free the moment that copy retires.
      // A parked send took its hold when it was parked (ipcParkSend), and
      // runs here with no element current.
      CkLocRec* direct_rec = CkpvAccess(_currentLocRec);
      if (direct_rec) {
        direct_rec->noteDeviceSendPosted();
        buffers[i]->cb = CkCallback(deviceSendReleaseFn,
                                    (void*)new DeviceSendRelease{direct_rec,
                                                                 buffers[i]->cb});
      }
    } else {
      buffers[i]->ipc_protocol = CmiIpcProtocol::STAGED;
      buffers[i]->comm_offset = (char*)alloc_comm_buffer - (char*)dm->comm_buffer->base_ptr;
      csv_gpu_manager.ipc_staged_sends.fetch_add(1, std::memory_order_relaxed);
    }
    buffers[i]->device_idx = my_device_idx;
    buffers[i]->event_idx = acquired_event_idx;
    buffers[i]->sender_prepared = true;


    // Initiate transfer from source buffer to device comm buffer. A direct
    // transfer has no comm buffer to fill -- that saved copy is the point of
    // it -- and an LB buffer is already in place.
    if(!is_lb_buffer && !direct) {
      // CHARM_ZC_VALIDATE: the buffer being staged belongs to the sending
      // chare. If that chare has migrated, its device scratch may already be
      // freed (or not yet reallocated on the new PE) while a send referencing
      // it is still being marshalled -- report that here rather than letting
      // it surface asynchronously somewhere unrelated.
      if (zcValidateOn()) {
        hapiPointerAttributes sattr{};
        const hapiError_t serr = hapiPointerGetAttributes(&sattr, buffers[i]->ptr);
        if (serr != hapiSuccess || sattr.type == hapiMemoryTypeUnregistered) {
          CmiPrintf("[%d] ZC SEND VALIDATE FAIL src=%p (type=%d err=%d) cnt=%zu "
                    "dest_pe=%d mode=IPC\n",
                    CkMyPe(), buffers[i]->ptr, (int)sattr.type, (int)serr,
                    (size_t)buffers[i]->cnt, dest_pe);
          fflush(stdout);
          hapiGetLastError();
        }
      }
      hapiCheck(hapiMemcpyAsync(alloc_comm_buffer, buffers[i]->ptr, buffers[i]->cnt,
            hapiMemcpyDefault, buffers[i]->hapi_stream));

      // The completion callback's contract is "the source buffer is safe to
      // reuse", and for a staged send that is the moment the staging copy
      // above retires -- not when the receiver finishes reading the staged
      // block, a full IPC round later. Fire it here on the sender's stream
      // and ship an ignore callback, so the receiver does not fire it a
      // second time. Every delivery of a STAGED payload reads the staged
      // block rather than the source buffer (including same-process
      // deliveries: a process's own devices are self-mapped in the comm
      // buffer table), so nothing downstream depends on the source after
      // this copy.
      if (buffers[i]->cb.type != CkCallback::ignore) {
        hapiAddCallback(buffers[i]->hapi_stream, buffers[i]->cb);
        buffers[i]->cb = CkCallback(CkCallback::ignore);
      }
    }

    // Record the event the receiver waits on before it reads. Staged, that
    // marks the staging copy as landed; direct, it marks the kernels that
    // produced the source buffer as retired. Either way it is recorded on the
    // application's own stream, so it sits after whatever produced the data.
    // The event comes from THIS PE's pool, so it belongs to this PE's device,
    // and an event can only be recorded on a stream of its own device. The
    // buffer's stream is not always ours: forward-time repair re-prepares a
    // payload on the forwarding PE while the buffer still carries the stream
    // of the PE that built it, and load balancing can put those on different
    // GPUs. Recording across devices fails with invalid-resource-handle.
    //
    // Same remedy the cross-device cases above use: settle that stream on the
    // host and record on ours instead. Stronger ordering than the stream
    // record, and confined to the case where the two devices differ -- which
    // only became reachable once chares started moving between GPUs.
    const int buf_dev = hapiStreamDeviceOf(buffers[i]->hapi_stream);
    if (buf_dev >= 0 && buf_dev != hapiGetDeviceNum()) {
      hapiCheck(ckSubmitDrainThenSync(buffers[i]->hapi_stream));
      hapiCheck(hapiEventRecord(
          my_device_info.src_event_pool[buffers[i]->event_idx], NULL));
      ipcPublishSrcReadyNow(my_device_idx, buffers[i]->event_idx);
    } else {
      hapiCheck(hapiEventRecordNoted(
          my_device_info.src_event_pool[buffers[i]->event_idx],
          buffers[i]->hapi_stream,
          my_device_idx * 100000 + buffers[i]->event_idx));
      ipcPublishSrcReady(my_device_idx, buffers[i]->event_idx, buffers[i]->hapi_stream);
    }
  }
  return true;
}

static void deviceSendProducerReady(void* arg, void*) {
  auto* pending = static_cast<CkDeviceDeferredSend*>(arg);
  CkAssert(CkMyPe() == pending->sourcePe);
  CkAssert(pending->remaining > 0);
  if (--pending->remaining != 0) return;

  // HAPI invokes this on the issuing PE, not on the CUDA host-function thread.
  // Registration and metadata publication happen only after every producer.
  for (auto& buffer : pending->buffers) {
    if (buffer.cnt == 0) continue;  // empty: prepared as such by the sender
    buffer.lci_ncpy_buffer =
        acquireDeviceRegistration(buffer.ptr, buffer.cnt, pending->owner);
    buffer.sender_prepared = true;
  }
  deviceDeferredPublish(pending);
}

void CkRdmaDeviceSendWhenReady(CkDeviceDeferredSend* pending, void* msg,
                              const std::function<void()>& send) {
  CkAssert(pending != nullptr && !pending->buffers.empty());
  pending->msg = msg;
  pending->send = send;
  PUP::sizer size;
  int numops = pending->buffers.size();
  size | numops;
  for (auto& buffer : pending->buffers) size | buffer;
  pending->descriptorBytes = size.size();
  _ck_pending_device_send_bytes = 0;  // do not charge the next unrelated send

  if (pending->slotWait) {
    // Parked for a CUDA IPC event slot; the pump prepares and sends it in
    // its turn. Outstanding for quiescence until then.
    CkIpcSlotQueue& q = ipc_slot_queue;
    q.queue.push_back(pending);
    QdCreate(1);
    q.parked++;
    if (q.queue.size() > q.maxDepth) q.maxDepth = q.queue.size();
    if (!q.reported) {
      q.reported = true;
      CmiPrintf("[%d] device sends wait for CUDA IPC event slots: parked in "
                "order and retried from the scheduler\n", CkMyPe());
      fflush(stdout);
    }
    ipcSlotPumpSchedule(0.05);
    return;
  }

  std::vector<hapiStream_t> streams;
  for (const auto& buffer : pending->buffers) {
    if (buffer.cnt == 0) continue;  // nothing produced it
    if (std::find(streams.begin(), streams.end(), buffer.hapi_stream) == streams.end())
      streams.push_back(buffer.hapi_stream);
  }
  pending->remaining = streams.size();
  // CHARM_LB_MIGSTATS: a migration payload's wait for its pack copies (ship-wait).
  if (pending->migrationPayload && pending->parkedAt == 0.0 && ckMigStatsOn()) pending->parkedAt = CkWallTimer();
  const CkCallback cb(deviceSendProducerReady, pending);
  for (auto stream : streams) hapiAddCallback(stream, cb);
  // HAPI accounts for these callbacks in quiescence detection. The final one
  // publishes the ordinary message before its callback work is retired.
}

// Performs sender-side operations necessary for device zerocopy
void CkRdmaDeviceOnSender(int dest_pe, int numops, CkDeviceBuffer** buffers) {
  CkRdmaDeviceOnSender(dest_pe, numops, buffers, nullptr);
}

void CkRdmaDeviceOnSender(int dest_pe, int numops, CkDeviceBuffer** buffers,
                          CkDeviceDeferredSend** pending) {
  ZcHostTimer zc_host_timer(zc_mode_stats.host_send_us, zc_mode_stats.host_send_n);
  if (pending) *pending = nullptr;
  // dest_pe == -1 means this PE has never confirmed where the target element
  // actually lives (xi-Parameter.C asks the location manager directly for
  // this, rather than substituting a homePe() guess). Don't decide
  // MEMCPY-vs-IPC from an unconfirmed location. But RDMA is not a safe
  // universal fallback here: findTransferModeDevice (above) only ever
  // returns RDMA for genuinely different physical nodes, so reconverse's
  // CmiIssueRget has never had to handle an RDMA-mode transfer that lands on
  // the sender's own physical node -- and its same-node loopback path does a
  // raw host memcpy between src/dst pointers, which corrupts memory (or
  // segfaults) when those are two different processes' device pointers, as
  // an unconfirmed-destination first contact on a single-node job always is.
  // When the whole job is on one physical node, an unconfirmed destination
  // can only ever truly resolve to MEMCPY or IPC (never RDMA), so stage IPC:
  // if the true destination turns out to be this same process after all,
  // the receiver's MEMCPY branch below uses source.ptr directly and ignores
  // this staging entirely -- the only cost is one unused, never-freed
  // comm-buffer slot and IPC event, a bounded one-time waste per
  // newly-confirmed pair, not a per-step cost. Only for a genuinely
  // multi-physical-node job with an unconfirmed destination do we fall back
  // to RDMA -- correct only if that destination doesn't land back on our own
  // physical node; that residual case isn't handled yet.
  const bool dest_confirmed = (dest_pe != -1);
  zcDestCount(dest_confirmed);
  CkNcpyModeDevice transfer_mode;
  if (dest_confirmed) {
    transfer_mode = findTransferModeDevice(CkMyPe(), dest_pe);
  } else if (CmiNumPhysicalNodes() == 1) {
    transfer_mode = CkNcpyModeDevice::IPC;
  } else {
    transfer_mode = CkNcpyModeDevice::RDMA;
  }

  // The mode above is chosen from where the target is now, but only becomes
  // true when the message lands. A target that migrates across a process
  // boundary in between is handled by the correction protocol, which re-sends
  // the payload by a route the new host can read -- so the mode does not have
  // to be pessimistic here.

  // Store destination PE in the metadata message
  // FIXME: Not necessary? save_op.dest_pe is set to CkMyPe() on the receiver
  size_t device_bytes = 0;
  for (int i = 0; i < numops; i++) {
    buffers[i]->dest_pe = dest_pe;
    buffers[i]->dest_mpi_rank = dest_confirmed ? CmiNodeOf(dest_pe) : -1;
    buffers[i]->src_pe = CmiMyPe();
    buffers[i]->src_mpi_rank = CmiNodeOf(CmiMyPe());
    device_bytes += buffers[i]->cnt;
  }
  _ck_pending_device_send_bytes = device_bytes;

  // Empty payloads. A zero-length buffer has nothing to transfer and nothing to
  // order against, so it takes none of what follows: no event, no IPC slot or
  // export, no registration, no producer wait, and no migration interlock. Its
  // completion callback fires now -- the source is trivially free -- and the
  // descriptor travels marked prepared with no protocol, which the receiver
  // (CkRdmaDeviceIssueRgets) completes without touching the device and the
  // forward repair (CkRdmaDeviceRepairForward) leaves alone. The message keeps
  // its device-zerocopy type, so the post entry method still runs, and a
  // message can mix empty and real payloads: this is per buffer.
  //
  // What it buys: a stencil exchange that sends to every neighbour every step,
  // whether or not it has anything for them, otherwise pays a device handshake
  // per empty message -- on sph2d that was 16 of the 32 device messages a
  // patch sends per step, in a step that is nothing but that protocol.
  //
  // The runtime's own migration payload is exempt: it is released by the
  // receiver's ack, not by a callback, and goes through unchanged.
  int live_ops = 0;
  for (int i = 0; i < numops; i++) {
    if (buffers[i]->cnt != 0 || ck_sending_migration_payload) { live_ops++; continue; }
    buffers[i]->ipc_protocol = CmiIpcProtocol::NONE;
    buffers[i]->device_idx = -1;
    buffers[i]->event_idx = -1;
    buffers[i]->memcpy_event = NULL;
    buffers[i]->ready_rank = -1;
    buffers[i]->ready_seq = 0;
    buffers[i]->sender_prepared = true;
    if (buffers[i]->cb.type != CkCallback::ignore) {
      buffers[i]->cb.send();
      buffers[i]->cb = CkCallback(CkCallback::ignore);
    }
  }
  if (live_ops == 0) return;  // nothing to prepare; the proxy sends at once

  // CHARM_ZC_VALIDATE: check every outgoing source pointer, in every transfer
  // mode, at the moment the send is posted. A device buffer whose owning chare
  // has migrated is either unmapped (freed by the old PE) or resident on a
  // different device than the one now current, and both surface later as an
  // "illegal memory access" inside an async copy with no hint of which send
  // produced it. Report it here, where dest_pe/cnt/mode still identify the send.
  {
    if (zcValidateOn()) {
      int cur_dev = -1;
      hapiGetDevice(&cur_dev);
      for (int i = 0; i < numops; i++) {
        if (buffers[i]->cnt == 0) continue;
        hapiPointerAttributes sattr{};
        const hapiError_t serr = hapiPointerGetAttributes(&sattr, buffers[i]->ptr);
        const bool unmapped = (serr != hapiSuccess || sattr.type == hapiMemoryTypeUnregistered);
        const bool wrong_dev = (!unmapped && sattr.type == hapiMemoryTypeDevice &&
                                sattr.device != cur_dev);
        if (unmapped || wrong_dev) {
          CmiPrintf("[%d] ZC SEND VALIDATE FAIL src=%p %s (type=%d ptr_dev=%d "
                    "cur_dev=%d err=%d) cnt=%zu dest_pe=%d mode=%s\n",
                    CkMyPe(), buffers[i]->ptr,
                    unmapped ? "UNMAPPED" : "WRONG-DEVICE",
                    (int)sattr.type, (int)sattr.device, cur_dev, (int)serr,
                    (size_t)buffers[i]->cnt, dest_pe,
                    transfer_mode == CkNcpyModeDevice::MEMCPY ? "MEMCPY"
                      : (transfer_mode == CkNcpyModeDevice::IPC ? "IPC" : "RDMA"));
          fflush(stdout);
          hapiGetLastError();
        }
      }
    }
  }

  if(transfer_mode == CkNcpyModeDevice::MEMCPY)
  {
    // Same process: the receiver can read buffers[i]->ptr directly, but only
    // once the kernels producing it have finished. Blocking the host here
    // (hapiStreamSynchronize) guaranteed that at the cost of stalling the
    // sender on every such send -- and same-process is the common case, so
    // that stall dominated. Record an event instead and let the receiver's
    // stream wait on it: identical ordering, no host block, no extra copy.
    // A same-process memcpy send leaves the payload in the application's buffer
    // for the receiver to copy out of, exactly as the inter-node path leaves it
    // for the network to read. Migration frees and reallocates that buffer, so
    // count these against the issuing element too -- this is the common case in
    // a blocked decomposition, where most neighbours share a process, and it
    // was the one path with no protection at all.
    CkLocRec* memcpy_rec = CkpvAccess(_currentLocRec);
    if (memcpy_rec) {
      for (int i = 0; i < numops; i++) {
        if (buffers[i]->cnt == 0) continue;  // completed above, nothing to hold
        memcpy_rec->noteDeviceSendPosted();
        buffers[i]->cb = CkCallback(deviceSendReleaseFn,
                                    (void*)new DeviceSendRelease{memcpy_rec, buffers[i]->cb});
      }
    }

    for (int i = 0; i < numops; i++) {
      if (buffers[i]->cnt == 0) continue;
      {
        // The receiver gates its copy on this on the host (DeviceRecvPending):
        // a pinned flag it reads with a plain load, or, when this ring has no
        // slot, the event it queries. A send made without a stream carries
        // neither -- the caller is sending from the completion callback of
        // the work that produced the buffer, or the data is static -- and the
        // receiver reads "no flag, no event" as complete.
        buffers[i]->ready_rank = -1;
        buffers[i]->ready_seq = 0;
        buffers[i]->memcpy_event = NULL;
        if (buffers[i]->source_ready) continue;
        int rank = -1;
        uint32_t seq = 0;
        if (hapiFlagIssue(buffers[i]->hapi_stream, &rank, &seq)) {
          buffers[i]->ready_rank = rank;
          buffers[i]->ready_seq = seq;
          continue;   // one driver call; the event is only its fallback
        }
        buffers[i]->memcpy_event = ckDeviceRecordMemcpyEvent(buffers[i]->hapi_stream);
        if (buffers[i]->memcpy_event == NULL)  // no event available; fall back
          ckSubmitDrainThenSync(buffers[i]->hapi_stream);
      }
    }
    return;
  }

  GPUManager& csv_gpu_manager = CsvAccess(gpu_manager);

  if(transfer_mode == CkNcpyModeDevice::IPC && csv_gpu_manager.use_shm) {
    if (pending) {
      // A deferrable send is never blocked for a slot. It is parked, in
      // order, when its slots are not there or earlier sends are already
      // waiting (see the IPC slot queue above ipcPrepareBuffers).
      if (!ipc_slot_queue.queue.empty() ||
          !ipcPrepareBuffers(dest_pe, numops, buffers, /*wait=*/false, nullptr))
        *pending = ipcParkSend(dest_pe, numops, buffers, device_bytes);
      return;
    }
    // Synchronous entries and the three-argument API keep their blocking
    // contract: the slots are waited for here.
    ipcPrepareBuffers(dest_pe, numops, buffers, /*wait=*/true, nullptr);
    return;
  } else {
#if !CMK_GPU_COMM
    // Use a naive host-staged mechanism
    // Allocate temporary host buffers and copy source buffers
    for (int i = 0; i < numops; i++) {
      if (buffers[i]->cnt == 0) continue;
      buffers[i]->data_stored = true;
      buffers[i]->sender_prepared = true;
      hapiCheck(hapiMallocHost(&buffers[i]->data, buffers[i]->cnt));
      hapiCheck(hapiMemcpyAsync(buffers[i]->data, buffers[i]->ptr, buffers[i]->cnt,
            hapiMemcpyDeviceToHost, buffers[i]->hapi_stream));
    }

    // Wait for the copies to finish
    for (int i = 0; i < numops; i++) {
      if (buffers[i]->cnt == 0) continue;
      hapiCheck(ckSubmitDrainThenSync(buffers[i]->hapi_stream));
    }
#else
  CkLocRec* sender_rec = CkpvAccess(_currentLocRec);
  if (pending && numops > 0) {
    *pending = new CkDeviceDeferredSend(sender_rec);
    (*pending)->migrationPayload = ck_sending_migration_payload;
  }
  for (int i = 0; i < numops; i++) {
    if (buffers[i]->cnt == 0) {
      // Empty: its descriptor keeps its place in the message's fixed-width
      // prefix, but nothing is registered or held.
      if (pending) (*pending)->buffers.push_back(*buffers[i]);
      continue;
    }
    // This registers the application's own buffer and the receiver reads it over
    // the network, so it stays live well past this call. Count it against the
    // issuing element; emigrate stands down while any are outstanding.
    // Same element owns the registration: it is that element's buffer, and its
    // migration is when the buffer dies.
    if (sender_rec) {
      sender_rec->noteDeviceSendPosted();
      buffers[i]->cb = CkCallback(deviceSendReleaseFn,
                                  (void*)new DeviceSendRelease{sender_rec, buffers[i]->cb});
    }
    if (pending) {
      // The proxy marshals an unregistered placeholder. SendWhenReady replaces
      // it after HAPI completion, before publishing any RDMA metadata.
      buffers[i]->lci_ncpy_buffer = CmiNcpyBuffer();
      buffers[i]->lci_ncpy_buffer.deviceRdmaOpInfo = nullptr;
      memset(buffers[i]->lci_ncpy_buffer.layerInfo, 0,
             sizeof(buffers[i]->lci_ncpy_buffer.layerInfo));
      buffers[i]->sender_prepared = false;
      (*pending)->buffers.push_back(*buffers[i]);
      (*pending)->bytes += buffers[i]->cnt;
    } else {
      // Synchronous entry methods and callers using the original prepare API
      // have no deferred continuation and retain their blocking contract.
      hapiCheck(ckSubmitDrainThenSync(buffers[i]->hapi_stream));
      buffers[i]->lci_ncpy_buffer =
          acquireDeviceRegistration(buffers[i]->ptr, buffers[i]->cnt, sender_rec);
      buffers[i]->sender_prepared = true;
    }
  }
#endif
  }
}
#endif // CMK_GPU
