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

cmake_minimum_required(VERSION 3.4)

# Print warning
message("##################################################################")
message("# The CMake build system may not support advanced build options. #")
message("# Please let us know if you experience any problems by opening   #")
message("# an issue on our GitHub (https://github.com/UIUC-PPL/charm),    #")
message("# with the below configuration information.                      #")
message("# You can run './buildold' to use the previous build system.     #")
message("##################################################################")

if(CMAKE_VERSION VERSION_GREATER 3.12 OR CMAKE_VERSION VERSION_EQUAL 3.12)
  cmake_policy(SET CMP0075 OLD)
endif()

# Fortran is optional, so don't include it in LANGUAGES here
project(Charm++ LANGUAGES CXX C ASM VERSION 7.0.0)

find_package(Threads)
find_package(OpenMP) # Do this before Fortran, in case we don't have a Fortran compiler

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
  set(CMK_COMPILER clang)
  get_filename_component(mytmp ${CMAKE_CXX_COMPILER} NAME)
  if(mytmp MATCHES "-")
    string(REGEX REPLACE ".*(-.*)" "\\1" CMK_COMPILER_SUFFIX ${mytmp})
  endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Intel")
  set(CMK_COMPILER icc)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Cray")
  set(CMK_COMPILER craycc)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "PGI")
  set(CMK_COMPILER pgcc)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "XL")
  set(CMK_COMPILER xlc)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" OR (CMAKE_CXX_COMPILER_ID STREQUAL "" AND (CMAKE_SYSTEM_NAME STREQUAL "Windows" OR CMAKE_SYSTEM_NAME STREQUAL "CYGWIN")))
  set(CMK_COMPILER msvc)
else()
  message(FATAL_ERROR "Unknown compiler ${CMAKE_CXX_COMPILER_ID}")
endif()

# No Fortran support with MSVC
if(NOT CMK_COMPILER STREQUAL "msvc")
  enable_language(Fortran OPTIONAL)
endif()

if(CMAKE_Fortran_COMPILER)
  include(FortranCInterface)
  FortranCInterface_VERIFY()
  FortranCInterface_VERIFY(CXX)
else()
  message(STATUS "Charm++: No Fortran support.")
endif()

if(${FortranCInterface_VERIFIED_CXX})
  set(CMK_CAN_LINK_FORTRAN 1)
else()
  set(CMK_CAN_LINK_FORTRAN 0)
endif()

# Build options
set(NETWORK "netlrts" CACHE STRING "Target network layer, can be one of mpi,multicore,netlrts,gni,ofi,pami,pamilrts,ucx,verbs")
set(TARGET "charm++" CACHE STRING "Target build type, can be one of charm++,AMPI,LIBS,charm4py,ChaNGa,everylb,all-test")
set(ARCH "" CACHE STRING "Target architecture, can be one of i386,x86_64,arm7,arm8,ppc64le") # By default, detect arch we are running on.

if(NOT ARCH STREQUAL CMAKE_HOST_SYSTEM_PROCESSOR)
  if (ARCH STREQUAL "i386" AND CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL "x86_64")
    message(WARNING "Attempting to cross-compile from ${CMAKE_HOST_SYSTEM_PROCESSOR} to ${ARCH}. This is only supported for x86_64 -> i386 currently.")
    set(CMAKE_C_FLAGS -m32)
    set(CMAKE_CXX_FLAGS -m32)
    set(CMAKE_FORTRAN_FLAGS -m32)
  else()
    message(WARNING "Cross-compiling from  ${CMAKE_HOST_SYSTEM_PROCESSOR} to ${ARCH} is not supported currently.")
  endif()
endif()

# Platform specific options (choose multiple if desired):
option(SMP "Enable shared-memory communication within a single multi-processor machine instead of message passing." ON)
option(OMP "Enable OpenMP support in Charm++" OFF)
option(TCP "use TCP sockets for communication (only for netlrts)" OFF)
option(PTHREADS "compile with pthreads Converse threads" OFF)

# Advanced options:
option(OOC "Compile with out of core support" OFF)
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
option(CUDA                "Build with CUDA support" OFF)

option(PXSHM               "Build with PXSHM" OFF)

# LRTS PMI options
set(LRTS_PMI "" CACHE STRING "PMI type for UCX and OFI layers, can be one of simplepmi, slurmpmi, slurmpmi2, ompipmix, or openpmix")

set(EXTRA_OPTS "" CACHE STRING "Extra flags to pass to compilers.")

separate_arguments(MY_EXTRA_OPTS UNIX_COMMAND "${EXTRA_OPTS}")
add_compile_options("${MY_EXTRA_OPTS}")
string(REPLACE ";" " " MY_EXTRA_OPTS "${MY_EXTRA_OPTS}")
set(OPTS "${OPTS} ${MY_EXTRA_OPTS}")
set(OPTSATBUILDTIME "${OPTSATBUILDTIME} ${MY_EXTRA_OPTS}")

# We need both BUILD_CUDA and CUDA
set(BUILD_CUDA ${CUDA})

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

if(${NETWORK} STREQUAL "multicore")
  set(CMK_SMP 1)
  set(CMK_MULTICORE 1)
else()
  set(CMK_MULTICORE 0)
endif()

if(NOT ${CMK_COMPILER} STREQUAL "msvc")
  set(CMK_USE_ZLIB ${ZLIB})
else()
  set(CMK_USE_ZLIB 0)
endif()

if(${CMK_USE_ZLIB})
  find_package(ZLIB REQUIRED)
  set(CMK_SYSLIBS "$CMK_SYSLIBS -lz")
endif()

set(CMK_USE_LIBJPEG 0)
find_package(JPEG)
IF(JPEG_FOUND)
  set(CMK_LIBJPEG "-ljpeg")
  set(CMK_USE_LIBJPEG 1)
ENDIF()

# Check for Python
# PythonCCS requires Python v2.x and is the only target that uses this
# With CMake 3.12+ we could use FindPython() here
set(CMK_HAS_PYTHON 0)
set(CMK_PYTHON_VERSION "")
set(CMK_BUILD_PYTHON "")

set(Python_ADDITIONAL_VERSIONS 2)
find_package(PythonInterp)

if(PYTHONINTERP_FOUND)
  set(__MY_PYTHON_VER "${PYTHON_VERSION_MAJOR}.${PYTHON_VERSION_MINOR}")
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
set(CMK_BLUEGENEQ 0)
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
elseif(IS_DIRECTORY "/bgsys")
  set(CHARM_OS "bluegeneq")
  set(CMK_BLUEGENEQ 1)
  set(CMK_SUPPORTS_MEMORY_ISOMALLOC 0)
  message(DEPRECATION "Support for Blue Gene/Q targets has been deprecated as of Charm++ 7.0.0.")
else()
  message(FATAL_ERROR "Unsupported operating system ${CMAKE_SYSTEM_NAME}.")
endif()

set(CMK_POWER7 0)
if(ARCH)
  set(CHARM_CPU ${ARCH})
elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64" OR CMAKE_SYSTEM_PROCESSOR STREQUAL "AMD64")
  set(CHARM_CPU "x86_64")
elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "i386" OR CMAKE_SYSTEM_PROCESSOR STREQUAL "i686")
  set(CHARM_CPU "i386")
elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "powerpc" OR CMAKE_SYSTEM_PROCESSOR MATCHES "^ppc")
  set(CHARM_CPU "ppc64le")
  execute_process(COMMAND grep f Makefile RESULT_VARIABLE isppc7 OUTPUT_QUIET ERROR_QUIET)
  if(${isppc7})
    set(CMK_POWER7 1)
  endif()
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "armv6" OR CMAKE_SYSTEM_PROCESSOR MATCHES "armv7")
  set(CHARM_CPU "arm7")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64" OR CMAKE_SYSTEM_PROCESSOR MATCHES "arm64")
  set(CHARM_CPU "arm8")
else()
  message(FATAL_ERROR "Unsupported CPU ${CMAKE_SYSTEM_PROCESSOR}.")
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
if(${NETWORK} MATCHES "gni-")
  set(CHARM_PLATFORM "${NETWORK}")
  set(CMK_BUILD_CRAY 1)
elseif(${NETWORK} MATCHES "mpi-crayx")
  set(CHARM_PLATFORM "${NETWORK}")
endif()

set(CMK_BUILD_MPI 0)
set(CMK_BUILD_ON_MPI 0)
set(CMK_CXX_MPI_BINDINGS 1) # Always true, this just checked for a conflict with old mpich versions.
if(${NETWORK} STREQUAL "mpi")
  set(CMK_BUILD_MPI 1)
  set(CMK_BUILD_ON_MPI 1)
  find_package(MPI REQUIRED)
endif()

set(CMK_BUILD_ON_UCX 0)
if(${NETWORK} STREQUAL "ucx")
  set(CMK_BUILD_ON_UCX 1)
endif()

set(CMK_BUILD_OFI 0)
if(${NETWORK} STREQUAL "ofi")
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
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib_so)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

if (NOT CMAKE_INSTALL_PREFIX)
    set(CMAKE_INSTALL_PREFIX ./install)
endif()

if (NOT CMAKE_BUILD_TYPE)
    message(STATUS "${PROJECT_NAME}: No build type selected, default to Release")
    set(CMAKE_BUILD_TYPE "Release")
endif()

if (NOT CMK_MEMPOOL_CUTOFFNUM)
  set(CMK_MEMPOOL_CUTOFFNUM 26)
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

include(cmake/detect-features.cmake)
include(cmake/ci-files.cmake)

if(${TARGET} STREQUAL "all-test")
  add_custom_target(all-test ALL COMMAND $(MAKE) -C ${CMAKE_BINARY_DIR}/tmp all-test
        COMMENT "Building all-test. This will take several minutes.")
  add_dependencies(all-test moduleampi ampi-compat ck modulecompletion
        ampi_funcptr_shim ampi_funcptr_shim_main
        ampi_funcptr_loader ampi_funcptr_loader_stub
        converse conv-static ldb-rand
        memory-os-isomalloc memory-default threads-default ckmain moduletcharmmain
        ckqt tcharm-compat moduleNDMeshStreamer
        create_symlinks moduleCkCache trace-converse moduleCommonLBs
        moduleTreeLB moduleCkMulticast moduleCkIO conv-cpm memory-os-wrapper
        threads-default-tls ldb-neighbor ldb-workstealing
        modulecollidecharm modulecollide memory-os memory-gnu-isomalloc
        ampi_globals_fs)
  if(CMK_CAN_LINK_FORTRAN)
    add_dependencies(all-test moduleampif conv-utilf ckmainf
        ampi_funcptr_shim_fortran ampi_funcptr_shim_fmpimain)
  endif()
  if(CMK_SUPPORTS_FSGLOBALS)
    add_dependencies(all-test ampi_globals_fs)
  endif()
  if(CMK_SUPPORTS_PIPGLOBALS)
    add_dependencies(all-test ampi_globals_pip)
  endif()
  set(TARGET "LIBS")
endif()

configure_file(${CMAKE_SOURCE_DIR}/src/scripts/charmc ${CMAKE_BINARY_DIR}/bin @ONLY)

# Use charmc to compile/link everything
set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE ${CMAKE_BINARY_DIR}/bin/charmc)

set(CMAKE_CXX_LINK_EXECUTABLE
      "${CMAKE_BINARY_DIR}/bin/charmc <CMAKE_CXX_LINK_FLAGS> <LINK_FLAGS> <OBJECTS>  -o <TARGET> <LINK_LIBRARIES>")

set(CMAKE_CXX_CREATE_SHARED_LIBRARY
      "${CMAKE_BINARY_DIR}/bin/charmc -build-shared <CMAKE_CXX_LINK_FLAGS> <LINK_FLAGS> <OBJECTS>  -o <TARGET> <LINK_LIBRARIES>")
set(CMAKE_CXX_CREATE_STATIC_LIBRARY
      "${CMAKE_BINARY_DIR}/bin/charmc ${BUILD_SHARED_OPTION} <CMAKE_CXX_LINK_FLAGS> <LINK_FLAGS> <OBJECTS>  -o <TARGET> <LINK_LIBRARIES>")

set(CMAKE_C_CREATE_SHARED_LIBRARY
      "${CMAKE_BINARY_DIR}/bin/charmc -build-shared <CMAKE_C_LINK_FLAGS> <LINK_FLAGS> <OBJECTS>  -o <TARGET> <LINK_LIBRARIES>")
set(CMAKE_C_CREATE_STATIC_LIBRARY
      "${CMAKE_BINARY_DIR}/bin/charmc ${BUILD_SHARED_OPTION} <CMAKE_C_LINK_FLAGS> <LINK_FLAGS> <OBJECTS>  -o <TARGET> <LINK_LIBRARIES>")

set(CMAKE_Fortran_CREATE_SHARED_LIBRARY
      "${CMAKE_BINARY_DIR}/bin/charmc -build-shared <CMAKE_Fortran_LINK_FLAGS> <LINK_FLAGS> <OBJECTS>  -o <TARGET> <LINK_LIBRARIES>")
set(CMAKE_Fortran_CREATE_STATIC_LIBRARY
      "${CMAKE_BINARY_DIR}/bin/charmc ${BUILD_SHARED_OPTION} <CMAKE_Fortran_LINK_FLAGS> <LINK_FLAGS> <OBJECTS>  -o <TARGET> <LINK_LIBRARIES>")


set(CMK_COMPILER_KNOWS_C11 1)
set(CMK_COMPILER_KNOWS_CPP11 1)

if(NOT CMK_AMPI_ONLY)
  set(CMK_AMPI_ONLY 0)
endif()

if(NOT DEFINED CMK_AMPI_WITH_ROMIO)
  set(CMK_AMPI_WITH_ROMIO 1)
endif()

set(CMK_ENABLE_C11   ${CMAKE_C11_EXTENSION_COMPILE_OPTION})

# Use -std=gnu++11 if available
if(NOT "${CMAKE_CXX11_EXTENSION_COMPILE_OPTION}" STREQUAL "")
  set(OPTS_CXX "${OPTS_CXX} ${CMAKE_CXX11_EXTENSION_COMPILE_OPTION}")
  set(CMK_ENABLE_CPP11 ${CMAKE_CXX11_EXTENSION_COMPILE_OPTION})
else()
  set(OPTS_CXX "${OPTS_CXX} ${CMAKE_CXX11_STANDARD_COMPILE_OPTION}")
  set(CMK_ENABLE_CPP11 ${CMAKE_CXX11_STANDARD_COMPILE_OPTION})
endif()

set(CMK_CC ${CMAKE_C_COMPILER})
set(CMK_CXX ${CMAKE_CXX_COMPILER})
set(CMK_CF90 ${CMAKE_Fortran_COMPILER})

set(CMK_C_OPENMP ${OpenMP_C_FLAGS})
set(CMK_F_OPENMP ${OpenMP_Fortran_FLAGS})

# Shell scripts

if(EXISTS ${CMAKE_SOURCE_DIR}/src/arch/${NETWORK}/gdir_link)
  file(STRINGS src/arch/${NETWORK}/gdir_link GDIR)
elseif(${NETWORK} MATCHES "gni-")
  set(GDIR "gni")
elseif(${NETWORK} MATCHES "mpi-crayx")
  set(GDIR "mpi")
else()
  set(GDIR ${NETWORK})
endif()

set(VDIR ${CHARM_PLATFORM})

set(CMK_VDIR ${VDIR})
set(CMK_GDIR ${GDIR})

configure_file(src/arch/common/cc-bluegene.sh                include/ COPYONLY)
configure_file(src/arch/common/cc-clang.sh                   include/ COPYONLY)
configure_file(src/arch/common/cc-clang.h                    include/ COPYONLY)
configure_file(src/arch/common/cc-craycc.h                   include/ COPYONLY)
configure_file(src/arch/common/cc-craycc.sh                  include/ COPYONLY)
configure_file(src/arch/common/cc-msvc.h                     include/ COPYONLY)
configure_file(src/arch/common/cc-msvc.sh                    include/ COPYONLY)
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
configure_file(src/arch/common/cc-mpiopts.sh                 include/ COPYONLY)
configure_file(src/arch/common/cc-msvc.h                     include/ COPYONLY)
configure_file(src/arch/common/cc-msvc.sh                    include/ COPYONLY)
configure_file(src/arch/common/conv-mach-craype.sh           include/ COPYONLY)
configure_file(src/arch/common/conv-mach-cuda.sh             include/ COPYONLY)
configure_file(src/arch/common/conv-mach-cuda.h              include/ COPYONLY)
configure_file(src/arch/common/conv-mach-darwin.sh           include/ COPYONLY)
configure_file(src/arch/common/conv-mach-flang.h             include/ COPYONLY)
configure_file(src/arch/common/conv-mach-flang.sh            include/ COPYONLY)
configure_file(src/arch/common/conv-mach-gfortran.sh         include/ COPYONLY)
configure_file(src/arch/common/conv-mach-gfortran.h          include/ COPYONLY)
configure_file(src/arch/common/conv-mach-ifort.sh            include/ COPYONLY)
configure_file(src/arch/common/conv-mach-ifort.h             include/ COPYONLY)
configure_file(src/arch/common/conv-mach-nolb.h              include/ COPYONLY)
configure_file(src/arch/common/conv-mach-nolb.sh             include/ COPYONLY)
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
configure_file(src/arch/common/conv-mach-syncft.sh           include/ COPYONLY)
configure_file(src/arch/common/conv-mach-tsan.h              include/ COPYONLY)
configure_file(src/arch/common/conv-mach-tsan.sh             include/ COPYONLY)
configure_file(src/scripts/conv-config.sh                    include/ COPYONLY)
configure_file(src/arch/${VDIR}/conv-mach.sh                 include/ COPYONLY)

set(CUDA_DIR "")
if(CUDA)
  file(GLOB_RECURSE hybridAPI-h-sources ${CMAKE_SOURCE_DIR}/src/arch/cuda/*.h)
  file(GLOB_RECURSE hybridAPI-cxx-sources ${CMAKE_SOURCE_DIR}/src/arch/cuda/*.cpp)
  foreach(file ${hybridAPI-h-sources})
    configure_file(${file} include/ COPYONLY)
  endforeach()

  find_package(CUDA REQUIRED)
  set(CUDA_DIR ${CUDA_TOOLKIT_ROOT_DIR})
  add_library(cudahybridapi ${hybridAPI-cxx-sources})
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/src/arch/${VDIR}/conv-mach-pxshm.sh)
  configure_file(src/arch/${VDIR}/conv-mach-pxshm.sh         include/ COPYONLY)
  configure_file(src/arch/${VDIR}/conv-mach-pxshm.h          include/ COPYONLY)
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/src/arch/${VDIR}/conv-mach-sysvshm.sh)
  configure_file(src/arch/${VDIR}/conv-mach-sysvshm.sh       include/ COPYONLY)
  configure_file(src/arch/${VDIR}/conv-mach-sysvshm.h        include/ COPYONLY)
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/src/arch/${GDIR}/conv-mach-pxshm.sh)
  configure_file(src/arch/${GDIR}/conv-mach-pxshm.sh         include/ COPYONLY)
  configure_file(src/arch/${GDIR}/conv-mach-pxshm.h          include/ COPYONLY)
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/src/arch/${GDIR}/conv-mach-hugepages.sh)
  configure_file(src/arch/${GDIR}/conv-mach-hugepages.sh         include/ COPYONLY)
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/src/arch/${VDIR}/conv-mach-smp.sh)
  configure_file(src/arch/${VDIR}/conv-mach-smp.sh           include/ COPYONLY)
  configure_file(src/arch/${VDIR}/conv-mach-smp.h            include/ COPYONLY)
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/src/arch/${GDIR}/conv-mach-smp.sh)
  configure_file(src/arch/${GDIR}/conv-mach-smp.sh           include/ COPYONLY)
  configure_file(src/arch/${GDIR}/conv-mach-smp.h            include/ COPYONLY)
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/src/arch/${GDIR}/conv-mach-tcp.sh)
  configure_file(src/arch/${GDIR}/conv-mach-tcp.sh           include/ COPYONLY)
  configure_file(src/arch/${GDIR}/conv-mach-tcp.h            include/ COPYONLY)
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/src/arch/${GDIR}/conv-mach-local.sh)
  configure_file(src/arch/${GDIR}/conv-mach-local.sh           include/ COPYONLY)
  configure_file(src/arch/${GDIR}/conv-mach-local.h            include/ COPYONLY)
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/src/arch/${GDIR}/conv-mach-syncft.h)
  configure_file(src/arch/${GDIR}/conv-mach-syncft.h            include/ COPYONLY)
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/src/arch/${GDIR}/machine-persistent.h)
  configure_file(src/arch/${GDIR}/machine-persistent.h          include/ COPYONLY)
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/src/arch/${GDIR}/machine-persistent.C)
  configure_file(src/arch/${GDIR}/machine-persistent.C          include/ COPYONLY)
endif()

if(LRTS_PMI)
  configure_file(${CMAKE_SOURCE_DIR}/src/arch/${GDIR}/conv-mach-${LRTS_PMI}.sh include/ COPYONLY)
  configure_file(${CMAKE_SOURCE_DIR}/src/arch/${GDIR}/conv-mach-${LRTS_PMI}.h include/ COPYONLY)
endif()

if(CMK_COMPILER STREQUAL "msvc")
  configure_file(src/arch/win/unix2nt_cc                      bin/ COPYONLY)
  configure_file(src/arch/win/unix2nt_ar                      bin/ COPYONLY)
  configure_file(${CMAKE_SOURCE_DIR}/src/arch/win/unistd.h    include/ COPYONLY)
endif()

# Header files
configure_file(src/arch/${GDIR}/conv-common.h                include/ COPYONLY)
if(EXISTS src/arch/${GDIR}/conv-common.sh)
  configure_file(src/arch/${GDIR}/conv-common.sh               include/ COPYONLY)
endif()
configure_file(src/arch/${VDIR}/conv-mach.h                  include/ COPYONLY)
configure_file(src/arch/common/conv-mach-common.h            include/ COPYONLY)
configure_file(src/arch/util/lrts-common.h                   include/ COPYONLY)
configure_file(src/arch/util/cmiqueue.h                      include/ COPYONLY)
configure_file(src/arch/util/lrtslock.h                      include/ COPYONLY)
configure_file(src/arch/util/mempool.h                       include/ COPYONLY)
configure_file(src/arch/util/machine.h                       include/ COPYONLY)
configure_file(src/arch/util/machine-lrts.h                  include/ COPYONLY)
configure_file(src/arch/util/machine-rdma.h                  include/ COPYONLY)
configure_file(src/arch/util/machine-smp.h                   include/ COPYONLY)
configure_file(src/arch/util/pcqueue.h                       include/ COPYONLY)
configure_file(src/util/pup_c_functions.h                    include/ COPYONLY)

set(src-util-h-sources src/util/SSE-Double.h src/util/SSE-Float.h
    src/util/ck128bitHash.h src/util/ckBIconfig.h src/util/ckbitvector.h
    src/util/ckcomplex.h src/util/ckdll.h src/util/ckhashtable.h
    src/util/ckimage.h src/util/cklists.h src/util/ckliststring.h
    src/util/ckregex.h src/util/cksequence.h src/util/cksequence_factory.h
    src/util/cksequence_internal.h src/util/ckstatistics.h src/util/ckvector3d.h
    src/util/cmimemcpy.h src/util/cmimemcpy_qpx.h src/util/cmirdmautils.h
    src/util/cmitls.h src/util/conv-lists.h src/util/crc32.h src/util/hilbert.h
    src/util/partitioning_strategies.h src/util/pup.h src/util/pup_c.h
    src/util/pup_cmialloc.h src/util/pup_mpi.h src/util/pup_paged.h
    src/util/pup_stl.h src/util/pup_toNetwork.h src/util/pup_toNetwork4.h
    src/util/pupf.h src/util/rand48_replacement.h src/util/random_sequence.h
    src/util/simd.h src/util/sockRoutines.h src/util/spanningTree.h
    src/util/spanningTreeStrategy.h src/util/spanningTreeVertex.h
    src/util/strided_sequence.h src/util/treeStrategy_3dTorus_minBytesHops.h
    src/util/treeStrategy_3dTorus_minHops.h
    src/util/treeStrategy_nodeAware_minBytes.h
    src/util/treeStrategy_nodeAware_minGens.h src/util/treeStrategy_topoUnaware.h
    src/util/uFcontext.h src/util/uJcontext.h src/util/valgrind.h
    src/util/vector2d.h)

foreach(filename ${src-util-h-sources})
    configure_file(${filename} include/ COPYONLY)
endforeach()


# proc_management
if(${NETWORK} STREQUAL "ucx" OR ${NETWORK} STREQUAL "ofi")
  # file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/include/proc_management/simple_pmi)

  set(proc_management-sources
  src/arch/util/proc_management/runtime-pmi.C
  src/arch/util/proc_management/runtime-pmix.C
  src/arch/util/proc_management/runtime-codec.h
  src/arch/util/proc_management/runtime-pmi2.C
  src/arch/util/proc_management/runtime.h
  )

  set(proc_management-simple_pmi-sources
  src/arch/util/proc_management/simple_pmi/pmi.h
  src/arch/util/proc_management/simple_pmi/mpl.h
  src/arch/util/proc_management/simple_pmi/simple_pmi.C
  src/arch/util/proc_management/simple_pmi/simple_pmiutil.C
  src/arch/util/proc_management/simple_pmi/simple_pmiutil.h
  )

  # FIXME: this should be in include/proc_management/
  foreach(filename ${proc_management-sources})
      configure_file(${filename} include/ COPYONLY)
  endforeach()

  # FIXME: this should be in include/proc_management/simple_pmi
  foreach(filename ${proc_management-simple_pmi-sources})
      configure_file(${filename} include/ COPYONLY)
  endforeach()
endif()

# Charmxi
add_subdirectory(src/xlat-i)
add_dependencies(charmxi create_symlinks)

# Hwloc
include(cmake/hwloc.cmake)

# Converse
include(cmake/converse.cmake)
add_subdirectory(src/QuickThreads)
add_subdirectory(src/util/boost-context)
add_subdirectory(src/conv-core)

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

# Charmrun + testrun
if(EXISTS ${CMAKE_SOURCE_DIR}/src/arch/${NETWORK}/charmrun)
  configure_file(src/arch/${NETWORK}/charmrun ${CMAKE_BINARY_DIR}/bin COPYONLY)
elseif(EXISTS ${CMAKE_SOURCE_DIR}/src/arch/${GDIR}/charmrun)
  configure_file(src/arch/${GDIR}/charmrun ${CMAKE_BINARY_DIR}/bin COPYONLY)
elseif(EXISTS ${CMAKE_SOURCE_DIR}/src/arch/${VDIR}/charmrun)
  configure_file(src/arch/${VDIR}/charmrun ${CMAKE_BINARY_DIR}/bin COPYONLY)
else()
  add_subdirectory(src/util/charmrun-src)
  add_dependencies(charmrun create_symlinks)
endif()
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

# NDMeshStreamer
file(GLOB moduleNDMeshStreamer-h-sources src/libs/ck-libs/NDMeshStreamer/*.h)
set(moduleNDMeshStreamer-cxx-sources src/libs/ck-libs/NDMeshStreamer/NDMeshStreamer.C)
add_library(moduleNDMeshStreamer ${moduleNDMeshStreamer-cxx-sources}
    ${moduleNDMeshStreamer-h-sources}
    ${CMAKE_BINARY_DIR}/include/NDMeshStreamer.decl.h)
target_include_directories(moduleNDMeshStreamer PUBLIC .
    ../../../util/topomanager ../../../ck-ldb ../../../ck-perf ../../../ck-cp
    ../completion)

add_dependencies(moduleNDMeshStreamer ck)

foreach(filename ${moduleNDMeshStreamer-h-sources})
  configure_file(${filename} ${CMAKE_BINARY_DIR}/include/ COPYONLY)
endforeach()

configure_file(src/libs/ck-libs/NDMeshStreamer/libmoduleNDMeshStreamer.dep ${CMAKE_BINARY_DIR}/lib/ COPYONLY)


# TCharm
file(GLOB tcharm-c-sources src/libs/ck-libs/tcharm/*.c)
file(GLOB tcharm-h-sources src/libs/ck-libs/tcharm/*.h)
add_library(moduletcharm src/libs/ck-libs/tcharm/tcharm.C ${tcharm-h-sources} ${CMAKE_BINARY_DIR}/include/tcharm.decl.h)
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

if(${TARGET} STREQUAL "AMPI" OR ${TARGET} STREQUAL "LIBS")
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
  target_link_libraries(charm ck converse memory-default threads-default ldb-rand "-Llib/ -standalone -whole-archive -c++stl -shared")

  add_dependencies(charm hwloc)
endif()

# Check that we are able to build and link an executable
add_executable(ckhello ${CMAKE_SOURCE_DIR}/tests/charm++/simplearrayhello/hello.C)
add_dependencies(ckhello ck ldb-rand memory-default threads-default conv-static
  converse ckmain ckqt
  moduleNDMeshStreamer modulecompletion)

# Create conv-mach-opt.sh
set(optfile_sh ${CMAKE_BINARY_DIR}/include/conv-mach-opt.sh)

file(WRITE  ${optfile_sh} "# Build-time options header for charmc, automatically generated by cmake.\n")
file(APPEND  ${optfile_sh} "[ -z \"$CHARMINC\" ] && CHARMINC=\".\"\n")

# This needs to appear before sourcing the cc-$compiler.sh file
file(APPEND  ${optfile_sh} "CMK_COMPILER_SUFFIX=${CMK_COMPILER_SUFFIX}\n")

if(NOT ${NETWORK} STREQUAL "mpi" AND NOT ${NETWORK} MATCHES "gni-" AND NOT ${NETWORK} MATCHES "-crayx")
    file(APPEND ${optfile_sh} ". ${CMAKE_BINARY_DIR}/include/cc-${CMK_COMPILER}.sh\n")
endif()

if(${BUILD_SHARED})
  set(CMK_NO_BUILD_SHARED "false")
else()
  set(CMK_NO_BUILD_SHARED "true")
endif()

file(APPEND ${optfile_sh} "CMK_NO_BUILD_SHARED=${CMK_NO_BUILD_SHARED}\n")

foreach(l BUILDOPTS CMK_AMPI_WITH_ROMIO CMK_BUILD_PYTHON CMK_CAN_LINK_FORTRAN
        CMK_CHARMDEBUG CMK_COMPILER_KNOWS_TLSDIRECTSEGREFS CMK_HAS_INT16 CMK_HAS_MMAP
        CMK_LIBJPEG CMK_MOD_EXT CMK_SUPPORTS_FSGLOBALS CMK_SUPPORTS_PIPGLOBALS
        CMK_SYSLIBS CMK_TRACE_ENABLED CP OPTS_CC OPTS_CXX CMK_VDIR CMK_GDIR
        CMK_HAS_ELF_H CMK_HAS_OPENMP CMK_AMPI_ONLY CMK_WINDOWS CMK_BLUEGENEQ
        CXX_NO_AS_NEEDED LDXX_WHOLE_ARCHIVE_PRE LDXX_WHOLE_ARCHIVE_POST
        CMK_MACOSX CMK_POST_EXE CMK_SHARED_SUF CMK_USER_SUFFIX OPTS_LD
        CMK_COMPILER_KNOWS_FVISIBILITY CMK_LINKER_KNOWS_UNDEFINED
        CMK_SUPPORTS_MEMORY_ISOMALLOC CUDA_DIR)
    file(APPEND ${optfile_sh} "${l}=\"${${l}}\"\n" )
endforeach(l)

if(NOT ${CMAKE_DL_LIBS} STREQUAL "")
    set(DL_LIB "-l${CMAKE_DL_LIBS}")
endif()

if (NOT CMK_WINDOWS)
  file(APPEND ${optfile_sh} "CMK_LIBS=\"$CMK_LIBS -lz ${DL_LIB}\"\n")
endif()

if(LRTS_PMI)
  file(APPEND ${optfile_sh} ". ${CMAKE_BINARY_DIR}/include/conv-mach-${LRTS_PMI}.sh\n")
endif()

# Create conv-mach-opt.h
set(optfile_h ${CMAKE_BINARY_DIR}/include/conv-mach-opt.h)

file(WRITE  ${optfile_h} "/* Build-time options header, automatically generated by cmake. */\n")
foreach(l CMK_AMPI_WITH_ROMIO CMK_OPTIMIZE CMK_AMPI_ONLY CMK_POST_EXE CMK_SHARED_SUF
        CMK_USER_SUFFIX)
    file(APPEND ${optfile_h} "#define ${l} ${${l}}\n")
endforeach(l)

if(NOT ${NETWORK} STREQUAL "mpi" AND NOT ${NETWORK} MATCHES "gni-" AND NOT ${NETWORK} MATCHES "-crayx")
    file(APPEND ${optfile_h} "#include \"cc-${CMK_COMPILER}.h\"\n")
endif()

if(LRTS_PMI)
  file(APPEND ${optfile_h} "#include \"conv-mach-${LRTS_PMI}.h\"\n")
endif()

# Create conv-mach-opt.mak
set(optfile_mak ${CMAKE_BINARY_DIR}/include/conv-mach-opt.mak)

file(WRITE  ${optfile_mak} "# Build-time options header for Makefiles, automatically generated by cmake.\n")
foreach(l CUDA_DIR BUILD_CUDA CMK_AMPI_WITH_ROMIO CMK_MACOSX CMK_BLUEGENEQ CMK_BUILD_PYTHON
        CMK_CHARMDEBUG CMK_COMPILER CMK_GDIR CMK_HAS_MALLOC_HOOK CMK_HAS_MMAP CMK_LIBJPEG
        CMK_LUSTREAPI CMK_MULTICORE CMK_NO_BUILD_SHARED CMK_NO_PARTITIONS
        CMK_SHARED_SUF CMK_SMP CMK_SUPPORTS_FSGLOBALS CMK_SUPPORTS_PIPGLOBALS
        CMK_TRACE_ENABLED CMK_USE_LRTS CMK_VDIR OPTSATBUILDTIME CMK_AMPI_ONLY CMK_WINDOWS
        CMK_USE_CMA CMK_USER_SUFFIX CMK_CAN_LINK_FORTRAN CMK_SUPPORTS_MEMORY_ISOMALLOC)
    file(APPEND ${optfile_mak} "${l}:=${${l}}\n" )
endforeach(l)

# Add options
foreach(opt SMP OMP TCP PTHREADS SYNCFT PXSHM PERSISTENT OOC CUDA PAPI)
    if(${opt})
        string(TOLOWER ${opt} optl)
        file(APPEND ${optfile_sh} ". ${CMAKE_BINARY_DIR}/include/conv-mach-${optl}.sh\n")
        file(APPEND ${optfile_h} "#include \"conv-mach-${optl}.h\"\n")
        set(opts_enabled "${opts_enabled}${opt} ")
    endif()
endforeach()

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


# Installation rules
install(DIRECTORY ${CMAKE_BINARY_DIR}/bin DESTINATION . PATTERN * PERMISSIONS OWNER_EXECUTE OWNER_WRITE OWNER_READ GROUP_EXECUTE GROUP_READ WORLD_READ WORLD_EXECUTE)
install(DIRECTORY ${CMAKE_BINARY_DIR}/lib     DESTINATION .)
if (${BUILD_SHARED})
  install(DIRECTORY ${CMAKE_BINARY_DIR}/lib_so  DESTINATION .)
endif()
install(DIRECTORY ${CMAKE_BINARY_DIR}/include DESTINATION .)
