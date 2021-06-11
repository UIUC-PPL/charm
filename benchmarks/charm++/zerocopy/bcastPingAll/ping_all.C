#include <string.h> // for strlen, and strcmp

#include <charm++.h>

#include "ping_all.decl.h"

CProxy_main mainProxy;
int printFormat;

#define DEBUG(x) //x

class main : public CBase_main
{
  int niter, send_counter, recv_counter, iterations;
  size_t minSize, maxSize, smallIter, bigIter;
  char *buffer, *regBuffer;
  CProxy_LargeDataNodeGroup ngid;
  double start_time, end_time, reg_time1, zcpy_send_time1, zcpy_send_time2, zcpy_send_time3;
  double reg_time2, zcpy_send_with_copy_time, zcpy_recv_time1, zcpy_recv_time2, zcpy_recv_time3;
  double zcpy_recv_mp_time1, zcpy_recv_mp_time2, zcpy_recv_mp_time3;
  CkCallback doneCb;
  size_t size;
  bool warmUp;
  int zc_current_operation;

public:
  main(CkMigrateMessage *m) {}
  main(CkArgMsg* m)
  {
    if(m->argc == 6) {
      minSize = atoi(m->argv[1])/2; // Start with a smaller size to run a warm up phase
      maxSize = atoi(m->argv[2]);
      smallIter = atoi(m->argv[3]);
      bigIter = atoi(m->argv[4]);
      printFormat = atoi(m->argv[5]);
    } else if(m->argc == 1) {
      // use defaults
      minSize = 16; // Start with a smaller size to run a warm up phase before starting message size at 1024 bytes
      maxSize = 1 << 25;
      smallIter = 100;
      bigIter = 10;
      printFormat = 0;
    } else {
      CkPrintf("Usage: ./ping_all <min size> <max size> <small message iter> <big message iter> <print format (0 for csv, 1 for regular)\n");
      CkExit(1);
    }
    if(printFormat != 0 && printFormat != 1) {
      CkPrintf("<print format> cannot be a value other than 0 or 1 (0 for csv, 1 for regular)\n");
      CkExit(1);
    }
    delete m;
    // Initialize
    size = minSize;
    niter = 0;
    send_counter = 0;
    recv_counter = 0;
    mainProxy = thisProxy;
    warmUp = true;
    iterations = smallIter;
    zc_current_operation = 1;

    // Allocate a buffer to send
    buffer = new char[maxSize];

    // Allocate another buffer from pre-registered memory to send
    regBuffer = (char *)CkRdmaAlloc(sizeof(char) * maxSize);

    // Create a nodegroup
    ngid = CProxy_LargeDataNodeGroup::ckNew(maxSize);

    int idx_zerocopySendDone = CkIndex_main::recv_zc_send_free_ptr(NULL);
    doneCb = CkCallback(idx_zerocopySendDone, thisProxy);

    if(printFormat == 0) // csv print format
      CkPrintf("Size (Bytes),Iterations,Regular Bcast(us),ZC EM Bcast Send UNREG mode(us),ZC EM Bcast Send REG Mode (us),ZC EM Bcast Send PREREG Mode(us),Regular Bcast with Copy(us),ZC EM Bcast Send with Copy(us),ZC Bcast Post UNREG(us),ZC Bcast Post REG(us),ZC Bcast Post PREREG(us), ZC Bcast Match and Post UNREG(us),ZC Bcast Match and Post REG(us),ZC Bcast Match and Post PREREG(us)\n");
    else // regular print format
      CkPrintf("Size (Bytes)\t\tIterations\t||\tRegular Bcast(us)\tZC EM Bcast 1(us)\tZC EM Bcast 2(us)\tZC EM Bcast 2(us)\t||\tRegular Bcast & Copy(us)\tZC EM Bcast Send & Copy(us)\tZC Bcast Post 1(us)\tZC Bcast Post 2(us)\tZC Bcast Post 3(us)\tZC Bcast Match and Post UNREG(us)\tZC Bcast Match and Post REG(us)\tZC Bcast Match and Post PREREG(us)\n");
    CkStartQD(CkCallback(CkIndex_main::start(), mainProxy));
  }

  void start() {
    if(size >= 1 << 19)
      iterations = bigIter;
    else
      iterations = smallIter;

    if(size < minSize) {
      // warmUp phase
      start_time = CkWallTimer();
      ngid.recv(buffer, size, niter, warmUp, iterations);
    } else if(size <= maxSize) {
      // regular experiment phase
      start_time = CkWallTimer();
      ngid.recv(buffer, size, niter, warmUp, iterations);
    } else {
      // completion phase
      done();
    }
  }

  void recv_zc_send_free_ptr(CkDataMsg *msg) {
    switch(zc_current_operation) {
      case 1:   recv_zc_send_done1();
                break;
      case 2:   recv_zc_send_done2();
                break;
      case 3:   recv_zc_send_done3();
                break;
      case 4:   recv_zc_send_with_copy_done();
                break;
      case 5:   recv_zc_post_done1();
                break;
      case 6:   recv_zc_post_done2();
                break;
      case 7:   recv_zc_post_done3();
                break;
      case 8:   recv_zc_match_and_post_done1();
                break;
      case 9:   recv_zc_match_and_post_done2();
                break;
      case 10:  recv_zc_match_and_post_done3();
                break;
      default:  CmiAbort("Incorrect mode\n");
                break;
    }
  }



  // Invoked on main after a reduction by all the nodegroup elements
  void recv_done() {
    niter++; // An iteration of the Regular Bcast API is complete
    if(niter == iterations) {
      end_time = CkWallTimer();
      reg_time1 = 1.0e6*(end_time - start_time)/iterations;
      niter = 0;
      zc_current_operation = 1;
      start_time = CkWallTimer();
      ngid.recv_zc_send1(CkSendBuffer(buffer, doneCb, CK_BUFFER_UNREG), size, niter, warmUp, iterations);
    } else {
      ngid.recv(buffer, size, niter, warmUp, iterations);
    }
  }

  void recv_zc_send_done1() {
    send_counter++;
    if(send_counter == 2) {
      send_counter = 0;
      niter++; // An iteration of the Zerocopy Bcast Send API is complete
      if(niter == iterations) {
        end_time = CkWallTimer();
        zcpy_send_time1 = 1.0e6*(end_time - start_time)/iterations;
        niter = 0;
        zc_current_operation = 2;
        start_time = CkWallTimer();
        ngid.recv_zc_send2(CkSendBuffer(buffer, doneCb, CK_BUFFER_REG), size, niter, warmUp, iterations);
      } else {
        ngid.recv_zc_send1(CkSendBuffer(buffer, doneCb, CK_BUFFER_UNREG), size, niter, warmUp, iterations);
      }
    }
  }

  void recv_zc_send_done2() {
    send_counter++;
    if(send_counter == 2) {
      send_counter = 0;
      niter++; // An iteration of the Zerocopy Bcast Send API is complete
      if(niter == iterations) {
        end_time = CkWallTimer();
        zcpy_send_time2 = 1.0e6*(end_time - start_time)/iterations;
        niter = 0;
        zc_current_operation = 3;
        start_time = CkWallTimer();
        ngid.recv_zc_send3(CkSendBuffer(regBuffer, doneCb, CK_BUFFER_PREREG), size, niter, warmUp, iterations);
      } else {
        ngid.recv_zc_send2(CkSendBuffer(buffer, doneCb, CK_BUFFER_REG), size, niter, warmUp, iterations);
      }
    }
  }

  void recv_zc_send_done3() {
    send_counter++;
    if(send_counter == 2) {
      send_counter = 0;
      niter++; // An iteration of the Zerocopy Bcast Send API is complete
      if(niter == iterations) {
        end_time = CkWallTimer();
        zcpy_send_time3 = 1.0e6*(end_time - start_time)/iterations;
        niter = 0;
        start_time = CkWallTimer();
        ngid.recv_with_copy(buffer, size, niter, warmUp, iterations);
      } else {
        ngid.recv_zc_send3(CkSendBuffer(regBuffer, doneCb, CK_BUFFER_PREREG), size, niter, warmUp, iterations);
      }
    }
  }

  void recv_with_copy_done() {
    niter++; // An iteration of the Regular Bcast API is complete
    if(niter == iterations) {
      end_time = CkWallTimer();
      reg_time2 = 1.0e6*(end_time - start_time)/iterations;
      niter = 0;
      zc_current_operation = 4;
      start_time = CkWallTimer();
      ngid.recv_zc_send_with_copy(CkSendBuffer(buffer, doneCb), size, niter, warmUp, iterations);
    } else {
      ngid.recv_with_copy(buffer, size, niter, warmUp, iterations);
    }
  }

  void recv_zc_send_with_copy_done() {
    send_counter++;
    if(send_counter == 2) {
      send_counter = 0;
      niter++; // An iteration of the Zerocopy Bcast Send API is complete
      if(niter == iterations) {
        end_time = CkWallTimer();
        zcpy_send_with_copy_time = 1.0e6*(end_time - start_time)/iterations;
        niter = 0;
        zc_current_operation = 5;
        start_time = CkWallTimer();
        ngid.recv_zc_post1(CkSendBuffer(buffer, doneCb, CK_BUFFER_UNREG), size, niter, warmUp, iterations);
      } else {
        ngid.recv_zc_send_with_copy(CkSendBuffer(buffer, doneCb), size, niter, warmUp, iterations);
      }
    }
  }

  void recv_zc_post_done1() {
    recv_counter++;
    if(recv_counter == 2) {
      recv_counter = 0;
      niter++; // An iteration of the Zerocopy Bcast Recv API is complete
      if(niter == iterations) {
        end_time = CkWallTimer();
        zcpy_recv_time1 = 1.0e6*(end_time - start_time)/iterations;
        niter = 0;
        zc_current_operation = 6;
        start_time = CkWallTimer();
        ngid.recv_zc_post2(CkSendBuffer(buffer, doneCb, CK_BUFFER_REG), size, niter, warmUp, iterations);
      } else {
        ngid.recv_zc_post1(CkSendBuffer(buffer, doneCb, CK_BUFFER_UNREG), size, niter, warmUp, iterations);
      }
    }
  }

  void recv_zc_post_done2() {
    recv_counter++;
    if(recv_counter == 2) {
      recv_counter = 0;
      niter++; // An iteration of the Zerocopy Bcast Recv API is complete
      if(niter == iterations) {
        end_time = CkWallTimer();
        zcpy_recv_time2 = 1.0e6*(end_time - start_time)/iterations;
        niter = 0;
        zc_current_operation = 7;
        start_time = CkWallTimer();
        ngid.recv_zc_post3(CkSendBuffer(regBuffer, doneCb, CK_BUFFER_PREREG), size, niter, warmUp, iterations);
      } else {
        ngid.recv_zc_post2(CkSendBuffer(buffer, doneCb, CK_BUFFER_REG), size, niter, warmUp, iterations);
      }
    }
  }

  void recv_zc_post_done3() {
    recv_counter++;
    if(recv_counter == 2) {
      recv_counter = 0;
      niter++; // An iteration of the Zerocopy Bcast Recv API is complete
      if(niter == iterations) {
        end_time = CkWallTimer();
        zcpy_recv_time3 = 1.0e6*(end_time - start_time)/iterations;
        niter = 0;
        zc_current_operation = 8;
        start_time = CkWallTimer();
        ngid.recv_zc_match_and_post1(CkSendBuffer(buffer, doneCb, CK_BUFFER_UNREG), size, niter, warmUp, iterations);
      } else {
        ngid.recv_zc_post3(CkSendBuffer(regBuffer, doneCb, CK_BUFFER_PREREG), size, niter, warmUp, iterations);
      }
    }
  }

  void recv_zc_match_and_post_done1() {
    recv_counter++;
    if(recv_counter == 2) {
      recv_counter = 0;
      niter++; // An iteration of the Zerocopy Bcast Recv API is complete
      if(niter == iterations) {
        end_time = CkWallTimer();
        zcpy_recv_mp_time1 = 1.0e6*(end_time - start_time)/iterations;
        niter = 0;
        zc_current_operation = 9;
        start_time = CkWallTimer();
        ngid.recv_zc_match_and_post2(CkSendBuffer(buffer, doneCb, CK_BUFFER_REG), size, niter, warmUp, iterations);
      } else {
        ngid.recv_zc_match_and_post1(CkSendBuffer(buffer, doneCb, CK_BUFFER_UNREG), size, niter, warmUp, iterations);
      }
    }
  }

  void recv_zc_match_and_post_done2() {
    recv_counter++;
    if(recv_counter == 2) {
      recv_counter = 0;
      niter++; // An iteration of the Zerocopy Bcast Recv API is complete
      if(niter == iterations) {
        end_time = CkWallTimer();
        zcpy_recv_mp_time2 = 1.0e6*(end_time - start_time)/iterations;
        niter = 0;
        zc_current_operation = 10;
        start_time = CkWallTimer();
        ngid.recv_zc_match_and_post3(CkSendBuffer(regBuffer, doneCb, CK_BUFFER_PREREG), size, niter, warmUp, iterations);
      } else {
        ngid.recv_zc_match_and_post2(CkSendBuffer(buffer, doneCb, CK_BUFFER_REG), size, niter, warmUp, iterations);
      }
    }
  }

  void recv_zc_match_and_post_done3() {
    recv_counter++;
    if(recv_counter == 2) {
      recv_counter = 0;
      niter++; // An iteration of the Zerocopy Bcast Recv API is complete
      if(niter == iterations) {
        end_time = CkWallTimer();
        zcpy_recv_mp_time3 = 1.0e6*(end_time - start_time)/iterations;
        niter = 0;

        printOutput();

        size = size << 1;
        if(warmUp)
          done();
        else
          mainProxy.start();
      } else {
        ngid.recv_zc_match_and_post3(CkSendBuffer(regBuffer, doneCb, CK_BUFFER_PREREG), size, niter, warmUp, iterations);
      }
    }
  }



  void printOutput() {
    if(warmUp == false) {
      if(printFormat == 0) // csv print format
        CkPrintf("%zu,%d,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf\n",size, iterations, reg_time1, zcpy_send_time1, zcpy_send_time2, zcpy_send_time3, reg_time2, zcpy_send_with_copy_time, zcpy_recv_time1, zcpy_recv_time2, zcpy_recv_time3, zcpy_recv_mp_time1, zcpy_recv_mp_time2, zcpy_recv_mp_time3);
      else { // regular print format
        if(size < 1 << 24)
          CkPrintf("%zu\t\t\t%d\t\t||\t%lf\t\t%lf\t\t%lf\t\t%lf\t\t||\t%lf\t\t\t%lf\t\t\t%lf\t\t%lf\t\t%lf\t\t%lf\t\t%lf\t\t%lf\n", size, iterations, reg_time1, zcpy_send_time1, zcpy_send_time2, zcpy_send_time3, reg_time2, zcpy_send_with_copy_time, zcpy_recv_time1, zcpy_recv_time2, zcpy_recv_time3, zcpy_recv_mp_time1, zcpy_recv_mp_time2, zcpy_recv_mp_time3);
        else
          CkPrintf("%zu\t\t%d\t\t||\t%lf\t\t%lf\t\t%lf\t\t%lf\t\t||\t%lf\t\t\t%lf\t\t\t%lf\t\t%lf\t\t%lf\t\t%lf\t\t%lf\t\t%lf\n", size, iterations, reg_time1, zcpy_send_time1, zcpy_send_time2, zcpy_send_time3, reg_time2, zcpy_send_with_copy_time, zcpy_recv_time1, zcpy_recv_time2, zcpy_recv_time3, zcpy_recv_mp_time1, zcpy_recv_mp_time2, zcpy_recv_mp_time3);
      }
    }
  }

  void done() {
    if(warmUp) {
      // warmUp phase complete
      warmUp = false;
      mainProxy.start();
    } else {
      // experiment complete
      CkExit();
    }
  }
};


class LargeDataNodeGroup : public CBase_LargeDataNodeGroup {

  CkCallback regCb1, zcSendCb1, zcSendCb2, zcSendCb3;
  CkCallback regCb2, zcSendWithCopyCb, zcRecvCb1, zcRecvCb2, zcRecvCb3;
  CkCallback zcRecvMpCb1, zcRecvMpCb2, zcRecvMpCb3;
  int tag1;
  char *myBuffer, *regBuffer;

public:
  LargeDataNodeGroup(int maxSize) {

    regCb1 = CkCallback(CkReductionTarget(main, recv_done), mainProxy);

    zcSendCb1 = CkCallback(CkReductionTarget(main, recv_zc_send_done1), mainProxy);
    zcSendCb2 = CkCallback(CkReductionTarget(main, recv_zc_send_done2), mainProxy);
    zcSendCb3 = CkCallback(CkReductionTarget(main, recv_zc_send_done3), mainProxy);

    regCb2 = CkCallback(CkReductionTarget(main, recv_with_copy_done), mainProxy);

    zcSendWithCopyCb = CkCallback(CkReductionTarget(main, recv_zc_send_with_copy_done), mainProxy);

    zcRecvCb1 = CkCallback(CkReductionTarget(main, recv_zc_post_done1), mainProxy);
    zcRecvCb2 = CkCallback(CkReductionTarget(main, recv_zc_post_done2), mainProxy);
    zcRecvCb3 = CkCallback(CkReductionTarget(main, recv_zc_post_done3), mainProxy);

    zcRecvMpCb1 = CkCallback(CkReductionTarget(main, recv_zc_match_and_post_done1), mainProxy);
    zcRecvMpCb2 = CkCallback(CkReductionTarget(main, recv_zc_match_and_post_done2), mainProxy);
    zcRecvMpCb3 = CkCallback(CkReductionTarget(main, recv_zc_match_and_post_done3), mainProxy);

    tag1 = 200;

    // allocate a large buffer to receive the sent buffer
    myBuffer = new char[maxSize];

    // Allocate another buffer from pre-registered memory to receive the sent buffer
    regBuffer = (char *)CkRdmaAlloc(sizeof(char) * maxSize);
  }

  void recv(char *msg, size_t size, int iter, bool warmUp, int iterations) {
    contribute(regCb1); // Nodegroup reduction to signal completion to the main chare
  }

  void recv_zc_send1(char *msg, size_t size, int iter, bool warmUp, int iterations) {
    contribute(zcSendCb1);
  }

  void recv_zc_send2(char *msg, size_t size, int iter, bool warmUp, int iterations) {
    contribute(zcSendCb2);
  }

  void recv_zc_send3(char *msg, size_t size, int iter, bool warmUp, int iterations) {
    contribute(zcSendCb3);
  }

  void recv_with_copy(char *msg, size_t size, int iter, bool warmUp, int iterations) {
    // manually copy the received message into myBuffer
    memcpy(myBuffer, msg, size);
    contribute(regCb2); // Nodegroup reduction to signal completion to the main chare
  }

  void recv_zc_send_with_copy(char *msg, size_t size, int iter, bool warmUp, int iterations) {
    // manually copy the received message into myBuffer
    memcpy(myBuffer, msg, size);
    contribute(zcSendWithCopyCb);
  }

  void recv_zc_post1(char *&msg, size_t &size, int iter, bool warmUp, int iterations, CkNcpyBufferPost *ncpyPost) {
    msg = myBuffer;
    ncpyPost[0].regMode = CK_BUFFER_UNREG;
  }

  void recv_zc_post1(char *msg, size_t size, int iter, bool warmUp, int iterations) {
    contribute(zcRecvCb1);
  }

  void recv_zc_post2(char *&msg, size_t &size, int iter, bool warmUp, int iterations, CkNcpyBufferPost *ncpyPost) {
    msg = myBuffer;
    ncpyPost[0].regMode = CK_BUFFER_REG;
  }

  void recv_zc_post2(char *msg, size_t size, int iter, bool warmUp, int iterations) {
    contribute(zcRecvCb2);
  }

  void recv_zc_post3(char *&msg, size_t &size, int iter, bool warmUp, int iterations, CkNcpyBufferPost *ncpyPost) {
    msg = regBuffer;
    ncpyPost[0].regMode = CK_BUFFER_PREREG;
  }

  void recv_zc_post3(char *msg, size_t size, int iter, bool warmUp, int iterations) {
    contribute(zcRecvCb3);
  }

  void recv_zc_match_and_post1(char *&msg, size_t &size, int iter, bool warmUp, int iterations, CkNcpyBufferPost *ncpyPost) {
    ncpyPost[0].regMode = CK_BUFFER_UNREG;
    CkMatchBuffer(ncpyPost, 0, tag1);
    CkPostBuffer(myBuffer, size, tag1);
  }

  void recv_zc_match_and_post1(char *msg, size_t size, int iter, bool warmUp, int iterations) {
    contribute(zcRecvMpCb1);
  }

  void recv_zc_match_and_post2(char *&msg, size_t &size, int iter, bool warmUp, int iterations, CkNcpyBufferPost *ncpyPost) {
    ncpyPost[0].regMode = CK_BUFFER_REG;
    CkMatchBuffer(ncpyPost, 0, tag1);
    CkPostBuffer(myBuffer, size, tag1);
  }

  void recv_zc_match_and_post2(char *msg, size_t size, int iter, bool warmUp, int iterations) {
    contribute(zcRecvMpCb2);
  }

  void recv_zc_match_and_post3(char *&msg, size_t &size, int iter, bool warmUp, int iterations, CkNcpyBufferPost *ncpyPost) {
    ncpyPost[0].regMode = CK_BUFFER_PREREG;
    CkMatchBuffer(ncpyPost, 0, tag1);
    CkPostBuffer(regBuffer, size, tag1);
  }

  void recv_zc_match_and_post3(char *msg, size_t size, int iter, bool warmUp, int iterations) {
    contribute(zcRecvMpCb3);
  }
};

#include "ping_all.def.h"

