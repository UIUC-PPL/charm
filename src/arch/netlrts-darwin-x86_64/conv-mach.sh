. $CHARMINC/cc-clang.sh
. $CHARMINC/conv-mach-darwin.sh

CMK_DEFS="$CMK_DEFS -mmacosx-version-min=10.14"
# Assumes gfortran compiler:
CMK_FDEFS="$CMK_FDEFS -mmacosx-version-min=10.14"
