#define CMK_USE_LRTS                                       1

#define	CMK_HAS_PARTITION		                   1

#define CMK_NET_VERSION					   1

#define CMK_CMIPRINTF_IS_A_BUILTIN                         1

#define CMK_CMIDELIVERS_USE_COMMON_CODE                    1

#define CMK_HANDLE_SIGUSR                                  1

#define CMK_MULTICAST_GROUP_TYPE                struct { unsigned pe, id; }
#define CMK_MULTICAST_DEF_USE_COMMON_CODE                  1
#define CMK_MULTICAST_LIST_USE_COMMON_CODE                 1
#define CMK_MULTICAST_GROUP_USE_COMMON_CODE                1

#define CMK_ONESIDED_IMPL                                  1

/* the first 4 fields of the header are defined in machine-dgram.C
   and are used for the udp retransmission protocol implementation.
   The root parameter is for the communication library and is used in
   broadcast operations. The cmaMsgType parameter is used to identify the type
   of the message and used in the LRTS based CMA implementaion.
*/
#define CMK_MSG_HEADER_BASIC   CMK_MSG_HEADER_EXT
#define CMK_MSG_HEADER_EXT_    CmiUInt2 d0,d1,d2,d3,hdl,xhdl,info,redID,rank; CmiInt4 root; size_t size; CmiUInt1 zcMsgType:4, cmaMsgType:2, nokeep:1;
#define CMK_MSG_HEADER_EXT       { CMK_MSG_HEADER_EXT_ }

#define CMK_SPANTREE_MAXSPAN                               4
#define CMK_SPANTREE_USE_COMMON_CODE                       1

#define CMK_VECTOR_SEND_USES_COMMON_CODE                   1

#define CMK_IMMEDIATE_MSG				   1

#define CMK_PERSISTENT_COMM				   0

#define CMK_OBJECT_QUEUE_AVAILABLE		   	   0
#define CMK_USE_SPECIAL_MESSAGE_QUEUE_CHECK                1

#define CMK_BARRIER_USE_COMMON_CODE                        1

#define CMK_MACHINE_PROGRESS_DEFINED                       1

#define NODE_0_IS_CONVHOST                                 0

/* call cpu timer for LB */
#define CMK_LB_CPUTIMER					   0

#define CMK_HAS_SIZE_IN_MSGHDR                             1

#define CMK_USE_COMMON_LOCK                                1

#define CMK_NOCOPY_DIRECT_BYTES                           16

#define CMK_REG_REQUIRED                                   CMK_ONESIDED_IMPL

#define CMK_CONVERSE_MPI                                   0
