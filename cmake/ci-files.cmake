# List all *.ci files in src/
file(GLOB_RECURSE ci-files ${CMAKE_SOURCE_DIR}/src/*.ci)

list(APPEND ci-files ${CMAKE_SOURCE_DIR}/tests/charm++/simplearrayhello/hello.ci)

foreach(in_f ${ci-files})

    # Special handling for ci files whose filename is not the same as the module name
    if(${in_f} MATCHES src/ck-pics/picsautoperf.ci)
        set(ci-output TraceAutoPerf.decl.h)
    elseif(${in_f} MATCHES src/libs/ck-libs/dummy/ckdummy.ci)
        set(ci-output CkDummy.decl.h)
    elseif(${in_f} MATCHES src/libs/ck-libs/io/ckio.ci)
        set(ci-output CkIO.decl.h)
    elseif(${in_f} MATCHES src/ck-core/ckreduction.ci)
        set(ci-output CkReduction.decl.h)
    elseif(${in_f} MATCHES src/ck-core/cklocation.ci)
        set(ci-output CkLocation.decl.h)
    elseif(${in_f} MATCHES src/ck-core/ckmulticast.ci)
        set(ci-output CkMulticast.decl.h)
    elseif(${in_f} MATCHES src/ck-core/ckarray.ci)
        set(ci-output CkArray.decl.h)
    elseif(${in_f} MATCHES src/ck-core/ckmarshall.ci)
        set(ci-output CkMarshall.decl.h)
    elseif(${in_f} MATCHES src/ck-cp/pathHistory.ci)
        set(ci-output PathHistory.decl.h)
    elseif(${in_f} MATCHES src/ck-core/ckfutures.ci)
        set(ci-output CkFutures.decl.h)
    elseif(${in_f} MATCHES src/ck-core/ckmemcheckpoint.ci)
        set(ci-output CkMemCheckpoint.decl.h)
    elseif(${in_f} MATCHES src/ck-core/ckcheckpointstatus.ci)
        set(ci-output CkCheckpointStatus.decl.h)
    elseif(${in_f} MATCHES src/ck-core/ckcallback.ci)
        set(ci-output CkCallback.decl.h)
    elseif(${in_f} MATCHES src/ck-core/ckcheckpoint.ci)
        set(ci-output CkCheckpoint.decl.h)
    elseif(${in_f} MATCHES src/ck-core/cksyncbarrier.ci)
        set(ci-output CkSyncBarrier.decl.h)
    elseif(${in_f} MATCHES src/ck-perf/trace-controlPoints.ci)
        set(ci-output TraceControlPoints.decl.h)
    elseif(${in_f} MATCHES src/ck-cp/controlPoints.ci)
        set(ci-output ControlPoints.decl.h)
    elseif(${in_f} MATCHES src/ck-cp/controlPointsNoTrace.ci)
        set(ci-output ControlPointsNoTrace.decl.h)
    elseif(${in_f} MATCHES src/ck-core/mpi-mainmodule.ci)
        set(ci-output mpi_main.decl.h)
    elseif(${in_f} MATCHES src/libs/ck-libs/multiphaseSharedArrays/msa-DistPageMgr.ci)
        set(ci-output msa.decl.h)
    elseif(${in_f} MATCHES src/libs/ck-libs/fem/fem_mesh_modify.ci)
        set(ci-output FEMMeshModify.decl.h)
    elseif(${in_f} MATCHES src/libs/ck-libs/pythonCCS/charmdebug-python.ci)
        set(ci-output charmdebug_python.decl.h)
    elseif(${in_f} MATCHES src/libs/ck-libs/ParFUM/mesh_modify.ci)
        set(ci-output ParFUM_Adapt.decl.h)
    elseif(${in_f} MATCHES src/libs/ck-libs/collide/threadCollide.ci)
        set(ci-output collide.decl.h)
    elseif(${in_f} MATCHES src/ck-core/ckarray.ci)
        set(ci-output CkArray.decl.h)
    elseif(${in_f} MATCHES src/libs/ck-libs/TMRC2D/tri.ci)
        # set(ci-output refine.decl.h) # ; not needed during LIBS build
        continue()
    elseif(${in_f} MATCHES src/libs/ck-libs/tmr/tri.ci)
        # set(ci-output tri_refine.decl.h) # incorrect file name to prevent name clash with TMRC2D/tri.ci ; not needed during LIBS build
        continue()
    elseif(${in_f} MATCHES src/libs/ck-libs/ParFUM-Tops-Dev/ParFUM_TOPS.ci)
        # set(ci-output ParFUM_TOPS_dev.decl.h) # incorrect file name to prevent name clash with ParFUM-Tops/ParFUM_TOPS.ci ; not needed during LIBS build
        continue()
    elseif(${in_f} MATCHES src/libs/ck-libs/TMRC2D/old_pgm.ci)
        # set(ci-output Pgm.decl.h) #disabled, as it clashes with another pgm.decl.h
        continue()
    elseif(${in_f} MATCHES src/libs/ck-libs/TMRC2D/refine.ci)
        set(ci-output TMR_Interface.decl.h)
    elseif(${in_f} MATCHES src/libs/ck-libs/pmaf/chunk.ci)
        set(ci-output PMAF.decl.h)
    elseif(${in_f} MATCHES src/ck-perf/trace-summary.ci)
        set(ci-output TraceSummary.decl.h)
    elseif(${in_f} MATCHES src/ck-perf/trace-simple.ci)
        set(ci-output TraceSimple.decl.h)
    elseif(${in_f} MATCHES src/ck-perf/trace-projections.ci)
        set(ci-output TraceProjections.decl.h)
    elseif(${in_f} MATCHES src/ck-perf/trace-utilization.ci)
        set(ci-output TraceUtilization.decl.h)

    else()
        # ci filename equal to module name
        get_filename_component(ci-output ${in_f} NAME_WE)
        string(APPEND ci-output ".decl.h")
    endif()

    string(REPLACE "decl.h" "def.h" ci-output-defh ${ci-output})

    set(all-ci-outputs ${all-ci-outputs} ${CMAKE_BINARY_DIR}/include/${ci-output} ${CMAKE_BINARY_DIR}/include/${ci-output-defh})

    if(${ci-output} MATCHES "search.decl.h")
        set(all-ci-outputs ${all-ci-outputs} ${CMAKE_BINARY_DIR}/include/cklibs/${ci-output} ${CMAKE_BINARY_DIR}/include/${ci-output-defh})
        add_custom_command(
          OUTPUT ${CMAKE_BINARY_DIR}/include/cklibs/${ci-output} ${CMAKE_BINARY_DIR}/include/cklibs/${ci-output-defh}
          COMMAND ${CMAKE_BINARY_DIR}/bin/charmc -I. ${in_f}
          WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/include/cklibs
          DEPENDS ${in_f} charmxi
          )
    endif()
    add_custom_command(
      OUTPUT ${CMAKE_BINARY_DIR}/include/${ci-output} ${CMAKE_BINARY_DIR}/include/${ci-output-defh}
      COMMAND ${CMAKE_BINARY_DIR}/bin/charmc -I. ${in_f}
      WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/include/
      DEPENDS ${in_f} charmxi
      )
endforeach()
