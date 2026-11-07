#ifndef QUADINDEX_H
#define QUADINDEX_H

#include <assert.h>

using std::string;

class OctIndex{
public:
  int nbits;// number of bits in the index
  unsigned int bitVector;// bit vector storing the index bits
        
public:
  OctIndex():bitVector(0), nbits(0){}
  OctIndex(const OctIndex& qindex):bitVector(qindex.bitVector), nbits(qindex.nbits){}
  OctIndex(int _bitVector, int _nbits):bitVector(_bitVector), nbits(_nbits) {
    assert(nbits < numBitsForStorage);
  }
  OctIndex(int x, int y, int z, int depth);

  void operator = (const OctIndex& qidx) {
    nbits = qidx.nbits;
    bitVector = qidx.bitVector;
  }

  bool operator < (const OctIndex& other) const {
    if (nbits != other.nbits) return nbits < other.nbits;
    return bitVector < other.bitVector;
  }

  bool operator != (OctIndex qidx) const { return !(*this == qidx); }
    
  bool operator == (OctIndex qidx) const {
    return (nbits==qidx.nbits && getIndexString() == qidx.getIndexString())? true: false;
  }

  void getCoordinates(int&, int&, int&)const;
  std::string getIndexString() const;
  inline int getDepth() const{ return nbits/numDims; }

  std::vector<OctIndex> getNeighbors();
  OctIndex getNeighbor(int) const;
  OctIndex getParent() const;

  OctIndex getChild(unsigned int) const;
  int getOctant() const;
    
  unsigned int getBitVector() const{ return bitVector;}
  unsigned int getNbits() const{ return nbits;}

  void pup(PUP::er &p){
    p|nbits;
    p|bitVector;
  }

};

class CkArrayIndexOctIndex: public CkArrayIndex {
public:
  OctIndex *idx;
    
public:
  CkArrayIndexOctIndex(const OctIndex &in){
    idx = new(index) OctIndex(in);
    nInts = sizeof(in)/sizeof(int);
  }
  CkArrayIndexOctIndex(){
    idx = new(index) OctIndex();
    nInts = sizeof(OctIndex)/sizeof(int);
  }
    
  CkArrayIndexOctIndex &operator=(const CkArrayIndexOctIndex &that)  {
    nInts = that.nInts;
    assert(nInts == 2);
    memcpy(idx, that.idx, sizeof(int)*2);
    return *this;
  }
};

#endif
