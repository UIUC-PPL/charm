#ifndef __ACTIVE_BIN_INFO_H__
#define __ACTIVE_BIN_INFO_H__

#include "charm++.h"
#include "Node.h"
#include <utility>
#include "Descriptor.h"

template<typename T>
struct ActiveBinInfo{ 
  CkVec<std::pair<Node<T>*, bool> > *oldvec;
  CkVec<std::pair<Node<T>*, bool> > *newvec;
  CkVec<NodeDescriptor> counts;
  CkVec<Node<T>*> unrefined;


  ActiveBinInfo(){
    oldvec = new CkVec<std::pair<Node<T> *, bool> >();
    newvec = new CkVec<std::pair<Node<T> *, bool> >();
  }

  ~ActiveBinInfo(){
    delete oldvec;
    delete newvec;
  }

  // XXX - add a function "postChildPush" which is empty
  // in ownershipactive.. but does the pushing of node descriptor
  // in active..
  // in that case, we will have to use length of new vector 
  // to get number of fat nodes in while loop (can't use getNumCounts)
  void addNewNode(Node<T> *node){
    std::pair<Node<T>*,bool> pr;
    pr.first = node;
    pr.second = false;

    Key kfirst, klast;

    newvec->push_back(pr);
    int np = node->getNumParticles();
    Particle *particles = node->getParticles();
    if(np > 0){
      kfirst = particles[0].key;
      klast = particles[np-1].key;
    }else{
      kfirst = klast = Node<T>::getParticleLevelKey(node);
    }
    counts.push_back(NodeDescriptor(np,node->getKey(),kfirst,klast));
  }

  void processRefine(int *binsToRefine, int numBinsToRefine){
    for(int i = 0; i < numBinsToRefine; i++){
      int bin = binsToRefine[i];
      (*oldvec)[bin].second = true;
      Node<T> *node = (*oldvec)[bin].first;

      refine(node);

      Key kfirst, klast;

      std::pair<Node<T>*,bool> pr;
      pr.second = false;
      for(int i = 0; i < node->getNumChildren(); i++){
        Node<T> *child = node->getChild(i);
        pr.first = child;
        
        newvec->push_back(pr);
        int np = child->getNumParticles();
        Particle *particles = child->getParticles();
        if(np > 0){
          kfirst = particles[0].key;
          klast = particles[np-1].key;
        }else{
          kfirst = klast = Node<T>::getParticleLevelKey(child);
        }
        counts.push_back(NodeDescriptor(np,child->getKey(),kfirst,klast));
      }
    }

  }

  virtual void refine(Node<T> *node){
    node->refine();
  }

  void processEmpty(int *emptyBins, int nEmptyBins){
    for(int i = 0; i < nEmptyBins; i++){
      int bin = emptyBins[i];
      Node<T> *node = (*oldvec)[bin].first;
    }
  }

  CkVec<Node<T>*> &getUnrefined(){
    for(int i = 0; i < oldvec->length(); i++){
      Node<T> *node = (*oldvec)[i].first;
      bool refined = (*oldvec)[i].second;

      if(!refined){
        unrefined.push_back(node);
      }
    }

    return unrefined;
  }

  int getNumCounts(){
    return counts.length();
  }

  NodeDescriptor *getCounts(){
    return counts.getVec();
  }

  void reset(){
    CkVec<std::pair<Node<T>*, bool> > *tmp;
    tmp = oldvec;
    oldvec = newvec;
    newvec = tmp;
    newvec->length() = 0;

    unrefined.length() = 0;
    counts.length() = 0;
  }

  CkVec<std::pair<Node<T>*, bool> > *getActive(){
    return oldvec;
  }
};

bool CompareKeys(void *a, Key k);

template<typename T>
struct OwnershipActiveBinInfo : public ActiveBinInfo<T> {

  Key *owners;

  OwnershipActiveBinInfo(Key *o) : 
    owners(o)
  {
  }

  void refine(Node<T> *node){
    node->refine();

    int ownerStart = node->getOwnerStart();
    int ownerEnd = node->getOwnerEnd();
    int start = (ownerStart<<1);
    int end = (ownerEnd<<1)+2;

    Node<T> *children = node->getChildren();
    Key childKey = children[0].getKey();

    children[0].setOwnerStart(ownerStart);
    int childDepth = children[0].getDepth();
    childKey++;

    for(int i = 1; i < BRANCH_FACTOR; i++){
      Key testKey = (childKey << (TREE_KEY_BITS-(childDepth*LOG_BRANCH_FACTOR+1)));
      // in the keyRanges array, search for the first key
      // that is GE the key of this child node
      int owner_idx = binary_search_ge<Key>(testKey,owners,start,end,CompareKeys);
      // since each tree piece has two keys in the keyRanges
      // array (a lo and a hi), we divide by two to get the tp
      int tp_idx = (owner_idx>>1);

      // is the range of the tree piece contained completely
      // within the child node or does it straddle this child
      // and the previous one?
      if(EVEN(owner_idx)) children[i-1].setOwnerEnd(tp_idx-1);
      else                children[i-1].setOwnerEnd(tp_idx);

      if(children[i-1].getOwnerEnd() < children[i-1].getOwnerStart()){
        children[i-1].setOwners(-69,-69);
        children[i-1].setType(EmptyBucket);
      }

      children[i].setOwnerStart(tp_idx);

      start = owner_idx;

      childKey++;
    }

    children[BRANCH_FACTOR-1].setOwnerEnd(ownerEnd);
    if(children[BRANCH_FACTOR-1].getOwnerEnd() < children[BRANCH_FACTOR-1].getOwnerStart()){
      children[BRANCH_FACTOR-1].setOwners(-171,-171);
      children[BRANCH_FACTOR-1].setType(EmptyBucket);
    }
  }
};

#endif
