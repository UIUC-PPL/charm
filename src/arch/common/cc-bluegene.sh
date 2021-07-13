BGQ_TYPICAL_FLOOR=/bgsys/drivers/ppcfloor

# if no floor set, use typical floor path
if test -z "$BGQ_FLOOR"
then
  BGQ_FLOOR=$BGQ_TYPICAL_FLOOR
fi

# if no install path (for experimental) set, use floor
if test -z "$BGQ_INSTALL"
then
  BGQ_INSTALL=$BGQ_TYPICAL_FLOOR
fi

BGQ_BIN=$BGQ_FLOOR/gnu-linux/bin
BGQ_ZLIB=/soft/libraries/alcf/current/xl/ZLIB/

GCC_OPTS="-Wno-deprecated "

CMK_CPP_CHARM="$BGQ_BIN/powerpc64-bgq-linux-cpp -P"
CMK_CPP_C="$BGQ_BIN/powerpc64-bgq-linux-cpp "

CMK_CPP_C_FLAGS="-E"

CMK_LD_LIBRARY_PATH="-Wl,-rpath,$CHARMLIBSO/"

CMK_NATIVE_CC='gcc'
CMK_NATIVE_LD='gcc'
CMK_NATIVE_CXX='g++'
CMK_NATIVE_LDXX='g++'

CMK_NATIVE_CC_FLAGS='-Wno-deprecated'
CMK_NATIVE_CXX_FLAGS='-Wno-deprecated'
CMK_NATIVE_LD_FLAGS=''
CMK_NATIVE_LDXX_FLAGS=''

CMK_LIBS='-lckqt'
CMK_RANLIB="$BGQ_BIN/powerpc64-bgq-linux-ranlib "
CMK_AR="$BGQ_BIN/powerpc64-bgq-linux-ar q "
CMK_QT="aix"
CMK_NM='nm '

CMK_C_OPTIMIZE='-O3'
CMK_CXX_OPTIMIZE='-O3'
CMK_ENABLE_C11='-qlanglvl=extc1x'
CMK_ENABLE_CPP11='-qlanglvl=extended0x'

CMK_F90LIBS="-lxlf90 -lxlopt -lxl -lxlfmath"
CMK_MOD_NAME_ALLCAPS=1
CMK_MOD_EXT="mod"
CMK_F90_USE_MODDIR=1
CMK_F90_MODINC="-I"

CMK_NO_ISO_MALLOC='1'

CMK_BLUEGENEQ="1"
