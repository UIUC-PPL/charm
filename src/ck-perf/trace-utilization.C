/**
 * \addtogroup CkPerf
*/
/*@{*/

#include "trace-utilization.h"


/* readonly */ CProxy_TraceUtilizationBOC traceUtilizationGroupProxy;
/* readonly */ CProxy_TraceObjLoadsBOC traceObjLoadsGroupProxy;


/// A reduction type for merging compressed sum detail data
CkReduction::reducerType sumDetailCompressedReducer;


// This function has unused arguments to match the type of
// CcdCondFn, which CcdCallOnConditionKeep takes
void collectUtilizationData(void *ignored) {
  traceUtilizationGroupProxy.collectSumDetailData();
}


CkpvStaticDeclare(TraceUtilization*, _trace);

/// Registered once per process (initnode): _createTraceutilization runs on
/// every PE, and concurrent addReducer calls from several PE threads in one
/// process can lose an entry and shift every later reducer index.
void registerSumDetailCompressedReducer(void) {
  sumDetailCompressedReducer=CkReduction::addReducer(sumDetailCompressedReduction, false, "sumDetailCompressedReduction");
}

/**
  For each TraceFoo module, _createTraceFoo() must be defined.
  This function is called in _createTraces() generated in moduleInit.C
*/
void _createTraceutilization(char **argv)
{
  //  CkPrintf("[%d] _createTraceutilization\n", CkMyPe());

  CkAssert(sizeof(short) == 2);

  CkpvInitialize(TraceUtilization*, _trace);
  CkpvAccess(_trace) = new TraceUtilization();
  CkpvAccess(_traces)->addTrace(CkpvAccess(_trace));

}



void TraceUtilization::beginExecute(CmiObjId *tid)
{
  beginExecute(-1,-1,_threadEP,-1);
}

void TraceUtilization::beginExecute(envelope *e, void *obj)
{
  // no message means thread execution
  if (e==NULL) {
    beginExecute(-1,-1,_threadEP,-1);
  }
  else {
    beginExecute(-1,-1,e->getEpIdx(),-1);
  }  
}

void TraceUtilization::beginExecute(int event,int msgType,int ep,int srcPe, int mlen, CmiObjId *idx, void *obj)
{
  // Array delivery traces twice: _invokeEntry in ck.C and the detailed
  // begin in cklocation.C, one inside the other. Count the nesting, as
  // trace-summary does, and attribute the time to the outer pair.
  execDepth++;
  if (execDepth <= OBJ_MAX_DEPTH) {
    ObjFrame &f = objFrames[execDepth];
    // The element, set by whoever begins its execution (see
    // _TRACE_SET_OBJECT); collection -1 for groups, chares, untraced work.
    f.key = ObjKey(_TRACE_OBJECT_COLL(), _TRACE_OBJECT_ID());
    f.valid = (f.key.first >= 0);
    if (f.valid) {
      f.start = TraceTimer();
      f.nested = 0.0;
    }
  }
  if (execDepth > 1) return;
  if (execEp != INVALIDEP && execEp != TRACEON_EP) {
    TRACE_WARN("Warning: TraceUtilization two consecutive BEGIN_PROCESSING!\n");
    return;
  }
  
  execEp=ep;
  start = TraceTimer();
}


void TraceUtilization::endExecute(void)
{
  if (execDepth > 0 && execDepth <= OBJ_MAX_DEPTH) {
    ObjFrame &f = objFrames[execDepth];
    if (f.valid) {
      double now = TraceTimer(), span = now - f.start;
      ObjLoad &o = objLoads[f.key];
      o.wall += span - f.nested;
      o.lastSeen = now;
      f.valid = false;
      if (execDepth > 1 && objFrames[execDepth - 1].valid) objFrames[execDepth - 1].nested += span;
    }
  }
  if (execDepth > 0) execDepth--;
  if (execDepth > 0) return;

  if (execEp == TRACEON_EP) {
    // if trace just got turned on, then one expects to see this
    // END_PROCESSING event without seeing a preceeding BEGIN_PROCESSING
    execEp = INVALIDEP;
    return;
  }

  double endTime = TraceTimer();
 
  updateCpuTime(execEp, start, endTime);

  execEp = INVALIDEP;
}


void TraceUtilization::expireObjLoads()
{
  double now = TraceTimer();
  for (auto it = objLoads.begin(); it != objLoads.end();) {
    if (now - it->second.lastSeen > OBJ_EXPIRE)
      it = objLoads.erase(it);
    else
      ++it;
  }
}

void TraceUtilization::snapshotObjLoads(std::vector<char> &buf)
{
  expireObjLoads();
  auto put = [&buf](const void *p, size_t n) {
    buf.insert(buf.end(), (const char *)p, (const char *)p + n);
  };
  int pe = CkMyPe(), n = (int)objLoads.size();
  double now = CkWallTimer(), elapsed = TraceTimer();
  put(&pe, sizeof(int));
  put(&now, sizeof(double));
  put(&elapsed, sizeof(double));
  put(&idleTotal, sizeof(double));
  put(&n, sizeof(int));
  for (const auto &kv : objLoads) {
    CmiUInt8 id = kv.first.second;
    int coll = kv.first.first;
    unsigned char migratable = 1;
    put(&id, sizeof(CmiUInt8));
    put(&coll, sizeof(int));
    put(&kv.second.wall, sizeof(double));
    put(&migratable, 1);
  }
}



void TraceUtilization::addEventType(int eventType)
{
  CkPrintf("FIXME handle TraceUtilization::addEventType(%d)\n", eventType);
}




/**

Send back to the client compressed sum-detail style measurements about the 
utilization for each active PE combined across all PEs.

The data format sent by this handler is a bunch of records(one for each bin) of the following format:
   #samples (EP,utilization)* 

One example record for two EPS that executed during the sample period. 
EP 3 used 150/200 of the time while EP 122 executed for 20/200 of the time. 
All of these would be packed as bytes into the message:
2 3 150 122 20

 */
void TraceUtilizationBOC::ccsRequestSumDetailCompressed(CkCcsRequestMsg *m) {
  CkPrintf("CCS request for compressed sum detail. (found %zu stored in deque)\n",  storedSumDetailResults.size() );
  //  CkAssert(sumDetail);
  int datalength;

#if 0

  compressedBuffer fakeMessage = fakeCompressedMessage();
  CcsSendDelayedReply(m->reply, fakeMessage.datalength(), fakeMessage.buffer() );
  fakeMessage.freeBuf();

#else

  if (storedSumDetailResults.size()  == 0) {
    compressedBuffer b = emptyCompressedBuffer();
    CcsSendDelayedReply(m->reply, b.datalength(), b.buffer()); 
    b.freeBuf();
  } else {
    CkReductionMsg * msg = storedSumDetailResults.front();
    storedSumDetailResults.pop_front();

    
    void *sendBuffer = (void *)msg->getData();
    datalength = msg->getSize();
    CcsSendDelayedReply(m->reply, datalength, sendBuffer);
    
    delete msg;
  }
    
  
#endif

  //  CkPrintf("CCS response of %d bytes sent.\n", datalength);
  delete m;
}



/** CCS handler "CkPerfSumDetail sts": reply with the sts text (the same
    lines TraceUtilization::writeSts puts in <traceroot>.util.sts), so a
    client can map the stream's entry-method indices to names without
    access to the job's files. Empty reply if the text cannot be built. */
void TraceUtilizationBOC::ccsRequestSts(CkCcsRequestMsg *m) {
  char *text = NULL;
  size_t len = 0;
#ifdef _WIN32
  // Windows has no open_memstream: write to an anonymous temporary file
  // (binary, so the bytes are the same) and read it back.
  FILE *f = tmpfile();
  if (f != NULL) {
    traceWriteSTS(f, 0);
    fprintf(f, "END\n");
    long n = ftell(f);
    if (n > 0 && (text = (char *)malloc((size_t)n)) != NULL) {
      rewind(f);
      len = fread(text, 1, (size_t)n, f);
    }
    fclose(f);
  }
#else
  FILE *f = open_memstream(&text, &len);
  if (f != NULL) {
    traceWriteSTS(f, 0);
    fprintf(f, "END\n");
    fclose(f);
  }
#endif
  CcsSendDelayedReply(m->reply, (int)len, text != NULL ? text : "");
  free(text);
  delete m;
}


/** CCS handler "CkPerfObjLoads" (PE 0): every PE's per-object cumulative
    execution time, in the layout LBManager's "LBObjLoads" uses, so a
    client can read either. Reply: int32 numPes, then one record per PE
    (see TraceUtilization::snapshotObjLoads). Requests that arrive while
    a collection is in flight are answered from the same reduction. */
void TraceObjLoadsBOC::ccsRequestObjLoads(CkCcsRequestMsg *m) {
  objLoadsReplies.push_back(m->reply);
  delete m;
  if (!objLoadsCollecting) {
    objLoadsCollecting = true;
    thisProxy.collectObjLoads();
  }
}

void TraceObjLoadsBOC::collectObjLoads() {
  std::vector<char> buf;
  CkpvAccess(_trace)->snapshotObjLoads(buf);
  contribute(buf.size(), buf.data(), CkReduction::concat,
             CkCallback(CkIndex_TraceObjLoadsBOC::objLoadsCollected(NULL), thisProxy[0]));
}

void TraceObjLoadsBOC::objLoadsCollected(CkReductionMsg *msg) {
  int npes = CkNumPes();
  std::vector<char> out(sizeof(int) + msg->getSize());
  memcpy(out.data(), &npes, sizeof(int));
  memcpy(out.data() + sizeof(int), msg->getData(), msg->getSize());
  for (const CcsDelayedReply &r : objLoadsReplies)
    CcsSendDelayedReply(r, (int)out.size(), out.data());
  objLoadsReplies.clear();
  objLoadsCollecting = false;
  delete msg;
}


void TraceUtilizationBOC::collectSumDetailData() {
  TraceUtilization* t = CkpvAccess(_trace);
  t->expireObjLoads();  // once a second, whether or not a client is polling

  // If we don't have enough data, just return and wait for the next invocation
  if (t->cpuTimeEntriesAvailable() - t->cpuTimeEntriesSentSoFar() < BIN_PER_SEC)
    return;

  compressedBuffer b = t->compressNRecentSumDetail(BIN_PER_SEC);

  //  CkPrintf("[%d] contributing buffer created by compressNRecentSumDetail avg util=%lg\n", CkMyPe(), averageUtilizationInBuffer(b));
  //  printCompressedBuf(b);
  // fflush(stdout);
  
  
#if 0
  b = fakeCompressedMessage();
#endif
  
  //  CkPrintf("[%d] contributing %d bytes worth of SumDetail data\n", CkMyPe(), b.datalength());
  
  //  CProxy_TraceUtilizationBOC sumProxy(traceSummaryGID);
  CkCallback cb(CkIndex_TraceUtilizationBOC::sumDetailDataCollected(NULL), thisProxy[0]);
  contribute(b.datalength(), b.buffer(), sumDetailCompressedReducer, cb);
  
  b.freeBuf();
}


void TraceUtilizationBOC::sumDetailDataCollected(CkReductionMsg *msg) {
  CkAssert(CkMyPe() == 0);

  compressedBuffer b(msg->getData());
  CkPrintf("putting CCS reply in queue (average utilization= %lg)\n", averageUtilizationInBuffer(b));
  //if(isCompressedBufferSane(b)){
    storedSumDetailResults.push_back(msg); 
    //}

    // CkPrintf("[%d] Reduction of SumDetail completed. Result stored in storedSumDetailResults deque(size now=%d)\n", CkMyPe(), storedSumDetailResults.size() );
    //  fflush(stdout);

}



void TraceUtilization::writeSts(void) {
  // open sts file
  int len = strlen(CkpvAccess(traceRoot))+strlen(".util.sts")+1;
  char *fname = new char[len];
  snprintf(fname, len, "%s.util.sts", CkpvAccess(traceRoot));
  FILE* stsfp = fopen(fname, "w+");
  if (stsfp == 0) {
       CmiAbort("Cannot open summary sts file for writing.\n");
  }
  delete[] fname;

  traceWriteSTS(stsfp,0);
  fprintf(stsfp, "END\n");

  fclose(stsfp);
}



////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


  
/// Compress a buffer by merging all entries in a bin that are less than the threshold into a single "other" category
  compressedBuffer moveTinyEntriesToOther(compressedBuffer src, double threshold){
    //    CkPrintf("[%d] moveTinyEntriesToOther\n", CkMyPe());
    
    // reset the src buffer to the beginning
    src.pos = 0;

    compressedBuffer dest(100000); 
    
    int numBins = src.pop<numBins_T>();
    int numProcs = src.pop<numProcs_T>();
    
    dest.push<numBins_T>(numBins);
    dest.push<numProcs_T>(numProcs);
    
    
    for(int i=0;i<numBins;i++){
      double utilizationInOther = 0.0;
      
      entriesInBin_T numEntriesInSrcBin = src.pop<entriesInBin_T>();
      int numEntriesInDestBinOffset = dest.push<entriesInBin_T>(0);
      
      CkAssert(numEntriesInSrcBin < 200);

      for(int j=0;j<numEntriesInSrcBin;j++){
	ep_T ep = src.pop<ep_T>();
	double v = src.pop<utilization_T>();
	
	if(v < threshold * 250.0){
	  // do not copy bin into destination
	  utilizationInOther += v / 250.0;
	} else {
	  // copy bin into destination
	  dest.increment<entriesInBin_T>(numEntriesInDestBinOffset);
	  dest.push<ep_T>(ep);
	  dest.push<utilization_T>(v);
	}

      }
      
      // if other category has stuff in it, add it to the destination buffer
      if(utilizationInOther > 0.0){
	dest.increment<entriesInBin_T>(numEntriesInDestBinOffset);
	dest.push<ep_T>(other_EP);
	if(utilizationInOther > 1.0)
	  utilizationInOther = 1.0;
	dest.push<utilization_T>(utilizationInOther*250.0);
      }
      
    }
   
    return dest;
  }
  
    


/// A reducer for merging compressed sum detail data
CkReductionMsg *sumDetailCompressedReduction(int nMsg,CkReductionMsg **msgs){
  // CkPrintf("[%d] sumDetailCompressedReduction(nMsgs=%d)\n", CkMyPe(), nMsg);
  
  compressedBuffer *incomingMsgs = new compressedBuffer[nMsg];
  int *numProcsRepresentedInMessage = new int[nMsg];
  
  int numBins = 0;
  int totalsize = 0;
  int totalProcsAcrossAllMessages = 0;
  
  for (int i=0;i<nMsg;i++) {
    incomingMsgs[i].init(msgs[i]->getData());
    
    //  CkPrintf("[%d] Incoming reduction message %d has average utilization %lg\n", CkMyPe(),  i, averageUtilizationInBuffer(incomingMsgs[i])); 
    //   CkPrintf("Is buffer %d sane? %s\n", i, isCompressedBufferSane(incomingMsgs[i]) ? "yes": "no" );


    totalsize += msgs[i]->getSize();
    //  CkPrintf("BEGIN MERGE MESSAGE=========================================================\n");
    //   printCompressedBuf(incomingMsgs[i]);
    
    // Read first value from message. 
    // Make sure all messages have the same number of bins
    // The pop must happen in every build: a CkAssert argument is not
    // evaluated when error checking is off, which left the bin count of
    // messages 1..n unread and shifted their parse by four bytes.
    numBins_T binsInThisMsg = incomingMsgs[i].pop<numBins_T>();
    if(i==0)
      numBins = binsInThisMsg;
    else 
      CkAssert( numBins == binsInThisMsg );
    
    // Read second value from message. 
    numProcsRepresentedInMessage[i] = incomingMsgs[i].pop<numProcs_T>();
    totalProcsAcrossAllMessages += numProcsRepresentedInMessage[i];
    //    CkPrintf("Number of procs in message[%d] is %d\n", i,  (int)numProcsRepresentedInMessage[i]);
  }
  
  compressedBuffer dest(totalsize + 100); 
  
  // build a compressed representation of each merged bin
  dest.push<numBins_T>(numBins);
  dest.push<numProcs_T>(totalProcsAcrossAllMessages);
  
  for(int i=0; i<numBins; i++){
    mergeCompressedBin(incomingMsgs, nMsg, numProcsRepresentedInMessage, totalProcsAcrossAllMessages, dest);
  }
  
  // CkPrintf("END MERGE RESULT=========================================================\n");
  // printCompressedBuf(dest);


  //CkPrintf("[%d] Merged buffer has average utilization %lg \n", CkMyPe(), averageUtilizationInBuffer(dest));

  //CkPrintf("Is resulting merged buffer sane? %s\n", isCompressedBufferSane(dest) ? "yes": "no" );  
  
  compressedBuffer dest2 = moveTinyEntriesToOther(dest, 0.10);
  
  //  CkPrintf("Is resulting merged Filtered buffer sane? %s\n", isCompressedBufferSane(dest2) ? "yes": "no" ); 

  //  CkPrintf("[%d] Outgoing reduction (filtered) message has average utilization %lf \n", CkMyPe(), averageUtilizationInBuffer(dest2));

  
  CkReductionMsg *m = CkReductionMsg::buildNew(dest2.datalength(),dest2.buffer());   
  dest.freeBuf();
  delete[] incomingMsgs;
  delete[] numProcsRepresentedInMessage;
  return m;
}







/// Create fake sum detail data in the compressed format (for debugging)
 compressedBuffer fakeCompressedMessage(){
   CkPrintf("[%d] fakeCompressedMessage\n", CkMyPe());
   
   compressedBuffer fakeBuf(10000);
   
   int numBins = 55;
   int numProcs = 1000;

   // build a compressed representation of each merged bin
   fakeBuf.push<numBins_T>(numBins);
   fakeBuf.push<numProcs_T>(numProcs);
   for(int i=0; i<numBins; i++){
     int numRecords = 3;
     fakeBuf.push<entriesInBin_T>(numRecords);
     for(int j=0;j<numRecords;j++){
       fakeBuf.push<ep_T>(j*10+2);
       fakeBuf.push<utilization_T>(120.00);
     }  
   }
   
   //CkPrintf("Fake Compressed Message:=========================================================\n");
   //   printCompressedBuf(fakeBuf);

   CkAssert(isCompressedBufferSane(fakeBuf));

   return fakeBuf;
 }


 /// Create an empty message
 compressedBuffer emptyCompressedBuffer(){
   compressedBuffer result(sizeof(numBins_T));
   result.push<numBins_T>(0);
   return result;
 }




/** print out the compressed buffer starting from its begining*/
void printCompressedBuf(compressedBuffer b){
  // b should be passed in by value, and hence we can modify it
  b.pos = 0;
  int numEntries = b.pop<numBins_T>();
  CkPrintf("Buffer contains %d records\n", numEntries);
  int numProcs = b.pop<numProcs_T>();
  CkPrintf("Buffer represents an average over %d PEs\n", numProcs);

  for(int i=0;i<numEntries;i++){
    entriesInBin_T recordLength = b.pop<entriesInBin_T>();
    if(recordLength > 0){
      CkPrintf("    Record %d is of length %d : ", i, recordLength);
      
      for(int j=0;j<recordLength;j++){
	ep_T ep = b.pop<ep_T>();
	utilization_T v = b.pop<utilization_T>();
	CkPrintf("(%d,%d) ", ep, v);
      }
    
      CkPrintf("\n");
    }
  }
}



 bool isCompressedBufferSane(compressedBuffer b){
   // b should be passed in by value, and hence we can modify it  
   b.pos = 0;  
   numBins_T numBins = b.pop<numBins_T>();  
   numProcs_T numProcs = b.pop<numProcs_T>();  
   
   if(numBins > 2000){
     ckout << "WARNING: numBins=" << numBins << endl;
     return false;
   }
   
   for(int i=0;i<numBins;i++){  
     entriesInBin_T recordLength = b.pop<entriesInBin_T>();  
     if(recordLength > 200){
       ckout << "WARNING: recordLength=" << recordLength << endl;
       return false;
     }
     
     if(recordLength > 0){  
       
       for(int j=0;j<recordLength;j++){  
         ep_T ep = b.pop<ep_T>();  
         utilization_T v = b.pop<utilization_T>();  
         //      CkPrintf("(%d,%f) ", ep, v);  
	 if(((ep>800 || ep <0 ) && ep != other_EP) || v < 0.0 || v > 251.0){
	   ckout << "WARNING: ep=" << ep << " v=" << v << endl;
	   return false;
	 }
       }  
       
     }  
   }  
   
   return true;
 }



 double averageUtilizationInBuffer(compressedBuffer b){
   // b should be passed in by value, and hence we can modify it  
   b.pos = 0;  
   numBins_T numBins = b.pop<numBins_T>();  
   numProcs_T numProcs = b.pop<numProcs_T>();  
   
   //   CkPrintf("[%d] averageUtilizationInBuffer numProcs=%d   (grep reduction message)\n", CkMyPe(), numProcs);
   
   double totalUtilization = 0.0;
   
   for(int i=0;i<numBins;i++) {  
     entriesInBin_T entriesInBin = b.pop<entriesInBin_T>();     
     for(int j=0;j<entriesInBin;j++){  
       ep_T ep = b.pop<ep_T>();  
       totalUtilization +=  b.pop<utilization_T>();  
     }
   }
   
   return totalUtilization / numBins / 2.5;
 }
 
 

void sanityCheckCompressedBuf(compressedBuffer b){  
   CkAssert(isCompressedBufferSane(b)); 
 }  
 


double TraceUtilization::sumUtilization(int startBin, int endBin){
   int epInfoSize = getEpInfoSize();
   
   double a = 0.0;

   for(int i=startBin; i<=endBin; i++){
     for(int j=0; j<epInfoSize; j++){
       a += cpuTime[(i%NUM_BINS)*epInfoSize+j];
     }
   }
   return a;
 }

 
 /// Create a compressed buffer of the n most recent sum detail samples
 compressedBuffer TraceUtilization::compressNRecentSumDetail(int desiredBinsToSend){
   //   CkPrintf("compressNRecentSumDetail(desiredBinsToSend=%d)\n", desiredBinsToSend);

   int startBin =  cpuTimeEntriesSentSoFar();
   int numEntries = getEpInfoSize();

   int endBin = startBin + desiredBinsToSend - 1;
   int binsToSend = endBin - startBin + 1;
   CkAssert(binsToSend >= desiredBinsToSend );
   incrementNumCpuTimeEntriesSent(binsToSend);


#if 0
   bool nonePrinted = true;
   for(int i=0;i<(NUM_BINS-1000);i+=1000){
     double expectedU = sumUtilization(i, i+999);
     if(expectedU > 0.0){
          CkPrintf("[%d of %d] compressNRecentSumDetail All bins: start=%05d end=%05d values in array sum to %lg\n", CkMyPe(), CkNumPes(),  i, i+999, expectedU);
       nonePrinted = false;
     }
   }
   
   if(nonePrinted)
     CkPrintf("[%d of %d] compressNRecentSumDetail All bins are 0\n", CkMyPe(), CkNumPes() );

   fflush(stdout);
#endif

   int bufferSize = 8*(2+numEntries) * (2+binsToSend)+100;
   compressedBuffer b(bufferSize);

   b.push<numBins_T>(binsToSend);
   b.push<numProcs_T>(1); // number of processors along reduction subtree. I am just one processor.
   //   double myu = 0.0;
   
   for(int i=0; i<binsToSend; i++) {
     // Create a record for bin i
     //  CkPrintf("Adding record for bin %d\n", i);
     int numEntriesInRecordOffset = b.push<entriesInBin_T>(0); // The number of entries in this record
     
     for(int e=0; e<numEntries; e++) {
       double scaledUtilization = getUtilization(i+startBin,e) * 2.5; // use range of 0 to 250 for the utilization, so it can fit in an unsigned char
       if(scaledUtilization > 0.0) {
	 //CkPrintf("scaledUtilization=%lg !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n", scaledUtilization);
	 if(scaledUtilization > 250.0)
	   scaledUtilization = 250.0;
	 
	 b.push<ep_T>(e);
	 b.push<utilization_T>(scaledUtilization);
	 //	 myu += scaledUtilization;
	 b.increment<entriesInBin_T>(numEntriesInRecordOffset);
       }
     }
   }
   
   // CkPrintf("[%d] compressNRecentSumDetail resulting buffer: averageUtilizationInBuffer()=%lg myu=%lg\n", CkMyPe(), averageUtilizationInBuffer(b), myu);
   // fflush(stdout);
   
   return b;
 }
 




/** Merge the compressed entries from the first bin in each of the srcBuf buffers.
     
*/
 void mergeCompressedBin(compressedBuffer *srcBufferArray, int numSrcBuf, int *numProcsRepresentedInMessage, int totalProcsAcrossAllMessages, compressedBuffer &destBuf){
  // put a counter at the beginning of destBuf
  int numEntriesInDestRecordOffset = destBuf.push<entriesInBin_T>(0);
  
  //  CkPrintf("BEGIN MERGE------------------------------------------------------------------\n");
  
  // Read off the number of bins in each buffer
  int *remainingEntriesToRead = new int[numSrcBuf];
  for(int i=0;i<numSrcBuf;i++){
    remainingEntriesToRead[i] = srcBufferArray[i].pop<entriesInBin_T>();
  }

  int count = 0;
  // Count remaining entries to process
  for(int i=0;i<numSrcBuf;i++){
    count += remainingEntriesToRead[i];
  }
  
  while (count>0) {
    // find first EP from all buffers (these are sorted by EP already)
    int minEp = 10000;
    for(int i=0;i<numSrcBuf;i++){
      if(remainingEntriesToRead[i]>0){
	int ep = srcBufferArray[i].peek<ep_T>();
	if(ep < minEp){
	  minEp = ep;
	}
      }
    }
    
    //   CkPrintf("[%d] mergeCompressedBin minEp found was %d   totalProcsAcrossAllMessages=%d\n", CkMyPe(), minEp, (int)totalProcsAcrossAllMessages);
    
    destBuf.increment<entriesInBin_T>(numEntriesInDestRecordOffset);

    // Merge contributions from all buffers that list the EP
    double v = 0.0;
    for(int i=0;i<numSrcBuf;i++){
      if(remainingEntriesToRead[i]>0){
	int ep = srcBufferArray[i].peek<ep_T>(); 
	if(ep == minEp){
	  srcBufferArray[i].pop<ep_T>();  // pop ep
	  double util = srcBufferArray[i].pop<utilization_T>();
	  v += util * numProcsRepresentedInMessage[i];
	  remainingEntriesToRead[i]--;
	  count --;
	}
      }
    }

    // create a new entry in the output for this EP.
    destBuf.push<ep_T>(minEp);
    destBuf.push<utilization_T>(v / (double)totalProcsAcrossAllMessages);

  }


  delete [] remainingEntriesToRead;
  // CkPrintf("[%d] End of mergeCompressedBin:\n", CkMyPe() );
  // CkPrintf("END MERGE ------------------------------------------------------------------\n");
 }



#include "TraceUtilization.def.h"


/*@}*/




















