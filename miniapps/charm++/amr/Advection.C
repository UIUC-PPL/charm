#include "Headers.h"
#include <assert.h>

using std::vector;
using std::ofstream;
using std::max;
using std::min;
using std::map;

extern CProxy_Main mainProxy;
extern CProxy_Advection qtree;

extern int array_height;
extern int array_width;
extern int array_depth;

extern int num_chare_rows;
extern int num_chare_cols;
extern int num_chare_Zs;

extern int block_width, block_height, block_depth;

extern int min_depth, max_depth;

extern int nframe;
extern float xctr, yctr, zctr, radius;
extern float vx, vy, vz;
extern float apx, anx, apy, any, apz, anz;
extern float tmax, t, dt, cfl;
extern int max_iterations, refine_frequency, lb_freq;
//extern bool inInitialMeshGenerationPhase;
#define inInitialMeshGenerationPhase (meshGenIterations <= max_depth)

float refine_filter = 0.01;
float refine_cutoff=0.2, derefine_cutoff=0.05;

CProxy_AdvectionGroup ppc;

enum BitsToUse { LOW, HIGH, BOTH };
void populateIndices(BitsToUse X, BitsToUse Y, BitsToUse Z, std::vector<int>&IDs) {
  BitsToUse Dims[3] = {Z, Y, X};

  for (unsigned i = 0; i < NUM_CHILDREN; ++i) {
    bool keep = true;
    for (unsigned j = 0; j < numDims; ++j) {
      unsigned bit = 1 << j;
      bool set = (i & bit) != 0;

      if (Dims[j] == BOTH) continue;

      if (set == (Dims[j] == LOW)) {
        keep = false;
        break;
      }
    }
    if (keep)
      IDs.push_back(i);
  }
}
inline void getChildrenInDir(OctIndex myIndex, int dir, std::vector<OctIndex> &children) {
  // Given an index "myIndex", return the children that share the surface
  // specified by 'dir'.
  std::vector<int> IDs;
  switch (dir) {
  case LEFT:     populateIndices(LOW,  BOTH, BOTH, IDs); break;
  case RIGHT:    populateIndices(HIGH, BOTH, BOTH, IDs); break;
  case UP:       populateIndices(BOTH, HIGH, BOTH, IDs); break;
  case DOWN:     populateIndices(BOTH, LOW,  BOTH, IDs); break;
  case FORWARD:  populateIndices(BOTH, BOTH, HIGH, IDs); break;
  case BACKWARD: populateIndices(BOTH, BOTH, LOW,  IDs); break;
  }

  for (std::vector<int>::iterator I = IDs.begin(), E = IDs.end(); I != E; ++I)
    children.push_back(myIndex.getChild(*I));
  assert(children.size() == 4);
  return;
}

inline void setFirstHalf(int &min, int &max) {
  max /= 2;
}
inline void setSecondHalf(int &min, int &max) {
  min = (max / 2) + 1;
}

inline void populateQuadrant(bool bit1, bool bit2,
                             int &min1, int &max1,
                             int &min2, int &max2) {
  if (bit1)
    setSecondHalf(min1, max1);
  else
    setFirstHalf(min1, max1);

  if (bit2)
    setSecondHalf(min2, max2);
  else
    setFirstHalf(min2, max2);
}

enum {
  X_MASK = 1 << 2,
  Y_MASK = 1 << 1,
  Z_MASK = 1 << 0
};
inline void populateRanges(int dir, int octant,
                           int &xmin, int &xmax,
                           int &ymin, int &ymax,
                           int &zmin, int &zmax) {
  xmin = 0; xmax = block_width-1;
  ymin = 0; ymax = block_height-1;
  zmin = 0; zmax = block_depth-1;
  switch (dir) {
  case UP:
    ymin = ymax = block_height+1;
    if (octant >= 0) {
      populateQuadrant(
        octant & X_MASK,
        octant & Z_MASK,
        xmin, xmax,
        zmin, zmax);
    }
    break;
  case DOWN:
    ymax = 0;
    if (octant >= 0) {
      populateQuadrant(
        octant & X_MASK,
        octant & Z_MASK,
        xmin, xmax,
        zmin, zmax);
    }
    break;
  case LEFT:
    xmax = 0;
    if (octant >= 0) {
      populateQuadrant(
        octant & Y_MASK,
        octant & Z_MASK,
        ymin, ymax,
        zmin, zmax);
    }
    break;
  case RIGHT:
    xmin = xmax = block_width+1;
    if (octant >= 0) {
      populateQuadrant(
        octant & Y_MASK,
        octant & Z_MASK,
        ymin, ymax,
        zmin, zmax);
    }
    break;
  case FORWARD:
    zmin = zmax = block_depth+1;
    if (octant >= 0) {
      populateQuadrant(
        octant & X_MASK,
        octant & Y_MASK,
        xmin, xmax,
        ymin, ymax);
    }
    break;
  case BACKWARD:
    zmax = 0;
    if (octant >= 0) {
      populateQuadrant(
        octant & X_MASK,
        octant & Y_MASK,
        xmin, xmax,
        ymin, ymax);
    }
    break;
  }
}

inline bool getOctantRange(int octant,
                           int &xmin, int &xmax,
                           int &ymin, int &ymax,
                           int &zmin, int &zmax) {
  xmin = 1;
  xmax = block_width;
  ymin = 1;
  ymax = block_height;
  zmin = 1;
  zmax = block_depth;
  if (octant & X_MASK)
    xmin = block_width/2+1;
  else
    xmax = block_width/2;

  if (octant & Y_MASK)
    ymin = block_height/2+1;
  else
    ymax = block_height/2;

  if (octant & Z_MASK)
    zmin = block_depth/2+1;
  else
    zmax = block_depth/2;
}

AdvectionGroup::AdvectionGroup()
  :workUnitCount(0)
{
  delu = new float***[numDims];
  delua = new float***[numDims];

  for (int d = 0; d < numDims; ++d) {
    delu[d] = new float**[block_width+2];
    delua[d] = new float**[block_width+2];
    for(int i=0; i<block_width+2; i++) {
      delu[d][i] = new float*[block_height+2];
      delua[d][i] = new float*[block_height+2];
      for (int j = 0; j < block_height+2; ++j) {
        delu[d][i][j] = new float[block_depth+2];
        delua[d][i][j] = new float[block_depth+2];
      }
    }
  }
}

AdvectionGroup::AdvectionGroup(CkMigrateMessage* m): CBase_AdvectionGroup(m){
  delu = new float***[numDims];
  delua = new float***[numDims];

  for (int d = 0; d < numDims; ++d) {
    delu[d] = new float**[block_width+2];
    delua[d] = new float**[block_width+2];
    for(int i=0; i<block_width+2; i++) {
      delu[d][i] = new float*[block_height+2];
      delua[d][i] = new float*[block_height+2];
      for (int j = 0; j < block_height+2; ++j) {
        delu[d][i][j] = new float[block_depth+2];
        delua[d][i][j] = new float[block_depth+2];
      }
    }
  }
}

void AdvectionGroup::incrementWorkUnitCount(int iterations) {
  workUnitCount++;
  workUnits[iterations]++;
  if(iterations % lb_freq == 0 || iterations % lb_freq == lb_freq-1){//this is the load balancing iteration
    minLoad[iterations] += 1;
    maxLoad[iterations] += 1;
    avgLoad[iterations] += 1;
  }
}

void AdvectionGroup::reduceWorkUnits() {
  CkCallback cb(CkReductionTarget(Main,totalWorkUnits), mainProxy);
  contribute(sizeof(int), &workUnitCount, CkReduction::sum_int, cb);
}

void AdvectionGroup::processQdTimes(map<int, pair<float, float> > peQdtimes, map<int, pair<float, float> > peRemeshtimes, map<int, int> peWorkunits,
                                    map<int, int> peminLoad, map<int, int> pemaxLoad, map<int, float> peavgLoad){
  for(map<int, std::pair<float, float> >::iterator it = peQdtimes.begin(); it!=peQdtimes.end(); it++){
    if(qdtimes.find(it->first) == qdtimes.end())
      qdtimes[it->first] = std::pair<float, float>(std::numeric_limits<float>::min(), std::numeric_limits<float>::max());
    qdtimes[it->first].first = max(qdtimes[it->first].first, it->second.first);
    qdtimes[it->first].second = min(qdtimes[it->first].second, it->second.second);
  }
  for(map<int, std::pair<float, float> >::iterator it = peRemeshtimes.begin(); it!=peRemeshtimes.end(); it++){
    if(remeshtimes.find(it->first) == remeshtimes.end())
      remeshtimes[it->first] = std::pair<float, float>(0, std::numeric_limits<float>::max());
    remeshtimes[it->first].first = max(remeshtimes[it->first].first, it->second.first);
    remeshtimes[it->first].second = min(remeshtimes[it->first].second, it->second.second);
  }
  
  for(map<int, int>::iterator it=peWorkunits.begin(); it!=peWorkunits.end(); it++){
    workUnits[it->first] += it->second;
    if(it->first % lb_freq == 0 || it->first % lb_freq == lb_freq-1){//this is the load balancing iteration
      minLoad[it->first] = std::min(minLoad[it->first], peminLoad[it->first]);
      maxLoad[it->first] = std::max(maxLoad[it->first], pemaxLoad[it->first]);
      avgLoad[it->first] += peavgLoad[it->first];
    }
  }
}

void AdvectionGroup::printLogs(){
    ckout << "qd times: ";
    for(map<int, pair<float, float> >::iterator it = qdtimes.begin(); it!=qdtimes.end(); it++)
      ckout << it->first << "," << it->second.second - it->second.first << " ";
    ckout << endl;

    ckout << "remesh times: ";
    for(map<int, pair<float, float> >::iterator it = remeshtimes.begin(); it!=remeshtimes.end(); it++)
      ckout << it->first << "," << it->second.second - it->second.first << " ";
    ckout << endl;

    ckout << "per iteration work units: ";
    for(map<int, int>::iterator it = workUnits.begin(); it!=workUnits.end(); it++)
      ckout << it->first << "," << it->second << " ";
    ckout << endl;
    
    ckout << "load balancing stats: ";
    for(map<int, int>::iterator it = minLoad.begin(); it!=minLoad.end(); it++){
      avgLoad[it->first]/=CkNumPes();
      ckout << it->first << "," << int(avgLoad[it->first]*100)/100. << "," << minLoad[it->first] << "," << maxLoad[it->first] << " ";
    }
    ckout << endl;
    CkExit();
}

void AdvectionGroup::meshGenerationPhaseIsOver(){
  //inInitialMeshGenerationPhase=false;
}

void Advection::prepareData4Exchange(){
  imsg=0;
  ghostReceived.clear();

  for (map<OctIndex, Neighbor>::iterator it = neighbors.begin(),
       iend = neighbors.end(); it != iend; ++it) {
      it->second.setDataSent(false);
  }

  // Cache boundary data in contiguous blocks

  // YZ surfaces
  for(int j=1; j <= block_height; ++j)
    for(int k=1; k <= block_depth; ++k) {
      left_surface[index_yz(j-1,k-1)] = u[index(1,j,k)];
      right_surface[index_yz(j-1,k-1)] = u[index(block_width,j,k)];
    }

  // XZ Surfaces
  for(int i=1; i <= block_width; ++i)
    for(int k=1; k <= block_depth; ++k) {
      top_surface[index_xz(i-1,k-1)] = u[index(i, block_height, k)];
      bottom_surface[index_xz(i-1,k-1)] = u[index(i, 1, k)];
    }

  // XY surfaces
  for(int i=1; i <= block_width; ++i)
    for(int j=1; j <= block_height; ++j) {
      forward_surface[index_xy(i-1,j-1)] = u[index(i, j, block_depth)];
      backward_surface[index_xy(i-1,j-1)] = u[index(i, j, 1)];
    }
}

void Advection::applyInitialCondition(){
  VB(logfile << "applying initial condition" << std::endl;);
  float rcub;
  for(int i=0; i<block_width+2; i++)
    for(int j=0; j<block_height+2; j++)
      for(int k=0; k<block_depth+2; k++){
        rcub = (x[i] - xctr)*(x[i]-xctr) +
               (y[j] - yctr)*(y[j]-yctr) +
               (z[k] - zctr)*(z[k]-zctr);
        u[index(i, j, k)] = (rcub<=radius*radius) ? 2:1;
        //VB(logfile << x[i] << " " << y[j] << " " << z[k] << ": " << rcub << " " << radius*radius << " " << u[index(i, j, k)] << std::endl;);
      }
}

void Advection::mem_allocate(float* &p, int size){
  p = new float[size];
}

void Advection::mem_allocate_all(){
  mem_allocate(u, (block_width+2)*(block_height+2)*(block_depth+2));
  mem_allocate(u2, (block_width+2)*(block_height+2)*(block_depth+2));
  mem_allocate(u3, (block_width+2)*(block_height+2)*(block_depth+2));

  mem_allocate(x, block_width+2);
  mem_allocate(y, block_height+2);
  mem_allocate(z, block_depth+2);

  mem_allocate(left_surface, block_height*block_depth);
  mem_allocate(right_surface, block_height*block_depth);

  mem_allocate(top_surface, block_width*block_depth);
  mem_allocate(bottom_surface, block_width*block_depth);

  mem_allocate(forward_surface, block_width*block_height);
  mem_allocate(backward_surface, block_width*block_height);
}

void Advection::mem_deallocate_all(){
  delete [] u;
  delete [] u2;
  delete [] u3;

  delete [] x;
  delete [] y;
  delete [] z;
  
  delete [] left_surface;
  delete [] right_surface;
  delete [] top_surface;
  delete [] bottom_surface;
  delete [] forward_surface;
  delete [] backward_surface;

}

Advection::Advection(float xmin, float xmax, float ymin, float ymax,
                     float zmin, float zmax)
{
  __sdag_init();

  thisIndex.getCoordinates(xc, yc, zc);
  dx = (xmax - xmin)/float(array_width);
  dy = (ymax - ymin)/float(array_height);
  dz = (zmax - zmin)/float(array_depth);

  nx = array_width/(num_chare_cols);
  ny = array_height/(num_chare_rows);
  nz = array_depth/(num_chare_Zs);

  myt = t;
  mydt = dt;

  this->xmin = xc*nx*dx;
  this->ymin = yc*ny*dy;
  this->zmin = zc*nz*dz;

  iterations=0;
  meshGenIterations=0;
  initializeRestofTheData();
}

void Advection::initializeRestofTheData(){
  usesAutoMeasure = false;
  usesAtSync = true;
  remeshStartTime = 0;
  mem_allocate_all();
  VB(logfile.open(string("log/"+thisIndex.getIndexString()+"log").c_str()););
  if(inInitialMeshGenerationPhase){
    for(int i=0; i<block_width+2; i++)
      x[i] = xmin + float(i)*dx - 0.5*dx;


    for(int i=0; i<block_height+2; i++)
      y[i] = ymin + float(i)*dy - 0.5*dy;

    for(int i=0; i<block_depth+2; i++)
      z[i] = zmin + float(i)*dz - 0.5*dz;

    applyInitialCondition();
  }

  FOR_EACH_CHILD
    child_isRefined[i] = false;
  END_FOR
  this->isRefined = false;

  FOR_EACH_NEIGHBOR
    VB(logfile << "neighbor in dir " << i << " is " << thisIndex.getNeighbor(i).getIndexString().c_str() << std::endl;);
    neighbors[thisIndex.getNeighbor(i)] = Neighbor(i);
  END_FOR

  parent = (thisIndex.getDepth()>min_depth) ? thisIndex.getParent() : thisIndex;
  resetMeshRestructureData();
  nChildDataRecvd = 0;
  phase1Over = false;
}

//added for array migration - see how 2D arrays can be packed
void Advection::pup(PUP::er &p){
  CBase_Advection::pup(p);
  __sdag_pup(p);

  p|isRefined;
  p|depth;

  PUParray(p, child_isRefined, NUM_CHILDREN);

  p|ghostReceived;

  p|decision;
  PUParray(p, child_decision, NUM_CHILDREN);

  p|parentHasAlreadyMadeDecision;
  p|hasReceivedParentDecision;
  PUParray(p, fname, 100);
  p|parent;
  p|neighbors;
  p|uncleDecisions;

  p|xc;
  p|yc;
  p|zc;
  p|imsg;
  p|nChildDataRecvd;
  p|phase1Over;

  if(p.isUnpacking()){
    mem_allocate_all();
    //resetMeshRestructureData();
  }

  PUParray(p, u, (block_width+2)*(block_height+2)*(block_depth+2));
  PUParray(p, x, block_width+2);
  PUParray(p, y, block_height+2);
  PUParray(p, z, block_depth+2);

  p|iterations;
  p|meshGenIterations;
  p|up_x;
  p|un_x;
  p|up_y;
  p|un_y;
  p|up_z;
  p|un_z;
  p|myt;
  p|mydt;

  p|dx; p|dy; p|dz;
  p|nx; p|ny; p|nz;
  p|xmin; p|xmax;
  p|ymin; p|ymax;
  p|zmin; p|zmax;

  p|itBeginTime;
  p|remeshStartTime;
  p|remeshEndTime;
  p|lastBusyTime; p|lastIdleTime;
  //p|logfile;
  p|lastIdleTimeQD;
  p|finishedPhase1;
  p|nChildDataRecvd;
  p|phase1Over;
  p|amr3d_i;
  p|ichild;
}

Advection::~Advection(){
  if (isLeaf){
    delete [] u;
    delete [] u2;
    delete [] u3;

    delete [] x;
    delete [] y;
    delete [] z;
    delete [] left_surface;
    delete [] right_surface;
    delete [] top_surface;
    delete [] bottom_surface;
    delete [] forward_surface;
    delete [] backward_surface;
  }
}

inline float downSample(float* u, int x, int y, int z) {
  return (
    u[index(x, y, z)]   + u[index(x+1, y, z)] +
    u[index(x, y+1, z)] + u[index(x+1, y+1, z)] +
    u[index(x, y, z+1)]   + u[index(x+1, y, z+1)] +
    u[index(x, y+1, z+1)] + u[index(x+1, y+1, z+1)]) / 8.0;
}

class surface_iterator {
  float *u;
  int minx, maxx;
  int miny, maxy;
  int minz, maxz;
  int curx, cury, curz;
  int dx, dy, dz;
  bool done;

  void incX() {
    curx += dx;
    if (curx > maxx || (dx == 0)) {
      curx = minx;
      incY();
    }
  }

  void incY() {
    cury += dy;
    if (cury > maxy || (dy == 0)) {
      cury = miny;
      incZ();
    }
  }

  void incZ() {
    curz += dz;
    if (curz > maxz || (dz == 0)) {
      done = true;
    }
  }

public:
  surface_iterator(float *u,
                   int minx, int maxx, int dx,
                   int miny, int maxy, int dy,
                   int minz, int maxz, int dz)
  : u(u), minx(minx), maxx(maxx), dx(dx),
          miny(miny), maxy(maxy), dy(dy),
          minz(minz), maxz(maxz), dz(dz),
          curx(minx), cury(miny), curz(minz),
          done(false) {
    assert((dx == 0) || (minx < maxx));
    assert((dy == 0) || (miny < maxy));
    assert((dz == 0) || (minz < maxz));
  }

  int getX() { assert(!done); return curx; }
  int getY() { assert(!done); return cury; }
  int getZ() { assert(!done); return curz; }

  inline surface_iterator& operator++() {
    assert(!done);
    incX();
    return *this;
  }

  inline float left()     { assert(!done); return u[index(curx-1, cury, curz)];  }
  inline float right()    { assert(!done); return u[index(curx+1, cury, curz)];  }
  inline float up()       { assert(!done); return u[index(curx, cury+1, curz)];  }
  inline float down()     { assert(!done); return u[index(curx, cury-1, curz)];  }
  inline float forward()  { assert(!done); return u[index(curx, cury, curz+1)];  }
  inline float backward() { assert(!done); return u[index(curx, cury, curz-1)];  }
  inline float& operator*() {
    assert(!done);
    return u[index(curx, cury, curz)];
  }

  bool isDone() {
    return done;
  }
};

float* Advection::getGhostBuffer(int dir) {
  switch (dir) {
    case UP:    return top_surface;
    case DOWN:  return bottom_surface;
    case LEFT:  return left_surface;
    case RIGHT: return right_surface;
    case FORWARD: return forward_surface;
    case BACKWARD: return backward_surface;
  }
}

int Advection::getGhostCount(int dir) {
  switch (dir) {
  case UP:    case DOWN: return block_width*block_depth;
  case RIGHT: case LEFT: return block_height*block_depth;
  case FORWARD: case BACKWARD: return block_height*block_width;
  //default: CkAbort("Asking for an unknown boundary's size");
  }
}

void Advection::sendGhost(int dir){
  int count = getGhostCount(dir);
  VB(logfile << "ghost count in direction " << dir << " = " << count << std::endl;);
  float* boundary;

  OctIndex QI = thisIndex.getNeighbor(dir);
  map<OctIndex, Neighbor>::iterator I = neighbors.find(QI);

  if (I == neighbors.end()) {
    VB(logfile << "neighbor is an uncle" << std::endl;);
    // Uncle case, neighbor doesn't exist in this direction (at this level)
    OctIndex receiver = QI.getParent();

    // TODO: Remove use of subdirections
    boundary = getGhostBuffer(dir);
    count /= 4;

    int xmin, xmax, ymin, ymax, zmin, zmax;
    populateRanges(dir, -1, xmin, xmax, ymin, ymax, zmin, zmax);

    int dx = (xmin == xmax) ? 0 : 2;
    int dy = (ymin == ymax) ? 0 : 2;
    int dz = (zmin == zmax) ? 0 : 2;
    if(xmin!=xmax)xmin++;
    else if(xmin==0) xmin = xmax = 1;
    else if(xmin==block_width+1) xmin = xmax = block_width-1;

    if(ymin!=ymax)ymin++;
    else if(ymin==0) ymin = ymax = 1;
    else if(ymin==block_width+1) ymin = ymax = block_height-1;

    if(zmin!=zmax)zmin++;
    else if(zmin==0) zmin = zmax = 1;
    else if(zmin==block_width+1) zmin = zmax = block_depth-1;

    surface_iterator iter(u, xmin, xmax, dx,
                             ymin, ymax, dy,
                             zmin, zmax, dz);
    int k;
    for (k = 0; !iter.isDone(); ++iter, ++k)
      boundary[k] = downSample(u, iter.getX(), iter.getY(), iter.getZ());
    assert(k == count);

    thisProxy(receiver).receiveGhosts(iterations, getSourceDirection(dir), thisIndex.getOctant(), count, boundary);
  } else if (!I->second.isRefined()) {
    // Friend
    VB(logfile << "neighbor " << QI.getIndexString() << " is a friend" << std::endl;);
    boundary = getGhostBuffer(dir);
    thisProxy(QI).receiveGhosts(iterations, getSourceDirection(dir), -1, count, boundary);
  }
}

void Advection::process(int iteration, int dir, int quadrant, int size, float gh[]){
  VB(logfile << "received ghost from direction " << dir << ", octant " << quadrant << std::endl;);
  bool fromNephew = (quadrant >= 0);

  OctIndex QI = thisIndex.getNeighbor(dir);
  if (fromNephew) QI = QI.getChild(quadrant);
  //assert(!ghostReceived.count(QI));
  ghostReceived.insert(QI);

  imsg += (fromNephew) ? 0.25:1;

  int xmin, xmax, ymin, ymax, zmin, zmax;
  populateRanges(dir, quadrant, xmin, xmax, ymin, ymax, zmin, zmax);
  VB(logfile << "after populate ranges " << " (iteration : " << iteration << ") " << dir << " " << quadrant << " " \
             << xmin << " " << xmax << " " << ymin << " " << ymax << " " \
             << zmin << " " << zmax << std::endl;);
  int dx = (xmin == xmax) ? 0 : 1;
  int dy = (ymin == ymax) ? 0 : 1;
  int dz = (zmin == zmax) ? 0 : 1;
  if (xmax != xmin) {
    xmin++;
    xmax++;
  }
  if (ymax != ymin) {
    ymin++;
    ymax++;
  }
  if (zmax != zmin) {
    zmin++;
    zmax++;
  }

  surface_iterator iter(u, xmin, xmax, dx,
                           ymin, ymax, dy,
                           zmin, zmax, dz);

  for(int i=0; i<size; ++i, ++iter){
      VB(
        if(y[iter.getY()]==0.46875) 
            //logfile << std::setw(5) << gh[i] << " ";
            logfile << "setting " << x[iter.getX()] << ", " << y[iter.getY()] << ", " << z[iter.getZ()] << " = " << gh[i] << std::endl;
       ); 
      *iter = gh[i];
  }
  VB(logfile << std::endl;);
  assert(iter.isDone());
}

void Advection::sendReadyData(){
  //check if data can be sent to any of the refined neighbors
  //If the neighbors are at the same level or do not exist at all 
  //data will be sent in begin_iteration function and need 
  //not be sent here
  for (map<OctIndex, Neighbor>::iterator it = neighbors.begin(),
       iend = neighbors.end(); it != iend; ++it) {
    Neighbor &N = it->second;
    if(N.isRefined() && !N.isDataSent()) {
      std::vector<OctIndex> children;
      getChildrenInDir(thisIndex, N.getDir(), children);

      vector<OctIndex> allNeighbors;
      for (std::vector<OctIndex>::iterator I = children.begin(),
           E = children.end(); I != E; ++I) {
        vector<OctIndex> neighbors = I->getNeighbors();
        allNeighbors.insert(allNeighbors.end(),
                            neighbors.begin(), neighbors.end());
      }

      // TODO: change to iterator
      bool receivedAll = true;
      for(int i = 0; i < allNeighbors.size(); i++) {
        if(allNeighbors[i].getParent() != thisIndex) { // external neighbor
          if(!ghostReceived.count(allNeighbors[i]) && !ghostReceived.count(allNeighbors[i].getParent())) {
            receivedAll = false;
            break;
          }
        }
      }

      if(receivedAll) {
        interpolateAndSend(N.getDir());
        N.setDataSent(true);
      }
    }
  }
}

int Advection::getSourceDirection(int NBR) {
  switch (NBR) {
  case UP:    return DOWN;
  case DOWN:  return UP;
  case LEFT:  return RIGHT;
  case RIGHT: return LEFT;
  case FORWARD: return BACKWARD;
  case BACKWARD: return FORWARD;
  }
}
    
void Advection::interpolateAndSend(int dir) {
  int uncledir = getSourceDirection(dir);
  std::vector<OctIndex> children;
  getChildrenInDir(thisIndex.getNeighbor(dir), uncledir, children);
  for (std::vector<OctIndex>::iterator I = children.begin(),
       E = children.end(); I != E; ++I)
    interpolateAndSendToNephew(uncledir, *I);
}

void Advection::interpolateAndSendToNephew(int uncledir, OctIndex QI) {
  float *boundary;
  int count = getGhostCount(uncledir);

  // TODO: Better generalize this code!
  int p = 1, m = -1;
  float sx_r, sx_l, sy_u, sy_d, sz_f, sz_b;

  float *sx[]={&sx_l, &sx_r};
  float *sy[]={&sy_d, &sy_u};
  float *sz[]={&sz_b, &sz_f};

  int octant = QI.getOctant();

  int xmin, xmax, ymin, ymax, zmin, zmax;
  populateRanges(getSourceDirection(uncledir), octant,
                 xmin, xmax,
                 ymin, ymax,
                 zmin, zmax);

  int dx = 1, dy = 1, dz = 1;
  unsigned columncount;
  float **a, **b, *c;
  if (xmin == xmax) {
    a = sy;
    b = sz;
    c = (xmax == 0) ? &sx_l : &sx_r;
    columncount = block_height;
    dx = 0;
    ymin++; ymax++;
    zmin++; zmax++;
    if(xmin==0){
        xmin = xmax = 1;
    }else{
        xmin = xmax = block_width;
    }

  }
  if (ymin == ymax) {
    a = sx;
    b = sz;
    c = (ymax == 0) ? &sy_d : &sy_u;
    columncount = block_width;
    dy = 0;
    xmin++; xmax++;
    zmin++; zmax++;
    if(ymin==0){
        ymin = ymax = 1;
    }else{
        ymin = ymax = block_height;
    }
  }
  if (zmin == zmax) {
    a = sx;
    b = sy;
    c = (zmax == 0) ? &sz_b : &sz_f;
    columncount = block_width;
    dz = 0;
    xmin++; xmax++;
    ymin++; ymax++;
    if(zmin==0){
        zmin = zmax = 1;
    }else{
        zmin = zmax = block_depth;
    }
  }

  surface_iterator in(u, xmin, xmax, dx,
                         ymin, ymax, dy,
                         zmin, zmax, dz);

  boundary = getGhostBuffer(getSourceDirection(uncledir));

  unsigned counter = 0;
  for (; !in.isDone(); ++in) {
    sx_r = (in.right()      - *in) / 4;
    sx_l = (in.left()       - *in) / 4;
    sy_u = (in.up()         - *in) / 4;
    sy_d = (in.down()       - *in) / 4;
    sz_f = (in.forward()    - *in) / 4;
    sz_b = (in.backward()  -  *in) / 4;

    int a_pos = counter % columncount;
    int b_pos = counter / columncount;
    counter++;

    boundary[index2d(2*a_pos,   2*b_pos,   columncount)] = *in + *a[0] + *b[0] + *c;
    boundary[index2d(2*a_pos+1, 2*b_pos,   columncount)] = *in + *a[1] + *b[0] + *c;
    boundary[index2d(2*a_pos,   2*b_pos+1, columncount)] = *in + *a[0] + *b[1] + *c;
    boundary[index2d(2*a_pos+1, 2*b_pos+1, columncount)] = *in + *a[1] + *b[1] + *c;
  }
  assert(counter == count/(1 << (numDims-1)));

  thisProxy(QI).receiveGhosts(iterations, uncledir, -2, count, boundary);
}

void Advection::compute(){
  //if(iterations==1){
#ifdef LOGGER
    char logfilename[100];
    sprintf(logfilename, "out/snap_%s_%d.vtk", thisIndex.getIndexString().c_str(), iterations);
    logfile.open(logfilename);
    logfile.precision(8);
    //logfile << "Variables=\"X\",\"Y\",\"Z\",\"radius\",\"nodeid\"\n";
    //logfile << "Zone I = 16, J = 16, K = 16, F = POINT\n";
    printData();
    logfile.close();
    /*int dims[] = {block_height+1, block_width+1, block_depth+1};
    int vardims[] = {1};
    int centering[] = {0};
    const char *varnames[]  = {"viscosity"};
    //varnames = new char*[1];
    //varnames[0] = "viscosity";
    
    float *vars[1];
    vars[0] = new float[block_width*block_height*block_depth];
    for(int i=0; i<block_width; i++)
        for(int j=0; j<block_height; j++)
            for(int k=0; k<block_depth; k++){
                vars[0][(k*block_height+j)*block_width+i] = u[index(i+1,j+1,k+1)];
                //ckout << x[i+1] << " " << y[j+1] << " " << z[k+1] << " " << u[index(i+1,j+1,k+1)] << endl;
            }
    float *xn = new float[block_width+1];
    float *yn = new float[block_height+1];
    float *zn = new float[block_depth+1];
    for(int i=0; i<block_width+1; i++) {xn[i]=xmin+dx*i;}
    for(int i=0; i<block_height+1; i++){yn[i]=ymin+dy*i;}
    for(int i=0; i<block_depth+1; i++) {zn[i]=zmin+dz*i;}
    //float *vars[] = {u};
    write_rectilinear_mesh(logfilename, 0, dims, xn, yn, zn, 1, vardims, centering, varnames, vars);
    delete [] vars[0]; delete [] xn; delete [] yn; delete [] zn;
    for(int k=0; k<=block_depth+1; k++){
        for(int j=0; j<=block_height+1; j++){
            for(int i=0; i<=block_width+1; i++){
                if(y[j]==0.484375)
                    logfile << std::setw(10) << u[index(i,j,k)] << " ";
            }
        }
        logfile << std::endl;
    }*/
#endif
  //}
  memcpy(u2, u, sizeof(float)*(block_width+2)*(block_height+2)*(block_depth+2));
  memcpy(u3, u, sizeof(float)*(block_width+2)*(block_height+2)*(block_depth+2));
  FOR_EACH_ZONE
      up_x = (u[index(i+1,j,k)] - u[index(i,j,k)])/dx;
      un_x = (u[index(i,j,k)]-u[index(i-1,j,k)])/dx;
      up_y = (u[index(i,j+1,k)] - u[index(i,j,k)])/dy;
      un_y = (u[index(i,j,k)] - u[index(i,j-1,k)])/dy;
      up_z = (u[index(i,j,k+1)] - u[index(i,j,k)])/dz;
      un_z = (u[index(i,j,k)] - u[index(i,j,k-1)])/dz;

      u2[index(i,j,k)] = u[index(i,j,k)] - dt* (apx*un_x + anx*up_x) - dt*(apy*un_y + any*up_y) - dt*(apz*un_z + anz*up_z);
  END_FOR

  // =========================================
  FOR_EACH_ZONE
      up_x = (u2[index(i+1,j,k)] - u2[index(i,j,k)])/dx;
      un_x = (u2[index(i,j,k)] - u2[index(i-1,j,k)])/dx;
      up_y = (u2[index(i,j+1,k)] - u2[index(i,j,k)])/dy;
      un_y = (u2[index(i,j,k)] - u2[index(i,j-1,k)])/dy;
      up_z = (u2[index(i,j,k+1)] - u2[index(i,j,k)])/dz;
      un_z = (u2[index(i,j,k)] - u2[index(i,j,k-1)])/dz;

      u3[index(i,j,k)] = u2[index(i,j,k)] - dt* (apx*un_x + anx*up_x) - dt*(apy*un_y + any*up_y) - dt*(apz*un_z + anz*up_z);
      u[index(i,j,k)] = 0.5*(u2[index(i,j,k)] + u3[index(i,j,k)]);
  END_FOR

#if 0
    logfile << "after compute" << std::endl;
    for(int k=1; k<=block_depth; k++){
        for(int j=1; j<=block_height; j++){
            for(int i=1; i<=block_width; i++){
                if(y[j]==0.484375)
                    logfile << std::setw(10) << u[index(i,j,k)] << " ";
            }
        }
        logfile << std::endl;
    }
  #endif
}

Decision Advection::getGranularityDecision(){
  float delx = 0.5/dx;
  float dely = 0.5/dy;
  float delz = 0.5/dz;
  float error=0;

  AdvectionGroup *ppcGrp = ppc.ckLocalBranch();

  for(int i=1; i <= block_width; i++){
    for(int j=1; j<=block_height; j++){
      for(int k=1; k<=block_depth; k++){
        // d/dx
        ppcGrp->delu[0][i][j][k] = (u[index(i+1, j, k)] - u[index(i-1, j, k)])*delx;
        ppcGrp->delua[0][i][j][k] = abs(u[index(i+1, j, k)]) + abs(u[index(i-1, j, k)])*delx;

        // d/dy
        ppcGrp->delu[1][i][j][k] = (u[index(i, j+1, k)] - u[index(i, j-1, k)])*dely;
        ppcGrp->delua[1][i][j][k] = (abs(u[index(i, j+1, k)]) + abs(u[index(i, j-1, k)]))*dely;

        // d/dz
        ppcGrp->delu[2][i][j][k] = (u[index(i, j, k+1)] - u[index(i, j, k-1)])*delz;
        ppcGrp->delua[2][i][j][k] = (abs(u[index(i, j, k+1)]) + abs(u[index(i, j, k-1)]))*delz;
      }
    }
  }

  int istart=2, iend=block_width-1,
      jstart=2, jend=block_height-1,
      kstart=2, kend=block_depth-1;
  for (int i=istart;i<=iend;i++){
    for (int j=jstart;j<=jend;j++){
      for (int k=kstart;k<=kend;k++){
        for (int d = 0; d < numDims; ++d) {
          ppcGrp->delu2[3*d+0] = (ppcGrp->delu[d][i+1][j][k] - ppcGrp->delu[d][i-1][j][k])*delx;
          ppcGrp->delu3[3*d+0] = (abs(ppcGrp->delu[d][i+1][j][k]) + abs(ppcGrp->delu[d][i-1][j][k]))*delx;
          ppcGrp->delu4[3*d+0] = (ppcGrp->delua[d][i+1][j][k] + ppcGrp->delua[d][i-1][j][k])*delx;

          ppcGrp->delu2[3*d+1] = (ppcGrp->delu[d][i][j+1][k] - ppcGrp->delu[d][i][j-1][k])*dely;
          ppcGrp->delu3[3*d+1] = (abs(ppcGrp->delu[d][i][j+1][k]) + abs(ppcGrp->delu[d][i][j-1][k]))*dely;
          ppcGrp->delu4[3*d+1] = (ppcGrp->delua[d][i][j+1][k] + ppcGrp->delua[d][i][j-1][k])*dely;

          ppcGrp->delu2[3*d+2] = (ppcGrp->delu[d][i][j][k+1] - ppcGrp->delu[d][i][j][k-1])*delz;
          ppcGrp->delu3[3*d+2] = (abs(ppcGrp->delu[d][i][j][k+1]) + abs(ppcGrp->delu[d][i][j][k-1]))*delz;
          ppcGrp->delu4[3*d+2] = (ppcGrp->delua[d][i][j][k+1] + ppcGrp->delua[d][i][j][k-1])*delz;
        }

        // compute the error
        float num = 0.;
        float denom = 0.;

        for (int kk = 0; kk < numDims2; kk++){  // kk= 1, 2, 3, 4, 5, ... 9
          num = num + pow(ppcGrp->delu2[kk],2.);
          denom = denom + pow(ppcGrp->delu3[kk], 2.) + (refine_filter*ppcGrp->delu4[kk])*2;
        }
        // compare the square of the error
        if (denom == 0. && num != 0.){
          error = std::numeric_limits<float>::max();
        } else if (denom != 0.0){
          error = std::max(error, num/denom);
        }
      }
    }
  }
  error = sqrt(error);
  if(error < derefine_cutoff && thisIndex.getDepth() > min_depth) return COARSEN;
  else if(error > refine_cutoff && thisIndex.getDepth() < max_depth) return REFINE;  
  else return STAY;
}

void Advection::resetMeshRestructureData(){
  decision = INV;
  parentHasAlreadyMadeDecision=false;
  hasReceivedParentDecision=false;

  uncleDecisions.clear();

  for (map<OctIndex, Neighbor>::iterator it = neighbors.begin(),
       iend = neighbors.end(); it != iend; ++it) {
    it->second.resetDecision();
  }

  for (int i = 0; i < NUM_CHILDREN; ++i)
    child_decision[i] = INV;
}

void Advection::makeGranularityDecisionAndCommunicate(){
  if(isLeaf) {//run this on leaf nodes
    Decision newDecision = (decision!=REFINE)?max(decision, getGranularityDecision()):decision;
    VB(logfile << thisIndex.getIndexString().c_str() << " decision = " << newDecision << std::endl;);
    updateDecisionState(1, newDecision);
  }
  else if(isGrandParent() && !parentHasAlreadyMadeDecision) informParent(meshGenIterations,-1, INV, 1);
}

/***** PHASE1 FUNCTIONS****/
void Advection::updateDecisionState(int cascade_length, Decision newDecision) {
  cascade_length++;

  assert(isLeaf);

  if (decision == newDecision) return;

  assert((decision != REFINE || !isRefined) && "Re-refining?!");

  decision = newDecision;
  if (decision == COARSEN) return; // Don't communicate the 'default' decision

  for (int i=0; i<NUM_NEIGHBORS; i++) {
    OctIndex QI = thisIndex.getNeighbor(i);

    if (neighbors.count(QI)) {
      Neighbor & N = neighbors[QI];
      if (!N.isRefined()) {
        // isFriend
        VB(logfile << "sending exchangePhase1Msg, decision = " << decision << ", receiver = " << QI.getIndexString() << std::endl;);
        thisProxy(QI).exchangePhase1Msg(meshGenIterations, getSourceDirection(i), -1, decision, cascade_length);
      } else {
        // isNephew
        //Get Corresponding Children of the neighbor
        std::vector<OctIndex> children;
        getChildrenInDir(QI, getSourceDirection(i), children);
        // XXX: -2 means "we're your uncle"
        for (std::vector<OctIndex>::iterator I = children.begin(),
             E = children.end(); I != E; ++I){
        VB(logfile << "sending exchangePhase1Msg, decision = " << decision << ", receiver = " << I->getIndexString() << std::endl;);
        thisProxy(*I).exchangePhase1Msg(meshGenIterations, getSourceDirection(i), -2, decision, cascade_length);
        }
      }
    } else {
      // Does not exist, talk to uncle
      VB(logfile << "sending exchangePhase1Msg, decision = " << decision << ", receiver = " << QI.getParent().getIndexString() << std::endl;);
      thisProxy(QI.getParent()).exchangePhase1Msg(meshGenIterations, getSourceDirection(i), thisIndex.getOctant(), decision, cascade_length);
    }
  }

  if(parent != thisIndex){
    VB(logfile << "sending informParent, decision = " << decision << std::endl;);
    thisProxy(parent).informParent(meshGenIterations, thisIndex.getOctant(), decision, cascade_length);
  }
}

// Will be called from two contexts:
//  a) If the parent is a grandparent and also have children that are leaves
//  b) when a child sends REFINE/STAY message to the parent
void Advection::processChildDecision(int childNum, Decision dec, int cascade_length) {
  VB(logfile << "recvd informParent, childNum " << childNum << std::endl;);
  if(childNum >= 0) child_decision[childNum]=dec;

  if(dec==REFINE) child_isRefined[childNum]=true;
  
  if(parentHasAlreadyMadeDecision == false){
    parentHasAlreadyMadeDecision = true;
    FOR_EACH_CHILD
      if(i!=childNum && !child_isRefined[i]){
        VB(logfile << "sending message to child " << thisIndex.getChild(i).getIndexString().c_str() << ", miterattions " << meshGenIterations << std::endl;);
        thisProxy(thisIndex.getChild(i)).recvParentDecision(meshGenIterations, cascade_length);
      }
    END_FOR
    if(parent!=thisIndex)
      thisProxy(parent).informParent(meshGenIterations, thisIndex.getOctant(), STAY, cascade_length);
  }
}

void Advection::processParentDecision(int cascade_length) {
  hasReceivedParentDecision = true;
  Decision newDecision = std::max(STAY, decision);
  if(isLeaf) updateDecisionState(cascade_length, newDecision);
}

void Advection::processPhase1Msg(int dir, int quadrant, Decision remoteDecision, int cascade_length) {
  VB(logfile << "isLeaf: " << isLeaf << std::endl;);
  Decision newDecision = decision;
  OctIndex QI = thisIndex.getNeighbor(dir);
  VB(logfile << "received exchangePhase1Msg, dir " << dir << ", quadrant " << quadrant << ", idx " << QI.getIndexString() << std::endl;);
  VB(logfile << QI.getIndexString() << " decision: " << remoteDecision << std::endl;);
  if(!isLeaf)
    logfile << "only leaves should exchange remesh messages" << std::endl;
  assert(isLeaf && "Only leaves should exchange remesh messages");

  if (quadrant == -2)
    assert(!neighbors.count(QI) && "Uncle in our neighbor map");
  else{
    VB(if(neighbors.count(QI)==0){
      logfile << thisIndex.getIndexString().c_str() << " didn't knew that " << QI.getIndexString().c_str() << " existed" << std::endl;

      for(map<OctIndex, Neighbor>::iterator it = neighbors.begin(); it != neighbors.end(); it++)
        logfile << it->first.getIndexString().c_str() << std::endl;
    });
    assert(neighbors.count(QI) && "Received message from neighbor we didn't know existed!");
  }

  // Ignore decisions sent from our uncle
  if (!neighbors.count(QI)) {
    VB(logfile << "received message from uncle" << std::endl;);
    OctIndex uncleIndex = QI.getParent();
    if (uncleDecisions.count(uncleIndex)) {
      uncleDecisions[uncleIndex] = max(uncleDecisions[uncleIndex], remoteDecision);
    } else {
      uncleDecisions[uncleIndex] = remoteDecision;
    }
    VB(logfile << "done processing message: decision = " << decision << std::endl;);
    return;
  }

  Neighbor &N = neighbors[QI];

  if(quadrant == -1)
    assert(!N.isRefined());
  else
    assert(N.isRefined());
  remoteDecision = N.setDecision(remoteDecision, quadrant);

  if(!N.isRefined()) {
    if(remoteDecision == REFINE) {
      newDecision = std::max(STAY, decision);
    }
  } else {
    newDecision = std::max(decision, remoteDecision);
  }

  updateDecisionState(cascade_length, newDecision);
}

/**** PHASE2 FUNCTIONS ****/
void Advection::doPhase2(){
  VB(logfile << "in doPhase2, iteration = " << iterations << " decision = " << decision << std::endl;);
  if(decision == COARSEN){//send data to the parent
    vector<float> child_u;
    if(inInitialMeshGenerationPhase==false){
      child_u.resize((block_height*block_width*block_depth)/8);
      for(int i=1; i<= block_width; i+=2)
        for(int j=1; j<=block_height; j+=2)
          for(int k=1; k<=block_depth; k+=2)
            child_u[index_c(i/2, j/2, k/2)] = downSample(u, i, j, k);
    }
    VB(logfile << "coarsening .. sending data to parent " << meshGenIterations << std::endl;);
    thisProxy(parent).recvChildData(meshGenIterations, thisIndex.getOctant(), myt, mydt, meshGenIterations, iterations, child_u, neighbors, uncleDecisions);
    thisProxy[thisIndex].ckDestroy();
    //thisProxy.doneInserting();
    return;
  }
  else if(decision == REFINE) refine();

  updateMeshState();
  resetMeshRestructureData();
  //iterate();
}

void Advection::updateMeshState(){
  //Update the Status of Your Neighbors, need to be done only if you are going to stay in that position
  if(isLeaf && decision == STAY){

    std::vector<OctIndex> ToRemove;
    for (map<OctIndex, Neighbor>::iterator it = neighbors.begin(),
         iend = neighbors.end(); it != iend; ++it) {
      Neighbor &N = it->second;
      if (!N.isRefined()) {
        switch(N.getDecision()) {
          case REFINE:      N.setRefined(true); break;
          case STAY:        N.setRefined(false); break;
          case COARSEN:     ToRemove.push_back(it->first); break;
          //default:          CkAbort("nbr_decision not set");
        }
      }
      else {
        // TODO: Only check nephews sharing a surface with us
        // For now, abuse the fact that default is COARSEN
        // and only nephews adjacent to us send messages,
        // so the rest of the data is invalid anyway.
        Decision D = COARSEN;
        for (int i = 0; i < NUM_CHILDREN; ++i)
          D = std::max(D, N.getDecision(i));
        switch (D) {
          case STAY:      N.setRefined(true); break;
          case COARSEN:   N.setRefined(false); break;
          case REFINE:    CkAbort("unacceptable decision");
          //default:        CkAbort("nbr_decision not set");
        }
      }
    }

    for (int i = 0; i < NUM_NEIGHBORS; ++i) {
      OctIndex QI = thisIndex.getNeighbor(i);
      OctIndex uncleIndex = QI.getParent();
      if (uncleDecisions.count(uncleIndex)) {
        assert(!neighbors.count(uncleIndex));
        assert(!neighbors.count(QI));

        Decision d = uncleDecisions[uncleIndex];
        if (d == REFINE) {
          neighbors[QI] = Neighbor(i);
        }
      }
    }

    // Remove the elements marked, avoid iterator badness
    for (std::vector<OctIndex>::iterator I = ToRemove.begin(),
         E = ToRemove.end(); I != E; ++I) {
      VB(logfile << "erasing " << I->getIndexString().c_str() << std::endl;);
      assert(neighbors.count(*I));
      neighbors.erase(*I);
    }
  }
  else if(decision == REFINE){
    isRefined=true;
  }
  else if(isRefined && !isGrandParent() && !parentHasAlreadyMadeDecision){
    VB(logfile << thisIndex.getIndexString().c_str() << " setting isRefined to false "
          << isRefined << " " << isGrandParent() << " " << parentHasAlreadyMadeDecision << std::endl;);
    isRefined = false;
    //set up the sdag for receving child data
    wait4ChildData();
    return;
  }

  if(isGrandParent()) {
    FOR_EACH_CHILD
      if(child_decision[i]==INV && child_isRefined[i])//did not receive any message
        child_isRefined[i]=false;
    END_FOR
  }
  iterate();
}

void Advection::recvChildData(int childNum, float myt, float mydt, 
                              int meshGenIterations, int iterations, vector<float> child_u, 
                              map<OctIndex, Neighbor> childNeighbors,
                              map<OctIndex, Decision> childUncleDecisions){
  VB(logfile << "recvd data from child: " << childNum << std::endl;);
  this->myt = myt;
  this->mydt = mydt;
  this->iterations = iterations;
  this->meshGenIterations = meshGenIterations;

  int st_i, end_i, st_j, end_j, st_k, end_k;
  getOctantRange(childNum, st_i, end_i, st_j, end_j, st_k, end_k);

  int ctr=0; float rsq;

  // If a leaf is coarsening into us, we should know
  // that we are no longer refined.  Check this now.
  if(isRefined) VB(logfile << "leaves coarsened into refined node" << std::endl;);
  assert(!isRefined && "Leaves coarsened into refined node?");

  if(inInitialMeshGenerationPhase) applyInitialCondition();
  else
    for (int k=st_k; k<=end_k; k++)
      for(int j=st_j; j<=end_j; j++){
        for(int i=st_i; i<=end_i; i++)
          u[index(i,j,k)]=child_u[ctr++];
      }

  //Update the Status of Your childNeighbors based on Data Sent from the Children

  int c1, c2, c3;
  switch(childNum){
    case 0: c1=LEFT;  c2=DOWN; c3=BACKWARD; break;
    case 1: c1=LEFT;  c2=DOWN; c3=FORWARD;  break;
    case 2: c1=LEFT;  c2=UP;   c3=BACKWARD; break;
    case 3: c1=LEFT;  c2=UP;   c3=FORWARD;  break;
    case 4: c1=RIGHT; c2=DOWN; c3=BACKWARD; break;
    case 5: c1=RIGHT; c2=DOWN; c3=FORWARD;  break;
    case 6: c1=RIGHT; c2=UP;   c3=BACKWARD; break;
    case 7: c1=RIGHT; c2=UP;   c3=FORWARD;  break;
  }

  this->iterations  = iterations;
  this->myt         = myt;
  this->mydt        = mydt;
  setNbrStateUponCoarsening(c1, childNum, childNeighbors, childUncleDecisions);
  setNbrStateUponCoarsening(c2, childNum, childNeighbors, childUncleDecisions);
  setNbrStateUponCoarsening(c3, childNum, childNeighbors, childUncleDecisions);

  resetMeshRestructureData();
  //if (nChildDataRecvd++ == NUM_CHILDREN){
  //  nChildDataRecvd = 0;
  //  iterate();
  //}
}

inline void Advection::setNbrStateUponCoarsening(int dir, int childNum, std::map<OctIndex, Neighbor> & childNeighbors, std::map<OctIndex, Decision> & childUncleDecisions) {
  OctIndex QIParent = thisIndex.getNeighbor(dir);
  OctIndex QIChild = thisIndex.getChild(childNum).getNeighbor(dir);

  if (childNeighbors.count(QIChild)) {
    Neighbor parentNeighbor = Neighbor(dir);
    Neighbor childNeighbor = childNeighbors[QIChild];

    if(childNeighbor.isRefined()) {
      parentNeighbor.setRefined(true);
    } else {
      switch(childNeighbor.getDecision()) {
        case COARSEN: parentNeighbor.setRefined(false); break;
        case STAY:    parentNeighbor.setRefined(true); break;
      }
    }

    // assert(!neighbors.count(QIParent));
    neighbors[QIParent] = parentNeighbor;
  } else if(neighbors.count(QIParent)) {
    Neighbor& N = neighbors[QIParent];

    // Okay, last we heard we do have a neighbor
    // in this direction, and because the first branch above
    // wasn't taken we know that this neighbor doesn't have a child.
    // Question is: does it even exist?
    // Look to our child (that is just not merging into us) for
    // if it received a decision (non-COARSEN), and act accordingly.
    // If it didn't receive a decision, the neighbor's decision
    // must have been to coarsen, and remove it.
    Decision D = COARSEN;
    if (childUncleDecisions.count(QIParent)) {
      D = childUncleDecisions[QIParent];
    }
    switch(D) {
      case REFINE: N.setRefined(true); break;
      case STAY: N.setRefined(false); break;
      case COARSEN: neighbors.erase(QIParent);
    }
  } else if (childUncleDecisions.count(QIParent)) {
    // Our child has information about our neighbor,
    // but we DON'T.  This is okay, this can happen
    // if *our* uncle refined in the same iteration
    // we did, as we only update our 'neighbor state'
    // if we're STAY.  Anyway, look at what our
    // neighbor decided to do and update accordingly:
    switch(childUncleDecisions[QIParent]) {
      case STAY:
        neighbors[QIParent] = Neighbor(dir);
        break;
      case REFINE:
        neighbors[QIParent] = Neighbor(dir);
        neighbors[QIParent].setRefined(true);
        break;
      case COARSEN: break; // We already don't know about it
    }
  }
}

void Advection::interpolate(float *u, vector<float>& refined_u, int xstart, int xend, int ystart, int yend, int zstart, int zend){
  float sx_l, sx_r, sy_u, sy_d, sz_b, sz_f;
  for(int i=xstart, m=1; i<=xend; i++, m++){
    for(int j=ystart, n=1; j<=yend; j++, n++){
      for(int k=zstart, o=1; k<=zend; k++, o++){
        sx_l = (-u[index(i-1,j,k)]+u[index(i,j,k)]) / 4;
        sx_r = (-u[index(i,j,k)]+u[index(i+1,j,k)]) / 4;

        sy_d = (-u[index(i,j-1,k)]+u[index(i,j,k)]) / 4;
        sy_u = (-u[index(i,j,k)]+u[index(i,j+1,k)]) / 4;

        sz_b = (-u[index(i,j,k-1)]+u[index(i,j,k)]) / 4;
        sz_f = (-u[index(i,j,k)]+u[index(i,j,k+1)]) / 4;

        refined_u[index_l(2*(m-1),   2*(n-1)  , 2*(o-1))] = u[index(i,j,k)] - sx_l - sy_d - sz_b;
        refined_u[index_l(2*(m-1)+1, 2*(n-1)  , 2*(o-1))] = u[index(i,j,k)] + sx_r - sy_d - sz_b;
        refined_u[index_l(2*(m-1),   2*(n-1)+1, 2*(o-1))] = u[index(i,j,k)] - sx_l + sy_u - sz_b;
        refined_u[index_l(2*(m-1)+1, 2*(n-1)+1, 2*(o-1))] = u[index(i,j,k)] + sx_r + sy_u - sz_b;
        refined_u[index_l(2*(m-1),   2*(n-1)  , 2*(o-1)+1)] = u[index(i,j,k)] - sx_l - sy_d + sz_f;
        refined_u[index_l(2*(m-1)+1, 2*(n-1)  , 2*(o-1)+1)] = u[index(i,j,k)] + sx_r - sy_d + sz_f;
        refined_u[index_l(2*(m-1),   2*(n-1)+1, 2*(o-1)+1)] = u[index(i,j,k)] - sx_l + sy_u + sz_f;
        refined_u[index_l(2*(m-1)+1, 2*(n-1)+1, 2*(o-1)+1)] = u[index(i,j,k)] + sx_r + sy_u + sz_f;
      }
    }
  }
}

void Advection::refineChild(unsigned int sChild, int xstart, int xend, int ystart, int yend, int zstart, int zend, float xmin, float ymin, float zmin) {
  OctIndex child = thisIndex.getChild(sChild);

  vector<float> refined_u;
  if(!inInitialMeshGenerationPhase){
    refined_u.resize(block_width*block_height*block_depth);
    interpolate(u, refined_u, xstart, xend, ystart, yend, zstart, zend);
  }
  VB(logfile << thisIndex.getIndexString().c_str() << " isRefined = " << isRefined << std::endl; 
  logfile << thisIndex.getIndexString().c_str() << " inserting " << child.getIndexString().c_str() << std::endl;);
  thisProxy(child).insert(dx/2, dy/2, dz/2, myt, mydt, xmin, ymin, zmin, meshGenIterations, iterations, refined_u, neighbors);
}

void Advection::refine(){

  for (unsigned c = 0; c < NUM_CHILDREN; ++c) {
    int cxmin, cxmax, cymin, cymax, czmin, czmax;
    getOctantRange(c, cxmin, cxmax, cymin, cymax, czmin, czmax);

    float cxx = xmin, cyy = ymin, czz = zmin;
    if (cxmax == block_width)
      cxx += (nx*dx)/2;
    if (cymax == block_height)
      cyy += (ny*dy)/2;
    if (czmax == block_depth)
      czz += (nz*dz)/2;

    refineChild(c, cxmin, cxmax, cymin, cymax, czmin, czmax, cxx, cyy, czz);
    //thisProxy.doneInserting();
  }
  mem_deallocate_all();
}

bool Advection::isGrandParent() {
  bool ret = false;
  for (int i = 0; i < NUM_CHILDREN; ++i) ret = ret || child_isRefined[i];
  return ret;
}

Advection::Advection(float dx, float dy, float dz,
                     float myt, float mydt, float xmin, float ymin, float zmin,
                     int meshGenIterations, int iterations, vector<float> refined_u, map<OctIndex, Neighbor> parentNeighbors)
{
  __sdag_init();

  this->dx = dx;
  this->dy = dy;
  this->dz = dz;

  this->myt = myt;
  this->mydt = mydt;

  this->xmin = xmin;
  this->ymin = ymin;
  this->zmin = zmin;

  nx = array_width/(num_chare_cols);
  ny = array_height/(num_chare_rows);
  nz = array_depth/(num_chare_Zs);

  thisIndex.getCoordinates(xc, yc, zc);
  this->meshGenIterations = meshGenIterations;
  this->iterations = iterations;

  initializeRestofTheData();
  nChildDataRecvd=0;
  map<OctIndex, Neighbor>::iterator it, iend;
  for (it = neighbors.begin(), iend = neighbors.end(); it != iend; ) {
    const OctIndex &neighborOctIndex = it->first;
    Neighbor &neighbor = it->second;
    bool shouldDelete = false;
    if (neighborOctIndex.getParent() != thisIndex.getParent()) {
      map<OctIndex, Neighbor>::iterator parentIt = parentNeighbors.find(neighborOctIndex.getParent());
      if (parentIt == parentNeighbors.end()) {
        shouldDelete = true;
      } else {
        Neighbor parentNeighbor = parentIt->second;
        if (parentNeighbor.isRefined()) {
          switch (parentNeighbor.getDecision(neighborOctIndex.getOctant())) {
            case COARSEN: shouldDelete = true; break;
            //case STAY: break;
            case REFINE: neighbor.setRefined(true); break;
          }
        } else {
          switch (parentNeighbor.getDecision()) {
            //case REFINE: break;
            case STAY: shouldDelete = true; break;
            case COARSEN: VB(logfile << "this neighbor cannot derefine" << std::endl;); 
                        CkAbort("this neighbor cannot derefine");
          }
        }
      }
    }

    // Advance iterator, removing this quadindex/neighbor pair if requested.
    if (shouldDelete)
      neighbors.erase(it++);
    else
      ++it;
  }

  assert(neighbors.size() >= numDims);
  unsigned sameParent = 0;
  for (map<OctIndex, Neighbor>::iterator it = neighbors.begin(),
       iend = neighbors.end(); it != iend; ++it) {
    if (it->first.getParent() == thisIndex.getParent())
      ++sameParent;
  }
  assert(sameParent == 3);

  if(!inInitialMeshGenerationPhase){
    int ctr=0;
    for(int k=1; k<=block_depth; k++)
      for(int j=1; j<=block_height; j++)
        for(int i=1; i<=block_width; i++)
          u[index(i,j,k)]=refined_u[ctr++];
  }
  iterate();
}

void Advection::startLdb(){
  UserSetLBLoad();
  /*if(isRoot()){ 
    ckout << "at sync" << endl;
  }*/
  //thisArray->remoteDoneInserting();
  //ckout << thisIndex.getIndexString().c_str() << " in startLdb " << isLeaf << " " << CkMyPe() << endl;
  AtSync();
  //ckout << "at sync" << endl;
  //if (isRoot())
  VB(logfile << "starting ldb" << std::endl;);
  VB(logfile << "memory usage: " << CmiMemoryUsage() << std::endl;);
  if(isRoot()){
    //LBDatabaseObj()->StartLB();
    ckout << "ldb start time: " << CmiWallTimer() << endl;
  }
}

void Advection::ResumeFromSync() {
  if(isRoot()) ckout << "ldb end time: " << CmiWallTimer() << endl;
  VB(logfile << "resuming from load balancing" << std::endl;);
  //ckout <<  thisIndex.getIndexString().c_str() << " " << isLeaf << endl;
  if(isLeaf)
    ppc.ckLocalBranch()->incrementWorkUnitCount(iterations);
  startPhase2(meshGenIterations);
}

bool Advection::isRoot(){
  return thisIndex.nbits == min_depth * numDims && thisIndex.bitVector == 0;
}

void Advection::printData() {
  int cntr=0;
  for(int xIndex = 1; xIndex <= block_width; xIndex++){
    for(int yIndex = 1; yIndex <= block_height; yIndex++) {
      for(int zIndex = 1; zIndex <= block_depth; zIndex++) {
        logfile << x[xIndex] << " " << y[yIndex] << " " << z[zIndex] << " " << u[index(xIndex,yIndex,zIndex)] << " " << cntr++ << std::endl;
      }
    }
  }

  //thisProxy[parent].donePrinting();
}

#include "Advection.def.h"
