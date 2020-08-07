
#undef CMK_MSG_HEADER_BASIC
#undef CMK_MSG_HEADER_EXT_
//#undef CMK_MSG_HEADER_EXT
/* expand the header to store the restart phase counter(pn) */
#define CMK_MSG_HEADER_BASIC   CMK_MSG_HEADER_EXT
#define CMK_MSG_HEADER_EXT_    CmiUInt2 d0,d1,d2,d3,hdl,pn,d4,type,xhdl,info,dd,redID,pad2,rank; CmiUInt4 root; size_t size; CmiUInt1 zcMsgType:4, cmaMsgType:2, nokeep:1;
//#define CMK_MSG_HEADER_EXT    { CMK_MSG_HEADER_EXT_ }

#define CmiGetRestartPhase(m)       ((((CmiMsgHeaderExt*)m)->pn))

#define __FAULT__					   1

#define CMK_MEM_CHECKPOINT				   1
