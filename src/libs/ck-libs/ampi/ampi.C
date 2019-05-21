#ifndef AMPI_PRINT_MSG_SIZES
#define AMPI_PRINT_MSG_SIZES 0 // Record and print comm routines used & message sizes
#endif

#define AMPIMSGLOG    0
#define AMPI_PRINT_IDLE 0

#include "ampiimpl.h"
#include "tcharm.h"

#if CMK_BIGSIM_CHARM
#include "bigsim_logs.h"
#endif

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

/* change this define to "x" to trace all send/recv's */
#define MSG_ORDER_DEBUG(x) //x /* empty */
/* change this define to "x" to trace user calls */
#define USER_CALL_DEBUG(x) // ckout<<"vp "<<TCHARM_Element()<<": "<<x<<endl;
#define STARTUP_DEBUG(x) //ckout<<"ampi[pe "<<CkMyPe()<<"] "<< x <<endl;
#define FUNCCALL_DEBUG(x) //x /* empty */

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
    int funclen = strlen(func);
    const char* filler = " failed with error code ";
    int fillerlen = strlen(filler);
    int errstrlen;
    char errstr[MPI_MAX_ERROR_STRING];
    MPI_Error_string(errcode, errstr, &errstrlen);
    vector<char> str(funclen + fillerlen + errstrlen);
    strcpy(str.data(), func);
    strcat(str.data(), filler);
    strcat(str.data(), errstr);
    CkAbort(str.data());
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

inline int checkBuf(const char* func, const void *buf, int count) noexcept {
  if ((count != 0 && buf == NULL) || buf == MPI_IN_PLACE)
    return ampiErrhandler(func, MPI_ERR_BUFFER);
  return MPI_SUCCESS;
}

int errorCheck(const char* func, MPI_Comm comm, bool ifComm, int count,
               bool ifCount, MPI_Datatype data, bool ifData, int tag,
               bool ifTag, int rank, bool ifRank, const void *buf1,
               bool ifBuf1, const void *buf2=nullptr, bool ifBuf2=false) noexcept {
  int ret;
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
    ret = checkBuf(func, buf1, count*getDDT()->getSize(data));
    if (ret != MPI_SUCCESS)
      return ampiErrhandler(func, ret);
  }
  if (ifBuf2 && ifData) {
    ret = checkBuf(func, buf2, count*getDDT()->getSize(data));
    if (ret != MPI_SUCCESS)
      return ampiErrhandler(func, ret);
  }
#if AMPI_PRINT_MSG_SIZES
  getAmpiParent()->recordMsgSize(func, getDDT()->getSize(data) * count);
#endif
  return MPI_SUCCESS;
}

//------------- startup -------------
static mpi_comm_worlds mpi_worlds;

int _mpi_nworlds; /*Accessed by ampif*/
int MPI_COMM_UNIVERSE[MPI_MAX_COMM_WORLDS]; /*Accessed by user code*/

class AmpiComplex {
 public:
  float re, im;
  void operator+=(const AmpiComplex &a) noexcept {
    re+=a.re;
    im+=a.im;
  }
  void operator*=(const AmpiComplex &a) noexcept {
    float nu_re=re*a.re-im*a.im;
    im=re*a.im+im*a.re;
    re=nu_re;
  }
  int operator>(const AmpiComplex &a) noexcept {
    CkAbort("AMPI> Cannot compare complex numbers with MPI_MAX\n");
    return 0;
  }
  int operator<(const AmpiComplex &a) noexcept {
    CkAbort("AMPI> Cannot compare complex numbers with MPI_MIN\n");
    return 0;
  }
};

class AmpiDoubleComplex {
 public:
  double re, im;
  void operator+=(const AmpiDoubleComplex &a) noexcept {
    re+=a.re;
    im+=a.im;
  }
  void operator*=(const AmpiDoubleComplex &a) noexcept {
    double nu_re=re*a.re-im*a.im;
    im=re*a.im+im*a.re;
    re=nu_re;
  }
  int operator>(const AmpiDoubleComplex &a) noexcept {
    CkAbort("AMPI> Cannot compare double complex numbers with MPI_MAX\n");
    return 0;
  }
  int operator<(const AmpiDoubleComplex &a) noexcept {
    CkAbort("AMPI> Cannot compare double complex numbers with MPI_MIN\n");
    return 0;
  }
};

class AmpiLongDoubleComplex {
 public:
  long double re, im;
  void operator+=(const AmpiLongDoubleComplex &a) noexcept {
    re+=a.re;
    im+=a.im;
  }
  void operator*=(const AmpiLongDoubleComplex &a) noexcept {
    long double nu_re=re*a.re-im*a.im;
    im=re*a.im+im*a.re;
    re=nu_re;
  }
  int operator>(const AmpiLongDoubleComplex &a) noexcept {
    CkAbort("AMPI> Cannot compare long double complex numbers with MPI_MAX\n");
    return 0;
  }
  int operator<(const AmpiLongDoubleComplex &a) noexcept {
    CkAbort("AMPI> Cannot compare long double complex numbers with MPI_MIN\n");
    return 0;
  }
};

typedef struct { float val; int idx; } FloatInt;
typedef struct { double val; int idx; } DoubleInt;
typedef struct { long val; int idx; } LongInt;
typedef struct { int val; int idx; } IntInt;
typedef struct { short val; int idx; } ShortInt;
typedef struct { long double val; int idx; } LongdoubleInt;
typedef struct { float val; float idx; } FloatFloat;
typedef struct { double val; double idx; } DoubleDouble;

/* For MPI_MAX, MPI_MIN, MPI_SUM, and MPI_PROD: */
#define MPI_OP_SWITCH(OPNAME) \
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
  case MPI_COMPLEX: for(i=0;i<(*len);i++) { MPI_OP_IMPL(AmpiComplex); } break; \
  case MPI_DOUBLE_COMPLEX: for(i=0;i<(*len);i++) { MPI_OP_IMPL(AmpiDoubleComplex); } break; \
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
  case MPI_FLOAT_COMPLEX: for(i=0;i<(*len);i++) { MPI_OP_IMPL(AmpiComplex); } break; \
  case MPI_LONG_DOUBLE_COMPLEX: for(i=0;i<(*len);i++) { MPI_OP_IMPL(AmpiLongDoubleComplex); } break; \
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
  MPI_OP_SWITCH(MPI_MAX)
#undef MPI_OP_IMPL
}

void MPI_MIN_USER_FN( void *invec, void *inoutvec, int *len, MPI_Datatype *datatype){
#define MPI_OP_IMPL(type) \
  if(((type *)invec)[i] < ((type *)inoutvec)[i]) ((type *)inoutvec)[i] = ((type *)invec)[i];
  MPI_OP_SWITCH(MPI_MIN)
#undef MPI_OP_IMPL
}

void MPI_SUM_USER_FN( void *invec, void *inoutvec, int *len, MPI_Datatype *datatype){
#define MPI_OP_IMPL(type) \
  ((type *)inoutvec)[i] += ((type *)invec)[i];
  MPI_OP_SWITCH(MPI_SUM)
#undef MPI_OP_IMPL
}

void MPI_PROD_USER_FN( void *invec, void *inoutvec, int *len, MPI_Datatype *datatype){
#define MPI_OP_IMPL(type) \
  ((type *)inoutvec)[i] *= ((type *)invec)[i];
  MPI_OP_SWITCH(MPI_PROD)
#undef MPI_OP_IMPL
}

void MPI_REPLACE_USER_FN( void *invec, void *inoutvec, int *len, MPI_Datatype *datatype){
#define MPI_OP_IMPL(type) \
  ((type *)inoutvec)[i] = ((type *)invec)[i];
  MPI_OP_SWITCH(MPI_REPLACE)
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

class Builtin_kvs{
 public:
  int tag_ub,host,io,wtime_is_global,appnum,lastusedcode,universe_size;
  int win_disp_unit,win_create_flavor,win_model;
  int ampi_tmp;
  void* win_base;
  MPI_Aint win_size;
  Builtin_kvs() noexcept {
    tag_ub = MPI_TAG_UB_VALUE;
    host = MPI_PROC_NULL;
    io = 0;
    wtime_is_global = 0;
    appnum = 0;
    lastusedcode = MPI_ERR_LASTCODE;
    universe_size = 0;
    win_base = NULL;
    win_size = 0;
    win_disp_unit = 0;
    win_create_flavor = MPI_WIN_FLAVOR_CREATE;
    win_model = MPI_WIN_SEPARATE;
    ampi_tmp = 0;
  }
};

// ------------ startup support -----------
int _ampi_fallback_setup_count = -1;
CLINKAGE void AMPI_Setup(void);
FLINKAGE void FTN_NAME(AMPI_SETUP,ampi_setup)(void);

FLINKAGE void FTN_NAME(MPI_MAIN,mpi_main)(void);

/*Main routine used when missing MPI_Setup routine*/
CLINKAGE
void AMPI_Fallback_Main(int argc,char **argv)
{
  AMPI_Main_cpp();
  AMPI_Main_cpp(argc,argv);
  AMPI_Main_c(argc,argv);
  FTN_NAME(MPI_MAIN,mpi_main)();
}

void ampiCreateMain(MPI_MainFn mainFn, const char *name,int nameLen);
/*Startup routine used if user *doesn't* write
  a TCHARM_User_setup routine.
 */
CLINKAGE
void AMPI_Setup_Switch(void) {
  _ampi_fallback_setup_count=0;
  FTN_NAME(AMPI_SETUP,ampi_setup)();
  AMPI_Setup();
  if (_ampi_fallback_setup_count==2)
  { //Missing AMPI_Setup in both C and Fortran:
    ampiCreateMain(AMPI_Fallback_Main,"default",strlen("default"));
  }
}

int AMPI_RDMA_THRESHOLD = AMPI_RDMA_THRESHOLD_DEFAULT;
int AMPI_SMP_RDMA_THRESHOLD = AMPI_SMP_RDMA_THRESHOLD_DEFAULT;
static bool nodeinit_has_been_called=false;
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
static int AMPI_threadstart_idx = -1;

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
  CkAbort("Could not get path to current binary!");
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
    CsvAccess(tcharm_funcmap)->insert(std::pair<std::string, int>(funclist[i], event_id));
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

  _mpi_nworlds=0;
  for(int i=0;i<MPI_MAX_COMM_WORLDS; i++)
  {
    MPI_COMM_UNIVERSE[i] = MPI_COMM_WORLD+1+i;
  }
  TCHARM_Set_fallback_setup(AMPI_Setup_Switch);

  /* read AMPI environment variables */
  char *value;
  bool rdmaSet = false;
  if ((value = getenv("AMPI_RDMA_THRESHOLD"))) {
    AMPI_RDMA_THRESHOLD = atoi(value);
    rdmaSet = true;
  }
  if ((value = getenv("AMPI_SMP_RDMA_THRESHOLD"))) {
    AMPI_SMP_RDMA_THRESHOLD = atoi(value);
    rdmaSet = true;
  }
  if (rdmaSet && CkMyNode() == 0) {
#if AMPI_RDMA_IMPL
    CkPrintf("AMPI> RDMA threshold is %d Bytes and SMP RDMA threshold is %d Bytes.\n", AMPI_RDMA_THRESHOLD, AMPI_SMP_RDMA_THRESHOLD);
#else
    CkPrintf("Warning: AMPI RDMA threshold ignored since AMPI RDMA is disabled.\n");
#endif
  }

  AmpiReducer = CkReduction::addReducer(AmpiReducerFunc, true /*streamable*/, "AmpiReducerFunc");

  CkAssert(AMPI_threadstart_idx == -1);    // only initialize once
  AMPI_threadstart_idx = TCHARM_Register_thread_function(AMPI_threadstart);

  nodeinit_has_been_called=true;

   // ASSUME NO ANYTIME MIGRATION and STATIC INSERTON
  _isAnytimeMigration = false;
  _isStaticInsertion = true;
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

  CkpvInitialize(AmpiMsgPool, msgPool); // pool of small AmpiMsg's
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

PUPfunctionpointer(MPI_MainFn)

class MPI_threadstart_t {
 public:
  MPI_MainFn fn;
  MPI_threadstart_t() noexcept {}
  MPI_threadstart_t(MPI_MainFn fn_) noexcept :fn(fn_) {}
  void start() {
    char **argv=CmiCopyArgs(CkGetArgv());
    int argc=CkGetArgc();

    // Set a pointer to somewhere close to the bottom of the stack.
    // This is used for roughly estimating the stack usage later.
    CtvAccess(stackBottom) = &argv;

#if !CMK_NO_BUILD_SHARED
    // If charm++ is built with shared libraries, it does not support
    // a custom AMPI_Setup method and always uses AMPI_Fallback_Main.
    // Works around bug #1508.
    if (_ampi_fallback_setup_count != -1 && _ampi_fallback_setup_count != 2 && CkMyPe() == 0) {
      CkAbort("AMPI> The application provided a custom AMPI_Setup() method, "
      "but AMPI is built with shared library support. This is an unsupported "
      "configuration. Please recompile charm++/AMPI without `-build-shared` or "
      "remove the AMPI_Setup() function from your application.\n");
    }
    AMPI_Fallback_Main(argc,argv);
#else
    (fn)(argc,argv);
#endif
  }
  void pup(PUP::er &p) noexcept {
    p|fn;
  }
};
PUPmarshall(MPI_threadstart_t)

CLINKAGE
void AMPI_threadstart(void *data)
{
  STARTUP_DEBUG("MPI_threadstart")
  MPI_threadstart_t t;
  pupFromBuf(data,t);
#if CMK_TRACE_IN_CHARM
  if(CpvAccess(traceOn)) CthTraceResume(CthSelf());
#endif
  t.start();
}

void ampiCreateMain(MPI_MainFn mainFn, const char *name,int nameLen)
{
  STARTUP_DEBUG("ampiCreateMain")
  int _nchunks=TCHARM_Get_num_chunks();
  //Make a new threads array:
  MPI_threadstart_t s(mainFn);
  memBuf b; pupIntoBuf(b,s);
  TCHARM_Create_data(_nchunks,AMPI_threadstart_idx,
                     b.getData(), b.getSize());
}

/* TCharm Semaphore ID's for AMPI startup */
#define AMPI_TCHARM_SEMAID 0x00A34100 /* __AMPI__ */
#define AMPI_BARRIER_SEMAID 0x00A34200 /* __AMPI__ */

static CProxy_ampiWorlds ampiWorldsGroup;

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

/*
   Called from MPI_Init, a collective initialization call:
   creates a new AMPI array and attaches it to the current
   set of TCHARM threads.
 */
static ampi *ampiInit(char **argv) noexcept
{
  FUNCCALL_DEBUG(CkPrintf("Calling from proc %d for tcharm element %d\n", CkMyPe(), TCHARM_Element());)
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
    if(_mpi_nworlds == MPI_MAX_COMM_WORLDS)
    {
      CkAbort("AMPI> Number of registered comm_worlds exceeded limit.\n");
    }
    int new_idx=_mpi_nworlds;
    new_world=MPI_COMM_WORLD+new_idx;

    //Create and attach the ampiParent array
    CkArrayID threads;
    opts=TCHARM_Attach_start(&threads,&_nchunks);
    opts.setSectionAutoDelegate(false);
    CkArrayCreatedMsg *m;
    CProxy_ampiParent::ckNew(new_world, threads, _nchunks, opts, CkCallbackResumeThread((void*&)m));
    parent = CProxy_ampiParent(m->aid);
    delete m;
    STARTUP_DEBUG("ampiInit> array size "<<_nchunks);
  }
  int *barrier = (int *)TCharm::get()->semaGet(AMPI_BARRIER_SEMAID);

  FUNCCALL_DEBUG(CkPrintf("After BARRIER: sema size %d from tcharm's ele %d\n", TCharm::get()->sema.size(), TCHARM_Element());)

  if (TCHARM_Element()==0)
  {
    //Make a new ampi array
    CkArrayID empty;

    ampiCommStruct worldComm(new_world,empty,_nchunks);
    CProxy_ampi arr;
    CkArrayCreatedMsg *m;
    CProxy_ampi::ckNew(parent, worldComm, opts, CkCallbackResumeThread((void*&)m));
    arr = CProxy_ampi(m->aid);
    delete m;

    //Broadcast info. to the mpi_worlds array
    // FIXME: remove race condition from MPI_COMM_UNIVERSE broadcast
    ampiCommStruct newComm(new_world,arr,_nchunks);
    if (ampiWorldsGroup.ckGetGroupID().isZero())
      ampiWorldsGroup=CProxy_ampiWorlds::ckNew(newComm);
    else
      ampiWorldsGroup.add(newComm);
    STARTUP_DEBUG("ampiInit> arrays created")
  }

  // Find our ampi object:
  ampi *ptr=(ampi *)TCharm::get()->semaGet(AMPI_TCHARM_SEMAID);
  CtvAccess(ampiInitDone)=true;
  CtvAccess(ampiFinalized)=false;
  STARTUP_DEBUG("ampiInit> complete")
#if CMK_BIGSIM_CHARM
    //  TRACE_BG_AMPI_START(ptr->getThread(), "AMPI_START");
    TRACE_BG_ADD_TAG("AMPI_START");
#endif

  ampiParent* pptr = getAmpiParent();
  vector<int>& keyvals = pptr->getKeyvals(MPI_COMM_WORLD);
  pptr->setAttr(MPI_COMM_WORLD, keyvals, MPI_UNIVERSE_SIZE, &_nchunks);
  ptr->setCommName("MPI_COMM_WORLD");

  pptr->ampiInitCallDone = 0;

  CProxy_ampi cbproxy = ptr->getProxy();
  CkCallback cb(CkReductionTarget(ampi, allInitDone), cbproxy[0]);
  ptr->contribute(cb);

  ampiParent *thisParent = getAmpiParent();
  while(thisParent->ampiInitCallDone!=1){
    thisParent->getTCharmThread()->stop();
    /*
     * thisParent needs to be updated in case of the parent is being pupped.
     * In such case, thisParent got changed
     */
    thisParent = getAmpiParent();
  }

  createCommSelf();

#if CMK_BIGSIM_CHARM
  BgSetStartOutOfCore();
#endif

  return ptr;
}

/// This group is used to broadcast the MPI_COMM_UNIVERSE communicators.
class ampiWorlds : public CBase_ampiWorlds {
 public:
  ampiWorlds(const ampiCommStruct &nextWorld) noexcept {
    ampiWorldsGroup=thisgroup;
    add(nextWorld);
  }
  ampiWorlds(CkMigrateMessage *m) noexcept : CBase_ampiWorlds(m) {}
  void pup(PUP::er &p) noexcept { }
  void add(const ampiCommStruct &nextWorld) noexcept {
    int new_idx=nextWorld.getComm()-(MPI_COMM_WORLD);
    mpi_worlds[new_idx]=nextWorld;
    if (_mpi_nworlds<=new_idx) _mpi_nworlds=new_idx+1;
    STARTUP_DEBUG("ampiInit> listed MPI_COMM_UNIVERSE "<<new_idx)
  }
};

//-------------------- ampiParent -------------------------
ampiParent::ampiParent(MPI_Comm worldNo_,CProxy_TCharm threads_,int nRanks_) noexcept
  : threads(threads_), ampiReqs(64, &reqPool), myDDT(ampiPredefinedTypes),
    worldNo(worldNo_), predefinedOps(ampiPredefinedOps), isTmpRProxySet(false)
{
  int barrier = 0x1234;
  STARTUP_DEBUG("ampiParent> starting up")
  thread=NULL;
  worldPtr=NULL;
  userAboutToMigrateFn=NULL;
  userJustMigratedFn=NULL;
  prepareCtv();

  // Allocate an empty groupStruct to represent MPI_EMPTY_GROUP
  groups.push_back(new groupStruct);

  init();

  //ensure MPI_INFO_ENV will always be first info object
  defineInfoEnv(nRanks_);
  // define Info objects for AMPI_Migrate calls
  defineInfoMigration();

  thread->semaPut(AMPI_BARRIER_SEMAID,&barrier);

#if CMK_FAULT_EVAC
  AsyncEvacuate(false);
#endif
}

ampiParent::ampiParent(CkMigrateMessage *msg) noexcept
  : CBase_ampiParent(msg), myDDT(ampiPredefinedTypes), predefinedOps(ampiPredefinedOps)
{
  thread=NULL;
  worldPtr=NULL;

  init();

#if CMK_FAULT_EVAC
  AsyncEvacuate(false);
#endif
}

PUPfunctionpointer(MPI_MigrateFn)

void ampiParent::pup(PUP::er &p) noexcept {
  p|threads;
  p|worldNo;
  p|myDDT;
  p|splitComm;
  p|groupComm;
  p|cartComm;
  p|graphComm;
  p|distGraphComm;
  p|interComm;
  p|intraComm;

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
    (*userAboutToMigrateFn)();
  }
}

void ampiParent::ckJustMigrated() noexcept {
  ArrayElement1D::ckJustMigrated();
  prepareCtv();
  if (userJustMigratedFn) {
    (*userJustMigratedFn)();
  }
}

void ampiParent::ckJustRestored() noexcept {
  FUNCCALL_DEBUG(CkPrintf("Call just restored from ampiParent[%d] with ampiInitCallDone %d\n", thisIndex, ampiInitCallDone);)
  ArrayElement1D::ckJustRestored();
  prepareCtv();
}

ampiParent::~ampiParent() noexcept {
  STARTUP_DEBUG("ampiParent> destructor called");
  finalize();
}

const ampiCommStruct& ampiParent::getWorldStruct() const noexcept {
  return worldPtr->getCommStruct();
}

//Children call this when they are first created or just migrated
TCharm *ampiParent::registerAmpi(ampi *ptr,ampiCommStruct s,bool forMigration) noexcept
{
  if (thread==NULL) prepareCtv(); //Prevents CkJustMigrated race condition

  if (s.getComm()>=MPI_COMM_WORLD)
  { //We now have our COMM_WORLD-- register it
    //Note that split communicators don't keep a raw pointer, so
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
    if (comm>=MPI_COMM_WORLD) {
      // Pass the new ampi to the waiting ampiInit
      thread->semaPut(AMPI_TCHARM_SEMAID, ptr);
    } else if (isSplit(comm)) {
      splitChildRegister(s);
    } else if (isGroup(comm)) {
      groupChildRegister(s);
    } else if (isCart(comm)) {
      cartChildRegister(s);
    } else if (isGraph(comm)) {
      graphChildRegister(s);
    } else if (isDistGraph(comm)) {
      distGraphChildRegister(s);
    } else if (isInter(comm)) {
      interChildRegister(s);
    } else if (isIntra(comm)) {
      intraChildRegister(s);
    }else
      CkAbort("ampiParent received child with bad communicator");
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

  thread->stop();

#if CMK_BIGSIM_CHARM
  TRACE_BG_ADD_TAG("CHECKPOINT_RESUME");
#endif
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

int ampiParent::setUserKeyval(int context, int keyval, void *attribute_val) noexcept {
#if AMPI_ERROR_CHECKING
  if (keyval < 0 || keyval >= kvlist.size() || kvlist[keyval] == NULL) {
    return MPI_ERR_KEYVAL;
  }
#endif
  KeyvalNode &kv = *kvlist[keyval];
  if (kv.hasVal()) {
    int ret = (*kv.delete_fn)(context, keyval, kv.val, kv.extra_state);
    if (ret != MPI_SUCCESS) {
      return ret;
    }
  }
  kvlist[keyval]->setVal(attribute_val);
  return MPI_SUCCESS;
}

int ampiParent::setAttr(int context, vector<int>& keyvals, int keyval, void* attribute_val) noexcept {
  if (kv_set_builtin(keyval, attribute_val)) {
    return MPI_SUCCESS;
  }
  keyvals.push_back(keyval);
  kvlist[keyval]->incRefCount();
  return setUserKeyval(context, keyval, attribute_val);
}

bool ampiParent::kv_set_builtin(int keyval, void* attribute_val) noexcept {
  switch(keyval) {
    case MPI_TAG_UB:            /*immutable*/ return false;
    case MPI_HOST:              /*immutable*/ return false;
    case MPI_IO:                /*immutable*/ return false;
    case MPI_WTIME_IS_GLOBAL:   /*immutable*/ return false;
    case MPI_APPNUM:            /*immutable*/ return false;
    case MPI_LASTUSEDCODE:      /*immutable*/ return false;
    case MPI_UNIVERSE_SIZE:     (CkpvAccess(bikvs).universe_size)     = *((int*)attribute_val);      return true;
    case MPI_WIN_BASE:          (CkpvAccess(bikvs).win_base)          = attribute_val;               return true;
    case MPI_WIN_SIZE:          (CkpvAccess(bikvs).win_size)          = *((MPI_Aint*)attribute_val); return true;
    case MPI_WIN_DISP_UNIT:     (CkpvAccess(bikvs).win_disp_unit)     = *((int*)attribute_val);      return true;
    case MPI_WIN_CREATE_FLAVOR: (CkpvAccess(bikvs).win_create_flavor) = *((int*)attribute_val);      return true;
    case MPI_WIN_MODEL:         (CkpvAccess(bikvs).win_model)         = *((int*)attribute_val);      return true;
    case AMPI_MY_WTH:           /*immutable*/ return false;
    case AMPI_NUM_WTHS:         /*immutable*/ return false;
    case AMPI_MY_PROCESS:       /*immutable*/ return false;
    case AMPI_NUM_PROCESSES:    /*immutable*/ return false;
    default: return false;
  };
}

bool ampiParent::kv_get_builtin(int keyval) noexcept {
  switch(keyval) {
    case MPI_TAG_UB:            kv_builtin_storage = &(CkpvAccess(bikvs).tag_ub);             return true;
    case MPI_HOST:              kv_builtin_storage = &(CkpvAccess(bikvs).host);               return true;
    case MPI_IO:                kv_builtin_storage = &(CkpvAccess(bikvs).io);                 return true;
    case MPI_WTIME_IS_GLOBAL:   kv_builtin_storage = &(CkpvAccess(bikvs).wtime_is_global);    return true;
    case MPI_APPNUM:            kv_builtin_storage = &(CkpvAccess(bikvs).appnum);             return true;
    case MPI_LASTUSEDCODE:      kv_builtin_storage = &(CkpvAccess(bikvs).lastusedcode);       return true;
    case MPI_UNIVERSE_SIZE:     kv_builtin_storage = &(CkpvAccess(bikvs).universe_size);      return true;
    case MPI_WIN_BASE:          win_base_storage   = &(CkpvAccess(bikvs).win_base);           return true;
    case MPI_WIN_SIZE:          win_size_storage   = &(CkpvAccess(bikvs).win_size);           return true;
    case MPI_WIN_DISP_UNIT:     kv_builtin_storage = &(CkpvAccess(bikvs).win_disp_unit);      return true;
    case MPI_WIN_CREATE_FLAVOR: kv_builtin_storage = &(CkpvAccess(bikvs).win_create_flavor);  return true;
    case MPI_WIN_MODEL:         kv_builtin_storage = &(CkpvAccess(bikvs).win_model);          return true;
    default: return false;
  };
}

bool ampiParent::getBuiltinKeyval(int keyval, void *attribute_val) noexcept {
  if (kv_get_builtin(keyval)){
    /* All builtin keyvals are ints except MPI_WIN_BASE, which is a pointer
     * to the window's base address in C but an integer representation of
     * the base address in Fortran.
     * Also, MPI_WIN_SIZE is an MPI_Aint. */
    if (keyval == MPI_WIN_BASE)
      *((void**)attribute_val) = *win_base_storage;
    else if (keyval == MPI_WIN_SIZE)
      *(MPI_Aint**)attribute_val = win_size_storage;
    else
      *(int **)attribute_val = kv_builtin_storage;
    return true;
  } else {
    switch(keyval) {
      case AMPI_MY_WTH: *(int *)attribute_val = CkMyPe(); return true;
      case AMPI_NUM_WTHS: *(int *)attribute_val = CkNumPes(); return true;
      case AMPI_MY_PROCESS: *(int *)attribute_val = CkMyNode(); return true;
      case AMPI_NUM_PROCESSES: *(int *)attribute_val = CkNumNodes(); return true;
    }
  }
  return false;
}

// Call copy_fn for each user-defined keyval in old_comm.
int ampiParent::dupUserKeyvals(MPI_Comm old_comm, MPI_Comm new_comm) noexcept {
  ampiCommStruct &old_cs = *(ampiCommStruct *)&comm2CommStruct(old_comm);
  for (int i=0; i<old_cs.getKeyvals().size(); i++) {
    int keyval = old_cs.getKeyvals()[i];
    void *val_out;
    int flag = 0;
    bool isValid = (keyval != MPI_KEYVAL_INVALID && kvlist[keyval] != NULL);
    if (isValid) {
      // Call the user's copy_fn
      KeyvalNode& kv = *kvlist[keyval];
      int ret = (*kv.copy_fn)(old_comm, keyval, kv.extra_state, kv.val, &val_out, &flag);
      if (ret != MPI_SUCCESS) {
        return ret;
      }
      if (flag == 1) {
        // Set keyval in new_comm
        ampiCommStruct &cs = *(ampiCommStruct *)&comm2CommStruct(new_comm);
        cs.getKeyvals().push_back(keyval);
        kv.incRefCount();
      }
    }
  }
  return MPI_SUCCESS;
}

int ampiParent::freeUserKeyval(int context, vector<int>& keyvals, int* keyval) noexcept {
  if (*keyval < 0 || *keyval >= kvlist.size()) {
    return MPI_SUCCESS;
  }
  // Call the user's delete_fn
  KeyvalNode& kv = *kvlist[*keyval];
  int ret = (*kv.delete_fn)(context, *keyval, kv.val, kv.extra_state);
  if (ret != MPI_SUCCESS) {
    return ret;
  }
  // Remove keyval from comm/win/type keyvals list
  kv.clearVal();
  for (int i=0; i<keyvals.size(); i++) {
    if (keyvals[i] == *keyval) {
      keyvals[*keyval] = MPI_KEYVAL_INVALID;
    }
  }
  if (!keyvals.empty()) {
    while (keyvals.back() == MPI_KEYVAL_INVALID) keyvals.pop_back();
  }
  // Remove keyval from parent kvlist if no remaining references to it
  if (kv.decRefCount() == 0) {
    delete kvlist[*keyval];
    kvlist[*keyval] = NULL;
  }
  *keyval = MPI_KEYVAL_INVALID;
  return MPI_SUCCESS;
}

int ampiParent::freeUserKeyvals(int context, vector<int>& keyvals) noexcept {
  for (int i=0; i<keyvals.size(); i++) {
    int keyval = keyvals[i];
    // Call the user's delete_fn
    KeyvalNode& kv = *kvlist[keyval];
    int ret = (*kv.delete_fn)(context, keyval, kv.val, kv.extra_state);
    if (ret != MPI_SUCCESS) {
      return ret;
    }
    kv.clearVal();
    keyvals[i] = MPI_KEYVAL_INVALID;
    // Remove keyval from parent kvlist if no remaining references to it
    if (kv.decRefCount() == 0) {
      delete kvlist[keyval];
      kvlist[keyval] = NULL;
    }
  }
  keyvals.clear();
  return MPI_SUCCESS;
}

bool ampiParent::getUserKeyval(MPI_Comm comm, vector<int>& keyvals, int keyval, void *attribute_val, int *flag) noexcept {
  if (keyval < 0 || keyval >= kvlist.size() || kvlist[keyval] == NULL) {
    *flag = 0;
    return false;
  }
  else {
    for (int i=0; i<keyvals.size(); i++) {
      int kv = keyvals[i];
      if (keyval == kv) { // Found a matching keyval
        *(void **)attribute_val = kvlist[keyval]->getVal();
        *flag = 1;
        return true;
      }
    }
    *flag = 0;
    return false;
  }
}

int ampiParent::getAttr(int context, vector<int>& keyvals, int keyval, void *attribute_val, int *flag) noexcept {
  if (keyval == MPI_KEYVAL_INVALID) {
    *flag = 0;
    return MPI_ERR_KEYVAL;
  }
  else if (getBuiltinKeyval(keyval, attribute_val)) {
    *flag = 1;
    return MPI_SUCCESS;
  }
  else if (getUserKeyval(context, keyvals, keyval, attribute_val, flag)) {
    *flag = 1;
    return MPI_SUCCESS;
  }
  else {
    *flag = 0;
    return MPI_SUCCESS;
  }
}

int ampiParent::deleteAttr(int context, vector<int>& keyvals, int keyval) noexcept {
  return freeUserKeyval(context, keyvals, &keyval);
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

#if CMK_FAULT_EVAC
  AsyncEvacuate(false);
#endif
}

ampi::ampi() noexcept
{
  /* this constructor only exists so we can create an empty array during split */
  CkAbort("Default ampi constructor should never be called");
}

ampi::ampi(CkArrayID parent_,const ampiCommStruct &s) noexcept :parentProxy(parent_), oorder(s.getSize())
{
  init();

  myComm=s; myComm.setArrayID(thisArrayID);
  myRank=myComm.getRankForIndex(thisIndex);

  findParent(false);
}

ampi::ampi(CkMigrateMessage *msg) noexcept : CBase_ampi(msg)
{
  init();
}

void ampi::ckJustMigrated() noexcept
{
  findParent(true);
  ArrayElement1D::ckJustMigrated();
}

void ampi::ckJustRestored() noexcept
{
  FUNCCALL_DEBUG(CkPrintf("Call just restored from ampi[%d]\n", thisIndex);)
  findParent(true);
  ArrayElement1D::ckJustRestored();
}

void ampi::findParent(bool forMigration) noexcept {
  STARTUP_DEBUG("ampi> finding my parent")
  parent=parentProxy[thisIndex].ckLocal();
#if CMK_ERROR_CHECKING
  if (parent==NULL) CkAbort("AMPI can't find its parent!");
#endif
  thread=parent->registerAmpi(this,myComm,forMigration);
#if CMK_ERROR_CHECKING
  if (thread==NULL) CkAbort("AMPI can't find its thread!");
#endif
}

//The following method should be called on the first element of the
//ampi array
void ampi::allInitDone() noexcept {
  FUNCCALL_DEBUG(CkPrintf("All mpi_init have been called!\n");)
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
  p|myComm;
  p|myRank;
  p|tmpVec;
  p|remoteProxy;
  unexpectedMsgs.pup(p, AmmPupUnexpectedMsgs);
  postedReqs.pup(p, AmmPupPostedReqs);
  unexpectedBcastMsgs.pup(p, AmmPupUnexpectedMsgs);
  postedBcastReqs.pup(p, AmmPupPostedReqs);
  p|greq_classes;
  p|oorder;
}

ampi::~ampi() noexcept
{
  if (CkInRestarting() || _BgOutOfCoreFlag==1) {
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
  int nextSplitComm;
  int color; //New class of processes we'll belong to
  int key; //To determine rank in new ordering
  int rank; //Rank in old ordering
  ampiSplitKey() noexcept {}
  ampiSplitKey(int nextSplitComm_,int color_,int key_,int rank_) noexcept
    :nextSplitComm(nextSplitComm_), color(color_), key(key_), rank(rank_) {}
};

#define MPI_INTER 10

/* "type" may indicate whether call is for a cartesian topology etc. */
void ampi::split(int color,int key,MPI_Comm *dest, int type) noexcept
{
#if CMK_BIGSIM_CHARM
  void *curLog; // store current log in timeline
  _TRACE_BG_TLINE_END(&curLog);
#endif
  if (type == MPI_CART) {
    ampiSplitKey splitKey(parent->getNextCart(),color,key,myRank);
    int rootIdx=myComm.getIndexForRank(0);
    CkCallback cb(CkIndex_ampi::splitPhase1(0),CkArrayIndex1D(rootIdx),myComm.getProxy());
    contribute(sizeof(splitKey),&splitKey,CkReduction::concat,cb);

    thread->suspend(); //Resumed by ampiParent::cartChildRegister
    MPI_Comm newComm=parent->getNextCart()-1;
    *dest=newComm;
  }
  else if (type == MPI_GRAPH) {
    ampiSplitKey splitKey(parent->getNextGraph(),color,key,myRank);
    int rootIdx=myComm.getIndexForRank(0);
    CkCallback cb(CkIndex_ampi::splitPhase1(0),CkArrayIndex1D(rootIdx),myComm.getProxy());
    contribute(sizeof(splitKey),&splitKey,CkReduction::concat,cb);

    thread->suspend(); //Resumed by ampiParent::graphChildRegister
    MPI_Comm newComm=parent->getNextGraph()-1;
    *dest=newComm;
  }
  else if (type == MPI_DIST_GRAPH) {
    ampiSplitKey splitKey(parent->getNextDistGraph(),color,key,myRank);
    int rootIdx=myComm.getIndexForRank(0);
    CkCallback cb(CkIndex_ampi::splitPhase1(0),CkArrayIndex1D(rootIdx),myComm.getProxy());
    contribute(sizeof(splitKey),&splitKey,CkReduction::concat,cb);

    thread->suspend(); //Resumed by ampiParent::distGraphChildRegister
    MPI_Comm newComm=parent->getNextDistGraph()-1;
    *dest=newComm;
  }
  else if (type == MPI_INTER) {
    ampiSplitKey splitKey(parent->getNextInter(),color,key,myRank);
    int rootIdx=myComm.getIndexForRank(0);
    CkCallback cb(CkIndex_ampi::splitPhaseInter(0),CkArrayIndex1D(rootIdx),myComm.getProxy());
    contribute(sizeof(splitKey),&splitKey,CkReduction::concat,cb);

    thread->suspend(); //Resumed by ampiParent::interChildRegister
    MPI_Comm newComm=parent->getNextInter()-1;
    *dest=newComm;
  }
  else {
    ampiSplitKey splitKey(parent->getNextSplit(),color,key,myRank);
    int rootIdx=myComm.getIndexForRank(0);
    CkCallback cb(CkIndex_ampi::splitPhase1(0),CkArrayIndex1D(rootIdx),myComm.getProxy());
    contribute(sizeof(splitKey),&splitKey,CkReduction::concat,cb);

    thread->suspend(); //Resumed by ampiParent::splitChildRegister
    MPI_Comm newComm=parent->getNextSplit()-1;
    *dest=newComm;
  }
#if CMK_BIGSIM_CHARM
  _TRACE_BG_SET_INFO(NULL, "SPLIT_RESUME", NULL, 0);
#endif
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
  CkArrayID unusedAID;
  ampiCommStruct unusedComm;
  CkCallback cb(CkCallback::resumeThread);
  CProxy_ampi::ckNew(unusedAID, unusedComm, opts, cb);
  CkArrayCreatedMsg *newAmpiMsg = static_cast<CkArrayCreatedMsg*>(cb.thread_delay());
  CProxy_ampi newAmpi = newAmpiMsg->aid;
  delete newAmpiMsg;
  return newAmpi;
}

void ampi::splitPhase1(CkReductionMsg *msg) noexcept
{
  //Order the keys, which orders the ranks properly:
  int nKeys=msg->getSize()/sizeof(ampiSplitKey);
  ampiSplitKey *keys=(ampiSplitKey *)msg->getData();
  if (nKeys!=myComm.getSize()) CkAbort("ampi::splitReduce expected a split contribution from every rank!");
  qsort(keys,nKeys,sizeof(ampiSplitKey),compareAmpiSplitKey);

  MPI_Comm newComm = -1;
  for(int i=0;i<nKeys;i++){
    if(keys[i].nextSplitComm>newComm)
      newComm = keys[i].nextSplitComm;
  }

  //Loop over the sorted keys, which gives us the new arrays:
  int lastColor=keys[0].color-1; //The color we're building an array for
  CProxy_ampi lastAmpi; //The array for lastColor
  int lastRoot=0; //C value for new rank 0 process for latest color
  ampiCommStruct lastComm; //Communicator info. for latest color
  for (int c=0;c<nKeys;c++) {
    if (keys[c].color!=lastColor)
    { //Hit a new color-- need to build a new communicator and array
      lastColor=keys[c].color;
      lastRoot=c;

      if (c!=0) lastAmpi.doneInserting();
      lastAmpi = createNewChildAmpiSync();

      vector<int> indices; //Maps rank to array indices for new array
      for (int i=c;i<nKeys;i++) {
        if (keys[i].color!=lastColor) break; //Done with this color
        int idx=myComm.getIndexForRank(keys[i].rank);
        indices.push_back(idx);
      }

      //FIXME: create a new communicator for each color, instead of
      // (confusingly) re-using the same MPI_Comm number for each.
      lastComm=ampiCommStruct(newComm,lastAmpi,indices);
    }
    int newRank=c-lastRoot;
    int newIdx=lastComm.getIndexForRank(newRank);

    lastAmpi[newIdx].insert(parentProxy,lastComm);
  }
  lastAmpi.doneInserting();

  delete msg;
}

void ampi::splitPhaseInter(CkReductionMsg *msg) noexcept
{
  //Order the keys, which orders the ranks properly:
  int nKeys=msg->getSize()/sizeof(ampiSplitKey);
  ampiSplitKey *keys=(ampiSplitKey *)msg->getData();
  if (nKeys!=myComm.getSize()) CkAbort("ampi::splitReduce expected a split contribution from every rank!");
  qsort(keys,nKeys,sizeof(ampiSplitKey),compareAmpiSplitKey);

  MPI_Comm newComm = -1;
  for(int i=0;i<nKeys;i++){
    if(keys[i].nextSplitComm>newComm)
      newComm = keys[i].nextSplitComm; // FIXME: use nextSplitr instead of nextInter?
  }

  //Loop over the sorted keys, which gives us the new arrays:
  int lastColor=keys[0].color-1; //The color we're building an array for
  CProxy_ampi lastAmpi; //The array for lastColor
  int lastRoot=0; //C value for new rank 0 process for latest color
  ampiCommStruct lastComm; //Communicator info. for latest color

  lastAmpi = createNewChildAmpiSync();

  for (int c=0;c<nKeys;c++) {
    vector<int> indices; // Maps rank to array indices for new array
    if (keys[c].color!=lastColor)
    { //Hit a new color-- need to build a new communicator and array
      lastColor=keys[c].color;
      lastRoot=c;

      for (int i=c;i<nKeys;i++) {
        if (keys[i].color!=lastColor) break; //Done with this color
        int idx=myComm.getIndexForRank(keys[i].rank);
        indices.push_back(idx);
      }

      if (c==0) {
        lastComm=ampiCommStruct(newComm,lastAmpi,indices, myComm.getRemoteIndices());
        for (int i=0; i<indices.size(); i++) {
          lastAmpi[indices[i]].insert(parentProxy,lastComm);
        }
        lastAmpi.doneInserting();
      }
    }
  }

  parentProxy[0].ExchangeProxy(lastAmpi);
  delete msg;
}

//...newly created array elements register with the parent, which calls:
void ampiParent::splitChildRegister(const ampiCommStruct &s) noexcept {
  int idx=s.getComm()-MPI_COMM_FIRST_SPLIT;
  if (splitComm.size()<=idx) splitComm.resize(idx+1);
  splitComm[idx]=new ampiCommStruct(s);
  thread->resume(); //Matches suspend at end of ampi::split
}

//-----------------create communicator from group--------------
// The procedure is like that of comm_split very much,
// so the code is shamelessly copied from above
//   1. reduction to make sure all members have called
//   2. the root in the old communicator create the new array
//   3. ampiParent::register is called to register new array as new comm
void ampi::commCreate(const vector<int>& vec,MPI_Comm* newcomm) noexcept {
  int rootIdx=vec[0];
  tmpVec = vec;
  CkCallback cb(CkReductionTarget(ampi,commCreatePhase1),CkArrayIndex1D(rootIdx),myComm.getProxy());
  MPI_Comm nextgroup = parent->getNextGroup();
  contribute(sizeof(nextgroup), &nextgroup,CkReduction::max_int,cb);

  if(getPosOp(thisIndex,vec)>=0){
    thread->suspend(); //Resumed by ampiParent::groupChildRegister
    MPI_Comm retcomm = parent->getNextGroup()-1;
    *newcomm = retcomm;
  }else{
    *newcomm = MPI_COMM_NULL;
  }
}

void ampi::insertNewChildAmpiElements(MPI_Comm nextComm, CProxy_ampi newAmpi) noexcept {
  ampiCommStruct newCommStruct = ampiCommStruct(nextComm, newAmpi, tmpVec);
  for (int i = 0; i < tmpVec.size(); ++i)
    newAmpi[tmpVec[i]].insert(parentProxy, newCommStruct);
  newAmpi.doneInserting();
}

void ampi::commCreatePhase1(MPI_Comm nextGroupComm) noexcept {
  CProxy_ampi newAmpi = createNewChildAmpiSync();
  insertNewChildAmpiElements(nextGroupComm, newAmpi);
}

void ampiParent::groupChildRegister(const ampiCommStruct &s) noexcept {
  int idx=s.getComm()-MPI_COMM_FIRST_GROUP;
  if (groupComm.size()<=idx) groupComm.resize(idx+1);
  groupComm[idx]=new ampiCommStruct(s);
  thread->resume(); //Matches suspend at end of ampi::split
}

/* Virtual topology communicator creation */

// 0-dimensional cart comm: rank 0 creates a dup of COMM_SELF with topo info.
MPI_Comm ampi::cartCreate0D() noexcept {
  if (getRank() == 0) {
    tmpVec.clear();
    tmpVec.push_back(0);
    commCreatePhase1(parent->getNextCart());
    MPI_Comm newComm = parent->getNextCart()-1;
    ampiCommStruct &newCommStruct = getAmpiParent()->getCart(newComm);
    ampiTopology *newTopo = newCommStruct.getTopology();
    newTopo->setndims(0);
    return newComm;
  }
  else {
    return MPI_COMM_NULL;
  }
}

MPI_Comm ampi::cartCreate(vector<int>& vec, int ndims, const int* dims) noexcept {
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

  int rootIdx = vec[0];
  tmpVec = vec;
  CkCallback cb(CkReductionTarget(ampi,commCreatePhase1),CkArrayIndex1D(rootIdx),myComm.getProxy());

  MPI_Comm nextcart = parent->getNextCart();
  contribute(sizeof(nextcart), &nextcart,CkReduction::max_int,cb);

  if (getPosOp(thisIndex,vec)>=0) {
    thread->suspend(); //Resumed by ampiParent::cartChildRegister
    return parent->getNextCart()-1;
  } else {
    return MPI_COMM_NULL;
  }
}

void ampiParent::cartChildRegister(const ampiCommStruct &s) noexcept {
  int idx=s.getComm()-MPI_COMM_FIRST_CART;
  if (cartComm.size()<=idx) {
    cartComm.resize(idx+1);
    cartComm.length()=idx+1;
  }
  cartComm[idx]=new ampiCommStruct(s,MPI_CART);
  thread->resume(); //Matches suspend at end of ampi::cartCreate
}

void ampi::graphCreate(const vector<int>& vec,MPI_Comm* newcomm) noexcept {
  int rootIdx=vec[0];
  tmpVec = vec;
  CkCallback cb(CkReductionTarget(ampi,commCreatePhase1),CkArrayIndex1D(rootIdx),
      myComm.getProxy());
  MPI_Comm nextgraph = parent->getNextGraph();
  contribute(sizeof(nextgraph), &nextgraph,CkReduction::max_int,cb);

  if(getPosOp(thisIndex,vec)>=0){
    thread->suspend(); //Resumed by ampiParent::graphChildRegister
    MPI_Comm retcomm = parent->getNextGraph()-1;
    *newcomm = retcomm;
  }else
    *newcomm = MPI_COMM_NULL;
}

void ampiParent::graphChildRegister(const ampiCommStruct &s) noexcept {
  int idx=s.getComm()-MPI_COMM_FIRST_GRAPH;
  if (graphComm.size()<=idx) {
    graphComm.resize(idx+1);
    graphComm.length()=idx+1;
  }
  graphComm[idx]=new ampiCommStruct(s,MPI_GRAPH);
  thread->resume(); //Matches suspend at end of ampi::graphCreate
}

void ampi::distGraphCreate(const vector<int>& vec, MPI_Comm* newcomm) noexcept
{
  int rootIdx = vec[0];
  tmpVec = vec;
  CkCallback cb(CkReductionTarget(ampi,commCreatePhase1), CkArrayIndex1D(rootIdx), myComm.getProxy());
  MPI_Comm nextDistGraph = parent->getNextDistGraph();
  contribute(sizeof(nextDistGraph), &nextDistGraph, CkReduction::max_int, cb);

  if (getPosOp(thisIndex,vec) >= 0) {
    thread->suspend(); //Resumed by ampiParent::distGraphChildRegister
    MPI_Comm retcomm = parent->getNextDistGraph()-1;
    *newcomm = retcomm;
  }
  else {
    *newcomm = MPI_COMM_NULL;
  }
}

void ampiParent::distGraphChildRegister(const ampiCommStruct &s) noexcept
{
  int idx = s.getComm()-MPI_COMM_FIRST_DIST_GRAPH;
  if (distGraphComm.size() <= idx) {
    distGraphComm.resize(idx+1);
    distGraphComm.length() = idx+1;
  }
  distGraphComm[idx] = new ampiCommStruct(s,MPI_DIST_GRAPH);
  thread->resume(); //Matches suspend at end of ampi::distGraphCreate
}

void ampi::intercommCreate(const vector<int>& remoteVec, const int root, MPI_Comm tcomm, MPI_Comm *ncomm) noexcept {
  if (thisIndex==root) { // not everybody gets the valid rvec
    tmpVec = remoteVec;
  }
  CkCallback cb(CkReductionTarget(ampi, intercommCreatePhase1),CkArrayIndex1D(root),myComm.getProxy());
  MPI_Comm nextinter = parent->getNextInter();
  contribute(sizeof(nextinter), &nextinter,CkReduction::max_int,cb);
  thread->suspend(); //Not resumed by ampiParent::interChildRegister. Resumed by ExchangeProxy.
  *ncomm = parent->getNextInter()-1;
}

void ampi::intercommCreatePhase1(MPI_Comm nextInterComm) noexcept {

  CProxy_ampi newAmpi = createNewChildAmpiSync();
  const vector<int>& lgroup = myComm.getIndices();
  ampiCommStruct newCommstruct = ampiCommStruct(nextInterComm,newAmpi,lgroup,tmpVec);
  for(int i=0;i<lgroup.size();i++){
    int newIdx=lgroup[i];
    newAmpi[newIdx].insert(parentProxy,newCommstruct);
  }
  newAmpi.doneInserting();

  parentProxy[0].ExchangeProxy(newAmpi);
}

void ampiParent::interChildRegister(const ampiCommStruct &s) noexcept {
  int idx=s.getComm()-MPI_COMM_FIRST_INTER;
  if (interComm.size()<=idx) interComm.resize(idx+1);
  interComm[idx]=new ampiCommStruct(s);
  // don't resume the thread yet, till parent set remote proxy
}

void ampi::intercommMerge(int first, MPI_Comm *ncomm) noexcept { // first valid only at local root
  if(myRank == 0 && first == 1){ // first (lower) group creates the intracommunicator for the higher group
    vector<int> lvec = myComm.getIndices();
    vector<int> rvec = myComm.getRemoteIndices();
    int rsize = rvec.size();
    tmpVec = lvec;
    for(int i=0;i<rsize;i++)
      tmpVec.push_back(rvec[i]);
    if(tmpVec.size()==0) CkAbort("Error in ampi::intercommMerge: merging empty comms!\n");
  }else{
    tmpVec.resize(0);
  }

  int rootIdx=myComm.getIndexForRank(0);
  CkCallback cb(CkReductionTarget(ampi, intercommMergePhase1),CkArrayIndex1D(rootIdx),myComm.getProxy());
  MPI_Comm nextintra = parent->getNextIntra();
  contribute(sizeof(nextintra), &nextintra,CkReduction::max_int,cb);

  thread->suspend(); //Resumed by ampiParent::interChildRegister
  MPI_Comm newcomm=parent->getNextIntra()-1;
  *ncomm=newcomm;
}

void ampi::intercommMergePhase1(MPI_Comm nextIntraComm) noexcept {
  // gets called on two roots, first root creates the comm
  if(tmpVec.size()==0) return;
  CProxy_ampi newAmpi = createNewChildAmpiSync();
  insertNewChildAmpiElements(nextIntraComm, newAmpi);
}

void ampiParent::intraChildRegister(const ampiCommStruct &s) noexcept {
  int idx=s.getComm()-MPI_COMM_FIRST_INTRA;
  if (intraComm.size()<=idx) intraComm.resize(idx+1);
  intraComm[idx]=new ampiCommStruct(s);
  thread->resume(); //Matches suspend at end of ampi::split
}

void ampi::topoDup(int topoType, int rank, MPI_Comm comm, MPI_Comm *newComm) noexcept
{
  if (getAmpiParent()->isInter(comm)) {
    split(0, rank, newComm, MPI_INTER);
  } else {
    split(0, rank, newComm, topoType);

    if (topoType != MPI_UNDEFINED) {
      ampiTopology *topo, *newTopo;
      if (topoType == MPI_CART) {
        topo = getAmpiParent()->getCart(comm).getTopology();
        newTopo = getAmpiParent()->getCart(*newComm).getTopology();
      } else if (topoType == MPI_GRAPH) {
        topo = getAmpiParent()->getGraph(comm).getTopology();
        newTopo = getAmpiParent()->getGraph(*newComm).getTopology();
      } else {
        CkAssert(topoType == MPI_DIST_GRAPH);
        topo = getAmpiParent()->getDistGraph(comm).getTopology();
        newTopo = getAmpiParent()->getDistGraph(*newComm).getTopology();
      }
      newTopo->dup(topo);
    }
  }
}

//------------------------ communication -----------------------
const ampiCommStruct &universeComm2CommStruct(MPI_Comm universeNo) noexcept
{
  if (universeNo>MPI_COMM_WORLD) {
    int worldDex=universeNo-MPI_COMM_WORLD-1;
    if (worldDex>=_mpi_nworlds)
      CkAbort("Bad world communicator passed to universeComm2CommStruct");
    return mpi_worlds[worldDex];
  }
  CkAbort("Bad communicator passed to universeComm2CommStruct");
  return mpi_worlds[0]; // meaningless return
}

void ampiParent::block() noexcept {
  thread->suspend();
}

void ampiParent::yield() noexcept {
  thread->schedule();
}

void ampi::unblock() noexcept {
  thread->resume();
}

ampiParent* ampiParent::blockOnRecv() noexcept {
  resumeOnRecv = true;
  // In case this thread is migrated while suspended,
  // save myComm to get the ampi instance back. Then
  // return "dis" in case the caller needs it.
  thread->suspend();
  ampiParent* dis = getAmpiParent();
  dis->resumeOnRecv = false;
  return dis;
}

ampi* ampi::blockOnRecv() noexcept {
  parent->resumeOnRecv = true;
  // In case this thread is migrated while suspended,
  // save myComm to get the ampi instance back. Then
  // return "dis" in case the caller needs it.
  MPI_Comm comm = myComm.getComm();
  thread->suspend();
  ampi *dis = getAmpiInstance(comm);
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
ampi* ampi::blockOnColl() noexcept {
#if CMK_BIGSIM_CHARM
  void *curLog; // store current log in timeline
  _TRACE_BG_TLINE_END(&curLog);
#if CMK_TRACE_IN_CHARM
  if(CpvAccess(traceOn)) traceSuspend();
#endif
#endif

  CkAssert(parent->resumeOnColl == true);
  MPI_Comm comm = myComm.getComm();
  thread->suspend();
  ampi *dis = getAmpiInstance(comm);
  dis->parent->resumeOnColl = false;

#if (defined(_FAULT_MLOG_) || defined(_FAULT_CAUSAL_))
  CpvAccess(_currentObj) = dis;
#endif
#if CMK_BIGSIM_CHARM
#if CMK_TRACE_IN_CHARM
  if(CpvAccess(traceOn)) CthTraceResume(dis->thread->getThread());
#endif
  TRACE_BG_AMPI_BREAK(dis->thread->getThread(), "RECV_RESUME", NULL, 0, 0);
  if (dis->parent->blockingReq->eventPe == CkMyPe()) _TRACE_BG_ADD_BACKWARD_DEP(dis->parent->blockingReq->event);
#endif

  delete dis->parent->blockingReq; dis->parent->blockingReq = NULL;
  return dis;
}

void ampi::ssend_ack(int sreq_idx) noexcept {
  if (sreq_idx == 1)
    thread->resume();           // MPI_Ssend
  else {
    sreq_idx -= 2;              // start from 2
    AmpiRequestList& reqs = getReqs();
    AmpiRequest *sreq = reqs[sreq_idx];
    sreq->setComplete(true);
    handleBlockedReq(sreq);
    resumeThreadIfReady();
  }
}

void ampi::injectMsg(int size, char* buf) noexcept
{
  generic(makeAmpiMsg(thisIndex, 0, thisIndex, (void*)buf, size, MPI_CHAR, MPI_COMM_WORLD, 0));
}

void ampi::generic(AmpiMsg* msg) noexcept
{
  MSG_ORDER_DEBUG(
    CkPrintf("AMPI vp %d arrival: tag=%d, src=%d, comm=%d (seq %d) resumeOnRecv %d\n",
             thisIndex, msg->getTag(), msg->getSrcRank(), getComm(), msg->getSeq(), parent->resumeOnRecv);
  )
#if CMK_BIGSIM_CHARM
  TRACE_BG_ADD_TAG("AMPI_generic");
  msg->event = NULL;
#endif

  if(msg->getSeq() != 0) {
    int seqIdx = msg->getSeqIdx();
    int n=oorder.put(seqIdx,msg);
    if (n>0) { // This message was in-order
      inorder(msg);
      if (n>1) { // It enables other, previously out-of-order messages
        while((msg=oorder.getOutOfOrder(seqIdx))!=0) {
          inorder(msg);
        }
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
#if CMK_BIGSIM_CHARM
  TRACE_BG_ADD_TAG("AMPI_generic");
  msg->event = NULL;
#endif

  CkAssert(msg->getSeq() != 0);
  int seqIdx = msg->getSeqIdx();
  int n=oorder.put(seqIdx,msg);
  if (n>0) { // This message was in-order
    inorderBcast(msg, false); // inorderBcast() is [nokeep]-aware, unlike inorder()
    if (n>1) { // It enables other, previously out-of-order messages
      while((msg=oorder.getOutOfOrder(seqIdx))!=0) {
        inorderBcast(msg, true);
      }
    }
  }
  // [nokeep] entry method, so do not delete msg
  resumeThreadIfReady();
}

inline static AmpiRequestList &getReqs() noexcept;

void AmpiRequestList::freeNonPersReq(int &idx) noexcept {
  ampiParent* pptr = getAmpiParent();
  if (!reqs[idx]->isPersistent()) {
    free(pptr->reqPool, idx, pptr->getDDT());
    idx = MPI_REQUEST_NULL;
  }
}

void AmpiRequestList::free(AmpiRequestPool &reqPool, int idx, CkDDT *ddt) noexcept {
  if (idx < 0) return;
  reqs[idx]->free(ddt);
  reqPool.deleteReq(reqs[idx]);
  reqs[idx] = NULL;
  startIdx = std::min(idx, startIdx);
}

void ampi::inorder(AmpiMsg* msg) noexcept
{
  MSG_ORDER_DEBUG(
    CkPrintf("AMPI vp %d inorder: tag=%d, src=%d, comm=%d (seq %d)\n",
             thisIndex, msg->getTag(), msg->getSrcRank(), getComm(), msg->getSeq());
  )

#if CMK_BIGSIM_CHARM
  _TRACE_BG_TLINE_END(&msg->event); // store current log
  msg->eventPe = CkMyPe();
#endif

  //Check posted recvs:
  int tag = msg->getTag();
  int srcRank = msg->getSrcRank();
  AmpiRequest* req = postedReqs.get(tag, srcRank);
  if (req) { // receive posted
    handleBlockedReq(req);
    req->receive(this, msg);
  } else {
    unexpectedMsgs.put(msg);
  }
}

void ampi::inorderBcast(AmpiMsg* msg, bool deleteMsg) noexcept
{
  MSG_ORDER_DEBUG(
    CkPrintf("AMPI vp %d inorder bcast: tag=%d, src=%d, comm=%d (seq %d)\n",
             thisIndex, msg->getTag(), msg->getSrcRank(), getComm(), msg->getSeq());
  )

#if CMK_BIGSIM_CHARM
  _TRACE_BG_TLINE_END(&msg->event); // store current log
  msg->eventPe = CkMyPe();
#endif

  //Check posted recvs:
  int tag = msg->getTag();
  int srcRank = msg->getSrcRank();
  AmpiRequest* req = postedBcastReqs.get(tag, srcRank);
  if (req) { // receive posted
    handleBlockedReq(req);
    req->receive(this, msg, deleteMsg);
  } else {
    // Reference the [nokeep] msg so it isn't freed by the runtime
    CmiReference(UsrToEnv(msg));
    unexpectedBcastMsgs.put(msg);
  }
}

static inline AmpiMsg* rdma2AmpiMsg(char *buf, int size, CMK_REFNUM_TYPE seq, int tag, int srcRank,
                                    int ssendReq) noexcept
{
  // Convert an Rdma message (parameter marshalled buffer) to an AmpiMsg
  AmpiMsg* msg = new (size, 0) AmpiMsg(seq, ssendReq, tag, srcRank, size);
  memcpy(msg->data, buf, size); // Assumes the buffer is contiguous
  return msg;
}

// RDMA version of ampi::generic
void ampi::genericRdma(char* buf, int size, CMK_REFNUM_TYPE seq, int tag, int srcRank, MPI_Comm destcomm, int ssendReq) noexcept
{
  MSG_ORDER_DEBUG(
    CkPrintf("[%d] in ampi::genericRdma on index %d, size=%d, seq=%d, srcRank=%d, tag=%d, comm=%d, ssendReq=%d\n",
             CkMyPe(), getIndexForRank(getRank()), size, seq, srcRank, tag, destcomm, ssendReq);
  )

  if (seq != 0) {
    int seqIdx = srcRank;
    int n = oorder.isInOrder(seqIdx, seq);
    if (n > 0) { // This message was in-order
      inorderRdma(buf, size, seq, tag, srcRank, destcomm, ssendReq);
      if (n > 1) { // It enables other, previously out-of-order messages
        AmpiMsg *msg = NULL;
        while ((msg = oorder.getOutOfOrder(seqIdx)) != 0) {
          inorder(msg);
        }
      }
    } else { // This message was out-of-order: stash it (as an AmpiMsg)
      AmpiMsg *msg = rdma2AmpiMsg(buf, size, seq, tag, srcRank, ssendReq);
      oorder.putOutOfOrder(seqIdx, msg);
    }
  } else { // Cross-world or system messages are unordered
    inorderRdma(buf, size, seq, tag, srcRank, destcomm, ssendReq);
  }

  resumeThreadIfReady();
}

// RDMA version of ampi::inorder
void ampi::inorderRdma(char* buf, int size, CMK_REFNUM_TYPE seq, int tag, int srcRank,
                       MPI_Comm comm, int ssendReq) noexcept
{
  MSG_ORDER_DEBUG(
    CkPrintf("AMPI vp %d inorderRdma: tag=%d, src=%d, comm=%d  (seq %d)\n",
             thisIndex, tag, srcRank, comm, seq);
  )

  //Check posted recvs:
  AmpiRequest* req = postedReqs.get(tag, srcRank);
  if (req) { // receive posted
    handleBlockedReq(req);
    req->receiveRdma(this, buf, size, ssendReq, srcRank, comm);
  } else {
    AmpiMsg* msg = rdma2AmpiMsg(buf, size, seq, tag, srcRank, ssendReq);
    unexpectedMsgs.put(msg);
  }
}

// Callback from ampi::genericRdma() signaling that the send buffer is now safe to re-use
void ampi::completedRdmaSend(CkDataMsg *msg) noexcept
{
  // refnum is the index into reqList for this SendReq
  int reqIdx = CkGetRefNum(msg);

  MSG_ORDER_DEBUG(
    CkPrintf("[%d] in ampi::completedRdmaSend on index %d, reqIdx = %d\n",
             CkMyPe(), parent->thisIndex, reqIdx);
  )

  AmpiRequestList& reqList = getReqs();
  AmpiRequest* sreq = reqList[reqIdx];
  sreq->setComplete(true);

  handleBlockedReq(sreq);
  resumeThreadIfReady();
  // CkDataMsg is allocated & freed by the runtime, so do not delete msg
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

AmpiMsg *ampi::makeAmpiMsg(int destRank,int t,int sRank,const void *buf,int count,
                           MPI_Datatype type,MPI_Comm destcomm, int ssendReq/*=0*/) noexcept
{
  CkDDT_DataType *ddt = getDDT()->getType(type);
  int len = ddt->getSize(count);
  CMK_REFNUM_TYPE seq = getSeqNo(destRank, destcomm, t);
  AmpiMsg *msg = CkpvAccess(msgPool).newAmpiMsg(seq, ssendReq, t, sRank, len);
  ddt->serialize((char*)buf, msg->getData(), count, msg->getLength(), PACK);
  return msg;
}

MPI_Request ampi::send(int t, int sRank, const void* buf, int count, MPI_Datatype type,
                       int rank, MPI_Comm destcomm, int ssendReq/*=0*/, AmpiSendType sendType/*=BLOCKING_SEND*/) noexcept
{
#if CMK_TRACE_IN_CHARM
  TRACE_BG_AMPI_BREAK(thread->getThread(), "AMPI_SEND", NULL, 0, 1);
#endif

#if (defined(_FAULT_MLOG_) || defined(_FAULT_CAUSAL_))
  MPI_Comm disComm = myComm.getComm();
  ampi *dis = getAmpiInstance(disComm);
  CpvAccess(_currentObj) = dis;
#endif

  const ampiCommStruct &dest=comm2CommStruct(destcomm);
  MPI_Request req = delesend(t,sRank,buf,count,type,rank,destcomm,dest.getProxy(),ssendReq,sendType);
  if (sendType == BLOCKING_SEND && req != MPI_REQUEST_NULL) {
    AmpiRequestList& reqList = getReqs();
    AmpiRequest *sreq = reqList[req];
    sreq->wait(MPI_STATUS_IGNORE);
    reqList.free(parent->reqPool, req, parent->getDDT());
    req = MPI_REQUEST_NULL;
  }

#if CMK_TRACE_IN_CHARM
  TRACE_BG_AMPI_BREAK(thread->getThread(), "AMPI_SEND_END", NULL, 0, 1);
#endif

  if (ssendReq == 1) {
    // waiting for receiver side
    parent->resumeOnRecv = false;            // so no one else awakes it
    parent->block();
  }

  return req;
}

void ampi::sendraw(int t, int sRank, void* buf, int len, CkArrayID aid, int idx) noexcept
{
  AmpiMsg *msg = new (len, 0) AmpiMsg(0, 0, t, sRank, len);
  memcpy(msg->getData(), buf, len);
  CProxy_ampi pa(aid);
  pa[idx].generic(msg);
}

CMK_REFNUM_TYPE ampi::getSeqNo(int destRank, MPI_Comm destcomm, int tag) noexcept {
  int seqIdx = (tag >= MPI_BCAST_TAG) ? COLL_SEQ_IDX : destRank;
  CMK_REFNUM_TYPE seq = 0;
  if (destcomm<=MPI_COMM_WORLD && tag<=MPI_BCAST_TAG) { //Not cross-module: set seqno
    seq = oorder.nextOutgoing(seqIdx);
  }
  return seq;
}

MPI_Request ampi::sendRdmaMsg(int t, int sRank, const void* buf, int size, MPI_Datatype type, int destIdx,
                              int destRank, MPI_Comm destcomm, CProxy_ampi arrProxy, int ssendReq) noexcept
{
  CMK_REFNUM_TYPE seq = getSeqNo(destRank, destcomm, t);

  if (ssendReq) { // Using a SsendReq to track matching receive, so no need for SendReq here
    arrProxy[destIdx].genericRdma(CkSendBuffer(buf), size, seq, t, sRank, destcomm, ssendReq);
    return MPI_REQUEST_NULL;
  }
  else { // Set up a SendReq to track completion of the send buffer
    MPI_Request req = postReq(parent->reqPool.newReq<SendReq>(type, destcomm, getDDT()));
    CkCallback completedSendCB(CkIndex_ampi::completedRdmaSend(NULL), thisProxy[thisIndex], true/*inline*/);
    completedSendCB.setRefnum(req);

    arrProxy[destIdx].genericRdma(CkSendBuffer(buf, completedSendCB), size, seq, t, sRank, destcomm, ssendReq);
    return req;
  }
}

// Call genericRdma inline on the local destination object
MPI_Request ampi::sendLocalMsg(int t, int sRank, const void* buf, int size, MPI_Datatype type, int destRank,
                               MPI_Comm destcomm, ampi* destPtr, int ssendReq, AmpiSendType sendType) noexcept
{
  CMK_REFNUM_TYPE seq = getSeqNo(destRank, destcomm, t);

  destPtr->genericRdma((char*)buf, size, seq, t, sRank, destcomm, ssendReq);

  if (ssendReq || sendType == BLOCKING_SEND) {
    return MPI_REQUEST_NULL;
  }
  else { // SendReq is pre-completed since we directly copied the send buffer
    return postReq(parent->reqPool.newReq<SendReq>(type, destcomm, getDDT(), AMPI_REQ_COMPLETED));
  }
}

MPI_Request ampi::delesend(int t, int sRank, const void* buf, int count, MPI_Datatype type,
                           int rank, MPI_Comm destcomm, CProxy_ampi arrProxy, int ssendReq,
                           AmpiSendType sendType) noexcept
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

  MSG_ORDER_DEBUG(
    CkPrintf("AMPI vp %d send: tag=%d, src=%d, comm=%d (to %d)\n",thisIndex,t,sRank,destcomm,destIdx);
  )

  ampi *destPtr = arrProxy[destIdx].ckLocal();
  CkDDT_DataType *ddt = getDDT()->getType(type);
  int size = ddt->getSize(count);
  if (ddt->isContig()) {
#if AMPI_LOCAL_IMPL
    if (destPtr != NULL) {
      return sendLocalMsg(t, sRank, buf, size, type, rank, destcomm, destPtr, ssendReq, sendType);
    }
#endif
#if AMPI_RDMA_IMPL
    if (size >= AMPI_RDMA_THRESHOLD ||
       (size >= AMPI_SMP_RDMA_THRESHOLD && destLikelyWithinProcess(arrProxy, destIdx)))
    {
      return sendRdmaMsg(t, sRank, buf, size, type, destIdx, rank, destcomm, arrProxy, ssendReq);
    }
#endif
  }
#if AMPI_LOCAL_IMPL
  if (destPtr != NULL) {
    destPtr->generic(makeAmpiMsg(rank, t, sRank, buf, count, type, destcomm, ssendReq));
    return MPI_REQUEST_NULL;
  } else
#endif
  {
    arrProxy[destIdx].generic(makeAmpiMsg(rank, t, sRank, buf, count, type, destcomm, ssendReq));
    return MPI_REQUEST_NULL;
  }
}

void ampi::processAmpiMsg(AmpiMsg *msg, void* buf, MPI_Datatype type, int count) noexcept
{
  int ssendReq = msg->getSsendReq();
  if (ssendReq > 0) { // send an ack to sender
    int srcRank = msg->getSrcRank();
    int srcIdx = getIndexForRank(srcRank);
    thisProxy[srcIdx].ssend_ack(ssendReq);
  }

  CkDDT_DataType *ddt = getDDT()->getType(type);

  ddt->serialize((char*)buf, msg->getData(), count, msg->getLength(), UNPACK);
}

// RDMA version of ampi::processAmpiMsg
void ampi::processRdmaMsg(const void *sbuf, int slength, int ssendReq, int srank, void* rbuf,
                          int rcount, MPI_Datatype rtype, MPI_Comm comm) noexcept
{
  if (ssendReq > 0) { // send an ack to sender
    int srcIdx = getIndexForRank(srank);
    thisProxy[srcIdx].ssend_ack(ssendReq);
  }

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
  vector<void *> contributionData(commSize);
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
    vector<char> deserializedBuf(contributionExtent);
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

static inline bool handle_MPI_PROC_NULL(int src, MPI_Comm comm, MPI_Status* sts) noexcept
{
  if (src == MPI_PROC_NULL) {
    clearStatus(sts);
    if (sts != MPI_STATUS_IGNORE) sts->MPI_SOURCE = MPI_PROC_NULL;
    return true;
  }
  return false;
}

int ampi::recv(int t, int s, void* buf, int count, MPI_Datatype type, MPI_Comm comm, MPI_Status *sts) noexcept
{
  MPI_Comm disComm = myComm.getComm();
  if (handle_MPI_PROC_NULL(s, disComm, sts)) return 0;

#if CMK_BIGSIM_CHARM
   void *curLog; // store current log in timeline
  _TRACE_BG_TLINE_END(&curLog);
#if CMK_TRACE_IN_CHARM
  if(CpvAccess(traceOn)) traceSuspend();
#endif
#endif

  if (isInter()) {
    s = myComm.getIndexForRemoteRank(s);
  }

  MSG_ORDER_DEBUG(
    CkPrintf("AMPI vp %d blocking recv: tag=%d, src=%d, comm=%d\n",thisIndex,t,s,comm);
  )

  ampi *dis = getAmpiInstance(disComm);
  MPI_Status tmpStatus;
  AmpiMsg* msg = unexpectedMsgs.get(t, s, (sts == MPI_STATUS_IGNORE) ? (int*)&tmpStatus : (int*)sts);
  if (msg) { // the matching message has already arrived
    if (sts != MPI_STATUS_IGNORE) {
      sts->MPI_SOURCE = msg->getSrcRank();
      sts->MPI_TAG    = msg->getTag();
      sts->MPI_COMM   = comm;
      sts->MPI_LENGTH = msg->getLength();
      sts->MPI_CANCEL = 0;
    }
    processAmpiMsg(msg, buf, type, count);
#if CMK_BIGSIM_CHARM
    TRACE_BG_AMPI_BREAK(thread->getThread(), "RECV_RESUME", NULL, 0, 0);
    if (msg->eventPe == CkMyPe()) _TRACE_BG_ADD_BACKWARD_DEP(msg->event);
#endif
    CkpvAccess(msgPool).deleteAmpiMsg(msg);
  }
  else { // post a request and block until the matching message arrives
    int request = postReq(dis->parent->reqPool.newReq<IReq>(buf, count, type, s, t, comm, getDDT(), AMPI_REQ_BLOCKED));
    CkAssert(parent->numBlockedReqs == 0);
    parent->numBlockedReqs = 1;
    dis = dis->blockOnRecv(); // "dis" is updated in case an ampi thread is migrated while waiting for a message
    parent = dis->parent;
    AmpiRequestList& reqs = parent->getReqs();
    if (sts != MPI_STATUS_IGNORE) {
      AmpiRequest& req = *reqs[request];
      sts->MPI_SOURCE = req.getSrcRank();
      sts->MPI_TAG    = req.getTag();
      sts->MPI_COMM   = req.getComm();
      sts->MPI_LENGTH = req.getNumReceivedBytes(getDDT());
      sts->MPI_CANCEL = 0;
    }
    reqs.freeNonPersReq(request);
  }

#if (defined(_FAULT_MLOG_) || defined(_FAULT_CAUSAL_))
  CpvAccess(_currentObj) = dis;
  MSG_ORDER_DEBUG( printf("[%d] AMPI thread rescheduled  to Index %d buf %p src %d\n",CkMyPe(),dis->thisIndex,buf,s); )
#endif
#if CMK_BIGSIM_CHARM && CMK_TRACE_IN_CHARM
  //Due to the reason mentioned the in the else-statement above, we need to
  //use "dis" as "this" in the case of migration (or out-of-core execution in BigSim)
  if(CpvAccess(traceOn)) CthTraceResume(dis->thread->getThread());
#endif

  return 0;
}

void ampi::probe(int t, int s, MPI_Comm comm, MPI_Status *sts) noexcept
{
  if (handle_MPI_PROC_NULL(s, comm, sts)) return;

#if CMK_BIGSIM_CHARM
  void *curLog; // store current log in timeline
  _TRACE_BG_TLINE_END(&curLog);
#endif

  ampi *dis = getAmpiInstance(comm);
  AmpiMsg *msg = NULL;
  while(1) {
    MPI_Status tmpStatus;
    msg = unexpectedMsgs.probe(t, s, (sts == MPI_STATUS_IGNORE) ? (int*)&tmpStatus : (int*)sts);
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

#if CMK_BIGSIM_CHARM
  _TRACE_BG_SET_INFO((char *)msg, "PROBE_RESUME",  &curLog, 1);
#endif
}

void ampi::mprobe(int t, int s, MPI_Comm comm, MPI_Status *sts, MPI_Message *message) noexcept
{
  if (handle_MPI_PROC_NULL(s, comm, sts)) {
    *message = MPI_MESSAGE_NO_PROC;
    return;
  }

#if CMK_BIGSIM_CHARM
  void *curLog; // store current log in timeline
  _TRACE_BG_TLINE_END(&curLog);
#endif

  ampi *dis = this;
  AmpiMsg *msg = NULL;
  while(1) {
    MPI_Status tmpStatus;
    // We call get() rather than probe() here because we want to remove this msg
    // from ampi::unexpectedMsgs and then insert it into ampiParent::matchedMsgs
    msg = unexpectedMsgs.get(t, s, (sts == MPI_STATUS_IGNORE) ? (int*)&tmpStatus : (int*)sts);
    if (msg)
      break;
    // "dis" is updated in case an ampi thread is migrated while waiting for a message
    dis = dis->blockOnRecv();
  }

  msg->setComm(comm);
  *message = parent->putMatchedMsg(msg);

  if (sts != MPI_STATUS_IGNORE) {
    sts->MPI_SOURCE = msg->getSrcRank();
    sts->MPI_TAG    = msg->getTag();
    sts->MPI_COMM   = msg->getComm();
    sts->MPI_LENGTH = msg->getLength();
    sts->MPI_CANCEL = 0;
  }

#if CMK_BIGSIM_CHARM
  _TRACE_BG_SET_INFO((char *)msg, "MPROBE_RESUME",  &curLog, 1);
#endif
}

int ampi::iprobe(int t, int s, MPI_Comm comm, MPI_Status *sts) noexcept
{
  if (handle_MPI_PROC_NULL(s, comm, sts)) return 1;

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
#if CMK_BIGSIM_CHARM
  void *curLog; // store current log in timeline
  _TRACE_BG_TLINE_END(&curLog);
#endif
  thread->schedule();
#if CMK_BIGSIM_CHARM
  _TRACE_BG_SET_INFO(NULL, "IPROBE_RESUME",  &curLog, 1);
#endif
  return 0;
}

int ampi::improbe(int tag, int source, MPI_Comm comm, MPI_Status *sts,
                  MPI_Message *message) noexcept
{
  if (handle_MPI_PROC_NULL(source, comm, sts)) {
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

#if CMK_BIGSIM_CHARM
  void *curLog; // store current log in timeline
  _TRACE_BG_TLINE_END(&curLog);
#endif
  thread->schedule();
#if CMK_BIGSIM_CHARM
  _TRACE_BG_SET_INFO(NULL, "IMPROBE_RESUME",  &curLog, 1);
#endif
  return 0;
}

void ampi::bcast(int root, void* buf, int count, MPI_Datatype type, MPI_Comm destcomm) noexcept
{
  MPI_Request req;

  if (root==getRank()) {
#if (defined(_FAULT_MLOG_) || defined(_FAULT_CAUSAL_))
    CpvAccess(_currentObj) = this;
#endif
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
#if (defined(_FAULT_MLOG_) || defined(_FAULT_CAUSAL_))
    CpvAccess(_currentObj) = this;
#endif
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
#if (defined(_FAULT_MLOG_) || defined(_FAULT_CAUSAL_))
    CpvAccess(_currentObj) = this;
#endif
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
#if (defined(_FAULT_MLOG_) || defined(_FAULT_CAUSAL_))
    CpvAccess(_currentObj) = this;
#endif
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
  AmpiMsg *msg = new (len, 0) AmpiMsg(0, 0, MPI_BCAST_TAG, 0, len);
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
                             sendcount, sendtype, i, intercomm, 0, I_SEND);
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
                             sendcounts[i], sendtype, i, intercomm, 0, I_SEND);
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
  if (msg->getSeq() < el.getSeqIncoming())
    CkAbort("AMPI Logic error: received late out-of-order message!\n");
#endif
  if (seqIdx == COLL_SEQ_IDX) CmiReference(UsrToEnv(msg)); // bcast msg is [nokeep]
  out.enq(msg);
  el.incNumOutOfOrder(); // We have another message in the out-of-order queue
}

AmpiMsg *AmpiSeqQ::getOutOfOrder(int seqIdx) noexcept
{
  AmpiOtherElement &el=elements[seqIdx];
  if (el.getNumOutOfOrder()==0) return 0; // No more out-of-order left.
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
  return 0;
}

void AmpiRequest::print() const noexcept {
  CkPrintf("In AmpiRequest: buf=%p, count=%d, type=%d, src=%d, tag=%d, comm=%d, reqIdx=%d, complete=%d, blocked=%d\n",
           buf, count, type, src, tag, comm, reqIdx, (int)isComplete(), (int)isBlocked());
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
  CkPrintf("In ATAReq: num_reqs=%d\n", reqs.size());
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
ampiParent *getAmpiParent() noexcept {
  ampiParent *p = CtvAccess(ampiPtr);
#if CMK_ERROR_CHECKING
  if (p==NULL) CkAbort("Cannot call MPI routines before AMPI is initialized.\n");
#endif
  return p;
}

ampi *getAmpiInstance(MPI_Comm comm) noexcept {
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

int testRequest(MPI_Request *reqIdx, int *flag, MPI_Status *sts) noexcept {
  if(*reqIdx==MPI_REQUEST_NULL){
    *flag = 1;
    clearStatus(sts);
    return MPI_SUCCESS;
  }
  checkRequest(*reqIdx);
  ampiParent* pptr = getAmpiParent();
  AmpiRequestList& reqList = pptr->getReqs();
  AmpiRequest& req = *reqList[*reqIdx];
  if(1 == (*flag = req.test())){
    req.wait(sts);
    reqList.freeNonPersReq(*reqIdx);
  }
  return MPI_SUCCESS;
}

int testRequestNoFree(MPI_Request *reqIdx, int *flag, MPI_Status *sts) noexcept {
  if(*reqIdx==MPI_REQUEST_NULL){
    *flag = 1;
    clearStatus(sts);
    return MPI_SUCCESS;
  }
  checkRequest(*reqIdx);
  AmpiRequestList& reqList = getReqs();
  AmpiRequest& req = *reqList[*reqIdx];
  *flag = req.test();
  if(*flag)
    req.wait(sts);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Is_thread_main, int *flag)
{
  AMPI_API_INIT("AMPI_Is_thread_main");
  if (isAmpiThread()) {
    *flag = 1;
  } else {
    *flag = 0;
  }
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Query_thread, int *provided)
{
  AMPI_API("AMPI_Query_thread");
  *provided = CkpvAccess(ampiThreadLevel);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Init_thread, int *p_argc, char*** p_argv, int required, int *provided)
{
  if (nodeinit_has_been_called) {
    AMPI_API_INIT("AMPI_Init_thread");

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
  else
  { /* Charm hasn't been started yet! */
    CkAbort("MPI_Init_thread> AMPI has not been initialized! Possibly due to AMPI requiring '#include \"mpi.h\" be in the same file as main() in C/C++ programs and \'program main\' be renamed to \'subroutine mpi_main\' in Fortran programs!");
    return MPI_SUCCESS;
  }
}

AMPI_API_IMPL(int, MPI_Init, int *p_argc, char*** p_argv)
{
  if (nodeinit_has_been_called) {
    AMPI_API_INIT("AMPI_Init");
    char **argv;
    if (p_argv) argv=*p_argv;
    else argv=CkGetArgv();
    ampiInit(argv);
    if (p_argc) *p_argc=CmiGetArgc(argv);
  }
  else
  { /* Charm hasn't been started yet! */
    CkAbort("MPI_Init> AMPI has not been initialized! Possibly due to AMPI requiring '#include \"mpi.h\" be in the same file as main() in C/C++ programs and \'program main\' be renamed to \'subroutine mpi_main\' in Fortran programs!");
  }

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Initialized, int *isInit)
{
  if (nodeinit_has_been_called) {
    AMPI_API_INIT("AMPI_Initialized");     /* in case charm init not called */
    *isInit=CtvAccess(ampiInitDone);
  }
  else {
    *isInit=0;
  }
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Finalized, int *isFinalized)
{
  AMPI_API_INIT("AMPI_Finalized");     /* in case charm init not called */
  *isFinalized=(CtvAccess(ampiFinalized)) ? 1 : 0;
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Comm_rank, MPI_Comm comm, int *rank)
{
  AMPI_API("AMPI_Comm_rank");

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
  AMPI_API("AMPI_Comm_size");

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
  AMPI_API("AMPI_Comm_compare");

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
    vector<int> ind1, ind2;
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
  AMPI_API_INIT("AMPI_Exit");
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
  AMPI_API("AMPI_Finalize");

#if AMPI_PRINT_IDLE
  CkPrintf("[%d] Idle time %fs.\n", CkMyPe(), totalidle);
#endif
  CtvAccess(ampiFinalized)=true;

#if AMPI_PRINT_MSG_SIZES
  getAmpiParent()->printMsgSizes();
#endif

#if CMK_BIGSIM_CHARM && CMK_TRACE_IN_CHARM
  if(CpvAccess(traceOn)) traceSuspend();
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

AMPI_API_IMPL(int, MPI_Send, const void *msg, int count, MPI_Datatype type,
                             int dest, int tag, MPI_Comm comm)
{
  AMPI_API("AMPI_Send");

  handle_MPI_BOTTOM((void*&)msg, type);

#if AMPI_ERROR_CHECKING
  int ret;
  ret = errorCheck("AMPI_Send", comm, 1, count, 1, type, 1, tag, 1, dest, 1, msg, 1);
  if(ret != MPI_SUCCESS)
    return ret;
#endif

#if AMPIMSGLOG
  if(msgLogRead){
    return MPI_SUCCESS;
  }
#endif

  ampi *ptr = getAmpiInstance(comm);
  ptr->send(tag, ptr->getRank(), msg, count, type, dest, comm);

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Bsend, const void *buf, int count, MPI_Datatype datatype,
                              int dest, int tag, MPI_Comm comm)
{
  AMPI_API("AMPI_Bsend");
  // FIXME: we don't actually use the buffer set in MPI_Buffer_attach
  //        for buffering of messages sent via MPI_Bsend
  return MPI_Send(buf, count, datatype, dest, tag, comm);
}

AMPI_API_IMPL(int, MPI_Buffer_attach, void *buffer, int size)
{
  AMPI_API("AMPI_Buffer_attach");
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
  AMPI_API("AMPI_Buffer_detach");
  getAmpiParent()->detachBuffer(buffer, size);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Rsend, const void *buf, int count, MPI_Datatype datatype,
                              int dest, int tag, MPI_Comm comm)
{
  /* FIXME: MPI_Rsend can be posted only after recv */
  AMPI_API("AMPI_Rsend");
  return MPI_Send(buf, count, datatype, dest, tag, comm);
}

AMPI_API_IMPL(int, MPI_Ssend, const void *msg, int count, MPI_Datatype type,
                              int dest, int tag, MPI_Comm comm)
{
  AMPI_API("AMPI_Ssend");

  handle_MPI_BOTTOM((void*&)msg, type);

#if AMPI_ERROR_CHECKING
  int ret = errorCheck("AMPI_Ssend", comm, 1, count, 1, type, 1, tag, 1, dest, 1, msg, 1);
  if(ret != MPI_SUCCESS)
    return ret;
#endif

#if AMPIMSGLOG
  if(msgLogRead){
    return MPI_SUCCESS;
  }
#endif

  ampi *ptr = getAmpiInstance(comm);
  ptr->send(tag, ptr->getRank(), msg, count, type, dest, comm, 1);

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Issend, const void *buf, int count, MPI_Datatype type, int dest,
                               int tag, MPI_Comm comm, MPI_Request *request)
{
  AMPI_API("AMPI_Issend");

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

  USER_CALL_DEBUG("AMPI_Issend("<<type<<","<<dest<<","<<tag<<","<<comm<<")");
  ampiParent* pptr = getAmpiParent();
  ampi *ptr = getAmpiInstance(comm);
  *request = ptr->postReq(pptr->reqPool.newReq<SsendReq>(type, comm, pptr->getDDT()));
  // 1:  blocking now  - used by MPI_Ssend
  // >=2:  the index of the requests - used by MPI_Issend
  ptr->send(tag, ptr->getRank(), buf, count, type, dest, comm, *request+2, I_SEND);

#if AMPIMSGLOG
  if(msgLogWrite && record_msglog(pptr->thisIndex)){
    PUParray(*(pptr->toPUPer), (char *)request, sizeof(MPI_Request));
  }
#endif

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Recv, void *msg, int count, MPI_Datatype type, int src, int tag,
                             MPI_Comm comm, MPI_Status *status)
{
  AMPI_API("AMPI_Recv");

  handle_MPI_BOTTOM(msg, type);

#if AMPI_ERROR_CHECKING
  int ret = errorCheck("AMPI_Recv", comm, 1, count, 1, type, 1, tag, 1, src, 1, msg, 1);
  if(ret != MPI_SUCCESS)
    return ret;
#endif

#if AMPIMSGLOG
  ampiParent* pptr = getAmpiParent();
  if(msgLogRead){
    (*(pptr->fromPUPer))|(pptr->pupBytes);
    PUParray(*(pptr->fromPUPer), (char *)msg, (pptr->pupBytes));
    PUParray(*(pptr->fromPUPer), (char *)status, sizeof(MPI_Status));
    return MPI_SUCCESS;
  }
#endif

  ampi *ptr = getAmpiInstance(comm);
  if(-1==ptr->recv(tag,src,msg,count,type,comm,status)) CkAbort("AMPI> Error in MPI_Recv");

#if AMPIMSGLOG
  if(msgLogWrite && record_msglog(pptr->thisIndex)){
    (pptr->pupBytes) = getDDT()->getSize(type) * count;
    (*(pptr->toPUPer))|(pptr->pupBytes);
    PUParray(*(pptr->toPUPer), (char *)msg, (pptr->pupBytes));
    PUParray(*(pptr->toPUPer), (char *)status, sizeof(MPI_Status));
  }
#endif

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Probe, int src, int tag, MPI_Comm comm, MPI_Status *status)
{
  AMPI_API("AMPI_Probe");

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
  AMPI_API("AMPI_Iprobe");

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
  AMPI_API("AMPI_Improbe");

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
  AMPI_API("AMPI_Imrecv");

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

  USER_CALL_DEBUG("AMPI_Imrecv("<<datatype<<","<<src<<","<<tag<<","<<comm<<")");
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
  AMPI_API("AMPI_Mprobe");

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
  AMPI_API("AMPI_Mrecv");

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
  if (status != MPI_STATUS_IGNORE) {
    status->MPI_SOURCE = msg->getSrcRank();
    status->MPI_TAG    = msg->getTag();
    status->MPI_COMM   = comm;
    status->MPI_LENGTH = msg->getLength();
    status->MPI_CANCEL = 0;
  }
  ptr->processAmpiMsg(msg, buf, datatype, count);
  CkpvAccess(msgPool).deleteAmpiMsg(msg);
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

  reqs[1] = send(stag, getRank(), sbuf, scount, stype, dest, comm, 0, I_SEND);

  if (sts == MPI_STATUS_IGNORE) {
    MPI_Waitall(2, reqs, MPI_STATUSES_IGNORE);
  }
  else {
    MPI_Status statuses[2];
    MPI_Waitall(2, reqs, statuses);
    *sts = statuses[0];
  }
}

AMPI_API_IMPL(int, MPI_Sendrecv, const void *sbuf, int scount, MPI_Datatype stype, int dest,
                                 int stag, void *rbuf, int rcount, MPI_Datatype rtype,
                                 int src, int rtag, MPI_Comm comm, MPI_Status *sts)
{
  AMPI_API("AMPI_Sendrecv");

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
  vector<char> tmpBuf(ddt->getSize(count));
  ddt->serialize((char*)buf, tmpBuf.data(), count, ddt->getSize(count), PACK);

  MPI_Request reqs[2];
  irecv(buf, count, datatype, source, recvtag, comm, &reqs[0]);

  // FIXME: this send may do a copy internally! If we knew now that it would, we could avoid double copying:
  reqs[1] = send(sendtag, getRank(), tmpBuf.data(), count, datatype, dest, comm, 0, I_SEND);

  if (status == MPI_STATUS_IGNORE) {
    MPI_Waitall(2, reqs, MPI_STATUSES_IGNORE);
  }
  else {
    MPI_Status statuses[2];
    MPI_Waitall(2, reqs, statuses);
    *status = statuses[0];
  }
}

AMPI_API_IMPL(int, MPI_Sendrecv_replace, void* buf, int count, MPI_Datatype datatype,
                                         int dest, int sendtag, int source, int recvtag,
                                         MPI_Comm comm, MPI_Status *status)
{
  AMPI_API("AMPI_Sendrecv_replace");

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

void ampi::barrier() noexcept
{
  CkAssert(parent->resumeOnColl == false);
  parent->resumeOnColl = true;
  CkCallback barrierCB(CkReductionTarget(ampi, barrierResult), getProxy());
  contribute(barrierCB);
  thread->suspend(); //Resumed by ampi::barrierResult
  getAmpiParent()->resumeOnColl = false;
}

void ampi::barrierResult() noexcept
{
  MSG_ORDER_DEBUG(CkPrintf("[%d] barrierResult called\n", thisIndex));
  CkAssert(parent->resumeOnColl == true);
  thread->resume();
}

AMPI_API_IMPL(int, MPI_Barrier, MPI_Comm comm)
{
  AMPI_API("AMPI_Barrier");

#if AMPI_ERROR_CHECKING
  int ret = checkCommunicator("AMPI_Barrier", comm);
  if(ret != MPI_SUCCESS)
    return ret;
#endif

#if CMK_BIGSIM_CHARM
  TRACE_BG_AMPI_LOG(MPI_BARRIER, 0);
#endif

  ampi *ptr = getAmpiInstance(comm);
  MSG_ORDER_DEBUG(CkPrintf("[%d] AMPI_Barrier called on comm %d\n", ptr->thisIndex, comm));

  if (ptr->getSize() == 1 && !getAmpiParent()->isInter(comm))
    return MPI_SUCCESS;

  // implementation of intercomm barrier is equivalent to that for intracomm barrier

  ptr->barrier();

  return MPI_SUCCESS;
}

void ampi::ibarrier(MPI_Request *request) noexcept
{
  *request = postReq(parent->reqPool.newReq<IReq>(nullptr, 0, MPI_INT, AMPI_COLL_SOURCE, MPI_ATA_TAG, myComm.getComm(), getDDT()));
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
  AMPI_API("AMPI_Ibarrier");

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

#if CMK_BIGSIM_CHARM
  TRACE_BG_AMPI_LOG(MPI_BARRIER, 0);
#endif

  MSG_ORDER_DEBUG(CkPrintf("[%d] AMPI_Ibarrier called on comm %d\n", ptr->thisIndex, comm));

  ptr->ibarrier(request);

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Bcast, void *buf, int count, MPI_Datatype type, int root, MPI_Comm comm)
{
  AMPI_API("AMPI_Bcast");

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
  AMPI_API("AMPI_Ibcast");

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
  MSG_ORDER_DEBUG(CkPrintf("[%d] rednResult called on comm %d\n", thisIndex, myComm.getComm()));

#if CMK_ERROR_CHECKING
  if (parent->blockingReq == NULL) {
    CkAbort("AMPI> recv'ed a blocking reduction unexpectedly!\n");
  }
#endif

#if CMK_BIGSIM_CHARM
  TRACE_BG_ADD_TAG("AMPI_generic");
  msg->event = NULL;
  _TRACE_BG_TLINE_END(&msg->event); // store current log
  msg->eventPe = CkMyPe();
#endif

  parent->blockingReq->receive(this, msg);

  CkAssert(parent->resumeOnColl);
  thread->resume();
  // [nokeep] entry method, so do not delete msg
}

// This routine is called with the results of an I(all)reduce or I(all)gather(v)
void ampi::irednResult(CkReductionMsg *msg) noexcept
{
  MSG_ORDER_DEBUG(CkPrintf("[%d] irednResult called on comm %d\n", thisIndex, myComm.getComm()));

  AmpiRequest* req = postedReqs.get(MPI_REDN_TAG, AMPI_COLL_SOURCE);
  if (req == NULL)
    CkAbort("AMPI> recv'ed a non-blocking reduction unexpectedly!\n");

#if CMK_BIGSIM_CHARM
  TRACE_BG_ADD_TAG("AMPI_generic");
  msg->event = NULL;
  _TRACE_BG_TLINE_END(&msg->event); // store current log
  msg->eventPe = CkMyPe();
#endif
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

    vector<char> sbuf;
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
    vector<char> serialized(slen);
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
  AMPI_API("AMPI_Reduce");

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
  ampiParent* pptr = getAmpiParent();
  if(msgLogRead){
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
    AmpiMsg *msg = new (0, 0) AmpiMsg(0, 0, MPI_REDN_TAG, -1, rootIdx, 0);
    CProxy_ampi pa(ptr->getProxy());
    pa.generic(msg);
#endif
  }
#if AMPI_SYNC_REDUCE
  ptr->recv(MPI_REDN_TAG, AMPI_COLL_SOURCE, NULL, 0, type, comm);
#endif

#if AMPIMSGLOG
  if(msgLogWrite && record_msglog(pptr->thisIndex)){
    (pptr->pupBytes) = getDDT()->getSize(type) * count;
    (*(pptr->toPUPer))|(pptr->pupBytes);
    PUParray(*(pptr->toPUPer), (char *)outbuf, (pptr->pupBytes));
  }
#endif

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Allreduce, const void *inbuf, void *outbuf, int count, MPI_Datatype type,
                                  MPI_Op op, MPI_Comm comm)
{
  AMPI_API("AMPI_Allreduce");

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

#if CMK_BIGSIM_CHARM
  TRACE_BG_AMPI_LOG(MPI_ALLREDUCE, getAmpiInstance(comm)->getDDT()->getType(type)->getSize(count));
#endif

#if AMPIMSGLOG
  ampiParent* pptr = getAmpiParent();
  if(msgLogRead){
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

  ptr->blockOnColl();

#if AMPIMSGLOG
  if(msgLogWrite && record_msglog(pptr->thisIndex)){
    (pptr->pupBytes) = getDDT()->getSize(type) * count;
    (*(pptr->toPUPer))|(pptr->pupBytes);
    PUParray(*(pptr->toPUPer), (char *)outbuf, (pptr->pupBytes));
  }
#endif

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Iallreduce, const void *inbuf, void *outbuf, int count, MPI_Datatype type,
                                   MPI_Op op, MPI_Comm comm, MPI_Request* request)
{
  AMPI_API("AMPI_Iallreduce");

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
  AMPI_API("AMPI_Reduce_local");

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
  AMPI_API("AMPI_Reduce_scatter_block");

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

  vector<char> tmpbuf(ptr->getDDT()->getType(datatype)->getSize(count)*size);

  MPI_Reduce(sendbuf, &tmpbuf[0], count*size, datatype, op, AMPI_COLL_SOURCE, comm);
  MPI_Scatter(&tmpbuf[0], count, datatype, recvbuf, count, datatype, AMPI_COLL_SOURCE, comm);

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Ireduce_scatter_block, const void* sendbuf, void* recvbuf, int count,
                                              MPI_Datatype datatype, MPI_Op op, MPI_Comm comm,
                                              MPI_Request* request)
{
  AMPI_API("AMPI_Ireduce_scatter_block");
  // FIXME: implement non-blocking reduce_scatter_block
  int ret = MPI_Reduce_scatter_block(sendbuf, recvbuf, count, datatype, op, comm);
  *request = MPI_REQUEST_NULL;
  return ret;
}

AMPI_API_IMPL(int, MPI_Reduce_scatter, const void* sendbuf, void* recvbuf, const int *recvcounts,
                                       MPI_Datatype datatype, MPI_Op op, MPI_Comm comm)
{
  AMPI_API("AMPI_Reduce_scatter");

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
  vector<int> displs(size);
  int len;

  //under construction
  for(int i=0;i<size;i++){
    displs[i] = count;
    count+= recvcounts[i];
  }
  vector<char> tmpbuf(ptr->getDDT()->getType(datatype)->getSize(count));
  MPI_Reduce(sendbuf, tmpbuf.data(), count, datatype, op, AMPI_COLL_SOURCE, comm);
  MPI_Scatterv(tmpbuf.data(), recvcounts, displs.data(), datatype,
                          recvbuf, recvcounts[ptr->getRank()], datatype, AMPI_COLL_SOURCE, comm);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Ireduce_scatter, const void* sendbuf, void* recvbuf, const int *recvcounts,
                                        MPI_Datatype datatype, MPI_Op op, MPI_Comm comm, MPI_Request* request)
{
  AMPI_API("AMPI_Ireduce_scatter");
  // FIXME: implement non-blocking reduce_scatter
  int ret = MPI_Reduce_scatter(sendbuf, recvbuf, recvcounts, datatype, op, comm);
  *request = MPI_REQUEST_NULL;
  return ret;
}

AMPI_API_IMPL(int, MPI_Scan, const void* sendbuf, void* recvbuf, int count, MPI_Datatype datatype,
                             MPI_Op op, MPI_Comm comm)
{
  AMPI_API("AMPI_Scan");

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
  vector<char> tmp_buf(blklen);
  vector<char> partial_scan(blklen);

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
  AMPI_API("AMPI_Iscan");
  // FIXME: implement non-blocking scan
  int ret = MPI_Scan(sendbuf, recvbuf, count, datatype, op, comm);
  *request = MPI_REQUEST_NULL;
  return ret;
}

AMPI_API_IMPL(int, MPI_Exscan, const void* sendbuf, void* recvbuf, int count, MPI_Datatype datatype,
                               MPI_Op op, MPI_Comm comm)
{
  AMPI_API("AMPI_Exscan");

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
  vector<char> tmp_buf(blklen);
  vector<char> partial_scan(blklen);

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
  AMPI_API("AMPI_Iexscan");
  // FIXME: implement non-blocking exscan
  int ret = MPI_Exscan(sendbuf, recvbuf, count, datatype, op, comm);
  *request = MPI_REQUEST_NULL;
  return ret;
}

AMPI_API_IMPL(int, MPI_Op_create, MPI_User_function *function, int commute, MPI_Op *op)
{
  AMPI_API("AMPI_Op_create");
  *op = getAmpiParent()->createOp(function, commute);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Op_free, MPI_Op *op)
{
  AMPI_API("AMPI_Op_free");
  getAmpiParent()->freeOp(*op);
  *op = MPI_OP_NULL;
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Op_commutative, MPI_Op op, int *commute)
{
  AMPI_API("AMPI_Op_commutative");
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

#if CMK_BIGSIM_CHARM
  return BgGetTime();
#else
  return TCHARM_Wall_timer();
#endif
}

AMPI_API_IMPL(double, MPI_Wtick, void)
{
  //AMPI_API("AMPI_Wtick");
  return 1e-6;
}

AMPI_API_IMPL(int, MPI_Start, MPI_Request *request)
{
  AMPI_API("AMPI_Start");
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
  AMPI_API("AMPI_Startall");
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
  setComplete(false);
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
  setComplete(false);
  ampi* ptr = getAmpiInstance(comm);
  ptr->send(tag, ptr->getRank(), buf, count, type, src /*really, the destination*/, comm);
  setComplete(true);
}

void SsendReq::start(MPI_Request reqIdx) noexcept {
  CkAssert(persistent);
  setComplete(false);
  ampi* ptr = getAmpiInstance(comm);
  ptr->send(tag, ptr->getRank(), buf, count, type, src /*really, the destination*/, comm, reqIdx+2, I_SEND);
}

int IReq::wait(MPI_Status *sts) noexcept {
  // ampi::generic() writes directly to the buffer, so the only thing we do here is wait
  ampiParent *parent = getAmpiParent();

  while (!isComplete()) {
    // parent is updated in case an ampi thread is migrated while waiting for a message
    parent->resumeOnRecv = true;
    parent->numBlockedReqs = 1;
    setBlocked(true);
    parent->block();
    setBlocked(false);
    parent = getAmpiParent();

    if (cancelled) {
      if (sts != MPI_STATUS_IGNORE) sts->MPI_CANCEL = 1;
      setComplete(true);
      parent->resumeOnRecv = false;
      return 0;
    }

#if CMK_BIGSIM_CHARM
    //Because of the out-of-core emulation, this pointer is changed after in-out
    //memory operation. So we need to return from this function and do the while loop
    //in the outer function call.
    if(_BgInOutOfCoreMode)
      return -1;
#endif
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

  return 0;
}

int RednReq::wait(MPI_Status *sts) noexcept {
  // ampi::irednResult() writes directly to the buffer, so the only thing we do here is wait
  ampiParent *parent = getAmpiParent();

  while (!isComplete()) {
    parent->resumeOnColl = true;
    parent->numBlockedReqs = 1;
    setBlocked(true);
    parent->block();
    setBlocked(false);
    parent = getAmpiParent();

#if CMK_BIGSIM_CHARM
    //Because of the out-of-core emulation, this pointer is changed after in-out
    //memory operation. So we need to return from this function and do the while loop
    //in the outer function call.
    if (_BgInOutOfCoreMode)
      return -1;
#endif
  }
  parent->resumeOnColl = false;

  AMPI_DEBUG("RednReq::wait has resumed\n");

  if (sts != MPI_STATUS_IGNORE) {
    sts->MPI_TAG = tag;
    sts->MPI_SOURCE = src;
    sts->MPI_COMM = comm;
    sts->MPI_CANCEL = 0;
  }
  return 0;
}

int GatherReq::wait(MPI_Status *sts) noexcept {
  // ampi::irednResult() writes directly to the buffer, so the only thing we do here is wait
  ampiParent *parent = getAmpiParent();

  while (!isComplete()) {
    parent->resumeOnColl = true;
    parent->numBlockedReqs = 1;
    setBlocked(true);
    parent->block();
    setBlocked(false);
    parent = getAmpiParent();

#if CMK_BIGSIM_CHARM
    //Because of the out-of-core emulation, this pointer is changed after in-out
    //memory operation. So we need to return from this function and do the while loop
    //in the outer function call.
    if (_BgInOutOfCoreMode)
      return -1;
#endif
  }
  parent->resumeOnColl = false;

  AMPI_DEBUG("GatherReq::wait has resumed\n");

  if (sts != MPI_STATUS_IGNORE) {
    sts->MPI_TAG = tag;
    sts->MPI_SOURCE = src;
    sts->MPI_COMM = comm;
    sts->MPI_CANCEL = 0;
  }
  return 0;
}

int GathervReq::wait(MPI_Status *sts) noexcept {
  // ampi::irednResult writes directly to the buffer, so the only thing we do here is wait
  ampiParent *parent = getAmpiParent();

  while (!isComplete()) {
    parent->resumeOnColl = true;
    parent->numBlockedReqs = 1;
    setBlocked(true);
    parent->block();
    setBlocked(false);
    parent = getAmpiParent();

#if CMK_BIGSIM_CHARM
    //Because of the out-of-core emulation, this pointer is changed after in-out
    //memory operation. So we need to return from this function and do the while loop
    //in the outer function call.
    if (_BgInOutOfCoreMode)
      return -1;
#endif
  }
  parent->resumeOnColl = false;

  AMPI_DEBUG("GathervReq::wait has resumed\n");

  if (sts != MPI_STATUS_IGNORE) {
    sts->MPI_TAG = tag;
    sts->MPI_SOURCE = src;
    sts->MPI_COMM = comm;
    sts->MPI_CANCEL = 0;
  }
  return 0;
}

int SendReq::wait(MPI_Status *sts) noexcept {
  ampiParent *parent = getAmpiParent();
  while (!isComplete()) {
    parent->resumeOnRecv = true;
    parent->numBlockedReqs = 1;
    setBlocked(true);
    parent->block();
    setBlocked(false);
    // "dis" is updated in case an ampi thread is migrated while waiting for a message
    parent = getAmpiParent();
  }
  parent->resumeOnRecv = false;
  AMPI_DEBUG("SendReq::wait has resumed\n");
  if (sts != MPI_STATUS_IGNORE) {
    sts->MPI_COMM = comm;
    sts->MPI_CANCEL = 0;
  }
  return 0;
}

int SsendReq::wait(MPI_Status *sts) noexcept {
  ampiParent *parent = getAmpiParent();
  while (!isComplete()) {
    // "dis" is updated in case an ampi thread is migrated while waiting for a message
    parent = parent->blockOnRecv();
  }
  if (sts != MPI_STATUS_IGNORE) {
    sts->MPI_COMM = comm;
    sts->MPI_CANCEL = 0;
  }
  return 0;
}

int ATAReq::wait(MPI_Status *sts) noexcept {
  MPI_Waitall(reqs.size(), reqs.data(), MPI_STATUSES_IGNORE);
  reqs.clear();
  setComplete(true);
  return 0;
}

int GReq::wait(MPI_Status *sts) noexcept {
  MPI_Status tmpStatus;
  if (pollFn)
    (*pollFn)(extraState, (sts == MPI_STATUS_IGNORE || sts == MPI_STATUSES_IGNORE) ? &tmpStatus : sts);
  (*queryFn)(extraState, (sts == MPI_STATUS_IGNORE || sts == MPI_STATUSES_IGNORE) ? &tmpStatus : sts);
  setComplete(true);
  return 0;
}

AMPI_API_IMPL(int, MPI_Wait, MPI_Request *request, MPI_Status *sts)
{
  AMPI_API("AMPI_Wait");

  if(*request == MPI_REQUEST_NULL){
    clearStatus(sts);
    return MPI_SUCCESS;
  }
  checkRequest(*request);
  ampiParent* pptr = getAmpiParent();
  AmpiRequestList& reqs = pptr->getReqs();

#if AMPIMSGLOG
  if(msgLogRead){
    (*(pptr->fromPUPer))|(pptr->pupBytes);
    PUParray(*(pptr->fromPUPer), (char *)(reqs[*request]->buf), (pptr->pupBytes));
    PUParray(*(pptr->fromPUPer), (char *)sts, sizeof(MPI_Status));
    return MPI_SUCCESS;
  }
#endif

#if CMK_BIGSIM_CHARM
  void *curLog; // store current log in timeline
  _TRACE_BG_TLINE_END(&curLog);
#endif

  AMPI_DEBUG("AMPI_Wait request=%d reqs[*request]=%p reqs[*request]->tag=%d\n",
             *request, reqs[*request], (int)(reqs[*request]->tag));
  AMPI_DEBUG("MPI_Wait: request=%d, reqs.size=%d, &reqs=%d\n",
             *request, reqs.size(), reqs);
  CkAssert(pptr->numBlockedReqs == 0);
  int waitResult = -1;
  do{
    AmpiRequest& waitReq = *reqs[*request];
    waitResult = waitReq.wait(sts);
#if CMK_BIGSIM_CHARM
    if(_BgInOutOfCoreMode){
      reqs = getReqs();
    }
#endif
  }while(waitResult==-1);

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

#if CMK_BIGSIM_CHARM
  TRACE_BG_AMPI_WAIT(&reqs); // setup forward and backward dependence
#endif

  reqs.freeNonPersReq(*request);

  AMPI_DEBUG("End of AMPI_Wait\n");

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Waitall, int count, MPI_Request request[], MPI_Status sts[])
{
  AMPI_API("AMPI_Waitall");

  checkRequests(count, request);
  if (count == 0) return MPI_SUCCESS;

  ampiParent* pptr = getAmpiParent();
  AmpiRequestList& reqs = pptr->getReqs();
  CkAssert(pptr->numBlockedReqs == 0);

#if AMPIMSGLOG
  if(msgLogRead){
    for(int i=0;i<count;i++){
      if(request[i] == MPI_REQUEST_NULL){
        clearStatus(sts, i);
        continue;
      }
      AmpiRequest *waitReq = reqs[request[i]];
      (*(pptr->fromPUPer))|(pptr->pupBytes);
      PUParray(*(pptr->fromPUPer), (char *)(waitReq->buf), pptr->pupBytes);
      PUParray(*(pptr->fromPUPer), (char *)(&sts[i]), sizeof(MPI_Status));
    }
    return MPI_SUCCESS;
  }
#endif
#if CMK_BIGSIM_CHARM
  void *curLog; // store current log in timeline
  _TRACE_BG_TLINE_END(&curLog);
#endif

  // First check for any incomplete requests
  for (int i=0; i<count; i++) {
    if (request[i] == MPI_REQUEST_NULL) {
      clearStatus(sts, i);
      continue;
    }
    AmpiRequest& req = *reqs[request[i]];
    if (req.test()) {
      req.wait((sts == MPI_STATUSES_IGNORE) ? MPI_STATUS_IGNORE : &sts[i]);
      req.setBlocked(false);
#if AMPIMSGLOG
      if(msgLogWrite && record_msglog(pptr->thisIndex)){
        (pptr->pupBytes) = getDDT()->getSize(req.type) * req.count;
        (*(pptr->toPUPer))|(pptr->pupBytes);
        PUParray(*(pptr->toPUPer), (char *)(req.buf), pptr->pupBytes);
        PUParray(*(pptr->toPUPer), (char *)(&sts[i]), sizeof(MPI_Status));
      }
#endif
      reqs.freeNonPersReq(request[i]);
    }
    else {
      req.setBlocked(true);
      pptr->numBlockedReqs++;
    }
  }

  // If any requests are incomplete, block until all have been completed
  if (pptr->numBlockedReqs > 0) {
    getAmpiParent()->blockOnRecv();
    pptr = getAmpiParent();
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
      req.wait((sts == MPI_STATUSES_IGNORE) ? MPI_STATUS_IGNORE : &sts[i]);
      req.setBlocked(false);
#if AMPIMSGLOG
      if(msgLogWrite && record_msglog(pptr->thisIndex)){
        (pptr->pupBytes) = getDDT()->getSize(req.type) * req.count;
        (*(pptr->toPUPer))|(pptr->pupBytes);
        PUParray(*(pptr->toPUPer), (char *)(req.buf), pptr->pupBytes);
        PUParray(*(pptr->toPUPer), (char *)(&sts[i]), sizeof(MPI_Status));
      }
#endif
      reqs.freeNonPersReq(request[i]);
    }
  }

  CkAssert(getAmpiParent()->numBlockedReqs == 0);

#if CMK_BIGSIM_CHARM
  TRACE_BG_AMPI_WAITALL(&reqs); // setup forward and backward dependence
#endif

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Waitany, int count, MPI_Request *request, int *idx, MPI_Status *sts)
{
  AMPI_API("AMPI_Waitany");

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
      req.wait(sts);
      reqs.unblockReqs(&request[0], i);
      reqs.freeNonPersReq(request[i]);
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
      req.wait(sts);
      reqs.unblockReqs(&request[i], count-i);
      reqs.freeNonPersReq(request[i]);
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
  AMPI_API("AMPI_Waitsome");

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
      req.wait(&sts);
      array_of_indices[(*outcount)] = i;
      (*outcount)++;
      if (array_of_statuses != MPI_STATUSES_IGNORE)
        array_of_statuses[(*outcount)] = sts;
      reqs.freeNonPersReq(array_of_requests[i]);
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
        req.wait(&sts);
        array_of_indices[(*outcount)] = i;
        (*outcount)++;
        if (array_of_statuses != MPI_STATUSES_IGNORE)
          array_of_statuses[(*outcount)] = sts;
        reqs.unblockReqs(&array_of_requests[i], incount-i);
        reqs.freeNonPersReq(array_of_requests[i]);
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
      setComplete(true);
    }
    else if (isComplete()) {
      sts->MPI_SOURCE = src;
      sts->MPI_TAG    = tag;
      sts->MPI_COMM   = comm;
      sts->MPI_LENGTH = length;
      sts->MPI_CANCEL = 0;
    }
  }
  else if (cancelled) {
    setComplete(true);
  }
  return isComplete();
}

bool RednReq::test(MPI_Status *sts/*=MPI_STATUS_IGNORE*/) noexcept {
  return isComplete();
}

bool GatherReq::test(MPI_Status *sts/*=MPI_STATUS_IGNORE*/) noexcept {
  return isComplete();
}

bool GathervReq::test(MPI_Status *sts/*=MPI_STATUS_IGNORE*/) noexcept {
  return isComplete();
}

bool SendReq::test(MPI_Status *sts/*=MPI_STATUS_IGNORE*/) noexcept {
  return isComplete();
}

bool SsendReq::test(MPI_Status *sts/*=MPI_STATUS_IGNORE*/) noexcept {
  return isComplete();
}

bool GReq::test(MPI_Status *sts/*=MPI_STATUS_IGNORE*/) noexcept {
  MPI_Status tmpStatus;
  if (pollFn)
    (*pollFn)(extraState, (sts == MPI_STATUS_IGNORE || sts == MPI_STATUSES_IGNORE) ? &tmpStatus : sts);
  (*queryFn)(extraState, (sts == MPI_STATUS_IGNORE || sts == MPI_STATUSES_IGNORE) ? &tmpStatus : sts);
  return isComplete();
}

bool ATAReq::test(MPI_Status *sts/*=MPI_STATUS_IGNORE*/) noexcept {
  AmpiRequestList& reqList = getReqs();
  int i = 0;
  while (i < reqs.size()) {
    if (reqs[i] == MPI_REQUEST_NULL) {
      std::swap(reqs[i], reqs.back());
      reqs.pop_back();
      continue;
    }
    AmpiRequest& req = *reqList[reqs[i]];
    if (req.test()) {
      req.wait(sts);
      reqList.freeNonPersReq(reqs[i]);
      std::swap(reqs[i], reqs.back());
      reqs.pop_back();
      continue;
    }
    i++;
  }
  setComplete(reqs.empty());
  return isComplete();
}

void IReq::receive(ampi *ptr, AmpiMsg *msg, bool deleteMsg/*=true*/) noexcept
{
  ptr->processAmpiMsg(msg, buf, type, count);
  setComplete(true);
  length = msg->getLength();
  tag = msg->getTag(); // Although not required, we also extract tag from msg
  src = msg->getSrcRank();   // Although not required, we also extract src from msg
  comm = ptr->getComm();
  AMPI_DEBUG("Setting this->tag to %d in IReq::receive this=%p\n", tag, this);
#if CMK_BIGSIM_CHARM
  event = msg->event;
  eventPe = msg->eventPe;
#endif
  // in case of an inorder bcast, msg is [nokeep] and shouldn't be freed
  if (deleteMsg) {
    CkpvAccess(msgPool).deleteAmpiMsg(msg);
  }
}

void IReq::receiveRdma(ampi *ptr, char *sbuf, int slength, int ssendReq, int srcRank, MPI_Comm scomm) noexcept
{
  ptr->processRdmaMsg(sbuf, slength, ssendReq, srcRank, buf, count, type, scomm);
  setComplete(true);
  length = slength;
  comm = scomm;
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
  setComplete(true);
  comm = ptr->getComm();
#if CMK_BIGSIM_CHARM
  event = msg->event;
  eventPe = msg->eventPe;
#endif
  // ampi::rednResult is a [nokeep] entry method, so do not delete msg
}

void GatherReq::receive(ampi *ptr, CkReductionMsg *msg) noexcept
{
  ptr->processGatherMsg(msg, buf, type, count);
  setComplete(true);
  comm = ptr->getComm();
#if CMK_BIGSIM_CHARM
  event = msg->event;
  eventPe = msg->eventPe;
#endif
  // ampi::rednResult is a [nokeep] entry method, so do not delete msg
}

void GathervReq::receive(ampi *ptr, CkReductionMsg *msg) noexcept
{
  ptr->processGathervMsg(msg, buf, type, recvCounts.data(), displs.data());
  setComplete(true);
  comm = ptr->getComm();
#if CMK_BIGSIM_CHARM
  event = msg->event;
  eventPe = msg->eventPe;
#endif
  // ampi::rednResult is a [nokeep] entry method, so do not delete msg
}

AMPI_API_IMPL(int, MPI_Request_get_status, MPI_Request request, int *flag, MPI_Status *sts)
{
  AMPI_API("AMPI_Request_get_status");
  testRequestNoFree(&request, flag, sts);
  if(*flag != 1)
    getAmpiParent()->yield();
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Test, MPI_Request *request, int *flag, MPI_Status *sts)
{
  AMPI_API("AMPI_Test");
  testRequest(request, flag, sts);
  if(*flag != 1)
    getAmpiParent()->yield();
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Testany, int count, MPI_Request *request, int *index, int *flag, MPI_Status *sts)
{
  AMPI_API("AMPI_Testany");

  checkRequests(count, request);

  if (count == 0) {
    *flag = 1;
    *index = MPI_UNDEFINED;
    clearStatus(sts);
    return MPI_SUCCESS;
  }

  int nullReqs = 0;
  *flag = 0;

  for (int i=0; i<count; i++) {
    if (request[i] == MPI_REQUEST_NULL) {
      nullReqs++;
      continue;
    }
    testRequest(&request[i], flag, sts);
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
    getAmpiParent()->yield();
  }

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Testall, int count, MPI_Request *request, int *flag, MPI_Status *sts)
{
  AMPI_API("AMPI_Testall");

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
      pptr->yield();
      return MPI_SUCCESS;
    }
  }

  if (nullReqs != count) {
    for (int i=0; i<count; i++) {
      int reqIdx = request[i];
      if (reqIdx != MPI_REQUEST_NULL) {
        AmpiRequest& req = *reqs[reqIdx];
        req.wait((sts == MPI_STATUSES_IGNORE) ? MPI_STATUS_IGNORE : &sts[i]);
        reqs.freeNonPersReq(request[i]);
      }
    }
  }

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Testsome, int incount, MPI_Request *array_of_requests, int *outcount,
                                 int *array_of_indices, MPI_Status *array_of_statuses)
{
  AMPI_API("AMPI_Testsome");

  checkRequests(incount, array_of_requests);
  if (incount == 0) {
    *outcount = MPI_UNDEFINED;
    return MPI_SUCCESS;
  }

  MPI_Status sts;
  int flag = 0, nullReqs = 0;
  *outcount = 0;

  for (int i=0; i<incount; i++) {
    if (array_of_requests[i] == MPI_REQUEST_NULL) {
      clearStatus(array_of_statuses, i);
      nullReqs++;
      continue;
    }
    testRequest(&array_of_requests[i], &flag, &sts);
    if (flag) {
      array_of_indices[(*outcount)] = i;
      (*outcount)++;
      if (array_of_statuses != MPI_STATUSES_IGNORE)
        array_of_statuses[(*outcount)] = sts;
    }
  }

  if (nullReqs == incount) {
    *outcount = MPI_UNDEFINED;
  }
  else if (*outcount == 0) {
    getAmpiParent()->yield();
  }

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Request_free, MPI_Request *request)
{
  AMPI_API("AMPI_Request_free");
  if(*request==MPI_REQUEST_NULL) return MPI_SUCCESS;
  checkRequest(*request);
  ampiParent* pptr = getAmpiParent();
  AmpiRequestList& reqs = pptr->getReqs();
  reqs.free(pptr->reqPool, *request, pptr->getDDT());
  *request = MPI_REQUEST_NULL;
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Grequest_start, MPI_Grequest_query_function *query_fn, MPI_Grequest_free_function *free_fn,
                                       MPI_Grequest_cancel_function *cancel_fn, void *extra_state, MPI_Request *request)
{
  AMPI_API("AMPI_Grequest_start");

  ampi* ptr = getAmpiInstance(MPI_COMM_SELF); // All GReq's are posted to MPI_COMM_SELF
  GReq *newreq = new GReq(query_fn, free_fn, cancel_fn, extra_state);
  *request = ptr->postReq(newreq);

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Grequest_complete, MPI_Request request)
{
  AMPI_API("AMPI_Grequest_complete");

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
  reqs[request]->setComplete(true);

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Cancel, MPI_Request *request)
{
  AMPI_API("AMPI_Cancel");
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
  AMPI_API("AMPI_Test_cancelled");
  // NOTE : current implementation requires AMPI_{Wait,Test}{any,some,all}
  // to be invoked before AMPI_Test_cancelled
  *flag = status->MPI_CANCEL;
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Status_set_cancelled, MPI_Status *status, int flag)
{
  AMPI_API("AMPI_Status_set_cancelled");
  status->MPI_CANCEL = flag;
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Recv_init, void *buf, int count, MPI_Datatype type, int src,
                                  int tag, MPI_Comm comm, MPI_Request *req)
{
  AMPI_API("AMPI_Recv_init");

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
  AMPI_API("AMPI_Send_init");

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
  AMPI_API("AMPI_Rsend_init");
  return MPI_Send_init(buf, count, type, dest, tag, comm, req);
}

AMPI_API_IMPL(int, MPI_Bsend_init, const void *buf, int count, MPI_Datatype type, int dest,
                                   int tag, MPI_Comm comm, MPI_Request *req)
{
  AMPI_API("AMPI_Bsend_init");
  return MPI_Send_init(buf, count, type, dest, tag, comm, req);
}

AMPI_API_IMPL(int, MPI_Ssend_init, const void *buf, int count, MPI_Datatype type, int dest,
                                   int tag, MPI_Comm comm, MPI_Request *req)
{
  AMPI_API("AMPI_Ssend_init");

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
  AMPI_API("AMPI_Type_contiguous");

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
  AMPI_API("AMPI_Type_vector");

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
  AMPI_API("AMPI_Type_create_hvector");

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
  AMPI_API("AMPI_Type_hvector");

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
  AMPI_API("AMPI_Type_indexed");

#if AMPI_ERROR_CHECKING
  int ret = checkData("AMPI_Type_indexed", oldtype);
  if (ret!=MPI_SUCCESS)
    return ret;
#endif

  /*CkDDT_Indexed's arrDisp has type MPI_Aint* (not int*). */
  vector<MPI_Aint> arrDispAint(count);
  for(int i=0; i<count; i++)
    arrDispAint[i] = (MPI_Aint)(arrDisp[i]);
  getDDT()->newIndexed(count, arrBlength, arrDispAint.data(), oldtype, newtype);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Type_create_hindexed, int count, const int* arrBlength, const MPI_Aint* arrDisp,
                                             MPI_Datatype oldtype, MPI_Datatype* newtype)
{
  AMPI_API("AMPI_Type_create_hindexed");

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
  AMPI_API("AMPI_Type_hindexed");

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
  AMPI_API("AMPI_Type_create_indexed_block");

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
  AMPI_API("AMPI_Type_create_hindexed_block");

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
  AMPI_API("AMPI_Type_create_struct");
  getDDT()->newStruct(count, arrBlength, arrDisp, oldtype, newtype);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Type_struct, int count, int* arrBlength, MPI_Aint* arrDisp,
                                    MPI_Datatype* oldtype, MPI_Datatype* newtype)
{
  AMPI_API("AMPI_Type_struct");
  return MPI_Type_create_struct(count, arrBlength, arrDisp, oldtype, newtype);
}

AMPI_API_IMPL(int, MPI_Type_commit, MPI_Datatype *datatype)
{
  AMPI_API("AMPI_Type_commit");

#if AMPI_ERROR_CHECKING
  int ret = checkData("MPI_Type_commit", *datatype);
  if (ret!=MPI_SUCCESS)
    return ret;
#endif

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Type_free, MPI_Datatype *datatype)
{
  AMPI_API("AMPI_Type_free");

#if AMPI_ERROR_CHECKING
  int ret = checkData("AMPI_Type_free", *datatype);
  if (ret!=MPI_SUCCESS)
    return ret;

  if (datatype == nullptr) {
    return ampiErrhandler("AMPI_Type_free", MPI_ERR_ARG);
  } else if (*datatype <= AMPI_MAX_PREDEFINED_TYPE) {
    return ampiErrhandler("AMPI_Type_free", MPI_ERR_TYPE);
  }
#endif
  getDDT()->freeType(*datatype);
  *datatype = MPI_DATATYPE_NULL;
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Type_get_extent, MPI_Datatype datatype, MPI_Aint *lb, MPI_Aint *extent)
{
  AMPI_API("AMPI_Type_get_extent");

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
  AMPI_API("AMPI_Type_get_extent_x");

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
  AMPI_API("AMPI_Type_extent");

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
  AMPI_API("AMPI_Type_get_true_extent");

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
  AMPI_API("AMPI_Type_get_true_extent_x");

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
  AMPI_API("AMPI_Type_size");

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
  AMPI_API("AMPI_Type_size_x");

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
  AMPI_API("AMPI_Type_set_name");

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
  AMPI_API("AMPI_Type_get_name");

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
  AMPI_API("AMPI_Type_create_resized");

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
  AMPI_API("AMPI_Type_dup");

#if AMPI_ERROR_CHECKING
  int ret = checkData("AMPI_Type_dup", oldtype);
  if (ret!=MPI_SUCCESS)
    return ret;
#endif

  getDDT()->createDup(oldtype, newtype);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Type_set_attr, MPI_Datatype datatype, int keyval, void *attribute_val)
{
  AMPI_API("AMPI_Type_set_attr");

#if AMPI_ERROR_CHECKING
  int ret = checkData("AMPI_Type_set_attr", datatype);
  if (ret!=MPI_SUCCESS)
    return ret;
#endif

  ampiParent *parent = getAmpiParent();
  vector<int>& keyvals = parent->getDDT()->getType(datatype)->getKeyvals();
  int err = parent->setAttr(datatype, keyvals, keyval, attribute_val);
  return ampiErrhandler("AMPI_Type_set_attr", err);
}

AMPI_API_IMPL(int, MPI_Type_get_attr, MPI_Datatype datatype, int keyval,
                                      void *attribute_val, int *flag)
{
  AMPI_API("AMPI_Type_get_attr");

#if AMPI_ERROR_CHECKING
  int ret = checkData("AMPI_Type_get_attr", datatype);
  if (ret!=MPI_SUCCESS)
    return ret;
#endif

  ampiParent *parent = getAmpiParent();
  vector<int>& keyvals = parent->getDDT()->getType(datatype)->getKeyvals();
  int err = parent->getAttr(datatype, keyvals, keyval, attribute_val, flag);
  return ampiErrhandler("AMPI_Type_get_attr", err);
}

AMPI_API_IMPL(int, MPI_Type_delete_attr, MPI_Datatype datatype, int keyval)
{
  AMPI_API("AMPI_Type_delete_attr");

#if AMPI_ERROR_CHECKING
  int ret = checkData("AMPI_Type_delete_attr", datatype);
  if (ret!=MPI_SUCCESS)
    return ret;
#endif

  ampiParent *parent = getAmpiParent();
  vector<int>& keyvals = parent->getDDT()->getType(datatype)->getKeyvals();
  int err = parent->deleteAttr(datatype, keyvals, keyval);
  return ampiErrhandler("AMPI_Type_delete_attr", err);
}

AMPI_API_IMPL(int, MPI_Type_create_keyval, MPI_Type_copy_attr_function *copy_fn,
                                           MPI_Type_delete_attr_function *delete_fn,
                                           int *keyval, void *extra_state)
{
  AMPI_API("AMPI_Type_create_keyval");
  return MPI_Comm_create_keyval(copy_fn, delete_fn, keyval, extra_state);
}

AMPI_API_IMPL(int, MPI_Type_free_keyval, int *keyval)
{
  AMPI_API("AMPI_Type_free_keyval");
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
  AMPI_API("AMPI_Type_create_darray");
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
  AMPI_API("AMPI_Type_create_subarray");
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
  AMPI_API("AMPI_Isend");

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

  USER_CALL_DEBUG("AMPI_Isend("<<type<<","<<dest<<","<<tag<<","<<comm<<")");

  ampi *ptr = getAmpiInstance(comm);
  *request = ptr->send(tag, ptr->getRank(), buf, count, type, dest, comm, 0, I_SEND);

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
  AMPI_API("AMPI_Ibsend");
  return MPI_Isend(buf, count, type, dest, tag, comm, request);
}

AMPI_API_IMPL(int, MPI_Irsend, const void *buf, int count, MPI_Datatype type, int dest,
                               int tag, MPI_Comm comm, MPI_Request *request)
{
  AMPI_API("AMPI_Irsend");
  return MPI_Isend(buf, count, type, dest, tag, comm, request);
}

void ampi::irecvBcast(void *buf, int count, MPI_Datatype type, int src,
                      MPI_Comm comm, MPI_Request *request) noexcept
{
  if (isInter()) {
    src = myComm.getIndexForRemoteRank(src);
  }
  AmpiRequestList& reqs = getReqs();
  IReq *newreq = parent->reqPool.newReq<IReq>(buf, count, type, src, MPI_BCAST_TAG, comm, getDDT());
  *request = reqs.insert(newreq);

  AmpiMsg* msg = unexpectedBcastMsgs.get(MPI_BCAST_TAG, src);
  // if msg has already arrived, do the receive right away
  if (msg) {
    newreq->receive(this, msg);
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
    src = myComm.getIndexForRemoteRank(src);
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
  // if msg has already arrived, do the receive right away
  if (msg) {
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
  AMPI_API("AMPI_Irecv");

  handle_MPI_BOTTOM(buf, type);

#if AMPI_ERROR_CHECKING
  int ret = errorCheck("AMPI_Irecv", comm, 1, count, 1, type, 1, tag, 1, src, 1, buf, 1);
  if(ret != MPI_SUCCESS){
    *request = MPI_REQUEST_NULL;
    return ret;
  }
#endif

  USER_CALL_DEBUG("AMPI_Irecv("<<type<<","<<src<<","<<tag<<","<<comm<<")");
  ampi *ptr = getAmpiInstance(comm);

  ptr->irecv(buf, count, type, src, tag, comm, request);

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Ireduce, const void *sendbuf, void *recvbuf, int count,
                                MPI_Datatype type, MPI_Op op, int root,
                                MPI_Comm comm, MPI_Request *request)
{
  AMPI_API("AMPI_Ireduce");

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

  vector<char> sbuf;
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

  vector<char> sbuf;
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
  AMPI_API("AMPI_Allgather");

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

  ptr->blockOnColl();

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Iallgather, const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                                   void *recvbuf, int recvcount, MPI_Datatype recvtype,
                                   MPI_Comm comm, MPI_Request* request)
{
  AMPI_API("AMPI_Iallgather");

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
  AMPI_API("AMPI_Allgatherv");

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

  ptr->blockOnColl();

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Iallgatherv, const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                                    void *recvbuf, const int *recvcounts, const int *displs,
                                    MPI_Datatype recvtype, MPI_Comm comm, MPI_Request *request)
{
  AMPI_API("AMPI_Iallgatherv");

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
  AMPI_API("AMPI_Gather");

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
  ampiParent* pptr = getAmpiParent();
  if(msgLogRead){
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
    ptr->blockOnColl();
  }

#if AMPIMSGLOG
  if(msgLogWrite && record_msglog(pptr->thisIndex)){
    (pptr->pupBytes) = getDDT()->getSize(recvtype) * recvcount * size;
    (*(pptr->toPUPer))|(pptr->pupBytes);
    PUParray(*(pptr->toPUPer), (char *)recvbuf, (pptr->pupBytes));
  }
#endif

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Igather, const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                                void *recvbuf, int recvcount, MPI_Datatype recvtype,
                                int root, MPI_Comm comm, MPI_Request *request)
{
  AMPI_API("AMPI_Igather");

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
  AMPI_API("AMPI_Gatherv");

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
    ptr->setBlockingReq(new GathervReq(recvbuf, size, recvtype, comm, recvcounts, displs, getDDT()));
  }

  int rootIdx = ptr->comm2CommStruct(comm).getIndexForRank(root);
  CkReductionMsg* msg = makeGathervMsg(sendbuf, sendcount, sendtype, rank, size);
  CkCallback gathervCB(CkIndex_ampi::rednResult(0), CkArrayIndex1D(rootIdx), ptr->getProxy());
  msg->setCallback(gathervCB);
  MSG_ORDER_DEBUG(CkPrintf("[%d] AMPI_Gatherv called on comm %d root %d \n", ptr->thisIndex, comm, rootIdx));
  ptr->contribute(msg);

  if (rank == root) {
    ptr->blockOnColl();
  }

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

AMPI_API_IMPL(int, MPI_Igatherv, const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                                 void *recvbuf, const int *recvcounts, const int *displs,
                                 MPI_Datatype recvtype, int root, MPI_Comm comm, MPI_Request *request)
{
  AMPI_API("AMPI_Igatherv");

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
  AMPI_API("AMPI_Scatter");

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
  AMPI_API("AMPI_Iscatter");

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
                                    sendcount, sendtype, i, comm, 0, I_SEND);
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
  AMPI_API("AMPI_Scatterv");

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
  AMPI_API("AMPI_Iscatterv");

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
                                    sendcounts[i], sendtype, i, comm, 0, I_SEND);
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
  AMPI_API("AMPI_Alltoall");

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

  ampi *ptr = getAmpiInstance(comm);

  if(getAmpiParent()->isInter(comm))
    CkAbort("AMPI does not implement MPI_Alltoall for Inter-communicators!");
  if(ptr->getSize() == 1)
    return copyDatatype(sendtype,sendcount,recvtype,recvcount,sendbuf,recvbuf);

  int itemsize = getDDT()->getSize(sendtype) * sendcount;
  int itemextent = getDDT()->getExtent(sendtype) * sendcount;
  int extent = getDDT()->getExtent(recvtype) * recvcount;
  int size = ptr->getSize();
  int rank = ptr->getRank();

#if CMK_BIGSIM_CHARM
  TRACE_BG_AMPI_LOG(MPI_ALLTOALL, itemextent);
#endif

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
    vector<MPI_Request> reqs(size*2);
    for (int i=0; i<size; i++) {
      int src = (rank+i) % size;
      ptr->irecv(((char*)recvbuf)+(extent*src), recvcount, recvtype,
                 src, MPI_ATA_TAG, comm, &reqs[i]);
    }
    for (int i=0; i<size; i++) {
      int dst = (rank+i) % size;
      reqs[size+i] = ptr->send(MPI_ATA_TAG, rank, ((char*)sendbuf)+(itemextent*dst),
                               sendcount, sendtype, dst, comm, 0, I_SEND);
    }
    MPI_Waitall(reqs.size(), reqs.data(), MPI_STATUSES_IGNORE);
  }
  else if (itemsize <= AMPI_ALLTOALL_LONG_MSG) {
    /* Don't post all sends and recvs at once. Instead do N sends/recvs at a time. */
    vector<MPI_Request> reqs(AMPI_ALLTOALL_THROTTLE*2);
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
      MPI_Waitall(blockSize*2, reqs.data(), MPI_STATUSES_IGNORE);
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
  AMPI_API("AMPI_Ialltoall");

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
                                     sendtype, dst, comm, 0, I_SEND);
  }
  *request = ptr->postReq(newreq);

  AMPI_DEBUG("MPI_Ialltoall: request=%d, reqs.size=%d, &reqs=%d\n",*request,reqs->size(),reqs);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Alltoallv, const void *sendbuf, const int *sendcounts, const int *sdispls,
                                  MPI_Datatype sendtype, void *recvbuf, const int *recvcounts,
                                  const int *rdispls, MPI_Datatype recvtype, MPI_Comm comm)
{
  AMPI_API("AMPI_Alltoallv");

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

  ampi *ptr = getAmpiInstance(comm);
  int size = ptr->getSize();

  if(getAmpiParent()->isInter(comm))
    CkAbort("AMPI does not implement MPI_Alltoallv for Inter-communicators!");
  if(size == 1)
    return copyDatatype(sendtype,sendcounts[0],recvtype,recvcounts[0],sendbuf,recvbuf);

  int rank = ptr->getRank();
  int itemextent = getDDT()->getExtent(sendtype);
  int extent = getDDT()->getExtent(recvtype);

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
    vector<MPI_Request> reqs(size*2);
    for (int i=0; i<size; i++) {
      int src = (rank+i) % size;
      ptr->irecv(((char*)recvbuf)+(extent*rdispls[src]), recvcounts[src], recvtype,
                 src, MPI_ATA_TAG, comm, &reqs[i]);
    }
    for (int i=0; i<size; i++) {
      int dst = (rank+i) % size;
      reqs[size+i] = ptr->send(MPI_ATA_TAG, rank, ((char*)sendbuf)+(itemextent*sdispls[dst]),
                               sendcounts[dst], sendtype, dst, comm, 0, I_SEND);
    }
    MPI_Waitall(size*2, reqs.data(), MPI_STATUSES_IGNORE);
  }
  else {
    /* Don't post all sends and recvs at once. Instead do N sends/recvs at a time. */
    vector<MPI_Request> reqs(AMPI_ALLTOALL_THROTTLE*2);
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
      MPI_Waitall(blockSize*2, reqs.data(), MPI_STATUSES_IGNORE);
    }
  }

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Ialltoallv, void *sendbuf, int *sendcounts, int *sdispls, MPI_Datatype sendtype,
                                   void *recvbuf, int *recvcounts, int *rdispls, MPI_Datatype recvtype,
                                   MPI_Comm comm, MPI_Request *request)
{
  AMPI_API("AMPI_Ialltoallv");

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
                                     sendcounts[dst], sendtype, dst, comm, 0, I_SEND);
  }
  *request = ptr->postReq(newreq);

  AMPI_DEBUG("MPI_Ialltoallv: request=%d, reqs.size=%d, &reqs=%d\n",*request,reqs->size(),reqs);

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Alltoallw, const void *sendbuf, const int *sendcounts, const int *sdispls,
                                  const MPI_Datatype *sendtypes, void *recvbuf, const int *recvcounts,
                                  const int *rdispls, const MPI_Datatype *recvtypes, MPI_Comm comm)
{
  AMPI_API("AMPI_Alltoallw");

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

  ampi *ptr = getAmpiInstance(comm);
  int size = ptr->getSize();
  int rank = ptr->getRank();

  if(getAmpiParent()->isInter(comm))
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
    vector<MPI_Request> reqs(size*2);
    for (int i=0; i<size; i++) {
      int src = (rank+i) % size;
      ptr->irecv(((char*)recvbuf)+rdispls[src], recvcounts[src], recvtypes[src],
                 src, MPI_ATA_TAG, comm, &reqs[i]);
    }
    for (int i=0; i<size; i++) {
      int dst = (rank+i) % size;
      reqs[size+i] = ptr->send(MPI_ATA_TAG, rank, ((char*)sendbuf)+sdispls[dst],
                               sendcounts[dst], sendtypes[dst], dst, comm, 0, I_SEND);
    }
    MPI_Waitall(size*2, reqs.data(), MPI_STATUSES_IGNORE);
  }
  else {
    /* Don't post all sends and recvs at once. Instead do N sends/recvs at a time. */
    vector<MPI_Request> reqs(AMPI_ALLTOALL_THROTTLE*2);
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
      MPI_Waitall(blockSize*2, reqs.data(), MPI_STATUSES_IGNORE);
    }
  }

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Ialltoallw, const void *sendbuf, const int *sendcounts, const int *sdispls,
                                   const MPI_Datatype *sendtypes, void *recvbuf, const int *recvcounts,
                                   const int *rdispls, const MPI_Datatype *recvtypes, MPI_Comm comm,
                                   MPI_Request *request)
{
  AMPI_API("AMPI_Ialltoallw");

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
                                sendcounts[dst], sendtypes[dst], dst, comm, 0, I_SEND);
  }
  *request = ptr->postReq(newreq);

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Neighbor_alltoall, const void* sendbuf, int sendcount, MPI_Datatype sendtype,
                                          void* recvbuf, int recvcount, MPI_Datatype recvtype,
                                          MPI_Comm comm)
{
  AMPI_API("AMPI_Neighbor_alltoall");

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

  ampi *ptr = getAmpiInstance(comm);
  int rank_in_comm = ptr->getRank();

  if (ptr->getSize() == 1)
    return copyDatatype(sendtype, sendcount, recvtype, recvcount, sendbuf, recvbuf);

  const vector<int>& neighbors = ptr->getNeighbors();
  int num_neighbors = neighbors.size();
  int itemsize = getDDT()->getSize(sendtype) * sendcount;
  int extent = getDDT()->getExtent(recvtype) * recvcount;

  vector<MPI_Request> reqs(num_neighbors*2);
  for (int j=0; j<num_neighbors; j++) {
    ptr->irecv(((char*)recvbuf)+(extent*j), recvcount, recvtype,
               neighbors[j], MPI_NBOR_TAG, comm, &reqs[j]);
  }

  for (int i=0; i<num_neighbors; i++) {
    reqs[num_neighbors+i] = ptr->send(MPI_NBOR_TAG, rank_in_comm, (void*)((char*)sendbuf+(itemsize*i)),
                                      sendcount, sendtype, neighbors[i], comm, 0, I_SEND);
  }

  MPI_Waitall(reqs.size(), reqs.data(), MPI_STATUSES_IGNORE);

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Ineighbor_alltoall, const void* sendbuf, int sendcount, MPI_Datatype sendtype,
                                           void* recvbuf, int recvcount, MPI_Datatype recvtype,
                                           MPI_Comm comm, MPI_Request *request)
{
  AMPI_API("AMPI_Ineighbor_alltoall");

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

  const vector<int>& neighbors = ptr->getNeighbors();
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
                                              sendcount, sendtype, neighbors[i], comm, 0, I_SEND);
  }
  *request = ptr->postReq(newreq);

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Neighbor_alltoallv, const void* sendbuf, const int *sendcounts, const int *sdispls,
                                           MPI_Datatype sendtype, void* recvbuf, const int *recvcounts,
                                           const int *rdispls, MPI_Datatype recvtype, MPI_Comm comm)
{
  AMPI_API("AMPI_Neighbor_alltoallv");

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

  ampi *ptr = getAmpiInstance(comm);
  int rank_in_comm = ptr->getRank();

  if (ptr->getSize() == 1)
    return copyDatatype(sendtype, sendcounts[0], recvtype, recvcounts[0], sendbuf, recvbuf);

  const vector<int>& neighbors = ptr->getNeighbors();
  int num_neighbors = neighbors.size();
  int itemsize = getDDT()->getSize(sendtype);
  int extent = getDDT()->getExtent(recvtype);

  vector<MPI_Request> reqs(num_neighbors*2);
  for (int j=0; j<num_neighbors; j++) {
    ptr->irecv(((char*)recvbuf)+(extent*rdispls[j]), recvcounts[j], recvtype,
               neighbors[j], MPI_NBOR_TAG, comm, &reqs[j]);
  }

  for (int i=0; i<num_neighbors; i++) {
    reqs[num_neighbors+i] = ptr->send(MPI_NBOR_TAG, rank_in_comm, (void*)((char*)sendbuf+(itemsize*sdispls[i])),
                                      sendcounts[i], sendtype, neighbors[i], comm, 0, I_SEND);
  }

  MPI_Waitall(reqs.size(), reqs.data(), MPI_STATUSES_IGNORE);

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Ineighbor_alltoallv, const void* sendbuf, const int *sendcounts, const int *sdispls,
                                            MPI_Datatype sendtype, void* recvbuf, const int *recvcounts,
                                            const int *rdispls, MPI_Datatype recvtype, MPI_Comm comm,
                                            MPI_Request *request)
{
  AMPI_API("AMPI_Ineighbor_alltoallv");

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

  const vector<int>& neighbors = ptr->getNeighbors();
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
                                              sendcounts[i], sendtype, neighbors[i], comm, 0, I_SEND);
  }
  *request = ptr->postReq(newreq);

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Neighbor_alltoallw, const void* sendbuf, const int *sendcounts, const MPI_Aint *sdispls,
                                           const MPI_Datatype *sendtypes, void* recvbuf, const int *recvcounts,
                                           const MPI_Aint *rdispls, const MPI_Datatype *recvtypes, MPI_Comm comm)
{
  AMPI_API("AMPI_Neighbor_alltoallw");

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

  ampi *ptr = getAmpiInstance(comm);
  int rank_in_comm = ptr->getRank();

  if (ptr->getSize() == 1)
    return copyDatatype(sendtypes[0], sendcounts[0], recvtypes[0], recvcounts[0], sendbuf, recvbuf);

  const vector<int>& neighbors = ptr->getNeighbors();
  int num_neighbors = neighbors.size();

  vector<MPI_Request> reqs(num_neighbors*2);
  for (int j=0; j<num_neighbors; j++) {
    ptr->irecv(((char*)recvbuf)+rdispls[j], recvcounts[j], recvtypes[j],
               neighbors[j], MPI_NBOR_TAG, comm, &reqs[j]);
  }

  for (int i=0; i<num_neighbors; i++) {
    reqs[num_neighbors+i] = ptr->send(MPI_NBOR_TAG, rank_in_comm, (void*)((char*)sendbuf+sdispls[i]),
                                      sendcounts[i], sendtypes[i], neighbors[i], comm, 0, I_SEND);
  }

  MPI_Waitall(reqs.size(), reqs.data(), MPI_STATUSES_IGNORE);

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Ineighbor_alltoallw, const void* sendbuf, const int *sendcounts, const MPI_Aint *sdispls,
                                            const MPI_Datatype *sendtypes, void* recvbuf, const int *recvcounts,
                                            const MPI_Aint *rdispls, const MPI_Datatype *recvtypes, MPI_Comm comm,
                                            MPI_Request *request)
{
  AMPI_API("AMPI_Ineighbor_alltoallw");

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

  const vector<int>& neighbors = ptr->getNeighbors();
  int num_neighbors = neighbors.size();

  // use an ATAReq to non-block the caller and get a request ptr
  ATAReq *newreq = new ATAReq(num_neighbors*2);
  for (int j=0; j<num_neighbors; j++) {
    ptr->irecv((char*)recvbuf+rdispls[j], recvcounts[j], recvtypes[j],
               neighbors[j], MPI_NBOR_TAG, comm, &newreq->reqs[j]);
  }

  for (int i=0; i<num_neighbors; i++) {
    newreq->reqs[num_neighbors+i] = ptr->send(MPI_NBOR_TAG, rank_in_comm, (void*)((char*)sendbuf+sdispls[i]),
                                              sendcounts[i], sendtypes[i], neighbors[i], comm, 0, I_SEND);
  }
  *request = ptr->postReq(newreq);

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Neighbor_allgather, const void* sendbuf, int sendcount, MPI_Datatype sendtype,
                                           void* recvbuf, int recvcount, MPI_Datatype recvtype,
                                           MPI_Comm comm)
{
  AMPI_API("AMPI_Neighbor_allgather");

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

  ampi *ptr = getAmpiInstance(comm);
  int rank_in_comm = ptr->getRank();

  if (ptr->getSize() == 1)
    return copyDatatype(sendtype, sendcount, recvtype, recvcount, sendbuf, recvbuf);

  const vector<int>& neighbors = ptr->getNeighbors();
  int num_neighbors = neighbors.size();

  int extent = getDDT()->getExtent(recvtype) * recvcount;
  vector<MPI_Request> reqs(num_neighbors*2);
  for (int j=0; j<num_neighbors; j++) {
    ptr->irecv(((char*)recvbuf)+(extent*j), recvcount, recvtype,
               neighbors[j], MPI_NBOR_TAG, comm, &reqs[j]);
  }

  for (int i=0; i<num_neighbors; i++) {
    reqs[num_neighbors+i] = ptr->send(MPI_NBOR_TAG, rank_in_comm, sendbuf, sendcount,
                                      sendtype, neighbors[i], comm, 0, I_SEND);
  }

  MPI_Waitall(reqs.size(), reqs.data(), MPI_STATUSES_IGNORE);

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Ineighbor_allgather, const void* sendbuf, int sendcount, MPI_Datatype sendtype,
                                            void* recvbuf, int recvcount, MPI_Datatype recvtype,
                                            MPI_Comm comm, MPI_Request *request)
{
  AMPI_API("AMPI_Ineighbor_allgather");

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

  const vector<int>& neighbors = ptr->getNeighbors();
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
                                              sendtype, neighbors[i], comm, 0, I_SEND);
  }
  *request = ptr->postReq(newreq);

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Neighbor_allgatherv, const void* sendbuf, int sendcount, MPI_Datatype sendtype,
                                            void* recvbuf, const int *recvcounts, const int *displs,
                                            MPI_Datatype recvtype, MPI_Comm comm)
{
  AMPI_API("AMPI_Neighbor_allgatherv");

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

  ampi *ptr = getAmpiInstance(comm);
  int rank_in_comm = ptr->getRank();

  if (ptr->getSize() == 1)
    return copyDatatype(sendtype, sendcount, recvtype, recvcounts[0], sendbuf, recvbuf);

  const vector<int>& neighbors = ptr->getNeighbors();
  int num_neighbors = neighbors.size();
  int extent = getDDT()->getExtent(recvtype);
  vector<MPI_Request> reqs(num_neighbors*2);
  for (int j=0; j<num_neighbors; j++) {
    ptr->irecv(((char*)recvbuf)+(extent*displs[j]), recvcounts[j], recvtype,
               neighbors[j], MPI_NBOR_TAG, comm, &reqs[j]);
  }
  for (int i=0; i<num_neighbors; i++) {
    reqs[num_neighbors+i] = ptr->send(MPI_NBOR_TAG, rank_in_comm, sendbuf, sendcount,
                                      sendtype, neighbors[i], comm, 0, I_SEND);
  }

  MPI_Waitall(reqs.size(), reqs.data(), MPI_STATUSES_IGNORE);

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Ineighbor_allgatherv, const void* sendbuf, int sendcount, MPI_Datatype sendtype,
                                             void* recvbuf, const int* recvcounts, const int* displs,
                                             MPI_Datatype recvtype, MPI_Comm comm, MPI_Request *request)
{
  AMPI_API("AMPI_Ineighbor_allgatherv");

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

  const vector<int>& neighbors = ptr->getNeighbors();
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
                                              sendtype, neighbors[i], comm, 0, I_SEND);
  }
  *request = ptr->postReq(newreq);

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Comm_dup, MPI_Comm comm, MPI_Comm *newcomm)
{
  AMPI_API("AMPI_Comm_dup");
  ampi *ptr = getAmpiInstance(comm);
  int topoType, rank = ptr->getRank();
  MPI_Topo_test(comm, &topoType);
  ptr->topoDup(topoType, rank, comm, newcomm);
  int ret = getAmpiParent()->dupUserKeyvals(comm, *newcomm);
  ptr->barrier();

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
  AMPI_API("AMPI_Comm_idup");
  // FIXME: implement non-blocking comm_dup
  *request = MPI_REQUEST_NULL;
  return MPI_Comm_dup(comm, newcomm);
}

AMPI_API_IMPL(int, MPI_Comm_dup_with_info, MPI_Comm comm, MPI_Info info, MPI_Comm *dest)
{
  AMPI_API("AMPI_Comm_dup_with_info");
  MPI_Comm_dup(comm, dest);
  MPI_Comm_set_info(*dest, info);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Comm_idup_with_info, MPI_Comm comm, MPI_Info info, MPI_Comm *dest, MPI_Request *request)
{
  AMPI_API("AMPI_Comm_idup_with_info");
  // FIXME: implement non-blocking comm_dup_with_info
  *request = MPI_REQUEST_NULL;
  return MPI_Comm_dup_with_info(comm, info, dest);
}

AMPI_API_IMPL(int, MPI_Comm_split, MPI_Comm src, int color, int key, MPI_Comm *dest)
{
  AMPI_API("AMPI_Comm_split");
  {
    ampi *ptr = getAmpiInstance(src);
    if (getAmpiParent()->isInter(src)) {
      ptr->split(color, key, dest, MPI_INTER);
    }
    else if (getAmpiParent()->isCart(src)) {
      ptr->split(color, key, dest, MPI_CART);
    }
    else if (getAmpiParent()->isGraph(src)) {
      ptr->split(color, key, dest, MPI_GRAPH);
    }
    else if (getAmpiParent()->isDistGraph(src)) {
      ptr->split(color, key, dest, MPI_DIST_GRAPH);
    }
    else {
      ptr->split(color, key, dest, MPI_UNDEFINED);
    }
  }
  if (color == MPI_UNDEFINED) *dest = MPI_COMM_NULL;

#if AMPIMSGLOG
  ampiParent* pptr = getAmpiParent();
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
  AMPI_API("AMPI_Comm_split_type");

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
  AMPI_API("AMPI_Comm_free");
  ampiParent* parent = getAmpiParent();
  int ret = MPI_SUCCESS;
  if (*comm != MPI_COMM_NULL) {
    // FIXME: free user-defined attribute keyvals owned by this communicator
    //ret = parent->freeUserKeyvals(*comm, parent->getKeyvals(*comm));
    if (*comm != MPI_COMM_WORLD && *comm != MPI_COMM_SELF) {
      ampi* ptr = getAmpiInstance(*comm);
      ptr->thisProxy[ptr->thisIndex].ckDestroy();
    }
    *comm = MPI_COMM_NULL;
  }
  return ampiErrhandler("AMPI_Comm_free", ret);
}

AMPI_API_IMPL(int, MPI_Comm_test_inter, MPI_Comm comm, int *flag)
{
  AMPI_API("AMPI_Comm_test_inter");
  *flag = getAmpiParent()->isInter(comm);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Comm_remote_size, MPI_Comm comm, int *size)
{
  AMPI_API("AMPI_Comm_remote_size");
  *size = getAmpiParent()->getRemoteSize(comm);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Comm_remote_group, MPI_Comm comm, MPI_Group *group)
{
  AMPI_API("AMPI_Comm_remote_group");
  *group = getAmpiParent()->getRemoteGroup(comm);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Intercomm_create, MPI_Comm localComm, int localLeader, MPI_Comm peerComm,
                                         int remoteLeader, int tag, MPI_Comm *newintercomm)
{
  AMPI_API("AMPI_Intercomm_create");

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

  vector<int> remoteVec;

  if (localRank == localLeader) {
    int remoteSize;
    MPI_Status sts;
    vector<int> localVec;
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

  localPtr->intercommCreate(remoteVec,rootIndex,localComm,newintercomm);

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Intercomm_merge, MPI_Comm intercomm, int high, MPI_Comm *newintracomm)
{
  AMPI_API("AMPI_Intercomm_merge");

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
    MPI_Request req = ptr->send(MPI_ATA_TAG, ptr->getRank(), &lhigh, 1, MPI_INT, 0, intercomm, 0, I_SEND);
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
  AMPI_API_INIT("AMPI_Abort");
  CkAbort("AMPI: Application called MPI_Abort()!\n");
  return errorcode;
}

AMPI_API_IMPL(int, MPI_Get_count, const MPI_Status *sts, MPI_Datatype dtype, int *count)
{
  AMPI_API("AMPI_Get_count");
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
  AMPI_API("AMPI_Type_lb");

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
  AMPI_API("AMPI_Type_ub");

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
  AMPI_API("AMPI_Get_address");
  *address = (MPI_Aint)location;
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Address, void* location, MPI_Aint *address)
{
  AMPI_API("AMPI_Address");
  return MPI_Get_address(location, address);
}

AMPI_API_IMPL(int, MPI_Status_set_elements, MPI_Status *sts, MPI_Datatype dtype, int count)
{
  AMPI_API("AMPI_Status_set_elements");
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
  AMPI_API("AMPI_Status_set_elements_x");
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
  AMPI_API("AMPI_Get_elements");

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
  AMPI_API("AMPI_Get_elements_x");
  *count = getDDT()->getType(dtype)->getNumBasicElements(sts->MPI_LENGTH);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Pack, const void *inbuf, int incount, MPI_Datatype dtype,
                             void *outbuf, int outsize, int *position, MPI_Comm comm)
{
  AMPI_API("AMPI_Pack");
  CkDDT_DataType* dttype = getDDT()->getType(dtype) ;
  int itemsize = dttype->getSize();
  dttype->serialize((char*)inbuf, ((char*)outbuf)+(*position), incount, outsize, PACK);
  *position += (itemsize*incount);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Unpack, const void *inbuf, int insize, int *position, void *outbuf,
                               int outcount, MPI_Datatype dtype, MPI_Comm comm)
{
  AMPI_API("AMPI_Unpack");
  CkDDT_DataType* dttype = getDDT()->getType(dtype) ;
  int itemsize = dttype->getSize();
  dttype->serialize((char*)outbuf, ((char*)inbuf+(*position)), outcount, insize, UNPACK);
  *position += (itemsize*outcount);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Pack_size, int incount, MPI_Datatype datatype, MPI_Comm comm, int *sz)
{
  AMPI_API("AMPI_Pack_size");
  CkDDT_DataType* dttype = getDDT()->getType(datatype) ;
  *sz = incount*dttype->getSize() ;
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Get_version, int *version, int *subversion)
{
  AMPI_API_INIT("AMPI_Get_version");
  *version = MPI_VERSION;
  *subversion = MPI_SUBVERSION;
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Get_library_version, char *version, int *resultlen)
{
  AMPI_API_INIT("AMPI_Get_library_version");
  const char *ampiNameStr = "Adaptive MPI ";
  strncpy(version, ampiNameStr, MPI_MAX_LIBRARY_VERSION_STRING);
  strncat(version, CmiCommitID, MPI_MAX_LIBRARY_VERSION_STRING - strlen(version));
  *resultlen = strlen(version);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Get_processor_name, char *name, int *resultlen)
{
  AMPI_API_INIT("AMPI_Get_processor_name");
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
  AMPI_API("AMPI_Comm_call_errhandler");
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Comm_create_errhandler, MPI_Comm_errhandler_fn *function, MPI_Errhandler *errhandler)
{
  AMPI_API("AMPI_Comm_create_errhandler");
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Comm_set_errhandler, MPI_Comm comm, MPI_Errhandler errhandler)
{
  AMPI_API("AMPI_Comm_set_errhandler");
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Comm_get_errhandler, MPI_Comm comm, MPI_Errhandler *errhandler)
{
  AMPI_API("AMPI_Comm_get_errhandler");
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Comm_free_errhandler, MPI_Errhandler *errhandler)
{
  AMPI_API("AMPI_Comm_free_errhandler");
  *errhandler = MPI_ERRHANDLER_NULL;
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Errhandler_create, MPI_Handler_function *function, MPI_Errhandler *errhandler)
{
  AMPI_API("AMPI_Errhandler_create");
  return MPI_Comm_create_errhandler(function, errhandler);
}

AMPI_API_IMPL(int, MPI_Errhandler_set, MPI_Comm comm, MPI_Errhandler errhandler)
{
  AMPI_API("AMPI_Errhandler_set");
  return MPI_Comm_set_errhandler(comm, errhandler);
}

AMPI_API_IMPL(int, MPI_Errhandler_get, MPI_Comm comm, MPI_Errhandler *errhandler)
{
  AMPI_API("AMPI_Errhandler_get");
  return MPI_Comm_get_errhandler(comm, errhandler);
}

AMPI_API_IMPL(int, MPI_Errhandler_free, MPI_Errhandler *errhandler)
{
  AMPI_API("AMPI_Errhandler_free");
  return MPI_Comm_free_errhandler(errhandler);
}

AMPI_API_IMPL(int, MPI_Add_error_code, int errorclass, int *errorcode)
{
  AMPI_API("AMPI_Add_error_code");
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Add_error_class, int *errorclass)
{
  AMPI_API("AMPI_Add_error_class");
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Add_error_string, int errorcode, const char *errorstring)
{
  AMPI_API("AMPI_Add_error_string");
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Error_class, int errorcode, int *errorclass)
{
  AMPI_API("AMPI_Error_class");
  *errorclass = errorcode;
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Error_string, int errorcode, char *errorstring, int *resultlen)
{
  AMPI_API("AMPI_Error_string");
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
  AMPI_API("AMPI_Comm_Group");
  *group = getAmpiParent()->comm2group(comm);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Group_union, MPI_Group group1, MPI_Group group2, MPI_Group *newgroup)
{
  AMPI_API("AMPI_Group_union");
  ampiParent *ptr = getAmpiParent();
  vector<int> vec1 = ptr->group2vec(group1);
  vector<int> vec2 = ptr->group2vec(group2);
  vector<int> newvec = unionOp(vec1,vec2);
  *newgroup = ptr->saveGroupStruct(newvec);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Group_intersection, MPI_Group group1, MPI_Group group2, MPI_Group *newgroup)
{
  AMPI_API("AMPI_Group_intersection");
  ampiParent *ptr = getAmpiParent();
  vector<int> vec1 = ptr->group2vec(group1);
  vector<int> vec2 = ptr->group2vec(group2);
  vector<int> newvec = intersectOp(vec1,vec2);
  *newgroup = ptr->saveGroupStruct(newvec);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Group_difference, MPI_Group group1, MPI_Group group2, MPI_Group *newgroup)
{
  AMPI_API("AMPI_Group_difference");
  ampiParent *ptr = getAmpiParent();
  vector<int> vec1 = ptr->group2vec(group1);
  vector<int> vec2 = ptr->group2vec(group2);
  vector<int> newvec = diffOp(vec1,vec2);
  *newgroup = ptr->saveGroupStruct(newvec);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Group_size, MPI_Group group, int *size)
{
  AMPI_API("AMPI_Group_size");
  *size = (getAmpiParent()->group2vec(group)).size();
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Group_rank, MPI_Group group, int *rank)
{
  AMPI_API("AMPI_Group_rank");
  *rank = getAmpiParent()->getRank(group);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Group_translate_ranks, MPI_Group group1, int n, const int *ranks1,
                                              MPI_Group group2, int *ranks2)
{
  AMPI_API("AMPI_Group_translate_ranks");
  ampiParent *ptr = getAmpiParent();
  vector<int> vec1 = ptr->group2vec(group1);
  vector<int> vec2 = ptr->group2vec(group2);
  translateRanksOp(n, vec1, ranks1, vec2, ranks2);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Group_compare, MPI_Group group1,MPI_Group group2, int *result)
{
  AMPI_API("AMPI_Group_compare");
  ampiParent *ptr = getAmpiParent();
  vector<int> vec1 = ptr->group2vec(group1);
  vector<int> vec2 = ptr->group2vec(group2);
  *result = compareVecOp(vec1, vec2);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Group_incl, MPI_Group group, int n, const int *ranks, MPI_Group *newgroup)
{
  AMPI_API("AMPI_Group_incl");
  ampiParent *ptr = getAmpiParent();
  vector<int> vec = ptr->group2vec(group);
  vector<int> newvec = inclOp(n,ranks,vec);
  *newgroup = ptr->saveGroupStruct(newvec);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Group_excl, MPI_Group group, int n, const int *ranks, MPI_Group *newgroup)
{
  AMPI_API("AMPI_Group_excl");
  ampiParent *ptr = getAmpiParent();
  vector<int> vec = ptr->group2vec(group);
  vector<int> newvec = exclOp(n,ranks,vec);
  *newgroup = ptr->saveGroupStruct(newvec);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Group_range_incl, MPI_Group group, int n, int ranges[][3], MPI_Group *newgroup)
{
  AMPI_API("AMPI_Group_range_incl");
  int ret;
  ampiParent *ptr = getAmpiParent();
  vector<int> vec = ptr->group2vec(group);
  vector<int> newvec = rangeInclOp(n,ranges,vec,&ret);
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
  AMPI_API("AMPI_Group_range_excl");
  int ret;
  ampiParent *ptr = getAmpiParent();
  vector<int> vec = ptr->group2vec(group);
  vector<int> newvec = rangeExclOp(n,ranges,vec,&ret);
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
  AMPI_API("AMPI_Group_free");
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Comm_create, MPI_Comm comm, MPI_Group group, MPI_Comm* newcomm)
{
  AMPI_API("AMPI_Comm_create");
  int rank_in_group, key, color, zero;
  MPI_Group group_of_comm;

  vector<int> vec = getAmpiParent()->group2vec(group);
  if(vec.size()==0){
    AMPI_DEBUG("AMPI> In MPI_Comm_create, creating an empty communicator");
    *newcomm = MPI_COMM_NULL;
    return MPI_SUCCESS;
  }

  if(getAmpiParent()->isInter(comm)){
    /* inter-communicator: create a single new comm. */
    ampi *ptr = getAmpiInstance(comm);
    ptr->commCreate(vec, newcomm);
    ptr->barrier();
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
  AMPI_API("AMPI_Comm_create_group");

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

  vector<int> groupPids(groupSize), pids(groupSize, 0);
  std::iota(groupPids.begin(), groupPids.end(), 0);
  MPI_Comm_group(comm, &parentGroup);
  MPI_Group_translate_ranks(group, groupSize, groupPids.data(), parentGroup, pids.data());
  MPI_Group_free(&parentGroup);

  MPI_Comm commOld, tmpInter;
  for (int i=0; i<groupSize; i*=2) {
    int groupId = groupRank/i;
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
  AMPI_API("AMPI_Comm_set_name");
  getAmpiInstance(comm)->setCommName(comm_name);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Comm_get_name, MPI_Comm comm, char *comm_name, int *resultlen)
{
  AMPI_API("AMPI_Comm_get_name");
  getAmpiInstance(comm)->getCommName(comm_name, resultlen);
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Comm_set_info, MPI_Comm comm, MPI_Info info)
{
  AMPI_API("AMPI_Comm_set_info");
  /* FIXME: no-op implementation */
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Comm_get_info, MPI_Comm comm, MPI_Info *info)
{
  AMPI_API("AMPI_Comm_get_info");
  /* FIXME: no-op implementation */
  *info = MPI_INFO_NULL;
  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Comm_create_keyval, MPI_Comm_copy_attr_function *copy_fn,
                                           MPI_Comm_delete_attr_function *delete_fn,
                                           int *keyval, void* extra_state)
{
  AMPI_API("AMPI_Comm_create_keyval");
  int ret = getAmpiParent()->createKeyval(copy_fn,delete_fn,keyval,extra_state);
  return ampiErrhandler("AMPI_Comm_create_keyval", ret);
}

AMPI_API_IMPL(int, MPI_Comm_free_keyval, int *keyval)
{
  AMPI_API("AMPI_Comm_free_keyval");
  vector<int>& keyvals = getAmpiParent()->getKeyvals(MPI_COMM_WORLD);
  int ret = getAmpiParent()->freeUserKeyval(MPI_COMM_WORLD, keyvals, keyval);
  return ampiErrhandler("AMPI_Comm_free_keyval", ret);
}

AMPI_API_IMPL(int, MPI_Comm_set_attr, MPI_Comm comm, int keyval, void* attribute_val)
{
  AMPI_API("AMPI_Comm_set_attr");
  ampiParent *parent = getAmpiParent();
  ampiCommStruct &cs = const_cast<ampiCommStruct &>(parent->comm2CommStruct(comm));
  vector<int>& keyvals = cs.getKeyvals();
  int ret = parent->setAttr(comm, keyvals, keyval, attribute_val);
  return ampiErrhandler("AMPI_Comm_set_attr", ret);
}

AMPI_API_IMPL(int, MPI_Comm_get_attr, MPI_Comm comm, int keyval, void *attribute_val, int *flag)
{
  AMPI_API("AMPI_Comm_get_attr");
  ampiParent *parent = getAmpiParent();
  ampiCommStruct &cs = const_cast<ampiCommStruct &>(parent->comm2CommStruct(comm));
  vector<int>& keyvals = cs.getKeyvals();
  int ret = parent->getAttr(comm, keyvals, keyval, attribute_val, flag);
  return ampiErrhandler("AMPI_Comm_get_attr", ret);
}

AMPI_API_IMPL(int, MPI_Comm_delete_attr, MPI_Comm comm, int keyval)
{
  AMPI_API("AMPI_Comm_delete_attr");
  ampiParent *parent = getAmpiParent();
  ampiCommStruct &cs = const_cast<ampiCommStruct &>(parent->comm2CommStruct(comm));
  vector<int>& keyvals = cs.getKeyvals();
  int ret = parent->deleteAttr(comm, keyvals, keyval);
  return ampiErrhandler("AMPI_Comm_delete_attr", ret);
}

AMPI_API_IMPL(int, MPI_Keyval_create, MPI_Copy_function *copy_fn, MPI_Delete_function *delete_fn,
                                      int *keyval, void* extra_state)
{
  AMPI_API("AMPI_Keyval_create");
  return MPI_Comm_create_keyval(copy_fn, delete_fn, keyval, extra_state);
}

AMPI_API_IMPL(int, MPI_Keyval_free, int *keyval)
{
  AMPI_API("AMPI_Keyval_free");
  return MPI_Comm_free_keyval(keyval);
}

AMPI_API_IMPL(int, MPI_Attr_put, MPI_Comm comm, int keyval, void* attribute_val)
{
  AMPI_API("AMPI_Attr_put");
  return MPI_Comm_set_attr(comm, keyval, attribute_val);
}

AMPI_API_IMPL(int, MPI_Attr_get, MPI_Comm comm, int keyval, void *attribute_val, int *flag)
{
  AMPI_API("AMPI_Attr_get");
  return MPI_Comm_get_attr(comm, keyval, attribute_val, flag);
}

AMPI_API_IMPL(int, MPI_Attr_delete, MPI_Comm comm, int keyval)
{
  AMPI_API("AMPI_Attr_delete");
  return MPI_Comm_delete_attr(comm, keyval);
}

AMPI_API_IMPL(int, MPI_Cart_map, MPI_Comm comm, int ndims, const int *dims,
                                 const int *periods, int *newrank)
{
  AMPI_API("AMPI_Cart_map");

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
  AMPI_API("AMPI_Graph_map");

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
  AMPI_API("AMPI_Cart_create");

  /* Create new cartesian communicator. No attention is being paid to mapping
     virtual processes to processors, which ideally should be handled by the
     load balancer with input from virtual topology information.

     No reorder done here. reorder input is ignored, but still stored in the
     communicator with other VT info.
   */

  int newrank;
  MPI_Cart_map(comm_old, ndims, dims, periods, &newrank);//no change in rank

  ampiParent *ptr = getAmpiParent();
  vector<int> vec = ptr->group2vec(ptr->comm2group(comm_old));
  *comm_cart = getAmpiInstance(comm_old)->cartCreate(vec, ndims, dims);

  if (*comm_cart != MPI_COMM_NULL) {
    ampiCommStruct &c = getAmpiParent()->getCart(*comm_cart);
    ampiTopology *topo = c.getTopology();
    topo->setndims(ndims);
    vector<int> dimsv(dims, dims+ndims), periodsv(periods, periods+ndims), nborsv;
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
  AMPI_API("AMPI_Graph_create");

  if (nnodes == 0) {
    *comm_graph = MPI_COMM_NULL;
    return MPI_SUCCESS;
  }

  /* No mapping done */
  int newrank;
  MPI_Graph_map(comm_old, nnodes, index, edges, &newrank);

  ampiParent *ptr = getAmpiParent();
  vector<int> vec = ptr->group2vec(ptr->comm2group(comm_old));
  getAmpiInstance(comm_old)->graphCreate(vec, comm_graph);
  ampiTopology &topo = *ptr->getGraph(*comm_graph).getTopology();

  vector<int> index_(index, index+nnodes), edges_, nborsv;
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
  AMPI_API("AMPI_Dist_graph_create_adjacent");

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
  vector<int> vec = ptr->group2vec(ptr->comm2group(comm_old));
  getAmpiInstance(comm_old)->distGraphCreate(vec,comm_dist_graph);
  ampiCommStruct &c = ptr->getDistGraph(*comm_dist_graph);
  ampiTopology *topo = c.getTopology();

  topo->setInDegree(indegree);
  topo->setOutDegree(outdegree);

  topo->setAreSourcesWeighted(sourceweights != MPI_UNWEIGHTED);
  if (topo->areSourcesWeighted()) {
    vector<int> tmpSourceWeights(sourceweights, sourceweights+indegree);
    topo->setSourceWeights(tmpSourceWeights);
  }

  topo->setAreDestsWeighted(destweights != MPI_UNWEIGHTED);
  if (topo->areDestsWeighted()) {
    vector<int> tmpDestWeights(destweights, destweights+outdegree);
    topo->setDestWeights(tmpDestWeights);
  }

  vector<int> tmpSources(sources, sources+indegree);
  topo->setSources(tmpSources);

  vector<int> tmpDestinations(destinations, destinations+outdegree);
  topo->setDestinations(tmpDestinations);

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Dist_graph_create, MPI_Comm comm_old, int n, const int sources[], const int degrees[],
                                          const int destinations[], const int weights[], MPI_Info info,
                                          int reorder, MPI_Comm *comm_dist_graph)
{
  AMPI_API("AMPI_Dist_graph_create");

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
  vector<int> vec = ptr->group2vec(ptr->comm2group(comm_old));
  getAmpiInstance(comm_old)->distGraphCreate(vec,comm_dist_graph);
  ampiCommStruct &c = ptr->getDistGraph(*comm_dist_graph);
  ampiTopology *topo = c.getTopology();

  int p = c.getSize();

  vector<int> edgeListIn(p, 0);
  vector<int> edgeListOut(p, 0);
  vector<vector<int> > edgeMatrixIn(p);
  vector<vector<int> > edgeMatrixOut(p);

  for (int i=0; i<p; i++) {
    vector<int> tmpVector(p, 0);
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

  vector<int> edgeCount(2*p);
  vector<int> totalcount(2);
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

  vector<MPI_Request> requests(sends, MPI_REQUEST_NULL);
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
  vector<int> saveSources, saveDestinations;
  for (int i=0; i<2; i++) {
    for (int j=0; j<totalcount[i]; j++) {
      MPI_Probe(MPI_ANY_SOURCE, MPI_ANY_TAG, comm_old, &status);
      MPI_Get_count(&status, MPI_INT, &numEdges);
      vector<int> saveEdges(numEdges);
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
    vector<int> tmpWeights(weights, weights+n);
    topo->setSourceWeights(tmpWeights);
    topo->setDestWeights(tmpWeights);
  }

  MPI_Waitall(sends, requests.data(), MPI_STATUSES_IGNORE);

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Topo_test, MPI_Comm comm, int *status)
{
  AMPI_API("AMPI_Topo_test");

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
  AMPI_API("AMPI_Cartdim_get");

#if AMPI_ERROR_CHECKING
  if (!getAmpiParent()->isCart(comm))
    return ampiErrhandler("AMPI_Cartdim_get", MPI_ERR_TOPOLOGY);
#endif

  *ndims = getAmpiParent()->getCart(comm).getTopology()->getndims();

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Cart_get, MPI_Comm comm, int maxdims, int *dims, int *periods, int *coords)
{
  int i, ndims;

  AMPI_API("AMPI_Cart_get");

#if AMPI_ERROR_CHECKING
  if (!getAmpiParent()->isCart(comm))
    return ampiErrhandler("AMPI_Cart_get", MPI_ERR_TOPOLOGY);
#endif

  ampiCommStruct &c = getAmpiParent()->getCart(comm);
  ampiTopology *topo = c.getTopology();
  ndims = topo->getndims();
  int rank = getAmpiInstance(comm)->getRank();

  const vector<int> &dims_ = topo->getdims();
  const vector<int> &periods_ = topo->getperiods();

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
  AMPI_API("AMPI_Cart_rank");

#if AMPI_ERROR_CHECKING
  if (!getAmpiParent()->isCart(comm))
    return ampiErrhandler("AMPI_Cart_rank", MPI_ERR_TOPOLOGY);
#endif

  ampiCommStruct &c = getAmpiParent()->getCart(comm);
  ampiTopology *topo = c.getTopology();
  int ndims = topo->getndims();
  const vector<int> &dims = topo->getdims();
  const vector<int> &periods = topo->getperiods();

  //create a copy of coords since we are not allowed to modify it
  vector<int> ncoords(coords, coords+ndims);

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
  AMPI_API("AMPI_Cart_coords");

#if AMPI_ERROR_CHECKING
  if (!getAmpiParent()->isCart(comm))
    return ampiErrhandler("AMPI_Cart_coorts", MPI_ERR_TOPOLOGY);
#endif

  ampiCommStruct &c = getAmpiParent()->getCart(comm);
  ampiTopology *topo = c.getTopology();
  int ndims = topo->getndims();
  const vector<int> &dims = topo->getdims();

  for (int i = ndims - 1; i >= 0; i--) {
    if (i < maxdims)
      coords[i] = rank % dims[i];
    rank = (int) (rank / dims[i]);
  }

  return MPI_SUCCESS;
}

// Offset coords[direction] by displacement, and set the rank that
// results
static void cart_clamp_coord(MPI_Comm comm, const vector<int> &dims,
                             const vector<int> &periodicity, int *coords,
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
  AMPI_API("AMPI_Cart_shift");

#if AMPI_ERROR_CHECKING
  if (!getAmpiParent()->isCart(comm))
    return ampiErrhandler("AMPI_Cart_shift", MPI_ERR_TOPOLOGY);
#endif

  ampiCommStruct &c = getAmpiParent()->getCart(comm);
  ampiTopology *topo = c.getTopology();
  int ndims = topo->getndims();

#if AMPI_ERROR_CHECKING
  if ((direction < 0) || (direction >= ndims))
    return ampiErrhandler("AMPI_Cart_shift", MPI_ERR_DIMS);
#endif

  const vector<int> &dims = topo->getdims();
  const vector<int> &periods = topo->getperiods();
  vector<int> coords(ndims);

  int mype = getAmpiInstance(comm)->getRank();
  MPI_Cart_coords(comm, mype, ndims, &coords[0]);

  cart_clamp_coord(comm, dims, periods, &coords[0], direction,  disp, rank_dest);
  cart_clamp_coord(comm, dims, periods, &coords[0], direction, -disp, rank_source);

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Graphdims_get, MPI_Comm comm, int *nnodes, int *nedges)
{
  AMPI_API("AMPI_Graphdim_get");

  ampiCommStruct &c = getAmpiParent()->getGraph(comm);
  ampiTopology *topo = c.getTopology();
  *nnodes = topo->getnvertices();
  const vector<int> &index = topo->getindex();
  *nedges = index[(*nnodes) - 1];

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Graph_get, MPI_Comm comm, int maxindex, int maxedges, int *index, int *edges)
{
  AMPI_API("AMPI_Graph_get");

#if AMPI_ERROR_CHECKING
  if (!getAmpiParent()->isGraph(comm))
    return ampiErrhandler("AMPI_Graph_get", MPI_ERR_TOPOLOGY);
#endif

  ampiCommStruct &c = getAmpiParent()->getGraph(comm);
  ampiTopology *topo = c.getTopology();
  const vector<int> &index_ = topo->getindex();
  const vector<int> &edges_ = topo->getedges();

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
  AMPI_API("AMPI_Graph_neighbors_count");

#if AMPI_ERROR_CHECKING
  if (!getAmpiParent()->isGraph(comm))
    return ampiErrhandler("AMPI_Graph_neighbors_count", MPI_ERR_TOPOLOGY);
#endif

  ampiCommStruct &c = getAmpiParent()->getGraph(comm);
  ampiTopology *topo = c.getTopology();
  const vector<int> &index = topo->getindex();

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
  AMPI_API("AMPI_Graph_neighbors");

#if AMPI_ERROR_CHECKING
  if (!getAmpiParent()->isGraph(comm))
    return ampiErrhandler("AMPI_Graph_neighbors", MPI_ERR_TOPOLOGY);
#endif

  ampiCommStruct &c = getAmpiParent()->getGraph(comm);
  ampiTopology *topo = c.getTopology();
  const vector<int> &index = topo->getindex();
  const vector<int> &edges = topo->getedges();

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
  AMPI_API("AMPI_Dist_graph_neighbors_count");

#if AMPI_ERROR_CHECKING
  if (!getAmpiParent()->isDistGraph(comm)) {
    return ampiErrhandler("AMPI_Dist_graph_neighbors_count", MPI_ERR_TOPOLOGY);
  }
#endif

  ampiParent *ptr = getAmpiParent();
  ampiCommStruct &c = ptr->getDistGraph(comm);
  ampiTopology *topo = c.getTopology();
  *indegree = topo->getInDegree();
  *outdegree = topo->getOutDegree();
  *weighted = topo->areSourcesWeighted() ? 1 : 0;

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Dist_graph_neighbors, MPI_Comm comm, int maxindegree, int sources[], int sourceweights[],
                                             int maxoutdegree, int destinations[], int destweights[])
{
  AMPI_API("AMPI_Dist_graph_neighbors");

#if AMPI_ERROR_CHECKING
  if (!getAmpiParent()->isDistGraph(comm)) {
    return ampiErrhandler("AMPI_Dist_graph_neighbors", MPI_ERR_TOPOLOGY);
  }
  if ((maxindegree < 0) || (maxoutdegree < 0)) {
    return ampiErrhandler("AMPI_Dist_graph_neighbors", MPI_ERR_TOPOLOGY);
  }
#endif

  ampiParent *ptr = getAmpiParent();
  ampiCommStruct &c = ptr->getDistGraph(comm);
  ampiTopology *topo = c.getTopology();

  const vector<int> &tmpSources = topo->getSources();
  const vector<int> &tmpSourceWeights = topo->getSourceWeights();
  const vector<int> &tmpDestinations = topo->getDestinations();
  const vector<int> &tmpDestWeights = topo->getDestWeights();

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
void ampi::findNeighbors(MPI_Comm comm, int rank, vector<int>& neighbors) const noexcept {
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
  AMPI_API("AMPI_Dims_create");

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
    vector<int> pdims(d);

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
  AMPI_API("AMPI_Cart_sub");

  int i, ndims;
  int color = 1, key = 1;

#if AMPI_ERROR_CHECKING
  if (!getAmpiParent()->isCart(comm))
    return ampiErrhandler("AMPI_Cart_sub", MPI_ERR_TOPOLOGY);
#endif

  int rank = getAmpiInstance(comm)->getRank();
  ampiCommStruct &c = getAmpiParent()->getCart(comm);
  ampiTopology *topo = c.getTopology();
  ndims = topo->getndims();
  const vector<int> &dims = topo->getdims();
  int num_remain_dims = 0;

  vector<int> coords(ndims);
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

  getAmpiInstance(comm)->split(color, key, newcomm, MPI_CART);

  ampiCommStruct &newc = getAmpiParent()->getCart(*newcomm);
  ampiTopology *newtopo = newc.getTopology();
  newtopo->setndims(num_remain_dims);
  vector<int> dimsv;
  const vector<int> &periods = topo->getperiods();
  vector<int> periodsv;

  for (i = 0; i < ndims; i++) {
    if (remain_dims[i]) {
      dimsv.push_back(dims[i]);
      periodsv.push_back(periods[i]);
    }
  }
  newtopo->setdims(dimsv);
  newtopo->setperiods(periodsv);

  vector<int> nborsv;
  getAmpiInstance(*newcomm)->findNeighbors(*newcomm, getAmpiParent()->getRank(*newcomm), nborsv);
  newtopo->setnbors(nborsv);

  return MPI_SUCCESS;
}

AMPI_API_IMPL(int, MPI_Type_get_envelope, MPI_Datatype datatype, int *ni, int *na,
                                          int *nd, int *combiner)
{
  AMPI_API("AMPI_Type_get_envelope");

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
  AMPI_API("AMPI_Type_get_contents");

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

/******** AMPI Extensions to the MPI standard *********/

CLINKAGE int AMPI_Init_universe(int * unicomm)
{
  AMPI_API("AMPI_Init_universe");
  for(int i=0; i<_mpi_nworlds; i++) {
    unicomm[i] = MPI_COMM_UNIVERSE[i];
  }
  return MPI_SUCCESS;
}

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
  AMPI_API("AMPI_Migrate");
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
        TCHARM_Migrate();
      }
      else if (strncmp(value, "async", MPI_MAX_INFO_VAL) == 0) {
        TCHARM_Async_Migrate();
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
        getAmpiInstance(MPI_COMM_WORLD)->barrier();
        getAmpiParent()->startCheckpoint(&value[offset]);
      }
      else if (strncmp(value, "in_memory", MPI_MAX_INFO_VAL) == 0) {
#if CMK_MEM_CHECKPOINT
        getAmpiInstance(MPI_COMM_WORLD)->barrier();
        getAmpiParent()->startCheckpoint("");
#else
        CkPrintf("AMPI> Error: In-memory checkpoint/restart is not enabled!\n");
        CkAbort("AMPI> Error: Recompile Charm++/AMPI with CMK_MEM_CHECKPOINT.\n");
#endif
      }
      else if (strncmp(value, "message_logging", MPI_MAX_INFO_VAL) == 0) {
#if CMK_MESSAGE_LOGGING
        TCHARM_Migrate();
#else
        CkPrintf("AMPI> Error: Message logging is not enabled!\n");
        CkAbort("AMPI> Error: Recompile Charm++/AMPI with CMK_MESSAGE_LOGGING.\n");
#endif
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

#if (defined(_FAULT_MLOG_) || defined(_FAULT_CAUSAL_))
  ampi *currentAmpi = getAmpiInstance(MPI_COMM_WORLD);
  CpvAccess(_currentObj) = currentAmpi;
#endif

#if CMK_BIGSIM_CHARM
  TRACE_BG_ADD_TAG("AMPI_MIGRATE");
#endif
  return MPI_SUCCESS;
}

#if CMK_FAULT_EVAC
CLINKAGE
int AMPI_Evacuate(void)
{
  //AMPI_API("AMPI_Evacuate");
  TCHARM_Evacuate();
  return MPI_SUCCESS;
}
#endif

CLINKAGE
int AMPI_Migrate_to_pe(int dest)
{
  AMPI_API("AMPI_Migrate_to_pe");
  TCHARM_Migrate_to(dest);
#if CMK_BIGSIM_CHARM
  TRACE_BG_ADD_TAG("AMPI_MIGRATE_TO_PE");
#endif
  return MPI_SUCCESS;
}

CLINKAGE
int AMPI_Set_migratable(int mig)
{
  AMPI_API("AMPI_Set_migratable");
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
  AMPI_API("AMPI_Load_start_measure");
  LBTurnInstrumentOn();
  return MPI_SUCCESS;
}

CLINKAGE
int AMPI_Load_stop_measure(void)
{
  AMPI_API("AMPI_Load_stop_measure");
  LBTurnInstrumentOff();
  return MPI_SUCCESS;
}

CLINKAGE
int AMPI_Load_reset_measure(void)
{
  AMPI_API("AMPI_Load_reset_measure");
  LBClearLoads();
  return MPI_SUCCESS;
}

CLINKAGE
int AMPI_Load_set_value(double value)
{
  AMPI_API("AMPI_Load_set_value");
  ampiParent *ptr = getAmpiParent();
  ptr->setObjTime(value);
  return MPI_SUCCESS;
}

void _registerampif(void) {
  _registerampi();
}

CLINKAGE
int AMPI_Register_main(MPI_MainFn mainFn,const char *name)
{
  AMPI_API("AMPI_Register_main");
  if (TCHARM_Element()==0)
  { // I'm responsible for building the TCHARM threads:
    ampiCreateMain(mainFn,name,strlen(name));
  }
  return MPI_SUCCESS;
}

FLINKAGE
void FTN_NAME(MPI_REGISTER_MAIN,mpi_register_main)
(MPI_MainFn mainFn,const char *name,int nameLen)
{
  AMPI_API("AMPI_register_main");
  if (TCHARM_Element()==0)
  { // I'm responsible for building the TCHARM threads:
    ampiCreateMain(mainFn,name,nameLen);
  }
}

CLINKAGE
int AMPI_Register_pup(MPI_PupFn fn, void *data, int *idx)
{
  AMPI_API("AMPI_Register_pup");
  *idx = TCHARM_Register(data, fn);
  return MPI_SUCCESS;
}

CLINKAGE
int AMPI_Register_about_to_migrate(MPI_MigrateFn fn)
{
  AMPI_API("AMPI_Register_about_to_migrate");
  ampiParent *thisParent = getAmpiParent();
  thisParent->setUserAboutToMigrateFn(fn);
  return MPI_SUCCESS;
}

CLINKAGE
int AMPI_Register_just_migrated(MPI_MigrateFn fn)
{
  AMPI_API("AMPI_Register_just_migrated");
  ampiParent *thisParent = getAmpiParent();
  thisParent->setUserJustMigratedFn(fn);
  return MPI_SUCCESS;
}

CLINKAGE
int AMPI_Get_pup_data(int idx, void *data)
{
  AMPI_API("AMPI_Get_pup_data");
  data = TCHARM_Get_userdata(idx);
  return MPI_SUCCESS;
}

CLINKAGE
int AMPI_Type_is_contiguous(MPI_Datatype datatype, int *flag)
{
  AMPI_API("AMPI_Type_is_contiguous");
  *flag = getDDT()->isContig(datatype);
  return MPI_SUCCESS;
}

CLINKAGE
int AMPI_Print(const char *str)
{
  AMPI_API("AMPI_Print");
  ampiParent *ptr = getAmpiParent();
  CkPrintf("[%d] %s\n", ptr->thisIndex, str);
  return MPI_SUCCESS;
}

CLINKAGE
int AMPI_Suspend(void)
{
  AMPI_API("AMPI_Suspend");
  getAmpiParent()->block();
  return MPI_SUCCESS;
}

CLINKAGE
int AMPI_Yield(void)
{
  AMPI_API("AMPI_Yield");
  getAmpiParent()->yield();
  return MPI_SUCCESS;
}

CLINKAGE
int AMPI_Resume(int dest, MPI_Comm comm)
{
  AMPI_API("AMPI_Resume");
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

#if CMK_BIGSIM_CHARM
extern "C" void startCFnCall(void *param,void *msg)
{
  BgSetStartEvent();
  ampi *ptr = (ampi*)param;
  ampi::bcastraw(NULL, 0, ptr->getProxy());
  delete (CkReductionMsg*)msg;
}

CLINKAGE
int AMPI_Set_start_event(MPI_Comm comm)
{
  AMPI_API("AMPI_Set_start_event");
  CkAssert(comm == MPI_COMM_WORLD);

  ampi *ptr = getAmpiInstance(comm);
  int rank = ptr->getRank();
  int size = ptr->getSize();

  CkDDT_DataType *ddt_type = ptr->getDDT()->getType(MPI_INT);

  CkReductionMsg *msg=makeRednMsg(ddt_type, NULL, 0, MPI_INT, rank, size, MPI_SUM);
  if (CkMyPe() == 0) {
    CkCallback allreduceCB(startCFnCall, ptr);
    msg->setCallback(allreduceCB);
  }
  ptr->contribute(msg);

  /*HACK: Use recv() to block until the reduction data comes back*/
  if(-1==ptr->recv(MPI_BCAST_TAG, -1, NULL, 0, MPI_INT, MPI_COMM_WORLD))
    CkAbort("AMPI> MPI_Allreduce called with different values on different processors!");

  return MPI_SUCCESS;
}

CLINKAGE
int AMPI_Set_end_event(void)
{
  AMPI_API("AMPI_Set_end_event");
  return MPI_SUCCESS;
}
#endif // CMK_BIGSIM_CHARM

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
  return isComplete();
}

int GPUReq::wait(MPI_Status *sts) noexcept
{
  (void)sts;
  while (!isComplete()) {
    getAmpiParent()->block();
  }
  return 0;
}

void GPUReq::receive(ampi *ptr, AmpiMsg *msg, bool deleteMsg/*=true*/) noexcept
{
  CkAbort("GPUReq::receive should never be called");
}

void GPUReq::receive(ampi *ptr, CkReductionMsg *msg) noexcept
{
  CkAbort("GPUReq::receive should never be called");
}

void GPUReq::print() const noexcept {
  AmpiRequest::print();
}

void AMPI_GPU_complete(void *request, void* dummy) noexcept
{
  GPUReq *req = static_cast<GPUReq *>(request);
  req->setComplete(true);
  ampi *ptr = static_cast<ampi *>(req->buf);
  ptr->unblock();
}

/* Submit hapiWorkRequest and corresponding GPU request. */
CLINKAGE
int AMPI_GPU_Iinvoke_wr(hapiWorkRequest *to_call, MPI_Request *request)
{
  AMPI_API("AMPI_GPU_Iinvoke");

  ampi* ptr = getAmpiInstance(MPI_COMM_WORLD);
  GPUReq* newreq = new GPUReq();
  *request = ptr->postReq(newreq);

  // A callback that completes the corresponding request
  CkCallback *cb = new CkCallback(&AMPI_GPU_complete, newreq);
  to_call->setCallback(cb);

  hapiEnqueue(to_call);
}

/* Submit GPU request that will be notified of completion once the previous
 * operations in the given CUDA stream are complete */
CLINKAGE
int AMPI_GPU_Iinvoke(cudaStream_t stream, MPI_Request *request)
{
  AMPI_API("AMPI_GPU_Iinvoke");

  ampi* ptr = getAmpiInstance(MPI_COMM_WORLD);
  GPUReq* newreq = new GPUReq();
  *request = ptr->postReq(newreq);

  // A callback that completes the corresponding request
  CkCallback *cb = new CkCallback(&AMPI_GPU_complete, newreq);

  hapiAddCallback(stream, cb, NULL);
}

CLINKAGE
int AMPI_GPU_Invoke_wr(hapiWorkRequest *to_call)
{
  AMPI_API("AMPI_GPU_Invoke");

  MPI_Request req;
  AMPI_GPU_Iinvoke_wr(to_call, &req);
  MPI_Wait(&req, MPI_STATUS_IGNORE);

  return MPI_SUCCESS;
}

CLINKAGE
int AMPI_GPU_Invoke(cudaStream_t stream)
{
  AMPI_API("AMPI_GPU_Invoke");

  MPI_Request req;
  AMPI_GPU_Iinvoke(stream, &req);
  MPI_Wait(&req, MPI_STATUS_IGNORE);

  return MPI_SUCCESS;
}
#endif // CMK_CUDA

#include "ampi.def.h"

