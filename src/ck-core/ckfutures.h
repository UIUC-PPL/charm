#ifndef _CKFUTURES_H_
#define _CKFUTURES_H_

#ifdef __cplusplus
#include "ckmarshall.h"
#endif
#include "CkFutures.decl.h"

/**
\addtogroup CkFutures
\brief Futures--ways to block Converse threads on remote events.

These routines are implemented in ckfutures.C.
*/
/*@{*/
typedef unsigned long long CkFutureID;
typedef struct _CkFuture {
  CkFutureID id;
  int        pe;
} CkFuture;
PUPbytes(CkFuture)

/* forward declare */
struct CkArrayID;

#ifdef __cplusplus
extern "C" {
#endif

CkFuture CkCreateFuture(void);
void  CkSendToFuture(CkFuture fut, void *msg);
void* CkWaitFuture(CkFuture futNum);
void CkReleaseFuture(CkFuture futNum);
int CkProbeFuture(CkFuture futNum);

void* CkRemoteCall(int eIdx, void *msg,const CkChareID *chare);
void* CkRemoteBranchCall(int eIdx, void *msg, CkGroupID gID, int pe);
void* CkRemoteNodeBranchCall(int eIdx, void *msg, CkGroupID gID, int node);
CkFutureID CkRemoteCallAsync(int eIdx, void *msg, const CkChareID *chare);
CkFutureID CkRemoteBranchCallAsync(int eIdx, void *msg, CkGroupID gID, int pe);
CkFutureID CkRemoteNodeBranchCallAsync(int eIdx, void *msg, CkGroupID gID, int node);

void* CkWaitFutureID(CkFutureID futNum);
void CkWaitVoidFuture(CkFutureID futNum);
void CkReleaseFutureID(CkFutureID futNum);
int CkProbeFutureID(CkFutureID futNum);
void  CkSendToFutureID(CkFutureID futNum, void *msg, int pe);
CkFutureID CkCreateAttachedFuture(void *msg);

CkFutureID CkCreateAttachedFutureSend(void *msg, int ep, struct CkArrayID id, CkArrayIndex idx,
                          void(*fptr)(struct CkArrayID, CkArrayIndex,void*,int,int),int size CK_MSGOPTIONAL);
/* CkFutureID CkCreateAttachedFutureSend(void *msg, int ep, void*,void(*fptr)(void*,void*,int,int)); */

void *CkWaitReleaseFuture(CkFutureID futNum);

void _futuresModuleInit(void);

#ifdef __cplusplus
}

namespace ck {
  template <typename T> class future {
    CkFuture handle_;

  public:
    future() { handle_ = CkCreateFuture(); }
    future(PUP::reconstruct) { }
    future(const CkFuture &handle): handle_(handle) { }
    future(const future<T> &other) { handle_ = other.handle_; }

    T get() const {
      if (handle_.pe != CkMyPe()) {
        CkAbort("A future's value can only be retrieved on the PE it was created on.");
      }
      auto msg = static_cast<ck::marshall_msg>(CkWaitFuture(handle_));
      T result;
      ck::unmarshall(msg, result);
      return result;
    }

    void set(const T &value) {
      CkSendToFuture(handle_, ck::make_marshall_message(value));
    }

    CkFuture handle() const { return handle_; }
    bool is_ready() const { return CkProbeFuture(handle_); }
    void release() {
      if (handle_.pe != CkMyPe()) {
        CkAbort("A future can only be released on the PE it was created on.");
      } else if (is_ready()) {
        delete (CkMarshallMsg *)CkWaitFuture(handle_);
      }
      CkReleaseFuture(handle_);
    }
    void pup(PUP::er &p) { p | handle_; }
  };
}
#endif

#endif
