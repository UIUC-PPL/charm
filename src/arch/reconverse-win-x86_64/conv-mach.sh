# Reconverse on Windows: MSVC driven from Cygwin through unix2nt_cc.
. $CHARMINC/cc-msvc.sh

# .ci files are preprocessed by Cygwin's cpp, as classic Windows builds did:
# cl's preprocessor writes to stdout rather than to a named output file.
CMK_CPP_CHARM="cpp -P"

CMK_XIOPTS=''
CMK_QT='none'
# QuickThreads is not built under reconverse; strip charmc's mainline -lckqt
# (same idiom as reconverse-linux-x86_64).
CMK_LIBS="${CMK_LIBS//-lckqt/}"

CMK_SMP='1'

CMK_WINDOWS='1'
CMK_POST_EXE='.exe'
CMK_SHARED_SUF='dll'
