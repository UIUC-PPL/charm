/**************************************************************************
Isomalloc:

A way to allocate memory at the same address on every processor.
This enables linked data structures, like thread stacks, to be migrated
to the same address range on other processors.  This is similar to an
explicitly managed shared memory system.

The memory is used and released via the mmap()/munmap() calls, so unused
memory does not take any (RAM, swap or disk) space.

The way it is implemented is that the available virtual address space is
divided into globally addressable sections for each unit of work in the
callee's design paradigm, such as the virtual rank or the PE.

Written for migratable threads by Milind Bhandarkar around August 2000;
generalized by Orion Lawlor November 2001.  B-tree implementation
added by Ryan Mokos in July 2008 (no longer present, see mem-arena.C).

Substantially rewritten by Evan Ramos in 2019.
 *************************************************************************/

#include "converse.h"
#include "memory-isomalloc.h"
#include "pup.h"
#include "pup_stl.h"

#define ISOMALLOC_DEBUG 0

#if ISOMALLOC_DEBUG
#define DEBUG_PRINT(...) CmiPrintf(__VA_ARGS__)
#else
#define DEBUG_PRINT(...)
#endif

#define ISOMEMPOOL_DEBUG 0

#if ISOMEMPOOL_DEBUG
#define IMP_DBG(...) CmiPrintf(__VA_ARGS__)
#else
#define IMP_DBG(...)
#endif

#include <errno.h> /* just so I can find dynamically-linked symbols */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#ifdef _WIN32
# include <io.h>
# define open _open
# define close _close
# define read _read
# define write _write
#endif

#if CMK_HAS_ADDR_NO_RANDOMIZE
#include <sys/personality.h>
#endif

template <typename T>
static inline typename std::enable_if<std::is_pointer<T>::value>::type pup_raw_pointer(PUP::er & p, T & ptr)
{
  p((uint8_t *)&ptr, sizeof(T));
}

#if __FAULT__
static char CmiIsomallocRestart;
#endif
static int _mmap_probe = 0;

static int read_randomflag(void)
{
  FILE * fp;
  int random_flag;
  fp = fopen("/proc/sys/kernel/randomize_va_space", "r");
  if (fp != NULL)
  {
    if (fscanf(fp, "%d", &random_flag) != 1)
    {
      CmiAbort("Isomalloc> fscanf failed reading /proc/sys/kernel/randomize_va_space!");
    }
    fclose(fp);
    if (random_flag) random_flag = 1;
#if CMK_HAS_ADDR_NO_RANDOMIZE
    if (random_flag)
    {
      int persona = personality(0xffffffff);
      if (persona & ADDR_NO_RANDOMIZE) random_flag = 0;
    }
#endif
  }
  else
  {
    random_flag = -1;
  }
  return random_flag;
}

/* Integral type to be used for pointer arithmetic: */
typedef size_t memRange_t;

/* Size in bytes of a single slot */
static size_t pagesize, slotsize;

/* Start and end of isomalloc-managed addresses.
   If isomallocStart==NULL, isomalloc is disabled.
   */
static uint8_t * isomallocStart;
static uint8_t * isomallocEnd;

#if 0
/*This version of the allocate/deallocate calls are used if the
  real mmap versions are disabled.*/
/* Disabled because currently unused */
static int disabled_map_warned = 0;
static void * disabled_map(int size)
{
  if (!disabled_map_warned)
  {
    disabled_map_warned = 1;
    if (CmiMyPe() == 0)
      CmiError(
          "Charm++> Warning: Isomalloc is uninitialized."
          " You won't be able to migrate threads.\n");
  }
  return malloc(size);
}
static void disabled_unmap(void * bk) { free(bk); }
#endif

/*Turn off isomalloc memory, for the given reason*/
static void disable_isomalloc(const char * why)
{
  isomallocStart = nullptr;
  isomallocEnd = nullptr;
  if (CmiMyPe() == 0)
    CmiPrintf("Converse> Disabling Isomalloc: %s.\n", why);
}

#ifdef _WIN32
/****************** Manipulate memory map (Win32 version) *****************/
#include <windows.h>

static constexpr void * const mmap_fail = nullptr;

static inline void * call_mmap_fixed(void * addr, size_t len)
{
  return VirtualAlloc(addr, len, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
}
static inline void * call_mmap_anywhere(size_t len)
{
  return call_mmap_fixed(nullptr, len);
}
static void call_munmap(void * addr, size_t len)
{
  auto ptr = (char *)addr;
  MEMORY_BASIC_INFORMATION minfo;
  while (len)
  {
    if (VirtualQuery(ptr, &minfo, sizeof(minfo)) == 0)
      return;
    if (minfo.BaseAddress != ptr || minfo.AllocationBase != ptr ||
        minfo.State != MEM_COMMIT || minfo.RegionSize > len)
      return;
    if (VirtualFree(ptr, 0, MEM_RELEASE) == 0)
      return;
    ptr += minfo.RegionSize;
    len -= minfo.RegionSize;
  }
}
static inline int init_map() { return 1; /* No init necessary */ }
#elif CMK_HAS_MMAP
/****************** Manipulate memory map (UNIX version) *****************/
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#if !CMK_HAS_MMAP_ANON
CpvStaticDeclare(int, zerofd); /*File descriptor for /dev/zero, for mmap*/
#endif

// Unlike VirtualAlloc, mmap returns -1 as a sentinel.
#define mmap_fail (MAP_FAILED)

/**
 * Maps this address with these flags.
 */
static inline void * call_mmap(void * addr, size_t len, int flags)
{
  return mmap(addr, len, PROT_READ | PROT_WRITE,
#if CMK_HAS_MMAP_ANON
              flags | MAP_PRIVATE | MAP_ANON, -1,
#else
              flags | MAP_PRIVATE, CpvAccess(zerofd),
#endif
              0);
}
static inline void * call_mmap_fixed(void * addr, size_t len)
{
  return call_mmap(addr, len, MAP_FIXED);
}
static inline void * call_mmap_anywhere(size_t len)
{
  return call_mmap((void *)0, len, 0);
}

/* Unmaps this address range */
static void call_munmap(void * addr, size_t len)
{
#if CMK_ERROR_CHECKING
  if (addr == 0)
    return; /* NULL address is never mapped */
#endif

  if (munmap(addr, len) == -1)
    CmiAbort("munmap call failed to deallocate requested memory.\n");
}

static int init_map()
{
#if CMK_HAS_MMAP_ANON
  /*Don't need /dev/zero*/
#else
  CpvInitialize(int, zerofd);
  CpvAccess(zerofd) = open("/dev/zero", O_RDWR);
  if (CpvAccess(zerofd) < 0)
    return 0; /* Cannot open /dev/zero or use MMAP_ANON, so can't mmap memory */
#endif
  return 1;
}
#else /* CMK_HAS_MMAP */
/****************** Manipulate memory map (stub non-version) *****************/
static constexpr void * const mmap_fail = nullptr;

static void * call_mmap_fixed(void * addr, size_t len)
{
  CmiAbort("isomalloc.C: mmap_fixed should never be called here.");
  return nullptr;
}
static void * call_mmap_anywhere(size_t len)
{
  CmiAbort("isomalloc.C: mmap_anywhere should never be called here.");
  return nullptr;
}
static void call_munmap(void * addr, size_t len)
{
  CmiAbort("isomalloc.C: munmap should never be called here.");
}

static int init_map() { return 0; /*Isomalloc never works without mmap*/ }
#endif

/**
 * maps the virtual memory associated with slot using mmap
 */
static void * map_global_memory(void * addr, size_t len)
{
  void * pa = call_mmap_fixed(addr, len);

  if (pa == mmap_fail)
  { /*Map just failed completely*/
    auto err = errno;
    CmiError("Charm++> [%d] Isomalloc tried to mmap(%p, %zu), but encountered error %d\n", CmiMyPe(), addr, len, err);
    CmiAbort("mmap failed in isomalloc");
    return NULL;
  }
  else if (pa != addr)
  { /*Map worked, but gave us back the wrong place*/
    CmiError("Charm++> [%d] tried to mmap(%p, %zu), but got %p back\n", CmiMyPe(), addr, len, pa);
    call_munmap(addr, len);
    CmiAbort("mmap failed in isomalloc");
    return NULL;
  }
  DEBUG_PRINT("[%d] mmap(%p, %zu) succeeded\n", CmiMyPe(), addr, len);
  return pa;
}

/*
 * unmaps the virtual memory associated with slot using munmap
 */
static void unmap_global_memory(void * addr, size_t len)
{
  call_munmap(addr, len);
  DEBUG_PRINT("[%d] munmap(%p, %zu) succeeded\n", CmiMyPe(), addr, len);
}

/************ Address space voodoo: find free address range **********/

/*This struct describes a range of virtual addresses*/
typedef struct
{
  uint8_t * start;      /*First byte of region*/
  memRange_t len;    /*Number of bytes in region*/
  const char * type; /*String describing memory in region (debugging only)*/
} memRegion_t;

/*Estimate the top of the current stack*/
static void * __cur_stack_frame(void)
{
  char __dummy = 'A';
  void * top_of_stack = (void *)&__dummy;
  return top_of_stack;
}
/*Estimate the location of the static data region*/
static void * __static_data_loc(void)
{
  static char __dummy;
  return (void *)&__dummy;
}

/*Pointer comparison is in these subroutines, because
  comparing arbitrary pointers is nonportable and tricky.
  */
static int pointer_lt(const void * a, const void * b)
{
  return ((memRange_t)a) < ((memRange_t)b);
}
static int pointer_ge(const void * a, const void * b)
{
  return ((memRange_t)a) >= ((memRange_t)b);
}

static uint8_t * pmin(uint8_t * a, uint8_t * b) { return pointer_lt(a, b) ? a : b; }
static uint8_t * pmax(uint8_t * a, uint8_t * b) { return pointer_lt(a, b) ? b : a; }

static const constexpr memRange_t meg = 1024u * 1024u;         /* One megabyte */
static const constexpr memRange_t gig = 1024u * 1024u * 1024u; /* One gigabyte */
static const constexpr CmiUInt8 tb = (CmiUInt8)gig * 1024ull;  /* One terabyte */

#if CMK_64BIT
static const constexpr CmiUInt8 vm_limit = tb * 256ull;
static const constexpr memRange_t other_libs = 16ul * gig; /* space for other libraries to use */
static const constexpr memRange_t heuristicHeapSize = 1u * gig;
static const constexpr memRange_t heuristicMmapSize = 2u * gig;
static const constexpr int minimumRegionSize = 512u * meg;
/* the smallest size used when describing unavailable regions */
static const constexpr memRange_t division_size = 256u * meg;
#else
static const constexpr memRange_t other_libs = 256u * meg;
static const constexpr memRange_t heuristicHeapSize = 64u * meg;
static const constexpr memRange_t heuristicMmapSize = 64u * meg;
static const constexpr int minimumRegionSize = 64u * meg;
static const constexpr memRange_t division_size = 32u * meg;
#endif

/* Maybe write a new function that distributes start points as
 * 0, 1/2, 1/4, 3/4, 1/8, 3/8, 5/8, 7/8, 1/16, ... */
static uint8_t * get_space_partition(uint8_t * start, uint8_t * end, int myunit, int numunits)
{
  const uintptr_t available_slots = ((uintptr_t)end - (uintptr_t)start) / slotsize;
  CmiEnforce(available_slots >= numunits);
  return start + (available_slots * myunit / numunits * slotsize);
}

/*Check if this memory location is usable.
  If not, return 1.
  */
static int bad_location(uint8_t * loc)
{
  void * addr = call_mmap_fixed(loc, slotsize);
  if (addr == mmap_fail || addr != loc)
  {
    DEBUG_PRINT("[%d] Skipping unmappable space at %p\n", CmiMyPe(), loc);
    return 1; /*No good*/
  }
  call_munmap(addr, slotsize);
  return 0; /*This works*/
}

/* Split this range up into n pieces, returning the size of each piece */
static constexpr memRange_t divide_range(memRange_t len, int n) { return (len + 1) / n; }

/* Return if this memory region has *any* good parts. */
static int partially_good(uint8_t * start, memRange_t len, int n)
{
  int i;
  memRange_t quant = divide_range(len, n);
  CmiAssert(quant > 0);
  for (i = 0; i < n; i++)
    if (!bad_location(start + i * quant)) return 1; /* it's got some good parts */
  return 0;                                         /* all locations are bad */
}

/* Return if this memory region is usable at n samples.
 */
static int good_range(uint8_t * start, memRange_t len, int n)
{
  int i;
  memRange_t quant = divide_range(len, n);
  DEBUG_PRINT("good_range: %zu, %d\n", quant, n);
  CmiAssert(quant > 0);

  for (i = 0; i < n; i++)
    if (bad_location(start + i * quant)) return 0; /* it's got some bad parts */
  /* It's all good: */
  return 1;
}

/*Check if this entire memory range, or some subset
  of the range, is usable.  If so, write it into max.
  */
static void check_range(uint8_t * start, uint8_t * end, memRegion_t * max)
{
  memRange_t len;

  if (start >= end) return; /*Ran out of hole*/
  len = (memRange_t)end - (memRange_t)start;

#if CMK_64BIT
  /* Note: 256TB == 2^48 bytes.  So a 48-bit virtual-address CPU
   *    can only actually address 256TB of space. */
  if (len / tb > 10u)
  { /* This is an absurd amount of space-- cut it down, for safety */
    if ((uintptr_t)start != other_libs)
      start += other_libs;
    end = pmin(start + vm_limit - 2 * other_libs, end - other_libs);
    len = (memRange_t)end - (memRange_t)start;
  }
#endif

  if (len <= max->len) return; /*It's too short already!*/
  DEBUG_PRINT("[%d] Checking at %p - %p\n", CmiMyPe(), start, end);

  /* Check the middle of the range */
  if (!good_range(start, len, 256))
  {
    /* Try to split into subranges: */
    int i, n = 2;
    DEBUG_PRINT("[%d] Trying to split bad address space at %p - %p...\n", CmiMyPe(),
                start, end);
    len = divide_range(len, n);
    for (i = 0; i < n; i++)
    {
      uint8_t * cur = start + i * len;
      if (partially_good(cur, len, 16)) check_range(cur, cur + len, max);
    }
    return; /* Hopefully one of the subranges will be any good */
  }
  else /* range is good */
  {
    DEBUG_PRINT("[%d] Address space at %p - %p is largest\n", CmiMyPe(), start, end);

    /*If we got here, we're the new largest usable range*/
    max->len = len;
    max->start = start;
    max->type = "Unused";
  }
}

/*Find the first available memory region of at least the
  given size not touching any data in the used list.
  */
static memRegion_t find_free_region(memRegion_t * used, int nUsed, int atLeast)
{
  memRegion_t max;
  int i, j;

  max.start = 0;
  max.len = atLeast;
  /*Find the largest hole between regions*/
  for (i = 0; i < nUsed; i++)
  {
    /*Consider a hole starting at the end of region i*/
    uint8_t * holeStart = used[i].start + used[i].len;
    uint8_t * holeEnd = (uint8_t *)~(intptr_t)0;

    /*Shrink the hole by all others*/
    for (j = 0; j < nUsed && pointer_lt(holeStart, holeEnd); j++)
    {
      if (pointer_lt(used[j].start, holeStart))
        holeStart = pmax(holeStart, used[j].start + used[j].len);
      else if (pointer_lt(used[j].start, holeEnd))
        holeEnd = pmin(holeEnd, used[j].start);
    }

    check_range(holeStart, holeEnd, &max);
  }

  return max;
}

/*
   By looking at the address range carefully, try to find
   the largest usable free region on the machine.
   */
static int find_largest_free_region(memRegion_t * destRegion)
{
  uint8_t * staticData = (uint8_t *)__static_data_loc();
  uint8_t * code = (uint8_t *)&find_free_region;
  uint8_t * threadData = (uint8_t *)&errno;
  uint8_t * codeDll = (uint8_t *)fprintf;
  uint8_t * heapLil = (uint8_t *)malloc(1);
  uint8_t * heapBig = (uint8_t *)malloc(6 * meg);
  uint8_t * stack = (uint8_t *)__cur_stack_frame();
  size_t mmapAnyLen = 1 * meg;
  void * mmapAny = call_mmap_anywhere(mmapAnyLen);

  int i, nRegions = 0;
  memRegion_t regions[10]; /*used portions of address space*/
  memRegion_t freeRegion;  /*Largest unused block of address space*/

  /*Mark off regions of virtual address space as ususable*/
  regions[nRegions].type = "NULL";
  regions[nRegions].start = NULL;
  regions[nRegions++].len = other_libs;

  regions[nRegions].type = "Static program data";
  regions[nRegions].start = staticData;
  regions[nRegions++].len = division_size;

  regions[nRegions].type = "Program executable code";
  regions[nRegions].start = code;
  regions[nRegions++].len = division_size;

  regions[nRegions].type = "Heap (small blocks)";
  regions[nRegions].start = heapLil;
  regions[nRegions++].len = heuristicHeapSize;

  regions[nRegions].type = "Heap (large blocks)";
  regions[nRegions].start = heapBig;
  regions[nRegions++].len = heuristicHeapSize;

  regions[nRegions].type = "Stack space";
  regions[nRegions].start = stack;
  regions[nRegions++].len = division_size;

  regions[nRegions].type = "Program dynamically linked code";
  regions[nRegions].start = codeDll;
  regions[nRegions++].len = division_size;

  if (mmapAny != mmap_fail)
  {
    regions[nRegions].type = "Result of a non-fixed call to mmap";
    regions[nRegions].start = (uint8_t *)mmapAny;
    regions[nRegions++].len = heuristicMmapSize;

    call_munmap(mmapAny, mmapAnyLen);
  }

  regions[nRegions].type = "Thread private data";
  regions[nRegions].start = threadData;
  regions[nRegions++].len = division_size;

  _MEMCHECK(heapBig);
  free(heapBig);
  _MEMCHECK(heapLil);
  free(heapLil);

  /*Align each memory region*/
  for (i = 0; i < nRegions; i++)
  {
#if ISOMALLOC_DEBUG
    memRegion_t old = regions[i];
#endif
    memRange_t p = (memRange_t)regions[i].start;
    p &= ~(regions[i].len - 1); /*Round start down to a len-boundary (mask off low bits)*/
    regions[i].start = (uint8_t *)p;
    DEBUG_PRINT("[%d] Memory map: %p - %p (len: %lu => %lu) %s \n", CmiMyPe(),
                regions[i].start, regions[i].start + regions[i].len, old.len,
                regions[i].len, regions[i].type);
  }

  /*Find a large, unused region in this map: */
  freeRegion = find_free_region(regions, nRegions, (512u) * meg);

  if (freeRegion.start == 0)
  { /*No free address space-- disable isomalloc:*/
    return 0;
  }
  else /* freeRegion is valid */
  {
    *destRegion = freeRegion;

    return 1;
  }
}

static int try_largest_mmap_region(memRegion_t * destRegion)
{
  void * range, * good_range = NULL;
  double shrink = 1.5;
  size_t size = ((size_t)(-1l)), good_size = 0;
  int retry = 0;
  if (sizeof(size_t) >= 8) size = size >> 2; /* 25% of machine address space! */
  while (1)
  { /* test out an allocation of this size */
#ifdef _WIN32
    range = VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_READWRITE);
#elif CMK_HAS_MMAP
    range = mmap(NULL, size, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE
#if CMK_HAS_MMAP_ANON
                     | MAP_ANON
#endif
#if CMK_HAS_MMAP_NORESERVE
                     | MAP_NORESERVE
#endif
                 ,
                 -1, 0);
#else
    range = mmap_fail;
#endif
    if (range == mmap_fail)
    { /* mmap failed */
#if CMK_HAS_USLEEP
      if (retry++ < 5)
      {
        usleep(rand() % 10000);
        continue;
      }
      else
        retry = 0;
#endif
      size = (double)size / shrink; /* shrink request */
      if (size <= 0) return 0;      /* mmap doesn't work */
    }
    else
    { /* this allocation size is available */
      DEBUG_PRINT("[%d] available: %p, %zu\n", CmiMyPe(), range, size);
      call_munmap(range, size); /* needed/wanted? */
      if (size > good_size)
      {
        good_range = range;
        good_size = size;
        size = ((double)size) * 1.1;
        continue;
      }
      break;
    }
  }
  CmiAssert(good_range != NULL);
  destRegion->start = (uint8_t *)good_range;
  destRegion->len = good_size;
#if ISOMALLOC_DEBUG
#ifndef _WIN32
  pid_t pid = getpid();
  {
    char s[128];
    sprintf(s, "cat /proc/%d/maps", pid);
    system(s);
  }
#endif
  DEBUG_PRINT("[%d] try_largest_mmap_region: %p, %zu\n", CmiMyPe(), good_range,
              good_size);
#endif
  return 1;
}

struct CmiAddressSpaceRegion
{
  CmiUInt8 s, e;
};

CmiAddressSpaceRegion IsoRegion;

static void CmiAddressSpaceRegionPup(pup_er cpup, void * data)
{
  auto region = (CmiAddressSpaceRegion *)data;
  PUP::er & p = *(PUP::er *)cpup;

  p | region->s;
  p | region->e;
}

static void * CmiAddressSpaceRegionMerge(int * size, void * data, void ** contributions, int count)
{
  auto local = (CmiAddressSpaceRegion *)data;

  for (int i = 0; i < count; ++i)
  {
    auto remote = (CmiAddressSpaceRegion *)contributions[i];

    if (remote->s > local->s)
      local->s = remote->s;
    if (remote->e < local->e)
      local->e = remote->e;
  }

  return local;
}

struct CmiAddressSpaceRegionMsg
{
  char converseHeader[CmiMsgHeaderSizeBytes];
  CmiAddressSpaceRegion region;
};

static std::atomic<char> CmiIsomallocSyncHandlerDone;
#if CMK_SMP && !CMK_SMP_NO_COMMTHD
extern void CommunicationServerThread(int sleepTime);
static std::atomic<char> CmiIsomallocSyncCommThreadDone;
#endif

#if CMK_SMP && !CMK_SMP_NO_COMMTHD
static void CmiIsomallocSyncWaitCommThread()
{
  do
    CommunicationServerThread(5);
  while (!CmiIsomallocSyncCommThreadDone.load());

  CommunicationServerThread(5);
}
#endif

static void CmiIsomallocSyncWait()
{
  do
    CsdSchedulePoll();
  while (!CmiIsomallocSyncHandlerDone.load());

  CsdSchedulePoll();
}

static int CmiIsomallocSyncBroadcastHandlerIdx;

static void CmiIsomallocSyncReductionHandler(void * data)
{
  auto region = (CmiAddressSpaceRegion *)data;
  CmiAssert(region == &IsoRegion);

  CmiIsomallocSyncHandlerDone = 1;
}
static void CmiIsomallocSyncBroadcastHandler(void * msg)
{
  const CmiAddressSpaceRegion region = ((CmiAddressSpaceRegionMsg *)msg)->region;
  DEBUG_PRINT("Isomalloc> Node %d received region for assignment: %" PRIx64 " %" PRIx64 "\n",
              CmiMyNode(), region.s, region.e);

  IsoRegion = region;

  CmiFree(msg);

  CmiIsomallocSyncHandlerDone = 1;
}

static void CmiIsomallocInitExtent(char ** argv)
{
#if 0
  /*Largest value a signed int can hold*/
  static constexpr memRange_t intMax = (((memRange_t)1) << (sizeof(int) * 8 - 1)) - 1;
#endif

  if (CmiMyRank() == 0 && isomallocStart == nullptr)
  {
    memRegion_t freeRegion{};

    /* Find the largest unused region of virtual address space */
    /*Round slot size up to nearest page size*/
    slotsize = 1024 * 1024;
    pagesize = CmiGetPageSize();
    slotsize = CMIALIGN(slotsize, pagesize);

#if ISOMALLOC_DEBUG
    if (CmiMyPe() == 0) DEBUG_PRINT("[%d] Using slotsize of %zu\n", CmiMyPe(), slotsize);
#endif

#ifdef CMK_MMAP_START_ADDRESS /* Hardcoded start address, for machines where automatic \
                                 fails */
    freeRegion.start = CMK_MMAP_START_ADDRESS;
    freeRegion.len = CMK_MMAP_LENGTH_MEGS * meg;
#endif

    if (freeRegion.len == 0u)
    {
      if (_mmap_probe == 1)
      {
        try_largest_mmap_region(&freeRegion);
      }
      else
      {
        find_largest_free_region(&freeRegion);
      }
    }

#if 0
    /*Make sure our largest slot number doesn't overflow an int:*/
    if (freeRegion.len/slotsize>intMax)
      freeRegion.len=intMax*slotsize;
#endif

    if (freeRegion.len == 0u)
    {
      disable_isomalloc("no free virtual address space");
    }
    else /* freeRegion.len>0, so can isomalloc */
    {
      memRange_t start = CMIALIGN((uintptr_t)freeRegion.start, division_size);
      memRange_t end = CMIALIGN((uintptr_t)freeRegion.start + freeRegion.len - (division_size-1), division_size);
      memRange_t len = end - start;
      IsoRegion.s = start;
      IsoRegion.e = end;
      DEBUG_PRINT("[%d] Isomalloc memory region: 0x%zx - 0x%zx (%zu gigs)\n", CmiMyPe(),
                  start, end, len/gig);
    }
  }

  /* isomalloc_sync
   * Available address space regions can vary across nodes.
   * Calculate the intersection of all memory regions on all nodes.
   */

  auto nosync = CmiGetArgFlagDesc(argv, "+no_isomalloc_sync", "disable global synchronization of isomalloc region");
  CmiAssignOnce(&CmiIsomallocSyncBroadcastHandlerIdx, CmiRegisterHandler(CmiIsomallocSyncBroadcastHandler));

#if __FAULT__
  if (CmiIsomallocRestart)
  {
    if (CmiMyRank() == 0)
    {
      CmiAddressSpaceRegion previous;

      int try_count = 0, fd;
      while ((fd = open(".isomalloc", O_RDONLY)) == -1 && try_count < 10000)
        try_count++;

      if (fd == -1)
        CmiAbort("isomalloc_sync failed during restart, make sure you have a shared file system.");
      if (read(fd, &previous, sizeof(CmiAddressSpaceRegion)) != sizeof(CmiAddressSpaceRegion))
        CmiAbort("Isomalloc> call to read() failed during restart!");

      close(fd);
      if (previous.s < IsoRegion.s || previous.e > IsoRegion.e)
        CmiError("Isomalloc> Warning: virtual memory regions do not overlap: "
                 "current %" PRIx64 " - %" PRIx64 ", previous %" PRIx64 " - %" PRIx64 "\n",
                 IsoRegion.s, IsoRegion.e, previous.s, previous.e);
      else
        IsoRegion = previous;

      if (CmiMyPe() == 0)
        CmiPrintf("Isomalloc> Synchronized global address space.\n");

      DEBUG_PRINT("Charm++> Consolidated Isomalloc memory region at restart: %p - %p (%" PRId64 " MB).\n",
                  (void *)IsoRegion.s, (void *)IsoRegion.e, (IsoRegion.e - IsoRegion.s) / meg);
    }
  }
  else
#endif
  if (nosync)
  {
    if (CmiMyPe() == 0)
      CmiPrintf("Isomalloc> Disabling global synchronization of address space.\n");
  }
  else if (CmiNumNodes() > 1)
  {
    if (CmiMyRank() == 0)
    {
      if (CmiMyNode() == 0)
      {
        DEBUG_PRINT("Charm++> Synchronizing Isomalloc memory region...\n");
      }

      DEBUG_PRINT("Isomalloc> Node %d sending region for comparison: %" PRIx64 " %" PRIx64 "\n",
                  CmiMyNode(), IsoRegion.s, IsoRegion.e);

      CmiNodeReduceStruct(&IsoRegion, CmiAddressSpaceRegionPup, CmiAddressSpaceRegionMerge,
                          CmiIsomallocSyncReductionHandler, nullptr);

      CmiIsomallocSyncWait();

      if (CmiMyNode() == 0)
      {
        DEBUG_PRINT("Isomalloc> Node %d sending region for assignment: %" PRIx64 " %" PRIx64 "\n",
                    CmiMyNode(), IsoRegion.s, IsoRegion.e);

        CmiAddressSpaceRegionMsg msg;
        CmiInitMsgHeader(msg.converseHeader, sizeof(CmiAddressSpaceRegionMsg));
        msg.region = IsoRegion;
        CmiSetHandler((char *)&msg, CmiIsomallocSyncBroadcastHandlerIdx);
        CmiSyncNodeBroadcast(sizeof(CmiAddressSpaceRegionMsg), &msg);

        CsdSchedulePoll();

        if (IsoRegion.s >= IsoRegion.e)
          CmiAbort("Isomalloc> failed to find consolidated region: %" PRIx64 " - %" PRIx64 ".\n"
                   "Try running with +no_isomalloc_sync if you do not need this functionality.\n",
                   IsoRegion.s, IsoRegion.e);

        if (CmiMyPe() == 0)
          CmiPrintf("Isomalloc> Synchronized global address space.\n");

        DEBUG_PRINT("Charm++> Consolidated Isomalloc memory region: %p - %p (%" PRId64 " MB).\n",
                    (void *)IsoRegion.s, (void *)IsoRegion.e, (IsoRegion.e - IsoRegion.s) / meg);
      }

#if CMK_SMP && !CMK_SMP_NO_COMMTHD
      CmiIsomallocSyncCommThreadDone = 1;
#endif
    }
#if CMK_SMP && !CMK_SMP_NO_COMMTHD
    else if (CmiInCommThread())
    {
      CmiIsomallocSyncWaitCommThread();
    }
#endif
    else
    {
      CmiIsomallocSyncWait();
    }

    CmiBarrier();
  }

  if (CmiMyRank() == 0)
  {
#if __FAULT__
    if (!CmiIsomallocRestart && CmiMyNode() == 0)
    {
      int fd;
      while ((fd = open(".isomalloc", O_WRONLY | O_TRUNC | O_CREAT, 0644)) == -1)
        ;
      if (write(fd, &IsoRegion, sizeof(CmiAddressSpaceRegion)) != sizeof(CmiAddressSpaceRegion))
        CmiAbort("Isomalloc> call to write() failed while synchronizing memory regions!");
      close(fd);
    }
#endif

    if (IsoRegion.e > IsoRegion.s)
    {
      isomallocStart = (uint8_t *)(uintptr_t)IsoRegion.s;
      isomallocEnd = (uint8_t *)(uintptr_t)IsoRegion.e;
    }
  }

  CmiNodeAllBarrier();
}

struct isommap
{
  isommap(uint8_t * s, uint8_t * e)
    : start{s}, end{e}, allocated_extent{s}, use_rdma{1}, lock{CmiCreateLock()}
  {
    IMP_DBG("[%d][%p] isommap::isommap(%p, %p)\n", CmiMyPe(), this, s, e);
  }
  isommap(PUP::reconstruct pr)
    : lock{CmiCreateLock()}
  {
    IMP_DBG("[%d][%p] isommap::isommap(PUP::reconstruct)\n", CmiMyPe(), this);
  }
  ~isommap()
  {
    IMP_DBG("[%d][%p] isommap::~isommap()\n", CmiMyPe(), this);
    clear();
    CmiDestroyLock(lock);
  }

  void print() const
  {
    CmiPrintf("[%d][%p] isommap::print(): %p-%p %zu\n", CmiMyPe(), this, start, allocated_extent, allocated_extent - start);
  }

  bool isInRange(void * user_ptr) const
  {
    return start <= (const uint8_t *)user_ptr && (const uint8_t *)user_ptr < end;
  }
  bool isMapped(void * user_ptr) const
  {
    return start <= (const uint8_t *)user_ptr && (const uint8_t *)user_ptr < allocated_extent;
  }

  void pup(PUP::er & p)
  {
    IMP_DBG("[%d][%p] isommap::pup()%s%s%s%s\n", CmiMyPe(), this, p.isSizing() ? " sizing" : "",
            p.isPacking() ? " packing" : "", p.isUnpacking() ? " unpacking" : "", p.isDeleting() ? " deleting" : "");

    pup_raw_pointer(p, start);
    pup_raw_pointer(p, end);
    pup_raw_pointer(p, allocated_extent);
    p | use_rdma;

    const size_t totalsize = allocated_extent - start;

    if (p.isUnpacking())
    {
      if (start < isomallocStart || isomallocEnd < allocated_extent)
        CmiAbort("Could not unpack Isomalloc memory region, virtual memory regions do not overlap: "
                 "current %p - %p, context %p - %p",
                 isomallocStart, isomallocEnd, start, allocated_extent);

      if (isomallocEnd < end)
        end = (uint8_t *)CMIALIGN((uintptr_t)isomallocEnd - (pagesize-1), pagesize);
    }

    if (use_rdma)
    {
      uint8_t * localstart = start;

      p.pup_buffer(localstart, totalsize,
                   [localstart](size_t totalsize) -> void *
                   {
                     void * const mapped = map_global_memory(localstart, totalsize);
                     if (mapped == nullptr)
                       CmiAbort("Failed to unpack Isomalloc memory region!");
                     return mapped;
                   },
                   [totalsize](void * start)
                   {
                     unmap_global_memory(start, totalsize);
                   }
                   );

      if (p.isDeleting())
        allocated_extent = start; // the context no longer owns the mmapped region
    }
    else
    {
      if (p.isUnpacking())
      {
        void * const mapped = map_global_memory(start, totalsize);
        if (mapped == nullptr)
          CmiAbort("Failed to unpack Isomalloc memory region!");
      }

      p(start, totalsize);

      if (p.isDeleting())
        clear();
    }
  }

  void JustMigrated()
  {
    // Can be used for any post-migration functionality, such as restoring mprotect permissions.
  }

  void clear()
  {
    if (allocated_extent == start)
      return;
    unmap_global_memory(start, allocated_extent - start);
    allocated_extent = start;
  }

  void * permanent_alloc(size_t size)
  {
    const size_t realsize = CMIALIGN(size, pagesize);

    CmiLock(lock);

    void * const mapped = map_global_memory(allocated_extent, realsize);
    if (mapped != nullptr)
      allocated_extent += realsize;

    CmiUnlock(lock);
    return mapped;
  }

  void * permanent_alloc(size_t size, size_t align)
  {
    const size_t realsize = CMIALIGN(size, pagesize);

    CmiLock(lock);

    const auto alignstart = (uint8_t *)CMIALIGN((uintptr_t)allocated_extent, align);
    const size_t allocsize = realsize + (alignstart - allocated_extent);

    void * const mapped = map_global_memory(allocated_extent, allocsize);
    if (mapped != nullptr)
      allocated_extent += allocsize;

    CmiUnlock(lock);
    return mapped;
  }

  void EnableRDMA(int enable)
  {
    use_rdma = enable;
  }

  // canonical data
  uint8_t * start, * end, * allocated_extent;
  int use_rdma;

  // local data
  /*
   * This lock provides mutual exclusion for this struct, particularly allocated_extent.
   * It is usually uncontended since each migratable thread has its own context,
   * but it is here as a safeguard for a multithreaded case such as AMPI+OpenMP.
   */
  CmiNodeLock lock;
};

/************** dlmalloc mempool ***************/

#include "memory-gnu-internal.C"

struct isomalloc_dlmalloc : dlmalloc_impl
{
  isomalloc_dlmalloc(uint8_t * s, uint8_t * e)
    : backend{s, e}, arena{}
  {
    IMP_DBG("[%d][%p] isomalloc_dlmalloc::isomalloc_dlmalloc(%p, %p)\n", CmiMyPe(), this, s, e);
  }
  isomalloc_dlmalloc(PUP::reconstruct pr)
    : backend{pr}
  {
    IMP_DBG("[%d][%p] isomalloc_dlmalloc::isomalloc_dlmalloc(PUP::reconstruct)\n", CmiMyPe(), this);
  }

  void activate_random_access_heap()
  {
    if (arena != nullptr)
      return;

    arena = create_mspace(0, 0);
  }

  void pup(PUP::er & p)
  {
    p | backend;
    pup_raw_pointer(p, arena);
  }

  isommap backend;
  mspace arena;

  virtual void * call_mmap(size_t length) override final
  {
    void * const mapped = map_global_memory(backend.allocated_extent, length);
    if (mapped == nullptr)
      return nullptr;
    backend.allocated_extent += length;
    return mapped;
  }
  virtual void * call_direct_mmap(size_t length) override final
  {
    return this->isomalloc_dlmalloc::call_mmap(length); // call call_mmap without the vtable lookup
  }
  virtual int call_munmap(void * addr, size_t length) override final
  {
    /*
     * TODO: Unmap regions in the middle when requested and maintain a record of what is
     * still mapped so that migration can skip the gaps. Complexity arises when
     * considering RDMA transfers. We would need to determine when it is better to leave
     * small gaps mapped and combine transfers instead of requesting multiple transfers.
     */
    if ((const uint8_t *)addr + length == backend.allocated_extent)
    {
      unmap_global_memory(addr, length);
      backend.allocated_extent = (uint8_t *)addr;
    }
    return 0;
  }
  virtual void * call_mremap(void *old_address, size_t old_size, size_t new_size, int flags) override final
  {
    return MFAIL;
  }

  void * alloc(size_t size)
  {
    CmiLock(backend.lock);
    CmiAssert(arena != nullptr);
    void * ret = mspace_malloc(arena, size);
    CmiUnlock(backend.lock);
    return ret;
  }
  void * alloc(size_t size, size_t align)
  {
    CmiLock(backend.lock);
    CmiAssert(arena != nullptr);
    void * ret = mspace_memalign(arena, align, size);
    CmiUnlock(backend.lock);
    return ret;
  }
  void * calloc(size_t nelem, size_t size)
  {
    CmiLock(backend.lock);
    CmiAssert(arena != nullptr);
    void * ret = mspace_calloc(arena, nelem, size);
    CmiUnlock(backend.lock);
    return ret;
  }
  void * realloc(void * ptr, size_t size)
  {
    CmiLock(backend.lock);
    CmiAssert(arena != nullptr);
    void * ret = mspace_realloc(arena, ptr, size);
    CmiUnlock(backend.lock);
    return ret;
  }
  void free(void * ptr)
  {
    CmiLock(backend.lock);

    CmiAssert(arena != nullptr);
    CmiAssert(backend.isInRange(ptr));
    CmiAssert(backend.isMapped(ptr));

    mspace_free(arena, ptr);

    CmiUnlock(backend.lock);
  }
  size_t length(void * ptr)
  {
    CmiLock(backend.lock);

    CmiAssert(arena != nullptr);

    mchunkptr oldp = mem2chunk(ptr);
    size_t oc = chunksize(oldp) - overhead_for(oldp);

    CmiUnlock(backend.lock);

    return oc;
  }
};

/************** Isomempool ***************/

#if ISOMEMPOOL_DEBUG
# define VERIFY_RBTREE
#endif

struct Isomempool
{
  /* Isomempool
   *
   * This class is a wholly self-contained allocator, depending only on mmap/munmap.
   * It batches allocations of all sizes into a slab region that can expand as needed.
   *
   * Advantages:
   * This mempool provides space-efficient memory management. It has been thoroughly
   * verified for correctness. It contains robust assertions to pinpoint problems in the
   * event of future modifications. The code is clean, self-documenting, modular, short
   * in length, and well-optimized (for the approach it takes).
   *
   * Disadvantages:
   * Performance is poor in programs that behave poorly with regard to the heap, as well
   * as in certain circumstances in all programs. Problems with this mempool include lack
   * of a special approach for very small allocations, and a cache-inefficient tree node
   * layout.
   *
   * Definitions:
   * Regions are divisions within the pool, either allocated space or empty space between
   * allocations.
   *
   * The pool prefixes each allocation with a small header forming a doubly linked list,
   * to allow flexible insertion, removal, and examination of neighbors.
   *
   * Calls to alloc() are accelerated by a cache structure:
   * a red-black tree of free space segments keyed by their sizes.
   */

  using BitCarrier = unsigned char;

  struct RegionHeader
  {
    // tag structs
    struct Occupied
    {
      explicit Occupied(bool v = false) : value{v} { }
      explicit Occupied(uintptr_t v) : value{(bool)v} { }
      operator bool() const { return value; }
    private:
      bool value;
    };

    RegionHeader(RegionHeader * p, RegionHeader * n, size_t s, Occupied o)
      : prevfield{p}
      , nextfield{n}
      , sizefield{s}
      , occupiedfield{o}
    {
      IMP_DBG("[%d][%p] RegionHeader::RegionHeader(%p, %p, %zu, %u)\n",
              CmiMyPe(), this, p, n, s, (int)(bool)o);

      CmiAssert(n == nullptr || (o ? (const uint8_t *)this + s <= (const uint8_t *)n : (const uint8_t *)this + s == (const uint8_t *)n));
    }

    RegionHeader * prev() const
    {
      return prevfield;
    }
    RegionHeader * next() const
    {
      return nextfield;
    }

    void setPrev(RegionHeader * p)
    {
      IMP_DBG("[%d][%p] RegionHeader::setPrev(%p)\n", CmiMyPe(), this, p);

      prevfield = p;
    }

    // Set next and size separately iff region is occupied.
    void setNext(RegionHeader * n)
    {
      IMP_DBG("[%d][%p] RegionHeader::setNext(%p)\n", CmiMyPe(), this, n);
      CmiAssert(!isEmpty());

      nextfield = n;
    }
    void setSize(size_t s)
    {
      IMP_DBG("[%d][%p] RegionHeader::setSize(%zu)\n", CmiMyPe(), this, s);
      CmiAssert(!isEmpty());
      CmiAssert(next() == nullptr || (const uint8_t *)this + s <= (const uint8_t *)next());

      sizefield = s;
    }

    // Set next and size together iff region is empty.
    void setNextAndSize(RegionHeader * n)
    {
      const ptrdiff_t s = (const uint8_t *)n - (const uint8_t *)this;
      IMP_DBG("[%d][%p] RegionHeader::setNextAndSize(%p), calculated %td\n", CmiMyPe(), this, n, s);
      CmiAssert(isEmpty());
      CmiAssert(s > 0);

      nextfield = n;
      sizefield = s;
    }
    void setNextAndSize(RegionHeader * n, size_t s)
    {
      IMP_DBG("[%d][%p] RegionHeader::setNextAndSize(%p, %zu)\n", CmiMyPe(), this, n, s);
      CmiAssert(isEmpty());
      CmiAssert(n == nullptr || (const uint8_t *)this + s == (const uint8_t *)n);

      nextfield = n;
      sizefield = s;
    }

    BitCarrier occupied() const
    {
      return occupiedfield;
    }
    bool isEmpty() const
    {
      return !occupiedfield;
    }
    void setOccupied(bool o)
    {
      IMP_DBG("[%d][%p] RegionHeader::setOccupied(%u)\n", CmiMyPe(), this, (unsigned int)o);

      occupiedfield = (BitCarrier)o;
    }
    void setOccupied(BitCarrier o)
    {
      IMP_DBG("[%d][%p] RegionHeader::setOccupied(%u)\n", CmiMyPe(), this, (unsigned int)o);

      occupiedfield = o;
    }

    bool isFirst() const
    {
      return prev() == nullptr;
    }
    bool isLast() const
    {
      return next() == nullptr;
    }

    bool prevIsEmpty() const
    {
      return !isFirst() && prev()->isEmpty();
    }
    bool nextIsEmpty() const
    {
      return !isLast() && next()->isEmpty();
    }

    size_t size() const
    {
      return sizefield;
    }
    size_t usersize() const
    {
      return size() - sizeof(RegionHeader);
    }

  private:
    RegionHeader * prevfield;
    RegionHeader * nextfield;
    size_t sizefield;
    BitCarrier occupiedfield;
  };

  using Occupied = RegionHeader::Occupied;

  struct RBTree
  {
    /* Red-Black Tree
     *
     * Implementation sourced from:
     * https://web.archive.org/web/20151002014345/http://en.literateprograms.org:80/Special:Downloadcode/Red-black_tree_(C)
     *
     * Its authors have released all rights to it and placed it in the public
     * domain under the Creative Commons CC0 1.0 waiver.
     * https://creativecommons.org/publicdomain/zero/1.0/
     *
     * This is custom for the Isomempool for several reasons. Using std::map or
     * other STL containers would introduce calls to the system's malloc/free,
     * which would have a performance impact. It is not semantically
     * appropriate for an allocator to call another allocator on the critical
     * path. Implementing our own structure also allows us to tweak it to the
     * needs of the mempool. Additionally, this implementation does not require
     * the tree structure to be reconstructed during migration.
     */

    enum color : uintptr_t
    {
      RED = 0,
      BLACK = 1
    };

    struct Node
    {
      // tag structs
      struct Color
      {
        explicit Color(uintptr_t v) : value{v} { }
        Color(RBTree::color v = RED) : value{(uintptr_t)v} { }
        operator uintptr_t() const { return value; }
        operator RBTree::color() const { return (RBTree::color)value; }
      private:
        uintptr_t value;
      };

      size_t key() const
      {
        return size();
      }
      size_t size() const
      {
        return header()->size();
      }
      uint8_t * ptr() const
      {
        return (uint8_t *)location();
      }
      RegionHeader * header() const
      {
        return (RegionHeader *)location();
      }
      uintptr_t location() const
      {
        return (uintptr_t)this - sizeof(RegionHeader);
      }

      // tree node data
      Node * leftField{};
      Node * rightField{};
      Node * parentField{};
      Color colorField{};

      bool isRoot() const
      {
        return parentField == nullptr;
      }
      Node * parent()
      {
        return parentField;
      }
      Node * left()
      {
        return leftField;
      }
      Node * right()
      {
        return rightField;
      }
      RBTree::color color() const
      {
        return colorField;
      }
      void setColor(RBTree::color c)
      {
        colorField = c;
      }

      Node * grandparent()
      {
        CmiAssert(!this->isRoot());           /* Not the root node */
        CmiAssert(!this->parent()->isRoot()); /* Not child of root */
        return this->parent()->parent();
      }
      Node * sibling()
      {
        CmiAssert(!this->isRoot());           /* Root node has no sibling */
        if (this == this->parent()->left())
          return this->parent()->right();
        else
          return this->parent()->left();
      }
      Node * parent_sibling()
      {
        CmiAssert(!this->isRoot());           /* Root node has no parent-sibling */
        CmiAssert(!this->parent()->isRoot()); /* Children of root have no parent-sibling */
        return this->parent()->sibling();
      }
      Node * maximum_node()
      {
        Node * n = this;
        Node * right;
        while ((right = n->right()) != nullptr)
        {
          n = right;
        }
        return n;
      }
    };

    void pup(PUP::er & p)
    {
      pup_raw_pointer(p, root);
    }

    RBTree() : root{} { }
    RBTree(PUP::reconstruct) { }

  private:

    Node * root;

    static color node_color(const RBTree::Node * n)
    {
      return n == nullptr ? BLACK : n->color();
    }

  public:
    Node * lookup_region(size_t size, size_t alignment, size_t alignment_offset)
    {
      Node * n = this->root;
      Node * result = nullptr;
      while (n != nullptr)
      {
        signed long long comp_result = get_alignment_filler(n->ptr() + alignment_offset, alignment) + size - n->size();
        if (comp_result == 0)
        {
          return n;
        }
        else if (comp_result < 0)
        {
          result = n;
          n = n->left();
        }
        else
        {
          CmiAssert(comp_result > 0);
          n = n->right();
        }
      }
      return result;
    }

  private:
    void replace_node(Node * oldn, Node * newn)
    {
      Node * parent = oldn->parent();
      if (oldn->isRoot())
      {
        this->root = newn;
      }
      else
      {
        if (oldn == parent->left())
          parent->leftField = newn;
        else
          parent->rightField = newn;
      }
      if (newn != nullptr)
      {
        newn->parentField = oldn->parent();
      }
    }

    void rotate_left(Node * n)
    {
      Node * r = n->right();
      replace_node(n, r);
      n->rightField = r->left();
      if (r->left() != nullptr)
      {
        r->left()->parentField = n;
      }
      r->leftField = n;
      n->parentField = r;
    }
    void rotate_right(Node * n)
    {
      Node * L = n->left();
      replace_node(n, L);
      n->leftField = L->right();
      if (L->right() != nullptr)
      {
        L->right()->parentField = n;
      }
      L->rightField = n;
      n->parentField = L;
    }

    void insert_case1(Node * n)
    {
      if (n->isRoot())
        n->setColor(BLACK);
      else
        insert_case2(n);
    }
    void insert_case2(Node * n)
    {
      if (n->parent()->color() == BLACK)
        return; /* Tree is still valid */
      else
        insert_case3(n);
    }
    void insert_case3(Node * n)
    {
      if (node_color(n->parent_sibling()) == RED)
      {
        n->parent()->setColor(BLACK);
        n->parent_sibling()->setColor(BLACK);
        n->grandparent()->setColor(RED);
        insert_case1(n->grandparent());
      }
      else
      {
        insert_case4(n);
      }
    }
    void insert_case4(Node * n)
    {
      Node * parent = n->parent();
      if (n == parent->right() && parent == n->grandparent()->left())
      {
        rotate_left(parent);
        n = n->left();
      }
      else if (n == parent->left() && parent == n->grandparent()->right())
      {
        rotate_right(parent);
        n = n->right();
      }
      insert_case5(n);
    }
    void insert_case5(Node * n)
    {
      Node * parent = n->parent();
      parent->setColor(BLACK);
      n->grandparent()->setColor(RED);
      if (n == parent->left() && parent == n->grandparent()->left())
      {
        rotate_right(n->grandparent());
      }
      else
      {
        CmiAssert(n == parent->right() && parent == n->grandparent()->right());
        rotate_left(n->grandparent());
      }
    }

  public:
    void insert(Node * inserted_node)
    {
      if (this->root == nullptr)
      {
        this->root = inserted_node;
        inserted_node->parentField = nullptr;
        inserted_node->setColor(BLACK);
      }
      else
      {
        Node * n = this->root;
        while (1)
        {
          signed long long comp_result = inserted_node->key() - n->key();
          if (comp_result < 0)
          {
            if (n->left() == nullptr)
            {
              n->leftField = inserted_node;
              break;
            }
            else
            {
              n = n->left();
            }
          }
          else
          {
            if (n->right() == nullptr)
            {
              n->rightField = inserted_node;
              break;
            }
            else
            {
              n = n->right();
            }
          }
        }
        inserted_node->parentField = n;
        insert_case2(inserted_node);
      }

#ifdef VERIFY_RBTREE
      this->verify_properties();
#endif
    }

  private:
    void delete_case1(Node * n)
    {
      if (n->isRoot())
        return;
      else
        delete_case2(n);
    }
    void delete_case2(Node * n)
    {
      if (node_color(n->sibling()) == RED)
      {
        Node * parent = n->parent();
        parent->setColor(RED);
        n->sibling()->setColor(BLACK);
        if (n == parent->left())
          rotate_left(parent);
        else
          rotate_right(parent);
      }
      delete_case3(n);
    }
    void delete_case3(Node * n)
    {
      Node * parent = n->parent();
      if (parent->color() == BLACK && node_color(n->sibling()) == BLACK &&
          node_color(n->sibling()->left()) == BLACK && node_color(n->sibling()->right()) == BLACK)
      {
        n->sibling()->setColor(RED);
        delete_case1(parent);
      }
      else
        delete_case4(n);
    }
    void delete_case4(Node * n)
    {
      Node * parent = n->parent();
      if (parent->color() == RED && node_color(n->sibling()) == BLACK &&
          node_color(n->sibling()->left()) == BLACK && node_color(n->sibling()->right()) == BLACK)
      {
        n->sibling()->setColor(RED);
        parent->setColor(BLACK);
      }
      else
        delete_case5(n);
    }
    void delete_case5(Node * n)
    {
      Node * parent = n->parent();
      if (n == parent->left() && node_color(n->sibling()) == BLACK &&
          node_color(n->sibling()->left()) == RED && node_color(n->sibling()->right()) == BLACK)
      {
        n->sibling()->setColor(RED);
        n->sibling()->left()->setColor(BLACK);
        rotate_right(n->sibling());
      }
      else if (n == parent->right() && node_color(n->sibling()) == BLACK &&
               node_color(n->sibling()->right()) == RED && node_color(n->sibling()->left()) == BLACK)
      {
        n->sibling()->setColor(RED);
        n->sibling()->right()->setColor(BLACK);
        rotate_left(n->sibling());
      }
      delete_case6(n);
    }
    void delete_case6(Node * n)
    {
      Node * parent = n->parent();
      n->sibling()->setColor(parent->color());
      parent->setColor(BLACK);
      if (n == parent->left())
      {
        CmiAssert(node_color(n->sibling()->right()) == RED);
        n->sibling()->right()->setColor(BLACK);
        rotate_left(parent);
      }
      else
      {
        CmiAssert(node_color(n->sibling()->left()) == RED);
        n->sibling()->left()->setColor(BLACK);
        rotate_right(parent);
      }
    }

  public:
    void remove(Node * n)
    {
      CmiAssert(n->color() == BLACK || n->color() == RED);

      auto remove_internal = [this](Node * n)
      {
        CmiAssert(n->left() == nullptr || n->right() == nullptr);

        Node * child = n->right() == nullptr ? n->left() : n->right();
        if (node_color(n) == BLACK)
        {
          n->setColor(node_color(child));
          delete_case1(n);
        }
        replace_node(n, child);
        if (n->isRoot() && child != nullptr)  // root should be black
          child->setColor(BLACK);

#ifdef VERIFY_RBTREE
        this->verify_properties();
#endif
      };

      if (n->left() != nullptr && n->right() != nullptr)
      {
        /* Copy key/value from predecessor and then delete it instead */
        Node * pred = n->left()->maximum_node();
        remove_internal(pred);
        replace_node(n, pred);
        pred->setColor(n->color());
        pred->leftField = n->leftField;
        pred->rightField = n->rightField;
        if (pred->left())
          pred->left()->parentField = pred;
        if (pred->right())
          pred->right()->parentField = pred;
      }
      else
      {
        remove_internal(n);
      }
    }

#ifdef VERIFY_RBTREE
  private:
    static void verify_property_1(Node * n)
    {
      CmiAssert(node_color(n) == RED || node_color(n) == BLACK);
      if (n == nullptr) return;
      verify_property_1(n->left());
      verify_property_1(n->right());
    }
    static void verify_property_2(Node * root)
    {
      CmiAssert(node_color(root) == BLACK);
    }
    static void verify_property_4(Node * n)
    {
      if (node_color(n) == RED)
      {
        CmiAssert(node_color(n->left()) == BLACK);
        CmiAssert(node_color(n->right()) == BLACK);
        CmiAssert(n->parent()->color() == BLACK);
      }
      if (n == nullptr) return;
      verify_property_4(n->left());
      verify_property_4(n->right());
    }
    static void verify_property_5_helper(Node * n, int black_count, int * path_black_count)
    {
      if (node_color(n) == BLACK)
      {
        black_count++;
      }
      if (n == nullptr)
      {
        if (*path_black_count == -1)
        {
          *path_black_count = black_count;
        }
        else
        {
          CmiAssert(black_count == *path_black_count);
        }
        return;
      }
      verify_property_5_helper(n->left(), black_count, path_black_count);
      verify_property_5_helper(n->right(), black_count, path_black_count);
    }
    static void verify_property_5(Node * root)
    {
      int black_count_path = -1;
      verify_property_5_helper(root, 0, &black_count_path);
    }

  protected:
    void verify_properties()
    {
      verify_property_1(this->root);
      verify_property_2(this->root);
      /* Property 3 is implicit */
      verify_property_4(this->root);
      verify_property_5(this->root);
    }
#endif
  };

  using EmptyRegion = RBTree::Node;

  static constexpr size_t minimum_alignment = ALIGN_BYTES;

  // roughly the threshold where bookkeeping for a free region takes more space than the region itself
  static constexpr size_t minimum_empty_region_size = sizeof(RegionHeader) + sizeof(EmptyRegion);
  static_assert(minimum_empty_region_size > 0, "region sizes cannot be zero");
  static_assert(minimum_empty_region_size >= sizeof(RegionHeader), "regions must allow space for a header");

  Isomempool(uint8_t * s, uint8_t * e)
    : empty_tree{}, first_region{}, last_region{}, backend{s, e}
  {
    IMP_DBG("[%d][%p] Isomempool::Isomempool(%p, %p)\n", CmiMyPe(), this, s, e);
  }
  Isomempool(PUP::reconstruct pr)
    : empty_tree{pr}, backend{pr}
  {
    IMP_DBG("[%d][%p] Isomempool::Isomempool(PUP::reconstruct)\n", CmiMyPe(), this);
  }

  void activate_random_access_heap()
  {
  }

  ~Isomempool()
  {
    IMP_DBG("[%d][%p] Isomempool::~Isomempool()\n", CmiMyPe(), this);
  }

  void print_contents() const
  {
    CmiPrintf("[%d][%p] Isomempool::print_contents()\n", CmiMyPe(), this);

    backend.print();

    if (first_region == nullptr)
      return;

    const RegionHeader * node = first_region;
    const RegionHeader * next = node->next();

    while (next != nullptr)
    {
      const size_t size = (const uint8_t *)next - (const uint8_t *)node;
      CmiPrintf("  %d %p-%p %zu %zu\n", (int)node->occupied(), node, next, size, node->size());

      node = next;
      next = node->next();
    }

    const size_t size = (const uint8_t *)backend.allocated_extent - (const uint8_t *)node;
    CmiPrintf("  %d %p-%p %zu %zu\n", (int)node->occupied(), node, backend.allocated_extent, size, node->size());
  }

  static size_t get_alignment_filler(const void * ptr, size_t alignment)
  {
    CmiAssert((alignment & (alignment-1)) == 0); // assuming alignment is a power of two or zero
    return (alignment - ((uintptr_t)ptr & (alignment-1))) & (alignment-1);
    // return (alignment - ptr % alignment) % alignment; // non-power of two version
  }

  static size_t minimum_alignment_filler(const void * ptr)
  {
    return get_alignment_filler(ptr, minimum_alignment);
  }

private:

  // canonical data
  isommap backend;
  RBTree empty_tree;
  RegionHeader * first_region, * last_region;

  void setFirstRegion(RegionHeader * p)
  {
    IMP_DBG("[%d][%p] Isomempool::setFirstRegion(%p)\n", CmiMyPe(), this, p);

    CmiAssert(p->prev() == nullptr);
    first_region = p;
  }
  void setLastRegion(RegionHeader * p)
  {
    IMP_DBG("[%d][%p] Isomempool::setLastRegion(%p)\n", CmiMyPe(), this, p);

    CmiAssert(p->next() == nullptr);
    last_region = p;
  }

  void insertEmptyRegion(RegionHeader * header)
  {
    CmiAssert(header == last_region || (const uint8_t *)header + minimum_empty_region_size <= (const uint8_t *)header->next());

    auto empty_region = new (header + 1) EmptyRegion{};
    empty_tree.insert(empty_region);

    CmiAssert((const uint8_t *)header + header->size() <= backend.allocated_extent);
    CmiAssert(header == last_region || (const uint8_t *)header + header->size() == (const uint8_t *)header->next());
  }

  void setEmptyRegionNext(RegionHeader * header, RegionHeader * next)
  {
    const auto empty_region = (EmptyRegion *)(header + 1);
    empty_tree.remove(empty_region);
    header->setNextAndSize(next);
    insertEmptyRegion(header);
  }

  void setRegionNext(RegionHeader * header, RegionHeader * next)
  {
    if (header->isEmpty())
      setEmptyRegionNext(header, next);
    else
      header->setNext(next);
  }

  void * allocFromEmptyRegion(const size_t size, EmptyRegion * const empty_region, const size_t alignment_filler)
  {
    const size_t region_size = empty_region->size();
    RegionHeader * const empty_header = empty_region->header();
    empty_tree.remove(empty_region);

    IMP_DBG("[%d][%p] Isomempool::allocFromEmptyRegion(%zu, %p, %zu) by erasing and filling {%p, %zu}\n",
            CmiMyPe(), this, size, empty_region, alignment_filler, empty_header, region_size);

    CmiAssert(!empty_header->prevIsEmpty());
    CmiAssert(!empty_header->nextIsEmpty());

    // Extract information from the empty region's header before it is (potentially) overwritten.
    RegionHeader * const empty_header_prev = empty_header->prev();
    RegionHeader * const empty_header_next = empty_header->next();

    // Prepare the fields for the new occupied region we will construct.
    const auto header_ptr = (RegionHeader *)((uint8_t *)empty_header + alignment_filler);
    RegionHeader * header_prev;
    RegionHeader * header_next;

    if (alignment_filler >= minimum_empty_region_size + minimum_alignment_filler(empty_header))
    {
      // Record a new empty region preceding the new occupied region.

      IMP_DBG("[%d][%p] Isomempool::allocFromEmptyRegion(%zu, %p, %zu) inserting empty region {%p, %zu} (left)\n",
              CmiMyPe(), this, size, empty_header, alignment_filler, empty_header, alignment_filler);

      header_prev = new (empty_header) RegionHeader{empty_header_prev, header_ptr, alignment_filler, Occupied{false}};

      if (empty_header_prev == nullptr)
        setFirstRegion(header_prev);
      else
        setRegionNext(empty_header_prev, header_prev);

      insertEmptyRegion(header_prev);

      CmiAssert(!header_prev->prevIsEmpty());
    }
    else
    {
      // Directly link the new occupied region to the erased empty region's left neighbor.

      header_prev = empty_header_prev;

      if (empty_header_prev == nullptr)
        setFirstRegion(header_ptr);

      if (header_prev != nullptr)
        setRegionNext(header_prev, header_ptr);
    }

    size_t size_difference = region_size - size - alignment_filler;
    auto follow_ptr = (uint8_t *)header_ptr + size;
    const size_t follow_alignment_filler = minimum_alignment_filler(follow_ptr);
    follow_ptr += follow_alignment_filler;

    if (size_difference >= minimum_empty_region_size + follow_alignment_filler)
    {
      // Record a new empty region following the new occupied region.

      size_difference -= follow_alignment_filler;

      IMP_DBG("[%d][%p] Isomempool::allocFromEmptyRegion(%zu, %p, %zu) inserting empty region {%p, %zu} (right)\n",
              CmiMyPe(), this, size, empty_region, alignment_filler, follow_ptr, size_difference);

      header_next = new (follow_ptr) RegionHeader{header_ptr, empty_header_next, size_difference, Occupied{false}};

      if (empty_header_next == nullptr)
        setLastRegion(header_next);
      else
        empty_header_next->setPrev(header_next);

      insertEmptyRegion(header_next);

      CmiAssert(!header_next->nextIsEmpty());
    }
    else
    {
      // Directly link the new occupied region to the erased empty region's right neighbor.

      header_next = empty_header_next;

      if (empty_header_next == nullptr)
        setLastRegion(header_ptr);
        
      if (header_next != nullptr)
        header_next->setPrev(header_ptr);
    }

    // Instantiate the new region.
    const auto header = new (header_ptr) RegionHeader{header_prev, header_next, size, Occupied{true}};
    return header + 1;
  }

  void * allocByExtending(const size_t size, const size_t alignment, const size_t alignment_offset)
  {
    IMP_DBG("[%d][%p] Isomempool::allocByExtending(%zu, %zu, %zu)\n", CmiMyPe(), this, size, alignment, alignment_offset);

    // Find the left bound of where we can place the new region.

    uint8_t * used_extent;
    RegionHeader * header_prev;

    if (last_region == nullptr)
    {
      CmiAssert(first_region == nullptr);

      used_extent = backend.start;
      header_prev = nullptr;
    }
    else if (last_region->isEmpty())
    {
      used_extent = (uint8_t *)last_region;
      header_prev = last_region->prev();

      auto empty_region = (EmptyRegion *)(last_region + 1);
      IMP_DBG("[%d][%p] Isomempool::allocByExtending(%zu, %zu, %zu) erasing empty region {%p, %zu} (left)\n",
              CmiMyPe(), this, size, alignment, alignment_offset, last_region, empty_region->size());
      empty_tree.remove(empty_region);
    }
    else
    {
      used_extent = (uint8_t *)last_region + last_region->size();
      header_prev = last_region;
    }

    // Determine how much space we need to mmap, and mmap it.

    const size_t alignment_filler = get_alignment_filler(used_extent + alignment_offset, alignment);
    const auto header_ptr = (RegionHeader *)(used_extent + alignment_filler);

    const size_t allocated_free_space = backend.allocated_extent - used_extent;
    CmiAssert(allocated_free_space < size); // should not be in this function if this fails
    const size_t space_needed = size - allocated_free_space;
    const size_t alloc_size = CMIALIGN(space_needed, slotsize);
    const size_t space_remaining = backend.end - used_extent;
    if (space_remaining < alloc_size)
      return nullptr;

    void * const mapped = map_global_memory(backend.allocated_extent, alloc_size);
    if (mapped == nullptr)
      return nullptr;
    backend.allocated_extent += alloc_size;

    // Manage our left neighbor, if any.

    const size_t used_extent_alignment_filler = minimum_alignment_filler(used_extent);
    if (alignment_filler >= minimum_empty_region_size + used_extent_alignment_filler)
    {
      // Record a new empty region preceding the new occupied region.

      const size_t empty_size = alignment_filler - used_extent_alignment_filler;
      uint8_t * const empty_header_ptr = used_extent + used_extent_alignment_filler;

      IMP_DBG("[%d][%p] Isomempool::allocByExtending(%zu, %zu, %zu) inserting empty region {%p, %zu} (left)\n",
              CmiMyPe(), this, size, alignment, alignment_offset, empty_header_ptr, empty_size);

      auto empty_header = new (empty_header_ptr) RegionHeader{header_prev, header_ptr, empty_size, Occupied{false}};

      if (header_prev != nullptr)
        header_prev->setNext(empty_header);
      else
        setFirstRegion(empty_header);

      insertEmptyRegion(empty_header);

      header_prev = empty_header;
    }
    else
    {
      // There is not enough wasted space to qualify for a new empty region to the left.
      // Directly link to the occupied region to the left if present, or mark as first.

      if (header_prev != nullptr)
        header_prev->setNext(header_ptr);
      else
        setFirstRegion(header_ptr);
    }

    // Manage our right neighbor, if any.

    auto follow_ptr = (uint8_t *)header_ptr + size;
    const size_t follow_alignment_filler = minimum_alignment_filler(follow_ptr);
    follow_ptr += follow_alignment_filler;
    size_t size_difference = (const uint8_t *)backend.allocated_extent - (const uint8_t *)follow_ptr;

    RegionHeader * header_next;

    if (size_difference >= minimum_empty_region_size)
    {
      // Record a new empty region following the new occupied region.

      IMP_DBG("[%d][%p] Isomempool::allocByExtending(%zu, %zu, %zu) inserting empty region {%p, %zu} (right)\n",
              CmiMyPe(), this, size, alignment, alignment_offset, follow_ptr, size_difference);

      header_next = new (follow_ptr) RegionHeader{header_ptr, nullptr, size_difference, Occupied{false}};

      setLastRegion(header_next);
      insertEmptyRegion(header_next);
    }
    else
    {
      // There is not enough space for a new empty region to the right.
      // Mark the new occupied region as the last in the pool.

      header_next = nullptr;

      setLastRegion(header_ptr);
    }

    // Instantiate the new region.
    const auto header = new (header_ptr) RegionHeader{header_prev, header_next, size, Occupied{true}};
    return header + 1;
  }

public:

  void * alloc(const size_t size, const size_t alignment = minimum_alignment, const size_t alignment_offset = 0)
  {
    CmiLock(backend.lock);

    IMP_DBG("[%d][%p] Isomempool::alloc(%zu, %zu, %zu)...\n", CmiMyPe(), this, size, alignment, alignment_offset);

    const size_t practical_size = size + sizeof(RegionHeader);
    const size_t practical_alignment_offset = alignment_offset + sizeof(RegionHeader);

    // Find the smallest empty region that can fit the data.
    EmptyRegion * node = empty_tree.lookup_region(practical_size, alignment, practical_alignment_offset);

    // Perform the allocation.
    void * ret = node != nullptr
                 ? allocFromEmptyRegion(practical_size, node, get_alignment_filler(node->ptr() + practical_alignment_offset, alignment))
                 : allocByExtending(practical_size, alignment, practical_alignment_offset);

    IMP_DBG("[%d][%p] Isomempool::alloc(%zu, %zu, %zu) returning %p {header = %p}\n",
            CmiMyPe(), this, size, alignment, alignment_offset, ret, (const uint8_t *)ret - sizeof(RegionHeader));

    CmiUnlock(backend.lock);

    return ret;
  }

  size_t length(void * user_ptr) const
  {
    const auto header = (const RegionHeader *)user_ptr - 1;
    return header->usersize();
  }

  void free(void * user_ptr)
  {
    CmiLock(backend.lock);

    CmiAssert(backend.isInRange(user_ptr));
    CmiAssert(backend.isMapped(user_ptr));

    const auto orig_header = (RegionHeader *)user_ptr - 1;
    CmiAssert(!orig_header->isEmpty());

    IMP_DBG("[%d][%p] Isomempool::free(%p)... {header = %p}\n", CmiMyPe(), this, user_ptr, orig_header);

    RegionHeader * const orig_header_prev = orig_header->prev();
    RegionHeader * const orig_header_next = orig_header->next();

    RegionHeader * header = orig_header;
    RegionHeader * header_prev;
    RegionHeader * header_next;
    size_t header_size = (orig_header_next != nullptr
                           ? (const uint8_t *)orig_header_next
                           : backend.allocated_extent)
                           - (const uint8_t *)orig_header;

    if (orig_header->nextIsEmpty())
    {
      // Merge with the free region to the right.

      RegionHeader * const orig_header_next_next = orig_header_next->next();

      const auto empty_region = (EmptyRegion *)(orig_header_next + 1);
      IMP_DBG("[%d][%p] Isomempool::free(%p) erasing empty region {%p, %zu} (right)\n",
              CmiMyPe(), this, user_ptr, orig_header_next, empty_region->size());
      empty_tree.remove(empty_region);

      header_next = orig_header_next_next;
      header_size += orig_header_next->size();
    }
    else
    {
      // Directly link to the occupied region to the right.

      header_next = orig_header_next;
    }

    if (orig_header->prevIsEmpty())
    {
      // Merge with the free region to the left.

      RegionHeader * const orig_header_prev_prev = orig_header_prev->prev();
      const size_t orig_header_prev_size = orig_header_prev->size();

      auto empty_region = (EmptyRegion *)(orig_header_prev + 1);
      IMP_DBG("[%d][%p] Isomempool::free(%p) erasing empty region {%p, %zu} (left)\n",
              CmiMyPe(), this, user_ptr, orig_header_prev, empty_region->size());
      empty_tree.remove(empty_region);

      header = orig_header_prev;
      header_prev = orig_header_prev_prev;
      header_size += orig_header_prev_size;
    }
    else
    {
      // Try reclaiming slop space to the left.

      header_prev = orig_header_prev;

      uint8_t * available_ptr;
      if (orig_header_prev != nullptr)
      {
        CmiAssert(!orig_header_prev->isEmpty());
        available_ptr = (uint8_t *)orig_header_prev + orig_header_prev->size();
        available_ptr += minimum_alignment_filler(available_ptr);
      }
      else
      {
        available_ptr = backend.start;
        CmiAssert(minimum_alignment_filler(available_ptr) == 0);
      }

      CmiAssert(available_ptr <= (const uint8_t *)header);

      header_size += (const uint8_t *)header - available_ptr;
      header = (RegionHeader *)available_ptr;
    }

    if (header_size < minimum_empty_region_size)
    {
      // There is not enough space for a new empty region.
      // Append it as slop to the previous occupied region.

      IMP_DBG("[%d][%p] Isomempool::free(%p) remnant too small {%p, %zu}, appending to %p on left (%p on right)\n",
              CmiMyPe(), this, user_ptr, header, header_size, header_prev, header_next);

      if (header_prev == nullptr)
        setFirstRegion(header_next);
      else
        header_prev->setNext(header_next);

      if (header_next == nullptr)
        setLastRegion(header_prev);
      else
        header_next->setPrev(header_prev);
    }
    else
    {
      // Add a new empty region containing the space from the freed region and any surrounding free space.

      IMP_DBG("[%d][%p] Isomempool::free(%p) inserting empty region {%p, %zu}\n",
              CmiMyPe(), this, user_ptr, header, header_size);

      header = new (header) RegionHeader{header_prev, header_next, header_size, Occupied{false}};

      if (header_prev == nullptr)
        setFirstRegion(header);
      else
        setRegionNext(header_prev, header);

      if (header_next == nullptr)
        setLastRegion(header);
      else
        header_next->setPrev(header);

      // Record the new empty region.
      CmiAssert(!header->prevIsEmpty() && !header->nextIsEmpty());

      insertEmptyRegion(header);
    }

    CmiUnlock(backend.lock);
  }

  void pup(PUP::er & p)
  {
    p | backend;

    pup_raw_pointer(p, first_region);
    pup_raw_pointer(p, last_region);
    p | empty_tree;
  }

  void * calloc(size_t nelem, size_t size)
  {
    const size_t bytesize = nelem * size;
    void * ret = alloc(bytesize);
    if (ret != nullptr)
      memset(ret, 0, bytesize);
    return ret;
  }

  void * realloc(void * ptr, size_t size)
  {
    void *ret = alloc(size);
    if (ret != nullptr && ptr != nullptr)
    {
      size_t copysize = length(ptr);
      if (copysize > size)
        copysize = size;
      if (copysize > 0)
        memcpy(ret, ptr, copysize);
    }
    if (ptr)
      free(ptr);
    return ret;
  }
};

using Mempool = isomalloc_dlmalloc;

/************** External interface ***************/

#ifndef MALLOC_ALIGNMENT
#define MALLOC_ALIGNMENT ALIGN_BYTES
#endif

static inline size_t isomalloc_internal_validate_align(size_t align)
{
  /* make sure alignment is power of 2 */
  if ((align & (align - 1)) != 0)
  {
    size_t a = MALLOC_ALIGNMENT * 2;
    while ((unsigned long)a < (unsigned long)align) a <<= 1;
    return a;
  }
  return align;
}

int CmiIsomallocEnabled()
{
  return isomallocStart != nullptr;
}

/*Return true if this address is in the region managed by isomalloc*/
int CmiIsomallocInRange(void * addr)
{
  if (isomallocStart == NULL) return 0; /* There is no range we manage! */
  return (addr == NULL) || (pointer_ge((uint8_t *)addr, isomallocStart) &&
                            pointer_lt((uint8_t *)addr, isomallocEnd));
}

#if CMK_CONVERSE_MPI && (CMK_MEM_CHECKPOINT || CMK_MESSAGE_LOGGING)
extern int num_workpes, total_pes;
#endif

void CmiIsomallocInit(char ** argv)
{
#if CMK_CONVERSE_MPI && (CMK_MEM_CHECKPOINT || CMK_MESSAGE_LOGGING)
  if (num_workpes != total_pes)
  {
    disable_isomalloc("+wp is active and spare processor init code is WIP");
    return;
  }
#endif

#if CMK_NO_ISO_MALLOC
  disable_isomalloc("unsupported platform");
#else
  if (CmiGetArgFlagDesc(argv, "+noisomalloc", "disable isomalloc"))
  {
    disable_isomalloc("specified by user");
    return;
  }
#if CMK_MMAP_PROBE
  _mmap_probe = 1;
#elif CMK_MMAP_TEST
  _mmap_probe = 0;
#endif
  if (CmiGetArgFlagDesc(argv, "+isomalloc_probe",
                        "call mmap to probe the largest available isomalloc region"))
    _mmap_probe = 1;
  if (CmiGetArgFlagDesc(
          argv, "+isomalloc_test",
          "mmap test common areas for the largest available isomalloc region"))
    _mmap_probe = 0;
#if __FAULT__
  if (CmiGetArgFlagDesc(argv, "+restartisomalloc",
                        "restarting isomalloc on this processor after a crash"))
    CmiIsomallocRestart = 1;
#endif
  if (!init_map())
  {
    disable_isomalloc("mmap() does not work");
  }
  else
  {
    CmiIsomallocInitExtent(argv);
  }
#endif
}

/* Contexts */

CmiIsomallocContext CmiIsomallocContextCreate(int myunit, int numunits)
{
  if (isomallocStart == nullptr)
    return { nullptr };

  uint8_t * unrounded_start = get_space_partition(isomallocStart, isomallocEnd, myunit, numunits);
  uint8_t * unrounded_end = get_space_partition(isomallocStart, isomallocEnd, myunit+1, numunits);
  auto start = (uint8_t *)CMIALIGN((uintptr_t)unrounded_start, pagesize);
  auto end = (uint8_t *)CMIALIGN((uintptr_t)unrounded_end - (pagesize-1), pagesize);
  return { new Mempool{start, end} };
}

void CmiIsomallocContextDelete(CmiIsomallocContext ctx)
{
  auto pool = (Mempool *)ctx.opaque;
#if ISOMEMPOOL_DEBUG
  pool->print_contents();
#endif
  delete pool;
}

void CmiIsomallocContextPup(pup_er cpup, CmiIsomallocContext * ctxptr)
{
  PUP::er & p = *(PUP::er *)cpup;

  if (p.isUnpacking())
  {
    ctxptr->opaque = new Mempool{PUP::reconstruct{}};
  }

  auto pool = (Mempool *)ctxptr->opaque;

  p | *pool;

  if (p.isDeleting())
  {
    delete pool;
    ctxptr->opaque = nullptr;
  }
}

void CmiIsomallocContextEnableRandomAccess(CmiIsomallocContext ctx)
{
  auto pool = (Mempool *)ctx.opaque;
  pool->activate_random_access_heap();
}

void CmiIsomallocContextJustMigrated(CmiIsomallocContext ctx)
{
  auto pool = (Mempool *)ctx.opaque;
  pool->backend.JustMigrated();
}

void CmiIsomallocEnableRDMA(CmiIsomallocContext ctx, int enable)
{
  auto pool = (Mempool *)ctx.opaque;
  pool->backend.EnableRDMA(enable);
}

CmiIsomallocRegion CmiIsomallocContextGetUsedExtent(CmiIsomallocContext ctx)
{
  auto pool = (Mempool *)ctx.opaque;
  return CmiIsomallocRegion{pool->backend.start, pool->backend.allocated_extent};
}

void * CmiIsomallocContextMalloc(CmiIsomallocContext ctx, size_t size)
{
  auto pool = (Mempool *)ctx.opaque;
  return pool->alloc(size);
}

void * CmiIsomallocContextMallocAlign(CmiIsomallocContext ctx, size_t align, size_t size)
{
  auto pool = (Mempool *)ctx.opaque;
  return pool->alloc(size, isomalloc_internal_validate_align(align));
}

void * CmiIsomallocContextCalloc(CmiIsomallocContext ctx, size_t nelem, size_t size)
{
  auto pool = (Mempool *)ctx.opaque;
  return pool->calloc(nelem, size);
}

void * CmiIsomallocContextRealloc(CmiIsomallocContext ctx, void * ptr, size_t size)
{
  auto pool = (Mempool *)ctx.opaque;
  return pool->realloc(ptr, size);
}

void CmiIsomallocContextFree(CmiIsomallocContext ctx, void * ptr)
{
  if (ptr == nullptr)
    return;

  auto pool = (Mempool *)ctx.opaque;
  pool->free(ptr);
}

size_t CmiIsomallocContextGetLength(CmiIsomallocContext ctx, void * ptr)
{
  auto pool = (Mempool *)ctx.opaque;
  return pool->length(ptr);
}

void * CmiIsomallocContextPermanentAlloc(CmiIsomallocContext ctx, size_t size)
{
  CmiMemoryIsomallocDisablePush();

  auto pool = (Mempool *)ctx.opaque;
  auto ret = pool->backend.permanent_alloc(size);

  CmiMemoryIsomallocDisablePop();

  return ret;
}

void * CmiIsomallocContextPermanentAllocAlign(CmiIsomallocContext ctx, size_t align, size_t size)
{
  CmiMemoryIsomallocDisablePush();

  auto pool = (Mempool *)ctx.opaque;
  size_t real_align = isomalloc_internal_validate_align(align);
  auto ret = pool->backend.permanent_alloc(size, real_align);

  CmiMemoryIsomallocDisablePop();

  return ret;
}
