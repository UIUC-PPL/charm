
#undef CMK_MSG_HEADER_EXT_
//#undef CMK_MSG_HEADER_EXT
/* expand the header to store the restart phase counter(pn) */
#define CMK_MSG_HEADER_EXT_    size_t size; CmiUInt4 seq; CmiUInt2 rank,hdl,xhdl,info,type,redID,pn,d9; CmiInt4 root; CmiUInt1 zcMsgType:4, cmaMsgType:2, nokeep:1;;
//#define CMK_MSG_HEADER_EXT    { CMK_MSG_HEADER_EXT_ }

#define CmiGetRestartPhase(m)       ((((CmiMsgHeaderExt*)m)->pn))

#define __FAULT__					   1

#define CMK_MEM_CHECKPOINT				   1
