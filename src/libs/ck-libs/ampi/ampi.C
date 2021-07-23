#ifndef AMPI_PRINT_MSG_SIZES
#define AMPI_PRINT_MSG_SIZES 0 // Record and print comm routines used & message sizes
#endif

#define AMPIMSGLOG    0
#define AMPI_PRINT_IDLE 0

#include "ampiimpl.h"
#include "tcharm.h"


#if CMK_TRACE_ENABLED
#include "register.h" // for _chareTable, _entryTable
#endif

// Default is to abort on error, but users can build
// AMPI with -DAMPI_ERRHANDLER_RETURN=1 to change it:
#if AMPI_ERRHANDLER_RETURN
#define AMPI_ERRHANDLER MPI_ERRORS_RETURN
#else
#define AMPI_ERRHANDLER MPI_ERRORS_ARE_FATAL
#endif

#define MSG_ORDER_DEBUG(x) //x
#define STARTUP_DEBUG(x) //ckout<<"ampi[pe "<<CkMyPe()<<"] "<< x <<endl;

/* For MPI_Get_library_version */
extern const char * const CmiCommitID;

static CkDDT *getDDT() noexcept {
  return &getAmpiParent()->myDDT;
}

/* if error checking is disabled, ampiErrhandler is defined as a macro in ampiimpl.h */
#if AMPI_ERROR_CHECKING
int ampiErrhandler(const char* func, int errcode) noexcept {
  if (AMPI_ERRHANDLER == MPI_ERRORS_ARE_FATAL && errcode != MPI_SUCCESS) {
    // Abort with a nice message of the form: 'func' failed with error code 'errstr'.
    //  where 'func' is the name of the failed AMPI_ function and 'errstr'
    //  is the string returned by AMPI_Error_string for errcode.
    int errstrlen;
    char errstr[MPI_MAX_ERROR_STRING];
    MPI_Error_string(errcode, errstr, &errstrlen);
    CkAbort("%s failed with error code %s", func, errstr);
  }
  return errcode;
}
#endif

#if AMPI_PRINT_MSG_SIZES
#if !AMPI_ERROR_CHECKING
#error "AMPI_PRINT_MSG_SIZES requires AMPI error checking to be enabled!\n"
#endif
#include <string>
#include <sstream>
#include "ckliststring.h"
CkpvDeclare(CkListString, msgSizesRanks);

bool ampiParent::isRankRecordingMsgSizes() noexcept {
  return (!CkpvAccess(msgSizesRanks).isEmpty() && CkpvAccess(msgSizesRanks).includes(thisIndex));
}

void ampiParent::recordMsgSize(const char* func, int msgSize) noexcept {
  if (isRankRecordingMsgSizes()) {
    msgSizes[func][msgSize]++;
  }
}

typedef std::unordered_map<std::string, std::map<int, int> >::iterator outer_itr_t;
typedef std::map<int, int>::iterator inner_itr_t;

void ampiParent::printMsgSizes() noexcept {
  if (isRankRecordingMsgSizes()) {
    // Prints msgSizes in the form: "AMPI_Routine: [ (num_msgs: msg_size) ... ]".
    // Each routine has its messages sorted by size, smallest to largest.
    std::stringstream ss;
    ss << std::endl << "Rank " << thisIndex << ":" << std::endl;
    for (outer_itr_t i = msgSizes.begin(); i != msgSizes.end(); ++i) {
      ss << i->first << ": [ ";
      for (inner_itr_t j = i->second.begin(); j != i->second.end(); ++j) {
        ss << "(" << j->second << ": " << j->first << " B) ";
      }
      ss << "]" << std::endl;
    }
    CkPrintf("%s", ss.str().c_str());
  }
}
#endif //AMPI_PRINT_MSG_SIZES

inline int checkCommunicator(const char* func, MPI_Comm comm) noexcept {
  if (comm == MPI_COMM_NULL)
    return ampiErrhandler(func, MPI_ERR_COMM);
  return MPI_SUCCESS;
}

inline int checkCount(const char* func, int count) noexcept {
  if (count < 0)
    return ampiErrhandler(func, MPI_ERR_COUNT);
  return MPI_SUCCESS;
}

inline int checkData(const char* func, MPI_Datatype data) noexcept {
  if (data == MPI_DATATYPE_NULL)
    return ampiErrhandler(func, MPI_ERR_TYPE);
  return MPI_SUCCESS;
}

inline int checkTag(const char* func, int tag) noexcept {
  if (tag != MPI_ANY_TAG && (tag < 0 || tag > MPI_TAG_UB_VALUE))
    return ampiErrhandler(func, MPI_ERR_TAG);
  return MPI_SUCCESS;
}

inline int checkRank(const char* func, int rank, MPI_Comm comm) noexcept {
  int size = (comm == MPI_COMM_NULL) ? 0 : getAmpiInstance(comm)->getSize();
  if (((rank >= 0) && (rank < size)) ||
      (rank == MPI_ANY_SOURCE)       ||
      (rank == MPI_PROC_NULL)        ||
      (rank == MPI_ROOT))
    return MPI_SUCCESS;
  return ampiErrhandler(func, MPI_ERR_RANK);
}

inline int checkBuf(const char* func, const void *buf, int count, bool isAbsolute) noexcept {
  if ((count != 0 && buf == NULL && !isAbsolute) || buf == MPI_IN_PLACE)
    return ampiErrhandler(func, MPI_ERR_BUFFER);
  return MPI_SUCCESS;
}

int errorCheck(const char* func, MPI_Comm comm, bool ifComm, int count,
               bool ifCount, MPI_Datatype data, bool ifData, int tag,
               bool ifTag, int rank, bool ifRank, const void *buf1,
               bool ifBuf1, const void *buf2=nullptr, bool ifBuf2=false) noexcept {
  int ret;
  bool isAbsolute = false;

  if (ifComm) {
    ret = checkCommunicator(func, comm);
    if (ret != MPI_SUCCESS)
      return ampiErrhandler(func, ret);
  }
  if (ifCount) {
    ret = checkCount(func, count);
    if (ret != MPI_SUCCESS)
      return ampiErrhandler(func, ret);
  }
  if (ifData) {
    ret = checkData(func, data);
    if (ret != MPI_SUCCESS)
      return ampiErrhandler(func, ret);
    isAbsolute = getDDT()->getType(data)->getAbsolute();
  }
  if (ifTag) {
    ret = checkTag(func, tag);
    if (ret != MPI_SUCCESS)
      return ampiErrhandler(func, ret);
  }
  if (ifRank) {
    ret = checkRank(func, rank, comm);
    if (ret != MPI_SUCCESS)
      return ampiErrhandler(func, ret);
  }
  if (ifBuf1 && ifData) {
    ret = checkBuf(func, buf1, count*getDDT()->getSize(data), isAbsolute);
    if (ret != MPI_SUCCESS)
      return ampiErrhandler(func, ret);
  }
  if (ifBuf2 && ifData) {
    ret = checkBuf(func, buf2, count*getDDT()->getSize(data), false);
    if (ret != MPI_SUCCESS)
      return ampiErrhandler(func, ret);
  }
#if AMPI_PRINT_MSG_SIZES
  getAmpiParent()->recordMsgSize(func, getDDT()->getSize(data) * count);
#endif
  return MPI_SUCCESS;
}

//------------- startup -------------
typedef struct { float val; int idx; } FloatInt;
typedef struct { double val; int idx; } DoubleInt;
typedef struct { long val; int idx; } LongInt;
typedef struct { int val; int idx; } IntInt;
typedef struct { short val; int idx; } ShortInt;
typedef struct { long double val; int idx; } LongdoubleInt;
typedef struct { float val; float idx; } FloatFloat;
typedef struct { double val; double idx; } DoubleDouble;

/* For MPI_MIN, and MPI_MAX: */
#define MPI_MINMAX_OP_SWITCH(OPNAME) \
  int i; \
switch (*datatype) { \
  case MPI_CHAR: for(i=0;i<(*len);i++) { MPI_OP_IMPL(char); } break; \
  case MPI_SHORT: for(i=0;i<(*len);i++) { MPI_OP_IMPL(signed short int); } break; \
  case MPI_INT: for(i=0;i<(*len);i++) { MPI_OP_IMPL(signed int); } break; \
  case MPI_LONG: for(i=0;i<(*len);i++) { MPI_OP_IMPL(signed long); } break; \
  case MPI_UNSIGNED_CHAR: for(i=0;i<(*len);i++) { MPI_OP_IMPL(unsigned char); } break; \
  case MPI_UNSIGNED_SHORT: for(i=0;i<(*len);i++) { MPI_OP_IMPL(unsigned short); } break; \
  case MPI_UNSIGNED: for(i=0;i<(*len);i++) { MPI_OP_IMPL(unsigned int); } break; \
  case MPI_UNSIGNED_LONG: for(i=0;i<(*len);i++) { MPI_OP_IMPL(unsigned long); } break; \
  case MPI_FLOAT: for(i=0;i<(*len);i++) { MPI_OP_IMPL(float); } break; \
  case MPI_DOUBLE: for(i=0;i<(*len);i++) { MPI_OP_IMPL(double); } break; \
  case MPI_LONG_LONG_INT: for(i=0;i<(*len);i++) { MPI_OP_IMPL(signed long long); } break; \
  case MPI_SIGNED_CHAR: for(i=0;i<(*len);i++) { MPI_OP_IMPL(signed char); } break; \
  case MPI_UNSIGNED_LONG_LONG: for(i=0;i<(*len);i++) { MPI_OP_IMPL(unsigned long long); } break; \
  case MPI_WCHAR: for(i=0;i<(*len);i++) { MPI_OP_IMPL(wchar_t); } break; \
  case MPI_INT8_T: for(i=0;i<(*len);i++) { MPI_OP_IMPL(int8_t); } break; \
  case MPI_INT16_T: for(i=0;i<(*len);i++) { MPI_OP_IMPL(int16_t); } break; \
  case MPI_INT32_T: for(i=0;i<(*len);i++) { MPI_OP_IMPL(int32_t); } break; \
  case MPI_INT64_T: for(i=0;i<(*len);i++) { MPI_OP_IMPL(int64_t); } break; \
  case MPI_UINT8_T: for(i=0;i<(*len);i++) { MPI_OP_IMPL(uint8_t); } break; \
  case MPI_UINT16_T: for(i=0;i<(*len);i++) { MPI_OP_IMPL(uint16_t); } break; \
  case MPI_UINT32_T: for(i=0;i<(*len);i++) { MPI_OP_IMPL(uint32_t); } break; \
  case MPI_UINT64_T: for(i=0;i<(*len);i++) { MPI_OP_IMPL(uint64_t); } break; \
  case MPI_AINT: for(i=0;i<(*len);i++) { MPI_OP_IMPL(MPI_Aint); } break; \
  default: \
           ckerr << "Type " << *datatype << " with Op "#OPNAME" not supported." << endl; \
  CkAbort("Unsupported MPI datatype for MPI Op"); \
};\

/* For MPI_SUM, MPI_PROD, and MPI_REPLACE: */
#define MPI_SUMPROD_OP_SWITCH(OPNAME) \
  int i; \
switch (*datatype) { \
  case MPI_CHAR: for(i=0;i<(*len);i++) { MPI_OP_IMPL(char); } break; \
  case MPI_SHORT: for(i=0;i<(*len);i++) { MPI_OP_IMPL(signed short int); } break; \
  case MPI_INT: for(i=0;i<(*len);i++) { MPI_OP_IMPL(signed int); } break; \
  case MPI_LONG: for(i=0;i<(*len);i++) { MPI_OP_IMPL(signed long); } break; \
  case MPI_UNSIGNED_CHAR: for(i=0;i<(*len);i++) { MPI_OP_IMPL(unsigned char); } break; \
  case MPI_UNSIGNED_SHORT: for(i=0;i<(*len);i++) { MPI_OP_IMPL(unsigned short); } break; \
  case MPI_UNSIGNED: for(i=0;i<(*len);i++) { MPI_OP_IMPL(unsigned int); } break; \
  case MPI_UNSIGNED_LONG: for(i=0;i<(*len);i++) { MPI_OP_IMPL(unsigned long); } break; \
  case MPI_FLOAT: for(i=0;i<(*len);i++) { MPI_OP_IMPL(float); } break; \
  case MPI_DOUBLE: for(i=0;i<(*len);i++) { MPI_OP_IMPL(double); } break; \
  case MPI_COMPLEX: for(i=0;i<(*len);i++) { MPI_OP_IMPL(std::complex<float>); } break; \
  case MPI_DOUBLE_COMPLEX: for(i=0;i<(*len);i++) { MPI_OP_IMPL(std::complex<double>); } break; \
  case MPI_LONG_LONG_INT: for(i=0;i<(*len);i++) { MPI_OP_IMPL(signed long long); } break; \
  case MPI_SIGNED_CHAR: for(i=0;i<(*len);i++) { MPI_OP_IMPL(signed char); } break; \
  case MPI_UNSIGNED_LONG_LONG: for(i=0;i<(*len);i++) { MPI_OP_IMPL(unsigned long long); } break; \
  case MPI_WCHAR: for(i=0;i<(*len);i++) { MPI_OP_IMPL(wchar_t); } break; \
  case MPI_INT8_T: for(i=0;i<(*len);i++) { MPI_OP_IMPL(int8_t); } break; \
  case MPI_INT16_T: for(i=0;i<(*len);i++) { MPI_OP_IMPL(int16_t); } break; \
  case MPI_INT32_T: for(i=0;i<(*len);i++) { MPI_OP_IMPL(int32_t); } break; \
  case MPI_INT64_T: for(i=0;i<(*len);i++) { MPI_OP_IMPL(int64_t); } break; \
  case MPI_UINT8_T: for(i=0;i<(*len);i++) { MPI_OP_IMPL(uint8_t); } break; \
  case MPI_UINT16_T: for(i=0;i<(*len);i++) { MPI_OP_IMPL(uint16_t); } break; \
  case MPI_UINT32_T: for(i=0;i<(*len);i++) { MPI_OP_IMPL(uint32_t); } break; \
  case MPI_UINT64_T: for(i=0;i<(*len);i++) { MPI_OP_IMPL(uint64_t); } break; \
  case MPI_FLOAT_COMPLEX: for(i=0;i<(*len);i++) { MPI_OP_IMPL(std::complex<float>); } break; \
  case MPI_LONG_DOUBLE_COMPLEX: for(i=0;i<(*len);i++) { MPI_OP_IMPL(std::complex<long double>); } break; \
  case MPI_AINT: for(i=0;i<(*len);i++) { MPI_OP_IMPL(MPI_Aint); } break; \
  default: \
           ckerr << "Type " << *datatype << " with Op "#OPNAME" not supported." << endl; \
  CkAbort("Unsupported MPI datatype for MPI Op"); \
};\

/* For MPI_LAND, MPI_LOR, and MPI_LXOR: */
#define MPI_LOGICAL_OP_SWITCH(OPNAME) \
  int i; \
switch (*datatype) { \
  case MPI_SHORT: for(i=0;i<(*len);i++) { MPI_OP_IMPL(signed short int); } break; \
  case MPI_INT: for(i=0;i<(*len);i++) { MPI_OP_IMPL(signed int); } break; \
  case MPI_LONG: for(i=0;i<(*len);i++) { MPI_OP_IMPL(signed long); } break; \
  case MPI_UNSIGNED_CHAR: for(i=0;i<(*len);i++) { MPI_OP_IMPL(unsigned char); } break; \
  case MPI_UNSIGNED_SHORT: for(i=0;i<(*len);i++) { MPI_OP_IMPL(unsigned short); } break; \
  case MPI_UNSIGNED: for(i=0;i<(*len);i++) { MPI_OP_IMPL(unsigned int); } break; \
  case MPI_UNSIGNED_LONG: for(i=0;i<(*len);i++) { MPI_OP_IMPL(unsigned long); } break; \
  case MPI_LONG_LONG_INT: for(i=0;i<(*len);i++) { MPI_OP_IMPL(signed long long); } break; \
  case MPI_SIGNED_CHAR: for(i=0;i<(*len);i++) { MPI_OP_IMPL(signed char); } break; \
  case MPI_UNSIGNED_LONG_LONG: for(i=0;i<(*len);i++) { MPI_OP_IMPL(unsigned long long); } break; \
  case MPI_INT8_T: for(i=0;i<(*len);i++) { MPI_OP_IMPL(int8_t); } break; \
  case MPI_INT16_T: for(i=0;i<(*len);i++) { MPI_OP_IMPL(int16_t); } break; \
  case MPI_INT32_T: for(i=0;i<(*len);i++) { MPI_OP_IMPL(int32_t); } break; \
  case MPI_INT64_T: for(i=0;i<(*len);i++) { MPI_OP_IMPL(int64_t); } break; \
  case MPI_UINT8_T: for(i=0;i<(*len);i++) { MPI_OP_IMPL(uint8_t); } break; \
  case MPI_UINT16_T: for(i=0;i<(*len);i++) { MPI_OP_IMPL(uint16_t); } break; \
  case MPI_UINT32_T: for(i=0;i<(*len);i++) { MPI_OP_IMPL(uint32_t); } break; \
  case MPI_UINT64_T: for(i=0;i<(*len);i++) { MPI_OP_IMPL(uint64_t); } break; \
  case MPI_LOGICAL: for(i=0;i<(*len);i++) { MPI_OP_IMPL(int); } break; \
  case MPI_C_BOOL: for(i=0;i<(*len);i++) { MPI_OP_IMPL(bool); } break; \
  case MPI_AINT: for(i=0;i<(*len);i++) { MPI_OP_IMPL(MPI_Aint); } break; \
  default: \
           ckerr << "Type " << *datatype << " with Op "#OPNAME" not supported." << endl; \
  CkAbort("Unsupported MPI datatype for MPI Op"); \
};\

/* For MPI_BAND, MPI_BOR, and MPI_BXOR: */
#define MPI_BITWISE_OP_SWITCH(OPNAME) \
  int i; \
switch (*datatype) { \
  case MPI_SHORT: for(i=0;i<(*len);i++) { MPI_OP_IMPL(signed short int); } break; \
  case MPI_INT: for(i=0;i<(*len);i++) { MPI_OP_IMPL(signed int); } break; \
  case MPI_LONG: for(i=0;i<(*len);i++) { MPI_OP_IMPL(signed long); } break; \
  case MPI_UNSIGNED_CHAR: for(i=0;i<(*len);i++) { MPI_OP_IMPL(unsigned char); } break; \
  case MPI_UNSIGNED_SHORT: for(i=0;i<(*len);i++) { MPI_OP_IMPL(unsigned short); } break; \
  case MPI_UNSIGNED: for(i=0;i<(*len);i++) { MPI_OP_IMPL(unsigned int); } break; \
  case MPI_UNSIGNED_LONG: for(i=0;i<(*len);i++) { MPI_OP_IMPL(unsigned long); } break; \
  case MPI_LONG_LONG_INT: for(i=0;i<(*len);i++) { MPI_OP_IMPL(signed long long); } break; \
  case MPI_SIGNED_CHAR: for(i=0;i<(*len);i++) { MPI_OP_IMPL(signed char); } break; \
  case MPI_UNSIGNED_LONG_LONG: for(i=0;i<(*len);i++) { MPI_OP_IMPL(unsigned long long); } break; \
  case MPI_INT8_T: for(i=0;i<(*len);i++) { MPI_OP_IMPL(int8_t); } break; \
  case MPI_INT16_T: for(i=0;i<(*len);i++) { MPI_OP_IMPL(int16_t); } break; \
  case MPI_INT32_T: for(i=0;i<(*len);i++) { MPI_OP_IMPL(int32_t); } break; \
  case MPI_INT64_T: for(i=0;i<(*len);i++) { MPI_OP_IMPL(int64_t); } break; \
  case MPI_UINT8_T: for(i=0;i<(*len);i++) { MPI_OP_IMPL(uint8_t); } break; \
  case MPI_UINT16_T: for(i=0;i<(*len);i++) { MPI_OP_IMPL(uint16_t); } break; \
  case MPI_UINT32_T: for(i=0;i<(*len);i++) { MPI_OP_IMPL(uint32_t); } break; \
  case MPI_UINT64_T: for(i=0;i<(*len);i++) { MPI_OP_IMPL(uint64_t); } break; \
  case MPI_BYTE: for(i=0;i<(*len);i++) { MPI_OP_IMPL(char); } break; \
  case MPI_AINT: for(i=0;i<(*len);i++) { MPI_OP_IMPL(MPI_Aint); } break; \
  default: \
           ckerr << "Type " << *datatype << " with Op "#OPNAME" not supported." << endl; \
  CkAbort("Unsupported MPI datatype for MPI Op"); \
};\

void MPI_MAX_USER_FN( void *invec, void *inoutvec, int *len, MPI_Datatype *datatype){
#define MPI_OP_IMPL(type) \
  if(((type *)invec)[i] > ((type *)inoutvec)[i]) ((type *)inoutvec)[i] = ((type *)invec)[i];
  MPI_MINMAX_OP_SWITCH(MPI_MAX)
#undef MPI_OP_IMPL
}

void MPI_MIN_USER_FN( void *invec, void *inoutvec, int *len, MPI_Datatype *datatype){
#define MPI_OP_IMPL(type) \
  if(((type *)invec)[i] < ((type *)inoutvec)[i]) ((type *)inoutvec)[i] = ((type *)invec)[i];
  MPI_MINMAX_OP_SWITCH(MPI_MIN)
#undef MPI_OP_IMPL
}

void MPI_SUM_USER_FN( void *invec, void *inoutvec, int *len, MPI_Datatype *datatype){
#define MPI_OP_IMPL(type) \
  ((type *)inoutvec)[i] += ((type *)invec)[i];
  MPI_SUMPROD_OP_SWITCH(MPI_SUM)
#undef MPI_OP_IMPL
}

void MPI_PROD_USER_FN( void *invec, void *inoutvec, int *len, MPI_Datatype *datatype){
#define MPI_OP_IMPL(type) \
  ((type *)inoutvec)[i] *= ((type *)invec)[i];
  MPI_SUMPROD_OP_SWITCH(MPI_PROD)
#undef MPI_OP_IMPL
}

void MPI_REPLACE_USER_FN( void *invec, void *inoutvec, int *len, MPI_Datatype *datatype){
#define MPI_OP_IMPL(type) \
  ((type *)inoutvec)[i] = ((type *)invec)[i];
  MPI_SUMPROD_OP_SWITCH(MPI_REPLACE)
#undef MPI_OP_IMPL
}

void MPI_NO_OP_USER_FN( void *invec, void *inoutvec, int *len, MPI_Datatype *datatype){
  /* no-op */
}

void MPI_LAND_USER_FN( void *invec, void *inoutvec, int *len, MPI_Datatype *datatype){
#define MPI_OP_IMPL(type) \
  ((type *)inoutvec)[i] = ((type *)inoutvec)[i] && ((type *)invec)[i];
  MPI_LOGICAL_OP_SWITCH(MPI_LAND)
#undef MPI_OP_IMPL
}

void MPI_BAND_USER_FN( void *invec, void *inoutvec, int *len, MPI_Datatype *datatype){
#define MPI_OP_IMPL(type) \
  ((type *)inoutvec)[i] = ((type *)inoutvec)[i] & ((type *)invec)[i];
  MPI_BITWISE_OP_SWITCH(MPI_BAND)
#undef MPI_OP_IMPL
}

void MPI_LOR_USER_FN( void *invec, void *inoutvec, int *len, MPI_Datatype *datatype){
#define MPI_OP_IMPL(type) \
  ((type *)inoutvec)[i] = ((type *)inoutvec)[i] || ((type *)invec)[i];
  MPI_LOGICAL_OP_SWITCH(MPI_LOR)
#undef MPI_OP_IMPL
}

void MPI_BOR_USER_FN( void *invec, void *inoutvec, int *len, MPI_Datatype *datatype){
#define MPI_OP_IMPL(type) \
  ((type *)inoutvec)[i] = ((type *)inoutvec)[i] | ((type *)invec)[i];
  MPI_BITWISE_OP_SWITCH(MPI_BOR)
#undef MPI_OP_IMPL
}

void MPI_LXOR_USER_FN( void *invec, void *inoutvec, int *len, MPI_Datatype *datatype){
#define MPI_OP_IMPL(type) \
  ((type *)inoutvec)[i] = (((type *)inoutvec)[i]&&(!((type *)invec)[i]))||(!(((type *)inoutvec)[i])&&((type *)invec)[i]);
  MPI_LOGICAL_OP_SWITCH(MPI_LXOR)
#undef MPI_OP_IMPL
}

void MPI_BXOR_USER_FN( void *invec, void *inoutvec, int *len, MPI_Datatype *datatype){
#define MPI_OP_IMPL(type) \
  ((type *)inoutvec)[i] = ((type *)inoutvec)[i] ^ ((type *)invec)[i];
  MPI_BITWISE_OP_SWITCH(MPI_BXOR)
#undef MPI_OP_IMPL
}

#ifndef MIN
#define MIN(a,b) (a < b ? a : b)
#endif

void MPI_MAXLOC_USER_FN( void *invec, void *inoutvec, int *len, MPI_Datatype *datatype){
  int i;

  switch (*datatype) {
    case MPI_FLOAT_INT:
      for(i=0;i<(*len);i++){
        if(((FloatInt *)invec)[i].val > ((FloatInt *)inoutvec)[i].val)
          ((FloatInt *)inoutvec)[i] = ((FloatInt *)invec)[i];
        else if(((FloatInt *)invec)[i].val == ((FloatInt *)inoutvec)[i].val)
          ((FloatInt *)inoutvec)[i].idx = MIN(((FloatInt *)inoutvec)[i].idx, ((FloatInt *)invec)[i].idx);
      }
      break;
    case MPI_DOUBLE_INT:
      for(i=0;i<(*len);i++){
        if(((DoubleInt *)invec)[i].val > ((DoubleInt *)inoutvec)[i].val)
          ((DoubleInt *)inoutvec)[i] = ((DoubleInt *)invec)[i];
        else if(((DoubleInt *)invec)[i].val == ((DoubleInt *)inoutvec)[i].val)
          ((DoubleInt *)inoutvec)[i].idx = MIN(((DoubleInt *)inoutvec)[i].idx, ((DoubleInt *)invec)[i].idx);
      }
      break;
    case MPI_LONG_INT:
      for(i=0;i<(*len);i++){
        if(((LongInt *)invec)[i].val > ((LongInt *)inoutvec)[i].val)
          ((LongInt *)inoutvec)[i] = ((LongInt *)invec)[i];
        else if(((LongInt *)invec)[i].val == ((LongInt *)inoutvec)[i].val)
          ((LongInt *)inoutvec)[i].idx = MIN(((LongInt *)inoutvec)[i].idx, ((LongInt *)invec)[i].idx);
      }
      break;
    case MPI_2INT:
      for(i=0;i<(*len);i++){
        if(((IntInt *)invec)[i].val > ((IntInt *)inoutvec)[i].val)
          ((IntInt *)inoutvec)[i] = ((IntInt *)invec)[i];
        else if(((IntInt *)invec)[i].val == ((IntInt *)inoutvec)[i].val)
          ((IntInt *)inoutvec)[i].idx = MIN(((IntInt *)inoutvec)[i].idx, ((IntInt *)invec)[i].idx);
      }
      break;
    case MPI_SHORT_INT:
      for(i=0;i<(*len);i++){
        if(((ShortInt *)invec)[i].val > ((ShortInt *)inoutvec)[i].val)
          ((ShortInt *)inoutvec)[i] = ((ShortInt *)invec)[i];
        else if(((ShortInt *)invec)[i].val == ((ShortInt *)inoutvec)[i].val)
          ((ShortInt *)inoutvec)[i].idx = MIN(((ShortInt *)inoutvec)[i].idx, ((ShortInt *)invec)[i].idx);
      }
      break;
    case MPI_LONG_DOUBLE_INT:
      for(i=0;i<(*len);i++){
        if(((LongdoubleInt *)invec)[i].val > ((LongdoubleInt *)inoutvec)[i].val)
          ((LongdoubleInt *)inoutvec)[i] = ((LongdoubleInt *)invec)[i];
        else if(((LongdoubleInt *)invec)[i].val == ((LongdoubleInt *)inoutvec)[i].val)
          ((LongdoubleInt *)inoutvec)[i].idx = MIN(((LongdoubleInt *)inoutvec)[i].idx, ((LongdoubleInt *)invec)[i].idx);
      }
      break;
    case MPI_2FLOAT:
      for(i=0;i<(*len);i++){
        if(((FloatFloat *)invec)[i].val > ((FloatFloat *)inoutvec)[i].val)
          ((FloatFloat *)inoutvec)[i] = ((FloatFloat *)invec)[i];
        else if(((FloatFloat *)invec)[i].val == ((FloatFloat *)inoutvec)[i].val)
          ((FloatFloat *)inoutvec)[i].idx = MIN(((FloatFloat *)inoutvec)[i].idx, ((FloatFloat *)invec)[i].idx);
      }
      break;
    case MPI_2DOUBLE:
      for(i=0;i<(*len);i++){
        if(((DoubleDouble *)invec)[i].val > ((DoubleDouble *)inoutvec)[i].val)
          ((DoubleDouble *)inoutvec)[i] = ((DoubleDouble *)invec)[i];
        else if(((DoubleDouble *)invec)[i].val == ((DoubleDouble *)inoutvec)[i].val)
          ((DoubleDouble *)inoutvec)[i].idx = MIN(((DoubleDouble *)inoutvec)[i].idx, ((DoubleDouble *)invec)[i].idx);
      }
      break;
    default:
      ckerr << "Type " << *datatype << " with Op MPI_MAXLOC not supported." << endl;
      CkAbort("exiting");
  }
}

void MPI_MINLOC_USER_FN( void *invec, void *inoutvec, int *len, MPI_Datatype *datatype){
  int i;
  switch (*datatype) {
    case MPI_FLOAT_INT:
      for(i=0;i<(*len);i++){
        if(((FloatInt *)invec)[i].val < ((FloatInt *)inoutvec)[i].val)
          ((FloatInt *)inoutvec)[i] = ((FloatInt *)invec)[i];
        else if(((FloatInt *)invec)[i].val == ((FloatInt *)inoutvec)[i].val)
          ((FloatInt *)inoutvec)[i].idx = MIN(((FloatInt *)inoutvec)[i].idx, ((FloatInt *)invec)[i].idx);
      }
      break;
    case MPI_DOUBLE_INT:
      for(i=0;i<(*len);i++){
        if(((DoubleInt *)invec)[i].val < ((DoubleInt *)inoutvec)[i].val)
          ((DoubleInt *)inoutvec)[i] = ((DoubleInt *)invec)[i];
        else if(((DoubleInt *)invec)[i].val == ((DoubleInt *)inoutvec)[i].val)
          ((DoubleInt *)inoutvec)[i].idx = MIN(((DoubleInt *)inoutvec)[i].idx, ((DoubleInt *)invec)[i].idx);
      }
      break;
    case MPI_LONG_INT:
      for(i=0;i<(*len);i++){
        if(((LongInt *)invec)[i].val < ((LongInt *)inoutvec)[i].val)
          ((LongInt *)inoutvec)[i] = ((LongInt *)invec)[i];
        else if(((LongInt *)invec)[i].val == ((LongInt *)inoutvec)[i].val)
          ((LongInt *)inoutvec)[i].idx = MIN(((LongInt *)inoutvec)[i].idx, ((LongInt *)invec)[i].idx);
      }
      break;
    case MPI_2INT:
      for(i=0;i<(*len);i++){
        if(((IntInt *)invec)[i].val < ((IntInt *)inoutvec)[i].val)
          ((IntInt *)inoutvec)[i] = ((IntInt *)invec)[i];
        else if(((IntInt *)invec)[i].val == ((IntInt *)inoutvec)[i].val)
          ((IntInt *)inoutvec)[i].idx = MIN(((IntInt *)inoutvec)[i].idx, ((IntInt *)invec)[i].idx);
      }
      break;
    case MPI_SHORT_INT:
      for(i=0;i<(*len);i++){
        if(((ShortInt *)invec)[i].val < ((ShortInt *)inoutvec)[i].val)
          ((ShortInt *)inoutvec)[i] = ((ShortInt *)invec)[i];
        else if(((ShortInt *)invec)[i].val == ((ShortInt *)inoutvec)[i].val)
          ((ShortInt *)inoutvec)[i].idx = MIN(((ShortInt *)inoutvec)[i].idx, ((ShortInt *)invec)[i].idx);
      }
      break;
    case MPI_LONG_DOUBLE_INT:
      for(i=0;i<(*len);i++){
        if(((LongdoubleInt *)invec)[i].val < ((LongdoubleInt *)inoutvec)[i].val)
          ((LongdoubleInt *)inoutvec)[i] = ((LongdoubleInt *)invec)[i];
        else if(((LongdoubleInt *)invec)[i].val == ((LongdoubleInt *)inoutvec)[i].val)
          ((LongdoubleInt *)inoutvec)[i].idx = MIN(((LongdoubleInt *)inoutvec)[i].idx, ((LongdoubleInt *)invec)[i].idx);
      }
      break;
    case MPI_2FLOAT:
      for(i=0;i<(*len);i++){
        if(((FloatFloat *)invec)[i].val < ((FloatFloat *)inoutvec)[i].val)
          ((FloatFloat *)inoutvec)[i] = ((FloatFloat *)invec)[i];
        else if(((FloatFloat *)invec)[i].val == ((FloatFloat *)inoutvec)[i].val)
          ((FloatFloat *)inoutvec)[i].idx = MIN(((FloatFloat *)inoutvec)[i].idx, ((FloatFloat *)invec)[i].idx);
      }
      break;
    case MPI_2DOUBLE:
      for(i=0;i<(*len);i++){
        if(((DoubleDouble *)invec)[i].val < ((DoubleDouble *)inoutvec)[i].val)
          ((DoubleDouble *)inoutvec)[i] = ((DoubleDouble *)invec)[i];
        else if(((DoubleDouble *)invec)[i].val == ((DoubleDouble *)inoutvec)[i].val)
          ((DoubleDouble *)inoutvec)[i].idx = MIN(((DoubleDouble *)inoutvec)[i].idx, ((DoubleDouble *)invec)[i].idx);
      }
      break;
    default:
      ckerr << "Type " << *datatype << " with Op MPI_MINLOC not supported." << endl;
      CkAbort("exiting");
  }
}

/*
 * AMPI's generic reducer type, AmpiReducer, is used only
 * for MPI_Op/MPI_Datatype combinations that Charm++ does
 * not have built-in support for. AmpiReducer reduction
 * contributions all contain an AmpiOpHeader, that contains
 * the function pointer to an MPI_User_function* that is
 * applied to all contributions in AmpiReducerFunc().
 *
 * If AmpiReducer is used, the final reduction message will
 * have an additional sizeof(AmpiOpHeader) bytes in the
 * buffer before any user data. ampi::processRednMsg() strips
 * the header.
 *
 * If a non-commutative (user-defined) reduction is used,
 * ampi::processNoncommutativeRednMsg() strips the headers
 * and applies the op to all contributions in rank order.
 */
CkReduction::reducerType AmpiReducer;

// every msg contains a AmpiOpHeader structure before user data
CkReductionMsg *AmpiReducerFunc(int nMsg, CkReductionMsg **msgs) noexcept {
  AmpiOpHeader *hdr = (AmpiOpHeader *)msgs[0]->getData();
  MPI_Datatype dtype;
  int szhdr, szdata, len;
  MPI_User_function* func;
  func = hdr->func;
  dtype = hdr->dtype;
  szdata = hdr->szdata;
  len = hdr->len;
  szhdr = sizeof(AmpiOpHeader);

  CkReductionMsg *retmsg = CkReductionMsg::buildNew(szhdr+szdata,NULL,AmpiReducer,msgs[0]);
  void *retPtr = (char *)retmsg->getData() + szhdr;
  for(int i=1;i<nMsg;i++){
    (*func)((void *)((char *)msgs[i]->getData()+szhdr),retPtr,&len,&dtype);
  }
  return retmsg;
}

static CkReduction::reducerType getBuiltinReducerType(MPI_Datatype type, MPI_Op op) noexcept
{
  switch (type) {
    case MPI_INT32_T:
      if (getDDT()->getSize(MPI_INT32_T) != getDDT()->getSize(MPI_INT)) break;
      // else: fall thru to MPI_INT
    case MPI_INT:
      switch (op) {
        case MPI_MAX:  return CkReduction::max_int;
        case MPI_MIN:  return CkReduction::min_int;
        case MPI_SUM:  return CkReduction::sum_int;
        case MPI_PROD: return CkReduction::product_int;
        case MPI_LAND: return CkReduction::logical_and_int;
        case MPI_LOR:  return CkReduction::logical_or_int;
        case MPI_LXOR: return CkReduction::logical_xor_int;
        case MPI_BAND: return CkReduction::bitvec_and_int;
        case MPI_BOR:  return CkReduction::bitvec_or_int;
        case MPI_BXOR: return CkReduction::bitvec_xor_int;
        default:       break;
      }
    case MPI_FLOAT:
      switch (op) {
        case MPI_MAX:  return CkReduction::max_float;
        case MPI_MIN:  return CkReduction::min_float;
        case MPI_SUM:  return CkReduction::sum_float;
        case MPI_PROD: return CkReduction::product_float;
        default:       break;
      }
    case MPI_DOUBLE:
      switch (op) {
        case MPI_MAX:  return CkReduction::max_double;
        case MPI_MIN:  return CkReduction::min_double;
        case MPI_SUM:  return CkReduction::sum_double;
        case MPI_PROD: return CkReduction::product_double;
        default:       break;
      }
    case MPI_INT8_T:
      if (getDDT()->getSize(MPI_INT8_T) != getDDT()->getSize(MPI_CHAR)) break;
      // else: fall thru to MPI_CHAR
    case MPI_CHAR:
      switch (op) {
        case MPI_MAX:  return CkReduction::max_char;
        case MPI_MIN:  return CkReduction::min_char;
        case MPI_SUM:  return CkReduction::sum_char;
        case MPI_PROD: return CkReduction::product_char;
        default:       break;
      }
    case MPI_INT16_T:
      if (getDDT()->getSize(MPI_INT16_T) != getDDT()->getSize(MPI_SHORT)) break;
      // else: fall thru to MPI_SHORT
    case MPI_SHORT:
      switch (op) {
        case MPI_MAX:  return CkReduction::max_short;
        case MPI_MIN:  return CkReduction::min_short;
        case MPI_SUM:  return CkReduction::sum_short;
        case MPI_PROD: return CkReduction::product_short;
        default:       break;
      }
    case MPI_LONG:
      switch (op) {
        case MPI_MAX:  return CkReduction::max_long;
        case MPI_MIN:  return CkReduction::min_long;
        case MPI_SUM:  return CkReduction::sum_long;
        case MPI_PROD: return CkReduction::product_long;
        default:       break;
      }
    case MPI_INT64_T:
      if (getDDT()->getSize(MPI_INT64_T) != getDDT()->getSize(MPI_LONG_LONG)) break;
      // else: fall thru to MPI_LONG_LONG
    case MPI_LONG_LONG:
      switch (op) {
        case MPI_MAX:  return CkReduction::max_long_long;
        case MPI_MIN:  return CkReduction::min_long_long;
        case MPI_SUM:  return CkReduction::sum_long_long;
        case MPI_PROD: return CkReduction::product_long_long;
        default:       break;
      }
    case MPI_UINT8_T:
      if (getDDT()->getSize(MPI_UINT8_T) != getDDT()->getSize(MPI_UNSIGNED_CHAR)) break;
      // else: fall thru to MPI_UNSIGNED_CHAR
    case MPI_UNSIGNED_CHAR:
      switch (op) {
        case MPI_MAX:  return CkReduction::max_uchar;
        case MPI_MIN:  return CkReduction::min_uchar;
        case MPI_SUM:  return CkReduction::sum_uchar;
        case MPI_PROD: return CkReduction::product_uchar;
        default:       break;
      }
    case MPI_UINT16_T:
      if (getDDT()->getSize(MPI_UINT16_T) != getDDT()->getSize(MPI_UNSIGNED_SHORT)) break;
      // else: fall thru to MPI_UNSIGNED_SHORT
    case MPI_UNSIGNED_SHORT:
      switch (op) {
        case MPI_MAX:  return CkReduction::max_ushort;
        case MPI_MIN:  return CkReduction::min_ushort;
        case MPI_SUM:  return CkReduction::sum_ushort;
        case MPI_PROD: return CkReduction::product_ushort;
        default:       break;
      }
    case MPI_UINT32_T:
      if (getDDT()->getSize(MPI_UINT32_T) != getDDT()->getSize(MPI_UNSIGNED)) break;
      // else: fall thru to MPI_UNSIGNED
    case MPI_UNSIGNED:
      switch (op) {
        case MPI_MAX:  return CkReduction::max_uint;
        case MPI_MIN:  return CkReduction::min_uint;
        case MPI_SUM:  return CkReduction::sum_uint;
        case MPI_PROD: return CkReduction::product_uint;
        default:       break;
      }
    case MPI_UNSIGNED_LONG:
      switch (op) {
        case MPI_MAX:  return CkReduction::max_ulong;
        case MPI_MIN:  return CkReduction::min_ulong;
        case MPI_SUM:  return CkReduction::sum_ulong;
        case MPI_PROD: return CkReduction::product_ulong;
        default:       break;
      }
    case MPI_UINT64_T:
      if (getDDT()->getSize(MPI_UINT64_T) != getDDT()->getSize(MPI_UNSIGNED_LONG_LONG)) break;
      // else: fall thru to MPI_UNSIGNED_LONG_LONG
    case MPI_UNSIGNED_LONG_LONG:
      switch (op) {
        case MPI_MAX:  return CkReduction::max_ulong_long;
        case MPI_MIN:  return CkReduction::min_ulong_long;
        case MPI_SUM:  return CkReduction::sum_ulong_long;
        case MPI_PROD: return CkReduction::product_ulong_long;
        default:       break;
      }
    case MPI_C_BOOL:
      switch (op) {
        case MPI_LAND: return CkReduction::logical_and_bool;
        case MPI_LOR:  return CkReduction::logical_or_bool;
        case MPI_LXOR: return CkReduction::logical_xor_bool;
        default:       break;
      }
    case MPI_LOGICAL:
      switch (op) {
        case MPI_LAND: return CkReduction::logical_and_int;
        case MPI_LOR:  return CkReduction::logical_or_int;
        case MPI_LXOR: return CkReduction::logical_xor_int;
        default:       break;
      }
    case MPI_BYTE:
      switch (op) {
        case MPI_BAND: return CkReduction::bitvec_and_bool;
        case MPI_BOR:  return CkReduction::bitvec_or_bool;
        case MPI_BXOR: return CkReduction::bitvec_xor_bool;
        default:       break;
      }
    default:
      break;
  }
  return CkReduction::invalid;
}

struct Builtin_kvs {
  int tag_ub = MPI_TAG_UB_VALUE;
  int host = MPI_PROC_NULL;
  int io = 0;
  int wtime_is_global = 0;
  int appnum = 0;
  int lastusedcode = MPI_ERR_LASTCODE;
  int universe_size = 0;

  int mype = CkMyPe();
  int numpes = CkNumPes();
  int mynode = CkMyNode();
  int numnodes = CkNumNodes();
};

// ------------ startup support -----------
FLINKAGE void FTN_NAME(MPI_MAIN,mpi_main)(void);

static int AMPI_threadstart_idx = -1;

/*Startup routine used if user *doesn't* write
  a TCHARM_User_setup routine.
 */
CLINKAGE
void AMPI_Setup(void) {
  STARTUP_DEBUG("AMPI_Setup")
  int _nchunks=TCHARM_Get_num_chunks();
  //Make a new threads array:
  TCHARM_Create(_nchunks,AMPI_threadstart_idx);
}

int AMPI_PE_LOCAL_THRESHOLD = AMPI_PE_LOCAL_THRESHOLD_DEFAULT;
int AMPI_NODE_LOCAL_THRESHOLD = AMPI_NODE_LOCAL_THRESHOLD_DEFAULT;
int AMPI_RDMA_THRESHOLD = AMPI_RDMA_THRESHOLD_DEFAULT;
int AMPI_SSEND_THRESHOLD = AMPI_SSEND_THRESHOLD_DEFAULT;
int AMPI_MSG_POOL_SIZE = AMPI_MSG_POOL_SIZE_DEFAULT;
int AMPI_POOLED_MSG_SIZE = AMPI_POOLED_MSG_SIZE_DEFAULT;

bool ampi_nodeinit_has_been_called=false;
CtvDeclare(ampiParent*, ampiPtr);
CtvDeclare(bool, ampiInitDone);
CtvDeclare(void*,stackBottom);
CtvDeclare(bool, ampiFinalized);
CkpvDeclare(Builtin_kvs, bikvs);
CkpvDeclare(int, ampiThreadLevel);
CkpvDeclare(AmpiMsgPool, msgPool);

CLINKAGE
long ampiCurrentStackUsage(void){
  int localVariable;

  unsigned long p1 =  (unsigned long)(uintptr_t)((void*)&localVariable);
  unsigned long p2 =  (unsigned long)(uintptr_t)(CtvAccess(stackBottom));

  if(p1 > p2)
    return p1 - p2;
  else
    return  p2 - p1;
}

FLINKAGE
void FTN_NAME(AMPICURRENTSTACKUSAGE, ampicurrentstackusage)(void){
  long usage = ampiCurrentStackUsage();
  CkPrintf("[%d] Stack usage is currently %ld\n", CkMyPe(), usage);
}

CLINKAGE
void AMPI_threadstart(void *data);

#if CMK_TRACE_ENABLED
CsvExtern(funcmap*, tcharm_funcmap);
#endif

// Predefined datatype's and op's are readonly, so store them only once per process here:
static const std::array<const CkDDT_DataType *, AMPI_MAX_PREDEFINED_TYPE+1> ampiPredefinedTypes = CkDDT::createPredefinedTypes();

static constexpr std::array<MPI_User_function*, AMPI_MAX_PREDEFINED_OP+1> ampiPredefinedOps = {{
  MPI_MAX_USER_FN,
  MPI_MIN_USER_FN,
  MPI_SUM_USER_FN,
  MPI_PROD_USER_FN,
  MPI_LAND_USER_FN,
  MPI_BAND_USER_FN,
  MPI_LOR_USER_FN,
  MPI_BOR_USER_FN,
  MPI_LXOR_USER_FN,
  MPI_BXOR_USER_FN,
  MPI_MAXLOC_USER_FN,
  MPI_MINLOC_USER_FN,
  MPI_REPLACE_USER_FN,
  MPI_NO_OP_USER_FN
}};

#if defined _WIN32
# ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
# endif
# ifndef NOMINMAX
#  define NOMINMAX
# endif
# include <windows.h>
#elif defined __APPLE__
# include <unistd.h>
# include <libproc.h>
#elif CMK_HAS_REALPATH || CMK_HAS_READLINK
# ifndef _GNU_SOURCE
#  define _GNU_SOURCE
# endif
# ifndef __USE_GNU
#  define __USE_GNU
# endif
# include <unistd.h>
#endif

char * ampi_binary_path;

static void getAmpiBinaryPath() noexcept
{
#if defined _WIN32
  DWORD bufsize = MAX_PATH;
  DWORD n;
  do
  {
    ampi_binary_path = (char *)realloc(ampi_binary_path, bufsize);
    SetLastError(0);
    n = GetModuleFileName(NULL, ampi_binary_path, bufsize);
    bufsize *= 2;
  }
  while (n == bufsize || GetLastError() == ERROR_INSUFFICIENT_BUFFER);

  if (n == 0)
  {
    CkError("ERROR> GetModuleFileName(): %d\n", (int)GetLastError());
    free(ampi_binary_path);
    ampi_binary_path = nullptr;
  }
#elif defined __APPLE__
  ampi_binary_path = (char *)malloc(PROC_PIDPATHINFO_MAXSIZE);
  pid_t pid = getpid();
  int n = proc_pidpath(pid, ampi_binary_path, PROC_PIDPATHINFO_MAXSIZE);

  if (n == 0)
  {
    CkError("ERROR> proc_pidpath(): %s\n", strerror(errno));
    free(ampi_binary_path);
    ampi_binary_path = nullptr;
  }
#elif CMK_HAS_REALPATH
  ampi_binary_path = realpath("/proc/self/exe", nullptr);
  if (ampi_binary_path == nullptr)
    CkError("ERROR> realpath(): %s\n", strerror(errno));
#elif CMK_HAS_READLINK
  ssize_t bufsize = 256;
  ssize_t n;
  do
  {
    ampi_binary_path = (char *)realloc(ampi_binary_path, bufsize);
    n = readlink("/proc/self/exe", ampi_binary_path, bufsize-1);
    bufsize *= 2;
  }
  while (n == bufsize-1);

  if (n == -1)
  {
    CkError("ERROR> readlink(): %s\n", strerror(errno));
    free(ampi_binary_path);
    ampi_binary_path = nullptr;
  }
  else
  {
    ampi_binary_path[n] = '\0';
  }
#else
// FIXME: We do not need to abort here, only if user requests pipglobals or fsglobals
#  error "AMPI: No known way to get path to current binary."
#endif
}

static void ampiNodeInit() noexcept
{
  getAmpiBinaryPath();

#if CMK_TRACE_ENABLED
  TCharm::nodeInit(); // make sure tcharm_funcmap is set up
  int funclength = sizeof(funclist)/sizeof(char*);
  for (int i=0; i<funclength; i++) {
    int event_id = traceRegisterUserEvent(funclist[i], -1);
    CsvAccess(tcharm_funcmap)->emplace(funclist[i], event_id);
  }

  // rename chare & function to something reasonable
  // TODO: find a better way to do this
  for (int i=0; i<_chareTable.size(); i++){
    if (strcmp(_chareTable[i]->name, "dummy_thread_chare") == 0)
      _chareTable[i]->name = "AMPI";
  }
  for (int i=0; i<_entryTable.size(); i++){
    if (strcmp(_entryTable[i]->name, "dummy_thread_ep") == 0)
      _entryTable[i]->setName("rank");
  }
#endif

  TCHARM_Set_fallback_setup(AMPI_Setup);

  /* read AMPI environment variables */
  char *value;
  bool localThresholdSet = false;
  if ((value = getenv("AMPI_PE_LOCAL_THRESHOLD"))) {
    AMPI_PE_LOCAL_THRESHOLD = atoi(value);
    localThresholdSet = true;
  }
  if ((value = getenv("AMPI_NODE_LOCAL_THRESHOLD"))) {
    AMPI_NODE_LOCAL_THRESHOLD = atoi(value);
    localThresholdSet = true;
  }
  if (CkMyNode() == 0 && localThresholdSet) {
#if AMPI_PE_LOCAL_IMPL
#if AMPI_NODE_LOCAL_IMPL
    CkPrintf("AMPI> PE-local messaging threshold is %d bytes and Node-local messaging threshold is %d bytes.\n",
             AMPI_PE_LOCAL_THRESHOLD, AMPI_NODE_LOCAL_THRESHOLD);
#else
    CkPrintf("AMPI> PE-local messaging threshold is %d bytes.\n",
             AMPI_PE_LOCAL_THRESHOLD);
    if (AMPI_NODE_LOCAL_THRESHOLD != AMPI_NODE_LOCAL_THRESHOLD_DEFAULT) {
      CkPrintf("Warning: AMPI Node-local messaging threshold ignored on non-SMP build.\n");
    }
#endif
#else
    CkPrintf("Warning: AMPI local messaging threshold ignored since local sends are disabled.\n");
#endif //AMPI_PE_LOCAL_IMPL
  }
  if ((value = getenv("AMPI_RDMA_THRESHOLD"))) {
    AMPI_RDMA_THRESHOLD = atoi(value);
    if (CkMyNode() == 0) {
#if AMPI_RDMA_IMPL
      CkPrintf("AMPI> RDMA threshold is %d bytes.\n", AMPI_RDMA_THRESHOLD);
#else
      CkPrintf("Warning: AMPI RDMA threshold ignored since AMPI RDMA is disabled.\n");
#endif
    }
  }
  if ((value = getenv("AMPI_SSEND_THRESHOLD"))) {
    AMPI_SSEND_THRESHOLD = atoi(value);
    if (CkMyNode() == 0) {
      CkPrintf("AMPI> Synchronous messaging threshold is %d bytes.\n", AMPI_SSEND_THRESHOLD);
    }
  }
  if ((value = getenv("AMPI_MSG_POOL_SIZE"))) {
    AMPI_MSG_POOL_SIZE = atoi(value);
    if (CkMyNode() == 0) {
      CkPrintf("AMPI> Message pool size is %d messages.\n", AMPI_MSG_POOL_SIZE);
    }
  }
  if ((value = getenv("AMPI_POOLED_MSG_SIZE"))) {
    AMPI_POOLED_MSG_SIZE = atoi(value);
    if (CkMyNode() == 0) {
      CkPrintf("AMPI> Pooled message size is %d bytes.\n", AMPI_POOLED_MSG_SIZE);
    }
  }

  AmpiReducer = CkReduction::addReducer(AmpiReducerFunc, true /*streamable*/, "AmpiReducerFunc");

  CkAssert(AMPI_threadstart_idx == -1);    // only initialize once
  AMPI_threadstart_idx = TCHARM_Register_thread_function(AMPI_threadstart);

  ampi_nodeinit_has_been_called=true;
}

#if AMPI_PRINT_IDLE
static double totalidle=0.0, startT=0.0;
static int beginHandle, endHandle;
static void BeginIdle(void *dummy,double curWallTime) noexcept
{
  startT = curWallTime;
}
static void EndIdle(void *dummy,double curWallTime) noexcept
{
  totalidle += curWallTime - startT;
}
#endif

static void ampiProcInit() noexcept {
  CtvInitialize(ampiParent*, ampiPtr);
  CtvInitialize(bool,ampiInitDone);
  CtvInitialize(bool,ampiFinalized);
  CtvInitialize(void*,stackBottom);

  CkpvInitialize(int, ampiThreadLevel);
  CkpvAccess(ampiThreadLevel) = MPI_THREAD_SINGLE;

  CkpvInitialize(Builtin_kvs, bikvs); // built-in key-values
  CkpvAccess(bikvs) = Builtin_kvs();

  CkpvInitialize(AmpiMsgPool, msgPool); // pool of small AmpiMsg's, big enough for rendezvous messages
  CkpvAccess(msgPool) = AmpiMsgPool(AMPI_MSG_POOL_SIZE, AMPI_POOLED_MSG_SIZE);

#if AMPIMSGLOG
  char **argv=CkGetArgv();
  msgLogWrite = CmiGetArgFlag(argv, "+msgLogWrite");
  if (CmiGetArgIntDesc(argv,"+msgLogRead", &msgLogRank, "Re-play message processing order for AMPI")) {
    msgLogRead = 1;
  }
  char *procs = NULL;
  if (CmiGetArgStringDesc(argv, "+msgLogRanks", &procs, "A list of AMPI processors to record , e.g. 0,10,20-30")) {
    msgLogRanks.set(procs);
  }
  CmiGetArgString(argv, "+msgLogFilename", &msgLogFilename);
  if (CkMyPe() == 0) {
    if (msgLogWrite) CkPrintf("Writing AMPI messages of rank %s to log: %s\n", procs?procs:"", msgLogFilename);
    if (msgLogRead) CkPrintf("Reading AMPI messages of rank %s from log: %s\n", procs?procs:"", msgLogFilename);
  }
#endif

#if AMPI_PRINT_MSG_SIZES
  // Only record and print message sizes if this option is given, and only for those ranks.
  // Running with the '+syncprint' option is recommended if printing from multiple ranks.
  char *ranks = NULL;
  CkpvInitialize(CkListString, msgSizesRanks);
  if (CmiGetArgStringDesc(CkGetArgv(), "+msgSizesRanks", &ranks,
      "A list of AMPI ranks to record and print message sizes on, e.g. 0,10,20-30")) {
    CkpvAccess(msgSizesRanks).set(ranks);
  }
#endif
}

#if AMPIMSGLOG
static inline int record_msglog(int rank) noexcept {
  return msgLogRanks.includes(rank);
}
#endif

CLINKAGE
void AMPI_threadstart(void *data)
{
  STARTUP_DEBUG("MPI_threadstart")
#if CMK_TRACE_IN_CHARM
  if(CpvAccess(traceOn)) CthTraceResume(CthSelf());
#endif

  char **argv=CmiCopyArgs(CkGetArgv());
  int argc=CkGetArgc();

  // Set a pointer to somewhere close to the bottom of the stack.
  // This is used for roughly estimating the stack usage later.
  CtvAccess(stackBottom) = &argv;

  int ret = 0;
  // Only one of the following four main functions actually runs application code,
  // the others are stubs provided by compat_ampi*.
  ret += AMPI_Main_cpp();
  ret += AMPI_Main_cpp(argc,argv);
  ret += AMPI_Main_c(argc,argv);
  FTN_NAME(MPI_MAIN,mpi_main)(); // returns void
  AMPI_Exit(ret);
}

/* TCharm Semaphore ID's for AMPI startup */
#define AMPI_TCHARM_SEMAID 0x00A34100 /* __AMPI__ */
#define AMPI_BARRIER_SEMAID 0x00A34200 /* __AMPI__ */

// Create MPI_COMM_SELF from MPI_COMM_WORLD
static void createCommSelf() noexcept {
  STARTUP_DEBUG("ampiInit> creating MPI_COMM_SELF")
  MPI_Comm selfComm;
  MPI_Group worldGroup, selfGroup;
  int ranks[1] = { getAmpiInstance(MPI_COMM_WORLD)->getRank() };

  MPI_Comm_group(MPI_COMM_WORLD, &worldGroup);
  MPI_Group_incl(worldGroup, 1, ranks, &selfGroup);
  MPI_Comm_create(MPI_COMM_WORLD, selfGroup, &selfComm);
  MPI_Comm_set_name(selfComm, "MPI_COMM_SELF");

  CkAssert(selfComm == MPI_COMM_SELF);
  STARTUP_DEBUG("ampiInit> created MPI_COMM_SELF")
}

// PE-level array object cache, declared in ck.C
typedef std::unordered_map<CmiUInt8, ArrayElement*> ArrayObjMap;
CkpvExtern(ArrayObjMap, array_objs);

// We remove objects from array_objs whose performance we don't really care about
// (TCharm, ampiParent, MPI_COMM_SELF) in order to keep it smaller and faster
// for those we do care about (MPI_COMM_WORLD and other communicators).
static void removeUnimportantArrayObjsfromPeCache() noexcept {
  ampiParent* pptr = getAmpiParent();
  ArrayObjMap& arrayObjs = CkpvAccess(array_objs);
  arrayObjs.erase(pptr->getThread()->ckGetID().getID());
  arrayObjs.erase(pptr->ckGetID().getID());
  arrayObjs.erase(getAmpiInstance(MPI_COMM_SELF)->ckGetID().getID());
}

/*
   Called from MPI_Init, a collective initialization call:
   creates a new AMPI array and attaches it to the current
   set of TCHARM threads.
 */
static ampi *ampiInit(char **argv) noexcept
{
  if (CtvAccess(ampiInitDone)) return NULL; /* Already called ampiInit */
  STARTUP_DEBUG("ampiInit> begin")

  MPI_Comm new_world;
  int _nchunks;
  CkArrayOptions opts;
  CProxy_ampiParent parent;
  if (TCHARM_Element()==0) //the rank of a tcharm object
  { /* I'm responsible for building the arrays: */
    STARTUP_DEBUG("ampiInit> creating arrays")

    // FIXME: Need to serialize global communicator allocation in one place.
    //Allocate the next communicator
    //Create and attach the ampiParent array
    CkArrayID threads;
    opts=TCHARM_Attach_start(&threads,&_nchunks);
    opts.setSectionAutoDelegate(false);
    opts.setStaticInsertion(true);
    opts.setAnytimeMigration(false);

    ck::future<CkArrayID> newAmpiFuture;
    CkCallback cb(newAmpiFuture.handle());
    CProxy_ampiParent::ckNew(threads, _nchunks, opts, cb);
    parent = newAmpiFuture.get();
    newAmpiFuture.release();

    STARTUP_DEBUG("ampiInit> array size "<<_nchunks);
  }
  int *barrier = (int *)TCharm::get()->semaGet(AMPI_BARRIER_SEMAID);

  if (TCHARM_Element()==0)
  {
    //Make a new ampi array
    CkArrayID empty;
    ampiCommStruct worldComm(MPI_COMM_WORLD, empty, _nchunks);

    ck::future<CkArrayID> newAmpiFuture;
    CkCallback cb(newAmpiFuture.handle());
    CProxy_ampi::ckNew(parent, worldComm, opts, cb);
    /* CProxy_ampi arr = */ newAmpiFuture.get();
    newAmpiFuture.release();

    STARTUP_DEBUG("ampiInit> arrays created")
  }

  // Find our ampi object:
  ampi *ptr=(ampi *)TCharm::get()->semaGet(AMPI_TCHARM_SEMAID);
  CtvAccess(ampiInitDone)=true;
  CtvAccess(ampiFinalized)=false;
  STARTUP_DEBUG("ampiInit> complete")

  ampiParent* pptr = getAmpiParent();
  CkpvAccess(bikvs).universe_size = _nchunks;
  ptr->setCommName("MPI_COMM_WORLD");

  pptr->ampiInitCallDone = 0;

  CProxy_ampi cbproxy = ptr->getProxy();
  CkCallback cb(CkReductionTarget(ampi, allInitDone), cbproxy[0]);
  ptr->contribute(cb);

  while (pptr->ampiInitCallDone != 1) {
    pptr = pptr->block();
  }

  createCommSelf();

  removeUnimportantArrayObjsfromPeCache();


  return ptr;
}

//-------------------- ampiParent -------------------------
ampiParent::ampiParent(CProxy_TCharm threads_,int nRanks_) noexcept
  : threads(threads_), ampiReqs(64, &reqPool), myDDT(ampiPredefinedTypes),
    predefinedOps(ampiPredefinedOps), isTmpRProxySet(false)
{
  int barrier = 0x1234;
  STARTUP_DEBUG("ampiParent> starting up")
  thread=NULL;
  worldPtr=NULL;
  userAboutToMigrateFn=NULL;
  userJustMigratedFn=NULL;
  prepareCtv();

#if CMK_AMPI_WITH_ROMIO
  ADIO_Init_Globals(&romio_globals);
#endif

  // Allocate an empty groupStruct to represent MPI_EMPTY_GROUP
  groups.push_back(new groupStruct);

  init();

  //ensure MPI_INFO_ENV will always be first info object
  defineInfoEnv(nRanks_);
  // define Info objects for AMPI_Migrate calls
  defineInfoMigration();

  thread->semaPut(AMPI_BARRIER_SEMAID,&barrier);

  thread->setResumeAfterMigrationCallback(CkCallback(CkIndex_ampiParent::resumeAfterMigration(), thisProxy[thisIndex]));
}

ampiParent::ampiParent(CkMigrateMessage *msg) noexcept
  : CBase_ampiParent(msg), myDDT(ampiPredefinedTypes), predefinedOps(ampiPredefinedOps)
{
  thread=NULL;
  worldPtr=NULL;

  init();
}

#if CMK_AMPI_WITH_ROMIO
void ADIO_Init_Globals(struct ADIO_GlobalStruct * globals)
{
  globals->ADIOI_Flatlist = NULL;
  globals->ADIOI_Datarep_head = NULL;
  /* list of datareps registered by the user */

  /* for f2c and c2f conversion */
  globals->ADIOI_Ftable = NULL;
  globals->ADIOI_Ftable_ptr = 0;
  globals->ADIOI_Ftable_max = 0;
  globals->ADIOI_Reqtable = NULL;
  globals->ADIOI_Reqtable_ptr = 0;
  globals->ADIOI_Reqtable_max = 0;
#ifndef HAVE_MPI_INFO
  globals->MPIR_Infotable = NULL;
  globals->MPIR_Infotable_ptr = 0;
  globals->MPIR_Infotable_max = 0;
#endif

  globals->ADIOI_syshints = MPI_INFO_NULL;

  globals->ADIO_same_amode = MPI_OP_NULL;

#if defined(ROMIO_XFS) || defined(ROMIO_LUSTRE) || defined(AMPI_INTERNAL_ADIOI_DIRECT)
  globals->ADIOI_Direct_read = 0;
  globals->ADIOI_Direct_write = 0;
#endif

  globals->ADIO_Init_keyval = MPI_KEYVAL_INVALID;

  globals->ADIOI_DFLT_ERR_HANDLER = MPI_ERRORS_RETURN;

  globals->ADIOI_cb_config_list_keyval = MPI_KEYVAL_INVALID;
  globals->yylval = NULL;
  globals->token_ptr = NULL;
}

struct ADIO_GlobalStruct * ADIO_Globals()
{
  return &getAmpiParent()->romio_globals;
}
#endif

PUPfunctionpointer(MPI_MigrateFn)

void ampiParent::pup(PUP::er &p) noexcept {
  p|threads;
  p|myDDT;
  p|comms;

  p|groups;
  p|winStructList;
  p|infos;
  p|userOps;

  p|reqPool;
  ampiReqs.pup(p, &reqPool);

  p|kvlist;
  p|isTmpRProxySet;
  p|tmpRProxy;

  p|userAboutToMigrateFn;
  p|userJustMigratedFn;

  p|ampiInitCallDone;
  p|resumeOnRecv;
  p|resumeOnColl;
  p|numBlockedReqs;
  p|bsendBufferSize;
  p((char *)&bsendBuffer, sizeof(void *));

#if CMK_AMPI_WITH_ROMIO
  // requires memory-isomalloc
  pup_bytes(&p, &romio_globals, sizeof(romio_globals));
#endif

  // pup blockingReq
  AmpiReqType reqType;
  if (!p.isUnpacking()) {
    if (blockingReq) {
      reqType = blockingReq->getType();
    } else {
      reqType = AMPI_INVALID_REQ;
    }
  }
  p|reqType;
  if (reqType != AMPI_INVALID_REQ) {
    if (p.isUnpacking()) {
      switch (reqType) {
        case AMPI_I_REQ:
          blockingReq = new IReq;
          break;
        case AMPI_REDN_REQ:
          blockingReq = new RednReq;
          break;
        case AMPI_GATHER_REQ:
          blockingReq = new GatherReq;
          break;
        case AMPI_GATHERV_REQ:
          blockingReq = new GathervReq;
          break;
        case AMPI_SEND_REQ:
          blockingReq = new SendReq;
          break;
        case AMPI_SSEND_REQ:
          blockingReq = new SsendReq;
          break;
        case AMPI_ATA_REQ:
          blockingReq = new ATAReq;
          break;
        case AMPI_G_REQ:
          blockingReq = new GReq;
          break;
#if CMK_CUDA
        case AMPI_GPU_REQ:
          CkAbort("AMPI> error trying to PUP a non-migratable GPU request!");
          break;
#endif
        case AMPI_INVALID_REQ:
          CkAbort("AMPI> error trying to PUP an invalid request!");
          break;
      }
    }
    blockingReq->pup(p);
  } else {
    blockingReq = NULL;
  }
  if (p.isDeleting()) {
    delete blockingReq; blockingReq = NULL;
  }

#if AMPI_PRINT_MSG_SIZES
  p|msgSizes;
#endif
}

void ampiParent::prepareCtv() noexcept {
  thread=threads[thisIndex].ckLocal();
  if (thread==NULL) CkAbort("AMPIParent cannot find its thread!\n");
  CtvAccessOther(thread->getThread(),ampiPtr) = this;
  STARTUP_DEBUG("ampiParent> found TCharm")
}

void ampiParent::init() noexcept{
  resumeOnRecv = false;
  resumeOnColl = false;
  numBlockedReqs = 0;
  bsendBufferSize = 0;
  bsendBuffer = NULL;
  blockingReq = NULL;
#if AMPIMSGLOG
  if(msgLogWrite && record_msglog(thisIndex)){
    char fname[128];
    sprintf(fname, "%s.%d", msgLogFilename,thisIndex);
#if CMK_USE_ZLIB && 0
    fMsgLog = gzopen(fname,"wb");
    toPUPer = new PUP::tozDisk(fMsgLog);
#else
    fMsgLog = fopen(fname,"wb");
    CkAssert(fMsgLog != NULL);
    toPUPer = new PUP::toDisk(fMsgLog);
#endif
  }else if(msgLogRead){
    char fname[128];
    sprintf(fname, "%s.%d", msgLogFilename,msgLogRank);
#if CMK_USE_ZLIB && 0
    fMsgLog = gzopen(fname,"rb");
    fromPUPer = new PUP::fromzDisk(fMsgLog);
#else
    fMsgLog = fopen(fname,"rb");
    CkAssert(fMsgLog != NULL);
    fromPUPer = new PUP::fromDisk(fMsgLog);
#endif
    CkPrintf("AMPI> opened message log file: %s for replay\n", fname);
  }
#endif
}

void ampiParent::finalize() noexcept {
#if AMPIMSGLOG
  if(msgLogWrite && record_msglog(thisIndex)){
    delete toPUPer;
#if CMK_USE_ZLIB && 0
    gzclose(fMsgLog);
#else
    fclose(fMsgLog);
#endif
  }else if(msgLogRead){
    delete fromPUPer;
#if CMK_USE_ZLIB && 0
    gzclose(fMsgLog);
#else
    fclose(fMsgLog);
#endif
  }
#endif
}

void ampiParent::setUserAboutToMigrateFn(MPI_MigrateFn f) noexcept {
  userAboutToMigrateFn = f;
}

void ampiParent::setUserJustMigratedFn(MPI_MigrateFn f) noexcept {
  userJustMigratedFn = f;
}

void ampiParent::ckAboutToMigrate() noexcept {
  if (userAboutToMigrateFn) {
    CtvAccess(_curTCharm) = thread;
    CtvAccess(ampiPtr) = this;
    const int old = CthInterceptionsTemporarilyActivateStart(thread->getThread());
    (*userAboutToMigrateFn)();
    CthInterceptionsTemporarilyActivateEnd(thread->getThread(), old);
    CtvAccess(_curTCharm) = nullptr;
    CtvAccess(ampiPtr) = nullptr;
  }
}

void ampiParent::ckJustMigrated() noexcept {
  ArrayElement1D::ckJustMigrated();
  prepareCtv();
  didMigrate = true;
}

void ampiParent::resumeAfterMigration() noexcept {
  if (didMigrate && userJustMigratedFn) {
    didMigrate = false;
    CtvAccess(_curTCharm) = thread;
    CtvAccess(ampiPtr) = this;
    const int old = CthInterceptionsTemporarilyActivateStart(thread->getThread());
    (*userJustMigratedFn)();
    CthInterceptionsTemporarilyActivateEnd(thread->getThread(), old);
    CtvAccess(_curTCharm) = nullptr;
    CtvAccess(ampiPtr) = nullptr;
  }

  thread->start();
}

void ampiParent::ckJustRestored() noexcept {
  ArrayElement1D::ckJustRestored();
  prepareCtv();
}

ampiParent::~ampiParent() noexcept {
  STARTUP_DEBUG("ampiParent> destructor called")
  finalize();
}

const ampiCommStruct& ampiParent::getWorldStruct() const noexcept {
  return worldPtr->getCommStruct();
}

const ampiCommStruct& ampiParent::comm2CommStruct(MPI_Comm comm) const noexcept {
  if (comm == MPI_COMM_WORLD) return worldPtr->getCommStruct();
  return comms.getCommStruct(comm);
}

//Children call this when they are first created or just migrated
TCharm *ampiParent::registerAmpi(ampi *ptr,const ampiCommStruct &s,bool forMigration) noexcept
{
  if (thread==NULL) prepareCtv(); //Prevents CkJustMigrated race condition

  if (s.getComm()==MPI_COMM_WORLD)
  { //We now have our COMM_WORLD-- register it
    //Note that other communicators don't keep a raw pointer, so
    //they don't need to re-register on migration.
    if (worldPtr!=NULL) CkAbort("One ampiParent has two MPI_COMM_WORLDs");
    worldPtr=ptr;
  }

  if (forMigration) { //Restore AmpiRequest*'s in postedReqs:
    AmmEntry<AmpiRequest *> *e = ptr->postedReqs.first;
    while (e) {
      // AmmPupPostedReqs() packed these as MPI_Requests
      MPI_Request reqIdx = (MPI_Request)(intptr_t)e->msg;
      CkAssert(reqIdx != MPI_REQUEST_NULL);
      AmpiRequest* req = ampiReqs[reqIdx];
      CkAssert(req);
      e->msg = req;
      e = e->next;
    }
  }
  else { //Register the new communicator:
    MPI_Comm comm = s.getComm();
    STARTUP_DEBUG("ampiParent> registering new communicator "<<comm)
    if (comm==MPI_COMM_WORLD) {
      // Pass the new ampi to the waiting ampiInit
      thread->semaPut(AMPI_TCHARM_SEMAID, ptr);
    }
    // Register the new child's ampiCommStruct with the parent
    comms.insert(s);
  }

  return thread;
}

// reduction client data - preparation for checkpointing
class ckptClientStruct {
 public:
  const char *dname;
  ampiParent *ampiPtr;
  ckptClientStruct(const char *s, ampiParent *a) noexcept : dname(s), ampiPtr(a) {}
};

static void checkpointClient(void *param,void *msg) noexcept
{
  ckptClientStruct *client = (ckptClientStruct*)param;
  const char *dname = client->dname;
  ampiParent *ampiPtr = client->ampiPtr;
  ampiPtr->Checkpoint(strlen(dname), dname);
  delete client;
}

void ampiParent::startCheckpoint(const char* dname) noexcept {
  if (thisIndex==0) {
    ckptClientStruct *clientData = new ckptClientStruct(dname, this);
    CkCallback *cb = new CkCallback(checkpointClient, clientData);
    thisProxy.ckSetReductionClient(cb);
  }
  contribute();

  ampiParent* unused = block();

}

void ampiParent::Checkpoint(int len, const char* dname) noexcept {
  if (len == 0) {
    // memory checkpoint
    CkCallback cb(CkIndex_ampiParent::ResumeThread(),thisArrayID);
    CkStartMemCheckpoint(cb);
  }
  else {
    char dirname[256];
    strncpy(dirname,dname,len);
    dirname[len]='\0';
    CkCallback cb(CkIndex_ampiParent::ResumeThread(),thisArrayID);
    CkStartCheckpoint(dirname,cb);
  }
}

void ampiParent::ResumeThread() noexcept {
  thread->resume();
}

int ampiParent::createKeyval(MPI_Comm_copy_attr_function *copy_fn, MPI_Comm_delete_attr_function *delete_fn,
                             int *keyval, void* extra_state) noexcept {
  KeyvalNode* newnode = new KeyvalNode(copy_fn, delete_fn, extra_state);
  int idx = kvlist.size();
  kvlist.resize(idx+1);
  kvlist[idx] = newnode;
  *keyval = idx;
  return 0;
}

int ampiParent::setUserAttribute(int context, std::unordered_map<int, uintptr_t> & attributes, int keyval, void *attribute_val) noexcept {
#if AMPI_ERROR_CHECKING
  if (keyval < 0 || keyval >= kvlist.size() || kvlist[keyval] == NULL) {
    return MPI_ERR_KEYVAL;
  }
#endif
  KeyvalNode &kv = *kvlist[keyval];
  auto iter = attributes.find(keyval);
  if (iter != attributes.end()) {
    void * val = (void *)iter->second;
    int ret = (*kv.delete_fn)(context, keyval, val, kv.extra_state);
    if (ret != MPI_SUCCESS) {
      return ret;
    }
    iter->second = (uintptr_t)attribute_val;
  }
  else {
    attributes.emplace(keyval, (uintptr_t)attribute_val);
    kv.incRefCount();
  }
  return MPI_SUCCESS;
}

int ampiParent::setAttrComm(MPI_Comm comm, std::unordered_map<int, uintptr_t> & attributes, int keyval, void* attribute_val) noexcept {
  switch (keyval) {
    case MPI_TAG_UB:
    case MPI_HOST:
    case MPI_IO:
    case MPI_WTIME_IS_GLOBAL:
    case MPI_APPNUM:
    case MPI_LASTUSEDCODE:
    case MPI_UNIVERSE_SIZE:
    case AMPI_MY_WTH:
    case AMPI_NUM_WTHS:
    case AMPI_MY_PROCESS:
    case AMPI_NUM_PROCESSES:
    case AMPI_MY_HOME_WTH:
      /* immutable */
      return MPI_ERR_KEYVAL;
    default:
      return setUserAttribute(comm, attributes, keyval, attribute_val);
  }
}

int ampiParent::setAttrWin(MPI_Win win, std::unordered_map<int, uintptr_t> & attributes, int keyval, void* attribute_val) noexcept {
  switch (keyval) {
    case MPI_WIN_BASE:
    case MPI_WIN_SIZE:
    case MPI_WIN_DISP_UNIT:
    case MPI_WIN_CREATE_FLAVOR:
    case MPI_WIN_MODEL:
      /* immutable */
      return MPI_ERR_KEYVAL;
    default:
      return setUserAttribute(win, attributes, keyval, attribute_val);
  }
}

int ampiParent::freeKeyval(int keyval) noexcept {
  if (keyval >= 0 && keyval < kvlist.size() && kvlist[keyval] != NULL) {
    if (kvlist[keyval]->decRefCount() == 0) {
      delete kvlist[keyval];
      kvlist[keyval] = nullptr;
    }
    return MPI_SUCCESS;
  }
  return MPI_ERR_KEYVAL;
}

bool ampiParent::getBuiltinAttributeComm(int keyval, void *attribute_val) noexcept {
  switch (keyval) {
    case MPI_TAG_UB:            *(int **)attribute_val = &(CkpvAccess(bikvs).tag_ub);            return true;
    case MPI_HOST:              *(int **)attribute_val = &(CkpvAccess(bikvs).host);              return true;
    case MPI_IO:                *(int **)attribute_val = &(CkpvAccess(bikvs).io);                return true;
    case MPI_WTIME_IS_GLOBAL:   *(int **)attribute_val = &(CkpvAccess(bikvs).wtime_is_global);   return true;
    case MPI_APPNUM:            *(int **)attribute_val = &(CkpvAccess(bikvs).appnum);            return true;
    case MPI_LASTUSEDCODE:      *(int **)attribute_val = &(CkpvAccess(bikvs).lastusedcode);      return true;
    case MPI_UNIVERSE_SIZE:     *(int **)attribute_val = &(CkpvAccess(bikvs).universe_size);     return true;
    case AMPI_MY_WTH:           *(int **)attribute_val = &(CkpvAccess(bikvs).mype);              return true;
    case AMPI_NUM_WTHS:         *(int **)attribute_val = &(CkpvAccess(bikvs).numpes);            return true;
    case AMPI_MY_PROCESS:       *(int **)attribute_val = &(CkpvAccess(bikvs).mynode);            return true;
    case AMPI_NUM_PROCESSES:    *(int **)attribute_val = &(CkpvAccess(bikvs).numnodes);          return true;
    case AMPI_MY_HOME_WTH:      myHomePE = thisArray->homePe(ckGetArrayIndex());
                                *(int **)attribute_val = &(myHomePE);                            return true;
    default: return false;
  }
}

bool ampiParent::getBuiltinAttributeWin(int keyval, void *attribute_val, WinStruct * winStruct) noexcept {
  switch (keyval) {
    case MPI_WIN_BASE:          *(void ***)attribute_val    = &winStruct->base;     return true;
    case MPI_WIN_SIZE:          *(MPI_Aint **)attribute_val = &winStruct->size;     return true;
    case MPI_WIN_DISP_UNIT:     *(int **)attribute_val = &winStruct->disp_unit;     return true;
    case MPI_WIN_CREATE_FLAVOR: *(int **)attribute_val = &winStruct->create_flavor; return true;
    case MPI_WIN_MODEL:         *(int **)attribute_val = &winStruct->model;         return true;
    default: return false;
  }
}

// Call copy_fn for each user-defined keyval
int ampiParent::dupUserAttributes(int old_context, std::unordered_map<int, uintptr_t> & old_attr, std::unordered_map<int, uintptr_t> & new_attr) noexcept {
  for (auto & attr : old_attr) {
    int keyval = attr.first;
    void *val_in = (void *)attr.second;
    void *val_out;
    int flag = 0;
    bool isValid = (keyval != MPI_KEYVAL_INVALID && kvlist[keyval] != NULL);
    if (isValid) {
      // Call the user's copy_fn
      KeyvalNode& kv = *kvlist[keyval];
      int ret = (*kv.copy_fn)(old_context, keyval, kv.extra_state, val_in, &val_out, &flag);
      if (ret != MPI_SUCCESS) {
        return ret;
      }
      if (flag == 1) {
        new_attr.emplace(keyval, (uintptr_t)val_out);
        kv.incRefCount();
      }
    }
  }
  return MPI_SUCCESS;
}

int ampiParent::freeUserAttributes(int context, std::unordered_map<int, uintptr_t> & attributes) noexcept {
  for (auto & attr : attributes) {
    int keyval = attr.first;
    KeyvalNode & kv = *kvlist[keyval];
    void * val = (void *)attr.second;
    int ret = (*kv.delete_fn)(context, keyval, val, kv.extra_state);
    if (ret != MPI_SUCCESS)
      return ret;

    if (kv.decRefCount() == 0) {
      delete kvlist[keyval];
      kvlist[keyval] = NULL;
    }
  }
  attributes.clear();
  return MPI_SUCCESS;
}

bool ampiParent::getUserAttribute(int context, std::unordered_map<int, uintptr_t> & attributes, int keyval, void *attribute_val, int *flag) noexcept {
  auto iter = attributes.find(keyval);
  if (iter == attributes.end()) {
    *flag = 0;
    return false;
  }
  else {
    *(void **)attribute_val = (void *)iter->second;
    *flag = 1;
    return true;
  }
}

int ampiParent::getAttrComm(MPI_Comm comm, std::unordered_map<int, uintptr_t> & attributes, int keyval, void *attribute_val, int *flag) noexcept {
  if (keyval == MPI_KEYVAL_INVALID) {
    *flag = 0;
    return MPI_ERR_KEYVAL;
  }
  else if (getBuiltinAttributeComm(keyval, attribute_val)) {
    *flag = 1;
    return MPI_SUCCESS;
  }
  else if (getUserAttribute(comm, attributes, keyval, attribute_val, flag)) {
    *flag = 1;
    return MPI_SUCCESS;
  }
  else {
    *flag = 0;
    return MPI_SUCCESS;
  }
}

int ampiParent::getAttrType(MPI_Datatype datatype, std::unordered_map<int, uintptr_t> & attributes, int keyval, void *attribute_val, int *flag) noexcept {
  if (keyval == MPI_KEYVAL_INVALID) {
    *flag = 0;
    return MPI_ERR_KEYVAL;
  }
  else if (getUserAttribute(datatype, attributes, keyval, attribute_val, flag)) {
    *flag = 1;
    return MPI_SUCCESS;
  }
  else {
    *flag = 0;
    return MPI_SUCCESS;
  }
}

int ampiParent::getAttrWin(MPI_Win win, std::unordered_map<int, uintptr_t> & attributes, int keyval, void *attribute_val, int *flag, WinStruct * winStruct) noexcept {
  if (keyval == MPI_KEYVAL_INVALID) {
    *flag = 0;
    return MPI_ERR_KEYVAL;
  }
  else if (getBuiltinAttributeWin(keyval, attribute_val, winStruct)) {
    *flag = 1;
    return MPI_SUCCESS;
  }
  else if (getUserAttribute(win, attributes, keyval, attribute_val, flag)) {
    *flag = 1;
    return MPI_SUCCESS;
  }
  else {
    *flag = 0;
    return MPI_SUCCESS;
  }
}

int ampiParent::deleteAttr(int context, std::unordered_map<int, uintptr_t> & attributes, int keyval) noexcept {
  auto iter = attributes.find(keyval);

  if (keyval < 0 || keyval >= kvlist.size() || kvlist[keyval] == NULL || iter == attributes.end())
    return MPI_ERR_KEYVAL;

  KeyvalNode & kv = *kvlist[keyval];
  void * val = (void *)iter->second;
  int ret = (*kv.delete_fn)(context, keyval, val, kv.extra_state);
  if (ret != MPI_SUCCESS)
    return ret;

  attributes.erase(iter);
  if (kv.decRefCount() == 0) {
    delete kvlist[keyval];
    kvlist[keyval] = nullptr;
  }

  return MPI_SUCCESS;
}

/*
 * AMPI Message Matching (Amm) queues:
 *   AmpiMsg*'s and AmpiRequest*'s are matched based on 2 ints: [tag, src].
 */

// Pt2pt msg queues:
template class Amm<AmpiMsg *, AMPI_AMM_PT2PT_POOL_SIZE>;
template class Amm<AmpiRequest *, AMPI_AMM_PT2PT_POOL_SIZE>;

// Bcast msg queues:
template class Amm<AmpiMsg *, AMPI_AMM_COLL_POOL_SIZE>;
template class Amm<AmpiRequest *, AMPI_AMM_COLL_POOL_SIZE>;

/* free all table entries but not the space pointed to by 'msg' */
template<typename T, size_t N>
void Amm<T, N>::freeAll() noexcept
{
  AmmEntry<T>* cur = first;
  while (cur) {
    AmmEntry<T>* toDel = cur;
    cur = cur->next;
    deleteEntry(toDel);
  }
}

/* free all msgs */
template<typename T, size_t N>
void Amm<T, N>::flushMsgs() noexcept
{
  T msg = get(MPI_ANY_TAG, MPI_ANY_SOURCE);
  while (msg) {
    delete msg;
    msg = get(MPI_ANY_TAG, MPI_ANY_SOURCE);
  }
}

template<typename T, size_t N>
void Amm<T, N>::put(T msg) noexcept
{
  AmmEntry<T>* e = newEntry(msg);
  *lasth = e;
  lasth = &e->next;
}

template<typename T, size_t N>
void Amm<T, N>::put(int tag, int src, T msg) noexcept
{
  AmmEntry<T>* e = newEntry(tag, src, msg);
  *lasth = e;
  lasth = &e->next;
}

template<typename T, size_t N>
bool Amm<T, N>::match(const int tags1[AMM_NTAGS], const int tags2[AMM_NTAGS]) const noexcept
{
  if (tags1[AMM_TAG]==tags2[AMM_TAG] && tags1[AMM_SRC]==tags2[AMM_SRC]) {
    // tag and src match
    return true;
  }
  else if (tags1[AMM_TAG]==tags2[AMM_TAG] && (tags1[AMM_SRC]==MPI_ANY_SOURCE || tags2[AMM_SRC]==MPI_ANY_SOURCE)) {
    // tag matches, src is MPI_ANY_SOURCE
    return true;
  }
  else if (tags1[AMM_SRC]==tags2[AMM_SRC] && (tags1[AMM_TAG]==MPI_ANY_TAG || tags2[AMM_TAG]==MPI_ANY_TAG)) {
    // src matches, tag is MPI_ANY_TAG
    return true;
  }
  else if ((tags1[AMM_SRC]==MPI_ANY_SOURCE || tags2[AMM_SRC]==MPI_ANY_SOURCE) && (tags1[AMM_TAG]==MPI_ANY_TAG || tags2[AMM_TAG]==MPI_ANY_TAG)) {
    // src and tag are MPI_ANY
    return true;
  }
  else {
    // no match
    return false;
  }
}

template<typename T, size_t N>
T Amm<T, N>::get(int tag, int src, int* rtags) noexcept
{
  AmmEntry<T> *ent, **enth;
  T msg;
  int tags[AMM_NTAGS] = { tag, src };

  enth = &first;
  while (true) {
    ent = *enth;
    if (!ent) return NULL;
    if (match(tags, ent->tags)) {
      if (rtags) memcpy(rtags, ent->tags, sizeof(int)*AMM_NTAGS);
      msg = ent->msg;
      // unlike probe, delete the matched entry:
      AmmEntry<T>* next = ent->next;
      *enth = next;
      if (!next) lasth = enth;
      deleteEntry(ent);
      return msg;
    }
    enth = &ent->next;
  }
}

template<typename T, size_t N>
T Amm<T, N>::probe(int tag, int src, int* rtags) noexcept
{
  AmmEntry<T> *ent, **enth;
  T msg;
  int tags[AMM_NTAGS] = { tag, src };
  CkAssert(rtags);

  enth = &first;
  while (true) {
    ent = *enth;
    if (!ent) return NULL;
    if (match(tags, ent->tags)) {
      memcpy(rtags, ent->tags, sizeof(int)*AMM_NTAGS);
      msg = ent->msg;
      return msg;
    }
    enth = &ent->next;
  }
}

template<typename T, size_t N>
int Amm<T, N>::size() const noexcept
{
  int n = 0;
  AmmEntry<T> *e = first;
  while (e) {
    e = e->next;
    n++;
  }
  return n;
}

template<typename T, size_t N>
void Amm<T, N>::pup(PUP::er& p, AmmPupMessageFn msgpup) noexcept
{
  int sz;
  if (!p.isUnpacking()) {
    sz = size();
    p|sz;
    AmmEntry<T> *doomed, *e = first;
    while (e) {
      pup_ints(&p, e->tags, AMM_NTAGS);
      msgpup(p, (void**)&e->msg);
      doomed = e;
      e = e->next;
      if (p.isDeleting()) {
        deleteEntry(doomed);
      }
    }
  } else { // unpacking
    p|sz;
    for (int i=0; i<sz; i++) {
      T msg;
      int tags[AMM_NTAGS];
      pup_ints(&p, tags, AMM_NTAGS);
      msgpup(p, (void**)&msg);
      put(tags[0], tags[1], msg);
    }
  }
}

//----------------------- ampi -------------------------
void ampi::init() noexcept {
  parent=NULL;
  thread=NULL;
}

ampi::ampi() noexcept
{
  /* this constructor only exists so we can create an empty array during split */
  CkAbort("Default ampi constructor should never be called");
}

ampi::ampi(CkArrayID parent_,const ampiCommStruct &s) noexcept :parentProxy(parent_), comm(s.getComm()), oorder(s.getSize())
{
  init();
  findParentAfterCreation(s);

  // My parent owns my ampiCommStruct, but keep a pointer to it
  myComm = &(parent->getCommStruct(s.getComm()));
  CkAssert(myComm != NULL);
  myComm->setArrayID(thisArrayID);
  myRank = myComm->getRankForIndex(thisIndex);
}

ampi::ampi(CkMigrateMessage *msg) noexcept : CBase_ampi(msg)
{
  init();
}

void ampi::ckJustMigrated() noexcept
{
  findParentAfterMigration();
  ArrayElement1D::ckJustMigrated();
}

void ampi::ckJustRestored() noexcept
{
  findParentAfterMigration();
  ArrayElement1D::ckJustRestored();
}

void ampi::findParentAfterMigration() noexcept {
  STARTUP_DEBUG("ampi> finding my parent")
  parent=parentProxy[thisIndex].ckLocal();
#if CMK_ERROR_CHECKING
  if (parent==NULL) CkAbort("AMPI can't find its parent!");
#endif
  myComm = &(parent->getCommStruct(getComm()));
  thread=parent->registerAmpi(this, *myComm, true);
#if CMK_ERROR_CHECKING
  if (thread==NULL) CkAbort("AMPI can't find its thread!");
#endif
}

void ampi::findParentAfterCreation(const ampiCommStruct &s) noexcept {
  STARTUP_DEBUG("ampi> finding my parent")
  parent=parentProxy[thisIndex].ckLocal();
#if CMK_ERROR_CHECKING
  if (parent==NULL) CkAbort("AMPI can't find its parent!");
#endif
  thread=parent->registerAmpi(this, s, false);
#if CMK_ERROR_CHECKING
  if (thread==NULL) CkAbort("AMPI can't find its thread!");
#endif
}

//The following method should be called on the first element of the
//ampi array
void ampi::allInitDone() noexcept {
  STARTUP_DEBUG("ampi> all mpi_init have been called")
  thisProxy.setInitDoneFlag();
}

void ampi::setInitDoneFlag() noexcept {
  parent->ampiInitCallDone=1;
  parent->getTCharmThread()->start();
}

static void AmmPupUnexpectedMsgs(PUP::er& p,void **msg) noexcept {
  CkPupMessage(p,msg,1);
  if (p.isDeleting()) delete (AmpiMsg *)*msg;
}

static void AmmPupPostedReqs(PUP::er& p,void **msg) noexcept {
  // AmpiRequests objects are PUPed by AmpiRequestList, so here we pack
  // the reqIdx of posted requests and in ampiParent::registerAmpi we
  // lookup the AmpiRequest*'s using the indices. That is necessary because
  // the ampiParent object is unpacked after the ampi objects.
  if (p.isPacking()) {
    int reqIdx = ((AmpiRequest*)*msg)->getReqIdx();
    CkAssert(reqIdx != MPI_REQUEST_NULL);
    *msg = (void*)(intptr_t)reqIdx;
  }
  pup_pointer(&p, msg);
#if CMK_ERROR_CHECKING
  if (p.isUnpacking()) {
    MPI_Request reqIdx = (MPI_Request)(intptr_t)*msg;
    CkAssert(reqIdx != MPI_REQUEST_NULL);
  }
#endif
}

void ampi::pup(PUP::er &p) noexcept
{
  p|parentProxy;
  p|myRank;
  p|comm;
  p|tmpVec;
  p|remoteProxy;
  unexpectedMsgs.pup(p, AmmPupUnexpectedMsgs);
  postedReqs.pup(p, AmmPupPostedReqs);
  unexpectedBcastMsgs.pup(p, AmmPupUnexpectedMsgs);
  postedBcastReqs.pup(p, AmmPupPostedReqs);
  p|greq_classes;
  p|oorder;
  // Do not PUP myComm here, since ampiParent owns it.
  // We update the pointer to it in findParentAfterMigration()
}

ampi::~ampi() noexcept
{
  if (CkInRestarting()) {
    // in restarting, we need to flush messages
    unexpectedMsgs.flushMsgs();
    postedReqs.freeAll();
    unexpectedBcastMsgs.flushMsgs();
    postedBcastReqs.freeAll();
  }
}

//------------------------ Communicator Splitting ---------------------
class ampiSplitKey {
 public:
  int nextComm; //MPI_Comm value of next comm
  int color; //New class of processes we'll belong to
  int key; //To determine rank in new ordering
  int rank; //Rank in old ordering
  int type; //Type of comm: intra, inter, cart, graph, etc.
  ampiSplitKey() noexcept {}
  ampiSplitKey(int nextComm_,int color_,int key_,int rank_,AmpiCommType type_) noexcept
    :nextComm(nextComm_), color(color_), key(key_), rank(rank_), type((int)type_) {}
};

/* "type" may indicate whether call is for a cartesian topology etc. */
void ampi::split(int color,int key,MPI_Comm *dest, AmpiCommType type) noexcept
{
  MPI_Comm nextComm = parent->getNextComm();
  ampiSplitKey splitKey(nextComm,color,key,myRank,type);
  int rootIdx=myComm->getIndexForRank(0);
  if (type == COMM_INTER) {
    CkCallback cb(CkIndex_ampi::splitPhaseInter(0),CkArrayIndex1D(rootIdx),myComm->getProxy());
    contribute(sizeof(splitKey),&splitKey,CkReduction::concat,cb);
  }
  else {
    CkCallback cb(CkIndex_ampi::splitPhase1(0),CkArrayIndex1D(rootIdx),myComm->getProxy());
    contribute(sizeof(splitKey),&splitKey,CkReduction::concat,cb);
  }

  ampi * dis = block(); //Resumed by ampi::registrationFinish
  nextComm = dis->parent->getNextComm()-1;
  *dest=nextComm;
}

CLINKAGE
int compareAmpiSplitKey(const void *a_, const void *b_) {
  const ampiSplitKey *a=(const ampiSplitKey *)a_;
  const ampiSplitKey *b=(const ampiSplitKey *)b_;
  if (a->color!=b->color) return a->color-b->color;
  if (a->key!=b->key) return a->key-b->key;
  return a->rank-b->rank;
}

// Caller needs to eventually call newAmpi.doneInserting()
CProxy_ampi ampi::createNewChildAmpiSync() noexcept {
  CkArrayOptions opts;
  opts.bindTo(parentProxy);
  opts.setSectionAutoDelegate(false);
  opts.setNumInitial(0);
  opts.setStaticInsertion(false);
  opts.setAnytimeMigration(false);
  CkCallback initCB(CkIndex_ampi::registrationFinish(), thisProxy[thisIndex]);
  opts.setInitCallback(initCB);

  ck::future<CkArrayID> newAmpiFuture;
  CkCallback cb(newAmpiFuture.handle());
  CProxy_ampi::ckNew(opts, cb);
  auto newAmpi = newAmpiFuture.get();
  newAmpiFuture.release();
  return newAmpi;
}

CProxy_ampi ampi::createNewSplitCommArray(MPI_Comm newComm, const std::vector<int> & indices, AmpiCommType type) noexcept
{
  CProxy_ampi lastAmpi = createNewChildAmpiSync();

  //FIXME: create a new communicator for each color, instead of
  // (confusingly) re-using the same MPI_Comm number for each.
  const ampiCommStruct lastComm = ampiCommStruct(newComm, lastAmpi, indices, type);

  for (int newIdx : indices)
  {
    lastAmpi[newIdx].insert(parentProxy, lastComm);
  }

  lastAmpi.doneInserting(); // will call ampi::registrationFinish

  return lastAmpi;
}

void ampi::splitPhase1(CkReductionMsg *msg) noexcept
{
  //Order the keys, which orders the ranks properly:
  int nKeys=msg->getSize()/sizeof(ampiSplitKey);
  ampiSplitKey *keys=(ampiSplitKey *)msg->getData();
  if (nKeys!=myComm->getSize()) CkAbort("ampi::splitReduce expected a split contribution from every rank!");
  qsort(keys,nKeys,sizeof(ampiSplitKey),compareAmpiSplitKey);

  MPI_Comm newComm = -1;
  for(int i=0;i<nKeys;i++){
    if(keys[i].nextComm>newComm)
      newComm = keys[i].nextComm;
  }

  // Count how many colors there are, which is how many reductions to expect
  {
    int numColors = 1;
    int lastColor = keys[0].color;
    for (int c = 0; c < nKeys; ++c)
    {
      if (keys[c].color != lastColor)
      {
        lastColor = keys[c].color;
        ++numColors;
      }
    }
    setNumCommCreationsInProgress(numColors);
  }

  //Loop over the sorted keys, which gives us the new arrays:
  int lastColor=keys[0].color; //The color we're building an array for
  AmpiCommType type=(AmpiCommType)keys[0].type; //The type of comm we're creating
  int lastRoot=0; //C value for new rank 0 process for latest color
  for (int c=0;c<nKeys;c++)
  {
    if (keys[c].color != lastColor)
    { //Hit a new color-- need to build a new communicator and array
      const int numIndices = c - lastRoot;
      std::vector<int> indices(numIndices); //Maps rank to array indices for new array
      for (int i = 0; i < numIndices; i++)
      {
        const int idx = myComm->getIndexForRank(keys[i + lastRoot].rank);
        indices[i] = idx;
      }
      createNewSplitCommArray(newComm, indices, type);

      lastColor=keys[c].color;
      lastRoot=c;
    }
  }

  const int numIndices = nKeys - lastRoot;
  std::vector<int> indices(numIndices); //Maps rank to array indices for new array
  for (int i = 0; i < numIndices; i++)
  {
    const int idx = myComm->getIndexForRank(keys[i + lastRoot].rank);
    indices[i] = idx;
  }
  createNewSplitCommArray(newComm, indices, type);

  delete msg;
}

void ampi::registrationFinish() noexcept
{
  CkAssert(numCommCreationsInProgress > 0);
  if (--numCommCreationsInProgress == 0)
    thisProxy.unblock(); //Matches suspends at end of ampi::commCreate, split, etc.
}

void ampi::splitPhaseInter(CkReductionMsg *msg) noexcept
{
  //Order the keys, which orders the ranks properly:
  int nKeys=msg->getSize()/sizeof(ampiSplitKey);
  ampiSplitKey *keys=(ampiSplitKey *)msg->getData();
  if (nKeys!=myComm->getSize()) CkAbort("ampi::splitReduce expected a split contribution from every rank!");
  qsort(keys,nKeys,sizeof(ampiSplitKey),compareAmpiSplitKey);

  MPI_Comm newComm = -1;
  for(int i=0;i<nKeys;i++){
    if(keys[i].nextComm>newComm)
      newComm = keys[i].nextComm;
  }

  // Count how many colors there are, which is how many reductions to expect
  {
    int numColors = 1;
    int lastColor = keys[0].color;
    for (int c = 0; c < nKeys; ++c)
    {
      if (keys[c].color != lastColor)
      {
        lastColor = keys[c].color;
        ++numColors;
      }
    }
    setNumCommCreationsInProgress(numColors);
  }

  //Loop over the sorted keys, which gives us the new arrays:
  int lastColor=keys[0].color; //The color we're building an array for
  AmpiCommType type=(AmpiCommType)keys[0].type; //The type of comm we're creating
  int lastRoot=0; //C value for new rank 0 process for latest color
  for (int c=0;c<nKeys;c++)
  {
    if (keys[c].color != lastColor)
    { //Hit a new color-- need to build a new communicator and array
      const int numIndices = c - lastRoot;
      std::vector<int> indices(numIndices); //Maps rank to array indices for new array
      for (int i = 0; i < numIndices; i++)
      {
        const int idx = myComm->getIndexForRank(keys[i + lastRoot].rank);
        indices[i] = idx;
      }
      CProxy_ampi lastAmpi = createNewSplitCommArray(newComm, indices, type);
      thisProxy[0].exchangeProxyForSplitLocal(lastColor, lastAmpi);
      remoteProxy[0].exchangeProxyForSplitRemote(lastColor, lastAmpi);

      lastColor=keys[c].color;
      lastRoot=c;
    }
  }

  const int numIndices = nKeys - lastRoot;
  std::vector<int> indices(numIndices); //Maps rank to array indices for new array
  for (int i = 0; i < numIndices; i++)
  {
    const int idx = myComm->getIndexForRank(keys[i + lastRoot].rank);
    indices[i] = idx;
  }
  CProxy_ampi lastAmpi = createNewSplitCommArray(newComm, indices, type);
  thisProxy[0].exchangeProxyForSplitLocal(lastColor, lastAmpi);
  remoteProxy[0].exchangeProxyForSplitRemote(lastColor, lastAmpi);

  delete msg;
}

//-----------------create communicator from group--------------
// The procedure is like that of comm_split very much,
// so the code is shamelessly copied from above
//   1. reduction to make sure all members have called
//   2. the root in the old communicator create the new array
//   3. ampiParent::register is called to register new array as new comm
MPI_Comm ampi::commCreate(const std::vector<int>& vec, AmpiCommType type) noexcept {
  int rootIdx = vec[0];
  tmpVec = vec;
  CkCallback cb(CkReductionTarget(ampi, commCreatePhase1), CkArrayIndex1D(rootIdx), myComm->getProxy());
  int data[2] = { parent->getNextComm(), (int)type };
  contribute(sizeof(data), data, CkReduction::max_int, cb);

  if (getPosOp(thisIndex, vec) >= 0) {
    ampi * dis = block(); //Resumed by ampi::registrationFinish
    return dis->parent->getNextComm() - 1;
  } else {
    return MPI_COMM_NULL;
  }
}

void ampi::insertNewChildAmpiElements(MPI_Comm nextComm, CProxy_ampi newAmpi, AmpiCommType type) noexcept {
  const ampiCommStruct newCommStruct = ampiCommStruct(nextComm, newAmpi, tmpVec, type);
  for (int i = 0; i < tmpVec.size(); ++i) {
    newAmpi[tmpVec[i]].insert(parentProxy, newCommStruct);
  }
  newAmpi.doneInserting(); // will call ampi::registrationFinish
}

void ampi::commCreatePhase1(int nextComm, int commType) noexcept {
  setNumCommCreationsInProgress(1);
  CProxy_ampi newAmpi = createNewChildAmpiSync();
  insertNewChildAmpiElements((MPI_Comm)nextComm, newAmpi, (AmpiCommType)commType);
}

/* Virtual topology communicator creation */

// 0-dimensional cart comm: rank 0 creates a dup of COMM_SELF with topo info.
MPI_Comm ampi::cartCreate0D() noexcept {
  if (getRank() == 0) {
    tmpVec.clear();
    tmpVec.push_back(0);
    commCreatePhase1(parent->getNextComm(), COMM_CART);
    MPI_Comm newComm = parent->getNextComm()-1;
    ampiCommStruct &newCommStruct = parent->getCommStruct(newComm);
    ampiTopology *newTopo = newCommStruct.getTopology();
    newTopo->setndims(0);
    return newComm;
  }
  else {
    return MPI_COMM_NULL;
  }
}

MPI_Comm ampi::cartCreate(std::vector<int>& vec, int ndims, const int* dims) noexcept {
  if (ndims == 0) {
    return cartCreate0D();
  }

  // Subtract out ranks from the group that won't be in the new comm
  int newsize = dims[0];
  for (int i = 1; i < ndims; i++) {
    newsize *= dims[i];
  }
  for (int i = vec.size(); i > newsize; i--) {
    vec.pop_back();
  }

  return commCreate(vec, COMM_CART);
}

MPI_Comm ampi::intercommCreate(const std::vector<int>& remoteVec, const int root, MPI_Comm tcomm) noexcept {
  if (thisIndex==root) { // not everybody gets the valid rvec
    tmpVec = remoteVec;
  }
  CkCallback cb(CkReductionTarget(ampi, intercommCreatePhase1),CkArrayIndex1D(root),myComm->getProxy());
  MPI_Comm nextinter = parent->getNextComm();
  contribute(sizeof(nextinter), &nextinter,CkReduction::max_int,cb);
  ampi * dis = block(); //Resumed by ampi::registrationFinish
  return dis->parent->getNextComm() - 1;
}

void ampi::intercommCreatePhase1(MPI_Comm nextInterComm) noexcept {
  setNumCommCreationsInProgress(1);
  CProxy_ampi newAmpi = createNewChildAmpiSync();
  const std::vector<int>& lgroup = myComm->getIndices();
  const ampiCommStruct newCommstruct = ampiCommStruct(nextInterComm,newAmpi,lgroup,tmpVec);
  for(int i=0;i<lgroup.size();i++){
    int newIdx=lgroup[i];
    newAmpi[newIdx].insert(parentProxy,newCommstruct);
  }
  newAmpi.doneInserting(); // will call ampi::registrationFinish

  parentProxy[0].ExchangeProxy(newAmpi);
}

void ampi::intercommMerge(int first, MPI_Comm *ncomm) noexcept { // first valid only at local root
  if(myRank == 0 && first == 1){ // first (lower) group creates the intracommunicator for the higher group
    const std::vector<int>& lvec = myComm->getIndices();
    const std::vector<int>& rvec = myComm->getRemoteIndices();
    int rsize = rvec.size();
    tmpVec = lvec;
    for(int i=0;i<rsize;i++)
      tmpVec.push_back(rvec[i]);
    if(tmpVec.size()==0) CkAbort("Error in ampi::intercommMerge: merging empty comms!\n");
  }else{
    tmpVec.resize(0);
  }

  int rootIdx=myComm->getIndexForRank(0);
  CkCallback cb(CkReductionTarget(ampi, intercommMergePhase1),CkArrayIndex1D(rootIdx),myComm->getProxy());
  MPI_Comm nextintra = parent->getNextComm();
  contribute(sizeof(nextintra), &nextintra,CkReduction::max_int,cb);

  ampi * dis = block(); //Resumed by ampi::registrationFinish
  MPI_Comm newcomm = dis->parent->getNextComm()-1;
  *ncomm=newcomm;
}

void ampi::intercommMergePhase1(MPI_Comm nextIntraComm) noexcept {
  // gets called on two roots, first root creates the comm
  if(tmpVec.size()==0) return;

  setNumCommCreationsInProgress(1);
  CProxy_ampi newAmpi = createNewChildAmpiSync();
  insertNewChildAmpiElements(nextIntraComm, newAmpi, COMM_INTRA);
}

void ampi::topoDup(int topoType, int rank, MPI_Comm comm, MPI_Comm *newComm) noexcept
{
  split(0, rank, newComm, parent->getCommStruct(comm).getType());

  if (topoType != MPI_UNDEFINED) {
    ampiParent *disParent = getAmpiParent();
    ampiTopology *topo = disParent->getCommStruct(comm).getTopology();
    ampiTopology *newTopo = disParent->getCommStruct(*newComm).getTopology();
    newTopo->dup(topo);
  }
}

//------------------------ communication -----------------------
CMI_WARN_UNUSED_RESULT ampiParent* ampiParent::block() noexcept {
  TCharm * disThread = thread->suspend();
  ampiParent* disParent = getAmpiParent();
  disParent->thread = disThread;
  return disParent;
}
CMI_WARN_UNUSED_RESULT ampiParent* ampiParent::yield() noexcept {
  TCharm * disThread = thread->schedule();
  ampiParent* disParent = getAmpiParent();
  disParent->thread = disThread;
  return disParent;
}

CMI_WARN_UNUSED_RESULT ampiParent* ampiParent::blockOnRecv() noexcept {
  resumeOnRecv = true;
  ampiParent* disParent = block();
  disParent->resumeOnRecv = false;
  return disParent;
}

CMI_WARN_UNUSED_RESULT ampi* ampi::blockOnRecv() noexcept {
  parent->resumeOnRecv = true;
  ampi *dis = block();
  dis->parent->resumeOnRecv = false;
  return dis;
}

void ampi::setBlockingReq(AmpiRequest *req) noexcept {
  CkAssert(parent->blockingReq == NULL);
  CkAssert(parent->resumeOnColl == false);
  parent->blockingReq = req;
  parent->resumeOnColl = true;
}

// block on (All)Reduce or (All)Gather(v)
CMI_WARN_UNUSED_RESULT ampi* ampi::static_blockOnColl(ampi *dis) noexcept {

  CkAssert(dis->parent->resumeOnColl == true);
  dis = dis->block();
  dis->parent->resumeOnColl = false;


  delete dis->parent->blockingReq; dis->parent->blockingReq = NULL;
  return dis;
}

// Only "sync" messages (the first message in the rendezvous protocol)
// are delivered here. We separate this only for visibility in Projections traces.
void ampi::genericSync(AmpiMsg* msg) noexcept
{
  CkAssert(msg->isSsend());
  generic(msg);
}

void ampi::injectMsg(int size, char* buf) noexcept
{
  generic(makeAmpiMsg(thisIndex, 0, thisIndex, (void*)buf, size, MPI_CHAR, MPI_COMM_WORLD));
}

void ampi::generic(AmpiMsg* msg) noexcept
{
  MSG_ORDER_DEBUG(
    CkPrintf("AMPI vp %d %s arrival: tag=%d, src=%d, comm=%d (seq %d) resumeOnRecv %d\n",
             thisIndex, (msg->isSsend()) ? "sync" : " ", msg->getTag(), msg->getSrcRank(),
             getComm(), msg->getSeq(), parent->resumeOnRecv);
  )

  if(msg->getSeq() != 0) {
    int seqIdx = msg->getSeqIdx();
    int n=oorder.put(seqIdx,msg);
    if (n>0 && inorder(msg)) { // This message was in-order, and is not an incomplete sync message
      for (int i=1; i<n; i++) { // It enables other, previously out-of-order messages
        msg = oorder.getOutOfOrder(seqIdx);
        if (!msg || !inorder(msg)) break; // Returns false if msg is an incomplete sync message
      }
    }
  } else { //Cross-world or system messages are unordered
    inorder(msg);
  }
  // msg may be free'ed from calling inorder()

  resumeThreadIfReady();
}

// Same as ampi::generic except it's [nokeep] and msg is sequenced
void ampi::bcastResult(AmpiMsg* msg) noexcept
{
  MSG_ORDER_DEBUG(
    CkPrintf("AMPI vp %d bcast arrival: tag=%d, src=%d, comm=%d (seq %d) resumeOnRecv %d\n",
             thisIndex, msg->getTag(), msg->getSrcRank(), getComm(), msg->getSeq(), parent->resumeOnRecv);
  )

  CkAssert(msg->getSeq() != 0);
  int seqIdx = msg->getSeqIdx();
  int n=oorder.put(seqIdx,msg);
  if (n>0) { // This message was in-order
    inorderBcast(msg, false); // inorderBcast() is [nokeep]-aware, unlike inorder()
    if (n>1) { // It enables other, previously out-of-order messages
      while((msg = oorder.getOutOfOrder(seqIdx)) != nullptr) {
        inorderBcast(msg, true);
      }
    }
  }
  // [nokeep] entry method, so do not delete msg
  resumeThreadIfReady();
}

inline static AmpiRequestList &getReqs() noexcept;

void AmpiRequestList::freeNonPersReq(ampiParent* pptr, int &idx) noexcept {
  if (!reqs[idx]->isPersistent()) {
    free(idx, pptr->getDDT());
    idx = MPI_REQUEST_NULL;
  }
}

void AmpiRequestList::free(int idx, CkDDT *ddt) noexcept {
  if (idx < 0) return;
  reqs[idx]->free(ddt);
  reqPool->deleteReq(reqs[idx]);
  reqs[idx] = NULL;
  startIdx = std::min(idx, startIdx);
}

// Returns true if msg is in-order and can be completed, otherwise false
bool ampi::inorder(AmpiMsg* msg) noexcept
{
  MSG_ORDER_DEBUG(
    CkPrintf("AMPI vp %d inorder: tag=%d, src=%d, comm=%d (seq %d)\n",
             thisIndex, msg->getTag(), msg->getSrcRank(), getComm(), msg->getSeq());
  )


  //Check posted recvs:
  int tag = msg->getTag();
  int srcRank = msg->getSrcRank();
  AmpiRequest* ireq = postedReqs.get(tag, srcRank);
  if (ireq) { // receive posted
    if (msg->isSsend()) {
      // Returns false if msg is an incomplete sync message
      return ireq->receive(this, msg);
    }
    else {
      handleBlockedReq(ireq);
      ireq->receive(this, msg);
    }
  }
  else {
    unexpectedMsgs.put(msg);
  }
  return true;
}

void ampi::inorderBcast(AmpiMsg* msg, bool deleteMsg) noexcept
{
  MSG_ORDER_DEBUG(
    CkPrintf("AMPI vp %d inorder bcast: tag=%d, src=%d, comm=%d (seq %d)\n",
             thisIndex, msg->getTag(), msg->getSrcRank(), getComm(), msg->getSeq());
  )


  //Check posted recvs:
  int tag = msg->getTag();
  int srcRank = msg->getSrcRank();
  AmpiRequest* req = postedBcastReqs.get(tag, srcRank);
  if (req) { // receive posted
    handleBlockedReq(req);
    req->receive(this, msg, deleteMsg);
  } else {
    // Reference the [nokeep] msg so it isn't freed by the runtime
    CkReferenceMsg(msg);
    unexpectedBcastMsgs.put(msg);
  }
}

static inline AmpiMsg* rdma2AmpiMsg(char *buf, int size, CMK_REFNUM_TYPE seq, int tag, int srcRank) noexcept
{
  // Convert an Rdma message (parameter marshalled buffer) to an AmpiMsg
  AmpiMsg* msg = new (size, 0) AmpiMsg(seq, MPI_REQUEST_NULL, tag, srcRank, size);
  memcpy(msg->getData(), buf, size); // Assumes the buffer is contiguous
  return msg;
}

// RDMA version of ampi::generic
void ampi::genericRdma(char* buf, int size, CMK_REFNUM_TYPE seq, int tag, int srcRank) noexcept
{
  MSG_ORDER_DEBUG(
    CkPrintf("[%d] in ampi::genericRdma on index %d, size=%d, seq=%d, srcRank=%d, tag=%d, comm=%d\n",
             CkMyPe(), getIndexForRank(getRank()), size, seq, srcRank, tag, getComm());
  )

  if (seq != 0) {
    int seqIdx = srcRank;
    int n = oorder.putIfInOrder(seqIdx, seq);
    if (n > 0) { // This message was in-order
      inorderRdma(buf, size, seq, tag, srcRank);
      for (int i=1; i<n; i++) { // It enables other, previously out-of-order messages
        AmpiMsg* msg = oorder.getOutOfOrder(seqIdx);
        if (!msg || !inorder(msg)) break; // Returns false if msg is an incomplete sync message
      }
    } else { // This message was out-of-order: stash it (as an AmpiMsg)
      AmpiMsg *msg = rdma2AmpiMsg(buf, size, seq, tag, srcRank);
      oorder.putOutOfOrder(seqIdx, msg);
    }
  } else { // Cross-world or system messages are unordered
    inorderRdma(buf, size, seq, tag, srcRank);
  }

  resumeThreadIfReady();
}

// RDMA version of ampi::inorder
void ampi::inorderRdma(char* buf, int size, CMK_REFNUM_TYPE seq, int tag, int srcRank) noexcept
{
  MSG_ORDER_DEBUG(
    CkPrintf("AMPI vp %d inorderRdma: tag=%d, src=%d, comm=%d  (seq %d)\n",
             thisIndex, tag, srcRank, getComm(), seq);
  )

  //Check posted recvs:
  AmpiRequest* ireq = postedReqs.get(tag, srcRank);
  if (ireq) { // receive posted
    handleBlockedReq(ireq);
    ireq->receiveRdma(this, buf, size, srcRank);
  } else {
    AmpiMsg* msg = rdma2AmpiMsg(buf, size, seq, tag, srcRank);
    unexpectedMsgs.put(msg);
  }
}

// Callback signaling that the send buffer is now safe to re-use
void ampi::completedSend(MPI_Request reqIdx, CkNcpyBuffer *srcInfo/*=nullptr*/) noexcept
{
  MSG_ORDER_DEBUG(
     CkPrintf("[%d] VP %d in completedSend, reqIdx = %d\n",
              CkMyPe(), parent->thisIndex, reqIdx);
  )

  AmpiRequestList& reqList = getReqs();
  AmpiRequest& sreq = (*reqList[reqIdx]);
  sreq.deregisterMem(srcInfo);
  sreq.complete = true;

  handleBlockedReq(&sreq);
  resumeThreadIfReady();
}

// Callback signaling that the send buffer is now safe to re-use
void ampi::completedRdmaSend(CkDataMsg *msg) noexcept
{
  // refnum is the index into reqList for this SendReq
  int reqIdx = CkGetRefNum(msg);
  CkNcpyBuffer *srcInfo = (CkNcpyBuffer *)(msg->data);
  completedSend(reqIdx, srcInfo);
  // [nokeep] entry method, so do not delete msg
}

void ampi::completedRecv(MPI_Request reqIdx, CkNcpyBuffer *targetInfo/*=nullptr*/) noexcept
{
  MSG_ORDER_DEBUG(
    CkPrintf("[%d] VP %d in completedRecv, reqIdx = %d\n", CkMyPe(), parent->thisIndex, reqIdx);
  )
  AmpiRequestList& reqList = getReqs();
  IReq& ireq = *((IReq*)(reqList[reqIdx]));
  CkAssert(!ireq.complete);

  if (ireq.systemBuf) {
    // deserialize message from intermediate/system buffer into non-contiguous user buffer
    processRdmaMsg(ireq.systemBuf, ireq.systemBufLen, ireq.buf, ireq.count, ireq.type);
  }
  ireq.deregisterMem(targetInfo);
  ireq.complete = true;

  handleBlockedReq(&ireq);
  resumeThreadIfReady();
}

void ampi::completedRdmaRecv(CkDataMsg *msg) noexcept
{
  // refnum is the index into reqList for this IReq
  int reqIdx = CkGetRefNum(msg);
  CkNcpyBuffer *targetInfo = (CkNcpyBuffer *)(msg->data);
  completedRecv(reqIdx, targetInfo);
  // [nokeep] entry method, so do not delete msg
}

void handle_MPI_BOTTOM(void* &buf, MPI_Datatype type) noexcept
{
  if (buf == MPI_BOTTOM) {
    buf = (void*)getDDT()->getType(type)->getLB();
    getDDT()->getType(type)->setAbsolute(true);
  }
}

void handle_MPI_BOTTOM(void* &buf1, MPI_Datatype type1, void* &buf2, MPI_Datatype type2) noexcept
{
  if (buf1 == MPI_BOTTOM) {
    buf1 = (void*)getDDT()->getType(type1)->getLB();
    getDDT()->getType(type1)->setAbsolute(true);
  }
  if (buf2 == MPI_BOTTOM) {
    buf2 = (void*)getDDT()->getType(type2)->getLB();
    getDDT()->getType(type2)->setAbsolute(true);
  }
}

AmpiMsg *ampi::makeBcastMsg(const void *buf,int count,MPI_Datatype type,int root,MPI_Comm destcomm) noexcept
{
  CkDDT_DataType *ddt = getDDT()->getType(type);
  int len = ddt->getSize(count);
  CMK_REFNUM_TYPE seq = getSeqNo(root, destcomm, MPI_BCAST_TAG);
  // Do not use the msg pool for bcasts:
  AmpiMsg *msg = new (len, 0) AmpiMsg(seq, MPI_REQUEST_NULL, MPI_BCAST_TAG, root, len);
  ddt->serialize((char*)buf, msg->getData(), count, msg->getLength(), PACK);
  return msg;
}

// Create an AmpiMsg with either an AmpiNcpyShmBuffer object or a CkNcpyBuffer object
// as the msg payload based on the expected locality of the destination VP.
// We optimize for PE/Process-local transfers by:
// 1. Avoiding memory registration/pinning costs: some networks have high pinning/unpinning
//    costs, which we want to avoid when unnecessary.
// 2. Handling DDTs: instead of sending one message per contiguous
//    memory region in a DDT, we handle non-contiguous messages with a single message.
// Note: a recv'er can migrate out of the process that a sender thinks it is co-located
//       within, meaning we have to handle that case in ampi::processSsendNcpyShmMsg().
AmpiMsg *ampi::makeSyncMsg(int t,int sRank,const void *buf,int count,
                           MPI_Datatype type,CProxy_ampi destProxy,
                           int destIdx, int ssendReq,CMK_REFNUM_TYPE seq,
                           ampi* destPtr) noexcept
{
  CkAssert(ssendReq >= 0);
#if AMPI_NODE_LOCAL_IMPL
  if (destLikelyWithinProcess(destProxy, destIdx, destPtr)) {
    return makeNcpyShmMsg(t, sRank, buf, count, type, ssendReq, seq);
  }
  else
#endif
  {
    return makeNcpyMsg(t, sRank, buf, count, type, ssendReq, seq);
  }
}

// Create an AmpiMsg with an AmpiMsgType + AmpiNcpyShmBuffer object as the msg payload
AmpiMsg* ampi::makeNcpyShmMsg(int t, int sRank, const void* buf, int count,
                              MPI_Datatype type, int ssendReq, int seq) noexcept
{
  CkDDT_DataType *ddt = getDDT()->getType(type);
  int len = ddt->getSize(count);
  AmpiNcpyShmBuffer srcInfo(thisIndex, count, (char*)buf, ddt, ssendReq);
  AmpiMsgType msgType = NCPY_SHM_MSG;
  PUP::sizer pupSizer;
  pupSizer | msgType;
  pupSizer | srcInfo;
  int srcInfoLen = pupSizer.size();

  AmpiMsg *msg = CkpvAccess(msgPool).newAmpiMsg(seq, ssendReq, t, sRank, srcInfoLen);
  msg->setLength(len); // set AmpiMsg's length to be that of the real msg payload

  PUP::toMem pupPacker(msg->getData());
  pupPacker | msgType;
  pupPacker | srcInfo;
  return msg;
}

// Create an AmpiMsg with an AmpiMsgType + CkNcpyBuffer object as the msg payload
AmpiMsg* ampi::makeNcpyMsg(int t, int sRank, const void* buf, int count,
                           MPI_Datatype type, int ssendReq, int seq) noexcept
{
  CkDDT_DataType *ddt = getDDT()->getType(type);
  int len = ddt->getSize(count);
  CkCallback sendCB(CkIndex_ampi::completedRdmaSend(NULL), thisProxy[thisIndex], true /*inline*/);
  sendCB.setRefnum(ssendReq);
  SsendReq& req = *((SsendReq*)(getReqs()[ssendReq]));
  CkNcpyBuffer srcInfo;

  if (ddt->isContig()) {
    srcInfo = CkNcpyBuffer(buf, len, sendCB);
  }
  else {
    // NOTE: if DDT could provide us with a list of pointers to contiguous chunks
    //       of non-contiguous datatypes, we could send them in-place here. For
    //       now we just copy into a contiguous system buffer and then send that.
    char* sbuf = new char[len];
    ddt->serialize((char*)buf, sbuf, count, len, PACK);
    srcInfo = CkNcpyBuffer(sbuf, len, sendCB);
    req.setSystemBuf(sbuf, len); // completedSend will need to free this
    // NOTE: We could set 'req.complete = true' here, but then we'd
    //       have to make sure req.systemBuf gets freed by someone else
    //       in case 'req' is freed before the put() actually completes...
  }

  AmpiMsgType msgType = NCPY_MSG;
  PUP::sizer pupSizer;
  pupSizer | msgType;
  pupSizer | srcInfo;
  int srcInfoLen = pupSizer.size();

  AmpiMsg *msg = CkpvAccess(msgPool).newAmpiMsg(seq, ssendReq, t, sRank, srcInfoLen);
  msg->setLength(len); // set AmpiMsg's length to be that of the real msg payload

  PUP::toMem pupPacker(msg->getData());
  pupPacker | msgType;
  pupPacker | srcInfo;
  return msg;
}

AmpiMsg *ampi::makeAmpiMsg(int destRank,int t,int sRank,const void *buf,int count,
                           MPI_Datatype type,MPI_Comm destcomm) noexcept
{
  CMK_REFNUM_TYPE seq = getSeqNo(destRank, destcomm, t);
  return makeAmpiMsg(destRank, t, sRank, buf, count, type, destcomm, seq);
}

AmpiMsg *ampi::makeAmpiMsg(int destRank,int t,int sRank,const void *buf,int count,
                           MPI_Datatype type,MPI_Comm destcomm,CMK_REFNUM_TYPE seq) noexcept
{
  CkDDT_DataType *ddt = getDDT()->getType(type);
  int len = ddt->getSize(count);
  AmpiMsg *msg = CkpvAccess(msgPool).newAmpiMsg(seq, MPI_REQUEST_NULL, t, sRank, len);
  ddt->serialize((char*)buf, msg->getData(), count, msg->getLength(), PACK);
  return msg;
}

void ampi::waitOnBlockingSend(MPI_Request* req, AmpiSendType sendType) noexcept
{
  if (*req != MPI_REQUEST_NULL && (sendType == BLOCKING_SEND || sendType == BLOCKING_SSEND)) {
    AmpiRequestList& reqList = getReqs();
    AmpiRequest& sreq = *reqList[*req];
    parent = sreq.wait(parent, MPI_STATUS_IGNORE);
    parent->getReqs().free(*req, parent->getDDT());
    *req = MPI_REQUEST_NULL;
  }
}

MPI_Request ampi::send(int t, int sRank, const void* buf, int count, MPI_Datatype type,
                       int rank, MPI_Comm destcomm, AmpiSendType sendType/*=BLOCKING_SEND*/,
                       MPI_Request reqIdx/*=MPI_REQUEST_NULL*/) noexcept
{
#if CMK_TRACE_IN_CHARM
  TRACE_BG_AMPI_BREAK(thread->getThread(), "AMPI_SEND", NULL, 0, 1);
#endif


  const ampiCommStruct &dest=comm2CommStruct(destcomm);
  MPI_Request req = delesend(t,sRank,buf,count,type,rank,destcomm,dest.getProxy(),sendType,reqIdx);
  waitOnBlockingSend(&req, sendType);

#if CMK_TRACE_IN_CHARM
  TRACE_BG_AMPI_BREAK(thread->getThread(), "AMPI_SEND_END", NULL, 0, 1);
#endif

  return req;
}

void ampi::sendraw(int t, int sRank, void* buf, int len, CkArrayID aid, int idx) noexcept
{
  AmpiMsg *msg = new (len, 0) AmpiMsg(0, MPI_REQUEST_NULL, t, sRank, len);
  memcpy(msg->getData(), buf, len);
  CProxy_ampi pa(aid);
  pa[idx].generic(msg);
}

CMK_REFNUM_TYPE ampi::getSeqNo(int destRank, MPI_Comm destcomm, int tag) noexcept {
  int seqIdx = (tag >= MPI_BCAST_TAG) ? COLL_SEQ_IDX : destRank;
  CMK_REFNUM_TYPE seq = 0;
  if (tag<=MPI_BCAST_TAG) {
    seq = oorder.nextOutgoing(seqIdx);
  }
  return seq;
}

MPI_Request ampi::sendRdmaMsg(int t, int sRank, const void* buf, int size, MPI_Datatype type, int destIdx,
                              int destRank, MPI_Comm destcomm, CMK_REFNUM_TYPE seq, CProxy_ampi arrProxy,
                              MPI_Request reqIdx) noexcept
{
  // Set up a SendReq to track completion of the send buffer
  if (reqIdx == MPI_REQUEST_NULL) {
    reqIdx = postReq(parent->reqPool.newReq<SendReq>(type, destcomm, getDDT()));
  }
  CkCallback completedSendCB(CkIndex_ampi::completedRdmaSend(NULL), thisProxy[thisIndex], true/*inline*/);
  completedSendCB.setRefnum(reqIdx);

  arrProxy[destIdx].genericRdma(CkSendBuffer(buf, completedSendCB), size, seq, t, sRank);
  return reqIdx;
}

// Local version of ampi::generic but assumes msg is in-order & ireq is the matching recv req
void ampi::localInorder(char* buf, int size, int seqIdx, CMK_REFNUM_TYPE seq, int tag,
                        int srcRank, IReq* ireq) noexcept
{
  MSG_ORDER_DEBUG(
    CkPrintf("[%d] in ampi::localInorder on index %d, size=%d, seq=%d, srcRank=%d, tag=%d, comm=%d\n",
             CkMyPe(), getIndexForRank(getRank()), size, seq, srcRank, tag, getComm());
  )

  if (seq != 0) {
    int n = oorder.putIfInOrder(seqIdx, seq);
    CkAssert(n > 0); // This message must be in-order
    handleBlockedReq(ireq);
    ireq->receiveRdma(this, buf, size, srcRank);
    if (n > 1) { // It enables other, previously out-of-order messages
      AmpiMsg *msg = nullptr;
      while ((msg = oorder.getOutOfOrder(seqIdx)) != nullptr) {
        if (!inorder(msg)) break; // Returns false if msg is an incomplete sync message
      }
    }
  } else { // Cross-world or system messages are unordered
    handleBlockedReq(ireq);
    ireq->receiveRdma(this, buf, size, srcRank);
  }

  resumeThreadIfReady();
}

// Call genericRdma inline on the local destination object
MPI_Request ampi::sendLocalMsg(int tag, int srcRank, const void* buf, int size, MPI_Datatype type,
                               int count, int destRank, MPI_Comm destComm, CMK_REFNUM_TYPE seq,
                               ampi* destPtr, AmpiSendType sendType, MPI_Request reqIdx) noexcept
{
  int seqIdx = srcRank;

  if (size >= AMPI_PE_LOCAL_THRESHOLD || (sendType == BLOCKING_SSEND || sendType == I_SSEND)) {
    // Block on the matching request to avoid making an intermediate copy
    MSG_ORDER_DEBUG(
      CkPrintf("[%d] AMPI vp %d sending local msg inline using a Sync send to vp %d\n",
               CkMyPe(), parent->thisIndex, destPtr->parent->thisIndex);
    )
    if (reqIdx == MPI_REQUEST_NULL) {
      reqIdx = postReq(parent->reqPool.newReq<SsendReq>((void*)buf, count, type, destRank, tag, destComm, srcRank, getDDT(),
                                                        (sendType == BLOCKING_SSEND) ?
                                                        AMPI_REQ_BLOCKED : AMPI_REQ_PENDING));
    }
    destPtr->genericSync(makeSyncMsg(tag, srcRank, buf, count, type, destPtr->thisProxy, destPtr->thisIndex, reqIdx, seq, destPtr));
    return reqIdx;
  }
  else {
    MSG_ORDER_DEBUG(
      CkPrintf("[%d] AMPI vp %d sending local msg inline to vp %d\n",
               CkMyPe(), parent->thisIndex, destPtr->parent->thisIndex);
    )
    destPtr->generic(makeAmpiMsg(destRank, tag, srcRank, buf, count, type, destComm, seq));
    if (reqIdx == MPI_REQUEST_NULL) {
      reqIdx = postReq(parent->reqPool.newReq<SendReq>((void*)buf, count, type, destRank, tag, destComm, getDDT(),
                                                        AMPI_REQ_COMPLETED));
    }
    return reqIdx;
  }
}

MPI_Request ampi::sendSyncMsg(int t, int sRank, const void* buf, MPI_Datatype type, int count,
                              int rank, MPI_Comm destcomm, CMK_REFNUM_TYPE seq, CProxy_ampi destProxy,
                              int destIdx, AmpiSendType sendType, MPI_Request reqIdx, ampi* destPtr) noexcept
{
  if (reqIdx == MPI_REQUEST_NULL) {
    reqIdx = postReq(parent->reqPool.newReq<SsendReq>((void*)buf, count, type, rank, t, destcomm, sRank, getDDT(),
                                                      (sendType == BLOCKING_SSEND) ?
                                                      AMPI_REQ_BLOCKED : AMPI_REQ_PENDING));
  }
  // All sync messages go thru ampi::genericSync (not generic or genericRdma)
#if AMPI_PE_LOCAL_IMPL
  if (destPtr != nullptr && destPtr->parent != nullptr) {
    destPtr->genericSync(makeSyncMsg(t, sRank, buf, count, type, destProxy, destIdx, reqIdx, seq, destPtr));
  } else
#endif
  {
    destProxy[destIdx].genericSync(makeSyncMsg(t, sRank, buf, count, type, destProxy, destIdx, reqIdx, seq, NULL));
  }
  return reqIdx;
}

MPI_Request ampi::delesend(int t, int sRank, const void* buf, int count, MPI_Datatype type,
                           int rank, MPI_Comm destcomm, CProxy_ampi arrProxy, AmpiSendType sendType,
                           MPI_Request reqIdx) noexcept
{
  if (rank==MPI_PROC_NULL) return MPI_REQUEST_NULL;
  const ampiCommStruct &dest=comm2CommStruct(destcomm);
  int destIdx;
  if(isInter()){
    sRank = thisIndex;
    destIdx = dest.getIndexForRemoteRank(rank);
    arrProxy = remoteProxy;
  } else {
    destIdx = dest.getIndexForRank(rank);
  }
  CMK_REFNUM_TYPE seq = getSeqNo(rank, destcomm, t);

  MSG_ORDER_DEBUG(
    CkPrintf("AMPI vp %d send: tag=%d, src=%d, comm=%d, seq=%d (to %d)\n",
             parent->thisIndex, t, sRank, destcomm, seq, destIdx);
  )

  CkDDT_DataType *ddt = getDDT()->getType(type);
  int size = ddt->getSize(count);
  ampi *destPtr = arrProxy[destIdx].ckLocal();
#if AMPI_PE_LOCAL_IMPL
  if (destPtr != nullptr && destPtr->parent != nullptr) {
    // Complete message inline to PE-local destination VP
    return sendLocalMsg(t, sRank, buf, size, type, count, rank, destcomm,
                        seq, destPtr, sendType, reqIdx);
  }
#endif
  if (
#if AMPI_NODE_LOCAL_IMPL
      (size >= AMPI_NODE_LOCAL_THRESHOLD && destLikelyWithinProcess(arrProxy, destIdx, destPtr)) ||
#endif
      (sendType == BLOCKING_SSEND || sendType == I_SSEND))
  {
    // Avoid sender- and receiver-side copies via zero copy direct API
    // (optimized for within-process transfers)
    return sendSyncMsg(t, sRank, buf, type, count, rank, destcomm,
                       seq, arrProxy, destIdx, sendType, reqIdx, destPtr);
  }
#if AMPI_RDMA_IMPL
  if (ddt->isContig() && size >= AMPI_RDMA_THRESHOLD) {
    // Avoid sender-side copy via zero copy entry method API
    return sendRdmaMsg(t, sRank, buf, size, type, destIdx,
                       rank, destcomm, seq, arrProxy, reqIdx);
  }
#endif
  if (size >= AMPI_SSEND_THRESHOLD) {
    // Avoid sender- and receiver-side copies via zero copy direct API
    return sendSyncMsg(t, sRank, buf, type, count, rank, destcomm,
                       seq, arrProxy, destIdx, sendType, reqIdx, destPtr);
  }

  // Send via normal Charm++ message with copies on both sender- and receiver-sides
  arrProxy[destIdx].generic(makeAmpiMsg(rank, t, sRank, buf, count, type, destcomm, seq));

  if (reqIdx == MPI_REQUEST_NULL) { // Sends via generic() get a pre-completed send request
    reqIdx = postReq(parent->reqPool.newReq<SendReq>((void*)buf, count, type, rank, t, destcomm,
                                                     getDDT(), AMPI_REQ_COMPLETED));
  }
  else { // Persistent request
    AmpiRequestList& reqList = parent->ampiReqs;
    AmpiRequest& sreq = (*reqList[reqIdx]);
    CkAssert(sreq.isPersistent());
    sreq.complete = true;
  }

  return reqIdx;
}

// Invoked by recv'er when not co-located in the same process as sender.
// Assumes that the recver has posted a contiguous buffer for the put() target,
// but the send buffer may be non-contiguous.
void ampi::requestPut(MPI_Request reqIdx, CkNcpyBuffer targetInfo) noexcept {
  SsendReq& req = *((SsendReq*)(parent->ampiReqs[reqIdx]));
  MSG_ORDER_DEBUG(
    CkPrintf("[%d] VP %d in requestPut, reqIdx = %d\n", CkMyPe(), parent->thisIndex, reqIdx);
  )
  CkDDT_DataType* sddt = getDDT()->getType(req.type);
  int len = sddt->getSize(req.count);
  CkCallback sendCB(CkIndex_ampi::completedRdmaSend(NULL), thisProxy[thisIndex], true /*inline*/);
  sendCB.setRefnum(reqIdx);
  CkNcpyBuffer srcInfo;

  if (sddt->isContig()) {
    srcInfo = CkNcpyBuffer(req.buf, len, sendCB);
  }
  else {
    char* sbuf = new char[len];
    sddt->serialize((char*)req.buf, sbuf, req.count, len, PACK);
    srcInfo = CkNcpyBuffer(sbuf, len, sendCB);
    req.setSystemBuf(sbuf, len); // completedSend will need to free this
    // NOTE: We could set 'req.statusIreq = true' here, but then we'd
    // have to make sure systemBuf gets freed by someone in case the
    // user tries to free 'req' before the put() actually completes...
  }
  srcInfo.put(targetInfo);
}

bool ampi::processSsendMsg(AmpiMsg* msg, void* buf, MPI_Datatype type,
                           int count, MPI_Request req) noexcept {
  CkAssert(req != MPI_REQUEST_NULL);
  if (msg->isNcpyShmMsg()) {
    return processSsendNcpyShmMsg(msg, buf, type, count, req);
  }
  else {
    return processSsendNcpyMsg(msg, buf, type, count, req);
  }
}

bool ampi::processSsendNcpyShmMsg(AmpiMsg* msg, void* buf, MPI_Datatype type,
                                  int count, MPI_Request req) noexcept {
  AmpiNcpyShmBuffer srcInfo;
  msg->getNcpyShmBuffer(srcInfo);
  CkDDT_DataType* rddt = getDDT()->getType(type);
  int len = rddt->getSize(count);
  IReq& ireq = *((IReq*)(parent->ampiReqs[req]));
  ireq.length = len;

  if (srcInfo.getNode() == CkMyNode()) {
    // Sender and recver are co-located in the same process: use memcpy
    MSG_ORDER_DEBUG(
      CkPrintf("[%d] AMPI vp %d doing inline memcpy with req %d\n",
               CkMyPe(), parent->thisIndex, req);
    )
    CkDDT_DataType* sddt = srcInfo.getDDT();
    int msgCount = srcInfo.getCount();
    int msgLen = sddt->getSize(msgCount);
    char* msgData = srcInfo.getBuf();

    // Handle non-contiguous send and/or recv datatypes
    if (sddt->isContig()) {
      rddt->serialize((char*)buf, msgData, count, msgLen, UNPACK);
    }
    else if (rddt->isContig()) {
      sddt->serialize(msgData, (char*)buf, msgCount, msgLen, PACK);
    }
    else { // Both datatypes are non-contiguous
      // NOTE: Intermediate copy here could be avoided if DDT
      // could copy directly b/w two non-contiguous datatypes
      std::vector<char> sbuf(msgLen);
      sddt->serialize(msgData, sbuf.data(), msgCount, msgLen, PACK);
      rddt->serialize((char*)buf, sbuf.data(), count, msgLen, UNPACK);
    }

    // complete the sender's SsendReq, inline if possible
    int srcIdx = srcInfo.getIdx();
    MPI_Request sreqIdx = msg->getSsendReq();
#if AMPI_PE_LOCAL_IMPL
    ampi* srcPtr = thisProxy[srcIdx].ckLocal();
    if (srcPtr != nullptr && srcPtr->parent != nullptr) {
      srcPtr->completedSend(sreqIdx);
    }
    else
#endif
    {
      thisProxy[srcIdx].completedSend(sreqIdx);
    }

    // complete the recver's IReq inline
    completedRecv(req);
    return true;
  }
  else {
    // Sender is no longer in the same process: request a put() of the data
    MSG_ORDER_DEBUG(
      CkPrintf("[%d] AMPI vp %d requesting rput with req %d\n",
               CkMyPe(), parent->thisIndex, req);
    )
    CkCallback recvCB(CkIndex_ampi::completedRdmaRecv(NULL), thisProxy[thisIndex], true /*inline*/);
    recvCB.setRefnum(req);
    IReq& ireq = *((IReq*)(parent->ampiReqs[req]));
    CkNcpyBuffer targetInfo;

    if (rddt->isContig()) {
      targetInfo = CkNcpyBuffer(buf, len, recvCB);
    }
    else {
      // Allocate a contiguous intermediate buffer for the put(),
      // and deserialize from that to the user's buffer in ampi::completedRecv
      int slen = srcInfo.getLength();
      char* sbuf = new char[slen];
      ireq.setSystemBuf(sbuf, slen); // completedRecv will need to free this
      targetInfo = CkNcpyBuffer(sbuf, slen, recvCB);
    }
    thisProxy[srcInfo.getIdx()].requestPut(srcInfo.getSreqIdx(), targetInfo);
    return false;
  }
}

bool ampi::processSsendNcpyMsg(AmpiMsg* msg, void* buf, MPI_Datatype type, int count, MPI_Request req) noexcept {
  MSG_ORDER_DEBUG(
    CkPrintf("[%d] AMPI vp %d performing get() with req %d\n",
             CkMyPe(), parent->thisIndex, req);
  )
  CkNcpyBuffer srcInfo;
  msg->getNcpyBuffer(srcInfo);
  CkCallback recvCB(CkIndex_ampi::completedRdmaRecv(NULL), thisProxy[thisIndex], true /*inline*/);
  recvCB.setRefnum(req);
  CkDDT_DataType* ddt = getDDT()->getType(type);
  int len = ddt->getSize(count);
  IReq& ireq = *((IReq*)(parent->ampiReqs[req]));
  ireq.length = len;
  CkNcpyBuffer targetInfo;

  if (ddt->isContig()) {
    targetInfo = CkNcpyBuffer(buf, len, recvCB);
  }
  else {
    char* sbuf = new char[len];
    ireq.setSystemBuf(sbuf, len);
    targetInfo = CkNcpyBuffer(sbuf, len, recvCB);
  }
  targetInfo.get(srcInfo);
  return ireq.complete; // did the get() complete inline (i.e. src is in same process as target)?
}

// Returns true if the message was processed,
// false if it is a sync msg that could not yet be processed
bool ampi::processAmpiMsg(AmpiMsg *msg, void* buf, MPI_Datatype type,
                          int count, MPI_Request req) noexcept
{
  if (msg->isSsend()) { // this is a sync msg, need to get the real msg data
    return processSsendMsg(msg, buf, type, count, req);
  }

  CkDDT_DataType *ddt = getDDT()->getType(type);
  ddt->serialize((char*)buf, msg->getData(), count, msg->getLength(), UNPACK);
  return true;
}

// RDMA version of ampi::processAmpiMsg
void ampi::processRdmaMsg(const void *sbuf, int slength, void* rbuf,
                          int rcount, MPI_Datatype rtype) noexcept
{
  CkDDT_DataType *ddt = getDDT()->getType(rtype);

  ddt->serialize((char*)rbuf, (char*)sbuf, rcount, slength, UNPACK);
}

void ampi::processRednMsg(CkReductionMsg *msg, void* buf, MPI_Datatype type, int count) noexcept
{
  // The first sizeof(AmpiOpHeader) bytes in the redn msg data are reserved
  // for an AmpiOpHeader if our custom AmpiReducer type was used.
  int szhdr = (msg->getReducer() == AmpiReducer) ? sizeof(AmpiOpHeader) : 0;
  getDDT()->getType(type)->serialize((char*)buf, (char*)msg->getData()+szhdr, count, msg->getLength()-szhdr, UNPACK);
}

void ampi::processNoncommutativeRednMsg(CkReductionMsg *msg, void* buf, MPI_Datatype type, int count, MPI_User_function* func) noexcept
{
  CkReduction::tupleElement* results = NULL;
  int numReductions = 0;
  msg->toTuple(&results, &numReductions);

  // Contributions are unordered and consist of a (srcRank, data) tuple
  char *data           = (char*)(results[1].data);
  CkDDT_DataType *ddt  = getDDT()->getType(type);
  int contributionSize = ddt->getSize(count);
  int commSize         = getSize();

  // Store pointers to each contribution's data at index 'srcRank' in contributionData
  // If the max rank value fits into an unsigned short int, srcRanks are those, otherwise int's
  std::vector<void *> contributionData(commSize);
  if (commSize < std::numeric_limits<unsigned short int>::max()) {
    unsigned short int *srcRank = (unsigned short int*)(results[0].data);
    for (int i=0; i<commSize; i++) {
      contributionData[srcRank[i]] = &data[i * contributionSize];
    }
  }
  else {
    int *srcRank = (int*)(results[0].data);
    for (int i=0; i<commSize; i++) {
      contributionData[srcRank[i]] = &data[i * contributionSize];
    }
  }

  if (ddt->isContig()) {
    // Copy rank 0's contribution into buf first
    memcpy(buf, contributionData[0], contributionSize);

    // Invoke the MPI_User_function on the contributions in 'rank' order
    for (int i=1; i<commSize; i++) {
      (*func)(contributionData[i], buf, &count, &type);
    }
  }
  else {
    int contributionExtent = ddt->getExtent() * count;

    // Deserialize rank 0's contribution into buf first
    ddt->serialize((char*)contributionData[0], (char*)buf, count, contributionExtent, UNPACK);

    // Invoke the MPI_User_function on the deserialized contributions in 'rank' order
    std::vector<char> deserializedBuf(contributionExtent);
    for (int i=1; i<commSize; i++) {
      ddt->serialize((char*)contributionData[i], deserializedBuf.data(), count, contributionExtent, UNPACK);
      (*func)(deserializedBuf.data(), buf, &count, &type);
    }
  }
  delete [] results;
}

void ampi::processGatherMsg(CkReductionMsg *msg, void* buf, MPI_Datatype type, int recvCount) noexcept
{
  CkReduction::tupleElement* results = NULL;
  int numReductions = 0;
  msg->toTuple(&results, &numReductions);
  CkAssert(numReductions == 2);

  // Re-order the gather data based on the rank of the contributor
  char *data             = (char*)(results[1].data);
  CkDDT_DataType *ddt    = getDDT()->getType(type);
  int contributionSize   = ddt->getSize(recvCount);
  int contributionExtent = ddt->getExtent()*recvCount;
  int commSize           = getSize();

  // If the max rank value fits into an unsigned short int, srcRanks are those, otherwise int's
  if (commSize < std::numeric_limits<unsigned short int>::max()) {
    unsigned short int *srcRank = (unsigned short int*)(results[0].data);
    for (int i=0; i<commSize; i++) {
      ddt->serialize(&(((char*)buf)[srcRank[i] * contributionExtent]),
                     &data[i * contributionSize],
                     recvCount,
                     contributionSize,
                     UNPACK);
    }
  }
  else {
    int *srcRank = (int*)(results[0].data);
    for (int i=0; i<commSize; i++) {
      ddt->serialize(&(((char*)buf)[srcRank[i] * contributionExtent]),
                     &data[i * contributionSize],
                     recvCount,
                     contributionSize,
                     UNPACK);
    }
  }
  delete [] results;
}

void ampi::processGathervMsg(CkReductionMsg *msg, void* buf, MPI_Datatype type,
                             int* recvCounts, int* displs) noexcept
{
  CkReduction::tupleElement* results = NULL;
  int numReductions = 0;
  msg->toTuple(&results, &numReductions);
  CkAssert(numReductions == 3);

  // Re-order the gather data based on the rank of the contributor
  int *dataSize          = (int*)(results[1].data);
  char *data             = (char*)(results[2].data);
  CkDDT_DataType *ddt    = getDDT()->getType(type);
  int contributionSize   = ddt->getSize();
  int contributionExtent = ddt->getExtent();
  int commSize           = getSize();
  int currDataOffset     = 0;

  // If the max rank value fits into an unsigned short int, srcRanks are those, otherwise int's
  if (commSize < std::numeric_limits<unsigned short int>::max()) {
    unsigned short int *srcRank = (unsigned short int*)(results[0].data);
    for (int i=0; i<commSize; i++) {
      ddt->serialize(&((char*)buf)[displs[srcRank[i]] * contributionExtent],
                     &data[currDataOffset],
                     recvCounts[srcRank[i]],
                     contributionSize * recvCounts[srcRank[i]],
                     UNPACK);
      currDataOffset += dataSize[i];
    }
  }
  else {
    int *srcRank = (int*)(results[0].data);
    for (int i=0; i<commSize; i++) {
      ddt->serialize(&((char*)buf)[displs[srcRank[i]] * contributionExtent],
                     &data[currDataOffset],
                     recvCounts[srcRank[i]],
                     contributionSize * recvCounts[srcRank[i]],
                     UNPACK);
      currDataOffset += dataSize[i];
    }
  }
  delete [] results;
}

CMI_WARN_UNUSED_RESULT ampi* ampi::blockOnIReq(void* buf, int count, MPI_Datatype type, int src,
                                               int tag, MPI_Comm comm, MPI_Status* sts) noexcept
{
  MPI_Request request = postReq(parent->reqPool.newReq<IReq>(buf, count, type, src, tag, comm, getDDT(),
                                                             AMPI_REQ_BLOCKED));
  CkAssert(parent->numBlockedReqs == 0);
  parent->numBlockedReqs = 1;
  ampi* dis = blockOnRecv(); // "dis" is updated in case an ampi thread is migrated
  AmpiRequestList& reqs = dis->getReqs();
  if (sts != MPI_STATUS_IGNORE) {
    AmpiRequest& req = *reqs[request];
    sts->MPI_SOURCE = req.src;
    sts->MPI_TAG    = req.tag;
    sts->MPI_COMM   = req.comm;
    sts->MPI_LENGTH = req.getNumReceivedBytes(dis->getDDT());
    sts->MPI_CANCEL = 0;
  }
  reqs.freeNonPersReq(dis->parent, request);
  return dis;
}

static inline void clearStatus(MPI_Status *sts) noexcept {
  if (sts != MPI_STATUS_IGNORE) {
    sts->MPI_TAG    = MPI_ANY_TAG;
    sts->MPI_SOURCE = MPI_ANY_SOURCE;
    sts->MPI_COMM   = MPI_COMM_NULL;
    sts->MPI_LENGTH = 0;
    sts->MPI_ERROR  = MPI_SUCCESS;
    sts->MPI_CANCEL = 0;
  }
}

static inline void clearStatus(MPI_Status sts[], int idx) noexcept {
  if (sts != MPI_STATUSES_IGNORE) {
    clearStatus(&sts[idx]);
  }
}

// Handle a MPI_PROC_NULL src argument according to Section 3.11 of the MPI-3.1 standard.
// Relevant for MPI_Recv, MPI_Probe, MPI_Iprobe, MPI_Improbe
static inline bool handle_MPI_PROC_NULL(int src, MPI_Status* sts) noexcept
{
  if (src == MPI_PROC_NULL) {
    if (sts != MPI_STATUS_IGNORE) {
      sts->MPI_SOURCE = MPI_PROC_NULL;
      sts->MPI_TAG = MPI_ANY_TAG;
      sts->MPI_LENGTH = 0;
    }
    return true;
  }
  return false;
}

int ampi::static_recv(ampi *dis, int t, int s, void* buf, int count, MPI_Datatype type, MPI_Comm comm, MPI_Status *sts) noexcept
{
  MSG_ORDER_DEBUG(
    CkPrintf("AMPI vp %d blocking recv: tag=%d, src=%d, comm=%d\n",dis->thisIndex,t,s,comm);
  )
  MPI_Request req;
  dis->irecv(buf, count, type, s, t, comm, &req);
  ampiParent* unused = dis->parent->wait(&req, sts);
  return MPI_SUCCESS;
}

void ampi::static_probe(ampi *dis, int t, int s, MPI_Comm comm, MPI_Status *sts) noexcept
{
  if (handle_MPI_PROC_NULL(s, sts)) 
    return;


  AmpiMsg *msg = NULL;
  while(1) {
    MPI_Status tmpStatus;
    msg = dis->unexpectedMsgs.probe(t, s, (sts == MPI_STATUS_IGNORE) ? (int*)&tmpStatus : (int*)sts);
    if (msg) break;
    // "dis" is updated in case an ampi thread is migrated while waiting for a message
    dis = dis->blockOnRecv();
  }

  if (sts != MPI_STATUS_IGNORE) {
    sts->MPI_SOURCE = msg->getSrcRank();
    sts->MPI_TAG    = msg->getTag();
    sts->MPI_COMM   = comm;
    sts->MPI_LENGTH = msg->getLength();
    sts->MPI_CANCEL = 0;
  }

}

void ampi::static_mprobe(ampi *dis, int t, int s, MPI_Comm comm, MPI_Status *sts, MPI_Message *message) noexcept
{
  if (handle_MPI_PROC_NULL(s, sts)) {
    *message = MPI_MESSAGE_NO_PROC;
    return;
  }


  AmpiMsg *msg = NULL;
  while(1) {
    MPI_Status tmpStatus;
    // We call get() rather than probe() here because we want to remove this msg
    // from ampi::unexpectedMsgs and then insert it into ampiParent::matchedMsgs
    msg = dis->unexpectedMsgs.get(t, s, (sts == MPI_STATUS_IGNORE) ? (int*)&tmpStatus : (int*)sts);
    if (msg)
      break;
    // "dis" is updated in case an ampi thread is migrated while waiting for a message
    dis = dis->blockOnRecv();
  }

  msg->setComm(comm);
  *message = dis->parent->putMatchedMsg(msg);

  if (sts != MPI_STATUS_IGNORE) {
    sts->MPI_SOURCE = msg->getSrcRank();
    sts->MPI_TAG    = msg->getTag();
    sts->MPI_COMM   = msg->getComm();
    sts->MPI_LENGTH = msg->getLength();
    sts->MPI_CANCEL = 0;
  }

}

// Returns whether there is a message that can be received (return 1) or not (return 0) 
int ampi::iprobe(int t, int s, MPI_Comm comm, MPI_Status *sts) noexcept
{
  if (handle_MPI_PROC_NULL(s, sts))
    return 1;

  MPI_Status tmpStatus;
  AmpiMsg* msg = unexpectedMsgs.probe(t, s, (sts == MPI_STATUS_IGNORE) ? (int*)&tmpStatus : (int*)sts);
  if (msg) {
    msg->setComm(comm);
    if (sts != MPI_STATUS_IGNORE) {
      sts->MPI_SOURCE = msg->getSrcRank();
      sts->MPI_TAG    = msg->getTag();
      sts->MPI_COMM   = msg->getComm();
      sts->MPI_LENGTH = msg->getLength();
      sts->MPI_CANCEL = 0;
    }
    return 1;
  }
  ampi* unused = yield();
  return 0;
}

// Returns whether there is a message that can be received (return 1) or not (return 0) 
int ampi::improbe(int tag, int source, MPI_Comm comm, MPI_Status *sts,
                  MPI_Message *message) noexcept
{
  if (handle_MPI_PROC_NULL(source, sts)) {
    *message = MPI_MESSAGE_NO_PROC;
    return 1;
  }

  MPI_Status tmpStatus;
  // We call get() rather than probe() here because we want to remove this msg
  // from ampi::unexpectedMsgs and then insert it into ampiParent::matchedMsgs
  AmpiMsg* msg = unexpectedMsgs.get(tag, source, (sts == MPI_STATUS_IGNORE) ? (int*)&tmpStatus : (int*)sts);
  if (msg) {
    msg->setComm(comm);
    *message = parent->putMatchedMsg(msg);
    if (sts != MPI_STATUS_IGNORE) {
      sts->MPI_SOURCE = msg->getSrcRank();
      sts->MPI_TAG    = msg->getTag();
      sts->MPI_COMM   = comm;
      sts->MPI_LENGTH = msg->getLength();
      sts->MPI_CANCEL = 0;
    }
    return 1;
  }

  ampi* unused = yield();
  return 0;
}

void ampi::bcast(int root, void* buf, int count, MPI_Datatype type, MPI_Comm destcomm) noexcept
{
  MPI_Request req;

  if (root==getRank()) {
    irecvBcast(buf, count, type, root, destcomm, &req);
    thisProxy.bcastResult(makeBcastMsg(buf, count, type, root, destcomm));
  }
  else { // Non-root ranks need to increment the outgoing sequence number for collectives
    oorder.incCollSeqOutgoing();
    irecvBcast(buf, count, type, root, destcomm, &req);
  }

  MPI_Wait(&req, MPI_STATUS_IGNORE);
}

int ampi::intercomm_bcast(int root, void* buf, int count, MPI_Datatype type, MPI_Comm intercomm) noexcept
{
  if (root==MPI_ROOT) {
    remoteProxy.bcastResult(makeBcastMsg(buf, count, type, getRank(), intercomm));
  }
  else { // Non-root ranks need to increment the outgoing sequence number for collectives
    oorder.incCollSeqOutgoing();
  }

  if (root!=MPI_PROC_NULL && root!=MPI_ROOT) {
    // remote group ranks
    MPI_Request req;
    irecvBcast(buf, count, type, root, intercomm, &req);
    MPI_Wait(&req, MPI_STATUS_IGNORE);
  }
  return MPI_SUCCESS;
}

void ampi::ibcast(int root, void* buf, int count, MPI_Datatype type, MPI_Comm destcomm, MPI_Request* request) noexcept
{
  if (root==getRank()) {
    thisProxy.bcastResult(makeBcastMsg(buf, count, type, getRank(), destcomm));
  }
  else { // Non-root ranks need to increment the outgoing sequence number for collectives
    oorder.incCollSeqOutgoing();
  }

  // call irecv to post an IReq and check for any pending messages
  irecvBcast(buf, count, type, root, destcomm, request);
}

int ampi::intercomm_ibcast(int root, void* buf, int count, MPI_Datatype type, MPI_Comm intercomm, MPI_Request *request) noexcept
{
  if (root==MPI_ROOT) {
    remoteProxy.bcastResult(makeBcastMsg(buf, count, type, getRank(), intercomm));
  }
  else { // Non-root ranks need to increment the outgoing sequence number for collectives
    oorder.incCollSeqOutgoing();
  }

  if (root!=MPI_PROC_NULL && root!=MPI_ROOT) {
    // call irecv to post IReq and process pending messages
    irecvBcast(buf, count, type, root, intercomm, request);
  }
  return MPI_SUCCESS;
}

void ampi::bcastraw(void* buf, int len, CkArrayID aid) noexcept
{
  AmpiMsg *msg = new (len, 0) AmpiMsg(0, MPI_REQUEST_NULL, MPI_BCAST_TAG, 0, len);
  memcpy(msg->getData(), buf, len);
  CProxy_ampi pa(aid);
  pa.generic(msg);
}

int ampi::intercomm_scatter(int root, const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                            void *recvbuf, int recvcount, MPI_Datatype recvtype, MPI_Comm intercomm) noexcept
{
  if (root == MPI_ROOT) {
    int remote_size = getRemoteIndices().size();

    CkDDT_DataType* dttype = getDDT()->getType(sendtype) ;
    int itemsize = dttype->getSize(sendcount) ;
    for(int i = 0; i < remote_size; i++) {
        send(MPI_SCATTER_TAG, getRank(), ((char*)sendbuf)+(itemsize*i),
             sendcount, sendtype, i, intercomm);
    }
  }

  if (root!=MPI_PROC_NULL && root!=MPI_ROOT) { //remote group ranks
    if(-1==recv(MPI_SCATTER_TAG, root, recvbuf, recvcount, recvtype, intercomm))
      CkAbort("AMPI> Error in intercomm MPI_Scatter recv");
  }

  return MPI_SUCCESS;
}

int ampi::intercomm_iscatter(int root, const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                             void *recvbuf, int recvcount, MPI_Datatype recvtype,
                             MPI_Comm intercomm, MPI_Request *request) noexcept
{
  if (root == MPI_ROOT) {
    int remote_size = getRemoteIndices().size();

    CkDDT_DataType* dttype = getDDT()->getType(sendtype) ;
    int itemsize = dttype->getSize(sendcount) ;
    // use an ATAReq to non-block the caller and get a request ptr
    ATAReq *newreq = new ATAReq(remote_size);
    for(int i = 0; i < remote_size; i++) {
      newreq->reqs[i] = send(MPI_SCATTER_TAG, getRank(), ((char*)sendbuf)+(itemsize*i),
                             sendcount, sendtype, i, intercomm, I_SEND);
    }
    *request = postReq(newreq);
  }

  if (root!=MPI_PROC_NULL && root!=MPI_ROOT) { //remote group ranks
    // call irecv to post an IReq and process any pending messages
    irecv(recvbuf,recvcount,recvtype,root,MPI_SCATTER_TAG,intercomm,request);
  }

  return MPI_SUCCESS;
}

int ampi::intercomm_scatterv(int root, const void* sendbuf, const int* sendcounts, const int* displs,
                             MPI_Datatype sendtype, void* recvbuf, int recvcount,
                             MPI_Datatype recvtype, MPI_Comm intercomm) noexcept
{
  if (root == MPI_ROOT) {
    int remote_size = getRemoteIndices().size();

    CkDDT_DataType* dttype = getDDT()->getType(sendtype);
    int itemsize = dttype->getSize();
    for (int i = 0; i < remote_size; i++) {
        send(MPI_SCATTER_TAG, getRank(), ((char*)sendbuf)+(itemsize*displs[i]),
             sendcounts[i], sendtype, i, intercomm);
    }
  }

  if (root != MPI_PROC_NULL && root != MPI_ROOT) { // remote group ranks
    if (-1 == recv(MPI_SCATTER_TAG, root, recvbuf, recvcount, recvtype, intercomm))
      CkAbort("AMPI> Error in intercomm MPI_Scatterv recv");
  }

  return MPI_SUCCESS;
}

int ampi::intercomm_iscatterv(int root, const void* sendbuf, const int* sendcounts, const int* displs,
                              MPI_Datatype sendtype, void* recvbuf, int recvcount,
                              MPI_Datatype recvtype, MPI_Comm intercomm, MPI_Request* request) noexcept
{
  if (root == MPI_ROOT) {
    int remote_size = getRemoteIndices().size();

    CkDDT_DataType* dttype = getDDT()->getType(sendtype);
    int itemsize = dttype->getSize();
    // use an ATAReq to non-block the caller and get a request ptr
    ATAReq *newreq = new ATAReq(remote_size);
    for (int i = 0; i < remote_size; i++) {
      newreq->reqs[i] = send(MPI_SCATTER_TAG, getRank(), ((char*)sendbuf)+(itemsize*displs[i]),
                             sendcounts[i], sendtype, i, intercomm, I_SEND);
    }
    *request = postReq(newreq);
  }

  if (root != MPI_PROC_NULL && root != MPI_ROOT) { // remote group ranks
    // call irecv to post an IReq and process any pending messages
    irecv(recvbuf, recvcount, recvtype, root, MPI_SCATTER_TAG, intercomm, request);
  }

  return MPI_SUCCESS;
}

int MPI_comm_null_copy_fn(MPI_Comm comm, int keyval, void *extra_state,
                          void *attr_in, void *attr_out, int *flag){
  (*flag) = 0;
  return (MPI_SUCCESS);
}

int MPI_comm_dup_fn(MPI_Comm comm, int keyval, void *extra_state,
                    void *attr_in, void *attr_out, int *flag){
  (*(void **)attr_out) = attr_in;
  (*flag) = 1;
  return (MPI_SUCCESS);
}

int MPI_comm_null_delete_fn(MPI_Comm comm, int keyval, void *attr, void *extra_state){
  return (MPI_SUCCESS);
}

int MPI_type_null_copy_fn(MPI_Datatype type, int keyval, void *extra_state,
                          void *attr_in, void *attr_out, int *flag){
  (*flag) = 0;
  return (MPI_SUCCESS);
}

int MPI_type_dup_fn(MPI_Datatype type, int keyval, void *extra_state,
                    void *attr_in, void *attr_out, int *flag){
  (*(void **)attr_out) = attr_in;
  (*flag) = 1;
  return (MPI_SUCCESS);
}

int MPI_type_null_delete_fn(MPI_Datatype type, int keyval, void *attr, void *extra_state){
  return (MPI_SUCCESS);
}

void AmpiSeqQ::pup(PUP::er &p) noexcept {
  p|out;
  p|elements;
}

void AmpiSeqQ::putOutOfOrder(int seqIdx, AmpiMsg *msg) noexcept
{
  AmpiOtherElement &el=elements[seqIdx];
#if CMK_ERROR_CHECKING
  if (msg->getSeqIdx() != COLL_SEQ_IDX && msg->getSeq() < el.getSeqIncoming())
    CkAbort("AMPI logic error: received late out-of-order message!\n");
#endif
  if (seqIdx == COLL_SEQ_IDX) CkReferenceMsg(msg); // bcast msg is [nokeep]
  out.enq(msg);
  el.incNumOutOfOrder(); // We have another message in the out-of-order queue
}

AmpiMsg *AmpiSeqQ::getOutOfOrder(int seqIdx) noexcept
{
  AmpiOtherElement &el=elements[seqIdx];
  if (el.getNumOutOfOrder() == 0) return nullptr; // No more out-of-order left.
  // Walk through our out-of-order queue, searching for our next message:
  for (int i=0;i<out.length();i++) {
    AmpiMsg *msg=out.deq();
    if (msg->getSeqIdx()==seqIdx && msg->getSeq()==el.getSeqIncoming()) {
      el.incSeqIncoming();
      el.decNumOutOfOrder(); // We have one less message out-of-order
      return msg;
    }
    else
      out.enq(msg);
  }
  // We walked the whole queue-- ours is not there.
  return nullptr;
}

void AmpiRequest::print() const noexcept {
  CkPrintf("In AmpiRequest: buf=%p, count=%d, type=%d, src=%d, tag=%d, comm=%d, reqIdx=%d, complete=%d, blocked=%d\n",
           buf, count, type, src, tag, comm, reqIdx, (int)complete, (int)blocked);
}

void IReq::print() const noexcept {
  AmpiRequest::print();
  CkPrintf("In IReq: this=%p, length=%d, cancelled=%d, persistent=%d\n", this, length, (int)cancelled, (int)persistent);
}

void RednReq::print() const noexcept {
  AmpiRequest::print();
  CkPrintf("In RednReq: this=%p, op=%d\n", this, op);
}

void GatherReq::print() const noexcept {
  AmpiRequest::print();
  CkPrintf("In GatherReq: this=%p\n", this);
}

void GathervReq::print() const noexcept {
  AmpiRequest::print();
  CkPrintf("In GathervReq: this=%p\n", this);
}

void ATAReq::print() const noexcept { //not complete for reqs
  AmpiRequest::print();
  CkPrintf("In ATAReq: num_reqs=%zu\n", reqs.size());
}

void GReq::print() const noexcept {
  AmpiRequest::print();
  CkPrintf("In GReq: this=%p\n", this);
}

void SendReq::print() const noexcept {
  AmpiRequest::print();
  CkPrintf("In SendReq: this=%p, persistent=%d\n", this, (int)persistent);
}

void SsendReq::print() const noexcept {
  AmpiRequest::print();
  CkPrintf("In SsendReq: this=%p, persistent=%d\n", this, (int)persistent);
}

void AmpiRequestList::pup(PUP::er &p, AmpiRequestPool* pool) noexcept {
  if (p.isUnpacking()) {
    CkAssert(pool);
    reqPool = pool;
  }
  if(!CmiMemoryIs(CMI_MEMORY_IS_ISOMALLOC)){
    return;
  }

  p|startIdx;
  int size;
  if(!p.isUnpacking()){
    size = reqs.size();
  }
  p|size;
  if(p.isUnpacking()){
    reqs.resize(size);
  }
  // Must preserve indices in 'block' so that MPI_Request's remain the same, so keep NULL entries:
  for(int i=0;i<size;i++){
    AmpiReqType reqType;
    if(!p.isUnpacking()){
      if(reqs[i] == NULL){
        reqType = AMPI_INVALID_REQ;
      }else{
        reqType = reqs[i]->getType();
      }
    }
    p|reqType;
    if(reqType != AMPI_INVALID_REQ){
      if(p.isUnpacking()){
        switch(reqType){
          case AMPI_I_REQ:
            reqs[i] = reqPool->newReq<IReq>();
            break;
          case AMPI_REDN_REQ:
            reqs[i] = new RednReq;
            break;
          case AMPI_GATHER_REQ:
            reqs[i] = new GatherReq;
            break;
          case AMPI_GATHERV_REQ:
            reqs[i] = new GathervReq;
            break;
          case AMPI_SEND_REQ:
            reqs[i] = reqPool->newReq<SendReq>();
            break;
          case AMPI_SSEND_REQ:
            reqs[i] = reqPool->newReq<SsendReq>();
            break;
          case AMPI_ATA_REQ:
            reqs[i] = new ATAReq;
            break;
          case AMPI_G_REQ:
            reqs[i] = new GReq;
            break;
#if CMK_CUDA
          case AMPI_GPU_REQ:
            CkAbort("AMPI> error trying to PUP a non-migratable GPU request!");
            break;
#endif
          case AMPI_INVALID_REQ:
            CkAbort("AMPI> error trying to PUP an invalid request!");
            break;
        }
      }
      reqs[i]->pup(p);
    }else{
      reqs[i] = NULL;
    }
  }
  if(p.isDeleting()){
    reqs.clear();
  }
}

//------------------ External Interface -----------------
CMI_WARN_UNUSED_RESULT ampiParent *getAmpiParent() noexcept {
  ampiParent *p = CtvAccess(ampiPtr);
#if CMK_ERROR_CHECKING
  if (p==NULL) CkAbort("Cannot call MPI routines before AMPI is initialized.\n");
#endif
  return p;
}

CMI_WARN_UNUSED_RESULT ampi *getAmpiInstance(MPI_Comm comm) noexcept {
  ampi *ptr=getAmpiParent()->comm2ampi(comm);
#if CMK_ERROR_CHECKING
  if (ptr==NULL) CkAbort("AMPI's getAmpiInstance> null pointer\n");
#endif
  return ptr;
}

bool isAmpiThread() noexcept {
  return (CtvAccess(ampiPtr) != NULL);
}

inline static AmpiRequestList &getReqs() noexcept {
  return getAmpiParent()->ampiReqs;
}

inline void checkComm(MPI_Comm comm) noexcept {
#if AMPI_ERROR_CHECKING
  getAmpiParent()->checkComm(comm);
#endif
}

inline void checkRequest(MPI_Request req) noexcept {
#if AMPI_ERROR_CHECKING
  getReqs().checkRequest(req);
#endif
}

inline void checkRequests(int n, MPI_Request* reqs) noexcept {
#if AMPI_ERROR_CHECKING
  AmpiRequestList& reqlist = getReqs();
  for(int i=0;i<n;i++)
    reqlist.checkRequest(reqs[i]);
#endif
}

int testRequest(ampiParent* pptr, MPI_Request *reqIdx, int *flag, MPI_Status *sts) noexcept {
  if(*reqIdx==MPI_REQUEST_NULL){
    *flag = 1;
    clearStatus(sts);
    return MPI_SUCCESS;
  }
  checkRequest(*reqIdx);
  AmpiRequestList& reqList = pptr->getReqs();
  AmpiRequest& req = *reqList[*reqIdx];
  if(1 == (*flag = req.test())){
    pptr = req.wait(pptr, sts);
    reqList.freeNonPersReq(pptr, *reqIdx);
  }
  return MPI_SUCCESS;
}

int testRequestNoFree(ampiParent* pptr, MPI_Request *reqIdx, int *flag, MPI_Status *sts) noexcept {
  if(*reqIdx==MPI_REQUEST_NULL){
    *flag = 1;
    clearStatus(sts);
    return MPI_SUCCESS;
  }
  checkRequest(*reqIdx);
  AmpiRequestList& reqList = pptr->getReqs();
  AmpiRequest& req = *reqList[*reqIdx];
  *flag = req.test();
  if(*flag)
    pptr = req.wait(pptr, sts);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Is_thread_main, int *flag)
{
  AMPI_API_INIT("AMPI_Is_thread_main", flag);
  if (isAmpiThread()) {
    *flag = 1;
  } else {
    *flag = 0;
  }
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Query_thread, int *provided)
{
  AMPI_API("AMPI_Query_thread", provided);
  *provided = CkpvAccess(ampiThreadLevel);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Init_thread, int *p_argc, char*** p_argv, int required, int *provided)
{
  AMPI_API_INIT("AMPI_Init_thread", p_argc, p_argv, required, provided);

#if AMPI_ERROR_CHECKING
  if (required < MPI_THREAD_SINGLE || required > MPI_THREAD_MULTIPLE) {
    return ampiErrhandler("AMPI_Init_thread", MPI_ERR_ARG);
  }
#endif

  if (required == MPI_THREAD_SINGLE) {
    CkpvAccess(ampiThreadLevel) = MPI_THREAD_SINGLE;
  }
  else {
    CkpvAccess(ampiThreadLevel) = MPI_THREAD_FUNNELED;
  }
  // AMPI does not support MPI_THREAD_SERIALIZED or MPI_THREAD_MULTIPLE

  *provided = CkpvAccess(ampiThreadLevel);
  return MPI_Init(p_argc, p_argv);
}

AMPI_API_IMPL(int, MPI_Init, int *p_argc, char*** p_argv)
{
  AMPI_API_INIT("AMPI_Init", p_argc, p_argv);
  char **argv;
  if (p_argv) argv=*p_argv;
  else argv=CkGetArgv();
  ampiInit(argv);
  if (p_argc) *p_argc=CmiGetArgc(argv);

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Initialized, int *isInit)
{
  AMPI_API_INIT("AMPI_Initialized", isInit);
  *isInit=CtvAccess(ampiInitDone);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Finalized, int *isFinalized)
{
  AMPI_API_INIT("AMPI_Finalized", isFinalized);
  *isFinalized=(CtvAccess(ampiFinalized)) ? 1 : 0;
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Comm_rank, MPI_Comm comm, int *rank)
{
  AMPI_API("AMPI_Comm_rank", comm, rank);

#if AMPI_ERROR_CHECKING
  int ret = checkCommunicator("AMPI_Comm_rank", comm);
  if(ret != MPI_SUCCESS)
    return ret;
#endif

#if AMPIMSGLOG
  ampiParent* pptr = getAmpiParent();
  if(msgLogRead){
    PUParray(*(pptr->fromPUPer), (char*)rank, sizeof(int));
    return MPI_SUCCESS;
  }
#endif

  *rank = getAmpiInstance(comm)->getRank();

#if AMPIMSGLOG
  if(msgLogWrite && record_msglog(pptr->thisIndex)){
    PUParray(*(pptr->toPUPer), (char*)rank, sizeof(int));
  }
#endif
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Comm_size, MPI_Comm comm, int *size)
{
  AMPI_API("AMPI_Comm_size", comm, size);

#if AMPI_ERROR_CHECKING
  int ret = checkCommunicator("AMPI_Comm_size", comm);
  if(ret != MPI_SUCCESS)
    return ret;
#endif

#if AMPIMSGLOG
  ampiParent* pptr = getAmpiParent();
  if(msgLogRead){
    PUParray(*(pptr->fromPUPer), (char*)size, sizeof(int));
    return MPI_SUCCESS;
  }
#endif

  *size = getAmpiInstance(comm)->getSize();

#if AMPIMSGLOG
  if(msgLogWrite && record_msglog(pptr->thisIndex)){
    PUParray(*(pptr->toPUPer), (char*)size, sizeof(int));
  }
#endif

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Comm_compare, MPI_Comm comm1, MPI_Comm comm2, int *result)
{
  AMPI_API("AMPI_Comm_compare", comm1, comm2, result);

#if AMPI_ERROR_CHECKING
  int ret;
  ret = checkCommunicator("AMPI_Comm_compare", comm1);
  if(ret != MPI_SUCCESS)
    return ret;
  ret = checkCommunicator("AMPI_Comm_compare", comm2);
  if(ret != MPI_SUCCESS)
    return ret;
#endif

  if(comm1==comm2) *result=MPI_IDENT;
  else{
    int congruent=1;
    std::vector<int> ind1, ind2;
    ind1 = getAmpiInstance(comm1)->getIndices();
    ind2 = getAmpiInstance(comm2)->getIndices();
    if(ind1.size()==ind2.size()){
      for(int i=0;i<ind1.size();i++){
        int equal=0;
        for(int j=0;j<ind2.size();j++){
          if(ind1[i]==ind2[j]){
            equal=1;
            if(i!=j) congruent=0;
          }
        }
        if(!equal){
          *result=MPI_UNEQUAL;
          return MPI_SUCCESS;
        }
      }
    }
    else{
      *result=MPI_UNEQUAL;
      return MPI_SUCCESS;
    }
    if(congruent==1) *result=MPI_CONGRUENT;
    else *result=MPI_SIMILAR;
  }
  return MPI_SUCCESS;
}

static bool atexit_called = false;

CLINKAGE
void ampiMarkAtexit()
{
  atexit_called = true;
}

CLINKAGE
void AMPI_Exit(int exitCode)
{
  // If we are not actually running AMPI code (e.g., by compiling a serial
  // application with ampicc), exit cleanly when the application calls exit().
  AMPI_API_INIT("AMPI_Exit", exitCode);
  CkpvAccess(msgPool).clear();

  if (!atexit_called)
    TCHARM_Done(exitCode);
}

FLINKAGE
void FTN_NAME(MPI_EXIT,mpi_exit)(int *exitCode)
{
  AMPI_Exit(*exitCode);
}

AMPI_API_IMPL(int, MPI_Finalize, void)
{
  { // This brace is necessary here to make sure the object created on the stack
    // by the AMPI_API call gets destroyed before the call to AMPI_Exit(), since
    // AMPI_Exit() never returns.
  AMPI_API("AMPI_Finalize", "");

  ampiParent* parent = getAmpiParent();
  int ret;
  if ((ret = parent->freeUserAttributes(MPI_COMM_WORLD, parent->getAttributes(MPI_COMM_WORLD))) != MPI_SUCCESS)
    return ret;
  if ((ret = parent->freeUserAttributes(MPI_COMM_SELF, parent->getAttributes(MPI_COMM_SELF))) != MPI_SUCCESS)
    return ret;

#if AMPI_PRINT_IDLE
  CkPrintf("[%d] Idle time %fs.\n", CkMyPe(), totalidle);
#endif
  CtvAccess(ampiFinalized)=true;

#if AMPI_PRINT_MSG_SIZES
  getAmpiParent()->printMsgSizes();
#endif

  }

  AMPI_Exit(0); // Never returns
  return MPI_SUCCESS;
}

MPI_Request ampi::postReq(AmpiRequest* newreq) noexcept
{
  // All valid requests must be inserted into the AmpiRequestList
  MPI_Request request = getReqs().insert(newreq);
  // Completed requests should not be inserted into the postedReqs queue.
  // All types of send requests are matched by their request number,
  // not by (tag, src, comm), so they should not be inserted either.
  if (newreq->isUnmatched()) {
    postedReqs.put(newreq);
  }
  return request;
}

AMPI_API_IMPL(int, MPI_Send, const void *buf, int count, MPI_Datatype type,
                             int dest, int tag, MPI_Comm comm)
{
  AMPI_API("AMPI_Send", buf, count, type, dest, tag, comm);

  handle_MPI_BOTTOM((void*&)buf, type);

#if AMPI_ERROR_CHECKING
  int ret;
  ret = errorCheck("AMPI_Send", comm, 1, count, 1, type, 1, tag, 1, dest, 1, buf, 1);
  if(ret != MPI_SUCCESS)
    return ret;
#endif

#if AMPIMSGLOG
  if(msgLogRead){
    return MPI_SUCCESS;
  }
#endif

  ampi *ptr = getAmpiInstance(comm);
  ptr->send(tag, ptr->getRank(), buf, count, type, dest, comm);

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Bsend, const void *buf, int count, MPI_Datatype datatype,
                              int dest, int tag, MPI_Comm comm)
{
  AMPI_API("AMPI_Bsend", buf, count, datatype, dest, tag, comm);
  // FIXME: we don't actually use the buffer set in MPI_Buffer_attach
  //        for buffering of messages sent via MPI_Bsend
  return MPI_Send(buf, count, datatype, dest, tag, comm);
}

AMPI_API_IMPL(int, MPI_Buffer_attach, void *buffer, int size)
{
  AMPI_API("AMPI_Buffer_attach", buffer, size);
#if AMPI_ERROR_CHECKING
  if (size < 0) {
    return ampiErrhandler("AMPI_Buffer_attach", MPI_ERR_ARG);
  }
#endif
  // NOTE: we don't really use this buffer for Bsend's,
  //       we only keep track of it so that it can be
  //       returned by MPI_Buffer_detach.
  getAmpiParent()->attachBuffer(buffer, size);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Buffer_detach, void *buffer, int *size)
{
  AMPI_API("AMPI_Buffer_detach", buffer, size);
  getAmpiParent()->detachBuffer(buffer, size);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Rsend, const void *buf, int count, MPI_Datatype datatype,
                              int dest, int tag, MPI_Comm comm)
{
  /* FIXME: MPI_Rsend can be posted only after recv */
  AMPI_API("AMPI_Rsend", buf, count, datatype, dest, tag, comm);
  return MPI_Send(buf, count, datatype, dest, tag, comm);
}

AMPI_API_IMPL(int, MPI_Ssend, const void *buf, int count, MPI_Datatype type,
                              int dest, int tag, MPI_Comm comm)
{
  AMPI_API("AMPI_Ssend", buf, count, type, dest, tag, comm);

  handle_MPI_BOTTOM((void*&)buf, type);

#if AMPI_ERROR_CHECKING
  int ret = errorCheck("AMPI_Ssend", comm, 1, count, 1, type, 1, tag, 1, dest, 1, buf, 1);
  if(ret != MPI_SUCCESS)
    return ret;
#endif

#if AMPIMSGLOG
  if(msgLogRead){
    return MPI_SUCCESS;
  }
#endif

  ampi *ptr = getAmpiInstance(comm);
  ptr->send(tag, ptr->getRank(), buf, count, type, dest, comm, BLOCKING_SSEND);

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Issend, const void *buf, int count, MPI_Datatype type, int dest,
                               int tag, MPI_Comm comm, MPI_Request *request)
{
  AMPI_API("AMPI_Issend", buf, count, type, dest, tag, comm, request);

  handle_MPI_BOTTOM((void*&)buf, type);

#if AMPI_ERROR_CHECKING
  int ret = errorCheck("AMPI_Issend", comm, 1, count, 1, type, 1, tag, 1, dest, 1, buf, 1);
  if(ret != MPI_SUCCESS){
    *request = MPI_REQUEST_NULL;
    return ret;
  }
#endif

#if AMPIMSGLOG
  ampiParent* pptr = getAmpiParent();
  if(msgLogRead){
    PUParray(*(pptr->fromPUPer), (char *)request, sizeof(MPI_Request));
    return MPI_SUCCESS;
  }
#endif

  ampi* ptr = getAmpiInstance(comm);
  *request = ptr->send(tag, ptr->getRank(), buf, count, type, dest, comm, I_SSEND);

#if AMPIMSGLOG
  if(msgLogWrite && record_msglog(pptr->thisIndex)){
    PUParray(*(pptr->toPUPer), (char *)request, sizeof(MPI_Request));
  }
#endif

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Recv, void *buf, int count, MPI_Datatype type, int src, int tag,
                             MPI_Comm comm, MPI_Status *status)
{
  AMPI_API("AMPI_Recv", buf, count, type, src, tag, comm, status);

  handle_MPI_BOTTOM(buf, type);

#if AMPI_ERROR_CHECKING
  int ret = errorCheck("AMPI_Recv", comm, 1, count, 1, type, 1, tag, 1, src, 1, buf, 1);
  if(ret != MPI_SUCCESS)
    return ret;
#endif

#if AMPIMSGLOG
  ampiParent* pptr = getAmpiParent();
  if(msgLogRead){
    (*(pptr->fromPUPer))|(pptr->pupBytes);
    PUParray(*(pptr->fromPUPer), (char *)buf, (pptr->pupBytes));
    PUParray(*(pptr->fromPUPer), (char *)status, sizeof(MPI_Status));
    return MPI_SUCCESS;
  }
#endif

  ampi *ptr = getAmpiInstance(comm);
  if(-1==ptr->recv(tag,src,buf,count,type,comm,status)) CkAbort("AMPI> Error in MPI_Recv");

#if AMPIMSGLOG
  if(msgLogWrite && record_msglog(pptr->thisIndex)){
    (pptr->pupBytes) = getDDT()->getSize(type) * count;
    (*(pptr->toPUPer))|(pptr->pupBytes);
    PUParray(*(pptr->toPUPer), (char *)buf, (pptr->pupBytes));
    PUParray(*(pptr->toPUPer), (char *)status, sizeof(MPI_Status));
  }
#endif

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Probe, int src, int tag, MPI_Comm comm, MPI_Status *status)
{
  AMPI_API("AMPI_Probe", src, tag, comm, status);

#if AMPI_ERROR_CHECKING
  int ret = errorCheck("AMPI_Probe", comm, 1, 0, 0, 0, 0, tag, 1, src, 1, 0, 0);
  if(ret != MPI_SUCCESS)
    return ret;
#endif

  ampi *ptr = getAmpiInstance(comm);
  ptr->probe(tag, src, comm, status);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Iprobe, int src, int tag, MPI_Comm comm, int *flag, MPI_Status *status)
{
  AMPI_API("AMPI_Iprobe", src, tag, comm, flag, status);

#if AMPI_ERROR_CHECKING
  int ret = errorCheck("AMPI_Iprobe", comm, 1, 0, 0, 0, 0, tag, 1, src, 1, 0, 0);
  if(ret != MPI_SUCCESS)
    return ret;
#endif

  ampi *ptr = getAmpiInstance(comm);
  *flag = ptr->iprobe(tag, src, comm, status);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Improbe, int source, int tag, MPI_Comm comm, int *flag,
                                MPI_Message *message, MPI_Status *status)
{
  AMPI_API("AMPI_Improbe", source, tag, comm, flag, message, status);

#if AMPI_ERROR_CHECKING
  int ret = errorCheck("AMPI_Improbe", comm, 1, 0, 0, 0, 0, tag, 1, source, 1, 0, 0);
  if(ret != MPI_SUCCESS)
    return ret;
#endif

  ampi *ptr = getAmpiInstance(comm);
  *flag = ptr->improbe(tag, source, comm, status, message);

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Imrecv, void* buf, int count, MPI_Datatype datatype, MPI_Message *message,
                               MPI_Request *request)
{
  AMPI_API("AMPI_Imrecv", buf, count, datatype, message, request);

#if AMPI_ERROR_CHECKING
  if (*message == MPI_MESSAGE_NULL) {
    return ampiErrhandler("AMPI_Imrecv", MPI_ERR_REQUEST);
  }
#endif

  if (*message == MPI_MESSAGE_NO_PROC) {
    *message = MPI_MESSAGE_NULL;
    IReq *newreq = getAmpiParent()->reqPool.newReq<IReq>(buf, count, datatype, MPI_PROC_NULL, MPI_ANY_TAG,
                                                         MPI_COMM_NULL, getDDT(), AMPI_REQ_COMPLETED);
    *request = getReqs().insert(newreq);
    return MPI_SUCCESS;
  }

  handle_MPI_BOTTOM(buf, datatype);

#if AMPI_ERROR_CHECKING
  int ret = errorCheck("AMPI_Imrecv", 0, 0, count, 1, datatype, 1, 0, 0, 0, 0, buf, 1);
  if(ret != MPI_SUCCESS){
    *request = MPI_REQUEST_NULL;
    return ret;
  }
#endif

  ampiParent* parent = getAmpiParent();
  AmpiMsg* msg = parent->getMatchedMsg(*message);
  CkAssert(msg);
  MPI_Comm comm = msg->getComm();
  int tag = msg->getTag();
  int src = msg->getSrcRank();

  ampi *ptr = getAmpiInstance(comm);
  AmpiRequestList& reqs = getReqs();
  IReq *newreq = parent->reqPool.newReq<IReq>(buf, count, datatype, src, tag, comm, parent->getDDT());
  *request = reqs.insert(newreq);

  newreq->receive(ptr, msg);
  *message = MPI_MESSAGE_NULL;

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Mprobe, int source, int tag, MPI_Comm comm, MPI_Message *message,
                               MPI_Status *status)
{
  AMPI_API("AMPI_Mprobe", source, tag, comm, message, status);

#if AMPI_ERROR_CHECKING
  int ret = errorCheck("AMPI_Mprobe", comm, 1, 0, 0, 0, 0, tag, 1, source, 1, 0, 0);
  if(ret != MPI_SUCCESS)
    return ret;
#endif

  ampi *ptr = getAmpiInstance(comm);
  ptr->mprobe(tag, source, comm, status, message);

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Mrecv, void* buf, int count, MPI_Datatype datatype, MPI_Message *message,
                              MPI_Status *status)
{
  AMPI_API("AMPI_Mrecv", buf, count, datatype, message, status);

#if AMPI_ERROR_CHECKING
  if (*message == MPI_MESSAGE_NULL) {
    return ampiErrhandler("AMPI_Mrecv", MPI_ERR_REQUEST);
  }
#endif

  if (*message == MPI_MESSAGE_NO_PROC) {
    if (status != MPI_STATUS_IGNORE) {
      status->MPI_SOURCE = MPI_PROC_NULL;
      status->MPI_TAG = MPI_ANY_TAG;
      status->MPI_LENGTH = 0;
    }
    *message = MPI_MESSAGE_NULL;
    return MPI_SUCCESS;
  }

#if AMPI_ERROR_CHECKING
  int ret = errorCheck("AMPI_Mrecv", 0, 0, count, 1, datatype, 1, 0, 0, 0, 0, buf, 1);
  if(ret != MPI_SUCCESS)
    return ret;
#endif

  handle_MPI_BOTTOM(buf, datatype);

  ampiParent* parent = getAmpiParent();
  AmpiMsg *msg = parent->getMatchedMsg(*message);
  CkAssert(msg); // the matching message has already arrived
  MPI_Comm comm = msg->getComm();
  int src = msg->getSrcRank();
  int tag = msg->getTag();

#if AMPIMSGLOG
  ampiParent* pptr = getAmpiParent();
  if(msgLogRead){
    (*(pptr->fromPUPer))|(pptr->pupBytes);
    PUParray(*(pptr->fromPUPer), (char *)buf, (pptr->pupBytes));
    PUParray(*(pptr->fromPUPer), (char *)status, sizeof(MPI_Status));
    return MPI_SUCCESS;
  }
#endif

  ampi *ptr = getAmpiInstance(comm);
  if (msg->isSsend()) {
    AmpiRequestList& reqs = ptr->getReqs();
    IReq *newreq = parent->reqPool.newReq<IReq>(buf, count, datatype, src, tag, comm, getDDT());
    MPI_Request request = reqs.insert(newreq);
    newreq->receive(ptr, msg);
    parent = parent->wait(&request, status);
  }
  else {
    if (status != MPI_STATUS_IGNORE) {
      status->MPI_SOURCE = msg->getSrcRank();
      status->MPI_TAG    = msg->getTag();
      status->MPI_COMM   = comm;
      status->MPI_LENGTH = msg->getLength();
      status->MPI_CANCEL = 0;
    }
    ptr->processAmpiMsg(msg, buf, datatype, count, MPI_REQUEST_NULL);
    CkpvAccess(msgPool).deleteAmpiMsg(msg);
  }
  *message = MPI_MESSAGE_NULL;

#if AMPIMSGLOG
  if(msgLogWrite && record_msglog(pptr->thisIndex)){
    (pptr->pupBytes) = getDDT()->getSize(datatype) * count;
    (*(pptr->toPUPer))|(pptr->pupBytes);
    PUParray(*(pptr->toPUPer), (char *)buf, (pptr->pupBytes));
    PUParray(*(pptr->toPUPer), (char *)status, sizeof(MPI_Status));
  }
#endif

  return MPI_SUCCESS;
}

void ampi::sendrecv(const void *sbuf, int scount, MPI_Datatype stype, int dest, int stag,
                    void *rbuf, int rcount, MPI_Datatype rtype, int src, int rtag,
                    MPI_Comm comm, MPI_Status *sts) noexcept
{
  MPI_Request reqs[2];
  irecv(rbuf, rcount, rtype, src, rtag, comm, &reqs[0]);

  reqs[1] = send(stag, getRank(), sbuf, scount, stype, dest, comm, I_SEND);

  if (sts == MPI_STATUS_IGNORE) {
    parent = parent->waitall(2, reqs);
  }
  else {
    MPI_Status statuses[2];
    parent = parent->waitall(2, reqs, statuses);
    *sts = statuses[0];
  }
}

AMPI_API_IMPL(int, MPI_Sendrecv, const void *sbuf, int scount, MPI_Datatype stype, int dest,
                                 int stag, void *rbuf, int rcount, MPI_Datatype rtype,
                                 int src, int rtag, MPI_Comm comm, MPI_Status *sts)
{
  AMPI_API("AMPI_Sendrecv", sbuf, scount, stype, dest, stag, rbuf, rcount, rtype, src, rtag, comm, sts);

  handle_MPI_BOTTOM((void*&)sbuf, stype, rbuf, rtype);

#if AMPI_ERROR_CHECKING
  if(sbuf == MPI_IN_PLACE || rbuf == MPI_IN_PLACE)
    CkAbort("MPI_sendrecv does not accept MPI_IN_PLACE; use MPI_Sendrecv_replace instead.");
  int ret;
  ret = errorCheck("AMPI_Sendrecv", comm, 1, scount, 1, stype, 1, stag, 1, dest, 1, sbuf, 1);
  if(ret != MPI_SUCCESS)
    return ret;
  ret = errorCheck("AMPI_Sendrecv", comm, 1, rcount, 1, rtype, 1, rtag, 1, src, 1, rbuf, 1);
  if(ret != MPI_SUCCESS)
    return ret;
#endif

  ampi *ptr = getAmpiInstance(comm);

  ptr->sendrecv(sbuf, scount, stype, dest, stag,
                rbuf, rcount, rtype, src, rtag,
                comm, sts);

  return MPI_SUCCESS;
}

void ampi::sendrecv_replace(void* buf, int count, MPI_Datatype datatype,
                            int dest, int sendtag, int source, int recvtag,
                            MPI_Comm comm, MPI_Status *status) noexcept
{
  CkDDT_DataType* ddt = getDDT()->getType(datatype);
  std::vector<char> tmpBuf(ddt->getSize(count));
  ddt->serialize((char*)buf, tmpBuf.data(), count, ddt->getSize(count), PACK);

  MPI_Request reqs[2];
  irecv(buf, count, datatype, source, recvtag, comm, &reqs[0]);

  // FIXME: this send may do a copy internally! If we knew now that it would, we could avoid double copying:
  reqs[1] = send(sendtag, getRank(), tmpBuf.data(), count, datatype, dest, comm, I_SEND);

  if (status == MPI_STATUS_IGNORE) {
    parent = parent->waitall(2, reqs);
  }
  else {
    MPI_Status statuses[2];
    parent = parent->waitall(2, reqs, statuses);
    *status = statuses[0];
  }
}

AMPI_API_IMPL(int, MPI_Sendrecv_replace, void* buf, int count, MPI_Datatype datatype,
                                         int dest, int sendtag, int source, int recvtag,
                                         MPI_Comm comm, MPI_Status *status)
{
  AMPI_API("AMPI_Sendrecv_replace", buf, count, datatype, dest, sendtag, source, recvtag, comm, status);

  handle_MPI_BOTTOM(buf, datatype);

#if AMPI_ERROR_CHECKING
  int ret;
  ret = errorCheck("AMPI_Sendrecv_replace", comm, 1, count, 1, datatype, 1, sendtag, 1, dest, 1, buf, 1);
  if(ret != MPI_SUCCESS)
    return ret;
  ret = errorCheck("AMPI_Sendrecv_replace", comm, 1, count, 1, datatype, 1, recvtag, 1, source, 1, buf, 1);
  if(ret != MPI_SUCCESS)
    return ret;
#endif

  ampi* ptr = getAmpiInstance(comm);

  ptr->sendrecv_replace(buf, count, datatype, dest, sendtag, source, recvtag, comm, status);

  return MPI_SUCCESS;
}

CMI_WARN_UNUSED_RESULT ampi * ampi::barrier() noexcept
{
  CkAssert(parent->resumeOnColl == false);
  parent->resumeOnColl = true;
  CkCallback barrierCB(CkReductionTarget(ampi, barrierResult), getProxy());
  contribute(barrierCB);
  ampi * dis = block(); //Resumed by ampi::barrierResult
  dis->parent->resumeOnColl = false;
  return dis;
}

CMI_WARN_UNUSED_RESULT ampi * ampi::block() noexcept
{
  // In case this thread is migrated while suspended,
  // save myComm to get the ampi instance back. Then
  // return "dis" in case the caller needs it.
  MPI_Comm disComm = myComm->getComm();
  ampiParent * disParent = parent->block();
  ampi * dis = getAmpiInstance(disComm);
  dis->parent = disParent;
  dis->thread = TCharm::get();
  return dis;
}

CMI_WARN_UNUSED_RESULT ampi * ampi::yield() noexcept
{
  // In case this thread is migrated while suspended,
  // save myComm to get the ampi instance back. Then
  // return "dis" in case the caller needs it.
  MPI_Comm disComm = myComm->getComm();
  ampiParent * disParent = parent->yield();
  ampi * dis = getAmpiInstance(disComm);
  dis->parent = disParent;
  dis->thread = TCharm::get();
  return dis;
}

void ampi::barrierResult() noexcept
{
  MSG_ORDER_DEBUG(CkPrintf("[%d] barrierResult called\n", thisIndex));
  CkAssert(parent->resumeOnColl == true);
  thread->resume();
}

AMPI_API_IMPL(int, MPI_Barrier, MPI_Comm comm)
{
  AMPI_API("AMPI_Barrier", comm);

#if AMPI_ERROR_CHECKING
  int ret = checkCommunicator("AMPI_Barrier", comm);
  if(ret != MPI_SUCCESS)
    return ret;
#endif


  ampi *ptr = getAmpiInstance(comm);
  MSG_ORDER_DEBUG(CkPrintf("[%d] AMPI_Barrier called on comm %d\n", ptr->thisIndex, comm));

  if (ptr->getSize() == 1 && !getAmpiParent()->isInter(comm))
    return MPI_SUCCESS;

  // implementation of intercomm barrier is equivalent to that for intracomm barrier

  ptr = ptr->barrier();

  return MPI_SUCCESS;
}

void ampi::ibarrier(MPI_Request *request) noexcept
{
  *request = postReq(parent->reqPool.newReq<IReq>(nullptr, 0, MPI_INT, AMPI_COLL_SOURCE, MPI_ATA_TAG, myComm->getComm(), getDDT()));
  CkCallback ibarrierCB(CkReductionTarget(ampi, ibarrierResult), getProxy());
  contribute(ibarrierCB);
}

void ampi::ibarrierResult() noexcept
{
  MSG_ORDER_DEBUG(CkPrintf("[%d] ibarrierResult called\n", thisIndex));
  ampi::sendraw(MPI_ATA_TAG, AMPI_COLL_SOURCE, NULL, 0, thisArrayID, thisIndex);
}

AMPI_API_IMPL(int, MPI_Ibarrier, MPI_Comm comm, MPI_Request *request)
{
  AMPI_API("AMPI_Ibarrier", comm, request);

#if AMPI_ERROR_CHECKING
  int ret = checkCommunicator("AMPI_Ibarrier", comm);
  if(ret != MPI_SUCCESS){
    *request = MPI_REQUEST_NULL;
    return ret;
  }
#endif

  ampi *ptr = getAmpiInstance(comm);

  if (ptr->getSize() == 1 && !getAmpiParent()->isInter(comm)) {
    *request = ptr->postReq(getAmpiParent()->reqPool.newReq<IReq>(nullptr, 0, MPI_INT, AMPI_COLL_SOURCE, MPI_ATA_TAG, AMPI_COLL_COMM,
                            getDDT(), AMPI_REQ_COMPLETED));
    return MPI_SUCCESS;
  }

  // implementation of intercomm ibarrier is equivalent to that for intracomm ibarrier


  MSG_ORDER_DEBUG(CkPrintf("[%d] AMPI_Ibarrier called on comm %d\n", ptr->thisIndex, comm));

  ptr->ibarrier(request);

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Bcast, void *buf, int count, MPI_Datatype type, int root, MPI_Comm comm)
{
  AMPI_API("AMPI_Bcast", buf, count, type, root, comm);

  handle_MPI_BOTTOM(buf, type);

#if AMPI_ERROR_CHECKING
  int validateBuf = 1;
  if (getAmpiParent()->isInter(comm)) {
    //if comm is an intercomm, then only root and remote ranks need to have a valid buf
    //local ranks need not validate it
    if (root==MPI_PROC_NULL) validateBuf = 0;
  }
  int ret = errorCheck("AMPI_Bcast", comm, 1, count, 1, type, 1, 0, 0, root, 1, buf, validateBuf);

  if(ret != MPI_SUCCESS)
    return ret;
#endif

  ampi* ptr = getAmpiInstance(comm);

  if(getAmpiParent()->isInter(comm)) {
    return ptr->intercomm_bcast(root, buf, count, type, comm);
  }
  if(ptr->getSize() == 1)
    return MPI_SUCCESS;

#if AMPIMSGLOG
  ampiParent* pptr = getAmpiParent();
  if(msgLogRead){
    (*(pptr->fromPUPer))|(pptr->pupBytes);
    PUParray(*(pptr->fromPUPer), (char *)buf, (pptr->pupBytes));
    return MPI_SUCCESS;
  }
#endif

  ptr->bcast(root, buf, count, type,comm);

#if AMPIMSGLOG
  if(msgLogWrite && record_msglog(pptr->thisIndex)) {
    (pptr->pupBytes) = getDDT()->getSize(type) * count;
    (*(pptr->toPUPer))|(pptr->pupBytes);
    PUParray(*(pptr->toPUPer), (char *)buf, (pptr->pupBytes));
  }
#endif

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Ibcast, void *buf, int count, MPI_Datatype type, int root,
                               MPI_Comm comm, MPI_Request *request)
{
  AMPI_API("AMPI_Ibcast", buf, count, type, root, comm, request);

  handle_MPI_BOTTOM(buf, type);

#if AMPI_ERROR_CHECKING
  int validateBuf = 1;
  if (getAmpiParent()->isInter(comm)) {
    //if comm is an intercomm, then only root and remote ranks need to have a valid buf
    //local ranks need not validate it
    if (root==MPI_PROC_NULL) validateBuf = 0;
  }
  int ret = errorCheck("AMPI_Ibcast", comm, 1, count, 1, type, 1, 0, 0, root, 1, buf, validateBuf);

  if(ret != MPI_SUCCESS){
    *request = MPI_REQUEST_NULL;
    return ret;
  }
#endif

  ampi* ptr = getAmpiInstance(comm);

  if(getAmpiParent()->isInter(comm)) {
    return ptr->intercomm_ibcast(root, buf, count, type, comm, request);
  }
  if(ptr->getSize() == 1){
    *request = ptr->postReq(getAmpiParent()->reqPool.newReq<IReq>(buf, count, type, root, MPI_BCAST_TAG, comm,
                            getDDT(), AMPI_REQ_COMPLETED));
    return MPI_SUCCESS;
  }

#if AMPIMSGLOG
  ampiParent* pptr = getAmpiParent();
  if(msgLogRead){
    (*(pptr->fromPUPer))|(pptr->pupBytes);
    PUParray(*(pptr->fromPUPer), (char *)buf, (pptr->pupBytes));
    return MPI_SUCCESS;
  }
#endif

  ptr->ibcast(root, buf, count, type, comm, request);

#if AMPIMSGLOG
  if(msgLogWrite && record_msglog(pptr->thisIndex)) {
    (pptr->pupBytes) = getDDT()->getSize(type) * count;
    (*(pptr->toPUPer))|(pptr->pupBytes);
    PUParray(*(pptr->toPUPer), (char *)buf, (pptr->pupBytes));
  }
#endif

  return MPI_SUCCESS;
}

// This routine is called with the results of an (All)Reduce or (All)Gather(v)
void ampi::rednResult(CkReductionMsg *msg) noexcept
{
  MSG_ORDER_DEBUG(CkPrintf("[%d] rednResult called on comm %d\n", thisIndex, myComm->getComm()));

#if CMK_ERROR_CHECKING
  if (parent->blockingReq == NULL) {
    CkAbort("AMPI> recv'ed a blocking reduction unexpectedly!\n");
  }
#endif


  parent->blockingReq->receive(this, msg);

  CkAssert(parent->resumeOnColl);
  thread->resume();
  // [nokeep] entry method, so do not delete msg
}

// This routine is called with the results of an I(all)reduce or I(all)gather(v)
void ampi::irednResult(CkReductionMsg *msg) noexcept
{
  MSG_ORDER_DEBUG(CkPrintf("[%d] irednResult called on comm %d\n", thisIndex, myComm->getComm()));

  AmpiRequest* req = postedReqs.get(MPI_REDN_TAG, AMPI_COLL_SOURCE);
  if (req == NULL)
    CkAbort("AMPI> recv'ed a non-blocking reduction unexpectedly!\n");

#if AMPIMSGLOG
  if(msgLogRead){
    PUParray(*(getAmpiParent()->fromPUPer), (char *)req, sizeof(int));
    return;
  }
#endif

  handleBlockedReq(req);
  req->receive(this, msg);

#if AMPIMSGLOG
  if(msgLogWrite && record_msglog(getAmpiParent()->thisIndex)){
    PUParray(*(getAmpiParent()->toPUPer), (char *)reqnReq, sizeof(int));
  }
#endif

  if (parent->resumeOnColl && parent->numBlockedReqs==0) {
    thread->resume();
  }
  // [nokeep] entry method, so do not delete msg
}

static CkReductionMsg *makeRednMsg(CkDDT_DataType *ddt, const void *inbuf, int count, int type,
                                   int rank, int size, MPI_Op op) noexcept
{
  CkReductionMsg *msg;
  ampiParent *parent = getAmpiParent();
  int szdata = ddt->getSize(count);
  CkReduction::reducerType reducer = getBuiltinReducerType(type, op);

  if (reducer != CkReduction::invalid) {
    // MPI predefined op matches a Charm++ builtin reducer type
    AMPI_DEBUG("[%d] In makeRednMsg, using Charm++ built-in reducer type for a predefined op\n", parent->thisIndex);
    msg = CkReductionMsg::buildNew(szdata, NULL, reducer);
    ddt->serialize((char*)inbuf, (char*)msg->getData(), count, msg->getLength(), PACK);
  }
  else if (parent->opIsCommutative(op) && ddt->isContig()) {
    // Either an MPI predefined reducer operation with no Charm++ builtin reducer type equivalent, or
    // a commutative user-defined reducer operation on a contiguous datatype
    AMPI_DEBUG("[%d] In makeRednMsg, using custom AmpiReducer type for a commutative op\n", parent->thisIndex);
    AmpiOpHeader newhdr = parent->op2AmpiOpHeader(op, type, count);
    int szhdr = sizeof(AmpiOpHeader);
    msg = CkReductionMsg::buildNew(szdata+szhdr, NULL, AmpiReducer);
    memcpy(msg->getData(), &newhdr, szhdr);
    ddt->serialize((char*)inbuf, (char*)msg->getData()+szhdr, count, msg->getLength()-szhdr, PACK);
  }
  else {
    // Non-commutative user-defined reducer operation, or
    // a commutative user-defined reduction on a non-contiguous datatype
    AMPI_DEBUG("[%d] In makeRednMsg, using a non-commutative user-defined operation\n", parent->thisIndex);
    const int tupleSize = 2;
    CkReduction::tupleElement tupleRedn[tupleSize];

    // Contribute rank as an unsigned short int if the max rank value fits into it, otherwise as an int
    unsigned short int ushortRank;
    if (size < std::numeric_limits<unsigned short int>::max()) {
      ushortRank = static_cast<unsigned short int>(rank);
      tupleRedn[0] = CkReduction::tupleElement(sizeof(unsigned short int), &ushortRank, CkReduction::concat);
    } else {
      tupleRedn[0] = CkReduction::tupleElement(sizeof(int), &rank, CkReduction::concat);
    }

    std::vector<char> sbuf;
    if (!ddt->isContig()) {
      sbuf.resize(szdata);
      ddt->serialize((char*)inbuf, sbuf.data(), count, szdata, PACK);
      tupleRedn[1] = CkReduction::tupleElement(szdata, sbuf.data(), CkReduction::concat);
    }
    else {
      tupleRedn[1] = CkReduction::tupleElement(szdata, (void*)inbuf, CkReduction::concat);
    }
    msg = CkReductionMsg::buildFromTuple(tupleRedn, tupleSize);
  }
  return msg;
}

// Copy the MPI datatype "type" from inbuf to outbuf
static int copyDatatype(MPI_Datatype sendtype, int sendcount, MPI_Datatype recvtype,
                        int recvcount, const void *inbuf, void *outbuf) noexcept
{
  if (inbuf == outbuf) return MPI_SUCCESS; // handle MPI_IN_PLACE

  CkDDT_DataType *sddt = getDDT()->getType(sendtype);
  CkDDT_DataType *rddt = getDDT()->getType(recvtype);

  if (sddt->isContig() && rddt->isContig()) {
    int slen = sddt->getSize(sendcount);
    memcpy(outbuf, inbuf, slen);
  } else if (sddt->isContig()) {
    rddt->serialize((char*)outbuf, (char*)inbuf, recvcount, sddt->getSize(sendcount), UNPACK);
  } else if (rddt->isContig()) {
    sddt->serialize((char*)inbuf, (char*)outbuf, sendcount, rddt->getSize(recvcount), PACK);
  } else {
    // ddts don't have "copy", so fake it by serializing into a temp buffer, then
    //  deserializing into the output.
    int slen = sddt->getSize(sendcount);
    std::vector<char> serialized(slen);
    sddt->serialize((char*)inbuf, serialized.data(), sendcount, rddt->getSize(recvcount), PACK);
    rddt->serialize((char*)outbuf, serialized.data(), recvcount, sddt->getSize(sendcount), UNPACK);
  }

  return MPI_SUCCESS;
}

static void handle_MPI_IN_PLACE(void* &inbuf, void* &outbuf) noexcept
{
  if (inbuf == MPI_IN_PLACE) inbuf = outbuf;
  if (outbuf == MPI_IN_PLACE) outbuf = inbuf;
  CkAssert(inbuf != MPI_IN_PLACE && outbuf != MPI_IN_PLACE);
}

static void handle_MPI_IN_PLACE_gather(void* &sendbuf, void* recvbuf, int &sendcount,
                                       MPI_Datatype &sendtype, int recvdispl,
                                       int recvcount, MPI_Datatype recvtype) noexcept
{
  if (sendbuf == MPI_IN_PLACE) {
    // The MPI standard says that when MPI_IN_PLACE is passed to any of the gather
    // variants, the contribution of the root to the gathered vector is assumed
    // to be already in the correct place in the receive buffer.
    sendbuf   = (char*)recvbuf + (recvdispl * getDDT()->getExtent(recvtype));
    sendcount = recvcount;
    sendtype  = recvtype;
  }
  CkAssert(recvbuf != MPI_IN_PLACE);
}

static void handle_MPI_IN_PLACE_gatherv(void* &sendbuf, void* recvbuf, int &sendcount,
                                        MPI_Datatype &sendtype, const int recvdispls[],
                                        const int recvcounts[], int rank,
                                        MPI_Datatype recvtype) noexcept
{
  if (sendbuf == MPI_IN_PLACE) {
    // The MPI standard says that when MPI_IN_PLACE is passed to any of the gather
    // variants, the contribution of the root to the gathered vector is assumed
    // to be already in the correct place in the receive buffer.
    CkAssert(recvbuf != NULL && recvdispls != NULL && recvcounts != NULL);
    sendbuf   = (char*)recvbuf + (recvdispls[rank] * getDDT()->getExtent(recvtype));
    sendcount = recvcounts[rank];
    sendtype  = recvtype;
  }
  CkAssert(recvbuf != MPI_IN_PLACE);
}

static void handle_MPI_IN_PLACE_alltoall(void* &sendbuf, void* recvbuf, int &sendcount,
                                         MPI_Datatype &sendtype, int recvcount,
                                         MPI_Datatype recvtype) noexcept
{
  if (sendbuf == MPI_IN_PLACE) {
    sendbuf   = recvbuf;
    sendcount = recvcount;
    sendtype  = recvtype;
  }
  CkAssert(recvbuf != MPI_IN_PLACE);
}

static void handle_MPI_IN_PLACE_alltoallv(void* &sendbuf, void* recvbuf, int* &sendcounts,
                                          MPI_Datatype &sendtype, int* &sdispls,
                                          const int* recvcounts, MPI_Datatype recvtype,
                                          const int* rdispls) noexcept
{
  if (sendbuf == MPI_IN_PLACE) {
    sendbuf    = recvbuf;
    sendcounts = (int*)recvcounts;
    sendtype   = recvtype;
    sdispls    = (int*)rdispls;
  }
  CkAssert(recvbuf != MPI_IN_PLACE);
}

static void handle_MPI_IN_PLACE_alltoallw(void* &sendbuf, void* recvbuf, int* &sendcounts,
                                          MPI_Datatype* &sendtypes, int* &sdispls,
                                          const int* recvcounts, const MPI_Datatype* recvtypes,
                                          const int* rdispls) noexcept
{
  if (sendbuf == MPI_IN_PLACE) {
    sendbuf    = recvbuf;
    sendcounts = (int*)recvcounts;
    sendtypes  = (MPI_Datatype*)recvtypes;
    sdispls    = (int*)rdispls;
  }
  CkAssert(recvbuf != MPI_IN_PLACE);
}

#define AMPI_SYNC_REDUCE 0

AMPI_API_IMPL(int, MPI_Reduce, const void *inbuf, void *outbuf, int count, MPI_Datatype type,
                               MPI_Op op, int root, MPI_Comm comm)
{
  AMPI_API("AMPI_Reduce", inbuf, outbuf, count, type, op, root, comm);

  handle_MPI_BOTTOM((void*&)inbuf, type, outbuf, type);
  handle_MPI_IN_PLACE((void*&)inbuf, outbuf);

#if AMPI_ERROR_CHECKING
  if(op == MPI_OP_NULL)
    return ampiErrhandler("AMPI_Reduce", MPI_ERR_OP);
  int ret = errorCheck("AMPI_Reduce", comm, 1, count, 1, type, 1, 0, 0, root, 1, inbuf, 1,
                       outbuf, getAmpiInstance(comm)->getRank() == root);
  if(ret != MPI_SUCCESS)
    return ret;
#endif

  ampi *ptr = getAmpiInstance(comm);
  int rank = ptr->getRank();
  int size = ptr->getSize();

  if(ptr->isInter())
    CkAbort("AMPI does not implement MPI_Reduce for Inter-communicators!");
  if(size == 1)
    return copyDatatype(type,count,type,count,inbuf,outbuf);

#if AMPIMSGLOG
  if(msgLogRead){
    ampiParent* pptr = getAmpiParent();
    (*(pptr->fromPUPer))|(pptr->pupBytes);
    PUParray(*(pptr->fromPUPer), (char *)outbuf, (pptr->pupBytes));
    return MPI_SUCCESS;
  }
#endif

  if (rank == root) {
    ptr->setBlockingReq(new RednReq(outbuf, count, type, comm, op, getDDT()));
  }

  int rootIdx=ptr->comm2CommStruct(comm).getIndexForRank(root);
  CkReductionMsg *msg=makeRednMsg(ptr->getDDT()->getType(type),inbuf,count,type,rank,size,op);
  CkCallback reduceCB(CkIndex_ampi::rednResult(0),CkArrayIndex1D(rootIdx),ptr->getProxy());
  msg->setCallback(reduceCB);
  MSG_ORDER_DEBUG(CkPrintf("[%d] AMPI_Reduce called on comm %d root %d \n",ptr->thisIndex,comm,rootIdx));
  ptr->contribute(msg);

  if (rank == root) {
    ptr = ptr->blockOnColl();

#if AMPI_SYNC_REDUCE
    AmpiMsg *msg = new (0, 0) AmpiMsg(0, MPI_REQUEST_NULL, MPI_REDN_TAG, -1, rootIdx, 0);
    CProxy_ampi pa(ptr->getProxy());
    pa.generic(msg);
#endif
  }
#if AMPI_SYNC_REDUCE
  ptr->recv(MPI_REDN_TAG, AMPI_COLL_SOURCE, NULL, 0, type, comm);
#endif

#if AMPIMSGLOG
  if(msgLogWrite){
    ampiParent* pptr = getAmpiParent();
    if(record_msglog(pptr->thisIndex)){
      (pptr->pupBytes) = getDDT()->getSize(type) * count;
      (*(pptr->toPUPer))|(pptr->pupBytes);
      PUParray(*(pptr->toPUPer), (char *)outbuf, (pptr->pupBytes));
    }
  }
#endif

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Allreduce, const void *inbuf, void *outbuf, int count, MPI_Datatype type,
                                  MPI_Op op, MPI_Comm comm)
{
  AMPI_API("AMPI_Allreduce", inbuf, outbuf, count, type, op, comm);

  handle_MPI_BOTTOM((void*&)inbuf, type, outbuf, type);
  handle_MPI_IN_PLACE((void*&)inbuf, outbuf);

#if AMPI_ERROR_CHECKING
  if(op == MPI_OP_NULL)
    return ampiErrhandler("AMPI_Allreduce", MPI_ERR_OP);
  int ret = errorCheck("AMPI_Allreduce", comm, 1, count, 1, type, 1, 0, 0, 0, 0, inbuf, 1, outbuf, 1);
  if(ret != MPI_SUCCESS)
    return ret;
#endif

  ampi *ptr = getAmpiInstance(comm);
  int rank = ptr->getRank();
  int size = ptr->getSize();

  if(ptr->isInter())
    CkAbort("AMPI does not implement MPI_Allreduce for Inter-communicators!");
  if(size == 1)
    return copyDatatype(type,count,type,count,inbuf,outbuf);


#if AMPIMSGLOG
  if(msgLogRead){
    ampiParent* pptr = getAmpiParent();
    (*(pptr->fromPUPer))|(pptr->pupBytes);
    PUParray(*(pptr->fromPUPer), (char *)outbuf, (pptr->pupBytes));
    return MPI_SUCCESS;
  }
#endif

  ptr->setBlockingReq(new RednReq(outbuf, count, type, comm, op, getDDT()));

  CkReductionMsg *msg=makeRednMsg(ptr->getDDT()->getType(type), inbuf, count, type, rank, size, op);
  CkCallback allreduceCB(CkIndex_ampi::rednResult(0),ptr->getProxy());
  msg->setCallback(allreduceCB);
  ptr->contribute(msg);

  ptr = ptr->blockOnColl();

#if AMPIMSGLOG
  if(msgLogWrite){
    ampiParent* pptr = getAmpiParent();
    if(record_msglog(pptr->thisIndex)){
      (pptr->pupBytes) = getDDT()->getSize(type) * count;
      (*(pptr->toPUPer))|(pptr->pupBytes);
      PUParray(*(pptr->toPUPer), (char *)outbuf, (pptr->pupBytes));
    }
  }
#endif

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Iallreduce, const void *inbuf, void *outbuf, int count, MPI_Datatype type,
                                   MPI_Op op, MPI_Comm comm, MPI_Request* request)
{
  AMPI_API("AMPI_Iallreduce", inbuf, outbuf, count, type, op, comm, request);

  handle_MPI_BOTTOM((void*&)inbuf, type, outbuf, type);
  handle_MPI_IN_PLACE((void*&)inbuf, outbuf);

#if AMPI_ERROR_CHECKING
  if(op == MPI_OP_NULL)
    return ampiErrhandler("AMPI_Iallreduce", MPI_ERR_OP);
  int ret = errorCheck("AMPI_Iallreduce", comm, 1, count, 1, type, 1, 0, 0, 0, 0, inbuf, 1, outbuf, 1);
  if(ret != MPI_SUCCESS){
    *request = MPI_REQUEST_NULL;
    return ret;
  }
#endif

  ampi *ptr = getAmpiInstance(comm);
  int rank = ptr->getRank();
  int size = ptr->getSize();

  if(ptr->isInter())
    CkAbort("AMPI does not implement MPI_Iallreduce for Inter-communicators!");
  if(size == 1){
    *request = ptr->postReq(new RednReq(outbuf,count,type,comm,op,getDDT(),AMPI_REQ_COMPLETED));
    return copyDatatype(type,count,type,count,inbuf,outbuf);
  }

  *request = ptr->postReq(new RednReq(outbuf,count,type,comm,op,getDDT()));

  CkReductionMsg *msg=makeRednMsg(ptr->getDDT()->getType(type),inbuf,count,type,rank,size,op);
  CkCallback allreduceCB(CkIndex_ampi::irednResult(0),ptr->getProxy());
  msg->setCallback(allreduceCB);
  ptr->contribute(msg);

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Reduce_local, const void *inbuf, void *outbuf, int count,
                                     MPI_Datatype type, MPI_Op op)
{
  AMPI_API("AMPI_Reduce_local", inbuf, outbuf, count, type, op);

  handle_MPI_BOTTOM((void*&)inbuf, type, outbuf, type);

#if AMPI_ERROR_CHECKING
  if(op == MPI_OP_NULL)
    return ampiErrhandler("AMPI_Reduce_local", MPI_ERR_OP);
  if(inbuf == MPI_IN_PLACE || outbuf == MPI_IN_PLACE)
    CkAbort("MPI_Reduce_local does not accept MPI_IN_PLACE!");
  int ret = errorCheck("AMPI_Reduce_local", MPI_COMM_SELF, 1, count, 1, type, 1, 0, 0, 0, 1, inbuf, 1, outbuf, 1);
  if(ret != MPI_SUCCESS)
    return ret;
#endif

  getAmpiParent()->applyOp(type, op, count, inbuf, outbuf);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Reduce_scatter_block, const void* sendbuf, void* recvbuf, int count,
                                             MPI_Datatype datatype, MPI_Op op, MPI_Comm comm)
{
  AMPI_API("AMPI_Reduce_scatter_block", sendbuf, recvbuf, count, datatype, op, comm);

  handle_MPI_BOTTOM((void*&)sendbuf, datatype, recvbuf, datatype);
  handle_MPI_IN_PLACE((void*&)sendbuf, recvbuf);

#if AMPI_ERROR_CHECKING
  if(op == MPI_OP_NULL)
    return ampiErrhandler("AMPI_Reduce_scatter_block", MPI_ERR_OP);
  int ret = errorCheck("AMPI_Reduce_scatter_block", comm, 1, 0, 0, datatype, 1, 0, 0, 0, 0, sendbuf, 1, recvbuf, 1);
  if(ret != MPI_SUCCESS)
    return ret;
#endif

  ampi *ptr = getAmpiInstance(comm);
  int size = ptr->getSize();

  if(getAmpiParent()->isInter(comm))
    CkAbort("AMPI does not implement MPI_Reduce_scatter_block for Inter-communicators!");
  if(size == 1)
    return copyDatatype(datatype, count, datatype, count, sendbuf, recvbuf);

  std::vector<char> tmpbuf(ptr->getDDT()->getType(datatype)->getSize(count)*size);

  MPI_Reduce(sendbuf, &tmpbuf[0], count*size, datatype, op, AMPI_COLL_SOURCE, comm);
  MPI_Scatter(&tmpbuf[0], count, datatype, recvbuf, count, datatype, AMPI_COLL_SOURCE, comm);

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Ireduce_scatter_block, const void* sendbuf, void* recvbuf, int count,
                                              MPI_Datatype datatype, MPI_Op op, MPI_Comm comm,
                                              MPI_Request* request)
{
  AMPI_API("AMPI_Ireduce_scatter_block", sendbuf, recvbuf, count, datatype, op, comm, request);
  // FIXME: implement non-blocking reduce_scatter_block
  int ret = MPI_Reduce_scatter_block(sendbuf, recvbuf, count, datatype, op, comm);
  *request = MPI_REQUEST_NULL;
  return ret;
}

AMPI_API_IMPL(int, MPI_Reduce_scatter, const void* sendbuf, void* recvbuf, const int *recvcounts,
                                       MPI_Datatype datatype, MPI_Op op, MPI_Comm comm)
{
  AMPI_API("AMPI_Reduce_scatter", sendbuf, recvbuf, recvcounts, datatype, op, comm);

  handle_MPI_BOTTOM((void*&)sendbuf, datatype, recvbuf, datatype);
  handle_MPI_IN_PLACE((void*&)sendbuf, recvbuf);

#if AMPI_ERROR_CHECKING
  if(op == MPI_OP_NULL)
    return ampiErrhandler("AMPI_Reduce_scatter", MPI_ERR_OP);
  int ret = errorCheck("AMPI_Reduce_scatter", comm, 1, 0, 0, datatype, 1, 0, 0, 0, 0, sendbuf, 1, recvbuf, 1);
  if(ret != MPI_SUCCESS)
    return ret;
#endif

  ampi *ptr = getAmpiInstance(comm);
  int size = ptr->getSize();

  if(getAmpiParent()->isInter(comm))
    CkAbort("AMPI does not implement MPI_Reduce_scatter for Inter-communicators!");
  if(size == 1)
    return copyDatatype(datatype,recvcounts[0],datatype,recvcounts[0],sendbuf,recvbuf);

  int count=0;
  std::vector<int> displs(size);
  int len;

  //under construction
  for(int i=0;i<size;i++){
    displs[i] = count;
    count+= recvcounts[i];
  }
  std::vector<char> tmpbuf(ptr->getDDT()->getType(datatype)->getSize(count));
  MPI_Reduce(sendbuf, tmpbuf.data(), count, datatype, op, AMPI_COLL_SOURCE, comm);
  MPI_Scatterv(tmpbuf.data(), recvcounts, displs.data(), datatype,
                          recvbuf, recvcounts[ptr->getRank()], datatype, AMPI_COLL_SOURCE, comm);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Ireduce_scatter, const void* sendbuf, void* recvbuf, const int *recvcounts,
                                        MPI_Datatype datatype, MPI_Op op, MPI_Comm comm, MPI_Request* request)
{
  AMPI_API("AMPI_Ireduce_scatter", sendbuf, recvbuf, recvcounts, datatype, op, comm, request);
  // FIXME: implement non-blocking reduce_scatter
  int ret = MPI_Reduce_scatter(sendbuf, recvbuf, recvcounts, datatype, op, comm);
  *request = MPI_REQUEST_NULL;
  return ret;
}

AMPI_API_IMPL(int, MPI_Scan, const void* sendbuf, void* recvbuf, int count, MPI_Datatype datatype,
                             MPI_Op op, MPI_Comm comm)
{
  AMPI_API("AMPI_Scan", sendbuf, recvbuf, count, datatype, op, comm);

  handle_MPI_BOTTOM((void*&)sendbuf, datatype, recvbuf, datatype);
  handle_MPI_IN_PLACE((void*&)sendbuf,recvbuf);

#if AMPI_ERROR_CHECKING
  if(op == MPI_OP_NULL)
    return ampiErrhandler("AMPI_Scan", MPI_ERR_OP);
  int ret = errorCheck("AMPI_Scan", comm, 1, count, 1, datatype, 1, 0, 0, 0, 0, sendbuf, 1, recvbuf, 1);
  if(ret != MPI_SUCCESS)
    return ret;
#endif

  ampi *ptr = getAmpiInstance(comm);
  int size = ptr->getSize();

  if (size == 1 && !getAmpiParent()->isInter(comm))
    return copyDatatype(datatype, count, datatype, count, sendbuf, recvbuf);

  int blklen = ptr->getDDT()->getType(datatype)->getSize(count);
  int rank = ptr->getRank();
  int mask = 0x1;
  int dst;
  std::vector<char> tmp_buf(blklen);
  std::vector<char> partial_scan(blklen);

  memcpy(recvbuf, sendbuf, blklen);
  memcpy(partial_scan.data(), sendbuf, blklen);
  while(mask < size){
    dst = rank^mask;
    if(dst < size){
      ptr->sendrecv(partial_scan.data(), count, datatype, dst, MPI_SCAN_TAG,
                    tmp_buf.data(), count, datatype, dst, MPI_SCAN_TAG, comm, MPI_STATUS_IGNORE);
      if(rank > dst){
        getAmpiParent()->applyOp(datatype, op, count, tmp_buf.data(), partial_scan.data());
        getAmpiParent()->applyOp(datatype, op, count, tmp_buf.data(), recvbuf);
      }else {
        getAmpiParent()->applyOp(datatype, op, count, partial_scan.data(), tmp_buf.data());
        memcpy(partial_scan.data(), tmp_buf.data(), blklen);
      }
    }
    mask <<= 1;
  }

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Iscan, const void* sendbuf, void* recvbuf, int count, MPI_Datatype datatype,
                              MPI_Op op, MPI_Comm comm, MPI_Request* request)
{
  AMPI_API("AMPI_Iscan", sendbuf, recvbuf, count, datatype, op, comm, request);
  // FIXME: implement non-blocking scan
  int ret = MPI_Scan(sendbuf, recvbuf, count, datatype, op, comm);
  *request = MPI_REQUEST_NULL;
  return ret;
}

AMPI_API_IMPL(int, MPI_Exscan, const void* sendbuf, void* recvbuf, int count, MPI_Datatype datatype,
                               MPI_Op op, MPI_Comm comm)
{
  AMPI_API("AMPI_Exscan", sendbuf, recvbuf, count, datatype, op, comm);

  handle_MPI_BOTTOM((void*&)sendbuf, datatype, recvbuf, datatype);
  handle_MPI_IN_PLACE((void*&)sendbuf,recvbuf);

#if AMPI_ERROR_CHECKING
  if(op == MPI_OP_NULL)
    return ampiErrhandler("AMPI_Exscan", MPI_ERR_OP);
  int ret = errorCheck("AMPI_Excan", comm, 1, count, 1, datatype, 1, 0, 0, 0, 0, sendbuf, 1, recvbuf, 1);
  if(ret != MPI_SUCCESS)
    return ret;
#endif

  ampi *ptr = getAmpiInstance(comm);
  int size = ptr->getSize();

  if (size == 1 && !getAmpiParent()->isInter(comm))
    return MPI_SUCCESS;

  int blklen = ptr->getDDT()->getType(datatype)->getSize(count);
  int rank = ptr->getRank();
  int mask = 0x1;
  int dst, flag;
  std::vector<char> tmp_buf(blklen);
  std::vector<char> partial_scan(blklen);

  if (rank > 0) memcpy(recvbuf, sendbuf, blklen);
  memcpy(partial_scan.data(), sendbuf, blklen);
  flag = 0;
  mask = 0x1;
  while(mask < size){
    dst = rank^mask;
    if(dst < size){
      ptr->sendrecv(partial_scan.data(), count, datatype, dst, MPI_EXSCAN_TAG,
                    tmp_buf.data(), count, datatype, dst, MPI_EXSCAN_TAG, comm, MPI_STATUS_IGNORE);
      if(rank > dst){
        getAmpiParent()->applyOp(datatype, op, count, tmp_buf.data(), partial_scan.data());
        if(rank != 0){
          if(flag == 0){
            memcpy(recvbuf, tmp_buf.data(), blklen);
            flag = 1;
          }
          else{
            getAmpiParent()->applyOp(datatype, op, count, tmp_buf.data(), recvbuf);
          }
        }
      }
      else{
        getAmpiParent()->applyOp(datatype, op, count, partial_scan.data(), tmp_buf.data());
        memcpy(partial_scan.data(), tmp_buf.data(), blklen);
      }
      mask <<= 1;
    }
  }

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Iexscan, const void* sendbuf, void* recvbuf, int count, MPI_Datatype datatype,
                                MPI_Op op, MPI_Comm comm, MPI_Request* request)
{
  AMPI_API("AMPI_Iexscan", sendbuf, recvbuf, count, datatype, op, comm, request);
  // FIXME: implement non-blocking exscan
  int ret = MPI_Exscan(sendbuf, recvbuf, count, datatype, op, comm);
  *request = MPI_REQUEST_NULL;
  return ret;
}

AMPI_API_IMPL(int, MPI_Op_create, MPI_User_function *function, int commute, MPI_Op *op)
{
  AMPI_API("AMPI_Op_create", function, commute, op);
  *op = getAmpiParent()->createOp(function, commute);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Op_free, MPI_Op *op)
{
  AMPI_API("AMPI_Op_free", op, *op);
  getAmpiParent()->freeOp(*op);
  *op = MPI_OP_NULL;
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Op_commutative, MPI_Op op, int *commute)
{
  AMPI_API("AMPI_Op_commutative", op, commute);
  if (op == MPI_OP_NULL)
    return ampiErrhandler("AMPI_Op_commutative", MPI_ERR_OP);
  *commute = (int)getAmpiParent()->opIsCommutative(op);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(double, MPI_Wtime, void)
{
  //AMPI_API("AMPI_Wtime");

#if AMPIMSGLOG
  double ret=TCHARM_Wall_timer();
  ampiParent* pptr = getAmpiParent();
  if(msgLogRead){
    (*(pptr->fromPUPer))|ret;
    return ret;
  }

  if(msgLogWrite && record_msglog(pptr->thisIndex)){
    (*(pptr->toPUPer))|ret;
  }
#endif

  return TCHARM_Wall_timer();
}

AMPI_API_IMPL(double, MPI_Wtick, void)
{
  //AMPI_API("AMPI_Wtick");
  return 1e-6;
}

AMPI_API_IMPL(int, MPI_Start, MPI_Request *request)
{
  AMPI_API("AMPI_Start", request);
  checkRequest(*request);
  AmpiRequestList& reqs = getReqs();
#if AMPI_ERROR_CHECKING
  if (!reqs[*request]->isPersistent())
    return ampiErrhandler("AMPI_Start", MPI_ERR_REQUEST);
#endif
  reqs[*request]->start(*request);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Startall, int count, MPI_Request *requests)
{
  AMPI_API("AMPI_Startall", count, requests);
  checkRequests(count,requests);
  AmpiRequestList& reqs = getReqs();
  for(int i=0;i<count;i++){
#if AMPI_ERROR_CHECKING
    if (!reqs[requests[i]]->isPersistent())
      return ampiErrhandler("MPI_Startall", MPI_ERR_REQUEST);
#endif
    reqs[requests[i]]->start(requests[i]);
  }
  return MPI_SUCCESS;
}

void IReq::start(MPI_Request reqIdx) noexcept {
  CkAssert(persistent);
  complete = false;
  ampi* ptr = getAmpiInstance(comm);
  AmpiMsg* msg = ptr->unexpectedMsgs.get(tag, src);
  if (msg) { // if msg has already arrived, do the receive right away
    receive(ptr, msg);
  }
  else { // ... otherwise post the receive
    ptr->postedReqs.put(this);
  }
}

void SendReq::start(MPI_Request reqIdx) noexcept {
  CkAssert(persistent);
  complete = false;
  ampi* ptr = getAmpiInstance(comm);
  ptr->send(tag, ptr->getRank(), buf, count, type, src /*really, the destination*/, comm, I_SEND, reqIdx);
  complete = true;
}

void SsendReq::start(MPI_Request reqIdx) noexcept {
  CkAssert(persistent);
  complete = false;
  ampi* ptr = getAmpiInstance(comm);
  ptr->send(tag, ptr->getRank(), buf, count, type, src /*really, the destination*/, comm, I_SSEND, reqIdx);
}

CMI_WARN_UNUSED_RESULT ampiParent* IReq::wait(ampiParent* parent, MPI_Status *sts, int* result/*=nullptr*/) noexcept {
  // ampi::generic() writes directly to the buffer, so the only thing we do here is wait

  while (!complete) {
    // parent is updated in case an ampi thread is migrated while waiting for a message
    parent->resumeOnRecv = true;
    parent->numBlockedReqs = 1;
    setBlocked(true);
    parent = parent->block();
    setBlocked(false);

    if (cancelled) {
      if (sts != MPI_STATUS_IGNORE) sts->MPI_CANCEL = 1;
      complete = true;
      parent->resumeOnRecv = false;
      return parent;
    }

  } // end of while
  parent->resumeOnRecv = false;

  AMPI_DEBUG("IReq::wait has resumed\n");

  if(sts!=MPI_STATUS_IGNORE) {
    AMPI_DEBUG("Setting sts->MPI_TAG to this->tag=%d in IReq::wait  this=%p\n", (int)this->tag, this);
    sts->MPI_TAG = tag;
    sts->MPI_SOURCE = src;
    sts->MPI_COMM = comm;
    sts->MPI_LENGTH = length;
    sts->MPI_CANCEL = 0;
  }

  return parent;
}

CMI_WARN_UNUSED_RESULT ampiParent* RednReq::wait(ampiParent* parent, MPI_Status *sts, int* result/*=nullptr*/) noexcept {
  // ampi::irednResult() writes directly to the buffer, so the only thing we do here is wait

  while (!complete) {
    parent->resumeOnColl = true;
    parent->numBlockedReqs = 1;
    setBlocked(true);
    parent = parent->block();
    setBlocked(false);

  }
  parent->resumeOnColl = false;

  AMPI_DEBUG("RednReq::wait has resumed\n");

  if (sts != MPI_STATUS_IGNORE) {
    sts->MPI_TAG = tag;
    sts->MPI_SOURCE = src;
    sts->MPI_COMM = comm;
    sts->MPI_CANCEL = 0;
  }

  return parent;
}

CMI_WARN_UNUSED_RESULT ampiParent* GatherReq::wait(ampiParent* parent, MPI_Status *sts, int* result/*=nullptr*/) noexcept {
  // ampi::irednResult() writes directly to the buffer, so the only thing we do here is wait

  while (!complete) {
    parent->resumeOnColl = true;
    parent->numBlockedReqs = 1;
    setBlocked(true);
    parent = parent->block();
    setBlocked(false);

  }
  parent->resumeOnColl = false;

  AMPI_DEBUG("GatherReq::wait has resumed\n");

  if (sts != MPI_STATUS_IGNORE) {
    sts->MPI_TAG = tag;
    sts->MPI_SOURCE = src;
    sts->MPI_COMM = comm;
    sts->MPI_CANCEL = 0;
  }

  return parent;
}

CMI_WARN_UNUSED_RESULT ampiParent* GathervReq::wait(ampiParent* parent, MPI_Status *sts, int* result/*=nullptr*/) noexcept {
  // ampi::irednResult writes directly to the buffer, so the only thing we do here is wait

  while (!complete) {
    parent->resumeOnColl = true;
    parent->numBlockedReqs = 1;
    setBlocked(true);
    parent = parent->block();
    setBlocked(false);

  }
  parent->resumeOnColl = false;

  AMPI_DEBUG("GathervReq::wait has resumed\n");

  if (sts != MPI_STATUS_IGNORE) {
    sts->MPI_TAG = tag;
    sts->MPI_SOURCE = src;
    sts->MPI_COMM = comm;
    sts->MPI_CANCEL = 0;
  }

  return parent;
}

CMI_WARN_UNUSED_RESULT ampiParent* SendReq::wait(ampiParent* parent, MPI_Status *sts, int* result/*=nullptr*/) noexcept {
  while (!complete) {
    parent->resumeOnRecv = true;
    parent->numBlockedReqs = 1;
    setBlocked(true);
    parent = parent->block();
    setBlocked(false);
  }
  parent->resumeOnRecv = false;
  AMPI_DEBUG("SendReq::wait has resumed\n");
  if (sts != MPI_STATUS_IGNORE) {
    sts->MPI_COMM = comm;
    sts->MPI_CANCEL = 0;
  }
  return parent;
}

CMI_WARN_UNUSED_RESULT ampiParent* SsendReq::wait(ampiParent* parent, MPI_Status *sts, int* result/*=nullptr*/) noexcept {
  while (!complete) {
    parent->resumeOnRecv = true;
    parent->numBlockedReqs = 1;
    setBlocked(true);
    parent = parent->block();
    setBlocked(false);
  }
  parent->resumeOnRecv = false;
  if (sts != MPI_STATUS_IGNORE) {
    sts->MPI_COMM = comm;
    sts->MPI_CANCEL = 0;
  }
  return parent;
}

CMI_WARN_UNUSED_RESULT ampiParent* ATAReq::wait(ampiParent* parent, MPI_Status *sts, int* result/*=nullptr*/) noexcept {
  parent = parent->waitall(reqs.size(), reqs.data());
  reqs.clear();
  complete = true;
  return parent;
}

CMI_WARN_UNUSED_RESULT ampiParent* GReq::wait(ampiParent* parent, MPI_Status *sts, int* result/*=nullptr*/) noexcept {
  MPI_Status tmpStatus;
  if (pollFn)
    (*pollFn)(extraState, (sts == MPI_STATUS_IGNORE || sts == MPI_STATUSES_IGNORE) ? &tmpStatus : sts);
  (*queryFn)(extraState, (sts == MPI_STATUS_IGNORE || sts == MPI_STATUSES_IGNORE) ? &tmpStatus : sts);
  complete = true;
  return parent;
}

AMPI_API_IMPL(int, MPI_Wait, MPI_Request *request, MPI_Status *sts)
{
  AMPI_API("AMPI_Wait", request, sts);
  ampiParent* unused = getAmpiParent()->wait(request, sts);
  return MPI_SUCCESS;
}

CMI_WARN_UNUSED_RESULT ampiParent* ampiParent::wait(MPI_Request *request, MPI_Status *sts) noexcept
{
  if(*request == MPI_REQUEST_NULL){
    clearStatus(sts);
    return MPI_SUCCESS;
  }
  checkRequest(*request);
  ampiParent* pptr = this;
  AmpiRequestList& reqs = getReqs();

#if AMPIMSGLOG
  if(msgLogRead){
    (*fromPUPer)|pupBytes;
    PUParray(*fromPUPer, (char *)reqs[*request]->buf, pupBytes);
    PUParray(*fromPUPer, (char *)sts, sizeof(MPI_Status));
    return MPI_SUCCESS;
  }
#endif


  AMPI_DEBUG("AMPI_Wait request=%d reqs[*request]=%p reqs[*request]->tag=%d &reqs=%p\n",
             *request, reqs[*request], (int)(reqs[*request]->tag), &reqs);
  CkAssert(pptr->numBlockedReqs == 0);

  int waitResult = -1;
    AmpiRequest& waitReq = *reqs[*request];
    pptr = waitReq.wait(pptr, sts, &waitResult);
  reqs = pptr->getReqs();

  CkAssert(pptr->numBlockedReqs == 0);
  AMPI_DEBUG("AMPI_Wait after calling wait, request=%d reqs[*request]=%p reqs[*request]->tag=%d\n",
             *request, reqs[*request], (int)(reqs[*request]->tag));

#if AMPIMSGLOG
  if(msgLogWrite && record_msglog(pptr->thisIndex)){
    (pptr->pupBytes) = getDDT()->getSize(reqs[*request]->type) * (reqs[*request]->count);
    (*(pptr->toPUPer))|(pptr->pupBytes);
    PUParray(*(pptr->toPUPer), (char *)(reqs[*request]->buf), (pptr->pupBytes));
    PUParray(*(pptr->toPUPer), (char *)sts, sizeof(MPI_Status));
  }
#endif


  reqs.freeNonPersReq(pptr, *request);

  return MPI_SUCCESS;
}

CMI_WARN_UNUSED_RESULT ampiParent* ampiParent::waitall(int count, MPI_Request request[], MPI_Status sts[]/*=MPI_STATUSES_IGNORE*/) noexcept
{
  if (count == 0) return this;

  ampiParent* pptr = this;
  AmpiRequestList& reqs = getReqs();
  CkAssert(numBlockedReqs == 0);

#if AMPIMSGLOG
  if(msgLogRead){
    for(int i=0;i<count;i++){
      if(request[i] == MPI_REQUEST_NULL){
        clearStatus(sts, i);
        continue;
      }
      AmpiRequest *waitReq = reqs[request[i]];
      (*fromPUPer)|pupBytes;
      PUParray(*fromPUPer, (char *)(waitReq->buf), pupBytes);
      PUParray(*fromPUPer, (char *)(&sts[i]), sizeof(MPI_Status));
    }
    return pptr;
  }
#endif

  // First check for any incomplete requests
  for (int i=0; i<count; i++) {
    if (request[i] == MPI_REQUEST_NULL) {
      clearStatus(sts, i);
      continue;
    }
    AmpiRequest& req = *reqs[request[i]];
    if (req.test()) {
      pptr = req.wait(pptr, (sts == MPI_STATUSES_IGNORE) ? MPI_STATUS_IGNORE : &sts[i]);
      req.setBlocked(false);
#if AMPIMSGLOG
      if(msgLogWrite && record_msglog(thisIndex)){
        pupBytes = getDDT()->getSize(req.type) * req.count;
        (*toPUPer)|pupBytes;
        PUParray(*toPUPer, (char *)(req.buf), pupBytes);
        PUParray(*toPUPer, (char *)(&sts[i]), sizeof(MPI_Status));
      }
#endif
      reqs.freeNonPersReq(this, request[i]);
    }
    else {
      req.setBlocked(true);
      numBlockedReqs++;
    }
  }

  // If any requests are incomplete, block until all have been completed
  if (numBlockedReqs > 0) {
    pptr = pptr->blockOnRecv();
    reqs = pptr->getReqs(); //update pointer in case of migration while suspended

    for (int i=0; i<count; i++) {
      if (request[i] == MPI_REQUEST_NULL) {
        continue;
      }
      AmpiRequest& req = *reqs[request[i]];
#if CMK_ERROR_CHECKING
      if (!req.test())
        CkAbort("In AMPI_Waitall, all requests should have completed by now!");
#endif
      pptr = req.wait(pptr, (sts == MPI_STATUSES_IGNORE) ? MPI_STATUS_IGNORE : &sts[i]);
      req.setBlocked(false);
#if AMPIMSGLOG
      if(msgLogWrite && record_msglog(pptr->thisIndex)){
        (pptr->pupBytes) = getDDT()->getSize(req.type) * req.count;
        (*(pptr->toPUPer))|(pptr->pupBytes);
        PUParray(*(pptr->toPUPer), (char *)(req.buf), pptr->pupBytes);
        PUParray(*(pptr->toPUPer), (char *)(&sts[i]), sizeof(MPI_Status));
      }
#endif
      reqs.freeNonPersReq(pptr, request[i]);
    }
  }

  CkAssert(pptr->numBlockedReqs == 0);


  return pptr;
}

AMPI_API_IMPL(int, MPI_Waitall, int count, MPI_Request request[], MPI_Status sts[])
{
  AMPI_API("AMPI_Waitall", count, request, sts);
  checkRequests(count, request);
  ampiParent* unused = getAmpiParent()->waitall(count, request, sts);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Waitany, int count, MPI_Request *request, int *idx, MPI_Status *sts)
{
  AMPI_API("AMPI_Waitany", count, request, idx, sts);

  checkRequests(count, request);
  if (count == 0) {
    *idx = MPI_UNDEFINED;
    return MPI_SUCCESS;
  }

  ampiParent* pptr = getAmpiParent();
  CkAssert(pptr->numBlockedReqs == 0);
  AmpiRequestList& reqs = pptr->getReqs();
  int nullReqs = 0;

  // First check for an already complete request
  for (int i=0; i<count; i++) {
    if (request[i] == MPI_REQUEST_NULL) {
      nullReqs++;
      continue;
    }
    AmpiRequest& req = *reqs[request[i]];
    if (req.test()) {
      pptr = req.wait(pptr, sts);
      reqs.unblockReqs(&request[0], i);
      reqs.freeNonPersReq(pptr, request[i]);
      *idx = i;
      CkAssert(pptr->numBlockedReqs == 0);
      return MPI_SUCCESS;
    }

    req.setBlocked(true);
  }

  if (nullReqs == count) {
    clearStatus(sts);
    *idx = MPI_UNDEFINED;
    CkAssert(pptr->numBlockedReqs == 0);
    return MPI_SUCCESS;
  }

  // block until one of the requests is completed
  pptr->numBlockedReqs = 1;
  pptr = pptr->blockOnRecv();
  reqs = pptr->getReqs(); // update pointer in case of migration while suspended

  for (int i=0; i<count; i++) {
    if (request[i] == MPI_REQUEST_NULL) {
      continue;
    }
    AmpiRequest& req = *reqs[request[i]];
    if (req.test()) {
      pptr = req.wait(pptr, sts);
      reqs.unblockReqs(&request[i], count-i);
      reqs.freeNonPersReq(pptr, request[i]);
      *idx = i;
      CkAssert(pptr->numBlockedReqs == 0);
      return MPI_SUCCESS;
    }

    req.setBlocked(false);
  }
#if CMK_ERROR_CHECKING
  CkAbort("In AMPI_Waitany, a request should have completed by now!");
#endif
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Waitsome, int incount, MPI_Request *array_of_requests, int *outcount,
                                 int *array_of_indices, MPI_Status *array_of_statuses)
{
  AMPI_API("AMPI_Waitsome", incount, array_of_requests, outcount, array_of_indices, array_of_statuses);

  checkRequests(incount, array_of_requests);
  if (incount == 0) {
    *outcount = MPI_UNDEFINED;
    return MPI_SUCCESS;
  }

  ampiParent* pptr = getAmpiParent();
  CkAssert(pptr->numBlockedReqs == 0);
  AmpiRequestList& reqs = pptr->getReqs();
  MPI_Status sts;
  int nullReqs = 0;
  *outcount = 0;

  for (int i=0; i<incount; i++) {
    if (array_of_requests[i] == MPI_REQUEST_NULL) {
      clearStatus(array_of_statuses, i);
      nullReqs++;
      continue;
    }
    AmpiRequest& req = *reqs[array_of_requests[i]];
    if (req.test()) {
      pptr = req.wait(pptr, &sts);
      array_of_indices[(*outcount)] = i;
      if (array_of_statuses != MPI_STATUSES_IGNORE)
        array_of_statuses[(*outcount)] = sts;
      reqs.freeNonPersReq(pptr, array_of_requests[i]);
      (*outcount)++;
    }
    else {
      req.setBlocked(true);
    }
  }

  if (*outcount > 0) {
    reqs.unblockReqs(&array_of_requests[0], incount);
    CkAssert(pptr->numBlockedReqs == 0);
    return MPI_SUCCESS;
  }
  else if (nullReqs == incount) {
    *outcount = MPI_UNDEFINED;
    CkAssert(pptr->numBlockedReqs == 0);
    return MPI_SUCCESS;
  }
  else { // block until one of the requests is completed
    pptr->numBlockedReqs = 1;
    pptr = pptr->blockOnRecv();
    reqs = pptr->getReqs(); // update pointer in case of migration while suspended

    for (int i=0; i<incount; i++) {
      if (array_of_requests[i] == MPI_REQUEST_NULL) {
        continue;
      }
      AmpiRequest& req = *reqs[array_of_requests[i]];
      if (req.test()) {
        pptr = req.wait(pptr, &sts);
        array_of_indices[(*outcount)] = i;
        if (array_of_statuses != MPI_STATUSES_IGNORE)
          array_of_statuses[(*outcount)] = sts;
        reqs.unblockReqs(&array_of_requests[i], incount-i);
        reqs.freeNonPersReq(pptr, array_of_requests[i]);
        *outcount = 1;
        CkAssert(pptr->numBlockedReqs == 0);
        return MPI_SUCCESS;
      }
      else {
        req.setBlocked(false);
      }
    }
#if CMK_ERROR_CHECKING
    CkAbort("In AMPI_Waitsome, a request should have completed by now!");
#endif
    return MPI_SUCCESS;
  }
}

bool IReq::test(MPI_Status *sts/*=MPI_STATUS_IGNORE*/) noexcept {
  if (sts != MPI_STATUS_IGNORE) {
    if (cancelled) {
      sts->MPI_CANCEL = 1;
      complete = true;
    }
    else if (complete) {
      sts->MPI_SOURCE = src;
      sts->MPI_TAG    = tag;
      sts->MPI_COMM   = comm;
      sts->MPI_LENGTH = length;
      sts->MPI_CANCEL = 0;
    }
  }
  else if (cancelled) {
    complete = true;
  }
  return complete;
}

bool RednReq::test(MPI_Status *sts/*=MPI_STATUS_IGNORE*/) noexcept {
  return complete;
}

bool GatherReq::test(MPI_Status *sts/*=MPI_STATUS_IGNORE*/) noexcept {
  return complete;
}

bool GathervReq::test(MPI_Status *sts/*=MPI_STATUS_IGNORE*/) noexcept {
  return complete;
}

bool SendReq::test(MPI_Status *sts/*=MPI_STATUS_IGNORE*/) noexcept {
  return complete;
}

bool SsendReq::test(MPI_Status *sts/*=MPI_STATUS_IGNORE*/) noexcept {
  return complete;
}

bool GReq::test(MPI_Status *sts/*=MPI_STATUS_IGNORE*/) noexcept {
  MPI_Status tmpStatus;
  if (pollFn)
    (*pollFn)(extraState, (sts == MPI_STATUS_IGNORE || sts == MPI_STATUSES_IGNORE) ? &tmpStatus : sts);
  (*queryFn)(extraState, (sts == MPI_STATUS_IGNORE || sts == MPI_STATUSES_IGNORE) ? &tmpStatus : sts);
  return complete;
}

bool ATAReq::test(MPI_Status *sts/*=MPI_STATUS_IGNORE*/) noexcept {
  ampiParent* pptr = getAmpiParent();
  AmpiRequestList& reqList = pptr->getReqs();
  int i = 0;
  while (i < reqs.size()) {
    if (reqs[i] == MPI_REQUEST_NULL) {
      std::swap(reqs[i], reqs.back());
      reqs.pop_back();
      continue;
    }
    AmpiRequest& req = *reqList[reqs[i]];
    if (req.test()) {
      pptr = req.wait(pptr, sts);
      reqList.freeNonPersReq(pptr, reqs[i]);
      std::swap(reqs[i], reqs.back());
      reqs.pop_back();
      continue;
    }
    i++;
  }
  complete = reqs.empty();
  return complete;
}

bool IReq::receive(ampi *ptr, AmpiMsg *msg, bool deleteMsg/*=true*/) noexcept
{
  if (!ptr->processAmpiMsg(msg, buf, type, count, getReqIdx())) { // Returns false if msg is an incomplete sync message
    CkpvAccess(msgPool).deleteAmpiMsg(msg);
    return false;
  }
  complete = true;
  length = msg->getLength();
  this->tag = msg->getTag(); // Although not required, we also extract tag from msg
  src = msg->getSrcRank();   // Although not required, we also extract src from msg
  comm = ptr->getComm();
  AMPI_DEBUG("Setting this->tag to %d in IReq::receive this=%p\n", tag, this);
  // in case of an inorder bcast, msg is [nokeep] and shouldn't be freed
  if (deleteMsg) {
    CkpvAccess(msgPool).deleteAmpiMsg(msg);
  }
  return true;
}

void IReq::receiveRdma(ampi *ptr, char *sbuf, int slength, int srcRank) noexcept
{
  ptr->processRdmaMsg(sbuf, slength, buf, count, type);
  complete = true;
  length = slength;
  comm = ptr->getComm();
  // ampi::genericRdma is parameter marshalled, so there is no msg to delete
}

void RednReq::receive(ampi *ptr, CkReductionMsg *msg) noexcept
{
  if (ptr->opIsCommutative(op) && ptr->getDDT()->isContig(type)) {
    ptr->processRednMsg(msg, buf, type, count);
  } else {
    MPI_User_function* func = ptr->op2User_function(op);
    ptr->processNoncommutativeRednMsg(msg, const_cast<void*>(buf), type, count, func);
  }
  complete = true;
  comm = ptr->getComm();
  // ampi::rednResult is a [nokeep] entry method, so do not delete msg
}

void GatherReq::receive(ampi *ptr, CkReductionMsg *msg) noexcept
{
  ptr->processGatherMsg(msg, buf, type, count);
  complete = true;
  comm = ptr->getComm();
  // ampi::rednResult is a [nokeep] entry method, so do not delete msg
}

void GathervReq::receive(ampi *ptr, CkReductionMsg *msg) noexcept
{
  ptr->processGathervMsg(msg, buf, type, recvCounts.data(), displs.data());
  complete = true;
  comm = ptr->getComm();
  // ampi::rednResult is a [nokeep] entry method, so do not delete msg
}

AMPI_API_IMPL(int, MPI_Request_get_status, MPI_Request request, int *flag, MPI_Status *sts)
{
  AMPI_API("AMPI_Request_get_status", request, flag, sts);
  ampiParent* pptr = getAmpiParent();
  testRequestNoFree(pptr, &request, flag, sts);
  if(*flag != 1)
    pptr = pptr->yield();
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Test, MPI_Request *request, int *flag, MPI_Status *sts)
{
  AMPI_API("AMPI_Test", request, flag, sts);
  ampiParent* pptr = getAmpiParent();
  testRequest(pptr, request, flag, sts);
  if(*flag != 1)
    pptr = pptr->yield();
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Testany, int count, MPI_Request *request, int *index, int *flag, MPI_Status *sts)
{
  AMPI_API("AMPI_Testany", count, request, index, flag, sts);

  checkRequests(count, request);

  if (count == 0) {
    *flag = 1;
    *index = MPI_UNDEFINED;
    clearStatus(sts);
    return MPI_SUCCESS;
  }

  ampiParent* pptr = getAmpiParent();
  int nullReqs = 0;
  *flag = 0;

  for (int i=0; i<count; i++) {
    if (request[i] == MPI_REQUEST_NULL) {
      nullReqs++;
      continue;
    }
    testRequest(pptr, &request[i], flag, sts);
    if (*flag) {
      *index = i;
      return MPI_SUCCESS;
    }
  }

  *index = MPI_UNDEFINED;
  if (nullReqs == count) {
    *flag = 1;
    clearStatus(sts);
  }
  else {
    pptr = pptr->yield();
  }

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Testall, int count, MPI_Request *request, int *flag, MPI_Status *sts)
{
  AMPI_API("AMPI_Testall", count, request, flag, sts);

  checkRequests(count, request);
  if (count == 0) {
    *flag = 1;
    return MPI_SUCCESS;
  }

  ampiParent* pptr = getAmpiParent();
  AmpiRequestList& reqs = pptr->getReqs();
  int nullReqs = 0;
  *flag = 1;

  for (int i=0; i<count; i++) {
    if (request[i] == MPI_REQUEST_NULL) {
      clearStatus(sts, i);
      nullReqs++;
      continue;
    }
    if (!reqs[request[i]]->test()) {
      *flag = 0;
      pptr = pptr->yield();
      return MPI_SUCCESS;
    }
  }

  if (nullReqs != count) {
    for (int i=0; i<count; i++) {
      int reqIdx = request[i];
      if (reqIdx != MPI_REQUEST_NULL) {
        AmpiRequest& req = *reqs[reqIdx];
        pptr = req.wait(pptr, (sts == MPI_STATUSES_IGNORE) ? MPI_STATUS_IGNORE : &sts[i]);
        reqs.freeNonPersReq(pptr, request[i]);
      }
    }
  }

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Testsome, int incount, MPI_Request *array_of_requests, int *outcount,
                                 int *array_of_indices, MPI_Status *array_of_statuses)
{
  AMPI_API("AMPI_Testsome", incount, array_of_requests, outcount, array_of_indices, array_of_statuses);

  checkRequests(incount, array_of_requests);
  if (incount == 0) {
    *outcount = MPI_UNDEFINED;
    return MPI_SUCCESS;
  }

  ampiParent* pptr = getAmpiParent();
  MPI_Status sts;
  int flag = 0, nullReqs = 0;
  *outcount = 0;

  for (int i=0; i<incount; i++) {
    if (array_of_requests[i] == MPI_REQUEST_NULL) {
      clearStatus(array_of_statuses, i);
      nullReqs++;
      continue;
    }
    testRequest(pptr, &array_of_requests[i], &flag, &sts);
    if (flag) {
      array_of_indices[(*outcount)] = i;
      if (array_of_statuses != MPI_STATUSES_IGNORE)
        array_of_statuses[(*outcount)] = sts;
      (*outcount)++;
    }
  }

  if (nullReqs == incount) {
    *outcount = MPI_UNDEFINED;
  }
  else if (*outcount == 0) {
    pptr = pptr->yield();
  }

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Request_free, MPI_Request *request)
{
  AMPI_API("AMPI_Request_free", request, *request);
  if(*request==MPI_REQUEST_NULL) return MPI_SUCCESS;
  checkRequest(*request);
  ampiParent* pptr = getAmpiParent();
  AmpiRequestList& reqs = pptr->getReqs();
  reqs.free(*request, pptr->getDDT());
  *request = MPI_REQUEST_NULL;
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Grequest_start, MPI_Grequest_query_function *query_fn, MPI_Grequest_free_function *free_fn,
                                       MPI_Grequest_cancel_function *cancel_fn, void *extra_state, MPI_Request *request)
{
  AMPI_API("AMPI_Grequest_start", query_fn, free_fn, cancel_fn, extra_state, request);

  ampi* ptr = getAmpiInstance(MPI_COMM_SELF); // All GReq's are posted to MPI_COMM_SELF
  GReq *newreq = new GReq(query_fn, free_fn, cancel_fn, extra_state);
  *request = ptr->postReq(newreq);

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Grequest_complete, MPI_Request request)
{
  AMPI_API("AMPI_Grequest_complete", request);

#if AMPI_ERROR_CHECKING
  if (request == MPI_REQUEST_NULL) {
    return ampiErrhandler("AMPI_Grequest_complete", MPI_ERR_REQUEST);
  }
  if (getReqs()[request]->getType() != AMPI_G_REQ) {
    return ampiErrhandler("AMPI_Grequest_complete", MPI_ERR_REQUEST);
  }
#endif

  ampiParent* parent = getAmpiParent();
  AmpiRequestList& reqs = parent->getReqs();
  reqs[request]->complete = true;

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Cancel, MPI_Request *request)
{
  AMPI_API("AMPI_Cancel", request);
  if(*request == MPI_REQUEST_NULL) return MPI_SUCCESS;
  checkRequest(*request);
  AmpiRequestList& reqs = getReqs();
  AmpiRequest& req = *reqs[*request];
  if(req.getType() == AMPI_I_REQ || req.getType() == AMPI_G_REQ) {
    req.cancel();
    return MPI_SUCCESS;
  }
  else {
    return ampiErrhandler("AMPI_Cancel", MPI_ERR_REQUEST);
  }
}

AMPI_API_IMPL(int, MPI_Test_cancelled, const MPI_Status* status, int* flag)
{
  AMPI_API("AMPI_Test_cancelled", status, flag);
  // NOTE : current implementation requires AMPI_{Wait,Test}{any,some,all}
  // to be invoked before AMPI_Test_cancelled
  *flag = status->MPI_CANCEL;
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Status_set_cancelled, MPI_Status *status, int flag)
{
  AMPI_API("AMPI_Status_set_cancelled", status, flag);
  status->MPI_CANCEL = flag;
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Status_c2f, const MPI_Status *c_status, MPI_Fint *f_status)
{
  AMPI_API("AMPI_Status_c2f", c_status, f_status);
  if (c_status == MPI_STATUS_IGNORE || c_status == MPI_STATUSES_IGNORE) {
    return MPI_ERR_OTHER;
  }

  *(MPI_Status *)f_status = *c_status;
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Status_f2c, const MPI_Fint *f_status, MPI_Status *c_status)
{
  AMPI_API("AMPI_Status_f2c", f_status, c_status);
  // FIXME: Currently, AMPI does not have MPI_F_STATUS_IGNORE or MPI_F_STATUSES_IGNORE
  /* if (f_status == MPI_F_STATUS_IGNORE || c_status == MPI_F_STATUSES_IGNORE) {
    return MPI_ERR_OTHER;
  }*/

  *c_status = *(MPI_Status *) f_status;
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Recv_init, void *buf, int count, MPI_Datatype type, int src,
                                  int tag, MPI_Comm comm, MPI_Request *req)
{
  AMPI_API("AMPI_Recv_init", buf, count, type, src, tag, comm, req);

  handle_MPI_BOTTOM(buf, type);

#if AMPI_ERROR_CHECKING
  int ret = errorCheck("AMPI_Recv_init", comm, 1, count, 1, type, 1, tag, 1, src, 1, buf, 1);
  if(ret != MPI_SUCCESS){
    *req = MPI_REQUEST_NULL;
    return ret;
  }
#endif

  IReq* ireq = getAmpiParent()->reqPool.newReq<IReq>(buf,count,type,src,tag,comm,getDDT());
  ireq->setPersistent(true);
  *req = getAmpiInstance(comm)->postReq(ireq);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Send_init, const void *buf, int count, MPI_Datatype type, int dest,
                                  int tag, MPI_Comm comm, MPI_Request *req)
{
  AMPI_API("AMPI_Send_init", buf, count, type, dest, tag, comm, req);

  handle_MPI_BOTTOM((void*&)buf, type);

#if AMPI_ERROR_CHECKING
  int ret = errorCheck("AMPI_Send_init", comm, 1, count, 1, type, 1, tag, 1, dest, 1, buf, 1);
  if(ret != MPI_SUCCESS){
    *req = MPI_REQUEST_NULL;
    return ret;
  }
#endif

  SendReq* sreq = getAmpiParent()->reqPool.newReq<SendReq>((void*)buf, count, type, dest, tag, comm, getDDT());
  sreq->setPersistent(true);
  *req = getAmpiInstance(comm)->postReq(sreq);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Rsend_init, const void *buf, int count, MPI_Datatype type, int dest,
                                   int tag, MPI_Comm comm, MPI_Request *req)
{
  AMPI_API("AMPI_Rsend_init", buf, count, type, dest, tag, comm, req);
  return MPI_Send_init(buf, count, type, dest, tag, comm, req);
}

AMPI_API_IMPL(int, MPI_Bsend_init, const void *buf, int count, MPI_Datatype type, int dest,
                                   int tag, MPI_Comm comm, MPI_Request *req)
{
  AMPI_API("AMPI_Bsend_init", buf, count, type, dest, tag, comm, req);
  return MPI_Send_init(buf, count, type, dest, tag, comm, req);
}

AMPI_API_IMPL(int, MPI_Ssend_init, const void *buf, int count, MPI_Datatype type, int dest,
                                   int tag, MPI_Comm comm, MPI_Request *req)
{
  AMPI_API("AMPI_Ssend_init", buf, count, type, dest, tag, comm, req);

  handle_MPI_BOTTOM((void*&)buf, type);

#if AMPI_ERROR_CHECKING
  int ret = errorCheck("AMPI_Ssend_init", comm, 1, count, 1, type, 1, tag, 1, dest, 1, buf, 1);
  if(ret != MPI_SUCCESS){
    *req = MPI_REQUEST_NULL;
    return ret;
  }
#endif

  ampi* ptr = getAmpiInstance(comm);
  SsendReq* sreq = getAmpiParent()->reqPool.newReq<SsendReq>((void*)buf, count, type, dest, tag, comm, ptr->getRank(), getDDT());
  sreq->setPersistent(true);
  *req = ptr->postReq(sreq);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Type_contiguous, int count, MPI_Datatype oldtype, MPI_Datatype *newtype)
{
  AMPI_API("AMPI_Type_contiguous", count, oldtype, newtype);

#if AMPI_ERROR_CHECKING
  int ret = checkData("MPI_Type_contiguous", oldtype);
  if (ret!=MPI_SUCCESS)
    return ret;
#endif

  getDDT()->newContiguous(count, oldtype, newtype);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Type_vector, int count, int blocklength, int stride,
                                    MPI_Datatype oldtype, MPI_Datatype* newtype)
{
  AMPI_API("AMPI_Type_vector", count, blocklength, stride, oldtype, newtype);

#if AMPI_ERROR_CHECKING
  int ret = checkData("AMPI_Type_vector", oldtype);
  if (ret!=MPI_SUCCESS)
    return ret;
#endif

  getDDT()->newVector(count, blocklength, stride, oldtype, newtype);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Type_create_hvector, int count, int blocklength, MPI_Aint stride,
                                            MPI_Datatype oldtype, MPI_Datatype* newtype)
{
  AMPI_API("AMPI_Type_create_hvector", count, blocklength, stride, oldtype, newtype);

#if AMPI_ERROR_CHECKING
  int ret = checkData("AMPI_Type_create_hvector", oldtype);
  if (ret!=MPI_SUCCESS)
    return ret;
#endif

  getDDT()->newHVector(count, blocklength, stride, oldtype, newtype);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Type_hvector, int count, int blocklength, MPI_Aint stride,
                                     MPI_Datatype oldtype, MPI_Datatype* newtype)
{
  AMPI_API("AMPI_Type_hvector", count, blocklength, stride, oldtype, newtype);

#if AMPI_ERROR_CHECKING
  int ret = checkData("AMPI_Type_hvector", oldtype);
  if (ret!=MPI_SUCCESS)
    return ret;
#endif

  return MPI_Type_create_hvector(count, blocklength, stride, oldtype, newtype);
}

AMPI_API_IMPL(int, MPI_Type_indexed, int count, const int* arrBlength, const int* arrDisp,
                                     MPI_Datatype oldtype, MPI_Datatype* newtype)
{
  AMPI_API("AMPI_Type_indexed", count, arrBlength, arrDisp, oldtype, newtype);

#if AMPI_ERROR_CHECKING
  int ret = checkData("AMPI_Type_indexed", oldtype);
  if (ret!=MPI_SUCCESS)
    return ret;
#endif

  /*CkDDT_Indexed's arrDisp has type MPI_Aint* (not int*). */
  std::vector<MPI_Aint> arrDispAint(count);
  for(int i=0; i<count; i++)
    arrDispAint[i] = (MPI_Aint)(arrDisp[i]);
  getDDT()->newIndexed(count, arrBlength, arrDispAint.data(), oldtype, newtype);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Type_create_hindexed, int count, const int* arrBlength, const MPI_Aint* arrDisp,
                                             MPI_Datatype oldtype, MPI_Datatype* newtype)
{
  AMPI_API("AMPI_Type_create_hindexed", count, arrBlength, arrDisp, oldtype, newtype);

#if AMPI_ERROR_CHECKING
  int ret = checkData("AMPI_Type_create_hindexed", oldtype);
  if (ret!=MPI_SUCCESS)
    return ret;
#endif

  getDDT()->newHIndexed(count, arrBlength, arrDisp, oldtype, newtype);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Type_hindexed, int count, int* arrBlength, MPI_Aint* arrDisp,
                                      MPI_Datatype oldtype, MPI_Datatype* newtype)
{
  AMPI_API("AMPI_Type_hindexed", count, arrBlength, arrDisp, oldtype, newtype);

#if AMPI_ERROR_CHECKING
  int ret = checkData("AMPI_Type_hindexed", oldtype);
  if (ret!=MPI_SUCCESS)
    return ret;
#endif

  return MPI_Type_create_hindexed(count, arrBlength, arrDisp, oldtype, newtype);
}

AMPI_API_IMPL(int, MPI_Type_create_indexed_block, int count, int Blength, const int *arr,
                                                  MPI_Datatype oldtype, MPI_Datatype *newtype)
{
  AMPI_API("AMPI_Type_create_indexed_block", count, Blength, arr, oldtype, newtype);

#if AMPI_ERROR_CHECKING
  int ret = checkData("AMPI_Type_create_indexed_block", oldtype);
  if (ret!=MPI_SUCCESS)
    return ret;
#endif

  getDDT()->newIndexedBlock(count,Blength, arr, oldtype, newtype);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Type_create_hindexed_block, int count, int Blength, const MPI_Aint *arr,
                                                   MPI_Datatype oldtype, MPI_Datatype *newtype)
{
  AMPI_API("AMPI_Type_create_hindexed_block", count, Blength, arr, oldtype, newtype);

#if AMPI_ERROR_CHECKING
  int ret = checkData("AMPI_Type_create_hindexed_block", oldtype);
  if (ret!=MPI_SUCCESS)
    return ret;
#endif

  getDDT()->newHIndexedBlock(count,Blength, arr, oldtype, newtype);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Type_create_struct, int count, const int* arrBlength, const MPI_Aint* arrDisp,
                                           const MPI_Datatype* oldtype, MPI_Datatype*  newtype)
{
  AMPI_API("AMPI_Type_create_struct", count, arrBlength, arrDisp, oldtype, newtype);
  getDDT()->newStruct(count, arrBlength, arrDisp, oldtype, newtype);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Type_struct, int count, int* arrBlength, MPI_Aint* arrDisp,
                                    MPI_Datatype* oldtype, MPI_Datatype* newtype)
{
  AMPI_API("AMPI_Type_struct", count, arrBlength, arrDisp, oldtype, newtype);
  return MPI_Type_create_struct(count, arrBlength, arrDisp, oldtype, newtype);
}

AMPI_API_IMPL(int, MPI_Type_commit, MPI_Datatype *datatype)
{
  AMPI_API("AMPI_Type_commit", datatype);

#if AMPI_ERROR_CHECKING
  int ret = checkData("MPI_Type_commit", *datatype);
  if (ret!=MPI_SUCCESS)
    return ret;
#endif

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Type_free, MPI_Datatype *datatype)
{
  AMPI_API("AMPI_Type_free", datatype, *datatype);

  int ret;

#if AMPI_ERROR_CHECKING
  ret = checkData("AMPI_Type_free", *datatype);
  if (ret!=MPI_SUCCESS)
    return ret;

  if (datatype == nullptr) {
    return ampiErrhandler("AMPI_Type_free", MPI_ERR_ARG);
  } else if (*datatype <= AMPI_MAX_PREDEFINED_TYPE) {
    return ampiErrhandler("AMPI_Type_free", MPI_ERR_TYPE);
  }
#endif

  ampiParent* parent = getAmpiParent();
  CkDDT * ddt = parent->getDDT();
  ret = parent->freeUserAttributes(*datatype, ddt->getType(*datatype)->getAttributes());
  if (ret != MPI_SUCCESS)
    return ret;

  ddt->freeType(*datatype);
  *datatype = MPI_DATATYPE_NULL;
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Type_get_extent, MPI_Datatype datatype, MPI_Aint *lb, MPI_Aint *extent)
{
  AMPI_API("AMPI_Type_get_extent", datatype, lb, extent);

#if AMPI_ERROR_CHECKING
  int ret = checkData("AMPI_Type_get_extent", datatype);
  if (ret!=MPI_SUCCESS)
    return(ret);
#endif

  *lb = getDDT()->getLB(datatype);
  *extent = getDDT()->getExtent(datatype);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Type_get_extent_x, MPI_Datatype datatype, MPI_Count *lb, MPI_Count *extent)
{
  AMPI_API("AMPI_Type_get_extent_x", datatype, lb, extent);

#if AMPI_ERROR_CHECKING
  int ret = checkData("AMPI_Type_get_extent_x", datatype);
  if (ret!=MPI_SUCCESS)
    return(ret);
#endif

  *lb = getDDT()->getLB(datatype);
  *extent = getDDT()->getExtent(datatype);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Type_extent, MPI_Datatype datatype, MPI_Aint *extent)
{
  AMPI_API("AMPI_Type_extent", datatype, extent);

#if AMPI_ERROR_CHECKING
  int ret = checkData("AMPI_Type_extent", datatype);
  if (ret!=MPI_SUCCESS)
    return ret;
#endif

  MPI_Aint tmpLB;
  return MPI_Type_get_extent(datatype, &tmpLB, extent);
}

AMPI_API_IMPL(int, MPI_Type_get_true_extent, MPI_Datatype datatype, MPI_Aint *true_lb, MPI_Aint *true_extent)
{
  AMPI_API("AMPI_Type_get_true_extent", datatype, true_lb, true_extent);

#if AMPI_ERROR_CHECKING
  int ret = checkData("AMPI_Type_get_true_extent", datatype);
  if (ret!=MPI_SUCCESS)
    return(ret);
#endif

  *true_lb = getDDT()->getTrueLB(datatype);
  *true_extent = getDDT()->getTrueExtent(datatype);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Type_get_true_extent_x, MPI_Datatype datatype, MPI_Count *true_lb, MPI_Count *true_extent)
{
  AMPI_API("AMPI_Type_get_true_extent_x", datatype, true_lb, true_extent);

#if AMPI_ERROR_CHECKING
  int ret = checkData("AMPI_Type_get_true_extent_x", datatype);
  if (ret!=MPI_SUCCESS)
    return(ret);
#endif

  *true_lb = getDDT()->getTrueLB(datatype);
  *true_extent = getDDT()->getTrueExtent(datatype);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Type_size, MPI_Datatype datatype, int *size)
{
  AMPI_API("AMPI_Type_size", datatype, size);

#if AMPI_ERROR_CHECKING
  int ret = checkData("AMPI_Type_size", datatype);
  if (ret!=MPI_SUCCESS)
    return ret;
#endif

  *size=getDDT()->getSize(datatype);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Type_size_x, MPI_Datatype datatype, MPI_Count *size)
{
  AMPI_API("AMPI_Type_size_x", datatype, size);

#if AMPI_ERROR_CHECKING
  int ret = checkData("AMPI_Type_size_x", datatype);
  if (ret!=MPI_SUCCESS)
    return ret;
#endif

  *size=getDDT()->getSize(datatype);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Type_set_name, MPI_Datatype datatype, const char *name)
{
  AMPI_API("AMPI_Type_set_name", datatype, name);

#if AMPI_ERROR_CHECKING
  int ret = checkData("MPI_Type_set_name", datatype);
  if (ret!=MPI_SUCCESS)
    return ret;
#endif

  getDDT()->setName(datatype, name);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Type_get_name, MPI_Datatype datatype, char *name, int *resultlen)
{
  AMPI_API("AMPI_Type_get_name", datatype, name, resultlen);

#if AMPI_ERROR_CHECKING
  int ret = checkData("AMPI_Type_get_name", datatype);
  if (ret!=MPI_SUCCESS)
    return ret;
#endif

  getDDT()->getName(datatype, name, resultlen);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Type_create_resized, MPI_Datatype oldtype, MPI_Aint lb,
                                            MPI_Aint extent, MPI_Datatype *newtype)
{
  AMPI_API("AMPI_Type_create_resized", oldtype, lb, extent, newtype);

#if AMPI_ERROR_CHECKING
  int ret = checkData("AMPI_Type_create_resized", oldtype);
  if (ret!=MPI_SUCCESS)
    return ret;
#endif

  getDDT()->createResized(oldtype, lb, extent, newtype);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Type_dup, MPI_Datatype oldtype, MPI_Datatype *newtype)
{
  AMPI_API("AMPI_Type_dup", oldtype, newtype);

#if AMPI_ERROR_CHECKING
  int ret = checkData("AMPI_Type_dup", oldtype);
  if (ret!=MPI_SUCCESS)
    return ret;
#endif

  ampiParent * parent = getAmpiParent();
  CkDDT * ddt = parent->getDDT();
  ddt->createDup(oldtype, newtype);

  auto & old_attr = ddt->getType(oldtype)->getAttributes();
  auto & new_attr = ddt->getType(*newtype)->getAttributes();
  return parent->dupUserAttributes(oldtype, old_attr, new_attr);
}

AMPI_API_IMPL(int, MPI_Type_match_size, int typeclass, int size, MPI_Datatype *rtype)
{
  AMPI_API("AMPI_Type_match_size", typeclass, size, rtype);

  switch(typeclass) {
    case MPI_TYPECLASS_INTEGER: switch(size) {
      case 1: *rtype = MPI_INTEGER1; return MPI_SUCCESS;
      case 2: *rtype = MPI_INTEGER2; return MPI_SUCCESS;
      case 4: *rtype = MPI_INTEGER4; return MPI_SUCCESS;
      case 8: *rtype = MPI_INTEGER8; return MPI_SUCCESS;
      default: return MPI_ERR_ARG;
    }
    case MPI_TYPECLASS_REAL: switch(size) {
      case 4:  *rtype = MPI_REAL4; return MPI_SUCCESS;
      case 8:  *rtype = MPI_REAL8; return MPI_SUCCESS;
      case 16: *rtype = MPI_REAL16; return MPI_SUCCESS;
      default: return MPI_ERR_ARG;
    }
    case MPI_TYPECLASS_COMPLEX: switch(size) {
      case 8:  *rtype = MPI_COMPLEX8; return MPI_SUCCESS;
      case 16: *rtype = MPI_COMPLEX16; return MPI_SUCCESS;
      case 32: *rtype = MPI_COMPLEX32; return MPI_SUCCESS;
      default: return MPI_ERR_ARG;
    }
    default: 
      return MPI_ERR_ARG;
  }
}

AMPI_API_IMPL(int, MPI_Type_set_attr, MPI_Datatype datatype, int keyval, void *attribute_val)
{
  AMPI_API("AMPI_Type_set_attr", datatype, keyval, attribute_val);

#if AMPI_ERROR_CHECKING
  int ret = checkData("AMPI_Type_set_attr", datatype);
  if (ret!=MPI_SUCCESS)
    return ret;
#endif

  ampiParent *parent = getAmpiParent();
  auto & attributes = parent->getDDT()->getType(datatype)->getAttributes();
  int err = parent->setAttrType(datatype, attributes, keyval, attribute_val);
  return ampiErrhandler("AMPI_Type_set_attr", err);
}

AMPI_API_IMPL(int, MPI_Type_get_attr, MPI_Datatype datatype, int keyval,
                                      void *attribute_val, int *flag)
{
  AMPI_API("AMPI_Type_get_attr", datatype, keyval, attribute_val, flag);

#if AMPI_ERROR_CHECKING
  int ret = checkData("AMPI_Type_get_attr", datatype);
  if (ret!=MPI_SUCCESS)
    return ret;
#endif

  ampiParent *parent = getAmpiParent();
  auto & attributes = parent->getDDT()->getType(datatype)->getAttributes();
  int err = parent->getAttrType(datatype, attributes, keyval, attribute_val, flag);
  return ampiErrhandler("AMPI_Type_get_attr", err);
}

AMPI_API_IMPL(int, MPI_Type_delete_attr, MPI_Datatype datatype, int keyval)
{
  AMPI_API("AMPI_Type_delete_attr", datatype, keyval);

#if AMPI_ERROR_CHECKING
  int ret = checkData("AMPI_Type_delete_attr", datatype);
  if (ret!=MPI_SUCCESS)
    return ret;
#endif

  ampiParent *parent = getAmpiParent();
  auto & attributes = parent->getDDT()->getType(datatype)->getAttributes();
  int err = parent->deleteAttr(datatype, attributes, keyval);
  return ampiErrhandler("AMPI_Type_delete_attr", err);
}

AMPI_API_IMPL(int, MPI_Type_create_keyval, MPI_Type_copy_attr_function *copy_fn,
                                           MPI_Type_delete_attr_function *delete_fn,
                                           int *keyval, void *extra_state)
{
  AMPI_API("AMPI_Type_create_keyval", copy_fn, delete_fn, keyval, extra_state);
  return MPI_Comm_create_keyval(copy_fn, delete_fn, keyval, extra_state);
}

AMPI_API_IMPL(int, MPI_Type_free_keyval, int *keyval)
{
  AMPI_API("AMPI_Type_free_keyval", keyval, *keyval);
  return MPI_Comm_free_keyval(keyval);
}

static int MPIOI_Type_block(const int array_of_gsizes[], int dim, int ndims, int nprocs,
         int rank, int darg, int order, MPI_Aint orig_extent,
         MPI_Datatype type_old, MPI_Datatype *type_new,
         MPI_Aint *st_offset)
{
  /* nprocs = no. of processes in dimension dim of grid
     rank = coordinate of this process in dimension dim */
  int blksize, global_size, mysize, i, j;
  MPI_Aint stride;

  global_size = array_of_gsizes[dim];

  if (darg == MPI_DISTRIBUTE_DFLT_DARG)
    blksize = (global_size + nprocs - 1)/nprocs;
  else {
    blksize = darg;

    /* --BEGIN ERROR HANDLING-- */
    if (blksize <= 0) {
        return MPI_ERR_ARG;
    }

    if (blksize * nprocs < global_size) {
        return MPI_ERR_ARG;
    }
    /* --END ERROR HANDLING-- */
  }

  j = global_size - blksize*rank;
  mysize = std::min(blksize, j);
  if (mysize < 0) mysize = 0;

  stride = orig_extent;
  if (order == MPI_ORDER_FORTRAN) {
    if (dim == 0)
      MPI_Type_contiguous(mysize, type_old, type_new);
    else {
      for (i=0; i<dim; i++) stride *= (MPI_Aint)array_of_gsizes[i];
      MPI_Type_hvector(mysize, 1, stride, type_old, type_new);
    }
  }
  else {
    if (dim == ndims-1)
      MPI_Type_contiguous(mysize, type_old, type_new);
    else {
      for (i=ndims-1; i>dim; i--) stride *= (MPI_Aint)array_of_gsizes[i];
      MPI_Type_hvector(mysize, 1, stride, type_old, type_new);
    }

  }

  *st_offset = (MPI_Aint)blksize * (MPI_Aint)rank;
   /* in terms of no. of elements of type oldtype in this dimension */
  if (mysize == 0) *st_offset = 0;

  return MPI_SUCCESS;
}


/* Returns MPI_SUCCESS on success, an MPI error code on failure.  Code above
 * needs to call MPIO_Err_return_xxx.
 */
static int MPIOI_Type_cyclic(const int array_of_gsizes[], int dim, int ndims, int nprocs,
          int rank, int darg, int order, MPI_Aint orig_extent,
          MPI_Datatype type_old, MPI_Datatype *type_new,
          MPI_Aint *st_offset)
{
  /* nprocs = no. of processes in dimension dim of grid
     rank = coordinate of this process in dimension dim */
  int blksize, i, blklens[3], st_index, end_index, local_size, rem, count;
  MPI_Aint stride, disps[3];
  MPI_Datatype type_tmp, types[3];

  if (darg == MPI_DISTRIBUTE_DFLT_DARG) blksize = 1;
  else blksize = darg;

  /* --BEGIN ERROR HANDLING-- */
  if (blksize <= 0) {
    return MPI_ERR_ARG;
  }
  /* --END ERROR HANDLING-- */

  st_index = rank*blksize;
  end_index = array_of_gsizes[dim] - 1;

  if (end_index < st_index) local_size = 0;
  else {
    local_size = ((end_index - st_index + 1)/(nprocs*blksize))*blksize;
    rem = (end_index - st_index + 1) % (nprocs*blksize);
    local_size += std::min(rem, blksize);
  }

  count = local_size/blksize;
  rem = local_size % blksize;

  stride = (MPI_Aint)nprocs*(MPI_Aint)blksize*orig_extent;
  if (order == MPI_ORDER_FORTRAN)
    for (i=0; i<dim; i++) stride *= (MPI_Aint)array_of_gsizes[i];
  else for (i=ndims-1; i>dim; i--) stride *= (MPI_Aint)array_of_gsizes[i];

  MPI_Type_hvector(count, blksize, stride, type_old, type_new);

  if (rem) {
  /* if the last block is of size less than blksize, include
     it separately using MPI_Type_struct */

    types[0] = *type_new;
    types[1] = type_old;
    disps[0] = 0;
    disps[1] = (MPI_Aint)count*stride;
    blklens[0] = 1;
    blklens[1] = rem;

    MPI_Type_struct(2, blklens, disps, types, &type_tmp);

    MPI_Type_free(type_new);
    *type_new = type_tmp;
  }

  /* In the first iteration, we need to set the displacement in that
     dimension correctly. */
  if ( ((order == MPI_ORDER_FORTRAN) && (dim == 0)) ||
       ((order == MPI_ORDER_C) && (dim == ndims-1)) ) {
    types[0] = MPI_LB;
    disps[0] = 0;
    types[1] = *type_new;
    disps[1] = (MPI_Aint)rank * (MPI_Aint)blksize * orig_extent;
    types[2] = MPI_UB;
    disps[2] = orig_extent * (MPI_Aint)array_of_gsizes[dim];
    blklens[0] = blklens[1] = blklens[2] = 1;
    MPI_Type_struct(3, blklens, disps, types, &type_tmp);
    MPI_Type_free(type_new);
    *type_new = type_tmp;

    *st_offset = 0;  /* set it to 0 because it is taken care of in
                          the struct above */
  }
    else {
      *st_offset = (MPI_Aint)rank * (MPI_Aint)blksize;
      /* st_offset is in terms of no. of elements of type oldtype in
       * this dimension */
  }

  if (local_size == 0) *st_offset = 0;

  return MPI_SUCCESS;
}

// Based on implementation in mpich 3.2.1
// Please see the romio/COPYRIGHT file for licensing information.
AMPI_API_IMPL(int, MPI_Type_create_darray, int size, int rank, int ndims,
          const int array_of_gsizes[], const int array_of_distribs[],
          const int array_of_dargs[], const int array_of_psizes[],
          int order, MPI_Datatype oldtype,
          MPI_Datatype *newtype)
{
  // FIXME: do error checking
  AMPI_API("AMPI_Type_create_darray", size, rank, ndims, array_of_gsizes, array_of_distribs, array_of_dargs, array_of_psizes, order, oldtype, newtype);
  MPI_Datatype type_old, type_new=MPI_DATATYPE_NULL, types[3];
  int procs, tmp_rank, i, tmp_size, blklens[3], *coords;
  MPI_Aint *st_offsets, orig_extent, disps[3];

  MPI_Type_extent(oldtype, &orig_extent);

  /* calculate position in Cartesian grid as MPI would (row-major
     ordering) */
  coords = (int *) malloc(ndims*sizeof(int));
  procs = size;
  tmp_rank = rank;
  for (i=0; i<ndims; i++) {
    procs = procs/array_of_psizes[i];
    coords[i] = tmp_rank/procs;
    tmp_rank = tmp_rank % procs;
  }

  st_offsets = (MPI_Aint *) malloc(ndims*sizeof(MPI_Aint));
  type_old = oldtype;

  if (order == MPI_ORDER_FORTRAN) {
    /* dimension 0 changes fastest */
    for (i=0; i<ndims; i++) {
      switch(array_of_distribs[i]) {
        case MPI_DISTRIBUTE_BLOCK:
          MPIOI_Type_block(array_of_gsizes, i, ndims,
               array_of_psizes[i],
               coords[i], array_of_dargs[i],
               order, orig_extent,
               type_old, &type_new,
               st_offsets+i);
          break;
        case MPI_DISTRIBUTE_CYCLIC:
          MPIOI_Type_cyclic(array_of_gsizes, i, ndims,
                array_of_psizes[i], coords[i],
                array_of_dargs[i], order,
                orig_extent, type_old,
                &type_new, st_offsets+i);
          break;
        case MPI_DISTRIBUTE_NONE:
          /* treat it as a block distribution on 1 process */
          MPIOI_Type_block(array_of_gsizes, i, ndims, 1, 0,
               MPI_DISTRIBUTE_DFLT_DARG, order,
               orig_extent,
               type_old, &type_new,
               st_offsets+i);
          break;
        }
      if (i) MPI_Type_free(&type_old);
      type_old = type_new;
    }

    /* add displacement and UB */
    disps[1] = st_offsets[0];
    tmp_size = 1;
    for (i=1; i<ndims; i++) {
      tmp_size *= array_of_gsizes[i-1];
      disps[1] += (MPI_Aint)tmp_size*st_offsets[i];
    }
    /* rest done below for both Fortran and C order */
  }

  else /* order == MPI_ORDER_C */ {
    /* dimension ndims-1 changes fastest */
    for (i=ndims-1; i>=0; i--) {
      switch(array_of_distribs[i]) {
        case MPI_DISTRIBUTE_BLOCK:
          MPIOI_Type_block(array_of_gsizes, i, ndims, array_of_psizes[i],
               coords[i], array_of_dargs[i], order,
               orig_extent, type_old, &type_new,
               st_offsets+i);
          break;
        case MPI_DISTRIBUTE_CYCLIC:
          MPIOI_Type_cyclic(array_of_gsizes, i, ndims,
                array_of_psizes[i], coords[i],
                array_of_dargs[i], order,
                orig_extent, type_old, &type_new,
                st_offsets+i);
          break;
        case MPI_DISTRIBUTE_NONE:
          /* treat it as a block distribution on 1 process */
          MPIOI_Type_block(array_of_gsizes, i, ndims, array_of_psizes[i],
                coords[i], MPI_DISTRIBUTE_DFLT_DARG, order, orig_extent,
                                 type_old, &type_new, st_offsets+i);
          break;
      }
      if (i != ndims-1) MPI_Type_free(&type_old);
      type_old = type_new;
    }

    /* add displacement and UB */
    disps[1] = st_offsets[ndims-1];
    tmp_size = 1;
    for (i=ndims-2; i>=0; i--) {
      tmp_size *= array_of_gsizes[i+1];
      disps[1] += (MPI_Aint)tmp_size*st_offsets[i];
    }
  }

  disps[1] *= orig_extent;

  disps[2] = orig_extent;
  for (i=0; i<ndims; i++) disps[2] *= (MPI_Aint)array_of_gsizes[i];

  disps[0] = 0;
  blklens[0] = blklens[1] = blklens[2] = 1;
  types[0] = MPI_LB;
  types[1] = type_new;
  types[2] = MPI_UB;

  MPI_Type_struct(3, blklens, disps, types, newtype);

  MPI_Type_free(&type_new);
  free(st_offsets);
  free(coords);
  return MPI_SUCCESS;
}

// Based on implementation in mpich 3.2.1
// Please see the romio/COPYRIGHT file for licensing information.
AMPI_API_IMPL(int, MPI_Type_create_subarray, int ndims,
              const int array_of_sizes[], const int array_of_subsizes[],
              const int array_of_starts[], int order, MPI_Datatype oldtype,
              MPI_Datatype *newtype)
{
  // FIXME: do error checking
  AMPI_API("AMPI_Type_create_subarray", ndims, array_of_sizes, array_of_subsizes, array_of_starts, order, oldtype, newtype);
  MPI_Aint extent, disps[3], size;
  int i, blklens[3];
  MPI_Datatype tmp1, tmp2, types[3];

  MPI_Type_extent(oldtype, &extent);

  if (order == MPI_ORDER_FORTRAN) {
    /* dimension 0 changes fastest */
    if (ndims == 1) {
        MPI_Type_contiguous(array_of_subsizes[0], oldtype, &tmp1);
    }
    else {
      MPI_Type_vector(array_of_subsizes[1],
          array_of_subsizes[0],
          array_of_sizes[0], oldtype, &tmp1);

      size = (MPI_Aint)array_of_sizes[0]*extent;
      for (i=2; i<ndims; i++) {
        size *= (MPI_Aint)array_of_sizes[i-1];
        MPI_Type_hvector(array_of_subsizes[i], 1, size, tmp1, &tmp2);
        MPI_Type_free(&tmp1);
        tmp1 = tmp2;
      }
    }

    /* add displacement and UB */
    disps[1] = array_of_starts[0];
    size = 1;
    for (i=1; i<ndims; i++) {
      size *= (MPI_Aint)array_of_sizes[i-1];
      disps[1] += size*(MPI_Aint)array_of_starts[i];
    }
    /* rest done below for both Fortran and C order */
  }

  else /* order == MPI_ORDER_C */ {
    /* dimension ndims-1 changes fastest */
    if (ndims == 1) {
        MPI_Type_contiguous(array_of_subsizes[0], oldtype, &tmp1);
    }
    else {
      MPI_Type_vector(array_of_subsizes[ndims-2],
          array_of_subsizes[ndims-1],
          array_of_sizes[ndims-1], oldtype, &tmp1);

      size = (MPI_Aint)array_of_sizes[ndims-1]*extent;
      for (i=ndims-3; i>=0; i--) {
        size *= (MPI_Aint)array_of_sizes[i+1];
        MPI_Type_hvector(array_of_subsizes[i], 1, size, tmp1, &tmp2);
        MPI_Type_free(&tmp1);
        tmp1 = tmp2;
      }
    }

    /* add displacement and UB */
    disps[1] = array_of_starts[ndims-1];
    size = 1;
    for (i=ndims-2; i>=0; i--) {
        size *= (MPI_Aint)array_of_sizes[i+1];
        disps[1] += size*(MPI_Aint)array_of_starts[i];
    }
  }

  disps[1] *= extent;

  disps[2] = extent;
  for (i=0; i<ndims; i++) disps[2] *= (MPI_Aint)array_of_sizes[i];

  disps[0] = 0;
  blklens[0] = blklens[1] = blklens[2] = 1;
  types[0] = MPI_LB;
  types[1] = tmp1;
  types[2] = MPI_UB;

  MPI_Type_struct(3, blklens, disps, types, newtype);

  MPI_Type_free(&tmp1);

  return MPI_SUCCESS;
}


AMPI_API_IMPL(int, MPI_Isend, const void *buf, int count, MPI_Datatype type, int dest,
                              int tag, MPI_Comm comm, MPI_Request *request)
{
  AMPI_API("AMPI_Isend", buf, count, type, dest, tag, comm, request);

  handle_MPI_BOTTOM((void*&)buf, type);

#if AMPI_ERROR_CHECKING
  int ret = errorCheck("AMPI_Isend", comm, 1, count, 1, type, 1, tag, 1, dest, 1, buf, 1);
  if(ret != MPI_SUCCESS){
    *request = MPI_REQUEST_NULL;
    return ret;
  }
#endif

#if AMPIMSGLOG
  ampiParent* pptr = getAmpiParent();
  if(msgLogRead){
    PUParray(*(pptr->fromPUPer), (char *)request, sizeof(MPI_Request));
    return MPI_SUCCESS;
  }
#endif

  ampi *ptr = getAmpiInstance(comm);
  *request = ptr->send(tag, ptr->getRank(), buf, count, type, dest, comm, I_SEND);

#if AMPIMSGLOG
  if(msgLogWrite && record_msglog(pptr->thisIndex)){
    PUParray(*(pptr->toPUPer), (char *)request, sizeof(MPI_Request));
  }
#endif

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Ibsend, const void *buf, int count, MPI_Datatype type, int dest,
                               int tag, MPI_Comm comm, MPI_Request *request)
{
  AMPI_API("AMPI_Ibsend", buf, count, type, dest, tag, comm, request);
  return MPI_Isend(buf, count, type, dest, tag, comm, request);
}

AMPI_API_IMPL(int, MPI_Irsend, const void *buf, int count, MPI_Datatype type, int dest,
                               int tag, MPI_Comm comm, MPI_Request *request)
{
  AMPI_API("AMPI_Irsend", buf, count, type, dest, tag, comm, request);
  return MPI_Isend(buf, count, type, dest, tag, comm, request);
}

void ampi::irecvBcast(void *buf, int count, MPI_Datatype type, int src,
                      MPI_Comm comm, MPI_Request *request) noexcept
{
  if (isInter()) {
    src = myComm->getIndexForRemoteRank(src);
  }
  AmpiRequestList& reqs = getReqs();
  IReq *newreq = parent->reqPool.newReq<IReq>(buf, count, type, src, MPI_BCAST_TAG, comm, getDDT());
  *request = reqs.insert(newreq);

  AmpiMsg* msg = unexpectedBcastMsgs.get(MPI_BCAST_TAG, src);
  // if msg has already arrived, do the receive right away
  if (msg) {
    newreq->receive(this, msg, false);
    delete msg; // never add bcast msgs to AmpiMsgPool because they are nokeep
  }
  else { // ... otherwise post the receive
    postedBcastReqs.put(newreq);
  }
}

void ampi::irecv(void *buf, int count, MPI_Datatype type, int src,
                 int tag, MPI_Comm comm, MPI_Request *request) noexcept
{
  if (src==MPI_PROC_NULL) {
    *request = MPI_REQUEST_NULL;
    return;
  }

  if (isInter()) {
    src = myComm->getIndexForRemoteRank(src);
  }

  AmpiRequestList& reqs = getReqs();
  IReq *newreq = parent->reqPool.newReq<IReq>(buf, count, type, src, tag, comm, getDDT());
  *request = reqs.insert(newreq);

#if AMPIMSGLOG
  ampiParent* pptr = getAmpiParent();
  if(msgLogRead){
    PUParray(*(pptr->fromPUPer), (char *)request, sizeof(MPI_Request));
    return MPI_SUCCESS;
  }
#endif

  AmpiMsg* msg = unexpectedMsgs.get(tag, src);
  if (msg) { // if msg has already arrived, do the receive right away
    newreq->receive(this, msg);
  }
  else { // ... otherwise post the receive
    postedReqs.put(newreq);
  }

#if AMPIMSGLOG
  if(msgLogWrite && record_msglog(pptr->thisIndex)){
    PUParray(*(pptr->toPUPer), (char *)request, sizeof(MPI_Request));
  }
#endif
}

AMPI_API_IMPL(int, MPI_Irecv, void *buf, int count, MPI_Datatype type, int src,
                              int tag, MPI_Comm comm, MPI_Request *request)
{
  AMPI_API("AMPI_Irecv", buf, count, type, src, tag, comm, request);

  handle_MPI_BOTTOM(buf, type);

#if AMPI_ERROR_CHECKING
  int ret = errorCheck("AMPI_Irecv", comm, 1, count, 1, type, 1, tag, 1, src, 1, buf, 1);
  if(ret != MPI_SUCCESS){
    *request = MPI_REQUEST_NULL;
    return ret;
  }
#endif

  ampi *ptr = getAmpiInstance(comm);

  ptr->irecv(buf, count, type, src, tag, comm, request);

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Ireduce, const void *sendbuf, void *recvbuf, int count,
                                MPI_Datatype type, MPI_Op op, int root,
                                MPI_Comm comm, MPI_Request *request)
{
  AMPI_API("AMPI_Ireduce", sendbuf, recvbuf, count, type, op, root, comm, request);

  handle_MPI_BOTTOM((void*&)sendbuf, type, recvbuf, type);
  handle_MPI_IN_PLACE((void*&)sendbuf, recvbuf);

#if AMPI_ERROR_CHECKING
  if(op == MPI_OP_NULL)
    return ampiErrhandler("AMPI_Ireduce", MPI_ERR_OP);
  int ret = errorCheck("AMPI_Ireduce", comm, 1, count, 1, type, 1, 0, 0, root, 1, sendbuf, 1,
                       recvbuf, getAmpiInstance(comm)->getRank() == root);
  if(ret != MPI_SUCCESS){
    *request = MPI_REQUEST_NULL;
    return ret;
  }
#endif

  ampi *ptr = getAmpiInstance(comm);
  int rank = ptr->getRank();
  int size = ptr->getSize();

  if(ptr->isInter())
    CkAbort("AMPI does not implement MPI_Ireduce for Inter-communicators!");
  if(size == 1){
    *request = ptr->postReq(new RednReq(recvbuf, count, type, comm, op, getDDT(), AMPI_REQ_COMPLETED));
    return copyDatatype(type,count,type,count,sendbuf,recvbuf);
  }

  if (rank == root){
    *request = ptr->postReq(new RednReq(recvbuf,count,type,comm,op,getDDT()));
  }
  else {
    *request = ptr->postReq(new RednReq(recvbuf,count,type,comm,op,getDDT(),AMPI_REQ_COMPLETED));
  }

  CkReductionMsg *msg=makeRednMsg(ptr->getDDT()->getType(type),sendbuf,count,type,rank,size,op);
  int rootIdx=ptr->comm2CommStruct(comm).getIndexForRank(root);
  CkCallback reduceCB(CkIndex_ampi::irednResult(0),CkArrayIndex1D(rootIdx),ptr->getProxy());
  msg->setCallback(reduceCB);
  ptr->contribute(msg);

  return MPI_SUCCESS;
}

// Gather's are done via a 2-tuple reduction consisting of (srcRank, contributionData)
static CkReductionMsg *makeGatherMsg(const void *inbuf, int count, MPI_Datatype type, int rank, int size) noexcept
{
  CkDDT_DataType* ddt = getDDT()->getType(type);
  int szdata = ddt->getSize(count);
  const int tupleSize = 2;
  CkReduction::tupleElement tupleRedn[tupleSize];

  // Contribute rank as an unsigned short int if the max rank value fits into it, otherwise as an int
  unsigned short int ushortRank;
  if (size < std::numeric_limits<unsigned short int>::max()) {
    ushortRank = static_cast<unsigned short int>(rank);
    tupleRedn[0] = CkReduction::tupleElement(sizeof(unsigned short int), &ushortRank, CkReduction::concat);
  } else {
    tupleRedn[0] = CkReduction::tupleElement(sizeof(int), &rank, CkReduction::concat);
  }

  std::vector<char> sbuf;
  if (ddt->isContig()) {
    tupleRedn[1] = CkReduction::tupleElement(szdata, (void*)inbuf, CkReduction::concat);
  } else {
    sbuf.resize(szdata);
    ddt->serialize((char*)inbuf, sbuf.data(), count, szdata, PACK);
    tupleRedn[1] = CkReduction::tupleElement(szdata, sbuf.data(), CkReduction::concat);
  }

  return CkReductionMsg::buildFromTuple(tupleRedn, tupleSize);
}

// Gatherv's are done via a 3-tuple reduction consisting of (srcRank, contributionSize, contributionData)
static CkReductionMsg *makeGathervMsg(const void *inbuf, int count, MPI_Datatype type, int rank, int size) noexcept
{
  CkDDT_DataType* ddt = getDDT()->getType(type);
  int szdata = ddt->getSize(count);
  const int tupleSize = 3;
  CkReduction::tupleElement tupleRedn[tupleSize];

  // Contribute rank as an unsigned short int if the max rank value fits into it, otherwise as an int
  unsigned short int ushortRank;
  if (size < std::numeric_limits<unsigned short int>::max()) {
    ushortRank = static_cast<unsigned short int>(rank);
    tupleRedn[0] = CkReduction::tupleElement(sizeof(unsigned short int), &ushortRank, CkReduction::concat);
  } else {
    tupleRedn[0] = CkReduction::tupleElement(sizeof(int), &rank, CkReduction::concat);
  }

  tupleRedn[1] = CkReduction::tupleElement(sizeof(int), &szdata, CkReduction::concat);

  std::vector<char> sbuf;
  if (ddt->isContig()) {
    tupleRedn[2] = CkReduction::tupleElement(szdata, (void*)inbuf, CkReduction::concat);
  } else {
    sbuf.resize(szdata);
    ddt->serialize((char*)inbuf, sbuf.data(), count, szdata, PACK);
    tupleRedn[2] = CkReduction::tupleElement(szdata, sbuf.data(), CkReduction::concat);
  }

  return CkReductionMsg::buildFromTuple(tupleRedn, tupleSize);
}

AMPI_API_IMPL(int, MPI_Allgather, const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                                  void *recvbuf, int recvcount, MPI_Datatype recvtype,
                                  MPI_Comm comm)
{
  AMPI_API("AMPI_Allgather", sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype, comm);

  ampi *ptr = getAmpiInstance(comm);
  int rank = ptr->getRank();
  int size = ptr->getSize();

  handle_MPI_BOTTOM((void*&)sendbuf, sendtype, recvbuf, recvtype);
  handle_MPI_IN_PLACE_gather((void*&)sendbuf, recvbuf, sendcount, sendtype,
                             rank*recvcount, recvcount, recvtype);

#if AMPI_ERROR_CHECKING
  int ret;
  if (sendbuf != recvbuf) {
    ret = errorCheck("AMPI_Allgather", comm, 1, sendcount, 1, sendtype, 1, 0, 0, 0, 0, sendbuf, 1);
    if(ret != MPI_SUCCESS)
      return ret;
  }
  ret = errorCheck("AMPI_Allgather", comm, 1, recvcount, 1, recvtype, 1, 0, 0, 0, 0, recvbuf, 1);
  if(ret != MPI_SUCCESS)
    return ret;
#endif

  if(ptr->isInter())
    CkAbort("AMPI does not implement MPI_Allgather for Inter-communicators!");
  if(size == 1)
    return copyDatatype(sendtype,sendcount,recvtype,recvcount,sendbuf,recvbuf);

  ptr->setBlockingReq(new GatherReq(recvbuf, recvcount, recvtype, comm, getDDT()));

  CkReductionMsg* msg = makeGatherMsg(sendbuf, sendcount, sendtype, rank, size);
  CkCallback allgatherCB(CkIndex_ampi::rednResult(0), ptr->getProxy());
  msg->setCallback(allgatherCB);
  MSG_ORDER_DEBUG(CkPrintf("[%d] AMPI_Allgather called on comm %d\n", ptr->thisIndex, comm));
  ptr->contribute(msg);

  ptr = ptr->blockOnColl();

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Iallgather, const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                                   void *recvbuf, int recvcount, MPI_Datatype recvtype,
                                   MPI_Comm comm, MPI_Request* request)
{
  AMPI_API("AMPI_Iallgather", sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype, comm, request);

  ampi *ptr = getAmpiInstance(comm);
  int rank = ptr->getRank();
  int size = ptr->getSize();

  handle_MPI_BOTTOM((void*&)sendbuf, sendtype, recvbuf, recvtype);
  handle_MPI_IN_PLACE_gather((void*&)sendbuf, recvbuf, sendcount, sendtype,
                             rank*recvcount, recvcount, recvtype);

#if AMPI_ERROR_CHECKING
  int ret;
  if (sendbuf != recvbuf) {
    ret = errorCheck("AMPI_Iallgather", comm, 1, sendcount, 1, sendtype, 1, 0, 0, 0, 0, sendbuf, 1);
    if(ret != MPI_SUCCESS){
      *request = MPI_REQUEST_NULL;
      return ret;
    }
  }
  ret = errorCheck("AMPI_Iallgather", comm, 1, recvcount, 1, recvtype, 1, 0, 0, 0, 0, recvbuf, 1);
  if(ret != MPI_SUCCESS){
    *request = MPI_REQUEST_NULL;
    return ret;
  }
#endif

  if(ptr->isInter())
    CkAbort("AMPI does not implement MPI_Iallgather for Inter-communicators!");
  if(size == 1){
    *request = ptr->postReq(new GatherReq(recvbuf, recvcount, recvtype, comm, getDDT(), AMPI_REQ_COMPLETED));
    return copyDatatype(sendtype,sendcount,recvtype,recvcount,sendbuf,recvbuf);
  }

  *request = ptr->postReq(new GatherReq(recvbuf, recvcount, recvtype, comm, getDDT()));

  CkReductionMsg* msg = makeGatherMsg(sendbuf, sendcount, sendtype, rank, size);
  CkCallback allgatherCB(CkIndex_ampi::irednResult(0), ptr->getProxy());
  msg->setCallback(allgatherCB);
  MSG_ORDER_DEBUG(CkPrintf("[%d] AMPI_Iallgather called on comm %d\n", ptr->thisIndex, comm));
  ptr->contribute(msg);

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Allgatherv, const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                                   void *recvbuf, const int *recvcounts, const int *displs,
                                   MPI_Datatype recvtype, MPI_Comm comm)
{
  AMPI_API("AMPI_Allgatherv", sendbuf, sendcount, sendtype, recvbuf, recvcounts, displs, recvtype, comm);

  ampi *ptr = getAmpiInstance(comm);
  int rank = ptr->getRank();
  int size = ptr->getSize();

  handle_MPI_BOTTOM((void*&)sendbuf, sendtype, recvbuf, recvtype);
  handle_MPI_IN_PLACE_gatherv((void*&)sendbuf, recvbuf, sendcount, sendtype,
                              displs, recvcounts, rank, recvtype);

#if AMPI_ERROR_CHECKING
  int ret;
  if (sendbuf != recvbuf) {
    ret = errorCheck("AMPI_Allgatherv", comm, 1, sendcount, 1, sendtype, 1, 0, 0, 0, 0, sendbuf, 1);
    if(ret != MPI_SUCCESS)
      return ret;
  }
  ret = errorCheck("AMPI_Allgatherv", comm, 1, recvcounts[0], 1, recvtype, 1, 0, 0, 0, 0, recvbuf, 1);
  if(ret != MPI_SUCCESS)
    return ret;
#endif

  if(ptr->isInter())
    CkAbort("AMPI does not implement MPI_Allgatherv for Inter-communicators!");
  if(size == 1)
    return copyDatatype(sendtype,sendcount,recvtype,recvcounts[0],sendbuf,recvbuf);

  ptr->setBlockingReq(new GathervReq(recvbuf, size, recvtype, comm, recvcounts, displs, getDDT()));

  CkReductionMsg* msg = makeGathervMsg(sendbuf, sendcount, sendtype, rank, size);
  CkCallback allgathervCB(CkIndex_ampi::rednResult(0), ptr->getProxy());
  msg->setCallback(allgathervCB);
  MSG_ORDER_DEBUG(CkPrintf("[%d] AMPI_Allgatherv called on comm %d\n", ptr->thisIndex, comm));
  ptr->contribute(msg);

  ptr = ptr->blockOnColl();

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Iallgatherv, const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                                    void *recvbuf, const int *recvcounts, const int *displs,
                                    MPI_Datatype recvtype, MPI_Comm comm, MPI_Request *request)
{
  AMPI_API("AMPI_Iallgatherv", sendbuf, sendcount, sendtype, recvbuf, recvcounts, displs, recvtype, comm, request);

  ampi *ptr = getAmpiInstance(comm);
  int rank = ptr->getRank();
  int size = ptr->getSize();

  handle_MPI_BOTTOM((void*&)sendbuf, sendtype, recvbuf, recvtype);
  handle_MPI_IN_PLACE_gatherv((void*&)sendbuf, recvbuf, sendcount, sendtype,
                              displs, recvcounts, rank, recvtype);

#if AMPI_ERROR_CHECKING
  int ret;
  if (sendbuf != recvbuf) {
    ret = errorCheck("AMPI_Iallgatherv", comm, 1, sendcount, 1, sendtype, 1, 0, 0, 0, 0, sendbuf, 1);
    if(ret != MPI_SUCCESS){
      *request = MPI_REQUEST_NULL;
      return ret;
    }
  }
  ret = errorCheck("AMPI_Iallgatherv", comm, 1, recvcounts[0], 1, recvtype, 1, 0, 0, 0, 0, recvbuf, 1);
  if(ret != MPI_SUCCESS){
    *request = MPI_REQUEST_NULL;
    return ret;
  }
#endif

  if(ptr->isInter())
    CkAbort("AMPI does not implement MPI_Iallgatherv for Inter-communicators!");
  if(size == 1){
    *request = ptr->postReq(new GathervReq(recvbuf, rank, recvtype, comm, recvcounts, displs,
                            getDDT(), AMPI_REQ_COMPLETED));
    return copyDatatype(sendtype,sendcount,recvtype,recvcounts[0],sendbuf,recvbuf);
  }

  *request = ptr->postReq(new GathervReq(recvbuf, size, recvtype, comm,
                                         recvcounts, displs, getDDT()));

  CkReductionMsg* msg = makeGathervMsg(sendbuf, sendcount, sendtype, rank, size);
  CkCallback allgathervCB(CkIndex_ampi::irednResult(0), ptr->getProxy());
  msg->setCallback(allgathervCB);
  MSG_ORDER_DEBUG(CkPrintf("[%d] AMPI_Iallgatherv called on comm %d\n", ptr->thisIndex, comm));
  ptr->contribute(msg);

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Gather, const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                               void *recvbuf, int recvcount, MPI_Datatype recvtype,
                               int root, MPI_Comm comm)
{
  AMPI_API("AMPI_Gather", sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype, root, comm);

  ampi *ptr = getAmpiInstance(comm);
  int rank = ptr->getRank();
  int size = ptr->getSize();

  handle_MPI_BOTTOM((void*&)sendbuf, sendtype, recvbuf, recvtype);
  handle_MPI_IN_PLACE_gather((void*&)sendbuf, recvbuf, sendcount, sendtype,
                             rank*recvcount, recvcount, recvtype);

#if AMPI_ERROR_CHECKING
  int ret;
  if (sendbuf != recvbuf) {
    ret = errorCheck("AMPI_Gather", comm, 1, sendcount, 1, sendtype, 1, 0, 0, 0, 0, sendbuf, 1);
    if(ret != MPI_SUCCESS)
      return ret;
  }
  if (getAmpiInstance(comm)->getRank() == root) {
    ret = errorCheck("AMPI_Gather", comm, 1, recvcount, 1, recvtype, 1, 0, 0, 0, 0, recvbuf, 1);
    if(ret != MPI_SUCCESS)
      return ret;
  }
#endif

  if(ptr->isInter())
    CkAbort("AMPI does not implement MPI_Gather for Inter-communicators!");
  if(size == 1)
    return copyDatatype(sendtype,sendcount,recvtype,recvcount,sendbuf,recvbuf);

#if AMPIMSGLOG
  if(msgLogRead){
    ampiParent* pptr = getAmpiParent();
    (*(pptr->fromPUPer))|(pptr->pupBytes);
    PUParray(*(pptr->fromPUPer), (char *)recvbuf, (pptr->pupBytes));
    return MPI_SUCCESS;
  }
#endif

  if (rank == root) {
    ptr->setBlockingReq(new GatherReq(recvbuf, recvcount, recvtype, comm, getDDT()));
  }

  int rootIdx = ptr->comm2CommStruct(comm).getIndexForRank(root);
  CkReductionMsg* msg = makeGatherMsg(sendbuf, sendcount, sendtype, rank, size);
  CkCallback gatherCB(CkIndex_ampi::rednResult(0), CkArrayIndex1D(rootIdx), ptr->getProxy());
  msg->setCallback(gatherCB);
  MSG_ORDER_DEBUG(CkPrintf("[%d] AMPI_Gather called on comm %d root %d \n", ptr->thisIndex, comm, rootIdx));
  ptr->contribute(msg);

  if (rank == root) {
    ptr = ptr->blockOnColl();
  }

#if AMPIMSGLOG
  if(msgLogWrite){
    ampiParent* pptr = getAmpiParent();
    if(record_msglog(pptr->thisIndex)){
      (pptr->pupBytes) = getDDT()->getSize(recvtype) * recvcount * size;
      (*(pptr->toPUPer))|(pptr->pupBytes);
      PUParray(*(pptr->toPUPer), (char *)recvbuf, (pptr->pupBytes));
    }
  }
#endif

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Igather, const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                                void *recvbuf, int recvcount, MPI_Datatype recvtype,
                                int root, MPI_Comm comm, MPI_Request *request)
{
  AMPI_API("AMPI_Igather", sendbuf, sendtype, sendtype, recvbuf, recvcount, recvtype, root, comm, request);

  ampi *ptr = getAmpiInstance(comm);
  int rank = ptr->getRank();
  int size = ptr->getSize();

  handle_MPI_BOTTOM((void*&)sendbuf, sendtype, recvbuf, recvtype);
  handle_MPI_IN_PLACE_gather((void*&)sendbuf, recvbuf, sendcount, sendtype,
                             rank*recvcount, recvcount, recvtype);

#if AMPI_ERROR_CHECKING
  int ret;
  if (sendbuf != recvbuf) {
    ret = errorCheck("AMPI_Igather", comm, 1, sendcount, 1, sendtype, 1, 0, 0, 0, 0, sendbuf, 1);
    if(ret != MPI_SUCCESS){
      *request = MPI_REQUEST_NULL;
      return ret;
    }
  }
  if (getAmpiInstance(comm)->getRank() == root) {
    ret = errorCheck("AMPI_Igather", comm, 1, recvcount, 1, recvtype, 1, 0, 0, 0, 0, recvbuf, 1);
    if(ret != MPI_SUCCESS){
      *request = MPI_REQUEST_NULL;
      return ret;
    }
  }
#endif

  if(ptr->isInter())
    CkAbort("AMPI does not implement MPI_Igather for Inter-communicators!");
  if(size == 1){
    *request = ptr->postReq(new GatherReq(recvbuf, recvcount, recvtype, comm, getDDT(), AMPI_REQ_COMPLETED));
    return copyDatatype(sendtype,sendcount,recvtype,recvcount,sendbuf,recvbuf);
  }

#if AMPIMSGLOG
  ampiParent* pptr = getAmpiParent();
  if(msgLogRead){
    (*(pptr->fromPUPer))|(pptr->pupBytes);
    PUParray(*(pptr->fromPUPer), (char *)recvbuf, (pptr->pupBytes));
    return MPI_SUCCESS;
  }
#endif

  if (rank == root) {
    *request = ptr->postReq(new GatherReq(recvbuf, recvcount, recvtype, comm, getDDT()));
  }
  else {
    *request = ptr->postReq(new GatherReq(recvbuf, recvcount, recvtype, comm, getDDT(), AMPI_REQ_COMPLETED));
  }

  int rootIdx = ptr->comm2CommStruct(comm).getIndexForRank(root);
  CkReductionMsg* msg = makeGatherMsg(sendbuf, sendcount, sendtype, rank, size);
  CkCallback gatherCB(CkIndex_ampi::irednResult(0), CkArrayIndex1D(rootIdx), ptr->getProxy());
  msg->setCallback(gatherCB);
  MSG_ORDER_DEBUG(CkPrintf("[%d] AMPI_Igather called on comm %d root %d \n", ptr->thisIndex, comm, rootIdx));
  ptr->contribute(msg);

#if AMPIMSGLOG
  if(msgLogWrite && record_msglog(pptr->thisIndex)){
    (pptr->pupBytes) = getDDT()->getSize(recvtype) * recvcount * size;
    (*(pptr->toPUPer))|(pptr->pupBytes);
    PUParray(*(pptr->toPUPer), (char *)recvbuf, (pptr->pupBytes));
  }
#endif

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Gatherv, const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                                void *recvbuf, const int *recvcounts, const int *displs,
                                MPI_Datatype recvtype, int root, MPI_Comm comm)
{
  AMPI_API("AMPI_Gatherv", sendbuf, sendtype, sendtype, recvbuf, recvcounts, displs, recvtype, root, comm);

  ampi *ptr = getAmpiInstance(comm);
  int rank = ptr->getRank();
  int size = ptr->getSize();

  handle_MPI_BOTTOM((void*&)sendbuf, sendtype, recvbuf, recvtype);
  handle_MPI_IN_PLACE_gatherv((void*&)sendbuf, recvbuf, sendcount, sendtype,
                              displs, recvcounts, rank, recvtype);

#if AMPI_ERROR_CHECKING
  int ret;
  if (sendbuf != recvbuf) {
    ret = errorCheck("AMPI_Gatherv", comm, 1, sendcount, 1, sendtype, 1, 0, 0, 0, 0, sendbuf, 1);
    if(ret != MPI_SUCCESS)
      return ret;
  }
  if (getAmpiInstance(comm)->getRank() == root) {
    ret = errorCheck("AMPI_Gatherv", comm, 1, recvcounts[0], 1, recvtype, 1, 0, 0, 0, 0, recvbuf, 1);
    if(ret != MPI_SUCCESS)
      return ret;
  }
#endif

  if(ptr->isInter())
    CkAbort("AMPI does not implement MPI_Gatherv for Inter-communicators!");
  if(size == 1)
    return copyDatatype(sendtype,sendcount,recvtype,recvcounts[0],sendbuf,recvbuf);

#if AMPIMSGLOG
  if(msgLogRead){
    ampiParent* pptr = getAmpiParent();
    int commsize;
    int itemsize = getDDT()->getSize(recvtype);
    (*(pptr->fromPUPer))|commsize;
    for(int i=0;i<commsize;i++){
      (*(pptr->fromPUPer))|(pptr->pupBytes);
      PUParray(*(pptr->fromPUPer), (char *)(((char*)recvbuf)+(itemsize*displs[i])), (pptr->pupBytes));
    }
    return MPI_SUCCESS;
  }
#endif

  if (rank == root) {
    ptr->setBlockingReq(new GathervReq(recvbuf, size, recvtype, comm, recvcounts, displs, getDDT()));
  }

  int rootIdx = ptr->comm2CommStruct(comm).getIndexForRank(root);
  CkReductionMsg* msg = makeGathervMsg(sendbuf, sendcount, sendtype, rank, size);
  CkCallback gathervCB(CkIndex_ampi::rednResult(0), CkArrayIndex1D(rootIdx), ptr->getProxy());
  msg->setCallback(gathervCB);
  MSG_ORDER_DEBUG(CkPrintf("[%d] AMPI_Gatherv called on comm %d root %d \n", ptr->thisIndex, comm, rootIdx));
  ptr->contribute(msg);

  if (rank == root) {
    ptr = ptr->blockOnColl();
  }

#if AMPIMSGLOG
  if(msgLogWrite){
    ampiParent* pptr = getAmpiParent();
    if(record_msglog(pptr->thisIndex)){
      for(int i=0;i<size;i++){
        (pptr->pupBytes) = getDDT()->getSize(recvtype) * recvcounts[i];
        (*(pptr->toPUPer))|(pptr->pupBytes);
        PUParray(*(pptr->toPUPer), (char *)(((char*)recvbuf)+(itemsize*displs[i])), (pptr->pupBytes));
      }
    }
  }
#endif

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Igatherv, const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                                 void *recvbuf, const int *recvcounts, const int *displs,
                                 MPI_Datatype recvtype, int root, MPI_Comm comm, MPI_Request *request)
{
  AMPI_API("AMPI_Igatherv", sendbuf, sendtype, sendtype, recvbuf, recvcounts, displs, recvtype, root, comm, request);

  ampi *ptr = getAmpiInstance(comm);
  int rank = ptr->getRank();
  int size = ptr->getSize();

  handle_MPI_BOTTOM((void*&)sendbuf, sendtype, recvbuf, recvtype);
  handle_MPI_IN_PLACE_gatherv((void*&)sendbuf, recvbuf, sendcount, sendtype,
                              displs, recvcounts, rank, recvtype);

#if AMPI_ERROR_CHECKING
  int ret;
  if (sendbuf != recvbuf) {
    ret = errorCheck("AMPI_Igatherv", comm, 1, sendcount, 1, sendtype, 1, 0, 0, 0, 0, sendbuf, 1);
    if(ret != MPI_SUCCESS){
      *request = MPI_REQUEST_NULL;
      return ret;
    }
  }
  if (getAmpiInstance(comm)->getRank() == root) {
    ret = errorCheck("AMPI_Igatherv", comm, 1, recvcounts[0], 1, recvtype, 1, 0, 0, 0, 0, recvbuf, 1);
    if(ret != MPI_SUCCESS){
      *request = MPI_REQUEST_NULL;
      return ret;
    }
  }
#endif

  if(ptr->isInter())
    CkAbort("AMPI does not implement MPI_Igatherv for Inter-communicators!");
  if(size == 1){
    *request = ptr->postReq(new GathervReq(recvbuf, rank, recvtype, comm, recvcounts, displs,
                            getDDT(), AMPI_REQ_COMPLETED));
    return copyDatatype(sendtype,sendcount,recvtype,recvcounts[0],sendbuf,recvbuf);
  }

#if AMPIMSGLOG
  ampiParent* pptr = getAmpiParent();
  if(msgLogRead){
    int commsize;
    int itemsize = getDDT()->getSize(recvtype);
    (*(pptr->fromPUPer))|commsize;
    for(int i=0;i<commsize;i++){
      (*(pptr->fromPUPer))|(pptr->pupBytes);
      PUParray(*(pptr->fromPUPer), (char *)(((char*)recvbuf)+(itemsize*displs[i])), (pptr->pupBytes));
    }
    return MPI_SUCCESS;
  }
#endif

  if (rank == root) {
    *request = ptr->postReq(new GathervReq(recvbuf, size, recvtype, comm,
                                           recvcounts, displs, getDDT()));
  }
  else {
    *request = ptr->postReq(new GathervReq(recvbuf, size, recvtype, comm,
                                           recvcounts, displs, getDDT(), AMPI_REQ_COMPLETED));
  }

  int rootIdx = ptr->comm2CommStruct(comm).getIndexForRank(root);
  CkReductionMsg* msg = makeGathervMsg(sendbuf, sendcount, sendtype, rank, size);
  CkCallback gathervCB(CkIndex_ampi::irednResult(0), CkArrayIndex1D(rootIdx), ptr->getProxy());
  msg->setCallback(gathervCB);
  MSG_ORDER_DEBUG(CkPrintf("[%d] AMPI_Igatherv called on comm %d root %d \n", ptr->thisIndex, comm, rootIdx));
  ptr->contribute(msg);

#if AMPIMSGLOG
  if(msgLogWrite && record_msglog(pptr->thisIndex)){
    for(int i=0;i<size;i++){
      (pptr->pupBytes) = getDDT()->getSize(recvtype) * recvcounts[i];
      (*(pptr->toPUPer))|(pptr->pupBytes);
      PUParray(*(pptr->toPUPer), (char *)(((char*)recvbuf)+(itemsize*displs[i])), (pptr->pupBytes));
    }
  }
#endif

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Scatter, const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                                void *recvbuf, int recvcount, MPI_Datatype recvtype,
                                int root, MPI_Comm comm)
{
  AMPI_API("AMPI_Scatter", sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype, root, comm);

  handle_MPI_BOTTOM((void*&)sendbuf, sendtype, recvbuf, recvtype);
  handle_MPI_IN_PLACE((void*&)sendbuf,recvbuf);

#if AMPI_ERROR_CHECKING
  int ret;
  if (getAmpiInstance(comm)->getRank() == root) {
    ret = errorCheck("AMPI_Scatter", comm, 1, sendcount, 1, sendtype, 1, 0, 0, 0, 0, sendbuf, 1);
    if(ret != MPI_SUCCESS)
      return ret;
  }
  if (sendbuf != recvbuf || getAmpiInstance(comm)->getRank() != root) {
    ret = errorCheck("AMPI_Scatter", comm, 1, recvcount, 1, recvtype, 1, 0, 0, 0, 0, recvbuf, 1);
    if(ret != MPI_SUCCESS)
      return ret;
  }
#endif

  ampi *ptr = getAmpiInstance(comm);

  if(getAmpiParent()->isInter(comm)) {
    return ptr->intercomm_scatter(root,sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,comm);
  }
  if(ptr->getSize() == 1)
    return copyDatatype(sendtype,sendcount,recvtype,recvcount,sendbuf,recvbuf);

#if AMPIMSGLOG
  ampiParent* pptr = getAmpiParent();
  if(msgLogRead){
    (*(pptr->fromPUPer))|(pptr->pupBytes);
    PUParray(*(pptr->fromPUPer), (char *)recvbuf, (pptr->pupBytes));
    return MPI_SUCCESS;
  }
#endif

  int size = ptr->getSize();
  int rank = ptr->getRank();
  int i;

  if(rank==root) {
    CkDDT_DataType* dttype = ptr->getDDT()->getType(sendtype) ;
    int itemextent = dttype->getExtent() * sendcount;
    for(i=0;i<size;i++) {
      if (i != rank) {
        ptr->send(MPI_SCATTER_TAG, rank, ((char*)sendbuf)+(itemextent*i),
                  sendcount, sendtype, i, comm);
      }
    }
    if (sendbuf != recvbuf) {
      copyDatatype(sendtype,sendcount,recvtype,recvcount,(char*)sendbuf+(itemextent*rank),recvbuf);
    }
  }
  else {
    if(-1==ptr->recv(MPI_SCATTER_TAG, root, recvbuf, recvcount, recvtype, comm))
      CkAbort("AMPI> Error in MPI_Scatter recv");
  }

#if AMPIMSGLOG
  if(msgLogWrite && record_msglog(pptr->thisIndex)){
    (pptr->pupBytes) = getDDT()->getSize(recvtype) * recvcount;
    (*(pptr->toPUPer))|(pptr->pupBytes);
    PUParray(*(pptr->toPUPer), (char *)recvbuf, (pptr->pupBytes));
  }
#endif

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Iscatter, const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                                 void *recvbuf, int recvcount, MPI_Datatype recvtype,
                                 int root, MPI_Comm comm, MPI_Request *request)
{
  AMPI_API("AMPI_Iscatter", sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype, root, comm, request);

  handle_MPI_BOTTOM((void*&)sendbuf, sendtype, recvbuf, recvtype);
  handle_MPI_IN_PLACE((void*&)sendbuf,recvbuf);

#if AMPI_ERROR_CHECKING
  int ret;
  if (getAmpiInstance(comm)->getRank() == root) {
    ret = errorCheck("AMPI_Iscatter", comm, 1, sendcount, 1, sendtype, 1, 0, 0, 0, 0, sendbuf, 1);
    if(ret != MPI_SUCCESS){
      *request = MPI_REQUEST_NULL;
      return ret;
    }
  }
  if (sendbuf != recvbuf || getAmpiInstance(comm)->getRank() != root) {
    ret = errorCheck("AMPI_Iscatter", comm, 1, recvcount, 1, recvtype, 1, 0, 0, 0, 0, recvbuf, 1);
    if(ret != MPI_SUCCESS){
      *request = MPI_REQUEST_NULL;
      return ret;
    }
  }
#endif

  ampi *ptr = getAmpiInstance(comm);

  if(getAmpiParent()->isInter(comm)) {
    return ptr->intercomm_iscatter(root,sendbuf,sendcount,sendtype,recvbuf,recvcount,recvtype,comm,request);
  }
  if(ptr->getSize() == 1){
    *request = ptr->postReq(getAmpiParent()->reqPool.newReq<IReq>(recvbuf,recvcount,recvtype,root,MPI_SCATTER_TAG,comm,
                            getDDT(), AMPI_REQ_COMPLETED));
    return copyDatatype(sendtype,sendcount,recvtype,recvcount,sendbuf,recvbuf);
  }

#if AMPIMSGLOG
  ampiParent* pptr = getAmpiParent();
  if(msgLogRead){
    (*(pptr->fromPUPer))|(pptr->pupBytes);
    PUParray(*(pptr->fromPUPer), (char *)recvbuf, (pptr->pupBytes));
    return MPI_SUCCESS;
  }
#endif

  int size = ptr->getSize();
  int rank = ptr->getRank();
  int i;

  if(rank==root) {
    CkDDT_DataType* dttype = ptr->getDDT()->getType(sendtype) ;
    int itemextent = dttype->getExtent() * sendcount;
    // use an ATAReq to non-block the caller and get a request ptr
    ATAReq *newreq = new ATAReq(size);
    for(i=0;i<size;i++) {
      if (i != rank) {
        newreq->reqs[i] = ptr->send(MPI_SCATTER_TAG, rank, (char*)sendbuf+(itemextent*i),
                                    sendcount, sendtype, i, comm, I_SEND);
      }
    }
    newreq->reqs[rank] = MPI_REQUEST_NULL;

    if (sendbuf != recvbuf) {
      copyDatatype(sendtype,sendcount,recvtype,recvcount,(char*)sendbuf+(itemextent*rank),recvbuf);
    }
    *request = ptr->postReq(newreq);
  }
  else {
    ptr->irecv(recvbuf,recvcount,recvtype,root,MPI_SCATTER_TAG,comm,request);
  }

#if AMPIMSGLOG
  if(msgLogWrite && record_msglog(pptr->thisIndex)){
    (pptr->pupBytes) = getDDT()->getSize(recvtype) * recvcount;
    (*(pptr->toPUPer))|(pptr->pupBytes);
    PUParray(*(pptr->toPUPer), (char *)recvbuf, (pptr->pupBytes));
  }
#endif

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Scatterv, const void *sendbuf, const int *sendcounts, const int *displs,
                                 MPI_Datatype sendtype, void *recvbuf, int recvcount,
                                 MPI_Datatype recvtype, int root, MPI_Comm comm)
{
  AMPI_API("AMPI_Scatterv", sendbuf, sendcounts, sendtype, recvbuf, recvcount, recvtype, root, comm);

  handle_MPI_BOTTOM((void*&)sendbuf, sendtype, recvbuf, recvtype);
  handle_MPI_IN_PLACE((void*&)sendbuf, recvbuf);

#if AMPI_ERROR_CHECKING
  int ret;
  if (getAmpiInstance(comm)->getRank() == root) {
    ret = errorCheck("AMPI_Scatterv", comm, 1, 0, 0, sendtype, 1, 0, 0, 0, 0, sendbuf, 1);
    if(ret != MPI_SUCCESS)
      return ret;
  }
  if (sendbuf != recvbuf || getAmpiInstance(comm)->getRank() != root) {
    ret = errorCheck("AMPI_Scatterv", comm, 1, recvcount, 1, recvtype, 1, 0, 0, 0, 0, recvbuf, 1);
    if(ret != MPI_SUCCESS)
      return ret;
  }
#endif

  ampi* ptr = getAmpiInstance(comm);

  if (getAmpiParent()->isInter(comm)) {
    return ptr->intercomm_scatterv(root, sendbuf, sendcounts, displs, sendtype, recvbuf, recvcount, recvtype, comm);
  }
  if(ptr->getSize() == 1)
    return copyDatatype(sendtype,sendcounts[0],recvtype,recvcount,sendbuf,recvbuf);

#if AMPIMSGLOG
  ampiParent* pptr = getAmpiParent();
  if(msgLogRead){
    (*(pptr->fromPUPer))|(pptr->pupBytes);
    PUParray(*(pptr->fromPUPer), (char *)recvbuf, (pptr->pupBytes));
    return MPI_SUCCESS;
  }
#endif

  int size = ptr->getSize();
  int rank = ptr->getRank();
  int i;

  if(rank == root) {
    CkDDT_DataType* dttype = ptr->getDDT()->getType(sendtype) ;
    int itemextent = dttype->getExtent();
    for(i=0;i<size;i++) {
      if (i != rank) {
        ptr->send(MPI_SCATTER_TAG, rank, ((char*)sendbuf)+(itemextent*displs[i]),
                  sendcounts[i], sendtype, i, comm);
      }
    }
    if (sendbuf != recvbuf) {
      copyDatatype(sendtype,sendcounts[rank],recvtype,recvcount,(char*)sendbuf+(itemextent*displs[rank]),recvbuf);
    }
  }
  else {
    if(-1==ptr->recv(MPI_SCATTER_TAG, root, recvbuf, recvcount, recvtype, comm))
      CkAbort("AMPI> Error in MPI_Scatterv recv");
  }

#if AMPIMSGLOG
  if(msgLogWrite && record_msglog(pptr->thisIndex)){
    (pptr->pupBytes) = getDDT()->getSize(recvtype) * recvcount;
    (*(pptr->toPUPer))|(pptr->pupBytes);
    PUParray(*(pptr->toPUPer), (char *)recvbuf, (pptr->pupBytes));
  }
#endif

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Iscatterv, const void *sendbuf, const int *sendcounts, const int *displs,
                                  MPI_Datatype sendtype, void *recvbuf, int recvcount,
                                  MPI_Datatype recvtype, int root, MPI_Comm comm,
                                  MPI_Request *request)
{
  AMPI_API("AMPI_Iscatterv", sendbuf, sendcounts, displs, sendtype, recvbuf, recvcount, recvtype, root, comm, request);

  handle_MPI_BOTTOM((void*&)sendbuf, sendtype, recvbuf, recvtype);
  handle_MPI_IN_PLACE((void*&)sendbuf,recvbuf);

#if AMPI_ERROR_CHECKING
  int ret;
  if (getAmpiInstance(comm)->getRank() == root) {
    ret = errorCheck("AMPI_Iscatterv", comm, 1, 0, 0, sendtype, 1, 0, 0, 0, 0, sendbuf, 1);
    if(ret != MPI_SUCCESS){
      *request = MPI_REQUEST_NULL;
      return ret;
    }
  }
  if (sendbuf != recvbuf || getAmpiInstance(comm)->getRank() != root) {
    ret = errorCheck("AMPI_Iscatterv", comm, 1, recvcount, 1, recvtype, 1, 0, 0, 0, 0, recvbuf, 1);
    if(ret != MPI_SUCCESS){
      *request = MPI_REQUEST_NULL;
      return ret;
    }
  }
#endif

  ampi* ptr = getAmpiInstance(comm);

  if (getAmpiParent()->isInter(comm)) {
    return ptr->intercomm_iscatterv(root, sendbuf, sendcounts, displs, sendtype, recvbuf, recvcount, recvtype, comm, request);
  }
  if(ptr->getSize() == 1){
    *request = ptr->postReq(getAmpiParent()->reqPool.newReq<IReq>(recvbuf,recvcount,recvtype,root,MPI_SCATTER_TAG,comm,
                            getDDT(), AMPI_REQ_COMPLETED));
    return copyDatatype(sendtype,sendcounts[0],recvtype,recvcount,sendbuf,recvbuf);
  }

#if AMPIMSGLOG
  ampiParent* pptr = getAmpiParent();
  if(msgLogRead){
    (*(pptr->fromPUPer))|(pptr->pupBytes);
    PUParray(*(pptr->fromPUPer), (char *)recvbuf, (pptr->pupBytes));
    return MPI_SUCCESS;
  }
#endif

  int size = ptr->getSize();
  int rank = ptr->getRank();
  int i;

  if(rank == root) {
    CkDDT_DataType* dttype = ptr->getDDT()->getType(sendtype) ;
    int itemextent = dttype->getExtent();
    // use an ATAReq to non-block the caller and get a request ptr
    ATAReq *newreq = new ATAReq(size);
    for(i=0;i<size;i++) {
      if (i != rank) {
        newreq->reqs[i] = ptr->send(MPI_SCATTER_TAG, rank, ((char*)sendbuf)+(itemextent*displs[i]),
                                    sendcounts[i], sendtype, i, comm, I_SEND);
      }
    }
    newreq->reqs[rank] = MPI_REQUEST_NULL;

    if (sendbuf != recvbuf) {
      copyDatatype(sendtype,sendcounts[rank],recvtype,recvcount,(char*)sendbuf+(itemextent*displs[rank]),recvbuf);
    }
    *request = ptr->postReq(newreq);
  }
  else {
    // call irecv to post an IReq and process any pending messages
    ptr->irecv(recvbuf,recvcount,recvtype,root,MPI_SCATTER_TAG,comm,request);
  }

#if AMPIMSGLOG
  if(msgLogWrite && record_msglog(pptr->thisIndex)){
    (pptr->pupBytes) = getDDT()->getSize(recvtype) * recvcount;
    (*(pptr->toPUPer))|(pptr->pupBytes);
    PUParray(*(pptr->toPUPer), (char *)recvbuf, (pptr->pupBytes));
  }
#endif

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Alltoall, const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                                 void *recvbuf, int recvcount, MPI_Datatype recvtype,
                                 MPI_Comm comm)
{
  AMPI_API("AMPI_Alltoall", sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype, comm);

  handle_MPI_BOTTOM((void*&)sendbuf, sendtype, recvbuf, recvtype);
  handle_MPI_IN_PLACE_alltoall((void*&)sendbuf, recvbuf, sendcount, sendtype, recvcount, recvtype);

#if AMPI_ERROR_CHECKING
  int ret;
  if (sendbuf != recvbuf) {
    ret = errorCheck("AMPI_Alltoall", comm, 1, sendcount, 1, sendtype, 1, 0, 0, 0, 0, sendbuf, 1);
    if(ret != MPI_SUCCESS)
      return ret;
  }
  ret = errorCheck("AMPI_Alltoall", comm, 1, recvcount, 1, recvtype, 1, 0, 0, 0, 0, recvbuf, 1);
  if(ret != MPI_SUCCESS)
    return ret;
#endif

  ampiParent *pptr = getAmpiParent();
  ampi *ptr = pptr->comm2ampi(comm);

  if(pptr->isInter(comm))
    CkAbort("AMPI does not implement MPI_Alltoall for Inter-communicators!");
  if(ptr->getSize() == 1)
    return copyDatatype(sendtype,sendcount,recvtype,recvcount,sendbuf,recvbuf);

  int itemsize = pptr->getDDT()->getSize(sendtype) * sendcount;
  int itemextent = pptr->getDDT()->getExtent(sendtype) * sendcount;
  int extent = pptr->getDDT()->getExtent(recvtype) * recvcount;
  int size = ptr->getSize();
  int rank = ptr->getRank();


  /* For MPI_IN_PLACE (sendbuf==recvbuf), prevent using the algorithm for
   * large message sizes, since it might lead to overwriting data before
   * it gets sent in the non-power-of-two communicator size case. */
  if (recvbuf == sendbuf) {
    for (int i=0; i<size; i++) {
      for (int j=i; j<size; j++) {
        if (rank == i) {
          ptr->sendrecv_replace(((char *)recvbuf + j*extent),
                                recvcount, recvtype, j, MPI_ATA_TAG, j,
                                MPI_ATA_TAG, comm, MPI_STATUS_IGNORE);
        }
        else if (rank == j) {
          ptr->sendrecv_replace(((char *)recvbuf + i*extent),
                                recvcount, recvtype, i, MPI_ATA_TAG, i,
                                MPI_ATA_TAG, comm, MPI_STATUS_IGNORE);
        }
      }
    }
  }
  else if (itemsize <= AMPI_ALLTOALL_SHORT_MSG && size <= AMPI_ALLTOALL_THROTTLE) {
    std::vector<MPI_Request> reqs(size*2);
    for (int i=0; i<size; i++) {
      int src = (rank+i) % size;
      ptr->irecv(((char*)recvbuf)+(extent*src), recvcount, recvtype,
                 src, MPI_ATA_TAG, comm, &reqs[i]);
    }
    for (int i=0; i<size; i++) {
      int dst = (rank+i) % size;
      reqs[size+i] = ptr->send(MPI_ATA_TAG, rank, ((char*)sendbuf)+(itemextent*dst),
                               sendcount, sendtype, dst, comm, I_SEND);
    }
    pptr = pptr->waitall(reqs.size(), reqs.data());
  }
  else if (itemsize <= AMPI_ALLTOALL_LONG_MSG) {
    /* Don't post all sends and recvs at once. Instead do N sends/recvs at a time. */
    std::vector<MPI_Request> reqs(AMPI_ALLTOALL_THROTTLE*2);
    for (int j=0; j<size; j+=AMPI_ALLTOALL_THROTTLE) {
      int blockSize = std::min(size - j, AMPI_ALLTOALL_THROTTLE);
      for (int i=0; i<blockSize; i++) {
        int src = (rank + j + i) % size;
        ptr->irecv(((char*)recvbuf)+(extent*src), recvcount, recvtype,
                   src, MPI_ATA_TAG, comm, &reqs[i]);
      }
      for (int i=0; i<blockSize; i++) {
        int dst = (rank - j - i + size) % size;
        reqs[blockSize+i] = ptr->send(MPI_ATA_TAG, rank, ((char*)sendbuf)+(itemextent*dst),
                                      sendcount, sendtype, dst, comm, I_SEND);
      }
      pptr = pptr->waitall(blockSize*2, reqs.data());
    }
  }
  else {
    /* Long message. Use pairwise exchange. If comm_size is a
       power-of-two, use exclusive-or to create pairs. Else send
       to rank+i, receive from rank-i. */
    int src, dst;

    /* Is comm_size a power-of-two? */
    int pof2 = 1;
    while (pof2 < size)
      pof2 *= 2;
    bool isPof2 = (pof2 == size);

    /* The i=0 case takes care of moving local data into recvbuf */
    for (int i=0; i<size; i++) {
      if (isPof2) {
        /* use exclusive-or algorithm */
        src = dst = rank ^ i;
      }
      else {
        src = (rank - i + size) % size;
        dst = (rank + i) % size;
      }

      ptr->sendrecv(((char *)sendbuf + dst*itemextent), sendcount, sendtype, dst, MPI_ATA_TAG,
                    ((char *)recvbuf + src*extent), recvcount, recvtype, src, MPI_ATA_TAG,
                    comm, MPI_STATUS_IGNORE);
    } // end of large message
  }

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Ialltoall, const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                                  void *recvbuf, int recvcount, MPI_Datatype recvtype,
                                  MPI_Comm comm, MPI_Request *request)
{
  AMPI_API("AMPI_Ialltoall", sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype, comm, request);

  handle_MPI_BOTTOM((void*&)sendbuf, sendtype, recvbuf, recvtype);
  handle_MPI_IN_PLACE_alltoall((void*&)sendbuf, recvbuf, sendcount, sendtype, recvcount, recvtype);

#if AMPI_ERROR_CHECKING
  int ret;
  if (sendbuf != recvbuf) {
    ret = errorCheck("AMPI_Ialltoall", comm, 1, sendcount, 1, sendtype, 1, 0, 0, 0, 0, sendbuf, 1);
    if(ret != MPI_SUCCESS){
      *request = MPI_REQUEST_NULL;
      return ret;
    }
  }
  ret = errorCheck("AMPI_Ialltoall", comm, 1, recvcount, 1, recvtype, 1, 0, 0, 0, 0, recvbuf, 1);
  if(ret != MPI_SUCCESS){
    *request = MPI_REQUEST_NULL;
    return ret;
  }
#endif

  ampi *ptr = getAmpiInstance(comm);
  int size = ptr->getSize();

  if(getAmpiParent()->isInter(comm))
    CkAbort("AMPI does not implement MPI_Ialltoall for Inter-communicators!");
  if(size == 1){
    *request = ptr->postReq(getAmpiParent()->reqPool.newReq<IReq>(recvbuf,recvcount,recvtype,ptr->getRank(),MPI_ATA_TAG,comm,
                            getDDT(), AMPI_REQ_COMPLETED));
    return copyDatatype(sendtype,sendcount,recvtype,recvcount,sendbuf,recvbuf);
  }

  int rank = ptr->getRank();
  int itemsize = getDDT()->getSize(sendtype) * sendcount;
  int extent = getDDT()->getExtent(recvtype) * recvcount;

  // use an ATAReq to non-block the caller and get a request ptr
  ATAReq *newreq = new ATAReq(size*2);
  for (int i=0; i<size; i++) {
    ptr->irecv((char*)recvbuf+(extent*i), recvcount, recvtype, i, MPI_ATA_TAG, comm, &newreq->reqs[i]);
  }

  for (int i=0; i<size; i++) {
    int dst = (rank+i) % size;
    newreq->reqs[size+i] = ptr->send(MPI_ATA_TAG, rank, ((char*)sendbuf)+(itemsize*dst), sendcount,
                                     sendtype, dst, comm, I_SEND);
  }
  *request = ptr->postReq(newreq);

  AMPI_DEBUG("MPI_Ialltoall: request=%d\n", *request);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Alltoallv, const void *sendbuf, const int *sendcounts, const int *sdispls,
                                  MPI_Datatype sendtype, void *recvbuf, const int *recvcounts,
                                  const int *rdispls, MPI_Datatype recvtype, MPI_Comm comm)
{
  AMPI_API("AMPI_Alltoallv", sendbuf, sendcounts, sdispls, sendtype, recvbuf, recvcounts, rdispls, recvtype, comm);

  handle_MPI_BOTTOM((void*&)sendbuf, sendtype, recvbuf, recvtype);
  handle_MPI_IN_PLACE_alltoallv((void*&)sendbuf, recvbuf, (int*&)sendcounts, sendtype,
                                (int*&)sdispls, recvcounts, recvtype, rdispls);

#if AMPI_ERROR_CHECKING
  int ret;
  if (sendbuf != recvbuf) {
    ret = errorCheck("AMPI_Alltoallv", comm, 1, 0, 0, sendtype, 1, 0, 0, 0, 0, sendbuf, 1);
    if(ret != MPI_SUCCESS)
      return ret;
  }
  ret = errorCheck("AMPI_Alltoallv", comm, 1, 0, 0, recvtype, 1, 0, 0, 0, 0, recvbuf, 1);
  if(ret != MPI_SUCCESS)
    return ret;
#endif

  ampiParent *pptr = getAmpiParent();
  ampi *ptr = pptr->comm2ampi(comm);
  int size = ptr->getSize();

  if(pptr->isInter(comm))
    CkAbort("AMPI does not implement MPI_Alltoallv for Inter-communicators!");
  if(size == 1)
    return copyDatatype(sendtype,sendcounts[0],recvtype,recvcounts[0],sendbuf,recvbuf);

  int rank = ptr->getRank();
  int itemextent = pptr->getDDT()->getExtent(sendtype);
  int extent = pptr->getDDT()->getExtent(recvtype);

  if (recvbuf == sendbuf) {
    for (int i=0; i<size; i++) {
      for (int j=i; j<size; j++) {
        if (rank == i) {
          ptr->sendrecv_replace(((char *)recvbuf + (extent*rdispls[j])),
                                recvcounts[j], recvtype, j, MPI_ATA_TAG, j,
                                MPI_ATA_TAG, comm, MPI_STATUS_IGNORE);
        }
        else if (rank == j) {
          ptr->sendrecv_replace(((char *)recvbuf + (extent*rdispls[i])),
                                recvcounts[i], recvtype, i, MPI_ATA_TAG, i,
                                MPI_ATA_TAG, comm, MPI_STATUS_IGNORE);
        }
      }
    }
  }
  else if (size <= AMPI_ALLTOALL_THROTTLE) {
    std::vector<MPI_Request> reqs(size*2);
    for (int i=0; i<size; i++) {
      int src = (rank+i) % size;
      ptr->irecv(((char*)recvbuf)+(extent*rdispls[src]), recvcounts[src], recvtype,
                 src, MPI_ATA_TAG, comm, &reqs[i]);
    }
    for (int i=0; i<size; i++) {
      int dst = (rank+i) % size;
      reqs[size+i] = ptr->send(MPI_ATA_TAG, rank, ((char*)sendbuf)+(itemextent*sdispls[dst]),
                               sendcounts[dst], sendtype, dst, comm, I_SEND);
    }
    pptr = pptr->waitall(size*2, reqs.data());
  }
  else {
    /* Don't post all sends and recvs at once. Instead do N sends/recvs at a time. */
    std::vector<MPI_Request> reqs(AMPI_ALLTOALL_THROTTLE*2);
    for (int j=0; j<size; j+=AMPI_ALLTOALL_THROTTLE) {
      int blockSize = std::min(size - j, AMPI_ALLTOALL_THROTTLE);
      for (int i=0; i<blockSize; i++) {
        int src = (rank + j + i) % size;
        ptr->irecv(((char*)recvbuf)+(extent*rdispls[src]), recvcounts[src], recvtype,
                   src, MPI_ATA_TAG, comm, &reqs[i]);
      }
      for (int i=0; i<blockSize; i++) {
        int dst = (rank - j - i + size) % size;
        reqs[blockSize+i] = ptr->send(MPI_ATA_TAG, rank, ((char*)sendbuf)+(itemextent*sdispls[dst]),
                                      sendcounts[dst], sendtype, dst, comm);
      }
      pptr = getAmpiParent()->waitall(blockSize*2, reqs.data());
    }
  }

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Ialltoallv, void *sendbuf, int *sendcounts, int *sdispls, MPI_Datatype sendtype,
                                   void *recvbuf, int *recvcounts, int *rdispls, MPI_Datatype recvtype,
                                   MPI_Comm comm, MPI_Request *request)
{
  AMPI_API("AMPI_Ialltoallv", sendbuf, sendcounts, sdispls, sendtype, recvbuf, recvcounts, rdispls, recvtype, comm, request);

  handle_MPI_BOTTOM(sendbuf, sendtype, recvbuf, recvtype);
  handle_MPI_IN_PLACE_alltoallv((void*&)sendbuf, recvbuf, (int*&)sendcounts, sendtype,
                                (int*&)sdispls, recvcounts, recvtype, rdispls);

#if AMPI_ERROR_CHECKING
  int ret;
  if (sendbuf != recvbuf) {
    ret = errorCheck("AMPI_Ialltoallv", comm, 1, 0, 0, sendtype, 1, 0, 0, 0, 0, sendbuf, 1);
    if(ret != MPI_SUCCESS){
      *request = MPI_REQUEST_NULL;
      return ret;
    }
  }
  ret = errorCheck("AMPI_Ialltoallv", comm, 1, 0, 0, recvtype, 1, 0, 0, 0, 0, recvbuf, 1);
  if(ret != MPI_SUCCESS){
    *request = MPI_REQUEST_NULL;
    return ret;
  }
#endif

  ampi *ptr = getAmpiInstance(comm);
  int size = ptr->getSize();

  if(getAmpiParent()->isInter(comm))
    CkAbort("AMPI does not implement MPI_Ialltoallv for Inter-communicators!");
  if(size == 1){
    *request = ptr->postReq(getAmpiParent()->reqPool.newReq<IReq>(recvbuf,recvcounts[0],recvtype,ptr->getRank(),MPI_ATA_TAG,comm,
                            getDDT(), AMPI_REQ_COMPLETED));
    return copyDatatype(sendtype,sendcounts[0],recvtype,recvcounts[0],sendbuf,recvbuf);
  }

  int rank = ptr->getRank();
  int itemextent = getDDT()->getExtent(sendtype);
  int extent = getDDT()->getExtent(recvtype);

  // use an ATAReq to non-block the caller and get a request ptr
  ATAReq *newreq = new ATAReq(size*2);
  for (int i=0; i<size; i++) {
    ptr->irecv((char*)recvbuf+(extent*rdispls[i]), recvcounts[i],
               recvtype, i, MPI_ATA_TAG, comm, &newreq->reqs[i]);
  }

  for (int i=0; i<size; i++) {
    int dst = (rank+i) % size;
    newreq->reqs[size+i] = ptr->send(MPI_ATA_TAG, rank, ((char*)sendbuf)+(itemextent*sdispls[dst]),
                                     sendcounts[dst], sendtype, dst, comm, I_SEND);
  }
  *request = ptr->postReq(newreq);

  AMPI_DEBUG("MPI_Ialltoallv: request=%d\n", *request);

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Alltoallw, const void *sendbuf, const int *sendcounts, const int *sdispls,
                                  const MPI_Datatype *sendtypes, void *recvbuf, const int *recvcounts,
                                  const int *rdispls, const MPI_Datatype *recvtypes, MPI_Comm comm)
{
  AMPI_API("AMPI_Alltoallw", sendbuf, sendcounts, sdispls, sendtypes, recvbuf, recvcounts, rdispls, recvtypes, comm);

  if (sendbuf == MPI_IN_PLACE) {
    handle_MPI_BOTTOM(recvbuf, recvtypes[0]);
  } else {
    handle_MPI_BOTTOM((void*&)sendbuf, sendtypes[0], recvbuf, recvtypes[0]);
  }
  handle_MPI_IN_PLACE_alltoallw((void*&)sendbuf, recvbuf, (int*&)sendcounts,
                                (MPI_Datatype*&)sendtypes, (int*&)sdispls,
                                recvcounts, recvtypes, rdispls);

#if AMPI_ERROR_CHECKING
  int ret;
  if (sendbuf != recvbuf) {
    ret = errorCheck("AMPI_Alltoallw", comm, 1, 0, 0, sendtypes[0], 1, 0, 0, 0, 0, sendbuf, 1);
    if(ret != MPI_SUCCESS)
      return ret;
  }
  ret = errorCheck("AMPI_Alltoallw", comm, 1, 0, 0, recvtypes[0], 1, 0, 0, 0, 0, recvbuf, 1);
  if(ret != MPI_SUCCESS)
    return ret;
#endif

  ampiParent *pptr = getAmpiParent();
  ampi *ptr = pptr->comm2ampi(comm);
  int size = ptr->getSize();
  int rank = ptr->getRank();

  if(pptr->isInter(comm))
    CkAbort("AMPI does not implement MPI_Alltoallw for Inter-communicators!");
  if(size == 1)
    return copyDatatype(sendtypes[0],sendcounts[0],recvtypes[0],recvcounts[0],sendbuf,recvbuf);

  /* displs are in terms of bytes for Alltoallw (unlike Alltoallv) */
  if (recvbuf == sendbuf) {
    for (int i=0; i<size; i++) {
      for (int j=i; j<size; j++) {
        if (rank == i) {
          ptr->sendrecv_replace(((char *)recvbuf + rdispls[j]),
                                recvcounts[j], recvtypes[j], j, MPI_ATA_TAG, j,
                                MPI_ATA_TAG, comm, MPI_STATUS_IGNORE);
        }
        else if (rank == j) {
          ptr->sendrecv_replace(((char *)recvbuf + rdispls[i]),
                                recvcounts[i], recvtypes[i], i, MPI_ATA_TAG, i,
                                MPI_ATA_TAG, comm, MPI_STATUS_IGNORE);
        }
      }
    }
  }
  else if (size <= AMPI_ALLTOALL_THROTTLE) {
    std::vector<MPI_Request> reqs(size*2);
    for (int i=0; i<size; i++) {
      int src = (rank+i) % size;
      ptr->irecv(((char*)recvbuf)+rdispls[src], recvcounts[src], recvtypes[src],
                 src, MPI_ATA_TAG, comm, &reqs[i]);
    }
    for (int i=0; i<size; i++) {
      int dst = (rank+i) % size;
      reqs[size+i] = ptr->send(MPI_ATA_TAG, rank, ((char*)sendbuf)+sdispls[dst],
                               sendcounts[dst], sendtypes[dst], dst, comm, I_SEND);
    }
    pptr = pptr->waitall(size*2, reqs.data());
  }
  else {
    /* Don't post all sends and recvs at once. Instead do N sends/recvs at a time. */
    std::vector<MPI_Request> reqs(AMPI_ALLTOALL_THROTTLE*2);
    for (int j=0; j<size; j+=AMPI_ALLTOALL_THROTTLE) {
      int blockSize = std::min(size - j, AMPI_ALLTOALL_THROTTLE);
      for (int i=0; i<blockSize; i++) {
        int src = (rank + j + i) % size;
        ptr->irecv(((char*)recvbuf)+rdispls[src], recvcounts[src], recvtypes[src],
                   src, MPI_ATA_TAG, comm, &reqs[i]);
      }
      for (int i=0; i<blockSize; i++) {
        int dst = (rank - j - i + size) % size;
        reqs[blockSize+i] = ptr->send(MPI_ATA_TAG, rank, ((char*)sendbuf)+sdispls[dst],
                                      sendcounts[dst], sendtypes[dst], dst, comm);
      }
      pptr = getAmpiParent()->waitall(blockSize*2, reqs.data());
    }
  }

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Ialltoallw, const void *sendbuf, const int *sendcounts, const int *sdispls,
                                   const MPI_Datatype *sendtypes, void *recvbuf, const int *recvcounts,
                                   const int *rdispls, const MPI_Datatype *recvtypes, MPI_Comm comm,
                                   MPI_Request *request)
{
  AMPI_API("AMPI_Ialltoallw", sendbuf, sendcounts, sdispls, sendtypes, recvbuf, recvcounts, rdispls, recvtypes, comm, request);

  if (sendbuf == MPI_IN_PLACE) {
    handle_MPI_BOTTOM(recvbuf, recvtypes[0]);
  } else {
    handle_MPI_BOTTOM((void*&)sendbuf, sendtypes[0], recvbuf, recvtypes[0]);
  }
  handle_MPI_IN_PLACE_alltoallw((void*&)sendbuf, recvbuf, (int*&)sendcounts,
                                (MPI_Datatype*&)sendtypes, (int*&)sdispls,
                                recvcounts, recvtypes, rdispls);

#if AMPI_ERROR_CHECKING
  int ret;
  if (sendbuf != recvbuf) {
    ret = errorCheck("AMPI_Ialltoallw", comm, 1, 0, 0, sendtypes[0], 1, 0, 0, 0, 0, sendbuf, 1);
    if(ret != MPI_SUCCESS){
      *request = MPI_REQUEST_NULL;
      return ret;
    }
  }
  ret = errorCheck("AMPI_Ialltoallw", comm, 1, 0, 0, recvtypes[0], 1, 0, 0, 0, 0, recvbuf, 1);
  if(ret != MPI_SUCCESS){
    *request = MPI_REQUEST_NULL;
    return ret;
  }
#endif

  ampi *ptr = getAmpiInstance(comm);
  int size = ptr->getSize();
  int rank = ptr->getRank();

  if(getAmpiParent()->isInter(comm))
    CkAbort("AMPI does not implement MPI_Ialltoallw for Inter-communicators!");
  if(size == 1){
    *request = ptr->postReq(getAmpiParent()->reqPool.newReq<IReq>(recvbuf,recvcounts[0],recvtypes[0],ptr->getRank(),MPI_ATA_TAG,comm,
                            getDDT(), AMPI_REQ_COMPLETED));
    return copyDatatype(sendtypes[0],sendcounts[0],recvtypes[0],recvcounts[0],sendbuf,recvbuf);
  }

  /* displs are in terms of bytes for Alltoallw (unlike Alltoallv) */

  // use an ATAReq to non-block the caller and get a request ptr
  ATAReq *newreq = new ATAReq(size*2);
  for (int i=0; i<size; i++) {
    ptr->irecv((char*)recvbuf+rdispls[i], recvcounts[i], recvtypes[i],
               i, MPI_ATA_TAG, comm, &newreq->reqs[i]);
  }

  for (int i=0; i<size; i++) {
    int dst = (rank+i) % size;
    newreq->reqs[i] = ptr->send(MPI_ATA_TAG, rank, (char*)sendbuf+sdispls[dst],
                                sendcounts[dst], sendtypes[dst], dst, comm, I_SEND);
  }
  *request = ptr->postReq(newreq);

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Neighbor_alltoall, const void* sendbuf, int sendcount, MPI_Datatype sendtype,
                                          void* recvbuf, int recvcount, MPI_Datatype recvtype,
                                          MPI_Comm comm)
{
  AMPI_API("AMPI_Neighbor_alltoall", sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype, comm);

  handle_MPI_BOTTOM((void*&)sendbuf, sendtype, recvbuf, recvtype);

#if AMPI_ERROR_CHECKING
  if (sendbuf == MPI_IN_PLACE || recvbuf == MPI_IN_PLACE)
    CkAbort("MPI_Neighbor_alltoall does not accept MPI_IN_PLACE!");
  if (getAmpiParent()->isInter(comm))
    CkAbort("MPI_Neighbor_alltoall is not defined for Inter-communicators!");
  int ret;
  ret = errorCheck("AMPI_Neighbor_alltoall", comm, 1, sendcount, 1, sendtype, 1, 0, 0, 0, 0, sendbuf, 1);
  if(ret != MPI_SUCCESS)
    return ret;
  ret = errorCheck("AMPI_Neighbor_alltoall", comm, 1, recvcount, 1, recvtype, 1, 0, 0, 0, 0, recvbuf, 1);
  if(ret != MPI_SUCCESS)
    return ret;
#endif

  ampiParent *pptr = getAmpiParent();
  ampi *ptr = pptr->comm2ampi(comm);
  int rank_in_comm = ptr->getRank();

  if (ptr->getSize() == 1)
    return copyDatatype(sendtype, sendcount, recvtype, recvcount, sendbuf, recvbuf);

  const std::vector<int>& neighbors = ptr->getNeighbors();
  int num_neighbors = neighbors.size();
  int itemsize = getDDT()->getSize(sendtype) * sendcount;
  int extent = getDDT()->getExtent(recvtype) * recvcount;

  std::vector<MPI_Request> reqs(num_neighbors*2);
  for (int j=0; j<num_neighbors; j++) {
    ptr->irecv(((char*)recvbuf)+(extent*j), recvcount, recvtype,
               neighbors[j], MPI_NBOR_TAG, comm, &reqs[j]);
  }

  for (int i=0; i<num_neighbors; i++) {
    reqs[num_neighbors+i] = ptr->send(MPI_NBOR_TAG, rank_in_comm, (void*)((char*)sendbuf+(itemsize*i)),
                                      sendcount, sendtype, neighbors[i], comm, I_SEND);
  }

  pptr = pptr->waitall(reqs.size(), reqs.data());
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Ineighbor_alltoall, const void* sendbuf, int sendcount, MPI_Datatype sendtype,
                                           void* recvbuf, int recvcount, MPI_Datatype recvtype,
                                           MPI_Comm comm, MPI_Request *request)
{
  AMPI_API("AMPI_Ineighbor_alltoall", sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype, comm, request);

  handle_MPI_BOTTOM((void*&)sendbuf, sendtype, recvbuf, recvtype);

#if AMPI_ERROR_CHECKING
  if (sendbuf == MPI_IN_PLACE || recvbuf == MPI_IN_PLACE)
    CkAbort("MPI_Ineighbor_alltoall does not accept MPI_IN_PLACE!");
  if (getAmpiParent()->isInter(comm))
    CkAbort("MPI_Ineighbor_alltoall is not defined for Inter-communicators!");
  int ret;
  ret = errorCheck("AMPI_Ineighbor_alltoall", comm, 1, sendcount, 1, sendtype, 1, 0, 0, 0, 0, sendbuf, 1);
  if(ret != MPI_SUCCESS){
    *request = MPI_REQUEST_NULL;
    return ret;
  }
  ret = errorCheck("AMPI_Ineighbor_alltoall", comm, 1, recvcount, 1, recvtype, 1, 0, 0, 0, 0, recvbuf, 1);
  if(ret != MPI_SUCCESS){
    *request = MPI_REQUEST_NULL;
    return ret;
  }
#endif

  ampi *ptr = getAmpiInstance(comm);
  int rank_in_comm = ptr->getRank();

  if (ptr->getSize() == 1) {
    *request = ptr->postReq(getAmpiParent()->reqPool.newReq<IReq>(recvbuf,recvcount,recvtype,rank_in_comm,MPI_NBOR_TAG,comm,
                            getDDT(), AMPI_REQ_COMPLETED));
    return copyDatatype(sendtype, sendcount, recvtype, recvcount, sendbuf, recvbuf);
  }

  const std::vector<int>& neighbors = ptr->getNeighbors();
  int num_neighbors = neighbors.size();
  int itemsize = getDDT()->getSize(sendtype) * sendcount;
  int extent = getDDT()->getExtent(recvtype) * recvcount;

  // use an ATAReq to non-block the caller and get a request ptr
  ATAReq *newreq = new ATAReq(num_neighbors*2);
  for (int j=0; j<num_neighbors; j++) {
    ptr->irecv((char*)recvbuf+(extent*j), recvcount, recvtype,
               neighbors[j], MPI_NBOR_TAG, comm, &newreq->reqs[j]);
  }

  for (int i=0; i<num_neighbors; i++) {
    newreq->reqs[num_neighbors+i] = ptr->send(MPI_ATA_TAG, rank_in_comm, ((char*)sendbuf)+(i*itemsize),
                                              sendcount, sendtype, neighbors[i], comm, I_SEND);
  }
  *request = ptr->postReq(newreq);

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Neighbor_alltoallv, const void* sendbuf, const int *sendcounts, const int *sdispls,
                                           MPI_Datatype sendtype, void* recvbuf, const int *recvcounts,
                                           const int *rdispls, MPI_Datatype recvtype, MPI_Comm comm)
{
  AMPI_API("AMPI_Neighbor_alltoallv", sendbuf, sendcounts, sdispls, sendtype, recvbuf, recvcounts, rdispls, recvtype, comm);

  handle_MPI_BOTTOM((void*&)sendbuf, sendtype, recvbuf, recvtype);

#if AMPI_ERROR_CHECKING
  if (sendbuf == MPI_IN_PLACE || recvbuf == MPI_IN_PLACE)
    CkAbort("MPI_Neighbor_alltoallv does not accept MPI_IN_PLACE!");
  if (getAmpiParent()->isInter(comm))
    CkAbort("MPI_Neighbor_alltoallv is not defined for Inter-communicators!");
  int ret;
  ret = errorCheck("AMPI_Neighbor_alltoallv", comm, 1, sendcounts[0], 1, sendtype, 1, 0, 0, 0, 0, sendbuf, 1);
  if(ret != MPI_SUCCESS)
    return ret;
  ret = errorCheck("AMPI_Neighbor_alltoallv", comm, 1, recvcounts[0], 1, recvtype, 1, 0, 0, 0, 0, recvbuf, 1);
  if(ret != MPI_SUCCESS)
    return ret;
#endif

  ampiParent *pptr = getAmpiParent();
  ampi *ptr = pptr->comm2ampi(comm);
  int rank_in_comm = ptr->getRank();

  if (ptr->getSize() == 1)
    return copyDatatype(sendtype, sendcounts[0], recvtype, recvcounts[0], sendbuf, recvbuf);

  const std::vector<int>& neighbors = ptr->getNeighbors();
  int num_neighbors = neighbors.size();
  int itemsize = getDDT()->getSize(sendtype);
  int extent = getDDT()->getExtent(recvtype);

  std::vector<MPI_Request> reqs(num_neighbors*2);
  for (int j=0; j<num_neighbors; j++) {
    ptr->irecv(((char*)recvbuf)+(extent*rdispls[j]), recvcounts[j], recvtype,
               neighbors[j], MPI_NBOR_TAG, comm, &reqs[j]);
  }

  for (int i=0; i<num_neighbors; i++) {
    reqs[num_neighbors+i] = ptr->send(MPI_NBOR_TAG, rank_in_comm, (void*)((char*)sendbuf+(itemsize*sdispls[i])),
                                      sendcounts[i], sendtype, neighbors[i], comm, I_SEND);
  }

  pptr = pptr->waitall(reqs.size(), reqs.data());
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Ineighbor_alltoallv, const void* sendbuf, const int *sendcounts, const int *sdispls,
                                            MPI_Datatype sendtype, void* recvbuf, const int *recvcounts,
                                            const int *rdispls, MPI_Datatype recvtype, MPI_Comm comm,
                                            MPI_Request *request)
{
  AMPI_API("AMPI_Ineighbor_alltoallv", sendbuf, sendcounts, sdispls, sendtype, recvbuf, recvcounts, rdispls, recvtype, comm, request);

  handle_MPI_BOTTOM((void*&)sendbuf, sendtype, recvbuf, recvtype);

#if AMPI_ERROR_CHECKING
  if (sendbuf == MPI_IN_PLACE || recvbuf == MPI_IN_PLACE)
    CkAbort("MPI_Ineighbor_alltoallv does not accept MPI_IN_PLACE!");
  if (getAmpiParent()->isInter(comm))
    CkAbort("MPI_Ineighbor_alltoallv is not defined for Inter-communicators!");
  int ret;
  ret = errorCheck("AMPI_Ineighbor_alltoallv", comm, 1, sendcounts[0], 1, sendtype, 1, 0, 0, 0, 0, sendbuf, 1);
  if(ret != MPI_SUCCESS){
    *request = MPI_REQUEST_NULL;
    return ret;
  }
  ret = errorCheck("AMPI_Ineighbor_alltoallv", comm, 1, recvcounts[0], 1, recvtype, 1, 0, 0, 0, 0, recvbuf, 1);
  if(ret != MPI_SUCCESS){
    *request = MPI_REQUEST_NULL;
    return ret;
  }
#endif

  ampi *ptr = getAmpiInstance(comm);
  int rank_in_comm = ptr->getRank();

  if (ptr->getSize() == 1) {
    *request = ptr->postReq(getAmpiParent()->reqPool.newReq<IReq>(recvbuf,recvcounts[0],recvtype,rank_in_comm,MPI_NBOR_TAG,comm,
                            getDDT(), AMPI_REQ_COMPLETED));
    return copyDatatype(sendtype, sendcounts[0], recvtype, recvcounts[0], sendbuf, recvbuf);
  }

  const std::vector<int>& neighbors = ptr->getNeighbors();
  int num_neighbors = neighbors.size();
  int itemsize = getDDT()->getSize(sendtype);
  int extent = getDDT()->getExtent(recvtype);

  // use an ATAReq to non-block the caller and get a request ptr
  ATAReq *newreq = new ATAReq(num_neighbors*2);
  for (int j=0; j<num_neighbors; j++) {
    ptr->irecv((char*)recvbuf+(extent*rdispls[j]), recvcounts[j], recvtype,
               neighbors[j], MPI_NBOR_TAG, comm, &newreq->reqs[j]);
  }

  for (int i=0; i<num_neighbors; i++) {
    newreq->reqs[num_neighbors+i] = ptr->send(MPI_NBOR_TAG, rank_in_comm, (char*)sendbuf+(itemsize*sdispls[i]),
                                              sendcounts[i], sendtype, neighbors[i], comm, I_SEND);
  }
  *request = ptr->postReq(newreq);

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Neighbor_alltoallw, const void* sendbuf, const int *sendcounts, const MPI_Aint *sdispls,
                                           const MPI_Datatype *sendtypes, void* recvbuf, const int *recvcounts,
                                           const MPI_Aint *rdispls, const MPI_Datatype *recvtypes, MPI_Comm comm)
{
  AMPI_API("AMPI_Neighbor_alltoallw", sendbuf, sendcounts, sdispls, sendtypes, recvbuf, recvcounts, rdispls, recvtypes, comm);

  handle_MPI_BOTTOM((void*&)sendbuf, sendtypes[0], recvbuf, recvtypes[0]);

#if AMPI_ERROR_CHECKING
  if (sendbuf == MPI_IN_PLACE || recvbuf == MPI_IN_PLACE)
    CkAbort("MPI_Neighbor_alltoallw does not accept MPI_IN_PLACE!");
  if (getAmpiParent()->isInter(comm))
    CkAbort("MPI_Neighbor_alltoallw is not defined for Inter-communicators!");
  int ret;
  ret = errorCheck("AMPI_Neighbor_alltoallw", comm, 1, sendcounts[0], 1, sendtypes[0], 1, 0, 0, 0, 0, sendbuf, 1);
  if(ret != MPI_SUCCESS)
    return ret;
  ret = errorCheck("AMPI_Neighbor_alltoallw", comm, 1, recvcounts[0], 1, recvtypes[0], 1, 0, 0, 0, 0, recvbuf, 1);
  if(ret != MPI_SUCCESS)
    return ret;
#endif

  ampiParent *pptr = getAmpiParent();
  ampi *ptr = pptr->comm2ampi(comm);
  int rank_in_comm = ptr->getRank();

  if (ptr->getSize() == 1)
    return copyDatatype(sendtypes[0], sendcounts[0], recvtypes[0], recvcounts[0], sendbuf, recvbuf);

  const std::vector<int>& neighbors = ptr->getNeighbors();
  int num_neighbors = neighbors.size();

  std::vector<MPI_Request> reqs(num_neighbors*2);
  for (int j=0; j<num_neighbors; j++) {
    ptr->irecv(((char*)recvbuf)+rdispls[j], recvcounts[j], recvtypes[j],
               neighbors[j], MPI_NBOR_TAG, comm, &reqs[j]);
  }

  for (int i=0; i<num_neighbors; i++) {
    reqs[num_neighbors+i] = ptr->send(MPI_NBOR_TAG, rank_in_comm, (void*)((char*)sendbuf+sdispls[i]),
                                      sendcounts[i], sendtypes[i], neighbors[i], comm, I_SEND);
  }

  pptr = pptr->waitall(reqs.size(), reqs.data());
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Ineighbor_alltoallw, const void* sendbuf, const int *sendcounts, const MPI_Aint *sdispls,
                                            const MPI_Datatype *sendtypes, void* recvbuf, const int *recvcounts,
                                            const MPI_Aint *rdispls, const MPI_Datatype *recvtypes, MPI_Comm comm,
                                            MPI_Request *request)
{
  AMPI_API("AMPI_Ineighbor_alltoallw", sendbuf, sendcounts, sdispls, sendtypes, recvbuf, recvcounts, rdispls, recvtypes, comm, request);

  handle_MPI_BOTTOM((void*&)sendbuf, sendtypes[0], recvbuf, recvtypes[0]);

#if AMPI_ERROR_CHECKING
  if (sendbuf == MPI_IN_PLACE || recvbuf == MPI_IN_PLACE)
    CkAbort("MPI_Ineighbor_alltoallw does not accept MPI_IN_PLACE!");
  if (getAmpiParent()->isInter(comm))
    CkAbort("MPI_Ineighbor_alltoallw is not defined for Inter-communicators!");
  int ret;
  ret = errorCheck("AMPI_Ineighbor_alltoallw", comm, 1, sendcounts[0], 1, sendtypes[0], 1, 0, 0, 0, 0, sendbuf, 1);
  if(ret != MPI_SUCCESS){
    *request = MPI_REQUEST_NULL;
    return ret;
  }
  ret = errorCheck("AMPI_Ineighbor_alltoallw", comm, 1, recvcounts[0], 1, recvtypes[0], 1, 0, 0, 0, 0, recvbuf, 1);
  if(ret != MPI_SUCCESS){
    *request = MPI_REQUEST_NULL;
    return ret;
  }
#endif

  ampi *ptr = getAmpiInstance(comm);
  int rank_in_comm = ptr->getRank();

  if (ptr->getSize() == 1) {
    *request = ptr->postReq(getAmpiParent()->reqPool.newReq<IReq>(recvbuf,recvcounts[0],recvtypes[0],rank_in_comm,MPI_NBOR_TAG,comm,
                            getDDT(), AMPI_REQ_COMPLETED));
    return copyDatatype(sendtypes[0], sendcounts[0], recvtypes[0], recvcounts[0], sendbuf, recvbuf);
  }

  const std::vector<int>& neighbors = ptr->getNeighbors();
  int num_neighbors = neighbors.size();

  // use an ATAReq to non-block the caller and get a request ptr
  ATAReq *newreq = new ATAReq(num_neighbors*2);
  for (int j=0; j<num_neighbors; j++) {
    ptr->irecv((char*)recvbuf+rdispls[j], recvcounts[j], recvtypes[j],
               neighbors[j], MPI_NBOR_TAG, comm, &newreq->reqs[j]);
  }

  for (int i=0; i<num_neighbors; i++) {
    newreq->reqs[num_neighbors+i] = ptr->send(MPI_NBOR_TAG, rank_in_comm, (void*)((char*)sendbuf+sdispls[i]),
                                              sendcounts[i], sendtypes[i], neighbors[i], comm, I_SEND);
  }
  *request = ptr->postReq(newreq);

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Neighbor_allgather, const void* sendbuf, int sendcount, MPI_Datatype sendtype,
                                           void* recvbuf, int recvcount, MPI_Datatype recvtype,
                                           MPI_Comm comm)
{
  AMPI_API("AMPI_Neighbor_allgather", sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype, comm);

  handle_MPI_BOTTOM((void*&)sendbuf, sendtype, recvbuf, recvtype);

#if AMPI_ERROR_CHECKING
  if (sendbuf == MPI_IN_PLACE || recvbuf == MPI_IN_PLACE)
    CkAbort("MPI_Neighbor_allgather does not accept MPI_IN_PLACE!");
  if (getAmpiParent()->isInter(comm))
    CkAbort("MPI_Neighbor_allgather is not defined for Inter-communicators!");
  int ret;
  ret = errorCheck("AMPI_Neighbor_allgather", comm, 1, sendcount, 1, sendtype, 1, 0, 0, 0, 0, sendbuf, 1);
  if(ret != MPI_SUCCESS)
    return ret;
  ret = errorCheck("AMPI_Neighbor_allgather", comm, 1, recvcount, 1, recvtype, 1, 0, 0, 0, 0, recvbuf, 1);
  if(ret != MPI_SUCCESS)
    return ret;
#endif

  ampiParent *pptr = getAmpiParent();
  ampi *ptr = pptr->comm2ampi(comm);
  int rank_in_comm = ptr->getRank();

  if (ptr->getSize() == 1)
    return copyDatatype(sendtype, sendcount, recvtype, recvcount, sendbuf, recvbuf);

  const std::vector<int>& neighbors = ptr->getNeighbors();
  int num_neighbors = neighbors.size();

  int extent = getDDT()->getExtent(recvtype) * recvcount;
  std::vector<MPI_Request> reqs(num_neighbors*2);
  for (int j=0; j<num_neighbors; j++) {
    ptr->irecv(((char*)recvbuf)+(extent*j), recvcount, recvtype,
               neighbors[j], MPI_NBOR_TAG, comm, &reqs[j]);
  }

  for (int i=0; i<num_neighbors; i++) {
    reqs[num_neighbors+i] = ptr->send(MPI_NBOR_TAG, rank_in_comm, sendbuf, sendcount,
                                      sendtype, neighbors[i], comm, I_SEND);
  }

  pptr = pptr->waitall(reqs.size(), reqs.data());
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Ineighbor_allgather, const void* sendbuf, int sendcount, MPI_Datatype sendtype,
                                            void* recvbuf, int recvcount, MPI_Datatype recvtype,
                                            MPI_Comm comm, MPI_Request *request)
{
  AMPI_API("AMPI_Ineighbor_allgather", sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype, comm, request);

  handle_MPI_BOTTOM((void*&)sendbuf, sendtype, recvbuf, recvtype);

#if AMPI_ERROR_CHECKING
  if (sendbuf == MPI_IN_PLACE || recvbuf == MPI_IN_PLACE)
    CkAbort("MPI_Ineighbor_allgather does not accept MPI_IN_PLACE!");
  if (getAmpiParent()->isInter(comm))
    CkAbort("MPI_Ineighbor_allgather is not defined for Inter-communicators!");
  int ret;
  ret = errorCheck("AMPI_Ineighbor_allgather", comm, 1, sendcount, 1, sendtype, 1, 0, 0, 0, 0, sendbuf, 1);
  if(ret != MPI_SUCCESS){
    *request = MPI_REQUEST_NULL;
    return ret;
  }
  ret = errorCheck("AMPI_Ineighbor_allgather", comm, 1, recvcount, 1, recvtype, 1, 0, 0, 0, 0, recvbuf, 1);
  if(ret != MPI_SUCCESS){
    *request = MPI_REQUEST_NULL;
    return ret;
  }
#endif

  ampi *ptr = getAmpiInstance(comm);
  int rank_in_comm = ptr->getRank();

  if (ptr->getSize() == 1) {
    *request = ptr->postReq(getAmpiParent()->reqPool.newReq<IReq>(recvbuf,recvcount,recvtype,rank_in_comm,MPI_NBOR_TAG,comm,
                            getDDT(), AMPI_REQ_COMPLETED));
    return copyDatatype(sendtype, sendcount, recvtype, recvcount, sendbuf, recvbuf);
  }

  const std::vector<int>& neighbors = ptr->getNeighbors();
  int num_neighbors = neighbors.size();

  // use an ATAReq to non-block the caller and get a request ptr
  ATAReq *newreq = new ATAReq(num_neighbors*2);
  int extent = getDDT()->getExtent(recvtype) * recvcount;
  for (int j=0; j<num_neighbors; j++) {
    ptr->irecv((char*)recvbuf+(extent*j), recvcount, recvtype,
               neighbors[j], MPI_NBOR_TAG, comm, &newreq->reqs[j]);
  }

  for (int i=0; i<num_neighbors; i++) {
    newreq->reqs[num_neighbors+i] = ptr->send(MPI_NBOR_TAG, rank_in_comm, sendbuf, sendcount,
                                              sendtype, neighbors[i], comm, I_SEND);
  }
  *request = ptr->postReq(newreq);

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Neighbor_allgatherv, const void* sendbuf, int sendcount, MPI_Datatype sendtype,
                                            void* recvbuf, const int *recvcounts, const int *displs,
                                            MPI_Datatype recvtype, MPI_Comm comm)
{
  AMPI_API("AMPI_Neighbor_allgatherv", sendbuf, sendcount, sendtype, recvbuf, recvcounts, displs, recvtype, comm);

  handle_MPI_BOTTOM((void*&)sendbuf, sendtype, recvbuf, recvtype);

#if AMPI_ERROR_CHECKING
  if (sendbuf == MPI_IN_PLACE || recvbuf == MPI_IN_PLACE)
    CkAbort("MPI_Neighbor_allgatherv does not accept MPI_IN_PLACE!");
  if (getAmpiParent()->isInter(comm))
    CkAbort("MPI_Neighbor_allgatherv is not defined for Inter-communicators!");
  int ret;
  ret = errorCheck("AMPI_Neighbor_allgatherv", comm, 1, sendcount, 1, sendtype, 1, 0, 0, 0, 0, sendbuf, 1);
  if(ret != MPI_SUCCESS)
    return ret;
  ret = errorCheck("AMPI_Neighbor_allgatherv", comm, 1, recvcounts[0], 1, recvtype, 1, 0, 0, 0, 0, recvbuf, 1);
  if(ret != MPI_SUCCESS)
    return ret;
#endif

  ampiParent *pptr = getAmpiParent();
  ampi *ptr = pptr->comm2ampi(comm);
  int rank_in_comm = ptr->getRank();

  if (ptr->getSize() == 1)
    return copyDatatype(sendtype, sendcount, recvtype, recvcounts[0], sendbuf, recvbuf);

  const std::vector<int>& neighbors = ptr->getNeighbors();
  int num_neighbors = neighbors.size();
  int extent = getDDT()->getExtent(recvtype);
  std::vector<MPI_Request> reqs(num_neighbors*2);
  for (int j=0; j<num_neighbors; j++) {
    ptr->irecv(((char*)recvbuf)+(extent*displs[j]), recvcounts[j], recvtype,
               neighbors[j], MPI_NBOR_TAG, comm, &reqs[j]);
  }
  for (int i=0; i<num_neighbors; i++) {
    reqs[num_neighbors+i] = ptr->send(MPI_NBOR_TAG, rank_in_comm, sendbuf, sendcount,
                                      sendtype, neighbors[i], comm, I_SEND);
  }

  pptr = pptr->waitall(reqs.size(), reqs.data());
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Ineighbor_allgatherv, const void* sendbuf, int sendcount, MPI_Datatype sendtype,
                                             void* recvbuf, const int* recvcounts, const int* displs,
                                             MPI_Datatype recvtype, MPI_Comm comm, MPI_Request *request)
{
  AMPI_API("AMPI_Ineighbor_allgatherv", sendbuf, sendcount, sendtype, recvbuf, recvcounts, displs, recvtype, comm, request);

  handle_MPI_BOTTOM((void*&)sendbuf, sendtype, recvbuf, recvtype);

#if AMPI_ERROR_CHECKING
  if (sendbuf == MPI_IN_PLACE || recvbuf == MPI_IN_PLACE)
    CkAbort("MPI_Ineighbor_allgatherv does not accept MPI_IN_PLACE!");
  if (getAmpiParent()->isInter(comm))
    CkAbort("MPI_Ineighbor_allgatherv is not defined for Inter-communicators!");
  int ret;
  ret = errorCheck("AMPI_Ineighbor_allgatherv", comm, 1, sendcount, 1, sendtype, 1, 0, 0, 0, 0, sendbuf, 1);
  if(ret != MPI_SUCCESS){
    *request = MPI_REQUEST_NULL;
    return ret;
  }
  ret = errorCheck("AMPI_Ineighbor_allgatherv", comm, 1, recvcounts[0], 1, recvtype, 1, 0, 0, 0, 0, recvbuf, 1);
  if(ret != MPI_SUCCESS){
    *request = MPI_REQUEST_NULL;
    return ret;
  }
#endif

  ampi *ptr = getAmpiInstance(comm);
  int rank_in_comm = ptr->getRank();

  if (ptr->getSize() == 1) {
    *request = ptr->postReq(getAmpiParent()->reqPool.newReq<IReq>(recvbuf,recvcounts[0],recvtype,rank_in_comm,MPI_NBOR_TAG,comm,
                            getDDT(), AMPI_REQ_COMPLETED));
    return copyDatatype(sendtype, sendcount, recvtype, recvcounts[0], sendbuf, recvbuf);
  }

  const std::vector<int>& neighbors = ptr->getNeighbors();
  int num_neighbors = neighbors.size();

  // use an ATAReq to non-block the caller and get a request ptr
  ATAReq *newreq = new ATAReq(num_neighbors*2);
  int extent = getDDT()->getExtent(recvtype);
  for (int j=0; j<num_neighbors; j++) {
    ptr->irecv((char*)recvbuf+(extent*displs[j]), recvcounts[j], recvtype,
               neighbors[j], MPI_NBOR_TAG, comm, &newreq->reqs[j]);
  }

  for (int i=0; i<num_neighbors; i++) {
    newreq->reqs[num_neighbors+i] = ptr->send(MPI_NBOR_TAG, rank_in_comm, sendbuf, sendcount,
                                              sendtype, neighbors[i], comm, I_SEND);
  }
  *request = ptr->postReq(newreq);

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Comm_dup, MPI_Comm comm, MPI_Comm *newcomm)
{
  AMPI_API("AMPI_Comm_dup", comm, newcomm);

  {
    ampi *ptr = getAmpiInstance(comm);
    int topoType, rank = ptr->getRank();
    MPI_Topo_test(comm, &topoType);
    ptr->topoDup(topoType, rank, comm, newcomm);
  }

  ampiParent * parent = getAmpiParent();
  auto & old_attr = parent->getAttributes(comm);
  auto & new_attr = parent->getAttributes(*newcomm);
  int ret = parent->dupUserAttributes(comm, old_attr, new_attr);
  ampi * unused = getAmpiInstance(comm)->barrier();

#if AMPIMSGLOG
  ampiParent* pptr = getAmpiParent();
  if(msgLogRead){
    PUParray(*(pptr->fromPUPer), (char *)newcomm, sizeof(int));
    return MPI_SUCCESS;
  }
  else if(msgLogWrite && record_msglog(pptr->thisIndex)){
    PUParray(*(pptr->toPUPer), (char *)newcomm, sizeof(int));
  }
#endif
  return ampiErrhandler("AMPI_Comm_dup", ret);
}

AMPI_API_IMPL(int, MPI_Comm_idup, MPI_Comm comm, MPI_Comm *newcomm, MPI_Request *request)
{
  AMPI_API("AMPI_Comm_idup", comm, newcomm, request);
  // FIXME: implement non-blocking comm_dup
  *request = MPI_REQUEST_NULL;
  return MPI_Comm_dup(comm, newcomm);
}

AMPI_API_IMPL(int, MPI_Comm_dup_with_info, MPI_Comm comm, MPI_Info info, MPI_Comm *dest)
{
  AMPI_API("AMPI_Comm_dup_with_info", comm, info, dest);
  MPI_Comm_dup(comm, dest);
  MPI_Comm_set_info(*dest, info);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Comm_idup_with_info, MPI_Comm comm, MPI_Info info, MPI_Comm *dest, MPI_Request *request)
{
  AMPI_API("AMPI_Comm_idup_with_info", comm, info, dest, request);
  // FIXME: implement non-blocking comm_dup_with_info
  *request = MPI_REQUEST_NULL;
  return MPI_Comm_dup_with_info(comm, info, dest);
}

AMPI_API_IMPL(int, MPI_Comm_split, MPI_Comm src, int color, int key, MPI_Comm *dest)
{
  AMPI_API("AMPI_Comm_split", src, color, key, dest);
  {
    ampiParent *pptr = getAmpiParent();
    ampi *ptr = pptr->comm2ampi(src);
    ptr->split(color, key, dest, ptr->getCommStruct().getType());
  }
  if (color == MPI_UNDEFINED) *dest = MPI_COMM_NULL;

#if AMPIMSGLOG
  if(msgLogRead){
    PUParray(*(pptr->fromPUPer), (char *)dest, sizeof(int));
    return MPI_SUCCESS;
  }
  else if(msgLogWrite && record_msglog(pptr->thisIndex)){
    PUParray(*(pptr->toPUPer), (char *)dest, sizeof(int));
  }
#endif

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Comm_split_type, MPI_Comm src, int split_type, int key,
                                        MPI_Info info, MPI_Comm *dest)
{
  AMPI_API("AMPI_Comm_split_type", src, split_type, key, info, dest);

  if (src == MPI_COMM_SELF && split_type == MPI_UNDEFINED) {
    *dest = MPI_COMM_NULL;
    return MPI_SUCCESS;
  }

  int color = MPI_UNDEFINED;

  if (split_type == MPI_COMM_TYPE_SHARED || split_type == AMPI_COMM_TYPE_HOST) {
    color = CmiPhysicalNodeID(CkMyPe());
  }
  else if (split_type == AMPI_COMM_TYPE_PROCESS) {
    color = CkMyNode();
  }
  else if (split_type == AMPI_COMM_TYPE_WTH) {
    color = CkMyPe();
  }

  return MPI_Comm_split(src, color, key, dest);
}

AMPI_API_IMPL(int, MPI_Comm_free, MPI_Comm *comm)
{
  AMPI_API("AMPI_Comm_free", comm, *comm);
  int ret = MPI_SUCCESS;
  if (*comm != MPI_COMM_NULL) {
    if (*comm != MPI_COMM_WORLD && *comm != MPI_COMM_SELF) {
      ampiParent* parent = getAmpiParent();
      ret = parent->freeUserAttributes(*comm, parent->getAttributes(*comm));
      ampi* ptr = getAmpiInstance(*comm);
      parent->freeCommStruct(*comm);
      ptr->thisProxy[ptr->thisIndex].ckDestroy();
    }
    *comm = MPI_COMM_NULL;
  }
  return ampiErrhandler("AMPI_Comm_free", ret);
}

AMPI_API_IMPL(int, MPI_Comm_test_inter, MPI_Comm comm, int *flag)
{
  AMPI_API("AMPI_Comm_test_inter", comm, flag);
  *flag = getAmpiParent()->isInter(comm);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Comm_remote_size, MPI_Comm comm, int *size)
{
  AMPI_API("AMPI_Comm_remote_size", comm, size);
  *size = getAmpiParent()->getRemoteSize(comm);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Comm_remote_group, MPI_Comm comm, MPI_Group *group)
{
  AMPI_API("AMPI_Comm_remote_group", comm, group);
  *group = getAmpiParent()->getRemoteGroup(comm);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Intercomm_create, MPI_Comm localComm, int localLeader, MPI_Comm peerComm,
                                         int remoteLeader, int tag, MPI_Comm *newintercomm)
{
  AMPI_API("AMPI_Intercomm_create", localComm, localLeader, peerComm, remoteLeader, newintercomm);

#if AMPI_ERROR_CHECKING
  if (getAmpiParent()->isInter(localComm) || getAmpiParent()->isInter(peerComm))
    return ampiErrhandler("AMPI_Intercomm_create", MPI_ERR_COMM);
#endif

  ampi *localPtr = getAmpiInstance(localComm);
  ampi *peerPtr = getAmpiInstance(peerComm);
  int rootIndex = localPtr->getIndexForRank(localLeader);
  int localSize, localRank;

  localSize = localPtr->getSize();
  localRank = localPtr->getRank();

  std::vector<int> remoteVec;

  if (localRank == localLeader) {
    int remoteSize;
    MPI_Status sts;
    std::vector<int> localVec;
    localVec = localPtr->getIndices();
    // local leader exchanges groups with remote leader
    peerPtr->send(tag, peerPtr->getRank(), localVec.data(), localVec.size(), MPI_INT, remoteLeader, peerComm);
    peerPtr->probe(tag, remoteLeader, peerComm, &sts);
    MPI_Get_count(&sts, MPI_INT, &remoteSize);
    remoteVec.resize(remoteSize);
    if (-1==peerPtr->recv(tag, remoteLeader, remoteVec.data(), remoteSize, MPI_INT, peerComm))
      CkAbort("AMPI> Error in MPI_Intercomm_create");

    if (remoteSize==0) {
      AMPI_DEBUG("AMPI> In MPI_Intercomm_create, creating an empty communicator\n");
      *newintercomm = MPI_COMM_NULL;
      return MPI_SUCCESS;
    }
  }

  *newintercomm = localPtr->intercommCreate(remoteVec,rootIndex,localComm);

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Intercomm_merge, MPI_Comm intercomm, int high, MPI_Comm *newintracomm)
{
  AMPI_API("AMPI_Intercomm_merge", intercomm, high, newintracomm);

#if AMPI_ERROR_CHECKING
  if (!getAmpiParent()->isInter(intercomm))
    return ampiErrhandler("AMPI_Intercomm_merge", MPI_ERR_COMM);
#endif

  ampi *ptr = getAmpiInstance(intercomm);
  int lroot, rroot, lrank, lhigh, rhigh, first;
  lroot = ptr->getIndexForRank(0);
  rroot = ptr->getIndexForRemoteRank(0);
  lhigh = high;
  lrank = ptr->getRank();
  first = 0;

  if(lrank==0){
    MPI_Request req = ptr->send(MPI_ATA_TAG, ptr->getRank(), &lhigh, 1, MPI_INT, 0, intercomm, I_SEND);
    if(-1==ptr->recv(MPI_ATA_TAG,0,&rhigh,1,MPI_INT,intercomm))
      CkAbort("AMPI> Error in MPI_Intercomm_create");
    MPI_Wait(&req, MPI_STATUS_IGNORE);

    if((lhigh && rhigh) || (!lhigh && !rhigh)){ // same value: smaller root goes first (first=1 if local goes first)
      first = (lroot < rroot);
    }else{ // different values, then high=false goes first
      first = (lhigh == false);
    }
  }

  ptr->intercommMerge(first, newintracomm);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Abort, MPI_Comm comm, int errorcode)
{
  AMPI_API_INIT("AMPI_Abort", comm, errorcode);
  CkAbort("AMPI: Application called MPI_Abort()!\n");
  return errorcode;
}

AMPI_API_IMPL(int, MPI_Get_count, const MPI_Status *sts, MPI_Datatype dtype, int *count)
{
  AMPI_API("AMPI_Get_count", sts, dtype, count);
  CkDDT_DataType* dttype = getDDT()->getType(dtype);
  int itemsize = dttype->getSize() ;
  if (itemsize == 0) {
    *count = 0;
  } else {
    if (sts->MPI_LENGTH%itemsize == 0) {
      *count = sts->MPI_LENGTH/itemsize;
    } else {
      *count = MPI_UNDEFINED;
    }
  }
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Type_lb, MPI_Datatype dtype, MPI_Aint* displacement)
{
  AMPI_API("AMPI_Type_lb", dtype, displacement);

#if AMPI_ERROR_CHECKING
  int ret = checkData("AMPI_Type_lb", dtype);
  if (ret!=MPI_SUCCESS)
    return ret;
#endif

  *displacement = getDDT()->getLB(dtype);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Type_ub, MPI_Datatype dtype, MPI_Aint* displacement)
{
  AMPI_API("AMPI_Type_ub", dtype, displacement);

#if AMPI_ERROR_CHECKING
  int ret = checkData("AMPI_Type_ub", dtype);
  if (ret!=MPI_SUCCESS)
    return ret;
#endif

  *displacement = getDDT()->getUB(dtype);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Get_address, const void* location, MPI_Aint *address)
{
  AMPI_API("AMPI_Get_address", location, address);
  *address = (MPI_Aint)location;
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Address, void* location, MPI_Aint *address)
{
  AMPI_API("AMPI_Address", location, address);
  return MPI_Get_address(location, address);
}

AMPI_API_IMPL(int, MPI_Status_set_elements, MPI_Status *sts, MPI_Datatype dtype, int count)
{
  AMPI_API("AMPI_Status_set_elements", sts, dtype, count);
  if(sts == MPI_STATUS_IGNORE || sts == MPI_STATUSES_IGNORE)
    return MPI_SUCCESS;

#if AMPI_ERROR_CHECKING
  int ret = checkData("AMPI_Status_set_elements", dtype);
  if (ret!=MPI_SUCCESS)
    return(ret);
#endif

  CkDDT_DataType* dttype = getDDT()->getType(dtype);
  int basesize = dttype->getBaseSize();
  if(basesize==0) basesize = dttype->getSize();
  sts->MPI_LENGTH = basesize * count;
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Status_set_elements_x, MPI_Status *sts, MPI_Datatype dtype, MPI_Count count)
{
  AMPI_API("AMPI_Status_set_elements_x", sts, dtype, count);
  if(sts == MPI_STATUS_IGNORE || sts == MPI_STATUSES_IGNORE)
    return MPI_SUCCESS;

#if AMPI_ERROR_CHECKING
  int ret = checkData("AMPI_Status_set_elements_x", dtype);
  if (ret!=MPI_SUCCESS)
    return(ret);
#endif

  CkDDT_DataType* dttype = getDDT()->getType(dtype);
  int basesize = dttype->getBaseSize();
  if(basesize==0) basesize = dttype->getSize();
  sts->MPI_LENGTH = basesize * count;
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Get_elements, const MPI_Status *sts, MPI_Datatype dtype, int *count)
{
  AMPI_API("AMPI_Get_elements", sts, dtype, count);

#if AMPI_ERROR_CHECKING
    int ret = checkData("AMPI_Type_create_keyval", dtype);
    if (ret!=MPI_SUCCESS)
      return ret;
#endif

  *count = getDDT()->getType(dtype)->getNumBasicElements(sts->MPI_LENGTH);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Get_elements_x, const MPI_Status *sts, MPI_Datatype dtype, MPI_Count *count)
{
  AMPI_API("AMPI_Get_elements_x", sts, dtype, count);
  *count = getDDT()->getType(dtype)->getNumBasicElements(sts->MPI_LENGTH);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Pack, const void *inbuf, int incount, MPI_Datatype dtype,
                             void *outbuf, int outsize, int *position, MPI_Comm comm)
{
  AMPI_API("AMPI_Pack", inbuf, incount, dtype, outbuf, outsize, position, comm);
  CkDDT_DataType* dttype = getDDT()->getType(dtype) ;
  int itemsize = dttype->getSize();
  dttype->serialize((char*)inbuf, ((char*)outbuf)+(*position), incount, outsize, PACK);
  *position += (itemsize*incount);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Unpack, const void *inbuf, int insize, int *position, void *outbuf,
                               int outcount, MPI_Datatype dtype, MPI_Comm comm)
{
  AMPI_API("AMPI_Unpack", inbuf, insize, position, outbuf, outcount, dtype, comm);
  CkDDT_DataType* dttype = getDDT()->getType(dtype) ;
  int itemsize = dttype->getSize();
  dttype->serialize((char*)outbuf, ((char*)inbuf+(*position)), outcount, insize, UNPACK);
  *position += (itemsize*outcount);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Pack_size, int incount, MPI_Datatype datatype, MPI_Comm comm, int *sz)
{
  AMPI_API("AMPI_Pack_size", incount, datatype, comm, sz);
  CkDDT_DataType* dttype = getDDT()->getType(datatype) ;
  *sz = incount*dttype->getSize() ;
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Get_version, int *version, int *subversion)
{
  AMPI_API_INIT("AMPI_Get_version", version, subversion);
  *version = MPI_VERSION;
  *subversion = MPI_SUBVERSION;
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Get_library_version, char *version, int *resultlen)
{
  AMPI_API_INIT("AMPI_Get_library_version", version, resultlen);
  const char *ampiNameStr = "Adaptive MPI ";
  strncpy(version, ampiNameStr, MPI_MAX_LIBRARY_VERSION_STRING);
  strncat(version, CmiCommitID, MPI_MAX_LIBRARY_VERSION_STRING - strlen(version));
  *resultlen = strlen(version);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Get_processor_name, char *name, int *resultlen)
{
  AMPI_API_INIT("AMPI_Get_processor_name", name, resultlen);
  ampiParent *ptr = getAmpiParent();
  sprintf(name,"AMPI_RANK[%d]_WTH[%d]",ptr->thisIndex,ptr->getMyPe());
  *resultlen = strlen(name);
  return MPI_SUCCESS;
}

/* Error handling */
#if defined(USE_STDARG)
void error_handler(MPI_Comm *, int *, ...);
#else
void error_handler ( MPI_Comm *, int * );
#endif

AMPI_API_IMPL(int, MPI_Comm_call_errhandler, MPI_Comm comm, int errorcode)
{
  AMPI_API("AMPI_Comm_call_errhandler", comm, errorcode);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Comm_create_errhandler, MPI_Comm_errhandler_fn *function, MPI_Errhandler *errhandler)
{
  AMPI_API("AMPI_Comm_create_errhandler", function, errhandler);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Comm_set_errhandler, MPI_Comm comm, MPI_Errhandler errhandler)
{
  AMPI_API("AMPI_Comm_set_errhandler", comm, errhandler);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Comm_get_errhandler, MPI_Comm comm, MPI_Errhandler *errhandler)
{
  AMPI_API("AMPI_Comm_get_errhandler", comm, errhandler);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Comm_free_errhandler, MPI_Errhandler *errhandler)
{
  AMPI_API("AMPI_Comm_free_errhandler", errhandler);
  *errhandler = MPI_ERRHANDLER_NULL;
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_File_call_errhandler, MPI_File file, int errorcode)
{
  AMPI_API("AMPI_File_call_errhandler", file, errorcode);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_File_create_errhandler, MPI_File_errhandler_function *function, MPI_Errhandler *errhandler)
{
  AMPI_API("AMPI_File_create_errhandler", function, errhandler);
  return MPI_SUCCESS;
}

#if !CMK_AMPI_WITH_ROMIO
// Disable ROMIO's get_errh.c and set_errh.c when implementing these.
AMPI_API_IMPL(int, MPI_File_set_errhandler, MPI_File file, MPI_Errhandler errhandler)
{
  AMPI_API("AMPI_File_set_errhandler", file, errhandler);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_File_get_errhandler, MPI_File file, MPI_Errhandler *errhandler)
{
  AMPI_API("AMPI_File_get_errhandler", file, errhandler);
  return MPI_SUCCESS;
}
#endif

AMPI_API_IMPL(int, MPI_Errhandler_create, MPI_Handler_function *function, MPI_Errhandler *errhandler)
{
  AMPI_API("AMPI_Errhandler_create", function, errhandler);
  return MPI_Comm_create_errhandler(function, errhandler);
}

AMPI_API_IMPL(int, MPI_Errhandler_set, MPI_Comm comm, MPI_Errhandler errhandler)
{
  AMPI_API("AMPI_Errhandler_set", comm, errhandler);
  return MPI_Comm_set_errhandler(comm, errhandler);
}

AMPI_API_IMPL(int, MPI_Errhandler_get, MPI_Comm comm, MPI_Errhandler *errhandler)
{
  AMPI_API("AMPI_Errhandler_get", comm, errhandler);
  return MPI_Comm_get_errhandler(comm, errhandler);
}

AMPI_API_IMPL(int, MPI_Errhandler_free, MPI_Errhandler *errhandler)
{
  AMPI_API("AMPI_Errhandler_free", errhandler);
  return MPI_Comm_free_errhandler(errhandler);
}

AMPI_API_IMPL(int, MPI_Add_error_code, int errorclass, int *errorcode)
{
  AMPI_API("AMPI_Add_error_code", errorclass, errorcode);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Add_error_class, int *errorclass)
{
  AMPI_API("AMPI_Add_error_class", errorclass);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Add_error_string, int errorcode, const char *errorstring)
{
  AMPI_API("AMPI_Add_error_string", errorcode, errorstring);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Error_class, int errorcode, int *errorclass)
{
  AMPI_API("AMPI_Error_class", errorcode, errorclass);
  *errorclass = errorcode;
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Error_string, int errorcode, char *errorstring, int *resultlen)
{
  AMPI_API("AMPI_Error_string", errorcode, errorstring, resultlen);
  const char *r="";
  switch(errorcode) {
    case MPI_SUCCESS:
      r="MPI_SUCCESS: no errors"; break;
    case MPI_ERR_BUFFER:
      r="MPI_ERR_BUFFER: invalid buffer pointer"; break;
    case MPI_ERR_COUNT:
      r="MPI_ERR_COUNT: invalid count argument"; break;
    case MPI_ERR_TYPE:
      r="MPI_ERR_TYPE: invalid datatype"; break;
    case MPI_ERR_TAG:
      r="MPI_ERR_TAG: invalid tag"; break;
    case MPI_ERR_COMM:
      r="MPI_ERR_COMM: invalid communicator"; break;
    case MPI_ERR_RANK:
      r="MPI_ERR_RANK: invalid rank"; break;
    case MPI_ERR_REQUEST:
      r="MPI_ERR_REQUEST: invalid request (handle)"; break;
    case MPI_ERR_ROOT:
      r="MPI_ERR_ROOT: invalid root"; break;
    case MPI_ERR_GROUP:
      r="MPI_ERR_GROUP: invalid group"; break;
    case MPI_ERR_OP:
      r="MPI_ERR_OP: invalid operation"; break;
    case MPI_ERR_TOPOLOGY:
      r="MPI_ERR_TOPOLOGY: invalid communicator topology"; break;
    case MPI_ERR_DIMS:
      r="MPI_ERR_DIMS: invalid dimension argument"; break;
    case MPI_ERR_ARG:
      r="MPI_ERR_ARG: invalid argument of some other kind"; break;
    case MPI_ERR_TRUNCATE:
      r="MPI_ERR_TRUNCATE: message truncated in receive"; break;
    case MPI_ERR_OTHER:
      r="MPI_ERR_OTHER: known error not in this list"; break;
    case MPI_ERR_INTERN:
      r="MPI_ERR_INTERN: internal MPI (implementation) error"; break;
    case MPI_ERR_IN_STATUS:
      r="MPI_ERR_IN_STATUS: error code in status"; break;
    case MPI_ERR_PENDING:
      r="MPI_ERR_PENDING: pending request"; break;
    case MPI_ERR_ACCESS:
      r="MPI_ERR_ACCESS: invalid access mode"; break;
    case MPI_ERR_AMODE:
      r="MPI_ERR_AMODE: invalid amode argument"; break;
    case MPI_ERR_ASSERT:
      r="MPI_ERR_ASSERT: invalid assert argument"; break;
    case MPI_ERR_BAD_FILE:
      r="MPI_ERR_BAD_FILE: bad file"; break;
    case MPI_ERR_BASE:
      r="MPI_ERR_BASE: invalid base"; break;
    case MPI_ERR_CONVERSION:
      r="MPI_ERR_CONVERSION: error in data conversion"; break;
    case MPI_ERR_DISP:
      r="MPI_ERR_DISP: invalid displacement"; break;
    case MPI_ERR_DUP_DATAREP:
      r="MPI_ERR_DUP_DATAREP: error duplicating data representation"; break;
    case MPI_ERR_FILE_EXISTS:
      r="MPI_ERR_FILE_EXISTS: file exists already"; break;
    case MPI_ERR_FILE_IN_USE:
      r="MPI_ERR_FILE_IN_USE: file in use already"; break;
    case MPI_ERR_FILE:
      r="MPI_ERR_FILE: invalid file"; break;
    case MPI_ERR_INFO_KEY:
      r="MPI_ERR_INFO_KEY: invalid key argument for info object"; break;
    case MPI_ERR_INFO_NOKEY:
      r="MPI_ERR_INFO_NOKEY: unknown key for info object"; break;
    case MPI_ERR_INFO_VALUE:
      r="MPI_ERR_INFO_VALUE: invalid value argument for info object"; break;
    case MPI_ERR_INFO:
      r="MPI_ERR_INFO: invalid info object"; break;
    case MPI_ERR_IO:
      r="MPI_ERR_IO: input/output error"; break;
    case MPI_ERR_KEYVAL:
      r="MPI_ERR_KEYVAL: invalid keyval"; break;
    case MPI_ERR_LOCKTYPE:
      r="MPI_ERR_LOCKTYPE: invalid locktype argument"; break;
    case MPI_ERR_NAME:
      r="MPI_ERR_NAME: invalid name argument"; break;
    case MPI_ERR_NO_MEM:
      r="MPI_ERR_NO_MEM: out of memory"; break;
    case MPI_ERR_NOT_SAME:
      r="MPI_ERR_NOT_SAME: objects are not identical"; break;
    case MPI_ERR_NO_SPACE:
      r="MPI_ERR_NO_SPACE: no space left on device"; break;
    case MPI_ERR_NO_SUCH_FILE:
      r="MPI_ERR_NO_SUCH_FILE: no such file or directory"; break;
    case MPI_ERR_PORT:
      r="MPI_ERR_PORT: invalid port"; break;
    case MPI_ERR_QUOTA:
      r="MPI_ERR_QUOTA: out of quota"; break;
    case MPI_ERR_READ_ONLY:
      r="MPI_ERR_READ_ONLY: file is read only"; break;
    case MPI_ERR_RMA_CONFLICT:
      r="MPI_ERR_RMA_CONFLICT: rma conflict during operation"; break;
    case MPI_ERR_RMA_SYNC:
      r="MPI_ERR_RMA_SYNC: error executing rma sync"; break;
    case MPI_ERR_SERVICE:
      r="MPI_ERR_SERVICE: unknown service name"; break;
    case MPI_ERR_SIZE:
      r="MPI_ERR_SIZE: invalid size argument"; break;
    case MPI_ERR_SPAWN:
      r="MPI_ERR_SPAWN: error in spawning processes"; break;
    case MPI_ERR_UNSUPPORTED_DATAREP:
      r="MPI_ERR_UNSUPPORTED_DATAREP: data representation not supported"; break;
    case MPI_ERR_UNSUPPORTED_OPERATION:
      r="MPI_ERR_UNSUPPORTED_OPERATION: operation not supported"; break;
    case MPI_ERR_WIN:
      r="MPI_ERR_WIN: invalid win argument"; break;
    default:
      r="unknown error";
      *resultlen=strlen(r);
      strcpy(errorstring,r);
      return MPI_ERR_UNKNOWN;
  };
  *resultlen=strlen(r);
  strcpy(errorstring,r);
  return MPI_SUCCESS;
}

/* Group operations */
AMPI_API_IMPL(int, MPI_Comm_group, MPI_Comm comm, MPI_Group *group)
{
  AMPI_API("AMPI_Comm_Group", comm, group);
  *group = getAmpiParent()->comm2group(comm);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Group_union, MPI_Group group1, MPI_Group group2, MPI_Group *newgroup)
{
  AMPI_API("AMPI_Group_union", group1, group2, newgroup);
  ampiParent *ptr = getAmpiParent();
  std::vector<int> vec1 = ptr->group2vec(group1);
  std::vector<int> vec2 = ptr->group2vec(group2);
  std::vector<int> newvec = unionOp(vec1,vec2);
  *newgroup = ptr->saveGroupStruct(newvec);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Group_intersection, MPI_Group group1, MPI_Group group2, MPI_Group *newgroup)
{
  AMPI_API("AMPI_Group_intersection", group1, group2, newgroup);
  ampiParent *ptr = getAmpiParent();
  std::vector<int> vec1 = ptr->group2vec(group1);
  std::vector<int> vec2 = ptr->group2vec(group2);
  std::vector<int> newvec = intersectOp(vec1,vec2);
  *newgroup = ptr->saveGroupStruct(newvec);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Group_difference, MPI_Group group1, MPI_Group group2, MPI_Group *newgroup)
{
  AMPI_API("AMPI_Group_difference", group1, group2, newgroup);
  ampiParent *ptr = getAmpiParent();
  std::vector<int> vec1 = ptr->group2vec(group1);
  std::vector<int> vec2 = ptr->group2vec(group2);
  std::vector<int> newvec = diffOp(vec1,vec2);
  *newgroup = ptr->saveGroupStruct(newvec);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Group_size, MPI_Group group, int *size)
{
  AMPI_API("AMPI_Group_size", group, size);
  *size = (getAmpiParent()->group2vec(group)).size();
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Group_rank, MPI_Group group, int *rank)
{
  AMPI_API("AMPI_Group_rank", group, rank);
  *rank = getAmpiParent()->getRank(group);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Group_translate_ranks, MPI_Group group1, int n, const int *ranks1,
                                              MPI_Group group2, int *ranks2)
{
  AMPI_API("AMPI_Group_translate_ranks", group1, n, ranks1, group2, ranks2);
  ampiParent *ptr = getAmpiParent();
  std::vector<int> vec1 = ptr->group2vec(group1);
  std::vector<int> vec2 = ptr->group2vec(group2);
  translateRanksOp(n, vec1, ranks1, vec2, ranks2);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Group_compare, MPI_Group group1, MPI_Group group2, int *result)
{
  AMPI_API("AMPI_Group_compare", group1, group2, result);
  ampiParent *ptr = getAmpiParent();
  std::vector<int> vec1 = ptr->group2vec(group1);
  std::vector<int> vec2 = ptr->group2vec(group2);
  *result = compareVecOp(vec1, vec2);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Group_incl, MPI_Group group, int n, const int *ranks, MPI_Group *newgroup)
{
  AMPI_API("AMPI_Group_incl", group, n, ranks, newgroup);
  ampiParent *ptr = getAmpiParent();
  std::vector<int> vec = ptr->group2vec(group);
  std::vector<int> newvec = inclOp(n,ranks,vec);
  *newgroup = ptr->saveGroupStruct(newvec);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Group_excl, MPI_Group group, int n, const int *ranks, MPI_Group *newgroup)
{
  AMPI_API("AMPI_Group_excl", group, n, ranks, newgroup);
  ampiParent *ptr = getAmpiParent();
  std::vector<int> vec = ptr->group2vec(group);
  std::vector<int> newvec = exclOp(n,ranks,vec);
  *newgroup = ptr->saveGroupStruct(newvec);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Group_range_incl, MPI_Group group, int n, int ranges[][3], MPI_Group *newgroup)
{
  AMPI_API("AMPI_Group_range_incl", group, n, ranges, newgroup);
  int ret;
  ampiParent *ptr = getAmpiParent();
  std::vector<int> vec = ptr->group2vec(group);
  std::vector<int> newvec = rangeInclOp(n,ranges,vec,&ret);
  if(ret != MPI_SUCCESS){
    *newgroup = MPI_GROUP_EMPTY;
    return ampiErrhandler("AMPI_Group_range_incl", ret);
  }else{
    *newgroup = ptr->saveGroupStruct(newvec);
    return MPI_SUCCESS;
  }
}

AMPI_API_IMPL(int, MPI_Group_range_excl, MPI_Group group, int n, int ranges[][3], MPI_Group *newgroup)
{
  AMPI_API("AMPI_Group_range_excl", group, n, ranges, newgroup);
  int ret;
  ampiParent *ptr = getAmpiParent();
  std::vector<int> vec = ptr->group2vec(group);
  std::vector<int> newvec = rangeExclOp(n,ranges,vec,&ret);
  if(ret != MPI_SUCCESS){
    *newgroup = MPI_GROUP_EMPTY;
    return ampiErrhandler("AMPI_Group_range_excl", ret);
  }else{
    *newgroup = ptr->saveGroupStruct(newvec);
    return MPI_SUCCESS;
  }
}

AMPI_API_IMPL(int, MPI_Group_free, MPI_Group *group)
{
  AMPI_API("AMPI_Group_free", group, *group);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Comm_create, MPI_Comm comm, MPI_Group group, MPI_Comm* newcomm)
{
  AMPI_API("AMPI_Comm_create", comm, group, newcomm);
  int rank_in_group, key, color, zero;
  MPI_Group group_of_comm;

  std::vector<int> vec = getAmpiParent()->group2vec(group);
  if(vec.size()==0){
    AMPI_DEBUG("AMPI> In MPI_Comm_create, creating an empty communicator");
    *newcomm = MPI_COMM_NULL;
    return MPI_SUCCESS;
  }

  if(getAmpiParent()->isInter(comm)){
    /* inter-communicator: create a single new comm. */
    *newcomm = getAmpiInstance(comm)->commCreate(vec, COMM_INTER);
    ampi * unused = getAmpiInstance(comm)->barrier();
  }
  else{
    /* intra-communicator: create comm's for disjoint subgroups,
     * by calculating (color, key) and splitting comm. */
    MPI_Group_rank(group, &rank_in_group);
    if(rank_in_group == MPI_UNDEFINED){
      color = MPI_UNDEFINED;
      key = 0;
    }
    else{
      /* use rank in 'comm' of the 0th rank in 'group'
       * as identical 'color' of all ranks in 'group' */
      MPI_Comm_group(comm, &group_of_comm);
      zero = 0;
      MPI_Group_translate_ranks(group, 1, &zero, group_of_comm, &color);
      key = rank_in_group;
    }
    return MPI_Comm_split(comm, color, key, newcomm);
  }
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Comm_create_group, MPI_Comm comm, MPI_Group group, int tag, MPI_Comm *newcomm)
{
  AMPI_API("AMPI_Comm_create_group", comm, group, tag, newcomm);

  if (group == MPI_GROUP_NULL) {
    *newcomm = MPI_COMM_NULL;
    return MPI_SUCCESS;
  }

#if AMPI_ERROR_CHECKING
  if (!getAmpiParent()->isIntra(comm)) {
    *newcomm = MPI_COMM_NULL;
    return ampiErrhandler("AMPI_Comm_create_group", MPI_ERR_COMM);
  }
  int ret = checkTag("AMPI_Comm_create_group", tag);
  if (ret != MPI_SUCCESS) {
     *newcomm = MPI_COMM_NULL;
     return ampiErrhandler("AMPI_Comm_create_group", ret);
  }
#endif

  int rank, groupRank, groupSize;
  MPI_Group parentGroup;
  MPI_Comm_rank(comm, &rank);
  MPI_Group_rank(group, &groupRank);
  MPI_Group_size(group, &groupSize);
  if (groupRank == MPI_UNDEFINED) {
    *newcomm = MPI_COMM_NULL;
    return MPI_SUCCESS;
  }
  MPI_Comm_dup(MPI_COMM_SELF, newcomm);

  std::vector<int> groupPids(groupSize), pids(groupSize, 0);
  std::iota(groupPids.begin(), groupPids.end(), 0);
  MPI_Comm_group(comm, &parentGroup);
  MPI_Group_translate_ranks(group, groupSize, groupPids.data(), parentGroup, pids.data());
  MPI_Group_free(&parentGroup);

  MPI_Comm commOld, tmpInter;
  for (int i=0; i<groupSize; i*=2) {
    int groupId = (i == 0) ? groupRank : groupRank/i;
    commOld = *newcomm;

    if (groupId % 2 == 0) {
      if ((groupId+1)*i < groupSize) {
        MPI_Intercomm_create(*newcomm, 0, comm, pids[(groupId+1)*i], tag, &tmpInter);
        MPI_Intercomm_merge(tmpInter, 0, newcomm);
      }
    }
    else {
      MPI_Intercomm_create(*newcomm, 0, comm, pids[(groupId+1)*i], tag, &tmpInter);
      MPI_Intercomm_merge(tmpInter, 1, newcomm);
    }

    if (*newcomm != commOld) {
      MPI_Comm_free(&tmpInter);
      MPI_Comm_free(&commOld);
    }
  }

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Comm_set_name, MPI_Comm comm, const char *comm_name)
{
  AMPI_API("AMPI_Comm_set_name", comm, comm_name);
  getAmpiInstance(comm)->setCommName(comm_name);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Comm_get_name, MPI_Comm comm, char *comm_name, int *resultlen)
{
  AMPI_API("AMPI_Comm_get_name", comm, comm_name, resultlen);
  getAmpiInstance(comm)->getCommName(comm_name, resultlen);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Comm_set_info, MPI_Comm comm, MPI_Info info)
{
  AMPI_API("AMPI_Comm_set_info", comm, info);
  /* FIXME: no-op implementation */
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Comm_get_info, MPI_Comm comm, MPI_Info *info)
{
  AMPI_API("AMPI_Comm_get_info", comm, info);
  /* FIXME: no-op implementation */
  *info = MPI_INFO_NULL;
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Comm_create_keyval, MPI_Comm_copy_attr_function *copy_fn,
                                           MPI_Comm_delete_attr_function *delete_fn,
                                           int *keyval, void* extra_state)
{
  AMPI_API("AMPI_Comm_create_keyval", copy_fn, delete_fn, keyval, extra_state);
  int ret = getAmpiParent()->createKeyval(copy_fn,delete_fn,keyval,extra_state);
  return ampiErrhandler("AMPI_Comm_create_keyval", ret);
}

AMPI_API_IMPL(int, MPI_Comm_free_keyval, int *keyval)
{
  AMPI_API("AMPI_Comm_free_keyval", keyval, *keyval);
  int ret = getAmpiParent()->freeKeyval(*keyval);
  *keyval = MPI_KEYVAL_INVALID;
  return ampiErrhandler("AMPI_Comm_free_keyval", ret);
}

AMPI_API_IMPL(int, MPI_Comm_set_attr, MPI_Comm comm, int keyval, void* attribute_val)
{
  AMPI_API("AMPI_Comm_set_attr", comm, keyval, attribute_val);
  ampiParent *parent = getAmpiParent();
  int ret = parent->setAttrComm(comm, parent->getAttributes(comm), keyval, attribute_val);
  return ampiErrhandler("AMPI_Comm_set_attr", ret);
}

AMPI_API_IMPL(int, MPI_Comm_get_attr, MPI_Comm comm, int keyval, void *attribute_val, int *flag)
{
  AMPI_API("AMPI_Comm_get_attr", comm, keyval, attribute_val, flag);
  ampiParent *parent = getAmpiParent();
  int ret = parent->getAttrComm(comm, parent->getAttributes(comm), keyval, attribute_val, flag);
  return ampiErrhandler("AMPI_Comm_get_attr", ret);
}

AMPI_API_IMPL(int, MPI_Comm_delete_attr, MPI_Comm comm, int keyval)
{
  AMPI_API("AMPI_Comm_delete_attr", comm, keyval);
  ampiParent *parent = getAmpiParent();
  int ret = parent->deleteAttr(comm, parent->getAttributes(comm), keyval);
  return ampiErrhandler("AMPI_Comm_delete_attr", ret);
}

AMPI_API_IMPL(int, MPI_Keyval_create, MPI_Copy_function *copy_fn, MPI_Delete_function *delete_fn,
                                      int *keyval, void* extra_state)
{
  AMPI_API("AMPI_Keyval_create", copy_fn, delete_fn, keyval, extra_state);
  return MPI_Comm_create_keyval(copy_fn, delete_fn, keyval, extra_state);
}

AMPI_API_IMPL(int, MPI_Keyval_free, int *keyval)
{
  AMPI_API("AMPI_Keyval_free", keyval, *keyval);
  return MPI_Comm_free_keyval(keyval);
}

AMPI_API_IMPL(int, MPI_Attr_put, MPI_Comm comm, int keyval, void* attribute_val)
{
  AMPI_API("AMPI_Attr_put", comm, keyval, attribute_val);
  return MPI_Comm_set_attr(comm, keyval, attribute_val);
}

AMPI_API_IMPL(int, MPI_Attr_get, MPI_Comm comm, int keyval, void *attribute_val, int *flag)
{
  AMPI_API("AMPI_Attr_get", comm, keyval, attribute_val, flag);
  return MPI_Comm_get_attr(comm, keyval, attribute_val, flag);
}

AMPI_API_IMPL(int, MPI_Attr_delete, MPI_Comm comm, int keyval)
{
  AMPI_API("AMPI_Attr_delete", comm, keyval);
  return MPI_Comm_delete_attr(comm, keyval);
}

AMPI_API_IMPL(int, MPI_Cart_map, MPI_Comm comm, int ndims, const int *dims,
                                 const int *periods, int *newrank)
{
  AMPI_API("AMPI_Cart_map", comm, ndims, dims, periods, newrank);

  ampi* ptr = getAmpiInstance(comm);
  int nranks;

  if (ndims == 0) {
    nranks = 1;
  } else {
    nranks = dims[0];
    for (int i=1; i<ndims; i++) {
      nranks *= dims[i];
    }
  }

  int rank = ptr->getRank();
  if (rank < nranks) {
    *newrank = rank;
  } else {
    *newrank = MPI_UNDEFINED;
  }
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Graph_map, MPI_Comm comm, int nnodes, const int *index,
                                  const int *edges, int *newrank)
{
  AMPI_API("AMPI_Graph_map", comm, nnodes, index, edges, newrank);

  ampi* ptr = getAmpiInstance(comm);

  if (ptr->getRank() < nnodes) {
    *newrank = ptr->getRank();
  } else {
    *newrank = MPI_UNDEFINED;
  }
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Cart_create, MPI_Comm comm_old, int ndims, const int *dims,
                                    const int *periods, int reorder, MPI_Comm *comm_cart)
{
  AMPI_API("AMPI_Cart_create", comm_old, ndims, dims, periods, reorder, comm_cart);

  /* Create new cartesian communicator. No attention is being paid to mapping
     virtual processes to processors, which ideally should be handled by the
     load balancer with input from virtual topology information.

     No reorder done here. reorder input is ignored, but still stored in the
     communicator with other VT info.
   */

  int newrank;
  MPI_Cart_map(comm_old, ndims, dims, periods, &newrank);//no change in rank

  ampiParent *ptr = getAmpiParent();
  std::vector<int> vec = ptr->group2vec(ptr->comm2group(comm_old));
  *comm_cart = getAmpiInstance(comm_old)->cartCreate(vec, ndims, dims);

  if (*comm_cart != MPI_COMM_NULL) {
    ampiCommStruct &c = getAmpiParent()->getCommStruct(*comm_cart);
    ampiTopology *topo = c.getTopology();
    topo->setndims(ndims);
    std::vector<int> dimsv(dims, dims+ndims), periodsv(periods, periods+ndims), nborsv;
    topo->setdims(dimsv);
    topo->setperiods(periodsv);
    getAmpiInstance(*comm_cart)->findNeighbors(*comm_cart, newrank, nborsv);
    topo->setnbors(nborsv);
  }

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Graph_create, MPI_Comm comm_old, int nnodes, const int *index,
                                     const int *edges, int reorder, MPI_Comm *comm_graph)
{
  AMPI_API("AMPI_Graph_create", comm_old, nnodes, index, edges, reorder, comm_graph);

  if (nnodes == 0) {
    *comm_graph = MPI_COMM_NULL;
    return MPI_SUCCESS;
  }

  /* No mapping done */
  int newrank;
  MPI_Graph_map(comm_old, nnodes, index, edges, &newrank);

  ampiParent *ptr = getAmpiParent();
  std::vector<int> vec = ptr->group2vec(ptr->comm2group(comm_old));
  *comm_graph = getAmpiInstance(comm_old)->commCreate(vec, COMM_GRAPH);
  ampiTopology &topo = *getAmpiParent()->getCommStruct(*comm_graph).getTopology();

  std::vector<int> index_(index, index+nnodes), edges_, nborsv;
  topo.setnvertices(nnodes);
  topo.setindex(index_);

  for (int i = 0; i < index[nnodes - 1]; i++)
    edges_.push_back(edges[i]);
  topo.setedges(edges_);

  getAmpiInstance(*comm_graph)->findNeighbors(*comm_graph, newrank, nborsv);
  topo.setnbors(nborsv);

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Dist_graph_create_adjacent, MPI_Comm comm_old, int indegree, const int sources[],
                                                   const int sourceweights[], int outdegree,
                                                   const int destinations[], const int destweights[],
                                                   MPI_Info info, int reorder, MPI_Comm *comm_dist_graph)
{
  AMPI_API("AMPI_Dist_graph_create_adjacent", comm_old, indegree, sources, sourceweights, outdegree, destinations, destweights, info, reorder, comm_dist_graph);

#if AMPI_ERROR_CHECKING
  if (indegree < 0 || outdegree < 0) {
    return ampiErrhandler("AMPI_Dist_graph_create_adjacent", MPI_ERR_TOPOLOGY);
  }
  for (int i=0; i<indegree; i++) {
    if (sources[i] < 0) {
      return ampiErrhandler("AMPI_Dist_graph_create_adjacent", MPI_ERR_TOPOLOGY);
    }
  }
  for (int i=0; i<outdegree; i++) {
    if (destinations[i] < 0) {
      return ampiErrhandler("AMPI_Dist_graph_create_adjacent", MPI_ERR_TOPOLOGY);
    }
  }
#endif

  ampiParent *ptr = getAmpiParent();
  std::vector<int> vec = ptr->group2vec(ptr->comm2group(comm_old));
  *comm_dist_graph = getAmpiInstance(comm_old)->commCreate(vec, COMM_DIST_GRAPH);
  ampiCommStruct &c = getAmpiParent()->getCommStruct(*comm_dist_graph);
  ampiTopology *topo = c.getTopology();

  topo->setInDegree(indegree);
  topo->setOutDegree(outdegree);

  topo->setAreSourcesWeighted(sourceweights != MPI_UNWEIGHTED);
  if (topo->areSourcesWeighted()) {
    std::vector<int> tmpSourceWeights(sourceweights, sourceweights+indegree);
    topo->setSourceWeights(tmpSourceWeights);
  }

  topo->setAreDestsWeighted(destweights != MPI_UNWEIGHTED);
  if (topo->areDestsWeighted()) {
    std::vector<int> tmpDestWeights(destweights, destweights+outdegree);
    topo->setDestWeights(tmpDestWeights);
  }

  std::vector<int> tmpSources(sources, sources+indegree);
  topo->setSources(tmpSources);

  std::vector<int> tmpDestinations(destinations, destinations+outdegree);
  topo->setDestinations(tmpDestinations);

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Dist_graph_create, MPI_Comm comm_old, int n, const int sources[], const int degrees[],
                                          const int destinations[], const int weights[], MPI_Info info,
                                          int reorder, MPI_Comm *comm_dist_graph)
{
  AMPI_API("AMPI_Dist_graph_create", comm_old, n, sources, degrees, destinations, weights, info, reorder, comm_dist_graph);

#if AMPI_ERROR_CHECKING
    if (n < 0) {
      return ampiErrhandler("AMPI_Dist_graph_create", MPI_ERR_TOPOLOGY);
    }
    int counter = 0;
    for (int i=0; i<n; i++) {
      if ((sources[i] < 0) || (degrees[i] < 0)) {
        return ampiErrhandler("AMPI_Dist_graph_create", MPI_ERR_TOPOLOGY);
      }
      for (int j=0; j<degrees[i]; j++) {
        if ((destinations[counter] < 0) || (weights != MPI_UNWEIGHTED && weights[counter] < 0)) {
          return ampiErrhandler("AMPI_Dist_graph_create", MPI_ERR_TOPOLOGY);
        }
        counter++;
      }
    }
#endif

  ampiParent *ptr = getAmpiParent();
  std::vector<int> vec = ptr->group2vec(ptr->comm2group(comm_old));
  *comm_dist_graph = getAmpiInstance(comm_old)->commCreate(vec, COMM_DIST_GRAPH);
  ampiCommStruct &c = getAmpiParent()->getCommStruct(*comm_dist_graph);
  ampiTopology *topo = c.getTopology();

  int p = c.getSize();

  std::vector<int> edgeListIn(p, 0);
  std::vector<int> edgeListOut(p, 0);
  std::vector<std::vector<int> > edgeMatrixIn(p);
  std::vector<std::vector<int> > edgeMatrixOut(p);

  for (int i=0; i<p; i++) {
    std::vector<int> tmpVector(p, 0);
    edgeMatrixIn[i] = tmpVector;
    edgeMatrixOut[i] = tmpVector;
  }

  int index = 0;
  for (int i=0; i<n; i++) {
    for (int j=0; j<degrees[i]; j++) {
      edgeMatrixOut[ sources[i] ][ edgeListOut[sources[i]]++ ] = destinations[index];
      edgeMatrixIn[ destinations[index] ][ edgeListIn[destinations[index]]++ ] = sources[i];
      index++;
    }
  }

  std::vector<int> edgeCount(2*p);
  std::vector<int> totalcount(2);
  int sends = 0;
  for (int i=0; i<p; i++) {
    if (edgeListIn[i] > 0) {
      edgeCount[2*i] = 1;
      sends++;
    }
    else {
      edgeCount[2*i] = 0;
    }
    if (edgeListOut[i] > 0) {
      edgeCount[2*i+1] = 1;
      sends++;
    }
    else {
      edgeCount[2*i+1] = 0;
    }
  }

  // Compute total number of ranks with incoming or outgoing edges for each rank
  MPI_Reduce_scatter_block(edgeCount.data(), totalcount.data(), 2, MPI_INT, MPI_SUM, comm_old);

  std::vector<MPI_Request> requests(sends, MPI_REQUEST_NULL);
  int count = 0;
  for (int i=0; i<p; i++) {
    if (edgeListIn[i] > 0) {
      if (edgeListIn[i] == p) {
        edgeMatrixIn[i].push_back(1);
      }
      else {
        edgeMatrixIn[i][edgeListIn[i]] = 1;
      }
      MPI_Isend(edgeMatrixIn[i].data(), edgeListIn[i]+1, MPI_INT, i, 0, comm_old, &requests[count++]);
    }
    if (edgeListOut[i] > 0) {
      if (edgeListOut[i] == p) {
        edgeMatrixOut[i].push_back(-1);
      }
      else {
        edgeMatrixOut[i][edgeListOut[i]] = -1;
      }
      MPI_Isend(edgeMatrixOut[i].data(), edgeListOut[i]+1, MPI_INT, i, 0, comm_old, &requests[count++]);
    }
  }

  // Receive all non-local incoming and outgoing edges
  int numEdges;
  MPI_Status status;
  std::vector<int> saveSources, saveDestinations;
  for (int i=0; i<2; i++) {
    for (int j=0; j<totalcount[i]; j++) {
      MPI_Probe(MPI_ANY_SOURCE, MPI_ANY_TAG, comm_old, &status);
      MPI_Get_count(&status, MPI_INT, &numEdges);
      std::vector<int> saveEdges(numEdges);
      MPI_Recv(saveEdges.data(), numEdges, MPI_INT, status.MPI_SOURCE, 0, comm_old, MPI_STATUS_IGNORE);

      if (saveEdges[numEdges-1] > 0) {
        for (int k=0; k<numEdges-1; k++) {
          saveSources.push_back(saveEdges[k]);
        }
      }
      else {
        for (int k=0; k<numEdges-1; k++) {
          saveDestinations.push_back(saveEdges[k]);
        }
      }
    }
  }

  topo->setDestinations(saveDestinations);
  topo->setSources(saveSources);
  topo->setOutDegree(saveDestinations.size());
  topo->setInDegree(saveSources.size());

  topo->setAreSourcesWeighted(weights != MPI_UNWEIGHTED);
  topo->setAreDestsWeighted(weights != MPI_UNWEIGHTED);
  if (topo->areSourcesWeighted()) {
    std::vector<int> tmpWeights(weights, weights+n);
    topo->setSourceWeights(tmpWeights);
    topo->setDestWeights(tmpWeights);
  }

  ptr = ptr->waitall(sends, requests.data());
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Topo_test, MPI_Comm comm, int *status)
{
  AMPI_API("AMPI_Topo_test", comm, status);

  ampiParent *ptr = getAmpiParent();

  if (ptr->isCart(comm))
    *status = MPI_CART;
  else if (ptr->isGraph(comm))
    *status = MPI_GRAPH;
  else if (ptr->isDistGraph(comm))
    *status = MPI_DIST_GRAPH;
  else *status = MPI_UNDEFINED;

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Cartdim_get, MPI_Comm comm, int *ndims)
{
  AMPI_API("AMPI_Cartdim_get", comm, ndims);

#if AMPI_ERROR_CHECKING
  if (!getAmpiParent()->isCart(comm))
    return ampiErrhandler("AMPI_Cartdim_get", MPI_ERR_TOPOLOGY);
#endif

  *ndims = getAmpiParent()->getCommStruct(comm).getTopology()->getndims();

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Cart_get, MPI_Comm comm, int maxdims, int *dims, int *periods, int *coords)
{
  int i, ndims;

  AMPI_API("AMPI_Cart_get", comm, maxdims, dims, periods, coords);

#if AMPI_ERROR_CHECKING
  if (!getAmpiParent()->isCart(comm))
    return ampiErrhandler("AMPI_Cart_get", MPI_ERR_TOPOLOGY);
#endif

  ampiCommStruct &c = getAmpiParent()->getCommStruct(comm);
  ampiTopology *topo = c.getTopology();
  ndims = topo->getndims();
  int rank = getAmpiInstance(comm)->getRank();

  const std::vector<int> &dims_ = topo->getdims();
  const std::vector<int> &periods_ = topo->getperiods();

  for (i = 0; i < maxdims; i++) {
    dims[i] = dims_[i];
    periods[i] = periods_[i];
  }

  for (i = ndims - 1; i >= 0; i--) {
    if (i < maxdims)
      coords[i] = rank % dims_[i];
    rank = (int) (rank / dims_[i]);
  }

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Cart_rank, MPI_Comm comm, const int *coords, int *rank)
{
  AMPI_API("AMPI_Cart_rank", comm, coords, rank);

#if AMPI_ERROR_CHECKING
  if (!getAmpiParent()->isCart(comm))
    return ampiErrhandler("AMPI_Cart_rank", MPI_ERR_TOPOLOGY);
#endif

  ampiCommStruct &c = getAmpiParent()->getCommStruct(comm);
  ampiTopology *topo = c.getTopology();
  int ndims = topo->getndims();
  const std::vector<int> &dims = topo->getdims();
  const std::vector<int> &periods = topo->getperiods();

  //create a copy of coords since we are not allowed to modify it
  std::vector<int> ncoords(coords, coords+ndims);

  int prod = 1;
  int r = 0;

  for (int i = ndims - 1; i >= 0; i--) {
    if ((ncoords[i] < 0) || (ncoords[i] >= dims[i])) {
      if (periods[i] != 0) {
        if (ncoords[i] > 0) {
          ncoords[i] %= dims[i];
        } else {
          while (ncoords[i] < 0) ncoords[i]+=dims[i];
        }
      }
    }
    r += prod * ncoords[i];
    prod *= dims[i];
  }

  *rank = r;

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Cart_coords, MPI_Comm comm, int rank, int maxdims, int *coords)
{
  AMPI_API("AMPI_Cart_coords", comm, rank, maxdims, coords);

#if AMPI_ERROR_CHECKING
  if (!getAmpiParent()->isCart(comm))
    return ampiErrhandler("AMPI_Cart_coorts", MPI_ERR_TOPOLOGY);
#endif

  ampiCommStruct &c = getAmpiParent()->getCommStruct(comm);
  ampiTopology *topo = c.getTopology();
  int ndims = topo->getndims();
  const std::vector<int> &dims = topo->getdims();

  for (int i = ndims - 1; i >= 0; i--) {
    if (i < maxdims)
      coords[i] = rank % dims[i];
    rank = (int) (rank / dims[i]);
  }

  return MPI_SUCCESS;
}

// Offset coords[direction] by displacement, and set the rank that
// results
static void cart_clamp_coord(MPI_Comm comm, const std::vector<int> &dims,
                             const std::vector<int> &periodicity, int *coords,
                             int direction, int displacement, int *rank_out)
{
  int base_coord = coords[direction];
  coords[direction] += displacement;

  if (periodicity[direction] != 0) {
    while (coords[direction] < 0)
      coords[direction] += dims[direction];
    while (coords[direction] >= dims[direction])
      coords[direction] -= dims[direction];
  }

  if (coords[direction]<0 || coords[direction]>= dims[direction])
    *rank_out = MPI_PROC_NULL;
  else
    MPI_Cart_rank(comm, coords, rank_out);

  coords[direction] = base_coord;
}

AMPI_API_IMPL(int, MPI_Cart_shift, MPI_Comm comm, int direction, int disp,
                                   int *rank_source, int *rank_dest)
{
  AMPI_API("AMPI_Cart_shift", comm, direction, disp, rank_source, rank_dest);

#if AMPI_ERROR_CHECKING
  if (!getAmpiParent()->isCart(comm))
    return ampiErrhandler("AMPI_Cart_shift", MPI_ERR_TOPOLOGY);
#endif

  ampiCommStruct &c = getAmpiParent()->getCommStruct(comm);
  ampiTopology *topo = c.getTopology();
  int ndims = topo->getndims();

#if AMPI_ERROR_CHECKING
  if ((direction < 0) || (direction >= ndims))
    return ampiErrhandler("AMPI_Cart_shift", MPI_ERR_DIMS);
#endif

  const std::vector<int> &dims = topo->getdims();
  const std::vector<int> &periods = topo->getperiods();
  std::vector<int> coords(ndims);

  int mype = getAmpiInstance(comm)->getRank();
  MPI_Cart_coords(comm, mype, ndims, &coords[0]);

  cart_clamp_coord(comm, dims, periods, &coords[0], direction,  disp, rank_dest);
  cart_clamp_coord(comm, dims, periods, &coords[0], direction, -disp, rank_source);

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Graphdims_get, MPI_Comm comm, int *nnodes, int *nedges)
{
  AMPI_API("AMPI_Graphdim_get", comm, nnodes, nedges);

  ampiCommStruct &c = getAmpiParent()->getCommStruct(comm);
  ampiTopology *topo = c.getTopology();
  *nnodes = topo->getnvertices();
  const std::vector<int> &index = topo->getindex();
  *nedges = index[(*nnodes) - 1];

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Graph_get, MPI_Comm comm, int maxindex, int maxedges, int *index, int *edges)
{
  AMPI_API("AMPI_Graph_get", comm, maxindex, maxedges, index, edges);

#if AMPI_ERROR_CHECKING
  if (!getAmpiParent()->isGraph(comm))
    return ampiErrhandler("AMPI_Graph_get", MPI_ERR_TOPOLOGY);
#endif

  ampiCommStruct &c = getAmpiParent()->getCommStruct(comm);
  ampiTopology *topo = c.getTopology();
  const std::vector<int> &index_ = topo->getindex();
  const std::vector<int> &edges_ = topo->getedges();

  if (maxindex > index_.size())
    maxindex = index_.size();

  int i;
  for (i = 0; i < maxindex; i++)
    index[i] = index_[i];

  for (i = 0; i < maxedges; i++)
    edges[i] = edges_[i];

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Graph_neighbors_count, MPI_Comm comm, int rank, int *nneighbors)
{
  AMPI_API("AMPI_Graph_neighbors_count", comm, rank, nneighbors);

#if AMPI_ERROR_CHECKING
  if (!getAmpiParent()->isGraph(comm))
    return ampiErrhandler("AMPI_Graph_neighbors_count", MPI_ERR_TOPOLOGY);
#endif

  ampiCommStruct &c = getAmpiParent()->getCommStruct(comm);
  ampiTopology *topo = c.getTopology();
  const std::vector<int> &index = topo->getindex();

#if AMPI_ERROR_CHECKING
  if ((rank >= index.size()) || (rank < 0))
    return ampiErrhandler("AMPI_Graph_neighbors_count", MPI_ERR_RANK);
#endif

  if (rank == 0)
    *nneighbors = index[rank];
  else
    *nneighbors = index[rank] - index[rank - 1];

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Graph_neighbors, MPI_Comm comm, int rank, int maxneighbors, int *neighbors)
{
  AMPI_API("AMPI_Graph_neighbors", comm, rank, maxneighbors, neighbors);

#if AMPI_ERROR_CHECKING
  if (!getAmpiParent()->isGraph(comm))
    return ampiErrhandler("AMPI_Graph_neighbors", MPI_ERR_TOPOLOGY);
#endif

  ampiCommStruct &c = getAmpiParent()->getCommStruct(comm);
  ampiTopology *topo = c.getTopology();
  const std::vector<int> &index = topo->getindex();
  const std::vector<int> &edges = topo->getedges();

  int numneighbors = (rank == 0) ? index[rank] : index[rank] - index[rank - 1];
  if (maxneighbors > numneighbors)
    maxneighbors = numneighbors;

#if AMPI_ERROR_CHECKING
  if (maxneighbors < 0)
    return ampiErrhandler("AMPI_Graph_neighbors", MPI_ERR_ARG);
  if ((rank >= index.size()) || (rank < 0))
    return ampiErrhandler("AMPI_Graph_neighbors", MPI_ERR_RANK);
#endif

  if (rank == 0) {
    for (int i = 0; i < maxneighbors; i++)
      neighbors[i] = edges[i];
  } else {
    for (int i = 0; i < maxneighbors; i++)
      neighbors[i] = edges[index[rank - 1] + i];
  }
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Dist_graph_neighbors_count, MPI_Comm comm, int *indegree, int *outdegree, int *weighted)
{
  AMPI_API("AMPI_Dist_graph_neighbors_count", comm, indegree, outdegree, weighted);

#if AMPI_ERROR_CHECKING
  if (!getAmpiParent()->isDistGraph(comm)) {
    return ampiErrhandler("AMPI_Dist_graph_neighbors_count", MPI_ERR_TOPOLOGY);
  }
#endif

  ampiParent *ptr = getAmpiParent();
  ampiCommStruct &c = ptr->getCommStruct(comm);
  ampiTopology *topo = c.getTopology();
  *indegree = topo->getInDegree();
  *outdegree = topo->getOutDegree();
  *weighted = topo->areSourcesWeighted() ? 1 : 0;

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Dist_graph_neighbors, MPI_Comm comm, int maxindegree, int sources[], int sourceweights[],
                                             int maxoutdegree, int destinations[], int destweights[])
{
  AMPI_API("AMPI_Dist_graph_neighbors", comm, maxindegree, sources, sourceweights, maxoutdegree, destinations, destweights);

#if AMPI_ERROR_CHECKING
  if (!getAmpiParent()->isDistGraph(comm)) {
    return ampiErrhandler("AMPI_Dist_graph_neighbors", MPI_ERR_TOPOLOGY);
  }
  if ((maxindegree < 0) || (maxoutdegree < 0)) {
    return ampiErrhandler("AMPI_Dist_graph_neighbors", MPI_ERR_TOPOLOGY);
  }
#endif

  ampiParent *ptr = getAmpiParent();
  ampiCommStruct &c = ptr->getCommStruct(comm);
  ampiTopology *topo = c.getTopology();

  const std::vector<int> &tmpSources = topo->getSources();
  const std::vector<int> &tmpSourceWeights = topo->getSourceWeights();
  const std::vector<int> &tmpDestinations = topo->getDestinations();
  const std::vector<int> &tmpDestWeights = topo->getDestWeights();

  maxindegree = std::min(maxindegree, static_cast<int>(tmpSources.size()));
  maxoutdegree = std::min(maxoutdegree, static_cast<int>(tmpDestinations.size()));

  for (int i=0; i<maxindegree; i++) {
    sources[i] = tmpSources[i];
  }
  for (int i=0; i<maxoutdegree; i++) {
    destinations[i] = tmpDestinations[i];
  }

  if (topo->areSourcesWeighted()) {
    for (int i=0; i<maxindegree; i++) {
      sourceweights[i] = tmpSourceWeights[i];
    }
    for (int i=0; i<maxoutdegree; i++) {
      destweights[i] = tmpDestWeights[i];
    }
  }
  else {
    sourceweights = NULL;
    destweights = NULL;
  }

  return MPI_SUCCESS;
}

/* Used by MPI_Cart_create & MPI_Graph_create */
void ampi::findNeighbors(MPI_Comm comm, int rank, std::vector<int>& neighbors) const noexcept {
  int max_neighbors = 0;
  ampiParent *ptr = getAmpiParent();
  if (ptr->isGraph(comm)) {
    MPI_Graph_neighbors_count(comm, rank, &max_neighbors);
    neighbors.resize(max_neighbors);
    MPI_Graph_neighbors(comm, rank, max_neighbors, &neighbors[0]);
  }
  else if (ptr->isCart(comm)) {
    int num_dims;
    MPI_Cartdim_get(comm, &num_dims);
    max_neighbors = 2*num_dims;
    for (int i=0; i<max_neighbors; i++) {
      int src, dest;
      MPI_Cart_shift(comm, i/2, (i%2==0)?1:-1, &src, &dest);
      if (dest != MPI_PROC_NULL)
        neighbors.push_back(dest);
    }
  }
}

/* Factorization code by Orion. Idea thrashed out by Orion and Prakash */

/**
  Return the integer "d'th root of n"-- the largest
  integer r such that
  r^d <= n
 */
int integerRoot(int n,int d) noexcept {
  double epsilon=0.001; /* prevents roundoff in "floor" */
  return (int)floor(pow(n+epsilon,1.0/d));
}

/**
  Factorize "n" into "d" factors, stored in "dims[0..d-1]".
  All the factors must be greater than or equal to m.
  The factors are chosen so that they are all as near together
  as possible (technically, chosen so that the increasing-size
  ordering is lexicagraphically as large as possible).
 */

bool factors(int n, int d, int *dims, int m) noexcept {
  if (d==1)
  { /* Base case */
    if (n>=m) { /* n is an acceptable factor */
      dims[0]=n;
      return true;
    }
  }
  else { /* induction case */
    int k_up=integerRoot(n,d);
    for (int k=k_up;k>=m;k--) {
      if (n%k==0) { /* k divides n-- try it as a factor */
        dims[0]=k;
        if (factors(n/k,d-1,&dims[1],k))
          return true;
      }
    }
  }
  /* If we fall out here, there were no factors available */
  return false;
}

AMPI_API_IMPL(int, MPI_Dims_create, int nnodes, int ndims, int *dims)
{
  AMPI_API("AMPI_Dims_create", nnodes, ndims, dims);

  int i, n, d;

  n = nnodes;
  d = ndims;

  for (i = 0; i < ndims; i++) {
    if (dims[i] != 0) {
      if (n % dims[i] != 0) {
        return ampiErrhandler("AMPI_Dims_create", MPI_ERR_DIMS);
      } else {
        n = n / dims[i];
        d--;
      }
    }
  }

  if(d > 0) {
    std::vector<int> pdims(d);

    if (!factors(n, d, &pdims[0], 1))
      CkAbort("MPI_Dims_create: factorization failed!\n");

    int j = 0;
    for (i = 0; i < ndims; i++) {
      if (dims[i] == 0) {
        dims[i] = pdims[j];
        j++;
      }
    }

    // Sort the factors in non-increasing order.
    // Bubble sort because dims is always small.
    for (int i=0; i<d-1; i++) {
      for (int j=i+1; j<d; j++) {
        if (dims[j] > dims[i]) {
          int tmp = dims[i];
          dims[i] = dims[j];
          dims[j] = tmp;
        }
      }
    }
  }

  return MPI_SUCCESS;
}

/* Implemented with call to MPI_Comm_Split. Color and key are single integer
   encodings of the lost and preserved dimensions, respectively,
   of the subgraphs.
 */
AMPI_API_IMPL(int, MPI_Cart_sub, MPI_Comm comm, const int *remain_dims, MPI_Comm *newcomm)
{
  AMPI_API("AMPI_Cart_sub", comm, remain_dims, newcomm);

  int i, ndims;
  int color = 1, key = 1;

#if AMPI_ERROR_CHECKING
  if (!getAmpiParent()->isCart(comm))
    return ampiErrhandler("AMPI_Cart_sub", MPI_ERR_TOPOLOGY);
#endif

  int rank = getAmpiInstance(comm)->getRank();
  ampiCommStruct &c = getAmpiParent()->getCommStruct(comm);
  ampiTopology *topo = c.getTopology();
  ndims = topo->getndims();
  const std::vector<int> &dims = topo->getdims();
  int num_remain_dims = 0;

  std::vector<int> coords(ndims);
  MPI_Cart_coords(comm, rank, ndims, coords.data());

  for (i = 0; i < ndims; i++) {
    if (remain_dims[i]) {
      /* key single integer encoding*/
      key = key * dims[i] + coords[i];
      num_remain_dims++;
    }
    else {
      /* color */
      color = color * dims[i] + coords[i];
    }
  }

  if (num_remain_dims == 0) {
    *newcomm = getAmpiInstance(comm)->cartCreate0D();
    return MPI_SUCCESS;
  }

  getAmpiInstance(comm)->split(color, key, newcomm, COMM_CART);

  ampiCommStruct &newc = getAmpiParent()->getCommStruct(*newcomm);
  ampiTopology *newtopo = newc.getTopology();
  newtopo->setndims(num_remain_dims);
  std::vector<int> dimsv;
  const std::vector<int> &periods = topo->getperiods();
  std::vector<int> periodsv;

  for (i = 0; i < ndims; i++) {
    if (remain_dims[i]) {
      dimsv.push_back(dims[i]);
      periodsv.push_back(periods[i]);
    }
  }
  newtopo->setdims(dimsv);
  newtopo->setperiods(periodsv);

  std::vector<int> nborsv;
  getAmpiInstance(*newcomm)->findNeighbors(*newcomm, getAmpiParent()->getRank(*newcomm), nborsv);
  newtopo->setnbors(nborsv);

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Type_get_envelope, MPI_Datatype datatype, int *ni, int *na,
                                          int *nd, int *combiner)
{
  AMPI_API("AMPI_Type_get_envelope", datatype, ni, na, nd, combiner);

#if AMPI_ERROR_CHECKING
  int ret = checkData("AMPI_Type_get_envelope", datatype);
  if (ret!=MPI_SUCCESS)
    return ret;
#endif

  return getDDT()->getEnvelope(datatype,ni,na,nd,combiner);
}

AMPI_API_IMPL(int, MPI_Type_get_contents, MPI_Datatype datatype, int ni, int na, int nd,
                                          int i[], MPI_Aint a[], MPI_Datatype d[])
{
  AMPI_API("AMPI_Type_get_contents", datatype, ni, na, nd, i, a, d);

#if AMPI_ERROR_CHECKING
  int ret = checkData("AMPI_Type_get_contents", datatype);
  if (ret!=MPI_SUCCESS)
    return ret;
#endif

  return getDDT()->getContents(datatype,ni,na,nd,i,a,d);
}

AMPI_API_IMPL(int, MPI_Pcontrol, const int level, ...)
{
//int AMPI_Pcontrol(const int level, ...) {
  //AMPI_API("AMPI_Pcontrol");
  return MPI_SUCCESS;
}

/* Extensions needed by ROMIO */

AMPI_API_IMPL(int, MPIR_Status_set_bytes, MPI_Status *sts, MPI_Datatype dtype, MPI_Count nbytes)
{
  AMPI_API("AMPIR_Status_set_bytes", sts, dtype, nbytes);
  return MPI_Status_set_elements_x(sts, MPI_BYTE, nbytes);
}

/******** AMPI Extensions to the MPI standard *********/

CLINKAGE char ** AMPI_Get_argv()
{
  return CkGetArgv();
}

CLINKAGE int AMPI_Get_argc()
{
  return CkGetArgc();
}

CLINKAGE int AMPI_Migrate(MPI_Info hints)
{
  AMPI_API("AMPI_Migrate", hints);
  int nkeys, exists;
  char key[MPI_MAX_INFO_KEY], value[MPI_MAX_INFO_VAL];

  MPI_Info_get_nkeys(hints, &nkeys);

  for (int i=0; i<nkeys; i++) {
    MPI_Info_get_nthkey(hints, i, key);
    MPI_Info_get(hints, key, MPI_MAX_INFO_VAL, value, &exists);
    if (!exists) {
      continue;
    }
    else if (strncmp(key, "ampi_load_balance", MPI_MAX_INFO_KEY) == 0) {

      if (strncmp(value, "sync", MPI_MAX_INFO_VAL) == 0) {
        int oldPe = CkMyPe();
        TCHARM_Migrate();
        if (oldPe != CkMyPe()) {
          removeUnimportantArrayObjsfromPeCache();
        }
      }
      else if (strncmp(value, "async", MPI_MAX_INFO_VAL) == 0) {
        int oldPe = CkMyPe();
        TCHARM_Async_Migrate();
        if (oldPe != CkMyPe()) {
          removeUnimportantArrayObjsfromPeCache();
        }
      }
      else if (strncmp(value, "false", MPI_MAX_INFO_VAL) == 0) {
        /* do nothing */
      }
      else {
        CkPrintf("WARNING: Unknown MPI_Info value (%s) given to AMPI_Migrate for key: %s\n", value, key);
      }
    }
    else if (strncmp(key, "ampi_checkpoint", MPI_MAX_INFO_KEY) == 0) {

      if (strncmp(value, "true", MPI_MAX_INFO_VAL) == 0) {
        CkAbort("AMPI> Error: Value \"true\" is not supported for AMPI_Migrate key \"ampi_checkpoint\"!\n");
      }
      else if (strncmp(value, "to_file=", strlen("to_file=")) == 0) {
        int offset = strlen("to_file=");
        int restart_dir_name_len = 0;
        MPI_Info_get_valuelen(hints, key, &restart_dir_name_len, &exists);
        if (restart_dir_name_len > offset) {
          value[restart_dir_name_len] = '\0';
        }
        else {
          CkAbort("AMPI> Error: No checkpoint directory name given to AMPI_Migrate\n");
        }
        ampi * ptr = getAmpiInstance(MPI_COMM_WORLD)->barrier();
        ptr->getParent()->startCheckpoint(&value[offset]);
      }
      else if (strncmp(value, "in_memory", MPI_MAX_INFO_VAL) == 0) {
#if CMK_MEM_CHECKPOINT
        ampi * ptr = getAmpiInstance(MPI_COMM_WORLD)->barrier();
        ptr->getParent()->startCheckpoint("");
#else
        CkPrintf("AMPI> Error: In-memory checkpoint/restart is not enabled!\n");
        CkAbort("AMPI> Error: Recompile Charm++/AMPI with CMK_MEM_CHECKPOINT.\n");
#endif
      }
      else if (strncmp(value, "message_logging", MPI_MAX_INFO_VAL) == 0) {
        CkPrintf("AMPI> Error: Message logging is not enabled!\n");
        CkAbort("AMPI> Error: Recompile Charm++/AMPI with CMK_MESSAGE_LOGGING.\n");
      }
      else if (strncmp(value, "false", MPI_MAX_INFO_VAL) == 0) {
        /* do nothing */
      }
      else {
        CkPrintf("WARNING: Unknown MPI_Info value (%s) given to AMPI_Migrate for key: %s\n", value, key);
      }
    }
    else {
      CkPrintf("WARNING: Unknown MPI_Info key given to AMPI_Migrate: %s\n", key);
    }
  }


  return MPI_SUCCESS;
}

CLINKAGE
int AMPI_Migrate_to_pe(int dest)
{
  AMPI_API("AMPI_Migrate_to_pe", dest);
  TCHARM_Migrate_to(dest);
  return MPI_SUCCESS;
}

CLINKAGE
int AMPI_Set_migratable(int mig)
{
  AMPI_API("AMPI_Set_migratable", mig);
#if CMK_LBDB_ON
  getAmpiParent()->setMigratable((mig!=0));
#else
  CkPrintf("WARNING: MPI_Set_migratable is not supported in this build of Charm++/AMPI.\n");
#endif
  return MPI_SUCCESS;
}

CLINKAGE
int AMPI_Load_start_measure(void)
{
  AMPI_API("AMPI_Load_start_measure", "");
  LBTurnInstrumentOn();
  return MPI_SUCCESS;
}

CLINKAGE
int AMPI_Load_stop_measure(void)
{
  AMPI_API("AMPI_Load_stop_measure", "");
  LBTurnInstrumentOff();
  return MPI_SUCCESS;
}

CLINKAGE
int AMPI_Load_reset_measure(void)
{
  AMPI_API("AMPI_Load_reset_measure", "");
  LBClearLoads();
  return MPI_SUCCESS;
}

CLINKAGE
int AMPI_Load_set_value(double value)
{
  AMPI_API("AMPI_Load_set_value", value);
  ampiParent *ptr = getAmpiParent();
  ptr->setObjTime(value);
  return MPI_SUCCESS;
}

void _registerampif(void) {
  _registerampi();
}

CLINKAGE
int AMPI_Register_pup(MPI_PupFn fn, void *data, int *idx)
{
  AMPI_API("AMPI_Register_pup", fn, data, idx);
  *idx = TCHARM_Register(data, fn);
  return MPI_SUCCESS;
}

CLINKAGE
int AMPI_Register_about_to_migrate(MPI_MigrateFn fn)
{
  AMPI_API("AMPI_Register_about_to_migrate", fn);
  ampiParent *thisParent = getAmpiParent();
  thisParent->setUserAboutToMigrateFn(fn);
  return MPI_SUCCESS;
}

CLINKAGE
int AMPI_Register_just_migrated(MPI_MigrateFn fn)
{
  AMPI_API("AMPI_Register_just_migrated", fn);
  ampiParent *thisParent = getAmpiParent();
  thisParent->setUserJustMigratedFn(fn);
  return MPI_SUCCESS;
}

CLINKAGE
int AMPI_Get_pup_data(int idx, void *data)
{
  AMPI_API("AMPI_Get_pup_data", idx, data);
  data = TCHARM_Get_userdata(idx);
  return MPI_SUCCESS;
}

CLINKAGE
int AMPI_Type_is_contiguous(MPI_Datatype datatype, int *flag)
{
  AMPI_API("AMPI_Type_is_contiguous", datatype, flag);
  *flag = getDDT()->isContig(datatype);
  return MPI_SUCCESS;
}

CLINKAGE
int AMPI_Print(const char *str)
{
  AMPI_API("AMPI_Print", str);
  ampiParent *ptr = getAmpiParent();
  CkPrintf("[%d] %s\n", ptr->thisIndex, str);
  return MPI_SUCCESS;
}

CLINKAGE
int AMPI_Suspend(void)
{
  AMPI_API("AMPI_Suspend", "");
  ampiParent* unused = getAmpiParent()->block();
  return MPI_SUCCESS;
}

CLINKAGE
int AMPI_Yield(void)
{
  AMPI_API("AMPI_Yield", "");
  ampiParent* unused = getAmpiParent()->yield();
  return MPI_SUCCESS;
}

CLINKAGE
int AMPI_Resume(int dest, MPI_Comm comm)
{
  AMPI_API("AMPI_Resume", dest, comm);
  getAmpiInstance(comm)->getProxy()[dest].unblock();
  return MPI_SUCCESS;
}

CLINKAGE
int AMPI_System(const char *cmd)
{
  return TCHARM_System(cmd);
}

CLINKAGE
int AMPI_Trace_begin(void)
{
  traceBegin();
  return MPI_SUCCESS;
}

CLINKAGE
int AMPI_Trace_end(void)
{
  traceEnd();
  return MPI_SUCCESS;
}

int AMPI_Install_idle_timer(void)
{
#if AMPI_PRINT_IDLE
  beginHandle = CcdCallOnConditionKeep(CcdPROCESSOR_BEGIN_IDLE,(CcdVoidFn)BeginIdle,NULL);
  endHandle = CcdCallOnConditionKeep(CcdPROCESSOR_END_IDLE,(CcdVoidFn)EndIdle,NULL);
#endif
  return MPI_SUCCESS;
}

int AMPI_Uninstall_idle_timer(void)
{
#if AMPI_PRINT_IDLE
  CcdCancelCallOnConditionKeep(CcdPROCESSOR_BEGIN_IDLE,beginHandle);
  CcdCancelCallOnConditionKeep(CcdPROCESSOR_BEGIN_BUSY,endHandle);
#endif
  return MPI_SUCCESS;
}


#if CMK_CUDA
GPUReq::GPUReq() noexcept
{
  comm = MPI_COMM_SELF;
  ampi* ptr = getAmpiInstance(comm);
  src = ptr->getRank();
  buf = ptr;
}

bool GPUReq::test(MPI_Status *sts/*=MPI_STATUS_IGNORE*/) noexcept
{
  return complete;
}

CMI_WARN_UNUSED_RESULT ampiParent* GPUReq::wait(ampiParent* parent, MPI_Status *sts, int* result/*=nullptr*/) noexcept
{
  (void)sts;
  while (!complete) {
    parent = parent->block();
  }
  return parent;
}

bool GPUReq::receive(ampi *ptr, AmpiMsg *msg, bool deleteMsg/*=true*/) noexcept
{
  CkAbort("GPUReq::receive should never be called");
  return true;
}

void GPUReq::receive(ampi *ptr, CkReductionMsg *msg) noexcept
{
  CkAbort("GPUReq::receive should never be called");
}

void GPUReq::setComplete() noexcept
{
  complete = true;
}

void GPUReq::print() const noexcept {
  AmpiRequest::print();
}

void AMPI_GPU_complete(void *request, void* dummy) noexcept
{
  GPUReq *req = static_cast<GPUReq *>(request);
  req->setComplete();
  ampi *ptr = static_cast<ampi *>(req->buf);
  ptr->unblock();
}

/* Submit hapiWorkRequest and corresponding GPU request. */
CLINKAGE
int AMPI_GPU_Iinvoke_wr(hapiWorkRequest *to_call, MPI_Request *request)
{
  AMPI_API("AMPI_GPU_Iinvoke", to_call, request);

  ampi* ptr = getAmpiInstance(MPI_COMM_WORLD);
  GPUReq* newreq = new GPUReq();
  *request = ptr->postReq(newreq);

  // A callback that completes the corresponding request
  CkCallback cb(&AMPI_GPU_complete, newreq);
  hapiWorkRequestSetCallback(to_call, &cb);
  hapiEnqueue(to_call);
}

/* Submit GPU request that will be notified of completion once the previous
 * operations in the given CUDA stream are complete */
CLINKAGE
int AMPI_GPU_Iinvoke(cudaStream_t stream, MPI_Request *request)
{
  AMPI_API("AMPI_GPU_Iinvoke", stream, request);

  ampi* ptr = getAmpiInstance(MPI_COMM_WORLD);
  GPUReq* newreq = new GPUReq();
  *request = ptr->postReq(newreq);

  // A callback that completes the corresponding request
  CkCallback cb(&AMPI_GPU_complete, newreq);
  hapiAddCallback(stream, &cb, nullptr);
}

CLINKAGE
int AMPI_GPU_Invoke_wr(hapiWorkRequest *to_call)
{
  AMPI_API("AMPI_GPU_Invoke", to_call);

  MPI_Request req;
  AMPI_GPU_Iinvoke_wr(to_call, &req);
  MPI_Wait(&req, MPI_STATUS_IGNORE);

  return MPI_SUCCESS;
}

CLINKAGE
int AMPI_GPU_Invoke(cudaStream_t stream)
{
  AMPI_API("AMPI_GPU_Invoke", stream);

  MPI_Request req;
  AMPI_GPU_Iinvoke(stream, &req);
  MPI_Wait(&req, MPI_STATUS_IGNORE);

  return MPI_SUCCESS;
}
#endif // CMK_CUDA

#include "ampi.def.h"

void TCHARM_Node_Setup(int numelements)
{
  AMPI_Node_Setup(numelements);
}
void TCHARM_Element_Setup(int myelement, int numelements, CmiIsomallocContext ctx)
{
  AMPI_Rank_Setup(myelement, numelements, ctx);
}

#if defined _WIN32 || CMK_DLL_USE_DLOPEN
static ampi_maintype AMPI_Main_Get_C(SharedObject myexe)
{
  auto AMPI_Main_cpp_ptr = (ampi_maintype)dlsym(myexe, "AMPI_Main_cpp");
  if (AMPI_Main_cpp_ptr)
    return AMPI_Main_cpp_ptr;

  auto AMPI_Main_c_ptr = (ampi_maintype)dlsym(myexe, "AMPI_Main_c");
  if (AMPI_Main_c_ptr)
    return AMPI_Main_c_ptr;

  auto AMPI_Main_ptr = (ampi_maintype)dlsym(myexe, "AMPI_Main");
  if (AMPI_Main_ptr)
    return AMPI_Main_ptr;

  return nullptr;
}

static ampi_fmaintype AMPI_Main_Get_F(SharedObject myexe)
{
  auto fmpi_main_ptr = (ampi_fmaintype)dlsym(myexe, STRINGIFY(FTN_NAME(MPI_MAIN,mpi_main)));
  if (fmpi_main_ptr)
    return fmpi_main_ptr;

  auto export_ptr = (ampi_fmaintype)dlsym(myexe, "AMPI_Main_fortran_export");
  if (export_ptr)
    return export_ptr;

  return nullptr;
}

ampi_mainstruct AMPI_Main_Get(SharedObject myexe)
{
  return ampi_mainstruct
  {
    AMPI_Main_Get_C(myexe),
    AMPI_Main_Get_F(myexe)
  };
}

int AMPI_Main_Dispatch(ampi_mainstruct mainstruct, int argc, char ** argv)
{
  ampi_maintype c = mainstruct.c;
  if (c != nullptr)
  {
    return c(argc, argv);
  }

  ampi_fmaintype f = mainstruct.f;
  if (f != nullptr)
  {
    f();
    return 0;
  }

  CkAbort("Could not find any AMPI entry points!");

  return 1;
}
#endif
