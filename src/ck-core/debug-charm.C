/*****************************************************************************
 * A few useful built-in CPD and CCS handlers.
 *****************************************************************************/

#include "converse.h"

#include <errno.h>
#include <string.h>

#ifndef __STDC_FORMAT_MACROS
# define __STDC_FORMAT_MACROS
#endif
#ifndef __STDC_LIMIT_MACROS
# define __STDC_LIMIT_MACROS
#endif
#include <inttypes.h>
#include <sys/stat.h>		// for chmod

#include "ckhashtable.h"
#include "conv-ccs.h"
#include "debug-charm.h"
#include "sockRoutines.h"
#include "charm.h"
#include "middle.h"
#include "cklists.h"
#include "register.h"
//#include "queueing.h"
#include <unistd.h>

#include "ck.h"
CkpvDeclare(DebugEntryTable, _debugEntryTable);

#if CMK_CHARMDEBUG && CMK_CCS_AVAILABLE && !defined(_WIN32)

CkpvDeclare(int, skipBreakpoint); /* This is a counter of how many breakpoints we should skip */
CpdPersistentChecker persistentCheckerUselessClass;

void resetAllCRC();
void checkAllCRC(int report);

typedef struct DebugRecursiveEntry {
  int previousChareID;
  int alreadyUserCode;
  char *memoryBackup;
  void *obj;
  void *msg;
} DebugRecursiveEntry;

CkQ<DebugRecursiveEntry> _debugData;

void *CpdGetCurrentObject() { return _debugData.peek().obj; }
void *CpdGetCurrentMsg() { return _debugData.peek().msg; }

extern int cpdInSystem;
int CpdInUserCode() {return cpdInSystem==0 && _debugData.length()>0 && _debugData.peek().alreadyUserCode==1;}

// Function called right before an entry method
void CpdBeforeEp(int ep, void *obj, void *msg) {
#if CMK_CHARMDEBUG
  if (cmiArgDebugFlag && CmiMyRank()==0) {
    DebugRecursiveEntry entry;
    entry.previousChareID = setMemoryChareIDFromPtr(obj);
    entry.alreadyUserCode = _entryTable[ep]->inCharm ? 0 : 1;
    entry.memoryBackup = NULL;
    entry.obj = obj;
    if (msg != NULL) {
      entry.msg = CkReferenceMsg(msg);
    }
    else entry.msg = NULL;
    _debugData.push(entry);
    setMemoryStatus(entry.alreadyUserCode);
    //if (CkpvAccess(_debugEntryTable)[ep].isBreakpoint) printf("CpdBeforeEp breakpointed %d\n",ep);
    memoryBackup = &_debugData.peek().memoryBackup;
    if (!_entryTable[ep]->inCharm) {
      CpdResetMemory();
      CpdSystemExit();
    }
    std::vector<DebugPersistentCheck> &preExecutes = CkpvAccess(_debugEntryTable)[ep].preProcess;
    for (int i=0; i<preExecutes.size(); ++i) {
      preExecutes[i].object->cpdCheck(preExecutes[i].msg);
    }
  }
#endif
}

// Function called right after an entry method
void CpdAfterEp(int ep) {
#if CMK_CHARMDEBUG
  if (cmiArgDebugFlag && CmiMyRank()==0) {
    DebugRecursiveEntry entry = _debugData.peek();
    std::vector<DebugPersistentCheck> &postExecutes = CkpvAccess(_debugEntryTable)[ep].postProcess;
    for (int i=0; i<postExecutes.size(); ++i) {
      postExecutes[i].object->cpdCheck(postExecutes[i].msg);
    }
    memoryBackup = &entry.memoryBackup;
    if (!_entryTable[ep]->inCharm) {
      CpdSystemEnter();
      CpdCheckMemory();
    }
    if (entry.msg != NULL) CmiFree(UsrToEnv(entry.msg));
    setMemoryChareID(entry.previousChareID);
    setMemoryStatus(entry.alreadyUserCode);
    _debugData.deq();
  }
#endif
}

/************ Array Element CPD Lists ****************/

/**
  Count array elements going by until they reach this
  range (lo to hi), then start passing them to dest.
*/
template <class T>
class CkArrayElementRangeIterator : public CkLocIterator {
private:
   T *dest;
   CkArray *mgr;
   int cur,lo,hi;
public:
   CkArrayElementRangeIterator(T *dest_,int l,int h)
   	:dest(dest_),mgr(0),cur(0),lo(l),hi(h) {}

  /** Called to iterate only on a specific array manager.
      Returs the number of objects it iterate on.
  */
  int iterate(int start, CkArray *m) {
    cur = start;
    mgr = m;
    mgr->getLocMgr()->iterate(*this);
    cur -= start;
    return cur;
  }

   /** Call add for every in-range array element on this processor */
   void iterate(void)
   { /* Walk the groupTable for arrays (FIXME: get rid of _groupIDTable) */
     int numGroups=CkpvAccess(_groupIDTable)->size();
     for(int i=0;i<numGroups;i++) {
        IrrGroup *obj = CkpvAccess(_groupTable)->find((*CkpvAccess(_groupIDTable))[i]).getObj();
	if (obj->isArrMgr())
	{ /* This is an array manager: examine its array elements */
	  mgr=(CkArray *)obj;
	  mgr->getLocMgr()->iterate(*this);
	}
     }
   }

   // Called by location manager's iterate function
   virtual void addLocation (CkLocation &loc)
   {
     if (cur>=lo && cur<hi)
     { /* This element is in our range-- look it up */
       dest->add(cur,mgr->lookup(loc.getIndex()),mgr->getGroupID().idx);
     }
     cur++;
   }

   // Return the number of total array elements seen so far.
   int getCount(void) {return cur;}
};

/**
  Count charm++ objects going by until they reach this
  range (lo to hi), then start passing them to dest.
*/
template <class T>
class CkObjectRangeIterator {
private:
   T *dest;
   int cur,lo,hi;
public:
   CkObjectRangeIterator(T *dest_,int l,int h)
   	:dest(dest_),cur(0),lo(l),hi(h) {}

   /** Call add for every in-range array element on this processor */
   void iterate(void)
   { /* Walk the groupTable for arrays (FIXME: get rid of _groupIDTable) */
     int numGroups=CkpvAccess(_groupIDTable)->size();
     for(int i=0;i<numGroups;i++) {
       CkGroupID groupID = (*CkpvAccess(_groupIDTable))[i];
        IrrGroup *obj = CkpvAccess(_groupTable)->find(groupID).getObj();
	/*if (obj->isArrMgr())
	{ / * This is an array manager: examine its array elements * /
	  CkArray *mgr=(CkArray *)obj;
          CkArrayElementRangeIterator<T> ait(dest,lo,hi);
	  ait.iterate(cur, mgr);
          cur+=ait.getCount();
	} else {*/
          dest->add(cur,obj,groupID.idx);
          cur++;
        //}
     }
   }

   // Return the number of total array elements seen so far.
   int getCount(void) {return cur;}
};

class ignoreAdd {
public: void add(int cur,Chare *elt,int group) {}
};

/** Examine all the objects on the server returning the name */
class CpdList_objectNames : public CpdListAccessor {
  PUP::er *pp; // Only used while inside pup routine.
  int curGroup;
public:
  virtual const char * getPath(void) const {return "charm/objectNames";}
  virtual size_t getLength(void) const {
    CkObjectRangeIterator<ignoreAdd> it(0,0,0);
    it.iterate();
    return it.getCount();
  }
  virtual void pup(PUP::er &p, CpdListItemsRequest &req) {
    pp=&p;
    CkObjectRangeIterator<CpdList_objectNames> it(this,req.lo,req.hi);
    it.iterate(); // calls "add" for in-range elements
  }
  void add(int cur, Chare *obj, int group) {
    PUP::er &p=*pp;
    beginItem(p,cur);
    p.comment("id");
    char *n = (char*)malloc(30);
    int s=obj->ckDebugChareID(n, 30);
    CkAssert(s > 0);
    p(n,s);
    free(n);
    PUPn(group);
    p.comment("name");
    n=obj->ckDebugChareName();
    p(n,strlen(n));
    free(n);
  }
};

/** Examine a single object identified by the id passed in the request and
    return its type and memory data */
class CpdList_object : public CpdListAccessor {
  PUP::er *pp; //Only used while inside pup routine.
  CpdListItemsRequest *reqq; // Only used while inside pup routine.
public:
  virtual const char * getPath(void) const {return "charm/object";}
  virtual size_t getLength(void) const {
    CkObjectRangeIterator<ignoreAdd> it(0,0,0);
    it.iterate();
    return it.getCount();
  }
  virtual void pup(PUP::er &p, CpdListItemsRequest &req) {
    pp=&p;
    reqq=&req;
    CkObjectRangeIterator<CpdList_object> it(this,req.lo,req.hi);
    it.iterate(); // calls "add" for in-range elements;
  }
  void add(int cur, Chare *obj, int group) {
    PUP::er &p=*pp;
    CpdListItemsRequest &req=*reqq;
    char *n = (char *)malloc(30);
    int s=obj->ckDebugChareID(n, 30);
    CkAssert(s > 0);
    if (req.extraLen == s && memcmp(req.extra, n, s) == 0) {
      // the object match, found!
      beginItem(p,cur);
      int type = obj->ckGetChareType();
      p.comment("type");
      const char *t = _chareTable[type]->name;
      p((char*)t,strlen(t));
      p.comment("value");
      int size = _chareTable[type]->size;
      p((char*)obj,size);
    }
  }
};

/** Coarse: examine array element names */
class CpdList_arrayElementNames : public CpdListAccessor {
  PUP::er *pp; // Only used while inside pup routine.
public:
  virtual const char * getPath(void) const {return "charm/arrayElementNames";}
  virtual size_t getLength(void) const {
    CkArrayElementRangeIterator<ignoreAdd> it(0,0,0);
    it.iterate();
    return it.getCount();
  }
  virtual void pup(PUP::er &p, CpdListItemsRequest &req) {
    pp=&p;
    CkArrayElementRangeIterator<CpdList_arrayElementNames> it(this,req.lo,req.hi);
    it.iterate(); // calls "add" for in-range elements
  }
  void add(int cur,Chare *e,int group)
  { // Just grab the name and nothing else:
    ArrayElement *elt = (ArrayElement*)e;
         PUP::er &p=*pp;
	 beginItem(p,cur);
         p.comment("name");
	 char *n=elt->ckDebugChareName();
	 p(n,strlen(n));
	 free(n);
  }
};

/** Detailed: examine array element data */
class CpdList_arrayElements : public CpdListAccessor {
  PUP::er *pp; // Only used while inside pup routine.
public:
  virtual const char * getPath(void) const {return "charm/arrayElements";}
  virtual size_t getLength(void) const {
    CkArrayElementRangeIterator<ignoreAdd> it(0,0,0);
    it.iterate();
    return it.getCount();
  }
  virtual void pup(PUP::er &p, CpdListItemsRequest &req) {
    pp=&p;
    CkArrayElementRangeIterator<CpdList_arrayElements> it(this,req.lo,req.hi);
    it.iterate(); // calls "add" for in-range elements
  }
  void add(int cur, Chare *e, int group)
  { // Pup the element data
    ArrayElement *elt = (ArrayElement*)e;
    PUP::er &p=*pp;
    beginItem(p,cur);
    //elt->ckDebugPup(p);
    // Now ignore any pupper, just copy all the memory as raw data
    p.comment("name");
    char *n=elt->ckDebugChareName();
    p(n,strlen(n));
    free(n);
    int type = elt->ckGetChareType();
    p.comment("type");
    const char *t = _chareTable[type]->name;
    p((char*)t,strlen(t));
    p.comment("value");
    int size = _chareTable[type]->size;
    p((char*)elt,size);
  }
};
#if CMK_OFI
// EJB TODO the fix for this belongs elsewhere, but this is ok for now
#undef CMK_HAS_GET_MYADDRESS
#define CMK_HAS_GET_MYADDRESS 0
#endif
#if CMK_HAS_GET_MYADDRESS
#include <rpc/rpc.h>
#endif

size_t hostInfoLength(void *) {return 1;}

void hostInfo(void *itemIter, pup_er pp, CpdListItemsRequest *req) {
  PUP::er &p = *(PUP::er *)pp;
  struct sockaddr_in addr;
  CpdListBeginItem(pp, 0);
#if CMK_HAS_GET_MYADDRESS
  get_myaddress(&addr);
#else
  CmiAbort("CharmDebug: get_myaddress() does not work on this machine, are you missing "
           "the rpc/rpc.h header (usually found in the glibc-dev package)?");
#endif
  char *address = (char*)&addr.sin_addr.s_addr;
  PUPv(address, 4);
  int pid = getpid();
  PUPn(pid);
}

/************ Message CPD Lists ****************/
CkpvExtern(void *,debugQueue);

// Interpret data in a message in a user-friendly way.
//  Ignores most of the envelope fields used by CkPupMessage,
//  and instead concentrates on user data
void CpdPupMessage(PUP::er &p, void *msg)
{
  envelope *env=UsrToEnv(msg);
  //int wasPacked=env->isPacked();
  int size=env->getTotalsize();
  int prioBits=env->getPriobits();
  int from=env->getSrcPe();
  PUPn(from);
  //PUPn(wasPacked);
  PUPn(prioBits);
  int userSize=size-sizeof(envelope)-sizeof(int)*CkPriobitsToInts(prioBits);
  PUPn(userSize);
  int msgType = env->getMsgIdx();
  PUPn(msgType);
  int envType = env->getMsgtype();
  PUPn(envType);

  //p.synchronize(PUP::sync_last_system);

  int ep=CkMessageToEpIdx(msg);
  PUPn(ep);

  // Pup the information specific to this envelope type
  if (envType == ForArrayEltMsg || envType == ArrayEltInitMsg) {
    int arrID = env->getArrayMgr().idx;
    PUPn(arrID);
    CmiUInt8 id = env->getRecipientID();
    PUPn(id);
  } else if (envType == ForNodeBocMsg || envType == ForBocMsg) {
    int groupID = env->getGroupNum().idx;
    PUPn(groupID);
  } else if (envType == BocInitMsg || envType == NodeBocInitMsg) {
    int groupID = env->getGroupNum().idx;
    PUPn(groupID);
  } else if (envType == NewVChareMsg || envType == ForVidMsg || envType == FillVidMsg) {
    p.comment("ptr");
    void *ptr = env->getVidPtr();
    pup_pointer(&p, &ptr);
  } else if (envType == ForChareMsg) {
    p.comment("ptr");
    void *ptr = env->getObjPtr();
    pup_pointer(&p, &ptr);
  }
  
  /* user data */
  p.comment("data");
  p.synchronize(PUP::sync_begin_object);
  if (_entryTable[ep]->messagePup!=NULL)
    _entryTable[ep]->messagePup(p,msg);
  else
    CkMessage::ckDebugPup(p,msg);
  p.synchronize(PUP::sync_end_object);
}

struct ConditionalList {
  int count;
  int deliver;
  int msgs[1];
};
CkpvStaticDeclare(void *, lastBreakPointMsg);
CpvExtern(void*, conditionalQueue);
ConditionalList * conditionalShm = NULL;

//Cpd Lists for local and scheduler queues
class CpdList_localQ : public CpdListAccessor {

public:
  CpdList_localQ() {}
  virtual const char * getPath(void) const {return "converse/localqueue";}
  virtual size_t getLength(void) const {
    int x = CdsFifo_Length((CdsFifo)(CkpvAccess(debugQueue)));
    //CmiPrintf("*******Returning fifo length %d*********\n", x);
    //return CdsFifo_Length((CdsFifo)(CpvAccess(CmiLocalQueue)));
    if (CkpvAccess(lastBreakPointMsg) != NULL) x++;
    return x;
  }
  virtual void pup(PUP::er &p, CpdListItemsRequest &req) {
    int length;
    void ** messages;
    int curObj=0;
    void *msg;

    length = CdsFifo_Length((CdsFifo)(CpvAccess(conditionalQueue)));
    messages = CdsFifo_Enumerate(CpvAccess(conditionalQueue));
    for (curObj=-length; curObj<0; curObj++) {
      void *msg = messages[length+curObj];
      pupSingleMessage(p, curObj-1, msg);
    }
    delete[] messages;
    
    curObj = 0;
    length = CdsFifo_Length((CdsFifo)(CkpvAccess(debugQueue)));
    messages = CdsFifo_Enumerate(CkpvAccess(debugQueue));
    
    if (CkpvAccess(lastBreakPointMsg) != NULL) {
      beginItem(p, -1);
      envelope *env=(envelope *)UsrToEnv(CkpvAccess(lastBreakPointMsg));
      p.comment("name");
      char *type=(char*)"Breakpoint";
      p(type,strlen(type));
      p.comment("charmMsg");
      p.synchronize(PUP::sync_begin_object);
      CkUnpackMessage(&env);
      CpdPupMessage(p, EnvToUsr(env));
      p.synchronize(PUP::sync_end_object);
    }

    for(curObj=req.lo; curObj<req.hi; curObj++)
      if ((curObj>=0) && (curObj<length))
      {
        void *msg=messages[curObj]; /* converse message */
        pupSingleMessage(p, curObj, msg);
      }
    delete[] messages;

  }

  void pupSingleMessage(PUP::er &p, int curObj, void *msg) {
    beginItem(p,curObj);
    int isCharm=0;
    const char *type="Converse";
    p.comment("name");
    char name[128];
    if (msg == (void*)-1) {
      type="Sentinel";
      p((char*)type, strlen(type));
      return;
    }
    if (CmiGetHandler(msg)==_charmHandlerIdx) {isCharm=1; type="Local Charm";}
    if (CmiGetXHandler(msg)==_charmHandlerIdx) {isCharm=1; type="Network Charm";}
    if (curObj < 0) type="Conditional";
    snprintf(name,sizeof(name),"%s %d: %s (%d)","Message",curObj,type,CmiGetHandler(msg));
    p(name, strlen(name));

    if (isCharm)
    { /* charm message */
      p.comment("charmMsg");
      p.synchronize(PUP::sync_begin_object);
      envelope *env=(envelope *)msg;
      CkUnpackMessage(&env);
      //messages[curObj]=env;
      CpdPupMessage(p, EnvToUsr(env));
      //CkPupMessage(p, &messages[curObj], 0);
      p.synchronize(PUP::sync_end_object);
    }
  }
};

class CpdList_message : public CpdListAccessor {
  virtual const char * getPath(void) const {return "converse/message";}
  virtual size_t getLength(void) const {return 1;}
  virtual void pup(PUP::er &p, CpdListItemsRequest &req) {
    envelope *env = (envelope*)(((uint64_t)req.lo) + (((uint64_t)req.hi)<<32)+sizeof(CmiChunkHeader));
    beginItem(p, 0);
    const char *type="Converse";
    p.comment("name");
    char name[128];
    if (CmiGetHandler(env)==_charmHandlerIdx) {type="Local Charm";}
    if (CmiGetXHandler(env)==_charmHandlerIdx) {type="Network Charm";}
    snprintf(name,sizeof(name),"%s 0: %s (%d)","Message",type,CmiGetHandler(env));
    p(name, strlen(name));
    p.comment("charmMsg");
    p.synchronize(PUP::sync_begin_object);
    CpdPupMessage(p, EnvToUsr(env));
    p.synchronize(PUP::sync_end_object);
  }
};

static void CpdHandleMessage(void *queuedMsg, int msgNum) {
  if (_conditionalDelivery) {
    if (_conditionalDelivery==1) conditionalShm->msgs[conditionalShm->count++] = msgNum;
    CmiReference(queuedMsg);
    CdsFifo_Enqueue(CpvAccess(conditionalQueue),queuedMsg);
  }
  CmiHandleMessage(queuedMsg);
}

static void CpdDeliverMessageInt(int msgNum) {
  void *debugQ=CkpvAccess(debugQueue);

  CdsFifo_Enqueue(debugQ, (void*)(-1)); // Enqueue a guard
  for (int i=0; i<msgNum; ++i) CdsFifo_Enqueue(debugQ, CdsFifo_Dequeue(debugQ));

  CkpvAccess(skipBreakpoint) = 1;
  CpdHandleMessage((char *)CdsFifo_Dequeue(debugQ), msgNum);
  CkpvAccess(skipBreakpoint) = 0;

  void *m;
  while ((m=CdsFifo_Dequeue(debugQ)) != (void*)(-1)) CdsFifo_Enqueue(debugQ, m);  
}

void CpdDeliverMessage(char * msg) {
  int msgNum;
  sscanf(msg+CmiMsgHeaderSizeBytes, "%d", &msgNum);
  //CmiPrintf("received deliver request %d\n",msgNum);
  CpdDeliverMessageInt(msgNum);
}

static void CpdDeliverAllMessages(char *) {
  void *debugQ=CkpvAccess(debugQueue);
  CdsFifo_Enqueue(debugQ, (void*)(-1)); // Enqueue a guard
  CkpvAccess(skipBreakpoint) = 1;
  int msgNum = 0;
  void *m;
  while ((m=CdsFifo_Dequeue(debugQ)) != (void*)(-1)) {
    CpdHandleMessage(m, msgNum++);
  }
  CkpvAccess(skipBreakpoint) = 0;
}

void *CpdGetNextMessageConditional(CsdSchedulerState_t *s) {
  int len;
  void *msg;
  if ((msg=CdsFifo_Dequeue(s->localQ)) != NULL) return msg;
  CqsDequeue((Queue_struct*)s->schedQ,(void **)&msg);
  if (msg!=NULL) return msg;
  if (4 != read(conditionalPipe[0], &len, 4))
    CmiAbort("CpdGetNextMessageConditional: len read failed");
  msg = CmiAlloc(len);
  if (len != read(conditionalPipe[0], msg, len))
  {
    CmiFree(msg);
    CmiAbort("CpdGetNextMessageConditional: msg read failed");
  }
  return msg;
}

#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/shm.h>

void CpdDeliverSingleMessage ();

static pid_t CpdConditional_SetupComm() {
  int pipefd[2][2];
  if (pipe(pipefd[0]) == -1)
    CmiAbort("CpdConditional_SetupComm: parent to child pipe failed");
  if (pipe(pipefd[1]) == -1)
    CmiAbort("CpdConditional_SetupComm: child to parent pipe failed");
  
  if (conditionalShm == NULL) {
    struct shmid_ds dummy;
    int shmemid = shmget(IPC_PRIVATE, 1024*1024, IPC_CREAT | 0666);
    conditionalShm = (ConditionalList*)shmat(shmemid, NULL, 0);
    conditionalShm->count = 0;
    conditionalShm->deliver = 0;
    shmctl(shmemid, IPC_RMID, &dummy);
  }
  
  pid_t pid = fork();
  if (pid < 0)
    CmiAbort("CpdConditional_SetupComm: fork failed");
  else if (pid > 0) {
    int bytes;
    CmiPrintf("parent %d\n",pid);
    close(pipefd[0][0]);
    close(pipefd[1][1]);
    conditionalPipe[0] = pipefd[1][0];
    conditionalPipe[1] = pipefd[0][1];
    //CpdConditionalDeliveryScheduler(pipefd[1][0], pipefd[0][1]);
    if (4 != read(conditionalPipe[0], &bytes, 4))
      CmiAbort("CpdConditional_SetupComm: bytes read failed");
    char *buf = (char*)malloc(bytes);
    if (bytes != read(conditionalPipe[0], buf, bytes))
    {
      free(buf);
      CmiAbort("CpdConditional_SetupComm: buf read failed");
    }
    CcsSendReply(bytes,buf);
    free(buf);
    return pid;
  }

  //int volatile tmp=1;
  //while (tmp);
  printf("child\n");
  _conditionalDelivery = 1;
  close(pipefd[0][1]);
  close(pipefd[1][0]);
  conditionalPipe[0] = pipefd[0][0];
  conditionalPipe[1] = pipefd[1][1];
  CpdGetNextMessage = CpdGetNextMessageConditional;
  return 0;
}

void CpdEndConditionalDelivery(char *msg) {
  int msgNum;
  void *m;
  sscanf(msg+CmiMsgHeaderSizeBytes, "%d", &msgNum);
  printf("%d messages:\n",conditionalShm->count);
  for (int i=0; i<conditionalShm->count; ++i)
    printf("message delivered %d\n",conditionalShm->msgs[i]);
  conditionalShm->count = msgNum;
  shmdt((char*)conditionalShm);
  _exit(0);
}

void CpdEndConditionalDeliver_master() {
  close(conditionalPipe[0]);
  conditionalPipe[0] = 0;
  close(conditionalPipe[1]);
  conditionalPipe[1] = 0;
  wait(NULL);
  int i;
  // Check if we have to deliver unconditionally some messages
  if (conditionalShm->deliver > 0) {
    // Deliver the requested number of messages
    for (i=0; i < conditionalShm->deliver; ++i) {
      int msgNum = conditionalShm->msgs[i];
      if (msgNum == -1) CpdDeliverSingleMessage();
      else CpdDeliverMessageInt(msgNum);
    }
    // Move back the remaining messages accordingly
    for (i=conditionalShm->deliver; i < conditionalShm->count; ++i) {
      conditionalShm->msgs[i-conditionalShm->deliver] = conditionalShm->msgs[i];
    }
    conditionalShm->count -= conditionalShm->deliver;
    conditionalShm->deliver = 0;
    CmiMachineProgressImpl();
  }
  CkAssert(conditionalShm->count >= 0);
  if (conditionalShm->count == 0) {
    CcsSendReply(0,NULL);
    shmdt((char*)conditionalShm);
    conditionalShm = NULL;
    CkPrintf("Conditional delivery on %d concluded; normal mode resumed\n",CkMyPe());
  } else {
    if (CpdConditional_SetupComm()==0) {
      // We are in the child, deliver again the messages
      _conditionalDelivery = 2;
      printf("new child: redelivering %d messages\n",conditionalShm->count);
      for (int i=0; i<conditionalShm->count; ++i) {
        int msgNum = conditionalShm->msgs[i];
        if (msgNum == -1) CpdDeliverSingleMessage();
        else CpdDeliverMessageInt(msgNum);
      }
      _conditionalDelivery = 1;
      CcsSendReply(0, NULL);
    }
  }
}

void CpdDeliverMessageConditionally(char * msg) {
  int msgNum;
  void *m;
  sscanf(msg+CmiMsgHeaderSizeBytes, "%d", &msgNum);
  //CmiPrintf("received deliver request %d\n",msgNum);

  if (CpdConditional_SetupComm()==0) {
    if (msgNum == -1) CpdDeliverSingleMessage();
    else CpdDeliverMessageInt(msgNum);
  }
}

void CpdCommitConditionalDelivery(char * msg) {
  int msgNum;
  sscanf(msg+CmiMsgHeaderSizeBytes, "%d", &msgNum);\
  conditionalShm->deliver = msgNum;
  shmdt((char*)conditionalShm);
  _exit(0);
}

class CpdList_msgStack : public CpdListAccessor {
  virtual const char * getPath(void) const {return "charm/messageStack";}
  virtual size_t getLength(void) const {
    return _debugData.length();
  }
  virtual void pup(PUP::er &p, CpdListItemsRequest &req) {
    for (int i=0; i<_debugData.length(); ++i) {
      beginItem(p, i);
      void *obj = _debugData[i].obj;
      p.comment("obj");
      pup_pointer(&p, &obj);
      void *msg = _debugData[i].msg;
      p.comment("msg");
      pup_pointer(&p, &msg);
    }
  }
};

/****************** Breakpoints and other debug support **************/

typedef CkHashtableTslow<int,EntryInfo *> CpdBpFuncTable_t;

extern void CpdFreeze(void);
extern void CpdUnFreeze(void);
extern int  CpdIsFrozen(void);

CpvStaticDeclare(int, _debugMsg);
CpvStaticDeclare(int, _debugChare);

CpvStaticDeclare(CpdBpFuncTable_t *, breakPointEntryTable);

//CpvStaticDeclare(void *, lastBreakPointMsg);
CkpvStaticDeclare(void *, lastBreakPointObject);
CkpvStaticDeclare(int, lastBreakPointIndex);

void CpdBreakPointInit()
{
  CkpvInitialize(void *, lastBreakPointMsg);
  CkpvInitialize(void *, lastBreakPointObject);
  CkpvInitialize(int, lastBreakPointIndex);
  CpvInitialize(int, _debugMsg);
  CpvInitialize(int, _debugChare);
  CpvInitialize(CpdBpFuncTable_t *, breakPointEntryTable);
  CkpvAccess(lastBreakPointMsg) = NULL;
  CkpvAccess(lastBreakPointObject) = NULL;
  CkpvAccess(lastBreakPointIndex) = 0;
  if(CkMyRank() == 0)
    {
      CpvAccess(_debugMsg) = CkRegisterMsg("debug_msg",0,0,0,0);
      CpvAccess(_debugChare) = CkRegisterChare("debug_Chare",0,TypeChare);
      CkRegisterChareInCharm(CpvAccess(_debugChare));
    }
  CpvAccess(breakPointEntryTable) = new CpdBpFuncTable_t(10,0.5,CkHashFunction_int,CkHashCompare_int );
}

void CpdFinishInitialization() {
  CkpvInitialize(int, skipBreakpoint);
  CkpvAccess(skipBreakpoint) = 0;
  CkpvInitialize(DebugEntryTable, _debugEntryTable);
  CkpvAccess(_debugEntryTable).resize(_entryTable.size());
}

static void _call_freeze_on_break_point(void * msg, void * object)
{
      //Save breakpoint entry point index. This is retrieved from msg.
      //So that the appropriate EntryInfo can be later retrieved from the hash table
      //of break point function entries, on continue.

  // If the counter "skipBreakpoint" is not zero we actually do not freeze and deliver the regular message
  EntryInfo * breakPointEntryInfo = CpvAccess(breakPointEntryTable)->get(CkMessageToEpIdx(msg));
  if (CkpvAccess(skipBreakpoint) > 0 || CkpvAccess(_debugEntryTable)[CkMessageToEpIdx(msg)].isBreakpoint==false) {
    CkAssert(breakPointEntryInfo != NULL);
    breakPointEntryInfo->call(msg, object);
    if (CkpvAccess(skipBreakpoint) > 0) CkpvAccess(skipBreakpoint) --;
  } else {
      CkpvAccess(lastBreakPointMsg) = msg;
      CkpvAccess(lastBreakPointObject) = object;
      CkpvAccess(lastBreakPointIndex) = CkMessageToEpIdx(msg);
      //      CkPrintf("[%d] notify for bp %s m %p o %p idx %d\n",CkMyPe(), breakPointEntryInfo->name, msg, object,CkpvAccess(lastBreakPointIndex));
      CpdNotify(CPD_BREAKPOINT,breakPointEntryInfo->name);
      CpdFreeze();
  }
}

//ccs handler when pressed the "next" command: deliver only a single message without unfreezing
void CpdDeliverSingleMessage () {
  if (!CpdIsFrozen()) return; /* Do something only if we are in freeze mode */
  if ( (CkpvAccess(lastBreakPointMsg) != NULL) && (CkpvAccess(lastBreakPointObject) != NULL) ) {
    EntryInfo * breakPointEntryInfo = CpvAccess(breakPointEntryTable)->get(CkpvAccess(lastBreakPointIndex));
    if (breakPointEntryInfo != NULL) {
      if (_conditionalDelivery) {
        if (_conditionalDelivery==1) conditionalShm->msgs[conditionalShm->count++] = -1;
        void *env = UsrToEnv(CkpvAccess(lastBreakPointMsg));
        CmiReference(env);
        CdsFifo_Enqueue(CpvAccess(conditionalQueue),env);
      }
      breakPointEntryInfo->call(CkpvAccess(lastBreakPointMsg), CkpvAccess(lastBreakPointObject));
    }
    CkpvAccess(lastBreakPointMsg) = NULL;
    CkpvAccess(lastBreakPointObject) = NULL;
  }
  else {
    // we were not stopped at a breakpoint, then deliver the first message in the debug queue
    void *debugQ = CkpvAccess(debugQueue);
    if (!CdsFifo_Empty(debugQ)) {
      CkpvAccess(skipBreakpoint) = 1;
      CpdHandleMessage((char *)CdsFifo_Dequeue(debugQ), 0);
      CkpvAccess(skipBreakpoint) = 0;
    }
  }
}

//ccs handler when continue from a break point
void CpdContinueFromBreakPoint ()
{
  
  //  CkPrintf("[%d] CpdContinueFromBreakPoint\n",CkMyPe());
  
    CpdUnFreeze();
    if ( (CkpvAccess(lastBreakPointMsg) != NULL) && (CkpvAccess(lastBreakPointObject) != NULL) )
    {
        EntryInfo * breakPointEntryInfo = CpvAccess(breakPointEntryTable)->get(CkpvAccess(lastBreakPointIndex));
        if (breakPointEntryInfo != NULL) {
	  //	  CkPrintf("[%d] Continue found calling lastBreakPoint\n",CkMyPe());
           breakPointEntryInfo->call(CkpvAccess(lastBreakPointMsg), CkpvAccess(lastBreakPointObject));
        } else {
          // This means that the breakpoint got deleted in the meanwhile
          
        }
    }
#if 0
    else 
      {//debugging block
	CmiPrintStackTrace(0);
	CkPrintf("[%d] Continue found lastBreakPointmsg %p object %p idx %d\n",CkMyPe(), CkpvAccess(lastBreakPointMsg), CkpvAccess(lastBreakPointObject), CkpvAccess(lastBreakPointIndex));
    }
#endif    
    CkpvAccess(lastBreakPointMsg) = NULL;
    CkpvAccess(lastBreakPointObject) = NULL;
}

//ccs handler to set a breakpoint with entry function name msg
void CpdSetBreakPoint (char *msg)
{
  char functionName[128];
  int tableSize, tableIdx = 0;
  int reply = 0;
  sscanf(msg+CmiReservedHeaderSize, "%s", functionName);
  if (strlen(functionName) > 0)
  {
    tableSize = _entryTable.size();
    // Replace entry in entry table with _call_freeze_on_break_point
    tableIdx = atoi(functionName);
    if (tableIdx >= 0 && tableIdx < tableSize) {
      if (! CkpvAccess(_debugEntryTable)[tableIdx].isBreakpoint) {
           EntryInfo * breakPointEntryInfo = (EntryInfo *)CpvAccess(breakPointEntryTable)->get(tableIdx);
           if (breakPointEntryInfo == 0) {
             breakPointEntryInfo = new EntryInfo(_entryTable[tableIdx]->name, _entryTable[tableIdx]->call, 1, 0 );
             //CmiPrintf("Breakpoint is set for function %s with an epIdx = %ld\n", _entryTable[tableIdx]->name, tableIdx);
             CpvAccess(breakPointEntryTable)->put(tableIdx) = breakPointEntryInfo;
#if CMK_SMP	     
	    if(++_entryTable[tableIdx]->breakPointSet==CkMyNodeSize() )
#endif	       
	      {

		_entryTable[tableIdx]->name = "debug_breakpoint_ep";
		_entryTable[tableIdx]->call = (CkCallFnPtr)_call_freeze_on_break_point;
	      }
           } else {
             if (breakPointEntryInfo->msgIdx == 0) {
               // Reset the breakpoint info
               _entryTable[tableIdx]->name = "debug_breakpoint_ep";
               _entryTable[tableIdx]->call = (CkCallFnPtr)_call_freeze_on_break_point;
             }
             breakPointEntryInfo->msgIdx ++;
             //CkAssert(breakPointEntryInfo->name == _entryTable[tableIdx]->name);
             //CkAssert(breakPointEntryInfo->call == _entryTable[tableIdx]->call);
             //CkAssert(breakPointEntryInfo->msgIdx == _entryTable[tableIdx]->msgIdx);
             //CkAssert(breakPointEntryInfo->chareIdx == _entryTable[tableIdx]->chareIdx);
           }
           CkpvAccess(_debugEntryTable)[tableIdx].isBreakpoint = true;
           reply = ~0;
      }
    }

  }
  CcsSendReply(sizeof(int), (void*)&reply);

}

void CpdQuitDebug()
{
  CpdContinueFromBreakPoint();
  CkExit();
}

void CpdRemoveBreakPoint (char *msg)
{
  char functionName[128];
  int reply = 0;
  sscanf(msg+CmiReservedHeaderSize, "%s", functionName);
  if (strlen(functionName) > 0) {
    int idx = atoi(functionName);
    if (idx >= 0 && idx < _entryTable.size()) {
      if (CkpvAccess(_debugEntryTable)[idx].isBreakpoint) {
        EntryInfo * breakPointEntryInfo = CpvAccess(breakPointEntryTable)->get(idx);
        if (breakPointEntryInfo != NULL) {
          if (--breakPointEntryInfo->msgIdx == 0) {
            // If we are the last to delete the breakpoint, then restore the original name and call function pointer
#if CMK_SMP	    
	    if(--_entryTable[idx]->breakPointSet==0)
#endif	      
	      {
	      _entryTable[idx]->name =  breakPointEntryInfo->name;
	      _entryTable[idx]->call = (CkCallFnPtr)breakPointEntryInfo->call;
	    }
	    //	    CkPrintf("[%d] remonebp _entryTable[%d]->breakPointSet %d\n",CkMyPe(), idx,  _entryTable[idx]->breakPointSet.load());
          }
          reply = ~0 ;
          CkpvAccess(_debugEntryTable)[idx].isBreakpoint = false;
          //CmiPrintf("Breakpoint is removed for function %s with epIdx %ld\n", _entryTable[idx]->name, idx);
          //CkpvAccess(breakPointEntryTable)->remove(idx);
        }
      }
    }
  }
  CcsSendReply(sizeof(int), (void*)&reply);
}

void CpdRemoveAllBreakPoints ()
{
  //all breakpoints removed
  void *objPointer;
  void *keyPointer;
  int reply = 1;
  CkHashtableIterator *it = CpvAccess(breakPointEntryTable)->iterator();
  while(NULL!=(objPointer = it->next(&keyPointer)))
  {
    EntryInfo * breakPointEntryInfo = *(EntryInfo **)objPointer;
    int idx = *(int *)keyPointer;
    if (--breakPointEntryInfo->msgIdx == 0) {
      // If we are the last to delete the breakpoint, then restore the original name and call function pointer
#if CMK_SMP	    
      if(--_entryTable[idx]->breakPointSet==0)
#endif	      
	{
	  _entryTable[idx]->name =  breakPointEntryInfo->name;
	  _entryTable[idx]->call = (CkCallFnPtr)breakPointEntryInfo->call;
	}
      //      CkPrintf("[%d] remallbp _entryTable[%d]->breakPointSet %d\n",CkMyPe(), idx,  _entryTable[idx]->breakPointSet.load());
    }
    CkpvAccess(_debugEntryTable)[idx].isBreakpoint = false;
  }
  CcsSendReply(sizeof(int), (void*)&reply);
}

int CpdIsCharmDebugMessage(void *msg) {
  envelope *env = (envelope*)msg;
  // Later should use "isDebug" value, but for now just bypass all intrinsic EPs
  return CmiGetHandler(msg) != _charmHandlerIdx || env->getMsgtype() == ForVidMsg ||
         env->getMsgtype() == FillVidMsg || _entryTable[env->getEpIdx()]->inCharm;
}


CpvExtern(char *, displayArgument);

void CpdStartGdb(void)
{
#if !defined(_WIN32)
  FILE *f;
  char gdbScript[200];
  int pid;
  if (CpvAccess(displayArgument) != NULL)
  {
     /*CmiPrintf("MY NODE IS %d  and process id is %d\n", CmiMyPe(), getpid());*/
     snprintf(gdbScript, sizeof(gdbScript), "/tmp/cpdstartgdb.%d.%d", getpid(), CmiMyPe());
     f = fopen(gdbScript, "w");
     fprintf(f,"#!/bin/sh\n");
     fprintf(f,"cat > /tmp/start_gdb.$$ << END_OF_SCRIPT\n");
     fprintf(f,"shell /bin/rm -f /tmp/start_gdb.$$\n");
     //fprintf(f,"handle SIGPIPE nostop noprint\n");
     fprintf(f,"handle SIGWINCH nostop noprint\n");
     fprintf(f,"handle SIGWAITING nostop noprint\n");
     fprintf(f, "attach %d\n", getpid());
     fprintf(f,"END_OF_SCRIPT\n");
     fprintf(f, "DISPLAY='%s';export DISPLAY\n",CpvAccess(displayArgument));
     fprintf(f,"/usr/X11R6/bin/xterm ");
     fprintf(f," -title 'Node %d ' ",CmiMyPe());
     fprintf(f," -e /usr/bin/gdb -x /tmp/start_gdb.$$ \n");
     fprintf(f, "exit 0\n");
     fclose(f);
     if( -1 == chmod(gdbScript, 0755))
     {
        CmiPrintf("ERROR> chmod on script failed!\n");
        return;
     }
     pid = fork();
     if (pid < 0)
        { CmiAbort("ERROR> forking to run debugger script\n"); }
     if (pid == 0)
     {
         //CmiPrintf("In child process to start script %s\n", gdbScript);
 if (-1 == execlp(gdbScript, gdbScript, NULL))
            CmiPrintf ("Error> Could not Execute Debugger Script: %s\n",strerror
(errno));

      }
    }
#endif
}

size_t cpd_memory_length(void*);
void cpd_memory_pup(void*,void*,CpdListItemsRequest*);
void cpd_memory_leak(void*,void*,CpdListItemsRequest*);
size_t cpd_memory_getLength(void*);
void cpd_memory_get(void*,void*,CpdListItemsRequest*);


void CpdCharmInit()
{
  CpdListRegister(new CpdListAccessor_c("memory/list",cpd_memory_length,0,cpd_memory_pup,0));
  CpdListRegister(new CpdListAccessor_c("memory/data",cpd_memory_getLength,0,cpd_memory_get,0,false));

  //CpdBreakPointInit();
  CcsRegisterHandler("debug/charm/bp/set",(CmiHandler)CpdSetBreakPoint);
  CcsSetMergeFn("debug/charm/bp/set",CcsMerge_logical_and);
  CcsRegisterHandler("debug/charm/bp/remove",(CmiHandler)CpdRemoveBreakPoint);
  CcsSetMergeFn("debug/charm/bp/remove",CcsMerge_logical_and);
  CcsRegisterHandler("debug/charm/bp/removeall",(CmiHandler)CpdRemoveAllBreakPoints);
  CcsSetMergeFn("debug/charm/bp/removeall",CmiReduceMergeFn_random);
  CcsRegisterHandler("debug/charm/continue",(CmiHandler)CpdContinueFromBreakPoint);
  CcsSetMergeFn("debug/charm/continue",CmiReduceMergeFn_random);
  CcsRegisterHandler("debug/charm/next",(CmiHandler)CpdDeliverSingleMessage);
  CcsSetMergeFn("debug/charm/next",CmiReduceMergeFn_random);
  CcsRegisterHandler("debug/converse/quit",(CmiHandler)CpdQuitDebug);
  CcsSetMergeFn("debug/converse/quit",CmiReduceMergeFn_random);
  CcsRegisterHandler("debug/converse/startgdb",(CmiHandler)CpdStartGdb);
  CpdListRegister(new CpdListAccessor_c("hostinfo",hostInfoLength,0,hostInfo,0));
  CpdListRegister(new CpdList_localQ());
  CcsRegisterHandler("debug/charm/deliver",(CmiHandler)CpdDeliverMessage);
  CcsRegisterHandler("debug/charm/deliverall",(CmiHandler)CpdDeliverAllMessages);
  CcsSetMergeFn("debug/charm/deliverall",CmiReduceMergeFn_random);
  CcsRegisterHandler("debug/provisional/deliver",(CmiHandler)CpdDeliverMessageConditionally);
  CcsRegisterHandler("debug/provisional/rollback",(CmiHandler)CpdEndConditionalDelivery);
  CcsRegisterHandler("debug/provisional/commit",(CmiHandler)CpdCommitConditionalDelivery);
  CpdListRegister(new CpdList_arrayElementNames());
  CpdListRegister(new CpdList_arrayElements());
  CpdListRegister(new CpdList_objectNames());
  CpdListRegister(new CpdList_object());
  CpdListRegister(new CpdList_message());
  CpdListRegister(new CpdList_msgStack());
  CpdGetNextMessage = CsdNextMessage;
  CpdIsDebugMessage = CpdIsCharmDebugMessage;
}


#else

void CpdBreakPointInit() {}
void CpdCharmInit() {}

void CpdFinishInitialization() {}

void *CpdGetCurrentObject() {return NULL;}
void *CpdGetCurrentMsg() {return NULL;}
void CpdEndConditionalDeliver_master() {}

void CpdBeforeEp(int ep, void *obj, void *msg) {}
void CpdAfterEp(int ep) {}

#endif /*CMK_CCS_AVAILABLE*/

