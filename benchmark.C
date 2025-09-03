#include "luConfig.h"
#include "platformBlas.h"
#include "driver.decl.h"
#include <ckmulticast.h>
#include "lu.h"

#include <algorithm>
#include <utility>
#include <cmath>
#include <limits>
#include <iostream>

#if CHARMLU_DEBUG >= 2
  #define VERBOSE_VALIDATION(...) CkPrintf(__VA_ARGS__)
#else
  #define VERBOSE_VALIDATION(...)
#endif

#define _QUOTEIT(x) #x
#define INQUOTES(x) _QUOTEIT(x)

/// The build system should define this macro to be the commit identifier
#ifndef LU_REVISION
    #define LU_REVISION Unknown
#endif

//A class for randomly generating matrix elements' value
#define MAXINT (~(1<<31))
class MatGen {
private:
  //variables for generating the sequence of random numbers
  int curRnd;
  int rndA;
  int rndQ;
  int rndR;

public:
  MatGen(int seed) {
    curRnd = seed;
    rndA = 48271;
    rndQ = MAXINT / rndA;
    rndR = MAXINT % rndA;
  }

  int nextRndInt() {
    curRnd = rndA * (curRnd % rndQ) - rndR * (curRnd / rndQ);
    if (curRnd < 0) curRnd += MAXINT;
    return curRnd;
  }

  //The range of the returned double random number is [-0.5, 0.5]
  double toRndDouble(int rndInt) {
    return (double)rndInt / MAXINT - 0.5;
  }

  double nextRndDouble() {
    return toRndDouble(nextRndInt());
  }

  void getNRndInts(int num, int *d) {
    for (int i = 0; i < num; i++)
      d[i] = nextRndInt();
  }

  void getNRndDoubles(int num, double *d) {
    for (int i = 0; i < num; i++)
      d[i] = nextRndDouble();
  }

  void skipNDoubles(int num) {
    for (int i = 0; i < num; i++)
      nextRndDouble();
  }
};

struct Benchmark : public CBase_Benchmark {
  int numIterations;

  Benchmark(CkArgMsg* m)
    : numIterations(1) {
    if (m->argc<4) {
      CkPrintf("Usage: %s <matrix size> <block size> <mem threshold>"
               " [<pivot batch size> <mapping scheme>"
               " [<peTileRows> <peTileCols> <peRotate> <peStride>] ]\n",
               m->argv[0]);
      CkExit();
    }

    LUConfig luCfg;
    /// Parse the command line and accept user input
    luCfg.matrixSize = atoi(m->argv[1]);
    luCfg.blockSize = atoi(m->argv[2]);
    luCfg.memThreshold = atoi(m->argv[3]);

    if (m->argc >= 5)
      luCfg.pivotBatchSize = atoi(m->argv[4]);
    else
      luCfg.pivotBatchSize = luCfg.blockSize / 4;

    if (m->argc >= 6) {
      luCfg.mappingScheme = atoi(m->argv[5]);
      if (luCfg.mappingScheme == 3) {
        if (m->argc < 8) {
          std::pair<int,int> tileDims = computePETileDimensions();
          luCfg.peTileRows = tileDims.first;
          luCfg.peTileCols = tileDims.second;
        }
        else {
          luCfg.peTileRows = atoi(m->argv[6]);
          luCfg.peTileCols = atoi(m->argv[7]);
        }
        if (m->argc < 9)
          luCfg.peTileRotate = 0;
        else
          luCfg.peTileRotate = atoi(m->argv[8]);

        if (m->argc < 10)
          luCfg.peTileStride = luCfg.peTileCols;
        else
          luCfg.peTileStride = atoi(m->argv[9]);

        int peTileSize = luCfg.peTileRows * luCfg.peTileCols;

        if (peTileSize > CkNumPes())
          CkAbort("The PE tile dimensions are too big for the num of PEs available!");

        if (peTileSize < CkNumPes())
          CkPrintf("WARNING: Configured to use a PE tile size (%d x %d)"
                   " (for 2D tile mapping) that does not use all the PEs(%d)\n",
                   luCfg.peTileRows, luCfg.peTileCols, CkNumPes());
      }
    }
    else
      luCfg.mappingScheme = 2;

    if (luCfg.matrixSize >= std::numeric_limits<short>::max() &&
        sizeof(CMK_REFNUM_TYPE) != sizeof(int)) {
      CkPrintf("Refnum size too small for large matrices."
               " Compile charm with build option --with-refnum-type=int\n");
      CkExit();
    }

    if (luCfg.matrixSize % luCfg.blockSize != 0) {
      CkPrintf("The matrix size %d should be a multiple of block size %d!\n",
               luCfg.matrixSize, luCfg.blockSize);
      CkExit();
    }

    luCfg.numBlocks = luCfg.matrixSize / luCfg.blockSize;

    CkPrintf("Running LU compiled from revision: " INQUOTES(LU_REVISION) "\n");
    CkPrintf("Running LU on %d processors (%d nodes): "
             "\n\tMatrix size: %d X %d "
             "\n\tBlock size: %d X %d "
             "\n\tChare Array size: %d X %d"
             "\n\tPivot batch size: %d"
             "\n\tMem Threshold (MB): %d"
             "\n\tSend Limit: %d"
             "\n\tMapping Scheme: %d (%s)\n",
             CkNumPes(), CmiNumNodes(),
             luCfg.matrixSize, luCfg.matrixSize,
             luCfg.blockSize, luCfg.blockSize,
             luCfg.numBlocks, luCfg.numBlocks,
             luCfg.pivotBatchSize,
             luCfg.memThreshold,
             SEND_LIM,
             luCfg.mappingScheme,
             (luCfg.mappingScheme == 2 ? "Block Cyclic" : "2D Tiling")
             );
    if (luCfg.mappingScheme == 3)
      CkPrintf("\tMapping PE tile size: %d x %d"
               " rotate %d stride %d \n", luCfg.peTileRows, luCfg.peTileCols,
               luCfg.peTileRotate, luCfg.peTileStride);

#ifdef SCHED_PIVOT_REDN
    CkPrintf("\tPivot Redn Scheduling: On\n");
#else
    CkPrintf("\tPivot Redn Scheduling: Off\n");
#endif
#if defined(CHARMLU_USEG_FROM_BELOW)
    CkPrintf("\tUseg Multicast from: Below\n");
#else
    CkPrintf("\tUseg Multicast from: Diagonal\n");
#endif

    for (int i = 0; i < numIterations; i++) {
      ckout << "Starting solve" << endl;
      CkCallback cb(CkIndex_Benchmark::finished(), thisProxy);
      CProxy_LUSolver solver = CProxy_LUSolver::ckNew(luCfg, cb);
    }
  }

  std::pair<int,int> computePETileDimensions() {
    // Identify two factors that can be used as the tile dimensions for the PE
    // tile
    int factor1 = std::sqrt((double)CkNumPes());
    while (CkNumPes() % factor1 != 0 && factor1 > 0)
      factor1--;
    if (factor1 == 0)
      CkAbort("Couldn't identify a factor of numPEs to"
              " represent the PEs as a 2D tile");

    int factor2 = CkNumPes() / factor1;

    // Set the tile dimensions
    int numPErows = (factor1 >= factor2) ? factor1 : factor2;
    int numPEcols = CkNumPes() / numPErows;

    if (numPErows * numPEcols != CkNumPes())
      CkAbort("The identified tile dimensions dont match the number of PEs!!");

    return std::make_pair(numPErows, numPEcols);
  }

  void finished() {
    ckout << "Finished solve" << endl;
    if (--numIterations == 0)
      CkExit();
  }
};

class LUSolver : public CBase_LUSolver {
  LUConfig luCfg;
  double startTime;
  bool solved, LUcomplete;
  CProxy_BenchmarkLUBlk luArrProxy;
  CProxy_BlockScheduler bs;
  CkCallback finishedSolve;

public:
  LUSolver(LUConfig luCfg_, CkCallback finishedSolve)
  : solved(false)
  , LUcomplete(false)
  , luCfg(luCfg_)
  , finishedSolve(finishedSolve) {
    // Create a multicast manager group
    luCfg.mcastMgrGID = CProxy_CkMulticastMgr::ckNew();

    thisProxy.startNextStep();
  }

  void continueIter() {
    startTime = CmiWallTimer();
    luArrProxy.factor();
  }

  void startNextStep() {
    if (solved && LUcomplete) {
      outputStats();
      //Perform validation
      CkPrintf("starting validation at wall time: %f\n", CmiWallTimer());
      luArrProxy.startValidation();
    } else if (!solved && LUcomplete) {
      CkPrintf("starting solve at wall time: %f\n", CmiWallTimer());
      for (int i = 0; i < luCfg.numBlocks; i++)
        luArrProxy(i, i).forwardSolve();
      solved = true;
    } else {
      CkArrayOptions opts(luCfg.numBlocks, luCfg.numBlocks);
      opts.setAnytimeMigration(false)
	  .setStaticInsertion(true);
      CkGroupID map;
      switch (luCfg.mappingScheme) {
      case 2:
        map = CProxy_BlockCyclicMap::ckNew(1, luCfg.numBlocks);
        break;
      case 3:
        map = CProxy_PE2DTilingMap::ckNew(luCfg.peTileRows, luCfg.peTileCols,
                                          luCfg.peTileRotate, luCfg.peTileStride,
					  luCfg.numBlocks);
        break;
      default:
        CkAbort("Unrecognized mapping scheme specified");
      }

      luCfg.map = map;
      opts.setMap(map);

      CProxy_LUMgr mgr = CProxy_PrioLU::ckNew(luCfg.blockSize, luCfg.matrixSize);

      luArrProxy = CProxy_BenchmarkLUBlk::ckNew(thisProxy, opts);

      CkArrayOptions bsOpts(CkNumPes());
      bsOpts.setMap(CProxy_OnePerPE::ckNew());
      bs = CProxy_BlockScheduler::ckNew(luArrProxy, luCfg, mgr, bsOpts);

      LUcomplete = true;

      luArrProxy.startup(luCfg, mgr, bs,
			 CkCallback(CkIndex_LUSolver::continueIter(), thisProxy),
			 CkCallback(CkIndex_LUSolver::startNextStep(), thisProxy),
			 CkCallback(CkIndex_LUSolver::startNextStep(), thisProxy));
      //luArrProxy.initVec();
    }
  }

  /// Returns how long a single dgemm of given block size takes
  double testdgemm(unsigned long blocksize) {
    double duration = 0;
    int numTrials = 10;

    /// Possible cache warmth/cold issues here because we create and initialize just before use
    for (int i=0; i<numTrials; i++) {
      MatGen rnd(0);

      // Touch the output matrix first to avoid keeping it really warm in the cache
      double *m3 = new double[blocksize*blocksize];
      rnd.getNRndDoubles(blocksize * blocksize, m3);

      double *m1 = new double[blocksize*blocksize];
      rnd.getNRndDoubles(blocksize * blocksize, m1);
      double *m2 = new double[blocksize*blocksize];
      rnd.getNRndDoubles(blocksize * blocksize, m2);

      double startTest = CmiWallTimer();

#if USE_ESSL || USE_ACML
      dgemm(BLAS_NOTRANSPOSE, BLAS_NOTRANSPOSE,
            blocksize, blocksize, blocksize,
            -1.0, m1,
            blocksize, m2, blocksize,
            1.0, m3, blocksize);
#else
      cblas_dgemm(CblasRowMajor,
                  CblasNoTrans, CblasNoTrans,
                  blocksize, blocksize, blocksize,
                  -1.0, m1,
                  blocksize, m2, blocksize,
                  1.0, m3, blocksize);
#endif

      double endTest = CmiWallTimer();
      duration += endTest-startTest;

      delete[] m1;
      delete[] m2;
      delete[] m3;
    }

    return duration / numTrials;
  }

  void outputStats() {
    double endTime = CmiWallTimer();
    double duration = endTime-startTime;

    double n = luCfg.matrixSize;
    double HPL_flop_count =  (2.0 / 3.0 * n * n * n + 3.0 / 2.0 * n * n) / duration ;
    double HPL_gflops =	 HPL_flop_count / 1000000000.0; // Giga fp ops per second
    double gflops_per_core = HPL_gflops / (double)CkNumPes();

    std::cout << "RESULT procs: \t" << CkNumPes()
              << "\tblock size:\t"  << luCfg.blockSize
              << "\tTime(s):\t"     << duration
              << std::endl;
    std::cout << "HPL flop count gives \t" << HPL_gflops << "\tGFlops" << std::endl;

    struct {
      const char *machine;
      double gflops_per_core;
    } peaks[] = {{"Order", 7.4585},
		 {"Abe", 9.332},
                 {"Ranger", 9.2},
                 {"Jaguar/Kraken XT5", 10.3987},
		 {"BG/P", 3.4}
		 ,{"Jaguar XK6 sans GPUs", 8.8}
    };

    for (int i = 0; i < sizeof(peaks)/sizeof(peaks[0]); ++i) {
      double fractionOfPeak = gflops_per_core / peaks[i].gflops_per_core;
      std::cout <<  peaks[i].machine << ":\t"
		<< 100.0*fractionOfPeak << "% of peak" << std::endl;
    }
    double dgemmDuration    = testdgemm(luCfg.blockSize);
    double dgemmFlopCount   = (double)luCfg.blockSize * (double)luCfg.blockSize * (double)luCfg.blockSize * 2.0;
    double dgemmGFlopCount  = dgemmFlopCount / 1000000000.0;
    double dgemmGFlopPerSec = dgemmGFlopCount / dgemmDuration;

    CkPrintf("The dgemm %d x %d takes %g ms and achieves %g GFlop/sec\n", luCfg.blockSize,
             luCfg.blockSize, dgemmDuration*1000, dgemmGFlopPerSec);
    CkPrintf("Percent of DGEMM is: %g%%\n", gflops_per_core / dgemmGFlopPerSec * 100.0);
    bs.outputStats();
  }

  void calcScaledResidual(CkReductionMsg *msg) {
    int reducedArrSize=msg->getSize() / sizeof(double);
    double *maxvals=(double *) msg->getData();

    double n = luCfg.blockSize * luCfg.numBlocks;
    double r = maxvals[3]/((maxvals[0]*maxvals[2]+maxvals[1])*n*std::numeric_limits<double>::epsilon());

    VERBOSE_VALIDATION("|A|inf = %e\n|b|inf = %e\n|x|inf = %e\n|Ax-b|inf = %e\n",maxvals[0],maxvals[1],maxvals[2],maxvals[3]);

    CkPrintf("epsilon = %e\nresidual = %f\n",std::numeric_limits<double>::epsilon(),r);
    if (r > 16) CkPrintf("=== WARNING: Scaled residual is greater than 16 - OUT OF SPEC ===\n");

    delete msg;
    CkPrintf("finished validation at wall time: %f\n", CmiWallTimer());
    finishedSolve.send();
  }
};

struct BenchmarkLUBlk : public CBase_BenchmarkLUBlk {
  BenchmarkLUBlk(CProxy_LUSolver solver) : mainProxy(solver), msgsRecvd(0) { }
  BenchmarkLUBlk(CkMigrateMessage *) { }

  CProxy_LUSolver mainProxy;

  /// Validation: count of the number of messages received in a row
  int msgsRecvd;
  double *Ax;
  /// Copy of untouched 'b' vector (allocated during validation)
  double *b;

  /// Random data generation
  int seed_A;
  int seed_b;
  //CrnStream blockStream, vecStream;
  void initVec();
  void genBlock();
  void genVec(double *buf);

  /// Validation
  void startValidation();
  void recvXvec(int size, double* xvec);
  void sumBvec(int size, double* partial_b);
  void calcResiduals();
};

void BenchmarkLUBlk::initVec() {
  LUmsg = createABlkMsg();
  LU = LUmsg->data;
  thisProxy(thisIndex).dataReady();

  // Save seed value to use for validation
  seed_A = 2998389;
  //CrnInitStream(&blockStream, seed_A + thisIndex.x*numBlks + thisIndex.y, 0);
  genBlock();

  bvec = new double[blkSize];

  seed_b = 9934835;
  genVec(bvec);
}

void BenchmarkLUBlk::genBlock() {
  //CrnStream stream;
  //memcpy(&stream, &blockStream, sizeof(CrnStream));

  for (double *d = LU; d < LU + blkSize * blkSize; ++d)
    *d = CrnDrand();
}

void BenchmarkLUBlk::genVec(double *buf) {
  MatGen rnd(seed_b);

  // Skip the blocks before this one
  rnd.skipNDoubles(thisIndex.x * blkSize);
  rnd.getNRndDoubles(blkSize, buf);
}

void BenchmarkLUBlk::startValidation() {
  // Starting state:
  // solution sub-vector x is in variable bvec on the diagonals
  // variable b has the original b vector on the diagonals

  // Diagonals regenerate b and distribute x across entire column
  if(thisIndex.x == thisIndex.y) {
    CProxySection_BenchmarkLUBlk col =
      CProxySection_BenchmarkLUBlk::ckNew(thisArrayID, 0, numBlks-1,
                                          1, thisIndex.y, thisIndex.y, 1);
    col.recvXvec(blkSize, bvec);
  }
}

double infNorm(int size, double * array) {
  double maxval = fabs(array[0]);
  for (int i = 1; i < size; i++) {
    if (fabs(array[i]) > maxval)
      maxval = fabs(array[i]);
  }
  return maxval;
}

void BenchmarkLUBlk::recvXvec(int size, double* xvec) {
  // Regenerate A and place into already allocated LU
  genBlock();
  double *partial_b = new double[blkSize];

  // Perform local dgemv
#if USE_ESSL || USE_ACML
  dgemv(BLAS_TRANSPOSE, blkSize, blkSize, 1.0, LU, blkSize, xvec, 1, 0.0, partial_b, 1);
#else
  cblas_dgemv( CblasRowMajor, CblasNoTrans,
               blkSize, blkSize, 1.0, LU,
               blkSize, xvec, 1, 0.0, partial_b, 1);
#endif

  // Sum-reduction of result across row with diagonal element as target
  thisProxy(thisIndex.x, thisIndex.x).sumBvec(blkSize, partial_b);
  delete [] partial_b;

  // If the block is not the diagonal, find the max A value and contribute
  if (thisIndex.x != thisIndex.y) {
    // Find local max of A
    double A_max = infNorm(blkSize * blkSize, LU);
    VERBOSE_VALIDATION("[%d,%d] A_max  = %e\n",thisIndex.x,thisIndex.y,A_max);

    double maxvals[4];
    maxvals[0] = A_max;
    maxvals[1] = -1;
    maxvals[2] = -1;
    maxvals[3] = -1;

    contribute(sizeof(maxvals), &maxvals, CkReduction::max_double,
	       CkCallback(CkIndex_LUSolver::calcScaledResidual(NULL), mainProxy));
  }
}

void BenchmarkLUBlk::sumBvec(int size, double* partial_b) {
  // Clear bvec before first message processed for sum-reduction
  if(msgsRecvd == 0) {
    Ax = new double[blkSize];
    memset(Ax, 0, blkSize*sizeof(double));
  }

  // Sum up messages
  if(++msgsRecvd <= numBlks) {
    for (int i = 0; i < size; i++) {
      Ax[i] += partial_b[i];
    }
  }

  // If all messages received, calculate the residual
  if (msgsRecvd == numBlks) {
    calcResiduals();
    delete []  Ax;
  }
}

void BenchmarkLUBlk::calcResiduals() {
  b = new double[blkSize];
  genVec(b);
  double *residuals = new double[blkSize];

  //diagonal elements that received sum-reduction perform b - A*x
  for (int i = 0; i < blkSize; i++) {
    residuals[i] = b[i] - Ax[i];
    // if(fabs(residuals[i]) > 1e-14 || std::isnan(residuals[i]) || std::isinf(residuals[i]))
    // CkPrintf("WARNING: Large Residual for x[%d]: %f - %f = %e\n", thisIndex.x*blkSize+i, b[i], bvec[i], residuals[i]);
  }

  // Find local max values
  double A_max = infNorm(blkSize * blkSize, LU);
  double b_max = infNorm(blkSize, b);
  delete [] b;
  double x_max = infNorm(blkSize, bvec);
  double res_max = infNorm(blkSize, residuals);
  delete [] residuals;
  VERBOSE_VALIDATION("[%d,%d] A_max  = %e\n",thisIndex.x,thisIndex.y,A_max);
  VERBOSE_VALIDATION("[%d,%d] b_max  = %e\n",thisIndex.x,thisIndex.y,b_max);
  VERBOSE_VALIDATION("[%d,%d] x_max  = %e\n",thisIndex.x,thisIndex.y,x_max);
  VERBOSE_VALIDATION("[%d,%d] res_max  = %e\n",thisIndex.x,thisIndex.y,res_max);

  double maxvals[4];
  maxvals[0] = A_max;
  maxvals[1] = b_max;
  maxvals[2] = x_max;
  maxvals[3] = res_max;

  contribute(sizeof(maxvals), &maxvals, CkReduction::max_double,
	     CkCallback(CkIndex_LUSolver::calcScaledResidual(NULL), mainProxy));
}

#include "driver.def.h"
