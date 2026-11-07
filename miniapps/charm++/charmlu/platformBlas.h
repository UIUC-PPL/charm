#ifndef CHARMLU_PLATFORM_BLAS_H
#define CHARMLU_PLATFORM_BLAS_H

#if USE_CBLAS_H
extern "C" {
#include <cblas.h>
//#include <clapack.h>
}
#include <lapack.h>

#elif USE_MKL_CBLAS_H
#include "mkl_cblas.h"
#include "mkl_lapack.h"

#elif USE_ACML_H
#include "acml.h"
#define BLAS_TRANSPOSE 'T'
#define BLAS_NOTRANSPOSE 'N'
#define BLAS_RIGHT 'R'
#define BLAS_UPPER 'U'
#define BLAS_UNIT 'U'

#elif USE_ACCELERATE_BLAS
#include <Accelerate/Accelerate.h>

#elif USE_ESSL
#define _ESVCPTR
#include <complex>
#include <essl.h>

#define BLAS_TRANSPOSE "T"
#define BLAS_NOTRANSPOSE "N"
#define BLAS_RIGHT "R"
#define BLAS_UPPER "U"
#define BLAS_UNIT "U"

#else
#error "No BLAS Header files included!"
#endif

#endif
