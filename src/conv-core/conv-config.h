/*
  Generic converse configuration-flags header.
*/
#ifndef __UIUC_CHARM_CONV_CONFIG_H
#define __UIUC_CHARM_CONV_CONFIG_H

/* for size_t */
#include <stddef.h>

/* 
 Include the automatically determined options.
  conv-autoconfig.h is written by the configure script.

 This header declares all automatically-determined properties
 of the machine, like compiler features, headers, and syscalls.
 Generally, more features should be moved into this header
 from the other manually generated headers.
*/
#include "conv-autoconfig.h"

/* 
 Include the machine.C configuration header
  (e.g., charm/src/arch/net/conv-common.h )

 This header declares communication properties
 (like message header formats) and the various 
 machine arcana handled by machine.C.
*/ 
#include "conv-common.h"

/*
 Include the common header for all layers and all architectures
 (charm/src/arch/common/conv-mach-common.h)

 This header declares properties common for all
 machine layer implementations and all architectures
 */
#include "conv-mach-common.h"

/* 
 Include the system/platform header.
  (e.g., charm/src/arch/netlrts-linux-x86_64/conv-mach.h )
 
 This header declares the handling of malloc (OS or charm),
 signals, threads, timers, and other details. 
*/
#include "conv-mach.h"

/* 
 Include the build-time options.
  conv-mach-opt.h is written by the build script.

 This header includes any special build-time options.
 It's typically empty or very short.
*/
#include "conv-mach-opt.h"

/*
 Include the lrts common header for all lrts layers
 (charm/src/arch/util/lrts-common.h)

 This header declares properties common for all lrts
 based machine layer implementations
 */
#if CMK_USE_LRTS
#include "lrts-common.h"
#endif



/* Fix various invariants (these symbols should probably 
  be eliminated entirely) */
#define CMK_LBDB_OFF (!CMK_LBDB_ON)

#if CMK_AMD64 && !defined(CMK_64BIT)
#define CMK_64BIT		1
#endif

#if CMK_64BIT && !CMK_SIZET_64BIT
#error "Compiler not generating 64 bit binary, please check compiler flags."
#endif

#if CMK_SIZET_64BIT && !CMK_64BIT
#define CMK_64BIT                1
#endif

#ifndef CMK_REPLAYSYSTEM
#define CMK_REPLAYSYSTEM            1
#endif

/* replay does not work on windows */
#ifdef _WIN32
#undef CMK_REPLAYSYSTEM
#define CMK_REPLAYSYSTEM            0
#endif

#if ! CMK_CCS_AVAILABLE
#undef CMK_CHARMDEBUG
#define  CMK_CHARMDEBUG             0
#endif

#ifndef CMK_TRACE_ENABLED
#define CMK_TRACE_ENABLED          1
#endif

#ifndef  CMK_WITH_CONTROLPOINT
#define CMK_WITH_CONTROLPOINT            1
#endif

/* sanity checks */
#if ! CMK_TRACE_ENABLED && CMK_SMP_TRACE_COMMTHREAD
#undef CMK_SMP_TRACE_COMMTHREAD
#define CMK_SMP_TRACE_COMMTHREAD                               0
#endif

#if !defined(CMK_SMP)
#define CMK_SMP                   0
#endif

#if CMK_SMP_TRACE_COMMTHREAD && ! CMK_SMP
#undef CMK_SMP_TRACE_COMMTHREAD
#define CMK_SMP_TRACE_COMMTHREAD                               0
#endif

#if !defined(CMK_CUDA)
#define CMK_CUDA                  0
#endif

#ifndef CMI_QD
#define CMI_QD (CMK_REPLAYSYSTEM)
#endif

#ifndef CMI_SWAPGLOBALS
#define CMI_SWAPGLOBALS (CMK_HAS_ELF_H && !CMK_SMP)
#endif

#if !defined(CMK_CRAYXE)
#define CMK_CRAYXE                0
#endif

#if !defined(CMK_CRAYXC)
#define CMK_CRAYXC                0
#endif

#if (CMK_CRAYXE || CMK_CRAYXC) && CMK_CONVERSE_UGNI && ! CMK_SMP
#include "conv-mach-pxshm.h"
#endif

/* Cache line size */
#ifdef __cplusplus
#include <new>
#endif
#ifdef __cpp_lib_hardware_interference_size
# define CMI_CACHE_LINE_SIZE std::hardware_destructive_interference_size
#elif CMK_PPC64
# define CMI_CACHE_LINE_SIZE 128
#else
# define CMI_CACHE_LINE_SIZE 64
#endif

/* Without stdint.h, CMK_TYPEDEF_(U)INT{2,4,8} must be defined in the
   corresponding conv-mach.h */
#if CMK_HAS_STDINT_H && !defined(CMK_TYPEDEF_INT2)
#include <stdint.h>
typedef int8_t  CMK_TYPEDEF_INT1;
typedef int16_t CMK_TYPEDEF_INT2;
typedef int32_t CMK_TYPEDEF_INT4;
typedef int64_t CMK_TYPEDEF_INT8;
typedef uint8_t  CMK_TYPEDEF_UINT1;
typedef uint16_t CMK_TYPEDEF_UINT2;
typedef uint32_t CMK_TYPEDEF_UINT4;
typedef uint64_t CMK_TYPEDEF_UINT8;
typedef intptr_t CmiIntPtr;
#else
#if CMK_SIZET_64BIT
typedef CMK_TYPEDEF_UINT8     CmiIntPtr;
#else
typedef CMK_TYPEDEF_UINT4     CmiIntPtr;
#endif
#endif

#endif

/* Enable node queue for all SMP and multicore builds */
#define CMK_NODE_QUEUE_AVAILABLE CMK_SMP
