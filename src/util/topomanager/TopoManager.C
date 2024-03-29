/** \file TopoManager.C
 *  Author: Abhinav S Bhatele
 *  Date Created: March 19th, 2007
 */

#include "TopoManager.h"
#ifndef __TPM_STANDALONE__
#include "partitioning_strategies.h"
#endif
#include <algorithm>

struct CompareRankDist {
  std::vector<int> peDist;

  CompareRankDist(int *root, int *pes, int n, const TopoManager *tmgr) : peDist(n) {
    for(int p = 0; p < n; p++) {
      peDist[p] = tmgr->getHopsBetweenRanks(root, pes[p]);
    }
  }

  bool operator() (int i, int j) {
    return (peDist[i] < peDist[j]);
  }
};


TopoManager::TopoManager() {
#if XT4_TOPOLOGY || XT5_TOPOLOGY || XE6_TOPOLOGY
  dimX = xttm.getDimX();
  dimY = xttm.getDimY();
  dimZ = xttm.getDimZ();

  dimNX = xttm.getDimNX();
  dimNY = xttm.getDimNY();
  dimNZ = xttm.getDimNZ();
  dimNT = xttm.getDimNT();

  procsPerNode = xttm.getProcsPerNode();
  int *torus;
  torus = xttm.isTorus();
  torusX = torus[0];
  torusY = torus[1];
  torusZ = torus[2];
  torusT = torus[3];

#else
  dimX = CmiNumPes();
  dimY = 1;
  dimZ = 1;

  dimNX = CmiNumPhysicalNodes();
  dimNY = 1;
  dimNZ = 1;

  dimNT = 0;
  for (int i=0; i < dimNX; i++) {
    int n = CmiNumPesOnPhysicalNode(i);
    if (n > dimNT) dimNT = n;
  }
  procsPerNode = dimNT;
  torusX = true;
  torusY = true;
  torusZ = true;
  torusT = false;
#endif


  numPes = CmiNumPes();
}

TopoManager::TopoManager(int NX, int NY, int NZ, int NT) : dimNX(NX), dimNY(NY), dimNZ(NZ), dimNT(NT) {
  /* we rashly assume only one dimension is expanded */
  procsPerNode = dimNT;
  dimX = dimNX * dimNT;
  dimY = dimNY;
  dimZ = dimNZ;
  torusX = true;
  torusY = true;
  torusZ = true;
  numPes = dimNX * dimNY * dimNZ * dimNT;
}

void TopoManager::rankToCoordinates(int pe, std::vector<int> &coords) const {
  coords.resize(getNumDims()+1);
  rankToCoordinates(pe,coords[0],coords[1],coords[2],coords[3]);
}

void TopoManager::rankToCoordinates(int pe, int &x, int &y, int &z) const {
  CmiAssert( pe >= 0 && pe < numPes );
#if XT4_TOPOLOGY || XT5_TOPOLOGY || XE6_TOPOLOGY
	int t;
  xttm.rankToCoordinates(pe, x, y, z, t);
#else
  if(dimY > 1){
    // Assumed TXYZ
    x = pe % dimX;
    y = (pe % (dimX * dimY)) / dimX;
    z = pe / (dimX * dimY);
  }
  else {
    x = CmiPhysicalNodeID(pe);
    y = 0; 
    z = 0;
  }
#endif

}

void TopoManager::rankToCoordinates(int pe, int &x, int &y, int &z, int &t) const {
  CmiAssert( pe >= 0 && pe < numPes );
#if XT4_TOPOLOGY || XT5_TOPOLOGY || XE6_TOPOLOGY
  xttm.rankToCoordinates(pe, x, y, z, t);
#else
  if(dimNY > 1) {
    t = pe % dimNT;
    x = (pe % (dimNT*dimNX)) / dimNT;
    y = (pe % (dimNT*dimNX*dimNY)) / (dimNT*dimNX);
    z = pe / (dimNT*dimNX*dimNY);
  } else {
    t = CmiPhysicalRank(pe);
    x = CmiPhysicalNodeID(pe);
    y = 0;
    z = 0;
  }
#endif

}

int TopoManager::coordinatesToRank(int x, int y, int z) const {
  if(!( x>=0 && x<dimX && y>=0 && y<dimY && z>=0 && z<dimZ ))
    return -1;


#if XT4_TOPOLOGY || XT5_TOPOLOGY || XE6_TOPOLOGY
  return xttm.coordinatesToRank(x, y, z, 0);
#else
  if(dimY > 1)
    return x + y*dimX + z*dimX*dimY;
  else
    return CmiGetFirstPeOnPhysicalNode(x);
#endif
}

int TopoManager::coordinatesToRank(int x, int y, int z, int t) const {
  if(!( x>=0 && x<dimNX && y>=0 && y<dimNY && z>=0 && z<dimNZ && t>=0 && t<dimNT ))
    return -1;

#if XT4_TOPOLOGY || XT5_TOPOLOGY || XE6_TOPOLOGY
  return xttm.coordinatesToRank(x, y, z, t);
#else
  if(dimNY > 1)
    return t + (x + (y + z*dimNY) * dimNX) * dimNT;
  else {
    if (t >= CmiNumPesOnPhysicalNode(x)) return -1;
    else return CmiGetFirstPeOnPhysicalNode(x)+t;
  }
#endif
}

int TopoManager::getHopsBetweenRanks(int pe1, int pe2) const {
  CmiAssert( pe1 >= 0 && pe1 < numPes );
  CmiAssert( pe2 >= 0 && pe2 < numPes );
  int x1, y1, z1, x2, y2, z2, t1, t2;
  rankToCoordinates(pe1, x1, y1, z1, t1);
  rankToCoordinates(pe2, x2, y2, z2, t2);
  return (absX(x2-x1)+absY(y2-y1)+absZ(z2-z1));
}

int TopoManager::getHopsBetweenRanks(int *pe1, int pe2) const {
  CmiAssert( pe2 >= 0 && pe2 < numPes );
  int x2, y2, z2, t2;
  rankToCoordinates(pe2, x2, y2, z2, t2);
  return (absX(x2-pe1[0])+absY(y2-pe1[1])+absZ(z2-pe1[2]));
}

void TopoManager::sortRanksByHops(int pe, int *pes, int *idx, int n) const {
  int root_coords[4];
  rankToCoordinates(pe, root_coords[0], root_coords[1], root_coords[2],
      root_coords[3]);
  sortRanksByHops(root_coords, pes, idx, n);
}

void TopoManager::sortRanksByHops(int* root_coords, int *pes, int *idx, int n) const {
  for(int i=0;i<n;i++)
    idx[i] = i;
  CompareRankDist comparator(root_coords, pes, n, this);
  std::sort(&idx[0], idx + n, comparator);
}

int TopoManager::pickClosestRank(int mype, int *pes, int n) const {
  int minHops = getHopsBetweenRanks(mype, pes[0]);
  int minIdx=0;
  int nowHops; 
  for(int i=1; i<n; i++) {
    nowHops = getHopsBetweenRanks(mype, pes[i]);
    if(nowHops < minHops) {
      minHops = nowHops;
      minIdx=i;
    }
  }
  return minIdx;
}

int TopoManager::areNeighbors(int pe1, int pe2, int pe3, int distance) const {
  int pe1_x, pe1_y, pe1_z, pe1_t;
  int pe2_x, pe2_y, pe2_z, pe2_t;
  int pe3_x, pe3_y, pe3_z, pe3_t;

  rankToCoordinates(pe1, pe1_x, pe1_y, pe1_z, pe1_t);
  rankToCoordinates(pe2, pe2_x, pe2_y, pe2_z, pe2_t);
  rankToCoordinates(pe3, pe3_x, pe3_y, pe3_z, pe3_t);

  if ( (absX(pe1_x - (pe2_x+pe3_x)/2) + absY(pe1_y - (pe2_y+pe3_y)/2) + absZ(pe1_z - (pe2_z+pe3_z)/2)) <= distance )
    return 1;
  else
    return 0;
}

void TopoManager::printAllocation(FILE *fp) const
{
	int i,x,y,z,t;
	fprintf(fp, "Topology Info-\n");
	fprintf(fp, "NumPes -  %d\n", numPes);
	fprintf(fp, "Dims - %d %d %d\n",dimNX,dimNY,dimNZ);
	fprintf(fp, "GlobalPe/GlobalNode - LocalPe/LocalNode - x y z t\n");
	for(i=0; i<numPes; i++) {
		rankToCoordinates(i,x,y,z,t);
		fprintf(fp, "%d/%d - %d/%d - %d %d %d %d\n",CmiGetPeGlobal(i,CmiMyPartition()),CmiGetNodeGlobal(CmiNodeOf(i),CmiMyPartition()),i,CmiNodeOf(i),x,y,z,t);
	}
}

#if XT4_TOPOLOGY || XT5_TOPOLOGY || XE6_TOPOLOGY
extern "C" void craynid_init();
extern "C" void craynid_reset();
extern "C" void craynid_free();
#endif

static bool _topoInitialized = false;
CmiNodeLock _topoLock = 0;
TopoManager *_tmgr = NULL;
#ifdef __TPM_STANDALONE__
int _tpm_numpes = 0;
int _tpm_numthreads = 1;
#endif

TopoManager *TopoManager::getTopoManager() {
  CmiAssert(_topoInitialized);
  CmiAssert(_tmgr != NULL);
  return _tmgr;
}

#ifndef __TPM_STANDALONE__
// NOTE: this is not thread-safe
extern "C" void TopoManager_init() {
#else
extern "C" void TopoManager_init(int numpes) {
  _tpm_numpes = numpes;
#endif
  if(!_topoInitialized) {
    _topoLock = CmiCreateLock();
#if XT4_TOPOLOGY || XT5_TOPOLOGY || XE6_TOPOLOGY
    craynid_init();
#endif
    _topoInitialized = true;
  }
#ifdef __TPM_STANDALONE__
  if(_tmgr) delete _tmgr;
  _tmgr = new TopoManager;
#endif
}

#ifdef __TPM_STANDALONE__
extern "C" void TopoManager_setNumThreads(int t) {
  _tpm_numthreads = t;
}
#endif

extern "C" void TopoManager_reset() {
  CmiLock(_topoLock);
#if XT4_TOPOLOGY || XT5_TOPOLOGY || XE6_TOPOLOGY
  craynid_reset();
#endif
  if(_tmgr) delete _tmgr;
  _tmgr = new TopoManager;
  CmiUnlock(_topoLock);
}

extern "C" void TopoManager_free() {
  CmiLock(_topoLock);
  if(_tmgr) delete _tmgr;
  _tmgr = NULL;
#if XT4_TOPOLOGY || XT5_TOPOLOGY || XE6_TOPOLOGY
  craynid_free();
#endif
  CmiUnlock(_topoLock);
}

extern "C" void TopoManager_printAllocation(FILE *fp) {
#ifndef __TPM_STANDALONE__
  if(_tmgr == NULL) { TopoManager_reset(); }
#else
  if(_tmgr == NULL) { printf("ERROR: TopoManager NOT initialized. Aborting...\n"); exit(1); }
#endif

  _tmgr->printAllocation(fp);
}

extern "C" void TopoManager_getDimCount(int *ndims) {
  *ndims = 3;
}

extern "C" void TopoManager_getDims(int *dims) {
#ifndef __TPM_STANDALONE__
  if(_tmgr == NULL) { TopoManager_reset(); }
#else
  if(_tmgr == NULL) { printf("ERROR: TopoManager NOT initialized. Aborting...\n"); exit(1); }
#endif

  dims[0] = _tmgr->getDimNX();
  dims[1] = _tmgr->getDimNY();
  dims[2] = _tmgr->getDimNZ();
  dims[3] = _tmgr->getDimNT()/CmiMyNodeSize();
}

extern "C" void TopoManager_getCoordinates(int rank, int *coords) {
#ifndef __TPM_STANDALONE__
  if(_tmgr == NULL) { TopoManager_reset(); }
#else
  if(_tmgr == NULL) { printf("ERROR: TopoManager NOT initialized. Aborting...\n"); exit(1); }
#endif

  int t;
  _tmgr->rankToCoordinates(CmiNodeFirst(rank),coords[0],coords[1],coords[2],t);
}

extern "C" void TopoManager_getPeCoordinates(int rank, int *coords) {
#ifndef __TPM_STANDALONE__
  if(_tmgr == NULL) { TopoManager_reset(); }
#else
  if(_tmgr == NULL) { printf("ERROR: TopoManager NOT initialized. Aborting...\n"); exit(1); }
#endif

  _tmgr->rankToCoordinates(rank,coords[0],coords[1],coords[2],coords[3]);
}

void TopoManager_getRanks(int *rank_cnt, int *ranks, int *coords) {
#ifndef __TPM_STANDALONE__
  if(_tmgr == NULL) { TopoManager_reset(); }
#else
  if(_tmgr == NULL) { printf("ERROR: TopoManager NOT initialized. Aborting...\n"); exit(1); }
#endif

  *rank_cnt = 0;
  for(int t = 0; t < _tmgr->getDimNT(); t += CmiMyNodeSize()) {
    int rank = _tmgr->coordinatesToRank(coords[0],coords[1],coords[2],t);
    if(rank != -1) {
      ranks[*rank_cnt] = CmiNodeOf(rank);
      *rank_cnt = *rank_cnt + 1;
    }
  }
}

extern "C" void TopoManager_getPeRank(int *rank, int *coords) {
#ifndef __TPM_STANDALONE__
  if(_tmgr == NULL) { TopoManager_reset(); }
#else
  if(_tmgr == NULL) { printf("ERROR: TopoManager NOT initialized. Aborting...\n"); exit(1); }
#endif

  *rank = _tmgr->coordinatesToRank(coords[0],coords[1],coords[2],coords[3]);
}

extern "C" void TopoManager_getHopsBetweenPeRanks(int pe1, int pe2, int *hops) {
#ifndef __TPM_STANDALONE__
  if(_tmgr == NULL) { TopoManager_reset(); }
#else
  if(_tmgr == NULL) { printf("ERROR: TopoManager NOT initialized. Aborting...\n"); exit(1); }
#endif

  *hops = _tmgr->getHopsBetweenRanks(pe1, pe2);
}

#ifndef __TPM_STANDALONE__
extern "C" void TopoManager_createPartitions(int scheme, int numparts, int *nodeMap) {
  if(scheme == 0) {
    if(!CmiMyNodeGlobal()) {
      printf("Charm++> Using rank ordered division (scheme 0) for topology aware partitions\n");
    }
    int i;
    for(i = 0; i < CmiNumNodes(); i++) {
      nodeMap[i] = i;
    }
  } else if(scheme == 1) {
    if(!CmiMyNodeGlobal()) {
      printf("Charm++> Using planar division (scheme 1) for topology aware partitions\n");
    }
    getPlanarList(nodeMap);
  } else if(scheme == 2) {
    if(!CmiMyNodeGlobal()) {
      printf("Charm++> Using hilber curve (scheme 2) for topology aware partitions\n");
    }
    getHilbertList(nodeMap);
  } else if(scheme == 3) {
    if(!CmiMyNodeGlobal()) {
      printf("Charm++> Using recursive bisection (scheme 3) for topology aware partitions\n");
    }
    getRecursiveBisectionList(numparts,nodeMap);
  } else {
    CmiAbort("Specified value for topology scheme is not supported\n");
  }
}
#endif
