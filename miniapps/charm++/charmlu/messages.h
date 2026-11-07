#ifndef LU_MESSAGES_H
#define LU_MESSAGES_H

#include "luMessages.decl.h"
#include <ckmulticast.h>
#include <algorithm>

/// Utility to retain a reference to a reference-counted message
static inline void takeRef(void *m) { CmiReference(UsrToEnv(m)); }
/// Utility to drop a reference to a reference-counted message
static inline void dropRef(void *m) { CmiFree(UsrToEnv(m)); }

/// A block of the matrix, with ornamentation for setup-free multicast
struct blkMsg: public CkMcastBaseMsg, CMessage_blkMsg {
  /// Payload
  double *data;
  CkIndex2D src;

  /// Destinations
  int *pes;
  int npes_sender, npes_receiver, offset;
  /// Whether the message has gone to the first half of its destination list
  bool firstHalfSent;

  blkMsg(CkIndex2D index_)
    : src(index_), npes_sender(0), npes_receiver(0), offset(0), firstHalfSent(false) {
    CkSetRefNum(this, std::min(src.x, src.y));
    CkSetQueueing(this, CK_QUEUEING_IFIFO);
  }
};

struct BlockReadyMsg : public CkMcastBaseMsg, CMessage_BlockReadyMsg {
  CkIndex2D src;

  BlockReadyMsg(CkIndex2D idx) : src(idx) {
    CkSetRefNum(this, std::min(src.x, src.y));
  }
};

struct rednSetupMsg: public CkMcastBaseMsg, public CMessage_rednSetupMsg {
  CkGroupID rednMgrGID;

  rednSetupMsg(CkGroupID _gid): rednMgrGID(_gid) {}
};

struct BVecMsg : public CMessage_BVecMsg, public CkMcastBaseMsg {
  /// The post-diagonal portion of the active row
  double *data;
  bool forward;

  BVecMsg(const int numElements, double *bvec, bool _forward) {
    memcpy(data, bvec, numElements * sizeof(double));
    forward = _forward;
  }
};

struct UMsg : public CMessage_UMsg, public CkMcastBaseMsg {
  /// The post-diagonal portion of the active row
  double *data;

  UMsg(const int numElements, double *useg) {
    memcpy(data, useg, numElements * sizeof(double));
  }
};

struct pivotSequencesMsg: public CMessage_pivotSequencesMsg, public CkMcastBaseMsg {
  int numRowsProcessed, numSequences;
  int *seqIndex, *pivotSequence;

  pivotSequencesMsg(const int _firstRowProcessed, const int _numRowsProcessed) {
    numRowsProcessed = _numRowsProcessed;
    numSequences = 0;
    CkSetRefNum(this, _firstRowProcessed);
  }
};

struct pivotRowsMsg: public CMessage_pivotRowsMsg, public CkMcastBaseMsg {
  int nRows, blockSize;
  int *rowNum;
  double *rows, *rhs;

  pivotRowsMsg(const int _blockSize, const int _refNum):
    nRows(0), blockSize(_blockSize) {
    CkSetRefNum(this, _refNum);
  }

  void copyRow(const int rNum, const double *row, const double b) {
    rowNum[nRows] = rNum;
    rhs[nRows] = b;
    memcpy(&rows[nRows*blockSize], row, blockSize*sizeof(double));
    nRows++;
  }
};
#endif
