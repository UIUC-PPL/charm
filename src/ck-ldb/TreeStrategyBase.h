#ifndef TREESTRATEGYBASE_H
#define TREESTRATEGYBASE_H

#include <algorithm>
#include <random>

namespace TreeStrategy
{
template <typename T, bool is_ptr = std::is_pointer<T>::value>
struct CmpLoadGreater
{
};

template <typename T>
struct CmpLoadGreater<T, true>
{
  inline bool operator()(const T a, const T b) const
  {
    return (a->getLoad() > b->getLoad());
  }
};

template <typename T>
struct CmpLoadGreater<T, false>
{
  inline bool operator()(const T& a, const T& b) const
  {
    return (a.getLoad() > b.getLoad());
  }
};

template <typename T, bool is_ptr = std::is_pointer<T>::value>
struct CmpLoadLess
{
};

template <typename T>
struct CmpLoadLess<T, true>
{
  inline bool operator()(const T a, const T b) const
  {
    return (a->getLoad() < b->getLoad());
  }
};

template <typename T>
struct CmpLoadLess<T, false>
{
  inline bool operator()(const T& a, const T& b) const
  {
    return (a.getLoad() < b.getLoad());
  }
};

template <typename T, bool is_ptr = std::is_pointer<T>::value>
struct CmpId
{
};

template <typename T>
struct CmpId<T, true>
{
  inline bool operator()(const T a, const T b) const { return (a->id < b->id); }
};

template <typename T>
struct CmpId<T, false>
{
  inline bool operator()(const T& a, const T& b) const { return (a.id < b.id); }
};

template <typename T>
T* ptr(T& obj)
{
  return &obj;
}  // turn reference into pointer

template <typename T>
T* ptr(T* obj)
{
  return obj;
}  // obj is already pointer, return it

// ---------------- Obj --------------------

struct obj_1_data
{
  float load;
};
template <int N>
struct obj_N_data
{
  float totalLoad;
  float load[N];
  float maxload;
};

template <int N, bool multi = (N > 1)>
class Obj : public std::conditional<multi, obj_N_data<N>, obj_1_data>::type
{
 public:
  int id;
  int oldPe;

  static constexpr auto dimension = N;

  inline void populate(int _id, float* _load, int _oldPe)
  {
    id = _id;
    oldPe = _oldPe;
    this->maxload = 0;
    // The zeroth element of _load will be the traditional walltime load,
    //  and then 1 through N will be the elements of the vector load
    this->totalLoad = *_load;
    _load++;
    for (int i = 0; i < N; i++)
    {
      this->load[i] = _load[i];
      if (this->load[i] > this->maxload) this->maxload = this->load[i];
    }
  }

  inline float getLoad() const { return this->maxload; }
  inline float getLoad(int dim) const
  {
    CkAssert(dim < dimension);
    return this->load[dim];
  }
};

template <>
inline void Obj<1>::populate(int _id, float* _load, int _oldPe)
{
  id = _id;
  load = *_load;
  oldPe = _oldPe;
}

template <>
inline float Obj<1>::getLoad() const
{
  return load;
}

template <>
inline float Obj<1>::getLoad(int) const
{
  return getLoad();
}

// ------------------ Proc ------------------

/**
 * class Proc<int N, bool rateAware>
 * This just shows the interface. only the specializations (below) are defined
 * N: number of dimensions of vector load
 * rateAware: true if speed aware, false otherwise
 */
template <int N, bool rateAware, bool multi = (N > 1)>
class Proc
{
 public:
  void populate(int _id, float* _bgload, float* _speed);
  float getLoad() const;         // returns current load of processor
  float getLoad(int dim) const;  // returns current load of processor
  void assign(const Obj<N>* o);  // add object loads to this processor's loads
  void assign(const Obj<N>& o);  // add object loads to this processor's loads
  void resetLoad();              // sets processor loads to background loads
};

template <int N>
struct proc_N_data
{
  float load[N] = {0};
  float bgload[N] = {0};
  float maxload = 0;
};
struct proc_1_data
{
  float load = 0;
  float bgload = 0;
};

// --------- Proc rateAware=false specializations ---------

template <int N, bool multi>
class Proc<N, false, multi>
    : public std::conditional<multi, proc_N_data<N>, proc_1_data>::type
{
 public:
  int id = -1;
  static constexpr auto dimension = N;

  inline void populate(int _id, float* _bgload, float* _speed)
  {
    id = _id;
    // TODO: implement vector bgload
    //std::copy_n(_bgload, N, this->bgload);
  }

  inline float getLoad() const { return this->maxload; }

  inline float getLoad(int dim) const
  {
    CkAssert(dim < dimension);
    return this->load[dim];
  }

  inline void assign(const Obj<N>* o)
  {
    for (int i = 0; i < N; i++)
    {
      this->load[i] += o->load[i];
      if (this->load[i] > this->maxload) this->maxload = this->load[i];
    }
  }
  inline void assign(const Obj<N>& o) { assign(&o); }

  inline void resetLoad()
  {
    this->maxload = 0;
    for (int i = 0; i < N; i++)
    {
      this->load[i] = this->bgload[i];
      if (this->load[i] > this->maxload) this->maxload = this->load[i];
    }
  }
};

template <>
void Proc<1, false>::populate(int _id, float* _bgload, float* _speed)
{
  id = _id;
  this->bgload = *_bgload;
}
template <>
float Proc<1, false>::getLoad() const
{
  return this->load;
}
template <>
float Proc<1, false>::getLoad(int) const
{
  return getLoad();
}
template <>
void Proc<1, false>::assign(const Obj<1>* o)
{
  this->load += o->load;
}
template <>
void Proc<1, false>::resetLoad()
{
  this->load = this->bgload;
}

// --------- Proc rateAware=true specializations ---------

// TODO further specialize for N=1 by having speed not be an array?

template <int N, bool multi>
class Proc<N, true, multi>
    : public std::conditional<multi, proc_N_data<N>, proc_1_data>::type
{
 public:
  int id = -1;
  static constexpr auto dimension = N;
  float speed[N] = {1.0};

  inline void populate(int _id, float* _bgload, float* _speed)
  {
    id = _id;
    std::copy_n(_bgload, N, this->bgload);
    std::copy_n(_speed, N, this->speed);
  }

  inline float getLoad() const { return this->maxload; }

  inline float getLoad(int dim) const
  {
    CkAssert(dim < dimension);
    return this->load[dim];
  }

  inline void assign(const Obj<N>* o)
  {
    for (int i = 0; i < N; i++)
    {
      this->load[i] += (o->load[i] / speed[i]);
      if (this->load[i] > this->maxload) this->maxload = this->load[i];
    }
  }
  inline void assign(const Obj<N>& o) { assign(&o); }

  inline void resetLoad()
  {
    this->maxload = 0;
    for (int i = 0; i < N; i++)
    {
      this->load[i] = this->bgload[i];
      if (this->load[i] > this->maxload) this->maxload = this->load[i];
    }
  }
};

template <>
void Proc<1, true>::populate(int _id, float* _bgload, float* _speed)
{
  id = _id;
  this->bgload = *_bgload;
  speed[0] = _speed[0];
}
template <>
float Proc<1, true>::getLoad() const
{
  return this->load;
}
template <>
float Proc<1, true>::getLoad(int) const
{
  return getLoad();
}
template <>
void Proc<1, true>::assign(const Obj<1>* o)
{
  this->load += (o->load / speed[0]);
}
template <>
void Proc<1, true>::resetLoad()
{
  this->load = this->bgload;
}

// ---------------- Strategy --------------------

template <typename O, typename P, typename S>
class Strategy
{
 public:
  virtual void solve(std::vector<O>& objs, std::vector<P>& procs, S& solution,
                     bool objsSorted = false) = 0;
  virtual ~Strategy() {}
};

template <typename O, typename P, typename S>
class Random : public Strategy<O, P, S>
{
 public:
  Random()
  {
    std::random_device rd;
    rng = std::mt19937(rd());
  }
  void solve(std::vector<O>& objs, std::vector<P>& procs, S& solution, bool objsSorted)
  {
    std::uniform_int_distribution<int> uni(0, procs.size() - 1);
    for (const auto& o : objs) solution.assign(o, procs[uni(rng)]);
  }

 private:
  std::mt19937 rng;
};

template <typename O, typename P, typename S>
class Dummy : public Strategy<O, P, S>
{
 public:
  void solve(std::vector<O>& objs, std::vector<P>& procs, S& solution, bool objsSorted) {
  }  // do nothing
};

template <typename O, typename P, typename S>
class Rotate : public Strategy<O, P, S>
{
 public:
  void solve(std::vector<O>& objs, std::vector<P>& procs, S& solution, bool objsSorted)
  {
    std::sort(procs.begin(), procs.end(), CmpId<P>());
    // could use unordered_map but vector is faster and doesn't require much memory, even
    // for a few million PEs
    std::vector<int> procMap(CkNumPes(), -1);  // real pe -> idx in procs
    for (int i = 0; i < procs.size(); i++) procMap[procs[i].id] = i;
    for (const auto& o : objs)
    {
      if (procMap[o.oldPe] == -1) CkAbort("Rotate does not support foreign objects\n");
      P& p = procs[(procMap[o.oldPe] + 1) % procs.size()];
      solution.assign(o, p);
      // fprintf(stderr, "Moving object %d from PE %d to %d\n", o.id, o.oldPe, p.id);
    }
  }
};

}  // namespace TreeStrategy

#endif /* TREESTRATEGYBASE_H */
