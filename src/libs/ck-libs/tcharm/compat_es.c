#include "charm-api.h"

typedef struct CmiIsomallocContext {
  void * opaque;
} CmiIsomallocContext;

CLINKAGE void TCHARM_Element_Setup(int myelement, int numelements, CmiIsomallocContext ctx);
void TCHARM_Element_Setup(int myelement, int numelements, CmiIsomallocContext ctx)
{
}
