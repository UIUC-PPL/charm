COMMENT="Use pgf90 fortran compiler in $PG_DIR"
PG_DIR=`command -v pgf90`
if test x$PG_DIR = x 
then
  echo charmc> Fatal error: pgf90 not found!
  exit 1
fi
PG_DIR="`dirname $PG_DIR`/.."
CMK_CF77="$PG_DIR/bin/pgf77 "
CMK_CF90="$PG_DIR/bin/pgf90 "
CMK_CF90_FIXED="$CMK_CF90 -Mfixed "
CMK_F90LIBS="-L$PG_DIR/lib -lpgf90 -lpgf90_rpm1 -lpgf902 -lpgf90rtl -lpgftnrtl -lm -lpgc "
CMK_F90_MODINC="-module"
CMK_F90_USE_MODDIR=""
