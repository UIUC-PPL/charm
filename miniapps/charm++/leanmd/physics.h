#ifndef __PHYSICS_H__
#define __PHYSICS_H__

#include "defs.h"

#define BLOCK_SIZE	512

//function to calculate forces among 2 lists of atoms
inline double calcPairForces(ParticleDataMsg* first, ParticleDataMsg* second, int stepCount,
                             std::vector<vec3>& force1, std::vector<vec3> &force2) {
  int i, j, ptpCutOffSqd, diff;
  int firstLen = first->lengthAll;
  int secondLen = second->lengthAll;
  double powTwenty, powTen, r, rsqd, f, fr;
  vec3 separation, force;
  double rSix, rTwelve;
  double energy = 0;
  int doEnergy = 0;
  if(stepCount == 1 || stepCount == finalStepCount)
    doEnergy = 1;

  force1.resize(firstLen);
  force2.resize(secondLen);
  //check for wrap around and adjust locations accordingly
  if (abs(first->x - second->x) > KAWAY_X){
    diff = CELL_SIZE_X * cellArrayDimX;
    if (second->x < first->x)
      diff = -1 * diff; 
    for (i = 0; i < firstLen; i++)
      first->part[i].x += diff;
  }
  if (abs(first->y - second->y) > KAWAY_Y){
    diff = CELL_SIZE_Y * cellArrayDimY;
    if (second->y < first->y)
      diff = -1 * diff; 
    for (i = 0; i < firstLen; i++)
      first->part[i].y += diff;
  }
  if (abs(first->z - second->z) > KAWAY_Z){
    diff = CELL_SIZE_Z * cellArrayDimZ;
    if (second->z < first->z)
      diff = -1 * diff; 
    for (i = 0; i < firstLen; i++)
      first->part[i].z += diff;
  } 
  ptpCutOffSqd = PTP_CUT_OFF * PTP_CUT_OFF;
  powTen = pow(10.0, -10);
  powTwenty = pow(10.0, -20);

  int i1, j1;
  for(i1 = 0; i1 < firstLen; i1=i1+BLOCK_SIZE)
    for(j1 = 0; j1 < secondLen; j1=j1+BLOCK_SIZE)
      for(i = i1; i < i1+BLOCK_SIZE && i < firstLen; i++) {
        for(j = j1; j < j1+BLOCK_SIZE && j < secondLen; j++) {
          separation = first->part[i] - second->part[j];
          rsqd = dot(separation, separation);
          if (rsqd > 1 && rsqd < ptpCutOffSqd) {
            rsqd = rsqd * powTwenty;
            r = sqrt(rsqd);
            rSix = ((double)rsqd) * rsqd * rsqd;
            rTwelve = rSix * rSix;
            f = (double)( (12 * VDW_A) / rTwelve - (6 * VDW_B) / rSix);
            if(doEnergy)
              energy += (double)( VDW_A / rTwelve - VDW_B / rSix); // in milliJoules
            fr = f / rsqd;
            force = separation * (fr * powTen);
            force1[i] += force;
            force2[j] -= force;
          }
        }
      }

  return energy;
}

//function to calculate forces among atoms in a single list
inline double calcInternalForces(ParticleDataMsg* first, int stepCount, std::vector<vec3>& force1) {
  int i, j, ptpCutOffSqd;
  int firstLen = first->lengthAll;
  double powTwenty, powTen, firstx, firsty, firstz, rx, ry, rz, r, rsqd, fx, fy, fz, f, fr;
  vec3 firstpos, separation, force;
  double rSix, rTwelve;
  double energy = 0;
  int doEnergy = 0;
  if(stepCount == 1 || stepCount == finalStepCount)
    doEnergy = 1;
  force1.resize(firstLen);

  ptpCutOffSqd = PTP_CUT_OFF * PTP_CUT_OFF;
  powTen = pow(10.0, -10);
  powTwenty = pow(10.0, -20);
  for(i = 0; i < firstLen; i++){
    firstpos = first->part[i];
    for(j = i+1; j < firstLen; j++) {
      // computing base values
      separation = firstpos - first->part[j];
      rsqd = dot(separation, separation);
      if(rsqd > 1 && rsqd < ptpCutOffSqd){
        rsqd = rsqd * powTwenty;
        r = sqrt(rsqd);
        rSix = ((double)rsqd) * rsqd * rsqd;
        rTwelve = rSix * rSix;
        f = (double)( (12 * VDW_A) / rTwelve - (6 * VDW_B) / rSix);
        if(doEnergy)
          energy += (double)( VDW_A / rTwelve - VDW_B / rSix);

        fr = f / rsqd;
        force = separation * (fr * powTen);
        force1[i] += force;
        force1[j] -= force;
      }
    }
  }
  return energy;
}
#endif
