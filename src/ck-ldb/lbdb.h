/**
 * \addtogroup CkLdb
*/
/*@{*/

#ifndef LBDBH_H
#define LBDBH_H

#include "converse.h"
#include "charm.h"
#include "middle.h"

#ifndef __STDC_FORMAT_MACROS
# define __STDC_FORMAT_MACROS
#endif
#ifndef __STDC_LIMIT_MACROS
# define __STDC_LIMIT_MACROS
#endif
#include <inttypes.h>
#include <list>
#include <vector>

#include "pup_stl.h"

class LBManager;//Forward declaration

//typedef float floatType;
// type defined by build option
#ifndef CMK_LBTIME_TYPE
#define CMK_LBTIME_TYPE double
#endif
typedef CMK_LBTIME_TYPE LBRealType;

#define COMPRESS_LDB	1

extern int _lb_version;

  /*  User-defined object ID is 4 ints long (as defined in converse.h) */
  /*  as OBJ_ID_SZ */

#if CMK_LBDB_ON
struct LDHandle {
  void *handle;            // pointer to LBDB
};
#else
typedef int LDHandle;
#endif

struct LDOMid {
  CkGroupID id;
  bool operator==(const LDOMid& omId) const {
    return id == omId.id?true:false;
  }
  bool operator<(const LDOMid& omId) const {
    return id < omId.id?true:false;
  }
  bool operator!=(const LDOMid& omId) const {
    return id == omId.id?false:true;
  }
  inline void pup(PUP::er &p);
};

struct LDOMHandle {
//  void *user_ptr;
  LDOMid id;
  int handle;		// index to LBOM
  inline void pup(PUP::er &p);

  bool operator==(const LDOMHandle &obj) const {
    return (this->handle == obj.handle && this->id == obj.id);
  }
};

/* LDObjKey uniquely identify one object */
struct LDObjKey {
  /// Id of the location manager for this object
  LDOMid omId;
  CmiUInt8 objId;
public:
  bool operator==(const LDObjKey& obj) const {
    return (bool)(omId == obj.omId && objId == obj.objId);
  }
  bool operator<(const LDObjKey& obj) const {
    if (omId < obj.omId) return true;
    else if (omId == obj.omId) return objId < obj.objId;
    else return false;
  }
  inline LDOMid &omID() { return omId; }
  inline CmiUInt8 &objID() { return objId; }
  inline const LDOMid &omID() const { return omId; }
  inline const CmiUInt8 &objID() const { return objId; }
  inline void pup(PUP::er &p);
};

typedef int LDObjIndex;
typedef int LDOMIndex;

struct LDObjHandle {
  LDOMHandle omhandle;
  CmiUInt8 id;
  LDObjIndex  handle;
  inline const LDOMid &omID() const { return omhandle.id; }
  inline const CmiUInt8 &objID() const { return id; }
  inline void pup(PUP::er &p);
  
  inline bool operator==(const LDObjHandle &obj) const {
    return (this == &obj) || ((this->id == obj.id) &&
      (this->handle == obj.handle) &&
      (this->omhandle == obj.omhandle)
    );
  }
};

/* defines user data layout  */
class LBUserDataLayout {
int length;
int count;
public:
  LBUserDataLayout(): length(0), count(0) {}
  int claim(int size) {
    count++;
    int oldlen = length;
    length+=size;
    return oldlen;
  }
  int size() { return length; }
};

CkpvExtern(LBUserDataLayout, lbobjdatalayout);

class LBObjUserData {
  std::vector<char> data;
public:
  LBObjUserData() {
    data.resize(CkpvAccess(lbobjdatalayout).size());
  }
  LBObjUserData(const LBObjUserData &d) {
    this->data = d.data;
  }
  LBObjUserData(LBObjUserData &&d) {
    this->data = std::move(d.data);
  }

  ~LBObjUserData() { }
  LBObjUserData &operator = (const LBObjUserData &d) {
    this->data = d.data;
    return *this;
  }
  LBObjUserData &operator = (LBObjUserData &&d) {
    this->data = std::move(d.data);
    return *this;
  }
  inline void pup(PUP::er &p);
  void *getData(int idx) { return data.data()+idx; }
};

struct LDObjData {
  LDObjHandle handle;
  LBRealType wallTime;
#if CMK_LB_CPUTIMER
  LBRealType cpuTime;
#endif
#if ! COMPRESS_LDB
  LBRealType minWall, maxWall;
#endif
  bool migratable;
  bool asyncArrival;
#if CMK_LB_USER_DATA
  LBObjUserData   userData;
#endif
  // An encoded approximation of the amount of data the object would pack;
  // call pup_decodeSize(pupSize) to get the actual approximate value
  CmiUInt2 pupSize;
  inline const LDOMHandle &omHandle() const { return handle.omhandle; }
  inline const LDOMid &omID() const { return handle.omhandle.id; }
  inline const CmiUInt8 &objID() const { return handle.id; }
  inline const CmiUInt8 &id() const { return handle.id; }
  inline void pup(PUP::er &p);
#if CMK_LB_USER_DATA
  void* getUserData(int idx)  { return userData.getData(idx); }
#endif
};

/* used by load balancer */
struct LDObjStats {
  int index;
  LDObjData data;
  int from_proc;
  int to_proc;
  inline void pup(PUP::er &p);
};

#define LD_PROC_MSG      1
#define LD_OBJ_MSG       2
#define LD_OBJLIST_MSG   3

struct LDCommDesc {
  char type;
  union {
    int destProc;		/* 1:   processor level message */
    struct{
      LDObjKey  destObj;		/* 2:   object based message    */
      int destObjProc;
    } destObj;
    struct {
      LDObjKey  *objs;
      int len;
    } destObjs;			/* 3:   one to many message     */
  } dest;
  char &get_type() { return type; }
  char get_type() const { return type; }
  int proc() const { return type==LD_PROC_MSG?dest.destProc:-1; }
  void setProc(int pe) { CmiAssert(type==LD_PROC_MSG); dest.destProc = pe; }
  int lastKnown() const { 
    if (type==LD_OBJ_MSG) return dest.destObj.destObjProc;
    if (type==LD_PROC_MSG) return dest.destProc;
    return -1;
  }
  LDObjKey &get_destObj() 
	{ CmiAssert(type==LD_OBJ_MSG); return dest.destObj.destObj; }
  LDObjKey const &get_destObj() const 
	{ CmiAssert(type==LD_OBJ_MSG); return dest.destObj.destObj; }
  LDObjKey * get_destObjs(int &len) 
	{ CmiAssert(type==LD_OBJLIST_MSG); len=dest.destObjs.len; return dest.destObjs.objs; }
  void init_objmsg(LDOMid &omid, CmiUInt8 &objid, int destObjProc) {
	type=LD_OBJ_MSG; 
  	dest.destObj.destObj.omID()=omid;
  	dest.destObj.destObj.objID() =objid;
  	dest.destObj.destObjProc = destObjProc;
  }
  void init_mcastmsg(LDOMid &omid, CmiUInt8 *objid, int len) {
	type=LD_OBJLIST_MSG; 
	dest.destObjs.len = len;
	dest.destObjs.objs = new LDObjKey[len];
        for (int i=0; i<len; i++) {
  	  dest.destObjs.objs[i].omID()=omid;
  	  dest.destObjs.objs[i].objID() =objid[i];
	}
  }
  inline bool operator==(const LDCommDesc &obj) const;
  inline LDCommDesc &operator=(const LDCommDesc &c);
  inline void pup(PUP::er &p);
};

struct LDCommData {
  int src_proc;			// sender can either be a proc or an obj
  LDObjKey  sender;		// 
  LDCommDesc   receiver;
  int  sendHash, recvHash;
  int messages;
  int bytes;
  inline LDCommData &operator=(const LDCommData &o) {
    if (&o == this) return *this;
    src_proc = o.src_proc;
    sender = o.sender; receiver = o.receiver;
    sendHash = o.sendHash; recvHash = o.recvHash;
    messages = o.messages;
    bytes = o.bytes;
    return *this;
  }
  inline int from_proc() const { return (src_proc != -1); }
  inline int recv_type() const { return receiver.get_type(); }
  inline void pup(PUP::er &p);
  inline void clearHash() { sendHash = recvHash = -1; }
};

/*
 * Callbacks from database to object managers
 */
typedef void (*LDMigrateFn)(LDObjHandle handle, int dest);
typedef void (*LDStatsFn)(LDOMHandle h, int state);
typedef void (*LDQueryEstLoadFn)(LDOMHandle h);
typedef void (*LDMetaLBResumeWaitingCharesFn) (LDObjHandle handle, int lb_ideal_period);
typedef void (*LDMetaLBCallLBOnCharesFn) (LDObjHandle handle);

struct LDCallbacks {
  LDMigrateFn migrate;
  LDStatsFn setStats;
  LDQueryEstLoadFn queryEstLoad;
  LDMetaLBResumeWaitingCharesFn metaLBResumeWaitingChares;
  LDMetaLBCallLBOnCharesFn metaLBCallLBOnChares;
};

/*
 * Local Barrier calls
 */
class LBClient;
typedef std::list<LBClient *>::iterator LDBarrierClient;

class LBReceiver;
typedef std::list<LBReceiver *>::iterator LDBarrierReceiver;

/*
 *  LBDB Configuration calls
 */


#if CMK_LBDB_ON
PUPbytes(LDHandle)
#endif

inline void LDOMid::pup(PUP::er &p) {
  id.pup(p);
}

inline void LDObjKey::pup(PUP::er &p) {
  p|omId;
  p|objId;
}

inline void LDObjStats::pup(PUP::er &p) {
  p|index;
  p|data;
  p|from_proc;
  p|to_proc;
}

inline void LDOMHandle::pup(PUP::er &p) {
  p|id;
  p|handle;
}

inline void LDObjHandle::pup(PUP::er &p) {
  p|omhandle;
  p|id;
  p|handle;
}

inline void LBObjUserData::pup(PUP::er &p) {
  p|data;
}

inline void LDObjData::pup(PUP::er &p) {
  p|handle;
  p|wallTime;
#if CMK_LB_CPUTIMER
  p|cpuTime;
#endif
#if ! COMPRESS_LDB
  p|minWall;
  p|maxWall;
#endif
  p|migratable;
  if (_lb_version > -1) p|asyncArrival;
#if CMK_LB_USER_DATA
  if (_lb_version > 2) {
    p|userData;
  }
#endif
  p|pupSize;
}

inline bool LDCommDesc::operator==(const LDCommDesc &obj) const {
    if (type != obj.type) return false;
    switch (type) {
    case LD_PROC_MSG: return dest.destProc == obj.dest.destProc;
    case LD_OBJ_MSG:  return dest.destObj.destObj == obj.dest.destObj.destObj;
    case LD_OBJLIST_MSG: { if (dest.destObjs.len != obj.dest.destObjs.len) 
                               return false;
                           for (int i=0; i<dest.destObjs.len; i++)
                             if (!(dest.destObjs.objs[i] == obj.dest.destObjs.objs[i])) return false;
                           return true; }
    }
    return false;
}
inline LDCommDesc & LDCommDesc::operator=(const LDCommDesc &c) {
    type = c.type;
    switch (type) {
    case LD_PROC_MSG: dest.destProc = c.dest.destProc; break;
    case LD_OBJ_MSG:  dest.destObj = c.dest.destObj; break;
    case LD_OBJLIST_MSG: { dest.destObjs.len = c.dest.destObjs.len;
                           dest.destObjs.objs = new LDObjKey[dest.destObjs.len];
                           for (int i=0; i<dest.destObjs.len; i++)
                             dest.destObjs.objs[i] = c.dest.destObjs.objs[i]; 
                           break; }
    }
    return *this;
}
inline void LDCommDesc::pup(PUP::er &p) {
  p|type;
  switch (type) {
  case LD_PROC_MSG:  p|dest.destProc; break;
  case LD_OBJ_MSG:   p|dest.destObj.destObj; 
                     if (_lb_version == -1 && p.isUnpacking()) 
		       dest.destObj.destObjProc = -1;
		     else
		       p|dest.destObj.destObjProc; 
		     break;
  case LD_OBJLIST_MSG:  {  p|dest.destObjs.len; 
                           if (p.isUnpacking()) 
                               dest.destObjs.objs = new LDObjKey[dest.destObjs.len];
                           for (int i=0; i<dest.destObjs.len; i++) p|dest.destObjs.objs[i];
                           break; }
  }   // end of switch
}

inline void LDCommData::pup(PUP::er &p) {
    p|src_proc;
    p|sender;
    p|receiver;
    p|messages;
    p|bytes;
    if (p.isUnpacking()) {
      sendHash = recvHash = -1;
    }
}

#endif /* LBDBH_H */

/*@}*/
