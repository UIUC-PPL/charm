#include "conv-config.h"

#if CMK_DLL_USE_DLOPEN && CMK_HAS_RTLD_NEXT

/* These macros are needed for:
 * dlfcn.h: RTLD_NEXT
 */
#ifndef _GNU_SOURCE
# define _GNU_SOURCE
#endif
#ifndef __USE_GNU
# define __USE_GNU
#endif

#include <sys/types.h>
#include <dlfcn.h>

extern void * (*mm_impl_malloc)(size_t);
extern void * (*mm_impl_calloc)(size_t,size_t);
extern void * (*mm_impl_realloc)(void*,size_t);
extern void (*mm_impl_free)(void*);

extern void * (*mm_impl_memalign)(size_t,size_t);
extern int (*mm_impl_posix_memalign)(void **,size_t,size_t);
extern void * (*mm_impl_aligned_alloc)(size_t,size_t);

#if CMK_HAS_VALLOC
extern void * (*mm_impl_valloc)(size_t);
#endif
#if CMK_HAS_PVALLOC
extern void * (*mm_impl_pvalloc)(size_t);
#endif
#if CMK_HAS_CFREE
extern void (*mm_impl_cfree)(void*);
#endif
#if CMK_HAS_MALLINFO2
struct mallinfo2;
extern struct mallinfo2 (*mm_impl_mallinfo)(void);
#elif CMK_HAS_MALLINFO
struct mallinfo;
extern struct mallinfo (*mm_impl_mallinfo)(void);
#endif

  
extern char initialize_memory_wrapper_status;
char initialize_memory_wrapper_status;

void initialize_memory_wrapper() {
  initialize_memory_wrapper_status = 1;

  // wait to install these all at once because dlsym calls them, and a mismatch would be bad
  auto os_malloc = (void *(*)(size_t)) dlsym(RTLD_NEXT, "malloc");
  auto os_calloc = (void *(*)(size_t,size_t)) dlsym(RTLD_NEXT, "calloc");
  auto os_realloc = (void *(*)(void*,size_t)) dlsym(RTLD_NEXT, "realloc");
  auto os_free = (void (*)(void*)) dlsym(RTLD_NEXT, "free");

  auto os_memalign = (void *(*)(size_t,size_t)) dlsym(RTLD_NEXT, "memalign");
  auto os_posix_memalign = (int (*)(void **,size_t,size_t)) dlsym(RTLD_NEXT, "posix_memalign");
  auto os_aligned_alloc = (void *(*)(size_t,size_t)) dlsym(RTLD_NEXT, "aligned_alloc");

#if CMK_HAS_VALLOC
  auto os_valloc = (void *(*)(size_t)) dlsym(RTLD_NEXT, "valloc");
#endif
#if CMK_HAS_PVALLOC
  auto os_pvalloc = (void *(*)(size_t)) dlsym(RTLD_NEXT, "pvalloc");
#endif
#if CMK_HAS_CFREE
  auto os_cfree = (void (*)(void*)) dlsym(RTLD_NEXT, "cfree");
#endif
#if CMK_HAS_MALLINFO2
  auto os_mallinfo2 = (struct mallinfo2 (*)(void)) dlsym(RTLD_NEXT, "mallinfo2");
#elif CMK_HAS_MALLINFO
  auto os_mallinfo = (struct mallinfo (*)(void)) dlsym(RTLD_NEXT, "mallinfo");
#endif

  mm_impl_malloc = os_malloc;
  mm_impl_calloc = os_calloc;
  mm_impl_realloc = os_realloc;
  mm_impl_free = os_free;

  mm_impl_memalign = os_memalign;
  mm_impl_posix_memalign = os_posix_memalign;
  mm_impl_aligned_alloc = os_aligned_alloc;

#if CMK_HAS_VALLOC
  mm_impl_valloc = os_valloc;
#endif
#if CMK_HAS_PVALLOC
  mm_impl_pvalloc = os_pvalloc;
#endif
#if CMK_HAS_CFREE
  mm_impl_cfree = os_cfree;
#endif
#if CMK_HAS_MALLINFO2
  mm_impl_mallinfo = os_mallinfo2;
#elif CMK_HAS_MALLINFO
  mm_impl_mallinfo = os_mallinfo;
#endif
}

#endif
