#include "sockRoutines.h"

#ifndef CMK_NO_SOCKETS /*<- for ASCI Red*/

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <ctype.h>
#if CMK_USE_POLL
#include <poll.h>
#endif


#ifndef CMK_NOT_USE_CONVERSE
#  include "converse.h" /* use real CmiTmpAlloc/Free */
#elif CMK_NOT_USE_CONVERSE /* fake CmiTmpAlloc/Free via malloc */
#  define CMI_TMP_SKIP
#  define CmiTmpAlloc(size) malloc(size)
#  define CmiTmpFree(ptr) free(ptr)
#endif

#if !CMK_HAS_SOCKLEN
typedef int socklen_t;
#endif

#if CMK_HAS_GETIFADDRS
#include <netinet/in.h> /* for sockaddr_in */
#include <ifaddrs.h> /* for getifaddrs */
#include <net/if.h> /* for IFF_RUNNING */
#endif

/*Just print out error message and exit*/
static int default_skt_abort(SOCKET skt, int code, const char *msg)
{
  fprintf(stderr,"Fatal socket error: code %d-- %s\n",code,msg);
  exit(1);
  return -1;
}

static skt_idleFn idleFunc=NULL;
static skt_abortFn skt_abort=default_skt_abort;
void skt_set_idle(skt_idleFn f) {idleFunc=f;}
skt_abortFn skt_set_abort(skt_abortFn f) 
{
	skt_abortFn old=skt_abort;
	skt_abort=f;
	return old;
}

/* These little flags are used to ignore the SIGPIPE signal
 * while we're inside one of our socket calls.
 * This lets us only handle SIGPIPEs we generated. */
static int skt_ignore_SIGPIPE=0;

#if defined(_WIN32) /*Windows systems:*/
static void doCleanup(void)
{ WSACleanup();}
static int skt_inited=0;
/*Initialization routine (Windows only)*/
void skt_init(void)
{
  WSADATA WSAData;
  WORD version=0x0002;
  if (skt_inited) return;
  skt_inited=1;
  WSAStartup(version, &WSAData);
  atexit(doCleanup);
}

void skt_close(SOCKET fd)
{
	closesocket(fd);
}
#else /*UNIX Systems:*/

typedef void (*skt_signal_handler_fn)(int sig);
static skt_signal_handler_fn skt_fallback_SIGPIPE=NULL;
static struct sigaction sa;
static void skt_SIGPIPE_handler(int sig) {
	if (skt_ignore_SIGPIPE) {
		fprintf(stderr,"Caught SIGPIPE.\n");
		sigaction(SIGPIPE, &sa, NULL);
	}
	else
		skt_fallback_SIGPIPE(sig);
}

void skt_init(void)
{
	/* Install a SIGPIPE signal handler.
	  This prevents us from dying when one of our network
	  connections goes down
	*/
	struct sigaction old_action;
	sa.sa_handler = skt_SIGPIPE_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART;
	sigaction(SIGPIPE, &sa, &old_action);
	skt_fallback_SIGPIPE = old_action.sa_handler;
}
void skt_close(SOCKET fd)
{
	skt_ignore_SIGPIPE=1;
	close(fd);
	skt_ignore_SIGPIPE=0;
}
#endif

#ifndef SKT_HAS_BUFFER_BEGIN
void skt_buffer_begin(SOCKET sk) {}
void skt_buffer_end(SOCKET sk) {}
#endif


static int ERRNO = -1;

/*Called when a socket or select routine returns
an error-- determines how to respond.
Return 1 if the last call was interrupted
by, e.g., an alarm and should be retried.
*/
static int skt_should_retry(void)
{
	int isinterrupt=0,istransient=0,istimeout=0;
#if defined(_WIN32) /*Windows systems-- check Windows Sockets Error*/
	int err=WSAGetLastError();
	if (err==WSAEINTR) isinterrupt=1;
	if (err==WSATRY_AGAIN||err==WSAECONNREFUSED)
		istransient=1;
#else /*UNIX systems-- check errno*/
	int err=errno;
	if (err==EINTR) isinterrupt=1;
	if (err==ETIMEDOUT) istimeout=1;
	if (err==EAGAIN||err==ECONNREFUSED
               ||err==EWOULDBLOCK||err==ENOBUFS
               ||err==ECONNRESET
        )
		istransient=1;
#endif
	ERRNO = err;
	if (isinterrupt) {
		/*We were interrupted by an alarm.  Schedule, then retry.*/
		if (idleFunc!=NULL) idleFunc();
	}
	else if (istransient)
	{ /*A transient error-- idle a while, then try again later.*/
		if (idleFunc!=NULL) idleFunc();
		else sleep(1);
	}
	else 
		return 0; /*Some unrecognized problem-- abort!*/
          /* timeout is used by send() for example to terminate node 
             program normally, when charmrun dies  */
	return 1;/*Otherwise, we recognized it*/
}


/* fd must be tcp socket */
int skt_tcp_no_nagle(SOCKET fd)
{
  int flag, ok;
  flag = 1;
  ok = setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(int));
  return ok;
}

#if CMK_USE_POLL
int skt_select1(SOCKET fd,int msec)
{
  struct pollfd fds[1];
  int begin, nreadable;
  int sec=msec/1000;
  int  secLeft=sec;

  fds[0].fd=fd; 
  fds[0].events=POLLIN;

  if (msec>0) begin = time(0);
  do
  {
    skt_ignore_SIGPIPE=1;
    nreadable = poll(fds, 1, msec);
    skt_ignore_SIGPIPE=0;
    
    if (nreadable < 0) {
		if (skt_should_retry()) continue;
		else skt_abort(fd, 93200, "Fatal error in poll");
	}
    if (nreadable >0) return 1; /*We gotta good socket*/
  }
  while(msec>0 && ((secLeft = sec - (time(0) - begin))>0));

  return 0;/*Timed out*/
}

#else

/*Sleep on given read socket until msec or readable*/
int skt_select1(SOCKET fd,int msec)
{
  int sec=msec/1000;
  fd_set  rfds;
  struct timeval tmo;
  int  secLeft=sec;
  int            begin, nreadable;
  
  FD_ZERO(&rfds);
  FD_SET(fd, &rfds);

  if (msec>0) begin = time(0);
  do
  {
    tmo.tv_sec=secLeft;
    tmo.tv_usec = (msec-1000*sec)*1000;
    skt_ignore_SIGPIPE=1;
    nreadable = select(1+fd, &rfds, NULL, NULL, &tmo);
    skt_ignore_SIGPIPE=0;
    
    if (nreadable < 0) {
		if (skt_should_retry()) continue;
		else skt_abort(fd, 93200, "Fatal error in select");
	}
    if (nreadable >0) return 1; /*We gotta good socket*/
  }
  while(msec>0 && ((secLeft = sec - (time(0) - begin))>0));

  return 0;/*Timed out*/
}
#endif

/******* DNS *********/
skt_ip_t _skt_invalid_ip={{0}};

skt_ip_t skt_my_ip(void)
{
  char hostname[1000];
  skt_ip_t ip = _skt_invalid_ip;
  int ifcount = 0;
#if CMK_HAS_GETIFADDRS
    /* Code snippet from  Jens Alfke
 *     http://lists.apple.com/archives/macnetworkprog/2008/May/msg00013.html */
  struct ifaddrs *ifaces=0;
  if( getifaddrs(&ifaces) == 0 ) {
        struct ifaddrs *iface;
        for( iface=ifaces; iface; iface=iface->ifa_next ) {
            if( (iface->ifa_flags & IFF_UP) && ! (iface->ifa_flags & IFF_LOOPBACK) ) {
                const struct sockaddr_in *addr = (const struct sockaddr_in*)iface->ifa_addr;
                if( addr && addr->sin_family==AF_INET ) {
                    ifcount ++;
                    if ( ifcount==1 ) memcpy(&ip, &addr->sin_addr, sizeof(ip));
                }
            }
        }
        freeifaddrs(ifaces);
  }
  /* fprintf(stderr, "My IP is %d.%d.%d.%d\n", ip.data[0],ip.data[1],ip.data[2],ip.data[3]); */
  if (ifcount==1) return ip;
#endif
  
  if (gethostname(hostname, 999)==0) {
      skt_ip_t ip2 = skt_lookup_ip(hostname);
      if ( ip2.data[0] != 127 ) return ip2;
      else if ( ifcount != 0 ) return ip;
  }

  return _skt_invalid_ip;
}

static int skt_parse_dotted(const char *str,skt_ip_t *ret)
{
  int i,v;
  *ret=_skt_invalid_ip;
  for (i=0;i<sizeof(skt_ip_t);i++) {
    if (1!=sscanf(str,"%d",&v)) return 0;
    if (v<0 || v>255) return 0;
    while (isdigit(*str)) str++; /* Advance over number */
    if (i!=sizeof(skt_ip_t)-1) { /*Not last time:*/
      if (*str!='.') return 0; /*Check for dot*/
    } else { /*Last time:*/
      if (*str!=0) return 0; /*Check for end-of-string*/
    }
    str++;
    ret->data[i]=(unsigned char)v;
  }
  return 1;
}

/* this is NOT thread safe ! */
skt_ip_t skt_lookup_ip(const char *name)
{
  skt_ip_t ret=_skt_invalid_ip;
  /*First try to parse the name as dotted decimal*/
  if (skt_parse_dotted(name,&ret))
    return ret;
  else {/*Try a DNS lookup*/
    struct hostent *h = gethostbyname(name);   /* not thread safe */
    if (h==0) return _skt_invalid_ip;
    memcpy(&ret,h->h_addr_list[0],h->h_length);
    return ret;
  }
}

/* these 2 functions will return the inner node IP, special for
   Linux Scyld.  G. Zheng 
*/
skt_ip_t skt_innode_my_ip(void)
{  
  return skt_my_ip();
}

skt_ip_t skt_innode_lookup_ip(const char *name)
{
  return skt_lookup_ip(name);
}

/*Write as dotted decimal*/
char *skt_print_ip(char *dest,skt_ip_t addr)
{
  char *o=dest;
  int i;
  for (i=0;i<sizeof(addr);i++) {
    const char *trail=".";
    if (i==sizeof(addr)-1) trail=""; /*No trailing separator dot*/
    sprintf(o,"%d%s",(int)addr.data[i],trail);
    o+=strlen(o);
  }
  return dest;
}
int skt_ip_match(skt_ip_t a,skt_ip_t b)
{
  return 0==memcmp(&a,&b,sizeof(a));
}
struct sockaddr_in skt_build_addr(skt_ip_t IP,int port)
{
  struct sockaddr_in ret={0};
  ret.sin_family=AF_INET;
  ret.sin_port = htons((short)port);
  memcpy(&ret.sin_addr,&IP,sizeof(IP));
  return ret;  
}

SOCKET skt_datagram(unsigned int *port, int bufsize)
{  
  int connPort=(port==NULL)?0:*port;
  struct sockaddr_in addr=skt_build_addr(_skt_invalid_ip,connPort);
  socklen_t          len;
  SOCKET             ret;
  
retry:
  ret = socket(AF_INET,SOCK_DGRAM,0);
  if (ret == SOCKET_ERROR) {
    if (skt_should_retry()) goto retry;  
    return skt_abort(-1, 93490, "Error creating datagram socket.");
  }
  if (bind(ret, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR)
	  return skt_abort(-1, 93491, "Error binding datagram socket.");
  
  len = sizeof(addr);
  if (getsockname(ret, (struct sockaddr *)&addr , &len))
	  return skt_abort(-1, 93492, "Error getting address on datagram socket.");

  if (bufsize) 
  {
    len = sizeof(int);
    if (setsockopt(ret, SOL_SOCKET , SO_RCVBUF , (char *)&bufsize, len) == SOCKET_ERROR) 
		return skt_abort(-1, 93495, "Error on RCVBUF sockopt for datagram socket.");
    if (setsockopt(ret, SOL_SOCKET , SO_SNDBUF , (char *)&bufsize, len) == SOCKET_ERROR) 
		return skt_abort(-1, 93496, "Error on SNDBUF sockopt for datagram socket.");
  }
  
  if (port!=NULL) *port = (int)ntohs(addr.sin_port);
  return ret;
}
SOCKET skt_server(unsigned int *port)
{
  return skt_server_ip(port,NULL);
}

SOCKET skt_server_ip(unsigned int *port,skt_ip_t *ip)
{
  SOCKET             ret;
  socklen_t          len;
  int on = 1; /* for setsockopt */
  int connPort=(port==NULL)?0:*port;
  struct sockaddr_in addr=skt_build_addr((ip==NULL)?_skt_invalid_ip:*ip,connPort);
  
retry:
  ret = socket(PF_INET, SOCK_STREAM, 0);
  
  if (ret == SOCKET_ERROR) {
    if (skt_should_retry()) goto retry;
    else return skt_abort(-1, 93483, "Error creating server socket.");
  }
  setsockopt(ret, SOL_SOCKET, SO_REUSEADDR, (char *) &on, sizeof(on));
  
  if (bind(ret, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) 
	  return skt_abort(-1, 93484, "Error binding server socket.");
  if (listen(ret,5) == SOCKET_ERROR) 
	  return skt_abort(-1, 93485, "Error listening on server socket.");
  len = sizeof(addr);
  if (getsockname(ret, (struct sockaddr *)&addr, &len) == SOCKET_ERROR) 
	  return skt_abort(-1, 93486, "Error getting name on server socket.");

  if (port!=NULL) *port = (int)ntohs(addr.sin_port);
  if (ip!=NULL) memcpy(ip, &addr.sin_addr, sizeof(*ip));
  return ret;
}

SOCKET skt_accept(SOCKET src_fd,skt_ip_t *pip, unsigned int *port)
{
  socklen_t len;
  struct sockaddr_in addr={0};
  SOCKET ret;
  len = sizeof(addr);
retry:
  ret = accept(src_fd, (struct sockaddr *)&addr, &len);
  if (ret == SOCKET_ERROR) {
    if (skt_should_retry()) goto retry;
    else return skt_abort(-1, 93523, "Error in accept.");
  }
  
  if (port!=NULL) *port=ntohs(addr.sin_port);
  if (pip!=NULL) memcpy(pip,&addr.sin_addr,sizeof(*pip));
  return ret;
}


SOCKET skt_connect(skt_ip_t ip, int port, int timeout)
{
  struct sockaddr_in addr=skt_build_addr(ip,port);
  int                ok, begin;
  SOCKET             ret;
  
  begin = time(0);
  while (time(0)-begin < timeout) 
  {
    ret = socket(AF_INET, SOCK_STREAM, 0);
    if (ret==SOCKET_ERROR) 
    {
	  if (skt_should_retry()) continue;  
      else return skt_abort(-1, 93512, "Error creating socket");
    }
    ok = connect(ret, (struct sockaddr *)&(addr), sizeof(addr));
    if (ok != SOCKET_ERROR) 
	  return ret;/*Good connect*/
	else { /*Bad connect*/
	  skt_close(ret);
	  if (skt_should_retry()) continue;
	  else {
#if ! defined(_WIN32)
            if (ERRNO == ETIMEDOUT) continue;      /* time out is fine */
#endif
            return skt_abort(-1, 93515, "Error connecting to socket\n");
          }
    }
  }
  /*Timeout*/
  if (timeout==60)
     return skt_abort(-1, 93517, "Timeout in socket connect\n");
  return INVALID_SOCKET;
}

void skt_setSockBuf(SOCKET skt, int bufsize)
{
  int len = sizeof(int);
  if (setsockopt(skt, SOL_SOCKET , SO_SNDBUF , (char *)&bufsize, len) == SOCKET_ERROR)
	skt_abort(-1, 93496, "Error on SNDBUF sockopt for datagram socket.");
  if (setsockopt(skt, SOL_SOCKET , SO_RCVBUF , (char *)&bufsize, len) == SOCKET_ERROR)
	skt_abort(-1, 93496, "Error on RCVBUF sockopt for datagram socket.");
}

int skt_recvN(SOCKET hSocket,void *buff,size_t nBytes)
{
  size_t nLeft,nRead;
  char *pBuff=(char *)buff;

  nLeft = nBytes;
  while (0 < nLeft)
  {
    if (0==skt_select1(hSocket,600*1000))
	return skt_abort(hSocket, 93610, "Timeout on socket recv!");
    skt_ignore_SIGPIPE=1;
    nRead = recv(hSocket,pBuff,nLeft,0);
    skt_ignore_SIGPIPE=0;
    if (nRead<=0)
    {
       if (nRead==0) return skt_abort(hSocket, 93620, "Socket closed before recv.");
       if (skt_should_retry()) continue;/*Try again*/
       else return skt_abort(hSocket, 93650+hSocket, "Error on socket recv!");
    }
    else
    {
      nLeft -= nRead;
      pBuff += nRead;
    }
  }
  return 0;
}

int skt_sendN(SOCKET hSocket,const void *buff,size_t nBytes)
{
  size_t nLeft,nWritten;
  const char *pBuff=(const char *)buff;
  
  nLeft = nBytes;
  while (0 < nLeft)
  {
    skt_ignore_SIGPIPE=1;
    nWritten = send(hSocket,pBuff,nLeft,0);
    skt_ignore_SIGPIPE=0;
    if (nWritten<=0)
    {
          if (nWritten==0) return skt_abort(hSocket, 93720, "Socket closed before send.");
	  if (skt_should_retry()) continue;/*Try again*/
	  else return skt_abort(hSocket, 93700+hSocket, "Error on socket send!");
    }
    else
    {
      nLeft -= nWritten;
      pBuff += nWritten;
    }
  }
  return 0;
}

/*Cheezy vector send: 
  really should use writev on machines where it's available. 
*/
#define skt_sendV_max (16*1024)

int skt_sendV(SOCKET fd,int nBuffers,const void **bufs,size_t *lens)
{
	size_t len = 0;
	int b;
	for (b=0;b<nBuffers;b++) len+=lens[b];
	if (len<=skt_sendV_max) { /*Short message: Copy and do one big send*/
		char *buf=(char *)CmiTmpAlloc(skt_sendV_max);
		char *dest=buf;
		int ret;
		for (b=0;b<nBuffers;b++) {
			memcpy(dest,bufs[b],lens[b]);
			dest+=lens[b];
		}
		ret=skt_sendN(fd,buf,len);
		CmiTmpFree(buf);
		return ret;
	}
	else { /*Big message: Just send one-by-one as usual*/
		int ret;
		for (b=0;b<nBuffers;b++) 
			if (0!=(ret=skt_sendN(fd,bufs[b],lens[b])))
				return ret;
		return 0;
	}
}

#if !defined(_WIN32)
int skt_sendmsg(SOCKET hSocket, struct msghdr *mh, int num_bufs, size_t nBytes)
{
  while (nBytes > 0) {
    skt_ignore_SIGPIPE = 1;
    size_t bytes_sent = sendmsg(hSocket, mh, 0);
    skt_ignore_SIGPIPE = 0;
    if (bytes_sent <= 0) {
      return skt_abort(hSocket, 93700+hSocket, "Error on socket send!");
    } else if (bytes_sent < nBytes) {
      // this should happen very rarely
      size_t offset = 0;
      int i;
      for (i=0; i < num_bufs; i++) {
        size_t l = mh->msg_iov[i].iov_len;
        if ((offset + l) <= bytes_sent) {
          mh->msg_iov[i].iov_len = 0; // all bytes from this buffer have been sent
          offset += l;
        } else {
          // bytes from this buffer have been partially sent
          size_t buf_transmitted = (bytes_sent - offset);
          mh->msg_iov[i].iov_len  -= buf_transmitted;
          mh->msg_iov[i].iov_base = (char*)mh->msg_iov[i].iov_base + buf_transmitted;
          break;
        }
      }
    }
    nBytes -= bytes_sent;
  }
  return 0;
}
#else
int skt_sendmsg(SOCKET hSocket, WSABUF *buffers, int num_bufs, size_t nBytes)
{
  DWORD bytes_sent = 0;
  int rc;
  skt_ignore_SIGPIPE = 1;
  rc = WSASend(hSocket, buffers, num_bufs, &bytes_sent, 0, NULL, NULL);
  skt_ignore_SIGPIPE = 0;
  if ((rc != 0) || (bytes_sent != nBytes))  {
    return skt_abort(hSocket, 93700+hSocket, "Error on socket send!");
  }
  return 0;
}
#endif

/***********************************************
  Routines for manipulating simple binary messages,
 e.g., to/from conv-host.

 A conv-host message with header has the following format 
on the network: 
ChMessage---------------------------------------------
 /--ChMessageHeader----------------------------     ^
 |12 bytes  |   Message type field           ^      |
 |          |   (ASCII, Null-terminated)     |      |
 +-----------------------------------     24 bytes  |
 | 4 bytes  |   Message data length d        |      |
 |          |   (big-endian binary integer)  |      |
 +-----------------------------------        |      |
 | 4 bytes  |   Return IP address            |      |
 | 4 bytes  |   Return TCP port number       |      |
 |          |   (big-endian binary integers) v  24+d bytes
 \---------------------------------------------     |
 d bytes  |   User data                             v
------------------------------------------------------

For completeness, a big-endian (network byte order) 4 byte 
integer has this format on the network:
ChMessageInt---------------------------------
  1 byte | Most significant byte  (&0xff000000; <<24)
  1 byte | More significant byte  (&0x00ff0000; <<16)
  1 byte | Less significant byte  (&0x0000ff00; <<8)
  1 byte | Least significant byte (&0x000000ff; <<0)
----------------------------------------------
*/

ChMessageInt_t ChMessageInt_new(unsigned int src)
{ /*Convert integer to bytes*/
  int i; ChMessageInt_t ret;
  for (i=0;i<4;i++) ret.data[i]=(unsigned char)(src>>(8*(3-i)));
  return ret;
}
unsigned int ChMessageInt(ChMessageInt_t src)
{ /*Convert bytes to integer*/
  int i; unsigned int ret=0;
  for (i=0;i<4;i++) {ret<<=8;ret+=src.data[i];}
  return (int)ret;
}

ChMessageLong_t ChMessageLong_new(CMK_TYPEDEF_UINT8 src)
{ /*Convert long integer to bytes*/
  int i; ChMessageLong_t ret;
  for (i=0;i<8;i++) ret.data[i]=(unsigned char)(src>>(8*(7-i)));
  return ret;
}
CMK_TYPEDEF_UINT8 ChMessageLong(ChMessageLong_t src)
{ /*Convert bytes to long integer*/
  int i; CMK_TYPEDEF_UINT8 ret=0;
  for (i=0;i<8;i++) {ret<<=8;ret+=src.data[i];}
  return (CMK_TYPEDEF_UINT8)ret;
}

int ChMessage_recv(SOCKET fd,ChMessage *dst)
{
  /*Get the binary header*/
  if (0!=ChMessageHeader_recv(fd,dst)) return -1;
  if (0!=ChMessageData_recv(fd,dst)) return -1;
  return 0;
}

int ChMessageHeader_recv(SOCKET fd,ChMessage *dst)
{
  /*Get the binary header*/
  if (0!=skt_recvN(fd,(char *)&dst->header,sizeof(dst->header))) return -1;
  /*Allocate a recieve buffer*/
  dst->len=ChMessageInt(dst->header.len);
  dst->data=0;
  return 0;
}
int ChMessageData_recv(SOCKET fd,ChMessage *dst)
{
  dst->data=(char *)malloc(dst->len);
  /*Get the actual data*/
  if (0!=skt_recvN(fd,dst->data,dst->len)) return -1;
  return 0;
}

void ChMessage_free(ChMessage *doomed)
{
  free(doomed->data);
  strncpy(doomed->header.type,"Free'd",CH_TYPELEN);
  doomed->data=NULL;
  doomed->len=-1234;
}
void ChMessageHeader_new(const char *type,int len,ChMessageHeader *dst)
{
  dst->len=ChMessageInt_new(len);
  if (type==NULL) type="default";
  strncpy(dst->type,type,CH_TYPELEN);
}
void ChMessage_new(const char *type,int len,ChMessage *dst)
{
  ChMessageHeader_new(type,len,&dst->header);
  dst->len=len;
  dst->data=(char *)malloc(dst->len);
}
int ChMessage_send(SOCKET fd,const ChMessage *src)
{
  const void *bufs[2]; size_t lens[2];
  bufs[0]=&src->header; lens[0]=sizeof(src->header);
  bufs[1]=src->data; lens[1]=src->len;
  return skt_sendV(fd,2,bufs,lens);
} /*You must free after send*/

#else

skt_ip_t _skt_invalid_ip={{0}};

skt_ip_t skt_my_ip(void)
{
  return _skt_invalid_ip;
}

#endif /*!CMK_NO_SOCKETS*/



