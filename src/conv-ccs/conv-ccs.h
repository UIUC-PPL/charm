/*This file describes the CCS Server-side handler
interface.  A CCS handler is just a CMI handler,
but it can use the CcsSendReply function.
*/

#ifndef CONV_CCS_H
#define CONV_CCS_H

#include "converse.h" /* for CMK_CCS_AVAILABLE and CmiHandler */
#include "sockRoutines.h"
#include "ccs-server.h" /*for CcsSecAttr*/

#include "ckhashtable.h"

#ifdef __cplusplus
extern "C" {
#endif

/******* Converse Client Server *****/

#define CMK_CCS_VERSION "2"

extern int _ccsHandlerIdx;

#if CMK_CCS_AVAILABLE

typedef CkHashtable_c CcsHandlerTable;
CpvExtern(CcsHandlerTable, ccsTab);

typedef struct CcsDelayedReply_struct {
  CcsImplHeader *hdr;
} CcsDelayedReply;

typedef void (*CcsHandlerFn)(void *userPtr,int reqLen,const void *reqData);

/* Includes all information stored about a single CCS handler. */
typedef struct CcsHandlerRec {
    const char *name; /*Name passed over socket*/
    CmiHandler fnOld; /*Old converse-style handler, or NULL if new-style*/
    CcsHandlerFn fn; /*New-style handler function, or NULL if old-style*/
    void *userPtr;
    CmiReduceMergeFn mergeFn; /*Merge function used for bcast requests*/
    int nCalls; /* Number of times handler has been executed*/
    CmiUInt2 redID; /*Reduction ID to be used with CmiListReduce*/
} CcsHandlerRec;

/**
 * Backward compatability routine: register a regular converse-style handler
 * to receive CCS requests.  The requests will arrive as a Converse message,
 * with a (useless) converse header.
 */
void CcsRegisterHandler(const char *ccs_handlername, CmiHandler fn);

CcsHandlerRec *CcsGetHandler(const char *name);

/**
 * Register a real Ccs handler function to receive these CCS requests. 
 * The requests will arrive as a flat, readonly buffer.
 */
void CcsRegisterHandlerFn(const char *ccs_handlername, CcsHandlerFn fn, void *userPtr);

/**
 * Set the merging function for this CCS handler to newMerge.
 */
void CcsSetMergeFn(const char *name, CmiReduceMergeFn newMerge);
/* A few standard functions for merging CCS messages */
#define SIMPLE_REDUCTION(name) void * CcsMerge_##name(size_t *size,void *local,void **remote,int n)
#define SIMPLE_POLYMORPH_REDUCTION(nameBase) \
  SIMPLE_REDUCTION(nameBase##_int); \
  SIMPLE_REDUCTION(nameBase##_float); \
  SIMPLE_REDUCTION(nameBase##_double)
SIMPLE_REDUCTION(concat);
SIMPLE_REDUCTION(logical_and);
SIMPLE_REDUCTION(logical_or);
SIMPLE_REDUCTION(bitvec_and);
SIMPLE_REDUCTION(bitvec_and);
SIMPLE_POLYMORPH_REDUCTION(sum);
SIMPLE_POLYMORPH_REDUCTION(product);
SIMPLE_POLYMORPH_REDUCTION(max);
SIMPLE_POLYMORPH_REDUCTION(min);
#undef SIMPLE_REDUCTION
#undef SIMPLE_POLYMORPH_REDUCTION

void CcsReleaseMessages();
void CcsInit(char **argv);
int CcsEnabled(void);
int CcsIsRemoteRequest(void);
void CcsCallerId(skt_ip_t *pip, unsigned int *pport);
void CcsSendReply(int replyLen, const void *replyData);
void CcsSendReplyNoError(int replyLen, const void *replyData);
CcsDelayedReply CcsDelayReply(void);
void CcsSendDelayedReply(CcsDelayedReply d,int replyLen, const void *replyData);
void CcsNoReply();
void CcsNoDelayedReply(CcsDelayedReply d);

#else
typedef void *CcsDelayedReply;
#define CcsReleaseMessages() /*empty*/
#define CcsInit(argv) /*empty*/
#define CcsRegisterHandler(x,y) 0
#define CcsRegisterHandlerFn(x,y,p) 0
#define CcsSetMergeFn(x,y) 0
#define CcsEnabled() 0
#define CcsIsRemoteRequest() 0
#define CcsCallerId(x,y)  /*empty*/
#define CcsDelayReply() 0
#define CcsSendReply(s,r) /*empty*/
#define CcsSendReplyNoError(s,r) /*empty*/
#define CcsSendDelayedReply(d,s,r); 
#define CcsNoReply() /*empty*/
#define CcsNoDelayedReply(d) /*empty*/
#endif

#ifdef __cplusplus
}
#endif
#endif
