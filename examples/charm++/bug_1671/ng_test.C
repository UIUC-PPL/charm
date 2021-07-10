#include "ng_test.decl.h"

#define DEBUG(x) //x

CProxy_main mainProxy;
CProxy_NG1 ngProxy;
CProxy_G1 gProxy;
int kVal;

struct ngMsg : public CMessage_ngMsg {

  int k;
  int *buffer;
  int size;

  ngMsg(int _k, int _size) {
    k = _k;
    size = _size;

    for(int i=0; i<size; i++) {
      buffer[i] = i;
    }
  }

  void verify() {
    for(int i=0; i<size; i++) {
      CkAssert(buffer[i] == i);
    }
  }
};


class main : public CBase_main
{
  int count;
  int iter, k, size;

public:
  main(CkMigrateMessage *m) {}
  main(CkArgMsg *msg)
  {
    mainProxy = thisProxy;
    if (msg->argc < 3) {
      CkPrintf("Usage: ./ng_test <k> <size>\n");
      CkExit(1);
    }

    k = atoi(msg->argv[1]);
    kVal = k;
    size = atoi(msg->argv[2]);
    iter = atoi(msg->argv[3]);
    count = 0;

    // Create a group
    gProxy = CProxy_G1::ckNew();

    // Create a nodegroup
    ngProxy = CProxy_NG1::ckNew();

    CkPrintf("[%d][%d][%d] **************** Iteration:%d \n", CmiMyPe(), CmiMyNode(), CmiMyRank(), count+1);
    ngMsg *sbMsg = new (size) ngMsg(k, size);
    ngProxy.recvMsg(sbMsg);
  };

  void done(){
    count++;

    if(count == iter) {
      CkPrintf("[%d][%d][%d] All iterations have completed\n", CmiMyPe(), CmiMyNode(), CmiMyRank());
      CkExit();
    } else {
      CkPrintf("[%d][%d][%d] **************** Iteration:%d \n", CmiMyPe(), CmiMyNode(), CmiMyRank(), count+1);
      ngMsg *sbMsg = new (size) ngMsg(k, size);
      ngProxy.recvMsg(sbMsg);
    }
  };
};

class NG1 : public CBase_NG1 {
  ngMsg *myReceivedMsg;

  public:
  NG1() {
    DEBUG(CkPrintf("[%d][%d][%d] NG1 Constructor\n", CmiMyPe(), CmiMyNode(), CmiMyRank());)
  }

  ngMsg *getReceivedMsgPointer() {
    return myReceivedMsg;
  }

  void recvMsg(ngMsg *msg) {

    CkAssert(msg->k == kVal);
    msg->verify();

    myReceivedMsg = msg;

    DEBUG(CkPrintf("[%d][%d][%d] NG1 received msg %p, forwarding the msg to group elements on my node first PE of my node: %d and size is %d\n", CmiMyPe(), CmiMyNode(), CmiMyRank(), msg, CkNodeFirst(CkMyNode()), CkMyNodeSize());)

    // Increase ref count
    for(int i= 0; i < CkMyNodeSize(); i++) {
      CmiReference(UsrToEnv(myReceivedMsg));
    }

    for(int i= 0; i < CkMyNodeSize(); i++) {
      int myPE = CkNodeFirst(CkMyNode()) + i;
      gProxy[myPE].ngMsgReceived();
    }
  }

};

class G1 : public CBase_G1 {
  CkCallback cb;

  public:
  G1() {
    DEBUG(CkPrintf("[%d][%d][%d] G1 Constructor\n", CmiMyPe(), CmiMyNode(), CmiMyRank());)
    cb = CkCallback(CkReductionTarget(main, done), mainProxy);
  }

  void ngMsgReceived() {

    // Get a pointer to the received NG message
    ngMsg *receivedMsg = ngProxy[CkMyNode()].ckLocalBranch()->getReceivedMsgPointer();

    receivedMsg->verify();

    CkAssert(receivedMsg->k == kVal);
    DEBUG(CkPrintf("[%d][%d][%d] ****** G1 received msg pointer %p, deleting\n", CmiMyPe(), CmiMyNode(), CmiMyRank(), receivedMsg);)

    // Delete the message
    delete receivedMsg;
    contribute(cb);
  }
};


#include "ng_test.def.h"
