#define CMK_ENABLE_ASYNC_PROGRESS                          1

//#define  DELTA_COMPRESS                                     1

#undef CMK_MSG_HEADER_EXT_ 
#if DELTA_COMPRESkS
#define CMK_MSG_HEADER_EXT_    CmiUInt2 rank, hdl,xhdl,info, type; unsigned char cksum, magic; int root, dstnode; size_t size; CmiUInt2 redID, padding; char work[6*sizeof(void *)]; CmiUInt4 compressStart; CmiUInt2 compress_flag,xxhdl; CmiUInt8 persistRecvHandler; CmiUInt1 cmaMsgType:2, nokeep:1;
#else
#define CMK_MSG_HEADER_EXT_    CmiUInt2 rank, hdl,xhdl,info, type; unsigned char cksum, magic; int root, dstnode; size_t size; CmiUInt2 redID, padding; char work[6*sizeof(void *)]; CmiUInt1 cmaMsgType:2, nokeep:1;
#endif


