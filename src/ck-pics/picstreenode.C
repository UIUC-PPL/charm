#include <stdlib.h>
#include <stdio.h>
#include "picstreenode.h"
#include "charm++.h"
#include "register.h"


char FieldName[NUM_NODES][30] = {
 "AVG_TotalTime",
 "AVG_IdlePercentage",
 "AVG_OverheadPercentage",
 "AVG_UtilizationPercentage",
 "AVG_AppPercentage",
 "AVG_EntryMethodDuration",
 "AVG_NumInvocations",
 "AVG_LoadPerObject",
 "AVG_LoadPerPE",
 "AVG_NumObjectsPerPE",
 "AVG_BytesPerMsg",
 "AVG_BytesPerObject",
 "AVG_NumMsgsPerObject",
 "AVG_NumMsgPerPE",
 "AVG_CacheMissRate",
 "AVG_BytesPerPE",
 "AVG_ExternalBytePerPE",
 "AVG_CompressTime",
 "AVG_CompressSourceBytes",
 "AVG_CompressDestBytes",
 "AVG_NumMsgRecv",
 "AVG_BytesMsgRecv",
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

void Condition::printDataToFile(double *input, FILE *fp) {

  fprintf(fp, "Condition  %s %d %d ", name.c_str(), varIndex, baseIndex);
  if(thresholdIndex > -1)
    threshold = input[thresholdIndex];
  if(varIndex>-1)
    fprintf(fp, "  %s %f %s ", FieldName[varIndex], input[varIndex], operatorName[op]);

  if(baseIndex > -1) {
    base = input[baseIndex];
    fprintf(fp, " %s %f ", FieldName[baseIndex], base);
  }
  else
    fprintf(fp, " %f ", base);

  fprintf(fp, " %s %f ", compareName[symbol], threshold);
  //potential improvement
  fprintf(fp, " %f ", potentialImprove);

  if(varIndex == MAX_EntryMethodDuration)
  {
    int entryIdx = (int)input[varIndex+1];
    fprintf(fp, " %d  %s %s ", entryIdx, _entryTable[entryIdx]->name, _chareTable[_entryTable[entryIdx]->chareIdx]->name); 
  }else if(varIndex>=NUM_AVG && varIndex<NUM_AVG+NUM_MAX)
    fprintf(fp, " %d ", (int)input[varIndex+1]);

  fprintf(fp, "\n");
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
  if(!strcmp(name.c_str(), "CPU_Util"))
    potentialImprove = 1 - realValue;
  else if(!strcmp(name.c_str(), "High_Overhead"))
    potentialImprove = realValue;
  else if(!strcmp(name.c_str(), "High_Idle"))
    potentialImprove = realValue;
  else 
    potentialImprove = -100;

  return ret;
}

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

void TreeNode::printDataToFile(double *input, FILE *fp) {
  if(_isSolution) {
    data.solution->printDataToFile(input, fp);
  }
  else {
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
