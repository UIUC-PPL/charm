#include "allGather.h"
#if defined(_WIN32)
//for getpid()
#include "process.h"
#endif

int AllGather::gen_rand()
{
  std::mt19937_64 gen(randCounter++);
  std::uniform_int_distribution<int> dis(0, n - 1);
  return dis(gen);
}

AllGather::AllGather(int k, int type, int seed) : k(k)
{
  n = CkNumPes();
  this->type = (allGatherType)type;
  switch (type)
  {
    case allGatherType::ALL_GATHER_HYPERCUBE:
      if ((n & (n - 1)) != 0)
        HypercubeRecursiveDoubling = true;
      numHypercubeIter = std::ceil(std::log2(n));
      break;
    case allGatherType::ALL_GATHER_FLOODING:
      randCounter = seed ? seed : getpid();
      graph.resize(n);
      for (int i = 0; i < n; i++) graph[i].resize(n);
      /**
       * TODO: Experiment with different graph structures
       */

      // Ring with constant number of random connections
      for (int i = 0; i < n; i++)
      {
        graph[i][(n + i + 1) % n] = 1;
        graph[i][(n + i - 1) % n] = 1;
      }
      for (int i = 0; i < 6; i++)
      {
        int x = gen_rand();
        int y = gen_rand();
        if (x != y)
        {
          graph[x][y] = 1;
          graph[y][x] = 1;
        }
      }
      break;
    case allGatherType::ALL_GATHER_RING:
      break;
  }
}

void AllGather::init(void* result, void* data, int idx, CkCallback cb)
{
  this->msg = new allGatherMsg;
  this->lib_done_callback = cb;
  this->idx = idx;
  zero_copy_callback =
      CkCallback(CkIndex_AllGather::local_buff_done(NULL), thisProxy[CkMyPe()]);
  this->store = (char*)result;
  this->data = (char*)data;
  this->numRecvMsg = 0;
  CkCallback cbInitDone(CkReductionTarget(AllGather, startGather), thisProxy);
  contribute(cbInitDone);
}

void AllGather::local_buff_done(CkDataMsg* m)
{
  numRecvMsg++;
  if (numRecvMsg == 2 * (n - 1))
  {
    switch (type)
    {
      case allGatherType::ALL_GATHER_HYPERCUBE:
        hyperCubeIndx.clear();
        hyperCubeStore.clear();
        break;
      case allGatherType::ALL_GATHER_FLOODING:
        recvFloodMsg.clear();
        break;
      case allGatherType::ALL_GATHER_RING:
        break;
    }
    // Every transfer from and into this element has completed.
    src.deregisterMem();
    lib_done_callback.send(msg);
  }
}

void AllGather::startGather()
{
  for (int i = 0; i < k; i++) store[k * idx + i] = data[i];
  src = CkNcpyBuffer(data, k * sizeof(char), zero_copy_callback, CK_BUFFER_REG,
                     CK_BUFFER_NODEREG);

  switch (type)
  {
    case allGatherType::ALL_GATHER_RING:
      thisProxy[(idx + 1) % n].recvRing(idx, src);
      break;
    case allGatherType::ALL_GATHER_HYPERCUBE:
      hyperCubeIndx.push_back(idx);
      hyperCubeStore.push_back(src);
      thisProxy[idx].Hypercube();
      break;
    case allGatherType::ALL_GATHER_FLOODING:
      recvFloodMsg[idx] = true;
      for (int i = 0; i < n; i++)
        if (graph[idx][i] == 1)
          thisProxy[i].Flood(idx, src);
      break;
  }
}

void AllGather::recvRing(int sender, CkNcpyBuffer src)
{
  if (numRecvMsg == 2 * (n - 1))
    return;
  CkNcpyBuffer dst(store + sender * k, k * sizeof(char), zero_copy_callback,
                   CK_BUFFER_REG);
  dst.get(src);
  if (((CkMyPe() + 1) % n) != sender)
    thisProxy[(CkMyPe() + 1) % n].recvRing(sender, src);
}

void AllGather::Flood(int sender, CkNcpyBuffer src)
{
  if (numRecvMsg == 2 * (n - 1))
    return;
  if (recvFloodMsg[sender])
    return;
  recvFloodMsg[sender] = true;
  CkNcpyBuffer dst(store + sender * k, k * sizeof(char), zero_copy_callback,
                   CK_BUFFER_REG);
  dst.get(src);
  for (int i = 0; i < n; i++)
    if (graph[CkMyPe()][i] == 1 && i != sender)
      thisProxy[i].Flood(sender, src);
}

#include "allGather.def.h"
