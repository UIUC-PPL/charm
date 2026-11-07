#include "defs.h"
#include "Cell.h"
#include "Compute.h"
#include "physics.h"
#include <algorithm>
using std::swap;

//compute - Default constructor
Compute::Compute() : stepCount(1) {
  energy[0] = energy[1] = 0;
  usesAtSync = true;
}

Compute::Compute(CkMigrateMessage *msg): CBase_Compute(msg)  { 
  usesAtSync = true;
  delete msg;
}

//interaction within a cell
void Compute::selfInteract(ParticleDataMsg *msg){
  double energyP = 0;
  std::vector<vec3> force1;

  energyP = calcInternalForces(msg, stepCount, force1);

  //energy assignment only in begining and end
  if (stepCount == 1) energy[0] = energyP;
  else if (stepCount == finalStepCount) energy[1] = energyP;

  //contribute to force reduction
  CkGetSectionInfo(mcast1, msg);
  CProxySection_Compute::contribute(sizeof(vec3)*msg->lengthAll, &force1[0], CkReduction::sum_double, mcast1);

  delete msg;
}

//interaction between two cells
void Compute::interact(ParticleDataMsg *msg1, ParticleDataMsg *msg2){
  CkSectionInfo *handleA = &mcast1, *handleB = &mcast2;
  if (msg2->x * cellArrayDimY * cellArrayDimZ + msg2->y * cellArrayDimZ + msg2->z <
      msg1->x * cellArrayDimY * cellArrayDimZ + msg1->y * cellArrayDimZ + msg1->z)
    swap(handleA, handleB);

  std::vector<vec3> force1, force2;
  double energyP = calcPairForces(msg1, msg2, stepCount, force1, force2);

  //energy assignment only in begining and end
  if (stepCount == 1) energy[0] = energyP;
  else if (stepCount == finalStepCount) energy[1] = energyP;

  //contribute to force reduction
  CkGetSectionInfo(*handleA, msg1);
  CProxySection_Compute::contribute(sizeof(vec3)*msg1->lengthAll, &force1[0], CkReduction::sum_double, *handleA);
  CkGetSectionInfo(*handleB, msg2);
  CProxySection_Compute::contribute(sizeof(vec3)*msg2->lengthAll, &force2[0], CkReduction::sum_double, *handleB);

  delete msg1;
  delete msg2;
}

//pack important information if I am moving
void Compute::pup(PUP::er &p) {
  CBase_Compute::pup(p);
  __sdag_pup(p);
  p | stepCount;
  p | mcast1;
  p | mcast2;
  PUParray(p, energy, 2);
  p | hopping;
  if (p.isUnpacking() && CkInRestarting()) {
    mcast1.set_redNo(0); 
    mcast2.set_redNo(0);
  }
}
static unsigned long long computeKey(const CkIndex6D &ix) {
  unsigned long long k = 0;
  const short *c = &ix.x1;
  for (int i = 0; i < 6; i++) k = k * 131 + (unsigned short)c[i];
  return k;
}

// A quarter of the computes, a different quarter each step.
bool Compute::hopsThisStep() {
  return CkNumPes() > 1 && leanmdHash(computeKey(thisIndex) ^ ((unsigned long long)stepCount << 40)) % 4 == 0;
}

// The SDAG in run() waits for arrived() while hopping is set.
void Compute::startHop() {
  hopping = true;
  int P = CkNumPes();
  int dest = (CkMyPe() + 1 + (int)(leanmdHash(computeKey(thisIndex) + stepCount) % (P - 1))) % P;
  thisProxy[thisIndex].hop(dest);
}

void Compute::hop(int dest) {
  migrateMe(dest);  // last action
}

void Compute::ckJustMigrated() {
  CBase_Compute::ckJustMigrated();
  if (hopping) { hopping = false; thisProxy[thisIndex].arrived(); }
}

