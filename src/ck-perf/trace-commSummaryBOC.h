#include "TraceCommSummary.decl.h"

extern CkGroupID traceCommSummaryGID;

class TraceCommSummaryInit : public Chare {
  public:
  TraceCommSummaryInit(CkArgMsg *m) {
    delete m;
    traceCommSummaryGID = CProxy_TraceCommSummaryBOC::ckNew();
    CProxy_TraceCommSummaryBOC commSummaryProxy(traceCommSummaryGID);
  }
  TraceCommSummaryInit(CkMigrateMessage *m):Chare(m) {}
};

class TraceCommSummaryBOC : public CBase_TraceCommSummaryBOC {
public:
  TraceCommSummaryBOC(void) {};
  TraceCommSummaryBOC(CkMigrateMessage *m) {};
};


