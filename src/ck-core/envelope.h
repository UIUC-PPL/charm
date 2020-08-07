/**
 @defgroup CkEnvelope
 \brief  Charm++ message header.
*/
#ifndef _ENVELOPE_H
#define _ENVELOPE_H

#include <limits.h>
#include <pup.h>
#include <charm.h>
#include <middle.h>
#include <cklists.h>
#include <objid.h>

#ifndef CkIntbits
#define CkIntbits (sizeof(int)*8)
#endif

#if CMK_ERROR_CHECKING
#define _SET_USED(env, x) (env)->setUsed((x))
#define _CHECK_USED(env) do { if(env->isUsed()) \
                           CmiAbort("Message being re-sent. Aborting...\n"); \
                         } while(0)
#else
#define _SET_USED(env, x) do{}while(0)
#define _CHECK_USED(env) do{}while(0)
#endif

#define CkMsgAlignLength(x)     ALIGN_DEFAULT(x)
#define CkMsgAlignOffset(x)     (CkMsgAlignLength(x)-(x))
#define CkPriobitsToInts(nBits)    ((nBits+CkIntbits-1)/CkIntbits)


/**
    \addtogroup CriticalPathFramework 
    @{
*/

/** A class that is used to track the entry points and other information 
    about a critical path as a charm++ program executes.

    This class won't do useful things unless USE_CRITICAL_PATH_HEADER_ARRAY is defined
*/
class PathHistoryEnvelope {
 protected:
  // When passing paths forward, store information on PEs, in backward pass, lookup necessary information
  int sender_history_table_idx;
  double totalTime;
 public:
  double getTotalTime() const{ return totalTime; }
  int get_sender_history_table_idx() const{ return sender_history_table_idx; }
  void set_sender_history_table_idx(int i) { sender_history_table_idx = i; }
  PathHistoryEnvelope(){ 
    reset(); 
  }
  double getTime() const{ return totalTime; }
  void setTime(double t){ totalTime = t; }
  void pup(PUP::er &p) {
    p | sender_history_table_idx;
    p | totalTime;
  } 
  void reset();
  void print() const;
  /// Write a description of the path into the beginning of the provided buffer. The buffer ought to be large enough.
  void printHTMLToString(char* buf) const{
    buf[0] = '\0';
    sprintf(buf+strlen(buf), "Path Time=%lf<br> Sender idx=%d", (double)totalTime, (int)sender_history_table_idx);
  }
  /// The number of available EP counts 
  int getNumUsed() const;
  /// Return the count value for the idx'th available EP  
  int getUsedCount(int idx) const;
  /// Return the idx'th available EP 
  int getUsedEp(int idx) const;
  int getEpCount(int ep) const;
  void incrementTotalTime(double time);
  //  void createPath(envelope *originatingMsg);
  void setDebug100();
};
/** @} */



typedef unsigned int   UInt;
typedef unsigned short UShort;
typedef unsigned char  UChar;

#include "charm.h" // for CkGroupID, and CkEnvelopeType

/**
@addtogroup CkEnvelope
*/

CkpvExtern(int, envelopeEventID);

/**
@{
The class envelope defines a Charm++ message's header. The first
'CmiReservedHeaderSize' bytes of memory is exclusively reserved for Converse
header, which is defined in converse.h and platform specific config files.

After Charm++ envelope comes the payload, i.e. a variable-length amount of user
data. Following the user data, optionally, a variable number of bits of
priority data can be stored at the end. Function
envelope::alloc(msgtype,size,prio) is always used to allocate the whole
message. Note that this memory layout must be observed.

The following are a few terms that are used often:

<pre>
 Envelope pointer        \
 Converse message pointer -> [ [ Converse envelope ]       ]
                             [       Charm envelope        ] 
 User message pointer     -> [ User data/payload ... ]
 Priority pointer         -> [ Priority ints ... ]
</pre>

The "message pointers" passed to and from users bypass the envelope and point
*directly* to the user data--the routine "EnvToUsr" below adjusts an envelope
(or converse message) pointer into this user message pointer.  There is a
corresponding routine "UsrToEnv" which takes the user data pointer and returns
a pointer to the envelope/converse message.

Unfortunately, in the guts of Charm++ it's not always clear whether you've been
given a converse or user message pointer, as both tend to be passed as void *.
Confusing the two will invariably result in data corruption and bizarre
crashes.

FIXME: Make CkMessage inherit from envelope,
which would unify converse, envelope, and 
user message pointers.
*/

namespace ck {

  namespace impl {
    /**
       These structures store the type-specific message information.
    */
    union u_type {
      struct s_chare {  // NewChareMsg, NewVChareMsg, ForChareMsg, ForVidMsg, FillVidMsg
        void *ptr;      ///< object pointer
        UInt forAnyPe;  ///< Used only by newChare
        int  bype;      ///< created by this pe
      } chare;
      struct s_group {         // NodeBocInitMsg, BocInitMsg, ForNodeBocMsg, ForBocMsg
        CkGroupID g;           ///< GroupID
        CkNodeGroupID rednMgr; ///< Reduction manager for this group (constructor only!)
        int epoch;             ///< "epoch" this group was created during (0--mainchare, 1--later)
        UShort arrayEp;        ///< Used only for array broadcasts
      } group;
      struct s_array{             ///< For arrays only (ArrayEltInitMsg, ForArrayEltMsg)
        CmiUInt8 id;              /// <ck::ObjID if it could be in a union
        CkGroupID arr;            ///< Array manager GID
#if CMK_SMP_TRACE_COMMTHREAD
        UInt srcpe;
#endif
        UChar hopCount;           ///< number of times message has been routed
        UChar ifNotThere;         ///< what to do if array element is missing
      } array;
      struct s_roData {    ///< RODataMsg for readonly data type
        UInt count;
      } roData;
      struct s_roMsg {     ///< ROMsgMsg for readonlys defined in ci files
        UInt roIdx;
      } roMsg;
    };

    struct s_attribs {  // Packed bitwise struct
      UChar msgIdx;     ///< Usertype of message (determines pack routine)
      UChar mtype;      ///< e.g., ForBocMsg
      UChar queueing:4; ///< Queueing strategy (FIFO, LIFO, PFIFO, ...)
      UChar isPacked:1; ///< If true, message must be unpacked before use
      UChar isUsed:1;   ///< Marker bit to prevent message re-send.
      UChar isVarSysMsg:1; ///< True if msg is a variable sized sys message that doesn't use a pool
    };

  }
}

#define CMK_ENVELOPE_FT_FIELDS

#if CMK_REPLAYSYSTEM || CMK_TRACE_ENABLED
#define CMK_ENVELOPE_OPTIONAL_FIELDS                                           \
  UInt   event;        /* used by projections and record-replay */
#else
#define CMK_ENVELOPE_OPTIONAL_FIELDS
#endif

#define CMK_ENVELOPE_FIELDS                                                    \
  /* Converse message envelope, Must be first field in this class */           \
  char   core[CmiReservedHeaderSize];                                          \
  UInt   pe;           /* source processor */                                  \
  ck::impl::u_type type; /* Depends on message type (attribs.mtype) */         \
  size_t totalsize;    /* Byte count from envelope start to end of group dependencies */ \
  CMK_ENVELOPE_OPTIONAL_FIELDS                                                 \
  CMK_REFNUM_TYPE ref; /* Used by futures and SDAG */                          \
  UShort priobits;     /* Number of bits of priority data after user data */   \
  UShort groupDepNum;  /* Number of group dependencies */                      \
  UShort epIdx;        /* Entry point to call */                               \
  ck::impl::s_attribs attribs;

class envelope {
private:

    class envelopeSizeHelper {
      CMK_ENVELOPE_FIELDS
      CMK_ENVELOPE_FT_FIELDS
    };

    #ifdef __clang__
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wunused-private-field"
    #endif
    CMK_ENVELOPE_FIELDS
    #ifdef __clang__
    #pragma GCC diagnostic pop
    #endif

public:

    CMK_ENVELOPE_FT_FIELDS

    #if defined(__GNUC__) || defined(__clang__)
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wpedantic"
    #if defined(__clang__)
    #pragma GCC diagnostic ignored "-Wunused-private-field"
    #endif
    #endif
    // padding to ensure ALIGN_BYTES alignment
    UChar align[CkMsgAlignOffset(sizeof(envelopeSizeHelper))];
    #if defined(__GNUC__) || defined(__clang__)
    #pragma GCC diagnostic pop
    #endif

    void pup(PUP::er &p);
#if CMK_REPLAYSYSTEM || CMK_TRACE_ENABLED
    UInt   getEvent(void) const { return event; }
    void   setEvent(const UInt e) { event = e; }
#endif
    CMK_REFNUM_TYPE   getRef(void) const { return ref; }
    void   setRef(const CMK_REFNUM_TYPE r) { ref = r; }
    UChar  getQueueing(void) const { return attribs.queueing; }
    void   setQueueing(const UChar q) { attribs.queueing=q; }
    UChar  getMsgtype(void) const { return attribs.mtype; }
    void   setMsgtype(const UChar m) { attribs.mtype = m; }
#if CMK_ERROR_CHECKING
    UChar  isUsed(void) const { return attribs.isUsed; }
    void   setUsed(const UChar u) { attribs.isUsed=u; }
#else /* CMK_ERROR_CHECKING */
    inline void setUsed(const UChar u) {}
#endif
    UChar  getMsgIdx(void) const { return attribs.msgIdx; }
    void   setMsgIdx(const UChar idx) { attribs.msgIdx = idx; }
    size_t getTotalsize(void) const { return totalsize; }
    void   setTotalsize(const size_t s) { totalsize = s; }
    size_t getUsersize(void) const {
      return totalsize - getGroupDepSize() - getPrioBytes() - sizeof(envelope); 
    }
    void   setUsersize(const size_t s) {
      if (s == getUsersize()) {
        return;
      }
      CkAssert(s < getUsersize());
      size_t newPrioOffset = sizeof(envelope) + CkMsgAlignLength(s);
      size_t newTotalsize = newPrioOffset + getPrioBytes() + getGroupDepSize();
      void *newPrioPtr = (void *) ((char *) this + newPrioOffset); 
      // use memmove instead of memcpy in case memory areas overlap
      memmove(newPrioPtr, getPrioPtr(), getPrioBytes()); 
      setTotalsize(newTotalsize); 
    }

    // s specifies number of bytes to remove from user portion of message
    void shrinkUsersize(const size_t s) {
      CkAssert(s <= getUsersize());
      setUsersize(getUsersize() - s);
    }

    UChar  isPacked(void) const { return attribs.isPacked; }
    void   setPacked(const UChar p) { attribs.isPacked = p; }
    UChar  isVarSysMsg(void) const { return attribs.isVarSysMsg; }
    void   setIsVarSysMsg(const UChar d) { attribs.isVarSysMsg = d; }
    UShort getPriobits(void) const { return priobits; }
    void   setPriobits(const UShort p) { priobits = p; }
    UShort getPrioWords(void) const { return CkPriobitsToInts(priobits); }
    UShort getPrioBytes(void) const { return getPrioWords()*sizeof(int); }
    void*  getPrioPtr(void) const { 
      return (void *)((char *)this + totalsize - getGroupDepSize() - getPrioBytes());
    }
    void* getGroupDepPtr(void) const {
      return (void *)((char *)this + totalsize - getGroupDepSize());
    }
    static envelope *alloc(const UChar type, const size_t size=0, const UShort prio=0, const GroupDepNum groupDepNumRequest=GroupDepNum{}, const bool incEvent=true)
    {
      CkAssert(type>=NewChareMsg && type<LAST_CK_ENVELOPE_TYPE);
#if CMK_USE_STL_MSGQ
      // Ideally, this should be a static compile-time assert. However we need API changes for that
      CkAssert(sizeof(CMK_MSG_PRIO_TYPE) >= sizeof(int)*CkPriobitsToInts(prio));
#endif

      size_t tsize = sizeof(envelope)+
                     CkMsgAlignLength(size)+
                     sizeof(int)*CkPriobitsToInts(prio) +
                     sizeof(CkGroupID)*(int)groupDepNumRequest;

      //CkPrintf("[%d] inside envelope alloc groupDepNum:%d\n", CkMyPe(), (int)groupDepNumRequest);

      envelope *env = (envelope *)CmiAlloc(tsize);
#if CMK_REPLAYSYSTEM
      //for record-replay
      memset(env, 0, sizeof(envelope));
      if(incEvent)
	env->setEvent(++CkpvAccess(envelopeEventID));
      else
	env->setEvent(CkpvAccess(envelopeEventID));
#endif
      env->setMsgtype(type);
      env->totalsize = tsize;
      env->priobits = prio;
      env->setPacked(0);
      env->setGroupDepNum((int)groupDepNumRequest);
      _SET_USED(env, 0);
      env->setEpIdx(0);
      env->setIsVarSysMsg(0);

#if USE_CRITICAL_PATH_HEADER_ARRAY
      env->pathHistory.reset();
#endif


      return env;
    }
    void reset() {
#if CMK_REPLAYSYSTEM
      setEvent(++CkpvAccess(envelopeEventID));
#endif
      memset(getGroupDepPtr(), 0, getGroupDepSize());
    }
    UShort getEpIdx(void) const { return epIdx; }
    void   setEpIdx(const UShort idx) { epIdx = idx; }
    UInt   getSrcPe(void) const { return pe; }
    void   setSrcPe(const UInt s) { pe = s; }
    static void setSrcPe(char *env, const UInt s) { ((envelope*)env)->setSrcPe(s); }

// Readonly-specific fields
    UInt   getCount(void) const { 
      CkAssert(getMsgtype()==RODataMsg); return type.roData.count; 
    }
    void   setCount(const UInt c) { 
      CkAssert(getMsgtype()==RODataMsg); type.roData.count = c; 
    }
    UInt   getRoIdx(void) const { 
      CkAssert(getMsgtype()==ROMsgMsg); return type.roMsg.roIdx; 
    }
    void   setRoIdx(const UInt r) { 
      CkAssert(getMsgtype()==ROMsgMsg); type.roMsg.roIdx = r; 
    }
    
 // Chare-specific fields
    UInt isForAnyPE(void) const {
      CkAssert(getMsgtype()==NewChareMsg || getMsgtype()==NewVChareMsg); 
      return type.chare.forAnyPe; 
    }
    void setForAnyPE(UInt f) { 
      CkAssert(getMsgtype()==NewChareMsg || getMsgtype()==NewVChareMsg); 
      type.chare.forAnyPe = f; 
    }
    void*  getVidPtr(void) const {
      CkAssert(getMsgtype()==NewVChareMsg || getMsgtype()==ForVidMsg
          || getMsgtype()==FillVidMsg ||  getMsgtype()==DeleteVidMsg);
      return type.chare.ptr;
    }
    void   setVidPtr(void *p) {
      CkAssert(getMsgtype()==NewVChareMsg || getMsgtype()==ForVidMsg
          || getMsgtype()==FillVidMsg ||  getMsgtype()==DeleteVidMsg);
      type.chare.ptr = p;
    }
    void*  getObjPtr(void) const { 
      CkAssert(getMsgtype()==ForChareMsg); return type.chare.ptr; 
    }
    void   setObjPtr(void *p) { 
      CkAssert(getMsgtype()==ForChareMsg); type.chare.ptr = p; 
    }
    UInt getByPe(void) const {
      CkAssert(getMsgtype()==NewChareMsg || getMsgtype()==NewVChareMsg); 
      return type.chare.bype; 
    }
    void setByPe(UInt pe) { 
      CkAssert(getMsgtype()==NewChareMsg || getMsgtype()==NewVChareMsg); 
      type.chare.bype = pe; 
    }

// Group-specific fields
    CkGroupID   getGroupNum(void) const {
      CkAssert(getMsgtype()==BocInitMsg || getMsgtype()==ForBocMsg
          || getMsgtype()==BocBcastMsg || getMsgtype()==NodeBocInitMsg
          || getMsgtype()==ForNodeBocMsg || getMsgtype()==ArrayBcastMsg
          || getMsgtype() == ArrayBcastFwdMsg);
      return type.group.g;
    }
    void   setGroupNum(const CkGroupID g) {
      CkAssert(getMsgtype()==BocInitMsg || getMsgtype()==ForBocMsg
          || getMsgtype()==BocBcastMsg || getMsgtype()==NodeBocInitMsg
          || getMsgtype()==ForNodeBocMsg || getMsgtype()==ArrayBcastMsg
          || getMsgtype() == ArrayBcastFwdMsg);
      type.group.g = g;
    }
    void setGroupEpoch(int epoch) { CkAssert(getMsgtype()==BocInitMsg || getMsgtype()==NodeBocInitMsg); type.group.epoch=epoch; }
    int getGroupEpoch(void) const { CkAssert(getMsgtype()==BocInitMsg || getMsgtype()==NodeBocInitMsg); return type.group.epoch; }
    void setRednMgr(CkNodeGroupID r){ CkAssert(getMsgtype()==BocInitMsg || getMsgtype()==ForBocMsg
          || getMsgtype()==NodeBocInitMsg || getMsgtype()==ForNodeBocMsg);
 type.group.rednMgr = r; }
    CkNodeGroupID getRednMgr(){       CkAssert(getMsgtype()==BocInitMsg || getMsgtype()==ForBocMsg
          || getMsgtype()==NodeBocInitMsg || getMsgtype()==ForNodeBocMsg);
 return type.group.rednMgr; }
    UShort getGroupDepSize() const { return groupDepNum*sizeof(CkGroupID); }
    UShort getGroupDepNum() const { return groupDepNum; }
    void setGroupDepNum(const UShort &r) { groupDepNum = r; }
    CkGroupID getGroupDep(int index=0) { return *((CkGroupID *)getGroupDepPtr() + index); }
    void setGroupDep(const CkGroupID &r, int index=0) {
      *(((CkGroupID *)getGroupDepPtr())+ index) = r;
    }

// Array-specific fields
    CkGroupID getArrayMgr(void) const { 
      if (getMsgtype() == ForArrayEltMsg || getMsgtype() == ArrayEltInitMsg)
	return type.array.arr;
      else
            CkAbort("Cannot return ArrayID from msg for non-array entity");
	/* compiler appeasement, even though this will never be executed */
      return type.array.arr;
    }
    void setArrayMgr(const CkGroupID gid) { CkAssert(getMsgtype() == ForArrayEltMsg || getMsgtype() == ArrayEltInitMsg);  type.array.arr = gid; }
    int getArrayMgrIdx(void) const {CkAssert(getMsgtype() == ForArrayEltMsg || getMsgtype() == ArrayEltInitMsg);  return type.array.arr.idx;}
    UShort &getsetArrayEp(void) {return epIdx;}
    UShort &getsetArrayBcastEp(void) {return type.group.arrayEp;}
#if CMK_SMP_TRACE_COMMTHREAD
    UInt &getsetArraySrcPe(void) {return type.array.srcpe;}
#else
    UInt &getsetArraySrcPe(void) {return pe;}
#endif
    UChar &getsetArrayHops(void) { CkAssert(getMsgtype() == ForArrayEltMsg || getMsgtype() == ArrayEltInitMsg); return type.array.hopCount;}
    int getArrayIfNotThere(void) const { CkAssert(getMsgtype() == ForArrayEltMsg || getMsgtype() == ArrayEltInitMsg); return type.array.ifNotThere;}
    void setArrayIfNotThere(int nt) { CkAssert(getMsgtype() == ForArrayEltMsg || getMsgtype() == ArrayEltInitMsg); type.array.ifNotThere=nt;}

    void setRecipientID(ck::ObjID objid)
    {
      CkAssert(getMsgtype() == ForArrayEltMsg || getMsgtype() == ArrayEltInitMsg);
      type.array.id = objid.getID();
    }

    CmiUInt8 getRecipientID() const
    {
      CkAssert(getMsgtype() == ForArrayEltMsg || getMsgtype() == ArrayEltInitMsg);
      return type.array.id;
    }

#if USE_CRITICAL_PATH_HEADER_ARRAY
 public:
    /** The information regarding the entry methods that executed along the path to this one.
	\addtogroup CriticalPathFramework
    */
    PathHistoryEnvelope pathHistory;
#endif

};


inline envelope *UsrToEnv(const void *const msg) {
  return (envelope *)((intptr_t)msg - sizeof(envelope));
}

inline void *EnvToUsr(const envelope *const env) {
  return (void *)((intptr_t)env + sizeof(envelope));
}

inline envelope *_allocEnv(const int msgtype, const size_t size=0, const int prio=0, const GroupDepNum groupDepNum=GroupDepNum{}) {
  return envelope::alloc(msgtype,size,prio,groupDepNum);
}

#if CMK_REPLAYSYSTEM
inline envelope *_allocEnvNoIncEvent(const int msgtype, const size_t size=0, const int prio=0, const GroupDepNum groupDepNum=GroupDepNum{}) {
  return envelope::alloc(msgtype,size,prio,groupDepNum,false);
}
#endif

inline void *_allocMsg(const int msgtype, const size_t size, const int prio=0, const GroupDepNum groupDepNum=GroupDepNum{}) {
  return EnvToUsr(envelope::alloc(msgtype,size,prio,groupDepNum));
}

inline void _resetEnv(envelope *env) {
  env->reset();
}

#if CMK_REPLAYSYSTEM
inline void setEventID(envelope *env){
  env->setEvent(++CkpvAccess(envelopeEventID));
}
#endif

/** @} */

extern UChar   _defaultQueueing;

extern void CkPackMessage(envelope **pEnv);
extern void CkUnpackMessage(envelope **pEnv);

class MsgPool: public SafePool<void *> {
private:
    static void *_alloc(void) {
      /* CkAllocSysMsg() called in .def.h is not thread of sigio safe */
      envelope *env = _allocEnv(ForChareMsg,0,0,GroupDepNum{});
      env->setQueueing(_defaultQueueing);
      env->setMsgIdx(0);
      return EnvToUsr(env);
    }
    static void _reset(void* m) {
      envelope *env = UsrToEnv(m);
      _resetEnv(env);
    }
public:
    MsgPool():SafePool<void*>(_alloc, CkFreeMsg, _reset) {}
};

CkpvExtern(MsgPool*, _msgPool);

#endif
