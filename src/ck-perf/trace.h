
#ifndef _TRACE_H
#define _TRACE_H

/*
 *   File: trace.h -- header file defines the super class of all trace modules
 *         written by Gengbin Zheng, gzheng@uiuc.edu 
 */ 

#if CMK_HAS_COUNTER_PAPI
#include <papi.h>
#endif

#include "envelope.h"

/**
 \defgroup CkPerf  Charm++ Trace Module
*/
/*@{*/

// An additional interface for summary data
extern "C" void traceClearEps();

// **CW** may not be necessary
extern "C" void traceEnableCCS();

extern double CmiTraceTimer();

extern int _sdagMsg, _sdagChare, _sdagEP;

/* CW Support for Thread Listener interface */
extern "C" void traceAddThreadListeners(CthThread tid, envelope *e);

// trace_in_charm means only do trace for Charm++ level, skip converse tracing
// Cpv traceOnPe controls if charm pe will generate trace logs (a default value)
// while traceOnPe flag in each trace module can also control independently if 
// tracing is wanted for each module
CkpvExtern(int, traceOnPe);

// eventID used for tracing of 'local' entry methods
CpvExtern(int, curPeEvent);

// A hack. We need to somehow tell the pup framework what size
// long_long is wrt PAPI.
#if CMK_HAS_COUNTER_PAPI
typedef long_long LONG_LONG_PAPI;
#else
typedef CMK_TYPEDEF_INT8 LONG_LONG_PAPI;
#endif

// Base class of all tracing strategies.
// 
class Trace {

protected:
    bool _traceOn;
  
  public:
    Trace(): _traceOn(false) {}
    virtual void setTraceOnPE(int flag) { _traceOn = (flag!=0); }
    virtual inline int traceOnPE() { return (int)_traceOn; }
    // turn trace on/off, note that charm will automatically call traceBegin()
    // at the beginning of every run unless the command line option "+traceoff"
    // is specified
    virtual void traceBegin() {}
    virtual void traceEnd() {}

    // for tracing comm thread only
    virtual void traceBeginComm() {}
    virtual void traceEndComm() {}
    virtual void traceBeginOnCommThread() {}   
    virtual void traceEndOnCommThread() {}
    virtual void traceCommSetMsgID(char *) {}
    virtual void traceGetMsgID(char *msg, int *pe, int *event) {
      (void)msg; (void)pe; (void)event;
    }
    virtual void traceSetMsgID(char *msg, int pe, int event) {
       (void)msg; (void)pe; (void)event;
     }

     // registers user event trace module returns int identifier
     virtual int traceRegisterUserEvent(const char* eventName, int e) {
       (void)eventName; (void)e;
       return 0;
     }
     // a user event has just occurred
     virtual void userEvent(int eventID) { (void)eventID; }
     // a pair of begin/end user event has just occurred
     virtual void userBracketEvent(int eventID, double bt, double et, int nestedID=0) {
       (void)eventID; (void)bt; (void)et; (void) nestedID;
     }
     // begin/end user event pair
     virtual void beginUserBracketEvent(int eventID, int nestedID=0) {
       (void)eventID; (void) nestedID;
     }
     virtual void endUserBracketEvent(int eventID, int nestedID=0) {
       (void)eventID; (void) nestedID;
     }

    //Register user stat trace module returns int identifier
    virtual int traceRegisterUserStat(const char* evt, int e) {
      return 0;
    }

    //update a user Stat with option to set user specified time
    virtual void updateStatPair(int e, double stat,double time) {}
    virtual void updateStat(int e, double stat) {}

     //interact with application
     virtual void beginAppWork() {}
     virtual void endAppWork() {}
     virtual void countNewChare() {}
     virtual void beginTuneOverhead() {}
     virtual void endTuneOverhead() {}

     // a user supplied integer value(likely a timestep)
     virtual void userSuppliedData(int e) { (void)e; }

     // a user supplied integer value(likely a timestep)
     virtual void userSuppliedNote(const char *note) { (void)note; }

     virtual void userSuppliedBracketedNote(const char *note, int eventID, double bt, double et) {
       (void)note; (void)eventID; (void)bt; (void)et;
     }

     // the current memory usage as a double
     virtual void memoryUsage(double currentMemUsage) { (void)currentMemUsage; }

     // creation of message(s)
     virtual void creation(envelope *, int epIdx, int num=1) {
       (void)epIdx; (void)num;
     }
     //epIdx is extracted from the envelope, num is always 1
     virtual void creation(char *) {}
     virtual void creationMulticast(envelope *, int epIdx, int num=1,
                      const int *pelist=NULL) {
       (void)epIdx; (void)num; (void)pelist;
     }
     virtual void creationDone(int num=1) { (void)num; }
     // ???
     virtual void messageRecv(void *env, int size) { (void)env; (void)size; }
     virtual void messageSend(void *env, int pe, int size) { (void)env; (void)pe; (void)size; }
     virtual void beginSDAGBlock(
       int event,   // event type defined in trace-common.h
       int msgType, // message type
       int ep,      // Charm++ entry point (will correspond to sts file)
       int srcPe,   // Which PE originated the call
       int ml,      // message size
       CmiObjId* idx)    // index
     {
       (void)event; (void)msgType; (void)ep; (void)srcPe; (void)ml; (void)idx;
     }
     virtual void endSDAGBlock(void) {}
     // **************************************************************
     // begin/end execution of a Charm++ entry point
     // NOTE: begin/endPack and begin/endUnpack can be called in between
     //       a beginExecute and its corresponding endExecute.
     virtual void beginExecute(envelope *, void *) {}
     virtual void beginExecute(char *) {}
     virtual void beginExecute(CmiObjId *tid) { (void)tid; }
     virtual void beginExecute(
       int event,   // event type defined in trace-common.h
       int msgType, // message type
       int ep,      // Charm++ entry point (will correspond to sts file)
       int srcPe,   // Which PE originated the call
       int ml,      // message size
       CmiObjId* idx,    // index
       void* obj)
    {
      (void)event; (void)msgType; (void)ep; (void)srcPe;
      (void)ml; (void)idx; (void)obj;
    }
    virtual void changeLastEntryTimestamp(double ts) { (void)ts; }
    virtual void endExecute(void) {}
    virtual void endExecute(char *) {}
    // begin/end idle time for this pe
    virtual void beginIdle(double curWallTime) { (void)curWallTime; }
    virtual void endIdle(double curWallTime) { (void)curWallTime; }
    // begin/end the process of packing a message (to send)
    virtual void beginPack(void) {}
    virtual void endPack(void) {}
    // begin/end the process of unpacking a message (can occur before calling
    // a entry point or during an entry point when 
    virtual void beginUnpack(void) {}
    virtual void endUnpack(void) {}
    // ???
    virtual void enqueue(envelope *) {}
    virtual void dequeue(envelope *) {}
    // begin/end of execution
    virtual void beginComputation(void) {}
    virtual void endComputation(void) {}
    // demarkation of a phase boundary
    virtual void endPhase() {}
    // clear all data collected for entry points
    virtual void traceClearEps() {}
    // enable CCS operations if supported on the trace module
    virtual void traceEnableCCS() {}
    // write the summary sts file for this trace
    virtual void traceWriteSts() {}
    // do any clean-up necessary for tracing
    virtual void traceClose() {}
    // flush log buffer immediately
    virtual void traceFlushLog() {}

    /* Memory tracing */
    virtual void malloc(void *where, int size, void **stack, int stackSize) {
      (void)where; (void)size; (void)stack; (void)stackSize;
    }
    virtual void free(void *where, int size) {
      (void)where; (void)size;
    }

    /* for implementing thread listeners */
    virtual void traceAddThreadListeners(CthThread tid, envelope *e) {
      (void)tid; (void)e;
    }

    virtual ~Trace() {}
};

#define ALLDO(x) for (int i=0; i<length(); i++) if (traces[i] && traces[i]->traceOnPE()) traces[i]->x
#define ALLREVERSEDO(x) for (int i=length()-1; i>=0; i--) if (traces[i]->traceOnPE()) traces[i]->x

/// Array of Traces modules,  every event raised will go through every Trace module.
class TraceArray {
private:
  CkVec<Trace *>  traces;
  int n;
  int cancel_beginIdle, cancel_endIdle;
public:
    TraceArray(): n(0) {}
    inline void addTrace(Trace *tr) { traces.push_back(tr); n++;}
    inline void setTraceOnPE(int flag) { for (int i=0; i<length(); i++) traces[i]->setTraceOnPE(flag); }
    // to allow traceCLose() to be called multiple times, remove trace module
    // from the array in each individual trace, and clean up (clearTrace)
    // after the loop.
    inline void removeTrace(Trace *tr) {    // remove a Trace from TraceArray
        int i;
        for (i=0; i<n; i++) if (tr == traces[i]) break;
        CmiAssert(i<n);
        traces[i] = NULL;
    }
    inline void clearTrace() {    // remove holes in TraceArray
	int len = traces.length();
	int removed = 0;
        for (int i=0; i<len; i++) {
          if (traces[i-removed] == NULL) { traces.remove(i-removed); removed++; }
        }
        n -= removed;
    }
    inline int length() const { return n; }

    inline void userEvent(int e) { ALLDO(userEvent(e));}
    inline void userBracketEvent(int e,double bt, double et, int nestedID=0) {ALLDO(userBracketEvent(e,bt,et,nestedID));}
    inline void beginUserBracketEvent(int e, int nestedID=0) {ALLDO(beginUserBracketEvent(e, nestedID));}
    inline void endUserBracketEvent(int e, int nestedID=0) {ALLDO(endUserBracketEvent(e, nestedID));}
    
    inline void beginAppWork() { ALLDO(beginAppWork());}
    inline void endAppWork() { ALLDO(endAppWork());}
    inline void countNewChare() { ALLDO(countNewChare());}
    inline void beginTuneOverhead() { ALLDO(beginTuneOverhead());}
    inline void endTuneOverhead() { ALLDO(endTuneOverhead());}

	inline void userSuppliedData(int d) { ALLDO(userSuppliedData(d));}

	inline void userSuppliedNote(const char *note) { ALLDO(userSuppliedNote(note));}
	inline void userSuppliedBracketedNote(const char *note, int eventID, double bt, double et) {ALLDO(userSuppliedBracketedNote(note, eventID, bt, et));}


	inline void memoryUsage(double memUsage) { ALLDO(memoryUsage(memUsage));}


    /* Creation needs to access _entryTable, so moved into trace-common.C */
    void creation(envelope *env, int ep, int num=1);
    inline void creation(char *msg){
        /* The check for whether the ep got traced is moved into each elem's
         * creation call as the ep could not be extracted here
         */
        ALLDO(creation(msg));
    }
    void creationMulticast(envelope *env, int ep, int num=1, const int *pelist=NULL);
    
    inline void creationDone(int num=1) { ALLDO(creationDone(num)); }
    inline void beginSDAGBlock(int event,int msgType,int ep,int srcPe, int mlen,CmiObjId *idx=NULL) {ALLDO(beginSDAGBlock(event, msgType, ep, srcPe, mlen,idx));}
    inline void endSDAGBlock(void) {ALLREVERSEDO(endExecute());}
    inline void beginExecute(envelope *env, void *obj) {ALLDO(beginExecute(env, obj));}
    inline void beginExecute(char *msg) {ALLDO(beginExecute(msg));}
    inline void beginExecute(CmiObjId *tid) {ALLDO(beginExecute(tid));}
    inline void beginExecute(int event,int msgType,int ep,int srcPe, int mlen,CmiObjId *idx=NULL, void *obj=NULL) {ALLDO(beginExecute(event, msgType, ep, srcPe, mlen,idx, obj));}
    inline void endExecute(void) {ALLREVERSEDO(endExecute());}
    inline void endExecute(char *msg) {ALLREVERSEDO(endExecute(msg));}
    inline void changeLastEntryTimestamp(double ts) {ALLDO(changeLastEntryTimestamp(ts));}
    inline void messageRecv(void *env, int size) {ALLDO(messageRecv(env, size));}
    inline void messageSend(void *env, int pe, int size) {ALLDO(messageSend(env, pe, size));}
    inline void beginPack(void) {ALLDO(beginPack());}
    inline void endPack(void) {ALLDO(endPack());}
    inline void beginUnpack(void) {ALLDO(beginUnpack());}
    inline void endUnpack(void) {ALLDO(endUnpack());}
    inline void enqueue(envelope *e) {ALLDO(enqueue(e));}
    inline void dequeue(envelope *e) {ALLDO(dequeue(e));}
    inline void beginComputation(void) {ALLDO(beginComputation());}
    inline void endComputation(void) {ALLDO(endComputation());}
    inline int traceRegisterUserEvent(const char*x, int evt) {
	  int eno = 0;
	  for (int i=0; i<length(); i++) {
	    if (traces[i]->traceOnPE() == 0) {
	      continue;
	    }
	    int e = traces[i]->traceRegisterUserEvent(x, evt);
	    if (e) eno = e;
          }
	  return eno;
    } 

//User stat functions for TraceArray
    inline int traceRegisterUserStat(const char*x, int evt) {
	  int eno = 0;
	  for (int i=0; i<length(); i++) {
	    if (traces[i]->traceOnPE() == 0) {
	      continue;
	    }
	    int e = traces[i]->traceRegisterUserStat(x, evt);
	    if (e) eno = e;
          }
	  return eno;
    }
    inline void updateStatPair(int e,double stat,double time) {ALLDO(updateStatPair(e,stat,time));}
    inline void updateStat(int e,double stat) {ALLDO(updateStat(e,stat));}

    inline void traceClearEps() {ALLDO(traceClearEps());}
    inline void traceEnableCCS() {ALLDO(traceEnableCCS());}
    inline void traceWriteSts() {ALLDO(traceWriteSts());}
    inline void traceClose() {ALLDO(traceClose()); clearTrace();}
    inline void traceFlushLog() {ALLDO(traceFlushLog());}
    
    // Tracing module registers *itself* for begin/end idle callbacks:
    inline void beginIdle(double curWallTime) {ALLDO(beginIdle(curWallTime));}
    inline void endIdle(double curWallTime) {ALLDO(endIdle(curWallTime));}
    void traceBegin();    
    void traceEnd();

    // for tracing comm thread only
    void traceBeginComm();
    void traceEndComm();
    void traceBeginOnCommThread();
    void traceEndOnCommThread();
    void traceCommSetMsgID(char *msg)  { ALLDO(traceCommSetMsgID(msg)); }
    void traceGetMsgID(char *msg, int *pe, int *event) { ALLDO(traceGetMsgID(msg, pe, event)); }
    void traceSetMsgID(char *msg, int pe, int event) { ALLDO(traceSetMsgID(msg, pe, event)); }

    /* Phase Demarcation */
    inline void endPhase() { ALLDO(endPhase()); }

    /* Memory tracing */
    inline void malloc(void *where, int size, void **stack, int stackSize){ ALLDO(malloc(where,size,stack,stackSize)); }
    inline void free(void *where, int size){ ALLDO(free(where, size)); }

    /* calls for thread listener registration for each trace module */
    inline void traceAddThreadListeners(CthThread tid, envelope *e) {
      ALLDO(traceAddThreadListeners(tid, e));
    }
};

CkpvExtern(TraceArray*, _traces);

#if CMK_TRACE_ENABLED
#  define _TRACE_ONLY(code) do{if(CpvAccess(traceOn) && CkpvAccess(_traces)->length()>0) { code; }} while(0)
#  define _TRACE_ALWAYS(code) do{ code; } while(0)
#else
#  define _TRACE_ONLY(code) /*empty*/
#  define _TRACE_ALWAYS(code) /*empty*/
#endif

extern "C" {
#include "conv-trace.h"
}

inline void _TRACE_USER_EVENT(int eventID)
{
  _TRACE_ONLY(CkpvAccess(_traces)->userEvent(eventID));
}
inline void _TRACE_USER_EVENT_BRACKET(int eventID, double bt, double et)
{
  _TRACE_ONLY(CkpvAccess(_traces)->userBracketEvent(eventID, bt, et));
}
inline void _TRACE_BEGIN_USER_EVENT_BRACKET(int eventID)
{
  _TRACE_ONLY(CkpvAccess(_traces)->beginUserBracketEvent(eventID));
}
inline void _TRACE_END_USER_EVENT_BRACKET(int eventID)
{
  _TRACE_ONLY(CkpvAccess(_traces)->endUserBracketEvent(eventID));
}
inline void _TRACE_BEGIN_APPWORK() { _TRACE_ONLY(CkpvAccess(_traces)->beginAppWork()); }
inline void _TRACE_END_APPWORK() { _TRACE_ONLY(CkpvAccess(_traces)->endAppWork()); }
inline void _TRACE_NEW_CHARE() { _TRACE_ONLY(CkpvAccess(_traces)->countNewChare()); }
inline void _TRACE_BEGIN_TUNEOVERHEAD()
{
  _TRACE_ONLY(CkpvAccess(_traces)->beginTuneOverhead());
}
inline void _TRACE_END_TUNEOVERHEAD()
{
  _TRACE_ONLY(CkpvAccess(_traces)->endTuneOverhead());
}
inline void _TRACE_CREATION_1(envelope* env)
{
  _TRACE_ONLY(CkpvAccess(_traces)->creation(env, env->getEpIdx()));
}
inline void _TRACE_CREATION_DETAILED(envelope* env, int ep)
{
  _TRACE_ONLY(CkpvAccess(_traces)->creation(env, ep));
}
inline void _TRACE_CREATION_N(envelope* env, int num)
{
  _TRACE_ONLY(CkpvAccess(_traces)->creation(env, env->getEpIdx(), num));
}
inline void _TRACE_CREATION_MULTICAST(envelope* env, int num, const int* pelist)
{
  _TRACE_ONLY(CkpvAccess(_traces)->creationMulticast(env, env->getEpIdx(), num, pelist));
}
inline void _TRACE_CREATION_DONE(int num)
{
  _TRACE_ONLY(CkpvAccess(_traces)->creationDone(num));
}
inline void _TRACE_BEGIN_SDAG(int event, int msgType, int ep, int srcPe, int ml,
                              CmiObjId* idx)
{
  _TRACE_ONLY(CkpvAccess(_traces)->beginSDAGBlock(event, msgType, ep, srcPe, ml, idx));
}
inline void _TRACE_END_SDAG() { _TRACE_ONLY(CkpvAccess(_traces)->endSDAGBlock()); }
inline void _TRACE_BEGIN_EXECUTE(envelope* env, void* obj)
{
  _TRACE_ONLY(CkpvAccess(_traces)->beginExecute(env, obj));
}
#if CMK_TRACE_ENABLED
inline void _TRACE_BEGIN_EXECUTE_DETAILED(int event, int msgType, int ep, int srcPe,
                                          int mLen, CmiObjId* idx, void* obj)
{
  _TRACE_ONLY(
      CkpvAccess(_traces)->beginExecute(event, msgType, ep, srcPe, mLen, idx, obj));
}
#else
// don't evaluate [local] when trace is turned off
#define _TRACE_BEGIN_EXECUTE_DETAILED(event, msgType, ep, srcPe, mLen, idx, obj) ((void)0)
#endif
inline void _TRACE_END_EXECUTE() { _TRACE_ONLY(CkpvAccess(_traces)->endExecute()); }
// The array element whose execution the next beginExecute belongs to: the
// group index of its location manager (the load database's object manager
// id) and its 64-bit id within it (CkLocRec::getID, the database's object
// id); collection -1 when no element is running. Set around the element's
// entry by CkLocRec::invokeEntry, around a threaded entry's resumed
// segments by the trace thread listener, and around a TCharm thread's
// segments by TCharm itself. Trace modules that attribute time to objects
// read the pair in beginExecute.
CkpvExtern(int, _traceObjColl);
CkpvExtern(CmiUInt8, _traceObjId);
#if CMK_TRACE_ENABLED
inline void _TRACE_SET_OBJECT(int coll, CmiUInt8 id) {
  CkpvAccess(_traceObjColl) = coll;
  CkpvAccess(_traceObjId) = id;
}
inline void _TRACE_CLEAR_OBJECT() { CkpvAccess(_traceObjColl) = -1; }
inline int _TRACE_OBJECT_COLL() { return CkpvAccess(_traceObjColl); }
inline CmiUInt8 _TRACE_OBJECT_ID() { return CkpvAccess(_traceObjId); }
#else
#define _TRACE_SET_OBJECT(coll, id) ((void)0)
#define _TRACE_CLEAR_OBJECT() ((void)0)
#define _TRACE_OBJECT_COLL() (-1)
#define _TRACE_OBJECT_ID() ((CmiUInt8)0)
#endif
extern int _threadEP;  // the entry index trace events use for thread execution
inline void _TRACE_MESSAGE_RECV(void* env, int size)
{
  _TRACE_ONLY(CkpvAccess(_traces)->messageRecv(env, size));
}
inline void _TRACE_MESSAGE_SEND(void* env, int pe, int size)
{
  _TRACE_ONLY(CkpvAccess(_traces)->messageSend(env, pe, size));
}
inline void _TRACE_BEGIN_PACK() { _TRACE_ONLY(CkpvAccess(_traces)->beginPack()); }
inline void _TRACE_END_PACK() { _TRACE_ONLY(CkpvAccess(_traces)->endPack()); }
inline void _TRACE_BEGIN_UNPACK() { _TRACE_ONLY(CkpvAccess(_traces)->beginUnpack()); }
inline void _TRACE_END_UNPACK() { _TRACE_ONLY(CkpvAccess(_traces)->endUnpack()); }
inline void _TRACE_BEGIN_COMPUTATION()
{
  _TRACE_ALWAYS(CkpvAccess(_traces)->beginComputation());
}
inline void _TRACE_END_COMPUTATION()
{
  _TRACE_ALWAYS(CkpvAccess(_traces)->endComputation());
}
inline void _TRACE_ENQUEUE(envelope* env)
{
  _TRACE_ONLY(CkpvAccess(_traces)->enqueue(env));
}
inline void _TRACE_DEQUEUE(envelope* env)
{
  _TRACE_ONLY(CkpvAccess(_traces)->dequeue(env));
}

inline void _TRACE_END_PHASE() { _TRACE_ONLY(CkpvAccess(_traces)->endPhase()); }

/* Memory tracing */
inline void _TRACE_MALLOC(void* where, int size, void** stack, int stackSize)
{
  _TRACE_ONLY(CkpvAccess(_traces)->malloc(where, size, stack, stackSize));
}
inline void _TRACE_FREE(void* where, int size)
{
  _TRACE_ONLY(CkpvAccess(_traces)->free(where, size));
}

#endif


/*@}*/
