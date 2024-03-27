CMK_LIBFABRIC_INC=`pkg-config --cflags libfabric`
CMK_LIBFABRIC_LIBS=`pkg-config --libs libfabric`
CMK_PMI_INC=`pkg-config --cflags cray-pmi`
CMK_PMI_LIBS=`pkg-config --libs cray-pmi`
CMK_INCDIR="$CMK_INCDIR $CMK_PMI_INC -I/usr/include/slurm/ $CMK_LIBFABRIC_INC"
CMK_LIBS="$CMK_LIBS $CMK_PMI_LIBS $CMK_LIBFABRIC_LIBS"
