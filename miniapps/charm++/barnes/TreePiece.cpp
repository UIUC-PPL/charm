#include "TreePiece.h"
#include "Messages.h"
#include "DataManager.h"
#include "Parameters.h"

#include <fstream>

extern CProxy_Main mainProxy;
extern CProxy_DataManager dataManagerProxy;
extern Parameters globalParams;

TreePiece::TreePiece() : 
  myNumParticles(0),
  numDecompMsgsRecvd(0),
  smallestKey(~Key(0)),
  largestKey(Key(0)),
  localTraversalState(),
  remoteTraversalState(),
  localStateID(0), 
  remoteStateID(1),
  localTraversalWorker(),
  remoteTraversalWorker(),
  totalNumTraversals(2),
  iteration(0),
  numTraversalsDone(0)
{
  myDM = dataManagerProxy.ckLocalBranch();
}

void TreePiece::receiveParticles(ParticleMsg *msg){
  int msgNumParticles = msg->numParticles;
  decompMsgsRecvd.push_back(msg);
  myNumParticles += msgNumParticles;
  numDecompMsgsRecvd++;
  if(smallestKey > msg->part[0].key) smallestKey = msg->part[0].key;
  if(largestKey < msg->part[msgNumParticles-1].key) largestKey = msg->part[msgNumParticles-1].key;
  
  if(numDecompMsgsRecvd == CkNumPes()){
    submitParticles();
    numDecompMsgsRecvd = 0;
  }
}

void TreePiece::receiveParticles(){
  numDecompMsgsRecvd++;
  if(numDecompMsgsRecvd == CkNumPes()){
    submitParticles();
    numDecompMsgsRecvd = 0;
  }
}

void TreePiece::submitParticles(){
  myDM->submitParticles(&decompMsgsRecvd,myNumParticles,this,smallestKey,largestKey);
}

void TreePiece::prepare(Node<ForceData> *_root, Node<ForceData> **buckets, int bucketStart, int bucketEnd){
  root = _root;
  myBuckets = buckets+bucketStart;
  myNumBuckets = bucketEnd-bucketStart;
}

void TreePiece::startTraversal(){
  numTraversalsDone = 0;
  trav.setDataManager(myDM);

  if(myNumBuckets == 0){
    localGravityDone();
    remoteGravityDone();
    return;
  }

  remoteTraversalState.reset(this,myNumBuckets,myBuckets);
  remoteTraversalWorker.reset(this,&remoteTraversalState,*myBuckets);
  RescheduleMsg *msg = new (NUM_PRIORITY_BITS) RescheduleMsg;
  *(int *)CkPriorityPtr(msg) = REMOTE_GRAVITY_PRIORITY;
  CkSetQueueing(msg, CK_QUEUEING_IFIFO);
  thisProxy[thisIndex].doRemoteGravity(msg);

  localTraversalState.reset(this,myNumBuckets,myBuckets);
  localTraversalWorker.reset(this,&localTraversalState,*myBuckets);

  msg = new (NUM_PRIORITY_BITS) RescheduleMsg;
  *(int *)CkPriorityPtr(msg) = LOCAL_GRAVITY_PRIORITY;
  CkSetQueueing(msg, CK_QUEUEING_IFIFO);
  thisProxy[thisIndex].doLocalGravity(msg);
}

void TreePiece::doLocalGravity(RescheduleMsg *msg){
  int i;
  for(i = 0; i < globalParams.yieldPeriod && 
                 localTraversalState.current < myNumBuckets; 
                 i++){
    trav.topDownTraversal(root,&localTraversalWorker,&localTraversalState);
    localTraversalState.current++;
    localTraversalState.currentBucketPtr++;
    localTraversalWorker.setContext(*localTraversalState.currentBucketPtr);
  }

  if(localTraversalState.decrPending(i)){
    localGravityDone();
    delete msg;
  }
  else if(localTraversalState.current < myNumBuckets) {
    thisProxy[thisIndex].doLocalGravity(msg);
  }
}

void TreePiece::doRemoteGravity(RescheduleMsg *msg){
  int i;
  for(i = 0; i < globalParams.yieldPeriod &&
                 remoteTraversalState.current < myNumBuckets;
                 i++){
    trav.topDownTraversal(root,&remoteTraversalWorker,&remoteTraversalState);
    remoteTraversalState.current++;
    remoteTraversalState.currentBucketPtr++;
    remoteTraversalWorker.setContext(*remoteTraversalState.currentBucketPtr);
  }

  if(remoteTraversalState.decrPending(i)){
    remoteGravityDone();
    delete msg;
  }
  else if (remoteTraversalState.current < myNumBuckets) {
    thisProxy[thisIndex].doRemoteGravity(msg);
  }
}

void TreePiece::localGravityDone(){
  traversalDone();
}

void TreePiece::remoteGravityDone(){ 
  traversalDone();
}

void TreePiece::traversalDone(){ 
  numTraversalsDone++;

  if(numTraversalsDone == totalNumTraversals){
#ifdef STATISTICS
    CmiUInt8 pn = localTraversalState.numInteractions[0]+remoteTraversalState.numInteractions[0];
    CmiUInt8 pp = localTraversalState.numInteractions[1]+remoteTraversalState.numInteractions[1];
    CmiUInt8 oc = localTraversalState.numInteractions[2]+remoteTraversalState.numInteractions[2];
    dataManagerProxy[CkMyPe()].traversalsDone(pn,pp,oc);
#else
    dataManagerProxy[CkMyPe()].traversalsDone();
#endif

    finishIteration();
  }
}

void TreePiece::finishIteration(){
  numTraversalsDone = 0;

  localTraversalState.finishedIteration();
  remoteTraversalState.finishedIteration();

  myNumParticles = 0;
  numDecompMsgsRecvd = 0;
  decompMsgsRecvd.length() = 0;

  iteration++;

  checkTraversals();
}

void TreePiece::checkTraversals(){
}

void TreePiece::quiescence(){
  CkPrintf("QUIESCENCE tree piece %d proc %d submitted %d numBuckets %d trav_done %d outstanding local %d remote %d\n", 
                thisIndex,
                CkMyPe(),
                myNumParticles,
                myNumBuckets,
                numTraversalsDone,
                localTraversalState.pending,
                remoteTraversalState.pending);

  CkCallback cb(CkIndex_Main::quiescenceExit(),mainProxy);
  contribute(0,0,CkReduction::sum_int,cb);
}

void TreePiece::requestMoments(Key k, int replyTo){
  myDM->requestMoments(k,replyTo);
}

void TreePiece::requestParticles(RequestMsg *msg){
  // forward the request to the DM, since TPs don't
  // really own particles or nodes
  myDM->requestParticles(msg);
}

void TreePiece::requestNode(RequestMsg *msg){
  myDM->requestNode(msg);
}

int TreePiece::getIteration() {
  return iteration;
}

void TreePiece::pup(PUP::er &p){
  p | iteration;
  if(p.isUnpacking()){
  }
}

#include "Traversal_defs.h"

