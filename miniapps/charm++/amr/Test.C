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

  mainProxy = thisProxy;

  CProxy_AdvMap map = CProxy_AdvMap::ckNew();
  CkArrayOptions opts;
  opts.setMap(map);
  qtree = CProxy_Advection::ckNew(opts);

  qtree[OctIndex(0, 0, 0, 1)].insert(0,0,0,0,0,0);
  qtree[OctIndex(0, 0, 1, 1)].insert(0,0,0,0,0,0);

  qtree.doneInserting();

  CkStartQD(CkCallback(CkIndex_Main::startMeshGeneration(), thisProxy));
  //ppc = CProxy_AdvectionGroup::ckNew();

}

void Main::startMeshGeneration() {
  start_time = CkWallTimer();
  qtree.test();
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

void Main::getQdTimes(map<int, pair<float, float> > peQdtimes, map<int, pair<float, float> > peRemeshtimes){
  for(map<int, std::pair<float, float> >::iterator it = peQdtimes.begin(); it!=peQdtimes.end(); it++){
    if(qdtimes.find(it->first) == qdtimes.end())
      qdtimes[it->first] = std::pair<float, float>(std::numeric_limits<float>::min(), std::numeric_limits<float>::max());
    qdtimes[it->first].first = max(qdtimes[it->first].first, it->second.first);
    qdtimes[it->first].second = min(qdtimes[it->first].second, it->second.second);
  }
  for(map<int, std::pair<float, float> >::iterator it = peRemeshtimes.begin(); it!=peRemeshtimes.end(); it++){
    if(remeshtimes.find(it->first) == remeshtimes.end())
      remeshtimes[it->first] = std::pair<float, float>(std::numeric_limits<float>::min(), std::numeric_limits<float>::max());
    remeshtimes[it->first].first = max(remeshtimes[it->first].first, it->second.first);
    remeshtimes[it->first].second = min(remeshtimes[it->first].second, it->second.second);
  }

  if(++nresponses == CkNumPes()){
    ckout << "qd times - ";
    for(map<int, pair<float, float> >::iterator it = qdtimes.begin(); it!=qdtimes.end(); it++)
      ckout << it->first << "," << it->second.second - it->second.first << " ";
    ckout << endl;

    ckout << "remesh times - ";
    for(map<int, pair<float, float> >::iterator it = remeshtimes.begin(); it!=remeshtimes.end(); it++)
      ckout << it->first << "," << it->second.second - it->second.first << " ";
    ckout << endl;
    CkExit();
  }
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
    int baseBits = 3*min_depth;

    //unsigned long long val = idx.bitVector >> (sizeof(unsigned int)*8 - baseBits);
    unsigned long long val = idx.bitVector.rightShit(numBitsForStorage - baseBits).v[0];
    unsigned long long hash = GOLDEN_RATIO_PRIME_64 * val;

    int basePE = hash >> (64 - bits);
    
    ArrayBitVector temp = idx.bitVector.leftShift(baseBits);
    temp = idx.bitVector.rightShit(numBitsForStorage-idx.nbits+baseBits);
    unsigned long long offset = temp.v[1] << 32 + temp.v[1];

    //unsigned long validBits = idx.bitVector & ((1L << 24) - 1);
    //validBits += (1L << 22);
    //unsigned long offset = validBits >> (sizeof(unsigned int)*8 - idx.nbits);
    offset += (idx.nbits == baseBits);

    int pe = (basePE + offset - 1) % numPes;

    return pe;
  }
};

#include "Main.def.h"
