#ifndef KRIPKE_SUBDOMAIN_H__
#define KRIPKE_SUBDOMAIN_H__

#include <vector>
#include "SubTVec.h"
#include "pup_stl.h"

// Forward Decl
struct Directions;
struct SubTVec;
struct Input_Variables;
class Kernel;

/**
  Describes a neighboring Subdomain using both mpi-rank and subdomain id
*/
struct Neighbor{
public:
  int x, y, z; // Neighbor X, Y, Z zone set coordinates, or x=-1 for boundary condition

  // Pack-UnPack method
  inline void pup(PUP::er &p) {
    p|x; p|y; p|z;
  }
};


/**
 * Provides sweep index sets for a given octant.
 * This generalizes the sweep pattern, and allows for experimenting with
 * a tiled approach to on-node sweeps.
 */
struct Grid_Sweep_Block {
public:
  int start_i, start_j, start_k; // starting index
  int end_i, end_j, end_k; // termination conditon (one past)
  int inc_i, inc_j, inc_k; // increment

  // Pack-UnPack method
  inline void pup(PUP::er &p) {
    p|start_i; p|start_j; p|start_k;
    p|end_i;   p|end_j;   p|end_k;
    p|inc_i;   p|inc_j;   p|inc_k;
  }
};


/**
 * Contains parameters and variables that describe a single Group Set and
 * Direction Set.
 */
struct Subdomain {
  Subdomain();
  ~Subdomain();

  void setup(int zsIdx[3], int sdom_id, Input_Variables *input_vars, 
          int gs, int ds, int zs, std::vector<Directions> &direction_list, Kernel *kernel);

  Neighbor getNeighbor(bool upwind, int zsIdx[3], int zsDims[3], int sdom_id, int dim, int dir);
  
  std::pair<double, double> getSpatialExtents(int dim, int zsIdx[3], int zsDims[3]) const;
  
  int getNumZones(int dim, int zsIdx, Input_Variables *input_vars);
  
  void setVars(SubTVec *ell_ptr, SubTVec *ell_plus_ptr,
    SubTVec *phi_ptr, SubTVec *phi_out_ptr);

  void randomizeData(void);
  void copy(Subdomain const &b);
  bool compare(Subdomain const &b, double tol, bool verbose);
  void computeSweepIndexSet(void);
  void computeLLPlus(int legendre_order);

  int idx_group_set;
  int idx_dir_set;
  int idx_zone_set;

  int num_groups;       // Number of groups in this set
  int num_directions;   // Number of directions in this set
  int num_zones;        // Number of zones in this set

  double zeros[3];                // origin of local mesh
  int nzones[3];                  // Number of zones in each dimension
  std::vector<double> deltas[3];  // Spatial grid deltas in each dimension (including ghost zones)

  int group0;           // Starting global group id
  int direction0;       // Starting global direction id

  Grid_Sweep_Block sweep_block;

  // Neighbors
  Neighbor upwind[3];   // Upwind dependencies in x,y,z
  Neighbor downwind[3]; // Downwind neighbors in x,y,z
  int deps;             // Number of upwind dependencies

  // Sweep boundary data
  SubTVec *plane_data[3];

  // Variables
  SubTVec *psi;         // Solution
  SubTVec *rhs;         // RHS, source term
  SubTVec *sigt;        // Zonal per-group cross-section

  // Pointers into directions and directionset data from Grid_Data
  Directions *directions;
  SubTVec *ell;
  SubTVec *ell_plus;
  SubTVec *phi;
  SubTVec *phi_out;

  // Materials on the mesh, used for scattering lookup
  double reg_volume[3];               // volume of each material region
  std::vector<double> volume;         // volume of each zone
  std::vector<int> mixed_to_zones;    // mapping from mixed slot to zones
  std::vector<int> num_mixed;         // mapping from mixed slot to zones
  std::vector<int> zones_to_mixed;    // mapping from zones to first mixed slot
  std::vector<int> mixed_material;    // material number for each mixed slot
  std::vector<double> mixed_fraction; // volume fraction each mixed slot

  // Pack-Unpack method
  void pup(PUP::er &p) {
    p|idx_group_set;
    p|idx_dir_set;
    p|idx_zone_set;
    p|num_groups;
    p|num_directions;
    p|num_zones;
    PUParray(p, zeros, 3);
    PUParray(p, nzones, 3);
    for (int i=0; i<3; i++) p|deltas[i];
    p|group0;
    p|direction0;
    p|sweep_block;
    PUParray(p, upwind, 3);
    PUParray(p, downwind, 3);
    p|deps;
    // Grid_Data manages the memory for Subdomain's members: directions, ell, ell_plus, 
    // ... phi, and phi_out. So Grid_Data is responsible for PUP'ing them.
    if (p.isUnpacking()) {
      for (int i=0; i<3; i++) plane_data[i] = new SubTVec;
      psi = new SubTVec;
      rhs = new SubTVec;
      sigt = new SubTVec;
    }
    for (int i=0; i<3; i++) p|*plane_data[i];
    p|*psi;
    p|*rhs;
    p|*sigt;
    PUParray(p, reg_volume, 3);
    p|volume;
    p|mixed_to_zones;
    p|num_mixed;
    p|zones_to_mixed;
    p|mixed_material;
    p|mixed_fraction;
    if (p.isDeleting()) {
      delete psi;
      delete rhs;
      delete sigt;
      for (int i=0; i<3; i++) delete plane_data[i];
    }
  }
};

#endif
