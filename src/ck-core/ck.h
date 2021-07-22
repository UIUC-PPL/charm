#ifndef _CK_H_
#define _CK_H_

#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "qd.h"
#include "charm++.h"
#include "envelope.h"
#include "register.h"
#include "stats.h"
#include "ckfutures.h"
#include "TopoManager.h"

#if CMK_ERROR_CHECKING
#define _CHECK_VALID(p, msg) do {if((p)==0){CkAbort(msg);}} while(0)
#else
#define _CHECK_VALID(p, msg) do { } while(0)
#endif

// Flag that tells the system if we are replaying using Record/Replay
extern int _replaySystem;

#if CMK_CHARMDEBUG
int ConverseDeliver(int pe);
inline void _CldEnqueue(int pe, void *msg, int infofn) {
  if (!ConverseDeliver(pe)) {
    CmiFree(msg);
    return;
  }
  envelope *env = (envelope *)msg;
  // Store source information to handle acknowledgements on completion
  if(CMI_IS_ZC(msg))
    CkRdmaPrepareZCMsg(env, CkNodeOf(pe));
  CldEnqueue(pe, msg, infofn);
}
inline void _CldEnqueueWithinNode(void *msg, int infofn) {
  if (!ConverseDeliver(-1)) {
    CmiFree(msg);
    return;
  }
  CldEnqueueWithinNode(msg, infofn);
}
inline void _CldEnqueueMulti(int npes, const int *pes, void *msg, int infofn) {
  if (!ConverseDeliver(-1)) {
    CmiFree(msg);
    return;
  }
  CldEnqueueMulti(npes, pes, msg, infofn);
}
inline void _CldEnqueueGroup(CmiGroup grp, void *msg, int infofn) {
  if (!ConverseDeliver(-1)) {
    CmiFree(msg);
    return;
  }
  CldEnqueueGroup(grp, msg, infofn);
}
inline void _CldNodeEnqueue(int node, void *msg, int infofn) {
  if (!ConverseDeliver(node)) {
    CmiFree(msg);
    return;
  }
  envelope *env = (envelope *)msg;
  // Store source information to handle acknowledgements on completion
  if(CMI_IS_ZC(msg))
    CkRdmaPrepareZCMsg(env, node);
  CldNodeEnqueue(node, msg, infofn);
}
#else

inline void _CldEnqueue(int pe, void *msg, int infofn) {
  envelope *env = (envelope *)msg;
  // Store source information to handle acknowledgements on completion
  if(CMI_IS_ZC(msg))
    CkRdmaPrepareZCMsg(env, CkNodeOf(pe));
  CldEnqueue(pe, msg, infofn);
}

inline void _CldNodeEnqueue(int node, void *msg, int infofn) {
  envelope *env = (envelope *)msg;
  // Store source information to handle acknowledgements on completion
  if(CMI_IS_ZC(msg))
    CkRdmaPrepareZCMsg(env, node);
  CldNodeEnqueue(node, msg, infofn);
}
#define _CldEnqueueMulti      CldEnqueueMulti
#define _CldEnqueueGroup      CldEnqueueGroup
#define _CldEnqueueWithinNode CldEnqueueWithinNode
#endif

#ifndef CMK_CHARE_USE_PTR
CkpvExtern(std::vector<void *>, chare_objs);
#endif

#include <unordered_map>
typedef std::unordered_map<CmiUInt8, ArrayElement*> ArrayObjMap;
CkpvExtern(ArrayObjMap, array_objs);

/// A set of "Virtual ChareID"'s
class VidBlock {
    enum VidState : uint8_t {FILLED, UNFILLED};
    VidState state;
    PtrQ *msgQ;
    CkChareID actualID;
    void msgDeliver(envelope *env) {
        // This was causing sync entry methods not to return properly in some cases
        //env->setSrcPe(CkMyPe());
        env->setMsgtype(ForChareMsg);
        env->setObjPtr(actualID.objPtr);
        _CldEnqueue(actualID.onPE, env, _infoIdx);
        CpvAccess(_qd)->create();      
    }
  public:
    VidBlock() ;
    void send(envelope *env) {
      if(state==UNFILLED) {
        msgQ->enq((void *)env);
      } else {
        msgDeliver(env);
      }
    }
    void fill(int onPE, void *oPtr) {
      state = FILLED;
      actualID.onPE = onPE;
      actualID.objPtr = oPtr;
      envelope *env;
      while(NULL!=(env=(envelope*)msgQ->deq())) {
        msgDeliver(env);
      }
      delete msgQ; msgQ=0;
    }
    void *getLocalChare(void) {
      if (state==FILLED && actualID.onPE==CkMyPe()) 
          return actualID.objPtr;
      return NULL;
    }
    void *getLocalChareObj(void) {   
         // returns actual object, different when CMK_CHARE_USE_PTR is false
      if (state==FILLED && actualID.onPE==CkMyPe()) 
#ifdef CMK_CHARE_USE_PTR
          return actualID.objPtr;
#else
          return CkpvAccess(chare_objs)[(CmiIntPtr)actualID.objPtr];
#endif
      return NULL;
    }
    void pup(PUP::er &p) {
#ifndef CMK_CHARE_USE_PTR
      int s = 0;
      if (!p.isUnpacking()) s = state-FILLED;
      p|s;
      if (p.isUnpacking()) state = (VidState)(FILLED+s);
      if (p.isUnpacking()) msgQ = NULL;    // fixme
      p|actualID;
#endif
    }
};

class CkCoreState;

/// Message watcher: for record/replay support
class CkMessageWatcher {
protected:
  FILE *f;
  CkMessageWatcher *next;
public:
    CkMessageWatcher() : f(NULL), next(NULL) { }
    virtual ~CkMessageWatcher();
	/**
	 * This message is about to be processed by Charm.
	 * If this function returns false, the message will not be processed.
	 * The message is processed by the watcher starting from the innermost one
	 * up to the outermost
	 */
#define PROCESS_MACRO(name,type) inline bool process##name(type *input,CkCoreState *ck) { \
  bool result = true; \
    if (next != NULL) result &= next->process##name(input, ck); \
    result &= process(input, ck); \
    return result; \
  }

    PROCESS_MACRO(Message,envelope*);
    PROCESS_MACRO(Thread,CthThreadToken);
    PROCESS_MACRO(LBMessage,LBMigrateMsg*);

#undef PROCESS_MACRO
protected:
    /** These are used internally by this class to call the correct subclass method */
	virtual bool process(envelope **env,CkCoreState *ck) =0;
	virtual bool process(CthThreadToken *token, CkCoreState *ck) {return true;}
	virtual bool process(LBMigrateMsg **msg, CkCoreState *ck) {return true;}
public:
    inline void setNext(CkMessageWatcher *w) { next = w; }
};

/// All the state that's useful to have on the receive side in the Charm Core (ck.C)
class CkCoreState {
	GroupTable *groupTable;
	QdState *qd;
public:
	CkMessageWatcher *watcher;
	/** Adds an extra watcher (which wrap the previously existing one) */
	inline void addWatcher(CkMessageWatcher *w) {
	  w->setNext(watcher);
	  watcher = w;
	}
	
	CkCoreState() 
		:groupTable(CkpvAccess(_groupTable)),
		 qd(CpvAccess(_qd)) { watcher=NULL; }
	~CkCoreState() { delete watcher;}

	inline GroupTable *getGroupTable() const {
 		return groupTable;
	}
	inline IrrGroup *localBranch(CkGroupID gID) const {
		return groupTable->find(gID).getObj();
	}

	inline QdState *getQD() {return qd;}
	// when in interrupt based net version, use the extra copy
 	// of qd when inside an immediate handler function.
	inline void process(int n=1) {
	  if (CmiImmIsRunning())
	    CpvAccessOther(_qd, 1)->process(n);
	  else
	    qd->process(n);
	}
	inline void create(int n=1) {
	  if (CmiImmIsRunning())
	    CpvAccessOther(_qd, 1)->create(n);
	  else
	    qd->create(n);
	}
};

CkpvExtern(CkCoreState *, _coreState);

#if CMK_LBDB_ON
CkLocRec *CkActiveLocRec(void);
#endif // CMK_LBDB_ON

void CpdHandleLBMessage(LBMigrateMsg **msg);
void CkMessageWatcherInit(char **argv,CkCoreState *ck);

extern void _processHandler(void *converseMsg,CkCoreState *ck);
extern void _processBocInitMsg(CkCoreState *ck,envelope *msg);
extern void _processNodeBocInitMsg(CkCoreState *ck,envelope *msg);
extern void _infoFn(void *msg, CldPackFn *pfn, int *len,
                    int *queueing, int *priobits, UInt **prioptr);
extern void CkCreateLocalGroup(CkGroupID groupID, int eIdx, envelope *env);
extern void CkCreateLocalNodeGroup(CkGroupID groupID, int eIdx, envelope *env);
extern void _createGroup(CkGroupID groupID, envelope *env);
extern void _createNodeGroup(CkGroupID groupID, envelope *env);
extern int _getGroupIdx(int,int,int);
static inline IrrGroup *_lookupGroupAndBufferIfNotThere(const CkCoreState *ck, const envelope *env,const CkGroupID &groupID);
extern IrrGroup* _getCkLocalBranchFromGroupID(CkGroupID &gID);

void QdCreate(int n);
void QdProcess(int n);
#endif
