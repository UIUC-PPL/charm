#ifndef _COMMSUMMARY_H
#define _COMMSUMMARY_H

#include "envelope.h"
#include "register.h"
#include "trace-common.h"
#include "trace.h"

class TraceCommSummary : public Trace
{
private:
  std::vector<uint64_t> selfCount = {0}, selfBytes = {0}, localCount = {0},
                        localBytes = {0}, remoteCount = {0}, remoteBytes = {0};
  const int myPe, myNode;

public:
  TraceCommSummary(char** argv);

  void beginExecute(envelope* e, void* /*unused*/) override
  {
    // no envelope means thread execution
    if (e == nullptr)
    {
      beginExecute(-1, -1, -1, myPe, 0);
    }
    else
    {
      beginExecute(-1, -1, -1, e->getSrcPe(), e->getTotalsize());
    }
  }

  void beginExecute(char* /*unused*/) override
  {
    beginExecute(-1, -1, -1, myPe, 0);
  }

  void beginExecute(CmiObjId* /* unused */) override
  {
    beginExecute(-1, -1, -1, myPe, 0);
  }

  void beginExecute(int event,                // event type defined in trace-common.h
                    int msgType,              // message type
                    int ep,                   // Charm++ entry point id
                    int srcPe,                // Which PE originated the call
                    int ml,                   // message size
                    CmiObjId* idx = nullptr,  // index
                    void* obj = nullptr) override;

  void endPhase() override;

  // do any clean-up necessary for tracing
  void traceClose() override;
};

#endif
