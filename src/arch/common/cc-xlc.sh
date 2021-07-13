CMK_DEFS="$CMK_DEFS -q32"
CMK_FDEFS="$CMK_FDEFS -q32"

CMK_CC="xlc_r -qcpluscmt "
CMK_CXX="xlC_r"
CMK_C_OPTIMIZE='-O3 -qstrict -Q  '
CMK_CXX_OPTIMIZE='-O3 -qstrict -Q '
CMK_PIC='-qpic=small'

CMK_LD="$CMK_CC -brtl "
CMK_LDXX="$CMK_CXX -brtl "

CMK_NATIVE_CC='xlc_r '
CMK_NATIVE_LD='xlc_r '
CMK_NATIVE_CXX='xlC_r -D_H_UNISTD -DYY_NEVER_INTERACTIVE=1 '
CMK_NATIVE_LDXX='xlC_r '

CMK_CF77='xlf_r'
CMK_CF90='xlf90_r -qsuffix=f=f90'
CMK_CF90_FIXED='xlf90_r'

if test "$isAIX" = "true"
then
  AR_OPTS="-X 32"
fi
CMK_AR="ar $AR_OPTS cq"
CMK_NM="nm $AR_OPTS"

CMK_C_OPENMP="-qsmp=omp"

CMK_COMPILER='xlc'
