#ifndef KDNODE_H
#define KDNODE_H

#include <array>
#include <limits>
#include <random>
#include <stack>
#include <utility>

using KDFloatType = LoadFloatType;

template <typename TreeType, typename Elem, int N=Elem::dimension>
class BaseKDNode
{
protected:
  Elem data;
  unsigned int size = 1;
  TreeType* left = nullptr;
  TreeType* right = nullptr;
  KDFloatType norm;

  BaseKDNode(const Elem& key) : data(key), norm(calcNorm(data)) {}

  template <typename T>
  static KDFloatType calcNorm(const T& x)
  {
    KDFloatType sum = 0;
    for (int i = 0; i < N; i++)
    {
      const auto element = x[i];
      /*
      const auto elementSq = element * element;
      sum += elementSq * elementSq;
      */
      sum += element * element;
    }
    return sum;
  }

  template <typename A, typename B>
  static KDFloatType calcNorm(const A& a, const B& b)
  {
    KDFloatType sum = 0;
    for (int i = 0; i < N; i++)
    {
      const auto element = a[i] + b[i];
      /*
      const auto elementSq = element * element;
      sum += elementSq * elementSq;
      */
      sum += element * element;
    }
    return sum;
  }

  template <typename A, typename B>
  static std::array<KDFloatType, N> addVecs(const A& a, const B& b)
  {
    std::array<KDFloatType, N> sum = {0};
    for (int i = 0; i < N; i++)
    {
      sum[i] = a[i] + b[i];
    }

    return sum;
  }
};


template <typename Elem, int N = Elem::dimension,
          typename Engine = std::default_random_engine>
class RKDNode : BaseKDNode<RKDNode<Elem, N, Engine>, Elem>
{
  using base = BaseKDNode<RKDNode<Elem, N, Engine>, Elem>;
  using rkdt = RKDNode*;
  using discr_t = std::uint_fast8_t;
private:
  discr_t discr;

  static int random(int min, int max)
  {
    static Engine rng;
    return std::uniform_int_distribution<int>(min, max)(rng);
  }

  static void perturb(Elem& x, int dim,
                      const std::array<std::pair<KDFloatType, KDFloatType>, N>& bounds)
  {
    static int count = 0;
    static Engine rng;
    if (x[dim] == 0)
    {
      // min returns the minimum positive normalized value for floating-point types
      x[dim] = 1e6 * std::numeric_limits<KDFloatType>::min();
    }
    const auto maxVal = std::min(x[dim] * 1.01f, bounds[dim].second);
    const auto minVal = std::max(x[dim] * 0.99f, bounds[dim].first) + std::numeric_limits<KDFloatType>::min();
    x[dim] = std::uniform_real_distribution<KDFloatType>(minVal, maxVal)(rng);
  }

public:
  RKDNode(const Elem& key, const discr_t discr = random(0, N - 1)) : base(key), discr(discr)
  {
  }

  static rkdt insert(rkdt t, Elem& x)
  {
    std::array<std::pair<KDFloatType, KDFloatType>, N> bounds;
    for (int i = 0; i < N; i++)
    {
      bounds[i] = {0, std::numeric_limits<KDFloatType>::max()};
    }
    std::stack<std::pair<rkdt, bool>> stack;
    while (t != nullptr && random(0, t->size) > 0)
    {
      t->size++;
      const auto i = t->discr;
      if (x[i] == t->data[i]) perturb(x, i, bounds);
      const bool isLeftChild = x[i] < t->data[i];
      stack.emplace(t, isLeftChild);
      if (isLeftChild)
      {
        bounds[i].second = t->data[i];
        t = t->left;
      }
      else
      {
        bounds[i].first = t->data[i];
        t = t->right;
      }
    }

    int maxDiffIndex = 0;
    KDFloatType maxDiff = bounds[0].second - bounds[0].first;
    for (int i = 1; i < N; i++)
    {
      const auto diff = bounds[i].second - bounds[i].first;
      if (diff > maxDiff)
      {
        maxDiffIndex = i;
        maxDiff = diff;
      }
    }

    // If we're not actually fully bound, then just randomly choose index
    if (bounds[maxDiffIndex].first == 0 ||
        bounds[maxDiffIndex].second == std::numeric_limits<KDFloatType>::max())
      maxDiffIndex = random(0, N - 1);

    if (t != nullptr && t->data[maxDiffIndex] == x[maxDiffIndex])
    {
      perturb(x, maxDiffIndex, bounds);
      t = insert_at_root(t, x, maxDiffIndex);
    }
    else
      t = insert_at_root(t, x, maxDiffIndex);

    while (!stack.empty())
    {
      const std::pair<rkdt, bool>& entry = stack.top();
      const rkdt parent = entry.first;
      const bool isLeftChild = entry.second;
      if (isLeftChild)
	parent->left = t;
      else
	parent->right = t;
      t = parent;
      stack.pop();
    }
    return t;
  }

  static rkdt remove(rkdt t, const Elem& x)
  {
    if (t == nullptr) return nullptr;
    const auto i = t->discr;
    if (t->data == x)
    {
      const auto newRoot = join(t->left, t->right, i);
      delete t;
      return newRoot;
    }
    t->size--;
    if (x[i] < t->data[i])
      t->left = remove(t->left, x);
    else
      t->right = remove(t->right, x);
    return t;
  }

  static rkdt insert_at_root(rkdt t, const Elem& x, const discr_t discr)
  {
    rkdt r;
    r = new RKDNode(x, discr);
    if (t != nullptr) r->size += t->size;
    auto p = split(t, r);
    r->left = p.first;
    r->right = p.second;
    return r;
  }

  // This splits t's subtrees at whatever r specifies
  // t might be nullptr, r cannot be
  static std::pair<rkdt, rkdt> split(rkdt t, const rkdt r)
  {
    if (t == nullptr) return std::make_pair(nullptr, nullptr);
    const auto i = r->discr;
    const auto j = t->discr;

    // t and r discriminate in the same dimension, so just resplit the appropriate subtree
    // of t
    if (i == j)
    {
      if (r->data[i] <= t->data[i])
      {
        const auto p = split(t->left, r);
        t->left = p.second;
        const int splitSize = (p.first == nullptr) ? 0 : p.first->size;
        t->size -= splitSize;
        return std::make_pair(p.first, t);
      }
      else
      {
        const auto p = split(t->right, r);
        t->right = p.first;
        const int splitSize = (p.second == nullptr) ? 0 : p.second->size;
        t->size -= splitSize;
        return std::make_pair(t, p.second);
      }
    }
    // t and r discriminate on different dimensions, so recursively split both subtrees of
    // t
    else
    {
      const auto L = split(t->left, r);
      const auto R = split(t->right, r);
      if (r->data[i] <= t->data[i])
      {
        t->left = L.second;
        t->right = R.second;
        const int splitSize = ((L.first == nullptr) ? 0 : L.first->size) +
                              ((R.first == nullptr) ? 0 : R.first->size);
        t->size -= splitSize;

        return std::make_pair(join(L.first, R.first, j), t);
      }
      else
      {
        t->left = L.first;
        t->right = R.first;
        const int splitSize = ((L.second == nullptr) ? 0 : L.second->size) +
                              ((R.second == nullptr) ? 0 : R.second->size);
        t->size -= splitSize;

        return std::make_pair(t, join(L.second, R.second, j));
      }
    }
  }

  static rkdt join(rkdt l, rkdt r, const discr_t dim)
  {
    if (l == nullptr) return r;
    if (r == nullptr) return l;

    const int m = l->size;
    const int n = r->size;
    const int u = random(0, m + n - 1);
    if (u < m)
    {
      l->size += r->size;
      if (l->discr == dim)
      {
        l->right = join(l->right, r, dim);
        return l;
      }
      else
      {
        auto R = split(r, l);
        l->left = join(l->left, R.first, dim);
        l->right = join(l->right, R.second, dim);
        return l;
      }
    }
    else
    {
      r->size += l->size;
      if (r->discr == dim)
      {
        r->left = join(l, r->left, dim);
        return r;
      }
      else
      {
        auto L = split(l, r);
        r->left = join(L.first, r->left, dim);
        r->right = join(L.second, r->right, dim);
        return r;
      }
    }
  }

  template <typename T>
  static Elem* findMinNorm(rkdt t, const T& x)
  {
    std::array<KDFloatType, N> mins = {0};
    KDFloatType bestNorm = std::numeric_limits<KDFloatType>::max();
    const auto objNorm = base::calcNorm(x);
    return findMinNormHelper(t, x, nullptr, objNorm, bestNorm, mins);
  }

private:
  template <typename T>
  static Elem* findMinNormHelper(rkdt t, const T& x, Elem* bestObj, const KDFloatType objNorm, KDFloatType& bestNorm,
                                 std::array<KDFloatType, N>& minBounds)
  {
    if (t->left != nullptr)
    {
      bestObj = findMinNormHelper(t->left, x, bestObj, objNorm, bestNorm, minBounds);
    }
    if (t->norm + objNorm < bestNorm)
    {
      const auto rootNorm = base::calcNorm(x, t->data);
      if (rootNorm < bestNorm)
      {
        bestObj = &(t->data);
        bestNorm = rootNorm;
      }
    }
    if (t->right != nullptr)
    {
      const auto dim = t->discr;
      const auto oldMin = minBounds[dim];
      minBounds[dim] = t->data[dim];
      if (base::calcNorm(x, minBounds) < bestNorm)
      {
        bestObj = findMinNormHelper(t->right, x, bestObj, objNorm, bestNorm, minBounds);
      }
      minBounds[dim] = oldMin;
    }

    return bestObj;
  }

  static Elem findMin(rkdt t, unsigned int targetDim)
  {
    assert(t != nullptr);
    const auto dim = t->discr;
    if (dim == targetDim)
    {
      if (t->left == nullptr)
        return t->data;
      else
        return findMin(t->left, targetDim);
    }
    else
    {
      auto obj = t->data;
      if (t->left != nullptr)
      {
        const auto left = findMin(t->left, targetDim);
        if (left[targetDim] < obj[targetDim]) obj = left;
      }
      if (t->right != nullptr)
      {
        const auto right = findMin(t->right, targetDim);
        if (right[targetDim] < obj[targetDim]) obj = right;
      }
      return obj;
    }
  }
};

#endif /* KDNODE_H */
