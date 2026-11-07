#if !defined(CHARM_HPCCG)
#define CHARM_HPCCG

#include "HPCCG.hpp"
#include <string>
#include <map>
#include <vector>
#include <iostream>
#include <pup_stl.h>
#include "completion.h"
#include "charmHpccg.decl.h"

/*readonly*/ extern int numChares;
/*readonly*/ extern CProxy_CompletionDetector detector;
/*readonly*/ extern CProxy_charmMain mainProxy;

struct RemoteX {
  std::vector<int> rows, values;
  int offset;

  RemoteX() : offset(-1) { }

  RemoteX(std::vector<int> values_, int offset_)
    : values(values_)
    , offset(offset_) { }
};

class charmHpccg : public CBase_charmHpccg {
public:
  charmHpccg() { __sdag_init(); }
  charmHpccg(CkMigrateMessage*) { }
  void findExternals();
  void needXElements(int requester, std::vector<int> rows);
protected:
  HPC_Sparse_Matrix* A;
  double *x, *b, *xexact;
  std::map<int, RemoteX> xToReceive, xToSend; // chare index -> rows of x

  double *r, *p, *Ap;
  double normr, rtrans;
  int xMessagesReceived;
  int iteration;
  charmHpccg_SDAG_CODE
};

class charmMain : public CBase_charmMain {
public:
  charmMain(CkArgMsg* msg);
private:
  CProxy_charmHpccg array;
  charmMain_SDAG_CODE
};

#endif /*CHARM_HPCCG*/
