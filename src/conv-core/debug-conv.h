/*
 Interface to Converse portion of parallel debugger.
 Moved here from converse.h 4/10/2001 by Orion Sky Lawlor, olawlor@acm.org
 */
#ifndef __CMK_DEBUG_CONV_H
#define __CMK_DEBUG_CONV_H

#include "pup_c.h"

/*If you are incrementing these numbers, you also need to increment MAJOR and MINOR
variables in ParDebug.java(in ccs_tools) to match, otherwise CharmDebug won't work*/
#define CHARMDEBUG_MAJOR   10
#define CHARMDEBUG_MINOR   11

#ifdef __cplusplus
extern "C" {
#endif

extern void * (*CpdDebugGetAllocationTree)(int*);
extern void (*CpdDebug_pupAllocationPoint)(pup_er p, void *data);
extern void (*CpdDebug_deleteAllocationPoint)(void *ptr);
extern void * (*CpdDebug_MergeAllocationTree)(size_t *size, void *data, void **remoteData, int numRemote);

extern void * (*CpdDebugGetMemStat)(void);
extern void (*CpdDebug_pupMemStat)(pup_er p, void *data);
extern void (*CpdDebug_deleteMemStat)(void *ptr);
extern void * (*CpdDebug_mergeMemStat)(size_t *size, void *data, void **remoteData, int numRemote);

extern int cmiArgDebugFlag; // Value is 0, unless reset in ConverseCommonInit
extern char ** memoryBackup;
extern void CpdCheckMemory(void);
extern void CpdResetMemory(void);

void CpdInit(void);
void CpdFreeze(void);
void CpdUnFreeze(void);
int  CpdIsFrozen(void);
void CpdFreezeModeScheduler(void);
void CpdStartGdb(void);
void Cpd_CmiHandleMessage(void *msg);
void CpdAborting(const char *message);

extern int (*CpdIsDebugMessage)(void*);
extern void * (*CpdGetNextMessage)(CsdSchedulerState_t*);
extern int _conditionalDelivery;
extern int conditionalPipe[2];

enum {
  CPD_ERROR = 0,
  CPD_SIGNAL = 1,
  CPD_ABORT = 2,
  CPD_FREEZE = 3,
  CPD_BREAKPOINT = 4,
  CPD_CROSSCORRUPTION = 5
};
extern void CpdNotify(int type, ...);

typedef struct LeakSearchInfo {
  char *begin_data, *end_data;
  char *begin_bss, *end_bss;
  int quick;
  int pe;
} LeakSearchInfo;
extern void CpdSearchLeaks(char*);

/* C bindings for CpdList functions: */

/**
  When a CCS client asks for some data in a CpdList, the
  system generates this struct to describe the range of
  items the client asked for (the items are numbered lo to hi-1),
  as well as store any extra data the CCS client passed in.
*/
typedef struct {
	int lo,hi; /**< Range of requested items in list is (lo .. hi-1)*/
	int extraLen; /**< Amount of data pointed to below*/
	void *extra; /**< List-defined request data shipped in via CCS */
} CpdListItemsRequest;

/**
 Call this routine at the start of each CpdList item.
 This lets the client distinguish one item from the next.
*/
void CpdListBeginItem(pup_er p,int itemNo);

/**
 User-written C routine to pup a range of items in a CpdList.
    \param itemsParam User-defined parameter passed to CpdListRegister_c.
    \param p pup_er to pup items to.
    \param req Cpd request object, describing items to pup.
*/
typedef void (*CpdListItemsFn_c)(void *itemsParam,pup_er p,
				CpdListItemsRequest *req);

/**
  User-written C routine to return the length (number of items)
  in this CpdList.
    \param lenParam User-defined parameter passed to CpdListRegister_c.
    \param return Length of the CpdList.
*/
typedef size_t  (*CpdListLengthFn_c)(void *lenParam);

/**
  Create a new CpdList at the given path.  When a CCS client requests
  this CpdList, Cpd will use these user-written C routines to extract
  the list's length and items.
    \param path CpdList request path.  The CCS client passes in this path.
    \param lenFn User-written subroutine to calculate the list's current length.
    \param lenParam User-defined parameter passed to lenFn.
    \param itemsFn User-written subroutine to pup the list's items.
    \param itemsParam User-defined parameter passed to itemsFn.
*/
void CpdListRegister_c(const char *path,
	    CpdListLengthFn_c lenFn,void *lenParam,
	    CpdListItemsFn_c itemsFn,void *itemsParam,
	    int checkBoundary);

#ifdef __cplusplus
}
#endif

#endif
