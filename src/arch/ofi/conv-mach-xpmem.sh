CMK_XPMEM_INC=`pkg-config --cflags cray-xpmem`
CMK_XPMEM_LIBS=`pkg-config --libs cray-xpmem`
CMK_INCDIR="$CMK_INCDIR $CMK_XPMEM_INC"
CMK_LIBS="$CMK_LIBS $CMK_XPMEM_LIBS -lrt -lpthread"
