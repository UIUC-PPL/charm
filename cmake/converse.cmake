
# conv-core
set(conv-core-h-sources
    src/util/cmitls.h
    src/conv-core/cmipool.h
    src/conv-core/cmishmem.h
    src/conv-core/cmidemangle.h
    src/conv-core/conv-config.h
    src/conv-core/conv-cpath.h
    src/conv-core/conv-cpm.h
    src/conv-core/conv-header.h
    src/conv-core/conv-ooc.h
    src/conv-core/conv-qd.h
    src/conv-core/conv-random.h
    src/conv-core/conv-rdma.h
    src/conv-core/conv-rdmadevice.h
    src/conv-core/conv-taskQ.h
    src/conv-core/conv-trace.h
    src/conv-core/converse.h
    src/conv-core/cpthreads.h
    src/conv-core/debug-conv++.h
    src/conv-core/debug-conv.h
    src/conv-core/hrctimer.h
    src/conv-core/mem-arena.h
    src/conv-core/memory-gnu-threads.h
    src/conv-core/memory-isomalloc.h
    src/conv-core/msgq.h
    src/conv-core/persistent.h
    src/conv-core/queueing.h
    src/conv-core/quiescence.h
    src/conv-core/taskqueue.h
)

set(conv-core-cxx-sources
    src/conv-core/cmipool.C
    src/conv-core/conv-conds.C
    src/conv-core/conv-rdma.C
    src/conv-core/conv-rdmadevice.C
    src/conv-core/convcore.C
    src/conv-core/cpm.C
    src/conv-core/cpthreads.C
    src/conv-core/cpuaffinity.C
    src/conv-core/debug-conv.C
    src/conv-core/futures.C
    src/conv-core/global-nop.C
    src/conv-core/isomalloc.C
    src/conv-core/mem-arena.C
    src/conv-core/memoryaffinity.C
    src/conv-core/msgmgr.C
    src/conv-core/quiescence.C
    src/conv-core/random.C
    src/util/cmitls.C
    src/conv-core/conv-interoperate.C
    src/conv-core/conv-taskQ.C
    src/conv-core/cputopology.C
    src/conv-core/debug-conv++.C
    src/conv-core/memory-darwin-clang.C
    src/conv-core/queueing.C
    src/conv-core/hrctimer.C
)

if(${CMK_USE_SHMEM})
    set(conv-core-cxx-sources
        "${conv-core-cxx-sources}"
        src/conv-core/shmem/cmishmem.C)
endif()

# conv-ccs
set(conv-ccs-h-sources
    src/conv-ccs/ccs-auth.h
    src/conv-ccs/ccs-builtins.h
    src/conv-ccs/ccs-server.h
    src/conv-ccs/conv-ccs.h
)

set(conv-ccs-cxx-sources
    src/conv-ccs/conv-ccs.C
    src/conv-ccs/ccs-builtins.C
    src/conv-ccs/middle-ccs.C
)

# conv-perf
set(conv-perf-cxx-sources
    src/conv-perf/traceCore.C
    src/conv-perf/traceCoreCommon.C
    src/conv-perf/machineProjections.C
)

set(conv-perf-h-sources
    src/conv-perf/allEvents.h
    src/conv-perf/charmEvents.h
    src/conv-perf/charmProjections.h
    src/conv-perf/converseEvents.h
    src/conv-perf/converseProjections.h
    src/conv-perf/machineEvents.h
    src/conv-perf/machineProjections.h
    src/conv-perf/threadEvents.h
    src/conv-perf/traceCore.h
    src/conv-perf/traceCoreAPI.h
    src/conv-perf/traceCoreCommon.h
)

# commitid
find_program(GIT git)

if(GIT AND EXISTS ${CMAKE_SOURCE_DIR}/.git)
  execute_process(COMMAND git describe --exact-match
             OUTPUT_VARIABLE CHARM_VERSION_GIT
             RESULT_VARIABLE git_result
             OUTPUT_STRIP_TRAILING_WHITESPACE
             ERROR_QUIET
             )
  if(NOT ${git_result} EQUAL 0)
    execute_process(COMMAND git describe --long --always
             OUTPUT_VARIABLE CHARM_VERSION_GIT
             OUTPUT_STRIP_TRAILING_WHITESPACE
             )
  endif()
else()
  set(CHARM_VERSION_GIT "v${CHARM_VERSION}")
endif()

file(GENERATE OUTPUT ${CMAKE_BINARY_DIR}/include/commitid.C CONTENT
"extern const char * const CmiCommitID;
const char * const CmiCommitID = \"${CHARM_VERSION_GIT}\";
"
)

# conv-util
set(conv-util-c-sources
    src/arch/util/lz4.c
)

set(conv-util-h-sources
    src/util/cmirdmautils.h
)

set(conv-util-cxx-sources
    src/arch/util/mempool.C
    src/arch/util/persist-comm.C
    src/util/cmirdmautils.C
    src/util/crc32.C
    src/util/sockRoutines.C
    src/util/ckdll.C
    src/util/ckhashtable.C
    src/util/ckimage.C
    src/util/conv-lists.C
    src/util/hilbert.C
    src/util/partitioning_strategies.C
    src/util/pup_c.C
    src/util/pup_cmialloc.C
    src/util/pup_paged.C
    src/util/pup_toNetwork.C
    src/util/pup_toNetwork4.C
    src/util/pup_util.C
    src/util/pup_xlater.C
    src/util/spanningTree.C
        src/util/fastforest.C
)

if(CMK_CAN_LINK_FORTRAN)
    add_library(conv-utilf pup_f.f90)
    add_custom_command(TARGET conv-utilf
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/pupmod.mod ${CMAKE_BINARY_DIR}/include/
        VERBATIM
    )
endif()

add_custom_command(OUTPUT pup_f.f90 COMMAND ${CMAKE_SOURCE_DIR}/src/util/pup_f.f90.sh > /dev/null)

# conv-partition
set(conv-partition-cxx-sources
    src/util/custom_partitioner.C
    src/util/set_partition_params.C
)

# conv-ldb
set(conv-ldb-cxx-sources
    src/conv-ldb/cldb.C
    src/conv-ldb/topology.C
    src/conv-ldb/edgelist.C
    src/conv-ldb/generate.C
)

set(conv-ldb-h-sources
    src/conv-ldb/cldb.h
    src/conv-ldb/graphdefs.h
    src/conv-ldb/topology.h
)

add_library(ldb-none src/conv-ldb/cldb.none.C ${conv-ldb-h-sources})
add_library(ldb-test src/conv-ldb/cldb.test.C ${conv-ldb-h-sources})
add_library(ldb-rand src/conv-ldb/cldb.rand.C ${conv-ldb-h-sources})
add_library(ldb-neighbor src/conv-ldb/cldb.neighbor.C src/conv-ldb/cldb.neighbor.h ${conv-ldb-h-sources})
add_library(ldb-workstealing src/conv-ldb/cldb.workstealing.C src/conv-ldb/cldb.workstealing.h ${conv-ldb-h-sources})
add_library(ldb-spray src/conv-ldb/cldb.spray.C ${conv-ldb-h-sources})
# add_library(ldb-prioritycentralized src/conv-ldb/cldb.prioritycentralized.C src/conv-ldb/cldb.prioritycentralized.h ${conv-ldb-h-sources})

# TopoManager
set(tmgr-c-sources src/util/topomanager/CrayNid.c)
set(tmgr-cxx-sources src/util/topomanager/TopoManager.C)
set(tmgr-h-sources src/util/topomanager/TopoManager.h ${CMAKE_BINARY_DIR}/include/topomanager_config.h src/util/topomanager/XTTorus.h)
file(WRITE ${CMAKE_BINARY_DIR}/include/topomanager_config.h "// empty\n" )

# Converse
add_library(converse
    ${CMAKE_BINARY_DIR}/include/commitid.C
    ${conv-core-cxx-sources}
    ${conv-core-h-sources}
    ${conv-ccs-h-sources}
    ${conv-ccs-cxx-sources}
    ${conv-perf-cxx-sources}
    ${conv-perf-h-sources}
    ${conv-util-c-sources}
    ${conv-util-cxx-sources}
    ${conv-util-h-sources}
    ${conv-partition-cxx-sources}
    ${conv-ldb-cxx-sources}
    ${conv-ldb-h-sources}
    src/arch/${GDIR}/machine.C
    ${tmgr-c-sources}
    ${tmgr-cxx-sources}
    ${tmgr-h-sources}
    ${hwloc-objects}
    ${all-ci-outputs}
)
add_dependencies(converse hwloc)

foreach(filename
    ${conv-core-h-sources}
    ${conv-ccs-h-sources}
    ${conv-perf-h-sources}
    ${conv-util-h-sources}
    ${conv-ldb-h-sources}
    ${tmgr-h-sources}
)
    configure_file(${filename} ${CMAKE_BINARY_DIR}/include/ COPYONLY)
endforeach()

target_include_directories(converse PRIVATE src/arch/util) # for machine*.*
target_include_directories(converse PRIVATE src/util) # for sockRoutines.C
target_include_directories(converse PRIVATE src/conv-core src/util/topomanager src/ck-ldb src/ck-perf src/ck-cp)

# conv-static
add_library(conv-static OBJECT src/conv-core/conv-static.c)
add_dependencies(converse conv-static)
add_custom_command(TARGET converse
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/conv-static.dir/src/conv-core/conv-static.c.o ${CMAKE_BINARY_DIR}/lib/conv-static.o
    VERBATIM
)
