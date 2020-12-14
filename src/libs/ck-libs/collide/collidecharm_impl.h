/*
 * Parallel layer for Collision detection system
 * Orion Sky Lawlor, olawlor@acm.org, 4/8/2001
 */
#ifndef __UIUC_CHARM_COLLIDECHARM_IMPL_H
#define __UIUC_CHARM_COLLIDECHARM_IMPL_H

#include "collide_serial.h"
#include "collide_buffers.h"
struct collideStats; // forward declaration for collideStats
#include "collidecharm.decl.h"
#include "collidecharm.h"

/******************** objListMsg *********************
  A pile of objects sent to a Collision voxel.
  Declared as a "packed" message
  type, which converts the message to a flat byte array
  only when needed (for sending across processors).
  */
class objListMsg : public CMessage_objListMsg
{
  public:
    class returnReceipt {
      CkGroupID gid;
      public:
      int onPE;
      returnReceipt() {}
      returnReceipt(CkGroupID gid_,int onPE_) :gid(gid_),onPE(onPE_) {}
      void send(void);
    };
  private:
    bool isHeapAllocated;
    returnReceipt receipt;

    int n;
    CollideObjRec *obj; //Bounding boxes & IDs

    void freeHeapAllocated();
  public:
    objListMsg() :isHeapAllocated(false) {}

    //Hand control of these arrays over to this message,
    // which will delete them when appropriate.
    objListMsg(int n_,CollideObjRec *obj_,
        const returnReceipt &receipt_);
    ~objListMsg() {freeHeapAllocated();}

    int getSource(void) {return receipt.onPE;}
    void sendReceipt(void) {receipt.send();}

    int getObjects(void) const {return n;}
    const CollideObjRec &getObj(int i) const {return obj[i];}
    const bbox3d &bbox(int i) const {return obj[i].box;}

    static void *pack(objListMsg *m);
    static objListMsg *unpack(void *m);
};


/****************** aggregator *************/
#include "ckhashtable.h"

class collideMgr;
class voxelAggregator;

/*This class splits each chunk's triangles into messages
 * headed out to each voxel.  It is implemented as a group.
 */
class CollisionAggregator {
  CollideGrid3d gridMap;
  CkHashtableT<CollideLoc3d,voxelAggregator *> voxels;
  collideMgr *mgr;

  //Add a new accumulator to the hashtable
  voxelAggregator *addAccum(const CollideLoc3d &dest);
  public:
  CollisionAggregator(const CollideGrid3d &gridMap_,collideMgr *mgr_);
  ~CollisionAggregator();

  //Add this chunk's triangles
  void aggregate(int pe,int chunk,
      int n,const bbox3d *boxes,const int *prio);

  //Send off all accumulated voxel messages
  void send(void);

  //Delete all cached aggregators
  void compact(void);
};

/********************* syncReductionMgr *****************
  A group to synchronize on some event across the machine.
  Maintains a reduction tree and waits for the "advance"
  method to be called from each processor.  To handle
  non-autonomous cases, calls "pleaseAdvance" when an advance
  is first expected from that PE.
  */
class syncReductionMgr : public CBase_syncReductionMgr
{
  CProxy_syncReductionMgr thisproxy;
  void status(const char *msg) {
    CkPrintf("SRMgr pe %d> %s\n",CkMyPe(),msg);
  }
  //Describes the reduction tree
  int onPE;
  enum {TREE_WID=4};
  int treeParent;//Parent in reduction tree
  int treeChildStart,treeChildEnd;//First and last+1 child
  int nChildren;//Number of children in the reduction tree
  void startStep(int stepNo,bool withProd);

  //State data
  int stepCount;//Increments by one every reduction, from zero
  bool stepFinished;//prior step complete
  bool localFinished;//Local advance called
  int childrenCount;//Number of tree children in delivered state
  void tryFinish(void);//Try to finish reduction

  protected:
  //This is called by subclasses
  void advance(void);
  //This is offered for subclasses's optional use
  virtual void pleaseAdvance(void);
  //This is called on PE 0 once the reduction is finished
  virtual void reductionFinished(void);
  public:
  int getStepCount(void) const {return stepCount;}
  syncReductionMgr();

  //Called by parent-- will you contribute?
  void childProd(int stepCount);
  //Called by tree children-- me and my children are finished
  void childDone(int stepCount);
};

/********************** statsCollector *****************
 Prints stats like the number of voxels created.
  */
class statsCollector : public CBase_statsCollector {
  public:
  statsCollector() {}
  statsCollector(CkMigrateMessage *m) {}
  void voxelCountDone(int result) { // Reduction target method used to print the voxel count
    CkPrintf("Collision Detection Library Stats - Number of voxels created = %d\n", result);
  }
};

/********************** collideStats *****************
 * Wraps together variables required for printing stats
 */
struct collideStats {
  bool printVoxCount; // Boolean stores if the CD library has to print voxel count
  CProxy_statsCollector statsCollProxy; // Proxy to stats collector, used for reduction

  void pup(PUP::er &p) {
    p | printVoxCount;
    p | statsCollProxy;
  }
};



/*********************** collideMgr **********************
  A group that synchronizes the Collision detection process.
  A single Collision operation consists of:
  -collect contributions from clients (contribute)
  -aggregate the contributions
  -send off triangle lists to voxels
  -wait for replies from the voxels indicating sucessful delivery
  -collideMgr reduction to make sure everybody's messages are delivered
  -root broadcasts startCollision to collideVoxel array
  -client group accepts the CollisionLists
  */

class collideMgr : public CBase_collideMgr
{
  CProxy_collideMgr thisproxy;
  private:
  void status(const char *msg) {
    CkPrintf("CMgr pe %d> %s\n",CkMyPe(),msg);
  }
  int steps; //Number of separate Collision operations
  CProxy_collideVoxel voxelProxy;
  CollideGrid3d gridMap; //Shape of 3D voxel grid
  CProxy_collideClient client; //Collision client group
  collideStats statObj;

  int nContrib;//Number of registered contributors
  int contribCount;//Number of contribute calls given this step

  CollisionAggregator aggregator;
  int msgsSent;//Messages sent out to voxels
  int msgsRecvd;//Return-receipt messages received from voxels
  void tryAdvance(void);
  protected:
  //Check if we're barren-- if so, advance now
  virtual void pleaseAdvance(void);
  //This is called on PE 0 once the voxel send reduction is finished
  virtual void reductionFinished(void);
  public:
  collideMgr(const CollideGrid3d &gridMap,
      const CProxy_collideClient &client,
      const CProxy_collideVoxel &voxels,
      const collideStats &statObj);

  //Maintain contributor registration count
  void registerContributor(int chunkNo);
  void unregisterContributor(int chunkNo);

  //Clients call this to contribute their objects
  void contribute(int chunkNo,
      int n,const bbox3d *boxes,const int *prio);

  //voxelAggregators deliver messages to voxels via this bottleneck
  void sendVoxelMessage(const CollideLoc3d &dest,
      int n,CollideObjRec *obj);

  //collideVoxels send a return receipt here
  void voxelMessageRecvd(void);
};

/********************** collideVoxel ********************
  A sparse 3D array that represents a region of space where
  Collisions may occur.  Each step it accumulates triangle
  lists and then computes their intersection
  */

class collideVoxel : public CBase_collideVoxel
{
  growableBufferT<objListMsg *> msgs;
  void status(const char *msg);
  void emptyMessages();
  void collide(const bbox3d &territory,CollisionList &dest);
  public:
  collideVoxel(void);
  collideVoxel(CkMigrateMessage *m);
  ~collideVoxel();
  void pup(PUP::er &p);

  void add(objListMsg *msg);
  void startCollision(int step,
      const CollideGrid3d &gridMap,
      const CProxy_collideClient &client,
      const collideStats &statObj);
};


/********************** serialCollideClient *****************
  Reduces the Collision list down to processor 0.
  */
class serialCollideClient : public collideClient {
  CollisionClientFn clientFn;
  void *clientParam;
  CkCallback clientCb;
  bool useCb;
  public:
  serialCollideClient(void);
  serialCollideClient(CkCallback clientCb_);

  /// Call this client function on processor 0:
  void setClient(CollisionClientFn clientFn,void *clientParam);

  /// Called by voxel array on each processor:
  virtual void collisions(ArrayElement *src,
      int step,CollisionList &colls);

  /// Called after the reduction is complete:
  virtual void reductionDone(CkReductionMsg *m);
};

#if CMK_TRACE_ENABLED
// List of COLLIDE functions to trace:
static const char *funclist[] = {"COLLIDE_Init", "COLLIDE_Boxes",
  "COLLIDE_Boxes_prio", "COLLIDE_Count", "COLLIDE_List",
  "COLLIDE_Destroy"};

#endif // CMK_TRACE_ENABLED

#endif //def(thisHeader)
