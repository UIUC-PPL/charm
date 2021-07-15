#ifndef _CONV_TRACE_H
#define _CONV_TRACE_H

#include "converse.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 
 * These functions are called from Converse, and should be provided C binding
 * by the tracing strategies.
 */

void traceInit(char **argv);
void traceCharmInit(char **argv);	/* init trace module in ck */
void traceMessageRecv(char *msg, int pe);
void traceBeginIdle(void);
void traceEndIdle(void);
void traceResume(int,int,int,CmiObjId *);
void traceSuspend(void);
void traceAwaken(CthThread t);
void traceUserEvent(int);
void beginAppWork(void);
void endAppWork(void);
void beginTuneOverhead(void);
void endTuneOverhead(void);
void traceUserBracketEvent(int, double, double);
void traceUserBracketEventNestedID(int, double, double, int nestedID);
void traceBeginUserBracketEvent(int eventID);
void traceBeginUserBracketEventNestedID(int eventID, int nestedID);
void traceEndUserBracketEvent(int eventID);
void traceEndUserBracketEventNestedID(int eventID, int nestedID);
void traceUserSuppliedData(int);
void traceUserSuppliedBracketedNote(const char *note, int eventID, double bt, double et);
void traceUserSuppliedNote(const char*);
void traceMemoryUsage(void);
int  traceRegisterUserEvent(const char*, int e
#ifdef __cplusplus
=-1
#endif
);

/*Declarations for user stat tracing functions*/
int traceRegisterUserStat(const char *evt, int e);
void updateStatPair(int e, double stat, double time);
void updateStat(int e, double stat);

#if CMK_SMP_TRACE_COMMTHREAD
int  traceBeginCommOp(char *msg);
void traceEndCommOp(char *msg);
void traceSendMsgComm(char *msg);
void traceCommSetMsgID(char *msg);
#endif
void traceChangeLastTimestamp(double ts);
void traceGetMsgID(char *msg, int *pe, int *event);
void traceSetMsgID(char *msg, int pe, int event);

/* Support for machine layers to register their user events to projections */
void registerMachineUserEventsFunction(void (*eventRegistrationFunc)(void));

void traceClose(void);
void traceCharmClose(void);          /* close trace in ck */
void traceBegin(void);
void traceEnd(void);
void traceBeginComm(void);
void traceEndComm(void);
void traceWriteSts(void);
void traceFlushLog(void);

#if CMK_TRACE_ENABLED
CpvExtern(int, traceOn);
#define traceIsOn()  (CpvAccess(traceOn))
#else 
#define traceIsOn()  0
#endif

int  traceAvailable(void);

/* Comm thread tracing */
#if CMK_SMP_TRACE_COMMTHREAD
#define  TRACE_COMM_CREATION(time, msg)   \
                    if (traceBeginCommOp(msg)) {   \
                      traceChangeLastTimestamp(time);    \
                      traceSendMsgComm(msg);   \
                      traceEndCommOp(msg);    \
                    }

#define  TRACE_COMM_CONTROL_CREATION(time0, time1, time2, msg)   \
                    if (traceBeginCommOp(msg)) {   \
                      traceChangeLastTimestamp(time0);    \
                      traceSendMsgComm(msg);   \
                      traceChangeLastTimestamp(time1);    \
                      traceEndCommOp(msg);    \
                      traceChangeLastTimestamp(time2);    \
                    }

#define TRACE_COMM_SET_MSGID(msg, pe, event)  traceSetMsgID(msg, pe, event)
#define TRACE_COMM_GET_MSGID(msg, pe, event)  traceGetMsgID(msg, pe, event)
#define TRACE_COMM_SET_COMM_MSGID(msg)  traceCommSetMsgID(msg)
#else
#define TRACE_COMM_CREATION(time, msg)
#define TRACE_COMM_CONTROL_CREATION(time0, time1, time2, msg)
#define TRACE_COMM_SET_MSGID(msg, pe, event) 
#define TRACE_COMM_GET_MSGID(msg, pe, event) 
#define TRACE_COMM_SET_COMM_MSGID(msg)
#endif

#ifdef __cplusplus
}
#endif

#endif
