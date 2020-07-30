#ifndef PROC_HEAP_H
#define PROC_HEAP_H

#include <vector>

// custom heap to allow removal of processors from any position
template <typename P>
class ProcHeap
{
 public:
  ProcHeap(std::vector<P>& procs, int dimension = 0)
  {
    this->dimension = dimension;
    int index = 1;
    Q.resize(procs.size() + 1);
    elem_pos.resize(CkNumPes(), 0);
    for (auto& p : procs)
    {
      Q[index] = p;
      elem_pos[ptr(p)->id] = index;
      index++;
    }
    buildMinHeap();
  }

  // IMPORTANT: right now, if procId refers to a processor that is *not* in the heap,
  // elem_pos[procId] refers to position 0, which should contain an invalid processor
  inline P& getProc(int procId) { return Q[elem_pos[procId]]; }

  inline P& top()
  {
    CkAssert(Q.size() > 1);
    return Q[1];
  }

  inline void push(P& p)
  {
    Q.push_back(p);
    int pos = Q.size() - 1;
    elem_pos[ptr(p)->id] = pos;
    siftUp(pos);
  }

  void pop()
  {
    CkAssert(Q.size() > 1);
    if (Q.size() == 2)
    {
      Q.pop_back();
    }
    else
    {
      Q[1] = Q.back();
      Q.pop_back();
      elem_pos[ptr(Q[1])->id] = 1;
      siftDown(1);
    }
  }

  // remove processor from any position in the heap
  void remove(P& p)
  {
    int pos = elem_pos[ptr(p)->id];
    if ((Q.size() == 2) || (pos == Q.size() - 1)) return Q.pop_back();
    if (pos == 1) return pop();
    Q[pos] = Q.back();
    Q.pop_back();
    elem_pos[ptr(Q[pos])->id] = pos;
    if (ptr(Q[pos / 2])->getLoad(dimension) > ptr(Q[pos])->getLoad(dimension))
      siftUp(pos);
    else
      siftDown(pos);
  }

  // Assumes that load will only ever increase when updating and that this will
  // always be a minheap
  void update(P& p)
  {
    const int pos = elem_pos[ptr(p)->id];
    min_heapify(pos);
  }
  /*void clear() {
    Q.clear();
    Q.emplace_back();
  }*/

 private:
  void min_heapify(int i)
  {
    const int left = 2 * i;
    const int right = 2 * i + 1;
    int smallest = i;
    if ((left < Q.size()) && (ptr(Q[left])->getLoad(dimension)) < ptr(Q[smallest])->getLoad(dimension))
      smallest = left;
    if ((right < Q.size()) && (ptr(Q[right])->getLoad(dimension)) < ptr(Q[smallest])->getLoad(dimension))
      smallest = right;
    if (smallest != i)
    {
      // swap(i, smallest);
      std::swap(Q[i], Q[smallest]);
      elem_pos[ptr(Q[i])->id] = i;
      elem_pos[ptr(Q[smallest])->id] = smallest;
      min_heapify(smallest);
    }
  }

  void inline buildMinHeap()
  {
    for (int i = Q.size() / 2; i > 0; i--) min_heapify(i);
  }

  void siftUp(int pos)
  {
    if (pos == 1) return;  // reached root
    int ppos = pos / 2;
    if (ptr(Q[ppos])->getLoad(dimension) > ptr(Q[pos])->getLoad(dimension))
    {
      std::swap(Q[ppos], Q[pos]);
      elem_pos[ptr(Q[ppos])->id] = ppos;
      elem_pos[ptr(Q[pos])->id] = pos;
      siftUp(ppos);
    }
  }

  inline int minChild(int pos) const
  {
    int c1 = pos * 2;
    int c2 = pos * 2 + 1;
    if (c1 >= Q.size()) return -1;
    if (c2 >= Q.size()) return c1;
    if (ptr(Q[c1])->getLoad(dimension) < ptr(Q[c2])->getLoad(dimension))
      return c1;
    else
      return c2;
  }

  void siftDown(int pos)
  {
    int cpos = minChild(pos);
    if (cpos == -1) return;
    if (ptr(Q[pos])->getLoad(dimension) > ptr(Q[cpos])->getLoad(dimension))
    {
      std::swap(Q[pos], Q[cpos]);
      elem_pos[ptr(Q[cpos])->id] = cpos;
      elem_pos[ptr(Q[pos])->id] = pos;
      siftDown(cpos);
    }
  }

  std::vector<P> Q;
  std::vector<int> elem_pos;
  int dimension;
};

#endif /* PROC_HEAP_H */
