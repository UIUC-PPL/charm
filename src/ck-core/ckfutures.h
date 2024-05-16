#ifndef _CKFUTURES_H_
#define _CKFUTURES_H_

#include "charm.h"

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

void* CkGetMsgBuffer(void* msg);
std::vector<void*> CkWaitAllIDs(const std::vector<CkFutureID>& handles);
std::pair<void*, CkFutureID> CkWaitAnyID(const std::vector<CkFutureID>& handles);

namespace ck {
namespace {
  void reject_non_local() {
    CkAbort("A future may only be polled/released/retrieved on the PE it was created on.");
  }
}

  template <typename T> class future {
    CkFuture handle_;

  public:
    using value_type = T;

    future() { handle_ = CkCreateFuture(); }
    future(PUP::reconstruct) { }
    future(const CkFuture &handle): handle_(handle) { }
    future(const future<T> &other) { handle_ = other.handle_; }

    static T unmarshall_value(void *msg) {
      PUP::detail::TemporaryObjectHolder<T> holder;
      ck::unmarshall(static_cast<ck::marshall_msg>(msg), holder);
      return holder.t;
    }

    T get() const {
      if (!is_local()) {
        reject_non_local();
      }
      return unmarshall_value(CkWaitFuture(handle_));
    }

    void set(const T &value) {
      CkSendToFuture(handle_, ck::make_marshall_message(value));
    }

    CkFuture handle() const { return handle_; }
    bool is_ready() const { return CkProbeFuture(handle_); }
    bool is_local() const { return handle_.pe == CkMyPe(); }
    void release() {
      if (!is_local()) {
        reject_non_local();
      } else if (is_ready()) {
        CkFreeMsg(CkWaitFuture(handle_));
      }
      CkReleaseFuture(handle_);
    }
    void pup(PUP::er &p) { p | handle_; }

    inline bool operator==(const future<T>& other) const {
      return (this->handle_.id == other.handle_.id) &&
             (this->handle_.pe == other.handle_.pe);
    }
  };

namespace {
  template<typename InputIter, typename T = typename InputIter::value_type::value_type>
  bool all_local(InputIter first, InputIter last) {
    return std::all_of(first, last,
      [](const ck::future<T>& f) { return f.is_local(); });
  }
}

  // returns a pair with the value and fulfilled future
  template<typename InputIter, typename T = typename InputIter::value_type::value_type>
  std::pair<T, InputIter> wait_any(InputIter first, InputIter last) {
    if (!all_local(first, last)) reject_non_local();
    const int n = last - first;
    std::vector<CkFutureID> handles;
    handles.reserve(n);
    std::transform(first, last, std::back_inserter(handles),
      [](future<T>& f) { return f.handle().id; });
    auto pair = CkWaitAnyID(handles);
    auto value = future<T>::unmarshall_value(pair.first);
    auto which = std::find_if(first, last,
      [&pair](future<T>& f) { return f.handle().id == pair.second; });
    return std::make_pair(value, which);
  }

  // returns a list of all the values
  template<typename InputIter, typename T = typename InputIter::value_type::value_type>
  std::vector<T> wait_all(InputIter first, InputIter last) {
    if (!all_local(first, last)) reject_non_local();
    const int n = last - first;
    std::vector<T> result;
    std::vector<CkFutureID> handles;
    result.reserve(n);
    handles.reserve(n);
    std::transform(first, last, std::back_inserter(handles),
      [](future<T>& f) { return f.handle().id; });
    auto values = CkWaitAllIDs(handles);
    std::transform(values.begin(), values.end(), std::back_inserter(result),
      [](void* value) { return future<T>::unmarshall_value(value); });
    return result;
  }

  // parititions the input iterators into ready/non ready
  // returns an iterator to the first non-ready future, and a list of any obtained value(s)
  template<typename InputIter, typename T = typename InputIter::value_type::value_type>
  std::pair<std::vector<T>, InputIter> wait_some(InputIter first, InputIter last) {
    // all_local check occurs within wait_all, anything remote is not-ready anyway
    auto pending = std::partition(first, last,
      [](const future<T>& f) { return f.is_ready(); });
    return std::make_pair(wait_all(first, pending), pending);
  }
}
#endif

#endif
