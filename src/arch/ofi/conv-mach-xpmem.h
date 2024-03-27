#ifndef _CONV_MACH_XPMEM_
#define  _CONV_MACH_XPMEM

#undef CMK_USE_PXSHM
#undef CMK_USE_XPMEM
#define CMK_USE_XPMEM 			1

#undef CMK_IMMEDIATE_MSG
#define CMK_IMMEDIATE_MSG       0

#undef CMK_WHEN_PROCESSOR_IDLE_USLEEP
#define CMK_WHEN_PROCESSOR_IDLE_USLEEP  0

#define XPMEM_LOCK                      1

#endif
