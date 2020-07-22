#ifndef LBDATABASE_H
#define LBDATABASE_H

#include "lbdb.h"

#include "LBObj.h"
#include "LBOM.h"
#include "LBComm.h"
#include "LBMachineUtil.h"

#include <vector>

class CkSyncBarrier;

class LBDatabase {
friend class LBManager;
private:
  LBDatabase();
  struct LBObjEntry {
    static const LDObjIndex DEFAULT_NEXT = -1;
    LBObj* obj;
    LDObjIndex nextEmpty;

    LBObjEntry(LBObj* obj, LDObjIndex nextEmpty = DEFAULT_NEXT) : obj(obj), nextEmpty(nextEmpty) {}
  };

  std::vector<LBObjEntry> objs;
  std::vector<LBOM*> oms;
  LDObjIndex objsEmptyHead;
  int omCount;
  int omsRegistering;
  bool obj_running;
  LBCommTable* commTable;
  LDObjIndex runningObj; // index of the runningObj in objs
  bool statsAreOn;
  double obj_walltime;
  LBMachineUtil machineUtil;
  CkSyncBarrier* syncBarrier;



#if CMK_LB_CPUTIMER
  double obj_cputime;
#endif

public:
  inline void MeasuredObjTime(double wtime, double ctime) {
    if (statsAreOn) {
      obj_walltime += wtime;
#if CMK_LB_CPUTIMER
      obj_cputime += ctime;
#endif
    }
  }
  inline LBOM* LbOM(LDOMHandle h) {
    return oms[h.handle];
  }
  inline LBObj *LbObj(const LDObjHandle &h) const {
    return objs[h.handle].obj;
  }
  inline LBObj *LbObjIdx(int h) const {
    return objs[h].obj;
  }
  inline const LDObjHandle &RunningObj() const {
    return objs[runningObj].obj->GetLDObjHandle();
  }

  inline void ObjTime(LDObjHandle h, double walltime, double cputime) {
    LbObj(h)->IncrementTime(walltime, cputime);
    MeasuredObjTime(walltime, cputime);
  };

  inline void GetObjLoad(LDObjHandle &h, LBRealType &walltime, LBRealType &cputime) {
    LbObj(h)->getTime(&walltime, &cputime);
  };

  inline const std::vector<LBRealType> GetObjVectorLoad (const LDObjHandle& h) const {
    return LbObj(h)->getVectorLoad();
  }

  inline void* GetObjUserData(LDObjHandle &h) {
    return LbObj(h)->getLocalUserData();
  }
  inline void TurnStatsOn(void)
       {statsAreOn = true; machineUtil.StatsOn();}
  inline void TurnStatsOff(void)
       {statsAreOn = false; machineUtil.StatsOff();}
  inline bool StatsOn(void) const
       { return statsAreOn; };
  inline void IdleTime(LBRealType *walltime) {
    machineUtil.IdleTime(walltime);
  }
  inline void TotalTime(LBRealType *walltime, LBRealType *cputime) {
    machineUtil.TotalTime(walltime, cputime);
  }

  inline void QueryKnownObjLoad(LDObjHandle &h, LBRealType &walltime, LBRealType &cputime) {
    LbObj(h)->lastKnownLoad(&walltime, &cputime);
  };
  inline void NonMigratable(LDObjHandle h) { LbObj(h)->SetMigratable(false); };
  inline void Migratable(LDObjHandle h) { LbObj(h)->SetMigratable(true); };
  inline void setPupSize(LDObjHandle h, size_t pup_size) { LbObj(h)->setPupSize(pup_size);};
  inline void UseAsyncMigrate(LDObjHandle h, bool flag) { LbObj(h)->UseAsyncMigrate(flag); };
  inline int GetCommDataSz(void) {
    if (commTable)
      return commTable->CommCount();
    else return 0;
  }

  inline void GetCommData(LDCommData *data) {
    if (commTable) commTable->GetCommData(data);
  }

  inline void GetCommInfo(int& bytes, int& msgs, int& withinbytes, int& outsidebytes, int& num_nghbors, int& hops, int& hopbytes) {
    if (commTable)
      commTable->GetCommInfo(bytes, msgs, withinbytes, outsidebytes, num_nghbors, hops, hopbytes);
  }


  LDOMHandle RegisterOM(LDOMid userID, void *userptr, LDCallbacks cb);
  int Migrate(LDObjHandle h, int dest);
  void UnregisterOM(LDOMHandle omh);
  void RegisteringObjects(LDOMHandle omh);
  void DoneRegisteringObjects(LDOMHandle omh);
  int GetObjDataSz(void);
  void GetObjData(LDObjData *data);
  void MetaLBCallLBOnChares();
  void MetaLBResumeWaitingChares(int lb_period);
  void ClearLoads(void);
  int useMem(void);
  LDObjHandle RegisterObj(LDOMHandle omh, CmiUInt8 id, void* userPtr,
                          int migratable);
  void UnregisterObj(LDObjHandle h);
  void EstObjLoad(const LDObjHandle &h, double cpuload);
  void BackgroundLoad(LBRealType *walltime, LBRealType *cputime);
  void Send(const LDOMHandle &destOM, const CmiUInt8 &destID, unsigned int bytes, int destObjProc, int force = 0);
  void MulticastSend(const LDOMHandle &_om, CmiUInt8 *_ids, int _n, unsigned int _b, int _nMsgs=1);
  void GetTime(LBRealType *total_walltime, LBRealType *total_cputime,
               LBRealType *idletime, LBRealType *bg_walltime,
               LBRealType *bg_cputime);
  const std::vector<LBObjEntry>& getObjs() {return objs;}
/**
     runningObj records the obj handler index so that load balancer
     knows if an event(e.g. Send) is in an entry function or not.
     An index is enough here because LDObjHandle can be retrieved from
     objs array. Copying LDObjHandle is expensive.
  */
  inline void SetRunningObj(const LDObjHandle &_h) {
    runningObj = _h.handle;
    obj_running = true;
  }
  inline void NoRunningObj() {
    obj_running = false;
  }
  inline bool ObjIsRunning() const {
    return obj_running;
  }
  inline int RunningObject(LDObjHandle* _o) const {
#if CMK_LBDB_ON
      if (ObjIsRunning()) {
        *_o = RunningObj();
        return 1;
      }
#endif
      return 0;
  };
  inline const LDObjHandle *RunningObject() const {
#if CMK_LBDB_ON
      if (ObjIsRunning()) {
        return &(RunningObj());
      }
#endif
      return NULL;
  };

  inline void ObjectStart(const LDObjHandle &h) {
    if (ObjIsRunning()) {
      ObjectStop(*RunningObject());
    }

    SetRunningObj(h);

    if (StatsOn()) {
      LbObj(h)->StartTimer();
    }
  };
  inline void ObjectStop(const LDObjHandle &h) {
    LBObj* const obj = LbObj(h);

    if (StatsOn()) {
      LBRealType walltime, cputime;
      obj->StopTimer(&walltime, &cputime);
      obj->IncrementTime(walltime, cputime);
      MeasuredObjTime(walltime, cputime);
    }

    NoRunningObj();
  };

  inline void SetPhase(const LDObjHandle &h, int phase) {
    LbObj(h)->setPhase(phase);
  }
  inline const LDObjHandle &GetObjHandle(int idx) {
    return LbObjIdx(idx)->GetLDObjHandle();
  }
  inline void CollectStatsOn(void) {
    if (!StatsOn()) {
      if (ObjIsRunning()) {
        LbObj(*RunningObject())->StartTimer();
      }
      TurnStatsOn();
    }
  };
  inline void CollectStatsOff(void) { TurnStatsOff(); };
  inline int  CollectingStats(void) {
  #if CMK_LBDB_ON
    return StatsOn();
  #else
    return 0;
  #endif
  };
};

#endif /* LBDATABASE_H */
