#include "benchmark.decl.h"
#include <vector>
#include <algorithm>
#include "acml.h"

CProxy_Main mainProxy;
int blockSize;
int numBlocks;

struct Main : public CBase_Main {
  int count, iter;
  CProxy_dgerTest dgers;
  std::vector<double> peTimes;
  int numIter;

  Main(CkArgMsg *m) : count(0), iter(0), peTimes(CkNumPes()) {
    if (m->argc < 4) {
      CkPrintf("usage: blockSize numBlocks numIter\n");
      CkExit();
    }

    blockSize = atoi(m->argv[1]);
    numBlocks = atoi(m->argv[2]);
    numIter = atoi(m->argv[3]);

    CkPrintf("numBlocks = %d, blockSize = %d, numIter = %d\n",
             numBlocks, blockSize, numIter);

    mainProxy = thisProxy;
    dgers = CProxy_dgerTest::ckNew();
    dgers.testAndSync(iter);
  }

  void finishedTest(int pe, double time) {
    peTimes[pe] = time;
    if (++count == CkNumPes()) {
      if (++iter <= numIter) {
        CkPrintf("Iteration %d\n", iter - 1);
        for (int i = 0; i < CkNumPes(); ++i) {
          CkPrintf("%d: time = %f\n", i, peTimes[i]);
        }
        CkPrintf("\n");
        count = 0;
        dgers.testAndSync(iter);
      } else {
        CkExit();
      }
    }
  }
};

struct dgerTest : public CBase_dgerTest {
  double **blocks;
  double *U;
  dgerTest() {
    /*CkPrintf("%d: allocating numBlocks = %d, blockSize = %d\n",
      CkMyPe(), numBlocks, blockSize);*/
    // Allocate numBlocks
    blocks = new double*[numBlocks];
    for (int i = 0; i < numBlocks; i++) {
      blocks[i] = new double[blockSize * blockSize];
    }
    U = new double[blockSize];
  }
  void testAndSync(int activeCol) {
    //CkPrintf("%d: running testAndSync with activeCol = %d\n", CkMyPe(), activeCol);

    double startTestTime = CmiWallTimer();
    for (int i = 0; i < numBlocks; i++) {
      double *block = blocks[i];

      int startingRow = 0;
      int offset = 2;

#if 0
      const int cacheLineWidth = 16;
      const int innerUnroll = 1;
      for(int k = activeCol + offset; k < blockSize; k += cacheLineWidth) {
	int row = startingRow * blockSize;
	int limit = std::min(blockSize, k + cacheLineWidth);
	for(int j = startingRow; j < blockSize; j++) {
	  int kk = k;
	  double L = block[row + activeCol];
	  double *bb = block + row + kk;
	  for (; kk < limit && kk % innerUnroll != 0; ++kk)
	    *bb++ -=  L * U[kk - activeCol];
	  for (; kk < limit - innerUnroll + 1; kk += innerUnroll) {
	    *bb++ -=  L * U[kk - activeCol];
	    /*block[row + kk + 1] -=  L * U[kk + 1 - activeCol];
	      block[row + kk + 2] -=  L * U[kk + 2 - activeCol];
	      block[row + kk + 3] -=  L * U[kk + 3 - activeCol];*/
	  }
	  for (; kk < limit; ++kk)
	    *bb++ -=  L * U[kk - activeCol];
	  row += blockSize;
	}
      }
#else
      dger(blockSize-(activeCol+offset), blockSize-startingRow,
	   -1.0,
	   U+offset, 1,
	   &block[getIndex(startingRow,activeCol)], blockSize,
	   &block[getIndex(startingRow,activeCol+offset)], blockSize);
#endif
    }
    double totalTestTime = CmiWallTimer() - startTestTime;
    /*CkPrintf("%d: Time taken for %d blocks = %f\n",
      CkMyPe(), numBlocks, totalTestTime);*/

    mainProxy.finishedTest(CkMyPe(), totalTestTime);
  }
  int getIndex(int i, int j) {
    return i * blockSize + j;
  }
};

#include "benchmark.def.h"
