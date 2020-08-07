/**************************************************************************
 *
 * SKT - simple TCP and UDP socket routines.
 *  All port numbers are taken and returned
 *  in *host* byte order.  This means you can hardcode port
 *  numbers in the code normally, and they will be properly
 *  translated even on little-endian machines.
 *
 *  SOCKET is just a #define for "unsigned int".
 *
 *  skt_ip_t is a flat bytes structure to hold an IP address--
 *  this is either 4 bytes (for IPv4) or 16 bytes (for IPv6).
 *  It is always in network byte order.
 *
 *  Errors are handled in the library by calling a user-overridable
 *  abort function.
 *
 * skt_ip_t skt_my_ip(void)
 *   - return the IP address of the current machine.
 *
 * skt_ip_t skt_lookup_ip(const char *name)
 *   - return the IP address of the given machine (DNS or dotted decimal).
 *     Returns 0 on failure.
 *
 * char *skt_print_ip(char *dest,skt_ip_t addr)
 *   - Print the given IP address to the given destination as
 *     dotted decimal.  Dest must be at least 130 bytes long,
 *     and will be returned.
 *
 * int skt_ip_match(skt_ip_t a,skt_ip_t b)
 *   - Return 1 if the given IP addresses are identical.
 *
 * SOCKET skt_datagram(unsigned int *port, int bufsize)
 *
 *   - creates a UDP datagram socket on the given port.
 *     Performs the whole socket/bind/getsockname procedure.
 *     Returns the actual port of the socket and
 *     the file descriptor.  Bufsize, if nonzero, controls the amount
 *     of buffer space the kernel sets aside for the socket.
 *
 * SOCKET skt_server(unsigned int *port)
 *
 *   - create a TCP server socket on the given port (0 for any port).
 *     Performs the whole socket/bind/listen procedure.
 *     Returns the actual port of the socket and the file descriptor.
 *
 * SOCKET skt_server_ip(unsigned int *port,skt_ip_t *ip)
 *
 *   - create a TCP server socket on the given port and IP
 *     Use 0 for any port and _skt_invalid_ip for any IP.
 *     Performs the whole socket/bind/listen procedure.
 *     Returns the actual port and IP address of the socket
 *     and the file descriptor.
 *
 * SOCKET skt_accept(SOCKET src_fd,skt_ip_t *pip, unsigned int *port)
 *
 *   - accepts a TCP connection to the specified server socket.  Returns the
 *     IP of the caller, the port number of the caller, and the file
 *     descriptor to talk to the caller.
 *
 * SOCKET skt_connect(skt_ip_t ip, int port, int timeout)
 *
 *   - Opens a TCP connection to the specified server.  Returns a socket for
 *     communication.
 *
 * void skt_close(SOCKET fd)
 *   - Finishes communication on and closes the given socket.
 *
 * int skt_select1(SOCKET fd,int msec)
 *   - Call select on the given socket, returning as soon as
 *     the socket can recv or accept, or (failing that) in the given
 *     number of milliseconds.  Returns 0 on timeout; 1 on readable.
 *
 * int skt_recvN(SOCKET fd,      void *buf,size_t nBytes)
 * int skt_sendN(SOCKET fd,const void *buf,size_t nBytes)
 *   - Blocking send/recv nBytes on the given socket.
 *     Retries if possible (e.g., if interrupted), but aborts
 *     on serious errors.  Returns zero or an abort code.
 *
 * int skt_sendV(SOCKET fd,int nBuffers,void **buffers,size_t *lengths)
 *   - Blocking call to write from several buffers.  This is much more
 *     performance-critical than read-from-several buffers, because
 *     individual sends go out as separate network packets, and include
 *     a (35 ms!) timeout for subsequent short messages.  Don't use more
 *     than 8 buffers.
 *
 * void skt_set_idle(idleFunc f)
 *   - Specify a routine to be called while waiting for the network.
 *     Replaces any previous routine.
 *
 * void skt_set_abort(abortFunc f)
 *   - Specify a routine to be called when an unrecoverable
 *     (i.e., non-transient) socket error is encountered.
 *     The default is to log the message to stderr and call exit(1).
 *
 **************************************************************************/
#ifndef __SOCK_ROUTINES_H
#define __SOCK_ROUTINES_H

#include "conv-config.h" /*<- for CMK_NO_SOCKETS*/

#include <string.h> /* for memcmp */

#ifdef CMK_NO_SOCKETS
#define SOCKET int
#define SOCKET_ERROR (-1)
#define INVALID_SOCKET (SOCKET)(~0)
typedef struct {
  int tag;
} skt_ip_t;

extern skt_ip_t _skt_invalid_ip;
skt_ip_t skt_my_ip(void);

#else /*Use actual sockets*/

/*Preliminaries*/
#if defined(_WIN32)
/*For windows systems:*/
#include <winsock2.h>
static void sleep(int secs) { Sleep(1000 * secs); }

#else
/*For non-windows (UNIX) systems:*/
#include <sys/types.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>

#ifndef SOCKET
#define SOCKET int
#define INVALID_SOCKET (SOCKET)(~0)
#define SOCKET_ERROR (-1)
#endif /*def SOCKET*/

#endif /*_WIN32*/

#ifdef __cplusplus
extern "C" {
#endif

/*Initialization*/
void skt_init(void);

/*Error and idle handling*/
typedef void (*skt_idleFn)(void);
typedef int (*skt_abortFn)(SOCKET skt, int errCode, const char *msg);
void skt_set_idle(skt_idleFn f);
skt_abortFn skt_set_abort(skt_abortFn f);

/*DNS*/
typedef struct {/*IPv4 IP address*/
  unsigned char data[4];
} skt_ip_t;
extern skt_ip_t _skt_invalid_ip;
skt_ip_t skt_my_ip(void);
skt_ip_t skt_lookup_ip(const char *name);
skt_ip_t skt_innode_my_ip(void); /* inner node version */
skt_ip_t skt_innode_lookup_ip(const char *name);

char *skt_print_ip(char *dest, skt_ip_t addr);
int skt_ip_match(skt_ip_t a, skt_ip_t b);
struct sockaddr_in skt_build_addr(skt_ip_t IP, int port);

/*UDP*/
SOCKET skt_datagram(unsigned int *port, int bufsize);

/*TCP*/
SOCKET skt_server(unsigned int *port);
SOCKET skt_server_ip(unsigned int *port, skt_ip_t *ip);
SOCKET skt_accept(SOCKET src_fd, skt_ip_t *pip, unsigned int *port);
SOCKET skt_connect(skt_ip_t ip, int port, int timeout);

/*Utility*/
void skt_close(SOCKET fd);
int skt_select1(SOCKET fd, int msec);
void skt_setSockBuf(SOCKET skt, int bufsize);

/*Blocking Send/Recv*/
int skt_sendN(SOCKET hSocket, const void *pBuff, size_t nBytes);
int skt_recvN(SOCKET hSocket,       void *pBuff, size_t nBytes);
int skt_sendV(SOCKET fd, int nBuffers, const void **buffers, size_t *lengths);
#if !defined(_WIN32)
int skt_sendmsg(SOCKET hSocket, struct msghdr *mh, int num_bufs, size_t total_bytes);
#else
int skt_sendmsg(SOCKET hSocket, WSABUF *buffers, int num_bufs, size_t total_bytes);
#endif

int skt_tcp_no_nagle(SOCKET fd);

#ifdef __cplusplus
}
#endif

#endif /*!CMK_NO_SOCKETS*/

/***********************************
Conv-host messages: these are a simple
binary format message, with the usual header,
then data arrangement.

A fundamental data type is a ChMessageInt_t,
a simple 4-byte big-endian (network byte order)
integer.  Routines are provided to read/write
these integers on all platforms, regardless of
endian-ness or native integer size.

 ChMessage_recv reads a ChMessage on a socket.
The ChMessage->data field is allocated to contain
the entire message, and the header is filled out
with the received fields.  You may keep or dispose
of the message memory with ChMessage_free.

 ChMessageHeader_new fills out the fields of a header--
no allocation is done.

 ChMessage_new fills out the header and allocates a
data area of the given size.

 ChMessage_send delivers the given ChMessage to a
socket.  You are still responsible for the ChMessage
memory (use ChMessage_free).  If you prefer, you may
receive sizeof(ChMessageHeader) header bytes, then
header->len data bytes on any socket yourself.
*/
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  unsigned char data[4]; /*4-byte, big-endian integer*/
} ChMessageInt_t;
ChMessageInt_t ChMessageInt_new(unsigned int src); /*Convert integer to bytes*/
unsigned int ChMessageInt(ChMessageInt_t src);     /*Convert bytes to integer*/

typedef struct {
  unsigned char data[8]; /*8-byte, big-endian integer*/
} ChMessageLong_t;
ChMessageLong_t
ChMessageLong_new(CMK_TYPEDEF_UINT8 src); /*Convert long integer to bytes*/
CMK_TYPEDEF_UINT8
ChMessageLong(ChMessageLong_t src); /*Convert bytes to long integer*/

#define CH_TYPELEN 12 /*Maximum length for the message type field*/
typedef struct ChMessageHeader {
  ChMessageInt_t len;    /*Length of message to follow (not incl. header)*/
  char type[CH_TYPELEN]; /*Kind of message to follow:
      (zero-terminated ASCII string)
      "getinfo" -- return a list of node IPs and control ports
      "req" -- a CCS message
      */
} ChMessageHeader;

typedef struct ChMessage {
  ChMessageHeader header;
  int len;    /*Length of message data below*/
  char *data; /*Pointer to heap-allocated data*/
} ChMessage;
int ChMessage_recv(SOCKET fd, ChMessage *dst);
int ChMessageHeader_recv(SOCKET fd, ChMessage *dst);
int ChMessageData_recv(SOCKET fd, ChMessage *dst);
void ChMessage_free(ChMessage *doomed);
void ChMessageHeader_new(const char *type, int len, ChMessageHeader *dst);
void ChMessage_new(const char *type, int len, ChMessage *dst);
int ChMessage_send(SOCKET fd,
                   const ChMessage *src); /*You must free after send*/

#if CMK_USE_IBVERBS | CMK_USE_IBUD
typedef struct {
  ChMessageInt_t nodeno;
  ChMessageInt_t lid, qpn, psn;
} ChInfiAddr;
#endif

#define ChInitNodetabFields 2
#define ChInitNodeforkFields 2

typedef struct {
  ChMessageInt_t nodeno;
  ChMessageInt_t nProcessesInPhysNode; /* Number of distinct OS processes on
                                          this physical hardware node */
  ChMessageInt_t nPE; /* Number of worker threads in this OS process */
  ChMessageInt_t dataport; /* node's data port (UDP or GM) */
  ChMessageInt_t mach_id;  /* node's hardware address (GM-only) */
  skt_ip_t IP; /* node's IP address */
} ChNodeinfo;

typedef struct {
  ChMessageInt_t nodeNo;
  ChMessageInt_t num_pus;
  ChMessageInt_t num_cores;
  ChMessageInt_t num_sockets;
  ChNodeinfo info;
} ChSingleNodeinfo;

/******* CCS Message type (included here for convenience) *******/
#define CCS_HANDLERLEN 32 /*Maximum length for the handler field*/
typedef struct {
  ChMessageInt_t len;           /*Length of user data to follow header*/
  ChMessageInt_t pe;            /*Destination processor number*/
  char handler[CCS_HANDLERLEN]; /*Handler name for message to follow*/
} CcsMessageHeader;

#ifdef __cplusplus
}

static inline bool operator< (const skt_ip_t & a, const skt_ip_t & b)
{
  return memcmp(&a, &b, sizeof(a)) < 0;
}
#endif

#endif /*SOCK_ROUTINES_H*/
