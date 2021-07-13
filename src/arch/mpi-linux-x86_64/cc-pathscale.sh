CMK_CPP_C='pathcc -m64 -E'
CMK_CC='pathcc -O2 -m64 -fPIC '
CMK_CXX='pathCC -m64 -fPIC'
CMK_LIBS='-lckqt -lmpich -L/opt/pathscale/lib/2.0 -lmv -lmpath'

CMK_SEQ_CC='pathcc -O1 -Wno-deprecated -m64 -fPIC '
CMK_SEQ_LD='pathcc -m64 -fPIC '
CMK_SEQ_CXX='pathCC -Wno-deprecated -m64 -fPIC'
CMK_SEQ_LDXX='pathCC -m64 -fPIC '

CMK_CF77='pathf90 -m64 -fPIC '
CMK_CF90='pathf90 -m64 -fPIC'
CMK_F90LIBS=''
CMK_F90_USE_MODDIR=1
CMK_F90_MODINC="-p"

CMK_COMPILER='pathscale'
