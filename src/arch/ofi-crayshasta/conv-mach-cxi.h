#ifndef _CONV_MACH_H
#define _CONV_MACH_H

#define CMK_OFI                                            1
/* for Slingshot-11 the provider is CXI, this is notably different
 in how memory registration is handled from the old OFI. */
#ifdef CMK_CXI
#undef CMK_CXI
#endif
#define CMK_CXI                                        1

/* define the default linker, together with its options */
#define CMK_DLL_CC   "g++ -shared -O3 -o "

/* 1 if the machine has a function called "getpagesize()", 0 otherwise .
   used in the memory files of converse */
#define CMK_GETPAGESIZE_AVAILABLE                          1

/* defines which version of memory handlers should be used.
   used in conv-core/machine.C */
#define CMK_MALLOC_USE_GNU_MALLOC                          0
#define CMK_MALLOC_USE_OS_BUILTIN                          1

#define CMK_MEMORY_PAGESIZE                                4096
#define CMK_MEMORY_PROTECTABLE                             1

/* the following definitions set the type of shared variables to be used. only
   one of them must be 1, all the others 0. The different implementations are in
   converse.h. Typically used are UNAVAILABLE for non SMP versions and
   POSIX_THREADS_SMP for SMP versions. The others are used only in special
   cases: NT_THREADS for Windows. */
#define CMK_SHARED_VARS_UNAVAILABLE                        1 /* non SMP versions */
#define CMK_SHARED_VARS_POSIX_THREADS_SMP                  0 /* SMP versions */
#define CMK_SHARED_VARS_NT_THREADS                         0

/* the following define if signal handlers should be used, both equal to zero
   means that signals will not be used. only one of the following can be 1, the
   other must be 0. they differ in the fact that the second (_WITH_RESTART)
   enables retry on interrupt (a function is recalled upon interrupt and does
   not return EINTR as in the first case) */
#define CMK_SIGNAL_USE_SIGACTION                           0
#define CMK_SIGNAL_USE_SIGACTION_WITH_RESTART              1

/* specifies whether the CthCpv variables should be defined as Cpv (0) or
   directly as normal c variables (1) */
#define CMK_THREADS_REQUIRE_NO_CPV                         0

/* decide which is the default implementation of the threads (see threads.C)
   Only one of the following can be 1. If none of them is selected, qthreads
   will be used as default. This default can be overwritten at compile time
   using -DCMK_THREADS_BUILD_"type"=1 */
#define CMK_THREADS_USE_CONTEXT                            0
#define CMK_THREADS_USE_FCONTEXT                           1
#define CMK_THREADS_USE_JCONTEXT                           0
#define CMK_THREADS_USE_PTHREADS                           0

/* Specifies what kind of timer to use, and the correspondent headers will be
   included in convcore.C. If none is selected, then the machine.C file needs to
   implement the timer primitives. */
#define CMK_TIMER_USE_RTC                                  0
#define CMK_TIMER_USE_RDTSC                                0
#define CMK_TIMER_USE_GETRUSAGE                            1
#define CMK_TIMER_USE_SPECIAL                              0
#define CMK_TIMER_USE_TIMES                                0

/* Specifies what the processor will do when it is idle, either sleep (1) or go
   into busy waiting mode (0). In convcore.C there are a few files included if
   sleeping mode, but the real distinct implementation is in the machine.C
   file. */
#define CMK_WHEN_PROCESSOR_IDLE_USLEEP                     0

/* specifies whether there is a web server collecting utilization statistics (1)
   or not (0) */
#define CMK_WEB_MODE                                       1

#define CMK_DEBUG_MODE                                     0

/* enables the load balancer framework. set to 1 for almost all the machines */
#define CMK_LBDB_ON					   1

#define CMK_64BIT                      1
#define CMK_AMD64                      1

/* Other possible definitions:

In fault tolerant architectures, CK_MEM_CHECKPOINT can be set. In this case the
extended header must contain also another field called "pn" (phase number).

*/

/* Use PMI2 by default on Cray systems with cray-pmi */
#include "conv-mach-slurmpmi2cray.h"

#endif
