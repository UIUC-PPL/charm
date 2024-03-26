#ifndef _CKENTRYOPTS_H_
#define _CKENTRYOPTS_H_

#include "charm++.h"

/**
 * CkEntryOptions describes the options associated
 * with an entry method invocation, which include
 * the message priority and queuing strategy.
 * It is only used with parameter marshalling.
 */
class CkEntryOptions : public CkNoncopyable {
  int queueingtype;  // CK_QUEUEING type
  int prioBits;  // Number of bits of priority to use
  using prio_t = unsigned int;  // Datatype used to represent priorities
  prio_t *prioPtr;  // Points to message priority values
  prio_t prioStore;  // For short priorities, stores the priority value
  std::vector<CkGroupID> depGroupIDs;  // group dependencies
 public:
  CkEntryOptions(void)
      : queueingtype(CK_QUEUEING_FIFO),
        prioBits(0),
        prioPtr(nullptr),
        prioStore(0) {}

  ~CkEntryOptions() {
    if (prioPtr != nullptr && queueingtype != CK_QUEUEING_IFIFO &&
        queueingtype != CK_QUEUEING_ILIFO) {
      delete[] prioPtr;
      prioBits = 0;
    }
  }

  inline CkEntryOptions &setPriority(prio_t integerPrio) {
    queueingtype = CK_QUEUEING_IFIFO;
    prioBits = 8 * sizeof(integerPrio);
    prioPtr = &prioStore;
    prioStore = integerPrio;
    return *this;
  }
  inline CkEntryOptions &setPriority(int prioBits_, const prio_t *prioPtr_) {
    if (prioPtr != nullptr && queueingtype != CK_QUEUEING_IFIFO &&
        queueingtype != CK_QUEUEING_ILIFO) {
      delete[] prioPtr;
      prioBits = 0;
    }
    queueingtype = CK_QUEUEING_BFIFO;
    prioBits = prioBits_;
    int dataLength =
        (prioBits + (sizeof(prio_t) * 8 - 1)) / (sizeof(prio_t) * 8);
    prioPtr = new prio_t[dataLength];
    memcpy((void *)prioPtr, prioPtr_, dataLength * sizeof(unsigned int));
    return *this;
  }
  inline CkEntryOptions &setPriority(const CkBitVector &cbv) {
    if (!cbv.data.empty()) {
      if (prioPtr != nullptr && queueingtype != CK_QUEUEING_IFIFO &&
          queueingtype != CK_QUEUEING_ILIFO) {
        delete[] prioPtr;
        prioBits = 0;
      }
      queueingtype = CK_QUEUEING_BFIFO;
      prioBits = cbv.usedBits;
      int dataLength =
          (prioBits + (sizeof(prio_t) * 8 - 1)) / (sizeof(prio_t) * 8);
      prioPtr = new prio_t[dataLength];
      memcpy((void *)prioPtr, cbv.data.data(), dataLength * sizeof(prio_t));
    } else {
      queueingtype = CK_QUEUEING_BFIFO;
      prioBits = 0;
      int dataLength = 1;
      prioPtr = new prio_t[dataLength];
      prioPtr[0] = 0;
    }
    return *this;
  }

  inline CkEntryOptions &setQueueing(int queueingtype_) {
    queueingtype = queueingtype_;
    return *this;
  }
  inline CkEntryOptions &setGroupDepID(const CkGroupID &gid) {
    depGroupIDs.clear();
    depGroupIDs.push_back(gid);
    return *this;
  }
  inline CkEntryOptions &addGroupDepID(const CkGroupID &gid) {
    depGroupIDs.push_back(gid);
    return *this;
  }

  /// These are used by CkAllocateMarshallMsg, below:
  inline int getQueueing(void) const { return queueingtype; }
  inline int getPriorityBits(void) const { return prioBits; }
  inline const prio_t *getPriorityPtr(void) const { return prioPtr; }
  inline CkGroupID getGroupDepID() const { return depGroupIDs[0]; }
  inline CkGroupID getGroupDepID(int index) const { return depGroupIDs[index]; }
  inline int getGroupDepSize() const {
    return sizeof(CkGroupID) * (depGroupIDs.size());
  }
  inline int getGroupDepNum() const { return depGroupIDs.size(); }
  inline const CkGroupID *getGroupDepPtr() const { return &(depGroupIDs[0]); }
};

#endif
