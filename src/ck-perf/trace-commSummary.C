#include "charm++.h"
#include "charm.h"
#include "middle-conv.h"
#include "trace-commSummary.h"
#include "trace-commSummaryBOC.h"
#include <fstream>

CkpvStaticDeclare(TraceCommSummary*, _trace);

CkGroupID traceCommSummaryGID;

void _createTracecommSummary(char** argv)
{
  CkpvInitialize(TraceCommSummary*, _trace);
  CkpvAccess(_trace) = new TraceCommSummary(argv);
  CkpvAccess(_traces)->addTrace(CkpvAccess(_trace));
}

TraceCommSummary::TraceCommSummary(char** argv) : myPe(CkMyPe()), myNode(CkMyNode()) {}

void TraceCommSummary::beginExecute(int event, int msgType, int ep, int srcPe, int mlen,
                                    CmiObjId* idx, void* obj)
{
  if (srcPe == myPe)
  {
    selfCount.back()++;
    selfBytes.back() += mlen;
  }
  else if (CkNodeOf(srcPe) == myNode)
  {
    localCount.back()++;
    localBytes.back() += mlen;
  }
  else
  {
    remoteCount.back()++;
    remoteBytes.back() += mlen;
  }
}

void TraceCommSummary::endPhase()
{
  selfCount.push_back(0);
  selfBytes.push_back(0);
  localCount.push_back(0);
  localBytes.push_back(0);
  remoteCount.push_back(0);
  remoteBytes.push_back(0);
}

void TraceCommSummary::traceClose(void)
{
  CkpvAccess(_trace)->endComputation();
  // remove myself from traceArray so that no tracing will be called.
  CkpvAccess(_traces)->removeTrace(this);

  std::ofstream f(std::string(CkpvAccess(traceRoot)) + "." + std::to_string(myPe) +
                  ".csumm");
  f << "PE Phase selfCount selfBytes localCount localBytes remoteCount remoteBytes\n";
  for (int i = 0; i < selfCount.size(); i++)
  {
    f << myPe << " " << i << " " << selfCount[i] << " " << selfBytes[i] << " "
      << localCount[i] << " " << localBytes[i] << " " << remoteCount[i] << " "
      << remoteBytes[i] << " "
      << "\n";
  }
  f.close();
}

extern "C" void traceCommSummaryExitFunction() { CkContinueExit(); }

// Initialization of the parallel trace module.
void initTraceCommSummaryBOC()
{
  if (CkMyRank() == 0)
  {
    registerExitFn(traceCommSummaryExitFunction);
  }
}

#include "TraceCommSummary.def.h"
