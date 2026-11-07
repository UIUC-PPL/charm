#include <iostream>
#include <fstream>
#include <stdlib.h>
#include <math.h>
using namespace std;

#include "stdint.h"

#define PREAMBLE_INTS 2 
#define PREAMBLE_REALS 1
#define PREAMBLE_SIZE ((PREAMBLE_INTS)*sizeof(int)+(PREAMBLE_REALS)*sizeof(real))
#define REALS_PER_PARTICLE 8
#define SIZE_PER_PARTICLE ((REALS_PER_PARTICLE)*sizeof(real))
typedef float real;
typedef float ptype;
typedef uint64_t Key;

void generateSequential(ofstream &file, int np){
  real start = 0.0;
  real incr = 0.001;
  int ndims = 3;
  real tnow = 0.0;
  real vals[REALS_PER_PARTICLE];
  for(int j = 0; j < REALS_PER_PARTICLE; j++){
    vals[j] = start;
  }
  // write number of particles first.
  file.write((char *)&np, sizeof(int));
  // ndims
  file.write((char *)&ndims,sizeof(int));
  // tnow
  file.write((char *)&tnow,sizeof(real));

  streamsize size = SIZE_PER_PARTICLE;
  for(int i = 0; i < np; i++){
    file.write((char *)vals,size);
    for(int j = 0; j < REALS_PER_PARTICLE; j++){
      vals[j] += incr;
    }
  }
}

void generateRandom(ofstream &file, int np){
  real vals[REALS_PER_PARTICLE];
  real mass = 0.001;
  real soft = 0.001;
  int max = 1000000;
  int ndims = 3;
  real tnow = 0.0;

  real minVel = 0.0003;
  real maxVel = 0.0006;

  streamsize size = SIZE_PER_PARTICLE;
  // write number of particles first.
  file.write((char *)&np, sizeof(int));
  // ndims
  file.write((char *)&ndims, sizeof(int));
  // tnow
  file.write((char *)&tnow, sizeof(real));

  for(int i = 0; i < np; i++){
    // position
    for(int j = 0; j < 3; j++){
      vals[j] = ((1.0*(rand()%max))/max)-0.5;
    }
    // velocity
    for(int j = 3; j < 6; j++){
      vals[j] = (maxVel-minVel)*((1.0*(rand()%max))/max)+minVel;
    }
    vals[6] = mass;
    vals[7] = soft;
    printf("%f %f %f %f\n", vals[0], vals[1], vals[2], sqrt(vals[3]*vals[3]+vals[4]*vals[4]+vals[5]*vals[5]));
    file.write((char *)vals,size);
  }
}

int main(int argc, char **argv){
  if(argc != 3){
    cerr << "usage: gen <num_particles> <file_name>" << endl;
    exit(1);
  }

  int numParticles = atoi(argv[1]);
  char *fileName = argv[2];

  ofstream file;
  file.open(fileName, ios::out|ios::binary);
  generateRandom(file,numParticles);
  file.close();
  return 0;
}

