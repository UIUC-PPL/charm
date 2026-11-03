/**
 * \addtogroup CkPerf
 */
/*@{*/


#ifndef _TRACE_UTILIZATION_H
#define _TRACE_UTILIZATION_H

#include <stdio.h>
#include <errno.h>
#include <deque>
#include <map>

#include "charm++.h"


#include "trace.h"
#include "envelope.h"
#include "register.h"
#include "trace-common.h"
#include "ckcallback-ccs.h"

#include "TraceUtilization.decl.h"

#define INVALIDEP     -2
#define TRACEON_EP    -3
#define NUM_DUMMY_EPS 9


// initial bin size, time in seconds
#define  BIN_PER_SEC	1000
#define  BIN_SIZE	0.001
#define NUM_BINS      32768


/** Define the types used in the gathering of sum detail statistics for use with CCS */
#define numBins_T int
#define numProcs_T int
#define entriesInBin_T short
#define ep_T short
#define utilization_T unsigned char
#define other_EP 10000


/* readonly */ extern CProxy_TraceUtilizationBOC traceUtilizationGroupProxy;
/* readonly */ extern CProxy_TraceObjLoadsBOC traceObjLoadsGroupProxy;

// This function has unused arguments to match the type of
// CcdCondFn, which CcdCallOnConditionKeep takes
void collectUtilizationData(void *);



/** A main chare that can create the BOC/group */
class TraceUtilizationInit : public Chare {
 public:
  TraceUtilizationInit(CkArgMsg *m) {
    delete m;
    CkPrintf("[%d] TraceUtilizationInit creating traceUtilizationGroupProxy", CkMyPe());
    fflush(stdout);
   
    traceUtilizationGroupProxy = CProxy_TraceUtilizationBOC::ckNew();
    traceObjLoadsGroupProxy = CProxy_TraceObjLoadsBOC::ckNew();
    
    CkPrintf("Trace Summary now listening in for CCS Client\n");
    CcsRegisterHandler("CkPerfSumDetail compressed", CkCallback(CkIndex_TraceUtilizationBOC::ccsRequestSumDetailCompressed(NULL), traceUtilizationGroupProxy[0])); 
    // The entry-method names the stream's indices refer to, as sts text,
    // so a client needs no copy of the .util.sts file.
    CcsRegisterHandler("CkPerfSumDetail sts", CkCallback(CkIndex_TraceUtilizationBOC::ccsRequestSts(NULL), traceUtilizationGroupProxy[0]));
    // Per-object cumulative execution time on every PE, from the same
    // begin/endExecute events, independent of the load-balancing
    // database and its instrumentation switch (same reply layout and
    // object ids as LBManager's "LBObjLoads").
    CcsRegisterHandler("CkPerfObjLoads", CkCallback(CkIndex_TraceObjLoadsBOC::ccsRequestObjLoads(NULL), traceObjLoadsGroupProxy[0]));
    
    CkPrintf("[%d] Setting up periodic startCollectData callback\n", CkMyPe());
    CcdCallOnConditionKeep(CcdPERIODIC_1second, collectUtilizationData, (void *)NULL);

  }
};






/** 
    A class that reads/writes a buffer out of different types of data.

    This class exists because I need to get references to parts of the buffer 
    that have already been used so that I can increment counters inside the buffer.
*/

class compressedBuffer {
 public:
  char* buf;
  int pos; ///<< byte position just beyond the previously read/written data

  compressedBuffer(){
    buf = NULL;
    pos = 0;
  }

  compressedBuffer(int bytes){
    buf = (char*)malloc(bytes);
    pos = 0;
  }

  compressedBuffer(void *buffer){
    buf = (char*)buffer;
    pos = 0;
  }
  
  void init(void *buffer){
    buf = (char*)buffer;
    pos = 0;
  }
  
  inline void * currentPtr(){
    return (void*)(buf+pos);
  }
  
  template <typename T>
    T read(int offset){
    // to resolve unaligned writes causing bus errors, need memcpy
    T v;
    memcpy(&v, buf+offset, sizeof(T));
    return v;
  }
  
  template <typename T>
    void write(T v, int offset){
    T v2 = v; // on stack
    // to resolve unaligned writes causing bus errors, need memcpy
    memcpy(buf+offset, &v2, sizeof(T));
  }
    
  template <typename T>
    void increment(int offset){
    T temp;
    temp = read<T>(offset);
    temp ++;
    write<T>(temp, offset);
  }

  template <typename T>
    void accumulate(T v, int offset){
    T temp;
    temp = read<T>(offset);
    temp += v;
    write<T>(temp, offset);
  }
  
  template <typename T>
    int push(T v){
    int oldpos = pos;
    write<T>(v, pos);
    pos += sizeof(T);
    return oldpos;
  }
  
  template <typename T>
    T pop(){
    T temp = read<T>(pos);
    pos += sizeof(T);
    return temp;
  }

  template <typename T>
    T peek(){
    T temp = read<T>(pos);
    return temp;
  }

  template <typename T0, typename T>
    T peekSecond(){
    T temp;
    memcpy(&temp, buf+pos+sizeof(T0), sizeof(T));
    return temp;
  }

  int datalength(){
    return pos;
  }
     
  void * buffer(){
    return (void*) buf;
  }  

  void freeBuf(){
    free(buf);
  }

  ~compressedBuffer(){
    // don't free the buf because the user my have supplied the buffer
  }
    
};



compressedBuffer compressAvailableNewSumDetail(int max=10000);
void mergeCompressedBin(compressedBuffer *srcBufferArray, int numSrcBuf, int *numProcsRepresentedInMessage, int totalProcsAcrossAllMessages, compressedBuffer &destBuffer);
//void printSumDetailInfo(int desiredBinsToSend);
CkReductionMsg *sumDetailCompressedReduction(int nMsg,CkReductionMsg **msgs);
void printCompressedBuf(compressedBuffer b);
compressedBuffer fakeCompressedMessage();
compressedBuffer emptyCompressedBuffer();
void sanityCheckCompressedBuf(compressedBuffer b);
bool isCompressedBufferSane(compressedBuffer b);
double averageUtilizationInBuffer(compressedBuffer b);





class TraceUtilization : public Trace {
 public:
  int execEp; // the currently executing EP
  int execDepth; // nesting of beginExecute/endExecute pairs (array delivery nests one inside another)
  unsigned int epInfoSize;
  double start; // the start time for the currently executing EP

  double *cpuTime;     // NUM_BINS*epInfoSize
  int lastBinUsed;
  unsigned int numBinsSent;
  unsigned int previouslySentBins;

  // Per-object loads for the CCS handler "CkPerfObjLoads": cumulative
  // execution time of every array element that ran on this PE, keyed by
  // its collection and 64-bit id (_TRACE_OBJECT_COLL / _TRACE_OBJECT_ID
  // at beginExecute: the load database's object manager id and object
  // id, so "LBObjLoads" reports the same keys).
  // Nothing is read from the element itself, which for a constructor
  // entry is not constructed yet. An element not seen for OBJ_EXPIRE
  // seconds is dropped (expireObjLoads, once a second), so the table
  // stays the size of the recently active object set; a client that
  // takes differences between snapshots sees a dropped-and-returned
  // element start from zero, which is the right value.
  //
  // The time is measured between the element's own begin and end, at
  // whatever nesting depth they occur: a broadcast to an array is one
  // group entry whose delivery loop runs every local element inside it,
  // so the outer pair (which the bins above use) spans all of them.
  // Time of nested element executions ([inline] sends) is excluded from
  // the enclosing element's. A threaded entry's resumed segments and a
  // TCharm thread's segments arrive with the element's id set by the
  // thread listener and by TCharm, so they count for the element too.
  struct ObjLoad { double wall = 0; double lastSeen = 0; };
  typedef std::pair<int, CmiUInt8> ObjKey;   // collection, id
  static constexpr double OBJ_EXPIRE = 60.0;
  static const int OBJ_MAX_DEPTH = 16;
  std::map<ObjKey, ObjLoad> objLoads;
  struct ObjFrame { ObjKey key; double start, nested; bool valid; };
  ObjFrame objFrames[OBJ_MAX_DEPTH + 1];   // indexed by execDepth
  double idleTotal, idleStart;   // from the scheduler's idle callbacks


  TraceUtilization() {
    execEp = TRACEON_EP;
    execDepth = 0;
    cpuTime = NULL;
    lastBinUsed = -1;
    numBinsSent = 0;
    for (int i = 0; i <= OBJ_MAX_DEPTH; i++) objFrames[i].valid = false;
    idleTotal = idleStart = 0.0;
  }

  /// Append this PE's record for "CkPerfObjLoads" to buf, the layout and
  /// keys of LBManager's "LBObjLoads" (there the third field is the wall
  /// time since the instrumentation was switched on):
  ///   int32 pe, double wallNow, double elapsed, double idleTotal, int32 n,
  ///   n x { uint64 objId, int32 collection, double objWall, uint8 1 }
  /// elapsed is the trace's clock (seconds since it started), so
  /// elapsed - idleTotal is the PE's busy time.
  void snapshotObjLoads(std::vector<char> &buf);
  /// Drop objects not seen for OBJ_EXPIRE seconds.
  void expireObjLoads();


  /// Initialize memory after the number of EPs has been determined
  void initMem(){
    int _numEntries=_entryTable.size();
    epInfoSize = _numEntries + NUM_DUMMY_EPS + 1; // keep a spare EP
    //    CkPrintf("allocating cpuTime[%d]\n", (int) (NUM_BINS*epInfoSize));
    cpuTime = new double[NUM_BINS*epInfoSize];
    _MEMCHECK(cpuTime);

    if(CkMyPe() == 0)
      writeSts();

  }

  void writeSts(void);

  void creation(envelope *e, int epIdx, int num=1) {}

  void beginExecute(envelope *e, void *obj);
  void beginExecute(CmiObjId  *tid);
  void beginExecute(int event,int msgType,int ep,int srcPe, int mlen=0, CmiObjId *idx=NULL, void *obj=NULL);
  void endExecute(void);
  void beginIdle(double currT) { idleStart = currT; }
  void endIdle(double currT) { idleTotal += currT - idleStart; }
  void beginPack(void){}
  void endPack(void) {}
  void beginUnpack(void) {}
  void endUnpack(void) {}
  void beginComputation(void) {  initMem(); }
  void endComputation(void) {}
  void traceClearEps() {}
  void traceWriteSts() {}
  void traceClose() {}

  void addEventType(int eventType);
  
  int cpuTimeEntriesAvailable() const { return lastBinUsed+1; }
  int cpuTimeEntriesSentSoFar() const { return numBinsSent; }
  void incrementNumCpuTimeEntriesSent(int n) { numBinsSent += n; }


  double sumUtilization(int startBin, int endBin);


  void updateCpuTime(int epIdx, double startTime, double endTime){
    
    //    CkPrintf("updateCpuTime(startTime=%lf endTime=%lf)\n", startTime, endTime);
    
    if (epIdx >= epInfoSize) {
      CkPrintf("WARNING: epIdx=%d >=  epInfoSize=%d\n", (int)epIdx, (int)epInfoSize );
      return;
    }
    
    int startingBinIdx = (int)(startTime/BIN_SIZE);
    int endingBinIdx = (int)(endTime/BIN_SIZE);
    
    if (startingBinIdx == endingBinIdx) {
      addToCPUtime(startingBinIdx, epIdx, endTime - startTime);
    } else if (startingBinIdx < endingBinIdx) { // EP spans intervals
      addToCPUtime(startingBinIdx, epIdx, (startingBinIdx+1)*BIN_SIZE - startTime);
      while(++startingBinIdx < endingBinIdx)
	addToCPUtime(startingBinIdx, epIdx, BIN_SIZE);
      addToCPUtime(endingBinIdx, epIdx, endTime - endingBinIdx*BIN_SIZE);
    } 
  }


  /// Zero out all entries from (lastBinUsed+1) up to and including interval.
  inline void zeroIfNecessary(unsigned int interval){
    for(unsigned int i=lastBinUsed+1; i<= interval; i++){
      // zero all eps for this bin
      for(unsigned int j=0;j<epInfoSize;j++){
	cpuTime[(i%NUM_BINS)*epInfoSize+j] = 0.0;
      }
    }
    lastBinUsed = interval;
  }
    
  UInt getEpInfoSize() { return epInfoSize; }

  /// for Summary-Detail
  inline double getCPUtime(unsigned int interval, unsigned int ep) {
    CkAssert(ep < epInfoSize);
    if(cpuTime != NULL && interval <= lastBinUsed)
      return cpuTime[(interval%NUM_BINS)*epInfoSize+ep]; 
    else {
      CkPrintf("getCPUtime called with invalid options: cpuTime=%p lastBinUsed=%d interval=%d ep=%d\n", (void *)cpuTime, lastBinUsed, (int)interval, (int)ep);
      return 0.0;
    }
  }

  inline void addToCPUtime(unsigned int interval, unsigned int ep, double val){
    //    CkAssert(ep < epInfoSize);
    zeroIfNecessary(interval);
    //    CkPrintf("addToCPUtime interval=%d\n", (int)interval);
    cpuTime[(interval%NUM_BINS)*epInfoSize+ep] += val;
  }
   

  inline double getUtilization(int interval, int ep){
    return getCPUtime(interval, ep) * 100.0 * (double)BIN_PER_SEC; 
  }


  compressedBuffer compressNRecentSumDetail(int desiredBinsToSend);


};






class TraceUtilizationBOC : public CBase_TraceUtilizationBOC {
  
  std::deque<CkReductionMsg *> storedSumDetailResults;
  
 public:
  TraceUtilizationBOC() {}
  TraceUtilizationBOC(CkMigrateMessage* msg) {}
  ~TraceUtilizationBOC() {}
 

  /// Entry methods:
  void ccsRequestSumDetailCompressed(CkCcsRequestMsg *m);
  void ccsRequestSts(CkCcsRequestMsg *m);
  void collectSumDetailData();
  void sumDetailDataCollected(CkReductionMsg *);

};


/// "CkPerfObjLoads": PE 0 keeps the reply tokens, broadcasts
/// collectObjLoads, and answers every waiting request from the
/// concatenated reduction. Its own group (see the .ci).
class TraceObjLoadsBOC : public CBase_TraceObjLoadsBOC {
 public:
  TraceObjLoadsBOC() {}
  TraceObjLoadsBOC(CkMigrateMessage *m) : CBase_TraceObjLoadsBOC(m) {}
  void ccsRequestObjLoads(CkCcsRequestMsg *m);
  void collectObjLoads();
  void objLoadsCollected(CkReductionMsg *);
 private:
  // Used on PE 0 only: the CCS callback and the reduction both land there.
  std::deque<CcsDelayedReply> objLoadsReplies;
  bool objLoadsCollecting = false;
};





#endif

/*@}*/
