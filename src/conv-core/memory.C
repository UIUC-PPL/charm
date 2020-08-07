

/**  
 @defgroup MemoryModule Memory Allocation and Monitoring

 This module provides the functions malloc, free, calloc, cfree,
 realloc, valloc, memalign, and other functions for determining
 the current and past memory usage. 

 There are several possible implementations provided here-- the user
 can link in whichever is best using the -memory option with charmc.

 The major possibilities here are empty (use the system's malloc),
 GNU (use memory-gnu.C), and meta (use e.g. memory-paranoid.C).
 On machines without sbrk(), only the system's malloc is available.

 The CMK_MEMORY_BUILD_* symbols come in from the compiler command line.

 To determine how much memory your Charm++ program is using on a 
 processor, call CmiMemoryUsage(). This function will return the 
 number of bytes allocated, usually representing the heap size. 
 It is possible that this measurement will not exactly represent
 the heap size, but rather will reflect the total amount of 
 memory used by the program.

*/


/** 
    @addtogroup MemoryModule
    @{
*/


/******************************************************************************
 *
 * This module provides the functions malloc, free, calloc, cfree,
 * realloc, valloc, and memalign.
 *
 *
 *****************************************************************************/

/* These macros are needed for:
 * sys/resource.h: rusage, getrusage
 */
#ifndef _GNU_SOURCE
# define _GNU_SOURCE
#endif
#ifndef __USE_GNU
# define __USE_GNU
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h> /*For memset, memcpy*/
#include <type_traits>
#include <new>

#ifndef __STDC_FORMAT_MACROS
# define __STDC_FORMAT_MACROS
#endif
#ifndef __STDC_LIMIT_MACROS
# define __STDC_LIMIT_MACROS
#endif
#include <inttypes.h>
#ifndef _WIN32
#  include <unistd.h> /*For getpagesize*/
#else
#  include <process.h>
#  define getpid _getpid
#endif
#include "converse.h"
#include "charm-api.h"

/* Wrap a CmiMemLock around this code */
#define MEM_LOCK_AROUND(code) \
  CmiMemLock(); \
  code; \
  CmiMemUnlock();

void * memory_stack_top; /*The higher end of the stack (approximation)*/
int cpdInSystem=1; /*Start inside the system (until we start executing user code)*/

/*Choose the proper default configuration*/
#if CMK_MEMORY_BUILD_DEFAULT

# if CMK_MALLOC_USE_OS_BUILTIN
/*Default to the system malloc-- perhaps the only possibility*/
#  define CMK_MEMORY_BUILD_OS 1

# else
/*Choose a good all-around default malloc*/
#  define CMK_MEMORY_BUILD_GNU 1
# endif

#endif

#if CMK_MEMORY_BUILD_OS_WRAPPED
#define CMK_MEMORY_BUILD_OS 1
#endif
#if CMK_MEMORY_BUILD_GNU_HOOKS
/*While in general on could build hooks on the GNU we have, for now the hooks
 * are setup to work only with OS memory libraries --Filippo*/
#define CMK_MEMORY_BUILD_OS 1
#endif

#if CMK_MEMORY_BUILD_OS
#if CMK_MEMORY_BUILD_OS_WRAPPED

struct mallinfo;

void initialize_memory_wrapper(void);
void * initialize_memory_wrapper_calloc(size_t nelem, size_t size);
void * initialize_memory_wrapper_malloc(size_t size);
void * initialize_memory_wrapper_realloc(void *ptr, size_t size);
void * initialize_memory_wrapper_memalign(size_t align, size_t size);
int initialize_memory_wrapper_posix_memalign(void **memptr, size_t align, size_t size);
void * initialize_memory_wrapper_aligned_alloc(size_t align, size_t size);
void * initialize_memory_wrapper_valloc(size_t size);
void * initialize_memory_wrapper_pvalloc(size_t size);
void initialize_memory_wrapper_free(void *ptr);
void initialize_memory_wrapper_cfree(void *ptr);

void * (*mm_impl_malloc)(size_t) = initialize_memory_wrapper_malloc;
void * (*mm_impl_calloc)(size_t,size_t) = initialize_memory_wrapper_calloc;
void * (*mm_impl_realloc)(void*,size_t) = initialize_memory_wrapper_realloc;
void * (*mm_impl_memalign)(size_t,size_t) = initialize_memory_wrapper_memalign;
int (*mm_impl_posix_memalign)(void **,size_t,size_t) = initialize_memory_wrapper_posix_memalign;
void * (*mm_impl_aligned_alloc)(size_t,size_t) = initialize_memory_wrapper_aligned_alloc;
void * (*mm_impl_valloc)(size_t) = initialize_memory_wrapper_valloc;
void * (*mm_impl_pvalloc)(size_t) = initialize_memory_wrapper_pvalloc;
void (*mm_impl_free)(void*) = initialize_memory_wrapper_free;
void (*mm_impl_cfree)(void*) = initialize_memory_wrapper_cfree;
struct mallinfo (*mm_impl_mallinfo)(void) = NULL;

static char fake_malloc_buffer[1024];
static char* fake_malloc_buffer_pos = fake_malloc_buffer;

static void* fake_malloc(size_t size)
{
  void *ptr = fake_malloc_buffer_pos;
  fake_malloc_buffer_pos += size;
  if (fake_malloc_buffer_pos > fake_malloc_buffer + sizeof(fake_malloc_buffer))
  {
    static char have_warned = 0; // in case malloc is called inside (f)printf
    if (!have_warned)
    {
      have_warned = 1;
      CmiPrintf("Error: fake_malloc has run out of space (%u / %u)\n",
                (unsigned int) (fake_malloc_buffer_pos - fake_malloc_buffer),
                (unsigned int) sizeof(fake_malloc_buffer));
    }
    exit(1);
  }
  return ptr;
}
static void* fake_calloc(size_t nelem, size_t size)
{
  const size_t total = nelem * size;
  void *ptr = fake_malloc(total);
  memset(ptr, 0, total);
  return ptr;
}
#if 0
static void fake_free(void* ptr)
{
}
#endif

extern char initialize_memory_wrapper_status;

void * initialize_memory_wrapper_calloc(size_t nelem, size_t size) {
  if (initialize_memory_wrapper_status)
    return fake_calloc(nelem, size);
  initialize_memory_wrapper();
  return (*mm_impl_calloc)(nelem,size);
}

void * initialize_memory_wrapper_malloc(size_t size) {
  if (initialize_memory_wrapper_status)
    return fake_malloc(size);
  initialize_memory_wrapper();
  return (*mm_impl_malloc)(size);
}

void * initialize_memory_wrapper_realloc(void *ptr, size_t size) {
  initialize_memory_wrapper();
  return (*mm_impl_realloc)(ptr,size);
}

void * initialize_memory_wrapper_memalign(size_t align, size_t size) {
  initialize_memory_wrapper();
  return (*mm_impl_memalign)(align,size);
}

int initialize_memory_wrapper_posix_memalign(void **memptr, size_t align, size_t size) {
  initialize_memory_wrapper();
  return (*mm_impl_posix_memalign)(memptr,align,size);
}

void * initialize_memory_wrapper_aligned_alloc(size_t align, size_t size) {
  initialize_memory_wrapper();
  return (*mm_impl_aligned_alloc)(align,size);
}

void * initialize_memory_wrapper_valloc(size_t size) {
  initialize_memory_wrapper();
  return (*mm_impl_valloc)(size);
}

void * initialize_memory_wrapper_pvalloc(size_t size) {
  initialize_memory_wrapper();
  return (*mm_impl_pvalloc)(size);
}

void initialize_memory_wrapper_free(void *ptr) {
  if (initialize_memory_wrapper_status)
    return;
  initialize_memory_wrapper();
  (*mm_impl_free)(ptr);
}

void initialize_memory_wrapper_cfree(void *ptr) {
  initialize_memory_wrapper();
  (*mm_impl_cfree)(ptr);
}

#define mm_impl_malloc   (*mm_impl_malloc)
#define mm_impl_free     (*mm_impl_free)
#define mm_impl_calloc   (*mm_impl_calloc)
#define mm_impl_cfree    (*mm_impl_cfree)
#define mm_impl_realloc  (*mm_impl_realloc)
#define mm_impl_memalign (*mm_impl_memalign)
#define mm_impl_posix_memalign (*mm_impl_posix_memalign)
#define mm_impl_aligned_alloc (*mm_impl_aligned_alloc)
#define mm_impl_valloc   (*mm_impl_valloc)
#define mm_impl_pvalloc  (*mm_impl_pvalloc)
#endif /* CMK_MEMORY_BUILD_OS_WRAPPED */
#endif /* CMK_MEMORY_BUILD_OS */

CMK_TYPEDEF_UINT8 _memory_allocated = 0;
CMK_TYPEDEF_UINT8 _memory_allocated_max = 0; /* High-Water Mark */
CMK_TYPEDEF_UINT8 _memory_allocated_min = 0; /* Low-Water Mark */

/* By default, there are no flags */
static int CmiMemoryIs_flag=0;

int CmiMemoryIs(int flag)
{
	return (CmiMemoryIs_flag&flag)==flag;
}

/**
 * memory_lifeRaft is a very small heap-allocated region.
 * The lifeRaft is supposed to be just big enough to provide
 * enough memory to cleanly shut down if we run out of memory.
 */
static char *memory_lifeRaft=NULL;

void CmiOutOfMemoryInit(void);

void CmiOutOfMemory(size_t nBytes)
{ /* We're out of memory: free up the liferaft memory and abort */
  if (memory_lifeRaft) free(memory_lifeRaft);
  if (nBytes>0) CmiAbort("Could not malloc() %zu bytes--are we out of memory? (used :%.3fMB)",nBytes,CmiMemoryUsage()/1000000.0);
  else CmiAbort("Could not malloc()--are we out of memory? (used: %.3fMB)", CmiMemoryUsage()/1000000.0);
}

/* Global variables keeping track of the status of the system (mostly used by charmdebug) */
int memory_status_info=0;
int memory_chare_id=0;

#if CMK_MEMORY_BUILD_OS
/* Just use the OS's built-in malloc.  All we provide is CmiMemoryInit.
*/

// The OS allocator doesn't need locking,
// so point the meta-meta calls directly to their implementations.
#define mm_malloc   mm_impl_malloc
#define mm_free     mm_impl_free
#define mm_calloc   mm_impl_calloc
#define mm_cfree    mm_impl_cfree
#define mm_realloc  mm_impl_realloc
#define mm_memalign mm_impl_memalign
#define mm_posix_memalign mm_impl_posix_memalign
#define mm_aligned_alloc mm_impl_aligned_alloc
#define mm_valloc   mm_impl_valloc
#define mm_pvalloc  mm_impl_pvalloc

#if CMK_MEMORY_BUILD_OS_WRAPPED || CMK_MEMORY_BUILD_GNU_HOOKS

#if CMK_MEMORY_BUILD_GNU_HOOKS

static void *meta_malloc(size_t);
static void *meta_realloc(void*,size_t);
static void *meta_memalign(size_t,size_t);
static void meta_free(void*);
static void *meta_malloc_hook(size_t s, const void* c) {return meta_malloc(s);}
static void *meta_realloc_hook(void* p,size_t s, const void* c) {return meta_realloc(p,s);}
static void *meta_memalign_hook(size_t s1,size_t s2, const void* c) {return meta_memalign(s1,s2);}
static void meta_free_hook(void* p, const void* c) {meta_free(p);}

#if CMK_HAS_MALLOC_H
#include <malloc.h>
#endif
static void *(*old_malloc_hook) (size_t, const void*);
static void *(*old_realloc_hook) (void*,size_t, const void*);
static void *(*old_memalign_hook) (size_t,size_t, const void*);
static void (*old_free_hook) (void*, const void*);

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
#ifdef __INTEL_COMPILER
#pragma warning push
#pragma warning disable 1478
#endif

#define BEFORE_MALLOC_CALL \
  __malloc_hook = old_malloc_hook; \
  __realloc_hook = old_realloc_hook; \
  __memalign_hook = old_memalign_hook; \
  __free_hook = old_free_hook
#define AFTER_MALLOC_CALL \
  old_malloc_hook = __malloc_hook; \
  old_realloc_hook = __realloc_hook; \
  old_memalign_hook = __memalign_hook; \
  old_free_hook = __free_hook; \
  __malloc_hook = meta_malloc_hook; \
  __realloc_hook = meta_realloc_hook; \
  __memalign_hook = meta_memalign_hook; \
  __free_hook = meta_free_hook

static void my_init_hook()
{
  AFTER_MALLOC_CALL;
}
/* Override initializing hook from the C library. */
#if defined(__MALLOC_HOOK_VOLATILE)
void (* __MALLOC_HOOK_VOLATILE __malloc_initialize_hook) (void) = my_init_hook;
#else
void (* __malloc_initialize_hook) (void) = my_init_hook;
#endif

static inline void *mm_impl_malloc(size_t size)
{
  void *result;
  BEFORE_MALLOC_CALL;
  result = malloc(size);
  AFTER_MALLOC_CALL;
  return result;
}
static inline void mm_impl_free(void *mem)
{
  BEFORE_MALLOC_CALL;
  free(mem);
  AFTER_MALLOC_CALL;
}
static inline void *mm_impl_calloc(size_t nelem, size_t size)
{
  void *result;
  BEFORE_MALLOC_CALL;
  result = calloc(nelem, size);
  AFTER_MALLOC_CALL;
  return result;
}
#if 0
static inline void mm_impl_cfree(void *mem)
{
  BEFORE_MALLOC_CALL;
  cfree(mem);
  AFTER_MALLOC_CALL;
}
#endif
static inline void *mm_impl_realloc(void *mem, size_t size)
{
  void *result;
  BEFORE_MALLOC_CALL;
  result = realloc(mem, size);
  AFTER_MALLOC_CALL;
  return result;
}
static inline void *mm_impl_memalign(size_t align, size_t size)
{
  void *result;
  BEFORE_MALLOC_CALL;
  result = memalign(align, size);
  AFTER_MALLOC_CALL;
  return result;
}
static inline int mm_impl_posix_memalign(void **outptr, size_t align, size_t size)
{
  int result;
  BEFORE_MALLOC_CALL;
  result = posix_memalign(outptr, align, size);
  AFTER_MALLOC_CALL;
  return result;
}
static inline void *mm_impl_aligned_alloc(size_t align, size_t size)
{
  void *result;
  BEFORE_MALLOC_CALL;
#if (defined __cplusplus && __cplusplus >= 201703L) || (defined __STDC_VERSION__ && __STDC_VERSION__ >= 201112L)
  result = aligned_alloc(align, size);
#else
  result = memalign(align, size);
#endif
  AFTER_MALLOC_CALL;
  return result;
}
#if 0
static inline void *mm_impl_valloc(size_t size)
{
  void *result;
  BEFORE_MALLOC_CALL;
  result = valloc(size);
  AFTER_MALLOC_CALL;
  return result;
}
static inline void *mm_impl_pvalloc(size_t size)
{
  void *result;
  BEFORE_MALLOC_CALL;
  result = pvalloc(size);
  AFTER_MALLOC_CALL;
  return result;
}
#endif

#undef BEFORE_MALLOC_CALL
#undef AFTER_MALLOC_CALL

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
#ifdef __INTEL_COMPILER
#pragma warning pop
#endif

#endif /* CMK_MEMORY_BUILD_GNU_HOOKS */

// Pick which Converse memory layer we want:

#if CMK_MEMORY_BUILD_VERBOSE
#include "memory-verbose.C"
#endif

#if CMK_MEMORY_BUILD_RECORD
#include "memory-record.C"
#endif

#if CMK_MEMORY_BUILD_PARANOID
#include "memory-paranoid.C"
#endif

#if CMK_MEMORY_BUILD_LEAK
#include "memory-leak.C"
#endif

#if CMK_MEMORY_BUILD_ISOMALLOC
#include "memory-isomalloc.C"
#endif

#if CMK_MEMORY_BUILD_LOCK
#include "memory-lock.C"
#endif

#if CMK_MEMORY_BUILD_CHARMDEBUG
#include "memory-charmdebug.C"
#endif

#if ! CMK_MEMORY_BUILD_GNU_HOOKS
// Define our own symbols replacing malloc et al.
void *malloc(size_t size) CMK_THROW { return meta_malloc(size); }
void free(void *ptr) CMK_THROW { meta_free(ptr); }
void *calloc(size_t nelem, size_t size) CMK_THROW { return meta_calloc(nelem,size); }
void cfree(void *ptr) CMK_THROW { meta_cfree(ptr); }
void *realloc(void *ptr, size_t size) CMK_THROW { return meta_realloc(ptr,size); }
CLINKAGE void *memalign(size_t align, size_t size) CMK_THROW { return meta_memalign(align,size); }
CLINKAGE int posix_memalign(void **outptr, size_t align, size_t size) CMK_THROW { return meta_posix_memalign(outptr,align,size); }
CLINKAGE void *aligned_alloc(size_t align, size_t size) CMK_THROW { return meta_aligned_alloc(align,size); }
void *valloc(size_t size) CMK_THROW { return meta_valloc(size); }
CLINKAGE void *pvalloc(size_t size) CMK_THROW { return meta_pvalloc(size); }
#endif /* ! CMK_MEMORY_BUILD_GNU_HOOKS */

#else

#define mm_impl_malloc   malloc
#define mm_impl_free     free
#define mm_impl_calloc   calloc
#define mm_impl_cfree    cfree
#define mm_impl_memalign memalign
#define mm_impl_posix_memalign posix_memalign
#if (defined __cplusplus && __cplusplus >= 201703L) || (defined __STDC_VERSION__ && __STDC_VERSION__ >= 201112L)
#define mm_impl_aligned_alloc aligned_alloc
#else
#define mm_impl_aligned_alloc memalign
#endif
#define mm_impl_valloc   valloc
#define mm_impl_pvalloc  pvalloc

#endif /* CMK_MEMORY_BUILD_OS_WRAPPED || CMK_MEMORY_BUILD_GNU_HOOKS */

static int skip_mallinfo = 0;

void CmiMemoryInit(char ** argv)
{
  if(CmiMyRank() == 0)   CmiMemoryIs_flag |= CMI_MEMORY_IS_OS;
#if CMK_MEMORY_BUILD_OS_WRAPPED || CMK_MEMORY_BUILD_GNU_HOOKS
  CmiArgGroup("Converse","Memory module");
  meta_init(argv);
  CmiNodeAllBarrier();
#endif
  CmiOutOfMemoryInit();
  if (getenv("MEMORYUSAGE_NO_MALLINFO"))  skip_mallinfo = 1;
}

/******Start of a general way to get memory usage information*****/
/*CMK_TYPEDEF_UINT8 CmiMemoryUsage() { return 0; }*/

#if ! CMK_HAS_SBRK
namespace {
  int sbrk(int s) { return 0; }
}
#endif

#if CMK_C_INLINE
#define INLINE inline
#else
#define INLINE
#endif


#if CMK_HAS_MSTATS
#include <malloc/malloc.h>
INLINE static CMK_TYPEDEF_UINT8 MemusageMstats(void){
	struct mstats ms = mstats();
	CMK_TYPEDEF_UINT8 memtotal = ms.bytes_used;
	return memtotal;
}
#else
INLINE static CMK_TYPEDEF_UINT8 MemusageMstats(void) { return 0; }
#endif

static int MemusageInited = 0;
static uintptr_t MemusageInitSbrkval = 0;
INLINE static CMK_TYPEDEF_UINT8 MemusageSbrk(void){
	uintptr_t newval;
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
	if(MemusageInited==0){
		MemusageInitSbrkval = (uintptr_t)sbrk(0);
		MemusageInited = 1;
	}
	newval = (uintptr_t)sbrk(0);
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
	return (newval - MemusageInitSbrkval);
}

INLINE static CMK_TYPEDEF_UINT8 MemusageProcSelfStat(void){
    FILE *f;
    int i, ret;
    static int failed_once = 0;
    CMK_TYPEDEF_UINT8 vsz = 0; /* should remain 0 on failure */

    if(failed_once) return 0; /* no point in retrying */
    
    f = fopen("/proc/self/stat", "r");
    if(!f) { failed_once = 1; return 0; }
    for(i=0; i<22; i++) ret = fscanf(f, "%*s");
    ret = fscanf(f, "%" PRIu64, &vsz);
    fclose(f);
    if(!vsz) failed_once=1;
    return vsz;
}

#if ! CMK_HAS_MALLINFO || defined(CMK_MALLINFO_IS_BROKEN)
INLINE static CMK_TYPEDEF_UINT8 MemusageMallinfo(void){ return 0;}
#else
#if CMK_HAS_MALLOC_H
#include <malloc.h>
#endif
INLINE static CMK_TYPEDEF_UINT8 MemusageMallinfo(void){
    /* IA64 seems to ignore mi.uordblks, but updates mi.hblkhd correctly */
    if (skip_mallinfo) return 0;
    else {
    struct mallinfo mi;
#if CMK_MEMORY_BUILD_OS_WRAPPED && !CMK_MEMORY_BUILD_GNU_HOOKS
    if (mm_impl_mallinfo == NULL)
      initialize_memory_wrapper();
    mi = (*mm_impl_mallinfo)();
#else
    mi = mallinfo();
#endif
    CMK_TYPEDEF_UINT8 memtotal = (CMK_TYPEDEF_UINT8) mi.uordblks;   /* malloc */
    CMK_TYPEDEF_UINT8 memtotal2 = (CMK_TYPEDEF_UINT8) mi.usmblks;   /* unused */
    memtotal2 += (CMK_TYPEDEF_UINT8) mi.hblkhd;               /* mmap */
    /* printf("%lld %lld %lld %lld %lld\n", mi.uordblks, mi.usmblks,mi.hblkhd,mi.arena,mi.keepcost); */
#if !CMK_CRAYXE && !CMK_CRAYXC
    if(memtotal2 > memtotal) memtotal = memtotal2;
#endif
    return memtotal;
    }
}
#endif

INLINE static CMK_TYPEDEF_UINT8 MemusagePS(void){
#if ! CMK_HAS_POPEN
    return 0;
#else	
    char pscmd[100];
    CMK_TYPEDEF_UINT8 vsz=0;
    FILE *p;
    int ret;
    sprintf(pscmd, "/bin/ps -o vsz= -p %d", getpid());
    p = popen(pscmd, "r");
    if(p){
	ret = fscanf(p, "%" PRIu64, &vsz);
	pclose(p);
    }
    return (vsz * (CMK_TYPEDEF_UINT8)1024);
#endif	
}

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>

INLINE static CMK_TYPEDEF_UINT8 MemusageWindows(void){
    PROCESS_MEMORY_COUNTERS pmc;
    if ( GetProcessMemoryInfo( GetCurrentProcess(), &pmc, sizeof(pmc)) )
    {
      /* return pmc.WorkingSetSize; */ 
      return pmc.PagefileUsage;    /* total vm size, possibly not in memory */
    }
    return 0;
}
#else
static CMK_TYPEDEF_UINT8 MemusageWindows(void){
    return 0;
}
#endif

#if CMK_BLUEGENEQ
#include <spi/include/kernel/memory.h>
INLINE static CMK_TYPEDEF_UINT8 MemusageBGQ(void){
  uint64_t heapUsed;
  Kernel_GetMemorySize(KERNEL_MEMSIZE_HEAP, &heapUsed);
  return (CMK_TYPEDEF_UINT8)heapUsed;
}
#else
static CMK_TYPEDEF_UINT8 MemusageBGQ(void){
    return 0;
}
#endif

typedef CMK_TYPEDEF_UINT8 (*CmiMemUsageFn)(void);

/* this structure defines the order of testing for memory usage functions */
struct CmiMemUsageStruct {
    CmiMemUsageFn  fn;
    const char *name;
} memtest_order[] = {
    {MemusageBGQ, "BlueGene/Q"},
    {MemusageWindows, "Windows"},
    {MemusageMstats, "Mstats"},
    {MemusageMallinfo, "Mallinfo"},
    {MemusageProcSelfStat, "/proc/self/stat"},
    {MemusageSbrk, "sbrk"},
    {MemusagePS, "ps"},
};

CMK_TYPEDEF_UINT8 CmiMemoryUsage(void){
    int i;
    CMK_TYPEDEF_UINT8 memtotal = 0;
    for (i=0; i<sizeof(memtest_order)/sizeof(struct CmiMemUsageStruct); i++) {
        memtotal = memtest_order[i].fn();
        if (memtotal) break;
    }
    return memtotal;
}

const char *CmiMemoryUsageReporter(void){
    int i;
    CMK_TYPEDEF_UINT8 memtotal = 0;
    const char *reporter = NULL;
    for (i=0; i<sizeof(memtest_order)/sizeof(struct CmiMemUsageStruct); i++) {
        memtotal = memtest_order[i].fn();
        reporter = memtest_order[i].name;
        if (memtotal) break;
    }
    return reporter;
}

/******End of a general way to get memory usage information*****/

#if CMK_HAS_RUSAGE_THREAD
#include <sys/resource.h>
CMK_TYPEDEF_UINT8 CmiMaxMemoryUsageR(void) {
  struct rusage usage;
  getrusage(RUSAGE_SELF, &usage);
  return usage.ru_maxrss;
}
#else
CMK_TYPEDEF_UINT8 CmiMaxMemoryUsageR(void) {
  return 0;
}
#endif

CMK_TYPEDEF_UINT8 CmiMaxMemoryUsage(void) { return 0; }
void CmiResetMaxMemory(void) {}
CMK_TYPEDEF_UINT8 CmiMinMemoryUsage(void) { return 0; }
void CmiResetMinMemory(void) {}

#else /* CMK_MEMORY_BUILD_OS */

// Use ptmalloc3 as meta-meta malloc fallbacks (mm_*)
#include "memory-gnu.C"

// ptmalloc3 needs locking around it
static inline void *mm_malloc(size_t size)
{
  void *result;
  MEM_LOCK_AROUND( result = mm_impl_malloc(size); )
  return result;
}
static inline void mm_free(void *mem)
{
  MEM_LOCK_AROUND( mm_impl_free(mem); )
}
static inline void *mm_calloc(size_t nelem, size_t size)
{
  void *result;
  MEM_LOCK_AROUND( result = mm_impl_calloc(nelem, size); )
  return result;
}
static inline void mm_cfree(void *mem)
{
  MEM_LOCK_AROUND( mm_impl_cfree(mem); )
}
static inline void *mm_realloc(void *mem, size_t size)
{
  void *result;
  MEM_LOCK_AROUND( result = mm_impl_realloc(mem, size); )
  return result;
}
static inline void *mm_memalign(size_t align, size_t size)
{
  void *result;
  MEM_LOCK_AROUND( result = mm_impl_memalign(align, size); )
  return result;
}
static inline int mm_posix_memalign(void **outptr, size_t align, size_t size)
{
  int result;
  MEM_LOCK_AROUND( result = mm_impl_posix_memalign(outptr, align, size); )
  return result;
}
static inline void *mm_aligned_alloc(size_t align, size_t size)
{
  void *result;
  MEM_LOCK_AROUND( result = mm_impl_aligned_alloc(align, size); )
  return result;
}
static inline void *mm_valloc(size_t size)
{
  void *result;
  MEM_LOCK_AROUND( result = mm_impl_valloc(size); )
  return result;
}
static inline void *mm_pvalloc(size_t size)
{
  void *result;
  MEM_LOCK_AROUND( result = mm_impl_pvalloc(size); )
  return result;
}

// Pick which Converse meta memory layer we want:

#if CMK_MEMORY_BUILD_GNU 
// We're just using ptmalloc3 without Converse doing anything in between,
// so point the meta calls directly to the meta-meta calls.
#define meta_malloc   mm_malloc
#define meta_free     mm_free
#define meta_calloc   mm_calloc
#define meta_cfree    mm_cfree
#define meta_realloc  mm_realloc
#define meta_memalign mm_memalign
#define meta_posix_memalign mm_posix_memalign
#define meta_aligned_alloc mm_aligned_alloc
#define meta_valloc   mm_valloc
#define meta_pvalloc  mm_pvalloc

static void meta_init(char **argv) {
  if (CmiMyRank()==0) CmiMemoryIs_flag |= CMI_MEMORY_IS_GNU;
}
#endif /* CMK_MEMORY_BUILD_GNU */

#if CMK_MEMORY_BUILD_VERBOSE
#include "memory-verbose.C"
#endif

#if CMK_MEMORY_BUILD_RECORD
#include "memory-record.C"
#endif

#if CMK_MEMORY_BUILD_PARANOID
#include "memory-paranoid.C"
#endif

#if CMK_MEMORY_BUILD_LEAK
#include "memory-leak.C"
#endif

#if CMK_MEMORY_BUILD_ISOMALLOC
#include "memory-isomalloc.C"
#endif

#if CMK_MEMORY_BUILD_CHARMDEBUG
#include "memory-charmdebug.C"
#endif

/*A trivial sample implementation of the meta_* calls:*/
#if 0
static void meta_init(char **argv)
{

}
static void *meta_malloc(size_t size)
{
  return mm_malloc(size);
}
static void meta_free(void *mem)
{
  mm_free(mem);
}
static void *meta_calloc(size_t nelem, size_t size)
{
  return mm_calloc(nelem,size);
}
static void meta_cfree(void *mem)
{
  mm_cfree(m);
}
static void *meta_realloc(void *mem, size_t size)
{
  return mm_realloc(mem,size);
}
static void *meta_memalign(size_t align, size_t size)
{
  return mm_memalign(align,size);
}
static int meta_posix_memalign(void **outptr, size_t align, size_t size)
{
  return mm_posix_memalign(outptr,align,size);
}
static void *meta_aligned_alloc(size_t align, size_t size)
{
  return mm_aligned_alloc(align,size);
}
static void *meta_valloc(size_t size)
{
  return mm_valloc(size);
}
static void *meta_pvalloc(size_t size)
{
  return mm_pvalloc(size);
}
#endif /* 0 */


/*******************************************************************
The locking code is common to all implementations except OS-builtin.
*/
static int CmiMemoryInited = 0;

void CmiMemoryInit(char **argv)
{
  CmiArgGroup("Converse","Memory module");
  meta_init(argv);
  CmiOutOfMemoryInit();
  if (CmiMyRank()==0) CmiMemoryInited = 1;
  CmiNodeAllBarrier();
}

// Define our own symbols replacing malloc et al.

// These could be factored into !CMK_MEMORY_BUILD_GNU_HOOKS,
// if not for the CmiOutOfMemory checks.

void *malloc(size_t size) CMK_THROW
{
  void *result;
  result = meta_malloc(size);
  if (result==NULL) CmiOutOfMemory(size);
  return result;
}

void free(void *mem) CMK_THROW
{
  meta_free(mem);
}

void *calloc(size_t nelem, size_t size) CMK_THROW
{
  void *result;
  result = meta_calloc(nelem, size);
  if (result==NULL) CmiOutOfMemory(size);
  return result;
}

void cfree(void *mem) CMK_THROW
{
  meta_cfree(mem);
}

void *realloc(void *mem, size_t size) CMK_THROW
{
  void *result;
  result = meta_realloc(mem, size);
  return result;
}

CLINKAGE void *memalign(size_t align, size_t size) CMK_THROW
{
  void *result;
  result = meta_memalign(align, size);
  if (result==NULL) CmiOutOfMemory(align*size);
  return result;
}

CLINKAGE int posix_memalign (void **outptr, size_t align, size_t size) CMK_THROW
{
  int result;
  result = meta_posix_memalign(outptr, align, size);
  if (result!=0) CmiOutOfMemory(align*size);
  return result;
}

CLINKAGE void *aligned_alloc(size_t align, size_t size) CMK_THROW
{
  void *result;
  result = meta_aligned_alloc(align, size);
  if (result==NULL) CmiOutOfMemory(align*size);
  return result;
}

void *valloc(size_t size) CMK_THROW
{
  void *result;
  result = meta_valloc(size);
  if (result==NULL) CmiOutOfMemory(size);
  return result;
}

void *pvalloc(size_t size) CMK_THROW
{
  void *result;
  result = meta_pvalloc(size);
  if (result==NULL) CmiOutOfMemory(size);
  return result;
}

/** Return number of bytes currently allocated, if possible. */
CMK_TYPEDEF_UINT8 CmiMemoryUsage(void)
{
  int i;
  struct malloc_arena* ar_ptr;

  if(__malloc_initialized < 0)
    ptmalloc_init ();

  size_t uordblks = 0;

  for (i=0, ar_ptr = &main_arena;; ++i)
  {
    struct malloc_state* msp = (struct malloc_state *)arena_to_mspace(ar_ptr);

    mstate ms = (mstate)msp;
    if (!ok_magic(ms)) {
      USAGE_ERROR_ACTION(ms,ms);
    }
    mstate m = ms;

    if (!PREACTION(m)) {
#ifdef DLMALLOC_DEBUG
      global_malloc_instance.do_check_malloc_state(m);
#endif
      if (is_initialized(m)) {
        size_t mfree = m->topsize + TOP_FOOT_SIZE;
        msegmentptr s = &m->seg;
        while (s != 0) {
          mchunkptr q = align_as_chunk(s->base);
          while (segment_holds(s, q) &&
                 q != m->top && q->head != FENCEPOST_HEAD) {
            size_t sz = chunksize(q);
            if (!cinuse(q)) {
              mfree += sz;
            }
            q = next_chunk(q);
          }
          s = s->next;
        }

        uordblks += m->footprint - mfree;
      }

      POSTACTION(m);
    }

    ar_ptr = ar_ptr->next;
    if (ar_ptr == &main_arena)
      break;
  }

  return uordblks;
}

/** Return number of maximum number of bytes allocated since the last call to CmiResetMaxMemory(), if possible. */
CMK_TYPEDEF_UINT8 CmiMaxMemoryUsage(void)
{
  int i;
  struct malloc_arena* ar_ptr;

  if(__malloc_initialized < 0)
    ptmalloc_init ();

  size_t usmblks = 0;

  for (i=0, ar_ptr = &main_arena;; ++i)
  {
    struct malloc_state* msp = (struct malloc_state *)arena_to_mspace(ar_ptr);

    mstate ms = (mstate)msp;
    if (!ok_magic(ms)) {
      USAGE_ERROR_ACTION(ms,ms);
    }
    mstate m = ms;

    if (!PREACTION(m)) {
#ifdef DLMALLOC_DEBUG
      global_malloc_instance.do_check_malloc_state(m);
#endif
      if (is_initialized(m)) {
        usmblks += m->max_footprint;
      }

      POSTACTION(m);
    }

    ar_ptr = ar_ptr->next;
    if (ar_ptr == &main_arena)
      break;
  }

  return usmblks;
}

/** Reset the mechanism that records the highest seen (high watermark) memory usage. */
void CmiResetMaxMemory(void) {
}

CMK_TYPEDEF_UINT8 CmiMinMemoryUsage(void)
{
  return 0;
}

void CmiResetMinMemory(void) {
}

#endif /* CMK_MEMORY_BUILD_OS */

#ifndef CMK_MEMORY_HAS_NOMIGRATE
/*Default implementations of the nomigrate routines:*/
CLINKAGE void *malloc_nomigrate(size_t);
CLINKAGE void free_nomigrate(void *);

void *malloc_nomigrate(size_t size) { return malloc(size); }
void free_nomigrate(void *mem) { free(mem); }
#endif

#ifndef CMK_MEMORY_HAS_ISOMALLOC
#include "memory-isomalloc.h"
void CmiMemoryIsomallocContextActivate(CmiIsomallocContext l) {}
void CmiMemoryIsomallocDisablePush() {}
void CmiMemoryIsomallocDisablePop() {}
#endif

#ifndef CMI_MEMORY_ROUTINES
void CmiMemoryMark(void) {}
void CmiMemoryMarkBlock(void *blk) {}
void CmiMemorySweep(const char *where) {}
void CmiMemoryCheck(void) {}
#endif

void memory_preallocate_hack(void)
{
#if CMK_MEMORY_PREALLOCATE_HACK
  /* Work around problems with brk() on some systems (e.g., Blue Gene/L)
     by grabbing a bunch of memory from the OS (which calls brk()),
     then releasing the memory back to malloc(), except for one block
     at the end, which is used to prevent malloc from moving brk() back down.
  */
#define MEMORY_PREALLOCATE_MAX 4096
  void *ptrs[MEMORY_PREALLOCATE_MAX];
  int i,len=0;
  for (i=0;i<MEMORY_PREALLOCATE_MAX;i++) {
    ptrs[i] = mm_malloc(1024*1024);
    if (ptrs[i]==NULL) break;
    else len=i+1; /* this allocation worked */
  }
  /* we now own all the memory-- release all but the last meg. */
  /* printf("CMK_MEMORY_PREALLOCATE_HACK claimed %d megs\n",len); */
  for (i=len-2;i>=0;i--) {
    mm_free(ptrs[i]);
  }
#endif
}

void CmiOutOfMemoryInit(void) {
  if (CmiMyRank() == 0) {
#if CMK_MEMORY_PREALLOCATE_HACK
  memory_preallocate_hack();
#endif
  memory_lifeRaft=(char *)mm_malloc(16384);
  }
}

#ifndef CMK_MEMORY_BUILD_CHARMDEBUG
/* declare the cpd_memory routines */
void CpdSetInitializeMemory(int v) { }
size_t  cpd_memory_length(void *lenParam) { return 0; }
void cpd_memory_pup(void *itemParam,pup_er p,CpdListItemsRequest *req) { }
void cpd_memory_leak(void *itemParam,pup_er p,CpdListItemsRequest *req) { }
void check_memory_leaks(LeakSearchInfo* i) { }
size_t  cpd_memory_getLength(void *lenParam) { return 0; }
void cpd_memory_get(void *itemParam,pup_er p,CpdListItemsRequest *req) { }
void CpdMemoryMarkClean(char *msg) { }
/* routine used by CthMemory{Protect,Unprotect} to specify that some region of
   memory has been protected */
void setProtection(char *mem, char *ptr, int len, int flag) { }
/* Routines used to specify how the memory will the used */
#ifdef setMemoryTypeChare
#undef setMemoryTypeChare
#endif
void setMemoryTypeChare(void *ptr) { }
#ifdef setMemoryTypeMessage
#undef setMemoryTypeMessage
#endif
void setMemoryTypeMessage(void *ptr) { }
void CpdSystemEnter(void) { }
void CpdSystemExit(void) { }

void CpdResetMemory(void) { }
void CpdCheckMemory(void) { }

int get_memory_allocated_user_total(void) { return 0; }
void * MemoryToSlot(void *ptr) { return NULL; }
int Slot_ChareOwner(void *s) { return 0; }
int Slot_AllocatedSize(void *s) { return 0; }
int Slot_StackTrace(void *s, void ***stack) { return 0; }
#ifdef setMemoryChareIDFromPtr
#undef setMemoryChareIDFromPtr
#endif
int setMemoryChareIDFromPtr(void *ptr) { return 0; }
#ifdef setMemoryChareID
#undef setMemoryChareID
#endif
void setMemoryChareID(int id) { }
#ifdef setMemoryOwnedBy
#undef setMemoryOwnedBy
#endif
void setMemoryOwnedBy(void *ptr, int id) { }

#endif


/* Genearl functions for malloc'ing aligned buffers */

/* CmiMallocAligned: Allocates a memory buffer with the given byte alignment.
     Additionally, the length of the returned buffer is also a multiple of
     alignment and >= size.
   NOTE: Memory allocated with CmiMallocAligned MUST be free'd with CmiFreeAligned()
*/
void* CmiMallocAligned(const size_t size, const unsigned int alignment) {

  void* rtn = NULL;
  int tailPadding;
  unsigned short offset = 0;

  /* Verify the pararmeters */
  if (size <= 0 || alignment <= 0) return NULL;

  /* Malloc memory of size equal to size + alignment + (alignment - (size % alignment)).  The
   *   last term 'alignment - (size % alignment)' ensures that there is enough tail padding
   *   so a DMA can be performed based on the alignment.  (I.e. - Start and end the memory
   *   region retured on an alignment boundry specified.)
   * NOTE: Since we need a byte long header, even if we "get lucky" and the malloc
   *   returns a pointer with the given alignment, we need to put in a byte
   *   preamble anyway.
   */
  tailPadding = alignment - (size % alignment);
  if (tailPadding == alignment)
    tailPadding = 0;

  /* Allocate the memory */
  rtn = malloc(size + alignment + tailPadding);

  /* Calculate the offset into the returned memory chunk that has the required alignment */
  offset = (char)(((size_t)rtn) % alignment);
  offset = alignment - offset;
  if (offset == 0) offset = alignment;

  /* Write the offset into the byte before the address to be returned */
  *((char*)rtn + offset - 1) = offset;

  /* Return the address with offset */
  /* cppcheck-suppress memleak */
  return (void*)((char*)rtn + offset);
}

void CmiFreeAligned(void* ptr) {

  char offset;

  /* Verify the parameter */
  if (ptr == NULL) return;

  /* Read the offset (byte before ptr) */
  offset = *((char*)ptr - 1);

  /* Free the memory */
  free ((void*)((char*)ptr - offset));
}


// Replace remaining symbols found in glibc's malloc.o to avoid linker errors
// See: https://github.com/UIUC-PPL/charm/issues/1325

#if (CMK_MEMORY_BUILD_OS && CMK_MEMORY_BUILD_OS_WRAPPED && !CMK_MEMORY_BUILD_GNU_HOOKS) || !CMK_MEMORY_BUILD_OS

#if !defined CMI_MEMORY_GNU || !defined _LIBC
CLINKAGE void * __libc_memalign (size_t alignment, size_t bytes) { return memalign(alignment, bytes); }

#if CMK_EXPECTS_MORECORE
CLINKAGE void * __default_morecore (ptrdiff_t) CMK_THROW;
void *(*__morecore)(ptrdiff_t) = __default_morecore;
#endif
#endif

#if defined CMI_MEMORY_GNU && defined _LIBC
CLINKAGE int mallopt (int param_number, int value) CMK_THROW { return __libc_mallopt(param_number, value); }
#elif !defined CMI_MEMORY_GNU || defined _LIBC
CLINKAGE int mallopt (int param_number, int value) CMK_THROW { return 1; }
#endif

CLINKAGE void __malloc_fork_lock_parent (void) { }
CLINKAGE void __malloc_fork_unlock_parent (void) { }
CLINKAGE void __malloc_fork_unlock_child (void) { }

#if defined __APPLE__
// strdup is statically linked against malloc on macOS
char * strdup (const char *str)
{
  const size_t length = strlen(str);
  const size_t bufsize = length + 1;
  char * const buf = (char *)malloc(bufsize);

  if (buf == nullptr)
    return nullptr;

  memcpy(buf, str, bufsize);
  return buf;
}
char * strndup (const char *str, size_t n)
{
  const size_t length = strnlen(str, n);
  const size_t bufsize = length + 1;
  char * const buf = (char *)malloc(bufsize);

  if (buf == nullptr)
    return nullptr;

  memcpy(buf, str, length);
  buf[length] = '\0';
  return buf;
}
#endif

#endif


/** @} */
