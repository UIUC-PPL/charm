
PGCC=`CC -v 2>&1 | grep pgCC`
ICPC=`CC -v 2>&1 | grep Intel`
CLANG=`CC -v 2>&1 | grep 'clang'`
GNU=`CC -v 2>&1 | grep 'g++'`
CCE=`CC -v 2>&1 | grep 'Cray'`

CMK_CPP_CHARM="cpp -P"
CMK_CPP_C="cc"
CMK_CC="cc "
CMK_CXX="CC "
CMK_LD="$CMK_CC "
CMK_LDXX="$CMK_CXX "

CMK_CPP_C_FLAGS="-E"

CMK_LINK_BINARY='-dynamic'

if test -v CMK_CRAY_NOGNI
then
    CMK_CRAY_LIBS="$CRAY_PMI_POST_LINK_OPTS -lpmi $CRAY_RCA_POST_LINK_OPTS"
else
    CMK_CRAY_LIBS="$CRAY_PMI_POST_LINK_OPTS $CRAY_UGNI_POST_LINK_OPTS -lugni -lpmi $CRAY_RCA_POST_LINK_OPTS"
fi
CMK_LIBS="-lckqt $CMK_CRAY_LIBS"

CMK_LD_LIBRARY_PATH="-Wl,-rpath,$CHARMLIBSO/"

CMK_QT="generic64-light"

# compiler for compiling sequential programs
if test -n "$PGCC"
then
CMK_CC_FLAGS="$CMK_CC_FLAGS -DCMK_FIND_FIRST_OF_PREDICATE=1 "
CMK_CXX_FLAGS="$CMK_CXX_FLAGS -DCMK_FIND_FIRST_OF_PREDICATE=1 --no_using_std "
# gcc is needed for building QT
CMK_SEQ_CC="gcc -fPIC "
CMK_SEQ_CXX="pgCC -fPIC --no_using_std "
CMK_COMPILER='pgcc'
elif test -n "$CCE"
then
CMK_CXX_OPTIMIZE=" -hipa4"   # For improved C++ performance
CMK_SEQ_CC="gcc -fPIC"
CMK_SEQ_CXX="g++ -fPIC "
CMK_COMPILER='craycc'
elif test -n "$ICPC"
then
CMK_SEQ_CC="icc -fPIC "
CMK_SEQ_CXX="icpc -fPIC "
CMK_COMPILER='icc'
elif test -n "$CLANG"
then
CMK_SEQ_CC="clang -fPIC "
CMK_SEQ_CXX="clang++ -fPIC "
CMK_COMPILER='clang'
else   # gcc
CMK_SEQ_CC="gcc -fPIC"
CMK_SEQ_CXX="g++ -fPIC "
CMK_COMPILER='gcc'
fi
CMK_SEQ_LD="$CMK_SEQ_CC "
CMK_SEQ_LDXX="$CMK_SEQ_CXX "
CMK_SEQ_LIBS=""

CMK_SEQ_CC_FLAGS=' '
CMK_SEQ_CXX_FLAGS=' '
CMK_SEQ_LD_FLAGS=' '
CMK_SEQ_LDXX_FLAGS=' '

# compiler for native programs
CMK_NATIVE_CC="gcc "
CMK_NATIVE_LD="gcc "
CMK_NATIVE_CXX="g++ "
CMK_NATIVE_LDXX="g++ "
CMK_NATIVE_LIBS=""

CMK_NATIVE_CC_FLAGS=' '
CMK_NATIVE_CXX_FLAGS=' '
CMK_NATIVE_LD_FLAGS=' '
CMK_NATIVE_LDXX_FLAGS=' '

CMK_RANLIB="ranlib"

# for F90 compiler
if test -n "$ICPC"
then
CMK_CF77="ftn -auto "
CMK_CF90="ftn -auto "

F90DIR=`command -v ifort 2> /dev/null`
if test -x "$F90DIR"
then
  MYDIR="$PWD"
  cd `dirname "$F90DIR"`
  if test -L 'ifort'
  then
    F90DIR=`readlink ifort`
    cd `dirname "$F90DIR"`
  fi
  F90DIR=`pwd -P`
  cd "$MYDIR"

  Minor=`basename $F90DIR`
  F90LIBDIR="$F90DIR/../lib/$Minor"
  if ! test -x "$F90LIBDIR"
  then
    F90LIBDIR="$F90DIR/../lib"
    if ! test -x "$F90LIBDIR"
    then
      F90LIBDIR="$F90DIR/../../compiler/lib/$Minor"
    fi
    if ! test -x "$F90LIBDIR"
    then
      F90LIBDIR="$F90DIR/../../lib/$Minor"
    fi
    if ! test -x "$F90LIBDIR"
    then
      F90LIBDIR="$F90DIR/../../compiler/lib/${Minor}_lin"
    fi
    if ! test -x "$F90LIBDIR"
    then
      F90LIBDIR="$F90DIR/../../lib/${Minor}_lin"
    fi
  fi
  F90MAIN="$F90LIBDIR/for_main.o"
fi
CMK_F90MAINLIBS="$F90MAIN"
CMK_F90LIBS="-L$F90LIBDIR -lifcore -lifport -lifcore"
CMK_F77LIBS="$CMK_F90LIBS"
else
CMK_CF77="ftn "
CMK_CF90="ftn "
CMK_F90LIBS=""
fi

if test -z "$CCE"
then
    CMK_CF77="$CMK_CF77 -ffree-line-length-none"
    CMK_CF90="$CMK_CF90 -ffree-line-length-none"
    CMK_F90LIBS=""
fi

CMK_F90_USE_MODDIR=1
CMK_F90_MODINC="-I"
CMK_MOD_EXT="mod"
