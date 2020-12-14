/*
 * Parallel layer for Collision detection system
 * Orion Sky Lawlor, olawlor@acm.org, 4/8/2001
 */
#include "charm++.h"
#include "collidecharm_impl.h"

#define COLLIDE_TRACE 0
#if COLLIDE_TRACE
//copious annoying debugging output
#  define SRM_STATUS(x) ckout<<"["<<CkMyPe()<<"] C.Sync> "<<x<<endl;
#  define CM_STATUS(x) ckout<<"["<<CkMyPe()<<"] C.Mgr> "<<x<<endl;
#  define CC_STATUS(x) { \
  char buf[100]; \
  voxName(thisIndex,buf); \
  ckout<<"["<<CkMyPe()<<"] "<<buf<<" Voxel> "<<x<<endl; \
}

#  define COL_STATUS(x) ckout<<"["<<CkMyPe()<<"] Collide> "<<x<<endl;
#else
//No debugging output
#  define SRM_STATUS(x) /*empty*/
#  define CM_STATUS(x) /*empty*/
#  define CC_STATUS(x) /*empty*/
#  define COL_STATUS(x) /*empty*/
#endif

/*************** Charm++ User External Interface ********
  Implementation of collide.h routines.
  */

/// Call this on processor 0 to build a Collision client that
///  just calls this serial routine on processor 0 with the final,
///  complete Collision list.
CkGroupID CollideSerialClient(CollisionClientFn clientFn,void *clientParam)
{
  CProxy_serialCollideClient cl=CProxy_serialCollideClient::ckNew();
  cl.ckLocalBranch()->setClient(clientFn,clientParam);
  return cl;
}

CkGroupID CollideSerialClient(CkCallback clientCb)
{
  CProxy_serialCollideClient cl = CProxy_serialCollideClient::ckNew(clientCb);
  return cl;
}

/// Create a collider group to contribute objects to.
///  Should be called on processor 0.
CollideHandle CollideCreate(const CollideGrid3d &gridMap,
    CkGroupID clientGroupID)
{

  char **argv = CkGetArgv();

  collideStats statsObj;
  statsObj.printVoxCount = CmiGetArgFlag(argv, "+printVoxCount");

  if(statsObj.printVoxCount)
    statsObj.statsCollProxy = CProxy_statsCollector::ckNew();

  CProxy_collideVoxel voxels=CProxy_collideVoxel::ckNew();
  voxels.doneInserting();
  CProxy_collideClient client(clientGroupID);
  return CProxy_collideMgr::ckNew(gridMap,client,voxels, statsObj);
}

/// Register with this collider group.
void CollideRegister(CollideHandle h,int chunkNo) {
  CProxy_collideMgr mgr(h);
  mgr.ckLocalBranch()->registerContributor(chunkNo);
}
/// Unregister with this collider group.
void CollideUnregister(CollideHandle h,int chunkNo) {
  CProxy_collideMgr mgr(h);
  mgr.ckLocalBranch()->unregisterContributor(chunkNo);
}

/// Send these objects off to be collided.
/// The results go the collisionClient group
/// registered at creation time.
void CollideBoxesPrio(CollideHandle h,int chunkNo,
    int nBox,const bbox3d *boxes,const int *prio)
{
  CProxy_collideMgr mgr(h);
  mgr.ckLocalBranch()->contribute(chunkNo,nBox,boxes,prio);
}

/******************** objListMsg **********************/
objListMsg::objListMsg(
    int n_,CollideObjRec *obj_,
    const returnReceipt &receipt_)
  :isHeapAllocated(true),receipt(receipt_),
  n(n_),obj(obj_)
{}

void objListMsg::freeHeapAllocated()
{
  if (!isHeapAllocated) return;
  free(obj);obj=NULL;
}

//This gives the byte offsets & sizes needed by each field
#define objListMsg_OFFSETS \
  int offM=0; \
int cntM=sizeof(objListMsg); \
int offB=offM+ALIGN8(cntM); \
int cntB=sizeof(CollideObjRec)*m->n;\

void *objListMsg::pack(objListMsg *m)
{
  objListMsg_OFFSETS
    int offEnd=offB+cntB;
  //Allocate a new message buffer and copy our fields into it
  void *buf = CkAllocBuffer(m, offEnd);
  char *cbuf=(char *)buf;
  memcpy(cbuf+offM,m,cntM);
  memcpy(cbuf+offB,m->obj,cntB);
  delete m;
  return buf;
}

objListMsg *objListMsg::unpack(void *buf)
{
  //Unpack ourselves in-place from the buffer; with
  // our arrays pointing into the allocated message data.
  objListMsg *m = new (buf) objListMsg;
  char *cbuf=(char *)buf;
  objListMsg_OFFSETS
    m->obj=(CollideObjRec *)(cbuf+offB);
  return m;
}

/*************** hashCache ***************
  Speeds up hashtable lookup via caching.
  */
#if 0 //A 2-element cache is actually slower than the 1-element case below
//Hashtable cache
template <int n>
class hashCache {
  typedef CollideLoc3d KEY;
  typedef cellAggregator *OBJ;
  KEY keys[n];
  OBJ objs[n];
  int lastFound;
  public:
  hashCache(const KEY &invalidKey) {
    for (int i=0;i<n;i++) keys[i]=invalidKey;
    lastFound=0;
  }
  inline OBJ lookup(const KEY &k) {
    if (k.compare(keys[lastFound]))
      return objs[lastFound];
    for (int i=0;i<n;i++)
      if (i!=lastFound)
        if (k.compare(keys[i]))
        {
          lastFound=i;
          return objs[i];
        }
    return OBJ(0);
  }
  void add(const KEY &k,const OBJ &o) {
    int doomed=lastFound+1;
    if (doomed>=n) doomed-=n;
    keys[doomed]=k;
    objs[doomed]=o;
  }
};
#endif

//Specialization of above for n==1
//  (partial specialization isn't well supported, so this is manual.)
template <class KEY,class OBJ>
class hashCache1 {
  KEY key;
  OBJ obj;
  public:
  hashCache1(const KEY &invalidKey) {
    key=invalidKey;
  }
  inline OBJ lookup(const KEY &k) {
    if (k.compare(key)) return obj;
    else return OBJ(0);
  }
  inline void add(const KEY &k,const OBJ &o) {
    key=k;
    obj=o;
  }
};

/************* voxelAggregator ***********
  Accumulates lists of objects for one voxel until
  there are enough to send off.  Private class of CollisionAggregator.
  */
class voxelAggregator {
  private:
    //Accumulates objects for the current message
    growableBufferT<CollideObjRec> obj;
    CollideLoc3d destination;
    collideMgr *mgr;
  public:
    voxelAggregator(const CollideLoc3d &dest,collideMgr *mgr_)
      :destination(dest),mgr(mgr_) { }

    //Add this object to the packList
    inline void add(const CollideObjRec &o) {
      obj.push_back(o);
    }

    //Send off all accumulated objects.
    void send(void);
};

//Send off any accumulated triangles.
void voxelAggregator::send(void) {
  if (obj.length()>0) {
    //Detach accumulated data and send it off
    CollideObjRec *o=obj.getData(); int n=obj.length();
    obj.detachBuffer();
    mgr->sendVoxelMessage(destination,n,o);
  }
}


/************* CollisionAggregator ***************
  Receives lists of points and triangles from the sources
  on a particular machine.  Determines which voxels each
  triangle spans, and adds the triangle to each voxelAggregator.
  Maintains a sparse hashtable voxels.
  */
CollisionAggregator::CollisionAggregator(const CollideGrid3d &gridMap_,collideMgr *mgr_)
  :gridMap(gridMap_),voxels(17,0.25),mgr(mgr_)
{}
CollisionAggregator::~CollisionAggregator()
{
  compact();
}

//Add a new accumulator to the hashtable
voxelAggregator *CollisionAggregator::addAccum(const CollideLoc3d &dest)
{
  voxelAggregator *ret=new voxelAggregator(dest,mgr);
  voxels.put(dest)=ret;
  return ret;
}

//Add this chunk's triangles
void CollisionAggregator::aggregate(int pe, int chunk, int n,
    const bbox3d *boxes, const int *prio)
{
  hashCache1<CollideLoc3d,voxelAggregator *>
    cache(CollideLoc3d(-1000000000,-1000000000,-1000000000));

  //Add each object to its corresponding voxelAggregators
  for (int i=0;i<n;i++) {
    //Compute bbox. and location
    const bbox3d &bbox=boxes[i];
    int oPrio=chunk;
    if (prio!=NULL) oPrio=prio[i];
    CollideObjRec obj(CollideObjID(chunk,i,oPrio,pe),bbox);

    iSeg1d sx(gridMap.world2grid(0,bbox.axis(0))),
           sy(gridMap.world2grid(1,bbox.axis(1))),
           sz(gridMap.world2grid(2,bbox.axis(2)));
    STATS(objects++)
      STATS(gridSizes[0]+=sx.getMax()-sx.getMin())
      STATS(gridSizes[1]+=sy.getMax()-sy.getMin())
      STATS(gridSizes[2]+=sz.getMax()-sz.getMin())

      //Loop over all grid voxels touched by this object
      CollideLoc3d g;
    g.z=sz.getMin();
    do {
      g.y=sy.getMin();
      do {
        g.x=sx.getMin();
        do {
          voxelAggregator *c=cache.lookup(g);
          if (c==NULL) { /* First object for this voxel: add record */
            c=voxels.get(g);
            if (c==NULL) c=addAccum(g);
            cache.add(g,c);
          }
          c->add(obj);
        } while (++g.x<sx.getMax());
      } while (++g.y<sy.getMax());
    } while (++g.z<sz.getMax());
  }
}

//Send off all accumulated messages
void CollisionAggregator::send(void)
{
  CkHashtableIterator *it=voxels.iterator();
  void *c;
  while (NULL!=(c=it->next())) (*(voxelAggregator **)c)->send();
  delete it;
}

//Delete all cached accumulators
void CollisionAggregator::compact(void)
{
  CkHashtableIterator *it=voxels.iterator();
  void *c;
  while (NULL!=(c=it->next())) delete *(voxelAggregator **)c;
  delete it;
  voxels.empty();
}

/********************* syncReductionMgr ******************/
syncReductionMgr::syncReductionMgr()
  :thisproxy(thisgroup)
{
  stepCount=-1;
  stepFinished=true;
  localFinished=false;

  //Set up the reduction tree
  onPE=CkMyPe();
  if (onPE==0) treeParent=-1;
  else treeParent=(onPE-1)/TREE_WID;
  treeChildStart=(onPE*TREE_WID)+1;
  treeChildEnd=treeChildStart+TREE_WID;
  if (treeChildStart>CkNumPes()) treeChildStart=CkNumPes();
  if (treeChildEnd>CkNumPes()) treeChildEnd=CkNumPes();
  nChildren=treeChildEnd-treeChildStart;
}
void syncReductionMgr::startStep(int stepNo,bool withProd)
{
  SRM_STATUS("syncReductionMgr::startStep");
  if (stepNo<1+stepCount) return;//Already started
  if (stepNo>1+stepCount) CkAbort("Tried to start SRMgr step from future\n");
  stepCount++;
  stepFinished=false;
  localFinished=false;
  childrenCount=0;
  if (nChildren>0)
    for (int i=0;i<TREE_WID;i++)
      if (treeChildStart+i<CkNumPes())
        thisproxy[treeChildStart+i].childProd(stepCount);
  if (withProd)
    pleaseAdvance();//Advise subclass to advance
}

void syncReductionMgr::advance(void)
{
  SRM_STATUS("syncReductionMgr::advance");
  if (stepFinished) startStep(stepCount+1,false);
  localFinished=true;
  tryFinish();
}

void syncReductionMgr::pleaseAdvance(void)
{ /*Child advisory only*/ }

//This is called on PE 0 once the reduction is finished
void syncReductionMgr::reductionFinished(void)
{ /*Child use only */ }

void syncReductionMgr::tryFinish(void) //Try to finish reduction
{
  SRM_STATUS("syncReductionMgr::tryFinish");
  if (localFinished && (!stepFinished) && childrenCount==nChildren)
  {
    stepFinished=true;
    if (treeParent!=-1)
      thisproxy[treeParent].childDone(stepCount);
    else
      reductionFinished();
  }
}
//Called by parent-- will you contribute?
void syncReductionMgr::childProd(int stepCount)
{
  SRM_STATUS("syncReductionMgr::childProd");
  if (stepFinished) startStep(stepCount,true);
  tryFinish();
}
//Called by tree children-- me and my children are finished
void syncReductionMgr::childDone(int stepCount)
{
  SRM_STATUS("syncReductionMgr::childDone");
  if (stepFinished) startStep(stepCount,true);
  childrenCount++;
  tryFinish();
}


/*********************** collideMgr ***********************/
//Extract the (signed) low 23 bits of src--
// this is the IEEE floating-point mantissa used as a grid index
static int low23(unsigned int src)
{
  unsigned int loMask=0x007fFFffu;//Low 23 bits set
  unsigned int offset=0x00400000u;
  return (src&loMask)-offset;
}
static const char * voxName(int ix,int iy,int iz,char *buf) {
  int x=low23(ix);
  int y=low23(iy);
  int z=low23(iz);
  sprintf(buf,"(%d,%d,%d)",x,y,z);
  return buf;
}
static const char * voxName(const CkIndex3D &idx,char *buf) {
  return voxName(idx.x,idx.y,idx.z,buf);
}


collideMgr::collideMgr(const CollideGrid3d &gridMap_,
    const CProxy_collideClient &client_,
    const CProxy_collideVoxel &voxels,
    const collideStats &statObj_)
  :thisproxy(thisgroup), voxelProxy(voxels),
  gridMap(gridMap_), client(client_), aggregator(gridMap,this), statObj(statObj_)
{
  steps=0;
  nContrib=0;
  contribCount=0;
  msgsSent=msgsRecvd=0;
}

//Maintain contributor registration count
void collideMgr::registerContributor(int chunkNo)
{
  nContrib++;
  CM_STATUS("Contributor register: now "<<nContrib);
}
void collideMgr::unregisterContributor(int chunkNo)
{
  nContrib--;
  CM_STATUS("Contributor unregister: now "<<nContrib);
}

//Clients call this to contribute their triangle lists
void collideMgr::contribute(int chunkNo,
    int n,const bbox3d *boxes,const int *prio)
{
  //printf("[%d] Receiving contribution from %d\n",CkMyPe(), chunkNo);
  CM_STATUS("collideMgr::contribute "<<n<<" boxes from "<<chunkNo);
  aggregator.aggregate(CkMyPe(),chunkNo,n,boxes,prio);
  aggregator.send(); //Deliver all outgoing messages
  if (++contribCount==nContrib) { //That's everybody
    //aggregator.send(); //Deliver all outgoing messages
    //if (getStepCount()%8==7)
    aggregator.compact();//Blow away all the old voxels (saves memory)
    tryAdvance();
  }
  //printf("[%d] DONE receiving contribution from %d\n",CkMyPe(), chunkNo);
}

inline CkArrayIndex3D buildIndex(const CollideLoc3d &l)
{return CkArrayIndex3D(l.x,l.y,l.z);}

//voxelAggregators deliver messages to voxels via this bottleneck
void collideMgr::sendVoxelMessage(const CollideLoc3d &dest,
    int n,CollideObjRec *obj)
{
  char destName[200];
  CM_STATUS("collideMgr::sendVoxelMessage to "<<voxName(dest.x,dest.y,dest.z,destName));
  msgsSent++;
  objListMsg *msg=new objListMsg(n,obj,
      objListMsg::returnReceipt(thisgroup,CkMyPe()));
  voxelProxy[buildIndex(dest)].add(msg);
}

void objListMsg::returnReceipt::send(void)
{
  CProxy_collideMgr p(gid);
  if (onPE==CkMyPe()) //Just send directly to the local branch
    p.ckLocalBranch()->voxelMessageRecvd();
  else //Deliver via the network
    p[onPE].voxelMessageRecvd();
}

//collideVoxels send a return receipt here
void collideMgr::voxelMessageRecvd(void)
{
  msgsRecvd++;
  CM_STATUS("collideMgr::voxelMessageRecvd: "<<msgsRecvd<<" of "<<msgsSent);
  //All the voxels we send messages to have received them--
  tryAdvance();
}

//Check if we're barren-- if so, advance now
void collideMgr::pleaseAdvance(void)
{
  CM_STATUS("collideMgr::pleaseAdvance");
  tryAdvance();
}

//Attempt to finish the voxel send/recv step
void collideMgr::tryAdvance(void)
{
  CM_STATUS("tryAdvance: "<<nContrib-contribCount<<" contrib, "<<msgsSent-msgsRecvd<<" msg")
    if ((contribCount==nContrib) && (msgsSent==msgsRecvd)) {
      CM_STATUS("advancing");
      advance();
      steps++;
      contribCount=0;
      msgsSent=msgsRecvd=0;
    }
}

//This is called on PE 0 once the voxel send/recv reduction is finished
void collideMgr::reductionFinished(void)
{
  CM_STATUS("collideMgr::reductionFinished");
  //Broadcast Collision start:
  voxelProxy.startCollision(steps,gridMap,client,statObj);
}


/********************** collideVoxel *********************/

iSeg1d collideVoxel_extents[3]={
  iSeg1d(100,-100),iSeg1d(100,-100),iSeg1d(100,-100)};


//Print debugging state information
void collideVoxel::status(const char *msg)
{
  int x=low23(thisIndex.x);
  int y=low23(thisIndex.y);
  int z=low23(thisIndex.z);
  CkPrintf("Pe %d, voxel (%d,%d,%d)> %s\n",CkMyPe(),x,y,z,msg);
}
void collideVoxel::emptyMessages()
{
  for (int i=0;i<msgs.length();i++) delete msgs[i];
  msgs.length()=0;
}

/* CollideVoxel is created using [createhere], so
   its constructor can't take any arguments: */
collideVoxel::collideVoxel(void)
{
  CC_STATUS("created");
  collideVoxel_extents[0].add(low23(thisIndex.x));
  collideVoxel_extents[1].add(low23(thisIndex.y));
  collideVoxel_extents[2].add(low23(thisIndex.z));
}
collideVoxel::collideVoxel(CkMigrateMessage *m)
{
  CC_STATUS("arrived from migration");
}
collideVoxel::~collideVoxel()
{
  emptyMessages();
  CC_STATUS("deleted. (migration depart)");
}
void collideVoxel::pup(PUP::er &p) {
  if (msgs.length()!=0) {
    status("Error!  Cannot migrate voxels with messages in tow!\n");
    CkAbort("collideVoxel::pup cannot handle message case");
  }
}
void collideVoxel::add(objListMsg *msg)
{
  CC_STATUS("add message from "<<msg->getSource());
  msg->sendReceipt();
  msgs.push_back(msg);
}

void collideVoxel::collide(const bbox3d &territory,CollisionList &dest)
{
  int m;

#if 0  //Check if all the priorities are identical-- early exit if so
  CC_STATUS("      early-exit (all prio. identical) test");
  int firstPrio=msgs[0]->tri(0).id.prio;
  bool allIdentical=true;
  for (m=0;allIdentical && m<msgs.length();m++)
    for (int i=0;i<msgs[m]->getNtris();i++)
      if (msgs[m]->tri(i).id.prio!=firstPrio)
      { allIdentical=false; break;}
  if (allIdentical) {CC_STATUS("      early-exit used!");return;}
#endif
  //Figure out how many objects we have total
  int n=0;
  for (m=0;m<msgs.length();m++) n+=msgs[m]->getObjects();
  CollideOctant o(n,territory);
  o.length()=0;
  bbox3d big;big.infinity();
  o.setBbox(big);

  CC_STATUS("      creating bbox, etc. for polys");
#if COLLIDE_TRACE
  bbox3d oBox; oBox.empty();
#endif
  //Create records for each referenced poly
  for (m=0;m<msgs.length();m++) {
    const objListMsg &msg=*(msgs[m]);
    for (int i=0;i<msg.getObjects();i++) {
      o.push_fast(&msg.getObj(i));
#if COLLIDE_TRACE
      oBox.add(msg.getObj(i).getBbox());
#endif
    }
  }
  o.markHome(o.length());
  COL_STATUS("    colliding polys");
  COL_STATUS("\t\t\tXXX "<<o.length()<<" polys, "<<msgs.length()<<" msgs")
#if COLLIDE_TRACE
    territory.print("Voxel territory: ");
  oBox.print(" Voxel objects: ");
  CkPrintf("\n");
#endif
  o.findCollisions(dest);
}


void collideVoxel::startCollision(int step,
    const CollideGrid3d &gridMap,
    const CProxy_collideClient &client,
    const collideStats &statObj)
{
  CC_STATUS("startCollision "<<step<<" on "<<msgs.length()<<" messages {");

  if(statObj.printVoxCount) {
    int myCount = 1;
    CkCallback countCb(CkReductionTarget(statsCollector, voxelCountDone), statObj.statsCollProxy);
    contribute(sizeof(int), &myCount, CkReduction::sum_int, countCb);
  }

  bbox3d territory(gridMap.grid2world(0,rSeg1d(thisIndex.x,thisIndex.x+1)),
      gridMap.grid2world(1,rSeg1d(thisIndex.y,thisIndex.y+1)),
      gridMap.grid2world(2,rSeg1d(thisIndex.z,thisIndex.z+1))
      );
  CollisionList colls;
  collide(territory,colls);
  client.ckLocalBranch()->collisions(this,step,colls);

  emptyMessages();
  CC_STATUS("} startCollision");
}

collideClient::~collideClient() {}

/********************** serialCollideClient *****************/
serialCollideClient::serialCollideClient(void) {
  clientFn=NULL;
  clientParam=NULL;
  clientCb = CkCallback(CkCallback::ignore);
  useCb = false;
}

serialCollideClient::serialCollideClient(CkCallback clientCb_) {
  clientFn=NULL;
  clientParam=NULL;
  clientCb = clientCb_;
  useCb = true;
}

/// Call this client function on processor 0:
void serialCollideClient::setClient(CollisionClientFn clientFn_,void *clientParam_) {
  clientFn=clientFn_;
  clientParam=clientParam_;
}

void serialCollideClient::collisions(ArrayElement *src,
    int step,CollisionList &colls)
{
  if(useCb) { // Use user passed callback as reduction target
    src->contribute(colls.length()*sizeof(Collision),colls.getData(),
      CkReduction::concat, clientCb);
  } else { // Use client fn with reduction targetted at serialCollideClient::reductionDone
    CkCallback cb(CkIndex_serialCollideClient::reductionDone(0),0,thisgroup);
    src->contribute(colls.length()*sizeof(Collision),colls.getData(),
      CkReduction::concat,cb);
  }
}

//This is called after the collideVoxel Collision reduction completes
void serialCollideClient::reductionDone(CkReductionMsg *msg)
{
  int nColl=msg->getSize()/sizeof(Collision);
  CM_STATUS("serialCollideClient with "<<nColl<<" collisions");
  if (clientFn!=NULL) //User wants Collisions on node 0
    (clientFn)(clientParam,nColl,(Collision *)msg->getData());
  else //FIXME: never registered a client
    CkAbort("Forgot to call serialCollideClient::setClient!\n");
  delete msg;
}



#include "collidecharm.def.h"
