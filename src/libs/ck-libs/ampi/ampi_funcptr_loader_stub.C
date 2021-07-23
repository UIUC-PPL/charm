
#include "ampi_funcptr_loader.h"

int AMPI_FuncPtr_Pack(struct AMPI_FuncPtr_Transport *, size_t)
{
  return 0;
}

AMPI_FuncPtr_Unpack_t AMPI_FuncPtr_Unpack_Locate(SharedObject)
{
  return nullptr;
}
