#include "Headers.h"
#include <assert.h>

using std::max;
using std::min;

CProxy_Advection qtree;
CProxy_Main mainProxy;

int array_height;
int array_width;
int array_depth;

int num_chare_rows;
int num_chare_cols;
int num_chare_Zs;

int block_height;
int block_width;
int block_depth;

int min_depth, max_depth;
int max_iterations, refine_frequency;
int lb_freq;//load balancing frequency

const char amrRevision[] = INQUOTES(AMR_REVISION);

float xmin, xmax, ymin, ymax, zmin, zmax;
float xctr, yctr, zctr, radius;
float dx, dy, dz, vx, vy, vz;
float apx, anx, apy, any, apz, anz;
float tmax, t, dt, cfl;
//bool inInitialMeshGenerationPhase;

float start_time, end_time;

Main::Main(CkArgMsg* m){
  ckout<<"Running amr code revision: "<<amrRevision<<endl;

  mainProxy = thisProxy;
  iterations = 0;

  if(m->argc < 5){
    ckout << "Usage: " << m->argv[0] << "[max_depth] [block_size] [iterations] [lb_freq] [array_dim]?" << endl; 
    CkExit();
  }

  if (m->argc >= 6) array_height = array_width = array_depth = atoi(m->argv[5]);
  else array_height = array_width = array_depth = 128;

  block_height = block_width = block_depth = atoi(m->argv[2]);
  max_iterations = atoi(m->argv[3]);
  lb_freq = atoi(m->argv[4]);
  refine_frequency = 3;
  if(lb_freq%refine_frequency != 0){
    ckout << "load balancing frequency should be a mulitple of refine frequency(3)" << endl;
    CkExit();
  }
  if(array_width%block_width < 0 || array_width < block_width){
    ckout << "Incompatible arguments" << endl;
    CkExit();
  }

  num_chare_rows = num_chare_cols = num_chare_Zs = array_height/block_height;

  /******** Do Some Initialization *********/
  xmin = 0;
  xmax = 1;
  ymin = 0;
  ymax = 1;
  zmin = 0;
  zmax = 1;
  t = 0;
  tmax = 10000;
  cfl = 0.4;
  vx = 0.0;
  vy = 0.0;
  vz = 0.1;
  //inInitialMeshGenerationPhase = true;

  // XXX: Why don't we initialize dx/dy/dz?
  dx = (xmax - xmin)/float(array_width);
  dy = (ymax - ymin)/float(array_height);
  dz = (zmax - zmin)/float(array_depth);

  xctr = 0.5;
  yctr = 0.5;
  zctr = 0.5;

  radius = 0.2;

  apx = max(vx, (float)0.0);
  anx = min(vx, (float)0.0);
  apy = max(vy, (float)0.0);
  any = min(vy, (float)0.0);
  apz = max(vz, (float)0.0);
  anz = min(vz, (float)0.0);

  /*****End Initialization **********/
  CProxy_AdvMap map = CProxy_AdvMap::ckNew();
  CkArrayOptions opts;
  opts.setMap(map);
  qtree = CProxy_Advection::ckNew(opts);

  //save the total number of worker chares we have in this simulation
  num_chares = num_chare_rows*num_chare_cols*num_chare_Zs;
  float fdepth = (log(num_chares)/log(NUM_CHILDREN));
  int depth = (fabs(fdepth - ceil(fdepth)) < 0.000001)?ceil(fdepth):floor(fdepth);
  min_depth = depth;
  //CkAssert(min_depth >= 4);

  // To maintain the semantics of "max_depth" that set it relative to
  // a grid fo 256, offset by 4
  // XXX: Revisit this logic
  max_depth = atoi(m->argv[1]);// + min_depth - 4;
  // Magic assert
  assert((max_depth < 11) && "Depth too high for bitvector index");

  dt = min(min(dx,dy),dz)/sqrt(vx*vx+vy*vy+vz*vz) * cfl;
  dt /= pow(2., max_depth - min_depth);
  if ((t + dt) >= tmax )
    dt = tmax - t;
  t = t+dt;
    
  printf("dx: %f, apx: %f, anx: %f, dt: %f, t: %f\n", dx, apx, anx, dt, t);

  CkPrintf("Running Advection on %d processors with (%d,%d,%d) elements, minDepth = %d, maxDepth = %d, blockSize = %d, maxIter = %d\n",
           CkNumPes(), array_width, array_height, array_width, min_depth, max_depth, block_height, max_iterations);

  for (int i = 0; i < num_chare_rows; ++i)
    for (int j = 0; j < num_chare_cols; ++j)
      for (int k = 0; k < num_chare_Zs; ++k)
        qtree[OctIndex(i, j, k, min_depth)].insert(xmin, xmax, ymin, ymax, zmin, zmax);

  qtree.doneInserting();

  CkStartQD(CkCallback(CkIndex_Main::startMeshGeneration(), thisProxy));
  ppc = CProxy_AdvectionGroup::ckNew();

}

void Main::startMeshGeneration() {
  start_time = CkWallTimer();
  qtree.iterate();
}

void Main::terminate(){
  ckout << "simulation time: " << CkWallTimer() - start_time << " s" << endl;
  ppc.reduceWorkUnits();
}

void Main::totalWorkUnits(int total) {
  CkPrintf("total work units = %d\n", total);
  nresponses = 0;
  ppc.reduceQdTimes();
}

#define GOLDEN_RATIO_PRIME_64 0x9e37fffffffc0001ULL

struct AdvMap : public CBase_AdvMap {
  int bits;
  AdvMap() : bits(log2(CkNumPes())) { }
  
  void pup(PUP::er &p){ bits = log2(CkNumPes()); }
  AdvMap(CkMigrateMessage *m): CBase_AdvMap(m),bits(log2(CkNumPes())){}

  int procNum(int arrayHdl, const CkArrayIndex& i) {
    int numPes = CkNumPes();
    const OctIndex& idx = *reinterpret_cast<const OctIndex*>(i.data());
    int baseBits = 8;

    unsigned long long val = idx.bitVector >> (sizeof(unsigned int)*8 - baseBits);
    unsigned long long hash = GOLDEN_RATIO_PRIME_64 * val;

    int basePE = hash >> (64 - bits);

    unsigned long validBits = idx.bitVector & ((1L << 24) - 1);
    validBits += (1L << 22);
    unsigned long offset = validBits >> (sizeof(unsigned int)*8 - idx.nbits);
    offset += (idx.nbits == 8);

    int pe = (basePE + offset - 1) % numPes;

    return pe;
  }
};

#include "Main.def.h"
