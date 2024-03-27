include(CheckTypeSize)
include(CheckFunctionExists)
include(CheckCSourceCompiles)
include(CheckSymbolExists)

if(NOT CHARM_OS STREQUAL "win")
  set(CMAKE_EXTRA_INCLUDE_FILES sys/types.h sys/socket.h)
  set(CMAKE_REQUIRED_LIBRARIES -lm ${CMAKE_DL_LIBS} -pthread)
else()
  set(CMAKE_REQUIRED_LIBRARIES -lm -pthread)
endif()
set(CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE)

# C types and type sizes
check_type_size("size_t" size_t_size)
if(${size_t_size} EQUAL 8)
    set(CMK_SIZET_64BIT 1)
else()
    set(CMK_SIZET_64BIT 0)
endif()

check_type_size("GROUP_AFFINITY" HAVE_GROUP_AFFINITY)
check_type_size("GROUP_RELATIONSHIP" HAVE_GROUP_RELATIONSHIP)
check_type_size("KAFFINITY" HAVE_KAFFINITY)
check_type_size("long double" CMK_LONG_DOUBLE_DEFINED)
check_type_size("long long" CMK_LONG_LONG_DEFINED)
check_type_size("pthread_t" HAVE_PTHREAD_T)
check_type_size("socklen_t" CMK_HAS_SOCKLEN)
check_type_size("ssize_t" HAVE_SSIZE_T)
check_type_size("unsigned int" SIZEOF_UNSIGNED_INT)
check_type_size("unsigned long" SIZEOF_UNSIGNED_LONG)
check_type_size("void *" SIZEOF_VOID_P)
check_type_size("__int64" CMK___int64_DEFINED)
check_type_size("__int128_t" CMK___int128_t_DEFINED)
check_type_size("__int128" CMK___int128_DEFINED)
check_type_size("__int128" CMK_HAS_INT16)

# C header files
check_include_file(alloca.h CMK_HAS_ALLOCA_H)
check_include_file(ctype.h HAVE_CTYPE_H)
check_include_file(cuda.h HAVE_CUDA_H)
check_include_file(dirent.h HAVE_DIRENT_H)
check_include_file(dlfcn.h HAVE_DLFCN_H)
check_include_file(elf.h CMK_HAS_ELF_H)
check_include_file(inttypes.h HAVE_INTTYPES_H)
check_include_file(kstat.h HAVE_KSTAT_H)
check_include_file(libudev.h HAVE_LIBUDEV_H)
check_include_file(malloc.h HAVE_MALLOC_H)
check_include_file(malloc.h CMK_HAS_MALLOC_H)
check_include_file(memory.h HAVE_MEMORY_H)
check_include_file(numaif.h HAVE_NUMAIF_H)
check_include_file(pthread_np.h HAVE_PTHREAD_NP_H)
check_include_file(stdlib.h HAVE_STDLIB_H)
check_include_file(string.h HAVE_STRING_H)
check_include_file(strings.h HAVE_STRINGS_H)
set(CMK_HAS_STRINGS_H ${HAVE_STRINGS_H})
check_include_file(unistd.h HAVE_UNISTD_H)
check_include_file(regex.h CMK_HAS_REGEX_H)
check_include_file(values.h CMK_HAS_VALUES_H)
check_include_file(stdint.h CMK_HAS_STDINT_H)
check_include_file(sys/utsname.h HAVE_SYS_UTSNAME_H)
check_include_file(sys/sysctl.h HAVE_SYS_SYSCTL_H)
check_include_file(sys/stat.h HAVE_SYS_STAT_H)
check_include_file(sys/param.h HAVE_SYS_PARAM_H)
check_include_file(sys/mman.h HAVE_SYS_MMAN_H)
check_include_file(sys/lgrp_user.h HAVE_SYS_LGRP_USER_H)
check_include_file(sys/cpuset.h HAVE_SYS_CPUSET_H)
check_include_file(valgrind/valgrind.h HAVE_VALGRIND_VALGRIND_H)
check_include_file(Multiprocessing.h CMK_HAS_MULTIPROCESSING_H) # for Apple

# C functions
check_function_exists(_putenv HAVE__PUTENV)
check_function_exists(_strdup HAVE__STRDUP)
check_function_exists(asctime CMK_HAS_ASCTIME)
check_function_exists(backtrace CMK_USE_BACKTRACE)
check_function_exists(bindprocessor CMK_HAS_BINDPROCESSOR)
check_function_exists(cfree CMK_HAS_CFREE)
check_function_exists(clz HAVE_CLZ)
check_function_exists(clzl HAVE_CLZL)
check_symbol_exists(dlopen dlfcn.h CMK_DLL_USE_DLOPEN)
set(CMK_HAS_DLOPEN ${CMK_DLL_USE_DLOPEN})
check_symbol_exists(dlmopen dlfcn.h CMK_HAS_DLMOPEN)
check_symbol_exists(fabsf "math.h" HAVE_DECL_FABSF)
set(CMK_HAS_FABSF ${HAVE_DECL_FABSF})
check_symbol_exists(fdatasync unistd.h CMK_HAS_FDATASYNC_FUNC)
check_function_exists(fsync CMK_HAS_FSYNC_FUNC)
check_symbol_exists(ffs "strings.h" HAVE_FFS)
check_function_exists(ffsl HAVE_FFSL)
check_function_exists(fls HAVE_FLS)
check_function_exists(flsl HAVE_FLSL)
check_function_exists(getexecname HAVE_DECL_GETEXECNAME)
check_function_exists(getifaddrs CMK_HAS_GETIFADDRS)
check_function_exists(getpagesize HAVE_GETPAGESIZE)
check_function_exists(getpagesize CMK_HAS_GETPAGESIZE)
check_function_exists(getpid CMK_HAS_GETPID)
check_type_size(pid_t MY_HAS_PID_T)
if(NOT MY_HAS_PID_T)
  set(CMK_HAS_GETPID 0)
endif()
check_function_exists(getprogname HAVE_DECL_GETPROGNAME)
check_symbol_exists(get_myaddress rpc/rpc.h CMK_HAS_GET_MYADDRESS)
check_function_exists(host_info HAVE_HOST_INFO)
check_function_exists(kill CMK_HAS_KILL)
check_symbol_exists(log2 math.h CMK_HAS_LOG2)
check_function_exists(mallinfo CMK_HAS_MALLINFO)
check_function_exists(mkstemp CMK_USE_MKSTEMP)
check_function_exists(mmap CMK_HAS_MMAP)
check_symbol_exists(MAP_ANON sys/mman.h CMK_HAS_MMAP_ANON)
check_symbol_exists(MAP_NORESERVE sys/mman.h CMK_HAS_MMAP_NORESERVE)
check_function_exists(mprotect CMK_HAS_MPROTECT)
check_function_exists(mstats CMK_HAS_MSTATS)
check_function_exists(ntohl CMK_HAS_NTOHL)
check_symbol_exists(offsetof stddef.h CMK_HAS_OFFSETOF)
check_function_exists(openat HAVE_OPENAT)
check_function_exists(poll CMK_USE_POLL)
check_function_exists(popen CMK_HAS_POPEN)
check_function_exists(posix_memalign HAVE_POSIX_MEMALIGN)
check_symbol_exists(pthread_getaffinity_np pthread.h HAVE_DECL_PTHREAD_GETAFFINITY_NP)
check_symbol_exists(pthread_setaffinity_np pthread.h HAVE_DECL_PTHREAD_SETAFFINITY_NP)
set(CMK_HAS_PTHREAD_SETAFFINITY ${HAVE_DECL_PTHREAD_SETAFFINITY_NP})
check_symbol_exists(pthread_spin_lock pthread.h CMK_HAS_SPINLOCK)
check_symbol_exists(pvalloc malloc.h CMK_HAS_PVALLOC)
check_symbol_exists(RTLD_DEEPBIND dlfcn.h CMK_HAS_RTLD_DEEPBIND)
check_symbol_exists(RTLD_DEFAULT dlfcn.h CMK_HAS_RTLD_DEFAULT)
check_symbol_exists(RTLD_NEXT dlfcn.h CMK_HAS_RTLD_NEXT)
check_function_exists(readlink CMK_HAS_READLINK)
check_function_exists(realpath CMK_HAS_REALPATH)
check_symbol_exists(RUSAGE_THREAD sys/resource.h CMK_HAS_RUSAGE_THREAD)
check_function_exists(sbrk CMK_HAS_SBRK)
check_function_exists(sched_setaffinity CMK_HAS_SETAFFINITY)
check_function_exists(setlocale HAVE_SETLOCALE)
check_symbol_exists(setpriority sys/resource.h CMK_HAS_SETPRIORITY)
check_function_exists(sleep CMK_HAS_SLEEP)
check_function_exists(snprintf HAVE_DECL_SNPRINTF)
check_function_exists(sqrtf CMK_HAS_SQRTF)
check_function_exists(strcasecmp HAVE_DECL_STRCASECMP)
check_function_exists(strftime HAVE_STRFTIME)
check_function_exists(strncasecmp HAVE_STRNCASECMP)
check_function_exists(strtoull HAVE_DECL_STRTOULL)
check_function_exists(strtoull HAVE_STRTOULL)
check_function_exists(sync CMK_HAS_SYNC_FUNC)
check_function_exists(system CMK_HAS_SYSTEM)
check_function_exists(sysctl HAVE_SYSCTL)
check_function_exists(sysctlbyname HAVE_SYSCTLBYNAME)
check_function_exists(uname HAVE_UNAME)
check_function_exists(usleep CMK_HAS_USLEEP)
check_symbol_exists(valloc malloc.h CMK_HAS_VALLOC)


# Complex tests

file(WRITE ${CMAKE_BINARY_DIR}/test_file "")
execute_process(COMMAND cp -p test_file test_file2 ERROR_VARIABLE CP_P_OPTION_ERROR)
if(NOT ${CP_P_OPTION_ERROR} STREQUAL "")
  set(CP "cp")
else()
  set(CP "cp -p")
endif()
file(REMOVE ${CMAKE_BINARY_DIR}/test_file ${CMAKE_BINARY_DIR}/test_file2)


check_c_source_compiles("
#include <stdio.h>
#include <lustre/lustreapi.h>
#include <lustre/lustre_user.h>

int main() {
  llapi_printf(LLAPI_MSG_NORMAL, \"Lustre FS is available\");
  return 0;
}
" CMK_HAS_LUSTREFS)


if(CMK_HAS_LUSTREFS)
  set(CMK_LUSTREAPI "-llustreapi")
else()
  set(CMK_LUSTREAPI "")
endif()


check_c_source_compiles("
int main()
{
  asm volatile(\"eieio\" ::: \"memory\");
}
" CMK_PPC_ASM)

check_c_source_compiles("
int main()
{
  __sync_synchronize();
}
" CMK_C_SYNC_SYNCHRONIZE_PRIMITIVE)

check_c_source_compiles("
int main()
{
  int t=1;
  __sync_add_and_fetch(&t, 1);
}
" CMK_C_SYNC_ADD_AND_FETCH_PRIMITIVE)

check_c_source_compiles("
#include <sys/personality.h>
int main() {
    int orig_persona = personality(0xffffffff);
    personality(orig_persona | ADDR_NO_RANDOMIZE);
    return 0;
}
" CMK_HAS_ADDR_NO_RANDOMIZE)

check_c_source_compiles("
__attribute__((visibility(\"default\"))) int myfunc();
int myfunc()
{
  return 0;
}
int main()
{
  return 0;
}
" CMK_HAS_ATTRIBUTE_VISIBILITY_DEFAULT)

check_c_source_compiles("
#define _GNU_SOURCE
#include <sys/uio.h>
#include <errno.h>
int main() {
  pid_t pid;
  struct iovec *local, *remote;
  int nread = process_vm_readv(pid, local, 1, remote, 1, 0);
  nread = process_vm_writev(pid, local, 1, remote, 1, 0);
  return errno;
}
" CMK_HAS_CMA)

check_c_source_compiles("
#include <stdio.h>
#include <papi.h>
int main() {
    if (PAPI_library_init(PAPI_VER_CURRENT) != PAPI_VER_CURRENT) return 1;
    return 0;
}
" CMK_HAS_COUNTER_PAPI)

check_c_source_compiles("
#define _GNU_SOURCE
#define __USE_GNU
#include <link.h>
#include <stddef.h>
static int callback(struct dl_phdr_info* info, size_t size, void* data)
{
  return 0;
}
int main()
{
  dl_iterate_phdr(callback, NULL);
  return 0;
}
" CMK_HAS_DL_ITERATE_PHDR)

check_c_source_compiles("
extern int __executable_start;
int main()
{
  return __executable_start;
}
" CMK_HAS_EXECUTABLE_START)

check_c_source_compiles("
#include <stdio.h>
extern int _IO_file_overflow(FILE *, int);
int main()
{
  return _IO_file_overflow(stdout, -1);
}
" CMK_HAS_IO_FILE_OVERFLOW)

check_c_source_compiles("
#include <stdlib.h>
#include <stdio.h>
#include <linux/mempolicy.h>
#include <numaif.h>
#include <numa.h>

int main()
{
  if (get_mempolicy(NULL, NULL, 0, 0, 0) == 0) return 0;
  return -1;
}
" CMK_HAS_NUMACTRL)

set(tmp ${CMAKE_REQUIRED_LIBRARIES})
set(CMAKE_REQUIRED_LIBRARIES $ENV{CRAY_PMI_POST_LINK_OPTS} -lpmi)
check_c_source_compiles("
#include <pmi.h>
int main() {
    int nid;
    PMI_Get_nid(0, &nid);

    return 0;
}
" CMK_HAS_PMI_GET_NID)
set(CMAKE_REQUIRED_LIBRARIES ${tmp})

check_c_source_compiles("
#include <rca_lib.h>
int main() {
    rca_mesh_coord_t xyz;
    rca_get_meshcoord(0, &xyz);

    return 0;
}
" CMK_HAS_RCALIB)

check_c_source_compiles("
#include <rca_lib.h>
int main() {
    rca_mesh_coord_t xyz;
    rca_get_max_dimension(&xyz);

    return 0;
}
" CMK_HAS_RCA_MAX_DIMENSION)

check_c_source_compiles("
__thread unsigned long long int x;
static __thread  int y;
int main(void)
{
  x = 1;
  y = 1;
}
" CMK_HAS_TLS_VARIABLES )

check_c_source_compiles("
#include <setjmp.h>
int main() {
  jmp_buf buf;
  _setjmp(buf);
  _longjmp(buf, 0);
}
" CMK_HAS_UNDERSCORE_SETJMP )

check_c_source_compiles("
#include <infiniband/verbs.h>
void test()
{
    struct ibv_context    *context;
    int ibPort;
    struct ibv_port_attr attr;
    if (ibv_query_port(context, ibPort, &attr) != 0) return;
    if (attr.link_layer == IBV_LINK_LAYER_INFINIBAND)  return;
}
" CMK_IBV_PORT_ATTR_HAS_LINK_LAYER)

check_c_source_compiles("
#include <stdint.h>
#include <gni_pub.h>
int main() {
    gni_bi_desc_t gni_bi_desc;
    uint32_t gni_device_id = 0;
    gni_return_t gni_rc = GNI_GetBIConfig(gni_device_id, &gni_bi_desc);
    if (gni_rc == GNI_RC_SUCCESS) {
    }
    return 0;
}
" CMK_BALANCED_INJECTION_API)

if(${NETWORK} STREQUAL "ofi" OR ${NETWORK} STREQUAL "ofi-crayshasta" OR ${NETWORK} STREQUAL "ofi-linux")
# assume HPC installation
include(CMakePrintHelpers)
  find_package(EnvModules REQUIRED)
  find_package(PkgConfig REQUIRED)
  if(EnvModules_FOUND)
  #at least get libfabric loaded if it isn't already
	env_module(load libfabric)
  endif()
  set(tmp ${CMAKE_REQUIRED_LIBRARIES})
  if(${PkgConfig_FOUND})
# this is tortured because pkg-config and cmake are infuriating
	set(myconfigCommand "pkg-config")
	set(myargs1 "libfabric")
	set(myargs2 "--libs")
	execute_process(COMMAND ${myconfigCommand} ${myargs1} ${myargs2}
	                OUTPUT_VARIABLE PKG_CONFIG_OFI_LIBS_OUTPUT
			RESULT_VARIABLE PKG_CONFIG_OFI_LIBS_RESULT
			WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
			ERROR_VARIABLE thiserror
			)
	string(STRIP ${PKG_CONFIG_OFI_LIBS_OUTPUT} CMAKE_PKG_CONFIG_OFI_LIBS)
	set(myargs2 "--cflags")
	execute_process(COMMAND ${myconfigCommand} ${myargs1} ${myargs2}
			OUTPUT_VARIABLE PKG_CONFIG_OFI_CFLAGS_OUTPUT
			RESULT_VARIABLE PKG_CONFIG_OFI_CFLAGS_RESULT
			WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
			ERROR_VARIABLE $thaterror
			)
	string(STRIP ${PKG_CONFIG_OFI_CFLAGS_OUTPUT} CMAKE_PKG_CONFIG_OFI_CFLAGS)
	set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} ${CMAKE_PKG_CONFIG_OFI_CFLAGS}")
	set(CMAKE_REQUIRED_LIBRARIES "${CMAKE_REQUIRED_LIBRARIES} ${CMAKE_PKG_CONFIG_OFI_LIBS}")
  else()
	message(WARNING "cmake can't find pkg-config")
	set(CMAKE_REQUIRED_LIBRARIES "${CMAKE_REQUIRED_LIBRARIES}")
  endif()

  check_c_source_compiles("
    #include <rdma/fabric.h>
    int main(int argc, char **argv)
    {
      struct fi_info *providers;
      int ret = fi_getinfo(FI_VERSION(1,0), NULL, NULL, 0ULL, NULL, &providers);
      return 0;
    }
  " CMK_BUILD_ON_OFI)

  if("${CMK_BUILD_ON_OFI}" STREQUAL "")
    message(FATAL_ERROR "Unable to build ofi with FLAGS ${CMAKE_REQUIRED_FLAGS} LIBS ${CMAKE_REQUIRED_LIBRARIES}  for network ${NETWORK}.")
    set(CMAKE_REQUIRED_LIBRARIES ${tmp})
  else()
#    set(CMAKE_EXTRA_INCLUDE_FILES "{CMAKE_EXTRA_INCLUDE_FILES} CMAKE_PKG_CONFIG_OFI_CFLAGS")
#    set(CMK_LIBDIR "{CMK_LIBS} CMAKE_PKG_CONFIG_OFI_LIBS")
  endif()
endif()

check_c_source_compiles("
#include <ucontext.h>
struct _libc_fpstate   fpstate;
fpregset_t *fp;
int main()
{
  ucontext_t context;
  context.uc_mcontext.fpregs = 0;
}
" CMK_CONTEXT_FPU_POINTER)

check_c_source_compiles("
#include <ucontext.h>
int main()
{
  ucontext_t context;
  context.uc_mcontext.uc_regs = 0;
}
" CMK_CONTEXT_FPU_POINTER_UCREGS)

check_c_source_compiles("
#include <ucontext.h>
vrregset_t *v_regs;
ucontext_t  uc;

int main()
{
  vrregset_t *ptr = uc.uc_mcontext.v_regs;
}
" CMK_CONTEXT_V_REGS)

check_c_source_compiles("
inline static int foo()
{
  return 1;
}
int main() {}
" CMK_C_INLINE)

check_c_source_compiles("
int main(void)
{
  unsigned long long int v=0;
  int *lo=0+(int *)&v;
  int *hi=1+(int *)&v;
  __asm__ __volatile__(
      \"rdtsc; movl %%edx,%0; movl %%eax,%1\"
      : /* output  */ \"=m\" (*hi), \"=m\" (*lo)
      : /* input */
      : /* trashes */ \"%edx\", \"%eax\"
  );
  return v;
}
" CMK_GCC_X86_ASM)

check_c_source_compiles("
int main(void)
{
  int x;
  asm(\"lock incl %0\" :: \"m\" (x));
  asm(\"lock decl %0\" :: \"m\" (x));
  return x;
}
" CMK_GCC_X86_ASM_ATOMICINCREMENT)
