#ifndef __GRAVITY_H__
#define __GRAVITY_H__

#include "Node.h"
#include "Vector3D.h"
#include "Space.h"
#include "defines.h"
#include "Parameters.h"

extern Parameters globalParams;

inline bool
openCriterionBucket(Node<ForceData> *node,
                   Node<ForceData> *bucketNode) {

  Vector3D<Real> dr = node->data.moments.cm - bucketNode->data.moments.cm;
  Real drsq = dr.lengthSquared();
  return (globalParams.tolsq*drsq < node->data.moments.rsq);
}

inline 
void grav(Particle *pstart, Particle *pend, Real mass, const Vector3D<Real> &position){
  Vector3D<Real> dr;
  Real drsq;
  Real drabs;
  Real phii;
  Real mor3;

  for(Particle *p = pstart; p != pend; p++){
    dr = position - p->position;
    drsq = dr.lengthSquared();
    drsq += globalParams.epssq;
    drabs = sqrt((double) drsq);
    phii = mass/drabs;
    p->potential -= phii;
    mor3 = phii/drsq;
    p->acceleration += mor3*dr;
  }
}

inline
int nodeBucketForce(Node<ForceData> *node, 
		    Node<ForceData> *req){
  
  Particle *particles = req->getParticles();
  int numParticles = req->getNumParticles();
  grav(particles,particles+numParticles,node->data.moments.totalMass,node->data.moments.cm);
  return req->getNumParticles();
}

inline int partBucketForce(ExternalParticle *part, 
			   Node<ForceData> *req){ 

  Particle *particles = req->getParticles();
  int numParticles = req->getNumParticles();
  grav(particles,particles+numParticles,part->mass,part->position);
  return numParticles;
}

#endif
