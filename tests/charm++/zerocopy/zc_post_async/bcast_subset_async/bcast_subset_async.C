#include "bcast_subset_async.decl.h"
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

      // Test bcast sends
#if DELAYED_POST
      // For delayed posting, buffers are posted after the execution of the Post EMs
      arrProxy.recv_zerocopy(CkSendBuffer(srcBuffer1), SIZE);
      grpProxy.recv_zerocopy(CkSendBuffer(srcBuffer1), SIZE, CkSendBuffer(srcBuffer2), SIZE);
      ngProxy.recv_zerocopy(CkSendBuffer(srcBuffer1), SIZE);
#elif EARLY_POST
      // For early posting, post buffers now before the Post EM is called
      arrProxy.postBuffers();
      grpProxy.postBuffers();
      ngProxy.postBuffers();
#endif
    }

    void bcastPostDone() {
      if(++counter == 3) { // All Bcast buffers are posted, call post EMs
        counter = 0;
        arrProxy.recv_zerocopy(CkSendBuffer(srcBuffer1), SIZE);
        grpProxy.recv_zerocopy(CkSendBuffer(srcBuffer1), SIZE, CkSendBuffer(srcBuffer2), SIZE);
        ngProxy.recv_zerocopy(CkSendBuffer(srcBuffer1), SIZE);
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
  bool evenElement;
  public:
    arr() {
      destBuffer = new int[SIZE];
      assignValuesToIndex(destBuffer, SIZE); // Initial values
      tag = 100 + thisIndex;
      evenElement = (thisIndex % 2 == 0);
    }

    void recv_zerocopy(int *&buffer, size_t &size, CkNcpyBufferPost *ncpyPost) {
      CkMatchBuffer(ncpyPost, 0, tag);

#if DELAYED_POST
      if(evenElement) // Post buffer now for delayed posting
        thisProxy[thisIndex].readyToPost();
#endif
      if(!evenElement)
        readyToPost();
    }

    void postBuffers() {
      if(evenElement)
        readyToPost();

      // Reduction to signal to tester chare to call EM
      CkCallback doneCb = CkCallback(CkReductionTarget(tester, bcastPostDone), chareProxy);
      contribute(doneCb);
    }

    void readyToPost() {
      CkPostBuffer(destBuffer, (size_t) SIZE, tag);
    }

    void recv_zerocopy(int *buffer, size_t size) {
      verifyValuesWithConstant(destBuffer, SIZE, CONSTANT);

      CkCallback doneCb = CkCallback(CkReductionTarget(tester, bcastDone), chareProxy);
      contribute(doneCb);
      delete [] destBuffer;
    }
};

class grp : public CBase_grp {
  int *destBuffer1;
  int *destBuffer2;
  int tag1, tag2;
  bool evenElement;
  public:
    grp() {
      destBuffer1 = new int[SIZE];
      assignValuesToIndex(destBuffer1, SIZE);
      tag1 = 200 + thisIndex;

      destBuffer2 = new int[SIZE];
      assignValuesToIndex(destBuffer2, SIZE);
      tag2 = 300 + thisIndex;

      evenElement = (thisIndex % 2 == 0);
    }

    void recv_zerocopy(int *&buffer1, size_t &size1, int *&buffer2, size_t &size2, CkNcpyBufferPost *ncpyPost) {
      CkMatchBuffer(ncpyPost, 0, tag1);
      CkMatchBuffer(ncpyPost, 1, tag2);

#if DELAYED_POST
      if(evenElement) // Post buffer now for delayed posting
        thisProxy[thisIndex].readyToPost();
#endif
      if(!evenElement)
        readyToPost();
    }

    void postBuffers() {
      if(evenElement)
        readyToPost();

      CkCallback doneCb = CkCallback(CkReductionTarget(tester, bcastPostDone), chareProxy);
      contribute(doneCb);
    }

    void readyToPost() {
      CkPostBuffer(destBuffer1, (size_t) SIZE, tag1);
      CkPostBuffer(destBuffer2, (size_t) SIZE, tag2);
    }

    void recv_zerocopy(int *buffer1, size_t size1, int *buffer2, size_t size2) {
      verifyValuesWithConstant(destBuffer1, SIZE, CONSTANT);
      verifyValuesWithConstant(destBuffer2, SIZE, CONSTANT);

      CkCallback doneCb = CkCallback(CkReductionTarget(tester, bcastDone), chareProxy);
      contribute(doneCb);
      delete [] destBuffer1;
      delete [] destBuffer2;
    }
};

class nodegrp : public CBase_nodegrp {
  int *destBuffer;
  int tag;
  bool evenElement;
  public:
    nodegrp() {
      destBuffer = new int[SIZE];
      assignValuesToIndex(destBuffer, SIZE);
      tag = 400 + thisIndex;
    }

    void recv_zerocopy(int *&buffer, size_t &size, CkNcpyBufferPost *ncpyPost) {
      CkMatchNodeBuffer(ncpyPost, 0, tag);

#if DELAYED_POST
      if(evenElement) // Post buffer now for delayed posting
        thisProxy[thisIndex].readyToPost();
#endif
      if(!evenElement)
        readyToPost();
    }

    void readyToPost() {
      CkPostNodeBuffer(destBuffer, (size_t) SIZE, tag);
    }

    void postBuffers() {
      if(evenElement)
        readyToPost();

      CkCallback doneCb = CkCallback(CkReductionTarget(tester, bcastPostDone), chareProxy);
      contribute(doneCb);
    }

    void recv_zerocopy(int *buffer, size_t size) {
      verifyValuesWithConstant(destBuffer, SIZE, CONSTANT);

      CkCallback doneCb = CkCallback(CkReductionTarget(tester, bcastDone), chareProxy);
      contribute(doneCb);
      delete [] destBuffer;
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

#include "bcast_subset_async.def.h"
