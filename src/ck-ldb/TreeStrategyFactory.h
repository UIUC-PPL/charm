// Author: Juan Galvez <jjgalvez@illinois.edu>

#ifndef TREESTRATEGYFACTORY_H
#define TREESTRATEGYFACTORY_H

#include <type_traits>

#include "TreeStrategyBase.h"
#include "greedy.h"
#include "orb.h"
#include "refine.h"

namespace TreeStrategy
{
// Strategies must be added here to be usable.
// The name must match exactly the name of the class and will be the string that is used
// to specify it in the config file.
// The second parameter is whether or not the constructor takes a json& config argument
// (which some strategies to accept additional parameters from the config file)
#define FOREACH_STRATEGY(STRATEGY)    \
  STRATEGY(Greedy, false, false)      \
  STRATEGY(GreedyRefine, true, false) \
  STRATEGY(RefineA, false, false)     \
  STRATEGY(RefineB, false, false)     \
  STRATEGY(Random, false, false)      \
  STRATEGY(Dummy, false, false)       \
  STRATEGY(Rotate, false, false)      \
  STRATEGY(ORB, false, true)

#define STRINGIFYLB(_name, _, __) #_name,
const auto LBNames = {FOREACH_STRATEGY(STRINGIFYLB)};

class Factory
{
public:
  // NOTE: This is the only place currently where the templates for each strategy
  // are instantiated. This means that code for any strategies that are disabled here
  // during preprocessing will not be part of the executable (because the templates
  // won't be instantiated)
  template <int N, bool R>
  static IStrategyWrapper* CreateStrategyWrapper(const std::string& name, bool isTreeRoot,
                                                 json& config)
  {
    // Only pass config if the strategy needs it
#define LBNEEDS_CONFIG(_config) LBNEEDS_CONFIG_##_config
#define LBNEEDS_CONFIG_true config
#define LBNEEDS_CONFIG_false

#define REGISTERLBANDWRAP(_name, _config, _position)                            \
  if (name == (#_name))                                                         \
  {                                                                             \
    typedef typename std::conditional<_position, ObjPos<N>, Obj<N>>::type O;    \
    typedef Proc<N, R> P;                                                       \
    typedef typename StrategyWrapper<O, P>::Solution S;                         \
    auto strategy = new _name<O, P, S>(LBNEEDS_CONFIG(_config));                \
    return new StrategyWrapper<O, P>(strategy, name, isTreeRoot, config[name]); \
  }
    FOREACH_STRATEGY(REGISTERLBANDWRAP);

    std::string error_msg("Unrecognized strategy ");
    error_msg += name;
    CkAbort("%s\n", error_msg.c_str());
  }
};
}  // namespace TreeStrategy
#endif /* TREESTRATEGYFACTORY_H */
