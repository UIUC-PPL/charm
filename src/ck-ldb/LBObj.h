/**
 * \addtogroup CkLdb
*/
/*@{*/

#ifndef LBOBJ_H
#define LBOBJ_H

#include "lbdb.h"

class LBDatabase;

class LBObj
{
friend class LBDatabase;

public:
  LBObj(const LDObjHandle &_h, void *usr_ptr = NULL, bool _migratable=true, bool _asyncArrival = false) {
    data.handle = _h;
    data.migratable = _migratable;
    data.asyncArrival = _asyncArrival;
    Clear();
//    data.cpuTime = 0.;
//    data.wallTime = 0.;
//    data.minWall = 1e6;
//    data.maxWall = 0.;
    localUserData = usr_ptr;
//    migratable = _migratable;
//    registered = true;
    startWTime = -1.0;
    lastWallTime = .0;
#if CMK_LB_CPUTIMER
    startCTime = -1.0;
    lastCpuTime = .0;
#endif
  }

  ~LBObj() { };

  void Clear(void);

  void IncrementTime(LBRealType walltime, LBRealType cputime);
  inline void StartTimer(void) {
	startWTime = CkWallTimer();
#if CMK_LB_CPUTIMER
	startCTime = CkCpuTimer();
#endif
  }
  inline void StopTimer(LBRealType* walltime, LBRealType* cputime) {
	if (startWTime >= 0.0) {	// in case startOn in middle of entry
          const double endWTime = CkWallTimer();
	  *walltime = endWTime - startWTime;
#if CMK_LB_CPUTIMER
          const double endCTime = CkCpuTimer();
	  *cputime = endCTime - startCTime;
#else
	  *cputime = *walltime;
#endif
	}
        else {
          *walltime = *cputime = 0.0;
        }
  }

  inline void getTime(LBRealType *w, LBRealType *c) {
    *w = data.wallTime;
#if CMK_LB_CPUTIMER
    *c = data.cpuTime;
#else
    *c = *w;
#endif
  }

  inline void setTiming(LBRealType cputime)
  {
    data.wallTime = cputime;
#if CMK_LB_CPUTIMER
    data.cpuTime = cputime;
#endif
  }

  inline void setPosition(const std::vector<LBRealType>& pos)
  {
    data.position = pos;
  }

  inline const std::vector<LBRealType>& getPosition()
  {
    return data.position;
  }

  inline LDOMHandle &parentOM() { return data.handle.omhandle; }
  inline const LDObjHandle &GetLDObjHandle() const { return data.handle; }
  inline void SetMigratable(bool mig) { data.migratable = mig; }
  inline void setPupSize(size_t obj_pup_size) {
    data.pupSize = pup_encodeSize(obj_pup_size);
  }
  inline void UseAsyncMigrate(bool async) { data.asyncArrival = async; }
  inline LDObjData &ObjData() { return data; };
  inline void lastKnownLoad(LBRealType *w, LBRealType *c) {
    *w = lastWallTime;
#if CMK_LB_CPUTIMER
    *c = lastCpuTime;
#else
    *c = *w;
#endif
  }
  inline void *getLocalUserData() { return  localUserData; }
#if CMK_LB_USER_DATA
  inline void *getDBUserData(int idx) { return  data.getUserData(idx); }
#endif
private:

  void *localUserData;               // local user data, not in database
//  LDOMHandle parentOM;
//  LDObjHandle myhandle;
  LDObjData data;
//  bool registered;
  double startWTime;             // needs double precision
  LBRealType lastWallTime;
#if CMK_LB_CPUTIMER
  double startCTime;
  LBRealType lastCpuTime;
#endif
//  bool migratable;   // temp
};

#endif

/*@}*/
