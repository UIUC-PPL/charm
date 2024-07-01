/**
  A PUP_fmt inserts a 1-byte data format code 
  before each pup'd data item.  This makes it possible
  to unpack items without using PUP.
*/
class PUP_fmt : public PUP::wrap_er {
    typedef PUP::er parent;
    
    typedef unsigned char byte;
    /**
      The introductory byte for each field is bit-coded as:
           | unused(2) | lengthLen(2) | typeCode(4) |
    */
    typedef enum {
        lengthLen_single=0, // field is a single item
        lengthLen_byte=1, // following 8 bits gives length of array
        lengthLen_int=2, // following 32 bits gives length of array
        lengthLen_long=3 // following 64 bits gives length of array (unimpl)
    } lengthLen_t;
    typedef enum { 
        typeCode_byte=0, // unknown data type: nItems bytes
        typeCode_int=2, // 32-bit integer array: nItems ints
        typeCode_long=3, // 64-bit integer array: nItems ints
        typeCode_float=5, // 32-bit floating-point array: nItems floats
        typeCode_double=6, // 64-bit floating-point array: nItems floats
        typeCode_comment=10, // comment/label: nItems byte characters
        typeCode_sync=11, // synchronization code
        typeCode_pointer=12 // 32 or 64 bit pointer, depending on the machine architecture
    } typeCode_t;
    void fieldHeader(typeCode_t typeCode,int nItems);
public:
    PUP_fmt(PUP::er &parent_) 
        :PUP::wrap_er(parent_,PUP::er::IS_COMMENTS) {}
    
    virtual void comment(const char *message);
    virtual void synchronize(unsigned int m);
    virtual void bytes(void *p,size_t n,size_t itemSize,PUP::dataType t);
    virtual void pup_buffer_async(void *&p,size_t n,size_t itemSize,PUP::dataType t);
    virtual void pup_buffer_async(void *&p,size_t n, size_t itemSize, PUP::dataType t, std::function<void *(size_t)> allocate, std::function<void (void *)> deallocate);
};

