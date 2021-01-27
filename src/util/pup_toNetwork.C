/*
Implementation of pup_toNetwork.h

Orion Sky Lawlor, olawlor@acm.org, 2004/3/18
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "converse.h"
#include "pup.h"
#include "pup_toNetwork.h"

/****************** toNetwork ********************
*/

void PUP_toNetwork_sizer::bytes(void *p,size_t n,size_t itemSize,PUP::dataType t)
{
	switch (t) {
	case PUP::Tchar: //Strings and bytes get copied as-is
	case PUP::Tuchar:
	case PUP::Tbyte:
		nBytes+=n;
		break;
	case PUP::Tulong:  // longs and doubles are 8 bytes
	case PUP::Tlong:
	case PUP::Tulonglong:
	case PUP::Tlonglong: 
	case PUP::Tdouble: 
	case PUP::Tlongdouble:
		nBytes+=n*8;
		break;
    case PUP::Tpointer:  // pointers are 8 bytes on 64-bit machines and 4 bytes on 32-bit machines
        nBytes+=n*sizeof(void*);
        break;
    default: //Everything else goes as a network int
		nBytes+=n*4;
		break;
	}
}

void PUP_toNetwork_sizer::pup_buffer_async(void *&p, size_t n, size_t itemSize, PUP::dataType t) {
  bytes(p, n, itemSize, t);
}

void PUP_toNetwork_sizer::pup_buffer_async(void *&p, size_t n, size_t itemSize, PUP::dataType t, std::function<void *(size_t)> allocate, std::function<void (void *)> deallocate) {
  bytes(p, n, itemSize, t);
}

#define casesPUP_toNetwork_types \
	casePUP_toNetwork_type(Tfloat,float,float); \
	casePUP_toNetwork_type(Tdouble,double,double); \
	case PUP::Tushort: /* Fallthrough (no special treatment for unsigned) */ \
	casePUP_toNetwork_type(Tshort,short,int); \
	case PUP::Tuint:  \
	casePUP_toNetwork_type(Tint,int,int); \
	case PUP::Tulong: \
	casePUP_toNetwork_type(Tlong,long,CMK_NETWORK_INT8); \
	case PUP::Tulonglong:  \
	casePUP_toNetwork_type(Tlonglong,CMK_NETWORK_INT8,CMK_NETWORK_INT8); \
	casePUP_toNetwork_type(Tbool,bool,int); \
	case PUP::Tsync: break; /* ignore syncs */ \
	casePUP_toNetwork_type(Tpointer,void*,CMK_POINTER_SIZED_INT);

void PUP_toNetwork_pack::bytes(void *p,size_t n,size_t itemSize,PUP::dataType t)
{
	size_t i;
	switch (t) {
	case PUP::Tchar: //Strings and bytes get copied as-is
	case PUP::Tuchar:
	case PUP::Tbyte:
		memcpy(buf,p,n);
		buf+=n;
		break;
#define casePUP_toNetwork_type(enumName,typeName,writeAs) \
	case PUP::enumName: \
	        for (i=0;i<n;i++) \
		  w((writeAs)( ((typeName *)p)[i] ));\
	        break
	casesPUP_toNetwork_types
#if CMK_LONG_DOUBLE_DEFINED
        casePUP_toNetwork_type(Tlongdouble,long double,double); 
#endif
#undef casePUP_toNetwork_type
	
       	default: 
       		CmiAbort("Unrecognized type passed to PUP_toNetwork_pack!\n");
       	}
}

void PUP_toNetwork_pack::pup_buffer_async(void *&p, size_t n, size_t itemSize, PUP::dataType t) {
  bytes(p, n, itemSize, t);
}

void PUP_toNetwork_pack::pup_buffer_async(void *&p, size_t n, size_t itemSize, PUP::dataType t, std::function<void *(size_t)> allocate, std::function<void (void *)> deallocate) {
  bytes(p, n, itemSize, t);
}

void PUP_toNetwork_unpack::bytes(void *p,size_t n,size_t itemSize,PUP::dataType t)
{
	size_t i;
	switch (t) {
	case PUP::Tchar: //Strings and bytes get copied as-is
	case PUP::Tuchar:
	case PUP::Tbyte:
		memcpy(p,buf,n);
		buf+=n;
		break;
#define casePUP_toNetwork_type(enumName,typeName,readAs) \
	case PUP::enumName: \
	        for (i=0;i<n;i++) \
		  ((typeName *)p)[i]=(typeName)read_##readAs();\
	        break
	casesPUP_toNetwork_types
#if CMK_LONG_DOUBLE_DEFINED
        casePUP_toNetwork_type(Tlongdouble,long double,double); 
#endif
#undef casePUP_toNetwork_type
	
       	default: 
       		CmiAbort("Unrecognized type passed to PUP_toNetwork_unpack!\n");
       	}
}

void PUP_toNetwork_unpack::pup_buffer_async(void *&p, size_t n, size_t itemSize, PUP::dataType t) {
  bytes(p, n, itemSize, t);
}

void PUP_toNetwork_unpack::pup_buffer_async(void *&p, size_t n, size_t itemSize, PUP::dataType t, std::function<void *(size_t)> allocate, std::function<void (void *)> deallocate) {
  bytes(p, n, itemSize, t);
}



