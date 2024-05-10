#include "test.decl.h"

/* readonly */ CProxy_Main main_proxy;
/* readonly */ CProxy_Array1 array1_proxy;

class Main : public CBase_Main {
public:
  Main(CkArgMsg* args) {
    array1_proxy = CProxy_Array1::ckNew(4);
    array1_proxy.foo();
  }

  void done() {
    CkExit();
  }
};

class Array1 : public CBase_Array1 {
  Array1_SDAG_CODE

public:
  int iter;
  CkCallback cb;

  Array1() {
    iter = 0;
    cb = CkCallback(CkIndex_Array1::recv(), thisProxy[thisIndex]);
  }

  void send() {
    cb.setRefnum(iter);
    cb.send();
  }
};

#include "test.def.h"
