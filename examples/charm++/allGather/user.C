#include "user.h"

start::start(CkArgMsg* msg)
{
  if (msg->argc != 4)
  {
    ckout << "Usage: " << msg->argv[0]
          << " <chare_array_size> <num_data_points_per_chare_array_element> "
             "<num_bits_for_data_points>"
          << endl;
    CkExit();
  }

  int n = atoi(msg->argv[1]);
  int k = atoi(msg->argv[2]);
  int d = atoi(msg->argv[3]);
  delete msg;

  sim = CProxy_simBox::ckNew(thisProxy, k, n, d, n);

#ifdef FLOODING
  AllGather = CProxy_AllGather::ckNew(k * sizeof(long int),
                                      (int)allGatherType::ALL_GATHER_FLOODING, 0);
#endif

#ifdef HYPERCUBE
  AllGather = CProxy_AllGather::ckNew(k * sizeof(long int),
                                      (int)allGatherType::ALL_GATHER_HYPERCUBE, 0);
#endif

#ifdef RING
  AllGather = CProxy_AllGather::ckNew(k * sizeof(long int),
                                      (int)allGatherType::ALL_GATHER_RING, 0);
#endif

  sim.begin(AllGather);
}

void start::fini()
{
  if (numIter == 1)
  {
    ckout << "[STATUS] Completed the AllGather Simulation" << endl;
    CkExit();
  }
  else
  {
    numIter++;
    ckout << "[STATUS] Completed the AllGather Simulation, starting again" << endl;
    sim.begin(AllGather);
  }
}

simBox::simBox(CProxy_start startProxy, int k, int n, int d)
    : startProxy(startProxy), k(k), n(n), d(d)
{
}

void simBox::begin(CProxy_AllGather AllGatherGroup)
{
  result = (long int*)CkRdmaAlloc(k * n * sizeof(long int));
  data = (long int*)CkRdmaAlloc(k * sizeof(long int));
  long int max_serial = (1 << d) - 1;
  long int base = thisIndex;
  while (max_serial > 0)
  {
    base = base * 10;
    max_serial = max_serial / 10;
  }
  for (int i = 0; i < k; i++)
  {
    data[i] = base + i;
  }
  CkCallback cb(CkIndex_simBox::done(NULL), CkArrayIndex1D(thisIndex), thisProxy);
  AllGather* libptr = AllGatherGroup.ckLocalBranch();
  libptr->init((void*)result, (void*)data, thisIndex, cb);
}

void simBox::done(allGatherMsg* msg)
{
  bool success = true;
  for (int i = 0; i < n; i++)
  {
    long int max_serial = (1 << d) - 1;
    long int base = i;
    while (max_serial > 0)
    {
      base = base * 10;
      max_serial = max_serial / 10;
    }
    for (int j = 0; j < k; j++)
    {
      if (result[i * k + j] != base + j)
      {
        success = false;
        break;
      }
    }
    if (!success)
      break;
  }

  if (success)
    ckout << "[STATUS] Correct result for Chare " << thisIndex << endl;
  else
  {
    ckout << "[STATUS] Incorrect result for Chare " << thisIndex << endl;
    for (int i = 0; i < n * k; i++)
    {
      ckout << result[i] << " ";
    }
    ckout << endl;
    CkAbort("allGather: incorrect result on chare %d", thisIndex);
  }
  CkRdmaFree(result);
  CkRdmaFree(data);
  CkCallback cbfini(CkReductionTarget(start, fini), startProxy);
  contribute(cbfini);
}

#include "user.def.h"
