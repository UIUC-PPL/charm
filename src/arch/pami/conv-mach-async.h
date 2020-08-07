#define CMK_ENABLE_ASYNC_PROGRESS                          1

#undef CMK_MSG_HEADER_EXT_ 

#if CMK_BLUEGENEQ
#define CMK_MSG_HEADER_EXT_    CmiUInt2 rank, hdl,xhdl,info, stratid; unsigned char cksum, magic; int root, dstnode; size_t size; CmiUInt2 redID, padding; char work[6*sizeof(void *)]; CmiUInt1 cmaMsgType:2, nokeep:1;
#else
#define CMK_MSG_HEADER_EXT_    CmiUInt2 rank, hdl,xhdl,info, stratid; unsigned char cksum, magic; int root, dstnode; size_t size; CmiUInt2 redID, padding; char work[8*sizeof(void *)]; CmiUInt1 cmaMsgType:2, nokeep:1;
#endif


