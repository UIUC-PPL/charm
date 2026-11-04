OPTS = -O3 -DHAPI_CUDA_CALLBACK

CHARM_DIR = ../../../../..
CHARMC = $(CHARM_DIR)/bin/charmc $(OPTS)
CHARM_INC = -I$(CHARM_DIR)/include

# cuBLAS ships beside the CUDA toolkit in the NVHPC tree (math_libs), not in
# the CUDA_DIR the runtime was built against. Derive it from the build's
# CUDA_DIR; override MATHLIBS on the make line for any other layout.
CUDA_DIR := $(shell sed -n 's/^CUDA_DIR="\(.*\)"/\1/p' $(CHARM_DIR)/include/conv-mach-opt.sh)
MATHLIBS ?= $(subst /cuda/,/math_libs/,$(CUDA_DIR))

# The device compiler and BLAS follow the build the runtime was made with (conv-mach-opt.mak), as leanmd's.
-include $(CHARM_DIR)/include/conv-mach-opt.mak
ifeq ($(BUILD_HIP),1)
NVCC = hipcc
NVCC_FLAGS = -x hip -c -std=c++17 -ffast-math $(OPTS) $(patsubst %,--offload-arch=%,$(subst ;, ,$(HIP_ARCHS)))
LD_LIBS = -balancer ShedLB -module EveryLB -module DiffusionLB -L$(ROCM_PATH)/lib -lhipblas -Wl,-rpath,$(ROCM_PATH)/lib
# The block variant's attention (-w) under -B runs on aotriton's fused attention kernels, the flash kernels
# PyTorch's attention dispatches to on ROCm: the library, its gfx90a kernel images and headers of the torch
# 2.13+rocm7.1 wheel, staged in a directory of their own so that the library's libamdhip64.so resolves to the
# runtime's HIP and not the wheel's (FRONTIER_START.md). Found there by default; MOE_AOTRITON_ATTN=0 on the make
# line builds the unfused kernels. The fp32 path is always unfused.
AOTRITON_DIR ?= /lustre/orion/csc710/scratch/adityapb/hip/aotriton
MOE_AOTRITON_ATTN ?= $(if $(wildcard $(AOTRITON_DIR)/include/aotriton/flash.h),1,0)
ifeq ($(MOE_AOTRITON_ATTN),1)
NVCC_FLAGS += -DMOE_AOTRITON_ATTN=1 -I$(AOTRITON_DIR)/include
LD_LIBS += -L$(AOTRITON_DIR)/lib -laotriton_v2 -Wl,-rpath,$(AOTRITON_DIR)/lib
endif
else
NVCC = nvcc
NVCC_FLAGS = -c -std=c++17 -use_fast_math -gencode arch=compute_86,code=sm_86 $(OPTS) -I$(MATHLIBS)/include
# ShedLB is this application's balancer (README: why); +balancer on the command line still overrides it.
LD_LIBS = -balancer ShedLB -module EveryLB -module DiffusionLB -L$(MATHLIBS)/lib64 -lcublas -Wl,-rpath,$(MATHLIBS)/lib64
# The block variant's attention (-w) under -B runs on cuDNN's fused attention graphs: cuDNN 9 and its header-only
# frontend from the pytorch-2.12.1-cu130 environment (the same kernels PyTorch's attention dispatches to). Found
# there by default; MOE_CUDNN_ATTN=0 on the make line builds the unfused kernels (the A/B of README "Measured
# 2026-11-04"). The fp32 path and the HIP build are always unfused.
CUDNN_DIR ?= /sw/rh9.4/user/python/conda-env/pytorch-2.12.1-cu130/lib/python3.12/site-packages/nvidia/cudnn
CUDNN_FE ?= /sw/rh9.4/user/python/conda-env/pytorch-2.12.1-cu130/lib/python3.12/site-packages/include
MOE_CUDNN_ATTN ?= $(if $(wildcard $(CUDNN_FE)/cudnn_frontend.h),1,0)
ifeq ($(MOE_CUDNN_ATTN),1)
NVCC_FLAGS += -DMOE_CUDNN_ATTN=1 -I$(CUDNN_DIR)/include -I$(CUDNN_FE)
LD_LIBS += -L$(CUDNN_DIR)/lib -l:libcudnn.so.9 -Wl,-rpath,$(CUDNN_DIR)/lib -L$(CUDA_DIR)/lib64 -lnvrtc -Wl,-rpath,$(CUDA_DIR)/lib64   # the wheel ships no libcudnn.so link; the frontend references NVRTC
endif
endif

TARGET = moe
all: $(TARGET)

OBJS = $(TARGET).o $(TARGET)CUDA.o

$(TARGET): $(OBJS)
	$(CHARMC) -language charm++ -o $@ $(OBJS) $(LD_LIBS)

$(TARGET).decl.h: $(TARGET).ci $(TARGET).h
	$(CHARMC) $<

$(TARGET).def.h: $(TARGET).ci $(TARGET).h

$(TARGET).o: $(TARGET).C $(TARGET).decl.h $(TARGET).def.h $(TARGET).h
	$(CHARMC) -c $<

$(TARGET)CUDA.o: $(TARGET).cu $(TARGET).h
	$(NVCC) -o $@ $(NVCC_FLAGS) $(CHARM_INC) $<

clean:
	rm -f *.decl.h *.def.h conv-host *.o $(TARGET) charmrun

# Kokkos variant (moe_kokkos.cpp, ../hapi_kokkos.h): the same host object, the kernels as Kokkos functors; cuBLAS
# stays (one handle per lane with a fixed workspace, which KokkosBlas does not offer). Same device flags as the CUDA
# variant (sm_86 code) plus C++20 and extended lambdas (Kokkos develop).
-include $(CHARM_DIR)/include/conv-mach-opt.mak
# Kokkos 5 (the develop branch) needs C++20; 4.7 builds with it too.
KOKKOS_STD ?= c++20
# Kokkos sources are plain .cpp; the vendor's compiler is told the language and gets its own flags: nvcc -x cu with
# extended lambdas on CUDA, hipcc -x hip on HIP (lambdas need no flag there). The Kokkos install follows the vendor.
ifeq ($(BUILD_HIP),1)
KOKKOS_DIR ?= /u/bhosale/kokkos-develop/install-hip
KOKKOS_CXX = hipcc -x hip $(patsubst %,--offload-arch=%,$(subst ;, ,$(HIP_ARCHS)))
KOKKOS_DEV_FLAGS = -ffast-math
KOKKOS_DEV_LIBS =
# The BLAS the Kokkos variant links (hapi_blas.h): hipBLAS from ROCm here, cuBLAS from MATHLIBS on CUDA.
KOKKOS_BLAS_INC = -I$${ROCM_PATH:-/opt/rocm}/include
KOKKOS_BLAS_LIBS = -L$${ROCM_PATH:-/opt/rocm}/lib -lhipblas
else
KOKKOS_DIR ?= /u/bhosale/kokkos-develop/install-sm86
KOKKOS_CXX = nvcc -x cu
KOKKOS_DEV_FLAGS = -use_fast_math -gencode arch=compute_86,code=sm_86 -extended-lambda -Wext-lambda-captures-this
KOKKOS_DEV_LIBS = -lcuda
KOKKOS_BLAS_INC = -I$(MATHLIBS)/include
KOKKOS_BLAS_LIBS = -L$(MATHLIBS)/lib64 -lcublas -Wl,-rpath,$(MATHLIBS)/lib64
endif
KOKKOS_NVCC_FLAGS = -c -std=$(KOKKOS_STD) $(KOKKOS_DEV_FLAGS) $(OPTS) $(KOKKOS_BLAS_INC) -I$(KOKKOS_DIR)/include -I..
KOKKOS_LIBS = -L$(KOKKOS_DIR)/lib64 -lkokkoscore -ldl $(KOKKOS_DEV_LIBS) $(KOKKOS_BLAS_LIBS)

$(TARGET).kokkos: $(TARGET).o $(TARGET)KOKKOS.o
	$(CHARMC) -language charm++ -o $@ $^ -balancer ShedLB -module EveryLB -module DiffusionLB $(KOKKOS_LIBS)

$(TARGET)KOKKOS.o: $(TARGET)_kokkos.cpp $(TARGET).h ../hapi_kokkos.h
	$(KOKKOS_CXX) -o $@ $(KOKKOS_NVCC_FLAGS) $(CHARM_INC) $<
