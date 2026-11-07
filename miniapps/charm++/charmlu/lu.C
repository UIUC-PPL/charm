/**
 * @file A Charm++ implementation of LU
 */

#include <algorithm>
using std::min;

#include <cmath>
#include <limits>
#include <map>
#include <string.h>

#include "lu.h"
#include "mapping.h"
#include "platformBlas.h"
#include "driver.decl.h"

#include "traceToggler.h"

#if defined(TRACE_TOGGLER_H)
// define static variable
int traceToggler::traceCmdHandlerID;
#endif

// Execute a trailing update
void LUBlk::updateMatrix(double *incomingL, double *incomingU) {
#if USE_ESSL || USE_ACML
  // By switching the order of incomingU and incomingL the transpose
  // is applied implicitly: C' = B*A
  dgemm( BLAS_NOTRANSPOSE, BLAS_NOTRANSPOSE,
         blkSize, blkSize, blkSize,
         -1.0, incomingU,
         blkSize, incomingL, blkSize,
         1.0, LU, blkSize);
#else
  cblas_dgemm( CblasRowMajor,
               CblasNoTrans, CblasNoTrans,
               blkSize, blkSize, blkSize,
               -1.0, incomingL,
               blkSize, incomingU, blkSize,
               1.0, LU, blkSize);
#endif
}

/// Given a set of pivot ops, send out participating row chunks that you own
void LUBlk::sendPendingPivots(const pivotSequencesMsg *msg) {
  const int *pivotSequence = msg->pivotSequence, *idx = msg->seqIndex;
  int numSequences = msg->numSequences;

  // Count the number of rows that I send to each chare
  int numMsgsTo[numBlks];
  memset(numMsgsTo, 0, sizeof(int)*numBlks);
  for (int i = 0; i < numSequences; i++) {
    for (int j = idx[i]; j < idx[i+1]; j++) {
      int recverIdx = pivotSequence[j]/blkSize;
      int senderIdx = -1;
      // circular traversal of pivot sequence
      if (j < idx[i+1]-1)
        senderIdx = pivotSequence[j+1]/blkSize;
      else
        senderIdx = pivotSequence[idx[i]]/blkSize;
      // If this chare the sending to another chare
      if (thisIndex.x == senderIdx && thisIndex.x != recverIdx)
        numMsgsTo[recverIdx]++;
    }
  }

  // Preallocate messages that are large enough to carry the rows that this
  // block will send to each of the other chares
  pivotRowsMsg* outgoingPivotMsgs[numBlks];
  memset(outgoingPivotMsgs, 0, sizeof(pivotRowsMsg*) * numBlks);
  for (int i = 0; i < numBlks; i++) {
    // If this other chare expects row data from me
    if (numMsgsTo[i] > 0) {
      // Create a big enough msg to carry all the rows I'll be sending to this chare
      outgoingPivotMsgs[i] = new (numMsgsTo[i], numMsgsTo[i]*blkSize, numMsgsTo[i], sizeof(int)*8)
        pivotRowsMsg(blkSize, pivotBatchTag);
      // Set a priority thats a function of your location wrt to the critical path
      if (thisIndex.y < internalStep)
        mgr->setPrio(outgoingPivotMsgs[i], PIVOT_NOT_CRITICAL);
      else
        mgr->setPrio(outgoingPivotMsgs[i], PIVOT_CRITICAL);
    }
  }

  pendingIncomingPivots = 0;
  double *tmpBuf = 0, tmpB;

  // Parse each sequence independently
  for (int i=0; i < numSequences; i++) {

    // Find the location of this sequence in the msg buffer
    const int *first = pivotSequence + idx[i];
    const int *beyondLast = pivotSequence + idx[i+1];
    CkAssert(beyondLast - first >= 2);

    // Identify a remote row in the pivot sequence as a point at which to
    // start and stop processing the circular pivot sequence
    const int *ringStart = first;
    while ((*ringStart / blkSize == thisIndex.x) && (ringStart < beyondLast))
      ringStart++;
    const int *ringStop = ringStart;

    // If there are no remote rows in the sequence, we *have* to use a tmp buffer
    // The tmp buffer will now complete the circular sequence
    bool isSequenceLocal = false;
    if (ringStart == beyondLast) {
      isSequenceLocal = true;
      ringStart = first;
      ringStop  = beyondLast - 1;

      if (tmpBuf == 0) tmpBuf = new double[blkSize];
      memcpy(tmpBuf, &LU[getIndex(*ringStart%blkSize,0)], blkSize*sizeof(double));
      tmpB = bvec[*ringStart%blkSize];
    }

    // Process all the pivot operations in the circular sequence
    const int *to = ringStart;
    do {
      const int *from = (to + 1 == beyondLast) ? first : to + 1;
      int fromChareIdx = *from / blkSize;
      // If the current source row in the pivot sequence belongs to me, send it
      if (fromChareIdx == thisIndex.x) {
        int toChareIdx = *to / blkSize;
        int fromLocal  = *from % blkSize;
        // If you're sending to yourself, memcopy
        if (toChareIdx == thisIndex.x) {
          applySwap(*to%blkSize, 0, &LU[getIndex(fromLocal,0)], bvec[fromLocal]);
        }
        // else, copy the data into the appropriate msg
        else {
          outgoingPivotMsgs[*to/blkSize]->copyRow(*to, &LU[getIndex(fromLocal,0)], bvec[fromLocal]);
        }
      }
      // else, the source data is remote
      else {
        // if the current destination row belongs to me, make sure I expect the remote data
        if (*to / blkSize == thisIndex.x) {
          pendingIncomingPivots++;
        }
        // else, i dont worry about this portion of the exchange sequence which is completely remote
        else {
        }
      }
      // Setup a circular traversal of the pivot sequence
      if (++to == beyondLast)
        to = first;
    } while (to != ringStop); // Keep going till you complete the ring

    // If the sequence was completely local, complete the circular sequence
    // by copying the temp buffer back into the matrix block
    if (isSequenceLocal) {
      applySwap(*(beyondLast - 1) % blkSize, 0, tmpBuf, tmpB);
    }
  } // end for loop through all sequences

  // Send out all the msgs carrying pivot data to other chares
  for (int i=0; i< numBlks; i++)
    if (numMsgsTo[i] > 0)
      thisProxy(i, thisIndex.y).trailingPivotRowsSwap(outgoingPivotMsgs[i]);

  if (tmpBuf) delete [] tmpBuf;
}

// Copy received pivot data into its place in this block
void LUBlk::applySwap(int row, int offset, const double *data, double b) {
  bvec[row] = b;
  memcpy( &(LU[getIndex(row,offset)]), data, sizeof(double)*(blkSize-offset) );
}

// Exchange local data
void LUBlk::swapLocal(int row1, int row2, int offset) {
  if (row1 == row2) return;
  std::swap(bvec[row1], bvec[row2]);
  /// @todo: Is this better or is it better to do 3 memcpys
  std::swap_ranges(&(LU[getIndex(row1,offset)]), &(LU[getIndex(row1,blkSize)]), &(LU[getIndex(row2,offset)]) );
}

// Compute a triangular solve
void LUBlk::computeU(double *LMsg) {
#if USE_ESSL || USE_ACML
  // LMsg is implicitly transposed by telling dtrsm that it is a
  // right, upper matrix. Since this also switches the order of
  // multiplication, the transpose is output to LU.
  dtrsm(BLAS_RIGHT, BLAS_UPPER, BLAS_NOTRANSPOSE, BLAS_UNIT, blkSize, blkSize, 1.0, LMsg, blkSize, LU, blkSize);
#else
  cblas_dtrsm(CblasRowMajor, CblasLeft, CblasLower, CblasNoTrans, CblasUnit, blkSize, blkSize, 1.0, LMsg, blkSize, LU, blkSize);
#endif
}

// Schedule U to be sent downward to the blocks in the same column
inline void LUBlk::scheduleDownwardU() {
  mgr->setPrio(LUmsg, MULT_RECV_U);
  localScheduler->scheduleSend(thisIndex, internalStep == thisIndex.y - 1);
}

MaxElm LUBlk::findMaxElm(int startRow, int col, MaxElm first) {
  MaxElm l = first;
  for (int row = startRow; row < blkSize; row++)
    if (fabs(LU[getIndex(row,col)]) > fabs(l.val)) {
      l.val = LU[getIndex(row,col)];
      l.loc = row + blkSize * thisIndex.x;
    }
  return l;
}

CkReductionMsg *MaxElm_max(int nMsg, CkReductionMsg **msgs) {
  CkAssert(nMsg > 0);

  MaxElm *l = (MaxElm*) msgs[0]->getData();
  for (int i = 1; i < nMsg; ++i) {
    MaxElm *n = (MaxElm *) msgs[i]->getData();
    if (fabs(n->val) > fabs(l->val))
      l = n;
  }

  return CkReductionMsg::buildNew(sizeof(MaxElm), l);
}

/// Global that holds the reducer type for MaxElm
CkReduction::reducerType MaxElmReducer;

/// Function that registers this reducer type on every processor
void registerMaxElmReducer() {
  MaxElmReducer = CkReduction::addReducer(MaxElm_max);
}

/// Update the sub-block of this L block starting at specified
/// offset from the active column
void LUBlk::updateLsubBlock(int activeCol, double* U, int offset, int startingRow) {
  // Should only get called on L blocks
  CkAssert(isOnDiagonal() || isBelowDiagonal());
  // Check for input edge cases
  if ((activeCol + offset) >= blkSize || startingRow >= blkSize)
    return;
#if USE_ESSL || USE_ACML
  dger(blkSize-(activeCol+offset), blkSize-startingRow,
       -1.0,
       U+offset, 1,
       &LU[getIndex(startingRow,activeCol)], blkSize,
       &LU[getIndex(startingRow,activeCol+offset)], blkSize);
#elif USE_ACCELERATE_BLAS
  for(int j = startingRow; j < blkSize; j++)
    for(int k = activeCol+offset; k<blkSize; k++)
      LU[getIndex(j,k)] -=  LU[getIndex(j,activeCol)] * U[k-activeCol];
#else
  cblas_dger(CblasRowMajor,
             blkSize-startingRow, blkSize-(activeCol+offset),
             -1.0,
             &LU[getIndex(startingRow,activeCol)], blkSize,
             U+offset, 1,
             &LU[getIndex(startingRow,activeCol+offset)], blkSize);
#endif
}

/// Compute the multipliers based on the pivot value in the
/// received row of U and also find the candidate pivot in
/// the immediate next column (after updating it simultaneously)
MaxElm LUBlk::computeMultipliersAndFindColMax(int col, double *U, int startingRow) {
  // Should only get called on L blocks
  CkAssert(isOnDiagonal() || isBelowDiagonal());
  MaxElm maxVal;
  // Check for input edge cases
  if (col >= blkSize || startingRow >= blkSize)
    return maxVal;

  if (col < blkSize -1) {
    for (int j = startingRow; j < blkSize; j++) {
      // Compute the multiplier
      LU[getIndex(j,col)]    = LU[getIndex(j,col)] / U[0];
      // Update the immediate next column
      LU[getIndex(j,col+1)] -= LU[getIndex(j,col)] * U[1];
      // Update the max value thus far
      if ( fabs(LU[getIndex(j,col+1)]) > fabs(maxVal.val) ) {
        maxVal.val = LU[getIndex(j,col+1)];
        maxVal.loc = j;
      }
    }
    // Convert local row num to global rownum
    maxVal.loc += thisIndex.x*blkSize;
  }
  else {
    for (int j = startingRow; j < blkSize; j++)
      LU[getIndex(j,col)]    = LU[getIndex(j,col)] / U[0];
  }

  return maxVal;
}

// Schedule L to be sent rightward to the blocks in the same row
inline void LUBlk::scheduleRightwardL() {
  mgr->setPrio(LUmsg, MULT_RECV_L);
  localScheduler->scheduleSend(thisIndex, true);
}

/**
 * Is it time to send out the next batch of pivots?
 * @note: Any runtime adaptivity should be plugged here
 */
bool LUBlk::shouldSendPivots() {
  return (numRowsSinceLastPivotSend >= suggestedPivotBatchSize);
}

/// Periodically send out the agglomerated pivot operations
void LUBlk::announceAgglomeratedPivots() {
  // Create and initialize a msg to carry the pivot sequences
  pivotSequencesMsg *msg = new (numRowsSinceLastPivotSend+1, numRowsSinceLastPivotSend*2, sizeof(int)*8) pivotSequencesMsg(pivotBatchTag, numRowsSinceLastPivotSend);
  msg->numSequences = 0;
  memset(msg->seqIndex, 0, sizeof(int) * numRowsSinceLastPivotSend);
  memset(msg->pivotSequence, 0, sizeof(int) * numRowsSinceLastPivotSend*2);

  /// Parse the pivot operations and construct optimized pivot sequences
  int seqNo = -1, i = 0;
  std::map<int,int>::iterator itr = pivotRecords.begin();
  while (itr != pivotRecords.end()) {
    msg->seqIndex[++seqNo] = i;
    int chainStart = itr->first;
    msg->pivotSequence[i++] = chainStart;
    while (itr->second != chainStart) {
      msg->pivotSequence[i] = itr->second;
      std::map<int,int>::iterator prev = itr;
      itr = pivotRecords.find(itr->second);
      pivotRecords.erase(prev);
      i++;
    }
    pivotRecords.erase(itr);
    itr = pivotRecords.begin();
  }
  msg->seqIndex[++seqNo] = i; ///< @note: Just so that we know where the last sequence ends
  msg->numSequences = seqNo;

  mgr->setPrio(msg, PIVOT_RIGHT_SEC, -1, thisIndex.y);
  thisProxy.applyTrailingPivots(msg);

  // Prepare for the next batch of agglomeration
  pivotRecords.clear();
  pivotBatchTag += numRowsSinceLastPivotSend;
  numRowsSinceLastPivotSend = 0;
}

/// Record the effect of a pivot operation in terms of actual row numbers
void LUBlk::recordPivot(const int r1, const int r2) {
  numRowsSinceLastPivotSend++;
  // If the two rows are the same, then dont record the pivot operation at all
  if (r1 == r2) return;
  std::map<int,int>::iterator itr1, itr2;
  // The records for the two rows (already existing or freshly created)
  itr1 = (pivotRecords.insert(std::make_pair(r1,r1))).first;
  itr2 = (pivotRecords.insert(std::make_pair(r2,r2))).first;
  // Swap the values (the actual rows living in these two positions)
  std::swap(itr1->second, itr2->second);
}

void LUBlk::resetMessage(bool reverse) {
  // Setup multicast of message to a dynamic set of processors
  blkMsg *m = LUmsg;

  CkAssert(requestingPEs.size() <= maxRequestingPEs);

  std::sort(requestingPEs.begin(), requestingPEs.end());
  if (reverse) std::reverse(requestingPEs.begin(), requestingPEs.end());

  // Junk value to catch bugs
  m->npes_sender = -1;
  m->npes_receiver = requestingPEs.size();
  m->offset = 0;
  memcpy(m->pes, &requestingPEs[0], sizeof(requestingPEs[0])*m->npes_receiver);

  requestingPEs.clear();
}

void LUBlk::requestBlock(int pe, int rx, int ry) {
  requestingPEs.push_back(pe);
  if (factored) {
    bool onActive = false;
    if      (isBelowDiagonal() && internalStep == thisIndex.y)
      onActive = true;
    else if (isAboveDiagonal() && internalStep == thisIndex.y - 1)
      onActive = true;

    localScheduler->scheduleSend(thisIndex, onActive);
  }
}

double* LUBlk::accessLocalBlock() {
  return LU;
}

// Internal functions for creating messages to encapsulate the priority
blkMsg* LUBlk::createABlkMsg() {
  int prioBits = mgr->bitsOfPrio();
  maxRequestingPEs = CProxy_LUMap(cfg.map).ckLocalBranch()->pesInPanel(thisIndex);
  blkMsg *msg = new (blkSize*blkSize, maxRequestingPEs, prioBits) blkMsg(thisIndex);
  memset(msg->pes, -1, maxRequestingPEs*sizeof(int));
  return msg;
}

void LUBlk::init(const LUConfig _cfg, CProxy_LUMgr _mgr,
                 CProxy_BlockScheduler bs,
		 CkCallback initialization, CkCallback factorization, CkCallback solution) {
  scheduler = bs;
  localScheduler = scheduler[CkMyPe()].ckLocal();
  CkAssert(localScheduler);
  localScheduler->registerBlock(thisIndex);
  contribute(CkCallback(CkIndex_BlockScheduler::allRegistered(NULL), bs));
  cfg = _cfg;
  blkSize = cfg.blockSize;
  numBlks = cfg.numBlocks;
  mgr = _mgr.ckLocalBranch();
  suggestedPivotBatchSize = cfg.pivotBatchSize;
  initDone = initialization;
  factorizationDone = factorization;
  solveDone = solution;
  internalStep = 0;

  CkAssert(blkSize > 0);

  CkMulticastMgr *mcastMgr = CProxy_CkMulticastMgr(cfg.mcastMgrGID).ckLocalBranch();

  /// Chares on the active panels will create sections of their brethren
#if defined(CHARMLU_USEG_FROM_BELOW)
  if (isOnDiagonal() || isBelowDiagonal())
#else
  if (isOnDiagonal())
#endif
  {
    // Elements in the active panel, not including this block
    CkVec<CkArrayIndex2D> activeElems;
    for (int i = thisIndex.y+1; i < numBlks; i++)
      if (i != thisIndex.x)
        activeElems.push_back(CkArrayIndex2D(i, thisIndex.y));
    activePanel = CProxySection_LUBlk::ckNew(thisArrayID, activeElems.getVec(), activeElems.size());
    activePanel.ckSectionDelegate(mcastMgr);
    rednSetupMsg *activePanelMsg = new rednSetupMsg(cfg.mcastMgrGID);
    activePanel.prepareForActivePanel(activePanelMsg);
  }
  /// Chares on the array diagonal will now create pivot sections that they will talk to
  if (isOnDiagonal()) {
    // Create the pivot section
    pivotSection = CProxySection_LUBlk::ckNew(thisArrayID, thisIndex.x,numBlks-1,1,thisIndex.y,thisIndex.y,1);
    rowBeforeDiag = CProxySection_LUBlk::ckNew(thisArrayID, thisIndex.x,thisIndex.x,1,0,thisIndex.y-1,1);
    rowAfterDiag = CProxySection_LUBlk::ckNew(thisArrayID, thisIndex.x,thisIndex.x,1,thisIndex.y+1,numBlks-1,1);
    // Delegate pivot section to the manager
    pivotSection.ckSectionDelegate(mcastMgr);
    rowBeforeDiag.ckSectionDelegate(mcastMgr);
    rowAfterDiag.ckSectionDelegate(mcastMgr);
    // Set the reduction client for this pivot section
    mcastMgr->setReductionClient( pivotSection, new CkCallback( CkReductionTarget(LUBlk,colMax), thisProxy(thisIndex.y, thisIndex.y) ) );

    // Invoke a dummy mcast so that all the section members know which section to reduce along
    rednSetupMsg *pivotMsg = new rednSetupMsg(cfg.mcastMgrGID);
    rednSetupMsg *rowBeforeMsg = new rednSetupMsg(cfg.mcastMgrGID);
    rednSetupMsg *rowAfterMsg = new rednSetupMsg(cfg.mcastMgrGID);

    pivotSection.prepareForPivotRedn(pivotMsg);
    rowBeforeDiag.prepareForRowBeforeDiag(rowBeforeMsg);
    rowAfterDiag.prepareForRowAfterDiag(rowAfterMsg);

    if (thisIndex.x == 0) {
      thisProxy.multicastRedns();
    }
  }

  CkArrayID benchmark = thisProxy;
  CProxy_BenchmarkLUBlk b = benchmark;
  b(thisIndex).initVec();      

  // All chares except members of pivot sections are done with init
}

void LUBlk::prepareForActivePanel(rednSetupMsg *msg) { }

LUBlk::~LUBlk() {
  delete LUmsg;
  LU = NULL;
}

#include "luUtils.def.h"
#include "lu.def.h"
#include "luMessages.def.h"
