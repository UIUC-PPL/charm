#ifndef CK_LOC_REC_H
#define CK_LOC_REC_H

class CkArray;//Array manager
class CkLocMgr;//Location manager
class CkMigratable;//Migratable object
struct CkStableSnapshot;//Stable device buffers shipped at the balancer's decision

/**
 * A CkLocRec is our local representation of an array element.
 * The location manager's main hashtable maps array indices to
 * CkLocRec *'s.
 */
class CkLocRec {
private:
  CkLocMgr *myLocMgr;
  CkArrayIndex idx;/// Element's array index
  CmiUInt8 id;
  bool *deletedMarker; /// Set this if we're deleted during processing
  bool running; /// True when inside a startTiming/stopTiming pair
#if CMK_LBDB_ON
  bool  asyncMigrate;  /// if readyMove is inited
  bool  readyMigrate;    /// status whether it is ready to migrate
  bool  enable_measure;
  int  nextPe;              /// next migration dest processor
  CkSyncBarrier* syncBarrier;
  LBManager *lbmgr;
  MetaBalancer *the_metalb;
  LDObjHandle ldHandle;
#endif

public:
  // Device zerocopy sends this element issued whose source buffers the runtime
  // has not finished reading. Zerocopy means the runtime keeps reading the
  // application's buffer after the send call returns -- the completion callback
  // is what says it may be reused -- so migrating inside that window frees and
  // reallocates a buffer a transfer is still sourcing from.
  int outstandingDeviceSends = 0;
  // Destination of a migration deferred because of the above, or -1. The
  // migration is re-driven from noteDeviceSendDone once the count reaches zero.
  int pendingMigrateTo = -1;
  // Step of the balancer move ledger this element's pending move is recorded
  // in, or -1. Set in recvMigrate while a decision loop is recording; consumed
  // by emigrate, which stamps the migration message so the destination knows
  // to ack this PE's LBManager once the element registers there. Survives any
  // deferral in between; never pupped (the message carries it instead).
  int lbLedgerStep = -1;
  // The device payload block this element's size said a staged migration
  // would pack (cached from the last setGPUPupSize), and the part of it counted
  // in this process's pending-pack floor: from the LB ordering the move until
  // emigrate has packed or handed the element over. See ckLandingAdmissible.
  size_t lbGpuPupSize = 0;
  size_t pendingPackBytes = 0;
  // Set while the element is parked in AtSyncWait. A parked element runs no
  // SDAG, so a device receive delivered to it could only sit preprocessed and
  // unconsumed -- pinned to this process -- until the resume; admission
  // control bounces such receives instead (see CkRdmaDeviceIssueRgets).
  bool deviceRecvParked = false;
  // Set when the count reached zero with a move pending and the kick was
  // enqueued. New device receives are held (admission buffer) until the kick
  // runs, so it finds the element still quiet. Without this the kick, queued
  // behind a step's worth of messages, found the next step's arrivals already
  // admitted: 28% of moves went at the first kick, median 2.5 s to move.
  bool migrateKickHeld = false;
  void noteDeviceSendPosted() { outstandingDeviceSends++; }
  void noteDeviceSendDone(bool urgentKick = false);
  // The move window tally (CHARM_LB_MIGSTATS, see MigStats in cklocation.C):
  // when the balancer's decision reached this element, and when emigrate
  // first tried to take it (the stand-down for outstanding sends and channel
  // closings starts there).
  double migDecidedAt = 0.0;
  double migFirstTryAt = 0.0;
  // PEs whose messages this element took lately, from other PEs. When the
  // element leaves they are told its new location directly (CkLocMgr::
  // informSenders), so their next message goes to the new home instead of
  // taking the forward from here and the repair that follows it.
  enum { RECENT_SENDERS = 8 };
  int recentSenders[RECENT_SENDERS];
  int nRecentSenders = 0;
  int recentSendersNext = 0;
  inline void noteSender(int pe) {
    for (int i = 0; i < nRecentSenders; i++) if (recentSenders[i] == pe) return;
    if (nRecentSenders < RECENT_SENDERS) recentSenders[nRecentSenders++] = pe;
    else { recentSenders[recentSendersNext] = pe; recentSendersNext = (recentSendersNext + 1) % RECENT_SENDERS; }
  }
  // Stable device buffers already shipped for the pending move (PUP::DeviceLife::Stable),
  // or null. Owned by this record; consumed by emigrate, discarded by a move
  // that goes elsewhere or by the destructor.
  CkStableSnapshot* stable = nullptr;
  int stableGen = 0;     // counts this element's snapshots (a discard names the one it means)

  //Creation and Destruction:
  CkLocRec(CkLocMgr *mgr,bool fromMigration,bool ignoreArrival, const CkArrayIndex &idx_, CmiUInt8 id);
  void migrateMe(int toPe); //Leave this processor
  void destroy(void); //User called destructor
  ~CkLocRec();

  /** Invoke the given entry method on this element.
   *   Returns false if the element died during the receive.
   *   If doFree is true, the message is freed after send;
   *    if false, the message can be reused.
   */
  bool invokeEntry(CkMigratable *obj,void *msg,int idx,bool doFree);

#if CMK_LBDB_ON  //For load balancing:
  /// Control the load balancer:
  void startTiming(int ignore_running=0);
  void stopTiming(int ignore_running=0);
  void setObjTime(double cputime);
  void setObjPosition(const std::vector<LBRealType>& pos);
  double getObjTime();
  const std::vector<LBRealType>& getObjPosition();
  // Previous home of the element and the balancing step that moved it; see
  // LDObjData::prevPe. Set on arrival by CkMigratable::pup.
  void setObjPrev(int pe, int stepNo);
  void getObjPrev(int& pe, int& stepNo);
  int lbStep();

  void setObjGPUTime(double gputime);
  double getObjGPUTime();
  // What the last balancing step read for this element: see
  // CkMigratable::getObjLastTime / getObjLastGPUTime.
  double getObjLastTime();
  double getObjLastGPUTime();

  void *getObjUserData(int idx);
#else
  inline void startTiming(int ignore_running=0) {  }
  inline void stopTiming(int ignore_running=0) { }
#endif
  inline const CkArrayIndex &getIndex(void) const {return idx;}
  inline CmiUInt8 getID() const { return id; }
  inline CkLocMgr *getLocMgr() const {return myLocMgr; }

#if CMK_LBDB_ON
public:
  inline CkSyncBarrier* getSyncBarrier() const { return syncBarrier; }
  inline LBManager *getLBMgr(void) const {return lbmgr;}
  inline MetaBalancer *getMetaBalancer(void) const {return the_metalb;}
  inline const LDObjHandle& getLdHandle() const{ return ldHandle; }
  static void staticMigrate(LDObjHandle h, int dest);
  // The balancer's replication decisions (LDCallbacks::clone / retire),
  // forwarded to CkMigratable::ckClone / ckRetire of the element(s) here.
  static void staticClone(LDObjHandle h, int dest, int rank);
  static void staticRetire(LDObjHandle h);
  static void staticCloneNotice(LDObjHandle h, int dest, int rank);
  static void staticRetireNotice(LDObjHandle h, int rank);
  static void staticMetaLBResumeWaitingChares(LDObjHandle h, int lb_ideal_period);
  static void staticMetaLBCallLBOnChares(LDObjHandle h);
  void recvMigrate(int dest);
  void setMigratable(int migratable);	/// set migratable
  void setPupSize(size_t obj_pup_size);
  void setGPUPupSize(size_t obj_gpu_pup_size);
  void setObjReplica(int group, int rank, int count, int maxCount, size_t extraBytes,
                     double costNode = 0.0, double costRemote = 0.0);
  void AsyncMigrate(bool use);
  bool isAsyncMigrate()   { return asyncMigrate; }
  void ReadyMigrate(bool ready) { readyMigrate = ready; } ///called from user
  bool isReadyMigrate()	{ return readyMigrate; }
  bool checkBufferedMigration();	// check and execute pending migration
  int   MigrateToPe();
  inline void setMeasure(bool status) { enable_measure = status; }
#else
  void AsyncMigrate(bool use){};
#endif
};

#endif // CK_LOC_REC_H

