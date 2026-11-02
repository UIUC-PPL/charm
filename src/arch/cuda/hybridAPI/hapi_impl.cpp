#include <stdio.h>
#include <stdlib.h>
#include <climits>
#include <cstdint>
#include <cstring>
#include <cerrno>
#include <string>
#include <cmath>
#include <algorithm>
#include <queue>
#include <atomic>
#include <vector>
#include <deque>
#include <type_traits>
#include <set>
#include <map>
#include <mutex>
#include <chrono>
#include <thread>
#include <condition_variable>
#include <functional>
#include <pthread.h>
#include <unistd.h>
#include <cxxabi.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sched.h>

// The per-thread default stream on either vendor (set before the runtime
// header is first included, which hapi_portable.h does).
#define CUDA_API_PER_THREAD_DEFAULT_STREAM
#define HIP_API_PER_THREAD_DEFAULT_STREAM
#include "hapi_portable.h"   // the vendor table; defines CMK_CUDA or CMK_HIP
#if CMK_CUDA
// The versioned driver entry-point lookup arrived after CUDA 12.0, whose
// runtime has only the unversioned one. Every lookup here asks for the 12.0
// ABI, which is what the unversioned form resolves, so the two agree there.
#if defined(CUDA_VERSION) && CUDA_VERSION <= 12000
#define cudaGetDriverEntryPointByVersion(symbol, fn, version, flags, status) \
    cudaGetDriverEntryPoint(symbol, fn, flags, status)
#endif
#endif

#include "converse.h"
#include "ckrescale.h"
#include "charm++.h"

#include "hapi.h"
#include "hapi_impl.h"
#include "hapi_range_heap.h"
// The device pool's vmm backend, defined with the pool further down; the IPC
// startup code above it publishes the heap and exports handles for it.
struct hapi_pool_shm_entry;
bool hapiDevPoolVmmPublish(int dev, hapi_pool_shm_entry* e, int shm_index,
                           hapiIpcMemHandle_t* handle);
bool hapiDevPoolVmmHandleOf(const void* ptr, hapiIpcMemHandle_t* handle, void** base);
// +gpuloadbusy: charge GPU load as a share of busy time instead of SM-seconds.
static bool hapi_gpu_load_busy = false;
#include <unordered_map>
#include <unordered_set>
#include "gpumanager.h"
#if CMK_NVTX
#include "hapi_nvtx.h"
bool hapiNvtxOn = false;   // +nvtx (ck-core/cknvtx.C) turns the HAPI ranges on
#endif

#if CMK_LBDB_ON
#if CMK_CUDA
#include <cupti.h>
#include <cupti_runtime_cbid.h>
#endif
#include "hapi_trace_backend.h"
#include "LBManager.h"
#include "ck.h"
#include "cklocrec.h"

#if CMK_CUDA
#define CUPTI_SAFE_CALL(call)                                              \
  do {                                                                     \
    CUptiResult _status = call;                                            \
    if (_status != CUPTI_SUCCESS) {                                        \
      const char *errstr;                                                  \
      cuptiGetResultString(_status, &errstr);                              \
      CmiPrintf("HAPI CUPTI error: %s at %s:%d\n", errstr, __FILE__, __LINE__); \
    }                                                          \
  } while (0)
#endif
#endif

#define SERVER_FIFO_TEMPLATE "/tmp/server_pipe_%ld"
#define CLIENT_FIFO_TEMPLATE "/tmp/client_pipe_%ld"
#define BUFFER_SIZE 256
#define STREAM_BUF_SIZE 1024

#if defined HAPI_TRACE || defined HAPI_INSTRUMENT_WRS
extern "C" double CmiWallTimer();
#endif

extern int Cmi_isOldProcess;

extern int CmiSetCPUAffinityLogical(int core);

static void createPool(int *nbuffers, int n_slots, std::vector<BufferPool> &pools);
static void releasePool(std::vector<BufferPool> &pools);

#ifdef HAPI_CUDA_CALLBACK
struct hapiCallbackMessage {
  char header[CmiMsgHeaderSizeBytes];
  int rank;
  CkCallback cb;
  void* cb_msg;
};
#endif

#ifndef HAPI_CUDA_CALLBACK
void* (*hapiDeviceWorkBegin)() = nullptr;
// Pool segments register with the fabric at the mapping (set by ck-core; see the device pool below).
void (*hapiDevPoolSegmentHook)(void* base, size_t bytes, size_t unit, int device) = nullptr;
void (*hapiDeviceWorkEnd)(void* token) = nullptr;

typedef struct hapiEvent {
  hapiEvent_t event;   // NULL marks a pinned-flag entry (see flag_seq)
  CkCallback cb;
  void* cb_msg;
  hapiWorkRequest* wr; // if this is not NULL, buffers and request itself are deallocated
  uint32_t flag_seq;   // pinned-flag entries: the value the slot must reach
  void* workToken;     // hapiDeviceWorkBegin's token, handed to hapiDeviceWorkEnd when this fires

  // track: mark device work in flight on the current element until this
  // fires. A readiness flag (hapiFlagIssue) carries no work of its own.
  hapiEvent(hapiEvent_t event_, const CkCallback& cb_, void* cb_msg_, hapiWorkRequest* wr_ = NULL,
            bool track = true)
            : event(event_), cb(cb_), cb_msg(cb_msg_), wr(wr_), flag_seq(0),
              workToken((track && hapiDeviceWorkBegin) ? hapiDeviceWorkBegin() : nullptr) {}
} hapiEvent;

// Pending events, one FIFO per stream. Events on one stream complete in the
// order they were recorded, so its queue is popped from the head and left at
// the first incomplete event; events on different streams do not, and one
// queue for all of them stalled everything recorded after a long kernel's
// completion event: in moe, every receive landed on the comm stream behind an
// expert's 10-150 ms compute-done event was not noticed until that expert
// finished. A stream's entry stays once created (a handful per PE), so a
// destroyed-and-reused handle just reuses it.
// A deque, not a vector: hapiPollEvents walks this and the callbacks it
// fires can record new events, and a new stream appends a queue here. A
// vector reallocates on that append, leaving the reference hapiPollEvents is
// holding dangling -- its next pop() then frees through stale memory, which
// is the double free seen inside hapiPollEvents when a balancer migrates
// enough objects to create a batch of new streams. A deque keeps references
// to existing elements valid across push_back -- but NOT iterators: past a
// few chunks (five queues each) a push_back reallocates the deque's node map,
// and a range-for's iterator holds a pointer into that map. So hapiPollEvents
// walks it by index (operator[] re-derives the chunk from the live map), and
// nothing may keep an iterator into it across a callback.
typedef std::deque<std::pair<hapiStream_t, std::queue<hapiEvent>>> hapiEventQueues;
CpvDeclare(hapiEventQueues, hapi_event_queue);
// Free events, partitioned by the device they were created on: an event can
// only be recorded on a stream of its own device.
typedef std::map<int, std::queue<hapiEvent_t>> HapiEventPools;
CpvDeclare(HapiEventPools, hapi_event_pool);
#endif // HAPI_CUDA_CALLBACK
CpvDeclare(int, n_hapi_events);

// Pinned-flag completion detection, ported from paw-atm26
// (2119bcbc1). The scheduler polls the head of every stream queue on every
// loop iteration, and an event query is a driver call: with eight PE threads
// on one context the not-ready answers alone held the driver 90% of a step
// on sph2d (42 million cudaEventQuery calls per LB window on the process
// with the fluid, each followed by a cudaGetLastError to clear the code), and
// every real launch queued behind them. Instead, completion is a stream-
// ordered 32-bit write of a per-PE sequence number into a pinned slot
// (cuStreamWriteValue32, executed by the stream's front end after all prior
// work), and the poll is a plain load: no driver call, no lock, nanoseconds.
// Slots form a per-PE ring; a slot is reassigned only after HAPI_FLAG_SLOTS
// newer entries, and recordEvent falls back to an event when that many are
// in flight or when the stream belongs to another device, so a pending
// entry's slot is never overwritten. Device time comes from CUPTI on this
// branch, so nothing needs event timestamps and every callback may take it.
// The driver entry point is looked up at startup: libcuda is only reached
// through libcupti here, not on the link line. This is the default; a
// driver without cuStreamWriteValue32 falls back to event queries, and
// +gpueventquery asks for them explicitly.
// Power of two; max in-flight flag entries per PE. 256 covered sph2d's few
// patches per PE. leanmd keeps 240 objects per PE with a callback or two in
// flight each, and every callback past the ring's end fell back to a CUDA
// event -- create, record, and a query on every scheduler loop: measured
// 2026-09-15 at 17.5 M cudaEventQuery and 44 k cudaEventCreate per process
// per 20-step window, 11% of PE time inside those two calls, and 34 M CUPTI
// runtime records for the load balancer to parse at each LB step (1.6-2.7 s,
// on the last PE to close its window, inside whatever entry method it was
// running). 4096 slots is 256 KB of pinned memory per PE.
#define HAPI_FLAG_SLOTS 4096
#define HAPI_FLAG_STRIDE 16  // uint32s per slot: one cache line, no false sharing
static bool hapi_use_flag_poll = false;
static void hapiSubmitStart(int nsub, int device);   // submitter threads, defined with recordEvent
static void hapiSubmitSetToken(uint64_t token);
static void hapiSetStreamCap(int cap);
static void hapiStreamCapAnnounce();   // prints the cap in force (the HIP default is 32), defined with the pool
static void hapiSetRecvPriority();
typedef hapiDrvResult (*hapiStreamWriteValue32Fn)(hapiDrvStream, uintptr_t, uint32_t, unsigned int);
static hapiStreamWriteValue32Fn hapi_stream_write_value32 = nullptr;
CpvDeclare(uint32_t*, hapi_flag_slots);   // pinned host ring
CpvDeclare(void*, hapi_flag_slots_dev);   // device alias of the ring
CpvDeclare(uint32_t, hapi_flag_seq);      // last assigned sequence number
CpvDeclare(int, hapi_flag_inflight);      // flag entries currently queued
// Which ring slots hold an entry that has not been drained yet. The in-flight
// COUNT is not enough to protect them: entries drain per stream, and
// hapiPollEvents stops at the first incomplete entry in each stream, so a
// stream whose head is blocked keeps its slots while other streams drain and
// the sequence number runs on. Once seq advances HAPI_FLAG_SLOTS past a queued
// entry, that entry's slot is rewritten with a newer value, its *slot ==
// flag_seq test can never come true again, and every later entry on the same
// stream is stuck behind it forever.
CpvDeclare(unsigned char*, hapi_flag_busy);

// With flags, a PE waiting on its GPU spins on a plain load and never leaves
// its core. With events it did: most of the PE threads sat in a futex on the
// driver lock, and CUPTI's worker and the network progress thread ran in the
// gaps. Measured 12 Sep 2026 on sph2d at 8 PEs on 8 cores: flags were 15%
// slower under LB than events, and 10% faster at 6 or 4 PEs. So while flag
// entries are pending and the scheduler finds nothing else to do, give the
// core away for a slice; it comes straight back when nothing else is runnable.
static void hapiIdleYield(void*) {
  if (CpvAccess(hapi_flag_inflight) > 0) sched_yield();
}

// A second way to write the flag, +gpuflagmemset: cuMemsetD32Async of one
// word into the slot's device alias. It goes through the fill path (a kernel
// or the copy engine) rather than the front end's stream memop, and exists to
// tell the two apart when a flag write never lands.
typedef hapiDrvResult (*hapiMemsetD32AsyncFn)(uintptr_t, unsigned int, size_t, hapiDrvStream);
static hapiMemsetD32AsyncFn hapi_memset_d32_async = nullptr;
#if CMK_HIP
// HIP has both writers as plain runtime calls (no driver entry point to look
// up); these adapt them to the signatures above.
static hipError_t hapiHipStreamWriteValue32(hipStream_t s, uintptr_t p, uint32_t v, unsigned int flags) {
  return hipStreamWriteValue32(s, (void*)p, v, flags);
}
// hipMemsetD32Async's completion makes its own write visible but releases nothing else at system
// scope: a kernel's or a small (blit) copy's writes queued before it can still sit in this device's
// L2 when the flag lands, and a peer device or process that reads that memory on the signal reads the
// old contents (MI250X, ROCm 7.14: a 32 KB fill or copy on device 0, the flag, a copy from a device-1
// stream -- stale in 200 of 200 trials; chantest's ipc puts and +LBAsync snapshots). An event record
// ahead of the memset carries the system-scope release (with hipEventDisableSystemFence the reads go
// stale again), so one per device is recorded, never queried, before every flag. CUDA needs none.
// +gpuflagkernel: the flag written by a one-thread kernel whose __threadfence_system() is the release
// (hapi_kernels.cu), one launch per flag instead of an event record and a blit.
extern "C" int hapiFlagKernelImpl(void* p, unsigned int v, void* stream);
// The HIP default (Frontier 2026-10-22): validated on the cross-device stale-read repro (vmmtest/flag_kernel,
// 0 of 200 stale for kernel- and copy-written data, against 200 of 200 for the memset without its fence
// event), 11 us of host time per flag against 26 for the event record and the blit, and leanmd's balancer
// arm 104 -> 84 ms/step. +gpuflagmemset restores the event-record-and-memset writer.
static bool hapi_flag_kernel = true;
static hipError_t hapiHipFlagKernelWrite(uintptr_t p, unsigned int v, size_t n, hipStream_t s) {
  (void)n;
  return (hipError_t)hapiFlagKernelImpl((void*)p, v, (void*)s);
}
static hipError_t hapiHipMemsetD32Async(uintptr_t p, unsigned int v, size_t n, hipStream_t s) {
  if (hapi_flag_kernel) return hapiHipFlagKernelWrite(p, v, n, s);
  thread_local std::vector<hipEvent_t> fence;   // per thread: PEs and submitters record their own
  hipDevice_t dev = 0;
  hipError_t r = hipStreamGetDevice(s, &dev);
  if (r != hipSuccess) return r;
  if ((size_t)dev >= fence.size()) fence.resize(dev + 1, nullptr);
  if (fence[dev] == nullptr) {
    int saved = 0;
    hipGetDevice(&saved);
    hipSetDevice(dev);
    r = hipEventCreateWithFlags(&fence[dev], hipEventDisableTiming);
    hipSetDevice(saved);
    if (r != hipSuccess) { fence[dev] = nullptr; return r; }
  }
  r = hipEventRecord(fence[dev], s);
  if (r != hipSuccess) return r;
  return hipMemsetD32Async((hipDeviceptr_t)p, (int)v, n, s);
}
#endif
static bool hapi_flag_use_memset = false;
// Completion-flag writes this process has issued (both flag paths, every
// stream callback): printed at exit with CHARM_ZC_STATS, to put a number on
// what a message path costs the copy engine (a channel put adds two).
static std::atomic<long> hapi_flag_writes_total{0};
long hapiFlagWritesIssued() { return hapi_flag_writes_total.load(std::memory_order_relaxed); }

// ---- stall trace (+gpustalltrace) --------------------------------------------
// A node-wide log of every event record, stream wait and flag issue that goes
// through hapi, kept so that a stalled stream can be walked back to the wait
// that never came and to the record it was waiting for. The lock and the maps
// cost on the hot path, so this is off unless asked for.
#include <unordered_map>
#include <vector>
#include <mutex>
struct HapiTraceOp {
  uint64_t idx;     // position in the stream's op sequence
  char kind;        // 'R' record, 'W' wait, 'F' flag issue
  void* ev;         // R/W: the event
  uint64_t evgen;   // R: the generation this record made; W: the one captured
  uint32_t seq;     // F: sequence number
  int rank;         // issuing PE's rank in the node
  int note;         // caller's tag (IPC slot), 0 if none
};
#define HAPI_TRACE_RING 128
struct HapiTraceStream {
  HapiTraceOp ops[HAPI_TRACE_RING];
  uint64_t n = 0;   // ops issued; the ring holds the last HAPI_TRACE_RING
};
struct HapiTraceEvRec { void* stream; uint64_t idx; uint64_t gen; int rank; int note; };
struct HapiTraceEvent {
  uint64_t gen = 0;
  HapiTraceEvRec last[4]; int nlast = 0;
};
static bool hapi_trace_on = false;
static CmiNodeLock hapi_trace_lock = NULL;
static std::unordered_map<void*, HapiTraceStream>* hapi_trace_streams = nullptr;
static std::unordered_map<void*, HapiTraceEvent>* hapi_trace_events = nullptr;
static std::once_flag hapi_trace_once;
CpvDeclare(uint32_t*, hapi_flag_probe);      // pinned word for live probes
CpvDeclare(void*, hapi_flag_probe_dev);

static void hapiTraceInit() {
  std::call_once(hapi_trace_once, []() {
    hapi_trace_lock = CmiCreateLock();
    hapi_trace_streams = new std::unordered_map<void*, HapiTraceStream>();
    hapi_trace_events = new std::unordered_map<void*, HapiTraceEvent>();
  });
}
static HapiTraceOp& hapiTracePush(void* stream) {
  HapiTraceStream& s = (*hapi_trace_streams)[stream];
  HapiTraceOp& op = s.ops[s.n % HAPI_TRACE_RING];
  op.idx = s.n++;
  return op;
}
static void hapiTraceFlag(void* stream, uint32_t seq) {
  if (!hapi_trace_on) return;
  CmiLock(hapi_trace_lock);
  HapiTraceOp& op = hapiTracePush(stream);
  op.kind = 'F'; op.ev = NULL; op.evgen = 0; op.seq = seq; op.rank = CmiMyRank(); op.note = 0;
  CmiUnlock(hapi_trace_lock);
}

void hapiSubmitEventRecord(void* event, hapiStream_t stream);
bool hapiSubmitOn();
hapiError_t hapiEventRecordNoted(hapiEvent_t event, hapiStream_t stream, int note) {
  // Submit mode: behind the PE's queued work on this stream, never ahead of it.
  if (hapiSubmitOn()) { hapiSubmitEventRecord((void*)event, (hapiStream_t)stream); return hapiSuccess; }
  if (!hapi_trace_on) return hapiEventRecordRaw(event, stream);
  // The lock spans the call so the generation the log assigns is the one the
  // driver saw: a wait logged after this record captured it, one before did not.
  CmiLock(hapi_trace_lock);
  const hapiError_t r = hapiEventRecordRaw(event, stream);
  if (r == hapiSuccess) {
    HapiTraceEvent& e = (*hapi_trace_events)[(void*)event];
    HapiTraceOp& op = hapiTracePush((void*)stream);
    op.kind = 'R'; op.ev = (void*)event; op.evgen = ++e.gen; op.seq = 0;
    op.rank = CmiMyRank(); op.note = note;
    HapiTraceEvRec& rec = e.last[e.nlast % 4]; e.nlast++;
    rec.stream = (void*)stream; rec.idx = op.idx; rec.gen = e.gen;
    rec.rank = op.rank; rec.note = note;
  }
  CmiUnlock(hapi_trace_lock);
  return r;
}
hapiError_t hapiStreamWaitEventNoted(hapiStream_t stream, hapiEvent_t event,
                                     unsigned int flags, int note) {
  if (!hapi_trace_on) return hapiStreamWaitEventRaw(stream, event, flags);
  CmiLock(hapi_trace_lock);
  const hapiError_t r = hapiStreamWaitEventRaw(stream, event, flags);
  if (r == hapiSuccess) {
    auto it = hapi_trace_events->find((void*)event);
    const uint64_t gen = (it == hapi_trace_events->end()) ? 0 : it->second.gen;
    HapiTraceOp& op = hapiTracePush((void*)stream);
    op.kind = 'W'; op.ev = (void*)event; op.evgen = gen; op.seq = 0;
    op.rank = CmiMyRank(); op.note = note;
  }
  CmiUnlock(hapi_trace_lock);
  return r;
}

// Has PE `rank`'s flag `seq` landed? True also once the slot has moved on to
// a later sequence number, which can only happen after this one was drained.
static bool hapiTraceFlagLanded(int rank, uint32_t seq) {
  uint32_t* slots = CpvAccessOther(hapi_flag_slots, rank);
  if (slots == NULL) return false;
  volatile uint32_t* slot = slots + (size_t)(seq & (HAPI_FLAG_SLOTS - 1)) * HAPI_FLAG_STRIDE;
  return (int32_t)(*slot - seq) >= 0;
}

// Has `stream` executed past op `idx`? 1: a flag issued at or after idx has
// landed. 0: the first flag after idx has not landed (out_seq says which).
// -1: no flag after idx in the ring, so nothing to go on.
static int hapiTraceStreamReached(void* stream, uint64_t idx, uint32_t* out_seq, int* out_rank) {
  auto it = hapi_trace_streams->find(stream);
  if (it == hapi_trace_streams->end()) return -1;
  HapiTraceStream& s = it->second;
  const uint64_t lo = s.n > HAPI_TRACE_RING ? s.n - HAPI_TRACE_RING : 0;
  for (uint64_t i = lo; i < s.n; i++) {
    HapiTraceOp& op = s.ops[i % HAPI_TRACE_RING];
    if (op.kind != 'F' || op.idx < idx) continue;
    *out_seq = op.seq; *out_rank = op.rank;
    return hapiTraceFlagLanded(op.rank, op.seq) ? 1 : 0;
  }
  return -1;
}

// Walk one stalled stream: list what was enqueued on it since its last landed
// flag, decide for each wait whether the record it captured has executed, and
// follow the first one that has not into the stream it was recorded on.
static void hapiTraceWalk(void* stream, int depth, std::vector<void*>& path) {
  char ind[32]; snprintf(ind, sizeof ind, "%*s", depth * 2, "");
  if (depth > 8) { CkPrintf("[trace]%s ... depth limit\n", ind); return; }
  for (void* p : path)
    if (p == stream) { CkPrintf("[trace]%s CYCLE: back to stream %p\n", ind, stream); return; }
  path.push_back(stream);
  auto it = hapi_trace_streams->find(stream);
  if (it == hapi_trace_streams->end()) {
    CkPrintf("[trace]%s stream %p: no trace (nothing recorded, waited or flagged through hapi)\n", ind, stream);
    return;
  }
  HapiTraceStream& s = it->second;
  const uint64_t lo = s.n > HAPI_TRACE_RING ? s.n - HAPI_TRACE_RING : 0;
  // The window starts after the newest flag that has landed.
  uint64_t start = lo;
  for (uint64_t i = lo; i < s.n; i++) {
    HapiTraceOp& op = s.ops[i % HAPI_TRACE_RING];
    if (op.kind == 'F' && hapiTraceFlagLanded(op.rank, op.seq)) start = op.idx + 1;
  }
  CkPrintf("[trace]%s stream %p: %llu ops logged, window [%llu,%llu) since last landed flag\n",
           ind, stream, (unsigned long long)s.n, (unsigned long long)start,
           (unsigned long long)s.n);
  void* follow = NULL;
  for (uint64_t i = start; i < s.n; i++) {
    HapiTraceOp& op = s.ops[i % HAPI_TRACE_RING];
    if (op.kind == 'F') {
      CkPrintf("[trace]%s  #%llu F seq=%u pe-rank=%d %s\n", ind, (unsigned long long)op.idx,
               op.seq, op.rank, hapiTraceFlagLanded(op.rank, op.seq) ? "landed" : "NOT landed");
    } else if (op.kind == 'R') {
      CkPrintf("[trace]%s  #%llu R ev=%p gen=%llu note=%d\n", ind, (unsigned long long)op.idx,
               op.ev, (unsigned long long)op.evgen, op.note);
    } else if (op.kind == 'W') {
      const hapiError_t q = hapiEventQuery((hapiEvent_t)op.ev);
      hapiGetLastError();
      const char* qs = (q == hapiSuccess) ? "done" : (q == hapiErrorNotReady ? "NOT-READY" : "ERR");
      auto et = hapi_trace_events->find(op.ev);
      if (et == hapi_trace_events->end()) {
        CkPrintf("[trace]%s  #%llu W ev=%p note=%d EXTERNAL (recorded outside this process) latest-record query=%s\n",
                 ind, (unsigned long long)op.idx, op.ev, op.note, qs);
        if (q == hapiErrorNotReady && follow == NULL) {
          CkPrintf("[trace]%s  -> blocked on an external record (see the peer's [trace] for note=%d)\n", ind, op.note);
          follow = (void*)1;  // resolved: external
        }
        continue;
      }
      HapiTraceEvent& e = et->second;
      if (op.evgen == 0) {
        CkPrintf("[trace]%s  #%llu W ev=%p captured NO record (no-op)\n", ind, (unsigned long long)op.idx, op.ev);
        continue;
      }
      const HapiTraceEvRec* rec = NULL;
      for (int k = 0; k < 4 && k < e.nlast; k++)
        if (e.last[k].gen == op.evgen) rec = &e.last[k];
      if (rec == NULL) {
        CkPrintf("[trace]%s  #%llu W ev=%p gen=%llu (now %llu): record fell out of the ring; latest query=%s\n",
                 ind, (unsigned long long)op.idx, op.ev, (unsigned long long)op.evgen,
                 (unsigned long long)e.gen, qs);
        continue;
      }
      uint32_t fseq = 0; int frank = -1;
      const int reached = hapiTraceStreamReached(rec->stream, rec->idx, &fseq, &frank);
      const bool aliased = (e.gen != op.evgen);
      const char* verdict =
          reached == 1 ? "SATISFIED (a later flag on that stream landed)" :
          reached == 0 ? "UNSATISFIED (that stream's next flag has not landed)" :
          (!aliased ? (q == hapiSuccess ? "SATISFIED (event query done)" :
                       q == hapiErrorNotReady ? "UNSATISFIED (event query not ready)" : "ERR") :
                      "UNKNOWN (re-recorded since, no flag after the record)");
      CkPrintf("[trace]%s  #%llu W ev=%p gen=%llu%s -> recorded on stream %p at #%llu (pe-rank %d, note %d): %s\n",
               ind, (unsigned long long)op.idx, op.ev, (unsigned long long)op.evgen,
               aliased ? " [RE-RECORDED since]" : "", rec->stream, (unsigned long long)rec->idx,
               rec->rank, rec->note, verdict);
      const bool blocked = (reached == 0) || (reached == -1 && (aliased || q == hapiErrorNotReady));
      if (blocked && follow == NULL) follow = rec->stream;
    }
  }
  if (follow == NULL) {
    CkPrintf("[trace]%s  => every wait in the window is satisfied: this stream is stuck on a "
             "non-wait op (its flag write, a copy, or a kernel)\n", ind);
  } else if (follow != (void*)1) {
    CkPrintf("[trace]%s  => following the first unsatisfied wait into stream %p\n", ind, follow);
    hapiTraceWalk(follow, depth + 1, path);
  }
}

int firstRankForDevice = 0; // First rank for each device, used for mapping

// Managing memory state in server
int hapiAllocId = 0; // Global allocation ID for HAPI

// Used to invoke user's Charm++ callback function
void (*hapiInvokeCallback)(void*, void*) = NULL;

// Functions used to support quiescence detection.
void (*hapiQdCreate)(int) = NULL;
void (*hapiQdProcess)(int) = NULL;

#define MAX_PINNED_REQ 64
#define MAX_DELAYED_FREE_REQS 64

// Declare GPU Manager as a process-shared object.
CsvDeclare(GPUManager, gpu_manager);

CpvDeclare(int, my_device); // GPU device that this thread is mapped to
CpvDeclare(int, my_device_id); // index to the deviceManager that stores info about the device
CpvDeclare(bool, device_rep); // Is this PE a device representative thread? (1 per device)

void hapiSendMemoryRequest(char* msg, int size);

// See gpumanager.h for what this indexes and why it is cached.
CpvDeclare(int, my_node_rank_local);

void hapiRefreshTopologyCache() {
  CpvAccess(my_node_rank_local) = CmiNodeRankLocal(CmiMyPe());
}

// HAPI internal function declarations
static void hapiInitCsv(char** argv);
static void hapiInitCpv();
static void hapiExitCsv();

static void hapiMapping(char** argv);
static void hapiRegisterCallbacks();

// CUDA IPC related functions
static void shmInit();
static void shmSetup();
static void shmCreate();
static void shmOpen();
static void shmMap();
static void shmAbort();
static void shmCleanup();
static void ipcHandleCreate();
void* hapiIpcImportBuffer(const hapiIpcMemHandle_t& handle, int src_process,
                          const void* src_base, size_t span);
static void ipcHandleOpen();

#ifdef CMK_LBDB_ON
// Sentinel external-correlation ID meaning "no owning migratable object".
// Must not collide with a real chare ID -- 0 is a perfectly valid one.
static constexpr uint64_t HAPI_CUPTI_NO_OBJECT =
    GpuObjectTokenTable::noObjectToken();

// The flush gate. Since the lock-free close (2026-10-23, see the deferred close below) the LB window close on CUDA
// flushes nothing and takes no gate; the gated flush remains for the +gputrace stop, the exit flush and the HIP
// close. cuptiActivityFlushAll on one thread (the LB helper, or the last PE of the process to reach the
// window close) races with the kernel-launch intercept of another PE thread inside CUPTI: the +LBAsync segfault
// (job 22809309, two more with the retired-buffer quarantine in place; every backtrace is the first launch of the
// next step, cudaLaunchKernel -> libcupti, at a window close). Every driver call the runtime issues for a chare goes
// through hapiSubmitIssue / hapiSubmitClosure / hapiSubmitEventRecord / hapiSubmitMemcpyAsync, so those take the gate
// shared and the flush takes it exclusive: no launch is in flight while CUPTI flushes, and none starts until it is
// done. A PE blocks at most for the flush (~3 ms per window). Nested submits on one thread hold one read lock.
// One gate per device of the process, held shared by that device's PEs for their launches; the flush takes every
// gate. There is no per-device flush: cuptiActivityFlush(context, stream, flag) is deprecated and "CONTEXT and
// STREAMID will be ignored" (cupti_activity.h), so a window close flushes the process, and the close is one per
// process per LB event (hapiDeviceGroupSize).
// The gates PREFER THE WRITER. glibc's default rwlock prefers readers: new readers keep acquiring while a writer
// waits, so the flush's write lock waited for a moment with no launch in flight on that device -- ~3.5 ms per gate
// with four PEs launching every ~60 us, taken for every device in turn: 5 ms in a one-device process, 14.5 ms in a
// four-device one, independent of buffer sizes, with the PEs' own gate waits at zero (jobs 22866653, 22868252).
// With writer preference a waiting flush stops new launches at once and gets the gate when the in-flight ones
// finish (microseconds); the PEs then wait for the flush call itself. Our readers never recurse at the lock level
// (one read lock per thread, t_cupti_gate_depth), which the NONRECURSIVE kind requires.
#define HAPI_CUPTI_MAX_GATES 32
// HAPI_CUPTI_FLUSH_FORCED (build-time): 1, the default since 2026-10-22, is the forced flush behind the gates at the
// window close: every buffer delivered, the records complete, the coverage scale 1. 0 is the NON-forced flush, the
// default from 2026-10-17 to 10-22: complete buffers only, no gate taken, the open buffer per thread (64 KB) withheld
// and its tail extrapolated by the coverage scale (hapiNormalizeCuptiLoads). That tail is the last 2-5 steps of
// leanmd 8x4x4 records per thread (cover 0.67-0.93 of the 20-step process window), and the scale is computed over the
// process window while an element's own measurement window under +LBAsync is the few steps between its AtSyncWait
// and its join (4 of 20 at lag 16): the withheld tail took half to all of an element's sample and the scale put back
// 10-30%, differently per thread and per round. DiffusionLB then moved 700-2100 objects per run for the same
// imbalance, and the channel transport, sensitive to the arrangement, ran 200-249 ms/step. Forced, with the same
// window (gpub086, 2026-10-22): cover 1.00, 700-718 moves in every run, the explained device share 89-95%, channels
// 199-200 ms/step -- the sync balancer's figures. The call costs 2-3 ms per close with one GPU per process and 12-14
// ms against the non-forced call's 9-11 with four GPUs per process, no gate waits; the ~15 ms of job 22953167
// (2026-10-17) that motivated the non-forced default was the whole close in that layout, not the forced flush's share.
#ifndef HAPI_CUPTI_FLUSH_FORCED
#define HAPI_CUPTI_FLUSH_FORCED 1
#endif
#define HAPI_CUPTI_GATE_INIT PTHREAD_RWLOCK_WRITER_NONRECURSIVE_INITIALIZER_NP
static pthread_rwlock_t g_cupti_flush_gates[HAPI_CUPTI_MAX_GATES] = {
  HAPI_CUPTI_GATE_INIT, HAPI_CUPTI_GATE_INIT, HAPI_CUPTI_GATE_INIT, HAPI_CUPTI_GATE_INIT,
  HAPI_CUPTI_GATE_INIT, HAPI_CUPTI_GATE_INIT, HAPI_CUPTI_GATE_INIT, HAPI_CUPTI_GATE_INIT,
  HAPI_CUPTI_GATE_INIT, HAPI_CUPTI_GATE_INIT, HAPI_CUPTI_GATE_INIT, HAPI_CUPTI_GATE_INIT,
  HAPI_CUPTI_GATE_INIT, HAPI_CUPTI_GATE_INIT, HAPI_CUPTI_GATE_INIT, HAPI_CUPTI_GATE_INIT,
  HAPI_CUPTI_GATE_INIT, HAPI_CUPTI_GATE_INIT, HAPI_CUPTI_GATE_INIT, HAPI_CUPTI_GATE_INIT,
  HAPI_CUPTI_GATE_INIT, HAPI_CUPTI_GATE_INIT, HAPI_CUPTI_GATE_INIT, HAPI_CUPTI_GATE_INIT,
  HAPI_CUPTI_GATE_INIT, HAPI_CUPTI_GATE_INIT, HAPI_CUPTI_GATE_INIT, HAPI_CUPTI_GATE_INIT,
  HAPI_CUPTI_GATE_INIT, HAPI_CUPTI_GATE_INIT, HAPI_CUPTI_GATE_INIT, HAPI_CUPTI_GATE_INIT};
// The last gated flush, for the close's timing line (CHARM_LB_CUPTI_TIME): the wait for the gates, the
// cuptiActivityFlushAll call, and the buffers it delivered through cuptiBufferCompleted.
struct CuptiFlushProf { double gate_s = 0.0, call_s = 0.0; uint32_t cbs = 0; uint64_t cb_bytes = 0; };
static CuptiFlushProf g_cupti_flush_prof;
static bool g_cupti_flush_counting = false;   // set by the flushing thread around the call; read under cupti_queue_lock_
static uint32_t g_cupti_flush_cbs = 0;
static uint64_t g_cupti_flush_cb_bytes = 0;
static thread_local int t_cupti_gate_dev = 0;   // this thread's device (local index); set per PE in hapiMapping
// The window a drain covers, in the CUPTI clock: from the previous LB window close to this one (stamped in
// hapiPrepareCuptiLoadsImpl just before its flush). Records that ended before the window belong to the previous
// one and are dropped by the sweep; a kernel straddling the start is clipped to it.
static uint64_t g_cupti_window_prev_ns = 0, g_cupti_window_close_ns = 0;
// Per-thread coverage of the window, this drain: the last runtime-API record each thread delivered. With the
// non-forced flush each thread's OPEN activity buffer is withheld until it fills, so a thread's records end
// somewhere before the close -- a random fraction of a buffer per thread (up to 64 KB, ~3 sph3d steps of a
// 25-step window). The thread's objects' loads are scaled by window / covered span (hapiNormalizeCuptiLoads):
// a steady load extrapolated over the tail it has not reported yet. Under the forced flush the coverage is the
// whole window and the scale 1. Statistics of the last normalisation, for the timed close line.
static std::unordered_map<uint32_t, uint64_t> g_cupti_cover_end;    // thread -> last API record end this drain
static std::unordered_map<uint32_t, double> g_cupti_cover_scale;    // thread -> window / covered
static std::unordered_map<LDObjKey, uint32_t, LDObjKeyHash> g_cupti_obj_thread;   // object -> thread of its launches
static double g_cupti_cover_min = 1.0, g_cupti_cover_scale_max = 1.0;
static size_t g_cupti_cover_threads = 0, g_cupti_cover_capped = 0;
static thread_local int t_cupti_gate_depth = 0;
// CHARM_LB_CUPTI_TIME: the window close prints its phase timings (see hapiPrepareCuptiLoadsImpl).
static bool hapiCuptiTimeIt() { static const bool t = (getenv("CHARM_LB_CUPTI_TIME") != nullptr); return t; }

// Entry-span log: which LB object each thread was running, and when.
//
// Replaces the CUPTI external-correlation push at every entry method start and the pop at its
// end. Measured 2026-10-12 (job 22849462, sph3d 256 patches on two nodes, ~600 pushes per PE per
// step): the pair cost 0.5-4 us in a 4-PE process and 2.5-15 us in a 16-PE process, 4.5-10 ms of
// every step on the (1,4,4) layout against 0.4-3 ms on (4,1,4) -- the one-process layout's whole
// steady-state penalty. CUPTI keeps the per-thread correlation stacks behind one process-wide
// lock, so the cost grows with the threads in the process. Here the thread appends a span
// {start, end, token} to its own log (two CUPTI timestamps and a vector push under its own,
// uncontended mutex), and the window close joins each runtime API record to the span of its
// thread that contains the record's start; the API record's correlation id then names the kernel
// exactly as the external-correlation record did (same tables, same deferral, same carry-over).
// Spans nest (an entry delivered inline inside another); the innermost wins, and `parent_delta`
// points back at the enclosing span. The work-tag lane (CUSTOM0, kernel scaling) is the same
// mechanism on a second lane.
struct CuptiEntrySpan { uint64_t start, end; uint64_t token; uint32_t parent_delta, pad; };
struct CuptiEntryLane {
  std::vector<CuptiEntrySpan> spans;
  std::vector<uint32_t> open;   // indices of the open spans, innermost last (ascending)
  uint64_t pruned = 0;          // spans dropped from the front so far: absolute index = pruned + local
  uint32_t gen = 0;             // bumped by a compacting prune: the copies rebuild
};
struct CuptiEntryLog {
  std::mutex m;
  CuptiEntryLane obj, tag;
  uint64_t arrivals[3] = {0, 0, 0};    // the owner's last window-close arrivals: the prune horizon
  int narrivals = 0;
};
static thread_local CuptiEntryLog* t_cupti_entry_log = nullptr;
static std::mutex g_cupti_entry_logs_lock;
static std::unordered_map<uint32_t, CuptiEntryLog*> g_cupti_entry_logs;   // by OS thread id (gettid), as the API records name threads
// The span clock. cuptiGetTimestamp goes through CUPTI (measured 15:16 job 22849462: 6.2 us per
// open+close pair in the 16-PE process, no better than the push/pop it replaced), so spans are
// stamped with clock_gettime on the clock CUPTI's timestamps follow, plus the offset between the
// two measured at tracing start and refreshed at every window-close arrival. The clock is the
// candidate whose offset is smallest at calibration; if none is within 10 ms CUPTI's own clock is
// used directly (correct, slow).
static std::atomic<int> g_cupti_span_clock{-1};        // clockid_t, or -1: cuptiGetTimestamp directly
static std::atomic<int64_t> g_cupti_span_offset{0};    // cupti_ns - clock_ns
static inline uint64_t hapiClockNs(clockid_t c) {
  struct timespec ts;
  clock_gettime(c, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
// The offset of CUPTI's clock from clock c: the tightest of five brackets.
static bool hapiCuptiClockOffset(clockid_t c, int64_t& off) {
  uint64_t spread = UINT64_MAX;
  for (int i = 0; i < 5; i++) {
    const uint64_t a = hapiClockNs(c);
    uint64_t ct = 0;
    ct = hapiTraceBackendTimestamp();
    const uint64_t b = hapiClockNs(c);
    if (b - a < spread) { spread = b - a; off = (int64_t)ct - (int64_t)((a + b) / 2); }
  }
  return true;
}
static void hapiCuptiCalibrateSpanClock(bool first) {
  if (!first) {   // refresh the offset of the clock already chosen
    const int c = g_cupti_span_clock.load(std::memory_order_relaxed);
    int64_t off = 0;
    if (c >= 0 && hapiCuptiClockOffset((clockid_t)c, off)) g_cupti_span_offset.store(off, std::memory_order_relaxed);
    return;
  }
  const clockid_t cands[] = {CLOCK_MONOTONIC_RAW, CLOCK_MONOTONIC, CLOCK_BOOTTIME, CLOCK_REALTIME};
  int best = -1; int64_t bestOff = 0;
  for (clockid_t c : cands) {
    int64_t off = 0;
    if (!hapiCuptiClockOffset(c, off)) continue;
    if (best < 0 || llabs(off) < llabs(bestOff)) { best = (int)c; bestOff = off; }
  }
  if (best < 0 || llabs(bestOff) > 10000000) { best = -1; bestOff = 0; }
  g_cupti_span_offset.store(bestOff, std::memory_order_relaxed);
  g_cupti_span_clock.store(best, std::memory_order_relaxed);
  if (hapiCuptiTimeIt() || _lb_args.debug() > 1)
    CmiPrintf("[LBCUPTI pe=%d] span clock: %s, offset %lld ns\n", CmiMyPe(),
              best == CLOCK_MONOTONIC_RAW ? "CLOCK_MONOTONIC_RAW" : best == CLOCK_MONOTONIC ? "CLOCK_MONOTONIC" :
              best == CLOCK_BOOTTIME ? "CLOCK_BOOTTIME" : best == CLOCK_REALTIME ? "CLOCK_REALTIME" : "cuptiGetTimestamp (no clock within 10 ms)",
              (long long)bestOff);
}
static inline uint64_t hapiCuptiSpanNow() {
  const int c = g_cupti_span_clock.load(std::memory_order_relaxed);
  if (c < 0) return hapiTraceBackendTimestamp();
  return (uint64_t)((int64_t)hapiClockNs((clockid_t)c) + g_cupti_span_offset.load(std::memory_order_relaxed));
}
static CuptiEntryLog& hapiCuptiEntryLog() {
  CuptiEntryLog* L = t_cupti_entry_log;
  if (L == nullptr) {
    L = new CuptiEntryLog;
    std::lock_guard<std::mutex> g(g_cupti_entry_logs_lock);
    g_cupti_entry_logs[(uint32_t)syscall(SYS_gettid)] = L;
    t_cupti_entry_log = L;
  }
  return *L;
}
static inline void hapiCuptiLaneOpen(CuptiEntryLane& ln, uint64_t token) {   // owner, under the log's mutex
  const uint64_t t = hapiCuptiSpanNow();
  const uint32_t idx = (uint32_t)ln.spans.size();
  const uint32_t pd = ln.open.empty() ? 0 : idx - ln.open.back();
  ln.spans.push_back({t, 0, token, pd, 0});
  ln.open.push_back(idx);
}
// One clock read per entry: an outermost span has no end stamp and runs until the next span starts
// on its thread (nothing launches a kernel between entries; the polls there are not attributed
// anyway). Only a nested span is stamped at its close, because its parent resumes. On a node whose
// TSC the kernel distrusts the clocksource is HPET (gpub090, 2026-10-12): 1.7 us per read, serialised
// node-wide, so the reads per entry are what the span log costs there.
static inline void hapiCuptiLaneClose(CuptiEntryLane& ln) {   // owner, under the log's mutex
  if (ln.open.empty()) return;
  const uint32_t idx = ln.open.back();
  ln.open.pop_back();
  if (!ln.open.empty() && idx < ln.spans.size()) ln.spans[idx].end = hapiCuptiSpanNow();
}
// Drop what no later drain can ask about: the spans closed before the owner's arrival two windows
// ago (a record is delivered at most one drain late). Owner, under the log's mutex.
static void hapiCuptiLanePrune(CuptiEntryLane& ln, uint64_t cutoff) {
  if (ln.spans.empty()) return;   // a lane nothing was logged in (the tag lane of an untagged run)
  // Keep from the last span that started before the cutoff (it may still cover times after it).
  size_t k = std::lower_bound(ln.spans.begin(), ln.spans.end(), cutoff,
                              [](const CuptiEntrySpan& sp, uint64_t t) { return sp.start < t; }) - ln.spans.begin();
  if (k > 0) k--;
  // Keep that span's whole nesting context: a record in the gap after a nested span closed is held by
  // an ancestor, so the kept prefix starts at the outermost CLOSED ancestor (an open one is kept by the
  // compaction below; everything a kept span can point at is then kept, by the stack discipline).
  for (;;) {
    const CuptiEntrySpan& sp = ln.spans[k];
    if (sp.parent_delta == 0 || sp.parent_delta > k) break;
    const size_t p = k - sp.parent_delta;
    if (std::binary_search(ln.open.begin(), ln.open.end(), (uint32_t)p)) break;
    k = p;
  }
  if (k == 0) return;
  // Spans below k that are still open are kept, moved to the front in their order (the open stack is
  // ascending); everything else below k goes. Before this the prune stopped at the first open span,
  // so one entry left open for the run (a suspended thread, a lost pop) pinned the whole log and the
  // drains copied all of it every close: the parse of the same records grew 9 -> 38 ms over 1000
  // steps (job 22856533). Compaction renumbers, so the copies rebuild from scratch once (gen).
  size_t nOpenBelow = 0;
  while (nOpenBelow < ln.open.size() && ln.open[nOpenBelow] < k) nOpenBelow++;
  if (nOpenBelow == 0) {
    ln.spans.erase(ln.spans.begin(), ln.spans.begin() + k);
    for (uint32_t& i : ln.open) i -= (uint32_t)k;
    ln.pruned += k;
    return;
  }
  std::vector<CuptiEntrySpan> kept;
  kept.reserve(nOpenBelow + (ln.spans.size() - k));
  for (size_t j = 0; j < nOpenBelow; j++) {
    CuptiEntrySpan sp = ln.spans[ln.open[j]];
    sp.parent_delta = (j == 0) ? 0 : 1;   // an open span's parent is the open span beneath it
    kept.push_back(sp);
  }
  for (size_t i = k; i < ln.spans.size(); i++) {
    CuptiEntrySpan sp = ln.spans[i];
    const size_t newIdx = nOpenBelow + (i - k);
    if (sp.parent_delta != 0) {
      const size_t parentOld = i - sp.parent_delta;
      if (parentOld < k) {
        // A parent below k is one of the kept open spans (a closed parent closes after its children,
        // so a child kept above k with a closed parent below k cannot exist) -- or it was dropped.
        const size_t j = std::lower_bound(ln.open.begin(), ln.open.begin() + nOpenBelow, (uint32_t)parentOld) - ln.open.begin();
        sp.parent_delta = (j < nOpenBelow && ln.open[j] == (uint32_t)parentOld) ? (uint32_t)(newIdx - j) : 0;
      }
    }
    kept.push_back(sp);
  }
  for (size_t j = 0; j < ln.open.size(); j++)
    ln.open[j] = (j < nOpenBelow) ? (uint32_t)j : (uint32_t)(nOpenBelow + (ln.open[j] - k));
  ln.spans.swap(kept);
  ln.pruned += k - nOpenBelow;
  ln.gen++;
}
// The drain's copy of one lane, kept between drains and brought up to date incrementally: the spans
// the owner pruned are dropped from the front, the spans logged since the last copy are appended,
// and the copy restarts from the earliest span that was still open at the last copy (its end may
// have been stamped since). The four device closes of one LB event each drain within milliseconds,
// so the second to fourth copy kilobytes, not every thread's whole log (16 x ~1.5 MB per drain,
// measured 2026-10-12 as the bulk of the close under the span log). Caller holds the owner's mutex.
struct CuptiLaneCopy {
  std::vector<CuptiEntrySpan> spans;
  uint64_t base = 0;                   // absolute index of spans[0]
  uint32_t gen = 0;                    // the lane generation this copy follows
  std::vector<uint32_t> open_at_copy;  // local indices of the spans open at the last copy: the only ones that can change
};
static std::atomic<uint64_t> g_cupti_snap_bytes{0}, g_cupti_snap_ns{0};   // this drain's copies (CHARM_LB_CUPTI_TIME)
static void hapiCuptiLaneRefresh(const CuptiEntryLane& ln, CuptiLaneCopy& cp) {
  const bool timeIt = hapiCuptiTimeIt();
  const uint64_t t0 = timeIt ? hapiClockNs(CLOCK_MONOTONIC) : 0;
  const uint64_t abs_begin = ln.pruned;
  size_t copied = 0;
  if (cp.gen != ln.gen || abs_begin < cp.base) {   // compacted (or a new lane): rebuild
    cp.spans.assign(ln.spans.begin(), ln.spans.end());
    cp.base = abs_begin;
    cp.gen = ln.gen;
    copied = cp.spans.size();
  } else {
    const uint64_t drop = abs_begin - cp.base;
    if (drop >= cp.spans.size()) cp.spans.clear();
    else if (drop > 0) cp.spans.erase(cp.spans.begin(), cp.spans.begin() + drop);
    cp.base = abs_begin;
    // Only a span that was open at the last copy can have changed since (its end stamp); closed
    // spans never change, so the rest of the copy stands and only the new spans are appended.
    for (uint32_t oi : cp.open_at_copy) {
      if (oi < drop) continue;
      const size_t li = oi - drop;
      if (li < cp.spans.size() && li < ln.spans.size()) cp.spans[li].end = ln.spans[li].end;
    }
    const size_t have = cp.spans.size();
    if (have < ln.spans.size()) {
      cp.spans.insert(cp.spans.end(), ln.spans.begin() + have, ln.spans.end());
      copied = ln.spans.size() - have;
    } else if (have > ln.spans.size()) {
      cp.spans.resize(ln.spans.size());
    }
  }
  cp.open_at_copy.assign(ln.open.begin(), ln.open.end());
  if (timeIt) {
    g_cupti_snap_bytes.fetch_add(copied * sizeof(CuptiEntrySpan), std::memory_order_relaxed);
    g_cupti_snap_ns.fetch_add(hapiClockNs(CLOCK_MONOTONIC) - t0, std::memory_order_relaxed);
  }
}
// Reader, on a copy of one lane: the innermost span containing time t, or NULL when t is before the
// first span or inside a closed nested span whose ancestors are gone. `cursor` is the caller's last
// hit for this lane: a thread's records arrive in time order, so the next span is usually the same
// one or a few ahead, and the binary search is the fallback.
static const CuptiEntrySpan* hapiCuptiSpanAt(const std::vector<CuptiEntrySpan>& snap, uint64_t t, size_t& cursor) {
  if (snap.empty() || t < snap.front().start) return nullptr;
  size_t lo = cursor < snap.size() ? cursor : 0;
  if (snap[lo].start <= t) {
    int steps = 0;
    while (lo + 1 < snap.size() && snap[lo + 1].start <= t && ++steps < 64) lo++;
    if (lo + 1 < snap.size() && snap[lo + 1].start <= t) lo = 0;   // far ahead: search
  } else {
    lo = 0;
  }
  if (lo == 0) {
    size_t hi = snap.size();   // the last span with start <= t
    while (hi - lo > 1) {
      const size_t mid = (lo + hi) / 2;
      if (snap[mid].start <= t) lo = mid; else hi = mid;
    }
  }
  cursor = lo;
  for (;;) {
    const CuptiEntrySpan& sp = snap[lo];
    if (sp.end == 0 || sp.end >= t) return &sp;
    if (sp.parent_delta == 0 || sp.parent_delta > lo) return nullptr;   // closed before t: only an ancestor can hold it
    lo -= sp.parent_delta;
  }
}
struct CuptiGateShared {
  CuptiGateShared() { if (t_cupti_gate_depth++ == 0) pthread_rwlock_rdlock(&g_cupti_flush_gates[t_cupti_gate_dev]); }
  ~CuptiGateShared() { if (--t_cupti_gate_depth == 0) pthread_rwlock_unlock(&g_cupti_flush_gates[t_cupti_gate_dev]); }
};
static int hapiCuptiGateCount() {
  const int n = CsvAccess(gpu_manager).device_count;
  return n < 1 ? 1 : (n > HAPI_CUPTI_MAX_GATES ? HAPI_CUPTI_MAX_GATES : n);
}
static void hapiCuptiFlusherWait(bool close);   // the close's flusher thread (CUDA; nothing on HIP)
void hapiCuptiFlushGated(bool forced) {
  if (t_cupti_gate_depth > 0) CmiAbort("hapiCuptiFlushGated: called from inside a gated launch");
  hapiCuptiFlusherWait(false);
  const bool timeIt = hapiCuptiTimeIt();
  // The gate exists for the FORCED flush, which takes every thread's open buffer from under its launch intercept
  // (the +LBAsync segfault, job 22809309). The non-forced flush returns only buffers whose records are complete --
  // what CUPTI's own worker thread does concurrently with launches by design -- and takes no gate: its call still
  // waits for the device queues to drain (13.7 ms in a four-GPU process, soft14 arm of job 22953167), but on the
  // helper thread with the PEs launching through it, that wait stalls nobody.
  const bool gate = forced;
  const int n = gate ? hapiCuptiGateCount() : 0;
  const double t0 = timeIt ? CmiWallTimer() : 0.0;
  for (int d = 0; d < n; d++) pthread_rwlock_wrlock(&g_cupti_flush_gates[d]);
  const double t1 = timeIt ? CmiWallTimer() : 0.0;
  if (timeIt) {
    std::lock_guard<std::mutex> lk(CsvAccess(gpu_manager).cupti_queue_lock_);
    g_cupti_flush_counting = true; g_cupti_flush_cbs = 0; g_cupti_flush_cb_bytes = 0;
  }
  hapiTraceBackendFlush(forced);
  if (timeIt) {
    const double t2 = CmiWallTimer();
    std::lock_guard<std::mutex> lk(CsvAccess(gpu_manager).cupti_queue_lock_);
    g_cupti_flush_counting = false;
    g_cupti_flush_prof.gate_s = t1 - t0; g_cupti_flush_prof.call_s = t2 - t1;
    g_cupti_flush_prof.cbs = g_cupti_flush_cbs; g_cupti_flush_prof.cb_bytes = g_cupti_flush_cb_bytes;
  }
  for (int d = n - 1; d >= 0; d--) pthread_rwlock_unlock(&g_cupti_flush_gates[d]);
}

// Buffers CUPTI handed back are not freed at the drain that parsed them. The forced flush returns buffers that still
// hold incomplete records (cupti_activity.h), and CUPTI completes such a record later, from inside a subsequent CUDA
// call of any thread, by writing into that buffer: freeing it at the drain put those writes into freed memory (the
// +LBAsync segfault in CUPTI's launch intercept, job 22808905 backtraces; silent heap corruption when glibc kept the
// chunk mapped). A retired buffer is kept for two more drains (hundreds of ms at any LB period; the late writes
// come within the kernel's completion), then freed. Only the drain touches this (cupti_prepare_lock_).
struct RetiredCuptiBuffer { uint8_t* buffer; size_t size; uint64_t drain; };
static std::deque<RetiredCuptiBuffer> g_cupti_retired;
static uint64_t g_cupti_drain = 0;
// Out of quarantine, a buffer goes back to CUPTI through cuptiBufferRequested rather than to
// free(): a 5 MB buffer is above glibc's mmap threshold, so free() unmapped it and the next
// request mapped a fresh one that CUPTI then page-faulted in record by record, during the steps
// after every window close. In one process driving four devices those faults are 16 PE threads'
// worth on one address space, and the steps after each close ran ~4 ms slower for a block
// (job 22846356). A reused buffer is already mapped and touched.
static std::mutex g_cupti_free_lock;
static std::vector<std::pair<uint8_t*, size_t>> g_cupti_free;
static void hapiCuptiRetireBuffer(uint8_t* buffer, size_t size) {
  if (buffer != nullptr) g_cupti_retired.push_back({buffer, size, g_cupti_drain});
}
// Buffers leave quarantine ZEROED: CUPTI_ACTIVITY_ATTR_ZEROED_OUT_ACTIVITY_BUFFER is set (hapiAttach), so CUPTI
// takes every buffer cuptiBufferRequested hands it as already zero and skips its own memset -- which it did "in the
// main thread", i.e. inside the gated flush call: 0.3 ms per 1 MB buffer, 5 ms of the (1,4,4) close's 16 ms flush
// (job 22953167 arrivals: one buffer every 0.3 ms, 0.04 ms at 64 KB). The memset runs here instead, on the drain's
// thread (the LB helper), outside g_cupti_free_lock so a PE whose buffer fills mid-window does not wait on it.
static void hapiCuptiFreeRetired(uint64_t keep_drains) {
  std::vector<std::pair<uint8_t*, size_t>> out;
  {
    std::lock_guard<std::mutex> lk(g_cupti_free_lock);
    // Never free()d while the run lasts: a buffer CUPTI once held may still be written by it (the forced flush's
    // incomplete records; the 2026-10-12 "free(): corrupted unsorted chunks" with a timed quarantine), and the
    // working set is bounded by what CUPTI asked for (~3 buffers per thread), so the free list just keeps them.
    while (!g_cupti_retired.empty() && g_cupti_retired.front().drain + keep_drains <= g_cupti_drain) {
      out.emplace_back(g_cupti_retired.front().buffer, g_cupti_retired.front().size);
      g_cupti_retired.pop_front();
    }
  }
  if (out.empty()) return;
  for (auto& b : out) memset(b.first, 0, b.second);
  std::lock_guard<std::mutex> lk(g_cupti_free_lock);
  for (auto& b : out) g_cupti_free.push_back(b);
}

// ---- The deferred close ------------------------------------------------------------------------------------
// At AtSync the window is only MARKED. Kernels issued before the mark may still be queued or running, and the
// record that carries a kernel's load exists only once the kernel has completed. On HIP it is written by ROCr's
// single async-signal thread, which on a loaded GCD runs 20-50k records behind the device (Frontier 2026-10-23).
// On CUDA it is reserved in the launching thread's activity buffer at the launch and its timestamps are written
// when the kernel completes: the forced flush at the close handed the record of every kernel still in flight
// back incomplete. A snapshot taken at the mark therefore missed the tail of the window -- exactly the last steps
// of every element's sample under +LBAsync -- and on Frontier the most loaded process read a fifth to a half of
// its device load (DiffusionLB then moved 1,300-2,000 objects into it and the next window ran 136 ms/step
// against 75). The last PE to arrive now calls hapiCuptiCloseBegin, keeps running, and polls
// hapiCuptiCloseComplete from its scheduler loop until the records of the work issued before the mark are in
// (per backend, below) or the bound (CHARM_CLOSE_WAIT_MS, default 1000) has passed. Records of work issued after
// the mark are set aside for the next drain (hapiProcessCuptiBuffers), so a round's accounting is exact and
// nothing is clipped. Under the barrier the PEs wait anyway; under +LBAsync the application runs on and the
// strategy starts when the records are in.
struct CuptiCloseWait {
  bool active = false;
  uint64_t mark = 0;               // the window close, in the records' clock
  uint32_t corr_at_mark = 0;       // HIP: the largest correlation id of a call that began before the mark
  double t0 = 0.0, last_poll = 0.0;
  size_t scanned = 0;              // HIP: queue items already scanned
  std::unordered_set<uint32_t> pending;        // HIP: launches before the mark without a completion record
  std::unordered_set<uint32_t> unmatched_pre;  // HIP: completion records (id <= corr_at_mark) with no launch record:
                                               // a packet written under an untraced call carries an id of its own
  unsigned idle = 0, polls = 0, stall = 0;
  size_t best_gap = (size_t)-1;     // HIP: the smallest (pending - unmatched_pre) seen: progress means pre-mark records still arrive
  uint64_t launches = 0, kernels = 0, kernels_pre = 0, arrived_in_wait = 0;
  // CUDA: the activity buffers CUPTI held at the mark and not yet returned, and the default flushes of the wait.
  std::vector<uint8_t*> bufs;
  size_t bufs_at_mark = 0, bufs_at_end = 0;
  size_t mark_bytes_max = 0, mark_bytes_sum = 0;   // CUDA: sizes of the buffers held at the mark
  double small_ms_before_mark = -1.0;              // CUDA: small buffers handed out since the first arrival, ms
  unsigned long long small_handed = 0;
  unsigned flushes = 0;                       // CUDA: flushes the wait asked the flusher thread for
  double flush_gap_ms = 10.0;                 // CUDA: the interval to the next ask; doubles each ask up to 100 ms
  uint64_t flush_calls_at_mark = 0, flusher_calls = 0;   // CUDA: the flusher's calls, and those during the wait
  double flush_ms = 0.0, flush_ms_at_mark = 0.0, last_flush = 0.0;   // CUDA: the flusher's time in them
  const char* ended_by = "";        // CUDA: "returned" (every buffer held at the mark is back), "bound"
  bool timed_out = false;
  double waited_ms = 0.0;          // the last close, for the timing print
  size_t pending_at_end = 0, unmatched_at_end = 0;
};
static CuptiCloseWait g_cupti_close;
static uint64_t g_cupti_mark_ns = 0;      // set while a deferred close is in progress or awaiting its drain
static bool g_cupti_mark_set = false;
static bool g_cupti_drain_deferred = false;   // this drain's records are complete to the mark: count them whole
static bool g_cupti_close_flushless = false;   // CUDA: the close got its buffers back; the build does not flush
// The activity buffers CUPTI holds: handed out by cuptiBufferRequested, not yet back through cuptiBufferCompleted.
// The deferred close on CUDA waits for those it held at the mark.
static std::mutex g_cupti_out_lock;
static std::unordered_set<uint8_t*> g_cupti_outstanding;
static std::unordered_map<uint8_t*, size_t> g_cupti_out_size;   // the size of each, for the close's print
// Small buffers around the mark. CUPTI returns a buffer only once every record in it is complete, and the buffer open
// at the mark keeps taking the launches that follow it: under +LBAsync the busiest process's host runs ~1 s ahead of
// its GPU, so with 32 KB buffers its close waited 892 ms while only 92 kernels issued before the mark were still
// queued (~70 ms of work; leanmd 16x8x8, job 23168861, 2026-10-24). CUPTI has no call that starts a new buffer, but
// every buffer comes from cuptiBufferRequested: from the first PE of the process to reach the LB step until the
// close completes, it hands out kCuptiMarkBufBytes buffers, so the one open at the mark holds a handful of launches.
static const size_t kCuptiMarkBufBytes = 4 * 1024;
static std::atomic<bool> g_cupti_small_bufs{false};
static std::atomic<unsigned long long> g_cupti_small_handed{0};
static double g_cupti_first_arrival_t = 0.0;   // CmiWallTimer of the process's first arrival this round
// Kernel records without timestamps in the last normalisation, owned by an object or not (the close line): on CUDA
// the records of kernels a forced flush caught in flight (see the deferred close on CUDA).
static size_t g_cupti_untimed_owned = 0, g_cupti_untimed_unowned = 0;
static double hapiCuptiCloseBoundMs() {
  static const double ms = getenv("CHARM_CLOSE_WAIT_MS") ? atof(getenv("CHARM_CLOSE_WAIT_MS")) : 1000.0;
  return ms;
}

#if CMK_CUDA
static void CUPTIAPI cuptiBufferRequested(uint8_t **buffer, size_t *size, size_t *maxNumRecords) {
  // CUPTI writes activity records straight into this buffer and has no way to
  // tell us the allocation failed: hand it NULL and it writes through a null
  // pointer, and the NULL comes back through cuptiBufferCompleted to be parsed
  // later, so the fault surfaces inside cuptiActivityGetNextRecord with nothing
  // left to say where it came from. Step down to a smaller buffer before giving
  // up, and if even that fails, say so here.
  // CHARM_LB_CUPTI_BUFKB=<kb>: the buffer size. CUPTI keeps one activity buffer per thread
  // (CUPTI_ACTIVITY_ATTR_PER_THREAD_ACTIVITY_BUFFER defaults to 1) and the forced flush at the window close
  // hands back every thread's buffer, full or not, each replaced by a fresh one that CUPTI zeroes before
  // writing: at the 5 MB default of 2026-10-12 that was 80 MB of buffers per flush in a 16-PE process. A PE
  // writes ~0.6 MB of records per 25-step sph3d window; a buffer that fills is delivered early and parsed at
  // the next close, so 1 MB (the default since 2026-10-13) loses nothing. The non-forced flush is not an option:
  // it withholds every buffer with an incomplete record, so the window's tail went missing (256 KB = up to 4 steps
  // of a 10-step sph3d window: every process's load jumped by a random fraction each window and MetisLB re-mapped
  // at all 80 events, 46.2 vs 40.5 ms/step) and at 16 KB the flush itself took 71 ms per drain.
  // Under the non-forced flush (HAPI_CUPTI_FLUSH_FORCED 0) the open buffer per thread is the window's unreported
  // tail, so 64 KB (~3 sph3d steps of a 25-step window; the coverage scale of hapiNormalizeCuptiLoads extrapolates it).
  // Since the lock-free close (2026-10-23) the buffers are one per context, shared by its threads, and the close
  // waits for the one open at the mark to come back, which it does once FULL and complete: while the context keeps
  // launching an incomplete record is always in it, so no default flush returns it. Its size is therefore the
  // close's wait beyond the device's queued work. 32 KB (~130 launches with their API records) fills inside the
  // step's launch burst; 256 KB did not on the busiest 16x8x8 rank, which issues a step in a burst and then waits
  // ~2 s on the GPU, so the buffer open at the mark filled only in the next step's burst: 3 of 4 of that rank's
  // closes hit the 1 s bound and one waited 2.7 s under a 3 s bound, against 0.84-0.95 s at 32 KB (its queued work;
  // 100-150 ms at 8x4x4). The drain's time did not change (0.12 s per close at 16x8x8 at both sizes) and the extra
  // buffer callbacks are ~40 per second per process (job 23143184, A40 2026-10-23).
  static const size_t first = getenv("CHARM_LB_CUPTI_BUFKB") ? (size_t)atoi(getenv("CHARM_LB_CUPTI_BUFKB")) * 1024
                              : 32*1024;
  const bool small = g_cupti_small_bufs.load(std::memory_order_relaxed);
  {
    // Recycled buffers of the size class wanted now: small ones only around the mark, the others otherwise.
    std::lock_guard<std::mutex> lk(g_cupti_free_lock);
    for (size_t i = g_cupti_free.size(); i-- > 0;) {
      if ((g_cupti_free[i].second <= kCuptiMarkBufBytes) != small) continue;
      *buffer = g_cupti_free[i].first;
      *size = g_cupti_free[i].second;
      *maxNumRecords = 0;
      g_cupti_free[i] = g_cupti_free.back();
      g_cupti_free.pop_back();
      if (small) g_cupti_small_handed++;
      std::lock_guard<std::mutex> o(g_cupti_out_lock);
      g_cupti_outstanding.insert(*buffer);
      g_cupti_out_size[*buffer] = *size;
      return;
    }
  }
  if (small) {
    *buffer = (uint8_t *)malloc(kCuptiMarkBufBytes);
    if (*buffer != NULL) {
      memset(*buffer, 0, kCuptiMarkBufBytes);
      *size = kCuptiMarkBufBytes;
      *maxNumRecords = 0;
      g_cupti_small_handed++;
      std::lock_guard<std::mutex> o(g_cupti_out_lock);
      g_cupti_outstanding.insert(*buffer);
      g_cupti_out_size[*buffer] = *size;
      return;
    }
  }
  const size_t sizes[] = {first, 32*1024, 16*1024};
  for (size_t s : sizes) {
    *buffer = (uint8_t *)malloc(s);
    if (*buffer != NULL) {
      memset(*buffer, 0, s);   // fault the pages in here, once, not under CUPTI's record writes
      *size = s;
      *maxNumRecords = 0;
      std::lock_guard<std::mutex> o(g_cupti_out_lock);
      g_cupti_outstanding.insert(*buffer);
      g_cupti_out_size[*buffer] = *size;
      return;
    }
  }
  CmiAbort("HAPI: could not allocate a CUPTI activity buffer (tried down to "
           "%zu bytes). GPU load instrumentation cannot continue.", sizes[2]);
}

static void CUPTIAPI cuptiBufferCompleted(CUcontext ctx, uint32_t streamId,
                                          uint8_t *buffer, size_t size, size_t validSize) {
  GPUManager& gm = CsvAccess(gpu_manager);
  {
    std::lock_guard<std::mutex> o(g_cupti_out_lock);
    g_cupti_outstanding.erase(buffer);
    g_cupti_out_size.erase(buffer);
  }
  std::lock_guard<std::mutex> lk(gm.cupti_queue_lock_);
  gm.cupti_buffer_queue_.push_back({buffer, validSize, size, (void*)ctx, nullptr});
  if (g_cupti_flush_counting) { g_cupti_flush_cbs++; g_cupti_flush_cb_bytes += validSize; }
}
// The deferred close on CUDA (see above). CUPTI reserves a kernel's record in an activity buffer at the launch and
// writes its timestamps when the kernel completes. A forced flush hands back every buffer as it stands -- the record
// of a kernel still queued or running comes back with no timestamps and is never completed afterwards (A40
// 2026-10-23: 0-178 of 6-16k kernels per process per close) -- and it races CUPTI's launch intercept, so it ran
// with every launch of the process held on the launch locks: up to 1.4 s on the busiest 16x8x8 rank. The close
// therefore takes no lock and forces nothing. It records the buffers CUPTI holds at the mark and waits until every
// one has come back through cuptiBufferCompleted: CUPTI returns a buffer only when all its records are complete, so
// then every kernel launched before the mark has its timestamps. The buffers are one per context (see
// hapiTraceBackendAttach): one per thread stayed open while its thread kept launching, an incomplete record always
// in it. A full buffer comes back through CUPTI's worker; one that is not full (a context gone quiet) through a
// default flush, asked 10 ms after the mark and then at a doubling interval up to 100 ms (hapiCuptiCloseComplete),
// which runs alongside the launches by design. The buffers are small (32 KB, cuptiBufferRequested) so that the one
// open at the mark fills inside the step's launch burst and the wait is the device's queued work. The drain then
// keeps the kernels launched before the mark (hapiSplitKernelsAfterLaunch) and leaves the rest for the next round.
// The close's default flushes run on a thread of their own, never on a PE: a CUPTI flush, forced or not, returns
// only once the device has run the work queued ahead of it -- 0.5-2.4 s per close on the busiest 16x8x8 ranks
// (leanmd, A40 2026-10-23), the same with CUPTI's and the driver's threads moved off the PEs' cores -- and a PE
// inside that call holds up its objects. One per process, started at the first close, on any core (not the
// creating PE's, which it would inherit). Heap-held and never destroyed (a static condition variable destroyed by
// exit() under a waiting thread blocks the exit, LBOffThread.h). Every flush of the runtime's own waits out a call
// in progress (hapiCuptiFlusherWait), and cuptiFinalize stops the flusher for good.
struct CuptiFlusher {
  std::mutex m;
  std::condition_variable cv;
  bool requested = false, busy = false, closed = false;
  uint64_t calls = 0;
  double total_ms = 0.0;
};
static CuptiFlusher& g_flusher = *new CuptiFlusher;
static void hapiCuptiFlushAsync() {
  static std::once_flag started;
  std::call_once(started, [] {
    std::thread([] {
      cpu_set_t any;
      CPU_ZERO(&any);
      for (int c = 0; c < CPU_SETSIZE; c++) CPU_SET(c, &any);
      sched_setaffinity(0, sizeof(any), &any);   // the kernel keeps the cores the process may use
      CuptiFlusher& f = g_flusher;
      std::unique_lock<std::mutex> lk(f.m);
      for (;;) {
        f.cv.wait(lk, [&f] { return f.requested; });
        f.requested = false;
        if (f.closed) continue;
        f.busy = true;
        lk.unlock();
        const double t0 = CmiWallTimer();
        hapiTraceBackendFlush(false);
        const double t1 = CmiWallTimer();
        lk.lock();
        f.busy = false;
        f.calls++;
        f.total_ms += (t1 - t0) * 1000.0;
        f.cv.notify_all();
      }
    }).detach();
  });
  std::lock_guard<std::mutex> lk(g_flusher.m);
  if (!g_flusher.busy && !g_flusher.requested && !g_flusher.closed) {
    g_flusher.requested = true;
    g_flusher.cv.notify_all();
  }
}
static void hapiCuptiFlusherWait(bool close) {
  std::unique_lock<std::mutex> lk(g_flusher.m);
  if (close) { g_flusher.closed = true; g_flusher.requested = false; }
  g_flusher.cv.wait(lk, [] { return !g_flusher.busy; });
}
void hapiCuptiCloseBegin() {
  CuptiCloseWait& w = g_cupti_close;
  w = CuptiCloseWait();
  if (!hapiCuptiTracingActive()) return;
  {
    std::lock_guard<std::mutex> lk(g_flusher.m);
    w.flush_calls_at_mark = g_flusher.calls;
    w.flush_ms_at_mark = g_flusher.total_ms;
  }
  // No launch lock: the drain assigns a kernel by its launch's correlation id (hapiSplitKernelsAfterLaunch).
  w.mark = hapiTraceBackendTimestamp();
  {
    std::lock_guard<std::mutex> o(g_cupti_out_lock);
    w.bufs.assign(g_cupti_outstanding.begin(), g_cupti_outstanding.end());
    for (uint8_t* b : w.bufs) {
      auto it = g_cupti_out_size.find(b);
      const size_t sz = it == g_cupti_out_size.end() ? 0 : it->second;
      w.mark_bytes_sum += sz;
      if (sz > w.mark_bytes_max) w.mark_bytes_max = sz;
    }
  }
  if (g_cupti_small_bufs.load()) w.small_ms_before_mark = (CmiWallTimer() - g_cupti_first_arrival_t) * 1000.0;
  w.small_handed = g_cupti_small_handed.load();
  w.bufs_at_mark = w.bufs.size();
  w.t0 = CmiWallTimer();
  w.last_poll = w.t0;
  w.last_flush = w.t0;
  w.active = true;
}
bool hapiCuptiCloseComplete() {
  CuptiCloseWait& w = g_cupti_close;
  if (!w.active) { g_cupti_small_bufs.store(false); return true; }
  const double now = CmiWallTimer();
  if ((now - w.last_poll) * 1000.0 < 1.0) return false;   // at most one check per ms
  w.last_poll = now;
  w.polls++;
  auto prune = [&w]() {
    std::lock_guard<std::mutex> o(g_cupti_out_lock);
    size_t keep = 0;
    for (size_t i = 0; i < w.bufs.size(); i++)
      if (g_cupti_outstanding.count(w.bufs[i])) w.bufs[keep++] = w.bufs[i];
    w.bufs.resize(keep);
  };
  prune();
  // CUPTI's worker returns full buffers on its own. A default flush returns only a buffer whose records are all
  // complete, so while the context keeps launching it cannot return the one open at the mark (an incomplete record
  // is always in it) and that buffer comes back once full. The flush is for a context gone quiet -- the barrier, the
  // final close, an idle rank -- whose buffer will never fill. One is asked 10 ms after the mark, then at an interval
  // that doubles to 100 ms: every 10 ms (90 asks per close at 16x8x8, job 23143184) none ever ended a close, the
  // flusher served a fraction of them, and each sat 28-150 ms inside CUPTI on the busy ranks. The call takes no lock
  // and runs alongside the launches (cupti_activity.h).
  if (!w.bufs.empty() && (now - w.last_flush) * 1000.0 >= w.flush_gap_ms) {
    hapiCuptiFlushAsync();   // asks; never waits
    w.flushes++;
    w.last_flush = now;
    w.flush_gap_ms = std::min(100.0, w.flush_gap_ms * 2.0);
  }
  bool done = true;
  if (w.bufs.empty()) w.ended_by = "returned";
  else if ((now - w.t0) * 1000.0 >= hapiCuptiCloseBoundMs()) { w.ended_by = "bound"; w.timed_out = true; }
  else done = false;
  if (!done) return false;
  w.waited_ms = (CmiWallTimer() - w.t0) * 1000.0;
  w.bufs_at_end = w.bufs.size();
  {
    std::lock_guard<std::mutex> lk(g_flusher.m);
    w.flusher_calls = g_flusher.calls - w.flush_calls_at_mark;
    w.flush_ms = g_flusher.total_ms - w.flush_ms_at_mark;
  }
  w.active = false;
  g_cupti_small_bufs.store(false);   // back to the normal size: the buffers held at the mark are in
  g_cupti_mark_ns = w.mark;
  g_cupti_mark_set = true;
  g_cupti_close_flushless = true;
  return true;
}
#else
static void hapiCuptiFlusherWait(bool) {}   // the HIP close flushes from its own poll
// A HIP backend (hapi_trace_rocprof.cpp) converts each buffer callback into a
// HapiTraceBatch and queues it here.
// The deferred close on HIP (see above): each poll flushes and scans what arrived, keeping the set of launches
// issued before the mark that have no completion record yet. The window is complete when that set is empty, or
// when the handler has visibly passed the mark (two consecutive deliveries with kernels and none ending before
// it: a launch whose packet was written under an untraced call carries a correlation id of its own and never
// matches), or when the device has gone quiet, or at the bound.
static bool hapiCuptiIsLaunchCbid(uint32_t cbid) {
  static std::vector<signed char> cache;
  const uint32_t n = hapiTraceBackendCallbackIdCount() + 1;
  if (cache.size() < n) cache.resize(n, -1);
  if (cbid >= n) return false;
  if (cache[cbid] < 0) {
    const char* name = hapiTraceBackendCallbackName(cbid);
    cache[cbid] = (name != nullptr && strstr(name, "Launch") != nullptr) ? 1 : 0;
  }
  return cache[cbid] == 1;
}
void hapiCuptiCloseBegin() {
  CuptiCloseWait& w = g_cupti_close;
  w = CuptiCloseWait();
  if (!hapiCuptiTracingActive()) return;
  w.mark = hapiTraceBackendTimestamp();
  w.t0 = CmiWallTimer();
  w.last_poll = -1.0;
  w.active = true;
}
// The window is complete when every launch issued before the mark is accounted for: matched by its own
// completion record, or explained by a completion record of a pre-mark id that no launch record claims (the
// packet of a launch written under an untraced call gets a correlation id of its own; the ids are one
// counter, so an id at or below the last call that began before the mark names pre-mark work). The
// runtime's completion handler delivers out of completion order by up to hundreds of ms on a loaded GCD
// (Frontier 2026-10-23), so nothing short of this accounting says the tail is in. A device gone idle
// (20 polls with no record) or the bound ends it too.
bool hapiCuptiCloseComplete() {
  CuptiCloseWait& w = g_cupti_close;
  if (!w.active) return true;
  const double bound_ms = hapiCuptiCloseBoundMs();
  const double now = CmiWallTimer();
  if (w.last_poll >= 0.0 && (now - w.last_poll) * 1000.0 < 1.0) return false;   // at most one flush per ms
  const bool first = (w.last_poll < 0.0);
  w.last_poll = now;
  w.polls++;
  hapiTraceBackendFlush(false);
  GPUManager& gm = CsvAccess(gpu_manager);
  uint64_t newKernels = 0;
  {
    std::lock_guard<std::mutex> lk(gm.cupti_queue_lock_);
    // The API records are synchronous: after the first flush the largest pre-mark correlation id is known.
    if (first)
      for (size_t i = 0; i < gm.cupti_buffer_queue_.size(); i++) {
        const HapiTraceBatch* b = gm.cupti_buffer_queue_[i].batch;
        if (b == nullptr) continue;
        for (const HapiTraceApiRec& a : b->apis)
          if (a.start < w.mark && a.correlationId > w.corr_at_mark) w.corr_at_mark = a.correlationId;
      }
    for (; w.scanned < gm.cupti_buffer_queue_.size(); w.scanned++) {
      const HapiTraceBatch* b = gm.cupti_buffer_queue_[w.scanned].batch;
      if (b == nullptr) continue;   // CUDA: nothing to scan here
      for (const HapiTraceApiRec& a : b->apis) {
        if (a.start >= w.mark || !hapiCuptiIsLaunchCbid(a.cbid)) continue;
        w.launches++;
        if (a.correlationId > w.corr_at_mark) w.corr_at_mark = a.correlationId;
        if (w.unmatched_pre.erase(a.correlationId) == 0) w.pending.insert(a.correlationId);
      }
      for (const HapiTraceKernelRec& k : b->kernels) {
        newKernels++;
        if (!first) w.arrived_in_wait++;
        if (k.correlationId > w.corr_at_mark) continue;   // issued after the mark: the next round's
        w.kernels_pre++;
        if (w.pending.erase(k.correlationId) == 0) w.unmatched_pre.insert(k.correlationId);
      }
    }
  }
  w.kernels += newKernels;
  // Done when every pre-mark launch is accounted for, or when the accounting stops making progress while
  // records keep arriving: a launch whose packet was written under a call that began after the mark has a
  // post-mark id, so a small residual gap is structural and never closes (300-900 of 130-200k launches per
  // close, Frontier 2026-10-23; waiting for it timed out at the bound every time the device was busy, the
  // LB step overran the lag and the elements parked).
  bool done = false;
  const size_t gap = w.pending.size() > w.unmatched_pre.size() ? w.pending.size() - w.unmatched_pre.size() : 0;
  if (gap == 0) done = true;
  else if (gap < w.best_gap) { w.best_gap = gap; w.stall = 0; w.idle = 0; }
  else if (newKernels > 0) { w.idle = 0; if (++w.stall >= 10) done = true; }
  else if (++w.idle >= 20) done = true;
  if (!done && (now - w.t0) * 1000.0 >= bound_ms) { done = true; w.timed_out = true; }
  if (!done) return false;
  w.waited_ms = (CmiWallTimer() - w.t0) * 1000.0;
  w.pending_at_end = w.pending.size();
  w.unmatched_at_end = w.unmatched_pre.size();
  w.active = false;
  g_cupti_mark_ns = w.mark;
  g_cupti_mark_set = true;
  return true;
}

void hapiTraceBackendDeliver(HapiTraceBatch* batch, size_t approxBytes) {
  GPUManager& gm = CsvAccess(gpu_manager);
  std::lock_guard<std::mutex> lk(gm.cupti_queue_lock_);
  gm.cupti_buffer_queue_.push_back({nullptr, approxBytes, 0, nullptr, batch});
  if (g_cupti_flush_counting) { g_cupti_flush_cbs++; g_cupti_flush_cb_bytes += approxBytes; }
}
#endif

// Populate DeviceManager with device attributes needed to compute per-kernel
// SM usage from CUPTI records. Queried once per local device.
static void hapiPopulateDeviceProps(GPUManager& gm) {
  for (DeviceManager& dm : gm.device_managers) {
    if (dm.props_initialized) continue;
    int dev = dm.global_index;
    hapiDeviceProp props;
    hapiCheck(hapiGetDeviceProperties(&props, dev));

    dm.multi_processor_count = props.multiProcessorCount;
    dm.max_threads_per_sm = props.maxThreadsPerMultiProcessor;
#ifdef hapiDevAttrMaxBlocksPerMultiprocessor
    hapiCheck(hapiDeviceGetAttribute(&dm.max_blocks_per_sm,
                                     hapiDevAttrMaxBlocksPerMultiprocessor, dev));
#else
    dm.max_blocks_per_sm = 0;
#endif
    dm.max_registers_per_sm = props.regsPerMultiprocessor;
    dm.max_shared_mem_per_sm = static_cast<int>(props.sharedMemPerMultiprocessor);
    dm.warp_size = props.warpSize;

    GpuDeviceDescriptor& descriptor = dm.descriptor;
    descriptor.instanceId =
        (static_cast<uint64_t>(static_cast<uint32_t>(CmiPhysicalNodeID(CmiMyPe())))
         << 32) |
        static_cast<uint32_t>(dm.global_index);
    descriptor.smCount = static_cast<uint32_t>(std::max(props.multiProcessorCount, 0));
    descriptor.computeMajor = static_cast<uint32_t>(std::max(props.major, 0));
    descriptor.computeMinor = static_cast<uint32_t>(std::max(props.minor, 0));
    // CUDA 13 dropped cudaDeviceProp::clockRate, so the device attribute is
    // the only way left to ask. It can legitimately fail (on a MIG instance,
    // for one); a zero here is what selects the Unknown-source path below.
    int clock_khz = 0;
    if (hapiDeviceGetAttribute(&clock_khz, hapiDevAttrClockRate, dev) !=
        hapiSuccess) {
      clock_khz = 0;
      hapiGetLastError();  // don't leave the failure for the next hapiCheck
    }
    descriptor.maxClockKHz = static_cast<uint32_t>(std::max(clock_khz, 0));
    descriptor.totalMemory = static_cast<uint64_t>(props.totalGlobalMem);
    descriptor.typeId = gpuStableDeviceType(
        props.name, descriptor.smCount, descriptor.computeMajor,
        descriptor.computeMinor, descriptor.maxClockKHz, descriptor.totalMemory);
    gpuDerivePeakRateScore(descriptor.smCount, descriptor.computeMajor,
                           descriptor.computeMinor, descriptor.maxClockKHz,
                           descriptor.peakRateScore, descriptor.peakRateSource);

    if (_lb_args.gpuScaling() &&
        gm.cupti_logged_device_types_.insert(descriptor.typeId).second) {
      CmiPrintf("HAPI GPU scaling: device type=%" PRIu64
                " name=%s cc=%u.%u sms=%u clock_khz=%u memory=%" PRIu64
                " rate=%.0f source=%s\n",
                descriptor.typeId, props.name, descriptor.computeMajor,
                descriptor.computeMinor, descriptor.smCount,
                descriptor.maxClockKHz, descriptor.totalMemory,
                descriptor.peakRateScore,
                gpuPeakRateSourceName(descriptor.peakRateSource));
      // An Unknown source means a property the prior needs was missing and a
      // nominal value stood in for it. The relative ordering against a fully
      // described device is then only as good as that guess, so say so once.
      if (descriptor.peakRateSource == GpuPeakRateSource::Unknown)
        CmiPrintf("HAPI GPU scaling: device type=%" PRIu64
                  " reported no usable clock rate; the cross-GPU prior for it "
                  "assumes %u kHz and will be corrected by observation\n",
                  descriptor.typeId, gpuNominalClockKHz());
    }
    dm.props_initialized = true;
  }
}

// Kept for callers that want tracing up before the balancer asks for it;
// hapiCuptiStartTracing attaches on its own, so this is not needed at startup.
void hapiCuptiInit() { hapiCuptiStartTracing(); }

// Attaching CUPTI to the process is NOT free even when no activity kind is
// enabled -- measured at ~1.3 ms per step on a 4-PE pic2d run. It used to be
// attached at the first start and detached at the last stop of each window;
// see hapiCuptiStopTracing for why it now stays attached and enabled.
// Per-PE view of the tracing switch. LBDatabase::TurnStatsOn/Off is a PE-local
// switch, but CUPTI tracing is process-wide: a PE that switched its own
// instrumentation off used to switch tracing off for every PE in the process,
// and whichever PE was still finishing its iteration lost every kernel it
// launched after that instant. On barnes, which closes its window from each
// element's AtSync, that was one whole PE per process reading zero GPU load at
// every step. So the process traces while ANY PE wants instrumentation:
// tracing starts with the first PE to switch on and stops with the last to
// switch off, counted per PE so repeated switches do not skew the count.
static thread_local bool cupti_pe_tracing = false;

// Entry-method hooks currently inside a CUPTI call, process-wide. A hook
// counts itself in, then re-reads the active flag, so a finalize (now only at
// exit) can wait for the count to reach zero before cuptiFinalize.
static std::atomic<int> cupti_hooks_inflight{0};
struct CuptiHookGuard {
  CuptiHookGuard() { cupti_hooks_inflight.fetch_add(1, std::memory_order_seq_cst); }
  ~CuptiHookGuard() { cupti_hooks_inflight.fetch_sub(1, std::memory_order_seq_cst); }
};

// Device-to-device copies are GPU work that no object owns, so the per-object
// attribution the balancer reads cannot see them and CUPTI is not even asked to
// trace them by default. leanmd issues one per local device receive -- ~6000 a
// step per process -- so the question "is the unexplained fraction of the step
// idle GPU, or GPU busy with work T_g structurally cannot see" turns on exactly
// this number. Off by default so a default run is unperturbed; the +gputrace
// timeline turns the copy records on.
// File-scope rather than a GPUManager field on purpose: gpumanager.h is
// included everywhere and changing it forces a full rebuild of every app.
static std::atomic<uint64_t> g_cupti_memcpy_n{0};
static std::atomic<uint64_t> g_cupti_memcpy_ns{0};
static std::atomic<uint64_t> g_cupti_memcpy_bytes{0};
static std::atomic<uint64_t> g_cupti_kernel_ns{0};
// ---- GPU timeline sink: +gputrace <dir> (the flag is parsed in ck-core/cknvtx.C) ----
// This process's CUPTI kernel, memcpy and memset records go to <dir>/gputrace.<host>.<pid>.proc as
// text records on the CUPTI clock (K dev stream start end nameId objToken / M,S dev stream start end
// bytes kind / N nameId name / T objToken omIdx objId); every PE writes its entry-method and idle
// ranges beside it (cknvtx.C). gputrace_merge.py joins them into one Perfetto trace of all GPUs.
// Unlike nsys, this leaves the runtime's CUPTI subscription -- and so the balancer's device loads --
// intact: CUPTI admits one subscriber per process.
#ifdef CMK_LBDB_ON
void hapiProcessCuptiBuffers();
static FILE* g_trace = nullptr;
static std::mutex g_trace_lock;
static std::unordered_map<std::string, int> g_trace_names;

uint64_t hapiTraceTimestamp() { return hapiTraceBackendTimestamp(); }

static void hapiTraceObject(uint64_t token, const LDObjKey& key) {
  std::lock_guard<std::mutex> lk(g_trace_lock);
  if (g_trace == nullptr) return;
  fprintf(g_trace, "T %llu %d %llu\n", (unsigned long long)token, (int)key.omID().id.idx,
          (unsigned long long)key.objID());
}

static void hapiTraceKernel(const HapiTraceKernelRec* k, uint64_t token) {
  std::lock_guard<std::mutex> lk(g_trace_lock);
  if (g_trace == nullptr) return;
  const char* nm = k->name != nullptr ? k->name : "?";
  int id;
  auto it = g_trace_names.find(nm);
  if (it == g_trace_names.end()) {
    id = (int)g_trace_names.size();
    g_trace_names.emplace(nm, id);
    int status = 0;
    char* dem = abi::__cxa_demangle(nm, nullptr, nullptr, &status);
    fprintf(g_trace, "N %d %s\n", id, (status == 0 && dem != nullptr) ? dem : nm);
    free(dem);
  } else {
    id = it->second;
  }
  fprintf(g_trace, "K %u %u %llu %llu %d %llu\n", (unsigned)k->deviceId, (unsigned)k->streamId,
          (unsigned long long)k->start, (unsigned long long)k->end, id, (unsigned long long)token);
}

static void hapiTraceCopy(char kind, uint32_t dev, uint32_t stream, uint64_t s, uint64_t e,
                          uint64_t bytes, int copyKind) {
  std::lock_guard<std::mutex> lk(g_trace_lock);
  if (g_trace == nullptr) return;
  fprintf(g_trace, "%c %u %u %llu %llu %llu %d\n", kind, (unsigned)dev, (unsigned)stream,
          (unsigned long long)s, (unsigned long long)e, (unsigned long long)bytes, copyKind);
}

void hapiTraceStart(const char* dir) {
  if (g_trace != nullptr) return;
  mkdir(dir, 0775);
  char host[256] = "?";
  gethostname(host, sizeof host - 1); host[sizeof host - 1] = 0;
  if (char* dot = strchr(host, '.')) *dot = 0;
  char path[1024];
  snprintf(path, sizeof path, "%s/gputrace.%s.%d.proc", dir, host, (int)getpid());
  FILE* f = fopen(path, "w");
  if (f == nullptr) {
    CmiPrintf("HAPI[pe=%d]: +gputrace: cannot open %s: %s\n", CmiMyPe(), path, strerror(errno));
    return;
  }
  setvbuf(f, nullptr, _IOFBF, 1 << 20);
  // The clock pair that lines processes up across nodes: CUPTI's clock against CLOCK_REALTIME.
  const uint64_t c0 = hapiTraceTimestamp();
  struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
  const uint64_t c1 = hapiTraceTimestamp();
  const uint64_t real_now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
  fprintf(f, "# gputrace v1 host=%s pid=%d pe0=%d realtime_ns=%llu cupti_ns=%llu\n", host,
          (int)getpid(), CmiMyPe(), (unsigned long long)real_now, (unsigned long long)((c0 + c1) / 2));
  { std::lock_guard<std::mutex> lk(g_trace_lock); g_trace = f; }
  hapiCuptiStartTracing();   // records from the start of the run, not from the first LB window
  CmiPrintf("HAPI[pe=%d]: +gputrace: writing %s\n", CmiMyPe(), path);
}

void hapiTraceStop() {
  if (g_trace == nullptr) return;
  if (hapiCuptiTracingActive()) hapiCuptiFlushGated(HAPI_CUPTI_FLUSH_FORCED != 0);
  {   // the records queued since the last LB step; the drain thread may be in the parse
    std::lock_guard<std::mutex> lk(CsvAccess(gpu_manager).cupti_prepare_lock_);
    hapiProcessCuptiBuffers();
  }
  std::lock_guard<std::mutex> lk(g_trace_lock);
  fclose(g_trace);
  g_trace = nullptr;
}
#endif

#ifdef CMK_LBDB_ON
static bool g_trace_copies = true;   // +gputracenocopy clears it: kernels only
void hapiTraceCopies(bool on) { g_trace_copies = on; }
#else
void hapiTraceCopies(bool) {}
#endif

bool hapiCuptiTraceMemcpy() {
#ifdef CMK_LBDB_ON
  if (g_trace != nullptr) return g_trace_copies;   // the +gputrace timeline wants the copies too, unless +gputracenocopy
#endif
  return false;
}

#if CMK_CUDA
// ---- the CUDA trace backend: CUPTI's activity API ----
bool hapiTraceBackendAttach() {
  {   // One activity buffer per context, shared by its threads, instead of one per thread (the default): the deferred
      // close needs the buffer open at the mark back without a forced flush, and a thread that keeps launching
      // keeps its own buffer open (an incomplete record in it) until it fills, several steps later. The context's
      // buffer fills at the rate of all its threads. Must precede the callbacks (cupti_activity.h).
    static bool set = false;
    if (!set) {
      set = true;
      uint8_t per_thread = 0;
      size_t sz = sizeof(per_thread);
      CUPTI_SAFE_CALL(cuptiActivitySetAttribute(CUPTI_ACTIVITY_ATTR_PER_THREAD_ACTIVITY_BUFFER, &sz, &per_thread));
    }
  }
  CUPTI_SAFE_CALL(
      cuptiActivityRegisterCallbacks(cuptiBufferRequested, cuptiBufferCompleted));
  // API records name their thread by the OS id (gettid), the id the mapping recorded per PE.
  CUPTI_SAFE_CALL(cuptiSetThreadIdType(CUPTI_ACTIVITY_THREAD_ID_TYPE_SYSTEM));
  return true;
}
void hapiTraceBackendEarlyInit() {}
void hapiTraceBackendEnable(bool trace_copies) {
  // RUNTIME records carry the launch's thread id and correlation id: the window
  // close joins them to the entry-span log (CuptiEntryLog) and the kernel record
  // with the same correlation id gets the object. Without them every kernel
  // arrives unattributed and the balancer sees zero GPU load. (The
  // EXTERNAL_CORRELATION kind and its per-entry push/pop are gone, see the log.)
  {   // our buffers arrive zeroed (fresh: memset in cuptiBufferRequested; recycled: hapiCuptiFreeRetired), so CUPTI
      // must not zero them again inside the flush call (cupti_activity.h: "a few stalls may appear in critical path
      // because CUPTI will zero out the buffer in the main thread"; measured 0.3 ms per 1 MB buffer, job 22953167).
    static bool zeroed_set = false;
    if (!zeroed_set) {
      zeroed_set = true;
      uint8_t zeroed = 1;
      size_t sz = sizeof(zeroed);
      CUPTI_SAFE_CALL(cuptiActivitySetAttribute(CUPTI_ACTIVITY_ATTR_ZEROED_OUT_ACTIVITY_BUFFER, &sz, &zeroed));
    }
  }
  CUPTI_SAFE_CALL(cuptiActivityEnable(CUPTI_ACTIVITY_KIND_CONCURRENT_KERNEL));
  CUPTI_SAFE_CALL(cuptiActivityEnable(CUPTI_ACTIVITY_KIND_RUNTIME));
  // CUPTI's worker thread reads the device-side profiling buffers (the kernel timestamps) back to the host and
  // delivers full activity buffers on its own heuristics; cuptiActivityFlushPeriod(ms) fixes its period.
  // CHARM_LB_CUPTI_PERIODIC=<ms> sets one (0 / unset = the heuristics). A 10 ms period was tried as the cure for the
  // forced flush call at the window close and changed nothing (15.9 vs 14.4 ms, job 22953167): that call is the GPU
  // queue drain plus the buffer zeroing (see hapiCuptiFreeRetired), not deferred readback work.
  {
    static bool period_set = false;
    if (!period_set) {
      period_set = true;
      const char* per = getenv("CHARM_LB_CUPTI_PERIODIC");
      const int ms = per ? atoi(per) : 0;
      if (ms > 0) CUPTI_SAFE_CALL(cuptiActivityFlushPeriod((uint32_t)ms));
      if (CmiMyPe() == 0 && hapiCuptiTimeIt())
        CmiPrintf("HAPI> CUPTI worker flush period %d ms%s\n", ms, ms > 0 ? "" : " (CUPTI heuristics)");
    }
  }
  // Opt-in: what share of device time is runtime copies rather than object
  // kernels. Off by default so the record volume of a normal run is unchanged.
  if (trace_copies) {
    CUPTI_SAFE_CALL(cuptiActivityEnable(CUPTI_ACTIVITY_KIND_MEMCPY));
    CUPTI_SAFE_CALL(cuptiActivityEnable(CUPTI_ACTIVITY_KIND_MEMSET));
  }
  // Except for the calls that carry no driver time and no correlation:
  // completion polls and error peeks. hapiPrepareCuptiLoads parses every
  // runtime record at each LB step, and these were 33 M of the 34 M records
  // in one leanmd window (see HAPI_FLAG_SLOTS) -- the launch dimension only
  // counts the other calls anyway (their names contain "Query").
  static const CUpti_CallbackId no_record[] = {
      CUPTI_RUNTIME_TRACE_CBID_cudaEventQuery_v3020,
      CUPTI_RUNTIME_TRACE_CBID_cudaStreamQuery_v3020,
      CUPTI_RUNTIME_TRACE_CBID_cudaGetLastError_v3020,
      CUPTI_RUNTIME_TRACE_CBID_cudaPeekAtLastError_v3020,
      CUPTI_RUNTIME_TRACE_CBID_cudaGetDevice_v3020,
  };
  for (CUpti_CallbackId cbid : no_record)
    CUPTI_SAFE_CALL(cuptiActivityEnableRuntimeApi(cbid, 0));
}
void hapiTraceBackendFlush(bool forced) {
  CUPTI_SAFE_CALL(cuptiActivityFlushAll(forced ? CUPTI_ACTIVITY_FLAG_FLUSH_FORCED : 0));
}
uint64_t hapiTraceBackendTimestamp() { uint64_t t = 0; cuptiGetTimestamp(&t); return t; }
const char* hapiTraceBackendCallbackName(uint32_t cbid) {
  const char* name = nullptr;
  if (cuptiGetCallbackName(CUPTI_CB_DOMAIN_RUNTIME_API, cbid, &name) != CUPTI_SUCCESS) return nullptr;
  return name;
}
uint32_t hapiTraceBackendCallbackIdCount() { return CUPTI_RUNTIME_TRACE_CBID_SIZE; }
void hapiTraceBackendFinalize() {
  hapiCuptiFlusherWait(true);
  CUPTI_SAFE_CALL(cuptiFinalize());
}
#endif

void hapiCuptiStartTracing() {
  // CHARM_LB_NO_CUPTI: never attach -- the host-side instrumentation alone,
  // for measuring what the GPU tracing itself costs. The balancer then sees
  // zero device load.
  static const bool no_cupti = (getenv("CHARM_LB_NO_CUPTI") != nullptr);
  if (no_cupti) return;
  GPUManager& gm = CsvAccess(gpu_manager);
  // Every PE thread reaches this through its own LBDatabase::TurnStatsOn, so
  // the check and the enable must be one atomic step -- otherwise several
  // threads each enable the same activity kinds.
  std::lock_guard<std::mutex> lk(gm.cupti_tracing_lock_);
  if (!cupti_pe_tracing) {
    cupti_pe_tracing = true;
    gm.cupti_tracing_users_++;
  }
  if (_lb_args.debug() > 1)
    CmiPrintf("HAPI[pe=%d]: start tracing: users %d, active %d, attached %d\n", CmiMyPe(),
              gm.cupti_tracing_users_, (int)gm.cupti_tracing_active_.load(), (int)gm.cupti_initialized_);
  if (gm.cupti_tracing_active_.load(std::memory_order_relaxed)) return;

  const double tAttach = hapiCuptiTimeIt() ? CmiWallTimer() : 0.0;
  if (!gm.cupti_initialized_) {
    hapiDeviceSynchronize();
    if (!hapiTraceBackendAttach()) return;
    gm.cupti_initialized_ = true;
  }

  hapiTraceBackendEnable(hapiCuptiTraceMemcpy());

  hapiCuptiCalibrateSpanClock(g_cupti_span_clock.load(std::memory_order_relaxed) == -1 && g_cupti_span_offset.load() == 0);
  gm.cupti_tracing_active_.store(true, std::memory_order_relaxed);
  if (tAttach != 0.0)
    CmiPrintf("[LBCUPTI pe=%d] attach %.3fs (device sync, register, enable; %d device(s) in this process)\n",
              CmiMyPe(), CmiWallTimer() - tAttach, gm.device_count);
}

void hapiCuptiStopTracing() {
  GPUManager& gm = CsvAccess(gpu_manager);
  std::lock_guard<std::mutex> lk(gm.cupti_tracing_lock_);
  if (cupti_pe_tracing) {
    cupti_pe_tracing = false;
    if (gm.cupti_tracing_users_ > 0) gm.cupti_tracing_users_--;
  }
  if (_lb_args.debug() > 1)
    CmiPrintf("HAPI[pe=%d]: stop tracing: users left %d, active %d, attached %d\n", CmiMyPe(),
              gm.cupti_tracing_users_, (int)gm.cupti_tracing_active_.load(), (int)gm.cupti_initialized_);
  // And nothing else: tracing stays enabled and attached from the first start
  // to the end of the run. Disabling the activity kinds, the forced flush and
  // the detach (cuptiFinalize, behind an explicit cudaDeviceSynchronize) that
  // used to follow when the last PE switched off each drain the device, and
  // this is reached from every element's AtSync -- the first element on a PE
  // to finish its step closes the PE's window while the rest of the process
  // is mid-step. Measured 2026-09-15 on leanmd 8x8x8: that drain stalled
  // other elements' CUDA calls for 1.2-2.9 s, charged as host time to
  // whatever entry method made them (receiveMigrants, forceSendDone), and
  // DiffusionLB balanced it: ~1000 spurious within-GPU moves and +3 s per LB
  // step, sync and async alike. What staying attached costs is tracing
  // through the LB step itself, where nothing of interest runs; the
  // interposition tax of an attached-but-idle CUPTI (0.8-1.3 ms per step on
  // small sph2d/pic2d) is not paid, because tracing is never idle now.
  // hapiPrepareCuptiLoads collects the records with a flush that does not
  // synchronise the device.
}

bool hapiCuptiTracingActive() {
  return CsvAccess(gpu_manager).cupti_tracing_active_.load(
      std::memory_order_relaxed);
}

void hapiCuptiFinalize() {
  GPUManager& gm = CsvAccess(gpu_manager);
  if(gm.cupti_initialized_== false) return;
  if (_lb_args.debug() > 1) CmiPrintf("HAPI[pe=%d]: detaching CUPTI\n", CmiMyPe());
  hapiDeviceSynchronize(); // Ensure all activity records are flushed
  gm.cupti_initialized_ = false;
  gm.cupti_tracing_active_.store(false, std::memory_order_seq_cst);
  ++gm.cupti_generation_;

  hapiTraceBackendFinalize();
}
#endif

#ifndef HAPI_CUDA_CALLBACK
#if CSD_NO_SCHEDLOOP
#  error please disable CSD_NO_SCHEDLOOP to use HAPI
#endif
#endif

// Hardware work queues per device. The driver default is 8, and every stream
// beyond the eighth aliases onto a queue already in use: two dependent kernels
// from one stream park that queue's head and block an unrelated stream behind
// them, costing (Q/C + 1) x T instead of 2 x T for Q concurrently active
// streams on C queues (tests/util/streamorder, A40, 2026-09-20). 32 is the
// driver maximum and costs nothing. The driver reads CUDA_DEVICE_MAX_CONNECTIONS
// when it initialises, so this runs from charm_main before ConverseInit, ahead
// of the first CUDA call anywhere in the process (LCI's cuInit included). A
// value already in the environment stays in force; the Config banner says which.
static bool hapi_connections_from_env = false;
void hapiPresetEnvironment() {
  if (getenv(HAPI_HW_QUEUES_ENV) != nullptr) {
    hapi_connections_from_env = true;
  } else {
#if CMK_CUDA
    // 32 hardware work queues per device instead of the driver's 8: over-decomposed
    // runs put thousands of streams on them (measured 2026-10-20, +5% at leanmd 8x4x4).
    setenv(HAPI_HW_QUEUES_ENV, "32", 0);
#endif
    // ROCm's default (GPU_MAX_HW_QUEUES) is left to the driver until measured there.
  }
#if CMK_LBDB_ON
  // The trace backend may need to register with the vendor runtime before its
  // first call (rocprofiler-sdk does); nothing happens on CUDA.
  hapiTraceBackendEarlyInit();
#endif
}

// Called by all PEs in Charm++ layer init
void hapiInit(char** argv) {
  // Before hapiInitCsv: device-manager creation already indexes by it.
  CpvInitialize(int, my_node_rank_local);
  hapiRefreshTopologyCache();
  if (!CmiInCommThread()) {
    if (CmiMyRank() == 0) {
      hapiInitCsv(argv); // Initialize per-process variables (GPUManager)
    }
    hapiInitCpv(); // Initialize per-PE variables

    CmiNodeBarrier(); // Ensure hapiInitCsv is done for all PEs within a logical node

    hapiMapping(argv); // Perform PE-device mapping

#if CMK_SHRINK_EXPAND
    hapiStartMemoryDaemon(argv);
#else
    int& cpv_my_device = CpvAccess(my_device);
    hapiCheck(hapiSetDevice(cpv_my_device));
#endif

#ifndef HAPI_CUDA_CALLBACK
    // Register polling function to be invoked at every scheduler loop
    CcdCallOnConditionKeep(CcdSCHEDLOOP, (CcdCondFn)hapiPollEvents, NULL);
    if (hapi_use_flag_poll)
      CcdCallOnConditionKeep(CcdPROCESSOR_STILL_IDLE, (CcdCondFn)hapiIdleYield, NULL);
#endif
  }

  CmiNodeAllBarrier();

  if (CmiInCommThread()) {
    // FIXME: Comm. thread sets its device to be the same as worker thread 0
    hapiSetDevice(CsvAccess(gpu_manager).comm_thread_device);
  }

  shmInit();

  hapiRegisterCallbacks(); // Register callback functions
}


void hapiStartMemoryDaemon(char** argv)
{
#if CMK_SHRINK_EXPAND
  // start client FIFO
  long pid = getpid();
  char client_fifo_path[BUFFER_SIZE];
  sprintf(client_fifo_path, CLIENT_FIFO_TEMPLATE, pid);
  std::remove(client_fifo_path);
  mkfifo(client_fifo_path, 0666);

  int& cpv_my_device = CpvAccess(my_device);
  CkPrintf("Device = %i\n", cpv_my_device);
  hapiCheck(hapiSetDevice(cpv_my_device));

  if (CmiPhysicalRank(CmiMyPe()) != firstRankForDevice)
  {
    CmiBarrier();
    return;
  }

  char server_fifo_path[BUFFER_SIZE];
  sprintf(server_fifo_path, SERVER_FIFO_TEMPLATE, cpv_my_device);

  // Create a ready signal FIFO for synchronization
  if (!CmiGetArgFlagDesc(argv,"+shrinkexpand","Restarting of already running prcoess")) {
    char ready_fifo_path[BUFFER_SIZE];
    sprintf(ready_fifo_path, "/tmp/daemon_ready_%d", cpv_my_device);

    CmiPrintf("Parent: Waiting for daemon to be ready...\n");
    
    int ready_fd = open(ready_fifo_path, O_RDONLY);
    if (ready_fd == -1) {
      perror("Parent: open ready FIFO");
      CmiAbort("Failed to open ready FIFO");
    }
  
    char ready_signal;
    read(ready_fd, &ready_signal, 1);
    close(ready_fd);
    unlink(ready_fifo_path);  // Clean up
    
    CmiPrintf("Parent: Daemon is ready!\n");
  }
  
  CmiBarrier();
  return;
#endif
}

int hapiCheckpoint(void* devPtr, int size) {
  pid_t pid = getpid();

  char client_fifo_path[BUFFER_SIZE];
  sprintf(client_fifo_path, CLIENT_FIFO_TEMPLATE, pid);

  hapiIpcMemHandle_t ipc_handle;
  hapiCheck(hapiIpcGetMemHandle(&ipc_handle, devPtr));

  char msg_buf[BUFFER_SIZE];
  int offset = sprintf(msg_buf, "CKPT:%ld:%d:%d:", pid, CkMyPe(), size);
  memcpy(msg_buf + offset, &ipc_handle, sizeof(hapiIpcMemHandle_t));
  int total_size = offset + sizeof(hapiIpcMemHandle_t);

  hapiSendMemoryRequest(msg_buf, total_size);

  int client_fd = open(client_fifo_path, O_RDONLY);
  int alloc_id;
  read(client_fd, &alloc_id, sizeof(int));
  close(client_fd);

  return alloc_id;
}

void hapiRestore(void* devPtr, int size, int alloc_id) {
  pid_t pid = getpid();

  char client_fifo_path[BUFFER_SIZE];
  sprintf(client_fifo_path, CLIENT_FIFO_TEMPLATE, pid);

  char msg_buf[BUFFER_SIZE];
  sprintf(msg_buf, "GET:%ld:%d", pid, alloc_id);

  hapiSendMemoryRequest(msg_buf, strlen(msg_buf) + 1);

  int client_fd = open(client_fifo_path, O_RDONLY);
  hapiIpcMemHandle_t ipc_handle;
  read(client_fd, &ipc_handle, sizeof(hapiIpcMemHandle_t));
  close(client_fd);

  void* srcPtr;
  hapiCheck(hapiIpcOpenMemHandle(&srcPtr, ipc_handle, hapiIpcMemLazyEnablePeerAccess));
  hapiCheck(hapiMemcpy(devPtr, srcPtr, size, hapiMemcpyDeviceToDevice));
  hapiCheck(hapiIpcCloseMemHandle(srcPtr));

  char free_msg[BUFFER_SIZE];
  sprintf(free_msg, "FREE:%ld:%d", pid, alloc_id);
  hapiSendMemoryRequest(free_msg, strlen(free_msg) + 1);

  client_fd = open(client_fifo_path, O_RDONLY);
  char status;
  read(client_fd, &status, sizeof(char));
  close(client_fd);
}

void hapiExit() {
  // Ensure all PEs have finished GPU work
  CmiPrintf("Exit called on PE %d\n", CmiMyPe());
  CmiNodeBarrier();
  if (CmiMyRank() == 0) hapiTraceStop();   // +gputrace: flush the last CUPTI records and close

  if (CpvAccess(hapi_flag_slots) != NULL) {
    hapiFreeHost((void*)CpvAccess(hapi_flag_slots));
    CpvAccess(hapi_flag_slots) = NULL;
    CpvAccess(hapi_flag_slots_dev) = NULL;
  }

#if CMK_SHRINK_EXPAND
  char client_fifo_path[BUFFER_SIZE];
  sprintf(client_fifo_path, CLIENT_FIFO_TEMPLATE, getpid());

  if (!get_shrinkexpand_exit() && CmiPhysicalRank(CmiMyPe()) == firstRankForDevice)
  {
    char msg_buf[BUFFER_SIZE];
    sprintf(msg_buf, "KILL:%ld:0", getpid());
    hapiSendMemoryRequest(msg_buf, strlen(msg_buf) + 1);

    int client_fd = open(client_fifo_path, O_RDONLY);
    char status;
    read(client_fd, &status, sizeof(char));
    close(client_fd);
  }

  if (!get_shrinkexpand_exit())
  {
    // Attempt to delete the file
    if (std::remove(client_fifo_path) == 0) {
        CmiPrintf("File '%s' deleted successfully.\n", client_fifo_path);
    } else {
        CmiPrintf("Error deleting file '%s': %s\n", client_fifo_path, strerror(errno));
    }
  }
#endif

  if (CmiMyRank() == 0) {
    if (getenv("CHARM_ZC_STATS") != NULL) hapiIpcReportStats();

    // Safe to close peer mappings here and nowhere cheaper: the node barrier
    // above has already quiesced GPU work, and closing a mapping some copy is
    // still reading is an illegal access. See the note on hapiIpcFlushImportCache.
    hapiIpcFlushImportCache();

    shmCleanup();

    hapiExitCsv();
  }
}

// Initialize per-process variables
static void hapiInitCsv(char** argv) {
  // Create and initialize GPU Manager object
  CsvInitialize(GPUManager, gpu_manager);
  CsvAccess(gpu_manager).init();
  // CUPTI is attached lazily by hapiCuptiStartTracing, which the balancer
  // reaches through LBDatabase::TurnStatsOn. Attaching here instead would pay
  // the attach cost for the whole run even when the application only wants
  // instrumentation around its load-balancing steps.
}


#ifdef CMK_LBDB_ON
// Find the DeviceManager that matches this kernel's device id.
static DeviceManager* findDeviceManager(GPUManager& gm, uint32_t device_id) {
  for (DeviceManager& dm : gm.device_managers) {
    if ((uint32_t)dm.global_index == device_id) return &dm;
  }
  return nullptr;
}

// Compute the number of SMs this kernel occupies while running.
// Uses the CUDA occupancy model: theoretical max_active_blocks_per_sm is
// limited by (a) max blocks per SM, (b) warp count, (c) register pressure,
// (d) shared memory. Then:
//   sms_used = min(num_sms, ceil(total_blocks / max_active_blocks_per_sm))
static int computeKernelSMs(const DeviceManager& dm,
                            const HapiTraceKernelRec* k) {
  if (!dm.props_initialized || dm.multi_processor_count <= 0) return 1;

  uint64_t threads_per_block =
      (uint64_t)k->blockX * (uint64_t)k->blockY * (uint64_t)k->blockZ;
  uint64_t total_blocks =
      (uint64_t)k->gridX * (uint64_t)k->gridY * (uint64_t)k->gridZ;
  if (threads_per_block == 0 || total_blocks == 0) return 1;

  // Warp-count limit: maxThreadsPerSM / threadsPerBlock (rounded down).
  int limit_warps =
      dm.max_threads_per_sm > 0
          ? (int)(dm.max_threads_per_sm / threads_per_block)
          : INT_MAX;
  if (limit_warps <= 0) limit_warps = 1;

  // Block-count limit (CUDA 11+; 0 means not available -> use a large value).
  int limit_blocks = dm.max_blocks_per_sm > 0 ? dm.max_blocks_per_sm : INT_MAX;

  // Register-pressure limit.
  uint64_t regs_per_block = (uint64_t)k->registersPerThread * threads_per_block;
  int limit_regs = INT_MAX;
  if (regs_per_block > 0 && dm.max_registers_per_sm > 0) {
    uint64_t r = (uint64_t)dm.max_registers_per_sm / regs_per_block;
    limit_regs = r > INT_MAX ? INT_MAX : (int)r;
    if (limit_regs <= 0) limit_regs = 1;
  }

  // Shared-memory limit.
  uint64_t smem_per_block =
      (uint64_t)k->staticSharedMemory + (uint64_t)k->dynamicSharedMemory;
  int limit_smem = INT_MAX;
  if (smem_per_block > 0 && dm.max_shared_mem_per_sm > 0) {
    uint64_t s = (uint64_t)dm.max_shared_mem_per_sm / smem_per_block;
    limit_smem = s > INT_MAX ? INT_MAX : (int)s;
    if (limit_smem <= 0) limit_smem = 1;
  }

  int max_active_blocks_per_sm =
      std::min(std::min(limit_blocks, limit_warps),
               std::min(limit_regs, limit_smem));
  if (max_active_blocks_per_sm < 1) max_active_blocks_per_sm = 1;

  uint64_t sms_needed =
      (total_blocks + max_active_blocks_per_sm - 1) / max_active_blocks_per_sm;
  int sms_used = (int)std::min<uint64_t>(sms_needed,
                                         (uint64_t)dm.multi_processor_count);
  if (sms_used < 1) sms_used = 1;
  return sms_used;
}

// A kernel record can be parsed before the correlation records that name it:
// correlation and kernel records are queued at different points in the
// launch's life and land in buffers that complete independently. Only those
// kernels are parked for a second pass; one whose correlations are already
// known is filed immediately, so the common case never holds two copies of
// every record. The launch signature rides along with a parked kernel because
// it is needed to rebucket if a work tag turns up, but it is deliberately not
// stored in LBKernelRecord: there is one of those per launch.
struct CuptiPendingKernel {
  uint32_t           correlation_id;
  uint32_t           enqueue_thread;   // the backend's thread id for the dispatch (HIP), 0 if unknown
  GpuLaunchSignature launch;
  LBKernelRecord     rec;
  bool               carried;           // parked at the previous drain already
};
struct CuptiPendingApi { uint32_t correlation_id; double seconds; };
struct CuptiResolvedToken { LDObjKey key{}; bool valid = false; };
struct CuptiCbidUse { uint64_t count = 0; double seconds = 0.0; bool launch = false;
  bool poll = false; bool known = false; };

// The parse state of ONE shard. CUPTI delivers its buffers with no context (the ctx argument of
// the completion callback is null), so the buffers of a drain are dealt round-robin over as many
// shards as the process has devices, and each shard's buffers are parsed on their own thread
// (pass 1). A kernel or API record and the correlation record that names it can then sit in
// different shards, so whatever pass 1 could not resolve from its own shard is resolved in a
// serial pass 2 that looks across every shard. One process driving four devices parsed four
// devices' records on one thread at every window close (16 ms against 4, job 22821909).
//
// Carried over from the previous drain: a kernel record and its correlation can also be
// delivered one drain apart, in either order. A kernel without its correlation is parked once
// more, and the previous drain's correlation maps live for one more drain.
struct CuptiParseShard {
  std::unordered_map<uint32_t, uint64_t> object_corr, work_tag_corr;             // this drain
  std::unordered_map<uint32_t, uint32_t> thread_corr;   // correlation -> launching thread (the API record's), this drain
  std::unordered_map<uint32_t, uint64_t> cover_end;     // thread -> last API record end seen in this shard's buffers
  std::unordered_map<LDObjKey, uint32_t, LDObjKeyHash> obj_thread;   // object -> thread that launched its kernels
  std::unordered_map<uint32_t, uint64_t> prev_object_corr, prev_work_tag_corr;   // the one before
  std::vector<CuptiPendingKernel> carried;
  uint32_t pending_hint = 0;
  // Knowledge shared by every shard: seeded from the GPUManager at the start of a drain, unioned
  // back at the merge.
  std::unordered_set<uint64_t> tagged_classes, untagged_classes;
  // One drain's results, moved into the GPUManager at the merge.
  std::unordered_map<LDObjKey, std::vector<LBKernelRecord>, LDObjKeyHash> obj_kernel_records;
  std::vector<LBKernelRecord> unattributed_kernels;
  std::unordered_map<uint64_t, std::string> kernel_names;
  std::unordered_map<LDObjKey, double, LDObjKeyHash> obj_api_raw;
  double api_total = 0.0;
  std::unordered_map<int, double> api_total_dev;
  std::vector<GPUManager::ApiInterval> api_intervals;
  std::vector<std::pair<uint8_t*, size_t>> retired;   // buffers parsed, to quarantine (the caller holds the lock)
  // Between pass 1 and pass 2 of one drain.
  std::vector<CuptiPendingKernel> pending;
  std::vector<CuptiPendingApi> pendingApi;
  std::unordered_map<uint64_t, double> apiByToken;
  std::unordered_map<uint64_t, CuptiResolvedToken> resolved_tokens;
  std::vector<CuptiCbidUse> byCbid;
  double api_unattributed_s = 0.0;
  // Per launching thread, this drain: launch API records whose span named an object, a joined object
  // (HAPI_CUPTI_NO_OBJECT) or nothing. For the per-thread attribution print (+LBDebug 2).
  struct ThreadAttr { uint32_t owned = 0, joined = 0, miss = 0; };
  std::unordered_map<uint32_t, ThreadAttr> perThread;
  uint32_t api_count = 0, kernel_count = 0, object_corr_count = 0, work_tag_count = 0,
           invalid_duration_count = 0, hash_collision_count = 0, attributed = 0, unattributed = 0,
           unresolved_token = 0, deferred = 0, lost_work_tags = 0;
  uint32_t carried_out = 0, carried_in_resolved = 0, carried_in_lost = 0;   // kernels parked for the next drain
  uint32_t api_corr_max = 0, carried_corr_min = UINT32_MAX, carried_corr_max = 0, carried_above_api = 0;
  uint32_t carried_pe_thread = 0, carried_other_thread = 0;
  uint32_t api_zero_dur = 0, api_launch_records = 0, copy_records = 0;
  std::unordered_map<uint32_t, uint32_t> corr_cbid;          // correlation -> API function (debug > 1)
  struct ApiBrief { uint32_t corr, cbid; uint64_t start, end; };
  std::unordered_map<uint32_t, std::vector<ApiBrief>> api_by_thread;   // debug > 1: every record, in arrival order
  int carried_examples = 0;
  std::unordered_map<uint32_t, uint32_t> kernels_by_nonlaunch_cbid;   // kernels whose record is not a launch
  std::unordered_map<uint32_t, uint32_t> carried_threads;
  std::unordered_map<std::string, uint32_t> carried_names;
};
static std::vector<CuptiParseShard> g_cupti_shards;   // under cupti_prepare_lock_
// The drains' copies of the entry-span logs (CuptiEntryLog), by OS thread id, kept between drains
// and refreshed incrementally (hapiCuptiLaneRefresh); std::map keeps the entries where they are
// while shards hold pointers.
struct CuptiLaneSnap { CuptiLaneCopy obj, tag; bool taken = false; };
struct CuptiLaneCursor { const CuptiLaneSnap* sn = nullptr; size_t obj = 0, tag = 0; };   // one shard's position in a thread's copy
static std::map<uint32_t, CuptiLaneSnap> g_cupti_snaps;
static std::mutex g_cupti_snaps_lock;

// Persistent parse workers: a thread that has called into CUPTI keeps per-thread tracing state
// (a fresh thread per LB step cost leanmd 115 -> 132 ms a step over four steps), so the same
// threads serve every close. Worker i parses shard i+1; shard 0 is parsed by the caller.
struct CuptiParseWorker {
  std::mutex m;
  std::condition_variable cv;
  std::function<void()> job;
  bool has_job = false;
  std::thread thread;
};
static std::vector<CuptiParseWorker*> g_cupti_workers;   // under cupti_prepare_lock_; never destroyed
static std::mutex g_cupti_workers_done_m;
static std::condition_variable g_cupti_workers_done_cv;
static int g_cupti_workers_pending = 0;
static void hapiCuptiWorkerRun(size_t i, std::function<void()> job) {
  while (g_cupti_workers.size() <= i) {
    CuptiParseWorker* w = new CuptiParseWorker;
    w->thread = std::thread([w]() {
      for (;;) {
        std::function<void()> j;
        {
          std::unique_lock<std::mutex> lk(w->m);
          w->cv.wait(lk, [w] { return w->has_job; });
          j = std::move(w->job);
          w->has_job = false;
        }
        j();
        {
          std::lock_guard<std::mutex> lk(g_cupti_workers_done_m);
          g_cupti_workers_pending--;
        }
        g_cupti_workers_done_cv.notify_all();
      }
    });
    w->thread.detach();
    g_cupti_workers.push_back(w);
  }
  CuptiParseWorker* w = g_cupti_workers[i];
  {
    std::lock_guard<std::mutex> lk(g_cupti_workers_done_m);
    g_cupti_workers_pending++;
  }
  {
    std::lock_guard<std::mutex> lk(w->m);
    w->job = std::move(job);
    w->has_job = true;
  }
  w->cv.notify_one();
}
static void hapiCuptiWorkersWait() {
  std::unique_lock<std::mutex> lk(g_cupti_workers_done_m);
  g_cupti_workers_done_cv.wait(lk, [] { return g_cupti_workers_pending == 0; });
}

// The object correlation for `id`, in this shard (this drain, then the previous one), then, when
// `all` is given (pass 2), in every other shard.
static const uint64_t* cuptiFindObjectCorr(CuptiParseShard& sh, uint32_t id, std::vector<CuptiParseShard>* all) {
  auto it = sh.object_corr.find(id);
  if (it != sh.object_corr.end()) return &it->second;
  auto pv = sh.prev_object_corr.find(id);
  if (pv != sh.prev_object_corr.end()) return &pv->second;
  if (all != nullptr)
    for (CuptiParseShard& o : *all) {
      if (&o == &sh) continue;
      auto i2 = o.object_corr.find(id);
      if (i2 != o.object_corr.end()) return &i2->second;
      auto p2 = o.prev_object_corr.find(id);
      if (p2 != o.prev_object_corr.end()) return &p2->second;
    }
  return nullptr;
}

static uint32_t cuptiThreadOfCorr(CuptiParseShard& sh, uint32_t corr, std::vector<CuptiParseShard>* all) {
  auto it = sh.thread_corr.find(corr);
  if (it != sh.thread_corr.end()) return it->second;
  if (all != nullptr)
    for (CuptiParseShard& o : *all) {
      auto j = o.thread_corr.find(corr);
      if (j != o.thread_corr.end()) return j->second;
    }
  return 0;
}

static void cuptiFileKernel(GPUManager& gm, CuptiParseShard& sh, const LBKernelRecord& rec, uint64_t object_token,
                            uint32_t launch_thread) {
  if (object_token == HAPI_CUPTI_NO_OBJECT) {
    sh.unattributed_kernels.push_back(rec);
    sh.unattributed++;
    return;
  }
  // Resolve each distinct token once rather than once per kernel record. A
  // round holds far more kernels than objects, and this lock is the same one
  // every entry method needs, so taking it per record would both dominate the
  // drain and stall PEs that are still running.
  auto memo = sh.resolved_tokens.find(object_token);
  if (memo == sh.resolved_tokens.end()) {
    CuptiResolvedToken entry;
    {
      std::lock_guard<std::mutex> token_lock(gm.cupti_object_token_lock_);
      entry.valid = gm.cupti_object_tokens_.resolve(object_token, entry.key);
    }
    memo = sh.resolved_tokens.emplace(object_token, entry).first;
  }
  if (!memo->second.valid) {
    sh.unattributed_kernels.push_back(rec);
    sh.unattributed++;
    sh.unresolved_token++;
    return;
  }
  sh.obj_kernel_records[memo->second.key].push_back(rec);
  if (launch_thread != 0) sh.obj_thread[memo->second.key] = launch_thread;
  sh.attributed++;
}

// Applies the work tag for this launch, if one has been parsed. Returns false
// when the tag may still be in an unparsed buffer, in which case the bucket
// is not settled and the kernel must be parked until the whole drain is done.
// With `all` (pass 2) the tag is also looked for in the other shards.
static bool cuptiApplyWorkTag(CuptiParseShard& sh, std::vector<CuptiParseShard>* all, uint32_t correlation_id,
                              const GpuLaunchSignature& launch, LBKernelRecord& rec, bool last_chance) {
  // Already resolved during the parse loop. Without this the second pass
  // would look the tag up again, fail to find the entry it consumed itself,
  // and condemn a correctly tagged kernel as unmodelable.
  if (rec.has_explicit_work_tag) return true;

  auto take = [&](std::unordered_map<uint32_t, uint64_t>& m) {
    auto it = m.find(correlation_id);
    if (it == m.end()) return false;
    rec.has_explicit_work_tag = true;
    rec.kernel_key.workBucket = gpuStableWorkBucket(launch, true, it->second);
    sh.tagged_classes.insert(rec.kernel_key.kernelClass);
    m.erase(it);
    return true;
  };
  if (take(sh.work_tag_corr) || take(sh.prev_work_tag_corr)) return true;
  if (all != nullptr)
    for (CuptiParseShard& o : *all)
      if (&o != &sh && (take(o.work_tag_corr) || take(o.prev_work_tag_corr))) return true;

  // No tag found yet. Whether that is final depends on what is known about
  // this kernel class, because a tag record can be in a buffer this drain has
  // not parsed. Deciding per class rather than per application matters: an
  // application that tags one kernel usually leaves its others untagged, and
  // parking all of them would give back the fast path this drain exists to
  // preserve.
  const uint64_t kernelClass = rec.kernel_key.kernelClass;
  if (!last_chance) {
    // Confirmed untagged by a previous full drain: nothing to wait for.
    if (sh.untagged_classes.count(kernelClass) != 0) return true;
    // Either known-tagged (so this is a missing tag, or one still unparsed)
    // or never seen before (so its first instance must not be committed to
    // the untagged bucket on incomplete information). Both need the full
    // drain first.
    return false;
  }

  // Every buffer has been parsed, so the absence is real. A legitimately
  // untagged class keeps the automatic launch-signature bucket and is
  // remembered so its later instances stay on the fast path.
  if (sh.tagged_classes.count(kernelClass) == 0) {
    sh.untagged_classes.insert(kernelClass);
    return true;
  }

  // The class has been seen tagged, so the tag went missing. The automatic
  // bucket is guaranteed by the hasExplicitTag discriminator to be a
  // different key from the one its tagged siblings use, and filing it there
  // would blend unrelated work sizes into a bucket the estimator treats as
  // one comparable population. Mark it unmodelable instead.
  rec.unmodelable = true;
  sh.lost_work_tags++;
  return true;
}

// The record walk of one queued buffer: CUPTI's activity records converted
// one by one on CUDA; the batch a HIP backend already converted. The three
// callbacks are the parse (hapiParseCuptiShardPass1).
template <class FK, class FC, class FA>
static void hapiTraceForEachRecord(CuptiBufferItem& item, FK&& onKernel, FC&& onCopy, FA&& onApi) {
#if CMK_CUDA
  if (item.batch == nullptr) {
    CUpti_Activity* record = NULL;
    while (cuptiActivityGetNextRecord(item.buffer, item.validSize, &record) == CUPTI_SUCCESS) {
      if (record->kind == CUPTI_ACTIVITY_KIND_CONCURRENT_KERNEL ||
          record->kind == CUPTI_ACTIVITY_KIND_KERNEL) {
        const CUpti_ActivityKernel4* k = (const CUpti_ActivityKernel4*)record;
        HapiTraceKernelRec r;
        r.start = k->start; r.end = k->end;
        r.deviceId = k->deviceId; r.streamId = k->streamId; r.correlationId = k->correlationId; r.threadId = 0;
        r.gridX = k->gridX; r.gridY = k->gridY; r.gridZ = k->gridZ;
        r.blockX = k->blockX; r.blockY = k->blockY; r.blockZ = k->blockZ;
        r.registersPerThread = k->registersPerThread;
        r.staticSharedMemory = k->staticSharedMemory; r.dynamicSharedMemory = k->dynamicSharedMemory;
        r.name = k->name;
        onKernel(&r);
      } else if (record->kind == CUPTI_ACTIVITY_KIND_MEMCPY) {
        const CUpti_ActivityMemcpy* mc = (const CUpti_ActivityMemcpy*)record;
        HapiTraceCopyRec r{'M', mc->deviceId, mc->streamId, mc->start, mc->end, mc->bytes, (int)mc->copyKind};
        onCopy(&r);
      } else if (record->kind == CUPTI_ACTIVITY_KIND_MEMSET) {
        const CUpti_ActivityMemset* ms = (const CUpti_ActivityMemset*)record;
        HapiTraceCopyRec r{'S', ms->deviceId, ms->streamId, ms->start, ms->end, ms->bytes, 0};
        onCopy(&r);
      } else if (record->kind == CUPTI_ACTIVITY_KIND_RUNTIME) {
        const CUpti_ActivityAPI* api = (const CUpti_ActivityAPI*)record;
        HapiTraceApiRec r{api->start, api->end, api->threadId, api->correlationId, (uint32_t)api->cbid};
        onApi(&r);
      }
    }
    return;
  }
#endif
  if (item.batch != nullptr) {
    for (const HapiTraceKernelRec& r : item.batch->kernels) onKernel(&r);
    for (const HapiTraceCopyRec& r : item.batch->copies) onCopy(&r);
    for (const HapiTraceApiRec& r : item.batch->apis) onApi(&r);
  }
}
// A queued buffer's records as a batch (a no-op for a batch already): the deferred close's drain sets the
// post-mark records of a CUDA buffer aside the way it does a HIP batch's. The buffer stays on the item and is
// retired with it.
static void hapiTraceItemToBatch(CuptiBufferItem& item) {
  if (item.batch != nullptr) return;
  HapiTraceBatch* b = new HapiTraceBatch;
  if (item.buffer != nullptr && item.validSize > 0)
    hapiTraceForEachRecord(item, [b](const HapiTraceKernelRec* k) { b->kernels.push_back(*k); },
                           [b](const HapiTraceCopyRec* c) { b->copies.push_back(*c); },
                           [b](const HapiTraceApiRec* a) { b->apis.push_back(*a); });
  item.batch = b;
}

// Pass 1, one shard, its own thread: every record of its buffers, resolved against this shard
// alone; what needs another shard waits in sh.pending / sh.pendingApi for pass 2.
static void hapiParseCuptiShardPass1(GPUManager& gm, CuptiParseShard& sh, std::vector<CuptiBufferItem>& items,
                                     const std::unordered_map<uint32_t, int>& devByThread, bool scaling) {
  if (_lb_args.debug() > 1)
    CmiPrintf("HAPI[pe=%d] pass1 entry: carried=%zu prev_object_corr=%zu object_corr=%zu\n", CmiMyPe(),
              sh.carried.size(), sh.prev_object_corr.size(), sh.object_corr.size());
  sh.pending = std::move(sh.carried);
  sh.carried.clear();
  for (CuptiPendingKernel& p : sh.pending) p.carried = true;
  sh.pending.reserve(sh.pending.size() + sh.pending_hint);
  sh.pendingApi.clear();
  sh.apiByToken.clear();
  sh.resolved_tokens.clear();
  sh.thread_corr.clear();
  sh.cover_end.clear();
  sh.byCbid.assign(hapiTraceBackendCallbackIdCount() + 1, CuptiCbidUse());
  sh.api_unattributed_s = 0.0;
  sh.api_count = sh.kernel_count = sh.object_corr_count = sh.work_tag_count = 0;
  sh.invalid_duration_count = sh.hash_collision_count = sh.attributed = sh.unattributed = 0;
  sh.unresolved_token = sh.deferred = sh.lost_work_tags = 0;
  sh.carried_out = sh.carried_in_resolved = sh.carried_in_lost = 0;
  sh.api_corr_max = 0; sh.carried_corr_min = UINT32_MAX; sh.carried_corr_max = 0; sh.carried_above_api = 0;
  sh.carried_names.clear();
  sh.carried_pe_thread = sh.carried_other_thread = 0; sh.carried_threads.clear();
  sh.api_zero_dur = sh.api_launch_records = sh.copy_records = 0;
  sh.corr_cbid.clear(); sh.kernels_by_nonlaunch_cbid.clear(); sh.api_by_thread.clear(); sh.carried_examples = 0;
  auto devOfThread = [&](uint32_t tid) -> int {
    auto it = devByThread.find(tid);
    return it != devByThread.end() ? it->second : -1;
  };
  // Most runtime records are the scheduler polling an event between entry
  // methods -- millions per window on a GPU-bound step, at a microsecond
  // each. They never belong to an object, so they skip the correlation
  // table (a lookup that misses in a table of every launch this window),
  // and a thread's back-to-back calls are folded into one interval as they
  // are read, which is what the union would make of them anyway. Measured
  // 12 Sep 2026: storing and sorting every record cost 3-5 s per LB step on
  // sph2d at 12.5M particles, most of the step's regression.
  // Per-record state is kept in flat arrays: at 85 million records a window
  // (measured, sph2d 12.5M particles, the process with the fluid) even a small
  // hash lookup per record is seconds. Callback ids are small integers, and
  // records arrive in long runs from one thread, so a one-entry thread cache
  // almost always hits.
  struct LastInterval { size_t idx = SIZE_MAX; uint64_t start = 0; };  // SIZE_MAX: none yet
  std::unordered_map<uint32_t, LastInterval> lastByThread;
  // The entry-span logs of the threads whose API records this drain reads, each copied once per
  // close under its owner's mutex (the owner keeps appending while we parse; every record here was
  // made before the flush, so its span is already in the copy, open or closed). The copies are
  // shared by the shards (g_cupti_snaps, node-stable); this shard caches the pointers it uses.
  std::map<uint32_t, CuptiLaneCursor> mySnaps;   // node-stable: curSnap points into it
  uint32_t curTid = 0;
  CuptiLaneCursor* curSnap = nullptr;
  auto snapOf = [&](uint32_t tid) -> CuptiLaneCursor& {
    if (curSnap != nullptr && curTid == tid) return *curSnap;
    CuptiLaneCursor& cur = mySnaps[tid];
    curTid = tid;
    curSnap = &cur;
    if (cur.sn != nullptr) return cur;
    std::lock_guard<std::mutex> sg(g_cupti_snaps_lock);
    CuptiLaneSnap& sn = g_cupti_snaps[tid];
    if (!sn.taken) {
      sn.taken = true;
      CuptiEntryLog* L = nullptr;
      {
        std::lock_guard<std::mutex> g(g_cupti_entry_logs_lock);
        auto it = g_cupti_entry_logs.find(tid);
        if (it != g_cupti_entry_logs.end()) L = it->second;
      }
      if (L != nullptr) {
        std::lock_guard<std::mutex> g(L->m);
        hapiCuptiLaneRefresh(L->obj, sn.obj);
        if (scaling) hapiCuptiLaneRefresh(L->tag, sn.tag);
      }
    }
    cur.sn = &sn;
    return cur;
  };
  uint32_t cachedThread = 0;
  LastInterval* cachedLast = nullptr;
  const uint64_t kCoalesceNs = 2000;

  for (CuptiBufferItem& item : items) {
    // A buffer CUPTI never wrote to (or one it handed back empty) has nothing
    // to parse, and passing it on would dereference whatever came back.
    if (item.batch == nullptr && (item.buffer == NULL || item.validSize == 0)) {
      sh.retired.push_back({item.buffer, item.size});
      continue;
    }

    // Parse the records of this buffer, whichever vendor's tracer produced
    // them (hapiTraceForEachRecord converts as it walks).
    hapiTraceForEachRecord(item,
      [&](const HapiTraceKernelRec* kernel) {
        sh.kernel_count++;

        DeviceManager* dm = findDeviceManager(gm, kernel->deviceId);

        LBKernelRecord rec{};
        rec.start_ns  = kernel->start;
        rec.end_ns    = kernel->end;
        rec.device_id = kernel->deviceId;
        rec.sms_used  = dm ? computeKernelSMs(*dm, kernel) : 1;
        if (rec.end_ns <= rec.start_ns) sh.invalid_duration_count++;
        else g_cupti_kernel_ns.fetch_add(rec.end_ns - rec.start_ns,
                                         std::memory_order_relaxed);

        GpuLaunchSignature launch;
        bool bucket_settled = true;
        // The kernel's identity for the audit's unowned-by-kernel breakdown, scaling or not.
        static const bool audit_names = (getenv("CHARM_GPU_LOAD_AUDIT") != nullptr);
        if (audit_names && !scaling) {
          rec.kernel_key.kernelClass = gpuStableKernelClass(kernel->name);
          sh.kernel_names.emplace(rec.kernel_key.kernelClass, kernel->name != nullptr ? kernel->name : "");
        }
        if (scaling) {
          launch.gridX = kernel->gridX;
          launch.gridY = kernel->gridY;
          launch.gridZ = kernel->gridZ;
          launch.blockX = kernel->blockX;
          launch.blockY = kernel->blockY;
          launch.blockZ = kernel->blockZ;
          launch.staticSharedMemory = kernel->staticSharedMemory;
          launch.dynamicSharedMemory = kernel->dynamicSharedMemory;
          rec.kernel_key.kernelClass = gpuStableKernelClass(kernel->name);
          rec.kernel_key.workBucket = gpuStableWorkBucket(launch, false, 0);

          if (_lb_args.debug() > 0 || audit_names) {
            const char* kernel_name = kernel->name != nullptr ? kernel->name : "";
            auto inserted = sh.kernel_names.emplace(rec.kernel_key.kernelClass, kernel_name);
            if (!inserted.second && inserted.first->second != kernel_name)
              sh.hash_collision_count++;
          }

          bucket_settled =
              cuptiApplyWorkTag(sh, nullptr, kernel->correlationId, launch, rec, /*last_chance=*/false);
        }

        // The correlation stays in the table: the launch's own API record
        // shares the id and may still be coming. Everything left is dropped
        // when the drain ends.
        const uint64_t* object = cuptiFindObjectCorr(sh, kernel->correlationId, nullptr);
        if (_lb_args.debug() > 1) { auto cb = sh.corr_cbid.find(kernel->correlationId); if (cb != sh.corr_cbid.end()) sh.kernels_by_nonlaunch_cbid[cb->second]++; }
        if (g_trace != nullptr)
          hapiTraceKernel(kernel, object != nullptr ? *object : HAPI_CUPTI_NO_OBJECT);
        if (bucket_settled && object != nullptr) {
          cuptiFileKernel(gm, sh, rec, *object, cuptiThreadOfCorr(sh, kernel->correlationId, nullptr));
        } else {
          sh.pending.push_back({kernel->correlationId, kernel->threadId, launch, rec, false});
          sh.deferred++;
        }
      
      },
      [&](const HapiTraceCopyRec* mc) {
        sh.copy_records++;
        if (g_trace != nullptr)
          hapiTraceCopy(mc->kind, mc->deviceId, mc->streamId, mc->start, mc->end, mc->bytes, mc->copyKind);
        if (mc->end > mc->start) {
          g_cupti_memcpy_n.fetch_add(1, std::memory_order_relaxed);
          g_cupti_memcpy_ns.fetch_add(mc->end - mc->start, std::memory_order_relaxed);
          if (mc->kind == 'M') g_cupti_memcpy_bytes.fetch_add(mc->bytes, std::memory_order_relaxed);
        }
      },
      [&](const HapiTraceApiRec* api) {
        if (api->end <= api->start) sh.api_zero_dur++;
        if (api->end > api->start) {
          sh.api_count++;
          const double seconds = (double)(api->end - api->start) / 1.0e9;
          sh.api_total += seconds;
          sh.api_total_dev[devOfThread(api->threadId)] += seconds;
          CuptiCbidUse& use = sh.byCbid[api->cbid < sh.byCbid.size() ? api->cbid : sh.byCbid.size() - 1];
          if (!use.known) {
            use.known = true;
            const char* name = hapiTraceBackendCallbackName(api->cbid);
            if (name != nullptr) {
              use.poll = (strstr(name, "Query") != nullptr);
              use.launch = (strstr(name, "Launch") != nullptr);
            }
          }
          use.count++;
          use.seconds += seconds;
          if (use.launch) sh.api_launch_records++;
          {
            uint64_t& ce = sh.cover_end[api->threadId];
            if (api->end > ce) ce = api->end;
          }
          if (!use.poll) {   // the span this thread was in when the call began names the object (and the work tag)
            sh.thread_corr[api->correlationId] = api->threadId;
            if (api->correlationId > sh.api_corr_max) sh.api_corr_max = api->correlationId;
            if (_lb_args.debug() > 1) {
              if (!use.launch) sh.corr_cbid[api->correlationId] = api->cbid;
              sh.api_by_thread[api->threadId].push_back({api->correlationId, api->cbid, api->start, api->end});
            }
            CuptiLaneCursor& cur = snapOf(api->threadId);
            const bool isLaunch = use.launch;
            if (const CuptiEntrySpan* sp = hapiCuptiSpanAt(cur.sn->obj.spans, api->start, cur.obj)) {
              sh.object_corr_count++;
              sh.object_corr[api->correlationId] = sp->token;
              if (isLaunch) { CuptiParseShard::ThreadAttr& ta = sh.perThread[api->threadId]; if (sp->token == HAPI_CUPTI_NO_OBJECT) ta.joined++; else ta.owned++; }
            } else if (isLaunch) sh.perThread[api->threadId].miss++;
            if (scaling)
              if (const CuptiEntrySpan* sp = hapiCuptiSpanAt(cur.sn->tag.spans, api->start, cur.tag)) {
                sh.work_tag_count++;
                sh.work_tag_corr[api->correlationId] = sp->token;
              }
          }
          if (cachedLast == nullptr || cachedThread != api->threadId) {
            cachedThread = api->threadId;
            cachedLast = &lastByThread[api->threadId];
          }
          LastInterval& last = *cachedLast;
          if (last.idx != SIZE_MAX && last.idx < sh.api_intervals.size() &&
              api->start >= last.start &&
              api->start <= sh.api_intervals[last.idx].end + kCoalesceNs) {
            auto& iv = sh.api_intervals[last.idx];
            if (api->end > iv.end) iv.end = api->end;
          } else {
            last.idx = sh.api_intervals.size();
            last.start = api->start;
            sh.api_intervals.push_back({api->start, api->end, devOfThread(api->threadId)});
          }
          if (use.poll) {
            sh.api_unattributed_s += seconds;
          } else {
            const uint64_t* object = cuptiFindObjectCorr(sh, api->correlationId, nullptr);
            if (object != nullptr)
              sh.apiByToken[*object] += seconds;
            else
              sh.pendingApi.push_back({api->correlationId, seconds});
          }
        }
      
      });
    if (item.batch != nullptr) { delete item.batch; item.batch = nullptr; }

    sh.retired.push_back({item.buffer, item.size});
  }
}

// Pass 2, serial, every shard: what pass 1 could not resolve from its own shard, against all of
// them; then the API attribution, the bookkeeping and the carry-over to the next drain.
static void hapiParseCuptiShardPass2(GPUManager& gm, CuptiParseShard& sh, std::vector<CuptiParseShard>& all,
                                     bool scaling, int shardIdx) {
  // Second pass: correlations that arrived in a later buffer, or in another shard, are now known.
  for (CuptiPendingKernel& pending_kernel : sh.pending) {
    const uint64_t* object = cuptiFindObjectCorr(sh, pending_kernel.correlation_id, &all);
    // Its correlation record may still be in a buffer CUPTI has not delivered: park it once more.
    if (object == nullptr && !pending_kernel.carried) {
      sh.carried.push_back(pending_kernel); sh.carried_out++;
      const uint32_t c = pending_kernel.correlation_id;
      if (c < sh.carried_corr_min) sh.carried_corr_min = c;
      if (c > sh.carried_corr_max) sh.carried_corr_max = c;
      if (c > sh.api_corr_max) sh.carried_above_api++;
      if (_lb_args.debug() > 1 && sh.carried_examples < 6) {   // where in the ringing thread's call sequence the dispatch fell
        sh.carried_examples++;
        auto it = sh.api_by_thread.find(pending_kernel.enqueue_thread);
        const uint32_t c = pending_kernel.correlation_id;
        const CuptiParseShard::ApiBrief *below = nullptr, *above = nullptr;
        if (it != sh.api_by_thread.end())
          for (const auto& a : it->second) {
            if (a.corr < c && (below == nullptr || a.corr > below->corr)) below = &a;
            if (a.corr > c && (above == nullptr || a.corr < above->corr)) above = &a;
          }
        auto nm = [](const CuptiParseShard::ApiBrief* a) { const char* n = a ? hapiTraceBackendCallbackName(a->cbid) : nullptr; return n ? n : "?"; };
        auto kn = gm.cupti_kernel_names_.find(pending_kernel.rec.kernel_key.kernelClass);
        CmiPrintf("HAPI[pe=%d] carried example: thread %u corr %u kernel %s dev_start %llu | below: corr %u %s end %+lld us | above: corr %u %s start %+lld us\n",
                  CmiMyPe(), pending_kernel.enqueue_thread, c,
                  kn != gm.cupti_kernel_names_.end() ? kn->second.substr(0, 24).c_str() : "?",
                  (unsigned long long)pending_kernel.rec.start_ns,
                  below ? below->corr : 0u, nm(below), below ? (long long)((int64_t)below->end - (int64_t)pending_kernel.rec.start_ns) / 1000 : 0LL,
                  above ? above->corr : 0u, nm(above), above ? (long long)((int64_t)above->start - (int64_t)pending_kernel.rec.start_ns) / 1000 : 0LL);
      }
      {
        const uint32_t t = pending_kernel.enqueue_thread;
        bool pe = false;
        if (t != 0) { std::lock_guard<std::mutex> g(g_cupti_entry_logs_lock); pe = g_cupti_entry_logs.count(t) > 0; }
        if (pe) sh.carried_pe_thread++; else sh.carried_other_thread++;
        sh.carried_threads[t]++;
      }
      if (_lb_args.debug() > 1) {
        auto it = gm.cupti_kernel_names_.find(pending_kernel.rec.kernel_key.kernelClass);
        sh.carried_names[it != gm.cupti_kernel_names_.end() ? it->second : std::string("?")]++;
      }
      continue;
    }
    if (pending_kernel.carried) { if (object != nullptr) sh.carried_in_resolved++; else sh.carried_in_lost++; }
    if (scaling)
      cuptiApplyWorkTag(sh, &all, pending_kernel.correlation_id, pending_kernel.launch,
                        pending_kernel.rec, /*last_chance=*/true);
    if (object == nullptr) {
      sh.unattributed_kernels.push_back(pending_kernel.rec);
      sh.unattributed++;
      continue;
    }
    cuptiFileKernel(gm, sh, pending_kernel.rec, *object, cuptiThreadOfCorr(sh, pending_kernel.correlation_id, &all));
  }
  sh.pending.clear();

  // API records whose correlation arrived later, then every token to its
  // object. A call made outside any entry method -- the scheduler polling an
  // event, say -- belongs to no object; it still occupied the driver and is
  // in the intervals, so the launch term counts it, but nobody is charged.
  for (const CuptiPendingApi& p : sh.pendingApi) {
    const uint64_t* object = cuptiFindObjectCorr(sh, p.correlation_id, &all);
    if (object == nullptr) sh.api_unattributed_s += p.seconds;
    else sh.apiByToken[*object] += p.seconds;
  }
  sh.pendingApi.clear();
  for (const auto& kv : sh.apiByToken) {
    auto memo = sh.resolved_tokens.find(kv.first);
    if (memo == sh.resolved_tokens.end()) {
      CuptiResolvedToken entry;
      {
        std::lock_guard<std::mutex> token_lock(gm.cupti_object_token_lock_);
        entry.valid = gm.cupti_object_tokens_.resolve(kv.first, entry.key);
      }
      memo = sh.resolved_tokens.emplace(kv.first, entry).first;
    }
    if (memo->second.valid) sh.obj_api_raw[memo->second.key] += kv.second;
    else sh.api_unattributed_s += kv.second;
  }
  sh.apiByToken.clear();
  if (_lb_args.debug() > 1) {
    CmiPrintf("HAPI[pe=%d] shard %d: runtime API records=%u total=%.6fs unattributed=%.6fs "
              "objects=%zu intervals=%zu\n", CmiMyPe(), shardIdx, sh.api_count, sh.api_total,
              sh.api_unattributed_s, sh.obj_api_raw.size(), sh.api_intervals.size());
    // Where the driver time went, by call: the few calls that carry most of it.
    std::vector<std::pair<uint32_t, CuptiCbidUse>> top;
    for (uint32_t c = 0; c < sh.byCbid.size(); c++)
      if (sh.byCbid[c].count > 0) top.emplace_back(c, sh.byCbid[c]);
    std::sort(top.begin(), top.end(), [](const std::pair<uint32_t, CuptiCbidUse>& a,
                                         const std::pair<uint32_t, CuptiCbidUse>& b) {
      return a.second.seconds > b.second.seconds;
    });
    std::string line;
    for (size_t i = 0; i < top.size() && i < 6; i++) {
      const char* name = hapiTraceBackendCallbackName(top[i].first);
      if (name == nullptr) name = "?";
      char buf[160];
      snprintf(buf, sizeof buf, " %s x%llu %.3fs", name, (unsigned long long)top[i].second.count,
               top[i].second.seconds);
      line += buf;
    }
    CmiPrintf("HAPI[pe=%d] shard %d: runtime API by call:%s\n", CmiMyPe(), shardIdx, line.c_str());
  }

  // Size next round's parked vector from this one: the straggler count is a
  // property of how CUPTI is batching buffers, which changes slowly.
  sh.pending_hint = sh.deferred;

  // A lost work tag is not just a lost sample: it would otherwise file the
  // kernel under a bucket its tagged siblings never use, so report it at the
  // first debug level rather than burying it with the bookkeeping counters.
  if (_lb_args.debug() > 0 && sh.lost_work_tags > 0) {
    CmiPrintf("HAPI[pe=%d]: %u kernel record(s) of a tagged class arrived with "
              "no work tag and were excluded from the scaling model\n",
              CmiMyPe(), sh.lost_work_tags);
  }

  if (_lb_args.debug() > 1) {
    {
      std::vector<std::pair<uint32_t, std::string>> top;
      for (const auto& kv : sh.carried_names) top.push_back({kv.second, kv.first});
      std::sort(top.rbegin(), top.rend());
      std::string names;
      for (size_t i = 0; i < top.size() && i < 4; i++) names += " " + std::to_string(top[i].first) + ":" + top[i].second.substr(0, 28);
      std::vector<std::pair<uint32_t, uint32_t>> tt;
      for (const auto& kv : sh.carried_threads) tt.push_back({kv.second, kv.first});
      std::sort(tt.rbegin(), tt.rend());
      std::string threads;
      for (size_t i = 0; i < tt.size() && i < 5; i++) threads += " " + std::to_string(tt[i].second) + "x" + std::to_string(tt[i].first);
      CmiPrintf("HAPI[pe=%d] shard %d: carried corr %u..%u vs api max %u, above_api=%u; pe_thread=%u other_thread=%u threads:%s;%s\n",
                CmiMyPe(), shardIdx, sh.carried_corr_min == UINT32_MAX ? 0u : sh.carried_corr_min, sh.carried_corr_max,
                sh.api_corr_max, sh.carried_above_api, sh.carried_pe_thread, sh.carried_other_thread, threads.c_str(), names.c_str());
      {
        std::vector<std::pair<uint32_t, uint32_t>> nl;
        for (const auto& kv : sh.kernels_by_nonlaunch_cbid) nl.push_back({kv.second, kv.first});
        std::sort(nl.rbegin(), nl.rend());
        std::string names;
        for (size_t i = 0; i < nl.size() && i < 6; i++) {
          const char* nm = hapiTraceBackendCallbackName(nl[i].second);
          names += " " + std::to_string(nl[i].first) + ":" + (nm ? nm : "?");
        }
        if (!nl.empty()) CmiPrintf("HAPI[pe=%d] shard %d: kernels whose record is a non-launch call:%s\n", CmiMyPe(), shardIdx, names.c_str());
      }
      CmiPrintf("HAPI[pe=%d] shard %d: records: api launch=%u zero_duration=%u kernels=%u copies=%u (launch - kernels - copies = %lld)\n",
                CmiMyPe(), shardIdx, sh.api_launch_records, sh.api_zero_dur, sh.kernel_count, sh.copy_records,
                (long long)sh.api_launch_records - (long long)sh.kernel_count - (long long)sh.copy_records);
    }
    for (const auto& kv : sh.perThread)
      CmiPrintf("HAPI[pe=%d] shard %d: thread %u launches: owned=%u joined=%u nospan=%u\n", CmiMyPe(), shardIdx,
                kv.first, kv.second.owned, kv.second.joined, kv.second.miss);
    sh.perThread.clear();
    CmiPrintf("HAPI[pe=%d] shard %d: hapiProcessCuptiBuffers kernels=%u "
              "object_correlations=%u work_tags=%u attributed=%u "
              "unattributed=%u deferred=%u invalid_durations=%u "
              "unresolved_tokens=%u hash_collisions=%u lost_work_tags=%u "
              "objects=%zu object_corr_dropped=%zu work_tags_dropped=%zu carried_out=%u carried_in=%u/%u\n",
              CmiMyPe(), shardIdx, sh.kernel_count, sh.object_corr_count, sh.work_tag_count,
              sh.attributed, sh.unattributed, sh.deferred, sh.invalid_duration_count,
              sh.unresolved_token, sh.hash_collision_count, sh.lost_work_tags,
              sh.obj_kernel_records.size(), sh.prev_object_corr.size(),
              sh.prev_work_tag_corr.size(), sh.carried_out, sh.carried_in_resolved, sh.carried_in_lost);
  }
}

// The drain: every queued buffer, dealt round-robin over one shard per device, pass 1 of each
// shard on its own thread (shard 0 on this one), pass 2 serial, then the merge. Called with
// cupti_prepare_lock_ held.
// Phase times of the last drain (CHARM_LB_CUPTI_TIME only), read by the close's timing print.
static double g_cupti_drain_prof[4] = {0, 0, 0, 0};   // retire, pass 1, pass 2, merge
static size_t g_cupti_drain_bufs = 0;        // buffers this drain took from the queue, and their records' bytes
static uint64_t g_cupti_drain_valid_kb = 0;
// Moves the records that start at or after the mark from vec to out, keeping order.
template <class V>
static void hapiSplitAfterMark(V& vec, uint64_t mark, V& out) {
  size_t keep = 0;
  for (size_t i = 0; i < vec.size(); i++) {
    if (vec[i].start >= mark) out.push_back(vec[i]);
    else vec[keep++] = vec[i];
  }
  vec.resize(keep);
}
// Kernels by LAUNCH, not start: the round owns what was launched before the mark, also a kernel still queued
// at the mark that ran after it. Correlation ids come from one counter of the process, taken when the call
// begins, and the mark is stamped with no gated launch in flight, so every launch before the mark has an id at
// or below corr_mark (the largest of the API records that began before it) and every later one above.
// g_cupti_kept_started_after counts the kernels the split by start would have moved to the next round.
static size_t g_cupti_kept_started_after = 0;
template <class V>
static void hapiSplitKernelsAfterLaunch(V& vec, uint32_t corr_mark, uint64_t mark, V& out) {
  size_t keep = 0;
  for (size_t i = 0; i < vec.size(); i++) {
    if (vec[i].correlationId > corr_mark) { out.push_back(vec[i]); continue; }
    if (vec[i].start >= mark) g_cupti_kept_started_after++;
    vec[keep++] = vec[i];
  }
  vec.resize(keep);
}
void hapiProcessCuptiBuffers() {
  GPUManager& gm = CsvAccess(gpu_manager);
  hapiPopulateDeviceProps(gm);  // lazy: device_managers is ready by now
  const bool scaling = _lb_args.gpuScaling();
  static const bool timeIt = (getenv("CHARM_LB_CUPTI_TIME") != nullptr);
  const double p0 = timeIt ? CmiWallTimer() : 0.0;
  g_cupti_drain++;
  hapiCuptiFreeRetired(2);
  g_cupti_cover_end.clear();
  const double p1 = timeIt ? CmiWallTimer() : 0.0;

  const size_t nShards = (size_t)std::max(1, gm.device_count);
  if (g_cupti_shards.size() < nShards) g_cupti_shards.resize(nShards);
  std::vector<std::vector<CuptiBufferItem>> items(g_cupti_shards.size());
  {
    std::lock_guard<std::mutex> lk(gm.cupti_queue_lock_);
    size_t i = 0;
    uint64_t valid = 0;
    // The deferred close: records of work issued after the mark belong to the next round and go back to the
    // front of the queue; what this drain parses is complete to the mark and is counted whole.
    g_cupti_drain_deferred = g_cupti_mark_set;
    // The last launch before the mark (see hapiSplitKernelsAfterLaunch); without API records, kernels split
    // by start. Copy records carry no correlation id and split by start.
    uint32_t corr_mark = 0;
    bool have_corr_mark = false;
    g_cupti_kept_started_after = 0;
    if (g_cupti_mark_set) {
      for (CuptiBufferItem& q : gm.cupti_buffer_queue_) {
        hapiTraceItemToBatch(q);
        if (q.batch == nullptr) continue;
        for (const HapiTraceApiRec& a : q.batch->apis)
          if (a.start < g_cupti_mark_ns && (!have_corr_mark || a.correlationId > corr_mark)) {
            corr_mark = a.correlationId;
            have_corr_mark = true;
          }
      }
    }
    std::vector<CuptiBufferItem> after;
    while (!gm.cupti_buffer_queue_.empty()) {
      CuptiBufferItem it = gm.cupti_buffer_queue_.front();
      gm.cupti_buffer_queue_.pop_front();
      if (g_cupti_mark_set) hapiTraceItemToBatch(it);   // CUDA: the buffer's records, complete to the mark
      if (g_cupti_mark_set && it.batch != nullptr) {
        const uint64_t mark = g_cupti_mark_ns;
        HapiTraceBatch* post = new HapiTraceBatch;
        if (have_corr_mark) hapiSplitKernelsAfterLaunch(it.batch->kernels, corr_mark, mark, post->kernels);
        else hapiSplitAfterMark(it.batch->kernels, mark, post->kernels);
        hapiSplitAfterMark(it.batch->apis, mark, post->apis);
        hapiSplitAfterMark(it.batch->copies, mark, post->copies);
        if (post->kernels.empty() && post->apis.empty() && post->copies.empty()) { delete post; post = nullptr; }
        if (post != nullptr) {
          const size_t n_post = post->kernels.size() + post->apis.size() + post->copies.size();
          const size_t n_pre = it.batch->kernels.size() + it.batch->apis.size() + it.batch->copies.size();
          const size_t bytes_post = (n_pre + n_post) ? it.validSize * n_post / (n_pre + n_post) : 0;
          after.push_back({nullptr, bytes_post, 0, nullptr, post});
          it.validSize -= bytes_post;
        }
        // Nothing before the mark: a batch alone is dropped here, a CUDA buffer goes on (empty) to be retired.
        if (it.buffer == nullptr && it.batch->kernels.empty() && it.batch->apis.empty() && it.batch->copies.empty()) {
          delete it.batch;
          continue;
        }
      }
      valid += it.validSize;
      items[i % nShards].push_back(it);
      i++;
    }
    for (auto r = after.rbegin(); r != after.rend(); ++r) gm.cupti_buffer_queue_.push_front(*r);
    if (g_cupti_mark_set) { g_cupti_mark_set = false; g_cupti_close.scanned = 0; }
    g_cupti_drain_bufs = i;
    g_cupti_drain_valid_kb = valid >> 10;
  }
  std::unordered_map<uint32_t, int> devByThread;
  {
    std::lock_guard<std::mutex> tl(gm.cupti_thread_device_lock_);
    devByThread = gm.cupti_thread_device_;
  }
  for (auto& kv : g_cupti_snaps) kv.second.taken = false;   // the copies persist; each drain refreshes them incrementally
  g_cupti_snap_bytes.store(0, std::memory_order_relaxed);
  g_cupti_snap_ns.store(0, std::memory_order_relaxed);
  // Every registered thread's entry-span log is copied/refreshed here, in parallel on the parse workers (one
  // lane per worker in turn), so pass 1 finds every copy ready. Taken first-come by the shards, the copies were
  // made one at a time under g_cupti_snaps_lock while the other shards waited (15 MB per close in a 16-PE
  // process, 1.8 ms, job 22868252). A thread with records but no registered log still goes through snapOf.
  {
    std::vector<std::pair<uint32_t, CuptiEntryLog*>> logs;
    {
      std::lock_guard<std::mutex> g(g_cupti_entry_logs_lock);
      logs.assign(g_cupti_entry_logs.begin(), g_cupti_entry_logs.end());
    }
    std::vector<CuptiLaneSnap*> snaps(logs.size());
    {
      std::lock_guard<std::mutex> sg(g_cupti_snaps_lock);
      for (size_t i = 0; i < logs.size(); i++) {
        CuptiLaneSnap& sn = g_cupti_snaps[logs[i].first];   // std::map: the pointer stays valid
        sn.taken = true;
        snaps[i] = &sn;
      }
    }
    auto refreshLanes = [&logs, &snaps, scaling, nShards](size_t w) {
      for (size_t i = w; i < logs.size(); i += nShards) {
        CuptiEntryLog* L = logs[i].second;
        std::lock_guard<std::mutex> g(L->m);
        hapiCuptiLaneRefresh(L->obj, snaps[i]->obj);
        if (scaling) hapiCuptiLaneRefresh(L->tag, snaps[i]->tag);
      }
    };
    for (size_t i = 1; i < nShards; i++) hapiCuptiWorkerRun(i - 1, [&refreshLanes, i]() { refreshLanes(i); });
    refreshLanes(0);
    hapiCuptiWorkersWait();
  }
  // Every shard starts from the class knowledge the process has so far.
  for (CuptiParseShard& sh : g_cupti_shards) {
    sh.tagged_classes = gm.cupti_tagged_kernel_classes_;
    sh.untagged_classes = gm.cupti_untagged_kernel_classes_;
  }
  for (size_t i = 1; i < g_cupti_shards.size(); i++) {
    CuptiParseShard* sh = &g_cupti_shards[i];
    std::vector<CuptiBufferItem>* its = &items[i];
    hapiCuptiWorkerRun(i - 1, [&gm, sh, its, &devByThread, scaling]() {
      hapiParseCuptiShardPass1(gm, *sh, *its, devByThread, scaling);
    });
  }
  hapiParseCuptiShardPass1(gm, g_cupti_shards[0], items[0], devByThread, scaling);
  hapiCuptiWorkersWait();
  const double p2 = timeIt ? CmiWallTimer() : 0.0;
  for (size_t i = 0; i < g_cupti_shards.size(); i++)
    hapiParseCuptiShardPass2(gm, g_cupti_shards[i], g_cupti_shards, scaling, (int)i);
  const double p3 = timeIt ? CmiWallTimer() : 0.0;

  // Every entry method pushes a correlation ID, and CUPTI emits a record for
  // each runtime call made under it -- memcpys, syncs and so on, not just
  // kernel launches. Those never match a kernel record. We device-sync and
  // force-flush before draining, so any kernel that was going to arrive has
  // arrived; whatever is left over is non-kernel traffic and would otherwise
  // accumulate without bound. (After pass 2 of every shard: a shard's pass 2
  // reads the others' current maps.)
  for (CuptiParseShard& sh : g_cupti_shards) {
    sh.prev_object_corr = std::move(sh.object_corr);
    sh.prev_work_tag_corr = std::move(sh.work_tag_corr);
    sh.object_corr.clear();
    sh.work_tag_corr.clear();
  }

  // Merge.
  for (CuptiParseShard& sh : g_cupti_shards) {
    for (auto& kv : sh.obj_kernel_records) {
      auto& dst = gm.cupti_obj_kernel_records_[kv.first];
      if (dst.empty()) dst = std::move(kv.second);
      else dst.insert(dst.end(), kv.second.begin(), kv.second.end());
    }
    sh.obj_kernel_records.clear();
    gm.cupti_unattributed_kernels_.insert(gm.cupti_unattributed_kernels_.end(),
                                          sh.unattributed_kernels.begin(), sh.unattributed_kernels.end());
    sh.unattributed_kernels.clear();
    gm.cupti_tagged_kernel_classes_.insert(sh.tagged_classes.begin(), sh.tagged_classes.end());
    gm.cupti_untagged_kernel_classes_.insert(sh.untagged_classes.begin(), sh.untagged_classes.end());
    for (auto& kv : sh.kernel_names) gm.cupti_kernel_names_.emplace(kv.first, kv.second);
    sh.kernel_names.clear();
    for (auto& kv : sh.obj_api_raw) gm.cupti_obj_api_raw_acc_[kv.first] += kv.second;
    sh.obj_api_raw.clear();
    for (auto& kv : sh.obj_thread) g_cupti_obj_thread[kv.first] = kv.second;
    sh.obj_thread.clear();
    for (auto& kv : sh.cover_end) {
      uint64_t& ce = g_cupti_cover_end[kv.first];
      if (kv.second > ce) ce = kv.second;
    }
    sh.cover_end.clear();
    gm.cupti_api_total_acc_ += sh.api_total;
    sh.api_total = 0.0;
    for (auto& kv : sh.api_total_dev) gm.cupti_api_total_dev_acc_[kv.first] += kv.second;
    sh.api_total_dev.clear();
    gm.cupti_api_intervals_acc_.insert(gm.cupti_api_intervals_acc_.end(),
                                       sh.api_intervals.begin(), sh.api_intervals.end());
    sh.api_intervals.clear();
    for (auto& b : sh.retired) hapiCuptiRetireBuffer(b.first, b.second);
    sh.retired.clear();
    gm.cupti_pending_hint_ = std::max(gm.cupti_pending_hint_, sh.pending_hint);
  }
  if (timeIt) {
    const double p4 = CmiWallTimer();
    g_cupti_drain_prof[0] = p1 - p0; g_cupti_drain_prof[1] = p2 - p1;
    g_cupti_drain_prof[2] = p3 - p2; g_cupti_drain_prof[3] = p4 - p3;
  }

  if (_lb_args.debug() > 1) {
    // Cumulative, not per drain: kernel device time CUPTI saw at all, against
    // the copies no object owns. Compare the first against the balancer's T_g
    // to see how much of what it CAN see it actually attributes, and the second
    // against the interval to see whether the rest of the step is idle GPU.
    CmiPrintf("HAPI[pe=%d]: drain over %zu shard(s); cupti-device-total kernel_s=%.6f memcpy_n=%llu "
              "memcpy_s=%.6f memcpy_GB=%.3f\n",
              CmiMyPe(), g_cupti_shards.size(),
              (double)g_cupti_kernel_ns.load() / 1.0e9,
              (unsigned long long)g_cupti_memcpy_n.load(),
              (double)g_cupti_memcpy_ns.load() / 1.0e9,
              (double)g_cupti_memcpy_bytes.load() / 1.0e9);
  }
}

// Convert this process's raw kernel timeline into one SM-utilization-
// normalized load per object, in seconds of whole-device occupancy.
//
// Per-device FIFO sweep-line over all kernel intervals. At each event (kernel
// start or end) the active kernels are walked in submission order (earliest
// start_ns first) and each is granted min(its sms_used, remaining capacity);
// its share of the device over the segment is integrated into the owning
// object's load. This matches the GPU's block-level FIFO scheduler: a running
// kernel keeps its allocation and a later arrival gets only what is left.
//
// Because the result is SM-seconds of demand rather than elapsed time, it is
// (to first order) invariant to how the objects happen to be placed right now,
// which is what makes it usable as a load estimate for a *different* placement.
//
// This runs in the process that produced the records, on rank 0, before the
// stats leave for the central LB: every PE bound to a device is in the same
// process, so the timeline for that device is already complete here. If
// several processes share one GPU, each sees only its own kernels and the
// contention between processes is not modelled.
// Which device (global index) each object's kernels last ran on, so a device's close can drop
// the loads of its own objects before rebuilding them (an object with no record this round has
// no GPU load) and leave the other devices' loads alone.
static std::unordered_map<LDObjKey, uint32_t, LDObjKeyHash> g_cupti_obj_device;   // under cupti_prepare_lock_
// onlyDevice: the global device index whose kernels this call normalises (-1: every device).
void hapiNormalizeCuptiLoads(int onlyDevice) {
  GPUManager& gm = CsvAccess(gpu_manager);
  // Coverage scales (see g_cupti_cover_end): window / the span each thread reported, from the previous close
  // to its last API record; 1 for a thread that reported the whole window. A thread that reported less than
  // half of it is capped at 2x and counted (capped=) -- a load extrapolated over more than half a window is a
  // guess, and the next close has the rest.
  g_cupti_cover_scale.clear();
  g_cupti_cover_min = 1.0; g_cupti_cover_scale_max = 1.0; g_cupti_cover_threads = 0; g_cupti_cover_capped = 0;
  const uint64_t win_prev = g_cupti_window_prev_ns, win_close = g_cupti_window_close_ns;
  if (win_prev > 0 && win_close > win_prev) {
    const double win = (double)(win_close - win_prev);
    for (const auto& kv : g_cupti_cover_end) {
      if (kv.second <= win_prev) continue;
      double cov = (double)(std::min(kv.second, win_close) - win_prev) / win;
      if (cov < 0.5) { cov = 0.5; g_cupti_cover_capped++; }
      if (cov < g_cupti_cover_min) g_cupti_cover_min = cov;
      const double sc = cov < 1.0 ? 1.0 / cov : 1.0;
      g_cupti_cover_scale[kv.first] = sc;
      if (sc > g_cupti_cover_scale_max) g_cupti_cover_scale_max = sc;
      g_cupti_cover_threads++;
    }
  }
  auto scaleOfObj = [](const LDObjKey& k) -> double {
    auto t = g_cupti_obj_thread.find(k);
    if (t == g_cupti_obj_thread.end()) return 1.0;
    auto sc = g_cupti_cover_scale.find(t->second);
    return sc == g_cupti_cover_scale.end() ? 1.0 : sc->second;
  };
  std::unordered_map<int, double> scaleOfDev;   // a device's mean thread scale, for its driver-busy union
  if (!g_cupti_cover_scale.empty()) {
    std::unordered_map<int, std::pair<double, int>> acc;
    std::lock_guard<std::mutex> tl(gm.cupti_thread_device_lock_);
    for (const auto& kv : g_cupti_cover_scale) {
      auto d = gm.cupti_thread_device_.find(kv.first);
      if (d == gm.cupti_thread_device_.end()) continue;
      acc[d->second].first += kv.second; acc[d->second].second++;
    }
    for (const auto& kv : acc) scaleOfDev[kv.first] = kv.second.first / kv.second.second;
  }
  auto devScale = [&scaleOfDev](int dev) -> double {
    auto it = scaleOfDev.find(dev);
    return it == scaleOfDev.end() ? 1.0 : it->second;
  };
  // The driver's busy time this round: the union of every runtime-API interval from the threads of a
  // device's PEs. The per-thread sums cannot be it -- eight threads waiting on one lock each count
  // the same wait -- and the union is exactly the time the serialised driver had a call in hand. A
  // process driving several devices has one driver per context, so each device-node's launch term is
  // the union of ITS PE threads' calls; the process-wide figures are the fallback and the debug print.
  {
    auto unionOf = [](std::vector<std::pair<uint64_t, uint64_t>>& iv) -> double {
      std::sort(iv.begin(), iv.end());
      uint64_t busy_ns = 0, cur_lo = 0, cur_hi = 0;
      bool open = false;
      for (const auto& s : iv) {
        if (!open || s.first > cur_hi) {
          if (open) busy_ns += cur_hi - cur_lo;
          cur_lo = s.first;
          cur_hi = s.second;
          open = true;
        } else if (s.second > cur_hi) {
          cur_hi = s.second;
        }
      }
      if (open) busy_ns += cur_hi - cur_lo;
      return (double)busy_ns / 1.0e9;
    };
    if (onlyDevice < 0) {
    std::vector<std::pair<uint64_t, uint64_t>> all;
    std::unordered_map<int, std::vector<std::pair<uint64_t, uint64_t>>> byDev;
    all.reserve(gm.cupti_api_intervals_acc_.size());
    for (const auto& s : gm.cupti_api_intervals_acc_) {
      all.emplace_back(s.start, s.end);
      byDev[s.device].emplace_back(s.start, s.end);
    }
    gm.cupti_driver_busy_dev_.clear();
    gm.cupti_driver_busy_ = 0.0;
    for (auto& kv : byDev) {
      const double b = unionOf(kv.second) * devScale(kv.first);
      gm.cupti_driver_busy_dev_[kv.first] = b;
      gm.cupti_driver_busy_ += b;
    }
    if (byDev.empty()) gm.cupti_driver_busy_ = unionOf(all);
    gm.cupti_api_intervals_acc_.clear();
    gm.cupti_api_intervals_acc_.shrink_to_fit();
    gm.cupti_api_intervals_.clear();
    // Publish this round's API attribution (scaled by each object's thread coverage) and reset the accumulators.
    gm.cupti_obj_api_raw_ = std::move(gm.cupti_obj_api_raw_acc_);
    gm.cupti_obj_api_raw_acc_.clear();
    for (auto& kv : gm.cupti_obj_api_raw_) kv.second *= scaleOfObj(kv.first);
    gm.cupti_api_total_ = gm.cupti_api_total_acc_;
    gm.cupti_api_total_acc_ = 0.0;
    gm.cupti_api_total_dev_ = std::move(gm.cupti_api_total_dev_acc_);
    gm.cupti_api_total_dev_acc_.clear();
    } else {
      // This device's close publishes ITS intervals, total and objects' API time and leaves the other
      // devices' in the accumulators for their own closes. Before this every device's close published
      // and reset the process-wide accumulators, so in a process driving four devices the first close
      // of an LB event took everything and the other three found nothing: their launch terms were zero
      // ("launch 0%" against the control's 4%, job 22856533).
      std::vector<std::pair<uint64_t, uint64_t>> mine;
      {
        auto& acc = gm.cupti_api_intervals_acc_;
        size_t w = 0;
        for (size_t i = 0; i < acc.size(); i++) {
          if (acc[i].device == onlyDevice) mine.emplace_back(acc[i].start, acc[i].end);
          else acc[w++] = acc[i];
        }
        acc.resize(w);
      }
      gm.cupti_driver_busy_dev_[onlyDevice] = unionOf(mine);
      {
        auto it = gm.cupti_api_total_dev_acc_.find(onlyDevice);
        gm.cupti_api_total_dev_[onlyDevice] = (it != gm.cupti_api_total_dev_acc_.end()) ? it->second : 0.0;
        if (it != gm.cupti_api_total_dev_acc_.end()) gm.cupti_api_total_dev_acc_.erase(it);
      }
      gm.cupti_driver_busy_ = 0.0;
      for (const auto& kv : gm.cupti_driver_busy_dev_) gm.cupti_driver_busy_ += kv.second;
      gm.cupti_api_total_ = 0.0;
      for (const auto& kv : gm.cupti_api_total_dev_) gm.cupti_api_total_ += kv.second;
      gm.cupti_api_total_acc_ = 0.0;
      for (const auto& kv : gm.cupti_api_total_dev_acc_) gm.cupti_api_total_acc_ += kv.second;
      // Objects: this device's by the last sweep (g_cupti_obj_device, read before this close clears
      // its entries below) and any object no device has claimed (host-only work, or never swept) are
      // published now, this device's previous round replaced; the other devices' stay accumulating.
      // What the other devices published stays in the map until they publish again -- a PE reads only
      // its own objects (LBDatabase::SetObjDriverLoad).
      for (auto it = gm.cupti_obj_api_raw_.begin(); it != gm.cupti_obj_api_raw_.end();) {
        auto d = g_cupti_obj_device.find(it->first);
        if (d != g_cupti_obj_device.end() && d->second == (uint32_t)onlyDevice) it = gm.cupti_obj_api_raw_.erase(it);
        else ++it;
      }
      for (auto it = gm.cupti_obj_api_raw_acc_.begin(); it != gm.cupti_obj_api_raw_acc_.end();) {
        auto d = g_cupti_obj_device.find(it->first);
        if (d == g_cupti_obj_device.end() || d->second == (uint32_t)onlyDevice) {
          gm.cupti_obj_api_raw_[it->first] = it->second;
          it = gm.cupti_obj_api_raw_acc_.erase(it);
        } else ++it;
      }
      gm.cupti_api_intervals_.clear();
    }
  }

  if (onlyDevice < 0) {
    gm.cupti_obj_norm_load_.clear();
    gm.cupti_obj_epoch_costs_.clear();
    g_cupti_obj_device.clear();
  } else {
    for (auto it = g_cupti_obj_device.begin(); it != g_cupti_obj_device.end();) {
      if (it->second == (uint32_t)onlyDevice) {
        gm.cupti_obj_norm_load_.erase(it->first);
        gm.cupti_obj_epoch_costs_.erase(it->first);
        it = g_cupti_obj_device.erase(it);
      } else ++it;
    }
  }

  const bool scaling = _lb_args.gpuScaling();

  struct SweepKernel {
    LDObjKey obj_key;
    uint64_t start_ns;
    uint64_t end_ns;
    int      sms_used;
    bool     attributed;   // false => consumes SMs but earns no load
    GpuKernelKey kernel_key;
    bool     unmodelable;
    // Whole-device occupancy this kernel earned, filled in by the sweep. Held
    // per kernel rather than summed straight into the object so the same
    // numbers can also be aggregated per kernel identity.
    double   demand;
  };
  // A record that ended before this window (the previous window's tail, delivered now) is dropped; one that
  // straddles the window's start is clipped to it. (Nothing is clipped at the close: a kernel still running at
  // the flush is reported whole when its record completes, in the next drain, and clipped there.)
  std::unordered_map<uint32_t, std::vector<SweepKernel>> byDevice;
  // Every record counts once, whole, in the drain it arrives in. A record is never clipped to the window
  // or dropped for ending before it: on HIP the dispatch record is written by ROCr's one async-signal
  // thread after the kernel completes, and that thread falls behind the device by tens of thousands of
  // records on a loaded GCD (Frontier 2026-10-23: 486,769 records, 11.5 s of kernel time per 200-step
  // leanmd run arrived after the close they ended before). Dropping them read the most loaded process at
  // a fifth to a half of its device load for the round, and the diffusion filled the hole with 1,300-2,000
  // cross-node moves and a 136 ms/step window. Counted in the next drain the time lands one round late,
  // which a steady rate does not notice; the audit still reports what arrived late (late_counted) and what
  // started before the window (straddle).
  // After a deferred close (both vendors) every record of the drain was issued before the mark and the round
  // owns it whole, straddling or not; nothing is late by construction (a record older than the previous mark
  // can only follow a bound timeout, and is counted, not dropped). Without one (a drain with tracing off, or
  // hapiTraceStop's) the original rule stands: a record that ended before this window is the previous window's
  // tail delivered late and is dropped; one that straddles the start is clipped to it.
  const bool whole = g_cupti_drain_deferred;
  size_t late_n = 0, clip_n = 0; uint64_t late_ns = 0, clip_ns = 0;
  auto noteLate = [&](const LBKernelRecord& k) { late_n++; late_ns += k.end_ns - k.start_ns; };
  auto noteClip = [&](const LBKernelRecord& k, uint64_t start) { if (start > k.start_ns && k.end_ns > start) { clip_n++; clip_ns += start - k.start_ns; } };
  g_cupti_untimed_owned = g_cupti_untimed_unowned = 0;
  for (const auto& kv : gm.cupti_obj_kernel_records_) {
    for (const LBKernelRecord& k : kv.second) {
      if (onlyDevice >= 0 && k.device_id != (uint32_t)onlyDevice) continue;
      if (k.end_ns <= k.start_ns) { g_cupti_untimed_owned++; continue; }
      if (win_prev > 0 && k.end_ns <= win_prev) { noteLate(k); if (!whole) continue; }
      const uint64_t start = (!whole && win_prev > 0 && k.start_ns < win_prev) ? win_prev : k.start_ns;
      if (win_prev > 0 && k.start_ns < win_prev) noteClip(k, win_prev);
      g_cupti_obj_device[kv.first] = k.device_id;
      byDevice[k.device_id].push_back({kv.first, start, k.end_ns, k.sms_used,
                                       true, k.kernel_key, k.unmodelable, 0.0});
    }
  }
  for (const LBKernelRecord& k : gm.cupti_unattributed_kernels_) {
    if (onlyDevice >= 0 && k.device_id != (uint32_t)onlyDevice) continue;
    if (k.end_ns <= k.start_ns) { g_cupti_untimed_unowned++; continue; }
    if (win_prev > 0 && k.end_ns <= win_prev) { noteLate(k); if (!whole) continue; }
    const uint64_t start = (!whole && win_prev > 0 && k.start_ns < win_prev) ? win_prev : k.start_ns;
    if (win_prev > 0 && k.start_ns < win_prev) noteClip(k, win_prev);
    byDevice[k.device_id].push_back({LDObjKey{}, start, k.end_ns, k.sms_used,
                                     false, k.kernel_key, true, 0.0});   // the key names it in the audit
  }

  // Per-object accumulators. Components are gathered in a hash map and only
  // flattened into the wire vector once, after the cap is applied.
  struct ObjectAccumulator {
    std::unordered_map<GpuKernelKey, GpuKernelEpochCost, GpuKernelKeyHash> components;
    double unmodeled = 0.0;
    // An object's kernels are almost always all on one device, but a migration
    // mid-round can split them. The device that did most of the work is the one
    // a destination prediction should scale from.
    double sourceDemand = 0.0;
    uint64_t sourceInstanceId = 0;
    uint64_t sourceTypeId = 0;
  };
  std::unordered_map<LDObjKey, ObjectAccumulator, LDObjKeyHash> accumulators;

  size_t total_kernels = 0, devices_normalized = 0;
  // Completeness accounting. The existing reconcile check compares the scalar
  // load against its own components, so it cannot see work that never reached
  // either -- a kernel whose object token failed to resolve is dropped from
  // both sides equally and the check still passes. These totals are what shows
  // that: device-seconds credited to an object, device-seconds consumed by
  // kernels with no owner, and the wall time the device had any work at all.
  double sweep_attr_demand = 0.0, sweep_unattr_demand = 0.0, sweep_busy_s = 0.0;
  size_t sweep_unattr_kernels = gm.cupti_unattributed_kernels_.size();
  // Each device's sweep -- the sort of its kernels' events and the proportional split of every interval --
  // is independent of the others', so a process driving several devices runs them in parallel on the parse
  // workers (serially they were 5.1 ms of the (1,4,4) close against 1.4 in the one-device control, job
  // 22868252). The per-object accumulation below stays serial.
  struct SweepJob { uint32_t device; std::vector<SweepKernel>* kernels; int total_sms; DeviceManager* dm; double busy_s; };
  std::vector<SweepJob> jobs;
  for (auto& kv : byDevice) {
    if (kv.second.empty()) continue;
    DeviceManager* dm = findDeviceManager(gm, kv.first);
    const int total_sms = (dm != nullptr) ? dm->multi_processor_count : 0;
    if (total_sms <= 0) continue;  // unknown device size -- cannot normalize
    total_kernels += kv.second.size();
    devices_normalized++;
    jobs.push_back({kv.first, &kv.second, total_sms, dm, 0.0});
  }
  auto sweepDevice = [](SweepJob& job) {
    std::vector<SweepKernel>& kernels = *job.kernels;
    const int total_sms = job.total_sms;
    double sweep_busy_s = 0.0;
    // Two events per kernel. END sorts before START on a tie so a kernel
    // ending at instant t does not briefly count alongside one starting at t.
    struct Event { uint64_t time; int kind; int kidx; };  // kind: 0=END, 1=START
    std::vector<Event> events;
    events.reserve(2 * kernels.size());
    for (int ki = 0; ki < (int)kernels.size(); ++ki) {
      events.push_back({kernels[ki].start_ns, 1, ki});
      events.push_back({kernels[ki].end_ns,   0, ki});
    }
    std::sort(events.begin(), events.end(),
              [](const Event& a, const Event& b) {
                if (a.time != b.time) return a.time < b.time;
                return a.kind < b.kind;
              });

    // Active set ordered by start_ns (then index, for stability) so that
    // iteration order is FIFO by submission.
    auto cmpActive = [&kernels](int a, int b) {
      if (kernels[a].start_ns != kernels[b].start_ns)
        return kernels[a].start_ns < kernels[b].start_ns;
      return a < b;
    };
    std::set<int, decltype(cmpActive)> active(cmpActive);

    uint64_t t_prev = events.front().time;
    for (const auto& ev : events) {
      if (ev.time > t_prev && !active.empty()) {
        double dt_s = (double)(ev.time - t_prev) / 1.0e9;

        // Split this interval's occupancy in proportion to what each active
        // kernel asked for, capping the pool at the device's SMs.
        //
        // This used to hand SMs out FIFO by submission and stop at the first
        // kernel that found the pool empty, so under oversubscription every
        // kernel behind that point earned nothing for the interval. Whether an
        // object was credited then depended on where its kernels happened to
        // land in the launch order relative to its neighbours' -- which
        // reshuffles whenever placement changes.
        //
        // That made the metric unstable in the one direction that matters: a
        // device running more objects oversubscribes harder, so more of its
        // kernels fall past the cut-off and its objects measure cheaper than
        // they are. A load balancer reading that sends the busiest node still
        // more work. Measured on pic2d, diffusing on GPU load drove per-process
        // object counts from an even 32 to a 16..39 spread, the reported load
        // swung 4x round to round, and the balancer issued 1415 migrations
        // against 250 for the host-time dimension.
        //
        // Proportional sharing is also the better model of the hardware:
        // kernels that oversubscribe an SM pool time-share it rather than
        // running strictly in submission order.
        long want = 0;
        for (int ki : active) {
          if (kernels[ki].sms_used > 0) want += kernels[ki].sms_used;
        }
        if (want > 0) {
          sweep_busy_s += dt_s;
          // Split the interval's DEVICE TIME among the kernels holding the
          // device, in proportion to the SMs each asked for. The weights decide
          // how concurrent work is divided; they must not decide how much there
          // is to divide.
          //
          // This used to charge dt * sms_used/total_sms, i.e. SM-occupancy
          // seconds, so a kernel that held the device for an interval at low
          // occupancy was recorded as almost no load. That is the right measure
          // of throughput and the wrong one for balancing: an object whose
          // kernels tie up the GPU for 10ms costs its PE 10ms whether they fill
          // 4 SMs or 80.
          //
          // The error is not uniform, which is what made it dangerous. An
          // imbalanced particle distribution gives lightly-loaded patches small
          // grids, so a node holding many of them runs many low-occupancy
          // kernels: busy the whole interval, yet reporting almost nothing.
          // Measured on pic2d, nodes that gained objects fell to util 0.055 --
          // 5.5% of their busy time counted as load -- so the balancer read them
          // as idle and sent still more work, and object counts ran away from an
          // even 32 to 1..162 across processes.
          //
          // Weighting by sms_used keeps a big kernel worth more than a small one
          // running beside it, while the interval total stays dt: summed over a
          // device, attributed demand then equals its busy time.
          //
          // The denominator (2026-09-18): the device's SM count, not the sum
          // of what the active kernels asked for, whenever that sum is
          // smaller. An interval in which one kernel holds 20 of 84 SMs is
          // then charged 20/84 of dt, the capacity it consumed, rather than
          // all of dt: a device running under-subscribed kernels has room the
          // balancer may use, and charging it as full hid that. Under
          // oversubscription the two agree: the interval is split among the
          // kernels in proportion to their demand and sums to dt. +gpuloadbusy
          // keeps the old busy-time split (the pic2d history above is why it
          // exists; that runaway was the FIFO cut-off, not the denominator).
          const double denom = hapi_gpu_load_busy
              ? (double)want
              : (double)std::max<long>(want, (long)total_sms);
          for (int ki : active) {
            const int used = kernels[ki].sms_used;
            if (used <= 0) continue;
            // Computed for unowned kernels too; they are still not credited to
            // any object below, but leaving their share uncomputed is what makes
            // such a loss invisible to the audit.
            kernels[ki].demand += dt_s * ((double)used / denom);
          }
        }
      }
      if (ev.kind == 1) active.insert(ev.kidx);
      else              active.erase(ev.kidx);
      t_prev = ev.time;
    }
    job.busy_s = sweep_busy_s;
  };
  for (size_t i = 1; i < jobs.size(); i++) hapiCuptiWorkerRun(i - 1, [&sweepDevice, &jobs, i]() { sweepDevice(jobs[i]); });
  if (!jobs.empty()) sweepDevice(jobs[0]);
  hapiCuptiWorkersWait();

  for (SweepJob& job : jobs) {
    std::vector<SweepKernel>& kernels = *job.kernels;
    DeviceManager* dm = job.dm;
    const int total_sms = job.total_sms;
    sweep_busy_s += job.busy_s;

    for (const SweepKernel& k : kernels) {
      if (k.demand <= 0.0) continue;
      if (!k.attributed) { sweep_unattr_demand += k.demand; continue; }
      sweep_attr_demand += k.demand;
      gm.cupti_obj_norm_load_[k.obj_key] += k.demand * scaleOfObj(k.obj_key);
    }

    // CHARM_GPU_LOAD_OBJDUMP: one line per object this device charged. Beside
    // the sweep's demand, what the same kernels would earn in isolation --
    // duration x SMs used / SMs -- so a benchmark that knows its kernels'
    // SM counts (benchmarks/.../smload) can check the model kernel by kernel.
    // The two agree while the device is not oversubscribed.
    static const bool objdump = (getenv("CHARM_GPU_LOAD_OBJDUMP") != nullptr);
    if (objdump) {
      struct Tally { int kernels = 0; double elapsed = 0.0, isolated = 0.0, demand = 0.0; };
      std::unordered_map<LDObjKey, Tally, LDObjKeyHash> tallies;
      for (const SweepKernel& k : kernels) {
        if (!k.attributed) continue;
        Tally& t = tallies[k.obj_key];
        const double d = (k.end_ns > k.start_ns) ? (double)(k.end_ns - k.start_ns) / 1.0e9 : 0.0;
        t.kernels++;
        t.elapsed += d;
        t.isolated += d * (double)k.sms_used / (double)total_sms;
        t.demand += k.demand;
      }
      for (const auto& tv : tallies)
        CmiPrintf("[gpu-objdump pe=%d dev=%u om=%d obj=%llu kernels=%d elapsed_s=%.6f "
                  "isolated_s=%.6f demand_s=%.6f]\n",
                  CmiMyPe(), (unsigned)job.device, (int)tv.first.omID().id.idx,
                  (unsigned long long)tv.first.objID(), tv.second.kernels,
                  tv.second.elapsed, tv.second.isolated, tv.second.demand);
    }

    if (!scaling) continue;

    const uint64_t instanceId = dm->descriptor.instanceId;
    const uint64_t typeId = dm->descriptor.typeId;
    std::unordered_map<LDObjKey, double, LDObjKeyHash> deviceDemand;
    for (const SweepKernel& k : kernels) {
      if (!k.attributed || k.demand <= 0.0) continue;
      deviceDemand[k.obj_key] += k.demand;

      ObjectAccumulator& acc = accumulators[k.obj_key];
      if (k.unmodelable) {
        acc.unmodeled += k.demand;
        continue;
      }
      GpuKernelEpochCost& component = acc.components[k.kernel_key];
      component.key = k.kernel_key;
      const double duration_s = (double)(k.end_ns - k.start_ns) / 1.0e9;
      // A rejected sample still has to be accounted for somewhere, or the
      // per-object total stops reconciling with the scalar load.
      if (!component.observe(k.demand, duration_s)) acc.unmodeled += k.demand;
    }
    for (const auto& objectDemand : deviceDemand) {
      ObjectAccumulator& acc = accumulators[objectDemand.first];
      if (objectDemand.second > acc.sourceDemand) {
        acc.sourceDemand = objectDemand.second;
        acc.sourceInstanceId = instanceId;
        acc.sourceTypeId = typeId;
      }
    }
  }

  if (scaling) {
    const std::size_t cap =
        static_cast<std::size_t>(_lb_args.gpuScalingMaxComponents());
    size_t capped_objects = 0;
    for (auto& kv : accumulators) {
      GpuObjectEpochCosts costs;
      costs.sourceInstanceId = kv.second.sourceInstanceId;
      costs.sourceTypeId = kv.second.sourceTypeId;
      costs.unmodeledGpuTime = kv.second.unmodeled;
      costs.components.reserve(kv.second.components.size());
      for (const auto& component : kv.second.components)
        costs.components.push_back(component.second);

      if (costs.components.size() > cap) capped_objects++;
      costs.enforceComponentCap(cap);
      // Canonical wire order, independent of hash iteration and of whether the
      // cap reordered anything, so every replica sees the same bytes.
      std::sort(costs.components.begin(), costs.components.end(),
                [](const GpuKernelEpochCost& left, const GpuKernelEpochCost& right) {
                  return left.key < right.key;
                });

      if (_lb_args.debug() > 0) {
        auto scalar = gm.cupti_obj_norm_load_.find(kv.first);
        const double expected =
            scalar == gm.cupti_obj_norm_load_.end() ? 0.0 : scalar->second;
        const double tolerance = 1.0e-9 + 1.0e-6 * expected;
        if (std::fabs(expected - costs.totalDemand()) > tolerance) {
          CmiPrintf("HAPI[pe=%d]: GPU summary does not reconcile: scalar=%.9f "
                    "components+residual=%.9f\n",
                    CmiMyPe(), expected, costs.totalDemand());
        }
      }

      gm.cupti_obj_epoch_costs_[kv.first] = std::move(costs);
    }

    if (_lb_args.debug() > 1) {
      CmiPrintf("HAPI[pe=%d]: GPU summaries objects=%zu capped=%zu cap=%zu\n",
                CmiMyPe(), gm.cupti_obj_epoch_costs_.size(), capped_objects, cap);
    }
  }

  if (_lb_args.debug() > 1) {
    CmiPrintf("HAPI[pe=%d]: hapiNormalizeCuptiLoads  %zu kernels across %zu "
              "device(s) -> %zu objects\n",
              CmiMyPe(), total_kernels, devices_normalized,
              gm.cupti_obj_norm_load_.size());
  }

  // Where the device's work went. attributed+unowned is every device-second the
  // sweep saw; the unowned share is work that no object is charged for, so it
  // vanishes from the balancer's view of the node.
  if (getenv("CHARM_GPU_LOAD_AUDIT") != nullptr) {
    const double seen = sweep_attr_demand + sweep_unattr_demand;
    // Wall time this window actually covered. busy_s alone cannot say whether a
    // round measured more work or simply measured for longer, and those call
    // for opposite responses.
    static double s_last_audit_t = 0.0;
    const double now_t = CmiWallTimer();
    const double window_s = (s_last_audit_t > 0.0) ? (now_t - s_last_audit_t) : 0.0;
    s_last_audit_t = now_t;
    CmiPrintf("[gpu-audit pe=%d] kernels=%zu unowned_kernels=%zu objects=%zu "
              "attributed_s=%.6f unowned_s=%.6f unowned_frac=%.3f busy_s=%.6f "
              "util=%.3f window_s=%.6f occ=%.4f\n",
              CmiMyPe(), total_kernels, sweep_unattr_kernels,
              gm.cupti_obj_norm_load_.size(), sweep_attr_demand,
              sweep_unattr_demand, (seen > 0.0) ? sweep_unattr_demand / seen : 0.0,
              sweep_busy_s, (sweep_busy_s > 0.0) ? seen / sweep_busy_s : 0.0,
              window_s, (window_s > 0.0) ? sweep_busy_s / window_s : 0.0);
    {   // the records the window could not use, and the copies (on HIP the blit kernels) the sweep never sees
      static uint64_t s_last_copy_n = 0, s_last_copy_ns = 0;
      const uint64_t cn = g_cupti_memcpy_n.load(std::memory_order_relaxed);
      const uint64_t cns = g_cupti_memcpy_ns.load(std::memory_order_relaxed);
      CmiPrintf("[gpu-audit pe=%d] window_rec_s=%.6f late=%zu/%.6fs straddle=%zu/%.6fs copies=%llu/%.6fs\n",
                CmiMyPe(), (win_prev > 0 && win_close > win_prev) ? (double)(win_close - win_prev) / 1.0e9 : 0.0,
                late_n, (double)late_ns / 1.0e9, clip_n, (double)clip_ns / 1.0e9,
                (unsigned long long)(cn - s_last_copy_n), (double)(cns - s_last_copy_ns) / 1.0e9);
      s_last_copy_n = cn; s_last_copy_ns = cns;
    }
    CmiPrintf("[driver-audit pe=%d] api_total_s=%.6f driver_busy_s=%.6f "
              "contention=%.2f window_s=%.6f driver_util=%.3f\n",
              CmiMyPe(), gm.cupti_api_total_, gm.cupti_driver_busy_,
              (gm.cupti_driver_busy_ > 0.0) ? gm.cupti_api_total_ / gm.cupti_driver_busy_ : 0.0,
              window_s, (window_s > 0.0) ? gm.cupti_driver_busy_ / window_s : 0.0);
    // The unowned work by kernel, named under +LBDebug 1: the runtime's own kernels, or an
    // attribution that lost the launch (its API record, thread or span).
    std::unordered_map<uint64_t, double> unowned_by_class;
    for (SweepJob& job : jobs)
      for (const SweepKernel& k : *job.kernels)
        if (!k.attributed && k.demand > 0.0) unowned_by_class[k.kernel_key.kernelClass] += k.demand;
    std::vector<std::pair<double, uint64_t>> top;
    for (auto& kv : unowned_by_class) top.push_back({kv.second, kv.first});
    std::sort(top.rbegin(), top.rend());
    for (size_t i = 0; i < top.size() && i < 6; i++) {
      auto it = gm.cupti_kernel_names_.find(top[i].second);
      CmiPrintf("[gpu-audit pe=%d]   unowned %.6f s  %s\n", CmiMyPe(), top[i].first,
                it != gm.cupti_kernel_names_.end() ? it->second.c_str() : "(class unnamed)");
    }
    fflush(stdout);
  }
}

// Build this round's per-object GPU loads, exactly once per round however many
// PE threads call in. Every PE needs cupti_obj_norm_load_ populated before it
// reads its own objects out of it, and the work itself must be done by one
// thread; holding the lock across both gives the readers their ordering without
// a barrier that every PE has to reach.
// The window close is per PROCESS: the last of its PEs to arrive flushes once, parses once and builds every
// device's loads. It was per device group (the PEs of one device) from 2026-10-12 to 10-13 on the premise that
// cuptiActivityFlush(context) flushes one context; it is deprecated and flushes the process (cupti_activity.h),
// so a four-device process paid four process-wide forced flushes per LB event (12-17 ms each against 1-6 ms in
// a one-device process, job 22866653) and three closers waited for the first one's parse. The balancers' arrival
// count and gpuLoadsReady fan-out use these two.
int hapiDeviceGroupSize() { return CmiMyNodeSize(); }
int hapiDeviceGroupFirstPe() { return CmiNodeFirst(CmiMyNode()); }
int hapiMyDeviceIndex() { return CpvAccess(my_device_id); }
struct CuptiArrival { uint64_t epoch = 0; int count = 0; };
static CuptiArrival g_cupti_arrival;   // the process's, under cupti_arrive_lock_
bool hapiCuptiArrive(uint64_t epoch, int expected) {
  GPUManager& gm = CsvAccess(gpu_manager);
  if (CuptiEntryLog* L = t_cupti_entry_log) {   // this PE's entry-span log: drop what no later drain can ask about
    hapiCuptiCalibrateSpanClock(false);
    const uint64_t now = hapiCuptiSpanNow();
    std::lock_guard<std::mutex> g(L->m);
    const uint64_t cutoff = L->narrivals >= 2 ? L->arrivals[(L->narrivals - 2) % 3] : 0;
    L->arrivals[L->narrivals % 3] = now;
    L->narrivals++;
    if (cutoff != 0) {
      hapiCuptiLanePrune(L->obj, cutoff);
      hapiCuptiLanePrune(L->tag, cutoff);
    }
    if (hapiCuptiTimeIt() && (L->narrivals == 2 || L->narrivals % 10 == 0))
      CmiPrintf("[LBCUPTI pe=%d] log: arrival %d spans=%zu open=%zu bottom_open=%u pruned=%llu gen=%u\n", CmiMyPe(),
                L->narrivals, L->obj.spans.size(), L->obj.open.size(), L->obj.open.empty() ? 0u : L->obj.open.front(),
                (unsigned long long)L->obj.pruned, L->obj.gen);
  }
  std::lock_guard<std::mutex> lk(gm.cupti_arrive_lock_);
  CuptiArrival& a = g_cupti_arrival;
  // A new round resets the count. Rounds are strictly sequential -- a step
  // completes on a job-wide barrier before the next can start -- so a stale
  // count from a previous epoch can only mean that epoch is over.
  if (a.epoch != epoch) {
    a.epoch = epoch;
    a.count = 0;
  }
  a.count++;
#if CMK_CUDA
  // The process's first PE at this LB step: CUPTI gets small buffers from here until the close completes, so the
  // buffer open at the mark (set by the last PE) holds few launches (see kCuptiMarkBufBytes).
  if (a.count == 1 && expected > 1 && hapiCuptiTracingActive()) {
    g_cupti_first_arrival_t = CmiWallTimer();
    g_cupti_small_bufs.store(true);
  }
#endif
  if (a.count < expected) return false;
  a.count = 0;
  return true;
}

// Per device: the ready flag and epoch of each device group's last build.
static bool g_cupti_dev_ready[HAPI_CUPTI_MAX_GATES] = {};
static uint64_t g_cupti_dev_epoch[HAPI_CUPTI_MAX_GATES] = {};
static void hapiPrepareCuptiLoadsImpl(uint64_t epoch, int dev);
void hapiPrepareCuptiLoads(uint64_t epoch) { hapiPrepareCuptiLoadsImpl(epoch, -1); }
// The process's window close, whichever device the arriving PE drives (see hapiDeviceGroupSize): one flush,
// one parse, every device's loads. The per-device path of hapiPrepareCuptiLoadsImpl (dev >= 0: parse, then
// normalise and drop one device's records) stays for a CUPTI that flushes one context.
void hapiPrepareCuptiLoadsDevice(uint64_t epoch, int dev) {
  (void)dev;
  hapiPrepareCuptiLoadsImpl(epoch, -1);
}
// Already built for this epoch or a later one? A sampler that is behind its process-mates reads
// their fresher loads rather than paying for a rebuild; an LB round always wins this comparison
// and rebuilds exactly once. Caller holds cupti_prepare_lock_.
static bool hapiCuptiLoadsBuilt(GPUManager& gm, uint64_t epoch, int dev) {
  if (dev < 0) return gm.cupti_loads_ready_ && epoch <= gm.cupti_loads_epoch_;
  return g_cupti_dev_ready[dev] && epoch <= g_cupti_dev_epoch[dev];
}

static void hapiPrepareCuptiLoadsImpl(uint64_t epoch, int dev) {
  GPUManager& gm = CsvAccess(gpu_manager);
  {
    std::lock_guard<std::mutex> lk(gm.cupti_prepare_lock_);
    if (hapiCuptiLoadsBuilt(gm, epoch, dev)) return;
  }
  // The flush runs OUTSIDE cupti_prepare_lock_: the forced flush of a device's context waits for
  // that device's queued kernels (13-43 ms under +LBAsync, sph3d two nodes, job 22853463), and the
  // device groups of one process close within milliseconds of each other on their own helper lanes.
  // Serialised under the lock, four closes took 4 x (flush + parse) ~ 95 ms per LB event against ~20
  // in a one-device process, the whole of the (1,4,4) layout's remaining async penalty (the lag
  // window ran 0.441 s against 0.373). The four flushes then waited concurrently and the parses
  // serialised -- but cuptiActivityFlush(context) is deprecated and flushes the PROCESS, so those were four
  // process-wide forced flushes of 12-17 ms each in a 16-PE process (1-6 ms in a 4-PE one), the first closer
  // parsed everything and the other three waited 11-22 ms for it (job 22866653). Since 2026-10-13 the close is
  // one per process (hapiDeviceGroupSize): one flush, one parse, every device's loads.

  const bool timeIt = (getenv("CHARM_LB_CUPTI_TIME") != nullptr);
  const double t0 = timeIt ? CmiWallTimer() : 0.0;
  // Only flush while CUPTI is attached: an application driving its own
  // instrumentation window may already have switched tracing off, and its stop
  // path flushed on the way out, so there is nothing left to pull.
  // The flush is the forced one by default (HAPI_CUPTI_FLUSH_FORCED 1, 2026-10-22): it returns every buffer
  // including those with incomplete records, which CUPTI still writes later from another thread's CUDA call: hence
  // the quarantine (hapiCuptiRetireBuffer) and the gates. The non-forced flush (0) hands over the buffers whose
  // records are complete, as CUPTI's own worker thread does concurrently with launches, so no gate is taken and no
  // launch waits; the open buffer per thread (64 KB, cuptiBufferRequested) is then the window's unreported tail,
  // which hapiNormalizeCuptiLoads extrapolates per thread from the window stamped below -- over the process window,
  // not the element's, which is why it is no longer the default (see the define).
  {   // this window ends here, in the CUPTI clock (the records' clock) -- at the mark when the close was deferred
    const uint64_t now = g_cupti_mark_set ? g_cupti_mark_ns : hapiTraceBackendTimestamp();
    g_cupti_window_prev_ns = g_cupti_window_close_ns;
    g_cupti_window_close_ns = now;
  }
  // After a CUDA deferred close the buffers holding the window are back already: no flush, so no launch waits.
  if (hapiCuptiTracingActive() && !(g_cupti_mark_set && g_cupti_close_flushless))
    hapiCuptiFlushGated(HAPI_CUPTI_FLUSH_FORCED != 0);
  g_cupti_close_flushless = false;
  const double t1 = timeIt ? CmiWallTimer() : 0.0;
  std::unique_lock<std::mutex> lk(gm.cupti_prepare_lock_);
  const double t1b = timeIt ? CmiWallTimer() : 0.0;   // the wait on the lock (the other device closers' parses)
  if (hapiCuptiLoadsBuilt(gm, epoch, dev)) return;   // built meanwhile (the process-wide path)
  hapiProcessCuptiBuffers();
  const double t2 = timeIt ? CmiWallTimer() : 0.0;
  hapiNormalizeCuptiLoads(dev < 0 ? -1 : gm.device_managers[dev].global_index);
  // Close the measurement window exactly where it was read. These records were
  // just consumed; anything recorded from here on belongs to the next round.
  //
  // They used to be dropped at MigrationDone instead, which is after the
  // strategy has run and the migrations have executed. Kernels running through
  // all of that were recorded and then discarded, belonging to no round at
  // all, and the amount discarded depended on how long that round's LB step
  // took -- so the window was "the interval minus a variable tail" rather than
  // the interval. That is a feedback loop between migration count and measured
  // load, and it is what made the GPU load dimension swing round to round even
  // after per-kernel attribution was fixed.
  if (dev < 0) {
    gm.cupti_obj_kernel_records_.clear();
    gm.cupti_unattributed_kernels_.clear();
  } else {
    // This device's records only; the others' belong to their own close.
    const uint32_t gidx = (uint32_t)gm.device_managers[dev].global_index;
    for (auto it = gm.cupti_obj_kernel_records_.begin(); it != gm.cupti_obj_kernel_records_.end();) {
      auto& v = it->second;
      v.erase(std::remove_if(v.begin(), v.end(), [gidx](const LBKernelRecord& k) { return k.device_id == gidx; }), v.end());
      if (v.empty()) it = gm.cupti_obj_kernel_records_.erase(it); else ++it;
    }
    auto& u = gm.cupti_unattributed_kernels_;
    u.erase(std::remove_if(u.begin(), u.end(), [gidx](const LBKernelRecord& k) { return k.device_id == gidx; }), u.end());
  }
  if (timeIt) {
    const double t3 = CmiWallTimer();
    CmiPrintf("[LBCUPTI pe=%d] close wait=%.1fms polls=%u launches=%llu kernels_pre=%llu arrived_in_wait=%llu "
              "unmatched launches=%zu kernels=%zu%s\n",
              CmiMyPe(), g_cupti_close.waited_ms, g_cupti_close.polls, (unsigned long long)g_cupti_close.launches,
              (unsigned long long)g_cupti_close.kernels_pre, (unsigned long long)g_cupti_close.arrived_in_wait,
              g_cupti_close.pending_at_end, g_cupti_close.unmatched_at_end, g_cupti_close.timed_out ? " TIMED OUT" : "");
    if (g_cupti_close.ended_by[0] != '\0')   // CUDA: the buffers waited for
      CmiPrintf("[LBCUPTI pe=%d] close buffers held at the mark=%zu (%zu KB, largest %zu KB; small buffers since the first arrival %.0f ms before the mark, %llu handed so far), still out at the end=%zu; default flushes asked %u, "
                "done by the flusher thread %llu (%.1f ms); ended by %s; kernel records without timestamps: owned %zu unowned %zu; launched before "
                "the mark, started after it: %zu\n",
                CmiMyPe(), g_cupti_close.bufs_at_mark, g_cupti_close.mark_bytes_sum >> 10, g_cupti_close.mark_bytes_max >> 10,
                g_cupti_close.small_ms_before_mark, g_cupti_close.small_handed, g_cupti_close.bufs_at_end, g_cupti_close.flushes,
                (unsigned long long)g_cupti_close.flusher_calls, g_cupti_close.flush_ms, g_cupti_close.ended_by,
                g_cupti_untimed_owned, g_cupti_untimed_unowned, g_cupti_kept_started_after);
    CmiPrintf("[LBCUPTI pe=%d] flush=%.3fs process=%.3fs normalize=%.3fs total=%.3fs shards=%zu free=%zu"
              " wait=%.3fs retire=%.3fs pass1=%.3fs pass2=%.3fs merge=%.3fs snap=%.3fs/%lluKB bufs=%zu/%lluKB"
              " gate=%.3fs call=%.3fs cbs=%u/%lluKB cover=%.2f scale=%.2f threads=%zu capped=%zu\n",
              CmiMyPe(), t1 - t0, t2 - t1, t3 - t2, t3 - t0, g_cupti_shards.size(), g_cupti_free.size(),
              t1b - t1, g_cupti_drain_prof[0], g_cupti_drain_prof[1], g_cupti_drain_prof[2], g_cupti_drain_prof[3],
              (double)g_cupti_snap_ns.load(std::memory_order_relaxed) / 1.0e9,
              (unsigned long long)(g_cupti_snap_bytes.load(std::memory_order_relaxed) >> 10),
              g_cupti_drain_bufs, (unsigned long long)g_cupti_drain_valid_kb,
              g_cupti_flush_prof.gate_s, g_cupti_flush_prof.call_s, g_cupti_flush_prof.cbs,
              (unsigned long long)(g_cupti_flush_prof.cb_bytes >> 10), g_cupti_cover_min, g_cupti_cover_scale_max,
              g_cupti_cover_threads, g_cupti_cover_capped);
    for (const auto& kv : gm.cupti_api_total_dev_) {
      auto b = gm.cupti_driver_busy_dev_.find(kv.first);
      CmiPrintf("[LBCUPTI pe=%d]   device %d: api total %.4fs busy %.4fs\n", CmiMyPe(), kv.first, kv.second,
                b != gm.cupti_driver_busy_dev_.end() ? b->second : 0.0);
    }
    {   // this drain's record counts
      uint64_t apiN = 0, kerN = 0;
      for (const auto& sh : g_cupti_shards) { apiN += sh.api_count; kerN += sh.kernel_count; }
      CmiPrintf("[LBCUPTI pe=%d]   records api=%llu kernels=%llu\n", CmiMyPe(), (unsigned long long)apiN,
                (unsigned long long)kerN);
    }
    fflush(stdout);
  }

  if (dev < 0) {
    gm.cupti_loads_ready_ = true;
    gm.cupti_loads_epoch_ = epoch;
  } else {
    g_cupti_dev_ready[dev] = true;
    g_cupti_dev_epoch[dev] = epoch;
  }
}

// The process's round is over: its loads may be rebuilt next round. The close is per process (see
// hapiDeviceGroupSize); the device argument names the caller's device and is not used.
void hapiClearCuptiDataDevice(int dev) {
  (void)dev;
  hapiClearCuptiData();
}
void hapiClearCuptiData() {
  GPUManager& gm = CsvAccess(gpu_manager);
  // Same lock as hapiPrepareCuptiLoads: this drops the maps that function
  // builds and that every PE reads, so it must not run underneath either.
  std::lock_guard<std::mutex> lk(gm.cupti_prepare_lock_);
  gm.cupti_loads_ready_ = false;
  gm.cupti_loads_epoch_ = 0;
  for (int d = 0; d < HAPI_CUPTI_MAX_GATES; d++) { g_cupti_dev_ready[d] = false; g_cupti_dev_epoch[d] = 0; }
  g_cupti_obj_device.clear();

  gm.cupti_obj_kernel_records_.clear();
  gm.cupti_unattributed_kernels_.clear();
  gm.cupti_obj_norm_load_.clear();
  gm.cupti_obj_epoch_costs_.clear();
  gm.cupti_obj_api_raw_.clear();
  gm.cupti_api_intervals_.clear();
  gm.cupti_api_total_ = 0.0;
  gm.cupti_driver_busy_ = 0.0;
  // The shards are NOT dropped here. They carry two things across drains that a round boundary must not
  // lose: the correlation table of the previous drain (prev_object_corr, prev_work_tag_corr) and the kernels
  // parked for their launch record (carried). A kernel enqueued before the close and completed after it has
  // its launch record in drain N and its dispatch record in drain N+1 -- the device backlog of the most loaded
  // GCD at the close, 20-50k kernels per close on Frontier (2026-10-23, leanmd 16x8x8, two nodes) -- and
  // dropping the shards at MigrationDone emptied both: every such kernel was parked once more and then
  // discarded, the heaviest process attributed 2-10% of its kernels against the others' 20%, read a third
  // of its load, and the diffusion filled the hole (1,890 cross-node moves, 136 ms/step for the window).
  // Each drain resets its per-drain state at pass 1 and the merge empties what it took.
  gm.cupti_obj_api_raw_acc_.clear();
  gm.cupti_api_total_acc_ = 0.0;
  gm.cupti_api_intervals_acc_.clear();
  gm.cupti_api_total_dev_acc_.clear();
  gm.cupti_driver_busy_dev_.clear();
  gm.cupti_api_total_dev_.clear();
  // Correlation maps are drained alongside the CUPTI buffers in
  // hapiProcessCuptiBuffers. Do not clear the object-token table: later epochs
  // must reuse the same token for the same full LB identity.
}

// The factor that turns an object's raw runtime-API time into its share of the serialised driver:
// the union of this PE's device's API intervals over the summed API time of that device's threads.
// A process can drive several devices; each device-node's launch term is its own driver's. Falls
// back to the process-wide ratio when the device has no record this round.
double hapiCuptiDriverScale() {
  GPUManager& gm = CsvAccess(gpu_manager);
  std::lock_guard<std::mutex> lk(gm.cupti_prepare_lock_);
  const int dev = CpvAccess(my_device);
  auto t = gm.cupti_api_total_dev_.find(dev);
  auto b = gm.cupti_driver_busy_dev_.find(dev);
  if (t != gm.cupti_api_total_dev_.end() && b != gm.cupti_driver_busy_dev_.end() && t->second > 0.0)
    return b->second / t->second;
  return gm.cupti_api_total_ > 0.0 ? gm.cupti_driver_busy_ / gm.cupti_api_total_ : 0.0;
}

#endif


// Initialize per-PE variables
static void hapiInitCpv() {
  // HAPI event-related
#ifndef HAPI_CUDA_CALLBACK
  CpvInitialize(hapiEventQueues, hapi_event_queue);
  CpvInitialize(HapiEventPools, hapi_event_pool);
  // for(int i = 0; i < 8; i++) {
  //   hapiEvent_t ev;
  //   hapiEventCreateWithFlags(&ev, hapiEventDisableTiming);
  //   CpvAccess(hapi_event_pool).push(ev);
  // }
#endif
  CpvInitialize(int, n_hapi_events);
  CpvAccess(n_hapi_events) = 0;

  CpvInitialize(uint32_t*, hapi_flag_slots);
  CpvInitialize(void*, hapi_flag_slots_dev);
  CpvInitialize(uint32_t, hapi_flag_seq);
  CpvInitialize(int, hapi_flag_inflight);
  CpvInitialize(unsigned char*, hapi_flag_busy);
  CpvAccess(hapi_flag_slots) = NULL;
  CpvAccess(hapi_flag_slots_dev) = NULL;
  CpvAccess(hapi_flag_seq) = 0;
  CpvAccess(hapi_flag_inflight) = 0;
  CpvAccess(hapi_flag_busy) = NULL;
  // The ring itself is allocated in hapiMapping, once the PE has its device.

  // Device mapping
  CpvInitialize(int, my_device);
  CpvInitialize(int, my_device_id);
  CpvAccess(my_device_id) = 0;
  CpvAccess(my_device) = 0;
  CpvInitialize(bool, device_rep);
  CpvAccess(device_rep) = false;
}

// Clean up per-process data
static void hapiExitCsv() {
  GPUManager& csv_gpu_manager = CsvAccess(gpu_manager);

  // Destroy GPU Manager object
  csv_gpu_manager.destroy();

  // Release memory pool
  if (csv_gpu_manager.mempool_initialized_) {
    releasePool(csv_gpu_manager.mempool_free_bufs_);
  }
#ifndef HAPI_CUDA_CALLBACK
  for (auto& kv : CpvAccess(hapi_event_pool)) {
    while (!kv.second.empty()) {
      hapiEventDestroy(kv.second.front());
      kv.second.pop();
    }
  }
#endif
}

// Set up PE to GPU mapping, invoked from all PEs
// TODO: Support custom mappings
static void hapiMapping(char** argv) {
  GPUManager& csv_gpu_manager = CsvAccess(gpu_manager);
  // Default: round robin with one device per process (unchanged), block as soon as a process
  // drives several devices, so each device owns a contiguous run of PEs (what the array map and
  // DiffusionLB's node addressing assume). +gpumap overrides either way.
  Mapping map_type = Mapping::RoundRobin;
  bool map_explicit = false;
  char* gpumap = NULL;

  // Process +gpumap
  if (CmiGetArgStringDesc(argv, "+gpumap", &gpumap,
        "define pe to gpu device mapping")) {
    map_explicit = true;
    if (CmiMyPe() == 0) {
      CmiPrintf("HAPI> PE-GPU mapping: %s\n", gpumap);
    }

    if (strcmp(gpumap, "none") == 0) {
      map_type = Mapping::None;
    } else if (strcmp(gpumap, "block") == 0) {
      map_type = Mapping::Block;
    } else if (strcmp(gpumap, "roundrobin") == 0) {
      map_type = Mapping::RoundRobin;
    } else {
      CmiAbort("Unsupported mapping type: %s, use one of \"none\", \"block\", "
          "\"roundrobin\"", gpumap);
    }
  }

  // +gpudevs G: devices per process, instead of deriving it as visible devices / processes on
  // the host. Every process on a host must agree (the shared-memory IPC layout is indexed by it).
  int want_devs = 0;
  const bool have_devs = CmiGetArgIntDesc(argv, "+gpudevs", &want_devs,
        "GPU devices per process (default: visible devices / processes per host)");

  // Rank 0 alone publishes the map type: it may switch it to block below, and a slower PE writing
  // its own parse here afterwards would undo that before the barrier.
  if (CmiMyRank() == 0) csv_gpu_manager.map_type = map_type;

  if (map_type == Mapping::None) {
    if (CmiMyPe() == 0) {
      CmiPrintf("HAPI> User should explicitly select devices for PEs/chares\n");
    }
    return;
  }

  CmiAssert(map_type != Mapping::None);

  if (CmiMyRank() == 0) {
    // Count number of GPU devices used by each process
    int visible_device_count;
    hapiCheck(hapiGetDeviceCount(&visible_device_count));
    if (visible_device_count <= 0) {
      CmiAbort("Unable to perform PE-GPU mapping, no GPUs found!");
    }

    const int procs_per_host = CmiNumNodes() / CmiNumPhysicalNodes();
    const int node_size = CmiNodeSize(CmiMyNode());
    int& device_count = csv_gpu_manager.device_count;
    device_count = have_devs ? want_devs : visible_device_count / procs_per_host;
    if (have_devs && (want_devs < 1 || want_devs > visible_device_count)) {
      CmiAbort("HAPI> +gpudevs %d: this host has %d visible device(s)", want_devs,
               visible_device_count);
    }

    // Handle the case where the number of GPUs per process are larger than
    // the number of PEs per process. This is needed because we currently don't
    // support each PE using more than one device.
    if (device_count > node_size) {
      if (CmiMyPe() == 0) {
        CmiPrintf("HAPI> Found more GPU devices (%d) than PEs (%d) per process, "
            "limiting to %d device(s) per process\n", device_count, node_size, node_size);
      }
      device_count = node_size;
    }

    // More processes on the host than devices: device_count comes out 0, and the processes
    // share the devices (the modulo below).
    if (device_count == 0) {
      device_count = 1;
    }

    // Every device gets the same number of PEs. An uneven split would leave the IPC event pool
    // (sized per device as pool_per_pe * pes_per_device) short for the larger groups.
    if (node_size % device_count != 0) {
      CmiAbort("HAPI> %d PE(s) per process over %d device(s): the PE count per process must be "
               "a multiple of the device count (+ppn, +gpudevs)", node_size, device_count);
    }
    csv_gpu_manager.pes_per_device = node_size / device_count;

    // Count number of devices on a physical node
    csv_gpu_manager.device_count_on_physical_node = visible_device_count;

    if (!map_explicit && device_count > 1) csv_gpu_manager.map_type = Mapping::Block;

    // The devices this process drives: a contiguous run, device_count * local rank onward, the
    // same for both PE rules. (The old Block formula, (rank * visible + i) / processes_per_host,
    // collapsed onto one device whenever both the process count and the device count were above
    // one.) The modulo lets more processes than devices share them.
    std::vector<DeviceManager>& device_managers = csv_gpu_manager.device_managers;
    for (int i = 0; i < device_count; i++) {
      device_managers.emplace_back(i, (device_count * CmiMyNodeRankLocal() + i) % visible_device_count);
    }
  }

  CmiNodeBarrier();
  map_type = csv_gpu_manager.map_type;

  if (CmiMyPe() == 0) {
    const char* conn = getenv(HAPI_HW_QUEUES_ENV);
    CmiPrintf("HAPI> Config: %d process(es) per host, %d device(s) per process, %d PE(s) per device, "
        "%d device(s) per host, PE-device map %s, %s hardware queue(s) per device (%s)\n",
        CmiNumNodes() / CmiNumPhysicalNodes(), csv_gpu_manager.device_count,
        csv_gpu_manager.pes_per_device, csv_gpu_manager.device_count_on_physical_node,
        map_type == Mapping::Block ? "block" : "roundrobin",
        conn != nullptr ? conn : HAPI_HW_QUEUES_DRIVER_DEFAULT,
        conn == nullptr ? "driver default, the runtime did not run hapiPresetEnvironment"
                        : hapi_connections_from_env ? HAPI_HW_QUEUES_ENV " from the environment"
                                                    : "runtime default");
  }
  if (CmiMyRank() == 0 && csv_gpu_manager.device_count > 1) {
    const std::vector<DeviceManager>& dms = csv_gpu_manager.device_managers;
    CmiPrintf("HAPI> [%d] process %d drives devices %d-%d\n", CmiMyPe(), CmiMyNode(),
              dms.front().global_index, dms.back().global_index);
  }

  // Perform mapping and set device representative PE. The representative is the first PE of
  // each device: it creates the device's arena, IPC handle, comm buffer and peer access.
  int my_rank = CmiMyRank();
  int& cpv_my_device = CpvAccess(my_device);
  int& cpv_my_device_id = CpvAccess(my_device_id);
  bool& cpv_device_rep = CpvAccess(device_rep);

  switch (map_type) {
    case Mapping::Block: {
      // PEs 0..K-1 on device 0, K..2K-1 on device 1, ...
      cpv_my_device_id   = my_rank / csv_gpu_manager.pes_per_device;
      cpv_my_device      = csv_gpu_manager.device_managers[cpv_my_device_id].global_index;
      cpv_device_rep     = (my_rank % csv_gpu_manager.pes_per_device == 0);
      firstRankForDevice = cpv_my_device;
    }
      break;
    case Mapping::RoundRobin: {
      // PE r on device r % G: the first G ranks are one per device.
      cpv_my_device_id   = my_rank % csv_gpu_manager.device_count;
      cpv_my_device      = csv_gpu_manager.device_managers[cpv_my_device_id].global_index;
      cpv_device_rep     = (my_rank < csv_gpu_manager.device_count);
      firstRankForDevice = cpv_my_device;
    }
      break;
    default:
      CmiAbort("Unsupported mapping type!");
  }

  hapiCheck(hapiSetDevice(cpv_my_device));
#ifdef CMK_LBDB_ON
  {
    // This PE thread's OS id: the CUPTI runtime-API records name the thread, and its API time
    // belongs to this device's driver share (hapiNormalizeCuptiLoads).
    std::lock_guard<std::mutex> lk(csv_gpu_manager.cupti_thread_device_lock_);
    csv_gpu_manager.cupti_thread_device_[(uint32_t)syscall(SYS_gettid)] = cpv_my_device;
  }
  // This PE's flush gate (its device's).
  if (cpv_my_device_id >= 0 && cpv_my_device_id < HAPI_CUPTI_MAX_GATES) {
    t_cupti_gate_dev = cpv_my_device_id;
    if (cpv_device_rep) hapiFreeRaw(0);   // the primary context exists from here on
  }
#endif

  // Pinned-flag completion is the default; +gpueventquery asks for the old
  // cudaEventQuery path instead. The flag WRITE goes on the copy engine
  // (cuMemsetD32Async), NOT the front-end stream-memop path
  // (cuStreamWriteValue32). The front-end path serialises all of a context's
  // memory operations through one channel, so a flag write queued behind a
  // stream's cross-process cudaStreamWaitEvent head-of-line blocks EVERY later
  // flag write in the process -- on fresh streams too -- and two processes each
  // waiting on the other's IPC event then deadlock with the GPU idle. Proven
  // with +gpustalltrace: at the stall a lone write on a brand-new empty stream
  // would not even execute. The copy engine is scheduled per stream, so a
  // stuck memset stalls only its own stream while the rest keep completing,
  // which is what lets the awaited data flow and the wait clear. Measured:
  // memset ties cuStreamWriteValue32 on the GPU-bound LB config (within 1%) and
  // beats event queries there by ~11%, at ~2% over events on a non-GPU-bound
  // run where flag polling is a wash anyway. cuStreamWriteValue32 stays behind
  // +gpustreamwrite for comparison; it must never be the default.
  // Every PE parses the flags (the value is the same on all); the ring is
  // allocated after hapiSetDevice so its pinned pages and their device alias
  // belong to this PE's device.
  const bool want_event_query = CmiGetArgFlagDesc(argv, "+gpueventquery",
        "detect kernel completion by querying CUDA events instead of pinned-flag writes");
  // Accepted and ignored: they ask for what is now the default. Kept so that
  // existing run scripts and job files keep working unchanged.
  CmiGetArgFlagDesc(argv, "+gpuflagpoll",
        "(deprecated, now the default) use pinned-flag completion detection");
  CmiGetArgFlagDesc(argv, "+gpuflagmemset",
        "(deprecated, now the default) write completion flags with cuMemsetD32Async");
  const bool want_stream_write = CmiGetArgFlagDesc(argv, "+gpustreamwrite",
        "write completion flags with cuStreamWriteValue32 (front-end memop; "
        "DEADLOCK-PRONE under cross-process IPC -- for comparison only)");
  if (!want_event_query) {
#if CMK_HIP
    if (hapi_memset_d32_async == nullptr) hapi_memset_d32_async = hapiHipMemsetD32Async;
    if (hapi_stream_write_value32 == nullptr) hapi_stream_write_value32 = hapiHipStreamWriteValue32;
#else
    // Preferred writer, looked up first because it is the default.
    if (hapi_memset_d32_async == nullptr) {
      void* fn = nullptr;
      cudaDriverEntryPointQueryResult st = cudaDriverEntryPointSymbolNotFound;
      if (cudaGetDriverEntryPointByVersion("cuMemsetD32Async", &fn, 12000,
                                           cudaEnableDefault, &st) == hapiSuccess &&
          st == cudaDriverEntryPointSuccess && fn != nullptr)
        hapi_memset_d32_async = (hapiMemsetD32AsyncFn)fn;
      else hapiGetLastError();
    }
    // Fallback / +gpustreamwrite opt-in writer.
    if (hapi_stream_write_value32 == nullptr) {
      void* fn = nullptr;
      cudaDriverEntryPointQueryResult status = cudaDriverEntryPointSymbolNotFound;
      if (cudaGetDriverEntryPointByVersion("cuStreamWriteValue32", &fn, 12000,
                                           cudaEnableDefault, &status) == hapiSuccess &&
          status == cudaDriverEntryPointSuccess && fn != nullptr)
        hapi_stream_write_value32 = (hapiStreamWriteValue32Fn)fn;
      else hapiGetLastError();
    }
#endif
  }
  if (!want_event_query &&
      (hapi_memset_d32_async != nullptr || hapi_stream_write_value32 != nullptr)) {
    hapi_use_flag_poll = true;
    uint32_t*& slots = CpvAccess(hapi_flag_slots);
    const size_t bytes = (size_t)HAPI_FLAG_SLOTS * HAPI_FLAG_STRIDE * sizeof(uint32_t);
    hapiCheck(hapiHostAlloc((void**)&slots, bytes, hapiHostAllocPortable | hapiHostAllocMapped));
    memset((void*)slots, 0, bytes);
    hapiCheck(hapiHostGetDevicePointer(&CpvAccess(hapi_flag_slots_dev), (void*)slots, 0));
    CpvAccess(hapi_flag_busy) = (unsigned char*)calloc(HAPI_FLAG_SLOTS, 1);

    // Pick the writer. memset by default, validated with one synchronous write
    // so a driver that will not fill mapped host memory is caught here rather
    // than as a silent stall. +gpustreamwrite forces the front-end path when
    // its entry point is present.
    bool memset_ok = false;
    if (hapi_memset_d32_async != nullptr) {
      hapiStream_t ts;
      hapiCheck(hapiStreamCreateWithFlags(&ts, hapiStreamNonBlocking));
      const hapiDrvResult r = hapi_memset_d32_async((uintptr_t)CpvAccess(hapi_flag_slots_dev),
                                               0x5A5A0001u, 1, (hapiDrvStream)ts);
      if (r == hapiDrvSuccess && hapiStreamSynchronize(ts) == hapiSuccess &&
          ((volatile uint32_t*)slots)[0] == 0x5A5A0001u) memset_ok = true;
      hapiGetLastError();
      ((volatile uint32_t*)slots)[0] = 0;
      hapiStreamDestroy(ts);
    }
    if (want_stream_write && hapi_stream_write_value32 != nullptr) {
      hapi_flag_use_memset = false;
    } else if (memset_ok) {
      hapi_flag_use_memset = true;
    } else if (hapi_stream_write_value32 != nullptr) {
      hapi_flag_use_memset = false;   // memset unusable here; last resort
    } else {
      // Neither writer is usable: undo the ring and fall back to events.
      hapi_use_flag_poll = false;
      hapiFreeHost((void*)slots); slots = NULL;
      CpvAccess(hapi_flag_slots_dev) = NULL;
      free(CpvAccess(hapi_flag_busy)); CpvAccess(hapi_flag_busy) = NULL;
    }
    if (CmiMyPe() == 0) {
      if (!hapi_use_flag_poll)
        CmiPrintf("HAPI> no usable flag writer from this driver; using "
                  "event-query completion detection\n");
      else
        CmiPrintf("HAPI> Pinned-flag completion detection enabled (%d slots per "
                  "PE), flags written by %s\n", HAPI_FLAG_SLOTS,
                  hapi_flag_use_memset
                      ? "cuMemsetD32Async (copy engine)"
                      : "cuStreamWriteValue32 (front-end memop; DEADLOCK-PRONE under IPC)");
    }
  } else if (!want_event_query && CmiMyPe() == 0) {
    CmiPrintf("HAPI> no flag writer available from this driver; using "
              "event-query completion detection\n");
  }
  if (CmiGetArgFlagDesc(argv, "+gpurecvpriority", "create the runtime's receive streams at the device's highest stream priority")) {
    hapiSetRecvPriority();
  }
  // +gpustreams N: cap on shared application streams per device (see the pool).
  {
    int cap = 0;
#if CMK_HIP
    if (CmiGetArgFlagDesc(argv, "+gpuflagmemset", "completion flags written by an event record and hipMemsetD32Async (the pre-2026-10-22 writer)"))
      hapi_flag_kernel = false;
    CmiGetArgFlagDesc(argv, "+gpuflagkernel", "completion flags written by a one-thread kernel with a system-scope fence (the HIP default)");
    if (CmiMyPe() == 0) CmiPrintf("HAPI> Completion flags: %s\n", hapi_flag_kernel ? "written by a flag kernel (HIP default; +gpuflagmemset for the event record and memset)" : "written by an event record and a memset");
#endif
    if (CmiGetArgIntDesc(argv, "+gpustreams", &cap, "application streams per GPU, shared by chares (0 = one per chare)"))
      hapiSetStreamCap(cap);
    else hapiStreamCapAnnounce();
  }
  // +gpusubmit: the runtime's own driver calls go through submitter threads
  // (+gpusubmitters N per GPU, default 1). Needs the pinned-flag path: a queued
  // completion has to be safe to read before it is issued, which an event is not.
  {
    const bool want_submit = CmiGetArgFlagDesc(argv, "+gpusubmit",
        "issue the runtime's device copies and completion flags from submitter threads");
    int nsub = 1;
    CmiGetArgIntDesc(argv, "+gpusubmitters", &nsub, "submitter threads per GPU (default 1)");
    if (want_submit && nsub >= 1) {
      if (!hapi_use_flag_poll) {
        if (CmiMyPe() == 0) CmiPrintf("HAPI> +gpusubmit ignored: it needs pinned-flag completion detection\n");
      } else if (CsvAccess(gpu_manager).device_count != 1) {
        // A submitter holds ONE current device; one GPU per process is the supported layout.
        if (CmiMyPe() == 0) CmiPrintf("HAPI> +gpusubmit ignored: %d devices in this process, it needs exactly one\n",
                                      CsvAccess(gpu_manager).device_count);
      } else {
        hapiSubmitStart(nsub, CpvAccess(my_device));
        if (CmiMyPe() == 0)
          CmiPrintf("HAPI> Submitter threads on: %d per GPU; device-receive copies and completion flags "
                    "are issued off the PEs\n", nsub);
      }
    }
  }
  // +gpustalltrace: log records, waits and flag issues for hapiDumpFlagState.
  if (CmiGetArgFlagDesc(argv, "+gpustalltrace",
                        "log event records and stream waits so a stalled stream can be walked")) {
    hapiTraceInit();
    hapi_trace_on = true;
    CpvInitialize(uint32_t*, hapi_flag_probe);
    CpvInitialize(void*, hapi_flag_probe_dev);
    uint32_t*& pr = CpvAccess(hapi_flag_probe);
    hapiCheck(hapiHostAlloc((void**)&pr, 256, hapiHostAllocPortable | hapiHostAllocMapped));
    memset((void*)pr, 0, 256);
    hapiCheck(hapiHostGetDevicePointer(&CpvAccess(hapi_flag_probe_dev), (void*)pr, 0));
    if (CmiMyPe() == 0) CmiPrintf("HAPI> stall trace on\n");
  }
#if CMK_SMP
  CmiLock(csv_gpu_manager.device_mapping_lock);
#endif
  csv_gpu_manager.device_map.emplace(CmiMyPe(), &(csv_gpu_manager.device_managers[cpv_my_device_id]));
#if CMK_SMP
  CmiUnlock(csv_gpu_manager.device_mapping_lock);
#endif

  // Comm. thread will set its device to the same one as worker thread 0
  if (CmiMyRank() == 0) csv_gpu_manager.comm_thread_device = cpv_my_device;

  // Check if user opted in to POSIX shared memory optimizations for
  // inter-process GPU messaging
  // +gpupool: the device pool as the runtime's allocation policy. Parsed before
  // +gpushm because that block decides, on it, whether a comm buffer exists.
  {
    const bool want_pool = CmiGetArgFlagDesc(argv, "+gpupool",
        "device memory from the runtime's pooled arenas: migration arenas and "
        "payloads come from it, cross-process device sends are direct CUDA IPC, "
        "and each process's first arena is opened on all peers at startup; "
        "+gpucommbuffer and +gpulbbuffer are not used");
    int pool_mb = 0;
    const bool have_size = CmiGetArgIntDesc(argv, "+gpupoolsize", &pool_mb,
        "size of each device pool arena in MB (default 256, or CK_GPU_ARENA_MB)");
    int pool_arenas = 0;
    const bool have_arenas = CmiGetArgIntDesc(argv, "+gpupoolarenas", &pool_arenas,
        "device pool arenas opened per device at startup (default 1): the load "
        "balancer plans within the arenas the pool has, so this reserves its "
        "migration headroom without growing the pool mid-run");
    char* pool_alloc = nullptr;
    const bool have_alloc = CmiGetArgStringDesc(argv, "+gpupoolalloc", &pool_alloc,
        "device pool backend: 'buddy' (default: power-of-two blocks in cudaMalloc "
        "arenas of +gpupoolsize) or 'vmm' (one reserved virtual range per device, "
        "backed in place by cuMemCreate chunks of +gpupoolchunk MB, 256-byte "
        "granularity, peers map chunks by file descriptor)");
    int pool_chunk_mb = 0, pool_reserve_mb = 0;
    const bool have_chunk = CmiGetArgIntDesc(argv, "+gpupoolchunk", &pool_chunk_mb,
        "vmm backend: chunk size in MB (default 256); one IPC handle and one RDMA "
        "registration per chunk. A buffer sent off-node must lie in one chunk, "
        "and a chunk of 2048 MB or more registers without error and then "
        "accepts no remote write at all (libfabric cxi 2.3.1), so on more than "
        "one node the size, rounded up to the allocation granularity (2 MB), "
        "must stay below 2048");
    const bool have_reserve = CmiGetArgIntDesc(argv, "+gpupoolreserve", &pool_reserve_mb,
        "vmm backend: virtual address range reserved per device in MB (default: "
        "twice the device's memory)");
    if (CmiMyRank() == 0) {
      csv_gpu_manager.device_pool_on = want_pool;
      if (have_size && pool_mb > 0)
        csv_gpu_manager.device_pool_arena_bytes = (size_t)pool_mb << 20;
      if (have_arenas && pool_arenas > 0)
        csv_gpu_manager.device_pool_arenas = pool_arenas;
      if (have_alloc && pool_alloc != nullptr) {
        if (strcmp(pool_alloc, "vmm") == 0) csv_gpu_manager.device_pool_backend = 1;
        else if (strcmp(pool_alloc, "buddy") == 0) csv_gpu_manager.device_pool_backend = 0;
        else CmiAbort("+gpupoolalloc: expected 'buddy' or 'vmm', got '%s'", pool_alloc);
      }
      if (have_chunk && pool_chunk_mb > 0)
        csv_gpu_manager.device_pool_chunk_bytes = (size_t)pool_chunk_mb << 20;
      if (have_reserve && pool_reserve_mb > 0)
        csv_gpu_manager.device_pool_reserve_bytes = (size_t)pool_reserve_mb << 20;
    }
    if (CmiMyPe() == 0 && want_pool && have_alloc && pool_alloc && strcmp(pool_alloc, "vmm") == 0)
      CmiPrintf("HAPI> Device pool backend: vmm (reserved range per device, %d MB chunks "
                "mapped in place, 256-byte blocks)\n", pool_chunk_mb > 0 ? pool_chunk_mb : 256);
    if (CmiMyPe() == 0 && want_pool)
      CmiPrintf("HAPI> Device pool on (+gpupool): arenas of %d MB, migration "
                "arenas and payloads from the pool, direct CUDA IPC only\n",
                (have_size && pool_mb > 0) ? pool_mb
                    : (getenv("CK_GPU_ARENA_MB") ? atoi(getenv("CK_GPU_ARENA_MB")) : 256));
  }

  // GPU load: how a busy interval is charged to the kernels holding the
  // device (hapiCuptiNormalizeLoads). Default: SM-seconds against the whole
  // device, dt * sms_used / max(sum of sms_used, total SMs). +gpuloadbusy
  // restores the split of the interval's wall time among the active kernels,
  // which charges a lone small kernel the whole interval.
  {
    const bool busy = CmiGetArgFlagDesc(argv, "+gpuloadbusy",
        "charge GPU load as a share of each busy interval's wall time (old "
        "behaviour) instead of SM-seconds against the device's SM count");
    if (CmiMyRank() == 0) hapi_gpu_load_busy = busy;
    if (CmiMyPe() == 0 && busy)
      CmiPrintf("HAPI> GPU load: busy-time split (+gpuloadbusy)\n");
  }

  bool use_shm = false;
  if (CmiGetArgFlagDesc(argv, "+gpushm",
        "enable shared memory optimizations for inter-process GPU messaging")) {
    use_shm = true;
    if (CmiMyPe() == 0) {
      CmiPrintf("HAPI> Enabled POSIX shared memory optimizations for inter-process GPU messaging\n");
    }
  }

  if (CmiMyRank() == 0) {
    if (use_shm) {
      csv_gpu_manager.use_shm = true;
    }
    // csv_gpu_manager.test_field = true;
  }

  CmiNodeBarrier();

  /* These three are consumed unconditionally, even though only the
   * shared-memory path acts on them. CmiGetArgIntDesc also *removes* the
   * argument from argv, so leaving the calls inside the use_shm branch meant
   * that passing any of them without +gpushm left a stray '+arg' behind, and
   * the RTS then reported "not parsed by the RTS ... you may need to recompile
   * Charm++ with different options" -- pointing at a build problem that does
   * not exist, for a flag that was simply inapplicable. Parse them always and
   * say plainly when they do not apply. */
  int input_comm_buffer_size = 0;
  const bool given_comm_buffer = CmiGetArgIntDesc(argv, "+gpucommbuffer",
      &input_comm_buffer_size, "GPU communication buffer size (in MB); requires +gpushm");

  int input_lb_buffer_size = 0;
  const bool given_lb_buffer = CmiGetArgIntDesc(argv, "+gpulbbuffer",
      &input_lb_buffer_size, "GPU load balancing buffer size (in MB); requires +gpushm");

  int input_hapi_ipc_event_pool_size = 16;
  const bool given_event_pool = CmiGetArgIntDesc(argv, "+gpuipceventpool",
      &input_hapi_ipc_event_pool_size, "GPU IPC event pool size per PE; requires +gpushm");

  if (!csv_gpu_manager.use_shm && CmiMyPe() == 0 &&
      (given_comm_buffer || given_lb_buffer || given_event_pool)) {
    CmiPrintf("HAPI> Ignoring +gpucommbuffer/+gpulbbuffer/+gpuipceventpool: "
        "these configure the shared-memory inter-process path, which is off "
        "unless +gpushm is also given\n");
  }

  if (csv_gpu_manager.use_shm) {
    if (given_comm_buffer) {
      if (CmiMyRank() == 0 && input_comm_buffer_size > 0) {
        // Round up to the next power of two, in integer arithmetic.
        //
        // This was pow(): `(size_t)std::pow(2, ceil(log2(bytes)))`. pow returns
        // a double and is not exact here -- on this toolchain pow(2,29) and
        // pow(2,31) both come back a hair under the true value, and the cast
        // truncated them to 2^29-1 and 2^31-1. Requests of 64, 128, 256 and
        // 1024 MB happened to survive; 512 and 2048 MB did not.
        //
        // A size one byte short of a power of two is not a rounding nuisance,
        // it silently corrupts data: the buddy allocator seeds its top bucket
        // with this whole region and reaches the smaller buckets by halving,
        // and halving 2^k-1 truncates to 2^(k-1)-1 all the way down. Every
        // bucket then hands out blocks one byte short of their nominal size,
        // so consecutive blocks overlap by a byte and a request of exactly 2^n
        // gets a 2^n-1 block. The staged copy writes its last byte into the
        // next block, and the next sender's first byte overwrites it -- one
        // wrong trailing byte per message, only for power-of-two payloads,
        // only with more than one send in flight.
        size_t requested_bytes = (size_t)input_comm_buffer_size * 1024 * 1024;
        size_t rounded = 1;
        while (rounded < requested_bytes) rounded <<= 1;
        csv_gpu_manager.comm_buffer_size = rounded;
      }
    }

    if (given_lb_buffer) {
      if (CmiMyRank() == 0) {
        csv_gpu_manager.lb_buffer_size =  (size_t)input_lb_buffer_size * 1024 * 1024;
      }
    }

    if (CmiMyPe() == 0) {
      if (csv_gpu_manager.device_pool_on) {
        CmiPrintf("HAPI> No GPU communication or load balancing buffer: the "
                  "device pool serves both (+gpupool)%s\n",
                  (input_comm_buffer_size > 0 || input_lb_buffer_size > 0)
                      ? "; +gpucommbuffer / +gpulbbuffer ignored" : "");
      } else {
        CmiPrintf("HAPI> GPU communication buffer size: %zu MB "
            "(rounded up to the nearest power of two)\n",
            csv_gpu_manager.comm_buffer_size / (1024 * 1024));

        CmiPrintf("HAPI> GPU load balancing buffer size: %zu MB "
            "\n",
            csv_gpu_manager.lb_buffer_size / (1024 * 1024));
      }
    }

    CmiNodeBarrier(); // Ensure device communication buffer size is set

    // Create device communication buffers
    // Should only be done by device representative threads
    if (cpv_device_rep && !csv_gpu_manager.device_pool_on) {
      DeviceManager* dm = csv_gpu_manager.device_map[CmiMyPe()];
#if CMK_SMP
      CmiLock(dm->lock);
#endif
      dm->create_comm_buffer(csv_gpu_manager.comm_buffer_size + csv_gpu_manager.lb_buffer_size, csv_gpu_manager.comm_buffer_size);
#if CMK_SMP
      CmiUnlock(dm->lock);
#endif
    }

    if (CmiMyRank() == 0) {
      csv_gpu_manager.hapi_ipc_event_pool_size_pe = input_hapi_ipc_event_pool_size;
      csv_gpu_manager.hapi_ipc_event_pool_size_total = input_hapi_ipc_event_pool_size * csv_gpu_manager.pes_per_device;
    }

    if (CmiMyPe() == 0) {
      CmiPrintf("HAPI> CUDA IPC event pool size - %d per PE, %d per device\n",
          csv_gpu_manager.hapi_ipc_event_pool_size_pe, csv_gpu_manager.hapi_ipc_event_pool_size_total);
    }

    // Whether cross-process sends export their source allocation instead of
    // staging it through the device communication buffer. One choice for the
    // whole run, not a per-payload one.
    const bool want_direct = CmiGetArgFlagDesc(argv, "+gpuipcdirect",
        "cross-process device sends export their source allocation (direct "
        "CUDA IPC) instead of staging it");
    // Under +gpupool there is nothing to stage into, so direct is the only mode.
    if (CmiMyRank() == 0)
      csv_gpu_manager.ipc_use_direct = want_direct || csv_gpu_manager.device_pool_on;

    CmiNodeBarrier(); // Ensure the choice is set before any send reads it

    if (CmiMyPe() == 0) {
      CmiPrintf("HAPI> Cross-process device sends: %s\n",
                csv_gpu_manager.ipc_use_direct
                    ? "direct CUDA IPC (source allocation exported)"
                    : "staged through the device communication buffer");
    }
  }

  // Check if P2P access should be enabled
  bool enable_peer = true; // Enabled by default
  if (CmiGetArgFlagDesc(argv, "+gpunopeer",
        "do not enable P2P access between visible GPU pairs")) {
    enable_peer = false;
  }

  // Enable P2P access to every other device on this host, not just the ones
  // this process owns. Inter-process GPU messaging opens an IPC handle to a
  // buffer on a peer process's device, and the resulting pointer is only usable
  // once peer access to that device has been enabled here. Ranging over
  // device_count (a per-process count) instead left peer access off entirely in
  // the common one-GPU-per-process layout, so the first copy touching a peer
  // buffer faulted with an illegal access -- reported at whichever CUDA call
  // checked next, since error 700 is sticky and asynchronous.
  //
  // Note cpv_my_device and the loop index are both global device indices;
  // device_count_on_physical_node counts devices on the host.
  //
  // Should only be done by device representative threads
  if (enable_peer) {
    if (CmiMyPe() == 0) {
      CmiPrintf("HAPI> Enabling P2P access between devices\n");
    }
    if (cpv_device_rep) {
      for (int i = 0; i < csv_gpu_manager.device_count_on_physical_node; i++) {
        if (i != cpv_my_device) {
          int can_access_peer;

          hapiCheck(hapiDeviceCanAccessPeer(&can_access_peer, cpv_my_device, i));
          if (can_access_peer) {
            // Returns hapiErrorPeerAccessAlreadyEnabled when already on, which
            // is benign -- deliberately not wrapped in hapiCheck.
            hapiDeviceEnablePeerAccess(i, 0);
          } else if (_lb_args.debug() > 0 && CmiMyPe() == 0) {
            CmiPrintf("HAPI> No P2P access from device %d to device %d\n",
                      cpv_my_device, i);
          }
        }
      }
    }
  } else {
    if (CmiMyPe() == 0) {
      CmiPrintf("HAPI> P2P access between devices not enabled\n");
    }
  }
}

int hapiStreamDeviceOf(hapiStream_t stream);   // defined with the stream pool below

/*** Per-GPU submitter threads (+gpusubmit, +gpusubmitters N) ***
 *
 * One thread (or N) issues the runtime's own driver calls for the whole
 * process; PEs hand them over through single-producer rings and go back to
 * running chares. Measured 2026-09-21 on an A40 with CUPTI attached: a driver
 * call costs 3.7 us from one thread and 15.6 us when 8 threads call at once,
 * and 8 threads together deliver only 1.9x one thread's call rate
 * (tests/util/launchcost). Stage 1 routes what the runtime issues itself and
 * what is safe to issue LATE: the device-receive copies on the per-PE receive
 * stream and every pinned completion-flag write. Event records stay direct --
 * a peer may QUERY an event, and an event not yet recorded reads as complete;
 * a flag not yet written reads as not landed, which is the safe direction.
 *
 * Ordering: a stream always maps to the same submitter, a PE's ops for it sit
 * in one FIFO ring, and the submitter issues each ring in order, so per-PE,
 * per-stream order is the enqueue order -- what hapiPollEvents relies on. An op
 * queued here is issued no earlier than any call the PE made directly before
 * it, so a flag can never precede the work it follows.
 *
 * No condition variables and nothing with a destructor: the threads spin, and
 * the state is heap-allocated and never freed, so exit() cannot block on it
 * (see the LB trace helper's exit hang, 2026-09-21).
 */
namespace {
enum HapiSubmitKind : uint32_t { HAPI_SUBMIT_MEMCPY = 1, HAPI_SUBMIT_FLAG = 2,
                                 HAPI_SUBMIT_CLOSURE = 3, HAPI_SUBMIT_EVENT = 4 };
static const size_t HAPI_SUBMIT_INLINE = 128;   // bytes of closure state carried in the entry
static const size_t HAPI_SUBMIT_BATCH_MAX = 64;   // ops one batch entry can hold
struct HapiSubmitOp {
  uint32_t kind;
  uint32_t value;       // FLAG: the sequence number to write
  hapiStream_t stream;
  void* dst;            // MEMCPY: destination; FLAG: device alias of the slot; EVENT: the event
  const void* src;
  size_t bytes;
  int copyKind;
  void (*fn)(void*);    // CLOSURE: called with the inline state below
  uint64_t token;       // CLOSURE: the chare the PE was running, for CUPTI attribution
  unsigned char state[HAPI_SUBMIT_INLINE];
  struct HapiSubmitOp* batch;   // BATCH: `value` ops, issued in order, then freed
};
enum : uint32_t { HAPI_SUBMIT_BATCH = 5 };

// hapiSubmitBatchBegin/End (a PE, on itself): between them every op this PE
// hands over is appended to ONE ring entry instead of taking a slot each. The
// submitter issues them back to back. A patch-step's fixed launch sequence is
// ~10-28 ops; measured 2026-09-21 the per-entry handoff, not the driver call,
// was the submitter's cost on sph2d. Nested begins are counted; a closure that
// must be issued before the batch ends does not exist (nothing here waits on
// the GPU inside a batch), and a drain inside a batch ends it first.
static thread_local HapiSubmitOp* hapi_submit_batch = nullptr;
static thread_local uint32_t hapi_submit_batch_n = 0;
static thread_local int hapi_submit_batch_depth = 0;
static const uint32_t HAPI_SUBMIT_RING = 1u << 14;   // entries per (submitter, PE) ring
struct HapiSubmitRing {
  alignas(64) std::atomic<uint32_t> head{0};   // advanced by the submitter, after issuing
  alignas(64) std::atomic<uint32_t> tail{0};   // advanced by the owning PE
  HapiSubmitOp ops[HAPI_SUBMIT_RING];
};
struct HapiSubmitState {
  int nsub = 0, npes = 0, device = -1;
  HapiSubmitRing** rings = nullptr;            // [nsub][npes]
  std::atomic<unsigned long long>* issued = nullptr;   // per submitter, for the exit report
};
HapiSubmitState* hapi_submit = nullptr;        // NULL: mode off (the default)
std::once_flag hapi_submit_once;

// The chare this submitter thread currently has pushed as its CUPTI external
// correlation (0: none). A push/pop pair costs about as much as the launch it
// brackets under CUPTI, and consecutive entries are usually the same chare's, so
// the correlation is pushed when the chare CHANGES and popped when the run of
// entries ends -- the same attribution, a fraction of the CUPTI calls.
static thread_local uint64_t hapi_submit_pushed_token = 0;
static inline void hapiSubmitSetCorrelation(uint64_t token) {
  if (token == hapi_submit_pushed_token) return;
#ifdef CMK_LBDB_ON
  CuptiEntryLog& L = hapiCuptiEntryLog();
  std::lock_guard<std::mutex> g(L.m);
  if (hapi_submit_pushed_token != 0) hapiCuptiLaneClose(L.obj);
  if (token != 0) hapiCuptiLaneOpen(L.obj, token);
#endif
  hapi_submit_pushed_token = token;
}

void hapiSubmitIssue(HapiSubmitOp& op) {
  CuptiGateShared gate;   // no CUPTI flush while this driver call runs
  if (op.kind == HAPI_SUBMIT_CLOSURE) {
    // The application's own launches. The PE's CUPTI correlation stack is not
    // this thread's, so the chare travels in the entry and is applied here, or
    // the balancer would see no GPU load for anything launched through a closure.
    hapiSubmitSetCorrelation(hapiCuptiTracingActive() ? op.token : 0);
    op.fn((void*)op.state);
  } else if (op.kind == HAPI_SUBMIT_BATCH) {
    for (uint32_t i = 0; i < op.value; i++) hapiSubmitIssue(op.batch[i]);
    delete[] op.batch;
  } else if (op.kind == HAPI_SUBMIT_EVENT) {
    if (hapiEventRecordRaw((hapiEvent_t)op.dst, (hapiStream_t)op.stream) != hapiSuccess) {
      fprintf(stderr, "HAPI submitter: cudaEventRecord failed\n"); abort();
    }
  } else if (op.kind == HAPI_SUBMIT_MEMCPY) {
    const hapiError_t e = hapiMemcpyAsync(op.dst, op.src, op.bytes, (hapiMemcpyKind)op.copyKind,
                                          (hapiStream_t)op.stream);
    if (e != hapiSuccess) {
      fprintf(stderr, "HAPI submitter: cudaMemcpyAsync(%p <- %p, %zu) failed: %s\n", op.dst, op.src,
              op.bytes, hapiGetErrorString(e));
      abort();
    }
  } else {
    const hapiDrvResult r = hapi_flag_use_memset
        ? hapi_memset_d32_async((uintptr_t)op.dst, op.value, 1, (hapiDrvStream)op.stream)
        : hapi_stream_write_value32((hapiDrvStream)op.stream, (uintptr_t)op.dst, op.value,
                                    hapiStreamWriteValueDefault);
    if (r != hapiDrvSuccess) {
      fprintf(stderr, "HAPI submitter: completion-flag write failed (%d)\n", (int)r);
      abort();
    }
  }
}

static thread_local bool hapi_is_submitter_thread = false;
void hapiSubmitThread(int k) {
  HapiSubmitState* st = hapi_submit;
  hapi_is_submitter_thread = true;
  // A thread inherits its creator's affinity -- one PE's core. Widen it to any
  // CPU, or the submitter time-slices with that PE.
  cpu_set_t set; CPU_ZERO(&set);
  for (int c = 0; c < CPU_SETSIZE; c++) CPU_SET(c, &set);
  sched_setaffinity(0, sizeof(set), &set);
  if (hapiSetDevice(st->device) != hapiSuccess) { fprintf(stderr, "HAPI submitter: cudaSetDevice failed\n"); abort(); }
  unsigned idle = 0;
  for (;;) {
    bool any = false;
    for (int r = 0; r < st->npes; r++) {
      HapiSubmitRing& ring = st->rings[k][r];
      const uint32_t h = ring.head.load(std::memory_order_relaxed);
      uint32_t n = ring.tail.load(std::memory_order_acquire) - h;
      if (n == 0) continue;
      if (n > 4) n = 4;                          // a few per PE, then the next PE: breadth-first
      for (uint32_t i = 0; i < n; i++) hapiSubmitIssue(ring.ops[(h + i) & (HAPI_SUBMIT_RING - 1)]);
      ring.head.store(h + n, std::memory_order_release);
      st->issued[k].fetch_add(n, std::memory_order_relaxed);
      any = true;
    }
    if (any) idle = 0;
    else {
      hapiSubmitSetCorrelation(0);   // nothing queued: leave no correlation pushed
      if (++idle > 4096) { sched_yield(); idle = 4096; }
    }
  }
}

inline bool hapiSubmitPushRaw(const HapiSubmitOp& op);
inline void hapiSubmitBatchFlush() {
  if (hapi_submit_batch_n == 0) return;
  HapiSubmitOp e{};
  e.kind = HAPI_SUBMIT_BATCH; e.value = hapi_submit_batch_n; e.batch = hapi_submit_batch;
  e.stream = hapi_submit_batch[0].stream;
  hapi_submit_batch = nullptr; hapi_submit_batch_n = 0;
  hapiSubmitPushRaw(e);
}
inline bool hapiSubmitPush(const HapiSubmitOp& op) {
  HapiSubmitState* st = hapi_submit;
  if (st == nullptr) return false;
  // Only a PE may push: a push from any other thread would land in ring 0
  // (CmiMyRank() is 0 on a thread with no Converse state) and reorder against
  // PE 0's real work. A submitter that reaches here is issuing an op that
  // itself tries to queue.
  if (hapi_is_submitter_thread) {
    fprintf(stderr, "HAPI submitter: an issued op pushed a new op (kind %u) from the submitter thread\n", op.kind);
    abort();
  }
  if (hapi_submit_batch_depth > 0) {
    if (hapi_submit_batch == nullptr) hapi_submit_batch = new HapiSubmitOp[HAPI_SUBMIT_BATCH_MAX];
    hapi_submit_batch[hapi_submit_batch_n++] = op;
    if (hapi_submit_batch_n == HAPI_SUBMIT_BATCH_MAX) hapiSubmitBatchFlush();
    return true;
  }
  return hapiSubmitPushRaw(op);
}
inline bool hapiSubmitPushRaw(const HapiSubmitOp& op) {
  HapiSubmitState* st = hapi_submit;
  if (st == nullptr) return false;
  // The submitter is chosen by PE, not by stream: everything one PE issues, on
  // all its streams, then has a single total order. A chare's streams depend on
  // each other (sph2d records an event on its comm stream and waits for it on
  // its compute stream), and with the choice made per stream those two calls
  // could reach different submitters and swap.
  const int k = CmiMyRank() % st->nsub;
  HapiSubmitRing& ring = st->rings[k][CmiMyRank()];
  const uint32_t t = ring.tail.load(std::memory_order_relaxed);
  while (t - ring.head.load(std::memory_order_acquire) >= HAPI_SUBMIT_RING) {}   // full: wait for room
  ring.ops[t & (HAPI_SUBMIT_RING - 1)] = op;
  ring.tail.store(t + 1, std::memory_order_release);
  return true;
}
}  // namespace

bool hapiSubmitOn() { return hapi_submit != nullptr; }

// Everything this PE has queued has been ISSUED to the driver. Call before a
// device- or stream-wide synchronize that must cover the runtime's queued work.
void hapiSubmitBatchBegin() { if (hapi_submit != nullptr) hapi_submit_batch_depth++; }
void hapiSubmitBatchEnd() {
  if (hapi_submit == nullptr || hapi_submit_batch_depth == 0) return;
  if (--hapi_submit_batch_depth == 0) hapiSubmitBatchFlush();
}

void hapiSubmitDrain() {
  HapiSubmitState* st = hapi_submit;
  if (st == nullptr) return;
  if (hapi_submit_batch_n > 0) hapiSubmitBatchFlush();   // a drain covers what was batched so far
  const int r = CmiMyRank();
  for (int k = 0; k < st->nsub; k++) {
    HapiSubmitRing& ring = st->rings[k][r];
    while (ring.head.load(std::memory_order_acquire) != ring.tail.load(std::memory_order_relaxed)) {}
  }
}

// A device copy the runtime issues on a stream it owns: through the submitter
// when the mode is on, directly otherwise.
void hapiSubmitMemcpyAsync(void* dst, const void* src, size_t bytes, int kind, hapiStream_t stream) {
  HapiSubmitOp op{};
  op.kind = HAPI_SUBMIT_MEMCPY; op.stream = stream; op.dst = dst; op.src = src; op.bytes = bytes; op.copyKind = kind;
  if (hapiSubmitPush(op)) return;
  CuptiGateShared gate;
  hapiCheck(hapiMemcpyAsync(dst, src, bytes, (hapiMemcpyKind)kind, (hapiStream_t)stream));
}

// The chare whose entry method this PE is running, as CUPTI knows it (0: none).
static thread_local uint64_t hapi_submit_cur_token = 0;

// An application launch (or any driver call on a chare's stream) handed to the
// submitter: fn(state) runs on the submitter thread. `bytes` of state are copied
// into the queue entry, so it must be trivially copyable and at most
// HAPI_SUBMIT_INLINE bytes. With the mode off it runs here, at once.
// CHARM_ZC_STATS: where the host time of a completion callback and of an inline closure submit goes
// (Frontier 2026-10-22: a same-host channel batch flush cost 54-76 us, a cross-host put's persistent
// put 25-42 us, while the HIP calls themselves measure 1-4 us in isolation).
namespace {
struct HapiCostStats {
  std::atomic<long> dev_n{0}, dev_us{0}, flag_n{0}, flag_us{0}, push_n{0}, push_us{0}, gate_n{0}, gate_us{0}, fn_n{0}, fn_us{0};
  std::atomic<long> poll_n{0}, poll_us{0}, poll_max_us{0};   // hapiPollEvents, the scheduler-loop callback
  static void addmax(std::atomic<long>& n, std::atomic<long>& us, std::atomic<long>& mx, double t0) {
    const long d = (long)((CmiWallTimer() - t0) * 1e6); n.fetch_add(1, std::memory_order_relaxed); us.fetch_add(d, std::memory_order_relaxed);
    long m = mx.load(std::memory_order_relaxed); while (d > m && !mx.compare_exchange_weak(m, d, std::memory_order_relaxed)) {}
  }
  static bool on() { static const bool v = getenv("CHARM_ZC_STATS") != nullptr; return v; }
  static void print() {
    HapiCostStats& c = get();
    fprintf(stderr, "[hapi-cost] pid=%d callback: device lookup n=%ld (%.2f us), flag write n=%ld (%.2f us), queue push n=%ld (%.2f us); "
            "inline submit: gate n=%ld (%.2f us), closure n=%ld (%.2f us); hapiPollEvents n=%ld (%.2f us, max %.2f ms)\n", (int)getpid(),
            c.dev_n.load(), c.dev_n.load() ? c.dev_us.load() / (double)c.dev_n.load() : 0.0,
            c.flag_n.load(), c.flag_n.load() ? c.flag_us.load() / (double)c.flag_n.load() : 0.0,
            c.push_n.load(), c.push_n.load() ? c.push_us.load() / (double)c.push_n.load() : 0.0,
            c.gate_n.load(), c.gate_n.load() ? c.gate_us.load() / (double)c.gate_n.load() : 0.0,
            c.fn_n.load(), c.fn_n.load() ? c.fn_us.load() / (double)c.fn_n.load() : 0.0,
            c.poll_n.load(), c.poll_n.load() ? c.poll_us.load() / (double)c.poll_n.load() : 0.0, c.poll_max_us.load() / 1e3);
  }
  static HapiCostStats& get() { static HapiCostStats* s = [] { HapiCostStats* p = new HapiCostStats; atexit(print); return p; }(); return *s; }
  static void add(std::atomic<long>& n, std::atomic<long>& us, double t0) {
    n.fetch_add(1, std::memory_order_relaxed); us.fetch_add((long)((CmiWallTimer() - t0) * 1e6), std::memory_order_relaxed);
  }
};
}  // namespace

void hapiSubmitClosure(hapiStream_t stream, void (*fn)(void*), const void* state, size_t bytes) {
  if (hapi_submit == nullptr) {
    const bool cost = HapiCostStats::on();
    double tc = cost ? CmiWallTimer() : 0.0;
    CuptiGateShared gate;
    if (cost) { HapiCostStats& c = HapiCostStats::get(); HapiCostStats::add(c.gate_n, c.gate_us, tc); tc = CmiWallTimer(); }
    fn(const_cast<void*>(state));
    if (cost) { HapiCostStats& c = HapiCostStats::get(); HapiCostStats::add(c.fn_n, c.fn_us, tc); }
    return;
  }
  if (bytes > HAPI_SUBMIT_INLINE) CmiAbort("hapiSubmitClosure: %zu bytes of state, at most %zu fit", bytes, HAPI_SUBMIT_INLINE);
  HapiSubmitOp op{};
  op.kind = HAPI_SUBMIT_CLOSURE; op.stream = stream; op.fn = fn; op.token = hapi_submit_cur_token;
  memcpy(op.state, state, bytes);
  hapiSubmitPush(op);
}

// cudaEventRecord behind this PE's queued work on the stream. NEVER query or
// wait on such an event from elsewhere before a flag says its work is done: an
// event still in the queue reads as complete.
void hapiSubmitEventRecord(void* event, hapiStream_t stream) {
  HapiSubmitOp op{};
  op.kind = HAPI_SUBMIT_EVENT; op.stream = stream; op.dst = event;
  if (hapiSubmitPush(op)) return;
  CuptiGateShared gate;
  hapiCheck(hapiEventRecordRaw((hapiEvent_t)event, (hapiStream_t)stream));
}

static void hapiSubmitSetToken(uint64_t token) { hapi_submit_cur_token = token; }

static void hapiSubmitStart(int nsub, int device) {
  std::call_once(hapi_submit_once, [nsub, device]() {
    HapiSubmitState* st = new HapiSubmitState;
    st->nsub = nsub; st->npes = CmiMyNodeSize(); st->device = device;
    st->rings = new HapiSubmitRing*[nsub];
    st->issued = new std::atomic<unsigned long long>[nsub];
    for (int k = 0; k < nsub; k++) { st->rings[k] = new HapiSubmitRing[st->npes]; st->issued[k].store(0); }
    hapi_submit = st;
    for (int k = 0; k < nsub; k++) std::thread(hapiSubmitThread, k).detach();
  });
}

#ifndef HAPI_CUDA_CALLBACK
// The pending-entry queue for `stream`, created on first use and kept (see
// hapi_event_queue).
static std::queue<hapiEvent>& hapiQueueFor(hapiStream_t stream) {
  hapiEventQueues& queues = CpvAccess(hapi_event_queue);
  for (auto& entry : queues)
    if (entry.first == stream) return entry.second;
  queues.emplace_back(stream, std::queue<hapiEvent>());
  return queues.back().second;
}

void recordEvent(hapiStream_t stream, const CkCallback& cb, void* cb_msg, hapiWorkRequest* wr = NULL) {
  const bool cost = HapiCostStats::on();
  double tc = cost ? CmiWallTimer() : 0.0;
  // if(obj!=NULL)
  //   CmiAbort("non null without HAPI CUDA CALLBACK");
  // create CUDA event / get CUDA event from the pool and insert into stream
  // An event belongs to the device that was current when it was created, and it
  // can only be recorded on a stream of that same device. `stream` is not
  // necessarily ours: the device-send path records a completion event on the
  // stream carried by a CkDeviceBuffer, which for a received buffer is the
  // SENDER's stream, on the sender's GPU. So switch to the stream's device for
  // both the create and the record, and keep the free pool partitioned by
  // device so a pooled event is never handed across GPUs.
  const int stream_dev = hapiStreamDeviceOf(stream);
  int prev_dev = -1;
  hapiCheck(hapiGetDevice(&prev_dev));
  const int use_dev = (stream_dev >= 0) ? stream_dev : prev_dev;
  if (cost) { HapiCostStats& c = HapiCostStats::get(); HapiCostStats::add(c.dev_n, c.dev_us, tc); tc = CmiWallTimer(); }

  // Pinned-flag path (the default): no event object, no query later. Only for
  // a stream on this PE's own device -- the ring's device alias belongs to
  // it -- and while the ring has a free slot; otherwise an event, as before.
  // hapiFlagNextSeq: the seq this issue would take, 0 skipped as it means
  // "slot never written". Checked against the slot it would land in, because
  // the slot is what must be free -- see hapi_flag_busy.
  uint32_t hapi_next_seq = CpvAccess(hapi_flag_seq) + 1;
  if (hapi_next_seq == 0) hapi_next_seq = 1;
  if (hapi_use_flag_poll && use_dev == CpvAccess(my_device) &&
      !CpvAccess(hapi_flag_busy)[hapi_next_seq & (HAPI_FLAG_SLOTS - 1)]) {
    const uint32_t seq = hapi_next_seq;
    CpvAccess(hapi_flag_seq) = seq;
    const uint32_t idx = seq & (HAPI_FLAG_SLOTS - 1);
    CpvAccess(hapi_flag_busy)[idx] = 1;
    uint32_t* slot_dev =
        (uint32_t*)CpvAccess(hapi_flag_slots_dev) + (size_t)idx * HAPI_FLAG_STRIDE;
    // Executes only after all prior work on the stream, so the slot reaching
    // seq means that work is complete.
    HapiSubmitOp flagOp{};
    flagOp.kind = HAPI_SUBMIT_FLAG; flagOp.value = seq; flagOp.stream = stream; flagOp.dst = (void*)slot_dev;
    const hapiDrvResult res = hapiSubmitPush(flagOp) ? hapiDrvSuccess : hapi_flag_use_memset
        ? hapi_memset_d32_async((uintptr_t)slot_dev, seq, 1, (hapiDrvStream)stream)
        : hapi_stream_write_value32((hapiDrvStream)stream, (uintptr_t)slot_dev,
                                    seq, hapiStreamWriteValueDefault);
    if (res != hapiDrvSuccess)
      CmiAbort("HAPI> %s failed (%d); rerun with +gpueventquery",
               hapi_flag_use_memset ? "cuMemsetD32Async" : "cuStreamWriteValue32", (int)res);
    if (cost) { HapiCostStats& c = HapiCostStats::get(); HapiCostStats::add(c.flag_n, c.flag_us, tc); tc = CmiWallTimer(); }
    hapi_flag_writes_total.fetch_add(1, std::memory_order_relaxed);
    hapiTraceFlag((void*)stream, seq);
    hapiEvent hev(NULL, cb, cb_msg, wr);
    hev.flag_seq = seq;
    CpvAccess(hapi_flag_inflight)++;
    hapiQueueFor(stream).push(hev);
    CpvAccess(n_hapi_events)++;
    if (cost) { HapiCostStats& c = HapiCostStats::get(); HapiCostStats::add(c.push_n, c.push_us, tc); }
    return;
  }

  // An event is recorded directly, so it must not overtake this PE's queued
  // work on the same stream (a receive copy still in a submitter ring): the
  // callback behind it would fire before the copy had even been issued.
  hapiSubmitDrain();

  if (hapi_use_flag_poll) {
    // Said once per PE: an event here costs a create (until the pool has
    // grown), a record, and a driver query on every scheduler loop until it
    // completes -- see HAPI_FLAG_SLOTS.
    static thread_local bool warned_ring = false, warned_dev = false;
    if (use_dev != CpvAccess(my_device)) {
      if (!warned_dev) {
        warned_dev = true;
        CmiPrintf("HAPI[pe=%d]: callback on a stream of device %d, not this PE's %d: "
                  "completion by CUDA event instead of the pinned flag (reported once)\n",
                  CmiMyPe(), use_dev, CpvAccess(my_device));
      }
    } else if (!warned_ring) {
      warned_ring = true;
      CmiPrintf("HAPI[pe=%d]: pinned-flag ring full (%d in flight): completion by CUDA "
                "event until it drains; raise HAPI_FLAG_SLOTS (reported once)\n",
                CmiMyPe(), CpvAccess(hapi_flag_inflight));
    }
  }
  if (use_dev != prev_dev) hapiCheck(hapiSetDevice(use_dev));

  hapiEvent_t ev;
  bool was_fresh = false;
  auto& hapi_event_pool_local = CpvAccess(hapi_event_pool)[use_dev];
  if(hapi_event_pool_local.size() == 0) {
    was_fresh = true;
  #if CMK_LBDB_ON
    hapiEventCreateWithFlags(&ev, hapiEventDefault);
  #else
    hapiEventCreateWithFlags(&ev, hapiEventDisableTiming);
  #endif
  } else {
    ev = hapi_event_pool_local.front();
    hapi_event_pool_local.pop();
  }
  // Checked, and loudly. This was unchecked, so a failure here left
  // cudaErrorInvalidResourceHandle as the thread's sticky error and the next
  // cudaPeekAtLastError in application code aborted on a fault it did not
  // cause. An event and a stream must belong to the same device; the pool is
  // per-PE, so a mismatch means the event outlived the device context it was
  // made for.
  // Checked, and self-healing. This was unchecked, so a failure left
  // cudaErrorInvalidResourceHandle as the thread's sticky error and the next
  // cudaPeekAtLastError in application code aborted on a fault it did not
  // cause -- which is how this surfaced: as a CUDA error reported by an
  // unrelated chare's kernel launch, hundreds of steps after a migration.
  //
  // A recycled event can become unusable (it belongs to a device context that
  // is no longer the right one for this stream). Recording on it is then
  // permanently impossible, and since the event goes back to the pool
  // afterwards the failure repeats on every callback for the rest of the run.
  // Drop such an event and record on a fresh one instead: the pool must never
  // hand out an event that cannot be recorded.
  {
    hapiError_t rec = hapiEventRecord(ev, stream);
    if (rec != hapiSuccess) {
      hapiGetLastError();               // do not leave it for a bystander
      if (!was_fresh) {
        hapiEventDestroy(ev);
        hapiGetLastError();
#if CMK_LBDB_ON
        hapiEventCreateWithFlags(&ev, hapiEventDefault);
#else
        hapiEventCreateWithFlags(&ev, hapiEventDisableTiming);
#endif
        rec = hapiEventRecord(ev, stream);
      }
      if (rec != hapiSuccess) {
        hapiGetLastError();
        CmiAbort("hapiAddCallback: cannot record a completion event on this "
                 "stream (%s)", hapiGetErrorName(rec));
      }
    }
  }

  if (use_dev != prev_dev) hapiCheck(hapiSetDevice(prev_dev));

  hapiEvent hev(ev, cb, cb_msg, wr);

  // push event information in this stream's queue
  hapiEventQueues& queues = CpvAccess(hapi_event_queue);
  std::queue<hapiEvent>* q = nullptr;
  for (auto& entry : queues)
    if (entry.first == stream) { q = &entry.second; break; }
  if (q == nullptr) {
    queues.emplace_back(stream, std::queue<hapiEvent>());
    q = &queues.back().second;
  }
  q->push(hev);

  // increase count so that scheduler can poll the queue
  CpvAccess(n_hapi_events)++;
}
#endif

inline static void hapiWorkRequestCleanup(hapiWorkRequest* wr) {
  GPUManager& csv_gpu_manager = CsvAccess(gpu_manager);

#if CMK_SMP
  CmiLock(csv_gpu_manager.progress_lock_);
#endif

  // free device buffers
  csv_gpu_manager.freeBuffers(wr);

#if CMK_SMP
  CmiUnlock(csv_gpu_manager.progress_lock_);
#endif

  // free hapiWorkRequest
  delete wr;
}

#ifdef HAPI_CUDA_CALLBACK
// Invokes user's host-to-device callback.
static void* hostToDeviceCallback(void* arg) {
#if CMK_NVTX
  NVTXTracer nvtx_range("hostToDeviceCallback", NVTXColor::Asbestos);
#endif
  hapiWorkRequest* wr = *((hapiWorkRequest**)((char*)arg + CmiMsgHeaderSizeBytes + sizeof(int)));
  wr->host_to_device_cb.send();

  // inform QD that the host-to-device transfer is complete
  CmiAssert(hapiQdProcess);
  hapiQdProcess(1);

  return NULL;
}

// Invokes user's kernel execution callback.
static void* kernelCallback(void* arg) {
#if CMK_NVTX
  NVTXTracer nvtx_range("kernelCallback", NVTXColor::Asbestos);
#endif
  hapiWorkRequest* wr = *((hapiWorkRequest**)((char*)arg + CmiMsgHeaderSizeBytes + sizeof(int)));
  wr->kernel_cb.send();

  // inform QD that the kernel is complete
  CmiAssert(hapiQdProcess);
  hapiQdProcess(1);

  return NULL;
}

// Frees device buffers and invokes user's device-to-host callback.
// Invoked regardless of the availability of the user's callback.
static void* deviceToHostCallback(void* arg) {
#if CMK_NVTX
  NVTXTracer nvtx_range("deviceToHostCallback", NVTXColor::Asbestos);
#endif
  hapiWorkRequest* wr = *((hapiWorkRequest**)((char*)arg + CmiMsgHeaderSizeBytes + sizeof(int)));
  wr->device_to_host_cb.send();

  hapiWorkRequestCleanup(wr);

  // inform QD that device-to-host transfer is complete
  CmiAssert(hapiQdProcess);
  hapiQdProcess(1);

  return NULL;
}

// Used by lightweight HAPI.
static void* lightCallback(void *arg) {
#if CMK_NVTX
  NVTXTracer nvtx_range("lightCallback", NVTXColor::Asbestos);
#endif

  hapiCallbackMessage* conv_msg = (hapiCallbackMessage*)arg;

  // invoke user callback
  conv_msg->cb.send(conv_msg->cb_msg);

  // notify process to QD
  CmiAssert(hapiQdProcess);
  hapiQdProcess(1);

  return NULL;
}
#endif // HAPI_CUDA_CALLBACK

// Register callback functions. All PEs need to call this.
static void hapiRegisterCallbacks() {
#ifdef HAPI_CUDA_CALLBACK
  GPUManager& csv_gpu_manager = CsvAccess(gpu_manager);

  // FIXME: Potential race condition on assignments, but CmiAssignOnce
  // causes a hang at startup.
  csv_gpu_manager.host_to_device_cb_idx_
    = CmiRegisterHandler((CmiHandler)hostToDeviceCallback);
  csv_gpu_manager.kernel_cb_idx_
    = CmiRegisterHandler((CmiHandler)kernelCallback);
  csv_gpu_manager.device_to_host_cb_idx_
    = CmiRegisterHandler((CmiHandler)deviceToHostCallback);
  csv_gpu_manager.light_cb_idx_
    = CmiRegisterHandler((CmiHandler)lightCallback);
#endif
}

#ifdef HAPI_CUDA_CALLBACK
// Callback function invoked by the CUDA runtime certain parts of GPU work are
// complete. It sends a converse message to the original PE to free the relevant
// device memory and invoke the user's callback. The reason for this method is
// that a thread created by the CUDA runtime does not have access to any of the
// CpvDeclare'd variables as it is not one of the threads created by the Charm++
// runtime.
static void CUDACallback(void *data) {
#if CMK_NVTX
  NVTXTracer nvtx_range("CUDACallback", NVTXColor::Silver);
#endif

  // send message to the original PE
  char *conv_msg = (char*)data;
  int dstRank = *((int *)(conv_msg + CmiMsgHeaderSizeBytes));
  CmiPushPE(dstRank, conv_msg);
}

enum CallbackStage {
  AfterHostToDevice,
  AfterKernel,
  AfterDeviceToHost
};

static void addCallback(hapiWorkRequest *wr, CallbackStage stage) {
  GPUManager& csv_gpu_manager = CsvAccess(gpu_manager);

  // create converse message to be delivered to this PE after CUDA callback
  char *conv_msg = (char *)CmiAlloc(CmiMsgHeaderSizeBytes + sizeof(int) +
                                  sizeof(hapiWorkRequest *)); // FIXME memory leak?
  *((int *)(conv_msg + CmiMsgHeaderSizeBytes)) = CmiMyRank();
  *((hapiWorkRequest **)(conv_msg + CmiMsgHeaderSizeBytes + sizeof(int))) = wr;

  int handlerIdx;
  switch (stage) {
    case AfterHostToDevice:
      handlerIdx = csv_gpu_manager.host_to_device_cb_idx_;
      break;
    case AfterKernel:
      handlerIdx = csv_gpu_manager.kernel_cb_idx_;
      break;
    case AfterDeviceToHost:
      handlerIdx = csv_gpu_manager.device_to_host_cb_idx_;
      break;
    default: // wrong type
      CmiFree(conv_msg);
      return;
  }
  CmiSetHandler(conv_msg, handlerIdx);

  // add callback into CUDA stream
  hapiCheck(hapiLaunchHostFunc(wr->stream, CUDACallback, (void*)conv_msg));
}
#endif // HAPI_CUDA_CALLBACK

/******************** DEPRECATED ********************/
// User calls this function to offload work to the GPU.
void hapiEnqueue(hapiWorkRequest* wr) {
#if CMK_NVTX
  NVTXTracer nvtx_range("enqueue", NVTXColor::Pomegranate);
#endif

  GPUManager& csv_gpu_manager = CsvAccess(gpu_manager);

#if CMK_SMP
  CmiLock(csv_gpu_manager.progress_lock_);
#endif

  // allocate device memory
  csv_gpu_manager.allocateBuffers(wr);

  // transfer data to device
  csv_gpu_manager.hostToDeviceTransfer(wr);

  // add host-to-device transfer callback
  if (wr->host_to_device_cb_set) {
    // while there is an ongoing workrequest, quiescence should not be detected
    // even if all PEs seem idle
    CmiAssert(hapiQdCreate);
    hapiQdCreate(1);

#ifdef HAPI_CUDA_CALLBACK
    addCallback(wr, AfterHostToDevice);
#else
    recordEvent(wr->stream, wr->host_to_device_cb, NULL);
#endif
  }

  // run kernel
  csv_gpu_manager.runKernel(wr);

  // add kernel callback
  if (wr->kernel_cb_set) {
    CmiAssert(hapiQdCreate);
    hapiQdCreate(1);

#ifdef HAPI_CUDA_CALLBACK
    addCallback(wr, AfterKernel);
#else
    recordEvent(wr->stream, wr->kernel_cb, NULL);
#endif
  }

  // transfer data to host
  csv_gpu_manager.deviceToHostTransfer(wr);

  // add device-to-host transfer callback
  CmiAssert(hapiQdCreate);
  hapiQdCreate(1);
#ifdef HAPI_CUDA_CALLBACK
  // always invoked to free memory
  addCallback(wr, AfterDeviceToHost);
#else
  if (wr->device_to_host_cb_set) {
    recordEvent(wr->stream, wr->device_to_host_cb, NULL, wr);
  }
  else {
    recordEvent(wr->stream, CkCallback::ignore, NULL, wr);
  }
#endif

#if CMK_SMP
  CmiUnlock(csv_gpu_manager.progress_lock_);
#endif
}

/******************** DEPRECATED ********************/
// Creates a hapiWorkRequest object on the heap and returns it to the user.
hapiWorkRequest* hapiCreateWorkRequest() {
  return (new hapiWorkRequest);
}

hapiWorkRequest::hapiWorkRequest() :
    grid_dim(0), block_dim(0), shared_mem(0), runKernel(NULL), state(0),
    user_data(NULL), free_user_data(false)
{
#ifdef HAPI_TRACE
  trace_name = "";
#endif
#ifdef HAPI_INSTRUMENT_WRS
  chare_index = -1;
#endif

  // Use CUDA per-thread default stream
  stream = hapiStreamPerThread;

  // Charm++ callbacks are not set by default
  host_to_device_cb = CkCallback(CkCallback::ignore);
  host_to_device_cb_set = false;
  kernel_cb = CkCallback(CkCallback::ignore);
  kernel_cb_set = false;
  device_to_host_cb = CkCallback(CkCallback::ignore);
  device_to_host_cb_set = false;
}

void hapiWorkRequestSetCallback(hapiWorkRequest* wr, void* cb) {
  wr->setCallback(*(CkCallback*)cb);
}

static void shmInit() {
  if (!CsvAccess(gpu_manager).use_shm) return;

  if (CmiMyRank() == 0) {
    if (!CmiInCommThread()) shmSetup();
    if (CmiMyNodeRankLocal() == 0) {
      if (!CmiInCommThread()) shmCreate(); // Create a per-host shared memory region
      CmiBarrier(); // FIXME: Only needs to be a host-wide barrier
    } else {
      CmiBarrier();
      if (!CmiInCommThread()) shmOpen(); // Open the shared memory region created by local logical node 0
    }
    if (!CmiInCommThread()) shmMap(); // Map the shared memory file into memory
  } else {
    CmiBarrier();
  }

  if (!CmiInCommThread()) CmiNodeBarrier(); // Ensure shared memory has been mapped into the logical node

  // Once per device, not once per PE. Everything ipcHandleCreate does is
  // per-device state living in csv_gpu_manager and in shared memory: the
  // device's memory handle, and hapi_ipc_event_pool_size_total events pushed
  // into hapi_ipc_device_infos. Running it on every PE sharing a device pushed
  // the pool twice and overwrote the shared-memory event handles, so the events
  // a peer opened were not the ones the local side went on to index.
  if (!CmiInCommThread() && CpvAccess(device_rep)) ipcHandleCreate();

  // Ensure CUDA IPC handles are available for all processes
  // Note: Causes a hang when this barrier is placed after CPU topology initialization
  // FIXME: This only needs to be a host-wide synchronization
  CmiBarrier();

  if (CmiMyRank() == 0) {
    if (!CmiInCommThread()) ipcHandleOpen(); // Open CUDA IPC handles for accessing other processes' device memory
  }
}

static void shmSetup() {
  GPUManager& csv_gpu_manager = CsvAccess(gpu_manager);

  // Set up shared memory file name: per host, job and job step. A run killed
  // by hand or by a timeout leaves its segment behind with live state (IPC
  // event slots, the pool entry of dead processes), and a later step of the
  // same host that opened it by the old host-only name hung before its first
  // step with every PE idle (2026-10-21, job 23078963, after the channel
  // segment had been given the same per-step name the night before).
  csv_gpu_manager.shm_name.assign("charm-hapi-host");
  int host_id = CmiPhysicalNodeID(CmiMyPe());
  csv_gpu_manager.shm_name.append(std::to_string(host_id));
  {
    const char* job = getenv("SLURM_JOB_ID");
    const char* step = getenv("SLURM_STEP_ID");
    csv_gpu_manager.shm_name.append("-" + (job ? std::string(job) : std::to_string((long)getppid())));
    if (step) csv_gpu_manager.shm_name.append("-" + std::string(step));
  }
  const char* shm_name = csv_gpu_manager.shm_name.c_str();

  // Calculate shared memory region size
  csv_gpu_manager.shm_chunk_size = sizeof(hapiIpcMemHandle_t) +
      sizeof(hapi_ipc_event_shared) * csv_gpu_manager.hapi_ipc_event_pool_size_total +
      sizeof(hapi_pool_shm_entry);
  csv_gpu_manager.shm_size = csv_gpu_manager.shm_chunk_size *
    csv_gpu_manager.device_count * ((CmiNumNodes() / CmiNumPhysicalNodes()));
}

// Create POSIX shared memory region accessible to all processes on the same host
// Invoked by PE rank 0 of local logical node 0 (1 PE per host)
static void shmCreate() {
  GPUManager& csv_gpu_manager = CsvAccess(gpu_manager);

  // Remove the shared memory file if it exists (could be left over from a
  // previous run that exited abnormally)
  struct stat stat_result;
  std::string stat_path("/dev/shm/");
  stat_path.append(csv_gpu_manager.shm_name);
  if (stat(stat_path.c_str(), &stat_result) == 0) {
    if (remove(stat_path.c_str())) {
      CmiAbort("Failure during shared memory file removal");
    }
  }

  // Create the shared memory file
  csv_gpu_manager.shm_file = shm_open(csv_gpu_manager.shm_name.c_str(),
      O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
  if (csv_gpu_manager.shm_file < 0) {
    CmiError("Failure at shm_open");
    shmAbort();
  }

  // Set it to the appropriate size
  if (ftruncate(csv_gpu_manager.shm_file, 0) != 0) {
    CmiError("Failure at ftruncate");
    shmAbort();
  }
  if (ftruncate(csv_gpu_manager.shm_file, csv_gpu_manager.shm_size) != 0) {
    CmiError("Failure at ftruncate");
    shmAbort();
  }

  // Busywait until file is properly sized
  struct stat shm_file_stat;
  do {
    if (fstat(csv_gpu_manager.shm_file, &shm_file_stat) != 0) {
      CmiError("Failure at fstat");
      shmAbort();
    }
  } while (shm_file_stat.st_size != csv_gpu_manager.shm_size);
}

// Open POSIX shared memory region
// Invoked by logical nodes other than local rank 0
static void shmOpen() {
  GPUManager& csv_gpu_manager = CsvAccess(gpu_manager);

  // Open the shared memory file
  csv_gpu_manager.shm_file = shm_open(csv_gpu_manager.shm_name.c_str(),
      O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
  if (csv_gpu_manager.shm_file < 0) {
    CmiError("Failure at shm_open");
    shmAbort();
  }

  // Busywait until file is properly sized
  struct stat shm_file_stat;
  do {
    if (fstat(csv_gpu_manager.shm_file, &shm_file_stat) != 0) {
      CmiError("Failure at fstat");
      shmAbort();
    }
  } while (shm_file_stat.st_size != csv_gpu_manager.shm_size);
}

static void shmMap() {
  GPUManager& csv_gpu_manager = CsvAccess(gpu_manager);

  // Map shared memory file into memory
  csv_gpu_manager.shm_ptr = mmap(NULL, csv_gpu_manager.shm_size,
      PROT_READ | PROT_WRITE, MAP_SHARED, csv_gpu_manager.shm_file, 0);
  if (csv_gpu_manager.shm_ptr == (void*)-1) {
    CmiError("Failure at mmap");
    shmAbort();
  }

  // Store pointer to my process' portion of the shared memory region
  csv_gpu_manager.shm_my_ptr = (void*)((char*)csv_gpu_manager.shm_ptr +
      csv_gpu_manager.shm_chunk_size * (csv_gpu_manager.device_count *
      CmiMyNodeRankLocal()));

  // Allocate memory for local storage
  for (int i = 0; i < csv_gpu_manager.device_count * ((CmiNumNodes() / CmiNumPhysicalNodes())); i++) {
    csv_gpu_manager.hapi_ipc_device_infos.emplace_back();
  }
}

static void shmAbort() {
  shmCleanup();
  CmiAbort("Failure in shared memory initialization");
}

// Clean up shared memory region
// Invoked by PE rank 0 of each process
static void shmCleanup() {
  GPUManager& csv_gpu_manager = CsvAccess(gpu_manager);
  if (!csv_gpu_manager.use_shm) return;

  if (csv_gpu_manager.shm_ptr != NULL) {
    munmap(csv_gpu_manager.shm_ptr, csv_gpu_manager.shm_size);
  }

  if (csv_gpu_manager.shm_file != -1) {
    close(csv_gpu_manager.shm_file);
  }

  if (!csv_gpu_manager.shm_name.empty()) {
    shm_unlink(csv_gpu_manager.shm_name.c_str());
    csv_gpu_manager.shm_name.clear();
  }
}

// Create CUDA IPC handles and populate shared memory region
// Invoked by all PEs
static void ipcHandleCreate() {
  // Only device reps should continue to perform the following operations
  // so that they are done only once per device
  if (!CpvAccess(device_rep)) return;

  GPUManager& csv_gpu_manager = CsvAccess(gpu_manager);
  int& cpv_my_device_id = CpvAccess(my_device_id);

  // Create CUDA IPC memory handle in shared memory
  auto it = csv_gpu_manager.device_map.find(CmiMyPe());
  if (it == csv_gpu_manager.device_map.end()) {
    CmiAbort("PE not found in device_map during ipcHandleCreate");
  }
  DeviceManager& my_dm = *(it->second);
  const bool pool_mode = csv_gpu_manager.device_pool_on;
  auto comm_buffer = my_dm.get_comm_buffer();
  if (!pool_mode) CmiAssert(comm_buffer);

  // Use local device index (0 to device_count-1) for shm_mem_handle offset
  // int local_device_idx = my_dm.local_index;
  hapiIpcMemHandle_t* shm_mem_handle = (hapiIpcMemHandle_t*)((char*)csv_gpu_manager.shm_my_ptr +
      csv_gpu_manager.shm_chunk_size * cpv_my_device_id);

  // Pool mode has no comm buffer: its handle slot stays zeroed and unused, and
  // the pool entry after the event slots carries the arena every peer opens.
  void* device_ptr = pool_mode ? nullptr : comm_buffer->base_ptr;
  if (pool_mode) memset(shm_mem_handle, 0, sizeof(*shm_mem_handle));
  else hapiCheck(hapiIpcGetMemHandle(shm_mem_handle, device_ptr));

  // Publish the device pool's first arena beside the comm buffer's handle, so
  // peers open it now rather than on the first send from it. Pool mode only.
  {
    const bool preopen = pool_mode;
    hapi_pool_shm_entry* pe_entry = (hapi_pool_shm_entry*)((char*)shm_mem_handle +
        sizeof(hapiIpcMemHandle_t) +
        sizeof(hapi_ipc_event_shared) * csv_gpu_manager.hapi_ipc_event_pool_size_total);
    memset(pe_entry, 0, sizeof(*pe_entry));
    pe_entry->valid = 0;
    if (preopen) {
      void* base = nullptr; size_t extent = 0;
      hapiDevPoolEnsureArena(cpv_my_device_id, &base, &extent);
      pe_entry->src_node = CmiMyNode();
      if (hapiDevPoolIsVmm()) {
        // The heap: its chunk descriptors, for peers to mirror (kind 1).
        const int shm_index = csv_gpu_manager.device_count * CmiMyNodeRankLocal() + cpv_my_device_id;
        if (!hapiDevPoolVmmPublish(cpv_my_device_id, pe_entry, shm_index, &pe_entry->handle))
          CmiAbort("HAPI> [%d] device pool (vmm): no heap to publish", CmiMyPe());
        CmiLock(csv_gpu_manager.ipc_cache_lock);   // several reps per process seed this concurrently
        csv_gpu_manager.ipc_export_cache[(const void*)base] = pe_entry->handle;
        CmiUnlock(csv_gpu_manager.ipc_cache_lock);
        pe_entry->valid = 1;
        CmiPrintf("HAPI> [%d] device pool (vmm) heap %p (%zu MB mapped of %zu MB) published "
                  "for pre-open, %d chunk descriptor(s)\n", CmiMyPe(), base, extent >> 20,
                  pe_entry->extent >> 20, pe_entry->n_chunks);
      } else {
        hapiCheck(hapiIpcGetMemHandle(&pe_entry->handle, base));
        pe_entry->base = base;
        pe_entry->extent = extent;
        // Our own export, seeded: the first send from this arena finds it.
        CmiLock(csv_gpu_manager.ipc_cache_lock);   // several reps per process seed this concurrently
        csv_gpu_manager.ipc_export_cache[(const void*)base] = pe_entry->handle;
        CmiUnlock(csv_gpu_manager.ipc_cache_lock);
        pe_entry->valid = 1;
        CmiPrintf("HAPI> [%d] device pool arena %p (%zu MB) published for pre-open\n",
                  CmiMyPe(), base, extent >> 20);
      }
    }
  }

  // Create CUDA IPC events and store them locally (in hapi_ipc_device_info),
  // and create corresponding IPC handles in shared memory
  hapi_ipc_device_info& my_device_info = csv_gpu_manager.hapi_ipc_device_infos[csv_gpu_manager.device_count * CmiMyNodeRankLocal() + cpv_my_device_id];

  hapi_ipc_event_shared* shm_event_shared = (hapi_ipc_event_shared*)((char*)shm_mem_handle + sizeof(hapiIpcMemHandle_t));

  // Each slot's flags live in the shared-memory region and are written by BOTH
  // the owning process and whichever peer receives from it. Only the slot's
  // owner initializes them, before any peer can reach them: the CmiBarrier
  // between ipcHandleCreate and ipcHandleOpen orders that.
  //
  // These were once guarded by a per-slot pthread mutex, which had to be
  // created with PTHREAD_PROCESS_SHARED -- mmap'd memory merely starts zeroed,
  // which is not a valid initialized mutex, so locking it from the peer was
  // undefined behavior and segfaulted on the first cross-process transfer.
  // dst_flag is now a lock-free atomic (see hapi_ipc_event_shared), which needs
  // no initialization ritual beyond the store below and cannot reintroduce that
  // failure mode.
  for (int i = 0; i < csv_gpu_manager.hapi_ipc_event_pool_size_total; i++) {
    hapi_ipc_event_shared* cur_shm_event_shared = shm_event_shared + i;

    cur_shm_event_shared->src_ready.store(false, std::memory_order_relaxed);
    cur_shm_event_shared->dst_flag.store(false, std::memory_order_relaxed);

    my_device_info.event_pool_flags.push_back(0);
    my_device_info.event_pool_buff_offsets.push_back(0);
    my_device_info.src_event_pool.emplace_back();
    my_device_info.dst_event_pool.emplace_back();
    hapiCheck(hapiEventCreateWithFlags(&my_device_info.src_event_pool[i],
          hapiEventDisableTiming | hapiEventInterprocess));
    hapiCheck(hapiEventCreateWithFlags(&my_device_info.dst_event_pool[i],
          hapiEventDisableTiming | hapiEventInterprocess));
    hapiCheck(hapiIpcGetEventHandle(&cur_shm_event_shared->src_event_handle,
          my_device_info.src_event_pool[i]));
    hapiCheck(hapiIpcGetEventHandle(&cur_shm_event_shared->dst_event_handle,
          my_device_info.dst_event_pool[i]));
  }

  // Self-map this device's comm buffer in the peer table. ipcHandleOpen skips
  // the owning process (a process cannot cudaIpcOpen its own handle), so this
  // assignment is the only thing that ever populates an own-device entry -- and
  // a STAGED payload delivered same-process (an unconfirmed destination that
  // stayed local, or a target that migrated home) reads exactly this entry. The
  // events in the slot are already the local originals, so the mapping is the
  // only piece that would otherwise be missing.
  my_device_info.buffer = device_ptr;
}

// Open CUDA IPC handles created by other processes
// Invoked by PE rank 0 of each process
static void ipcHandleOpen() {
  GPUManager& csv_gpu_manager = CsvAccess(gpu_manager);

  // Loop through all processes on this host
  for (int i = 0; i < CmiNumNodes() / CmiNumPhysicalNodes(); i++) {
    if (i == CmiMyNodeRankLocal()) continue;

    // Loop through GPU devices per process
    for (int j = 0; j < csv_gpu_manager.device_count; j++) {
      int device_index = csv_gpu_manager.device_count * i + j;
      hapi_ipc_device_info& cur_device_info = csv_gpu_manager.hapi_ipc_device_infos[device_index];

      // Open memory handle
      hapiIpcMemHandle_t* shm_mem_handle =
        (hapiIpcMemHandle_t*)((char*)csv_gpu_manager.shm_ptr
            + csv_gpu_manager.shm_chunk_size * device_index);
      // Pool mode published no comm buffer; nothing is ever staged, so no
      // receive reads through this mapping.
      if (csv_gpu_manager.device_pool_on)
        cur_device_info.buffer = nullptr;
      else
        hapiCheck(hapiIpcOpenMemHandle(&cur_device_info.buffer, *shm_mem_handle,
              hapiIpcMemLazyEnablePeerAccess));

      // The peer's published pool arena, if any: open it once and seed the
      // import cache under the same key the send path will look up
      // (owner's node id, arena base), so that lookup hits from the start.
      {
        hapi_pool_shm_entry* pe_entry = (hapi_pool_shm_entry*)((char*)shm_mem_handle +
            sizeof(hapiIpcMemHandle_t) +
            sizeof(hapi_ipc_event_shared) * csv_gpu_manager.hapi_ipc_event_pool_size_total);
        if (pe_entry->valid) {
          void* mapped = hapiIpcImportBuffer(pe_entry->handle, pe_entry->src_node,
                                             pe_entry->base, pe_entry->extent);
          CmiPrintf("HAPI> [%d] pre-opened peer node %d's pool %s %p (%zu MB) at %p\n",
                    CmiMyPe(), pe_entry->src_node, pe_entry->kind == 1 ? "heap" : "arena",
                    pe_entry->base, pe_entry->extent >> 20, mapped);
        }
      }

      // Open event handles
      hapi_ipc_event_shared* shm_event_shared =
        (hapi_ipc_event_shared*)((char*)shm_mem_handle + sizeof(hapiIpcMemHandle_t));

      cur_device_info.event_pool_flags.clear();
      cur_device_info.event_pool_buff_offsets.clear();

      // Open the peer's event handles under OUR current device. Do NOT switch to
      // the exporter's device around this: that was tried and it breaks all
      // cross-process transfers (git bisect, commit bec910fef). cudaIpcOpen*
      // is defined in terms of the caller's context, and the imported event is
      // usable from here without any device change.
      for (int k = 0; k < csv_gpu_manager.hapi_ipc_event_pool_size_total; k++) {
        hapi_ipc_event_shared* cur_shm_event_shared = shm_event_shared + k;

        cur_device_info.src_event_pool.emplace_back();
        cur_device_info.dst_event_pool.emplace_back();
        hapiCheck(hapiIpcOpenEventHandle(&cur_device_info.src_event_pool[k],
              cur_shm_event_shared->src_event_handle));
        hapiCheck(hapiIpcOpenEventHandle(&cur_device_info.dst_event_pool[k],
              cur_shm_event_shared->dst_event_handle));
      }

    }
  }
}

/*** Direct CUDA IPC transport: export and import caches ***/

bool hapiIpcUseDirect() {
  return CsvAccess(gpu_manager).ipc_use_direct;
}

bool hapiDevPoolOn() {
  return CsvAccess(gpu_manager).device_pool_on;
}

// +gpupoolalloc vmm: cudaIpcGetMemHandle cannot name cuMemMap'd memory, so a
// direct send from the vmm heap carries this instead, in the same 64 bytes. It
// names the owner's whole heap: one mirror of the reserved range reaches every
// chunk, and the chunk file descriptors travel through the owner's +gpushm
// entry (hapi_pool_shm_entry), duplicated by the peer with pidfd_getfd.
struct HapiVmmIpcHandle {
  uint64_t magic, base, reserve, chunk;
  int32_t pid, shm_index, src_node, pad;
  uint8_t rest[16];
};
static_assert(sizeof(HapiVmmIpcHandle) == sizeof(hapiIpcMemHandle_t),
              "the vmm heap handle must overlay a CUDA IPC handle");
static const uint64_t HAPI_VMM_IPC_MAGIC = 0x564D4D2D48415049ull;   // "VMM-HAPI"

static bool hapiVmmHandleIs(const hapiIpcMemHandle_t& h) {
  HapiVmmIpcHandle v;
  memcpy(&v, &h, sizeof(v));
  return v.magic == HAPI_VMM_IPC_MAGIC;
}
static void hapiVmmMakeHandle(hapiIpcMemHandle_t* h, uint64_t base, uint64_t reserve,
                              uint64_t chunk, int pid, int shm_index, int src_node) {
  HapiVmmIpcHandle v;
  memset(&v, 0, sizeof(v));
  v.magic = HAPI_VMM_IPC_MAGIC;
  v.base = base; v.reserve = reserve; v.chunk = chunk;
  v.pid = pid; v.shm_index = shm_index; v.src_node = src_node;
  memcpy(h, &v, sizeof(v));
}

// A peer's vmm heap as mapped here: the mirror of its reserved range, with the
// chunks it has published so far mapped at their own offsets. Grows as the
// owner publishes more; never shrinks (the owner never unmaps).
struct HapiVmmMirror {
  uintptr_t base = 0;
  size_t reserve = 0, chunk = 0;
  int pid = -1, pidfd = -1;
  hapi_pool_shm_entry* entry = nullptr;
  size_t mapped = 0;   // chunks
  std::vector<hapiMemGenericAllocationHandle> handles;
};
static std::map<std::pair<int, const void*>, HapiVmmMirror> hapi_vmm_mirrors;  // under ipc_cache_lock

static const char* hapiCuErrNameIpc(hapiDrvResult r) {
  const char* s = nullptr;
  hapiDrvGetErrorName(r, &s);
  return s ? s : "CUDA_ERROR_?";
}

// cuMemSetAccess for every device this process drives, the owner first: one descriptor per
// local device. A same-process send between two of the process's GPUs reads the source from
// the destination's device, and peer access (cudaDeviceEnablePeerAccess) does not cover vmm
// mappings. Where the whole set is refused (a pair without P2P) the owner alone is granted, so
// a one-device process behaves exactly as before.
// phys_dev: the device whose physical memory this is (the exporter's, for an import; the owner's
// otherwise). HIP maps another device's memory into a device's address space only once peer
// access from that device to the memory's device is enabled (cuMemSetAccess alone is refused
// with "peer access between the devices is required", MI100 2026-10-21), and a device that
// cannot reach it is left out of the grant.
static hapiDrvResult hapiVmmSetAccessAllLocal(uintptr_t at, size_t bytes, int owner_dev,
                                         const char* what, int phys_dev = -1) {
  if (phys_dev < 0) phys_dev = owner_dev;
  const std::vector<DeviceManager>& dms = CsvAccess(gpu_manager).device_managers;
  std::vector<hapiMemAccessDesc> acc;
  auto add = [&](int dev) {
    for (const hapiMemAccessDesc& a : acc) if (a.location.id == dev) return;
#if CMK_HIP
    if (dev != phys_dev) {
      int can = 0;
      if (hapiDeviceCanAccessPeer(&can, dev, phys_dev) != hapiSuccess || !can) {
        static bool said = false;
        if (!said) {
          said = true;
          CmiPrintf("HAPI> [%d] %s: device %d cannot access device %d's memory as a peer; "
                    "it is left out of the grant\n", CmiMyPe(), what, dev, phys_dev);
        }
        hapiGetLastError();
        return;
      }
      int saved = 0;
      hapiGetDevice(&saved);
      hapiSetDevice(dev);
      const hapiError_t pe = hapiDeviceEnablePeerAccess(phys_dev, 0);
      if (pe != hapiSuccess && pe != hapiErrorPeerAccessAlreadyEnabled)
        CmiPrintf("HAPI> [%d] %s: enabling peer access from device %d to device %d: %s\n",
                  CmiMyPe(), what, dev, phys_dev, hapiGetErrorString(pe));
      hapiGetLastError();
      hapiSetDevice(saved);
    }
#endif
    hapiMemAccessDesc d;
    memset(&d, 0, sizeof(d));
    d.location.type = hapiMemLocationTypeDevice;
    d.location.id = dev;
    d.flags = hapiMemAccessFlagsProtReadWrite;
    acc.push_back(d);
  };
  add(owner_dev);
  for (const DeviceManager& dm : dms) add(dm.global_index);
  hapiDrvResult r = hapiDrvMemSetAccess(at, bytes, acc.data(), acc.size());
  if (r != hapiDrvSuccess && acc.size() > 1) {
    static bool said = false;
    if (!said) {
      said = true;
      CmiPrintf("HAPI> [%d] %s: cuMemSetAccess for %zu devices: %s; granting the owner (device "
                "%d) only -- same-process cross-device reads of this memory will fault\n",
                CmiMyPe(), what, acc.size(), hapiCuErrNameIpc(r), owner_dev);
    }
    r = hapiDrvMemSetAccess(at, bytes, acc.data(), 1);
  }
  return r;
}

// Map the peer heap named by `handle` (a HapiVmmIpcHandle) and return where
// its base is reachable from this process; every chunk the owner has
// published is mapped, so any offset into it addresses. Called with
// ipc_cache_lock held.
static void* hapiVmmImportLocked(const hapiIpcMemHandle_t& handle, int src_process,
                                 const void* src_base) {
  GPUManager& gm = CsvAccess(gpu_manager);
  HapiVmmIpcHandle v;
  memcpy(&v, &handle, sizeof(v));
  const auto key = std::make_pair(src_process, src_base);
  auto it = hapi_vmm_mirrors.find(key);
  if (it == hapi_vmm_mirrors.end()) {
    HapiVmmMirror m;
    m.reserve = v.reserve;
    m.chunk = v.chunk;
    m.pid = v.pid;
    m.entry = (hapi_pool_shm_entry*)((char*)gm.shm_ptr + gm.shm_chunk_size * v.shm_index +
        sizeof(hapiIpcMemHandle_t) +
        sizeof(hapi_ipc_event_shared) * gm.hapi_ipc_event_pool_size_total);
    if (m.entry->kind != 1 || m.entry->base != (void*)(uintptr_t)v.base)
      CmiAbort("HAPI> [%d] vmm import: the +gpushm entry %d does not describe heap %p of "
               "process %d (kind %d, base %p)", CmiMyPe(), v.shm_index,
               (void*)(uintptr_t)v.base, src_process, m.entry->kind, m.entry->base);
#ifdef SYS_pidfd_open
    m.pidfd = (int)syscall(SYS_pidfd_open, (pid_t)v.pid, 0);
#else
    m.pidfd = -1;
#endif
    if (m.pidfd < 0)
      CmiAbort("HAPI> [%d] vmm import: pidfd_open(%d) failed (%s); the vmm backend needs "
               "Linux >= 5.6", CmiMyPe(), v.pid, strerror(errno));
    hapiDrvResult r = hapiDrvMemAddressReserve(&m.base, m.reserve, 0, 0, 0);
    if (r != hapiDrvSuccess)
      CmiAbort("HAPI> [%d] vmm import: cuMemAddressReserve(%zu MB) for a peer heap: %s",
               CmiMyPe(), m.reserve >> 20, hapiCuErrNameIpc(r));
    it = hapi_vmm_mirrors.emplace(key, m).first;
    gm.ipc_import_misses.fetch_add(1, std::memory_order_relaxed);
  } else {
    gm.ipc_import_hits.fetch_add(1, std::memory_order_relaxed);
  }
  HapiVmmMirror& m = it->second;
  const int published = __atomic_load_n(&m.entry->n_chunks, __ATOMIC_ACQUIRE);
  int cur_dev = 0;
  if (m.mapped < (size_t)published) hapiCheck(hapiGetDevice(&cur_dev));
  for (; m.mapped < (size_t)published; m.mapped++) {
    const int theirfd = m.entry->fds[m.mapped];
#ifdef SYS_pidfd_getfd
    const int fd = (int)syscall(SYS_pidfd_getfd, m.pidfd, theirfd, 0);
#else
    const int fd = -1;
#endif
    if (fd < 0)
      CmiAbort("HAPI> [%d] vmm import: pidfd_getfd(pid %d, fd %d) failed (%s). The peer's "
               "chunk descriptor could not be duplicated; this needs Linux >= 5.6 and "
               "ptrace permission over same-user processes (kernel.yama.ptrace_scope <= 1)",
               CmiMyPe(), m.pid, theirfd, strerror(errno));
    hapiMemGenericAllocationHandle h;
    hapiDrvResult r = hapiDrvMemImportFromShareableHandle(&h, (void*)(uintptr_t)fd,
                                                hapiMemHandleTypePosixFd);
    if (r != hapiDrvSuccess)
      CmiAbort("HAPI> [%d] vmm import: cuMemImportFromShareableHandle(chunk %zu of process %d): %s",
               CmiMyPe(), m.mapped, src_process, hapiCuErrNameIpc(r));
    const uintptr_t at = m.base + (uintptr_t)(m.mapped * m.chunk);
    r = hapiDrvMemMap(at, m.chunk, 0, h, 0);
    if (r != hapiDrvSuccess)
      CmiAbort("HAPI> [%d] vmm import: cuMemMap(chunk %zu): %s", CmiMyPe(), m.mapped,
               hapiCuErrNameIpc(r));
    // The import cache is process-wide: a PE on another of this process's devices reuses
    // this mapping, so every local device gets access, not only the importing PE's.
    r = hapiVmmSetAccessAllLocal(at, m.chunk, cur_dev, "vmm import", m.entry->dev);
    // Closed only after the grant: ROCm resolves the imported descriptor at hipMemSetAccess and
    // refuses it with hipErrorInvalidValue once the fd is closed (ROCm 7.14, MI250X); CUDA does not care.
    close(fd);
    if (r != hapiDrvSuccess)
      CmiAbort("HAPI> [%d] vmm import: cuMemSetAccess(chunk %zu) from device %d: %s (peer "
               "access between the devices is required)", CmiMyPe(), m.mapped, cur_dev,
               hapiCuErrNameIpc(r));
    m.handles.push_back(h);
  }
  return (void*)m.base;
}

bool hapiIpcExportBuffer(const void* ptr, hapiIpcMemHandle_t* handle,
                         size_t* offset, void** base_out) {
  // A vmm pool block: the handle names the heap, the offset is from its base.
  if (hapiDevPoolIsVmm()) {
    void* hb = NULL;
    if (hapiDevPoolVmmHandleOf(ptr, handle, &hb)) {
      *offset = (size_t)((const char*)ptr - (const char*)hb);
      if (base_out != NULL) *base_out = hb;
      return true;
    }
  }
  // cudaIpcGetMemHandle names an allocation, and cudaIpcOpenMemHandle hands the
  // peer that allocation's base -- so an interior pointer has to be split into
  // (base, offset) here and reassembled on the far side.
  void* base = NULL;
  size_t alloc_size = 0;
  if (!hapiMemGetAddressRange(&base, &alloc_size, ptr)) return false;

  const void* base_ptr = (const void*)base;
  *offset = (size_t)((const char*)ptr - (const char*)base_ptr);
  // The receiver cannot address with this, but it is what lets it recognise a
  // stale mapping of a recycled base. See CmiNcpyBufferDevice::ipc_base.
  if (base_out != NULL) *base_out = base;

  GPUManager& csv_gpu_manager = CsvAccess(gpu_manager);
#if CMK_SMP
  CmiLock(csv_gpu_manager.ipc_cache_lock);
#endif
  auto it = csv_gpu_manager.ipc_export_cache.find(base_ptr);
  if (it != csv_gpu_manager.ipc_export_cache.end()) {
    *handle = it->second;
#if CMK_SMP
    CmiUnlock(csv_gpu_manager.ipc_cache_lock);
#endif
    return true;
  }

  hapiIpcMemHandle_t new_handle;
  const hapiError_t err = hapiIpcGetMemHandle(&new_handle, (void*)base_ptr);
  if (err != hapiSuccess) {
    // Not an exportable allocation (managed or host-registered memory, for
    // instance). Clear the sticky error and let the caller stage instead.
    hapiGetLastError();
#if CMK_SMP
    CmiUnlock(csv_gpu_manager.ipc_cache_lock);
#endif
    return false;
  }

  csv_gpu_manager.ipc_export_cache.emplace(base_ptr, new_handle);
  *handle = new_handle;
#if CMK_SMP
  CmiUnlock(csv_gpu_manager.ipc_cache_lock);
#endif
  return true;
}

void* hapiIpcImportBuffer(const hapiIpcMemHandle_t& handle, int src_process,
                          const void* src_base, size_t span) {
  GPUManager& csv_gpu_manager = CsvAccess(gpu_manager);
#if CMK_SMP
  CmiLock(csv_gpu_manager.ipc_cache_lock);
#endif
  if (hapiVmmHandleIs(handle)) {
    void* mirror = hapiVmmImportLocked(handle, src_process, src_base);
#if CMK_SMP
    CmiUnlock(csv_gpu_manager.ipc_cache_lock);
#endif
    return mirror;
  }
  auto& cache = csv_gpu_manager.ipc_import_cache;
  const auto key = std::make_pair(src_process, src_base);
  auto it = cache.find(key);
  if (it != cache.end()) {
    if (hapiIpcMemHandleEq()(it->second.handle, handle)) {
      void* ptr = it->second.mapped;
      csv_gpu_manager.ipc_import_hits.fetch_add(1, std::memory_order_relaxed);
#if CMK_SMP
      CmiUnlock(csv_gpu_manager.ipc_cache_lock);
#endif
      return ptr;
    }
    // Same address, different allocation: the sender freed what this mapped and
    // the allocator handed the address to something new. Measured: a recycled
    // base got fresh handle bytes 208 times out of 208, so the handle is what
    // separates the generations -- the address alone cannot.
    //
    // Dropped, not closed. A transfer may still be reading this mapping and
    // closing one mid-copy is an illegal access, so the mapping is left to the
    // process teardown and only the entry goes.
    cache.erase(it);
    csv_gpu_manager.ipc_import_stale_dropped.fetch_add(1,
                                                       std::memory_order_relaxed);
  }

  // Open under the caller's current device, as ipcHandleOpen does for the comm
  // buffers. Switching devices around cudaIpcOpen* breaks cross-process
  // transfers (commit bec910fef) -- the call is defined in terms of the
  // caller's context and the mapping is usable from here because P2P access is
  // enabled between every pair of devices on the host.
  void* mapped = NULL;
  const hapiError_t err =
      hapiIpcOpenMemHandle(&mapped, handle, hapiIpcMemLazyEnablePeerAccess);

  // One cudaIpcOpenMemHandle maps the whole suballocator region containing the
  // allocation, so a live allocation sitting between live siblings already
  // mapped cannot be opened at all -- the driver says AlreadyMapped. Reach it by
  // offset from a sibling instead.
  //
  // Which sibling: entries mapped through one region share a constant
  // (mapped - base) delta, measured at 0x2134800000 across the 11 entries
  // bracketing the failure this handles. Requiring a mapped base on either side
  // of src_base with the same delta is what establishes that src_base is
  // interior to that one mapping -- extents cannot, because
  // cuMemGetAddressRange on an imported pointer reports the allocation's size,
  // not the region's, which is why an earlier extent-based version of this
  // never once fired.
  // No attempt to reach this allocation through a mapping already held. One
  // open maps the whole suballocator region, so a sibling from that region is
  // genuinely unopenable here, and there is no supported way to learn which
  // held mapping covers it: an address that merely verifies as mapped can
  // belong to an unrelated region, which returns wrong bytes instead of an
  // error. The caller asks for the payload by another route instead.
  (void)span;

  if (err != hapiSuccess) {
    // Keep the reason: the caller only learns that the import returned NULL.
    // already-mapped means this process still holds a mapping of this memory
    // that the key above did not find; an invalid handle means the sender's
    // allocation is gone.
    csv_gpu_manager.ipc_import_last_err = (int)err;
    hapiGetLastError();
#if CMK_SMP
    CmiUnlock(csv_gpu_manager.ipc_cache_lock);
#endif
    return NULL;
  }

  csv_gpu_manager.ipc_import_misses.fetch_add(1, std::memory_order_relaxed);
  cache[key] = GPUManager::IpcImportEntry{handle, mapped, true};
#if CMK_SMP
  CmiUnlock(csv_gpu_manager.ipc_cache_lock);
#endif
  return mapped;
}

// Drop the export entry for the allocation containing ptr, before it is freed.
//
// The export cache is keyed by allocation base, and migration frees and
// reallocates device buffers constantly while cudaMalloc happily reuses
// addresses. Without this, a send from a fresh allocation that landed on a
// recycled base hands the peer the dead allocation's handle, and the peer's
// import cache -- keyed by that handle -- serves the mapping it opened for the
// memory that is gone. Both sides then agree, and both are wrong.
//
// The flush below does exactly this reasoning at teardown, which is too late to
// help a running job. Must be called while ptr is still valid: finding the base
// needs the allocation to exist.
int hapiIpcLastImportError() {
  return CsvAccess(gpu_manager).ipc_import_last_err;
}

const char* hapiIpcLastImportErrorName() {
  const int e = CsvAccess(gpu_manager).ipc_import_last_err;
  return (e == 0) ? "none" : hapiGetErrorName((hapiError_t)e);
}

void hapiIpcInvalidateExport(const void* ptr) {
  if (ptr == NULL) return;
  GPUManager& csv_gpu_manager = CsvAccess(gpu_manager);
  if (csv_gpu_manager.ipc_export_cache.empty()) return;
  // A vmm heap's handle names the heap, which is never freed or recycled.
  if (hapiDevPoolIsVmm() && hapiDevPoolContains(ptr)) return;
  void* base = NULL;
  size_t alloc_size = 0;
  if (!hapiMemGetAddressRange(&base, &alloc_size, ptr)) return;
#if CMK_SMP
  CmiLock(csv_gpu_manager.ipc_cache_lock);
#endif
  csv_gpu_manager.ipc_export_cache.erase((const void*)base);
#if CMK_SMP
  CmiUnlock(csv_gpu_manager.ipc_cache_lock);
#endif
}

void hapiIpcFlushImportCache() {
  GPUManager& csv_gpu_manager = CsvAccess(gpu_manager);
#if CMK_SMP
  CmiLock(csv_gpu_manager.ipc_cache_lock);
#endif
  for (auto& entry : csv_gpu_manager.ipc_import_cache) {
    // Borrowed mappings share the owner's pointer; only that one can be closed.
    if (!entry.second.owns_mapping) continue;
    if (hapiIpcCloseMemHandle(entry.second.mapped) != hapiSuccess)
      hapiGetLastError();
  }
  csv_gpu_manager.ipc_import_cache.clear();
  // Exports name this process's own allocations, which migration also frees and
  // reallocates -- and cudaMalloc reuses addresses, so a stale entry would hand
  // out a handle for memory that is no longer there.
  csv_gpu_manager.ipc_export_cache.clear();
#if CMK_SMP
  CmiUnlock(csv_gpu_manager.ipc_cache_lock);
#endif
}

void hapiIpcReportStats() {
  GPUManager& csv_gpu_manager = CsvAccess(gpu_manager);
  const long staged = csv_gpu_manager.ipc_staged_sends.load();
  const long direct = csv_gpu_manager.ipc_direct_sends.load();
  const long hits = csv_gpu_manager.ipc_import_hits.load();
  const long misses = csv_gpu_manager.ipc_import_misses.load();
  const long stale = csv_gpu_manager.ipc_import_stale_dropped.load();
  const long shared = csv_gpu_manager.ipc_import_region_shared.load();
  const long repairs = csv_gpu_manager.ipc_forward_repairs.load();
  if (staged + direct + hits + misses == 0) return;
  CmiPrintf("[ipc-stats] pid=%d staged=%ld direct=%ld import_hits=%ld "
            "import_misses=%ld stale_dropped=%ld region_shared=%ld "
            "forward_repairs=%ld transport=%s\n",
            (int)getpid(), staged, direct, hits, misses, stale, shared, repairs,
            csv_gpu_manager.ipc_use_direct ? "direct" : "staged");
}

/*** Migration arena registry (see hapi.h) ***/

namespace {
struct HapiArenaRec {
  size_t extent;
  size_t live;
};
// Ordered by base so an interior pointer resolves with upper_bound. Shared by
// every PE in the process (SMP threads included), hence the mutex.
std::map<char*, HapiArenaRec> hapi_arena_registry;
std::mutex hapi_arena_registry_lock;
}

// Arenas that are not ours to cudaFree: base -> the function that takes it
// back. Kept beside the registry rather than in HapiArenaRec so the common
// (hapiMalloc) arena pays nothing for it.
std::map<char*, void (*)(void*)> hapi_arena_release;

void hapiArenaRegister(void* base, size_t extent, size_t liveBuffers) {
  std::lock_guard<std::mutex> g(hapi_arena_registry_lock);
  hapi_arena_registry[(char*)base] = HapiArenaRec{extent, liveBuffers};
}

void hapiArenaRegister(void* base, size_t extent, size_t liveBuffers,
                       void (*release)(void*)) {
  std::lock_guard<std::mutex> g(hapi_arena_registry_lock);
  hapi_arena_registry[(char*)base] = HapiArenaRec{extent, liveBuffers};
  if (release) hapi_arena_release[(char*)base] = release;
  else hapi_arena_release.erase((char*)base);
}

/*** Device pool (see hapi.h) ***/
namespace {
// The allocator behind one pool segment. Two backends (+gpupoolalloc): the
// buddy allocator over one cudaMalloc arena -- the default, power-of-two
// blocks, a new arena when none fits -- and the range heap over a virtual
// range reserved once per device and backed in place by cuMemCreate chunks
// (hapi_range_heap.h), which grows by mapping only what a request is short
// of and rounds sizes to 256 bytes. An arena's [start, end) is what is usable
// now; under vmm `end` moves as chunks are mapped.
struct HapiRangeAlloc {
  virtual ~HapiRangeAlloc() {}
  virtual void* malloc(size_t size) = 0;
  virtual void free(void* p) = 0;
  virtual bool blockSize(const void* p, size_t* size) const = 0;
  virtual size_t freeBytes() const = 0;
  virtual size_t largestFree() const = 0;
  virtual size_t extent() const = 0;                       // usable bytes now
  virtual bool grow(size_t need) { (void)need; return false; }  // in place; vmm only
  virtual bool vmm() const { return false; }
};
struct HapiBuddyAlloc : HapiRangeAlloc {
  buddy::allocator* a;
  size_t bytes;
  HapiBuddyAlloc(size_t b) : a(new buddy::allocator(b, b)), bytes(b) {}  // the one cudaMalloc
  void* malloc(size_t size) override { return a->malloc(size, true); }
  void free(void* p) override { a->free(p); }
  bool blockSize(const void* p, size_t* size) const override {
    const auto blk = a->alloc_map.find((uint8_t*)p);
    if (blk == a->alloc_map.end()) return false;
    *size = blk->second.size;
    return true;
  }
  size_t freeBytes() const override { return a->get_free_size(); }
  size_t largestFree() const override { return a->largest_free_block(); }
  size_t extent() const override { return bytes; }
};
struct HapiVmmAlloc;   // below, after the registration check it uses
struct HapiDevPoolArena { HapiRangeAlloc* alloc; uintptr_t start, end; int device; };
std::vector<HapiDevPoolArena> hapi_devpool_arenas;
std::mutex hapi_devpool_mutex;
// Segments the pool maps are handed to ck-core (hapi.h) once the pool lock is released: it registers
// them with the fabric there and then, instead of at the first off-node transfer out of each one.
// Measured (job 22856533, sph3d two nodes): a 256 MB chunk run registers in 10 ms (median; 268 max)
// and the one-process-per-node layout met ~150 of them per process inside its first LB event, which
// is where its 0.8 s Metis window over the control's came from. Deferred to the lock's release
// because the registration takes milliseconds and must not hold the pool.
struct HapiDevPoolPendingSegment { void* base; size_t bytes, unit; int device; };
static thread_local std::vector<HapiDevPoolPendingSegment> t_devpool_pending_segments;
static void hapiDevPoolRunSegmentHook() {
  if (t_devpool_pending_segments.empty()) return;
  std::vector<HapiDevPoolPendingSegment> v;
  v.swap(t_devpool_pending_segments);
  if (hapiDevPoolSegmentHook == nullptr) return;   // ck not up yet: the lazy registration path stands
  for (const HapiDevPoolPendingSegment& sg : v) hapiDevPoolSegmentHook(sg.base, sg.bytes, sg.unit, sg.device);
}
// The pool lock; segments mapped under it reach the registration hook once it is released.
struct HapiDevPoolLock {
  std::unique_lock<std::mutex> lk;
  HapiDevPoolLock() : lk(hapi_devpool_mutex) {}
  // Segments mapped under this lock reach the registration hook once it is released.
  ~HapiDevPoolLock() {
    if (lk.owns_lock()) lk.unlock();
    hapiDevPoolRunSegmentHook();
  }
};
// Buffers the packer is reading: ptr -> the stream those reads are on. An
// ordered map, because what the packer notes and what comes back to be freed
// are not always the same pointer: a migration arena carved from the pool holds
// several sub-buffers, each noted on its own address, and its release is one
// free of the arena base. The free has to find every note inside the block it
// is freeing, which lower_bound over the block's range does.
std::multimap<const void*, hapiStream_t> hapi_devpool_read_on;
// Freed blocks waiting for their reads to retire before they can be reused --
// one event per stream the reads were on (in practice one).
// A block whose last readers may still be running: reusable once every flag
// has landed and every event completed. Flags (hapiFlagIssue on the reading
// stream) are the normal case and cost the reap a plain load each; an event
// stands in when the ring had no slot or the stream is another device's.
struct HapiDevPoolPending {
  void* ptr; int device;
  std::vector<std::pair<int, uint32_t>> flags;   // (rank, seq)
  std::vector<hapiEvent_t> evs;
};
std::vector<HapiDevPoolPending> hapi_devpool_pending;
// Spare events to record with, kept per device. A CUDA event belongs to the
// device that was current when it was created, and recording it on a stream
// belonging to another device fails with cudaErrorInvalidResourceHandle. PEs
// in one process sit on different devices (my_device is a Cpv, and a process
// that owns the whole node sees every GPU), and they share this pool under
// hapi_devpool_mutex -- so a single list hands a PE freeing a device-1 block
// an event created on device 0. That costs nothing until the first reuse,
// which is why it survives one load-balancing step and fails on the next.
std::map<int, std::vector<hapiEvent_t>> hapi_devpool_spare_events;

size_t hapiDevPoolArenaBytes() {
  static size_t bytes = 0;
  if (!bytes) {
    bytes = CsvAccess(gpu_manager).device_pool_arena_bytes;   // +gpupoolsize
    if (!bytes) {
      const char* e = getenv("CK_GPU_ARENA_MB");
      bytes = (size_t)(e ? atol(e) : 256) << 20;
    }
  }
  return bytes;
}

HapiDevPoolArena* hapiDevPoolArenaOfLocked(const void* p) {
  const uintptr_t q = (uintptr_t)p;
  for (auto& ar : hapi_devpool_arenas)
    if (q >= ar.start && q < ar.end) return &ar;
  return nullptr;
}

// Return every parked block whose reads have retired. Called with the lock
// held, before a malloc searches, so a block is never handed out while the
// migration copy that packed it is still reading.
void hapiDevPoolReapLocked() {
  for (size_t i = 0; i < hapi_devpool_pending.size();) {
    HapiDevPoolPending& pd = hapi_devpool_pending[i];
    bool done = true;
    for (const auto& f : pd.flags)
      if (!hapiFlagLanded(f.first, f.second)) { done = false; break; }
    for (size_t k = 0; done && k < pd.evs.size(); k++) {   // fallback events only
      hapiEvent_t ev = pd.evs[k];
      const hapiError_t ev_state = hapiEventQuery(ev);
      // A query records its verdict as this thread's last error, and the
      // application's next cudaPeekAtLastError would report our "not ready"
      // as its own kernel's failure. Clear that one code and nothing else: a
      // real fault is sticky and comes back from the query itself, so leaving
      // any other code in place keeps it visible to whoever checks next.
      if (ev_state == hapiErrorNotReady) hapiGetLastError();
      if (ev_state != hapiSuccess) { done = false; break; }
    }
    if (done) {
      HapiDevPoolArena* ar = hapiDevPoolArenaOfLocked(pd.ptr);
      if (ar) ar->alloc->free(pd.ptr);   // interface call: buddy or vmm
      auto& spares = hapi_devpool_spare_events[pd.device];
      for (hapiEvent_t ev : pd.evs) spares.push_back(ev);
      pd = std::move(hapi_devpool_pending.back());
      hapi_devpool_pending.pop_back();
    } else {
      ++i;
    }
  }
}

// At 2+ physical nodes a buddy arena is registered with the fabric whole, the
// first time a peer off-node is sent to (acquireArenaRegistration), and never
// deregistered. The provider has a ceiling per registration: one 4096 MB
// arena fails every rank in lci poll_comp with "Err 5: Input/output error" --
// at startup for a pre-sized pool, or in the middle of a step for growth --
// while 2048 MB works. 3-4 x 2048 MB buddy arenas per process also failed
// (2026-09-18), read then as a per-process total; a vmm heap registering 6 GB
// per process in 256 MB chunks ran clean (job 22217872), so the total applies
// to buddy arenas only (`totalToo`) and a vmm chunk is checked on its own
// size. A vmm chunk (cuMemCreate memory) of exactly the arena ceiling is the
// one more case: 2048 MB chunks registered without error on every rank and
// every remote write into them then failed with ADDR_OUT_OF_RANGE at the first
// step, while 1536, 1792 and 2046 MB chunks on the same nodes carried every
// transfer exactly (job 23388275, 2026-10-31): the ceiling is 2^31 bytes
// exactly, so for a vmm chunk it is exclusive.
// Refuse here, with the reason, instead of dying in the transport. One node
// takes any size. CK_GPU_ARENA_MAX_MB and CK_GPU_ARENA_TOTAL_MAX_MB move the
// two ceilings when a provider allows more.
size_t hapiDevPoolHeldLocked() {
  size_t held = 0;
  for (auto& ar : hapi_devpool_arenas) held += (size_t)(ar.end - ar.start);
  return held;
}

void hapiDevPoolCheckRegistrable(size_t bytes, size_t nArenas, size_t held, bool totalToo) {
  if (CmiNumPhysicalNodes() <= 1) return;
  if (!totalToo) held = 0;
  static const size_t arenaMax = []() {
    const char* e = getenv("CK_GPU_ARENA_MAX_MB");
    return (size_t)(e ? atol(e) : 2048) << 20;
  }();
  static const size_t totalMax = []() {
    const char* e = getenv("CK_GPU_ARENA_TOTAL_MAX_MB");
    return (size_t)(e ? atol(e) : 4096) << 20;
  }();
  const bool vmm = CsvAccess(gpu_manager).device_pool_backend == 1;
  if ((vmm ? bytes < arenaMax : bytes <= arenaMax) && held + bytes <= totalMax) return;
  CmiAbort("HAPI> [%d] device pool: refusing to open %s %zu of %zu MB: this process "
           "would hold %zu MB of pool arenas. With %d physical nodes every arena is "
           "registered with the fabric whole, and registrations past %zu MB per arena "
           "(a vmm chunk: of %zu MB or more) or %zu MB per process fail every rank in "
           "the transport (lci poll_comp Err 5, ADDR_OUT_OF_RANGE on every write). "
           "%sReduce the per-GPU problem size, or size +gpupoolsize so the working set "
           "fits at startup (+gpupoolchunk below the ceiling but above the largest "
           "buffer sent off-node); CK_GPU_ARENA_MAX_MB / CK_GPU_ARENA_TOTAL_MAX_MB move "
           "the ceilings.",
           CmiMyPe(), vmm ? "chunk" : "arena",
           nArenas + 1, bytes >> 20, (held + bytes) >> 20,
           CmiNumPhysicalNodes(), arenaMax >> 20, arenaMax >> 20, totalMax >> 20,
           held > 0 ? "The balancer plans only within the arenas the pool already has, "
                      "so this growth is an application or runtime allocation. "
                    : "");
}

HapiDevPoolArena& hapiDevPoolNewArenaLocked(size_t need, int dev) {
  size_t bytes = hapiDevPoolArenaBytes();
  while (bytes < need * 2) bytes <<= 1;
  hapiDevPoolCheckRegistrable(bytes, hapi_devpool_arenas.size(), hapiDevPoolHeldLocked(), true);
  HapiDevPoolArena ar;
  HapiBuddyAlloc* b = new HapiBuddyAlloc(bytes);   // the one real cudaMalloc
  ar.alloc = b;
  ar.start = (uintptr_t)b->a->base_ptr;
  ar.end = ar.start + bytes;
  ar.device = dev;
  hapi_devpool_arenas.push_back(ar);
  t_devpool_pending_segments.push_back({(void*)ar.start, bytes, bytes, dev});
  CmiPrintf("HAPI> [%d] device pool: arena %zu of %zu MB at %p on device %d "
            "(CK_GPU_ARENA_MB)\n", CmiMyPe(), hapi_devpool_arenas.size(),
            bytes >> 20, (void*)ar.start, dev);
  return hapi_devpool_arenas.back();
}

const char* hapiCuErrName(hapiDrvResult r) {
  const char* s = nullptr;
  hapiDrvGetErrorName(r, &s);
  return s ? s : "CUDA_ERROR_?";
}

// The vmm backend's segment: one per device. The heap does the bookkeeping
// (hapi_range_heap.h); this maps chunks when it asks, exports each chunk's
// file descriptor for peers, and publishes them through the +gpushm entry.
struct HapiVmmAlloc : HapiRangeAlloc {
  hapi_pool::RangeHeap heap;
  int dev;       // the pool's key: the process-local device index (my_device_id)
  int cudaDev;   // the CUDA ordinal the chunks live on (DeviceManager::global_index)
  hapiMemAllocationProp prop;
  std::vector<hapiMemGenericAllocationHandle> handles;
  std::vector<int> fds;
  hapi_pool_shm_entry* shm = nullptr;   // set by publish(); null without +gpushm
  int shm_index = -1;
  bool exportable = false;

  HapiVmmAlloc(uintptr_t base, size_t reserve, size_t chunk, int dev_, int cudaDev_)
      : heap(base, reserve, chunk), dev(dev_), cudaDev(cudaDev_) {
    memset(&prop, 0, sizeof(prop));
    heap.mapper = [this](uintptr_t addr, size_t bytes) { return mapChunks(addr, bytes); };
  }
  void* malloc(size_t size) override { return heap.malloc(size); }
  void free(void* p) override { heap.free(p); }
  bool blockSize(const void* p, size_t* size) const override { return heap.blockSize(p, size); }
  size_t freeBytes() const override { return heap.freeBytes(); }
  size_t largestFree() const override { return heap.largestFree(); }
  size_t extent() const override { return heap.mappedBytes(); }
  bool grow(size_t need) override { return heap.grow(need); }
  bool vmm() const override { return true; }

  // Called by the heap with addr == its mapped end and bytes a multiple of
  // the chunk. Every chunk is checked against the fabric's per-registration
  // ceiling first (a chunk is one allocation and registers on its own; no
  // per-process total applies, see hapiDevPoolCheckRegistrable), then
  // created, mapped, made accessible to this device, and exported.
  bool mapChunks(uintptr_t addr, size_t bytes) {
    const size_t chunk = heap.chunkBytes();
    const size_t n = bytes / chunk;
    size_t held = hapiDevPoolHeldLocked();
    size_t mapped = 0;   // chunks of this call that are mapped: handed to the segment hook as one run
    for (size_t i = 0; i < n; i++) {
      if (handles.size() >= HAPI_VMM_MAX_CHUNKS)
        CmiAbort("HAPI> [%d] device pool (vmm): %zu chunks of %zu MB mapped, the shared "
                 "table's limit; raise +gpupoolchunk", CmiMyPe(), handles.size(), chunk >> 20);
      hapiDevPoolCheckRegistrable(chunk, handles.size(), held, false);
      const uintptr_t at = (uintptr_t)(addr + i * chunk);
      hapiMemGenericAllocationHandle h;
      hapiDrvResult r = hapiDrvMemCreate(&h, chunk, &prop, 0);
      if (r != hapiDrvSuccess && handles.empty()) {
        // The very first chunk: the failure is about the allocation
        // properties, not memory. Step down through the optional ones --
        // RDMA capability, then the exportable handle type -- and keep
        // whatever the driver accepts, saying so.
        hapiMemAllocationProp p2 = prop;
        const hapiDrvResult first = r;
        if (r != hapiDrvSuccess && p2.allocFlags.gpuDirectRDMACapable) {
          p2.allocFlags.gpuDirectRDMACapable = 0;
          r = hapiDrvMemCreate(&h, chunk, &p2, 0);
          if (r == hapiDrvSuccess)
            CmiPrintf("HAPI> [%d] device pool (vmm): chunks are NOT GPUDirect-RDMA capable "
                      "(cuMemCreate with the flag: %s); inter-node sends from the pool "
                      "will not register\n", CmiMyPe(), hapiCuErrName(first));
        }
        if (r != hapiDrvSuccess && p2.requestedHandleTypes != hapiMemHandleTypeNone) {
          p2.requestedHandleTypes = hapiMemHandleTypeNone;
          r = hapiDrvMemCreate(&h, chunk, &p2, 0);
          if (r == hapiDrvSuccess) {
            exportable = false;
            CmiPrintf("HAPI> [%d] device pool (vmm): chunks are NOT exportable as file "
                      "descriptors (cuMemCreate with the handle type: %s); peers cannot map "
                      "this heap, cross-process sends will stage\n", CmiMyPe(),
                      hapiCuErrName(first));
          }
        }
        if (r == hapiDrvSuccess) prop = p2;
      }
      if (r != hapiDrvSuccess) {
        size_t freeMem = 0, totalMem = 0;
        hapiMemGetInfo(&freeMem, &totalMem);
        size_t gran = 0;
        hapiDrvMemGetAllocationGranularity(&gran, &prop, hapiMemAllocGranularityMinimum);
        CmiPrintf("HAPI> [%d] device pool (vmm): cuMemCreate of a %zu MB chunk failed: %s "
                  "(cuda device %d, %zu MB mapped, %zu of %zu MB free on the device, minimum "
                  "granularity %zu KB, handle types 0x%x, rdma %d)\n", CmiMyPe(),
                  chunk >> 20, hapiCuErrName(r), cudaDev, extent() >> 20, freeMem >> 20,
                  totalMem >> 20, gran >> 10, (unsigned)prop.requestedHandleTypes,
                  (int)prop.allocFlags.gpuDirectRDMACapable);
        if (mapped) t_devpool_pending_segments.push_back({(void*)addr, mapped * chunk, chunk, dev});
        return false;   // the heap stays as it was; the caller reports OOM
      }
      r = hapiDrvMemMap(at, chunk, 0, h, 0);
      if (r != hapiDrvSuccess)
        CmiAbort("HAPI> [%d] device pool (vmm): cuMemMap failed: %s", CmiMyPe(), hapiCuErrName(r));
      r = hapiVmmSetAccessAllLocal(at, chunk, cudaDev, "device pool (vmm)");
      if (r != hapiDrvSuccess)
        CmiAbort("HAPI> [%d] device pool (vmm): cuMemSetAccess failed: %s", CmiMyPe(), hapiCuErrName(r));
      int fd = -1;
      if (exportable) {
        r = hapiDrvMemExportToShareableHandle(&fd, h, hapiMemHandleTypePosixFd, 0);
        if (r != hapiDrvSuccess)
          CmiAbort("HAPI> [%d] device pool (vmm): cuMemExportToShareableHandle failed: %s",
                   CmiMyPe(), hapiCuErrName(r));
      }
      handles.push_back(h);
      fds.push_back(fd);   // kept open for the run: peers duplicate it with pidfd_getfd
      mapped++;
      held += chunk;
      if (shm != nullptr) {
        shm->fds[handles.size() - 1] = fd;
        __atomic_store_n(&shm->n_chunks, (int)handles.size(), __ATOMIC_RELEASE);
      }
    }
    if (mapped) t_devpool_pending_segments.push_back({(void*)addr, mapped * chunk, chunk, dev});
    return true;
  }

  // Fill this process's +gpushm entry for the heap so peers can mirror it.
  void publish(hapi_pool_shm_entry* e, int index) {
    shm = e;
    shm_index = index;
    e->kind = 1;
    e->pid = (int)getpid();
    e->base = (void*)heap.base();
    e->extent = heap.reserveBytes();
    e->chunk_bytes = heap.chunkBytes();
    for (size_t i = 0; i < fds.size(); i++) e->fds[i] = fds[i];
    __atomic_store_n(&e->n_chunks, (int)fds.size(), __ATOMIC_RELEASE);
  }
};

HapiDevPoolArena* hapiDevPoolVmmArenaLocked(int dev) {
  for (auto& ar : hapi_devpool_arenas)
    if (ar.device == dev && ar.alloc->vmm()) return &ar;
  return nullptr;
}

size_t hapiDevPoolChunkBytesConfigured() {
  size_t c = CsvAccess(gpu_manager).device_pool_chunk_bytes;
  return c ? c : ((size_t)256 << 20);
}

// Open the device's vmm heap: reserve the range, and map the startup budget
// (+gpupoolsize x +gpupoolarenas) so the load balancer's headroom exists from
// the first step (the balancer plans only within what is mapped).
HapiDevPoolArena& hapiDevPoolNewVmmHeapLocked(int dev) {
  GPUManager& gm = CsvAccess(gpu_manager);
  // `dev` is the pool's key, the process-local index (my_device_id); the
  // physical location cuMemCreate wants is that device's CUDA ordinal. Under
  // cudaMalloc this never mattered (it allocates on the current device); a
  // heap placed on ordinal 0 by every process is how the first vmm run put
  // four 40 GB budgets on one GPU.
  int cudaDev = dev;
  if (dev >= 0 && dev < (int)gm.device_managers.size())
    cudaDev = gm.device_managers[dev].global_index;
  hapiDrvDevice cudev;
  hapiDrvResult r = hapiDrvDeviceGet(&cudev, cudaDev);
  if (r != hapiDrvSuccess)
    CmiAbort("HAPI> [%d] device pool (vmm): cuDeviceGet(%d): %s", CmiMyPe(), cudaDev, hapiCuErrName(r));
  int vmmOk = 0, fdOk = 0, rdmaOk = 0;
  hapiDrvDeviceVmmSupport(cudev, &vmmOk, &fdOk, &rdmaOk);
  if (!vmmOk)
    CmiAbort("HAPI> [%d] device %d does not support CUDA virtual memory management; "
             "use +gpupoolalloc buddy", CmiMyPe(), dev);
  hapiMemAllocationProp prop;
  memset(&prop, 0, sizeof(prop));
  prop.type = hapiMemAllocationTypePinned;
  prop.location.type = hapiMemLocationTypeDevice;
  prop.location.id = cudaDev;
  if (fdOk) prop.requestedHandleTypes = hapiMemHandleTypePosixFd;
  if (rdmaOk) prop.allocFlags.gpuDirectRDMACapable = 1;   // registrable by the NIC
  size_t gran = 0;
  r = hapiDrvMemGetAllocationGranularity(&gran, &prop, hapiMemAllocGranularityRecommended);
  if (r != hapiDrvSuccess || gran == 0) gran = (size_t)2 << 20;
  size_t chunk = hapi_pool::RangeHeap::roundUp(hapiDevPoolChunkBytesConfigured(), gran);
  size_t reserve = gm.device_pool_reserve_bytes;
  if (reserve == 0) {
    size_t total = 0;
    hapiDrvDeviceTotalMem(&total, cudev);
    reserve = total * 2;
  }
  reserve = hapi_pool::RangeHeap::roundUp(reserve, chunk);
  // A distinct address range per process and device. Left to the driver,
  // every process reserves the same range (0x602000000 on Delta), so a
  // device pointer in an incoming message -- an address in the SENDER's
  // process -- equals one of this process's own blocks on every cross-node
  // message (ckrdmadevice.C, the staged-landing mark: with buddy arenas that
  // collision was occasional; with vmm heaps it is certain). Job 22219945:
  // leanmd and sph2d async died in the fabric and leanmd sync in the host
  // heap at their first cross-node migrations. The hint is a slot of
  // `reserve` bytes at 1 TiB + (process x devices + device) x reserve; the
  // driver may still place the range elsewhere, and then says so.
  const uintptr_t slot = (uintptr_t)CmiMyNode() * (uintptr_t)std::max(1, gm.device_count) + (uintptr_t)dev;
  const uintptr_t hint = (uintptr_t)(((uintptr_t)1 << 40) + slot * (uintptr_t)reserve);
  uintptr_t base = 0;
  r = hapiDrvMemAddressReserve(&base, reserve, 0, hint, 0);
  if (r != hapiDrvSuccess)
    CmiAbort("HAPI> [%d] device pool (vmm): cuMemAddressReserve of %zu MB failed: %s",
             CmiMyPe(), reserve >> 20, hapiCuErrName(r));
  if (base != hint)
    CmiPrintf("HAPI> [%d] device pool (vmm): the driver placed the heap at %p, not at the "
              "per-process slot %p; foreign pointers may collide with local blocks\n",
              CmiMyPe(), (void*)(uintptr_t)base, (void*)(uintptr_t)hint);
  HapiVmmAlloc* v = new HapiVmmAlloc((uintptr_t)base, reserve, chunk, dev, cudaDev);
  v->prop = prop;
  v->exportable = fdOk != 0;
  HapiDevPoolArena ar;
  ar.alloc = v;
  ar.start = (uintptr_t)base;
  ar.end = ar.start;   // nothing mapped yet
  ar.device = dev;
  hapi_devpool_arenas.push_back(ar);
  HapiDevPoolArena& ref = hapi_devpool_arenas.back();
  const size_t initial = hapiDevPoolArenaBytes() * (size_t)std::max(1, gm.device_pool_arenas);
  if (!v->grow(initial))
    CmiAbort("HAPI> [%d] device pool (vmm): could not map the startup budget of %zu MB on "
             "device %d (+gpupoolsize x +gpupoolarenas)", CmiMyPe(), initial >> 20, dev);
  ref.end = ref.start + v->extent();
  CmiPrintf("HAPI> [%d] device pool (vmm): heap at %p on device %d (cuda %d), %zu MB reserved, "
            "%zu MB mapped in %zu chunks of %zu MB (RDMA-capable: %s, exportable: %s)\n",
            CmiMyPe(), (void*)base, dev, cudaDev, reserve >> 20, v->extent() >> 20,
            v->heap.nChunks(), chunk >> 20, rdmaOk ? "yes" : "no", fdOk ? "yes" : "no");
  return ref;
}
}  // namespace

// Who is asking for device memory, for the growth notice (defined with the
// footprint ledger below).
static const char* hapiDescribeRequester();

namespace {
// The landing set current on this thread (hapiPreallocBegin), if any.
thread_local std::multimap<size_t, void*>* hapi_prealloc_current = nullptr;

void* hapiDevPoolMallocImpl(size_t size, int dev, bool grow) {
  // A landing set serves first: the smallest block in it that fits.
  if (hapi_prealloc_current != nullptr && !hapi_prealloc_current->empty()) {
    auto it = hapi_prealloc_current->lower_bound(size);
    if (it != hapi_prealloc_current->end()) {
      void* p = it->second;
      const size_t held = it->first;
      hapi_prealloc_current->erase(it);
      hapiRecordAlloc(p, held);   // the element's now, at the block's size
      return p;
    }
  }
  void* q = nullptr;
  size_t held = 0;
  {
    HapiDevPoolLock g;
    hapiDevPoolReapLocked();
    HapiDevPoolArena* from = nullptr;
    for (auto& ar : hapi_devpool_arenas) {
      if (ar.device != dev) continue;
      q = ar.alloc->malloc(size);
      if (q) { from = &ar; break; }
    }
    if (!q && !grow) return nullptr;
    if (!q) {
      // Growth: say what asked and what the arenas had, so a grown arena can
      // be traced to its allocation. The free-byte sum and the largest free
      // block differ under a buddy allocator, and the difference is the
      // usual reason a request that "fits" by bytes still lands here.
      size_t freeSum = 0, largest = 0;
      int mine = 0;
      for (auto& ar : hapi_devpool_arenas) {
        if (ar.device != dev) continue;
        mine++;
        freeSum += ar.alloc->freeBytes();
        largest = std::max(largest, ar.alloc->largestFree());
      }
      const bool vmm = CsvAccess(gpu_manager).device_pool_backend == 1;
      CmiPrintf("HAPI> [%d] device pool%s: growing for a %zu-byte request from %s; the %d "
                "%s on device %d had %zu MB free, largest free block %zu KB\n",
                CmiMyPe(), vmm ? " (vmm)" : "", size, hapiDescribeRequester(), mine,
                vmm ? "heap(s)" : "arena(s)", dev, freeSum >> 20, largest >> 10);
      if (vmm) {
        // In place: map only what the request is short of, at the heap's end.
        HapiDevPoolArena* ar = hapiDevPoolVmmArenaLocked(dev);
        if (ar == nullptr) ar = &hapiDevPoolNewVmmHeapLocked(dev);
        if (!ar->alloc->grow(size))
          CmiAbort("HAPI> [%d] device pool (vmm): could not map %zu more bytes on device %d "
                   "(%zu MB mapped of %zu MB reserved): the device is out of memory or "
                   "the reservation (+gpupoolreserve) is exhausted",
                   CmiMyPe(), size, dev, ar->alloc->extent() >> 20,
                   ((HapiVmmAlloc*)ar->alloc)->heap.reserveBytes() >> 20);
        ar->end = ar->start + ar->alloc->extent();
        q = ar->alloc->malloc(size);
        if (!q) CmiAbort("hapiDevPoolMalloc (vmm): no block after growing");
        from = ar;
      } else {
        HapiDevPoolArena& ar = hapiDevPoolNewArenaLocked(size, dev);
        q = ar.alloc->malloc(size);
        if (!q) CmiAbort("hapiDevPoolMalloc: request larger than a fresh arena");
        from = &ar;
      }
    }
    if (!from->alloc->blockSize(q, &held)) held = size;
  }
  // Charged at the block's size, which is what the pool gave up for it.
  hapiRecordAlloc(q, held);
  return q;
}
}  // namespace

void* hapiDevPoolMalloc(size_t size, int dev) {
  return hapiDevPoolMallocImpl(size, dev, /*grow=*/true);
}

// hapiMalloc / hapiFree (hapi_portable.h): the pool when +gpupool is on,
// cudaMalloc/cudaFree otherwise. The pool charges the footprint ledger
// itself; the cudaMalloc path charges here.
hapiError_t hapiMallocImpl(void** ptr, size_t size) {
  if (hapiDevPoolOn()) {
    // The pool is keyed by the process-local device index, the same key
    // CkDeviceMalloc passes (CpvAccess(my_device_id)); the CUDA ordinal is
    // the pool's business (hapiDevPoolNewVmmHeapLocked).
    *ptr = hapiDevPoolMalloc(size, CpvAccess(my_device_id));
    return (*ptr != nullptr) ? hapiSuccess : hapiErrorMemoryAllocation;
  }
  const hapiError_t e = hapiMallocRaw(ptr, size);
  if (e == hapiSuccess) hapiRecordAlloc(*ptr, size);
  return e;
}

hapiError_t hapiFreeImpl(void* ptr) {
  if (ptr == nullptr) return hapiSuccess;
  // A pool block goes back to the pool (hapiDevPoolFree forgets its
  // attribution); anything else -- including allocations made before the
  // pool was switched on -- is a real cudaFree.
  if (hapiDevPoolOn() && hapiDevPoolContains(ptr)) {
    hapiDevPoolFree(ptr);
    return hapiSuccess;
  }
  hapiRecordFree(ptr);
  return hapiFreeRaw(ptr);
}

// Free ordered behind `stream`: a pool block is parked until the work queued
// on the stream so far has retired (hapiDevPoolNoteRead); anything else is
// cudaFree, which waits for the device.
hapiError_t hapiFreeAsync(void* ptr, hapiStream_t stream) {
  if (ptr == nullptr) return hapiSuccess;
  if (hapiDevPoolOn() && hapiDevPoolContains(ptr)) {
    hapiDevPoolNoteRead(ptr, stream);
    hapiDevPoolFree(ptr);
    return hapiSuccess;
  }
  hapiRecordFree(ptr);
  return hapiFreeRaw(ptr);
}

void* hapiDevPoolMallocNoGrow(size_t size, int dev) {
  return hapiDevPoolMallocImpl(size, dev, /*grow=*/false);
}

void hapiPreallocBegin(std::multimap<size_t, void*>* set) { hapi_prealloc_current = set; }
void hapiPreallocEnd() { hapi_prealloc_current = nullptr; }

bool hapiDevPoolBlockSize(const void* ptr, size_t* size) {
  HapiDevPoolLock g;
  HapiDevPoolArena* ar = hapiDevPoolArenaOfLocked(ptr);
  if (ar == nullptr) return false;
  return ar->alloc->blockSize(ptr, size);
}

size_t hapiDevPoolFreeBytes() {
  HapiDevPoolLock g;
  hapiDevPoolReapLocked();   // parked blocks whose reads retired are free again
  size_t free = 0;
  for (auto& ar : hapi_devpool_arenas) free += ar.alloc->freeBytes();
  return free;
}

size_t hapiDevPoolFreeBytesOn(int device) {
  HapiDevPoolLock g;
  hapiDevPoolReapLocked();
  size_t free = 0;
  for (auto& ar : hapi_devpool_arenas)
    if (ar.device == device) free += ar.alloc->freeBytes();
  return free;
}

size_t hapiDevPoolUsedBytesOn(int device) {
  HapiDevPoolLock g;
  hapiDevPoolReapLocked();
  size_t used = 0;
  for (auto& ar : hapi_devpool_arenas)
    if (ar.device == device)
      used += (size_t)(ar.end - ar.start) - ar.alloc->freeBytes();
  return used;
}

size_t hapiDevPoolCapacityOn(int device) {
  HapiDevPoolLock g;
  size_t cap = 0;
  for (auto& ar : hapi_devpool_arenas)
    if (ar.device == device) cap += (size_t)(ar.end - ar.start);
  return cap;
}

bool hapiDevPoolIsVmm() { return CsvAccess(gpu_manager).device_pool_backend == 1; }

size_t hapiDevPoolGrowthBytes() {
  return hapiDevPoolIsVmm() ? hapiDevPoolChunkBytesConfigured() : hapiDevPoolArenaBytes();
}

size_t hapiDevPoolArenaSize() { return hapiDevPoolGrowthBytes(); }

void hapiLBDevicePool(size_t* poolFree, int* pesOnDevice) {
  GPUManager& csv_gpu_manager = CsvAccess(gpu_manager);
  *poolFree = csv_gpu_manager.device_pool_on ? hapiDevPoolFreeBytesOn(CpvAccess(my_device_id)) : 0;
  int n = 0;
  auto me = csv_gpu_manager.device_map.find(CmiMyPe());
  if (me != csv_gpu_manager.device_map.end()) {
    const int first = CmiNodeFirst(CmiMyNode());
    for (int pe = first; pe < first + CmiMyNodeSize(); pe++) {
      auto it = csv_gpu_manager.device_map.find(pe);
      if (it != csv_gpu_manager.device_map.end() && it->second == me->second) n++;
    }
  }
  *pesOnDevice = n > 0 ? n : 1;
}

size_t hapiLBDevicePoolCapacity() {
  if (!CsvAccess(gpu_manager).device_pool_on) return 0;
  return hapiDevPoolCapacityOn(CpvAccess(my_device_id));
}

void hapiLBDeviceMemory(size_t* devFree, size_t* poolFree, size_t* arenaBytes,
                        int* ipcSlots) {
  GPUManager& csv_gpu_manager = CsvAccess(gpu_manager);
  size_t freeMem = 0, totalMem = 0;
  hapiCheck(hapiMemGetInfo(&freeMem, &totalMem));
  *devFree = freeMem;
  if (csv_gpu_manager.device_pool_on) {
    // Chare state, migration payloads and landing arenas all come from this
    // pool, which grows into free device memory an arena at a time. The
    // contract reads the two halves separately: pool bytes are per process,
    // device bytes are shared by every process on the device.
    *poolFree = hapiDevPoolFreeBytesOn(CpvAccess(my_device_id));
    *arenaBytes = hapiDevPoolGrowthBytes();   // an arena, or a vmm chunk
  } else if (csv_gpu_manager.use_shm) {
    DeviceManager* dm = csv_gpu_manager.device_map[CmiMyPe()];
    *poolFree = dm->get_lb_buffer_free_size();
    *arenaBytes = 0;
  } else {
    *poolFree = 0;
    *arenaBytes = 0;
  }
  if (csv_gpu_manager.use_shm) {
    const int slice = csv_gpu_manager.hapi_ipc_event_pool_size_pe;
    *ipcSlots = slice >= 4 ? slice * 3 / 4 : (slice > 0 ? 1 : 0);
  } else {
    *ipcSlots = 0;
  }
}

void hapiDevPoolFree(void* ptr) {
  if (ptr == NULL) return;
  // Not hapiRecordFree: a pool block's base is never cudaFree'd, and dropping
  // the IPC export by its address would drop the whole arena's.
  hapiRecordForget(ptr);
  HapiDevPoolLock g;
  HapiDevPoolArena* ar = hapiDevPoolArenaOfLocked(ptr);
  if (!ar) CmiAbort("hapiDevPoolFree: pointer is not from the device pool");
  // The block's full extent, from the allocator's own record of it: every
  // read noted anywhere inside it -- the block itself, or a sub-buffer of an
  // arena carved from it -- has to retire before the block goes back.
  size_t blkSize = 0;
  const bool known = ar->alloc->blockSize(ptr, &blkSize);
  const char* lo = (const char*)ptr;
  const char* hi = lo + (known ? blkSize : 1);
  std::vector<hapiStream_t> streams;
  for (auto it = hapi_devpool_read_on.lower_bound(ptr);
       it != hapi_devpool_read_on.end() && (const char*)it->first < hi;) {
    if (std::find(streams.begin(), streams.end(), it->second) == streams.end())
      streams.push_back(it->second);
    it = hapi_devpool_read_on.erase(it);
  }
  if (streams.empty()) {
    ar->alloc->free(ptr);       // nothing reading it: reusable at once
    return;
  }
  // The packer is (or was) reading inside this block on those streams. Record
  // behind the reads and park; reaped once every event has completed.
  // Keyed by the device this PE has current, which is the device the streams
  // it noted reads on belong to -- not the device the block's arena is on.
  // A block can be freed by a PE on a different device than the arena was
  // created for, so pairing the event with the arena records a device-0 event
  // on a device-1 stream and CUDA rejects it.
  int cur_dev = 0;
  hapiCheck(hapiGetDevice(&cur_dev));
  HapiDevPoolPending pd; pd.ptr = ptr; pd.device = cur_dev;
  auto& spares = hapi_devpool_spare_events[cur_dev];
  for (hapiStream_t st : streams) {
    int rank = -1;
    uint32_t seq = 0;
    if (hapiFlagIssue(st, &rank, &seq)) {   // the normal case: reaped by a plain load
      pd.flags.emplace_back(rank, seq);
      continue;
    }
    hapiEvent_t ev;
    if (!spares.empty()) {
      ev = spares.back(); spares.pop_back();
    } else {
      hapiCheck(hapiEventCreateWithFlags(&ev, hapiEventDisableTiming));
    }
    const hapiError_t rec = hapiEventRecordRaw(ev, st);
    if (rec != hapiSuccess) {
      int cur = -1; hapiGetDevice(&cur);
      const hapiError_t sq = hapiStreamQuery(st);
      const hapiError_t eq = hapiEventQuery(ev);
      CmiPrintf("[%d] devpool record failed: %s | ptr=%p arena_dev=%d cur_dev=%d "
                "stream=%p (query %s) event=%p (query %s)\n",
                CmiMyPe(), hapiGetErrorName(rec), ptr, ar->device, cur,
                (void*)st, hapiGetErrorName(sq), (void*)ev, hapiGetErrorName(eq));
      CmiAbort("devpool: cudaEventRecord failed");
    }
    pd.evs.push_back(ev);
  }
  hapi_devpool_pending.push_back(std::move(pd));
}

bool hapiDevPoolContains(const void* ptr) {
  HapiDevPoolLock g;
  return hapiDevPoolArenaOfLocked(ptr) != nullptr;
}

bool hapiDevPoolArenaOf(const void* ptr, void** base, size_t* extent) {
  HapiDevPoolLock g;
  HapiDevPoolArena* ar = hapiDevPoolArenaOfLocked(ptr);
  if (ar == nullptr) return false;
  *base = (void*)ar->start;
  *extent = (size_t)(ar->end - ar->start);
  return true;
}

bool hapiDevPoolSegmentOf(const void* ptr, size_t cnt, void** base, size_t* extent) {
  HapiDevPoolLock g;
  HapiDevPoolArena* ar = hapiDevPoolArenaOfLocked(ptr);
  if (ar == nullptr) return false;
  if (!ar->alloc->vmm()) {
    *base = (void*)ar->start;
    *extent = (size_t)(ar->end - ar->start);
    return true;
  }
  // The chunk holding [ptr, ptr+cnt): what peers map and the fabric registers,
  // one unit each. Every chunk is its own physical allocation (cuMemCreate),
  // and the provider accepts a registration spanning two but fails the first
  // transfer on it (lci poll_comp Err 5; job 22217038: every 512 MB two-chunk
  // run died, 166 single-chunk runs ran clean), so the heap never places a
  // block across a boundary (RangeHeap::malloc). A block bigger than a chunk
  // is the one case left: it gets the run, and a warning once.
  const size_t chunk = ((HapiVmmAlloc*)ar->alloc)->heap.chunkBytes();
  const uintptr_t p = (uintptr_t)ptr;
  const uintptr_t end = p + (cnt ? cnt : 1);
  const uintptr_t lo = ar->start + (p - ar->start) / chunk * chunk;
  uintptr_t hi = lo + chunk;
  if (end > hi) {
    static bool warned = false;
    if (!warned) {
      warned = true;
      CmiPrintf("HAPI> [%d] device pool (vmm): a %zu-byte buffer at %p spans %zu chunks of "
                "%zu MB; the fabric cannot register a run of chunks, so this transfer will "
                "fail -- raise +gpupoolchunk above the largest buffer sent off-node "
                "(and below 2048 MB)\n",
                CmiMyPe(), cnt, ptr, (size_t)((end - lo + chunk - 1) / chunk), chunk >> 20);
    }
    hi = ar->start + hapi_pool::RangeHeap::roundUp(end - ar->start, chunk);
  }
  if (hi > ar->end) hi = ar->end;
  *base = (void*)lo;
  *extent = (size_t)(hi - lo);
  return true;
}

void hapiDevPoolEnsureArena(int dev, void** base, size_t* extent) {
  HapiDevPoolLock g;
  if (CsvAccess(gpu_manager).device_pool_backend == 1) {
    HapiDevPoolArena* ar = hapiDevPoolVmmArenaLocked(dev);
    if (ar == nullptr) ar = &hapiDevPoolNewVmmHeapLocked(dev);   // maps the startup budget
    *base = (void*)ar->start;
    *extent = (size_t)(ar->end - ar->start);
    return;
  }
  // +gpupoolarenas: the pool's whole budget is opened here, at startup, since
  // the balancer plans only within the arenas the pool has (LBMemoryContract.h).
  int have = 0;
  for (auto& ar : hapi_devpool_arenas) if (ar.device == dev) have++;
  const int want = std::max(1, CsvAccess(gpu_manager).device_pool_arenas);
  for (; have < want; have++) hapiDevPoolNewArenaLocked(0, dev);
  for (auto& ar : hapi_devpool_arenas)
    if (ar.device == dev) { *base = (void*)ar.start; *extent = ar.end - ar.start; return; }
  CmiAbort("hapiDevPoolEnsureArena: no arena after opening one");
}

// +gpushm publication of the vmm heap (ipcHandleCreate): the entry peers read
// the chunk descriptors from, and the synthetic handle a send carries.
bool hapiDevPoolVmmPublish(int dev, hapi_pool_shm_entry* e, int shm_index,
                           hapiIpcMemHandle_t* handle) {
  HapiDevPoolLock g;
  HapiDevPoolArena* ar = hapiDevPoolVmmArenaLocked(dev);
  if (ar == nullptr) return false;
  HapiVmmAlloc* v = (HapiVmmAlloc*)ar->alloc;
  if (!v->exportable)
    CmiAbort("HAPI> [%d] device pool (vmm): device %d cannot export chunks as file "
             "descriptors, so peers cannot map the heap; use +gpupoolalloc buddy",
             CmiMyPe(), dev);
  v->publish(e, shm_index);
  e->dev = v->cudaDev;   // the physical ordinal: importers compare it with their own devices
  hapiVmmMakeHandle(handle, (uint64_t)v->heap.base(), v->heap.reserveBytes(),
                    v->heap.chunkBytes(), e->pid, shm_index, CmiMyNode());
  return true;
}

// The synthetic handle for a pointer in this process's vmm heap, for a send.
bool hapiDevPoolVmmHandleOf(const void* ptr, hapiIpcMemHandle_t* handle, void** base) {
  HapiDevPoolLock g;
  HapiDevPoolArena* ar = hapiDevPoolArenaOfLocked(ptr);
  if (ar == nullptr || !ar->alloc->vmm()) return false;
  HapiVmmAlloc* v = (HapiVmmAlloc*)ar->alloc;
  if (v->shm == nullptr) return false;   // not published: no peers can map it
  hapiVmmMakeHandle(handle, (uint64_t)v->heap.base(), v->heap.reserveBytes(),
                    v->heap.chunkBytes(), v->shm->pid, v->shm_index, CmiMyNode());
  *base = (void*)ar->start;
  return true;
}

void hapiDevPoolNoteRead(const void* ptr, hapiStream_t stream) {
  HapiDevPoolLock g;
  if (hapiDevPoolArenaOfLocked(ptr) == nullptr) return;   // not ours; nothing to order
  // The null stream is the legacy default: recording on it is legal and orders
  // behind everything blocking, which is the safe reading.
  // More than one stream can be reading a block when it is freed: the packer
  // on the migration stream and the application's kernels on its own stream
  // (moe notes both). Keep every distinct (block, stream) pair; the free
  // parks on all of them.
  auto range = hapi_devpool_read_on.equal_range(ptr);
  for (auto it = range.first; it != range.second; ++it)
    if (it->second == (hapiStream_t)stream) return;
  hapi_devpool_read_on.emplace(ptr, (hapiStream_t)stream);
}

void hapiFreeMigratable(void* ptr) {
  if (ptr == NULL) return;
  void* arena_to_free = NULL;
  void (*release)(void*) = NULL;
  {
    std::lock_guard<std::mutex> g(hapi_arena_registry_lock);
    if (!hapi_arena_registry.empty()) {
      auto it = hapi_arena_registry.upper_bound((char*)ptr);
      if (it != hapi_arena_registry.begin()) {
        --it;
        if ((char*)ptr >= it->first &&
            (char*)ptr < it->first + it->second.extent) {
          if (--it->second.live == 0) {
            arena_to_free = it->first;
            auto r = hapi_arena_release.find(it->first);
            if (r != hapi_arena_release.end()) {
              release = r->second;
              hapi_arena_release.erase(r);
            }
            hapi_arena_registry.erase(it);
          }
          // Interior pointer handled: either the arena still has live
          // buffers (nothing to free yet) or it is freed below.
          if (arena_to_free == NULL) return;
        }
      }
    }
  }
  if (arena_to_free != NULL) {
    if (release != NULL) {
      // The base belongs to another allocator (the device pool) and is never
      // recycled by the driver, so its IPC export stays valid and is NOT
      // invalidated: invalidating by this address would resolve to the pool
      // arena that contains it and drop the export every peer is using for
      // every other buffer in it.
      release(arena_to_free);
      return;
    }
    // A cudaMalloc base about to be cudaFree'd: a recycled base would
    // otherwise keep handing out this allocation's IPC handle. Invalidate at
    // the free itself, not on every interior release.
    hapiIpcInvalidateExport(arena_to_free);
    hapiCheck(hapiFree(arena_to_free));
    return;
  }
  // Not arena-interior: an ordinary allocation.
  hapiIpcInvalidateExport(ptr);
  hapiCheck(hapiFree(ptr));
}

// Kernels live in hapi_kernels.cu (nvcc); this file is plain C++.
extern "C" void hapiHashAsyncImpl(const void* p, size_t n, unsigned long long* d_out, hapiStream_t stream);
extern "C" void hapiHashCompareImpl(const unsigned long long* a, const unsigned long long* b, size_t count,
                                    unsigned long long* d_flag, hapiStream_t stream);
void hapiHashAsync(const void* ptr, size_t bytes, unsigned long long* d_out, hapiStream_t stream) {
  hapiHashAsyncImpl(ptr, bytes, d_out, (hapiStream_t)stream);
}
void hapiHashCompareAsync(const unsigned long long* a, const unsigned long long* b, size_t count,
                          unsigned long long* d_flag, hapiStream_t stream) {
  hapiHashCompareImpl(a, b, count, d_flag, (hapiStream_t)stream);
}

/*** Per-chare device footprint tracking (see hapi_portable.h) ***/
//
// The producer behind the LB memory contract: every hapiMalloc/hapiFree made
// while a chare's entry method is running is attributed to that chare's LB
// identity, giving the balancer the per-object resident footprint its
// final-placement feasibility check needs. Identity comes from the same
// source as CUPTI kernel attribution -- the active location record -- and is
// stable across migration within a process. Allocations made outside entry
// methods (startup, comm buffers, arenas) are deliberately unattributed; the
// consumer floors the footprint at the serialized size, so missing
// attribution errs toward refusing a move, never toward approving one.
//
// The device pool records too (hapiDevPoolMalloc/hapiDevPoolFree), at the
// buddy block's size, which is what the allocation holds. The migration
// runtime, which allocates on a chare's behalf outside its entry methods,
// says whom to charge with hapiFootprintBegin/End and hapiFootprintCharge.

#if CMK_LBDB_ON

namespace {
std::unordered_map<LDObjKey, size_t, LDObjKeyHash> gpu_obj_footprint;
// The same footprint as blocks: size -> count, the shape the admission gate
// has to find room for at the destination (hapiObjectBlocks).
std::unordered_map<LDObjKey, std::map<size_t, int>, LDObjKeyHash> gpu_obj_blocks;
std::unordered_map<void*, std::pair<LDObjKey, size_t>> gpu_ptr_owner;
std::mutex gpu_footprint_lock;

void hapiDropBlockLocked(const LDObjKey& key, size_t size) {
  auto ob = gpu_obj_blocks.find(key);
  if (ob == gpu_obj_blocks.end()) return;
  auto b = ob->second.find(size);
  if (b == ob->second.end()) return;
  if (--b->second <= 0) ob->second.erase(b);
  if (ob->second.empty()) gpu_obj_blocks.erase(ob);
}

// Attribution set by the runtime: charge a given element, or nobody.
struct HapiFootprintScope { bool nobody; LDObjKey key; };
thread_local std::vector<HapiFootprintScope> hapi_footprint_scopes;

bool hapiKeyOf(CkLocRec* rec, LDObjKey& key) {
  if (rec == NULL) return false;
  const LDObjHandle& handle = rec->getLdHandle();
  key.omID() = handle.omID();
  key.objID() = handle.objID();
  return true;
}

bool hapiActiveObjKey(LDObjKey& key) {
  if (!hapi_footprint_scopes.empty()) {
    if (hapi_footprint_scopes.back().nobody) return false;
    key = hapi_footprint_scopes.back().key;
    return true;
  }
  return hapiKeyOf(CkActiveLocRec(), key);
}

void hapiChargeLocked(const LDObjKey& key, void* ptr, size_t size) {
  auto prev = gpu_ptr_owner.find(ptr);
  if (prev != gpu_ptr_owner.end()) {   // re-charged: move it, do not double it
    auto owner = gpu_obj_footprint.find(prev->second.first);
    if (owner != gpu_obj_footprint.end())
      owner->second -= std::min(owner->second, prev->second.second);
    hapiDropBlockLocked(prev->second.first, prev->second.second);
  }
  gpu_obj_footprint[key] += size;
  gpu_obj_blocks[key][size]++;
  gpu_ptr_owner[ptr] = std::make_pair(key, size);
}
}

void hapiRecordAlloc(void* ptr, size_t size) {
  if (ptr == NULL) return;
  LDObjKey key{};
  if (!hapiActiveObjKey(key)) return;  // runtime allocation: unattributed
  std::lock_guard<std::mutex> g(gpu_footprint_lock);
  hapiChargeLocked(key, ptr, size);
}

void hapiFootprintBegin(CkLocRec* rec) {
  HapiFootprintScope s;
  s.nobody = !hapiKeyOf(rec, s.key);
  hapi_footprint_scopes.push_back(s);
}

void hapiFootprintEnd() {
  if (!hapi_footprint_scopes.empty()) hapi_footprint_scopes.pop_back();
}

void hapiFootprintCharge(void* ptr, size_t size, CkLocRec* rec) {
  LDObjKey key{};
  if (ptr == NULL || !hapiKeyOf(rec, key)) return;
  size_t blk = 0;
  if (hapiDevPoolBlockSize(ptr, &blk)) size = blk;
  std::lock_guard<std::mutex> g(gpu_footprint_lock);
  hapiChargeLocked(key, ptr, size);
}

void hapiRecordForget(void* ptr) {
  if (ptr == NULL) return;
  std::lock_guard<std::mutex> g(gpu_footprint_lock);
  auto it = gpu_ptr_owner.find(ptr);
  if (it == gpu_ptr_owner.end()) return;  // was not attributed at allocation
  auto owner = gpu_obj_footprint.find(it->second.first);
  if (owner != gpu_obj_footprint.end()) {
    owner->second -= (owner->second >= it->second.second)
                         ? it->second.second : owner->second;
  }
  hapiDropBlockLocked(it->second.first, it->second.second);
  gpu_ptr_owner.erase(it);
}

void hapiRecordFree(void* ptr) {
  if (ptr == NULL) return;
  // Freeing an allocation must retire any IPC handle exported for it. The
  // export cache is keyed by base pointer, so once this address is recycled by
  // a later cudaMalloc the cached handle still names the dead allocation, and
  // hapiIpcExportBuffer hands it out to peers -- who fail to open it. That is
  // the "+gpuipcdirect plus load balancing" abort: the element whose buffer was
  // exported migrates, its buffers are freed and reallocated, and a base gets
  // reused. It is not the sender's own migration racing its sends, which is why
  // registering direct sends with the send interlock did not stop it.
  //
  // hapiFreeMigratable already did this, but only applications that free
  // through that entry point were covered; an ordinary hapiFree left the stale
  // entry behind. Invalidation belongs with the free itself, at every free.
  hapiIpcInvalidateExport(ptr);
  hapiRecordForget(ptr);
}

static const char* hapiDescribeRequester() {
  if (!hapi_footprint_scopes.empty())
    return hapi_footprint_scopes.back().nobody ? "the runtime (a payload or landing)"
                                               : "an arriving element's unpack";
  LDObjKey k{};
  return hapiActiveObjKey(k) ? "the running element" : "no element (startup or runtime)";
}

// The running chare's attributed live bytes; called from the AtSync path,
// where the active object is the chare itself.
size_t hapiCurrentObjectFootprint() {
  LDObjKey key{};
  if (!hapiActiveObjKey(key)) return 0;
  std::lock_guard<std::mutex> g(gpu_footprint_lock);
  auto it = gpu_obj_footprint.find(key);
  return (it == gpu_obj_footprint.end()) ? 0 : it->second;
}

size_t hapiObjectFootprint(CkLocRec* rec) {
  LDObjKey key{};
  if (!hapiKeyOf(rec, key)) return 0;
  std::lock_guard<std::mutex> g(gpu_footprint_lock);
  auto it = gpu_obj_footprint.find(key);
  return (it == gpu_obj_footprint.end()) ? 0 : it->second;
}

void hapiObjectBlocks(CkLocRec* rec, std::vector<std::pair<size_t, int>>& out) {
  out.clear();
  LDObjKey key{};
  if (!hapiKeyOf(rec, key)) return;
  std::lock_guard<std::mutex> g(gpu_footprint_lock);
  auto it = gpu_obj_blocks.find(key);
  if (it == gpu_obj_blocks.end()) return;
  for (const auto& b : it->second) out.emplace_back(b.first, b.second);
}

#else

void hapiRecordAlloc(void* ptr, size_t size) { (void)ptr; (void)size; }
void hapiRecordFree(void* ptr) { (void)ptr; }
void hapiRecordForget(void* ptr) { (void)ptr; }
void hapiFootprintBegin(CkLocRec* rec) { (void)rec; }
void hapiFootprintEnd() {}
void hapiFootprintCharge(void* ptr, size_t size, CkLocRec* rec) {
  (void)ptr; (void)size; (void)rec;
}
static const char* hapiDescribeRequester() { return "an allocation"; }
size_t hapiCurrentObjectFootprint() { return 0; }
size_t hapiObjectFootprint(CkLocRec* rec) { (void)rec; return 0; }
void hapiObjectBlocks(CkLocRec* rec, std::vector<std::pair<size_t, int>>& out) {
  (void)rec; out.clear();
}

#endif  // CMK_LBDB_ON

/******************** DEPRECATED ********************/
// Need to be updated with the Tracing API.
static inline void gpuEventStart(hapiWorkRequest* wr, int* index,
                                 WorkRequestStage event, ProfilingStage stage) {
#ifdef HAPI_TRACE
  GPUManager& csv_gpu_manager = CsvAccess(gpu_manager);
  gpuEventTimer* shared_gpu_events_ = csv_gpu_manager.gpu_events_;
  int shared_time_idx_ = csv_gpu_manager.time_idx_++;
  shared_gpu_events_[shared_time_idx_].cmi_start_time = CmiWallTimer();
  shared_gpu_events_[shared_time_idx_].event_type = event;
  shared_gpu_events_[shared_time_idx_].trace_name = wr->trace_name;
  *index = shared_time_idx_;
  shared_gpu_events_[shared_time_idx_].stage = stage;
#ifdef HAPI_DEBUG
  CmiPrintf("[HAPI] start event %d of WR %s, profiling stage %d\n",
         event, wr->trace_name, stage);
#endif
#endif // HAPI_TRACE
}

/******************** DEPRECATED ********************/
// Need to be updated with the Tracing API.
static inline void gpuEventEnd(int index) {
#ifdef HAPI_TRACE
  GPUManager& csv_gpu_manager = CsvAccess(gpu_manager);
  csv_gpu_manager.gpu_events_[index].cmi_end_time = CmiWallTimer();
  traceUserBracketEvent(csv_gpu_manager.gpu_events_[index].stage,
                        csv_gpu_manager.gpu_events_[index].cmi_start_time,
                        csv_gpu_manager.gpu_events_[index].cmi_end_time);
#ifdef HAPI_DEBUG
  Cmiprintf("[HAPI] end event %d of WR %s, profiling stage %d\n",
          csv_gpu_manager.gpu_events_[index].event_type,
          csv_gpu_manager.gpu_events_[index].trace_name,
          csv_gpu_manager.gpu_events_[index].stage);
#endif
#endif // HAPI_TRACE
}

static inline void hapiWorkRequestStartTime(hapiWorkRequest* wr) {
#ifdef HAPI_INSTRUMENT_WRS
  wr->phase_start_time = CmiWallTimer();
#endif
}

static inline void profileWorkRequestEvent(hapiWorkRequest* wr,
                                           WorkRequestStage event) {
#ifdef HAPI_INSTRUMENT_WRS
  GPUManager& csv_gpu_manager = CsvAccess(gpu_manager);

#if CMK_SMP
  CmiLock(csv_gpu_manager.inst_lock_);
#endif

  if (csv_gpu_manager.init_instr_) {
    double tt = CmiWallTimer() - (wr->phase_start_time);
    int index = wr->chare_index;
    char type = wr->comp_type;
    char phase = wr->comp_phase;

    std::vector<hapiRequestTimeInfo> &vec = csv_gpu_manager.avg_times_[index][type];
    if (vec.size() <= phase) {
      vec.resize(phase+1);
    }
    switch (event) {
      case DataSetup:
        vec[phase].transfer_time += tt;
        break;
      case KernelExecution:
        vec[phase].kernel_time += tt;
        break;
      case DataCleanup:
        vec[phase].cleanup_time += tt;
        vec[phase].n++;
        break;
      default:
        CmiPrintf("[HAPI] invalid event during profileWorkRequestEvent\n");
    }
  }
  else {
    CmiPrintf("[HAPI] instrumentation not initialized!\n");
  }

#if CMK_SMP
  CmiUnlock(csv_gpu_manager.inst_lock_);
#endif
#endif // HAPI_INSTRUMENT_WRS
}

// Create a pool with n_slots slots.
// There are n_buffers[i] buffers for each buffer size corresponding to entry i.
// TODO list the alignment/fragmentation issues with either of two allocation schemes:
// if single, large buffer is allocated for each subpool
// if multiple, smaller buffers are allocated for each subpool
static void createPool(int *n_buffers, int n_slots, std::vector<BufferPool> &pools){
  std::vector<size_t>& mempool_boundaries = CsvAccess(gpu_manager).mempool_boundaries_;

  // initialize pools
  pools.resize(n_slots);
  for (int i = 0; i < n_slots; i++) {
    pools[i].size = mempool_boundaries[i];
    pools[i].head = NULL;
  }

  int device;
  hapiDeviceProp device_prop;
  hapiCheck(hapiGetDevice(&device));
  hapiCheck(hapiGetDeviceProperties(&device_prop, device));

  // divide by # of PEs on physical node and multiply by # of PEs in logical node
  size_t available_memory = device_prop.totalGlobalMem /
                           CmiNumPesOnPhysicalNode(CmiPhysicalNodeID(CmiMyPe()))
                           * CmiMyNodeSize() * HAPI_MEMPOOL_SCALE;

  // pre-calculate memory per size
  int max_buffers = *std::max_element(n_buffers, n_buffers + n_slots);
  // Not `int n_buffers_to_allocate[n_slots]`: n_slots is a runtime parameter,
  // so that is a VLA -- a GNU extension rather than standard C++, and one the
  // arm builds compile with -Werror=vla. The fill constructor also replaces
  // the memset, whose sizeof() only gave the right length while this was a VLA.
  std::vector<int> n_buffers_to_allocate(n_slots, 0);
  size_t buf_size;
  while (available_memory >= mempool_boundaries[0] + sizeof(BufferPoolHeader)) {
    for (int i = 0; i < max_buffers; i++) {
      for (int j = n_slots - 1; j >= 0; j--) {
        buf_size = mempool_boundaries[j] + sizeof(BufferPoolHeader);
        if (i < n_buffers[j] && buf_size <= available_memory) {
          n_buffers_to_allocate[j]++;
          available_memory -= buf_size;
        }
      }
    }
  }

  // pin the host memory
  for (int i = 0; i < n_slots; i++) {
    buf_size = mempool_boundaries[i] + sizeof(BufferPoolHeader);
    int num_buffers = n_buffers_to_allocate[i];

    BufferPoolHeader* hd;
    BufferPoolHeader* previous = NULL;

    // pin host memory in a contiguous block for a slot
    void* pinned_chunk;
    hapiCheck(hapiMallocHost(&pinned_chunk, buf_size * num_buffers));

    // initialize header structs
    for (int j = num_buffers - 1; j >= 0; j--) {
      hd = reinterpret_cast<BufferPoolHeader*>(reinterpret_cast<unsigned char*>(pinned_chunk)
                                     + buf_size * j);
      hd->slot = i;
      hd->next = previous;
      previous = hd;
    }

    pools[i].head = previous;
    pools[i].chunk = pinned_chunk;
#ifdef HAPI_MEMPOOL_DEBUG
    pools[i].num = num_buffers;
#endif
  }
}

static void releasePool(std::vector<BufferPool> &pools){
  int device;
  hapiCheck(hapiGetDevice(&device));
  for (int i = 0; i < pools.size(); i++) {
    void* chunk = pools[i].chunk;
    if (chunk != NULL) {
      hapiCheck(hapiFreeHost(chunk));
    }
  }
  pools.clear();
}

static int findPool(size_t size){
  GPUManager& csv_gpu_manager = CsvAccess(gpu_manager);
  int boundary_array_len = csv_gpu_manager.mempool_boundaries_.size();
  if (size <= csv_gpu_manager.mempool_boundaries_[0]) {
    return 0;
  }
  else if (size > csv_gpu_manager.mempool_boundaries_[boundary_array_len-1]) {
    // create new slot
    csv_gpu_manager.mempool_boundaries_.push_back(size);

    BufferPool newpool;
    hapiCheck(hapiMallocHost((void**)&newpool.head, size + sizeof(BufferPoolHeader)));
    if (newpool.head == NULL) {
      CmiPrintf("[HAPI (%d)] findPool: failed to allocate newpool %d head, size %zu\n",
             CmiMyPe(), boundary_array_len, size);
      return -1;
    }
    newpool.size = size;
    newpool.chunk = (void *)newpool.head;
#ifdef HAPI_MEMPOOL_DEBUG
    newpool.num = 1;
#endif
    csv_gpu_manager.mempool_free_bufs_.push_back(newpool);

    BufferPoolHeader* hd = newpool.head;
    hd->next = NULL;
    hd->slot = boundary_array_len;

    return boundary_array_len;
  }
  for (int i = 0; i < csv_gpu_manager.mempool_boundaries_.size()-1; i++) {
    if (csv_gpu_manager.mempool_boundaries_[i] < size &&
        size <= csv_gpu_manager.mempool_boundaries_[i+1]) {
      return (i + 1);
    }
  }
  return -1;
}

static void* getBufferFromPool(int pool, size_t size){
  BufferPoolHeader* ret;
  GPUManager& csv_gpu_manager = CsvAccess(gpu_manager);

  if (pool < 0 || pool >= csv_gpu_manager.mempool_free_bufs_.size()) {
    CmiPrintf("[HAPI (%d)] getBufferFromPool, pool: %d, size: %zu invalid pool\n",
           CmiMyPe(), pool, size);
#ifdef HAPI_MEMPOOL_DEBUG
    CmiPrintf("[HAPI (%d)] num: %d\n", CmiMyPe(),
           csv_gpu_manager.mempool_free_bufs_[pool].num);
#endif
    CmiAbort("[HAPI] exiting after invalid pool");
  }
  else if (csv_gpu_manager.mempool_free_bufs_[pool].head == NULL) {
    BufferPoolHeader* hd;
    hapiCheck(hapiMallocHost((void**)&hd, sizeof(BufferPoolHeader) +
                             csv_gpu_manager.mempool_free_bufs_[pool].size));
#ifdef HAPI_MEMPOOL_DEBUG
    CmiPrintf("[HAPI (%d)] getBufferFromPool, pool: %d, size: %zu expand by 1\n",
           CmiMyPe(), pool, size);
#endif
    if (hd == NULL) {
      CmiAbort("[HAPI] exiting after NULL hd from pool");
    }
    hd->slot = pool;
    return (void*)(hd + 1);
  }
  else {
    ret = csv_gpu_manager.mempool_free_bufs_[pool].head;
    csv_gpu_manager.mempool_free_bufs_[pool].head = ret->next;
#ifdef HAPI_MEMPOOL_DEBUG
    ret->size = size;
    csv_gpu_manager.mempool_free_bufs_[pool].num--;
#endif
    return (void*)(ret + 1);
  }
  return NULL;
}

static void returnBufferToPool(int pool, BufferPoolHeader* hd) {
  GPUManager& csv_gpu_manager = CsvAccess(gpu_manager);
  hd->next = csv_gpu_manager.mempool_free_bufs_[pool].head;
  csv_gpu_manager.mempool_free_bufs_[pool].head = hd;
#ifdef HAPI_MEMPOOL_DEBUG
  csv_gpu_manager.mempool_free_bufs_[pool].num++;
#endif
}

hapiError_t hapiPoolMalloc(void** ptr, size_t size) {
  GPUManager& csv_gpu_manager = CsvAccess(gpu_manager);

#if CMK_SMP
  CmiLock(csv_gpu_manager.mempool_lock_);
#endif

  if (!csv_gpu_manager.mempool_initialized_) {
    // create pool of page-locked memory
    int sizes[HAPI_MEMPOOL_NUM_SLOTS];
          /*256*/ sizes[0]  =  4;
          /*512*/ sizes[1]  =  2;
         /*1024*/ sizes[2]  =  2;
         /*2048*/ sizes[3]  =  4;
         /*4096*/ sizes[4]  =  2;
         /*8192*/ sizes[5]  =  6;
        /*16384*/ sizes[6]  =  5;
        /*32768*/ sizes[7]  =  2;
        /*65536*/ sizes[8]  =  1;
       /*131072*/ sizes[9]  =  1;
       /*262144*/ sizes[10] =  1;
       /*524288*/ sizes[11] =  1;
      /*1048576*/ sizes[12] =  1;
      /*2097152*/ sizes[13] =  2;
      /*4194304*/ sizes[14] =  2;
      /*8388608*/ sizes[15] =  2;
     /*16777216*/ sizes[16] =  2;
     /*33554432*/ sizes[17] =  1;
     /*67108864*/ sizes[18] =  1;
    /*134217728*/ sizes[19] =  7;
    createPool(sizes, HAPI_MEMPOOL_NUM_SLOTS, csv_gpu_manager.mempool_free_bufs_);
    csv_gpu_manager.mempool_initialized_ = true;

#ifdef HAPI_MEMPOOL_DEBUG
    CmiPrintf("[HAPI (%d)] done creating buffer pool\n", CmiMyPe());
#endif
  }

  int pool = findPool(size);
  if (pool < 0) {
    *ptr = nullptr;

#if CMK_SMP
    CmiUnlock(csv_gpu_manager.mempool_lock_);
#endif

    return hapiErrorMemoryAllocation;
  }
  *ptr = getBufferFromPool(pool, size);

#ifdef HAPI_MEMPOOL_DEBUG
  CmiPrintf("[HAPI (%d)] hapiPoolMalloc size %zu pool %d left %d\n",
      CmiMyPe(), size, pool, csv_gpu_manager.mempool_free_bufs_[pool].num);
#endif

#if CMK_SMP
  CmiUnlock(csv_gpu_manager.mempool_lock_);
#endif

  return hapiSuccess;
}

hapiError_t hapiPoolFree(void* ptr) {
  GPUManager& csv_gpu_manager = CsvAccess(gpu_manager);

  // Check if mempool was initialized
  if (!csv_gpu_manager.mempool_initialized_)
    return hapiErrorInitializationError;

  BufferPoolHeader* hd = ((BufferPoolHeader*)ptr) - 1;
  int pool = hd->slot;

#ifdef HAPI_MEMPOOL_DEBUG
  size_t size = hd->size;
#endif

#if CMK_SMP
  CmiLock(csv_gpu_manager.mempool_lock_);
#endif

  returnBufferToPool(pool, hd);

#if CMK_SMP
  CmiUnlock(csv_gpu_manager.mempool_lock_);
#endif

#ifdef HAPI_MEMPOOL_DEBUG
  CmiPrintf("[HAPI (%d)] hapiPoolFree size %zu pool %d left %d\n",
         CmiMyPe(), size, pool,
         csv_gpu_manager.mempool_free_bufs_[pool].num);
#endif

  return hapiSuccess;
}

#ifdef HAPI_INSTRUMENT_WRS
void hapiInitInstrument(int n_chares, int n_types) {
  GPUManager& csv_gpu_manager = CsvAccess(gpu_manager);

#if CMK_SMP
  CmiLock(csv_gpu_manager.inst_lock_);
#endif

  if (!csv_gpu_manager.init_instr_) {
    csv_gpu_manager.avg_times_.resize(n_chares);
    for (int i = 0; i < n_chares; i++) {
      csv_gpu_manager.avg_times_[i].resize(n_types);
    }
    csv_gpu_manager.init_instr_ = true;
  }

#if CMK_SMP
  CmiUnlock(csv_gpu_manager.inst_lock_);
#endif
}

hapiRequestTimeInfo* hapiQueryInstrument(int chare, char type, char phase) {
  GPUManager& csv_gpu_manager = CsvAccess(gpu_manager);

#if CMK_SMP
  CmiLock(csv_gpu_manager.inst_lock_);
#endif

  if (phase < csv_gpu_manager.avg_times_[chare][type].size()) {
    return &csv_gpu_manager.avg_times_[chare][type][phase];
  }
  else {
    return NULL;
  }

#if CMK_SMP
  CmiUnlock(csv_gpu_manager.inst_lock_);
#endif
}

void hapiClearInstrument() {
  GPUManager& csv_gpu_manager = CsvAccess(gpu_manager);

#if CMK_SMP
  CmiLock(csv_gpu_manager.inst_lock_);
#endif

  for (int chare = 0; chare < csv_gpu_manager.avg_times_.size(); chare++) {
    for (char type = 0; type < csv_gpu_manager.avg_times_[chare].size(); type++) {
      csv_gpu_manager.avg_times_[chare][type].clear();
    }
    csv_gpu_manager.avg_times_[chare].clear();
  }
  csv_gpu_manager.avg_times_.clear();
  csv_gpu_manager.init_instr_ = false;

#if CMK_SMP
  CmiUnlock(csv_gpu_manager.inst_lock_);
#endif
}
#endif // HAPI_INSTRUMENT_WRS

// Poll HAPI events stored in the PE's queue. Current strategy is to process
// all successive completed events in the queue starting from the front.
// TODO Maybe we should make one pass of all events in the queue instead,
// since there might be completed events later in the queue.
// What is the flag path waiting for? For each stream queue on this PE, the
// head entry's kind, its awaited sequence number, and what the slot actually
// holds. Slot 0 means the GPU never wrote it; a different nonzero value means
// the slot was reassigned under it; the matching value means the poll should
// have drained it.
void hapiDumpFlagState() {
#ifndef HAPI_CUDA_CALLBACK
  hapiEventQueues& queues = CpvAccess(hapi_event_queue);
  std::vector<void*> pending;
  int qi = 0;
  for (auto& entry : queues) {
    std::queue<hapiEvent>& q = entry.second;
    if (q.empty()) { qi++; continue; }
    const hapiEvent& h = q.front();
    if (h.event == NULL) {
      pending.push_back((void*)entry.first);
      volatile uint32_t* slot = CpvAccess(hapi_flag_slots) +
          (size_t)(h.flag_seq & (HAPI_FLAG_SLOTS - 1)) * HAPI_FLAG_STRIDE;
      // Is the stream itself done? If the write has not landed AND the
      // stream still has pending work while the GPU is idle, the stream is
      // blocked -- on a cudaStreamWaitEvent whose event never came, not on
      // anything the flag mechanism did.
      const hapiError_t sq = hapiStreamQuery((hapiStream_t)entry.first);
      hapiGetLastError();
      CkPrintf("[hapi] pe %d q%d stream %p depth %zu head=FLAG seq=%u "
               "slot[%u]=%u delta=%d write=%s stream=%s inflight=%d\n",
               CkMyPe(), qi, entry.first, q.size(), h.flag_seq,
               h.flag_seq & (HAPI_FLAG_SLOTS - 1), (uint32_t)*slot,
               (int)(h.flag_seq - (uint32_t)*slot),
               (*slot == h.flag_seq) ? "LANDED" : "NOT-LANDED",
               sq == hapiSuccess ? "IDLE(all work done)"
                 : (sq == hapiErrorNotReady ? "PENDING(blocked)" : "ERR"),
               CpvAccess(hapi_flag_inflight));
    } else {
      CkPrintf("[hapi] pe %d q%d stream %p depth %zu head=EVENT query=%d\n",
               CkMyPe(), qi, entry.first, q.size(),
               (int)hapiEventQuery(h.event));
      hapiGetLastError();
    }
    qi++;
  }
  if (!hapi_trace_on || pending.empty()) return;

  // Live probes: does a flag write land right now on a stream with nothing
  // ahead of it? If the memop lands here but not behind the stalled work,
  // the stall is in the dependency; if it does not land even here, the
  // write path itself is wedged.
  {
    volatile uint32_t* pr = CpvAccess(hapi_flag_probe);
    uint32_t* prd = (uint32_t*)CpvAccess(hapi_flag_probe_dev);
    hapiStream_t ps;
    hapiCheck(hapiStreamCreateWithFlags(&ps, hapiStreamNonBlocking));
    pr[0] = 0; pr[4] = 0;
    hapiDrvResult r1 = hapi_stream_write_value32((hapiDrvStream)ps, (uintptr_t)prd, 0x11u,
                                            hapiStreamWriteValueDefault);
    hapiDrvResult r2 = hapi_memset_d32_async ? hapi_memset_d32_async((uintptr_t)(prd + 4), 0x22u, 1, (hapiDrvStream)ps)
                                        : hapiDrvErrorNotSupported;
    const double t0 = CmiWallTimer();
    while (CmiWallTimer() - t0 < 0.5 && !(pr[0] == 0x11u && (r2 != hapiDrvSuccess || pr[4] == 0x22u))) {}
    const hapiError_t sq = hapiStreamQuery(ps); hapiGetLastError();
    CkPrintf("[probe] pe %d empty-stream memop=%s (issue rc %d) memset=%s (issue rc %d) stream=%s after %.0f ms\n",
             CkMyPe(), pr[0] == 0x11u ? "LANDED" : "NOT-LANDED", (int)r1,
             r2 != hapiDrvSuccess ? "n/a" : (pr[4] == 0x22u ? "LANDED" : "NOT-LANDED"), (int)r2,
             sq == hapiSuccess ? "idle" : "PENDING", (CmiWallTimer() - t0) * 1e3);
    hapiStreamDestroy(ps);
  }
  // Same probe, but enqueued BEHIND the stalled work on each pending stream:
  // a memop that lands proves the stream is not blocked at all, only its
  // earlier flag write was lost.
  for (void* st : pending) {
    volatile uint32_t* pr = CpvAccess(hapi_flag_probe);
    uint32_t* prd = (uint32_t*)CpvAccess(hapi_flag_probe_dev);
    pr[8] = 0;
    hapi_stream_write_value32((hapiDrvStream)st, (uintptr_t)(prd + 8), 0x33u, hapiStreamWriteValueDefault);
    const double t0 = CmiWallTimer();
    while (CmiWallTimer() - t0 < 0.2 && pr[8] != 0x33u) {}
    CkPrintf("[probe] pe %d memop behind stream %p: %s\n", CkMyPe(), st,
             pr[8] == 0x33u ? "LANDED (stream is NOT blocked; earlier write lost)" : "NOT-LANDED (stream blocked)");
  }
  CmiLock(hapi_trace_lock);
  for (void* st : pending) {
    std::vector<void*> path;
    hapiTraceWalk(st, 0, path);
  }
  CmiUnlock(hapi_trace_lock);
#endif
}

// For the channel watchdog (ckrdmadevice.C): what this PE's poll is waiting
// on. One summary line, then up to max_streams streams with queued entries:
// the head entry's kind, its sequence against the slot's current value, and
// whether the stream itself is idle. A flag entry whose slot never reaches its
// sequence on an idle stream is a write that never landed; a busy stream is
// blocked ahead of it.
extern "C" void hapiDumpEventQueues(int max_streams) {
#ifndef HAPI_CUDA_CALLBACK
  hapiEventQueues& queues = CpvAccess(hapi_event_queue);
  int nonempty = 0;
  for (auto& e : queues) if (!e.second.empty()) nonempty++;
  CmiPrintf("[HAPI-QUEUES %d] n_hapi_events=%d flag_inflight=%d streams=%zu with_entries=%d flagpoll=%d my_device=%d\n",
            CmiMyPe(), CpvAccess(n_hapi_events), CpvAccess(hapi_flag_inflight), queues.size(), nonempty,
            (int)hapi_use_flag_poll, CpvAccess(my_device));
  int shown = 0;
  for (size_t qi = 0; qi < queues.size() && shown < max_streams; qi++) {
    auto& e = queues[qi];
    if (e.second.empty()) continue;
    shown++;
    const hapiEvent& h = e.second.front();
    unsigned slotval = 0; int busy = -1; const char* kind = "event"; const char* evq = "-";
    if (h.event == NULL) {
      kind = "flag";
      const size_t idx = h.flag_seq & (HAPI_FLAG_SLOTS - 1);
      slotval = *((volatile uint32_t*)CpvAccess(hapi_flag_slots) + idx * HAPI_FLAG_STRIDE);
      busy = CpvAccess(hapi_flag_busy)[idx];
    } else {
      const hapiError_t r = hapiEventQuery(h.event);
      evq = (r == hapiSuccess) ? "done" : (r == hapiErrorNotReady) ? "pending" : hapiGetErrorName(r);
      hapiGetLastError();
    }
    const hapiError_t q = hapiStreamQuery(e.first);
    hapiGetLastError();
    CmiPrintf("[HAPI-QUEUES %d] stream %p dev %d: %zu queued; head %s seq %u slot=%u busy=%d event=%s; stream %s\n",
              CmiMyPe(), (void*)e.first, hapiStreamDeviceOf(e.first), e.second.size(), kind, (unsigned)h.flag_seq,
              slotval, busy, evq, (q == hapiSuccess) ? "idle" : (q == hapiErrorNotReady) ? "busy" : hapiGetErrorName(q));
  }
#else
  (void)max_streams;
#endif
}

void hapiPollEvents(void* param) {
  struct PollTimer { double t0; ~PollTimer() { if (t0 != 0.0) { HapiCostStats& c = HapiCostStats::get(); HapiCostStats::addmax(c.poll_n, c.poll_us, c.poll_max_us, t0); } } }
    poll_timer{HapiCostStats::on() ? CmiWallTimer() : 0.0};
#ifndef HAPI_CUDA_CALLBACK
  if (CpvAccess(n_hapi_events) <= 0) return;

  hapiEventQueues& queues = CpvAccess(hapi_event_queue);
  // By index, never by iterator (see hapiEventQueues): a callback fired below
  // can register the first callback on a new stream, which appends a queue
  // here and may reallocate the deque's node map. The range-for this was held
  // a pointer into that map and, on the next chunk boundary, read the next
  // chunk's address out of freed memory -- leanmd's sync LB flood (job
  // 22169082) segfaulted here on every rank that crossed a chunk after such
  // an append. Queues appended during the walk are simply visited too.
  for (size_t qi = 0; qi < queues.size(); qi++) {
    std::pair<hapiStream_t, std::queue<hapiEvent>>& entry = queues[qi];
    std::queue<hapiEvent>& queue = entry.second;
    while (!queue.empty()) {
      hapiEvent hev = queue.front();
      if (hev.event == NULL) {
        // Pinned-flag entry: a plain load, no runtime call, no lock. Entries
        // on one stream complete in order, so a slot short of its value means
        // nothing behind it is done either.
        volatile uint32_t* slot = CpvAccess(hapi_flag_slots) +
            (size_t)(hev.flag_seq & (HAPI_FLAG_SLOTS - 1)) * HAPI_FLAG_STRIDE;
        if (*slot != hev.flag_seq) break;
      } else {
        const hapiError_t q = hapiEventQuery(hev.event);
        // A query records its verdict as this thread's last error. cudaErrorNot
        // Ready is this poll's own answer, not a fault of the application's, and
        // leaving it set means the next cudaPeekAtLastError in application code
        // reports a failure that never happened. Anything else is a real problem
        // with the queued event -- say so rather than silently treating it as
        // "not finished yet" and polling it forever.
        if (q == hapiErrorNotReady) {
          hapiGetLastError();
          // this stream's later events cannot be complete; try the next stream
          break;
        } else if (q != hapiSuccess) {
          // A queued event that cannot even be queried is not going to
          // complete; drop it rather than polling it forever.
          hapiGetLastError();
          queue.pop();
          {
            const int d = hapiStreamDeviceOf(entry.first);
            CpvAccess(hapi_event_pool)[d >= 0 ? d : 0].push(hev.event);
          }
          CpvAccess(n_hapi_events)--;
          continue;
        }
      }

      queue.pop(); // TODO: investigate possible race condition with charm4py futures - temporarily resolved by popping here

      // invoke Charm++ callback if one was given
      hev.cb.send(hev.cb_msg);
      if (hev.workToken && hapiDeviceWorkEnd) hapiDeviceWorkEnd(hev.workToken);

      // clean up hapiWorkRequest
      if (hev.wr) {
        hapiWorkRequestCleanup(hev.wr);
      }
      if (hev.event == NULL) {
        CpvAccess(hapi_flag_inflight)--;
        CpvAccess(hapi_flag_busy)[hev.flag_seq & (HAPI_FLAG_SLOTS - 1)] = 0;
      } else {
        const int d = hapiStreamDeviceOf(entry.first);
        CpvAccess(hapi_event_pool)[d >= 0 ? d : 0].push(hev.event);
      }
      CpvAccess(n_hapi_events)--;

      // inform QD that an event was processed
      CmiAssert(hapiQdProcess);
      hapiQdProcess(1);
    }
  }
#endif
}


/*** Per-device stream pool (see hapi.h) ***/
namespace {
// device -> free streams belonging to it, and the reverse map so a stream can
// be returned by a PE that is not on its device (a chare that migrated between
// GPUs releases from its new PE).
std::map<int, std::vector<hapiStream_t>> hapi_stream_free;
std::unordered_map<void*, int> hapi_stream_owner;
// +gpustreams N: at most N application streams per device, SHARED by the chares
// bound to them (least-bound first), instead of one stream per chare. A stream is
// only a GPU-side ordering lane; the device has CUDA_DEVICE_MAX_CONNECTIONS
// hardware channels (32 here), so leanmd's ~1,900 streams per GPU were already
// sharing them, ~60 to a channel, in an order the driver chose. 0 = unbounded.
// Frontier 2026-10-22: hipStreamCreate costs ~13 ms on ROCm 7.14 (20 us on CUDA), and the balancer's net
// arrivals on a device each created one, blocking the destination PE for the whole burst (a channel move's
// hold 184-206 ms, its destination-side landing 46 ms); 32 shared streams per device cut that to 111 / 2.6 ms
// and the channel step from 146 to 109 ms. So HIP defaults to a cap of 32 (+gpustreams 0 restores one per
// chare); CUDA keeps one stream per chare.
#if CMK_HIP
int hapi_stream_cap = 32;
#else
int hapi_stream_cap = 0;
#endif
struct HapiSharedStream { hapiStream_t stream; int bound; };
std::map<int, std::vector<HapiSharedStream>> hapi_stream_shared;
// Constructed before any PE thread exists. It used to be a CmiNodeLock created
// on first use ("if NULL, create"), and the first use is every PE of a process
// at once -- leanmd's cells all acquire their streams in the same broadcast.
// Two PEs could each see NULL and each create a lock; a PE that locked the
// first and unlocked through the overwritten pointer left the first locked for
// good, and a PE still holding the old pointer then blocked on it forever: a
// silent hang in step 1, about one run in six at 4 4 4 (stack: job 22283809).
std::mutex hapi_stream_pool_lock;
}  // namespace

// A stream the RUNTIME owns (the per-PE receive stream, staging, restage): never
// one of the shared application lanes -- a receive copy queued behind a chare's
// kernels is exactly what the receive stream exists to avoid. highPriority asks
// for the device's greatest stream priority (+gpurecvpriority): the copy is tiny
// and gates a whole entry method, so it should not wait its turn behind kernels.
static bool hapi_recv_priority = false;
hapiStream_t hapiAcquireRuntimeStream(bool highPriority) {
  const int dev = CpvAccess(my_device);
  hapiStream_t s;
  int prev = -1;
  hapiCheck(hapiGetDevice(&prev));
  if (prev != dev) hapiCheck(hapiSetDevice(dev));
  bool done = false;
  if (highPriority && hapi_recv_priority) {
    int least = 0, greatest = 0;
    if (hapiDeviceGetStreamPriorityRange(&least, &greatest) == hapiSuccess &&
        hapiStreamCreateWithPriority((hapiStream_t*)&s, hapiStreamNonBlocking, greatest) == hapiSuccess)
      done = true;
    else hapiGetLastError();
  }
  if (!done) hapiCheck(hapiStreamCreateNonBlocking(&s));
  if (prev != dev) hapiCheck(hapiSetDevice(prev));
  std::lock_guard<std::mutex> lk(hapi_stream_pool_lock);
  hapi_stream_owner[(void*)s] = dev;
  return s;
}

static void hapiSetRecvPriority() {
  if (!hapi_recv_priority && CmiMyPe() == 0) CmiPrintf("HAPI> Receive streams at the device's highest stream priority\n");
  hapi_recv_priority = true;
}

static void hapiStreamCapAnnounce() {
  if (hapi_stream_cap > 0 && CmiMyPe() == 0)
    CmiPrintf("HAPI> Application streams: at most %d per GPU, shared by the chares bound to them (HIP default; +gpustreams N, 0 = one per chare)\n", hapi_stream_cap);
}
static void hapiSetStreamCap(int cap) {
  if (cap < 0) cap = 0;
  if (hapi_stream_cap != cap && CmiMyPe() == 0)
    CmiPrintf("HAPI> Application streams: %s%d per GPU%s\n", cap > 0 ? "at most " : "one per chare (", cap > 0 ? cap : 0, cap > 0 ? ", shared by the chares bound to them" : ")");
  hapi_stream_cap = cap;   // same value from every PE, before any chare exists
}

hapiStream_t hapiAcquireStream() {
  // The PE's own device, NOT whatever the calling thread happens to have
  // current. A chare's streams are acquired from its constructor, and the
  // thread running a constructor is not necessarily the PE worker thread --
  // the communication thread, for one, is deliberately put on its own device.
  // Keying off cudaGetDevice() there binds the stream to the wrong GPU, and
  // every completion event later recorded against it (events are created on
  // the current device) fails with cudaErrorInvalidResourceHandle.
  const int dev = CpvAccess(my_device);

  if (hapi_stream_cap > 0) {
    std::lock_guard<std::mutex> lk(hapi_stream_pool_lock);
    std::vector<HapiSharedStream>& lanes = hapi_stream_shared[dev];
    if ((int)lanes.size() < hapi_stream_cap) {
      hapiStream_t s;
      int prev = -1;
      hapiCheck(hapiGetDevice(&prev));
      if (prev != dev) hapiCheck(hapiSetDevice(dev));
      hapiCheck(hapiStreamCreateNonBlocking(&s));
      if (prev != dev) hapiCheck(hapiSetDevice(prev));
      hapi_stream_owner[(void*)s] = dev;
      lanes.push_back(HapiSharedStream{s, 1});
      return s;
    }
    size_t best = 0;
    for (size_t i = 1; i < lanes.size(); i++) if (lanes[i].bound < lanes[best].bound) best = i;
    lanes[best].bound++;
    return lanes[best].stream;
  }

  hapi_stream_pool_lock.lock();
  auto it = hapi_stream_free.find(dev);
  if (it != hapi_stream_free.end() && !it->second.empty()) {
    hapiStream_t s = it->second.back();
    it->second.pop_back();
    hapi_stream_pool_lock.unlock();
    return s;
  }
  hapi_stream_pool_lock.unlock();

  // Non-blocking: a pooled stream must not implicitly synchronize with the
  // legacy default stream, or every chare using one serializes against every
  // other chare's default-stream work.
  // Created with the PE's device actually current, then the caller's device
  // restored -- a stream belongs to whichever device was current at creation.
  hapiStream_t s;
  int prev = -1;
  hapiCheck(hapiGetDevice(&prev));
  if (prev != dev) hapiCheck(hapiSetDevice(dev));
  hapiCheck(hapiStreamCreateNonBlocking(&s));
  if (prev != dev) hapiCheck(hapiSetDevice(prev));
  hapi_stream_pool_lock.lock();
  hapi_stream_owner[(void*)s] = dev;
  hapi_stream_pool_lock.unlock();
  return s;
}

int hapiStreamDeviceOf(hapiStream_t stream) {
  if (stream == NULL) return -1;
  hapi_stream_pool_lock.lock();
  auto it = hapi_stream_owner.find((void*)stream);
  const int d = (it != hapi_stream_owner.end()) ? it->second : -1;
  hapi_stream_pool_lock.unlock();
  return d;
}

int hapiGetDeviceNum() { return CpvAccess(my_device); }

// Priority application streams (hapiAcquirePriorityStream): their own per-device free list, and
// hapi_stream_prio remembers which streams are priority ones so a release returns them to it.
static std::unordered_map<int, std::vector<hapiStream_t>> hapi_stream_free_prio;
static std::unordered_set<void*> hapi_stream_prio;
hapiStream_t hapiAcquirePriorityStream() {
  const int dev = CpvAccess(my_device);
  {
    std::lock_guard<std::mutex> lk(hapi_stream_pool_lock);
    auto it = hapi_stream_free_prio.find(dev);
    if (it != hapi_stream_free_prio.end() && !it->second.empty()) {
      hapiStream_t s = it->second.back();
      it->second.pop_back();
      return s;
    }
  }
  hapiStream_t s;
  int prev = -1;
  hapiCheck(hapiGetDevice(&prev));
  if (prev != dev) hapiCheck(hapiSetDevice(dev));
  int least = 0, greatest = 0;
  hapiCheck(hapiDeviceGetStreamPriorityRange(&least, &greatest));
  hapiCheck(hapiStreamCreateWithPriority((hapiStream_t*)&s, hapiStreamNonBlocking, greatest));
  if (prev != dev) hapiCheck(hapiSetDevice(prev));
  std::lock_guard<std::mutex> lk(hapi_stream_pool_lock);
  hapi_stream_owner[(void*)s] = dev;
  hapi_stream_prio.insert((void*)s);
  return s;
}

void hapiReleaseStream(hapiStream_t stream) {
  if (stream == NULL) return;
  if (hapi_stream_prio.count((void*)stream)) {
    std::lock_guard<std::mutex> lk(hapi_stream_pool_lock);
    auto it = hapi_stream_owner.find((void*)stream);
    hapi_stream_free_prio[it != hapi_stream_owner.end() ? it->second : CpvAccess(my_device)].push_back(stream);
    return;
  }
  if (hapi_stream_cap > 0) {
    // A shared lane is never handed back for exclusive reuse: just unbind.
    std::lock_guard<std::mutex> lk(hapi_stream_pool_lock);
    for (auto& dev : hapi_stream_shared)
      for (auto& lane : dev.second)
        if (lane.stream == stream) { if (lane.bound > 0) lane.bound--; return; }
    return;
  }
  hapi_stream_pool_lock.lock();
  auto it = hapi_stream_owner.find((void*)stream);
  // Back to the device that created it, not the device of whoever is releasing
  // it. A chare that migrated across GPUs releases from its new PE, and the
  // stream still belongs to the old one.
  if (it != hapi_stream_owner.end())
    hapi_stream_free[it->second].push_back(stream);
  hapi_stream_pool_lock.unlock();
}

int hapiCreateStreams() {
  GPUManager& csv_gpu_manager = CsvAccess(gpu_manager);

#if CMK_SMP
  CmiLock(csv_gpu_manager.stream_lock_);
#endif

  int ret = csv_gpu_manager.createStreams();

#if CMK_SMP
  CmiUnlock(csv_gpu_manager.stream_lock_);
#endif

  return ret;
}

hapiStream_t hapiGetStream() {
  GPUManager& csv_gpu_manager = CsvAccess(gpu_manager);

#if CMK_SMP
  CmiLock(csv_gpu_manager.stream_lock_);
#endif

  hapiStream_t ret = csv_gpu_manager.getNextStream();

#if CMK_SMP
  CmiUnlock(csv_gpu_manager.stream_lock_);
#endif

  return ret;
}
#if CMK_LBDB_ON
// Lightweight HAPI, to be invoked after data transfer or kernel execution.
#endif

// How many external-correlation IDs this PE has actually pushed and not yet
// popped. Tracing is switched on and off from inside entry methods, so a push
// can be skipped while its matching pop still runs (or the reverse). Pairing
// the pop against this count rather than against the tracing flag keeps
// CUPTI's stack balanced across those transitions; without it the pop reports
// CUPTI_ERROR_QUEUE_EMPTY and attribution drifts. Each Charm++ PE is its own
// thread, so thread_local is per-PE.
static thread_local int cupti_pushed_depth = 0;
static thread_local int cupti_tag_pushed_depth = 0;
// The detach generation this PE last observed; see GPUManager::cupti_generation_.
static thread_local uint64_t cupti_seen_generation = 0;

// This PE's view of the process-wide token table. Every entry method on a
// migratable chare needs its object's token, and the table behind it is shared
// by every PE in the process, so consulting it under the node-wide lock would
// serialize the whole process on one mutex for the length of the run.
//
// The table is append-only for the lifetime of the process: a token is never
// reassigned, reused for a different identity, or dropped -- hapiClearCuptiData
// deliberately keeps it so that later epochs reuse the same token for the same
// LB identity. That invariant is what makes this cache safe without any
// invalidation protocol: an entry, once correct, stays correct, including
// across migration (the destination PE simply misses once and interns the same
// token the source PE already has). Anything that gains the ability to clear or
// renumber GpuObjectTokenTable must also invalidate these caches.
static thread_local std::unordered_map<LDObjKey, uint64_t, LDObjKeyHash>
    cupti_local_object_tokens;

// Objects on this PE that have joined the current load balancing step.
//
// The measurement window has to close per OBJECT, at its own join, not per PE.
// The PE-wide switch (LBManager::CollectStatsOff) is the wrong granularity in
// both directions: closed at the first join it stops measuring every object on
// the PE that has not joined yet, and closed at the barrier it has already let
// the early joiners run ahead. Under the unsplit barrier the distinction does
// not exist -- a joined element is parked and does no more work -- but under
// +LBAsync it keeps running, and the kernels it launches after joining were
// being charged to its own measured load. That inflated the async load by 11-13%
// against a 2.8-3.9% run-to-run spread on leanmd, non-uniformly across nodes, so
// the strategy saw a different imbalance than the sync run over identical work.
//
// Suppressing the correlation stamp is what "stop measuring this one" means
// here: its later kernels land in cupti_unattributed_kernels_ instead of its
// own bucket, so they still count toward device occupancy but are not billed
// to an object whose load has already been read.
static thread_local std::unordered_set<LDObjKey, LDObjKeyHash>
    cupti_joined_objects;

// Drop this PE's outstanding push count if CUPTI has been detached since we
// last looked -- the stack those pushes referred to no longer exists, so
// popping against it would report CUPTI_ERROR_QUEUE_EMPTY.
static inline void hapiCuptiSyncGeneration(GPUManager& gm) {
  if (cupti_seen_generation != gm.cupti_generation_) {
    cupti_seen_generation = gm.cupti_generation_;
    cupti_pushed_depth = 0;
    cupti_tag_pushed_depth = 0;
    if (CuptiEntryLog* L = t_cupti_entry_log) {   // the spans those pushes opened belong to the detached tracing
      std::lock_guard<std::mutex> g(L->m);
      L->obj.open.clear();
      L->tag.open.clear();
    }
  }
}

void hapiCuptiObjectJoinedStep(const LDObjHandle& handle) {
  LDObjKey key;
  key.omID() = handle.omID();
  key.objID() = handle.objID();
  cupti_joined_objects.insert(key);
}

void hapiCuptiObjectResumed(const LDObjHandle& handle) {
  LDObjKey key;
  key.omID() = handle.omID();
  key.objID() = handle.objID();
  cupti_joined_objects.erase(key);
}

uint64_t hapiCuptiPushObjCorrelation() {
  GPUManager& gm = CsvAccess(gpu_manager);
  // Gated on tracing rather than initialization: this runs on every entry
  // method, so when tracing is off the whole body -- the active-object lookup
  // and two CUPTI calls -- must be skipped, not just wasted.
  if (!gm.cupti_tracing_active_.load(std::memory_order_relaxed)) return 0;
  CuptiHookGuard guard;
  if (!gm.cupti_tracing_active_.load(std::memory_order_seq_cst)) return 0;
  hapiCuptiSyncGeneration(gm);

  // The CUPTI external ID is a process-local token for the complete LB object
  // key. Using CkMigratable::ckGetID() here loses the object-manager identity
  // and aliases equal element IDs from different chare arrays.
  uint64_t object_token = HAPI_CUPTI_NO_OBJECT;
  // An array element, or a group branch registered with ckRegisterWithLB.
  LDObjHandle handle;
  if (CkActiveLdHandle(handle)) {
    LDObjKey key;
    key.omID() = handle.omID();
    key.objID() = handle.objID();

    // Steady state is a PE-local hit: the shared lock is taken only the first
    // time this PE runs a given object, so it costs O(objects that ever run
    // here) acquisitions rather than one per entry method.
    // Joined the step already: leave it unattributed for the rest of the round.
    if (cupti_joined_objects.find(key) != cupti_joined_objects.end()) {
      {
        CuptiEntryLog& L = hapiCuptiEntryLog();
        std::lock_guard<std::mutex> g(L.m);
        hapiCuptiLaneOpen(L.obj, HAPI_CUPTI_NO_OBJECT);
      }
      ++cupti_pushed_depth;
      return HAPI_CUPTI_NO_OBJECT;
    }

    auto cached = cupti_local_object_tokens.find(key);
    if (cached != cupti_local_object_tokens.end()) {
      object_token = cached->second;
    } else {
      {
        std::lock_guard<std::mutex> token_lock(gm.cupti_object_token_lock_);
        if (!gm.cupti_object_tokens_.intern(key, object_token))
          CmiAbort("HAPI: exhausted CUPTI object-correlation tokens");
      }
      cupti_local_object_tokens.emplace(key, object_token);
      if (g_trace != nullptr) hapiTraceObject(object_token, key);
    }
  }

  // Always open a span, even with the sentinel, so that the matching pop always
  // has something to close; an unbalanced stack would mis-attribute every
  // subsequent kernel.
  {
    CuptiEntryLog& L = hapiCuptiEntryLog();
    std::lock_guard<std::mutex> g(L.m);
    hapiCuptiLaneOpen(L.obj, object_token);
  }
  ++cupti_pushed_depth;
  hapiSubmitSetToken(object_token);

  return object_token;
}

void hapiCuptiPopObjCorrelation() {
  hapiSubmitSetToken(0);
  // Runs the generation check even when tracing is off: a detach may have
  // happened between this entry method's push and its pop, and the stale count
  // has to be cleared here rather than on the next push.
  GPUManager& gm = CsvAccess(gpu_manager);
  hapiCuptiSyncGeneration(gm);

  // Pop exactly what was pushed. Checking the tracing flag here instead would
  // pop entries this PE never pushed, once tracing is switched on part-way
  // through an entry method.
  if (cupti_pushed_depth == 0 || !gm.cupti_initialized_) return;
  CuptiHookGuard guard;
  // Detaching (or detached): the generation bump has cleared, or will clear,
  // the depth this pop belongs to.
  if (!gm.cupti_tracing_active_.load(std::memory_order_seq_cst) && !gm.cupti_initialized_) return;
  --cupti_pushed_depth;

  {
    CuptiEntryLog& L = hapiCuptiEntryLog();
    std::lock_guard<std::mutex> g(L.m);
    hapiCuptiLaneClose(L.obj);
  }
}

bool hapiCuptiPushKernelTag(uint64_t workTag) {
  GPUManager& gm = CsvAccess(gpu_manager);
  if (!_lb_args.gpuScaling() ||
      !gm.cupti_tracing_active_.load(std::memory_order_relaxed))
    return false;
  CuptiHookGuard guard;
  if (!gm.cupti_tracing_active_.load(std::memory_order_seq_cst)) return false;
  hapiCuptiSyncGeneration(gm);

  {
    CuptiEntryLog& L = hapiCuptiEntryLog();
    std::lock_guard<std::mutex> g(L.m);
    hapiCuptiLaneOpen(L.tag, workTag);
  }
  ++cupti_tag_pushed_depth;
  return true;
}

void hapiCuptiPopKernelTag() {
  GPUManager& gm = CsvAccess(gpu_manager);
  hapiCuptiSyncGeneration(gm);
  if (cupti_tag_pushed_depth == 0 || !gm.cupti_initialized_) return;
  CuptiHookGuard guard;
  if (!gm.cupti_tracing_active_.load(std::memory_order_seq_cst) && !gm.cupti_initialized_) return;
  --cupti_tag_pushed_depth;

  CuptiEntryLog& L = hapiCuptiEntryLog();
  std::lock_guard<std::mutex> g(L.m);
  hapiCuptiLaneClose(L.tag);
}

// Lightweight HAPI, to be invoked after data transfer or kernel execution.
void hapiAddCallback(hapiStream_t stream, const CkCallback& cb, void* cb_msg) {
#ifndef HAPI_CUDA_CALLBACK
  // record CUDA event
  recordEvent(stream, cb, cb_msg);
#else
  GPUManager& csv_gpu_manager = CsvAccess(gpu_manager);

  /* FIXME works for now (faster too), but CmiAlloc might not be thread-safe
#if CMK_SMP
  CmiLock(csv_gpu_manager.queue_lock_);
#endif
*/

  // create converse message to be delivered to this PE after CUDA callback
  hapiCallbackMessage* conv_msg = (hapiCallbackMessage*)CmiAlloc(sizeof(hapiCallbackMessage)); // FIXME memory leak?
  conv_msg->rank = CmiMyRank();
  conv_msg->cb = cb;
  conv_msg->cb_msg = cb_msg;
  CmiSetHandler(conv_msg, csv_gpu_manager.light_cb_idx_);

  // push into CUDA stream
  hapiCheck(hapiLaunchHostFunc(stream, CUDACallback, (void*)conv_msg));

  /*
#if CMK_SMP
  CmiUnlock(csv_gpu_manager.queue_lock_);
#endif
*/
#endif

  // while there is an ongoing workrequest, quiescence should not be detected
  // even if all PEs seem idle
  CmiAssert(hapiQdCreate);
  hapiQdCreate(1);
}

void hapiAddCallback(hapiStream_t stream, void* cb, void* cb_msg) {
  hapiAddCallback(stream, *(CkCallback*)cb, cb_msg);
}

// A readiness flag: recordEvent's pinned-flag path with no callback behind
// it. The entry still drains through hapiPollEvents, which is what frees the
// slot (and what makes hapiFlagLanded's "later value means landed" sound);
// it fires nothing and marks no device work on an element.
bool hapiFlagIssue(hapiStream_t stream, int* rank, uint32_t* seq) {
#ifndef HAPI_CUDA_CALLBACK
  if (!hapi_use_flag_poll) return false;
  if (CpvAccess(hapi_flag_slots) == NULL) return false;   // no ring on this thread
  const int stream_dev = hapiStreamDeviceOf(stream);
  if (stream_dev >= 0 && stream_dev != CpvAccess(my_device)) return false;
  uint32_t next = CpvAccess(hapi_flag_seq) + 1;
  if (next == 0) next = 1;
  const uint32_t idx = next & (HAPI_FLAG_SLOTS - 1);
  if (CpvAccess(hapi_flag_busy)[idx]) return false;   // ring full: caller uses an event
  CpvAccess(hapi_flag_seq) = next;
  CpvAccess(hapi_flag_busy)[idx] = 1;
  uint32_t* slot_dev =
      (uint32_t*)CpvAccess(hapi_flag_slots_dev) + (size_t)idx * HAPI_FLAG_STRIDE;
  HapiSubmitOp flagOp{};
  flagOp.kind = HAPI_SUBMIT_FLAG; flagOp.value = next; flagOp.stream = stream; flagOp.dst = (void*)slot_dev;
  const hapiDrvResult res = hapiSubmitPush(flagOp) ? hapiDrvSuccess : hapi_flag_use_memset
      ? hapi_memset_d32_async((uintptr_t)slot_dev, next, 1, (hapiDrvStream)stream)
      : hapi_stream_write_value32((hapiDrvStream)stream, (uintptr_t)slot_dev,
                                  next, hapiStreamWriteValueDefault);
  if (res != hapiDrvSuccess)
    CmiAbort("HAPI> %s failed (%d); rerun with +gpueventquery",
             hapi_flag_use_memset ? "cuMemsetD32Async" : "cuStreamWriteValue32", (int)res);
  hapi_flag_writes_total.fetch_add(1, std::memory_order_relaxed);
  hapiTraceFlag((void*)stream, next);
  hapiEvent hev(NULL, CkCallback(CkCallback::ignore), NULL, NULL, /*track=*/false);
  hev.flag_seq = next;
  CpvAccess(hapi_flag_inflight)++;
  hapiQueueFor(stream).push(hev);
  CpvAccess(n_hapi_events)++;
  // Drained by hapiPollEvents like any callback, which processes one; balance it.
  CmiAssert(hapiQdCreate);
  hapiQdCreate(1);
  *rank = CmiMyRank();
  *seq = next;
  return true;
#else
  (void)stream; (void)rank; (void)seq;
  return false;
#endif
}

bool hapiFlagLanded(int rank, uint32_t seq) {
#ifndef HAPI_CUDA_CALLBACK
  if (seq == 0) return true;
  uint32_t* slots = CpvAccessOther(hapi_flag_slots, rank);
  if (slots == NULL) return true;   // no ring there: nothing was ever issued on it
  volatile uint32_t* slot =
      slots + (size_t)(seq & (HAPI_FLAG_SLOTS - 1)) * HAPI_FLAG_STRIDE;
  return (int32_t)(*slot - seq) >= 0;
#else
  (void)rank; (void)seq;
  return true;
#endif
}

void hapiSendMemoryRequest(char* msg, int size)
{
    int cpv_my_device = CpvAccess(my_device);
    
    char server_fifo[BUFFER_SIZE];
    sprintf(server_fifo, SERVER_FIFO_TEMPLATE, cpv_my_device);
    CmiPrintf("Sending request to %s\n", server_fifo);
    
    int server_fd = open(server_fifo, O_WRONLY | O_NONBLOCK);
    if (server_fd == -1) {
        perror("open server FIFO for writing");
        return;
    }

    ssize_t written = write(server_fd, msg, size);
    if (written == -1) {
        perror("write to server FIFO");
    } else {
        //CmiPrintf("Successfully wrote %zd bytes to server FIFO\n", written);
    }
    
    close(server_fd);
}


// hapiError_t hapiMemcpyAsync(void* dst, const void* src, size_t count, cudaMemcpyKind kind, cudaStream_t stream = 0) {
//   hapiError_t err;
// #if CMK_LBDB_ON
//   hapiEvent_t start;

//   cudaEventCreate(&start);
//   cudaEventRecord(start, stream);
// #endif

//   err = cudaMemcpyAsync(dst, src, count, kind, stream);
// #if CMK_LBDB_ON
//   hapiRecordTime(stream, start);  
// #endif
//   return err;
// }

// cudaError_t hapiMemcpy2DAsync(void* dst, size_t dpitch, const void* src, size_t spitch, size_t width, size_t height, cudaMemcpyKind kind, cudaStream_t stream = 0) {
//   cudaError_t err;
// #if CMK_LBDB_ON
//   cudaEvent_t start;

//   cudaEventCreate(&start);
//   cudaEventRecord(start, stream);
// #endif
//   err = cudaMemcpy2DAsync(dst, dpitch, src, spitch, width, height, kind, stream);
// #if CMK_LBDB_ON
//   hapiRecordTime(stream, start);
// #endif
//   return err;
// }


void hapiErrorDie(hapiError_t retCode, const char* code, const char* file, int line) {
  if (retCode != hapiSuccess) {
    fprintf(stderr, "Fatal CUDA Error [%d] %s at %s:%d\n", retCode, hapiGetErrorString(retCode), file, line);
    CmiAbort("Exit due to CUDA error");
  }
}

uint64_t hapiMyDevice() {
  int physical_node_id = CmiPhysicalNodeID(CmiMyPe());
  int my_device = CpvAccess(my_device);
  return (static_cast<uint64_t>(physical_node_id) << 32) | my_device;
}

int hapiDeviceForPe(int pe) {
  return CpvAccessOther(my_device, CmiRankOf(pe));
}

// The device (index on its host) ANY PE of the job drives: exact for a PE of this process, by the
// mapping's own formula for every other process (every process has the same PE count and device
// count: the shared-memory IPC layout on a host is indexed by both). -1 before the mapping ran.
// The balancers price a transfer between two PEs by this: same device, another device of the same
// process (a peer copy), another process on the host (IPC), another host.
int hapiDeviceIndexOfPe(int pe) {
  GPUManager& gm = CsvAccess(gpu_manager);
  if (gm.device_managers.empty() || gm.pes_per_device <= 0) return -1;
  if (CmiNodeOf(pe) == CmiMyNode()) return CpvAccessOther(my_device, CmiRankOf(pe));
  const int G = gm.device_count, K = gm.pes_per_device;
  const int visible = gm.device_count_on_physical_node > 0 ? gm.device_count_on_physical_node : 1;
  const int rank = CmiRankOf(pe);
  const int local = (gm.map_type == Mapping::Block) ? rank / K : rank % G;
  return (G * CmiNodeRankLocal(pe) + local) % visible;
}
int hapiPesPerDevice() { return CsvAccess(gpu_manager).pes_per_device; }
int hapiDevicesPerProcess() { return CsvAccess(gpu_manager).device_count; }
bool hapiDeviceMapIsBlock() { return CsvAccess(gpu_manager).map_type == Mapping::Block; }

#ifdef CMK_LBDB_ON
const GpuDeviceDescriptor& hapiMyDeviceDescriptor() {
  static const GpuDeviceDescriptor kNoDevice;
  GPUManager& gm = CsvAccess(gpu_manager);
  int local_id = CpvAccess(my_device_id);
  if (local_id < 0 || local_id >= (int)gm.device_managers.size()) return kNoDevice;
  // Same lock the CUPTI drain uses, because that is the other caller of the
  // lazy discovery this may have to run.
  {
    std::lock_guard<std::mutex> lk(gm.cupti_prepare_lock_);
    hapiPopulateDeviceProps(gm);
  }
  return gm.device_managers[local_id].descriptor;
}
#endif

int hapiMyDeviceTotalSMs() {
#ifdef CMK_LBDB_ON
  GPUManager& gm = CsvAccess(gpu_manager);
  int local_id = CpvAccess(my_device_id);
  if (local_id < 0 || local_id >= (int)gm.device_managers.size()) return 0;
  DeviceManager& dm = gm.device_managers[local_id];
  if (!dm.props_initialized) {
    // Fallback: query directly for just this device if the lazy population in
    // hapiProcessCuptiBuffers hasn't run yet.
    int count = 0;
    hapiDeviceGetAttribute(&count, hapiDevAttrMultiProcessorCount,
                           dm.global_index);
    return count;
  }
  return dm.multi_processor_count;
#else
  return 0;
#endif
}
