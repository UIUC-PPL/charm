/*
Pack/UnPack Library for UIUC Parallel Programming Lab
Orion Sky Lawlor, olawlor@uiuc.edu, 4/5/2000

This part of the pack/unpack library handles translating
between different binary representations for integers and floats.
All machines are assumed to be byte-oriented.

Currently supported are converting between 8,16,32,64, and 128-bit
integers, and swapping bytes between big and little integers and
big and little-endian IEEE 32- and 64-bit floats.

*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pup.h"

//////////////////// MachineInfo utilities ///////////////////

//This 4-byte sequence identifies a PPL machineInfo structure.
static const unsigned char machInfo_magic[4]={0x10,0xea,0xbd,0xf9};

//Return true if our magic number is valid.
bool PUP::machineInfo::valid(void) const
{
	for (int i=0;i<4;i++)
		if (magic[i]!=machInfo_magic[i])
			return false;
	return true;
}

//Return true if we differ from the current (running) machine.
bool PUP::machineInfo::needsConversion(void) const
{
	const machineInfo &m=current();
	if (intFormat==m.intFormat && floatFormat==m.floatFormat &&
	    intBytes[0]==m.intBytes[0] && intBytes[1]==m.intBytes[1] &&
	    intBytes[2]==m.intBytes[2] && intBytes[3]==m.intBytes[3] && 
	    floatBytes==m.floatBytes && doubleBytes==m.doubleBytes && 
	    boolBytes==m.boolBytes && pointerBytes==m.pointerBytes
	   )
		return false;//No conversion needed
	else 
		return true;//Some differences-- convert
}

////////// For getting info. about the current machine /////////
static int getIntFormat(void)
{
	int test=0x1c;
	unsigned char *c=(unsigned char *)&test;
	if (c[sizeof(int)-1]==0x1c) 
		//Macintosh and most workstations are big-endian
		return 0;//Big-endian machine
	if (c[0]==0x1c) 
		//Intel x86 PC's, and DEC VAX are little-endian
		return 1;//Little-endian machine
	return 99;//Unknown integer type
}
/*Known values for this routine come from this (compressed) program:
main() {double d=-9.5; unsigned char *c=(unsigned char *)&d;
int i; for (i=0;i<sizeof(double);i++) printf("c[%d]==0x%02x && ",i,c[i]); }
*/
int getFloatFormat(void)
{
	float ftest=-9.5;//Float test value
	double dtest=-9.5;//Double test value
	
	//Find the 8-byte floating-point type
	unsigned char *c;
	    if (sizeof(double)==8) c=(unsigned char *)&dtest;
	else if (sizeof(float)==8) c=(unsigned char *)&ftest;
	else return 98;//Unknown floating-point sizes
	
	if (c[0]==0xc0 && c[1]==0x23 && c[2]==0x00 && c[3]==0x00) 
		return 0;//Big-endian IEEE machine (e.g., Mac, Sun, SGI)
	if (c[4]==0x00 && c[5]==0x00 && c[6]==0x23 && c[7]==0xc0) 
		return 1;//Little-endian IEEE machine (e.g., Intel)
	if (c[0]==0xC0 && c[1]==0x04 && c[2]==0x98 && c[3]==0x00)
		return 50;//Cray Y-MP (unsupported)
	return 99;//Unknown machine type
}

const PUP::machineInfo &PUP::machineInfo::current(void)
{
	static machineInfo *m=NULL;
	if (m==NULL) 
	{//Allocate, initialize, and return m
		m=new machineInfo();
		for (int i=0;i<4;i++)
			m->magic[i]=machInfo_magic[i];
		m->version=1;
		m->intBytes[0]=sizeof(char);
		m->intBytes[1]=sizeof(short);
		m->intBytes[2]=sizeof(int);
		m->intBytes[3]=sizeof(long);
#if CMK_HAS_INT16
		m->intBytes[4]=sizeof(CmiInt16);
#else
		m->intBytes[4]=0;
#endif
		m->intFormat=getIntFormat();
		m->floatBytes=sizeof(float);
		m->doubleBytes=sizeof(double);
		m->floatFormat=getFloatFormat();
		m->boolBytes=sizeof(bool);
		m->pointerBytes=sizeof(void*);
		//m->padding[0]=0;    // version 1 does not have padding field
	}
	return *m;
}

////////////////////////// Conversion Functions ///////////////////////////
typedef unsigned char myByte;

//Do nothing to the given bytes (the "null conversion")
static void cvt_null(int N,const myByte *in,myByte *out,size_t nElem) {}

//Swap the order of each N-byte chunk in the given array (in can equal out)
static void cvt_swap(int N,const myByte *in,myByte *out,size_t nElem)
{
	size_t i;
	for (i=0;i<nElem;i++)
	{
		const myByte *s=&in[N*i];
		myByte t,*d=&out[N*i];
		for (int j=N/2-1;j>=0;j--)
			{t=s[j];d[j]=s[N-j-1];d[N-j-1]=t;}
	}
}
/*******************************************************
Convert N-byte boolean to machine boolean.
*/
static void cvt_bool(int N,const myByte *in,myByte *out,size_t nElem)
{
	size_t i;
        for (i=nElem;i>1;i--)
	{
		const myByte *s=&in[N*(i-1)];
		bool ret=false;
		int j;
                for (j=0;j<N;j++)
			if (s[j]!=0) //Some bit is set
				ret=true;
		((bool *)(out))[i-1]=ret;
	}
}

/*******************************************************
Convert N-byte big or little endian integers to 
native char, short, or long signed or unsigned. 
Since this is so many functions, we define them 
with the preprocessor.

Values too large to be represented will be garbage 
(keeping only the low-order bits).
*/

/// These defines actually provide the conversion function bodies
#define def_cvtFunc(bigName,bigIdx,nameT,rT,uT) \
static void cvt##bigName##_to##nameT(int N,const myByte *in,myByte *out,size_t nElem) \
{ \
	size_t i;for (i=0;i<nElem;i++)\
	{\
		const myByte *s=&in[N*i];\
		rT ret=0;\
		int j;\
		for (j=0;j<N-1;j++) \
			ret|=((uT)s[bigIdx])<<(8*j);\
		ret|=((rT)s[bigIdx])<<(8*j);\
		((rT *)(out))[i]=ret;\
	}\
}
#define def_cvtBig_toT(T)  def_cvtFunc(Big,N-j-1,T    ,T         ,unsigned T)
#define def_cvtBig_touT(T) def_cvtFunc(Big,N-j-1,u##T ,unsigned T,unsigned T)
#define def_cvtLil_toT(T)  def_cvtFunc(Lil,j    ,T    ,T         ,unsigned T)
#define def_cvtLil_touT(T) def_cvtFunc(Lil,j    ,u##T ,unsigned T,unsigned T)

#define def_cvtTypes(cvtNT) \
cvtNT(char)  cvtNT(short)  cvtNT(int)  cvtNT(long)

def_cvtTypes(def_cvtLil_toT)  //the lil conversion functions
def_cvtTypes(def_cvtLil_touT) //the lil unsigned conversion functions
def_cvtTypes(def_cvtBig_toT)  //the big conversion functions
def_cvtTypes(def_cvtBig_touT) //the big unsigned conversion functions


/// These defines are used to initialize the conversion function array below
#define arr_cvtBig_toT(T)  cvtBig_to##T
#define arr_cvtBig_touT(T) cvtBig_tou##T
#define arr_cvtLil_toT(T)  cvtLil_to##T
#define arr_cvtLil_touT(T) cvtLil_tou##T

#define arr_cvtTypes(cvtNT) \
  {cvtNT(char), cvtNT(short), cvtNT(int), cvtNT(long)}

typedef void (*dataConverterFn)(int N,const myByte *in,myByte *out,size_t nElem);

const static dataConverterFn cvt_intTable
	[2]//Indexed by source endian-ness (big--0, little-- 1)
	[2]//Indexed by signed-ness (signed--0, unsigned-- 1)
	[4]//Index by dest type (0-- char, 1-- short, 2-- int, 3-- long)
={
{ arr_cvtTypes(arr_cvtBig_toT),  //the big conversion functions
  arr_cvtTypes(arr_cvtBig_touT) }, //the big unsigned conversion functions
{ arr_cvtTypes(arr_cvtLil_toT),  //the lil conversion functions
  arr_cvtTypes(arr_cvtLil_touT) } //the lil unsigned conversion functions
};

/*Set an appropriate conversion function for the given
number of source integer bytes to the given integer type index.
*/
void PUP::xlater::setConverterInt(const machineInfo &src,const machineInfo &cur,
	int isUnsigned,int intType,dataType dest)
{
	if (src.intFormat==cur.intFormat && src.intBytes[intType]==cur.intBytes[intType])
		convertFn[dest]=cvt_null;//Same format and size-- no conversion needed
	else 
		convertFn[dest]=cvt_intTable[src.intFormat][isUnsigned][intType];
	convertSize[dest]=src.intBytes[intType];
}

//Return the appropriate floating-point conversion routine 
static dataConverterFn converterFloat(
	const PUP::machineInfo &src,const PUP::machineInfo &cur,
	int srcSize,int curSize)
{
	if (src.floatFormat==cur.floatFormat && srcSize==curSize)
		return cvt_null;//No conversion needed
	else {
		if ((src.floatFormat==1 && cur.floatFormat==0)
		  ||(src.floatFormat==0 && cur.floatFormat==1))
		{//Endian-ness difference only-- just swap bytes
			if (srcSize==4 && curSize==4)
				return cvt_swap;
			else if (srcSize==8 && curSize==8)
				return cvt_swap;
		}
	}
	fprintf(stderr,__FILE__" Non-convertible float sizes %d and %d\n",srcSize,curSize);
	abort();
	return NULL;//<- for whining compilers
}

/*Constructor (builds conversionFn table)*/
PUP::xlater::xlater(const PUP::machineInfo &src,PUP::er &fromData)
	:wrap_er(fromData)
{
	const machineInfo &cur=PUP::machineInfo::current();
	if (src.intFormat>1) abort();//Unknown integer format
	//Set up table for converting integral types
	setConverterInt(src,cur,0,0,Tchar);
	setConverterInt(src,cur,0,1,Tshort);
	setConverterInt(src,cur,0,2,Tint);
	setConverterInt(src,cur,0,3,Tlong);
	setConverterInt(src,cur,1,0,Tuchar);
	setConverterInt(src,cur,1,1,Tushort);
	setConverterInt(src,cur,1,2,Tuint);
	setConverterInt(src,cur,1,3,Tulong);
	if (src.intFormat==cur.intFormat) //At worst, need to swap 8-byte integers:
		convertFn[Tlonglong]=convertFn[Tulonglong]=cvt_null;
	else
		convertFn[Tlonglong]=convertFn[Tulonglong]=cvt_swap;
#if CMK_HAS_INT16
	setConverterInt(src,cur,0,4,Tint128);
	setConverterInt(src,cur,1,4,Tuint128);
#endif
	convertFn[Tfloat]=converterFloat(src,cur,src.floatBytes,cur.floatBytes);
	convertFn[Tdouble]=converterFloat(src,cur,src.doubleBytes,cur.doubleBytes);
	convertFn[Tlongdouble]=cvt_null; //<- a lie, but no better alternative
	
	if (src.boolBytes!=cur.boolBytes)
		convertFn[Tbool]=cvt_bool;
	else
		convertFn[Tbool]=cvt_null;
	
	convertFn[Tbyte]=cvt_null;//Bytes are never converted at all
	setConverterInt(src,cur,0,2,Tsync);
	convertFn[Tpointer]=cvt_null; //<- a lie, but pointers should never be converted across machines
	
	//Finish out the size table (integer portion is done by setConverterInt)
#ifdef CMK_PUP_LONG_LONG
	convertSize[Tlonglong]=convertSize[Tlonglong]=sizeof(CMK_PUP_LONG_LONG);
#else
	convertSize[Tlonglong]=convertSize[Tlonglong]=8;
#endif
	convertSize[Tfloat]=src.floatBytes;
	convertSize[Tdouble]=src.doubleBytes;
#if CMK_LONG_DOUBLE_DEFINED
	convertSize[Tlongdouble]=sizeof(long double);
#else
	convertSize[Tlongdouble]=12; //<- again, a lie.  Need machineInfo.longdoubleSize!
#endif
	convertSize[Tbool]=src.boolBytes;
	convertSize[Tbyte]=1;//Byte always takes one byte of storage
	convertSize[Tpointer]=src.pointerBytes;
}

void PUP::xlater::pup_buffer_async(void *&ptr,size_t n,size_t itemSize,dataType t) {
  bytes(ptr, n, itemSize, t);
}

void PUP::xlater::pup_buffer_async(void *&ptr,size_t n, size_t itemSize, dataType t, std::function<void *(size_t)> allocate, std::function<void (void *)> deallocate) {
  bytes(ptr, n, itemSize, t);
}

//Generic bottleneck: unpack n items of size itemSize from p.
void PUP::xlater::bytes(void *ptr,size_t n,size_t itemSize,dataType t)
{
	if (convertSize[t]==itemSize)
	{//Do conversion in-place
		p.bytes(ptr,n,itemSize,t);
		convertFn[t](itemSize,(const myByte *)ptr,(myByte *)ptr,n);//Convert in-place
	}
	else 
	{//Read into temporary buffer, unpack, and then convert
		void *buf=(void *)malloc(convertSize[t]*n);
		p.bytes(buf,n,convertSize[t],t);
		convertFn[t](convertSize[t],(const myByte *)buf,(myByte *)ptr,n);
		free(buf);
	}
}

