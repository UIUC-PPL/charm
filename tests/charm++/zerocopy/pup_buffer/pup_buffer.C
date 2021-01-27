#include "pup_buffer.decl.h"
#define SIZE 1000000
#define TOTAL_ITER 10
#define LB_FREQ 5

CProxy_arr arrProxy;
CProxy_main mainProxy;
int totalElems;

class main : public CBase_main {
  public:
    main(CkArgMsg *m) {
      delete m;

      mainProxy = thisProxy;

      totalElems = 10 * CkNumPes();
      arrProxy = CProxy_arr::ckNew(totalElems);
      arrProxy.run();
    }

    void done() {
      CmiPrintf("[%d][%d][%d] Completed testing, Exiting\n", CmiMyPe(), CmiMyNode(), CmiMyRank());
      CkExit();
    }
};

class arr : public CBase_arr {

  int numAtSync, numNonAtSync;
  int *buffer;
  CkCallback cb;
  int iteration;
  int counts[4];

  public:
    arr() {
      buffer = (int *)malloc(sizeof(int) * SIZE);
      iteration = 0;

      usesAtSync = true;

      numAtSync = TOTAL_ITER / LB_FREQ; // number of times AtSync is reached

      if(TOTAL_ITER % LB_FREQ  == 0)
        numAtSync--; // AtSync is skipped on the last iteration

      numNonAtSync = TOTAL_ITER - numAtSync;

      for(int i=0; i < 4; i++)
        counts[i] = 0;

      for(int i=0; i < SIZE; i++)
        buffer[i] = thisIndex;
      cb = CkCallback(CkReductionTarget(main, done), mainProxy);
    }

    arr(CkMigrateMessage *msg) {
      delete msg;
    }

    ~arr() {}

    static void *buff_allocate(size_t n) {
      return new int[n];
    }

    static void buff_deallocate(void *ptr) {
      delete [] (int *)(ptr);
    }

    void pup(PUP::er &p) {
      p|iteration;
      p|cb;
      p|numAtSync;
      p|numNonAtSync;

      if(iteration % 2 == 0) // Test pup_buffer custom
        p.pup_buffer_async(buffer, SIZE, buff_allocate, buff_deallocate);
      else // Test pup_buffer default
        p.pup_buffer_async(buffer, SIZE);

      PUParray(p, counts, 4);
    }

    void verify(int mode) {
      counts[mode]++;

      for(int i=0; i < SIZE; i++) {
        CkAssert(buffer[i] == thisIndex);
      }

      if(iteration == TOTAL_ITER) {
        testCompletion();
      }
    }

    void testCompletion() {
      bool res1, res2, res3, res4;
      res1 = (counts[0] == numNonAtSync);
      res2 = (counts[2] == numAtSync);
      res3 = (counts[3] == numAtSync);

      res4  = (counts[1] == 0); // Default, if CMK_LBDB_ON is 0 or CkNumPes() == 1
#if CMK_LBDB_ON
      if(CkNumPes() > 1) {
        res4 = (counts[1] == numAtSync);
      }
#endif

      if(res1 && res2 && res3 && res4) {
        free(buffer);
        contribute(cb);
      }
    }

    void run() {
      iteration++;
      if(thisIndex == 0)
        CmiPrintf("Iteration %d completed\n", iteration);
      if(iteration < TOTAL_ITER) {
        if(iteration % LB_FREQ == 0) {
          AtSync();
          // Test data when an entry method is called
          thisProxy[(thisIndex + 1) % totalElems].verify(3); // Call verify on the next element
        }
        else {
          verify(0); // local, regular
          run();
        }
      } else {
        verify(0); //local, regular, last local call
      }
    }

    void ckJustMigrated() { // Test data when ckJustMigrated is called by RTS
      verify(1); // local, just after migration
    }

    void ResumeFromSync() { // Test data when ResumeFromSync is called by RTS
      verify(2); // local, from resumeFromSync
      run();
    }
};

#include "pup_buffer.def.h"
