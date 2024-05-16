#ifndef _AMPIIMPL_H
#define _AMPIIMPL_H

#ifndef __STDC_FORMAT_MACROS
# define __STDC_FORMAT_MACROS
#endif
#ifndef __STDC_LIMIT_MACROS
# define __STDC_LIMIT_MACROS
#endif
#include <stdint.h>
#include <inttypes.h>

#include <string.h> /* for strlen */
#include <algorithm>
#include <numeric>
#include <bitset>
#include <complex>
#include <iostream>
#include <sstream>

#include "ampi.h"
#include "ddt.h"
#include "charm++.h"
#include "tcharm.h"
#include "tcharmc.h"

#if CMK_AMPI_WITH_ROMIO
# include "mpio_globals.h"
#endif

extern int quietModeRequested;

// Set to 1 to print debug statements
#define AMPI_DO_DEBUG 0

#if AMPI_DO_DEBUG

#define AMPI_DEBUG(...) CkPrintf(__VA_ARGS__)

// Support for variable-argument macros (up to 16 arguments)
#define FE_1(WHAT, X) WHAT(X, true /*last argument*/) 
#define FE_2(WHAT, X, ...) WHAT(X, false)FE_1(WHAT, __VA_ARGS__)
#define FE_3(WHAT, X, ...) WHAT(X, false)FE_2(WHAT, __VA_ARGS__)
#define FE_4(WHAT, X, ...) WHAT(X, false)FE_3(WHAT, __VA_ARGS__)
#define FE_5(WHAT, X, ...) WHAT(X, false)FE_4(WHAT, __VA_ARGS__)
#define FE_6(WHAT, X, ...) WHAT(X, false)FE_5(WHAT, __VA_ARGS__)
#define FE_7(WHAT, X, ...) WHAT(X, false)FE_6(WHAT, __VA_ARGS__)
#define FE_8(WHAT, X, ...) WHAT(X, false)FE_7(WHAT, __VA_ARGS__)
#define FE_9(WHAT, X, ...) WHAT(X, false)FE_8(WHAT, __VA_ARGS__)
#define FE_10(WHAT, X, ...) WHAT(X,false)FE_9(WHAT, __VA_ARGS__)
#define FE_11(WHAT, X, ...) WHAT(X,false)FE_10(WHAT, __VA_ARGS__)
#define FE_12(WHAT, X, ...) WHAT(X,false)FE_11(WHAT, __VA_ARGS__)
#define FE_13(WHAT, X, ...) WHAT(X,false)FE_12(WHAT, __VA_ARGS__)
#define FE_14(WHAT, X, ...) WHAT(X,false)FE_13(WHAT, __VA_ARGS__)
#define FE_15(WHAT, X, ...) WHAT(X,false)FE_14(WHAT, __VA_ARGS__)
#define FE_16(WHAT, X, ...) WHAT(X,false)FE_15(WHAT, __VA_ARGS__)

#define GET_MACRO(_1,_2,_3,_4,_5,_6,_7,_8,_9,_10,_11,_12,_13,_14,_15,_16,NAME,...) NAME

// Perform 'action' (PRINT_ARG in this case) on each argument
#define FOR_EACH(action,...) \
  GET_MACRO(__VA_ARGS__,FE_16,FE_15,FE_14,FE_13,\
    FE_12,FE_11,FE_10,FE_9,FE_8,FE_7,FE_6,FE_5,FE_4,FE_3,FE_2,FE_1)(action,__VA_ARGS__)

// Prints a single argument name and its value (unless the argument name is
// '""', which indicates a nonexistent argument)
#define PRINT_ARG(arg, last) \
  if ("\"\"" != #arg) stream << #arg << "=" << arg << (last ? "" : ", ");

// Prints PE:VP, function name, and argument name/value for each function argument
#define AMPI_DEBUG_ARGS(function_name, ...) \
  if(!quietModeRequested) { \
  std::stringstream stream; \
  stream << "[" << CkMyPe() << ":" << \
  (isAmpiThread() ? getAmpiParent()->thisIndex : -1) << "] " << function_name << "("; \
  FOR_EACH(PRINT_ARG, __VA_ARGS__); \
  stream << ")\n"; \
  std::cout << stream.str() << std::flush; }

#else // !AMPI_DO_DEBUG

#define AMPI_DEBUG(...) /*empty*/
#define AMPI_DEBUG_ARGS(...) /*empty*/

#endif // AMPI_DO_DEBUG


/*
 * All MPI_* routines must be defined using the AMPI_API_IMPL macro.
 * All calls inside AMPI to MPI_* routines must use MPI_* as the name.
 * There are two reasons for this:
 *
 * 1. AMPI supports the PMPI interface only on Linux.
 *
 * 2. When AMPI is built on top of MPI, we rename the user's MPI_* calls as AMPI_*.
 */
#define STRINGIFY_INTERNAL(a) #a
#define STRINGIFY(a) STRINGIFY_INTERNAL(a)

// keep in sync with ampi_noimpl.C
#if AMPI_HAVE_PMPI
  #define AMPI_API_IMPL(ret, name, ...) \
    CLINKAGE \
    __attribute__((weak, alias(STRINGIFY(name)))) \
    ret P##name(__VA_ARGS__); \
    CLINKAGE \
    __attribute__((weak)) \
    ret name(__VA_ARGS__)
#else // not Linux (no PMPI support):
  #define AMPI_API_IMPL(ret, name, ...) \
    CLINKAGE \
    ret name(__VA_ARGS__)
#endif

extern char * ampi_binary_path;
extern bool ampiUsingPieglobals;

#if AMPIMSGLOG
#include "ckliststring.h"
static CkListString msgLogRanks;
static int msgLogWrite;
static int msgLogRead;
static char *msgLogFilename;

#if CMK_USE_ZLIB && 0
#include <zlib.h>
namespace PUP{
class zdisk : public er {
 protected:
  gzFile F;//Disk file to read from/write to
  zdisk(unsigned int type,gzFile f):er(type),F(f) {}
  zdisk(const zdisk &p);			//You don't want to copy
  void operator=(const zdisk &p);	// You don't want to copy

  //For seeking (pack/unpack in different orders)
  virtual void impl_startSeek(seekBlock &s); /*Begin a seeking block*/
  virtual int impl_tell(seekBlock &s); /*Give the current offset*/
  virtual void impl_seek(seekBlock &s,int off); /*Seek to the given offset*/
};

//For packing to a disk file
class tozDisk : public zdisk {
 protected:
  //Generic bottleneck: pack n items of size itemSize from p.
  virtual void bytes(void *p,int n,size_t itemSize,dataType t);
 public:
  //Write data to the given file pointer
  // (must be opened for binary write)
  // You must close the file yourself when done.
  tozDisk(gzFile f):zdisk(IS_PACKING,f) {}
};

//For unpacking from a disk file
class fromzDisk : public zdisk {
 protected:
  //Generic bottleneck: unpack n items of size itemSize from p.
  virtual void bytes(void *p,int n,size_t itemSize,dataType t);
 public:
  //Write data to the given file pointer
  // (must be opened for binary read)
  // You must close the file yourself when done.
  fromzDisk(gzFile f):zdisk(IS_UNPACKING,f) {}
};
}; // namespace PUP
#endif
#endif // AMPIMSGLOG

/* AMPI sends messages inline to PE-local destination VPs if
 * tracing is disabled (see bug #1640). */
#ifndef AMPI_PE_LOCAL_IMPL
#define AMPI_PE_LOCAL_IMPL ( !CMK_TRACE_ENABLED )
#endif

/* AMPI sends messages using a zero copy protocol to Node-local destination VPs
 * tracing is disabled (such msgs are currently untraced). */
#ifndef AMPI_NODE_LOCAL_IMPL
#define AMPI_NODE_LOCAL_IMPL ( CMK_SMP && !CMK_TRACE_ENABLED )
#endif

/* messages larger than or equal to this threshold may block on a matching recv if local to the PE*/
#ifndef AMPI_PE_LOCAL_THRESHOLD_DEFAULT
#define AMPI_PE_LOCAL_THRESHOLD_DEFAULT 8192
#endif

/* messages larger than or equal to this threshold may block on a matching recv if local to the Node */
#ifndef AMPI_NODE_LOCAL_THRESHOLD_DEFAULT
#define AMPI_NODE_LOCAL_THRESHOLD_DEFAULT 32768
#endif

/* messages larger than or equal to this threshold will always block on a matching recv */
#ifndef AMPI_SSEND_THRESHOLD_DEFAULT
#if   CMK_USE_IBVERBS || CMK_CONVERSE_UGNI
#define AMPI_SSEND_THRESHOLD_DEFAULT 262144
#else
#define AMPI_SSEND_THRESHOLD_DEFAULT 131072
#endif
#endif

#ifndef AMPI_RDMA_IMPL
#define AMPI_RDMA_IMPL 1
#endif

/* contiguous messages larger than or equal to this threshold are sent via RDMA */
#ifndef AMPI_RDMA_THRESHOLD_DEFAULT
#define AMPI_RDMA_THRESHOLD_DEFAULT 102400
#endif

extern int AMPI_RDMA_THRESHOLD;

#define AMPI_ALLTOALL_THROTTLE   64
#define AMPI_ALLTOALL_SHORT_MSG  256
#define AMPI_ALLTOALL_LONG_MSG   32768

typedef void (*MPI_MigrateFn)(void);

/*
 * AMPI Message Matching (Amm) Interface:
 * messages are matched on 2 ints: [tag, src]
 */
#define AMM_TAG   0
#define AMM_SRC   1
#define AMM_NTAGS 2

// Number of AmmEntry<T>'s in AmmEntryPool for pt2pt msgs:
#ifndef AMPI_AMM_PT2PT_POOL_SIZE
#define AMPI_AMM_PT2PT_POOL_SIZE 32
#endif

// Number of AmmEntry<T>'s in AmmEntryPool for coll msgs:
#ifndef AMPI_AMM_COLL_POOL_SIZE
#define AMPI_AMM_COLL_POOL_SIZE 4
#endif

class AmpiRequestList;

typedef void (*AmmPupMessageFn)(PUP::er& p, void **msg);

template <class T>
class AmmEntry {
 public:
  int tags[AMM_NTAGS]; // [tag, src]
  AmmEntry<T>* next;
  T msg; // T is either an AmpiRequest* or an AmpiMsg*
  AmmEntry(T m) noexcept { tags[AMM_TAG] = m->getTag(); tags[AMM_SRC] = m->getSrcRank(); next = NULL; msg = m; }
  AmmEntry(int tag, int src, T m) noexcept { tags[AMM_TAG] = tag; tags[AMM_SRC] = src; next = NULL; msg = m; }
  AmmEntry() = default;
  ~AmmEntry() = default;
};

template <class T, size_t N>
class Amm {
 public:
  AmmEntry<T>* first;
  AmmEntry<T>** lasth;

 private:
  int startIdx;
  std::bitset<N> validEntries;
  std::array<AmmEntry<T>, N> entryPool;

 public:
  Amm() noexcept : first(NULL), lasth(&first), startIdx(0) { validEntries.reset();  }
  ~Amm() = default;
  inline AmmEntry<T>* newEntry(int tag, int src, T msg) noexcept {
    if (validEntries.all()) {
      return new AmmEntry<T>(tag, src, msg);
    } else {
      for (int i=startIdx; i<validEntries.size(); i++) {
        if (!validEntries[i]) {
          validEntries[i] = 1;
          AmmEntry<T>* ent = new (&entryPool[i]) AmmEntry<T>(tag, src, msg);
          startIdx = i+1;
          return ent;
        }
      }
      CkAbort("AMPI> failed to find a free entry in pool!");
      return NULL;
    }
  }
  inline AmmEntry<T>* newEntry(T msg) noexcept {
    if (validEntries.all()) {
      return new AmmEntry<T>(msg);
    } else {
      for (int i=startIdx; i<validEntries.size(); i++) {
        if (!validEntries[i]) {
          validEntries[i] = 1;
          AmmEntry<T>* ent = new (&entryPool[i]) AmmEntry<T>(msg);
          startIdx = i+1;
          return ent;
        }
      }
      CkAbort("AMPI> failed to find a free entry in pool!");
      return NULL;
    }
  }
  inline void deleteEntry(AmmEntry<T> *ent) noexcept {
    if (ent >= &entryPool.front() && ent <= &entryPool.back()) {
      int idx = (int)((intptr_t)ent - (intptr_t)&entryPool.front()) / sizeof(AmmEntry<T>);
      validEntries[idx] = 0;
      startIdx = std::min(idx, startIdx);
    } else {
      delete ent;
    }
  }
  void freeAll() noexcept;
  void flushMsgs() noexcept;
  inline bool match(const int tags1[AMM_NTAGS], const int tags2[AMM_NTAGS]) const noexcept;
  inline void put(T msg) noexcept;
  inline void put(int tag, int src, T msg) noexcept;
  inline T get(int tag, int src, int* rtags=NULL) noexcept;
  inline T probe(int tag, int src, int* rtags) noexcept;
  inline int size() const noexcept;
  void pup(PUP::er& p, AmmPupMessageFn msgpup) noexcept;
};

PUPfunctionpointer(MPI_User_function*)

/*
 * OpStruct's are used to lookup an MPI_User_function* and check its commutativity.
 * They are also used to create AmpiOpHeader's, which are transmitted in reductions
 * that are user-defined or else lack an equivalent Charm++ reducer type.
 *
 * Note: when running with PIEglobals, func is stored as its offset from the rank's
 *       base address, so that the real function can be looked up relative to any
 *       rank's base address via ampiPeMgr.
 */
class OpStruct {
 public:
  MPI_User_function* func;
  bool isCommutative;
 private:
  bool isValid;

  MPI_User_function *createUserFunc(MPI_User_function *f) noexcept {
    if (ampiUsingPieglobals) { // handle PIEglobals code relocation:
      const CthThread th = TCharm::get()->getThread();
      CmiIsomallocContext ctx = CmiIsomallocGetThreadContext(th);
      const CmiIsomallocRegion heap = CmiIsomallocContextGetUsedExtent(ctx);
      char *basePtr = (char *)heap.start;
      return (MPI_User_function *)((char *)f - basePtr);
    } else {
      return f;
    }
  }

 public:
  OpStruct() = default;
  OpStruct(MPI_User_function* f) noexcept : func(createUserFunc(f)), isCommutative(true), isValid(true) {}
  OpStruct(MPI_User_function* f, bool c) noexcept : func(createUserFunc(f)), isCommutative(c), isValid(true) {}
  void init(MPI_User_function* f, bool c) noexcept {
    func = createUserFunc(f);
    isCommutative = c;
    isValid = true;
  }
  bool isFree() const noexcept { return !isValid; }
  void free() noexcept { isValid = false; }
  void pup(PUP::er &p) {
    p|func;  p|isCommutative;  p|isValid;
  }
};

class AmpiOpHeader {
 public:
  MPI_User_function* func;
  MPI_Datatype dtype;
  int len;
  int szdata;
  AmpiOpHeader(MPI_User_function* f,MPI_Datatype d,int l,int szd) noexcept :
    func(f),dtype(d),len(l),szdata(szd) { }
};

/*
 * For within-process Ssend's, we use this in place of a CkNcpyBuffer object in the
 * AmpiMsg's data. This allows us to handle non-contiguous DDTs directly and to avoid
 * the cost of pinning memory when doing Ssend's on the same PE and in the same process.
 */
class AmpiNcpyShmBuffer {
 private:
  int node;
  int idx;
  int count;
  int length;
  char* buf;
  CkDDT_DataType* ddt;
  MPI_Request sreq;

 public:
  AmpiNcpyShmBuffer() = default;
  AmpiNcpyShmBuffer(int idx_, int count_, char* buf_, CkDDT_DataType* ddt_, MPI_Request sreq_) noexcept
    : idx(idx_), count(count_), buf(buf_), ddt(ddt_), sreq(sreq_)
  {
    node = CkMyNode();
    length = ddt_->getSize(count_);
  }
  ~AmpiNcpyShmBuffer() = default;
  inline int getNode() const noexcept { return node; }
  inline int getIdx() const noexcept { return idx; }
  inline int getCount() const noexcept { return count; }
  inline int getLength() const noexcept { return length; }
  inline int getSreqIdx() const noexcept { return sreq; }
  inline char* getBuf() const noexcept { CkAssert(node == CkMyNode()); return buf; }
  inline CkDDT_DataType* getDDT() const noexcept { CkAssert(node == CkMyNode()); return ddt; }
};
PUPbytes(AmpiNcpyShmBuffer) // PUP as bytes b/c buf & ddt are only meant to be accessed from within shared memory

//------------------- added by YAN for one-sided communication -----------
/* the index is unique within a communicator */
class WinStruct{
 public:
  MPI_Comm comm;
  int index;

  void * base = nullptr;
  MPI_Aint size = 0;
  int disp_unit = 0;
  int create_flavor = MPI_WIN_FLAVOR_CREATE;
  int model = MPI_WIN_SEPARATE;

  // Windows created with MPI_Win_allocate/MPI_Win_allocate_shared need to free their
  // memory region on MPI_Win_free.
  bool ownsMemory = false;

private:
  bool areRecvsPosted;
  bool inEpoch;
  std::vector<int> exposureRankList;
  std::vector<int> accessRankList;
  std::vector<MPI_Request> requestList;

public:
  WinStruct() noexcept : comm(MPI_COMM_NULL), index(-1), areRecvsPosted(false), inEpoch(false) {
    exposureRankList.clear(); accessRankList.clear(); requestList.clear();
  }
  WinStruct(MPI_Comm comm_, int index_) noexcept : comm(comm_), index(index_), areRecvsPosted(false), inEpoch(false) {
    exposureRankList.clear(); accessRankList.clear(); requestList.clear();
  }
  void pup(PUP::er &p) noexcept {
    p|comm; p|index; p|ownsMemory; p|areRecvsPosted; p|inEpoch; p|exposureRankList; p|accessRankList; p|requestList;
  }
  void clearEpochAccess() noexcept {
    accessRankList.clear(); inEpoch = false;
  }
  void clearEpochExposure() noexcept {
    exposureRankList.clear(); areRecvsPosted = false; requestList.clear(); inEpoch=false;
  }
  std::vector<int>& getExposureRankList() noexcept {return exposureRankList;}
  std::vector<int>& getAccessRankList() noexcept {return accessRankList;}
  void setExposureRankList(std::vector<int> &tmpExposureRankList) noexcept {exposureRankList = tmpExposureRankList;}
  void setAccessRankList(std::vector<int> &tmpAccessRankList) noexcept {accessRankList = tmpAccessRankList;}
  std::vector<int>& getRequestList() noexcept {return requestList;}
  bool AreRecvsPosted() const noexcept {return areRecvsPosted;}
  void setAreRecvsPosted(bool setR) noexcept {areRecvsPosted = setR;}
  bool isInEpoch() const noexcept {return inEpoch;}
  void setInEpoch(bool arg) noexcept {inEpoch = arg;}
};

class lockQueueEntry {
 public:
  int requestRank;
  int lock_type;
  lockQueueEntry (int _requestRank, int _lock_type) noexcept
    : requestRank(_requestRank), lock_type(_lock_type) {}
  lockQueueEntry() = default;
};

typedef CkQ<lockQueueEntry *> LockQueue;

class ampiParent;

class win_obj {
 public:
  void *baseAddr;
  MPI_Aint winSize;
  int disp_unit;
  MPI_Comm comm;

  int owner; // Rank of owner of the lock, -1 if not locked
  LockQueue lockQueue; // queue of waiting processors for the lock
                       // top of queue is the one holding the lock
                       // queue is empty if lock is not applied
  std::string winName;
  bool initflag;

  std::unordered_map<int, uintptr_t> attributes;

  void setName(const char *src) noexcept;
  void getName(char *src,int *len) noexcept;

 public:
  void pup(PUP::er &p) noexcept;

  win_obj() noexcept;
  win_obj(const char *name, void *base, MPI_Aint size, int disp_unit, MPI_Comm comm) noexcept;
  ~win_obj() noexcept;

  int create(const char *name, void *base, MPI_Aint size, int disp_unit,
             MPI_Comm comm) noexcept;
  int free() noexcept;

  std::unordered_map<int, uintptr_t> & getAttributes() { return attributes; }

  int put(void *orgaddr, int orgcnt, int orgunit,
          MPI_Aint targdisp, int targcnt, int targunit) noexcept;

  int get(void *orgaddr, int orgcnt, int orgunit,
          MPI_Aint targdisp, int targcnt, int targunit) noexcept;
  int accumulate(void *orgaddr, int count, MPI_Aint targdisp, MPI_Datatype targtype,
                 MPI_Op op, ampiParent* pptr) noexcept;

  int iget(int orgcnt, MPI_Datatype orgtype,
          MPI_Aint targdisp, int targcnt, MPI_Datatype targtype) noexcept;
  int igetWait(MPI_Request *req, MPI_Status *status) noexcept;
  int igetFree(MPI_Request *req, MPI_Status *status) noexcept;

  int fence() noexcept;

  int lock(int requestRank, int lock_type) noexcept;
  int unlock(int requestRank) noexcept;

  int wait() noexcept;
  int post() noexcept;
  int start() noexcept;
  int complete() noexcept;

  void lockTopQueue() noexcept;
  void enqueue(int requestRank, int lock_type) noexcept;
  void dequeue() noexcept;
  bool emptyQueue() noexcept;
};
//-----------------------End of code by YAN ----------------------

class KeyvalPair{
 protected:
  std::string key;
  std::string val;
 public:
  KeyvalPair() = default;
  KeyvalPair(const char* k, const char* v) noexcept;
  ~KeyvalPair() = default;
  void pup(PUP::er& p) noexcept {
    p|key;
    p|val;
  }
  friend class InfoStruct;
};

class InfoStruct{
  CkPupPtrVec<KeyvalPair> nodes;
  bool valid;
 public:
  InfoStruct() noexcept : valid(true) { }
  void setvalid(bool valid_) noexcept { valid = valid_; }
  bool getvalid() const noexcept { return valid; }
  int set(const char* k, const char* v) noexcept;
  int dup(InfoStruct& src) noexcept;
  int get(const char* k, int vl, char*& v, int *flag) const noexcept;
  int deletek(const char* k) noexcept;
  int get_valuelen(const char* k, int* vl, int *flag) const noexcept;
  int get_nkeys(int *nkeys) const noexcept;
  int get_nthkey(int n,char* k) const noexcept;
  void myfree() noexcept;
  void pup(PUP::er& p) noexcept;
};

class CProxy_ampi;
class CProxyElement_ampi;

//Virtual class describing a virtual topology: Cart, Graph, DistGraph
class ampiTopology {
 private:
  std::vector<int> v; // dummy variable for const& returns from virtual functions

 public:
  virtual ~ampiTopology() noexcept {};
  virtual void pup(PUP::er &p) noexcept =0;
  virtual int getType() const noexcept =0;
  virtual void dup(ampiTopology* topo) noexcept =0;
  virtual std::vector<int> &getnbors() noexcept =0;
  virtual const std::vector<int> &getnbors() const noexcept =0;
  virtual void setnbors(const std::vector<int> &nbors_) noexcept =0;
  virtual void sortnbors(CProxy_ampi arrProxy, std::vector<int> &nbors_) noexcept;

  virtual const std::vector<int> &getdims() const noexcept {CkAbort("AMPI: instance of invalid Virtual Topology class."); return v;}
  virtual const std::vector<int> &getperiods() const noexcept {CkAbort("AMPI: instance of invalid Virtual Topology class."); return v;}
  virtual int getndims() const noexcept {CkAbort("AMPI: instance of invalid Virtual Topology class."); return -1;}
  virtual void setdims(const std::vector<int> &dims_) noexcept {CkAbort("AMPI: instance of invalid Virtual Topology class.");}
  virtual void setperiods(const std::vector<int> &periods_) noexcept {CkAbort("AMPI: instance of invalid Virtual Topology class.");}
  virtual void setndims(int ndims_) noexcept {CkAbort("AMPI: instance of invalid Virtual Topology class.");}

  virtual int getnvertices() const noexcept {CkAbort("AMPI: instance of invalid Virtual Topology class."); return -1;}
  virtual const std::vector<int> &getindex() const noexcept {CkAbort("AMPI: instance of invalid Virtual Topology class."); return v;}
  virtual const std::vector<int> &getedges() const noexcept {CkAbort("AMPI: instance of invalid Virtual Topology class."); return v;}
  virtual void setnvertices(int nvertices_) noexcept {CkAbort("AMPI: instance of invalid Virtual Topology class.");}
  virtual void setindex(const std::vector<int> &index_) noexcept {CkAbort("AMPI: instance of invalid Virtual Topology class.");}
  virtual void setedges(const std::vector<int> &edges_) noexcept {CkAbort("AMPI: instance of invalid Virtual Topology class.");}

  virtual int getInDegree() const noexcept {CkAbort("AMPI: instance of invalid Virtual Topology class."); return -1;}
  virtual const std::vector<int> &getSources() const noexcept {CkAbort("AMPI: instance of invalid Virtual Topology class."); return v;}
  virtual const std::vector<int> &getSourceWeights() const noexcept {CkAbort("AMPI: instance of invalid Virtual Topology class."); return v;}
  virtual int getOutDegree() const noexcept {CkAbort("AMPI: instance of invalid Virtual Topology class."); return -1;}
  virtual const std::vector<int> &getDestinations() const noexcept {CkAbort("AMPI: instance of invalid Virtual Topology class."); return v;}
  virtual const std::vector<int> &getDestWeights() const noexcept {CkAbort("AMPI: instance of invalid Virtual Topology class."); return v;}
  virtual bool areSourcesWeighted() const noexcept {CkAbort("AMPI: instance of invalid Virtual Topology class."); return false;}
  virtual bool areDestsWeighted() const noexcept {CkAbort("AMPI: instance of invalid Virtual Topology class."); return false;}
  virtual void setAreSourcesWeighted(bool val) noexcept {CkAbort("AMPI: instance of invalid Virtual Topology class.");}
  virtual void setAreDestsWeighted(bool val) noexcept {CkAbort("AMPI: instance of invalid Virtual Topology class.");}
  virtual void setInDegree(int degree) noexcept {CkAbort("AMPI: instance of invalid Virtual Topology class.");}
  virtual void setSources(const std::vector<int> &sources) noexcept {CkAbort("AMPI: instance of invalid Virtual Topology class.");}
  virtual void setSourceWeights(const std::vector<int> &sourceWeights) noexcept {CkAbort("AMPI: instance of invalid Virtual Topology class.");}
  virtual void setOutDegree(int degree) noexcept {CkAbort("AMPI: instance of invalid Virtual Topology class.");}
  virtual void setDestinations(const std::vector<int> &destinations) noexcept {CkAbort("AMPI: instance of invalid Virtual Topology class.");}
  virtual void setDestWeights(const std::vector<int> &destWeights) noexcept {CkAbort("AMPI: instance of invalid Virtual Topology class.");}
};

class ampiCartTopology final : public ampiTopology {
 private:
  int ndims;
  std::vector<int> dims, periods, nbors;

 public:
  ampiCartTopology() noexcept : ndims(-1) {}

  void pup(PUP::er &p) noexcept {
    p|ndims;
    p|dims;
    p|periods;
    p|nbors;
  }

  inline int getType() const noexcept {return MPI_CART;}
  inline void dup(ampiTopology* topo) noexcept {
    CkAssert(topo->getType() == MPI_CART);
    setndims(topo->getndims());
    setdims(topo->getdims());
    setperiods(topo->getperiods());
    setnbors(topo->getnbors());
  }

  inline const std::vector<int> &getdims() const noexcept {return dims;}
  inline const std::vector<int> &getperiods() const noexcept {return periods;}
  inline int getndims() const noexcept {return ndims;}
  inline std::vector<int> &getnbors() noexcept {return nbors;}
  inline const std::vector<int> &getnbors() const noexcept {return nbors;}

  inline void setdims(const std::vector<int> &d) noexcept {dims = d; dims.shrink_to_fit();}
  inline void setperiods(const std::vector<int> &p) noexcept {periods = p; periods.shrink_to_fit();}
  inline void setndims(int nd) noexcept {ndims = nd;}
  inline void setnbors(const std::vector<int> &n) noexcept {nbors = n; nbors.shrink_to_fit();}
};

class ampiGraphTopology final : public ampiTopology {
 private:
  int nvertices;
  std::vector<int> index, edges, nbors;

 public:
  ampiGraphTopology() noexcept : nvertices(-1) {}

  void pup(PUP::er &p) noexcept {
    p|nvertices;
    p|index;
    p|edges;
    p|nbors;
  }

  inline int getType() const noexcept {return MPI_GRAPH;}
  inline void dup(ampiTopology* topo) noexcept {
    CkAssert(topo->getType() == MPI_GRAPH);
    setnvertices(topo->getnvertices());
    setindex(topo->getindex());
    setedges(topo->getedges());
    setnbors(topo->getnbors());
  }

  inline int getnvertices() const noexcept {return nvertices;}
  inline const std::vector<int> &getindex() const noexcept {return index;}
  inline const std::vector<int> &getedges() const noexcept {return edges;}
  inline std::vector<int> &getnbors() noexcept {return nbors;}
  inline const std::vector<int> &getnbors() const noexcept {return nbors;}

  inline void setnvertices(int nv) noexcept {nvertices = nv;}
  inline void setindex(const std::vector<int> &i) noexcept {index = i; index.shrink_to_fit();}
  inline void setedges(const std::vector<int> &e) noexcept {edges = e; edges.shrink_to_fit();}
  inline void setnbors(const std::vector<int> &n) noexcept {nbors = n; nbors.shrink_to_fit();}
};

class ampiDistGraphTopology final : public ampiTopology {
 private:
  int inDegree, outDegree;
  bool sourcesWeighted, destsWeighted;
  std::vector<int> sources, sourceWeights, destinations, destWeights, nbors;

 public:
  ampiDistGraphTopology() noexcept : inDegree(-1), outDegree(-1), sourcesWeighted(false), destsWeighted(false) {}

  void pup(PUP::er &p) noexcept {
    p|inDegree;
    p|outDegree;
    p|sourcesWeighted;
    p|destsWeighted;
    p|sources;
    p|sourceWeights;
    p|destinations;
    p|destWeights;
    p|nbors;
  }

  inline int getType() const noexcept {return MPI_DIST_GRAPH;}
  inline void dup(ampiTopology* topo) noexcept {
    CkAssert(topo->getType() == MPI_DIST_GRAPH);
    setAreSourcesWeighted(topo->areSourcesWeighted());
    setAreDestsWeighted(topo->areDestsWeighted());
    setInDegree(topo->getInDegree());
    setSources(topo->getSources());
    setSourceWeights(topo->getSourceWeights());
    setOutDegree(topo->getOutDegree());
    setDestinations(topo->getDestinations());
    setDestWeights(topo->getDestWeights());
    setnbors(topo->getnbors());
  }

  inline int getInDegree() const noexcept {return inDegree;}
  inline const std::vector<int> &getSources() const noexcept {return sources;}
  inline const std::vector<int> &getSourceWeights() const noexcept {return sourceWeights;}
  inline int getOutDegree() const noexcept {return outDegree;}
  inline const std::vector<int> &getDestinations() const noexcept {return destinations;}
  inline const std::vector<int> &getDestWeights() const noexcept {return destWeights;}
  inline bool areSourcesWeighted() const noexcept {return sourcesWeighted;}
  inline bool areDestsWeighted() const noexcept {return destsWeighted;}
  inline std::vector<int> &getnbors() noexcept {return nbors;}
  inline const std::vector<int> &getnbors() const noexcept {return nbors;}

  inline void setAreSourcesWeighted(bool v) noexcept {sourcesWeighted = v ? 1 : 0;}
  inline void setAreDestsWeighted(bool v) noexcept {destsWeighted = v ? 1 : 0;}
  inline void setInDegree(int d) noexcept {inDegree = d;}
  inline void setSources(const std::vector<int> &s) noexcept {sources = s; sources.shrink_to_fit();}
  inline void setSourceWeights(const std::vector<int> &sw) noexcept {sourceWeights = sw; sourceWeights.shrink_to_fit();}
  inline void setOutDegree(int d) noexcept {outDegree = d;}
  inline void setDestinations(const std::vector<int> &d) noexcept {destinations = d; destinations.shrink_to_fit();}
  inline void setDestWeights(const std::vector<int> &dw) noexcept {destWeights = dw; destWeights.shrink_to_fit();}
  inline void setnbors(const std::vector<int> &nbors_) noexcept {nbors = nbors_; nbors.shrink_to_fit();}
};

/* KeyValue class for attribute caching */
class KeyvalNode {
 public:
  MPI_Copy_function *copy_fn;
  MPI_Delete_function *delete_fn;
  void *extra_state;
  int refCount;

  KeyvalNode() : copy_fn(NULL), delete_fn(NULL), extra_state(NULL), refCount(1) { }
  KeyvalNode(MPI_Copy_function *cf, MPI_Delete_function *df, void* es) :
             copy_fn(cf), delete_fn(df), extra_state(es), refCount(1) { }
  void incRefCount() { refCount++; }
  int decRefCount() { CkAssert(refCount > 0); refCount--; return refCount; }
  void pup(PUP::er& p) {
    p((char *)copy_fn, sizeof(void *));
    p((char *)delete_fn, sizeof(void *));
    p((char *)extra_state, sizeof(void *));
    p|refCount;
  }
};

// Only store Group ranks explicitly when they can't be
// lazily and transiently created via std::iota()
class groupStruct {
 private:
  int sz; // -1 if ranks is valid, otherwise the size to pass to std::iota()
  std::vector<int> ranks;
  int refCount = 1;

 private:
  bool ranksIsIota() const noexcept {
    for (int i=0; i<ranks.size(); i++)
      if (ranks[i] != i)
        return false;
    return true;
  }

 public:
  groupStruct() noexcept : sz(0) {}
  groupStruct(int s) noexcept : sz(s) {}
  groupStruct(std::vector<int> r) noexcept : sz(-1), ranks(std::move(r)) {
    if (ranksIsIota()) {
      sz = ranks.size();
      ranks.clear();
    }
    ranks.shrink_to_fit();
  }
  groupStruct &operator=(const groupStruct &obj) noexcept {
    sz = obj.sz;
    ranks = obj.ranks;
    return *this;
  }
  ~groupStruct() = default;
  void pup(PUP::er& p) noexcept {
    p|sz;
    p|ranks;
    p|refCount;
  }
  bool isIota() const noexcept {return (sz != -1);}
  int operator[](int i) const noexcept {return (isIota()) ? i : ranks[i];}
  int size() const noexcept {return (isIota()) ? sz : ranks.size();}
  std::vector<int> getRanks() const noexcept {
    if (isIota()) {
      // Lazily create ranks:
      std::vector<int> tmpRanks(sz);
      std::iota(tmpRanks.begin(), tmpRanks.end(), 0);
      tmpRanks.shrink_to_fit();
      return tmpRanks;
    }
    else {
      return ranks;
    }
  }
  int decRefCount() noexcept { return refCount--; }
  int incRefCount() noexcept { return refCount++; }
};

inline bool operator==(const groupStruct &g1, const groupStruct &g2) {
  if (g1.size() != g2.size() || g1.isIota() != g2.isIota()) {
    return false;
  }
  for (int i=0; i<g1.size(); i++) {
    if (g1[i] != g2[i]) {
      return false;
    }
  }
  return true;
}

inline bool operator!=(const groupStruct &g1, const groupStruct &g2) {
  return !(g1 == g2);
}

enum AmpiCommType : uint8_t {
   COMM_WORLD      = 0
  ,COMM_INTRA      = 1
  ,COMM_INTER      = 2
  ,COMM_SPLIT      = 3
  ,COMM_GROUP      = 4
  ,COMM_CART       = 5
  ,COMM_GRAPH      = 6
  ,COMM_DIST_GRAPH = 7
};

//Describes an AMPI communicator
class ampiCommStruct {
 private:
  MPI_Comm comm; //Communicator
  CkArrayID ampiID; //ID of corresponding ampi array
  int size; //Number of processes in communicator
  AmpiCommType commType; //COMM_WORLD, intracomm, intercomm, etc?

  groupStruct indices;  //indices[r] gives the array index for rank r
  groupStruct remoteIndices;  // remote group for inter-communicator

  ampiTopology *ampiTopo; // Virtual topology
  int topoType; // Type of virtual topology: MPI_CART, MPI_GRAPH, MPI_DIST_GRAPH, or MPI_UNDEFINED

  // For communicator attributes (MPI_*_get_attr): indexed by keyval
  std::unordered_map<int, uintptr_t> attributes;

  // For communicator names
  std::string commName;

 public:
  ampiCommStruct(int ignored=0) noexcept
    : size(-1), commType(COMM_INTRA), ampiTopo(NULL), topoType(MPI_UNDEFINED)
  {}
  ampiCommStruct(MPI_Comm comm_,const CkArrayID &id_,int size_) noexcept
    : comm(comm_), ampiID(id_),size(size_), commType(COMM_WORLD), indices(size_),
      ampiTopo(NULL), topoType(MPI_UNDEFINED)
  {}
  ampiCommStruct(MPI_Comm comm_,const CkArrayID &id_, const std::vector<int> &indices_, AmpiCommType type_) noexcept
    : comm(comm_), ampiID(id_), size(indices_.size()), commType(type_), indices(indices_),
      ampiTopo(NULL), topoType(MPI_UNDEFINED)
  {
    switch (commType) {
      case COMM_CART:
        topoType = MPI_CART;
        ampiTopo = new ampiCartTopology();
        break;
      case COMM_GRAPH:
        topoType = MPI_GRAPH;
        ampiTopo = new ampiGraphTopology();
        break;
      case COMM_DIST_GRAPH:
        topoType = MPI_DIST_GRAPH;
        ampiTopo = new ampiDistGraphTopology();
        break;
      default:
        topoType = MPI_UNDEFINED;
        ampiTopo = NULL;
        break;
    }
  }
  ampiCommStruct(MPI_Comm comm_, const CkArrayID &id_, const std::vector<int> &indices_,
                 const std::vector<int> &remoteIndices_) noexcept
    : comm(comm_), ampiID(id_), size(indices_.size()), commType(COMM_INTER), indices(indices_),
      remoteIndices(remoteIndices_), ampiTopo(NULL), topoType(MPI_UNDEFINED)
  {}

  ~ampiCommStruct() noexcept {
    if (ampiTopo != NULL)
      delete ampiTopo;
  }

  ampiCommStruct(const ampiCommStruct &obj) noexcept {
    switch (obj.topoType) {
      case MPI_CART:
        ampiTopo = new ampiCartTopology();
        break;
      case MPI_GRAPH:
        ampiTopo = new ampiGraphTopology();
        break;
      case MPI_DIST_GRAPH:
        ampiTopo = new ampiDistGraphTopology();
        break;
      default:
        ampiTopo = NULL;
        break;
    }
    commType       = obj.commType;
    topoType       = obj.topoType;
    comm           = obj.comm;
    ampiID         = obj.ampiID;
    size           = obj.size;
    commType       = obj.commType;
    indices        = obj.indices;
    remoteIndices  = obj.remoteIndices;
    attributes     = obj.attributes;
    commName       = obj.commName;
  }

  ampiCommStruct &operator=(const ampiCommStruct &obj) noexcept {
    if (this == &obj) {
      return *this;
    }
    switch (obj.topoType) {
      case MPI_CART:
        ampiTopo = new ampiCartTopology(*(static_cast<ampiCartTopology*>(obj.ampiTopo)));
        break;
      case MPI_GRAPH:
        ampiTopo = new ampiGraphTopology(*(static_cast<ampiGraphTopology*>(obj.ampiTopo)));
        break;
      case MPI_DIST_GRAPH:
        ampiTopo = new ampiDistGraphTopology(*(static_cast<ampiDistGraphTopology*>(obj.ampiTopo)));
        break;
      default:
        ampiTopo = NULL;
        break;
    }
    commType       = obj.commType;
    topoType       = obj.topoType;
    comm           = obj.comm;
    ampiID         = obj.ampiID;
    size           = obj.size;
    commType       = obj.commType;
    indices        = obj.indices;
    remoteIndices  = obj.remoteIndices;
    attributes     = obj.attributes;
    commName       = obj.commName;
    return *this;
  }

  const ampiTopology* getTopology() const noexcept { return ampiTopo; }
  ampiTopology* getTopology() noexcept { return ampiTopo; }

  inline bool isinter() const noexcept {return commType==COMM_INTER;}
  inline AmpiCommType getType() const noexcept {return commType;}
  void setArrayID(const CkArrayID &nID) noexcept {ampiID=nID;}

  MPI_Comm getComm() const noexcept {return comm;}
  inline std::vector<int> getIndices() const noexcept {return indices.getRanks();}
  inline std::vector<int> getRemoteIndices() const noexcept {return remoteIndices.getRanks();}
  std::unordered_map<int, uintptr_t> & getAttributes() noexcept {return attributes;}

  void setName(const char *src) noexcept {
    CkDDT_SetName(commName, src);
  }

  void getName(char *name, int *len) const noexcept {
    int length = *len = commName.size();
    memcpy(name, commName.data(), length);
    name[length] = '\0';
  }

  //Get the proxy for the entire array
  CProxy_ampi getProxy() const noexcept;

  //Get the array index for rank r in this communicator
  int getIndexForRank(int r) const noexcept {
#if CMK_ERROR_CHECKING
    if (r>=size) CkAbort("AMPI> You passed in an out-of-bounds process rank!");
#endif
    return indices[r];
  }
  int getIndexForRemoteRank(int r) const noexcept {
#if CMK_ERROR_CHECKING
    if (r>=remoteIndices.size()) CkAbort("AMPI> You passed in an out-of-bounds intercomm remote process rank!");
#endif
    return remoteIndices[r];
  }
  //Get the rank for this array index (Warning: linear time)
  int getRankForIndex(int i) const noexcept {
    if (indices.isIota()) return i;
    else {
      const std::vector<int>& ind = indices.getRanks();
      for (int r=0;r<ind.size();r++)
        if (ind[r]==i) return r;
      return -1; /*That index isn't in this communicator*/
    }
  }

  int getSize() const noexcept {return size;}

  void pup(PUP::er &p) noexcept {
    p|comm;
    p|ampiID;
    p|size;
    p|commType;
    p|indices;
    p|remoteIndices;
    p|attributes;
    p|commName;
    p|topoType;
    if (topoType != MPI_UNDEFINED) {
      if (p.isUnpacking()) {
        switch (topoType) {
          case MPI_CART:
            ampiTopo = new ampiCartTopology();
            break;
          case MPI_GRAPH:
            ampiTopo = new ampiGraphTopology();
            break;
          case MPI_DIST_GRAPH:
            ampiTopo = new ampiDistGraphTopology();
            break;
          default:
            CkAbort("AMPI> Communicator has an invalid topology!");
            break;
        }
      }
      ampiTopo->pup(p);
    } else {
      ampiTopo = NULL;
    }
    if (p.isDeleting()) {
      delete ampiTopo; ampiTopo = NULL;
    }
  }
};

// group operations
inline void outputOp(const std::vector<int>& vec) noexcept {
  if (vec.size() > 50) {
    CkPrintf("vector too large to output!\n");
    return;
  }
  CkPrintf("output vector: size=%zu {",vec.size());
  for (int i=0; i<vec.size(); i++) {
    CkPrintf(" %d ", vec[i]);
  }
  CkPrintf("}\n");
}

inline int getPosOp(int idx, const std::vector<int>& vec) noexcept {
  for (int r=0; r<vec.size(); r++) {
    if (vec[r] == idx) {
      return r;
    }
  }
  return MPI_UNDEFINED;
}

inline std::vector<int> unionOp(const std::vector<int>& vec1, const std::vector<int>& vec2) noexcept {
  std::vector<int> newvec(vec1);
  for (int i=0; i<vec2.size(); i++) {
    if (getPosOp(vec2[i], vec1) == MPI_UNDEFINED) {
      newvec.push_back(vec2[i]);
    }
  }
  return newvec;
}

inline std::vector<int> intersectOp(const std::vector<int>& vec1, const std::vector<int>& vec2) noexcept {
  std::vector<int> newvec;
  for (int i=0; i<vec1.size(); i++) {
    if (getPosOp(vec1[i], vec2) != MPI_UNDEFINED) {
      newvec.push_back(vec1[i]);
    }
  }
  return newvec;
}

inline std::vector<int> diffOp(const std::vector<int>& vec1, const std::vector<int>& vec2) noexcept {
  std::vector<int> newvec;
  for (int i=0; i<vec1.size(); i++) {
    if (getPosOp(vec1[i], vec2) == MPI_UNDEFINED) {
      newvec.push_back(vec1[i]);
    }
  }
  return newvec;
}

inline int* translateRanksOp(int n, const std::vector<int>& vec1, const int* ranks1,
                             const std::vector<int>& vec2, int *ret) noexcept {
  for (int i=0; i<n; i++) {
    ret[i] = (ranks1[i] == MPI_PROC_NULL) ? MPI_PROC_NULL : getPosOp(vec1[ranks1[i]], vec2);
  }
  return ret;
}

inline int compareVecOp(const std::vector<int>& vec1, const std::vector<int>& vec2) noexcept {
  int pos, ret = MPI_IDENT;
  if (vec1.size() != vec2.size()) {
    return MPI_UNEQUAL;
  }
  for (int i=0; i<vec1.size(); i++) {
    pos = getPosOp(vec1[i], vec2);
    if (pos == MPI_UNDEFINED) {
      return MPI_UNEQUAL;
    }
    else if (pos != i) {
      ret = MPI_SIMILAR;
    }
  }
  return ret;
}

inline std::vector<int> inclOp(int n, const int* ranks, const std::vector<int>& vec) noexcept {
  std::vector<int> retvec(n);
  for (int i=0; i<n; i++) {
    retvec[i] = vec[ranks[i]];
  }
  return retvec;
}

inline std::vector<int> exclOp(int n, const int* ranks, const std::vector<int>& vec) noexcept {
  std::vector<int> retvec;
  bool add = true;
  for (int j=0; j<vec.size(); j++) {
    for (int i=0; i<n; i++) {
      if (j == ranks[i]) {
        add = false;
        break;
      }
    }
    if (add) {
      retvec.push_back(vec[j]);
    }
    else {
      add = true;
    }
  }
  return retvec;
}

inline std::vector<int> rangeInclOp(int n, int ranges[][3], const std::vector<int>& vec,
                               int *flag) noexcept {
  std::vector<int> retvec;
  int first, last, stride;
  for (int i=0; i<n; i++) {
    first  = ranges[i][0];
    last   = ranges[i][1];
    stride = ranges[i][2];
    if (stride != 0) {
      for (int j=0; j<=(last-first)/stride; j++) {
        retvec.push_back(vec[first+stride*j]);
      }
    }
    else {
      *flag = MPI_ERR_ARG;
      return std::vector<int>();
    }
  }
  *flag = MPI_SUCCESS;
  return retvec;
}

inline std::vector<int> rangeExclOp(int n, int ranges[][3], const std::vector<int>& vec,
                               int *flag) noexcept {
  std::vector<int> ranks;
  int first, last, stride;
  for (int i=0; i<n; i++) {
    first  = ranges[i][0];
    last   = ranges[i][1];
    stride = ranges[i][2];
    if (stride != 0) {
      for (int j=0; j<=(last-first)/stride; j++) {
        ranks.push_back(first+stride*j);
      }
    }
    else {
      *flag = MPI_ERR_ARG;
      return std::vector<int>();
    }
  }
  *flag = MPI_SUCCESS;
  return exclOp(ranks.size(), &ranks[0], vec);
}

#include "ampi.decl.h"
#include "charm-api.h"
#include <sys/stat.h> // for mkdir

//MPI_ANY_TAG is defined in ampi.h to MPI_TAG_UB_VALUE+1
#define MPI_ATA_SEQ_TAG     MPI_TAG_UB_VALUE+2
#define MPI_BCAST_TAG       MPI_TAG_UB_VALUE+3
#define MPI_REDN_TAG        MPI_TAG_UB_VALUE+4
#define MPI_SCATTER_TAG     MPI_TAG_UB_VALUE+5
#define MPI_SCAN_TAG        MPI_TAG_UB_VALUE+6
#define MPI_EXSCAN_TAG      MPI_TAG_UB_VALUE+7
#define MPI_ATA_TAG         MPI_TAG_UB_VALUE+8
#define MPI_NBOR_TAG        MPI_TAG_UB_VALUE+9
#define MPI_RMA_TAG         MPI_TAG_UB_VALUE+10
#define MPI_EPOCH_START_TAG MPI_TAG_UB_VALUE+11
#define MPI_EPOCH_END_TAG   MPI_TAG_UB_VALUE+12

#define AMPI_COLL_SOURCE 0
#define AMPI_COLL_COMM   MPI_COMM_WORLD

enum AmpiReqType : uint8_t {
  AMPI_INVALID_REQ = 0,
  AMPI_I_REQ       = 1,
  AMPI_ATA_REQ     = 2,
  AMPI_SEND_REQ    = 3,
  AMPI_SSEND_REQ   = 4,
  AMPI_REDN_REQ    = 5,
  AMPI_GATHER_REQ  = 6,
  AMPI_GATHERV_REQ = 7,
  AMPI_G_REQ       = 8,
#if CMK_CUDA
  AMPI_GPU_REQ     = 9
#endif
};

enum AmpiReqSts : char {
  AMPI_REQ_PENDING   = 0,
  AMPI_REQ_BLOCKED   = 1,
  AMPI_REQ_COMPLETED = 2
};

enum AmpiSendType : char {
  BLOCKING_SEND = 0,
  I_SEND = 1,
  BLOCKING_SSEND = 2,
  I_SSEND = 3
};

#define MyAlign8(x) (((x)+7)&(~7))

/**
Represents an MPI request that has been initiated
using Isend, Irecv, Ialltoall, Send_init, etc.
*/
class AmpiRequest {
 public:
  void *buf          = nullptr;
  int count          = 0;
  MPI_Datatype type  = MPI_DATATYPE_NULL;
  int tag            = MPI_ANY_TAG; // the order must match MPI_Status
  int src            = MPI_ANY_SOURCE;
  MPI_Comm comm      = MPI_COMM_NULL;
  MPI_Request reqIdx = MPI_REQUEST_NULL;
  bool complete      = false;
  bool blocked       = false; // this req is currently blocked on


 public:
  AmpiRequest() =default;
  virtual ~AmpiRequest() =default;

  /// Activate this persistent request.
  ///  Only meaningful for persistent Ireq, SendReq, and SsendReq requests.
  virtual void start(MPI_Request reqIdx) noexcept {}

  /// Used by AmmEntry's constructor
  virtual int getTag() const noexcept { return tag; }
  virtual int getSrcRank() const noexcept { return src; }

  /// Return true if this request is finished (progress):
  virtual bool test(MPI_Status *sts=MPI_STATUS_IGNORE) noexcept =0;

  /// Block until this request is finished,
  ///  returning a valid MPI error code.
  virtual CMI_WARN_UNUSED_RESULT ampiParent* wait(ampiParent* parent, MPI_Status *sts) noexcept =0;

  /// Mark this request for cancellation.
  /// Supported only for IReq requests
  virtual void cancel() noexcept {}

  /// Mark this request persistent.
  /// Supported only for IReq, SendReq, and SsendReq requests
  virtual void setPersistent(bool p) noexcept {}
  virtual bool isPersistent() const noexcept { return false; }

  /// Deregister memory from a get/put
  virtual void deregisterMem(CkNcpyBuffer* info) { }

  /// Set an intermediate/system buffer that a message will be serialized thru
  virtual void setSystemBuf(void* buf_, int len=0) { }

  /// Receive an AmpiMsg
  /// Returns true if the msg payload is recv'ed, otherwise return false
  /// (if the msg is a sync msg, it can't be recv'ed until the caller
  /// acks the sender to get the real payload)
  virtual bool receive(ampi *ptr, AmpiMsg *msg, bool deleteMsg=true) noexcept =0;

  /// Receive a CkReductionMsg
  virtual void receive(ampi *ptr, CkReductionMsg *msg) noexcept =0;

  /// Receive an Rdma message
  virtual void receiveRdma(ampi *ptr, char *sbuf, int slength, int srcRank) noexcept { }

  /// Set the request's index into AmpiRequestList
  void setReqIdx(MPI_Request idx) noexcept { reqIdx = idx; }
  MPI_Request getReqIdx() const noexcept { return reqIdx; }

  /// Free the request's datatype
  void free(CkDDT* ddt) noexcept {
    if (type != MPI_DATATYPE_NULL) ddt->freeType(type);
  }

  /// Set whether the request is currently blocked on
  void setBlocked(bool b) noexcept { blocked = b; }
  bool isBlocked() const noexcept { return blocked; }

  /// Returns the type of request:
  ///  AMPI_I_REQ, AMPI_ATA_REQ, AMPI_SEND_REQ, AMPI_SSEND_REQ,
  ///  AMPI_REDN_REQ, AMPI_GATHER_REQ, AMPI_GATHERV_REQ, AMPI_G_REQ
  virtual AmpiReqType getType() const noexcept =0;

  /// Returns whether this request will need to be matched.
  /// It is used to determine whether this request should be inserted into postedReqs.
  /// AMPI_SEND_REQ, AMPI_SSEND_REQ, and AMPI_ATA_REQ should not be posted.
  virtual bool isUnmatched() const noexcept =0;

  /// Returns whether this type is pooled or not:
  /// Only AMPI_I_REQ, AMPI_SEND_REQ, and AMPI_SSEND_REQs are pooled.
  virtual bool isPooledType() const noexcept { return false; }

  /// Return the actual number of bytes that were received.
  virtual int getNumReceivedBytes(CkDDT *ddt) const noexcept {
    // by default, return number of bytes requested
    return count * ddt->getSize(type);
  }

  virtual void pup(PUP::er &p) noexcept {
    p((char *)&buf, sizeof(void *)); //supposed to work only with Isomalloc
    p(count);
    p(type);
    p(tag);
    p(src);
    p(comm);
    p(reqIdx);
    p(complete);
    p(blocked);
  }

  virtual void print() const noexcept =0;
};

// This is used in the constructors of the AmpiRequest types below,
// assuming arguments: (MPI_Datatype type_, CkDDT* ddt_, AmpiReqSts sts_)
#define AMPI_REQUEST_COMMON_INIT           \
{                                          \
  complete = (sts_ == AMPI_REQ_COMPLETED); \
  blocked  = (sts_ == AMPI_REQ_BLOCKED);   \
  if (type_ != MPI_DATATYPE_NULL) {        \
    ddt_->getType(type_)->incRefCount();   \
  }                                        \
}

class IReq final : public AmpiRequest {
 public:
  bool cancelled   = false; // track if request is cancelled
  bool persistent  = false; // Is this a persistent recv request?
  int length       = 0; // recv'ed length in bytes
  char* systemBuf  = nullptr; // non-NULL for non-contiguous recv datatypes
  int systemBufLen = 0; // length in bytes of systemBuf

  IReq(void *buf_, int count_, MPI_Datatype type_, int src_, int tag_,
       MPI_Comm comm_, CkDDT *ddt_, AmpiReqSts sts_=AMPI_REQ_PENDING) noexcept
  {
    buf   = buf_;
    count = count_;
    type  = type_;
    src   = src_;
    tag   = tag_;
    comm  = comm_;
    AMPI_REQUEST_COMMON_INIT
  }
  IReq() =default;
  ~IReq() =default;
  bool test(MPI_Status *sts=MPI_STATUS_IGNORE) noexcept override;
  CMI_WARN_UNUSED_RESULT ampiParent* wait(ampiParent* parent, MPI_Status *sts) noexcept override;
  void cancel() noexcept override { if (!complete) cancelled = true; }
  AmpiReqType getType() const noexcept override { return AMPI_I_REQ; }
  bool isUnmatched() const noexcept override { return !complete; }
  bool isPooledType() const noexcept override { return true; }
  void setPersistent(bool p) noexcept override { persistent = p; }
  bool isPersistent() const noexcept override { return persistent; }
  void start(MPI_Request reqIdx) noexcept override;
  bool receive(ampi *ptr, AmpiMsg *msg, bool deleteMsg=true) noexcept override;
  void receive(ampi *ptr, CkReductionMsg *msg) noexcept override {}
  void receiveRdma(ampi *ptr, char *sbuf, int slength, int srcRank) noexcept override;
  int getNumReceivedBytes(CkDDT *ptr) const noexcept override {
    return length;
  }
  void setSystemBuf(void* buf_, int len=0) noexcept override {
    systemBuf = (char*)buf_;
    systemBufLen = len;
  }
  void deregisterMem(CkNcpyBuffer *targetInfo) noexcept override {
    if (targetInfo) {
      targetInfo->deregisterMem();
      // targetInfo is owned by the CkDataMsg and so is freed with it
    }
    if (systemBuf) {
      delete [] systemBuf;
    }
  }
  void pup(PUP::er &p) noexcept override {
    AmpiRequest::pup(p);
    p|cancelled;
    p|persistent;
    p|length;
    p|systemBufLen;
    p((char *)&systemBuf, sizeof(char *));
  }
  void print() const noexcept override;
};

class RednReq final : public AmpiRequest {
 public:
  MPI_Op op = MPI_OP_NULL;

  RednReq(void *buf_, int count_, MPI_Datatype type_, MPI_Comm comm_,
          MPI_Op op_, CkDDT* ddt_, AmpiReqSts sts_=AMPI_REQ_PENDING) noexcept
  {
    buf   = buf_;
    count = count_;
    type  = type_;
    src   = AMPI_COLL_SOURCE;
    tag   = MPI_REDN_TAG;
    comm  = comm_;
    op    = op_;
    AMPI_REQUEST_COMMON_INIT
  }
  RednReq() =default;
  ~RednReq() =default;
  bool test(MPI_Status *sts=MPI_STATUS_IGNORE) noexcept override;
  CMI_WARN_UNUSED_RESULT ampiParent* wait(ampiParent* parent, MPI_Status *sts) noexcept override;
  void cancel() noexcept override {}
  AmpiReqType getType() const noexcept override { return AMPI_REDN_REQ; }
  bool isUnmatched() const noexcept override { return !complete; }
  bool receive(ampi *ptr, AmpiMsg *msg, bool deleteMsg=true) noexcept override { return true; }
  void receive(ampi *ptr, CkReductionMsg *msg) noexcept override;
  void pup(PUP::er &p) noexcept override {
    AmpiRequest::pup(p);
    p|op;
  }
  void print() const noexcept override;
};

class GatherReq final : public AmpiRequest {
 public:
  GatherReq(void *buf_, int count_, MPI_Datatype type_, MPI_Comm comm_,
            CkDDT *ddt_, AmpiReqSts sts_=AMPI_REQ_PENDING) noexcept
  {
    buf   = buf_;
    count = count_;
    type  = type_;
    src   = AMPI_COLL_SOURCE;
    tag   = MPI_REDN_TAG;
    comm  = comm_;
    AMPI_REQUEST_COMMON_INIT
  }
  GatherReq() =default;
  ~GatherReq() =default;
  bool test(MPI_Status *sts=MPI_STATUS_IGNORE) noexcept override;
  CMI_WARN_UNUSED_RESULT ampiParent* wait(ampiParent* parent, MPI_Status *sts) noexcept override;
  void cancel() noexcept override {}
  AmpiReqType getType() const noexcept override { return AMPI_GATHER_REQ; }
  bool isUnmatched() const noexcept override { return !complete; }
  bool receive(ampi *ptr, AmpiMsg *msg, bool deleteMsg=true) noexcept override { return true; }
  void receive(ampi *ptr, CkReductionMsg *msg) noexcept override;
  void pup(PUP::er &p) noexcept override {
    AmpiRequest::pup(p);
  }
  void print() const noexcept override;
};

class GathervReq final : public AmpiRequest {
 public:
  std::vector<int> recvCounts;
  std::vector<int> displs;

  GathervReq(void *buf_, int count_, MPI_Datatype type_, MPI_Comm comm_, const int *rc,
             const int *d, CkDDT* ddt_, AmpiReqSts sts_=AMPI_REQ_PENDING) noexcept
  {
    buf   = buf_;
    count = count_;
    type  = type_;
    src   = AMPI_COLL_SOURCE;
    tag   = MPI_REDN_TAG;
    comm  = comm_;
    recvCounts.assign(rc, rc+count);
    displs.assign(d, d+count);
    AMPI_REQUEST_COMMON_INIT
  }
  GathervReq() =default;
  ~GathervReq() =default;
  bool test(MPI_Status *sts=MPI_STATUS_IGNORE) noexcept override;
  CMI_WARN_UNUSED_RESULT ampiParent*  wait(ampiParent* parent, MPI_Status *sts) noexcept override;
  AmpiReqType getType() const noexcept override { return AMPI_GATHERV_REQ; }
  bool isUnmatched() const noexcept override { return !complete; }
  bool receive(ampi *ptr, AmpiMsg *msg, bool deleteMsg=true) noexcept override { return true; }
  void receive(ampi *ptr, CkReductionMsg *msg) noexcept override;
  void pup(PUP::er &p) noexcept override {
    AmpiRequest::pup(p);
    p|recvCounts;
    p|displs;
  }
  void print() const noexcept override;
};

class SendReq final : public AmpiRequest {
  bool persistent = false; // is this a persistent send request?

 public:
  SendReq(MPI_Datatype type_, MPI_Comm comm_, CkDDT* ddt_, AmpiReqSts sts_=AMPI_REQ_PENDING) noexcept
  {
    type = type_;
    comm = comm_;
    AMPI_REQUEST_COMMON_INIT
  }
  SendReq(void* buf_, int count_, MPI_Datatype type_, int dest_, int tag_,
          MPI_Comm comm_, CkDDT* ddt_, AmpiReqSts sts_=AMPI_REQ_PENDING) noexcept
  {
    buf   = buf_;
    count = count_;
    type  = type_;
    src   = dest_;
    tag   = tag_;
    comm  = comm_;
    AMPI_REQUEST_COMMON_INIT
  }
  SendReq() noexcept {}
  ~SendReq() noexcept {}
  bool test(MPI_Status *sts=MPI_STATUS_IGNORE) noexcept override;
  CMI_WARN_UNUSED_RESULT ampiParent* wait(ampiParent* parent, MPI_Status *sts) noexcept override;
  void setPersistent(bool p) noexcept override { persistent = p; }
  bool isPersistent() const noexcept override { return persistent; }
  void start(MPI_Request reqIdx) noexcept override;
  bool receive(ampi *ptr, AmpiMsg *msg, bool deleteMsg=true) noexcept override { return true; }
  void receive(ampi *ptr, CkReductionMsg *msg) noexcept override {}
  AmpiReqType getType() const noexcept override { return AMPI_SEND_REQ; }
  bool isUnmatched() const noexcept override { return false; }
  bool isPooledType() const noexcept override { return true; }
  void pup(PUP::er &p) noexcept override {
    AmpiRequest::pup(p);
    p|persistent;
  }
  void print() const noexcept override;
};

class SsendReq final : public AmpiRequest {
 private:
  bool systemBuf = false; // is 'buf' an intermediate/system buffer?
  bool persistent = false; // is this a persistent Ssend request?
 public:
  int destRank = MPI_PROC_NULL;

 public:
  SsendReq(MPI_Datatype type_, MPI_Comm comm_, CkDDT* ddt_, AmpiReqSts sts_=AMPI_REQ_PENDING) noexcept
  {
    type = type_;
    comm = comm_;
    AMPI_REQUEST_COMMON_INIT
  }
  SsendReq(void* buf_, int count_, MPI_Datatype type_, int dest_, int tag_, MPI_Comm comm_,
           CkDDT* ddt_, AmpiReqSts sts_=AMPI_REQ_PENDING) noexcept
  {
    buf      = (void*)buf_;
    count    = count_;
    type     = type_;
    tag      = tag_;
    comm     = comm_;
    destRank = dest_;
    AMPI_REQUEST_COMMON_INIT
  }
  SsendReq(void* buf_, int count_, MPI_Datatype type_, int dest_, int tag_, MPI_Comm comm_,
           int src_, CkDDT* ddt_, AmpiReqSts sts_=AMPI_REQ_PENDING) noexcept
  {
    buf      = (void*)buf_;
    count    = count_;
    type     = type_;
    src      = src_;
    tag      = tag_;
    comm     = comm_;
    destRank = dest_;
    AMPI_REQUEST_COMMON_INIT
  }
  SsendReq() =default;
  ~SsendReq() =default;
  bool test(MPI_Status *sts=MPI_STATUS_IGNORE) noexcept override;
  CMI_WARN_UNUSED_RESULT ampiParent* wait(ampiParent* parent, MPI_Status *sts) noexcept override;
  void setPersistent(bool p) noexcept override { persistent = p; }
  bool isPersistent() const noexcept override { return persistent; }
  void start(MPI_Request reqIdx) noexcept override;
  bool receive(ampi *ptr, AmpiMsg *msg, bool deleteMsg=true) noexcept override { return true; }
  void receive(ampi *ptr, CkReductionMsg *msg) noexcept override {}
  AmpiReqType getType() const noexcept override { return AMPI_SSEND_REQ; }
  bool isUnmatched() const noexcept override { return false; }
  bool isPooledType() const noexcept override { return true; }
  void deregisterMem(CkNcpyBuffer *srcInfo) noexcept override {
    if (srcInfo) {
      srcInfo->deregisterMem();
      // srcInfo is owned by the CkDataMsg and so is freed with it
    }
    if (systemBuf) {
      delete [] (char*)buf;
    }
  }
  void setSystemBuf(void* buf_, int len=0) noexcept override {
    systemBuf = true;
    buf = buf_;
  }
  void pup(PUP::er &p) noexcept override {
    AmpiRequest::pup(p);
    p|persistent;
    p|destRank;
    p|systemBuf;
  }
  void print() const noexcept override;
};

#if CMK_CUDA
class GPUReq : public AmpiRequest {
 public:
  GPUReq() noexcept;
  ~GPUReq() =default;
  bool test(MPI_Status *sts=MPI_STATUS_IGNORE) noexcept override;
  CMI_WARN_UNUSED_RESULT ampiParent* wait(ampiParent* parent, MPI_Status *sts) noexcept override;
  bool receive(ampi *ptr, AmpiMsg *msg, bool deleteMsg=true) noexcept override;
  void receive(ampi *ptr, CkReductionMsg *msg) noexcept override;
  AmpiReqType getType() const noexcept override { return AMPI_GPU_REQ; }
  bool isUnmatched() const noexcept override { return false; }
  void setComplete() noexcept;
  void print() const noexcept override;
};
#endif

class ATAReq final : public AmpiRequest {
 public:
  std::vector<MPI_Request> reqs;

  ATAReq(int numReqs_) noexcept : reqs(numReqs_) {}
  ATAReq() =default;
  ~ATAReq() =default;
  bool test(MPI_Status *sts=MPI_STATUS_IGNORE) noexcept override;
  CMI_WARN_UNUSED_RESULT ampiParent* wait(ampiParent* parent, MPI_Status *sts) noexcept override;
  bool receive(ampi *ptr, AmpiMsg *msg, bool deleteMsg=true) noexcept override { return true; }
  void receive(ampi *ptr, CkReductionMsg *msg) noexcept override {}
  int getCount() const noexcept { return reqs.size(); }
  AmpiReqType getType() const noexcept override { return AMPI_ATA_REQ; }
  bool isUnmatched() const noexcept override { return false; }
  void pup(PUP::er &p) noexcept override {
    AmpiRequest::pup(p);
    p|reqs;
  }
  void print() const noexcept override;
};

class GReq final : public AmpiRequest {
 private:
  MPI_Grequest_query_function* queryFn;
  MPI_Grequest_free_function* freeFn;
  MPI_Grequest_cancel_function* cancelFn;
  MPIX_Grequest_poll_function* pollFn;
  MPIX_Grequest_wait_function* waitFn;
  void* extraState;

 public:
  GReq(MPI_Grequest_query_function* q, MPI_Grequest_free_function* f, MPI_Grequest_cancel_function* c, void* es) noexcept
    : queryFn(q), freeFn(f), cancelFn(c), pollFn(nullptr), waitFn(nullptr), extraState(es) {}
  GReq(MPI_Grequest_query_function *q, MPI_Grequest_free_function* f, MPI_Grequest_cancel_function* c, MPIX_Grequest_poll_function* p, void* es) noexcept
    : queryFn(q), freeFn(f), cancelFn(c), pollFn(p), waitFn(nullptr), extraState(es) {}
  GReq(MPI_Grequest_query_function *q, MPI_Grequest_free_function* f, MPI_Grequest_cancel_function* c, MPIX_Grequest_poll_function* p, MPIX_Grequest_wait_function* w, void* es) noexcept
    : queryFn(q), freeFn(f), cancelFn(c), pollFn(p), waitFn(w), extraState(es) {}
  GReq() =default;
  ~GReq() noexcept { (*freeFn)(extraState); }
  bool test(MPI_Status *sts=MPI_STATUS_IGNORE) noexcept override;
  CMI_WARN_UNUSED_RESULT ampiParent* wait(ampiParent* parent, MPI_Status *sts) noexcept override;
  bool receive(ampi *ptr, AmpiMsg *msg, bool deleteMsg=true) noexcept override { return true; }
  void receive(ampi *ptr, CkReductionMsg *msg) noexcept override {}
  void cancel() noexcept override { (*cancelFn)(extraState, complete); }
  AmpiReqType getType() const noexcept override { return AMPI_G_REQ; }
  bool isUnmatched() const noexcept override { return false; }
  void pup(PUP::er &p) noexcept override {
    AmpiRequest::pup(p);
    p((char *)queryFn, sizeof(void *));
    p((char *)freeFn, sizeof(void *));
    p((char *)cancelFn, sizeof(void *));
    p((char *)pollFn, sizeof(void *));
    p((char *)waitFn, sizeof(void *));
    p((char *)extraState, sizeof(void *));
  }
  void print() const noexcept override;
};

class AmpiRequestPool;

class AmpiRequestList {
 private:
  std::vector<AmpiRequest*> reqs; // indexed by MPI_Request
  int startIdx; // start next search from this index
  AmpiRequestPool* reqPool;
 public:
  AmpiRequestList() noexcept : startIdx(0) {}
  AmpiRequestList(int size, AmpiRequestPool* reqPoolPtr) noexcept
    : reqs(size), startIdx(0), reqPool(reqPoolPtr) {}
  ~AmpiRequestList() noexcept {}

  inline AmpiRequest* operator[](int n) noexcept {
#if CMK_ERROR_CHECKING
    return reqs.at(n);
#else
    return reqs[n];
#endif
  }
  void free(int idx, CkDDT *ddt) noexcept;
  void freeNonPersReq(ampiParent* pptr, int &idx) noexcept;
  inline int insert(AmpiRequest* req) noexcept {
    for (int i=startIdx; i<reqs.size(); i++) {
      if (reqs[i] == NULL) {
        req->setReqIdx(i);
        reqs[i] = req;
        startIdx = i+1;
        return i;
      }
    }
    reqs.push_back(req);
    int idx = reqs.size()-1;
    req->setReqIdx(idx);
    startIdx = idx+1;
    return idx;
  }

  inline size_t size() noexcept {
    return reqs.size();
  }

  inline size_t validSize() noexcept {
    size_t count=0;
    for (int i=0; i<reqs.size(); i++) {
      if (reqs[i] == NULL) continue;
      else count++;
    }
    return count;
  }

  inline void checkRequest(MPI_Request idx) const noexcept {
    if (idx != MPI_REQUEST_NULL && (idx < 0 || idx >= reqs.size()))
      CkAbort("Invalid MPI_Request\n");
  }

  inline void unblockReqs(MPI_Request *requests, int numReqs) noexcept {
    for (int i=0; i<numReqs; i++) {
      if (requests[i] != MPI_REQUEST_NULL) {
        reqs[requests[i]]->setBlocked(false);
      }
    }
  }

  void pup(PUP::er &p, AmpiRequestPool* reqPool) noexcept;

  void print() const noexcept {
    for (int i=0; i<reqs.size(); i++) {
      if (reqs[i] == NULL) continue;
      CkPrintf("AmpiRequestList Element %d [%p]: \n", i+1, (void *)reqs[i]);
      reqs[i]->print();
    }
  }
};

//A simple memory buffer
class memBuf {
  CkVec<char> buf;
 public:
  memBuf() =default;
  memBuf(int size) noexcept : buf(size) {}
  void setSize(int s) noexcept {buf.resize(s);}
  int getSize() const noexcept {return buf.size();}
  const void *getData() const noexcept {return (const void *)&buf[0];}
  void *getData() noexcept {return (void *)&buf[0];}
};

template <class T>
inline void pupIntoBuf(memBuf &b,T &t) noexcept {
  PUP::sizer ps;ps|t;
  b.setSize(ps.size());
  PUP::toMem pm(b.getData()); pm|t;
}

template <class T>
inline void pupFromBuf(const void *data,T &t) noexcept {
  PUP::fromMem p(data); p|t;
}

#define COLL_SEQ_IDX      -1

class AmpiMsgPool;

enum AmpiMsgType : bool {
  NCPY_SHM_MSG = true,
  NCPY_MSG     = false
};

class AmpiMsg final : public CMessage_AmpiMsg {
 private:
  int ssendReq; //Index to the sender's request (MPI_REQUEST_NULL if no request)
  int tag; //MPI tag
  int srcRank; //Communicator rank for source
  int length; //Number of bytes in this message
  int origLength; // true size of allocation
  MPI_Comm comm; // Communicator
 public:
  char *data; //Payload

 public:
  AmpiMsg() noexcept : data(NULL) {}
  AmpiMsg(int sreq, int t, int sRank, int l, MPI_Comm c) noexcept :
    ssendReq(sreq), tag(t), srcRank(sRank), length(l), origLength(l), comm(c)
  { /* only called from AmpiMsg::pup() since the refnum (seq) will get pup'ed by the runtime */ }
  AmpiMsg(CMK_REFNUM_TYPE seq, int sreq, int t, int sRank, int l) noexcept :
    ssendReq(sreq), tag(t), srcRank(sRank), length(l), origLength(l)
  { CkSetRefNum(this, seq); }
  inline bool isSsend() const noexcept { return (ssendReq >= 0); }
  inline void setSsendReq(int s) noexcept { ssendReq = s; }
  inline void setSeq(CMK_REFNUM_TYPE s) noexcept { CkAssert(s >= 0); UsrToEnv(this)->setRef(s); }
  inline void setSrcRank(int sr) noexcept { srcRank = sr; }
  inline void setLength(int l) noexcept { length = l; }
  inline void setTag(int t) noexcept { tag = t; }
  inline void setComm(MPI_Comm c) noexcept { comm = c; }
  inline CMK_REFNUM_TYPE getSeq() const noexcept { return UsrToEnv(this)->getRef(); }
  inline int getSsendReq() const noexcept { return ssendReq; }
  inline int getSeqIdx() const noexcept {
    // seqIdx is srcRank, unless this message was part of a collective
    if (tag >= MPI_BCAST_TAG && tag <= MPI_ATA_TAG) {
      return COLL_SEQ_IDX;
    }
    else {
      return srcRank;
    }
  }
  inline AmpiMsgType isNcpyShmMsg() const noexcept { CkAssert(isSsend()); return ((AmpiMsgType*)data)[0]; }
  inline void getNcpyShmBuffer(AmpiNcpyShmBuffer& srcInfo) noexcept {
    CkAssert(isNcpyShmMsg());
    PUP::fromMem p(data+sizeof(AmpiMsgType));
    p | srcInfo;
  }
  inline void getNcpyBuffer(CkNcpyBuffer& srcInfo) noexcept {
    CkAssert(!isNcpyShmMsg());
    PUP::fromMem p(data+sizeof(AmpiMsgType));
    p | srcInfo;
  }
  inline int getSrcRank() const noexcept { return srcRank; }
  inline int getLength() const noexcept { return length; }
  inline char* getData() const noexcept { return data; }
  inline int getTag() const noexcept { return tag; }
  inline MPI_Comm getComm() const noexcept { return comm; }
  static AmpiMsg* pup(PUP::er &p, AmpiMsg *m) noexcept
  {
    int ref, ssendReq, tag, srcRank, length, origLength, dataLen;
    MPI_Comm comm;
    if(p.isPacking() || p.isSizing()) {
      ref = CkGetRefNum(m);
      ssendReq = m->ssendReq;
      tag = m->tag;
      srcRank = m->srcRank;
      length = m->length;
      origLength = m->origLength;
      comm = m->comm;
      if (m->isSsend()) {
        // For SsendMsg's, m->data is a ncpyBuffer object and m->length
        // is the length of the real msg payload to be recv'ed.
        PUP::sizer pupSizer;
        AmpiMsgType msgType;
        pupSizer | msgType;
        if (m->isNcpyShmMsg()) {
          AmpiNcpyShmBuffer srcInfo;
          pupSizer | srcInfo;
        } else {
          CkNcpyBuffer srcInfo;
          pupSizer | srcInfo;
        }
        dataLen = pupSizer.size();
      } else {
        dataLen = length;
      }
    }
    p(ref); p(ssendReq); p(tag); p(srcRank); p(length); p(origLength); p(comm); p(dataLen);
    if(p.isUnpacking()) {
      m = new (origLength, 0) AmpiMsg(ref, ssendReq, tag, srcRank, origLength);
      m->setLength(length);
      m->setComm(comm);
    }
    p(m->data, dataLen);
    if(p.isDeleting()) {
      delete m;
      m = 0;
    }
    return m;
  }

  friend AmpiMsgPool;
};

#ifndef AMPI_MSG_POOL_SIZE_DEFAULT
#define AMPI_MSG_POOL_SIZE_DEFAULT    32 // Max # of AmpiMsg's allowed in the pool
#endif

#ifndef AMPI_POOLED_MSG_SIZE_DEFAULT
#define AMPI_POOLED_MSG_SIZE_DEFAULT 128 // Max # of Bytes in pooled msgs' payload
#endif

class AmpiMsgPool {
 private:
  std::vector<AmpiMsg *> msgs; // list of free msgs
  int msgLength; // AmpiMsg::length of messages in the pool
  int maxMsgs; // max # of msgs in the pool
  int currMsgs; // current # of msgs in the pool

 public:
  AmpiMsgPool(int _numMsgs = 0, int _msgLength = 0) noexcept
    : msgs(_numMsgs), msgLength{_msgLength}, maxMsgs{_numMsgs}, currMsgs{0} {}
  AmpiMsgPool(PUP::reconstruct) noexcept
    : currMsgs{0} {}
  ~AmpiMsgPool() {
    for (int i = 0; i < currMsgs; ++i)
      delete msgs[i];
  }
  inline void clear() noexcept {
    for (int i = 0; i < currMsgs; ++i) {
      AmpiMsg *& msg = msgs[i];
      delete msg;
      msg = nullptr;
    }
    currMsgs = 0;
  }
  inline AmpiMsg* newAmpiMsg(CMK_REFNUM_TYPE seq, int ssendReq, int tag, int srcRank, int len) noexcept {
    if (currMsgs == 0 || msgs[currMsgs-1]->origLength < len) {
      int newlen = std::max(msgLength, len);
      AmpiMsg* msg = new (newlen, 0) AmpiMsg(seq, ssendReq, tag, srcRank, newlen);
      msg->setLength(len);
      return msg;
    } else {
      AmpiMsg *& front = msgs[currMsgs-1];
      AmpiMsg * msg = front;
      currMsgs--;
      front = nullptr;
      msg->setSeq(seq);
      msg->setSsendReq(ssendReq);
      msg->setTag(tag);
      msg->setSrcRank(srcRank);
      msg->setLength(len);
      return msg;
    }
  }
  inline void deleteAmpiMsg(AmpiMsg* msg) noexcept {
    /* msg->origLength is the true size of the message's data buffer, while
     * msg->length is the space taken by the payload within it. */
    CkAssert(msg->getTag() != MPI_BCAST_TAG);
    if (currMsgs != maxMsgs && msg->origLength >= msgLength && msg->origLength < 2*msgLength) {
      msgs[currMsgs] = msg;
      currMsgs++;
    } else {
      delete msg;
    }
  }
  void pup(PUP::er& p) {
    p|msgLength;
    p|maxMsgs;
    if (p.isUnpacking())
      msgs.resize(maxMsgs);
    // Don't PUP the msgs in the free list or currMsgs, let the pool fill lazily
  }
};

// Number of requests in the pool
#ifndef AMPI_REQ_POOL_SIZE
#define AMPI_REQ_POOL_SIZE 64
#endif

// Helper macro for pool size and alignment calculations
#define DefinePooledReqX(name, func) \
static const size_t ireq##name = func(IReq); \
static const size_t sreq##name = func(SendReq); \
static const size_t ssreq##name = func(SsendReq); \
static const size_t pooledReq##name = (ireq##name >= sreq##name && ireq##name >= ssreq##name) ? ireq##name : \
                                      (sreq##name >= ireq##name && sreq##name >= ssreq##name) ? sreq##name : \
                                      (ssreq##name);

// This defines 'static const size_t pooledReqSize = ... ;'
DefinePooledReqX(Size, sizeof)

// This defines 'static const size_t pooledReqAlign = ... ;'
DefinePooledReqX(Align, alignof)

// Pool of IReq, SendReq, and SsendReq objects:
// These are different sizes, but we use a single pool for them so
// that iteration over these objects is fast, as in AMPI_Waitall.
// We also try to always allocate new requests from the start to the end
// of the pool, so that forward iteration over requests is fast.
class AmpiRequestPool {
 private:
  std::bitset<AMPI_REQ_POOL_SIZE> validReqs; // reqs in the pool are either valid (being used by a real req) or invalid
  int startIdx = 0; // start next search from this index
  alignas(pooledReqAlign) std::array<char, AMPI_REQ_POOL_SIZE*pooledReqSize> reqs; // pool of memory for requests

 public:
  AmpiRequestPool() =default;
  ~AmpiRequestPool() =default;
  template <typename T, typename... Args>
  inline T* newReq(Args&&... args) noexcept {
    if (validReqs.all()) {
      return new T(std::forward<Args>(args)...);
    } else {
      for (int i=startIdx; i<validReqs.size(); i++) {
        if (!validReqs[i]) {
          validReqs[i] = 1;
          startIdx = i+1;
          T* req = new (&reqs[i*pooledReqSize]) T(std::forward<Args>(args)...);
          return req;
        }
      }
      CkAbort("AMPI> failed to find a free request in pool!");
      return NULL;
    }
  }
  inline void deleteReq(AmpiRequest* req) noexcept {
    if (req->isPooledType() &&
        ((char*)req >= &reqs.front() && (char*)req <= &reqs.back()))
    {
      int idx = (int)((intptr_t)req - (intptr_t)&reqs[0]) / pooledReqSize;
      validReqs[idx] = 0;
      startIdx = std::min(idx, startIdx);
    } else {
      delete req;
    }
  }
  void pup(PUP::er& p) noexcept {
    // Nothing to do here, because AmpiRequestList::pup will be the
    // one to actually PUP the AmpiRequest objects to/from the pool
  }
};

/**
  Our local representation of another AMPI
 array element.  Used to keep track of incoming
 and outgoing message sequence numbers, and
 the out-of-order message list.
*/
class AmpiOtherElement {
private:
  /// Next incoming and outgoing message sequence number
  CMK_REFNUM_TYPE seqIncoming, seqOutgoing;

  /// Number of messages in out-of-order queue (normally 0)
  uint16_t numOutOfOrder;

public:
  /// seqIncoming starts from 1, b/c 0 means unsequenced
  /// seqOutgoing starts from 0, b/c this will be incremented for the first real seq #
  AmpiOtherElement() noexcept : seqIncoming(1), seqOutgoing(0), numOutOfOrder(0) {}

  /// Handle wrap around of unsigned type CMK_REFNUM_TYPE
  inline void incSeqIncoming() noexcept { seqIncoming++; if (seqIncoming==0) seqIncoming=1; }
  inline CMK_REFNUM_TYPE getSeqIncoming() const noexcept { return seqIncoming; }

  inline void incSeqOutgoing() noexcept { seqOutgoing++; if (seqOutgoing==0) seqOutgoing=1; }
  inline void decSeqOutgoing() noexcept { seqOutgoing--; if (seqOutgoing==0) seqOutgoing=std::numeric_limits<CMK_REFNUM_TYPE>::max(); }
  inline CMK_REFNUM_TYPE getSeqOutgoing() const noexcept { return seqOutgoing; }

  inline void incNumOutOfOrder() noexcept { numOutOfOrder++; }
  inline void decNumOutOfOrder() noexcept { numOutOfOrder--; }
  inline uint16_t getNumOutOfOrder() const noexcept { return numOutOfOrder; }
};
PUPbytes(AmpiOtherElement)

class AmpiSeqQ : private CkNoncopyable {
  CkMsgQ<AmpiMsg> out; // all out of order messages
  std::unordered_map<int, AmpiOtherElement> elements; // element info: indexed by seqIdx (comm rank)

public:
  AmpiSeqQ() =default;
  AmpiSeqQ(int commSize) noexcept {
    elements.reserve(std::min(commSize, 64));
  }
  ~AmpiSeqQ() =default;
  void pup(PUP::er &p) noexcept;

  /// Insert this message in the table.  Returns the number
  /// of messages now available for the element.
  ///   If 0, the message was out-of-order and is buffered.
  ///   If 1, this message can be immediately processed.
  ///   If >1, this message can be immediately processed,
  ///     and you should call "getOutOfOrder" repeatedly.
  inline int put(int seqIdx, AmpiMsg *msg) noexcept {
    AmpiOtherElement &el = elements[seqIdx];
    if (msg->getSeq() == el.getSeqIncoming()) { // In order:
      el.incSeqIncoming();
      return 1+el.getNumOutOfOrder();
    }
    else { // Out of order: stash message
      putOutOfOrder(seqIdx, msg);
      return 0;
    }
  }

  /// Is this message in order (return >0) or not (return 0)?
  /// Same as put() except we don't call putOutOfOrder() here,
  /// so the caller should do that separately
  inline int putIfInOrder(int srcRank, CMK_REFNUM_TYPE seq) noexcept {
    AmpiOtherElement &el = elements[srcRank];
    if (seq == el.getSeqIncoming()) { // In order:
      el.incSeqIncoming();
      return 1+el.getNumOutOfOrder();
    }
    else { // Out of order: caller should stash message
      return 0;
    }
  }

  /// Is this in-order?
  inline bool isInOrder(int seqIdx, CMK_REFNUM_TYPE seq) noexcept {
    return (seq == elements[seqIdx].getSeqIncoming());
  }

  /// Get an out-of-order message from the table.
  /// (in-order messages never go into the table)
  AmpiMsg *getOutOfOrder(int seqIdx) noexcept;

  /// Stash an out-of-order message
  void putOutOfOrder(int seqIdx, AmpiMsg *msg) noexcept;

  /// Increment the outgoing sequence number.
  inline void incCollSeqOutgoing() noexcept {
    elements[COLL_SEQ_IDX].incSeqOutgoing();
  }

  /// Return the next outgoing sequence number, and increment it.
  inline CMK_REFNUM_TYPE nextOutgoing(int destRank) noexcept {
    AmpiOtherElement &el = elements[destRank];
    el.incSeqOutgoing();
    return el.getSeqOutgoing();
  }

  /// Reset the outgoing sequence number to its previous value.
  inline void resetOutgoing(int destRank) noexcept {
    elements[destRank].decSeqOutgoing();
  }
};

inline CProxy_ampi ampiCommStruct::getProxy() const noexcept {return ampiID;}

// Max value of a predefined MPI_Op (values defined in ampi.h)
#define AMPI_MAX_PREDEFINED_OP 13

// COMM_WORLD and COMM_SELF, in that order, are the first two communicators
// created in AMPI, so MPI_COMM_SELF must be MPI_COMM_WORLD+1 in ampi.h
static_assert(MPI_COMM_SELF == MPI_COMM_WORLD+1,
              "AMPI's MPI_COMM_SELF constant should be equal to MPI_COMM_WORLD+1");

class AmpiCommStructMap {
 private:
  std::unordered_map<MPI_Comm, ampiCommStruct> comms; //All communicators
  MPI_Comm nextComm = MPI_COMM_WORLD; // Value of the next MPI_Comm to be created
                                      // COMM_WORLD and COMM_SELF are the first two created

 public:
  AmpiCommStructMap() = default;
  ~AmpiCommStructMap() = default;
  void pup(PUP::er& p) noexcept {
    p|comms;
    p|nextComm;
  }
  void checkComm(MPI_Comm comm) const noexcept {
    if (comm == MPI_COMM_SELF || comm == MPI_COMM_WORLD)
      return;
    const auto search = comms.find(comm);
    if (search == comms.end()) {
      CkAbort("AMPI: invalid MPI_Comm %d used!", comm);
    }
  }
  MPI_Comm getNextComm() const noexcept { return nextComm; }
  MPI_Comm insert(const ampiCommStruct &commStruct) noexcept {
    int comm = commStruct.getComm();
    comms[comm] = commStruct;
    if (comm >= nextComm) {
      nextComm = comm+1;
    }
    return comm;
  }
  void free(MPI_Comm comm) noexcept {
    comms.erase(comm);
    if (comm == nextComm-1) nextComm--;
  }
  bool isComm(MPI_Comm comm) const noexcept {
    const auto search = comms.find(comm);
    return (search != comms.end());
  }
  const ampiCommStruct& getCommStruct(MPI_Comm comm) const noexcept {
    const auto search = comms.find(comm);
#if CMK_ERROR_CHECKING
    if (search == comms.end()) {
      CkAbort("AMPI: invalid MPI_Comm %d used!", comm);
    }
#endif
    return search->second;
  }
  ampiCommStruct& getCommStruct(MPI_Comm comm) noexcept {
    return comms[comm];
  }
  const ampiCommStruct& operator[](MPI_Comm comm) const noexcept {
    const auto search = comms.find(comm);
#if CMK_ERROR_CHECKING
    if (search == comms.end()) {
      CkAbort("AMPI: invalid MPI_Comm %d used!", comm);
    }
#endif
    return search->second;
  }
  ampiCommStruct& operator[](MPI_Comm comm) noexcept { return comms[comm]; }
  AmpiCommType getType(MPI_Comm comm) const noexcept {
    const auto search = comms.find(comm);
#if CMK_ERROR_CHECKING
    if (search == comms.end()) {
      CkAbort("AMPI: invalid MPI_Comm %d used!", comm);
    }
#endif
    return search->second.getType();
  }
};

/*
An ampiParent holds all the communicators and the TCharm thread
for its children, which are bound to it.
*/
class ampiParent final : public CBase_ampiParent {
 private:
  TCharm *thread;
  CProxy_TCharm threads;

 public: // Communication state:
  int numBlockedReqs; // number of requests currently blocked on
  bool resumeOnRecv, resumeOnColl;
  AmpiRequestList ampiReqs;
  AmpiRequestPool reqPool;
  AmpiRequest *blockingReq;
  CkDDT myDDT;

 private:
  ampi *worldPtr; //AMPI element corresponding to MPI_COMM_WORLD

  AmpiCommStructMap comms; //All communicators

  CkPupPtrVec<groupStruct> groups; // "Wild" groups that don't have a communicator
  CkPupPtrVec<WinStruct> winStructList; //List of windows for one-sided communication
  CkPupPtrVec<InfoStruct> infos; // list of all MPI_Infos
  const std::array<MPI_User_function*, AMPI_MAX_PREDEFINED_OP+1>& predefinedOps; // owned by ampiNodeMgr
  std::vector<OpStruct> userOps; // list of any user-defined MPI_Ops
  std::vector<AmpiMsg *> matchedMsgs; // for use with MPI_Mprobe and MPI_Mrecv

  CkPupPtrVec<KeyvalNode> kvlist;
  void* bsendBuffer;   // NOTE: we don't actually use this for buffering of MPI_Bsend's,
  int bsendBufferSize; //       we only keep track of it to return it from MPI_Buffer_detach

  // Intercommunicator creation:
  bool isTmpRProxySet;
  CProxy_ampi tmpRProxy;

  int myHomePE;

  MPI_MigrateFn userAboutToMigrateFn, userJustMigratedFn;
  bool didMigrate{};

 public:
  bool ampiInitCallDone;

#if CMK_AMPI_WITH_ROMIO
  ADIO_GlobalStruct romio_globals;
#endif

 public:
  void prepareCtv() noexcept;
  TCharm* getThread() noexcept { return thread; }

  MPI_Message putMatchedMsg(AmpiMsg* msg) noexcept {
    // Search thru matchedMsgs for any NULL ones first:
    for (int i=0; i<matchedMsgs.size(); i++) {
      if (matchedMsgs[i] == NULL) {
        matchedMsgs[i] = msg;
        return i;
      }
    }
    // No NULL entries, so create a new one:
    matchedMsgs.push_back(msg);
    return matchedMsgs.size() - 1;
  }
  AmpiMsg* getMatchedMsg(MPI_Message message) noexcept {
    if (message == MPI_MESSAGE_NO_PROC || message == MPI_MESSAGE_NULL) {
      return NULL;
    }
    CkAssert(message >= 0 && message < matchedMsgs.size());
    AmpiMsg* msg = matchedMsgs[message];
    // Mark this matchedMsg index NULL and free from back of vector:
    matchedMsgs[message] = NULL;
    while (matchedMsgs.back() == NULL) {
      matchedMsgs.pop_back();
    }
    return msg;
  }

  inline void attachBuffer(void *buffer, int size) noexcept {
    bsendBuffer = buffer;
    bsendBufferSize = size;
  }
  inline void detachBuffer(void *buffer, int *size) noexcept {
    *(void **)buffer = bsendBuffer;
    *size = bsendBufferSize;
  }

  AmpiCommStructMap &getCommStructMap() noexcept { return comms; }
  ampiCommStruct &getCommStruct(MPI_Comm comm) noexcept { return comms[comm]; }
  const ampiCommStruct &getCommStruct(MPI_Comm comm) const noexcept { return comms[comm]; }

  ampiParent(CProxy_TCharm threads_,int nRanks_) noexcept;
  ampiParent(CkMigrateMessage *msg) noexcept;
  void ckAboutToMigrate() noexcept;
  void ckJustMigrated() noexcept;
  void ckJustRestored() noexcept;
  void resumeAfterMigration() noexcept;
  void setUserAboutToMigrateFn(MPI_MigrateFn f) noexcept;
  void setUserJustMigratedFn(MPI_MigrateFn f) noexcept;
  ~ampiParent() noexcept;

  //Children call this when they are first created, or just migrated
  TCharm *registerAmpi(ampi *ptr,const ampiCommStruct &s,bool forMigration) noexcept;

  // exchange proxy info between two ampi proxies
  void ExchangeProxy(CProxy_ampi rproxy) noexcept {
    if(!isTmpRProxySet){ tmpRProxy=rproxy; isTmpRProxySet=true; }
    else{ tmpRProxy.setRemoteProxy(rproxy); rproxy.setRemoteProxy(tmpRProxy); isTmpRProxySet=false; }
  }

  //Grab the next available split/group communicator
  MPI_Comm getNextComm() const noexcept {return comms.getNextComm();}

  bool isSplit(MPI_Comm comm) const noexcept { return (comms.getType(comm) == COMM_SPLIT); }
  bool isCart(MPI_Comm comm) const noexcept { return (comms.getType(comm) == COMM_CART); }
  bool isGraph(MPI_Comm comm) const noexcept { return (comms.getType(comm) == COMM_GRAPH); }
  bool isDistGraph(MPI_Comm comm) const noexcept { return (comms.getType(comm) == COMM_DIST_GRAPH); }
  bool isInter(MPI_Comm comm) const noexcept { return (comms.getType(comm) == COMM_INTER); }
  bool isIntra(MPI_Comm comm) const noexcept { return (comms.getType(comm) == COMM_INTRA); }
  bool isGroup(MPI_Comm comm) const noexcept { return (comms.getType(comm) == COMM_GROUP); }
  bool isInGroups(MPI_Group group) const noexcept { return (group>=0 && group<groups.size()); }

  void pup(PUP::er &p) noexcept;

  void startCheckpoint(const char* dname) noexcept;
  void Checkpoint(int len, const char* dname) noexcept;
  void ResumeThread() noexcept;
  TCharm* getTCharmThread() const noexcept {return thread;}
  CMI_WARN_UNUSED_RESULT inline ampiParent* blockOnRecv() noexcept;
  CMI_WARN_UNUSED_RESULT static ampiParent* static_blockOnColl(ampiParent* dis) noexcept;
  CMI_WARN_UNUSED_RESULT CMI_FORCE_INLINE ampiParent* blockOnColl() noexcept {
    return static_blockOnColl(this);
  }
  inline CkDDT* getDDT() noexcept { return &myDDT; }

#if CMK_LBDB_ON
  void setMigratable(bool mig) noexcept {
    thread->setMigratable(mig);
  }
#endif

  const ampiCommStruct &getWorldStruct() const noexcept;

  const ampiCommStruct &comm2CommStruct(MPI_Comm comm) const noexcept;

  void freeCommStruct(MPI_Comm comm) noexcept { comms.free(comm); }

  inline std::unordered_map<int, uintptr_t> & getAttributes(MPI_Comm comm) noexcept {
    ampiCommStruct & cs = const_cast<ampiCommStruct &>(comm2CommStruct(comm));
    return cs.getAttributes();
  }

  inline ampi *comm2ampi(MPI_Comm comm) const noexcept {
    if (comm==MPI_COMM_WORLD) return worldPtr;
    const ampiCommStruct &st = getCommStruct(comm);
    return st.getProxy()[thisIndex].ckLocal();
  }

  inline bool hasComm(const MPI_Group group) const noexcept {
    MPI_Comm comm = (MPI_Comm)group;
    return (comms.isComm(comm) && !isInter(comm));
    //isInter omitted because its comm number != its group number
  }
  inline std::vector<int> group2vec(MPI_Group group) const noexcept {
    if (group == MPI_GROUP_NULL || group == MPI_GROUP_EMPTY) {
      return std::vector<int>();
    }
    else if (hasComm(group)) {
      return comm2CommStruct((MPI_Comm)group).getIndices();
    }
    else {
      CkAssert(isInGroups(group));
      return groups[group]->getRanks();
    }
  }
  inline MPI_Group saveGroupStruct(const std::vector<int>& vec) noexcept {
    if (vec.empty()) return MPI_GROUP_EMPTY;
    for (int i=0; i<groups.size(); i++) {
      if (vec == groups[i]->getRanks()) {
        groups[i]->incRefCount();
        return i;
      }
    }
    int idx = groups.size();
    groups.resize(idx+1);
    groups[idx]=new groupStruct(vec);
    return (MPI_Group)idx;
  }
  void freeGroupStruct(const MPI_Group group) noexcept {
    if (group == MPI_GROUP_EMPTY || group == MPI_GROUP_NULL || hasComm(group)) {
      return;
    }
    if (groups[group]->decRefCount() == 0) {
      delete groups[group];
      groups.remove(group);
    }
  }
  inline int getRank(const MPI_Group group) const noexcept {
    std::vector<int> vec = group2vec(group);
    return getPosOp(thisIndex,vec);
  }
  inline AmpiRequestList &getReqs() noexcept { return ampiReqs; }
  inline int getMyPe() const noexcept {
    return CkMyPe();
  }
  inline bool hasWorld() const noexcept {
    return worldPtr!=NULL;
  }

  inline void checkComm(MPI_Comm comm) const noexcept {
    comms.checkComm(comm);
  }

  /// if intra-communicator, return comm, otherwise return null group
  inline MPI_Group comm2group(const MPI_Comm comm) const noexcept {
    if(isInter(comm)) return MPI_GROUP_NULL;   // we don't support inter-communicator in such functions
    const ampiCommStruct& s = comm2CommStruct(comm);
    if(comm!=MPI_COMM_WORLD && comm!=s.getComm()) CkAbort("Error in ampiParent::comm2group()");
    return (MPI_Group)(s.getComm());
  }

  inline int getRemoteSize(const MPI_Comm comm) const noexcept {
    if(isInter(comm)) return getCommStruct(comm).getRemoteIndices().size();
    else return -1;
  }
  inline MPI_Group getRemoteGroup(const MPI_Comm comm) noexcept {
    if(isInter(comm)) return saveGroupStruct(getCommStruct(comm).getRemoteIndices());
    else return MPI_GROUP_NULL;
  }

  int createKeyval(MPI_Copy_function *copy_fn, MPI_Delete_function *delete_fn,
                  int *keyval, void* extra_state) noexcept;
  bool getBuiltinAttributeComm(int keyval, void *attribute_val) noexcept;
  bool getBuiltinAttributeWin(int keyval, void *attribute_val, WinStruct * winStruct) noexcept;
  int setUserAttribute(int context, std::unordered_map<int, uintptr_t> & attributes, int keyval, void *attribute_val) noexcept;
  bool getUserAttribute(int context, std::unordered_map<int, uintptr_t> & attributes, int keyval, void *attribute_val, int *flag) noexcept;
  int dupUserAttributes(int old_context, std::unordered_map<int, uintptr_t> & old_attr, std::unordered_map<int, uintptr_t> & new_attr) noexcept;
  int freeUserAttributes(int context, std::unordered_map<int, uintptr_t> & attributes) noexcept;
  int freeKeyval(int keyval) noexcept;

  int getAttrComm(MPI_Comm comm, std::unordered_map<int, uintptr_t> & attributes, int keyval, void *attribute_val, int *flag) noexcept;
  int getAttrType(MPI_Datatype datatype, std::unordered_map<int, uintptr_t> & attributes, int keyval, void *attribute_val, int *flag) noexcept;
  int getAttrWin(MPI_Win win, std::unordered_map<int, uintptr_t> & attributes, int keyval, void *attribute_val, int *flag, WinStruct * winStruct) noexcept;
  int setAttrComm(MPI_Comm comm, std::unordered_map<int, uintptr_t> & attributes, int keyval, void *attribute_val) noexcept;
  int setAttrType(MPI_Datatype datatype, std::unordered_map<int, uintptr_t> & attributes, int keyval, void *attribute_val) noexcept
  {
    return setUserAttribute(datatype, attributes, keyval, attribute_val);
  }
  int setAttrWin(MPI_Win win, std::unordered_map<int, uintptr_t> & attributes, int keyval, void *attribute_val) noexcept;
  int deleteAttr(int context, std::unordered_map<int, uintptr_t> & attributes, int keyval) noexcept;

  int addWinStruct(WinStruct *win) noexcept;
  WinStruct *getWinStruct(MPI_Win win) const noexcept;
  void removeWinStruct(WinStruct *win) noexcept;

  int createInfo(MPI_Info *newinfo) noexcept;
  int dupInfo(MPI_Info info, MPI_Info *newinfo) noexcept;
  int setInfo(MPI_Info info, const char *key, const char *value) noexcept;
  int deleteInfo(MPI_Info info, const char *key) noexcept;
  int getInfo(MPI_Info info, const char *key, int valuelen, char *value, int *flag) const noexcept;
  int getInfoValuelen(MPI_Info info, const char *key, int *valuelen, int *flag) const noexcept;
  int getInfoNkeys(MPI_Info info, int *nkeys) const noexcept;
  int getInfoNthkey(MPI_Info info, int n, char *key) const noexcept;
  int freeInfo(MPI_Info info) noexcept;
  void defineInfoEnv(int nRanks_) noexcept;
  void defineInfoMigration() noexcept;

  // An 'MPI_Op' is an integer that indexes into either:
  //   A) an array of predefined ops owned by ampiNodeMgr, or
  //   B) a vector of user-defined ops owned by ampiParent
  // The MPI_Op is compared to AMPI_MAX_PREDEFINED_OP to disambiguate.
  inline int createOp(MPI_User_function *fn, bool isCommutative) noexcept {
    // Search thru non-predefined op's for any invalidated ones:
    for (int i=0; i<userOps.size(); i++) {
      if (userOps[i].isFree()) {
        userOps[i].init(fn, isCommutative);
        return AMPI_MAX_PREDEFINED_OP + 1 + i;
      }
    }
    // No invalid entries, so create a new one:
    userOps.emplace_back(fn, isCommutative);
    return AMPI_MAX_PREDEFINED_OP + userOps.size();
  }
  inline void freeOp(MPI_Op op) noexcept {
    // Don't free predefined op's:
    if (!opIsPredefined(op)) {
      // Invalidate op, then free all invalid op's from the back of the userOp's vector
      int opIdx = op - 1 - AMPI_MAX_PREDEFINED_OP;
      CkAssert(opIdx < userOps.size());
      userOps[opIdx].free();
      while (!userOps.empty() && userOps.back().isFree()) {
        userOps.pop_back();
      }
    }
  }
  inline bool opIsPredefined(MPI_Op op) const noexcept {
    return (op <= AMPI_MAX_PREDEFINED_OP);
  }
  inline bool opIsCommutative(MPI_Op op) const noexcept {
    if (opIsPredefined(op)) {
      return true; // all predefined ops are commutative
    }
    else {
      int opIdx = op - 1 - AMPI_MAX_PREDEFINED_OP;
      CkAssert(opIdx < userOps.size());
      return userOps[opIdx].isCommutative;
    }
  }
  MPI_User_function* op2User_function(MPI_Op op) const noexcept;
  inline AmpiOpHeader op2AmpiOpHeader(MPI_Op op, MPI_Datatype type, int count) const noexcept {
    if (opIsPredefined(op)) {
      int size = myDDT.getType(type)->getSize(count);
      return AmpiOpHeader(predefinedOps[op], type, count, size);
    }
    else {
      int opIdx = op - 1 - AMPI_MAX_PREDEFINED_OP;
      CkAssert(opIdx < userOps.size());
      int size = myDDT.getType(type)->getSize(count);
      return AmpiOpHeader(userOps[opIdx].func, type, count, size);
    }
  }
  inline void applyOp(MPI_Datatype datatype, MPI_Op op, int count, const void* invec, void* inoutvec) const noexcept {
    // inoutvec[i] = invec[i] op inoutvec[i]
    MPI_User_function *func = op2User_function(op);
    (func)((void*)invec, inoutvec, &count, &datatype);
  }

  CMI_WARN_UNUSED_RESULT ampiParent* waitWithoutFree(MPI_Request* req, MPI_Status *sts) noexcept;

  CMI_WARN_UNUSED_RESULT ampiParent* wait(MPI_Request* req, MPI_Status* sts) noexcept;
  CMI_WARN_UNUSED_RESULT ampiParent* waitall(int count, MPI_Request request[], MPI_Status sts[]=MPI_STATUSES_IGNORE) noexcept;
  void init() noexcept;
  void finalize() noexcept;
  CMI_WARN_UNUSED_RESULT ampiParent* block() noexcept;
  CMI_WARN_UNUSED_RESULT ampiParent* yield() noexcept;

#if AMPI_PRINT_MSG_SIZES
// Map of AMPI routine names to message sizes and number of messages:
// ["AMPI_Routine"][ [msg_size][num_msgs] ]
  std::unordered_map<std::string, std::map<int, int> > msgSizes;
  inline bool isRankRecordingMsgSizes() noexcept;
  inline void recordMsgSize(const char* func, int msgSize) noexcept;
  void printMsgSizes() noexcept;
#endif

#if AMPIMSGLOG
  /* message logging */
  int pupBytes;
#if CMK_USE_ZLIB && 0
  gzFile fMsgLog;
  PUP::tozDisk *toPUPer;
  PUP::fromzDisk *fromPUPer;
#else
  FILE* fMsgLog;
  PUP::toDisk *toPUPer;
  PUP::fromDisk *fromPUPer;
#endif
#endif
};

// Store a generalized request class created by MPIX_Grequest_class_create
class greq_class_desc {
public:
  MPI_Grequest_query_function *query_fn;
  MPI_Grequest_free_function *free_fn;
  MPI_Grequest_cancel_function *cancel_fn;
  MPIX_Grequest_poll_function *poll_fn;
  MPIX_Grequest_wait_function *wait_fn;

  void pup(PUP::er &p) noexcept {
    p((char *)query_fn, sizeof(void *));
    p((char *)free_fn, sizeof(void *));
    p((char *)cancel_fn, sizeof(void *));
    p((char *)poll_fn, sizeof(void *));
    p((char *)wait_fn, sizeof(void *));
  }
};

/*
An ampi manages the communication of one thread over
one MPI communicator.
*/
class ampi final : public CBase_ampi {
 private:
  friend class IReq; // for checking resumeOnRecv
  friend class SendReq;
  friend class SsendReq;
  friend class RednReq;
  friend class GatherReq;
  friend class GathervReq;

  ampiParent *parent;
  CProxy_ampiParent parentProxy;
  TCharm *thread;
  MPI_Comm comm;
  int myRank;
  AmpiSeqQ oorder;

 public:
  /*
   * AMPI Message Matching (Amm) queues are indexed by the tag and sender.
   * Since ampi objects are per-communicator, there are separate Amm's per communicator.
   */
  Amm<AmpiRequest *, AMPI_AMM_PT2PT_POOL_SIZE> postedReqs;
  Amm<AmpiMsg *, AMPI_AMM_PT2PT_POOL_SIZE> unexpectedMsgs;

  // Bcast requests / msgs must be kept separate from pt2pt,
  // so we don't match them to wildcard recv's
  Amm<AmpiRequest *, AMPI_AMM_COLL_POOL_SIZE> postedBcastReqs;
  Amm<AmpiMsg *, AMPI_AMM_COLL_POOL_SIZE> unexpectedBcastMsgs;

  // Store generalized request classes created by MPIX_Grequest_class_create
  std::vector<greq_class_desc> greq_classes;

 private:
  ampiCommStruct* myComm; // pointer to my ampiCommStruct, owned by ampiParent::comms
  std::vector<int> tmpVec; // stores temp group info
  CProxy_ampi remoteProxy; // valid only for intercommunicator
  CkPupPtrVec<win_obj> winObjects;
  int numCommCreationsInProgress{};
  void setNumCommCreationsInProgress(int x)
  {
    CkAssert(numCommCreationsInProgress == 0);
    numCommCreationsInProgress = x;
  }

  // Intercommunicator splitting:
  std::unordered_map<int, CProxy_ampi> splitRProxyMap;

 private:
  inline bool isInOrder(int seqIdx, int seq) noexcept { return oorder.isInOrder(seqIdx, seq); }
  bool inorder(AmpiMsg *msg) noexcept;
  void inorderBcast(AmpiMsg *msg, bool deleteMsg) noexcept;
  void inorderRdma(char* buf, int size, CMK_REFNUM_TYPE seq, int tag, int srcRank) noexcept;
  void inorderBcastRdma(char *buf, int size, CMK_REFNUM_TYPE, int tag, int srcRank) noexcept;
  inline void localInorder(char* buf, int size, int seqIdx, CMK_REFNUM_TYPE seq, int tag,
                           int srcRank, IReq* ireq) noexcept;

  void init() noexcept;
  void findParentAfterMigration() noexcept;
  void findParentAfterCreation(const ampiCommStruct &s) noexcept;

 public: // entry methods
  ampi() noexcept;
  ampi(CkArrayID parent_,const ampiCommStruct &s) noexcept;
  ampi(CkMigrateMessage *msg) noexcept;
  void ckJustMigrated() noexcept;
  void ckJustRestored() noexcept;
  ~ampi() noexcept;

  void pup(PUP::er &p) noexcept;

  void allInitDone() noexcept;
  void setInitDoneFlag() noexcept;

  inline void unblock() noexcept {
    thread->resume();
  }

  void injectMsg(int size, char* buf) noexcept;
  void genericSync(AmpiMsg *) noexcept;
  void generic(AmpiMsg *) noexcept;
  void genericRdma(char* buf, int size, CMK_REFNUM_TYPE seq, int tag, int srcRank) noexcept;
  void completedRdmaSend(CkDataMsg *msg) noexcept;
  void completedRdmaBcastSend(CkDataMsg *msg) noexcept;
  void completedRdmaRecv(CkDataMsg *msg) noexcept;
  void requestPut(MPI_Request req, CkNcpyBuffer targetInfo) noexcept;
  void bcastResult(AmpiMsg *msg) noexcept;
  void bcastResultRdma(char* buf, int size, CMK_REFNUM_TYPE seq, int tag, int srcRank) noexcept;
  void barrierResult(void) noexcept;
  void ibarrierResult(void) noexcept;
  void rednResult(CkReductionMsg *msg) noexcept;
  void irednResult(CkReductionMsg *msg) noexcept;

  void splitPhase1(CkReductionMsg *msg) noexcept;
  void splitPhaseInter(CkReductionMsg *msg) noexcept;
  void commCreatePhase1(int nextComm, int commType) noexcept;
  void registrationFinish() noexcept;
  void intercommCreatePhase1(MPI_Comm nextInterComm) noexcept;
  void intercommMergePhase1(MPI_Comm nextIntraComm) noexcept;

  void exchangeProxyForSplitLocal(int color, CProxy_ampi splitLocalProxy) noexcept {
    auto iter = splitRProxyMap.find(color);
    if (iter == splitRProxyMap.end()) {
      splitRProxyMap.emplace(color, splitLocalProxy);
    }
    else {
      CProxy_ampi splitRemoteProxy = iter->second;
      splitRProxyMap.erase(iter);
      splitLocalProxy.setRemoteProxy(splitRemoteProxy);
    }
  }
  void exchangeProxyForSplitRemote(int color, CProxy_ampi splitRemoteProxy) noexcept {
    auto iter = splitRProxyMap.find(color);
    if (iter == splitRProxyMap.end()) {
      splitRProxyMap.emplace(color, splitRemoteProxy);
    }
    else {
      CProxy_ampi splitLocalProxy = iter->second;
      splitRProxyMap.erase(iter);
      splitLocalProxy.setRemoteProxy(splitRemoteProxy);
    }
  }

 private: // Used by the above entry methods that create new MPI_Comm objects
  CProxy_ampi createNewChildAmpiSync() noexcept;
  CProxy_ampi createNewSplitCommArray(MPI_Comm newComm, const std::vector<int> & indices, AmpiCommType type) noexcept;
  void insertNewChildAmpiElements(MPI_Comm newComm, CProxy_ampi newAmpi, AmpiCommType type) noexcept;

  inline void handleBlockedReq(AmpiRequest* req) noexcept {
    if (req->isBlocked() && parent->numBlockedReqs != 0) {
      parent->numBlockedReqs--;
    }
  }
  inline void resumeThreadIfReady() noexcept {
    if (parent->resumeOnRecv && parent->numBlockedReqs == 0) {
      thread->resume();
    }
  }

 private: // for this pointer safety after migration
  CMI_WARN_UNUSED_RESULT static ampi* static_blockOnColl(ampi* dis) noexcept;
  static int static_recv(ampi* dis,int t,int s,void* buf,int count,MPI_Datatype type,MPI_Comm comm,MPI_Status *sts) noexcept;
  static void static_probe(ampi* dis,int t,int s,MPI_Comm comm,MPI_Status *sts) noexcept;
  static void static_mprobe(ampi* dis, int t, int s, MPI_Comm comm, MPI_Status *sts, MPI_Message *message) noexcept;

 public: // to be used by MPI_* functions
  const ampiCommStruct &comm2CommStruct(MPI_Comm comm) const noexcept { return parent->comm2CommStruct(comm); }
  const ampiCommStruct &getCommStruct() const noexcept { return *myComm; }

  CMI_WARN_UNUSED_RESULT inline ampi* blockOnRecv() noexcept;
  CMI_WARN_UNUSED_RESULT CMI_FORCE_INLINE ampi* blockOnColl() noexcept {
    return static_blockOnColl(this);
  }
  inline void setBlockingReq(AmpiRequest *req) noexcept;
  inline AmpiRequestPool& getReqPool() const { return parent->reqPool; }
  CMI_WARN_UNUSED_RESULT inline ampi* blockOnIReq(void* buf, int count, MPI_Datatype type, int s,
                                                  int t, MPI_Comm comm, MPI_Status* sts) noexcept;
  MPI_Request postReq(AmpiRequest* newreq) noexcept;
  inline void waitOnBlockingSend(MPI_Request* req, AmpiSendType sendType) noexcept;
  inline void completedSend(MPI_Request req, CkNcpyBuffer *srcInfo=nullptr) noexcept;
  inline void completedRecv(MPI_Request req, CkNcpyBuffer *targetInfo=nullptr) noexcept;

  inline CMK_REFNUM_TYPE getSeqNo(int destRank, MPI_Comm destcomm, int tag) noexcept;
  AmpiMsg *makeBcastMsg(const void *buf,int count,MPI_Datatype type,int root,MPI_Comm destcomm) noexcept;
  AmpiMsg *makeSyncMsg(int t,int sRank,const void *buf,int count,
                       MPI_Datatype type,CProxy_ampi destProxy,
                       int destIdx,int ssendReq,CMK_REFNUM_TYPE seq,
                       ampi* destPtr) noexcept;
  AmpiMsg *makeNcpyShmMsg(int t, int sRank, const void* buf, int count,
                          MPI_Datatype type, int ssendReq, int seq) noexcept;
  AmpiMsg *makeNcpyMsg(int t, int sRank, const void* buf, int count,
                       MPI_Datatype type, int ssendReq, int seq) noexcept;
  AmpiMsg *makeAmpiMsg(int destRank,int t,int sRank,const void *buf,int count,
                       MPI_Datatype type,MPI_Comm destcomm) noexcept;
  AmpiMsg *makeAmpiMsg(int destRank,int t,int sRank,const void *buf,int count,
                       MPI_Datatype type,MPI_Comm destcomm,CMK_REFNUM_TYPE seq) noexcept;

  MPI_Request send(int t, int s, const void* buf, int count, MPI_Datatype type, int rank,
                   MPI_Comm destcomm, AmpiSendType sendType=BLOCKING_SEND, MPI_Request=MPI_REQUEST_NULL) noexcept;
  static void sendraw(int t, int s, void* buf, int len, CkArrayID aid, int idx) noexcept;
  inline MPI_Request sendSyncMsg(int t, int sRank, const void* buf, MPI_Datatype type, int count,
                                 int rank, MPI_Comm destcomm, CMK_REFNUM_TYPE seq, CProxy_ampi destElem,
                                 int destIdx, AmpiSendType sendType, MPI_Request reqIdx, ampi* destPtr) noexcept;
  inline MPI_Request sendLocalMsg(int t, int sRank, const void* buf, int size, MPI_Datatype type,
                                  int count, int destRank, MPI_Comm destcomm, CMK_REFNUM_TYPE seq,
                                  ampi* destPtr, AmpiSendType sendType, MPI_Request reqIdx) noexcept;
  inline MPI_Request sendRdmaMsg(int t, int sRank, const void* buf, int size, MPI_Datatype type, int destIdx,
                                 int destRank, MPI_Comm destcomm, CMK_REFNUM_TYPE seq, CProxy_ampi arrProxy,
                                 MPI_Request reqIdx) noexcept;

  inline MPI_Request sendRdmaBcastMsg(const void* buf, int size, MPI_Datatype type, MPI_Comm destcomm, int root, MPI_Request reqIdx) noexcept;
  inline bool destLikelyWithinProcess(CProxy_ampi arrProxy, int destIdx, ampi* destPtr) const noexcept {
#if CMK_MULTICORE
    return true;
#elif CMK_SMP
    if (destPtr != NULL) return true;
    CkArray* localBranch = arrProxy.ckLocalBranch();
    int destPe = localBranch->lastKnown(CkArrayIndex1D(destIdx));
    return (CkNodeOf(destPe) == CkMyNode());
#else // non-SMP
    return (destPtr != NULL);
#endif
  }
  inline MPI_Request delesend(int t, int s, const void* buf, int count, MPI_Datatype type, int rank,
                              MPI_Comm destcomm, CProxy_ampi arrproxy, AmpiSendType sendType, MPI_Request req) noexcept;
  inline bool processSsendMsg(AmpiMsg* msg, void* buf, MPI_Datatype type, int count, MPI_Request req) noexcept;
  inline bool processSsendNcpyShmMsg(AmpiMsg* msg, void* buf, MPI_Datatype type, int count, MPI_Request req) noexcept;
  inline bool processSsendNcpyMsg(AmpiMsg* msg, void* buf, MPI_Datatype type, int count, MPI_Request req) noexcept;
  inline bool processAmpiMsg(AmpiMsg *msg, void* buf, MPI_Datatype type, int count, MPI_Request req) noexcept;
  inline void processRdmaMsg(const void *sbuf, int slength, void* rbuf, int rcount, MPI_Datatype rtype) noexcept;
  inline void processRednMsg(CkReductionMsg *msg, void* buf, MPI_Datatype type, int count) noexcept;
  inline void processNoncommutativeRednMsg(CkReductionMsg *msg, void* buf, MPI_Datatype type, int count,
                                           MPI_User_function* func) noexcept;
  inline void processGatherMsg(CkReductionMsg *msg, void* buf, MPI_Datatype type, int recvCount) noexcept;
  inline void processGathervMsg(CkReductionMsg *msg, void* buf, MPI_Datatype type,
                               int* recvCounts, int* displs) noexcept;
  inline AmpiMsg * getMessage(int t, int s, MPI_Comm comm, int *sts) const noexcept;
  CMI_FORCE_INLINE int recv(int t,int s,void* buf,int count,MPI_Datatype type,MPI_Comm comm,MPI_Status *sts=NULL) noexcept {
    return static_recv(this, t, s, buf, count, type, comm, sts);
  }
  void irecv(void *buf, int count, MPI_Datatype type, int src,
             int tag, MPI_Comm comm, MPI_Request *request) noexcept;
  void mrecv(int tag, int src, void* buf, int count, MPI_Datatype datatype, MPI_Comm comm,
             MPI_Status* status, MPI_Message* message) noexcept;
  void imrecv(void* buf, int count, MPI_Datatype datatype, int src, int tag, MPI_Comm comm,
              MPI_Request* request, MPI_Message* message) noexcept;
  void irecvBcast(void *buf, int count, MPI_Datatype type, int src,
                  MPI_Comm comm, MPI_Request *request) noexcept;
  void sendrecv(const void *sbuf, int scount, MPI_Datatype stype, int dest, int stag,
                void *rbuf, int rcount, MPI_Datatype rtype, int src, int rtag,
                MPI_Comm comm, MPI_Status *sts) noexcept;
  void sendrecv_replace(void* buf, int count, MPI_Datatype datatype,
                        int dest, int sendtag, int source, int recvtag,
                        MPI_Comm comm, MPI_Status *status) noexcept;
  CMI_FORCE_INLINE void probe(int t,int s,MPI_Comm comm,MPI_Status *sts) noexcept {
    return static_probe(this, t, s, comm, sts);
  }
  CMI_FORCE_INLINE void mprobe(int t, int s, MPI_Comm comm, MPI_Status *sts, MPI_Message *message) noexcept {
    return static_mprobe(this, t, s, comm, sts, message);
  }
  int iprobe(int t,int s,MPI_Comm comm,MPI_Status *sts) noexcept;
  int improbe(int t, int s, MPI_Comm comm, MPI_Status *sts, MPI_Message *message) noexcept;
  CMI_WARN_UNUSED_RESULT ampi * barrier() noexcept;
  CMI_WARN_UNUSED_RESULT ampi * block() noexcept;
  CMI_WARN_UNUSED_RESULT ampi * yield() noexcept;
  void ibarrier(MPI_Request *request) noexcept;
  void bcast(int root, void* buf, int count, MPI_Datatype type, MPI_Comm comm) noexcept;
  int intercomm_bcast(int root, void* buf, int count, MPI_Datatype type, MPI_Comm intercomm) noexcept;
  void ibcast(int root, void* buf, int count, MPI_Datatype type, MPI_Comm comm, MPI_Request* request) noexcept;
  int intercomm_ibcast(int root, void* buf, int count, MPI_Datatype type, MPI_Comm intercomm, MPI_Request *request) noexcept;
  static void bcastraw(void* buf, int len, CkArrayID aid) noexcept;
  void split(int color,int key,MPI_Comm *dest, AmpiCommType type) noexcept;
  MPI_Comm commCreate(const std::vector<int>& vec, AmpiCommType type) noexcept;
  MPI_Comm cartCreate0D() noexcept;
  MPI_Comm cartCreate(std::vector<int>& vec, int ndims, const int* dims) noexcept;
  MPI_Comm intercommCreate(const std::vector<int>& rvec, int root, MPI_Comm tcomm) noexcept;

  inline bool isInter() const noexcept { return myComm->isinter(); }
  void intercommMerge(int first, MPI_Comm *ncomm) noexcept;

  inline ampiParent* getParent() const noexcept { return parent; }
  inline int getWorldRank() const noexcept {return parent->thisIndex;}
  /// Return our rank in this communicator
  inline int getRank() const noexcept {return myRank;}
  inline int getSize() const noexcept {return myComm->getSize();}
  inline MPI_Comm getComm() const noexcept {return comm;}
  inline void setCommName(const char *name) noexcept {myComm->setName(name);}
  inline void getCommName(char *name, int *len) const noexcept {myComm->getName(name,len);}
  inline std::vector<int> getIndices() const noexcept { return myComm->getIndices(); }
  inline std::vector<int> getRemoteIndices() const noexcept { return myComm->getRemoteIndices(); }
  inline const CProxy_ampi &getProxy() const noexcept {return thisProxy;}
  inline const CProxy_ampi &getRemoteProxy() const noexcept {return remoteProxy;}
  inline void setRemoteProxy(CProxy_ampi rproxy) noexcept { remoteProxy = rproxy; }
  inline int getIndexForRank(int r) const noexcept {return myComm->getIndexForRank(r);}
  inline int getIndexForRemoteRank(int r) const noexcept {return myComm->getIndexForRemoteRank(r);}
  void findNeighbors(MPI_Comm comm, int rank, std::vector<int>& neighbors) const noexcept;
  inline void sortNeighborsByLocality(std::vector<int>& nbors) noexcept { myComm->getTopology()->sortnbors(thisProxy, nbors); }
  inline std::vector<int>& getNeighbors() noexcept { return myComm->getTopology()->getnbors(); }
  inline const std::vector<int>& getNeighbors() const noexcept { return myComm->getTopology()->getnbors(); }
  inline bool opIsCommutative(MPI_Op op) const noexcept { return parent->opIsCommutative(op); }
  inline MPI_User_function* op2User_function(MPI_Op op) const noexcept { return parent->op2User_function(op); }
  void topoDup(int topoType, int rank, MPI_Comm comm, MPI_Comm *newcomm) noexcept;

  inline AmpiRequestList& getReqs() noexcept { return parent->ampiReqs; }
  CkDDT *getDDT() noexcept {return &parent->myDDT;}
  CthThread getThread() const noexcept { return thread->getThread(); }

 public:
  MPI_Win createWinInstance(void *base, MPI_Aint size, int disp_unit, MPI_Info info) noexcept;
  int deleteWinInstance(MPI_Win win) noexcept;
  int winGetGroup(WinStruct *win, MPI_Group *group) const noexcept;
  int winPut(const void *orgaddr, int orgcnt, MPI_Datatype orgtype, int rank,
             MPI_Aint targdisp, int targcnt, MPI_Datatype targtype, WinStruct *win) noexcept;
  int winGet(void *orgaddr, int orgcnt, MPI_Datatype orgtype, int rank,
             MPI_Aint targdisp, int targcnt, MPI_Datatype targtype, WinStruct *win) noexcept;
  int winIget(MPI_Aint orgdisp, int orgcnt, MPI_Datatype orgtype, int rank,
              MPI_Aint targdisp, int targcnt, MPI_Datatype targtype, WinStruct *win,
              MPI_Request *req) noexcept;
  int winIgetWait(MPI_Request *request, MPI_Status *status) noexcept;
  int winIgetFree(MPI_Request *request, MPI_Status *status) noexcept;
  void winRemotePut(int orgtotalsize, char* orgaddr, int orgcnt, MPI_Datatype orgtype,
                    MPI_Aint targdisp, int targcnt, MPI_Datatype targtype, int winIndex) noexcept;
  char* winLocalGet(int orgcnt, MPI_Datatype orgtype, MPI_Aint targdisp, int targcnt,
                    MPI_Datatype targtype, int winIndex) noexcept;
  AmpiMsg* winRemoteGet(int orgcnt, MPI_Datatype orgtype, MPI_Aint targdisp,
                    int targcnt, MPI_Datatype targtype, int winIndex) noexcept;
  AmpiMsg* winRemoteIget(MPI_Aint orgdisp, int orgcnt, MPI_Datatype orgtype, MPI_Aint targdisp,
                         int targcnt, MPI_Datatype targtype, int winIndex) noexcept;
  int winLock(int lock_type, int rank, WinStruct *win) noexcept;
  int winUnlock(int rank, WinStruct *win) noexcept;
  void winRemoteLock(int lock_type, int winIndex, int requestRank) noexcept;
  void winRemoteUnlock(int winIndex, int requestRank) noexcept;
  int winAccumulate(const void *orgaddr, int orgcnt, MPI_Datatype orgtype, int rank,
                    MPI_Aint targdisp, int targcnt, MPI_Datatype targtype,
                    MPI_Op op, WinStruct *win) noexcept;
  void winRemoteAccumulate(int orgtotalsize, char* orgaddr, int orgcnt, MPI_Datatype orgtype,
                           MPI_Aint targdisp, int targcnt, MPI_Datatype targtype,
                           MPI_Op op, int winIndex) noexcept;
  int winGetAccumulate(const void *orgaddr, int orgcnt, MPI_Datatype orgtype, void *resaddr,
                       int rescnt, MPI_Datatype restype, int rank, MPI_Aint targdisp,
                       int targcnt, MPI_Datatype targtype, MPI_Op op, WinStruct *win) noexcept;
  void winLocalGetAccumulate(int orgtotalsize, char* sorgaddr, int orgcnt, MPI_Datatype orgtype,
                             MPI_Aint targdisp, int targcnt, MPI_Datatype targtype, MPI_Op op,
                             char *resaddr, int winIndex) noexcept;
  AmpiMsg* winRemoteGetAccumulate(int orgtotalsize, char* sorgaddr, int orgcnt, MPI_Datatype orgtype,
                                  MPI_Aint targdisp, int targcnt, MPI_Datatype targtype, MPI_Op op,
                                  int winIndex) noexcept;
  int winCompareAndSwap(const void *orgaddr, const void *compaddr, void *resaddr, MPI_Datatype type,
                        int rank, MPI_Aint targdisp, WinStruct *win) noexcept;
  char* winLocalCompareAndSwap(int size, char* sorgaddr, char* compaddr, MPI_Datatype type,
                               MPI_Aint targdisp, int winIndex) noexcept;
  AmpiMsg* winRemoteCompareAndSwap(int size, char *sorgaddr, char *compaddr, MPI_Datatype type,
                                   MPI_Aint targdisp, int winIndex) noexcept;
  void winSetName(WinStruct *win, const char *name) noexcept;
  void winGetName(WinStruct *win, char *name, int *length) const noexcept;
  win_obj* getWinObjInstance(WinStruct *win) const noexcept;
  int getNewSemaId() noexcept;

  int intercomm_scatter(int root, const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                        void *recvbuf, int recvcount, MPI_Datatype recvtype, MPI_Comm intercomm) noexcept;
  int intercomm_iscatter(int root, const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                         void *recvbuf, int recvcount, MPI_Datatype recvtype,
                         MPI_Comm intercomm, MPI_Request *request) noexcept;
  int intercomm_scatterv(int root, const void* sendbuf, const int* sendcounts, const int* displs,
                         MPI_Datatype sendtype, void* recvbuf, int recvcount,
                         MPI_Datatype recvtype, MPI_Comm intercomm) noexcept;
  int intercomm_iscatterv(int root, const void* sendbuf, const int* sendcounts, const int* displs,
                          MPI_Datatype sendtype, void* recvbuf, int recvcount,
                          MPI_Datatype recvtype, MPI_Comm intercomm, MPI_Request* request) noexcept;
};

CMI_WARN_UNUSED_RESULT ampiParent *getAmpiParent() noexcept;
bool isAmpiThread() noexcept;
CMI_WARN_UNUSED_RESULT ampi *getAmpiInstance(MPI_Comm comm) noexcept;
void checkComm(MPI_Comm comm) noexcept;
void checkRequest(MPI_Request req) noexcept;
void handle_MPI_BOTTOM(void* &buf, MPI_Datatype type) noexcept;
void handle_MPI_BOTTOM(void* &buf1, MPI_Datatype type1, void* &buf2, MPI_Datatype type2) noexcept;

#if AMPI_ERROR_CHECKING
int ampiErrhandler(const char* func, int errcode) noexcept;
#else
#define ampiErrhandler(func, errcode) (errcode)
#endif


#if CMK_TRACE_ENABLED

// List of AMPI functions to trace:
static const char *funclist[] = {"AMPI_Abort", "AMPI_Add_error_class", "AMPI_Add_error_code", "AMPI_Add_error_string",
"AMPI_Address", "AMPI_Allgather", "AMPI_Allgatherv", "AMPI_Allreduce", "AMPI_Alltoall",
"AMPI_Alltoallv", "AMPI_Alltoallw", "AMPI_Attr_delete", "AMPI_Attr_get",
"AMPI_Attr_put", "AMPI_Barrier", "AMPI_Bcast", "AMPI_Bsend", "AMPI_Cancel",
"AMPI_Cart_coords", "AMPI_Cart_create", "AMPI_Cart_get", "AMPI_Cart_map",
"AMPI_Cart_rank", "AMPI_Cart_shift", "AMPI_Cart_sub", "AMPI_Cartdim_get",
"AMPI_Comm_call_errhandler", "AMPI_Comm_compare", "AMPI_Comm_create", "AMPI_Comm_create_group",
"AMPI_Comm_create_errhandler", "AMPI_Comm_create_keyval", "AMPI_Comm_delete_attr",
"AMPI_Comm_dup", "AMPI_Comm_dup_with_info", "AMPI_Comm_free",
"AMPI_Comm_free_errhandler", "AMPI_Comm_free_keyval", "AMPI_Comm_get_attr",
"AMPI_Comm_get_errhandler", "AMPI_Comm_get_info", "AMPI_Comm_get_name",
"AMPI_Comm_group", "AMPI_Comm_rank", "AMPI_Comm_remote_group", "AMPI_Comm_remote_size",
"AMPI_Comm_set_attr", "AMPI_Comm_set_errhandler", "AMPI_Comm_set_info", "AMPI_Comm_set_name",
"AMPI_Comm_size", "AMPI_Comm_split", "AMPI_Comm_split_type", "AMPI_Comm_test_inter",
"AMPI_Dims_create", "AMPI_Dist_graph_create", "AMPI_Dist_graph_create_adjacent",
"AMPI_Dist_graph_neighbors", "AMPI_Dist_graph_neighbors_count",
"AMPI_Errhandler_create", "AMPI_Errhandler_free", "AMPI_Errhandler_get",
"AMPI_Errhandler_set", "AMPI_Error_class", "AMPI_Error_string", "AMPI_Exscan", "AMPI_Finalize",
"AMPI_Finalized", "AMPI_Gather", "AMPI_Gatherv", "AMPI_Get_address", "AMPI_Get_count",
"AMPI_Get_elements", "AMPI_Get_library_version", "AMPI_Get_processor_name", "AMPI_Get_version",
"AMPI_Graph_create", "AMPI_Graph_get", "AMPI_Graph_map", "AMPI_Graph_neighbors",
"AMPI_Graph_neighbors_count", "AMPI_Graphdims_get", "AMPI_Group_compare", "AMPI_Group_difference",
"AMPI_Group_excl", "AMPI_Group_free", "AMPI_Group_incl", "AMPI_Group_intersection",
"AMPI_Group_range_excl", "AMPI_Group_range_incl", "AMPI_Group_rank", "AMPI_Group_size",
"AMPI_Group_translate_ranks", "AMPI_Group_union", "AMPI_Iallgather", "AMPI_Iallgatherv",
"AMPI_Iallreduce", "AMPI_Ialltoall", "AMPI_Ialltoallv", "AMPI_Ialltoallw", "AMPI_Ibarrier",
"AMPI_Ibcast", "AMPI_Iexscan", "AMPI_Igather", "AMPI_Igatherv", "AMPI_Ineighbor_allgather",
"AMPI_Ineighbor_allgatherv", "AMPI_Ineighbor_alltoall", "AMPI_Ineighbor_alltoallv",
"AMPI_Ineighbor_alltoallw", "AMPI_Init", "AMPI_Init_thread", "AMPI_Initialized", "AMPI_Intercomm_create",
"AMPI_Intercomm_merge", "AMPI_Iprobe", "AMPI_Irecv", "AMPI_Ireduce", "AMPI_Ireduce_scatter",
"AMPI_Ireduce_scatter_block", "AMPI_Is_thread_main", "AMPI_Iscan", "AMPI_Iscatter", "AMPI_Iscatterv",
"AMPI_Isend", "AMPI_Issend", "AMPI_Keyval_create", "AMPI_Keyval_free", "AMPI_Neighbor_allgather",
"AMPI_Neighbor_allgatherv", "AMPI_Neighbor_alltoall", "AMPI_Neighbor_alltoallv", "AMPI_Neighbor_alltoallw",
"AMPI_Op_commutative", "AMPI_Op_create", "AMPI_Op_free", "AMPI_Pack", "AMPI_Pack_size",
"AMPI_Pcontrol", "AMPI_Probe", "AMPI_Query_thread", "AMPI_Recv", "AMPI_Recv_init", "AMPI_Reduce",
"AMPI_Reduce_local", "AMPI_Reduce_scatter", "AMPI_Reduce_scatter_block", "AMPI_Request_free",
"AMPI_Request_get_status", "AMPI_Rsend", "AMPI_Scan", "AMPI_Scatter", "AMPI_Scatterv", "AMPI_Send",
"AMPI_Send_init",  "AMPI_Sendrecv", "AMPI_Sendrecv_replace", "AMPI_Ssend", "AMPI_Ssend_init",
"AMPI_Start", "AMPI_Startall", "AMPI_Status_set_cancelled", "AMPI_Status_set_elements", "AMPI_Test",
"AMPI_Test_cancelled", "AMPI_Testall", "AMPI_Testany", "AMPI_Testsome", "AMPI_Topo_test",
"AMPI_Type_commit", "AMPI_Type_contiguous", "AMPI_Type_create_hindexed",
"AMPI_Type_create_hindexed_block", "AMPI_Type_create_hvector", "AMPI_Type_create_indexed_block",
"AMPI_Type_create_keyval", "AMPI_Type_create_resized", "AMPI_Type_create_struct",
"AMPI_Type_delete_attr", "AMPI_Type_dup", "AMPI_Type_extent", "AMPI_Type_free",
"AMPI_Type_free_keyval", "AMPI_Type_get_attr", "AMPI_Type_get_contents", "AMPI_Type_get_envelope",
"AMPI_Type_get_extent", "AMPI_Type_get_name", "AMPI_Type_get_true_extent", "AMPI_Type_hindexed",
"AMPI_Type_hvector", "AMPI_Type_indexed", "AMPI_Type_lb", "AMPI_Type_set_attr",
"AMPI_Type_set_name", "AMPI_Type_size", "AMPI_Type_struct", "AMPI_Type_ub", "AMPI_Type_vector",
"AMPI_Type_create_darray", "AMPI_Type_create_subarray",
"AMPI_Unpack", "AMPI_Wait", "AMPI_Waitall", "AMPI_Waitany", "AMPI_Waitsome", "AMPI_Wtick", "AMPI_Wtime",
"AMPI_Accumulate", "AMPI_Compare_and_swap", "AMPI_Fetch_and_op", "AMPI_Get", "AMPI_Get_accumulate",
"AMPI_Info_create", "AMPI_Info_delete", "AMPI_Info_dup", "AMPI_Info_free", "AMPI_Info_get",
"AMPI_Info_get_nkeys", "AMPI_Info_get_nthkey", "AMPI_Info_get_valuelen",
"AMPI_Info_set", "AMPI_Put", "AMPI_Raccumulate", "AMPI_Rget", "AMPI_Rget_accumulate",
"AMPI_Rput", "AMPI_Win_complete", "AMPI_Win_create", "AMPI_Win_create_errhandler",
"AMPI_Win_create_keyval", "AMPI_Win_delete_attr", "AMPI_Win_fence", "AMPI_Win_free",
"AMPI_Win_free_keyval", "AMPI_Win_get_attr", "AMPI_Win_get_errhandler",
"AMPI_Win_get_group", "AMPI_Win_get_info", "AMPI_Win_get_name", "AMPI_Win_lock",
"AMPI_Win_post", "AMPI_Win_set_attr", "AMPI_Win_set_errhandler", "AMPI_Win_set_info",
"AMPI_Win_set_name", "AMPI_Win_start", "AMPI_Win_test", "AMPI_Win_unlock",
"AMPI_Win_wait", "AMPI_Exit" /*AMPI extensions:*/, "AMPI_Migrate",
"AMPI_Load_start_measure", "AMPI_Load_stop_measure",
"AMPI_Load_set_value", "AMPI_Migrate_to_pe", "AMPI_Set_migratable",
"AMPI_Register_pup", "AMPI_Get_pup_data",
"AMPI_Register_about_to_migrate", "AMPI_Register_just_migrated",
"AMPI_Iget", "AMPI_Iget_wait", "AMPI_Iget_free", "AMPI_Iget_data",
"AMPI_Type_is_contiguous", "AMPI_Yield", "AMPI_Suspend",
"AMPI_Resume", "AMPI_Print", "AMPI_Alltoall_medium",
"AMPI_Alltoall_long", "AMPI_System"};

// not traced: AMPI_Trace_begin, AMPI_Trace_end

#endif // CMK_TRACE_ENABLED

extern bool ampi_nodeinit_has_been_called;
inline void ampiVerifyNodeinit(const char * routineName)
{
  if (!ampi_nodeinit_has_been_called)
  { /* Charm hasn't been started yet! */
    CkAbort("%s> AMPI has not been initialized! Possibly due to AMPI requiring '#include \"mpi.h\" be in the same file as main() in C/C++ programs and \'program main\' be renamed to \'subroutine mpi_main\' in Fortran programs!", routineName);
  }
}

//Use this to mark the start of AMPI interface routines that can only be called on AMPI threads:
#if CMK_ERROR_CHECKING
#define AMPI_API(routineName, ...) \
  if (!isAmpiThread()) { CkAbort("AMPI> cannot call MPI routines from non-AMPI threads!"); } \
  TCHARM_API_TRACE(routineName, "ampi"); AMPI_DEBUG_ARGS(routineName, __VA_ARGS__)
#else
#define AMPI_API(routineName, ...) TCHARM_API_TRACE(routineName, "ampi"); \
  AMPI_DEBUG_ARGS(routineName, __VA_ARGS__) 
#endif

//Use this for MPI_Init and routines than can be called before AMPI threads have been initialized:
#define AMPI_API_INIT(routineName, ...) ampiVerifyNodeinit(routineName); \
  TCHARM_API_TRACE(routineName, "ampi"); \
  AMPI_DEBUG_ARGS(routineName, __VA_ARGS__)


#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
# define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
# define NOMINMAX
#endif
#include <windows.h>

typedef HMODULE SharedObject;

#define dlopen(name, flags) LoadLibrary(name)
#define dlsym(handle, name) ((void(*)())GetProcAddress((handle), (name)))
#define dlclose(handle) FreeLibrary(handle)
#define dlerror() ""

#else

#if CMK_DLL_USE_DLOPEN
#ifndef _GNU_SOURCE
# define _GNU_SOURCE
#endif
#ifndef __USE_GNU
# define __USE_GNU
#endif
#include <dlfcn.h>
#endif

typedef void * SharedObject;

#endif

typedef int (*ampi_maintype)(int, char **);
typedef void (*ampi_fmaintype)(void);

struct ampi_mainstruct
{
  ampi_maintype c;
  ampi_fmaintype f;
};

ampi_mainstruct AMPI_Main_Get(SharedObject myexe);
int AMPI_Main_Dispatch(ampi_mainstruct, int argc, char ** argv);

/* For internal AMPI use only: semantics subject to change. */
CLINKAGE void AMPI_Node_Setup(int numranks);
CLINKAGE void AMPI_Rank_Setup(int myrank, int numranks, CmiIsomallocContext ctx);

#endif // _AMPIIMPL_H
