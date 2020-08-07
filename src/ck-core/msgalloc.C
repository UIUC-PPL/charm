#include "ck.h"
#include "queueing.h"

CkpvDeclare(size_t *, _offsets);

void *CkAllocSysMsg(const CkEntryOptions *opts)
{
  if(opts == NULL)
    return CkpvAccess(_msgPool)->get();

  envelope *env = _allocEnv(ForChareMsg, 0, opts->getPriorityBits(), GroupDepNum{(int)opts->getGroupDepNum()});
  setMemoryTypeMessage(env);
  env->setMsgIdx(0);

  env->setIsVarSysMsg(1);
  // Set the message's queueing type
  env->setQueueing((unsigned char)opts->getQueueing());

  // Copy the priority bytes into the env from the opts
  if (opts->getPriorityPtr() != NULL)
    CmiMemcpy(env->getPrioPtr(), opts->getPriorityPtr(), env->getPrioBytes());

  // Copy the group dependence into the env from the opts
  if(opts->getGroupDepNum() > 0)
    CmiMemcpy(env->getGroupDepPtr(), opts->getGroupDepPtr(), env->getGroupDepSize());

  return EnvToUsr(env);
}

void CkFreeSysMsg(void *m)
{
  CkpvAccess(_msgPool)->put(m);
}

void* CkAllocMsg(int msgIdx, size_t msgBytes, int prioBits, GroupDepNum groupDepNum)
{
  envelope* env = _allocEnv(ForChareMsg, msgBytes, prioBits, groupDepNum);
  setMemoryTypeMessage(env);

  env->setQueueing(_defaultQueueing);
  env->setMsgIdx(msgIdx);

  return EnvToUsr(env);
}

void* CkAllocBuffer(void *msg, size_t bufsize)
{
  bufsize = CkMsgAlignLength(bufsize);
  envelope *env = UsrToEnv(msg);
  envelope *packbuf = _allocEnv(env->getMsgtype(), bufsize,
                      env->getPriobits(),
                      GroupDepNum{(int)env->getGroupDepNum()});
  
  size_t size = packbuf->getTotalsize();
  CmiMemcpy(packbuf, env, sizeof(envelope));
  packbuf->setTotalsize(size);
  packbuf->setPacked(!env->isPacked());
  CmiMemcpy(packbuf->getPrioPtr(), env->getPrioPtr(), packbuf->getPrioBytes());

  return EnvToUsr(packbuf);;
}

void  CkFreeMsg(void *msg)
{
  if (msg!=NULL) {
      CmiFree(UsrToEnv(msg));
  }
}


void* CkCopyMsg(void **pMsg)
{// cannot simply memcpy, because srcMsg could be varsize msg
  void *srcMsg = *pMsg;
  envelope *env = UsrToEnv(srcMsg);
  unsigned char msgidx = env->getMsgIdx();
  if(!env->isPacked() && _msgTable[msgidx]->pack) {
    srcMsg = _msgTable[msgidx]->pack(srcMsg);
    UsrToEnv(srcMsg)->setPacked(1);
  }
  size_t size = UsrToEnv(srcMsg)->getTotalsize();
  envelope *newenv = (envelope *) CmiAlloc(size);
  CmiMemcpy(newenv, UsrToEnv(srcMsg), size);
  //memcpy(newenv, UsrToEnv(srcMsg), size);
  if(UsrToEnv(srcMsg)->isPacked() && _msgTable[msgidx]->unpack) {
    srcMsg = _msgTable[msgidx]->unpack(srcMsg);
    UsrToEnv(srcMsg)->setPacked(0);
  }
  *pMsg = srcMsg;
  if(newenv->isPacked() && _msgTable[msgidx]->unpack) {
    srcMsg = _msgTable[msgidx]->unpack(EnvToUsr(newenv));
    UsrToEnv(srcMsg)->setPacked(0);
  } else srcMsg = EnvToUsr(newenv);

  setMemoryTypeMessage(newenv);
  return srcMsg;
}

void* CkReferenceMsg(void* msg)
{
  CmiReference(UsrToEnv(msg));
  return msg;
}

void  CkSetQueueing(void *msg, int strategy)
{
  UsrToEnv(msg)->setQueueing((unsigned char) strategy);
}


void* CkPriorityPtr(void *msg)
{
#if CMK_ERROR_CHECKING
  if (UsrToEnv(msg)->getPriobits() == 0) CkAbort("Trying to access priority bits, but none was allocated");
#endif
  return UsrToEnv(msg)->getPrioPtr();
}

CkMarshallMsg *CkAllocateMarshallMsgNoninline(size_t size,const CkEntryOptions *opts)
{
	//Allocate the message
	CkMarshallMsg *m=new (size,opts->getPriorityBits(),GroupDepNum{(int)opts->getGroupDepNum()}) CkMarshallMsg;
	//Copy the user's priority data into the message
	envelope *env=UsrToEnv(m);
	setMemoryTypeMessage(env);
	if (opts->getPriorityPtr() != NULL)
		CmiMemcpy(env->getPrioPtr(),opts->getPriorityPtr(),env->getPrioBytes());

	// Copy the group dependence into the env from the opts
	if(opts->getGroupDepNum() > 0)
		CmiMemcpy(env->getGroupDepPtr(), opts->getGroupDepPtr(), env->getGroupDepSize());

	//Set the message's queueing type
	env->setQueueing((unsigned char)opts->getQueueing());
	return m;
}

