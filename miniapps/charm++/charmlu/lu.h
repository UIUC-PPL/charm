#include "manager.h"
#include "messages.h"
#include "scheduler.h"
#include "luConfig.h"
#include "lu.decl.h"
#include <ckmulticast.h>
#include <limits>
#include <map>
#include <algorithm>
#include <vector>
using std::min;

/**
 * 2D chare array that embodies a block of the input matrix
 *
 * Main participant in the LU factorization and solve phases
 */
class LUBlk: public CBase_LUBlk {
public:
  //------ Public Interface for Block Scheduler Object (for memory management) ------
  /// Broadcast the U downwards to the blocks in the same column
  void resetMessage(bool reverse);
  /// Sends out the block to requesting PE if / when this block has been factored
  void requestBlock(int pe, int rx, int ry);
  /// Gives the local scheduler object access to this block's data
  double *accessLocalBlock();

  /// Constructor
  LUBlk() : factored(false), blockPulled(0), blocksAfter(0), maxRequestingPEs(0) {
    // allow SDAG to initialize its internal state for this chare
    __sdag_init();
  }

  void init(const LUConfig _cfg, CProxy_LUMgr _mgr, CProxy_BlockScheduler bs,
	    CkCallback initDone, CkCallback fznDone, CkCallback slnDone);
  void prepareForActivePanel(rednSetupMsg *msg);
  ~LUBlk();
  LUBlk(CkMigrateMessage* m) { CkAbort("LU blocks not migratable yet"); }
  // Added for migration
  void pup(PUP::er &p) {  }

public:
  int internalStep;
  bool factored;
  blkMsg *LUmsg;

protected:
  // Internal functions for creating messages to encapsulate the priority
  blkMsg* createABlkMsg();
  CProxy_BlockScheduler scheduler;
  BlockScheduler *localScheduler;
  int l_block, u_block;

  /// Configuration settings
  LUConfig cfg;

  /// Variables used during factorization
  double *LU;
  int blkSize, numBlks;
  blkMsg *U;
  int activeCol, ind;

  LUMgr *mgr;

  /// Variables used only during solution
  double *bvec;

  // Timer for each block
  double startTime;

  // Variables for pivoting SDAG code
  bool ownedPivotThisStep;
  // Stores the local column max which is a candidate for that column's pivot element
  MaxElm pivotCandidate, pivot;
  int pivotBlk;
  /// Tag for all msgs associated with a single batch of pivots
  int pivotBatchTag;
  /// A map to store and optimize the pivot operations
  std::map<int,int> pivotRecords;
  /// The number of rows factored since the last batch of pivots
  int numRowsSinceLastPivotSend ;
  /// The number of pending incoming remote pivots in a given batch
  int pendingIncomingPivots;
  /// The suggested pivot batch size
  int suggestedPivotBatchSize;
  /// How many blocks in the trailing submatrix have pulled our data and consumed it?
  int blockPulled;
  /// How many blocks in the trailing submatrix live on processors that have asked for it?
  int blocksAfter;
  /// Which PE's schedulers have requested this block?
  std::vector<int> requestingPEs;
  int maxRequestingPEs;

  /// The sub-diagonal chare array section that will participate in pivot selection
  /// @note: Only the diagonal chares will create and mcast along this section
  CProxySection_LUBlk pivotSection;
  /// All pivot sections members will save a cookie to their section
  CkSectionInfo pivotCookie;

  // The panel of blocks below the active diagonal chare
  CProxySection_LUBlk activePanel;
  CProxySection_LUBlk rowBeforeDiag;
  CProxySection_LUBlk rowAfterDiag;
  CkSectionInfo rowBeforeCookie;
  CkSectionInfo rowAfterCookie;

  /// A pointer to the local branch of the multicast manager group that handles the pivot section comm
  CkMulticastMgr *mcastMgr;

  /// Pointer to a U msg indicating a pending L sub-block update. Used only in L chares
  UMsg *pendingUmsg;

  bool updateExecuted;
  CkCallback initDone, factorizationDone, solveDone;

  // System-generated macro for SDAG code
  LUBlk_SDAG_CODE

  private:
  /// Perform trailing update based on input matrices (dgemm)
  void updateMatrix(double *incomingL, double *incomingU);
  /// Performs the triangular solve required to compute a block of the U matrix (dtrsm)
  void computeU(double *LMsg);
  /// Schedule U to be sent downward to the blocks in the same column
  void scheduleDownwardU();
  // Schedule L to be sent rightwards to the blocks in the same row
  void scheduleRightwardL();
  // Copy received pivot data into its place in this block
  void applySwap(int row, int offset, const double *data, double b);
  // Exchange local data
  void swapLocal(int row1, int row2, int offset=0);
  void doPivotLocal(int row1, int row2);
  /// Record the effect of a pivot operation in terms of actual row numbers
  void recordPivot(const int r1, const int r2);
  // Is it time to send out the next batch of pivots
  bool shouldSendPivots();
  /// Periodically send out the agglomerated pivot operations
  void announceAgglomeratedPivots();
  /// Given a set of pivot ops, send out participating row chunks that you own
  void sendPendingPivots(const pivotSequencesMsg *msg);
  /// Find the local column max
  MaxElm findMaxElm(int startRow, int col, MaxElm first = MaxElm());
  /// Update the sub-block of this L block starting at specified
  /// offset from the active column
  void updateLsubBlock(int activeCol, double* U, int offset=1, int startingRow=0);
  /// Compute the multipliers based on the pivot value in the
  /// received row of U and also find the candidate pivot in
  /// the immediate next column (after updating it simultaneously)
  MaxElm computeMultipliersAndFindColMax(int col, double *U, int startingRow=0);
  /// Compute linearized array index from 2D matrix index
  inline int getIndex(int i, int j) {
    return i * blkSize + j;
  }

  bool isOnDiagonal()    { return thisIndex.x == thisIndex.y; }
  bool isAboveDiagonal() { return thisIndex.x <  thisIndex.y; }
  bool isBelowDiagonal() { return thisIndex.x >  thisIndex.y; }
};
