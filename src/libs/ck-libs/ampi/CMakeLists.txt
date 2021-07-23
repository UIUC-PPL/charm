set(ampi-cxx-sources ampi.C ampiMisc.C ampiOneSided.C ampif.C ddt.C mpich-alltoall.C ampi_mpix.C ampi_noimpl.C)

set(ampi-f90-sources ampifimpl.f90 ampimod.f90)

set(ampi-h-sources ampi.h ampi-interoperate.h ampif.h ampiimpl.h ddt.h ampi_functions.h ampi_funcptr.h ampi_funcptr_loader.h
                   romio-stub/mpio_functions.h romio-stub/mpio.h romio-stub/mpio_globals.h romio-stub/mpiof.h)

add_library(ampi-compat compat_ampi.c
       compat_ampim.C compat_ampifm.C compat_ampicm.C
       compat_ampicpp.C compat_ampins.C compat_ampirs.C)


# fsglobals/pipglobals

add_library(ampi_funcptr_shim ampi_funcptr_shim.C)
target_compile_options(ampi_funcptr_shim PRIVATE -language ampi -DAMPI_USE_FUNCPTR -DAMPI_NO_UNIMPLEMENTED_WARNINGS -fPIE -shared)
add_dependencies(ampi_funcptr_shim moduleampi)
add_custom_command(TARGET ampi_funcptr_shim
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/ampi_funcptr_shim.dir/ampi_funcptr_shim.C.o ${CMAKE_BINARY_DIR}/lib/ampi_funcptr_shim.o
    VERBATIM
)

add_library(ampi_funcptr_shim_main ampi_funcptr_shim_main.C)
target_compile_options(ampi_funcptr_shim_main PRIVATE -language ampi -fPIE)
add_dependencies(ampi_funcptr_shim_main moduleampi)
add_custom_command(TARGET ampi_funcptr_shim_main
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/ampi_funcptr_shim_main.dir/ampi_funcptr_shim_main.C.o ${CMAKE_BINARY_DIR}/lib/ampi_funcptr_shim_main.o
    VERBATIM
)

add_library(ampi_funcptr_loader ampi_funcptr_loader.C)
target_compile_options(ampi_funcptr_loader PRIVATE -language ampi -DAMPI_NO_UNIMPLEMENTED_WARNINGS)
add_dependencies(ampi_funcptr_loader moduleampi)
add_custom_command(TARGET ampi_funcptr_loader
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/ampi_funcptr_loader.dir/ampi_funcptr_loader.C.o ${CMAKE_BINARY_DIR}/lib/ampi_funcptr_loader.o
    VERBATIM
)

add_library(ampi_funcptr_loader_stub ampi_funcptr_loader_stub.C)
target_compile_options(ampi_funcptr_loader_stub PRIVATE -language ampi)
add_dependencies(ampi_funcptr_loader_stub moduleampi)
add_custom_command(TARGET ampi_funcptr_loader_stub
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/ampi_funcptr_loader_stub.dir/ampi_funcptr_loader_stub.C.o ${CMAKE_BINARY_DIR}/lib/ampi_funcptr_loader_stub.o
    VERBATIM
)

if(CMK_CAN_LINK_FORTRAN)
    add_library(ampi_funcptr_shim_fortran ampif.C)
    target_compile_options(ampi_funcptr_shim_fortran PRIVATE -language ampi -DAMPI_USE_FUNCPTR -fPIE -shared)
    add_dependencies(ampi_funcptr_shim_fortran moduleampi moduleampif)
    add_custom_command(TARGET ampi_funcptr_shim_fortran
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/ampi_funcptr_shim_fortran.dir/ampif.C.o ${CMAKE_BINARY_DIR}/lib/ampi_funcptr_shim_fortran.o
        VERBATIM
    )

    add_library(ampi_funcptr_shim_fortran_impl ampifimpl.f90)
    target_compile_options(ampi_funcptr_shim_fortran_impl PRIVATE -language ampi -fPIE -shared)
    add_dependencies(ampi_funcptr_shim_fortran_impl moduleampi moduleampif)
    add_custom_command(TARGET ampi_funcptr_shim_fortran_impl
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/ampi_funcptr_shim_fortran_impl.dir/ampifimpl.f90.o ${CMAKE_BINARY_DIR}/lib/ampi_funcptr_shim_fortran_impl.o
        VERBATIM
    )

    add_library(ampi_funcptr_shim_fortran_mod ampimod.f90)
    target_compile_options(ampi_funcptr_shim_fortran_mod PRIVATE -language ampi -fPIE -shared)
    add_dependencies(ampi_funcptr_shim_fortran_mod moduleampi moduleampif)
    add_custom_command(TARGET ampi_funcptr_shim_fortran_mod
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/ampi_funcptr_shim_fortran_mod.dir/ampimod.f90.o ${CMAKE_BINARY_DIR}/lib/ampi_funcptr_shim_fortran_mod.o
        VERBATIM
    )

    add_library(ampi_funcptr_shim_fmpimain ampi_funcptr_shim_fmpimain.C)
    target_compile_options(ampi_funcptr_shim_fmpimain PRIVATE -language ampi -fPIE)
    add_dependencies(ampi_funcptr_shim_fmpimain moduleampi moduleampif)
    add_custom_command(TARGET ampi_funcptr_shim_fmpimain
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/ampi_funcptr_shim_fmpimain.dir/ampi_funcptr_shim_fmpimain.C.o ${CMAKE_BINARY_DIR}/lib/ampi_funcptr_shim_fmpimain.o
        VERBATIM
    )
endif()

if(CMK_SUPPORTS_FSGLOBALS)
    add_library(ampi_globals_fs ampi_globals_fs.C)
    target_compile_options(ampi_globals_fs PRIVATE -language ampi)
    add_dependencies(ampi_globals_fs moduleampi)
    add_custom_command(TARGET ampi_globals_fs
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/ampi_globals_fs.dir/ampi_globals_fs.C.o ${CMAKE_BINARY_DIR}/lib/ampi_globals_fs.o
        VERBATIM
    )
endif()

if(CMK_SUPPORTS_PIPGLOBALS)
    add_library(ampi_globals_pip ampi_globals_pip.C)
    target_compile_options(ampi_globals_pip PRIVATE -language ampi)
    add_dependencies(ampi_globals_pip moduleampi)
    add_custom_command(TARGET ampi_globals_pip
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/ampi_globals_pip.dir/ampi_globals_pip.C.o ${CMAKE_BINARY_DIR}/lib/ampi_globals_pip.o
        VERBATIM
    )

    add_library(ampi_globals_pie ampi_globals_pie.C)
    target_compile_options(ampi_globals_pie PRIVATE -language ampi)
    add_dependencies(ampi_globals_pie moduleampi)
    add_custom_command(TARGET ampi_globals_pie
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/ampi_globals_pie.dir/ampi_globals_pie.C.o ${CMAKE_BINARY_DIR}/lib/ampi_globals_pie.o
        VERBATIM
    )
endif()


configure_file(ampiCC        ${CMAKE_BINARY_DIR}/bin/ampicc         @ONLY)
configure_file(ampiCC        ${CMAKE_BINARY_DIR}/bin/               @ONLY)
configure_file(ampiCC        ${CMAKE_BINARY_DIR}/bin/ampicxx        @ONLY)
configure_file(ampiCC        ${CMAKE_BINARY_DIR}/bin/ampic++        @ONLY)
configure_file(ampif77       ${CMAKE_BINARY_DIR}/bin/               COPYONLY)
configure_file(ampif90       ${CMAKE_BINARY_DIR}/bin/               COPYONLY)
configure_file(ampif90       ${CMAKE_BINARY_DIR}/bin/ampifort       COPYONLY)
configure_file(ampirun       ${CMAKE_BINARY_DIR}/bin/               COPYONLY)
configure_file(ampirun       ${CMAKE_BINARY_DIR}/bin/ampiexec       COPYONLY)
configure_file(ampiCC        ${CMAKE_BINARY_DIR}/bin/mpicc.ampi     @ONLY)
configure_file(ampiCC        ${CMAKE_BINARY_DIR}/bin/mpiCC.ampi     @ONLY)
configure_file(ampiCC        ${CMAKE_BINARY_DIR}/bin/mpicxx.ampi    @ONLY)
configure_file(ampiCC        ${CMAKE_BINARY_DIR}/bin/mpic++.ampi    @ONLY)
configure_file(ampif77       ${CMAKE_BINARY_DIR}/bin/mpif77.ampi    COPYONLY)
configure_file(ampif90       ${CMAKE_BINARY_DIR}/bin/mpif90.ampi    COPYONLY)
configure_file(ampif90       ${CMAKE_BINARY_DIR}/bin/mpifort.ampi   COPYONLY)
configure_file(ampirun       ${CMAKE_BINARY_DIR}/bin/mpirun.ampi    COPYONLY)
configure_file(ampirun       ${CMAKE_BINARY_DIR}/bin/mpiexec.ampi   COPYONLY)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/bin/ampi/)
configure_file(pathstub.sh   ${CMAKE_BINARY_DIR}/bin/ampi/mpicc     COPYONLY)
configure_file(pathstub.sh   ${CMAKE_BINARY_DIR}/bin/ampi/mpiCC     COPYONLY)
configure_file(pathstub.sh   ${CMAKE_BINARY_DIR}/bin/ampi/mpicxx    COPYONLY)
configure_file(pathstub.sh   ${CMAKE_BINARY_DIR}/bin/ampi/mpic++    COPYONLY)
configure_file(pathstub.sh   ${CMAKE_BINARY_DIR}/bin/ampi/mpif77    COPYONLY)
configure_file(pathstub.sh   ${CMAKE_BINARY_DIR}/bin/ampi/mpif90    COPYONLY)
configure_file(pathstub.sh   ${CMAKE_BINARY_DIR}/bin/ampi/mpifort   COPYONLY)
configure_file(pathstub.sh   ${CMAKE_BINARY_DIR}/bin/ampi/mpirun    COPYONLY)
configure_file(pathstub.sh   ${CMAKE_BINARY_DIR}/bin/ampi/mpiexec   COPYONLY)

configure_file(nm_globals.sh ${CMAKE_BINARY_DIR}/bin/               COPYONLY)

foreach(file ${ampi-h-sources})
  if(${file} STREQUAL "ampi.h")
    file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/include/ampi/)
    configure_file(${file} ${CMAKE_BINARY_DIR}/include/ampi/mpi.h COPYONLY)
    configure_file(${file} ${CMAKE_BINARY_DIR}/include/ampi.h COPYONLY)
  elseif(${file} STREQUAL "ampif.h")
    configure_file(${file} ${CMAKE_BINARY_DIR}/include/mpif.h COPYONLY)
    configure_file(${file} ${CMAKE_BINARY_DIR}/include/ampif.h COPYONLY)
  else()
    configure_file(${file} ${CMAKE_BINARY_DIR}/include COPYONLY)
  endif()
endforeach(file)

configure_file(libmoduleampi.dep  ${CMAKE_BINARY_DIR}/lib COPYONLY)
configure_file(libmoduleampif.dep ${CMAKE_BINARY_DIR}/lib COPYONLY)


# ROMIO
set(romio-objects)
set(romio-fortran-objects)
if(CMK_AMPI_WITH_ROMIO)
    set(romio_dir ${CMAKE_BINARY_DIR}/src/libs/ck-libs/ampi/romio-prefix/src/romio)

    if(CMK_WINDOWS)
        set(obj_suf ".obj")
    else()
        set(obj_suf ".o")
    endif()

    set(romio-objects
        ${romio_dir}/adio/ad_nfs/ad_nfs${obj_suf}
        ${romio_dir}/adio/ad_nfs/ad_nfs_done${obj_suf}
        ${romio_dir}/adio/ad_nfs/ad_nfs_fcntl${obj_suf}
        ${romio_dir}/adio/ad_nfs/ad_nfs_features${obj_suf}
        ${romio_dir}/adio/ad_nfs/ad_nfs_getsh${obj_suf}
        ${romio_dir}/adio/ad_nfs/ad_nfs_iread${obj_suf}
        ${romio_dir}/adio/ad_nfs/ad_nfs_iwrite${obj_suf}
        ${romio_dir}/adio/ad_nfs/ad_nfs_open${obj_suf}
        ${romio_dir}/adio/ad_nfs/ad_nfs_read${obj_suf}
        ${romio_dir}/adio/ad_nfs/ad_nfs_resize${obj_suf}
        ${romio_dir}/adio/ad_nfs/ad_nfs_setsh${obj_suf}
        ${romio_dir}/adio/ad_nfs/ad_nfs_wait${obj_suf}
        ${romio_dir}/adio/ad_nfs/ad_nfs_write${obj_suf}
        ${romio_dir}/adio/ad_testfs/ad_testfs${obj_suf}
        ${romio_dir}/adio/ad_testfs/ad_testfs_close${obj_suf}
        ${romio_dir}/adio/ad_testfs/ad_testfs_delete${obj_suf}
        ${romio_dir}/adio/ad_testfs/ad_testfs_done${obj_suf}
        ${romio_dir}/adio/ad_testfs/ad_testfs_fcntl${obj_suf}
        ${romio_dir}/adio/ad_testfs/ad_testfs_flush${obj_suf}
        ${romio_dir}/adio/ad_testfs/ad_testfs_hints${obj_suf}
        ${romio_dir}/adio/ad_testfs/ad_testfs_iread${obj_suf}
        ${romio_dir}/adio/ad_testfs/ad_testfs_iwrite${obj_suf}
        ${romio_dir}/adio/ad_testfs/ad_testfs_open${obj_suf}
        ${romio_dir}/adio/ad_testfs/ad_testfs_rdcoll${obj_suf}
        ${romio_dir}/adio/ad_testfs/ad_testfs_read${obj_suf}
        ${romio_dir}/adio/ad_testfs/ad_testfs_resize${obj_suf}
        ${romio_dir}/adio/ad_testfs/ad_testfs_seek${obj_suf}
        ${romio_dir}/adio/ad_testfs/ad_testfs_wait${obj_suf}
        ${romio_dir}/adio/ad_testfs/ad_testfs_wrcoll${obj_suf}
        ${romio_dir}/adio/ad_testfs/ad_testfs_write${obj_suf}
        ${romio_dir}/adio/ad_ufs/ad_ufs${obj_suf}
        ${romio_dir}/adio/ad_ufs/ad_ufs_open${obj_suf}
        ${romio_dir}/adio/common/ad_aggregate${obj_suf}
        ${romio_dir}/adio/common/ad_aggregate_new${obj_suf}
        ${romio_dir}/adio/common/ad_close${obj_suf}
        ${romio_dir}/adio/common/ad_coll_build_req_new${obj_suf}
        ${romio_dir}/adio/common/ad_coll_exch_new${obj_suf}
        ${romio_dir}/adio/common/ad_darray${obj_suf}
        ${romio_dir}/adio/common/ad_delete${obj_suf}
        ${romio_dir}/adio/common/ad_done${obj_suf}
        ${romio_dir}/adio/common/ad_done_fake${obj_suf}
        ${romio_dir}/adio/common/ad_end${obj_suf}
        ${romio_dir}/adio/common/ad_fcntl${obj_suf}
        ${romio_dir}/adio/common/ad_features${obj_suf}
        ${romio_dir}/adio/common/ad_flush${obj_suf}
        ${romio_dir}/adio/common/ad_fstype${obj_suf}
        ${romio_dir}/adio/common/ad_get_sh_fp${obj_suf}
        ${romio_dir}/adio/common/ad_hints${obj_suf}
        ${romio_dir}/adio/common/ad_init${obj_suf}
        ${romio_dir}/adio/common/ad_io_coll${obj_suf}
        ${romio_dir}/adio/common/ad_iopen${obj_suf}
        ${romio_dir}/adio/common/ad_iread${obj_suf}
        ${romio_dir}/adio/common/ad_iread_coll${obj_suf}
        ${romio_dir}/adio/common/ad_iread_fake${obj_suf}
        ${romio_dir}/adio/common/ad_iwrite${obj_suf}
        ${romio_dir}/adio/common/ad_iwrite_coll${obj_suf}
        ${romio_dir}/adio/common/ad_iwrite_fake${obj_suf}
        ${romio_dir}/adio/common/ad_open${obj_suf}
        ${romio_dir}/adio/common/ad_opencoll${obj_suf}
        ${romio_dir}/adio/common/ad_opencoll_failsafe${obj_suf}
        ${romio_dir}/adio/common/ad_opencoll_scalable${obj_suf}
        ${romio_dir}/adio/common/ad_prealloc${obj_suf}
        ${romio_dir}/adio/common/ad_read${obj_suf}
        ${romio_dir}/adio/common/ad_read_coll${obj_suf}
        ${romio_dir}/adio/common/ad_read_str${obj_suf}
        ${romio_dir}/adio/common/ad_read_str_naive${obj_suf}
        ${romio_dir}/adio/common/ad_resize${obj_suf}
        ${romio_dir}/adio/common/ad_seek${obj_suf}
        ${romio_dir}/adio/common/ad_set_sh_fp${obj_suf}
        ${romio_dir}/adio/common/ad_set_view${obj_suf}
        ${romio_dir}/adio/common/ad_subarray${obj_suf}
        ${romio_dir}/adio/common/ad_threaded_io${obj_suf}
        ${romio_dir}/adio/common/ad_wait${obj_suf}
        ${romio_dir}/adio/common/ad_wait_fake${obj_suf}
        ${romio_dir}/adio/common/ad_write${obj_suf}
        ${romio_dir}/adio/common/ad_write_coll${obj_suf}
        ${romio_dir}/adio/common/ad_write_nolock${obj_suf}
        ${romio_dir}/adio/common/ad_write_str${obj_suf}
        ${romio_dir}/adio/common/ad_write_str_naive${obj_suf}
        ${romio_dir}/adio/common/adi_close${obj_suf}
        ${romio_dir}/adio/common/byte_offset${obj_suf}
        ${romio_dir}/adio/common/cb_config_list${obj_suf}
        ${romio_dir}/adio/common/eof_offset${obj_suf}
        ${romio_dir}/adio/common/error${obj_suf}
        ${romio_dir}/adio/common/flatten${obj_suf}
        ${romio_dir}/adio/common/get_fp_posn${obj_suf}
        ${romio_dir}/adio/common/greq_fns${obj_suf}
        ${romio_dir}/adio/common/heap-sort${obj_suf}
        ${romio_dir}/adio/common/hint_fns${obj_suf}
        ${romio_dir}/adio/common/iscontig${obj_suf}
        ${romio_dir}/adio/common/lock${obj_suf}
        ${romio_dir}/adio/common/malloc${obj_suf}
        ${romio_dir}/adio/common/onesided_aggregation${obj_suf}
        ${romio_dir}/adio/common/p2p_aggregation${obj_suf}
        ${romio_dir}/adio/common/shfp_fname${obj_suf}
        ${romio_dir}/adio/common/status_setb${obj_suf}
        ${romio_dir}/adio/common/strfns${obj_suf}
        ${romio_dir}/adio/common/system_hints${obj_suf}
        ${romio_dir}/adio/common/utils${obj_suf}
        ${romio_dir}/mpi-io/close${obj_suf}
        ${romio_dir}/mpi-io/delete${obj_suf}
        ${romio_dir}/mpi-io/file_c2f${obj_suf}
        ${romio_dir}/mpi-io/file_f2c${obj_suf}
        ${romio_dir}/mpi-io/fsync${obj_suf}
        ${romio_dir}/mpi-io/get_amode${obj_suf}
        ${romio_dir}/mpi-io/get_atom${obj_suf}
        ${romio_dir}/mpi-io/get_bytoff${obj_suf}
        ${romio_dir}/mpi-io/get_errh${obj_suf}
        ${romio_dir}/mpi-io/get_extent${obj_suf}
        ${romio_dir}/mpi-io/get_group${obj_suf}
        ${romio_dir}/mpi-io/get_info${obj_suf}
        ${romio_dir}/mpi-io/get_posn${obj_suf}
        ${romio_dir}/mpi-io/get_posn_sh${obj_suf}
        ${romio_dir}/mpi-io/get_size${obj_suf}
        ${romio_dir}/mpi-io/get_view${obj_suf}
        ${romio_dir}/mpi-io/glue/default/mpio_err${obj_suf}
        ${romio_dir}/mpi-io/glue/default/mpio_file${obj_suf}
        ${romio_dir}/mpi-io/glue/large_count${obj_suf}
        ${romio_dir}/mpi-io/ioreq_c2f${obj_suf}
        ${romio_dir}/mpi-io/ioreq_f2c${obj_suf}
        ${romio_dir}/mpi-io/iread${obj_suf}
        ${romio_dir}/mpi-io/iread_all${obj_suf}
        ${romio_dir}/mpi-io/iread_at${obj_suf}
        ${romio_dir}/mpi-io/iread_atall${obj_suf}
        ${romio_dir}/mpi-io/iread_sh${obj_suf}
        ${romio_dir}/mpi-io/iwrite${obj_suf}
        ${romio_dir}/mpi-io/iwrite_all${obj_suf}
        ${romio_dir}/mpi-io/iwrite_at${obj_suf}
        ${romio_dir}/mpi-io/iwrite_atall${obj_suf}
        ${romio_dir}/mpi-io/iwrite_sh${obj_suf}
        ${romio_dir}/mpi-io/mpich_fileutil${obj_suf}
        ${romio_dir}/mpi-io/mpir-mpioinit${obj_suf}
        ${romio_dir}/mpi-io/mpiu_external32${obj_suf}
        ${romio_dir}/mpi-io/mpiu_greq${obj_suf}
        ${romio_dir}/mpi-io/open${obj_suf}
        ${romio_dir}/mpi-io/prealloc${obj_suf}
        ${romio_dir}/mpi-io/rd_atallb${obj_suf}
        ${romio_dir}/mpi-io/rd_atalle${obj_suf}
        ${romio_dir}/mpi-io/read${obj_suf}
        ${romio_dir}/mpi-io/read_all${obj_suf}
        ${romio_dir}/mpi-io/read_allb${obj_suf}
        ${romio_dir}/mpi-io/read_alle${obj_suf}
        ${romio_dir}/mpi-io/read_at${obj_suf}
        ${romio_dir}/mpi-io/read_atall${obj_suf}
        ${romio_dir}/mpi-io/read_ord${obj_suf}
        ${romio_dir}/mpi-io/read_ordb${obj_suf}
        ${romio_dir}/mpi-io/read_orde${obj_suf}
        ${romio_dir}/mpi-io/read_sh${obj_suf}
        ${romio_dir}/mpi-io/register_datarep${obj_suf}
        ${romio_dir}/mpi-io/seek${obj_suf}
        ${romio_dir}/mpi-io/seek_sh${obj_suf}
        ${romio_dir}/mpi-io/set_atom${obj_suf}
        ${romio_dir}/mpi-io/set_errh${obj_suf}
        ${romio_dir}/mpi-io/set_info${obj_suf}
        ${romio_dir}/mpi-io/set_size${obj_suf}
        ${romio_dir}/mpi-io/set_view${obj_suf}
        ${romio_dir}/mpi-io/wr_atallb${obj_suf}
        ${romio_dir}/mpi-io/wr_atalle${obj_suf}
        ${romio_dir}/mpi-io/write${obj_suf}
        ${romio_dir}/mpi-io/write_all${obj_suf}
        ${romio_dir}/mpi-io/write_allb${obj_suf}
        ${romio_dir}/mpi-io/write_alle${obj_suf}
        ${romio_dir}/mpi-io/write_at${obj_suf}
        ${romio_dir}/mpi-io/write_atall${obj_suf}
        ${romio_dir}/mpi-io/write_ord${obj_suf}
        ${romio_dir}/mpi-io/write_ordb${obj_suf}
        ${romio_dir}/mpi-io/write_orde${obj_suf}
        ${romio_dir}/mpi-io/write_sh${obj_suf}
    )

    if(CMK_CAN_LINK_FORTRAN)
        set(romio_f77 yes)

        set(romio-fortran-objects
            ${romio_dir}/mpi-io/fortran/romio_fortran${obj_suf}
        )
    else()
        set(romio_f77 no)
    endif()

    set_source_files_properties(
        ${romio-objects}
        ${romio-fortran-objects}
        PROPERTIES
        EXTERNAL_OBJECT true
        GENERATED true
    )

    # Compile ROMIO with the same flags as AMPI
    get_target_property(CK_CFLAGS ck COMPILE_OPTIONS)
    string(REPLACE ";" " " CK_CFLAGS "${CK_CFLAGS}")
    set(ROMIO_FLAGS "${CK_CFLAGS} -DAMPI_NO_UNIMPLEMENTED_WARNINGS -standalone -fPIC")

    include(ExternalProject)
    ExternalProject_Add(romio
        URL ${CMAKE_CURRENT_SOURCE_DIR}/romio
        CONFIGURE_COMMAND cd ${romio_dir} &&
            export MPI_IMPL=ampi &&
            export FROM_MPICH=no &&
            export FROM_MPICH2=no &&
            export FROM_LAM=no &&
            export F77=${CMAKE_BINARY_DIR}/bin/ampif90 &&
            export CC=${CMAKE_BINARY_DIR}/bin/ampicc &&
            ./configure
            --disable-aio
            --enable-f77=${romio_f77}
            --enable-static=yes
            --enable-shared=no
            --disable-weak-symbols
            CFLAGS=${ROMIO_FLAGS}
            FFLAGS=${ROMIO_FLAGS}
            > /dev/null
        COMMAND cp -f ${romio_dir}/include/mpio.h ${CMAKE_BINARY_DIR}/include/
        COMMAND cp -f ${romio_dir}/include/mpiof.h ${CMAKE_BINARY_DIR}/include/
        COMMAND cp -f ${romio_dir}/include/mpio_functions.h ${CMAKE_BINARY_DIR}/include/
        BUILD_COMMAND $(MAKE) -C ${romio_dir} AUTOCONF=: AUTOHEADER=: AUTOMAKE=: ACLOCAL=: V=$(VERBOSE)
        INSTALL_COMMAND ""
        LIST_SEPARATOR ^^
    )

    if(CMK_CAN_LINK_FORTRAN)
        add_custom_command(TARGET romio
            POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy ${romio_dir}/mpi-io/fortran/romio_fortran${obj_suf} ${CMAKE_BINARY_DIR}/lib/ampi_funcptr_shim_fortran_romio.o
            VERBATIM
        )
    endif()

    # Since we link a program with charmc as part of ROMIO's configure,
    # we need all libraries built at this point.
    add_dependencies(romio ck modulecompletion
        converse conv-static ldb-rand
        memory-os-isomalloc memory-default threads-default ckmain
        ckqt moduleNDMeshStreamer ckhello)

    add_dependencies(ampi_funcptr_shim romio)
    add_dependencies(ampi_funcptr_shim_main romio)
    add_dependencies(ampi_funcptr_loader romio)
    add_dependencies(ampi_funcptr_loader_stub romio)
    if(CMK_CAN_LINK_FORTRAN)
        add_dependencies(ampi_funcptr_shim_fortran romio)
        add_dependencies(ampi_funcptr_shim_fmpimain romio)
    endif()
    if(CMK_SUPPORTS_FSGLOBALS)
        add_dependencies(ampi_globals_fs romio)
    endif()
    if(CMK_SUPPORTS_PIPGLOBALS)
        add_dependencies(ampi_globals_pip romio)
        add_dependencies(ampi_globals_pie romio)
    endif()
endif()


add_library(moduleampi
    ${ampi-cxx-sources}
    ${ampi-h-sources}
    ${romio-objects}
)
target_compile_options(moduleampi PRIVATE -DAMPI_NO_UNIMPLEMENTED_WARNINGS -I${CMAKE_BINARY_DIR}/include/ampi/)
add_dependencies(moduleampi moduletcharm ck)


if(CMK_CAN_LINK_FORTRAN)
    add_library(moduleampif
        ${ampi-f90-sources}
        ${ampi-cxx-sources}
        ${ampi-h-sources}
        ${romio-objects}
        ${romio-fortran-objects}
    )
    target_compile_options(moduleampif PRIVATE -DAMPI_NO_UNIMPLEMENTED_WARNINGS -I${CMAKE_BINARY_DIR}/include/ampi/)
    add_dependencies(moduleampif moduletcharm ck)

    add_library(ampif OBJECT ampif.C)
    target_compile_options(ampif PRIVATE -I${CMAKE_BINARY_DIR}/include/ampi/)
    add_library(ampifimpl OBJECT ampifimpl.f90)
    target_compile_options(ampifimpl PRIVATE -I${CMAKE_BINARY_DIR}/include/ampi/)
    add_library(ampimod OBJECT ampimod.f90)
    target_compile_options(ampimod PRIVATE -I${CMAKE_BINARY_DIR}/include/ampi/)
    add_dependencies(ampif ck)
    add_dependencies(ampifimpl ck)
    add_dependencies(ampimod ck)
    add_dependencies(moduleampif ampif ampifimpl ampimod)
    add_custom_command(TARGET moduleampif
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/ampif.dir/ampif.C.o ${CMAKE_BINARY_DIR}/lib/ampif.o
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/ampifimpl.dir/ampifimpl.f90.o ${CMAKE_BINARY_DIR}/lib/ampifimpl.o
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/ampimod.dir/ampimod.f90.o ${CMAKE_BINARY_DIR}/lib/ampimod.o
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/mpi.mod ${CMAKE_BINARY_DIR}/include/
        VERBATIM
    )
endif()


if(CMK_AMPI_WITH_ROMIO)
    add_dependencies(moduleampi romio)
    if(CMK_CAN_LINK_FORTRAN)
        add_dependencies(moduleampif romio)
    endif()
endif()
