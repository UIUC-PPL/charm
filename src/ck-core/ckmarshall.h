#ifndef _CKMARSHALL_H_
#define _CKMARSHALL_H_

// This is the message type marshalled parameters get packed into:
class CkMarshallMsg;
class CkEntryOptions;

CkMarshallMsg *CkAllocateMarshallMsg(int size, const CkEntryOptions *opts = nullptr);

namespace ck {
using marshall_msg = CkMarshallMsg *;

inline char *get_message_buffer(marshall_msg msg);

template <class... Args>
inline marshall_msg make_marshall_message(const Args&... _args) {
  auto args = std::forward_as_tuple(const_cast<Args&>(_args)...);
  auto size = PUP::size(args);
  auto msg = CkAllocateMarshallMsg(size);
  PUP::toMemBuf(args, get_message_buffer(msg), size);
  return msg;
}

template <class... Args>
inline void unmarshall(marshall_msg msg, Args&... _args) {
  auto args = std::forward_as_tuple(_args...);
  PUP::fromMem p(get_message_buffer(msg));
  p | args;
}

}

#include "ckmessage.h"
#include "ckentryopts.h"
#include "CkMarshall.decl.h"

class CkMarshallMsg : public CMessage_CkMarshallMsg {
public:
  char *msgBuf;
};

namespace ck {
  inline char *get_message_buffer(marshall_msg msg) {
    return msg->msgBuf;
  }
}

CkMarshallMsg *CkAllocateMarshallMsgNoninline(int size, const CkEntryOptions *opts);

inline CkMarshallMsg *CkAllocateMarshallMsg(int size, const CkEntryOptions *opts) {
  if (opts == NULL) {
    CkMarshallMsg *newMemory = new (size, 0) CkMarshallMsg;
    setMemoryTypeMessage(UsrToEnv(newMemory));
    return newMemory;
  } else
    return CkAllocateMarshallMsgNoninline(size, opts);
}

template <typename T>
inline T *CkAllocateMarshallMsgT(int size, const CkEntryOptions *opts) {
  int priobits = 0;
  if (opts != NULL)
    priobits = opts->getPriorityBits();
  // Allocate the message
  T *m = new (size, priobits) T;
  // Copy the user's priority data into the message
  envelope *env = UsrToEnv(m);
  setMemoryTypeMessage(env);
  if (opts != NULL) {
    CmiMemcpy(env->getPrioPtr(), opts->getPriorityPtr(), env->getPrioBytes());
    // Set the message's queueing type
    env->setQueueing((unsigned char)opts->getQueueing());
  }
  return m;
}

#endif
