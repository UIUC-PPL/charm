#include <string>
#include <cstring>
#include <cmath>
#include "time.h"
#include "Main.h"
#include "Cell.h"
#include "Compute.h"

/* readonly */ CProxy_Main mainProxy;
/* readonly */ CProxy_Cell cellArray;
/* readonly */ CProxy_Compute computeArray;

/* readonly */ int cellArrayDimX;
/* readonly */ int cellArrayDimY;
/* readonly */ int cellArrayDimZ;
/* readonly */ int finalStepCount; 
/* readonly */ int firstLdbStep; 
/* readonly */ int ldbPeriod;
/* readonly */ int checkptFreq; 
/* readonly */ int checkptStrategy;
/* readonly */ std::string logs;
/* readonly */ int bFactor;
/* readonly */ int placeMode;
/* readonly */ int hopMode;
/* readonly */ int hopEvery;
/* readonly */ int cellsMigratable;

static const double kWatchdogSecs = 60.0;
static void watchdogFire(void *, double) { mainProxy.checkProgress(); }

static int parseMode(const char *s, const char *const *names, int count, const char *opt) {
  for (int i = 0; i < count; i++)
    if (!strcmp(s, names[i])) return i;
  CkAbort("leanmd: unknown %s mode %s", opt, s);
  return 0;
}

// Entry point of Charm++ application
Main::Main(CkArgMsg* m) {
  CkPrintf("\nLENNARD JONES MOLECULAR DYNAMICS START UP ...\n");

  //set variable values to a default set
  cellArrayDimX = CELLARRAY_DIM_X;
  cellArrayDimY = CELLARRAY_DIM_Y;
  cellArrayDimZ = CELLARRAY_DIM_Z;
  finalStepCount = DEFAULT_FINALSTEPCOUNT;
  firstLdbStep = DEFAULT_FIRST_LDB;
  ldbPeriod = DEFAULT_LDB_PERIOD;
  checkptFreq = DEFAULT_FT_PERIOD;

  mainProxy = thisProxy;

  bFactor = 4;
  placeMode = PLACE_DEFAULT;
  hopMode = HOP_NONE;
  hopEvery = 5;
  cellsMigratable = 0;

  // Stress-test flags come out first; the positional arguments follow.
  static const char *const placeNames[] = {"default", "away", "onepe", "random"};
  static const char *const hopNames[] = {"none", "between", "inflight", "twice", "computes"};
  {
    int out = 1;
    for (int i = 1; i < m->argc; i++) {
      if (!strcmp(m->argv[i], "-place") && i + 1 < m->argc)
        placeMode = parseMode(m->argv[++i], placeNames, 4, "-place");
      else if (!strcmp(m->argv[i], "-hop") && i + 1 < m->argc)
        hopMode = parseMode(m->argv[++i], hopNames, 5, "-hop");
      else if (!strcmp(m->argv[i], "-hopevery") && i + 1 < m->argc)
        hopEvery = atoi(m->argv[++i]);
      else if (!strcmp(m->argv[i], "-migratable-cells"))
        cellsMigratable = 1;
      else
        m->argv[out++] = m->argv[i];
    }
    m->argc = out;
  }
  if (hopEvery < 1) CkAbort("leanmd: -hopevery must be at least 1");
  if (placeMode != PLACE_DEFAULT || hopMode != HOP_NONE || cellsMigratable)
    CkPrintf("Stress options: -place %s -hop %s -hopevery %d%s\n", placeNames[placeMode],
             hopNames[hopMode], hopEvery, cellsMigratable ? " -migratable-cells" : "");

  int numPes = CkNumPes();
  int currPe = -1, pe;
  int cur_arg = 1;

  CkPrintf("\nInput Parameters...\n");

  //read user parameters
  //number of cells in each dimension
  if (m->argc > cur_arg) {
    cellArrayDimX=atoi(m->argv[cur_arg++]);
    cellArrayDimY=atoi(m->argv[cur_arg++]);
    cellArrayDimZ=atoi(m->argv[cur_arg++]);
    CkPrintf("Cell Array Dimension X:%d Y:%d Z:%d of size %d %d %d\n",cellArrayDimX,cellArrayDimY,cellArrayDimZ,CELL_SIZE_X,CELL_SIZE_Y,CELL_SIZE_Z);
  }

  //number of steps in simulation
  if (m->argc > cur_arg) {
    finalStepCount=atoi(m->argv[cur_arg++]);
    CkPrintf("Final Step Count:%d\n",finalStepCount);
  }

  //step after which load balancing starts
  if (m->argc > cur_arg) {
    firstLdbStep=atoi(m->argv[cur_arg++]);
    CkPrintf("First LB Step:%d\n",firstLdbStep);
  }

  //periodicity of load balancing
  if (m->argc > cur_arg) {
    ldbPeriod=atoi(m->argv[cur_arg++]);
    CkPrintf("LB Period:%d\n",ldbPeriod);
  }

  //periodicity of checkpointing
  if (m->argc > cur_arg) {
    checkptFreq=atoi(m->argv[cur_arg++]);
    CkPrintf("FT Period:%d\n",checkptFreq);
  }

  checkptStrategy = 1;
  //choose the checkpointing strategy use in disk checkpointing
  if (m->argc > cur_arg) {
  	checkptStrategy = 0;
    logs = m->argv[cur_arg];
  }

  CProxy_CellMap cellMap = CProxy_CellMap::ckNew(cellArrayDimX, 
    cellArrayDimY, cellArrayDimZ);
  CkArrayOptions opts(cellArrayDimX, cellArrayDimY, cellArrayDimZ);
  opts.setMap(cellMap);
  //create a 3D Patch array (with a uniform distribution)
  cellArray = CProxy_Cell::ckNew(opts);

  //create an empty 6D computer array to be filled in by Cells
  computeArray = CProxy_Compute::ckNew();

  cellArray.createComputes();
  lastProgress = CkWallTimer();
  CcdCallFnAfter(watchdogFire, NULL, 2000);
  CkPrintf("\nCells: %d X %d X %d .... created\n", cellArrayDimX, 
    cellArrayDimY, cellArrayDimZ);

  delete m;
}

//constructor for chare object migration
Main::Main(CkMigrateMessage* msg): CBase_Main(msg) { 
}

//pup routine incase the main chare moves, pack important information
void Main::pup(PUP::er &p) {
  CBase_Main::pup(p);
  __sdag_pup(p);
}

// The forces each compute adds to its two cells are equal and opposite, so the
// forces summed over all cells vanish each step; a lost, doubled or misrouted
// section contribution shows up here. v = {sum fx, sum fy, sum fz, sum |f|}.
void Main::checkForceSum(double *v, int n) {
  lastProgress = CkWallTimer();
  if (n != 4) CkAbort("leanmd: force sum has %d values, expected 4", n);
  double tol = 1e-9 * v[3] + 1e-300;
  if (fabs(v[0]) > tol || fabs(v[1]) > tol || fabs(v[2]) > tol)
    CkAbort("leanmd: forces over all cells sum to (%e, %e, %e), magnitude sum %e",
            v[0], v[1], v[2], v[3]);
}

void Main::checkProgress() {
  double idle = CkWallTimer() - lastProgress;
  // One tick before the abort, every cell prints its state.
  if (idle > kWatchdogSecs - 3 && idle <= kWatchdogSecs - 1) cellArray.report();
  if (idle > kWatchdogSecs)
    CkAbort("leanmd: no step completed for %.0f s (%d of %d steps checked)",
            kWatchdogSecs, forceChecks, finalStepCount);
  CcdCallFnAfter(watchdogFire, NULL, 2000);
}

#include "leanmd.def.h"
