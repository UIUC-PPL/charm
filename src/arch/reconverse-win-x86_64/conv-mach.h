#ifndef _CONV_MACH_H
#define _CONV_MACH_H

/* Reconverse on Windows with MSVC: a single process whose PEs are threads.
   Reconverse itself provides the threads, locks, timers and user-level
   threads (fcontext, from Boost's MASM sources); this file describes the
   platform to the rest of Charm++. */

#define CMK_AMD64					   1
#define CMK_64BIT    1
#define CMK_SMP						   1

#undef CMK_IMMEDIATE_MSG
#define CMK_IMMEDIATE_MSG                                  0

#define CMK_ASYNC_NOT_NEEDED                               1
#define CMK_ASYNC_USE_FIOASYNC_AND_FIOSETOWN               0
#define CMK_ASYNC_USE_FIOASYNC_AND_SIOCSPGRP               0
#define CMK_ASYNC_USE_FIOSSAIOSTAT_AND_FIOSSAIOOWN         0
#define CMK_ASYNC_USE_F_SETFL_AND_F_SETOWN                 0

#define CMK_GETPAGESIZE_AVAILABLE                          0

#define CMK_MALLOC_USE_GNU_MALLOC                          0
#define CMK_MALLOC_USE_OS_BUILTIN                          1

#define CMK_MEMORY_PAGESIZE                                4096
#define CMK_MEMORY_PROTECTABLE                             0


#define CMK_SSH_IS_A_COMMAND                               0
#define CMK_SSH_NOT_NEEDED                                 1

#define CMK_SHARED_VARS_UNAVAILABLE                        0
#define CMK_SHARED_VARS_NT_THREADS                         1

#define CMK_THREADS_USE_CONTEXT                            0
#define CMK_THREADS_USE_FCONTEXT                           1
#define CMK_THREADS_USE_PTHREADS                           0
#define CMK_THREADS_ARE_WIN32_FIBERS                       0

#define CMK_SIGNAL_NOT_NEEDED                              1
#define CMK_SIGNAL_USE_SIGACTION                           0
#define CMK_SIGNAL_USE_SIGACTION_WITH_RESTART              0

#define CMK_THREADS_REQUIRE_NO_CPV                         0
#define CMK_THREADS_COPY_STACK                             0

#define CMK_TIMER_USE_RDTSC                                0
#define CMK_TIMER_USE_GETRUSAGE                            0
#define CMK_TIMER_USE_SPECIAL                              0
#define CMK_TIMER_USE_TIMES                                0
#define CMK_TIMER_USE_WIN32API                             1


#define CMK_WHEN_PROCESSOR_IDLE_BUSYWAIT                   0
#define CMK_WHEN_PROCESSOR_IDLE_USLEEP                     0

#define CMK_DEBUG_MODE					   0
#define CMK_WEB_MODE                                       1

#define CMK_LBDB_ON					   1

#define CMK_CONVERSE_MPI                                   0

#endif
