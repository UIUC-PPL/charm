/**
 \defgroup CkLdb  Charm++ Load Balancing Framework 
*/
/*@{*/

#ifndef BASELB_H
#define BASELB_H

#include "LBManager.h"

#define PER_MESSAGE_SEND_OVERHEAD_DEFAULT   3.5e-5
#define PER_BYTE_SEND_OVERHEAD_DEFAULT      8.5e-9
#define PER_MESSAGE_RECV_OVERHEAD  	    0.0
#define PER_BYTE_RECV_OVERHEAD      	    0.0

/// What a decision record asks of its object. A Move is a migration from
/// from_pe to to_pe. A Clone asks the object on from_pe to make a copy of
/// itself on to_pe with replica rank `rank`; a Retire asks the copy on
/// from_pe to go away. Both apply only to objects whose application declared
/// them replicable (LDObjData::replicaGroup) and are executed by the
/// application through CkMigratable::ckClone / ckRetire; the balancer never
/// counts them as arrivals.
// A notice goes to every other copy of the group a clone or retire changes, so all copies act in the
// same step (the application partitions its work by copy count).
enum LBDecisionKind : unsigned char { LB_DECISION_MOVE = 0, LB_DECISION_CLONE = 1, LB_DECISION_RETIRE = 2,
                                      LB_DECISION_CLONE_NOTICE = 3, LB_DECISION_RETIRE_NOTICE = 4 };

/// A replication decision a strategy adds beside its to_proc assignment
/// (LDStats::clones / retires): obj is the stats index of the copy it
/// addresses -- the source of a clone, the copy a retire removes.
struct LBAction {
  int obj;
  int fromPe;
  int toPe;   // the clone's destination; unused by a retire
  int rank;   // the clone's replica rank; unused by a retire
};

/// Base class for all LB strategies.
/**
  BaseLB is the base class for all LB strategy class.
  it does some tracking about how many lb strategies are created.
  it also defines some common functions.
*/
class BaseLB: public CBase_BaseLB
{
protected:
  int  seqno;
  const char *lbname;
  LBManager *lbmgr;
  int  startLbFnHdl;

  // LB timing instrumentation, used by DiffusionLB to separate strategy time from
  // total LB time (migration included).
  static double totalLBTime;
  static double lbStartTime;
  static double totalLBStrategyTime;
  static int totalPhases;

  static void startLBTiming() { lbStartTime = CmiWallTimer(); }
  static void endLBStrategyTiming() { totalLBStrategyTime += CmiWallTimer() - lbStartTime; }
  static void endLBTiming() {
    totalLBTime += CmiWallTimer() - lbStartTime;
    totalPhases += 1;
  }
private:
  void initLB(const CkLBOptions &);
public:
  struct ProcStats {		// per processor data
    int n_objs;			// number of objects on the processor
    double pe_speed;		// processor frequency
#if defined(TEMP_LDB)
	float pe_temp;
#endif

    /// total time (total_walltime) = idletime + overhead (bg_walltime)
    ///                             + object load (obj_walltime)
    /// walltime and cputime may be different on shared compute nodes
    /// it is advisable to use walltime in most cases
    double total_walltime;
    /// time for which the processor is sitting idle
    double idletime;
    /// bg_walltime called background load (overhead in ckgraph.h) is a
    /// derived quantity: total_walltime - idletime - object load (obj_walltime)
    double bg_walltime;
#if CMK_LB_CPUTIMER
    double total_cputime;
    double bg_cputime;
#endif
    // double utilization;
    int pe;			// processor id
    bool available;
#if CMK_GPU
    // Device memory as the memory contract reads it (hapiLBDeviceMemory):
    // free device bytes; this process's free pool bytes on the device (or the
    // +gpulbbuffer region's, without the pool); the pool's arena quantum (0
    // without it); the pool's capacity on the device, free or not, which the
    // contract's reserve is measured against (0: not reported; format 7); and
    // the IPC event slots a batch may use from this PE.
    size_t gpu_mem_remaining;
    size_t pool_buff_mem_remaining;
    size_t gpu_pool_arena_bytes;
    size_t gpu_pool_capacity_bytes;
    int gpu_ipc_slots;
    uint64_t gpu_device_id;		// GPU device this PE is mapped to (-1 = no GPU)
    int gpu_total_sms;			// Number of SMs on that GPU (0 if unknown)
    // Hardware identity and rate prior for that GPU. Empty (typeId 0) unless
    // +LBGpuScaling is on. A destination-aware strategy needs this to know what
    // kind of device a candidate PE actually has.
    GpuDeviceDescriptor gpu_descriptor;
#endif
    ProcStats(): n_objs(0), pe_speed(1), total_walltime(0.0), idletime(0.0),
#if CMK_LB_CPUTIMER
		 total_cputime(0.0), bg_cputime(0.0),
#endif
#if CMK_GPU
	   	 gpu_device_id((uint64_t)-1), gpu_total_sms(0),
		 gpu_mem_remaining(0), pool_buff_mem_remaining(0),
		 gpu_pool_arena_bytes(0), gpu_pool_capacity_bytes(0), gpu_ipc_slots(0),
#endif
	   	 bg_walltime(0.0), pe(-1), available(true) {}
    inline void clearBgLoad() {
      idletime = bg_walltime = 
#if CMK_LB_CPUTIMER
      bg_cputime = 
#endif
      0.0;
    }
    inline void pup(PUP::er &p) {
      p|total_walltime;
      p|idletime;
      p|bg_walltime;
#if CMK_LB_CPUTIMER
      p|total_cputime;
      p|bg_cputime;
#endif
      p|pe_speed;
      if (_lb_args.lbversion() < 1 && p.isUnpacking()) {
         double dummy;  p|dummy;    // for old format with utilization
      }
      p|available; p|n_objs;
      if (_lb_args.lbversion()>=2) p|pe;
#if CMK_GPU
      p|gpu_mem_remaining;
      p|pool_buff_mem_remaining;
      p|gpu_device_id;
      p|gpu_total_sms;
      if (_lb_args.lbversion() > 3) p|gpu_descriptor;
      if (_lb_args.lbversion() > 4) { p|gpu_pool_arena_bytes; p|gpu_ipc_slots; }
      if (_lb_args.lbversion() > 6) p|gpu_pool_capacity_bytes;
#endif
    }
  };

  /** Passed to the virtual functions Strategy(...) and work(...) */
  struct LDStats {
    std::vector<ProcStats> procs;		// processor statistics

    int n_migrateobjs;		// total number of migratable objects
    int n_nodes;			// total number of nodes (differs from procs in SMP)
    std::vector<LDObjData> objData;	// LDObjData and LDCommData defined in lbdb.h
    std::vector<int> from_proc;	// current pe an object is on
    std::vector<int> to_proc;		// new pe you want the object to be on
    // Replication decisions beside the assignment above (see LBAction). Only
    // a strategy that understands replica groups fills them; createMigrateMsg
    // turns them into Clone / Retire records after the moves.
    std::vector<LBAction> clones;
    std::vector<LBAction> retires;
    std::vector<LBAction> cloneNotices;    // to the other copies: obj = the copy told, toPe/rank = the clone's
    std::vector<LBAction> retireNotices;   // to the other copies: obj = the copy told, rank = the retired copy's

    std::vector<LDCommData> commData;	// communication data - edge list representation
				// of the communication between objects

    std::vector<int> objHash;		// this a map from the hash for the 4 integer
				// LDObjId to the index in the vector "objData"
    int  hashSize;

    int complete_flag;		// if this ocg is complete, eg in HybridLB,
    // this LDStats may not be complete

    int is_prev_lb_refine;
    double after_lb_max;
    double after_lb_avg;

    size_t nprocs() const { return procs.size(); }
    void setNprocs(size_t count) { procs.resize(count); }
    LDStats(int npes = CkNumPes(), int complete_flag = 1);

    void assign(int oid, int pe) { CmiAssert(procs[pe].available); to_proc[oid] = pe; }
    /// build hash table
    void makeCommHash();
    void deleteCommHash();
    /// given an LDObjKey, returns the index in the objData vector
    /// this index changes every time one does load balancing even within a run
    int getHash(const LDObjKey &);
    int getHash(const CmiUInt8 &oid, const LDOMid &mid);
    int getSendHash(LDCommData &cData);
    int getRecvHash(LDCommData &cData);
    void clearCommHash();
    void clear() {
      n_migrateobjs = 0;
      objData.clear();
      commData.clear();
      from_proc.clear();
      to_proc.clear();
      clones.clear();
      retires.clear();
      cloneNotices.clear();
      retireNotices.clear();
      deleteCommHash();
    }
    void clearBgLoad() {
      for (auto& proc : procs) proc.clearBgLoad();
    }
    void computeNonlocalComm(int &nmsgs, int &nbytes);
    double computeAverageLoad();
    void normalize_speed();
    void print();
    // edit functions
    void removeObject(int obj);
    void pup(PUP::er &p);
    int useMem();
  };

  BaseLB(const CkLBOptions &opt)  { initLB(opt); }
  BaseLB(CkMigrateMessage *m):CBase_BaseLB(m) {
    lbmgr = CProxy_LBManager(_lbmgr).ckLocalBranch();
  }
  virtual ~BaseLB() {}

  inline const char *lbName() { return lbname; }
  inline int step() { return lbmgr->step(); }
  virtual void turnOff();
  virtual void turnOn();
  virtual int  useMem()  { return 0; }
  virtual void pup(PUP::er &p);
  virtual void flushStates();
//  virtual void AtSync(void) {  CmiAbort("AtSync not implemented"); } // Everything is at the PE barrier
  virtual void InvokeLB(void) { CmiAbort("InvokeLB not implemented"); }
  virtual void Migrated(int waitBarrier=1) { CmiAbort("Migrated not implemented"); }

  CkGroupID getGroupID() {return thisgroup;}
};

/// migration decision for an obj.
struct MigrateInfo {
    int index;   // object index in objData array
    LDObjHandle obj;
    int from_pe;
    int to_pe;
    int async_arrival;	    // if an object is available for immediate migrate
    unsigned char kind;     // LBDecisionKind
    int rank;               // replica rank of the copy a Clone makes
    MigrateInfo():  async_arrival(0), kind(LB_DECISION_MOVE), rank(0) {
    }
    bool isMove() const { return kind == LB_DECISION_MOVE; }

    void pup(PUP::er &p) {
      p | index;
      p | obj;
      p | from_pe;
      p | to_pe;
      p | async_arrival;
      p | kind;
      p | rank;
    }
};

struct MigrateDecision {
  LDObjIndex dbIndex;
  int fromPe;
  int toPe;
  // Whether the load balancer holds its migration barrier for this arrival.
  // The destination counts the two kinds separately, so a decision message
  // that leaves it out cannot tell the destination what to expect.
  bool asyncArrival;
  unsigned char kind;     // LBDecisionKind
  int rank;
  // The object's id, so a Clone / Retire can be checked against the slot
  // dbIndex names on its PE (slots are reused when objects die).
  CmiUInt8 id;
  bool isMove() const { return kind == LB_DECISION_MOVE; }

  MigrateDecision &operator=(const MigrateInfo &mInfo) {
    dbIndex = mInfo.obj.handle;
    fromPe = mInfo.from_pe;
    toPe = mInfo.to_pe;
    asyncArrival = mInfo.async_arrival;
    kind = mInfo.kind;
    rank = mInfo.rank;
    id = mInfo.obj.id;

    return *this;
  }

};

class LBScatterMsg : public CMessage_LBScatterMsg {
public:
  int numMigrates;
  int firstPeInSpan;
  int lastPeInSpan;
  // Batched migration execution (the memory contract's I-batch): which batch
  // of the step's decision this message carries, out of how many.
  int lb_batch;
  int lb_nbatches;
  // The PE that runs the next centralized step, carried so the peer decision
  // path needs nothing from the broadcast it replaces.
  int next_lb;
  // Decisions of the whole step, job-wide (LBManager::LastRoundDecisions).
  int totalDecisions;
  // Arrivals the LB waits for, and arrivals it does not, per PE in the span.
  int *numMigratesPerPe;
  int *numFutureMigratesPerPe;
  MigrateDecision *moves;

  LBScatterMsg(int firstPe, int lastPe) {
    numMigrates = 0;
    firstPeInSpan = firstPe;
    lastPeInSpan = lastPe;
    lb_batch = 0;
    lb_nbatches = 1;
    next_lb = 0;
  }
};



/**
  message contains the migration decision from LB strategies.
*/
class LBMigrateMsg : public CMessage_LBMigrateMsg {
public:
  int level;			// which level in hierarchy, used in hybridLB

  int n_moves;			// number of moves
  MigrateInfo* moves;

  char * avail_vector;		// processor bit vector
  int next_lb;			// next load balancer

  double * expectedLoad;	// expected load for future

  // Batched migration execution (the memory contract's I-batch): which batch
  // of the step's decision this message carries, out of how many.
  int lb_batch;
  int lb_nbatches;

public:
  LBMigrateMsg(): level(0), n_moves(0), next_lb(0), lb_batch(0), lb_nbatches(1) {}
  void pup(PUP::er &p) {
    int i;
    p | level;
    p | n_moves;
    p | lb_batch;
    p | lb_nbatches;
    // Warning: it relies on the fact that the message has been allocated already
    // with the correct number of moves!
    p | next_lb;
    int numPes = CkNumPes();
    p | numPes;
    CkAssert(numPes == CkNumPes());
    for (i=0; i<n_moves; ++i) p | moves[i];
    p(avail_vector, numPes);
    for (i=0; i<numPes; ++i) p | expectedLoad[i];
  }
};

struct VectorMigrateInfo {  
    int from_pe;
    int to_pe;
    double load;
    int async_arrival;	    // if an object is available for immediate migrate
    VectorMigrateInfo():  async_arrival(0) {}
};

class LBVectorMigrateMsg : public CMessage_LBVectorMigrateMsg {
public:
  int level;			// which level in hierarchy, used in hybridLB

  int n_moves;			// number of moves
  VectorMigrateInfo* moves;

public:
  LBVectorMigrateMsg(): level(0), n_moves(0) {}
};

// Deprecated, left around to support external custom load balancers
// Internal load balancers should define lbinit directly and use LBRegisterBalancer<T>
//
// For a FooLB, the following macro defines these functions for each LB:
// CreateFooLB():        create BOC and register with LBManager with a
//                       sequence ticket,
// AllocateFooLB():      allocate the class instead of a BOC
// static void lbinit(): an init call for charm module registration
#if CMK_LBDB_ON

#  define CreateLBFunc_Def(x, str)                                                       \
    CMK_DEPRECATED_MSG("Use LBRegisterBalancer() instead of CreateLBFunc_Def()")         \
    void Create##x(const CkLBOptions& opts) { CProxy_##x::ckNew(opts); }                 \
    CMK_DEPRECATED_MSG("Use LBRegisterBalancer() instead of CreateLBFunc_Def()")         \
    BaseLB* Allocate##x(void) { return new x(static_cast<CkMigrateMessage*>(nullptr)); } \
    CMK_DEPRECATED_MSG("Use LBRegisterBalancer() instead of CreateLBFunc_Def()")         \
    static void lbinit(void) { LBRegisterBalancer(#x, Create##x, Allocate##x, str); }

#else /* CMK_LBDB_ON */

#  define CreateLBFunc_Def(x, str)                \
    void Create##x(void) {}                       \
    BaseLB* Allocate##x(void) { return nullptr; } \
    static void lbinit(void) {}

#endif /* CMK_LBDB_ON */

#endif

/*@}*/
