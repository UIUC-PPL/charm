/*
A CkCallback is a simple way for a library to return data 
to a wide variety of user code, without the library having
to handle all 17 possible cases.

This object is implemented as a union, so the entire object
can be sent as bytes.  Another option would be to use a virtual 
"send" method.

Initial version by Orion Sky Lawlor, olawlor@acm.org, 2/8/2002
*/
#include "charm++.h"
#include "ckcallback-ccs.h"
#include "CkCallback.decl.h"
#include "envelope.h"

extern "C" void LibCkExit();  // Included for CkCallback::ckExit with interop

/*readonly*/ CProxy_ckcallback_group _ckcallbackgroup;

typedef CkHashtableT<CkHashtableAdaptorT<unsigned int>, CkCallback*> threadCB_t;
CpvStaticDeclare(threadCB_t*, threadCBs);
CpvStaticDeclare(unsigned int, nextThreadCB);

//This main chare is only used to create the callback forwarding group
class ckcallback_main : public CBase_ckcallback_main {
public:
	ckcallback_main(CkArgMsg *m) {
		_ckcallbackgroup=CProxy_ckcallback_group::ckNew();
		delete m;
	}
};

//The callback group is used to forward a callback to the processor
// it originated from.
class ckcallback_group : public CBase_ckcallback_group {
public:
	ckcallback_group() { /*empty*/ }
	ckcallback_group(CkMigrateMessage *m) { /*empty*/ }
	void registerCcsCallback(const char *name,const CkCallback &cb);
	void call(CkCallback &&c, CkMarshalledMessage &&msg) {
		c.send(msg.getMessage());
	}
	void call(CkCallback &&c, size_t length, const char *data) {
		if(c.requiresMsgConstruction())
			c.send(CkDataMsg::buildNew(length,data));
		else
			c.send(NULL); // do not construct CkDataMsg
	}
};

/*************** CkCallback implementation ***************/
//Initialize the callback's thread fields before sending it off:
void CkCallback::impl_thread_init(void)
{
    int exist;
    CkCallback **cb;
    d.thread.onPE=CkMyPe();
	do {
	  if (CpvAccess(nextThreadCB)==0) CpvAccess(nextThreadCB)=1;
	  d.thread.cb=CpvAccess(nextThreadCB)++;
	  cb = &CpvAccess(threadCBs)->put(d.thread.cb, &exist);
	} while (exist==1);
	*cb = this; //<- so we can find this structure later
	d.thread.th=NULL; //<- thread isn't suspended yet
	d.thread.ret=(void*)-1;//<- no data to return yet
}

//Actually suspend this thread
void *CkCallback::impl_thread_delay(void) const
{
	if (type!=resumeThread) 
		CkAbort("Called impl_thread_delay on non-threaded callback");
	if (CkMyPe()!=d.thread.onPE)
		CkAbort("Called thread_delay on different processor than where callback was created");
	
	//Find the original callback object:
	CkCallback *dest=(CkCallback *)this;
	if (d.thread.cb!=0) dest=CpvAccess(threadCBs)->get(d.thread.cb);
	if (dest==0)
	    CkAbort("Called thread_delay on an already deleted callback");
	if (dest->d.thread.ret==(void*)-1) 
	{  //We need to sleep for the result:
		dest->d.thread.th=CthSelf(); //<- so we know a thread is waiting
		CthSuspend();
		if (dest->d.thread.ret==(void*)-1) 
			CkAbort("thread resumed, but callback data is still empty");
	}
	return dest->d.thread.ret;
}


/*These can't be defined in the .h file like the other constructors
 * because we need CkCallback before CProxyElement* are defined.
 */
CkCallback::CkCallback(Chare *p, int ep, bool forceInline) {
#if CMK_ERROR_CHECKING
      memset(this, 0, sizeof(CkCallback));
#endif
      type = (forceInline || _entryTable[ep]->isInline) ? isendChare : sendChare;
	d.chare.ep=ep; 
	d.chare.id=p->ckGetChareID();
        d.chare.hasRefnum= false;
        d.chare.refnum = 0;
}
CkCallback::CkCallback(Group *p, int ep, bool forceInline) {
#if CMK_ERROR_CHECKING
      memset(this, 0, sizeof(CkCallback));
#endif
      type = (forceInline || _entryTable[ep]->isInline) ? isendGroup : sendGroup;
	d.group.ep=ep; d.group.id=p->ckGetGroupID(); d.group.onPE=CkMyPe();
        d.group.hasRefnum= false;
        d.group.refnum = 0;
}
CkCallback::CkCallback(NodeGroup *p, int ep, bool forceInline) {
#if CMK_ERROR_CHECKING
      memset(this, 0, sizeof(CkCallback));
#endif
      type = (forceInline || _entryTable[ep]->isInline) ? isendNodeGroup : sendNodeGroup;
	d.group.ep=ep; d.group.id=p->ckGetGroupID(); d.group.onPE=CkMyNode();
        d.group.hasRefnum= false;
        d.group.refnum = 0;
}

CkCallback::CkCallback(int ep,const CProxy_NodeGroup &ngp) {
#if CMK_ERROR_CHECKING
      memset(this, 0, sizeof(CkCallback));
#endif
      type=bcastNodeGroup;
	d.group.ep=ep; d.group.id=ngp.ckGetGroupID();
        d.group.hasRefnum= false;
        d.group.refnum = 0;
}

CkCallback::CkCallback(int ep,int onPE,const CProxy_NodeGroup &ngp,bool forceInline) {
#if CMK_ERROR_CHECKING
      memset(this, 0, sizeof(CkCallback));
#endif
      type = (forceInline || _entryTable[ep]->isInline) ? isendNodeGroup : sendNodeGroup;
	d.group.ep=ep; d.group.id=ngp.ckGetGroupID(); d.group.onPE=onPE;
        d.group.hasRefnum= false;
        d.group.refnum = 0;
}

CkCallback::CkCallback(int ep,const CProxyElement_Group &grpElt,bool forceInline) {
#if CMK_ERROR_CHECKING
      memset(this, 0, sizeof(CkCallback));
#endif
      type = (forceInline || _entryTable[ep]->isInline) ? isendGroup : sendGroup;
	d.group.ep=ep; 
	d.group.id=grpElt.ckGetGroupID(); 
	d.group.onPE=grpElt.ckGetGroupPe();
        d.group.hasRefnum= false;
        d.group.refnum = 0;
}

CkCallback::CkCallback(int ep, const CProxyElement_NodeGroup &grpElt, bool forceInline) {
#if CMK_ERROR_CHECKING
  memset(this, 0, sizeof(CkCallback));
#endif
  type = (forceInline || _entryTable[ep]->isInline) ? isendNodeGroup : sendNodeGroup;
  d.group.ep = ep;
  d.group.id = grpElt.ckGetGroupID();
  d.group.onPE = grpElt.ckGetGroupPe();
  d.group.hasRefnum = false;
  d.group.refnum = 0;
}

CkCallback::CkCallback(int ep,const CProxyElement_ArrayBase &arrElt,bool forceInline) {
#if CMK_ERROR_CHECKING
      memset(this, 0, sizeof(CkCallback));
#endif
      type = (forceInline || _entryTable[ep]->isInline) ? isendArray : sendArray;
	d.array.ep=ep; 
	d.array.id=arrElt.ckGetArrayID(); 
	d.array.idx = arrElt.ckGetIndex();
        d.array.hasRefnum= false;
        d.array.refnum = 0;
}

#if !CMK_CHARM4PY
CkCallback::CkCallback(int ep,CProxySection_ArrayBase &sectElt,bool forceInline) {
#if CMK_ERROR_CHECKING
      memset(this, 0, sizeof(CkCallback));
#endif
      type=bcastSection; // forceInline currently ignored
      d.section.ep=ep; 
      CkSectionID secID=sectElt.ckGetSectionID(0); 
      d.section.sinfo = secID._cookie.info;
      d.section._elems = secID._elems.data();
      d.section._nElems = secID._elems.size();
      d.section.pelist = secID.pelist.data();
      d.section.npes = secID.pelist.size();
      d.section.hasRefnum = false;
      d.section.refnum = 0;
}

CkCallback::CkCallback(int ep, CkSectionID &id) {
#if CMK_ERROR_CHECKING
      memset(this, 0, sizeof(CkCallback));
#endif
      type=bcastSection;
      d.section.ep=ep;
      d.section.sinfo = id._cookie.info;
      d.section._elems = id._elems.data();
      d.section._nElems = id._elems.size();
      d.section.pelist = id.pelist.data();
      d.section.npes = id.pelist.size();
      d.section.hasRefnum = false;
      d.section.refnum = 0;
}
#endif

CkCallback::CkCallback(ArrayElement *p, int ep,bool forceInline) {
#if CMK_ERROR_CHECKING
      memset(this, 0, sizeof(CkCallback));
#endif
      type = (forceInline || _entryTable[ep]->isInline) ? isendArray : sendArray;
    d.array.ep=ep; 
	d.array.id=p->ckGetArrayID(); 
	d.array.idx = p->ckGetArrayIndex();
        d.array.hasRefnum= false;
        d.array.refnum = 0;
}

#if CMK_CHARM4PY

// currently this is only used with Charm4py, so we are only enabling it for that case
// to guarantee best performance for non-charm4py applications

// function pointer to interact with Charm4py to generate callback msg
extern void (*CreateCallbackMsgExt)(void*, int, int, int, int *, char**, int*);

static void CkCallbackSendExt(const CkCallback &cb, void *msg)
{
  char *extResultMsgData[2] = {NULL, NULL};
  int extResultMsgDataSizes[2] = {0, 0};
  void *data = NULL;
  int dataLen = 0;
  int reducerType = -1;
  if (msg != NULL) {
    // right now this can only be a CkReductionMsg
    CkReductionMsg* redMsg = (CkReductionMsg*)msg;
    data = redMsg->getData();
    dataLen = redMsg->getLength();
    reducerType = redMsg->getReducer();
  }

  int _pe = -1;
  int sectionInfo[3] = {-1, 0, 0};
  switch (cb.type) {
    case CkCallback::sendChare: // Send message to a chare
      CreateCallbackMsgExt(data, dataLen, reducerType, cb.d.chare.refnum, sectionInfo,
                                  extResultMsgData, extResultMsgDataSizes);
      CkChareExtSend_multi(cb.d.chare.id.onPE, cb.d.chare.id.objPtr, cb.d.chare.ep,
                           2, extResultMsgData, extResultMsgDataSizes);
      break;
    case CkCallback::sendGroup: // Send message to a group element
      CreateCallbackMsgExt(data, dataLen, reducerType, cb.d.group.refnum, sectionInfo,
                                  extResultMsgData, extResultMsgDataSizes);
      CkGroupExtSend_multi(cb.d.group.id.idx, 1, &(cb.d.group.onPE), cb.d.group.ep,
                           2, extResultMsgData, extResultMsgDataSizes);
      break;
    case CkCallback::sendArray: // Send message to an array element
      CreateCallbackMsgExt(data, dataLen, reducerType, cb.d.array.refnum, sectionInfo,
                                  extResultMsgData, extResultMsgDataSizes);
      CkArrayExtSend_multi(cb.d.array.id.idx, cb.d.array.idx.asChild().data(), cb.d.array.idx.dimension,
                           cb.d.array.ep, 2, extResultMsgData, extResultMsgDataSizes);
      break;
    case CkCallback::bcastGroup:
      CreateCallbackMsgExt(data, dataLen, reducerType, cb.d.group.refnum, sectionInfo,
                                  extResultMsgData, extResultMsgDataSizes);
      // onPE is set to -1 since its a bcast
      CkGroupExtSend_multi(cb.d.group.id.idx, 1, &_pe, cb.d.group.ep, 2, extResultMsgData, extResultMsgDataSizes);
      break;
    case CkCallback::bcastArray:
      CreateCallbackMsgExt(data, dataLen, reducerType, cb.d.array.refnum, sectionInfo,
                                  extResultMsgData, extResultMsgDataSizes);
      // numDimensions is set to 0 since its bcast
      CkArrayExtSend_multi(cb.d.array.id.idx, cb.d.array.idx.asChild().data(), 0,
                           cb.d.array.ep, 2, extResultMsgData, extResultMsgDataSizes);
      break;
    case CkCallback::bcastSection: // Send message to a section
      sectionInfo[0] = cb.d.section.sid_pe;
      sectionInfo[1] = cb.d.section.sid_cnt;
      sectionInfo[2] = cb.d.section.ep;
      CreateCallbackMsgExt(data, dataLen, reducerType, 0, sectionInfo,
                           extResultMsgData, extResultMsgDataSizes);
      // after CreateCallbackMsgExt:
      // sectionInfo[0] contains SectionManager gid
      // sectionInfo[1] contains SectionManager ep (for sending section broadcasts)
      // send to SectionManager on root PE
      CkGroupExtSend_multi(sectionInfo[0], 1, &(cb.d.section.rootPE), sectionInfo[1],
                           2, extResultMsgData, extResultMsgDataSizes);
      break;
    default:
      CkAbort("Unsupported callback for ext reduction, or corrupted callback");
      break;
  }

  CkFreeMsg(msg); // free no longer used msg object
}
#endif

void CkCallback::send(size_t length,const void *data) const
{
	if(requiresMsgConstruction())
		send(CkDataMsg::buildNew(length,data));
	else
		send(NULL); // do not construct CkDataMsg
}

/*Libraries should call this from their "done" entry points.
  It takes the given message and handles it appropriately.
  After the send(), this callback is finished and cannot be reused.
*/
void CkCallback::send(void *msg) const
{
  // Variable is set with CK_MSG_IMMEDIATE when callback is pointing to an
  // immediate entry method
  int opts = 0;

#if CMK_CHARM4PY
  if (isExtCallback) { // callback target is external
    CkCallbackSendExt(*this, msg);
    return;
  }
#endif

	switch(type) {
	  //	CkPrintf("type:%d\n",type);
	case ignore: //Just ignore the callback
		if (msg) CkFreeMsg(msg);
		break;
	case ckExit: //Call ckExit (or LibCkExit if in interop mode)
		if (msg) CkFreeMsg(msg);
		if (CharmLibInterOperate) LibCkExit();
		else CkExit();
		break;
	case resumeThread: //Resume a waiting thread
		if (d.thread.onPE==CkMyPe()) {
			CkCallback *dest=CpvAccess(threadCBs)->get(d.thread.cb);
			if (dest==0 || dest->d.thread.ret!=(void*)-1)
				CkAbort("Already sent a value to this callback!\n");
			dest->d.thread.ret=msg; //<- return data
			if (dest->d.thread.th!=NULL)
				CthAwaken(dest->d.thread.th);
		} 
		else //Forward message to processor where the thread actually lives
			_ckcallbackgroup[d.thread.onPE].call(*this,(CkMessage *)msg);
		break;
	case call1Fn: //Call a C function pointer on the current processor
		(d.c1fn.fn)(msg);
		break;
	case callCFn: //Call a C function pointer on the appropriate processor
		if (d.cfn.onPE==CkMyPe())
			(d.cfn.fn)(d.cfn.param,msg);
		else
			_ckcallbackgroup[d.cfn.onPE].call(*this,(CkMessage *)msg);
		break;
	case sendChare: //Send message to a chare
		if (!msg) msg=CkAllocSysMsg();
                if (d.chare.hasRefnum) CkSetRefNum(msg, d.chare.refnum);
		CkSendMsg(d.chare.ep, msg, &d.chare.id);
		break;
	case isendChare: //inline send-to-chare
		if (!msg) msg=CkAllocSysMsg();
                if (d.chare.hasRefnum) CkSetRefNum(msg, d.chare.refnum);
		CkSendMsgInline(d.chare.ep, msg, &d.chare.id);
		break;
	case sendGroup: //Send message to a group element
		if (!msg) msg=CkAllocSysMsg();
                if (d.group.hasRefnum) CkSetRefNum(msg, d.group.refnum);
		CkSendMsgBranch(d.group.ep, msg, d.group.onPE, d.group.id);
		break;
	case sendNodeGroup: //Send message to a group element
		if (!msg) msg=CkAllocSysMsg();
                if (d.group.hasRefnum) CkSetRefNum(msg, d.group.refnum);
                if (_entryTable[d.group.ep]->isImmediate) opts = CK_MSG_IMMEDIATE;
		CkSendMsgNodeBranch(d.group.ep, msg, d.group.onPE, d.group.id, opts);
		break;
	case isendGroup: //inline send-to-group element
		if (!msg) msg=CkAllocSysMsg();
                if (d.group.hasRefnum) CkSetRefNum(msg, d.group.refnum);
		CkSendMsgBranchInline(d.group.ep, msg, d.group.onPE, d.group.id);
		break;
	case isendNodeGroup: //inline send-to-group element
		if (!msg) msg=CkAllocSysMsg();
                if (d.group.hasRefnum) CkSetRefNum(msg, d.group.refnum);
                if (_entryTable[d.group.ep]->isImmediate) opts = CK_MSG_IMMEDIATE;
		CkSendMsgNodeBranchInline(d.group.ep, msg, d.group.onPE, d.group.id, opts);
		break;
	case sendArray: //Send message to an array element
		if (!msg) msg=CkAllocSysMsg();
                if (d.array.hasRefnum) CkSetRefNum(msg, d.array.refnum);
		CkSetMsgArrayIfNotThere(msg);
		CkSendMsgArray(d.array.ep, msg, d.array.id, d.array.idx.asChild());
		break;
	case isendArray: //inline send-to-array element
		if (!msg) msg=CkAllocSysMsg();
                if (d.array.hasRefnum) CkSetRefNum(msg, d.array.refnum);
		CkSendMsgArrayInline(d.array.ep, msg, d.array.id, d.array.idx.asChild());
		break;
	case bcastGroup:
		if (!msg) msg=CkAllocSysMsg();
                if (d.group.hasRefnum) CkSetRefNum(msg, d.group.refnum);
		CkBroadcastMsgBranch(d.group.ep, msg, d.group.id);
		break;
	case bcastNodeGroup:
		if (!msg) msg=CkAllocSysMsg();
                if (d.group.hasRefnum) CkSetRefNum(msg, d.group.refnum);
                if (_entryTable[d.group.ep]->isImmediate) opts = CK_MSG_IMMEDIATE;
		CkBroadcastMsgNodeBranch(d.group.ep, msg, d.group.id, opts);
		break;
	case bcastArray:
		if (!msg) msg=CkAllocSysMsg();
                if (d.array.hasRefnum) CkSetRefNum(msg, d.array.refnum);
		CkBroadcastMsgArray(d.array.ep, msg, d.array.id);
		break;
#if !CMK_CHARM4PY
	case bcastSection: {
		if(!msg)msg=CkAllocSysMsg();
                if (d.section.hasRefnum) CkSetRefNum(msg, d.section.refnum);
                CkSectionInfo sinfo(d.section.sinfo);
                CkSectionID secID(sinfo, d.section._elems, d.section._nElems, d.section.pelist, d.section.npes);
		CkBroadcastMsgSection(d.section.ep, msg, secID);
		break;
             }
#endif
	case replyCCS: { /* Send CkDataMsg as a CCS reply */
		void *data=NULL;
		size_t length=0;
		if (msg) {
			CkDataMsg *m=(CkDataMsg *)msg;
			m->check();
			data=m->getData();
			length=m->getLength();
		}
		CcsSendDelayedReply(d.ccsReply.reply,length,data);
		if (msg) CkFreeMsg(msg);
		} break;
	case invalid: //Uninitialized
		CmiAbort("Called send on uninitialized callback");
		break;
	default: //Out-of-bounds type code
		CmiAbort("Called send on corrupted callback");
		break;
	};
}

void CkCallback::pup(PUP::er &p) {
  //p((char*)this, sizeof(CkCallback));
  int t = (int)type;
  p|t;
  type = (callbackType)t;
  switch (type) {
  case resumeThread:
    p|d.thread.onPE;
    p|d.thread.cb;
    break;
  case isendChare:
  case sendChare:
    p|d.chare.ep;
    p|d.chare.id;
    p|d.chare.hasRefnum;
    p|d.chare.refnum;
    break;
  case isendGroup:
  case sendGroup:
  case isendNodeGroup:
  case sendNodeGroup:
    p|d.group.onPE;
    p|d.group.hasRefnum;
    p|d.group.refnum;
  case bcastNodeGroup:
  case bcastGroup:
    p|d.group.ep;
    p|d.group.id;
    p|d.group.hasRefnum;
    p|d.group.refnum;
    break;
  case isendArray:
  case sendArray:
    p|d.array.idx;
    p|d.array.hasRefnum;
    p|d.array.refnum;
  case bcastArray:
    p|d.array.ep;
    p|d.array.id;
    p|d.array.hasRefnum;
    p|d.array.refnum;
    break;
  case replyCCS:
    p((char*)&d.ccsReply.reply, sizeof(d.ccsReply.reply));
    break;
  case call1Fn:
    p((char*)&d.c1fn, sizeof(d.c1fn));
    break;
  case callCFn:
    p((char*)&d.cfn, sizeof(d.cfn));
    break;
  case ignore:
  case ckExit:
  case invalid:
    break;
  default:
    CkAbort("Inconsistent CkCallback type");
  }
#if CMK_CHARM4PY
  p|isExtCallback;
#endif
}

bool CkCallback::containsPointer() const {
  switch(type) {
  case invalid:
  case ignore:
  case ckExit:
  case sendGroup:
  case sendNodeGroup:
  case sendArray:
  case isendGroup:
  case isendNodeGroup:
  case isendArray:
  case bcastGroup:
  case bcastNodeGroup:
  case bcastArray:
#if CMK_CHARM4PY
  case bcastSection:
#endif
    return false;

  case resumeThread:
  case callCFn:
  case call1Fn:
  case replyCCS:
#if !CMK_CHARM4PY
  case bcastSection:
#endif
    return true;

  case sendChare:
  case isendChare:
#if CMK_CHARE_USE_PTR
    return true;
#else
    return false;
#endif

  default:
    CkAbort("Asked about an unknown CkCallback type");
    return true;
  }
}

void CkCallback::thread_destroy() const {
  if (type==resumeThread && CpvAccess(threadCBs)->get(d.thread.cb)==this) {
    CpvAccess(threadCBs)->remove(d.thread.cb);
  }
}

CkCallbackResumeThread::~CkCallbackResumeThread() {
  void * res = thread_delay(); //<- block thread here if it hasn't already
  if (result != NULL) *result = res;
  else CkFreeMsg(res);
  thread_destroy();
}

/****** Callback-from-CCS ******/

// This function is called by CCS when a request comes in-- it maps the 
// request to a Charm++ message and passes the message to its callback.
extern "C" void ccsHandlerToCallback(void *cbPtr,int reqLen,const void *reqData) 
{
	CkCallback *cb=(CkCallback *)cbPtr;
	CkCcsRequestMsg *msg=new (reqLen,0) CkCcsRequestMsg;
	msg->reply=CcsDelayReply();
	msg->length=reqLen;
	memcpy(msg->data,reqData,reqLen);
	cb->send(msg);
}

// Register this callback with CCS.
void ckcallback_group::registerCcsCallback(const char *name,const CkCallback &cb)
{
	CcsRegisterHandlerFn(name,ccsHandlerToCallback,new CkCallback(cb));
}

// Broadcast this callback registration to all processors
void CcsRegisterHandler(const char *ccs_handlername,const CkCallback &cb) {
	_ckcallbackgroup.registerCcsCallback(ccs_handlername,cb);
}

#if CMK_ERROR_CHECKING
enum {dataMsgTag=0x7ed2beef};
#endif

CkDataMsg *CkDataMsg::buildNew(size_t length,const void *data)
{
	CkDataMsg *msg=new (&length,0) CkDataMsg;
	msg->length=length;
	memcpy(msg->data,data,length);
#if CMK_ERROR_CHECKING
	msg->checkTag=dataMsgTag;
#endif
	return msg;
}

void CkDataMsg::check(void)
{
#if CMK_ERROR_CHECKING
	if (checkTag!=dataMsgTag)
		CkAbort("CkDataMsg corrupted-- bad tag.");
#endif
}

void CkCallbackInit() {
  CpvInitialize(threadCB_t*, threadCBs);
  CpvAccess(threadCBs) = new threadCB_t;
  CpvInitialize(unsigned int, nextThreadCB);
  CpvAccess(nextThreadCB)=1;
}

#include "CkCallback.def.h"

