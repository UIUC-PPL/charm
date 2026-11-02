/**
 * \addtogroup CkLdb
*/
/*@{*/

/**
 * ShedLB: threshold refinement minimising migrated bytes. See ShedLB.h for
 * what it guarantees and why it is not another repacking strategy.
*/

#include "charm++.h"
#include "ShedLB.h"
#if CMK_GPU
#include "LBMemoryContract.h"
#endif

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <vector>

extern int quietModeRequested;

CreateLBFunc_Def(ShedLB, "Threshold refinement that minimises migrated bytes")

ShedLB::ShedLB(const CkLBOptions& opt) : CBase_ShedLB(opt)
{
  lbname = "ShedLB";
  if ((CkMyPe() == 0) && !quietModeRequested)
    CkPrintf("CharmLB> ShedLB created.\n");
}

ShedLB::ShedLB(CkMigrateMessage* m) : CBase_ShedLB(m) { lbname = "ShedLB"; }

namespace
{
// What one migration of this object stages. Unknown sizes fall back to 1, so
// the objective degenerates to minimising the NUMBER of moves rather than
// silently treating every object as free.
inline double objBytes(const LDObjData& od)
{
#if CMK_GPU
  const double b = (double)od.gpuPupSize;
  return b > 0.0 ? b : 1.0;
#else
  (void)od;
  return 1.0;
#endif
}
}  // namespace

void ShedLB::work(LDStats* stats)
{
  const int n_pes = stats->nprocs();
  const int n_objs = (int)stats->objData.size();
  if (n_pes <= 0 || n_objs <= 0) return;

  // ---- which resource is being balanced --------------------------------
  // Device time when there is any, because a migratable object's cost is
  // then its kernels and the PEs sharing a GPU are one bin. Otherwise host
  // time, one bin per PE.
  bool useGpu = false;
#if CMK_GPU
  {
    double gpuTotal = 0.0;
    for (int i = 0; i < n_objs; i++)
      if (stats->objData[i].migratable) gpuTotal += stats->objData[i].gpuTime;
    useGpu = gpuTotal > 0.0;
  }
#endif

  // ---- bins ------------------------------------------------------------
  std::vector<Bin> bins;
  std::vector<int> peToBin(n_pes, -1);
  {
    std::unordered_map<uint64_t, int> devToBin;
    for (int pe = 0; pe < n_pes; pe++)
    {
      if (!stats->procs[pe].available) continue;
      int b;
      if (useGpu)
      {
#if CMK_GPU
        const uint64_t dev = stats->procs[pe].gpu_device_id;
        auto it = devToBin.find(dev);
        if (it == devToBin.end()) { b = (int)bins.size(); devToBin[dev] = b; bins.push_back(Bin()); }
        else b = it->second;
#else
        b = (int)bins.size(); bins.push_back(Bin());
#endif
      }
      else
      {
        b = (int)bins.size();
        bins.push_back(Bin());
      }
      peToBin[pe] = b;
      bins[b].pes.push_back(pe);
    }
  }
  const int nbins = (int)bins.size();
  if (nbins <= 1) return;   // nothing to balance across

  // ---- loads, and the candidates each bin could shed -------------------
  // Non-migratable objects are background: they count toward a bin's load and
  // can never be shed, which is what keeps the threshold honest.
  std::vector<double> peCpu(n_pes, 0.0);
  std::vector<Cand> cands;
  cands.reserve(n_objs);
  double total = 0.0;
  for (int i = 0; i < n_objs; i++)
  {
    const LDObjData& od = stats->objData[i];
    const int pe = stats->from_proc[i];
    if (pe < 0 || pe >= n_pes || peToBin[pe] < 0) continue;
    const int b = peToBin[pe];
#if CMK_GPU
    const double load = useGpu ? od.gpuTime : od.wallTime;
#else
    const double load = od.wallTime;
#endif
    bins[b].load += load;
    total += load;
    peCpu[pe] += od.wallTime;
    if (od.migratable) cands.push_back(Cand{i, load, objBytes(od), b});
  }
  if (total <= 0.0) return;

  const double avg = total / nbins;
  double eps = _lb_args.shedEps();
  if (eps < 0.0) eps = 0.0;
  const double T = avg * (1.0 + eps);

  double maxLoad = 0.0;
  for (int b = 0; b < nbins; b++) maxLoad = std::max(maxLoad, bins[b].load);

  // Memory is a constraint here, not the objective: the bytes a move stages
  // are still what shedding minimises, and a destination is only a candidate
  // if it can hold what the object holds (the memory contract's I-final). The
  // two differ under the device pool -- a Compute stages its live prefix but
  // holds its capacity. Staging and IPC slots are the batch planner's.
#if CMK_GPU
  LBMemoryModel memModel;
  memModel.build(stats);
  MemoryLedger ledger;
  ledger.init(&memModel, 0.95, /*waveStaging=*/false);
  int memRefusals = 0;
#endif
  // Where an object placed in each bin would go: its least host-loaded PE.
  auto landingPe = [&](int b) {
    int dstPe = bins[b].pes[0];
    for (int pe : bins[b].pes)
      if (peCpu[pe] < peCpu[dstPe]) dstPe = pe;
    return dstPe;
  };
  std::vector<int> binPe(nbins);
  for (int b = 0; b < nbins; b++) binPe[b] = landingPe(b);

  // ---- multiplicity: replicable groups -------------------------------
  // An object above T cannot be placed anywhere, so for objects the
  // application declared replicable (LDObjData::replicaGroup) the balancer
  // changes how many copies exist before it places anything. A group's load
  // L_g is the sum over its copies; it needs k = ceil(L_g / T) copies, each
  // carrying L_g / k, up to the application's maximum. One copy is added per
  // step and one taken back per step, the latter only when the remaining
  // copies would sit below the mean (T (1 - eps) ~ avg), so the two decisions
  // do not alternate on a load that hovers around T. A copy costs the
  // object's bytes at its destination like a move; nothing is staged for a
  // retirement and the bytes it frees are not spent in the same step.
  // Members of a group never share a bin; the source of a copy and a retiring
  // copy are left out of the shedding below.
  std::vector<char> pinned(cands.size(), 0);
  std::unordered_map<int, std::vector<int>> groupMembers;   // group -> cand indices
  for (size_t k = 0; k < cands.size(); k++)
  {
    const LDObjData& od = stats->objData[cands[k].idx];
    if (od.replicable()) groupMembers[od.replicaGroup].push_back((int)k);
  }
  auto groupHoldsBin = [&](int g, int b) {
    auto it = groupMembers.find(g);
    if (it == groupMembers.end()) return false;
    for (int k : it->second)
      if (cands[k].bin == b) return true;
    return false;
  };
  int clonesPlanned = 0, retiresPlanned = 0;
  for (auto& gm : groupMembers)
  {
    const int g = gm.first;
    std::vector<int>& members = gm.second;
    double L = 0.0;
    int maxCount = 1;
    for (int k : members)
    {
      L += cands[k].load;
      maxCount = std::max(maxCount, stats->objData[cands[k].idx].replicaMax);
    }
    const int count = (int)members.size();
    if (_lb_args.debug() > 0 && CkMyPe() == cur_ld_balancer)
      for (int k : members)
        CkPrintf("[ShedLB] group %d: copy %d of %d on PE %d, load %.4f (max %d)\n", g,
                 stats->objData[cands[k].idx].replicaRank, count, stats->from_proc[cands[k].idx],
                 cands[k].load, maxCount);
    if (L <= 0.0) continue;
    int want = (int)std::ceil(L / T);
    if (want < 1) want = 1;
    if (want > maxCount) want = maxCount;

    // The recurring cost of a copy, as the application declared it: what
    // one exchange with one peer adds to a copy per balancing interval, on
    // the same host or across hosts (the physical node: a Charm++ node is a
    // process). The measured loads leave it out, so each copy's exchanges
    // with the others are counted into its bin here.
    const LDObjData& odg = stats->objData[cands[members[0]].idx];
    auto memberPe = [&](int k) { return stats->from_proc[cands[k].idx]; };
    auto peerCost = [&](int peA, int peB) {
      return CmiPhysicalNodeID(peA) == CmiPhysicalNodeID(peB) ? odg.replicaCostNode : odg.replicaCostRemote;
    };
    std::vector<double> xcost(members.size(), 0.0);
    if (count > 1)
      for (size_t a = 0; a < members.size(); a++)
      {
        for (size_t b = 0; b < members.size(); b++)
          if (a != b) xcost[a] += peerCost(memberPe(members[a]), memberPe(members[b]));
        bins[cands[members[a]].bin].load += xcost[a];
      }
    double before = 0.0;
    for (int b = 0; b < nbins; b++) before = std::max(before, bins[b].load);

    if (want > count)
    {
      // One more copy. The destination is the fullest bin that still clears T
      // with the copy's share, else the emptiest; never a bin the group is
      // already on, and only where the memory ledger says the copy fits.
      const double share = L / (count + 1);
      int src = members[0];
      for (int k : members)
        if (bins[cands[k].bin].load > bins[cands[src].bin].load) src = k;
      const LDObjData& sod = stats->objData[cands[src].idx];
      const int srcPe = stats->from_proc[cands[src].idx];
      int dst = -1;
      double bestAfter = -1.0, emptiest = std::numeric_limits<double>::max();
      int emptiestBin = -1;
      for (int b = 0; b < nbins; b++)
      {
        if (groupHoldsBin(g, b)) continue;
#if CMK_GPU
        if (!ledger.feasibleArrival(memModel.footprint(cands[src].idx) + sod.replicaExtraBytes, binPe[b]))
        {
          memRefusals++;
          continue;
        }
#endif
        const double after = bins[b].load + share;
        if (after <= T && after > bestAfter) { bestAfter = after; dst = b; }
        if (bins[b].load < emptiest) { emptiest = bins[b].load; emptiestBin = b; }
      }
      if (dst < 0) dst = emptiestBin;
#if CMK_GPU
      // The source grows by the exchange state too when it was alone.
      if (dst >= 0 && count == 1 && !ledger.feasibleArrival(sod.replicaExtraBytes, srcPe))
      {
        memRefusals++;
        dst = -1;
      }
#endif
      if (dst < 0)
      {
        if (CkMyPe() == cur_ld_balancer)
          CkPrintf("CharmLB> ShedLB: group %d carries %.3f of a mean %.3f and would need %d copies, "
                   "but no bin can hold one\n", g, L, avg, want);
        continue;
      }
      const int dstPe = binPe[dst];
      // A copy is made only where it lowers the step's critical path, the
      // fullest bin anywhere, by more than the tolerance that defines T, with
      // every copy carrying its share and its exchanges. A copy is paid for
      // every step, so relieving a bin that was not the fullest, or leaving
      // another bin as full as the one relieved, buys nothing: eight experts
      // on eight GPUs, each 0.7-1.4x the mean, got two copies that lowered
      // nothing (job 23388275: +29% in bf16 training, +35% in bf16 serving);
      // on Mixtral's real routing (hottest 1.27x, the next 1.18x) a copy that
      // lowered its own bin but not the fullest cost +15% in training and
      // gained 10% in serving, where the exchange is free (job 23411075):
      // the declared cost is what tells the two apart.
      std::vector<double> newx(members.size());
      double dstx = 0.0;
      for (size_t a = 0; a < members.size(); a++)
      {
        newx[a] = xcost[a] + peerCost(memberPe(members[a]), dstPe);
        dstx += peerCost(dstPe, memberPe(members[a]));
      }
      double after = 0.0;
      for (int b = 0; b < nbins; b++)
      {
        double l = bins[b].load + (b == dst ? share + dstx : 0.0);
        for (size_t a = 0; a < members.size(); a++)
          if (cands[members[a]].bin == b) l += (share + newx[a]) - (cands[members[a]].load + xcost[a]);
        after = std::max(after, l);
      }
      if (after > before - eps * avg)
      {
        if (CkMyPe() == cur_ld_balancer)
          CkPrintf("CharmLB> ShedLB: group %d: load %.3f exceeds the per-bin target %.3f, but a copy "
                   "on the best bin (PE %d, %.3f; exchange %.3f per copy) would leave the fullest bin "
                   "at %.3f of %.3f: no copy\n", g, L, T, dstPe, bins[dst].load, dstx, after, before);
        continue;
      }
      int rank = 0;
      for (bool used = true; used; rank++)
      {
        used = false;
        for (int k : members)
          if (stats->objData[cands[k].idx].replicaRank == rank) used = true;
      }
      rank--;
#if CMK_GPU
      ledger.commitArrival(memModel.footprint(cands[src].idx) + sod.replicaExtraBytes, dstPe);
      if (count == 1) ledger.commitArrival(sod.replicaExtraBytes, srcPe);
#endif
      for (size_t a = 0; a < members.size(); a++)
      {
        const int k = members[a];
        bins[cands[k].bin].load += (share + newx[a]) - (cands[k].load + xcost[a]);
        cands[k].load = share;
        xcost[a] = newx[a];
      }
      bins[dst].load += share + dstx;
      peCpu[dstPe] += sod.wallTime / (count + 1);
      pinned[src] = 1;
      stats->clones.push_back(LBAction{cands[src].idx, srcPe, dstPe, rank});
      for (int k : members)
        if (k != src) stats->cloneNotices.push_back(LBAction{cands[k].idx, memberPe(k), dstPe, rank});
      clonesPlanned++;
      if (CkMyPe() == cur_ld_balancer)
        CkPrintf("CharmLB> ShedLB: replicate group %d: load %.3f exceeds the per-bin target %.3f "
                 "(mean %.3f); copy %d from PE %d to PE %d, each copy %.3f plus %.3f of exchange; "
                 "the fullest bin %.3f -> %.3f\n",
                 g, L, T, avg, rank, srcPe, dstPe, share, dstx, before, after);
      // The new copy is a member for the same-bin rule for the rest of the step.
      cands.push_back(Cand{cands[src].idx, share, cands[src].bytes, dst});
      pinned.push_back(1);
      members.push_back((int)cands.size() - 1);
      xcost.push_back(dstx);
    }
    else if (count > 1)
    {
      // One copy fewer: the highest-ranked copy, so the application's
      // partition of the work over ranks 0..k-1 stays contiguous. Taken back
      // when the remaining copies would sit below the mean (T (1 - eps) ~
      // avg), or when the exchanges cost the critical path more than the
      // sharing saves it.
      const double share = L / (count - 1);
      int victim = -1;
      for (int k : members)
      {
        if (stats->objData[cands[k].idx].replicaRank == 0) continue;
        if (victim < 0 || stats->objData[cands[k].idx].replicaRank >
                              stats->objData[cands[victim].idx].replicaRank)
          victim = k;
      }
      if (victim < 0) continue;
      const int vPe = stats->from_proc[cands[victim].idx];
      size_t va = 0;
      for (size_t a = 0; a < members.size(); a++) if (members[a] == victim) va = a;
      double after = 0.0;
      for (int b = 0; b < nbins; b++)
      {
        double l = bins[b].load;
        for (size_t a = 0; a < members.size(); a++)
        {
          const int k = members[a];
          if (cands[k].bin != b) continue;
          if (k == victim) l -= cands[k].load + xcost[a];
          else l += (share + xcost[a] - peerCost(memberPe(k), vPe)) - (cands[k].load + xcost[a]);
        }
        after = std::max(after, l);
      }
      const bool fits = share <= T * (1.0 - eps);
      const bool cheaper = after <= before - eps * avg;
      if (!fits && !cheaper) continue;
      bins[cands[victim].bin].load -= cands[victim].load + xcost[va];
      peCpu[vPe] -= stats->objData[cands[victim].idx].wallTime;
      for (size_t a = 0; a < members.size(); a++)
      {
        const int k = members[a];
        if (k == victim) continue;
        const double nx = xcost[a] - peerCost(memberPe(k), vPe);
        bins[cands[k].bin].load += (share + nx) - (cands[k].load + xcost[a]);
        cands[k].load = share;
        xcost[a] = nx;
      }
      cands[victim].load = 0.0;
      pinned[victim] = 1;
      const int vRank = stats->objData[cands[victim].idx].replicaRank;
      stats->retires.push_back(LBAction{cands[victim].idx, vPe, vPe, vRank});
      for (int k : members)
        if (k != victim) stats->retireNotices.push_back(LBAction{cands[k].idx, memberPe(k), vPe, vRank});
      retiresPlanned++;
      if (CkMyPe() == cur_ld_balancer)
        CkPrintf("CharmLB> ShedLB: retire replica %d of group %d on PE %d: %s (load %.3f, %d copies, "
                 "fullest bin %.3f -> %.3f, target %.3f, mean %.3f)\n",
                 stats->objData[cands[victim].idx].replicaRank, g, vPe,
                 fits ? "the group fits fewer copies" : "the exchange costs more than the sharing saves",
                 L, count - 1, before, after, T * (1.0 - eps), avg);
      xcost.erase(xcost.begin() + va);
      members.erase(std::find(members.begin(), members.end(), victim));
    }
  }
  if (clonesPlanned || retiresPlanned)
  {
    maxLoad = 0.0;
    for (int b = 0; b < nbins; b++) maxLoad = std::max(maxLoad, bins[b].load);
  }

  if (maxLoad <= T)
  {
    if (_lb_args.debug() > 0 && CkMyPe() == cur_ld_balancer)
      CkPrintf("[ShedLB] max/avg %.3f already within 1+eps (%.3f): no moves\n",
               maxLoad / avg, 1.0 + eps);
    return;
  }

  // ---- shed and place -------------------------------------------------
  // One transfer at a time, always off the fullest bin. A candidate is only
  // worth moving if some other bin can take it and stay under T: an object
  // larger than a bin's whole share cannot be placed anywhere, so moving it
  // buys nothing and costs its bytes. Deciding shed and placement together is
  // what keeps that case from silently consuming the bin's excess -- an
  // earlier version debited the excess for such an object, put it straight
  // back where it came from, and left the bin overloaded having moved nothing.
  //
  // Among the objects that can be placed, prefer the ones that fit inside the
  // excess (so the bin is not sent below its share for nothing), and among
  // those the highest load per byte: the most excess cleared per byte staged.
  // If none fits inside the excess, take the smallest load, which overshoots
  // least. That last case is the granularity term in the bound.
  std::vector<char> moved(cands.size(), 0);
  int moves = 0;
  double shedBytes = 0.0;

  for (;;)
  {
    int h = 0;
    for (int b = 1; b < nbins; b++)
      if (bins[b].load > bins[h].load) h = b;
    if (bins[h].load <= T) break;
    const double excess = bins[h].load - T;
    for (int b = 0; b < nbins; b++) binPe[b] = landingPe(b);

    int pick = -1, pickDst = -1;
    double pickScore = -1.0, pickSmallest = std::numeric_limits<double>::max();
    for (size_t k = 0; k < cands.size(); k++)
    {
      if (moved[k] || pinned[k]) continue;
      const Cand& c = cands[k];
      if (c.bin != h) continue;
      const LDObjData& cod = stats->objData[c.idx];

      // Best fit among the other bins: fullest that still clears T.
      int dst = -1;
      double bestAfter = -1.0;
      for (int b = 0; b < nbins; b++)
      {
        if (b == h) continue;
        // Copies of one group never share a bin.
        if (cod.replicable() && groupHoldsBin(cod.replicaGroup, b)) continue;
        const double after = bins[b].load + c.load;
        if (!(after <= T && after > bestAfter)) continue;
#if CMK_GPU
        if (!ledger.feasible(c.idx, stats->from_proc[c.idx], binPe[b]))
        {
          memRefusals++;
          continue;
        }
#endif
        bestAfter = after;
        dst = b;
      }
      if (dst < 0) continue;   // nowhere to put it: not a candidate at all

      if (c.load <= excess)
      {
        const double score = c.load / c.bytes;
        if (pickScore < 0.0 || score > pickScore) { pickScore = score; pick = (int)k; pickDst = dst; }
      }
      else if (pickScore < 0.0 && c.load < pickSmallest)
      {
        pickSmallest = c.load;   // only if nothing fits inside the excess
        pick = (int)k;
        pickDst = dst;
      }
    }
    if (pick < 0) break;   // this bin cannot be improved; neither can any fuller one

    Cand& c = cands[pick];
    moved[pick] = 1;
    bins[h].load -= c.load;
    bins[pickDst].load += c.load;
    c.bin = pickDst;
    shedBytes += c.bytes;

    // Within the destination bin, the least host-loaded PE. Host time decides
    // only this, never which bin the object lands on.
    const int oldPe = stats->from_proc[c.idx];
    const int dstPe = binPe[pickDst];
    if (dstPe != oldPe)
    {
#if CMK_GPU
      ledger.commit(c.idx, oldPe, dstPe);
#endif
      stats->to_proc[c.idx] = dstPe;
      peCpu[dstPe] += stats->objData[c.idx].wallTime;
      peCpu[oldPe] -= stats->objData[c.idx].wallTime;
      moves++;
    }
  }

  if (_lb_args.debug() > 0 && CkMyPe() == cur_ld_balancer)
  {
    double after = 0.0;
    for (int b = 0; b < nbins; b++) after = std::max(after, bins[b].load);
    CkPrintf("[ShedLB] %d bins on %s time: max/avg %.3f -> %.3f (target %.3f), "
             "%d of %d objects moved, %.1f MB staged\n",
             nbins, useGpu ? "device" : "host", maxLoad / avg, after / avg,
             1.0 + eps, moves, (int)cands.size(), shedBytes / 1e6);
#if CMK_GPU
    if (memRefusals > 0)
      CkPrintf("[ShedLB] %d destination check(s) refused because the bin could not "
               "hold the object\n", memRefusals);
#endif
  }
}

#include "ShedLB.def.h"

/*@}*/
