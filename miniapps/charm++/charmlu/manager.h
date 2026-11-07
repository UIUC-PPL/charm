#ifndef LU_MANAGER_H
#define LU_MANAGER_H

#include "luUtils.decl.h"
#include "messages.h"
#include <algorithm>

// Prefix notation:
// b: an index into an LUBlk chare array

enum PrioType {
  SEND_PIVOT_DATA, PROCESS_TRAILING_UPDATE, RECVL, DIAG_SEND_PIVOT,
  BELOW_SEND_USEG, DIAG_SEND_USEG, MULT_RECV_U, MULT_RECV_L,
  PIVOT_RIGHT_SEC, PIVOT_LEFT_SEC, PIVOT_CRITICAL, PIVOT_NOT_CRITICAL,
  GET_BLOCK, SEND_BLOCKS, PROCESS_COMPUTE_U
};

class LUMgr : public CBase_LUMgr {
protected:
  int blkSize;
  int matSize;
  int numBlks;
  LUMgr(int blkSize_, int matSize_) : blkSize(blkSize_), matSize(matSize_) {
    numBlks = matSize / blkSize;
  }

public:
  virtual int bitsOfPrio() { return 0; }
  virtual CkEntryOptions& setPrio(PrioType type, CkEntryOptions& opts,
                                  int y = 0 , int x = 0, int t = 0) = 0;
  virtual void setPrio(CkMessage *msg, PrioType type, int refnum = -1, int y = 0, int x = 0) = 0;
};

struct PrioLU : public LUMgr {
  PrioLU(int blkSize, int matSize) : LUMgr(blkSize, matSize) {}

  int bitsOfPrio() { return sizeof(int) * 8; }

  CkEntryOptions& setPrio(PrioType type, CkEntryOptions& opts, int y, int x, int t) {
    int prio = 0;
    switch (type) {
    case SEND_PIVOT_DATA: case DIAG_SEND_PIVOT:
      prio = -1;
      break;
    case PROCESS_COMPUTE_U: case RECVL:
      prio = -1;
      break;
    case GET_BLOCK:
      prio = -3;
      break;
    case SEND_BLOCKS:
      prio = -2;
      break;
    case PROCESS_TRAILING_UPDATE:
      if (y == t + 1)
        prio = 0;
      else
        prio = t * numBlks * blkSize + y * blkSize;
      break;
    }
    opts.setPriority(prio);
    return opts;
  }

  void setPrio(CkMessage *msg, PrioType type, int refnum, int y, int x) {
    int prio = 0;
    switch (type) {
    case DIAG_SEND_USEG: case BELOW_SEND_USEG: case MULT_RECV_U: case MULT_RECV_L:
      prio = -1;
      break;
    case PIVOT_RIGHT_SEC:
      prio = -1;
      break;
    case PIVOT_LEFT_SEC:
      prio = blkSize * numBlks * numBlks + 1;
      break;
    case PIVOT_CRITICAL:
      prio = -1;
      break;
    case PIVOT_NOT_CRITICAL:
      prio = numBlks * numBlks * blkSize;
      break;
    }
    *(int*)CkPriorityPtr(msg) = prio;
    CkSetQueueing(msg, CK_QUEUEING_IFIFO);
    // Better way than this?
    if (refnum != -1)
      CkSetRefNum(msg, refnum);
  }
};

#endif
