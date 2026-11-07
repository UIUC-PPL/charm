#include "Headers.h"
#include <assert.h>

using std::pow;

OctIndex::OctIndex(int x, int y, int z, int depth){ 
  bitVector = 0;
  nbits = numDims*depth;
  int vals[numDims] = {x, y, z};

  int index = numBitsForStorage - 1;
  assert(nbits <= index);
  for(int i=0; i<depth; i++){
    int range = 1 << depth-i;
    for (int d=0; d < numDims; ++d) {
      vals[d] %= range;
      unsigned bit = liesin(vals[d], 0, (range - 1)/2) ? 0 : 1;
      bitVector |= bit << index--;
    }
  }
}

void OctIndex::getCoordinates(int &x, int &y, int &z) const{
  unsigned vals[numDims] = {};
  for (int i = 0; i < nbits; ++i) {
    int d = i % numDims;
    unsigned index = numBitsForStorage - 1 - i;
    unsigned bit = (bitVector >> index) & 1U;
    vals[d] = (vals[d] << 1U) | bit;
  }

  // Export computed values
  x = vals[0];
  y = vals[1];
  z = vals[2];
}

// returns the Index String
std::string OctIndex::getIndexString() const{
  std::string str;

  for(int i=0; i<nbits; ++i)
    str += (bitVector & (1<<(numBitsForStorage - 1 - i)))> 0 ? '1' : '0';

  return str;
}

std::vector<OctIndex> OctIndex::getNeighbors() {
  std::vector<OctIndex> neighbors;
  for(int dir = UP; dir <= BACKWARD; dir++) {
    neighbors.push_back(getNeighbor(dir));
  }
  return neighbors;
}
    
OctIndex OctIndex::getNeighbor(int dir) const{
  int depth = nbits / numDims;
  int range = 1 << depth;
  int x, y, z;
  getCoordinates(x, y, z);
  int xc = x, yc = y, zc = z;
  switch(dir){
      case UP:      yc = (y+1)%range; break;
      case DOWN:    yc = (y==0)?(range-1):y-1; break;
      case LEFT:    xc = (x==0)?(range-1):x-1;  break;
      case RIGHT:   xc = (x+1)%range;           break;
      case FORWARD: zc = (z+1)%range; break;
      case BACKWARD: zc = (z==0)?(range-1):z-1; break;
      default: CkAbort("invald dir#");
  };
  return OctIndex(xc, yc, zc, depth);
}

OctIndex OctIndex::getParent() const {
  unsigned int shiftbits = numBitsForStorage - (nbits - numDims);
  unsigned int newbv = (bitVector >> shiftbits) << shiftbits;
  return OctIndex(newbv, nbits - numDims);
}

OctIndex OctIndex::getChild(unsigned int idx) const {
  return OctIndex(bitVector | (idx << (numBitsForStorage - nbits - numDims)),
                   nbits + numDims);
}

int OctIndex::getOctant() const{
  return (bitVector >> (numBitsForStorage - nbits)) & 7;
}
