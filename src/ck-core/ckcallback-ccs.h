/*
Make CCS call a CkCallback instead of a flat C function.

Initial version by Orion Sky Lawlor, olawlor@acm.org, 2/8/2002
*/
#ifndef _CKCALLBACK_CCS_H_
#define _CKCALLBACK_CCS_H_

#include "charm++.h" /*for CkCallback, etc.*/
#include "conv-ccs.h" /*for CcsDelayedReply struct*/
#include "CkCallback.decl.h" /*for CMessage_CkCcsRequestMsg*/

/**
 * Message sent from CCS to callbacks.
 * You must eventually call CcsSendDelayedReply(msg->reply,...) 
 * for each CCS-called callback.
 */
class CkCcsRequestMsg : public CMessage_CkCcsRequestMsg {
public:
	CcsDelayedReply reply; /*Object to send reply to*/
	size_t length; //Number of bytes of request data.
	char *data; //Actual data sent along with request.
};

/**
 * Very generic message type: contains a bunch of bytes.
 */
class CkDataMsg : public CMessage_CkDataMsg {
public:
	size_t length; //Number of bytes of data below.
	char *data; //Message data.
#if CMK_ERROR_CHECKING
	int checkTag; // For detecting message corruption
#endif
	
	inline size_t getLength(void) const {return length;}
        inline size_t getSize(void) const {return length;}
        inline void *getData(void) const {return data;}
	
	/// This is how you must create a CkDataMsg
	static CkDataMsg *buildNew(size_t length,const void *data);
	
	void check(void);
};


#ifdef CcsRegisterHandler /*pollution from C conv-ccs header*/
# undef CcsRegisterHandler
#endif

/**
 * When a CCS request comes in from the network with this handlername,
 * call this callback with an appropriate CkCcsRequestMsg.
 * You must eventually call CcsSendDelayedReply(msg->reply,...) 
 * each time this callback is activated.
 *
 * Unlike the regular converse CcsRegisterHandler (in conv-ccs.h),
 * this call need only be made once, on processor 0, and all processors
 * will be able to respond to the CCS request. 
 */
void CcsRegisterHandler(const char *ccs_handlername,const CkCallback &cb);


#endif
