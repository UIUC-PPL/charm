/**
\file
\brief Charm Kernel--the groups and chares level of Charm++.
*/
#ifndef CHARM_H
#define CHARM_H

#include "converse.h"
#include <sys/types.h> /* for size_t */

#ifdef __cplusplus
#include "conv-rdma.h"
#include "pup.h"
extern "C" {
#endif


/******************************************************************************
 *
 * Converse Concepts, renamed to CK
 *
 *****************************************************************************/

/** Queueing types, for use with CkSetQueueing: */
#define CK_QUEUEING_FIFO   CQS_QUEUEING_FIFO
#define CK_QUEUEING_LIFO   CQS_QUEUEING_LIFO
#define CK_QUEUEING_IFIFO  CQS_QUEUEING_IFIFO
#define CK_QUEUEING_ILIFO  CQS_QUEUEING_ILIFO
#define CK_QUEUEING_BFIFO  CQS_QUEUEING_BFIFO
#define CK_QUEUEING_BLIFO  CQS_QUEUEING_BLIFO
#define CK_QUEUEING_LFIFO  CQS_QUEUEING_LFIFO
#define CK_QUEUEING_LLIFO  CQS_QUEUEING_LLIFO

#define CkTimer  	CmiTimer
#define CkWallTimer  	CmiWallTimer
#define CkCpuTimer  	CmiCpuTimer

#define CkMyPe			CmiMyPe
#define CkMyRank		CmiMyRank
#define CkMyNode		CmiMyNode
#define CkNumPes		CmiNumPes
#define CkNumNodes		CmiNumNodes
#define CkNumPhysicalNodes	CmiNumPhysicalNodes
#define CkNodeFirst		CmiNodeFirst
#define CkNodeSize		CmiNodeSize
#define CkMyNodeSize		CmiMyNodeSize
#define CkNodeOf		CmiNodeOf
#define CkRankOf		CmiRankOf

#define CkPrintf                CmiPrintf
#define CkScanf                 CmiScanf
#define CkError                 CmiError
#define CkAbort                 CmiAbort
#define CkAssert                CmiAssert
#define CkAssertMsg             CmiAssertMsg
#define CkEnforce               CmiEnforce
#define CkEnforceMsg            CmiEnforceMsg
#define CkSetPeHelpsOtherThreads CmiSetPeHelpsOtherThreads

void realCkExit(int exitcode);

/* Optional parameter for CkExit() - based on
https://stackoverflow.com/a/28074198/1250282 */

#define CKEXIT_1(x) realCkExit(x)
#define CKEXIT_0() CKEXIT_1(0) /* Default CkExit() exit code: 0 */

#define CKEXIT_FUNC_CHOOSER(_f1, _f2, _f3, ...) _f3
#define CKEXIT_FUNC_RECOMPOSER(argsWithParentheses) CKEXIT_FUNC_CHOOSER argsWithParentheses
#define CKEXIT_CHOOSE_FROM_ARG_COUNT(...) CKEXIT_FUNC_RECOMPOSER((__VA_ARGS__, CKEXIT_2, CKEXIT_1, ))
#define CKEXIT_NO_ARG_EXPANDER() ,,CKEXIT_0
#define CKEXIT_MACRO_CHOOSER(...) CKEXIT_CHOOSE_FROM_ARG_COUNT(CKEXIT_NO_ARG_EXPANDER __VA_ARGS__ ())
#define CkExit(...) CKEXIT_MACRO_CHOOSER(__VA_ARGS__)(__VA_ARGS__)


#if CMK_SHRINK_EXPAND
extern void  CkCleanup(void);
#endif
extern char **CkGetArgv(void);
extern int  CkGetArgc(void);

/******************************************************************************
 *
 * Miscellaneous Constants
 *
 *****************************************************************************/

#define CK_PE_ALL        CLD_BROADCAST_ALL
#define CK_PE_ALL_BUT_ME CLD_BROADCAST
#define CK_PE_ANY        CLD_ANYWHERE

/******************************************************************************
 *
 * Message Allocation Calls
 *
 *****************************************************************************/
#ifdef __cplusplus
class CkEntryOptions;
extern void* CkAllocSysMsg(const CkEntryOptions *opts = NULL);
#ifndef GROUPDEPNUM_DECLARED
# define GROUPDEPNUM_DECLARED
struct GroupDepNum
{
  int groupDepNum;
  explicit GroupDepNum(int g = 0) : groupDepNum{g} { }
  operator int() const { return groupDepNum; }
};
#endif
extern void* CkAllocMsg(int msgIdx, int msgBytes, int prioBits, GroupDepNum groupDepNum=GroupDepNum{});
#endif
extern void  CkFreeSysMsg(void *msg);
extern void* CkAllocBuffer(void *msg, int bufsize);
extern void  CkFreeMsg(void *msg);
extern void* CkCopyMsg(void **pMsg);
extern void* CkReferenceMsg(void *msg);
extern void  CkSetQueueing(void *msg, int strategy);
extern void* CkPriorityPtr(void *msg);

/******************************************************************************
 *
 * Functions be to called from external clients (e.g. Charm4py)
 *
 *****************************************************************************/
#if CMK_CHARM4PY

extern void registerCkRegisterMainModuleCallback(void (*cb)(void));
extern void registerMainchareCtorExtCallback(void (*cb)(int, void*, int, int, char **));
extern void registerReadOnlyRecvExtCallback(void (*cb)(int, char*));
extern void registerChareMsgRecvExtCallback(void (*cb)(int, void*, int, int, char*, int));
extern void registerGroupMsgRecvExtCallback(void (*cb)(int, int, int, char *, int));
extern void registerArrayMsgRecvExtCallback(void (*cb)(int, int, int *, int, int, char *, int));
extern void registerArrayBcastRecvExtCallback(void (*cb)(int, int, int, int, int*, int, int, char *, int));
extern void registerArrayElemLeaveExtCallback(int (*cb)(int, int, int *, char**, int));
extern void registerArrayElemJoinExtCallback(void (*cb)(int, int, int *, int, char*, int));
extern void registerArrayResumeFromSyncExtCallback(void (*cb)(int, int, int *));
extern void registerArrayMapProcNumExtCallback(int (*cb)(int, int, const int *));
extern void StartCharmExt(int argc, char **argv); // start Converse/Charm, argv are the command-line arguments
extern int CkMyPeHook(void);   // function equivalent of CkMyPe macro
extern int CkNumPesHook(void); // function equivalent of CkNumPes macro
/// Get current redNo of specified group instance on this PE
extern int CkGroupGetReductionNumber(int gid);
/// Get current redNo of specified array element on this PE
extern int CkArrayGetReductionNumber(int aid, int ndims, int *index);
extern void CkSetMigratable(int aid, int ndims, int *index, char migratable);
extern void CkStartQDExt_ChareCallback(int onPE, void* objPtr, int epIdx, int fid);
extern void CkStartQDExt_GroupCallback(int gid, int pe, int epIdx, int fid);
extern void CkStartQDExt_ArrayCallback(int aid, int* idx, int ndims, int epIdx, int fid);
extern void CkStartQDExt_SectionCallback(int sid_pe, int sid_cnt, int rootPE, int ep);
extern void registerCreateCallbackMsgExtCallback(void (*cb)(void*, int, int, int, int *, char**, int*));
extern void registerPyReductionExtCallback(int (*cb)(char**, int*, int, char**));

#endif
/*********************************************************/
/**
\addtogroup CkRegister
\brief Charm Registration--keeps track of the possible chare and method types.

These are implemented in register.C.
*/
/*@{*/
/** Message pack function: convert a message into a buffer. */
typedef void* (*CkPackFnPtr)(void *msg);
/** Message unpack function: convert a buffer into a message. */
typedef void* (*CkUnpackFnPtr)(void *buf);
/** Message dealloc function: deletes a message. */
typedef void (*CkDeallocFnPtr)(void *msg);

/** Register this message name, with this basic size and pack and unpack functions. */
extern int CkRegisterMsg(const char *name, CkPackFnPtr pack, 
                       CkUnpackFnPtr unpack, CkDeallocFnPtr dealloc, size_t size);

/** This entry point flag indicates the method does not keep the passed-in message. */
#define CK_EP_NOKEEP        (1<<2) 
#define CK_EP_INTRINSIC     (1<<3) 
#define CK_EP_TRACEDISABLE  (1<<4) 

#define CK_EP_MEMCRITICAL (1<<5)
#define CK_EP_APPWORK     (1<<6)
#define CK_EP_IMMEDIATE   (1<<7)
#define CK_EP_INLINE      (1<<8)

/** type of a chare */
typedef enum{
	TypeInvalid=0,
	TypeChare,
	TypeMainChare,
	TypeGroup,
	TypeNodeGroup,
	TypeArray
} ChareType;

// What to do if an entry method is invoked on
// an array element that does not (yet) exist:
typedef enum{
	CkArray_IfNotThere_buffer = 0,     // Wait for it to be created
	CkArray_IfNotThere_createhere = 1, // Make it on sending Pe
	CkArray_IfNotThere_createhome = 2  // Make it on (a) home Pe
} CkArray_IfNotThere;

/** A "call function" to invoke a method on an object. See EntryInfo */
typedef void  (*CkCallFnPtr) (void *msg, void *obj);
/** Register this entry point, with this call function and flags.
    Returns the entry point's index in the _entryTable. */
extern int CkRegisterEp(const char *name, CkCallFnPtr call, int msgIdx,
                        int chareIdx, int ck_ep_flags);
extern int CkRegisterEpTemplated(const char *name, CkCallFnPtr call, int msgIdx,
                                 int chareIdx, int ck_ep_flags);

/** Register this type of chare (group, or array), with this size.
    Returns the Chare's index in the _chareTable. */
extern int CkRegisterChare(const char *name, size_t dataSz, ChareType chareType);
/** Register number of array dimensions for this chare array*/
extern void CkRegisterArrayDimensions(int chareIndex, int ndims);
/** Register this chare as internal to Charm++.*/
extern void CkRegisterChareInCharm(int chareIndex);
/** Register this chare as a mainchare, with this entry point as its constructor.*/
extern int CkRegisterMainChare(int chareIndex, int epIndex);
/** Register a default constructor for this chare.*/
extern void CkRegisterDefaultCtor(int chareIndex, int ctorEpIndex);
/** Register a migration constructor for this chare.*/
extern void CkRegisterMigCtor(int chareIndex, int ctorEpIndex);
/** Indicate whether this group is an IrrGroup. */
extern void CkRegisterGroupIrr(int chareIndex,int isIrr);
/** Register the chare baseIdx as a base class of the chare derivedIdx. */
extern void CkRegisterBase(int derivedIdx, int baseIdx);
/** Sets the ifNotThere policy of an EP **/
extern void CkRegisterIfNotThere(int epIdx, CkArray_IfNotThere policy);
#if CMK_CHARM4PY
extern void CkRegisterMainChareExt(const char *s, int numEntryMethods, int *chareIdx, int *startEpIdx);
extern void CkRegisterGroupExt(const char *s, int numEntryMethods, int *chareIdx, int *startEpIdx);
extern void CkRegisterSectionManagerExt(const char *s, int numEntryMethods, int *chareIdx, int *startEpIdx);
extern void CkRegisterArrayMapExt(const char *s, int numEntryMethods, int *chareIdx, int *startEpIdx);
extern void CkRegisterArrayExt(const char *s, int numEntryMethods, int *chareIdx, int *startEpIdx);
extern void CkRegisterReadonlyExt(const char *name, const char *type, size_t msgSize, char *msg);
#endif

/** This function pup's a global variable.*/
typedef void (*CkPupReadonlyFnPtr)(void *pup_er);
/** Register this readonly global variable.*/
extern void CkRegisterReadonly(const char *name,const char *type,
	size_t size, void *ptr,CkPupReadonlyFnPtr pup_fn);
/** Register this readonly message.*/
extern void CkRegisterReadonlyMsg(const char *name,const char *type,
	void** pMsg);

/** A "marshall unpack" function: pups out parameters and calls a method. */
typedef int (*CkMarshallUnpackFn)(char *marshall_buf,void *object);
/** Register this marshall unpack function with this entry point.*/
extern void CkRegisterMarshallUnpackFn(int epIndex,CkMarshallUnpackFn m);
/** Lookup the marshall unpack function, if any, for this entry point.*/
extern CkMarshallUnpackFn CkLookupMarshallUnpackFn(int epIndex);

#ifdef __cplusplus
/** A "message pup" function: pups message data for debugger display. */
typedef void (*CkMessagePupFn)(PUP::er &p,void *userMessage);
/** Register this message pup function with this entry point.*/
extern void CkRegisterMessagePupFn(int epIndex,CkMessagePupFn m);
#endif
/*@}*/

/*********************************************************/
/**
\addtogroup Ck
\brief Charm Kernel--the groups and chares level of Charm++.

These routines are implemented in ck.C.
*/
/*@{*/

typedef struct {
  int   onPE;
  void* objPtr;
} CkChareID;

typedef struct _ckGroupID{
  int idx;		/* pe(processor number) is removed from the structure */
#ifdef __cplusplus
  inline void pup(PUP::er &p) {  p|idx; }
  inline bool isZero(void) const { return (idx==0); }
  inline void setZero(void) { idx=0; }
  inline int operator==(const struct _ckGroupID& gid) const {
    return (gid.idx==idx);
  }
  inline int operator<(const struct _ckGroupID& gid) const {
    return (gid.idx<idx);
  }
#endif
} CkGroupID;

typedef CkGroupID CkNodeGroupID;

/******************************************************************************
 *
 * Object Creation Calls
 *
 *****************************************************************************/
#ifdef __cplusplus
class envelope;
#else
typedef struct envelope envelope;
#endif
extern void CkCreateChare(int chareIdx, int constructorIdx, void *msg,
                          CkChareID *vid, int destPE);
extern CkGroupID CkCreateGroup(int chareIdx, int constructorIdx, void *msg);
extern CkGroupID CkCreateNodeGroup(int chareIdx, int constructorIdx, void *msg);
extern void CkCreateLocalGroup(CkGroupID groupID, int constructorIdx, envelope *env);
extern void CkCreateLocalNodeGroup(CkGroupID groupID, int constructorIdx, envelope *env);

#if CMK_CHARM4PY
extern int CkCreateGroupExt(int cIdx, int eIdx, int num_bufs, char **bufs, int *buf_sizes);
extern int CkCreateArrayExt(int cIdx, int ndims, int *dims, int eIdx, int num_bufs, char **bufs, int *buf_sizes, int map_gid, char useAtSync);
extern void CkInsertArrayExt(int aid, int ndims, int *index, int epIdx, int onPE, int num_bufs, char **bufs, int *buf_sizes, char useAtSync);
extern void CkArrayDoneInsertingExt(int aid);
extern void CkMigrateExt(int aid, int ndims, int *index, int toPe);
#endif


/******************************************************************************

 This set of message type (mtype) constants
 defines the basic class of charm++ message.
 
 It is very questionable whether bizarre stuff like
 "ExitMsg", "StatMsg", "ROMsgMsg" should actually
 share the envelope with regular user messages;
 but it doesn't waste any space so it's probably OK.

 These were formerly in envelope.h

 *****************************************************************************/
/*** WARNING!!!! The following enum is linked to charmdebug finals in MsgInfo.java.
 *   Make sure the two remain synchronized if changing this one.
 ***/
typedef enum {
  NewChareMsg          =1,               // Singleton chare creation message
  NewVChareMsg         =2,               // Singleton virtual chare creation message
  BocInitMsg           =3,               // Group creation message
  ForChareMsg          =4,               // Singleton chare entry method message (non creation)
  ForBocMsg            =5,               // Group entry method message (non creation)
  ForVidMsg            =6,               // Singleton virtual chare entry method message (non creation)
  FillVidMsg           =7,               // Message sent to fill a VidBlock on a virtual chare PE
  DeleteVidMsg         =8,               // Message sent to delete a VidBlock on a virtual chare PE
  RODataMsg            =9,               // Readonly Data Message (for user declared readonly variables)
  ROMsgMsg             =10,              // Readonly message Message (for user declared readonly messages)
  ROPeerCompletionMsg  =11,              // Message to signal completion of RO Data transfer using Zcpy API
                                         // ^(used by child nodes to signal completion to their parent node in the bcast spanning tree)
  ROChildCompletionMsg =12,              // Message to signal completion of RO Data transfer using Zcpy API
                                         // ^(used by peer nodes to signal completion to the 0th node)
  StartExitMsg         =13,              // Exit sequence trigger message
  ExitMsg              =14,              // Exit sequence trigger message using user registered exit function
  ReqStatMsg           =15,              // Request stats and warnings message
  StatMsg              =16,              // Stats data message (Reduction)
  StatDoneMsg          =17,              // Signal completion of stats reduction (Broadcast)
  NodeBocInitMsg       =18,              // Nodegroup creation message
  ForNodeBocMsg        =19,              // Nodegroup entry method message (non creation)
  ArrayEltInitMsg      =20,              // Array Element Initialization message
  ForArrayEltMsg       =21,              // Array Element entry method message
  ForIDedObjMsg        =22,
  BocBcastMsg          =23,              // A broadcast to a group which will be sent to each node
  ArrayBcastMsg        =24,              // A broadcast to an array which will be sent to each node
  ArrayBcastFwdMsg     =25,              // A bcast which arrived on node and must be forwarded to local array elements
#if CMK_LOCKLESS_QUEUE
  WarnMsg              =26,              // Warning data message (Reduction)
  WarnDoneMsg          =27,              // Signal completion of warnings reduction (Broadcast)
  LAST_CK_ENVELOPE_TYPE =28              // Used for error-checking
#else
  LAST_CK_ENVELOPE_TYPE =26              // Used for error-checking
#endif
} CkEnvelopeType;



/******************************************************************************
 *
 * Asynchronous Remote Method Invocation Calls
 *
 *****************************************************************************/

#define CK_MSG_INLINE      	0x1
#define CK_MSG_IMMEDIATE   	0x2
#define CK_MSG_EXPEDITED	0x4
#define CK_MSG_LB_NOTRACE	0x8   /* load balancer doesn't trace */

#ifdef __cplusplus
#define CK_MSGOPTIONAL =0
#else
#define CK_MSGOPTIONAL
#endif

extern void CkSendMsg(int entryIndex, void *msg, const CkChareID *chare, int opts CK_MSGOPTIONAL);
extern void CkSendMsgBranch(int eIdx, void *msg, int destPE, CkGroupID gID, int opts CK_MSGOPTIONAL);
extern void CkSendMsgInline(int entryIndex, void *msg, const CkChareID *chare, int opts CK_MSGOPTIONAL);
extern void CkSendMsgBranchInline(int eIdx, void *msg, int destPE, CkGroupID gID, int opts CK_MSGOPTIONAL);
extern void CkSendMsgBranchMulti(int eIdx, void *msg, CkGroupID gID, int npes, const int *pes, int opts CK_MSGOPTIONAL);
extern void CkSendMsgBranchGroup(int eIdx,void *msg,CkGroupID gID,CmiGroup grp, int opts CK_MSGOPTIONAL);
extern void CkSendMsgNodeBranch(int eIdx, void *msg, int destNode, CkGroupID gID, int opts CK_MSGOPTIONAL);
extern void CkSendMsgNodeBranchInline(int eIdx, void *msg, int destNode, CkGroupID gID, int opts CK_MSGOPTIONAL);
extern void CkSendMsgNodeBranchPe(int eIdx, void *msg, int destPe, CkGroupID gID, int opts CK_MSGOPTIONAL);
extern void CkSendMsgNodeBranchMulti(int eIdx, void *msg, CkGroupID gID, int npes, const int *nodes, int opts CK_MSGOPTIONAL);
extern void CkBroadcastWithinNode(int eIdx, void *msg, CkGroupID gID, int opts CK_MSGOPTIONAL);
extern void CkBroadcastMsgBranch(int eIdx, void *msg, CkGroupID gID, int opts CK_MSGOPTIONAL);
extern void CkBroadcastMsgNodeBranch(int eIdx, void *msg, CkGroupID gID, int opts CK_MSGOPTIONAL);

extern int  CkChareMsgPrep(int eIdx, void *msg,const CkChareID *pCid);
extern void CkGroupMsgPrep(int eIdx, void *msg, CkGroupID gID);
extern void CkNodeGroupMsgPrep(int eIdx, void *msg, CkGroupID gID);

extern void CkSetRefNum(void *msg, CMK_REFNUM_TYPE ref);
extern CMK_REFNUM_TYPE  CkGetRefNum(void *msg);
extern int  CkGetSrcPe(void *msg);
extern int  CkGetSrcNode(void *msg);

extern void CkDeliverMessageFree(int epIdx,void *msg,void *object);
extern void CkDeliverMessageReadonly(int epIdx,const void *msg,void *object);

extern void *CkLocalBranch(CkGroupID gID);
extern void *CkLocalBranchOther(CkGroupID gID, int rank);
extern void *CkLocalNodeBranch(CkGroupID gID);
extern void *CkLocalChare(const CkChareID *chare);

extern void CkArrayManagerDeliver(int onPe,void *msg, int opts CK_MSGOPTIONAL);

#if CMK_CHARM4PY
/// Send msg to chare with ID (onPe,objPtr) to entry method 'epIdx'
extern void CkChareExtSend(int onPE, void *objPtr, int epIdx, char *msg, int msgSize);
/// Send msg to chare copying data into CkMessage from multiple input buffers
extern void CkChareExtSend_multi(int onPE, void *objPtr, int epIdx, int num_bufs, char **bufs, int *buf_sizes);
/// Send msg to group with ID 'gid'. if pe == -1, msg will be broadcasted, else
/// it will go to the group instance in that PE
extern void CkGroupExtSend(int gid, int npes, const int *pes, int epIdx, char *msg, int msgSize);
/// Send msg to group copying data into CkMessage from multiple input buffers
extern void CkGroupExtSend_multi(int gid, int npes, const int *pes, int epIdx, int num_bufs, char **bufs, int *buf_sizes);
extern void CkForwardMulticastMsg(int gid, int num_children, const int *children);
/// Send msg to array with ID 'aid'. idx is index of destination and ndims the number
/// of dimensions of the index. If ndims <= 0, msg will be broadcasted to all array elements
extern void CkArrayExtSend(int aid, int *idx, int ndims, int epIdx, char *msg, int msgSize);
/// Send msg to array copying data into CkMessage from multiple input buffers
extern void CkArrayExtSend_multi(int aid, int *idx, int ndims, int epIdx, int num_bufs, char **bufs, int *buf_sizes);
#endif

/*@}*/

typedef CMK_REFNUM_TYPE CkFutureID;
typedef struct _ckFuture {
  CkFutureID id;
  int        pe;
#ifdef __cplusplus
  public:
    void pup(PUP::er &p) { p(id); p(pe); }

    bool operator==(const _ckFuture& other) const {
      return this->id == other.id && this->pe == other.pe;
    }
#endif
} CkFuture;

/******************************************************************************
 *
 * Semaphore calls
 *
 *****************************************************************************/

typedef struct _ckSemaID {
  int pe;
  int idx;
#ifdef __cplusplus
  public:
    void pup(PUP::er &p) { p(pe); p(idx); }
#endif
} CkSemaID;

extern CkSemaID CkSemaCreate(void);
extern void *CkSemaWait(CkSemaID id);
extern void CkSemaWaitN(CkSemaID id, int n, void *marray[]);
extern void CkSemaSignal(CkSemaID id, void *m);
extern void CkSemaDestroy(CkSemaID id);
/*@}*/


/******************************************************************************
 *
 * Quiescence Calls
 *
 *****************************************************************************/
/**
\addtogroup CkQD
\brief Quiescence Detection--a way to tell when nothing is happening.

These routines are implemented in qd.C and waitqd.C.
*/
/*@{*/

/** When quiescence occurs, send a message to this entry point of this Chare. */
extern void CkStartQD(int eIdx,const CkChareID *chare);
/** Block until quiescence occurs. */
extern void CkWaitQD(void);
/*@}*/

/******************************************************************************
 *
 * Miscellaneous Calls
 *
 *****************************************************************************/

extern int CkMessageToEpIdx(void *msg);
extern void CkPrintEntryMethod(int epIdx);
extern void CkPrintChareName(int chareIdx);
extern void CkSummary_MarkEvent(int);
extern void CkSummary_StartPhase(int);
extern int CkDisableTracing(int epIdx);
extern void CkEnableTracing(int epIdx);
extern void CkCallWhenIdle(int epIdx, void* obj);

#ifdef __cplusplus
}
#endif
#endif
