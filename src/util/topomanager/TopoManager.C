/** \file TopoManager.C
 *  Author: Abhinav S Bhatele
 *  Date Created: March 19th, 2007
 *
 *  This would be the top level interface for all topology managers we
 *  will write for different machines (cray, bg/l ... for tori, meshes ...)
 *  Currently we have functionality for Blue Gene, Cray XT/XE,
 *  and non-topo machines.
 */

#include "TopoManager.h"
#ifndef __TPM_STANDALONE__
#include "partitioning_strategies.h"
#endif
#include <algorithm>

std::vector<int> PhyNode::D;

// NOTE: this method is topology-dependent. Current implementation assumes a
// mesh/torus topology.
// It also works for scenarios with no topology info. In this case, each physical
// node is assumed to have 2 neighbors (e.g. host i would have as neighbors i-1 and i+1)
// Might need to special-case this for other/future topologies
void PhyNode::calculateNbs(std::vector<int> &dims, std::vector<int> &nbIds) const
{
  int nDims = dims.size();
  for (int i=0; i < nDims; i++) {
    // dimension i
    const int &x = coords[i];
    const int &X = dims[i];
    if (X == 1) continue;
    bool isTorus = TopoManager_isTorus(i);

    std::vector<int> nb_coords = coords;
    int nbId = -1;
    if ((x+1) >= X) {
      if (isTorus) {
        nb_coords[i] = 0;
        nbId = PhyNode::generateID(nb_coords,nDims);
      }
    } else {
      nb_coords[i] += 1;
      nbId = PhyNode::generateID(nb_coords,nDims);
    }
    if (nbId >= 0) nbIds.push_back(nbId);

    nb_coords = coords;
    nbId = -1;
    if (x-1 < 0) {
      if (isTorus) {
        nb_coords[i] = X - 1;
        nbId = PhyNode::generateID(nb_coords,nDims);
      }
    } else {
      nb_coords[i] -= 1;
      nbId = PhyNode::generateID(nb_coords,nDims);
    }
    if ((isTorus) && (nbId == nbIds.back())) nbId = -1;  // already inserted this neighbor above
    if (nbId >= 0) nbIds.push_back(nbId);
  }
}

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
#if CMK_BLUEGENEQ
  dimX = bgqtm.getDimX();
  dimY = bgqtm.getDimY();
  dimZ = bgqtm.getDimZ();

  dimNX = bgqtm.getDimNX();
  dimNY = bgqtm.getDimNY();
  dimNZ = bgqtm.getDimNZ();
  dimNT = bgqtm.getDimNT();
  
  dimNA = bgqtm.getDimNA();
  dimNB = bgqtm.getDimNB();
  dimNC = bgqtm.getDimNC();
  dimND = bgqtm.getDimND();
  dimNE = bgqtm.getDimNE();

  procsPerNode = bgqtm.getProcsPerNode();
  int *torus;
  torus = bgqtm.isTorus();
  torusA = torus[0];
  torusB = torus[1];
  torusC = torus[2]; 
  torusD = torus[3];
  torusE = torus[4];

  PhyNode::D.resize(4);
  PhyNode::D[3] = dimNE;
  PhyNode::D[2] = dimND * PhyNode::D[3];
  PhyNode::D[1] = dimNC * PhyNode::D[2];
  PhyNode::D[0] = dimNB * PhyNode::D[1];

#elif XT4_TOPOLOGY || XT5_TOPOLOGY || XE6_TOPOLOGY
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

  PhyNode::D.resize(2);
  PhyNode::D[1] = dimNZ;
  PhyNode::D[0] = dimNY * PhyNode::D[1];

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

  PhyNode::D.resize(2);
  PhyNode::D[1] = dimNZ;
  PhyNode::D[0] = dimNY * PhyNode::D[1];

#endif


  allocatedPhyNodes = 0;
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
#if CMK_BLUEGENEQ
  torusA = true;
  torusB = true;
  torusC = true;
  torusD = true;
  torusE = true;
#else
  PhyNode::D.resize(2);
  PhyNode::D[1] = dimZ;
  PhyNode::D[0] = dimY * PhyNode::D[1];
#endif
  allocatedPhyNodes = 0;
  numPes = dimNX * dimNY * dimNZ * dimNT;
}

void TopoManager::rankToCoordinates(int pe, std::vector<int> &coords) const {
  coords.resize(getNumDims()+1);
#if CMK_BLUEGENEQ
  rankToCoordinates(pe,coords[0],coords[1],coords[2],coords[3],coords[4],coords[5]);
#else
  rankToCoordinates(pe,coords[0],coords[1],coords[2],coords[3]);
#endif
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
#if CMK_BLUEGENEQ
  bgqtm.rankToCoordinates(pe, x, y, z, t);
#elif XT4_TOPOLOGY || XT5_TOPOLOGY || XE6_TOPOLOGY
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

#if CMK_BLUEGENEQ
void TopoManager::rankToCoordinates(int pe, int &a, int &b, int &c, int &d, int &e, int &t) const {
  CmiAssert( pe >= 0 && pe < numPes );
  bgqtm.rankToCoordinates(pe, a, b, c, d, e, t);
}
#endif

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

#if CMK_BLUEGENEQ
  return bgqtm.coordinatesToRank(x, y, z, t);
#elif XT4_TOPOLOGY || XT5_TOPOLOGY || XE6_TOPOLOGY
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

#if CMK_BLUEGENEQ
int TopoManager::coordinatesToRank(int a, int b, int c, int d, int e, int t) const {
  if(!( a>=0 && a<dimNA && b>=0 && b<dimNB && c>=0 && c<dimNC && d>=0 && d<dimND && e>=0 && e<dimNE && t>=0 && t<dimNT ))
    return -1;
  return bgqtm.coordinatesToRank(a, b, c, d, e, t);
}
#endif

int TopoManager::getHopsBetweenRanks(int pe1, int pe2) const {
  CmiAssert( pe1 >= 0 && pe1 < numPes );
  CmiAssert( pe2 >= 0 && pe2 < numPes );
#if CMK_BLUEGENEQ
  int a1, b1, c1, d1, e1, t1, a2, b2, c2, d2, e2, t2; 
  rankToCoordinates(pe1, a1, b1, c1, d1, e1, t1);
  rankToCoordinates(pe2, a2, b2, c2, d2, e2, t2);
  return (absA(a2-a1)+absB(b2-b1)+absC(c2-c1)+absD(d2-d1)+absE(e2-e1));  
#else
  int x1, y1, z1, x2, y2, z2, t1, t2;
  rankToCoordinates(pe1, x1, y1, z1, t1);
  rankToCoordinates(pe2, x2, y2, z2, t2);
  return (absX(x2-x1)+absY(y2-y1)+absZ(z2-z1));
#endif
}

int TopoManager::getHopsBetweenRanks(int *pe1, int pe2) const {
  CmiAssert( pe2 >= 0 && pe2 < numPes );
#if CMK_BLUEGENEQ
  int a2, b2, c2, d2, e2, t2; 
  rankToCoordinates(pe2, a2, b2, c2, d2, e2, t2);
  return (absA(a2-pe1[0])+absB(b2-pe1[1])+absC(c2-pe1[2]) + 
      absD(d2-pe1[3])+absE(e2-pe1[4]));  
#else
  int x2, y2, z2, t2;
  rankToCoordinates(pe2, x2, y2, z2, t2);
  return (absX(x2-pe1[0])+absY(y2-pe1[1])+absZ(z2-pe1[2]));
#endif
}

void TopoManager::sortRanksByHops(int pe, int *pes, int *idx, int n) const {
#if CMK_BLUEGENEQ
  int root_coords[6];
  rankToCoordinates(pe, root_coords[0], root_coords[1], root_coords[2],
      root_coords[3], root_coords[4], root_coords[5]);
#else
  int root_coords[4];
  rankToCoordinates(pe, root_coords[0], root_coords[1], root_coords[2],
      root_coords[3]);
#endif
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
#if CMK_BLUEGENEQ
  int pe1_a, pe1_b, pe1_c, pe1_d, pe1_e, pe1_t;
  int pe2_a, pe2_b, pe2_c, pe2_d, pe2_e, pe2_t;
  int pe3_a, pe3_b, pe3_c, pe3_d, pe3_e, pe3_t;
  rankToCoordinates(pe1, pe1_a, pe1_b, pe1_c, pe1_d, pe1_e, pe1_t);
  rankToCoordinates(pe2, pe2_a, pe2_b, pe2_c, pe2_d, pe2_e, pe2_t);
  rankToCoordinates(pe3, pe3_a, pe3_b, pe3_c, pe3_d, pe3_e, pe3_t);

  if ( (absA(pe1_a - (pe2_a+pe3_a)/2) + absB(pe1_b - (pe2_b+pe3_b)/2)+absC(pe1_c - (pe2_c+pe3_c)/2)+absD(pe1_d - (pe2_d+pe3_d)/2)+absE(pe1_e - (pe2_e+pe3_e)/2)) <= distance )
    return 1;
  else
    return 0;
#else
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
#endif
}

void TopoManager::getAllocationShape(std::vector<int> &shape) const
{
  const int nDims = getNumDims();
  shape.resize(nDims);

#if CMK_BLUEGENEQ
  for (int i=0; i < nDims; i++) shape[i] = getDimSize(i);
#else
  std::vector< std::vector<bool> > used_coordinates(nDims);
  for (int i=0; i < nDims; i++) used_coordinates[i].resize(getDimSize(i), false);
  for (int i=0; i < getDimSize(0); i++) {
    for (int j=0; j < getDimSize(1); j++) {
      for (int k=0; k < getDimSize(2); k++) {
        int coords[4] = {i, j, k, 0};
        int p = coordinatesToRank(coords[0],coords[1],coords[2],coords[3]);
        if (p >= 0) {
          for (int x=0; x < nDims; x++) used_coordinates[x][coords[x]] = true;
        }
      }
    }
  }
  for (int i=0; i < nDims; i++) {
    shape[i] = 0;
    for (int j=0; j < used_coordinates[i].size(); j++) {
      if (used_coordinates[i][j]) shape[i]++;
    }
  }
#endif
}

void TopoManager::buildPhyNodeList()
{
  if (phy_nodes.size() > 0) return; // list already built

  const int nDims = getNumDims();
  std::vector<int> dims(nDims);
  for (int i=0; i < nDims; i++) dims[i] = getDimSize(i);

#if TMGR_PE_TO_NODE_TABLE
  peToNode.reserve(numPes);
#endif

  allocatedPhyNodes = 0;
  int nCoordinates=1; // total number of unique coordinates in the system
  for (int i=0; i < nDims; i++) nCoordinates *= dims[i];
  std::vector<int> coords(nDims+1, 0);
  phy_nodes.resize(nCoordinates);
  int totalPes = 0;
  for (int i=0; i < nCoordinates; i++) {
#if CMK_BLUEGENEQ
    int p = coordinatesToRank(coords[0],coords[1],coords[2],coords[3],coords[4],coords[5]);
#else
    int p = coordinatesToRank(coords[0],coords[1],coords[2],coords[3]);
#endif
    if (p >= 0) { // PE rank 0 in this physical node is allocated to us
      allocatedPhyNodes++;
      PhyNode n0(coords,nDims);
      n0.p = p;
      int numpes = 1;
      for (int j=1; j < getDimNT(); j++) {
        coords[nDims] = j;
#if CMK_BLUEGENEQ
        int p_j = coordinatesToRank(coords[0],coords[1],coords[2],coords[3],coords[4],coords[5]);
#else
        int p_j = coordinatesToRank(coords[0],coords[1],coords[2],coords[3]);
#endif
        if (p_j >= 0) { // PE rank j in this physical node exists / is allocated to us
          numpes++;
          CmiAssert(p + j == p_j); // make sure our assumption of consecutive PE numbers holds
        }
      }
      n0.numPes = numpes;
      totalPes += numpes;
      phy_nodes[n0.id] = n0;
#if TMGR_PE_TO_NODE_TABLE
      for (int j=0; j < numpes; j++) peToNode[p+j] = &phy_nodes[n0.id];
#endif
    }
    // go to next coordinates
    for (int j=nDims-1; j > -1; j--) {
      coords[j] = (coords[j]+1) % dims[j];
      if (coords[j] != 0) break;
    }
    coords[nDims] = 0;
  }
  CmiAssert(CmiNumPes() == totalPes);

  // populate node adjacency lists
  // I could avoid this piece of code and do it above if I consider neighbor nodes
  // with no PEs as valid neighbors
  // Nodes with no PEs can happen, for example, on Blue Waters
  for (int i=0; i < phy_nodes.size(); i++) {
    if (phy_nodes[i].numPes) {  // this phynode is allocated to us
      PhyNode &n = phy_nodes[i];
      std::vector<int> nbIds;
      n.calculateNbs(dims, nbIds);
      for (int j=0; j < nbIds.size(); j++) {
        PhyNode &nb = phy_nodes[nbIds[j]];
        if (nb.numPes) n.nbs.push_back(&nb);
      }
    }
  }
}

const PhyNode &TopoManager::phyNodeOf(int pe) const {
#if TMGR_PE_TO_NODE_TABLE
  // faster implementation, more memory (sizeof(pointer) * CmiNumPes())
  return *(peToNode[pe]);
#else
  // slower implementation, no additional memory
#if CMK_BLUEGENEQ
  std::vector<int> coords(6);
  rankToCoordinates(pe, coords[0], coords[1], coords[2], coords[3], coords[4], coords[5]);
#else
  std::vector<int> coords(4);
  rankToCoordinates(pe, coords[0], coords[1], coords[2], coords[3]);
#endif
  return getNode(coords);
#endif
}

#if CMK_BLUEGENEQ
void TopoManager::printAllocation(FILE *fp) const
{
	int i,a,b,c,d,e,t;
	fprintf(fp, "Topology Info-\n");
	fprintf(fp, "NumPes -  %d\n", numPes);
	fprintf(fp, "Dims - %d %d %d %d %d\n",dimNA,dimNB,dimNC,dimND,dimNE);
	fprintf(fp, "Rank - a b c d e t\n");
	for(i=0; i<numPes; i++) {
		rankToCoordinates(i,a,b,c,d,e,t);
		fprintf(fp, "%d/%d - %d/%d - %d %d %d %d %d %d\n",CmiGetPeGlobal(i,CmiMyPartition()),CmiGetNodeGlobal(CmiNodeOf(i),CmiMyPartition()),i,CmiNodeOf(i),a,b,c,d,e,t);
	}
}
#else
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
#endif

#if XT4_TOPOLOGY || XT5_TOPOLOGY || XE6_TOPOLOGY
extern "C" void craynid_init();
extern "C" void craynid_reset();
extern "C" void craynid_free();
#elif CMK_BLUEGENEQ
extern void bgq_topo_init();
extern void bgq_topo_reset();
extern void bgq_topo_free();
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
#elif CMK_BLUEGENEQ
    bgq_topo_init();
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
#elif CMK_BLUEGENEQ
  bgq_topo_reset();
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
#elif CMK_BLUEGENEQ
  bgq_topo_free();
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
#if CMK_BLUEGENEQ
  *ndims = 5;
#else
  *ndims = 3;
#endif
}

extern "C" void TopoManager_getDims(int *dims) {
#ifndef __TPM_STANDALONE__
  if(_tmgr == NULL) { TopoManager_reset(); }
#else
  if(_tmgr == NULL) { printf("ERROR: TopoManager NOT initialized. Aborting...\n"); exit(1); }
#endif

#if CMK_BLUEGENEQ
  dims[0] = _tmgr->getDimNA();
  dims[1] = _tmgr->getDimNB();
  dims[2] = _tmgr->getDimNC();
  dims[3] = _tmgr->getDimND();
  dims[4] = _tmgr->getDimNE();
  dims[5] = _tmgr->getDimNT()/CmiMyNodeSize();
#else 
  dims[0] = _tmgr->getDimNX();
  dims[1] = _tmgr->getDimNY();
  dims[2] = _tmgr->getDimNZ();
  dims[3] = _tmgr->getDimNT()/CmiMyNodeSize();
#endif
}

extern "C" void TopoManager_getCoordinates(int rank, int *coords) {
#ifndef __TPM_STANDALONE__
  if(_tmgr == NULL) { TopoManager_reset(); }
#else
  if(_tmgr == NULL) { printf("ERROR: TopoManager NOT initialized. Aborting...\n"); exit(1); }
#endif

  int t;
#if CMK_BLUEGENEQ
  _tmgr->rankToCoordinates(CmiNodeFirst(rank),coords[0],coords[1],coords[2],coords[3],coords[4],t);
#else
  _tmgr->rankToCoordinates(CmiNodeFirst(rank),coords[0],coords[1],coords[2],t);
#endif
}

extern "C" void TopoManager_getPeCoordinates(int rank, int *coords) {
#ifndef __TPM_STANDALONE__
  if(_tmgr == NULL) { TopoManager_reset(); }
#else
  if(_tmgr == NULL) { printf("ERROR: TopoManager NOT initialized. Aborting...\n"); exit(1); }
#endif

#if CMK_BLUEGENEQ
  _tmgr->rankToCoordinates(rank,coords[0],coords[1],coords[2],coords[3],coords[4],coords[5]);
#else
  _tmgr->rankToCoordinates(rank,coords[0],coords[1],coords[2],coords[3]);
#endif
}

void TopoManager_getRanks(int *rank_cnt, int *ranks, int *coords) {
#ifndef __TPM_STANDALONE__
  if(_tmgr == NULL) { TopoManager_reset(); }
#else
  if(_tmgr == NULL) { printf("ERROR: TopoManager NOT initialized. Aborting...\n"); exit(1); }
#endif

  *rank_cnt = 0;
  for(int t = 0; t < _tmgr->getDimNT(); t += CmiMyNodeSize()) {
#if CMK_BLUEGENEQ
    int rank = _tmgr->coordinatesToRank(coords[0],coords[1],coords[2],coords[3],coords[4],t);
#else
    int rank = _tmgr->coordinatesToRank(coords[0],coords[1],coords[2],t);
#endif
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

#if CMK_BLUEGENEQ
  *rank = _tmgr->coordinatesToRank(coords[0],coords[1],coords[2],coords[3],coords[4],coords[5]);
#else
  *rank = _tmgr->coordinatesToRank(coords[0],coords[1],coords[2],coords[3]);
#endif
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

extern "C" int TopoManager_isTorus(int dim) {
#ifndef __TPM_STANDALONE__
  if(_tmgr == NULL) { TopoManager_reset(); }
#else
  if(_tmgr == NULL) { printf("ERROR: TopoManager NOT initialized. Aborting...\n"); exit(1); }
#endif

  return _tmgr->isTorus(dim);
}

