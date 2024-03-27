include(CheckCXXSymbolExists)
include(CheckCXXSourceCompiles)
include(CheckCXXCompilerFlag)

# Workaround for CMake bug when calling check_include_file_cxx() with C++11:
# https://stackoverflow.com/uestions/47213356/cmake-using-corrext-c-standard-when-checking-for-header-files
set(CMAKE_REQUIRED_FLAGS "-std=c++11")


# C++ type sizes
check_type_size("std::void_t" CMK_HAS_STD_VOID_T LANGUAGE CXX)

# C++ header files
check_include_file_cxx(atomic CMK_HAS_CXX11_ATOMIC)
check_include_file_cxx(cstdatomic CMK_HAS_CXX0X_CSTDATOMIC)
check_include_file_cxx(regex CMK_HAS_REGEX)

# C++ compiler flags
# Keep in sync with UNKNOWN_FLAGS section in src/arch/win/unix2nt_cc

if(CHARM_CPU STREQUAL "i386" OR CHARM_CPU STREQUAL "x86_64")
  check_cxx_compiler_flag("-mno-tls-direct-seg-refs" CMK_COMPILER_KNOWS_TLSDIRECTSEGREFS)
elseif()
  set(CMK_COMPILER_KNOWS_TLSDIRECTSEGREFS 0)
endif()

check_cxx_compiler_flag("-fvisibility=hidden" CMK_COMPILER_KNOWS_FVISIBILITY)

# Needed to avoid migratable threads failing the stack check
# See https://github.com/UIUC-PPL/charm/pull/3174 for details.
check_cxx_compiler_flag("-fno-stack-protector" CMK_COMPILER_KNOWS_FNOSTACKPROTECTOR)
if(${CMK_COMPILER_KNOWS_FNOSTACKPROTECTOR})
  set(OPTS_CC "${OPTS_CC} -fno-stack-protector")
  set(OPTS_CXX "${OPTS_CXX} -fno-stack-protector")
endif()

# Workaround for bug #1045 appearing in GCC >6.x
check_cxx_compiler_flag("-fno-lifetime-dse" CMK_COMPILER_KNOWS_LIFETIMEDSE)
if(${CMK_COMPILER_KNOWS_LIFETIMEDSE})
  set(OPTS_CXX "${OPTS_CXX} -fno-lifetime-dse")
endif()

# Needed so that tlsglobals works correctly with --build-shared
# See https://github.com/UIUC-PPL/charm/issues/3168 for details.
check_cxx_compiler_flag("-ftls-model=initial-exec" CMK_COMPILER_KNOWS_FTLS_MODEL)
if(CMK_COMPILER_KNOWS_FTLS_MODEL AND NOT DISABLE_TLS)
  set(OPTS_CC "${OPTS_CC} -ftls-model=initial-exec")
  set(OPTS_CXX "${OPTS_CXX} -ftls-model=initial-exec")
  set(OPTS_LD "${OPTS_LD} -ftls-model=initial-exec")
endif()

# Allow seeing own symbols dynamically, needed for programmatic backtraces
check_cxx_compiler_flag("-rdynamic" CMK_COMPILER_KNOWS_RDYNAMIC)
check_cxx_compiler_flag("-Wl,--export-dynamic" CMK_LINKER_KNOWS_EXPORT_DYNAMIC)
if(${CMK_COMPILER_KNOWS_RDYNAMIC})
  set(OPTS_LD "${OPTS_LD} -rdynamic")
elseif(${CMK_LINKER_KNOWS_EXPORT_DYNAMIC})
  set(OPTS_LD "${OPTS_LD} -Wl,--export-dynamic")
endif()

check_cxx_compiler_flag("-Wl,-undefined,dynamic_lookup" CMK_LINKER_KNOWS_UNDEFINED)

# C++ complex tests
check_cxx_source_compiles("
#include <type_traits>
struct s { s(int a) { } };
int main()
{
  return std::is_constructible<s, int>::value;
}" CMK_HAS_IS_CONSTRUCTIBLE)

check_cxx_source_compiles("
#include <vector>
#include <iterator>
int main()
{
  std::vector<int> tree;
  return std::distance(tree.begin(), tree.end());
}
" CMK_HAS_STD_DISTANCE)

check_cxx_source_compiles("
#include <stdlib.h>
class er {
 protected:
   void operator()(char &v,const char *desc=NULL) {};
   void operator()(signed char &v,const char *desc=NULL) {};
};
int main() {}
" CMK_SIGNEDCHAR_DIFF_CHAR)

check_cxx_source_compiles("
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <spawn.h>
int main() {
    return posix_spawn(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
}
" CMK_HAS_POSIX_SPAWN)

check_cxx_source_compiles("
#include <type_traits>
int main()
{
  return std::alignment_of<int>::value;
}
" CMK_HAS_ALIGNMENT_OF)

check_cxx_source_compiles("
#include <iterator>
template <typename T> // T models Input Iterator
typename std::iterator_traits<T>::value_type accumulate(T first, T last)
{
      typename std::iterator_traits<T>::value_type result = 0;
      while(first != last)
            result += *first++;
      return result;
}
int main() {}
" CMK_HAS_ITERATOR_TRAITS)

check_cxx_source_compiles("
#include <list>
#include <iterator>
int main()
{
  using namespace std;
  list<int> L;
  inserter ( L, L.end ( ) ) = 500;
}
" CMK_HAS_STD_INSERTER )

check_cxx_source_compiles("
#include <typeinfo>
int main() {
  int x;
  typeid(x).name();
}
" CMK_HAS_TYPEINFO )

check_cxx_source_compiles("
class foo {
public:
  void operator delete(void*p){};
  void operator delete(void*p,int*){};
};
int main() {}
" CMK_MULTIPLE_DELETE)

file(READ ${CMAKE_SOURCE_DIR}/src/util/ckdll_system.C ckdll_system)
check_cxx_source_compiles("${ckdll_system}\n int main(){}" CMK_SIGSAFE_SYSTEM)

file(READ ${CMAKE_SOURCE_DIR}/src/util/ckdll_win32.C ckdll_win32)
check_cxx_source_compiles("${ckdll_win32}\n int main(){}" CMK_DLL_USE_WIN32)

check_cxx_source_compiles("
#include <cstddef>
int main() {
  extern void *(*__morecore)(ptrdiff_t);
  __morecore(0);
  return 0;
}
" CMK_EXPECTS_MORECORE)

# Unset workaround from above
set(CMAKE_REQUIRED_FLAGS "")
if(${NETWORK} STREQUAL "ofi" OR ${NETWORK} STREQUAL "ofi-crayshasta" OR ${NETWORK} STREQUAL "ofi-linux")
# assume HPC installation with LMOD
  include(CMakePrintHelpers)
  find_package(EnvModules REQUIRED)
  find_package(PkgConfig REQUIRED)
  if(EnvModules_FOUND)
  # we need libfabric cray-libpals and cray-pmi
	env_module(load libfabric)
	env_module(load cray-libpals)
	env_module(load cray-pmi)
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
    set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} ${CMAKE_PKG_CONFIG_OFI_CFLAGS}")
    set(CMAKE_REQUIRED_LIBRARIES "${CMAKE_REQUIRED_LIBRARIES} ${CMAKE_PKG_CONFIG_OFI_LIBS}")

check_cxx_source_compiles("
    #include <rdma/fabric.h>
    #include <rdma/fi_cxi_ext.h>
    int main(int argc, char **argv)
    {
      struct fi_info *providers;
      int ret = fi_getinfo(FI_VERSION(1,0), NULL, NULL, 0ULL, NULL, &providers);
      return 0;
    }
  " CMK_BUILD_ON_CXI)
endif()
