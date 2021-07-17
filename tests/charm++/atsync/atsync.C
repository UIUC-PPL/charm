#include <stdio.h>
#include <thread>

#include "atsync.decl.h"

/*readonly*/ CProxy_Main mainProxy;
/*readonly*/ int nElements;
/*readonly*/ int iters;

/*mainchare*/
class Main : public CBase_Main
{
public:
  Main(CkArgMsg* m)
  {
    // Process command-line arguments
    nElements = 5;
    if (m->argc > 1)
      nElements = atoi(m->argv[1]);
    iters = 10;
    if (m->argc > 2)
      iters = atoi(m->argv[2]);
    delete m;

    // Start the computation
    CkPrintf("Running atsync on %d processors for %d elements\n", CkNumPes(), nElements);

    mainProxy = thisProxy;
    CProxy_Test arr = CProxy_Test::ckNew();
    for (int i = 0; i < nElements; i++)
    {
      arr[i].insert();
    }
    arr.doneInserting();

    arr[0].start();
  };

  void done(void)
  {
    CkPrintf("All done\n");
    CkExit();
  };
};

class Test : public CBase_Test
{
private:
  int counter;
  int originalPE;

public:
  Test()
  {
    CkPrintf("atsync (%d) created on %d\n", thisIndex, CkMyPe());
    counter = 0;
    originalPE = CkMyPe();
    usesAtSync = true;
  }

  Test(CkMigrateMessage* m) {}

  void ResumeFromSync()
  {
    counter++;
    CkPrintf("[%d] %d resume from sync, iter %d\n", CkMyPe(), thisIndex, counter);
    if (counter == iters)
    {
      CkCallback cb(CkIndex_Main::done(), mainProxy);
      contribute(cb);
    }
    else if (thisIndex == 0)
      thisProxy[thisIndex].start();
  }

  void pup(PUP::er& p)
  {
    p | counter;
    p | originalPE;
  }

  void start()
  {
    if (thisIndex + 1 < nElements)
    {
      CkPrintf("[%d] Sending to %d from element %d\n", CkMyPe(), thisIndex + 1,
               thisIndex);
      thisProxy[thisIndex + 1].start();
    }

    AtSync();
  }
};

#include "atsync.def.h"
