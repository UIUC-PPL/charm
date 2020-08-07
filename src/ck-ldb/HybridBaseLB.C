/**
 * \addtogroup CkLdb
*/
/*@{*/

#include "HybridBaseLB.h"

#define  DEBUGF(x)     // CmiPrintf x;

extern int quietModeRequested;

CreateLBFunc_Def(HybridBaseLB, "HybridBase load balancer")

class DummyMsg: public CMessage_DummyMsg 
{
};

HybridBaseLB::HybridBaseLB(const CkLBOptions &opt): CBase_HybridBaseLB(opt)
{
#if CMK_LBDB_ON
  lbname = (char *)"HybridBaseLB";
  thisProxy = CProxy_HybridBaseLB(thisgroup);

  statsStrategy = FULL;

  // defines topology
  if (CkNumPes() <= 4)  {
    tree = new TwoLevelTree;
  }
  else {
    tree = new ThreeLevelTree;
    if (CkNumPes() >= 4096) statsStrategy = SHRINK;
    //statsStrategy = SHRINK;

  }
  //tree = new FourLevelTree;
  if (CkMyPe() == 0 && !quietModeRequested)
    CkPrintf("CharmLB> %s: %s is created.\n", lbname, tree->name());

  // decide which load balancer to call
//  greedy = (CentralLB *)AllocateGreedyLB();
//  refine = (CentralLB *)AllocateRefineLB();

  currentLevel = 0;
  foundNeighbors = 0;
  future_migrates_expected = -1;

  vector_n_moves = 0;

  maxLoad = 0.0;
  maxCpuLoad = 0.0;
  totalLoad = 0.0;
  maxCommCount = 0;
  maxCommBytes = 0.0;
  maxMem = 0.0;

  if (_lb_args.statsOn()) lbmgr->CollectStatsOn();

  group1_created = 0;             // base class need to call initTree()
#endif
}

void HybridBaseLB::initTree()
{
#if CMK_LBDB_ON
    // create a multicast group to optimize level 1 multicast
  if (tree->isroot(CkMyPe(), 1)) {
    int npes = tree->numChildren(CkMyPe(), 1);
    if (npes >= 128) {                          // only when the group is big
      int *pes = new int[npes];
      tree->getChildren(CkMyPe(), 1, pes, npes);
      group1 = CmiEstablishGroup(npes, pes);
      group1_created = 1;
      delete [] pes;
    }
  }
#endif
}

HybridBaseLB::~HybridBaseLB()
{
#if CMK_LBDB_ON
  delete tree;
#endif
}

// get tree information
void HybridBaseLB::FindNeighbors()
{
  if (foundNeighbors == 0) { // Neighbors never initialized, so init them
                           // and other things that depend on the number
                           // of neighbors

    int nlevels = tree->numLevels();
    int mype = CkMyPe();
    for (int level=0; level<nlevels; level++) 
    {
      LevelData *data = new LevelData;
      data->parent = tree->parent(mype, level);
      if (tree->isroot(mype, level)) {
        data->nChildren = tree->numChildren(mype, level);
        data->children = new int[data->nChildren];
        tree->getChildren(mype, level, data->children, data->nChildren);
        data->statsMsgsList = new CLBStatsMsg*[data->nChildren];
        for(int i=0; i < data->nChildren; i++)
           data->statsMsgsList[i] = NULL;
        data->statsData = new LDStats(data->nChildren+1, 0);  // incomplete
        //  a fake processor
        ProcStats &procStat = data->statsData->procs[data->nChildren];
        procStat.available = false;
      }
      levelData.push_back(data);
      DEBUGF(("[%d] level: %d nchildren:%d - %d %d\n", CkMyPe(), level, data->nChildren, data->nChildren>0?data->children[0]:-1, data->nChildren>1?data->children[1]:-1));
    }
    
    foundNeighbors = 1;
  }   // end if
}

void HybridBaseLB::InvokeLB()
{
#if CMK_MEM_CHECKPOINT
  CkSetInLdb();
#endif

#if CMK_LBDB_ON
  //  CkPrintf("[%d] HybridBaseLB At Sync step %d!!!!\n",CkMyPe(),mystep);

  FindNeighbors();

  // if num of processor is only 1, nothing should happen
  if (!QueryBalanceNow(step()) || CkNumPes() == 1) {
    MigrationDone(0);
    return;
  }

  thisProxy[CkMyPe()].ProcessAtSync();
#endif
}

void HybridBaseLB::ProcessAtSync()
{
#if CMK_LBDB_ON
  start_lb_time = 0;

  if (CkMyPe() == 0) {
    start_lb_time = CkWallTimer();
    if (_lb_args.debug())
      CkPrintf("[%s] Load balancing step %d starting at %f\n",
	       lbName(), step(), CkWallTimer());
  }

  // assemble LB database
  CLBStatsMsg* msg = AssembleStats();

  CkMarshalledCLBStatsMessage marshmsg(msg);
  // send to parent
  thisProxy[levelData[0]->parent].ReceiveStats(marshmsg, 0);

  DEBUGF(("[%d] Send stats to myself\n", CkMyPe()));
#endif
}

// only called on leaves
CLBStatsMsg* HybridBaseLB::AssembleStats()
{
#if CMK_LBDB_ON
  // build and send stats
  const int osz = lbmgr->GetObjDataSz();
  const int csz = lbmgr->GetCommDataSz();

  CLBStatsMsg* msg = new CLBStatsMsg(osz, csz);
  msg->from_pe = CkMyPe();

  // Get stats
#if CMK_LB_CPUTIMER
  lbmgr->GetTime(&msg->total_walltime,&msg->total_cputime,
                   &msg->idletime, &msg->bg_walltime,&msg->bg_cputime);
#else
  lbmgr->GetTime(&msg->total_walltime,&msg->total_walltime,
                   &msg->idletime, &msg->bg_walltime,&msg->bg_walltime);
#endif
//  msg->pe_speed = myspeed;
  // number of pes
  msg->pe_speed = 1;

  msg->n_objs = osz;
  lbmgr->GetObjData(msg->objData);
  msg->n_comm = csz;
  lbmgr->GetCommData(msg->commData);

  return msg;
#else
  return NULL;
#endif
}

void HybridBaseLB::ReceiveStats(CkMarshalledCLBStatsMessage &&data, int fromlevel)
{
#if CMK_LBDB_ON
  FindNeighbors();

  // store the message
  CLBStatsMsg *m = data.getMessage();
  int atlevel = fromlevel + 1;
  CmiAssert(tree->isroot(CkMyPe(), atlevel));

  depositLBStatsMessage(m, atlevel);

  int &stats_msg_count = levelData[atlevel]->stats_msg_count;
  stats_msg_count ++;

  DEBUGF(("[%d] ReceiveStats at level: %d %d/%d\n", CkMyPe(), atlevel, stats_msg_count, levelData[atlevel]->nChildren));
  if (stats_msg_count == levelData[atlevel]->nChildren)  
  {
    // build LDStats
    buildStats(atlevel);
    stats_msg_count = 0;
    int parent = levelData[atlevel]->parent;
    if (parent != -1) {
      // combine and shrink message
      // build a new message based on our LDStats
      CLBStatsMsg* cmsg = buildCombinedLBStatsMessage(atlevel);

      // send to parent
      CkMarshalledCLBStatsMessage marshmsg(cmsg);
      thisProxy[parent].ReceiveStats(marshmsg, atlevel);
    }
    else {
      // root of all processors, calls top-level strategy (refine)
      thisProxy[CkMyPe()].Loadbalancing(atlevel);
    }
  }

#endif  
}

// store stats message in a level data
void HybridBaseLB::depositLBStatsMessage(CLBStatsMsg *m, int atlevel)
{
  int pe = m->from_pe;
  int neighborIdx = NeighborIndex(pe, atlevel);

  CLBStatsMsg **statsMsgsList = levelData[atlevel]->statsMsgsList;
  LDStats *statsData = levelData[atlevel]->statsData;
  CmiAssert(statsMsgsList && statsData);

  if (statsMsgsList[neighborIdx] != 0) {
    CkPrintf("*** Unexpected CLBStatsMsg in ReceiveStats from PE %d-%d ***\n", pe,neighborIdx);
    CkAbort("HybridBaseLB> Abort!");
  }

  // replace real pe to relative pe number in preparation for calling Strategy()
  for (int i=0; i<m->n_comm; i++) {
     LDCommData &commData = m->commData[i];
     // modify processor to be this local pe
     if (commData.from_proc()) m->commData[i].src_proc = neighborIdx;
     if (commData.receiver.get_type() == LD_PROC_MSG) m->commData[i].receiver.setProc(neighborIdx);
  }

  statsMsgsList[neighborIdx] = m;
      // store per processor data right away
  struct ProcStats &procStat = statsData->procs[neighborIdx];
  procStat.pe = pe;	// real PE
  procStat.total_walltime = m->total_walltime;
  procStat.idletime = m->idletime;
  procStat.bg_walltime = m->bg_walltime;
#if CMK_LB_CPUTIMER
  procStat.total_cputime = m->total_cputime;
  procStat.bg_cputime = m->bg_cputime;
#endif
  procStat.pe_speed = m->pe_speed;		// important
  procStat.available = true;
  procStat.n_objs = m->n_objs;

  statsData->n_objs += m->n_objs;
  statsData->n_comm += m->n_comm;
}

// assmebly all stats messages from children
void HybridBaseLB::buildStats(int atlevel)
{
#if CMK_LBDB_ON
  // build LDStats
  LevelData *lData = levelData[atlevel];
  LDStats *statsData = lData->statsData;
  CLBStatsMsg **statsMsgsList = lData->statsMsgsList;
  int stats_msg_count = lData->stats_msg_count;

  // statsMsgsList
  DEBUGF(("[%d] buildStats for %d nobj:%d\n", CkMyPe(), stats_msg_count, statsData->n_objs));
  statsData->nprocs() = stats_msg_count;
  statsData->objData.resize(statsData->n_objs);
  statsData->commData.resize(statsData->n_comm);
  statsData->from_proc.resize(statsData->n_objs);
  statsData->to_proc.resize(statsData->n_objs);
  int nobj = 0;
  int nmigobj = 0;
  int ncom = 0;
  for (int n=0; n<stats_msg_count; n++) {
     int i;
     CLBStatsMsg *msg = statsMsgsList[n];
     int pe = msg->from_pe;
     for (i=0; i<msg->n_objs; i++) {
         // need to map index to relative index
         statsData->from_proc[nobj] = statsData->to_proc[nobj] = NeighborIndex(pe, atlevel);
         statsData->objData[nobj] = msg->objData[i];
         if (msg->objData[i].migratable) nmigobj++;
         nobj++;
     }
     for (i=0; i<msg->n_comm; i++) {
         statsData->commData[ncom] = msg->commData[i];
         ncom++;
     }
     // free the message
     delete msg;
     statsMsgsList[n]=0;
  }
  if (_lb_args.debug()>1) {
      CmiPrintf("[%d] n_obj:%d migratable:%d ncom:%d at level %d at %f.\n", CkMyPe(), nobj, nmigobj, ncom, atlevel, CkWallTimer());
  }
  CmiAssert(statsData->n_objs == nobj);
  CmiAssert(statsData->n_comm == ncom);
  statsData->n_migrateobjs = nmigobj;
#endif
}

// build a message based on our LDStats for sending to parent
// shrink if neccessary
CLBStatsMsg * HybridBaseLB::buildCombinedLBStatsMessage(int atlevel)
{
#if CMK_LBDB_ON
  int i;
  double obj_walltime, obj_nmwalltime;
#if CMK_LB_CPUTIMER 
  double obj_cputime, obj_nmcputime;
#endif

  LDStats *statsData = levelData[atlevel]->statsData;
  CmiAssert(statsData);

  CLBStatsMsg* cmsg;

  int osz = statsData->n_objs;
  int csz = statsData->n_comm;

  int shrink = 0;
  if ((statsStrategy == SHRINK || statsStrategy == SHRINK_NULL) && atlevel == tree->numLevels()-2) 
  {
    shrink = 1;
    obj_walltime = obj_nmwalltime = 0.0;
#if CMK_LB_CPUTIMER
    obj_cputime = obj_nmcputime = 0.0;
#endif
    for (i=0; i<osz; i++)  {
      if (statsData->objData[i].migratable) {
        obj_walltime += statsData->objData[i].wallTime;
#if CMK_LB_CPUTIMER
        obj_cputime += statsData->objData[i].cpuTime;
#endif
      }
      else {
        obj_nmwalltime += statsData->objData[i].wallTime;
#if CMK_LB_CPUTIMER
        obj_nmcputime += statsData->objData[i].cpuTime;
#endif
      }
    }
      // skip obj and comm data
    osz = csz = 0;
  }

  cmsg = new CLBStatsMsg(osz, csz);
  int mype = CkMyPe();
  cmsg->from_pe = mype;	// real PE

  // Get stats
  cmsg->pe_speed = 0;
  cmsg->total_walltime = 0.0;
  cmsg->idletime = 0.0;
  cmsg->bg_walltime = 0.0;
#if CMK_LB_CPUTIMER
  cmsg->total_cputime = 0.0;
  cmsg->bg_cputime = 0.0;
#endif

  for (int pe=0; pe<statsData->nprocs(); pe++) {
        struct ProcStats &procStat = statsData->procs[pe];
        cmsg->pe_speed += procStat.pe_speed;		// important
        cmsg->total_walltime += procStat.total_walltime;
        cmsg->idletime += procStat.idletime;
        cmsg->bg_walltime += procStat.bg_walltime;
#if CMK_LB_CPUTIMER
        cmsg->total_cputime += procStat.total_cputime;
        cmsg->bg_cputime += procStat.bg_cputime;
#endif
  }
/*
  cmsg->idletime = 0.0;
  cmsg->bg_walltime = 0.0;
  cmsg->bg_cputime = 0.0;
*/

  // copy obj data
  cmsg->n_objs = osz;
  for (i=0; i<osz; i++)  {
     cmsg->objData[i] = statsData->objData[i];
  }
  // copy comm data
  cmsg->n_comm = csz;
  for (i=0; i<csz; i++) {
     LDCommData &commData = statsData->commData[i];
     cmsg->commData[i] = commData;
     // modify processor to be this real pe
     if (commData.from_proc()) cmsg->commData[i].src_proc = mype;
     if (commData.receiver.get_type() == LD_PROC_MSG) cmsg->commData[i].receiver.setProc(mype);
  }

  if (shrink) {
    cmsg->total_walltime = obj_walltime;
    cmsg->bg_walltime += obj_nmwalltime;
#if CMK_LB_CPUTIMER
    cmsg->total_cputime = obj_cputime;
    cmsg->bg_cputime += obj_nmcputime;
#endif
  }

  return cmsg;
#else
  return NULL;
#endif
}

//  LDStats data sent to parent contains real PE
//  LDStats in parent should contain relative PE
void HybridBaseLB::Loadbalancing(int atlevel)
{
  int i;

  CmiAssert(atlevel >= 1);
  CmiAssert(tree->isroot(CkMyPe(), atlevel));

  LevelData *lData = levelData[atlevel];
  LDStats *statsData = lData->statsData;
  CmiAssert(statsData);

  // at this time, all objects processor location is relative, and 
  // all incoming objects from outside group belongs to the fake root proc.

  // clear background load if needed
  if (_lb_args.ignoreBgLoad()) statsData->clearBgLoad();

  currentLevel = atlevel;
  int nclients = lData->nChildren;

  DEBUGF(("[%d] Calling Strategy ... \n", CkMyPe()));
  double start_lb_time, strat_end_time;
  start_lb_time = CkWallTimer();

  if ((statsStrategy == SHRINK || statsStrategy == SHRINK_NULL) && atlevel == tree->numLevels()-1) {
    // no obj and comm data
    LBVectorMigrateMsg* migrateMsg = VectorStrategy(statsData);
    strat_end_time = CkWallTimer();

    // send to children 
    thisProxy.ReceiveVectorMigration(migrateMsg, nclients, lData->children);
  }
  else {
    LBMigrateMsg* migrateMsg = Strategy(statsData);
    strat_end_time = CkWallTimer();

    // send to children 
    //CmiPrintf("[%d] level: %d nclients:%d children: %d %d\n", CkMyPe(), atlevel, nclients, lData->children[0], lData->children[1]);
    if (!group1_created)
      thisProxy.ReceiveMigration(migrateMsg, nclients, lData->children);
    else {
        // send in multicast tree
      thisProxy.ReceiveMigration(migrateMsg, group1);
      //CkSendMsgBranchGroup(CkIndex_HybridBaseLB::ReceiveMigration(NULL),  migrateMsg, thisgroup, group1);
    }
    // CkPrintf("[%d] ReceiveMigration takes %f \n", CkMyPe(), CkWallTimer()-strat_end_time);
  }

  if (_lb_args.debug()>0){
    CkPrintf("[%d] Loadbalancing Level %d (%d children) started at %f, elapsed time %f\n", CkMyPe(), atlevel, lData->nChildren, start_lb_time, strat_end_time-start_lb_time);
    if (atlevel == tree->numLevels()-1) {
    	CkPrintf("[%d] %s memUsage: %.2fKB\n", CkMyPe(), lbName(), (1.0*useMem())/1024);
    }
  }

  // inform new objects that are from outside group
  if (atlevel < tree->numLevels()-1) {
    for (i=0; i<statsData->n_objs; i++) {
      CmiAssert(statsData->from_proc[i] != -1);   // ???
      if (statsData->from_proc[i] == nclients)  {    // from outside
        CmiAssert(statsData->to_proc[i] < nclients);
        int tope = lData->children[statsData->to_proc[i]];
        // comm data
        CkVec<LDCommData> comms;
//        collectCommData(i, comms, atlevel);
        thisProxy[tope].ObjMigrated(statsData->objData[i], comms.getVec(), comms.size(), atlevel-1);
      }
    }
  }
}

LBMigrateMsg* HybridBaseLB::Strategy(LDStats* stats)
{
#if CMK_LBDB_ON
  work(stats);

  if (_lb_args.debug()>2)  {
    CkPrintf("Obj Map:\n");
    for (int i=0; i<stats->n_objs; i++) CkPrintf("%d ", stats->to_proc[i]);
    CkPrintf("\n");
  }

  return createMigrateMsg(stats);
#else
  return NULL;
#endif
}

// migrate only object LDStat in group
// leaf nodes actually migrate objects
void HybridBaseLB::ReceiveMigration(LBMigrateMsg *msg)
{
#if CMK_LBDB_ON
#if CMK_MEM_CHECKPOINT
  CkResetInLdb();
#endif
  FindNeighbors();

  int atlevel = msg->level - 1;

  DEBUGF(("[%d] ReceiveMigration\n", CkMyPe()));

  LevelData *lData = levelData[atlevel];

  // only non NULL when level > 0
  LDStats *statsData = lData->statsData;

  // do LDStats migration
  const int me = CkMyPe();
  lData->migrates_expected = 0;
  for(int i=0; i < msg->n_moves; i++) {
    MigrateInfo& move = msg->moves[i];
    // incoming
    if (move.from_pe != me && move.to_pe == me) {
      // I can not be the parent node
      DEBUGF(("[%d] expecting LDStats object from %d\n",me,move.from_pe));
      // will receive a ObjData message
      lData->migrates_expected ++;
    }
    else if (move.from_pe == me) {   // outgoing
      if (statsData) {		// this is inner node
        // send objdata
        int obj;
        int found = 0;
        for (obj = 0; obj<statsData->n_objs; obj++) {
          if (move.obj.omID() == statsData->objData[obj].handle.omID() && 
            move.obj.objID() == statsData->objData[obj].handle.objID())
          {
            DEBUGF(("[%d] level: %d sending objData %d to %d. \n", CkMyPe(), atlevel, obj, move.to_pe));
	    found = 1;
            // TODO send comm data
            CkVec<LDCommData> comms;
            collectCommData(obj, comms, atlevel);
	    if (move.to_pe != -1) {
              // this object migrates to another PE of the parent domain
              thisProxy[move.to_pe].ObjMigrated(statsData->objData[obj], comms.getVec(), comms.size(), atlevel);
            }
            lData->outObjs.push_back(MigrationRecord(move.obj, lData->children[statsData->from_proc[obj]], -1));
            statsData->removeObject(obj);
            break;
          }
        }
        CmiAssert(found == 1);
      }
      else {		// this is leave node
        if (move.to_pe == -1) {
          lData->outObjs.push_back(MigrationRecord(move.obj, CkMyPe(), -1));
        }
        else {
          // migrate the object
          lbmgr->Migrate(move.obj,move.to_pe);
        }
      }
    }   // end if
  }

  if (lData->migrationDone())
    StatsDone(atlevel);
#endif
}

extern LBVectorMigrateMsg * VectorStrategy(BaseLB::LDStats *stats);

LBVectorMigrateMsg* HybridBaseLB::VectorStrategy(LDStats* stats)
{
#if CMK_LBDB_ON
  LBVectorMigrateMsg* msg;
  if (statsStrategy == SHRINK_NULL) {
    //msg = new(0,0) LBVectorMigrateMsg;
    msg = new LBVectorMigrateMsg;
    msg->n_moves = 0;
    msg->level = currentLevel;
  }
  else {
    msg = ::VectorStrategy(stats);
    msg->level = currentLevel;

    // translate pe number
    LevelData *lData = levelData[currentLevel];
    for(int i=0; i < msg->n_moves; i++) {
      VectorMigrateInfo* move = &msg->moves[i];
      move->from_pe = lData->children[move->from_pe];
      move->to_pe = lData->children[move->to_pe];
    }
  }
  return msg;
#else
  return NULL;
#endif
}

void HybridBaseLB::ReceiveVectorMigration(LBVectorMigrateMsg *msg)
{
#if CMK_LBDB_ON
  FindNeighbors();

  int atlevel = msg->level - 1;

  DEBUGF(("[%d] ReceiveMigration\n", CkMyPe()));

  LevelData *lData = levelData[atlevel];
  LDStats *statsData = lData->statsData;

  // pick objects for required load migration, first fit
  lData->vector_expected  = 0;
  for (int i=0; i<msg->n_moves; i++)  {
    VectorMigrateInfo &move = msg->moves[i];
    CkVec<LDObjData> objs;
    CkVec<LDCommData> comms;
    if (move.from_pe == CkMyPe()) {
      int toPe = move.to_pe;
      double load = move.load;

      GetObjsToMigrate(toPe, load, statsData, atlevel, comms, objs);
      int count = objs.size();

      if (_lb_args.debug()>1)
        CkPrintf("[%d] sending %d objects to %d at %f.\n", CkMyPe(), count, toPe, CkWallTimer());
      if (objs.size() > 0)
        thisProxy[toPe].ObjsMigrated(objs, objs.size(), comms.getVec(), comms.size(), atlevel);
      thisProxy[toPe].TotalObjMigrated(count, atlevel);
    }
    else if (move.to_pe == CkMyPe()) {
      // expecting objects
      lData->vector_expected ++;
    }
  }

  if (_lb_args.debug()>1)
    CkPrintf("[%d] expecting %d vectors. \n", CkMyPe(), lData->vector_expected);
  if (lData->vectorReceived()) {
    VectorDone(atlevel);
    if (lData->migrationDone())
      StatsDone(atlevel);
  }

  delete msg;
#endif
}

void HybridBaseLB::GetObjsToMigrate(int toPe, double load, LDStats *stats, int atlevel,
    CkVec<LDCommData>& comms, CkVec<LDObjData>& objs) {
  // TODO: sort max => low
  for (int obj=stats->n_objs-1; obj>=0; obj--) {
    LDObjData &objData = stats->objData[obj];
    if (!objData.migratable) continue;
    if (objData.wallTime <= load) {
      if (_lb_args.debug()>2) {
        CkPrintf("[%d] send obj: %d to PE %d (load: %f).\n", CkMyPe(), obj, toPe,
          objData.wallTime);
      }
      objs.push_back(objData);
      // send comm data
      collectCommData(obj, comms, atlevel);
      load -= objData.wallTime;
      CreateMigrationOutObjs(atlevel, stats, obj);
      stats->removeObject(obj);
      if (load <= 0.0) break;
    }
  }
}

void HybridBaseLB::CreateMigrationOutObjs(int atlevel, LDStats* stats,
    int objidx) {
  LDObjData& objData = stats->objData[objidx];
  LevelData *lData = levelData[atlevel];
  lData->outObjs.push_back(MigrationRecord(objData.handle,
      lData->children[stats->from_proc[objidx]], -1));
}

// objects arrives with only objdata
void HybridBaseLB::ObjsMigrated(CkVec<LDObjData>&& datas, int m,
    LDCommData *cdata, int n, int atlevel)
{
  int i;
  LevelData *lData = levelData[atlevel];
  LDStats *statsData = lData->statsData;

  if (statsData != NULL) {
    CkVec<LDObjData> &oData = statsData->objData;

    for (i=0; i<m; i++)
    {
      // copy into LDStats
      LDObjData &data = datas[i];
      oData.push_back(data);
      statsData->n_objs++;
      if (data.migratable) statsData->n_migrateobjs++;
      // an incoming object to the root
      // pretend this object belongs to it
      statsData->from_proc.push_back(lData->nChildren);
      statsData->to_proc.push_back(lData->nChildren);
    }

    // copy into comm data
    if (n) {
      CkVec<LDCommData> &cData = statsData->commData;
      for (int i=0; i<n; i++)
        cData.push_back(cdata[i]);
      statsData->n_comm += n;
      statsData->deleteCommHash();
    }
  }
  else {        // leaf node, from which proc is unknown at this time
    for (i=0; i<m; i++) {
      LDObjData &data = datas[i];
      LDObjKey key;
      key.omID() = data.omID();
      key.objID() = data.objID();
      newObjs.push_back(Location(key, -1));
    }
  }

  lData->obj_completed+=m;
  if (lData->migrationDone()) {
    StatsDone(atlevel);
  }
}

void HybridBaseLB::VectorDone(int atlevel)
{
  LevelData *lData = levelData[atlevel];
  lData->vector_expected = -1;
  lData->vector_completed = 0;
    // update the migration expected
  lData->migrates_expected = vector_n_moves;
  vector_n_moves = 0;
  if (_lb_args.debug()>1)
    CkPrintf("[%d] VectorDone %d %d at %f.\n", CkMyPe(), lData->vector_expected, lData->migrates_expected, CkWallTimer());
}

// one processor is going to send "count" objects to this processor
void HybridBaseLB::TotalObjMigrated(int count, int atlevel)
{
  LevelData *lData = levelData[atlevel];
  lData->vector_completed ++;
  vector_n_moves += count;
  if (_lb_args.debug()>1)
    CkPrintf("[%d] TotalObjMigrated receive %d objects at %f.\n", CkMyPe(), count, CkWallTimer());
  if (lData->vectorReceived()) {
    VectorDone(atlevel);
    if (lData->migrationDone())
      StatsDone(atlevel);
  }
}

void HybridBaseLB::Migrated(LDObjHandle h, int waitBarrier)
{
  LevelData *lData = levelData[0];

  lData->migrates_completed++;
  DEBUGF(("[%d] An object migrated! %d %d\n", CkMyPe(),lData->migrates_completed,lData->migrates_expected));
  if (lData->migrationDone()) {
    if (!lData->resumeAfterMigration) {
      StatsDone(0);
    }
    else {
      // migration done finally
      MigrationDone(1);
    }
  }
}

// find sender comms
void HybridBaseLB::collectCommData(int objIdx, CkVec<LDCommData> &comms, int atlevel)
{
  LevelData *lData = levelData[atlevel];
  LDStats *statsData = lData->statsData;

  LDObjData &objData = statsData->objData[objIdx];

  for (int com=0; com<statsData->n_comm; com++) {
    LDCommData &cdata = statsData->commData[com];
    if (cdata.from_proc()) continue;
    if (cdata.sender.objID() == objData.objID() && cdata.sender.omID() == objData.omID())
      comms.push_back(cdata);
  }
}

// an object arrives with only objdata
void HybridBaseLB::ObjMigrated(LDObjData data, LDCommData *cdata, int n, int atlevel)
{
  LevelData *lData = levelData[atlevel];
  LDStats *statsData = lData->statsData;

  if (statsData != NULL) {
    CkVec<LDObjData> &oData = statsData->objData;

    // copy into LDStats
    oData.push_back(data);
    statsData->n_objs++;
    if (data.migratable) statsData->n_migrateobjs++;
    // an incoming object to the root
    // pretend this object belongs to it
    statsData->from_proc.push_back(lData->nChildren);
    statsData->to_proc.push_back(lData->nChildren);

    // copy into comm data
    if (n) {
      CkVec<LDCommData> &cData = statsData->commData;
      for (int i=0; i<n; i++) 
        cData.push_back(cdata[i]);
      statsData->n_comm += n;
      statsData->deleteCommHash();
    }
  }
  else { 	// leaf node, from which proc is unknown at this time
    LDObjKey key;
    key.omID() = data.omID();
    key.objID() = data.objID();
    newObjs.push_back(Location(key, -1));
  }

  lData->obj_completed++;
  if (lData->migrationDone()) {
    StatsDone(atlevel);
  }
}


void HybridBaseLB::StatsDone(int atlevel)
{

  LevelData *lData = levelData[atlevel];
  lData->obj_expected = -1;
  lData->migrates_expected = -1;
  lData->obj_completed = 0;
  lData->migrates_completed = 0;

  CmiAssert(lData->parent!=-1);

  thisProxy[lData->parent].NotifyObjectMigrationDone(atlevel);
}

// called on a parent node
void HybridBaseLB::NotifyObjectMigrationDone(int fromlevel)
{

  int atlevel = fromlevel + 1;
  LevelData *lData = levelData[atlevel];

  lData->mig_reported ++;
  if (lData->mig_reported == lData->nChildren) {
    lData->mig_reported = 0;
    // start load balancing at this level
    if (atlevel > 1) {
      // I am done at the level, propagate load balancing to next level
      thisProxy.Loadbalancing(atlevel-1, lData->nChildren, lData->children);
    }
    else {  // atlevel = 1
      if (_lb_args.debug() > 1)
         CkPrintf("[%d] NotifyObjectMigrationDone at level %d started at %f\n",
	        CkMyPe(), atlevel, CkWallTimer());
      DummyMsg *m = new (8*sizeof(int)) DummyMsg;
      *((int *)CkPriorityPtr(m)) = -100-atlevel;
      CkSetQueueing(m, CK_QUEUEING_IFIFO);
      thisProxy.StartCollectInfo(m, lData->nChildren, lData->children);
    }
  }
}

// start from leaves of a domain, all processors in the domain start a 
// tree reduction to fill pending from/to proc pairs.
void HybridBaseLB::StartCollectInfo(DummyMsg *m)
{
  int i;
  delete m;
  LevelData *lData = levelData[0];
      // we are leaf, start a tree reduction to find from/to proc pairs
      // set this counter
  lData->resumeAfterMigration =  1;

      // Locations
  int migs = lData->outObjs.size() + newObjs.size();
  Location *locs = new Location[migs];
  int count=0;
  int me = CkMyPe();
  for (i=0; i<newObjs.size(); i++) {
        locs[count] = newObjs[i];
        locs[count].loc = me;
        count++;
  }
  for (i=0; i<lData->outObjs.size(); i++) {
        LDObjKey key;
        key.omID() = lData->outObjs[i].handle.omID();
        key.objID() = lData->outObjs[i].handle.objID();
        locs[count].key = key;
        locs[count].loc = -1;		// unknown
        count++;
  }
    // assuming leaf must have a parent
  DEBUGF(("[%d] level 0 has %d unmatched (out)%d+(new)%d. \n", CkMyPe(), migs, lData->outObjs.size(), newObjs.size()));
  thisProxy[lData->parent].CollectInfo(locs, migs, 0);
  delete [] locs;
}

void HybridBaseLB::CollectInfo(Location *loc, int n, int fromlevel)
{
   int atlevel = fromlevel + 1;
   LevelData *lData = levelData[atlevel];
   lData->info_recved++;

   CkVec<Location> &matchedObjs = lData->matchedObjs;
   std::map<LDObjKey, int> &unmatchedObjs = lData->unmatchedObjs;

   // sort into matched and unmatched list
#if 0
   for (int i=0; i<n; i++) {
     // search and see if we have answer, put to matched
     // store in unknown
     int found = 0;
     for (int obj=0; obj<unmatchedObjs.size(); obj++) {
       if (loc[i].key == unmatchedObjs[obj].key) {
         // answer must exist
         CmiAssert(unmatchedObjs[obj].loc != -1 || loc[i].loc != -1);
         if (unmatchedObjs[obj].loc == -1) unmatchedObjs[obj].loc = loc[i].loc;
         matchedObjs.push_back(unmatchedObjs[obj]);
         unmatchedObjs.remove(obj);
         found = 1;
         break;
       }
     }
     if (!found) unmatchedObjs.push_back(loc[i]);
   }
#else
   for (int i=0; i<n; i++) {
     std::map<LDObjKey, int>::iterator iter = unmatchedObjs.find(loc[i].key);
     if (iter != unmatchedObjs.end()) {
       CmiAssert(iter->second != -1 || loc[i].loc != -1);
       if (loc[i].loc == -1) loc[i].loc = iter->second;
       matchedObjs.push_back(loc[i]);
       unmatchedObjs.erase(iter);
     }
     else
       unmatchedObjs[loc[i].key] = loc[i].loc;
   }
#endif

  DEBUGF(("[%d] level %d has %d unmatched and %d matched. \n", CkMyPe(), atlevel, unmatchedObjs.size(), matchedObjs.size()));

   if (lData->info_recved == lData->nChildren) {
     lData->info_recved = 0;
     if (_lb_args.debug() > 1)
         CkPrintf("[%d] CollectInfo at level %d started at %f\n",
	        CkMyPe(), atlevel, CkWallTimer());
     if (lData->parent != -1) {
       // send only unmatched ones up the tree
       CkVec<Location> unmatchedbuf;
       for(std::map<LDObjKey, int>::const_iterator it = unmatchedObjs.begin(); it != unmatchedObjs.end(); ++it)
       {
         unmatchedbuf.push_back(Location(it->first, it->second));
       }

       thisProxy[lData->parent].CollectInfo(unmatchedbuf.getVec(), unmatchedbuf.size(), atlevel);
     }
     else { // root
       // we should have all answers now
       CmiAssert(unmatchedObjs.size() == 0);
       // start send match list down
       thisProxy.PropagateInfo(matchedObjs.getVec(), matchedObjs.size(), atlevel, lData->nChildren, lData->children);
       lData->statsData->clear();
     }
   }
}

void HybridBaseLB::PropagateInfo(Location *loc, int n, int fromlevel)
{
#if CMK_LBDB_ON
  int i, obj;
  int atlevel = fromlevel - 1;
  LevelData *lData = levelData[atlevel];
  CkVec<Location> &matchedObjs = lData->matchedObjs;
  //CkVec<Location> &unmatchedObjs = lData->unmatchedObjs;
  std::map<LDObjKey, int> &unmatchedObjs = lData->unmatchedObjs;

  if (atlevel > 0) {
    if (_lb_args.debug() > 1)
      CkPrintf("[%d] PropagateInfo at level %d started at %f\n",
	        CkMyPe(), atlevel, CkWallTimer());
    // search in unmatched
#if 0
    for (i=0; i<n; i++) {
       // search and see if we have answer, put to matched
       // store in unknown
       for (obj=0; obj<unmatchedObjs.size(); obj++) {
         if (loc[i].key == unmatchedObjs[obj].key) {
           // answer must exist now
           CmiAssert(unmatchedObjs[obj].loc != -1 || loc[i].loc != -1);
           if (unmatchedObjs[obj].loc == -1) unmatchedObjs[obj].loc = loc[i].loc;
           matchedObjs.push_back(unmatchedObjs[obj]);
           unmatchedObjs.remove(obj);
           break;
         }
       }
    }
#else
   for (int i=0; i<n; i++) {
       // search and see if we have answer, put to matched
     const LDObjKey key = loc[i].key;
     std::map<LDObjKey, int>::iterator iter = unmatchedObjs.find(key);
     if (iter != unmatchedObjs.end()) {
       // answer must exist now
       CmiAssert(iter->second != -1 || loc[i].loc != -1);
       if (loc[i].loc == -1) loc[i].loc = iter->second;
       matchedObjs.push_back(loc[i]);
       unmatchedObjs.erase(iter);
     }
   }
#endif
    CmiAssert(unmatchedObjs.size() == 0);
    DEBUGF(("[%d] level %d PropagateInfo had %d matchedObjs. \n", CkMyPe(), atlevel, matchedObjs.size()));

      // send down
    thisProxy.PropagateInfo(matchedObjs.getVec(), matchedObjs.size(), atlevel, lData->nChildren, lData->children);

    lData->statsData->clear();
    matchedObjs.free();
  }
  else {  // leaf node
      // now start to migrate
    CkVec<MigrationRecord> & outObjs = lData->outObjs;
    int migs = outObjs.size() + newObjs.size();
    for (i=0; i<outObjs.size(); i++) {
      if (outObjs[i].toPe == -1) {
        for (obj=0; obj<n; obj++) {
          if (loc[obj].key.omID() == outObjs[i].handle.omID() &&
              loc[obj].key.objID() == outObjs[i].handle.objID()) {
            outObjs[i].toPe = loc[obj].loc;
            break;
          }
        }
        CmiAssert(obj < n);
      }
      CmiAssert(outObjs[i].toPe != -1);
        // migrate now!
      lbmgr->Migrate(outObjs[i].handle,outObjs[i].toPe);
    }   // end for out
    // incoming
    lData->migrates_expected = 0;
    future_migrates_expected = 0;
    for (i=0; i<newObjs.size(); i++) {
      if (newObjs[i].loc == -1) {
        for (obj=0; obj<n; obj++) {
          if (loc[obj].key == newObjs[i].key) {
            newObjs[i].loc = loc[obj].loc;
            break;
          }
        }
        CmiAssert(obj < n);
      }
      CmiAssert(newObjs[i].loc != -1);
      lData->migrates_expected++;
    }   // end of for
    DEBUGF(("[%d] expecting %d\n", CkMyPe(), lData->migrates_expected));
    if (lData->migrationDone()) {
      MigrationDone(1);
    }
  }
#endif
}

void HybridBaseLB::MigrationDone(int balancing)
{
#if CMK_LBDB_ON
  LevelData *lData = levelData[0];

  DEBUGF(("[%d] HybridBaseLB::MigrationDone!\n", CkMyPe()));

  lbmgr->incStep();

  // reset 
  for (int i=0; i<tree->numLevels(); i++) 
    levelData[i]->clear();
  newObjs.free();

  DEBUGF(("[%d] calling ResumeClients.\n", CkMyPe()));
  if (balancing && _lb_args.syncResume()) {
    // max load of all
    CkCallback cb(CkReductionTarget(HybridBaseLB, ResumeClients), thisProxy);
    contribute(sizeof(double), &maxLoad, CkReduction::max_double, cb);
  }
  else
    thisProxy[CkMyPe()].ResumeClients(balancing);

  maxLoad = 0.0;
#endif
}

void HybridBaseLB::ResumeClients(double result)
{
/*
  if (CkMyPe() == 0 && _lb_args.printSummary()) {
    double mload = *(double *)msg->getData();
    CkPrintf("[%d] MAX Load: %f at step %d.\n", CkMyPe(), mload, step()-1);
  }
*/
  ResumeClients(1);
}

void HybridBaseLB::ResumeClients(int balancing)
{
#if CMK_LBDB_ON
  DEBUGF(("[%d] ResumeClients. \n", CkMyPe()));

  double end_lb_time = CkWallTimer();
  if (CkMyPe() == 0 && balancing) {
    if (_lb_args.debug())
      CkPrintf("[%s] Load balancing step %d finished at %f duration %f\n",
	        lbName(), step()-1,end_lb_time,end_lb_time - start_lb_time);
  }

  // zero out stats
  lbmgr->ClearLoads();

  lbmgr->ResumeClients();
	lbmgr->SetMigrationCost(end_lb_time - start_lb_time);
#endif
}

void HybridBaseLB::work(LDStats* stats)
{
#if CMK_LBDB_ON
  CkPrintf("[%d] HybridBaseLB::work called!\n", CkMyPe());
#endif
}
  
LBMigrateMsg * HybridBaseLB::createMigrateMsg(LDStats* stats)
{
#if CMK_LBDB_ON
  int i;

  LevelData *lData = levelData[currentLevel];

  CkVec<MigrateInfo*> migrateInfo;

  // stats contains all objects that belong to this group
  // outObjs contains objects that are migrated out
  for (i=0; i<stats->n_objs; i++) {
    LDObjData &objData = stats->objData[i];
    int frompe = stats->from_proc[i];
    int tope = stats->to_proc[i];
    CmiAssert(tope != -1);
    if (frompe != tope) {
      //      CkPrintf("[%d] Obj %d migrating from %d to %d\n",
      //         CkMyPe(),obj,pe,dest);
#if 0
      // delay until a summary is printed
      if (frompe == lData->nChildren)  {
        frompe = -1;
        CmiAssert(tope != -1 && tope != lData->nChildren);
      }
      else
        frompe = lData->children[frompe];
      if (tope != -1) {
        CmiAssert(tope < lData->nChildren);
        tope = lData->children[tope];
      }
#endif
      MigrateInfo *migrateMe = new MigrateInfo;
      migrateMe->obj = objData.handle;
      migrateMe->from_pe = frompe;
      migrateMe->to_pe = tope;
      migrateMe->async_arrival = objData.asyncArrival;
      migrateInfo.insertAtEnd(migrateMe);
    }
    else 
      CmiAssert(frompe != lData->nChildren);
  }

  // merge outgoing objs
  CkVec<MigrationRecord> &outObjs = lData->outObjs;
  for (i=0; i<outObjs.size(); i++) {
    MigrateInfo *migrateMe = new MigrateInfo;
    migrateMe->obj = outObjs[i].handle;
    migrateMe->from_pe = outObjs[i].fromPe;
    migrateMe->to_pe = -1;
//    migrateMe->async_arrival = objData.asyncArrival;
    migrateInfo.insertAtEnd(migrateMe);
  }

  // construct migration message
  int migrate_count=migrateInfo.length();
  DEBUGF(("[%d] level: %d has %d migrations. \n", CkMyPe(), currentLevel, migrate_count));
  // ignore avail_vector, etc for now
  //LBMigrateMsg * msg = new(migrate_count,count,count,0) LBMigrateMsg;
  LBMigrateMsg * msg = new(migrate_count,0,0,0) LBMigrateMsg;
  msg->level = currentLevel;
  msg->n_moves = migrate_count;
  for(i=0; i < migrate_count; i++) {
    MigrateInfo* item = (MigrateInfo*) migrateInfo[i];
    msg->moves[i] = *item;
    delete item;
    migrateInfo[i] = 0;
    DEBUGF(("[%d] obj (%d %d %d %d) migrate from %d to %d\n", CkMyPe(), item->obj.objID().id[0], item->obj.objID().id[1], item->obj.objID().id[2], item->obj.objID().id[3], item->from_pe, item->to_pe));
  }

  if (_lb_args.printSummary())  printSummary(stats, stats->nprocs());

  // translate relative pe number to its real number
  for(i=0; i < migrate_count; i++) {
    MigrateInfo* move = &msg->moves[i];
    if (move->to_pe != -1) {
      if (move->from_pe == lData->nChildren)  {
          // an object from outside group
        move->from_pe = -1;
        CmiAssert(move->to_pe != -1 && move->to_pe != lData->nChildren);
      }
      else
        move->from_pe = lData->children[move->from_pe];
      CmiAssert(move->to_pe < lData->nChildren);
      move->to_pe = lData->children[move->to_pe];
    }
  }

  return msg;
#else
  return NULL;
#endif
}

// function used for any application that uses Strategy() instead of work()
LBMigrateMsg * HybridBaseLB::createMigrateMsg(CkVec<MigrateInfo *> &migrateInfo,int count)
{
  int i;

  // merge outgoing objs
  LevelData *lData = levelData[currentLevel];
  CkVec<MigrationRecord> &outObjs = lData->outObjs;
  for (i=0; i<outObjs.size(); i++) {
    MigrateInfo *migrateMe = new MigrateInfo;
    migrateMe->obj = outObjs[i].handle;
    migrateMe->from_pe = outObjs[i].fromPe;
    migrateMe->to_pe = -1;
    migrateInfo.insertAtEnd(migrateMe);
  }

  if (_lb_args.printSummary())  printSummary(NULL, count);

  int migrate_count=migrateInfo.length();
  // ignore avail_vector, etc for now
  //LBMigrateMsg * msg = new(migrate_count,count,count,0) LBMigrateMsg;
  LBMigrateMsg* msg = new(migrate_count,0,0,0) LBMigrateMsg;
  msg->level = currentLevel;
  msg->n_moves = migrate_count;
  for(i=0; i < migrate_count; i++) {
    MigrateInfo* item = migrateInfo[i];
    msg->moves[i] = *item;
    delete item;
    migrateInfo[i] = 0;
  } 
  return msg;
}

int HybridBaseLB::NeighborIndex(int pe, int atlevel)
{
    int peslot = -1;
    for(int i=0; i < levelData[atlevel]->nChildren; i++) {
      if (pe == levelData[atlevel]->children[i]) {
	peslot = i;
	break;
      }
    }
    return peslot;
}

void HybridBaseLB::printSummary(LDStats *stats, int count)
{
  double stime = CkWallTimer();
#if 1
  if (currentLevel == 1 && stats!=NULL) {
      LBInfo info(count);
      info.getInfo(stats, count, 1);	// no comm cost
      double mLoad, mCpuLoad, totalLoad;
      info.getSummary(mLoad, mCpuLoad, totalLoad);
      int nmsgs, nbytes;
      stats->computeNonlocalComm(nmsgs, nbytes);
      //CkPrintf("[%d] Load Summary: max (with comm): %f max (obj only): %f total: %f on %d processors at step %d useMem: %fKB nonlocal: %d %dKB.\n", CkMyPe(), maxLoad, mCpuLoad, totalLoad, count, step(), (1.0*useMem())/1024, nmsgs, nbytes/1024);
      thisProxy[0].reportLBQulity(mLoad, mCpuLoad, totalLoad, nmsgs, nbytes/1024);
  }
#endif

  if (currentLevel == tree->numLevels()-2) {
      double mem = (1.0*useMem())/1024;
      thisProxy[0].reportLBMem(mem);
  }
  CkPrintf("[%d] Print Summary takes %f seconds. \n", CkMyPe(), CkWallTimer()-stime);
}

// only called on PE 0
void HybridBaseLB::reportLBQulity(double mload, double mCpuLoad, double totalload, int nmsgs, double bytes)
{
  static int pecount=0;
  CmiAssert(CkMyPe() == 0);
  if (mload > maxLoad) maxLoad = mload;
  if (mCpuLoad > maxCpuLoad) maxCpuLoad = mCpuLoad;
  totalLoad += totalload;
  maxCommCount += nmsgs;
  maxCommBytes += bytes;   // KB
  pecount++;
  if (pecount == tree->numNodes(1)) {
    CkPrintf("[%d] Load Summary: max (with comm): %f max (obj only): %f total: %f at step %d nonlocal: %d msgs, %.2fKB reported from %d PEs.\n", CkMyPe(), maxLoad, maxCpuLoad, totalLoad, step(), maxCommCount, maxCommBytes, pecount);
    maxLoad = 0.0;
    maxCpuLoad = 0.0;
    totalLoad = 0.0;
    maxCommCount = 0;
    maxCommBytes = 0.0;
    pecount = 0;
  }
}

// only called on PE 0
void HybridBaseLB::reportLBMem(double mem)
{
  static int pecount=0;
  CmiAssert(CkMyPe() == 0);
  if (mem > maxMem) maxMem = mem;
  pecount++;
  if (pecount == tree->numNodes(tree->numLevels()-2)) {
    CkPrintf("[%d] Load Summary: maxMem: %fKB reported at step %d from %d PEs.\n", CkMyPe(), maxMem, step(), pecount);
    maxMem = 0.0;
    pecount = 0;
  }
}

int HybridBaseLB::useMem()
{
  int i;
  int memused = 0;
  for (i=0; i<levelData.size(); i++)
    if (levelData[i]) memused+=levelData[i]->useMem();
  memused += newObjs.size() * sizeof(Location);
  return memused;
}

#include "HybridBaseLB.def.h"

/*@{*/


