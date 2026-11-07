#include "charm++.h"
#include "OrientedBox.h"
#include "defines.h"
#include "ActiveBinInfo.h"

CkReduction::reducerType BoundingBoxGrowReductionType;
CkReduction::reducerType NodeDescriptorReductionType;
CkReduction::reducerType DtReductionType;

CkReductionMsg *BoundingBoxGrowReduction(int nmsgs, CkReductionMsg **msgs){
  BoundingBox &bb = *((BoundingBox *) msgs[0]->getData());
  for(int i = 1; i < nmsgs; i++){
    CkAssert(msgs[i]->getSize() == sizeof(BoundingBox));
    BoundingBox &other = *((BoundingBox *) msgs[i]->getData());
    bb.grow(other);
  }
  return CkReductionMsg::buildNew(sizeof(BoundingBox),&bb);
}

CkReductionMsg *NodeDescriptorReduction(int nmsgs, CkReductionMsg **msgs){
  int numElements = (msgs[0]->getSize()/sizeof(NodeDescriptor));

  for(int i = 0; i < numElements; i++){
    NodeDescriptor *zeroth = ((NodeDescriptor*)msgs[0]->getData())+i;
    for(int j = 1; j < nmsgs; j++){
      const NodeDescriptor &source = *(((NodeDescriptor *)msgs[j]->getData())+i);
      zeroth->grow(source);
    }
  }
  return CkReductionMsg::buildNew(msgs[0]->getSize(),msgs[0]->getData());
}

CkReductionMsg *DtReduction(int nmsgs, CkReductionMsg **msgs){
  int numElements = (msgs[0]->getSize()/sizeof(DtReductionStruct));

  for(int i = 0; i < numElements; i++){
    DtReductionStruct *zeroth = ((DtReductionStruct*)(msgs[0]->getData()))+i;
    for(int j = 1; j < nmsgs; j++){
      const DtReductionStruct *source = ((const DtReductionStruct *)(msgs[j]->getData()))+i;
      *zeroth += *source;
    }
  }
  return CkReductionMsg::buildNew(msgs[0]->getSize(),msgs[0]->getData());
}

void registerReducers(){
  BoundingBoxGrowReductionType = CkReduction::addReducer(BoundingBoxGrowReduction);
  NodeDescriptorReductionType = CkReduction::addReducer(NodeDescriptorReduction);
  DtReductionType = CkReduction::addReducer(DtReduction);
}
