/**
 * The process-level location table: one per process, shared by every PE and
 * every chare array, keyed by the full 64-bit element id. Reads are lock-free
 * and wait-free; every write is one compare-and-swap. Design and protocols:
 * doc/internals/process-location-cache.md.
 */
#ifndef CK_LOCNODE_H
#define CK_LOCNODE_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>

#include "ckarrayindex.h"

namespace ck {
namespace objid {

/// Set from +objid_table_log2 before the table is first used (init.C).
extern int _objidTableLog2;
/// +objid_diag: count table events and print them per process at exit.
extern bool _objidDiag;

class LocTable
{
public:
  /// A slot's value word: epoch:31 | arriving:1 | pe+1:32. Zero means the key is
  /// known but has no location (never had one, or the element died: section 3.5).
  struct Value
  {
    uint64_t raw = 0;
    bool hasLocation() const { return (raw & PE_MASK) != 0; }
    int pe() const { return (int)(raw & PE_MASK) - 1; }  // -1 when no location
    int epoch() const { return hasLocation() ? (int)(raw >> EPOCH_SHIFT) : -1; }
    bool arriving() const { return (raw & ARRIVING_BIT) != 0; }
    /// Written from a received update (W4), not by the PE it names (W1, W2, W3).
    bool byMessage() const { return (raw & BY_MESSAGE_BIT) != 0; }
    static Value make(int pe, int epoch, bool arriving, bool byMessage = false)
    {
      Value v;
      v.raw = ((uint64_t)(uint32_t)epoch << EPOCH_SHIFT) | (arriving ? ARRIVING_BIT : 0) |
              (byMessage ? BY_MESSAGE_BIT : 0) | (uint64_t)(uint32_t)(pe + 1);
      return v;
    }
    static constexpr uint64_t PE_MASK = 0xffffffffull;
    static constexpr uint64_t ARRIVING_BIT = 1ull << 32;
    static constexpr uint64_t BY_MESSAGE_BIT = 1ull << 33;
    static constexpr int EPOCH_SHIFT = 34;  // 30 bits of epoch
  };

  /// Counters for +objid_diag; relaxed atomics, touched only when the flag is on.
  struct Counters
  {
    std::atomic<uint64_t> lookupMiss{0};       // a read that found no location
    std::atomic<uint64_t> requestsSent{0};     // one per process per unresolved id
    std::atomic<uint64_t> requestsSkipped{0};  // another PE of this process had asked
    std::atomic<uint64_t> fills{0};            // a write that took waiter bits
    std::atomic<uint64_t> wakesSent{0};        // wake messages to other ranks
    std::atomic<uint64_t> repairsSkipped{0};   // repair to a sender in this process
    std::atomic<uint64_t> missNoId{0};         // whichPe(idx): no id for the index
    std::atomic<uint64_t> missNoLocation{0};   // whichPe(idx): id known, no location
    std::atomic<uint64_t> growths{0};
  };

  explicit LocTable(int log2Slots);
  ~LocTable();
  LocTable(const LocTable&) = delete;
  LocTable& operator=(const LocTable&) = delete;

  // Reads: hash, probe, acquire-load the key, acquire-load the value. Wait-free.
  Value lookup(uint64_t key) const;
  int pe(uint64_t key) const { return lookup(key).pe(); }
  int epoch(uint64_t key) const { return lookup(key).epoch(); }

  // Writes. Each returns the waiter bits it took, which the caller turns into wake
  // messages (section 3.3); 0 when nothing was written or the value written has no
  // location to deliver to.
  /// Unconditional store (W1 creation, W3 landing): the writer holds the element.
  uint64_t store(uint64_t key, int pe, int epoch, bool arriving = false);
  /// Store if epoch is greater than the stored epoch (W4): newer epoch wins under
  /// any interleaving. *written tells whether it did.
  /// With received = true the update came by message (W4) and the value is marked
  /// byMessage. It is not applied over an entry that a PE of this process wrote about
  /// itself (W1, W2, W3; not marked): those writes are synchronous, so the message is
  /// at best redundant, at worst from an earlier life. A received update clears the
  /// request flag whether or not it writes.
  uint64_t update(uint64_t key, int pe, int epoch, bool arriving, bool* written = nullptr,
                  bool received = false);
  /// W2: {fromPe, e} -> {toPe, e+1}; returns the new epoch. The entry must name fromPe.
  int recordEmigration(uint64_t key, int fromPe, int toPe);
  /// Forget the location (death, or the record leaving this PE); the key, the
  /// waiter bits and the request flag stay: a rank that registered while the
  /// location was unknown is woken by the next store, and a request that is out is
  /// held at the home until the element exists again. With ifPe >= 0 the entry is cleared only while it
  /// names ifPe (and, with ifEpoch >= 0, that epoch): a death notice that arrives
  /// after a re-creation leaves the live entry alone. True if cleared.
  /// With keepOwn = true an entry a PE of this process wrote about itself (naming it,
  /// not byMessage) is left alone: the home's clears on a death notice, since that PE
  /// cleared or rewrote its own entry before anything was sent.
  bool clear(uint64_t key, int ifPe = -1, int ifEpoch = -1, bool keepOwn = false);

  /// Set rank's bit in the key's waiter word: the next fill wakes this rank.
  /// Inserts the key if absent. A bit left by a rank that then found the location
  /// itself costs that rank one wake with nothing to do.
  void registerWaiter(uint64_t key, int rank);
  /// Set the key's request flag; true if it was clear, which makes the caller the
  /// one that sends the request to the home. A fill (store, update) clears the flag,
  /// and so does every received update, written or not.
  /// The waiter word cannot carry this: a bit may be left by a rank that found the
  /// location, and a fill between a miss and the registration takes no bits
  /// (section 3.3).
  bool claimRequest(uint64_t key);
  uint64_t takeWaiters(uint64_t key);

  Counters& counters() { return counts; }
  size_t slots() const;  // total capacity over all segments
  int segments() const;
  size_t used() const;

private:
  struct Slot
  {
    std::atomic<uint64_t> key;
    std::atomic<uint64_t> value;
    std::atomic<uint64_t> waiters;
    std::atomic<uint64_t> request;  // 1: a request for this key is out (section 3.3)
  };
  // Growth (cklocnode.C, findOrInsert and grow): an inserter counts itself in
  // `active` of the head segment before it looks for its key, and leaves the count
  // when done; the grower sets `sealed`, waits for `active` to reach 0, then
  // publishes the bigger segment. So a key is never inserted into two segments.
  struct Segment
  {
    Segment* older;
    int log2;
    uint64_t mask;
    std::atomic<uint64_t> used;
    std::atomic<int> active;
    std::atomic<bool> sealed;
    Slot* slots;
  };
  std::atomic<Segment*> head;
  uint64_t fill(Slot* s);  // after a write with a location: take waiters, clear the flag
  mutable Counters counts;

  static Segment* makeSegment(int log2, Segment* older);
  static Slot* probe(Segment* seg, uint64_t key);  // nullptr if not in this segment
  Slot* find(uint64_t key) const;                  // newest segment first
  Slot* findOrInsert(uint64_t key);
  void grow(Segment* from);
  void awaitGrowth(const Segment* from) const;  // spin until head != from
};

/// The process's table, created on first use after the command line is parsed.
LocTable& locTable();

/**
 * The process-level index table of the hashed kind: (collection number, index) ->
 * element id, shared by every PE (section 2.2). A slot is a claim word (the key's
 * hash, non-zero), the key words, and the id word. A writer claims the slot by CAS
 * on the claim word, stores the key words, then publishes the id with release
 * ordering; a reader compares the key words only after it has loaded a non-zero
 * id (callers store the full id, which is never 0). Key words never change once published; the id word changes when an index
 * is rebound to a fresh id (the old one is retired, see RetiredIds).
 */
class IndexTable
{
public:
  explicit IndexTable(int log2Slots);
  ~IndexTable();
  IndexTable(const IndexTable&) = delete;
  IndexTable& operator=(const IndexTable&) = delete;

  /// Lock-free read; false if the index has no binding in this process.
  bool lookup(uint64_t collection, const CkArrayIndex& idx, uint64_t& id) const;
  /// Bind idx -> id. Returns the id it was bound to before: 0 if none, id itself if
  /// the binding already existed, or an older id now retired.
  uint64_t bind(uint64_t collection, const CkArrayIndex& idx, uint64_t id);

  size_t slots() const;
  size_t used() const;
  int segments() const;

  // Key words: word 0 = collection | nInts << 32 | dimension << 48; then the index
  // ints two per word. Unused ints are zero (CkArrayIndex::init zero-fills).
  static constexpr int KEY_WORDS = 1 + (CK_ARRAYINDEX_MAXLEN + 1) / 2;
  static constexpr int SLOT_WORDS = 1 + KEY_WORDS + 1;

private:
  struct Segment  // active and sealed: as in LocTable::Segment
  {
    Segment* older;
    int log2;
    uint64_t mask;
    std::atomic<uint64_t> used;
    std::atomic<int> active;
    std::atomic<bool> sealed;
    std::atomic<uint64_t>* words;  // SLOT_WORDS per slot
  };
  std::atomic<Segment*> head;

  static void makeKey(uint64_t collection, const CkArrayIndex& idx, uint64_t key[KEY_WORDS]);
  static uint64_t hashKey(const uint64_t key[KEY_WORDS]);
  static Segment* makeSegment(int log2, Segment* older);
  // The id word of the slot holding key in seg, or nullptr. A claimed slot whose id
  // is not yet published counts as a match for a writer (which waits for it) and
  // as absent for a reader.
  static std::atomic<uint64_t>* probe(Segment* seg, const uint64_t key[KEY_WORDS],
                                      uint64_t h, bool writer);
  std::atomic<uint64_t>* find(const uint64_t key[KEY_WORDS], uint64_t h, bool writer) const;
  void grow(Segment* from);
  void awaitGrowth(const Segment* from) const;
};

IndexTable& indexTable();

/**
 * Ids of the hashed kind that no longer name a live element -> their index: an id
 * a process rebound to a newer one, and, at the home, every dead element's id.
 * Read on the miss path only (CkLocMgr::recoverIndex), never on a send, so a
 * mutex is acceptable here. Never pruned.
 */
class RetiredIds
{
public:
  void note(uint64_t fullId, const CkArrayIndex& idx);
  bool find(uint64_t fullId, CkArrayIndex& idx) const;
  size_t size() const;

private:
  mutable std::mutex lock;
  std::unordered_map<uint64_t, CkArrayIndex> map;
};

RetiredIds& retiredIds();

}  // namespace objid
}  // namespace ck

#endif
