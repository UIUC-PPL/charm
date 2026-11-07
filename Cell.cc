#include "defs.h"
#include "leanmd.decl.h"
#include "Cell.h"

Cell::Cell() : inbrs(NUM_NEIGHBORS), stepCount(1), updateCount(0), computesList(NUM_NEIGHBORS) {
  //load balancing to be called when AtSync is called
  usesAtSync = true;

  int myid = thisIndex.z+cellArrayDimZ*(thisIndex.y+thisIndex.x*cellArrayDimY); 
  myNumParts = PARTICLES_PER_CELL_START + (myid*(PARTICLES_PER_CELL_END-PARTICLES_PER_CELL_START))/(cellArrayDimX*cellArrayDimY*cellArrayDimZ);

  // starting random generator
  srand48(myid);

  // Particle initialization
  for(int i = 0; i < myNumParts; i++) {
    particles.push_back(Particle());
    particles[i].mass = HYDROGEN_MASS;

    //uniformly place particles, avoid close distance among them
    particles[i].pos.x = (GAP/(float)2) + thisIndex.x * CELL_SIZE_X + ((i*KAWAY_Y*KAWAY_Z)/(PERDIM*PERDIM))*GAP;
    particles[i].pos.y = (GAP/(float)2) + thisIndex.y * CELL_SIZE_Y + (((i*KAWAY_Z)/PERDIM)%(PERDIM/KAWAY_Y))*GAP;
    particles[i].pos.z = (GAP/(float)2) + thisIndex.z * CELL_SIZE_Z + (i%(PERDIM/KAWAY_Z))*GAP; 
    //give random values for velocity
    particles[i].vel.x = (drand48() - 0.5) * .2 * MAX_VELOCITY;
    particles[i].vel.y = (drand48() - 0.5) * .2 * MAX_VELOCITY;
    particles[i].vel.z = (drand48() - 0.5) * .2 * MAX_VELOCITY;
  }

  energy[0] = energy[1] = 0;
  setMigratable(cellsMigratable || (hopMode != HOP_NONE && hopMode != HOP_COMPUTES));
}

//constructor for chare object migration
Cell::Cell(CkMigrateMessage *msg): CBase_Cell(msg) {
  usesAtSync = true;
  setMigratable(cellsMigratable || (hopMode != HOP_NONE && hopMode != HOP_COMPUTES));
  delete msg;
}  

Cell::~Cell() {}

//function to create my computes
void Cell::createComputes() {
  int x = thisIndex.x, y = thisIndex.y, z = thisIndex.z;
  int px1, py1, pz1, dx, dy, dz, px2, py2, pz2;

  /*  The computes X are inserted by a given cell:
   *
   *	^  X  X  X
   *	|  0  X  X
   *	y  0  0  0
   *	   x ---->
   */

  // for round robin insertion
  int currPe = CkMyPe();

  for (int num = 0; num < inbrs; num++) {
    dx = num / (NBRS_Y * NBRS_Z)                - NBRS_X/2;
    dy = (num % (NBRS_Y * NBRS_Z)) / NBRS_Z     - NBRS_Y/2;
    dz = num % NBRS_Z                           - NBRS_Z/2;

    if (num >= inbrs / 2){
      px1 = x + KAWAY_X;
      py1 = y + KAWAY_Y;
      pz1 = z + KAWAY_Z;
      px2 = px1+dx;
      py2 = py1+dy;
      pz2 = pz1+dz;
      CkArrayIndex6D index(px1, py1, pz1, px2, py2, pz2);
      int P = CkNumPes();
      int pe = (++currPe) % P;
      unsigned long long h = leanmdHash(((unsigned long long)(x * 64 + y) * 64 + z) * 1024 + num);
      if (placeMode == PLACE_AWAY && P > 1) pe = (CkMyPe() + 1 + (int)(h % (P - 1))) % P;
      else if (placeMode == PLACE_ONEPE) pe = P - 1;
      else if (placeMode == PLACE_RANDOM) pe = (int)(h % P);
      computeArray[index].insert(pe);
      computesList[num] = index;
    } else {
      // these computes will be created by pairing cells
      px1 = WRAP_X(x + dx) + KAWAY_X;
      py1 = WRAP_Y(y + dy) + KAWAY_Y;
      pz1 = WRAP_Z(z + dz) + KAWAY_Z;
      px2 = px1 - dx;
      py2 = py1 - dy;
      pz2 = pz1 - dz;
      CkArrayIndex6D index(px1, py1, pz1, px2, py2, pz2);
      computesList[num] = index;
    }
  } // end of for loop
  int n = particles.size();
  contribute(sizeof(int), &n, CkReduction::sum_int, CkCallback(CkReductionTarget(Main,run),mainProxy));
}

//call multicast section creation
void Cell::createSection() {
  //knit the computes into a section
  mCastSecProxy = CProxySection_Compute::ckNew(computeArray.ckGetArrayID(), &computesList[0], computesList.size(), bFactor);
  mCastSecProxy.setReductionClient(new CkCallback(CkReductionTarget(Cell,reduceForces), thisProxy(thisIndex.x, thisIndex.y, thisIndex.z)));
}

// Function to start interaction among particles in neighboring cells as well as its own particles
void Cell::sendPositions() {
  unsigned int len = particles.size();
  //create the particle and control message to be sent to computes
  ParticleDataMsg* msg = new (len) ParticleDataMsg(thisIndex.x, thisIndex.y, thisIndex.z, len);

  for(int i = 0; i < len; ++i)
    msg->part[i] = particles[i].pos;

  mCastSecProxy.calculateForces(msg);
}

//send the atoms that have moved beyond my cell to neighbors
void Cell::migrateParticles(){
  int x1, y1, z1;
  std::vector<std::vector<Particle> > outgoing;
  outgoing.resize(inbrs);

  int size = particles.size();
  for(std::vector<Particle>::reverse_iterator iter = particles.rbegin(); iter != particles.rend(); iter++) {
    migrateToCell(*iter, x1, y1, z1);
    if(x1!=0 || y1!=0 || z1!=0) {
      outgoing[(x1+KAWAY_X)*NBRS_Y*NBRS_Z + (y1+KAWAY_Y)*NBRS_Z + (z1+KAWAY_Z)].push_back(wrapAround(*iter));
      std::swap(*iter, particles[size - 1]);
      size--;
    }
  }
  particles.resize(size);

  for(int num = 0; num < inbrs; num++) {
    x1 = num / (NBRS_Y * NBRS_Z)            - NBRS_X/2;
    y1 = (num % (NBRS_Y * NBRS_Z)) / NBRS_Z - NBRS_Y/2;
    z1 = num % NBRS_Z                       - NBRS_Z/2;
    cellArray(WRAP_X(thisIndex.x+x1), WRAP_Y(thisIndex.y+y1), WRAP_Z(thisIndex.z+z1)).receiveParticles(outgoing[num]);
  }
}

//check if the particle is to be moved
void Cell::migrateToCell(Particle p, int &px, int &py, int &pz) {
  double x = thisIndex.x * CELL_SIZE_X + CELL_ORIGIN_X;
  double y = thisIndex.y * CELL_SIZE_Y + CELL_ORIGIN_Y;
  double z = thisIndex.z * CELL_SIZE_Z + CELL_ORIGIN_Z;
  px = py = pz = 0;

  if (p.pos.x < (x-CELL_SIZE_X)) px = -2;
  else if (p.pos.x < x) px = -1;
  else if (p.pos.x > (x+2*CELL_SIZE_X)) px = 2;
  else if (p.pos.x > (x+CELL_SIZE_X)) px = 1;

  if (p.pos.y < (y-CELL_SIZE_Y)) py = -2;
  else if (p.pos.y < y) py = -1;
  else if (p.pos.y > (y+2*CELL_SIZE_Y)) py = 2;
  else if (p.pos.y > (y+CELL_SIZE_Y)) py = 1;

  if (p.pos.z < (z-CELL_SIZE_Z)) pz = -2;
  else if (p.pos.z < z) pz = -1;
  else if (p.pos.z > (z+2*CELL_SIZE_Z)) pz = 2;
  else if (p.pos.z > (z+CELL_SIZE_Z)) pz = 1;
}

// Function to update properties (i.e. acceleration, velocity and position) in particles
void Cell::updateProperties(vec3 *forces) {
  int i;
  double powTen, powTwenty, realTimeDeltaVel, invMassParticle;
  powTen = pow(10.0, 10);
  powTwenty = pow(10.0, -20);
  realTimeDeltaVel = DEFAULT_DELTA * powTwenty;
  for(i = 0; i < particles.size(); i++) {
    //calculate energy only in begining and end
    if(stepCount == 1) {
      energy[0] += (0.5 * particles[i].mass * dot(particles[i].vel, particles[i].vel) * powTen); // in milliJoules
    } else if(stepCount == finalStepCount) { 
      energy[1] += (0.5 * particles[i].mass * dot(particles[i].vel, particles[i].vel) * powTen);
    }
    // applying kinetic equations
    invMassParticle = 1 / particles[i].mass;
    particles[i].acc = forces[i] * invMassParticle; // in m/sec^2
    particles[i].vel += particles[i].acc * realTimeDeltaVel; // in A/fs

    limitVelocity(particles[i]);

    particles[i].pos += particles[i].vel * DEFAULT_DELTA; // in A
  }
}

inline double velocityCheck(double inVelocity) {
  if(fabs(inVelocity) > MAX_VELOCITY) {
    if(inVelocity < 0.0 )
      return -MAX_VELOCITY;
    else
      return MAX_VELOCITY;
  } else {
    return inVelocity;
  }
}

void Cell::limitVelocity(Particle &p) {
  p.vel.x = velocityCheck(p.vel.x);
  p.vel.y = velocityCheck(p.vel.y);
  p.vel.z = velocityCheck(p.vel.z);
}

Particle& Cell::wrapAround(Particle &p) {
  if(p.pos.x < CELL_ORIGIN_X) p.pos.x += CELL_SIZE_X*cellArrayDimX;
  if(p.pos.y < CELL_ORIGIN_Y) p.pos.y += CELL_SIZE_Y*cellArrayDimY;
  if(p.pos.z < CELL_ORIGIN_Z) p.pos.z += CELL_SIZE_Z*cellArrayDimZ;
  if(p.pos.x > CELL_ORIGIN_X + CELL_SIZE_X*cellArrayDimX) p.pos.x -= CELL_SIZE_X*cellArrayDimX;
  if(p.pos.y > CELL_ORIGIN_Y + CELL_SIZE_Y*cellArrayDimY) p.pos.y -= CELL_SIZE_Y*cellArrayDimY;
  if(p.pos.z > CELL_ORIGIN_Z + CELL_SIZE_Z*cellArrayDimZ) p.pos.z -= CELL_SIZE_Z*cellArrayDimZ;

  return p;
}

//pack important data when I move/checkpoint
void Cell::pup(PUP::er &p) {
  CBase_Cell::pup(p);
  __sdag_pup(p);
  p | particles;
  p | stepCount;
  p | myNumParts;
  p | updateCount;
  p | stepTime;
  p | inbrs;
  p | numReadyCheckpoint;
  PUParray(p, energy, 2);
  p | hopsLeft;
  p | hopping;

  p | computesList;

  p | mCastSecProxy;
  //adjust the multicast tree to give best performance after moving
  if (p.isUnpacking()){
    if(CkInRestarting()){
      createSection();
    }
    else{
      mCastSecProxy.resetSection();
      mCastSecProxy.setReductionClient(new CkCallback(CkReductionTarget(Cell,reduceForces), thisProxy(thisIndex.x, thisIndex.y, thisIndex.z)));
    }
  }
}

void Cell::checkForces(vec3 *forces, int n) {
  double v[4] = {0, 0, 0, 0};
  for (int i = 0; i < n; i++) {
    v[0] += forces[i].x; v[1] += forces[i].y; v[2] += forces[i].z;
    v[3] += fabs(forces[i].x) + fabs(forces[i].y) + fabs(forces[i].z);
  }
  contribute(sizeof(v), v, CkReduction::sum_double, CkCallback(CkReductionTarget(Main, forceCheck), mainProxy));
}

int Cell::pickDest() {
  int P = CkNumPes();
  int myid = thisIndex.z + cellArrayDimZ * (thisIndex.y + thisIndex.x * cellArrayDimY);
  unsigned long long h = leanmdHash(((unsigned long long)stepCount << 32) ^ (myid * 8 + hopsLeft));
  return (CkMyPe() + 1 + (int)(h % (P - 1))) % P;
}

// The SDAG in run() waits for arrived() while hopping is set.
void Cell::startHop(int hops) {
  if (CkNumPes() < 2) return;
  hopping = true;
  hopsLeft = hops;
  thisProxy(thisIndex.x, thisIndex.y, thisIndex.z).hop(pickDest());
}

void Cell::hop(int dest) {
  hopsLeft--;
  migrateMe(dest);  // last action
}

void Cell::ckJustMigrated() {
  CBase_Cell::ckJustMigrated();
  if (!hopping) return;
  if (hopsLeft > 0) thisProxy(thisIndex.x, thisIndex.y, thisIndex.z).hop(pickDest());
  else { hopping = false; thisProxy(thisIndex.x, thisIndex.y, thisIndex.z).arrived(); }
}

void Cell::report() {
  CkPrintf("leanmd: cell (%d,%d,%d) on PE %d: step %d, hopping %d, hops left %d\n",
           thisIndex.x, thisIndex.y, thisIndex.z, CkMyPe(), stepCount, (int)hopping, hopsLeft);
}
