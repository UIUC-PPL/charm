#define CMK_USE_LRTS                                       1

/* CMK_HAS_PARTITION requires the 'root' field to be present in CMK_MSG_HEADER_UNIQUE */
#define CMK_HAS_PARTITION                                  1

/* if set to 1 it uses the default scheduler (Csd) defined in convcore.c,
   otherwise machine.C has to provide its own scheduler. Should be 1 in almost
   every machine. */
#define CMK_CMIDELIVERS_USE_COMMON_CODE                    1

/* specifies if the functions CmiPrintf, CmiError and CmiScanf are implemented
   in machine.C (1), or if the standard definitions in convcore.c should be used
   (0). */
#define CMK_CMIPRINTF_IS_A_BUILTIN                         0

/* define the converse headers. For most of the purposes, only the UNIQUE header
   needs to be modified, the others will follow. BLUEGENE may need to be
   adapted.

   In particular, the fields "hdl", "xhdl" and "info" must be always present in
   the extended header, since they are directly accessed in converse.h */
/* - root is needed by CMK_HAS_PARTITION
 * - startid, redID
 * - rank is needed by broadcast
 */
#define CMK_MSG_HEADER_UNIQUE    CmiUInt4 size; CmiUInt2 rank,hdl,xhdl,info,redID; CmiInt4 root; CmiUInt1 zcMsgType:4, cmaMsgType:2, nokeep:1;

#define CMK_MSG_HEADER_BASIC  CMK_MSG_HEADER_EXT
#define CMK_MSG_HEADER_EXT            { CMK_MSG_HEADER_UNIQUE }

/* defines different parameters of groups of processors. (next 4 definitions)
   used in converse.h (the first) and convcore.c (the others). a value of 1
   means that convcore.c defines the methods, otherwise it is up to machine.C to
   define them */

/* basic structure of a CmiGroup (defined in converse.h) */
#define CMK_MULTICAST_GROUP_TYPE                struct { unsigned pe, id; }
/* definitions of establishment and lookup of groups */
#define CMK_MULTICAST_DEF_USE_COMMON_CODE                  1
/* definitions of List sending functions */
#define CMK_MULTICAST_LIST_USE_COMMON_CODE                 1
/* definitions of Multicast sending functions */
#define CMK_MULTICAST_GROUP_USE_COMMON_CODE                1

/* define the entity of the spanning tree used (it is 4 in all configurations)
   definese also if the code in converse.h will be used (1) or not and
   implemented in machine.C (0). At the momement all configurations use the
   common code. */
#define CMK_SPANTREE_MAXSPAN                               4
#define CMK_SPANTREE_USE_COMMON_CODE                       1

/* Specifies if the routines which send multiple messages (vectors of messages)
   to a processors are implemented in convcore.c (1) or in machine.C (1). */
#define CMK_VECTOR_SEND_USES_COMMON_CODE                   1

/* Defines if there is a "charmrun" program running on the system, which
   interacts with possible connecting clients (0), or if there is no such
   program, and processor 0 does the job (1). Currently only netlrts- and
   verbs- versions have this set to 0, all the others have it to 1. */
#define NODE_0_IS_CONVHOST                                 1

/* Enables the persistent communication protocol if set to 1. */
#define CMK_PERSISTENT_COMM                                0

/* Enables support for immediate messages if set to 1. */
#define CMK_IMMEDIATE_MSG				   1

/* This is needed to be 1 if the machine layer is used in some architectures
   where there is no coprocessor, and to pull messages out of the network there
   is the need of the processor intervention (like in BlueGene/L). 0 otherwise.
 */
#define CMK_MACHINE_PROGRESS_DEFINED                       1

/* This is required to define LrtsLock/LrtsUnlock */
#define CMK_USE_COMMON_LOCK                                1

#define CMK_ONESIDED_IMPL                                  1

/* Should be enough to fit UCP memh + rkey */
#define CMK_NOCOPY_DIRECT_BYTES                            256

#define CMK_REG_REQUIRED                                   CMK_ONESIDED_IMPL

#define CMK_CONVERSE_MPI                                   0

#undef  CMK_HAS_CMA
#define  CMK_HAS_CMA                                       0

/* By default use simple PMI */
#define CMK_USE_PMI                    1
#define CMK_USE_SIMPLEPMI              1
#define CMK_USE_PMI2                   0
#define CMK_USE_PMIX                   0

/* UCX machine layer supports GPU-aware communication */
#define CMK_GPU_COMM                   1
