
CMK_CPP_CHARM="cpp -P"
CMK_CPP_C_FLAGS="$CMK_CC_FLAGS -E"

CMK_CF77="f77"
CMK_CF90="f90"

CMK_XIOPTS=""
CMK_F90LIBS="-lvast90 -lg2c"

CMK_POST_EXE=".exe"
CMK_QT="none"

. $CHARMINC/cc-msvc.sh

CMK_MULTICORE="1"
CMK_NO_PARTITIONS="1"