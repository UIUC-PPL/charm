/**
 * The process-level location table. doc/internals/process-location-cache.md,
 * section 2.1 (structure) and 3 (who writes when).
 */
#include "cklocnode.h"

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <new>
#include <thread>

#include "converse.h"

namespace ck {
namespace objid {

int _objidTableLog2 = 16;
bool _objidDiag = false;

namespace {

// Fibonacci hashing: the top log2 bits of key * golden ratio.
inline uint64_t slotIndex(uint64_t key, int log2)
{
  return (key * 0x9E3779B97F4A7C15ull) >> (64 - log2);
}

}  // namespace

LocTable::Segment* LocTable::makeSegment(int log2, Segment* older)
{
  Segment* seg = new Segment;
  seg->older = older;
  seg->log2 = log2;
  seg->mask = (1ull << log2) - 1;
  seg->used.store(0, std::memory_order_relaxed);
  seg->active.store(0, std::memory_order_relaxed);
  seg->sealed.store(false, std::memory_order_relaxed);
  // Value-initialization zeroes every atomic: key 0 is the empty marker.
  seg->slots = new Slot[1ull << log2]();
  return seg;
}

LocTable::LocTable(int log2Slots)
{
  if (log2Slots < 4) log2Slots = 4;
  if (log2Slots > 40) log2Slots = 40;
  head.store(makeSegment(log2Slots, nullptr), std::memory_order_release);
}

LocTable::~LocTable()
{
  Segment* seg = head.load(std::memory_order_acquire);
  while (seg)
  {
    Segment* older = seg->older;
    delete[] seg->slots;
    delete seg;
    seg = older;
  }
}

// Linear probing. Keys are never removed, so an empty slot ends the probe: the
// key is not in this segment.
LocTable::Slot* LocTable::probe(Segment* seg, uint64_t key)
{
  uint64_t i = slotIndex(key, seg->log2);
  for (uint64_t n = 0; n <= seg->mask; ++n, i = (i + 1) & seg->mask)
  {
    Slot& s = seg->slots[i];
    const uint64_t k = s.key.load(std::memory_order_acquire);
    if (k == key) return &s;
    if (k == 0) return nullptr;
  }
  return nullptr;
}

LocTable::Slot* LocTable::find(uint64_t key) const
{
  for (Segment* seg = head.load(std::memory_order_acquire); seg; seg = seg->older)
    if (Slot* s = probe(seg, key)) return s;
  return nullptr;
}

void LocTable::awaitGrowth(const Segment* from) const
{
  while (head.load(std::memory_order_acquire) == from) std::this_thread::yield();
}

// The one writer whose seal succeeds publishes the bigger segment once no insert
// into `from` is in progress; the others wait for the publication. Inserters
// that arrive after the seal wait too (findOrInsert).
void LocTable::grow(Segment* from)
{
  if (from->sealed.exchange(true, std::memory_order_seq_cst))
  {
    awaitGrowth(from);
    return;
  }
  Segment* bigger = makeSegment(from->log2 + 2, from);
  while (from->active.load(std::memory_order_seq_cst) != 0) std::this_thread::yield();
  head.store(bigger, std::memory_order_release);
  counts.growths.fetch_add(1, std::memory_order_relaxed);
}

// A key present in the table is found lock-free: slots never move. Only a miss
// enters the insert protocol: the search is repeated and the insert done while this
// writer is counted active in the head segment, and a segment is sealed before it
// is superseded (grow). Either this writer sees the seal and starts over in the
// newer segment, or the grower sees this writer and publishes only after it is
// done. So no insert into an older segment is in progress or still to come while a
// writer searches the newer one, and a key gets one slot in one segment
// (doc/internals/process-location-cache.md, section 2.1).
LocTable::Slot* LocTable::findOrInsert(uint64_t key)
{
  if (Slot* s = find(key)) return s;
  for (;;)
  {
    Segment* seg = head.load(std::memory_order_acquire);
    seg->active.fetch_add(1, std::memory_order_seq_cst);
    if (seg->sealed.load(std::memory_order_seq_cst))
    {
      seg->active.fetch_sub(1, std::memory_order_release);
      awaitGrowth(seg);
      continue;
    }
    Slot* found = find(key);
    if (found == nullptr)
    {
      // Keep the load under 1/2: a miss with linear probing then costs about 2.5
      // slots on average (32 at 7/8), and every lookup of a key in an older segment
      // misses in the head segment first.
      if ((seg->used.load(std::memory_order_relaxed) + 1) * 2 > seg->mask + 1)
      {
        seg->active.fetch_sub(1, std::memory_order_release);
        grow(seg);
        continue;
      }
      uint64_t i = slotIndex(key, seg->log2);
      for (uint64_t n = 0; n <= seg->mask && found == nullptr; ++n, i = (i + 1) & seg->mask)
      {
        Slot& s = seg->slots[i];
        uint64_t k = s.key.load(std::memory_order_acquire);
        if (k == key) found = &s;
        else if (k != 0) continue;  // another key's slot: keep probing
        else if (s.key.compare_exchange_strong(k, key, std::memory_order_acq_rel))
        {
          seg->used.fetch_add(1, std::memory_order_relaxed);
          found = &s;
        }
        else if (k == key) found = &s;  // lost the race to a writer of the same key
      }
    }
    seg->active.fetch_sub(1, std::memory_order_release);
    if (found) return found;
    grow(seg);  // the segment filled up under us
  }
}

LocTable::Value LocTable::lookup(uint64_t key) const
{
  Value v;
  if (Slot* s = find(key)) v.raw = s->value.load(std::memory_order_acquire);
  if (_objidDiag && !v.hasLocation())
    counts.lookupMiss.fetch_add(1, std::memory_order_relaxed);
  return v;
}

uint64_t LocTable::store(uint64_t key, int pe, int epoch, bool arriving)
{
  Slot* s = findOrInsert(key);
  s->value.store(Value::make(pe, epoch, arriving).raw, std::memory_order_release);
  if (arriving) return 0;
  return fill(s);
}

// After a write that gave the key a location: take the waiter bits for the caller
// to wake, and clear the request flag, so the next rank that misses on this key
// sends a new request (section 3.3).
uint64_t LocTable::fill(Slot* s)
{
  const uint64_t w = s->waiters.exchange(0, std::memory_order_acq_rel);
  s->request.store(0, std::memory_order_release);
  if (_objidDiag && w) counts.fills.fetch_add(1, std::memory_order_relaxed);
  return w;
}

// An entry a PE of this process wrote about itself (section 3.5).
static inline bool ownWrite(const LocTable::Value& c)
{
  return c.hasLocation() && !c.byMessage() && CmiNodeOf(c.pe()) == CmiMyNode();
}

uint64_t LocTable::update(uint64_t key, int pe, int epoch, bool arriving, bool* written,
                          bool received)
{
  Slot* s = findOrInsert(key);
  // A received update answers or overtakes this process's request: it clears the
  // request flag whether or not it writes, so a reply that finds the entry already as
  // new does not leave the flag set with no request out. The exchange comes before
  // the value is read, which orders it with a concurrent claim (section 3.3).
  if (received) s->request.exchange(0, std::memory_order_acq_rel);
  const uint64_t want = Value::make(pe, epoch, arriving, received).raw;
  uint64_t cur = s->value.load(std::memory_order_acquire);
  for (;;)
  {
    Value c;
    c.raw = cur;
    if (c.hasLocation() && (epoch <= c.epoch() || (received && ownWrite(c))))
    {
      if (written) *written = false;
      return 0;
    }
    if (s->value.compare_exchange_weak(cur, want, std::memory_order_acq_rel,
                                       std::memory_order_acquire))
      break;
    // cur now holds the value another writer stored; redo the epoch test.
  }
  if (written) *written = true;
  if (arriving) return 0;
  return fill(s);
}

int LocTable::recordEmigration(uint64_t key, int fromPe, int toPe)
{
  Slot* s = find(key);
  if (s == nullptr)
    CmiAbort("LocTable::recordEmigration: no entry for an element leaving PE %d\n", fromPe);
  uint64_t cur = s->value.load(std::memory_order_acquire);
  for (;;)
  {
    Value c;
    c.raw = cur;
    if (!c.hasLocation() || c.pe() != fromPe)
      CmiAbort("LocTable::recordEmigration: the entry of an element leaving PE %d names"
               " PE %d (epoch %d)\n", fromPe, c.pe(), c.epoch());
    const int next = c.epoch() + 1;
    if (s->value.compare_exchange_weak(cur, Value::make(toPe, next, false).raw,
                                       std::memory_order_acq_rel, std::memory_order_acquire))
      return next;
  }
}

bool LocTable::clear(uint64_t key, int ifPe, int ifEpoch, bool keepOwn)
{
  Slot* s = find(key);
  if (s == nullptr) return false;
  if (ifPe < 0 && !keepOwn)
  {
    s->value.store(0, std::memory_order_release);
    return true;
  }
  uint64_t cur = s->value.load(std::memory_order_acquire);
  for (;;)
  {
    Value c;
    c.raw = cur;
    if (!c.hasLocation() || (ifPe >= 0 && c.pe() != ifPe) ||
        (ifEpoch >= 0 && c.epoch() != ifEpoch) || (keepOwn && ownWrite(c)))
      return false;
    if (s->value.compare_exchange_weak(cur, 0, std::memory_order_acq_rel,
                                       std::memory_order_acquire))
      return true;
  }
}

void LocTable::registerWaiter(uint64_t key, int rank)
{
  Slot* s = findOrInsert(key);
  s->waiters.fetch_or(1ull << (rank & 63), std::memory_order_acq_rel);
}

bool LocTable::claimRequest(uint64_t key)
{
  Slot* s = findOrInsert(key);
  return s->request.exchange(1, std::memory_order_acq_rel) == 0;
}

uint64_t LocTable::takeWaiters(uint64_t key)
{
  Slot* s = find(key);
  return s ? s->waiters.exchange(0, std::memory_order_acq_rel) : 0;
}

size_t LocTable::slots() const
{
  size_t n = 0;
  for (Segment* seg = head.load(std::memory_order_acquire); seg; seg = seg->older)
    n += seg->mask + 1;
  return n;
}

size_t LocTable::used() const
{
  size_t n = 0;
  for (Segment* seg = head.load(std::memory_order_acquire); seg; seg = seg->older)
    n += seg->used.load(std::memory_order_relaxed);
  return n;
}

int LocTable::segments() const
{
  int n = 0;
  for (Segment* seg = head.load(std::memory_order_acquire); seg; seg = seg->older) ++n;
  return n;
}

/*************************** IndexTable **************************/

void IndexTable::makeKey(uint64_t collection, const CkArrayIndex& idx, uint64_t key[KEY_WORDS])
{
  key[0] = collection | ((uint64_t)idx.nInts << 32) | ((uint64_t)idx.dimension << 48);
  for (int w = 1; w < KEY_WORDS; ++w) key[w] = 0;
  for (int i = 0; i < idx.nInts && i < CK_ARRAYINDEX_MAXLEN; ++i)
    key[1 + i / 2] |= (uint64_t)(uint32_t)idx.index[i] << (32 * (i & 1));
}

uint64_t IndexTable::hashKey(const uint64_t key[KEY_WORDS])
{
  uint64_t h = 0x9E3779B97F4A7C15ull;
  for (int w = 0; w < KEY_WORDS; ++w)
  {
    h ^= key[w];
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 31;
  }
  return h | 1;  // non-zero: 0 is the empty claim word
}

IndexTable::Segment* IndexTable::makeSegment(int log2, Segment* older)
{
  Segment* seg = new Segment;
  seg->older = older;
  seg->log2 = log2;
  seg->mask = (1ull << log2) - 1;
  seg->used.store(0, std::memory_order_relaxed);
  seg->active.store(0, std::memory_order_relaxed);
  seg->sealed.store(false, std::memory_order_relaxed);
  seg->words = new std::atomic<uint64_t>[(1ull << log2) * SLOT_WORDS]();
  return seg;
}

IndexTable::IndexTable(int log2Slots)
{
  if (log2Slots < 4) log2Slots = 4;
  if (log2Slots > 40) log2Slots = 40;
  head.store(makeSegment(log2Slots, nullptr), std::memory_order_release);
}

IndexTable::~IndexTable()
{
  Segment* seg = head.load(std::memory_order_acquire);
  while (seg)
  {
    Segment* older = seg->older;
    delete[] seg->words;
    delete seg;
    seg = older;
  }
}

std::atomic<uint64_t>* IndexTable::probe(Segment* seg, const uint64_t key[KEY_WORDS],
                                         uint64_t h, bool writer)
{
  uint64_t i = h >> (64 - seg->log2);
  for (uint64_t n = 0; n <= seg->mask; ++n, i = (i + 1) & seg->mask)
  {
    std::atomic<uint64_t>* slot = seg->words + i * SLOT_WORDS;
    const uint64_t claim = slot[0].load(std::memory_order_acquire);
    if (claim == 0) return nullptr;
    if (claim != h) continue;
    std::atomic<uint64_t>* idw = slot + 1 + KEY_WORDS;
    uint64_t id = idw->load(std::memory_order_acquire);
    if (id == 0)
    {
      // Claimed, key words being written. A writer waits for the publication (it
      // must not claim a second slot for what may be its own key); a reader treats
      // the binding as not there yet.
      if (!writer) return nullptr;
      while ((id = idw->load(std::memory_order_acquire)) == 0) {}
    }
    bool same = true;
    for (int w = 0; w < KEY_WORDS && same; ++w)
      same = slot[1 + w].load(std::memory_order_relaxed) == key[w];
    if (same) return idw;
  }
  return nullptr;
}

std::atomic<uint64_t>* IndexTable::find(const uint64_t key[KEY_WORDS], uint64_t h,
                                        bool writer) const
{
  for (Segment* seg = head.load(std::memory_order_acquire); seg; seg = seg->older)
    if (std::atomic<uint64_t>* idw = probe(seg, key, h, writer)) return idw;
  return nullptr;
}

void IndexTable::awaitGrowth(const Segment* from) const
{
  while (head.load(std::memory_order_acquire) == from) std::this_thread::yield();
}

// As LocTable::grow.
void IndexTable::grow(Segment* from)
{
  if (from->sealed.exchange(true, std::memory_order_seq_cst))
  {
    awaitGrowth(from);
    return;
  }
  Segment* bigger = makeSegment(from->log2 + 2, from);
  while (from->active.load(std::memory_order_seq_cst) != 0) std::this_thread::yield();
  head.store(bigger, std::memory_order_release);
}

bool IndexTable::lookup(uint64_t collection, const CkArrayIndex& idx, uint64_t& id) const
{
  uint64_t key[KEY_WORDS];
  makeKey(collection, idx, key);
  const std::atomic<uint64_t>* idw = find(key, hashKey(key), false);
  if (idw == nullptr) return false;
  id = idw->load(std::memory_order_acquire);
  return id != 0;
}

uint64_t IndexTable::bind(uint64_t collection, const CkArrayIndex& idx, uint64_t id)
{
  uint64_t key[KEY_WORDS];
  makeKey(collection, idx, key);
  const uint64_t h = hashKey(key);
  // A binding already in the table is found lock-free; a miss searches and inserts
  // while counted active in the head segment: see LocTable::findOrInsert.
  if (std::atomic<uint64_t>* idw = find(key, h, true))
  {
    uint64_t cur = idw->load(std::memory_order_acquire);
    for (;;)
    {
      if (cur == id) return id;
      if (idw->compare_exchange_weak(cur, id, std::memory_order_acq_rel,
                                     std::memory_order_acquire))
        return cur;
    }
  }
  for (;;)
  {
    Segment* seg = head.load(std::memory_order_acquire);
    seg->active.fetch_add(1, std::memory_order_seq_cst);
    if (seg->sealed.load(std::memory_order_seq_cst))
    {
      seg->active.fetch_sub(1, std::memory_order_release);
      awaitGrowth(seg);
      continue;
    }
    if (std::atomic<uint64_t>* idw = find(key, h, true))
    {
      seg->active.fetch_sub(1, std::memory_order_release);
      uint64_t cur = idw->load(std::memory_order_acquire);
      for (;;)
      {
        if (cur == id) return id;
        if (idw->compare_exchange_weak(cur, id, std::memory_order_acq_rel,
                                       std::memory_order_acquire))
          return cur;
      }
    }
    if ((seg->used.load(std::memory_order_relaxed) + 1) * 2 > seg->mask + 1)
    {
      seg->active.fetch_sub(1, std::memory_order_release);
      grow(seg);
      continue;
    }
    uint64_t i = h >> (64 - seg->log2);
    bool again = false;  // our key's claim appeared in this segment: look it up again
    bool claimed = false;
    for (uint64_t n = 0; n <= seg->mask && !again && !claimed; ++n, i = (i + 1) & seg->mask)
    {
      std::atomic<uint64_t>* slot = seg->words + i * SLOT_WORDS;
      uint64_t claim = slot[0].load(std::memory_order_acquire);
      if (claim == h) again = true;
      if (claim != 0) continue;
      if (slot[0].compare_exchange_strong(claim, h, std::memory_order_acq_rel))
      {
        for (int w = 0; w < KEY_WORDS; ++w) slot[1 + w].store(key[w], std::memory_order_relaxed);
        slot[1 + KEY_WORDS].store(id, std::memory_order_release);
        seg->used.fetch_add(1, std::memory_order_relaxed);
        claimed = true;
      }
      else if (claim == h) again = true;  // lost the claim to a writer of the same hash
      // else another key took the slot: keep probing
    }
    seg->active.fetch_sub(1, std::memory_order_release);
    if (claimed) return 0;
    // No slot claimed: the key is in the segment now (again), or no empty slot was
    // left for it. Growing guarantees progress in the second case.
    if (!again) grow(seg);
  }
}

size_t IndexTable::slots() const
{
  size_t n = 0;
  for (Segment* seg = head.load(std::memory_order_acquire); seg; seg = seg->older)
    n += seg->mask + 1;
  return n;
}

size_t IndexTable::used() const
{
  size_t n = 0;
  for (Segment* seg = head.load(std::memory_order_acquire); seg; seg = seg->older)
    n += seg->used.load(std::memory_order_relaxed);
  return n;
}

int IndexTable::segments() const
{
  int n = 0;
  for (Segment* seg = head.load(std::memory_order_acquire); seg; seg = seg->older) ++n;
  return n;
}

/*************************** RetiredIds **************************/

void RetiredIds::note(uint64_t fullId, const CkArrayIndex& idx)
{
  std::lock_guard<std::mutex> g(lock);
  map[fullId] = idx;
}

bool RetiredIds::find(uint64_t fullId, CkArrayIndex& idx) const
{
  std::lock_guard<std::mutex> g(lock);
  auto it = map.find(fullId);
  if (it == map.end()) return false;
  idx = it->second;
  return true;
}

size_t RetiredIds::size() const
{
  std::lock_guard<std::mutex> g(lock);
  return map.size();
}

namespace {

// The process's tables, and the +objid_diag report printed when the process exits
// (reconverse ends a run with exit(), which runs static destructors).
struct TableHolder
{
  LocTable* table = nullptr;
  IndexTable* index = nullptr;
  RetiredIds retired;
  std::once_flag once, onceIndex;
  ~TableHolder()
  {
    if (table && _objidDiag)
    {
      const LocTable::Counters& c = table->counters();
      fprintf(stderr,
              "[objid_diag] process %d: location table %zu slots in %d segments, %zu used;"
              " index table %zu slots in %d segments, %zu used; %zu retired ids;"
              " misses %llu (by index: no id %llu, no location %llu); requests sent %llu,"
              " skipped %llu; fills with waiters %llu, wakes sent %llu; repairs skipped"
              " (same process) %llu; growths %llu\n",
              CmiMyNode(), table->slots(), table->segments(), table->used(),
              index ? index->slots() : (size_t)0, index ? index->segments() : 0,
              index ? index->used() : (size_t)0, retired.size(),
              (unsigned long long)c.lookupMiss.load(), (unsigned long long)c.missNoId.load(),
              (unsigned long long)c.missNoLocation.load(),
              (unsigned long long)c.requestsSent.load(),
              (unsigned long long)c.requestsSkipped.load(), (unsigned long long)c.fills.load(),
              (unsigned long long)c.wakesSent.load(),
              (unsigned long long)c.repairsSkipped.load(),
              (unsigned long long)c.growths.load());
    }
    delete table;
    delete index;
  }
};
TableHolder _holder;

}  // namespace

LocTable& locTable()
{
  std::call_once(_holder.once, [] { _holder.table = new LocTable(_objidTableLog2); });
  return *_holder.table;
}

IndexTable& indexTable()
{
  std::call_once(_holder.onceIndex, [] { _holder.index = new IndexTable(_objidTableLog2); });
  return *_holder.index;
}

RetiredIds& retiredIds() { return _holder.retired; }

}  // namespace objid
}  // namespace ck
