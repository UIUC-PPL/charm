/**
 Dummy versions of the Charm++/C++ routines in trace-common.C,
 used when you're *only* linking with Converse.

 FIXME: this file should be eliminated via a pluggable
 tracing architecture (projector?).
 
 Orion Sky Lawlor, olawlor@acm.org, 2003/3/27
*/
#include <stdlib.h>
#include "conv-trace.h"

CpvDeclare(int, traceOn); /* For threads.C */
#if CMK_TRACE_ENABLED && CMK_PROJECTOR
CpvExtern(int, _traceCoreOn);   /* For cursed projector core */
#endif
int _threadEP=-123; /* for charmProjections.C */

void traceInit(char **argv) {
  CpvInitialize(int, traceOn);
  CpvAccess(traceOn)=0;
#if CMK_TRACE_ENABLED && CMK_PROJECTOR
  CpvInitialize(int, _traceCoreOn); 
  CpvAccess(_traceCoreOn)=0; 
  /* initTraceCore(argv); */
#endif
}
void traceMessageRecv(char *msg, int pe) {}
void traceBeginIdle(void) {}
void traceEndIdle(void) {}
void traceResume(int event, int srcPE, int ep, CmiObjId *t) {}
void traceSuspend(void) {}
void traceAwaken(CthThread t) {}
void traceUserEvent(int i) {}
void traceUserBracketEvent(int a, double b, double c) {}
int traceRegisterUserEvent(const char* e, int f) { return -1; }
// User stat functions for converse.
int traceRegisterUserStat(const char* evt, int e) { return -1; }
void updateStatPair(int e, double stat, double time) {}
void updateStat(int e, double stat) {}

void traceClose(void) {}
void traceCharmClose(void) {}
void traceBegin(void) {}
void traceEnd(void) {}
void traceWriteSts(void) {}
void traceFlushLog(void) {}
int  traceAvailable(void) {return 0;}

void traceUserSuppliedNote(const char *note) {}
void traceUserSuppliedData(int data) {}
void traceUserSuppliedBracketedNote(const char *note, int eventID, double bt, double et) {}
void traceMemoryUsage(void) {}

#if CMK_SMP_TRACE_COMMTHREAD
int traceBeginCommOp(char *msg) { return 0; }
void traceEndCommOp(char *msg) {}
void traceSendMsgComm(char *msg) {}
void traceChangeLastTimestamp(double ts) {}
void registerMachineUserEventsFunction(void (*eventRegistrationFunc)()) {}
void traceCommSetMsgID(char *msg) {}
double TraceTimerCommon(void){return CmiWallTimer();}
#endif

void traceGetMsgID(char *msg, int *pe, int *event) {}
void traceSetMsgID(char *msg, int pe, int event) {}

/* This routine, included in Charm++ programs from init.C, needs to be present in converse as well.
   Here is a place where it gets included only in converse, and not in Charm++ (thus not generating conflicts). */
void EmergencyExit(void) {}
void CpdEndConditionalDeliver_master(void) {}


int _lb_version;

