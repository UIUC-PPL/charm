
/*
 * converse master-slave (or manager-worker or agenda) paradigm library 
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cms.h"

typedef struct {
    int ref;
    char *response;
} ResponseRecord;

CpvDeclare(int, stopHandlerIndex);
CpvDeclare(int, workHandler);
CpvDeclare(int, responseHandler);
CpvDeclare(int, infoIdx);
CpvDeclare(ResponseRecord *, responses);
CpvDeclare(CmsWorkerFn, clientWorker);
CpvDeclare(CmsConsumerFn, consumerFunction);
CpvDeclare(int, counter);
CpvDeclare(int, tableSize);

static void initForCms(int argc, char *argv[])
{
}

static void
cms_infoFn(void *msg, CldPackFn * pfn, size_t *len,
	   int *queueing, int *priobits, unsigned int **prioptr)
{
    *pfn = 0;
    *len =
	*((int *) ((char *) msg + CmiExtHeaderSizeBytes)) +
	CmiExtHeaderSizeBytes + 8;
    *queueing = CQS_QUEUEING_FIFO;
    *priobits = 0;
    *prioptr = 0;
}

static void stopHandler(void *msg)
{
    CsdExitScheduler();
    ConverseExit();
}

static void callWorker(void *msg)
{
    char *m, *r;
    int size, msgSize, ref;
    char *msg2;

    ref = *((int *) ((char *) msg + sizeof(int) + CmiExtHeaderSizeBytes));
    m = (char *) msg + 2 * sizeof(int) + CmiExtHeaderSizeBytes;
    size = CpvAccess(clientWorker) (m, &r);

    msgSize = 2 * sizeof(int) + CmiMsgHeaderSizeBytes + size;
    msg2 = (char *)CmiAlloc(msgSize);
    m = msg2 + CmiMsgHeaderSizeBytes;
    *((int *) m) = size;
    m += sizeof(int);
    *((int *) m) = ref;
    m += sizeof(int);
    memcpy(m, r, size);
    CmiSetHandler(msg2, CpvAccess(responseHandler));
    CmiSyncSendAndFree(0, msgSize, msg2);
}

static void response(void *msg)
{
    char *r, *m;
    int size, ref;
    m = (char *) msg + CmiMsgHeaderSizeBytes;
    size = *((int *) m);
    m += sizeof(int);
    ref = *((int *) m);
    m += sizeof(int);
    r = (char *) malloc(size);
    memcpy(r, m, size);
    if (CpvAccess(consumerFunction) == 0) {
	CpvAccess(responses)[ref].ref = ref;
	CpvAccess(responses)[ref].response = r;
    } else {
	CpvAccess(consumerFunction) (r, ref);
    }

    if (--CpvAccess(counter) == 0) {
	CsdExitScheduler();
    }
}


void *CmsGetResponse(int ref)
{
    return (CpvAccess(responses)[ref].response);
}


static void cms_registerHandlers(CmsWorkerFn f)
{
    CpvInitialize(int, stopHandlerIndex);
    CpvInitialize(int, workHandler);
    CpvInitialize(int, responseHandler);
    CpvInitialize(int, infoIdx);
    CpvInitialize(ResponseRecord *, responses);
    CpvInitialize(CmsWorkerFn, clientWorker);
    CpvInitialize(CmsConsumerFn, consumerFunction);
    CpvInitialize(int, counter);
    CpvInitialize(int, tableSize);


    CpvAccess(stopHandlerIndex) =
	CmiRegisterHandler((CmiHandler) stopHandler);
    CpvAccess(workHandler) = CmiRegisterHandler((CmiHandler) callWorker);
    CpvAccess(responseHandler) = CmiRegisterHandler((CmiHandler) response);
    CpvAccess(infoIdx) = CldRegisterInfoFn(cms_infoFn);
    CpvAccess(clientWorker) = f;
    CpvAccess(consumerFunction) = (CmsConsumerFn) 0;
}



void CmsInit(CmsWorkerFn f, int maxResponses)
{
    char *argv[1];
    int argc = 0;
    argv[0] = 0;
    ConverseInit(argc, argv, (CmiStartFn) initForCms, 1, 1);
    cms_registerHandlers(f);
    CpvAccess(tableSize) = maxResponses;
    if (CmiMyPe() == 0) {	/*
				 * I am the manager 
				 */
	CpvAccess(responses) =
	    (ResponseRecord *) CmiAlloc(maxResponses * sizeof(ResponseRecord));
	CpvAccess(counter) = 0;
    } else {			/*
				 * I am a worker 
				 */
	CsdScheduler(-1);
	ConverseExit();
    }
}

void CmsFireTask(int ref, void *t, int size)
{
    char *m;
    char *msg;

    msg = (char *)CmiAlloc(2 * sizeof(int) + CmiExtHeaderSizeBytes + size);

    CmiSetHandler(msg, CpvAccess(workHandler));
    m = msg + CmiExtHeaderSizeBytes;
    *((int *) m) = size;
    m += sizeof(int);
    *((int *) m) = ref;
    m += sizeof(int);
    memcpy(m, t, size);

    CldEnqueue(CLD_ANYWHERE, msg, CpvAccess(infoIdx));

    CpvAccess(counter)++;
}

/*
 * allows the system to use processor 0 as a worker.
 */
void CmsAwaitResponses(void)
{

    CsdScheduler(-1);		/*
				 * be a worker for a while, and also process
				 * responses 
				 */
    /*
     * when back from the sceduler, return. Because all response have been
     * recvd 
     */
}

void CmsProcessResponses(CmsConsumerFn f)
{
    CpvAccess(consumerFunction) = f;
    CsdScheduler(-1);
}

void CmsExit(void)
{
    void *msg;

    msg = CmiAlloc(CmiMsgHeaderSizeBytes + 8);
    CmiSetHandler(msg, CpvAccess(stopHandlerIndex));
    CmiSyncBroadcastAndFree(CmiMsgHeaderSizeBytes + 8, msg);

    ConverseExit();
}
