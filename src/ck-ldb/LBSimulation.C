/* Implementation of the CLBSimResults class
 */
#include "LBSimulation.h"

/*****************************************************************************
		Sequentail Simulation 
*****************************************************************************/

int LBSimulation::dumpStep = -1;  	     /// first step number to dump
int LBSimulation::dumpStepSize = 1;          /// number of steps to dump 
char* LBSimulation::dumpFile = (char*)"lbdata.dat"; /// dump file name
bool LBSimulation::doSimulation = false; 	     /// flag if do simulation
int LBSimulation::simStep = -1;              /// first step number to simulate
int LBSimulation::simStepSize = 1;           /// number of steps to simulate
int LBSimulation::simProcs = 0; 	     /// simulation target procs
bool LBSimulation::procsChanged = false;          /// flag if the number of procs has been changed

bool LBSimulation::showDecisionsOnly = false;     /// flag to write all LB decisions
int _lb_version = LB_FORMAT_VERSION;	     /// data file version

/*****************************************************************************
		LBInfo: evaluation information for LBStats  
*****************************************************************************/

LBInfo::LBInfo(int count): numPes(count), minObjLoad(0.0), maxObjLoad(0.0)
{
  peLoads = new LBRealType[numPes]; 
  objLoads = new LBRealType[numPes]; 
  comLoads = new LBRealType[numPes]; 
  bgLoads = new LBRealType[numPes]; 
  clear();
}

LBInfo::~LBInfo()
{
  // only free when it is allocated in the constructor
  if (peLoads && bgLoads) {
    delete [] bgLoads;
    delete [] comLoads;
    delete [] objLoads;
    delete [] peLoads;
  }
}

void LBInfo::clear()
{
  for (int i=0; i<numPes; i++) {
    peLoads[i] = 0.0;
    if (objLoads) objLoads[i] = 0.0;
    if (comLoads) comLoads[i] = 0.0;
    if (bgLoads)  bgLoads[i] = 0.0;
  }
  minObjLoad = 0.0;
  maxObjLoad = 0.0;
  msgCount = msgBytes = 0;
}

void LBInfo::getInfo(BaseLB::LDStats* stats, int count, int considerComm)
{
#if CMK_LBDB_ON
	int i, pe;

	CmiAssert(peLoads);

	clear();

	double alpha = _lb_args.alpha();
	double beta = _lb_args.beta();

        minObjLoad = 1.0e20;  // I suppose no object load is beyond this
	maxObjLoad = 0.0;

    	msgCount = 0;
	msgBytes = 0;

	if (considerComm) stats->makeCommHash();

        // get background load
	if (bgLoads)
    	  for(pe = 0; pe < count; pe++)
    	   bgLoads[pe] = stats->procs[pe].bg_walltime;

	for(pe = 0; pe < count; pe++)
    	  peLoads[pe] = stats->procs[pe].bg_walltime;

    	for(int obj = 0; obj < stats->objData.size(); obj++)
    	{
		int pe = stats->to_proc[obj];
		if (pe == -1) continue;     // this object is out
		CmiAssert(pe >=0 && pe < count);
		double oload = stats->objData[obj].wallTime;
		if (oload < minObjLoad) minObjLoad = oload;
		if (oload > maxObjLoad) maxObjLoad = oload;
		peLoads[pe] += oload;
		if (objLoads) objLoads[pe] += oload;
	}

	// handling of the communication overheads. 
	if (considerComm) {
	  int* msgSentCount = new int[count]; // # of messages sent by each PE
	  int* msgRecvCount = new int[count]; // # of messages received by each PE
	  int* byteSentCount = new int[count];// # of bytes sent by each PE
	  int* byteRecvCount = new int[count];// # of bytes reeived by each PE
	  for(i = 0; i < count; i++)
	    msgSentCount[i] = msgRecvCount[i] = byteSentCount[i] = byteRecvCount[i] = 0;

	  int mcast_count = 0;
          for (auto& cdata : stats->commData) {
	    int senderPE, receiverPE;
	    if (cdata.from_proc())
	      senderPE = cdata.src_proc;
  	    else {
	      int idx = stats->getHash(cdata.sender);
	      if (idx == -1) continue;    // sender has just migrated?
	      senderPE = stats->to_proc[idx];
	      CmiAssert(senderPE != -1);
	    }
	    CmiAssert(senderPE < count && senderPE >= 0);

            // find receiver: point-to-point and multicast two cases
	    int receiver_type = cdata.receiver.get_type();
	    if (receiver_type == LD_PROC_MSG || receiver_type == LD_OBJ_MSG) {
              if (receiver_type == LD_PROC_MSG)
	        receiverPE = cdata.receiver.proc();
              else  {  // LD_OBJ_MSG
	        int idx = stats->getHash(cdata.receiver.get_destObj());
	        if (idx == -1) continue;    // receiver has just been removed?
	        receiverPE = stats->to_proc[idx];
	        CmiAssert(receiverPE != -1);
              }
              CmiAssert(receiverPE < count && receiverPE >= 0);
	      if(senderPE != receiverPE)
	      {
	  	msgSentCount[senderPE] += cdata.messages;
		byteSentCount[senderPE] += cdata.bytes;
		msgRecvCount[receiverPE] += cdata.messages;
		byteRecvCount[receiverPE] += cdata.bytes;
	      }
	    }
            else if (receiver_type == LD_OBJLIST_MSG) {
              int nobjs;
              const LDObjKey *objs = cdata.receiver.get_destObjs(nobjs);
	      mcast_count ++;
	      std::vector<int> pes;
	      for (i=0; i<nobjs; i++) {
	        int idx = stats->getHash(objs[i]);
		CmiAssert(idx != -1);
	        if (idx == -1) continue;    // receiver has just been removed?
	        receiverPE = stats->to_proc[idx];
		CmiAssert(receiverPE < count && receiverPE >= 0);
		int exist = 0;
	        for (auto pe : pes)
		  if (receiverPE == pe) { exist=1; break; }
		if (exist) continue;
		pes.push_back(receiverPE);
	        if(senderPE != receiverPE)
	        {
	  	msgSentCount[senderPE] += cdata.messages;
		byteSentCount[senderPE] += cdata.bytes;
		msgRecvCount[receiverPE] += cdata.messages;
		byteRecvCount[receiverPE] += cdata.bytes;
	        }
              }
	    }
	  }   // end of for
          if (_lb_args.debug())
             CkPrintf("Number of MULTICAST: %d\n", mcast_count);

	  // now for each processor, add to its load the send and receive overheads
	  for(i = 0; i < count; i++)
	  {
		double comload = msgRecvCount[i]  * PER_MESSAGE_RECV_OVERHEAD +
			      msgSentCount[i]  * alpha +
			      byteRecvCount[i] * PER_BYTE_RECV_OVERHEAD +
			      byteSentCount[i] * beta;
		peLoads[i] += comload;
		if (comLoads) comLoads[i] += comload;
		msgCount += msgRecvCount[i] + msgSentCount[i];
		msgBytes += byteRecvCount[i] + byteSentCount[i];
	  }
	  delete [] msgRecvCount;
	  delete [] msgSentCount;
	  delete [] byteRecvCount;
	  delete [] byteSentCount;
	}
#endif
}

void LBInfo::print()
{
  int i;  
  double minLoad, maxLoad, maxProcObjLoad, avgProcObjLoad, maxComLoad, sum, average, avgComLoad;
  double avgBgLoad;
  int max_loaded_proc = 0;
  sum = .0;
  sum = minLoad = maxLoad = peLoads[0];
  avgProcObjLoad = maxProcObjLoad = objLoads[0];
  avgComLoad = maxComLoad = comLoads[0];
  avgBgLoad = bgLoads[0];
  for (i = 1; i < numPes; i++) {
    double load = peLoads[i];
    if (load>maxLoad) {
      maxLoad=load;
      max_loaded_proc = i;
    } else if (peLoads[i]<minLoad) minLoad=load;
    if (objLoads[i]>maxProcObjLoad) maxProcObjLoad = objLoads[i];
    if (comLoads[i]>maxComLoad) maxComLoad = comLoads[i];
    sum += load;
    avgProcObjLoad += objLoads[i];
    avgBgLoad += bgLoads[i];
    avgComLoad += comLoads[i];
  }
  average = sum/numPes;
  avgProcObjLoad /= numPes; 
  avgBgLoad /= numPes; 
  avgComLoad /= numPes;
  CmiPrintf("The processor loads are: \n");
  CmiPrintf("PE   (Total Load) (Obj Load) (Comm Load) (BG Load)\n");
  if (_lb_args.debug() > 3)
    for(i = 0; i < numPes; i++)
      CmiPrintf("%-4d %10f %10f %10f %10f\n", i, peLoads[i], objLoads[i], comLoads[i], bgLoads[i]);
  CmiPrintf("max: %10f %10f %10f\n", maxLoad, maxProcObjLoad, maxComLoad);
  CmiPrintf("Min : %f Max : %f  Average: %f AvgBgLoad: %f\n", minLoad, maxLoad, average, avgBgLoad);
  CmiPrintf("ProcObjLoad  Max : %f  Average: %f\n", maxProcObjLoad, avgProcObjLoad);
  CmiPrintf("CommLoad  Max : %f  Average: %f\n", maxComLoad, avgComLoad);
  CmiPrintf("[%d] is Maxloaded maxload: %f ObjLoad %f BgLoad %f\n",
			max_loaded_proc, peLoads[max_loaded_proc], objLoads[max_loaded_proc], bgLoads[max_loaded_proc]);
  // the min and max object (calculated in getLoadInfo)
  CmiPrintf("MinObj : %f  MaxObj : %f  Average : %f\n", minObjLoad, maxObjLoad, average);
  CmiPrintf("Non-local comm: %d msgs %" PRIu64 " bytes\n", msgCount, msgBytes);
}

void LBInfo::getSummary(LBRealType &maxLoad, LBRealType &maxCpuLoad, LBRealType &totalLoad)
{
  totalLoad = maxLoad = peLoads[0];
  maxCpuLoad = objLoads[0];
  for (int i = 1; i < numPes; i++) {
    LBRealType load = peLoads[i];
    if (load>maxLoad) maxLoad=load;
    LBRealType cpuload = objLoads[i];
    if (cpuload>maxCpuLoad) maxCpuLoad=cpuload;
    totalLoad += load;
  }
}

////////////////////////////////////////////////////////////////////////////

LBSimulation::LBSimulation(int numPes_) : lbinfo(numPes_), numPes(numPes_)
{
}

LBSimulation::~LBSimulation()
{
}

void LBSimulation::reset()
{
  lbinfo.clear();
}

void LBSimulation::SetProcessorLoad(int pe, double load, double bgload)
{
	CkAssert(0 <= pe && pe < numPes);
	lbinfo.peLoads[pe] = load;
	lbinfo.bgLoads[pe] = bgload;
}

void LBSimulation::PrintSimulationResults()
{
  lbinfo.print();
}

void LBSimulation::PrintDecisions(LBMigrateMsg *m, char *simFileName,
				  int peCount)
{
  int len = (strlen(simFileName) + strlen("results") + 2)*sizeof(char);
  char *resultFile = (char *)malloc(len);
  snprintf(resultFile, len, "%s.results", simFileName);
  FILE *f = fopen(resultFile, "w");
  fprintf(f, "%d %d\n", peCount, m->n_moves); // header
  for (int i=0; i<m->n_moves; i++) {
    fprintf(f, "%" PRIu64 " ", m->moves[i].obj.id);
    if (m->moves[i].kind == LB_DECISION_CLONE)
      fprintf(f, "%d clone %d\n", m->moves[i].to_pe, m->moves[i].rank);
    else if (m->moves[i].kind == LB_DECISION_RETIRE)
      fprintf(f, "%d retire\n", m->moves[i].from_pe);
    else
      fprintf(f, "%d\n",m->moves[i].to_pe);
  }
  fclose(f);
}

void LBSimulation::PrintDifferences(LBSimulation *realSim, BaseLB::LDStats *stats)
{
  LBRealType *peLoads = lbinfo.peLoads;
  LBRealType *realPeLoads = realSim->lbinfo.peLoads;

  // the number of procs during the simulation and the real execution must be checked by the caller!
  int i;
  // here to print the differences between the predicted (this) and the real (real)
  CmiPrintf("Differences between predicted and real balance:\n");
  CmiPrintf("PE   (Predicted Load) (Real Predicted)  (Difference)  (Real CPU)  (Prediction Error)\n");
  for(i = 0; i < numPes; ++i) {
    CmiPrintf("%-4d %13f %16f %15f %12f %14f\n", i, peLoads[i], realPeLoads[i], peLoads[i]-realPeLoads[i],
	      stats->procs[i].total_walltime-stats->procs[i].idletime, realPeLoads[i]-(stats->procs[i].total_walltime-stats->procs[i].idletime));
  }
}
