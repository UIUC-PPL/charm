#ifndef __COMMON_H__
#define __COMMON_H__

#include <limits.h>
#include <stdint.h>

typedef float Real;
typedef uint64_t Key;

#define   PI         3.14159265358979323846
#define   TWO_PI     6.28318530717958647693
#define   FOUR_PI   12.56637061435917295385
#define   HALF_PI    1.57079632679489661923
#define   FRTHRD_PI  4.18879020478639098462

#define PREAMBLE_INTS 2 
#define PREAMBLE_REALS 1
#define PREAMBLE_SIZE ((PREAMBLE_INTS)*sizeof(int)+(PREAMBLE_REALS)*sizeof(Real))
#define REALS_PER_PARTICLE 8
#define SIZE_PER_PARTICLE ((REALS_PER_PARTICLE)*sizeof(Real))



#endif
