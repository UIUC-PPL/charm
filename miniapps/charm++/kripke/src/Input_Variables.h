/*--------------------------------------------------------------------------
 * Header file for the Input_Variables structure
 *--------------------------------------------------------------------------*/

#ifndef KRIPKE_INPUT_VARIABLES_H__
#define KRIPKE_INPUT_VARIABLES_H__

#include "Kripke.h"
#include "pup_stl.h"

/**
 * Tags for which data nesting to use.
 */
enum Nesting_Order {
  // Nestings for Psi and Phi
  // D refers to directions OR moments, depending on context
  NEST_DGZ,
  NEST_DZG,
  NEST_GDZ,
  NEST_GZD,
  NEST_ZDG,
  NEST_ZGD
};

inline void operator|(PUP::er &p, Nesting_Order &n) {
  pup_bytes(&p, (void *)&n, sizeof(Nesting_Order));
}

/**
 * This structure defines the input parameters to setup a problem.
 */
struct Input_Variables {
public:
  Input_Variables() :
    run_name("kripke"),
    nx(16), ny(16), nz(16),
    num_directions(96),
    num_groups(32),
    legendre_order(4),
    quad_num_polar(0),
    quad_num_azimuthal(0),

    nesting(NEST_DGZ),

    num_dirsets(8),
    num_groupsets(2)
  {
    num_zonesets_dim[0] = 1;
    num_zonesets_dim[1] = 1;
    num_zonesets_dim[2] = 1;

    sigt[0] = 0.1;
    sigt[1] = 0.0001;
    sigt[2] = 0.1;

    sigs[0] = 0.05;
    sigs[1] = 0.00005;
    sigs[2] = 0.05;
  }

  bool checkValues(void) const {
    if(num_zonesets_dim[0] <= 0 || num_zonesets_dim[1] <= 0 || num_zonesets_dim[2] <= 0){
      CkPrintf("Number of zone-sets in each dim needs to be greater than or equal to 1\n");
      return true;
    }

    if(nesting < 0){
      CkPrintf("Invalid nesting selected\n");
      return true;
    }

    if(num_groups < 1){
      CkPrintf("Number of groups (%d) needs to be at least 1\n", num_groups);
      return true;
    }

    if(num_groups % num_groupsets){
      CkPrintf("Number of groups (%d) must be evenly divided by number of groupsets (%d)\n",
              num_groups, num_groupsets);
      return true;
    }

    if(num_directions < 8){
      CkPrintf("Number of directions (%d) needs to be at least 8\n", num_directions);
      return true;
    }

    if(num_dirsets % 8 && num_dirsets < 8){
      CkPrintf("Number of direction sets (%d) must be a multiple of 8\n", num_dirsets);
      return true;
    }

    if(num_directions % num_dirsets){
      CkPrintf("Number of directions (%d) must be evenly divided by number of directionsets(%d)\n",
              num_directions, num_dirsets);
      return true;
    }

    if(legendre_order < 0){
      CkPrintf("Legendre scattering order (%d) must be >= 0\n", legendre_order);
      return true;
    }

    return false;
  }

  // Problem Description
  int nx, ny, nz;               // Number of spatial zones in x,y,z
  int num_directions;           // Total number of directions
  int num_groups;               // Total number of energy groups
  int legendre_order;           // Scattering order (number Legendre coeff's - 1)
  int quad_num_polar;           // Number of polar quadrature points (0 for dummy)
  int quad_num_azimuthal;       // Number of azimuthal quadrature points (0 for dummy)

  // On-Node Options
  Nesting_Order nesting;        // Data layout and loop ordering (of Psi)

  // Parallel Decomp
  int num_dirsets;              // Number of direction sets
  int num_groupsets;            // Number of energy group sets
  int num_zonesets_dim[3];      // Number of zoneset in x, y, z  

  // Physics and Solver Options
  double sigt[3];               // Total cross section for 3 materials
  double sigs[3];               // Total scattering cross section for 3 materials

  // Output Options
  std::string run_name;         // Name to use when generating output files
#ifdef KRIPKE_USE_SILO
  std::string silo_basename;    // Name prefix for silo output files
#endif

  // Pack-UnPack method
  void pup(PUP::er &p) {
    p|nx; p|ny; p|nz;
    p|num_directions;
    p|num_groups;
    p|legendre_order;
    p|quad_num_polar;
    p|quad_num_azimuthal;
    
    p|nesting;

    p|num_dirsets;
    p|num_groupsets;
    PUParray(p, num_zonesets_dim, 3);
    
    PUParray(p, sigt, 3);
    PUParray(p, sigs, 3);

    p|run_name;
#ifdef KRIPKE_USE_SILO
    p|silo_basename;
#endif
  }
};

#endif
