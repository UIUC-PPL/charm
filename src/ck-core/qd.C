#define  DEBUGP(x)   //  CmiPrintf x;

#include "ck.h"

class QdMsg {
  private:
    int phase; // 0..2
    union {
      struct { /* none */ } p1;
      struct { CmiInt8 created; CmiInt8 processed; } p2;
      struct { /* none */ } p3;
      struct { char dirty; } p4;
    } u;
    CkCallback cb;
  public:
    int getPhase(void) { return phase; }
    void setPhase(int p) { phase = p; }
    CkCallback getCb(void) { CkAssert(phase==0); return cb; }
    void setCb(CkCallback cb_) { CkAssert(phase==0); cb = cb_; }
    CmiInt8 getCreated(void) { CkAssert(phase==1); return u.p2.created; }
    void setCreated(CmiInt8 c) { CkAssert(phase==1); u.p2.created = c; }
    CmiInt8 getProcessed(void) { CkAssert(phase==1); return u.p2.processed; }
    void setProcessed(CmiInt8 p) { CkAssert(phase==1); u.p2.processed = p; }
    char getDirty(void) { CkAssert(phase==2); return u.p4.dirty; }
    void setDirty(char d) { CkAssert(phase==2); u.p4.dirty = d; }
};

class QdCommMsg {
  public:
    bool isCreated;     //  false: create   true: process
    int count;
};

class QdCallback {
  public:
	CkCallback cb;
  public:
    QdCallback(int e, CkChareID c) : cb(e, c) {}
	QdCallback(CkCallback cb_) : cb(cb_) {}
//    void send(void) { CkSendMsg(ep,CkAllocMsg(0,0,0),&cid); }
    void send(void) {
      cb.send(NULL);
    }
};

extern int _qdCommHandlerIdx;

// a fake QD which just wait for several seconds to triger QD callback
int _dummy_dq = 0;                      /* seconds to wait for */


CpvDeclare(QdState*, _qd);

// called when a node asks children for their counters
// send broadcast msg (phase 0) to children, and report to itself (phase 1)
// stage 1 means the node is waiting for reports from children
static inline void _bcastQD1(QdState* state, QdMsg *msg)
{
  msg->setPhase(0);
  state->propagate(msg);
  msg->setPhase(1);
  DEBUGP(("[%d] _bcastQD1: State: getCreated:%lld getProcessed:%lld\n", CmiMyPe(), state->getCreated(), state->getProcessed()));
/*
  QdState *comm_state;
  static int comm_create=0, comm_process=0;
  if (CmiMyRank()==0) {
    comm_state = CpvAccessOther(_qd, CmiMyNodeSize());
    int new_create = comm_state->getCreated();
    int new_process = comm_state->getProcessed();
    // combine counters with comm thread
    CmiAssert(new_create==0);
    CmiAssert(new_create == 0&& new_process==0);
    state->create(new_create-comm_create);
    state->process(new_process-comm_process);
    comm_create = new_create;
    comm_process = new_process;
  }
*/
  msg->setCreated(state->getCreated());
  msg->setProcessed(state->getProcessed());
  envelope *env = UsrToEnv((void*)msg);
  CmiSyncSendAndFree(CmiMyPe(), env->getTotalsize(), (char *)env);
  state->markProcessed();
  state->reset();
  state->setStage(1);
  DEBUGP(("[%d] _bcastQD1 stage changed to: %d\n", CmiMyPe(), state->getStage()));
}

// final phase to check if the counters become dirty or not
// stage 2 means the node is waiting for children to report their dirty state
static inline void _bcastQD2(QdState* state, QdMsg *msg)
{
  DEBUGP(("[%d] _bcastQD2: \n", CmiMyPe()));
  msg->setPhase(1);
  state->propagate(msg);
  msg->setPhase(2);
  msg->setDirty(state->isDirty());
  envelope *env = UsrToEnv((void*)msg);
  CmiSyncSendAndFree(CmiMyPe(), env->getTotalsize(), (char *)env);
  state->reset();
  state->setStage(2);
  DEBUGP(("[%d] _bcastQD2: stage changed to: %d\n", CmiMyPe(), state->getStage()));
}

static inline void _handlePhase0(QdState *state, QdMsg *msg)
{
  DEBUGP(("[%d] _handlePhase0: stage: %d, msg phase: %d\n", CmiMyPe(), state->getStage(), msg->getPhase()));
  CkAssert(CmiMyPe()==0 || state->getStage()==0);
  if(CmiMyPe()==0) {
    QdCallback *qdcb = new QdCallback(msg->getCb());
    _MEMCHECK(qdcb);
    state->enq(qdcb);		// stores qd callback
  }
  if(state->getStage()==0)
    _bcastQD1(state, msg);        // start asking children for the counters
  else
    CkFreeMsg(msg);               // already in the middle of processing
}

// collecting counters from children
static inline void _handlePhase1(QdState *state, QdMsg *msg)
{
  DEBUGP(("[%d] _handlePhase1: stage: %d, msg phase: %d\n", CmiMyPe(), state->getStage(), msg->getPhase()));
  switch(state->getStage()) {
    case 0 :
      CkAssert(CmiMyPe()!=0);
      _bcastQD2(state, msg);
      break;
    case 1 :
      DEBUGP(("[%d] msg: getCreated:%lld getProcessed:%lld\n", CmiMyPe(), msg->getCreated(), msg->getProcessed()));
        // add children's counters
      state->subtreeCreate(msg->getCreated());
      state->subtreeProcess(msg->getProcessed());
      state->reported();
      if(state->allReported()) {
        if(CmiMyPe()==0) {
          DEBUGP(("ALL: %p getCCreated:%lld getCProcessed:%lld\n", state, state->getCCreated(), state->getCProcessed()));
          if(state->getCCreated()==state->getCProcessed()) {
            if(state->oldCount == state->getCProcessed()) {// counts unchanged in second round
              _bcastQD2(state, msg);    // almost reached, one pass to make sure
            } else {
              state->oldCount = state->getCProcessed();
              _bcastQD1(state, msg);    // may have reached, go over again
            }
          } else {
            _bcastQD1(state, msg);    // not reached, go over again
          }
        } else {
            // report counters to parent
          msg->setCreated(state->getCCreated());
          msg->setProcessed(state->getCProcessed());
          envelope *env = UsrToEnv((void*)msg);
          CmiSyncSendAndFree(state->getParent(), 
                             env->getTotalsize(), (char *)env);
          state->reset();
          state->setStage(0);
        }
      } else
          CkFreeMsg(msg);
      break;
    default: CmiAbort("Internal QD Error. Contact Developers.!\n");
  }
}

// check if counters became dirty and notify parents
static inline void _handlePhase2(QdState *state, QdMsg *msg)
{
//  This assertion seems too strong for smp version.
  DEBUGP(("[%d] _handlePhase2: stage: %d, msg phase: %d \n", CmiMyPe(), state->getStage(), msg->getPhase()));
  CkAssert(state->getStage()==2);
  state->subtreeSetDirty(msg->getDirty());
  state->reported();
  if(state->allReported()) {
    if(CmiMyPe()==0) {
      if(state->isDirty()) {
        _bcastQD1(state, msg);   // dirty, restart again
      } else {             
          // quiescence detected, send callbacks
        DEBUGP(("[%d] quiescence detected at %f.\n", CmiMyPe(), CmiWallTimer()));
        QdCallback* cb;
        while(NULL!=(cb=state->deq())) {
          cb->send();
          delete cb;
        }
        state->reset();
        state->setStage(0);
        CkFreeMsg(msg);
      }
    } else {
        // tell parent if the counters on the node is dirty or not
      DEBUGP(("[%d] _handlePhase2 dirty:%d\n", CmiMyPe(), (int)state->isDirty()));
      msg->setDirty(state->isDirty());
      envelope *env = UsrToEnv((void*)msg);
      CmiSyncSendAndFree(state->getParent(), env->getTotalsize(), (char *)env);
      state->reset();
      state->setStage(0);
    }
  } else
    CkFreeMsg(msg);
}

static void _callWhenIdle(QdMsg *msg)
{
  DEBUGP(("[%d] callWhenIdle msg:%p \n", CmiMyPe(), msg));
  QdState *state = CpvAccess(_qd);
  switch(msg->getPhase()) {
    case 0 : _handlePhase0(state, msg); break;
    case 1 : _handlePhase1(state, msg); break;
    case 2 : _handlePhase2(state, msg); break;
    default: CmiAbort("Internal QD Error. Contact Developers.!\n");
  }
}

static void _invokeQD(QdMsg *msg)
{
  QdCallback *cb = new QdCallback(msg->getCb());
  cb->send();
  delete cb;
}

void _qdHandler(envelope *env)
{
  QdMsg *msg = (QdMsg*) EnvToUsr(env);
  DEBUGP(("[%d] _qdHandler msg:%p \n", CmiMyPe(), msg));
  if (_dummy_dq > 0)
    CcdCallFnAfter((CcdVoidFn)_invokeQD,(void *)msg, _dummy_dq*1000); // in ms
  else
    CcdCallOnCondition(CcdPROCESSOR_STILL_IDLE, (CcdCondFn)_callWhenIdle, (void*) msg);
}

// when a message is sent or received from an immediate handler from comm
// thread or interrupt handler, the counter is sent to rank 0 of the same node
void _qdCommHandler(envelope *env)
{
  QdCommMsg *msg = (QdCommMsg*) EnvToUsr(env);
  DEBUGP(("[%d] _qdCommHandler msg:%p \n", CmiMyPe(), msg));
  if (!msg->isCreated)
    CpvAccess(_qd)->create(msg->count);
  else
    CpvAccess(_qd)->process(msg->count);
  CmiFree(env);
}

void QdState::sendCount(bool isCreated, int count)
{
  if (_dummy_dq == 0) {
        if (CmiMyRank() == CmiMyNodeSize())
        {
          QdCommMsg *msg = (QdCommMsg*) CkAllocMsg(0,sizeof(QdCommMsg),0);
          msg->isCreated = isCreated;
          msg->count = count;
          envelope *env = UsrToEnv((void *)msg);
          CmiSetHandler(env, _qdCommHandlerIdx);
          CmiFreeSendFn(CmiNodeFirst(CmiMyNode()), env->getTotalsize(), (char *)env);
        }
  }
}

void CkStartQD(const CkCallback& cb)
{
  QdMsg *msg = (QdMsg*) CkAllocMsg(0,sizeof(QdMsg),0);
  msg->setPhase(0);
  msg->setCb(cb);
  envelope *env = UsrToEnv((void *)msg);
  CmiSetHandler(env, _qdHandlerIdx);
// The restart phase exists in the message header only in __FAULT__ builds.
#if CMK_MEM_CHECKPOINT && __FAULT__
  CmiGetRestartPhase(env) = 9999;        // make sure it is always executed
#endif
  _CldEnqueue(0, env, _infoIdx);
}

void CkStartQD(int eIdx, const CkChareID *cid)
{
  CkStartQD(CkCallback(eIdx, *cid));
}
