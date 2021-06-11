#include "megaZCPingpong.decl.h"

CProxy_main mainProxy;
int minSize, maxSize, smallIter, bigIter, printFormat;

class main : public CBase_main {
  CProxy_Ping1 arr1;
  int size;
  bool warmUp;
  public:
  main(CkArgMsg *m) {
    if(CkNumPes() > 2) {
      CkAbort("Run this program on 1 or 2 processors only\n");
    }
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
      smallIter = 1000;
      bigIter = 100;
      printFormat = 0;
    } else {
      CkPrintf("Usage: ./pingpong <min size> <max size> <small message iter> <big message iter> <print format (0 for csv, 1 for regular)\n");
      CkExit(1);
    }
    if(printFormat != 0 && printFormat != 1) {
      CkPrintf("<print format> cannot be a value other than 0 or 1 (0 for csv, 1 for regular)\n");
      CkExit(1);
    }
    delete m;
    size = minSize;
    mainProxy = thisProxy;
    warmUp = true;

    if(printFormat == 0) { // csv print format

      CkPrintf("Size (Bytes),Iterations,Regular Send(us),ZC EM Send UNREG mode(us),ZC EM Send REG Mode (us),ZC EM Send PREREG Mode(us),Regular Recv with Copy(us),ZC EM Send with Copy(us),ZC Direct UNREG(us),ZC Direct REG(us),ZC Direct PREREG(us),ZC Direct (Reg/Dereg),ZC Post Recv UNREG(us),ZC Post Recv REG(us),ZC Post Recv PREREG(us), ZC Match and Post Recv UNREG(us), ZC Match and Post Recv REG(us), ZC Match and Post Recv PREREG(us), Reg Time(us),Dereg Time(us),Reg + Dereg Time 1(us),Reg + Dereg Time 2(us)\n");

    } else { // regular print format

      CkPrintf("Size (bytes)\t\tIterations\t||\tRegular Send\tZC EM Send1\tZC EM Send 2\tZC EM Send 3\t||\tRegular Recv with Copy\tZC EM Send with Copy\tZC Direct1\tZC Direct2\tZC Direct3\tZC Direct (Reg/Dereg)\tZC EM Recv1\tZC EM Recv2\tZC EM Recv3\tZC EM MP Recv1\tZC EM MP Recv2\tZC EM MP Recv3||\tReg time\tDereg time\tReg+Dereg 1\tReg+Dereg 2\n");

    }

    arr1 = CProxy_Ping1::ckNew(2);

    CkStartQD(CkCallback(CkIndex_main::maindone(), mainProxy));
  }

  void maindone() {
    if(size <= maxSize) {
      arr1[0].start(size, warmUp);
      warmUp = false;
      size = size << 1;
    } else {
      CkExit();
    }
  }

};

class Ping1 : public CBase_Ping1 {
  int size;
  int niter;
  int iterations;
  double start_time, end_time;
  char *nocopySrcBuffer, *nocopyDestBuffer;
  char *nocopySrcBufferReg, *nocopyDestBufferReg;

  CkNcpyBuffer src, dest;

  bool warmUp;
  double reg_send_time, zc_em_send_time1, zc_em_send_time2, zc_em_send_time3;
  double reg_recv_time, zc_direct_time1, zc_direct_time2, zc_direct_time3, zc_direct_time4;
  double zc_em_recv_time, zc_recv_time1, zc_recv_time2, zc_recv_time3;
  double zc_recv_mp_time1, zc_recv_mp_time2, zc_recv_mp_time3;

  double time1, time2, time3;
  double regTime, deregTime, regDeregSumTime, regDeregSumTime2;

  int directCounter;

  int tag1 = 200;

  public:
    Ping1() {
      nocopySrcBuffer = new char[maxSize];
      nocopyDestBuffer = new char[maxSize];

      nocopySrcBufferReg = (char *)CkRdmaAlloc(sizeof(char) * maxSize);
      nocopyDestBufferReg = (char *)CkRdmaAlloc(sizeof(char) * maxSize);

      niter = 0;
      directCounter = 0;
    }

    void start(int _size, bool _warmUp) {
      niter = 0;
      regTime = 0;
      deregTime = 0;

      size = _size;
      if(size >= 1 << 19)
        iterations = bigIter;
      else
        iterations = smallIter;

      warmUp = _warmUp;
      start_time = CkWallTimer();
      thisProxy[1].regularRecvOnly(nocopySrcBuffer, size);
    }

    // Send only regular Entry Method
    void regularRecvOnly(char *msg, int size) {
      if(thisIndex==0) {
        niter++;
        if(niter==iterations) {
          end_time = CkWallTimer();
          reg_send_time = 1.0e6*(end_time-start_time)/iterations;
          niter = 0;
          start_time = CkWallTimer();
          thisProxy[1].zerocopyEMSendApi1(CkSendBuffer(nocopySrcBuffer, CK_BUFFER_UNREG), size);
        } else {
          thisProxy[1].regularRecvOnly(nocopySrcBuffer, size);
        }
      } else {
        thisProxy[0].regularRecvOnly(nocopySrcBuffer, size);
      }
    }

    // Send only Zerocopy Entry Method Send API
    void zerocopyEMSendApi1(char *msg, int size) {
      if(thisIndex==0) {
        niter++;
        if(niter==iterations) {
          end_time = CkWallTimer();
          zc_em_send_time1 = 1.0e6*(end_time-start_time)/iterations;
          niter=0;
          start_time = CkWallTimer();
          thisProxy[1].zerocopyEMSendApi2(CkSendBuffer(nocopySrcBuffer, CK_BUFFER_REG), size);
        } else {
          thisProxy[1].zerocopyEMSendApi1(CkSendBuffer(nocopySrcBuffer, CK_BUFFER_UNREG), size);
        }
      } else {
        thisProxy[0].zerocopyEMSendApi1(CkSendBuffer(nocopySrcBuffer, CK_BUFFER_UNREG), size);
      }
    }

    void zerocopyEMSendApi2(char *msg, int size) {
      if(thisIndex==0) {
        niter++;
        if(niter==iterations) {
          end_time = CkWallTimer();
          zc_em_send_time2 = 1.0e6*(end_time-start_time)/iterations;
          niter=0;
          start_time = CkWallTimer();
          thisProxy[1].zerocopyEMSendApi3(CkSendBuffer(nocopySrcBufferReg, CK_BUFFER_PREREG), size);
        } else {
          thisProxy[1].zerocopyEMSendApi2(CkSendBuffer(nocopySrcBuffer, CK_BUFFER_REG), size);
        }
      } else {
        thisProxy[0].zerocopyEMSendApi2(CkSendBuffer(nocopySrcBuffer, CK_BUFFER_REG), size);
      }
    }

    void zerocopyEMSendApi3(char *msg, int size) {
      if(thisIndex==0) {
        niter++;
        if(niter==iterations) {
          end_time = CkWallTimer();
          zc_em_send_time3 = 1.0e6*(end_time-start_time)/iterations;
          niter=0;
          start_time = CkWallTimer();
          thisProxy[1].regularRecvAndCopy(nocopySrcBuffer, size);
        } else {
          thisProxy[1].zerocopyEMSendApi3(CkSendBuffer(nocopySrcBufferReg, CK_BUFFER_PREREG), size);
        }
      } else {
        thisProxy[0].zerocopyEMSendApi3(CkSendBuffer(nocopySrcBufferReg, CK_BUFFER_PREREG), size);
      }
    }

    // Send and Recv for regular Entry Method
    void regularRecvAndCopy(char *msg, int size) {
      memcpy(nocopyDestBuffer, msg, size);
      if(thisIndex==0) {
        niter++;
        if(niter==iterations) {
          end_time = CkWallTimer();
          reg_recv_time = 1.0e6*(end_time-start_time)/iterations;
          niter = 0;
          start_time = CkWallTimer();
          thisProxy[1].zerocopyEMSendApiAndCopy(CkSendBuffer(nocopySrcBuffer), size);

        } else {
          thisProxy[1].regularRecvAndCopy(nocopySrcBuffer, size);
        }
      } else {
        thisProxy[0].regularRecvAndCopy(nocopySrcBuffer, size);
      }
    }

    // Send only ZC Entry Method Send API with Memcpy
    void zerocopyEMSendApiAndCopy(char *msg, int size) {
      memcpy(nocopyDestBuffer, msg, size);
      if(thisIndex==0) {
        niter++;
        if(niter==iterations) {
          end_time = CkWallTimer();
          zc_em_recv_time = 1.0e6*(end_time-start_time)/iterations;
          niter = 0;

          thisProxy.setupDirectPingpong1(size, iterations);

        } else {
          thisProxy[1].zerocopyEMSendApiAndCopy(CkSendBuffer(nocopySrcBuffer), size);
        }
      } else {
        thisProxy[0].zerocopyEMSendApiAndCopy(CkSendBuffer(nocopySrcBuffer), size);
      }
    }

    void setupDirectPingpong1(int _size, int _iterations) {

      size = _size;
      iterations = _iterations;

      CkCallback srcCb = CkCallback(CkCallback::ignore);
      src = CkNcpyBuffer(nocopySrcBuffer, sizeof(char) * size, srcCb, CK_BUFFER_UNREG, CK_BUFFER_NODEREG);

      CkCallback destCb = CkCallback(CkIndex_Ping1::getCompleteDest1(), thisProxy[thisIndex]);
      dest = CkNcpyBuffer(nocopyDestBuffer, sizeof(char) * size, destCb, CK_BUFFER_UNREG, CK_BUFFER_NODEREG);

      thisProxy[0].beginDirectPingpong1();
    }

    void beginDirectPingpong1() {
      if(++directCounter == 2) {
        directCounter = 0;
        start_time = CkWallTimer();
        thisProxy[1].recvNcpySrcInfo(src);
      }
    }

    void recvNcpySrcInfo(CkNcpyBuffer src) {
      dest.get(src);
    }

    void getCompleteDest1() {
      if(thisIndex==0) {
        niter++;
        if(niter==iterations) {
          end_time = CkWallTimer();
          zc_direct_time1 = 1.0e6*(end_time-start_time)/iterations;
          niter = 0;

          thisProxy.setupDirectPingpong2(size, iterations);

        } else {
          thisProxy[1].recvNcpySrcInfo(src);
        }
      } else {
        thisProxy[0].recvNcpySrcInfo(src);
      }
    }

    void setupDirectPingpong2(int _size, int _iterations) {
      size = _size;
      iterations = _iterations;

      CkCallback srcCb = CkCallback(CkCallback::ignore);
      src = CkNcpyBuffer(nocopySrcBuffer, sizeof(char) * size, srcCb, CK_BUFFER_REG, CK_BUFFER_NODEREG);

      CkCallback destCb = CkCallback(CkIndex_Ping1::getCompleteDest2(), thisProxy[thisIndex]);
      dest = CkNcpyBuffer(nocopyDestBuffer, sizeof(char) * size, destCb, CK_BUFFER_REG, CK_BUFFER_NODEREG);

      thisProxy[0].beginDirectPingpong2();
    }

    void beginDirectPingpong2() {
      if(++directCounter == 2) {
        directCounter = 0;
        start_time = CkWallTimer();
        thisProxy[1].recvNcpySrcInfo(src);
      }
    }

    void getCompleteDest2() {
      if(thisIndex==0) {
        niter++;

        if(niter==iterations) {
          end_time = CkWallTimer();
          zc_direct_time2 = 1.0e6*(end_time-start_time)/iterations;
          niter = 0;

          thisProxy.setupDirectPingpong3(size, iterations);

        } else {
          thisProxy[1].recvNcpySrcInfo(src);
        }
      } else {
        thisProxy[0].recvNcpySrcInfo(src);
      }
    }

    void setupDirectPingpong3(int _size, int _iterations) {
      size = _size;
      iterations = _iterations;

      CkCallback srcCb = CkCallback(CkCallback::ignore);
      src = CkNcpyBuffer(nocopySrcBufferReg, sizeof(char) * size, srcCb, CK_BUFFER_PREREG, CK_BUFFER_NODEREG);

      CkCallback destCb = CkCallback(CkIndex_Ping1::getCompleteDest3(), thisProxy[thisIndex]);
      dest = CkNcpyBuffer(nocopyDestBufferReg, sizeof(char) * size, destCb, CK_BUFFER_PREREG, CK_BUFFER_NODEREG);

      thisProxy[0].beginDirectPingpong3();
    }

    void beginDirectPingpong3() {
      if(++directCounter == 2) {
        directCounter = 0;
        start_time = CkWallTimer();
        thisProxy[1].recvNcpySrcInfo(src);
      }
    }

    void getCompleteDest3() {
      if(thisIndex==0) {
        niter++;

        if(niter==iterations) {
          end_time = CkWallTimer();
          zc_direct_time3 = 1.0e6*(end_time-start_time)/iterations;
          niter = 0;

          thisProxy.setupDirectPingpong4(size, iterations);

        } else {
          thisProxy[1].recvNcpySrcInfo(src);
        }
      } else {
        thisProxy[0].recvNcpySrcInfo(src);
      }
    }

    void setupDirectPingpong4(int _size, int _iterations) {
      size = _size;
      iterations = _iterations;

      CkCallback srcCb = CkCallback(CkCallback::ignore);
      src = CkNcpyBuffer(nocopySrcBuffer, sizeof(char) * size, srcCb, CK_BUFFER_REG, CK_BUFFER_NODEREG);

      CkCallback destCb = CkCallback(CkIndex_Ping1::getCompleteDest4(), thisProxy[thisIndex]);
      dest = CkNcpyBuffer(nocopyDestBuffer, sizeof(char) * size, destCb, CK_BUFFER_REG, CK_BUFFER_NODEREG);

      thisProxy[0].beginDirectPingpong4();
    }

    void beginDirectPingpong4() {
      if(++directCounter == 2) {
        directCounter = 0;
        start_time = CkWallTimer();
        thisProxy[1].recvNcpySrcInfo(src);
      }
    }

    void getCompleteDest4() {
      if(thisIndex==0) {
        niter++;

        time1 = CkWallTimer();

        // De-register the buffer
        dest.deregisterMem();

        time2 = CkWallTimer();

        // Register the buffer
        dest.registerMem();

        time3 = CkWallTimer();

        regTime += time3 - time2;
        deregTime += time2 - time1;

        if(niter==iterations) {
          end_time = CkWallTimer();
          zc_direct_time4 = 1.0e6*(end_time-start_time)/iterations;
          regTime = 1.0e6 * regTime; // convert regTime to us
          deregTime = 1.0e6 * deregTime; // convert deregTime to us
          niter = 0;

          start_time = CkWallTimer();
          thisProxy[1].zerocopyEMRecvApi1(CkSendBuffer(nocopySrcBuffer, CK_BUFFER_UNREG), size);

        } else {
          thisProxy[1].recvNcpySrcInfo(src);
        }
      } else {
        thisProxy[0].recvNcpySrcInfo(src);
      }
    }

    void zerocopyEMRecvApi1(char *&msg, int &size, CkNcpyBufferPost *ncpyPost) {
      msg = nocopyDestBuffer;

      ncpyPost[0].regMode = CK_BUFFER_UNREG;
    }

    void zerocopyEMRecvApi1(char *msg, int size) {
      if(thisIndex==0) {
        niter++;
        if(niter==iterations) {
          end_time = CkWallTimer();
          zc_recv_time1 = 1.0e6*(end_time-start_time)/iterations;

          niter = 0;

          start_time = CkWallTimer();
          thisProxy[1].zerocopyEMRecvApi2(CkSendBuffer(nocopySrcBuffer, CK_BUFFER_REG), size);

        } else {
          thisProxy[1].zerocopyEMRecvApi1(CkSendBuffer(nocopySrcBuffer, CK_BUFFER_UNREG), size);
        }
      } else {
        thisProxy[0].zerocopyEMRecvApi1(CkSendBuffer(nocopySrcBuffer, CK_BUFFER_UNREG), size);
      }
    }

    void zerocopyEMRecvApi2(char *&msg, int &size, CkNcpyBufferPost *ncpyPost) {
      msg = nocopyDestBuffer;

      ncpyPost[0].regMode = CK_BUFFER_REG;
    }

    void zerocopyEMRecvApi2(char *msg, int size) {
      if(thisIndex==0) {
        niter++;
        if(niter==iterations) {
          end_time = CkWallTimer();
          zc_recv_time2 = 1.0e6*(end_time-start_time)/iterations;

          niter = 0;

          start_time = CkWallTimer();
          thisProxy[1].zerocopyEMRecvApi3(CkSendBuffer(nocopySrcBufferReg, CK_BUFFER_PREREG), size);

        } else {
          thisProxy[1].zerocopyEMRecvApi2(CkSendBuffer(nocopySrcBuffer, CK_BUFFER_REG), size);
        }
      } else {
        thisProxy[0].zerocopyEMRecvApi2(CkSendBuffer(nocopySrcBuffer, CK_BUFFER_REG), size);
      }
    }

    void zerocopyEMRecvApi3(char *&msg, int &size, CkNcpyBufferPost *ncpyPost) {
      msg = nocopyDestBufferReg;

      ncpyPost[0].regMode = CK_BUFFER_PREREG;
    }

    // Send and Recv for ZC Entry Method API
    void zerocopyEMRecvApi3(char *msg, int size) {
      if(thisIndex==0) {
        niter++;
        if(niter==iterations) {
          end_time = CkWallTimer();
          zc_recv_time3 = 1.0e6*(end_time-start_time)/iterations;

          niter = 0;
          start_time = CkWallTimer();
          thisProxy[1].zerocopyEMRecvApiMatchAndPost1(CkSendBuffer(nocopySrcBuffer, CK_BUFFER_UNREG), size);

        } else {
          thisProxy[1].zerocopyEMRecvApi3(CkSendBuffer(nocopySrcBufferReg, CK_BUFFER_PREREG), size);
        }
      } else {
        thisProxy[0].zerocopyEMRecvApi3(CkSendBuffer(nocopySrcBufferReg, CK_BUFFER_PREREG), size);
      }
    }

    void zerocopyEMRecvApiMatchAndPost1(char *&msg, int &size, CkNcpyBufferPost *ncpyPost) {
      ncpyPost[0].regMode = CK_BUFFER_UNREG;

      CkMatchBuffer(ncpyPost, 0, tag1);
      CkPostBuffer(nocopyDestBuffer, size, tag1);
    }

    void zerocopyEMRecvApiMatchAndPost1(char *msg, int size) {
      if(thisIndex==0) {
        niter++;
        if(niter==iterations) {
          end_time = CkWallTimer();
          zc_recv_mp_time1 = 1.0e6*(end_time-start_time)/iterations;

          niter = 0;

          start_time = CkWallTimer();
          thisProxy[1].zerocopyEMRecvApiMatchAndPost2(CkSendBuffer(nocopySrcBuffer, CK_BUFFER_REG), size);

        } else {
          thisProxy[1].zerocopyEMRecvApiMatchAndPost1(CkSendBuffer(nocopySrcBuffer, CK_BUFFER_UNREG), size);
        }
      } else {
        thisProxy[0].zerocopyEMRecvApiMatchAndPost1(CkSendBuffer(nocopySrcBuffer, CK_BUFFER_UNREG), size);
      }
    }

    void zerocopyEMRecvApiMatchAndPost2(char *&msg, int &size, CkNcpyBufferPost *ncpyPost) {
      ncpyPost[0].regMode = CK_BUFFER_REG;

      CkMatchBuffer(ncpyPost, 0, tag1);
      CkPostBuffer(nocopyDestBuffer, size, tag1);
    }

    void zerocopyEMRecvApiMatchAndPost2(char *msg, int size) {
      if(thisIndex==0) {
        niter++;
        if(niter==iterations) {
          end_time = CkWallTimer();
          zc_recv_mp_time2 = 1.0e6*(end_time-start_time)/iterations;

          niter = 0;

          start_time = CkWallTimer();
          thisProxy[1].zerocopyEMRecvApiMatchAndPost3(CkSendBuffer(nocopySrcBufferReg, CK_BUFFER_PREREG), size);

        } else {
          thisProxy[1].zerocopyEMRecvApiMatchAndPost2(CkSendBuffer(nocopySrcBuffer, CK_BUFFER_REG), size);
        }
      } else {
        thisProxy[0].zerocopyEMRecvApiMatchAndPost2(CkSendBuffer(nocopySrcBuffer, CK_BUFFER_REG), size);
      }
    }

    void zerocopyEMRecvApiMatchAndPost3(char *&msg, int &size, CkNcpyBufferPost *ncpyPost) {
      ncpyPost[0].regMode = CK_BUFFER_PREREG;

      CkMatchBuffer(ncpyPost, 0, tag1);
      CkPostBuffer(nocopyDestBufferReg, size, tag1);
    }

    // Send and Recv for ZC Entry Method API
    void zerocopyEMRecvApiMatchAndPost3(char *msg, int size) {
      if(thisIndex==0) {
        niter++;
        if(niter==iterations) {
          end_time = CkWallTimer();
          zc_recv_mp_time3 = 1.0e6*(end_time-start_time)/iterations;
          if(warmUp == false) {
            printOutput();
          }
          niter=0;
          mainProxy.maindone();
        } else {
          thisProxy[1].zerocopyEMRecvApiMatchAndPost3(CkSendBuffer(nocopySrcBufferReg, CK_BUFFER_PREREG), size);
        }
      } else {
        thisProxy[0].zerocopyEMRecvApiMatchAndPost3(CkSendBuffer(nocopySrcBufferReg, CK_BUFFER_PREREG), size);
      }
    }

    void printOutput() {
      if(printFormat == 0) { // csv print format

        CkPrintf("%d,%d,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf\n", size, iterations, reg_send_time/2, zc_em_send_time1/2, zc_em_send_time2/2, zc_em_send_time3/2, reg_recv_time/2, zc_em_recv_time/2, zc_direct_time1/2, zc_direct_time2/2, zc_direct_time3/2, zc_direct_time4/2, zc_recv_time1/2, zc_recv_time2/2, zc_recv_time3/2, zc_recv_mp_time1/2, zc_recv_mp_time2/2, zc_recv_mp_time3/2, regTime/iterations, deregTime/iterations, (regTime + deregTime)/iterations, zc_direct_time4 - zc_direct_time2);
      }
      else { // regular print format

        if(size < 1 << 24) {

          CkPrintf("%d\t\t\t%d\t\t||\t%lf\t%lf\t%lf\t%lf\t||\t%lf\t\t%lf\t\t%lf\t%lf\t%lf\t%lf\t\t%lf\t%lf\t%lf\t%lf\t%lf\t%lf\t||\t%lf\t%lf\t%lf\t%lf\n", size, iterations, reg_send_time/2, zc_em_send_time1/2, zc_em_send_time2/2, zc_em_send_time3/2, reg_recv_time/2, zc_em_recv_time/2, zc_direct_time1/2, zc_direct_time2/2, zc_direct_time3/2, zc_direct_time4/2, zc_recv_time1/2, zc_recv_time2/2, zc_recv_time3/2, zc_recv_mp_time1/2, zc_recv_mp_time2/2, zc_recv_mp_time3/2, regTime/iterations, deregTime/iterations, (regTime + deregTime)/iterations, zc_direct_time4 - zc_direct_time2);

        } else { //using different print format for larger numbers for aligned output

          CkPrintf("%d\t\t%d\t\t||\t%lf\t%lf\t%lf\t%lf\t||\t%lf\t\t%lf\t\t%lf\t%lf\t%lf\t%lf\t\t%lf\t%lf\t%lf\t%lf\t%lf\t%lf\t||\t%lf\t%lf\t%lf\t%lf\n", size, iterations, reg_send_time/2, zc_em_send_time1/2, zc_em_send_time2/2, zc_em_send_time3/2, reg_recv_time/2, zc_em_recv_time/2, zc_direct_time1/2, zc_direct_time2/2,  zc_direct_time3/2, zc_direct_time4/2, zc_recv_time1/2, zc_recv_time2/2, zc_recv_time3/2, zc_recv_mp_time1/2, zc_recv_mp_time2/2, zc_recv_mp_time3/2, regTime/iterations, deregTime/iterations, (regTime + deregTime)/iterations, zc_direct_time4 - zc_direct_time2);

        }
      }
    }

};

#include "megaZCPingpong.def.h"
