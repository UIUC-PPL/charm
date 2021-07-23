/*Contains declarations used by memory-isomalloc.C to provide
migratable heap allocation to arbitrary clients.
*/
#ifndef CMK_MEMORY_ISOMALLOC_H
#define CMK_MEMORY_ISOMALLOC_H

#include <stddef.h>
#include "conv-config.h"

#ifdef __cplusplus
#include <vector>
#include <tuple>

extern "C" {
#endif

/****** Isomalloc: Migratable Memory Allocation ********/
int CmiIsomallocEnabled(void);

int CmiIsomallocInRange(void * addr);

typedef struct CmiIsomallocContext {
  void * opaque;
} CmiIsomallocContext;

typedef struct CmiIsomallocRegion {
  void * start, * end;
} CmiIsomallocRegion;

/*Build/pup/destroy a context.*/
/* TODO: Some kind of registration scheme so multiple users can coexist.
 * No use case for this currently exists. */
CmiIsomallocContext CmiIsomallocContextCreate(int myunit, int numunits);
void CmiIsomallocContextDelete(CmiIsomallocContext ctx);
void CmiIsomallocContextPup(pup_er p, CmiIsomallocContext * ctxptr);
void CmiIsomallocContextEnableRandomAccess(CmiIsomallocContext ctx);
void CmiIsomallocContextJustMigrated(CmiIsomallocContext ctx);
void CmiIsomallocEnableRDMA(CmiIsomallocContext ctx, int enable); /* on by default */
CmiIsomallocRegion CmiIsomallocContextGetUsedExtent(CmiIsomallocContext ctx);

/*Allocate/free from this context*/
void * CmiIsomallocContextMalloc(CmiIsomallocContext ctx, size_t size);
void * CmiIsomallocContextMallocAlign(CmiIsomallocContext ctx, size_t align, size_t size);
void * CmiIsomallocContextCalloc(CmiIsomallocContext ctx, size_t nelem, size_t size);
void * CmiIsomallocContextRealloc(CmiIsomallocContext ctx, void * ptr, size_t size);
void CmiIsomallocContextFree(CmiIsomallocContext ctx, void * ptr);
size_t CmiIsomallocContextGetLength(CmiIsomallocContext ctx, void * ptr);
void CmiIsomallocContextProtect(CmiIsomallocContext ctx, void * addr, size_t len, int prot);

void * CmiIsomallocContextPermanentAlloc(CmiIsomallocContext ctx, size_t size);
void * CmiIsomallocContextPermanentAllocAlign(CmiIsomallocContext ctx, size_t align, size_t size);

CmiIsomallocContext CmiIsomallocGetThreadContext(CthThread th);

void CmiIsomallocContextEnableRecording(CmiIsomallocContext ctx, int enable); /* internal use only */
#ifdef __cplusplus
void CmiIsomallocGetRecordedHeap(CmiIsomallocContext ctx,
  std::vector<std::tuple<uintptr_t, size_t, size_t>> & heap_vector);
#endif

/****** Converse Thread functionality that depends on Isomalloc ********/

int CthMigratable(void);
CthThread CthPup(pup_er, CthThread);
CthThread CthCreateMigratable(CthVoidFn fn, void * arg, int size, CmiIsomallocContext ctx);

/****** Memory-Isomalloc: malloc wrappers for Isomalloc ********/

/*Allocate non-migratable memory*/
void * malloc_nomigrate(size_t size);
void free_nomigrate(void *mem);

/*Make this context active for malloc interception.*/
void CmiMemoryIsomallocContextActivate(CmiIsomallocContext ctx);

/* Only for internal runtime use, not for Isomalloc users. */
void CmiMemoryIsomallocDisablePush(void);
void CmiMemoryIsomallocDisablePop(void);

#ifdef __cplusplus
}
#endif

#endif

