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

#include <stdio.h>
#include <unistd.h>

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
   single-process job never migrates anything between address spaces. */
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
static void iso_contextPup(CmiPupStream * p, CmiIsomallocContext * ctx)
{
  auto & pupper = *(PUP::er *)p->user;
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

