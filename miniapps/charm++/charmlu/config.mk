#------------------------------------------------
# To compile on order or faith(Ubuntu):
SEND_LIM  = 2
BLAS_INC  = -DUSE_CBLAS_H=1
BLAS_LD   =
BLAS_LIBS = -lcblas


# To compile on hope (OSX):
#SEND_LIM ?= 2
#BLAS_INC  = -DUSE_ACCELERATE_BLAS=1
#BLAS_LD   =
#BLAS_LIBS = -framework Accelerate


# To compile on Cray XT5 with "module load atlas/3.8.3"
#SEND_LIM ?= 1
#BLAS_INC  = -I$(ATLASINCLUDE) -DUSE_BLAS  -DUSE_CBLAS_H=1 -DUSE_MEMALIGN
#BLAS_LD   = -L$(ATLAS_LIB)
#BLAS_LIBS = -llapack -lf77blas -lcblas -latlas

# To compile on Cray XT5 with "module load acml"
#SEND_LIM ?= 1
#BLAS_INC  =  -I$(ACML_DIR)/pgi64/include/ -DUSE_ACML_H=1 -DUSE_ACML
#BLAS_LD   = -L$(ACML_DIR)/pgi64/lib/
#BLAS_LIBS = -lacml

# To compile on Cray XT5 with GNU compilers "module swap PrgEnv-pgi PrgEnv-gnu" and "module load acml"
#SEND_LIM ?= 1
#BLAS_INC  = -I$(ACML_DIR)/gnu64/include/ -DUSE_ACML_H=1 -DUSE_ACML
#BLAS_LD   = -L$(ACML_DIR)/gnu64/lib/
#BLAS_LIBS = -lacml


# To compile on BG/P with ESSL:
#SEND_LIM ?= 50
#BGP_ESSL  = /soft/apps/ESSL-4.4.1-0
#BGP_LIBS  = -L$(BGP_ESSL)/lib \
#	        -L/bgsys/ibm_compilers/sles10/prod/opt/ibmcmp/xlf/bg/11.1/bglib/ \
#	        -L/soft/apps/ibmcmp/xlsmp/bg/1.7/bglib \
#	        -L/soft/apps/ibmcmp/xlf/bg/11.1/bglib \
#
#BLAS_INC  = -DUSE_ESSL=1 -I$(BGP_ESSL)/include -DUSE_MEMALIGN=1
#BLAS_LD   = $(BGP_LIBS)
#BLAS_LIBS = -lesslbg -lmass -lxlfmath -lxlf90_r -lxlsmp -lxlomp_ser -lpthread


# To compile on BG/Q with ESSL:
#SEND_LIM ?= 500
#BGQ_ESSL  = /soft/libraries/essl/current
#BGQ_LIBS  = -L$(BGQ_ESSL)/lib64 \
#            -L/soft/compilers/ibmcmp-aug2012-interim120809/xlf/bg/14.1/lib64
#
#BLAS_INC  = -DUSE_ESSL=1 -I$(BGQ_ESSL)/include -DUSE_MEMALIGN=1
#BLAS_LD   = $(BGQ_LIBS)
#BLAS_LIBS = -lesslsmpbg -lmass -lxlfmath -lxlf90_r -lxlsmp -lxlomp_ser -lpthread
