/* This header consists of declarations and definitions that are common to all
 * machine layer implementations and architectures. This header is included in
 * conv-config.h, which is included in the machine layer implementations through
 * converse.h.
 */

// Enum for registration modes used in the Zerocopy API
// TODO:Convert to typed enum post C++ conversion
enum ncpyRegModes {
  /* CMK_BUFFER_REG is the default mode that always registers the buffer by treating network and non-network transfers
   * equally. It avoids an extra small message to register, as done in the case of CMK_BUFFER_UNREG
   */
  CMK_BUFFER_REG      = 0,

  /* CMK_BUFFER_UNREG is used for not registering the buffer during object creation and it avoids
   * non-network registration by registering only when required i.e only when this buffer
   * is involved in an RDMA operation using the network. (either as a source or destination).
   * The registeration on demand is performed by sending a small message to the remote process
   * in order to register the remote buffer. Zerocopy operations using this mode use the inverse
   * operation mechanism, where along with the remote registration, the RDMA operation is performed
   * by the remote process (which is not the initiator) to avoid sending another small message back to
   * the initiator.
   */
  CMK_BUFFER_UNREG    = 1,

  /* CMK_BUFFER_PREREG is a mode that is to be supported in the future. It is intended to allow users
   * to pass buffers allocated through CmiAlloc (which uses mempools on networks requiring explicit
   * registration).
   */
  CMK_BUFFER_PREREG   = 2,

  /* CMK_BUFFER_NOREG is used in those cases where there is no registration or setting of machine specific
   * information required. It is intended for use cases where the Ncpy objects are created on remote processes
   * to just store basic information about remote buffers without registering them.
   */
  CMK_BUFFER_NOREG    = 3
};

// Enum for de-registration modes used in the Zerocopy API
// TODO:Convert to typed enum post C++ conversion
enum ncpyDeregModes {
  /* CMK_BUFFER_DEREG is the default mode which makes the RTS de-register the buffers after the completion
   * of the Zero Copy operation
   */
  CMK_BUFFER_DEREG    = 4,

  /* CMK_BUFFER_NODEREG is used to make the RTS skip the de-registration of the buffers after the completion
   * of the Zero Copy operation. For the ZC Send API, specifying CMK_BUFFER_NODEREG in CkSendBuffer ensures that
   * the source buffer remains registered after the completion of the operation. For the ZC Post API, in addition
   * to specifying CMK_BUFFER_NODEREG in CkSendBuffer, it can also be specified in the Post Entry Method using the
   * CkNcpyBufferPost object. This ensures that the destination buffer, which was posted by the user remains
   * registered after the completion of the operation.
   */
  CMK_BUFFER_NODEREG  = 5
};

// default of CMK_COMMON_NOCOPY_DIRECT_BYTES assumes no CMA support
// Refined for lrts layers with CMA support inside lrts-common.h
#define CMK_COMMON_NOCOPY_DIRECT_BYTES 0

// Enum for the type of zerocopy operation
// TODO: Convert to typed enum post C++ conversion
enum ncpyOperationMode {
  CMK_DIRECT_API             = 0,
  CMK_EM_API                 = 1,
  CMK_EM_API_SRC_ACK_INVOKE  = 2,
  CMK_EM_API_DEST_ACK_INVOKE = 3,
  CMK_EM_API_REVERSE         = 4,
  CMK_BCAST_EM_API           = 5,
  CMK_BCAST_EM_API_REVERSE   = 6,
  CMK_READONLY_BCAST         = 7,
  CMK_ZC_PUP                 = 8
};

// Enum for the method of acknowledglement handling after the completion of a zerocopy operation
// TODO: Convert to typed enum post C++ conversion
enum ncpyAckMode {
  CMK_SRC_DEST_ACK       = 0,
  CMK_SRC_ACK            = 1,
  CMK_DEST_ACK           = 2
};

// Enum to determine if a NcpyOperationInfo can be freed upon completion
// TODO: Convert to a bool variable post C++ conversion
enum ncpyFreeNcpyOpInfoMode {
  CMK_FREE_NCPYOPINFO                = 0,
  CMK_DONT_FREE_NCPYOPINFO           = 1
};

// Enum for the type of converse message
// TODO: Convert to a bool variable post C++ conversion
enum cmiZCMsgType {
  CMK_REG_NO_ZC_MSG = 0,
  CMK_ZC_P2P_SEND_MSG = 1,
  CMK_ZC_P2P_RECV_MSG = 2,
  CMK_ZC_SEND_DONE_MSG = 3, // USED for both ZC_BCAST_SEND_DONE_MSG & ZC_P2P_SEND_DONE_MSG
  CMK_ZC_BCAST_SEND_MSG = 4,
  CMK_ZC_BCAST_RECV_MSG = 5,
  CMK_ZC_BCAST_RECV_DONE_MSG = 6,
  CMK_ZC_BCAST_RECV_ALL_DONE_MSG = 7,
  CMK_ZC_DEVICE_MSG = 8
};

#ifndef CMK_NOCOPY_DIRECT_BYTES

#if defined(_WIN32)
#define CMK_NOCOPY_DIRECT_BYTES 1
/* It is required to declare CMK_NOCOPY_DIRECT_BYTES to 1 instead of 0
 * as this avoids the C2229 error (illegal zero-sized array)
 * for char layerInfo[CMK_NOCOPY_DIRECT_BYTES] which is seen for
 * a 0 sized array on VC++
 */
#else
#define CMK_NOCOPY_DIRECT_BYTES 0
#endif // end of if defined(_WIN32)

#endif // end of ifndef CMK_NOCOPY_DIRECT_BYTES

#ifndef CMK_COMMON_NOCOPY_DIRECT_BYTES
#define CMK_COMMON_NOCOPY_DIRECT_BYTES 0
#endif

/* GPU-aware communication is not supported by the machine layer by default */
#ifndef CMK_GPU_COMM
#define CMK_GPU_COMM 0
#endif
