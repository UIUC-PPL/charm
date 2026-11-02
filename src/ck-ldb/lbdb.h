/**
 * \addtogroup CkLdb
*/
/*@{*/

#ifndef LBDBH_H
#define LBDBH_H

/* First: reconverse's converse.h does not pull in conv-mach-opt.h, and the
   CMK_GPU / CMK_CUDA tests below must see the build options. */
#include "conv-mach-opt.h"
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
#include <unordered_map>
#include <vector>

#include "pup_stl.h"

#if CMK_GPU
#include "GpuScalingModel.h"
#endif

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

// Hash the complete load-balancing identity. Equality still verifies both
// fields, so hash collisions cannot alias objects from different managers.
struct LDObjKeyHash {
  std::size_t operator()(const LDObjKey &key) const noexcept {
    uint64_t hash = static_cast<uint64_t>(key.objID());
    hash ^= static_cast<uint64_t>(key.omID().id.idx) +
            UINT64_C(0x9e3779b97f4a7c15) + (hash << 6) + (hash >> 2);
    hash ^= hash >> 30;
    hash *= UINT64_C(0xbf58476d1ce4e5b9);
    hash ^= hash >> 27;
    hash *= UINT64_C(0x94d049bb133111eb);
    hash ^= hash >> 31;
    return static_cast<std::size_t>(hash);
  }
};

// CUPTI external-correlation IDs carry only 64 bits, while a Charm++ load-
// balancing identity contains both an object-manager ID and an element ID.
// This table assigns process-local tokens without discarding either part of
// that identity. Synchronization is intentionally left to its owner because
// lookups during buffer processing are already serialized.
class GpuObjectTokenTable {
public:
  static constexpr uint64_t noObjectToken() { return UINT64_MAX; }

  bool intern(const LDObjKey &key, uint64_t &token) {
    auto found = keyToToken_.find(key);
    if (found != keyToToken_.end()) {
      token = found->second;
      return true;
    }
    if (nextToken_ == noObjectToken()) return false;

    token = nextToken_++;
    keyToToken_.emplace(key, token);
    tokenToKey_.emplace(token, key);
    return true;
  }

  bool resolve(uint64_t token, LDObjKey &key) const {
    auto found = tokenToKey_.find(token);
    if (found == tokenToKey_.end()) return false;
    key = found->second;
    return true;
  }

  std::size_t size() const { return keyToToken_.size(); }

  void clear() {
    keyToToken_.clear();
    tokenToKey_.clear();
    nextToken_ = 1;
  }

private:
  std::unordered_map<LDObjKey, uint64_t, LDObjKeyHash> keyToToken_;
  std::unordered_map<uint64_t, LDObjKey> tokenToKey_;
  uint64_t nextToken_ = 1;
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

#if CMK_GPU
// Per-kernel record captured via CUPTI, used for SM-utilization-normalized
// GPU load attribution.
//
// These are consumed entirely within the process that produced them (see
// hapiNormalizeCuptiLoads) and are never shipped to the central LB: every PE
// sharing a GPU is in the same process, so the whole kernel timeline for a
// device is already local. Only the resulting scalar load per object crosses
// the wire, in LDObjData::gpuTime.
struct LBKernelRecord {
  uint64_t start_ns;   // CUPTI device-clock timestamp (ns)
  uint64_t end_ns;     // CUPTI device-clock timestamp (ns)
  uint32_t device_id;  // CUPTI device id this kernel ran on
  int      sms_used;   // Number of SMs occupied while this kernel was running
  // Identity under which this invocation is modeled. The launch signature it
  // was derived from is deliberately not kept: only the hash is consumed
  // downstream, and there is one of these per kernel launch.
  GpuKernelKey kernel_key;
  bool has_explicit_work_tag;
  // Set when the identity cannot be trusted -- currently only when a work tag
  // was known to exist for this launch but had already been aged out by the
  // time the kernel record arrived. Such a kernel still earns load and still
  // occupies SMs, but its demand goes to unmodeledGpuTime rather than into a
  // bucket it does not belong in. See design decision 2 in the plan.
  bool unmodelable;
};
#endif

struct LDObjData {
  LDObjHandle handle;
  LBRealType wallTime;
  // Wall time this object's measurement window was open during the interval
  // (LBObj::windowTime). Under +LBAsync that is from its reopen once every
  // migration has landed until its own join -- deliberately less than the
  // time it was active, because an object's rate depends on the device it
  // sits on, so the transition is not measured. The database scales the loads
  // and traffic it hands out by activeSpan/windowTime (LBDatabase::GetObjData
  // and GetCommData), so every strategy sees rates over the object's active
  // time; this is the window they came from, for diagnosis and the simulator.
  LBRealType windowTime;
#if CMK_GPU
  // SM-utilization-normalized GPU load, in seconds of whole-device occupancy.
  // Computed in-process by hapiNormalizeCuptiLoads before the stats are sent.
  LBRealType gpuTime;
  // The object's share of its process's driver time: the host time spent
  // inside CUDA runtime calls, which several PEs on one context serialise
  // between them. Taken out of wallTime (LBDatabase::SetObjDriverLoad), so
  // wallTime is host work the PE alone does, and aggregated per process like
  // gpuTime -- the launch term of LBLoadDim.h.
  LBRealType driverTime;
  // The same demand broken down by kernel identity, plus the GPU it was
  // measured on. Empty unless +LBGpuScaling is on. gpuTime remains the
  // authoritative scalar: gpuCosts.totalDemand() reconciles with it.
  GpuObjectEpochCosts gpuCosts;
#endif
#if CMK_LB_CPUTIMER
  LBRealType cpuTime;
#endif
#if ! COMPRESS_LDB
  LBRealType minWall, maxWall;
#endif
  bool migratable;
  bool asyncArrival;
  // Object position, for geometric load balancers (DiffusionLB's MetricCentroid).
  // Ported as a field only: the CkMigratable::setObjPosition plumbing that would
  // populate it was deliberately not brought across, because the comm-aware metric
  // (MetricComm, +LBDiffusionCommOn) does not use it. It therefore stays empty --
  // anything reading it must guard for that.
  std::vector<LBRealType> position;
  // The PE this object lived on before its most recent migration, and the
  // balancing step that moved it; -1 until it has ever moved. Set by the
  // runtime on arrival (CkMigratable::pup), so a balancer can undo its own
  // previous step -- send every object that step moved back where it came
  // from -- without keeping a ledger of moves that would itself have to
  // survive the moves.
  int prevPe;
  int prevStep;
#if CMK_LB_USER_DATA
  LBObjUserData   userData;
#endif
  // An encoded approximation of the amount of data the object would pack;
  // call pup_decodeSize(pupSize) to get the actual approximate value
  CmiUInt2 pupSize;
#if CMK_GPU
  size_t gpuPupSize;
#endif
  // Replication, declared by the application (CkMigratable::setLBReplica).
  // replicaGroup < 0: the object cannot be replicated, which is the default
  // and what every object that never declares has. Otherwise the objects
  // sharing a group are copies of one logical object: replicaRank is this
  // copy's index, replicaCount how many copies exist now, replicaMax how many
  // the application can keep consistent, and replicaExtraBytes what one copy
  // holds on its device beyond the object's own footprint (the state the
  // copies exchange). A balancer that understands groups (ShedLB) may ask for
  // a copy to be made or retired; see LBAction and CkMigratable::ckClone.
  // replicaCostNode / replicaCostRemote: the load (same units as the declared
  // load) one more copy adds to EACH copy per balancing interval for the
  // exchange with that one peer, on the same node or across nodes; zero when
  // the copies exchange nothing (serving). The balancer counts a copy as
  // its share of the group's load plus its exchanges with every other copy.
  int replicaGroup;
  int replicaRank;
  int replicaCount;
  int replicaMax;
  size_t replicaExtraBytes;
  double replicaCostNode;
  double replicaCostRemote;
  inline bool replicable() const { return replicaGroup >= 0 && replicaMax > 1; }
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
// A balancer's two replication decisions (LDObjData::replicaGroup): make a
// copy of this object on dest with the given replica rank; retire this copy.
typedef void (*LDCloneFn)(LDObjHandle handle, int dest, int rank);
typedef void (*LDRetireFn)(LDObjHandle handle);
typedef void (*LDCloneNoticeFn)(LDObjHandle handle, int dest, int rank);
typedef void (*LDRetireNoticeFn)(LDObjHandle handle, int rank);
typedef void (*LDStatsFn)(LDOMHandle h, int state);
typedef void (*LDQueryEstLoadFn)(LDOMHandle h);
typedef void (*LDMetaLBResumeWaitingCharesFn) (LDObjHandle handle, int lb_ideal_period);
typedef void (*LDMetaLBCallLBOnCharesFn) (LDObjHandle handle);

struct LDCallbacks {
  LDMigrateFn migrate;
  LDCloneFn clone;
  LDRetireFn retire;
  LDCloneNoticeFn cloneNotice;
  LDRetireNoticeFn retireNotice;
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
  // Version-gated like gpuCosts: an older dump has no such field.
  if (_lb_version > 5) p|windowTime;
  else if (p.isUnpacking()) windowTime = 0;
#if CMK_GPU
  p|gpuTime;
  // Version-gated like gpuCosts: an older dump has no such field.
  if (_lb_version > 3) p|driverTime;
  else if (p.isUnpacking()) driverTime = 0;
#endif
#if CMK_LB_CPUTIMER
  p|cpuTime;
#endif
#if ! COMPRESS_LDB
  p|minWall;
  p|maxWall;
#endif
  p|migratable;
  if (_lb_version > -1) p|asyncArrival;
  p|position;
  // Version-gated like gpuCosts: an older dump has no such fields.
  if (_lb_version > 3) { p|prevPe; p|prevStep; }
  else if (p.isUnpacking()) { prevPe = -1; prevStep = -1; }
#if CMK_LB_USER_DATA
  if (_lb_version > 2) {
    p|userData;
  }
#endif
  p|pupSize;
#if CMK_GPU
  p|gpuPupSize;
  // Version-gated so an older simulation dump loads with empty summaries
  // rather than misreading the stream.
  if (_lb_version > 3) p|gpuCosts;
#endif
  // Version 8 adds the replication declaration; an older dump has none.
  if (_lb_version > 7) {
    p|replicaGroup; p|replicaRank; p|replicaCount; p|replicaMax; p|replicaExtraBytes;
  } else if (p.isUnpacking()) {
    replicaGroup = -1; replicaRank = 0; replicaCount = 1; replicaMax = 1; replicaExtraBytes = 0;
  }
  // Version 9 adds the copy's recurring exchange cost; an older dump has none.
  if (_lb_version > 8) {
    p|replicaCostNode; p|replicaCostRemote;
  } else if (p.isUnpacking()) {
    replicaCostNode = replicaCostRemote = 0.0;
  }
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
