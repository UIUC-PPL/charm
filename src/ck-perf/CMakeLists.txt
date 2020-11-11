set(ckperf-h-sources
    trace-controlPoints.h trace-controlPointsBOC.h trace-counter.h trace-common.h
    trace-memory.h trace-projections.h trace-projectionsBOC.h trace-projector.h
    trace-simple.h trace-simpleBOC.h trace-summary.h trace-summaryBOC.h
    trace-utilization.h trace.h tracec.h)

foreach(filename ${ckperf-h-sources})
    configure_file(${filename} ${CMAKE_BINARY_DIR}/include/ COPYONLY)
endforeach()

if(CMK_TRACE_ENABLED)
    add_library(trace-projector trace-projector.C ../conv-perf/traceCore.C
        ../conv-perf/traceCoreCommon.C ../conv-perf/charmProjections.C
        ../conv-perf/converseProjections.C ../conv-perf/machineProjections.C)
    add_dependencies(trace-projector ck)

    add_library(trace-projections trace-projections.C)
    add_dependencies(trace-projections ck)

    add_library(trace-controlPoints trace-controlPoints.C)
    add_dependencies(trace-controlPoints ck)

    add_library(trace-summary trace-summary.C )
    add_dependencies(trace-summary ck)

    add_library(trace-utilization trace-utilization.C)
    add_dependencies(trace-utilization ck)

    add_library(trace-simple trace-simple.C)
    add_dependencies(trace-simple ck)

    add_library(trace-counter trace-counter.C)
    add_dependencies(trace-counter ck)

    add_library(trace-memory trace-memory.C)
    add_dependencies(trace-memory ck)
endif()

add_library(trace-converse trace-converse.C)
add_dependencies(trace-converse ck)


if(CMK_CAN_LINK_FORTRAN)
    add_library(tracef_f tracef_f.f90)
    add_custom_command(TARGET tracef_f
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/tracemod.mod ${CMAKE_BINARY_DIR}/include/
        VERBATIM
    )
endif()
