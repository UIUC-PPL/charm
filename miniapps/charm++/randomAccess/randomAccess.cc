#include "NDMeshStreamer.h"

typedef CmiUInt8 dtype;
#include "randomAccess.decl.h"
#include "TopoManager.h"

#define POLY 0x0000000000000007ULL
#define PERIOD 1317624576693539401LL

int                             N;                      // log_2 of the local table size
CmiInt8                         localTableSize;         // The local table size
CProxy_TestDriver               driverProxy;            // Handle to the test driver (chare)
CProxy_GroupMeshStreamer<dtype, Updater,
                         SimpleMeshRouter> aggregator;  // Handle to the communication library (group)
const int                       numMsgsBuffered = 1024; // Max number of keys buffered by communication library

CmiUInt8 HPCC_starts(CmiInt8 n);

class TestDriver : public CBase_TestDriver {
private:
    CProxy_Updater  updater_group;
    double starttime;
    CmiInt8 tableSize;

public:
    TestDriver(CkArgMsg* args) {
        N = atoi(args->argv[1]);
        localTableSize = 1l << N;
        tableSize = localTableSize * CkNumPes();

        CkPrintf("Global table size   = 2^%d * %d = %lld words\n", N, CkNumPes(), tableSize);
        CkPrintf("Number of processors = %d\nNumber of updates = %lld\n", CkNumPes(), 4*tableSize);

        driverProxy = thishandle;
        // Create the chares storing and updating the global table
        updater_group   = CProxy_Updater::ckNew();
        // Query charm++ topology interface to obtain network topology information
        TopoManager tmgr;
        int dims[3] = {tmgr.getDimNX() * tmgr.getDimNT(), tmgr.getDimNY(), tmgr.getDimNZ()}; 
        // Instantiate communication library group with a handle to the client (data receiver)
        aggregator = CProxy_GroupMeshStreamer<dtype, Updater, SimpleMeshRouter>::ckNew(numMsgsBuffered, 3, dims, updater_group, 1);

        delete args;
    }

    void start() {
        starttime = CkWallTimer();
        CkCallback startCb(CkIndex_Updater::generateUpdates(), updater_group);
        CkCallback endCb(CkIndex_TestDriver::startVerificationPhase(), thisProxy);          
        // Initialize the communication library, which, upon readiness, will initiate the test via startCb
        aggregator.init(1, startCb, endCb, -1, false);
    }

    void startVerificationPhase() {
        double update_walltime = CkWallTimer() - starttime;
        double gups = 1e-9 * tableSize * 4.0/update_walltime;

        CkPrintf( "CPU time used = %.6f seconds\n", update_walltime );
        CkPrintf( "%.9f Billion(10^9) Updates    per second [GUP/s]\n",  gups);
        CkPrintf( "%.9f Billion(10^9) Updates/PE per second [GUP/s]\n", gups / CkNumPes() );

        // Repeat the update process to verify
        // At the end of the second update phase, check the global table for errors in Updater::checkErrors()
        CkCallback startCb(CkIndex_Updater::generateUpdates(), updater_group);  
        CkCallback endCb(CkIndex_Updater::checkErrors(), updater_group);
        // Initialize the communication library, which, upon readiness, will initiate the verification via startCb
        aggregator.init(1, startCb, endCb, -1, false);
    }
    
    void reportErrors(CmiInt8 globalNumErrors) {
        CkPrintf("Found %lld errors in %lld locations (%s).\n", globalNumErrors, 
                 tableSize, (globalNumErrors <= 0.01*tableSize) ? "passed" : "failed");
        CkExit();
    }
};


// Charm++ Group (i.e. one chare on each PE)
// Each chare owns a portion of the global table and performs updates on its portion
// Each chare also generates random keys and sends them to the appropriate chares
class Updater : public CBase_Updater {
private:
    CmiUInt8 *HPCC_Table;
    CmiUInt8 globalStartmyProc;

public:
    Updater() {
        // Compute table start for this chare
        globalStartmyProc = CkMyPe()* localTableSize  ;
        // Create table
        HPCC_Table = (CmiUInt8*)malloc(sizeof(CmiUInt8) * localTableSize);
        // Initialize
        for(CmiInt8 i=0; i<localTableSize; i++)
            HPCC_Table[i] = i + globalStartmyProc;
        // Contribute to a reduction to signal the end of the setup phase
        contribute(CkCallback(CkReductionTarget(TestDriver, start), driverProxy));
    }

    // Communication library calls this to deliver each randomly generated key
    inline void process(const dtype  &ran) {
        CmiInt8  localOffset = ran & (localTableSize - 1);
        // Apply update
        HPCC_Table[localOffset] ^= ran;
    }

    void generateUpdates() {
        CmiUInt8 ran= HPCC_starts(4* globalStartmyProc);
        // Get a pointer to the local communication library object from its proxy handle
        GroupMeshStreamer<dtype, Updater, SimpleMeshRouter> * localAggregator = aggregator.ckLocalBranch();

        // Generate this chare's share of global updates
        for(CmiInt8 i=0; i< 4 * localTableSize; i++) {
            ran = (ran << 1) ^ ((CmiInt8) ran < 0 ? POLY : 0);
            int tableIndex = (ran >>  N)&(CkNumPes()-1);
            // Submit generated key 'ran' to chare owning that portion of the table (index = tableIndex)
            localAggregator->insertData(ran, tableIndex);
        }

        // Indicate to the communication library that this chare is done sending data
        localAggregator->done();
    }

    void checkErrors() {
        CmiInt8 numErrors = 0;
        // The second verification phase should have returned the table to its initial state
        for (CmiInt8 j=0; j<localTableSize; j++)
            if (HPCC_Table[j] != j + globalStartmyProc)
                numErrors++;
        // Sum the errors observed across the entire system
        contribute(sizeof(CmiInt8), &numErrors, CkReduction::sum_long, CkCallback(CkReductionTarget(TestDriver,reportErrors), driverProxy));
    }
};


/** random number generator */
CmiUInt8 HPCC_starts(CmiInt8 n) {
    int i, j;
    CmiUInt8 m2[64];
    CmiUInt8 temp, ran;
    while (n < 0) n += PERIOD;
    while (n > PERIOD) n -= PERIOD;
    if (n == 0) return 0x1;
    temp = 0x1;
    for (i=0; i<64; i++) {
        m2[i] = temp;
        temp = (temp << 1) ^ ((CmiInt8) temp < 0 ? POLY : 0);
        temp = (temp << 1) ^ ((CmiInt8) temp < 0 ? POLY : 0);
    }
    for (i=62; i>=0; i--)
        if ((n >> i) & 1)
            break;

    ran = 0x2;
    while (i > 0) {
        temp = 0;
        for (j=0; j<64; j++)
            if ((ran >> j) & 1)
                temp ^= m2[j];
        ran = temp;
        i -= 1;
        if ((n >> i) & 1)
            ran = (ran << 1) ^ ((CmiInt8) ran < 0 ? POLY : 0);
    }
    return ran;

}

#include "randomAccess.def.h"
