#ifndef __TREENODE__H__
#define __TREENODE__H__

#include <vector>
#include <string.h>
#include <assert.h>
#include <stdio.h>

#include "charm++.h"
#include "picsdefs.h"
#include "picsdefscpp.h"
class TreeNode;

typedef std::vector<TreeNode*> NodeCollection;
typedef std::vector<TreeNode*>::iterator NodeIter;

class Condition {
private:
  std::string name;
  int     varIndex;
  double  base;
  int     baseIndex;
  int     thresholdIndex;
  double  threshold;
  CompareSymbol symbol;
  Operator op;
  //potential performance improvement if problem solved
  double potentialImprove;

public:
  Condition() : varIndex(-2) {}
  Condition(std::string n, int _varIndex, Operator _op,  double _base,
      CompareSymbol c) : varIndex(_varIndex), base(_base), baseIndex(-1),
      thresholdIndex(-1), threshold(0), symbol(c), op(_op) {
      name = n;
  }

  Condition(std::string n, int _varIndex, Operator _op, int _baseIndex,
      double _threshold, CompareSymbol c) : varIndex(_varIndex),
      baseIndex(_baseIndex), thresholdIndex(-1), threshold(_threshold),
      symbol(c), op(_op) {
      name = n;
  }

  double getPotentialImprove() { return potentialImprove;}
  void setPotentialImprove(double v) { potentialImprove = v;}
  void printMe();
  void printDataToFile(double *input, FILE *fp);
  void parseString(std::string str, FILE *fp);
  void printFields(double *input, FILE *fp);
  bool test(double *input); //test whether this condition is satisfied with input data
};

class Solution {

private:
  int eff;

public:
  Solution(Direction d, Effect n) {
    if(d == UP)
      eff = n;
    else
      eff = -n;
  }
  void printMe(){
    int abseff = eff>=0?eff:-eff;
    CkPrintf("solution %s  %s \n", eff>0?"UP":"Down", EffectName[abseff]);
  }
  void printDataToFile(double *input, FILE *fp) ;
  int getValue() { return eff;}
};

union Data_t {
  Condition *condition;
  Solution *solution;
};

typedef union Data_t Data;

class TreeNode {

private:
  TreeNode *parent;
  //bool isLeaf;
  NodeCollection children;
  NodeIter it;
  Data data;
  bool _isSolution;

public:

  TreeNode(TreeNode *parent, Condition *c);

  TreeNode(TreeNode *parent, Solution *s);

  void addChild(TreeNode*);

  TreeNode* getParent();
  void setParent(TreeNode *p);

  Data getValue();

  int getSolutionValue();

  void beginChild();
  int isEndChild();

  TreeNode* getCurrentChild();
  void nextChild();

  void printMe();
  void printDataToFile(double *input, FILE *fp);

  bool test(double *input) ;

  bool isLeaf() { return children.size()==0;}

  bool isSolution() { return _isSolution; }

  double getPotentialImprove() {
    CkAssert(!_isSolution);
    return data.condition->getPotentialImprove();
  }
  void setPotentialImprove(double v) {
    CkAssert(!_isSolution);
    data.condition->setPotentialImprove(v);
  }
};

#endif
