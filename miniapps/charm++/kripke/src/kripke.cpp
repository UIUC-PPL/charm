/* 
 * This is a Charm++ version of Kripke-1.1
 */
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <string>
#include <sstream>
#include "Kripke.h"

/*readonly*/ CProxy_Main mainProxy;
/*readonly*/ CProxy_Kripke arrayProxy;
/*readonly*/ int zsDimX;
/*readonly*/ int zsDimY;
/*readonly*/ int zsDimZ;
/*readonly*/ int nIter;
/*readonly*/ bool loadBalance;
/*readonly*/ int lbIter;

#ifdef KRIPKE_USE_PAPI
std::vector<std::string> papi_names;
#endif

#ifdef KRIPKE_USE_TCMALLOC
#include<gperftools/malloc_extension.h>
#endif

#ifdef __bgq__
#include </bgsys/drivers/ppcfloor/spi/include/kernel/location.h>
#include </bgsys/drivers/ppcfloor/spi/include/kernel/memory.h>
#endif

void usage(void){
  // Get a new object with defaulted values
  Input_Variables def;

  // Display command line
  CkPrintf("Usage:  [./charmrun +p <N>] ./kripke [options ...]\n\n");

  // Display each option
  CkPrintf("Problem Size Options:\n");
  CkPrintf("---------------------\n");

  CkPrintf("  --groups <ngroups>     Number of energy groups\n");
  CkPrintf("                         Default:  --groups %d\n\n", def.num_groups);

  CkPrintf("  --zones <x,y,z>        Number of zones in x,y,z\n");
  CkPrintf("                         Default:  --zones %d,%d,%d\n\n", def.nx, def.ny, def.nz);
  
  CkPrintf("  --legendre <lorder>    Scattering Legendre Expansion Order (0, 1, ...)\n");
  CkPrintf("                         Default:  --legendre %d\n\n", def.legendre_order);

  CkPrintf("  --quad [<ndirs>|<polar>:<azim>]\n");
  CkPrintf("                         Define the quadrature set to use\n");
  CkPrintf("                         Either a fake S2 with <ndirs> points,\n");
  CkPrintf("                         OR Gauss-Legendre with <polar> by <azim> points\n");
  CkPrintf("                         Default:  --quad %d\n\n", def.num_directions);

  CkPrintf("\n");
  CkPrintf("Physics Parameters:\n");
  CkPrintf("-------------------\n");
  CkPrintf("  --sigt <st0,st1,st2>   Total material cross-sections\n");
  CkPrintf("                         Default:   --sigt %lf,%lf,%lf\n\n", def.sigt[0], def.sigt[1], def.sigt[2]);

  CkPrintf("  --sigs <ss0,ss1,ss2>   Scattering material cross-sections\n");
  CkPrintf("                         Default:   --sigs %lf,%lf,%lf\n\n", def.sigs[0], def.sigs[1], def.sigs[2]);

  CkPrintf("\n");
  CkPrintf("On-Node Options:\n");
  CkPrintf("----------------\n");
  CkPrintf("  --nest <NEST>          Loop nesting order (and data layout)\n");
  CkPrintf("                         Available: DGZ,DZG,GDZ,GZD,ZDG,ZGD\n");
  CkPrintf("                         Default:   --nest %s\n\n", nestingString(def.nesting).c_str());

  CkPrintf("\n");
  CkPrintf("Parallel Decomposition Options:\n");
  CkPrintf("-------------------------------\n");
  CkPrintf("  --layout <lout>        Layout of spatial subdomains over mpi ranks\n");
  CkPrintf("                         0: Blocked: local zone sets are adjacent\n");
  CkPrintf("                         1: Scattered: adjacent zone sets are distributed\n");
  CkPrintf("                         Default: --layout 0\n\n");

  CkPrintf("  --dset <ds>            Number of direction-sets\n");
  CkPrintf("                         Must be a factor of 8, and divide evenly the number\n");
  CkPrintf("                         of quadrature points\n");
  CkPrintf("                         Default:  --dset %d\n\n", def.num_dirsets);

  CkPrintf("  --gset <gs>            Number of energy group-sets\n");
  CkPrintf("                         Must divide evenly the number energy groups\n");
  CkPrintf("                         Default:  --gset %d\n\n", def.num_groupsets);

  CkPrintf("  --zset <zx>,<zy>,<zz>  Number of zone-sets in x,y, and z\n");
  CkPrintf("                         Default:  --zset %d,%d,%d\n\n", def.num_zonesets_dim[0], def.num_zonesets_dim[1], def.num_zonesets_dim[2]);

  CkPrintf("\n");
  CkPrintf("Solver Options:\n");
  CkPrintf("---------------\n");

  CkPrintf("  --niter <NITER>        Number of solver iterations to run\n");
  CkPrintf("                         Default:  --niter %d\n\n", nIter);
  CkPrintf("  --lbiter <LBITER>      Iteration after which to load balance (default: -1)\n");

  CkPrintf("\n");
  CkPrintf("Output and Testing Options:\n");
  CkPrintf("---------------------------\n");

#ifdef KRIPKE_USE_PAPI
  CkPrintf("  --papi <PAPI_X_X,...>  Track PAPI hardware counters for each timer\n\n");
#endif
#ifdef KRIPKE_USE_SILO
  CkPrintf("  --silo <BASENAME>      Create SILO output files\n\n");
#endif
  CkPrintf("\n");
  CkExit();
}

struct CmdLine{
  CmdLine(int argc, char **argv) :
    size(argc-1),
    cur(0),
    args()
  {
    for(int i = 0;i < size;++ i){
      args.push_back(argv[i+1]);
    }
  }

  std::string pop(void){
    if(atEnd())
      usage();
    return args[cur++];
  }

  bool atEnd(void){
    return(cur >= size);
  }

  int size;
  int cur;
  std::vector<std::string> args;
};

std::vector<std::string> split(std::string const &str, char delim){
  std::vector<std::string> elem;
  std::stringstream ss(str);
  std::string e;
  while(std::getline(ss, e, delim)){
    elem.push_back(e);
  }
  return elem;
}

namespace{
  template<typename T>
  std::string toString(T const &val){
    std::stringstream ss;
    ss << val;
    return ss.str();
  }
}


/**
 * Main chare
 */
Main::Main(CkArgMsg *m) {
  /* Print out a banner message along with a version number. */
  CkPrintf("\n");
  CkPrintf("---------------------------------------------------------\n");
  CkPrintf("------------------- KRIPKE VERSION 1.1 ------------------\n");
  CkPrintf("---------------------------------------------------------\n");

  /*
   * Default input parameters
   */
  std::vector<std::string> papi_names;
  int layout = 0;
  nIter = 10;
  loadBalance = false;
  lbIter = -1;

  /*
   * Parse command line
   */
  CmdLine cmd(m->argc, m->argv);
  while(!cmd.atEnd()){
    std::string opt = cmd.pop();
    if(opt == "-h" || opt == "--help"){usage();}
    else if(opt == "--name"){vars.run_name = cmd.pop();}
    else if(opt == "--dset"){
      vars.num_dirsets = std::atoi(cmd.pop().c_str());      
    }
    else if(opt == "--gset"){
      vars.num_groupsets = std::atoi(cmd.pop().c_str());      
    }
    else if(opt == "--zset"){
      std::vector<std::string> nz = split(cmd.pop(), ',');
      if(nz.size() != 3) usage();
      vars.num_zonesets_dim[0] = std::atoi(nz[0].c_str());
      vars.num_zonesets_dim[1] = std::atoi(nz[1].c_str());
      vars.num_zonesets_dim[2] = std::atoi(nz[2].c_str());      
    }
    else if(opt == "--layout"){
      layout = std::atoi(cmd.pop().c_str());      
    }
    else if(opt == "--groups"){
      vars.num_groups = std::atoi(cmd.pop().c_str());      
    }
    else if(opt == "--zones"){
      std::vector<std::string> nz = split(cmd.pop(), ',');
      if(nz.size() != 3) usage();
      vars.nx = std::atoi(nz[0].c_str());
      vars.ny = std::atoi(nz[1].c_str());
      vars.nz = std::atoi(nz[2].c_str());
    }
    else if(opt == "--quad"){
      std::vector<std::string> p = split(cmd.pop(), ':');
      if(p.size() == 1){
        vars.num_directions = std::atoi(p[0].c_str());
        vars.quad_num_polar = 0;
        vars.quad_num_azimuthal = 0;
      }
      else if(p.size() == 2){
        vars.quad_num_polar = std::atoi(p[0].c_str());
        vars.quad_num_azimuthal = std::atoi(p[1].c_str());
        vars.num_directions = vars.quad_num_polar * vars.quad_num_azimuthal;
      }
      else{
        usage();
      }
    }
    else if(opt == "--legendre"){
      vars.legendre_order = std::atoi(cmd.pop().c_str());
    }
    else if(opt == "--sigs"){
      std::vector<std::string> values = split(cmd.pop(), ',');
      if(values.size()!=3)usage();
      for(int mat = 0;mat < 3;++ mat){
        vars.sigs[mat] = std::atof(values[mat].c_str());
      }
    }
    else if(opt == "--sigt"){
      std::vector<std::string> values = split(cmd.pop(), ',');
      if(values.size()!=3)usage();
      for(int mat = 0;mat < 3;++ mat){
        vars.sigt[mat] = std::atof(values[mat].c_str());
      }
    }
    else if(opt == "--niter"){
      nIter = std::atoi(cmd.pop().c_str());
    }
    else if(opt == "--lbiter"){
      lbIter = std::atoi(cmd.pop().c_str());
      loadBalance = true;
    }
    else if(opt == "--nest"){
      vars.nesting = nestingFromString(cmd.pop());     
    }
#ifdef KRIPKE_USE_SILO
    else if(opt == "--silo"){
      vars.silo_basename = cmd.pop();
    }
#endif
#ifdef KRIPKE_USE_PAPI
    else if(opt == "--papi"){
      papi_names = split(cmd.pop(), ',');
    }
#endif
    else{
      CkPrintf("Unknown options %s\n", opt.c_str());
      usage();
    }
  }

  // Check that the input arguments are valid
  if(vars.checkValues()){
    CkExit();
  }

  // Set chare array dimensions to zoneset dimensions
  zsDimX = vars.num_zonesets_dim[0]; 
  zsDimY = vars.num_zonesets_dim[1]; 
  zsDimZ = vars.num_zonesets_dim[2];

  /*
   * Display Options
   */
  CkPrintf("Chare Array Dims:      %d x %d x %d\n", zsDimX, zsDimY, zsDimZ);
  CkPrintf("Processors:            %d\n", CkNumPes());
  CkPrintf("Zones:                 %d x %d x %d\n", vars.nx, vars.ny, vars.nz);
  CkPrintf("Legendre Order:        %d\n", vars.legendre_order);
  CkPrintf("Total X-Sec:           sigt=[%lf, %lf, %lf]\n", vars.sigt[0], vars.sigt[1], vars.sigt[2]);
  CkPrintf("Scattering X-Sec:      sigs=[%lf, %lf, %lf]\n", vars.sigs[0], vars.sigs[1], vars.sigs[2]);
  CkPrintf("Quadrature Set:        ");
  if(vars.quad_num_polar == 0){
    CkPrintf("Dummy S2 with %d points\n", vars.num_directions);
  }
  else {
    CkPrintf("Gauss-Legendre, %d polar, %d azimuthal (%d points)\n", vars.quad_num_polar, vars.quad_num_azimuthal, vars.num_directions);
  }
  CkPrintf("Layout:                %s\n", (layout==0)?"Blocked":"Scattered");
  CkPrintf("Loop Nesting Order:    %s\n", nestingString(vars.nesting).c_str());
  CkPrintf("Number iterations:     %d\n", nIter);
  CkPrintf("Load balancing iter:   %d\n", lbIter);
  CkPrintf("GroupSet/Groups:       %d sets, %d groups/set\n", vars.num_groupsets, vars.num_groups/vars.num_groupsets);
  CkPrintf("DirSets/Directions:    %d sets, %d directions/set\n", vars.num_dirsets, vars.num_directions/vars.num_dirsets);
  CkPrintf("Zone Sets:             %d,%d,%d\n", vars.num_zonesets_dim[0], vars.num_zonesets_dim[1], vars.num_zonesets_dim[2]);
  
  if (zsDimX * zsDimY * zsDimZ < CkNumPes()) {
    CkPrintf("\nWARNING: Running Kripke with fewer zonesets or chares (%d) than cores (%d) ...\n",
            zsDimX * zsDimY * zsDimZ, CkNumPes());
    CkPrintf("Use the '--zset' option to specify the global number of zone sets.\n\n");
  }
  delete m;

  iter = 0; 
  part_last = 0.0;
  mainProxy = thisProxy;

  // Setup the chare array (blocked or scattered layout)
  CkArrayOptions opts(zsDimX, zsDimY, zsDimZ);
  if (layout != 0) {
    CProxy_ScatterLayout scatterMap = CProxy_ScatterLayout::ckNew();
    opts.setMap(scatterMap);
  }
  arrayProxy = CProxy_Kripke::ckNew(vars, opts);
  
  // Run the solver
  arrayProxy.run();
}

/*
 * Reduction target for Kripke chares to call when done.
 */
void Main::done() {
  // Get timing info
  arrayProxy(zsDimX-1, zsDimY-1, zsDimZ-1).getTimers();
}

/* 
 * Output timers and memory usage info.
 */
void Main::outputStats(Timing timing) {
  // Print timing info
  timing.print();
  CkPrintf("\n\n");

  // Gather post-run memory info
  double heap_mb = -1.0;
  double hwm_mb = -1.0;
#ifdef KRIPKE_USE_TCMALLOC
  // If we are using tcmalloc, we need to use its interface
  MallocExtension *mext = MallocExtension::instance();
  size_t bytes;

  mext->GetNumericProperty("generic.current_allocated_bytes", &bytes);
  heap_mb = ((double)bytes)/1024.0/1024.0;

  mext->GetNumericProperty("generic.heap_size", &bytes);
  hwm_mb = ((double)bytes)/1024.0/1024.0;
#else
#ifdef __bgq__
  // use BG/Q specific calls (if NOT using tcmalloc)
  uint64_t bytes;

  int rc = Kernel_GetMemorySize(KERNEL_MEMSIZE_HEAP, &bytes);
  heap_mb = ((double)bytes)/1024.0/1024.0;

  rc = Kernel_GetMemorySize(KERNEL_MEMSIZE_HEAPMAX, &bytes);
  hwm_mb = ((double)bytes)/1024.0/1024.0;
#endif
#endif
  // Print memory info
  if(heap_mb >= 0.0){
    CkPrintf("Bytes allocated: %lf MB\n", heap_mb);
    CkPrintf("Heap Size      : %lf MB\n", hwm_mb);
  }

  CkExit();
}

/*
 * Reduction target gets the sum of all unknown counts.
 */
void Main::unknownCounts(long psi, long rhs, long phi, long phi_out) {
  CkPrintf("Unknown counts: psi=%ld, rhs=%ld, phi=%ld, phi_out=%ld\n",
          psi, rhs, phi, phi_out);
}

/*
 * Reduction target gets the sum of all region volumes.
 */
void Main::regionVolumes(double reg1, double reg2, double reg3) {
  CkPrintf("Region volumes: Reg1=%e, Reg2=%e, Reg3=%e\n", reg1, reg2, reg3);
}

/*
 * Reduction target gets the sum of all particles.
 */
void Main::particleEdit(double part) {
  CkPrintf("iter %d: particle count=%e, change=%e\n", iter, part, (part-part_last)/part);
  iter++;
  part_last = part;
}


/** 
 * Kripke 3D chare array
 */
Kripke::Kripke(Input_Variables ivars) {

  int zsIdx[3] = {thisIndex.x, thisIndex.y, thisIndex.z};
  usesAtSync = true;

  // Initialize grid data
  grid_data = new Grid_Data(&ivars, zsIdx);
  kernel = grid_data->kernel;
#ifdef KRIPKE_USE_PAPI
  grid_data->timing.setPapiEvents(papi_names);
#endif
  
  // Initialize total communication dependencies for this chare
  currDeps = new int[grid_data->subdomains.size()];
  domainCorner = false; 
  totalDeps = 0;
  setDomainCorner();
  setTotalSweepDeps();

  // Contribute to reductions for unknown counts and region volumes
  contributeLocalInfo();
}

Kripke::Kripke(CkMigrateMessage *m) {}

Kripke::~Kripke() {
  delete grid_data;
  delete [] currDeps;
}

/*
 * Pack-UnPack method.
 */
void Kripke::pup(PUP::er &p) {
  p|iter; p|dep; p|totalDeps;
  p|domainCorner;
  if (p.isUnpacking()) grid_data = new Grid_Data;
  p|*grid_data;

  // kernel is allocated in grid_data.
  // currDeps is populated by updateSweepDeps() every iter.
  if (p.isUnpacking()) {
    kernel = grid_data->kernel;
    currDeps = new int[grid_data->subdomains.size()];
  }
}

/*
 * Contribute local unknown counts and region volumes to global count.
 */
void Kripke::contributeLocalInfo() {

  long unknowns[4] = {0, 0, 0, 0};
  double volumes[3] = {0.0, 0.0, 0.0};

  for(int sdom_id = 0;sdom_id < grid_data->subdomains.size();++sdom_id){
    Subdomain &sdom = grid_data->subdomains[sdom_id];
    unknowns[0] += sdom.psi->elements;
    unknowns[1] += sdom.psi->elements;
  }
  for (int zs=0; zs < grid_data->num_zone_sets; zs++) {
    unknowns[2] += grid_data->phi[zs]->elements;
    unknowns[3] += grid_data->phi_out[zs]->elements;
    int sdom_id = grid_data->zs_to_sdomid[zs];
    
    for (int mat=0; mat < 3; mat++) {
      volumes[mat] += grid_data->subdomains[sdom_id].reg_volume[mat];
    }
  }

  // Unknown count reduction
  contribute(4*sizeof(long), unknowns, CkReduction::sum_long, 
            CkCallback(CkReductionTarget(Main, unknownCounts), mainProxy));
  
  // Region volume reduction
  contribute(3*sizeof(double), volumes, CkReduction::sum_double, 
            CkCallback(CkReductionTarget(Main, regionVolumes), mainProxy));
}

/*
 * Update the total number of particles on this chare.
 */
double Kripke::getNumParticles() {
  double numParticles = 0.0;

  for(int sdom_id = 0;sdom_id < grid_data->subdomains.size();++ sdom_id){
    Subdomain &sdom = grid_data->subdomains[sdom_id];

    int num_zones = sdom.num_zones;
    int num_directions = sdom.num_directions;
    int num_groups= sdom.num_groups;
    Directions *dirs = sdom.directions;

    for(int z = 0;z < num_zones;++ z){
      double vol = sdom.volume[z];
      for(int d = 0;d < num_directions;++ d){
        double w = dirs[d].w;
        for(int g = 0;g < num_groups;++ g){
          numParticles += w * (*sdom.psi)(g,d,z) * vol;
        }
      }
    }
  }
  return numParticles;
}

/*
 * Is this chare one of the 8 domain corners?
 */
inline void Kripke::setDomainCorner() {
  for (int sdom_id=0; sdom_id < grid_data->subdomains.size(); sdom_id++) {
    if (grid_data->subdomains[sdom_id].deps == 0) {
      domainCorner = true;
    }
  }
}

/*
 * Set the total number of dependencies for this chare. 
 */
inline void Kripke::setTotalSweepDeps() {
  for (int sdom_id=0; sdom_id < grid_data->subdomains.size(); sdom_id++) {
    totalDeps += grid_data->subdomains[sdom_id].deps;
  }
}

/*
 * Update dependencies between iterations.
 */
inline void Kripke::updateSweepDeps() {
  for (int sdom_id=0; sdom_id < grid_data->subdomains.size(); sdom_id++) {
    currDeps[sdom_id] = grid_data->subdomains[sdom_id].deps;
  }
}

/*
 * Check boundary conditions for a given subdomain
 * and clear out plane_data for boundary conditions.
 */
inline void Kripke::applyBoundaryConditions(Subdomain *sdom) {
  for (int dim=0; dim<3; dim++) {
    if (sdom->upwind[dim].x == -1) {
      sdom->plane_data[dim]->clear(0.0);
    }
  }
}

/*
 * Entry method to pass timing info to main chare for output.
 */
void Kripke::getTimers() {
  mainProxy.outputStats(grid_data->timing);
}

/** 
 * Scattered layout is implemented using a chare array map.
 */
ScatterLayout::ScatterLayout() {}

ScatterLayout::~ScatterLayout() {}

int ScatterLayout::procNum(int, const CkArrayIndex &idx) {
  return ((idx.data()[0]*zsDimY*zsDimZ) + (idx.data()[1]*zsDimZ) + idx.data()[2]) % CkNumPes();
}

#include "kripke.def.h"
