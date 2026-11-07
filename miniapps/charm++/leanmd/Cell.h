#ifndef __CELL_H__
#define __CELL_H__

#include "ckmulticast.h"
#include <string>

//data message to be sent to computes
struct ParticleDataMsg : public CkMcastBaseMsg, public CMessage_ParticleDataMsg {
  vec3* part; //list of atoms
  int lengthAll;  //length of list
  int x, y, z;    // (x,y,z) coordinate of cell sending this message

  ParticleDataMsg(const int x_, const int y_, const int z_, const int numPos)
    : x(x_), y(y_), z(z_), lengthAll(numPos) { }

  void pup(PUP::er &p){
    CMessage_ParticleDataMsg::pup(p);
    p | lengthAll;
    p | x; p | y; p | z;
    if (p.isUnpacking()) part = new vec3[lengthAll];
    PUParray(p, part, lengthAll);
  } 
};

class CellMap : public CkArrayMap {
  int num_x, num_y, num_z, num_yz;
  float ratio;
  public:
  CellMap(int num_x_, int num_y_, int num_z_) : 
    num_x(num_x_), num_y(num_y_), num_z(num_z_) 
  {
    num_yz = num_y * num_z;
    ratio = ((float)CkNumPes())/(num_x * num_yz);
  }
  CellMap(CkMigrateMessage *m) {}
  int registerArray(CkArrayIndex& numElements, CkArrayID aid) {
    return 0;
  }
  int procNum(int arrayHdl, const CkArrayIndex &idx) {
    int *index =(int *)idx.data();
    int patchID = index[2] + index[1] * num_z + index[0] * num_yz;
    if (placeMode == PLACE_ONEPE) return 0;
    if (placeMode == PLACE_RANDOM) return (int)(leanmdHash(patchID) % CkNumPes());
    return (int)(patchID * ratio);
  }
};

//chare used to represent a cell
class Cell : public CBase_Cell {
private:
  Cell_SDAG_CODE;

  // list of atoms
  std::vector<Particle> particles;
  // my compute locations
  std::vector<CkArrayIndex6D> computesList;
  // to count the number of steps, and decide when to stop
  int stepCount;
  // number of atoms in my cell
  int myNumParts;
  // number of interacting neighbors
  int inbrs;
  double stepTime;
  int updateCount;
  // store kinetic energy - initial and final
  double energy[2];
  int numReadyCheckpoint;
  void migrateToCell(Particle p, int &px, int &py, int &pz);
  // updates properties after receiving forces from computes
  void updateProperties(vec3 *forces);
  // limit velcities to an upper limit
  void limitVelocity(Particle &p);
  // particles going out of right enters from left
  Particle& wrapAround(Particle &p);
  // handle to section proxy of computes
  CProxySection_Compute mCastSecProxy;
  // -hop: migrations still to make in this hop, and whether one is under way
  int hopsLeft = 0;
  bool hopping = false;
  void startHop(int hops);
  int pickDest();
  // per-step sum of the forces on my particles, to Main::forceCheck
  void checkForces(vec3 *forces, int n);

public:
  Cell();
  Cell(CkMigrateMessage *msg);
  ~Cell();
  void createComputes();  //add my computes
  void createSection();   //created multicast section of computes
  void migrateParticles();
  void sendPositions();
  void startCheckpoint(int);
  void pup(PUP::er &p);
  void hop(int dest);
  void ckJustMigrated();
  void report();
};

#endif
