
/**
 * \addtogroup CkLdb
*/
/*@{*/

#include <algorithm>
#include <charm++.h>
#include "ck.h"
#include "envelope.h"
#include "CentralLB.h"
#include "LBSimulation.h"
#include "LBMemoryContract.h"
#include "LBOffThread.h"
#include <atomic>
#if CMK_GPU
#include "gpumanager.h"
// extern void hapiProcessCuptiBuffers();
// extern void hapiClearCuptiData();
CsvExtern(GPUManager, gpu_manager);
CkpvExtern(int, _lb_obj_index);
#include "hapi.h"
#endif

#define  DEBUGF(x)       // CmiPrintf x;
#define  DEBUG(x)        // x;

#if CMK_MEM_CHECKPOINT
   /* can not handle reduction in inmem FT */
#define USE_REDUCTION         0
#define USE_LDB_SPANNING_TREE 0
#else
#define USE_REDUCTION         1
#define USE_LDB_SPANNING_TREE 1
#endif


#if CMK_GRID_QUEUE_AVAILABLE
CpvExtern(void *, CkGridObject);
#endif

#if CMK_GLOBAL_LOCATION_UPDATE      
extern void UpdateLocation(MigrateInfo& migData); 
#endif

#if CMK_SHRINK_EXPAND
extern "C" void charmrun_realloc(char *s);
extern char willContinue;
extern realloc_state pending_realloc_state;
extern char * se_avail_vector;
extern int mynewpe;
extern char *_shrinkexpand_basedir;
extern int numProcessAfterRestart;
#endif
CkGroupID loadbalancer;
int * lb_ptr;
extern bool load_balancer_created;

static void lbinit()
{
  LBRegisterBalancer<CentralLB>("CentralLB", "CentralLB base class", false);
}

// Below this many PEs a broadcast beats building a scatter tree, so the peer
// decision path falls back to one.
static const int broadcastThreshold = 32;

static void getPredictedLoadWithMsg(BaseLB::LDStats* stats, int count, 
		             LBMigrateMsg *, LBInfo &info, int considerComm);

#if CMK_LBDB_ON
// ---- The strategy off the PE thread: CHARM_LB_STRATEGY_THREAD=near | <cpulist> | 1 ----
// The decision PE runs the strategy as an entry method (MetisLB: ~13 ms at 256 objects), and for
// that long none of its own objects are delivered a message. With a halo exchange every neighbour
// of those objects then waits for them, and the stall spreads: on sph3d at 98M it was half of a
// 46 ms bubble on all 8 GPUs at every async LB event (job 22776188, the +gputrace timeline).
// With this knob the compute half, work(), runs on one persistent helper thread per process while
// the decision PE keeps stepping, and a scheduler-loop poll continues with strategyFinish() --
// the memory-contract verifier and the migrate message, which read PE-local state -- and
// ApplyDecision() as an entry. The helper's CmiGetState() is a zero thread-local, so CkMyPe()
// reads 0 there and the prints keyed on the decision PE still work when that PE is 0.
// Validated with MetisLB; a balancer that reads Ckpv state in work() is not safe here.
namespace {
struct StrategyJob {
  CentralLB* lb;
  BaseLB::LDStats* stats;
  std::atomic<bool> done;
  StrategyJob(CentralLB* l, BaseLB::LDStats* s) : lb(l), stats(s), done(false) {}
};
std::atomic<bool> g_strategyInFlight{false};

bool strategyOffThreadCpus(cpu_set_t* set) {
  static const char* spec = lbHelperSpec("CHARM_LB_STRATEGY_THREAD");
  return lbOffThreadCpus(spec, set);
}

struct CuptiBuildJob {
  CkGroupID gid;
  int first, count;
  std::atomic<bool> done;
  CuptiBuildJob(CkGroupID g, int f, int c) : gid(g), first(f), count(c), done(false) {}
};
void cuptiBuildPoll(void* arg);
// The deferred close (hapi.h): the window is marked at the last arrival; this poll waits from the scheduler
// loop for the records of the work issued before the mark, then starts the build.
struct CuptiCloseWaitJob { CkGroupID gid; int first, count, dev; };
void cuptiCloseWaitPoll(void* arg) {
  CuptiCloseWaitJob* w = (CuptiCloseWaitJob*)arg;
  if (!hapiCuptiCloseComplete()) { CcdCallOnCondition(CcdSCHEDLOOP, cuptiCloseWaitPoll, arg); return; }
  static const char* spec = lbHelperSpec("CHARM_LB_CUPTI_OFFTHREAD");
  cpu_set_t cpus;
  if (lbOffThreadCpus(spec, &cpus)) {
    static bool announced = false;
    if (!announced) {
      announced = true;
      CmiPrintf("CharmLB> PE %d: CUPTI load build on the helper thread (CHARM_LB_CUPTI_OFFTHREAD=%s)\n", CkMyPe(), spec);
    }
    CuptiBuildJob* bj = new CuptiBuildJob(w->gid, w->first, w->count);
    const int dev = w->dev;
    lbHelperSubmit(cpus, [bj, dev]() { hapiPrepareCuptiLoadsDevice(HAPI_CUPTI_EPOCH_LB_ROUND, dev); bj->done.store(true, std::memory_order_release); }, dev < 0 ? 0 : dev);
    CcdCallOnCondition(CcdSCHEDLOOP, cuptiBuildPoll, (void*)bj);
  } else {
    hapiPrepareCuptiLoadsDevice(HAPI_CUPTI_EPOCH_LB_ROUND, w->dev);
    CProxy_CentralLB proxy(w->gid);
    for (int r = 0; r < w->count; r++) proxy[w->first + r].gpuLoadsReady();
  }
  delete w;
}
void cuptiBuildPoll(void* arg) {
  CuptiBuildJob* b = (CuptiBuildJob*)arg;
  if (!b->done.load(std::memory_order_acquire)) {
    CcdCallOnCondition(CcdSCHEDLOOP, cuptiBuildPoll, arg);
    return;
  }
  CProxy_CentralLB proxy(b->gid);
  for (int r = 0; r < b->count; r++) proxy[b->first + r].gpuLoadsReady();
  delete b;
}

void strategyPoll(void* arg) {
  StrategyJob* j = (StrategyJob*)arg;
  if (!j->done.load(std::memory_order_acquire)) {
    CcdCallOnCondition(CcdSCHEDLOOP, strategyPoll, arg);
    return;
  }
  CentralLB* lb = j->lb;
  BaseLB::LDStats* stats = j->stats;
  delete j;
  g_strategyInFlight.store(false);
  lb->strategyFinishOffThread(stats);
}
}  // namespace
#endif

void CentralLB::initLB(const CkLBOptions &opt)
{
#if CMK_LBDB_ON
  lbname = "CentralLB";
  thisProxy = CProxy_CentralLB(thisgroup);
  strategyOffThreadOk = true;
  //  CkPrintf("Construct in %d\n",CkMyPe());
  loadbalancer = thisgroup;
  // create and turn on by default
  startLbFnHdl = lbmgr->
    AddStartLBFn(this, &CentralLB::StartLB);

  // CkPrintf("[%d] CentralLB initLB \n",CkMyPe());
  if (opt.getSeqNo() > 0 || (_lb_args.metaLbOn() && _lb_args.metaLbModelDir() != nullptr))
    turnOff();

  #if CMK_GPU
  // The footprint slot is registered once per PE in _loadbalancerInit; a
  // second claim here would leak layout space and desynchronize the index.
  if (CkpvAccess(_lb_obj_index) < 0) {
    CkpvAccess(_lb_obj_index) = LBRegisterObjUserData(sizeof(size_t));
    lbSetObjFootprintSlot(CkpvAccess(_lb_obj_index));
  }
  #endif

  stats_msg_count = 0;
  statsMsgsList = NULL;
  statsData = NULL;

#if CMK_GPU
  gpuScalingEpoch_ = 0;
  gpuScalingModel_.configure(_lb_args.gpuScalingAlphaMin(),
                             (uint64_t)_lb_args.gpuScalingMinSamples());
#endif

  storedMigrateMsg = NULL;
  reduction_started = false;

  // for future predictor
  if (_lb_predict) predicted_model = new FutureModel(_lb_predict_window);
  else predicted_model=0;
  // register user interface callbacks
  lbmgr->SetupPredictor(this, &CentralLB::predictorOn, &CentralLB::predictorOn, &CentralLB::predictorOff, &CentralLB::changePredictor);

  myspeed = lbmgr->ProcessorSpeed();

  migrates_completed = 0;
  future_migrates_completed = 0;
  migrates_expected = -1;
  future_migrates_expected = -1;
  cur_ld_balancer = _lb_args.central_pe();      // 0 default
  lbdone = 0;
  count_msgs=0;
  statsMsg = NULL;
  use_thread = false;

  if (_lb_args.statsOn()) lbmgr->CollectStatsOn();

  load_balancer_created = true;
#endif
#ifdef TEMP_LDB
	logicalCoresPerNode=physicalCoresPerNode=4;
	logicalCoresPerChip=4;
	numSockets=1;
#endif

}

CentralLB::~CentralLB()
{
#if CMK_LBDB_ON
  delete [] statsMsgsList;
  delete statsData;
  lbmgr = CProxy_LBManager(_lbmgr).ckLocalBranch();
  if (lbmgr) {
    lbmgr->RemoveStartLBFn(startLbFnHdl);
  }
#endif
}

void CentralLB::SetPESpeed(int speed) 
{
  myspeed = speed;
}

int CentralLB::GetPESpeed() 
{
  return myspeed;
}

void CentralLB::CallLB()
{
  #if CMK_LBDB_ON
  DEBUGF(("[%d] CentralLB AtSync step %d!!!!!\n",CkMyPe(),step()));
#if CMK_MEM_CHECKPOINT
  CkSetInLdb();
#endif

  // if num of processor is only 1, nothing should happen
  if (!QueryBalanceNow(step()) || CkNumPes() == 1) {
    MigrationDone(0);
    return;
  }
  
#if CMK_GPU
  // Reduce the kernel timeline to one scalar load per object, while the records
  // are still local -- only those scalars are sent to the central LB.
  //
  // Built by the LAST PE of the process to get here, not the first: CallLB runs
  // when this PE's own objects are at AtSync, but the records are shared by the
  // whole process, and a drain by the first arrival dropped every kernel the
  // other PEs' objects were still finishing. See DistBaseLB::barrierDone for
  // the measurement. The earlier arrivals continue from gpuLoadsReady.
  if (!hapiCuptiArrive((uint64_t)step(), hapiDeviceGroupSize())) return;   // per process (see DistBaseLB)
  // Mark the window and wait, from the scheduler loop, for the records of the work issued before the mark
  // (the deferred close, hapi.h); cuptiCloseWaitPoll then builds -- on the helper thread
  // (CHARM_LB_CUPTI_OFFTHREAD=near | <cpulist> | 1) or inline -- and fans out gpuLoadsReady.
  hapiCuptiCloseBegin();
  CcdCallOnCondition(CcdSCHEDLOOP, cuptiCloseWaitPoll,
                     (void*)new CuptiCloseWaitJob{thisgroup, hapiDeviceGroupFirstPe(), hapiDeviceGroupSize(), hapiMyDeviceIndex()});
#else
  thisProxy [CkMyPe()].ProcessAtSync();
#endif
#endif
}

// The round's GPU loads exist: copy this PE's share out and send its stats.
void CentralLB::gpuLoadsReady()
{
#if CMK_LBDB_ON
#if CMK_GPU
  lbmgr->SetObjGPULoad(CsvAccess(gpu_manager).cupti_obj_norm_load_);
  lbmgr->SetObjDriverLoad(CsvAccess(gpu_manager).cupti_obj_api_raw_, hapiCuptiDriverScale());
  if (_lb_args.gpuScaling())
    lbmgr->SetObjGPUCosts(CsvAccess(gpu_manager).cupti_obj_epoch_costs_);
#endif
  thisProxy [CkMyPe()].ProcessAtSync();
#endif
}

void CentralLB::InvokeLB()
{
  lbmgr->lb_in_progress = true;
  // See DistBaseLB::barrierDone: the window closes where the load is read.
  // Without this the only OFF is AtSyncSample's, which is MetaBalancer-gated,
  // so with MetaBalancer off instrumentation never stopped.
  LBTurnInstrumentOff();
#if CMK_SHRINK_EXPAND
  contribute(CkCallback(CkReductionTarget(CentralLB, CheckForLB), thisProxy[0]));
#else
  CallLB();
#endif
}

void CentralLB::ProcessAtSync()
{
#if CMK_LBDB_ON
  if (reduction_started) return;              // reducton in progress

  if (CkMyPe() == cur_ld_balancer) {
    start_lb_time = CkWallTimer();
  }


  // build message
  BuildStatsMsg();

#if USE_REDUCTION
    // reduction to get total number of objects and comm
    // so that processor 0 can pre-allocate load balancing database
  int counts[2];
  counts[0] = lbmgr->GetObjDataSz();
  counts[1] = lbmgr->GetCommDataSz();

  CkCallback cb;
  if (concurrent)
    cb = CkCallback(CkReductionTarget(CentralLB, ReceiveCounts), thisProxy); // every PE receives counts
  else
    cb = CkCallback(CkReductionTarget(CentralLB, ReceiveCounts), thisProxy[0]);
  contribute(2*sizeof(int), counts, CkReduction::sum_int, cb);
  reduction_started = true;
#else
  SendStats();
#endif
#endif
}

#if defined(TEMP_LDB)
static int  cpufreq_sysfs_write (
                     const char *setting,int proc
                     )
{
char path[100];
snprintf(path,sizeof(path),"/sys/devices/system/cpu/cpu%d/cpufreq/scaling_setspeed",proc);
                FILE *fd = fopen (path, "w");

                if (!fd) {
                        printf("PROC#%d ooooooo666 FILE OPEN ERROR file=%s\n",CkMyPe(),path);
                        return -1;
                }
//                else CkPrintf("PROC#%d opened freq file=%s\n",proc,path);

        fseek ( fd , 0 , SEEK_SET );
        int numw=fprintf (fd, setting);
        if (numw <= 0) {

                fclose (fd);
                printf("FILE WRITING ERROR\n");
                return 0;
        }
//        else CkPrintf("Freq for Proc#%d set to %s numw=%d\n",proc,setting,numw);
        fclose(fd);
        return 1;
}


static int cpufreq_sysfs_read (int proc)
{
        FILE *fd;
        char path[100];
        int i=proc;
        snprintf(path,sizeof(path),"/sys/devices/system/cpu/cpu%d/cpufreq/scaling_setspeed",i);

        fd = fopen (path, "r");

        if (!fd) {
                printf("33 FILE OPEN ERROR file=%s\n",path);
                return 0;
        }
        char val[10];
        fgets(val,10,fd);
        int ff=atoi(val);
        fclose (fd);

        return ff;
}

float CentralLB::getTemp(int cpu)
{
        char val[10];
        FILE *f;
                char path[100];
                snprintf(path,sizeof(path),"/sys/devices/platform/coretemp.%d/temp1_input",cpu);
                f=fopen(path,"r");
                if (!f) {
                        printf("777 FILE OPEN ERROR file=%s\n",path);
                        exit(0);
                }

        if(f==NULL) {printf("ddddddddddddddddddddddddddd\n");exit(0);}
        fgets(val,10,f);
        fclose(f);
        return atof(val)/1000;
}
#endif


// called only on 0 (or every PE if concurrent=true)
void CentralLB::ReceiveCounts(int *counts, int n)
{
  if (!concurrent) CmiAssert(CkMyPe() == 0);
  if (statsData == NULL) statsData = new LDStats;

    // check that only 2 counts are sent
  CmiAssert(n == 2);
  int n_objs = counts[0];
  int n_comm = counts[1];

    // resize database
  statsData->objData.reserve(n_objs);
  statsData->from_proc.reserve(n_objs);
  statsData->to_proc.reserve(n_objs);
  statsData->commData.reserve(n_comm);

  DEBUGF(("[%d] ReceiveCounts: n_objs:%d n_comm:%d\n",CkMyPe(), n_objs, n_comm));
	
  if (concurrent) {
    CkCallback cb = CkCallback(CkReductionTarget(CentralLB, SendStats), thisProxy);
    contribute(cb);
  }
  else thisProxy.SendStats(); // broadcast call to let everybody start to send stats
}

void CentralLB::BuildStatsMsg()
{
#if CMK_LBDB_ON
  // build and send stats
  const int osz = lbmgr->GetObjDataSz();
  const int csz = lbmgr->GetCommDataSz();

  CLBStatsMsg* msg = new CLBStatsMsg(osz, csz);
  _MEMCHECK(msg);
  msg->from_pe = CkMyPe();
  //msg->serial = CrnRand();

#if CMK_LB_CPUTIMER
  lbmgr->GetTime(&msg->total_walltime,&msg->total_cputime,
		   &msg->idletime, &msg->bg_walltime,&msg->bg_cputime);
#else
  lbmgr->GetTime(&msg->total_walltime,&msg->total_walltime,
		   &msg->idletime, &msg->bg_walltime,&msg->bg_walltime);
#endif
#if defined(TEMP_LDB)
	float mytemp=getTemp(CkMyPe()%physicalCoresPerNode);
	int freq=cpufreq_sysfs_read (CkMyPe()%logicalCoresPerNode);
	msg->pe_temp=mytemp;
	msg->pe_speed=freq;
#else
  msg->pe_speed = myspeed;
#endif

#if CMK_GPU
  // printf("CMK_CUDA setting device is %ld\n", hapiMyDevice());
  msg->gpu_device_id = hapiMyDevice();
  msg->gpu_total_sms = hapiMyDeviceTotalSMs();
  if (_lb_args.gpuScaling()) msg->gpu_descriptor = hapiMyDeviceDescriptor();
  // Device memory for the memory contract. Under +gpupool the pool's free
  // bytes and the device's are reported apart: the pool is per process and
  // the device is shared, so LBMemoryModel combines them per device.
  hapiLBDeviceMemory(&msg->gpu_mem_remaining, &msg->pool_buff_mem_remaining,
                     &msg->gpu_pool_arena_bytes, &msg->gpu_ipc_slots);
  msg->gpu_pool_capacity_bytes = hapiLBDevicePoolCapacity();
#endif

  DEBUGF(("Processor %d Total time (wall,cpu) = %f Idle = %f Bg = %f\n", CkMyPe(),msg->total_walltime,msg->idletime,msg->bg_walltime));

  msg->objData.resize(osz);
  lbmgr->GetObjData(msg->objData.data());
  msg->commData.resize(csz);
  lbmgr->GetCommData(msg->commData.data());
//  lbmgr->ClearLoads();
  DEBUGF(("PE %d BuildStatsMsg %d objs, %d comm\n",CkMyPe(),msg->objData.size(),msg->commData.size()));

  if(CkMyPe() == cur_ld_balancer) {
    int count_avail = 0;
    lbmgr->get_avail_vector(msg->avail_vector);
    msg->next_lb = LBManagerObj()->new_lbbalancer();
  }

  CmiAssert(statsMsg == NULL);
  statsMsg = msg;
#endif
}


// called on every processor
void CentralLB::SendStats()
{
#if CMK_LBDB_ON
  CmiAssert(statsMsg != NULL);
  reduction_started = false;

#if USE_LDB_SPANNING_TREE
  if(CkNumPes()>1024)
  {
    if (CkMyPe() == cur_ld_balancer)
      thisProxy[CkMyPe()].ReceiveStats(statsMsg);
    else
      thisProxy[CkMyPe()].ReceiveStatsViaTree(statsMsg);
  }
  else
#endif
  {
    DEBUGF(("[%d] calling ReceiveStats on step %d \n",CmiMyPe(),step()));
    thisProxy[cur_ld_balancer].ReceiveStats(statsMsg);
  }

  statsMsg = NULL;


  {
  // enfore the barrier to wait until centralLB says no
  LDOMHandle h;
  h.id.id.idx = 0;
  lbmgr->RegisteringObjects(h);
  }
#endif
}


static inline bool lbMigDbg() {
  static const bool on = (getenv("CHARM_DEBUG_MIGRATE") != nullptr);
  return on;
}

void CentralLB::Migrated(int waitBarrier)
{

#if CMK_LBDB_ON
  if (waitBarrier) {
	    migrates_completed++;
      DEBUGF(("[%d] An object migrated! %d %d\n",CkMyPe(),migrates_completed,migrates_expected));
    if (lbMigDbg()) {
      CkPrintf("[LBMIG %d] arrived %d/%d\n", CkMyPe(), migrates_completed,
               migrates_expected);
      fflush(stdout);
    }
    if (migrates_completed == migrates_expected) {
      MigrationDone(1);
    }
  }
  else {
    future_migrates_completed ++;
    DEBUGF(("[%d] An object migrated with no barrier! %d expected: %d\n",CkMyPe(),future_migrates_completed,future_migrates_expected));
    if (lbMigDbg()) {
      CkPrintf("[LBMIG %d] async arrived %d/%d\n", CkMyPe(),
               future_migrates_completed, future_migrates_expected);
      fflush(stdout);
    }
    if (future_migrates_completed == future_migrates_expected)  {
	CheckMigrationComplete();
    }
  }
#endif
}

void CentralLB::MissMigrate(int waitForBarrier)
{
  Migrated(waitForBarrier);
}

// build a complete data from bufferred messages
// not used when USE_REDUCTION = 1
void CentralLB::buildStats()
{
    // copy all data in individual messages to this big structure
    // Space has already been reserved in ReceiveStats
    for (int pe=0; pe<CkNumPes(); pe++) {
       CLBStatsMsg *msg = statsMsgsList[pe];
       if(msg == NULL) continue;
       statsData->objData.insert(statsData->objData.end(), msg->objData.begin(), msg->objData.end());
       statsData->from_proc.insert(statsData->from_proc.end(), msg->objData.size(), pe);
       statsData->to_proc.insert(statsData->to_proc.end(), msg->objData.size(), pe);
       statsData->commData.insert(statsData->commData.end(), msg->commData.begin(), msg->commData.end());
       // free the memory
       delete msg;
       statsMsgsList[pe]=0;
    }
    statsData->n_migrateobjs =
        std::count_if(statsData->objData.begin(), statsData->objData.end(),
                      [](const LDObjData& data) { return data.migratable; });
}

// deposit one processor data at a time, note database is pre-allocated
// to have enough space
// used when USE_REDUCTION = 1
void CentralLB::depositData(CLBStatsMsg *m)
{
  if (m == NULL) return;

  const int n_objs = m->objData.size();
  const int n_comm = m->commData.size();

  const int pe = m->from_pe;
  struct ProcStats &procStat = statsData->procs[pe];
#if defined(TEMP_LDB)
	procStat.pe_temp=m->pe_temp;
	procStat.pe_speed=m->pe_speed;
#endif

  procStat.pe = pe;
  procStat.total_walltime = m->total_walltime;
  procStat.idletime = m->idletime;
  procStat.bg_walltime = m->bg_walltime;
#if CMK_LB_CPUTIMER
  procStat.total_cputime = m->total_cputime;
  procStat.bg_cputime = m->bg_cputime;
#endif
  procStat.pe_speed = m->pe_speed;
#if CMK_GPU
  procStat.gpu_device_id = m->gpu_device_id;
  procStat.gpu_total_sms = m->gpu_total_sms;
  procStat.gpu_mem_remaining = m->gpu_mem_remaining;
  procStat.pool_buff_mem_remaining = m->pool_buff_mem_remaining;
  procStat.gpu_pool_arena_bytes = m->gpu_pool_arena_bytes;
  procStat.gpu_pool_capacity_bytes = m->gpu_pool_capacity_bytes;
  procStat.gpu_ipc_slots = m->gpu_ipc_slots;
  procStat.gpu_descriptor = m->gpu_descriptor;
#endif

  //procStat.utilization = 1.0;
  procStat.available = true;
  procStat.n_objs = n_objs;

  CmiAssert(statsData->objData.size() + n_objs <= statsData->objData.capacity());
  statsData->objData.insert(statsData->objData.end(), m->objData.begin(), m->objData.end());
  statsData->from_proc.insert(statsData->from_proc.end(), n_objs, pe);
  statsData->to_proc.insert(statsData->to_proc.end(), n_objs, pe);

  CmiAssert(statsData->commData.size() + n_comm <= statsData->commData.capacity());
  statsData->commData.insert(statsData->commData.end(), m->commData.begin(), m->commData.end());

  statsData->n_migrateobjs +=
      std::count_if(m->objData.begin(), m->objData.end(),
                    [](const LDObjData& data) { return data.migratable; });
  delete m;
}

void CentralLB::ReceiveStatsFromRoot(CkMarshalledCLBStatsMessage &&msg) {
#if CMK_LBDB_ON
  if (CkMyPe() == cur_ld_balancer) return;
  else ReceiveStats(std::move(msg));
#endif
}

void CentralLB::ReceiveStats(CkMarshalledCLBStatsMessage &&msg)
{
#if CMK_LBDB_ON
  if (concurrent && (CkMyPe() == cur_ld_balancer)) {
    thisProxy.ReceiveStatsFromRoot(msg);  // broadcast stats to all other PEs
  }

  if (statsMsgsList == NULL) {
    statsMsgsList = new CLBStatsMsg*[CkNumPes()];
    CmiAssert(statsMsgsList != NULL);
    for(int i=0; i < CkNumPes(); i++)
      statsMsgsList[i] = 0;
  }
  if (statsData == NULL) statsData = new LDStats;

    //  loop through all CLBStatsMsg in the incoming msg
  int count = msg.getCount();
  int n_objs = 0, n_comm = 0;
  for (int num = 0; num < count; num++) 
  {
    CLBStatsMsg *m = msg.getMessage(num);
    CmiAssert(m!=NULL);
    const int pe = m->from_pe;
    const int msg_n_objs = m->objData.size();
    DEBUGF(("Stats msg received, %d %d %d %p step %d\n", pe,stats_msg_count,msg_n_objs,m,step()));
	
    if (!m->avail_vector.empty()) {
      LBManagerObj()->set_avail_vector(m->avail_vector, m->next_lb);
    }

    if (statsMsgsList[pe] != 0) {
      CkPrintf("*** Unexpected CLBStatsMsg in ReceiveStats from PE %d ***\n",
	     pe);
    } else {
      statsMsgsList[pe] = m;
#if USE_REDUCTION
      depositData(m);
#else
      // store per processor data right away
      struct ProcStats &procStat = statsData->procs[pe];
      procStat.pe = pe;
      procStat.total_walltime = m->total_walltime;
      procStat.idletime = m->idletime;
      procStat.bg_walltime = m->bg_walltime;
#if CMK_LB_CPUTIMER
      procStat.total_cputime = m->total_cputime;
      procStat.bg_cputime = m->bg_cputime;
#endif
      procStat.pe_speed = m->pe_speed;
#if CMK_GPU
      procStat.gpu_device_id = m->gpu_device_id;
      procStat.gpu_total_sms = m->gpu_total_sms;
      procStat.gpu_mem_remaining = m->gpu_mem_remaining;
      procStat.pool_buff_mem_remaining = m->pool_buff_mem_remaining;
      procStat.gpu_pool_arena_bytes = m->gpu_pool_arena_bytes;
      procStat.gpu_pool_capacity_bytes = m->gpu_pool_capacity_bytes;
      procStat.gpu_ipc_slots = m->gpu_ipc_slots;
      procStat.gpu_descriptor = m->gpu_descriptor;
#endif
      //procStat.utilization = 1.0;
      procStat.available = true;
      procStat.n_objs = msg_n_objs;

      n_objs += msg_n_objs;
      n_comm += m->commData.size();
#if defined(TEMP_LDB)
			procStat.pe_temp=m->pe_temp;
			procStat.pe_speed=m->pe_speed;
#endif
#endif

      stats_msg_count++;
    }
  }    // end of for

#if ! USE_REDUCTION
  statsData->objData.reserve(n_objs);
  statsData->from_proc.reserve(n_objs);
  statsData->to_proc.reserve(n_objs);
  statsData->commData.reserve(n_comm);
#endif

  const int clients = CkNumPes();

  DEBUGF(("THIS POINT count = %d, clients = %d\n",stats_msg_count,clients));
 
  if (stats_msg_count == clients) {
	DEBUGF(("[%d] All stats messages received \n",CmiMyPe()));
    statsData->procs.resize(stats_msg_count);
    if (use_thread)
        thisProxy[CkMyPe()].t_LoadBalance();
    else
        thisProxy[CkMyPe()].LoadBalance();
  }
#endif
}

/** added by Abhinav for receiving msgs via spanning tree */
void CentralLB::ReceiveStatsViaTree(CkMarshalledCLBStatsMessage &&msg)
{
#if CMK_LBDB_ON
	CmiAssert(CkMyPe() != 0);
	bufMsg.add(std::move(msg));         // buffer messages
	count_msgs++;
	//CkPrintf("here %d\n", CkMyPe());
	if (count_msgs == st.numChildren+1) {
		if(st.parent == 0)
		{
			thisProxy[0].ReceiveStats(bufMsg);
			//CkPrintf("from %d\n", CkMyPe());
		}
		else
			thisProxy[st.parent].ReceiveStatsViaTree(bufMsg);
		count_msgs = 0;
                bufMsg.free();
	} 
#endif
}

#if CMK_REPLAYSYSTEM
static LDHandle *loadBalancer_pointers;
#endif

#if CMK_GPU
// Every registered device type this job currently has, one entry per PE that
// reports one, so the reference policy can pick the most common.
static void collectGpuTypes(const BaseLB::LDStats* stats,
                            std::vector<uint64_t>& available)
{
  for (const BaseLB::ProcStats& proc : stats->procs) {
    if (!proc.available) continue;
    if (proc.gpu_descriptor.typeId == 0) continue;
    available.push_back(proc.gpu_descriptor.typeId);
  }
}

static LDObjKey objKeyOf(const LDObjData& obj) {
  LDObjKey key;
  key.omID() = obj.omID();
  key.objID() = obj.objID();
  return key;
}

// The GPU type an object's work was measured on. Prefer what the summary itself
// records, since that is the device the kernels actually ran on; fall back to
// the descriptor of the PE it is currently assigned to.
static uint64_t sourceTypeOf(const LDObjData& obj, const BaseLB::LDStats* stats,
                             int pe) {
  if (obj.gpuCosts.sourceTypeId != 0) return obj.gpuCosts.sourceTypeId;
  if (pe < 0 || pe >= (int)stats->procs.size()) return 0;
  return stats->procs[pe].gpu_descriptor.typeId;
}

void CentralLB::updateGpuScalingModel(LDStats* stats)
{
  if (!_lb_args.gpuScaling() || stats == NULL) return;

  const double t0 = CmiWallTimer();

  // Register every type before choosing a reference: registerGpuType refuses a
  // score that disagrees with one already recorded for the same type id, which
  // would mean two PEs described the same hardware differently.
  size_t conflicting_types = 0;
  for (const ProcStats& proc : stats->procs) {
    const GpuDeviceDescriptor& descriptor = proc.gpu_descriptor;
    if (descriptor.typeId == 0) continue;
    if (!gpuScalingModel_.registerGpuType(descriptor.typeId, descriptor.peakRateScore))
      conflicting_types++;
  }
  if (conflicting_types > 0) {
    CmiPrintf("CharmLB> GPU scaling: %zu PE(s) reported a rate score that "
              "disagrees with an earlier report of the same device type; the "
              "first report is kept\n", conflicting_types);
  }

  std::vector<uint64_t> available;
  collectGpuTypes(stats, available);
  const uint64_t previousReference =
      gpuScalingModel_.hasReference() ? gpuScalingModel_.referenceType() : 0;
  gpuScalingModel_.selectReference(available);
  if (gpuScalingModel_.hasReference() &&
      gpuScalingModel_.referenceType() != previousReference && _lb_args.debug() > 0) {
    // Rebasing is free here because the model stores raw per-type log costs and
    // derives the relative factors on demand, so no stored moment is rewritten.
    CmiPrintf("CharmLB> GPU scaling: reference type is now %" PRIu64
              " (was %" PRIu64 ")\n",
              gpuScalingModel_.referenceType(), previousReference);
  }

  // Score last epoch's claims before this epoch's observations move the model,
  // so the error measured is the error of the prediction that was actually made.
  scoreGpuShadowPredictions(stats);

  gpuScalingEpoch_++;
  size_t observed = 0, rejected = 0;
  // objData order is deterministic (PE order, then arrival order within a PE),
  // and every concurrent strategy replica walks it the same way, so all
  // replicas see identical observations in identical order.
  for (const LDObjData& obj : stats->objData) {
    if (obj.gpuCosts.components.empty()) continue;
    size_t objRejected = 0;
    observed += gpuScalingModel_.observeObjectCosts(obj.gpuCosts, gpuScalingEpoch_,
                                                    &objRejected);
    rejected += objRejected;
  }

  // Shadow predictions for the next epoch: one per object, for every type this
  // job currently has. Only the object's own destination is retained for
  // scoring, but pricing every type is what a destination-aware strategy will
  // ask for, so its cost is measured here too.
  gpuShadow_.clear();
  std::vector<uint64_t> distinctTypes = available;
  std::sort(distinctTypes.begin(), distinctTypes.end());
  distinctTypes.erase(std::unique(distinctTypes.begin(), distinctTypes.end()),
                      distinctTypes.end());

  size_t predictions = 0, unpriceable = 0;
  for (size_t i = 0; i < stats->objData.size(); i++) {
    const LDObjData& obj = stats->objData[i];
    if (obj.gpuCosts.components.empty() && obj.gpuCosts.unmodeledGpuTime <= 0.0)
      continue;

    GpuObjectEpochCosts costs = obj.gpuCosts;
    if (costs.sourceTypeId == 0)
      costs.sourceTypeId = sourceTypeOf(obj, stats, stats->from_proc[i]);

    for (uint64_t type : distinctTypes) {
      double predicted = 0.0;
      GpuPredictionSource source = GpuPredictionSource::Invalid;
      if (!gpuScalingModel_.predictObjectCost(costs, type,
                                              GpuCostMetric::NormalizedDemand,
                                              predicted, &source)) {
        unpriceable++;
        continue;
      }
      predictions++;
      if (type != costs.sourceTypeId) continue;

      // Same-type prediction is the one that can be checked next epoch without
      // assuming the object migrates anywhere in particular.
      GpuShadowPrediction claim;
      claim.destinationType = type;
      claim.predictedCost = predicted;
      claim.source = source;
      claim.epoch = gpuScalingEpoch_;
      gpuShadow_[objKeyOf(obj)] = claim;
    }
  }

  const double elapsed = CmiWallTimer() - t0;
  if (_lb_args.debug() > 0) {
    CmiPrintf("CharmLB> GPU scaling epoch %" PRIu64 ": types=%zu entries=%zu "
              "observed=%zu rejected=%zu predictions=%zu unpriceable=%zu "
              "shadow=%zu in %.4fs\n",
              gpuScalingEpoch_, distinctTypes.size(),
              gpuScalingModel_.entryCount(), observed, rejected, predictions,
              unpriceable, gpuShadow_.size(), elapsed);
  }
  if (_lb_args.debug() > 1) reportGpuScalingModel(stats);
}

void CentralLB::scoreGpuShadowPredictions(LDStats* stats)
{
  if (gpuShadow_.empty()) return;

  size_t scored = 0;
  for (size_t i = 0; i < stats->objData.size(); i++) {
    const LDObjData& obj = stats->objData[i];
    auto claim = gpuShadow_.find(objKeyOf(obj));
    if (claim == gpuShadow_.end()) continue;

    // Only score the claim if the object really did run on the type it was
    // priced for; otherwise the difference is a change of hardware, not a
    // prediction error.
    const uint64_t actualType = sourceTypeOf(obj, stats, stats->from_proc[i]);
    if (actualType != claim->second.destinationType) continue;

    const double actual = obj.gpuCosts.totalDemand();
    if (gpuAccuracy_.observe(claim->second.predictedCost, actual,
                             claim->second.source))
      scored++;
  }

  if (_lb_args.debug() > 0 && scored > 0) {
    static const GpuPredictionSource kSources[] = {GpuPredictionSource::PriorOnly,
                                                   GpuPredictionSource::Mixed,
                                                   GpuPredictionSource::Calibrated};
    static const char* kNames[] = {"prior-only", "mixed", "calibrated"};
    for (int s = 0; s < 3; s++) {
      const uint64_t n = gpuAccuracy_.samples(kSources[s]);
      if (n == 0) continue;
      double median = 0.0, p95 = 0.0;
      gpuAccuracy_.quantileAbsolutePercentageError(kSources[s], 0.5, median);
      gpuAccuracy_.quantileAbsolutePercentageError(kSources[s], 0.95, p95);
      CmiPrintf("CharmLB> GPU scaling accuracy [%s]: n=%" PRIu64
                " mean_ape=%.4f median_ape=%.4f p95_ape=%.4f%s\n",
                kNames[s], n, gpuAccuracy_.meanAbsolutePercentageError(kSources[s]),
                median, p95,
                gpuAccuracy_.truncated(kSources[s]) > 0
                    ? " (percentiles over a truncated sample)" : "");
    }
  }
}

void CentralLB::reportGpuScalingModel(LDStats* stats) const
{
  std::vector<uint64_t> available;
  collectGpuTypes(stats, available);
  std::sort(available.begin(), available.end());
  available.erase(std::unique(available.begin(), available.end()), available.end());

  for (uint64_t type : available) {
    const GpuDeviceTypeInfo* info = gpuScalingModel_.findGpuType(type);
    if (info == NULL) continue;
    CmiPrintf("CharmLB>   type=%" PRIu64 " rate=%.0f%s\n", type, info->peakRateScore,
              gpuScalingModel_.hasReference() &&
                      gpuScalingModel_.referenceType() == type
                  ? " (reference)" : "");
  }
}

double CentralLB::predictGpuCost(const LDObjData& obj,
                                 const ProcStats& destination) const
{
  const double measured = obj.gpuTime;
  if (!_lb_args.gpuScaling()) return measured;
  if (destination.gpu_descriptor.typeId == 0) return measured;

  double predicted = 0.0;
  if (!gpuScalingModel_.predictObjectCost(obj.gpuCosts,
                                          destination.gpu_descriptor.typeId,
                                          GpuCostMetric::NormalizedDemand, predicted))
    return measured;
  return predicted;
}
#endif

void CentralLB::LoadBalance()
{
#if CMK_LBDB_ON
  int proc;
  const int clients = CkNumPes();

#if ! USE_REDUCTION
  // build data
  buildStats();
#else
  for (proc = 0; proc < clients; proc++) statsMsgsList[proc] = NULL;
#endif

  lbmgr->ResetAdaptive();
  if (!_lb_args.samePeSpeed()) statsData->normalize_speed();

  if (_lb_args.debug() && (CkMyPe() == cur_ld_balancer))
      CmiPrintf("\nCharmLB> %s: PE [%d] step %d starting at %f Memory: %f MB\n",
		  lbname, cur_ld_balancer, step(), start_lb_time,
		  CmiMemoryUsage()/(1024.0*1024.0));

  // if we are in simulation mode read data
  if (LBSimulation::doSimulation) simulationRead();

  const char *availVector = lbmgr->availVector();
  for(proc = 0; proc < clients; proc++)
      statsData->procs[proc].available = (bool)availVector[proc];


  removeCommDataOfDeletedObjs(statsData);
  preprocess(statsData);

#if CMK_GPU
  // After the statistics are complete and before any strategy runs. Shadow
  // mode only: nothing here feeds placement.
  updateGpuScalingModel(statsData);
#endif

//    CkPrintf("Before Calling Strategy\n");

  if (_lb_args.printSummary()) {
      LBInfo info(clients);
        // not take comm data
      info.getInfo(statsData, clients, 0);
      LBRealType mLoad, mCpuLoad, totalLoad;
      info.getSummary(mLoad, mCpuLoad, totalLoad);
      int nmsgs, nbytes;
      statsData->computeNonlocalComm(nmsgs, nbytes);
      CkPrintf("[%d] Load Summary (before LB): max (with bg load): %f max (obj only): %f average: %f at step %d nonlocal: %d msgs %.2fKB.\n", CkMyPe(), mLoad, mCpuLoad, totalLoad/clients, step(), nmsgs, 1.0*nbytes/1024);
      // Which PE is the max, and whether its excess is object time or
      // background: the summary's two maxima alone cannot say, and on sph2d
      // the first-window max PE read 1.27 to 1.79x the mean across identical
      // runs while the mean held still.
      if (_lb_args.debug() > 1) {
        std::vector<double> objSum(clients, 0.0);
        std::vector<int> objCnt(clients, 0);
        for (int i = 0; i < statsData->objData.size(); i++) {
          const int p = statsData->from_proc[i];
          if (p < 0 || p >= clients) continue;
          objSum[p] += statsData->objData[i].wallTime;
          objCnt[p]++;
        }
        for (int p = 0; p < clients; p++)
          CkPrintf("[%d] PE %d: %d obj, obj %.4f bg %.4f total %.4f%s\n", CkMyPe(), p,
                   objCnt[p], objSum[p], (double)statsData->procs[p].bg_walltime,
                   objSum[p] + statsData->procs[p].bg_walltime,
                   objSum[p] + statsData->procs[p].bg_walltime >= mLoad * 0.999 ? "  <-- max" : "");
      }
//      if (_lb_args.debug() > 1) {
//        for (int i=0; i<statsData->n_objs; i++)
//          CmiPrintf("[%d] %.10f %.10f\n", i, statsData->objData[i].minWall, statsData->objData[i].maxWall);
//      }
  }
  
  {
    cpu_set_t cpus;
    if (!concurrent && !use_thread && strategyOffThreadOk && !g_strategyInFlight.load() &&
        strategyOffThreadCpus(&cpus)) {
      g_strategyInFlight.store(true);
      StrategyJob* j = new StrategyJob(this, statsData);
      lbHelperSubmit(cpus, [j]() { j->lb->strategyCompute(j->stats); j->done.store(true, std::memory_order_release); });
      CcdCallOnCondition(CcdSCHEDLOOP, strategyPoll, (void*)j);
      return;   // strategyFinishOffThread -> ApplyDecision once the helper is done
    }
  }
  storedMigrateMsg = Strategy(statsData);

  if (!concurrent) ApplyDecision(); // immediately apply the migration decision
#endif
}

void CentralLB::strategyFinishOffThread(LDStats* stats)
{
#if CMK_LBDB_ON
  if (_lb_args.debug() && (CkMyPe() == cur_ld_balancer))
    CkPrintf("CharmLB> %s: strategy ran %.1f ms on the helper thread\n", lbname,
             (CkWallTimer() - strat_start_time) * 1e3);
  storedMigrateMsg = strategyFinish(stats);
  if (!concurrent) thisProxy[CkMyPe()].ApplyDecision();
#endif
}

void CentralLB::ApplyDecision() {
#if CMK_LBDB_ON
  const int clients = CkNumPes();

  LBMigrateMsg *migrateMsg;
  if (concurrent) {
    migrateMsg = createMigrateMsg(statsData);
    if (_lb_args.debug()) printStrategyStats(migrateMsg);
  } else {
    migrateMsg = storedMigrateMsg;
    storedMigrateMsg = NULL;
  }


#if CMK_REPLAYSYSTEM
  CpdHandleLBMessage(&migrateMsg);
#endif

#if CMK_GPU
  // Memory contract, I-batch: partition the decision so each batch fits what
  // every device can stage and land, and every PE's IPC slots. One batch (the
  // usual case) takes the unmodified path below; multiple batches are stashed
  // here and released sequentially by ReleaseNextBatch as each completes.
  {
    std::vector<int> batchOf;
    int refused = 0;
    const int nBatches = LBBatchPlanner::plan(statsData, batchOf, &refused);
    if (nBatches == 1 && refused > 0) {
      // The planner took moves back; the message must not carry them.
      delete migrateMsg;
      migrateMsg = createMigrateMsg(statsData);
    }
    if (nBatches > 1) {
      int nMoves = 0;
      for (int i = 0; i < (int)statsData->objData.size(); i++)
        if (statsData->to_proc[i] != statsData->from_proc[i]) nMoves++;
      CkPrintf("CharmLB> memory contract: executing this step's %d migrations "
               "in %d batches\n", nMoves, nBatches);
      std::vector<int> savedTo = statsData->to_proc;
      // Replication decisions stage nothing and the application executes
      // them once the step has resumed, so they ride the last batch only.
      std::vector<LBAction> savedClones, savedRetires;
      savedClones.swap(statsData->clones);
      savedRetires.swap(statsData->retires);
      lbBatchMsgs.clear();
      for (int k = 0; k < nBatches; k++) {
        for (int i = 0; i < (int)statsData->objData.size(); i++)
          if (savedTo[i] != statsData->from_proc[i] && batchOf[i] != k)
            statsData->to_proc[i] = statsData->from_proc[i];
          else
            statsData->to_proc[i] = savedTo[i];
        if (k == nBatches - 1) {
          statsData->clones = savedClones;
          statsData->retires = savedRetires;
        }
        LBMigrateMsg* bm = createMigrateMsg(statsData);
        bm->lb_batch = k;
        bm->lb_nbatches = nBatches;
        LBManagerObj()->get_avail_vector(bm->avail_vector);
        bm->next_lb = LBManagerObj()->new_lbbalancer();
        lbBatchMsgs.push_back(bm);
      }
      statsData->to_proc = savedTo;
      delete migrateMsg;
      migrateMsg = lbBatchMsgs[0];
      lbNextBatch = 1;
    }
  }
#endif
  
  LBManagerObj()->get_avail_vector(migrateMsg->avail_vector);
  migrateMsg->next_lb = LBManagerObj()->new_lbbalancer();

  // if this is the step at which we need to dump the database
  simulationWrite();

//  calculate predicted load
//  very time consuming though, so only happen when debugging is on
  if (_lb_args.printSummary()) {
      LBInfo info(clients);
        // not take comm data
      getPredictedLoadWithMsg(statsData, clients, migrateMsg, info, 0);
      LBRealType mLoad, mCpuLoad, totalLoad;
      info.getSummary(mLoad, mCpuLoad, totalLoad);
      int nmsgs, nbytes;
      statsData->computeNonlocalComm(nmsgs, nbytes);
      CkPrintf("[%d] Load Summary (after LB): max (with bg load): %f max (obj only): %f average: %f at step %d nonlocal: %d msgs %.2fKB useMem: %.2fKB.\n", CkMyPe(), mLoad, mCpuLoad, totalLoad/clients, step(), nmsgs, 1.0*nbytes/1024, (1.0*useMem())/1024);
      for (int i=0; i<clients; i++)
        migrateMsg->expectedLoad[i] = info.peLoads[i];
  }

  DEBUGF(("[%d]calling recv migration\n",CkMyPe()));

  sendDecision(migrateMsg);
  // Zero out data structures for next cycle
  // CkPrintf("zeroing out data\n");
  statsData->clear();
  stats_msg_count=0;
#endif
}

void CentralLB::t_LoadBalance()
{
    LoadBalance();
}

// Ship one decision message to every PE -- the send tail shared by the
// normal path and batched release.
void CentralLB::sendDecision(LBMigrateMsg* migrateMsg)
{
  // +LBPeerDecision (forced on by +LBAsync) ships each PE only the moves it is
  // party to plus its own arrival counts, instead of broadcasting the whole
  // decision to everyone. Under async load balancing the broadcast is not just
  // wasteful: the only thing a bystander PE does with it is update its location
  // cache, and with elements migrating while the application runs those caches
  // are being corrected continuously by the ordinary home-based routing anyway.
  if (_lb_args.lbPeerDecision())
    InitiateScatter(migrateMsg);
  else
    thisProxy.ReceiveMigration(migrateMsg);
}

// Reduction target on the decision PE: the previous batch has completed on
// every PE, so release the next one. The final batch flows into the normal
// MigrationDone path, which resumes clients.
void CentralLB::ReleaseNextBatch()
{
#if CMK_LBDB_ON
  if (lbNextBatch >= (int)lbBatchMsgs.size()) return;
  LBMigrateMsg* m = lbBatchMsgs[lbNextBatch++];
  if (lbNextBatch >= (int)lbBatchMsgs.size()) lbBatchMsgs.clear();
  sendDecision(m);
#endif
}

void CentralLB::InitiateScatter(LBMigrateMsg *msg) {

  if (CkNumPes() <= broadcastThreshold) {
    thisProxy.ReceiveMigration(msg);
    return;
  }

  int middlePe = CkNumPes() / 2;

  // allocate maximum possible size to avoid later copies
  // the messages will be resized before sending
  LBScatterMsg *leftMsg = new (middlePe, middlePe, msg->n_moves)
    LBScatterMsg(0, middlePe - 1);
  LBScatterMsg *rightMsg = new (CkNumPes() - middlePe, CkNumPes() - middlePe, msg->n_moves)
    LBScatterMsg(middlePe, CkNumPes() - 1);
  leftMsg->lb_batch = msg->lb_batch;
  leftMsg->lb_nbatches = msg->lb_nbatches;
  leftMsg->totalDecisions = rightMsg->totalDecisions = msg->n_moves;
  leftMsg->next_lb = msg->next_lb;
  rightMsg->lb_batch = msg->lb_batch;
  rightMsg->lb_nbatches = msg->lb_nbatches;
  rightMsg->next_lb = msg->next_lb;

  int *migrateTally = new int[CkNumPes()];
  int *futureTally = new int[CkNumPes()];
  memset(migrateTally, 0, CkNumPes() * sizeof(int));
  memset(futureTally, 0, CkNumPes() * sizeof(int));

  for (int i = 0; i < msg->n_moves; i++) {
    MigrateInfo* item = (MigrateInfo*) &msg->moves[i];
    // Replication decisions are executed by the application and never
    // arrive through immigrate, so no PE expects them.
    if (item->isMove()) {
      if (item->async_arrival) futureTally[item->to_pe]++;
      else migrateTally[item->to_pe]++;
    }
    if (item->from_pe < middlePe) {
      leftMsg->moves[leftMsg->numMigrates++] = *item;
    }
    else {
      rightMsg->moves[rightMsg->numMigrates++] = *item;
    }
  }

  memcpy(leftMsg->numMigratesPerPe, migrateTally, middlePe * sizeof(int));
  memcpy(rightMsg->numMigratesPerPe, &migrateTally[middlePe], (CkNumPes() - middlePe) * sizeof(int));
  memcpy(leftMsg->numFutureMigratesPerPe, futureTally, middlePe * sizeof(int));
  memcpy(rightMsg->numFutureMigratesPerPe, &futureTally[middlePe],
         (CkNumPes() - middlePe) * sizeof(int));

  delete [] migrateTally;
  delete [] futureTally;

  // shrink the size of the messages
  envelope *env = UsrToEnv(rightMsg);
  env->shrinkUsersize((msg->n_moves - rightMsg->numMigrates) * sizeof(MigrateDecision));

  // left message is not getting sent yet, but better resize it now
  // before we lose track of its original size
  env = UsrToEnv(leftMsg);
  env->shrinkUsersize((msg->n_moves - leftMsg->numMigrates) * sizeof(MigrateDecision));

  // send out results for right half of PEs first
  // to overlap communication with computation
  thisProxy[middlePe].ScatterMigrationResults(rightMsg);

  delete msg;
  ScatterMigrationResults(leftMsg);
}

void CentralLB::ScatterMigrationResults(LBScatterMsg *msg) {

  int finished = false;
  do {
    CkAssert(msg->firstPeInSpan == CkMyPe());
    int numPesInSpan = msg->lastPeInSpan - msg->firstPeInSpan + 1 ;

    if (numPesInSpan <= broadcastThreshold) {
      for (int i = msg->firstPeInSpan; i < msg->lastPeInSpan; i++) {
        // TODO: multicast without allocating new message each time
        LBScatterMsg *msgCopy = new (numPesInSpan, numPesInSpan, msg->numMigrates)
          LBScatterMsg(msg->firstPeInSpan, msg->lastPeInSpan);
        msgCopy->lb_batch = msg->lb_batch;
        msgCopy->lb_nbatches = msg->lb_nbatches;
        msgCopy->next_lb = msg->next_lb;
        msgCopy->totalDecisions = msg->totalDecisions;
        msgCopy->numMigrates = msg->numMigrates;
        memcpy(msgCopy->numMigratesPerPe, msg->numMigratesPerPe,
               numPesInSpan * sizeof(int));
        memcpy(msgCopy->numFutureMigratesPerPe, msg->numFutureMigratesPerPe,
               numPesInSpan * sizeof(int));
        memcpy(msgCopy->moves, msg->moves,
               msg->numMigrates * sizeof(MigrateDecision));
        thisProxy[i].ReceiveMigration(msgCopy);
      }
      // use original message for last send
      thisProxy[msg->lastPeInSpan].ReceiveMigration(msg);
      finished = true;
    }
    else {
      int middlePe = (msg->firstPeInSpan + msg->lastPeInSpan + 1) / 2;
      // reuse received message, taking care not to overwrite needed data
      LBScatterMsg *leftMsg = msg;
      int numMigrates = leftMsg->numMigrates;
      int numPesInRightSpan = leftMsg->lastPeInSpan - middlePe + 1;
      LBScatterMsg *rightMsg =
        new (numPesInRightSpan, numPesInRightSpan, leftMsg->numMigrates)
        LBScatterMsg(middlePe, leftMsg->lastPeInSpan);
      rightMsg->lb_batch = leftMsg->lb_batch;
      rightMsg->lb_nbatches = leftMsg->lb_nbatches;
      rightMsg->next_lb = leftMsg->next_lb;
      rightMsg->totalDecisions = leftMsg->totalDecisions;
      leftMsg->numMigrates = 0;
      leftMsg->lastPeInSpan = middlePe - 1;
      for (int i = 0; i < numMigrates; i++) {
        if (leftMsg->moves[i].fromPe < middlePe) {
          leftMsg->moves[leftMsg->numMigrates++] = leftMsg->moves[i];
        }
        else {
          rightMsg->moves[rightMsg->numMigrates++] = leftMsg->moves[i];
        }
      }

      memcpy(rightMsg->numMigratesPerPe,
             &leftMsg->numMigratesPerPe[middlePe - leftMsg->firstPeInSpan],
             (numPesInRightSpan) * sizeof(int));
      memcpy(rightMsg->numFutureMigratesPerPe,
             &leftMsg->numFutureMigratesPerPe[middlePe - leftMsg->firstPeInSpan],
             (numPesInRightSpan) * sizeof(int));

      // shrink the size of the messages
      envelope *env = UsrToEnv(rightMsg);
      env->shrinkUsersize((numMigrates - rightMsg->numMigrates)
                          * sizeof(MigrateDecision));

      // left message is not getting sent yet, but better resize it now
      // before we lose track of its original size
      env = UsrToEnv(leftMsg);
      env->shrinkUsersize((numMigrates - leftMsg->numMigrates)
                          * sizeof(MigrateDecision));

      thisProxy[middlePe].ScatterMigrationResults(rightMsg);
    }

  } while (!finished);

}

// test if sender and receiver in a commData is nonmigratable.
static bool isMigratable(LDObjData **objData, int *len, int count, const LDCommData &commData)
{
#if CMK_LBDB_ON
  for (int pe=0 ; pe<count; pe++)
  {
    for (int i=0; i<len[pe]; i++)
      if (objData[pe][i].objID() == commData.sender.objID() ||
          objData[pe][i].objID() == commData.receiver.get_destObj().objID())
      return false;
  }
#endif
  return true;
}

// rebuild LDStats and remove all non-migratble objects and related things
void CentralLB::removeNonMigratable(LDStats* stats, int count)
{
  int i;

  // check if we have non-migratable objects
  int have = 0;
  for (const auto& odata : stats->objData)
  {
    if (!odata.migratable) {
      have = 1; break;
    }
  }
  if (have == 0) return;

  std::vector<LDObjData> mig;
  std::vector<int> new_from_proc, new_to_proc;
  mig.reserve(stats->n_migrateobjs);
  new_from_proc.reserve(stats->n_migrateobjs);
  new_to_proc.reserve(stats->n_migrateobjs);
  for (i=0; i<stats->objData.size(); i++)
  {
    LDObjData &odata = stats->objData[i];
    if (odata.migratable) {
      mig.push_back(odata);
      new_from_proc.push_back(stats->from_proc[i]);
      new_to_proc.push_back(stats->to_proc[i]);
    }
    else {
      stats->procs[stats->from_proc[i]].bg_walltime += odata.wallTime;
#if CMK_LB_CPUTIMER
      stats->procs[stats->from_proc[i]].bg_cputime += odata.cpuTime;
#endif
    }
  }
  CmiAssert(stats->n_migrateobjs == mig.size());

  stats->makeCommHash();
  
  std::vector<LDCommData> newCommData;
  newCommData.reserve(stats->commData.size());
  for (auto& cdata : stats->commData)
  {
    if (!cdata.from_proc()) 
    {
      int idx = stats->getSendHash(cdata);
      CmiAssert(idx != -1);
      if (!stats->objData[idx].migratable) continue;
    }
    switch (cdata.receiver.get_type()) {
    case LD_PROC_MSG:
      break;
    case LD_OBJ_MSG:  {
      int idx = stats->getRecvHash(cdata);
      if (stats->complete_flag)
        CmiAssert(idx != -1);
      else if (idx == -1) continue;          // receiver not in this group
      if (!stats->objData[idx].migratable) continue;
      break;
      }
    case LD_OBJLIST_MSG:    // object message FIXME add multicast
      break;
    }
    newCommData.push_back(cdata);
  }

  if (mig.size() != stats->objData.size())
    CmiPrintf("Removed %zu nonmigratable objs (& %zu associated comms) from total n_objs:%zu (%d migratable objs left)\n",
              stats->objData.size() - stats->n_migrateobjs,
              stats->commData.size() - newCommData.size(), stats->objData.size(),
              stats->n_migrateobjs);

  // swap to new data
  stats->objData = mig;
  stats->from_proc = new_from_proc;
  stats->to_proc = new_to_proc;

  stats->commData = newCommData;

  stats->deleteCommHash();
  stats->makeCommHash();

}



void CentralLB::ReceiveMigration(LBScatterMsg *m) {
  if (concurrent) {
    if (CkMyPe() == 0) lbmgr->SetStrategyCost(CkWallTimer() - strat_start_time);
    // Zero out data structures for next cycle
    statsData->clear();
    stats_msg_count=0;
  }
  storedMigrateMsg = NULL;
  storedScatterMsg = m;
#if CMK_MEM_CHECKPOINT
  CkResetInLdb();
#endif
  contribute(CkCallback(CkReductionTarget(CentralLB, ProcessMigrationDecision),
              thisProxy));

}

void CentralLB::ReceiveMigration(LBMigrateMsg *m)
{
  if (concurrent) {
    if (CkMyPe() == 0) lbmgr->SetStrategyCost(CkWallTimer() - strat_start_time);
    // Zero out data structures for next cycle
    statsData->clear();
    stats_msg_count=0;
  }
  storedMigrateMsg = m;
#if CMK_MEM_CHECKPOINT
  CkResetInLdb();
#endif
  contribute(CkCallback(CkReductionTarget(CentralLB, ProcessReceiveMigration),
              thisProxy));
}

void CentralLB::ProcessMigrationDecision() {
#if CMK_LBDB_ON
  LBScatterMsg *m = storedScatterMsg;
  CkAssert(m != NULL);

  lbCurBatch = m->lb_batch;
  lbCurNBatches = m->lb_nbatches;

  // Each PE is told only its own two arrival counts, which is all
  // ProcessReceiveMigration ever derived from the full move list. Every
  // destination has these before any source moves anything: this runs from the
  // pre-migration reduction in ReceiveMigration, so every PE has arrived here
  // before any of them issues a migrate.
  const int myIdx = CkMyPe() - m->firstPeInSpan;
  migrates_expected = m->numMigratesPerPe[myIdx];
  future_migrates_expected = m->numFutureMigratesPerPe[myIdx];
  cur_ld_balancer = m->next_lb;
  lbmgr->SetLastRoundDecisions(m->totalDecisions);

  for(int i = 0; i < m->numMigrates; i++) {
    MigrateDecision& move = m->moves[i];
    const int me = CkMyPe();
    if (!move.isMove()) {
      // A replication decision: handed to the object at once; the
      // application chooses when to act on it (CkMigratable::ckClone).
      if (move.fromPe == me) {
        LDObjHandle h;
        if (lbmgr->LiveObjHandle(move.dbIndex, move.id, h))
          executeReplicaDecision(h, move.kind, move.toPe, move.rank);
        else if (_lb_args.debug() > 0)
          CkPrintf("[%d] CharmLB> replication decision for object %llu dropped: the object is gone\n",
                   CkMyPe(), (unsigned long long)move.id);
      }
      continue;
    }
    if (move.fromPe == me) {
      if (move.toPe == me) {
        CkAbort("[%d] Error, attempting to migrate from myself to myself\n",
            CkMyPe());
      }
      DEBUGF(("[%d] migrating object to %d\n", move.fromPe, move.toPe));
      // migrate object, in case it is already gone, inform toPe
      LDObjHandle objInfo = lbmgr->GetObjHandle(move.dbIndex);

      // The object died between the stats this decision was planned from and
      // now. Tell the destination to stop expecting it, exactly as the
      // broadcast path does -- it is counting on the arrival.
      if (lbmgr->Migrate(objInfo,move.toPe) == 0)
        thisProxy[move.toPe].MissMigrate(!move.asyncArrival);
    }
  }

  if (migrates_expected == 0 || migrates_completed == migrates_expected) {
    MigrationDone(1);
  }

  delete m;
#endif
}

// A Clone / Retire record addressed to this PE (CentralLB.h). The database
// checks that the handle still names the object the strategy planned for;
// the object itself decides when to act (CkMigratable::ckClone / ckRetire),
// so nothing here waits and nothing is counted as an expected arrival.
void CentralLB::executeReplicaDecision(const LDObjHandle& h, unsigned char kind, int toPe, int rank)
{
#if CMK_LBDB_ON
  int delivered = 0;
  static const char* const names[] = {"move", "clone", "retire", "clone notice", "retire notice"};
  const char* name = kind <= LB_DECISION_RETIRE_NOTICE ? names[kind] : "decision";
  if (kind == LB_DECISION_CLONE) delivered = lbmgr->Clone(h, toPe, rank);
  else if (kind == LB_DECISION_RETIRE) delivered = lbmgr->Retire(h);
  else if (kind == LB_DECISION_CLONE_NOTICE) delivered = lbmgr->CloneNotice(h, toPe, rank);
  else if (kind == LB_DECISION_RETIRE_NOTICE) delivered = lbmgr->RetireNotice(h, rank);
  if (_lb_args.debug() > 0) {
    if (!delivered)
      CkPrintf("[%d] CharmLB> %s of object %llu dropped: the object is gone\n", CkMyPe(), name,
               (unsigned long long)h.id);
    else
      CkPrintf("[%d] CharmLB> %s for object %llu (PE %d, replica %d)\n", CkMyPe(), name,
               (unsigned long long)h.id, toPe, rank);
  }
#endif
}

void CentralLB::ProcessReceiveMigration()
{
  // CmiPrintf("[%d] ProcessReceiveMigration\n", CkMyPe());
#if CMK_LBDB_ON
	int i;
        LBMigrateMsg *m = storedMigrateMsg;
        CmiAssert(m!=NULL);

  lbCurBatch = m->lb_batch;
  lbCurNBatches = m->lb_nbatches;

  if (_lb_args.debug() > 1)
    if (CkMyPe()%1024==0) CmiPrintf("[%d] Starting ReceiveMigration step %d at %f\n",CkMyPe(),step(), CmiWallTimer());

  for (i=0; i<CkNumPes(); i++) lbmgr->lastLBInfo.expectedLoad[i] = m->expectedLoad[i];
  CmiAssert(migrates_expected <= 0 || migrates_completed == migrates_expected);
  migrates_expected = 0;
  future_migrates_expected = 0;
  lbmgr->SetLastRoundDecisions(m->n_moves);
  // CmiPrintf("[%d] ProcessReceiveMigration: n_moves=%d\n", CkMyPe(), m->n_moves);
  for(i=0; i < m->n_moves; i++) {
    MigrateInfo& move = m->moves[i];
    const int me = CkMyPe();
    if (!move.isMove()) {
      // A replication decision: handed to the object at once; the
      // application chooses when to act on it (CkMigratable::ckClone).
      if (move.from_pe == me) executeReplicaDecision(move.obj, move.kind, move.to_pe, move.rank);
      continue;
    }
    if (move.from_pe == me && move.to_pe != me) {
#if CMK_DRONE_MODE
      int to_pe_rank0 = CMK_RANK_0(move.to_pe);
      if(move.from_pe == to_pe_rank0) continue;
      move.to_pe = to_pe_rank0;
#endif

      DEBUGF(("[%d] migrating object to %d\n",move.from_pe,move.to_pe));
      // migrate object, in case it is already gone, inform toPe
      if (lbmgr->Migrate(move.obj,move.to_pe) == 0)
         thisProxy[move.to_pe].MissMigrate(!move.async_arrival);
    } else if (move.from_pe != me && move.to_pe == me) {
#if CMK_DRONE_MODE
      int to_pe_rank0 = CMK_RANK_0(move.to_pe);
      if(me != to_pe_rank0) continue;
#endif
       DEBUGF(("[%d] expecting object from %d\n",move.to_pe,move.from_pe));
      if (!move.async_arrival) migrates_expected++;
      else future_migrates_expected++;
    }
    else {
      #if CMK_GLOBAL_LOCATION_UPDATE
      // CmiPrintf("[%d] Updating location for obj id=%llu from %d to %d\n", CkMyPe(), move.obj.id, move.from_pe, move.to_pe);
        UpdateLocation(move);
      #endif
    }
  }

  DEBUGF(("[%d] in ReceiveMigration %d moves expected: %d future expected: %d\n",CkMyPe(),m->n_moves, migrates_expected, future_migrates_expected));
  // if (_lb_debug) CkPrintf("[%d] expecting %d objects migrating.\n", CkMyPe(), migrates_expected);



#if 0
  if (m->n_moves ==0) {
    lbmgr->SetLBPeriod(lbmgr->GetLBPeriod()*2);
  }
#endif
  cur_ld_balancer = m->next_lb;
  if((CkMyPe() == cur_ld_balancer) && (cur_ld_balancer != 0)){
      LBManagerObj()->set_avail_vector(m->avail_vector, -2);
  }

  if (migrates_expected == 0 || migrates_completed == migrates_expected)
    MigrationDone(1);
  delete m;
#endif
}

void CentralLB::CheckForLB() {
  //sleep(5);
#if CMK_SHRINK_EXPAND
  if (pending_realloc_state == EXPAND_MSG_RECEIVED)
    CheckForRealloc();
  //else if (pending_realloc_state == NO_REALLOC)
  //  thisProxy.ResumeClients(0);
  else
    thisProxy.CallLB();
#else
  // if we are not in shrink/expand mode, just call LB
  thisProxy.CallLB();
#endif
  //else
  //  thisProxy.ResumeClients(0);
}

// We assume that bit vector would have been aptly set async by either scheduler or charmrun.
void CentralLB::CheckForRealloc(){
#if CMK_SHRINK_EXPAND
  if(pending_realloc_state != NO_REALLOC) {
    pending_realloc_state = (pending_realloc_state == SHRINK_MSG_RECEIVED) ? SHRINK_IN_PROGRESS : EXPAND_IN_PROGRESS; //in progress
    CkPrintf("Load balancer invoking charmrun to handle reallocation on pe %d\n", CkMyPe());
    double end_lb_time = CkWallTimer();
    CkPrintf("CharmLB> %s: PE [%d] step %d finished at %f duration %f s\n\n",
        lbname, cur_ld_balancer, step()-1, end_lb_time,	end_lb_time-start_lb_time);
    // do checkpoint
    CkCallback cb(CkIndex_CentralLB::ResumeFromReallocCheckpoint(), thisProxy[0]);
    CkStartRescaleCheckpoint(_shrinkexpand_basedir, cb, 
      std::vector<char>(se_avail_vector, se_avail_vector + CkNumPes()));
  } else {
    thisProxy.MigrationDoneImpl(1);
  }
#endif
}

void CentralLB::ResumeFromReallocCheckpoint(){
#if CMK_SHRINK_EXPAND
    CkPrintf("Resumed from realloc\n");
    std::vector<char> avail(se_avail_vector, se_avail_vector + CkNumPes());
    //free(se_avail_vector);
    thisProxy.WillIbekilled(avail, numProcessAfterRestart);
#endif
}

void CentralLB::WillIbekilled(std::vector<char> avail, int newnumProcessAfterRestart){
#if CMK_SHRINK_EXPAND
 numProcessAfterRestart = newnumProcessAfterRestart;
 mynewpe =  GetNewPeNumber(avail);
 //CkPrintf("[%d] -> new pe %d\n", CkMyPe(), mynewpe);
 willContinue = avail[CkMyPe()];
 //CkPrintf("PE%i> Sending start cleanup reduction\n", CkMyPe());
 CkCallback cb(CkIndex_CentralLB::StartCleanup(), thisProxy[0]);
 contribute(cb);
#endif
}

void CentralLB::StartCleanup(){
#if CMK_SHRINK_EXPAND
  //CkAbort("FLAG\n");
  //CkPrintf("Starting cleanup\n");
	CkCleanup();
#endif
}

void CentralLB::MigrationDone(int balancing)
{
#if CMK_SHRINK_EXPAND
   // barrier to check for reallocation
    CkCallback cb(CkIndex_CentralLB::CheckForRealloc(), thisProxy[0]);
    contribute(cb);
	return;
#else
    MigrationDoneImpl(balancing);
#endif
}

void CentralLB::MigrationDoneImpl (int balancing)
{

#if CMK_LBDB_ON
  if (lbMigDbg()) {
    CkPrintf("[LBMIG %d] MigrationDone balancing=%d\n", CkMyPe(), balancing);
    fflush(stdout);
  }

  // Batch boundary, not the end of the step: reset the arrival accounting
  // and signal the decision PE to release the next batch. Clients stay at
  // the sync barrier; loads, step counters, and callbacks are untouched
  // until the final batch flows through below.
  if (lbCurBatch < lbCurNBatches - 1) {
    if (lbMigDbg())
      CkPrintf("[LBMIG %d] batch %d/%d complete\n", CkMyPe(), lbCurBatch + 1,
               lbCurNBatches);
    migrates_completed = 0;
    migrates_expected = -1;
    contribute(CkCallback(CkReductionTarget(CentralLB, ReleaseNextBatch),
                          thisProxy[cur_ld_balancer]));
    return;
  }
  lbCurBatch = 0;
  lbCurNBatches = 1;

  migrates_completed = 0;
  migrates_expected = -1;
  // clear load stats
  if (balancing) lbmgr->ClearLoads();
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
  if (CkMyPe() == hapiDeviceGroupFirstPe())
    hapiClearCuptiDataDevice(hapiMyDeviceIndex());
#endif
  // Increment to next step
  lbmgr->incStep();
	DEBUGF(("[%d] Incrementing Step %d \n",CkMyPe(),step()));
  // if sync resume, invoke a barrier


  LBManager::Object()->MigrationDone();    // call registered callbacks

  LoadbalanceDone(balancing);        // callback
  // if sync resume invoke a barrier
  if (balancing && _lb_args.syncResume()) {
    contribute(CkCallback(CkReductionTarget(CentralLB, ResumeClients),
                thisProxy));
  }
  else{
    {
	thisProxy [CkMyPe()].ResumeClients(balancing);
    }	
  }	
#if CMK_GRID_QUEUE_AVAILABLE
  CmiGridQueueDeregisterAll ();
  CpvAccess(CkGridObject) = NULL;
#endif  // if CMK_GRID_QUEUE_AVAILABLE
#endif  // if CMK_LBDB_ON
}

void CentralLB::ResumeClients()
{
  ResumeClients(1);
}

void CentralLB::ResumeClients(int balancing)
{
#if CMK_LBDB_ON
  if (lbMigDbg()) {
    // The residency table must describe exactly the elements this process holds
    // once a step has settled. A count that drifts step over step means an
    // arrival or departure path is not maintaining it.
    CkPrintf("[LBMIG %d] ResumeClients balancing=%d resident=%zu\n", CkMyPe(),
             balancing, CkLocMgr::numResidentInProcess());
    fflush(stdout);
  }
  //CkPrintf("[%d] Resuming clients. balancing:%d.\n",CkMyPe(),balancing);

  lbmgr->ResumeClients();
  if (balancing)  {

    CheckMigrationComplete();
    if (future_migrates_expected == 0 || 
            future_migrates_expected == future_migrates_completed) {
      CheckMigrationComplete();
    }
  }
  lbmgr->lb_in_progress = false;

  if (CkMyPe() == 0)
    lbmgr->callRealloc();
#endif
}

/*
  migration of objects contains two different kinds:
  (1) objects want to make a barrier for migration completion
      (waitForBarrier is true)
      migrationDone() to finish and resumeClients
  (2) objects don't need a barrier
  However, next load balancing can only happen when both migrations complete
*/ 
void CentralLB::CheckMigrationComplete()
{
#if CMK_LBDB_ON
  lbdone ++;
  if (lbdone == 2) {
    double end_lb_time = CkWallTimer();
    if (_lb_args.debug() && CkMyPe()==0) {
      CkPrintf("CharmLB> %s: PE [%d] step %d finished at %f duration %f s\n\n",
                lbname, CkMyPe(), step()-1, end_lb_time,
		end_lb_time-start_lb_time);
    }

    lbmgr->SetMigrationCost(end_lb_time - start_lb_time);

    lbdone = 0;
    future_migrates_expected = -1;
    future_migrates_completed = 0;


    DEBUGF(("[%d] Migration Complete\n", CkMyPe()));
    // release local barrier  so that the next load balancer can go
    LDOMHandle h;
    h.id.id.idx = 0;
    lbmgr->DoneRegisteringObjects(h);
    // switch to the next load balancer in the list
    // subtle: called from Migrated() may result in Migrated() called in next LB
    if (!(_lb_args.metaLbOn() && _lb_args.metaLbModelDir() != nullptr))
      lbmgr->nextLoadbalancer(seqno);
  }
#endif
}

// Remove edges from commData in LDStats which contains deleted elements
void CentralLB::removeCommDataOfDeletedObjs(LDStats* stats) {
  stats->makeCommHash();

  int n_comm = 0;
  for (auto& cdata : stats->commData) {
    switch (cdata.receiver.get_type()) {
      case LD_PROC_MSG:
        break;
      case LD_OBJ_MSG:  {
        if (!cdata.from_proc()) {
          int sidx = stats->getSendHash(cdata);
          int ridx = stats->getRecvHash(cdata);
          if (sidx == -1 || ridx == -1) continue;
        }
        break;
      }
      case LD_OBJLIST_MSG:  {
        int sidx = stats->getSendHash(cdata);
        if (sidx == -1) continue;
        int nobjs;
        LDObjKey *objs = cdata.receiver.get_destObjs(nobjs);
        for (int id=0; id<nobjs; id++) {
          int idx = stats->getHash(objs[id]);
          if (idx == -1)
          {
            objs[id] = objs[nobjs-1];
            id--;
            nobjs--;
          }
        }
        if(nobjs == 0) continue;
        cdata.receiver.dest.destObjs.len = nobjs;
        break;
      }
    }

    stats->commData[n_comm] = cdata;
    n_comm++;
  }

  stats->commData.resize(n_comm);
}

void CentralLB::preprocess(LDStats* stats)
{
  if (_lb_args.ignoreBgLoad())
    stats->clearBgLoad();

  // Call the predictor for the future
  if (_lb_predict) FuturePredictor(statsData);
}

void CentralLB::printStrategyStats(LBMigrateMsg *msg) {
#if CMK_LBDB_ON
  envelope *env = UsrToEnv(msg);

  double strat_end_time = CkWallTimer();
  double lbdbMemsize = LBManager::Object()->useMem()/1000;
  CkPrintf("CharmLB> %s: PE [%d] Memory: LBManager: %d KB CentralLB: %d KB\n",
        lbname, CkMyPe(), (int)lbdbMemsize, (int)(useMem()/1000));
  CkPrintf("CharmLB> %s: PE [%d] #Objects migrating: %d, LBMigrateMsg size: %.2f MB\n", lbname, CkMyPe(), msg->n_moves, env->getTotalsize()/1024.0/1024.0);
  CkPrintf("CharmLB> %s: PE [%d] strategy finished at %f duration %f s\n",
      lbname, CkMyPe(), strat_end_time, strat_end_time-strat_start_time);
#endif
}

// default load balancing strategy
LBMigrateMsg* CentralLB::Strategy(LDStats* stats)
{
  strategyCompute(stats);
  return strategyFinish(stats);
}

void CentralLB::strategyCompute(LDStats* stats)
{
#if CMK_LBDB_ON
  strat_start_time = CkWallTimer();
  if (_lb_args.debug() && (CkMyPe() == cur_ld_balancer))
    CkPrintf("CharmLB> %s: PE [%d] strategy starting at %f\n", lbname, cur_ld_balancer, strat_start_time);

  work(stats);
#endif
}

LBMigrateMsg* CentralLB::strategyFinish(LDStats* stats)
{
#if CMK_LBDB_ON
#if CMK_GPU
  // Memory contract: every strategy's output passes through the verifier,
  // memory-aware or not. Moves that would violate a device's headroom are
  // refused (the object stays home -- always feasible); strategies that plan
  // with the ledger produce plans that pass untouched.
  {
    std::vector<ContractVerifier::Move> cvMoves;
    for (int i = 0; i < (int)stats->objData.size(); i++) {
      if (!stats->objData[i].migratable) continue;
      if (stats->to_proc[i] == stats->from_proc[i]) continue;
      ContractVerifier::Move m;
      m.obj = i;
      m.fromPe = stats->from_proc[i];
      m.toPe = &stats->to_proc[i];
      cvMoves.push_back(m);
    }
    ContractVerifier::verifyAndRepair(stats, cvMoves, 0.95, nullptr, &stats->clones);
    // The notices of a clone the verifier refused go with it (a notice names
    // the clone's rank; the group is the object's).
    {
      std::vector<LBAction> kept;
      for (const LBAction& n : stats->cloneNotices) {
        const int g = stats->objData[n.obj].replicaGroup;
        bool live = false;
        for (const LBAction& c : stats->clones)
          if (stats->objData[c.obj].replicaGroup == g && c.rank == n.rank) live = true;
        if (live) kept.push_back(n);
      }
      stats->cloneNotices.swap(kept);
    }
  }
#endif

  if ((_lb_args.debug()>2) && (CkMyPe() == cur_ld_balancer))  {
    CkPrintf("CharmLB> Obj Map:\n");
    for (const auto& val : stats->to_proc) CkPrintf("%d ", val);
    CkPrintf("\n");
  }

  if (concurrent) return NULL;  // migrate msg will only be created on PE with best solution

  LBMigrateMsg *msg = createMigrateMsg(stats);

	/* Extra feature for MetaBalancer
  if (_lb_args.metaLbOn()) {
    int clients = CkNumPes();
    LBInfo info(clients);
    getPredictedLoadWithMsg(stats, clients, msg, info, 0);
    LBRealType mLoad, mCpuLoad, totalLoad, totalLoadWComm;
    info.getSummary(mLoad, mCpuLoad, totalLoad);
    lbmgr->UpdateDataAfterLB(mLoad, mCpuLoad, totalLoad/clients);
  }
	*/

  double strat_end_time = CkWallTimer();
  lbmgr->SetStrategyCost(strat_end_time - strat_start_time);

  if (_lb_args.debug() && (CkMyPe() == cur_ld_balancer)) {
    printStrategyStats(msg);
  }
  return msg;
#else
  return NULL;
#endif
}
/*
void CentralLB::changeFreq(int r)
{
	CkAbort("ERROR: changeFreq in CentralLB should never be called!\n");
}
*/
void CentralLB::changeFreq(int nFreq)
{
#ifdef TEMP_LDB
        //CkPrintf("PROC#%d in changeFreq numProcs=%d\n",CkMyPe(),nFreq);
//  for(int i=0;i<numProcs;i++)
  {
//        if(procFreq[i]!=procFreqNew[i])
        {
              char newfreq[10];
              snprintf(newfreq,sizeof(newfreq),"%d",nFreq);
              cpufreq_sysfs_write(newfreq,CkMyPe()%physicalCoresPerNode);//i%physicalCoresPerNode);
//            CkPrintf("PROC#%d freq changing from %d to %d temp=%f\n",i,procFreq[i],procFreqNew[i],procTemp[i]);
        }
  }
#else
	CmiAbort("You should never call CentralLB::changeFreq without using the flag TEMP_LDB\n");
#endif

}

void CentralLB::work(LDStats* stats)
{
  // does nothing but print the database
  stats->print();
}

// generate migrate message from stats->from_proc and to_proc, followed by
// the strategy's replication decisions (stats->clones / retires)
LBMigrateMsg * CentralLB::createMigrateMsg(LDStats* stats)
{
  int i;
  std::vector<MigrateInfo*> migrateInfo;
  for (i=0; i<stats->objData.size(); i++) {
    LDObjData &objData = stats->objData[i];
    int frompe = stats->from_proc[i];
    int tope = stats->to_proc[i];
    if (frompe != tope) {
      //      CkPrintf("[%d] Obj %d migrating from %d to %d\n",
      //         CkMyPe(),obj,pe,dest);
      MigrateInfo *migrateMe = new MigrateInfo;
      migrateMe->obj = objData.handle;
      migrateMe->from_pe = frompe;
      migrateMe->to_pe = tope;
      migrateMe->async_arrival = objData.asyncArrival;
      migrateInfo.push_back(migrateMe);
    }
  }
  for (const LBAction& a : stats->clones) {
    MigrateInfo *rec = new MigrateInfo;
    rec->obj = stats->objData[a.obj].handle;
    rec->from_pe = a.fromPe;
    rec->to_pe = a.toPe;
    rec->kind = LB_DECISION_CLONE;
    rec->rank = a.rank;
    migrateInfo.push_back(rec);
  }
  for (const LBAction& a : stats->retires) {
    MigrateInfo *rec = new MigrateInfo;
    rec->obj = stats->objData[a.obj].handle;
    rec->from_pe = a.fromPe;
    rec->to_pe = a.fromPe;
    rec->kind = LB_DECISION_RETIRE;
    migrateInfo.push_back(rec);
  }
  for (const LBAction& a : stats->cloneNotices) {
    MigrateInfo *rec = new MigrateInfo;
    rec->obj = stats->objData[a.obj].handle;
    rec->from_pe = a.fromPe;
    rec->to_pe = a.toPe;
    rec->kind = LB_DECISION_CLONE_NOTICE;
    rec->rank = a.rank;
    migrateInfo.push_back(rec);
  }
  for (const LBAction& a : stats->retireNotices) {
    MigrateInfo *rec = new MigrateInfo;
    rec->obj = stats->objData[a.obj].handle;
    rec->from_pe = a.fromPe;
    rec->to_pe = a.fromPe;
    rec->kind = LB_DECISION_RETIRE_NOTICE;
    rec->rank = a.rank;
    migrateInfo.push_back(rec);
  }

  int migrate_count=migrateInfo.size();
  LBMigrateMsg* msg = new(migrate_count,CkNumPes(),CkNumPes(),0) LBMigrateMsg;
  msg->n_moves = migrate_count;
  for(i=0; i < migrate_count; i++) {
    MigrateInfo* item = (MigrateInfo*) migrateInfo[i];
    msg->moves[i] = *item;
    delete item;
    migrateInfo[i] = 0;
  }
  return msg;
}

LBMigrateMsg * CentralLB::extractMigrateMsg(LBMigrateMsg *m, int p)
{
  int nmoves = 0;
  int nunavail = 0;
  int i;
  for (i=0; i<m->n_moves; i++) {
    MigrateInfo* item = (MigrateInfo*) &m->moves[i];
    if (item->from_pe == p || item->to_pe == p) nmoves++;
  }
  for (i=0; i<CkNumPes();i++) {
    if (!m->avail_vector[i]) nunavail++;
  }
  LBMigrateMsg* msg;
  if (nunavail) msg = new(nmoves,CkNumPes(),CkNumPes(),0) LBMigrateMsg;
  else msg = new(nmoves,0,0,0) LBMigrateMsg;
  msg->n_moves = nmoves;
  msg->level = m->level;
  msg->next_lb = m->next_lb;
  for (i=0,nmoves=0; i<m->n_moves; i++) {
    MigrateInfo* item = (MigrateInfo*) &m->moves[i];
    if (item->from_pe == p || item->to_pe == p) {
      msg->moves[nmoves] = *item;
      nmoves++;
    }
  }
  // copy processor data
  if (nunavail)
  for (i=0; i<CkNumPes();i++) {
    msg->avail_vector[i] = m->avail_vector[i];
    msg->expectedLoad[i] = m->expectedLoad[i];
  }
  return msg;
}

void CentralLB::simulationWrite() {
  if(step() == LBSimulation::dumpStep)
  {
    // here we are supposed to dump the database
    int dumpFileSize = strlen(LBSimulation::dumpFile) + 4;
    char *dumpFileName = (char *)malloc(dumpFileSize);
    while (snprintf(dumpFileName, dumpFileSize, "%s.%d", LBSimulation::dumpFile, LBSimulation::dumpStep) >= dumpFileSize) {
      free(dumpFileName);
      dumpFileSize+=3;
      dumpFileName = (char *)malloc(dumpFileSize);
    }
    writeStatsMsgs(dumpFileName);
    free(dumpFileName);
    CmiPrintf("LBDump: Dumped the load balancing data at step %d.\n",LBSimulation::dumpStep);
    ++LBSimulation::dumpStep;
    --LBSimulation::dumpStepSize;
    if (LBSimulation::dumpStepSize <= 0) { // prevent stupid step sizes
      CmiPrintf("Charm++> Exiting...\n");
      CkExit();
    }
    return;
  }
}

void CentralLB::simulationRead() {
  if (concurrent) CkAbort("Error: LB simulation not supported in concurrent mode");
  LBSimulation *simResults = NULL, *realResults;
  LBMigrateMsg *voidMessage = new (0,0,0,0) LBMigrateMsg();
  voidMessage->n_moves=0;
  for ( ;LBSimulation::simStepSize > 0; --LBSimulation::simStepSize, ++LBSimulation::simStep) {
    // here we are supposed to read the data from the dump database
    int simFileSize = strlen(LBSimulation::dumpFile) + 4;
    char *simFileName = (char *)malloc(simFileSize);
    while (snprintf(simFileName, simFileSize, "%s.%d", LBSimulation::dumpFile, LBSimulation::simStep) >= simFileSize) {
      free(simFileName);
      simFileSize+=3;
      simFileName = (char *)malloc(simFileSize);
    }
    readStatsMsgs(simFileName);

    // allocate simResults (only the first step)
    if (simResults == NULL) {
      simResults = new LBSimulation(LBSimulation::simProcs);
      realResults = new LBSimulation(LBSimulation::simProcs);
    }
    else {
      // should be the same number of procs of the original simulation!
      if (!LBSimulation::procsChanged) {
	// it means we have a previous step, so in simResults there is data.
	// we can now print the real effects of the load balancer during the simulation
	// or print the difference between the predicted data and the real one.
	realResults->reset();
	// reset to_proc of statsData to be equal to from_proc
        statsData->to_proc = statsData->from_proc;
	findSimResults(statsData, LBSimulation::simProcs, voidMessage, realResults);
	simResults->PrintDifferences(realResults,statsData);
      }
      simResults->reset();
    }

    // now pass it to the strategy routine
    double startT = CkWallTimer();
    preprocess(statsData);
    CmiPrintf("%s> Strategy starts ... \n", lbname);
    LBMigrateMsg* migrateMsg = Strategy(statsData);
    CmiPrintf("%s> Strategy took %fs memory usage: CentralLB: %d KB.\n",
               lbname, CkWallTimer()-startT, (int)(useMem()/1000));

    // now calculate the results of the load balancing simulation
    findSimResults(statsData, LBSimulation::simProcs, migrateMsg, simResults);

    // now we have the simulation data, so print it and loop
    CmiPrintf("Charm++> LBSim: Simulation of load balancing step %d done.\n",LBSimulation::simStep);
    // **CWL** Officially recording my disdain here for using ints for bool
    if (LBSimulation::showDecisionsOnly) {
      simResults->PrintDecisions(migrateMsg, simFileName, 
				 LBSimulation::simProcs);
    } else {
      simResults->PrintSimulationResults();
    }

    free(simFileName);
    delete migrateMsg;
    CmiPrintf("Charm++> LBSim: Passing to the next step\n");
  }
  // deallocate simResults
  delete simResults;
  CmiPrintf("Charm++> Exiting...\n");
  CkExit();
}

void CentralLB::readStatsMsgs(const char* filename) 
{
#if CMK_LBDB_ON
  int i;
  FILE *f = fopen(filename, "r");
  if (f==NULL) {
    CkAbort("Fatal Error> Cannot open LB Dump file %s!\n", filename);
  }

  // at this stage, we need to rebuild the statsMsgList and
  // statsDataList structures. For that first deallocate the
  // old structures
  if (statsMsgsList) {
    for(i = 0; i < stats_msg_count; i++)
      delete statsMsgsList[i];
    delete[] statsMsgsList;
    statsMsgsList=0;
  }

  PUP::fromDisk pd(f);
  PUP::machineInfo machInfo;

  pd((char *)&machInfo, sizeof(machInfo));	// read machine info
  PUP::xlater p(machInfo, pd);

  if (_lb_args.lbversion() > 1) {
    p|_lb_args.lbversion();		// write version number
    CkPrintf("LB> File version detected: %d\n", _lb_args.lbversion());
    CmiAssert(_lb_args.lbversion() <= LB_FORMAT_VERSION);
  }
  p|stats_msg_count;

  CmiPrintf("readStatsMsgs for %d pes starts ... \n", stats_msg_count);
  if (LBSimulation::simProcs == 0) LBSimulation::simProcs = stats_msg_count;
  if (LBSimulation::simProcs != stats_msg_count) LBSimulation::procsChanged = true;

  // LBSimulation::simProcs must be set
  statsData->pup(p);

  CmiPrintf("Simulation for %d pes \n", LBSimulation::simProcs);
  CmiPrintf("n_obj: %zu n_migratable: %d \n", statsData->objData.size(), statsData->n_migrateobjs);

  // file f is closed in the destructor of PUP::fromDisk
  CmiPrintf("ReadStatsMsg from %s completed\n", filename);
#endif
}

void CentralLB::writeStatsMsgs(const char* filename) 
{
#if CMK_LBDB_ON
  FILE *f = fopen(filename, "w");
  if (f==NULL) {
    CkAbort("Fatal Error> writeStatsMsgs failed to open the output file %s!\n", filename);
  }

  const PUP::machineInfo &machInfo = PUP::machineInfo::current();
  PUP::toDisk p(f);
  p((char *)&machInfo, sizeof(machInfo));	// machine info

  p|_lb_args.lbversion();		// write version number
  p|stats_msg_count;
  statsData->pup(p);

  fclose(f);

  CmiPrintf("WriteStatsMsgs to %s succeed!\n", filename);
#endif
}

// calculate the predicted wallclock/cpu load for every processors
// considering communication overhead if considerComm is true
void getPredictedLoadWithMsg(BaseLB::LDStats* stats, int count, 
                      LBMigrateMsg *msg, LBInfo &info, 
		      int considerComm)
{
#if CMK_LBDB_ON
	stats->makeCommHash();

 	// update to_proc according to migration msgs
	for(int i = 0; i < msg->n_moves; i++) {
	  MigrateInfo &mInfo = msg->moves[i];
	  if (!mInfo.isMove()) continue;   // a clone or retire changes no assignment
	  int idx = stats->getHash(mInfo.obj.objID(), mInfo.obj.omID());
	  CmiAssert(idx != -1);
          stats->to_proc[idx] = mInfo.to_pe;
	}

	info.getInfo(stats, count, considerComm);
#endif
}


void CentralLB::findSimResults(LDStats* stats, int count, LBMigrateMsg* msg, LBSimulation* simResults)
{
    CkAssert(simResults != NULL && count == simResults->numPes);
    // estimate the new loads of the processors. As a first approximation, this is the
    // sum of the cpu times of the objects on that processor
    double startT = CkWallTimer();
    getPredictedLoadWithMsg(stats, count, msg, simResults->lbinfo, 1);
    CmiPrintf("getPredictedLoad finished in %fs\n", CkWallTimer()-startT);
}

void CentralLB::pup(PUP::er &p) { 
  if (p.isUnpacking())  {
    initLB(CkLBOptions(seqno)); 
  }
  p|reduction_started;
  int has_statsMsg=0;
  if (p.isPacking()) has_statsMsg = (statsMsg!=NULL);
  p|has_statsMsg;
  if (has_statsMsg) {
    if (p.isUnpacking())
      statsMsg = new CLBStatsMsg;
    statsMsg->pup(p);
  }
  p | use_thread;
#if CMK_GPU
  // Learning must survive checkpoint/restart and migration of the central PE,
  // or every restart throws away the calibration and falls back to the prior.
  gpuScalingModel_.pup(p);
  p | gpuScalingEpoch_;
#endif
}

int CentralLB::useMem() { 
  return sizeof(CentralLB) + statsData->useMem() + 
         CkNumPes() * sizeof(CLBStatsMsg *);
}


/**
  CLBStatsMsg is not a real message now.
  CLBStatsMsg is used for all processors to fill in their local load and comm
  statistics and send to processor 0
*/

CLBStatsMsg::CLBStatsMsg(int osz, int csz) {
  objData.resize(osz);
  commData.resize(csz);
}

CLBStatsMsg::~CLBStatsMsg() {
}

void CLBStatsMsg::pup(PUP::er &p) {
  p|from_pe;
  p|pe_speed;
#if CMK_GPU
  p|gpu_device_id;
  p|gpu_total_sms;
  p|gpu_mem_remaining;
  p|pool_buff_mem_remaining;
  p|gpu_pool_arena_bytes;
  p|gpu_pool_capacity_bytes;
  p|gpu_ipc_slots;
  p|gpu_descriptor;
#endif
  p|total_walltime;
  p|idletime;
#if defined(TEMP_LDB)
	p|pe_temp;
#endif

  p|bg_walltime;
#if CMK_LB_CPUTIMER
  p|total_cputime;
  p|bg_cputime;
#endif
  p|objData;
  p|commData;

  p|avail_vector;

  p(next_lb);
}

// CkMarshalledCLBStatsMessage is used in the marshalled parameter in
// the entry function, it is just used to use to pup.
// I don't use CLBStatsMsg directly as marshalled parameter because
// I want the data pointer stored and not to be freed by the Charm++.
void CkMarshalledCLBStatsMessage::free() { 
  int count = msgs.size();
  for  (int i=0; i<count; i++) {
    delete msgs[i];
    msgs[i] = NULL;
  }
  msgs.clear();
}

void CkMarshalledCLBStatsMessage::add(CkMarshalledCLBStatsMessage &&m)
{
  int count = m.getCount();
  for (int i=0; i<count; i++) add(m.getMessage(i));
}

void CkMarshalledCLBStatsMessage::pup(PUP::er &p)
{
  int count = msgs.size();
  p|count;
  for (int i=0; i<count; i++) {
    CLBStatsMsg *msg;
    if (p.isUnpacking()) msg = new CLBStatsMsg;
    else { 
      msg = msgs[i]; CmiAssert(msg!=NULL);
    }
    msg->pup(p);
    if (p.isUnpacking()) add(msg);
  }
}

SpanningTree::SpanningTree()
{
	double sq = sqrt(CkNumPes()*4.0-3.0) - 1; // 1 + arity + arity*arity = CkNumPes()
	arity = (int)ceil(sq/2);
	calcParent(CkMyPe());
	calcNumChildren(CkMyPe());
}

void SpanningTree::calcParent(int n)
{
	parent=-1;
	if(n != 0  && arity > 0)
		parent = (n-1)/arity;
}

void SpanningTree::calcNumChildren(int n)
{
	numChildren = 0;
	if (arity == 0) return;
	int fullNode=(CkNumPes()-1-arity)/arity;
	if(n <= fullNode)
		numChildren = arity;
	if(n == fullNode+1)
		numChildren = CkNumPes()-1-(fullNode+1)*arity;
	if(n > fullNode+1)
		numChildren = 0;
}

#include "CentralLB.def.h"
 
/*@}*/
