
#ifndef MEMPOOL_H
#define MEMPOOL_H

#include "conv-config.h"
#include "converse.h"

#if CMK_CONVERSE_UGNI
#include "gni_pub.h"
#include "pmi.h"
typedef gni_mem_handle_t mem_handle_t;
#elif CMK_OFI
#include "rdma/fi_domain.h"
typedef struct fid_mr* mem_handle_t;
#else
// in uGNI, it is memory handler, other versions, this is an integer
// a unique integer to represent the memory block
typedef CmiInt8 mem_handle_t;
#endif

typedef void* (*mempool_newblockfn)(size_t* size, mem_handle_t* mem_hndl, int expand_flag);
typedef void (*mempool_freeblock)(void* ptr, mem_handle_t mem_hndl);

#define cutOffNum CMK_MEMPOOL_CUTOFFNUM

//given x as mptr get
#define MEMPOOL_GetBlockHead(x) (&((x)->block_head))
//given x as block header, get ...
#define MEMPOOL_GetBlockSize(x) ((x)->size)
#define MEMPOOL_GetBlockMemHndl(x) ((x)->mem_hndl)
#define MEMPOOL_GetBlockNext(x) ((x)->block_next)
//given x as user pointer, get mempool_header/slot_header
#define MEMPOOL_GetMempoolHeader(x, align) \
  ((mempool_header*)((char*)(x) - (align)))
//given x as mempool_header/slot_header, get ...
#define MEMPOOL_GetBlockPtr(x) ((x)->block_ptr)
#define MEMPOOL_GetMempoolPtr(x) (MEMPOOL_GetBlockPtr(x)->mptr)
#define MEMPOOL_GetSize(x) (MEMPOOL_GetBlockPtr(x)->size)
#define MEMPOOL_GetMemHndl(x) (MEMPOOL_GetBlockPtr(x)->mem_hndl)
#define MEMPOOL_GetMsgInRecv(x) (MEMPOOL_GetBlockPtr(x)->msgs_in_recv)
#define MEMPOOL_GetMsgInSend(x) (MEMPOOL_GetBlockPtr(x)->msgs_in_send)
#define MEMPOOL_IncMsgInRecv(x) (MEMPOOL_GetBlockPtr(x)->msgs_in_recv)++
#define MEMPOOL_DecMsgInRecv(x) (MEMPOOL_GetBlockPtr(x)->msgs_in_recv)--
#define MEMPOOL_IncMsgInSend(x) (MEMPOOL_GetBlockPtr(x)->msgs_in_send)++
#define MEMPOOL_DecMsgInSend(x) (MEMPOOL_GetBlockPtr(x)->msgs_in_send)--
#define MEMPOOL_GetSlotGNext(x) ((x)->gnext)
#define MEMPOOL_GetSlotStatus(x) ((x)->status)
#define MEMPOOL_GetSlotSize(x) ((x)->size)
struct block_header;
struct mempool_type;

//header of an free slot
typedef struct slot_header_
{
  struct block_header* block_ptr;
  size_t size;
  int power, status;    // status is 1 for free, 0 for used
  size_t gprev, gnext;  // global slot list within a block
  size_t prev, next;    // linked list for freelists slots
#if CMK_64BIT
  int padding[2];
#endif
} slot_header;

typedef struct used_header_
{
  struct block_header* block_ptr;
  size_t size;
  int power, status;    // status is 1 for free, 0 for used
  size_t gprev, gnext;  // global slot list within a block
  int padding[2];
} used_header;

typedef used_header mempool_header;

// multiple mempool for different size allocation
// make sure this is 16 byte aligned
typedef struct block_header
{
  mem_handle_t mem_hndl;
  size_t size, used;
  size_t block_prev, block_next;  // offset to next memblock
  struct mempool_type* mptr;      // mempool_type
#if CMK_CONVERSE_UGNI
  int msgs_in_send;
  int msgs_in_recv;
#define freelists_extra (sizeof(mem_handle_t) + sizeof(struct mempool_type*) + 2 * sizeof(int))
#else
#define freelists_extra (sizeof(mem_handle_t) + sizeof(struct mempool_type*))
#endif
  // use freelists as padding to the nearest 16-byte interval to avoid size-zero array warnings
  size_t freelists[cutOffNum + CMIPADDING(cutOffNum * sizeof(size_t) + freelists_extra, 16) / sizeof(size_t)];
#undef freelists_extra
} block_header;

typedef struct out_of_pool_header
{
  block_header block_head;
  mempool_header block;
} out_of_pool_header;

typedef struct large_block_header
{
  mem_handle_t mem_hndl;
  size_t size;
  size_t block_prev, block_next;  // offset to next memblock
  struct mempool_type* mptr;      // mempool_type
#if CMK_CONVERSE_UGNI
  int msgs_in_send;
  int msgs_in_recv;
#endif
  int padding[2];
} large_block_header;

// only at beginning of first block of mempool, representing the mempool
typedef struct mempool_type
{
  block_header block_head;
  mempool_newblockfn newblockfn;
  mempool_freeblock freeblockfn;
  size_t large_blocks;
  size_t block_tail;
  size_t limit;
  size_t size;
#if CMK_SMP && (CMK_CONVERSE_UGNI || CMK_OFI)
  CmiNodeLock mempoolLock;
  char padding[CMIPADDING((6 * sizeof(size_t) + sizeof(CmiNodeLock)), 16)];
#elif !CMK_64BIT
  size_t padding[2];
#endif
} mempool_type;

#ifdef __cplusplus
static_assert(sizeof(slot_header) % 16 == 0, "slot_header is not a multiple of 16 bytes");
static_assert(sizeof(used_header) % 16 == 0, "used_header is not a multiple of 16 bytes");
static_assert(sizeof(block_header) % 16 == 0, "block_header is not a multiple of 16 bytes");
static_assert(sizeof(large_block_header) % 16 == 0, "large_block_header is not a multiple of 16 bytes");
static_assert(sizeof(mempool_type) % 16 == 0, "mempool_type is not a multiple of 16 bytes");

extern "C" {
#endif

mempool_type* mempool_init(size_t pool_size, mempool_newblockfn newfn, mempool_freeblock freefn, size_t limit);
void mempool_destroy(mempool_type* mptr);
void* mempool_malloc(mempool_type* mptr, size_t size, int expand);
void* mempool_large_malloc(mempool_type* mptr, size_t size, int expand);
void mempool_free(mempool_type* mptr, void* ptr_free);

#if CMK_SMP && (CMK_CONVERSE_UGNI || CMK_OFI)
void mempool_free_thread(void* ptr_free);
#endif

#if defined(__cplusplus)
}
#endif

#if CMK_CONVERSE_UGNI
void* getNextRegisteredPool();
#endif

#endif /* MEMPOOL.H */
