#include "post_and_match.decl.h"
#define SIZE 2000
#define NUM_ELEMENTS_PER_PE 10
#define CONSTANT 188

CProxy_arr arrProxy;
CProxy_grp grpProxy;
CProxy_nodegrp ngProxy;
CProxy_tester chareProxy;

void assignValuesToIndex(int *arr, int size);
void assignValuesToConstant(int *arr, int size, int constantVal);
void verifyValuesWithConstant(int *arr, int size, int constantVal);
void verifyValuesWithIndex(int *arr, int size, int startIndex);

class main : public CBase_main {
  public:
    main(CkArgMsg *m) {
      delete m;

      // Create a chare array
      arrProxy = CProxy_arr::ckNew(CkNumPes() * NUM_ELEMENTS_PER_PE);

      // Create a group
      grpProxy = CProxy_grp::ckNew();

      // Create a nodegroup
      ngProxy = CProxy_nodegrp::ckNew();

      // Create the tester chare
      chareProxy = CProxy_tester::ckNew();
    }
};

class tester : public CBase_tester {
  int *srcBuffer1;
  int *srcBuffer2;
  int counter;
  public:
    tester() {

      counter = 0;

      srcBuffer1 = new int[SIZE];
      assignValuesToConstant(srcBuffer1, SIZE, CONSTANT);

      srcBuffer2 = new int[SIZE];
      assignValuesToConstant(srcBuffer2, SIZE, CONSTANT);

      int lastArrEleIndex = CkNumPes() * NUM_ELEMENTS_PER_PE - 1;

      // Test p2p sends
      arrProxy[lastArrEleIndex].recv_zerocopy(CkSendBuffer(srcBuffer1), SIZE, false);
      grpProxy[CkNumPes() - 1].recv_zerocopy(CkSendBuffer(srcBuffer1), SIZE, CkSendBuffer(srcBuffer2), SIZE, false);
      ngProxy[CkNumNodes() - 1].recv_zerocopy(CkSendBuffer(srcBuffer1), SIZE, false);
    }

    void p2pDone() {
      if(++counter == 3) { // All p2p sends are complete
        counter = 0;

        CkPrintf("[%d][%d][%d] All p2p tests have successfully completed\n", CkMyPe(), CkMyNode(), CkMyRank());
        // Test bcast sends
#if DELAYED_POST || SYNC_POST
        // For delayed posting, buffers are posted after the execution of the Post EMs
        arrProxy.recv_zerocopy(CkSendBuffer(srcBuffer1), SIZE, true);
        grpProxy.recv_zerocopy(CkSendBuffer(srcBuffer1), SIZE, CkSendBuffer(srcBuffer2), SIZE, true);
        ngProxy.recv_zerocopy(CkSendBuffer(srcBuffer1), SIZE, true);
#elif EARLY_POST
        // For early posting, post buffers now before the Post EM is called
        arrProxy.postBuffers();
        grpProxy.postBuffers();
        ngProxy.postBuffers();
#endif
      }
    }

    void bcastPostDone() {
      if(++counter == 3) { // All Bcast buffers are posted, call post EMs
        counter = 0;
        arrProxy.recv_zerocopy(CkSendBuffer(srcBuffer1), SIZE, true);
        grpProxy.recv_zerocopy(CkSendBuffer(srcBuffer1), SIZE, CkSendBuffer(srcBuffer2), SIZE, true);
        ngProxy.recv_zerocopy(CkSendBuffer(srcBuffer1), SIZE, true);
      }
    }

    void bcastDone() {
      if(++counter == 3) { // All Post EMs are complete
        counter = 0;
        delete [] srcBuffer1;
        delete [] srcBuffer2;
        CkPrintf("[%d][%d][%d] All bcasts tests have successfully completed\n", CkMyPe(), CkMyNode(), CkMyRank());
        CkExit();
      }
    }
};

class arr : public CBase_arr {
  int *destBuffer;
  int tag;
  public:
    arr() {
      destBuffer = new int[SIZE];
      assignValuesToIndex(destBuffer, SIZE); // Initial values
      tag = 100 + thisIndex;
#if EARLY_POST
      // Post buffer for p2p send
      if(thisIndex == CkNumPes() * NUM_ELEMENTS_PER_PE - 1)
        readyToPost();
#endif
    }

    void recv_zerocopy(int *&buffer, size_t &size, bool isBcast, CkNcpyBufferPost *ncpyPost) {
      CkMatchBuffer(ncpyPost, 0, tag);

#if DELAYED_POST
      // Post buffer now for delayed posting
      thisProxy[thisIndex].readyToPost();
#elif SYNC_POST
      readyToPost();
#endif
    }

    void postBuffers() {
      readyToPost();

      // Reduction to signal to tester chare to call EM
      CkCallback doneCb = CkCallback(CkReductionTarget(tester, bcastPostDone), chareProxy);
      contribute(doneCb);
    }

    void readyToPost() {
      CkPostBuffer(destBuffer, (size_t) SIZE, tag);
    }

    void recv_zerocopy(int *buffer, size_t size, bool isBcast) {
      verifyValuesWithConstant(destBuffer, SIZE, CONSTANT);

      if(isBcast) {
        CkCallback doneCb = CkCallback(CkReductionTarget(tester, bcastDone), chareProxy);
        contribute(doneCb);
        delete [] destBuffer;
      } else {
        assignValuesToIndex(destBuffer, SIZE); // Reset values
        chareProxy.p2pDone();
      }
    }
};

class grp : public CBase_grp {
  int *destBuffer1;
  int *destBuffer2;
  int tag1, tag2;
  public:
    grp() {
      destBuffer1 = new int[SIZE];
      assignValuesToIndex(destBuffer1, SIZE);
      tag1 = 200 + thisIndex;

      destBuffer2 = new int[SIZE];
      assignValuesToIndex(destBuffer2, SIZE);
      tag2 = 300 + thisIndex;
#if EARLY_POST
      // Post buffer for p2p send
      if(thisIndex == CkNumPes() - 1)
        readyToPost();
#endif
    }

    void recv_zerocopy(int *&buffer1, size_t &size1, int *&buffer2, size_t &size2, bool isBcast, CkNcpyBufferPost *ncpyPost) {
      CkMatchBuffer(ncpyPost, 0, tag1);
      CkMatchBuffer(ncpyPost, 1, tag2);

#if DELAYED_POST
      // Post buffer now for delayed posting
      thisProxy[thisIndex].readyToPost();
#elif SYNC_POST
      readyToPost();
#endif
    }

    void postBuffers() {
      readyToPost();
      CkCallback doneCb = CkCallback(CkReductionTarget(tester, bcastPostDone), chareProxy);
      contribute(doneCb);
    }

    void readyToPost() {
      CkPostBuffer(destBuffer1, (size_t) SIZE, tag1);
      CkPostBuffer(destBuffer2, (size_t) SIZE, tag2);
    }

    void recv_zerocopy(int *buffer1, size_t size1, int *buffer2, size_t size2, bool isBcast ) {
      verifyValuesWithConstant(destBuffer1, SIZE, CONSTANT);
      verifyValuesWithConstant(destBuffer2, SIZE, CONSTANT);

      if(isBcast) {
        CkCallback doneCb = CkCallback(CkReductionTarget(tester, bcastDone), chareProxy);
        contribute(doneCb);
        delete [] destBuffer1;
        delete [] destBuffer2;
      } else {
        assignValuesToIndex(destBuffer1, SIZE); // Reset values
        assignValuesToIndex(destBuffer2, SIZE); // Reset values
        chareProxy.p2pDone();
     }
    }
};

class nodegrp : public CBase_nodegrp {
  int *destBuffer;
  int tag;
  public:
    nodegrp() {
      destBuffer = new int[SIZE];
      assignValuesToIndex(destBuffer, SIZE);
      tag = 400 + thisIndex;
#if EARLY_POST
      // Post buffer for p2p send
      if(thisIndex == CkNumNodes() - 1)
        readyToPost();
#endif
    }

    void recv_zerocopy(int *&buffer, size_t &size, bool isBcast, CkNcpyBufferPost *ncpyPost) {
      CkMatchNodeBuffer(ncpyPost, 0, tag);

#if DELAYED_POST
      // Post buffer now for delayed posting
      thisProxy[thisIndex].readyToPost();
#elif SYNC_POST
      readyToPost();
#endif
    }

    void readyToPost() {
      CkPostNodeBuffer(destBuffer, (size_t) SIZE, tag);
    }

    void postBuffers() {
      readyToPost();
      CkCallback doneCb = CkCallback(CkReductionTarget(tester, bcastPostDone), chareProxy);
      contribute(doneCb);
    }

    void recv_zerocopy(int *buffer, size_t size, bool isBcast) {
      verifyValuesWithConstant(destBuffer, SIZE, CONSTANT);

      if(isBcast) {
        CkCallback doneCb = CkCallback(CkReductionTarget(tester, bcastDone), chareProxy);
        contribute(doneCb);
        delete [] destBuffer;
      } else {
        assignValuesToIndex(destBuffer, SIZE); // Reset values
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

#include "post_and_match.def.h"
