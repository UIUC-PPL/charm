
#include "ampi_funcptr_loader.h"

#include <string>
#include <vector>
#include <atomic>

struct rankstruct
{
  ampi_mainstruct mainstruct;
  SharedObject exe;
};

static std::vector<rankstruct> rankdata;

void AMPI_Node_Setup(int numranks)
{
  if (CmiMyNode() == 0 && !quietModeRequested)
    CmiPrintf("AMPI> Using pipglobals privatization method.\n");

  AMPI_FuncPtr_Transport funcptrs{};
  if (AMPI_FuncPtr_Pack(&funcptrs, sizeof(funcptrs)))
    CkAbort("Globals runtime linking pack failed due to mismatch!");

  static const char exe_suffix[] = STRINGIFY(CMK_POST_EXE);
  static const char suffix[] = STRINGIFY(CMK_USER_SUFFIX) "." STRINGIFY(CMK_SHARED_SUF);
  static constexpr size_t exe_suffix_len = sizeof(exe_suffix)-1;
  std::string binary_path{ampi_binary_path};
  if (exe_suffix_len > 0)
  {
    size_t pos = binary_path.length() - exe_suffix_len;
    if (!binary_path.compare(pos, exe_suffix_len, exe_suffix))
      binary_path.resize(pos);
  }
  binary_path += suffix;

  // open the user binary for each rank in a unique namespace
  rankdata.resize(numranks);
  for (int myrank = 0; myrank < numranks; ++myrank)
  {
    const Lmid_t lmid = LM_ID_NEWLM;
    int flags = RTLD_NOW|RTLD_LOCAL;
    SharedObject myexe = dlmopen(lmid, binary_path.c_str(), flags);

    if (myexe == nullptr)
    {
      CkError("dlmopen error: %s\n", dlerror());
      CkAbort("Could not open pipglobals user program!");
    }

    auto unpack = AMPI_FuncPtr_Unpack_Locate(myexe);
    if (unpack != nullptr)
    {
      if (unpack(&funcptrs, sizeof(funcptrs)))
        CkAbort("Globals runtime linking unpack failed due to mismatch!");
    }

    rankdata[myrank].exe = myexe;
    rankdata[myrank].mainstruct = AMPI_Main_Get(myexe);
  }
}

// separate function so that setting a breakpoint is straightforward
static int ampi_pipglobals(int argc, char ** argv)
{
  const size_t myrank = TCHARM_Element();
  return AMPI_Main_Dispatch(rankdata[myrank].mainstruct, argc, argv);
}

int main(int argc, char ** argv)
{
  return ampi_pipglobals(argc, argv);
}
