# user environ var: MPICXX and MPICC
# or, use the definition in file $CHARMINC/MPIOPTS
if test -x "$CHARMINC/MPIOPTS"
then
  . $CHARMINC/MPIOPTS
else
  MPICXX_DEF=mpicxx
  MPICC_DEF=mpicc
fi

test -z "$MPICXX" && MPICXX=$MPICXX_DEF
test -z "$MPICC" && MPICC=$MPICC_DEF
test "$MPICXX" != "$MPICXX_DEF" && /bin/rm -f $CHARMINC/MPIOPTS
if test ! -f "$CHARMINC/MPIOPTS"
then
  echo MPICXX_DEF=$MPICXX > $CHARMINC/MPIOPTS
  echo MPICC_DEF=$MPICC >> $CHARMINC/MPIOPTS
  chmod +x $CHARMINC/MPIOPTS
fi

CMK_REAL_COMPILER=`$MPICXX -show 2>/dev/null | cut -d' ' -f1 `

CMK_CPP_CHARM='cpp -P'
CMK_CPP_C="$MPICC"

CMK_CPP_C_FLAGS="-E"

# avoid the need to link -lmpi_cxx on some systems
CMK_DEFS="$CMK_DEFS -DMPICH_SKIP_MPICXX -DOMPI_SKIP_MPICXX"

CMK_LIBS="-lckqt $CMK_SYSLIBS "
CMK_QT='generic64-light'
CMK_RANLIB='ranlib'
