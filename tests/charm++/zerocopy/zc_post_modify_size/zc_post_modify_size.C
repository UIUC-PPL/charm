#include "zc_post_modify_size.decl.h"
#define SIZE 2000
#define NUM_ELEMENTS_PER_PE 10
#define CONSTANT 188

CProxy_arr arrProxy;
CProxy_grp grpProxy;
CProxy_nodegrp ngProxy;
CProxy_tester chareProxy;
int arrSize;

void assignValuesToIndex(int *arr, int size);
void assignValuesToConstant(int *arr, int size, int constantVal);
void verifyValuesWithConstant(int *arr, int size, int constantVal);
void verifyValuesWithIndex(int *arr, int size, int startIndex);

class main : public CBase_main {
  public:
    main(CkArgMsg *m) {
      delete m;

      arrSize = CkNumPes() * NUM_ELEMENTS_PER_PE;
      // Create a chare array
      arrProxy = CProxy_arr::ckNew(arrSize);

      // Create a group
      grpProxy = CProxy_grp::ckNew();

      // Create a nodegroup
      ngProxy = CProxy_nodegrp::ckNew();

      // Create the tester chare
      chareProxy = CProxy_tester::ckNew();
    }
};

class tester : public CBase_tester {
  int *srcBuffer;
  int counter;
  public:
    tester() {

      counter = 0;

      srcBuffer = new int[SIZE];
      assignValuesToConstant(srcBuffer, SIZE, CONSTANT);

      // Test p2p sends
      arrProxy[9].recv_zerocopy(CkSendBuffer(srcBuffer), SIZE, false);
      grpProxy[CkNumPes() - 1].recv_zerocopy(CkSendBuffer(srcBuffer), SIZE, false);
      ngProxy[CkNumNodes() - 1].recv_zerocopy(CkSendBuffer(srcBuffer), SIZE, false);
    }

    void p2pDone() {
      if(++counter == 3) {
        counter = 0;

        // Test bcast sends
        arrProxy.recv_zerocopy(CkSendBuffer(srcBuffer), SIZE, true);
        grpProxy.recv_zerocopy(CkSendBuffer(srcBuffer), SIZE, true);
        ngProxy.recv_zerocopy(CkSendBuffer(srcBuffer), SIZE, true);
      }
    }

    void bcastDone() {
      if(++counter == 3) {
        counter = 0;
        delete [] srcBuffer;
        CkPrintf("[%d][%d][%d] All tests have successfully completed\n", CkMyPe(), CkMyNode(), CkMyRank());
        CkExit();
      }
    }
};

class arr : public CBase_arr {
  int *destBuffer;
  public:
    arr() {
      destBuffer = new int[SIZE];
      assignValuesToIndex(destBuffer, SIZE);
    }

    void recv_zerocopy(int *buffer, size_t size, bool isBcast, CkNcpyBufferPost *ncpyPost) {
      CkMatchBuffer(ncpyPost, 0, thisIndex);

      if(isBcast) {
        CkPostBuffer(destBuffer, SIZE/2, thisIndex);
      } else {
        CkPostBuffer(destBuffer, SIZE/4, thisIndex);
      }
    }

    void recv_zerocopy(int *buffer, size_t size, bool isBcast) {
      if(isBcast) {
        verifyValuesWithConstant(buffer, SIZE/2, CONSTANT);
        verifyValuesWithIndex(buffer, SIZE, SIZE/2);

        CkCallback doneCb = CkCallback(CkReductionTarget(tester, bcastDone), chareProxy);
        contribute(doneCb);

        delete [] destBuffer;
      } else {
        verifyValuesWithConstant(buffer, SIZE/4, CONSTANT);
        verifyValuesWithIndex(buffer, SIZE, SIZE/4);

        chareProxy.p2pDone();
      }
    }
};

class grp : public CBase_grp {
  int *destBuffer;
  public:
    grp() {
      destBuffer = new int[SIZE];
      assignValuesToIndex(destBuffer, SIZE);
    }

    void recv_zerocopy(int *buffer, size_t size, bool isBcast, CkNcpyBufferPost *ncpyPost) {
      CkMatchBuffer(ncpyPost, 0, arrSize + thisIndex);

      if(isBcast) {
        CkPostBuffer(destBuffer, SIZE/2, arrSize + thisIndex);
      } else {
        CkPostBuffer(destBuffer, SIZE/4, arrSize + thisIndex);
      }
    }

    void recv_zerocopy(int *buffer, size_t size, bool isBcast) {
      if(isBcast) {
        verifyValuesWithConstant(buffer, SIZE/2, CONSTANT);
        verifyValuesWithIndex(buffer, SIZE, SIZE/2);

        CkCallback doneCb = CkCallback(CkReductionTarget(tester, bcastDone), chareProxy);
        contribute(doneCb);

        delete [] destBuffer;
      } else {
        verifyValuesWithConstant(buffer, SIZE/4, CONSTANT);
        verifyValuesWithIndex(buffer, SIZE, SIZE/4);

        chareProxy.p2pDone();
      }
    }
};

class nodegrp : public CBase_nodegrp {
  int *destBuffer;
  public:
    nodegrp() {
      destBuffer = new int[SIZE/2];
      assignValuesToIndex(destBuffer, SIZE/2);
    }

    void recv_zerocopy(int *buffer, size_t size, bool isBcast, CkNcpyBufferPost *ncpyPost) {
      CkMatchBuffer(ncpyPost, 0, arrSize + CkNumPes() + thisIndex);

      if(isBcast) {
        CkPostBuffer(destBuffer, SIZE/2, arrSize + CkNumPes() + thisIndex);
      } else {
        CkPostBuffer(destBuffer, SIZE/4, arrSize + CkNumPes() + thisIndex);
      }
    }

    void recv_zerocopy(int *buffer, size_t size, bool isBcast) {
      if(isBcast) {
        verifyValuesWithConstant(buffer, SIZE/2, CONSTANT);

        CkCallback doneCb = CkCallback(CkReductionTarget(tester, bcastDone), chareProxy);
        contribute(doneCb);

        delete [] destBuffer;
      } else {
        verifyValuesWithConstant(buffer, SIZE/4, CONSTANT);
        verifyValuesWithIndex(buffer, SIZE/2, SIZE/4);

        chareProxy.p2pDone();
      }
    }
};

// Util methods
void assignValuesToIndex(int *arr, int size){
  for(int i=0; i<size; i++)
     arr[i] = i;
}

void assignValuesToConstant(int *arr, int size, int constantVal){
  for(int i=0; i<size; i++)
     arr[i] = constantVal;
}

void verifyValuesWithConstant(int *arr, int size, int constantVal){
  for(int i=0; i<size; i++)
     CkAssert(arr[i] == constantVal);
}

void verifyValuesWithIndex(int *arr, int size, int startIndex){
  for(int i=startIndex; i<size; i++)
     CkAssert(arr[i] == i);
}

#include "zc_post_modify_size.def.h"
