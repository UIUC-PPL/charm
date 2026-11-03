/**************************************************************************
Isomalloc <-> Reconverse glue.

Reconverse owns user-level threads but not Isomalloc: Isomalloc's context
serialization is written against PUP, which belongs to the layer above
Reconverse, so the two cannot simply be merged. Instead Reconverse declares
the handful of operations a migratable thread needs (CthIsomallocOps) and
this file registers Charm++'s Isomalloc as the provider.

Two directions of adaptation live here:

  - Down: the CthIsomallocOps table, plus registerIsomallocInit so that
    CmiIsomallocInit runs at the right point in Reconverse's per-PE startup.

  - Up: CthPup(pup_er, ...), the signature TCharm and the rest of Charm++
    call, wrapping Reconverse's serializer-agnostic CthPupThread.

Only built for Reconverse targets; the classic Converse build has all of this
inside threads.C and isomalloc.C already.
 *************************************************************************/

#include "converse.h"
#include "conv-autoconfig.h"


#include "memory-isomalloc.h"
#include "pup.h"
#include "pup_c.h"
#include "pup_stl.h"

#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <string>
#include <vector>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <link.h>
#endif

/* MSVC has no weak symbols. The linker's /alternatename gives the same
   fallback: a reference to the first name binds to the second unless some
   object in the link defines the first. */
#if defined(_MSC_VER) && !defined(__clang__)
#define CMI_HAS_WEAK 0
#else
#define CMI_HAS_WEAK 1
#endif

/****************** Down: Isomalloc as a thread-stack provider **************/

/* Defined in isomalloc.C. Declared here rather than in memory-isomalloc.h
   because that header is extern "C" and isomalloc.C's definition is not. */
void CmiIsomallocInit(char ** argv);

/* Isomalloc calls into the memory module to keep its own bookkeeping
   allocations out of the context it is servicing, and the glue below activates
   a context for malloc interception. Both live in memory-isomalloc.C, which is
   only linked when the program asked for `charmc -memory isomalloc`. Weak
   no-ops stand in otherwise, so that Isomalloc is usable for thread stacks
   even in a program whose heap is not intercepted; the real definitions
   override these whenever the memory module is present. */
#if CMI_HAS_WEAK
CLINKAGE __attribute__((weak)) void CmiMemoryIsomallocDisablePush(void) {}
CLINKAGE __attribute__((weak)) void CmiMemoryIsomallocDisablePop(void) {}
CLINKAGE __attribute__((weak)) void
CmiMemoryIsomallocContextActivate(CmiIsomallocContext ctx) {}
#else
CLINKAGE void CmiMemoryIsomallocDisablePush(void);
CLINKAGE void CmiMemoryIsomallocDisablePop(void);
CLINKAGE void CmiMemoryIsomallocContextActivate(CmiIsomallocContext ctx);
CLINKAGE void CmiMemoryIsomallocDisablePush_default(void) {}
CLINKAGE void CmiMemoryIsomallocDisablePop_default(void) {}
CLINKAGE void CmiMemoryIsomallocContextActivate_default(CmiIsomallocContext ctx) {}
#pragma comment(linker, "/alternatename:CmiMemoryIsomallocDisablePush=CmiMemoryIsomallocDisablePush_default")
#pragma comment(linker, "/alternatename:CmiMemoryIsomallocDisablePop=CmiMemoryIsomallocDisablePop_default")
#pragma comment(linker, "/alternatename:CmiMemoryIsomallocContextActivate=CmiMemoryIsomallocContextActivate_default")
#endif

static int iso_enabled(void) { return CmiIsomallocEnabled(); }

/* Isomalloc puts a migratable thread's stack and heap at addresses every
   process agrees on, but it has no say over where the program's own code
   lands. A suspended thread's stack holds return addresses into that code, so
   if the loader placed it somewhere else in the destination process, resuming
   the thread jumps into whatever is there now -- a segmentation fault with no
   other symptom and nothing pointing at the cause. Say so once, at the moment
   the first migratable thread is created, and only where it could bite: a
   single-process job never migrates anything between address spaces. The
   refusal itself happens later, in pupLoadedObjects below, when a thread
   packed by a process with a different layout is about to be unpacked. */
static void warnOnceAboutAslr(void)
{
  static bool warned = false;
  extern int CmiIsomallocAddressSpaceIsRandomized(void);

  if (warned || CmiMyPe() != 0 || CmiNumNodes() <= 1)
    return;
  warned = true;

  if (!CmiIsomallocAddressSpaceIsRandomized())
    return;

  /* stderr, not stdout: when a migrated thread then crashes the process, a
     piped stdout (the shape of a CI log) is discarded with it, and the one
     line that explains the crash would be lost. */
#if defined(__APPLE__)
  CmiError("Isomalloc> Warning: address space randomization is enabled, and "
            "this job just created a migratable thread. Moving one between "
            "processes needs the program's code at the same address in both. "
            "macOS has no setarch; a launcher can start the processes with "
            "randomization off (posix_spawn with _POSIX_SPAWN_DISABLE_ASLR, as "
            "classic charmrun did), but lcrun does not, so under it these "
            "threads must not migrate between processes (migration between "
            "PEs of one process is fine).\n");
#else
  CmiError("Isomalloc> Warning: address space randomization is enabled (or "
            "could not be checked), and this job just created a migratable "
            "thread. Moving one between processes needs the program's code at "
            "the same address in both; run under `setarch -R`, or disable "
            "randomize_va_space, if these threads will migrate.\n");
#endif
}

static void * iso_permanentAllocAlign(CmiIsomallocContext ctx, size_t align,
                                      size_t size)
{
  warnOnceAboutAslr();
  return CmiIsomallocContextPermanentAllocAlign(ctx, align, size);
}

static void iso_contextDelete(CmiIsomallocContext ctx)
{
  CmiIsomallocContextDelete(ctx);
}

/* The stream's `user` is the PUP::er that Charm++ is driving; recovering it
   is what lets the context be pupped with the real framework even though
   Reconverse only knows about the byte-stream shape. */
/* A migratable thread's stack holds return addresses into the program's
   code: the executable, libreconverse, and whatever else the loader mapped.
   The thread can resume only in a process that has each of those at the same
   address. The load address of every object this process has loaded is
   recorded with a thread's context when the context is packed, and a context
   whose record disagrees with this process is refused when it is unpacked:
   that is a migration between processes, or a restart from a checkpoint, with
   address space layout randomization on, and resuming the thread would jump
   into whatever this process has at the old addresses. The list is collected
   once, at init, before any thread can be packed. */
struct LoadedObject
{
  std::string name;
  unsigned long long base;
};

static std::vector<LoadedObject> loadedObjects;

#if defined(__linux__)
static int collectLoadedObject(struct dl_phdr_info * info, size_t, void * data)
{
  auto objects = (std::vector<LoadedObject> *)data;
  const std::string name = info->dlpi_name ? info->dlpi_name : "";
  /* The vdso is left out: its functions neither block nor call back, so no
     suspended thread's stack points into it, and it is the entry right after
     the program, so a mismatch report would name it instead of the object
     that matters. Newer glibc lists it as linux-vdso.so.1 or linux-gate.so.1;
     glibc 2.17 (CentOS and RHEL 7) lists it with an empty name, the same as
     the program's own entry. The program is always the first entry visited,
     so a later unnamed entry is the vdso and is skipped. */
  if (name.compare(0, 10, "linux-vdso") == 0 || name.compare(0, 10, "linux-gate") == 0)
    return 0;
  if (name.empty() && !objects->empty())
    return 0;
  objects->push_back({name, (unsigned long long)info->dlpi_addr});
  return 0;
}
#endif

static void collectLoadedObjects(void)
{
  loadedObjects.clear();
#if defined(__APPLE__)
  const uint32_t n = _dyld_image_count();
  for (uint32_t i = 0; i < n; i++)
    loadedObjects.push_back({_dyld_get_image_name(i),
                             (unsigned long long)(uintptr_t)_dyld_get_image_header(i)});
#elif defined(__linux__)
  dl_iterate_phdr(collectLoadedObject, &loadedObjects);
#endif
}

static void pupLoadedObjects(PUP::er & p)
{
  unsigned long long n = loadedObjects.size();
  p | n;
  for (unsigned long long i = 0; i < n; i++)
  {
    std::string name;
    unsigned long long base = 0;
    if (!p.isUnpacking())
    {
      name = loadedObjects[i].name;
      base = loadedObjects[i].base;
    }
    p | name;
    p | base;
    if (!p.isUnpacking())
      continue;
    /* Compared with the first local entry of that name only: a second entry
       of the same name, if a loader ever lists one, must not fail the thread
       against the entry it does not match. An object this process has not
       loaded is not compared. */
    for (const LoadedObject & o : loadedObjects)
    {
      if (o.name != name)
        continue;
      if (o.base != base)
      {
        const char * shown = name.empty() ? "the program" : name.c_str();
        CmiAbort("Isomalloc> Cannot resume a migratable thread in this process: "
                 "%s is loaded at 0x%llx here but was at 0x%llx in the process "
                 "that packed the thread, whose stack holds addresses into the "
                 "program's code. Every process of a job whose threads migrate "
                 "between processes or restart from a checkpoint must load the "
                 "program at the same addresses: run with address space layout "
                 "randomization off (Linux: setarch $(uname -m) -R in front of "
                 "the program; with srun or mpirun, which start the tasks "
                 "themselves, inside the launch: srun ... setarch $(uname -m) "
                 "-R ./pgm). ampirun and testrun cannot turn randomization off "
                 "on macOS, so there threads must not migrate between processes "
                 "and a restart from a checkpoint is not possible.",
                 shown, o.base, base);
      }
      break;
    }
  }
}

static void iso_contextPup(CmiPupStream * p, CmiIsomallocContext * ctx)
{
  auto & pupper = *(PUP::er *)p->user;
  pupLoadedObjects(pupper);
  CmiIsomallocContextPup((pup_er)&pupper, ctx);
}

static void iso_contextActivate(CmiIsomallocContext ctx)
{
  CmiMemoryIsomallocContextActivate(ctx);
}

static const CthIsomallocOps cmiIsomallocOps = {
  iso_enabled,
  iso_permanentAllocAlign,
  iso_contextDelete,
  iso_contextPup,
  iso_contextActivate,
};

/* Defined by the memory module, which is linked only when the program asked
   for one (charmc -memory ...); without one there is nothing to initialize and
   nothing intercepts malloc. A weak no-op stands in, and memory.C's definition
   overrides it. A weak definition rather than a weak undefined reference
   tested against null: Mach-O's linker does not accept an undefined weak
   reference that nothing defines. C++ linkage, to match memory.C's
   definition: Reconverse's converse.h does not declare this. */
#if CMI_HAS_WEAK
__attribute__((weak)) void CmiMemoryInit(char ** argv) {}
#else
void CmiMemoryInit(char ** argv);
void CmiMemoryInit_default(char ** argv) {}
/* The C++ names, as MSVC decorates them for x64. */
#pragma comment(linker, "/alternatename:?CmiMemoryInit@@YAXPEAPEAD@Z=?CmiMemoryInit_default@@YAXPEAPEAD@Z")
#endif

/* stdio allocates a stream's buffer with malloc the first time the stream is
   written. With the isomalloc memory module linked, a process whose first
   printf comes from a migratable thread gets its stdout buffer inside that
   thread's heap, and the buffer leaves the process when the thread migrates:
   the next printf there writes into unmapped memory. The classic netlrts
   layer never met this because it makes stdout unbuffered. Give stdout a
   buffer that no heap owns, keeping the buffering mode stdio would pick. */
static char cmiStdoutBuffer[BUFSIZ];

static void CmiStdoutBufferOutsideIsomalloc(void)
{
  fflush(stdout);
  setvbuf(stdout, cmiStdoutBuffer, isatty(fileno(stdout)) ? _IOLBF : _IOFBF,
          sizeof(cmiStdoutBuffer));
}

static void CmiIsomallocInitForReconverse(char ** argv)
{
  /* Before Isomalloc: the memory module owns the per-PE "which context is
     receiving mallocs" state that Isomalloc pushes and pops around its own
     bookkeeping allocations. */
  CmiMemoryInit(argv);
  /* Set by the isomalloc memory module's init above, on rank 0. */
  if (CmiMyRank() == 0 && CmiMemoryIs(CMI_MEMORY_IS_ISOMALLOC))
    CmiStdoutBufferOutsideIsomalloc();

  CmiIsomallocInit(argv);
  if (CmiMyRank() == 0)
    collectLoadedObjects();
  /* After init, so that CthMigratable() answers with a region that has
     actually been negotiated rather than one that is about to be. */
  CthRegisterIsomallocOps(&cmiIsomallocOps);
}

/* Called from charm_main, before ConverseInit. */
void CmiIsomallocHookIntoReconverse(void)
{
  registerIsomallocInit(CmiIsomallocInitForReconverse);
}

/****************** Up: the PUP-shaped thread serializer ********************/

static void charmPupBytes(CmiPupStream * p, void * data, size_t n)
{
  if (n == 0) return;
  auto & pupper = *(PUP::er *)p->user;
  pupper((char *)data, (size_t)n);
}

CthThread CthPup(pup_er cpup, CthThread t)
{
  PUP::er & p = *(PUP::er *)cpup;

  CmiPupStream s;
  s.user = &p;
  s.bytes = charmPupBytes;
  s.mode = 0;
  if (p.isSizing()) s.mode |= CMI_PUP_SIZING;
  if (p.isPacking()) s.mode |= CMI_PUP_PACKING;
  if (p.isUnpacking()) s.mode |= CMI_PUP_UNPACKING;
  if (p.isDeleting()) s.mode |= CMI_PUP_DELETING;

  return CthPupThread(&s, t);
}

CmiIsomallocContext CmiIsomallocGetThreadContext(CthThread th)
{
  return CthGetIsomallocContext(th);
}

