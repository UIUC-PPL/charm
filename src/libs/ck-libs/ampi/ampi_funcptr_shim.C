// This object can be linked to AMPI binaries in place of the RTS.

#ifndef AMPI_USE_FUNCPTR
# error This file requires -fPIE -DAMPI_USE_FUNCPTR.
#endif
#include "ampi_funcptr.h"


// Provide the definitions of function pointers corresponding to the entire AMPI API.

#define AMPI_FUNC AMPI_FUNCPTR_DEF
#define AMPI_FUNC_NOIMPL AMPI_FUNC
#define AMPI_CUSTOM_FUNC AMPI_CUSTOM_FUNCPTR_DEF

#include "ampi_functions.h"

#undef AMPI_FUNC
#undef AMPI_FUNC_NOIMPL
#undef AMPI_CUSTOM_FUNC


// Provide an interface to link the function pointers at runtime.

extern "C" CMI_EXPORT int AMPI_FuncPtr_Unpack(const struct AMPI_FuncPtr_Transport * funcptrs, size_t size)
{
  if (sizeof(*funcptrs) != size)
    return 1;

#define AMPI_CUSTOM_FUNC(return_type, function_name, ...) \
  function_name = funcptrs->function_name;
#if AMPI_HAVE_PMPI
  #define AMPI_FUNC(return_type, function_name, ...) \
    function_name = funcptrs->function_name; \
    P##function_name = funcptrs->P##function_name;
#else
  #define AMPI_FUNC AMPI_CUSTOM_FUNC
#endif
#define AMPI_FUNC_NOIMPL AMPI_FUNC

#include "ampi_functions.h"

#undef AMPI_FUNC
#undef AMPI_FUNC_NOIMPL
#undef AMPI_CUSTOM_FUNC

  return 0;
}
