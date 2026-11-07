#ifndef _CK_MEM_CHECKPT_
#define _CK_MEM_CHECKPT_

#include "CkMemCheckpoint.decl.h"

extern CkGroupID ckCheckPTGroupID;
class CkArrayCheckPTReqMessage: public CMessage_CkArrayCheckPTReqMessage {
public: 
  CkArrayCheckPTReqMessage()  {}
};

class CkArrayCheckPTMessage: public CMessage_CkArrayCheckPTMessage {
public:
	CkArrayID  aid;
	CkGroupID  locMgr;
	CkArrayIndex index;
	double *packData;
	int bud1, bud2;
	size_t len;
	bool cp_flag;          // true: from checkpoint, false: from recover
};


class CkProcCheckPTMessage: public CMessage_CkProcCheckPTMessage {
public:
	int pe;
	int reportPe;		// chkpt starter
	int failedpe;
	int cur_restart_phase;
	size_t len;
	int pointer;
	char *packData;
};

// table entry base class
class CkCheckPTInfo {
   friend class CkMemCheckPT;
protected:
   CkArrayID aid;
   CkGroupID locMgr;
   CkArrayIndex index;
   int pNo;   //another buddy
public:
   CkCheckPTInfo();
   CkCheckPTInfo(CkArrayID a, CkGroupID loc, CkArrayIndex idx, int pno):
                  aid(a), locMgr(loc), index(idx), pNo(pno)   {}
   virtual ~CkCheckPTInfo() {}
   virtual void updateBuffer(CkArrayCheckPTMessage *data) = 0;
   virtual CkArrayCheckPTMessage * getCopy() = 0;
   virtual void updateBuddy(int b1, int b2) = 0;
   virtual size_t getSize() = 0;
};

/// memory or disk checkpointing
#define CkCheckPoint_inMEM   1
#define CkCheckPoint_inDISK  2

class CkCheckPTEntry{
  std::vector<CkArrayCheckPTMessage *> data;
  std::string fname;
public:
  int bud1, bud2;
  int where;
  void init(int _where, int idx)
  {
    data.resize(2, NULL);
    where = _where;
    if(where == CkCheckPoint_inDISK)
    {
#if CMK_USE_MKSTEMP
#if CMK_CONVERSE_MPI
      fname = "/tmp/ckpt" + std::to_string(CmiMyPartition()) + "-" + std::to_string(CkMyPe()) + "-" + std::to_string(idx) + "-XXXXXX";
#else
      fname = "/tmp/ckpt" + std::to_string(CkMyPe()) + "-" + std::to_string(idx) + "-XXXXXX";
#endif
      if(mkstemp(&fname[0])<0)
	{
	  CmiAbort("mkstemp fail in checkpoint");
	}
#else
      fname = tmpnam(NULL);
#endif
    }
  }

  void updateBuffer(int pointer, CkArrayCheckPTMessage * msg)
  {
    if(where == CkCheckPoint_inDISK)
    {
      envelope *env = UsrToEnv(msg);
      CkUnpackMessage(&env);
      data[pointer] = (CkArrayCheckPTMessage *)EnvToUsr(env);
      FILE *f = fopen(fname.c_str(),"wb");
      PUP::toDisk p(f);
      CkPupMessage(p, (void **)&msg);
      // delay sync to the end because otherwise the messages are blocked
  //    fsync(fileno(f));
      fclose(f);
      bud1 = msg->bud1;
      bud2 = msg->bud2;
      delete msg;
    }else
    {
      CmiAssert(where == CkCheckPoint_inMEM);
      CmiAssert(msg!=NULL);
      delete data[pointer];
      data[pointer] = msg;
      bud1 = msg->bud1;
      bud2 = msg->bud2;
    }
  }
  
  CkArrayCheckPTMessage * getCopy(int pointer)
  {
    if(where == CkCheckPoint_inDISK)
    {
      CkArrayCheckPTMessage *msg;
      FILE *f = fopen(fname.c_str(),"rb");
      PUP::fromDisk p(f);
      CkPupMessage(p, (void **)&msg);
      fclose(f);
      msg->bud1 = bud1;				// update the buddies
      msg->bud2 = bud2;
      return msg;
    }else
    {
      CmiAssert(where == CkCheckPoint_inMEM);
      if (data[pointer] == NULL) {
        CmiPrintf("[%d] recoverArrayElements: element does not have checkpoint data.", CkMyPe());
        CmiAbort("Abort!");
      }
      return (CkArrayCheckPTMessage *)CkCopyMsg((void **)&data[pointer]);
    }
  }
};


class CkMemCheckPT: public CBase_CkMemCheckPT {
public:
  CkMemCheckPT(int w);
  CkMemCheckPT(CkMigrateMessage *m):CBase_CkMemCheckPT(m) {};
  virtual ~CkMemCheckPT();
  void pup(PUP::er& p);
  inline int BuddyPE(int pe);
  inline int ReverseBuddyPE(int pe);
  void doItNow(int sp, CkCallback &&);
  void restart(int diePe);
  void removeArrayElements();
  void createEntry(CkArrayID aid, CkGroupID loc, CkArrayIndex index, int buddy);
  void recvData(CkArrayCheckPTMessage *);
  void gotData();
  void recvProcData(CkProcCheckPTMessage *);
  void cpFinish();
  void cpDone();
  void requestRollback();
  void rollback();
  int numCompletedCheckpoints() const { return completedCheckpoints; }
  void syncFiles(void);
  void report();
  void recoverBuddies();
  void recoverEntry(CkArrayCheckPTMessage *msg);
  void recoverArrayElements();
  void quiescence(CkCallback &&);
  void resetReductionMgr();
  void finishUp();
  void gotReply();
  void inmem_restore(CkArrayCheckPTMessage *m);
  void updateLocations(int n, CkGroupID *g, CkArrayIndex *idx, CmiUInt8 *id, int nowOnPe);
  void resetLB(int diepe);
  bool isFailed(int pe);
  void pupAllElements(PUP::er &p);
  void startArrayCheckpoint();
  void recvArrayCheckpoint(CkArrayCheckPTMessage *m);
  void arrayCopyStored();
  void recoverAll(CkArrayCheckPTMessage * msg, std::vector<CkGroupID> * gmap=NULL, std::vector<CkArrayIndex> * imap=NULL);
public:
  static CkCallback  cpCallback;

  static bool inRestarting;
  static bool inCheckpointing;
  static bool inLoadbalancing;
  static double startTime;
  static char*  stage;

private:
  std::vector<CkCheckPTInfo *> ckTable;
  CkCheckPTEntry chkpTable[2];

  int recvCount, peCount;
  int expectCount, ackCount;
  int recvChkpCount;//expect to receive both the processor checkpoint and array checkpoint from buddy PE
  /// the processor who initiate the checkpointing
  int cpStarter;
  std::vector<int> failedPes;
  int thisFailedPe;           // -1 during a rollback
  int completedCheckpoints;   // checkpoints whose report() ran on this PE
  bool rollbackPending;       // PE 0 only: a rollback was requested and has not finished

    /// to use memory or disk checkpointing
  int    where;
private:
  void initEntry();
  inline bool isMaster(int pe);

  void failed(int pe);
  int  totalFailed();

  void sendProcData();
};

// called in initCharm
void CkMemRestart(const char *, CkArgMsg *);

// called by user applications
// to start a checkpointing
void CkStartMemCheckpoint(CkCallback &cb);

// called by user applications, on any PE, after a completed checkpoint: restores
// every array element from it and sends the checkpoint's callback again
void CkRollbackMemCheckpoint();

// called by the location manager when an array element leaves this PE
void CkMemCheckPTNoteMigration();

// true if inside a restarting phase
extern "C" int CkInRestarting();
extern "C" int CkInLdb();
extern "C" void CkSetInLdb(); 
extern "C" void CkResetInLdb();

extern "C" int CkHasCheckpoints();

void CkDieNow();

#endif
