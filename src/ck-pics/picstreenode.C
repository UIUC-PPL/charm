#include <stdlib.h>
#include <stdio.h>
#include "picstreenode.h"
#include "charm++.h"
#include "register.h"


/**
 * 28 Avg
 * 40 Max
 * 9 Min
 */
char FieldName[NUM_NODES][30] = {
    "AVG_TotalTime",
    "AVG_IdlePercentage",
    "AVG_OverheadPercentage",
    "AVG_UtilizationPercentage",
    "AVG_NumObjectsPerPE",
    "AVG_NumMsgRecvPerPE",
    "AVG_BytesMsgRecvPerPE",
    "AVG_EntryMethodDuration",
    "AVG_NumInvocations",
    "AVG_LoadPerPE",
    "AVG_CacheMissRate",
    "AVG_NumMsgsPerObject",
    "AVG_BytesPerObject",
    "AVG_LoadPerObject",
    "AVG_BytesPerMsg",
    "AVG_AppPercentage",
    "AVG_EntryMethodDuration_1",
    "AVG_EntryMethodDuration_2",
    "AVG_NumInvocations_1",
    "AVG_NumInvocations_2",
    "AVG_NumMsgPerPE",
    "AVG_BytesPerPE",
    "AVG_ExternalBytePerPE",
    "AVG_CompressTime",
    "AVG_CompressSourceBytes",
    "AVG_CompressDestBytes",
    "AVG_MsgTimeCost",
    "AVG_TuningOverhead",
    "MAX_IdlePercentage",
   "MAX_IdlePE",
   "MAX_OverheadPercentage",
   "MAX_OverheadPE",
   "MAX_UtilizationPercentage",
   "MAX_UtilPE",
   "MAX_AppPercentage",
   "MAX_AppPE",
   "MAX_NumInvocations",
   "MAX_NumInvocPE",
   "MAX_LoadPerObject",
   "MAX_ObjID",
   "MAX_LoadPerPE",
   "MAX_LoadPE",
   "MAX_BytesPerMsg",
   "MAX_BytesEntryID",
   "MAX_BytesPerObject",
   "MAX_ByteObjID",
   "MAX_NumMsgsPerObject",
   "MAX_NumMsgObjID",
   "MAX_BytesPerPE",
   "MAX_BytesPE",
   "MAX_ExternalBytePerPE",
   "MAX_ExternalBytePE",
   "MAX_CriticalPathLength",
   "MAX_CPPE",
   "MAX_NumMsgRecv",
   "MAX_NumMsgRecvPE",
   "MAX_BytesMsgRecv",
   "MAX_BytesMsgRecvPE",
   "MAX_EntryMethodDuration",
   "MAX_EntryID",
   "MAX_EntryMethodDuration_1",
   "MAX_EntryID_1",
   "MAX_EntryMethodDuration_2",
   "MAX_EntryID_2",
   "MAX_NumMsgSend",
   "MAX_NumMsgSendPE",
   "MAX_BytesSend",
   "MAX_BytesSendPE",
   "MIN_IdlePercentage",
   "MIN_OverheadPercentage",
   "MIN_UtilizationPercentage",
   "MIN_AppPercentage",
   "MIN_LoadPerObject",
   "MIN_LoadPerPE",
   "MIN_BytesPerMsg",
   "MIN_NumMsgRecv",
   "MIN_BytesMsgRecv",
   "MinIdlePE",
   "MaxEntryPE"
};


char EffectName[PICS_NUM_EFFECTS][30] = { 
  "PICS_EFF_PERFGOOD",
  "PICS_EFF_GRAINSIZE",
  "PICS_EFF_AGGREGATION", 
  "PICS_EFF_COMPRESSION",
  "PICS_EFF_REPLICA", 
  "PICS_EFF_LDBFREQUENCY",
  "PICS_EFF_NODESIZE",
  "PICS_EFF_MESSAGESIZE",
  "PICS_EFF_GRAINSIZE_1",
  "PICS_EFF_GRAINSIZE_2",
  "PICS_EFF_UNKNOWN"
};

char operatorName[4][2] = {"+", "-", "*", "/" };
char compareName[6][3] = {"==", "<", ">", ">=", "<=", "!="};


void Condition::printMe() {
  printf("condition %s \n", name.c_str());
}

void Condition::parseString(std::string str, FILE *fp) {
  std::size_t pos = str.find("_");
  fprintf(fp, "Condition:\n");
  if(strstr(str.c_str(), "Low")) {
    fprintf(fp, "%s is too low.\n", str.substr(pos+1).c_str());
  } else if(strstr(str.c_str(), "High")) {
    fprintf(fp, "%s is too high.\n", str.substr(pos+1).c_str());
  } else if(str.c_str(), "Small") {
    fprintf(fp, "%s is too small.\n", str.substr(pos+1).c_str());
  } else if(str.c_str(), "Many") {
    fprintf(fp, "%s is too many.\n", str.substr(pos+1).c_str());
  } else if(str.c_str(), "Few") {
    fprintf(fp, "%s is too few.\n", str.substr(pos+1).c_str());
  } else if(str.c_str(), "Long") {
    fprintf(fp, "%s is too long.\n", str.substr(pos+1).c_str());
  } else {
    fprintf(fp, "Invalid entry format in decision tree for %s.\n", str.substr(pos+1).c_str());
  }
}

void Condition::printFields(double *input, FILE *fp) {
  if(thresholdIndex > -1) {
    threshold = input[thresholdIndex];
  }
  if(varIndex > -1) {
    fprintf(fp, "%s %f\n", FieldName[varIndex], input[varIndex]);
  }

  if(baseIndex > -1) {
    base = input[baseIndex];
    fprintf(fp, "%s %f \n", FieldName[baseIndex], base);
  }
}

/**
 * Parse string to print data to files in readable form
 * @param input
 * @param fp
 */
void Condition::printDataToFile(double *input, FILE *fp) {
  parseString(name, fp);
  printFields(input, fp);
}

bool Condition::test(double *input) {
  bool ret;
  double result;
  if(varIndex == -2) return true;     //always true

  assert(varIndex>-1 && varIndex<NUM_NODES);
  double realValue = input[varIndex];
  if(baseIndex > -1)
    base = input[baseIndex];
  if(thresholdIndex > -1)
    threshold = input[thresholdIndex];

  switch(op) {
  case ADD:
    result = realValue + base;
    break;

  case SUB:
    result = realValue - base;
    break;

  case MUL:
    result = realValue * base;
    break;

  case DIV:
    result = realValue / base;
    break;

  default:
    printf("Undefined OP\n");
    exit(1);
  }

  switch(symbol) {
  case IS:
    ret = (result == threshold);
    break;

  case LT:
    ret = (result < threshold);
    break;

  case GT:
    ret = (result > threshold);
    break;

  case NLT:
    ret = (result >= threshold);
    break;

  case NGT:
    ret = (result <= threshold);
    break;

  case NOTIS:
    ret = (result != threshold);
    break;

  default:
    printf("Undefined symbol \n");
    exit(1);
  }
  if(!strcmp(name.c_str(), "Low_CPU_Util"))
    potentialImprove = 1 - realValue;
  else if(!strcmp(name.c_str(), "High_Overhead"))
    potentialImprove = realValue;
  else if(!strcmp(name.c_str(), "High_Idle"))
    potentialImprove = realValue;
  else 
    potentialImprove = -100;

  return ret;
}

/**
 * PICS suggested solutions.
 * Applied to code iff PICS is used for tuning otherwise just prints a suggestion
 * @param input
 * @param fp
 */
void Solution::printDataToFile(double *input, FILE *fp) {
  int abseff = eff>=0?eff:-eff;
  fprintf(fp, "Solution %s %s \n", eff>0?"UP":"Down", EffectName[abseff]);
}

TreeNode::TreeNode( TreeNode *p, Condition *c ) {
  parent = p;
  data.condition = c;
  _isSolution = false;
}

TreeNode::TreeNode( TreeNode *p, Solution *s ) {
  parent = p;
  data.solution = s;
  _isSolution = true;
}

void TreeNode::addChild(TreeNode *tn) {
  children.push_back(tn);
}

void TreeNode::setParent(TreeNode *p) {
  parent = p;
}

TreeNode* TreeNode::getParent() {
  return parent;
}

Data TreeNode::getValue() {
  return data;
}

int TreeNode::getSolutionValue() {
  assert(_isSolution);
  return data.solution->getValue();
}

void TreeNode::beginChild() {
  it = children.begin(); 
}

int TreeNode::isEndChild() {
  return it == children.end();
}

void TreeNode::nextChild() {
  it++;
}

TreeNode* TreeNode::getCurrentChild() {
  return *it;
}

void TreeNode::printMe() {
  if(_isSolution) {
    data.solution->printMe();
  }
  else {
    data.condition->printMe();
  }
}

/**
 * Call printData func iff the data is a condition data and not solution data
 * @param input
 * @param fp
 */
void TreeNode::printDataToFile(double *input, FILE *fp) {
  if(!_isSolution) {
    data.condition->printDataToFile(input, fp);
  }
}


bool TreeNode::test(double *input) {
  if(!children.empty()) {
    return data.condition->test(input);
  }
  else {
    return false;
  }
}
