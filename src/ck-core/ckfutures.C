/**
\file
\addtogroup CkFutures

To call [sync] entry methods, we need a way to block
the current Converse thread until the called method returns.

A "future" represents a thread of control that has been passed
to another processor.  It provides a place for a (local) thread to
block and the machinery for resuming control based on a remote
event.  Futures are thus used to implement Charm++'s "[sync]" methods.

This "sequential futures abstraction" is a well-studied concept
in remote process control.
*/
/*@{*/
#include <limits>
#include <memory>
#include <vector>
#include <cstdlib>
#include <algorithm>
#include <unordered_map>

#include "charm++.h"
#include "ck.h"
#include "ckarray.h"
#include "ckfutures.h"

CkGroupID _fbocID;

struct FuturePairMsg : public CMessage_FuturePairMsg {
  CkFuture first;
  CkFuture second;
  FuturePairMsg(const CkFuture &first_) : first(first_) {}
  FuturePairMsg(const CkFuture &first_, const CkFuture &second_)
      : first(first_), second(second_){};
};

struct FutureRequest {
  virtual void fulfill(const CkFutureID& id, void* value) = 0;
};

class FutureToThread: public FutureRequest {
  CthThread th;
 public:
  FutureToThread(CthThread th_) : th(th_) {}

  virtual void fulfill(const CkFutureID&, void* value) override {
    // If we have a valid thread to wake up, do so
    if (th) CthAwaken(th);
    // Then invalidate ourself
    th = nullptr;
  }
};

class MultiToThread: public FutureRequest {
  CthThread th;
  const std::vector<CkFutureID>& ids;
  std::size_t nRecvd;
 public:
  std::vector<void*> values;

  MultiToThread(CthThread th_, const std::vector<CkFutureID>& ids_) : ids(ids_), th(th_), nRecvd(0) {
    values.resize(ids.size());

    std::fill(values.begin(), values.end(), nullptr);
  }

  virtual void fulfill(const CkFutureID& id, void* value) override {
    const auto search = std::find(ids.begin(), ids.end(), id);
    const auto offset = search - ids.begin();

    CkAssert(value != nullptr && search != ids.end());

    if (th != nullptr && values[offset] == nullptr) {
      values[offset] = value;

      if (++nRecvd >= values.size()) {
        CthAwaken(th);
        th = nullptr;
      }
    }
  }
};

class FutureToFuture: public FutureRequest {
  CkFuture fut; 
  bool fulfilled;
 public:
  FutureToFuture(const CkFuture& fut_) : fut(fut_), fulfilled(false) {}

  virtual void fulfill(const CkFutureID&, void* value) override {
    // If we have not been fulfilled, forward the value
    if (!fulfilled) CkSendToFuture(fut, value);
    // Then mark ourself as fulfilled
    fulfilled = true;
  }
};

struct FutureState {
  using request_t = std::shared_ptr<FutureRequest>;
  using request_queue_t = std::vector<request_t>;
 private:
  std::unordered_map<CkFutureID, void*> values;
  std::unordered_map<CkFutureID, request_queue_t> waiting;
  std::vector<CkFutureID> freeList;

  CkFutureID last;
 public:
  FutureState() : last(-1) {}

  // takes a free id from the set of released IDs, when one
  // is available, or increments the PE's local ID counter
  // and uses the updated value
  CkFutureID next() {
    CkFutureID id;
    if (freeList.empty()) {
      id = ++last;
    } else {
      id = *freeList.begin();
      freeList.erase(freeList.begin());
    }
    CkAssert(id <= std::numeric_limits<CMK_REFNUM_TYPE>::max() &&
             "future count has exceeded CMK_REFNUM_TYPE, see manual.");
    return id;
  }

  // returns a future's value when it's available, otherwise
  // returns nullptr
  void* operator[](const CkFutureID& f) const {
    auto found = values.find(f);
    return (found != values.end()) ? found->second : nullptr;
  }

  // enqueue a request for a given future id, creating a
  // queue as necessary
  void request(const CkFutureID& f, request_t req) {
    auto found = waiting.find(f);
    if (found != waiting.end()) {
      found->second.push_back(req);
    } else {
      waiting[f] = {req};
    }
  }

  // enuqueue a request to be fulfilled by multiple futures
  void request(const std::vector<CkFutureID>& fs, request_t req) {
    for (auto& f : fs) {
      request(f, req);
    }
  }

  // stores a value for a given future id, and fulfills all
  // outstanding requests for the value (then erases them)
  // (this is usually called by the FutureBOC chare-group)
  void fulfill(const CkFutureID& f, void* value) {
    values[f] = value;
    auto found = waiting.find(f);
    if (found != waiting.end()) {
      for (auto& th : found->second) {
        th->fulfill(f, value);
      }
      waiting.erase(found);
    }
  }

  // erase any listeners and values for a given future
  // and adds it to the set of free future ids (that
  // can be reused). note, does not free any memory
  void release(const CkFutureID& f) {
    values.erase(f);
    waiting.erase(f);
    CkAssert(std::find(freeList.begin(), freeList.end(), f) == freeList.end() &&
            "repeated frees of the same future");
    freeList.push_back(f);
  }

  bool is_ready(const CkFutureID& f) {
    return values.find(f) != values.end();
  }
};

class CkSema {
  private:
    CkQ<void*> msgs;
    CkQ<CthThread> waiters;
  public:
    void *wait(void) {
      void *retmsg = msgs.deq();
      if(retmsg==0) {
        waiters.enq(CthSelf());
        CthSuspend();
        retmsg = msgs.deq();
      }
      return retmsg;
    }
    void waitN(int n, void *marray[]) {
      while (1) {
        if(msgs.length()<n) {
          waiters.enq(CthSelf());
          CthSuspend();
          continue;
        }
        for(int i=0;i<n;i++)
          marray[i] = msgs.deq();
        return;
      }
    }
    void signal(void *msg)
    {
      msgs.enq(msg);
      if(!waiters.isEmpty())
        CthAwaken(waiters.deq());
      return;
    }
};

class CkSemaPool {
  private:
    std::vector<CkSema*> pool;
    CkQ<int> freelist;
  public:
    int getNew(void) {
      int idx;
      if(freelist.isEmpty()) {
        idx = pool.size();
        pool.push_back(new CkSema());
      } else {
        idx = freelist.deq();
        pool[idx] = new CkSema();
      }
      return idx;
    }
    void release(int idx) {
      CkSema * sem = pool[idx];
      delete sem;
      freelist.enq(idx);
    }
    void _check(int idx) {
#if CMK_ERROR_CHECKING
      if(pool[idx]==0) {
	      CkAbort("ERROR! operation attempted on invalid semaphore\n");
      }
#endif
    }
    void *wait(int idx) { 
      _check(idx);
      return pool[idx]->wait(); 
    }
    void waitN(int idx, int n, void *marray[]) { 
      _check(idx);
      pool[idx]->waitN(n, marray); 
    }
    void signal(int idx, void *msg) { 
      _check(idx);
      pool[idx]->signal(msg); 
    }
};

CpvStaticDeclare(FutureState, futurestate);
CpvStaticDeclare(CkSemaPool*, semapool);

static 
inline
CkFutureID createFuture(void)
{
  FutureState *fs = &(CpvAccess(futurestate));
  return fs->next();
}

CkFuture CkCreateFuture(void)
{
  CkFuture fut;
  fut.id = createFuture();
  fut.pe = CkMyPe();
  return fut;
}

void CkReleaseFutureID(CkFutureID handle)
{
  FutureState *fs = &(CpvAccess(futurestate));
  fs->release(handle);
}

int CkProbeFutureID(CkFutureID handle)
{
  FutureState *fs = &(CpvAccess(futurestate));
  return fs->is_ready(handle);
}

void *CkWaitFutureID(CkFutureID handle)
{
  FutureState *fs = &(CpvAccess(futurestate));

  if (!fs->is_ready(handle)) {
    fs->request(handle, std::make_shared<FutureToThread>(CthSelf()));
    while (!fs->is_ready(handle)) { CthSuspend(); }
  }

  void *value = (*fs)[handle];
#if CMK_ERROR_CHECKING
  if (value==NULL) 
	CkAbort("ERROR! CkWaitFuture would have to return NULL!\n"
	"This can happen when a thread that calls a sync method "
	"gets a CthAwaken call *before* the sync method returns.");
#endif
  return value;
}

std::pair<void*, CkFutureID> CkWaitAnyID(const std::vector<CkFutureID>& handles) {
  auto fs = &(CpvAccess(futurestate));
  const auto ready = std::any_of(handles.begin(), handles.end(),
    [&fs](const CkFutureID& id) { return fs->is_ready(id); });
  /* A single request is generated, and enqueued for all the futures that we are
   * interested in. Note, a request may only be fulfilled once, then it will expire
   * and become a no-op; thus, the corresponding `CthAwaken` for this thread will
   * only be called once (for more details, see FutureToThread::fulfill).
   */ 
  if (!ready) fs->request(handles, std::make_shared<FutureToThread>(CthSelf()));
  do {
    if (!ready) CthSuspend();
    for (const auto& handle : handles) {
      if (fs->is_ready(handle)) {
        return std::make_pair((*fs)[handle], handle);
      }
    }
  } while (true);
}

std::vector<void*> CkWaitAllIDs(const std::vector<CkFutureID>& ids) {
  auto fs = &(CpvAccess(futurestate));
  auto req = std::make_shared<MultiToThread>(CthSelf(), ids);
  bool wait = false;

  for (const auto& id : ids) {
    auto val = (*fs)[id];

    if (val != nullptr) {
      req->fulfill(id, val);
    } else {
      fs->request(id, req);
      wait = true;
    }
  }

  if (wait) {
    CthSuspend();
  }

  return req->values;
}

void CkReleaseFuture(CkFuture fut)
{
  if (fut.pe == CkMyPe()) {
    CkReleaseFutureID(fut.id);
  } else {
    auto fBOC = CProxy_FutureBOC(_fbocID);
    auto msg = new FuturePairMsg(fut);
    fBOC[fut.pe].RelFuture(msg);
  }
}

int CkProbeFuture(CkFuture fut)
{
  return CkProbeFutureID(fut.id);
}

CkFuture CkLocalizeFuture(const CkFuture &fut) {
  auto ours = CkCreateFuture();
  auto fBOC = CProxy_FutureBOC(_fbocID);
  auto msg = new FuturePairMsg(fut, ours);
  fBOC[fut.pe].ReqFuture(msg);
  return ours;
}

void *CkWaitFuture(CkFuture fut) {
  const auto isRemote = fut.pe != CkMyPe();
  const auto local = isRemote ? CkLocalizeFuture(fut) : fut;
  auto value = CkWaitFutureID(local.id);
  if (isRemote) CkReleaseFuture(local);
  return value;
}

void CkWaitVoidFuture(CkFutureID handle)
{
  CkFreeMsg(CkWaitFutureID(handle));
}

static void setFuture(CkFutureID handle, void *pointer)
{
  FutureState *fs = &(CpvAccess(futurestate));
  fs->fulfill(handle, pointer);
}

void _futuresModuleInit(void)
{
  CpvInitialize(FutureState, futurestate);
  CpvInitialize(CkSemaPool *, semapool);
  CpvAccess(semapool) = new CkSemaPool();
}

class FutureInitMsg : public CMessage_FutureInitMsg {
  public: int x ;
};

class  FutureMain : public Chare {
  public:
    FutureMain(CkArgMsg *m) {
      _fbocID = CProxy_FutureBOC::ckNew(new FutureInitMsg);
      delete m;
    }
    FutureMain(CkMigrateMessage *m) {}
};

extern "C" 
CkFutureID CkRemoteBranchCallAsync(int ep, void *m, CkGroupID group, int PE)
{ 
  CkFutureID ret=CkCreateAttachedFuture(m);
  CkSendMsgBranch(ep, m, PE, group);
  return ret;
}

extern "C" 
void *CkRemoteBranchCall(int ep, void *m, CkGroupID group, int PE)
{ 
  CkFutureID i = CkRemoteBranchCallAsync(ep, m, group, PE);  
  return CkWaitReleaseFuture(i);
}

extern "C" 
CkFutureID CkRemoteNodeBranchCallAsync(int ep, void *m, CkGroupID group, int node)
{ 
  CkFutureID ret=CkCreateAttachedFuture(m);
  CkSendMsgNodeBranch(ep, m, node, group);
  return ret;
}

extern "C" 
void *CkRemoteNodeBranchCall(int ep, void *m, CkGroupID group, int node)
{ 
  CkFutureID i = CkRemoteNodeBranchCallAsync(ep, m, group, node);
  return CkWaitReleaseFuture(i);
}

extern "C" 
CkFutureID CkRemoteCallAsync(int ep, void *m, const CkChareID *ID)
{ 
  CkFutureID ret=CkCreateAttachedFuture(m);
  CkSendMsg(ep, m, ID);
  return ret;
}

extern "C" 
void *CkRemoteCall(int ep, void *m, const CkChareID *ID)
{ 
  CkFutureID i = CkRemoteCallAsync(ep, m, ID);
  return CkWaitReleaseFuture(i);
}


CkFutureID CkCreateAttachedFuture(void *msg)
{
  CkFutureID ret=createFuture();
  UsrToEnv(msg)->setRef(ret);
  return ret;
}

CkFutureID CkCreateAttachedFutureSend(void *msg, int ep,
CkArrayID id, CkArrayIndex idx,
void(*fptr)(CkArrayID,CkArrayIndex,void*,int,int),int size)
{
CkFutureID ret=createFuture();
UsrToEnv(msg)->setRef(ret);
#if IGET_FLOWCONTROL
if (TheIGetControlClass.iget_request(ret,msg,ep,id,idx,fptr,size))
#endif
(fptr)(id,idx,msg,ep,0);
return ret;
}


/*
CkFutureID CkCreateAttachedFutureSend(void *msg, int ep, void *obj,void(*fptr)(void*,void*,int,int))
{
  CkFutureID ret=createFuture();
  UsrToEnv(msg)->setRef(ret);
#if IGET_FLOWCONTROL
  if (TheIGetControlClass.iget_request(ret,msg,ep,obj,fptr)) 
#endif
  (fptr)(obj,msg,ep,0);
  return ret;
}
*/
void *CkWaitReleaseFuture(CkFutureID futNum)
{
#if IGET_FLOWCONTROL
  TheIGetControlClass.iget_resend(futNum);
#endif
  void *result=CkWaitFutureID(futNum);
  CkReleaseFutureID(futNum);
#if IGET_FLOWCONTROL
  TheIGetControlClass.iget_free(1);
//  TheIGetControlClass.iget_free(sizeof(result));
#endif
  return result;
}

class FutureBOC: public IrrGroup {
public:
  FutureBOC(void){ }
  FutureBOC(FutureInitMsg *m) { delete m; }
  FutureBOC(CkMigrateMessage *m) { }
  void SetFuture(FutureInitMsg * m) { 
#if CMK_ERROR_CHECKING
    if (m==NULL) CkAbort("FutureBOC::SetFuture called with NULL!");
#endif
    int key;
    key = UsrToEnv((void *)m)->getRef();
    setFuture( key, m);
  }
  void SetSema(FutureInitMsg *m) {
#if CMK_ERROR_CHECKING
    if (m==NULL) CkAbort("FutureBOC::SetSema called with NULL!");
#endif
    int idx;
    idx = UsrToEnv((void *)m)->getRef();
    CpvAccess(semapool)->signal(idx,(void*)m);
  }
  void ReqFuture(FuturePairMsg* msg) { 
    const auto& ours = msg->first;
    const auto& theirs = msg->second;
    CkAssert(ours.pe == CkMyPe());
    auto req = std::make_shared<FutureToFuture>(theirs);
    auto fs = &(CpvAccess(futurestate));
    if (fs->is_ready(ours.id)) {
      req->fulfill(ours.id, (*fs)[ours.id]);
    } else {
      fs->request(ours.id, req);
    }
    delete msg;
  }
  // this should be used only in conjunction with
  // future-to-future requests, otherwise it may
  // leak the pointer held on the remote side
  void RelFuture(FuturePairMsg* msg) {
    const auto& ours = msg->first;
    CkAssert(ours.pe == CkMyPe());
    CkReleaseFutureID(ours.id);
    delete msg;
  }
};

extern "C" 
void CkSendToFutureID(CkFutureID futNum, void *m, int PE)
{
  UsrToEnv(m)->setRef(futNum);
  CProxy_FutureBOC fBOC(_fbocID);
  fBOC[PE].SetFuture((FutureInitMsg *)m);
}

void  CkSendToFuture(CkFuture fut, void *msg)
{
  CkSendToFutureID(fut.id, msg, fut.pe);
}

CkSemaID CkSemaCreate(void)
{
  CkSemaID id;
  id.pe = CkMyPe();
  id.idx = CpvAccess(semapool)->getNew();
  return id;
}

void *CkSemaWait(CkSemaID id)
{
#if CMK_ERROR_CHECKING
  if(id.pe != CkMyPe()) {
    CkAbort("ERROR: Waiting on nonlocal semaphore! Aborting..\n");
  }
#endif
  return CpvAccess(semapool)->wait(id.idx);
}

void CkSemaWaitN(CkSemaID id, int n, void *marray[])
{
#if CMK_ERROR_CHECKING
  if(id.pe != CkMyPe()) {
    CkAbort("ERROR: Waiting on nonlocal semaphore! Aborting..\n");
  }
#endif
  CpvAccess(semapool)->waitN(id.idx, n, marray);
}

void CkSemaSignal(CkSemaID id, void *m)
{
  UsrToEnv(m)->setRef(id.idx);
  CProxy_FutureBOC fBOC(_fbocID);
  fBOC[id.pe].SetSema((FutureInitMsg *)m);
}

void CkSemaDestroy(CkSemaID id)
{
#if CMK_ERROR_CHECKING
  if(id.pe != CkMyPe()) {
    CkAbort("ERROR: destroying a nonlocal semaphore! Aborting..\n");
  }
#endif
  CpvAccess(semapool)->release(id.idx);
}


/*@}*/
#include "CkFutures.def.h"
