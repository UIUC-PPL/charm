
#define CMK_SMP						   1


#undef CMK_SHARED_VARS_UNAVAILABLE
#undef CMK_SHARED_VARS_POSIX_THREADS_SMP
#define CMK_SHARED_VARS_UNAVAILABLE                        0
#define CMK_SHARED_VARS_POSIX_THREADS_SMP                  1

#undef CMK_MALLOC_USE_GNU_MALLOC
#undef CMK_MALLOC_USE_OS_BUILTIN
#define CMK_MALLOC_USE_GNU_MALLOC                          0
#define CMK_MALLOC_USE_OS_BUILTIN                          1

/*#define CMK_MMAP_PROBE                                     1 */

/*#define CMK_PCQUEUE_LOCK				   1 */
/*#define CMK_USE_MFENCE 					   1 */
/* Replaced by CMK_NOT_USE_TLS_THREAD as default */
/*#define CMK_USE_TLS_THREAD                                 1*/
