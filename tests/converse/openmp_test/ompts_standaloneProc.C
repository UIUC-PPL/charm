#include "OmpCharmTest.decl.h"
#include <omp.h>
#include "omp_testsuite.h"
CProxy_Main mainProxy;
CProxy_TestInstance testArray;
int numChares;

class Main : public CBase_Main
{
  public:

    Main(CkArgMsg* m) {
      mainProxy = thisProxy;

      CkPrintf("######## OpenMP Validation Suite V %s ######\n", OMPTS_VERSION );
      CkPrintf("## Repetitions: %3d                       ####\n",REPETITIONS);
      CkPrintf("## Loop Count : %6d                    ####\n",LOOPCOUNT);
      CkPrintf("##############################################\n");
      CkPrintf("Testing <directive></directive>\n\n");
      numChares = CkNumPes() / 2;//create chares as 3 times many as the number of PEs to run OpenMP in overdecomposed chares. 
      CkPrintf("NumPes: %d, NumChares: %d\n", CkNumPes(), numChares);
      testArray = CProxy_TestInstance::ckNew(numChares);
    }

    void testStart() {
      testArray.testRun();
    }

    void testDone(int result) {
      CkPrintf ("Result: %i\n", result);
      CkExit();
    }
};

class TestInstance : public CBase_TestInstance 
{
  TestInstance_SDAG_CODE

  private:
    int success;
    int failed;
    int result;
//    FILE *logFile;

  public:
    TestInstance(CkMigrateMessage* m) {}
    TestInstance()  {
      //const char* logFileName = "<testfunctionname></testfunctionname>.log";
//      logFile = NULL; // fopen(logFileName,"w+");
      success = 0;
      failed = 0;
      result = 0;
      CkCallback cb(CkReductionTarget(Main,testStart), mainProxy);
      contribute(cb);
    }

    void testRun() {
      int i;
      for ( i = 0; i < REPETITIONS; i++ ) {
        CkPrintf("\n\n PE: %d, Chare: %d, %d. run of <testfunctionname></testfunctionname> out of %d\n\n",CkMyPe(), thisIndex, i+1, REPETITIONS);
        if(<testfunctionname></testfunctionname>()){
          CkPrintf("PE: %d, Chare: %d, Test successful.\n", CkMyPe(), thisIndex);
          success++;
        }
        else {
          CkPrintf("PE: %d, Chare: %d, Error: Test failed.\n", CkMyPe(), thisIndex);
          failed++;
        }
      }
      if (failed==0) {
        CkPrintf("PE: %d, Chare: %d, Directive worked without errors.\n", CkMyPe(), thisIndex);
        result=0;
      }
      else {
        CkPrintf("PE: %d, Directive failed the test %i times out of %i.\n%i test(s) were successful\n", CkMyPe(), failed, REPETITIONS, success);
        result = (int) (((double) failed / (double) REPETITIONS ) * 100 );   
      }
      CkCallback cb(CkReductionTarget(Main,testDone), mainProxy);
      contribute(sizeof(int), &result, CkReduction::max_int, cb); /* Aggregate the result on a single chare to the main chare */
    }
    
    int <testfunctionname></testfunctionname>();
};
