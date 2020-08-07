#ifndef  _MACHINE_LRTS_H_
#define  _MACHINE_LRTS_H_

#include "converse.h"

void LrtsPrepareEnvelope(char *msg, size_t size);

/* The machine-specific send function */
CmiCommHandle LrtsSendFunc(int destNode, int destPE, size_t size, char *msg, int mode);

void LrtsSyncListSendFn(int npes, const int *pes, size_t len, char *msg);
CmiCommHandle LrtsAsyncListSendFn(int npes, const int *pes, size_t len, char *msg);
void LrtsFreeListSendFn(int npes, const int *pes, size_t len, char *msg);

#if CMK_PERSISTENT_COMM
void LrtsSendPersistentMsg(PersistentHandle h, int destPE, int size, void *m); //TODO
#endif

/* ### Beginning of Machine-startup Related Functions ### */
void LrtsInit(int *argc, char ***argv, int *numNodes, int *myNodeID);

void LrtsPreCommonInit(int everReturn);
void LrtsPostCommonInit(int everReturn);
/* ### End of Machine-startup Related Functions ### */

/* ### Beginning of Machine-running Related Functions ### */
void LrtsAdvanceCommunication(int whileidle);
void LrtsDrainResources(void); /* used when exit */
void LrtsExit(int exitcode=0);
CMK_NORETURN void LrtsAbort(const char *message);
/* ### End of Machine-running Related Functions ### */
void LrtsPostNonLocal(void);

void* LrtsAlloc(int, int);
void* LrtsRdmaAlloc(int, int);

void  LrtsFree(void*);
void  LrtsRdmaFree(void*);
void  LrtsNotifyIdle(void);

void  LrtsBeginIdle(void);
void  LrtsStillIdle(void);
void  LrtsBarrier(void);

/* ### lock functions ### */
#include "lrtslock.h"

#endif
