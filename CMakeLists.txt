# CMake-based build system for Charm++ and AMPI
#
# When updating any of the following files:
# - any Makefile
# - adding, deleting, renaming any source file
# - any configure script
#
# , please also update:
# - this file
# - the buildcmake script
# - files in cmake/

cmake_minimum_required(VERSION 3.12)

# Print warning
message("##################################################################")
message("# The CMake build system may not support advanced build options. #")
message("# Please let us know if you experience any problems by opening   #")
message("# an issue on our GitHub (https://github.com/charmplusplus/charm)#")
message("# with the below configuration information.                      #")
message("##################################################################")

cmake_policy(SET CMP0075 NEW)

if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.17)
  cmake_policy(SET CMP0102 NEW)
endif()

# Fortran is optional, so don't include it in LANGUAGES here
project(Charm++ LANGUAGES CXX C ASM VERSION 9.0.0)

find_package(Threads)
find_package(OpenMP) # Do this before Fortran, in case we don't have a Fortran compiler

set(CMK_BUILD_MPI 0)
set(CMK_BUILD_ON_MPI 0)
set(CMK_CXX_MPI_BINDINGS 1) # Always true, this just checked for a conflict with old mpich versions.
if(${NETWORK} STREQUAL "mpi")
  set(CMK_BUILD_MPI 1)
  set(CMK_BUILD_ON_MPI 1)
  find_package(MPI REQUIRED)
endif()

# We need C++11 for (almost) all targets
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  set(CMK_COMPILER gcc)
  get_filename_component(mytmp ${CMAKE_CXX_COMPILER} NAME)
  if(mytmp MATCHES "-")
    string(REGEX REPLACE ".*(-.*)" "\\1" CMK_COMPILER_SUFFIX ${mytmp})
  endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  if(CMAKE_CXX_COMPILER MATCHES "(icpx)$")
    set(CMK_COMPILER icx)
  else()
    set(CMK_COMPILER clang)
  endif()
  get_filename_component(mytmp ${CMAKE_CXX_COMPILER} NAME)
  if(mytmp MATCHES "-")
    string(REGEX REPLACE ".*(-.*)" "\\1" CMK_COMPILER_SUFFIX ${mytmp})
  endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "IntelLLVM")
  set(CMK_COMPILER icx)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Intel")
  set(CMK_COMPILER icc)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Cray")
  set(CMK_COMPILER craycc)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "PGI")
  set(CMK_COMPILER pgcc)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "NVHPC")
  set(CMK_COMPILER nvhpc)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "XL")
  # FIXME: No support for 32-bit XLC
  set(CMK_COMPILER xlc64)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" OR (CMAKE_CXX_COMPILER_ID STREQUAL "" AND (CMAKE_SYSTEM_NAME STREQUAL "Windows" OR CMAKE_SYSTEM_NAME STREQUAL "CYGWIN")))
  set(CMK_COMPILER msvc)
else()
  message(FATAL_ERROR "Unknown compiler ${CMAKE_CXX_COMPILER_ID}")
endif()

option(ENABLE_FORTRAN "Enable Fortran" ON)

# No Fortran support with MSVC
if(NOT CMK_COMPILER STREQUAL "msvc" AND ENABLE_FORTRAN)
  enable_language(Fortran OPTIONAL)
endif()

if(ENABLE_FORTRAN AND CMAKE_Fortran_COMPILER)
  include(FortranCInterface)
  FortranCInterface_VERIFY()
  FortranCInterface_VERIFY(CXX)
  if(CMAKE_Fortran_COMPILER_ID STREQUAL "Cray")
    # Needed to make lowercase modules on Cray platforms
    set(CMAKE_Fortran_FLAGS "${CMAKE_Fortran_FLAGS} -ef")
  endif()
else()
  message(STATUS "Charm++: No Fortran support.")
endif()

if(${FortranCInterface_VERIFIED_CXX})
  set(CMK_CAN_LINK_FORTRAN 1)
else()
  set(CMK_CAN_LINK_FORTRAN 0)
endif()

# Build options
set(NETWORK "netlrts" CACHE STRING "Target network layer, can be one of mpi,multicore,netlrts,reconverse,gni,ofi,pami,pamilrts,ucx,verbs")
set(TARGET "charm++" CACHE STRING "Target build type, can be one of charm++,AMPI,LIBS,ChaNGa,everylb")
set(ARCH "" CACHE STRING "Target architecture, can be one of i386,x86_64,arm7,arm8,ppc64le") # By default, detect arch we are running on.

function(charm_determine_arch output input)
  if(input STREQUAL "x86_64" OR input STREQUAL "AMD64")
    set(retval "x86_64")
  elseif(input STREQUAL "i386" OR input STREQUAL "i686")
    set(retval "i386")
  elseif(input STREQUAL "powerpc" OR input MATCHES "^ppc")
    set(retval "ppc64le")
  elseif(input MATCHES "armv6" OR input MATCHES "armv7")
    set(retval "arm7")
  elseif(input MATCHES "aarch64" OR input MATCHES "arm64")
    set(retval "arm8")
  else()
    set(retval ${input})
  endif()

  set(${output} ${retval} PARENT_SCOPE)
endfunction()

if(NOT ARCH)
  charm_determine_arch(ARCH ${CMAKE_SYSTEM_PROCESSOR})
endif()
set(CHARM_CPU ${ARCH})

charm_determine_arch(CHARM_HOST_CPU ${CMAKE_HOST_SYSTEM_PROCESSOR})

if(NOT CHARM_CPU STREQUAL CHARM_HOST_CPU)
  if (CHARM_CPU STREQUAL "i386" AND CHARM_HOST_CPU STREQUAL "x86_64")
    message(STATUS "Attempting to cross-compile from ${CHARM_HOST_CPU} to ${CHARM_CPU}.")
    set(CMAKE_C_FLAGS -m32)
    set(CMAKE_CXX_FLAGS -m32)
    set(CMAKE_Fortran_FLAGS -m32)
  else()
    message(WARNING "Cross-compiling from ${CHARM_HOST_CPU} to ${CHARM_CPU} is not supported currently.")
  endif()
endif()

# Platform specific options (choose multiple if desired):
option(SMP "Enable shared-memory communication within a single multi-processor machine instead of message passing." ON)
option(OMP "Enable OpenMP support in Charm++" OFF)
option(TCP "use TCP sockets for communication (only for netlrts)" OFF)
option(PTHREADS "compile with pthreads Converse threads" OFF)

# Slingshot-11 specific, off by default, auto enabled if support is found
option(CXI "Enable CXI (Cassini/Slingshot-11) extensions to OFI" OFF)

# Advanced options:
option(OOC "Compile with out of core support" OFF)
option(NOLB "Build without the load balancing framework (conv-mach-nolb)" OFF)
option(TSAN "Build with ThreadSanitizer (conv-mach-tsan)" OFF)
option(PERFTOOLS "Build for Cray perftools (conv-mach-perftools)" OFF)
option(CRAYPE "Cray programming environment flags (conv-mach-craype)" OFF)
option(SYNCFT "Compile with Charm++ fault tolerance support" OFF)
option(PAPI "Compile with PAPI performance counter support" OFF)

# Enable/disable features:
set(ERROR_CHECKING "" CACHE STRING "Enable error checking")
option(AMPI_ERROR_CHECKING "Enable AMPI error checking" OFF)
option(STATS               "Enable statistics collection" OFF)
set(TRACING "" CACHE STRING "Enable tracing modules")
option(TRACING_COMMTHREAD  "Enable tracing communication thread" OFF)
option(CHARMDEBUG          "Enable charmDebug" OFF)
option(REPLAY              "Enable record/replay" OFF)
option(CCS                 "Enable CCS" OFF)
option(CONTROLPOINT        "Enable control point" OFF)
option(PERSISTENT          "Enable Persistent Communication API" OFF)
option(LBUSERDATA          "Enable LB user data" OFF)
option(LOCKLESS_QUEUE      "Enable lockless queue for PE local and node queue" OFF)
option(SHRINKEXPAND        "Enable malleable jobs / shrink expand" OFF)
option(NUMA                "Support memory affinity with NUMA" OFF)
set(LBTIME_TYPE "double" CACHE STRING "Load balancing timer type")
option(QLOGIC              "QLogic based Infiniband" OFF)
set(REFNUM_TYPE "unsigned short" CACHE STRING  "Size of the envelope refnum field")
set(PRIO_TYPE "bitvec"    CACHE STRING "Size of expected message priorities")
option(RANDOMIZED_MSGQ     "Enable a randomized msg queue (for debugging etc)" OFF)
option(ZLIB                "Enable zlib support" ON)
option(AMPI_MPICH_TESTS    "Enable mpich tests for AMPI" OFF)
option(DRONE_MODE          "Enable drone mode" OFF)
option(TASK_QUEUE          "Enable task queue" OFF)
option(RECONVERSE          "Use the Reconverse communication layer" OFF)
option(CMK_COVERAGE        "Instrument for clang source-based coverage (see coverage/README.md)" OFF)

if(TRACING STREQUAL "")
  if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(TRACING 1)
  else()
    set(TRACING 0)
  endif()
endif()

if(ERROR_CHECKING STREQUAL "")
  if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(ERROR_CHECKING 1)
  else()
    set(ERROR_CHECKING 0)
  endif()
endif()

# Charm++ dynamic libraries
option(BUILD_SHARED        "Build Charm++ dynamic libraries" OFF)

# Other options
option(BUILD_CUDA          "Build with CUDA support" OFF)
option(BUILD_HIP           "Build with HIP support" OFF)
option(PXSHM               "Build with PXSHM" OFF)

# LRTS PMI options
set(LRTS_PMI "" CACHE STRING "PMI type for UCX and OFI layers, can be one of simplepmi, slurmpmi, slurmpmi2, slurmpmi2cray, ompipmix, or openpmix")

set(EXTRA_OPTS "" CACHE STRING "Extra flags to pass to compilers.")

separate_arguments(MY_EXTRA_OPTS UNIX_COMMAND "${EXTRA_OPTS}")
add_compile_options("${MY_EXTRA_OPTS}")
string(REPLACE ";" " " MY_EXTRA_OPTS "${MY_EXTRA_OPTS}")
set(OPTS "${OPTS} ${MY_EXTRA_OPTS}")
set(OPTSATBUILDTIME "${OPTSATBUILDTIME} ${MY_EXTRA_OPTS}")


# Also build shared Charm++ libraries in lib_so/
if(BUILD_SHARED)
  add_compile_options("-build-shared")
  set(OPTS "${OPTS} -build-shared")
  set(OPTSATBUILDTIME "${OPTSATBUILDTIME} -build-shared")
  # Shared libraries are created by pretending to build a
  # static library and instructing charmc to also create
  # the shared library via the -build-shared option.
  set(BUILD_SHARED_OPTION "-build-shared")
endif()

set(CMK_REPLAYSYSTEM ${REPLAY})

string(TOLOWER "${TARGET}" target_lower)

if(${target_lower} STREQUAL "changa")
  set(TARGET "LIBS")
  set(LBUSERDATA 1)
  set(BUILD_CHANGA 1)
else()
  set(BUILD_CHANGA 0)
endif()

# The GPU-aware load balancers store each object's device allocation size in LB
# user data (registered by CentralLB::initLB), so GPU builds imply LBUSERDATA.
if(BUILD_CUDA OR BUILD_HIP)
  set(LBUSERDATA 1)
endif()

if(${TARGET} STREQUAL "everylb")
  set(TARGET "LIBS")
endif()

if(${REPLAY} AND NOT ${TRACING})
  set(CMK_REPLAYSYSTEM 0)
  message(WARNING "Charm record/replay is disabled because tracing is disabled")
endif()

if(${DRONE_MODE})
  set(CMK_DRONE_MODE 1)
else()
  set(CMK_DRONE_MODE 0)
endif()

if(${TASK_QUEUE} OR ${OMP})
  set(CMK_TASKQUEUE 1)
else()
  set(CMK_TASKQUEUE 0)
endif()

# Keyed on NETWORK, not the RECONVERSE option: -DRECONVERSE=ON with a classic
# network would otherwise get through and fail later on a missing src/arch dir.
if(NOT NETWORK STREQUAL "reconverse")
  message(FATAL_ERROR "This tree builds only on reconverse (classic Converse was removed in charm #4061); use the classic branch for netlrts/mpi/ucx/verbs/ofi/gni/pami/multicore builds.")
endif()
set(CMK_RECONVERSE 1)

if(RANDOMIZED_MSGQ)
  message(FATAL_ERROR "--enable-randomized-msgq is a classic Converse build option and does nothing on reconverse. Reconverse randomizes at run time instead: run the program with +randomized_msgq (and +randomized_seed <N> to repeat the draws).")
endif()

if(${AMPI_MPICH_TESTS})
  add_definitions(-DAMPI_ERRHANDLER_RETURN=1)
  set(BUILD_MPICH_TESTS true)
else()
  add_definitions(-DAMPI_ERRHANDLER_RETURN=0)
  set(BUILD_MPICH_TESTS false)
endif()

if(${OMP})
    find_package(OpenMP REQUIRED)
endif()

if(SMP)
  set(CMK_SMP 1)
else()
  set(CMK_SMP 0)
endif()

if(${NETWORK} STREQUAL "multicore" OR ${NETWORK} STREQUAL "reconverse")
  set(CMK_SMP 1)
endif()
set(CMK_MULTICORE 0)

if(NOT ${CMK_COMPILER} STREQUAL "msvc")
  set(CMK_USE_ZLIB ${ZLIB})
else()
  set(CMK_USE_ZLIB 0)
endif()

if(${CMK_USE_ZLIB})
  find_package(ZLIB REQUIRED)
  set(CMK_SYSLIBS "${CMK_SYSLIBS} -lz")
endif()

if(${CMK_USE_SHMEM})
  if(${CMK_HAS_XPMEM})
    set(CMK_SYSLIBS "${CMK_SYSLIBS} -lxpmem")
  elseif(NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(CMK_SYSLIBS "${CMK_SYSLIBS} -lrt")
  endif()
endif()

# $CMK_SYSLIBS also gets updated in the charmc shell config files, therefore we need to include
# a reference to $CMK_SYSLIBS here.
set(CMK_SYSLIBS "$CMK_SYSLIBS ${CMK_SYSLIBS}")

set(CMK_USE_LIBJPEG 0)
find_package(JPEG)
IF(JPEG_FOUND)
  set(CMK_LIBJPEG "-ljpeg")
  set(CMK_USE_LIBJPEG 1)
ENDIF()

set(CMK_USE_LRTS 0)

# Check for Python
# PythonCCS requires Python v2.x and is the only target that uses this
set(CMK_HAS_PYTHON 0)
set(CMK_PYTHON_VERSION "")
set(CMK_BUILD_PYTHON "")

find_package(Python2 COMPONENTS Interpreter)
if(Python2_FOUND)
  set(CMK_PYTHON_VERSION_MAJOR ${Python2_VERSION_MAJOR})
  set(CMK_PYTHON_VERSION_MINOR ${Python2_VERSION_MINOR})
endif()

if(CMK_PYTHON_VERSION_MAJOR)
  set(__MY_PYTHON_VER "${CMK_PYTHON_VERSION_MAJOR}.${CMK_PYTHON_VERSION_MINOR}")
  check_include_file(python${__MY_PYTHON_VER}/Python.h __MY_HAS_PYTHONH)
  if(__MY_HAS_PYTHONH)
    set(CMK_HAS_PYTHON 1)
    set(CMK_PYTHON_VERSION ${__MY_PYTHON_VER})
    set(CMK_BUILD_PYTHON ${__MY_PYTHON_VER})
  endif()
endif()

set(CMK_REFNUM_TYPE ${REFNUM_TYPE})
set(CMK_LBTIME_TYPE ${LBTIME_TYPE})

set(CMK_LB_USER_DATA ${LBUSERDATA})

set(CMK_RANDOMIZED_MSGQ ${RANDOMIZED_MSGQ})
set(CMK_SHRINK_EXPAND ${SHRINKEXPAND})
set(CMK_SMP_TRACE_COMMTHREAD ${TRACING_COMMTHREAD})

set(CMK_TRACE_ENABLED ${TRACING})

set(CMK_WITH_CONTROLPOINT ${CONTROLPOINT})
set(CMK_WITH_STATS ${STATS})

set(CMK_CHARMDEBUG ${CHARMDEBUG})


set(CMK_MSG_PRIO_TYPE ${PRIO_TYPE})
if(${CMK_MSG_PRIO_TYPE} STREQUAL "bitvec")
  set(CMK_USE_STL_MSGQ 0)
else()
  set(CMK_USE_STL_MSGQ 1)
endif()

set(CMK_OS_IS_LINUX 0)
set(CMK_MACOSX 0)
set(CMK_WINDOWS 0)
set(CMK_POST_EXE "")
set(CMK_SHARED_SUF "so")
set(CMK_USER_SUFFIX ".user")
set(CMK_SUPPORTS_MEMORY_ISOMALLOC 1)
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  set(CHARM_OS "darwin")
  set(CMK_MACOSX 1)
  set(CMK_SHARED_SUF "dylib")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows" OR CMAKE_SYSTEM_NAME STREQUAL "CYGWIN")
  set(CHARM_OS "win")
  set(CMK_WINDOWS 1)
  set(CMK_POST_EXE ".exe")
  set(CMK_SHARED_SUF "dll")
  set(CMK_SUPPORTS_MEMORY_ISOMALLOC 0)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(CHARM_OS "linux")
  set(CMK_OS_IS_LINUX 1)
else()
  message(FATAL_ERROR "Unsupported operating system ${CMAKE_SYSTEM_NAME}.")
endif()

# Adjust version number so it can be tested by numerical comparisons, e.g. 6.10.1 -> 61001:
# #if CHARM_VERSION > 61001
if(${PROJECT_VERSION_PATCH} LESS 10)
  set(CHARM_VERSION_PATCH_PREFIX 0)
endif()
if(${PROJECT_VERSION_MINOR} LESS 10)
  set(CHARM_VERSION_MINOR_PREFIX 0)
endif()
set(CHARM_VERSION "${PROJECT_VERSION_MAJOR}${CHARM_VERSION_MINOR_PREFIX}${PROJECT_VERSION_MINOR}${CHARM_VERSION_PATCH_PREFIX}${PROJECT_VERSION_PATCH}")

unset(CHARM_VERSION_PATCH_PREFIX)
unset(CHARM_VERSION_MINOR_PREFIX)

set(CHARM_PLATFORM "${NETWORK}-${CHARM_OS}-${CHARM_CPU}")


set(CMK_BUILD_CRAY 0)
if(${NETWORK} MATCHES "gni-" OR ${NETWORK} MATCHES "ofi-cray")
  set(CHARM_PLATFORM "${NETWORK}")
  set(CMK_BUILD_CRAY 1)
elseif(${NETWORK} MATCHES "mpi-cray")
  set(CHARM_PLATFORM "${NETWORK}")
endif()

set(CMK_BUILD_ON_UCX 0)
if(${NETWORK} STREQUAL "ucx")
  set(CMK_BUILD_ON_UCX 1)
endif()

set(CMK_BUILD_OFI 0)
if(${NETWORK} MATCHES "ofi")
  set(CMK_BUILD_OFI 1)
endif()

set(CMK_ERROR_CHECKING ${ERROR_CHECKING})
set(CMK_LOCKLESS_QUEUE 0)

if(NOT ${CMK_ERROR_CHECKING})
  set(OPTS_CC "${OPTS_CC} -U_FORTIFY_SOURCE")
  set(OPTS_CXX "${OPTS_CXX} -U_FORTIFY_SOURCE")
endif()

file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/lib/)
if(${BUILD_SHARED})
  file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/lib_so/)
  file(WRITE          ${CMAKE_BINARY_DIR}/lib_so/.charmso "")
endif()
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/bin/)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/include/cklibs)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
if(BUILD_SHARED)
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib_so)
else()
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
endif()
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

if (NOT CMAKE_INSTALL_PREFIX)
    set(CMAKE_INSTALL_PREFIX ./install)
endif()

if (NOT CMAKE_BUILD_TYPE)
    message(STATUS "${PROJECT_NAME}: No build type selected, default to Release")
    set(CMAKE_BUILD_TYPE "Release")
endif()

# Reconverse has to be built with the same GPU support as Charm++, so that
# its GPU-aware loopback copies and LCI's GPUDirect registration are enabled.
# These must be set before the subdirectory is added.
if(BUILD_CUDA)
  set(RECONVERSE_ENABLE_CUDA ON CACHE BOOL "Enable CUDA support in reconverse" FORCE)
endif()
if(BUILD_HIP)
  set(RECONVERSE_ENABLE_HIP ON CACHE BOOL "Enable HIP support in reconverse" FORCE)
endif()
# Coverage flags go into CMAKE_<LANG>_FLAGS, the only flags that reach
# reconverse (cmake/reconverse clears the directory compile options), so they
# must be set before the subdirectory is added. charmc adds the same flags for
# programs built against this build (CMK_COVERAGE in conv-mach-opt.sh).
if(CMK_COVERAGE)
  if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang" OR NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR
        "--coverage uses clang source-based coverage and needs a clang-family "
        "compiler; this build has C=${CMAKE_C_COMPILER_ID}, "
        "C++=${CMAKE_CXX_COMPILER_ID}. Set CC=clang CXX=clang++ or pass 'clang'.")
  endif()
  set(_cov_compile "-fprofile-instr-generate -fcoverage-mapping")
  string(APPEND CMAKE_C_FLAGS " ${_cov_compile}")
  string(APPEND CMAKE_CXX_FLAGS " ${_cov_compile}")
  string(APPEND CMAKE_SHARED_LINKER_FLAGS " -fprofile-instr-generate")
  # Executables are linked by charmc, which adds the link flag itself except
  # for host tools (charmxi is linked -host, with a compiler that may not be
  # clang), so it is not put in CMAKE_EXE_LINKER_FLAGS.
endif()

# This needs to be include before we set charmc as the linker
# but after CMAKE_LIBRARY_OUTPUT_DIRECTORY has been set
add_subdirectory(cmake/reconverse)

if (NOT CMK_MEMPOOL_CUTOFFNUM)
  set(CMK_MEMPOOL_CUTOFFNUM 28)
endif()

set(CMK_OPTIMIZE 0)
if(CMAKE_BUILD_TYPE MATCHES "Release" OR CMAKE_BUILD_TYPE MATCHES "RelWithDebInfo")
    set(CMK_OPTIMIZE 1)
    add_compile_options(-optimize -production)
    set(BUILDOPTS "${BUILDOPTS} -optimize -production")
    set(OPTS "${OPTS} -optimize -production")
    set(OPTSATBUILDTIME "${OPTSATBUILDTIME} -optimize -production")
endif()

set(CMK_CHARM4PY 0)
if(${TARGET} STREQUAL "charm4py")
  set(CMK_CHARM4PY 1)
  # Create libcharm.so for Charm4py
  add_compile_options(-build-shared)
  set(BUILDOPTS "${BUILDOPTS} -build-shared")
  set(OPTS "${OPTS} -build-shared")
  set(OPTSATBUILDTIME "${OPTSATBUILDTIME} -build-shared")
endif()

if(CMAKE_BUILD_TYPE MATCHES "Debug" OR CMAKE_BUILD_TYPE MATCHES "RelWithDebInfo")
  add_compile_options(-g)
  set(BUILDOPTS "${BUILDOPTS} -g")
  set(OPTS "${OPTS} -g")
  set(OPTSATBUILDTIME "${OPTSATBUILDTIME} -g")
endif()

# Identify arch/network-specific paths
if(EXISTS ${CMAKE_SOURCE_DIR}/src/arch/${NETWORK}/gdir_link)
  file(STRINGS src/arch/${NETWORK}/gdir_link GDIR)
else()
  set(GDIR ${NETWORK})
endif()

set(VDIR ${CHARM_PLATFORM})

include(cmake/detect-features.cmake)
include(cmake/ci-files.cmake)
add_custom_target(ci-generated DEPENDS ${all-ci-outputs})


configure_file(${CMAKE_SOURCE_DIR}/src/scripts/charmc ${CMAKE_BINARY_DIR}/bin @ONLY)

# Use charmc to compile/link everything
set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE ${CMAKE_BINARY_DIR}/bin/charmc)

set(CMAKE_CXX_LINK_EXECUTABLE
      "${CMAKE_BINARY_DIR}/bin/charmc <CMAKE_CXX_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES> ${MY_EXTRA_OPTS}")

set(CMAKE_CXX_CREATE_SHARED_LIBRARY
      "${CMAKE_BINARY_DIR}/bin/charmc -build-shared <CMAKE_CXX_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES> ${MY_EXTRA_OPTS}")
set(CMAKE_CXX_CREATE_STATIC_LIBRARY
      "${CMAKE_BINARY_DIR}/bin/charmc ${BUILD_SHARED_OPTION} <CMAKE_CXX_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES> ${MY_EXTRA_OPTS}")

set(CMAKE_C_CREATE_SHARED_LIBRARY
      "${CMAKE_BINARY_DIR}/bin/charmc -build-shared <CMAKE_C_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES> ${MY_EXTRA_OPTS}")
set(CMAKE_C_CREATE_STATIC_LIBRARY
      "${CMAKE_BINARY_DIR}/bin/charmc ${BUILD_SHARED_OPTION} <CMAKE_C_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES> ${MY_EXTRA_OPTS}")

set(CMAKE_Fortran_CREATE_SHARED_LIBRARY
      "${CMAKE_BINARY_DIR}/bin/charmc -build-shared <CMAKE_Fortran_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES> ${MY_EXTRA_OPTS}")
set(CMAKE_Fortran_CREATE_STATIC_LIBRARY
      "${CMAKE_BINARY_DIR}/bin/charmc ${BUILD_SHARED_OPTION} <CMAKE_Fortran_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES> ${MY_EXTRA_OPTS}")


set(CMK_COMPILER_KNOWS_C11 1)
set(CMK_COMPILER_KNOWS_CPP11 1)

if(NOT CMK_AMPI_ONLY)
  set(CMK_AMPI_ONLY 0)
endif()

#disable ROMIO due to GCC 14 cascade of failures
if(NOT DEFINED CMK_AMPI_WITH_ROMIO)
  set(CMK_AMPI_WITH_ROMIO 0)
endif()
set(CMK_ENABLE_C11   ${CMAKE_C11_EXTENSION_COMPILE_OPTION})

# Use -std=gnu++11 if available
if(NOT "${CMAKE_CXX11_EXTENSION_COMPILE_OPTION}" STREQUAL "")
  string(REPLACE ";" " " MY_CMAKE_CXX11_EXTENSION_COMPILE_OPTION "${CMAKE_CXX11_EXTENSION_COMPILE_OPTION}")
  set(OPTS_CXX "${OPTS_CXX} ${MY_CMAKE_CXX11_EXTENSION_COMPILE_OPTION}")
  set(CMK_ENABLE_CPP11 ${MY_CMAKE_CXX11_EXTENSION_COMPILE_OPTION})
else()
  string(REPLACE ";" " " MY_CMAKE_CXX11_STANDARD_COMPILE_OPTION "${CMAKE_CXX11_STANDARD_COMPILE_OPTION}")
  set(OPTS_CXX "${OPTS_CXX} ${MY_CMAKE_CXX11_STANDARD_COMPILE_OPTION}")
  set(CMK_ENABLE_CPP11 ${MY_CMAKE_CXX11_STANDARD_COMPILE_OPTION})
endif()

set(CMK_CC ${CMAKE_C_COMPILER})
set(CMK_CXX ${CMAKE_CXX_COMPILER})
set(CMK_CF90 ${CMAKE_Fortran_COMPILER})

set(CMK_C_OPENMP ${OpenMP_C_FLAGS})
set(CMK_F_OPENMP ${OpenMP_Fortran_FLAGS})

# Shell scripts
set(CMK_VDIR ${VDIR})
set(CMK_GDIR ${GDIR})

if(NOT EXISTS ${CMAKE_SOURCE_DIR}/src/arch/${VDIR}/)
  message(FATAL_ERROR "Unsupported platform ${VDIR}.")
endif()

configure_file(src/arch/common/cc-clang.sh                   include/ COPYONLY)
configure_file(src/arch/common/cc-clang.h                    include/ COPYONLY)
configure_file(src/arch/common/cc-icx.sh                     include/ COPYONLY)
configure_file(src/arch/common/cc-icx.h                      include/ COPYONLY)
configure_file(src/arch/common/cc-craycc.h                   include/ COPYONLY)
configure_file(src/arch/common/cc-craycc.sh                  include/ COPYONLY)
configure_file(src/arch/common/cc-msvc.h                     include/ COPYONLY)
configure_file(src/arch/common/cc-msvc.sh                    include/ COPYONLY)
configure_file(src/arch/common/cc-nvhpc.sh                   include/ COPYONLY)
configure_file(src/arch/common/cc-nvhpc.h                    include/ COPYONLY)
configure_file(src/arch/common/cc-pgcc.h                     include/ COPYONLY)
configure_file(src/arch/common/cc-pgcc.sh                    include/ COPYONLY)
configure_file(src/arch/common/cc-icc.sh                     include/ COPYONLY)
configure_file(src/arch/common/cc-icc.h                      include/ COPYONLY)
configure_file(src/arch/common/cc-xlc.h                      include/ COPYONLY)
configure_file(src/arch/common/cc-xlc.sh                     include/ COPYONLY)
configure_file(src/arch/common/cc-xlc64.sh                   include/ COPYONLY)
configure_file(src/arch/common/cc-xlc64.h                    include/ COPYONLY)
configure_file(src/arch/common/cc-iccstatic.sh               include/ COPYONLY)
configure_file(src/arch/common/cc-iccstatic.h                include/ COPYONLY)
configure_file(src/arch/common/cc-gcc.sh                     include/ COPYONLY)
configure_file(src/arch/common/cc-gcc.h                      include/ COPYONLY)
configure_file(src/arch/common/cc-msvc.h                     include/ COPYONLY)
configure_file(src/arch/common/cc-msvc.sh                    include/ COPYONLY)
configure_file(src/arch/common/conv-mach-craype.sh           include/ COPYONLY)
configure_file(src/arch/common/conv-mach-cuda.sh             include/ COPYONLY)
configure_file(src/arch/common/conv-mach-cuda.h              include/ COPYONLY)
configure_file(src/arch/common/conv-mach-hip.sh              include/ COPYONLY)
configure_file(src/arch/common/conv-mach-hip.h               include/ COPYONLY)
configure_file(src/arch/common/conv-mach-darwin.sh           include/ COPYONLY)
configure_file(src/arch/common/conv-mach-flang.h             include/ COPYONLY)
configure_file(src/arch/common/conv-mach-flang.sh            include/ COPYONLY)
configure_file(src/arch/common/conv-mach-gfortran.sh         include/ COPYONLY)
configure_file(src/arch/common/conv-mach-gfortran.h          include/ COPYONLY)
configure_file(src/arch/common/conv-mach-ifort.sh            include/ COPYONLY)
configure_file(src/arch/common/conv-mach-ifort.h             include/ COPYONLY)
configure_file(src/arch/common/conv-mach-nolb.h              include/ COPYONLY)
configure_file(src/arch/common/conv-mach-nolb.sh             include/ COPYONLY)
configure_file(src/arch/common/conv-mach-nvfortran.sh        include/ COPYONLY)
configure_file(src/arch/common/conv-mach-nvfortran.h         include/ COPYONLY)
configure_file(src/arch/common/conv-mach-omp.sh              include/ COPYONLY)
configure_file(src/arch/common/conv-mach-omp.h               include/ COPYONLY)
configure_file(src/arch/common/conv-mach-ooc.h               include/ COPYONLY)
configure_file(src/arch/common/conv-mach-ooc.sh              include/ COPYONLY)
configure_file(src/arch/common/conv-mach-papi.sh             include/ COPYONLY)
configure_file(src/arch/common/conv-mach-papi.h              include/ COPYONLY)
configure_file(src/arch/common/conv-mach-perftools.sh        include/ COPYONLY)
configure_file(src/arch/common/conv-mach-perftools.h         include/ COPYONLY)
configure_file(src/arch/common/conv-mach-persistent.sh       include/ COPYONLY)
configure_file(src/arch/common/conv-mach-persistent.h        include/ COPYONLY)
configure_file(src/arch/common/conv-mach-pgf90.sh            include/ COPYONLY)
configure_file(src/arch/common/conv-mach-pgf90.h             include/ COPYONLY)
configure_file(src/arch/common/conv-mach-tsan.h              include/ COPYONLY)
configure_file(src/arch/common/conv-mach-tsan.sh             include/ COPYONLY)
configure_file(src/scripts/conv-config.sh                    include/ COPYONLY)
configure_file(src/arch/${VDIR}/conv-mach.sh                 include/ COPYONLY)

# MSVC is driven through these Unix-style wrappers (cc-msvc.sh); unistd.h is a
# stand-in for the flex-generated charmxi scanner, which includes it.
if(CMK_COMPILER STREQUAL "msvc")
  configure_file(src/arch/win/unix2nt_cc                     bin/ COPYONLY)
  configure_file(src/arch/win/unix2nt_ar                     bin/ COPYONLY)
  configure_file(src/arch/win/unistd.h                       include/ COPYONLY)
endif()

set(CUDA_DIR "")
set(HIP_DIR "")
if(BUILD_CUDA OR BUILD_HIP)

  file(GLOB_RECURSE hybridAPI-h-sources ${CMAKE_SOURCE_DIR}/src/arch/cuda/*.h)
  file(GLOB_RECURSE hybridAPI-cxx-sources ${CMAKE_SOURCE_DIR}/src/arch/cuda/*.cpp)
  foreach(file ${hybridAPI-h-sources})
    configure_file(${file} include/ COPYONLY)
  endforeach()

  if (BUILD_CUDA)
    if(CMAKE_VERSION VERSION_GREATER 3.17 OR CMAKE_VERSION VERSION_EQUAL 3.17)
      find_package(CUDAToolkit REQUIRED)
      set(CMAKE_CUDA_COMPILER "${CUDAToolkit_NVCC_EXECUTABLE}")
      enable_language(CUDA)
      set(CUDA_DIR "${CUDAToolkit_TARGET_DIR}")
    else()
      find_package(CUDA REQUIRED)
      set(CUDA_DIR "${CUDA_TOOLKIT_ROOT_DIR}")
    endif()

    # Find CUPTI library and include directory
    find_library(CUPTI_LIBRARY cupti
      HINTS "${CUDA_DIR}/extras/CUPTI/lib64"
            "${CUDA_DIR}/lib64"
    )
    set(CUPTI_INCLUDE_DIR "${CUDA_DIR}/extras/CUPTI/include")
    if(NOT CUPTI_LIBRARY)
      message(WARNING "CUPTI library not found. GPU load balancing will not be available.")
    else()
      message(STATUS "Found CUPTI: ${CUPTI_LIBRARY}")
      # Global, not per-target: the GPU load fields live in LDObjData, which
      # ck-core, ck-ldb and hybridapi all see. A per-target definition would
      # give them different layouts for the same struct.
      add_compile_definitions(HAPI_CUPTI_LB)
      set(HAPI_CUPTI_LB ON)
    endif()

    add_library(hybridapi ${hybridAPI-cxx-sources})
    add_dependencies(hybridapi ci-generated)

    if(CUPTI_LIBRARY)
      target_include_directories(hybridapi PRIVATE "${CUPTI_INCLUDE_DIR}")
      target_link_libraries(hybridapi ${CUPTI_LIBRARY})
    endif()

    if(TRACING)
      target_compile_definitions(hybridapi PRIVATE HAPI_TRACE)
    endif()
  endif()

  if (BUILD_HIP)
    add_compile_definitions(__HIP_PLATFORM_AMD__)
    # Modern ROCm/HIP detection
    if(NOT DEFINED ROCM_PATH)
      if(NOT DEFINED ENV{ROCM_PATH})
        set(ROCM_PATH "/opt/rocm" CACHE PATH "Path to ROCm installation")
      else()
        set(ROCM_PATH $ENV{ROCM_PATH} CACHE PATH "Path to ROCm installation")
      endif()
    endif()
    
    # Find hipcc wrapper for reference
    find_program(HIP_HIPCC_EXECUTABLE
      NAMES hipcc
      PATHS "${ROCM_PATH}/bin" "${ROCM_PATH}/hip/bin"
      NO_DEFAULT_PATH
    )
    
    if(NOT HIP_HIPCC_EXECUTABLE)
      message(FATAL_ERROR "Could not find hipcc. Please set ROCM_PATH to your ROCm installation directory.")
    endif()
    
    # Find the actual clang compiler used by ROCm (required by CMake)
    find_program(CMAKE_HIP_COMPILER
      NAMES clang++
      PATHS "${ROCM_PATH}/llvm/bin" "${ROCM_PATH}/bin"
      NO_DEFAULT_PATH
    )
    
    if(NOT CMAKE_HIP_COMPILER)
      message(FATAL_ERROR "Could not find ROCm clang++ compiler in ${ROCM_PATH}")
    endif()
    
    set(HIP_DIR "${ROCM_PATH}")
    set(CMAKE_HIP_ARCHITECTURES "gfx803;gfx900;gfx906;gfx908;gfx90a;gfx1030;gfx1100" CACHE STRING "HIP architectures")
    
    # Enable HIP language support
    enable_language(HIP)
    
    add_library(hybridapi ${hybridAPI-cxx-sources})
    add_dependencies(hybridapi ci-generated)
    target_include_directories(hybridapi PRIVATE "${ROCM_PATH}/include")

    if(TRACING)
      target_compile_definitions(hybridapi PRIVATE HAPI_TRACE)
    endif()
  endif()


endif()

# Header files
configure_file(src/arch/${VDIR}/conv-mach.h                  include/ COPYONLY)
configure_file(src/arch/common/conv-mach-common.h            include/ COPYONLY)
configure_file(src/arch/util/lrts-common.h                   include/ COPYONLY)
configure_file(src/arch/util/mempool.h                       include/ COPYONLY)
configure_file(src/util/pup_c_functions.h                    include/ COPYONLY)

set(src-util-h-sources src/util/SSE-Double.h src/util/SSE-Float.h
    src/util/ck128bitHash.h src/util/ckBIconfig.h src/util/ckbitvector.h
    src/util/ckcomplex.h src/util/ckdll.h src/util/ckhashtable.h
    src/util/ckimage.h src/util/cklists.h src/util/ckliststring.h
    src/util/ckregex.h src/util/cksequence.h src/util/cksequence_factory.h
    src/util/cksequence_internal.h src/util/ckstatistics.h src/util/ckvector3d.h
    src/util/cmirdmautils.h
    src/util/cmitls.h src/util/conv-lists.h src/util/crc32.h src/util/hilbert.h
    src/util/partitioning_strategies.h src/util/pup.h src/util/pup_c.h
    src/util/pup_cmialloc.h src/util/pup_mpi.h src/util/pup_paged.h
    src/util/pup_stl.h src/util/pup_toNetwork.h src/util/pup_toNetwork4.h
    src/util/rand48_replacement.h src/util/random_sequence.h
    src/util/simd.h src/util/spanningTree.h
    src/util/spanningTreeStrategy.h src/util/spanningTreeVertex.h
    src/util/strided_sequence.h src/util/treeStrategy_3dTorus_minBytesHops.h
    src/util/treeStrategy_3dTorus_minHops.h
    src/util/treeStrategy_nodeAware_minBytes.h
    src/util/treeStrategy_nodeAware_minGens.h src/util/treeStrategy_topoUnaware.h src/util/valgrind.h
    src/util/vector2d.h)

if(CMK_CAN_LINK_FORTRAN)
    set(src-util-h-sources
        ${src-util-h-sources}
        src/util/pupf.h
    )
endif()

foreach(filename ${src-util-h-sources})
    configure_file(${filename} include/ COPYONLY)
endforeach()


# Charmxi
add_subdirectory(src/xlat-i)
add_dependencies(charmxi create_symlinks)

# Hwloc. Nothing in a reconverse build includes this embedded copy (reconverse
# finds its own hwloc), and on Windows its autotools configure would have to
# run through the MSVC wrapper, so it is skipped there.
if(NOT CMK_WINDOWS)
  include(cmake/hwloc.cmake)
endif()

# Converse
include(cmake/converse.cmake)

# LIBS
if(${TARGET} STREQUAL "LIBS")
  add_subdirectory(src/libs/conv-libs)
  add_subdirectory(src/libs/ck-libs)
  set(EVERYLB 1)
endif()

# Charm++
add_subdirectory(src/ck-core)
add_subdirectory(src/ck-perf)

if(TRACING)
  add_subdirectory(src/ck-pics)
endif()

set(modulecompletion-h-sources src/libs/ck-libs/completion/completion.h)
set(modulecompletion-cxx-sources src/libs/ck-libs/completion/completion.C)
add_library(modulecompletion ${modulecompletion-cxx-sources} ${modulecompletion-h-sources} )
configure_file(src/libs/ck-libs/completion/completion.h include/ COPYONLY)
add_dependencies(modulecompletion ck)

# testrun
configure_file(src/scripts/testrun bin/ COPYONLY)

# Charm++ libraries

# CkLoop
set(ckloop-cxx-files src/libs/ck-libs/ckloop/CkLoop.C)
set(ckloop-h-files src/libs/ck-libs/ckloop/CkLambda.h src/libs/ck-libs/ckloop/CkLoopAPI.h src/libs/ck-libs/ckloop/CkLoop.h)
add_library(moduleCkLoop ${ckloop-cxx-files} ${ckloop-h-files} ${CMAKE_BINARY_DIR}/include/CkLoop.decl.h)
add_dependencies(moduleCkLoop ck)

foreach(file ${ckloop-h-files})
  configure_file(${file} ${CMAKE_BINARY_DIR}/include COPYONLY)
endforeach(file)

# allGather
configure_file(src/libs/ck-libs/allGather/allGather.h  ${CMAKE_BINARY_DIR}/include COPYONLY)
add_library(moduleallGather src/libs/ck-libs/allGather/allGather.C src/libs/ck-libs/allGather/allGather.h)
add_dependencies(moduleallGather ck)

# NDMeshStreamer
file(GLOB moduleNDMeshStreamer-h-sources src/libs/ck-libs/NDMeshStreamer/*.h)
set(moduleNDMeshStreamer-cxx-sources src/libs/ck-libs/NDMeshStreamer/NDMeshStreamer.C)
add_library(moduleNDMeshStreamer ${moduleNDMeshStreamer-cxx-sources}
    ${moduleNDMeshStreamer-h-sources}
    ${CMAKE_BINARY_DIR}/include/NDMeshStreamer.decl.h)
target_include_directories(moduleNDMeshStreamer PUBLIC .)

add_dependencies(moduleNDMeshStreamer ck)

foreach(filename ${moduleNDMeshStreamer-h-sources})
  configure_file(${filename} ${CMAKE_BINARY_DIR}/include/ COPYONLY)
endforeach()

configure_file(src/libs/ck-libs/NDMeshStreamer/libmoduleNDMeshStreamer.dep ${CMAKE_BINARY_DIR}/lib/ COPYONLY)


# TCharm. Migratable threads come from reconverse and isomalloc from
# libisomalloc (cmake/converse.cmake); src/libs/ck-libs/tcharm/reconverse-compat
# declares the few classic Converse functions reconverse defines but does not
# declare.
file(GLOB tcharm-c-sources src/libs/ck-libs/tcharm/*.c)
set(tcharm-h-sources
    src/libs/ck-libs/tcharm/tcharm.h
    src/libs/ck-libs/tcharm/tcharm_impl.h
    src/libs/ck-libs/tcharm/tcharmc.h
)
if(CMK_CAN_LINK_FORTRAN)
    set(tcharm-h-sources
        ${tcharm-h-sources}
        src/libs/ck-libs/tcharm/tcharmf.h
    )
endif()
set(tcharm-cxx-sources src/libs/ck-libs/tcharm/tcharm.C)
add_library(moduletcharm ${tcharm-cxx-sources} ${tcharm-h-sources} ${CMAKE_BINARY_DIR}/include/tcharm.decl.h)
add_library(moduletcharmmain src/libs/ck-libs/tcharm/tcharmmain.C ${CMAKE_BINARY_DIR}/include/tcharm.decl.h ${CMAKE_BINARY_DIR}/include/tcharmmain.decl.h)
add_library(tcharm-compat ${tcharm-c-sources} ${CMAKE_BINARY_DIR}/include/tcharm.decl.h ${CMAKE_BINARY_DIR}/include/tcharmmain.decl.h)

configure_file(src/libs/ck-libs/tcharm/libmoduletcharm.dep     ${CMAKE_BINARY_DIR}/lib/ COPYONLY)
configure_file(src/libs/ck-libs/tcharm/libmoduletcharmmain.dep ${CMAKE_BINARY_DIR}/lib/ COPYONLY)

foreach(f ${tcharm-h-sources})
    configure_file(${f} ${CMAKE_BINARY_DIR}/include COPYONLY)
endforeach(f)


add_dependencies(moduletcharmmain ck)
add_dependencies(moduletcharm ck)
add_dependencies(tcharm-compat ck)

if(${TARGET} STREQUAL "AMPI" OR (${TARGET} STREQUAL "LIBS" AND NOT BUILD_CHANGA))
  add_subdirectory(src/libs/ck-libs/ampi)
endif()

# Charm4py
if(${TARGET} STREQUAL "charm4py")
  # Create libcharm.so for Charm4py
  # add_compile_options(-build-shared)
  file(WRITE empty.cpp "// This file left intentionally blank. It is used for the charm4py build.")
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
  add_library(charm SHARED empty.cpp)
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib_so)
  # `converse` is an aggregation (UTILITY) target on Reconverse builds and
  # cannot be linked: libreconverse is the Converse layer (CCS core
  # included), charm_cxx_utils holds charm's utilities, TopoManager and
  # the CCS builtins, and isomalloc the init hook StartCharmExt registers
  # (-standalone means charmc adds none of these itself).
  set(CHARM4PY_RUNTIME_TARGETS reconverse charm_cxx_utils isomalloc)
  if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    target_link_libraries(charm ck ${CHARM4PY_RUNTIME_TARGETS} "-Llib/ -standalone -all_load -c++stl -shared")
  else()
    target_link_libraries(charm ck ${CHARM4PY_RUNTIME_TARGETS} "-Llib/ -standalone -whole-archive -c++stl -shared")
  endif()

  if (${BUILD_CUDA})
    target_link_libraries(charm cudart hybridapi)
  endif()

  if (${BUILD_HIP})
    target_link_libraries(charm hiprtc hybridapi)
  endif()

  if(${TRACING})
    if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
      target_link_libraries(charm trace-summary trace-projections "-Llib/ -standalone -all_load -c++stl -shared")
    else()
      target_link_libraries(charm trace-summary trace-projections "-Llib/ -standalone -whole-archive -c++stl -shared")
    endif()
  endif()

  if(TARGET hwloc)
    add_dependencies(charm hwloc)
  endif()

else()
  # Check that we are able to build and link an executable
  add_executable(ckhello ${CMAKE_SOURCE_DIR}/tests/charm++/simplearrayhello/hello.C)
  target_link_libraries(ckhello PRIVATE reconverse)
  set(ckhello_deps ck converse ckmain moduleNDMeshStreamer modulecompletion)
  if(BUILD_CUDA OR BUILD_HIP)
    list(APPEND ckhello_deps hybridapi)
  endif()
  add_dependencies(ckhello ${ckhello_deps})
endif()

# Create conv-mach-opt.sh
set(optfile_sh ${CMAKE_BINARY_DIR}/include/conv-mach-opt.sh)

file(WRITE  ${optfile_sh} "# Build-time options header for charmc, automatically generated by cmake.\n")
file(APPEND  ${optfile_sh} "[ -z \"$CHARMINC\" ] && CHARMINC=\".\"\n")

# This needs to appear before sourcing the cc-$compiler.sh file
file(APPEND  ${optfile_sh} "CMK_COMPILER_SUFFIX=${CMK_COMPILER_SUFFIX}\n")

if(NOT ${NETWORK} STREQUAL "mpi" AND NOT ${NETWORK} MATCHES "gni-" AND NOT ${NETWORK} MATCHES "-cray")
    file(APPEND ${optfile_sh} ". ${CMAKE_BINARY_DIR}/include/cc-${CMK_COMPILER}.sh\n")
    # The cc-*.sh compiler files add -lckqt to CMK_LIBS, a library this
    # build does not produce. This file is sourced last by
    # conv-config.sh (after conv-mach.sh has already sourced the compiler
    # file once), so stripping here is authoritative.
    file(APPEND ${optfile_sh} "CMK_LIBS=\"\${CMK_LIBS//-lckqt/}\"\n")
    if(BUILD_CUDA)
        # liblci.so uses the CUDA driver API directly, so every charmc-linked
        # binary needs -lcuda under reconverse+CUDA. Link against the toolkit
        # stub (standard NVIDIA practice for non-GPU build nodes); the real
        # driver resolves at runtime on GPU nodes. Classic never links LCI
        # and its link line is untouched.
        file(APPEND ${optfile_sh} "CMK_LIBS=\"$CMK_LIBS -L${CUDA_DIR}/lib64/stubs -lcuda\"\n")
    endif()
endif()

if(${BUILD_SHARED})
  set(CMK_NO_BUILD_SHARED "false")
else()
  set(CMK_NO_BUILD_SHARED "true")
endif()

file(APPEND ${optfile_sh} "CMK_NO_BUILD_SHARED=${CMK_NO_BUILD_SHARED}\n")

if(DISABLE_TLS)
  set(CMK_SUPPORTS_TLSGLOBALS "0")
  set(CMK_USER_DISABLED_TLS "1")
else()
  set(CMK_USER_DISABLED_TLS "0")
endif()


if(CMK_BUILD_ON_CXI)
	set(CMK_CXI 1)
	set(CXI ON)
endif()

foreach(l BUILDOPTS CMK_AMPI_WITH_ROMIO CMK_BUILD_PYTHON CMK_CAN_LINK_FORTRAN
        CMK_CHARMDEBUG CMK_COMPILER_KNOWS_TLSDIRECTSEGREFS CMK_HAS_INT16 CMK_HAS_MMAP
        CMK_LIBJPEG CMK_MOD_EXT CMK_SUPPORTS_FSGLOBALS CMK_SUPPORTS_PIPGLOBALS
        CMK_SUPPORTS_PIEGLOBALS CMK_SUPPORTS_SWAPGLOBALS CMK_SUPPORTS_TLSGLOBALS
        CMK_SYSLIBS CMK_TRACE_ENABLED CP OPTS_CC OPTS_CXX CMK_VDIR CMK_GDIR
        CMK_HAS_ELF_H CMK_HAS_OPENMP CMK_AMPI_ONLY CMK_WINDOWS
        CXX_NO_AS_NEEDED LDXX_WHOLE_ARCHIVE_PRE LDXX_WHOLE_ARCHIVE_POST
        CMK_MACOSX CMK_POST_EXE CMK_SHARED_SUF CMK_USER_SUFFIX OPTS_LD
        CMK_COMPILER_KNOWS_FVISIBILITY CMK_LINKER_KNOWS_UNDEFINED
        CMK_SUPPORTS_MEMORY_ISOMALLOC CUDA_DIR HIP_DIR CMK_USER_DISABLED_TLS CMK_CXI
        CMK_RECONVERSE)
    file(APPEND ${optfile_sh} "${l}=\"${${l}}\"\n" )
endforeach(l)

if(NOT ${CMAKE_DL_LIBS} STREQUAL "")
    set(DL_LIB "-l${CMAKE_DL_LIBS}")
endif()

if (NOT CMK_WINDOWS)
  file(APPEND ${optfile_sh} "CMK_LIBS=\"$CMK_LIBS -lz ${DL_LIB}\"\n")
endif()

if(CMK_COVERAGE)
  file(APPEND ${optfile_sh} "CMK_COVERAGE=1\n")
endif()


# Create conv-mach-opt.h
set(optfile_h ${CMAKE_BINARY_DIR}/include/conv-mach-opt.h)

file(WRITE  ${optfile_h} "/* Build-time options header, automatically generated by cmake. */\n")
foreach(l CMK_AMPI_WITH_ROMIO CMK_AMPI_ONLY CMK_POST_EXE CMK_SHARED_SUF
        CMK_USER_SUFFIX CMK_CXI)
    file(APPEND ${optfile_h} "#define ${l} ${${l}}\n")
endforeach(l)
# Use #undef before #define so this overrides converse.h's #ifndef default
# regardless of include order, without triggering a redefinition warning.
file(APPEND ${optfile_h} "#undef CMK_OPTIMIZE\n#define CMK_OPTIMIZE ${CMK_OPTIMIZE}\n")

if(NOT ${NETWORK} STREQUAL "mpi" AND NOT ${NETWORK} MATCHES "gni-" AND NOT ${NETWORK} MATCHES "-cray")
    file(APPEND ${optfile_h} "#include \"cc-${CMK_COMPILER}.h\"\n")
endif()


# Create conv-mach-opt.mak
set(optfile_mak ${CMAKE_BINARY_DIR}/include/conv-mach-opt.mak)

file(WRITE  ${optfile_mak} "# Build-time options header for Makefiles, automatically generated by cmake.\n")
foreach(l CUDA_DIR HIP_DIR BUILD_CUDA BUILD_HIP CMK_AMPI_WITH_ROMIO CMK_MACOSX CMK_BUILD_PYTHON
        CMK_CHARMDEBUG CMK_COMPILER CMK_GDIR CMK_HAS_MALLOC_HOOK CMK_HAS_MMAP CMK_LIBJPEG
        CMK_LUSTREAPI CMK_MULTICORE CMK_NO_BUILD_SHARED CMK_NO_PARTITIONS CMK_SHARED_SUF
        CMK_SMP CMK_SUPPORTS_FSGLOBALS CMK_SUPPORTS_PIPGLOBALS CMK_SUPPORTS_PIEGLOBALS
        CMK_SUPPORTS_SWAPGLOBALS CMK_SUPPORTS_TLSGLOBALS CMK_HAS_OPENMP
        CMK_TRACE_ENABLED CMK_USE_LRTS CMK_VDIR OPTSATBUILDTIME CMK_AMPI_ONLY CMK_WINDOWS
        CMK_USE_CMA CMK_USER_SUFFIX CMK_CAN_LINK_FORTRAN CMK_SUPPORTS_MEMORY_ISOMALLOC
	CMK_CXI)
    file(APPEND ${optfile_mak} "${l}:=${${l}}\n" )
endforeach(l)

# Add options
set(CUDA ${BUILD_CUDA}) # need CUDA to match conv-mach file name
set(HIP ${BUILD_HIP})   # need HIP to match conv-mach file name
foreach(opt SMP OMP TCP PTHREADS SYNCFT PXSHM PERSISTENT OOC CUDA HIP PAPI CXI NOLB TSAN PERFTOOLS)
    if(${opt})
        string(TOLOWER ${opt} optl)
        file(APPEND ${optfile_sh} ". ${CMAKE_BINARY_DIR}/include/conv-mach-${optl}.sh\n")
        file(APPEND ${optfile_h} "#include \"conv-mach-${optl}.h\"\n")
        set(opts_enabled "${opts_enabled}${opt} ")
    endif()
endforeach()
if(CRAYPE)   # conv-mach-craype has only a .sh file
    file(APPEND ${optfile_sh} ". ${CMAKE_BINARY_DIR}/include/conv-mach-craype.sh\n")
    set(opts_enabled "${opts_enabled}CRAYPE ")
endif()
# Options that need no .h/.sh additions
foreach(opt TRACING TRACING_COMMTHREAD ERROR_CHECKING LBUSERDATA QLOGIC
  BUILD_SHARED TASK_QUEUE DRONE_MODE LOCKLESS_QUEUE CHARMDEBUG CCS CONTROLPOINT
  AMPI_ERROR_CHECKING AMPI_MPICH_TESTS NUMA RANDOMIZED_MSGQ REPLAY SHRINKEXPAND
  STATS ZLIB)
    if(${opt})
        set(opts_enabled "${opts_enabled}${opt} ")
    endif()
endforeach()

# Determine OS/kernel version
find_program(LSB_RELEASE_EXEC lsb_release)

if(LSB_RELEASE_EXEC)
  execute_process(COMMAND ${LSB_RELEASE_EXEC} -ds
             OUTPUT_VARIABLE OS_RELEASE
             OUTPUT_STRIP_TRAILING_WHITESPACE)
elseif(CMK_MACOSX)
  execute_process(COMMAND sw_vers -productVersion
             OUTPUT_VARIABLE OS_RELEASE
             OUTPUT_STRIP_TRAILING_WHITESPACE)
  set(OS_RELEASE "MacOS ${OS_RELEASE}")
else()
  set(OS_RELEASE "${CMAKE_SYSTEM_NAME}")
endif()

set(OS_RELEASE "${OS_RELEASE} (${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_VERSION}, ${CMAKE_SYSTEM_PROCESSOR})")

cmake_host_system_information(RESULT MY_HOSTNAME QUERY FQDN)

get_target_property(MAIN_CFLAGS ck COMPILE_OPTIONS)
string(REPLACE ";" " " MAIN_CFLAGS "${MAIN_CFLAGS}")

if(NOT opts_enabled)
  set(opts_enabled "---")
endif()

# Get version information
find_program(GIT git)

if(GIT AND EXISTS ${CMAKE_SOURCE_DIR}/.git)
  execute_process(COMMAND git describe --exact-match --dirty=*
             OUTPUT_VARIABLE CHARM_VERSION_GIT
             RESULT_VARIABLE git_result
             OUTPUT_STRIP_TRAILING_WHITESPACE
             ERROR_QUIET
             )
  if(NOT ${git_result} EQUAL 0)
    execute_process(COMMAND git describe --long --always --dirty=*
             OUTPUT_VARIABLE CHARM_VERSION_GIT
             OUTPUT_STRIP_TRAILING_WHITESPACE
             )
    set(CHARM_REL "(dev)")
  else()
    set(CHARM_REL "(release)")
  endif()
else()
  set(CHARM_VERSION_GIT "v${CHARM_VERSION}")
endif()

# Print configuration
message("==============================")
message("Charm++ ${PROJECT_VERSION} configuration: ")
message("  OS version:      ${OS_RELEASE}")
message("  Charm++ version: ${CHARM_VERSION_GIT} ${CHARM_REL}")
message("  Hostname:        ${MY_HOSTNAME}")
message("  Machine layer:   ${CHARM_PLATFORM}")
message("  Build target:    ${TARGET}")
message("  Build type:      ${CMAKE_BUILD_TYPE}")
message("  C compiler:      ${CMAKE_C_COMPILER} [${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}]")
message("  C++ compiler:    ${CMAKE_CXX_COMPILER} [${CMAKE_CXX_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}]")
if(CMAKE_Fortran_COMPILER)
  message("  F90 compiler:    ${CMAKE_Fortran_COMPILER} [${CMAKE_Fortran_COMPILER_ID} ${CMAKE_Fortran_COMPILER_VERSION}]")
else()
  message("  F90 compiler:    ---")
endif()
message("  CMake:           ${CMAKE_COMMAND} [${CMAKE_VERSION}]")
message("  Charmc flags:    ${MAIN_CFLAGS}")
message("  Enabled options: ${opts_enabled}")
message("==============================")


# Make symlinks
if(CMK_WINDOWS)
  # add_executable(createlink src/arch/win/createlink.cpp)
  # target_compile_options(createlink PRIVATE -seq -D_WIN32_WINNT=0x0500)
  # target_link_libraries(createlink -seq)
  # set(SYSTEMLN "${CMAKE_BINARY_DIR}/bin/createlink.exe")
  set(SYSTEMLN "ln")
else()
  set(SYSTEMLN "ln -s")
endif()

add_custom_target(create_symlinks ALL
                  COMMAND ${CMAKE_COMMAND} -E create_symlink ${CMAKE_BINARY_DIR}/bin ${CMAKE_SOURCE_DIR}/bin
                  COMMAND ${CMAKE_COMMAND} -E create_symlink ${CMAKE_BINARY_DIR}/lib ${CMAKE_SOURCE_DIR}/lib
                  COMMAND test ${BUILD_SHARED} = 1 && ${CMAKE_COMMAND} -E create_symlink ${CMAKE_BINARY_DIR}/lib_so ${CMAKE_SOURCE_DIR}/lib_so || true
                  COMMAND ${CMAKE_COMMAND} -E create_symlink ${CMAKE_BINARY_DIR}/include ${CMAKE_SOURCE_DIR}/include
                  COMMAND ${CMAKE_COMMAND} -E create_symlink ${CMAKE_BINARY_DIR}/include ${CMAKE_SOURCE_DIR}/tmp

                  COMMAND ${CMAKE_COMMAND} -E create_symlink ${CMAKE_BINARY_DIR}/include ${CMAKE_BINARY_DIR}/tmp

                  COMMAND test -d ${CMAKE_BINARY_DIR}/benchmarks || env SYSTEMLN=${SYSTEMLN} ${CMAKE_SOURCE_DIR}/src/scripts/gathertree ${CMAKE_SOURCE_DIR}/benchmarks ${CMAKE_BINARY_DIR}/benchmarks && true
                  COMMAND test -d ${CMAKE_BINARY_DIR}/examples || env SYSTEMLN=${SYSTEMLN} ${CMAKE_SOURCE_DIR}/src/scripts/gathertree ${CMAKE_SOURCE_DIR}/examples ${CMAKE_BINARY_DIR}/examples && true
                  COMMAND test -d ${CMAKE_BINARY_DIR}/tests || env SYSTEMLN=${SYSTEMLN} ${CMAKE_SOURCE_DIR}/src/scripts/gathertree ${CMAKE_SOURCE_DIR}/tests ${CMAKE_BINARY_DIR}/tests && true
                  VERBATIM
                 )

# if(CMK_WINDOWS)
#   add_dependencies(create_symlinks createlink)
# endif()

# Create the .vdir/.gdir/charm-version.h/charm-version-git.h files
file(WRITE ${CMAKE_BINARY_DIR}/include/.gdir   "${CMK_GDIR}\n")
file(WRITE ${CMAKE_BINARY_DIR}/include/.vdir   "${CMK_VDIR}\n")
file(WRITE ${CMAKE_BINARY_DIR}/include/charm-version.h "#define CHARM_VERSION ${CHARM_VERSION}\n")
file(APPEND ${CMAKE_BINARY_DIR}/include/charm-version.h "#define CHARM_VERSION_MAJOR ${PROJECT_VERSION_MAJOR}\n")
file(APPEND ${CMAKE_BINARY_DIR}/include/charm-version.h "#define CHARM_VERSION_MINOR ${PROJECT_VERSION_MINOR}\n")
file(APPEND ${CMAKE_BINARY_DIR}/include/charm-version.h "#define CHARM_VERSION_PATCH ${PROJECT_VERSION_PATCH}\n")

file(WRITE ${CMAKE_BINARY_DIR}/include/charm-version-git.h "#define CHARM_VERSION_GIT \"${CHARM_VERSION_GIT}\"\n")


# Copy the Makefile used for building and running tests
configure_file(${CMAKE_SOURCE_DIR}/cmake/Makefile.tests ${CMAKE_BINARY_DIR}/include/Makefile COPYONLY)
configure_file(${CMAKE_SOURCE_DIR}/cmake/Makefile.tests.common ${CMAKE_BINARY_DIR}/include/Makefile.tests.common COPYONLY)

# Configure CharmConfig.cmake
include(CMakePackageConfigHelpers)
configure_package_config_file(
  ${CMAKE_SOURCE_DIR}/cmake/templates/CharmConfig.cmake.in
  "${CMAKE_BINARY_DIR}/lib/cmake/Charm/CharmConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/cmake/Charm/CharmConfig.cmake
  )

set(VERSION_COMPATIBILITY_MODE SameMinorVersion)

write_basic_package_version_file(
  ${CMAKE_BINARY_DIR}/lib/cmake/Charm/CharmConfigVersion.cmake
  COMPATIBILITY ${VERSION_COMPATIBILITY_MODE})

# Installation rules

# install all includes
install(
  DIRECTORY ${CMAKE_BINARY_DIR}/include
  DESTINATION .)

# install all libraries
install(
  DIRECTORY ${CMAKE_BINARY_DIR}/lib
  DESTINATION .)

# install shared libraries if present
if (${BUILD_SHARED})
install(
  DIRECTORY ${CMAKE_BINARY_DIR}/lib_so
  DESTINATION .)
endif()

# install all executables
install(
  DIRECTORY ${CMAKE_BINARY_DIR}/bin
  DESTINATION .
  PATTERN *
  PERMISSIONS
    OWNER_EXECUTE
    OWNER_WRITE
    OWNER_READ
    GROUP_EXECUTE
    GROUP_READ
    WORLD_READ
    WORLD_EXECUTE)
