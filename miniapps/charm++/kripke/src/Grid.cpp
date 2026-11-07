#include "Grid.h"

#include "Input_Variables.h"
#include "SubTVec.h"
#include <cmath>
#include <sstream>
#include "pup_stl.h"

#ifdef KRIPKE_USE_SILO
#include <sys/stat.h>
#include <silo.h>
#include <string.h>
#endif

/**
 * Grid_Data constructor
 */
Grid_Data::Grid_Data(Input_Variables *input_vars, int zsIdx[3])
{
  // create the kernel object based on nesting
  kernel = createKernel(input_vars->nesting, 3);

  // Create quadrature set (for all directions)
  int total_num_directions = input_vars->num_directions;
  InitDirections(this, input_vars);

  num_direction_sets = input_vars->num_dirsets;
  num_directions_per_set = total_num_directions / num_direction_sets;
  num_group_sets = input_vars->num_groupsets;
  num_groups_per_set = input_vars->num_groups / num_group_sets;
  num_zone_sets = 1;

  legendre_order = input_vars->legendre_order;
  total_num_moments = (legendre_order+1) * (legendre_order+1);

  int num_subdomains = num_direction_sets * num_group_sets * num_zone_sets;

  nesting = input_vars->nesting;

  // setup mapping of moments to legendre coefficients
  moment_to_coeff.resize(total_num_moments);
  int nm = 0;
  for(int n = 0;n < legendre_order+1;++ n){
    for(int m = -n;m <= n; ++ m){
      moment_to_coeff[nm] = n;
      ++ nm;
    }
  }

  // setup cross-sections
  int total_num_groups = num_group_sets * num_groups_per_set;
  sigma_tot.resize(total_num_groups, 0.0);

  // Setup scattering transfer matrix for 3 materials
  sigs = new SubTVec(kernel->nestingSigs(), total_num_groups * total_num_groups, legendre_order+1, 3);

  // Set to isotropic scattering given user inputs
  sigs->clear(0.0);
  for(int mat = 0;mat < 3;++ mat){
    for(int g = 0;g < total_num_groups;++ g){
      int idx_g_gp = g*total_num_groups + g;
      (*sigs)(idx_g_gp, 0, mat) = input_vars->sigs[mat];
    }
  }

  // just allocate pointer vectors, we will allocate them below
  ell.resize(num_direction_sets, NULL);
  ell_plus.resize(num_direction_sets, NULL);
  phi.resize(num_zone_sets, NULL);
  phi_out.resize(num_zone_sets, NULL);

  // Initialize Subdomains
  zs_to_sdomid.resize(num_zone_sets);
  subdomains.resize(num_subdomains);
  for(int gs = 0;gs < num_group_sets;++ gs){
    for(int ds = 0;ds < num_direction_sets;++ ds){
      for(int zs = 0;zs < num_zone_sets;++ zs){

        // Compute subdomain id
        int sdom_id = setSubdomainId(gs, ds, zs);

        // Setup the subdomain
        Subdomain &sdom = subdomains[sdom_id];
        sdom.setup(zsIdx, sdom_id, input_vars, gs, ds, zs, directions, kernel);

        // Create ell and ell_plus, if this is the first of this ds
        bool compute_ell = false;
        if(ell[ds] == NULL){
          ell[ds] = new SubTVec(kernel->nestingEll(), total_num_moments, sdom.num_directions, 1);
          ell_plus[ds] = new SubTVec(kernel->nestingEllPlus(), total_num_moments, sdom.num_directions, 1);

          compute_ell = true;
        }

        // Create phi and phi_out, if this is the first of this zs
        if(phi[zs] == NULL){
          phi[zs] = new SubTVec(nesting, total_num_groups, total_num_moments, sdom.num_zones);
          phi_out[zs] = new SubTVec(nesting, total_num_groups, total_num_moments, sdom.num_zones);
        }

        // setup zs to sdom mapping
        if(gs == 0 && ds == 0){
          zs_to_sdomid[zs] = 0; //sdom_id;
        }

        // Set the variables for this subdomain
        sdom.setVars(ell[ds], ell_plus[ds], phi[zs], phi_out[zs]);

        if(compute_ell){
          // Compute the L and L+ matrices
          sdom.computeLLPlus(legendre_order);
        }
      }
    }
  }
}

Grid_Data::~Grid_Data(){
  delete kernel;
  for(int zs = 0;zs < num_zone_sets;++ zs){
    delete phi[zs];
    delete phi_out[zs];
  }
  for(int ds = 0;ds < num_direction_sets;++ ds){
    delete ell[ds];
    delete ell_plus[ds];
  }
  delete sigs;
}

/*
 * Set Subdomain ID based on GS, DS, and ZS indices.
 */
int Grid_Data::setSubdomainId(int gs, int ds, int zs) {
  return (gs * num_direction_sets) + ds;
}

/**
 * Pack-UnPack method
 */
void Grid_Data::pup(PUP::er &p) {
  p|nesting;
  p|timing;
  p|source_value;
  p|sigma_tot;
  p|num_group_sets;
  p|num_groups_per_set;
  p|num_direction_sets;
  p|num_directions_per_set;
  p|num_zone_sets;
  PUParray(p, num_zone_sets_dim, 3);
  p|legendre_order;
  p|total_num_moments;
  p|moment_to_coeff;
  p|directions;
  p|subdomains;
  if (p.isUnpacking()) {
    kernel = createKernel(nesting, 3);
    sigs = new SubTVec;

    ell.resize(num_direction_sets);
    ell_plus.resize(num_direction_sets);
    for (int ds = 0; ds < num_direction_sets; ds++) {
      ell[ds] = new SubTVec;
      ell_plus[ds] = new SubTVec;
    }

    phi.resize(num_zone_sets);
    phi_out.resize(num_zone_sets);
    for (int zs = 0; zs < num_zone_sets; zs++) {
      phi[zs] = new SubTVec;
      phi_out[zs] = new SubTVec;
    }
  }
  for (int i = 0; i < num_direction_sets; i++) {
    p|*ell[i];
    p|*ell_plus[i];
  }
  for (int j = 0; j < num_zone_sets; j++) {
    p|*phi[j];
    p|*phi_out[j];
  }
  if (p.isUnpacking()) {
    // Grid_Data manages memory for Subdomain's directions, ell, ell_plus, phi, and phi_out
    // ... so Grid_Data updates its pointers here.
    for (int sdom_id = 0; sdom_id < subdomains.size(); sdom_id++) {
      Subdomain &sdom = subdomains[sdom_id];
      sdom.directions = &directions[sdom.idx_dir_set * sdom.num_directions];
      sdom.ell = ell[sdom.idx_dir_set];
      sdom.ell_plus = ell_plus[sdom.idx_dir_set];
      sdom.phi = phi[sdom.idx_zone_set];
      sdom.phi_out = phi_out[sdom.idx_zone_set];
    }
  }
  p|*sigs;
  p|zs_to_sdomid;
}

/**
 * Randomizes all variables and matrices for testing suite.
 */
void Grid_Data::randomizeData(void){
  for(int i = 0;i < sigma_tot.size();++i){
    sigma_tot[i] = drand48();
  }

  for(int i = 0;i < directions.size();++i){
    directions[i].xcos = drand48();
    directions[i].ycos = drand48();
    directions[i].zcos = drand48();
  }

  for(int s = 0;s < subdomains.size();++ s){
    subdomains[s].randomizeData();
  }

  for(int zs = 0;zs < num_zone_sets;++ zs){
    phi[zs]->randomizeData();
    phi_out[zs]->randomizeData();
  }

  for(int ds = 0;ds < num_direction_sets;++ ds){
    ell[ds]->randomizeData();
    ell_plus[ds]->randomizeData();
  }

  sigs->randomizeData();
}

/**
 * Copies all variables and matrices for testing suite.
 * Correctly copies data from one nesting to another.
 */
void Grid_Data::copy(Grid_Data const &b){
  sigma_tot = b.sigma_tot;
  directions = b.directions;

  subdomains.resize(b.subdomains.size());
  for(int s = 0;s < subdomains.size();++ s){
    subdomains[s].copy(b.subdomains[s]);
  }

  for(int zs = 0;zs < num_zone_sets;++ zs){
    phi[zs]->copy(*b.phi[zs]);
    phi_out[zs]->copy(*b.phi_out[zs]);
  }

  for(int ds = 0;ds < ell.size();++ ds){
    ell[ds]->copy(*b.ell[ds]);
    ell_plus[ds]->copy(*b.ell_plus[ds]);
  }

  sigs->copy(*b.sigs);
}

/**
 * Compares all variables and matrices for testing suite.
 * Correctly compares data from one nesting to another.
 */
bool Grid_Data::compare(Grid_Data const &b, double tol, bool verbose){
  bool is_diff = false;

  for(int i = 0;i < directions.size();++i){
    std::stringstream dirname;
    dirname << "directions[" << i << "]";

    is_diff |= compareScalar(dirname.str()+".xcos",
        directions[i].xcos, b.directions[i].xcos, tol, verbose);

    is_diff |= compareScalar(dirname.str()+".ycos",
        directions[i].ycos, b.directions[i].ycos, tol, verbose);

    is_diff |= compareScalar(dirname.str()+".zcos",
        directions[i].zcos, b.directions[i].zcos, tol, verbose);
  }

  for(int s = 0;s < subdomains.size();++ s){
    is_diff |= subdomains[s].compare(
        b.subdomains[s], tol, verbose);
  }
  is_diff |= compareVector("sigma_tot", sigma_tot, b.sigma_tot, tol, verbose);

  for(int zs = 0;zs < num_zone_sets;++ zs){
    is_diff |= phi[zs]->compare("phi", *b.phi[zs], tol, verbose);
    is_diff |= phi_out[zs]->compare("phi_out", *b.phi_out[zs], tol, verbose);
  }

  for(int ds = 0;ds < ell.size();++ ds){
    is_diff |= ell[ds]->compare("ell", *b.ell[ds], tol, verbose);
    is_diff |= ell_plus[ds]->compare("ell_plus", *b.ell_plus[ds], tol, verbose);
  }

  is_diff |= sigs->compare("sigs", *b.sigs, tol, verbose);

  return is_diff;
}


#ifdef KRIPKE_USE_SILO

enum MultivarType {
  MULTI_MESH,
  MULTI_MAT,
  MULTI_VAR
};

namespace {
  /**
    Writes a multimesh or multivar to the root file.
  */

  void siloWriteMulti(DBfile *root, MultivarType mv_type,
    std::string const &fname_base, std::string const &var_name,
    std::vector<int> sdom_id_list, int var_type = 0)
  {
    int mpi_size;
    //MPI_Comm_size(MPI_COMM_WORLD, &mpi_size);
    int num_sdom = sdom_id_list.size();

    // setup names and types
    std::vector<int> var_types(mpi_size*num_sdom, var_type);
    std::vector<char *> var_names(mpi_size*num_sdom);
    int var_idx = 0;
    for(int rank = 0;rank < mpi_size;++ rank){
      for(int idx = 0;idx < num_sdom;++ idx){
        int sdom_id = sdom_id_list[idx];
        std::stringstream name;
        name << fname_base << "/rank_" << rank << ".silo:/sdom" << sdom_id << "/" << var_name;
        var_names[var_idx] = strdup(name.str().c_str());
        var_idx ++;
      }
    }

    if(mv_type == MULTI_MESH){
      DBPutMultimesh(root, var_name.c_str(), mpi_size*num_sdom,
          &var_names[0], &var_types[0], NULL);
    }
    else if(mv_type == MULTI_MAT){
      DBPutMultimat(root, var_name.c_str(), mpi_size*num_sdom,
          &var_names[0],  NULL);
    }
    else{
      DBPutMultivar(root, var_name.c_str(), mpi_size*num_sdom,
          &var_names[0],  &var_types[0] , NULL);
    }

    // cleanup
    for(int i = 0;i < mpi_size*num_sdom; ++i){
      free(var_names[i]);
    }
  }

  void siloWriteRectMesh(DBfile *silo_file,
    std::string const &mesh_name,
    int const *nzones,
    double const *zeros,
    double const *deltas_x,
    double const *deltas_y,
    double const *deltas_z)
  {
    static char const *coordnames[3] = {"X", "Y", "Z"};
    double const *deltas[3] = {deltas_x, deltas_y, deltas_z};
    double *coords[3];
    for(int dim = 0;dim < 3;++ dim){
      coords[dim] = new double[nzones[dim]];
      coords[dim][0] = zeros[dim];
      for(int z = 0;z < nzones[dim];++ z){
        coords[dim][1+z] = coords[dim][z] + deltas[dim][z];
      }
    }
    int nnodes[3] = {
      nzones[0]+1,
      nzones[1]+1,
      nzones[2]+1
    };

    DBPutQuadmesh(silo_file, mesh_name.c_str(), const_cast<char**>(coordnames), coords, nnodes, 3, DB_DOUBLE,
        DB_COLLINEAR, NULL);

    // cleanup
    delete[] coords[0];
    delete[] coords[1];
    delete[] coords[2];
  }


} //namespace


void Grid_Data::writeSilo(std::string const &fname_base){

  // Recompute Phi... so we can write out phi0
  kernel->LTimes(this);

  int mpi_rank;
  //MPI_Comm_rank(MPI_COMM_WORLD, &mpi_rank);

  if(mpi_rank == 0){
    // Create a root file
    std::string fname_root = fname_base + ".silo";
    DBfile *root = DBCreate(fname_root.c_str(),
        DB_CLOBBER, DB_LOCAL, NULL, DB_HDF5);

    // Write out multimesh and multivars
    siloWriteMulti(root, MULTI_MESH, fname_base, "mesh", zs_to_sdomid, DB_QUAD_RECT);
    siloWriteMulti(root, MULTI_MAT, fname_base, "material", zs_to_sdomid);
    siloWriteMulti(root, MULTI_VAR, fname_base, "phi0", zs_to_sdomid, DB_QUADVAR);

    // Close root file
    DBClose(root);

    // Create a subdirectory to hold processor info
    mkdir(fname_base.c_str(), 0750);
  }

  // Sync up, so everyone sees the subdirectory
  //MPI_Barrier(MPI_COMM_WORLD);

  // Create our processor file
  std::stringstream ss_proc;
  ss_proc << fname_base << "/rank_" << mpi_rank << ".silo";
  DBfile *proc = DBCreate(ss_proc.str().c_str(),
      DB_CLOBBER, DB_LOCAL, NULL, DB_HDF5);

  // Write out data for each subdomain
  int num_zone_sets = zs_to_sdomid.size();
  for(int sdom_idx = 0;sdom_idx < num_zone_sets;++ sdom_idx){
    int sdom_id = zs_to_sdomid[sdom_idx];
    //Subdomain &sdom = subdomains[sdom_id];

    // Create a directory for the subdomain
    std::stringstream dirname;
    dirname << "/sdom" << sdom_id;
    DBMkDir(proc, dirname.str().c_str());

    // Set working directory
    DBSetDir(proc, dirname.str().c_str());

    // Write the mesh
    siloWriteRectMesh(proc, "mesh", sdom.nzones, sdom.zeros,
      &sdom.deltas[0][1], &sdom.deltas[1][1], &sdom.deltas[2][1]);


    // Write the material
    {
      int num_zones = sdom.num_zones;
      int num_mixed = sdom.mixed_material.size();
      int matnos[3] = {1, 2, 3};
      std::vector<int> matlist(num_zones, 0);
      std::vector<int> mix_next(num_mixed, 0);
      std::vector<int> mix_mat(num_mixed, 0);

      // setup matlist and mix_next arrays
      int last_z = -1;
      for(int m = 0;m < num_mixed;++ m){
        mix_mat[m] = sdom.mixed_material[m] + 1;
        int z = sdom.mixed_to_zones[m];
        if(matlist[z] == 0){
            matlist[z] = -(1+m);
        }
        // if we are still on the same zone, make sure the last mix points
        // here
        if(z == last_z){
          mix_next[m-1] = m+1;
        }
        last_z = z;
      }

      DBPutMaterial(proc, "material", "mesh", 3, matnos,
          &matlist[0], sdom.nzones, 3,
          &mix_next[0], &mix_mat[0], &sdom.mixed_to_zones[0], &sdom.mixed_fraction[0], num_mixed,
          DB_DOUBLE, NULL);
    }

    // Write phi0
    {

      int num_zones = sdom.num_zones;
      std::vector<double> phi0(num_zones);

      // extract phi0 from phi for the 0th group
      for(int z = 0;z < num_zones;++ z){
        phi0[z] = (*sdom.phi)(0,0,z);
      }

      DBPutQuadvar1(proc, "phi0", "mesh", &phi0[0],
          sdom.nzones, 3, NULL, 0, DB_DOUBLE, DB_ZONECENT, NULL);
    }
  }

  // Close processor file
  DBClose(proc);
}
#endif

