if test -z $"CMK_PMI_INC"
then
    CMK_PMI_INC=`pkg-config --cflags cray-pmi`
    CMK_PMI_LIBS=`pkg-config --libs cray-pmi`
    CMK_LIBPMI_LDPATH=`pkg-config cray-pmi --variable=libdir`
fi

