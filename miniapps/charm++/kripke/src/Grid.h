#ifndef KRIPKE_GRID_DATA_H__
#define KRIPKE_GRID_DATA_H__

#include "Timing.h"
#include "Directions.h"
#include "Kernel.h"
#include "Subdomain.h"
#include <vector>

// Forward Decl
struct Input_Variables;
struct Grid_Data;
struct SubTVec;
class Timing;

/**
 * Contains all grid parameters and variables.
 */
struct Grid_Data {
public:
  explicit Grid_Data(Input_Variables *input_vars, int chareIdx[3]);
  Grid_Data(){}
  ~Grid_Data();

  void randomizeData(void);
  void copy(Grid_Data const &b);
  bool compare(Grid_Data const &b, double tol, bool verbose);
  double particleEdit(void);
#ifdef KRIPKE_USE_SILO
  void writeSilo(std::string const &fname);
#endif
  int setSubdomainId(int gs, int ds, int zs);
  void pup(PUP::er &p);

  Nesting_Order nesting;
  Timing timing;
  double source_value;
  std::vector<double> sigma_tot;            // Cross section data

  int num_group_sets;                       // Number of group-sets
  int num_groups_per_set;                   // How many groups in each set
  int num_direction_sets;                   // Number of direction-sets
  int num_directions_per_set;               // Number of directions per dir set
  int num_zone_sets;                        // Number of zone sets
  int num_zone_sets_dim[3];                 // Number of zone sets per dim
  int legendre_order;                       // Legendre expansion order ( >= 0 )
  int total_num_moments;                    // Number of spherical harmonic moments

  std::vector<int> moment_to_coeff;         // Map from harmonic moments to legendre coefficients

  std::vector<Directions> directions;       // Quadrature point data, for all directions
  Kernel *kernel;                           // Layout-specific math kernels

  std::vector<Subdomain> subdomains;        // Group/Angle/Zone set data
  std::vector<int> zs_to_sdomid;            // map of zonesets to subdomains with ds=gs=0

  // Variables:
  SubTVec *sigs;                            // scattering lookup table for each material
                                            // G=g->gp, D=legendre coeff, Z=matidx

  // Per directionset ell and ell_plus matrices (Subdomain point into these arrays)
  std::vector<SubTVec *> ell;               // L matrix in nm_offset coordinates
  std::vector<SubTVec *> ell_plus;          // L+ matrix in nm_offset coordinates

  // Per zoneset phi and phi_out (Subdomains point into these arrays)
  std::vector<SubTVec *> phi;               // Moments of psi
  std::vector<SubTVec *> phi_out;           // Scattering source

  inline Grid_Data &operator=(const Grid_Data &in) {
    nesting = in.nesting;
    timing = in.timing;
    source_value = in.source_value;
    sigma_tot = in.sigma_tot;
    num_group_sets = in.num_group_sets;
    num_groups_per_set = in.num_groups_per_set;
    num_direction_sets = in.num_direction_sets;
    num_directions_per_set = in.num_directions_per_set;
    num_zone_sets = in.num_zone_sets;
    for (int i=0; i<3; i++) num_zone_sets_dim[i] = in.num_zone_sets_dim[i];
    legendre_order = in.legendre_order;
    total_num_moments = in.total_num_moments;
    moment_to_coeff = in.moment_to_coeff;
    directions = in.directions;
    kernel = in.kernel;
    subdomains = in.subdomains;
    sigs = in.sigs;
    ell = in.ell;
    ell_plus = in.ell_plus;
    phi = in.phi;
    phi_out = in.phi_out;
    return *this;
  }
};

#endif
