#ifndef CKMIGRATABLE_H
#define CKMIGRATABLE_H

class CkMigratable : public Chare {
protected:
private:
  int thisChareType;//My chare type
  int atsync_iteration;
  double prev_load;
  enum state : uint8_t {
    OFF,
    ON,
    PAUSE,
    DECIDED,
    LOAD_BALANCE
  } local_state;
  bool can_reset;
  // Sequence number of the last load balancing step this chare joined. It rises
  // to CkpvAccess(_lbStepRequested) in AtSyncStart; comparing the two is the
  // whole cost of an AtSyncStart that has no step to start.
  int lbStepSeen;
  // Count of AtSyncStart() calls this chare has made. The application calls
  // AtSyncStart on a fixed cadence, so every chare's k-th call is the same
  // application iteration -- which makes this counter a clock all chares agree
  // on without exchanging iteration numbers. The load balancing step is agreed
  // as a value of this counter. Deliberately not atsync_iteration: that one
  // advances in AtSyncSample(), whose cadence is kept sparse because CUPTI
  // sampling is expensive, and is therefore far too coarse to schedule on.
  int lbIterNo;
  // Whether this chare has adjusted the sample-stream counts of the PE it is
  // currently on. Deliberately not pupped: a chare arriving by migration starts
  // false and so performs the adjustment for its new PE, which the
  // atsync_iteration == -1 path only ever did for a freshly created chare.
  // Which PE's MetaBalancer sample stream this element has credited, or -1 for
  // none. A bool cannot answer the question that matters -- "have I credited
  // the stream I am contributing to now?" -- because an element can reach a new
  // PE without being reconstructed, and then carries a stale "yes" from the PE
  // it left.
  int metaLBStreamJoinedPe;
  // Set when AtSyncStart stopped this chare at the tentative count because the
  // step's count had not been agreed yet. Released by recvLBPeriod.
  bool lbStepBlocked;
  // +LBAsync state. lbStepPending is set when AtSyncStart joins a step and
  // cleared when that step resumes clients here; waitParked records that
  // AtSyncWait found the step still running and stopped the element. lbWaitEpoch
  // is the LBManager resume epoch the element is waiting past, which lets an
  // element migrated by the very step it waits on notice, on arrival, that its
  // destination has already resumed. All three are pupped across migration.
  bool lbStepPending;
  // The measurement window stays closed until AtSyncWait even if the
  // balancing step has already finished. Preserve it across every migration.
  bool lbMeasurementClosed;
  bool waitParked;
  int lbWaitEpoch;
protected:
  bool usesAtSync;//You must set this in the constructor to use AtSync().
  bool usesAutoMeasure; //You must set this to use auto lb instrumentation.
  bool barrierRegistered;//True iff barrier handle below is set

private: //Load balancer state:
  LDBarrierClient ldBarrierHandle;//Transient (not migrated)
  LDBarrierReceiver ldBarrierRecvHandle;//Transient (not migrated)
public:
  CkArrayIndex thisIndexMax;

private:
  void commonInit(void);
public:
  CkMigratable(void);
  CkMigratable(CkMigrateMessage *m);
  virtual ~CkMigratable();
  virtual void pup(PUP::er &p);
  virtual void CkAddThreadListeners(CthThread tid, void *msg);

  virtual int ckGetChareType(void) const;// {return thisChareType;}
  const CkArrayIndex &ckGetArrayIndex(void) const {return myRec->getIndex();}
  CmiUInt8 ckGetID(void) const { return myRec->getID(); }

#if CMK_LBDB_ON  //For load balancing:
  inline LBManager *getLBMgr(void) const {return myRec->getLBMgr();}
  inline MetaBalancer *getMetaBalancer(void) const {return myRec->getMetaBalancer();}
#endif

  //Initiate a migration to the given processor
  inline void ckMigrate(int toPe) {myRec->migrateMe(toPe);}
  
  /// Called by the system just before and after migration to another processor:
  virtual void ckAboutToMigrate(void); /*default is empty*/
  virtual void ckJustMigrated(void); /*default is empty*/

  /// The balancer's replication decisions, for an element that declared itself
  /// replicable (setLBReplica): make a copy of this element on toPe with copy
  /// index `rank`; retire this copy. Delivered as soon as the decision reaches
  /// this PE, which may be any time in the element's step: the element records
  /// it and acts at a point of its own choosing, and it alone moves the state
  /// (the runtime does not pack a copy). The defaults abort for a replicable
  /// element that did not override them and do nothing for any other element.
  virtual void ckClone(int toPe, int rank);
  virtual void ckRetire(void);
  /// The balancer makes a copy of (or retires) another copy of this element's group: `rank` is the
  /// copy made on `toPe` (or the copy retired). Delivered before the round releases the element,
  /// so every copy can act in the same step. Default: nothing.
  virtual void ckCloneNotice(int toPe, int rank) { (void)toPe; (void)rank; }
  virtual void ckRetireNotice(int rank) { (void)rank; }
  // The location manager's path to ckClone (CkLocRec::staticClone); not for
  // applications.
  void ckCloneFromDecision(void* args);
  void ckCloneNoticeFromDecision(void* args);
  void ckRetireNoticeFromDecision(void* args);

  void recvLBPeriod(void *data);
  void metaLBCallLB();
  void clearMetaLBData(void);

  //used for out-of-core emulation
  virtual void ckJustRestored(void); /*default is empty*/

  /// Delete this object
  virtual void ckDestroy(void);

  /// Execute the given entry method.  Returns false if the element 
  /// deleted itself or migrated away during execution.
  // TODO: Why does this have a different signature than other invoke calls?
  inline bool ckInvokeEntry(int epIdx,void *msg,bool doFree) 
	  {return myRec->invokeEntry(this,msg,epIdx,doFree);}

protected:
  /// A more verbose form of abort
  CMK_NORETURN
#if defined __GNUC__ || defined __clang__
  __attribute__ ((format (printf, 2, 3)))
#endif
  virtual void CkAbort(const char *format, ...) const;

public:
  virtual void ResumeFromSync(void);
  virtual void UserSetLBLoad(void);  /// user define this when setLBLoad is true
  void setObjTime(double cputime);
  // Register this object's position for geometric load balancers (DiffusionLB's
  // centroid metric). Optional: left uncalled, the position stays empty and
  // centroid-based balancing has nothing to work with.
  void setObjPosition(const std::vector<LBRealType>& pos);
  double getObjTime();
  void setObjGPUTime(double cputime);
  double getObjGPUTime();
  // The host and GPU loads the LAST balancing step read for this element,
  // i.e. the figures the strategy decided from. getObjTime / getObjGPUTime
  // return the window still being measured, which the step's completion
  // clears, so from ResumeFromSync they read 0 -- these do not. An element
  // that migrated in the step has a fresh record on its new PE and reads 0.
  double getObjLastTime();
  double getObjLastGPUTime();
#if CMK_LB_USER_DATA
  void *getObjUserData(int idx);
#endif

#if CMK_LBDB_ON  //For load balancing:
  void AtSync(int waitForMigration=1);

  // MetaBalancer's AtSync split in two, so that collecting load and starting a
  // step can run at different rates. Call AtSyncSample() every few iterations
  // (it flushes GPU counters and feeds a background reduction) and
  // AtSyncStart() every iteration (it costs one integer compare unless a step
  // has actually been requested, so a step begins one iteration after the
  // imbalance that caused it was seen).
  void AtSyncSample();
  // What an AtSyncStart() call did.
  //   Continue -- no step at this count; carry on iterating.
  //   Started  -- this call joined the step.
  //   Blocked  -- the step's count is still being agreed and this chare has
  //               reached the tentative count. It is stopped: do not iterate,
  //               do not call AtSyncStart again. It will be resumed (through
  //               ResumeFromSync) or joined for you once the count is agreed.
  enum class AtSyncStatus { Continue, Started, Blocked };
  AtSyncStatus AtSyncStart(int waitForMigration=1);

  // The other half of the split. Under +LBAsync this blocks the element until
  // every migration the in-flight step planned, on every PE, has completed, and
  // then calls ResumeFromSync() exactly once; with no step in flight it calls
  // ResumeFromSync() inline. Without +LBAsync it does nothing, because the
  // resume already arrives from the AtSync barrier -- so an application written
  // to the async pattern runs correctly either way.
  void AtSyncWait();

  // Remove this element from the PE-level fast-delivery table, so messages for
  // it fall through to the location manager and are forwarded. Needed by any
  // migration path that does not destroy the element on the source: the
  // destructor is what normally keeps that table honest.
  virtual void ckStopFastDelivery() {}
private:
  void lbJoinStep(int waitForMigration);
  void lbSetMeasurementClosed(bool closed);
public:

  // True when the next AtSyncStart() will actually start a step, so an
  // application can do its pre-migration work -- draining device streams, say
  // -- only on the iterations where that work is needed.
  bool AtSyncPending() const;

  // True while this chare owes AtSyncWait() to a step it has already joined --
  // including a step it was joined into by the runtime, after blocking at the
  // tentative count. An application cannot track that itself, because that join
  // does not happen inside its AtSyncStart() call.
  bool AtSyncStepInFlight() const { return lbStepPending; }

  // Release an element that parked in AtSyncWait() and was then migrated by the
  // step it was waiting on, landing after its destination had already resumed.
  // Called from the migration arrival paths in CkLocMgr.
  void lbCheckWaitRelease();

  int MigrateToPe()  { return myRec->MigrateToPe(); }

private:
  void ResumeFromSyncHelper();
  void recordLBSizes(bool forStep);
  void sampleMetaLBLoad();
  double objRebuildSeconds = 0.0;
  // The replication declaration (setLBReplica), kept on the element so it
  // travels with it and is declared again to the database of every PE it
  // reaches. lbReplicaGroup < 0 means not replicable, the default.
  int lbReplicaGroup = -1;
  int lbReplicaRank = 0;
  int lbReplicaCount = 1;
  int lbReplicaMax = 1;
  size_t lbReplicaExtraBytes = 0;
  double lbReplicaCostNode = 0.0;
  double lbReplicaCostRemote = 0.0;
public:

  void ReadyMigrate(bool ready);
  void ckFinishConstruction(int epoch = -1);
  void setMigratable(int migratable);
  void setPupSize(size_t obj_pup_size);
  void setGPUPupSize(size_t obj_gpu_pup_size);
  // Seconds this element spends on arrival rebuilding device state it leaves
  // out of its pup (a neighbour list, a cell table) instead of shipping it.
  // The balancers price every move of it with this added, since the bytes
  // it ships do not show it. Travels with the element.
  void setObjRebuildCost(double seconds) { objRebuildSeconds = seconds; }
  double getObjRebuildCost() const { return objRebuildSeconds; }
  // Declare this element one of `count` copies of logical object `group`
  // (its copy index `rank`), of which the application can keep up to
  // `maxCount` consistent; `extraBytes` is what one copy holds on its device
  // beyond the element's own footprint. A balancer that understands groups
  // (ShedLB) may then ask for a copy with ckClone or take one back with
  // ckRetire. Call it before every AtSync, as with setObjGPUTime; an element
  // that never calls it is never replicated. `costNode` / `costRemote`: the
  // load one more copy adds to each copy per balancing interval for its
  // exchange with that peer, on the same node or across nodes (0 when the
  // copies exchange nothing); the balancer prices a copy with them.
  void setLBReplica(int group, int rank, int count, int maxCount, size_t extraBytes,
                    double costNode = 0.0, double costRemote = 0.0);
  bool lbReplicable() const { return lbReplicaGroup >= 0 && lbReplicaMax > 1; }
#else
  void setObjRebuildCost(double seconds) { (void)seconds; }
  double getObjRebuildCost() const { return 0.0; }
  void setLBReplica(int, int, int, int, size_t, double = 0.0, double = 0.0) {}
  bool lbReplicable() const { return false; }
  void AtSync(int waitForMigration=1) { ResumeFromSync();}
  enum class AtSyncStatus { Continue, Started, Blocked };
  AtSyncStatus AtSyncStart(int waitForMigration=1) { ResumeFromSync(); return AtSyncStatus::Continue; }
  void AtSyncSample() { }
  void AtSyncStart(int waitForMigration=1) { ResumeFromSync();}
  void AtSyncWait() { }
  bool AtSyncPending() const { return false; }
  void lbCheckWaitRelease() { }
  void setMigratable(int migratable)  { }
  void setPupSize(size_t obj_pup_size) { }
  void setGPUPupSize(size_t obj_gpu_pup_size) { }
public:
  void ckFinishConstruction(int epoch) { }
#endif

public:
  // Intra-process (same CmiNode / SMP mode) migration fast path.
  // Called on the source PE just before ownership is transferred: returns
  // the current AtSync barrier epoch and detaches from the source PE's
  // sync barrier. Returns -1 if no epoch tracking is needed.
#if CMK_LBDB_ON
  /// Detach a chare from the AtSync barrier while it is held alive on the
  /// source PE waiting for a migration ack. Without this, the held chare would
  /// keep iterating in parallel with the migrated copy on the destination.
  /// ~CkMigratable sees barrierRegistered already false and skips the
  /// duplicate removeClient call.
  void ckSuspendBarrierForDeferredDestroy();
#endif

  virtual int ckPrepareIntraProcessMigrate();
  // Called on the destination PE after ownership is transferred: rebinds
  // the chare to its new CkLocRec and re-registers with the destination
  // PE's sync barrier at the given epoch.
  virtual void ckFinalizeIntraProcessMigrate(CkLocRec* newRec, int epoch);

#if CMK_OUT_OF_CORE
private:
  friend class CkLocMgr;
  friend int CkArrayPrefetch_msg2ObjId(void *msg);
  friend void CkArrayPrefetch_writeToSwap(FILE *swapfile,void *objptr);
  friend void CkArrayPrefetch_readFromSwap(FILE *swapfile,void *objptr);
  int prefetchObjID; //From CooRegisterObject
  bool isInCore; //If true, the object is present in memory
#endif
};

#endif // CKMIGRATABLE_H
