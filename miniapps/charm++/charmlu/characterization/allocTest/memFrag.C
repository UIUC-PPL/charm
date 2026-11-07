#include "allocTest.decl.h"
#include <stdio.h>
#include <vector>
#include <algorithm>
#include <numeric>

/*readonly*/ CProxy_Main mainProxy;

/*mainchare*/
class Main : public CBase_Main
{
public:
  Main(CkArgMsg* m)
  {
    //Process command-line arguments
    if (m->argc < 2)
        CkAbort("Usage: <exe> blockSize\nwhere, blocksize is the size of 2D array blocks that should be allocated\n");
    blocksize = atoi(m->argv[1]);
    delete m;

    // Other initialization
    numFinished = 0;
    numblocks.resize(CkNumPes(), 0);

    //Start the computation
    CkPrintf("Running memory fragmentation test on %d processors\n", CkNumPes());
    mainProxy = thisProxy;

    CProxy_BlockAllocator grp = CProxy_BlockAllocator::ckNew();
    grp.fillAndTestMem(blocksize);
  }

  void done(int pe, int numAllocated)
  {
    numblocks[pe] = numAllocated;

    if (++numFinished == CkNumPes()) {

      double MBperBlock = blocksize * blocksize * 8.0 / 1024 / 1024;
      int minBlocks = * std::min_element(numblocks.begin(), numblocks.end());
      int maxBlocks = * std::max_element(numblocks.begin(), numblocks.end());
      int sumBlocks =   std::accumulate(numblocks.begin(), numblocks.end(), 0);

      CkPrintf("\nblock size: %d x %d doubles", blocksize, blocksize);
      CkPrintf("\nblock size: %f MB", MBperBlock);

      CkPrintf("\ntotal number of blocks that fit on all PEs   : %d (%f MB)", sumBlocks, sumBlocks * MBperBlock);
      CkPrintf("\nmin number of blocks that fit on any given PE: %d (%f MB)", minBlocks, minBlocks * MBperBlock);
      CkPrintf("\nmax number of blocks that fit on any given PE: %d (%f MB)", maxBlocks, maxBlocks * MBperBlock);
      for (int i=0; i<numblocks.size(); i++)
        CkPrintf("\nPE %d: %d blocks (%f MB)",i, numblocks[i], numblocks[i] * MBperBlock);

      CkPrintf("\n\n");
      CkExit();
    }
  }

private:
  int blocksize;
  std::vector<int> numblocks;
  int numFinished;
};


class BlockAllocator : public CBase_BlockAllocator
{
public:
  void fillAndTestMem(int blocksize)
  {
    int arrsize = blocksize * blocksize;
    std::vector<double*> blocks;
    int numAllocated;
    try {
      while(1) { blocks.push_back(new double[arrsize]); }
    }
    catch (...) {
      numAllocated = blocks.size();
      for (int i=0; i< blocks.size(); i++)
        delete [] blocks[i];
      blocks.clear();
    }

    mainProxy.done(CkMyPe(), numAllocated);
  }
};

#include "allocTest.def.h"
