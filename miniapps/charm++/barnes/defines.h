#ifndef __DEFINES_H__
#define __DEFINES_H__

#include "limits.h"
#include "charm++.h"

#include "common.h"


#define TREE_KEY_BITS (sizeof(Key)*CHAR_BIT)

#define BITS_PER_DIM 21
#define BOXES_PER_DIM (1<<(BITS_PER_DIM))

#define NDIMS 3

#define DECOMP_TOLERANCE 1.2
#define BUCKET_TOLERANCE 1.2

#define DIV2(x) ((x)>>1)
#define EVEN(x) ((DIV2(x)<<1)==(x))

#define BRANCH_FACTOR 2
#define LOG_BRANCH_FACTOR 1

const double opening_geometry_factor = 2 / sqrt(3.0);

#define COSMO_CONST(x) (x)
#define CONVERT_TO_COSMO_TYPE

#define RRDEBUG /* empty */

#define TB_DEBUG /* empty */

#define NUM_PRIORITY_BITS (sizeof(int)*CHAR_BIT)

#define REQUEST_MOMENTS_PRIORITY (-8)
#define RECV_MOMENTS_PRIORITY (-7)
#define REQUEST_NODE_PRIORITY (-6)
#define REQUEST_PARTICLES_PRIORITY (-5)
#define RECV_NODE_PRIORITY (-4)
#define RECV_PARTICLES_PRIORITY (-3)
#define REMOTE_GRAVITY_PRIORITY (-2)
#define LOCAL_GRAVITY_PRIORITY (-1)

#define REMOTE_NODE_REQUEST 9998
#define REMOTE_PARTICLE_REQUEST 9999

#endif
