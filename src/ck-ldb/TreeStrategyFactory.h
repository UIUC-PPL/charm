#ifndef TREESTRATEGYFACTORY_H
#define TREESTRATEGYFACTORY_H

#include "TreeStrategyBase.h"
#include "greedy.h"
#include "kd.h"
#include "refine.h"

namespace TreeStrategy
{
// Strategies must be added here to be usable.
// The name must match exactly the name of the class and will be the string that is used
// to specify it in the config file.
// The second parameter is whether or not the constructor takes a json& config argument
// (which some strategies to accept additional parameters from the config file)
#define FOREACH_STRATEGY(STRATEGY) \
  STRATEGY(GreedyNorm, true)       \
  STRATEGY(Greedy, false)          \
  STRATEGY(GreedyRefine, true)     \
  STRATEGY(RefineA, false)         \
  STRATEGY(RefineB, false)         \
  STRATEGY(Random, false)          \
  STRATEGY(Dummy, false)           \
  STRATEGY(Rotate, false)          \
  STRATEGY(Kd, false)

#define STRINGIFYLB(_name, _) #_name,
const auto LBNames = {FOREACH_STRATEGY(STRINGIFYLB)};

class Factory
{
public:
  // NOTE: This is the only place currently where the templates for each strategy
  // are instantiated. This means that code for any strategies that are disabled here
  // during preprocessing will not be part of the executable (because the templates
  // won't be instantiated)
  template <class O, class P, class S>
  static Strategy<O, P, S>* makeStrategy(const std::string& name, json& config)
  {
    // Only pass config if the strategy needs it
#define LBNEEDS_CONFIG(_config) LBNEEDS_CONFIG_##_config
#define LBNEEDS_CONFIG_true config
#define LBNEEDS_CONFIG_false

#define REGISTERLB(_name, _config) \
  if (name == (#_name)) return new _name<O, P, S>(LBNEEDS_CONFIG(_config));
    FOREACH_STRATEGY(REGISTERLB);

    std::string error_msg("Unrecognized strategy ");
    error_msg += name;
    CkAbort("%s\n", error_msg.c_str());
  }
};
}  // namespace TreeStrategy
#endif /* TREESTRATEGYFACTORY_H */
