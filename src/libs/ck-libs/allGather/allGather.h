#pragma once

#include "allGather.decl.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <utility>
#include <vector>

class allGatherMsg : public CMessage_allGatherMsg
{
};

enum allGatherType
{
  ALL_GATHER_RING,
  ALL_GATHER_HYPERCUBE,
  ALL_GATHER_FLOODING
};

class AllGather : public CBase_AllGather
{
private:
  int k;
  int n;
  int idx;
  char* store;
  int numRecvMsg;
  CkCallback lib_done_callback;
  allGatherType type;
  int numHypercubeIter;
  bool HypercubeRecursiveDoubling{};
  int iter;
  int HypercubeToSend;
  std::vector<std::vector<int>> graph{};
  std::map<int, bool> recvFloodMsg{};
  int randCounter;
  std::vector<int> hyperCubeIndx{};
  std::vector<CkNcpyBuffer> hyperCubeStore{};
  allGatherMsg* msg;
  char* data;
  CkCallback zero_copy_callback;
  // This element's buffer; every other element gets from it, so the runtime
  // must not deregister it after the first transfer (CK_BUFFER_NODEREG).
  CkNcpyBuffer src;

public:
  AllGather_SDAG_CODE

  AllGather(int k, int type, int seed);

  void startGather();

  void recvRing(int sender, CkNcpyBuffer data);

  void local_buff_done(CkDataMsg* m);

  int gen_rand();

  void Flood(int sender, CkNcpyBuffer data);

  void init(void* result, void* data, int idx, CkCallback cb);
};
