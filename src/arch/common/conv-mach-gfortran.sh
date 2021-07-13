CMK_CC_FLAGS="$CMK_CC_FLAGS -DCMK_GFORTRAN"
CMK_CXX_FLAGS="$CMK_CXX_FLAGS -DCMK_GFORTRAN"

if test -n "$CMK_MACOSX"
then
CMK_F90FLAGS="$CMK_F90FLAGS -fno-common"
CMK_F77FLAGS="$CMK_F77FLAGS -fno-common"
fi

CMK_FPP="$CMK_CPP_C -P -CC"

CMK_CF90=''

# If using gcc, try to find the matching gfortran version
[ "$CMK_COMPILER" = 'gcc' ] && command -v "gfortran$CMK_COMPILER_SUFFIX" >/dev/null 2>&1 && CMK_CF90="gfortran$CMK_COMPILER_SUFFIX"

# Find common gfortran binary names, and choose the first one found
# (presumably the most modern).
[ -z "$CMK_CF90" ] && CMK_CF90=$(command -v gfortran f95 gfortran-{19..4} gfortran-mp-{19..4} 2>/dev/null | head -1)

[ -z "$CMK_CF90" ] && { echo 'No gfortran found, exiting'; exit 1; }

# Find libgfortran, which we need to link to manually as the C++ compiler does
# the linking.
f90libdir="$(dirname "$($CMK_CF90 $CMK_FDEFS -print-file-name=libgfortran.a)")"
gccehlibdir="$(dirname "$($CMK_CF90 $CMK_FDEFS -print-file-name=libgcc_eh.a)")"

CMK_CF90="$CMK_CF90 $CMK_F90FLAGS -fPIC -fno-second-underscore -fdollar-ok "
CMK_CF90_FIXED="$CMK_CF90 -ffixed-form "

CMK_F90LIBS="-L$f90libdir/ -L$gccehlibdir/ -lgfortran -lgcc_eh "

CMK_CF77=$CMK_CF90
CMK_F77LIBS=$CMK_F90LIBS

CMK_MOD_NAME_ALLCAPS=
CMK_MOD_EXT="mod"
CMK_F90_USE_MODDIR=1
CMK_F90_MODINC="-I"
