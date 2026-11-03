
CMK_CC="$CHARMBIN/unix2nt_cc"
CMK_CPP_C="$CMK_CC"
CMK_CXX="$CHARMBIN/unix2nt_cc"
CMK_LD="$CMK_CC"
CMK_LDXX="$CMK_CXX"

CMK_CC_FLAGS="-D_CRT_SECURE_NO_DEPRECATE"
# Reconverse's converse.h needs C++17, and MSVC defaults to C++14 (GCC and
# clang default to C++17 already). This file is sourced again after the arch's
# conv-mach.sh, so the flag has to be here to stick.
CMK_CXX_FLAGS="-D_CRT_SECURE_NO_DEPRECATE -std=c++17"
CMK_LD_FLAGS="$CMK_CC_FLAGS"
CMK_LDXX_FLAGS="$CMK_CXX_FLAGS"

CMK_AR="$CHARMBIN/unix2nt_ar"
CMK_RANLIB="echo "
CMK_LIBS=""
CMK_MOD_EXT="vo"

# space character: avoid reset to default by conv-config.sh
CMK_PIC=' '
CMK_PIE=' '

CMK_WARNINGS_ARE_ERRORS="-WX"

CMK_COMPILER='msvc'
