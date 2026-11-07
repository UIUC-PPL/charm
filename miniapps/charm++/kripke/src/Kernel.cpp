#include"Kernel.h"
#include"Grid.h"
#include"SubTVec.h"

#include"Kernel_3d_GDZ.h"
#include"Kernel_3d_DGZ.h"
#include"Kernel_3d_ZDG.h"
#include"Kernel_3d_DZG.h"
#include"Kernel_3d_ZGD.h"
#include"Kernel_3d_GZD.h"


/**
 * Factory to create a kernel object for the specified nesting
 */
Kernel *createKernel(Nesting_Order nest, int num_dims){
  if(num_dims == 3){
    switch(nest){
    case NEST_GDZ:
      return new Kernel_3d_GDZ();
    case NEST_DGZ:
      return new Kernel_3d_DGZ();
    case NEST_ZDG:
      return new Kernel_3d_ZDG();
    case NEST_DZG:
      return new Kernel_3d_DZG();
    case NEST_ZGD:
      return new Kernel_3d_ZGD();
    case NEST_GZD:
      return new Kernel_3d_GZD();
    }
  }

  CkAbort("Failed to allocate a new Kernel object with specified nesting and dimensions\n");
}

