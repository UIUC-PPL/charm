#ifndef CK_LOC_REC_H
#define CK_LOC_REC_H

class CkArray;//Array manager
class CkLocMgr;//Location manager
class CkMigratable;//Migratable object

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
  double getObjTime();
  const std::vector<LBRealType> getObjVectorLoad() const;
  void *getObjUserData(int idx);
  void CkLBSetPhase(int phase);
#else
  inline void startTiming(int ignore_running=0) {  }
  inline void stopTiming(int ignore_running=0) { }
#endif
  inline const CkArrayIndex &getIndex(void) const {return idx;}
  inline CmiUInt8 getID() const { return id; }
  inline CkLocMgr *getLocMgr() const {return myLocMgr; }
  inline CkSyncBarrier* getSyncBarrier() const { return syncBarrier; }

#if CMK_LBDB_ON
public:
  inline LBManager *getLBMgr(void) const {return lbmgr;}
  inline MetaBalancer *getMetaBalancer(void) const {return the_metalb;}
  inline LDObjHandle getLdHandle() const{return ldHandle;}
  static void staticMigrate(LDObjHandle h, int dest);
  static void staticMetaLBResumeWaitingChares(LDObjHandle h, int lb_ideal_period);
  static void staticMetaLBCallLBOnChares(LDObjHandle h);
  void recvMigrate(int dest);
  void setMigratable(int migratable);	/// set migratable
  void setPupSize(size_t obj_pup_size);
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

