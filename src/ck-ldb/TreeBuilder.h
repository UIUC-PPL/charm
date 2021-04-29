#ifndef TREEBUILDER_H
#define TREEBUILDER_H

#include "TreeLB.h"
#include "TreeLevel.h"
#include "spanningTree.h"
#include <algorithm>

extern int quietModeRequested;

class LBTreeBuilder
{
public:
  virtual uint8_t build(std::vector<LevelLogic*>& logic, std::vector<int>& comm_parent,
                        std::vector<std::vector<int>>& comm_children,
                        std::vector<LevelLogic*>& comm_logic, json& config) = 0;

  virtual ~LBTreeBuilder() {}

  int getProperty(const char* property, const int defaultValue, const json& config) const
  {
    return getPropertyHelper(property, defaultValue, config, &json::is_number_integer);
  }

  bool getProperty(const char* property, const bool defaultValue,
                   const json& config) const
  {
    return getPropertyHelper(property, defaultValue, config, &json::is_boolean);
  }

  std::vector<std::string> getProperty(const char* property,
                                       const std::vector<std::string> defaultValue,
                                       const json& config) const
  {
    return getPropertyHelper(property, defaultValue, config, &json::is_array);
  }

protected:
  int rootPE = 0;
  bool useCommMsgs,useCommBytes;

  void loadCommConfig(const json& config)
  {
    useCommMsgs = getProperty("use_comm_msgs", false, config);
    useCommBytes = getProperty("use_comm_bytes", false, config);
  }

  void reset(uint8_t num_levels, std::vector<LevelLogic*>& logic,
             std::vector<int>& comm_parent, std::vector<std::vector<int>>& comm_children,
             std::vector<LevelLogic*>& comm_logic)
  {
    logic.resize(num_levels);
    comm_parent.resize(num_levels);
    comm_children.resize(num_levels);
    comm_logic.resize(num_levels);

    std::fill(logic.begin(), logic.end(), nullptr);
    std::fill(comm_parent.begin(), comm_parent.end(), -1);
    for (auto& children : comm_children) children.clear();
    std::fill(comm_logic.begin(), comm_logic.end(), nullptr);
  }

private:
  template <typename T>
  T getPropertyHelper(const char* property, const T defaultValue, const json& config,
                      bool (json::*checkFn)() const) const
  {
    if (config.contains(property))
    {
      const auto& value = config.at(property);
      if (!(value.*checkFn)())
        CkAbort("TreeLB: Given value \"%s\" for %s is not of type %s.\n",
                value.dump().c_str(), property, typeid(T).name());

      return value.get<T>();
    }

    return defaultValue;
  }
};

class PE_Root_Tree : public LBTreeBuilder
{
private:
  std::vector<std::string> strategies;
  bool repeat_strategies;

public:
  PE_Root_Tree(json& config)
  {
    loadCommConfig(config);
    if (!config.contains("root"))
      CkAbort("TreeLB: Configuration must include \"root\" section.\n");
    const auto& root_config = config.at("root");

    rootPE = getProperty("pe", 0, root_config);
    if (rootPE < 0 || rootPE >= CkNumPes())
      CkAbort("TreeLB: Root PE %d is not a valid PE.\n", rootPE);

    repeat_strategies = getProperty("repeat_strategies", false, root_config);

    strategies = getProperty("strategies", std::vector<std::string>(), root_config);
    if (strategies.empty())
      CkAbort(
          "TreeLB: Section \"strategies\" must exist inside \"root\" and not be "
          "empty.\n");
  }

  virtual ~PE_Root_Tree() {}

  virtual uint8_t build(std::vector<LevelLogic*>& logic, std::vector<int>& comm_parent,
                        std::vector<std::vector<int>>& comm_children,
                        std::vector<LevelLogic*>& comm_logic, json& config)
  {
    const uint8_t L = 2;  // num levels
    reset(L, logic, comm_parent, comm_children, comm_logic);

    LBManager* lbmgr = CProxy_LBManager(_lbmgr).ckLocalBranch();

    // PE level (level 0)
    logic[0] = new PELevel(lbmgr, useCommMsgs, useCommBytes);

    int parent, num_children;
    int* children;
    getPETopoTreeEdges(CkMyPe(), rootPE, NULL, -1, 4, &parent, &num_children, &children);
    comm_parent[0] = parent;
    if (num_children > 0)
    {
      comm_children[0].assign(children, children + num_children);
      free(children);
      comm_logic[0] = new MsgAggregator();
    }

    // root of tree (level 1)
    if (CkMyPe() == rootPE)
    {
      RootLevel* level = new RootLevel();
      level->configure(_lb_args.testPeSpeed(), strategies, config["root"], repeat_strategies);
      logic[1] = level;
    }

    if (CkMyPe() == 0 && !quietModeRequested)
    {
      CkPrintf("[%d] TreeLB: Using PE_Root tree with: ", CkMyPe());
      for (const auto& strategy : strategies)
      {
        CkPrintf("%s ", strategy.c_str());
      }
      CkPrintf("\n");

      if (_lb_args.debug() > 0)
      {
        CkPrintf(
            "\tUsing %d as root\n"
            "\tTest PE Speed: %s\n",
            rootPE, _lb_args.testPeSpeed() ? "true" : "false");
      }
    }

    return L;
  }
};

class PE_Node_Root_Tree : public LBTreeBuilder
{
private:
  int step_freq_lvl2;
  // Pad with spot for the PE level so that level index matches logic levels
  std::array<std::vector<std::string>, 3> strategies;
  std::array<bool, 3> repeat_strategies;

public:
  PE_Node_Root_Tree(json& config)
  {
    loadCommConfig(config);
    // Load configuration for process level
    if (!config.contains("process"))
    {
      CkAbort("TreeLB: Configuration must include \"process\" section.\n");
    }
    const auto& process_config = config.at("process");

    repeat_strategies[1] = getProperty("repeat_strategies", false, process_config);
    strategies[1] = getProperty("strategies", std::vector<std::string>(), process_config);
    if (strategies[1].empty())
      CkAbort("TreeLB: Non-empty Section \"strategies\" must exist inside \"process\".\n");

    // Load configuration for root level
    if (!config.contains("root"))
    {
      CkAbort("TreeLB: Configuration must include \"root\" section.\n");
    }
    const auto& root_config = config.at("root");

    rootPE = getProperty("pe", 0, root_config);
    if (rootPE < 0 || rootPE >= CkNumPes())
      CkAbort("TreeLB: Root PE %d is not a valid PE.\n", rootPE);

    step_freq_lvl2 = getProperty("step_freq", 1, root_config);

    repeat_strategies[2] = getProperty("repeat_strategies", false, root_config);
    strategies[2] = getProperty("strategies", std::vector<std::string>(), root_config);
    if (strategies[2].empty())
      CkAbort("TreeLB: Non-empty Section \"strategies\" must exist inside \"root\".\n");
  }

  virtual ~PE_Node_Root_Tree() {}

  virtual uint8_t build(std::vector<LevelLogic*>& logic, std::vector<int>& comm_parent,
                        std::vector<std::vector<int>>& comm_children,
                        std::vector<LevelLogic*>& comm_logic, json& config)
  {
    const uint8_t L = 3;  // num levels
    reset(L, logic, comm_parent, comm_children, comm_logic);

    const int mype = CkMyPe();
    const int mynode = CkMyNode();
    const int level1root = CkNodeFirst(mynode);
    const int level2root = rootPE;
    LBManager* lbmgr = CProxy_LBManager(_lbmgr).ckLocalBranch();

    // PE level (level 0)
    logic[0] = new PELevel(lbmgr, useCommMsgs, useCommBytes);

    // set up comm-tree between levels 0 and 1
    if (mype == level1root)
    {
      // pes in my node excluding me
      comm_children[0].resize(CkNodeSize(mynode) - 1);
      std::iota(comm_children[0].begin(), comm_children[0].end(), level1root + 1);
      comm_logic[0] = new MsgAggregator();
    }
    else
    {
      comm_parent[0] = level1root;
    }

    // node level (level 1)
    if (mype == level1root)
    {
      std::vector<int> pes_in_node(CkNodeSize(mynode));
      std::iota(pes_in_node.begin(), pes_in_node.end(), level1root);

      NodeLevel* level = new NodeLevel(lbmgr, pes_in_node);
      level->configure(_lb_args.testPeSpeed(), strategies[1], config["process"],
                       repeat_strategies[1], step_freq_lvl2);
      logic[1] = level;

      // set up comm-tree between levels 1 and 2
      std::vector<int> level1_roots(CkNumNodes());
      level1_roots[0] = level2root;  // need this for getPETopoTreeEdges

      for (int node = 0, index = 1; node < CkNumNodes(); node++)
      {
        if (CkNodeFirst(node) == level2root) continue;
        level1_roots[index++] = CkNodeFirst(node);
      }

      int parent, num_children;
      int* children;
      getPETopoTreeEdges(mype, level2root, level1_roots.data(), level1_roots.size(), 4,
                         &parent, &num_children, &children);
      comm_parent[1] = parent;
      if (num_children > 0)
      {
        comm_children[1].assign(children, children + num_children);
        free(children);
        comm_logic[1] = new MsgAggregator();
      }
    }

    // root of tree (level 2)
    if (mype == level2root)
    {
      RootLevel* level = new RootLevel();
      level->configure(_lb_args.testPeSpeed(), strategies[2], config["root"],
                       repeat_strategies[2]);
      logic[2] = level;
    }

    if (CkMyPe() == 0 && !quietModeRequested)
    {
      CkPrintf("[%d] TreeLB: Using PE_Process_Root tree with:\n", CkMyPe());

      CkPrintf("\tProcess: ");
      for (const auto& strategy : strategies[1])
      {
        CkPrintf("%s ", strategy.c_str());
      }
      CkPrintf("\n");

      CkPrintf("\tRoot: ");
      for (const auto& strategy : strategies[2])
      {
        CkPrintf("%s ", strategy.c_str());
      }
      CkPrintf("\n");

      if (_lb_args.debug() > 0)
      {
        CkPrintf(
            "\tUsing %d as root\n"
            "\tTest PE Speed: %s\n"
            "\tRoot step frequency: %d\n",
            rootPE, _lb_args.testPeSpeed() ? "true" : "false", step_freq_lvl2);
      }
    }

    return L;
  }
};

class PE_Node_NodeSet_Root_Tree : public LBTreeBuilder
{
private:
  int GroupOf(int pe) { return CkNodeOf(pe) / group_size; }
  int GroupFirstPe(int group) { return CkNodeFirst(group * group_size); }

  int num_groups, group_size, step_freq_lvl2, step_freq_lvl3;
  bool token_passing;
  
  // Pad with spot for the PE level so that level index matches logic levels
  // and don't add spot for root level since it can't have strategies
  std::array<std::vector<std::string>, 3> strategies;
  std::array<bool, 3> repeat_strategies;

public:
  PE_Node_NodeSet_Root_Tree(json& config)
  {
    loadCommConfig(config);
    // Load configuration for process level
    if (!config.contains("process"))
      CkAbort("TreeLB: Configuration must include \"process\" section.\n");
    const auto& process_config = config.at("process");

    repeat_strategies[1] = getProperty("repeat_strategies", false, process_config);
    strategies[1] = getProperty("strategies", std::vector<std::string>(), process_config);
    if (strategies[1].empty())
      CkAbort(
          "TreeLB: Non-empty Section \"strategies\" must exist inside \"process\".\n");

    // Load configuration for processgroup level
    if (!config.contains("processgroup"))
      CkAbort("TreeLB: Configuration must include \"processgroup\" section.\n");
    const auto& processgroup_config = config.at("processgroup");

    num_groups = getProperty("num_groups", 0, processgroup_config);
    if (num_groups <= 0 || num_groups > CkNumNodes())
      CkAbort("TreeLB: \"num_groups\" has an invalid value.\n");
    // NOTE to simplify things for now, assume equal number of nodes per group
    CkAssert(CkNumNodes() % num_groups == 0);
    group_size = CkNumNodes() / num_groups;

    step_freq_lvl2 = getProperty("step_freq", 1, processgroup_config);

    repeat_strategies[2] = getProperty("repeat_strategies", false, processgroup_config);
    strategies[2] =
        getProperty("strategies", std::vector<std::string>(), processgroup_config);
    if (strategies[2].empty())
      CkAbort(
          "TreeLB: Non-empty Section \"strategies\" must exist inside "
          "\"processgroup\".\n");

    // Load configuration for root level
    if (!config.contains("root"))
      CkAbort("TreeLB: Configuration must include \"root\" section.\n");
    const auto& root_config = config.at("root");

    rootPE = getProperty("pe", 0, root_config);
    if (rootPE < 0 || rootPE >= CkNumPes())
      CkAbort("TreeLB: Root PE %d is not a valid PE.\n", rootPE);

    step_freq_lvl3 = getProperty("step_freq", step_freq_lvl2, root_config);
    if (step_freq_lvl3 % step_freq_lvl2 != 0)
      CkAbort("TreeLB: step_freq of root level is not multiple of previous level.\n");

    token_passing = getProperty("token_passing", true, root_config);
    if (root_config.contains("repeat_strategies") || root_config.contains("strategies"))
      CkAbort("TreeLB: Root level of PE_Process_ProcessGroup_Root tree cannot have strategies.\n");
  }

  virtual ~PE_Node_NodeSet_Root_Tree() {}

  virtual uint8_t build(std::vector<LevelLogic*>& logic, std::vector<int>& comm_parent,
                        std::vector<std::vector<int>>& comm_children,
                        std::vector<LevelLogic*>& comm_logic, json& config)
  {
    const uint8_t L = 4;  // num levels
    reset(L, logic, comm_parent, comm_children, comm_logic);

    const int mype = CkMyPe();
    const int mynode = CkMyNode();
    const int mygroup = GroupOf(mype);
    const int level1root = CkNodeFirst(mynode);
    const int level2root = GroupFirstPe(mygroup);
    const int level3root = rootPE;
    LBManager* lbmgr = CProxy_LBManager(_lbmgr).ckLocalBranch();

    // PE level (level 0)
    logic[0] = new PELevel(lbmgr, useCommMsgs, useCommBytes);

    // set up comm-tree between levels 0 and 1
    if (mype == level1root)
    {
      // pes in my node excluding me
      comm_children[0].resize(CkNodeSize(mynode) - 1);
      std::iota(comm_children[0].begin(), comm_children[0].end(), level1root + 1);
      comm_logic[0] = new MsgAggregator();
    }
    else
    {
      comm_parent[0] = level1root;
    }

    // node level (level 1)
    if (mype == level1root)
    {
      std::vector<int> pes_in_node(CkNodeSize(mynode));
      std::iota(pes_in_node.begin(), pes_in_node.end(), level1root);

      NodeLevel* level = new NodeLevel(lbmgr, pes_in_node);
      level->configure(_lb_args.testPeSpeed(), strategies[1], config["process"], repeat_strategies[1], step_freq_lvl2);
      logic[1] = level;

      // set up comm-tree between levels 1 and 2
      std::vector<int> level1_roots(group_size);
      // level2root is the first PE on the first node of mygroup
      const int firstGroupNode = CkNodeOf(level2root);
      for (int index = 0; index < group_size; index++)
      {
        level1_roots[index] = CkNodeFirst(firstGroupNode + index);
      }

      int parent, num_children;
      int* children;
      getPETopoTreeEdges(mype, level2root, level1_roots.data(), level1_roots.size(), 4,
                         &parent, &num_children, &children);
      comm_parent[1] = parent;
      if (num_children > 0)
      {
        comm_children[1].assign(children, children + num_children);
        free(children);
        comm_logic[1] = new MsgAggregator();
      }
    }

    // nodeset level (level 2)
    if (mype == level2root)
    {
      // assumes all nodes in my group are same size
      std::vector<int> pes_in_group((size_t)group_size * CkNodeSize(mynode));
      std::iota(pes_in_group.begin(), pes_in_group.end(), GroupFirstPe(mygroup));

      NodeSetLevel* level = new NodeSetLevel(lbmgr, pes_in_group);
      level->configure(_lb_args.testPeSpeed(), strategies[2], config["processgroup"], repeat_strategies[2], step_freq_lvl3);
      logic[2] = level;

      if (mype != level3root) comm_parent[2] = level3root;
    }

    // root of tree (level 3)
    if (mype == level3root)
    {
      // Note that we are adding to comm level 2 here (which is responsible for
      // communication between logic levels 2 and 3)
      for (int group = 0; group < num_groups; group++)
      {
        const int pe = GroupFirstPe(group);
        if (pe != mype) comm_children[2].push_back(pe);
      }
      comm_logic[2] = new MsgAggregator();

      RootLevel* level = new RootLevel(num_groups);
      // The root level of this tree has no strategies, so pass empty strategy vector
      level->configure(_lb_args.testPeSpeed(), {""}, config["root"], false, token_passing);
      logic[3] = level;
    }

    if (CkMyPe() == 0 && !quietModeRequested)
    {
      CkPrintf(
          "[%d] TreeLB: Using PE_Process_ProcessGroup_Root tree with %d groups and "
          "with:\n",
          CkMyPe(), num_groups);

      CkPrintf("\tProcess: ");
      for (const auto& strategy : strategies[1])
      {
        CkPrintf("%s ", strategy.c_str());
      }
      CkPrintf("\n");

      CkPrintf("\tProcessGroup: ");
      for (const auto& strategy : strategies[2])
      {
        CkPrintf("%s ", strategy.c_str());
      }
      CkPrintf("\n");

      CkPrintf("\tRoot: %s\n", token_passing ? "TokenPassing" : "Dummy");

      if (_lb_args.debug() > 0)
      {
        CkPrintf(
            "\tUsing %d as root\n"
            "\tTest PE Speed: %s\n"
            "\tProcessGroup step frequency: %d\n"
            "\tRoot step frequency: %d\n",
            rootPE, _lb_args.testPeSpeed() ? "true" : "false", step_freq_lvl2,
            step_freq_lvl3);
      }
    }

    return L;
  }
};

#endif /* TREEBUILDER_H */
