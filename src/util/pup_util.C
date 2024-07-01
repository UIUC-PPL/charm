/*
Pack/UnPack Library for UIUC Parallel Programming Lab
Orion Sky Lawlor, olawlor@uiuc.edu, 4/5/2000

This library allows you to easily pack an array, structure,
or object into a memory buffer or disk file, and then read 
the object back later.  The library will also handle translating
between different machine representations.

This file is needed because virtual function definitions in
header files cause massive code bloat-- hence the PUP library
virtual functions are defined here.

*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>

#include "converse.h"
#include "pup.h"
#include "ckhashtable.h"

#include "conv-rdma.h"
#if defined(_WIN32)
#include <io.h>

int pwrite(int fd, const void *buf, size_t nbytes, __int64 offset)
{
  __int64 ret = _lseek(fd, offset, SEEK_SET);

  if (ret == -1) {
    return(-1);
  }
  return(_write(fd, buf, nbytes));
}
#define NO_UNISTD_NEEDED
#endif

#if defined(__PGIC__)
// PGI compilers define funny feature flags that lead to standard
// headers omitting this prototype

extern "C" {

extern ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset);
}
#define NO_UNISTD_NEEDED
#endif

#if !defined(NO_UNISTD_NEEDED)
#include <unistd.h>
#endif

PUP::er::~er() {}

void PUP::er::operator()(able& a)
  {a.pup(*this);}

void PUP::er::comment(const char *message)
  { /* ignored by default */ }

const char * PUP::er::typeString() const
{
  if (isSizing()) return "sizing";
  else if (isPacking()) return "packing";
  else if (isUnpacking()) return "unpacking";
  return "unknown";
}

void PUP::er::synchronize(unsigned int m)
  { /* ignored by default */ }

/*define CK_CHECK_PUP to get type and bounds checking during Pack and unpack.
This checking substantially slows down PUPing, and increases the space
required by packed objects. It can save hours of debugging, however.
*/
#ifdef CK_CHECK_PUP
static int bannerDisplayed=0;
static void showBanner(void) {
	bannerDisplayed=1;
	fprintf(stderr,"CK_CHECK_PUP pup routine checking enabled\n");
	CmiPrintf("CK_CHECK_PUP pup routine checking enabled\n");
}

class pupCheckRec {
	unsigned char magic[4];//Cannot use "int" because of alignment
	unsigned char type;
	unsigned char length[8];
	enum {pupMagic=0xf36c5a21,typeMask=0x75};
	int getMagic(void) const {return (magic[3]<<24)+(magic[2]<<16)+(magic[1]<<8)+magic[0];}
	void setMagic(int v) {for (int i=0;i<4;i++) magic[i]=(v>>(8*i));}
	PUP::dataType getType(void) const {return (PUP::dataType)(type^typeMask);}
	void setType(PUP::dataType v) {type=v^typeMask;}
	size_t getLength(void) const {
          size_t v = 0;
          for (int i=0;i<8;i++) v += (length[i]<<(8*i));
          return v;
        }
	void setLength(size_t v) {for (int i=0;i<8;i++) length[i]=(v>>(8*i));}
	
	/*Compare the packed value (from us) and the unpacked value
	  (from the user).
	 */
	void compare(const char *kind,const char *why,int packed,int unpacked) const
	{
		if (packed==unpacked) return;
		//If we get here, there is an error in the user's pack/unpack routine
		fprintf(stderr,"CK_CHECK_PUP error!\nPacked %s (%d, or %08x) does "
			"not equal unpacked value (%d, or %08x)!\nThis means %s\n",
			kind,packed,packed,unpacked,unpacked,why);
		CmiPrintf("CK_CHECK_PUP error! Run with debugger for more info.\n");
		//Invoke the debugger
		abort();
	}
public:
	void write(PUP::dataType t,size_t n) {
		if (!bannerDisplayed) showBanner();
		setMagic(pupMagic);
		type=t^typeMask;
		setLength(n);
	}
	void check(PUP::dataType t,size_t n) const {
		compare("magic number",
			"you unpacked more than you packed, or the values were corrupted during transport",
			getMagic(),pupMagic);
		compare("data type",
			"the pack and unpack paths do not match up",
			getType(),t);
		compare("length",
			"you may have forgotten to pup the array length",
			getLength(),n);
	}
};
#endif


void PUP::sizer::bytes(void * /*p*/,size_t n,size_t itemSize,dataType /*t*/)
{
#ifdef CK_CHECK_PUP
	nBytes+=sizeof(pupCheckRec);
#endif
	nBytes+=n*itemSize;
}

/*Memory PUP::er's*/
void PUP::toMem::bytes(void *p,size_t n,size_t itemSize,dataType t)
{
#ifdef CK_CHECK_PUP
	((pupCheckRec *)buf)->write(t,n);
	buf+=sizeof(pupCheckRec);
#endif
	n*=itemSize;
	memcpy((void *)buf,p,n); 
	buf+=n;
}
void PUP::fromMem::bytes(void *p,size_t n,size_t itemSize,dataType t)
{
#ifdef CK_CHECK_PUP
	((pupCheckRec *)buf)->check(t,n);
	buf+=sizeof(pupCheckRec);
#endif
	n*=itemSize; 
	memcpy(p,(const void *)buf,n); 
	buf+=n;
}

void PUP::sizer::pup_buffer_async(void *&p,size_t n, size_t itemSize, dataType t) {
#ifdef CK_CHECK_PUP
	nBytes+=sizeof(pupCheckRec);
#endif
	nBytes+=sizeof(CmiNcpyBuffer);
}

void PUP::sizer::pup_buffer_async(void *&p,size_t n, size_t itemSize, dataType t, std::function<void *(size_t)> allocate, std::function<void (void *)> deallocate) {
	pup_buffer_async(p, n, itemSize, t);
}

void PUP::toMem::pup_buffer_async(void *&p,size_t n,size_t itemSize,dataType t) {
	pup_buffer_async(p, n, itemSize, t, malloc, free);
}

void PUP::fromMem::pup_buffer_async_generic(void *&p,size_t n, size_t itemSize, dataType t, std::function<void *(size_t)> allocate, bool isMalloc) {
#ifdef CK_CHECK_PUP
	((pupCheckRec *)buf)->check(t,n);
	buf+=sizeof(pupCheckRec);
#endif
	// Unpup a CmiNcpyBuffer object with the callback
	CmiNcpyBuffer src;

	PUP::fromMem fMem(buf);
	fMem|src;
	CmiAssert(sizeof(src) == sizeof(CmiNcpyBuffer));

	// Allocate only if inter-process
	if(isUnpacking()) {
		if(CmiNodeOf(src.pe)!=CmiMyNode()) {
			if(isMalloc)
				p = malloc(n * itemSize);
			else
				p = allocate(n);
			CmiNcpyBuffer dest(p, n*itemSize);
			zcPupGet(src, dest);
		} else {
			p = (void *)src.ptr;
			// Free the allocated zcPupSourceInfo
			zcPupSourceInfo *srcInfo = (zcPupSourceInfo *)(src.ref);
			delete srcInfo;
		}
	}
	buf+=sizeof(src);
}

void PUP::fromMem::pup_buffer_async(void *&p,size_t n,size_t itemSize,dataType t) {
	pup_buffer_async_generic(p, n, itemSize, t, malloc, true);
}

void PUP::toMem::pup_buffer_async(void *&p,size_t n, size_t itemSize, dataType t, std::function<void *(size_t)> allocate, std::function<void (void *)> deallocate) {
#ifdef CK_CHECK_PUP
	((pupCheckRec *)buf)->write(t,n);
	buf+=sizeof(pupCheckRec);
#endif
	// Create a CmiNcpyBuffer object with the callback
	CmiNcpyBuffer src(p, n*itemSize);
	zcPupSourceInfo *srcInfo = zcPupAddSource(src, deallocate);
	src.ref = srcInfo;
	CmiAssert(sizeof(src) == sizeof(CmiNcpyBuffer));
	PUP::toMem tMem(buf);
	tMem|src;
	buf+=sizeof(src);
}

void PUP::fromMem::pup_buffer_async(void *&p,size_t n, size_t itemSize, dataType t, std::function<void *(size_t)> allocate, std::function<void (void *)> deallocate) {
	pup_buffer_async_generic(p, n, itemSize, t, allocate, false);
}

extern "C" {

int CmiOpen(const char *pathname, int flags, int mode)
{
        int fd = -1;
        while (1) {
#if defined(_WIN32)
          fd = _open(pathname, flags, mode);
#else
          fd = open(pathname, flags, mode);
#endif
          if (fd == -1 && errno==EINTR) {
            CmiError("Warning: CmiOpen retrying on %s\n", pathname);
            continue;
          }
          else
            break;
        }
        return fd;
}

// dealing with short write
size_t CmiFwrite(const void *ptr, size_t size, size_t nmemb, FILE *f)
{
        size_t nwritten = 0;
        const char *buf = (const char *)ptr;
        double firsttime = 0;
	while (nwritten < nmemb) {
          size_t ncur = fwrite(buf+nwritten*size,size,nmemb-nwritten,f);
          if (ncur <= 0) {
            if  (errno == EINTR)
              CmiError("Warning: CmiFwrite retrying ...\n");
            else if(errno == ENOMEM)
	    {
	      if(firsttime == 0) firsttime = CmiWallTimer();
              if(CmiWallTimer()-firsttime > 300)
		break;
            }
	    else
              break;
          }
          else
            nwritten += ncur;
        }
	if(firsttime != 0)
	  CmiError("Warning: CmiFwrite retried for %lf ...\n", CmiWallTimer() - firsttime);

        return nwritten;
}

CmiInt8 CmiPwrite(int fd, const char *buf, size_t bytes, size_t offset)
{
  size_t origBytes = bytes;
  while (bytes > 0) {
    CmiInt8 ret = pwrite(fd, buf, bytes, offset);
    if (ret < 0) {
      if (errno == EINTR) {
        continue;
      } else {
        return ret;
      }
    }
    bytes -= ret;
    buf += ret;
    offset += ret;
  }
  return origBytes;
}

size_t CmiFread(void *ptr, size_t size, size_t nmemb, FILE *f)
{
        size_t nread = 0;
        char *buf = (char *)ptr;
        while (nread < nmemb) {
          size_t ncur = fread(buf + nread*size, size, nmemb-nread, f);
          if (ncur <= 0) {
            if  (errno == EINTR)
              CmiError("Warning: CmiFread retrying ...\n");
            else
              break;
          }
          else
            nread += ncur;
        }
        return nread;
}

FILE *CmiFopen(const char *path, const char *mode)
{
        FILE *fp = NULL;
        while (1) {
          fp = fopen(path, mode);
          if (fp == 0 && errno==EINTR) {
            CmiError("Warning: CmiFopen retrying on %s\n", path);
            continue;
          }
          else
            break;
        }
        return fp;
}

// more robust fclose that handling interrupt
int CmiFclose(FILE *fp)
{
        int status = 0;
        while (1) {
          status = fflush(fp);
          if (status != 0 && errno==EINTR) {
            CmiError("Warning: CmiFclose flush retrying ...\n");
            continue;
          }
          else
            break;
        }
        if (status != 0) return status;
        while (1) {
          status = fclose(fp);
          if (status != 0 && errno==EINTR) {
            CmiError("Warning: CmiFclose retrying ...\n");
            continue;
          }
          else
            break;
        }
        return status;
}

} // extern "C"

/*Disk PUP::er's*/
void PUP::toDisk::bytes(void *p,size_t n,size_t itemSize,dataType /*t*/)
{/* CkPrintf("writing %d bytes\n",itemSize*n); */ 
  if(CmiFwrite(p,itemSize,n,F) != n)
  {
    error = true;
  }
}

void PUP::toDisk::pup_buffer_async(void *&p,size_t n,size_t itemSize,dataType t) {
  bytes(p, n, itemSize, t);
  if(isDeleting()) free(p);
}

void PUP::toDisk::pup_buffer_async(void *&p,size_t n, size_t itemSize, dataType t, std::function<void *(size_t)> allocate, std::function<void (void *)> deallocate) {
  bytes(p, n, itemSize, t);
  if(isDeleting()) deallocate(p);
}

void PUP::fromDisk::bytes(void *p,size_t n,size_t itemSize,dataType /*t*/)
{/* CkPrintf("reading %d bytes\n",itemSize*n); */ CmiFread(p,itemSize,n,F);}

void PUP::fromDisk::pup_buffer_async(void *&p,size_t n,size_t itemSize,dataType t) {
  if(isUnpacking()) p = malloc(n * itemSize);
  bytes(p, n, itemSize, t);
}

void PUP::fromDisk::pup_buffer_async(void *&p,size_t n, size_t itemSize, dataType t, std::function<void *(size_t)> allocate, std::function<void (void *)> deallocate) {
  if(isUnpacking()) p = allocate(n * itemSize);
  bytes(p, n, itemSize, t);
}

/****************** Seek support *******************
For seeking:
Occasionally, one will need to pack and unpack items in different
orders (e.g., pack the user data, then the runtime support; but
unpack the runtime support first, then the user data).  These routines
support this, via the "PUP::seekBlock" class.

The abstraction is a (nestable) "seek block", which may contain
several "seek sections".  A typical use is:
//Code:
	PUP::seekBlock s(p,2);
	if (p.isUnpacking()) {s.seek(0); rt.pack(p); }
	s.seek(1); ud.pack(p); 
	if (p.isPacking()) {s.seek(0); rt.pack(p); }
	s.endBlock();
*/
PUP::seekBlock::seekBlock(PUP::er &Np,int nSections)
	:nSec(nSections),p(Np) 
{
	if (nSections<0 || nSections>maxSections)
		CmiAbort("Invalid # of sections passed to PUP::seekBlock!");
	p.impl_startSeek(*this);
	if (p.isPacking()) 
	{ //Must fabricate the section table
		secTabOff=p.impl_tell(*this);
		for (int i=0;i<=nSec;i++) secTab[i]=-1;
	}
	p(secTab,nSec+1);
	hasEnded=false;
}
PUP::seekBlock::~seekBlock() 
{
	if (!hasEnded)
		endBlock();
}

void PUP::seekBlock::seek(int toSection) 
{
	if (toSection<0 || toSection>=nSec)
		CmiAbort("Invalid section # passed to PUP::seekBlock::seek!");
	if (p.isPacking()) //Build the section table
		secTab[toSection]=p.impl_tell(*this);
	else if (p.isUnpacking()) //Extract the section table
		p.impl_seek(*this,secTab[toSection]);
	/*else ignore the seeking*/
}

void PUP::seekBlock::endBlock(void) 
{
	if (p.isPacking()) {
		//Finish off and write out the section table
		secTab[nSec]=p.impl_tell(*this);
		p.impl_seek(*this,secTabOff);
		p(secTab,nSec+1); //Write out the section table
	}
	//Seek to the end of the seek block
	p.impl_seek(*this,secTab[nSec]);
	p.impl_endSeek(*this);
	hasEnded=true;
}

/** PUP::er seek implementation routines **/
/*Default seek implementations are empty, which is the 
appropriate behavior for, e.g., sizers.
*/
void PUP::er::impl_startSeek(PUP::seekBlock &s) /*Begin a seeking block*/
{}
size_t PUP::er::impl_tell(seekBlock &s) /*Give the current offset*/
{return 0;}
void PUP::er::impl_seek(seekBlock &s,size_t off) /*Seek to the given offset*/
{}
void PUP::er::impl_endSeek(seekBlock &s)/*End a seeking block*/
{}


/*Memory buffer seeking is trivial*/
void PUP::mem::impl_startSeek(seekBlock &s) /*Begin a seeking block*/
  {s.data.ptr=buf;}
size_t PUP::mem::impl_tell(seekBlock &s) /*Give the current offset*/
  {return buf-s.data.ptr;}
void PUP::mem::impl_seek(seekBlock &s,size_t off) /*Seek to the given offset*/
  {buf=s.data.ptr+off;}

/*Disk buffer seeking is also simple*/
void PUP::disk::impl_startSeek(seekBlock &s) /*Begin a seeking block*/
  {s.data.loff=ftell(F);}
size_t PUP::disk::impl_tell(seekBlock &s) /*Give the current offset*/
  {return (int)(ftell(F)-s.data.loff);}
void PUP::disk::impl_seek(seekBlock &s,size_t off) /*Seek to the given offset*/
  {fseek(F,s.data.loff+off,0);}

/*PUP::wrap_er just forwards seek calls to its wrapped PUP::er.*/
void PUP::wrap_er::impl_startSeek(seekBlock &s) /*Begin a seeking block*/
  {p.impl_startSeek(s);}
size_t PUP::wrap_er::impl_tell(seekBlock &s) /*Give the current offset*/
  {return p.impl_tell(s);}
void PUP::wrap_er::impl_seek(seekBlock &s,size_t off) /*Seek to the given offset*/
  {p.impl_seek(s,off);}
void PUP::wrap_er::impl_endSeek(seekBlock &s) /*Finish a seeking block*/
  {p.impl_endSeek(s);}
  

/**************** PUP::able support **********************
If a class C inherits from PUP::able, 
and you keep a new/delete pointer to C "C *cptr" somewhere,
you can call "p(cptr)" in your pup routine, and the object
will be saved/delete'd/new'd/restored properly with no 
additional effort, even if C has virtual methods or is actually
a subclass of C.  There is no space or time overhead for C 
objects other than the virtual function.

This is implemented by registering a constructor and ID
for each PUP::able class.  A packer can then write the ID
before the class; and unpacker can look up the constructor
from the ID.
 */

static PUP::able::PUP_ID null_PUP_ID(0); /*ID of null object*/

PUP::able *PUP::able::clone(void) const {
	// Make a new object to fill out
	PUP::able *ret=get_constructor(get_PUP_ID()) ();

	// Save our own state into a buffer
	PUP::able *mthis=(PUP::able *)this; /* cast away constness */
	size_t size;
	{ PUP::sizer ps; mthis->pup(ps); size=ps.size(); }
	void *buf=malloc(size);
	{ PUP::toMem pt(buf); mthis->pup(pt); }
	
	// Fill the new object with our values
	{ PUP::fromMem pf(buf); ret->pup(pf); }
	free(buf);
	
	return ret;
}

//Empty destructor & pup routine
PUP::able::~able() {}
void PUP::able::pup(PUP::er &p) {}

//Compute a good hash of the given string 
// (registration-time only-- allowed to be slow)
void PUP::able::PUP_ID::setName(const char *name)
{
	int i,o,n=strlen(name);
	unsigned int t[len]={0};
	for (o=0;o<n;o++)
		for (i=0;i<len;i++) {
			unsigned char c=name[o];
			unsigned int shift1=(((o+2)*(i+1)*5+4)%13);
			unsigned int shift2=(((o+2)*(i+1)*3+2)%11)+13;
			t[i]+=(c<<shift1)+(c<<shift2);
		}
	for (i=0;i<len;i++) 
		hash[i]=(unsigned char)(t[i]%20117 + t[i]%1217 + t[i]%157);
}

//Registration routines-- called at global initialization time
class PUP_regEntry {
public:
	PUP::able::PUP_ID id;
	const char *name;
	PUP::able::constructor_function ctor;
	PUP_regEntry(const char *Nname,
		const PUP::able::PUP_ID &Nid,PUP::able::constructor_function Nctor)
		:id(Nid),name(Nname),ctor(Nctor) {}
	PUP_regEntry(int zero) {
		name=NULL; //For marking "not found"
	}
};

typedef CkHashtableTslow<PUP::able::PUP_ID,PUP_regEntry> PUP_registry;

// FIXME: not SMP safe!    // gzheng
static PUP_registry *PUP_getRegistry(void) {
        static PUP_registry *reg = NULL;
	if (reg==NULL)
		reg=new PUP_registry();
	return reg;
}

const PUP_regEntry *PUP_getRegEntry(const PUP::able::PUP_ID &id,
                                    const char *const name_hint = NULL)
{
	const PUP_regEntry *cur=(const PUP_regEntry *)(
		PUP_getRegistry()->CkHashtable::get((const void *)&id) );
	if (cur==NULL) {
		if (name_hint != NULL)
			CmiAbort("Unrecognized PUP::able::PUP_ID for %s", name_hint);
		CmiAbort("Unrecognized PUP::able::PUP_ID. is there an unregistered module?");
	}
	return cur;
}

PUP::able::PUP_ID PUP::able::register_constructor
	(const char *className,constructor_function fn)
{
	PUP::able::PUP_ID id(className);
	PUP_getRegistry()->put(id)=PUP_regEntry(className,id,fn);
	return id;
}

PUP::able::constructor_function PUP::able::get_constructor
	(const PUP::able::PUP_ID &id)
{
	return PUP_getRegEntry(id)->ctor;
}

//For allocatable objects: new/delete object and call pup routine
void PUP::er::object(able** a)
{
	const PUP_regEntry *r=NULL;
	if (isUnpacking()) 
	{ //Find the object type & create the object
		PUP::able::PUP_ID id;//The object's id
		id.pup(*this);
		if (id==null_PUP_ID) {*a=NULL; return;}
		r=PUP_getRegEntry(id);
		//Invoke constructor (calls new)
		*a=(r->ctor)();
		
	} else {//Just write out the object type
		if (*a==NULL) {
			null_PUP_ID.pup(*this);
			return;
		} else {
			const PUP::able::PUP_ID &id=(*a)->get_PUP_ID();
			id.pup(*this);
			r=PUP_getRegEntry(id, typeid(**a).name());
		}
	}
	syncComment(PUP::sync_begin_object,r->name);
	(*a)->pup(*this);
	syncComment(PUP::sync_end_object);
}

/****************** Text Pup ******************/

char *PUP::toTextUtil::beginLine(void) {
  //Indent level tabs over:
  for (int i=0;i<level;i++) cur[i]='\t';
  cur[level]=0;
  return cur+level;
}
void PUP::toTextUtil::endLine(void) {
  cur=advance(cur);
}
void PUP::toTextUtil::beginEnv(const char *type,int n)
{
  char * const begin=beginLine();
  char *o=begin;
  snprintf(o,maxCount,"begin "); o+=strlen(o);
  snprintf(o,maxCount - (o-begin),type,n); o+=strlen(o);
  snprintf(o,maxCount - (o-begin)," {\n");
  endLine();
  level++;
}
void PUP::toTextUtil::endEnv(const char *type)
{
  level--;
  snprintf(beginLine(),maxCount,"} end %s;\n",type);
  endLine();
}
PUP::toTextUtil::toTextUtil(unsigned int inType,char *buf,size_t len)
  :er(inType)
{
  cur=buf;
  maxCount=len;
  level=0;
}

void PUP::toTextUtil::comment(const char *message)
{
  snprintf(beginLine(),maxCount,"//%s\n",message); endLine();
}

void PUP::toTextUtil::synchronize(unsigned int m)
{
  snprintf(beginLine(),maxCount,"sync=0x%08x\n",m); endLine();
#if 0 /* text people don't care this much about synchronization */
  char *o=beginLine();
  sprintf(o,"sync=");o+=strlen(o);
  const char *consonants="bcdfgjklmprstvxz";
  const char *vowels="aeou";
  for (int firstBit=0;firstBit<32;firstBit+=6) {
	sprintf(o,"%c%c%c", consonants[0xf&(m>>firstBit)],
		vowels[0x3&(m>>(firstBit+4))], 
		(firstBit==30)?';':'-');
	o+=strlen(o);
  }
  sprintf(o,"\n"); endLine();
#endif
}

void PUP::toTextUtil::pup_buffer_async(void *&p,size_t n,size_t itemSize,dataType t) {
  bytes(p, n, itemSize, t);
}

void PUP::toTextUtil::pup_buffer_async(void *&p,size_t n, size_t itemSize, dataType t, std::function<void *(size_t)> allocate, std::function<void (void *)> deallocate) {
  bytes(p, n, itemSize, t);
}

void PUP::toTextUtil::bytes(void *p,size_t n,size_t itemSize,dataType t) {
  if (t==Tchar) 
  { /*Character data is written out directly (rather than numerically)*/
    char * const begin=beginLine();
    char *o=begin;
    snprintf(o,maxCount,"string=");o+=strlen(o);
    *o++='\"'; /*Leading quote*/
    /*Copy each character, possibly escaped*/
    const char *c=(const char *)p;
    for (size_t i=0;i<n;i++) {
      if (c[i]=='\n') {
	snprintf(o,maxCount - (o-begin),"\\n");o+=strlen(o);
      } else if (iscntrl(c[i])) {
	snprintf(o,maxCount - (o-begin),"\\x%02X",(unsigned char)c[i]);o+=strlen(o);
      } else if (c[i]=='\\' || c[i]=='\"') {
	snprintf(o,maxCount - (o-begin),"\\%c",c[i]);o+=strlen(o);
      } else
	*o++=c[i];
    }
    /*Add trailing quote and newline*/
    snprintf(o,maxCount - (o-begin),"\";\n");o+=strlen(o);
    endLine();
  } else if (t==Tbyte || t==Tuchar)
  { /*Byte data is written out in hex (rather than decimal) */
    beginEnv("byte %d",n);
    const unsigned char *c=(const unsigned char *)p;
    char * const begin=beginLine();
    char *o=begin;
    for (size_t i=0;i<n;i++) {
      snprintf(o,maxCount - (o-begin),"%02X ",c[i]);o+=strlen(o);
      if (i%25==24 && (i+1!=n)) 
      { /* This line is too long-- wrap it */
	snprintf(o,maxCount - (o-begin),"\n"); o+=strlen(o);
	endLine(); o=beginLine();
      }
    }
    snprintf(o,maxCount - (o-begin),"\n");
    endLine();
    endEnv("byte");
  }
  else
  { /*Ordinary number-- write out in decimal */
    if (n!=1) beginEnv("array %d",n);
    for (size_t i=0;i<n;i++) {
      char * const o=beginLine();
      switch(t) {
      case Tshort: snprintf(o,maxCount,"short=%d;\n",((short *)p)[i]); break;
      case Tushort: snprintf(o,maxCount,"ushort=%u;\n",((unsigned short *)p)[i]); break;
      case Tint: snprintf(o,maxCount,"int=%d;\n",((int *)p)[i]); break;
      case Tuint: snprintf(o,maxCount,"uint=%u;\n",((unsigned int *)p)[i]); break;
      case Tlong: snprintf(o,maxCount,"long=%ld;\n",((long *)p)[i]); break;
      case Tulong: snprintf(o,maxCount,"ulong=%lu;\n",((unsigned long *)p)[i]); break;
      case Tfloat: snprintf(o,maxCount,"float=%.7g;\n",((float *)p)[i]); break;
      case Tdouble: snprintf(o,maxCount,"double=%.15g;\n",((double *)p)[i]); break;
      case Tbool: snprintf(o,maxCount,"bool=%s;\n",((bool *)p)[i]?"true":"false"); break;
#if CMK_LONG_DOUBLE_DEFINED
      case Tlongdouble: snprintf(o,maxCount,"longdouble=%Lg;\n",((long double *)p)[i]);break;
#endif
#ifdef CMK_PUP_LONG_LONG
      case Tlonglong: snprintf(o,maxCount,"longlong=%lld;\n",((CMK_PUP_LONG_LONG *)p)[i]);break;
      case Tulonglong: snprintf(o,maxCount,"ulonglong=%llu;\n",((unsigned CMK_PUP_LONG_LONG *)p)[i]);break;
#endif
      case Tpointer: snprintf(o,maxCount,"pointer=%p;\n",((void **)p)[i]); break;
      default: CmiAbort("Unrecognized pup type code!");
      }
      endLine();
    }
    if (n!=1) endEnv("array");
  }
}
void PUP::toTextUtil::object(able** a) {
  beginEnv("object");
  er::object(a);
  endEnv("object");
}


//Text sizer
char *PUP::sizerText::advance(char *cur) {
  charCount+=strlen(cur);
  return line;
}

PUP::sizerText::sizerText(void)
  :toTextUtil(IS_SIZING+IS_COMMENTS,line,sizerText::lineLen),charCount(0) { }

//Text packer
char *PUP::toText::advance(char *cur) {
  charCount+=strlen(cur);
  return buf+charCount;
}

PUP::toText::toText(char *outBuf, size_t len)
  :toTextUtil(IS_PACKING+IS_COMMENTS,outBuf,len),buf(outBuf),charCount(0) { }

/************** To/from text FILE ****************/
void PUP::toTextFile::pup_buffer_async(void *&p,size_t n,size_t itemSize,dataType t) {
  bytes(p, n, itemSize, t);
}

void PUP::toTextFile::pup_buffer_async(void *&p,size_t n, size_t itemSize, dataType t, std::function<void *(size_t)> allocate, std::function<void (void *)> deallocate) {
  bytes(p, n, itemSize, t);
}

void PUP::toTextFile::bytes(void *p,size_t n,size_t itemSize,dataType t)
{
  for (size_t i=0;i<n;i++) 
    switch(t) {
    case Tchar: fprintf(f," '%c'",((char *)p)[i]); break;
    case Tuchar:
    case Tbyte: fprintf(f," %02X",((unsigned char *)p)[i]); break;
    case Tshort: fprintf(f," %d",((short *)p)[i]); break;
    case Tushort: fprintf(f," %u",((unsigned short *)p)[i]); break;
    case Tint: fprintf(f," %d",((int *)p)[i]); break;
    case Tuint: fprintf(f," %u",((unsigned int *)p)[i]); break;
    case Tlong: fprintf(f," %ld",((long *)p)[i]); break;
    case Tulong: fprintf(f," %lu",((unsigned long *)p)[i]); break;
    case Tfloat: fprintf(f," %.7g",((float *)p)[i]); break;
    case Tdouble: fprintf(f," %.15g",((double *)p)[i]); break;
    case Tbool: fprintf(f," %s",((bool *)p)[i]?"true":"false"); break;
#if CMK_LONG_DOUBLE_DEFINED
    case Tlongdouble: fprintf(f," %Lg",((long double *)p)[i]);break;
#endif
#ifdef CMK_PUP_LONG_LONG
    case Tlonglong: fprintf(f," %lld",((CMK_PUP_LONG_LONG *)p)[i]);break;
    case Tulonglong: fprintf(f," %llu",((unsigned CMK_PUP_LONG_LONG *)p)[i]);break;
#endif
    case Tpointer: fprintf(f," %p",((void **)p)[i]); break;
    default: CmiAbort("Unrecognized pup type code!");
    };
  fprintf(f,"\n");
}
void PUP::toTextFile::comment(const char *message)
{
  fprintf(f,"! %s\n",message);
}

void PUP::fromTextFile::parseError(const char *what) {
  // find line number by counting how many returns
  long cur = ftell(f);
  int lineno=0;
  rewind(f);
  while (!feof(f)) {
     char c;
     if (fscanf(f,"%c",&c) != 1) {
         CmiAbort("PUP> reading text from file failed!");
     }
     if (c=='\n') lineno++;
     if (ftell(f) > cur) break;
  }
  fprintf(stderr,"Parse error during pup from text file: %s at line: %d\n",what, lineno);
  CmiAbort("Parse error during pup from text file!\n");
}
int PUP::fromTextFile::readInt(const char *fmt) {
  int ret=0;
  if (1!=fscanf(f,fmt,&ret)) {
	if (feof(f)) return 0; /* start spitting out zeros at EOF */
  	else parseError("could not match integer");
  }
  return ret;
}
unsigned int PUP::fromTextFile::readUint(const char *fmt) {
  unsigned int ret=0;
  if (1!=fscanf(f,fmt,&ret))  {
	if (feof(f)) return 0u; /* start spitting out zeros at EOF */
	else parseError("could not match unsigned integer");
  }
  return ret;  
}
CMK_TYPEDEF_INT8 PUP::fromTextFile::readLongInt(const char *fmt) {
  CMK_TYPEDEF_INT8 ret=0;
  if (1!=fscanf(f,fmt,&ret)) {
    if (feof(f)) return 0u;
    else parseError("could not match large integer");
  }
  return ret;
}
double PUP::fromTextFile::readDouble(void) {
  double ret=0;
  if (1!=fscanf(f,"%lg",&ret)) {
  	if (feof(f)) return 0.0; /* start spitting out zeros at EOF */
	else parseError("could not match double");
  }
  return ret;
}

void PUP::fromTextFile::pup_buffer_async(void *&p,size_t n,size_t itemSize,dataType t) {
  bytes(p, n, itemSize, t);
}

void PUP::fromTextFile::pup_buffer_async(void *&p,size_t n, size_t itemSize, dataType t, std::function<void *(size_t)> allocate, std::function<void (void *)> deallocate) {
  bytes(p, n, itemSize, t);
}

void PUP::fromTextFile::bytes(void *p,size_t n,size_t itemSize,dataType t)
{
  for (size_t i=0;i<n;i++) 
    switch(t) {
    case Tchar: 
      if (1!=fscanf(f," '%c'",&((char *)p)[i]))
	parseError("Could not match character");
      break;
    case Tuchar:
    case Tbyte: ((unsigned char *)p)[i]=(unsigned char)readInt("%02X"); break;
    case Tshort:((short *)p)[i]=(short)readInt(); break;
    case Tushort: ((unsigned short *)p)[i]=(unsigned short)readUint(); break;
    case Tint:  ((int *)p)[i]=readInt(); break;
    case Tuint: ((unsigned int *)p)[i]=readUint(); break;
    case Tlong: ((long *)p)[i]=readInt(); break;
    case Tulong:((unsigned long *)p)[i]=readUint(); break;
    case Tfloat: ((float *)p)[i]=(float)readDouble(); break;
    case Tdouble:((double *)p)[i]=readDouble(); break;
#if CMK_LONG_DOUBLE_DEFINED
    case Tlongdouble: {
      long double ret=0;
      if (1!=fscanf(f,"%Lg",&ret)) parseError("could not match long double");
      ((long double *)p)[i]=ret;
    } break;
#endif
#ifdef CMK_PUP_LONG_LONG
    case Tlonglong: {
      CMK_PUP_LONG_LONG ret=0;
      if (1!=fscanf(f,"%lld",&ret)) parseError("could not match long long");
      ((CMK_PUP_LONG_LONG *)p)[i]=ret;
    } break;
    case Tulonglong: {
      unsigned CMK_PUP_LONG_LONG ret=0;
      if (1!=fscanf(f,"%llu",&ret)) parseError("could not match unsigned long long");
      ((unsigned CMK_PUP_LONG_LONG *)p)[i]=ret;
    } break;
#endif
    case Tbool: {
      char tmp[20];
      if (1!=fscanf(f," %19s",tmp)) parseError("could not read boolean string");
      bool val=false;
      if (0==strcmp(tmp,"true")) val=true;
      else if (0==strcmp(tmp,"false")) val=false;
      else parseError("could not recognize boolean string");
      ((bool *)p)[i]=val; 
    }
      break;
    case Tpointer: {
      void *ret=0;
      if (1!=fscanf(f,"%p",&ret)) parseError("could not match pointer");
      ((void **)p)[i]=ret;
    } break;
    default: CmiAbort("Unrecognized pup type code!");
    };
}
void PUP::fromTextFile::comment(const char *message)
{
  char c;
  //Skip to the start of the message:
  while (isspace(c=fgetc(f))) {}
  
  if (c!='!') return; //This isn't the start of a comment
  //Skip over the whole line containing the comment:
  char *commentBuf=(char *)CmiTmpAlloc(1024);
  if (fgets(commentBuf,1024,f) == NULL) {
    CmiAbort("PUP> skipping over comment in text file failed!");
  }
  CmiTmpFree(commentBuf);
}







