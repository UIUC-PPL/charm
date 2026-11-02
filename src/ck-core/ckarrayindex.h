#ifndef CKARRAYINDEX_H
#define CKARRAYINDEX_H

#include "pup.h"
#include "ckhashtable.h"
#include "charm.h"
#include "objid.h"
#include <vector>
#include <math.h>

/// Max number of integers in an array index
#ifndef CK_ARRAYINDEX_MAXLEN
    #define CK_ARRAYINDEX_MAXLEN 3
#endif

#ifndef CK_ARRAYLISTENER_MAXLEN
# define CK_ARRAYLISTENER_MAXLEN 2
#endif

/** @warning: fwd declaration of child class to support crazy ptr cast
 */
class CkArrayIndex;


/**
 * Base class for array index objects used in charm.
 *
 * An array index is just a hash key; a run of integers
 * used to look up an object in a hash table.
 *
 * @note: Should define *all* data members that make up an index object.
 * @warning: Do not instantiate! Always create and use a child class
 * @warning: Do not add constructors / destructors. Class participates in unions
 *
 * @note: Should be completely invisible to most client code except those that directly
 * need to put index objects in unions. This happens only in a few entities in the charm
 * codebase and should not happen at all in user codes.
 */
struct CkArrayIndexBase
{
    public:
        ///Length of index in *integers*
        short unsigned int nInts;
        ///Number of dimensions in this index, not valid for user-defined indices
        short unsigned int dimension;
        /// The actual index data
        union {
            int index[CK_ARRAYINDEX_MAXLEN];
            short int indexShorts[2 * CK_ARRAYINDEX_MAXLEN];
        };

        /// Obtain usable object from base object. @warning: Dangerous pointer cast to child class!!!
        inline CkArrayIndex& asChild() const { return *(CkArrayIndex*)this; }

        /// Permit serialization via calling pup(), but also declare this type as PUPbytes below
        inline void pup(PUP::er &p)
        {
            p((char *)this, sizeof(CkArrayIndexBase));
        }

        bool operator==(CkArrayIndexBase &other) {
          if(nInts != other.nInts) return false;
          if(dimension != other.dimension) return false;
          for (int i=0;i<nInts;i++) {
            if(index[i] != other.index[i]) return false;
          }
          return true;
        }

};
PUPbytes(CkArrayIndexBase)



/**
 * Actual array index class intended for regular use
 *
 * @warning: Put all data members in base class or they may not be transmitted
 * in envelopes or callbacks (basically in any entity that stores indices in a
 * union). Only add behaviors to this class.
 */
class CkArrayIndex: public CkArrayIndexBase
{
    public:
        /// Default
        CkArrayIndex() { nInts=0; dimension=0; for (int i=0; i<CK_ARRAYINDEX_MAXLEN; i++) index[i] = 0; }

        /// Create array index of 'ndims' dimensions with given size for each dimension
        CkArrayIndex(int ndims, int dims[]) {
          init_nd(ndims, dims);
        }

        /// Create array index of 'ndims' dimensions with same size 'val' for each dimension
        CkArrayIndex(int ndims, int val) {
          std::vector<int> dims(ndims, val);
          init_nd(ndims, dims.data());
        }

	explicit CkArrayIndex(int idx) {init(1,1,idx);};

        /// Return a pointer to the actual index data
        int *data(void)             {return index; }
        /// Return a const pointer to the actual index data
        const int *data(void) const {return index; }

        /// Return the dimension size
        short getDimension(void) const            { return dimension; }
        /// Return a pointer to the short index data
        short *shortData(void)             { return indexShorts; }
        /// Return a const pointer to the short index data
        const short *shortData(void) const { return indexShorts; }

        /// Return the total number of elements (assuming a dense chare array)
        int getCombinedCount(void) const
        {
            if      (dimension == 1) return data()[0];
            else if (dimension == 2) return data()[0] * data()[1];
            else if (dimension == 3) return data()[0] * data()[1] * data()[2];
            else if (dimension == 4) return shortData()[0] * shortData()[1] * shortData()[2] *
                                            shortData()[3];
            else if (dimension == 5) return shortData()[0] * shortData()[1] * shortData()[2] *
                                            shortData()[3] * shortData()[4];
            else if (dimension == 6) return shortData()[0] * shortData()[1] * shortData()[2] *
                                            shortData()[3] * shortData()[4] * shortData()[5];
            else return 0;
        }

        /// Used for debug prints elsewhere
        void print() const {
            if (dimension < 4) CmiPrintf("%d: %d %d %d\n", dimension, index[0], index[1], index[2]);
            else CmiPrintf("%d: %d %d %d %d %d %d\n", dimension, indexShorts[0], indexShorts[1], indexShorts[2],
                                                      indexShorts[3], indexShorts[4], indexShorts[5]);
        }

        /// Equality comparison
        bool operator==(const CkArrayIndex& idx) const
        {
            if (nInts != idx.nInts) return false;
            for (int i=0; i<nInts; i++)
                if (index[i] != idx.index[i]) return false;
            return true;
        }

        /// These routines allow CkArrayIndex to be used in a CkHashtableT
        inline CkHashCode hash(void) const
        {
            int i;
            const int *d=data();
            CkHashCode ret=d[0];
            for (i=0;i<nInts;i++)
                ret +=circleShift(d[i],10+11*i)+circleShift(d[i],9+7*i);
            return ret;
        }
        ///
        static CkHashCode staticHash(const void *a,size_t) { return ((const CkArrayIndex *)a)->hash(); }
        ///
        inline int compare(const CkArrayIndex &idx) const { return (idx == *this); }
        ///
        static int staticCompare(const void *a,const void *b, size_t)
        { return (*(const CkArrayIndex *)a == *(const CkArrayIndex *)b); }

        /**
         * @todo: Chee Wai Lee had a FIXME note attached to this method because he
         * felt it was a temporary solution
         */
        CmiObjId getProjectionID() const
        {
            CmiObjId ret;
            int i;
            const int *data=this->data();
            if (OBJ_ID_SZ>=this->nInts)
            {
                for (i=0;i<this->nInts;i++)
                    ret.id[i]=data[i];
                for (i=this->nInts;i<OBJ_ID_SZ;i++)
                    ret.id[i]=0;
            }
            else
            {
                int j;
                //Must hash array index into LBObjid
                for (j=0;j<OBJ_ID_SZ;j++)
                    ret.id[j]=data[j];
                for (i=0;i<this->nInts;i++)
                    for (j=0;j<OBJ_ID_SZ;j++)
                        ret.id[j]+=circleShift(data[i],22+11*i*(j+1))+
                            circleShift(data[i],21-9*i*(j+1));
            }
            return ret;
        }

    protected:
        inline void init(const short num, const short dims, const int x, const int y=0, const int z=0)
        {
            nInts = num;
            dimension = dims;
            index[0] = x;
            index[1] = y;
            index[2] = z;
            for (int i=3; i < CK_ARRAYINDEX_MAXLEN; i++)
                index[i] = 0;
        }

        inline void init(const short num, const short dims,
                         const short u, const short v, const short w,
                         const short x, const short y=0, const short z=0)
        {
            nInts = num;
            dimension = dims;
            indexShorts[0] = u;
            indexShorts[1] = v;
            indexShorts[2] = w;
            indexShorts[3] = x;
            indexShorts[4] = y;
            indexShorts[5] = z;
            for (int i=6; i < 2 * CK_ARRAYINDEX_MAXLEN; i++)
                indexShorts[i] = 0;
        }

        inline void init_nd(int ndims, int dims[]) {
          switch (ndims) {
            case 1: init(1,1,dims[0]); break;
            case 2: init(2,2,dims[0],dims[1]); break;
            case 3: init(3,3,dims[0],dims[1],dims[2]); break;
            case 4: init(2,4,dims[0],dims[1],dims[2],dims[3]); break;
            case 5: init(3,5,dims[0],dims[1],dims[2],dims[3],dims[4]); break;
            case 6: init(3,6,dims[0],dims[1],dims[2],dims[3],dims[4],dims[5]); break;
            default: CkAbort("CKArrayIndex() unsupported number of dimensions\n");
          }
        }


        /// A very crude comparison operator to enable using in comparison-based containers
        friend bool operator< (const CkArrayIndex &lhs, const CkArrayIndex &rhs)
        {
#if CMK_ERROR_CHECKING
            if (lhs.nInts != rhs.nInts)
                CkAbort("cannot compare two indices of different cardinality");
#endif

            for (int i = 0; i < lhs.nInts; i++)
                if (lhs.data()[i] < rhs.data()[i])
                    return true;
                else if (rhs.data()[i] < lhs.data()[i])
                    return false;
            return false;
        }
};


/**
 * Support applications and other charm codes that still use the (now dead)
 * CkArrayIndexMax class to manipulate array indices. All the functionality is
 * now incorporated into the CkArrayIndex base class itself.
 *
 * It is recommended that newer code directly use the base class when there is
 * need to handle an array index.
 *
 * @todo: After at least one minor release announcing the deprecation,
 * CkArrayIndexMax should no longer be supported.
 */
typedef CkArrayIndex CkArrayIndexMax;

class CkArray;

struct CkArrayID {
private:
	CkGroupID _gid;
public:
	CkArrayID() : _gid() { }
	CkArrayID(CkGroupID g) :_gid(g) {}
	inline void setZero(void) {_gid.setZero();}
	inline bool isZero(void) const {return _gid.isZero();}
	operator CkGroupID() const {return _gid;}
	CkArray *ckLocalBranch(void) const
		{ return (CkArray *)CkLocalBranch(_gid); }
	static CkArray *CkLocalBranch(CkArrayID id)
		{ return (CkArray *)::CkLocalBranch(id); }
	CkArray *ckLocalBranchOther(int rank) const
		{ return (CkArray *)CkLocalBranchOther(_gid, rank); }
	static CkArray *CkLocalBranchOther(CkArrayID id, int rank)
		{ return (CkArray *)::CkLocalBranchOther(id, rank); }
	void pup(PUP::er &p) {p | _gid; }
	bool operator == (const CkArrayID& other) const {
		return (_gid == other._gid);
	}
    friend bool operator< (const CkArrayID &lhs, const CkArrayID &rhs) {
        return (lhs._gid < rhs._gid);
    }
};

typedef int CkIndex1D;
typedef struct {int x,y;} CkIndex2D;
PUPbytes(CkIndex2D)
typedef struct {int x,y,z;} CkIndex3D;
PUPbytes(CkIndex3D)
typedef struct {short int w,x,y,z;} CkIndex4D;
PUPbytes(CkIndex4D)
typedef struct {short int v,w,x,y,z;} CkIndex5D;
PUPbytes(CkIndex5D)
typedef struct {short int x1,y1,z1,x2,y2,z2;} CkIndex6D;
PUPbytes(CkIndex6D)
typedef struct {int data[CK_ARRAYINDEX_MAXLEN];} CkIndexMax;
PUPbytes(CkIndexMax)

/// Simple ArrayIndex classes: the key is just integer indices.
class CkArrayIndex1D : public CkArrayIndex {
public:
	CkArrayIndex1D() {}
	// CkIndex1D is an int, so that conversion is automatic
	CkArrayIndex1D(int i0) { init(1, 1, i0); }
};
class CkArrayIndex2D : public CkArrayIndex {
public:
	CkArrayIndex2D() {}
	CkArrayIndex2D(int i0,int i1) { init(2, 2, i0, i1); }
	CkArrayIndex2D(CkIndex2D idx) { init(2, 2, idx.x, idx.y); }
};
class CkArrayIndex3D : public CkArrayIndex {
public:
	CkArrayIndex3D() {}
	CkArrayIndex3D(int i0,int i1,int i2) { init(3, 3, i0, i1, i2); }
	CkArrayIndex3D(CkIndex3D idx) { init(3, 3, idx.x, idx.y, idx.z); }
};
class CkArrayIndex4D : public CkArrayIndex {
public:
	CkArrayIndex4D(){}
	CkArrayIndex4D(short int i0,short int i1,short int i2,short int i3) { init(2, 4, i0, i1, i2, i3); }
	CkArrayIndex4D(CkIndex4D idx) { init(2, 4, idx.w, idx.x, idx.y, idx.z); }
};
class CkArrayIndex5D : public CkArrayIndex {
public:
	CkArrayIndex5D() {}
	CkArrayIndex5D(short int i0,short int i1,short int i2,short int i3,short int i4) { init(3, 5, i0, i1, i2, i3, i4); }
	CkArrayIndex5D(CkIndex5D idx) { init(3, 5, idx.v, idx.w, idx.x, idx.y, idx.z); }
};
class CkArrayIndex6D : public CkArrayIndex {
public:
	CkArrayIndex6D(){}
	CkArrayIndex6D(short int i0,short int i1,short int i2,short int i3,short int i4,short int i5) { init(3, 6, i0, i1, i2, i3, i4, i5); }
	CkArrayIndex6D(CkIndex6D idx) { init(3, 6, idx.x1, idx.y1, idx.z1, idx.x2, idx.y2, idx.z2); }
};

/** A slightly more complex array index: the key is an object
 *  whose size is fixed at compile time.
 */
template <class object> //Key object
class CkArrayIndexT : public CkArrayIndex {
public:
	object obj;
	CkArrayIndexT(const object &srcObj) {obj=srcObj;
		nInts=sizeof(obj)/sizeof(int);
		dimension=0; }
};

namespace ck {
  class ArrayIndexCompressor {
  public:
    virtual CmiUInt8 compress(const CkArrayIndex &idx) = 0;
    virtual CkArrayIndex decompress(CmiUInt8 id) = 0;
  };

  class FixedArrayIndexCompressor : public ArrayIndexCompressor {
  public:
    /// Factory that checks whether a bit-packing compression is possible given
    /// @arg bounds
    /// Number of bits a packed index needs for these bounds (sum over dimensions
    /// of ceil(log2(bound))); 0 when no bounds are set.
    static unsigned int bitsNeeded(const CkArrayIndex &bounds) {
      if (bounds.nInts == 0)
        return 0;
      unsigned int sum = 0;
      bool shorts = bounds.dimension > 3;
      for (int i = 0; i < bounds.dimension; ++i) {
        int bound = shorts ? bounds.indexShorts[i] : bounds.index[i];
        sum += bitCount(bound);
      }
      return sum;
    }

    /// @arg payloadBits is the element payload width of the array's id layout
    /// (ck::ObjID::PAYLOAD_BITS, or WIDE_PAYLOAD_BITS for a wide-collection array).
    static FixedArrayIndexCompressor* make(const CkArrayIndex &bounds,
                                           unsigned int payloadBits = CMK_OBJID_PAYLOAD_BITS) {
      if (bounds.nInts == 0)
        return NULL;

      char dims = bounds.dimension;
      char bits[6];
      bool shorts = bounds.dimension > 3;

      for (int i = 0; i < bounds.dimension; ++i) {
        int bound = shorts ? bounds.indexShorts[i] : bounds.index[i];
        bits[i] = bitCount(bound);
      }

      if (bitsNeeded(bounds) > payloadBits)
        return NULL;

      return new FixedArrayIndexCompressor(dims, bits, payloadBits);
    }

    /// Pack the bits of @arg idx into an ObjID
    CmiUInt8 compress(const CkArrayIndex &idx) {
      CkAssert(idx.dimension == dims);

      CmiUInt8 eid = 0;

      bool shorts = idx.dimension > 3;

      for (unsigned int i = 0; i < idx.dimension; ++i) {
        unsigned int numBits = bitsPerDim[i];
        unsigned int thisDim = shorts ? idx.indexShorts[i] : idx.index[i];
        CmiAssertMsg(thisDim < (1UL << numBits),
                     "array index component %u is %u, outside the %u bits its bounds"
                     " allow (CkArrayOptions::setBounds)", i, thisDim, numBits);
        eid = (eid << numBits) | thisDim;
      }

      CmiAssertMsg(eid <= payloadMask,
                   "eid is too big! (eid: %" PRIx64 ", max: %" PRIx64 ")", eid,
                   payloadMask);

      return eid;
    }

    CkArrayIndex decompress(CmiUInt8 id) {
      int ix[6];
      for (int i = dims - 1; i >= 0; --i) {
        int bits = bitsPerDim[i];
        // CmiUInt8 arithmetic: with the 49-bit payload one dimension may need 31
        // bits, for which a signed int shift is undefined.
        ix[i] = (int)(id & (((CmiUInt8)1 << bits) - 1));
        id >>= bits;
      }

      switch(dims) {
      case 1:
        return CkArrayIndex1D(ix[0]);
      case 2:
        return CkArrayIndex2D(ix[0], ix[1]);
      case 3:
        return CkArrayIndex3D(ix[0], ix[1], ix[2]);
      case 4:
        return CkArrayIndex4D(ix[0], ix[1], ix[2], ix[3]);
      case 5:
        return CkArrayIndex5D(ix[0], ix[1], ix[2], ix[3], ix[4]);
      default:
        return CkArrayIndex6D(ix[0], ix[1], ix[2], ix[3], ix[4], ix[5]);
      }
    }

  private:
    FixedArrayIndexCompressor(char dims, char* bits, unsigned int payloadBits)
      : payloadMask(((CmiUInt8)1 << payloadBits) - 1), dims(dims)
      { for (int i = 0; i < dims; i++) bitsPerDim[i] = bits[i]; }
    CmiUInt8 payloadMask;
    char bitsPerDim[6];
    char dims;

    /// Compute the number of bits to represent integer indices in the range
    /// [0..bound). Essentially, ceil(log2(bound)).
    static unsigned int bitCount(int bound) {
      CkAssert(bound > 0);

      // Round up to the nearest power of 2 (effectively, ceiling)
      bound--;
      bound |= bound >> 1;
      bound |= bound >> 2;
      bound |= bound >> 4;
      bound |= bound >> 8;
      bound |= bound >> 16;
      bound++;

      // log2(bound)
      unsigned int result = 0;
      while (bound >>= 1) {
        ++result;
      }

      return result;
    }
  };
}

#endif // CKARRAYINDEX_H

