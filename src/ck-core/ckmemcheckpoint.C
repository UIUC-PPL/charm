
/*
Charm++ support for fault tolerance of
In memory synchronous checkpointing and restart

written by Gengbin Zheng, gzheng@uiuc.edu
           Lixia Shi,     lixiashi@uiuc.edu

added 12/18/03:

To support fault tolerance while allowing migration, it uses double
checkpointing scheme for each array element (not a infallible scheme).
In this version, checkpointing is done based on array elements. 
Each array element individully sends its checkpoint data to two buddies.

In this implementation, assume only one failure happens at a time,
or two failures on two processors which are not buddy to each other;
also assume there is no failure during a checkpointing or restarting phase.

Restart phase contains two steps:
1. Converse level restart: the newly created process for the failed
   processor recover its system data (no array elements) from 
   its backup processor.
2. Charm++ level restart: CkMemCheckPT gets control and recover array 
   elements and reset all states of system groups to be consistent.

added 3/14/04:
1. also support for double in-disk checkpoint/restart
   set "where" to CkCheckPoint_inDISK/CkCheckPoint_inMEM in init()

added 4/16/04:
1. also support the case when there is a pool of extra processors.
   set CK_NO_PROC_POOL to 0.

TODO:
1. checkpoint scheme can be reimplemented based on per processor scheme;
 restart phase should restore/reset group table, etc on all processors, thus flushStates() can be eliminated.
2. crash at checkpointing phase currently is catastrophic, can be fixed by storing another transient checkpoints.

*/

#include "unistd.h"

#include "charm++.h"
#include "ck.h"
#include "register.h"
#include "conv-ccs.h"
#include <signal.h>

extern int quietModeRequested;

void noopck(const char*, ...)
{}


//#define DEBUGF       CkPrintf
#define DEBUGF noopck

// pick buddy processor from a different physical node
#define NODE_CHECKPOINT                        0

static int replicaDieHandlerIdx;
static int replicaDieBcastHandlerIdx;
static int changePhaseHandlerIdx;
// assume NO extra processors--1
// assume extra processors--0
#if CMK_CONVERSE_MPI
#define CK_NO_PROC_POOL				0
static int pingHandlerIdx;
static int pingCheckHandlerIdx;
static int buddyDieHandlerIdx;
static double lastPingTime = -1;
void pingBuddy();
void pingCheckHandler();
#else
#define CK_NO_PROC_POOL				0
#endif

// 0 - use QD, 1 - use reduction
#define CMK_CHKP_USE_REDN 1

#define CMK_CHKP_ALL		1
#define CMK_USE_BARRIER		0

#define FAIL_DET_THRESHOLD 10

//stream remote records happned only if CK_NO_PROC_POOL =1 which means the chares to pe map will change
#define STREAMING_INFORMHOME                    1
CpvDeclare(int, _crashedNode);

// static, so that it is accessible from Converse part
bool CkMemCheckPT::inRestarting = false;
bool CkMemCheckPT::inCheckpointing = false;
bool CkMemCheckPT::inLoadbalancing = false;
double CkMemCheckPT::startTime;
char *CkMemCheckPT::stage;
CkCallback CkMemCheckPT::cpCallback;

bool _memChkptOn = true;		// checkpoint is on or off

CkGroupID ckCheckPTGroupID;		// readonly

static bool checkpointed = false;

/// @todo the following declarations should be moved into a separate file for all 
// fault tolerant strategies

#ifdef CMK_MEM_CHECKPOINT
// name of the kill file that contains processes to be killed 
char *killFile;                                               
// flag for the kill file         
int killFlag=0;
// variable for storing the killing time
double killTime=0.0;
#endif

#ifdef CKLOCMGR_LOOP
#undef CKLOCMGR_LOOP
#endif
// loop over all CkLocMgr and do "code"
#define  CKLOCMGR_LOOP(code)	{	\
  int numGroups = CkpvAccess(_groupIDTable)->size(); 	\
  for(int i=0;i<numGroups;i++) {	\
    IrrGroup *obj = CkpvAccess(_groupTable)->find((*CkpvAccess(_groupIDTable))[i]).getObj();	\
    if(obj->isLocMgr())  {	\
      CkLocMgr *mgr = (CkLocMgr*)obj;	\
      code	\
    }	\
  }	\
 }

/// checkpoint buffer for processor system data, remove static to make icpc 10.1 pass with -O
//make procChkptBuf an array of two to store both previous and current checkpoint
CpvDeclare(CkProcCheckPTMessage**, procChkptBuf);
//point to the checkpoint should be used for recovery
CpvDeclare(int, chkpPointer);
CpvDeclare(int, chkpNum);

// compute the backup processor
// FIXME: avoid crashed processors
inline int ChkptOnPe(int pe) { return (pe+CmiMyNodeSize())%CkNumPes(); }

inline int CkMemCheckPT::BuddyPE(int pe)
{
  int budpe;
#if NODE_CHECKPOINT
    // buddy is the processor with same rank on the next physical node
  int r1 = CmiPhysicalRank(pe);
  int budnode = CmiPhysicalNodeID(pe);
  do {
    budnode = (budnode+1)%CmiNumPhysicalNodes();
    int *pelist;
    int num;
    CmiGetPesOnPhysicalNode(budnode, &pelist, &num);
    budpe = pelist[r1 % num];
  } while (isFailed(budpe));
  if (budpe == pe) {
    CmiPrintf("[%d] Error: failed to find a buddy processor on a different node.\n", pe);
    CmiAbort("Failed to find a buddy processor");
  }
#else
  budpe = pe;
  while (budpe == pe || isFailed(budpe)) 
          budpe = (budpe+1)%CkNumPes();
#endif
  return budpe;
}

inline int CkMemCheckPT::ReverseBuddyPE(int pe)
{
  int budpe = pe;
  while (budpe == pe || isFailed(budpe))
  {
    budpe = (budpe == 0 ? CkNumPes() : budpe) - 1;
  }
  return budpe;
}

// called in array element constructor
// choose and register with 2 buddies for checkpoiting 
#if CMK_MEM_CHECKPOINT
void ArrayElement::init_checkpt() {
	if (!_memChkptOn) return;
	if (CkInRestarting()) {
	  CkPrintf("[%d] Warning: init_checkpt called during restart, possible bug in migration constructor!\n", CmiMyPe());
	}
	// only master init checkpoint
        if (thisArray->getLocMgr()->managers.begin()->second != thisArray) return;

        budPEs[0] = CkMyPe();
        budPEs[1] = CProxy_CkMemCheckPT(ckCheckPTGroupID).ckLocalBranch()->BuddyPE(CkMyPe());
	CmiAssert(budPEs[0] != budPEs[1]);
        // inform checkPTMgr
        CProxy_CkMemCheckPT checkptMgr(ckCheckPTGroupID);
	//DEBUGF("[%d] ArrayElement::init_checkpt array %d %p pe: %d %d\n", CkMyPe(), ((CkGroupID)thisArrayID).idx, this, budPEs[0], budPEs[1]);
        checkptMgr[budPEs[0]].createEntry(thisArrayID, thisArray->getLocMgr()->getGroupID(), thisIndexMax, budPEs[1]);        
	checkptMgr[budPEs[1]].createEntry(thisArrayID, thisArray->getLocMgr()->getGroupID(), thisIndexMax, budPEs[0]);
}
#endif

// entry function invoked by checkpoint mgr asking for checkpoint data
void ArrayElement::inmem_checkpoint(CkArrayCheckPTReqMessage *m) {
#if CMK_MEM_CHECKPOINT
//  DEBUGF("[p%d] HERE checkpoint to PE %d %d \n", CkMyPe(), budPEs[0], budPEs[1]);
//char index[128];   thisIndexMax.sprint(index);
//DEBUGF("[%d] checkpointing %s\n", CkMyPe(), index);
  CkLocMgr *locMgr = thisArray->getLocMgr();
  CmiAssert(myRec!=NULL);
  size_t size;
  {
        PUP::sizer p;
        locMgr->pupElementsFor (p, myRec, CkElementCreation_migrate);
        size = p.size();
  }
  size_t packSize = size/sizeof(double) +1;
  CkArrayCheckPTMessage *msg =
                 new (packSize, 0) CkArrayCheckPTMessage;
  msg->len = size;
  msg->index =thisIndexMax;
  msg->aid = thisArrayID;
  msg->locMgr = locMgr->getGroupID();
  msg->cp_flag = true;
  {
        PUP::toMem p(msg->packData);
        locMgr->pupElementsFor (p, myRec, CkElementCreation_migrate);
  }

  CProxy_CkMemCheckPT checkptMgr(ckCheckPTGroupID);
  checkptMgr.recvData(msg, 2, budPEs);
  delete m;
#endif
}

// checkpoint holder class - for memory checkpointing
class CkMemCheckPTInfo: public CkCheckPTInfo
{
  CkArrayCheckPTMessage *ckBuffer;
public:
  CkMemCheckPTInfo(CkArrayID a, CkGroupID loc, CkArrayIndex idx, int pno): 
	            CkCheckPTInfo(a, loc, idx, pno)
  {
    ckBuffer = NULL;
  }
  ~CkMemCheckPTInfo() 
  {
    if (ckBuffer) delete ckBuffer; 
  }
  inline void updateBuffer(CkArrayCheckPTMessage *data) 
  {
    CmiAssert(data!=NULL);
    if (ckBuffer) delete ckBuffer;
    ckBuffer = data;
  }    
  inline CkArrayCheckPTMessage * getCopy()
  {
    if (ckBuffer == NULL) {
      CmiPrintf("[%d] recoverArrayElements: element does not have checkpoint data.", CkMyPe());
      CmiAbort("Abort!");
    }
    return (CkArrayCheckPTMessage *)CkCopyMsg((void **)&ckBuffer);
  }     
  inline void updateBuddy(int b1, int b2) {
     CmiAssert(ckBuffer);
     ckBuffer->bud1 = b1; ckBuffer->bud2 = b2;
     pNo = b1;  if (pNo == CkMyPe()) pNo = b2;
     CmiAssert(pNo != CkMyPe());
  }
  inline size_t getSize() {
     CmiAssert(ckBuffer);
     return ckBuffer->len; 
  }
};

// checkpoint holder class - for in-disk checkpointing
class CkDiskCheckPTInfo: public CkCheckPTInfo 
{
  std::string fname;
  int bud1, bud2;
  size_t len; 			// checkpoint size
public:
  CkDiskCheckPTInfo(CkArrayID a, CkGroupID loc, CkArrayIndex idx, int pno, int myidx): CkCheckPTInfo(a, loc, idx, pno)
  {
#if CMK_USE_MKSTEMP
#if CMK_CONVERSE_MPI
    fname = "/tmp/ckpt" + std::to_string(CmiMyPartition()) + "-" + std::to_string(CkMyPe()) + "-" + std::to_string(myidx) + "-XXXXXX";
#else
    fname = "/tmp/ckpt" + std::to_string(CkMyPe()) + "-" + std::to_string(myidx) + "-XXXXXX";
#endif
    if(mkstemp(&fname[0]) < 0)
    {
      CmiAbort("mkstemp fail in checkpoint");
    }
#else
    fname = tmpnam(NULL);
#endif
    bud1 = bud2 = -1;
    len = 0;
  }
  ~CkDiskCheckPTInfo() 
  {
    remove(fname.c_str());
  }
  inline void updateBuffer(CkArrayCheckPTMessage *data) 
  {
    double t = CmiWallTimer();
    // unpack it
    envelope *env = UsrToEnv(data);
    CkUnpackMessage(&env);
    data = (CkArrayCheckPTMessage *)EnvToUsr(env);
    FILE *f = fopen(fname.c_str(),"wb");
    PUP::toDisk p(f);
    CkPupMessage(p, (void **)&data);
    // delay sync to the end because otherwise the messages are blocked
//    fsync(fileno(f));
    fclose(f);
    bud1 = data->bud1;
    bud2 = data->bud2;
    len = data->len;
    delete data;
    //DEBUGF("[%d] updateBuffer took %f seconds. \n", CkMyPe(), CmiWallTimer()-t);
  }
  inline CkArrayCheckPTMessage * getCopy()	// get a copy of checkpoint
  {
    CkArrayCheckPTMessage *data;
    FILE *f = fopen(fname.c_str(),"rb");
    PUP::fromDisk p(f);
    CkPupMessage(p, (void **)&data);
    fclose(f);
    data->bud1 = bud1;				// update the buddies
    data->bud2 = bud2;
    return data;
  }
  inline void updateBuddy(int b1, int b2) {
     bud1 = b1; bud2 = b2;
     pNo = b1;  if (pNo == CkMyPe()) pNo = b2;
     CmiAssert(pNo != CkMyPe());
  }
  inline size_t getSize() {
     return len; 
  }
};

CkMemCheckPT::CkMemCheckPT(int w)
{
  int numnodes = 0;
#if NODE_CHECKPOINT
  numnodes = CmiNumPhysicalNodes();
#else
  numnodes = CkNumPes();
#endif
#if CK_NO_PROC_POOL
  if (numnodes <= 2)
#else
  if (numnodes == 1)
#endif
  {
    if (CkMyPe() == 0 && !quietModeRequested)
#if CK_NO_PROC_POOL
      CkPrintf("CharmFT> Warning: In-memory checkpointing is disabled because there are less than three nodes.\n");
#else
      CkPrintf("CharmFT> Warning: In-memory checkpointing is disabled because there is only one node.\n");
#endif
    _memChkptOn = false;
  }
  inRestarting = false;
  recvCount = peCount = 0;
  recvChkpCount = 0;
  ackCount = 0;
  expectCount = -1;
  where = w;

#if CMK_CONVERSE_MPI
  if(CkNumPes() > 1) {
    void pingBuddy();
    void pingCheckHandler();
    CcdCallOnCondition(CcdPERIODIC_100ms,(CcdVoidFn)pingBuddy,NULL);
    CcdCallOnCondition(CcdPERIODIC_5s,(CcdVoidFn)pingCheckHandler,NULL);
  }
#endif
#if CMK_CHKP_ALL
  initEntry();
#endif        
}

void CkMemCheckPT::initEntry()
{
#if CMK_CHKP_ALL
  chkpTable[0].init(where, 0);
  chkpTable[1].init(where, 1);
#endif 
}

CkMemCheckPT::~CkMemCheckPT()
{
  for (CkCheckPTInfo* it : ckTable) {
    delete it;
  }
}

void CkMemCheckPT::pup(PUP::er& p) 
{ 
  p|cpStarter;
  p|thisFailedPe;
  p|failedPes;
  p|ckCheckPTGroupID;		// recover global variable
  p|cpCallback;			// store callback
  p|where;			// where to checkpoint
  p|peCount;
  if (p.isUnpacking()) {
  	recvCount = peCount = 0;
 	recvChkpCount = 0;
	ackCount = 0;
  	expectCount = -1;
        inCheckpointing = false;
#if CMK_CONVERSE_MPI
  if(CkNumPes() > 1) {
    void pingBuddy();
    void pingCheckHandler();
    CcdCallOnCondition(CcdPERIODIC_100ms,(CcdVoidFn)pingBuddy,NULL);
    CcdCallOnCondition(CcdPERIODIC_5s,(CcdVoidFn)pingCheckHandler,NULL);
  }
#endif
  }
}

// called by checkpoint mgr to restore an array element
void CkMemCheckPT::inmem_restore(CkArrayCheckPTMessage *m) 
{
#if CMK_MEM_CHECKPOINT
  DEBUGF("[%d] inmem_restore restore: mgr: %d \n", CmiMyPe(), m->locMgr);  
  // m->index.print();
  PUP::fromMem p(m->packData, PUP::er::IS_CHECKPOINT);
  CkLocMgr *mgr = CProxy_CkLocMgr(m->locMgr).ckLocalBranch();
  CmiAssert(mgr);
  CmiUInt8 id = mgr->lookupID(m->index);
#if !STREAMING_INFORMHOME && CK_NO_PROC_POOL
  mgr->resume(m->index, id, p, true);     // optimize notifyHome
#else
  mgr->resume(m->index, id, p, false);    // optimize notifyHome
#endif

  // find a list of array elements bound together
  CkArray *arrmgr = m->aid.ckLocalBranch();
  CmiAssert(arrmgr);
  ArrayElement *elt = arrmgr->lookup(m->index);
  CmiAssert(elt);
  CkLocRec *rec = elt->myRec;
  std::vector<CkMigratable *> list;
  mgr->migratableList(rec, list);
  CmiAssert(!list.empty());
  for (int l=0; l<list.size(); l++) {
    elt = (ArrayElement *)list[l];
    elt->budPEs[0] = m->bud1;
    elt->budPEs[1] = m->bud2;
    //    reset, may not needed now
    // for now.
    for (int i=0; i<CK_ARRAYLISTENER_MAXLEN; i++) {
      contributorInfo *c=(contributorInfo *)&elt->listenerData[i];
      if (c) c->redNo = 0;
    }
  }
#endif
  delete m;
}

// return 1 if pe is a crashed processor
bool CkMemCheckPT::isFailed(int pe)
{
  for (int i=0; i<failedPes.size(); i++)
    if (failedPes[i] == pe) return true;
  return false;
}

// add pe into history list of all failed processors
void CkMemCheckPT::failed(int pe)
{
  if (isFailed(pe)) return;
  failedPes.push_back(pe);
}

int CkMemCheckPT::totalFailed()
{
  return failedPes.size();
}

// create an checkpoint entry for array element of aid with index.
void CkMemCheckPT::createEntry(CkArrayID aid, CkGroupID loc, CkArrayIndex index, int buddy)
{
  // error check, no duplicate
  int idx, len = ckTable.size();
  for (idx=0; idx<len; idx++) {
    CkCheckPTInfo *entry = ckTable[idx];
    if (index == entry->index) {
      if (loc == entry->locMgr) {
	  // bindTo array elements
          return;
      }
        // for array inheritance, the following check may fail
        // because ArrayElement constructor of all superclasses are called
      if (aid == entry->aid) {
        CkPrintf("[%d] CkMemCheckPT::createEntry a duplciated entry for arrayID %d:", CkMyPe(), ((CkGroupID)aid).idx); index.print(); CkPrintf("\n");
        CmiAbort("CkMemCheckPT::createEntry a duplciated entry");
      }
    }
  }
  CkCheckPTInfo *newEntry;
  if (where == CkCheckPoint_inMEM)
    newEntry = new CkMemCheckPTInfo(aid, loc, index, buddy);
  else
    newEntry = new CkDiskCheckPTInfo(aid, loc, index, buddy, len+1);
  ckTable.push_back(newEntry);
  //DEBUGF("[%d] CkMemCheckPT::createEntry for arrayID %d:", CkMyPe(), ((CkGroupID)aid).idx); index.print(); CkPrintf("\n");
}

void CkMemCheckPT::recoverEntry(CkArrayCheckPTMessage *msg)
{
#if !CMK_CHKP_ALL	
  int buddy = msg->bud1;
  if (buddy == CkMyPe()) buddy = msg->bud2;
  createEntry(msg->aid, msg->locMgr, msg->index, buddy);
  recvData(msg);
    // ack
  thisProxy[buddy].gotData();
#else
  initEntry();
  thisProxy[msg->bud2].gotData();
  recvArrayCheckpoint(msg);
#endif
}

// loop through my checkpoint table and ask checkpointed array elements
// to send me checkpoint data.
void CkMemCheckPT::doItNow(int starter, CkCallback &&cb)
{
  checkpointed = true;
  cpCallback = cb;
  inCheckpointing = true;
  cpStarter = starter;
  if (CkMyPe() == cpStarter) {
    startTime = CmiWallTimer();
    if (!quietModeRequested)
      CkPrintf("CharmFT> Checkpointing...\n");
  }
#if !CMK_CHKP_ALL
  int len = ckTable.size();
  for (int i=0; i<len; i++) {
    CkCheckPTInfo *entry = ckTable[i];
      // always let the bigger number processor send request
    //if (CkMyPe() < entry->pNo) continue;
      // always let the smaller number processor send request, may on same proc
    if (!isMaster(entry->pNo)) continue;
      // call inmem_checkpoint to the array element, ask it to send
      // back checkpoint data via recvData().
    CkArrayCheckPTReqMessage *msg = new CkArrayCheckPTReqMessage;
    CkSendMsgArray(CkIndex_ArrayElement::inmem_checkpoint(NULL),(CkArrayMessage *)msg,entry->aid,entry->index);
  }
    // if my table is empty, then I am done
  if (len == 0) contribute(CkCallback(CkReductionTarget(CkMemCheckPT, cpFinish), thisProxy[cpStarter]));
#else
  startArrayCheckpoint();
#endif
  // pack and send proc level data
  sendProcData();
}

class MemElementPacker : public CkLocIterator{
	private:
		CkLocMgr *locMgr;
		PUP::er &p;
	public:
		MemElementPacker(CkLocMgr * mgr_,PUP::er &p_):locMgr(mgr_),p(p_){};
		void addLocation(CkLocation &loc){
			CkArrayIndexMax idx = loc.getIndex();
			CkGroupID gID = locMgr->ckGetGroupID();
			CmiUInt8 id = loc.getID();
                        ArrayElement *elt = (ArrayElement *)loc.getLocalRecord();
			CmiAssert(elt);
			//elt = (ArrayElement *)locMgr->lookup(idx, aid);
			p|gID;
			p|idx;
			p|id;
                        locMgr->pupElementsFor(p,loc.getLocalRecord(),CkElementCreation_migrate);
		}
};

void CkMemCheckPT::pupAllElements(PUP::er &p){
#if CMK_CHKP_ALL && CMK_MEM_CHECKPOINT
	int numElements;
	if(!p.isUnpacking()){
		numElements = CkCountArrayElements();
	}
	//cppcheck-suppress uninitvar
	p | numElements;
	if(!p.isUnpacking()){
		CKLOCMGR_LOOP(MemElementPacker packer(mgr,p);mgr->iterate(packer););
	}
#endif
}

void CkMemCheckPT::startArrayCheckpoint(){
#if CMK_CHKP_ALL
	size_t size;
	{
		PUP::sizer psizer;
		pupAllElements(psizer);
		size = psizer.size();
	}
	size_t packSize = size/sizeof(double)+1;
	// DEBUGF("[%d] checkpoint size: %ld\n", CkMyPe(), (CmiUInt8)packSize);
	CkArrayCheckPTMessage * msg = new (packSize,0) CkArrayCheckPTMessage;
	msg->len = size;
	msg->cp_flag = true;
	int budPEs[2];
	msg->bud1=CkMyPe();
	msg->bud2=ChkptOnPe(CkMyPe());
	{
		PUP::toMem p(msg->packData);
		pupAllElements(p);
	}
	thisProxy[msg->bud2].recvArrayCheckpoint((CkArrayCheckPTMessage *)CkCopyMsg((void **)&msg));
	chkpTable[0].updateBuffer(CpvAccess(chkpPointer)^1,msg);
        recvCount++;
#endif
}

void CkMemCheckPT::recvArrayCheckpoint(CkArrayCheckPTMessage *msg)
{
#if CMK_CHKP_ALL
	int idx = 1;
	if(msg->bud1 == CkMyPe()){
		idx = 0;
	}
	
	bool isChkpting = msg->cp_flag;
	int pointer;
	if(isChkpting)
	  pointer = CpvAccess(chkpPointer)^1;
	else
	  pointer = CpvAccess(chkpPointer);

	chkpTable[idx].updateBuffer(pointer,msg);
	
	if(isChkpting){
		recvCount++;
  
		recvChkpCount++;
		if(recvChkpCount==2)
		{
		  CpvAccess(chkpNum)++;
		  recvChkpCount=0;
		}

		if(recvCount == 2){
		  if (where == CkCheckPoint_inMEM) {
			contribute(CkCallback(CkReductionTarget(CkMemCheckPT, cpFinish), thisProxy[cpStarter]));
		  }
		  else if (where == CkCheckPoint_inDISK) {
			// another barrier for finalize the writing using fsync
			contribute(CkCallback(CkReductionTarget(CkMemCheckPT, syncFiles), thisgroup));
		  }
		  else
			CmiAbort("Unknown checkpoint scheme");
		  recvCount = 0;
		}
	}
#endif
}

// don't handle array elements
static inline void _handleProcData(PUP::er &p)
{
    // save readonlys, and callback BTW
    CkPupROData(p);

    // save mainchares 
    if(CkMyPe()==0) CkPupMainChareData(p, (CkArgMsg*)NULL);
	
#ifndef CMK_CHARE_USE_PTR
    // save non-migratable chare
    CkPupChareData(p);
#endif

    // save groups into Groups.dat
    CkPupGroupData(p);

    // save nodegroups into NodeGroups.dat
    if(CkMyRank()==0) CkPupNodeGroupData(p);
}

void CkMemCheckPT::sendProcData()
{
  // find out size of buffer
  size_t size;
  {
    PUP::sizer p;
    _handleProcData(p);
    size = p.size();
  }
  size_t packSize = size;
  CkProcCheckPTMessage *msg = new (packSize, 0) CkProcCheckPTMessage;
  DEBUGF("[%d] CkMemCheckPT::sendProcData - size: %ld to %d\n", CkMyPe(), (CmiUInt8)size, ChkptOnPe(CkMyPe()));
  {
    PUP::toMem p(msg->packData);
    _handleProcData(p);
  }
  msg->pe = CkMyPe();
  msg->len = size;
  msg->reportPe = cpStarter;  //in case other processor isn't in checkpoint mode
  thisProxy[ChkptOnPe(CkMyPe())].recvProcData(msg);
}

void CkMemCheckPT::recvProcData(CkProcCheckPTMessage *msg)
{
  int pointer = CpvAccess(chkpPointer)^1;
  if (CpvAccess(procChkptBuf)[pointer]) delete CpvAccess(procChkptBuf)[pointer];
  CpvAccess(procChkptBuf)[pointer] = msg;
  DEBUGF("[%d] CkMemCheckPT::recvProcData report to %d\n", CkMyPe(), msg->reportPe);
  
  recvChkpCount++;
  if(recvChkpCount==2)
  {
    CpvAccess(chkpNum)++;
    recvChkpCount=0;
  }

  contribute(CkCallback(CkReductionTarget(CkMemCheckPT, cpFinish), thisProxy[msg->reportPe]));
}

// ArrayElement call this function to give us the checkpointed data
void CkMemCheckPT::recvData(CkArrayCheckPTMessage *msg)
{
  int len = ckTable.size();
  int idx;
  for (idx=0; idx<len; idx++) {
    CkCheckPTInfo *entry = ckTable[idx];
    if (msg->locMgr == entry->locMgr && msg->index == entry->index) break;
  }
  CkAssert(idx < len);
  bool isChkpting = msg->cp_flag;
  ckTable[idx]->updateBuffer(msg);
  if (isChkpting) {
      // all my array elements have returned their inmem data
      // inform starter processor that I am done.
    recvCount ++;
    if (recvCount == ckTable.size()) {
      if (where == CkCheckPoint_inMEM) {
        contribute(CkCallback(CkReductionTarget(CkMemCheckPT, cpFinish), thisProxy[cpStarter]));
      }
      else if (where == CkCheckPoint_inDISK) {
        // another barrier for finalize the writing using fsync
        contribute(CkCallback(CkReductionTarget(CkMemCheckPT, syncFiles), thisgroup));
      }
      else
        CmiAbort("Unknown checkpoint scheme");
      recvCount = 0;
    } 
  }
}

// only used in disk checkpointing
void CkMemCheckPT::syncFiles()
{
#if CMK_HAS_SYNC && ! CMK_DISABLE_SYNC
  if(system("sync")< 0)
  {
    CmiAbort("sync file failed");
  }
#endif
  contribute(CkCallback(CkReductionTarget(CkMemCheckPT, cpFinish), thisProxy[cpStarter]));
}

// only is called on cpStarter when checkpoint is done
void CkMemCheckPT::cpFinish()
{
  CmiAssert(CkMyPe() == cpStarter);
  peCount++;
    // now that all processors have finished, activate callback
  if (peCount == 2) 
{
    if (!quietModeRequested)
      CkPrintf("CharmFT> Checkpoint finished in %f seconds.\n", CmiWallTimer()-startTime);
    cpCallback.send();
    peCount = 0;
    thisProxy.report();
  }
}

// for debugging, report checkpoint info
void CkMemCheckPT::report()
{
  inCheckpointing = false;
#if !CMK_CHKP_ALL
  int objsize = 0;
  int len = ckTable.size();
  for (int i=0; i<len; i++) {
    CkCheckPTInfo *entry = ckTable[i];
    CmiAssert(entry);
    objsize += entry->getSize();
  }
#else
  //this checkpoint has finished, update the pointer
  CpvAccess(chkpPointer) = CpvAccess(chkpPointer)^1;
  if(CkMyPe()==0)
    DEBUGF("[%d] Checkpointed processor data of size: %zu\n", CkMyPe(), CpvAccess(procChkptBuf)[CpvAccess(chkpPointer)]->len);
#endif
}

/*****************************************************************************
			RESTART Procedure
*****************************************************************************/

// master processor of two buddies
inline bool CkMemCheckPT::isMaster(int buddype)
{
#if 0
  int mype = CkMyPe();
//DEBUGF("ismaster: %d %d\n", pe, mype);
  if (CkNumPes() - totalFailed() == 2) {
    return mype > buddype;
  }
  for (int i=1; i<CkNumPes(); i++) {
    int me = (buddype+i)%CkNumPes();
    if (isFailed(me)) continue;
    if (me == mype) return 1;
    else return 0;
  }
  return 0;
#else
    // smaller one
  int mype = CkMyPe();
//DEBUGF("ismaster: %d %d\n", pe, mype);
  if (CkNumPes() - totalFailed() == 2) {
    return mype < buddype;
  }
#if NODE_CHECKPOINT
  int pe_per_node = CmiNumPesOnPhysicalNode(CmiPhysicalNodeID(mype));
  for (int i=pe_per_node; i<CkNumPes(); i+=pe_per_node) {
#else
  for (int i=1; i<CkNumPes(); i++) {
#endif
    int me = (mype+i)%CkNumPes();
    if (isFailed(me)) continue;
    if (me == buddype) return 1;
    else return 0;
  }
  return 0;
#endif
}



#if 0
// helper class to pup all elements that belong to same ckLocMgr
class ElementDestroyer : public CkLocIterator {
private:
        CkLocMgr *locMgr;
public:
        ElementDestroyer(CkLocMgr* mgr_):locMgr(mgr_){};
        void addLocation(CkLocation &loc) {
		CkArrayIndex idx=loc.getIndex();
		DEBUGF("[%d] destroy: ", CkMyPe()); idx.print();
		loc.destroy();
        }
};
#endif

// restore the bitmap vector for LB
void CkMemCheckPT::resetLB(int diepe)
{
#if CMK_LBDB_ON
  int i;
  std::vector<char> bitmap(CkNumPes());
  // set processor available bitmap
  get_avail_vector(bitmap.data());

  for (i=0; i<failedPes.size(); i++)
    bitmap[failedPes[i]] = 0; 
  bitmap[diepe] = 0;

#if CK_NO_PROC_POOL
  set_avail_vector(bitmap.data());
#endif

  // if I am the crashed pe, rebuild my failedPEs array
  if (CkMyNode() == diepe)
    for (i=0; i<CkNumPes(); i++) 
      if (bitmap[i]==0) failed(i);
#endif
}

// in case when failedPe dies, everybody go through its checkpoint table:
// destroy all array elements
// recover lost buddies
// reconstruct all array elements from check point data
// called on all processors
void CkMemCheckPT::restart(int diePe)
{
#if CMK_MEM_CHECKPOINT
  double curTime = CmiWallTimer();
  if (CkMyPe() == diePe)
    DEBUGF("[%d] Process data restored in %f seconds\n", CkMyPe(), curTime - startTime);
  stage = (char*)"resetLB";
  startTime = curTime;
  if (CkMyPe() == diePe)
    DEBUGF("[%d] CkMemCheckPT ----- restart.\n",CkMyPe());

#if CK_NO_PROC_POOL
  failed(diePe);	// add into the list of failed pes
#endif
  thisFailedPe = diePe;

  if (CkMyPe() == diePe) CmiAssert(ckTable.empty());

  inRestarting = true;
                                                                                
  // disable load balancer's barrier
  if (CkMyPe() != diePe) resetLB(diePe);

  CKLOCMGR_LOOP(mgr->startInserting(););

  //thisProxy[0].quiescence(CkCallback(CkIndex_CkMemCheckPT::removeArrayElements(), thisProxy));
  barrier(CkCallback(CkIndex_CkMemCheckPT::removeArrayElements(), thisProxy));
/*
  if (CkMyPe() == 0)
    CkStartQD(CkCallback(CkIndex_CkMemCheckPT::removeArrayElements(), thisProxy));
*/
#endif
}

// loally remove all array elements
void CkMemCheckPT::removeArrayElements()
{
#if CMK_MEM_CHECKPOINT
  int len = ckTable.size();
  double curTime = CmiWallTimer();
  if (CkMyPe() == thisFailedPe) 
    DEBUGF("[%d] CkMemCheckPT ----- %s len:%d in %f seconds.\n",CkMyPe(),stage,len,curTime-startTime);
  stage = (char*)"removeArrayElements";
  startTime = curTime;

  if (cpCallback.isInvalid()) CkAbort("Didn't set restart callback\n");;
  if (CkMyPe()==thisFailedPe) CmiAssert(len == 0);

  // get rid of all buffering and remote recs
  // including destroying all array elements
#if CK_NO_PROC_POOL  
	CKLOCMGR_LOOP(mgr->flushAllRecs(););
#else
	CKLOCMGR_LOOP(mgr->flushLocalRecs(););
#endif
//  CKLOCMGR_LOOP(ElementDestroyer chk(mgr); mgr->iterate(chk););

  //thisProxy[0].quiescence(CkCallback(CkIndex_CkMemCheckPT::resetReductionMgr(), thisProxy));
  barrier(CkCallback(CkIndex_CkMemCheckPT::resetReductionMgr(), thisProxy));
#endif
}

// flush state in reduction manager
void CkMemCheckPT::resetReductionMgr()
{
  //DEBUGF("[%d] CkMemCheckPT ----- resetReductionMgr\n",CkMyPe());
  int numGroups = CkpvAccess(_groupIDTable)->size();
  for(int i=0;i<numGroups;i++) {
    CkGroupID gID = (*CkpvAccess(_groupIDTable))[i];
    IrrGroup *obj = CkpvAccess(_groupTable)->find(gID).getObj();
    obj->flushStates();
    obj->ckJustMigrated();
  }
  // reset again
  //CpvAccess(_qd)->flushStates();

#if 1
  //thisProxy[0].quiescence(CkCallback(CkIndex_CkMemCheckPT::recoverBuddies(), thisProxy));
  barrier(CkCallback(CkIndex_CkMemCheckPT::recoverBuddies(), thisProxy));
#else
  if (CkMyPe() == 0)
    CkStartQD(CkCallback(CkIndex_CkMemCheckPT::recoverBuddies(), thisProxy));
#endif
}

// recover the lost buddies
void CkMemCheckPT::recoverBuddies()
{
  int idx;
  int len = ckTable.size();
  // ready to flush reduction manager
  // cannot be CkMemCheckPT::restart because destroy will modify states
  double curTime = CmiWallTimer();
  if (CkMyPe() == thisFailedPe)
    DEBUGF("[%d] CkMemCheckPT ----- %s  in %f seconds\n",CkMyPe(), stage, curTime-startTime);
  stage = (char *)"recoverBuddies";
  if (CkMyPe() == thisFailedPe)
    DEBUGF("[%d] CkMemCheckPT ----- %s  starts at %f\n",CkMyPe(), stage, curTime);
  startTime = curTime;

  // recover buddies
  expectCount = 0;
#if !CMK_CHKP_ALL
  for (idx=0; idx<len; idx++) {
    CkCheckPTInfo *entry = ckTable[idx];
    if (entry->pNo == thisFailedPe) {
#if CK_NO_PROC_POOL
      // find a new buddy
      int budPe = BuddyPE(CkMyPe());
#else
      int budPe = thisFailedPe;
#endif
      CkArrayCheckPTMessage *msg = entry->getCopy();
      msg->bud1 = budPe;
      msg->bud2 = CkMyPe();
      msg->cp_flag = 0;            // not checkpointing
      thisProxy[budPe].recoverEntry(msg);
      expectCount ++;
    }
  }
#else
  //send to failed pe
  if(CkMyPe()!=thisFailedPe&&chkpTable[1].bud1==thisFailedPe){
#if CK_NO_PROC_POOL
      // find a new buddy
      int budPe = BuddyPE(CkMyPe());
#else
      int budPe = thisFailedPe;
#endif
      CkArrayCheckPTMessage *msg = chkpTable[1].getCopy(CpvAccess(chkpPointer));
      DEBUGF("[%d] got message for crashed pe %d\n",CkMyPe(),thisFailedPe);
	  msg->cp_flag = 0;            // not checkpointing
      msg->bud1 = budPe;
      msg->bud2 = CkMyPe();
      thisProxy[budPe].recoverEntry(msg);
      expectCount ++;
  }
#endif

#if CMK_CHKP_USE_REDN
  if (expectCount == 0) {
    contribute(CkCallback(CkReductionTarget(CkMemCheckPT, recoverArrayElements), thisProxy));
    //thisProxy[0].quiescence(CkCallback(CkIndex_CkMemCheckPT::recoverArrayElements(), thisProxy));
  }
#else
  if (CkMyPe() == 0) {
    CkStartQD(CkCallback(CkIndex_CkMemCheckPT::recoverArrayElements(), thisProxy));
  }
#endif

  //DEBUGF("[%d] CkMemCheckPT ----- recoverBuddies done  in %f seconds\n",CkMyPe(), curTime-startTime);
}

void CkMemCheckPT::gotData()
{
  ackCount ++;
  if (ackCount == expectCount) {
    ackCount = 0;
    expectCount = -1;
#if CMK_CHKP_USE_REDN
    //thisProxy[0].quiescence(CkCallback(CkIndex_CkMemCheckPT::recoverArrayElements(), thisProxy));
    contribute(CkCallback(CkReductionTarget(CkMemCheckPT, recoverArrayElements), thisProxy));
#endif
  }
}

void CkMemCheckPT::updateLocations(int n, CkGroupID *g, CkArrayIndex *idx, CmiUInt8 *id, int nowOnPe)
{

  for (int i=0; i<n; i++) {
    CkLocMgr *mgr = CProxy_CkLocMgr(g[i]).ckLocalBranch();
    mgr->updateLocation(idx[i], id[i], nowOnPe);
  }
	thisProxy[nowOnPe].gotReply();
}

// restore array elements
void CkMemCheckPT::recoverArrayElements()
{
  double curTime = CmiWallTimer();
  int len = ckTable.size();
  //DEBUGF("[%d] CkMemCheckPT ----- %s len: %d in %f seconds \n",CkMyPe(), stage, len, curTime-startTime);
  stage = (char *)"recoverArrayElements";
  if (CkMyPe() == thisFailedPe)
    DEBUGF("[%d] CkMemCheckPT ----- %s starts at %f \n",CkMyPe(), stage, curTime);
  startTime = curTime;
 int flag = 0;
  // recover all array elements
  int count = 0;

#if STREAMING_INFORMHOME && CK_NO_PROC_POOL
  std::vector<CkGroupID> * gmap = new std::vector<CkGroupID>[CkNumPes()];
  std::vector<CkArrayIndex> * imap = new std::vector<CkArrayIndex>[CkNumPes()];
#endif

#if !CMK_CHKP_ALL
  for (int idx=0; idx<len; idx++)
  {
    CkCheckPTInfo *entry = ckTable[idx];
#if CK_NO_PROC_POOL
    // the bigger one will do 
//    if (CkMyPe() < entry->pNo) continue;
    if (!isMaster(entry->pNo)) continue;
#else
    // smaller one do it, which has the original object
    if (CkMyPe() == entry->pNo+1 || 
        CkMyPe()+CkNumPes() == entry->pNo+1) continue;
#endif
//DEBUGF("[%d] restore idx:%d aid:%d loc:%d ", CkMyPe(), idx, (CkGroupID)(entry->aid), entry->locMgr); entry->index.print();

    entry->updateBuddy(CkMyPe(), entry->pNo);
    CkArrayCheckPTMessage *msg = entry->getCopy();
    // gzheng
    //thisProxy[CkMyPe()].inmem_restore(msg);
    inmem_restore(msg);
#if STREAMING_INFORMHOME && CK_NO_PROC_POOL
    CkLocMgr *mgr = CProxy_CkLocMgr(msg->locMgr).ckLocalBranch();
    int homePe = mgr->homePe(msg->index);
    if (homePe != CkMyPe()) {
      gmap[homePe].push_back(msg->locMgr);
      imap[homePe].push_back(msg->index);
      CkAbort("Missing element IDs");
    }
#endif
    CkFreeMsg(msg);
    count ++;
  }
#else
	CkArrayCheckPTMessage * msg = chkpTable[0].getCopy(CpvAccess(chkpPointer));
#if STREAMING_INFORMHOME && CK_NO_PROC_POOL
	recoverAll(msg,gmap,imap);
#else
	recoverAll(msg);
#endif
    CkFreeMsg(msg);
#endif
  curTime = CmiWallTimer();
  if (CkMyPe() == thisFailedPe)
	DEBUGF("[%d] CkMemCheckPT ----- %s streams at %f \n",CkMyPe(), stage, curTime);
#if STREAMING_INFORMHOME && CK_NO_PROC_POOL
  for (int i=0; i<CkNumPes(); i++) {
    if (gmap[i].size() && i!=CkMyPe()&& i==thisFailedPe) {
      thisProxy[i].updateLocations(gmap[i].size(), gmap[i].data(), imap[i].data(), CkMyPe());
	flag++;	
	  }
  }
  delete [] imap;
  delete [] gmap;
#endif
  DEBUGF("[%d] recoverArrayElements restore %d objects\n", CkMyPe(), count);

  CKLOCMGR_LOOP(mgr->doneInserting(););

  // _crashedNode = -1;
  CpvAccess(_crashedNode) = -1;
  inRestarting = false;
#if (!STREAMING_INFORMHOME && CK_NO_PROC_POOL) || !CMK_CHKP_USE_REDN
  if (CkMyPe() == 0)
    CkStartQD(CkCallback(CkIndex_CkMemCheckPT::finishUp(), thisProxy));
#else
if(flag == 0)
{
    contribute(CkCallback(CkReductionTarget(CkMemCheckPT, finishUp), thisProxy));
}
#endif
}

void CkMemCheckPT::gotReply(){
    contribute(CkCallback(CkReductionTarget(CkMemCheckPT, finishUp), thisProxy));
}

void CkMemCheckPT::recoverAll(CkArrayCheckPTMessage * msg,std::vector<CkGroupID> * gmap, std::vector<CkArrayIndex> * imap){
#if CMK_CHKP_ALL
	PUP::fromMem p(msg->packData, PUP::er::IS_CHECKPOINT);
	int numElements = 0;
	p|numElements;
	if(p.isUnpacking()){
		for(int i=0;i<numElements;i++){
			CkGroupID gID;
			CkArrayIndex idx;
                        CmiUInt8 id;
			p|gID;
			p|idx;
                        p|id;
			CkLocMgr * mgr = (CkLocMgr *)CkpvAccess(_groupTable)->find(gID).getObj();
    			int homePe = mgr->homePe(idx);
#if !STREAMING_INFORMHOME && CK_NO_PROC_POOL
			mgr->resume(idx, id, p, true, true);
#else
			mgr->resume(idx, id, p, false, true);
#endif
#if STREAMING_INFORMHOME && CK_NO_PROC_POOL
			homePe = mgr->homePe(idx);
			if (homePe != CkMyPe()) {
			  gmap[homePe].push_back(gID);
			  imap[homePe].push_back(idx);
			}
#endif
		}
	}
	if(CkMyPe()==thisFailedPe)
	  DEBUGF("recover all ends\n");
#endif
}

static double restartT;

// on every processor
// turn load balancer back on
void CkMemCheckPT::finishUp()
{
  //DEBUGF("[%d] CkMemCheckPT::finishUp\n", CkMyPe());
  //CKLOCMGR_LOOP(mgr->doneInserting(););
  recvCount = peCount = 0;
  recvChkpCount = 0;
  if (CkMyPe() == thisFailedPe)
  {
       DEBUGF("[%d] CkMemCheckPT ----- %s in %f seconds, callback triggered\n",CkMyPe(), stage, CmiWallTimer()-startTime);
       //CkStartQD(cpCallback);
       cpCallback.send();
       if (!quietModeRequested)
         CkPrintf("CharmFT> Restart finished in %f seconds.\n", CkWallTimer()-restartT);
  }
#if CMK_CONVERSE_MPI	
  if (CmiMyPe() == BuddyPE(thisFailedPe)) {
    lastPingTime = CmiWallTimer();
    CcdCallOnCondition(CcdPERIODIC_5s,(CcdVoidFn)pingCheckHandler,NULL);
  }
#endif

#if CK_NO_PROC_POOL
#if NODE_CHECKPOINT
  int numnodes = CmiNumPhysicalNodes();
#else
  int numnodes = CkNumPes();
#endif
  if (numnodes-totalFailed() <=2) {
    if (CkMyPe()==0 && !quietModeRequested)
      CkPrintf("CharmFT> Warning: CkMemCheckPT disabled!\n");
    _memChkptOn = false;
  }
#endif
}

// called only on 0
void CkMemCheckPT::quiescence(CkCallback &&cb)
{
  static int pe_count = 0;
  pe_count ++;
  CmiAssert(CkMyPe() == 0);
  //DEBUGF("quiescence %d %d\n", pe_count, CkNumPes());
  if (pe_count == CkNumPes()) {
    pe_count = 0;
    cb.send();
  }
}

// User callable function - to start a checkpoint
// callback cb is used to pass control back
void CkStartMemCheckpoint(CkCallback &cb)
{
#if CMK_MEM_CHECKPOINT
  if(cb.isInvalid()) 
    CkAbort("callback after checkpoint is not set properly");
  if (!_memChkptOn) {
    if (!quietModeRequested)
      CkPrintf("CharmFT> Warning: In-memory checkpointing has been disabled!\n");
    cb.send();
    return;
  }
  if (CkInRestarting()) {
      // trying to checkpointing during restart
    cb.send();
    return;
  }
    // store user callback and user data
  CkMemCheckPT::cpCallback = cb;

    // broadcast to start check pointing
  CProxy_CkMemCheckPT checkptMgr(ckCheckPTGroupID);
  checkptMgr.doItNow(CkMyPe(), cb);
#else
  // when mem checkpoint is disabled, invike cb immediately
  if (!quietModeRequested)
    CkPrintf("CharmFT> Warning: In-memory checkpointing is unavailable! Please build Charm++ with the syncft option.\n");
  cb.send();
#endif
}

void CkRestartCheckPoint(int diePe)
{
  DEBUGF("CkRestartCheckPoint  CkMemCheckPT GID:%d at time %f\n", ckCheckPTGroupID.idx, CkWallTimer());
  CProxy_CkMemCheckPT checkptMgr(ckCheckPTGroupID);
  // broadcast
  checkptMgr.restart(diePe);
}

static int _diePE = -1;

// callback function used locally by ccs handler
static void CkRestartCheckPointCallback(void *ignore, void *msg)
{
  DEBUGF("[%d] CkRestartCheckPointCallback activated for diePe: %d at %f\n", CkMyPe(), _diePE, CkWallTimer());
  CkRestartCheckPoint(_diePE);
}

// Converse function handles
static int askPhaseHandlerIdx;
static int recvPhaseHandlerIdx;
static int askProcDataHandlerIdx;
static int restartBcastHandlerIdx;
static int recoverProcDataHandlerIdx;
static int restartBeginHandlerIdx;
static int notifyHandlerIdx;
static int reportChkpSeqHandlerIdx;
static int getChkpSeqHandlerIdx;

// called on crashed PE
static void restartBeginHandler(char *msg)
{
  CmiFree(msg);
#if CMK_MEM_CHECKPOINT
#if CMK_USE_BARRIER
	if(CkMyPe()!=_diePE){
		DEBUGF("restart begin on %d\n",CkMyPe());
  		char *restartmsg = (char*)CmiAlloc(CmiMsgHeaderSizeBytes);
  		CmiSetHandler(restartmsg, restartBeginHandlerIdx);
  		CmiSyncSendAndFree(_diePE, CmiMsgHeaderSizeBytes, (char *)restartmsg);
	}else{
	DEBUGF("[%d] restartBeginHandler cur_restart_phase=%d _diePE:%d at %f.\n", CkMyPe(), CpvAccess(_curRestartPhase), _diePE, CkWallTimer());
    	CkRestartCheckPointCallback(NULL, NULL);
	}
#else
  static int count = 0;
  CmiAssert(CkMyPe() == _diePE);
  count ++;
  if (count == CkNumPes()) {
    CkRestartCheckPointCallback(NULL, NULL);
    count = 0;
  }
#endif
#endif
}

extern void _discard_charm_message();
extern void _resume_charm_message();

static void * doNothingMsg(size_t * size, void * data, void ** remote, int count){
	return data;
}

static void * minChkpNumMsg(size_t * size, void * data, void ** remote, int count)
{
  int minNum = *(int *)((char *)data+CmiMsgHeaderSizeBytes);
  for(int i = 0; i < count;i++)
  {
    int num = *(int *)((char *)(remote[i])+CmiMsgHeaderSizeBytes);
    if(num != -1 && (num < minNum || minNum == -1))
    {
      minNum = num;
    }
  }
  *(int *)((char *)data+CmiMsgHeaderSizeBytes) = minNum;
  return data;
}

static void restartBcastHandler(char *msg)
{
#if CMK_MEM_CHECKPOINT
  // advance phase counter
  CkMemCheckPT::inRestarting = true;
  _diePE = *(int *)(msg+CmiMsgHeaderSizeBytes);
  CpvAccess(chkpNum) = *(int *)(msg+CmiMsgHeaderSizeBytes+sizeof(int));
  CpvAccess(chkpPointer) = CpvAccess(chkpNum)%2;

  if (CkMyPe()==_diePE)
    DEBUGF("[%d] restartBcastHandler cur_restart_phase=%d _diePE:%d at %f.\n", CkMyPe(), CpvAccess(_curRestartPhase), _diePE, CkWallTimer());

  CmiFree(msg);

  _resume_charm_message();

    // reduction
  char *restartmsg = (char*)CmiAlloc(CmiMsgHeaderSizeBytes);
  CmiSetHandler(restartmsg, restartBeginHandlerIdx);
#if CMK_USE_BARRIER
	//DEBUGF("before reduce\n");
  	CmiReduce(restartmsg,CmiMsgHeaderSizeBytes,doNothingMsg);
	//DEBUGF("after reduce\n");
#else
  CmiSyncSendAndFree(_diePE, CmiMsgHeaderSizeBytes, (char *)restartmsg);
#endif 
 checkpointed = false;
#endif
}

extern void _initDone();

// called on crashed processor
static void recoverProcDataHandler(char *msg)
{
#if CMK_MEM_CHECKPOINT
   int i;
   envelope *env = (envelope *)msg;
   CkUnpackMessage(&env);
   CkProcCheckPTMessage* procMsg = (CkProcCheckPTMessage *)(EnvToUsr(env));
   CpvAccess(chkpNum) = procMsg->pointer;
   CpvAccess(chkpPointer) = CpvAccess(chkpNum)%2;
   CpvAccess(_curRestartPhase) = procMsg->cur_restart_phase;
   DEBUGF("[%d] ----- recoverProcDataHandler  cur_restart_phase:%d at time: %f\n", CkMyPe(), CpvAccess(_curRestartPhase), CkWallTimer());
   PUP::fromMem p(procMsg->packData, PUP::er::IS_CHECKPOINT);
   _handleProcData(p);

   CProxy_CkMemCheckPT(ckCheckPTGroupID).ckLocalBranch()->resetLB(CkMyPe());
   // gzheng
   CKLOCMGR_LOOP(mgr->startInserting(););

   char *reqmsg = (char*)CmiAlloc(CmiMsgHeaderSizeBytes+sizeof(int)*2);
   *(int *)(reqmsg+CmiMsgHeaderSizeBytes) = CkMyPe();
   *(int *)(reqmsg+CmiMsgHeaderSizeBytes+sizeof(int)) = CpvAccess(chkpNum);
   CmiSetHandler(reqmsg, restartBcastHandlerIdx);
   CmiSyncBroadcastAllAndFree(CmiMsgHeaderSizeBytes+sizeof(int)*2, (char *)reqmsg);

   _initDone();
//   CpvAccess(_qd)->flushStates();
   DEBUGF("[%d] ----- recoverProcDataHandler  done at %f\n", CkMyPe(), CkWallTimer());
#endif
}

// called on its backup processor
// get backup message buffer and sent to crashed processor
static void askProcDataHandler(char *msg)
{
#if CMK_MEM_CHECKPOINT
    int diePe = *(int *)(msg+CmiMsgHeaderSizeBytes);
    CpvAccess(chkpNum) = *(int *)(msg+CmiMsgHeaderSizeBytes+sizeof(int));
    CmiFree(msg);
    int pointer = CpvAccess(chkpNum)%2;
    CpvAccess(chkpPointer) = pointer;
    DEBUGF("[%d] askProcDataHandler called with '%d' cur_restart_phase:%d at time %f.\n",CmiMyPe(),diePe, CpvAccess(_curRestartPhase), CkWallTimer());
    if (CpvAccess(procChkptBuf)[pointer] == NULL)  {
      CkPrintf("[%d] no checkpoint found for processor %d. This could be due to a crash before the first checkpointing.\n", CkMyPe(), diePe);
      CkAbort("no checkpoint found");
    }
    CpvAccess(procChkptBuf)[pointer]->pointer = CpvAccess(chkpNum);
    envelope *env = (envelope *)(UsrToEnv(CpvAccess(procChkptBuf)[pointer]));
    CmiAssert(CpvAccess(procChkptBuf)[pointer]->pe == diePe);

    CpvAccess(procChkptBuf)[pointer]->cur_restart_phase = CpvAccess(_curRestartPhase);
    CkPackMessage(&env);
    CmiSetHandler(env, recoverProcDataHandlerIdx);
    CmiSyncSendAndFree(CpvAccess(procChkptBuf)[pointer]->pe, env->getTotalsize(), (char *)env);
    CpvAccess(procChkptBuf)[pointer] = NULL;
    DEBUGF("[%d] askProcDataHandler called with '%d' cur_restart_phase:%d done at time %f.\n",CmiMyPe(),diePe, CpvAccess(_curRestartPhase), CkWallTimer());
#endif
}

// called on PE 0
void qd_callback(void *m)
{
   DEBUGF("[%d] callback after QD for crashed node: %d. at %lf\n", CkMyPe(), CpvAccess(_crashedNode),CmiWallTimer());
   CkFreeMsg(m);
   //broadcast to collect the checkpoint number
   char *msg = (char*)CmiAlloc(CmiMsgHeaderSizeBytes);
   CmiSetHandler(msg,reportChkpSeqHandlerIdx);
   CmiSyncBroadcastAllAndFree(CmiMsgHeaderSizeBytes, (char *)msg);
}

static void reportChkpSeqHandler(char * m)
{
  CmiFree(m);
  CmiResetGlobalReduceSeqID();
  CmiResetGlobalNodeReduceSeqID();
  char *msg = (char*)CmiAlloc(CmiMsgHeaderSizeBytes+sizeof(int));
  int num = CpvAccess(chkpNum);
  if(CkMyNode() == CpvAccess(_crashedNode))
  {
    num = -1;
  }
  *(int *)(msg+CmiMsgHeaderSizeBytes) = num;
  CmiSetHandler(msg,getChkpSeqHandlerIdx);
  CmiReduce(msg,CmiMsgHeaderSizeBytes+sizeof(int),minChkpNumMsg);
}

static void getChkpSeqHandler(char * m)
{
  CpvAccess(chkpNum) = *(int *)(m+CmiMsgHeaderSizeBytes);
  CpvAccess(chkpPointer) = CpvAccess(chkpNum)%2;
  CmiFree(m);
#ifdef CMK_SMP
   for(int i=0;i<CmiMyNodeSize();i++){
    char *msg = (char*)CmiAlloc(CmiMsgHeaderSizeBytes+sizeof(int)*2);
    *(int *)(msg+CmiMsgHeaderSizeBytes) =CpvAccess(_crashedNode);
    *(int *)(msg+CmiMsgHeaderSizeBytes+sizeof(int)) =CpvAccess(chkpNum);
    CmiSetHandler(msg, askProcDataHandlerIdx);
    int pe = ChkptOnPe(CpvAccess(_crashedNode)*CmiMyNodeSize()+i);    // FIXME ?
    CmiSyncSendAndFree(pe, CmiMsgHeaderSizeBytes+sizeof(int)*2, (char *)msg);
   }
   return;
#endif
   char *msg = (char*)CmiAlloc(CmiMsgHeaderSizeBytes+sizeof(int)*2);
   *(int *)(msg+CmiMsgHeaderSizeBytes) = CpvAccess(_crashedNode);
    *(int *)(msg+CmiMsgHeaderSizeBytes+sizeof(int)) =CpvAccess(chkpNum);
   // cur_restart_phase = RESTART_PHASE_MAX;             // big enough to get it processed, moved to machine.C
   CmiSetHandler(msg, askProcDataHandlerIdx);
   int pe = ChkptOnPe(CpvAccess(_crashedNode));
   CmiSyncSendAndFree(pe, CmiMsgHeaderSizeBytes+sizeof(int)*2, (char *)msg);
}

static void changePhaseHandler(char *msg){
#if CMK_MEM_CHECKPOINT
  CpvAccess(_curRestartPhase)--;
  if(CkMyNode()==CpvAccess(_crashedNode)){
    if(CmiMyRank()==0){
      CkCallback cb(qd_callback);//safe to send now?
      CkStartQD(cb);
      DEBUGF("crash_node:%d\n",CpvAccess( _crashedNode));
    }
  }
#endif  
}

// on crashed node
void CkMemRestart(const char *dummy, CkArgMsg *args)
{
#if CMK_MEM_CHECKPOINT
   _diePE = CmiMyNode();
   CkMemCheckPT::startTime = restartT = CmiWallTimer();
   DEBUGF("[%d] I am restarting  cur_restart_phase:%d at time: %f\n",CmiMyPe(), CpvAccess(_curRestartPhase), CkMemCheckPT::startTime);
   CkMemCheckPT::inRestarting = true;

  CpvAccess( _crashedNode )= CmiMyNode();
	
  _discard_charm_message();
    restartT = CmiWallTimer();
   DEBUGF("[%d] I am restarting  cur_restart_phase:%d discard charm message at time: %f\n",CmiMyPe(), CpvAccess(_curRestartPhase), restartT);
  
  //now safe to change the phase handler,braodcast every
  if(CmiNumPartitions()>1){
    char *msg = (char*)CmiAlloc(CmiMsgHeaderSizeBytes);
    CmiSetHandler(msg, changePhaseHandlerIdx);
    CmiSyncBroadcastAllAndFree(CmiMsgHeaderSizeBytes, (char *)msg);
  }else{  
   /*char *msg = (char*)CmiAlloc(CmiMsgHeaderSizeBytes+sizeof(int));
   *(int *)(msg+CmiMsgHeaderSizeBytes) = CpvAccess(_crashedNode);
   // cur_restart_phase = RESTART_PHASE_MAX;             // big enough to get it processed, moved to machine.C
   CmiSetHandler(msg, askProcDataHandlerIdx);
   int pe = ChkptOnPe(CpvAccess(_crashedNode));
   CmiSyncSendAndFree(pe, CmiMsgHeaderSizeBytes+sizeof(int), (char *)msg);*/
   char *msg = (char*)CmiAlloc(CmiMsgHeaderSizeBytes);
   CmiSetHandler(msg,reportChkpSeqHandlerIdx);
   CmiSyncBroadcastAllAndFree(CmiMsgHeaderSizeBytes, (char *)msg);
  }
#else
   CmiAbort("Fault tolerance is not support, rebuild charm++ with 'syncft' option");
#endif
}

// can be called in other files
// return true if it is in restarting
int CkInRestarting()
{
#if CMK_MEM_CHECKPOINT
  if (CpvAccess( _crashedNode)!=-1) return 1;
  // gzheng
  //if (cur_restart_phase == RESTART_PHASE_MAX || cur_restart_phase == 0) return 1;
  //return CProxy_CkMemCheckPT(ckCheckPTGroupID).ckLocalBranch()->inRestarting;
  return (int)CkMemCheckPT::inRestarting;
#else
  return 0;
#endif
}

static int CkInCheckpointing()
{
  return CkMemCheckPT::inCheckpointing;
}

void CkSetInLdb(){
#if CMK_MEM_CHECKPOINT
	CkMemCheckPT::inLoadbalancing = true;
#endif
}

int CkInLdb(){
#if CMK_MEM_CHECKPOINT
	return (int)CkMemCheckPT::inLoadbalancing;
#endif
	return 0;
}

void CkResetInLdb(){
#if CMK_MEM_CHECKPOINT
	CkMemCheckPT::inLoadbalancing = false;
#endif
}

/*****************************************************************************
                module initialization
*****************************************************************************/

static int arg_where = CkCheckPoint_inMEM;

#if CMK_MEM_CHECKPOINT
void init_memcheckpt(char **argv)
{
    if (CmiGetArgFlagDesc(argv, "+ftc_disk", "Double-disk Checkpointing")) {
      arg_where = CkCheckPoint_inDISK;
    }

	// initiliazing _crashedNode variable
	CpvInitialize(int, _crashedNode);
	CpvAccess(_crashedNode) = -1;

}
#endif

class CkMemCheckPTInit: public Chare {
public:
  CkMemCheckPTInit(CkArgMsg *m) {
    delete m;
#if CMK_MEM_CHECKPOINT
    if (!quietModeRequested) {
      if (arg_where == CkCheckPoint_inDISK)
        CkPrintf("CharmFT> Activated double-disk checkpointing.\n");
      else if (arg_where == CkCheckPoint_inMEM)
        CkPrintf("CharmFT> Activated double in-memory checkpointing.\n");
    }
    ckCheckPTGroupID = CProxy_CkMemCheckPT::ckNew(arg_where);
#endif
  }
};

static void notifyHandler(char *msg)
{
#if CMK_MEM_CHECKPOINT
  CmiFree(msg);
      /* immediately increase restart phase to filter old messages */
  CpvAccess(_curRestartPhase) ++;
  CpvAccess(_qd)->flushStates();
  _discard_charm_message();

#endif
}

static void notify_crash(int node)
{
#ifdef CMK_MEM_CHECKPOINT
  CpvAccess( _crashedNode) = node;
#ifdef CMK_SMP
  for(int i=0;i<CkMyNodeSize();i++){
  	CpvAccessOther(_crashedNode,i)=node;
  }
#endif
  CmiAssert(CmiMyNode() !=CpvAccess( _crashedNode));
  CkMemCheckPT::inRestarting = true;

    // this may be in interrupt handler, send a message to reset QD
  int pe = CmiNodeFirst(CkMyNode());
  for(int i=0;i<CkMyNodeSize();i++){
  	char *msg = (char*)CmiAlloc(CmiMsgHeaderSizeBytes);
  	CmiSetHandler(msg, notifyHandlerIdx);
  	CmiSyncSendAndFree(pe+i, CmiMsgHeaderSizeBytes, (char *)msg);
  }
#endif
}

extern void (*notify_crash_fn)(int node);

#if CMK_CONVERSE_MPI
//static int pingHandlerIdx;
//static int pingCheckHandlerIdx;
//static int buddyDieHandlerIdx;
//static double lastPingTime = -1;

void mpi_restart_crashed(int pe, int rank);
int  find_spare_mpirank(int pe,int partition);

//void pingBuddy();
//void pingCheckHandler();
static void replicaDieHandler(char * msg){
#if CMK_MEM_CHECKPOINT
#if CMK_HAS_PARTITION
  //broadcast to every one in my partition
  CmiSetHandler(msg, replicaDieBcastHandlerIdx);
  CmiSyncBroadcastAllAndFree(CmiMsgHeaderSizeBytes+sizeof(int), (char *)msg);
#endif
#endif
}

static void replicaDieBcastHandler(char *msg){
#if CMK_MEM_CHECKPOINT
#if CMK_HAS_PARTITION
  int diePe = *(int *)(msg+CmiMsgHeaderSizeBytes);
  int partition = *(int *)(msg+CmiMsgHeaderSizeBytes+sizeof(int));
  find_spare_mpirank(diePe,partition);
  CmiFree(msg);
#endif
#endif
}

void buddyDieHandler(char *msg)
{
#if CMK_MEM_CHECKPOINT
   // notify
   int diepe = *(int *)(msg+CmiMsgHeaderSizeBytes);
   notify_crash(diepe);
   // send message to crash pe to let it restart
   CkMemCheckPT *obj = CProxy_CkMemCheckPT(ckCheckPTGroupID).ckLocalBranch();
   int newrank;
   newrank = find_spare_mpirank(diepe,CmiMyPartition());
   int buddy = obj->BuddyPE(CmiMyPe());
   if (buddy == diepe)  {
     mpi_restart_crashed(diepe, newrank);
     //CcdCallOnCondition(CcdPERIODIC_5s,(CcdVoidFn)pingCheckHandler,NULL);
   }
#endif
}

void pingHandler(void *msg)
{
  lastPingTime = CmiWallTimer();
  CmiFree(msg);
}

void pingCheckHandler()
{
#if CMK_MEM_CHECKPOINT
  double now = CmiWallTimer();
  if (lastPingTime > 0 && now - lastPingTime > FAIL_DET_THRESHOLD && !CkInLdb() && !CkInRestarting() && !CkInCheckpointing()) {
    // tell everyone the buddy dies
    CkMemCheckPT *obj = CProxy_CkMemCheckPT(ckCheckPTGroupID).ckLocalBranch();
    int buddy = obj->ReverseBuddyPE(CmiMyPe());
    if (!quietModeRequested)
      CkPrintf("CharmFT> Node %d detected buddy %d died at %f s (last ping: %f s). \n", CmiMyNode(), buddy, now, lastPingTime);
    /*for (int pe = 0; pe < CmiNumPes(); pe++) {
      if (obj->isFailed(pe) || pe == buddy) continue;
      char *msg = (char*)CmiAlloc(CmiMsgHeaderSizeBytes+sizeof(int));
      *(int *)(msg+CmiMsgHeaderSizeBytes) = buddy;
      CmiSetHandler(msg, buddyDieHandlerIdx);
      CmiSyncSendAndFree(pe, CmiMsgHeaderSizeBytes+sizeof(int), (char *)msg);
    }*/
    char *msg = (char*)CmiAlloc(CmiMsgHeaderSizeBytes+sizeof(int));
    *(int *)(msg+CmiMsgHeaderSizeBytes) = buddy;
    CmiSetHandler(msg, buddyDieHandlerIdx);
    CmiSyncBroadcastAllAndFree(CmiMsgHeaderSizeBytes+sizeof(int), (char *)msg);
#if CMK_HAS_PARTITION
    //notify processors in the other partition
    for(int i=0;i<CmiNumPartitions();i++){
      if(i!=CmiMyPartition()){
        char * rMsg = (char*)CmiAlloc(CmiMsgHeaderSizeBytes+sizeof(int)*2);
        *(int *)(rMsg+CmiMsgHeaderSizeBytes) = buddy;
        *(int *)(rMsg+CmiMsgHeaderSizeBytes+sizeof(int)) = CmiMyPartition();
        CmiSetHandler(rMsg, replicaDieHandlerIdx);
        CmiInterSyncSendAndFree(CkMyPe(),i,CmiMsgHeaderSizeBytes+sizeof(int),(char *)rMsg);
      }
    }
#endif
  }
  else 
    CcdCallOnCondition(CcdPERIODIC_5s,(CcdVoidFn)pingCheckHandler,NULL);
#endif
}

void pingBuddy()
{
#if CMK_MEM_CHECKPOINT
  CkMemCheckPT *obj = CProxy_CkMemCheckPT(ckCheckPTGroupID).ckLocalBranch();
  if (obj) {
    int buddy = obj->BuddyPE(CkMyPe());
//DEBUGF("[%d] pingBuddy %d\n", CmiMyPe(), buddy);
    char *msg = (char*)CmiAlloc(CmiMsgHeaderSizeBytes+sizeof(int));
    *(int *)(msg+CmiMsgHeaderSizeBytes) = CmiMyPe();
    CmiSetHandler(msg, pingHandlerIdx);
    CmiGetRestartPhase(msg) = 9999;
    CmiSyncSendAndFree(buddy, CmiMsgHeaderSizeBytes+sizeof(int), (char *)msg);
  }
  CcdCallOnCondition(CcdPERIODIC_100ms,(CcdVoidFn)pingBuddy,NULL);
#endif
}
#endif

// initproc
void CkRegisterRestartHandler( )
{
#if CMK_MEM_CHECKPOINT
  notifyHandlerIdx = CkRegisterHandler(notifyHandler);
  askProcDataHandlerIdx = CkRegisterHandler(askProcDataHandler);
  recoverProcDataHandlerIdx = CkRegisterHandler(recoverProcDataHandler);
  restartBcastHandlerIdx = CkRegisterHandler(restartBcastHandler);
  restartBeginHandlerIdx = CkRegisterHandler(restartBeginHandler);
  reportChkpSeqHandlerIdx = CkRegisterHandler(reportChkpSeqHandler);
  getChkpSeqHandlerIdx = CkRegisterHandler(getChkpSeqHandler);

#if CMK_CONVERSE_MPI
  pingHandlerIdx = CkRegisterHandler(pingHandler);
  pingCheckHandlerIdx = CkRegisterHandler(pingCheckHandler);
  buddyDieHandlerIdx = CkRegisterHandler(buddyDieHandler);
  replicaDieHandlerIdx = CkRegisterHandler(replicaDieHandler);
  replicaDieBcastHandlerIdx = CkRegisterHandler(replicaDieBcastHandler);
#endif
  changePhaseHandlerIdx = CkRegisterHandler(changePhaseHandler);

  CpvInitialize(CkProcCheckPTMessage **, procChkptBuf);
  CpvAccess(procChkptBuf) = new CkProcCheckPTMessage *[2];
  CpvAccess(procChkptBuf)[0] = NULL;
  CpvAccess(procChkptBuf)[1] = NULL;

  CpvInitialize(int, chkpPointer);
  CpvAccess(chkpPointer) = 0;
  CpvInitialize(int, chkpNum);
  CpvAccess(chkpNum) = 0;

  notify_crash_fn = notify_crash;

#if ! CMK_CONVERSE_MPI
  // print pid to kill
  //CkPrintf("[%d] PID %d \n", CkMyPe(), getpid());
  //sleep(4);
#endif
#endif
}


int CkHasCheckpoints()
{
  return (int)checkpointed;
}

/// @todo: the following definitions should be moved to a separate file containing
// structures and functions about fault tolerance strategies

/**
 *  * @brief: function for killing a process                                             
 *   */
#ifdef CMK_MEM_CHECKPOINT
#if CMK_HAS_GETPID
void killLocal(void *_dummy,double curWallTime){
        if (!quietModeRequested)
          CkPrintf("CharmFT> KillLocal called on %d at %.6lf \n",CkMyPe(),CmiWallTimer());
        if(CmiWallTimer()<killTime-1){
                CcdCallFnAfter(killLocal,NULL,(killTime-CmiWallTimer())*1000);        
        }else{ 
#if CMK_CONVERSE_MPI
				CkDieNow();
#else 
                kill(getpid(),SIGKILL);                                               
#endif
        }              
} 
#else
void killLocal(void *_dummy,double curWallTime){
  CmiAbort("kill() not supported!");
}
#endif
#endif

#ifdef CMK_MEM_CHECKPOINT
/**
 * @brief: reads the file with the kill information
 */
void readKillFile(){
        FILE *fp=fopen(killFile,"r");
        if(!fp){
                CkPrintf("CharmFT> [%d] Cannot open kill file %s\n", CkMyPe(), killFile);
                return;
        }
        int proc;
        double sec;
        while(fscanf(fp,"%d %lf",&proc,&sec)==2){
                if(proc == CkMyNode() && CkMyRank() == 0){
                        killTime = CmiWallTimer()+sec;
                        if (!quietModeRequested)
                          CkPrintf("CharmFT> Will kill %d after %.3lf s\n", CkMyPe(), sec);
                        CcdCallFnAfter(killLocal,NULL,sec*1000);
                }
        }
        fclose(fp);
}

#if ! CMK_CONVERSE_MPI
void CkDieNow()
{
#if __FAULT__
         // ignored for non-mpi version
        CmiPrintf("[%d] die now.\n", CmiMyPe());
        killTime = CmiWallTimer()+0.001;
        CcdCallFnAfter(killLocal,NULL,1);
#endif
}
#endif

#endif

#include "CkMemCheckpoint.def.h"


