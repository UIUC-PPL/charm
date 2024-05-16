set(ck-cxx-sources
    ../ck-cp/API-generator/cp_effects.C ../ck-cp/arrayRedistributor.C
    ../ck-cp/controlPoints.C ../ck-cp/pathHistory.C
    ../ck-ldb/BaseLB.C ../ck-ldb/CentralLB.C
    ../ck-ldb/CentralPredictor.C ../ck-ldb/ckgraph.C
    ../ck-ldb/ckheap.C ../ck-ldb/ckset.C
    ../ck-ldb/DistBaseLB.C ../ck-ldb/forestTest.C
    ../ck-ldb/HybridBaseLB.C
    ../ck-ldb/LBComm.C ../ck-ldb/LBDatabase.C
    ../ck-ldb/LBManager.C
    ../ck-ldb/lbdbf.C
    ../ck-ldb/LBMachineUtil.C
    ../ck-ldb/LBObj.C
    ../ck-ldb/LBSimulation.C ../ck-ldb/LButil.C
    ../ck-ldb/manager.C ../ck-ldb/MetaBalancer.C
    ../ck-ldb/readmodel.C
    ../ck-ldb/Refiner.C ../ck-ldb/RefinerTemp.C
    ../ck-ldb/TreeLB.C
    ../ck-ldb/treeTest.C ../ck-ldb/weakTest.C
    ../ck-perf/trace-common.C ../ck-perf/trace-controlPoints.C
    ../ck-perf/tracec.C ../ck-perf/tracef.C ../conv-perf/charmProjections.C
    ../util/ckbitvector.C ../util/ckregex.C
    ck.C ckarray.C ckarrayoptions.C ckcallback.C
    ckcheckpoint.C ckfutures.C ckIgetControl.C
    cklocation.C ckmemcheckpoint.C ckmulticast.C
    ckobjQ.C ckrdma.C ckrdmadevice.C ckreduction.C cksyncbarrier.C debug-charm.C
    debug-message.C init.C modifyScheduler.C mpi-interoperate.C msgalloc.C
    qd.C register.C sdag.C waitqd.C
)

set(ck-h-sources XArraySectionReducer.h charm++.h charm++_type_traits.h
    charm-api.h charm.h ck.h ckIgetControl.h ckarray.h ckarrayindex.h
    ckarrayoptions.h ckcallback-ccs.h ckcallback.h ckcheckpoint.h
    ckmarshall.h ckentryopts.h ckfutures.h cklocation.h cklocrec.h
    ckmemcheckpoint.h ckmessage.h ckmigratable.h ckmulticast.h
    ckobjQ.h ckrdma.h ckrdmadevice.h ckreduction.h cksection.h
    ckstream.h cksyncbarrier.h debug-charm.h envelope-path.h envelope.h init.h
    middle-conv.h middle.h mpi-interoperate.h objid.h qd.h
    readonly.h register.h sdag.h stats.h waitqd.h ../ck-ldb/CentralLB.h
    ../ck-cp/arrayRedistributor.h ../ck-cp/controlPoints.h ../ck-cp/pathHistory.h ../ck-cp/API-generator/cp_effects.h)

if(CMK_CAN_LINK_FORTRAN)
    set(ck-h-sources ${ck-h-sources} charmf.h ../ck-cp/controlPointsf.h)
endif()

set(ldb-cxx-sources
    ../ck-ldb/CommonLBs.C  ../ck-ldb/DistributedLB.C
    ../ck-ldb/EveryLB.C
    ../ck-ldb/LButil.C
    ../ck-ldb/RecBipartLB.C
    ../ck-ldb/Refiner.C ../ck-ldb/RefinerTemp.C
    ../ck-ldb/TempAwareRefineLB.C
    ../ck-ldb/ckgraph.C ../ck-ldb/ckheap.C
    ../ck-ldb/ckset.C  ../ck-ldb/manager.C)


set(ldb-h-sources ../ck-ldb/BaseLB.h ../ck-ldb/CentralLBMsg.h
    ../ck-ldb/DistBaseLB.h ../ck-ldb/DistributedLB.h
    ../ck-ldb/HybridBaseLB.h ../ck-ldb/HybridLBMsg.h
    ../ck-ldb/LBComm.h
    ../ck-ldb/LBDatabase.h ../ck-ldb/LBManager.h ../ck-ldb/LBMachineUtil.h ../ck-ldb/LBOM.h
    ../ck-ldb/LBObj.h ../ck-ldb/LBSimulation.h
    ../ck-ldb/MetaBalancer.h ../ck-ldb/MetisLB.h
    ../ck-ldb/RandomForestModel.h
    ../ck-ldb/RecBipartLB.h
    ../ck-ldb/Refiner.h
    ../ck-ldb/RefinerTemp.h
    ../ck-ldb/ScotchLB.h ../ck-ldb/ScotchRefineLB.h
    ../ck-ldb/ScotchTopoLB.h ../ck-ldb/TempAwareRefineLB.h
    ../ck-ldb/TreeLB.h
    ../ck-ldb/ZoltanLB.h ../ck-ldb/ckgraph.h
    ../ck-ldb/ckheap.h ../ck-ldb/ckset.h
    ../ck-ldb/elements.h
    ../ck-ldb/lbdb.h ../ck-ldb/manager.h
    ../ck-ldb/idm.h ../util/json.hpp ../util/json_fwd.hpp)

include(${CMAKE_SOURCE_DIR}/cmake/ci-files.cmake)

add_library(ck ${ck-cxx-sources} ${ck-h-sources} ${all-ci-outputs})
add_library(ckmain main.C)
add_dependencies(ckmain ck)

# CommonLBs used to be a subset of EveryLB, but they were unified as
# part of the LB refactor. However, maintain both targets for
# backwards compatibility and for possible future divergence.
add_library(moduleCommonLBs ${ldb-cxx-sources} ${ldb-h-sources} ../ck-ldb/MetisLB.C ../ck-ldb/MetisLB.h)
add_library(moduleEveryLB ${ldb-cxx-sources} ${ldb-h-sources} ../ck-ldb/MetisLB.C ../ck-ldb/MetisLB.h)
configure_file(../ck-ldb/libmoduleCommonLBs.dep ${CMAKE_BINARY_DIR}/lib/ COPYONLY)
configure_file(../ck-ldb/libmoduleEveryLB.dep ${CMAKE_BINARY_DIR}/lib/ COPYONLY)
add_dependencies(moduleCommonLBs ck ckmetis)
add_dependencies(moduleEveryLB ck ckmetis)

add_library(moduleMetisLB ../ck-ldb/MetisLB.C ../ck-ldb/MetisLB.h)

add_subdirectory(${CMAKE_SOURCE_DIR}/src/libs/ck-libs/metis/libmetis ${CMAKE_BINARY_DIR}/src/libs/ck-libs/metis/libmetis) # defines "ckmetis" target

target_include_directories(ckmetis PRIVATE ${CMAKE_SOURCE_DIR}/src/libs/ck-libs/metis/GKlib )
target_include_directories(moduleMetisLB PRIVATE ${CMAKE_SOURCE_DIR}/src/libs/ck-libs/metis/libmetis)
target_include_directories(moduleCommonLBs PRIVATE ${CMAKE_SOURCE_DIR}/src/libs/ck-libs/metis/libmetis)
target_include_directories(moduleEveryLB PRIVATE ${CMAKE_SOURCE_DIR}/src/libs/ck-libs/metis/libmetis)

add_dependencies(moduleMetisLB ck ckmetis)

# Single LBs
add_library(moduleDistributedLB ../ck-ldb/DistributedLB.C)
add_dependencies(moduleDistributedLB ck)
add_library(moduleTreeLB ../ck-ldb/TreeLB.C)
add_dependencies(moduleTreeLB ck)
add_library(moduleRecBipartLB ../ck-ldb/RecBipartLB.C ../ck-ldb/RecBipartLB.h)
add_dependencies(moduleRecBipartLB ck)
add_library(moduleScotchLB EXCLUDE_FROM_ALL ../ck-ldb/ScotchLB.C ../ck-ldb/ScotchLB.h)
configure_file(../ck-ldb/libmoduleScotchLB.dep ${CMAKE_BINARY_DIR}/lib/ COPYONLY)
add_dependencies(moduleScotchLB ck)
add_custom_target(ScotchLB DEPENDS moduleScotchLB)

# Only for backwards compatibility
add_library(moduleCkMulticast ../libs/ck-libs/dummy/ckdummy.C)
configure_file(../libs/ck-libs/dummy/ckdummy.h ${CMAKE_BINARY_DIR}/include/ COPYONLY)

# CkIO
add_library(moduleCkIO ../libs/ck-libs/io/ckio.C ../libs/ck-libs/io/fs_parameters.c ../libs/ck-libs/io/ckio.h)
add_dependencies(moduleCkIO ck)
configure_file(../libs/ck-libs/io/ckio.h ${CMAKE_BINARY_DIR}/include/ COPYONLY)

if(CMK_LUSTREAPI)
    file(WRITE ${CMAKE_BINARY_DIR}/lib/libmoduleCkIO.dep "${CMK_LUSTREAPI}\n")
else()
    file(WRITE ${CMAKE_BINARY_DIR}/lib/libmoduleCkIO.dep "\n")
endif()

foreach(filename ${ck-h-sources} ${ldb-h-sources})
    configure_file(${filename} ${CMAKE_BINARY_DIR}/include/ COPYONLY)
endforeach()

target_include_directories(ck PRIVATE . ../ck-ldb ../ck-perf ../ck-cp ../util/topomanager ../conv-perf)
target_include_directories(ckmain PRIVATE . ../ck-ldb ../ck-perf ../ck-cp ../util/topomanager)

# Fortran interface
if(CMK_CAN_LINK_FORTRAN)
    add_library(ckf charmf.C charmmod.f90 charmf.h)
    add_dependencies(ckf ck)
    add_custom_command(TARGET ckf
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/charm.mod ${CMAKE_BINARY_DIR}/include/
        VERBATIM
    )
    add_library(ckmainf mainf.f90)

    add_library(f90charm ../langs/f90charm/f90main.C ${CMAKE_BINARY_DIR}/include/f90main.decl.h)
    add_dependencies(f90charm ck)
endif()

add_library(mpi-mainmodule mpi-mainmodule.C)
add_dependencies(mpi-mainmodule ck)
