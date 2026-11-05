#ifndef OBJID_H
#define OBJID_H

#include "charm.h"
#include "converse.h"
#include "pup.h"

// The 64-bit object id has two layouts, told apart by the 3 type tag bits at the top:
//
//   bit   63 62 61 | 60                      49 | 48                          0
//        +---------+----------------------------+------------------------------+
//        |  0 0 0  |   COLLECTION   (C = 12)    |      PAYLOAD   (64-3-C = 49) |  standard
//        +---------+----------------------------+------------------------------+
//
//   bit   63 62 61 | 60                      37 | 36            0
//        +---------+----------------------------+----------------+
//        |  0 0 1  |   COLLECTION   (24)        |  PAYLOAD (37)  |  wide
//        +---------+----------------------------+----------------+
//
//   PAYLOAD, array with bounds (packed kind, either layout): the index, packed.
//   PAYLOAD, array without bounds (hashed kind, standard layout only):
//        | HOME KEY (H bits, H fixed per run) | UNIQUE NUMBER (49 - H bits) |
//
//   - COLLECTION is the array's collection number: a dense number handed out on PE 0
//     when the array is created (CkCreateArray), standard numbers in [1, 2^12) and
//     wide numbers in [2^12, 2^24). The standard width C can be overridden with
//     -DCMK_OBJID_COLLECTION_BITS=N at build time; the wide width is fixed.
//   - PAYLOAD identifies the element within the array. Its meaning is decided per
//     array by CkLocMgr, not here: for an array with bounds that fit, it is the
//     packed index; otherwise it is a hash key of the index followed by a number
//     that is unique within the array (see cklocation.h, ck::objid::Layout).
//   - The wide layout is opt-in per array (CkArrayOptions::setWideCollection) for
//     programs that create many small arrays (AMPI: one per communicator). It
//     requires bounds, since 37 payload bits are too few for hashed ids at scale.
//
// No PE number is stored in the id. The home of an element is computed from the
// id, or from the index, by CkLocMgr (design: doc/objid64-design.md, section 12 for
// the wide layout).

#ifndef CMK_OBJID_COLLECTION_BITS
#define CMK_OBJID_COLLECTION_BITS 12
#endif

#define CMK_OBJID_TYPE_TAG_BITS   3
#define CMK_OBJID_PAYLOAD_BITS    (64 - CMK_OBJID_COLLECTION_BITS - CMK_OBJID_TYPE_TAG_BITS)

#define CMK_OBJID_WIDE_COLLECTION_BITS 24
#define CMK_OBJID_WIDE_PAYLOAD_BITS    (64 - CMK_OBJID_WIDE_COLLECTION_BITS - CMK_OBJID_TYPE_TAG_BITS)

// Sanity checks:
static_assert(CMK_OBJID_COLLECTION_BITS > 0,
              "CMK_OBJID_COLLECTION_BITS must be greater than 0!");
static_assert(CMK_OBJID_COLLECTION_BITS <= 40,
              "CMK_OBJID_COLLECTION_BITS must leave at least 21 payload bits!");
static_assert((CMK_OBJID_COLLECTION_BITS + CMK_OBJID_PAYLOAD_BITS + CMK_OBJID_TYPE_TAG_BITS) == 64,
              "The total number of collection + payload + type tag bits must be 64!");

namespace ck {

/**
 * The basic element identifier
 */
class ObjID {
    public:
        enum bits {
          PAYLOAD_BITS         = CMK_OBJID_PAYLOAD_BITS,
          COLLECTION_BITS      = CMK_OBJID_COLLECTION_BITS,
          TYPE_TAG_BITS        = CMK_OBJID_TYPE_TAG_BITS,
          WIDE_PAYLOAD_BITS    = CMK_OBJID_WIDE_PAYLOAD_BITS,
          WIDE_COLLECTION_BITS = CMK_OBJID_WIDE_COLLECTION_BITS
        };
        enum tags : CmiUInt8 {
          TAG_STANDARD = 0,
          TAG_WIDE     = 1
        };
        enum masks : CmiUInt8 {
          PAYLOAD_MASK         = ((1ULL << PAYLOAD_BITS) - 1),
          COLLECTION_MASK      = (((1ULL << COLLECTION_BITS) - 1) << PAYLOAD_BITS),
          TYPE_TAG_MASK        = (((1ULL << TYPE_TAG_BITS) - 1) << (64 - TYPE_TAG_BITS)),
          WIDE_PAYLOAD_MASK    = ((1ULL << WIDE_PAYLOAD_BITS) - 1),
          WIDE_COLLECTION_MASK = (((1ULL << WIDE_COLLECTION_BITS) - 1) << WIDE_PAYLOAD_BITS),
          WIDE_TAG             = (TAG_WIDE << (64 - TYPE_TAG_BITS))
        };
        /// Collection numbers: standard arrays take [1, STANDARD_LIMIT), wide arrays
        /// [STANDARD_LIMIT, WIDE_LIMIT), so a program with many wide arrays never
        /// pushes a standard array out of its range.
        enum limits : CmiUInt8 {
          STANDARD_LIMIT = (1ULL << COLLECTION_BITS),
          WIDE_LIMIT     = (1ULL << WIDE_COLLECTION_BITS),
          // A build with C >= 24 (an AMPI build with -DCMK_OBJID_COLLECTION_BITS=29)
          // has no room above the standard range; wide numbers then start at 1 and
          // are told apart from standard ones by the tag alone.
          WIDE_FIRST     = (STANDARD_LIMIT < WIDE_LIMIT) ? STANDARD_LIMIT : 1
        };

        ObjID(): id(0) {}
        ///
        ObjID(const CmiUInt8 id_) : id(id_) { }
        /// An element id from its array's collection number and kind and the
        /// element payload. Checked in every build: a payload out of range (an index
        /// outside the array's bounds) would otherwise land in another array's field.
        ObjID(const CmiUInt8 collection, const CmiUInt8 eid, const bool wide)
            : id(wide ? (WIDE_TAG | (collection << WIDE_PAYLOAD_BITS) | eid)
                      : ((collection << PAYLOAD_BITS) | eid))
        {
          if (wide ? (collection >= WIDE_LIMIT || eid > WIDE_PAYLOAD_MASK)
                   : (collection >= STANDARD_LIMIT || eid > PAYLOAD_MASK))
            CmiAbort("Element id out of range: collection %llu, payload 0x%llx, %s layout"
                     " (an array index outside the array's bounds?)\n",
                     (unsigned long long)collection, (unsigned long long)eid,
                     wide ? "wide" : "standard");
        }

        /// Wide layout (tag 001)?
        inline bool isWide() const { return (id & TYPE_TAG_MASK) == WIDE_TAG; }
        /// The collection number of the chare array this element belongs to
        inline CmiUInt8 getCollectionNumber() const {
            return isWide() ? ((id & WIDE_COLLECTION_MASK) >> WIDE_PAYLOAD_BITS)
                            : ((id & COLLECTION_MASK) >> PAYLOAD_BITS);
        }
        /// The element payload: everything location management keys on. The mask
        /// comes from a table indexed by the tag, so delivery does not branch here.
        inline CmiUInt8 getElementID() const {
            static constexpr CmiUInt8 masks[1 << TYPE_TAG_BITS] = {
                PAYLOAD_MASK, WIDE_PAYLOAD_MASK, PAYLOAD_MASK, PAYLOAD_MASK,
                PAYLOAD_MASK, PAYLOAD_MASK,      PAYLOAD_MASK, PAYLOAD_MASK};
            return id & masks[id >> (64 - TYPE_TAG_BITS)];
        }
        /// The whole id, tag included (two ids are the same element iff equal)
        inline CmiUInt8 getID() const { return id; }

    private:

        /// The actual id data
        CmiUInt8 id;
};

inline bool operator==(ObjID lhs, ObjID rhs) {
  return lhs.getID() == rhs.getID();
}
inline bool operator!=(ObjID lhs, ObjID rhs) {
  return !(lhs == rhs);
}

} // end namespace ck

PUPbytes(ck::ObjID)
#endif // OBJID_H
