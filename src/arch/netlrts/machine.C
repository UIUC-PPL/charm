
/** @file
 * Basic NET-LRTS implementation of Converse machine layer
 * @ingroup NET
 */

/** @defgroup NET
 * NET implementation of machine layer, ethernet in particular
 * @ingroup Machine
 *
 * THE DATAGRAM STREAM
 *
 * Messages are sent using UDP datagrams.  The sender allocates a
 * struct for each datagram to be sent.  These structs stick around
 * until slightly after the datagram is acknowledged.
 *
 * Datagrams are transmitted node-to-node (as opposed to pe-to-pe).
 * Each node has an OtherNode struct for every other node in the
 * system.  The OtherNode struct contains:
 *
 *   send_queue   (all datagram-structs not yet transmitted)
 *   send_window  (all datagram-structs transmitted but not ack'd)
 *
 * When an acknowledgement comes in, all packets in the send-window
 * are either marked as acknowledged or pushed back into the send
 * queue for retransmission.
 *
 * THE OUTGOING MESSAGE
 *
 * When you send or broadcast a message, the first thing the system
 * does is system creates an OutgoingMsg struct to represent the
 * operation.  The OutgoingMsg contains a very direct expression
 * of what you want to do:
 *
 * OutgoingMsg:
 *
 *   size      --- size of message in bytes
 *   data      --- pointer to the buffer containing the message
 *   src       --- processor which sent the message
 *   dst       --- destination processor (-1=broadcast, -2=broadcast all)
 *   freemode  --- see below.
 *   refcount  --- see below.
 *
 * The OutgoingMsg is kept around until the transmission is done, then
 * it is garbage collected --- the refcount and freemode fields are
 * to assist garbage collection.
 *
 * The freemode indicates which kind of buffer-management policy was
 * used (sync, async, or freeing).  The sync policy is handled
 * superficially by immediately converting sync sends into freeing
 * sends.  Thus, the freemode can either be 'A' (async) or 'F'
 * (freeing).  If the freemode is 'F', then garbage collection
 * involves freeing the data and the OutgoingMsg structure itself.  If
 * the freemode is 'A', then the only cleanup is to change the
 * freemode to 'X', a condition which is then detectable by
 * CmiAsyncMsgSent.  In this case, the actual freeing of the
 * OutgoingMsg is done by CmiReleaseCommHandle.
 *
 * When the transmission is initiated, the system computes how many
 * datagrams need to be sent, total.  This number is stored in the
 * refcount field.  Each time a datagram is delivered, the refcount
 * is decremented, when it reaches zero, cleanup is performed.  There
 * are two exceptions to this rule.  Exception 1: if the OutgoingMsg
 * is a send (not a broadcast) and can be performed with shared
 * memory, the entire datagram system is bypassed, the message is
 * simply delivered and freed, not using the refcount mechanism at
 * all.  Exception 2: If the message is a broadcast, then part of the
 * broadcast that can be done via shared memory is performed prior to
 * initiating the datagram/refcount system.
 *
 * DATAGRAM FORMATS AND MESSAGE FORMATS
 *
 * Datagrams have this format:
 *
 *   srcpe   (16 bits) --- source processor number.
 *   magic   ( 8 bits) --- magic number to make sure DG is good.
 *   dstrank ( 8 bits) --- destination processor rank.
 *   seqno   (32 bits) --- packet sequence number.
 *   data    (XX byte) --- user data.
 *
 * The only reason the srcpe is in there is because the receiver needs
 * to know which receive window to use.  The dstrank field is needed
 * because transmission is node-to-node.  Once the message is
 * assembled by the node, it must be delivered to the appropriate PE.
 * The dstrank field is used to encode certain special-case scenarios.
 * If the dstrank is DGRAM_BROADCAST, the transmission is a broadcast,
 * and should be delivered to all processors in the node.  If the dstrank
 * is DGRAM_ACKNOWLEDGE, the datagram is an acknowledgement datagram, in
 * which case the srcpe is the number of the acknowledger, the seqno is
 * always zero, and the user data is a list of the seqno's being
 * acknowledged.  There may be other dstrank codes for special functions.
 *
 * To send a message, one chops it up into datagrams and stores those
 * datagrams in a send-queue.  These outgoing datagrams aren't stored
 * in the explicit format shown above.  Instead, they are stored as
 * ImplicitDgrams, which contain the datagram header and a pointer to
 * the user data (which is in the user message buffer, which is in the
 * OutgoingMsg).  At transmission time these are combined together.

 * The combination of the datagram header with the user's data is
 * performed right in the user's message buffer.  Note that the
 * datagram header is exactly 64 bits.  One simply overwrites 64 bits
 * of the user's message with a datagram header, sends the datagram
 * straight from the user's message buffer, then restores the user's
 * buffer to its original state.  There is a small problem with the
 * first datagram of the message: one needs 64 bits of space to store
 * the datagram header.  To make sure this space is there, we added a
 * 64-bit unused space to the front of the Cmi message header.  In
 * addition to this, we also add 32 bits to the Cmi message header
 * to make room for a length-field, making it possible to identify
 * message boundaries.
 *
 * CONCURRENCY CONTROL
 *
 * This has changed recently.
 *
 * EFFICIENCY NOTES
 *
 * The sender-side does little copying.  The async and freeing send
 * routines do no copying at all.  The sync send routines copy the
 * message, then use the freeing-send routines.  The other alternative
 * is to not copy the message, and use the async send mechanism
 * combined with a blocking wait.  Blocking wait seems like a bad
 * idea, since it could take a VERY long time to get all those
 * datagrams out the door.
 *
 * The receiver side, unfortunately, must copy.  To avoid copying,
 * it would have to receive directly into a preallocated message buffer.
 * Unfortunately, this can't work: there's no way to know how much
 * memory to preallocate, and there's no way to know which datagram
 * is coming next.  Thus, we receive into fixed-size (large) datagram
 * buffers.  These are then inspected, and the messages extracted from
 * them.
 *
 * Note that we are allocating a large number of structs: OutgoingMsg's,
 * ImplicitDgrams, ExplicitDgrams.  By design, each of these structs
 * is a fixed-size structure.  Thus, we can do memory allocation by
 * simply keeping a linked-list of unused structs around.  The only
 * place where expensive memory allocation is performed is in the
 * sync routines.
 *
 * Since the datagrams from one node to another are fully ordered,
 * there is slightly more ordering than is needed: in theory, the
 * datagrams of one message don't need to be ordered relative to the
 * datagrams of another.  This was done to simplify the sequencing
 * mechanisms: implementing a fully-ordered stream is much simpler
 * than a partially-ordered one.  It also makes it possible to
 * modularize, layering the message transmitter on top of the
 * datagram-sequencer.  In other words, it was just easier this way.
 * Hopefully, this won't cause serious degradation: LAN's rarely get
 * datagrams out of order anyway.
 *
 * A potential efficiency problem is the lack of message-combining.
 * One datagram could conceivably contain several messages.  This
 * might be more efficient, it's not clear how much overhead is
 * involved in sending a short datagram.  Message-combining isn't
 * really ``integrated'' into the design of this software, but you
 * could fudge it as follows.  Whenever you pull a short datagram from
 * the send-queue, check the next one to see if it's also a short
 * datagram.  If so, pack them together into a ``combined'' datagram.
 * At the receive side, simply check for ``combined'' datagrams, and
 * treat them as if they were simply two datagrams.  This would
 * require extra copying.  I have no idea if this would be worthwhile.
 *
 *****************************************************************************/

/**
 * @addtogroup NET
 * @{
 */

/*****************************************************************************
 *
 * Include Files
 *
 ****************************************************************************/

#define _GNU_SOURCE 1
#include <stdarg.h> /*<- was <varargs.h>*/

#define CMK_USE_PRINTF_HACK 0
#if CMK_USE_PRINTF_HACK
/*HACK: turn printf into CmiPrintf, by just defining our own
external symbol "printf".  This may be more trouble than it's worth,
since the only advantage is that it works properly with +syncprint.

This version *won't* work with fprintf(stdout,...) or C++ or Fortran I/O,
because they don't call printf.  Has to be defined up here because we probably 
haven't properly guessed this compiler's prototype for "printf".
*/
static void InternalPrintf(const char *f, va_list l);
int printf(const char *fmt, ...) {
	int nChar;
	va_list p; va_start(p, fmt);
        InternalPrintf(fmt,p);
	va_end(p);
	return 10;
}
#endif


#include "converse.h"
#include "cmirdmautils.h"
#include "memory-isomalloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <fcntl.h>
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

/* define machine debug */
#include "machine.h"
static int        Cmi_charmrun_pid;
static int        Cmi_charmrun_fd = -1;

extern int quietMode;

/******************* Producer-Consumer Queues ************************/
#include "pcqueue.h"

#include "machine-smp.h"

#include "machine-lrts.h"
#include "machine-common-core.C"

#if CMK_USE_KQUEUE
#include <sys/event.h>
int _kq = -1;
#endif

#if CMK_USE_POLL
#include <poll.h>
#endif

#include "conv-ccs.h"
#include "ccs-server.h"
#include "sockRoutines.h"

#if defined(_WIN32)
/*For windows systems:*/
#  include <windows.h>
#  include <wincon.h>
#  include <sys/types.h>
#  include <sys/timeb.h>
#  define fdopen _fdopen
#  define SIGBUS -1  /*These signals don't exist in Win32*/
#  define SIGKILL -1
#  define SIGQUIT -1
/*#  define SIGTERM -1*/       /* VC++ ver 8 now has SIGTERM */

#else /*UNIX*/
#  include <pwd.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <sys/file.h>
#endif

#if CMK_PERSISTENT_COMM
#include "machine-persistent.C"
#endif

#define PRINTBUFSIZE 16384

#include "machine-rdma.h"
#if CMK_ONESIDED_IMPL
#include "machine-onesided.h"
#endif

#if CMK_SHRINK_EXPAND
extern void resumeAfterRealloc(void);
extern char willContinue;
extern int mynewpe;
extern int numProcessAfterRestart;
CcsDelayedReply shrinkExpandreplyToken;
extern char *_shrinkexpand_basedir;
#endif

static void CommunicationServerNet(int withDelayMs, int where);
//static void CommunicationServer(int withDelayMs);

void CmiHandleImmediate(void);
extern int CmemInsideMem(void);
extern void CmemCallWhenMemAvail(void);

static unsigned int dataport=0;
static SOCKET       dataskt;

extern void TokenUpdatePeriodic(void);
extern void getAvailSysMem(void);

static int Lrts_numNodes;
static int Lrts_myNode;

/****************************************************************************
 *
 * Handling Errors
 *
 * Errors should be handled by printing a message on stderr and
 * calling exit(1).  Nothing should be sent to charmrun, no attempt at
 * communication should be made.  The other processes will notice the
 * abnormal termination and will deal with it.
 *
 * Rationale: if an error triggers an attempt to send a message,
 * the attempt to send a message is likely to trigger another error,
 * leading to an infinite loop and a process that spins instead of
 * shutting down.
 *
 *****************************************************************************/

static int machine_initiated_shutdown=0;
static int already_in_signal_handler=0;

static void CmiDestroyLocks(void);

void EmergencyExit(void);
void MachineExit(void);

static void machine_exit(int status)
{
  MACHSTATE(3,"     machine_exit");
  machine_initiated_shutdown=1;

  CmiDestroyLocks();		/* destroy locks to prevent dead locking */
  EmergencyExit();

  MachineExit();
  exit(status);
}

static void charmrun_abort(const char*);

static void KillEveryone(const char *msg)
{
  charmrun_abort(msg);
  machine_exit(1);
}

static void KillEveryoneCode(int n)
{
  char _s[100];
  sprintf(_s, "[%d] XXX Fatal error #%d\n", CmiMyPe(), n);
  charmrun_abort(_s);
  machine_exit(1);
}

CpvCExtern(int, freezeModeFlag);

static int Cmi_truecrash;

static void KillOnAllSigs(int sigNo)
{
  const char *sig="unknown signal";
  const char *suggestion="";
  if (machine_initiated_shutdown ||
      already_in_signal_handler) 
  	machine_exit(1); /*Don't infinite loop if there's a signal during a signal handler-- just die.*/
  already_in_signal_handler=1;

#if CMK_CCS_AVAILABLE
  if (cmiArgDebugFlag && CmiMyRank()==0) {
    int reply = 0;
    CpdNotify(CPD_SIGNAL,sigNo);
    CcsSendReplyNoError(4,&reply);/*Send an empty reply if not*/
    CpvAccess(freezeModeFlag) = 1;
    CpdFreezeModeScheduler();
  }
#endif
  

  if (sigNo==SIGSEGV) {
     sig="segmentation violation";
     suggestion="Try running with '++debug', or linking with '-memory paranoid' (memory paranoid requires '+netpoll' at runtime).";
  }
  if (sigNo==SIGFPE) {
     sig="floating point exception";
     suggestion="Check for integer or floating-point division by zero.";
  }
  if (sigNo==SIGBUS) {
     sig="bus error";
     suggestion="Check for misaligned reads or writes to memory.";
  }
  if (sigNo==SIGILL) {
     sig="illegal instruction";
     suggestion="Check for calls to uninitialized function pointers.";
  }
  if (sigNo==SIGKILL) sig="caught signal KILL";
  if (sigNo==SIGQUIT) sig="caught signal QUIT";
  if (sigNo==SIGTERM) sig="caught signal TERM";
  MACHSTATE1(5,"     Caught signal %s ",sig);
/*ifdef this part*/
#ifdef __FAULT__
  if(sigNo == SIGKILL || sigNo == SIGQUIT || sigNo == SIGTERM){
		CmiPrintf("[%d] Caught but ignoring signal\n",CmiMyPe());
  } else
#endif
  {
    Cmi_truecrash = 0;
    CmiAbortHelper("Caught Signal", sig, suggestion, 0, 1);
  }
}

static void machine_atexit_check(void)
{
  if (!machine_initiated_shutdown)
    CmiAbort("unexpected call to exit by user program. Must use CkExit, not exit!");
#if 0 /*Wait for the user to press any key (for Win32 debugging)*/
  fgetc(stdin);
#endif
}

#if !defined(_WIN32)
static void HandleUserSignals(int signum)
{
  int condnum = ((signum==SIGUSR1) ? CcdSIGUSR1 : CcdSIGUSR2);
  CcdRaiseCondition(condnum);
}
#endif

/*****************************************************************************
 *
 *     Utility routines for network machine interface.
 *
 *****************************************************************************/

/*
Horrific #defines to hide the differences between select() and poll().
 */
#if CMK_USE_POLL /*poll() version*/
# define CMK_PIPE_DECL(delayMs) \
	struct pollfd fds[10]; \
	int nFds_sto=0; int *nFds=&nFds_sto; \
	int pollDelayMs=delayMs;
# define CMK_PIPE_SUB fds,nFds
# define CMK_PIPE_CALL() poll(fds, *nFds, pollDelayMs); *nFds=0

# define CMK_PIPE_PARAM struct pollfd *fds,int *nFds
# define CMK_PIPE_ADDREAD(rd_fd) \
	do {fds[*nFds].fd=rd_fd; fds[*nFds].events=POLLIN; (*nFds)++;} while(0)
# define CMK_PIPE_ADDWRITE(wr_fd) \
	do {fds[*nFds].fd=wr_fd; fds[*nFds].events=POLLOUT; (*nFds)++;} while(0)
# define CMK_PIPE_CHECKREAD(rd_fd) fds[(*nFds)++].revents&POLLIN
# define CMK_PIPE_CHECKWRITE(wr_fd) fds[(*nFds)++].revents&POLLOUT

#elif CMK_USE_KQUEUE /* kqueue version */

# define CMK_PIPE_DECL(delayMs) \
        if (_kq == -1) _kq = kqueue(); \
    struct kevent ke_sto; \
    struct kevent* ke = &ke_sto; \
    struct timespec tmo; \
    tmo.tv_sec = 0; tmo.tv_nsec = delayMs*1e6;
# define CMK_PIPE_SUB ke
# define CMK_PIPE_CALL() kevent(_kq, NULL, 0, ke, 1, &tmo)

# define CMK_PIPE_PARAM struct kevent* ke
# define CMK_PIPE_ADDREAD(rd_fd) \
        do { EV_SET(ke, rd_fd, EVFILT_READ, EV_ADD, 0, 10, NULL); \
                kevent(_kq, ke, 1, NULL, 0, NULL); memset(ke, 0, sizeof(*ke));} while(0)
# define CMK_PIPE_ADDWRITE(wr_fd) \
        do { EV_SET(ke, wr_fd, EVFILT_WRITE, EV_ADD, 0, 10, NULL); \
                kevent(_kq, ke, 1, NULL, 0, NULL); memset(ke, 0, sizeof(*ke));} while(0)
# define CMK_PIPE_CHECKREAD(rd_fd) (ke->ident == rd_fd && ke->filter == EVFILT_READ)
# define CMK_PIPE_CHECKWRITE(wr_fd) (ke->ident == wr_fd && ke->filter == EVFILT_WRITE)

#else /*select() version*/

# define CMK_PIPE_DECL(delayMs) \
	fd_set rfds_sto,wfds_sto;\
	fd_set *rfds=&rfds_sto,*wfds=&wfds_sto; struct timeval tmo; \
	FD_ZERO(rfds); FD_ZERO(wfds);tmo.tv_sec=0; tmo.tv_usec=1000*delayMs;
# define CMK_PIPE_SUB rfds,wfds
# define CMK_PIPE_CALL() select(FD_SETSIZE, rfds, wfds, NULL, &tmo)

# define CMK_PIPE_PARAM fd_set *rfds,fd_set *wfds
# define CMK_PIPE_ADDREAD(rd_fd) FD_SET(rd_fd,rfds)
# define CMK_PIPE_ADDWRITE(wr_fd) FD_SET(wr_fd,wfds)
# define CMK_PIPE_CHECKREAD(rd_fd) FD_ISSET(rd_fd,rfds)
# define CMK_PIPE_CHECKWRITE(wr_fd) FD_ISSET(wr_fd,wfds)
#endif

static void CMK_PIPE_CHECKERR(void) {
#if defined(_WIN32)
/* Win32 socket seems to randomly return inexplicable errors
here-- WSAEINVAL, WSAENOTSOCK-- yet everything is actually OK. 
        int err=WSAGetLastError();
        CmiPrintf("(%d)Select returns -1; errno=%d, WSAerr=%d\n",withDelayMs,errn
o,err);
*/
#else /*UNIX machine*/
        if (errno!=EINTR)
                KillEveryone("Socket error in CheckSocketsReady!\n");
#endif
}


static void CmiStdoutFlush(void);
static int  CmiStdoutNeedsService(void);
static void CmiStdoutService(void);
static void CmiStdoutAdd(CMK_PIPE_PARAM);
static void CmiStdoutCheck(CMK_PIPE_PARAM);


double GetClock(void)
{
#if defined(_WIN32)
  struct _timeb tv; 
  _ftime(&tv);
  return (tv.time * 1.0 + tv.millitm * 1.0E-3);
#else
  struct timeval tv; int ok;
  ok = gettimeofday(&tv, NULL);
  if (ok<0) { perror("gettimeofday"); KillEveryoneCode(9343112); }
  return (tv.tv_sec * 1.0 + tv.tv_usec * 1.0E-6);
#endif
}


/***********************************************************************
 *
 * Abort function:
 *
 ************************************************************************/

static int already_aborting=0;
void LrtsAbort(const char *message)
{
  if (already_aborting) machine_exit(1);
  already_aborting=1;
  MACHSTATE1(5,"CmiAbort(%s)",message);

  /*Send off any remaining prints*/
  CmiStdoutFlush();
  
  if(Cmi_truecrash) {
    printf("CHARM++ FATAL ERROR: %s\n", message);
    volatile int* ptr = NULL;
    *ptr = 0; /*Write to null, causing bus error*/
  } else {
    charmrun_abort(message);
    machine_exit(1);
  }
  CMI_NORETURN_FUNCTION_END
}


/******************************************************************************
 *
 * CmiEnableAsyncIO
 *
 * The net and tcp versions use a bunch of unix processes talking to each
 * other via file descriptors.  We need for a signal SIGIO to be generated
 * each time a message arrives, making it possible to write a signal
 * handler to handle the messages.  The vast majority of unixes can,
 * in fact, do this.  However, there isn't any standard for how this is
 * supposed to be done, so each version of UNIX has a different set of
 * calls to turn this signal on.  So, there is like one version here for
 * every major brand of UNIX.
 *
 *****************************************************************************/

#if CMK_ASYNC_USE_F_SETFL_AND_F_SETOWN
#include <fcntl.h>
void CmiEnableAsyncIO(int fd)
{
  if ( fcntl(fd, F_SETOWN, getpid()) < 0 ) {
    CmiError("setting socket owner: %s\n", strerror(errno)) ;
    exit(1);
  }
  if ( fcntl(fd, F_SETFL, O_ASYNC) < 0 ) {
    CmiError("setting socket async: %s\n", strerror(errno)) ;
    exit(1);
  }
}
#else
void CmiEnableAsyncIO(int fd) { }
#endif

/* We should probably have a set of "CMK_NONBLOCK_USE_..." defines here:*/
#if !defined(_WIN32)
void CmiEnableNonblockingIO(int fd) {
  int on=1;
  if (fcntl(fd,F_SETFL,O_NONBLOCK,&on)<0) {
    CmiError("setting nonblocking IO: %s\n", strerror(errno)) ;
    exit(1);
  }
}
#else
void CmiEnableNonblockingIO(int fd) { }
#endif


/******************************************************************************
*
* Configuration Data
*
* This data is all read in from the NETSTART variable (provided by the
* charmrun) and from the command-line arguments.  Once read in, it is never
 * modified.
 *
 *****************************************************************************/

static skt_ip_t   Cmi_self_IP;
static skt_ip_t   Cmi_charmrun_IP; /*Address of charmrun machine*/
static int        Cmi_charmrun_port;
/* Magic number to be used for sanity check in messege header */
static int 				Cmi_net_magic;

static int    Cmi_netpoll;
static int    Cmi_asyncio;
static int    Cmi_idlepoll;
static int    Cmi_syncprint;
static int Cmi_print_stats = 0;

#if CMK_SHRINK_EXPAND
int    Cmi_isOldProcess = 0; // means this process was already there
static int    Cmi_mynewpe = 0;
static int    Cmi_oldpe = 0;
static int    Cmi_newnumnodes = 0;
int    Cmi_myoldpe = 0;
static int Cmi_charmrun_assigned_pe;
#endif

extern int    CmiMyLocalRank;

#if ! defined(_WIN32)
/* parse forks only used in non-smp mode */
static void parse_forks(void) {
  char *forkstr;
  int nread;
  int forks;
  int i,pid;
  forkstr=getenv("CmiMyForks");
  if(forkstr!=0) { /* charmrun */
	nread = sscanf(forkstr,"%d",&forks);
          /* CmiMyLocalRank is used for setting default cpu affinity */
        CmiMyLocalRank = 0;
	for(i=1;i<=forks;i++) { /* by default forks = 0 */ 
		pid=fork();
		if(pid<0) CmiAbort("Fork returned an error");
		if(pid==0) { /* forked process */
			/* reset mynode,pe & exit loop */
			CmiMyLocalRank = i;
			Lrts_myNode+=i;
#if CMK_SHRINK_EXPAND
			Cmi_charmrun_assigned_pe += i;
#endif
#if ! CMK_SMP
                        _Cmi_mype+=i;
#endif
			break;
		}
	}
  }
}
#endif

static void parse_magic(void)
{
	char* nm;	
	int nread;
  nm = getenv("NETMAGIC");
  if (nm!=0) 
  {/*Read values set by Charmrun*/
        nread = sscanf(nm, "%d",&Cmi_net_magic);
	}
}

static void parse_netstart(void)
{
  char *ns;
  int nread;
  int port;
  ns = getenv("NETSTART");
  if (ns!=0) 
  {/*Read values set by Charmrun*/
        char Cmi_charmrun_name[1024];
        nread = sscanf(ns, "%d%s%d%d%d",
                 &Lrts_myNode,
                 Cmi_charmrun_name, &Cmi_charmrun_port,
                 &Cmi_charmrun_pid, &port);
	Cmi_charmrun_IP=skt_lookup_ip(Cmi_charmrun_name);

        if (nread!=5) {
                fprintf(stderr,"Error parsing NETSTART '%s'\n",ns);
                exit(1);
        }
#if CMK_SHRINK_EXPAND
    Cmi_charmrun_assigned_pe = Lrts_myNode;
    if (Cmi_isOldProcess) {
      Cmi_myoldpe = Lrts_myNode;
    }
#endif
    if (getenv("CmiLocal") != NULL) {      /* ++local */
          /* CmiMyLocalRank is used for setting default cpu affinity */
        CmiMyLocalRank = Lrts_myNode;
    }

  } else 
  {/*No charmrun-- set flag values for standalone operation*/
  	Lrts_myNode=0;
  	Cmi_charmrun_IP=_skt_invalid_ip;
  	Cmi_charmrun_port=0;
  	Cmi_charmrun_pid=0;
        dataport = -1;
#if CMK_USE_PXSHM
    CmiAbort("pxshm must be run with charmrun");
#endif
  }
}

static void extract_common_args(char **argv)
{
  if (CmiGetArgFlagDesc(argv,"+stats","Print network statistics at shutdown"))
    Cmi_print_stats = 1;
#if CMK_SHRINK_EXPAND
  //Realloc specific args
  CmiGetArgIntDesc(argv,"+mynewpe",&Cmi_mynewpe,"New PE after realloc");
  CmiGetArgIntDesc(argv,"+myoldpe",&Cmi_oldpe,"New PE after realloc");
  CmiGetArgIntDesc(argv,"+newnumpes",&Cmi_newnumnodes,"New num PEs after realloc");
#endif
}


/******************************************************************************
 *
 * Packet Performance Logging
 *
 * This module is designed to give a detailed log of the packets and their
 * acknowledgements, for performance tuning.  It can be disabled.
 *
 *****************************************************************************/

#define LOGGING 0

#if LOGGING

typedef struct logent {
  double time;
  int seqno;
  int srcpe;
  int dstpe;
  int kind;
} *logent;


logent log;
int    log_pos;
int    log_wrap;

static void log_init(void)
{
  log = (logent)malloc(50000 * sizeof(struct logent));
  _MEMCHECK(log);
  log_pos = 0;
  log_wrap = 0;
}

static void log_done(void)
{
  char logname[100]; FILE *f; int i, size;
  sprintf(logname, "log.%d", Lrts_myNode);
  f = fopen(logname, "w");
  if (f==0) KillEveryone("fopen problem");
  if (log_wrap) size = 50000; else size=log_pos;
  for (i=0; i<size; i++) {
    logent ent = log+i;
    fprintf(f, "%1.4f %d %c %d %d\n",
	    ent->time, ent->srcpe, ent->kind, ent->dstpe, ent->seqno);
  }
  fclose(f);
}

void printLog(void)
{
  char logname[100]; FILE *f; int i, j, size;
  static int logged = 0;
  if (logged)
      return;
  logged = 1;
  CmiPrintf("Logging: %d\n", Lrts_myNode);
  sprintf(logname, "log.%d", Lrts_myNode);
  f = fopen(logname, "w");
  if (f==0) KillEveryone("fopen problem");
  for (i = 5000; i; i--)
  {
  /*for (i=0; i<size; i++) */
    j = log_pos - i;
    if (j < 0)
    {
        if (log_wrap)
	    j = 5000 + j;
	else
	    j = 0;
    };
    {
    logent ent = log+j;
    fprintf(f, "%1.4f %d %c %d %d\n",
	    ent->time, ent->srcpe, ent->kind, ent->dstpe, ent->seqno);
    }
  }
  fclose(f);
  CmiPrintf("Done Logging: %d\n", Lrts_myNode);
}

#define LOG(t,s,k,d,q) { if (log_pos==50000) { log_pos=0; log_wrap=1;} { logent ent=log+log_pos; ent->time=t; ent->srcpe=s; ent->kind=k; ent->dstpe=d; ent->seqno=q; log_pos++; }}

#endif

#if !LOGGING

#define log_init() /*empty*/
#define log_done() /*empty*/
#define printLog() /*empty*/
#define LOG(t,s,k,d,q) /*empty*/

#endif

/******************************************************************************
 *
 * Node state
 *
 *****************************************************************************/


static CmiNodeLock    Cmi_comm_var_mutex;
static CmiNodeLock    Cmi_scanf_mutex;
static double         Cmi_clock;
static double         Cmi_check_delay = 3.0;

/******************************************************************************
 *
 * OS Threads
 * SMP implementation moved to machine-smp.C
 *****************************************************************************/
int* inProgress;

/** Mechanism to prevent dual locking when comm-layer functions, including prints, 
 * are called recursively. (UN)LOCK_IF_AVAILABLE is used before and after a code piece
 * which is guaranteed not to make any-recursive locking calls. (UN)LOCK_AND_(UN)SET 
 * is used before and after a code piece that may make recursive locking calls.
 */

#define LOCK_IF_AVAILABLE() \
  if(!inProgress[CmiMyRank()]) { \
    CmiCommLock(); \
  }
 
#define UNLOCK_IF_AVAILABLE() \
  if(!inProgress[CmiMyRank()]) { \
    CmiCommUnlock(); \
  }
   
#define LOCK_AND_SET() \
    if(!inProgress[CmiMyRank()]) { \
      CmiCommLock(); \
      acqLock = 1; \
    } \
    inProgress[CmiMyRank()] += 1;

#define UNLOCK_AND_UNSET() \
    if(acqLock) { \
      CmiCommUnlock(); \
      acqLock = 0; \
    } \
    inProgress[CmiMyRank()] -= 1;

/************************ No kernel SMP threads ***************/
//XX
#if CMK_SHARED_VARS_UNAVAILABLE

static volatile int memflag=0;
void CmiMemLockNet(void) { memflag++; }
void CmiMemUnlockNet(void) { memflag--; }

static volatile int comm_flag=0;
#define CmiCommLockOrElse(dothis) if (comm_flag!=0) dothis
#ifndef MACHLOCK_DEBUG
#  define CmiCommLock() (comm_flag=1)
#  define CmiCommUnlock() (comm_flag=0)
#else /* Error-checking flag locks */
void CmiCommLock(void) {
  MACHLOCK_ASSERT(!comm_flag,"CmiCommLock");
  comm_flag=1;
}
void CmiCommUnlock(void) {
  MACHLOCK_ASSERT(comm_flag,"CmiCommUnlock");
  comm_flag=0;
}
#endif

static void CommunicationInterrupt(int ignored)
{
  MACHLOCK_ASSERT(!_Cmi_myrank,"CommunicationInterrupt");
  if (memflag || comm_flag || _immRunning || CmiCheckImmediateLock(0)) 
  { /* Already busy inside malloc, comm, or immediate messages */
    MACHSTATE(5,"--SKIPPING SIGIO--");
    return;
  }
  MACHSTATE1(2,"--BEGIN SIGIO comm_mutex_isLocked: %d--", comm_flag)
  {
    /*Make sure any malloc's we do in here are NOT migratable:*/
    CmiMemoryIsomallocDisablePush();
/*    _Cmi_myrank=1; */
    CommunicationServerNet(0, COMM_SERVER_FROM_INTERRUPT);  /* from interrupt */
    //CommunicationServer(0);  /* from interrupt */
/*    _Cmi_myrank=0; */
    CmiMemoryIsomallocDisablePop();
  }
  MACHSTATE(2,"--END SIGIO--")
}

void CmiSignal(int sig1, int sig2, int sig3, void (*handler)(int));

static void CmiDestroyLocks(void)
{
  comm_flag = 0;
  memflag = 0;
}

#endif

/*Add a message to this processor's receive queue 
  Must be called while holding comm. lock
*/

extern double evacTime;

/***************************************************************
 Communication with charmrun:
 We can send (ctrl_sendone) and receive (ctrl_getone)
 messages on a TCP socket connected to charmrun.
 This is used for printfs, CCS, etc; and also for
 killing ourselves if charmrun dies.
*/

/*This flag prevents simultaneous outgoing
messages on the charmrun socket.  It is protected
by the commlock.*/
static int Cmi_charmrun_fd_sendflag=0;

/* ctrl_sendone */
static int sendone_abort_fn(SOCKET skt,int code,const char *msg) {
#if !defined(_WIN32)
	// on Windows, this is sometimes reached during normal program termination,
	// so printing an error makes no sense. The message is not seen normally
	// since processes are spawned by charmrun as DETACHED_PROCESS by default and
	// have no console output.
	// With CMK_CHARM4PY=1 on Windows, charmrun spawns one process in same console,
	// because any output before charm starts needs to be seen (e.g. Python syntax errors)
	// so this print needs to be disabled
	fprintf(stderr,"Socket error %d in ctrl_sendone! %s\n",code,msg);
#endif
	machine_exit(1);
	return -1;
}

static void ctrl_sendone_nolock(const char *type,
				const char *data1,size_t dataLen1,
				const char *data2,size_t dataLen2)
{
  const void *bufs[3]; size_t lens[3]; int nBuffers=0;
  ChMessageHeader hdr;
  skt_abortFn oldAbort=skt_set_abort(sendone_abort_fn);
  MACHSTATE1(2,"ctrl_sendone_nolock { type=%s", type);
  if (Cmi_charmrun_fd==-1) 
  	charmrun_abort("ctrl_sendone called in standalone!\n");
  Cmi_charmrun_fd_sendflag=1;
  ChMessageHeader_new(type,dataLen1+dataLen2,&hdr);
  bufs[nBuffers]=&hdr; lens[nBuffers]=sizeof(hdr); nBuffers++;
  if (dataLen1>0) {bufs[nBuffers]=data1; lens[nBuffers]=dataLen1; nBuffers++;}
  if (dataLen2>0) {bufs[nBuffers]=data2; lens[nBuffers]=dataLen2; nBuffers++;}
  skt_sendV(Cmi_charmrun_fd,nBuffers,bufs,lens);
  Cmi_charmrun_fd_sendflag=0;
  skt_set_abort(oldAbort);
  MACHSTATE(2,"} ctrl_sendone_nolock");
}

static void ctrl_sendone_locking(const char *type,
				const char *data1,size_t dataLen1,
				const char *data2,size_t dataLen2)
{
  LOCK_IF_AVAILABLE();
  ctrl_sendone_nolock(type,data1,dataLen1,data2,dataLen2);
  UNLOCK_IF_AVAILABLE();
}

#ifndef MEMORYUSAGE_OUTPUT
#define MEMORYUSAGE_OUTPUT 0 
#endif
#if MEMORYUSAGE_OUTPUT 
#define MEMORYUSAGE_OUTPUT_FREQ 10 //how many prints in a second
static int memoryusage_counter;
#define memoryusage_isOutput ((memoryusage_counter%MEMORYUSAGE_OUTPUT_FREQ)==0)
#define memoryusage_output {\
  memoryusage_counter++;\
  if(CmiMyPe()==0) printf("-- %d %f %ld --\n", CmiMyPe(), GetClock(), CmiMemoryUsage());}
#endif

/* if charmrun dies, we finish */
static void pingCharmrun(int ignored)
{
  static double Cmi_check_last;

#if MEMORYUSAGE_OUTPUT
  memoryusage_output;
  if(memoryusage_isOutput){
    memoryusage_counter = 0;
#else
  {
#endif 

  double clock=GetClock();
  if (clock > Cmi_check_last + Cmi_check_delay) {
    MACHSTATE1(3,"CommunicationsClock pinging charmrun Cmi_charmrun_fd_sendflag=%d", Cmi_charmrun_fd_sendflag);
    Cmi_check_last = clock; 
    CmiCommLockOrElse(return;); /*Already busy doing communication*/
    if (Cmi_charmrun_fd_sendflag) return; /*Busy talking to charmrun*/
    LOCK_IF_AVAILABLE();
    ctrl_sendone_nolock("ping",NULL,0,NULL,0); /*Charmrun may have died*/
    UNLOCK_IF_AVAILABLE();
  }
  CmiStdoutFlush(); /*Make sure stdout buffer hasn't filled up*/
  }
}

/* periodic charm ping, for netpoll */
static void pingCharmrunPeriodic(void *ignored)
{
  pingCharmrun(0);
  CcdCallFnAfter((CcdVoidFn)pingCharmrunPeriodic,NULL,1000);
}

static int ignore_further_errors(SOCKET skt,int c,const char *msg) {machine_exit(2);return -1;}
static void charmrun_abort(const char *s)
{
  if (Cmi_charmrun_fd==-1) {/*Standalone*/
  	fprintf(stderr,"Charm++ fatal error:\n%s\n",s);
CmiPrintStackTrace(0);
  	abort();
  } else {
	char msgBuf[80];
  	skt_set_abort(ignore_further_errors);
    if (CmiNumPartitions() == 1) {
        sprintf(msgBuf,"Fatal error on PE %d> ",CmiMyPe());
    }
    else
    {
        sprintf(msgBuf,"Fatal error on Partition %d PE %d> ", CmiMyPartition(), CmiMyPe());
    }
  	ctrl_sendone_nolock("abort",msgBuf,strlen(msgBuf),s,strlen(s)+1);
  	skt_close(Cmi_charmrun_fd);
  }
}

#if CMK_SHRINK_EXPAND
void charmrun_realloc(const char *s)
{
  ctrl_sendone_nolock("realloc",s,strlen(s)+1,NULL,0);
}
#endif

/* ctrl_getone */

#ifdef __FAULT__
#include "machine-recover.C"
#endif

static void node_addresses_store(ChMessage *msg);

static int barrierReceived = 0;

static void ctrl_getone(void)
{
  ChMessage msg;
  MACHSTATE(2,"ctrl_getone")
  MACHLOCK_ASSERT(comm_mutex_isLocked,"ctrl_getone")
  ChMessage_recv(Cmi_charmrun_fd,&msg);
  MACHSTATE1(2,"ctrl_getone recv one '%s'", msg.header.type);

  if (strcmp(msg.header.type,"die")==0) {
    MACHSTATE(2,"ctrl_getone bye bye")
    fprintf(stderr,"aborting: %s\n",msg.data);
    log_done();
    ConverseCommonExit();
    machine_exit(0);
  } 
#if CMK_CCS_AVAILABLE
  else if (strcmp(msg.header.type, "req_fw")==0) {
    CcsImplHeader *hdr=(CcsImplHeader *)msg.data;
	/*Sadly, I *can't* do a:
      CcsImpl_netRequest(hdr,msg.data+sizeof(CcsImplHeader));
	here, because I can't send converse messages in the
	communication thread.  I *can* poke this message into 
	any convenient processor's queue, though:  (OSL, 9/14/2000)
	*/
    int pe=0;/*<- node-local processor number. Any one will do.*/
    void *cmsg=(void *)CcsImpl_ccs2converse(hdr,msg.data+sizeof(CcsImplHeader),NULL);
    MACHSTATE(2,"Incoming CCS request");
    if (cmsg!=NULL) 
    {
      if(CmiNumPes() == 1 && CmiNumPartitions() == 1)
        ccsRunning = 1;
      CmiPushPE(pe,cmsg);
    }
  }
#endif
#ifdef __FAULT__	
  else if(strcmp(msg.header.type,"crashnode")==0) {
	crash_node_handle(&msg);
  }
  else if(strcmp(msg.header.type,"initnodetab")==0) {
	/** A processor crashed and got recreated. So charmrun sent 
	  across the whole nodetable data to update this processor*/
	node_addresses_store(&msg);
	// fprintf(stdout,"nodetable added %d\n",CmiMyPe());
  }
#endif
  else if(strcmp(msg.header.type,"barrier")==0) {
        barrierReceived = 1;
  }
  else if(strcmp(msg.header.type,"barrier0")==0) {
        barrierReceived = 2;
  }
  else {
  /* We do not use KillEveryOne here because it calls CmiMyPe(),
   * which is not available to the communication thread on an SMP version.
   */
    /* CmiPrintf("Unknown message: %s\n", msg.header.type); */
    charmrun_abort("ERROR> Unrecognized message from charmrun.\n");
    machine_exit(1);
  }
  
  MACHSTATE(2,"ctrl_getone done")
  ChMessage_free(&msg);
}

#if CMK_CCS_AVAILABLE && !NODE_0_IS_CONVHOST
/*Deliver this reply data to this reply socket.
  The data is forwarded to CCS server via charmrun.*/
void CcsImpl_reply(CcsImplHeader *hdr,int repLen,const void *repData)
{
  MACHSTATE(2,"Outgoing CCS reply");
  ctrl_sendone_locking("reply_fw",(const char *)hdr,sizeof(CcsImplHeader),
      (const char *)repData,repLen);
  MACHSTATE(1,"Outgoing CCS reply away");
}
#endif

/*****************************************************************************
 *
 * CmiPrintf, CmiError, CmiScanf
 *
 *****************************************************************************/
static void InternalWriteToTerminal(int isStdErr,const char *str,int len);
static void InternalPrintf(const char *f, va_list l)
{
  ChMessage replymsg;
  char *buffer = (char *)CmiTmpAlloc(PRINTBUFSIZE);
  CmiStdoutFlush();
  vsprintf(buffer, f, l);
  if(Cmi_syncprint) {
          LOCK_IF_AVAILABLE();
  	  ctrl_sendone_nolock("printsyn", buffer,strlen(buffer)+1,NULL,0);
  	  ChMessage_recv(Cmi_charmrun_fd,&replymsg);
  	  ChMessage_free(&replymsg);
          UNLOCK_IF_AVAILABLE();
  } else {
  	  ctrl_sendone_locking("print", buffer,strlen(buffer)+1,NULL,0);
  }
  InternalWriteToTerminal(0,buffer,strlen(buffer));
  CmiTmpFree(buffer);
}

static void InternalError(const char *f, va_list l)
{
  ChMessage replymsg;
  char *buffer = (char *)CmiTmpAlloc(PRINTBUFSIZE);
  CmiStdoutFlush();
  vsprintf(buffer, f, l);
  if(Cmi_syncprint) {
  	  ctrl_sendone_locking("printerrsyn", buffer,strlen(buffer)+1,NULL,0);
          LOCK_IF_AVAILABLE();
  	  ChMessage_recv(Cmi_charmrun_fd,&replymsg);
  	  ChMessage_free(&replymsg);
          UNLOCK_IF_AVAILABLE();
  } else {
  	  ctrl_sendone_locking("printerr", buffer,strlen(buffer)+1,NULL,0);
  }
  InternalWriteToTerminal(1,buffer,strlen(buffer));
  CmiTmpFree(buffer);
}

static int InternalScanf(char *fmt, va_list l)
{
  ChMessage replymsg;
  char *ptr[20];
  char *p; int nargs, i;
  nargs=0;
  p=fmt;
  while (*p) {
    if ((p[0]=='%')&&(p[1]=='*')) { p+=2; continue; }
    if ((p[0]=='%')&&(p[1]=='%')) { p+=2; continue; }
    if (p[0]=='%') { nargs++; p++; continue; }
    if (*p=='\n') *p=' '; p++;
  }
  if (nargs > 18) KillEveryone("CmiScanf only does 18 args.\n");
  for (i=0; i<nargs; i++) ptr[i]=va_arg(l, char *);
  CmiLock(Cmi_scanf_mutex);
  if (Cmi_charmrun_fd!=-1)
  {/*Send charmrun the format string*/
        ctrl_sendone_locking("scanf", fmt, strlen(fmt)+1,NULL,0);
        /*Wait for the reply (characters to scan) from charmrun*/
        LOCK_IF_AVAILABLE();
        ChMessage_recv(Cmi_charmrun_fd,&replymsg);
        i = sscanf((char*)replymsg.data, fmt,
                     ptr[ 0], ptr[ 1], ptr[ 2], ptr[ 3], ptr[ 4], ptr[ 5],
                     ptr[ 6], ptr[ 7], ptr[ 8], ptr[ 9], ptr[10], ptr[11],
                     ptr[12], ptr[13], ptr[14], ptr[15], ptr[16], ptr[17]);
        ChMessage_free(&replymsg);
        UNLOCK_IF_AVAILABLE();
  } else
  {/*Just do the scanf normally*/
        i=scanf(fmt, ptr[ 0], ptr[ 1], ptr[ 2], ptr[ 3], ptr[ 4], ptr[ 5],
                     ptr[ 6], ptr[ 7], ptr[ 8], ptr[ 9], ptr[10], ptr[11],
                     ptr[12], ptr[13], ptr[14], ptr[15], ptr[16], ptr[17]);
  }
  CmiUnlock(Cmi_scanf_mutex);
  return i;
}
#if CMK_CMIPRINTF_IS_A_BUILTIN

/*New stdarg.h declarations*/
void CmiPrintf(const char *fmt, ...)
{
  if (quietMode) return;
  CpdSystemEnter();
  {
  va_list p; va_start(p, fmt);
  if (Cmi_charmrun_fd!=-1 && _writeToStdout)
    InternalPrintf(fmt, p);
  else
    vfprintf(stdout,fmt,p);
  va_end(p);
  }
  CpdSystemExit();
}

void CmiError(const char *fmt, ...)
{
  CpdSystemEnter();
  {
  va_list p; va_start (p, fmt);
  if (Cmi_charmrun_fd!=-1)
    InternalError(fmt, p);
  else
    vfprintf(stderr,fmt,p);
  va_end(p);
  }
  CpdSystemExit();
}

int CmiScanf(const char *fmt, ...)
{
  int i;
  CpdSystemEnter();
  {
  va_list p; va_start(p, fmt);
  i = InternalScanf((char *)fmt, p);
  va_end(p);
  }
  CpdSystemExit();
  return i;
}

#endif

/***************************************************************************
 * Output redirection:
 *  When people don't use CkPrintf, like above, we'd still like to be able
 * to collect their output.  Thus we make a pipe and dup2 it to stdout,
 * which lets us read the characters sent to stdout at our lesiure.
 ***************************************************************************/

/*Can read from stdout or stderr using these fd's*/
static int readStdout[2]; 
static int writeStdout[2]; /*The original stdout/stderr sockets*/ 
static int serviceStdout[2]; /*(bool) Normally zero; one if service needed.*/
#define readStdoutBufLen (16*1024)
static char readStdoutBuf[readStdoutBufLen+1]; /*Protected by comm. lock*/
static int servicingStdout;

/*Initialization-- should only be called once per node*/
static void CmiStdoutInit(void) {
	int i;
	if (Cmi_charmrun_fd==-1) return; /* standalone mode */

/*There's some way to do this same thing in windows, but I don't know how*/
#if !defined(_WIN32)
	/*Prevent buffering in stdio library:*/
	setbuf(stdout,NULL); setbuf(stderr,NULL);

	/*Reopen stdout and stderr fd's as new pipes:*/
        for (i=0;i<2;i++) {
		int pair[2];
		int srcFd=1+i; /* 1 is stdout; 2 is stderr */
		
		/*First, save a copy of the original stdout*/
		writeStdout[i]=dup(srcFd);
#if 0
		/*Build a pipe to connect to stdout (4kb buffer, but no SIGIO...)*/
		if (-1==pipe(pair)) {perror("building stdio redirection pipe"); exit(1);}
#else
	       /* UNIX socket (16kb default buffer, and works with SIGIO!) */
		if (-1==socketpair(PF_UNIX,SOCK_STREAM,0,pair)) 
			{perror("building stdio redirection socketpair"); exit(1);}
#endif
		readStdout[i]=pair[0]; /*We get the read end of pipe*/
		if (-1==dup2(pair[1],srcFd)) {perror("dup2 redirection pipe"); exit(1);}
		//if (-1==dup2(srcFd,pair[1])) {perror("dup2 redirection pipe"); exit(1);}
		
#if 0 /*Keep writes from blocking.  This just drops excess output, which is bad.*/
		CmiEnableNonblockingIO(srcFd);
#endif
#if CMK_SHARED_VARS_UNAVAILABLE 
                if (Cmi_asyncio)
		{
  /*No communication thread-- get a SIGIO on each write(), which keeps the buffer clean*/
			CmiEnableAsyncIO(pair[1]);
		}
#endif
	}
#else
/*Windows system-- just fake reads for now*/
# ifndef read
#  define read(x,y,z) 0
# endif
# ifndef write
#  define write(x,y,z) z
# endif
#endif
}

/*Sends data to original stdout (e.g., for ++debug or ++in-xterm)*/
static void InternalWriteToTerminal(int isStdErr,const char *str,int len)
{
	if (write(writeStdout[isStdErr],str,len) != len) {
		CmiAbort("Writing to terminal failed!");
	}
}

/*
  Service this particular stdout pipe.  
  Must hold comm. lock.
*/
static void CmiStdoutServiceOne(int i) {
	int nBytes;
	const static char *cmdName[2]={"print","printerr"};
	servicingStdout=1;
	while(1) {
		const char *tooMuchWarn=NULL; int tooMuchLen=0;
		if (!skt_select1(readStdout[i],0)) break; /*Nothing to read*/
		nBytes=read(readStdout[i],readStdoutBuf,readStdoutBufLen);
		if (nBytes<=0) break; /*Nothing to send*/
		
		/*Send these bytes off to charmrun*/
		readStdoutBuf[nBytes]=0; /*Zero-terminate read string*/
		nBytes++; /*Include zero-terminator in message to charmrun*/
		
		if (nBytes>=readStdoutBufLen-100) 
		{ /*We must have filled up our output pipe-- most output libraries
		   don't handle this well (e.g., glibc printf just drops the line).*/
			
			tooMuchWarn="\nWARNING: Too much output at once-- possible output discontinuity!\n"
				"Use CkPrintf to avoid discontinuity (and this warning).\n\n";
			nBytes--; /*Remove terminator from user's data*/
			tooMuchLen=strlen(tooMuchWarn)+1;
		}
		ctrl_sendone_nolock(cmdName[i],readStdoutBuf,nBytes,
				    tooMuchWarn,tooMuchLen);
		
		InternalWriteToTerminal(i,readStdoutBuf,nBytes);
	}
	servicingStdout=0;
	serviceStdout[i]=0; /*This pipe is now serviced*/
}

/*Service all stdout pipes, whether it looks like they need it
  or not.  Used when you aren't sure if select() has been called recently.
  Must hold comm. lock.
*/
static void CmiStdoutServiceAll(void) {
	int i;
	for (i=0;i<2;i++) {
		if (readStdout[i]==0) continue; /*Pipe not open*/
		CmiStdoutServiceOne(i);
	}
}

/*Service any outstanding stdout pipes.
  Must hold comm. lock.
*/
static void CmiStdoutService(void) {
	CmiStdoutServiceAll();
}

/*Add our pipes to the pile for select() or poll().
  Both can be called with or without the comm. lock.
*/
static void CmiStdoutAdd(CMK_PIPE_PARAM) {
	int i;
	for (i=0;i<2;i++) {
		if (readStdout[i]==0) continue; /*Pipe not open*/
		CMK_PIPE_ADDREAD(readStdout[i]);
	}
}
static void CmiStdoutCheck(CMK_PIPE_PARAM) {
	int i;
	for (i=0;i<2;i++) {
		if (readStdout[i]==0) continue; /*Pipe not open*/
		if (CMK_PIPE_CHECKREAD(readStdout[i])) serviceStdout[i]=1;
	}
}
static int CmiStdoutNeedsService(void) {
	return (serviceStdout[0]!=0 || serviceStdout[1]!=0);
}

/*Called every few milliseconds to flush the stdout pipes*/
static void CmiStdoutFlush(void) {
	if (servicingStdout) return; /* might be called by SIGALRM */
	CmiCommLockOrElse( return; )
        LOCK_IF_AVAILABLE();
	CmiStdoutServiceAll();
        UNLOCK_IF_AVAILABLE();
}

/***************************************************************************
 * Message Delivery:
 *
 ***************************************************************************/

#include "machine-dgram.C"

static void open_charmrun_socket(void)
{
#if CMK_USE_TCP
  dataskt=skt_server(&dataport);
#else
  dataskt=skt_datagram(&dataport, Cmi_os_buffer_size);
#endif
  MACHSTATE2(5,"skt_connect at dataskt:%d Cmi_charmrun_port:%d",dataskt, Cmi_charmrun_port);
  Cmi_charmrun_fd = skt_connect(Cmi_charmrun_IP, Cmi_charmrun_port, 1800);
  MACHSTATE2(5,"Opened connection to charmrun at socket %d, dataport=%d", Cmi_charmrun_fd, dataport);
  skt_tcp_no_nagle(Cmi_charmrun_fd);
}


/*****************************************************************************
 *
 * node_addresses
 *
 *  These two functions fill the node-table.
 *
 *
 *   This node, like all others, first sends its own address to charmrun
 *   using this command:
 *
 *     Type: nodeinfo
 *     Data: Big-endian 4-byte ints
 *           <my-node #><Dataport>
 *
 *   When charmrun has all the addresses, he sends this table to me:
 *
 *     Type: nodes
 *     Data: Big-endian 4-byte ints
 *           <number of nodes n>
 *           <#PEs><IP><Dataport> Node 0
 *           <#PEs><IP><Dataport> Node 1
 *           ...
 *           <#PEs><IP><Dataport> Node n-1
 *
 *****************************************************************************/

static void send_singlenodeinfo(void)
{
  /* Contact charmrun for machine info. */
  ChSingleNodeinfo me;
  memset(&me, 0, sizeof(me));

#if CMK_SHRINK_EXPAND
  me.nodeNo = ChMessageInt_new(Cmi_charmrun_assigned_pe);
#else
  me.nodeNo = ChMessageInt_new(Lrts_myNode);
#endif
  me.num_pus = ChMessageInt_new(CmiHwlocTopologyLocal.num_pus);
  me.num_cores = ChMessageInt_new(CmiHwlocTopologyLocal.num_cores);
  me.num_sockets = ChMessageInt_new(CmiHwlocTopologyLocal.num_sockets);

  /* The nPE fields are set by charmrun--these values don't matter.
     Set IP in case it is mpiexec mode where charmrun does not have IP yet */
  me.info.nPE = ChMessageInt_new(0);
  /* me.info.IP = _skt_invalid_ip; */
  me.info.IP = skt_innode_my_ip();
  me.info.dataport = ChMessageInt_new(dataport);

  /* Send our node info. to charmrun.
     CommLock hasn't been initialized yet--
     use non-locking version */
  ctrl_sendone_nolock("initnode", (const char *)&me, sizeof(me), NULL, 0);
  MACHSTATE1(5, "send initnode - dataport:%d", dataport);

  MACHSTATE(3, "initnode sent");
}

/*Note: node_addresses_obtain is called before starting
  threads, so no locks are needed (or valid!)*/
static void node_addresses_obtain(char **argv)
{
  ChMessage nodetabmsg; /* info about all nodes*/
  MACHSTATE(3,"node_addresses_obtain { ");
  if (Cmi_charmrun_fd==-1) 
  {/*Standalone-- fake a single-node nodetab message*/
	ChMessageInt_t *n32;
	ChNodeinfo *nodeInfo;
	ChMessage_new("initnodetab", sizeof(ChMessageInt_t)*ChInitNodetabFields+sizeof(ChNodeinfo), &nodetabmsg);
	n32 = (ChMessageInt_t *)nodetabmsg.data;
	nodeInfo = (ChNodeinfo *)(nodetabmsg.data + sizeof(ChMessageInt_t)*ChInitNodetabFields);

	n32[0] = ChMessageInt_new(1);
	n32[1] = ChMessageInt_new(Lrts_myNode);
	nodeInfo->nPE = ChMessageInt_new(_Cmi_mynodesize);
	nodeInfo->dataport = ChMessageInt_new(0);
	nodeInfo->IP = _skt_invalid_ip;
	nodeInfo->nProcessesInPhysNode = ChMessageInt_new(1);
  }
  else 
  {
    send_singlenodeinfo();

    /* We get the other node addresses from a message sent
       back via the charmrun control port. */
    if (!skt_select1(Cmi_charmrun_fd, 1200*1000))
      CmiAbort("Timeout waiting for nodetab!\n");

        MACHSTATE(2,"recv initnode {");
  	ChMessage_recv(Cmi_charmrun_fd,&nodetabmsg);

    while (strcmp("nodefork", nodetabmsg.header.type) == 0)
    {
#ifndef _WIN32
      int i;

      assert(sizeof(ChMessageInt_t)*ChInitNodeforkFields == (size_t)nodetabmsg.len);
      ChMessageInt_t *n32 = (ChMessageInt_t *) nodetabmsg.data;
      const int phase2_forks = ChMessageInt(n32[0]);
      const int start_id = ChMessageInt(n32[1]);

      ChMessage_free(&nodetabmsg);

      for (i = 0; i < phase2_forks; ++i)
      {
        const int pid = fork();
        if (pid < 0)
          CmiAbort("fork failed");
        else if (pid == 0)
        {
          skt_close(Cmi_charmrun_fd);
          dataport = 0;
          Lrts_myNode = start_id + i;
#if CMK_SHRINK_EXPAND
          Cmi_charmrun_assigned_pe = start_id + i;
#endif
          open_charmrun_socket();
          send_singlenodeinfo();
          break;
        }
      }
#endif

      if (!skt_select1(Cmi_charmrun_fd, 1200*1000))
        CmiAbort("Timeout waiting for nodetab!\n");

      ChMessage_recv(Cmi_charmrun_fd, &nodetabmsg);
    }

        MACHSTATE(2,"} recv initnode");
  }

  if (strcmp("initnodetab", nodetabmsg.header.type) == 0)
  {
    ChMessageInt_t *n32 = (ChMessageInt_t *) nodetabmsg.data;
    ChNodeinfo *d = (ChNodeinfo *) (n32+ChInitNodetabFields);
    Lrts_myNode = ChMessageInt(n32[1]);
    _Cmi_myphysnode_numprocesses = ChMessageInt(d[Lrts_myNode].nProcessesInPhysNode);
    CmiMyLocalRank = Lrts_myNode % _Cmi_myphysnode_numprocesses;

    node_addresses_store(&nodetabmsg);
    ChMessage_free(&nodetabmsg);
  }
  else if (strcmp("die", nodetabmsg.header.type) == 0)
  {
    _Exit(1);
  }

  MACHSTATE(3,"} node_addresses_obtain ");
}


/***********************************************************************
 * DeliverOutgoingMessage()
 *
 * This function takes care of delivery of outgoing messages from the
 * sender end. Broadcast messages are divided into sets of messages that 
 * are bound to the local node, and to remote nodes. For local
 * transmission, the messages are directly pushed into the recv
 * queues. For non-local transmission, the function DeliverViaNetwork()
 * is called
 ***********************************************************************/
int DeliverOutgoingMessage(OutgoingMsg ogm)
{
  int i, rank, dst; OtherNode node;
	
  int network = 1;

  dst = ogm->dst;

  //printf("deliver outgoing message, dest: %d \n", dst);
#if CMK_ERROR_CHECKING
    if (dst<0 || dst>=CmiNumPesGlobal())
      CmiAbort("Send to out-of-bounds processor!");
#endif
    node = nodes_by_pe[dst];
    rank = dst - node->nodestart;
    int acqLock = 0;
    if (node->nodestart != Cmi_nodestartGlobal) {
        DeliverViaNetwork(ogm, node, rank, DGRAM_ROOTPE_MASK, 0);
    }
#if CMK_MULTICORE
  network = 0;
#endif
  return network;
}

/**
 * Set up an OutgoingMsg structure for this message.
 */
static OutgoingMsg PrepareOutgoing(int pe,size_t size,int freemode,char *data) {
  OutgoingMsg ogm;
  MallocOutgoingMsg(ogm);
  MACHSTATE2(2,"Preparing outgoing message for pe %d, size %zu",pe,size);
  ogm->size = size;
  ogm->data = data;
  ogm->src = CmiMyPeGlobal();
  ogm->dst = pe;
  ogm->freemode = freemode;
  ogm->refcount = 0;
  return ogm;
}


/******************************************************************************
 *
 * CmiGeneralSend
 *
 * Description: This is a generic message sending routine. All the
 * converse message send functions are implemented in terms of this
 * function. (By setting appropriate flags (eg freemode) that tell
 * CmiGeneralSend() how exactly to handle the particular case of
 * message send)
 *
 *****************************************************************************/

//CmiCommHandle CmiGeneralSend(int pe, size_t size, int freemode, char *data)
CmiCommHandle LrtsSendFunc(int destNode, int pe, size_t size, char *data, int freemode)
{
  int sendonnetwork;
  OutgoingMsg ogm;
  MACHSTATE(1,"CmiGeneralSend {");

  CMI_MSG_SIZE(data) = size;

  ogm=PrepareOutgoing(pe,size,'F',data);

  sendonnetwork = DeliverOutgoingMessage(ogm);
  MACHSTATE(1,"}  LrtsSend");
  return (CmiCommHandle)ogm;
}


/******************************************************************************
 *
 * Comm Handle manipulation.
 *
 *****************************************************************************/

#if ! CMK_MULTICAST_LIST_USE_COMMON_CODE

/*****************************************************************************
 *
 * NET version List-Cast and Multicast Code
 *
 ****************************************************************************/

void LrtsSyncListSendFn(int npes, const int *pes, size_t len, char *msg)
{
  int i;
  for(i=0;i<npes;i++) {
    CmiReference(msg);
    CmiSyncSendAndFree(pes[i], len, msg);
  }
}

CmiCommHandle LrtsAsyncListSendFn(int npes, const int *pes, size_t len, char *msg)
{
  CmiError("ListSend not implemented.");
  return (CmiCommHandle) 0;
}
                                                                                
/* 
  because in all net versions, the message buffer after CmiSyncSendAndFree
  returns is not changed, we can use memory reference trick to avoid 
  memory copying here
*/
void LrtsFreeListSendFn(int npes, const int *pes, size_t len, char *msg)
{
  int i;
  for(i=0;i<npes;i++) {
    CmiReference(msg);
    CmiSyncSendAndFree(pes[i], len, msg);
  }
  CmiFree(msg);
}

#endif

void LrtsDrainResources(void){}
void LrtsPostNonLocal(void) {}

/* Network progress function is used to poll the network when for
   messages. This flushes receive buffers on some implementations*/
    
#if CMK_MACHINE_PROGRESS_DEFINED
void CmiMachineProgressImpl(void){
#if CMK_SMP && !CMK_MULTICORE
  if (CmiMyRank() == CmiMyNodeSize())
#endif
    CommunicationServerNet(0, COMM_SERVER_FROM_SMP);
}
#endif

void LrtsAdvanceCommunication(int whileidle)
{
#if CMK_SMP
  CommunicationServerNet(0, COMM_SERVER_FROM_SMP);
#else
  CommunicationServerNet(0, COMM_SERVER_FROM_WORKER);
#endif

}

/******************************************************************************
 *
 * Main code, Init, and Exit
 *
 *****************************************************************************/

#if CMK_BARRIER_USE_COMMON_CODE

/* happen at node level */
/* must be called on every PE including communication processors */
void LrtsBarrier(void)
{
  int numnodes = CmiNumNodesGlobal();
  static int barrier_phase = 0;

  if (Cmi_charmrun_fd == -1) return;                // standalone
  if (numnodes == 1) {
    return;
  }

  ctrl_sendone_locking("barrier",NULL,0,NULL,0);
  while (barrierReceived != 1) {
    LOCK_IF_AVAILABLE();
    ctrl_getone();
    UNLOCK_IF_AVAILABLE();
  }
  barrierReceived = 0;
  barrier_phase ++;
}

int CmiBarrierZero(void)
{
  int i;
  int numnodes = CmiNumNodesGlobal();
  ChMessage msg;

  if (Cmi_charmrun_fd == -1) return 0;                // standalone
  if (numnodes == 1) {
    CmiNodeAllBarrier();
    return 0;
  }

  if (CmiMyRank() == 0) {
    char str[64];
    sprintf(str, "%d", CmiMyNodeGlobal());
    ctrl_sendone_locking("barrier0",str,strlen(str)+1,NULL,0);
    if (CmiMyNodeGlobal() == 0) {
      while (barrierReceived != 2) {
        LOCK_IF_AVAILABLE();
        ctrl_getone();
        UNLOCK_IF_AVAILABLE();
      }
      barrierReceived = 0;
    }
  }

  CmiNodeAllBarrier();
  return 0;
}

#endif

/******************************************************************************
 *
 * Main code, Init, and Exit
 *
 *****************************************************************************/

void LrtsPreCommonInit(int everReturn)
{
#if !CMK_SMP
#if !CMK_ASYNC_NOT_NEEDED
  if (Cmi_asyncio)
  {
    CmiSignal(SIGIO, 0, 0, CommunicationInterrupt);
    if (!Cmi_netpoll) {
      if (dataskt!=-1) CmiEnableAsyncIO(dataskt);
      if (Cmi_charmrun_fd!=-1) CmiEnableAsyncIO(Cmi_charmrun_fd);
    }
  }
#endif
#endif
}

void LrtsPostCommonInit(int everReturn)
{
   /* better to show the status here */
  if (CmiMyPe() == 0) {
    if (Cmi_netpoll == 1) {
      CmiPrintf("Charm++> scheduler running in netpoll mode.\n");
    }
#if CMK_SHARED_VARS_UNAVAILABLE
    else {
      if (CmiMemoryIs(CMI_MEMORY_IS_OS))
        CmiAbort("Charm++ Fatal Error: interrupt mode does not work with default system memory allocator. Run with +netpoll to disable the interrupt.");
    }
#endif
  }       

#if MEMORYUSAGE_OUTPUT
  memoryusage_counter = 0;
#endif

#if CMK_SHARED_VARS_UNAVAILABLE
  if (Cmi_netpoll) /*Repeatedly call CommServer*/
    CcdCallOnConditionKeep(CcdPERIODIC, 
        (CcdVoidFn) CommunicationPeriodic, NULL);
  else /*Only need this for retransmits*/
    CcdCallOnConditionKeep(CcdPERIODIC_10ms, 
        (CcdVoidFn) CommunicationPeriodic, NULL);
#endif
    
  if (CmiMyRank()==0 && Cmi_charmrun_fd!=-1) {
    CcdCallOnConditionKeep(CcdPERIODIC_10ms, (CcdVoidFn) CmiStdoutFlush, NULL);
#if CMK_SHARED_VARS_UNAVAILABLE && !CMK_TIMER_USE_WIN32API
    if (!Cmi_asyncio) {
    CcdCallFnAfter((CcdVoidFn)pingCharmrunPeriodic,NULL,1000);
    }
    else {
    /*Occasionally ping charmrun, to test if it's dead*/
    struct itimerval i;
    CmiSignal(SIGALRM, 0, 0, pingCharmrun);
#if MEMORYUSAGE_OUTPUT
    i.it_interval.tv_sec = 0;
    i.it_interval.tv_usec = 1000000/MEMORYUSAGE_OUTPUT_FREQ;
    i.it_value.tv_sec = 0;
    i.it_value.tv_usec = 1000000/MEMORYUSAGE_OUTPUT_FREQ;
#else
    i.it_interval.tv_sec = 10;
    i.it_interval.tv_usec = 0;
    i.it_value.tv_sec = 10;
    i.it_value.tv_usec = 0;
#endif
    setitimer(ITIMER_REAL, &i, NULL);
    }

#if ! CMK_USE_TCP
    /*Occasionally check for retransmissions, outgoing acks, etc.*/
    CcdCallFnAfter((CcdVoidFn)CommunicationsClockCaller,NULL,Cmi_comm_clock_delay);
#endif
#endif
      
    /*Initialize the clock*/
    Cmi_clock=GetClock();
  }

#ifdef IGET_FLOWCONTROL 
  /* Call the function once to determine the amount of physical memory available */
  getAvailSysMem();
  /* Call the function to periodically call the token adapt function */
  CcdCallFnAfter((CcdVoidFn)TokenUpdatePeriodic, NULL, 2000); // magic number of 2000ms
  CcdCallOnConditionKeep(CcdPERIODIC_10s,   // magic number of PERIOD 10s
        (CcdVoidFn) TokenUpdatePeriodic, NULL);
#endif
  
#ifdef CMK_RANDOMLY_CORRUPT_MESSAGES
  srand((int)(1024.0*CmiWallTimer()));
  if (CmiMyPe()==0)
    CmiPrintf("Charm++: Machine layer will randomly corrupt every %d'th message (rand %d)\n",
    	CMK_RANDOMLY_CORRUPT_MESSAGES,rand());
#endif

}

void LrtsExit(int exitcode)
{
  int i;
  machine_initiated_shutdown=1;

  CmiStdoutFlush();
  if (Cmi_charmrun_fd==-1) {
    exit(exitcode);
  }
  else {
    char tmp[16];
    sprintf(tmp, "%d", exitcode);
    Cmi_check_delay = 1.0;      /* speed up checking of charmrun */
    for(i = 0; i < CmiMyNodeSize(); i++) {
      ctrl_sendone_locking("ending",tmp,strlen(tmp)+1,NULL,0); /* this causes charmrun to go away, every PE needs to report */
    }
    while(1) {
#if CMK_USE_PXSHM
      CommunicationServerPxshm();
#endif
      CommunicationServerNet(5, COMM_SERVER_FROM_SMP);
    }
  }
}

#if CMK_SHRINK_EXPAND
void ConverseCleanup(void)
{
  MACHSTATE(2,"ConverseCleanup {");
  if (CmiMyRank()==0) {
    if(Cmi_print_stats)
      printNetStatistics();
    log_done();
  }

  CmiBarrier();

  if (CmiMyPe() == 0) {
    if (willContinue) {
      CcsSendDelayedReply(shrinkExpandreplyToken, 0, 0); //reply to CCS client
      // wait for this message to receive, hack
      // TODO: figure out why this is important
      usleep(500);
      // this causes charmrun to go away
      ChMessageInt_t numProcessAfterRestart_msg = ChMessageInt_new(numProcessAfterRestart);
      ctrl_sendone_locking("realloc",(char *)&numProcessAfterRestart_msg, sizeof(int),NULL,0);
    } else {
      ctrl_sendone_locking("ending",NULL,0,NULL,0);
    }
  }

  // TODO: ensure this won't gobble up some other important message
  ChMessage replymsg;
  memset(replymsg.header.type, 0, sizeof(replymsg.header.type));
  while (strncmp(replymsg.header.type, "realloc_ack", CH_TYPELEN) != 0)
    ChMessage_recv(Cmi_charmrun_fd, &replymsg);

#if CMK_USE_SYSVSHM
	CmiExitSysvshm();
#elif CMK_USE_PXSHM
	CmiExitPxshm();
#endif
  ConverseCommonExit();               /* should be called by every rank */
  CmiNodeBarrier();        /* single node SMP, make sure every rank is done */
  if (CmiMyRank()==0) CmiStdoutFlush();
  if (Cmi_charmrun_fd==-1) {
    if (CmiMyRank() == 0) exit(0); /*Standalone version-- just leave*/
    else while (1) CmiYield();
  } else {
    if (willContinue) {
      int argc=CmiGetArgc(Cmi_argvcopy);

      int i;
      int restart_idx = -1;
      for (i = 0; i < argc; ++i) {
        if (strcmp(Cmi_argvcopy[i], "+restart") == 0) {
          restart_idx = i;
          break;
        }
      }

      const char **ret;
      if (restart_idx == -1) {
        ret=(const char **)malloc(sizeof(char *)*(argc+10));
      } else {
        ret=(const char **)malloc(sizeof(char *)*(argc+8));
      }

      for (i=0;i<argc;i++) {
        MACHSTATE1(2,"Parameters %s",Cmi_argvcopy[i]);
        ret[i]=Cmi_argvcopy[i];
      }

      ret[argc+0]="+shrinkexpand";
      ret[argc+1]="+newnumpes";

      char temp[50];
      sprintf(temp,"%d", numProcessAfterRestart);
      ret[argc+2]=temp;

      ret[argc+3]="+mynewpe";
      char temp2[50];
      sprintf(temp2,"%d", mynewpe);
      ret[argc+4]=temp2;

      ret[argc+5]="+myoldpe";
      char temp3[50];
      sprintf(temp3,"%d", CmiMyPe());
      ret[argc+6]=temp3;

      if (restart_idx == -1) {
        ret[argc+7]="+restart";
        ret[argc+8]=_shrinkexpand_basedir;
        ret[argc+9]=Cmi_argvcopy[argc];
      } else {
        ret[restart_idx + 1] = _shrinkexpand_basedir;
        ret[argc+7]=Cmi_argvcopy[argc];
      }

      free(Cmi_argvcopy);
      MACHSTATE1(3,"ConverseCleanup mynewpe %s", temp2);
      MACHSTATE(2,"} ConverseCleanup");

      skt_close(Cmi_charmrun_fd);
      // Avoid crash by SIGALRM
#if !defined(_WIN32)
      struct sigaction act;
      memset(&act, 0, sizeof(act));
      act.sa_handler = SIG_IGN;
      act.sa_flags = SA_RESETHAND;
      sigaction(SIGALRM, &act, 0);
#else
      signal(SIGALRM, SIG_IGN);
#endif
      //put references to the controlling tty back on normal fd so that
      //CmiStdoutInit  refers to the tty not the old pipe
      dup2(writeStdout[0], 1);
      dup2(writeStdout[1], 2);

      // Close any old file descriptors.
      // FDs carry over execv, so these have to be closed at some point.
      // Since some of them are async pipes, however, doing so flushes the buffer,
      // raising a SIGIO before a handler is assigned in LrtsPreCommonInit, which
      // kills the program.
      // An easy way to deal with this is to simply close the FDs here.
      int b;
      for (b = 3; b < 20; b++) {
        close(b);
      }

      // TODO: check variant of execv that takes file descriptor
      execv(ret[0], const_cast<char * const *>(ret)); // Need to check if the process name is always first arg
      /* should not be here */
      MACHSTATE1(3,"execv error: %s", strerror(errno));
      CmiPrintf("[%d] should not be here\n", CmiMyPe());
      exit(1);
    } else {
      skt_close(Cmi_charmrun_fd);
      exit(0);
    }
  }
}

#endif

static void set_signals(void)
{
  if(!Cmi_truecrash) {
#if !defined(_WIN32)
    struct sigaction sa;
    sa.sa_handler = KillOnAllSigs;
    sigemptyset(&sa.sa_mask);    
    sa.sa_flags = SA_RESTART;

    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
#else
    signal(SIGSEGV, KillOnAllSigs);
    signal(SIGFPE, KillOnAllSigs);
    signal(SIGILL, KillOnAllSigs);
    signal(SIGINT, KillOnAllSigs);
    signal(SIGTERM, KillOnAllSigs);
    signal(SIGABRT, KillOnAllSigs);
#endif
#   if !defined(_WIN32) /*UNIX-only signals*/
    sigaction(SIGQUIT, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
#     if CMK_HANDLE_SIGUSR
    sa.sa_handler = HandleUserSignals;
    sigaction(SIGUSR1, &sa, NULL);
    sigaction(SIGUSR2, &sa, NULL);
#     endif
#   endif /*UNIX*/
  }
}

/*Socket idle function to use before addresses have been
  obtained.  During the real program, we idle with CmiYield.
*/
static void obtain_idleFn(void) {sleep(0);}

static int net_default_skt_abort(SOCKET skt,int code,const char *msg)
{
  fprintf(stderr,"Fatal socket error: code %d-- %s\n",code,msg);
  machine_exit(1);
  return -1;
}

void LrtsInit(int *argc, char ***argv, int *numNodes, int *myNodeID)
{
  int i;
  Cmi_netpoll = 0;
#if CMK_NETPOLL
  Cmi_netpoll = 1;
#endif
#if CMK_WHEN_PROCESSOR_IDLE_USLEEP
  Cmi_idlepoll = 0;
#else
  Cmi_idlepoll = 1;
#endif
#if CMK_OPTIMIZE
  Cmi_truecrash = 0;
#else
  Cmi_truecrash = 1;
#endif
  if (CmiGetArgFlagDesc(*argv,"+truecrash","Do not install signal handlers") ||
      CmiGetArgFlagDesc(*argv,"++debug",NULL /*meaning: don't show this*/) ) Cmi_truecrash = 1;
    /* netpoll disable signal */
  if (CmiGetArgFlagDesc(*argv,"+netpoll","Do not use SIGIO--poll instead")) Cmi_netpoll = 1;
  if (CmiGetArgFlagDesc(*argv,"+netint","Use SIGIO")) Cmi_netpoll = 0;
    /* idlepoll use poll instead if sleep when idle */
  if (CmiGetArgFlagDesc(*argv,"+idlepoll","Do not sleep when idle")) Cmi_idlepoll = 1;
    /* idlesleep use sleep instead if busywait when idle */
  if (CmiGetArgFlagDesc(*argv,"+idlesleep","Make sleep calls when idle")) Cmi_idlepoll = 0;
  Cmi_syncprint = CmiGetArgFlagDesc(*argv,"+syncprint", "Flush each CmiPrintf to the terminal");

#if CMK_SHRINK_EXPAND
  if (CmiGetArgFlagDesc(*argv,"+shrinkexpand","Restarting of already running prcoess")) Cmi_isOldProcess = 1;
#endif

  Cmi_asyncio = 1;
#if CMK_ASYNC_NOT_NEEDED
  Cmi_asyncio = 0;
#endif
  if (CmiGetArgFlagDesc(*argv,"+asyncio","Use async IO")) Cmi_asyncio = 1;
  if (CmiGetArgFlagDesc(*argv,"+asynciooff","Don not use async IO")) Cmi_asyncio = 0;
#if CMK_MULTICORE
  if (CmiGetArgFlagDesc(*argv,"+commthread","Use communication thread")) {
    Cmi_commthread = 1;
#if CMK_SHARED_VARS_POSIX_THREADS_SMP
    _Cmi_sleepOnIdle = 1;   /* worker thread go sleep */
#endif
    if (CmiMyPe() == 0) CmiPrintf("Charm++> communication thread is launched in multicore version. \n");
  }
#endif

  skt_init();
  /* use special abort handler instead of default_skt_abort to 
     prevent exit trapped by atexit_check() due to the exit() call  */
  skt_set_abort(net_default_skt_abort);
  atexit(machine_atexit_check);
  parse_netstart();
  parse_magic();
#if ! defined(_WIN32)
  /* only get forks in non-smp mode */
#if CMK_SHRINK_EXPAND
  if(Cmi_isOldProcess!=1)
#endif
  parse_forks();
#endif
  extract_args(*argv);
  log_init();
  Cmi_comm_var_mutex = CmiCreateLock();
  Cmi_freelist_mutex = CmiCreateLock();
  Cmi_scanf_mutex = CmiCreateLock();
#if CMK_SHRINK_EXPAND
  if (Cmi_isOldProcess == 1) {
    Lrts_myNode = Cmi_mynewpe;
    Cmi_myoldpe = Cmi_oldpe;
    Lrts_numNodes = Cmi_newnumnodes;
  }
#endif

    /* NOTE: can not acutally call timer before timerInit ! GZ */
#if CMK_SHRINK_EXPAND
  MACHSTATE3(2,"After reorg  %d %d %d \n", Cmi_oldpe, Lrts_myNode, Lrts_numNodes);
#endif
  MACHSTATE2(5,"Init: (netpoll=%d), (idlepoll=%d)",Cmi_netpoll,Cmi_idlepoll);

  skt_set_idle(obtain_idleFn);
  if (!skt_ip_match(Cmi_charmrun_IP,_skt_invalid_ip)) {
  	set_signals();
    open_charmrun_socket();
  } else {/*Standalone operation*/
  	if (!quietMode) printf("Charm++: standalone mode (not using charmrun)\n");
  	dataskt=-1;
  	Cmi_charmrun_fd=-1;
  }

  node_addresses_obtain(*argv);
  MACHSTATE(5,"node_addresses_obtain done");

  if (Cmi_charmrun_fd != -1)
    CmiStdoutInit();

  CmiMachineInit(*argv);

  CmiCommunicationInit(*argv);

  skt_set_idle(CmiYield);
  Cmi_check_delay = 1.0+0.25*Lrts_numNodes;

  if (Cmi_charmrun_fd==-1) /*Don't bother with check in standalone mode*/
      Cmi_check_delay=1.0e30;

#if CMK_SMP
  // Allocate a slot for the comm thread. Do this even for multicore,
  // since it's possible to ask for a 'comm' thread at runtime
  inProgress = (int *)calloc(_Cmi_mynodesize+1, sizeof(int));
#else
  inProgress = (int *)calloc(_Cmi_mynodesize, sizeof(int));
#endif

  *numNodes = Lrts_numNodes;
  *myNodeID = Lrts_myNode;
}


void LrtsPrepareEnvelope(char *msg, size_t size)
{
  CMI_MSG_SIZE(msg) = size;
}


/*@}*/
