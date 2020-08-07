/*
Converse Client/Server: Server-side interface
Orion Sky Lawlor, 9/11/2000, olawlor@acm.org

This file describes the under-the-hood implementation
of the CCS Server.  Here's where Ccs requests from the
network are actually received.
*/
#include <stdio.h>
#include <ctype.h>
#include "conv-ccs.h"
#include "ccs-server.h"
#include "ccs-auth.h"

#if CMK_CCS_AVAILABLE

/****************** Security and Server Utilities ***************/
/*Security guard for a CCS server*/
struct CcsSecMan;
typedef int (*CcsSecMan_allowFn) 
     (struct CcsSecMan *self,CcsSecAttr *attr);
typedef CcsSec_secretKey * (*CcsSecMan_getKeyFn) 
     (struct CcsSecMan *self,CcsSecAttr *attr);

typedef struct CcsSecMan {
  CcsSecMan_allowFn allowRequest;
  CcsSecMan_getKeyFn getKey;
  CcsSec_secretKey * keys[256];
} CcsSecMan;


/*A table of connected clients*/
typedef struct {
  int nClients;
  int *clients; /*Gives current salt of corresponding client.*/
  CCS_RAND_state rand;
} CCS_AUTH_clients;

static void CCS_AUTH_new(CCS_AUTH_clients *cl)
{
  cl->nClients=0;
  cl->clients=NULL;
  CCS_RAND_new(&cl->rand);
}
static int CCS_AUTH_numClients(CCS_AUTH_clients *cl) {
  return cl->nClients;
}
static int CCS_AUTH_addClient(CCS_AUTH_clients *cl) {
  int clientNo=cl->nClients++;
  if ((clientNo%64)==0)
    cl->clients=(int*)realloc(cl->clients,sizeof(int)*(cl->nClients+63));
  cl->clients[clientNo]=CCS_RAND_next(&cl->rand);
  return clientNo;
}
static int CCS_AUTH_clientSalt(CCS_AUTH_clients *cl,int clientNo) {
  return cl->clients[clientNo];
}
static void CCS_AUTH_advanceSalt(CCS_AUTH_clients *cl,int clientNo) {
  cl->clients[clientNo]++;
}

/***********************************************
Send the given data as a CCS reply on the given socket.
Goes to some effort to minimize the number of "sends"
(and hence outgoing TCP packets) by assembling the 
header and data in-place.
*/
static void CcsServer_writeReply(SOCKET fd,
			 CcsSecMan *security,
			 CcsSecAttr *attr,
			 int replyLen,char *reply)
{
  const void *bufs[3]; size_t lens[3]; int nBuffers=0;
  struct { /*Authentication header*/
    SHA1_hash_t hash;
  } aheader;
  struct { /*Reply header*/
    ChMessageInt_t len;
  } header;
  if (attr->auth==1) 
  { /*Compose a reply SHA-1 hash header*/
    CCS_AUTH_hash(security->getKey(security,attr),
		  ChMessageInt(attr->replySalt),NULL,&aheader.hash);
    bufs[nBuffers]=&aheader; lens[nBuffers]=sizeof(aheader); nBuffers++;
  }
  /*Compose a simple reply header*/
  header.len=ChMessageInt_new(replyLen);
  bufs[nBuffers]=&header; lens[nBuffers]=sizeof(header); nBuffers++;
  bufs[nBuffers]=reply; lens[nBuffers]=replyLen; nBuffers++;
  if (-1==skt_sendV(fd,nBuffers,bufs,lens)) return;
  skt_close(fd);
#undef n
}

/********************************************************
Authenticate incoming request for a client salt value.
Exchange looks like:

1.) Client sends request code 0x80 (SHA-1), 0x00 (version 0), 
0x01 (create salt), 0xNN (security level); followed by 
client challenge (4 bytes, s1)

2.) Server replies with server challenge (4 bytes, s2)

3.) Client replies with hashed key & server challenge (20 bytes, s2hash)

4.) Server replies with hashed key & client challenge (20 bytes, s1hash),
as well as client identifier and initial client salt. (8 bytes total).
*/
static const char *CcsServer_createSalt(SOCKET fd,CCS_AUTH_clients *cl,
					CcsSecMan *security,CcsSecAttr *attr)
{
  ChMessageInt_t s1;
  ChMessageInt_t s2=ChMessageInt_new(CCS_RAND_next(&cl->rand));
  SHA1_hash_t s2hash;
  int clientId;
  struct {
    SHA1_hash_t s1hash;
    ChMessageInt_t clientId;
    ChMessageInt_t clientSalt;
  } reply;
  if (-1==skt_recvN(fd,&s1,sizeof(s1))) return "ERROR> CreateSalt challenge recv";
  if (-1==skt_sendN(fd,&s2,sizeof(s2))) return "ERROR> CreateSalt challenge send";
  if (-1==skt_recvN(fd,&s2hash,sizeof(s2hash))) return "ERROR> CreateSalt reply recv";
  if (CCS_AUTH_differ(security->getKey(security,attr),ChMessageInt(s2),
		      NULL,&s2hash))
    return "ERROR> CreateSalt client hash mismatch! (bad password?)";
  CCS_AUTH_hash(security->getKey(security,attr),ChMessageInt(s1),
		NULL,&reply.s1hash);
  clientId=CCS_AUTH_addClient(cl);
  reply.clientId=ChMessageInt_new(clientId);
  reply.clientSalt=ChMessageInt_new(CCS_AUTH_clientSalt(cl,clientId));
  if (-1==skt_sendN(fd,&reply,sizeof(reply))) return "ERROR> CreateSalt reply send";
  /*HACK: this isn't an error return, and returning an error code
   here is wrong; but all we want is to close the socket (not process
   a CCS request), and printing out this text isn't a bad idea, so... 
  */
  return "Created new client";
}

/*******************
Grab an ordinary authenticated message off this socket.
The format is:
-4 byte client ID number (returned by createSalt earlier)
-4 byte client challenge (used to by client to authenticate reply)
-20 byte authentication hash code
-Regular CcsMessageHeader
*/
static const char *CcsServer_SHA1_message(SOCKET fd,CCS_AUTH_clients *cl,
					CcsSecMan *security,CcsSecAttr *attr,
					CcsMessageHeader *hdr)
{
  ChMessageInt_t clientNo_net;
  int clientNo;
  unsigned int salt;
  SHA1_hash_t hash;

  /* An ordinary authenticated message */      
  if (-1==skt_recvN(fd,&clientNo_net,sizeof(clientNo_net)))
    return "ERROR> During recv. client number";
  if (-1==skt_recvN(fd,&attr->replySalt,sizeof(attr->replySalt)))
    return "ERROR> During recv. reply salt";
  if (-1==skt_recvN(fd,&hash,sizeof(hash)))
    return "ERROR> During recv. authentication hash";
  if (-1==skt_recvN(fd,hdr,sizeof(CcsMessageHeader)))
    return "ERROR> During recv. message header";
  clientNo=ChMessageInt(clientNo_net);
  
  if (clientNo<0 || clientNo>=CCS_AUTH_numClients(cl))
    return "ERROR> Bad client number in SHA-1 request!";
  salt=CCS_AUTH_clientSalt(cl,clientNo);
  
  /*Check the client's hash*/
  if (CCS_AUTH_differ(security->getKey(security,attr),salt,
		      hdr,&hash))
    return "ERROR> Authentication hash code MISMATCH-- bad or faked key";

  CCS_AUTH_advanceSalt(cl,clientNo);
  return NULL; /*It's a good message*/
}

/*********************
Grab a message header from this socket.
 */
static const char *CcsServer_readHeader(SOCKET fd,CCS_AUTH_clients *cl,
			CcsSecMan *security,
			CcsSecAttr *attr,CcsMessageHeader *hdr) 
{
  /*Read the first bytes*/
  unsigned char len[4];
  if (-1==skt_recvN(fd,&len[0],sizeof(len)))
    return "ERROR> During recv. length";
  
  /*
    Decide what kind of message it is by the high byte of the length field.
  */
  if (len[0]<0x20) 
  { /*Unauthenticated message-- do a security check*/
      attr->auth=0;
      attr->level=0;
      attr->replySalt=ChMessageInt_new(0);
      if (!security->allowRequest(security,attr))
	return "ERROR> Unauthenticated request denied at security check";
    /*Request is authorized-- grab the rest of the header*/
      hdr->len=*(ChMessageInt_t *)len;
      if (-1==skt_recvN(fd,&hdr->pe,sizeof(hdr->pe))) 
	return "ERROR> During recv. PE";
      if (-1==skt_recvN(fd,&hdr->handler[0],sizeof(hdr->handler))) 
	return "ERROR> During recv. handler name"; 
      return NULL; /*it's a good message*/
  }
  else if (len[0]==0x80)
  { /*SHA-1 Authenticated request*/
      if (len[1]!=0x00)
	return "ERROR> Bad SHA-1 version field!";
      attr->auth=1;
      attr->level=len[3];/*Requested security level.*/
      if (!security->allowRequest(security,attr))
	return "ERROR> Authenticated request denied at security check";
      
      switch(len[2]) {
      case 0x00: /*Regular message*/
	return CcsServer_SHA1_message(fd,cl,security,attr,hdr); 
      case 0x01: /*Request for salt*/
	return CcsServer_createSalt(fd,cl,security,attr);
      default: 
	return "ERROR> Bad SHA-1 request field!";
      };
  }
  else
    return "ERROR> Unknown authentication protocol";
}

/*******************************************************************
Default security manager-- without any files, allow 
unauthenticated calls at level 0.  If securityString is
non-NULL, get keys for higher levels by reading this 
text file.
*/
static int allowRequest_default
     (struct CcsSecMan *self,CcsSecAttr *attr)
{
  if (attr->auth==0) 
  { /*Non-authenticated request-- allow only if *no* password for level zero*/
    return NULL==self->keys[0];
  } else {
    /*Authenticated request-- allow only if we *have* a password*/
    return NULL!=self->keys[attr->level];
  }
}

static CcsSec_secretKey *getKey_default
     (struct CcsSecMan *self,CcsSecAttr *attr)
{
  return self->keys[attr->level];
}

static void CcsSecMan_make_otp(const char *str,CcsSec_secretKey *key)
{
  int i;
  CCS_RAND_state state;
  CCS_RAND_new(&state);
  i=0;
  while (str[i]!=0 && i<sizeof(state)) {
    state.state[i] ^= str[i];
    i++;
  }
  for (i=0;i<sizeof(key->data)/sizeof(int);i++) {
    unsigned int cur=CCS_RAND_next(&state);
    key->data[4*i+0]=(unsigned char)(cur>>24);
    key->data[4*i+1]=(unsigned char)(cur>>16);
    key->data[4*i+2]=(unsigned char)(cur>> 8);
    key->data[4*i+3]=(unsigned char)(cur>> 0);
  }
}

static void CcsSecMan_printkey(FILE *out,int level,CcsSec_secretKey *k)
{
  int i;
  fprintf(out,"CCS_OTP_KEY> Level %d key: ",level);
  for (i=0;i<sizeof(k->data);i++) 
    fprintf(out,"%02X",k->data[i]);
  fprintf(out,"\n");
}

static CcsSecMan *CcsSecMan_default(const char *authFile)
{
  int i;
  FILE *secFile;
  char line[200];
  CcsSecMan *ret=(CcsSecMan *)malloc(sizeof(CcsSecMan));
  ret->allowRequest=allowRequest_default;
  ret->getKey=getKey_default;
  for (i=0;i<256;i++) {
    ret->keys[i]=NULL; /*Null means no password set-- disallow unless level 0*/
  }
  if (authFile==NULL) return ret;
  secFile=fopen(authFile,"r");
  if (secFile==NULL) {
    fprintf(stderr,"CCS ERROR> Cannot open CCS authentication file '%s'!\n",
		  authFile);
    exit(1);
  }
  while (NULL!=fgets(line,200,secFile)) {
    int level;
    char key[200]; /*Secret key, in ASCII hex*/
    int nItems=sscanf(line,"%d%s",&level,key);
    if (nItems==2 && level>=0 && level<255) {
	/*Parse out the secret key*/
	CcsSec_secretKey *k=(CcsSec_secretKey *)malloc(sizeof(CcsSec_secretKey));
	memset(k->data,0,sizeof(CcsSec_secretKey));
	if (isxdigit(key[0]) && isxdigit(key[1]))
	  CCS_AUTH_makeSecretKey(key,k);
	else if (0==strncmp("OTP",key,3)) {
	  FILE *keyDest=stdout;
	  CcsSecMan_make_otp(&key[3],k);
	  CcsSecMan_printkey(keyDest,level,k);
	}
	else {
	  fprintf(stderr,"CCS ERROR> Cannot parse key '%s' for level %d from CCS security file '%s'!\n",
		  key,level,authFile);
	  exit(1);
	}
	ret->keys[level]=k;
    }
  }
  fclose(secFile);
  return ret;
}

/*********************************************************/
#define CCSDBG(x) //printf x

/*CCS Server state is all stored in global variables.
Since there's only one server, this is ugly but OK.
*/
static SOCKET ccs_server_fd=SOCKET_ERROR;/*CCS request socket*/
static CCS_AUTH_clients ccs_clientlist;
static CcsSecMan *security;

/*Make a new Ccs Server socket, on the given port.
Returns the actual port and IP address.
*/
void CcsServer_new(skt_ip_t *ret_ip,int *use_port,const char *authFile)
{
  char ip_str[200];
  skt_ip_t ip;
  unsigned int port=0;if (use_port!=NULL) port=*use_port;
  
  CCS_AUTH_new(&ccs_clientlist);
  security=CcsSecMan_default(authFile);
  skt_init();
  ip=skt_my_ip();
  ccs_server_fd=skt_server(&port);
  printf("ccs: %s\nccs: Server IP = %s, Server port = %u $\n", 
           CMK_CCS_VERSION, skt_print_ip(ip_str,ip), port);
  fflush(stdout);
  if (ret_ip!=NULL) *ret_ip=ip;
  if (use_port!=NULL) *use_port=port;
}

/*Get the Ccs Server socket.  This socket can
be added to the rdfs list for calling select().
*/
SOCKET CcsServer_fd(void) {return ccs_server_fd;}

/*Connect to the Ccs Server socket, and 
receive a ccs request from the network.
Returns 1 if a request was successfully received.
reqData is allocated with malloc(hdr->len).
*/
static int req_abortFn(SOCKET skt, int code, const char *msg) {
	/*Just ignore bad requests-- indicates a client is messed up*/
	fprintf(stderr,"CCS ERROR> Socket abort during request-- ignoring\n");
	return -1;
}

static int CcsServer_recvRequestData(SOCKET fd,
				     CcsImplHeader *hdr,void **reqData)
{
  CcsMessageHeader req;/*CCS header, from requestor*/
  int reqBytes, numPes, destPE;
  const char *err;
  if (NULL!=(err=CcsServer_readHeader(fd,&ccs_clientlist,security,
				      &hdr->attr,&req))) 
  { /*Not a regular message-- write error message and return error.*/
    fprintf(stdout,"CCS %s\n",err);
    return 0;
  }

  /*Fill out the internal CCS header*/
  strncpy(hdr->handler,req.handler,CCS_MAXHANDLER);  
  hdr->pe=req.pe;
  hdr->len=req.len;
  hdr->replyFd=ChMessageInt_new(fd);

  /*Is it a multicast?*/
  numPes = 0;
  destPE = ChMessageInt(hdr->pe);
  if (destPE < -1) numPes = -destPE;
  
  /*Grab the user data portion of the message*/
  reqBytes=ChMessageInt(req.len) + numPes*sizeof(ChMessageInt_t);
  *reqData=(char *)malloc(reqBytes);
  if (-1==skt_recvN(fd,*reqData,reqBytes)) {
    fprintf(stdout,"CCS ERROR> Retrieving %d message bytes\n",reqBytes);
    free(*reqData);
    return 0;
  }
  return 1;
}

int CcsServer_recvRequest(CcsImplHeader *hdr,void **reqData) 
{
  char ip_str[200];
  skt_ip_t ip;
  unsigned int port,ret=1;
  SOCKET fd;
  skt_abortFn old=skt_set_abort(req_abortFn);

  CCSDBG(("CCS Receiving connection...\n"));
  fd=skt_accept(ccs_server_fd,&ip,&port);

  CCSDBG(("CCS   Connected to IP=%s, port=%d...\n",skt_print_ip(ip_str,ip),port));
  hdr->attr.ip=ip;
  hdr->attr.port=ChMessageInt_new(port);

  if (0==CcsServer_recvRequestData(fd,hdr,reqData))
  {
    fprintf(stdout,"During CCS Client IP:port (%s:%d) processing.\n",
	    skt_print_ip(ip_str,ip),
	    port);
    skt_close(fd);
    ret=0;
  }

  CCSDBG(("CCS   Ret %d request.\n",ret));
  skt_set_abort(old);

  return ret;
}

static int reply_abortFn(SOCKET skt, int code, const char *msg) {
	/*Just ignore bad replies-- just indicates a client has died*/
	fprintf(stderr,"CCS ERROR> Socket abort during reply-- ignoring\n");
	return -1;
}

/*Send a Ccs reply down the given socket.
Closes the socket afterwards.
A CcsImplHeader len field equal to 0 means do not send any reply.
*/
void CcsServer_sendReply(CcsImplHeader *hdr,int repBytes,const void *repData)
{
  int fd=ChMessageInt(hdr->replyFd);
  skt_abortFn old;
  if (ChMessageInt(hdr->len)==0) {
    CCSDBG(("CCS Closing reply socket without a reply.\n"));
    skt_close(fd);
    return;
  }
  old=skt_set_abort(reply_abortFn);
  CCSDBG(("CCS   Sending %d bytes of reply data\n",repBytes));
  CcsServer_writeReply(fd,security,&hdr->attr,repBytes,(char *)repData);
  skt_close(fd);
  CCSDBG(("CCS Reply socket closed.\n"));
  skt_set_abort(old);
}

/***************************************************************************
 * Routines to handle standard out/err forwarding from application to remote
 * program connecting through CCS (used by CharmDebug)
 ***************************************************************************/

#define REDIRECT_STDIO  "redirect stdio"
#define FETCH_STDIO "fetch stdio"
char *stdio_buffer = NULL;
int stdio_size = 0;
int stdio_alloc = 0;
int stdio_waiting = 0;
CcsImplHeader stdio_waiting_hdr;

void write_stdio_duplicate(char* data) {
  if (stdio_alloc > 0) {
    int size = strlen(data);
    
    if (stdio_waiting) {
      stdio_waiting = 0;
      CcsServer_sendReply(&stdio_waiting_hdr,size+1,data);
    }
    else {
      if (size+stdio_size >= stdio_alloc) {
        char *newbuf;
        stdio_alloc += (size>4096 ? size : 4096);
        newbuf = (char*)malloc(stdio_alloc);
        memcpy(newbuf, stdio_buffer, stdio_size);
        free(stdio_buffer);
        stdio_buffer = newbuf;
      }
      strcpy(&stdio_buffer[stdio_size], data);
      stdio_size += size;
    }
  }
}

int check_stdio_header(CcsImplHeader *hdr) {
  if (strncmp(REDIRECT_STDIO, hdr->handler, strlen(REDIRECT_STDIO))==0) {
    /*This is a request to make a duplicate to stdio*/
    if (stdio_alloc == 0) {
      stdio_alloc = 4096;
      stdio_buffer = (char*)malloc(stdio_alloc);
    }
    CcsServer_sendReply(hdr,0,0);
  }
  else if (strncmp(FETCH_STDIO, hdr->handler, strlen(FETCH_STDIO))==0) {
    /*Reply with the data loaded until now*/
    if (stdio_size > 0) {
      hdr->len = ChMessageInt_new(1); /* fake len to prevent socket closed without reply! */
      CcsServer_sendReply(hdr,stdio_size,stdio_buffer);
      stdio_size = 0;
    } else {
      if (stdio_waiting) {
        CcsServer_sendReply(&stdio_waiting_hdr,0,0);
      }
      stdio_waiting = 1;
      stdio_waiting_hdr = *hdr;
      stdio_waiting_hdr.len = ChMessageInt_new(1); /* fake len to prevent socket closed without reply! */
    }
  } else {
    return 0;
  }
  return 1;
}

#if ! CMK_CMIPRINTF_IS_A_BUILTIN
#define MAX_PRINT_BUF_SIZE 8192
int print_fw_handler_idx;

/* Receives messages passed to processor 0 by all other processors as a
 * consequence of prints in debug mode.
 */
void print_fw_handler(char *msg) {
  write_stdio_duplicate(msg+CmiReservedHeaderSize);
}

/* Forward prints to node0 to be buffered and delivered through CCS */
void print_node0(const char *format, va_list args) {
  char buffer[MAX_PRINT_BUF_SIZE];
  int len;
  if ((len=vsnprintf(buffer, MAX_PRINT_BUF_SIZE, format, args)) >= MAX_PRINT_BUF_SIZE) CmiAbort("CmiPrintf: printing buffer too long\n");
  if (CmiMyPe() == 0) {
    /* We are the print server, just concatenate the printed string */
    write_stdio_duplicate(buffer);
  } else {
    /* Need to forward the string to processor 0 */
    char* msg = (char *)CmiAlloc(CmiReservedHeaderSize+len+1);
    memcpy(msg+CmiReservedHeaderSize, buffer, len+1);
    CmiSetHandler(msg,print_fw_handler_idx);
    CmiSyncSendAndFree(0,CmiReservedHeaderSize+len+1,msg);
  }
}
#endif

#endif /*CMK_CCS_AVAILABLE*/


