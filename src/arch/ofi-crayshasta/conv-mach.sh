CMK_BUILD_CRAY=1
CMK_CRAY_NOGNI=1
. $CHARMINC/conv-mach-craype.sh

# For libfabric
#If the user doesn't pass --basedir, use defaults for libfabric headers and library
if test -z "$USER_OPTS_LD"
then
    if test -z $"CMK_LIBFABRIC_INC"
    then
	CMK_LIBFABRIC_INC=`pkg-config --cflags libfabric`
	CMK_LIBFABRIC_LIBS=`pkg-config --libs libfabric`
	CMK_LIBPALS_LIBS=`pkg-config --libs libpals`
	CMK_LIBPALS_LDPATH=`pkg-config libpals --variable=libdir`
    fi
fi

# Use PMI2 by default on Cray systems with cray-pmi
. $CHARMINC/conv-mach-slurmpmi2cray.sh

CMK_INCDIR="$CMK_PMI_INC -I/usr/include/slurm/ $CMK_LIBFABRIC_INC $CMK_INCDIR "
CMK_LIBS="-Wl,-rpath,$CMK_LIBPALS_LDPATH,-rpath,$CMK_LIBPMI_LDPATH $CMK_LIBPALS_LIBS $CMK_PMI_LIBS -L/usr/lib64/ $CMK_LIBFABRIC_LIBS $CMK_LIBS "

# For runtime
CMK_INCDIR="$CMK_INCDIR -I./proc_management/"
