#ifndef __PICSDEFS__H__
#define  __PICSDEFS__H__

extern char FieldName[][30];
extern char EffectName[][30];

#define NUM_AVG 28
#define NUM_MIN 9
#define NUM_MAX 40 
#define NUM_NODES   79
enum FieldIndex_t {
  AVG_TotalTime=0,
  AVG_IdlePercentage,
  AVG_OverheadPercentage,
  AVG_UtilizationPercentage,
  AVG_AppPercentage,
  AVG_EntryMethodDuration,
  AVG_EntryMethodDuration_1,
  AVG_EntryMethodDuration_2,
  AVG_NumInvocations,
  AVG_NumInvocations_1,
  AVG_NumInvocations_2,
  AVG_LoadPerObject,
  AVG_LoadPerPE,
  AVG_NumObjectsPerPE,
  AVG_BytesPerMsg,
  AVG_BytesPerObject,
  AVG_NumMsgsPerObject,
  AVG_NumMsgPerPE,
  AVG_CacheMissRate,
  AVG_BytesPerPE,
  AVG_ExternalBytePerPE,
  AVG_CompressTime,
  AVG_CompressSourceBytes,
  AVG_CompressDestBytes,
  AVG_NumMsgRecv,
  AVG_BytesMsgRecv,
  AVG_MsgTimeCost,
  AVG_TuningOverhead,
  MAX_IdlePercentage,
  MAX_IdlePE,
  MAX_OverheadPercentage,
  MAX_OverheadPE,
  MAX_UtilizationPercentage,
  MAX_UtilPE,
  MAX_AppPercentage,
  MAX_AppPE,
  MAX_NumInvocations,
  MAX_NumInvocPE,
  MAX_LoadPerObject,
  MAX_ObjID,
  MAX_LoadPerPE,
  MAX_LoadPE,
  MAX_BytesPerMsg,
  MAX_BytesEntryID,
  MAX_BytesPerObject,
  MAX_ByteObjID,
  MAX_NumMsgsPerObject,
  MAX_NumMsgObjID,
  MAX_BytesPerPE,
  MAX_BytesPE,
  MAX_ExternalBytePerPE,
  MAX_ExternalBytePE,
  MAX_CriticalPathLength,
  MAX_CPPE,
  MAX_NumMsgRecv,
  MAX_NumMsgRecvPE,
  MAX_BytesMsgRecv,
  MAX_BytesMsgRecvPE,
  MAX_EntryMethodDuration,
  MAX_EntryID,
  MAX_EntryMethodDuration_1,
  MAX_EntryID_1,
  MAX_EntryMethodDuration_2,
  MAX_EntryID_2,
  MAX_NumMsgSend,
  MAX_NumMsgSendPE,
  MAX_BytesSend,
  MAX_BytesSendPE,
  MIN_IdlePercentage,
  MIN_OverheadPercentage,
  MIN_UtilizationPercentage,
  MIN_AppPercentage,
  MIN_LoadPerObject,
  MIN_LoadPerPE,
  MIN_BytesPerMsg,
  MIN_NumMsgRecv,
  MIN_BytesMsgRecv,
  MinIdlePE,
  MaxEntryPE
};

#define PICS_NUM_EFFECTS 11 
enum Effect_t {
  PICS_EFF_PERFGOOD=0,
  PICS_EFF_GRAINSIZE,
  PICS_EFF_AGGREGATION,
  PICS_EFF_COMPRESSION,
  PICS_EFF_REPLICA,
  PICS_EFF_LDBFREQUENCY,
  PICS_EFF_NODESIZE,
  PICS_EFF_MESSAGESIZE,
  PICS_EFF_GRAINSIZE_1,
  PICS_EFF_GRAINSIZE_2,
  PICS_EFF_UNKNOWN
};

typedef enum Effect_t Effect;

enum Direction_t { UP=0, DOWN};
enum CompareSymbol_t {IS=0, LT, GT, NLT, NGT, NOTIS} ;
enum Operator_t {ADD=0, SUB, MUL, DIV};
enum PREFIX_t {AVG=0, MIN, MAX};

typedef enum CompareSymbol_t    CompareSymbol ; 
typedef enum Operator_t     Operator;
typedef enum Direction_t    Direction;

#define FULL 0
#define PARTIAL 1
#define SEQUENTIAL 10
#define PARALLEL  11
#define SINGLE 20
#define MULTIPLE 21
#define PICS_INVALID -1

#define   PERIOD_PERF 1

#endif
