. $CHARMINC/cc-gcc.sh

# For libfabric
#If the user doesn't pass --basedir, use defaults for libfabric headers and library
#if test -z "$USER_OPTS_LD"
#then
#    CMK_INCDIR="-I/usr/include/"
#    CMK_LIBDIR="-L/usr/lib64/"
#fi

#CMK_LIBS="$CMK_LIBS -lfabric"

# For runtime
CMK_INCDIR="$CMK_INCDIR -I./proc_management/ -I./proc_management/simple_pmi/"

CMK_QT='generic64-light'
