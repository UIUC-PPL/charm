#ifndef __MAIN_H__
#define __MAIN_H__

#include "defs.h"

//central controller chare
class Main : public CBase_Main {
  private:
    Main_SDAG_CODE
    double startBenchmarkTime;
    int startParticles = 0;
    int forceChecks = 0;
    double lastProgress = 0;
    void checkForceSum(double *v, int n);

  public:
    Main(CkArgMsg* msg);
    Main(CkMigrateMessage* msg);
    void pup(PUP::er &p);
    void checkProgress();
};
#endif
