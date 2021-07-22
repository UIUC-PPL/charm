#ifndef _CHARMPP_H_
#define _CHARMPP_H_

#include <stdlib.h>
#include <memory.h>
#include <type_traits>
#include <vector>

#include "charm.h"
#include "middle.h"

class CMessage_CkArgMsg {
public: static int __idx;
};
#define CK_ALIGN(val,to) (((val)+(to)-1)&~((to)-1))

#ifdef __GNUC__
#define UNUSED __attribute__ ((unused))
#else
#define UNUSED
#endif

#include "pup.h"
#include "cklists.h"
#include "ckbitvector.h"
#include "init.h"
#include "debug-charm.h"
#ifndef __CUDACC__
#include "simd.h"
#endif

#include <ckmessage.h>

PUPbytes(CkChareID)
PUPbytes(CkGroupID)
PUPbytes(CmiGroup)

CkpvExtern(size_t *, _offsets);

class CkArray;

//This is for passing a single Charm++ message via parameter marshalling
class CkMarshalledMessage {
	void *msg;
 public:
	CkMarshalledMessage(void): msg(NULL) {}
	CkMarshalledMessage(CkMessage *m): msg(m) {} //Takes ownership of message

        CkMarshalledMessage(const CkMarshalledMessage &) = delete;
	void operator=     (const CkMarshalledMessage &) = delete;
        CkMarshalledMessage(CkMarshalledMessage &&rhs) { msg = rhs.msg; rhs.msg = NULL; }
        void operator=     (CkMarshalledMessage &&rhs) { msg = rhs.msg; rhs.msg = NULL; }
	~CkMarshalledMessage() { if (msg) CkFreeMsg(msg); }

	CkMessage *getMessage(void) {
          void *ret = msg;
          msg = nullptr;
          return (CkMessage *)ret;
        }
	void pup(PUP::er &p) {CkPupMessage(p,&msg,1);}
};

/**
 * CkEntryOptions describes the options associated
 * with an entry method invocation, which include
 * the message priority and queuing strategy.
 * It is only used with parameter marshalling.
 */
class CkEntryOptions : public CkNoncopyable {
	int queueingtype; //CK_QUEUEING type
	int prioBits; //Number of bits of priority to use
	typedef unsigned int prio_t; //Datatype used to represent priorities
	prio_t *prioPtr; //Points to message priority values
	prio_t prioStore; //For short priorities, stores the priority value
	std::vector<CkGroupID> depGroupIDs;  // group dependencies
public:
	CkEntryOptions(void): queueingtype(CK_QUEUEING_FIFO), prioBits(0), 
                              prioPtr(NULL), prioStore(0) {
	}

	~CkEntryOptions() {
		if ( prioPtr != NULL && queueingtype != CK_QUEUEING_IFIFO &&
                     queueingtype != CK_QUEUEING_ILIFO ) {
			delete [] prioPtr;
			prioBits = 0;
		}
	}
	
	inline CkEntryOptions& setPriority(prio_t integerPrio) {
		queueingtype=CK_QUEUEING_IFIFO;
		prioBits=8*sizeof(integerPrio);
		prioPtr=&prioStore;
		prioStore=integerPrio;
        return *this;
	}
	inline CkEntryOptions& setPriority(int prioBits_,const prio_t *prioPtr_) {
		if ( prioPtr != NULL && queueingtype != CK_QUEUEING_IFIFO &&
                     queueingtype != CK_QUEUEING_ILIFO ) {
			delete [] prioPtr;
			prioBits = 0;
		}
		queueingtype=CK_QUEUEING_BFIFO;
		prioBits=prioBits_;
		int dataLength = (prioBits + (sizeof(prio_t)*8 - 1)) /
		                 (sizeof(prio_t)*8);
		prioPtr = new prio_t[dataLength];
		memcpy((void *)prioPtr, prioPtr_, dataLength*sizeof(unsigned int));
        return *this;
	}
	inline CkEntryOptions& setPriority(const CkBitVector &cbv) {
		if ( !cbv.data.empty() ) {
			if ( prioPtr != NULL && queueingtype != CK_QUEUEING_IFIFO &&
                             queueingtype != CK_QUEUEING_ILIFO ) {
				delete [] prioPtr;
				prioBits = 0;
			}
			queueingtype=CK_QUEUEING_BFIFO;
			prioBits=cbv.usedBits;
			int dataLength = (prioBits + (sizeof(prio_t)*8 - 1)) /
		                 	(sizeof(prio_t)*8);
			prioPtr = new prio_t[dataLength];
			memcpy((void *)prioPtr, cbv.data.data(), dataLength*sizeof(prio_t));
		} else {
			queueingtype=CK_QUEUEING_BFIFO;
			prioBits=0;
			int dataLength = 1;
			prioPtr = new prio_t[dataLength];
			prioPtr[0] = 0;
		}
        return *this;
	}
	
	inline CkEntryOptions& setQueueing(int queueingtype_) { queueingtype=queueingtype_; return *this; }
	inline CkEntryOptions& setGroupDepID(const CkGroupID &gid) {
		depGroupIDs.clear();
		depGroupIDs.push_back(gid);
		return *this;
	}
	inline CkEntryOptions& addGroupDepID(const CkGroupID &gid) { depGroupIDs.push_back(gid); return *this; }

	///These are used by CkAllocateMarshallMsg, below:
	inline int getQueueing(void) const {return queueingtype;}
	inline int getPriorityBits(void) const {return prioBits;}
	inline const prio_t *getPriorityPtr(void) const {return prioPtr;}
	inline CkGroupID getGroupDepID() const { return depGroupIDs[0]; }
	inline CkGroupID getGroupDepID(int index) const { return depGroupIDs[index]; }
	inline int getGroupDepSize() const { return sizeof(CkGroupID)*(depGroupIDs.size()); }
	inline int getGroupDepNum() const { return depGroupIDs.size(); }
	inline const CkGroupID *getGroupDepPtr() const { return &(depGroupIDs[0]); }
};

#include "ckmarshall.h"

//A queue-of-messages, like CkMsgQ<CkReductionMsg>
template <class MSG>
class CkMsgQ : public CkQ<MSG *> {
public:
	CkMsgQ() = default;
	CkMsgQ(CkMsgQ&&) = default;
	CkMsgQ(const CkMsgQ&) = delete;
	void operator=(CkMsgQ&&) = delete;
	void operator=(const CkMsgQ&) = delete;
	~CkMsgQ() { //Delete the messages in the queue:
		MSG *m;
		while (NULL!=(m=this->deq())) delete m;
	}
	void pup(PUP::er &p) {
		int l=this->length();
		p(l);
		for (int i=0;i<l;i++) {
			MSG *m=NULL;
			if (!p.isUnpacking()) m=this->deq();
			CkPupMessage(p,(void **)&m);
			this->enq(m);
		}
	}
	friend void operator|(PUP::er &p,CkMsgQ<MSG> &v) {v.pup(p);}
};



#include "ckarrayindex.h"

#include "cksection.h"

#include "ckcallback.h"

#include "ckrdma.h"

#include "ckrdmadevice.h"

/********************* Superclass of all Chares ******************/
#if CMK_MULTIPLE_DELETE
#define CHARM_INPLACE_NEW \
    void *operator new(size_t, void *ptr) { return ptr; }; \
    void operator delete(void*, void*) {}; \
    void *operator new(size_t s) { return malloc(s); } \
    void operator delete(void *ptr) { free(ptr); }
#else
#define CHARM_INPLACE_NEW \
    void *operator new(size_t, void *ptr) { return ptr; }; \
    void *operator new(size_t s) { return malloc(s); } \
    void operator delete(void *ptr) { free(ptr); }
#endif

// for object message queue
#include "ckobjQ.h"
#if CMK_SMP && CMK_TASKQUEUE
#include "conv-taskQ.h"
#endif

#define CHARE_MAGIC    0x201201

/**
  The base class of all parallel objects in Charm++,
  including Array Elements, Groups, and NodeGroups.
*/
class Chare {
  protected:
    CkChareID thishandle;
#if CMK_OBJECT_QUEUE_AVAILABLE
    CkObjectMsgQ objQ;                // object message queue
#endif
  public:
    bool ckInitialized;
#if CMK_ERROR_CHECKING
    int magic;
#endif
#ifndef CMK_CHARE_USE_PTR
    int chareIdx;                  // index in the chare obj table (chare_objs)
#endif
    Chare(CkMigrateMessage *m);
    Chare();
    virtual ~Chare(); //<- needed for *any* child to have a virtual destructor
    virtual bool isLocMgr(void) { return false; }
    /// Pack/UnPack - tell the runtime how to serialize this class's
    /// data for migration, checkpoint, etc.
    virtual void pup(PUP::er &p);
    /// Routine that runtime the runtime actually calls, to enable
    /// more intelligence in generated code that overrides this. The
    /// actual pup() method must remain virtual, so that this
    /// continues to work for older code.
    virtual void virtual_pup(PUP::er &p) { pup(p); }
    void parent_pup(PUP::er &p) {
      (void)p;
      CkAbort("Should never get here - only called in generated CBase code");
    }

    inline const CkChareID &ckGetChareID(void) const {return thishandle;}
    inline void CkGetChareID(CkChareID *dest) const {*dest=thishandle;}
    // object message queue
    void  CkEnableObjQ();
#if CMK_OBJECT_QUEUE_AVAILABLE
    inline CkObjectMsgQ &CkGetObjQueue() { return objQ; }
#endif
    CHARM_INPLACE_NEW
    /// Return the type of this chare, as present in _chareTable
    virtual int ckGetChareType() const;
    /// Return a strdup'd array containing this object's string name.
    virtual char *ckDebugChareName(void);
    /// Place into str a copy of the id of this object up to limit bytes, return
    /// the number of bytes used for the id
    virtual int ckDebugChareID(char *str, int limit);
    virtual void ckDebugPup(PUP::er &p);
    /// Called when a [threaded] charm entry method is created:
    virtual void CkAddThreadListeners(CthThread tid, void *msg);
#if CMK_ERROR_CHECKING
    inline void sanitycheck() { 
        if (magic != CHARE_MAGIC)
          CmiAbort("Charm++ Fatal Error> Chare magic number does not agree, possibly due to pup functions not calling parent class.");
    }
#endif
};

void CkCallstackPop(Chare *obj);
void CkCallstackPush(Chare *obj);
void CkCallstackUnwind(Chare *obj);

Chare *CkActiveObj(void);
Chare *CkAllocateChare(const int &objId);

static inline void CkInvokeEP(Chare *obj, const int &epIdx, void *msg) {
  CkCallstackPush(obj);
  _entryTable[epIdx]->call(msg, obj);
  CkCallstackPop(obj);
}

#if CMK_HAS_IS_CONSTRUCTIBLE
#include <type_traits>

template <typename T, bool has_migration_constructor = std::is_constructible<T, CkMigrateMessage*>::value>
struct call_migration_constructor
{
  call_migration_constructor(void* t);
  void operator()(void* impl_msg);
};

template <typename T>
struct call_migration_constructor<T, true>
{
  void *impl_obj;
  call_migration_constructor(void* t) : impl_obj(t) { }
  void operator()(void* impl_msg)
  {
    new (impl_obj) T((CkMigrateMessage *)impl_msg);
  }
};

template <typename T>
struct call_migration_constructor<T, false>
{
  void *impl_obj;
  call_migration_constructor(void* t) : impl_obj(t) { }
  void operator()(void* impl_msg)
  {
    CkAbort("Attempted to migrate a type that lacks a migration constructor");
  }
};
#else
template <typename T>
struct call_migration_constructor
{
  void *impl_obj;
  call_migration_constructor(void* t) : impl_obj(t) { }
  void operator()(void* impl_msg)
  {
    new (impl_obj) T((CkMigrateMessage *)impl_msg);
  }
};
#endif


//Superclass of all Groups that cannot participate in reductions.
//  Undocumented: should only be used inside Charm++.
/*forward*/ class Group;
class IrrGroup : public Chare {
  protected:
    CkGroupID thisgroup;
  public:
    IrrGroup(CkMigrateMessage *m): Chare(m) { }
    IrrGroup();
    virtual ~IrrGroup(); //<- needed for *any* child to have a virtual destructor

    virtual void pup(PUP::er &p);//<- pack/unpack routine
    virtual void ckJustMigrated(void);
    inline const CkGroupID &ckGetGroupID(void) const {return thisgroup;}
    inline CkGroupID CkGetGroupID(void) const {return thisgroup;}
    virtual int ckGetChareType() const;
    virtual char *ckDebugChareName();
    virtual int ckDebugChareID(char *, int);

    // Silly run-time type information
    virtual bool isNodeGroup() { return false; };
    virtual bool isArrMgr(void){ return false; }
    virtual bool isReductionMgr(void){ return false; }
    static bool isIrreducible(){ return true;}
    virtual void flushStates() {}

    virtual void CkAddThreadListeners(CthThread tid, void *msg);
};

#if CMK_CHARM4PY

extern void (*GroupMsgRecvExtCallback)(int, int, int, char *, int);        // callback to forward received msg to external Group chare
extern void (*ChareMsgRecvExtCallback)(int, void*, int, int, char *, int); // callback to forward received msg to external Chare

/// Supports readonlies outside of the C/C++ runtime. See README.charm4py
class ReadOnlyExt {
public:
  static void *ro_data; // on PE 0, points to the readonly data that is broadcast to every PE
  static size_t data_size;

  static void setData(void *msg, size_t msgSize);
  static void _roPup(void *pup_er); // pack/unpack readonly data
};

/// Lightweight object to support mainchares defined outside of the C/C++ runtime.
/// Relays messages to appropiate external chare. See README.charm4py
class MainchareExt: public Chare {
public:
  MainchareExt(CkArgMsg *m);

  static void __Ctor_CkArgMsg(void *impl_msg, void *impl_obj_void) {
    new (impl_obj_void) MainchareExt((CkArgMsg*)impl_msg);
  }

  static void __entryMethod(void *impl_msg, void *impl_obj_void) {
    //fprintf(stderr, "MainchareExt:: entry method invoked\n");
    MainchareExt *obj = static_cast<MainchareExt *>(impl_obj_void);
    CkMarshallMsg *impl_msg_typed = (CkMarshallMsg *)impl_msg;
    char *impl_buf = impl_msg_typed->msgBuf;
    PUP::fromMem implP(impl_buf);
    int msgSize; implP|msgSize;
    int ep; implP|ep;
    int dcopy_start; implP|dcopy_start;
    // relay msg to external chare
    ChareMsgRecvExtCallback(obj->thishandle.onPE, obj->thishandle.objPtr, ep, msgSize,
                            impl_buf+(3*sizeof(int)), dcopy_start);
  }
};

#endif

/// Base case for the infrastructure to recursively handle inheritance
/// through CBase_foo from anything that implements X::pup(). Chare
/// classes have generated specializations that call PUPs for their
/// parents, SDAG, etc. The default case is an explicit specialization
/// so that corner cases I haven't handled are more likely to produce
/// linker errors, rather than potentially running incorrectly.
///
/// The specialized templates are structs for reasons explained by
/// http://www.gotw.ca/publications/mill17.htm
struct CBase { };
template <typename T, bool automatic>
struct recursive_pup_impl {
  void operator()(T *obj, PUP::er &p);
};

template <typename T>
struct recursive_pup_impl<T, true> {
  void operator()(T *obj, PUP::er &p) {
    obj->parent_pup(p);
    obj->_sdag_pup(p);
    obj->T::pup(p);
  }
};

template <typename T>
struct recursive_pup_impl<T, false> {
  void operator()(T *obj, PUP::er &p) {
    obj->T::pup(p);
  }
};
template <typename T>
void recursive_pup(T *obj, PUP::er &p) {
  recursive_pup_impl<T, std::is_base_of<CBase, T>::value>()(obj, p);
}

class CProxy_ArrayBase;

// CBaseX::pup must be an empty override, so that the recursive PUPing
// doesn't call an implementation multiple times up the inheritance
// hierarchy, and old-style calls to CBase_foo::pup actually produce
// no-ops. We give the prototype for virtual_pup to avoid repetition.
#define CBASE_MEMBERS                                         \
  typedef typename CProxy_Derived::local_t local_t;           \
  typedef typename CProxy_Derived::index_t index_t;           \
  typedef typename CProxy_Derived::proxy_t proxy_t;           \
  typedef typename CProxy_Derived::element_t element_t;       \
  template <typename T = proxy_t>                             \
  typename std::enable_if<std::is_base_of<CProxy_ArrayBase, T>::value>::type migrateMe(int toPe) { \
    this->thisProxy[this->thisIndex].ckEmigrate(toPe);        \
  }                                                           \
  CProxy_Derived thisProxy;                                   \
  void pup(PUP::er &p) { (void)p; }                           \
  inline void _sdag_pup(PUP::er &p) { (void)p; }              \
  void virtual_pup(PUP::er &p)

/*Templated implementation of CBase_* classes.*/
template <class Parent,class CProxy_Derived>
struct CBaseT1 : Parent, virtual CBase {
  CBASE_MEMBERS;

  template <typename... Args>
  CBaseT1(Args&&... args) : Parent(std::forward<Args>(args)...) { thisProxy = this; }

  void parent_pup(PUP::er &p) {
    recursive_pup<Parent>(this, p);
    p|thisProxy;
  }
};

/*Templated version of above for multiple (at least duplicate) inheritance:*/
template <class Parent1,class Parent2,class CProxy_Derived>
struct CBaseT2 : public Parent1, public Parent2, virtual CBase {
  CBASE_MEMBERS;

  CBaseT2(void) :Parent1(), Parent2()
    { thisProxy = (Parent1 *)this; }
  CBaseT2(CkMigrateMessage *m) :Parent1(m), Parent2(m)
    { thisProxy = (Parent1 *)this; }

  void parent_pup(PUP::er &p) {
    recursive_pup<Parent1>(this, p);
    recursive_pup<Parent2>(this, p);
    p|thisProxy;
  }

  //These overloads are needed to prevent ambiguity for multiple inheritance:
  inline const CkChareID &ckGetChareID(void) const
    {return ((Parent1 *)this)->ckGetChareID();}
  static bool isIrreducible(){ return (Parent1::isIrreducible() && Parent2::isIrreducible());}
  CHARM_INPLACE_NEW
};

#define BASEN(n) CMK_CONCAT(CBaseT, n)
#define PARENTN(n) CMK_CONCAT(Parent,n)

#define CBASETN(n)                                                    \
  BASEN(n)() : base(), PARENTN(n)() {}				      \
  BASEN(n)(CkMigrateMessage *m)                                       \
	  : base(m), PARENTN(n)(m) {}				      \
  void parent_pup(PUP::er &p) {                                       \
    recursive_pup<base>(this, p);                                     \
    recursive_pup<PARENTN(n)>(this, p);                               \
  }                                                                   \
  static bool isIrreducible() {                                       \
    return (base::isIrreducible() && PARENTN(n)::isIrreducible());    \
  }


template <class Parent1, class Parent2, class Parent3, class CProxy_Derived>
struct CBaseT3 : public CBaseT2<Parent1, Parent2, CProxy_Derived>,
                 public Parent3
{
  typedef CBaseT2<Parent1, Parent2, CProxy_Derived> base;
  CBASETN(3)
};

template <class Parent1, class Parent2, class Parent3, class Parent4,
  class CProxy_Derived>
  struct CBaseT4 : public CBaseT3<Parent1, Parent2, Parent3, 
  CProxy_Derived>,
                 public Parent4
{
  typedef CBaseT3<Parent1, Parent2, Parent3, CProxy_Derived> base;
  CBASETN(4)
};

template <class Parent1, class Parent2, class Parent3, class Parent4,
  class Parent5, class CProxy_Derived>
  struct CBaseT5 : public CBaseT4<Parent1, Parent2, Parent3, 
  Parent4, CProxy_Derived>,
                 public Parent5
{
  typedef CBaseT4<Parent1, Parent2, Parent3, Parent4, CProxy_Derived> base;
  CBASETN(5)
};

template <class Parent1, class Parent2, class Parent3, class Parent4,
  class Parent5, class Parent6, class CProxy_Derived>
  struct CBaseT6 : public CBaseT5<Parent1, Parent2, Parent3, 
  Parent4, Parent5, CProxy_Derived>,
                 public Parent6
{
  typedef CBaseT5<Parent1, Parent2, Parent3, Parent4, Parent5, 
    CProxy_Derived> base;
  CBASETN(6)
};

template <class Parent1, class Parent2, class Parent3, class Parent4,
  class Parent5, class Parent6, class Parent7, class CProxy_Derived>
  struct CBaseT7 : public CBaseT6<Parent1, Parent2, Parent3, 
  Parent4, Parent5, Parent6, CProxy_Derived>,
                 public Parent7
{
  typedef CBaseT6<Parent1, Parent2, Parent3, Parent4, Parent5, 
    Parent6, CProxy_Derived> base;
  CBASETN(7)
};

template <class Parent1, class Parent2, class Parent3, class Parent4,
  class Parent5, class Parent6, class Parent7, class Parent8, class CProxy_Derived>
  struct CBaseT8 : public CBaseT7<Parent1, Parent2, Parent3, 
  Parent4, Parent5, Parent6, Parent7, CProxy_Derived>,
                 public Parent8
{
  typedef CBaseT7<Parent1, Parent2, Parent3, Parent4, Parent5, Parent6, Parent7, CProxy_Derived> base;
  CBASETN(8)
};

template <class Parent1, class Parent2, class Parent3, class Parent4,
  class Parent5, class Parent6, class Parent7, class Parent8, class Parent9, class CProxy_Derived>
  struct CBaseT9 : public CBaseT8<Parent1, Parent2, Parent3, 
  Parent4, Parent5, Parent6, Parent7, Parent8, CProxy_Derived>,
                 public Parent9
{
  typedef CBaseT8<Parent1, Parent2, Parent3, Parent4, Parent5, Parent6, Parent7, Parent8, CProxy_Derived> base;
  CBASETN(9)
};

#undef CBASETN
#undef BASEN
#undef PARENTN

/**************************** CkDelegateMgr **************************/

class CProxy;

/**
 Per-proxy data storage for delegation.  A CkDelegateMgr
 inherits from this class and adds his per-proxy data. 
 This class is reference counted.
*/
class CkDelegateData : public CkNoncopyable {
	int refcount; // reference count
public:
	CkDelegateData() :refcount(0) {}
	virtual ~CkDelegateData();
	
        //Child class constructor may have to set this.
        inline void reset() {
            refcount = 0;
        }
        
	/// Add a reference to this delegation data.  Just increments the refcount.
	///   Only CProxy should ever have to call this routine.
        /// Actually now the delegate manager calls it.
	inline void ref(void) {refcount++;}
	
	/// Remove our reference from this data.  If the refcount
	///  reaches 0, deletes the delegateData.
	///   Only CProxy should ever have to call this routine.
        /// Actually now the delegate manager calls it.
	inline void unref(void) {
		refcount--;
		if (refcount==0) delete this;
	}
};

/**
Message delegation support, where you send a message via
a proxy normally, but the message ends up routed via this
special delegateMgr group.

An "interface" class-- all delegated messages are routed via 
this class.  The default action is to deliver the message directly.
*/
class CkDelegateMgr : public IrrGroup {
  public:
    virtual ~CkDelegateMgr(); //<- so children can have virtual destructor
    virtual void ChareSend(CkDelegateData *pd,int ep,void *m,const CkChareID *c,int onPE);

    virtual void GroupSend(CkDelegateData *pd,int ep,void *m,int onPE,CkGroupID g);
    virtual void GroupBroadcast(CkDelegateData *pd,int ep,void *m,CkGroupID g);
    virtual void GroupSectionSend(CkDelegateData *pd,int ep,void *m,int nsid,CkSectionID *s);

    virtual void NodeGroupSend(CkDelegateData *pd,int ep,void *m,int onNode,CkNodeGroupID g);
    virtual void NodeGroupBroadcast(CkDelegateData *pd,int ep,void *m,CkNodeGroupID g);
    virtual void NodeGroupSectionSend(CkDelegateData *pd,int ep,void *m,int nsid,CkSectionID *s);

    virtual void ArrayCreate(CkDelegateData *pd,int ep,void *m,const CkArrayIndex &idx,int onPE,CkArrayID a);
    virtual void ArraySend(CkDelegateData *pd,int ep,void *m,const CkArrayIndex &idx,CkArrayID a);
    virtual void ArrayBroadcast(CkDelegateData *pd,int ep,void *m,CkArrayID a);
    virtual void ArraySectionSend(CkDelegateData *pd,int ep,void *m,int nsid,CkSectionID *s,int opts);
    virtual void initDelegateMgr(CProxy *proxy, int opts=0)  { (void)proxy; }
    virtual CkDelegateData* ckCopyDelegateData(CkDelegateData *data) {
        data->ref();
        return data;
    } 
    
    /**
     Management of per-proxy data: pup this delegate's data.
     If p.isUnpacking, allocate and return a new set of delegate data.
     Never delete (or unref) the data-- the proxy will do that itself
        when it is required.
     The default implementation just ignores this call.
     
     A typical implementation that uses CkDelegateData might look like this:
     <code>
       myData *d=(myData *)pd;
       if (p.isUnpacking()) d=new myData();
       p|d->myField1;
       p|d->myField2;
       return d;
     </code>
    */
    virtual CkDelegateData *DelegatePointerPup(PUP::er &p,CkDelegateData *pd);
};


/**************************** Proxies **************************/

/** 
  A proxy is a local handle to a remote object.  This is the superclass
  of all proxies: CProxy_Array, CProxy_Group, etc. inherit from this class.
  
  Real proxies for user classes are generated by the .ci file translator charmxi
  and put in the generated .decl.h headers.
*/
class CProxy {
  private:
    mutable CkDelegateMgr *delegatedMgr; // can be either a group or a nodegroup
    CkDelegateData *delegatedPtr; // private data for use by delegatedMgr.
    CkGroupID delegatedGroupId;
    bool isNodeGroup;
  protected: //Never allocate CProxy's-- only subclass them.
 CProxy() :  delegatedMgr(0), delegatedPtr(0), isNodeGroup(false)
      {delegatedGroupId.setZero(); }

#define CK_DELCTOR_PARAM CkDelegateMgr *dTo,CkDelegateData *dPtr
#define CK_DELCTOR_ARGS dTo,dPtr
#define CK_DELCTOR_CALL ckDelegatedTo(),ckDelegatedPtr()
/// Delegation constructor: used when building 
///   an element proxy from a collective proxy, like in "aProxy[i]".
    CProxy(CK_DELCTOR_PARAM)
	:delegatedMgr(dTo)
        {
            delegatedPtr = NULL;
            if(delegatedMgr != NULL && dPtr != NULL) {
                delegatedPtr = delegatedMgr->ckCopyDelegateData(dPtr);            
		delegatedGroupId = delegatedMgr->CkGetGroupID();
		isNodeGroup = delegatedMgr->isNodeGroup();
	    }
        }
  public:
    /// Copy constructor.  Only needed for delegated proxies.
    CProxy(const CProxy &src);
    /// Assignment operator.  Only needed for delegated proxies.
    CProxy& operator=(const CProxy &src);
    /// Destructor.  Only needed for delegated proxies. 
    ~CProxy() {
        if (delegatedPtr) delegatedPtr->unref();
    }
    
    /**
      Delegation allows a class, called a CkDelegateMgr, to 
      intercept calls made to this proxy for further processing.
      
      "ptr" is any delegator-specific data the CkDelegateMgr wants
      to associate with this proxy: the pointer is owned by this 
      proxy, but will be copied and pupped by calling delegator routines.
      
      This interface should only be used by library writers,
      not ordinary user code.
    */
    void ckDelegate(CkDelegateMgr *to,CkDelegateData *pd=NULL);
    
    /// Remove delegation from this proxy.
    void ckUndelegate(void);
    
    /// Return true if this proxy is delegated.
    int ckIsDelegated(void) const { return(delegatedMgr!=NULL);}
    
    /// Return the delegator of this proxy, to which the proxies' messages
    ///  are actually sent.
    inline CkDelegateMgr *ckDelegatedTo(void) const { 

      // needed if proxy was defined before group creation 
      // (i.e. for delegated readonly proxies)
      if (delegatedMgr == NULL && !delegatedGroupId.isZero()) {
	if (isNodeGroup) {
	  delegatedMgr=(CkDelegateMgr *)CkLocalNodeBranch(delegatedGroupId);
	}
	else {
	  delegatedMgr=(CkDelegateMgr *)CkLocalBranch(delegatedGroupId);
	}
      }

      return delegatedMgr; 
    }

    
    
    /// Return the delegator's local data associated with this proxy.
    inline CkDelegateData *ckDelegatedPtr(void) const {return delegatedPtr;}
    
    /// Return the groupID of our delegator.
    ///   Note that this can be a GroupID or a NodeGroupID, so be careful!
    CkGroupID ckDelegatedIdx(void) const {
    	if (delegatedMgr) return delegatedMgr->CkGetGroupID();
	else {
	  CkGroupID gid; gid.setZero();
	  return gid;
	}
    }
    
    /// Pup the data for this proxy.  Only needed for delegated proxies.
    void pup(PUP::er &p);
};


/*The base classes of each proxy type
*/
class CProxy_Chare : public CProxy {
  private:
    CkChareID _ck_cid;
  public:
    CProxy_Chare() {
#if CMK_ERROR_CHECKING
	_ck_cid.onPE=0; _ck_cid.objPtr=0;
#endif
    }
#if CMK_ERROR_CHECKING
    inline void ckCheck(void) const  {   //Make sure this proxy has a value
#ifdef CMK_CHARE_USE_PTR
	if (_ck_cid.objPtr==0)
		CkAbort("Error! This chare proxy has not been initialized!");
#endif
    }
#else
    inline void ckCheck() const {}
#endif
    CProxy_Chare(const CkChareID &c) : _ck_cid(c) {}
    CProxy_Chare(const Chare *c) : _ck_cid(c->ckGetChareID()) {}
    const CkChareID &ckGetChareID(void) const {return _ck_cid;}
    operator const CkChareID &(void) const {return ckGetChareID();}
    void ckSetChareID(const CkChareID &c) {_ck_cid=c;}
    void pup(PUP::er &p) {
    	CProxy::pup(p);
    	p(_ck_cid.onPE);
    	//Copy the pointer as straight bytes
    	p((char *)&_ck_cid.objPtr,sizeof(_ck_cid.objPtr));
    }
};

/******************* Reduction Declarations ****************/
//Silly: need the type of a reduction client here so it can be used by proxies.
//A clientFn is called on PE 0 when all contributions
// have been received and reduced.
//  param can be ignored, or used to pass any client-specific data you $
//  dataSize gives the size (in bytes) of the data array
//  data gives the reduced contributions--
//       it will be disposed of after this procedure returns.
typedef void (*CkReductionClientFn)(void *param,int dataSize,void *data);

/// Tiny utility class used by CkReductionClientAdaptor--
/// lets us keep backward compatability with the old C-style interface.
class CkReductionClientBundle : public CkCallback {
	CkReductionClientFn fn;
	void *param;
 public:
	static void callbackCfn(void *thisPtr,void *reductionMsg);
        CkReductionClientBundle(): fn(NULL), param(NULL) {}
	CkReductionClientBundle(CkReductionClientFn fn_,void *param_);
};
PUPbytes(CkReductionClientBundle)

#define CK_REDUCTION_CLIENT_DECL \
	void setReductionClient(CkReductionClientFn fn,void *param=NULL) const\
		{ ckSetReductionClient(fn,param); } \
	void ckSetReductionClient(CkReductionClientFn fn,void *param=NULL) const \
		{ ckSetReductionClient(new CkReductionClientBundle(fn,param)); } \
	void ckSetReductionClient(CkCallback *cb) const;\

#define CK_REDUCTION_CLIENT_DEF(className,mgr) \
 	void className::ckSetReductionClient(CkCallback *cb) const \
		{ (mgr)->ckSetReductionClient(cb); }\


class CProxy_NodeGroup;
class CProxy_Group : public CProxy {
  private:
    CkGroupID _ck_gid;

  public:
    CProxy_Group() {
#if CMK_ERROR_CHECKING
	_ck_gid.setZero();
#endif
	//CkPrintf(" In CProxy_Group Constructor\n");
    }
    CProxy_Group(CkGroupID g)
       :CProxy(),_ck_gid(g) {
       //CkPrintf(" In CProxy_Group Constructor\n");
       }
    CProxy_Group(CkGroupID g,CK_DELCTOR_PARAM)
    	:CProxy(CK_DELCTOR_ARGS),_ck_gid(g) {
	//CkPrintf(" In CProxy_Group Constructor\n");
	}
    CProxy_Group(const IrrGroup *g)
        :CProxy(), _ck_gid(g->ckGetGroupID()) {
	//CkPrintf(" In CProxy_Group Constructor\n");
	}
/*    CProxy_Group(const NodeGroup *g)  //<- for compatability with NodeGroups
        :CProxy(), _ck_gid(g->ckGetGroupID()) {}*/

    bool operator==(const CProxy_Group& other) {
      return ckGetGroupID() == other.ckGetGroupID();
    }

#if CMK_ERROR_CHECKING
    inline void ckCheck(void) const {   //Make sure this proxy has a value
	if (_ck_gid.isZero())
		CkAbort("Error! This group proxy has not been initialized!");
    }
#else
    inline void ckCheck() const {}
#endif

    CkChareID ckGetChareID(void) const {
    	CkChareID ret;
    	ret.onPE=CkMyPe();
    	ret.objPtr=CkLocalBranch(_ck_gid);
    	return ret;
    }
    CkGroupID ckGetGroupID(void) const {return _ck_gid;}
    operator CkGroupID () const {return ckGetGroupID();}
    void ckSetGroupID(CkGroupID g) {_ck_gid=g;}
    void pup(PUP::er &p) {
    	CProxy::pup(p);
	p|_ck_gid;
    }
    CK_REDUCTION_CLIENT_DECL
};

class CProxyElement_Group : public CProxy_Group {
  private:
    int _onPE;
  public:
    CProxyElement_Group() { }
    CProxyElement_Group(CkGroupID g,int onPE)
       : CProxy_Group(g),_onPE(onPE) {}
    CProxyElement_Group(CkGroupID g,int onPE,CK_DELCTOR_PARAM)
	: CProxy_Group(g,CK_DELCTOR_ARGS),_onPE(onPE) {}
    CProxyElement_Group(const IrrGroup *g)
        :CProxy_Group(g), _onPE(CkMyPe()) {}
    /*CProxyElement_Group(const NodeGroup *g)  //<- for compatability with NodeGroups
        :CProxy_Group(g), _onPE(CkMyPe()) {}*/

    bool operator==(const CProxyElement_Group& other) {
      return ckGetGroupID() == other.ckGetGroupID() &&
             ckGetGroupPe() == other.ckGetGroupPe();
    }

    int ckGetGroupPe(void) const {return _onPE;}
    void pup(PUP::er &p) {
    	CProxy_Group::pup(p);
    	p(_onPE);
    }
};

#define GROUP_SECTION_PROXY 1
class CProxySection_Group : public CProxy_Group {
private:
  std::vector<CkSectionID> _sid;
public:
  CProxySection_Group() { }
  CProxySection_Group(const CkGroupID &gid, const int *elems, const int nElems, int factor=USE_DEFAULT_BRANCH_FACTOR)
      :CProxy_Group(gid), _sid({CkSectionID(gid, elems, nElems, factor)}) { }
  CProxySection_Group(const CkGroupID &gid, const int *elems, const int nElems, CK_DELCTOR_PARAM)
      :CProxy_Group(gid,CK_DELCTOR_ARGS), _sid({CkSectionID(gid, elems, nElems)}) { }
  CProxySection_Group(const CProxySection_Group &cs)
      :CProxy_Group(cs.ckGetGroupID()), _sid(cs._sid) { }
  CProxySection_Group(const CProxySection_Group &cs,CK_DELCTOR_PARAM)
      :CProxy_Group(cs.ckGetGroupID(),CK_DELCTOR_ARGS), _sid(cs._sid) { }
  CProxySection_Group(const IrrGroup *g)
      :CProxy_Group(g) { }
  CProxySection_Group(const int n, const CkGroupID *gid,  int const * const *elems, const int *nElems, int factor=USE_DEFAULT_BRANCH_FACTOR)
      :CProxy_Group(gid[0]) {
    _sid.resize(n);
    for (int i=0; i<n; ++i) _sid[i] = CkSectionID{gid[i], elems[i], nElems[i], factor};
  }
  CProxySection_Group(const int n, const CkGroupID *gid, int const * const *elems, const int *nElems,CK_DELCTOR_PARAM)
      :CProxy_Group(gid[0],CK_DELCTOR_ARGS) {
    _sid.resize(n);
    for (int i=0; i<n; ++i) _sid[i] = CkSectionID{gid[i], elems[i], nElems[i]};
  }
  
  ~CProxySection_Group() {
  }
  
  CProxySection_Group &operator=(const CProxySection_Group &cs) {
    CProxy_Group::operator=(cs);
    _sid = cs._sid;
    return *this;
  }
 
  void ckSectionDelegate(CkDelegateMgr *d)
  { ckDelegate(d); d->initDelegateMgr(this, GROUP_SECTION_PROXY); } 
  //void ckSend(CkArrayMessage *m, int ep, int opts = 0) ;

  inline int ckGetNumSections() const {return _sid.size();}
  inline CkSectionInfo &ckGetSectionInfo() {return _sid[0]._cookie;}
  inline CkSectionID *ckGetSectionIDs() {return _sid.data(); }
  inline CkSectionID &ckGetSectionID() {return _sid[0]; }
  inline CkSectionID &ckGetSectionID(int i) {return _sid[i]; }
  inline CkGroupID ckGetGroupIDn(int i) const {return _sid[i]._cookie.get_aid();}
  inline const int *ckGetElements() const {return _sid[0].pelist.data();}
  inline const int *ckGetElements(int i) const {return _sid[i].pelist.data();}
  inline int ckGetNumElements() const { return _sid[0].pelist.size(); }
  inline int ckGetNumElements(int i) const { return _sid[i].pelist.size(); }
  inline int ckGetBfactor() const { return _sid[0].bfactor; }
  void pup(PUP::er &p) {
    CProxy_Group::pup(p);
    p | _sid;
  }
};

/* These classes exist to provide chare indices for the basic
 chare types.*/
class CkIndex_Chare { public:
    static int __idx;//Fake chare index for registration
};
class CkIndex_ArrayBase { public:
    static int __idx;//Fake chare index for registration
};
class CkIndex_Group { public:
    static int __idx;//Fake chare index for registration
};

typedef CkIndex_Group CkIndex_NodeGroup;
typedef CkIndex_Group CkIndex_IrrGroup;


//typedef CProxy_Group CProxy_NodeGroup;
class CProxy_NodeGroup : public CProxy{

  private:
    CkGroupID _ck_gid;
  public:
    CProxy_NodeGroup() {
#if CMK_ERROR_CHECKING
	_ck_gid.setZero();
#endif
	//CkPrintf("In CProxy_NodeGroup0 Constructor %d\n",CkLocalNodeBranch(_ck_gid));
    }
    CProxy_NodeGroup(CkGroupID g)
       :CProxy(),_ck_gid(g) {/*CkPrintf("In CProxy_NodeGroup1 Constructor %d\n",CkLocalNodeBranch(_ck_gid));*/}
    CProxy_NodeGroup(CkGroupID g,CK_DELCTOR_PARAM)
    	:CProxy(CK_DELCTOR_ARGS),_ck_gid(g) {/*CkPrintf("In CProxy_NodeGroup2 Constructor %d\n",CkLocalNodeBranch(_ck_gid));*/}
    CProxy_NodeGroup(const IrrGroup *g)
        :CProxy(), _ck_gid(g->ckGetGroupID()) {/*CkPrintf("In CProxy_NodeGroup3 Constructor %d\n",CkLocalNodeBranch(_ck_gid));*/}
/*    CProxy_Group(const NodeGroup *g)  //<- for compatability with NodeGroups
        :CProxy(), _ck_gid(g->ckGetGroupID()) {}*/

    bool operator==(const CProxy_NodeGroup& other) {
      return ckGetGroupID() == other.ckGetGroupID();
    }

#if CMK_ERROR_CHECKING
    inline void ckCheck(void) const {   //Make sure this proxy has a value
	if (_ck_gid.isZero())
		CkAbort("Error! This group proxy has not been initialized!");
    }
#else
    inline void ckCheck() const {}
#endif

    CkChareID ckGetChareID(void) const {
    	CkChareID ret;
    	ret.onPE=CkMyPe();
    	ret.objPtr=CkLocalBranch(_ck_gid);
    	return ret;
    }
    CkGroupID ckGetGroupID(void) const {return _ck_gid;}
    operator CkGroupID () const {return ckGetGroupID();}
    void ckSetGroupID(CkGroupID g) {_ck_gid=g;}
    void pup(PUP::er &p) {
    	CProxy::pup(p);
	p | _ck_gid;
    }
    CK_REDUCTION_CLIENT_DECL

};

class CProxyElement_NodeGroup : public CProxy_NodeGroup {
  private:
    int _onNode;
  public:
    CProxyElement_NodeGroup() {}
    CProxyElement_NodeGroup(CkGroupID g, int onNode)
      : CProxy_NodeGroup(g), _onNode(onNode) {}
    CProxyElement_NodeGroup(CkGroupID g, int onNode, CK_DELCTOR_PARAM)
      : CProxy_NodeGroup(g, CK_DELCTOR_ARGS), _onNode(onNode) {}
    CProxyElement_NodeGroup(const IrrGroup *g)
      : CProxy_NodeGroup(g), _onNode(CkMyNode()) {}

    bool operator ==(const CProxyElement_NodeGroup& other) {
      return ckGetGroupID() == other.ckGetGroupID() &&
             ckGetGroupPe() == other.ckGetGroupPe();
    }

    int ckGetGroupPe(void) const { return _onNode; }
    void pup(PUP::er &p) {
      CProxy_NodeGroup::pup(p);
      p(_onNode);
    }
};

typedef CProxy_Group CProxy_IrrGroup;
typedef CProxyElement_Group CProxyElement_IrrGroup;
typedef CProxySection_Group CProxySection_NodeGroup;
typedef CProxySection_Group CProxySection_IrrGroup;


//(CProxy_ArrayBase is defined in ckarray.h)

//Defines the actual "Group"
#include "ckreduction.h"

#if CMK_CHARM4PY

/// Lightweight object to support chares defined outside of the C/C++ runtime
/// Relays messages to appropiate external chare. See README.charm4py
class GroupExt: public Group {
public:
  GroupExt(void *impl_msg);

  static void __GroupExt(void *impl_msg, void *impl_obj_void) {
    new (impl_obj_void) GroupExt(impl_msg);
  }

  static void __entryMethod(void *impl_msg, void *impl_obj_void) {
    //fprintf(stderr, "GroupExt:: entry method invoked\n");
    GroupExt *obj = static_cast<GroupExt *>(impl_obj_void);
    CkMarshallMsg *impl_msg_typed = (CkMarshallMsg *)impl_msg;
    char *impl_buf = impl_msg_typed->msgBuf;
    PUP::fromMem implP(impl_buf);
    int msgSize; implP|msgSize;
    int ep; implP|ep;
    int dcopy_start; implP|dcopy_start;
    // relay received msg to external chare
    GroupMsgRecvExtCallback(obj->thisgroup.idx, ep, msgSize, impl_buf+(3*sizeof(int)),
                            dcopy_start);
  }
};

class SectionManagerExt: public GroupExt {
public:

  SectionManagerExt(void *impl_msg) : GroupExt(impl_msg) {}

  static void __SectionManagerExt(void *impl_msg, void *impl_obj_void) {
    new (impl_obj_void) SectionManagerExt(impl_msg);
  }

  // entry methods of SectionManager other than sendToSection
  static void __entryMethod(void *impl_msg, void *impl_obj_void) {
    SectionManagerExt *obj = static_cast<SectionManagerExt *>(impl_obj_void);
    CkMarshallMsg *impl_msg_typed = (CkMarshallMsg *)impl_msg;
    char *impl_buf = impl_msg_typed->msgBuf;
    PUP::fromMem implP(impl_buf);
    int msgSize; implP|msgSize;
    implP|obj->ep;
    int dcopy_start; implP|dcopy_start;
    GroupMsgRecvExtCallback(obj->thisgroup.idx, obj->ep, msgSize, impl_buf+(3*sizeof(int)),
                            dcopy_start);
  }

  // sendToSection entry method
  static void __sendToSection(void *impl_msg, void *impl_obj_void) {
    SectionManagerExt *obj = static_cast<SectionManagerExt *>(impl_obj_void);
    obj->msg = impl_msg; // store msg for forwarding to children in multicast spanning tree
    SectionManagerExt::__entryMethod(impl_msg, impl_obj_void);
    // if msg != null because I haven't forwarded it (I'm a leaf) delete it now
    CkFreeMsg(obj->msg);
  }

  // Used by Charm4py SectionManager to forward a multicast msg to its children without copying.
  // The msg was received by SectionManagerExt::__sendToSection. When forwardMulticastMsg is reached, we
  // are still inside the GroupMsgRecvExtCallback call, so the msg has not been deleted yet
  void forwardMulticastMsg(int num_children, const int *children) {
    CkSendMsgBranchMulti(ep, msg, thisgroup, num_children, children);
    // CkSendMsgBranchMulti deletes the msg, so mark it as null to not delete it again
    msg = nullptr;
  }

private:
  int ep;
  void *msg;
};

#endif

class CkQdMsg {
  public:
    void *operator new(size_t s) { return CkAllocMsg(0,(int)s,0,GroupDepNum{}); }
    void operator delete(void* ptr) { CkFreeMsg(ptr); }
    static void *alloc(int, size_t s, int*, int, int) {
      return CkAllocMsg(0,(int)s,0,GroupDepNum{});
    }
    static void *pack(CkQdMsg *m) { return (void*) m; }
    static CkQdMsg *unpack(void *buf) { return (CkQdMsg*) buf; }
    /// This is used to display message contents in the debugger.
    static void ckDebugPup(PUP::er &p,void *msg) {
      (void)p;
      (void)msg;
    }
};

class CkThrCallArg {
  public:
    void *msg;
    void *obj;
    CkThrCallArg(void *m, void *o) : msg(m), obj(o) {}
};

extern void CkStartQD(const CkCallback& cb);
#define CkExitAfterQuiescence() CkStartQD(CkCallback(CkCallback::ckExit))


#if !CMK_MACHINE_PROGRESS_DEFINED
#define CkNetworkProgress() 
#define CkNetworkProgressAfter(p) 

#else
void CmiMachineProgressImpl();

#define CkNetworkProgress() {CpvAccess(networkProgressCount) ++; \
if(CpvAccess(networkProgressCount) >=  networkProgressPeriod)  \
    if (LBManagerObj()->StatsOn() == 0) { \
        CmiMachineProgressImpl(); \
        CpvAccess(networkProgressCount) = 0; \
    } \
} \

#define CkNetworkProgressAfter(p) {CpvAccess(networkProgressCount) ++; \
if(CpvAccess(networkProgressCount) >=  p)  \
    if (LBManagerObj()->StatsOn() == 0) { \
        CmiMachineProgressImpl(); \
        CpvAccess(networkProgressCount) = 0; \
    } \
} \

#endif



#include "ckmemcheckpoint.h"
#include "readonly.h"
#include "ckarray.h"
#include "ckstream.h"
#include "ckfutures.h"
#include "waitqd.h"
#include "sdag.h"
#include "ckcheckpoint.h"
#include "trace.h"
#include "envelope.h"
#include "pathHistory.h"
#include "ckcallback-ccs.h"


template<typename... tArgs>
int CkRegisterEp(const std::string& name, CkCallFnPtr call, int msgIdx, int chareIdx, int ck_ep_flags) {
  std::string combined(name);

  size_t argStart = name.find('(');
  if (argStart == std::string::npos)
    argStart = name.length();

#if defined(__GNUG__) || defined(__clang__)
  std::string functionName = __PRETTY_FUNCTION__;
  std::string templateString = functionName.substr(functionName.find("tArgs = ") + 8);
#ifdef __clang__
  // Clang seems to use the format [tArgs = <args>], so remove the closing ]
  // (the opening one will have been removed by the substr above)
  if (templateString.rfind(']') != std::string::npos)
    templateString.erase(templateString.rfind(']'));
#else // __GNUG__
  // Gcc seems to use the format [with tArgs = {args}; ...]
  // so find and extract the substring bounded by {}
  // Assumes that there are no occurrences of '}' inside tArgs
  size_t openingPos = templateString.find('{'), closingPos = templateString.find('}');
  if (openingPos != std::string::npos && closingPos != std::string::npos && closingPos > openingPos + 1)
    templateString = templateString.substr(openingPos + 1, closingPos - openingPos - 1);
  templateString = "<" + templateString + ">";
#endif
#elif defined(_MSC_VER)
  // Vc++ seems to use the format int __cdecl CkRegisterEp<class tArgs>(...)
  // so find '<' and try to match with '>' until a full expression is found
  std::string functionName = __FUNCSIG__;
  std::string templateString;
  size_t openingIndex = functionName.find('<');
  if (openingIndex != std::string::npos) {
    int count = 1;
    size_t index = openingIndex + 1;
    while (count > 0 && index < functionName.length()) {
      switch (functionName[index]) {
        case '<':
          count++;
          break;
        case '>':
          count--;
          break;
        default:
          break;
      }

      index++;
    }

    // if we've found a matching set of <>, then fill in args, otherwise leave empty
    if (count == 0) {
      templateString = functionName.substr(openingIndex, index - openingIndex + 1);
    }
  }
#else
  // { ()... } is a pack expansion, calls typeid(var).name() for each var in tArgs
  std::vector<std::string> tArgNames = { (typeid(tArgs).name())... };

  std::string templateString = "<";
  for (auto it = tArgNames.begin(); it != tArgNames.end(); it++) {
    templateString += *it;
    if (it + 1 != tArgNames.end())
      templateString += ", ";
  }
  templateString += ">";
#endif

  combined.insert(argStart, templateString);

  return CkRegisterEpTemplated(combined.c_str(), call, msgIdx, chareIdx, ck_ep_flags);
}

/************************** Debugging Utilities **************/

CkpvExtern(DebugEntryTable, _debugEntryTable);

//For debugging: convert given index to a string (NOT threadsafe)
static const char *idx2str(const CkArrayIndex &ind) {
  static char retBuf[80];
  retBuf[0]=0;
  if (ind.dimension <= 3) {
    for (int i=0;i<ind.nInts;i++) {
      if (i>0) strcat(retBuf,";");
      sprintf(&retBuf[strlen(retBuf)],"%d",ind.data()[i]);
    }
  } else {
    const short int *idx = (const short int*)ind.data();
    for (int i=0;i<ind.dimension;i++) {
      if (i>0) strcat(retBuf,";");
      sprintf(&retBuf[strlen(retBuf)],"%hd",idx[i]);
    }
  }
  return retBuf;
}

static const char *idx2str(const ArrayElement *el) UNUSED;
static const char *idx2str(const ArrayElement* el) {
  return idx2str(el->thisIndexMax);
}



class CkConditional {
  int refcount;
public:
  CkConditional() : refcount(1) { }
  virtual ~CkConditional() { }
    /*
  void* operator new(size_t s) {
    return malloc(s);
  }
    */
  void deallocate() {
    //CkPrintf("CkConditional::delete %d\n",refcount);
    if (--refcount == 0) {
      //((CkConditional*)p)->~CkConditional();
      delete this;
    }
  }
  void copyreference(void) {
    ++refcount;
  }
};

// Method to check if the Charm RTS initialization phase has completed
void checkForInitDone(bool rdmaROCompleted);

#endif



