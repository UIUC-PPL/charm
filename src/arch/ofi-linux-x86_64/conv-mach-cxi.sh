

# For libfabric If the user doesn't pass --basedir, use pkg-config for
#libfabric headers and library to avoid some linker wackiness, we
#order them: pal libs, PMI libs, lib64.  So that if someplace (i.e.,
#NCSA) puts regular pmi libs in /usr/lib64, we get them from the
#package's cray-pmi dir not their unextended pmi.  libpals comes along
#for the ride here due to a dependency in pmi.  fabric can just go
#after the others.


if test -z "$USER_OPTS_LD"
then
    module load cray-libpals cray-pmi libfabric
    CMK_LIBFABRIC_INC=`pkg-config --cflags libfabric`
    CMK_LIBFABRIC_LIBS=`pkg-config --libs libfabric`
    CMK_LIBPALS_LIBS=`pkg-config --libs libpals`
    CMK_LIBPALS_LDPATH=`pkg-config libpals --variable=libdir`
    CMK_PMI_INC=`pkg-config --cflags cray-pmi`
    CMK_PMI_LIBS=`pkg-config --libs cray-pmi`
    CMK_LIBPMI_LDPATH=`pkg-config cray-pmi --variable=libdir`
    CMK_INCDIR="$CMK_PMI_INC -I/usr/include/slurm/ $CMK_LIBFABRIC_INC $CMK_INCDIR "
    CMK_LIBS="-Wl,-rpath,$CMK_LIBPALS_LDPATH,-rpath,$CMK_LIBPMI_LDPATH $CMK_LIBPALS_LIBS $CMK_PMI_LIBS -L/usr/lib64/ $CMK_LIBFABRIC_LIBS $CMK_LIBS "
fi

# For runtime
CMK_INCDIR="$CMK_INCDIR -I./proc_management/"
