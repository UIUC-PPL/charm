/**
 * \addtogroup CkLdb
*/
/*@{*/

#ifndef LBOM_H
#define LBOM_H

#include "lbdb.h"
#include "LBObj.h"

class LBDatabase;

class LBOM
{
friend class LBDatabase;

public:
  LDOMid id() { return myid; };

  void *getUserData() { return userData; }

private:
  LBOM() { };

  LBOM(LBDatabase *_parent, LDOMid _id,
       void *_userData, LDCallbacks _callbacks)  {
    parent = _parent;
    myid = _id;
    userData = _userData;
    callbacks = _callbacks;
    registering_objs = false;
  };
  ~LBOM() { }

  void DepositHandle(LDOMHandle _h) { myhandle = _h; };
  void Migrate(LDObjHandle _h, int dest) { callbacks.migrate(_h,dest); };
  void Clone(LDObjHandle _h, int dest, int rank) { callbacks.clone(_h, dest, rank); };
  void Retire(LDObjHandle _h) { callbacks.retire(_h); };
  void CloneNotice(LDObjHandle _h, int dest, int rank) { callbacks.cloneNotice(_h, dest, rank); };
  void RetireNotice(LDObjHandle _h, int rank) { callbacks.retireNotice(_h, rank); };
#if CMK_LBDB_ON
  void MetaLBResumeWaitingChares(LDObjHandle _h, int lb_ideal_period) {
    callbacks.metaLBResumeWaitingChares(_h, lb_ideal_period);
  }
  void MetaLBCallLBOnChares(LDObjHandle _h) {
    callbacks.metaLBCallLBOnChares(_h);
  }
#endif
  bool RegisteringObjs() { return registering_objs; };
  void SetRegisteringObjs(bool _set) { registering_objs = _set; };

  LBDatabase *parent;
  LDOMid myid;
  LDOMHandle myhandle;
  void *userData;
  LDCallbacks callbacks;
  bool registering_objs;

};

#endif

/*@}*/
