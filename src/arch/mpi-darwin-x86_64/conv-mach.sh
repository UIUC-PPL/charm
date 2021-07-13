. $CHARMINC/cc-mpiopts.sh
. $CHARMINC/conv-mach-darwin.sh

CMK_DEFS="$CMK_DEFS -mmacosx-version-min=10.7"
# Assumes gfortran compiler:
CMK_FDEFS="$CMK_FDEFS -mmacosx-version-min=10.7"

CMK_CC="$MPICC"
CMK_CXX="$MPICXX"

CMK_REAL_COMPILER=`$MPICXX -show 2>/dev/null | cut -d' ' -f1 `
case "${CMK_REAL_COMPILER##*/}" in
  gcc|g++|gcc-*|g++-*)
    # keep in sync with common/cc-gcc.sh
    CMK_CC_FLAGS="-fPIC"
    CMK_CXX_FLAGS="-fPIC -Wno-deprecated"
    CMK_LD_FLAGS="-fPIC"
    CMK_LDXX_FLAGS="-fPIC -multiply_defined suppress"
    CMK_COMPILER='gcc'
    ;;
  clang|clang++|clang-*|clang++-*)
    CMK_COMPILER='clang'
    ;;
esac
