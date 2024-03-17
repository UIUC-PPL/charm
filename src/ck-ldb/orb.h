#ifndef ORB_H
#define ORB_H

#include "TreeStrategyBase.h"

#include <vector>

namespace TreeStrategy
{
template <typename O, typename P, typename S>
class ORB : public Strategy<O, P, S>
{
private:
  struct Bounds
  {
    float lower, upper;
    Bounds() = default;
    Bounds(float lower, float upper) : lower(lower), upper(upper) {}
  };

  enum class Side
  {
    Left,
    Right,
    Invalid
  };

  std::vector<Side> objSide;

  using Box = std::vector<Bounds>;
  using ProcIter = typename std::vector<P>::iterator;
  using Indices = std::vector<int>;

  void partition(const std::vector<O>& objs, ProcIter procStart, ProcIter procEnd,
                 S& solution, std::vector<Indices>& sortedIndices, const Box& box)
  {
    // Note that objs is always the vector of all the objects and objs.size() != numObjs.
    // numObjs is the number of objects in the current subset, which corresponds to the
    // size of the entries in sortedIndices.
    const auto numObjs = sortedIndices[0].size();
    // If there are no more objects left, then they've all gone to other partitions
    if (numObjs == 0)
      return;

    const int numProcs = std::distance(procStart, procEnd);
    // Only one PE in our subset, assign all of our objects there or only one
    // obj in our subset, assign it to first proc
    if (numProcs == 1 || numObjs == 1)
    {
      P& p = *procStart;
      for (int index : sortedIndices[0])
      {
        solution.assign(objs[index], p);
      }
      return;
    }

    const int posDimension = box.size();

    // Find longest dimension
    float maxDiff = 0;
    int maxDim = 0;
    for (int i = 0; i < posDimension; i++)
    {
      const float diff = box[i].upper - box[i].lower;
      if (diff > maxDiff)
      {
        maxDiff = diff;
        maxDim = i;
      }
    }

    // Decide where to split along the longest dimension
    const int numLeftProcs = numProcs / 2;
    const float bgLeft =
        std::accumulate(procStart, procStart + numLeftProcs, 0.0,
                        [&](float val, const P& proc) { return val + proc.getLoad(); });
    const float bgRight =
        std::accumulate(procStart + numLeftProcs, procEnd, 0.0,
                        [&](float val, const P& proc) { return val + proc.getLoad(); });
    const float ratio = (1.0F * numLeftProcs) / numProcs;
    // splitIndex is the index in sortedIndices[maxDim] of the first object
    // going to the right partition. Note that this can be equal to numObjs if
    // all objs are going to the left partition, which we correct for below.
    const auto splitIndex = findSplit(objs, sortedIndices[maxDim], ratio, bgLeft, bgRight);

    // Now actually split into two sets
    // First, split the box
    Box leftBox(box);
    Box rightBox(box);
    const float splitPosition = objs[sortedIndices[maxDim][std::min(numObjs - 1, splitIndex)]].position[maxDim];
    leftBox[maxDim].upper = splitPosition;
    rightBox[maxDim].lower = splitPosition;

    // Store decision for each object in index -> side table
    for (int i = 0; i < numObjs; i++)
    {
      const int objIndex = sortedIndices[maxDim][i];
      CkAssert(objSide[objIndex] == Side::Invalid);
      objSide[objIndex] = (i < splitIndex) ? Side::Left : Side::Right;
    }

    // Create new index collections for each half, using the just filled table
    std::vector<Indices> leftIndices(posDimension);
    std::vector<Indices> rightIndices(posDimension);
    for (int i = 0; i < posDimension; i++)
    {
      leftIndices[i].reserve(splitIndex);
      rightIndices[i].reserve(numObjs - splitIndex);
      for (int val : sortedIndices[i])
      {
        CkAssert(objSide[val] != Side::Invalid);
        if (objSide[val] == Side::Left)
          leftIndices[i].push_back(val);
        else
          rightIndices[i].push_back(val);
      }
    }

    // Cleanup the table
    for (int val : sortedIndices[0])
    {
      CkAssert(objSide[val] != Side::Invalid);
      objSide[val] = Side::Invalid;
    }

    // Free the current index collection
    for (auto& index : sortedIndices) Indices().swap(index);

    // Recurse on the two halves
    partition(objs, procStart, procStart + numLeftProcs, solution, leftIndices, leftBox);
    partition(objs, procStart + numLeftProcs, procEnd, solution, rightIndices, rightBox);
  }

  size_t findSplit(const std::vector<O>& objs, const Indices& sortedPositions,
                   const float ratio, const float bgLeft, const float bgRight) const
  {
    const float approxBgPerObj = (bgLeft + bgRight) / sortedPositions.size();
    // Total load is the bg load of left procs + bg load of right procs + load of objects
    const float totalLoad =
        bgLeft + bgRight +
        std::accumulate(sortedPositions.begin(), sortedPositions.end(), 0.0,
                        [&](float l, int index) { return l + objs[index].getLoad(); });

    // leftTarget is the amount of object load we want to assign to the left procs
    const float leftTarget = ratio * totalLoad;
    size_t splitIndex = 0;
    float leftLoad = 0;
    float nextLeftLoad = 0;
    for (splitIndex = 0; splitIndex < sortedPositions.size(); splitIndex++)
    {
      nextLeftLoad += objs[sortedPositions[splitIndex]].getLoad() + approxBgPerObj;
      if (nextLeftLoad > leftTarget)
      {
        // Decide if split element should go to left or right partition
        if (std::abs(nextLeftLoad - leftTarget) < std::abs(leftLoad - leftTarget))
          splitIndex++;
        break;
      }
      leftLoad = nextLeftLoad;
    }

    return splitIndex;
  }

public:
  void solve(std::vector<O>& objs, std::vector<P>& procs, S& solution, bool objsSorted)
  {
    static_assert(O::isPosition, "ORB must be used with position objects");
    CkAssert(!objs.empty());
    CkAssertMsg(!objs[0].position.empty(),
                "Objects used with ORBLB must have a valid, non-empty position.");

    // Sort processors by ID
    std::sort(procs.begin(), procs.end(), CmpId<P>());

    // Assumes all objects have same position dimension
    const auto posDimension = objs[0].position.size();

    // Create a list of sorted indices into the objs vector, one per positional dimension
    // i.e. sortedIndices[0] will contain object indices sorted by the x dimension of
    // their position, [1] by y, and so on.
    std::vector<Indices> sortedIndices(posDimension);
    for (int i = 0; i < posDimension; i++)
    {
      Indices& current = sortedIndices[i];
      current.resize(objs.size());
      std::iota(current.begin(), current.end(), 0);
      std::sort(current.begin(), current.end(),
                [&](int a, int b) { return objs[a].position[i] < objs[b].position[i]; });
    }

    // Create a bounding box for the objects
    Box box(posDimension);
    for (int i = 0; i < posDimension; i++)
    {
      box[i] = {objs[sortedIndices[i][0]].position[i],
                objs[sortedIndices[i][objs.size() - 1]].position[i]};
    }

    // Cleanup helper vector for fresh run
    objSide.clear();
    objSide.resize(objs.size(), Side::Invalid);

    // Now that the prep work is done, actually do the partitioning.
    // This call will internally set the mapping in the solution.
    partition(objs, procs.begin(), procs.end(), solution, sortedIndices, box);
  }
};
}  // namespace TreeStrategy

#endif /* ORB_H */
