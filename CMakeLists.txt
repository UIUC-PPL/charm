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

# Version 3.4 added CMAKE_C_COMPILER_LAUNCHER and CMAKE_CXX_COMPILER_LAUNCHER
cmake_minimum_required(VERSION 3.4)

project(Charm++ LANGUAGES CXX C VERSION 6.10.1)

find_package(Threads REQUIRED)
find_package(OpenMP) # Do this before Fortran, in case we don't have a Fortran compiler


# We need C++11 for (almost) all targets
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Setting charmc as the compiler launcher for Fortran files only works with
# CMake 3.11.0 or above. See 'CMAKE_Fortran_COMPILER_LAUNCHER' below for details.
if(${CMAKE_VERSION} VERSION_LESS "3.11.0")
  message(WARNING "CMake version 3.11.0 or higher is needed for Fortran support in Charm++.")
else()
  include(CheckLanguage)
  check_language(Fortran)
  if(CMAKE_Fortran_COMPILER)
    enable_language(Fortran OPTIONAL)
    include(FortranCInterface)
    FortranCInterface_VERIFY()
    FortranCInterface_VERIFY(CXX)
  else()
    message(STATUS "No Fortran support")
  endif()
endif()

if(${FortranCInterface_VERIFIED_CXX})
  set(CMK_CAN_LINK_FORTRAN 1)
else()
  set(CMK_CAN_LINK_FORTRAN 0)
endif()

if(${CMAKE_CXX_COMPILER_ID} STREQUAL "GNU")
  set(CMK_COMPILER gcc)
elseif(${CMAKE_CXX_COMPILER_ID} STREQUAL "AppleClang" OR ${CMAKE_CXX_COMPILER_ID} STREQUAL "Clang")
  set(CMK_COMPILER clang)
elseif(${CMAKE_CXX_COMPILER_ID} STREQUAL "Intel")
  set(CMK_COMPILER icc)
elseif(${CMAKE_CXX_COMPILER_ID} STREQUAL "Cray")
  set(CMK_COMPILER craycc)
elseif(${CMAKE_CXX_COMPILER_ID} STREQUAL "PGI")
  set(CMK_COMPILER pgcc)
elseif(${CMAKE_CXX_COMPILER_ID} STREQUAL "XL")
  set(CMK_COMPILER xlc)
elseif(${CMAKE_CXX_COMPILER_ID} STREQUAL "MSVC")
  set(CMK_COMPILER msvc)
else()
  message(FATAL_ERROR "Unknown compiler ${CMAKE_CXX_COMPILER_ID}")
endif()

# Build options
set(NETWORK "netlrts" CACHE STRING "Target network layer, can be one of mpi,multicore,netlrts,gni,ofi,pami,pamilrts,ucx,verbs")
set(TARGET "charm++" CACHE STRING "Target build type, can be one of charm++,AMPI,LIBS")

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
option(ERROR_CHECKING      "Enable error checking" OFF)
option(AMPI_ERROR_CHECKING "Enable AMPI error checking" OFF)
option(STATS               "Enable statistics collection" OFF)
option(TRACING             "Enable tracing modules" OFF)
option(TRACING_COMMTHREAD  "Enable tracing communication thread" OFF)
option(CHARMDEBUG          "Enable charmDebug" OFF)
option(REPLAY              "Enable record/replay" OFF)
option(CCS                 "Enable CCS" OFF)
option(CONTROLPOINT        "Enable control point" OFF)
option(LBUSERDATA          "Enable LB user data" OFF)
option(LOCKLESS_QUEUE      "Enable lockless queue for PE local and node queue" OFF)
option(SHRINKEXPAND        "Enable malleable jobs / shrink expand" OFF)
option(CHARMPY             "Enable CharmPy support" OFF)
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

# Charm++ dynamic libraries
option(BUILD_SHARED        "Build Charm++ dynamic libraries" OFF)

# Other options
option(EVERYLB             "Compile EveryLB suite of load balancing strategies" OFF)
option(F90CHARM            "Build f90charm" OFF)

set(EXTRA_OPTS "" CACHE STRING "Extra flags to pass to compilers.")
add_compile_options(${EXTRA_OPTS})

set(CMK_REPLAYSYSTEM ${REPLAY})

if(${REPLAY} AND NOT ${TRACING})
  set(CMK_REPLAYSYSTEM 0)
  message(WARNING "Charm record/replay is disabled because tracing is disabled")
endif()

if(${DRONE_MODE})
  set(CMK_DRONE_MODE 1)
else()
  set(CMK_DRONE_MODE 0)
endif()

if(${TASK_QUEUE})
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

if(${NETWORK} STREQUAL "multicore")
  set(SMP 0)
endif()

set(CMK_USE_ZLIB ${ZLIB})
if(${CMK_USE_ZLIB})
  find_package(ZLIB REQUIRED)
  set(CMK_SYSLIBS "${CMK_SYSLIBS} -lz")
endif()

find_package(JPEG)
IF(JPEG_FOUND)
  set(CMK_LIBJPEG "-ljpeg")
ENDIF()

find_package(Python COMPONENTS Interpreter Development)
if(${Python_FOUND})
  set(CMK_HAS_PYTHON 1)
  set(CMK_PYTHON_VERSION ${Python_VERSION})
  set(CMK_BUILD_PYTHON ${Python_VERSION})
else()
  set(CMK_HAS_PYTHON 0)
endif()

set(CMK_REFNUM_TYPE ${REFNUM_TYPE})
set(CMK_LBTIME_TYPE ${LBTIME_TYPE})
set(CMK_LB_USER_DATA 0)

set(CMK_RANDOMIZED_MSGQ ${RANDOMIZED_MSGQ})
set(CMK_SHRINK_EXPAND ${SHRINKEXPAND})
set(CMK_SMP_TRACE_COMMTHREAD ${TRACING_COMMTHREAD})

set(CMK_TRACE_ENABLED ${TRACING})

set(CMK_WITH_CONTROLPOINT ${CONTROLPOINT})
set(CMK_WITH_STATS ${STATS})

set(CMK_CHARMDEBUG ${CHARMDEBUG})
set(CMK_CHARMPY ${CHARMPY})


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
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  set(CHARM_OS "darwin")
  set(CMK_MACOSX 1)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
  set(CHARM_OS "win")
  set(CMK_WINDOWS 1)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(CHARM_OS "linux")
  set(CMK_OS_IS_LINUX 1)
elseif(IS_DIRECTORY "/bgsys")
  set(CHARM_OS "bluegeneq")
  set(CMK_BLUEGENEQ 1)
else()
  message(FATAL_ERROR "Unsupported operating system ${CMAKE_SYSTEM_NAME}.")
endif()

set(CMK_POWER7 0)
if(CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
  set(CHARM_CPU "x86_64")
elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "powerpc" OR CMAKE_SYSTEM_PROCESSOR MATCHES "^ppc")
  set(CHARM_CPU "ppc")
  execute_process(COMMAND grep f Makefile RESULT_VARIABLE isppc7 OUTPUT_QUIET ERROR_QUIET)
  if(${isppc7})
    set(CMK_POWER7 1)
  endif()
elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "arm7")
  set(CHARM_CPU "arm7")
else()
  message(FATAL_ERROR "Unsupported CPU ${CMAKE_SYSTEM_PROCESSOR}.")
endif()


set(CHARM_PLATFORM "${NETWORK}-${CHARM_OS}-${CHARM_CPU}")

set(CMK_BUILD_MPI 0)
set(CMK_BUILD_ON_MPI 0)
if(${NETWORK} STREQUAL "mpi")
  set(CMK_BUILD_MPI 1)
  set(CMK_BUILD_ON_MPI 1)
  find_package(MPI REQUIRED)
endif()

set(CMK_BUILD_OFI 0)
if(${NETWORK} STREQUAL "ofi")
  set(CMK_BUILD_OFI 1)
endif()

set(CMK_BUILD_CRAY 0)
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
if(${CMAKE_BUILD_TYPE} MATCHES "Release" OR ${CMAKE_BUILD_TYPE} MATCHES "RelWithDebInfo")
    set(CMK_OPTIMIZE 1)
    add_compile_options(-optimize -production)
    set(BUILDOPTS "${BUILDOPTS} -optimize -production")
    set(OPTS "${OPTS} -optimize -production")
    set(OPTSATBUILDTIME "${OPTSATBUILDTIME} -optimize -production")
endif()

if(${CMAKE_BUILD_TYPE} MATCHES "Debug" OR ${CMAKE_BUILD_TYPE} MATCHES "RelWithDebInfo")
  add_compile_options(-g)
  set(BUILDOPTS "${BUILDOPTS} -g")
  set(OPTS "${OPTS} -g")
  set(OPTSATBUILDTIME "${OPTSATBUILDTIME} -g")
endif()

include(cmake/detect-features.cmake)
include(cmake/ci-files.cmake)

configure_file(${CMAKE_SOURCE_DIR}/src/scripts/charmc ${CMAKE_BINARY_DIR}/bin @ONLY)

# Use charmc to compile/link everything
# set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE ${CMAKE_BINARY_DIR}/bin/charmc)
# set_property(GLOBAL PROPERTY RULE_LAUNCH_LINK ${CMAKE_BINARY_DIR}/bin/charmc)
# set_property(GLOBAL PROPERTY RULE_LAUNCH_CUSTOM ${CMAKE_BINARY_DIR}/bin/charmc)
set(CMAKE_C_COMPILER_LAUNCHER ${CMAKE_BINARY_DIR}/bin/charmc)
set(CMAKE_CXX_COMPILER_LAUNCHER ${CMAKE_BINARY_DIR}/bin/charmc)
# FIXME: CMAKE_Fortran_COMPILER_LAUNCHER is the only reason we require CMake 3.11+.
# See also https://gitlab.kitware.com/cmake/cmake/issues/17499
set(CMAKE_Fortran_COMPILER_LAUNCHER ${CMAKE_BINARY_DIR}/bin/charmc)
set(CMAKE_CXX_LINK_EXECUTABLE
      "${CMAKE_BINARY_DIR}/bin/charmc <CMAKE_CXX_LINK_FLAGS> <LINK_FLAGS> <OBJECTS>  -o <TARGET> <LINK_LIBRARIES>")

set(CMAKE_CXX_CREATE_SHARED_LIBRARY
      "${CMAKE_BINARY_DIR}/bin/charmc -build-shared <CMAKE_CXX_LINK_FLAGS> <LINK_FLAGS> <OBJECTS>  -o <TARGET> <LINK_LIBRARIES>")
set(CMAKE_CXX_CREATE_STATIC_LIBRARY
      "${CMAKE_BINARY_DIR}/bin/charmc <CMAKE_CXX_LINK_FLAGS> <LINK_FLAGS> <OBJECTS>  -o <TARGET> <LINK_LIBRARIES>")

set(CMAKE_C_CREATE_SHARED_LIBRARY
      "${CMAKE_BINARY_DIR}/bin/charmc -build-shared <CMAKE_C_LINK_FLAGS> <LINK_FLAGS> <OBJECTS>  -o <TARGET> <LINK_LIBRARIES>")
set(CMAKE_C_CREATE_STATIC_LIBRARY
      "${CMAKE_BINARY_DIR}/bin/charmc <CMAKE_C_LINK_FLAGS> <LINK_FLAGS> <OBJECTS>  -o <TARGET> <LINK_LIBRARIES>")

set(CMAKE_Fortran_CREATE_SHARED_LIBRARY
      "${CMAKE_BINARY_DIR}/bin/charmc -build-shared <CMAKE_Fortran_LINK_FLAGS> <LINK_FLAGS> <OBJECTS>  -o <TARGET> <LINK_LIBRARIES>")
set(CMAKE_Fortran_CREATE_STATIC_LIBRARY
      "${CMAKE_BINARY_DIR}/bin/charmc <CMAKE_Fortran_LINK_FLAGS> <LINK_FLAGS> <OBJECTS>  -o <TARGET> <LINK_LIBRARIES>")


set(CHARM_VERSION ${PROJECT_VERSION})
set(CMK_COMPILER_KNOWS_C11 1)
set(CMK_COMPILER_KNOWS_CPP11 1)

if(NOT CMK_AMPI_ONLY)
  set(CMK_AMPI_ONLY 0)
endif()

if(NOT CMK_AMPI_WITH_ROMIO)
  set(CMK_AMPI_WITH_ROMIO 1)
endif()

set(CMK_ENABLE_C11   ${CMAKE_C11_EXTENSION_COMPILE_OPTION})
set(CMK_ENABLE_CPP11 ${CMAKE_CXX11_STANDARD_COMPILE_OPTION})
set(OPTS_CXX "${OPTS_CXX} ${CMAKE_CXX11_STANDARD_COMPILE_OPTION}")

set(CMK_CC ${CMAKE_C_COMPILER})
set(CMK_CXX ${CMAKE_CXX_COMPILER})
set(CMK_CF90 ${CMAKE_Fortran_COMPILER})

set(CMK_C_OPENMP ${OpenMP_C_FLAGS})
set(CMK_F_OPENMP ${OpenMP_Fortran_FLAGS})

# Shell scripts

if(EXISTS ${CMAKE_SOURCE_DIR}/src/arch/${NETWORK}/gdir_link)
  file(STRINGS src/arch/${NETWORK}/gdir_link GDIR)
else()
  set(GDIR ${NETWORK})
endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/src/arch/${CHARM_PLATFORM}/vdir_link)
  file(STRINGS src/arch/${CHARM_PLATFORM}/vdir_link VDIR)
else()
  set(VDIR ${CHARM_PLATFORM})
endif()

set(CMK_VDIR ${VDIR})
set(CMK_GDIR ${GDIR})

# configure_file(src/arch/common/conv-mach-opt.sh            include/ COPYONLY) # generated
# configure_file(src/arch/common/conv-mach-pre.sh            include/ COPYONLY) # unused
configure_file(src/arch/common/cc-clang.sh                   include/ COPYONLY)
configure_file(src/arch/common/cc-clang.h                    include/ COPYONLY)
configure_file(src/arch/common/cc-gcc.sh                     include/ COPYONLY)
configure_file(src/arch/common/cc-gcc.h                      include/ COPYONLY)
configure_file(src/arch/common/cc-mpi.sh                     include/ COPYONLY)
configure_file(src/arch/common/cc-mpiopts.sh                 include/ COPYONLY) #sh only
configure_file(src/arch/common/conv-mach-craype.sh           include/ COPYONLY)
configure_file(src/arch/common/conv-mach-cuda.sh             include/ COPYONLY)
configure_file(src/arch/common/conv-mach-darwin.sh           include/ COPYONLY)
configure_file(src/arch/common/conv-mach-flang.sh            include/ COPYONLY)
configure_file(src/arch/common/conv-mach-gfortran.sh         include/ COPYONLY)
configure_file(src/arch/common/conv-mach-ifort.sh            include/ COPYONLY)
configure_file(src/arch/common/conv-mach-nolb.sh             include/ COPYONLY)
configure_file(src/arch/common/conv-mach-omp.sh              include/ COPYONLY)
configure_file(src/arch/common/conv-mach-ooc.sh              include/ COPYONLY)
configure_file(src/arch/common/conv-mach-papi.sh             include/ COPYONLY)
configure_file(src/arch/common/conv-mach-pedantic.sh         include/ COPYONLY)
configure_file(src/arch/common/conv-mach-perftools.sh        include/ COPYONLY)
configure_file(src/arch/common/conv-mach-persistent.sh       include/ COPYONLY)
configure_file(src/arch/common/conv-mach-pgf90.sh            include/ COPYONLY)
configure_file(src/arch/common/conv-mach-syncft.sh           include/ COPYONLY)
configure_file(src/arch/common/conv-mach-tsan.sh             include/ COPYONLY)
configure_file(src/scripts/conv-config.sh                    include/ COPYONLY)
configure_file(src/arch/${VDIR}/conv-mach.sh                 include/ COPYONLY)

if(EXISTS ${CMAKE_SOURCE_DIR}/src/arch/${VDIR}/conv-mach-pxshm.sh)
  configure_file(src/arch/${VDIR}/conv-mach-pxshm.sh         include/ COPYONLY)
  configure_file(src/arch/${VDIR}/conv-mach-pxshm.h          include/ COPYONLY)
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/src/arch/${VDIR}/conv-mach-smp.sh)
  configure_file(src/arch/${VDIR}/conv-mach-smp.sh           include/ COPYONLY)
  configure_file(src/arch/${VDIR}/conv-mach-smp.h            include/ COPYONLY)
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/src/arch/${GDIR}/conv-mach-tcp.sh)
  configure_file(src/arch/${GDIR}/conv-mach-tcp.sh           include/ COPYONLY)
  configure_file(src/arch/${GDIR}/conv-mach-tcp.h            include/ COPYONLY)
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/src/arch/${GDIR}/conv-mach-local.sh)
  configure_file(src/arch/${GDIR}/conv-mach-local.sh           include/ COPYONLY)
  configure_file(src/arch/${GDIR}/conv-mach-local.h            include/ COPYONLY)
endif()

# Header files
configure_file(src/arch/${GDIR}/conv-common.h                include/ COPYONLY)
configure_file(src/arch/${VDIR}/conv-mach.h                  include/ COPYONLY)
configure_file(src/arch/common/conv-mach-common.h            include/ COPYONLY)
configure_file(src/arch/util/lrts-common.h                   include/ COPYONLY)
configure_file(src/arch/util/cmiqueue.h                      include/ COPYONLY)
configure_file(src/arch/util/lrtslock.h                      include/ COPYONLY)
configure_file(src/arch/util/mempool.h                       include/ COPYONLY)
configure_file(src/arch/util/machine.h                       include/ COPYONLY)
configure_file(src/arch/util/machine-lrts.h                  include/ COPYONLY)
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
# file(MAKE_DIRECTORY include/proc_management/simple_pmi)

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

# FIXME: these should be in include/proc_management
foreach(filename ${proc_management-sources})
    configure_file(${filename} include/ COPYONLY)
endforeach()

# FIXME: these should be in include/proc_management/simple_pmi
foreach(filename ${proc_management-simple_pmi-sources})
    configure_file(${filename} include/ COPYONLY)
endforeach()


# Charmxi
add_subdirectory(src/xlat-i)

# Converse
add_subdirectory(src/QuickThreads)
add_subdirectory(src/util/boost-context)
add_subdirectory(src/conv-core)
add_subdirectory(src/conv-ldb)
add_subdirectory(src/util)

# Hwloc
if(${BUILD_SHARED})
    set(hwloc_shared yes)
else()
    set(hwloc_shared no)
endif()

# Figure out arguments to pass to hwloc's configure
set(command ${CMAKE_BINARY_DIR}/bin/charmc -print-building-blocks $OPTS)
execute_process(COMMAND ${command} COMMAND grep CHARM_CC_FLAGS= COMMAND cut -c16- COMMAND tr -d \' COMMAND tr -d '\n' OUTPUT_VARIABLE CHARM_CC_FLAGS)
execute_process(COMMAND ${command} COMMAND grep CHARM_CC= COMMAND cut -c10- COMMAND tr -d \' COMMAND tr -d '\n' OUTPUT_VARIABLE CHARM_CC)

# FIXME: copy .so
set(hwloc_dir ${CMAKE_BINARY_DIR}/hwloc-prefix/src/hwloc)
include(ExternalProject)
ExternalProject_Add(hwloc
    URL ${CMAKE_SOURCE_DIR}/contrib/hwloc
    # touch: update timestamps on these files so that hwloc's Makefile does not invoke automake
    CONFIGURE_COMMAND touch ${hwloc_dir}/aclocal.m4 ${hwloc_dir}/Makefile.in ${hwloc_dir}/configure &&
        ${hwloc_dir}/configure
        --disable-cairo
        --disable-cuda
        --disable-gl
        --disable-libxml2
        --disable-nvml
        --disable-opencl
        --disable-pci
        --disable-libudev
        --disable-visibility
        --enable-embedded-mode
        --enable-shared=${hwloc_shared}
        --enable-static
        --with-hwloc-symbol-prefix=cmi_
        --without-x
        HWLOC_FLAGS=${CHARM_CC_FLAGS}
        CFLAGS=${CHARM_CC_FLAGS}
        CC=${CHARM_CC}
        > /dev/null
    BUILD_COMMAND make V=$(VERBOSE)
    COMMAND cp hwloc/.libs/libhwloc_embedded.a ${CMAKE_BINARY_DIR}/lib/
    COMMAND cp -f ${CMAKE_BINARY_DIR}/hwloc-prefix/src/hwloc/include/hwloc.h ${CMAKE_BINARY_DIR}/include/
    COMMAND cp -LRf ${CMAKE_SOURCE_DIR}/contrib/hwloc/include/hwloc ${CMAKE_BINARY_DIR}/include/
    COMMAND cp -f ${CMAKE_BINARY_DIR}/hwloc-prefix/src/hwloc-build/include/hwloc/autogen/config.h ${CMAKE_BINARY_DIR}/include/hwloc/autogen/
    INSTALL_COMMAND "" #empty
)

# TopoManager
set(tmgr-cxx-sources src/util/topomanager/TopoManager.C)
set(tmgr-h-sources src/util/topomanager/TopoManager.h ${CMAKE_BINARY_DIR}/include/topomanager_config.h)
file(WRITE ${CMAKE_BINARY_DIR}/include/topomanager_config.h "// empty\n" )
add_library(tmgr ${tmgr-cxx-sources} ${tmgr-h-sources})
foreach(filename ${tmgr-h-sources})
    configure_file(${filename} ${CMAKE_BINARY_DIR}/include/ COPYONLY)
endforeach()

# Charm++
add_subdirectory(src/ck-core)
add_subdirectory(src/ck-perf)
add_subdirectory(src/ck-pics)


set(modulecompletion-h-sources src/libs/ck-libs/completion/completion.h)
set(modulecompletion-cxx-sources src/libs/ck-libs/completion/completion.C)
add_library(modulecompletion ${modulecompletion-cxx-sources} ${modulecompletion-h-sources} )
configure_file(src/libs/ck-libs/completion/completion.h include/ COPYONLY)
add_dependencies(modulecompletion ck)

# Charmrun + testrun
if(EXISTS ${CMAKE_SOURCE_DIR}/src/arch/${NETWORK}/charmrun)
  configure_file(src/arch/${NETWORK}/charmrun ${CMAKE_BINARY_DIR}/bin COPYONLY)
else()
  add_subdirectory(src/util/charmrun-src)
  add_dependencies(charmrun create_symlinks)
endif()
configure_file(src/scripts/testrun bin/ COPYONLY)

# Charm++ libraries

# CkLoop
set(ckloop-cxx-files src/libs/ck-libs/ckloop/CkLoop.C)
file(GLOB ckloop-h-files src/libs/ck-libs/ckloop/*.h)
add_library(moduleCkLoop ${ckloop-cxx-files} ${ckloop-h-files} ${CMAKE_BINARY_DIR}/include/CkLoop.decl.h)
add_dependencies(moduleCkLoop ck)


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

if(NOT ${TARGET} STREQUAL "charm++")
  add_subdirectory(src/libs/ck-libs/ampi)
endif()

if(${TARGET} STREQUAL "LIBS")
  add_subdirectory(src/libs/conv-libs)
  add_subdirectory(src/libs/ck-libs)
endif()

if(${CMAKE_SYSTEM_NAME} STREQUAL "Windows")
  set(CMK_SUPPORTS_FSGLOBALS 0)
elseif(${CMK_HAS_DLOPEN} AND ${CMAKE_SYSTEM_NAME} STREQUAL "Darwin")
  set(CMK_SUPPORTS_FSGLOBALS 1)
elseif(${CMK_HAS_DLOPEN} AND (${CMK_HAS_READLINK} OR ${CMK_HAS_REALPATH}))
  set(CMK_SUPPORTS_FSGLOBALS 1)
else()
  set(CMK_SUPPORTS_FSGLOBALS 0)
endif()

if(${CMAKE_SYSTEM_NAME} STREQUAL "Windows")
  set(CMK_CAN_OPEN_SHARED_OBJECTS_DYNAMICALLY 0)
else()
  set(CMK_CAN_OPEN_SHARED_OBJECTS_DYNAMICALLY ${CMK_HAS_DLOPEN})
endif()

if(${CMK_HAS_DLMOPEN} AND ${CMK_CAN_OPEN_SHARED_OBJECTS_DYNAMICALLY})
  set(CMK_SUPPORTS_PIPGLOBALS 1)
else()
  set(CMK_SUPPORTS_PIPGLOBALS 0)
endif()

# Create conv-mach-opt.sh
set(optfile_sh ${CMAKE_BINARY_DIR}/include/conv-mach-opt.sh)

file(WRITE  ${optfile_sh} "# Build-time options header, automatically generated by cmake\n")
file(APPEND  ${optfile_sh} "[ -z \"$CHARMINC\" ] && CHARMINC=\".\"\n")

if(NOT ${NETWORK} STREQUAL "mpi")
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
        CMK_HAS_ELF_H CMK_HAS_OPENMP CMK_AMPI_ONLY CMK_WINDOWS CMK_BLUEGENEQ)
    file(APPEND ${optfile_sh} "${l}=\"${${l}}\"\n" )
endforeach(l)

if(NOT ${CMAKE_DL_LIBS} STREQUAL "")
    set(DL_LIB "-l${CMAKE_DL_LIBS}")
endif()
file(APPEND ${optfile_sh} "CMK_HWLOC_LIBS=\"-lhwloc_embedded -lm\"\n" )
file(APPEND ${optfile_sh} "CMK_LIBS=\"$CMK_LIBS -lz ${DL_LIB}\"\n")


# Create conv-mach-opt.h
set(optfile_h ${CMAKE_BINARY_DIR}/include/conv-mach-opt.h)

file(WRITE  ${optfile_h} "/* Build-time options header, automatically generated by cmake */\n")
foreach(l CMK_AMPI_WITH_ROMIO CMK_OPTIMIZE CMK_HAS_OPENMP CMK_AMPI_ONLY)
    file(APPEND ${optfile_h} "#define ${l} ${${l}}\n")
endforeach(l)

if(NOT ${NETWORK} STREQUAL "mpi")
    file(APPEND ${optfile_h} "#include \"cc-${CMK_COMPILER}.h\"\n")
endif()

# Create conv-mach-opt.mak
set(optfile_mak ${CMAKE_BINARY_DIR}/include/conv-mach-opt.mak)

file(WRITE  ${optfile_mak} "# Build-time options header, automatically generated by cmake\n")
foreach(l BUILD_CUDA CMK_AMPI_WITH_ROMIO CMK_MACOSX CMK_BLUEGENEQ CMK_BUILD_PYTHON
        CMK_CHARMDEBUG CMK_COMPILER CMK_GDIR CMK_HAS_MALLOC_HOOK CMK_HAS_MMAP CMK_LIBJPEG
        CMK_LUSTREAPI CMK_MULTICORE CMK_UTH CMK_NO_BUILD_SHARED CMK_NO_PARTITIONS
        CMK_SHARED_SUF CMK_SMP CMK_SUPPORTS_FSGLOBALS CMK_SUPPORTS_PIPGLOBALS
        CMK_TRACE_ENABLED CMK_USE_LRTS CMK_VDIR OPTSATBUILDTIME CMK_AMPI_ONLY CMK_WINDOWS)
    file(APPEND ${optfile_mak} "${l}:=${${l}}\n" )
endforeach(l)

# Add options
foreach(opt SMP OMP TCP PTHREADS)
    if(${opt})
        string(TOLOWER ${opt} optl)
        file(APPEND ${optfile_sh} ". ${CMAKE_BINARY_DIR}/include/conv-mach-${optl}.sh\n")
        file(APPEND ${optfile_h} "#include \"conv-mach-${optl}.h\"\n")
        set(opts_enabled "${opts_enabled}${opt} ")
    endif()
endforeach()

# Print configuration
message("==============================")
message("Charm++ ${CHARM_VERSION} configuration: ")
message("  Triplet:         ${CHARM_PLATFORM}")
message("  Network:         ${NETWORK}")
message("  Build target:    ${TARGET}")
message("  Build type:      ${CMAKE_BUILD_TYPE}")
message("  C compiler:      ${CMAKE_C_COMPILER} [${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}]")
message("  C++ compiler:    ${CMAKE_CXX_COMPILER} [${CMAKE_CXX_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}]")
message("  F90 compiler:    ${CMAKE_Fortran_COMPILER} [${CMAKE_Fortran_COMPILER_ID} ${CMAKE_Fortran_COMPILER_VERSION}]")
message("  Enabled options: ${opts_enabled}")
message("==============================" )

# Make symlinks
add_custom_target(create_symlinks ALL
                  COMMAND ${CMAKE_COMMAND} -E create_symlink ${CMAKE_BINARY_DIR}/bin ${CMAKE_SOURCE_DIR}/bin
                  COMMAND ${CMAKE_COMMAND} -E create_symlink ${CMAKE_BINARY_DIR}/lib ${CMAKE_SOURCE_DIR}/lib
                  COMMAND test ${BUILD_SHARED} -eq 1 && ${CMAKE_COMMAND} -E create_symlink ${CMAKE_BINARY_DIR}/lib_so ${CMAKE_SOURCE_DIR}/lib_so || true
                  COMMAND ${CMAKE_COMMAND} -E create_symlink ${CMAKE_BINARY_DIR}/include ${CMAKE_SOURCE_DIR}/include
                  COMMAND ${CMAKE_COMMAND} -E create_symlink ${CMAKE_BINARY_DIR}/include ${CMAKE_SOURCE_DIR}/tmp

                  COMMAND ${CMAKE_COMMAND} -E create_symlink ${CMAKE_BINARY_DIR}/include ${CMAKE_BINARY_DIR}/tmp

                  COMMAND test -d ${CMAKE_BINARY_DIR}/benchmarks || env SYSTEMLN='ln -s' ${CMAKE_SOURCE_DIR}/src/scripts/gathertree ${CMAKE_SOURCE_DIR}/benchmarks ${CMAKE_BINARY_DIR}/benchmarks && true
                  COMMAND test -d ${CMAKE_BINARY_DIR}/examples || env SYSTEMLN='ln -s' ${CMAKE_SOURCE_DIR}/src/scripts/gathertree ${CMAKE_SOURCE_DIR}/examples ${CMAKE_BINARY_DIR}/examples && true
                  COMMAND test -d ${CMAKE_BINARY_DIR}/tests || env SYSTEMLN='ln -s' ${CMAKE_SOURCE_DIR}/src/scripts/gathertree ${CMAKE_SOURCE_DIR}/tests ${CMAKE_BINARY_DIR}/tests && true
                 )

# Create .vdir/.gdir/VERSION files
file(WRITE ${CMAKE_BINARY_DIR}/include/.gdir   "${CMK_GDIR}\n")
file(WRITE ${CMAKE_BINARY_DIR}/include/.vdir   "${CMK_VDIR}\n")
file(WRITE ${CMAKE_BINARY_DIR}/include/VERSION "${CHARM_VERSION}\n")


# Installation rules
install(DIRECTORY ${CMAKE_BINARY_DIR}/bin DESTINATION . PATTERN * PERMISSIONS OWNER_EXECUTE OWNER_WRITE OWNER_READ GROUP_EXECUTE GROUP_READ WORLD_READ WORLD_EXECUTE)
install(DIRECTORY ${CMAKE_BINARY_DIR}/lib     DESTINATION .)
if (${BUILD_SHARED})
  install(DIRECTORY ${CMAKE_BINARY_DIR}/lib_so  DESTINATION .)
endif()
install(DIRECTORY ${CMAKE_BINARY_DIR}/include DESTINATION .)
