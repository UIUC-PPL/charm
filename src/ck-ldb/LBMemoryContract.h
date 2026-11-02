#ifndef LB_MEMORY_CONTRACT_H
#define LB_MEMORY_CONTRACT_H

// The LB memory contract: the strategy-side interface that discharges the
// migration runtime's per-device preconditions. For every device g,
//
//   I-final:  sum_{dst=g} phi(m) - sum_{src=g} phi(m)      <=  H_g
//   I-batch:  for each batch, in the order batches run,
//               staged(g)                                 <=  A_g
//               staged(g) - departed(g) + arrived(g)      <=  A_g
//             and for each source PE p, ipc(p)            <=  slots(p)
//
// phi is a chare's resident device footprint, sigma its staged payload block.
//
//   T_g  what a migration can obtain on g. Under +gpupool, the free bytes in
//        the arenas every process on g already holds -- and nothing more: the
//        pool grows a whole arena at a time, so a plan that leaned on growth
//        turned a few hundred MB of misjudgement into a full-arena cudaMalloc
//        mid-migration, and at 2+ nodes every arena is registered with the
//        fabric whole, which has a ceiling (hapiDevPoolNewArenaLocked). A plan
//        that does not fit the arenas is split into more batches or refused,
//        visibly; growth is left to the application's own allocations.
//        Without the pool, free device memory.
//   C_g  under +gpupool, the pool's capacity on g: every byte its arenas
//        span, free or not. The reserve below is measured against it.
//   H_g  T_g - eps*C_g - L_g - sigma_max(g): final placement is planned against
//        T_g less the largest payload block g could pack, so one pack always
//        fits, and less L_g, the landing arenas the runtime may hold on g at
//        once: every PE of g may have one migration window's worth of
//        payloads landing (cklocation.C, CHARM_LB_MIGRATE_WINDOW_MB, 256 MB
//        by default), transient blocks the final state does not show. Planned
//        without L_g a device filled to (1-eps)*T_g stalled its last arrivals
//        for the gate's whole timeout (sph2d strong, job 22196595): 1.1 GB of
//        slack, 1 GB of landings in flight, and fragmentation took the rest.
//        eps and L_g are taken from C_g, not from T_g. Taken from T_g -- the
//        bytes free at this step -- they shrank with it, each step could fill
//        ~70% of what the last had left, and the device the balancer kept
//        feeding ended within one element of full, where blocks stop fitting
//        by shape: landings waited out the gate's timeout and ungated arrivals
//        grew the pool (sph2d strong N=1, jobs 22244226 and 22245278; the rule
//        and the numbers are in LBMigrateWindow.h). Without the pool there is
//        no capacity to read and the margin is eps*T_g as before.
//   A_g  T_g - eps*C_g - L_g less the net change of the batches already released:
//        what a batch stages into. Under the pool the payload block, the
//        landing arena and chare state are all allocations from the same
//        arenas, so there is no separate reserve; the second line is the peak
//        with departures packed before arrivals land, which the admission gate
//        in immigrateGPU enforces.
//
// Without the pool, staging is the +gpulbbuffer region S_g, debited on both
// ends, and the first line reads staged(g) <= S_g instead.
//
// Any strategy whose emitted move set satisfies I-final inherits the runtime's
// no-OOM / no-deadlock guarantee once LBBatchPlanner has split it for I-batch;
// the ContractVerifier below repairs the output of strategies that do not use
// the ledger themselves.
//
// Header-only: strategies and the central LB include this directly.

#include "BaseLB.h"
#include "CentralLB.h"
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <cstdint>
#include <cstdlib>

CkpvExtern(int, _lb_obj_index);  // the footprint slot (see _loadbalancerInit)
int lbObjFootprintSlot();        // the same slot without Ckpv: readable from a strategy helper thread
void lbSetObjFootprintSlot(int slot);

// The runtime's per-PE migration window and the landing reserve it adds up to
// (L_g) come from LBMigrateWindow.h: one rule for the runtime and the contract.
#include "LBMigrateWindow.h"

// ---------------------------------------------------------------------------
// LBMemoryTopology: where PEs are, as the migration transport sees it. The
// runtime answers from Converse; a test substitutes its own to exercise
// cross-process cases on one process.
// ---------------------------------------------------------------------------
class LBMemoryTopology {
public:
  virtual ~LBMemoryTopology() {}
  virtual int nodeOf(int pe) const { return CmiNodeOf(pe); }
  virtual bool samePhysicalNode(int a, int b) const {
    return CmiPeOnSamePhysicalNode(a, b);
  }
  // Whether a move within one process and one device hands the chare over
  // without serializing it (CkLocMgr::emigrateIntraProcess).
  virtual bool intraProcessHandoff() const { return true; }
  static const LBMemoryTopology& runtime() {
    static const LBMemoryTopology t;
    return t;
  }
};

// ---------------------------------------------------------------------------
// LBMemoryModel: one uniform view over what the contract needs, built from
// the stats every central strategy already receives. PEs are grouped into
// devices by gpu_device_id, mirroring the GPU-group construction the GPU
// strategies use.
// ---------------------------------------------------------------------------
class LBMemoryModel {
public:
  struct Device {
    uint64_t gpu_id;
    size_t reach;         // T_g
    size_t capacity;      // C_g: the pool's extent on the device; 0 if not reported
    size_t stagingFree;   // S_g; equal to reach when the device is pooled
    bool pooled;          // staging, landing and chare state share the pool
    size_t sigmaMax;      // largest payload block a migratable object here packs
    std::vector<int> pes; // PEs mapped to this device
  };

  // How a move travels. Only kStaged moves device bytes: the source packs a
  // payload block and frees the chare's state, the destination lands the
  // payload in an arena that becomes the chare's state.
  enum Transport { kNone, kStaged };

  void build(BaseLB::LDStats* stats, const LBMemoryTopology* topo = nullptr) {
    stats_ = stats;
    topo_ = topo ? topo : &LBMemoryTopology::runtime();
    devices_.clear();
    devToIdx_.clear();
    peToDev_.assign(stats->nprocs(), -1);
    peSlots_.assign(stats->nprocs(), 0);
    pooled_ = false;

    std::vector<size_t> devFree, poolSum, poolCap, arena;
    std::vector<std::vector<int>> poolNodes;  // processes already summed per device
    for (int pe = 0; pe < stats->nprocs(); pe++) {
      if (!stats->procs[pe].available) continue;
      const BaseLB::ProcStats& ps = stats->procs[pe];
      auto it = devToIdx_.find(ps.gpu_device_id);
      int d;
      if (it == devToIdx_.end()) {
        d = (int)devices_.size();
        devToIdx_[ps.gpu_device_id] = d;
        Device dev;
        dev.gpu_id = ps.gpu_device_id;
        dev.reach = 0;
        dev.capacity = 0;
        dev.stagingFree = ps.pool_buff_mem_remaining;  // the legacy reading
        dev.pooled = false;
        dev.sigmaMax = 0;
        devices_.push_back(dev);
        devFree.push_back(ps.gpu_mem_remaining);
        poolSum.push_back(0);
        poolCap.push_back(0);
        arena.push_back(0);
        poolNodes.emplace_back();
      } else {
        d = it->second;
      }
      devices_[d].pes.push_back(pe);
      peToDev_[pe] = d;
      peSlots_[pe] = ps.gpu_ipc_slots;
      // PEs report at slightly different moments; take the least.
      devFree[d] = std::min(devFree[d], ps.gpu_mem_remaining);
      if (ps.gpu_pool_arena_bytes > 0) {
        arena[d] = std::max(arena[d], ps.gpu_pool_arena_bytes);
        const int node = topo_->nodeOf(pe);
        if (std::find(poolNodes[d].begin(), poolNodes[d].end(), node) ==
            poolNodes[d].end()) {
          poolNodes[d].push_back(node);
          poolSum[d] += ps.pool_buff_mem_remaining;
          poolCap[d] += ps.gpu_pool_capacity_bytes;
        }
      }
    }

    for (int d = 0; d < (int)devices_.size(); d++) {
      Device& dev = devices_[d];
      if (arena[d] > 0) {
        dev.pooled = true;
        pooled_ = true;
        dev.reach = poolSum[d];  // the arenas the pool has; no growth (T_g)
        dev.capacity = poolCap[d];
      } else {
        dev.reach = devFree[d];
      }
      if (dev.pooled) dev.stagingFree = dev.reach;
    }

    // sigma_max after pooled_ is known: under the pool a payload is a buddy
    // block, so its size is rounded.
    for (int i = 0; i < (int)stats->objData.size(); i++) {
      if (!stats->objData[i].migratable) continue;
      const int d = deviceOfPe(stats->from_proc[i]);
      if (d < 0) continue;
      devices_[d].sigmaMax = std::max(devices_[d].sigmaMax, stagedSize(i));
    }
  }

  int numDevices() const { return (int)devices_.size(); }
  const Device& device(int d) const { return devices_[d]; }
  int deviceOfPe(int pe) const {
    return (pe >= 0 && pe < (int)peToDev_.size()) ? peToDev_[pe] : -1;
  }
  int deviceIndexOf(uint64_t gpu_id) const {
    auto it = devToIdx_.find(gpu_id);
    return it == devToIdx_.end() ? -1 : it->second;
  }
  bool pooled() const { return pooled_; }
  const LBMemoryTopology& topology() const { return *topo_; }

  // The buddy block a request of `bytes` occupies in the pool.
  static size_t poolBlock(size_t bytes) {
    if (bytes == 0) return 0;
    size_t b = 4;  // buddy::allocator's min_size
    while (b < bytes) b <<= 1;
    return b;
  }

  // Staged size of object i: the block one migration packs.
  size_t stagedSize(int i) const {
    const size_t s = (size_t)stats_->objData[i].gpuPupSize;
    return pooled_ ? poolBlock(s) : s;
  }

  // Resident device footprint of object i: the user-data slot the AtSync
  // producer fills, floored at the staged size so a missing producer can
  // never read as "free to move". Under the pool a chare that arrives by
  // migration holds at least its landing arena, which is that same block.
  size_t footprint(int i) const {
    size_t f = 0;
#if CMK_LB_USER_DATA
    const int slot = lbObjFootprintSlot();
    if (slot >= 0) f = *(size_t*)stats_->objData[i].getUserData(slot);
#endif
    size_t s = stagedSize(i);
    return f > s ? f : s;
  }

  // A move within one process onto the same device is an ownership handoff
  // and moves no bytes. Everything else stages -- including a move within one
  // process onto a different device, and a move between processes sharing a
  // device (see CkLocMgr::emigrate).
  Transport transport(int fromPe, int toPe) const {
    if (fromPe == toPe) return kNone;
    if (topo_->nodeOf(fromPe) == topo_->nodeOf(toPe) &&
        deviceOfPe(fromPe) == deviceOfPe(toPe) && topo_->intraProcessHandoff())
      return kNone;
    return kStaged;
  }

  // Whether a move's payload holds one of the source PE's CUDA IPC event
  // slots until the destination has read it: cross-process on one physical
  // node (findTransferModeDevice's IPC mode).
  bool usesIpcSlot(int obj, int fromPe, int toPe) const {
    return transport(fromPe, toPe) == kStaged && stagedSize(obj) > 0 &&
           topo_->nodeOf(fromPe) != topo_->nodeOf(toPe) &&
           topo_->samePhysicalNode(fromPe, toPe);
  }

  // The source PE's slot budget for one batch; 0 means unbounded.
  int slotsOf(int pe) const {
    return (pe >= 0 && pe < (int)peSlots_.size()) ? peSlots_[pe] : 0;
  }

  // One line per device: what it can reach, what the objects on it hold, and
  // (under the pool) how many bytes its arenas hand out now -- the attributed
  // footprints should account for nearly all of that.
  void print(const char* who) const {
    for (int d = 0; d < numDevices(); d++) {
      const Device& dev = devices_[d];
      size_t resident = 0, poolFree = 0, arenaSz = 0;
      int objs = 0;
      for (int i = 0; i < (int)stats_->objData.size(); i++)
        if (deviceOfPe(stats_->from_proc[i]) == d) {
          resident += footprint(i);
          objs++;
        }
      std::vector<int> seen;
      for (int pe : dev.pes) {
        const BaseLB::ProcStats& ps = stats_->procs[pe];
        const int node = topo_->nodeOf(pe);
        if (ps.gpu_pool_arena_bytes == 0 ||
            std::find(seen.begin(), seen.end(), node) != seen.end())
          continue;
        seen.push_back(node);
        poolFree += ps.pool_buff_mem_remaining;
        arenaSz = ps.gpu_pool_arena_bytes;
      }
      CkPrintf("%s device %d (gpu %llu, %zu PE(s)): T %.1f MB of C %.1f MB, %s, "
               "sigma_max %.1f KB, %d object(s) holding %.1f MB; pool free %.1f MB in "
               "%zu-MB arenas\n",
               who, d, (unsigned long long)dev.gpu_id, dev.pes.size(),
               dev.reach / 1048576.0, dev.capacity / 1048576.0,
               dev.pooled ? "pooled" : "separate staging",
               dev.sigmaMax / 1024.0, objs, resident / 1048576.0, poolFree / 1048576.0,
               arenaSz >> 20);
    }
  }

private:
  std::vector<Device> devices_;
  std::unordered_map<uint64_t, int> devToIdx_;
  std::vector<int> peToDev_;
  std::vector<int> peSlots_;
  bool pooled_ = false;
  BaseLB::LDStats* stats_ = nullptr;
  const LBMemoryTopology* topo_ = nullptr;
};

// ---------------------------------------------------------------------------
// MemoryLedger: transactional feasibility for planning I-final. A staged move
// debits its footprint at the destination and credits it at the source, which
// frees the chare's state at pack. The batch planner is what makes that
// credit safe to take.
//
// waveStaging: for a strategy whose waves execute unbatched, the whole wave's
// staging must also fit, so sigma is debited per move. CentralLB batches every
// step, so the verifier and batch-aware strategies leave it off.
// ---------------------------------------------------------------------------
class MemoryLedger {
public:
  void init(const LBMemoryModel* model, double headroom = 0.95,
            bool waveStaging = true) {
    model_ = model;
    waveStaging_ = waveStaging;
    memAvail_.resize(model->numDevices());
    stagingAvail_.resize(model->numDevices());
    for (int d = 0; d < model->numDevices(); d++) {
      const LBMemoryModel::Device& dev = model->device(d);
      long long h;
      if (dev.pooled) {
        // Less the margin and L_g (landings in flight), both from C_g, and
        // one pack.
        h = (long long)lbPlannableBytes(dev.reach, dev.capacity, dev.pes.size(), headroom);
        h -= (long long)dev.sigmaMax;
      } else {
        h = (long long)((double)dev.reach * headroom);
      }
      memAvail_[d] = h > 0 ? h : 0;
      stagingAvail_[d] = (long long)dev.stagingFree;
    }
  }

  bool feasible(int obj, int fromPe, int toPe) const {
    int s = model_->deviceOfPe(fromPe), d = model_->deviceOfPe(toPe);
    if (s < 0 || d < 0) return true;
    if (model_->transport(fromPe, toPe) == LBMemoryModel::kNone) return true;
    const long long f = (long long)model_->footprint(obj);
    const long long sig = (long long)model_->stagedSize(obj);
    if (f == 0 && sig == 0) return true;        // no device state
    const LBMemoryModel::Device& src = model_->device(s);
    if (sig > (long long)src.stagingFree) return false;  // could never be packed
    if (s != d && memAvail_[d] < f) return false;
    if (waveStaging_) {
      if (src.pooled) {
        // The wave's payloads draw on the pool that H_g held sigma_max back in.
        if (memAvail_[s] + (long long)src.sigmaMax < sig) return false;
      } else if (stagingAvail_[s] < sig ||
                 (!model_->device(d).pooled && stagingAvail_[d] < sig)) {
        return false;
      }
    }
    return true;
  }

  void commit(int obj, int fromPe, int toPe) { apply(obj, fromPe, toPe, +1); }
  void rollback(int obj, int fromPe, int toPe) { apply(obj, fromPe, toPe, -1); }

  // New device state of `bytes` appearing on toPe's device with no departure
  // anywhere: a copy of an object (LDStats::clones). The copy lands in pool
  // memory, so it draws on H_g like a migration arrival; nothing is staged.
  bool feasibleArrival(size_t bytes, int toPe) const {
    const int d = model_->deviceOfPe(toPe);
    if (d < 0 || bytes == 0) return true;
    return memAvail_[d] >= (long long)bytes;
  }
  void commitArrival(size_t bytes, int toPe) {
    const int d = model_->deviceOfPe(toPe);
    if (d < 0) return;
    memAvail_[d] -= (long long)bytes;
  }

  size_t memAvailOn(int dev) const {
    return memAvail_[dev] > 0 ? (size_t)memAvail_[dev] : 0;
  }
  size_t stagingAvailOn(int dev) const {
    return stagingAvail_[dev] > 0 ? (size_t)stagingAvail_[dev] : 0;
  }

private:
  void apply(int obj, int fromPe, int toPe, int sign) {
    int s = model_->deviceOfPe(fromPe), d = model_->deviceOfPe(toPe);
    if (s < 0 || d < 0) return;
    if (model_->transport(fromPe, toPe) == LBMemoryModel::kNone) return;
    const long long f = (long long)model_->footprint(obj) * sign;
    const long long sig = (long long)model_->stagedSize(obj) * sign;
    if (s != d) {
      memAvail_[d] -= f;
      memAvail_[s] += f;  // departure frees at pack: the two-phase credit
    }
    if (waveStaging_) {
      if (model_->device(s).pooled) {
        memAvail_[s] -= sig;
      } else {
        stagingAvail_[s] -= sig;
        if (!model_->device(d).pooled) stagingAvail_[d] -= sig;
      }
    }
  }

  const LBMemoryModel* model_ = nullptr;
  bool waveStaging_ = true;
  std::vector<long long> memAvail_;
  std::vector<long long> stagingAvail_;
};

// ---------------------------------------------------------------------------
// ContractVerifier: hardens ANY strategy's finished move list against I-final.
// Sums each device's net change over the whole move list, so departures credit
// the arrivals they make room for whatever order the strategy listed them in
// -- a swap between two full devices nets to zero and passes. While a device
// is over its H_g, the arrival that brings the least measured load per byte is
// refused; refusing it takes the credit back from its source, which is
// re-checked in turn. The bytes are what the device is short of and the load
// is what the strategy moved the object for, so this keeps the most of the
// plan the memory allows: sph2d's idle patches hold a full 74 MB each and do
// no work, and refusing by size alone (every footprint equal) dropped active
// and idle arrivals alike, leaving the device that collects the idle ones
// short of the load it was planned to carry. Equal densities fall back to the
// larger footprint, then the object index, so the outcome is deterministic. A
// move whose payload no batch could ever stage is refused first. A refused
// move keeps its chare where it is, which is always feasible. Returns the
// number of refused moves; refused entries have to_pe set back to from_pe.
// Staging is not checked here: the batch planner splits the step for it.
// ---------------------------------------------------------------------------
class ContractVerifier {
public:
  // moves reference objects by stats index; from/to are PEs.
  struct Move {
    int obj;
    int fromPe;
    int* toPe;  // points into the strategy's decision storage, edited on refusal
  };

  // The bytes a copy of object i adds to its destination device: what the
  // object holds plus what one copy holds for the exchange between copies
  // (LDObjData::replicaExtraBytes). When the object is not yet replicated
  // its own device grows by the extra too (sourceExtra).
  static size_t cloneBytes(const LBMemoryModel& model, const BaseLB::LDStats* stats, int i) {
    return model.footprint(i) + stats->objData[i].replicaExtraBytes;
  }
  static size_t sourceExtra(const BaseLB::LDStats* stats, int i) {
    const LDObjData& od = stats->objData[i];
    return od.replicaCount <= 1 ? od.replicaExtraBytes : 0;
  }

  // `clones`, when given, are the strategy's copy decisions (LDStats::clones):
  // arrivals with no departure. A refused clone is removed from the list.
  static int verifyAndRepair(BaseLB::LDStats* stats, std::vector<Move>& moves,
                             double headroom = 0.95,
                             const LBMemoryTopology* topo = nullptr,
                             std::vector<LBAction>* clones = nullptr) {
    LBMemoryModel model;
    model.build(stats, topo);
    const int D = model.numDevices();
    if (D == 0) return 0;
    MemoryLedger ledger;  // for H_g
    ledger.init(&model, headroom, /*waveStaging=*/false);

    const bool dbg = (getenv("CHARM_DEBUG_MEMCONTRACT") != NULL);
    int refused = 0, refusedClones = 0;
    if (dbg) model.print("[memcontract]");

    auto refuse = [&](Move& m, const char* why) {
      if (dbg)
        CkPrintf("[%d] memcontract: refusing move of obj %d (%zu bytes) pe %d -> %d: %s\n",
                 CkMyPe(), m.obj, model.footprint(m.obj), m.fromPe, *m.toPe, why);
      *m.toPe = m.fromPe;
      refused++;
    };

    // Arrivals are keyed so moves and clones sort together: key k < M is
    // moves[k], key M + j is (*clones)[j].
    const int M = (int)moves.size();
    const int C = clones ? (int)clones->size() : 0;
    std::vector<char> cloneDropped(C, 0);
    auto isClone = [&](int key) { return key >= M; };
    auto objOf = [&](int key) { return isClone(key) ? (*clones)[key - M].obj : moves[key].obj; };
    auto bytesOf = [&](int key) {
      return isClone(key) ? cloneBytes(model, stats, objOf(key)) : model.footprint(objOf(key));
    };

    std::vector<long long> net(D, 0);
    std::vector<std::vector<int>> arrivals(D);  // keys, see above
    for (size_t k = 0; k < moves.size(); k++) {
      Move& m = moves[k];
      if (*m.toPe == m.fromPe) continue;
      const int s = model.deviceOfPe(m.fromPe), d = model.deviceOfPe(*m.toPe);
      if (s < 0 || d < 0) continue;
      if (model.transport(m.fromPe, *m.toPe) == LBMemoryModel::kNone) continue;
      if (model.stagedSize(m.obj) > model.device(s).stagingFree) {
        refuse(m, "payload larger than the source can ever stage");
        continue;
      }
      if (s == d) continue;
      const long long f = (long long)model.footprint(m.obj);
      net[d] += f;
      net[s] -= f;
      arrivals[d].push_back((int)k);
    }
    for (int j = 0; j < C; j++) {
      const LBAction& a = (*clones)[j];
      const int s = model.deviceOfPe(a.fromPe), d = model.deviceOfPe(a.toPe);
      if (d < 0) continue;
      net[d] += (long long)cloneBytes(model, stats, a.obj);
      if (s >= 0) net[s] += (long long)sourceExtra(stats, a.obj);
      arrivals[d].push_back(M + j);
    }
    auto loadOf = [&](int key) {
      const LDObjData& od = stats->objData[objOf(key)];
      double l = (double)od.wallTime;
#if CMK_GPU
      l += (double)od.gpuTime + (double)od.driverTime;
#endif
      // A copy carries the group's load split one way further.
      if (isClone(key)) l /= (double)(od.replicaCount + 1);
      return l > 0.0 ? l : 0.0;
    };
    for (int g = 0; g < D; g++)
      std::sort(arrivals[g].begin(), arrivals[g].end(), [&](int a, int b) {
        const size_t fa = bytesOf(a), fb = bytesOf(b);
        // load/footprint ascending, cross-multiplied so a move without a
        // footprint -- which refusing would free nothing for -- sorts last.
        const double da = loadOf(a) * (double)fb, db = loadOf(b) * (double)fa;
        if (da != db) return da < db;
        if (fa != fb) return fa > fb;
        return objOf(a) < objOf(b);
      });

    std::vector<size_t> cursor(D, 0);
    for (bool again = true; again;) {
      again = false;
      for (int g = 0; g < D; g++) {
        const long long H = (long long)ledger.memAvailOn(g);
        while (net[g] > H && cursor[g] < arrivals[g].size()) {
          const int key = arrivals[g][cursor[g]++];
          if (isClone(key)) {
            LBAction& a = (*clones)[key - M];
            if (dbg)
              CkPrintf("[%d] memcontract: refusing copy of obj %d (%zu bytes) onto pe %d: "
                       "destination device would overfill\n",
                       CkMyPe(), a.obj, cloneBytes(model, stats, a.obj), a.toPe);
            cloneDropped[key - M] = 1;
            refusedClones++;
            net[g] -= (long long)cloneBytes(model, stats, a.obj);
            const int s = model.deviceOfPe(a.fromPe);
            if (s >= 0) {
              net[s] -= (long long)sourceExtra(stats, a.obj);
              if (s < g) again = true;
            }
            continue;
          }
          Move& m = moves[key];
          if (*m.toPe == m.fromPe) continue;
          const int s = model.deviceOfPe(m.fromPe);
          const long long f = (long long)model.footprint(m.obj);
          refuse(m, "destination device would overfill");
          net[g] -= f;
          net[s] += f;
          if (s < g) again = true;  // a device already passed lost credit
        }
      }
    }
    if (refusedClones) {
      std::vector<LBAction> kept;
      for (int j = 0; j < C; j++)
        if (!cloneDropped[j]) kept.push_back((*clones)[j]);
      clones->swap(kept);
    }
    if (refused || refusedClones)
      CkPrintf("CharmLB> memory contract: refused %d of %zu move(s) and %d of %d copy(ies) "
               "that would overfill a device\n", refused, moves.size(), refusedClones, C);
    return refused + refusedClones;
  }
};

// ---------------------------------------------------------------------------
// LBBatchPlanner: discharges I-batch. Splits the decided moves into batches
// that run one after another (CentralLB::ReleaseNextBatch). Each batch starts
// as every move not yet placed and sheds moves until every device and every
// source PE is within bounds:
//
//   pooled device g:  staged(g) <= A_g  and  staged(g) - departed(g) + arrived(g) <= A_g
//   legacy device g:  staged(g) + landed(g) <= S_g  and  arrived(g) - departed(g) <= A_g
//   source PE p:      ipc(p) <= slots(p)
//
// where A_g starts at (1-eps)*T_g and each released batch moves it by its net
// change. A move within one device stages but changes no final footprint.
// Moves that move no device bytes ride batch 0. Returns the number of
// batches, and batchOf[i] for every object whose to != from. A move that
// cannot run in any batch -- which I-final with its sigma_max holdback should
// rule out -- is refused (to_proc reset to from_proc) and counted in *refused.
// ---------------------------------------------------------------------------
class LBBatchPlanner {
public:
  static int plan(BaseLB::LDStats* stats, std::vector<int>& batchOf,
                  int* refusedOut = nullptr, double headroom = 0.95,
                  const LBMemoryTopology* topo = nullptr) {
    batchOf.assign(stats->objData.size(), 0);
    if (refusedOut) *refusedOut = 0;

    std::vector<int> moved;
    for (int i = 0; i < (int)stats->objData.size(); i++)
      if (stats->to_proc[i] != stats->from_proc[i]) moved.push_back(i);
    if (moved.empty()) return 1;

    LBMemoryModel model;
    model.build(stats, topo);
    const int D = model.numDevices();
    if (D == 0) return 1;

    struct Mv { int obj, from, s, d; long long sig, phi; bool ipc; };
    std::vector<Mv> mv;
    for (int i : moved) {
      const int from = stats->from_proc[i], to = stats->to_proc[i];
      const int s = model.deviceOfPe(from), d = model.deviceOfPe(to);
      if (s < 0 || d < 0) continue;
      if (model.transport(from, to) == LBMemoryModel::kNone) continue;
      const long long sig = (long long)model.stagedSize(i);
      const long long phi = (long long)model.footprint(i);
      if (sig == 0 && phi == 0) continue;
      mv.push_back(Mv{i, from, s, d, sig, phi, model.usesIpcSlot(i, from, to)});
    }
    if (mv.empty()) return 1;

    const int P = stats->nprocs();
    std::vector<long long> avail(D);
    for (int g = 0; g < D; g++) {
      const LBMemoryModel::Device& dev = model.device(g);
      // Under the pool, less the margin and L_g: the landing arenas the
      // batch's arrivals hold while in flight, bounded by the window on each
      // of the device's PEs. The same reserve I-final planned with.
      avail[g] = dev.pooled
                     ? (long long)lbPlannableBytes(dev.reach, dev.capacity,
                                                   dev.pes.size(), headroom)
                     : (long long)((double)dev.reach * headroom);
      if (avail[g] < 0) avail[g] = 0;
    }

    std::vector<int> remaining(mv.size());
    for (size_t k = 0; k < mv.size(); k++) remaining[k] = (int)k;

    int nb = 0;
    while (!remaining.empty()) {
      std::vector<char> in(mv.size(), 0);
      std::vector<long long> stg(D, 0), net(D, 0);
      std::vector<int> ipc(P, 0);
      auto account = [&](int k, int sign) {
        const Mv& m = mv[k];
        const bool ps = model.device(m.s).pooled, pd = model.device(m.d).pooled;
        stg[m.s] += sign * m.sig;
        if (ps) net[m.s] += sign * m.sig;
        if (!pd && m.d != m.s) stg[m.d] += sign * m.sig;
        if (!ps && m.d == m.s) stg[m.d] += sign * m.sig;  // lands in the same region
        if (m.s != m.d) {
          net[m.s] -= sign * m.phi;
          net[m.d] += sign * m.phi;
        }
        if (m.ipc) ipc[m.from] += sign;
      };
      for (int k : remaining) { in[k] = 1; account(k, +1); }

      // Candidates to shed, largest first, walked with cursors.
      std::vector<std::vector<int>> depBySig(D), landBySig(D), arrByPhi(D),
                                    sameBySig(D), ipcBySig(P);
      for (int k : remaining) {
        const Mv& m = mv[k];
        depBySig[m.s].push_back(k);
        if (m.s != m.d) {
          landBySig[m.d].push_back(k);
          arrByPhi[m.d].push_back(k);
        } else {
          sameBySig[m.s].push_back(k);
        }
        if (m.ipc) ipcBySig[m.from].push_back(k);
      }
      auto bySig = [&](int a, int b) { return mv[a].sig > mv[b].sig; };
      auto byPhi = [&](int a, int b) { return mv[a].phi > mv[b].phi; };
      for (int g = 0; g < D; g++) {
        std::sort(depBySig[g].begin(), depBySig[g].end(), bySig);
        std::sort(landBySig[g].begin(), landBySig[g].end(), bySig);
        std::sort(arrByPhi[g].begin(), arrByPhi[g].end(), byPhi);
        std::sort(sameBySig[g].begin(), sameBySig[g].end(), bySig);
      }
      for (int p = 0; p < P; p++)
        std::sort(ipcBySig[p].begin(), ipcBySig[p].end(), bySig);
      std::vector<size_t> cDep(D, 0), cLand(D, 0), cArr(D, 0), cSame(D, 0), cIpc(P, 0);
      auto next = [&](const std::vector<int>& list, size_t& c) {
        while (c < list.size() && !in[list[c]]) c++;
        return c < list.size() ? list[c] : -1;
      };

      std::vector<int> devQ, peQ;
      std::vector<char> devQueued(D, 1), peQueued(P, 1);
      for (int g = 0; g < D; g++) devQ.push_back(g);
      for (int p = 0; p < P; p++) peQ.push_back(p);
      auto shed = [&](int k) {
        in[k] = 0;
        account(k, -1);
        const Mv& m = mv[k];
        for (int g : {m.s, m.d})
          if (!devQueued[g]) { devQueued[g] = 1; devQ.push_back(g); }
        if (m.ipc && !peQueued[m.from]) { peQueued[m.from] = 1; peQ.push_back(m.from); }
      };

      while (!devQ.empty() || !peQ.empty()) {
        while (!devQ.empty()) {
          const int g = devQ.back();
          devQ.pop_back();
          devQueued[g] = 0;
          const LBMemoryModel::Device& dev = model.device(g);
          for (;;) {
            const long long stagingCap =
                dev.pooled ? avail[g] : (long long)dev.stagingFree;
            int k = -1;
            if (stg[g] > stagingCap) {
              k = next(depBySig[g], cDep[g]);
              if (!dev.pooled) {
                const int l = next(landBySig[g], cLand[g]);
                if (k < 0 || (l >= 0 && mv[l].sig > mv[k].sig)) k = l;
              }
            } else if (net[g] > avail[g]) {
              // What raises the net: arrivals, and moves that stage without
              // changing a footprint (within one device).
              const int a = next(arrByPhi[g], cArr[g]);
              const int w = next(sameBySig[g], cSame[g]);
              k = a;
              if (k < 0 || (w >= 0 && mv[w].sig > mv[a].phi)) k = w;
              if (k < 0) k = next(depBySig[g], cDep[g]);
            } else {
              break;
            }
            if (k < 0) break;
            shed(k);
          }
        }
        while (!peQ.empty() && devQ.empty()) {
          const int p = peQ.back();
          peQ.pop_back();
          peQueued[p] = 0;
          const int slots = model.slotsOf(p);
          while (slots > 0 && ipc[p] > slots) {
            const int k = next(ipcBySig[p], cIpc[p]);
            if (k < 0) break;
            shed(k);
          }
        }
      }

      std::vector<int> later;
      int placed = 0;
      std::vector<long long> change(D, 0);
      for (int k : remaining) {
        if (!in[k]) { later.push_back(k); continue; }
        const Mv& m = mv[k];
        batchOf[m.obj] = nb;
        placed++;
        if (m.s != m.d) {
          change[m.s] -= m.phi;
          change[m.d] += m.phi;
        }
      }
      if (placed == 0) {
        // Nothing left fits even alone. Refuse the rest: a chare that stays
        // put is always feasible.
        for (int k : remaining) {
          stats->to_proc[mv[k].obj] = stats->from_proc[mv[k].obj];
          batchOf[mv[k].obj] = 0;
        }
        if (refusedOut) *refusedOut = (int)remaining.size();
        CkPrintf("CharmLB> memory contract: %zu move(s) fit no batch and were "
                 "refused\n", remaining.size());
        break;
      }
      for (int g = 0; g < D; g++) avail[g] -= change[g];
      remaining.swap(later);
      nb++;
    }
    return nb > 0 ? nb : 1;
  }
};

#endif  // LB_MEMORY_CONTRACT_H
