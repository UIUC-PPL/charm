/*
 * Filippo's charm debug memory module, gioachin@uiuc.edu, 2005/10
 * based on Orion's memory-leak.C
 *
 * This special version of malloc() and company is meant to be used in
 * conjunction with the parallel debugger CharmDebug.
 *
 * Functionalities provided:
 * - detect multiple delete on a pointer
 * - stacktrace for all memory allocated
 * - division of allocated memory among different types of allocations
 * - sweep of the memory searching for leaks
 */

#include "tracec.h"
#include <sys/mman.h>

/* Utilities needed by the code */
#include "ckhashtable.h"

/*#include "pup_c.h" */

#include "crc32.h"

#if ! CMK_CHARMDEBUG
#error "charmdebug is not enabled (e.g. when building with-production)"
static void *meta_malloc(size_t size);
static void *meta_calloc(size_t nelem, size_t size);
static void *meta_realloc(void *oldBuffer, size_t newSize);
static void *meta_memalign(size_t align, size_t size);
static int meta_posix_memalign(void **outptr, size_t align, size_t size);
static void *meta_aligned_alloc(size_t align, size_t size);
static void *meta_valloc(size_t size);
static void *meta_pvalloc(size_t size);
#else

void CmiBacktraceRecordHuge(void **retPtrs,int *nLevels);

typedef struct _Slot Slot;
typedef struct _SlotStack SlotStack;

int nextChareID;
extern int memory_chare_id;

int memory_charmdebug_internal = 0;

/**
 * Struct Slot contains all of the information about a malloc buffer except
 * for the contents of its memory.
 */
struct _Slot {
#ifdef CMK_SEPARATE_SLOT
  char *userData;
#else
  /*Doubly-linked allocated block list*/
  Slot *next;
  Slot *prev;
#endif

  /*The number of bytes of user data*/
  int userSize;

#define FLAGS_MASK        0xFF
#define BLOCK_PROTECTED   0x80
#define MODIFIED          0x40
#define NEW_BLOCK         0x20
#define LEAK_CLEAN        0x10
#define LEAK_FLAG         0x8
#define UNKNOWN_TYPE      0x0
#define SYSTEM_TYPE       0x1
#define USER_TYPE         0x2
#define CHARE_TYPE        0x3
#define MESSAGE_TYPE      0x4
  /* A magic number field, to verify this is an actual malloc'd buffer, and what
     type of allocation it is. The last 4 bits of the magic number are used to
     define a classification of mallocs. */
#define SLOTMAGIC            0x8402a500
#define SLOTMAGIC_VALLOC     0x7402a500
#define SLOTMAGIC_FREED      0xDEADBEEF
  int magic;

  int chareID;
  /* Controls the number of stack frames to print out. */
  int stackLen;
  void **from;

  /* Pointer to extra stacktrace, when the user requested more trace */
  SlotStack *extraStack;

  /* CRC32 checksums */
  CmiUInt8 slotCRC;
  CmiUInt8 userCRC;
};

struct _SlotStack {
  char *protectedMemory;
  int protectedMemoryLength;
  /* empty for the moment, to be filled when needed */
};

/********* List of allocated memory *********/

/* First memory slot */
#ifdef CMK_SEPARATE_SLOT
CkHashtable_c block_slots = NULL;
#else
Slot slot_first_storage = {&slot_first_storage, &slot_first_storage};
Slot *slot_first = &slot_first_storage;
#endif

int memory_allocated_user_total;
int get_memory_allocated_user_total() {return memory_allocated_user_total;}

void *lastMemoryAllocated = NULL;
Slot **allocatedSince = NULL;
int allocatedSinceSize = 0;
int allocatedSinceMaxSize = 0;
int saveAllocationHistory = 0;

/* Convert a slot to a user address */
static char *SlotToUser(Slot *s) {
#ifdef CMK_SEPARATE_SLOT
  return s->userData;
#else
  return ((char *)s)+sizeof(Slot);
#endif
}


/* Convert a user address to a slot. The parameter "user" must be at the
 * beginning of the allocated block */
static Slot *UserToSlot(void *user) {
#ifdef CMK_SEPARATE_SLOT
  return (Slot *)CkHashtableGet(block_slots, &user);
#else
  char *cu=(char *)user;
  Slot *s=(Slot *)(cu-sizeof(Slot));
  return s;
#endif
}

static int isLeakSlot(Slot *s) {
  return s->magic & LEAK_FLAG;
}

static int isProtected(Slot *s) {
  return s->magic & BLOCK_PROTECTED;
}

int Slot_ChareOwner(void *s) {
  return ((Slot*)s)->chareID;
}

int Slot_AllocatedSize(void *s) {
  return ((Slot*)s)->userSize;
}

int Slot_StackTrace(void *s, void ***stack) {
  *stack = ((Slot*)s)->from;
  return ((Slot*)s)->stackLen;
}

static void printSlot(Slot *s) {
  CmiPrintf("[%d] Leaked block of %d bytes at %p:\n",
	    CmiMyPe(), s->userSize, (void *)SlotToUser(s));
  CmiBacktracePrint(s->from,s->stackLen);
}

/********* Cpd routines for pupping data to the debugger *********/

/** Returns the number of total blocks of memory allocated */
size_t cpd_memory_length(void *lenParam) {
  size_t n=0;
#ifdef CMK_SEPARATE_SLOT
  n = CkHashtableSize(block_slots) - 1;
#else
  Slot *cur = slot_first->next;
  while (cur != slot_first) {
    n++;
    cur = cur->next;
  }
#endif
  return n;
}

/** PUP a single slot of memory for the debugger. This includes the information
 * about the slot (like size and location), but not the allocated data itself. */
#ifdef CMK_SEPARATE_SLOT
void cpd_memory_single_pup(CkHashtable_c h, pup_er p) {
  CkHashtableIterator_c hashiter;
  void *key;
  Slot *cur;
  //int counter=0;

  memory_charmdebug_internal = 1;
  hashiter = CkHashtableGetIterator(h);
  while ((cur = (Slot *)CkHashtableIteratorNext(hashiter, &key)) != NULL) {
    if (pup_isPacking(p) && cur->userData == lastMemoryAllocated) continue;
    //counter++;
#else
void cpd_memory_single_pup(Slot* list, pup_er p) {
  Slot *cur = list->next;
  /* Stupid hack to avoid sending the memory we just allocated for this packing,
     otherwise the lengths will mismatch */
  if (pup_isPacking(p)) cur = cur->next;
  for ( ; cur != list; cur = cur->next) {
#endif
    {
    int i;
    int flags;
    void *loc = SlotToUser(cur);
    CpdListBeginItem(p, 0);
    pup_comment(p, "loc");
    pup_pointer(p, &loc);
    pup_comment(p, "size");
    pup_int(p, &cur->userSize);
    pup_comment(p, "flags");
    flags = cur->magic & FLAGS_MASK;
    pup_int(p, &flags);
    pup_comment(p, "chare");
    pup_int(p, &cur->chareID);
    pup_comment(p, "stack");
    //for (i=0; i<STACK_LEN; ++i) {
    //  if (cur->from[i] <= 0) break;
      //      if (cur->from[i] > 0) pup_uint(p, (unsigned int*)&cur->from[i]);
      //      else break;
    //}
    if (cur->from != NULL)
      pup_pointers(p, cur->from, cur->stackLen);
    else {
      void *myNULL = NULL;
      printf("Block %p has no stack!\n",(void *)cur);
      pup_pointer(p, &myNULL);
    }
    }
  }
  /*CmiPrintf("counter=%d\n",counter);*/
  memory_charmdebug_internal = 0;
}

/** PUP the entire information about the allocated memory to the debugger */
void cpd_memory_pup(void *itemParam, pup_er p, CpdListItemsRequest *req) {
  static char s_memory[] = "memory";
  CpdListBeginItem(p, 0);
  pup_comment(p, "name");
  pup_chars(p, s_memory, sizeof(s_memory)-1);
  pup_comment(p, "slots");
  pup_syncComment(p, pup_sync_begin_array, 0);
#ifdef CMK_SEPARATE_SLOT
  cpd_memory_single_pup(block_slots, p);
#else
  cpd_memory_single_pup(slot_first, p);
#endif
  pup_syncComment(p, pup_sync_end_array, 0);
}

/*
void check_memory_leaks(CpdListItemsRequest *);
void cpd_memory_leak(void *iterParam, pup_er p, CpdListItemsRequest *req) {
  if (pup_isSizing(p)) {
    // let's perform the memory leak checking. This is the first step in the
    // packing, where we size, in the second step we pack and we avoid doing
    // this check again.
    check_memory_leaks(req);
  }
  cpd_memory_pup(iterParam, p, req);
}
*/

size_t cpd_memory_getLength(void *lenParam) { return 1; }
/** Returns the content of a block of memory (i.e the user data).
    This request must always be at the beginning of an allocated block
    (not for example an object part of an array) */
void cpd_memory_get(void *iterParam, pup_er p, CpdListItemsRequest *req) {
  void *userData = (void*)(uintptr_t)(((uint64_t)req->lo) + (((uint64_t)req->hi)<<32));
  Slot *sl = UserToSlot(userData);
  CpdListBeginItem(p, 0);
  pup_comment(p, "size");
  //printf("value: %x %x %x %d\n",sl->magic, sl->magic&~FLAGS_MASK, SLOTMAGIC, ((sl->magic&~FLAGS_MASK) != SLOTMAGIC));
  if ((sl->magic&~FLAGS_MASK) != SLOTMAGIC) {
    int zero = 0;
    pup_int(p, &zero);
  } else {
    pup_int(p, &sl->userSize);
    pup_comment(p, "value");
    pup_bytes(p, userData, sl->userSize);
  }
}

/********* Heap Checking ***********/

int charmEnvelopeSize = 0;

#include "pcqueue.h"

#ifdef CMK_SEPARATE_SLOT
#define SLOTSPACE 0
#define SLOT_ITERATE_START(scanner) \
  { \
    CkHashtableIterator_c hashiter = CkHashtableGetIterator(block_slots); \
    void *key; \
    while ((scanner = (Slot *)CkHashtableIteratorNext(hashiter, &key)) != NULL) {
#define SLOT_ITERATE_END \
    } \
    CkHashtableDestroyIterator(hashiter); \
  }
#else
#define SLOTSPACE sizeof(Slot)
#define SLOT_ITERATE_START(scanner) \
  for (scanner=slot_first->next; scanner!=slot_first; scanner=scanner->next) {
#define SLOT_ITERATE_END   }
#endif

/** Perform a scan of all the memory to find all the memory that is reacheable
 * from either the stack or the global variables. */
// FIXME: this function assumes that all memory is allocated in slot_unknown!
void check_memory_leaks(LeakSearchInfo *info) {
  //FILE* fd=fopen("check_memory_leaks", "w");
  // Step 1)
  // index all memory into a CkHashtable, with a scan of 4 bytes.
  CkHashtable_c table;
  PCQueue inProgress;
  Slot *sl, **fnd, *found;
  char *scanner;
  char *begin_stack, *end_stack;
  //char *begin_data, *end_data;
  //char *begin_bss, *end_bss;
  int growing_dimension = 0;

  // copy all the memory from "slot_first" to "leaking"

  Slot *slold1=0, *slold2=0, *slold3=0;

  memory_charmdebug_internal = 1;
  
  inProgress = PCQueueCreate();
  table = CkCreateHashtable_pointer(sizeof(char *), 10000);
  SLOT_ITERATE_START(sl)
    // index the i-th memory slot
    //printf("hashing slot %p\n",sl);
    char *ptr;
    sl->magic |= LEAK_FLAG;
    if (info->quick > 0) {
      char **object;
      //CmiPrintf("checking memory fast\n");
      // means index only specific offsets of the memory region
      ptr = SlotToUser(sl);
      object = (char**)CkHashtablePut(table, &ptr);
      *object = (char*)sl;
      ptr += 4;
      object = (char**)CkHashtablePut(table, &ptr);
      *object = (char*)sl;
      // beginning of converse header
      ptr += sizeof(CmiChunkHeader) - 4;
      if (ptr < SlotToUser(sl)+sizeof(Slot)+sl->userSize) {
        object = (char**)CkHashtablePut(table, &ptr);
        *object = (char*)sl;
      }
      // beginning of charm header
      ptr += CmiReservedHeaderSize;
      if (ptr < SlotToUser(sl)+sizeof(Slot)+sl->userSize) {
        object = (char**)CkHashtablePut(table, &ptr);
        *object = (char*)sl;
      }
      // beginning of ampi header
      ptr += charmEnvelopeSize - CmiReservedHeaderSize;
      if (ptr < SlotToUser(sl)+sizeof(Slot)+sl->userSize) {
        object = (char**)CkHashtablePut(table, &ptr);
        *object = (char*)sl;
      }
    } else {
      //CmiPrintf("checking memory extensively\n");
      // means index every fourth byte of the memory region
      for (ptr = SlotToUser(sl); ptr <= SlotToUser(sl)+sl->userSize; ptr+=sizeof(char*)) {
        //printf("memory %p\n",ptr);
        //growing_dimension++;
        //if ((growing_dimension&0xFF) == 0) printf("inserted %d objects\n",growing_dimension);
        char **object = (char**)CkHashtablePut(table, &ptr);
        *object = (char*)sl;
      }
    }
    slold3 = slold2;
    slold2 = slold1;
    slold1 = sl;
  SLOT_ITERATE_END

  // Step 2)
  // start the check with the stack and the global data. The stack is found
  // through the current pointer, going up until 16 bits filling (considering
  // the stack grows toward decreasing addresses). The pointers to the global
  // data (segments .data and .bss) are passed in with "req" as the "extra"
  // field, with the structure "begin .data", "end .data", "begin .bss", "end .bss".
  begin_stack = (char*)&table;
  end_stack = (char*)memory_stack_top;
  /*if (req->extraLen != 4*4 / *sizeof(char*) FIXME: assumes 64 bit addresses of .data and .bss are small enough * /) {
    CmiPrintf("requested for a memory leak check with wrong information! %d bytes\n",req->extraLen);
  }*/
  /*if (sizeof(char*) == 4) {
    / * 32 bit addresses; for 64 bit machines it assumes the following addresses were small enough * /
    begin_data = (char*)ntohl(((int*)(req->extra))[0]);
    end_data = (char*)ntohl(((int*)(req->extra))[1]) - sizeof(char*) + 1;
    begin_bss = (char*)ntohl(((int*)(req->extra))[2]);
    end_bss = (char*)ntohl(((int*)(req->extra))[3]) - sizeof(char*) + 1;
  / *} else {
    CmiAbort("not ready yet");
    begin_data = ntohl(((char**)(req->extra))[0]);
    end_data = ntohl(((char**)(req->extra))[1]) - sizeof(char*) + 1;
    begin_bss = ntohl(((char**)(req->extra))[2]);
    end_bss = ntohl(((char**)(req->extra))[3]) - sizeof(char*) + 1;
  }*/
  printf("scanning stack from %p to %p\n", (void *)begin_stack, (void *)end_stack);
  for (scanner = begin_stack; scanner < end_stack; scanner+=sizeof(char*)) {
    fnd = (Slot**)CkHashtableGet(table, scanner);
    //if (fnd != NULL) printf("scanning stack %p, %d\n",*fnd,isLeakSlot(*fnd));
    if (fnd != NULL && isLeakSlot(*fnd)) {
      found = *fnd;
      /* mark slot as not leak */
      //printf("stack pointing to %p\n",found+1);
      found->magic &= ~LEAK_FLAG;
      /* move the slot into inProgress */
      PCQueuePush(inProgress, (char*)found);
    }
  }
  printf("scanning data from %p to %p\n", (void *)info->begin_data, (void *)info->end_data);
  for (scanner = info->begin_data; scanner < info->end_data; scanner+=sizeof(char*)) {
    //fprintf(fd, "scanner = %p\n",scanner);
    //fflush(fd);
    fnd = (Slot**)CkHashtableGet(table, scanner);
    //if (fnd != NULL) printf("scanning data %p, %d\n",*fnd,isLeakSlot(*fnd));
    if (fnd != NULL && isLeakSlot(*fnd)) {
      found = *fnd;
      /* mark slot as not leak */
      //printf("data pointing to %p\n",found+1);
      found->magic &= ~LEAK_FLAG;
      /* move the slot into inProgress */
      PCQueuePush(inProgress, (char*)found);
    }
  }
  printf("scanning bss from %p to %p\n", (void *)info->begin_bss, (void *)info->end_bss);
  for (scanner = info->begin_bss; scanner < info->end_bss; scanner+=sizeof(char*)) {
    //printf("bss: %p %p\n",scanner,*(char**)scanner);
    fnd = (Slot**)CkHashtableGet(table, scanner);
    //if (fnd != NULL) printf("scanning bss %p, %d\n",*fnd,isLeakSlot(*fnd));
    if (fnd != NULL && isLeakSlot(*fnd)) {
      found = *fnd;
      /* mark slot as not leak */
      //printf("bss pointing to %p\n",found+1);
      found->magic &= ~LEAK_FLAG;
      /* move the slot into inProgress */
      PCQueuePush(inProgress, (char*)found);
    }
  }

  // Step 3)
  // continue iteratively to check the memory by sweeping it with the
  // "inProcess" list
  while ((sl = (Slot *)PCQueuePop(inProgress)) != NULL) {
    //printf("scanning memory %p of size %d\n",sl,sl->userSize);
    /* scan through this memory and pick all the slots which are still leaking
       and add them to the inProgress list */
    if (sl->extraStack != NULL && sl->extraStack->protectedMemory != NULL) mprotect(sl->extraStack->protectedMemory, sl->extraStack->protectedMemoryLength, PROT_READ);
    for (scanner = SlotToUser(sl); scanner < SlotToUser(sl)+sl->userSize-sizeof(char*)+1; scanner+=sizeof(char*)) {
      fnd = (Slot**)CkHashtableGet(table, scanner);
      //if (fnd != NULL) printf("scanning heap %p, %d\n",*fnd,isLeakSlot(*fnd));
      if (fnd != NULL && isLeakSlot(*fnd)) {
        found = *fnd;
        /* mark slot as not leak */
        //printf("heap pointing to %p\n",found+1);
        found->magic &= ~LEAK_FLAG;
        /* move the slot into inProgress */
        PCQueuePush(inProgress, (char*)found);
      }
    }
    if (sl->extraStack != NULL && sl->extraStack->protectedMemory != NULL) mprotect(sl->extraStack->protectedMemory, sl->extraStack->protectedMemoryLength, PROT_NONE);
  }

  // Step 4)
  // move back all the entries in leaking to slot_first
  /*if (leaking.next != &leaking) {
    leaking.next->prev = slot_first;
    leaking.prev->next = slot_first->next;
    slot_first->next->prev = leaking.prev;
    slot_first->next = leaking.next;
  }*/


  // mark all the entries in the leaking list as leak, and put them back
  // into the main list
  /*sl = leaking.next;
  while (sl != &leaking) {
    sl->magic | LEAK_FLAG;
  }
  if (leaking.next != &leaking) {
    slot_first->next->prev = leaking.prev;
    leaking.prev->next = slot_first->next;
    leaking.next->prev = slot_first;
    slot_first->next = leaking.next;
  }
  */

  PCQueueDestroy(inProgress);
  CkDeleteHashtable(table);

  memory_charmdebug_internal = 0;
}

void CpdMemoryMarkClean(char *msg) {
  Slot *sl;
  /* The first byte of the data packet indicates if we want o mark or unmark */
  if ((msg+CmiMsgHeaderSizeBytes)[0]) {
    SLOT_ITERATE_START(sl)
      sl->magic |= LEAK_CLEAN;
    SLOT_ITERATE_END
  } else {
    SLOT_ITERATE_START(sl)
      sl->magic &= ~LEAK_CLEAN;
    SLOT_ITERATE_END
  }
  CmiFree(msg);
}

/****************** memory allocation tree ******************/

/* This allows the representation and creation of a tree where each node
 * represents a line in the code part of a stack trace of a malloc. The node
 * contains how much data has been allocated starting from that line of code,
 * down the stack.
 */

typedef struct _AllocationPoint AllocationPoint;

struct _AllocationPoint {
  /* The stack pointer this allocation refers to */
  void * key;
  /* Pointer to the parent AllocationPoint of this AllocationPoint in the tree */
  AllocationPoint * parent;
  /* Pointer to the first child AllocationPoint in the tree */
  AllocationPoint * firstChild;
  /* Pointer to the sibling of this AllocationPoint (i.e the next child of the parent) */
  AllocationPoint * sibling;
  /* Pointer to the next AllocationPoint with the same key.
   * There can be more than one AllocationPoint with the same key because the
   * parent can be different. Used only in the hashtable. */
  AllocationPoint * next;
  /* Size of the memory allocate */
  int size;
  /* How many blocks have been allocated from this point */
  int count;
  /* Flags pertaining to the allocation point, currently only LEAK_FLAG */
  char flags;
};

// pup a single AllocationPoint. The data structure must be already allocated
void pupAllocationPointSingle(pup_er p, AllocationPoint *node, int *numChildren) {
  AllocationPoint *child;
  pup_pointer(p, &node->key);
  pup_int(p, &node->size);
  pup_int(p, &node->count);
  pup_char(p, &node->flags);
  if (pup_isUnpacking(p)) {
    node->parent = NULL;
    node->firstChild = NULL;
    node->sibling = NULL;
    node->next = NULL;
  }
  *numChildren = 0;
  for (child = node->firstChild; child != NULL; child = child->sibling) (*numChildren) ++;
  pup_int(p, numChildren);

}

// TODO: the following pup does not work for unpacking!
void pupAllocationPoint(pup_er p, void *data) {
  AllocationPoint *node = (AllocationPoint*)data;
  int numChildren;
  AllocationPoint *child;
  pupAllocationPointSingle(p, node, &numChildren);
  for (child = node->firstChild; child != NULL; child = child->sibling) {
    pupAllocationPoint(p, child);
  }
}

void deleteAllocationPoint(void *ptr) {
  AllocationPoint *node = (AllocationPoint*)ptr;
  AllocationPoint *child;
  for (child = node->firstChild; child != NULL; child = child->sibling) deleteAllocationPoint(child);
  mm_impl_free(node);
}

void printAllocationTree(AllocationPoint *node, FILE *fd, int depth) {
  int i;
  int numChildren = 0;
  AllocationPoint *child;

  if (node==NULL) return;
  for (child = node->firstChild; child != NULL; child = child->sibling) numChildren ++;
  for (i=0; i<depth; ++i) fprintf(fd, " ");
  fprintf(fd, "node %p: bytes=%d, count=%d, child=%d\n",(void *)node->key,node->size,node->count,numChildren);
  printAllocationTree(node->sibling, fd, depth);
  printAllocationTree(node->firstChild, fd, depth+2);
}

AllocationPoint * CreateAllocationTree(int *nodesCount) {
  Slot *scanner;
  CkHashtable_c table;
  int i, isnew;
  AllocationPoint *parent, **start, *cur;
  AllocationPoint *root = NULL;
  int numNodes = 0;
  char filename[100];
  FILE *fd;
  CkHashtableIterator_c it;
  AllocationPoint **startscan, *scan;

  table = CkCreateHashtable_pointer(sizeof(char *), 10000);

  root = (AllocationPoint*) mm_impl_malloc(sizeof(AllocationPoint));
  *(AllocationPoint**)CkHashtablePut(table, &numNodes) = root;
  numNodes ++;
  root->key = 0;
  root->parent = NULL;
  root->size = 0;
  root->count = 0;
  root->flags = 0;
  root->firstChild = NULL;
  root->sibling = NULL;
  root->next = root;

  SLOT_ITERATE_START(scanner)
    parent = root;
    for (i=scanner->stackLen-1; i>=0; --i) {
      isnew = 0;
      start = (AllocationPoint**)CkHashtableGet(table, &scanner->from[i]);
      if (start == NULL) {
        cur = (AllocationPoint*) mm_impl_malloc(sizeof(AllocationPoint));
        numNodes ++;
        isnew = 1;
        cur->next = cur;
        *(AllocationPoint**)CkHashtablePut(table, &scanner->from[i]) = cur;
      } else {
        for (cur = (*start)->next; cur != *start && cur->parent != parent; cur = cur->next);
        if (cur->parent != parent) {
          cur = (AllocationPoint*) mm_impl_malloc(sizeof(AllocationPoint));
          numNodes ++;
          isnew = 1;
          cur->next = (*start)->next;
          (*start)->next = cur;
        }
      }
      // here "cur" points to the correct AllocationPoint for this stack frame
      if (isnew) {
        cur->key = scanner->from[i];
        cur->parent = parent;
        cur->size = 0;
        cur->count = 0;
        cur->flags = 0;
        cur->firstChild = NULL;
        //if (parent == NULL) {
        //  cur->sibling = NULL;
        //  CmiAssert(root == NULL);
        //  root = cur;
        //} else {
          cur->sibling = parent->firstChild;
          parent->firstChild = cur;
        //}
      }
      cur->size += scanner->userSize;
      cur->count ++;
      cur->flags |= isLeakSlot(scanner);
      parent = cur;
    }
  SLOT_ITERATE_END

  sprintf(filename, "allocationTree_%d", CmiMyPe());
  fd = fopen(filename, "w");
  fprintf(fd, "digraph %s {\n", filename);
  it = CkHashtableGetIterator(table);
  while ((startscan=(AllocationPoint**)CkHashtableIteratorNext(it,NULL))!=NULL) {
    fprintf(fd, "\t\"n%p\" [label=\"%p\\nsize=%d\\ncount=%d\"];\n",(void *)*startscan,(void *)(*startscan)->key,
          (*startscan)->size,(*startscan)->count);
    for (scan = (*startscan)->next; scan != *startscan; scan = scan->next) {
      fprintf(fd, "\t\"n%p\" [label=\"%p\\nsize=%d\\ncount=%d\"];\n",(void *)scan,(void *)scan->key,scan->size,scan->count);
    }
  }
  CkHashtableIteratorSeekStart(it);
  while ((startscan=(AllocationPoint**)CkHashtableIteratorNext(it,NULL))!=NULL) {
    fprintf(fd, "\t\"n%p\" -> \"n%p\";\n",(void *)(*startscan)->parent,(void *)(*startscan));
    for (scan = (*startscan)->next; scan != *startscan; scan = scan->next) {
      fprintf(fd, "\t\"n%p\" -> \"n%p\";\n",(void *)scan->parent,(void *)scan);
    }
  }
  fprintf(fd, "}\n");
  fclose(fd);

  sprintf(filename, "allocationTree_%d.tree", CmiMyPe());
  fd = fopen(filename, "w");
  printAllocationTree(root, fd, 0);
  fclose(fd);

  CkDeleteHashtable(table);
  if (nodesCount != NULL) *nodesCount = numNodes;
  return root;
}

void MergeAllocationTreeSingle(AllocationPoint *node, AllocationPoint *remote, int numChildren, pup_er p) {
  AllocationPoint child;
  int numChildChildren;
  int i;
  //pupAllocationPointSingle(p, &remote, &numChildren);
  /* Update the node with the information coming from remote */
  node->size += remote->size;
  node->count += remote->count;
  node->flags |= remote->flags;
  /* Recursively merge the children */
  for (i=0; i<numChildren; ++i) {
    AllocationPoint *localChild;
    pupAllocationPointSingle(p, &child, &numChildChildren);
    /* Find the child in the local tree */
    for (localChild = node->firstChild; localChild != NULL; localChild = localChild->sibling) {
      if (localChild->key == child.key) {
        break;
      }
    }
    if (localChild == NULL) {
      /* This child did not exist locally, allocate it */
      localChild = (AllocationPoint*) mm_impl_malloc(sizeof(AllocationPoint));
      localChild->key = child.key;
      localChild->flags = 0;
      localChild->count = 0;
      localChild->size = 0;
      localChild->firstChild = NULL;
      localChild->next = NULL;
      localChild->parent = node;
      localChild->sibling = node->firstChild;
      node->firstChild = localChild;
    }
    MergeAllocationTreeSingle(localChild, &child, numChildChildren, p);
  }
}

void * MergeAllocationTree(size_t *size, void *data, void **remoteData, int numRemote) {
  int i;
  for (i=0; i<numRemote; ++i) {
    pup_er p = pup_new_fromMem(remoteData[i]);
    AllocationPoint root;
    int numChildren;
    pupAllocationPointSingle(p, &root, &numChildren);
    MergeAllocationTreeSingle((AllocationPoint*)data, &root, numChildren, p);
    pup_destroy(p);
  }
  return data;
}

/********************** Memory statistics ***********************/

/* Collect the statistics relative to the amount of memory allocated.
 * Starts from the statistics of a single processor and combines them to contain
 * all the processors in the application.
 */

typedef struct MemStatSingle {
  // [0] is total, [1] is the leaking part
  int pe;
  int sizes[2][5];
  int counts[2][5];
} MemStatSingle;

typedef struct MemStat {
  int count;
  MemStatSingle array[1];
} MemStat;

void pupMemStat(pup_er p, void *st) {
  int i;
  MemStat *comb = (MemStat*)st;
  pup_fmt_sync_begin_object(p);
  pup_comment(p, "count");
  pup_int(p, &comb->count);
  pup_comment(p, "list");
  pup_fmt_sync_begin_array(p);
  for (i=0; i<comb->count; ++i) {
    MemStatSingle *stat = &comb->array[i];
    pup_fmt_sync_item(p);
    pup_comment(p, "pe");
    pup_int(p, &stat->pe);
    pup_comment(p, "totalsize");
    pup_ints(p, stat->sizes[0], 5);
    pup_comment(p, "totalcount");
    pup_ints(p, stat->counts[0], 5);
    pup_comment(p, "leaksize");
    pup_ints(p, stat->sizes[1], 5);
    pup_comment(p, "leakcount");
    pup_ints(p, stat->counts[1], 5);
  }
  pup_fmt_sync_end_array(p);
  pup_fmt_sync_end_object(p);
}

void deleteMemStat(void *ptr) {
  mm_impl_free(ptr);
}

static int memStatReturnOnlyOne = 1;
void * mergeMemStat(size_t *size, void *data, void **remoteData, int numRemote) {
  int i,j,k;
  if (memStatReturnOnlyOne) {
    MemStatSingle *l = &((MemStat*) data)->array[0];
    MemStat r;
    MemStatSingle *m = &r.array[0];
    l->pe = -1;
    for (i=0; i<numRemote; ++i) {
      pup_er p = pup_new_fromMem(remoteData[i]);
      pupMemStat(p, &r);
      for (j=0; j<2; ++j) {
        for (k=0; k<5; ++k) {
          l->sizes[j][k] += m->sizes[j][k];
          l->counts[j][k] += m->counts[j][k];
        }
      }
      pup_destroy(p);
    }
    return data;
  } else {
    MemStat *l = (MemStat*)data, *res;
    MemStat r;
    int count = l->count;
    for (i=0; i<numRemote; ++i) count += ((MemStat*)remoteData[i])->count;
    res = (MemStat*)mm_impl_malloc(sizeof(MemStat) + (count-1)*sizeof(MemStatSingle));
    memset(res, 0, sizeof(MemStat)+(count-1)*sizeof(MemStatSingle));
    res->count = count;
    memcpy(res->array, l->array, l->count*sizeof(MemStatSingle));
    count = l->count;
    for (i=0; i<numRemote; ++i) {
      pup_er p = pup_new_fromMem(remoteData[i]);
      pupMemStat(p, &r);
      memcpy(&res->array[count], r.array, r.count*sizeof(MemStatSingle));
      count += r.count;
      pup_destroy(p);
    }
    deleteMemStat(data);
    return res;
  }
}

MemStat * CreateMemStat(void) {
  Slot *cur;
  MemStat *st;
  MemStatSingle *stat;
  st = (MemStat*)mm_impl_calloc(1, sizeof(MemStat));
  st->count = 1;
  stat = &st->array[0];
  SLOT_ITERATE_START(cur)
    stat->sizes[0][(cur->magic&0x7)] += cur->userSize;
    stat->counts[0][(cur->magic&0x7)] ++;
    if (cur->magic & 0x8) {
      stat->sizes[1][(cur->magic&0x7)] += cur->userSize;
      stat->counts[1][(cur->magic&0x7)] ++;
    }
  SLOT_ITERATE_END
  stat->pe=CmiMyPe();
  return st;
}


/*********************** Cross-chare corruption detection *******************/
static int reportMEM = 0;

/* This first method uses two fields (userCRC and slotCRC) of the Slot structure
 * to store the CRC32 checksum of the user data and the slot itself. It compares
 * the stored values against a new value recomputed after the entry method
 * returned to detect cross-chare corruption.
 */

static int CpdCRC32 = 0;

#define SLOT_CRC_LENGTH (sizeof(Slot) - 2*sizeof(CmiUInt8))

static int checkSlotCRC(void *userPtr) {
  Slot *sl = UserToSlot(userPtr);
  if (sl!=NULL) {
    unsigned int crc = crc32_initial((unsigned char*)sl, SLOT_CRC_LENGTH);
    crc = crc32_update((unsigned char*)sl->from, sl->stackLen*sizeof(void*), crc);
    return sl->slotCRC == crc;
  } else return 0;
}

static int checkUserCRC(void *userPtr) {
  Slot *sl = UserToSlot(userPtr);
  if (sl!=NULL) return sl->userCRC == crc32_initial((unsigned char*)userPtr, sl->userSize);
  else return 0;
}

static void resetUserCRC(void *userPtr) {
  Slot *sl = UserToSlot(userPtr);
  if (sl!=NULL) sl->userCRC = crc32_initial((unsigned char*)userPtr, sl->userSize);
}

static void resetSlotCRC(void *userPtr) {
  Slot *sl = UserToSlot(userPtr);
  if (sl!=NULL) {
    unsigned int crc = crc32_initial((unsigned char*)sl, SLOT_CRC_LENGTH);
    crc = crc32_update((unsigned char*)sl->from, sl->stackLen*sizeof(void*), crc);
    sl->slotCRC = crc;
  }
}

static void ResetAllCRC(void) {
  Slot *cur;
  unsigned int crc1, crc2;

  SLOT_ITERATE_START(cur)
    crc1 = crc32_initial((unsigned char*)cur, SLOT_CRC_LENGTH);
    crc1 = crc32_update((unsigned char*)cur->from, cur->stackLen*sizeof(void*), crc1);
    crc2 = crc32_initial((unsigned char*)SlotToUser(cur), cur->userSize);
    cur->slotCRC = crc1;
    cur->userCRC = crc2;
  SLOT_ITERATE_END
}

static void CheckAllCRC(void) {
  Slot *cur;
  unsigned int crc1, crc2;

  SLOT_ITERATE_START(cur)
    crc1 = crc32_initial((unsigned char*)cur, SLOT_CRC_LENGTH);
    crc1 = crc32_update((unsigned char*)cur->from, cur->stackLen*sizeof(void*), crc1);
    crc2 = crc32_initial((unsigned char*)SlotToUser(cur), cur->userSize);
    /* Here we can check if a modification has occured */
    if (reportMEM && cur->slotCRC != crc1) CmiPrintf("CRC: Object %d modified slot for %p\n",memory_chare_id,(void *)SlotToUser(cur));
    cur->slotCRC = crc1;
    if (reportMEM && cur->userCRC != crc2 && memory_chare_id != cur->chareID)
      CmiPrintf("CRC: Object %d modified memory of object %d for %p\n",memory_chare_id,cur->chareID,(void *)SlotToUser(cur));
    cur->userCRC = crc2;
  SLOT_ITERATE_END
}

/* This second method requires all the memory in the processor to be copied
 * into a safe region, and then compare it with the working copy after the
 * entry method returned.
 */

static int CpdMemBackup = 0;

static void backupMemory(void) {
  Slot *cur;
  char * ptr;
  int totalMemory = SLOTSPACE;
  if (*memoryBackup != NULL)
    CmiAbort("memoryBackup != NULL\n");

  {
    SLOT_ITERATE_START(cur)
      totalMemory += sizeof(Slot) + cur->userSize + cur->stackLen*sizeof(void*);
    SLOT_ITERATE_END
  }
  if (reportMEM) CmiPrintf("CPD: total memory in use (%d): %d\n",CmiMyPe(),totalMemory);
  *memoryBackup = (char *)mm_impl_malloc(totalMemory);

  ptr = *memoryBackup;
#ifndef CMK_SEPARATE_SLOT
  memcpy(*memoryBackup, slot_first, sizeof(Slot));
  ptr += sizeof(Slot);
#endif
  SLOT_ITERATE_START(cur)
    int tocopy = SLOTSPACE + cur->userSize + cur->stackLen*sizeof(void*);
    char *data = (char *)cur;
#ifdef CMK_SEPARATE_SLOT
    memcpy(ptr, cur, sizeof(Slot));
    ptr += sizeof(Slot);
    data = SlotToUser(cur);
#endif
    memcpy(ptr, data, tocopy);
    cur->magic &= ~ (NEW_BLOCK | MODIFIED);
    ptr += tocopy;
  SLOT_ITERATE_END
  allocatedSinceSize = 0;
}

static void checkBackup(void) {
#ifndef CMK_SEPARATE_SLOT
  Slot *cur = slot_first->next;
  char *ptr = *memoryBackup + sizeof(Slot);

  // skip over the new allocated blocks
  //while (cur != ((Slot*)*memoryBackup)->next) cur = cur->next;
  int idx = allocatedSinceSize-1;
  while (idx >= 0) {
    while (idx >= 0 && allocatedSince[idx] != cur) idx--;
    if (idx >= 0) {
      cur = cur->next;
      idx --;
    }
  }

  while (cur != slot_first) {
    char *last;
    // ptr is the old copy of cur
    if (memory_chare_id != cur->chareID) {
      int res = memcmp(ptr+sizeof(Slot), ((char*)cur)+sizeof(Slot), cur->userSize + cur->stackLen*sizeof(void*));
      if (res != 0) {
        cur->magic |= MODIFIED;
        if (reportMEM) CmiPrintf("CPD: Object %d modified memory of object %d for %p on pe %d\n",memory_chare_id,cur->chareID,(void *)(cur+1),CmiMyPe());
      }
    }

    // advance to next, skipping deleted memory blocks
    cur = cur->next;
    do {
      last = ptr;
      ptr += sizeof(Slot) + ((Slot*)ptr)->userSize + ((Slot*)ptr)->stackLen*sizeof(void*);
    } while (((Slot*)last)->next != cur);
  }
#endif

  mm_impl_free(*memoryBackup);
  *memoryBackup = NULL;
}

/* Third method to detect cross-chare corruption. Use mprotect to change the
 * protection bits of each page, and a following segmentation fault to detect
 * the corruption. It is more accurate as it can provide the stack trace of the
 * first instruction that modified the memory.
 */

#include <signal.h>

static int CpdMprotect = 0;

static void** unProtectedPages = NULL;
static int unProtectedPagesSize = 0;
static int unProtectedPagesMaxSize = 0;

static void* lastAddressSegv;
static void CpdMMAPhandler(int sig, siginfo_t *si, void *unused){
  void *pageToUnprotect;
  if (lastAddressSegv == si->si_addr) {
    CmiPrintf("Second SIGSEGV at address 0x%lx\n", (long) si->si_addr);
    CpdFreeze();
  }
  lastAddressSegv = si->si_addr;
  pageToUnprotect = (void*)(uintptr_t)((CmiUInt8)(uintptr_t)si->si_addr & ~(CmiGetPageSize()-1));
  mprotect(pageToUnprotect, 4, PROT_READ|PROT_WRITE);
  if (unProtectedPagesSize >= unProtectedPagesMaxSize) {
    void **newUnProtectedPages;
    unProtectedPagesMaxSize += 10;
    newUnProtectedPages = (void**)mm_impl_malloc((unProtectedPagesMaxSize)*sizeof(void*));
    memcpy(newUnProtectedPages, unProtectedPages, unProtectedPagesSize*sizeof(void*));
    mm_impl_free(unProtectedPages);
    unProtectedPages = newUnProtectedPages;
  }
  unProtectedPages[unProtectedPagesSize++] = pageToUnprotect;
  if (reportMEM) CpdNotify(CPD_CROSSCORRUPTION, si->si_addr, memory_chare_id);
  //CmiPrintf("Got SIGSEGV at address: 0x%lx\n", (long) si->si_addr);
  //CmiPrintStackTrace(0);
}

static void protectMemory(void) {
#ifdef CPD_USE_MMAP
  Slot *cur;
  /*printf("protecting memory (chareid=%d)",memory_chare_id);*/
  SLOT_ITERATE_START(cur)
    if (cur->chareID != memory_chare_id && cur->chareID > 0) {
      /*printf(" %p",cur->userData);*/
#ifdef CMK_SEPARATE_SLOT
      char * data = cur->userData;
#else
      char * data = (char *)cur;
#endif
      cur->magic |= BLOCK_PROTECTED;
      mprotect(data, cur->userSize+SLOTSPACE+cur->stackLen*sizeof(void*), PROT_READ);
    } /*else printf(" (%p)",cur->userData);*/
  SLOT_ITERATE_END
  /*printf("\n");*/
#endif
}

static void unProtectMemory(void) {
#ifdef CPD_USE_MMAP
  Slot *cur;
  SLOT_ITERATE_START(cur)
#ifdef CMK_SEPARATE_SLOT
      char * data = cur->userData;
#else
      char * data = (char *)cur;
#endif
    mprotect(data, cur->userSize+SLOTSPACE+cur->stackLen*sizeof(void*), PROT_READ|PROT_WRITE);
    cur->magic &= ~BLOCK_PROTECTED;
  SLOT_ITERATE_END
  /*printf("unprotecting memory\n");*/
#endif
}

/** Called before the entry method: resets all current memory for the chare
 * receiving the message.
 */
void CpdResetMemory(void) {
  if (CpdMemBackup) backupMemory();
  if (CpdCRC32) ResetAllCRC();
  if (CpdMprotect) protectMemory();
}

/** Called after the entry method to check if the chare that just received the
 * message has corrupted the memory of some other chare, or some system memory.
 */
void CpdCheckMemory(void) {
  Slot *cur;
  if (CpdMprotect) unProtectMemory();
  if (CpdCRC32) CheckAllCRC();
  if (CpdMemBackup) checkBackup();
  SLOT_ITERATE_START(cur)
    if (cur->magic == SLOTMAGIC_FREED) CmiAbort("SLOT deallocate in list");
    if (cur->from == NULL) printf("SLOT %p has no stack\n",(void *)cur);
#ifndef CMK_SEPARATE_SLOT
    if (cur->next == NULL) printf("SLOT %p has null next!\n",(void *)cur);
#endif
  SLOT_ITERATE_END
}

void CpdSystemEnter(void) {
#ifdef CPD_USE_MMAP
  Slot *cur;
  if (++cpdInSystem == 1) {
    if (CpdMprotect) {
      int count=0;
      SLOT_ITERATE_START(cur)
        if (cur->chareID == 0) {
#ifdef CMK_SEPARATE_SLOT
          char * data = cur->userData;
#else
          char * data = (char *)cur;
#endif
          mprotect(data, cur->userSize+SLOTSPACE+cur->stackLen*sizeof(void*), PROT_READ|PROT_WRITE);
          cur->magic &= ~BLOCK_PROTECTED;
          count++;
        }
      SLOT_ITERATE_END
      //printf("CpdSystemEnter: unprotected %d elements\n",count);
    }
  }
#endif
}

void CpdSystemExit(void) {
#ifdef CPD_USE_MMAP
  Slot *cur;
  int i;
  if (--cpdInSystem == 0) {
    if (CpdMprotect) {
      int count=0;
      SLOT_ITERATE_START(cur)
        if (cur->chareID == 0) {
#ifdef CMK_SEPARATE_SLOT
          char * data = cur->userData;
#else
          char * data = (char *)cur;
#endif
          cur->magic |= BLOCK_PROTECTED;
          mprotect(data, cur->userSize+SLOTSPACE+cur->stackLen*sizeof(void*), PROT_READ);
          count++;
        }
      SLOT_ITERATE_END
      //printf("CpdSystemExit: protected %d elements\n",count);
      /* unprotect the pages that have been unprotected by a signal SEGV */
      for (i=0; i<unProtectedPagesSize; ++i) {
        mprotect(unProtectedPages[i], 4, PROT_READ|PROT_WRITE);
      }
    }
  }
#endif
}


/*

/ *Head of the current circular allocated block list* /
Slot slot_first_storage={&slot_first_storage,&slot_first_storage};
Slot *slot_first=&slot_first_storage;

#define CMI_MEMORY_ROUTINES 1

/ * Mark all allocated memory as being OK * /
static inline void CmiMemoryMarkImpl(void) {
	/ * Just make a new linked list of slots * /
	slot_first=(Slot *)mm_impl_malloc(sizeof(Slot));
	slot_first->next=slot_first->prev=slot_first;
}
void CmiMemoryMark(void) {
	CmiMemLock();
	CmiMemoryMarkImpl();
	CmiMemUnlock();
}

/ * Mark this allocated block as being OK * /
void CmiMemoryMarkBlock(void *blk) {
	Slot *s=Slot_fmUser(blk);
	CmiMemLock();
	if (s->magic!=SLOTMAGIC) CmiAbort("CmiMemoryMarkBlock called on non-malloc'd block!\n");
	/ * Splice us out of the current linked list * /
	s->next->prev=s->prev;
	s->prev->next=s->next;
	s->prev=s->next=s; / * put us in our own list * /
	CmiMemUnlock();
}

/ * Print out all allocated memory * /
void CmiMemorySweep(const char *where) {
	Slot *cur;
	int nBlocks=0,nBytes=0;
	CmiMemLock();
	cur=slot_first->next;
	CmiPrintf("[%d] ------- LEAK CHECK: %s -----\n",CmiMyPe(), where);
	while (cur!=slot_first) {
		printSlot(cur);
		nBlocks++; nBytes+=cur->userSize;
		cur=cur->next;
	}
	if (nBlocks) {
		CmiPrintf("[%d] Total leaked memory: %d blocks, %d bytes\n",
			CmiMyPe(),nBlocks,nBytes);
		/ * CmiAbort("Memory leaks detected!\n"); * /
	}
	CmiMemoryMarkImpl();
	CmiMemUnlock();
}
void CmiMemoryCheck(void) {}
*/


/********** Allocation/Free ***********/

static int stackTraceDisabled = 0;
#define MAX_STACK_FRAMES   2048
static int numStackFrames; // the number of frames presetn in stackFrames - 4 (this number is trimmed at 0
static void *stackFrames[MAX_STACK_FRAMES];

static void dumpStackFrames(void) {
  numStackFrames=MAX_STACK_FRAMES;
  if (stackTraceDisabled==0) {
    stackTraceDisabled = 1;
    CmiBacktraceRecordHuge(stackFrames,&numStackFrames);
    stackTraceDisabled = 0;
    numStackFrames-=4;
    if (numStackFrames < 0) numStackFrames = 0;
  } else {
    numStackFrames=0;
    stackFrames[0] = (void*)0;
  }
}

/* Write a valid slot to this field */
static void *setSlot(Slot **sl,int userSize) {
#ifdef CMK_SEPARATE_SLOT
  Slot *s;
  char *user;

  static int mallocFirstTime = 1;
  if (mallocFirstTime) {
    mallocFirstTime = 0;
    memory_charmdebug_internal = 1;
    block_slots = CkCreateHashtable_pointer(sizeof(Slot), 10000);
    memory_charmdebug_internal = 0;
  }
  
  user = (char *)*sl;
  memory_charmdebug_internal = 1;
  s = (Slot *)CkHashtablePut(block_slots, sl);
  memory_charmdebug_internal = 0;
  *sl = s;

  s->userData = user;
#else
  Slot *s = *sl;
  char *user=(char*)(s+1);

  /* Splice into the slot list just past the head (part 1) */
  s->next=slot_first->next;
  s->prev=slot_first;
  /* Handle correctly memory protection while changing neighbor blocks */
  if (CpdMprotect) {
    mprotect(s->next, 4, PROT_READ | PROT_WRITE);
    mprotect(s->prev, 4, PROT_READ | PROT_WRITE);
  }
  /* Splice into the slot list just past the head (part 2) */
  s->next->prev=s;
  s->prev->next=s;

  if (CpdCRC32) {
    /* fix crc for previous and next block */
    resetSlotCRC(s->next + 1);
    resetSlotCRC(s->prev + 1);
  }
  if (CpdMprotect) {
    if (isProtected(s->next)) mprotect(s->next, 4, PROT_READ);
    if (isProtected(s->prev)) mprotect(s->prev, 4, PROT_READ);
  }
#endif

  /* Set the last 4 bits of magic to classify the memory together with the magic */
  s->magic=SLOTMAGIC + NEW_BLOCK + (memory_status_info>0? USER_TYPE : SYSTEM_TYPE);
  //if (memory_status_info>0) printf("user allocation\n");
  s->chareID = memory_chare_id;
  s->userSize=userSize;
  s->extraStack=(SlotStack *)0;
  
  /* Set the stack frames */
  s->stackLen=numStackFrames;
  s->from=(void**)(user+userSize);
  memcpy(s->from, &stackFrames[4], numStackFrames*sizeof(void*));

  if (CpdCRC32) {
    unsigned int crc = crc32_initial((unsigned char*)s, SLOT_CRC_LENGTH);
    s->slotCRC = crc32_update((unsigned char*)s->from, numStackFrames*sizeof(void*), crc);
    s->userCRC = crc32_initial((unsigned char*)user, userSize);
  }
  if (saveAllocationHistory) {
    if (allocatedSinceSize >= allocatedSinceMaxSize) {
      Slot **newAllocatedSince;
      allocatedSinceMaxSize += 10;
      newAllocatedSince = (Slot**)mm_impl_malloc((allocatedSinceMaxSize)*sizeof(Slot*));
      memcpy(newAllocatedSince, allocatedSince, allocatedSinceSize*sizeof(Slot*));
      mm_impl_free(allocatedSince);
      allocatedSince = newAllocatedSince;
    }
    allocatedSince[allocatedSinceSize++] = s;
  }
  lastMemoryAllocated = user;

  return (void *)user;
}

/* Delete this slot structure */
static void freeSlot(Slot *s) {
#ifdef CMK_SEPARATE_SLOT
  /* Don't delete it from the hash table, simply mark it as freed */
  int removed = CkHashtableRemove(block_slots, &s->userData);
  CmiAssert(removed);
  /* WARNING! After the slot has been removed from the hashtable,
   * the pointer "s" becomes invalid.
   */
#else
  /* Handle correctly memory protection while changing neighbor blocks */
  if (CpdMprotect) {
    mprotect(s->next, 4, PROT_READ | PROT_WRITE);
    mprotect(s->prev, 4, PROT_READ | PROT_WRITE);
  }
  /* Splice out of the slot list */
  s->next->prev=s->prev;
  s->prev->next=s->next;
  if (CpdCRC32) {
    /* fix crc for previous and next block */
    resetSlotCRC(s->next + 1);
    resetSlotCRC(s->prev + 1);
  }
  if (CpdMprotect) {
    if (isProtected(s->next)) mprotect(s->next, 4, PROT_READ);
    if (isProtected(s->prev)) mprotect(s->prev, 4, PROT_READ);
  }
  s->prev=s->next=(Slot *)0;//0x0F00; why was it not 0?

  s->magic=SLOTMAGIC_FREED;
  s->userSize=-1;
#endif
}

/********** meta_ routines ***********/

/* Only display startup status messages from processor 0 */
static void status(const char *msg) {
  if (CmiMyPe()==0 && !CmiArgGivingUsage()) {
    CmiPrintf("%s",msg);
  }
}

extern int getCharmEnvelopeSize(void);

static int disableVerbosity = 1;
int cpdInitializeMemory;
void CpdSetInitializeMemory(int v) { cpdInitializeMemory = v; }

static void meta_init(char **argv) {
  if (CmiMyRank()==0) {
    char buf[100];
    status("Converse -memory mode: charmdebug\n");
    sprintf(buf, "slot size %d\n", (int)sizeof(Slot));
    status(buf);
    CmiMemoryIs_flag|=CMI_MEMORY_IS_CHARMDEBUG;
    cpdInitializeMemory = 0;
    charmEnvelopeSize = getCharmEnvelopeSize();
    CpdDebugGetAllocationTree = (void* (*)(int*))CreateAllocationTree;
    CpdDebug_pupAllocationPoint = pupAllocationPoint;
    CpdDebug_deleteAllocationPoint = deleteAllocationPoint;
    CpdDebug_MergeAllocationTree = MergeAllocationTree;
    CpdDebugGetMemStat = (void* (*)(void))CreateMemStat;
    CpdDebug_pupMemStat = pupMemStat;
    CpdDebug_deleteMemStat = deleteMemStat;
    CpdDebug_mergeMemStat = mergeMemStat;
    memory_allocated_user_total = 0;
    nextChareID = 1;
#ifndef CMK_SEPARATE_SLOT
    slot_first->userSize = 0;
    slot_first->stackLen = 0;
#endif
  }
  if (CmiGetArgFlagDesc(argv,"+memory_report", "Print all cross-object violations")) {
    if (CmiMyRank()==0) reportMEM = 1;
  }
  if (CmiGetArgFlagDesc(argv,"+memory_backup", "Backup all memory at every entry method")) {
    if (CmiMyRank()==0) {
      CpdMemBackup = 1;
      saveAllocationHistory = 1;
    }
  }
  if (CmiGetArgFlagDesc(argv,"+memory_crc", "Use CRC32 to detect memory changes")) {
    if (CmiMyRank()==0) CpdCRC32 = 1;
  }
#ifdef CPD_USE_MMAP
  if (CmiGetArgFlagDesc(argv,"+memory_mprotect", "Use mprotect to protect memory of other chares")) {
    if (CmiMyRank()==0) {
      struct sigaction sa;
      CpdMprotect = 1;
      sa.sa_flags = SA_SIGINFO | SA_NODEFER | SA_RESTART;
      sigemptyset(&sa.sa_mask);
      sa.sa_sigaction = CpdMMAPhandler;
      if (sigaction(SIGSEGV, &sa, NULL) == -1) CmiPrintf("failed to install signal handler\n");
    }
  }
#endif
  if (CmiGetArgFlagDesc(argv,"+memory_verbose", "Print all memory-related operations")) {
    if (CmiMyRank()==0) disableVerbosity = 0;
  }
  if (CmiGetArgFlagDesc(argv,"+memory_nostack", "Do not collect stack traces for memory allocations")) {
    if (CmiMyRank()==0) stackTraceDisabled = 1;
  }
}

static void *meta_malloc(size_t size) {
  void *user = NULL;
  CmiMemLock();
  if (memory_charmdebug_internal==0) {
    Slot *s;
    dumpStackFrames();
#if CPD_USE_MMAP
    s=(Slot *)mmap(NULL, SLOTSPACE+size+numStackFrames*sizeof(void*), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#else
    s=(Slot *)mm_impl_malloc(SLOTSPACE+size+numStackFrames*sizeof(void*));
#endif
    if (s!=NULL) {
      user = (char*)setSlot(&s,size);
      memory_allocated_user_total += size;
      traceMalloc_c(user, size, s->from, s->stackLen);
    }
    if (disableVerbosity == 0) {
      disableVerbosity = 1;
      CmiPrintf("allocating %p: %zu bytes\n",(void *)s,size);
      disableVerbosity = 0;
    }
  } else {
    user = mm_impl_malloc(size);
  }
  CmiMemUnlock();
  if (cpdInitializeMemory) {
    memset(user, 0, size); // for Record-Replay must initialize all memory otherwise paddings may differ (screwing up the CRC)
  }
  return user;
}

static void meta_free(void *mem) {
  if (mem==NULL) return; /*Legal, but misleading*/
  CmiMemLock();
  if (memory_charmdebug_internal==0) {
    int memSize;
    Slot *s;

    s=UserToSlot(mem);
#if CMK_MEMORY_BUILD_OS
    /* In this situation, we can have frees that were not allocated by our malloc...
     * for now simply skip over them. */
    if (s == NULL || ((s->magic&~FLAGS_MASK) != SLOTMAGIC_VALLOC &&
        (s->magic&~FLAGS_MASK) != SLOTMAGIC)) {
      mm_impl_free(mem);
      CmiMemUnlock();
      return;
    }
#endif

    /* Check that the memory is really allocated, and we can use its slot */
    /* TODO */

    if (s == NULL ||
        ((s->magic&~FLAGS_MASK) != SLOTMAGIC &&
            (s->magic&~FLAGS_MASK) != SLOTMAGIC_FREED &&
            (s->magic&~FLAGS_MASK) != SLOTMAGIC_VALLOC)) {
      CmiAbort("Free'd non-malloc'd block");
    }
#ifdef CMK_SEPARATE_SLOT
    CmiAssert(s->userData == mem);
#endif
    
    memSize = 0;
    if (mem!=NULL) memSize = s->userSize;
    memory_allocated_user_total -= memSize;
    traceFree_c(mem, memSize);

    if (disableVerbosity == 0) {
      disableVerbosity = 1;
      CmiPrintf("freeing %p\n",(void *)mem);
      disableVerbosity = 0;
    }

    /*Overwrite stack trace with the one of the free*/
    dumpStackFrames();
    if (s->stackLen > numStackFrames) s->stackLen=numStackFrames;
    memcpy(s->from, &stackFrames[4], s->stackLen*sizeof(void*));

    if ((s->magic&~FLAGS_MASK)==SLOTMAGIC_VALLOC)
    { /*Allocated with special alignment*/
      void *ptr = s->extraStack;
      freeSlot(s);
      mm_impl_free(ptr);
      /*mm_impl_free(((char *)mem)-CmiGetPageSize());*/
    }
    else if ((s->magic&~FLAGS_MASK)==SLOTMAGIC)
    { /*Ordinary allocated block */
      int freeSize=SLOTSPACE+s->userSize+s->stackLen*sizeof(void*);
      void *ptr;
      freeSlot(s);
#ifdef CMK_SEPARATE_SLOT
      ptr = mem;
#else
      ptr = s;
#endif
#if CPD_USE_MMAP
      munmap(ptr, freeSize);
#else
      mm_impl_free(ptr);
#endif
    }
    else if (s->magic==SLOTMAGIC_FREED)
      CmiAbort("Free'd block twice");
    else /*Unknown magic number*/
      CmiAbort("Free'd non-malloc'd block");
  } else {
    mm_impl_free(mem);
  }
  CmiMemUnlock();
}

static void *meta_calloc(size_t nelem, size_t size) {
  void *area=meta_malloc(nelem*size);
  if (area != NULL) memset(area,0,nelem*size);
  return area;
}

static void meta_cfree(void *mem) {
  meta_free(mem);
}

static void *meta_realloc(void *oldBuffer, size_t newSize) {
  void *newBuffer = meta_malloc(newSize);
  if ( newBuffer && oldBuffer ) {
    /*Preserve old buffer contents*/
    CmiMemLock();
    Slot *o=UserToSlot(oldBuffer);
    size_t size=o->userSize;
    CmiMemUnlock();
    if (size>newSize) size=newSize;
    if (size > 0)
      memcpy(newBuffer, oldBuffer, size);

    meta_free(oldBuffer);
  }
  return newBuffer;
}

static void *meta_memalign(size_t align, size_t size) {
  int overhead = align;
  char *alloc;
  Slot *s;
  void *user;

  while (overhead < SLOTSPACE+sizeof(SlotStack)) overhead += align;

  CmiMemLock();
  /* Allocate the required size + the overhead needed to keep the user alignment */
  dumpStackFrames();

  alloc=(char *)mm_impl_memalign(align,overhead+size+numStackFrames*sizeof(void*));
  s=(Slot*)(alloc+overhead-SLOTSPACE);
  user=setSlot(&s,size);
  s->magic = SLOTMAGIC_VALLOC + (s->magic&0xF);
  s->extraStack = (SlotStack *)alloc; /* use the extra space as stack */
  s->extraStack->protectedMemory = NULL;
  s->extraStack->protectedMemoryLength = 0;
  memory_allocated_user_total += size;
  CmiMemUnlock();
  traceMalloc_c(user, size, s->from, s->stackLen);
  return user;
}

static int meta_posix_memalign(void **outptr, size_t align, size_t size) {
  int overhead = align;
  int ret;
  char *alloc;
  Slot *s;
  void *user;

  while (overhead < SLOTSPACE+sizeof(SlotStack)) overhead += align;

  CmiMemLock();
  /* Allocate the required size + the overhead needed to keep the user alignment */
  dumpStackFrames();

  ret = mm_impl_posix_memalign(outptr,align,overhead+size+numStackFrames*sizeof(void*));
  alloc=(char *)*outptr;
  if (ret != 0)
  {
    CmiMemUnlock();
    return ret;
  }
  s=(Slot*)(alloc+overhead-SLOTSPACE);
  user=setSlot(&s,size);
  s->magic = SLOTMAGIC_VALLOC + (s->magic&0xF);
  s->extraStack = (SlotStack *)alloc; /* use the extra space as stack */
  s->extraStack->protectedMemory = NULL;
  s->extraStack->protectedMemoryLength = 0;
  memory_allocated_user_total += size;
  CmiMemUnlock();
  traceMalloc_c(user, size, s->from, s->stackLen);
  *outptr = user;
  return 0;
}

static void *meta_aligned_alloc(size_t align, size_t size) {
  int overhead = align;
  char *alloc;
  Slot *s;
  void *user;

  while (overhead < SLOTSPACE+sizeof(SlotStack)) overhead += align;

  CmiMemLock();
  /* Allocate the required size + the overhead needed to keep the user alignment */
  dumpStackFrames();

  alloc=(char *)mm_impl_aligned_alloc(align,overhead+size+numStackFrames*sizeof(void*));
  s=(Slot*)(alloc+overhead-SLOTSPACE);
  user=setSlot(&s,size);
  s->magic = SLOTMAGIC_VALLOC + (s->magic&0xF);
  s->extraStack = (SlotStack *)alloc; /* use the extra space as stack */
  s->extraStack->protectedMemory = NULL;
  s->extraStack->protectedMemoryLength = 0;
  memory_allocated_user_total += size;
  CmiMemUnlock();
  traceMalloc_c(user, size, s->from, s->stackLen);
  return user;
}

static void *meta_valloc(size_t size) {
  return meta_memalign(CmiGetPageSize(),size);
}

static void *meta_pvalloc(size_t size) {
  const size_t pagesize = CmiGetPageSize();
  return meta_memalign(pagesize, (size + pagesize - 1) & ~(pagesize - 1));
}

void setProtection(char* mem, char *ptr, int len, int flag) {
  CmiMemLock();
  Slot *sl = UserToSlot(mem);
  if (sl->extraStack == NULL) CmiAbort("Tried to protect memory not memaligned\n");
  if (flag != 0) {
    sl->extraStack->protectedMemory = ptr;
    sl->extraStack->protectedMemoryLength = len;
  } else {
    sl->extraStack->protectedMemory = NULL;
    sl->extraStack->protectedMemoryLength = 0;
  }
  CmiMemUnlock();
}

void **chareObjectMemory = NULL;
int chareObjectMemorySize = 0;

void setMemoryTypeChare(void *ptr) {
  CmiMemLock();
  Slot *sl = UserToSlot(ptr);
  sl->magic = (sl->magic & ~FLAGS_MASK) | CHARE_TYPE;
  sl->chareID = nextChareID;
  if (nextChareID >= chareObjectMemorySize) {
    void **newChare;
    newChare = (void**)mm_impl_malloc((nextChareID+100) * sizeof(void*));
    memcpy(newChare, chareObjectMemory, chareObjectMemorySize*sizeof(void*));
    chareObjectMemorySize = nextChareID+100;
    mm_impl_free(chareObjectMemory);
    chareObjectMemory = newChare;
  }
  chareObjectMemory[nextChareID] = ptr;
  nextChareID ++;
  CmiMemUnlock();
}

/* The input parameter is the pointer to the envelope, after the CmiChunkHeader */
void setMemoryTypeMessage(void *ptr) {
  CmiMemLock();
  void *realptr = (char*)ptr - sizeof(CmiChunkHeader);
  Slot *sl = UserToSlot(realptr);
  if ((sl->magic&~FLAGS_MASK) == SLOTMAGIC || (sl->magic&~FLAGS_MASK) == SLOTMAGIC_VALLOC) {
    sl->magic = (sl->magic & ~FLAGS_MASK) | MESSAGE_TYPE;
  }
  CmiMemUnlock();
}

int setMemoryChareIDFromPtr(void *ptr) {
  CmiMemLock();
  int tmp = memory_chare_id;
  if (ptr == NULL || ptr == 0) memory_chare_id = 0;
  else memory_chare_id = UserToSlot(ptr)->chareID;
  CmiMemUnlock();
  return tmp;
}

void setMemoryChareID(int chareID) {
  CmiMemLock();
  memory_chare_id = chareID;
  CmiMemUnlock();
}

void setMemoryOwnedBy(void *ptr, int chareID) {
  CmiMemLock();
  Slot *sl = UserToSlot(ptr);
  sl->chareID = chareID;
  CmiMemUnlock();
}

void * MemoryToSlot(void *ptr) {
  Slot *sl;
  int i;
#if defined(CPD_USE_MMAP) && defined(CMK_SEPARATE_SLOT)
  CmiMemLock();
  for (i=0; i<1000; ++i) {
    sl = UserToSlot((void*)(intptr_t)(((CmiUInt8)(intptr_t)ptr)-i*CmiGetPageSize() & ~(CmiGetPageSize()-1)));
    if (sl != NULL)
    {
      CmiMemUnlock();
      return sl;
    }
  }
  CmiMemUnlock();
#endif
  return NULL;
}

/*void PrintDebugStackTrace(void *ptr) {
  int i;
  CmiMemLock();
  Slot *sl = UserToSlot((void*)(((CmiUInt8)ptr) & ~(CmiGetPageSize()-1)));
  if (sl != NULL) {
    CmiPrintf("%d %d ",sl->chareID,sl->stackLen);
    for (i=0; i<sl->stackLen; ++i) CmiPrintf("%p ",sl->from[i]);
  } else {
    CmiPrintf("%d 0 ",sl->chareID);
  }
  CmiMemUnlock();
}*/


#endif
