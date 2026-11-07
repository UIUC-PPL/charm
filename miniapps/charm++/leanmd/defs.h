
#ifndef __DEFS__
#define __DEFS__

#include "pup.h"

#define HYDROGEN_MASS           (1.67 * pow( 10.0,-24)) // in g
#define VDW_A                   (1.1328 * pow(10.0, -133)) // in (g m^2/s^2) m^12
#define VDW_B                   (2.23224 * pow(10.0, -76)) // in (g m^2/s^2) m^6

#define ENERGY_VAR              (1.0 * pow(10.0,-5))

//average of next two should be what you want as your atom density
//this should comply with the PERDIM parameter; for KAWAY 1 1 1, the maximum number
//of particles can be 10*10*10 = 1000 - 10 comes from PERDIM parameter, which is
//currently set to be 10, using a GAP of 3; as KWAYness increases, the maximum
//number of particles decreases - for 2 1 1, it is 500, for 2 2 1 it is 250; you
//can set them to have lower values but not higher; alternatively a host of
//paramters including PTP_CUT_OFF, PERDIM, GAP can be set to suitable values to
#ifndef PARTICLES_PER_CELL_START
#define PARTICLES_PER_CELL_START        100 
#endif
#ifndef PARTICLES_PER_CELL_END
#define PARTICLES_PER_CELL_END          250
#endif

#define DEFAULT_DELTA           1	// in femtoseconds

#define DEFAULT_FIRST_LDB       20
#define DEFAULT_LDB_PERIOD      20
#define DEFAULT_FT_PERIOD       100000

#ifndef KAWAY_X
#define KAWAY_X                 2
#endif
#ifndef KAWAY_Y
#define KAWAY_Y                 2
#endif
#ifndef KAWAY_Z
#define KAWAY_Z                 1
#endif
#define NBRS_X	                (2*KAWAY_X+1)
#define NBRS_Y                  (2*KAWAY_Y+1)
#define NBRS_Z                  (2*KAWAY_Z+1)
#define NUM_NEIGHBORS           (NBRS_X * NBRS_Y * NBRS_Z)

#define CELLARRAY_DIM_X         3
#define CELLARRAY_DIM_Y         3
#define CELLARRAY_DIM_Z         3
#define PTP_CUT_OFF             26 // cut off for atom to atom interactions
#define CELL_MARGIN             4  // constant diff between cutoff and cell size
#define CELL_SIZE_X             (PTP_CUT_OFF + CELL_MARGIN)/KAWAY_X
#define CELL_SIZE_Y             (PTP_CUT_OFF + CELL_MARGIN)/KAWAY_Y
#define CELL_SIZE_Z             (PTP_CUT_OFF + CELL_MARGIN)/KAWAY_Z

//variables to control initial uniform placement of atoms;
//atoms should not be too close at startup for a stable system;  
//PERDIM * GAP should be less than (PTPCUTOFF+CELL_MARGIN);
//max particles per cell should not be greater thatn PERDIM^3 for 1 AWAY;
#define PERDIM                  10
#define GAP                     3 

#define CELL_ORIGIN_X           0
#define CELL_ORIGIN_Y	        0
#define CELL_ORIGIN_Z	        0

#define MIGRATE_STEPCOUNT	        20
#define DEFAULT_FINALSTEPCOUNT	        1001
#define MAX_VELOCITY		        .1  //in A/fs

#define WRAP_X(a)		(((a) + cellArrayDimX) % cellArrayDimX)
#define WRAP_Y(a)		(((a) + cellArrayDimY) % cellArrayDimY)
#define WRAP_Z(a)		(((a) + cellArrayDimZ) % cellArrayDimZ)

struct vec3 {
  double x, y, z;

  vec3(double d = 0.0) : x(d), y(d), z(d) { }
  vec3(double x_, double y_, double z_) : x(x_), y(y_), z(z_) { }

  inline vec3& operator += (const vec3 &rhs) {
    x += rhs.x; y += rhs.y; z += rhs.z;
    return *this;
  }
  inline vec3& operator -= (const vec3 &rhs) {
    return *this += (rhs * -1.0);
  }
  inline vec3 operator* (const double d) const {
    return vec3(d*x, d*y, d*z);
  }
  inline vec3 operator- (const vec3& rhs) const {
    return vec3(x - rhs.x, y - rhs.y, z - rhs.z);
  }
};
inline double dot(const vec3& a, const vec3& b) {
  return a.x*b.x + a.y*b.y + a.z*b.z;
}
PUPbytes(vec3)

//class for keeping track of the properties for a particle
struct Particle {
  double mass;
  //   Position, acceleration, velocity
  vec3 pos,acc,vel;
};
PUPbytes(Particle);

#include "leanmd.decl.h"

extern /* readonly */ CProxy_Main mainProxy;
extern /* readonly */ CProxy_Cell cellArray;
extern /* readonly */ CProxy_Compute computeArray;

extern /* readonly */ int cellArrayDimX;
extern /* readonly */ int cellArrayDimY;
extern /* readonly */ int cellArrayDimZ;
extern /* readonly */ int finalStepCount;
extern /* readonly */ int checkptStrategy;
extern /* readonly */ std::string logs;
extern /* readonly */ int bFactor;
extern /* readonly */ int placeMode;
extern /* readonly */ int hopMode;
extern /* readonly */ int hopEvery;
extern /* readonly */ int cellsMigratable;
// Stress-test options (see README): -place and -hop modes.
#define PLACE_DEFAULT   0
#define PLACE_AWAY      1   // computes never on the PE of the cell that inserts them
#define PLACE_ONEPE     2   // all cells on PE 0, all computes on the last PE
#define PLACE_RANDOM    3   // cells and computes on hashed PEs
#define HOP_NONE        0
#define HOP_BETWEEN     1   // cells migrate after their force reduction completes
#define HOP_INFLIGHT    2   // cells migrate right after their multicast
#define HOP_TWICE       3   // like HOP_BETWEEN, two migrations in a row
#define HOP_COMPUTES    4   // a quarter of the computes migrate after contributing

inline unsigned long long leanmdHash(unsigned long long z) {
  z += 0x9E3779B97F4A7C15ULL;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

#endif
