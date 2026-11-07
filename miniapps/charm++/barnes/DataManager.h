#ifndef __DATA_MANAGER_H__
#define __DATA_MANAGER_H__

#include "Particle.h"

#include "OrientedBox.h"
#include "barnes.decl.h"
#include "Node.h"
#include "Descriptor.h"
#include "ActiveBinInfo.h"

#include "Traversal_decls.h"
#include "Request.h"

class TreePiece;

#include <map>
using namespace std;

class TreePieceCounter : public CkLocIterator {            
  public:
  int count;
  CkHashtableT<CkArrayIndex, int> registered;               
  TreePieceCounter() : count(0) { }                      
  void addLocation(CkLocation &loc) {
    registered.put(loc.getIndex()) = ++count;               
  }
  void reset() {                                            
    count = 0;
    registered.empty();                                     
  }                                                         
};

struct RequestedMomentsDescriptor {
  Node<ForceData> *node;
  int numOutstanding;

  RequestedMomentsDescriptor() : 
    node(NULL), numOutstanding(-1)
  {
  }

  RequestedMomentsDescriptor(Node<ForceData> *nd, int n) : 
    node(nd), numOutstanding(n)
  {
  }

};

struct CacheStats {
  int outstandingRequests;
  int outstandingDeliveries;

  CacheStats() : 
    outstandingRequests(0),
    outstandingDeliveries(0)
  {
  }

  void incrRequests(){ outstandingRequests++; }
  void decrRequests(int n=1){ outstandingRequests -= n; }
  void incrDeliveries(){ outstandingDeliveries++; }
  void decrDeliveries(int n=1){ outstandingDeliveries -= n; }
  bool test(){ return (outstandingRequests==0) && (outstandingDeliveries==0); }
};

class DataManager : public CBase_DataManager {
  int numRankBits;
  double prevIterationStart;
  double avgIterationRuntime;


  CkVec<Particle> myParticles;
  int myNumParticles;

  bool firstSplitterRound;

  Node<NodeDescriptor> *sortingRoot;
  int numTreePieces;

  int iteration;
  int decompIterations;
  ActiveBinInfo<NodeDescriptor> activeBins;

  TreePieceCounter localTreePieces;
  int numLocalTreePieces;
  CkVec<TreePieceDescriptor> submittedParticles;
  Node<ForceData> *root;

  Key *keyRanges;
  bool haveRanges;
  RangeMsg *rangeMsg;
  CkVec<Node<ForceData>*> myBuckets;

  // I am done constructing the tree 
  // from particles present on this PE
  bool doneTreeBuild;
  //CkVec<RequestedMomentsDescriptor> requestedMoments;
  map<Key,Node<ForceData>*> nodeTable;

  map<Key,CkVec<int> > pendingMoments;

  // I have processed the moment 
  // contributions from all other PEs, so that
  // the tree on this PE is now ready for 
  // traversal
  bool treeMomentsReady;
  CkVec<RequestMsg *> bufferedNodeRequests;
  CkVec<RequestMsg *> bufferedParticleRequests;

  Traversal<NodeDescriptor> scaffoldTrav;
  Traversal<ForceData> fillTrav;

  map<Key,Request> nodeRequestTable;
  map<Key,Request> particleRequestTable;

  int numTreePiecesDoneTraversals;
  CacheStats nodeReqs;
  CacheStats partReqs;

  Real savedEnergy;

#ifdef STATISTICS
  CmiUInt8 numInteractions[3];
#endif

  void kickDriftKick(OrientedBox<Real> &box, Real &energy);

  void hashParticleCoordinates(const OrientedBox<Real> &universe);
  void initHistogramParticles();
  void sendHistogram();
  
  void senseTreePieces();
  void buildTree();

  void printTree();
  void flushParticles();

  void processSubmittedParticles();
  void makeMoments();
  void flushMomentRequests();
  void respondToMomentsRequest(Node<ForceData> *,CkVec<int>&);
  Node<ForceData> *lookupNode(Key k);

  void updateLeafMoments(Node<ForceData> *node, MomentsExchangeStruct &data);
  void passMomentsUpward(Node<ForceData> *node);
  void treeReady();

  void startTraversal();
  void flushBufferedRemoteDataRequests();

  void freeCachedData();
  void freeTree();
  void finishIteration();

  void findMinVByA(DtReductionStruct &);

  void markNaNBuckets();

  public:
  DataManager();

  void loadParticles(const CkCallback &cb);

  void decompose(const BoundingBox &universe);
  void receiveHistogram(CkReductionMsg *msg);
  void receiveSplitters(SplitterMsg *msg);
  void sendParticles(RangeMsg *msg);
  void sendParticlesToTreePiece(Node<NodeDescriptor> *nd, int tp);

  void receiveMoments(MomentsMsg *msg);
  
  // called by tree pieces
  void submitParticles(CkVec<ParticleMsg *> *vec, int numParticles, TreePiece *tp, Key smallestKey, Key largestKey); 
  void requestMoments(Key k, int replyTo);
  void advance(CkReductionMsg *);
#ifdef STATISTICS
  void traversalsDone(CmiUInt8 pnInter, CmiUInt8 ppInter, CmiUInt8 openCrit);
#else
  void traversalsDone();
#endif

  // called by tree piece that is making a request
  void requestNode(Node<ForceData> *leaf, CutoffWorker<ForceData> *worker, State *state, Traversal<ForceData> *callbackTraversal);
  void requestParticles(Node<ForceData> *leaf, CutoffWorker<ForceData> *worker, State *state, Traversal<ForceData> *callbackTraversal);

  // called by tree piece that is forwarding a remote request
  void requestNode(RequestMsg *msg);
  void requestParticles(RequestMsg *msg);
  
  void recvParticles(ParticleReplyMsg *msg);
  void recvNode(NodeReplyMsg *msg);

  void recvUnivBoundingBox(CkReductionMsg *msg);

  void quiescence();

  void addBucketNodeInteractions(Key k, CmiUInt8 pn);
  void addBucketPartInteractions(Key k, CmiUInt8 pp);
};

#endif
