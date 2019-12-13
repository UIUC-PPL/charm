#ifndef __HAPI_IMPL_H_
#define __HAPI_IMPL_H_

#ifdef __cplusplus
extern "C" {
#endif

// Initialize & exit hybrid API.
void initHybridAPI();
void initDeviceMapping(char** argv);
void exitHybridAPI();

// Manages event queues used for polling.
void initEventQueues(char** argv);
void destroyEventQueues();

// Registers callback handler functions.
void hapiRegisterCallbacks();

// Polls for GPU work completion. Does not do anything if HAPI_CUDA_CALLBACK is defined.
void hapiPollEvents();

// BufferPool constructs for mempool implementation.
// Data and metadata reside in same chunk of memory.
typedef struct _bufferPoolHeader {
  struct _bufferPoolHeader *next;
  int slot;
#ifdef HAPI_MEMPOOL_DEBUG
  size_t size;
#endif
} BufferPoolHeader;

typedef struct _bufferPool {
  BufferPoolHeader *head;
  size_t size;
#ifdef HAPI_MEMPOOL_DEBUG
  int num;
#endif
} BufferPool;

// PE-GPU mapping types
enum class Mapping {
  None, // Mapping is explicitly performed by the user
  Block,
  RoundRobin
};

#ifdef __cplusplus
}
#endif

#endif // __HAPI_IMPL_H_
