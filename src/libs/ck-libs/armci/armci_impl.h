#ifndef _ARMCI_IMPL_H
#define _ARMCI_IMPL_H

#include <vector>
using std::vector;

#include <stdlib.h>
#include <math.h>

#include "tcharmc.h"
#include "tcharm.h"

//Types needed for remote method parameters:
typedef void* pointer;
PUPbytes(pointer) //Pointers get sent as raw bytes

#include "armci.decl.h"
#include "armci.h"

/* Operations for Armci_Hdl */
#define ARMCI_INVALID	0x0
#define ARMCI_GET 	0x1
#define ARMCI_PUT 	0x2
#define ARMCI_ACC 	0x3
#define ARMCI_BGET      0x5
#define ARMCI_BPUT	0x6
#define ARMCI_BACC 	0x7

#define ARMCI_IGET      0x9  // implicit get
#define ARMCI_IPUT      0xa  // implicit put

#define BLOCKING_MASK	0x4
#define IMPLICIT_MASK   0x8  // anything that is implicit is non-blocking

class Armci_Hdl {
public:
   int op;
   int proc;
   int nbytes;
   int acked;
   int wait;    // Is WaitProc or WaitAll waiting for me?
   pointer src;
   pointer dst;
   
 Armci_Hdl() : op(ARMCI_INVALID), proc(-1), nbytes(0), acked(0), wait(0), src(NULL), dst(NULL) 
   	{ }
   Armci_Hdl(int o, int p, int n, pointer s, pointer d):
   op(o), proc(p), nbytes(n), acked(0), wait(0), src(s), dst(d) { }
   void pup(PUP::er &p){
     p|op; p|proc; p|nbytes; p|acked; p|wait; p|src; p|dst;	
   }
};

// Support for ARMCI notify feature designed for Co-Array Fortran. Should
//   be orthogonal to wait/fence operations.
class Armci_Note{
public:
  int proc;
  int waited;
  int notified;
  Armci_Note() : proc(-1), waited(0), notified(0) { }
  Armci_Note(int p, int w, int n) : proc(p), waited(w), notified(n) { }
  void pup(PUP::er &p){ p|proc; p|waited; p|notified; }
};

// structure definitions and forward declarations (for reductions)
typedef struct peAddr {
  int pe;
  pointer ptr;
} addressPair;

extern CkArrayID armciVPAid;

#define ARMCI_TCHARM_SEMAID 0x00A53C10 /* __ARMCI_ */

/* Support for ARMCI reduction types */
#define _ARMCI_NUM_REDN_OPS 6
#define _ARMCI_REDN_OP_SUM 0
#define _ARMCI_REDN_OP_PRODUCT 1
#define _ARMCI_REDN_OP_MIN 2
#define _ARMCI_REDN_OP_MAX 3
#define _ARMCI_REDN_OP_ABSMIN 4
#define _ARMCI_REDN_OP_ABSMAX 5

// **CWL** WARNING: Macro-Generated code inspired by Charm++ reduction code.
//   The way Charm++ uses ::buildNew appears to be an undocumented
//   optimization. I am retaining the use of the documented version
//   until the time we need to consider performance.

#define _ARMCI_TEMPLATE_REDUCTION(name,dataType,reductionWork) \
  CkReductionMsg *name(int nMsg,CkReductionMsg **msg)  \
  { \
    int m,i;\
    int nElem=msg[0]->getLength()/sizeof(dataType);\
    dataType *ret=(dataType *)(msg[0]->getData());\
    for (m=1;m<nMsg;m++) {\
      dataType *value=(dataType *)(msg[m]->getData());\
      for (i=0;i<nElem;i++) {\
        reductionWork\
      }\
    }\
    return CkReductionMsg::buildNew(nElem*sizeof(dataType),ret);\
  }

#define _ARMCI_GENERATE_POLYMORPHIC_REDUCTION(opName,reductionWork) \
  _ARMCI_TEMPLATE_REDUCTION(opName##_int,int,reductionWork) \
  _ARMCI_TEMPLATE_REDUCTION(opName##_long,long,reductionWork) \
  _ARMCI_TEMPLATE_REDUCTION(opName##_longlong,long long,reductionWork) \
  _ARMCI_TEMPLATE_REDUCTION(opName##_float,float,reductionWork) \
  _ARMCI_TEMPLATE_REDUCTION(opName##_double,double,reductionWork)

#if defined(_WIN32)
#define llabs(x) abs(x)
#endif

#if ! CMK_HAS_FABSF
inline float fabsf(float x)
{
  return x>0.0?x:-x;
}
#endif

/* 
   **CWL** The absolute function is inconveniently unpolymorphic in the
   standard C++ and math libraries.
*/
#define _ARMCI_GENERATE_ABS_REDUCTION() \
  _ARMCI_TEMPLATE_REDUCTION(absmax_int,int,if (ret[i]<abs(value[i])) ret[i]=value[i];) \
  _ARMCI_TEMPLATE_REDUCTION(absmax_long,long,if (ret[i]<labs(value[i])) ret[i]=value[i];) \
  _ARMCI_TEMPLATE_REDUCTION(absmax_longlong,long long,if (ret[i]<labs(value[i])) ret[i]=value[i];) \
  _ARMCI_TEMPLATE_REDUCTION(absmax_float,float,if (ret[i]<fabsf(value[i])) ret[i]=value[i];) \
  _ARMCI_TEMPLATE_REDUCTION(absmax_double,double,if (ret[i]<fabs(value[i])) ret[i]=value[i];) \
  _ARMCI_TEMPLATE_REDUCTION(absmin_int,int,if (ret[i]>abs(value[i])) ret[i]=value[i];) \
  _ARMCI_TEMPLATE_REDUCTION(absmin_long,long,if (ret[i]>labs(value[i])) ret[i]=value[i];) \
  _ARMCI_TEMPLATE_REDUCTION(absmin_longlong,long long,if (ret[i]>labs(value[i])) ret[i]=value[i];) \
  _ARMCI_TEMPLATE_REDUCTION(absmin_float,float,if (ret[i]>fabsf(value[i])) ret[i]=value[i];) \
  _ARMCI_TEMPLATE_REDUCTION(absmin_double,double,if (ret[i]>fabs(value[i])) ret[i]=value[i];) 

#define _ARMCI_REGISTER_REDUCTION(fnName,dataType,op) \
  _armciRednLookupTable[op][dataType] = \
    CkReduction::addReducer(fnName);

// The following names have to match those generated by 
//    _ARMCI_GENERATE_POLYMORPHIC_REDUCTION and
//    _ARMCI_GENERATE_ABS_REDUCTION
#define _ARMCI_REGISTER_POLYMORPHIC_REDUCTION(opName, op) \
  _ARMCI_REGISTER_REDUCTION(opName##_int,ARMCI_INT,op) \
  _ARMCI_REGISTER_REDUCTION(opName##_long,ARMCI_LONG,op) \
  _ARMCI_REGISTER_REDUCTION(opName##_longlong,ARMCI_LONG_LONG,op) \
  _ARMCI_REGISTER_REDUCTION(opName##_float,ARMCI_FLOAT,op) \
  _ARMCI_REGISTER_REDUCTION(opName##_double,ARMCI_DOUBLE,op) 
  
class ArmciMsg : public CMessage_ArmciMsg {
public:
  pointer dst; 
  int nbytes;
  int src_proc;
  int hdl;
  char *data;
  
  ArmciMsg(void) { data = NULL; }
  ArmciMsg(pointer d, int n, int s, int h) :
    dst(d), nbytes(n), src_proc(s), hdl(h) { }
  static ArmciMsg* pup(PUP::er &p, ArmciMsg *m){
    pointer d=NULL;
    int n=0, s=0, h=0;
    if(p.isPacking() || p.isSizing()){
      d = m->dst;
      n = m->nbytes;
      s = m->src_proc;
      h = m->hdl;
    }
    p|d; p|n; p|s; p|h;
    if(p.isUnpacking()){
      m = new (n, 0) ArmciMsg(d,n,s,h);
    }
    p(m->data,n);
    if(p.isDeleting()){
      delete m;
      m = NULL;
    }
    return m;
  }
};

class ArmciStridedMsg : public CMessage_ArmciStridedMsg {
public:
  pointer dst; 
  int stride_levels;
  int nbytes;
  int src_proc;
  int hdl;
  int *dst_stride_ar;
  int *count;
  char *data;
  
  ArmciStridedMsg(void) { dst_stride_ar = NULL; count = NULL; data = NULL; }
  ArmciStridedMsg(pointer d, int l, int n, int s, int h) :
    dst(d), stride_levels(l), nbytes(n), src_proc(s), hdl(h) { }
  static ArmciStridedMsg* pup(PUP::er &p, ArmciStridedMsg *m){
    pointer d=NULL;
    int l=0, n=0, s=0, h=0;
    if(p.isPacking() || p.isSizing()){
      d = m->dst;
      l = m->stride_levels;
      n = m->nbytes;
      s = m->src_proc;
      h = m->hdl;
    }
    p|d; p|l; p|n; p|s; p|h;
    if(p.isUnpacking()){
      m = new (l,l+1,n, 0) ArmciStridedMsg(d,l,n,s,h);
    }
    p((char *)(m->dst_stride_ar),sizeof(int)*l);
    p((char *)(m->count),sizeof(int)*(l+1));
    p(m->data,n);
    if(p.isDeleting()){
      delete m;
      m = NULL;
    }
    return m;
  }
};

// virtual processor class declaration
// ARMCI is supposed to be platform neutral, so calling this a thread did
// not seem like a proper abstraction.
class ArmciVirtualProcessor : public TCharmClient1D {
  CProxy_ArmciVirtualProcessor thisProxy;
  AddressMsg *addressReply;
  CkPupPtrVec<Armci_Hdl> hdlList;
  CkPupPtrVec<Armci_Note> noteList;

  void *collectiveTmpBufferPtr;
 protected:
  virtual void setupThreadPrivate(CthThread forThread);
 public:
  ArmciVirtualProcessor(const CProxy_TCharm &_thr_proxy);
  ArmciVirtualProcessor(CkMigrateMessage *m);
  ~ArmciVirtualProcessor();
 
  pointer BlockMalloc(int bytes) { 
    CmiIsomallocContext ctx = CmiIsomallocGetThreadContext(thread->getThread());
    if (ctx.opaque == nullptr)
      return malloc(bytes);
    return CmiIsomallocContextMalloc(ctx, bytes);
  }
  void BlockFree(void * ptr) {
    CmiIsomallocContext ctx = CmiIsomallocGetThreadContext(thread->getThread());
    if (ctx.opaque == nullptr)
      free(ptr);
    else
      CmiIsomallocContextFree(ctx, ptr);
  }
  void getAddresses(AddressMsg *msg);

  void put(pointer src, pointer dst, int bytes, int dst_proc);
  void putData(pointer dst, int nbytes, char *data, int src_proc, int hdl);
  void putData(ArmciMsg* msg);
  void putAck(int hdl);
  int nbput(pointer src, pointer dst, int bytes, int dst_proc);
  void nbput_implicit(pointer src, pointer dst, int bytes, int dst_proc);
  void wait(int hdl);
  int test(int hdl);
  void waitmulti(vector<int> procs);
  void waitproc(int proc);
  void waitall();
  void fence(int proc);
  void allfence();
  void barrier();
  
  void get(pointer src, pointer dst, int bytes, int src_proc);
  int nbget(pointer src, pointer dst, int bytes, int dst_proc);
  void nbget_implicit(pointer src, pointer dst, int bytes, int dst_proc);
  void requestFromGet(pointer src, pointer dst, int nbytes, int dst_proc, int hdl);
  void putDataFromGet(pointer dst, int nbytes, char *data, int hdl);
  void putDataFromGet(ArmciMsg* msg);

  void puts(pointer src_ptr, int src_stride_ar[], 
	   pointer dst_ptr, int dst_stride_ar[],
	   int count[], int stride_levels, int dst_proc);
  int nbputs(pointer src_ptr, int src_stride_ar[], 
	   pointer dst_ptr, int dst_stride_ar[],
	   int count[], int stride_levels, int dst_proc);
  void nbputs_implicit(pointer src_ptr, int src_stride_ar[], 
		       pointer dst_ptr, int dst_stride_ar[],
		       int count[], int stride_levels, int dst_proc);
  void putsData(pointer dst_ptr, int dst_stride_ar[], 
  		int count[], int stride_levels,
		int nbytes, char *data, int src_proc, int hdl);
  void putsData(ArmciStridedMsg *m);
  
  void gets(pointer src_ptr, int src_stride_ar[], 
	   pointer dst_ptr, int dst_stride_ar[],
	   int count[], int stride_levels, int src_proc);
  int nbgets(pointer src_ptr, int src_stride_ar[], 
	   pointer dst_ptr, int dst_stride_ar[],
	   int count[], int stride_levels, int src_proc);
  void nbgets_implicit(pointer src_ptr, int src_stride_ar[], 
		       pointer dst_ptr, int dst_stride_ar[],
		       int count[], int stride_levels, int src_proc);
  void requestFromGets(pointer src_ptr, int src_stride_ar[], 
	   pointer dst_ptr, int dst_stride_ar[],
	   int count[], int stride_levels, int dst_proc, int hdl);
  void putDataFromGets(pointer dst_ptr, int dst_stride_ar[], 
  		int count[], int stride_levels,
		int nbytes, char *data, int hdl);
  void putDataFromGets(ArmciStridedMsg *m);

  void notify(int proc);
  void sendNote(int proc);
  void notify_wait(int proc);

  // non-entry methods. Mainly interfaces to API interface methods.
  void requestAddresses(pointer  ptr, pointer ptr_arr[], int bytes);
  void stridedCopy(void *base, void *buffer_ptr,
		  int *stride, int *count, 
		  int dim_id, bool flatten);
  virtual void pup(PUP::er &p);

  // collective operations for CAF
  void msgBcast(void *buffer, int len, int root);
  void recvMsgBcast(int len, char *buffer, int root);
  void msgGop(void *x, int n, char *op, int type);

  void mallocClient(CkReductionMsg *msg);
  void resumeThread(void);
  void startCheckpoint(const char* dname);
  void checkpoint(int len, const char* dname);
};

class AddressMsg : public CMessage_AddressMsg {
 public:
  pointer *addresses;
  friend class CMessage_AddressMsg;
};

// pointer to the current tcshmem thread. Needed to regain context after
// getting called by user.
CtvExtern(ArmciVirtualProcessor *, _armci_ptr);


#if CMK_TRACE_ENABLED
// List of ARMCI functions to trace:
static const char *funclist[] = {"ARMCI_Init", "ARMCI_Finalize",
 "ARMCI_Error", "ARMCI_Cleanup", "ARMCI_Procs", "ARMCI_Myid",
 "ARMCI_GetV", "ARMCI_NbGetV", "ARMCI_PutV", "ARMCI_NbPutV",
 "ARMCI_AccV", "ARMCI_NbAccV", "ARMCI_Put", "ARMCI_NbPut",
 "ARMCI_Get", "ARMCI_NbGet", "ARMCI_Acc", "ARMCI_NbAcc",
 "ARMCI_PutS", "ARMCI_NbPutS", "ARMCI_GetS", "ARMCI_NbGetS",
 "ARMCI_AccS", "ARMCI_NbAccS", "ARMCI_PutValueLong",
 "ARMCI_PutValueInt", "ARMCI_PutValueFloat", "ARMCI_PutValueDouble",
 "ARMCI_NbPutValueLong", "ARMCI_NbPutValueInt",
 "ARMCI_NbPutValueFloat", "ARMCI_NbPutValueDouble",
 "ARMCI_GetValueLong", "ARMCI_GetValueInt", "ARMCI_GetValueFloat",
 "ARMCI_GetValueDouble", "ARMCI_NbGetValueLong",
 "ARMCI_NbGetValueInt", "ARMCI_NbGetValueFloat",
 "ARMCI_NbGetValueDouble", "ARMCI_Wait", "ARMCI_WaitProc",
 "ARMCI_WaitAll", "ARMCI_Test", "ARMCI_Barrier", "ARMCI_Fence",
 "ARMCI_AllFence", "ARMCI_Malloc", "ARMCI_Free",
 "ARMCI_Malloc_local", "ARMCI_Free_local",
 "ARMCI_SET_AGGREGATE_HANDLE", "ARMCI_UNSET_AGGREGATE_HANDLE",
 "ARMCI_Rmw", "ARMCI_Create_mutexes", "ARMCI_Destroy_mutexes",
 "ARMCI_Lock", "ARMCI_Unlock", "armci_notify", "armci_notify_wait",
 "ARMCI_Migrate", "ARMCI_Async_Migrate", "ARMCI_Checkpoint",
 "ARMCI_MemCheckpoint", "armci_msg_brdcst", "armci_msg_bcast",
 "armci_msg_gop2", "armci_msg_igop", "armci_msg_lgop",
 "armci_msg_fgop", "armci_msg_dgop", "armci_msg_barrier",
 "armci_msg_reduce", "armci_domain_nprocs", "armci_domain_count",
 "armci_domain_id", "armci_domain_glob_proc_id",
 "armci_domain_my_id"};
#endif // CMK_TRACE_ENABLED

#endif // _ARMCI_IMPL_H
