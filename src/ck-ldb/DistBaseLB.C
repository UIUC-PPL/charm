/**
 * Author: gplkrsh2@illinois.edu (Harshitha Menon)
 * Base class for distributed load balancer.
*/

#include "LBOffThread.h"
#include "BaseLB.h"
#include "ckrdmadevice.h"
#include "DistBaseLB.h"
#include "DistBaseLB.def.h"

#if CMK_GPU
#include "gpumanager.h"
#include "hapi.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <chrono>
#include <thread>
#include <sched.h>

// CHARM_LB_CUPTI_OFFTHREAD: build the round's GPU loads on a helper thread and
// let this PE go back to its scheduler until they are ready.
//
// hapiPrepareCuptiLoads parses the whole window's CUPTI records: measured on
// sph2d at 100k particles per patch, 56-65 ms per process per LB step (120 ms
// at worst) with LB every 400 iterations, 225 ms every 2000. It ran on a PE
// thread. Under sync LB that is simply most of the stall (105 ms per step).
// Under +LBAsync the application is running while it happens, but a PE that is
// busy for 60 ms delays its own patches by 60 ms, and in a lock-step halo
// exchange that delays everyone: the overlapped LB step cost exactly what the
// stall did, and async tied sync (job 22284602). The work needs a core, not a
// PE, and every rank has idle cores next to its PEs.
//
// The value is the CPU list the helper may run on (e.g. 8-15,24-31), or 1 for
// any CPU. A thread inherits its creator's affinity, which is the PE's single
// core, so it has to be widened or the helper just time-slices with that PE.
namespace {
struct CuptiBuild {
  CkGroupID gid;
  int first, count;
  int dev = -1;   // the device group whose window this build closes
  std::atomic<bool> done{false};
};

bool cuptiOffThreadCpus(cpu_set_t* set) {
  static const char* spec = lbHelperSpec("CHARM_LB_CUPTI_OFFTHREAD");
  return lbOffThreadCpus(spec, set);   // LBOffThread.h: near | <cpulist> | 1
}

// ONE helper per process, started on first use and reused. A fresh thread per
// LB step was measured first and is a trap: every thread that has called into
// CUPTI leaves per-thread tracing state behind, and leanmd's quiet steps grew
// 115 -> 126 -> 131 -> 132 ms over four LB steps (flat at 118 without it).
struct CuptiWorker {
  std::mutex m;
  std::condition_variable cv;
  CuptiBuild* job = nullptr;
  bool started = false;
};
// Heap-allocated and never destroyed, on purpose. As a static object its
// condition_variable is destroyed by exit() while the detached helper is still
// waiting on it, and pthread_cond_destroy blocks until no waiter is left:
// every process hung in __run_exit_handlers after "Exit called" (caught with
// gdb, job 22284602, leanmd pw_p35_r2).
CuptiWorker& g_cuptiWorker = *new CuptiWorker;

void cuptiWorkerSubmit(CuptiBuild* b, const cpu_set_t& cpus) {
  CuptiWorker& w = g_cuptiWorker;
  std::unique_lock<std::mutex> lk(w.m);
  if (!w.started) {
    w.started = true;
    std::thread([cpus]() {
      sched_setaffinity(0, sizeof(cpus), &cpus);
      CuptiWorker& w = g_cuptiWorker;
      for (;;) {
        CuptiBuild* job;
        {
          std::unique_lock<std::mutex> lk(w.m);
          w.cv.wait(lk, [&w] { return w.job != nullptr; });
          job = w.job;
          w.job = nullptr;
        }
        hapiPrepareCuptiLoads();
        job->done.store(true, std::memory_order_release);
      }
    }).detach();
  }
  w.job = b;
  w.cv.notify_one();
}

// The deferred close (hapi.h): the last arrival marks the window and this poll, from the scheduler loop, waits
// for the records of the work issued before the mark; the build starts when they are in.
struct CuptiCloseWaitCtx { CkGroupID gid; int first, count, dev; };
void cuptiBuildPoll(void* arg);
void cuptiCloseWaitPoll(void* arg) {
  CuptiCloseWaitCtx* w = (CuptiCloseWaitCtx*)arg;
  if (!hapiCuptiCloseComplete()) { CcdCallOnCondition(CcdSCHEDLOOP, cuptiCloseWaitPoll, arg); return; }
  cpu_set_t cpus;
  if (cuptiOffThreadCpus(&cpus)) {
    CuptiBuild* b = new CuptiBuild;
    b->gid = w->gid; b->first = w->first; b->count = w->count; b->dev = w->dev;
    lbHelperSubmit(cpus, [b]() {
      hapiPrepareCuptiLoadsDevice(HAPI_CUPTI_EPOCH_LB_ROUND, b->dev);
      b->done.store(true, std::memory_order_release);
    }, b->dev < 0 ? 0 : b->dev);
    CcdCallOnCondition(CcdSCHEDLOOP, cuptiBuildPoll, (void*)b);
  } else {
    hapiPrepareCuptiLoadsDevice(HAPI_CUPTI_EPOCH_LB_ROUND, w->dev);
    CProxy_DistBaseLB proxy(w->gid);
    for (int r = 0; r < w->count; r++) proxy[w->first + r].gpuLoadsReady();
  }
  delete w;
}
void cuptiBuildPoll(void* arg) {
  CuptiBuild* b = (CuptiBuild*)arg;
  if (!b->done.load(std::memory_order_acquire)) {
    CcdCallOnCondition(CcdSCHEDLOOP, cuptiBuildPoll, arg);
    return;
  }
  CProxy_DistBaseLB proxy(b->gid);
  for (int r = 0; r < b->count; r++) proxy[b->first + r].gpuLoadsReady();
  delete b;
}
}  // namespace
CsvExtern(GPUManager, gpu_manager);
#endif

#define  DEBUGF(x)      // CmiPrintf x;

#if CMK_GLOBAL_LOCATION_UPDATE
extern void UpdateLocation(MigrateInfo& migData);
#endif

void DistBaseLB::barrierDone() {
#if CMK_LBDB_ON
  if (lb_started) {
    return;
  }
  lb_started = true;

  // Close the measurement window where the load is READ.
  //
  // The runtime opens it in three places -- AtSyncWait's early return,
  // ResumeFromSyncHelper, and the arrival-epoch release -- plus the balancer's
  // own constructor. Until now it closed it in exactly one: AtSyncSample(),
  // which returns immediately unless MetaBalancer is on. With MetaBalancer off
  // the window therefore never closed, and CUPTI kernel tracing -- the
  // expensive half of instrumentation -- ran for the whole job after the first
  // resume. That asymmetry is why pic2d, barnes and jacobi2d each hand-roll a
  // window with their own LBTurnInstrumentOn/Off calls: not as an
  // optimisation, but because nothing else switched it off.
  //
  // Here is the symmetric point: the strategy is about to read the loads, so
  // anything measured past here belongs to the next window, and tracing it is
  // pure cost. Reopened at the resume, which is where the runtime already
  // opens it.
  LBTurnInstrumentOff();

  // Hold the AtSync barrier down for the duration of the step, the way
  // CentralLB does. It is what makes the framework one-step-at-a-time: while a
  // step is registering, checkBarrier cannot fire, so no element can start the
  // next one. CentralLB has always done this; the distributed path never did,
  // which only stayed invisible because its elements were parked in AtSync for
  // the whole step. Under +LBAsync they are not.
  {
    LDOMHandle h;
    h.id.id.idx = 0;
    lbmgr->RegisteringObjects(h);
  }
  lbmgr->lb_in_progress = true;

  start_lb_time = 0;

  if (CkNumPes() == 1) {
    MigrationDone(0);
    return;
  }

  start_lb_time = CkWallTimer();
  if (CkMyPe() == 0) {
    if (_lb_args.debug()) {
      CkPrintf("[%s] Load balancing step %d starting at %f\n",
          lbName(), step(),start_lb_time);
    }
  }

#if CMK_GPU
  // Turn the CUPTI kernel timeline into a per-object GPU load before
  // AssembleStats copies object data out of the LB database. Without this
  // LDObjData::gpuTime stays zero for every distributed strategy, so
  // DiffusionLB's across-node dimension (+LBDiffusionGpuDim, which diffuses on
  // gpuTime) sees no load at all and diffuses nothing -- the device work is
  // invisible because the host only enqueues kernels.
  //
  // Built once per process by the LAST PE to reach its barrier, not the first.
  // This barrier is per PE -- it fires when this PE's own objects are at
  // AtSync -- while the CUPTI records are per process. The first arrival used
  // to flush, drain and clear them on the spot, so any PE whose objects were
  // still finishing their kernels lost those kernels from the round: measured
  // on barnes, one PE per process (always the last to arrive) reported zero GPU
  // load for all of its objects at every step, and the balancer spent every
  // step filling a hole that was never there. The earlier arrivals return here
  // and continue from gpuLoadsReady once the build is done.
  // Per process (hapiDeviceGroupSize is its PE count): CUPTI flushes the process whatever context is
  // named, so the last PE of the process closes the window for all its devices at once.
  if (!hapiCuptiArrive((uint64_t)step(), hapiDeviceGroupSize())) return;
  // Mark the window and wait, from the scheduler loop, for the records of the work issued before the mark
  // (the deferred close, hapi.h); the build (helper thread or inline) and the fan-out follow in
  // cuptiCloseWaitPoll. Immediate where there is nothing to wait for.
  hapiCuptiCloseBegin();
  CcdCallOnCondition(CcdSCHEDLOOP, cuptiCloseWaitPoll,
                     (void*)new CuptiCloseWaitCtx{thisgroup, hapiDeviceGroupFirstPe(), hapiDeviceGroupSize(), hapiMyDeviceIndex()});
#else
  gpuLoadsReady();
#endif
#endif
}

// The round's GPU loads are built (or there are none to build): copy this
// PE's share out, assemble its stats and start the strategy.
void DistBaseLB::gpuLoadsReady() {
#if CMK_LBDB_ON
#if CMK_GPU
  // Every PE picks up the normalized loads for its own objects.
  lbmgr->SetObjGPULoad(CsvAccess(gpu_manager).cupti_obj_norm_load_);
  lbmgr->SetObjDriverLoad(CsvAccess(gpu_manager).cupti_obj_api_raw_, hapiCuptiDriverScale());
  if (_lb_args.gpuScaling())
    lbmgr->SetObjGPUCosts(CsvAccess(gpu_manager).cupti_obj_epoch_costs_);
#endif
  AssembleStats();
  thisProxy[CkMyPe()].LoadBalance();
#endif
}

void DistBaseLB::InvokeLB() {
  // Ensure that the strategy starts only after the barrier
  CkCallback cb (CkReductionTarget(DistBaseLB, barrierDone), thisProxy);
  contribute(cb);
}

DistBaseLB::DistBaseLB(const CkLBOptions &opt): CBase_DistBaseLB(opt) {
#if CMK_LBDB_ON
  lbname = (char *)"DistBaseLB";
  thisProxy = CProxy_DistBaseLB(thisgroup);
  startLbFnHdl = lbmgr->AddStartLBFn(this, &DistBaseLB::barrierDone);

  if (opt.getSeqNo() > 0)
    turnOff();

  migrates_completed = 0;
  migrates_expected = 0;
  lb_started = false;
  mig_msgs = NULL;

  myStats.pe_speed = lbmgr->ProcessorSpeed();
  myStats.from_pe = CkMyPe();

  if (_lb_args.statsOn()) {
    lbmgr->CollectStatsOn();
  }
#endif
}

DistBaseLB::~DistBaseLB() {
#if CMK_LBDB_ON
  lbmgr = CProxy_LBManager(_lbmgr).ckLocalBranch();
  if (lbmgr) {
    lbmgr->RemoveStartLBFn(startLbFnHdl);
  }
  if (mig_msgs) {
    delete [] mig_msgs;
  }
#endif
}

// Assemble the stats for the local PE. The stats are collected by the
// LBManager so assemble all the stats.
void DistBaseLB::AssembleStats() {
#if CMK_LBDB_ON
#if CMK_LB_CPUTIMER
  lbmgr->TotalTime(&myStats.total_walltime,&myStats.total_cputime);
  lbmgr->BackgroundLoad(&myStats.bg_walltime,&myStats.bg_cputime);
#else
  lbmgr->TotalTime(&myStats.total_walltime,&myStats.total_walltime);
  lbmgr->BackgroundLoad(&myStats.bg_walltime,&myStats.bg_walltime);
#endif
  lbmgr->IdleTime(&myStats.idletime);

  myStats.move = true;

#if CMK_GPU
  hapiLBDeviceMemory(&myStats.gpu_mem_remaining, &myStats.pool_buff_mem_remaining,
                     &myStats.gpu_pool_arena_bytes, &myStats.gpu_ipc_slots);
  myStats.gpu_pool_capacity_bytes = hapiLBDevicePoolCapacity();
#endif

  myStats.objData.clear();
  myStats.objData.resize(lbmgr->GetObjDataSz());
  lbmgr->GetObjData(myStats.objData.data());

  myStats.commData.clear();
  myStats.commData.resize(lbmgr->GetCommDataSz());
  lbmgr->GetCommData(myStats.commData.data());

  // CHARM_LB_LOADDUMP: is there anything here to balance? Prints this PE's
  // totals and spread in both dimensions, so a strategy that migrates a lot
  // and gains nothing can be told apart from one that never saw any load.
  if (getenv("CHARM_LB_LOADDUMP")) {
    double sw = 0, sg = 0, mw = 0, mg = 0;
    for (const auto& o : myStats.objData) {
      sw += o.wallTime;
      if (o.wallTime > mw) mw = o.wallTime;
#if CMK_GPU
      sg += o.gpuTime;
      if (o.gpuTime > mg) mg = o.gpuTime;
#endif
    }
    CmiPrintf("[LBLOAD pe=%d] objs=%zu wall_sum=%.6f wall_max=%.6f "
              "gpu_sum=%.6f gpu_max=%.6f busyIpcSlots=%d\n",
              CkMyPe(), myStats.objData.size(), sw, mw, sg, mg,
              CkRdmaDeviceBusyIpcSlots());
    fflush(stdout);
  }

  myStats.obj_walltime = 0;
#if CMK_LB_CPUTIMER
  myStats.obj_cputime = 0;
#endif
  const int n_objs = myStats.objData.size();
  for(int i = 0; i < n_objs; i++) {
    myStats.obj_walltime += myStats.objData[i].wallTime;
#if CMK_LB_CPUTIMER
    myStats.obj_cputime += myStats.objData[i].cpuTime;
#endif
  }    
#endif
}

void DistBaseLB::LoadBalance() {
#if CMK_LBDB_ON
  strat_start_time = CkWallTimer();

  if (CkMyPe() == 0 &&  _lb_args.debug()) {
    CkPrintf("DistLB> %s: step %d starting at %f Memory: %f MB\n",
        lbname, step(), strat_start_time, CmiMemoryUsage()/(1024.0*1024.0));
  }

  migrates_expected = 0;
  migrates_completed = 0;
  Strategy(&myStats);
#endif  
}

void DistBaseLB::Migrated(int waitBarrier) {
  // Diagnostic count only. The step's barrier no longer keys off anonymous
  // arrivals: each source PE holds its share of the resume until every move it
  // issued is acked by the destination's registration (the move ledger opened
  // in ProcessMigrationDecision), so an arrival can neither spuriously satisfy
  // the step nor silently fail to.
  migrates_completed++;
  if (getenv("CHARM_DEBUG_MIGRATE"))
    CkPrintf("[MIGRATED %d] step=%d completed=%d expected=%d lb_started=%d\n",
             CkMyPe(), step(), migrates_completed, migrates_expected,
             (int)lb_started);
}

/*
* Migrates the objs from my PE according to the new mapping specified in the
* migrateMsg
*/
void DistBaseLB::ProcessMigrationDecision(LBMigrateMsg *migrateMsg) {
#if CMK_LBDB_ON
  strat_end_time = CkWallTimer() - strat_start_time;
  const int me = CkMyPe();

  // Every lbmgr->Migrate below reaches CkLocRec::recvMigrate synchronously,
  // which records the move in this ledger -- by element identity, before any
  // async deferral. The ledger, not an arrival count, is what holds this PE's
  // share of the step's resume.
  lbmgr->ledgerOpen(step());

  // Migrate messages from me to elsewhere
  for(int i=0; i < migrateMsg->n_moves; i++) {
    MigrateInfo& move = migrateMsg->moves[i];
    if (move.from_pe == me) {
      if (move.to_pe == me) {
        CkAbort("[%i] Error, attempting to migrate object myself to myself\n",
            CkMyPe());
      }
      lbmgr->Migrate(move.obj,move.to_pe);
    } else if (move.from_pe != me) {
      CkPrintf("[%d] Error, strategy wants to move from %d to  %d\n",
          me,move.from_pe,move.to_pe);
      CkAbort("Trying to move objs not on my PE\n");
    }
  }

#if CMK_GLOBAL_LOCATION_UPDATE
  // See the note in DiffusionLB::AcrossNodeLB: with +LBPeerDecision the process
  // residency table settles the transfer-mode question locally, so no PE needs
  // to be told about moves it is not party to.
  if (!_lb_args.lbPeerDecision()) BroadcastLocationUpdate(migrateMsg);
#endif

  if (CkMyPe() == 0) {
    double strat_end_time = CkWallTimer();
    if (_lb_args.debug())
      CkPrintf("%s> Strategy took %fs memory usage: %f MB.\n", lbName(),
          strat_end_time - strat_start_time, CmiMemoryUsage()/(1024.0*1024.0));
  }

  // The message is this function's to free: both callers (DiffusionLB and
  // DistributedLB) build it, hand it over and forget it, and
  // BroadcastLocationUpdate above copies rather than keeping it. Do it before
  // MigrationDone, which can run the whole tail of the step.
  delete migrateMsg;

  // All moves are issued. The step completes on this PE when every one of them
  // has registered at its destination -- deferred moves included, which the
  // old expected/completed arrival counters never waited for on the source
  // side. Fires inline when there is nothing outstanding.
  lbmgr->ledgerClose([this]() {
    if (lb_started) MigrationDone(1);
  });
#endif
}

#if CMK_GLOBAL_LOCATION_UPDATE
// Each PE only knows the moves it is the source of -- there is no global move
// list to hand a bystander PE the way CentralLB::ReceiveMigration does. So the
// source broadcasts its own moves, and every PE that is neither the source nor
// the destination of a given move (the migration mechanics already update
// those two directly) refreshes its cache for it.
void DistBaseLB::BroadcastLocationUpdate(LBMigrateMsg* migrateMsg) {
  if (migrateMsg->n_moves == 0) return;
  void* copy = CkCopyMsg((void**)&migrateMsg);
  thisProxy.ReceiveLocationUpdate((LBMigrateMsg*)copy);
}

// Same purpose as BroadcastLocationUpdate, for a strategy that migrates objects
// one at a time with lbmgr->Migrate() instead of handing down a move list (see
// DiffusionLB's per-object sends). Those migrations are invisible to the
// list-based broadcast above, so without this every PE keeps a stale cached
// location for the moved object -- which for a GPU-direct send means the sender
// picks its transfer mode for the wrong process (see CkRdmaDeviceOnSender).
void DistBaseLB::BroadcastSingleLocationUpdate(const LDObjHandle& h, int to_pe) {
  const int sizes = 1;
  LBMigrateMsg* msg = new(sizes, CkNumPes(), CkNumPes(), 0) LBMigrateMsg;
  msg->n_moves = 1;
  msg->moves[0].index = 0;
  msg->moves[0].obj = h;
  msg->moves[0].from_pe = CkMyPe();
  msg->moves[0].to_pe = to_pe;
  msg->moves[0].async_arrival = 0;
  msg->moves[0].kind = LB_DECISION_MOVE;
  msg->moves[0].rank = 0;
  thisProxy.ReceiveLocationUpdate(msg);
}

#endif  // CMK_GLOBAL_LOCATION_UPDATE -- the broadcasters above are guarded, the
        // receiving entry method below is not (DistBaseLB.ci declares it
        // unconditionally, so the generated .def.h always needs a definition).

void DistBaseLB::ReceiveLocationUpdate(LBMigrateMsg* msg) {
#if !CMK_GLOBAL_LOCATION_UPDATE
  // Nothing broadcasts these when the flag is off; the per-PE location cache is
  // allowed to be stale and a forward is repaired in place instead.
  delete msg;
  return;
#else
  const int me = CkMyPe();
  for (int i = 0; i < msg->n_moves; i++) {
    MigrateInfo& move = msg->moves[i];
    if (move.from_pe != me && move.to_pe != me) {
      UpdateLocation(move);
    }
  }
  delete msg;
#endif
}

void DistBaseLB::MigrationDone(int balancing) {
#if CMK_LBDB_ON
  // Reset the lb_started flag to indicate that the lb is done
  lb_started = false;
  // Increment to next step
  lbmgr->incStep();
  lbmgr->ClearLoads();
  // Open the next interval's window here, PE-wide, symmetric with the
  // LBTurnInstrumentOff() where the strategy read the loads. It used to be
  // opened per chare at the resume and closed per chare at AtSyncStart
  // (cklocation.C): a per-element event driving a process-level flag, so with
  // ~240 elements per PE the FIRST element to reach AtSync stopped billing all
  // the others -- which had not joined and were still working -- and the first
  // to resume started it again. Under -lbasync that gap is the lag, and its
  // length varied per LB step with scheduling order: leanmd 8x8x8 at -lblag 16
  // saw the diffused device bound T_g swing 2.95/7.08/4.43/3.57 where sync held
  // 14.8-16.0, so DiffusionLB re-balanced an already balanced system every step
  // (cross-node moves [1186 636 607 2] against sync's [1181 0 0 0]).
  // Per-element exclusion stays per element: LBObj::joinedStep gates
  // IncrementTime/IncrementGPUTime and cupti_joined_objects gates the CUPTI
  // attribution, so a chare that has joined is still not billed.
  LBTurnInstrumentOn();

#if CMK_GPU
  // Drop the kernel records this round's loads were derived from, so the next
  // round measures the next interval rather than everything since startup
  // (mirrors CentralLB::ProcessMigrationDecision).
  if (CkMyPe() == hapiDeviceGroupFirstPe())
    hapiClearCuptiDataDevice(hapiMyDeviceIndex());
#endif

  // Settle before resuming.
  //
  // A PE that resumes its own objects as soon as its own migrations are done
  // lets them start sending to objects whose new home this PE has not learned
  // yet -- the location updates for another PE's moves are still in flight.
  // For an ordinary message that is harmless: it goes to the stale PE and is
  // forwarded. For a device zerocopy send it is not. The sender picks its
  // transfer mode from that stale location, and picking MEMCPY (same process)
  // means staging nothing at all, because the receiver is expected to read the
  // source pointer directly. When the message is then forwarded into a
  // different process, that pointer is in an address space the receiver cannot
  // read and there is no staged copy to fall back on -- see the abort in
  // CkRdmaDeviceIssueRgets, which is where this used to surface, as an
  // unregistered rdmaGet or an illegal access on an unrelated stream.
  //
  // So on a CUDA build the barrier is not optional -- +LBSyncResume stops being
  // something the user has to know to pass. Elsewhere the existing behaviour is
  // unchanged.
  //
  // The switch itself is +LBSyncResume, which on a CUDA build LBManager now
  // defaults to on. It has to be that one switch rather than a barrier forced
  // from here: DiffusionLB reads the same flag to decide which of its own two
  // paths performs the migrations, so turning the barrier on behind its back
  // leaves the two halves waiting on each other.
  if (balancing && _lb_args.syncResume()) {
    if (getenv("CHARM_DEBUG_MIGRATE"))
      CkPrintf("[MDONE %d] step=%d contributing resume\n", CkMyPe(), step());
    contribute(CkCallback(CkReductionTarget(DistBaseLB, ResumeClients),
                thisProxy));
  }
  else 
    thisProxy [CkMyPe()].ResumeClients(balancing);
#endif
}

// Quiescence would be the stronger guarantee for the barrier above -- it would
// prove the location updates have been *processed* everywhere, not just that the
// moves finished -- but it cannot be used from inside the LB step: DiffusionLB
// drives its own phases off CkStartQD, so an extra quiescence point in the
// middle of its sequence re-fires whichever of its phase callbacks is still
// armed, and the step never completes. The reduction is what is available, and
// it closes the window that matters: every PE has finished migrating and has
// broadcast its location updates before any PE resumes. A residual stale-location
// send remains possible in principle, and is caught precisely by the abort in
// CkRdmaDeviceIssueRgets rather than corrupting memory.

void DistBaseLB::ResumeClients() {
  ResumeClients(1);
}

void DistBaseLB::ResumeClients(int balancing) {
#if CMK_LBDB_ON
  if (getenv("CHARM_DEBUG_MIGRATE"))
    CkPrintf("[RCLIENTS %d] balancing=%d\n", CkMyPe(), balancing);
  DEBUGF(("[%d] ResumeClients. \n", CkMyPe()));

  if (CkMyPe() == 0 && balancing) {
    double end_lb_time = CkWallTimer();
    if (_lb_args.debug())
      CkPrintf("%s> step %d finished at %f duration %f memory usage: %f\n",
          lbName(), step() - 1, end_lb_time, end_lb_time - strat_start_time,
          CmiMemoryUsage() / (1024.0 * 1024.0));
  }

  lbmgr->ResumeClients();

  // Release the barrier only after the clients have been resumed, matching
  // CentralLB::CheckMigrationComplete. Turning it on any earlier would let an
  // element that is still running under +LBAsync start the next step while this
  // one was finishing. Unconditional, to pair exactly with the
  // RegisteringObjects in barrierDone: MigrationDone reaches here once per step
  // either way, and the single-PE path arrives with balancing == 0.
  {
    LDOMHandle h;
    h.id.id.idx = 0;
    lbmgr->DoneRegisteringObjects(h);
  }
  lbmgr->lb_in_progress = false;
#endif
}

void DistBaseLB::Strategy(const LDStats* const stats) {
  int sizes=0;
  LBMigrateMsg* msg = new(sizes, CkNumPes(), CkNumPes(), 0) LBMigrateMsg;
  msg->n_moves = 0;
}
