/*
Pack/unpack to network-byte-order integer/float/double.
On-the-wire sizes correspond to Java sizes for 
int, long, float, and double.

Orion Sky Lawlor, olawlor@acm.org, 11/1/2001
 */
#ifndef __OSL_PUP_TONETWORK_H
#define __OSL_PUP_TONETWORK_H

#include "pup.h"


typedef CMK_TYPEDEF_INT4 CMK_NETWORK_INT4;
#ifdef CMK_PUP_LONG_LONG
typedef CMK_PUP_LONG_LONG CMK_NETWORK_INT8;
#else
typedef CMK_TYPEDEF_INT8 CMK_NETWORK_INT8; /* long is 8 bytes */
#endif

/// Integer type the same size as "float"
typedef CMK_NETWORK_INT4 CMK_FLOAT_SIZED_INT;
/// Integer type the same size as "double"
typedef CMK_NETWORK_INT8 CMK_DOUBLE_SIZED_INT;

#if CMK_SIZET_64BIT
typedef CMK_NETWORK_INT8 CMK_POINTER_SIZED_INT;
#else
typedef CMK_NETWORK_INT4 CMK_POINTER_SIZED_INT;
#endif

class PUP_toNetwork_sizer : public PUP::er {
	size_t nBytes;
	virtual void bytes(void *p,size_t n,size_t itemSize,PUP::dataType t);
	virtual void pup_buffer_async(void *&p, size_t n, size_t itemSize, PUP::dataType t);
	virtual void pup_buffer_async(void *&p, size_t n, size_t itemSize, PUP::dataType t, std::function<void *(size_t)> allocate, std::function<void (void *)> deallocate);

 public:
	PUP_toNetwork_sizer(void) :PUP::er(IS_SIZING), nBytes(0) {}
	size_t size(void) const {return nBytes;}
};

class PUP_toNetwork_pack : public PUP::er {
	unsigned char *buf,*start;
	inline void w(CMK_NETWORK_INT4 i) {
		//Write big-endian integer to output stream
		*buf++=(unsigned char)(i>>24); //High end first
		*buf++=(unsigned char)(i>>16);
		*buf++=(unsigned char)(i>>8);
		*buf++=(unsigned char)(i>>0);  //Low end last
	}
	inline void w(CMK_NETWORK_INT8 i) {
		w(CMK_NETWORK_INT4(i>>32));
		w(CMK_NETWORK_INT4(i));
	}
	// NOTE: Need to use a union to force gcc compiler to consider pointer aliasing
	//Write floating-point number to stream.
	inline void w(float f) {
	  union { float f; CMK_FLOAT_SIZED_INT i; } uaw;
	  uaw.f=f;
	  w(uaw.i);
	  //w(*(CMK_FLOAT_SIZED_INT *)&f);
	}
	//Write floating-point number to stream.
	inline void w(double f)  {
	  union { double f; CMK_DOUBLE_SIZED_INT i; } uaw;
	  uaw.f=f;
	  w(uaw.i);
	  //w(*(CMK_DOUBLE_SIZED_INT *)&f);
	}

	virtual void bytes(void *p,size_t n,size_t itemSize,PUP::dataType t);
	virtual void pup_buffer_async(void *&p, size_t n, size_t itemSize, PUP::dataType t);
	virtual void pup_buffer_async(void *&p, size_t n, size_t itemSize, PUP::dataType t, std::function<void *(size_t)> allocate, std::function<void (void *)> deallocate);

public:
	PUP_toNetwork_pack(void *dest) :PUP::er(IS_PACKING) {
		start=buf=(unsigned char *)dest;
		CmiAssert(sizeof(void *) == sizeof(CMK_POINTER_SIZED_INT));
	}
	inline size_t size(void) const {return buf-start;}
};

class PUP_toNetwork_unpack : public PUP::er {
	const unsigned char *buf,*start;
	inline CMK_NETWORK_INT4 read_int(void) {
		//Read big-endian integer to output stream
		CMK_NETWORK_INT4 ret=(buf[0]<<24)|(buf[1]<<16)|(buf[2]<<8)|(buf[3]);
		buf+=4;
		return ret;
	}
	inline CMK_NETWORK_INT8 read_CMK_NETWORK_INT8(void) {
		CMK_NETWORK_INT8 hi=0xffFFffFFu&(CMK_NETWORK_INT8)read_int();
		CMK_NETWORK_INT8 lo=0xffFFffFFu&(CMK_NETWORK_INT8)read_int();
		return (hi<<32)|(lo);
	}
	inline void read_integer(CMK_NETWORK_INT4 &i) { i=read_int(); }
    inline void read_integer(CMK_NETWORK_INT8 &i) { i=read_CMK_NETWORK_INT8(); }
    // NOTE: Need to use a union to force gcc compiler to consider pointer aliasing
	inline float read_float(void) {
	  union { float f; CMK_FLOAT_SIZED_INT i; } uaw;
	  read_integer(uaw.i);
	  return uaw.f;
	  //CMK_NETWORK_INT4 i=read_int();
	  //return *(float *)&i;
	}
	inline double read_double(void) {
	  union { double f; CMK_DOUBLE_SIZED_INT i; } uaw;
	  read_integer(uaw.i);
	  return uaw.f;
	  //CMK_NETWORK_INT8 i=read_CMK_NETWORK_INT8();
	  //return *(double *)&i;
	}
	inline void * read_CMK_POINTER_SIZED_INT(void) {
	    CMK_POINTER_SIZED_INT i;
	    read_integer(i);
	    return *(void **)&i;
	}

	virtual void bytes(void *p,size_t n,size_t itemSize,PUP::dataType t);
	virtual void pup_buffer_async(void *&p, size_t n, size_t itemSize, PUP::dataType t);
	virtual void pup_buffer_async(void *&p, size_t n, size_t itemSize, PUP::dataType t, std::function<void *(size_t)> allocate, std::function<void (void *)> deallocate);

 public:
	PUP_toNetwork_unpack(const void *src) :PUP::er(IS_UNPACKING) {
		start=buf=(const unsigned char *)src;
	}
	inline size_t size(void) const {return buf-start;}
};

#endif

