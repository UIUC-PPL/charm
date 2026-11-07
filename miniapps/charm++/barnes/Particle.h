#ifndef __PARTICLE_H__
#define __PARTICLE_H__

#include "common.h"
#include "Vector3D.h"

struct Particle;
struct ExternalParticle {
  Vector3D<Real> position;
  Real mass;
  ExternalParticle &operator=(const Particle &p);
};

struct Particle : public ExternalParticle {
  Key key;
  Vector3D<Real> velocity;
  Vector3D<Real> acceleration;
  Real potential;

  Particle() : 
    potential(0.0),
    acceleration(0.0)
  {
  }

  bool operator>=(const Particle &other){
    return key >= other.key;
  }

  bool operator<=(const Particle &other){
    return key <= other.key;
  }
};


#endif
