# This directory is solely for making the reconverse source tree available to
# the rest of the build.

set_directory_properties(PROPERTIES
  RULE_LAUNCH_COMPILE ""
  COMPILE_OPTIONS ""
  COMPILE_DEFINITIONS ""
  INCLUDE_DIRECTORIES ""
)

# Reconverse is a git submodule at contrib/reconverse, and the gitlink recorded
# there IS the pin: `git submodule status` shows which reconverse a given charm
# commit builds against, and moving the pairing is a reviewed commit like any
# other. That replaces the FetchContent pin this file used to carry, which had
# to defend itself against a stale CMake cache quietly holding the previous SHA
# (a build directory that had already configured went on fetching the old
# reconverse, and the mismatch surfaced as an unrelated-looking compile error).
# A submodule has no such cache: the source tree on disk is the pin.
#
# To build against a different reconverse, check the submodule out to it --
#   git -C contrib/reconverse fetch origin
#   git -C contrib/reconverse checkout <branch-or-sha>
# -- and rebuild; nothing in CMake needs to change. Committing the moved
# gitlink is what makes the new pairing everyone else's too.
set(RECONVERSE_SOURCE_DIR "" CACHE PATH
    "Reconverse source tree to build against (default: contrib/reconverse)")

set(_reconverse_submodule ${CMAKE_SOURCE_DIR}/contrib/reconverse)

if(RECONVERSE_SOURCE_DIR)
  # An out-of-tree reconverse (--with-reconverse-dir). Used for a working copy
  # kept outside the charm tree; the submodule is left alone.
  get_filename_component(_reconverse_src "${RECONVERSE_SOURCE_DIR}" ABSOLUTE)
  if(NOT EXISTS ${_reconverse_src}/CMakeLists.txt)
    message(FATAL_ERROR
        "RECONVERSE_SOURCE_DIR=${_reconverse_src} is not a reconverse source "
        "tree (no CMakeLists.txt in it).")
  endif()
  message(STATUS "Using reconverse from ${_reconverse_src}")
else()
  # The submodule. A fresh `git clone` without --recurse-submodules leaves it
  # empty, which is the common first-build failure, so initialize it here
  # rather than failing with a directory that exists but is empty.
  if(NOT EXISTS ${_reconverse_submodule}/CMakeLists.txt)
    find_package(Git QUIET)
    if(GIT_FOUND AND EXISTS ${CMAKE_SOURCE_DIR}/.git)
      message(STATUS "Initializing the reconverse submodule")
      execute_process(
          COMMAND ${GIT_EXECUTABLE} submodule update --init --recursive
                  -- contrib/reconverse
          WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
          RESULT_VARIABLE _submodule_result)
      if(NOT _submodule_result EQUAL 0)
        message(FATAL_ERROR
            "git submodule update --init -- contrib/reconverse failed "
            "(${_submodule_result}). Run it by hand in ${CMAKE_SOURCE_DIR} "
            "and rebuild.")
      endif()
    endif()
  endif()
  if(NOT EXISTS ${_reconverse_submodule}/CMakeLists.txt)
    # Not a git checkout (a release tarball, say), so there is no submodule to
    # initialize and no pin to honor. Point at a reconverse tree explicitly.
    message(FATAL_ERROR
        "contrib/reconverse is empty and cannot be initialized from git. "
        "Clone charm with --recurse-submodules, run `git submodule update "
        "--init -- contrib/reconverse`, or build with "
        "--with-reconverse-dir=<path to a reconverse source tree>.")
  endif()
  set(_reconverse_src ${_reconverse_submodule})
endif()

# Set cmake variables for reconverse
set(_save_BUILD_SHARED_LIBS ${BUILD_SHARED_LIBS})
if(CMK_COMPILER STREQUAL "msvc")
  # A static library on Windows: an MSVC DLL exports nothing without
  # __declspec(dllexport) annotations, and its thread-local variables (every
  # Cpv) cannot be exported from one at all.
  set(BUILD_SHARED_LIBS OFF CACHE INTERNAL "")
  # Reconverse is added before Charm++ switches its rules to charmc, so its
  # archive would otherwise be written by Cygwin's ar; use LIB.EXE, as
  # charmc does through the same wrapper.
  set(CMAKE_CXX_CREATE_STATIC_LIBRARY
      "${CMAKE_SOURCE_DIR}/src/arch/win/unix2nt_ar <TARGET> <OBJECTS>")
  set(CMAKE_C_CREATE_STATIC_LIBRARY ${CMAKE_CXX_CREATE_STATIC_LIBRARY})
else()
  set(BUILD_SHARED_LIBS ON CACHE INTERNAL "")
endif()

# Reconverse's build directory lives under charm's, not in the source tree, so
# an out-of-tree reconverse (and the submodule itself) stays clean.
add_subdirectory(${_reconverse_src} ${CMAKE_BINARY_DIR}/reconverse)

set(BUILD_SHARED_LIBS ${_save_BUILD_SHARED_LIBS} CACHE INTERNAL "")

# Charm4py's extension registers CCS handlers through CcsRegisterHandlerExt,
# which libreconverse compiles only when CMK_CHARM4PY is defined; reconverse
# is its own project and does not see Charm++'s configuration macros.
if("${TARGET}" STREQUAL "charm4py")
  target_compile_definitions(reconverse PRIVATE CMK_CHARM4PY=1)
endif()

configure_file(${_reconverse_src}/include/converse.h ${CMAKE_BINARY_DIR}/include/ COPYONLY)
configure_file(${_reconverse_src}/src/cldb.h ${CMAKE_BINARY_DIR}/include/ COPYONLY)
configure_file(${_reconverse_src}/include/charm-config.h ${CMAKE_BINARY_DIR}/include/ COPYONLY)
# Charm++ must build against the same RDMA configuration that libreconverse was
# compiled with (CMK_REG_REQUIRED, CMK_NOCOPY_DIRECT_BYTES). The legacy
# src/conv-core/conv-rdma.h is excluded from the Converse header copy in
# cmake/converse.cmake so that this is the only conv-rdma.h on the include path.
configure_file(${_reconverse_src}/include/conv-rdma.h ${CMAKE_BINARY_DIR}/include/ COPYONLY)
# reconverse's converse.h includes persistent.h, so this must be the copy on
# the include path. The legacy src/conv-core/persistent.h is excluded from the
# Converse header copy in cmake/converse.cmake, and its machine-layer
# implementation (src/arch/util/persist-comm.C) is dropped from the build
# there as well.
configure_file(${_reconverse_src}/include/persistent.h ${CMAKE_BINARY_DIR}/include/ COPYONLY)
# Likewise taskqueue.h, the other header reconverse's converse.h includes. Note
# that this copy does not include converse.h itself, so anything including it
# directly has to include converse.h first.
configure_file(${_reconverse_src}/include/taskqueue.h ${CMAKE_BINARY_DIR}/include/ COPYONLY)
# CCS lives in libreconverse: the server-side API (conv-ccs.h and what it
# includes) and the client-side header for programs linking ccs-client.
# cmake/converse.cmake keeps the classic copies out of the include tree.
foreach(_h conv-ccs.h ccs-server.h ccs-auth.h sockRoutines.h ccs-client.h)
  configure_file(${_reconverse_src}/include/${_h} ${CMAKE_BINARY_DIR}/include/ COPYONLY)
endforeach()
