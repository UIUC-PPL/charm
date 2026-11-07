#ifndef __TREE_NODE_H__
#define __TREE_NODE_H__

#include "MultipoleMoments.h"
#include "OrientedBox.h"
#include "Particle.h"
#include "util.h"
#include "Descriptor.h"

#include <stack>
using namespace std;

enum NodeType {
  Invalid = 0,

  Internal,
  Bucket,
  EmptyBucket,

  Boundary,

  Remote,
  RemoteBucket,
  RemoteEmptyBucket
};

extern string NodeTypeString[];

struct NodeCore {
  Key key;
  NodeType type;

  int depth;
  Particle *particleStart;
  int numParticles;
  int numChildren;

  // tps [ownerStart,ownerEnd] own this node
  int ownerStart;
  int ownerEnd;

  bool cached;

  NodeCore(){
    particleStart = NULL;
    numParticles = 0;
    numChildren = 0;
    type = Invalid;
    depth = -1;
    ownerStart = -1;
    ownerEnd = -1;
    cached = false;
  }

  NodeCore(Key k, int d, Particle *p, int np) : 
    key(k), depth(d), 
    particleStart(p), numParticles(np),
    type(Invalid), numChildren(0),
    ownerStart(-1), ownerEnd(-1), cached(false)
  {
  }
};

template<typename T>
class Node {

  NodeCore core;
  // all children are stored together
  Node<T> *children;
  Node<T> *parent;

  int numChildrenMomentsReady;

  public:
  T data;
  Node(Key k, int d, Particle *p, int np, Node<T> *par=NULL) : 
    core(k,d,p,np), parent(par), children(NULL), data(),
   numChildrenMomentsReady(0)
  {
  }

  Node() : 
    core(), parent(NULL), children(NULL), data(),
   numChildrenMomentsReady(0)
  {
  }

  static Key getParticleLevelKey(const Node<T> *node){
    Key k = node->getKey();
    int depth = node->getDepth();

    return (k<<(TREE_KEY_BITS-(LOG_BRANCH_FACTOR*depth+1)));
  }

  static Key getLastParticleLevelKey(const Node<T> *node){
    Key k = node->getKey();
    int depth = node->getDepth();

    int nshift = TREE_KEY_BITS-(LOG_BRANCH_FACTOR*depth+1);
    k <<= nshift;
    Key mask = (Key(1) << nshift);
    mask--;
    k |= mask;
    return k;
  }

  static int getDepthFromKey(Key k){
    int nUsedBits = mssb64_pos(k);
    return (nUsedBits/LOG_BRANCH_FACTOR);
  }

  int getNumChildren() const {
    return core.numChildren;
  }

  void setChildren(Node<T> *ch, int n) {
    children = ch;
    core.numChildren = n;
  }

  Node<T> *getChildren() const { return children; }
  Node<T> *getChild(int i) const { return children+i; }
  int getNumParticles() const { return core.numParticles; }
  Particle *getParticles() const { return core.particleStart; }
  Key getKey() const { return core.key; }
  int getDepth() const { return core.depth; }
  int getOwnerStart() const { return core.ownerStart; }
  int getOwnerEnd() const { return core.ownerEnd; }
  NodeType getType() const { return core.type; }
  Node<T> *getParent() const { return parent; }
  void setKey(Key &k){ core.key = k; }
  void setDepth(int d){ core.depth = d; }
  void setType(NodeType t){ core.type = t; }
  void setParent(Node<T> *par){ parent = par; }

  void setOwners(int lb, int ub){
    core.ownerStart = lb;
    core.ownerEnd = ub;
  }

  void setOwnerStart(int lb){ core.ownerStart = lb; }
  void setOwnerEnd(int ub){ core.ownerEnd = ub; }

  void setParticles(Particle *p, int n){
    core.particleStart = p;
    core.numParticles = n;
  }

  void childMomentsReady(){ numChildrenMomentsReady++; }
  int getNumChildrenMomentsReady() const { return numChildrenMomentsReady; }
  bool allChildrenMomentsReady() const { return numChildrenMomentsReady == core.numChildren; }

  void getOwnershipFromChildren(){
    // set node type from children types
    int numChildren = core.numChildren;
    bool allChildrenExternal = true;
    for(int i = 0; i < numChildren; i++){
      NodeType c_type = children[i].getType();
      if(c_type != Remote 
          && c_type != RemoteBucket 
          && c_type != RemoteEmptyBucket){
        allChildrenExternal = false;
      }
      if(i == 0) continue;
      int diff = children[i].getOwnerStart()
        - children[i-1].getOwnerEnd(); 
      CkAssert(diff >= 0 && diff <= 1);
    }

    // since we invoke this method only when updating
    // the local tree after moments exchange, the
    // nodes we encounter here can only be Remote/RemoteBucket/RemoteEmptyBucket
    // or Boundary
    if(allChildrenExternal) setType(Remote);
    else setType(Boundary);

    core.ownerStart = children[0].getOwnerStart();
    core.ownerEnd = children[numChildren-1].getOwnerEnd();

  }

  void fullyDelete(){
    if(core.numChildren == 0){
      delete this;
      return;
    }

    for(int i = 0; i < core.numChildren; i++){
      children[i].fullyDelete();
    }
  }

  void refine(){
    CkAssert(core.numChildren == 0);
    int numRankBits = LOG_BRANCH_FACTOR;
    int depth = getDepth();
    CkAssert(depth >= 0);
    CkAssert(depth < ((TREE_KEY_BITS-1)/numRankBits));

    core.numChildren = BRANCH_FACTOR;
    children = new Node<T>[BRANCH_FACTOR];

    Key myKey = getKey();

    int start = 0;
    int end = getNumParticles();
    Particle *particles = getParticles();
    int splitters[BRANCH_FACTOR+1];

    Key childKey = (myKey << numRankBits);
    int childDepth = depth+1;

    findSplitters(particles,start,end,splitters,childKey,childDepth);

    for(int i = 0; i < BRANCH_FACTOR; i++){
      initChild(i,splitters,childKey,childDepth);
      childKey++;
    }
  }

  void initChild(int i, int *splitters, Key childKey, int childDepth){
    Node<T> *child = children+i;
    int childPartStart = splitters[i]; 
    int childPartEnd = splitters[i+1]; 
    int childNumParticles = childPartEnd-childPartStart;

    // set child's key and depth
    child->setKey(childKey);
    child->setDepth(childDepth);
    child->setParent(this);

    // findSplitters distributed particles over
    // different children
    child->core.particleStart = core.particleStart+childPartStart;
    child->core.numParticles = childNumParticles;
    child->core.numChildren = 0;
  }

  void printBoundingBox(ostream &os){
    os << getKey() << " bb ";
    os << data.box.lesser_corner.x << " ";
    os << data.box.lesser_corner.y << " ";
    os << data.box.lesser_corner.z << " ";
    os << data.box.greater_corner.x << " ";
    os << data.box.greater_corner.y << " ";
    os << data.box.greater_corner.z << endl;

    for(int i = 0; i < getNumChildren(); i++){
      getChild(i)->printBoundingBox(os);
    }
  }

  void serialize(Node<T> *placeInBuf, Node<T> *&emptyBuf, int subtreeDepth){
    CmiUInt8 childOffset = emptyBuf-placeInBuf;
    if(subtreeDepth == 0){
      // if subtreeDepth is 0 and placeInBuf is NULL, then
      // we have been asked to place all of the root's
      // successors up to depth 0, but not the root itself.
      // this is a nonsensical request.
      CkAssert(placeInBuf != NULL);
      // my children are not to be included
      placeInBuf->setChildren((Node<T>*)childOffset,0);
      return;
    }

    if(placeInBuf != NULL){
      // we exclude the root from the buffer 
      placeInBuf->setChildren((Node<T>*)childOffset,getNumChildren());
    }

    // copy children into empty portion of buffer
    memcpy((void*)emptyBuf,(void*)getChildren(),sizeof(Node<T>)*getNumChildren());
    if(placeInBuf != NULL){
      Node<T> *childInBuf = emptyBuf;
      for(int i = 0; i < getNumChildren(); i++){
        childInBuf->setParent((Node<T>*)(placeInBuf-childInBuf));
        childInBuf++;
      }
    }
    // mark the start of children
    placeInBuf = emptyBuf;
    // we have used up a bit of the empty space in the buffer
    emptyBuf += getNumChildren();

    // invoke serialize on each of the children in turn
    for(int i = 0; i < getNumChildren(); i++){
      getChild(i)->serialize(placeInBuf,emptyBuf,subtreeDepth-1);
      // move to space reserved for next child in buffer
      placeInBuf++;
    }
  }

  void deserialize(Node<T> *start, int nn){
    setChildren(start,BRANCH_FACTOR); 
    Node<T> *buf = start;
    for(int i = 0; i < nn; i++){
      if(i < BRANCH_FACTOR){
        buf->setParent(this);
      }
      else{
        CmiUInt8 parentOffset = (CmiUInt8)(buf->getParent());
        buf->setParent(buf+parentOffset);
      }
      CmiUInt8 childrenOffset = (CmiUInt8)(buf->getChildren());
      buf->setChildren(buf+childrenOffset,buf->getNumChildren());

      buf->setParticles(NULL,0);
      buf->setType(makeRemote(buf->getType()));
      buf->setCached();

      buf++;
    }
  }

  void setCached(){ core.cached = true; }
  bool isCached(){ return core.cached; }

  static NodeType makeRemote(NodeType type){
    switch(type){
      case Boundary : 
      case Internal : return Remote;
      case Bucket : return RemoteBucket;
      case EmptyBucket : return RemoteEmptyBucket;

      case Remote :
      case RemoteBucket :
      case RemoteEmptyBucket : return type;

      default: CkAbort("bad type!\n");
    }
  }

  void getMomentsFromChildren(){
    OrientedBox<Real> &bb = data.box;
    Real &mass = data.moments.totalMass;
    Vector3D<Real> &cm = data.moments.cm;

    for(Node<ForceData> *c = getChildren(); c != getChildren()+getNumChildren(); c++){
      MultipoleMoments &childMoments = c->data.moments;
      mass += childMoments.totalMass;
      cm += childMoments.totalMass*childMoments.cm;
      bb.grow(c->data.box);
    }
    if(mass > 0.0) cm /= mass;

    Vector3D<Real> delta1 = data.moments.cm - data.box.lesser_corner;	
    Vector3D<Real> delta2 = data.box.greater_corner - data.moments.cm;
    delta1.x = (delta1.x > delta2.x ? delta1.x : delta2.x);
    delta1.y = (delta1.y > delta2.y ? delta1.y : delta2.y);
    delta1.z = (delta1.z > delta2.z ? delta1.z : delta2.z);
    data.moments.rsq = delta1.lengthSquared();
  }

  void getMomentsFromParticles(){
    OrientedBox<Real> &bb = data.box;
    Real &mass = data.moments.totalMass;
    Vector3D<Real> &cm = data.moments.cm;

    Particle *p;
    for(p = getParticles(); p != getParticles()+getNumParticles(); p++) {
      mass += p->mass;
      cm += p->mass*p->position;
      bb.grow(p->position);
    }
    if(mass > 0.0) cm /= mass;

    Real d;
    data.moments.rsq = 0;
    for(p = getParticles(); p != getParticles()+getNumParticles(); p++) {
      d = (cm - p->position).lengthSquared();
      if(d > data.moments.rsq) data.moments.rsq = d;
    }

  }

};

template<typename T>
ostream &operator<<(ostream &os, const Node<T> &node){
  ostringstream oss;
  oss << hex << &node;
  os << node.getKey() 
     << " [label=\"" << node.getKey() 
     << " (" << node.getNumParticles() << ","
    << "\\n" << node.data.moments.rsq << ","
     << "\\n" << node.getOwnerStart() << ":" 
     << node.getOwnerEnd() << ")\\n"
     << NodeTypeString[node.getType()]
     << "\"";

  if(node.getType() == Internal) os << ", color=\"darkolivegreen3\"";
  else if(node.getType() == Boundary) os << ", color=\"darkgoldenrod1\"";
  else if(node.getType() == Bucket) os << ", color=\"chartreuse\"";
  else if(node.getType() == EmptyBucket) os << ", color=\"coral\"";
  else if(node.getType() == Remote) os << ", color=\"aquamarine3\"";
  else if(node.getType() == RemoteBucket) os << ", color=\"aquamarine1\"";
  else if(node.getType() == Invalid) os << ", color=\"firebrick1\"";

  os << "]" << endl;

  for(int i = 0; i < node.getNumChildren(); i++){
    Node<T> &childref = *(node.getChild(i));
    os << node.getKey() << " -> " << childref.getKey() << endl;
    os << childref;
  }

  return os;
}

struct MomentsExchangeStruct {
  MultipoleMoments moments;
  OrientedBox<Real> box;
  Key key;
  NodeType type;

  MomentsExchangeStruct &operator=(const Node<ForceData> &node){
    moments = node.data.moments;
    box = node.data.box;
    key = node.getKey();
    type = node.getType();

    return *this;
  }

};



#endif
