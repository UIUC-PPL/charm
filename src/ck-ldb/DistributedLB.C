/**
 * Author: gplkrsh2@illinois.edu (Harshitha Menon)
 * A distributed load balancer.
*/

#include "DistributedLB.h"

#include "elements.h"

extern int quietModeRequested;

CreateLBFunc_Def(DistributedLB, "The distributed load balancer")

using std::vector;

DistributedLB::DistributedLB(CkMigrateMessage *m) : CBase_DistributedLB(m) {
}

DistributedLB::DistributedLB(const CkLBOptions &opt) : CBase_DistributedLB(opt) {
  lbname = "DistributedLB";
  if (CkMyPe() == 0 && !quietModeRequested) {
    CkPrintf("CharmLB> DistributedLB created: threshold %lf, max phases %i\n",
        kTargetRatio, kMaxPhases);
  }
  InitLB(opt);
}

void DistributedLB::initnodeFn()
{
  _registerCommandLineOpt("+DistLBTargetRatio");
  _registerCommandLineOpt("+DistLBMaxPhases");
}

void DistributedLB::InitLB(const CkLBOptions &opt) {
  thisProxy = CProxy_DistributedLB(thisgroup);
  if (opt.getSeqNo() > 0 || (_lb_args.metaLbOn() && _lb_args.metaLbModelDir() != nullptr))
    turnOff();

  // Set constants
  kUseAck = true;
  kPartialInfoCount = -1;
  kMaxPhases = _lb_args.maxDistPhases();
  kTargetRatio = _lb_args.targetRatio();
}

void DistributedLB::Strategy(const DistBaseLB::LDStats* const stats) {
  if (CkMyPe() == 0 && _lb_args.debug() >= 1) {
    start_time = CmiWallTimer();
    CkPrintf("In DistributedLB strategy at %lf\n", start_time);
  }

  // Set constants for this iteration (these depend on CkNumPes() or number of
  // objects, so may not be constant for the entire program)
  kMaxObjPickTrials = stats->n_objs;
  // Maximum number of times we will try to find a PE to transfer an object
  // successfully
  kMaxTrials = CkNumPes();
  // Max gossip messages sent from each PE
  kMaxGossipMsgCount = 2 * CmiLog2(CkNumPes());

  // Reset member variables for this LB iteration
  phase_number = 0;
  my_stats = stats;

	my_load = 0.0;
	for (int i = 0; i < my_stats->n_objs; i++) {
		my_load += my_stats->objData[i].wallTime; 
  }
  init_load = my_load;
  b_load = my_stats->total_walltime - (my_stats->idletime + my_load);

	pe_no.clear();
	loads.clear();
	distribution.clear();
  lb_started = false;
  gossip_msg_count = 0;
  negack_count = 0;

  total_migrates = 0;
  total_migrates_ack = 0;

  srand((unsigned)(CmiWallTimer()*1.0e06) + CkMyPe());

  // Do a reduction to obtain load information for the system
  CkReduction::tupleElement tupleRedn[] = {
    CkReduction::tupleElement(sizeof(double), &my_load, CkReduction::sum_double),
    CkReduction::tupleElement(sizeof(double), &my_load, CkReduction::max_double)
  };
  CkReductionMsg* msg = CkReductionMsg::buildFromTuple(tupleRedn, 2);
  CkCallback cb(CkIndex_DistributedLB::LoadReduction(NULL), thisProxy);
  msg->setCallback(cb);
  contribute(msg);
}

/*
* Once the reduction callback is obtained for average load in the system, the
* gossiping starts. Only the underloaded processors gossip.
* Termination of gossip is via QD and callback is DoneGossip.
*/
void DistributedLB::LoadReduction(CkReductionMsg* redn_msg) {
  int count;
  CkReduction::tupleElement* results;
  redn_msg->toTuple(&results, &count);
  delete redn_msg;

  // Set the initial global load stats and print when LBDebug is on
  avg_load = *(double*)results[0].data / CkNumPes();
  max_load = *(double*)results[1].data;
  load_ratio = max_load / avg_load;

  if (CkMyPe() == 0 && _lb_args.debug() >= 1) {
    CkPrintf("DistributedLB>>>Before LB: max = %lf, avg = %lf, ratio = %lf\n",
        max_load, avg_load, load_ratio);
  }

  // If there are no overloaded processors, immediately terminate
  if (load_ratio <= kTargetRatio) {
    if (CkMyPe() == 0 && _lb_args.debug() >= 1) {
      CkPrintf("DistributedLB>>>Load ratio already within the target of %lf, ending early.\n",
          kTargetRatio);
    }
    PackAndSendMigrateMsgs();
    delete [] results;
    return;
  }

  // Set transfer threshold for the gossip phase, for which only PEs lower than
  // the target ratio are considered underloaded.
  transfer_threshold = kTargetRatio * avg_load;

  // If my load is under the acceptance threshold, then I am underloaded and
  // can receive more work. So assuming there exists an overloaded PE that can
  // donate work, I will start gossipping my load information.
  if (my_load < transfer_threshold) {
    underloaded = true;
		double r_loads[1];
		int r_pe_no[1];
    r_loads[0] = my_load;
    r_pe_no[0] = CkMyPe();
    GossipLoadInfo(CkMyPe(), 1, r_pe_no, r_loads);
  } else {
    underloaded = false;
  }

  // Start quiescence detection at PE 0.
  if (CkMyPe() == 0) {
    CkCallback cb(CkIndex_DistributedLB::DoneGossip(), thisProxy);
    CkStartQD(cb);
  }
  delete [] results;
}

/*
* Gossip load information between peers. Receive the gossip message.
*/
void DistributedLB::GossipLoadInfo(int from_pe, int n,
    int remote_pe_no[], double remote_loads[]) {
  // Placeholder temp vectors for the sorted pe and their load 
  vector<int> p_no;
  vector<double> l;

  int i = 0;
  int j = 0;
  int m = pe_no.size();

  // Merge (using merge sort) information received with the information at hand
  // Since the initial list is sorted, the merging is linear in the size of the
  // list. 
  while (i < m && j < n) {
    if (pe_no[i] < remote_pe_no[j]) {
      p_no.push_back(pe_no[i]);
      l.push_back(loads[i]);
      i++;
    } else {
      p_no.push_back(remote_pe_no[j]);
      l.push_back(remote_loads[j]);
      if (pe_no[i] == remote_pe_no[j]) {
        i++;
      }
      j++;
    }
  }

  if (i == m && j != n) {
    while (j < n) {
      p_no.push_back(remote_pe_no[j]);
      l.push_back(remote_loads[j]);
      j++;
    }
  } else if (j == n && i != m) {
    while (i < m) {
      p_no.push_back(pe_no[i]);
      l.push_back(loads[i]);
      i++;
    }
  }

  // After the merge sort, swap. Now pe_no and loads have updated information
  pe_no.swap(p_no);
  loads.swap(l);

  SendLoadInfo();
}

/*
* Construct the gossip message and send to peers
*/
void DistributedLB::SendLoadInfo() {
  // TODO: Keep it 0.8*log
  // This PE has already sent the maximum set threshold for gossip messages.
  // Hence don't send out any more messages. This is to prevent flooding.
  if (gossip_msg_count > kMaxGossipMsgCount) {
    return;
  }

  // Pick two random neighbors to send the message to
  int rand_nbor1;
  int rand_nbor2 = -1;
  do {
    rand_nbor1 = rand() % CkNumPes();
  } while (rand_nbor1 == CkMyPe());
  // Pick the second neighbor which is not the same as the first one.
  if(CkNumPes() > 2)
    do {
      rand_nbor2 = rand() % CkNumPes();
    } while ((rand_nbor2 == CkMyPe()) || (rand_nbor2 == rand_nbor1));

  // kPartialInfoCount indicates how much information is send in gossip. If it
  // is set to -1, it means use all the information available.
  int info_count = (kPartialInfoCount >= 0) ? kPartialInfoCount : pe_no.size();
  int* p = new int[info_count];
  double* l = new double[info_count];
  for (int i = 0; i < info_count; i++) {
    p[i] = pe_no[i];
    l[i] = loads[i];
  }

  thisProxy[rand_nbor1].GossipLoadInfo(CkMyPe(), info_count, p, l);

  if(CkNumPes() > 2)
    thisProxy[rand_nbor2].GossipLoadInfo(CkMyPe(), info_count, p, l);

  // Increment the outgoind msg count
  gossip_msg_count++;

  delete[] p;
  delete[] l;
}

/*
* Callback invoked when gossip is done and QD is detected
*/
void DistributedLB::DoneGossip() {
  if (CkMyPe() == 0 && _lb_args.debug() >= 1) {
    double end_time = CmiWallTimer();
    CkPrintf("DistributedLB>>>Gossip finished at %lf (%lf elapsed)\n",
        end_time, end_time - start_time);
  }
  // Set a new transfer threshold for the actual load balancing phase. It starts
  // high so that load is initially only transferred from the most loaded PEs.
  // In subsequent phases it gets relaxed to allow less overloaded PEs to
  // transfer load as well.
  transfer_threshold = fmax(kTargetRatio * avg_load, (max_load + avg_load) / 2);
  lb_started = true;
  underloaded_pe_count = pe_no.size();
  Setup();
  StartNextLBPhase();
}

void DistributedLB::StartNextLBPhase() {
  if (underloaded_pe_count == 0 || my_load <= transfer_threshold || underloaded) {
    // If this PE has no information about underloaded processors, or it has
    // no objects to donate to underloaded processors then do nothing.
    DoneWithLBPhase();
  } else {
    // Otherwise this PE has work to donate, and should attempt to do so.
    LoadBalance();
  }
}

void DistributedLB::DoneWithLBPhase() {
  phase_number++;

  int count = 1;
  if (_lb_args.debug() >= 1) count = 3;
  CkReduction::tupleElement tupleRedn[] = {
    CkReduction::tupleElement(sizeof(double), &my_load, CkReduction::max_double),
    CkReduction::tupleElement(sizeof(double), &total_migrates, CkReduction::sum_int),
    CkReduction::tupleElement(sizeof(double), &negack_count, CkReduction::max_int)
  };
  CkReductionMsg* msg = CkReductionMsg::buildFromTuple(tupleRedn, count);
  CkCallback cb(CkIndex_DistributedLB::AfterLBReduction(NULL), thisProxy);
  msg->setCallback(cb);
  contribute(msg);
}

void DistributedLB::AfterLBReduction(CkReductionMsg* redn_msg) {
  int count, migrations, max_nacks;
  CkReduction::tupleElement* results;
  redn_msg->toTuple(&results, &count);
  delete redn_msg;

  // Update load stats and print if in debug mode
  max_load = *(double*)results[0].data;
  double old_ratio = load_ratio;
  load_ratio = max_load / avg_load;
  if (count > 1) migrations = *(int*)results[1].data;
  if (count > 2) max_nacks = *(int*)results[2].data;

  if (CkMyPe() == 0 && _lb_args.debug() >= 1) {
    CkPrintf("DistributedLB>>>After phase %i: max = %lf, avg = %lf, ratio = %lf\n",
        phase_number, max_load, avg_load, load_ratio);
  }

  // Try more phases as long as our load ratio is still worse than our target,
  // our transfer threshold hasn't decayed below the target, and we haven't hit
  // the maximum number of phases yet.
  if (load_ratio > kTargetRatio &&
      transfer_threshold > kTargetRatio * avg_load &&
      phase_number < kMaxPhases) {
    // Relax the transfer ratio to allow more phases based on whether or not the
    // previous phase was able to reduce the load ratio at all.
    if (std::abs(load_ratio - old_ratio) < 0.01) {
      // The previous phase didn't meaningfully reduce the max load, so relax
      // the transfer threshold.
      transfer_threshold = fmax(kTargetRatio * avg_load,
          (transfer_threshold + avg_load) / 2);
    } else {
      // The previous phase did reduce the max load, so update the transfer
      // threshold based on the new max load.
      transfer_threshold = fmax(kTargetRatio * avg_load,
          (max_load + avg_load) / 2);
    }
    StartNextLBPhase();
  } else {
    if (CkMyPe() == 0 && _lb_args.debug() >= 1) {
      double end_time = CmiWallTimer();
      CkPrintf("DistributedLB>>>Balancing completed at %lf (%lf elapsed)\n",
          end_time, end_time - start_time);
      CkPrintf("DistributedLB>>>%i total migrations with %i negative ack max\n",
          migrations, max_nacks);
    }
    Cleanup();
    PackAndSendMigrateMsgs();
    if (!(_lb_args.metaLbOn() && _lb_args.metaLbModelDir() != nullptr))
      lbmgr->nextLoadbalancer(seqno);
  }
  delete [] results;
}

/*
* Perform load balancing based on the partial information obtained from the
* information propagation stage (gossip).
*/
void DistributedLB::LoadBalance() {
  CkVec<int> obj_no;
  CkVec<int> obj_pe_no;

  // Balance load and add objs to be transferred to obj_no and pe to be
  // transferred to in obj_pe_no
  MapObjsToPe(objs, obj_no, obj_pe_no);
  total_migrates += obj_no.length();
  total_migrates_ack = obj_no.length();

  // If there is no migration, then this is done
  if (obj_no.length() == 0) {
    DoneWithLBPhase();
	}
}

void DistributedLB::Setup() {
  objs_count = 0;
  double avg_objload = 0.0;
  double max_objload = 0.0;
  // Count the number of objs that are migratable and whose load is not 0.
  for(int i=0; i < my_stats->n_objs; i++) {
    if (my_stats->objData[i].migratable &&
      my_stats->objData[i].wallTime > 0.000001) {
      objs_count++;
    }
  }
 
  // Create a min heap of objs. The idea is to transfer smaller objs. The reason
  // is that since we are making probabilistic transfer of load, sending small
  // objs will result in better load balance.
  objs = new minHeap(objs_count);
  for(int i=0; i < my_stats->n_objs; i++) {
    if (my_stats->objData[i].migratable &&
        my_stats->objData[i].wallTime > 0.0001) {
      InfoRecord* item = new InfoRecord;
      item->load = my_stats->objData[i].wallTime;
      item->Id = i;
      objs->insert(item);
    }
  }

  // Calculate the probabilities and cdf for PEs based on their load
  // distribution
	CalculateCumulateDistribution();
}

void DistributedLB::Cleanup() {

  // Delete the object records from the heap
  InfoRecord* obj;
  while (NULL!=(obj=objs->deleteMin())) {
    delete obj;
  }
  delete objs;
}

/*
* Map objects to PE for load balance. It takes in a min heap of objects which
* can be transferred and finds suitable receiver PEs. The mapping is stored in
* obj_no and the corresponding entry in obj_pe_no indicates the receiver PE.
*/
void DistributedLB::MapObjsToPe(minHeap *objs, CkVec<int> &obj_no,
    CkVec<int> &obj_pe_no) {
  int p_id;
  double p_load;
  int rand_pe;

  // While my load is more than the threshold, try to transfer objs
  while (my_load > transfer_threshold) {
    // If there is only one object, then nothing can be done to balance it.
    if (objs_count < 2) break;

    // Flag to indicate whether successful in finding a transfer
    bool success = false;

    // Get the smallest object
    InfoRecord* obj = objs->deleteMin();
    // No more objects to retrieve
    if (obj == 0) break;

    // Pick random PE based on the probability and the find is successful only
    // if on transferring the object, that PE does not become overloaded
    do {
      rand_pe = PickRandReceiverPeIdx();
      if (rand_pe == -1) break;
      p_id = pe_no[rand_pe];
      p_load = loads[rand_pe];
      if ((p_load + obj->load < transfer_threshold) && (CkMyPe() != p_id)) {
        success = true;
      }
      kMaxTrials--;
    } while (!success && (kMaxTrials > 0));

    kMaxObjPickTrials--;

    // No successful in finding a suitable PE to transfer the object
    if (!success) {
      objs->insert(obj);
      break;
    }

    // Found an object and a suitable PE to transfer it to. Decrement the obj
    // count and update the loads.
    obj_no.insertAtEnd(obj->Id);
    obj_pe_no.insertAtEnd(p_id);
    objs_count--;
    loads[rand_pe] += obj->load;
    my_load -= obj->load;

    // Send information to the receiver PE about this obj. This is necessary for
    // ack as well as finding out how many objs are migrating in
		thisProxy[p_id].InformMigration(obj->Id, CkMyPe(),
        my_stats->objData[obj->Id].wallTime, false);

    // This object is assigned, so we delete it from the heap
    delete obj;
  }
}

/*
* Receive information about inbound object including the id, from_pe and its
* load. 
*
* obj_id is the index of the object in the original PE.
* from_pe is the originating PE
* obj_load is the load of this object
* force flag indicates that this PE is forced to accept the object after
* multiple trials and ack should not be sent.
*/
void DistributedLB::InformMigration(int obj_id, int from_pe, double obj_load,
    bool force) {
  // If not using ack based scheme or adding this obj does not make this PE
  // overloaded, then accept the migrated obj and return. 
  if (!kUseAck || my_load + obj_load <= transfer_threshold) {
    migrates_expected++;
    // add to my load and reply true
    my_load += obj_load;
    thisProxy[from_pe].RecvAck(obj_id, CkMyPe(), true);
    return;
  }

  // We are using ack based scheme and turns out accepting this obj will make me
  // overloaded but if it is a forced one, then accept it else return negative
  // acknowledgement.
  if (force) {
    migrates_expected++;
    // add to my load and reply with positive ack
    my_load += obj_load;
  } else {
    // If my_load + obj_load is > threshold, then reply with negative ack 
    //CkPrintf("[%d] Cannot accept obj with load %lf my_load %lf and init_load %lf migrates_expected %d\n", CkMyPe(), obj_load, my_load, init_load, migrates_expected);
    thisProxy[from_pe].RecvAck(obj_id, CkMyPe(), false);
  }
}

/*
* Receive an ack message which the message whether the assigned object can be
* assigned or not. If all the acks have been received, then create migration
* message.
*/
void DistributedLB::RecvAck(int obj_id, int assigned_pe, bool can_accept) {
  total_migrates_ack--;

  // If it is a positive ack, then create a migrate msg for that object
  if (can_accept) {
    MigrateInfo* migrateMe = new MigrateInfo;
    migrateMe->obj = my_stats->objData[obj_id].handle;
    migrateMe->from_pe = CkMyPe();
    migrateMe->to_pe = assigned_pe;
    migrateInfo.push_back(migrateMe);
  } else if (negack_count > 0.01*underloaded_pe_count) {
    // If received negative acks more than the specified threshold, then drop it
    negack_count++;
    total_migrates--;
    objs_count++;
    my_load += my_stats->objData[obj_id].wallTime;
  } else {
    // Try to transfer again. Add the object back to a heap, update my load and
    // try to find a suitable PE now.
    total_migrates--;
    negack_count++;
    objs_count++;
    my_load += my_stats->objData[obj_id].wallTime;

    minHeap* objs = new minHeap(1);
    InfoRecord* item = new InfoRecord;
    item->load = my_stats->objData[obj_id].wallTime;
    item->Id = obj_id;
    objs->insert(item);

    CkVec<int> obj_no;
    CkVec<int> obj_pe_no;
    MapObjsToPe(objs, obj_no, obj_pe_no);

    // If a PE could be found to transfer this object, MapObjsToPe sends a
    // message to it. Wait for the ack.
    // Maybe at this point we can try to force it or just drop it.
    if (obj_pe_no.size() > 0) {
      total_migrates_ack++;
      total_migrates++;
    }
    InfoRecord* obj;
    while (NULL!=(obj=objs->deleteMin())) {
      delete obj;
    }
  }

  // Whenever all the acks have been received, create migration msg, go into the
  // barrier and wait for everyone to finish their load balancing phase
  if (total_migrates_ack == 0) {
    DoneWithLBPhase();
  }
}

void DistributedLB::PackAndSendMigrateMsgs() {
  LBMigrateMsg* msg = new(total_migrates,CkNumPes(),CkNumPes(),0) LBMigrateMsg;
  msg->n_moves = total_migrates;
  for(int i=0; i < total_migrates; i++) {
    MigrateInfo* item = (MigrateInfo*) migrateInfo[i];
    msg->moves[i] = *item;
    delete item;
    migrateInfo[i] = 0;
  }
  migrateInfo.clear();
  ProcessMigrationDecision(msg);
}

/*
* Pick a random PE based on the probability distribution.
*/
int DistributedLB::PickRandReceiverPeIdx() const {
  // The min loaded PEs have probabilities inversely proportional to their load.
  // A cumulative distribution is calculated and a PE is randomly selected based
  // on the cdf.
  // Generate a random number and return the index of min loaded PE whose cdf is
  // greater than the random number.
	double no = (double) rand()/(double) RAND_MAX;
	for (int i = 0; i < underloaded_pe_count; i++) {
		if (distribution[i] >= no) {
			return i;
		}
	}
	return -1;
}

/*
* The PEs have probabilities inversely proportional to their load. Construct a
* CDF based on this.
*/
void DistributedLB::CalculateCumulateDistribution() {
  // The min loaded PEs have probabilities inversely proportional to their load.
	double cumulative = 0.0;
	for (int i = 0; i < underloaded_pe_count; i++) {
		cumulative += (transfer_threshold - loads[i])/transfer_threshold;
		distribution.push_back(cumulative);
	}

  for (int i = 0; i < underloaded_pe_count; i++) {
    distribution[i] = distribution[i]/cumulative;
  }
}

#include "DistributedLB.def.h"
