/* adapted from barnes benchmark in SPLASH2 */

/*
 * TESTDATA: generate Plummer model initial conditions for test runs,
 * scaled to units such that M = -4E = G = 1 (Henon, Hegge, etc).
 * See Aarseth, SJ, Henon, M, & Wielen, R (1974) Astr & Ap, 37, 183.
 */

#include "Vector3D.h"
#include "Particle.h"
#include <assert.h>
#include <stdlib.h>
#include <iostream>
#include <fstream>

using namespace std;

#define MFRAC  0.999                /* mass cut off at MFRAC of total */

Real xrand(Real,Real);
void pickshell(Vector3D<Real> &vec, Real rad);

Particle *testdata(int nbody)
{
   Real rsc, vsc, rsq, r, v, x, y;
   Vector3D<Real> cmr, cmv;
   Particle *p;
   int rejects = 0;
   int k;
   int halfnbody, i;
   Real offset;
   Particle *cp;
   Real tmp;

   Particle *bodytab = new Particle[nbody * sizeof(Particle)];
   assert(bodytab != NULL);

   rsc = 9 * PI / 16;
   vsc = sqrt(1.0 / rsc);

   cmr = Vector3D<Real>(0.0);
   cmv = Vector3D<Real>(0.0);

   halfnbody = nbody / 2;
   if (nbody % 2 != 0) halfnbody++;
   for (p = bodytab; p < bodytab+halfnbody; p++) {
      p->mass = 1.0/nbody;
      r = 1 / sqrt(std::pow((double)xrand(0.0, MFRAC), (double)-2.0/3.0) - 1);
      /*   reject radii greater than 10 */
      while (r > 9.0) {
	 rejects++;
	 r = 1 / sqrt(std::pow((double)xrand(0.0, MFRAC), (double)-2.0/3.0) - 1);

      }        
      pickshell(p->position, rsc * r);
      
      cmr += p->position;
      do {
	 x = xrand(0.0, 1.0);
	 y = xrand(0.0, 0.1);

      } while (y > x*x * std::pow((double)(1 - x*x), (double)3.5));

      v = sqrt(2.0) * x / std::pow((double)(1 + r*r), (double)0.25);
      pickshell(p->velocity, vsc * v);
      cmv += p->velocity;
      
   }

   offset = 4.0;

   for (p = bodytab + halfnbody; p < bodytab+nbody; p++) {
      p->mass = 1.0 / nbody;
      cp = p - halfnbody;
      p->position = cp->position+offset;
      p->velocity = cp->velocity;
      cmr += p->position;
      cmv += p->position;
   }

   cmr /= nbody;
   cmv /= nbody;

   for (p = bodytab; p < bodytab+nbody; p++) {
     p->position -= cmr;
     p->velocity -= cmv;
   }

   return bodytab;
}

/*
 * PICKSHELL: pick a random point on a sphere of specified radius.
 */

void pickshell(Vector3D<Real> &vec, Real rad)
{
   register int k;
   Real rsq, rsc;

   do {
     vec.x = xrand(-1.0,1.0);
     vec.y = xrand(-1.0,1.0);
     vec.z = xrand(-1.0,1.0);
     rsq = vec.lengthSquared();
   } while (rsq > 1.0);

   rsc = rad / sqrt(rsq);
   vec = rsc*vec;
}


int intpow(int i, int j)
{   
    int k;
    int temp = 1;

    for (k = 0; k < j; k++)
        temp = temp*i;
    return temp;
}

void pranset(int);

int main(int argc, char **argv){
  pranset(128363);
  assert(argc == 3);

  int nbody = atoi(argv[1]);
  int ndims = 3;
  Real tnow = 0.0;
  ofstream out(argv[2], ios::out|ios::binary);
  
  Particle *p = testdata(nbody);
  
  out.write((char *)&nbody, sizeof(int));
  out.write((char *)&ndims, sizeof(int));
  out.write((char *)&tnow, sizeof(Real));

  Real tmp[REALS_PER_PARTICLE];
  Real soft = 0.001;
  
  for(int i = 0; i < nbody; i++){
    tmp[0] = p->position.x;
    tmp[1] = p->position.y;
    tmp[2] = p->position.z;
    tmp[3] = p->velocity.x;
    tmp[4] = p->velocity.y;
    tmp[5] = p->velocity.z;
    tmp[6] = p->mass;
    tmp[7] = soft;

    cout << p->position.x << " "
         << p->position.y << " "
         << p->position.z << endl;

    out.write((char*)tmp, REALS_PER_PARTICLE*sizeof(Real));

    p++;
  }

  out.close();

  return 0;
}


