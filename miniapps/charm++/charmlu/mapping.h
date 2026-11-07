#ifndef LU_MAPPING_H
#define LU_MAPPING_H

#include "lu.decl.h"
#include <string>

struct LUMap : public CkArrayMap {
  LUMap() {}
  virtual std::string desc() { return ""; }
  virtual int pesInPanel(CkIndex2D index) { return CkNumPes(); }
  int procNum(int arrayHdl, const CkArrayIndex &idx) { return 0; }
};

class BlockCyclicMap : public LUMap {
  int r, num_blocks;
public:
  BlockCyclicMap(int r_, int num_blocks_) : r(r_), num_blocks(num_blocks_) {}
  int procNum(int arrayHdl, const CkArrayIndex &idx) {
    int *coor = (int *)idx.data();
    int m = coor[1] * num_blocks + coor[0];
    return (m % (r * CkNumPes())) / r;
  }
};

#include <set>

// Implement a mapping that tiles a 2D processor tile in the 2D chare array
class PE2DTilingMap: public LUMap {
public:
  PE2DTilingMap(int _peRows, int _peCols, int _peRotate, int _peStride, int _numBlks)
    : peRows(_peRows), peCols(_peCols), peRotate(_peRotate), peStride(_peStride),
      numBlks(_numBlks) {
    CkAssert(peRows > 0 && peCols > 0);
  }

  int map(const int coor[2]) {
    int tileYIndex = coor[1]  / peCols;
    int XwithinPEtile = (coor[0] + tileYIndex * peRotate) % peRows;
    int YwithinPEtile = coor[1] % (peCols / peStride);
    int subtileY = (coor[1] % peCols) / (peCols / peStride);
    int peNum = XwithinPEtile * peStride + YwithinPEtile * peStride * peRows + subtileY;
    CkAssert(peNum < CkNumPes());
    return peNum;
  }

  int procNum(int arrayHdl, const CkArrayIndex &idx) {
    const int *coor = idx.data();
    return map(coor);
  }

  int pesInPanel(CkIndex2D index) {
    if (index.x < index.y)
      return peRows;
    else {
      std::set<int> pesInRow;
      int coor[2];

      coor[0] = index.x;
      for (int y = index.y+1; y < numBlks; ++y) {
	coor[1] = y;
	pesInRow.insert(map(coor));
      }

      // Leave out the PE that the block lives on - it will never send to itself
      coor[0] = index.x; coor[1] = index.y;
      int homePE = map(coor);
      pesInRow.erase(homePE);

      return pesInRow.size();
    }
  }

private:
  const int peRows, peCols, peRotate, peStride, numBlks;
};

/// Mapping for BlockScheduler - created as an array so group section multicast
/// can be used
struct OnePerPE : public CBase_OnePerPE {
  OnePerPE() { }
  int registerArray(CkArrayIndexMax& numElements,CkArrayID aid) {
    CkAssert(CkNumPes() == numElements.index[0]);
    return 0;
  }
  int procNum(int arrayHdl, const CkArrayIndex &elt) {
    return *(int*)elt.data();
  }
};
#endif
