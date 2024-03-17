/*
Pack/UnPack Library for UIUC Parallel Programming Lab
Orion Sky Lawlor, olawlor@uiuc.edu, 4/5/2000

This library allows you to easily pack an array, structure,
or object into a memory buffer or disk file, and then read
the object back later.  The library can also handle translating
between different machine representations for integers and floats.

Originally, you had to write separate functions for buffer 
sizing, pack to memory, unpack from memory, pack to disk, and 
unpack from disk.  These functions all perform the exact same function--
namely, they list the members of the array, struct, or object.
Further, all the functions must agree, or the unpacked data will 
be garbage.  This library allows the user to write *one* function,
pup, which will perform all needed packing/unpacking.

A simple example is:
class foo {
 private:
  bool isBar;
  int x;
  char y;
  unsigned long z;
  CkVec<double> q;
 public:
  ...
  void pup(PUP::er &p) {
    PUPn(isBar);
    PUPn(x);PUPn(y);PUPn(z);
    PUPn(q);
  }
};

A more complex example is:
class bar {
 private:
  foo f;
  int nArr;//Length of array below
  double *arr;//Heap-allocated array
 public:
  ...
  
  void pup(PUP::er &p) {
    PUPn(f); // <- automatically calls foo::pup
    PUPn(nArr);
    if (p.isUnpacking()) // <- must allocate array on other side.
    {
      arr=new double[nArr];
      _MEMCHECK(arr);
    }
    PUPv(arr,nArr); // <- special syntax for arrays of simple types
  }
};
*/

#ifndef __CK_PUP_H
#define __CK_PUP_H

#include <stdio.h> /*<- for "FILE *" */
#include <type_traits>
#include <utility>
#include <functional>

#ifndef __cplusplus
#error "Use pup_c.h for C programs-- pup.h is for C++ programs"
#endif

#ifdef STANDALONE_PUP
#define CmiAbort(x) { printf(x); abort(); }
#else
#ifndef CHARM_H
#  include "converse.h" // <- for CMK_* defines
#endif
#endif

//We need CkMigrateMessage only to distinguish the migration
// constructor from all other constructors-- the type
// itself has no meaningful fields.
typedef struct {int is_only_a_name;} CkMigrateMessage;

namespace PUP {
  class er; // Forward declare for all sorts of things

  /**
   * A utility to let classes avoid default construction when they're
   * about to be unpacked, by defining a constructor that takes a
   * value of type PUP::reconstruct
   */
  struct reconstruct {};
  namespace detail {
    template <typename T, bool b = std::is_constructible<T, reconstruct>::value>
    struct TemporaryObjectHolder { };
    template <typename T>
    struct TemporaryObjectHolder<T, true>
    {
      typename std::remove_cv<typename std::remove_reference<T>::type>::type t{reconstruct()};
    };
    template <typename T>
    struct TemporaryObjectHolder<T, false>
    {
      typename std::remove_cv<typename std::remove_reference<T>::type>::type t{};
    };
    template <typename T>
    void operator|(er &p, TemporaryObjectHolder<T> &t) {
      p | t.t;
    }
  }

#if CMK_LONG_LONG_DEFINED
#define CMK_PUP_LONG_LONG long long
#elif CMK___int64_DEFINED
#define CMK_PUP_LONG_LONG __int64
#endif

 
//Item data types-- these are used to do byte swapping, etc.
typedef enum {
//(this list must exactly match that in PUPer_xlate)
  Tchar=0,Tshort, Tint, Tlong, Tlonglong,
  Tuchar,Tushort,Tuint,Tulong, Tulonglong,
#if CMK_HAS_INT16
  Tint128, Tuint128,
#endif
  Tfloat,Tdouble,Tlongdouble,
  Tbool,
  Tbyte,
  Tsync,
  Tpointer,
  dataType_last //<- for setting table lengths, etc.
} dataType;

static inline dataType getXlateDataType(signed char *a) { return Tchar; }
#if CMK_SIGNEDCHAR_DIFF_CHAR
static inline dataType getXlateDataType(char *a) { return Tchar; }
#endif
static inline dataType getXlateDataType(short *a) { return Tshort; }
static inline dataType getXlateDataType(int *a) { return Tint; }
static inline dataType getXlateDataType(long *a) { return Tlong; }
static inline dataType getXlateDataType(unsigned char *a) { return Tuchar; }
static inline dataType getXlateDataType(unsigned short *a) { return Tushort; }
static inline dataType getXlateDataType(unsigned int *a) { return Tuint; }
static inline dataType getXlateDataType(unsigned long *a) { return Tulong; }
static inline dataType getXlateDataType(float *a) { return Tfloat; }
static inline dataType getXlateDataType(double *a) { return Tdouble; }
#if CMK_LONG_DOUBLE_DEFINED
static inline dataType getXlateDataType(long double *a) { return Tlongdouble; }
#endif
static inline dataType getXlateDataType(bool *a) { return Tbool; }
#ifdef CMK_PUP_LONG_LONG
static inline dataType getXlateDataType(CMK_PUP_LONG_LONG *a) { return Tlonglong; }
static inline dataType getXlateDataType(unsigned CMK_PUP_LONG_LONG *a) { return Tulonglong; }
#endif
#if CMK_HAS_INT16
static inline dataType getXlateDataType(CmiInt16 *a) { return Tint128; }
static inline dataType getXlateDataType(CmiUInt16 *a) { return Tuint128; }
#endif


//This should be a 1-byte unsigned type
typedef unsigned char myByte;

//Forward declarations
class er;
class able;

//Used for out-of-order unpacking
class seekBlock {
	enum {maxSections=3};
	int secTab[maxSections+1];//The start of each seek section
	int nSec;//Number of sections; current section #
	int secTabOff;//Start of the section table, relative to the seek block
	er &p;
	bool hasEnded;
public:
	//Constructor
	seekBlock(er &Np,int nSections);
	//Destructor
	~seekBlock();

	//Seek to the given section number (0-based, less than nSections)
	void seek(int toSection);
	//Finish with this seeker (must be called)
	void endBlock(void);

	//An evil hack to avoid inheritance and virtual functions among seekers--
	// stores the PUP::er specific block start information.
	union {
		size_t off;
		long loff;
		const myByte *cptr;
		myByte *ptr;
		void *vptr;
	} data;
};

//The abstract base class:  PUP::er.
class er {
 private:
  er(const er &p);//You don't want to copy PUP::er's.
 protected:
   unsigned int PUP_er_state;
   // You don't want to create raw PUP::er's.
   explicit er(unsigned int inType) : PUP_er_state(inType) {}

   /// These state bits describe the PUP::er's direction.
   enum
   {
     IS_SIZING = 0x0100,
     IS_PACKING = 0x0200,
     IS_UNPACKING = 0x0400,
     TYPE_MASK = 0xFF00
   };
 public:
  virtual ~er();//<- does nothing, but might be needed by some child

  // These state bits describe various user-settable properties. This needs to be public
  // because it used when PUPers are created from the checkpointing and migration code.
  enum
  {
    IS_USERLEVEL = 0x0004,   // If set, this is not a migration pup - it's something else.
    IS_DELETING = 0x0008,    // If set, C & f90 objects should delete themselves after pup
    IS_COMMENTS = 0x0010,    // If set, this PUP::er wants comments and sync codes.
    IS_CHECKPOINT = 0x0020,  // If set, it is creating or restarting from a checkpoint
    IS_MIGRATION = 0x0040    // If set, it is migrating between PEs (e.g. in LB)
  };

  //State queries (exactly one of these will be true)
  bool isSizing(void) const {return (PUP_er_state&IS_SIZING)!=0?true:false;}
  bool isPacking(void) const {return (PUP_er_state&IS_PACKING)!=0?true:false;}
  bool isUnpacking(void) const {return (PUP_er_state&IS_UNPACKING)!=0?true:false;}
  const char *  typeString() const;
  unsigned int getStateFlags(void) const {return PUP_er_state;}

  //This indicates that the pup routine should free memory during packing.
  void becomeDeleting(void) {PUP_er_state|=IS_DELETING;}
  bool isDeleting(void) const {return (PUP_er_state&IS_DELETING)!=0?true:false;}

  //This indicates that the pup routine should not call system objects' pups.
  void becomeUserlevel(void) {PUP_er_state|=IS_USERLEVEL;}
  bool isUserlevel(void) const {return (PUP_er_state&IS_USERLEVEL)!=0?true:false;}
  
  bool isCheckpoint(void) const
  {
    return (PUP_er_state & IS_CHECKPOINT) != 0;
  }

  bool isMigration(void) const
  {
    return (PUP_er_state & IS_MIGRATION) != 0;
  }

  bool hasComments(void) const {return (PUP_er_state&IS_COMMENTS)!=0?true:false;}

//For single elements, pretend it's an array containing one element
  template<class T>
  void operator()(T &v)               {(*this)(&v,1);}

  void operator()(void* &v,void* sig) {(*this)(&v,1,sig);}

  //For arrays:
  template<class T>
  void operator()(T *a,size_t nItems) {
    bytes((void *)a,nItems, sizeof(T), getXlateDataType(a));
  }

  // Standard pup_buffer API that calls malloc for allocation on isUnpacking and free for deallocation on isPacking
  template<class T>
  void pup_buffer_async(T *&a, size_t nItems) {
    pup_buffer_async((void *&)a, nItems, sizeof(T), getXlateDataType(a));
  }

  // Custom pup_buffer API that calls user provided 'allocate' function for allocation on isUnpacking and
  // user provided 'deallocate' function for deallocation on isPacking
  // Custom pup_buffer behaves same as the standard pup_buffer except for calling custom allocator and deallocator methods
  template<class T>
  void pup_buffer_async(T *&a, size_t nItems, std::function<void *(size_t)> allocate, std::function<void (void *)> deallocate) {
    pup_buffer_async((void *&)a, nItems, sizeof(T), getXlateDataType(a), allocate, deallocate);
  }

  //For pointers: the last parameter is to make it more difficult to call
  //(should not be used in normal code as pointers may loose meaning across processor)
  void operator()(void **a,size_t nItems,void *pointerSignature) {
    (void)pointerSignature;
    bytes((void *)a,nItems,sizeof(void *),Tpointer);
  }

  //For raw memory (n gives number of bytes)
/*
  // pup void * is error-prune, let's avoid it - Gengbin
  void operator()(void *a,size_t nBytes)
    {bytes((void *)a,nBytes,1,Tbyte);}
*/

  //For allocatable objects (system will new/delete object and call pup routine)
  void operator()(able** a)
    {object(a);}
  //For pre- or stack-allocated PUP::able objects-- just call object's pup
  void operator()(able& a);

  /// A descriptive (but entirely optional) human-readable comment field
  virtual void comment(const char *message);

  /// A 32-bit "synchronization marker" (not human readable).
  ///  Some standard codes are listed under PUP::sync_....
  virtual void synchronize(unsigned int sync);
  
  /// Insert a synchronization marker and comment into the stream.
  ///  Only applies if this PUP::er wants comments.
  inline void syncComment(unsigned int sync,const char *message=0) {
#if CMK_ERROR_CHECKING
  	if (hasComments()) {
		synchronize(sync);
		if (message) comment(message);
	}
#else
	/* empty, to avoid expensive virtual function calls */
#endif
  }

  //Generic bottleneck: pack/unpack n items of size itemSize
  // and data type t from p.  Desc describes the data item
  virtual void bytes(void *p,size_t n,size_t itemSize,dataType t) =0;
  virtual void object(able** a);

  virtual void pup_buffer_async(void *&p, size_t n, size_t itemSize, dataType t) = 0;
  virtual void pup_buffer_async(void *&p, size_t n, size_t itemSize, dataType t, std::function<void *(size_t)> allocate, std::function<void (void *)> deallocate) = 0;

  virtual size_t size(void) const { return 0; }
  
  //For seeking (pack/unpack in different orders)
  virtual void impl_startSeek(seekBlock &s); /*Begin a seeking block*/
  virtual size_t impl_tell(seekBlock &s); /*Give the current offset*/
  virtual void impl_seek(seekBlock &s,size_t off); /*Seek to the given offset*/
  virtual void impl_endSeek(seekBlock &s);/*End a seeking block*/

  //See more documentation before PUP_cmiAllocSizer in pup_cmialloc.h
  //Must be a CmiAlloced buf while packing
  virtual void pupCmiAllocBuf(void **msg) {
    (void)msg;
    CmiAbort("Undefined PUPer:Did you use PUP_toMem or PUP_fromMem?\n");
  }

  //In case source is not CmiAlloced the size can be passed and any
  //user buf can be converted into a cmialloc'ed buf
  virtual void pupCmiAllocBuf(void **msg, size_t size) {
    (void)msg;
    (void)size;
    CmiAbort("Undefined PUPer:Did you use PUP_toMem or PUP_fromMem?\n");
  }
};

/**
 "Sync" codes are an extra channel to encode data in a pup stream, 
 to indicate higher-order structures in the pup'd objects.
 Sync codes must follow this grammar:
   <obj> -> <obj> <obj> | <array> | <list>
   <obj> -> begin (<obj> system) <obj> end
   <array> -> begin <obj> (item <obj>)* end
   <list> -> begin <obj> (index <obj> item <obj>)* end
 This hack is used, e.g., by the debugger.
*/
enum {
  sync_builtin=0x70000000, // Built-in, standard sync codes begin here
  sync_begin=sync_builtin+0x01000000, // Sync code at start of collection
  sync_end=sync_builtin+0x02000000, // Sync code at end of collection
  sync_last_system=sync_builtin+0x09000000, // Sync code at end of "system" portion of object
  sync_array_m=0x00100000, // Linear-indexed (0..n) array-- use item to separate
  sync_list_m=0x00200000, // Some other collection-- use index and item
  sync_object_m=0x00300000, // Sync mask for general object
  
  sync_begin_array=sync_begin+sync_array_m,
  sync_begin_list=sync_begin+sync_list_m, 
  sync_begin_object=sync_begin+sync_object_m, 
  
  sync_end_array=sync_end+sync_array_m, 
  sync_end_list=sync_end+sync_list_m, 
  sync_end_object=sync_end+sync_object_m, 
  
  sync_item=sync_builtin+0x00110000, // Sync code for a list or array item
  sync_index=sync_builtin+0x00120000, // Sync code for index of item in a list
  
  sync_last
};

/************** PUP::er -- Sizer ******************/
//For finding the number of bytes to pack (e.g., to preallocate a memory buffer)
class sizer : public er {
 protected:
  size_t nBytes;
  //Generic bottleneck: n items of size itemSize
  virtual void bytes(void *p,size_t n,size_t itemSize,dataType t);

  virtual void pup_buffer_async(void *&p, size_t n, size_t itemSize, dataType t);
  virtual void pup_buffer_async(void *&p, size_t n, size_t itemSize, dataType t, std::function<void *(size_t)> allocate, std::function<void (void *)> deallocate);

 public:
  //Write data to the given buffer
  sizer(const unsigned int purpose = 0) : er(IS_SIZING | purpose), nBytes(0)
  {
    CmiAssert((purpose & TYPE_MASK) == 0);
  }

  //Return the current number of bytes to be packed
  size_t size(void) const {return nBytes;}
};

template <class T>
inline size_t size(T &t) {
	PUP::sizer p; p|t; return p.size();
}

/********** PUP::er -- Binary memory buffer pack/unpack *********/
class mem : public er { //Memory-buffer packers and unpackers
 protected:
  myByte *origBuf;//Start of memory buffer
  myByte *buf;//Memory buffer (stuff gets packed into/out of here)
  mem(const unsigned int type, myByte* Nbuf, const unsigned int purpose = 0)
      : er(type | purpose), origBuf(Nbuf), buf(Nbuf)
  {
    CmiAssert((purpose & TYPE_MASK) == 0);
  }
  mem(const mem &p);			//You don't want to copy
  void operator=(const mem &p);		// You don't want to copy

  //For seeking (pack/unpack in different orders)
  virtual void impl_startSeek(seekBlock &s); /*Begin a seeking block*/
  virtual size_t impl_tell(seekBlock &s); /*Give the current offset*/
  virtual void impl_seek(seekBlock &s,size_t off); /*Seek to the given offset*/
 public:
  //Return the current number of buffer bytes used
  size_t size(void) const {return buf-origBuf;}

  inline char* get_current_pointer() const {
    return reinterpret_cast<char*>(buf);
  }

  inline char* get_orig_pointer() const {
    return reinterpret_cast<char*>(origBuf);
  }

  inline void reset() {
    buf = origBuf;
  }

  inline void advance(size_t const offset) {
    buf += offset;
  }
};

//For packing into a preallocated, presized memory buffer
class toMem : public mem {
 protected:
  //Generic bottleneck: pack n items of size itemSize from p.
  virtual void bytes(void *p,size_t n,size_t itemSize,dataType t);

  virtual void pup_buffer_async(void *&p, size_t n, size_t itemSize, dataType t);
  virtual void pup_buffer_async(void *&p, size_t n, size_t itemSize, dataType t, std::function<void *(size_t)> allocate, std::function<void (void *)> deallocate);

 public:
  //Write data to the given buffer
  toMem(void* Nbuf, const unsigned int purpose = 0)
      : mem(IS_PACKING, (myByte*)Nbuf, purpose)
  {
  }
};
template <class T>
inline void toMemBuf(T &t,void *buf, size_t len) {
	PUP::toMem p(buf);
	p|t;
	if (p.size()!=len) CmiAbort("Size mismatch during PUP::toMemBuf!\n"
		"This means your pup routine doesn't match during sizing and packing");
}

//For unpacking from a memory buffer
class fromMem : public mem {
 protected:
  //Generic bottleneck: unpack n items of size itemSize from p.
  virtual void bytes(void *p,size_t n,size_t itemSize,dataType t);

  virtual void pup_buffer_async(void *&p, size_t n, size_t itemSize, dataType t);
  virtual void pup_buffer_async(void *&p, size_t n, size_t itemSize, dataType t, std::function<void *(size_t)> allocate, std::function<void (void *)> deallocate);

  void pup_buffer_async_generic(void *&p,size_t n, size_t itemSize, dataType t, std::function<void *(size_t)> allocate, bool isMalloc);

 public:
  //Read data from the given buffer
  fromMem(const void* Nbuf, const unsigned int purpose = 0)
      : mem(IS_UNPACKING, (myByte*)Nbuf, purpose)
  {
  }
};
template <class T>
inline void fromMemBuf(T &t,void *buf,size_t len) {
	PUP::fromMem p(buf);
	p|t;
	if (p.size()!=len) CmiAbort("Size mismatch during PUP::fromMemBuf!\n"
		"This means your pup routine doesn't match during packing and unpacking");
}

/********** PUP::er -- Binary disk file pack/unpack *********/
class disk : public er {
 protected:
  FILE *F;//Disk file to read from/write to
  disk(const unsigned int type, FILE* f, const unsigned int purpose = 0)
      : er(type | purpose), F(f)
  {
    CmiAssert((purpose & TYPE_MASK) == 0);
  }

  disk(const disk &p);			//You don't want to copy
  void operator=(const disk &p);	// You don't want to copy

  //For seeking (pack/unpack in different orders)
  virtual void impl_startSeek(seekBlock &s); /*Begin a seeking block*/
  virtual size_t impl_tell(seekBlock &s); /*Give the current offset*/
  virtual void impl_seek(seekBlock &s,size_t off); /*Seek to the given offset*/
};

//For packing to a disk file
class toDisk : public disk {
 protected:
  //Generic bottleneck: pack n items of size itemSize from p.
  virtual void bytes(void *p,size_t n,size_t itemSize,dataType t);

  virtual void pup_buffer_async(void *&p, size_t n, size_t itemSize, dataType t);
  virtual void pup_buffer_async(void *&p, size_t n, size_t itemSize, dataType t, std::function<void *(size_t)> allocate, std::function<void (void *)> deallocate);

  bool error;
 public:
  // Write data to the given file pointer
  // (must be opened for binary write)
  // You must close the file yourself when done.
  toDisk(FILE* f, const unsigned int purpose = 0) : disk(IS_PACKING, f, purpose)
  {
    error = false;
  }
  bool checkError(){return error;}
};

//For unpacking from a disk file
class fromDisk : public disk {
 protected:
  //Generic bottleneck: unpack n items of size itemSize from p.
  virtual void bytes(void *p,size_t n,size_t itemSize,dataType t);

  virtual void pup_buffer_async(void *&p, size_t n, size_t itemSize, dataType t);
  virtual void pup_buffer_async(void *&p, size_t n, size_t itemSize, dataType t, std::function<void *(size_t)> allocate, std::function<void (void *)> deallocate);

 public:
  // Read data from the given file pointer 
  // (must be opened for binary read)
  // You must close the file yourself when done.
  fromDisk(FILE* f, const unsigned int purpose = 0) : disk(IS_UNPACKING, f, purpose) {}
};

/************** PUP::er -- Text *****************/
class toTextUtil : public er {
 private:
  char *cur; /*Current output buffer*/
  size_t maxCount; /*Max length of cur buffer*/
  int level; /*Indentation distance*/
  void beginEnv(const char *type,int n=0);
  void endEnv(const char *type);
  char *beginLine(void);
  void endLine(void);
 protected:
  virtual char *advance(char *cur)=0; /*Consume current buffer and return next*/
  toTextUtil(unsigned int inType,char *buf,size_t len);
  toTextUtil(const toTextUtil &p);		//You don't want to copy
  void operator=(const toTextUtil &p);		// You don't want to copy
 public:
  virtual void comment(const char *message);
  virtual void synchronize(unsigned int m);
 protected:
  virtual void bytes(void *p,size_t n,size_t itemSize,dataType t);

  virtual void pup_buffer_async(void *&p, size_t n, size_t itemSize, dataType t);
  virtual void pup_buffer_async(void *&p, size_t n, size_t itemSize, dataType t, std::function<void *(size_t)> allocate, std::function<void (void *)> deallocate);

  virtual void object(able** a);
};
/* Return the number of characters, including terminating NULL */
class sizerText : public toTextUtil {
 public:
  static constexpr int lineLen = 1000;
 private:
  char line[lineLen];
  size_t charCount; /*Total characters seen so far (not including NULL) */
 protected:
  virtual char *advance(char *cur);
 public:
  sizerText(void);
  size_t size(void) const {return charCount+1; /*add NULL*/ }
};

/* Copy data to this C string, including terminating NULL. */
class toText : public toTextUtil {
 private:
  char *buf;
  size_t charCount; /*Total characters written so far (not including NULL) */
  size_t maxCount; /*Max length of buf*/
 protected:
  virtual char *advance(char *cur);
 public:
  toText(char *outStr, size_t len);
  toText(const toText &p);			//You don't want to copy
  void operator=(const toText &p);		// You don't want to copy
  size_t size(void) const {return charCount+1; /*add NULL*/ }
};

class toTextFile : public er {
 protected:
  FILE *f;
  virtual void bytes(void *p,size_t n,size_t itemSize,dataType t);

  virtual void pup_buffer_async(void *&p, size_t n, size_t itemSize, dataType t);
  virtual void pup_buffer_async(void *&p, size_t n, size_t itemSize, dataType t, std::function<void *(size_t)> allocate, std::function<void (void *)> deallocate);

 public:
  //Begin writing to this file, which should be opened for ascii write.
  // You must close the file yourself when done.
  toTextFile(FILE *f_) :er(IS_PACKING), f(f_) {}
  toTextFile(const toTextFile &p);		//You don't want to copy
  void operator=(const toTextFile &p);		// You don't want to copy
  virtual void comment(const char *message);
};
class fromTextFile : public er {
 protected:
  FILE *f;
  int readInt(const char *fmt="%d");
  unsigned int readUint(const char *fmt="%u");
  CMK_TYPEDEF_INT8 readLongInt(const char *fmt="%lld");
  double readDouble(void);
  
  virtual void bytes(void *p,size_t n,size_t itemSize,dataType t);

  virtual void pup_buffer_async(void *&p, size_t n, size_t itemSize, dataType t);
  virtual void pup_buffer_async(void *&p, size_t n, size_t itemSize, dataType t, std::function<void *(size_t)> allocate, std::function<void (void *)> deallocate);

  virtual void parseError(const char *what);
 public:
  //Begin writing to this file, which should be opened for ascii read.
  // You must close the file yourself when done.
  fromTextFile(FILE *f_) :er(IS_UNPACKING), f(f_) {}
  fromTextFile(const fromTextFile &p);		//You don't want to copy
  void operator=(const fromTextFile &p);	// You don't want to copy
  virtual void comment(const char *message);
};

/********** PUP::er -- Heterogenous machine pack/unpack *********/
//This object describes the data representation of a machine.
class machineInfo {
 public:
  typedef unsigned char myByte;
  myByte magic[4];//Magic number (to identify machineInfo structs)
  myByte version;//0-- current version

  myByte intBytes[5]; //<- sizeof(char,short,int,long,int128)
  myByte intFormat;//0-- big endian.  1-- little endian.

  myByte floatBytes; //<- sizeof(...)
  myByte doubleBytes;
  myByte floatFormat;//0-- big endian IEEE.  1-- little endian IEEE.

  myByte boolBytes;
  myByte pointerBytes;

//  myByte padding[1];//Padding to 16 bytes

  //Return true if our magic number is valid.
  bool valid(void) const;
  //Return true if we differ from the current (running) machine.
  bool needsConversion(void) const;
  
  //Get a machineInfo for the current machine
  static const machineInfo &current(void);

  void pup(er &p) {
      myByte  padding;

      p(magic, 4);
      p(version);
      if (version == 0) p(intBytes, 4);
      else p(intBytes, 5);
      p(intFormat);
      p(floatBytes); p(doubleBytes); p(floatFormat);
      p(boolBytes); p(pointerBytes);
      if (version == 0) p(padding);
  }
};

/// "Wrapped" PUP::er: forwards requests to another PUP::er.
class wrap_er : public er {
protected:
	er &p;
public:
	wrap_er(er &p_,unsigned int newFlags=0) :er(p_.getStateFlags()|newFlags), p(p_) {}
	virtual size_t size(void) const { return p.size(); }
	
	virtual void impl_startSeek(seekBlock &s); /*Begin a seeking block*/
	virtual size_t impl_tell(seekBlock &s); /*Give the current offset*/
	virtual void impl_seek(seekBlock &s,size_t off); /*Seek to the given offset*/
	virtual void impl_endSeek(seekBlock &s);/*End a seeking block*/
};

//For translating some odd disk/memory representation into the 
// current machine representation.  (We really only need to
// translate during unpack-- "reader makes right".)
class xlater : public wrap_er {
 protected:
  typedef void (*dataConverterFn)(int N,const myByte *in,myByte *out,size_t nElem);
  
  //This table is indexed by dataType, and contains an appropriate
  // conversion function to unpack a n-item array of the corresponding 
  // data type (possibly in-place).
  dataConverterFn convertFn[dataType_last];
  //Maps dataType to source machine's dataSize
  size_t convertSize[dataType_last];
  void setConverterInt(const machineInfo &m,const machineInfo &cur,
    int isUnsigned,int intType,dataType dest);
  
  //Generic bottleneck: unpack n items of size itemSize from p.
  virtual void bytes(void *p,size_t n,size_t itemSize,dataType t);

  virtual void pup_buffer_async(void *&p, size_t n, size_t itemSize, dataType t);
  virtual void pup_buffer_async(void *&p, size_t n, size_t itemSize, dataType t, std::function<void *(size_t)> allocate, std::function<void (void *)> deallocate);

 public:
  xlater(const machineInfo &fromMachine, er &fromData);
};

/*************** PUP::able support ***************/
//The base class of system-allocatable objects with pup routines
class able {
public:
	//A globally-unique, persistent identifier for an allocatable object
	class PUP_ID {
	public:
		enum {len=8};
		unsigned char hash[len];
		PUP_ID() {}
		PUP_ID(int val) {for (int i=0;i<len;i++) hash[i]=val;}
		PUP_ID(const char *name) {setName(name);}
		void setName(const char *name);//Write name into hash
		bool operator==(const PUP_ID &other) const {
			for (int i=0;i<len;i++)
				if (hash[i]!=other.hash[i])
					return false;
			return true;
		}
		void pup(er &p) {
			 p((char *)hash,sizeof(unsigned char)*len);
		}
		void pup(er &p) const {
			 p((char *)hash,sizeof(unsigned char)*len);
		}
	};

protected:
	able() {}
	able(CkMigrateMessage *) {}

public:
	virtual ~able();//Virtual destructor may be needed by some child

//Constructor function registration:
	typedef able* (*constructor_function)(void);
	static PUP_ID register_constructor(const char *className,
		constructor_function fn);
	static constructor_function get_constructor(const PUP_ID &id);
	virtual /*PUP::*/able *clone(void) const;

//Target methods:
	virtual void pup(er &p);
	virtual const PUP_ID &get_PUP_ID(void) const=0;
};

template <typename T, bool PUPable = std::is_base_of<PUP::able, T>::value>
struct ptr_helper;

#define SINGLE_ARG(...) __VA_ARGS__

//Declarations which create routines implemeting the | operator.
//  Macros to be used inside a class body.
#define PUPable_operator_inside(className)\
    friend inline void operator|(PUP::er &p,className &a) {a.pup(p);}\
    friend inline void operator|(PUP::er &p,className* &a) {\
	PUP::able *pa=a;  p(&pa);  a=dynamic_cast<className *>(pa);\
    }

//  Macros to be used outside a class body.
#define PUPable_operator_outside(className)\
    inline void operator|(PUP::er &p,className &a) {a.pup(p);}\
    inline void operator|(PUP::er &p,className* &a) {\
	PUP::able *pa=a;  p(&pa);  a=dynamic_cast<className *>(pa);\
    }

//Declarations to include in a PUP::able's body.
//  Convenient, but only usable if class is not inside a namespace.
#define PUPable_decl(className) \
    PUPable_decl_inside(className) \
    PUPable_operator_inside(className)

#define PUPable_decl_template(className) \
    PUPable_decl_inside_template(SINGLE_ARG(className))   \
    PUPable_operator_inside(SINGLE_ARG(className))

//PUPable_decl for classes inside a namespace: inside body
#define PUPable_decl_inside(className) \
private: \
    static PUP::able *call_PUP_constructor(void); \
    static PUP::able::PUP_ID my_PUP_ID;\
public:\
    virtual const PUP::able::PUP_ID &get_PUP_ID(void) const; \
    static void register_PUP_ID(const char* name);

#define PUPable_decl_base_template(baseClassName, className)                   \
  PUPable_decl_inside_base_template(SINGLE_ARG(baseClassName),                 \
                                    SINGLE_ARG(className))                     \
  PUPable_operator_inside(SINGLE_ARG(className))

#define PUPable_decl_inside_template(className)	\
private: \
    static PUP::able* call_PUP_constructor(void) { \
        className* pupobj = new className((CkMigrateMessage*)0); \
        _MEMCHECK(pupobj); \
        return pupobj; } \
    static PUP::able::PUP_ID my_PUP_ID;\
public: \
    virtual const PUP::able::PUP_ID &get_PUP_ID(void) const { \
        return my_PUP_ID; }					\
    static void register_PUP_ID(const char* name) { \
        my_PUP_ID=register_constructor(name,call_PUP_constructor);}

#define PUPable_decl_inside_base_template(baseClassName, className)            \
private:                                                                       \
    static PUP::able *call_PUP_constructor(void) {                             \
        className* pupobj = new className((CkMigrateMessage*)0); \
        _MEMCHECK(pupobj); \
        return pupobj; \
    }                                                                          \
    static PUP::able::PUP_ID my_PUP_ID;                                        \
                                                                               \
public:                                                                        \
    virtual const PUP::able::PUP_ID &get_PUP_ID(void) const {                  \
        return my_PUP_ID;                                                      \
    }                                                                          \
    static void register_PUP_ID(const char *name) {                            \
        my_PUP_ID =                                                            \
           baseClassName::register_constructor(name, call_PUP_constructor);    \
    }

//PUPable_decl for classes inside a namespace: in header at file scope
#define PUPable_decl_outside(className) \
     PUPable_operator_outside(className)

//PUPable_decl for classes inside a namespace: in header at file scope
#define PUPable_decl_outside_template(templateParameters,className)	\
     template<templateParameters> inline void operator|(PUP::er &p,className &a) {a.pup(p);} \
     template<templateParameters> inline void operator|(PUP::er &p,className* &a) { \
         PUP::able *pa=a;  p(&pa);  a=(className *)pa; } \
     template<templateParameters> PUP::able *className::call_PUP_constructor(void) { \
         className* pupobj = new className((CkMigrateMessage*)0); \
         _MEMCHECK(pupobj); \
         return pupobj; } \
     template<templateParameters> const PUP::able::PUP_ID &className::get_PUP_ID(void) const { \
         return className::my_PUP_ID; }					\
     template<templateParameters> void className::register_PUP_ID(const char* name) { \
         my_PUP_ID=register_constructor(name,className::call_PUP_constructor);}


//Declarations to include in an abstract PUP::able's body.
//  Abstract PUP::ables do not need def or reg.
#define PUPable_abstract(className) \
public:\
    virtual const PUP::able::PUP_ID &get_PUP_ID(void) const =0; \
    PUPable_operator_inside(className)

//Definitions to include exactly once at file scope
#define PUPable_def(className) \
	PUP::able *className::call_PUP_constructor(void) { \
		className* pupobj = new className((CkMigrateMessage*)0); \
		_MEMCHECK(pupobj); \
		return pupobj; } \
	const PUP::able::PUP_ID &className::get_PUP_ID(void) const\
		{ return className::my_PUP_ID; }\
	PUP::able::PUP_ID className::my_PUP_ID;\
	void className::register_PUP_ID(const char* name)\
		{my_PUP_ID=register_constructor(name,\
		              className::call_PUP_constructor);}

//Definitions to include exactly once at file scope
#define PUPable_def_template(className) \
	template<> PUP::able::PUP_ID className::my_PUP_ID = 0;

//Definitions to include exactly once at file scope
#define PUPable_def_template_nonInst(className) \
	PUP::able::PUP_ID className::my_PUP_ID = 0;

//Code to execute exactly once at program start time
#define PUPable_reg(className)	\
    className::register_PUP_ID(#className);
#define PUPable_reg2(classIdentifier,className)	\
    classIdentifier::register_PUP_ID(className);

  inline void operator|(er &p,able &a) {a.pup(p);}
  inline void operator|(er &p,able* &a) {p(&a);}
} //<- End namespace PUP


//Holds a pointer to a (possibly dynamically allocated) PUP::able.
//  Extracting the pointer hands the deletion responsibility over.
//  This is used by parameter marshalling, which doesn't work well 
//  with bare pointers.
//   CkPointer<T> t   is the parameter-marshalling equivalent of   T *t
template <class T>
class CkPointer {
	T *allocated; //Pointer that PUP dynamically allocated for us (recv only)
	T *ptr; //Read-only pointer

	CkPointer(const CkPointer<T> &) = delete;
	void operator=(const CkPointer<T> &) = delete;
	void operator=(CkPointer<T> &&) = delete;
protected:
	T *peek(void) {return ptr;}
	CkPointer(T *src, T *alloc): allocated(alloc), ptr(src) {}
public:
	/// Used on the send side, and does *not* delete the object.
	CkPointer(T *src): CkPointer(src, nullptr) {}
	/// Begin completely empty: used on marshalling recv side.
	CkPointer(void): CkPointer(nullptr, nullptr) {}
	/// Move constructor (copy src fields, then invalidate)
	CkPointer(CkPointer<T> &&src): CkPointer(src.ptr, src.allocated)
	{ src.allocated = src.ptr = nullptr; }
	
	~CkPointer() { if (allocated) delete allocated; }
	
	/// Extract the object held by this class.  
	///  Deleting the pointer is now the user's responsibility
	inline operator T* () { allocated=0; return ptr; }
	
	inline void pup(PUP::er &p) {
		PUP::ptr_helper<T>()(p, ptr);
		if (p.isUnpacking()) {
			allocated = ptr;
		}
	}
	friend inline void operator|(PUP::er &p,CkPointer<T> &v) {v.pup(p);}
};
#define PUPable_marshall CkPointer

//Like CkPointer, but keeps deletion responsibility forever.
//   CkReference<T> t  is the parameter-marshalling equivalent of   T &t
template<class T>
class CkReference : private CkPointer<T> {
public:
	/// Used on the send side, and does *not* delete the object.
	CkReference(T &src)   ///< Marshall this object.
		:CkPointer<T>(&src) { }
	
	/// Begin completely empty: used on the recv side.
	CkReference(void) {}
	
	/// Look at the object held by this class.  Does *not* hand over
	/// deletion responsiblity.
	inline operator T& () { return *this->peek(); }
	
	inline void pup(PUP::er &p) {CkPointer<T>::pup(p);}
	
	friend inline void operator|(PUP::er &p,CkReference<T> &v) {v.pup(p);}
};

// For people that forget the "::"
typedef PUP::er PUPer;
typedef PUP::able PUPable;

/******** PUP via pipe: another way to access PUP::ers *****
The parameter marshalling system pups each variable v using just:
     p|v;
Thus we need a "void operator|(PUP::er &p,T &v)" for all types
that work with parameter marshalling. 
*/

namespace PUP {
	/** 
	  Traits class: decide if the type T can be safely
	  pupped as raw bytes.  This is true of classes that
	  do not contain pointers and do not need pup routines.
	  Use this like:
	     if (PUP::as_bytes<someClass>::value) { ... }
	*/
	template<class T> class as_bytes {
    /* default is to not pack as bytes by default */
		public: enum {value=0};
	};


// Defines is_pupable to allow enums to be pupped in pup_stl.h
namespace details {

template <typename... Ts>
struct make_void {
  typedef void type;
};
template <typename... Ts>
using void_t = typename make_void<Ts...>::type;

template <typename T, typename U = void>
struct is_pupable : std::false_type {};

template <typename T>
struct is_pupable<
    T, void_t<decltype(std::declval<T>().pup(std::declval<PUP::er &>()))>>
    : std::true_type {};

}

/**
  Default operator|: call pup routine (as long as T has a pup function).
*/
template <class T, typename std::enable_if<details::is_pupable<T>::value, int>::type = 0>
inline void operator|(PUP::er &p, T &t) {
  p.syncComment(PUP::sync_begin_object);
  t.pup(p);
  p.syncComment(PUP::sync_end_object);
}

/**
  Default PUParray: pup each element.
*/
template<class T>
inline void PUParray(PUP::er &p,T *t,size_t n) {
	p.syncComment(PUP::sync_begin_array);
	for (size_t i=0;i<n;i++) {
		p.syncComment(PUP::sync_item);
		p|t[i];
	}
	p.syncComment(PUP::sync_end_array);
}

/// Copy this type as raw memory (like memcpy).
#define PUPbytes(type) \
  namespace PUP { inline void operator|(PUP::er &p,type &t) {p((char *)&t,sizeof(type));} } \
  namespace PUP { inline void PUParray(PUP::er &p,type *ta,size_t n) { p((char *)ta,n*sizeof(type)); } } \
  namespace PUP { template<> class as_bytes<type> { \
  	public: enum {value=1};  \
  }; }

/// Make PUP work with this function pointer type, copied as raw bytes.
#define PUPfunctionpointer(fnPtrType) \
  inline void operator|(PUP::er &p,fnPtrType &t) {p((char *)&t,sizeof(fnPtrType));}

/// Make PUP work with this enum type, copied as an "int".
#define PUPenum(enumType) \
  inline void operator|(PUP::er &p,enumType &e) { int v=e;  p|v; e=v; }

}

/**
  For all builtin types, like "int",
  operator| and PUParray use p(t) and p(ta,n).
*/
#define PUP_BUILTIN_SUPPORT(type) \
  namespace PUP { inline void operator|(er &p,type &t) {p(t);} } \
  namespace PUP { inline void PUParray(er &p,type *ta,size_t n) { p(ta,n); } } \
  namespace PUP { template<> class as_bytes<type> { \
  	public: enum {value=1};  \
  }; }
PUP_BUILTIN_SUPPORT(signed char)
#if CMK_SIGNEDCHAR_DIFF_CHAR
PUP_BUILTIN_SUPPORT(char)
#endif
PUP_BUILTIN_SUPPORT(unsigned char)
PUP_BUILTIN_SUPPORT(short)
PUP_BUILTIN_SUPPORT(int)
PUP_BUILTIN_SUPPORT(long)
PUP_BUILTIN_SUPPORT(unsigned short)
PUP_BUILTIN_SUPPORT(unsigned int)
PUP_BUILTIN_SUPPORT(unsigned long)
PUP_BUILTIN_SUPPORT(float)
PUP_BUILTIN_SUPPORT(double)
PUP_BUILTIN_SUPPORT(bool)
#if CMK_LONG_DOUBLE_DEFINED
PUP_BUILTIN_SUPPORT(long double)
#endif
#ifdef CMK_PUP_LONG_LONG
PUP_BUILTIN_SUPPORT(CMK_PUP_LONG_LONG)
PUP_BUILTIN_SUPPORT(unsigned CMK_PUP_LONG_LONG)
#endif
#if CMK_HAS_INT16
PUP_BUILTIN_SUPPORT(CmiInt16)
PUP_BUILTIN_SUPPORT(CmiUInt16)
#endif


//This macro is useful in simple pup routines:
//  It's just p|x, but it also documents the *name* of the variable.
// You must have a PUP::er named p.
#define PUPn(field) \
  do{  if (p.hasComments()) p.comment(#field); p|field; } while(0)

//Like PUPn(x), above, but for arrays.
#define PUPv(field,len) \
  do{  if (p.hasComments()) p.comment(#field); PUParray(p,field,len); } while(0)

namespace PUP {
template <typename T>
struct ptr_helper<T, true> {
  inline void operator()(PUP::er &p, T *&t) const {
    bool is_nullptr = nullptr == t;
    p | is_nullptr;
    if (!is_nullptr) {
      PUP::able *t_able = static_cast<PUP::able *>(t);
      p | t_able;
      if (p.isUnpacking()) t = static_cast<T *>(t_able);
    }
  }
};

template <typename T>
struct ptr_helper<T, false> {
  inline void operator()(PUP::er &p, T *&t) const {
    bool is_nullptr = nullptr == t;
    p | is_nullptr;
    if (!is_nullptr) {
      if (p.isUnpacking()) {
        initialize_ptr(t);
      }
      p | *t;
    }
  }

 protected:
  template <typename U>
  inline typename std::enable_if<std::is_constructible<U, reconstruct>::value>::type
  initialize_ptr(U *&u) const {
    u = new U(reconstruct());
  }

  template <typename U>
  inline typename std::enable_if<!std::is_constructible<U, reconstruct>::value>::type
  initialize_ptr(U *&u) const {
    u = new U();
  }
};
}

#endif //def __CK_PUP_H


