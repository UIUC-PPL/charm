/** @file


There are three options here for synchronization:
      XPMEM_FENCE is the default. It uses memory fences
      XPMEM_OSSPINLOCK will cause OSSpinLock's to be used (available on OSX)
      XPMEM_LOCK will cause POSIX semaphores to be used

  created by 
	Gengbin Zheng, September 2011
*/

/**
 * @addtogroup NET
 * @{
 */

#include <sys/types.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/ioctl.h>
#if CMK_CXI
#include <pmi_cray.h>
#endif
#include "xpmem.h"

/************** 
   Determine which type of synchronization to use 
*/
#if XPMEM_OSSPINLOCK
#include <libkern/OSAtomic.h>
#elif XPMEM_LOCK
#include <semaphore.h>
#else
/* Default to using fences */
#define XPMEM_FENCE 1
#endif
#if CMK_CXI
#define CmiGetMsgSize(msg)  ((((CmiMsgHeaderBasic *)msg)->size))
#endif
#define MEMDEBUG(x) //x

#define XPMEM_STATS    0

#define SENDQ_LIST     0

/*** The following code was copied verbatim from pcqueue.h file ***/
#undef CmiMemoryWriteFence
#if XPMEM_FENCE
#ifdef POWER_PC
#define CmiMemoryWriteFence(startPtr,nBytes) asm volatile("eieio":::"memory")
#else
#define CmiMemoryWriteFence(startPtr,nBytes) asm volatile("sfence":::"memory")
//#define CmiMemoryWriteFence(startPtr,nBytes) 
#endif
#else
#undef CmiMemoryWriteFence
#define CmiMemoryWriteFence(startPtr,nBytes)  
#endif

#undef CmiMemoryReadFence
#if XPMEM_FENCE
#ifdef POWER_PC
#define CmiMemoryReadFence(startPtr,nBytes) asm volatile("eieio":::"memory")
#else
#define CmiMemoryReadFence(startPtr,nBytes) asm volatile("lfence":::"memory")
//#define CmiMemoryReadFence(startPtr,nBytes) 
#endif
#else
#define CmiMemoryReadFence(startPtr,nBytes) 
#endif

#if CMK_SMP
#error  "XPMEM can only be used in non-smp build of Charm++"
#endif

/***************************************************************************************/

enum entities {SENDER,RECEIVER};

/************************
 * 	Implementation currently assumes that
 * 	1) all nodes have the same number of processors
 *  2) in the nodelist all processors in a node are listed in sequence
 *   0 1 2 3      4 5 6 7 
 *   -------      -------
 *    node 1       node 2 
 ************************/

#define NAMESTRLEN 60
#define PREFIXSTRLEN 50 

static int XPMEMBUFLEN  =   (1024*1024*4);
#define XPMEMMINSIZE     (1*1024)
#define XPMEMMAXSIZE     (1024*1024)

static int  SENDQSTARTSIZE  =  256;


/// This struct is used as the first portion of a shared memory region, followed by data
typedef struct {
	int count; //number of messages
	int bytes; //number of bytes

#if XPMEM_OSSPINLOCK
	OSSpinLock lock;
#endif

#if XPMEM_FENCE
	volatile int flagSender;
	alignas(CMI_CACHE_LINE_SIZE) volatile int flagReceiver;
	alignas(CMI_CACHE_LINE_SIZE) volatile int turn;
#endif	

} sharedBufHeader;


typedef struct {
#if XPMEM_LOCK
	sem_t *mutex;
#endif
	sharedBufHeader *header;	
	char *data;
        __s64  segid;
} sharedBufData;

typedef struct OutgoingMsgRec
{
  char *data;
  int  *refcount;
  int   size;
}
OutgoingMsgRec;

typedef struct {
	int size;       //total size of data array
	int begin;      //position of first element
	int end;	//position of next element
	int numEntries; //number of entries
        int rank;       // for dest rank
#if SENDQ_LIST
        int next;         // next dstrank of non-empty queue
#endif
	OutgoingMsgRec *data;

} XpmemSendQ;

typedef struct {
	int nodesize;
	int noderank;
	int nodestart,nodeend;//proc numbers for the start and end of this node
	char prefixStr[PREFIXSTRLEN];
	char **recvBufNames;
	char **sendBufNames;

	sharedBufData *recvBufs;
	sharedBufData *sendBufs;

	XpmemSendQ **sendQs;

#if XPMEM_STATS
	int sendCount;
	int validCheckCount;
	int lockRecvCount;
	double validCheckTime;
	double sendTime;
	double commServerTime;
#endif

} XpmemContext;


#if SENDQ_LIST
static int sendQ_head_index = -1;
#endif

XpmemContext *xpmemContext=NULL; //global context


void calculateNodeSizeAndRank(char **);
void setupSharedBuffers(void);
void initAllSendQs(void);

void CmiExitXpmem(void);

static void cleanupOnAllSigs(int signo)
{
    CmiExitXpmem();
}

static int xpmem_fd;

/******************
 * 	Initialization routine
 * 	currently just testing start up
 * ****************/
void CmiInitXpmem(char **argv){
        char input[32];
        char *env;

	MACHSTATE(3,"CminitXpmem start");
	xpmemContext = (XpmemContext *)calloc(1,sizeof(XpmemContext));

#if CMK_NET_VERSION
	if(Cmi_charmrun_pid <= 0){
		CmiAbort("pxshm must be run with charmrun");
	}
#endif
	calculateNodeSizeAndRank(argv);

	MACHSTATE1(3,"CminitXpmem  %d calculateNodeSizeAndRank",xpmemContext->nodesize);

	if(xpmemContext->nodesize == 1) return;
	
        env = getenv("CHARM_XPMEM_SIZE");
        if (env) {
            XPMEMBUFLEN = CmiReadSize(env);
        }
        SENDQSTARTSIZE = 32 * xpmemContext->nodesize;

        if (_Cmi_mynode == 0)
            CmiPrintf("Charm++> xpmem enabled: %d cores per node, buffer size: %.1fMB\n", xpmemContext->nodesize, XPMEMBUFLEN/1024.0/1024.0);

        xpmem_fd = open("/dev/xpmem", O_RDWR);
        if (xpmem_fd == -1) {
            CmiAbort("Opening /dev/xpmem");
        }

#if CMK_CRAYXE || CMK_CRAYXC || CMK_OFI
        srand(getpid());
        int Cmi_charmrun_pid = rand();
        PMI_Bcast(&Cmi_charmrun_pid, sizeof(int));
#elif !CMK_NET_VERSION
        #error "need a unique number"
#endif
	snprintf(&(xpmemContext->prefixStr[0]),PREFIXSTRLEN-1,"charm_xpmem_%d",Cmi_charmrun_pid);

	MACHSTATE2(3,"CminitXpmem %s %d pre setupSharedBuffers",xpmemContext->prefixStr,xpmemContext->nodesize);

	setupSharedBuffers();

	MACHSTATE2(3,"CminitXpmem %s %d setupSharedBuffers",xpmemContext->prefixStr,xpmemContext->nodesize);

	initAllSendQs();
	
	MACHSTATE2(3,"CminitXpmem %s %d initAllSendQs",xpmemContext->prefixStr,xpmemContext->nodesize);

	MACHSTATE2(3,"CminitXpmem %s %d done",xpmemContext->prefixStr,xpmemContext->nodesize);

    struct sigaction sa;
    sa.sa_handler = cleanupOnAllSigs;
    sigemptyset(&sa.sa_mask);    
    sa.sa_flags = SA_RESTART;

    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
};

/**************
 * shutdown shmem objects and semaphores
 *
 * *******************/
static int pxshm_freed = 0;
void tearDownSharedBuffers(void);
void freeSharedBuffers(void);

void CmiExitXpmem(void){
	int i=0;
	
        if (xpmemContext == NULL) return;

	if(xpmemContext->nodesize != 1) {
                //tearDownSharedBuffers();
	
		for(i=0;i<xpmemContext->nodesize;i++){
			if(i != xpmemContext->noderank){
				break;
			}
		}
		free(xpmemContext->recvBufNames[i]);
		free(xpmemContext->sendBufNames[i]);

		free(xpmemContext->recvBufNames);
		free(xpmemContext->sendBufNames);

		free(xpmemContext->recvBufs);
		free(xpmemContext->sendBufs);

	}
#if XPMEM_STATS
CmiPrintf("[%d] sendCount %d sendTime %6lf validCheckCount %d validCheckTime %.6lf commServerTime %6lf lockRecvCount %d \n",_Cmi_mynode,xpmemContext->sendCount,xpmemContext->sendTime,xpmemContext->validCheckCount,xpmemContext->validCheckTime,xpmemContext->commServerTime,xpmemContext->lockRecvCount);
#endif
	free(xpmemContext);
        xpmemContext = NULL;
}

/******************
 *Should this message be sent using PxShm or not ?
 * ***********************/

/* dstNode is node number */
inline 
static int CmiValidXpmem(int node, int size){
#if XPMEM_STATS
	xpmemContext->validCheckCount++;
#endif
	//replace by bitmap later
	//if(dst >= xpmemContext->nodestart && dst <= xpmemContext->nodeend && size < XPMEMMAXSIZE && size > XPMEMMINSIZE){
	return (node >= xpmemContext->nodestart && node <= xpmemContext->nodeend && size <= XPMEMMAXSIZE )? 1: 0;
};


inline int XpmemRank(int dstnode){
	return dstnode - xpmemContext->nodestart;
}

inline void pushSendQ(XpmemSendQ *q, char *msg, int size, int *refcount);
inline int sendMessage(char *msg, int size, int *refcount, sharedBufData *dstBuf,XpmemSendQ *dstSendQ);
inline int flushSendQ(XpmemSendQ *sendQ);

inline int sendMessageRec(OutgoingMsgRec *omg, sharedBufData *dstBuf,XpmemSendQ *dstSendQ){
  return sendMessage(omg->data, omg->size, omg->refcount, dstBuf, dstSendQ);
}

/***************
 *
 *Send this message through shared memory
 *if you cannot get lock, put it in the sendQ
 *Before sending messages pick them from sendQ
 *
 * ****************************/

void CmiSendMessageXpmem(char *msg, int size, int dstnode, int *refcount)
{
#if XPMEM_STATS
	double _startSendTime = CmiWallTimer();
#endif

        LrtsPrepareEnvelope(msg, size);
	
	int dstRank = XpmemRank(dstnode);
	MEMDEBUG(CmiMemoryCheck());
  
	//	MACHSTATE4(3,"Send Msg Xpmem msg %p size %d dst %d dstRank %d",msg,msg->size,msg->dst,dstRank);
	//	MACHSTATE4(3,"Send Msg Xpmem msg %p size %d dst %d dstRank %d",msg,msg->size,msg->dst,dstRank);

	CmiAssert(dstRank >=0 && dstRank != xpmemContext->noderank);
	
	sharedBufData *dstBuf = &(xpmemContext->sendBufs[dstRank]);
        XpmemSendQ *sendQ = xpmemContext->sendQs[dstRank];

#if XPMEM_OSSPINLOCK
	if(! OSSpinLockTry(&dstBuf->header->lock)){
#elif XPMEM_LOCK
	if(sem_trywait(dstBuf->mutex) < 0){
#elif XPMEM_FENCE
	dstBuf->header->flagSender = 1;
	dstBuf->header->turn = RECEIVER;
	CmiMemoryReadFence(0,0);
	CmiMemoryWriteFence(0,0);
	//if(dstBuf->header->flagReceiver && dstBuf->header->turn == RECEIVER){
	if(dstBuf->header->flagReceiver){
	        dstBuf->header->flagSender = 0;
#endif
		/**failed to get the lock 
		insert into q and retain the message*/
#if SENDQ_LIST
                if (sendQ->numEntries == 0 && sendQ->next == -2) {
                    sendQ->next = sendQ_head_index;
                    sendQ_head_index = dstRank;
                }
#endif
		pushSendQ(sendQ, msg, size, refcount);
		(*refcount)++;
		MEMDEBUG(CmiMemoryCheck());
		return;
	}else{
		/***
		 * We got the lock for this buffer
		 * first write all the messages in the sendQ and then write this guy
		 * */
		 if(sendQ->numEntries == 0){
		 	// send message user event
			int ret = sendMessage(msg,size,refcount,dstBuf,sendQ);
#if SENDQ_LIST
                        if (sendQ->numEntries > 0 && sendQ->next == -2) 
                        {
                        	sendQ->next = sendQ_head_index;
                                sendQ_head_index = dstRank;
                        }
#endif
			MACHSTATE(3,"Xpmem Send succeeded immediately");
		 }else{
			(*refcount)+=2;/*this message should not get deleted when the queue is flushed*/
			pushSendQ(sendQ,msg,size,refcount);
			//			MACHSTATE3(3,"Xpmem msg %p pushed to sendQ length %d refcount %d",msg,sendQ->numEntries,msg->refcount);
			int sent = flushSendQ(sendQ);
			(*refcount)--; /*if it has been sent, can be deleted by caller, if not will be deleted when queue is flushed*/
			MACHSTATE1(3,"Xpmem flushSendQ sent %d messages",sent);
		 }
		 /* unlock the recvbuffer*/

#if XPMEM_OSSPINLOCK
		 OSSpinLockUnlock(&dstBuf->header->lock);
#elif XPMEM_LOCK
		 sem_post(dstBuf->mutex);
#elif XPMEM_FENCE
		 CmiMemoryReadFence(0,0);			
		 CmiMemoryWriteFence(0,0);
		 dstBuf->header->flagSender = 0;
#endif
	}
#if XPMEM_STATS
		xpmemContext->sendCount ++;
		xpmemContext->sendTime += (CmiWallTimer()-_startSendTime);
#endif
	MEMDEBUG(CmiMemoryCheck());

};

inline void emptyAllRecvBufs(void);
inline void flushAllSendQs(void);

/**********
 * Extract all the messages from the recvBuffers you can
 * Flush all sendQs
 * ***/
inline void CommunicationServerXpmem(void)
{
#if XPMEM_STATS
	double _startCommServerTime =CmiWallTimer();
#endif	
	MEMDEBUG(CmiMemoryCheck());

	emptyAllRecvBufs();
	flushAllSendQs();

#if XPMEM_STATS
	xpmemContext->commServerTime += (CmiWallTimer()-_startCommServerTime);
#endif
	MEMDEBUG(CmiMemoryCheck());
};

static void CmiNotifyStillIdleXpmem(CmiIdleState *s){
	CommunicationServerXpmem();
}


static void CmiNotifyBeginIdleXpmem(CmiIdleState *s)
{
	CmiNotifyStillIdle(s);
}

void calculateNodeSizeAndRank(char **argv)
{
	xpmemContext->nodesize=1;
	MACHSTATE(3,"calculateNodeSizeAndRank start");
	//CmiGetArgIntDesc(argv, "+nodesize", &(xpmemContext->nodesize),"Number of cores in this node (for non-smp case).Used by the shared memory communication layer");
	CmiGetArgIntDesc(argv, "+nodesize", &(xpmemContext->nodesize),"Number of cores in this node");
	MACHSTATE1(3,"calculateNodeSizeAndRank argintdesc %d",xpmemContext->nodesize);

	xpmemContext->noderank = _Cmi_mynode % (xpmemContext->nodesize);
	
	MACHSTATE1(3,"calculateNodeSizeAndRank noderank %d",xpmemContext->noderank);
	
	xpmemContext->nodestart = _Cmi_mynode -xpmemContext->noderank;
	
	MACHSTATE(3,"calculateNodeSizeAndRank nodestart ");

	xpmemContext->nodeend = xpmemContext->nodestart + xpmemContext->nodesize -1;

	if(xpmemContext->nodeend >= _Cmi_numnodes){
		xpmemContext->nodeend = _Cmi_numnodes-1;
		xpmemContext->nodesize = (xpmemContext->nodeend - xpmemContext->nodestart) +1;
	}
	
	MACHSTATE3(3,"calculateNodeSizeAndRank nodestart %d nodesize %d noderank %d",xpmemContext->nodestart,xpmemContext->nodesize,xpmemContext->noderank);
}

void allocBufNameStrings(char ***bufName);
void createRecvXpmemAndSems(sharedBufData **bufs,char **bufNames);
void createSendXpmemAndSems(sharedBufData **bufs,char **bufNames);
void removeXpmemFiles(void);

/***************
 * 	calculate the name of the shared objects and semaphores
 * 	
 * 	name scheme
 * 	shared memory: charm_pxshm_<recvernoderank>_<sendernoderank>  
 *  semaphore    : charm_pxshm_<recvernoderank>_<sendernoderank>.sem for semaphore for that shared object
 *                the semaphore name used by us is the same as the shared memory object name
 *                the posix library adds the semaphore tag // in linux at least . other machines might need more portable code
 *
 * 	open these shared objects and semaphores
 * *********/
void setupSharedBuffers(void){
	int i=0;
        
	allocBufNameStrings(&(xpmemContext->recvBufNames));
	
	allocBufNameStrings((&xpmemContext->sendBufNames));
	
	for(i=0;i<xpmemContext->nodesize;i++){
		if(i != xpmemContext->noderank){
			snprintf(xpmemContext->recvBufNames[i],NAMESTRLEN-1,"%s_%d_%d",xpmemContext->prefixStr,xpmemContext->noderank+xpmemContext->nodestart,i+xpmemContext->nodestart);
			MACHSTATE2(3,"recvBufName %s with rank %d",xpmemContext->recvBufNames[i],i)
			snprintf(xpmemContext->sendBufNames[i],NAMESTRLEN-1,"%s_%d_%d",xpmemContext->prefixStr,i+xpmemContext->nodestart,xpmemContext->noderank+xpmemContext->nodestart);
			MACHSTATE2(3,"sendBufName %s with rank %d",xpmemContext->sendBufNames[i],i);
		}
	}
	
	createRecvXpmemAndSems(&(xpmemContext->recvBufs),xpmemContext->recvBufNames);
        CmiBarrier();
	createSendXpmemAndSems(&(xpmemContext->sendBufs),xpmemContext->sendBufNames);
        CmiBarrier();
        removeXpmemFiles();
        freeSharedBuffers();
	
	for(i=0;i<xpmemContext->nodesize;i++){
		if(i != xpmemContext->noderank){
			//CmiAssert(xpmemContext->sendBufs[i].header->count == 0);
			xpmemContext->sendBufs[i].header->count = 0;
			xpmemContext->sendBufs[i].header->bytes = 0;
		}
	}
}

void allocBufNameStrings(char ***bufName)
{
	int i,count;
	int totalAlloc = sizeof(char)*NAMESTRLEN*(xpmemContext->nodesize-1);
	char *tmp = (char *) malloc(totalAlloc);
	
	MACHSTATE2(3,"allocBufNameStrings tmp %p totalAlloc %d",tmp,totalAlloc);

	*bufName = (char **)malloc(sizeof(char *)*xpmemContext->nodesize);
	for(i=0,count=0;i<xpmemContext->nodesize;i++){
		if(i != xpmemContext->noderank){
			(*bufName)[i] = &(tmp[count*NAMESTRLEN*sizeof(char)]);
			count++;
		}else{
			(*bufName)[i] = NULL;
		}
	}
}

__s64 createXpmemObject(int size,char **pPtr)
{
        struct xpmem_cmd_make make_info;
        int ret;

        *pPtr = (char*) mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, 0, 0);
        if (*pPtr == MAP_FAILED) {
            perror("Creating mapping.");
            return -1;
        }
        make_info.vaddr = (__u64) *pPtr;
        make_info.size = size;
        make_info.permit_type = XPMEM_PERMIT_MODE;
        make_info.permit_value = (__u64) 0600;
        ret = ioctl(xpmem_fd, XPMEM_CMD_MAKE, &make_info);
        if (ret != 0) {
            perror("xpmem_make");
            CmiAbort("xpmem_make");
        }
        return make_info.segid;
}

void attachXpmemObject(__s64 segid, int size, char **pPtr)
{
       int ret;
       __s64 apid;
       struct xpmem_cmd_get get_info;
       struct xpmem_cmd_attach attach_info;

       get_info.segid = segid;
       get_info.flags = XPMEM_RDWR;
       get_info.permit_type = XPMEM_PERMIT_MODE;
       get_info.permit_value = (__u64) NULL;
       ret = ioctl(xpmem_fd, XPMEM_CMD_GET, &get_info);
       if (ret != 0) {
               CmiAbort("xpmem_get");
       }
       apid = get_info.apid;

       attach_info.apid = get_info.apid;
       attach_info.offset = 0;
       attach_info.size = size;
       attach_info.vaddr = (__u64) NULL;
       attach_info.fd = xpmem_fd;
       attach_info.flags = 0;

       ret = ioctl(xpmem_fd, XPMEM_CMD_ATTACH, &attach_info);
       if (ret != 0) {
               CmiAbort("xpmem_attach");
       }

       *pPtr = (char *)attach_info.vaddr;
}

void createRecvXpmemAndSems(sharedBufData **bufs,char **bufNames){
	int i=0;
        __s64 *segid_arr;
        int size, pagesize = getpagesize();
	
	*bufs = (sharedBufData *)calloc(xpmemContext->nodesize, sizeof(sharedBufData));
        segid_arr = (__s64 *) malloc(sizeof(__s64)*xpmemContext->nodesize);
	
        size = XPMEMBUFLEN+sizeof(sharedBufHeader);
        size = ((~(pagesize-1))&(size+pagesize-1));

	for(i=0;i<xpmemContext->nodesize;i++){
	    if(i != xpmemContext->noderank)  {
                (*bufs)[i].segid = segid_arr[i] = createXpmemObject(size,(char **)&((*bufs)[i].header));
                memset(((*bufs)[i].header), 0, size);
		(*bufs)[i].data = ((char *)((*bufs)[i].header))+sizeof(sharedBufHeader);
#if XPMEM_OSSPINLOCK
		(*bufs)[i].header->lock = 0; // by convention(see man page) 0 means unlocked
#elif XPMEM_LOCK
		(*bufs)[i].mutex = sem_open(bufNames[i],O_CREAT, S_IRUSR | S_IWUSR,1);
#endif
	    }else{
		(*bufs)[i].header = NULL;
		(*bufs)[i].data = NULL;
#if XPMEM_LOCK
		(*bufs)[i].mutex = NULL;
#endif
	    }
	}	

        int fd;
        char fname[128];
        snprintf(fname, sizeof(fname), ".xpmem.%d", xpmemContext->nodestart+xpmemContext->noderank);
        fd = open(fname, O_CREAT|O_WRONLY, S_IRUSR|S_IWUSR);
        if (fd == -1) {
          CmiAbort("createShmObjectsAndSems failed");
        }
        write(fd, segid_arr, sizeof(__s64*)*xpmemContext->nodesize);
        close(fd);
        free(segid_arr);
}

void createSendXpmemAndSems(sharedBufData **bufs,char **bufNames)
{
        int i;
        int size, pagesize;

        pagesize = getpagesize();
        size = XPMEMBUFLEN+sizeof(sharedBufHeader);
        size = ((~(pagesize-1))&(size+pagesize-1));

	*bufs = (sharedBufData *)calloc(xpmemContext->nodesize, sizeof(sharedBufData));

	for(i=0;i<xpmemContext->nodesize;i++){
	    if(i != xpmemContext->noderank)  {
                __s64 segid;
                 char fname[128];
                 int fd;
                 snprintf(fname, sizeof(fname), ".xpmem.%d", xpmemContext->nodestart+i);
                 fd = open(fname, O_RDONLY);
                 if (fd == -1) {
                     CmiAbort("createShmObjectsAndSems failed");
                 }
                lseek(fd, xpmemContext->noderank*sizeof(__s64), SEEK_SET);
                read(fd, &segid, sizeof(__s64*));
                close(fd);
                (*bufs)[i].segid = segid;
                attachXpmemObject(segid, size,(char **)&((*bufs)[i].header));
                memset(((*bufs)[i].header), 0, XPMEMBUFLEN+sizeof(sharedBufHeader));
		(*bufs)[i].data = ((char *)((*bufs)[i].header))+sizeof(sharedBufHeader);
#if XPMEM_OSSPINLOCK
		(*bufs)[i].header->lock = 0; // by convention(see man page) 0 means unlocked
#elif XPMEM_LOCK
		(*bufs)[i].mutex = sem_open(bufNames[i],O_CREAT, S_IRUSR | S_IWUSR,1);
#endif
	    }else{
		(*bufs)[i].header = NULL;
		(*bufs)[i].data = NULL;
#if XPMEM_LOCK
		(*bufs)[i].mutex = NULL;
#endif
	    }
        }
}

void removeXpmemFiles(void)
{
        char fname[64];
        snprintf(fname, sizeof(fname), ".xpmem.%d", xpmemContext->nodestart+xpmemContext->noderank);
        unlink(fname);
}

void freeSharedBuffers(void){
	int i;
	for(i= 0;i<xpmemContext->nodesize;i++){
	    if(i != xpmemContext->noderank){
#if XPMEM_LOCK
		sem_unlink(xpmemContext->sendBufNames[i]);
		sem_unlink(xpmemContext->recvBufNames[i]);
#endif
	    }
	}
}

void tearDownSharedBuffers(void){
	int i;
	for(i= 0;i<xpmemContext->nodesize;i++){
	    if(i != xpmemContext->noderank){
#if XPMEM_LOCK
		sem_close(xpmemContext->recvBufs[i].mutex);
		sem_close(xpmemContext->sendBufs[i].mutex);
		sem_unlink(xpmemContext->sendBufNames[i]);
		sem_unlink(xpmemContext->recvBufNames[i]);
                xpmemContext->recvBufs[i].mutex = NULL;
                xpmemContext->sendBufs[i].mutex = NULL;
#endif
	    }
	}
};

void initSendQ(XpmemSendQ *q,int size,int rank);

void initAllSendQs(void){
	int i=0;
	xpmemContext->sendQs = (XpmemSendQ **) malloc(sizeof(XpmemSendQ *)*xpmemContext->nodesize);
	for(i=0;i<xpmemContext->nodesize;i++){
		if(i != xpmemContext->noderank){
			xpmemContext->sendQs[i] = (XpmemSendQ *)calloc(1, sizeof(XpmemSendQ));
			initSendQ((xpmemContext->sendQs)[i],SENDQSTARTSIZE,i);
		}else{
			xpmemContext->sendQs[i] = NULL;
		}
	}
};


/****************
 *copy this message into the sharedBuf
 If it does not succeed
 *put it into the sendQ 
 *NOTE: This method is called only after obtaining the corresponding mutex
 * ********/
int sendMessage(char *msg, int size, int *refcount, sharedBufData *dstBuf,XpmemSendQ *dstSendQ){

	if(dstBuf->header->bytes+size <= XPMEMBUFLEN){
		/**copy  this message to sharedBuf **/
		dstBuf->header->count++;
		CmiMemcpy(dstBuf->data+dstBuf->header->bytes,msg,size);
		dstBuf->header->bytes += size;
		//		MACHSTATE4(3,"Xpmem send done msg %p size %d dstBuf->header->count %d dstBuf->header->bytes %d",msg,msg->size,dstBuf->header->count,dstBuf->header->bytes);
                CmiFree(msg);
		return 1;
	}
	/***
	 * Shared Buffer is too full for this message
	 * **/
	//printf("[%d] send buffer is too full\n", CmiMyPe());
	pushSendQ(dstSendQ,msg,size,refcount);
	(*refcount)++;
	//	MACHSTATE3(3,"Xpmem send msg %p size %d queued refcount %d",ogm,ogm->size,ogm->refcount);
	return 0;
}

inline OutgoingMsgRec* popSendQ(XpmemSendQ *q);

/****
 *Try to send all the messages in the sendq to this destination rank
 *NOTE: This method is called only after obtaining the corresponding mutex
 * ************/

inline int flushSendQ(XpmemSendQ  *dstSendQ){
	sharedBufData *dstBuf = &(xpmemContext->sendBufs[dstSendQ->rank]);
	int count=dstSendQ->numEntries;
	int sent=0;
	while(count > 0){
		OutgoingMsgRec *ogm = popSendQ(dstSendQ);
		(*ogm->refcount)--;
		//		MACHSTATE4(3,"Xpmem trysending ogm %p size %d to dstRank %d refcount %d",ogm,ogm->size,dstSendQ->rank,ogm->refcount);
		int ret = sendMessageRec(ogm,dstBuf,dstSendQ);
		if(ret==1){
			sent++;
#if CMK_NET_VERSION
                        GarbageCollectMsg(ogm);
#endif
		}
		count--;
	}
	return sent;
}

inline void emptyRecvBuf(sharedBufData *recvBuf);

inline void emptyAllRecvBufs(void){
	int  i;
        for(i=0;i<xpmemContext->nodesize;i++){
                if(i != xpmemContext->noderank){
			sharedBufData *recvBuf = &(xpmemContext->recvBufs[i]);
			if(recvBuf->header->count > 0){

#if XPMEM_STATS
				xpmemContext->lockRecvCount++;
#endif

#if XPMEM_OSSPINLOCK
				if(! OSSpinLockTry(&recvBuf->header->lock)){
#elif XPMEM_LOCK
				if(sem_trywait(recvBuf->mutex) < 0){
#elif XPMEM_FENCE
				recvBuf->header->flagReceiver = 1;
				recvBuf->header->turn = SENDER;
				CmiMemoryReadFence(0,0);
				CmiMemoryWriteFence(0,0);
				//if((recvBuf->header->flagSender && recvBuf->header->turn == SENDER)){
				if((recvBuf->header->flagSender)){
					recvBuf->header->flagReceiver = 0;
#endif
				}else{


					MACHSTATE1(3,"emptyRecvBuf to be called for rank %d",i);			
					emptyRecvBuf(recvBuf);

#if XPMEM_OSSPINLOCK
					OSSpinLockUnlock(&recvBuf->header->lock);
#elif XPMEM_LOCK
					sem_post(recvBuf->mutex);
#elif XPMEM_FENCE
					CmiMemoryReadFence(0,0);
					CmiMemoryWriteFence(0,0);
					recvBuf->header->flagReceiver = 0;
#endif

				}
			
			}
		}
	}
};

inline void flushAllSendQs(void){
	int i;
#if SENDQ_LIST
        int index_prev = -1;

        i =  sendQ_head_index;
        while (i!= -1) {
                XpmemSendQ *sendQ = xpmemContext->sendQs[i];
                CmiAssert(i !=  xpmemContext->noderank);
		if(sendQ->numEntries > 0){
#else
        for(i=0;i<xpmemContext->nodesize;i++) {
                if (i == xpmemContext->noderank) continue;
                XpmemSendQ *sendQ = xpmemContext->sendQs[i];
                if(sendQ->numEntries > 0) {
#endif
	
#if XPMEM_OSSPINLOCK
		        if(OSSpinLockTry(&xpmemContext->sendBufs[i].header->lock)){
#elif XPMEM_LOCK
			if(sem_trywait(xpmemContext->sendBufs[i].mutex) >= 0){
#elif XPMEM_FENCE
			xpmemContext->sendBufs[i].header->flagSender = 1;
			xpmemContext->sendBufs[i].header->turn = RECEIVER;
			CmiMemoryReadFence(0,0);			
			CmiMemoryWriteFence(0,0);
			if(!(xpmemContext->sendBufs[i].header->flagReceiver && xpmemContext->sendBufs[i].header->turn == RECEIVER)){
#endif

				MACHSTATE1(3,"flushSendQ %d",i);
				flushSendQ(sendQ);

#if XPMEM_OSSPINLOCK	
				OSSpinLockUnlock(&xpmemContext->sendBufs[i].header->lock);
#elif XPMEM_LOCK
				sem_post(xpmemContext->sendBufs[i].mutex);
#elif XPMEM_FENCE
				CmiMemoryReadFence(0,0);			
				CmiMemoryWriteFence(0,0);
				xpmemContext->sendBufs[i].header->flagSender = 0;
#endif
			}else{

#if XPMEM_FENCE
			  xpmemContext->sendBufs[i].header->flagSender = 0;
#endif				

			}
		}        
#if SENDQ_LIST
                if (sendQ->numEntries == 0) {
                    if (index_prev != -1)
                        xpmemContext->sendQs[index_prev]->next = sendQ->next;
                    else
                        sendQ_head_index = sendQ->next;
                    i = sendQ->next;
                    sendQ->next = -2;
                }
                else {
                    index_prev = i;
                    i = sendQ->next;
                }
#endif
	}	
};

void static inline handoverXpmemMessage(char *newmsg,int total_size,int rank,int broot);

void emptyRecvBuf(sharedBufData *recvBuf){
 	int numMessages = recvBuf->header->count;
	int i;

	char *ptr=recvBuf->data;

	for(i=0;i<numMessages;i++){
		int size;
		int rank, srcpe, seqno, magic, i;
		unsigned int broot;
		char *msg = ptr;
		char *newMsg;

#if CMK_NET_VERSION
		DgramHeaderBreak(msg, rank, srcpe, magic, seqno, broot);
		size = CMI_MSG_SIZE(msg);
#else
                size = CmiGetMsgSize(msg);
#endif
	
		newMsg = (char *)CmiAlloc(size);
		memcpy(newMsg,msg,size);

#if CMK_NET_VERSION
		handoverPxshmMessage(newMsg,size,rank,broot);
#else
                handleOneRecvedMsg(size, newMsg);
#endif
		
		ptr += size;

		MACHSTATE3(3,"message of size %d recvd ends at ptr-data %d total bytes %d bytes %d",size,ptr-recvBuf->data,recvBuf->header->bytes);
	}
#if 1
  if(ptr - recvBuf->data != recvBuf->header->bytes){
		CmiPrintf("[%d] ptr - recvBuf->data  %d recvBuf->header->bytes %d numMessages %d \n",_Cmi_mynode, ptr - recvBuf->data, recvBuf->header->bytes,numMessages);
	}
#endif
	CmiAssert(ptr - recvBuf->data == recvBuf->header->bytes);
	recvBuf->header->count=0;
	recvBuf->header->bytes=0;
}


#if CMK_NET_VERSION
void static inline handoverPxshmMessage(char *newmsg,int total_size,int rank,int broot){
	CmiAssert(rank == 0);
#if CMK_BROADCAST_SPANNING_TREE
        if (rank == DGRAM_BROADCAST
#if CMK_NODE_QUEUE_AVAILABLE
          || rank == DGRAM_NODEBROADCAST
#endif
         ){
          	SendSpanningChildren(NULL, 0, total_size, newmsg,broot,rank);
					}
#elif CMK_BROADCAST_HYPERCUBE
        if (rank == DGRAM_BROADCAST
#if CMK_NODE_QUEUE_AVAILABLE
          || rank == DGRAM_NODEBROADCAST
#endif
         ){
          		SendHypercube(NULL, 0, total_size, newmsg,broot,rank);
					}
#endif

		switch (rank) {
    	case DGRAM_BROADCAST: {
          CmiPushPE(0, newmsg);
          break;
      }
        default:
				{
					
          CmiPushPE(rank, newmsg);
				}
  	}    /* end of switch */
}
#endif


/**************************
 *sendQ helper functions
 * ****************/

void initSendQ(XpmemSendQ *q,int size,int rank){
	q->data = (OutgoingMsgRec *)calloc(size, sizeof(OutgoingMsgRec));

	q->size = size;
	q->numEntries = 0;

	q->begin = 0;
	q->end = 0;

        q->rank = rank;
#if SENDQ_LIST
        q->next = -2;
#endif
}

void pushSendQ(XpmemSendQ *q, char *msg, int size, int *refcount){
	if(q->numEntries == q->size){
		//need to resize 
		OutgoingMsgRec *oldData = q->data;
		int newSize = q->size<<1;
		q->data = (OutgoingMsgRec *)calloc(newSize, sizeof(OutgoingMsgRec));
		//copy head to the beginning of the new array
		CmiAssert(q->begin == q->end);

		CmiAssert(q->begin < q->size);
		memcpy(&(q->data[0]),&(oldData[q->begin]),sizeof(OutgoingMsgRec)*(q->size - q->begin));

		if(q->end!=0){
			memcpy(&(q->data[(q->size - q->begin)]),&(oldData[0]),sizeof(OutgoingMsgRec)*(q->end));
		}
		free(oldData);
		q->begin = 0;
		q->end = q->size;
		q->size = newSize;
	}
	OutgoingMsgRec *omg = &q->data[q->end];
        omg->size = size;
        omg->data = msg;
        omg->refcount = refcount;
	(q->end)++;
	if(q->end >= q->size){
		q->end -= q->size;
	}
	q->numEntries++;
}

OutgoingMsgRec * popSendQ(XpmemSendQ *q){
	OutgoingMsgRec * ret;
	if(0 == q->numEntries){
		return NULL;
	}

	ret = &q->data[q->begin];
	(q->begin)++;
	if(q->begin >= q->size){
		q->begin -= q->size;
	}
	
	q->numEntries--;
	return ret;
}
