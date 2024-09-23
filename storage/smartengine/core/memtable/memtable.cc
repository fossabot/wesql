//  Portions Copyright (c) 2023, ApeCloud Inc Holding Limited
// Portions Copyright (c) 2020, Alibaba Group Holding Limited
//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "memtable/memtable.h"

#include "db/dbformat.h"
#include "db/pinned_iterators_manager.h"
#include "logger/log_module.h"
#include "monitoring/query_perf_context.h"
#include "monitoring/statistics.h"
#include "table/internal_iterator.h"
#include "table/iterator_wrapper.h"
#include "table/merging_iterator.h"
#include "util/arena.h"
#include "util/autovector.h"
#include "util/coding.h"

namespace smartengine {
using namespace common;
using namespace util;
using namespace memtable;
using namespace table;
using namespace monitor;
using namespace storage;

namespace memtable {
using namespace db;
Slice MemTableRep::UserKey(const char* key) const {
  Slice slice = GetLengthPrefixedSlice(key);
  return Slice(slice.data(), slice.size() - 8);
}

KeyHandle MemTableRep::Allocate(const size_t len, char** buf) {
  *buf = allocator_->Allocate(len);
  return static_cast<KeyHandle>(*buf);
}

void MemTableRep::Get(const LookupKey& k, void* callback_args,
                      bool (*callback_func)(void* arg, const char* entry)) {
  auto iter = GetDynamicPrefixIterator();
  for (iter->Seek(k.internal_key(), k.memtable_key().data());
       iter->Valid() && callback_func(callback_args, iter->key());
       iter->Next()) {
  }
}
}
namespace db {

MemTableOptions::MemTableOptions(const ImmutableCFOptions& ioptions,
                                 const MutableCFOptions& mutable_cf_options)
    : write_buffer_size(mutable_cf_options.write_buffer_size),
      flush_delete_percent(mutable_cf_options.flush_delete_percent),
      flush_delete_percent_trigger(mutable_cf_options.flush_delete_percent_trigger),
      flush_delete_record_trigger(mutable_cf_options.flush_delete_record_trigger),
      statistics(ioptions.statistics)
{}

MemTable::MemTable(const InternalKeyComparator& cmp,
                   const ImmutableCFOptions& ioptions,
                   const MutableCFOptions& mutable_cf_options,
                   WriteBufferManager* write_buffer_manager,
                   SequenceNumber latest_seq)
    : comparator_(cmp),
      moptions_(ioptions, mutable_cf_options),
      refs_(0),
      kArenaBlockSize(OptimizeBlockSize(MEMTABLE_ARENA_BLOCK_SIZE)),
      arena_(MEMTABLE_ARENA_BLOCK_SIZE, 0 /**huge_page_size*/, memory::ModId::kMemtable),
      allocator_(&arena_, write_buffer_manager),
      table_(ioptions.memtable_factory->CreateMemTableRep(comparator_, &allocator_)),
      data_size_(0),
      num_entries_(0),
      num_deletes_(0),
      flush_in_progress_(false),
      flush_completed_(false),
      file_number_(0),
      dump_in_progress_(false),
      dump_completed_(false),
      delete_triggered_(false),
      first_seqno_(0),
      last_seqno_(0),
      earliest_seqno_(latest_seq),
      creation_seq_(latest_seq),
      mem_next_logfile_number_(0),
      recovery_point_(),
      min_prep_log_referenced_(0),
      temp_min_prep_log_(UINT64_MAX),
      flush_state_(FLUSH_NOT_REQUESTED),
      env_(ioptions.env),
      no_flush_(false),
      dump_seq_(0)
{
  UpdateFlushState();
  // something went wrong if we need to flush before inserting anything
  assert(!ShouldScheduleFlush());

#ifndef NDEBUG
  SE_LOG(DEBUG, " New MemTable ", K(moptions_.flush_delete_percent),
      K(moptions_.flush_delete_record_trigger), K(moptions_.flush_delete_percent_trigger));
#endif // NDEBUG
}

MemTable::~MemTable() {
  assert(refs_ == 0);
}
size_t MemTable::ApproximateMemoryAllocated() {
  autovector<size_t> usages = {arena_.MemoryAllocatedBytes(),
                               table_->ApproximateMemoryUsage()};
  size_t total_usage = 0;
  for (size_t usage : usages) {
    // If usage + total_usage >= kMaxSizet, return kMaxSizet.
    // the following variation is to avoid numeric overflow.
    if (usage >= port::kMaxSizet - total_usage) {
      return port::kMaxSizet;
    }
    total_usage += usage;
  }
  // otherwise, return the actual usage
  return total_usage;
}

size_t MemTable::ApproximateMemoryUsage() {
  autovector<size_t> usages = {arena_.ApproximateMemoryUsage(),
                               table_->ApproximateMemoryUsage()};
  size_t total_usage = 0;
  for (size_t usage : usages) {
    // If usage + total_usage >= kMaxSizet, return kMaxSizet.
    // the following variation is to avoid numeric overflow.
    if (usage >= port::kMaxSizet - total_usage) {
      return port::kMaxSizet;
    }
    total_usage += usage;
  }
  // otherwise, return the actual usage
  return total_usage;
}

bool MemTable::ShouldFlushNow() {
  if (no_flush_) {
    return false;
  }
  // In a lot of times, we cannot allocate arena blocks that exactly matches the
  // buffer size. Thus we have to decide if we should over-allocate or
  // under-allocate.
  // This constant variable can be interpreted as: if we still have more than
  // "kAllowOverAllocationRatio * kArenaBlockSize" space left, we'd try to over
  // allocate one more block.
  const double kAllowOverAllocationRatio = 0.6;

  // If arena still have room for new block allocation, we can safely say it
  // shouldn't flush.
  auto allocated_memory = table_->ApproximateMemoryUsage() +
                          arena_.MemoryAllocatedBytes();

  // We would switch memtable if too many deletion records, when
  // a) delete entries has exceed a threshold(flush_delete_record_trigger)
  // or b) total entries has exceed the water line(flush_delete_percent_trigger and
  //       delete percent exceed the threshold(flush_delete_percent)
  uint64_t total_entries = num_entries_.load(std::memory_order_relaxed);
  uint64_t delete_entries = num_deletes_.load(std::memory_order_relaxed);

#ifndef NDEBUG
  SE_LOG(DEBUG, "check ", K(total_entries), K(delete_entries),
      K(moptions_.flush_delete_percent),
      K(moptions_.flush_delete_record_trigger),
      K(moptions_.flush_delete_percent_trigger));
#endif

  if ((delete_entries >= (uint64_t)moptions_.flush_delete_record_trigger) ||
      (moptions_.flush_delete_percent > 0 &&
       total_entries >= (uint64_t)moptions_.flush_delete_percent_trigger &&
       (total_entries * moptions_.flush_delete_percent <= delete_entries * 100))) {
    delete_triggered_ = true;
    return true;
  }

  // if we can still allocate one more block without exceeding the
  // over-allocation ratio, then we should not flush.
  if (allocated_memory + kArenaBlockSize <
      moptions_.write_buffer_size +
          kArenaBlockSize * kAllowOverAllocationRatio) {
    return false;
  }

  // if user keeps adding entries that exceeds moptions.write_buffer_size,
  // we need to flush earlier even though we still have much available
  // memory left.
  if (allocated_memory > moptions_.write_buffer_size +
                             kArenaBlockSize * kAllowOverAllocationRatio) {
    return true;
  }

  // In this code path, Arena has already allocated its "last block", which
  // means the total allocatedmemory size is either:
  //  (1) "moderately" over allocated the memory (no more than `0.6 * arena
  // block size`. Or,
  //  (2) the allocated memory is less than write buffer size, but we'll stop
  // here since if we allocate a new arena block, we'll over allocate too much
  // more (half of the arena block size) memory.
  //
  // In either case, to avoid over-allocate, the last block will stop allocation
  // when its usage reaches a certain ratio, which we carefully choose "0.75
  // full" as the stop condition because it addresses the following issue with
  // great simplicity: What if the next inserted entry's size is
  // bigger than AllocatedAndUnused()?
  //
  // The answer is: if the entry size is also bigger than 0.25 *
  // kArenaBlockSize, a dedicated block will be allocated for it; otherwise
  // arena will anyway skip the AllocatedAndUnused() and allocate a new, empty
  // and regular block. In either case, we *overly* over-allocated.
  //
  // Therefore, setting the last block to be at most "0.75 full" avoids both
  // cases.
  //
  // NOTE: the average percentage of waste space of this approach can be counted
  // as: "arena block size * 0.25 / write buffer size". User who specify a small
  // write buffer size and/or big arena block size may suffer.
  return arena_.AllocatedAndUnused() < kArenaBlockSize / 4;
}

void MemTable::UpdateFlushState() {
  auto state = flush_state_.load(std::memory_order_relaxed);
  if (state == FLUSH_NOT_REQUESTED && ShouldFlushNow()) {
    // ignore CAS failure, because that means somebody else requested
    // a flush
    flush_state_.compare_exchange_strong(state, FLUSH_REQUESTED,
                                         std::memory_order_relaxed,
                                         std::memory_order_relaxed);
  }
}

int MemTable::KeyComparator::operator()(const char* prefix_len_key1,
                                        const char* prefix_len_key2) const {
  // Internal keys are encoded as length-prefixed strings.
  Slice k1 = GetLengthPrefixedSlice(prefix_len_key1);
  Slice k2 = GetLengthPrefixedSlice(prefix_len_key2);
  return comparator.Compare(k1, k2);
}

int MemTable::KeyComparator::operator()(const char* prefix_len_key,
                                        const Slice& key) const {
  // Internal keys are encoded as length-prefixed strings.
  Slice a = GetLengthPrefixedSlice(prefix_len_key);
  return comparator.Compare(a, key);
}

// Encode a suitable internal key target for "target" and return it.
// Uses *scratch as scratch space, and the returned pointer will point
// into this scratch space.
const char* EncodeKey(std::string* scratch, const Slice& target) {
  scratch->clear();
  PutVarint32(scratch, static_cast<uint32_t>(target.size()));
  scratch->append(target.data(), target.size());
  return scratch->data();
}

class DumpMemIterator : public InternalIterator
{
 public:
  DumpMemIterator(const common::SequenceNumber max_seq,
                  InternalIterator *mem_iter)
      : dump_max_seq_(max_seq),
        mem_iter_(mem_iter),
        valid_(true) {
  }
  virtual ~DumpMemIterator() override {}
  virtual void SeekToFirst() override {
    assert(mem_iter_);
    if (IS_NULL(mem_iter_)) {
      valid_ = false;
    } else {
      mem_iter_->SeekToFirst();
      valid_ = mem_iter_->Valid();
      pick_next_dump_item();
    }
  }

  virtual void SeekToLast() override {
    assert(mem_iter_);
    if (IS_NULL(mem_iter_)) {
      valid_ = false;
    } else {
      mem_iter_->SeekToLast();
      valid_ = mem_iter_->Valid();
      pick_prev_dump_item();
    }
  }

  virtual void Seek(const common::Slice& target) override {
    mem_iter_->Seek(target);
  }

  virtual void SeekForPrev(const common::Slice& target) override {
    mem_iter_->SeekForPrev(target);
  }

  virtual void Next() override {
    assert(mem_iter_);
    mem_iter_->Next();
    pick_next_dump_item();
  }

  virtual void Prev() override {
    assert(mem_iter_);
    mem_iter_->Prev();
    pick_prev_dump_item();
  }

  virtual common::Slice key() const override {
    assert(mem_iter_);
    return mem_iter_->key();
  }

  virtual common::Slice value() const override {
    assert(mem_iter_);
    return mem_iter_->value();
  }

  virtual bool Valid() const override {
    return valid_ && (nullptr != mem_iter_) && mem_iter_->Valid();
  }

  // no use
  virtual common::Status status() const override {
    assert(mem_iter_);
    return mem_iter_->status();
  }

 private:
  void pick_next_dump_item() {
    assert(mem_iter_);
    while (mem_iter_->Valid()) {
      Slice key = mem_iter_->key();
      // todo yeti check key size
      common::SequenceNumber key_seq = ExtractKeySeq(key);
      if (key_seq <= dump_max_seq_) {
        break;
      } else {
        mem_iter_->Next();
      }
    }
    valid_ = mem_iter_->Valid();
  }
  void pick_prev_dump_item() {
    assert(mem_iter_);
    while (mem_iter_->Valid()) {
      Slice key = mem_iter_->key();
      common::SequenceNumber key_seq = ExtractKeySeq(key);
      if (key_seq <= dump_max_seq_) {
        break;
      } else {
        mem_iter_->Prev();
      }
    }
    valid_ = mem_iter_->Valid();
  }
  common::SequenceNumber dump_max_seq_;
  InternalIterator *mem_iter_;
  bool valid_;
};

class MemTableIterator : public InternalIterator {
 public:
  MemTableIterator(const MemTable& mem,
                   const ReadOptions& read_options,
                   Arena* arena)
      : comparator_(mem.comparator_),
        valid_(false),
        arena_mode_(arena != nullptr)
  {
    iter_ = mem.table_->GetIterator(arena);
    set_source(ROW_SOURCE_MEMTABLE);
  }

  virtual ~MemTableIterator() override
  {
#ifndef NDEBUG
    // Assert that the MemTableIterator is never deleted while
    // Pinning is Enabled.
    assert(!pinned_iters_mgr_ ||
           (pinned_iters_mgr_ && !pinned_iters_mgr_->PinningEnabled()));
#endif
    if (arena_mode_) {
      iter_->~Iterator();
    } else {
      delete iter_;
    }
  }

#ifndef NDEBUG
  virtual void SetPinnedItersMgr(
      PinnedIteratorsManager* pinned_iters_mgr) override {
    pinned_iters_mgr_ = pinned_iters_mgr;
  }
  PinnedIteratorsManager* pinned_iters_mgr_ = nullptr;
#endif

  virtual bool Valid() const override 
  { 
    return valid_ && (end_ikey_.size() > 0
        ? comparator_.comparator.Compare(GetLengthPrefixedSlice(iter_->key()), end_ikey_) < 0 : true);
  }
  virtual void Seek(const Slice& k) override {
    iter_->Seek(k, nullptr);
    valid_ = iter_->Valid();
  }
  virtual void SeekForPrev(const Slice& k) override {
    iter_->Seek(k, nullptr);
    valid_ = iter_->Valid();
    if (!Valid()) {
      SeekToLast();
    }
    while (Valid() && comparator_.comparator.Compare(k, key()) < 0) {
      Prev();
    }
  }
  virtual void SeekToFirst() override {
    iter_->SeekToFirst();
    valid_ = iter_->Valid();
  }
  virtual void SeekToLast() override {
    iter_->SeekToLast();
    valid_ = iter_->Valid();
  }
  virtual void Next() override {
    QUERY_COUNT(CountPoint::MEMTABLE_NEXT);
    assert(Valid());
    iter_->Next();
    valid_ = iter_->Valid();
  }
  virtual void Prev() override {
    QUERY_COUNT(CountPoint::MEMTABLE_PREV);
    assert(Valid());
    iter_->Prev();
    valid_ = iter_->Valid();
  }
  virtual Slice key() const override {
    assert(Valid());
    return GetLengthPrefixedSlice(iter_->key());
  }
  virtual Slice value() const override {
    assert(Valid());
    Slice key_slice = GetLengthPrefixedSlice(iter_->key());
    return GetLengthPrefixedSlice(key_slice.data() + key_slice.size());
  }

  virtual Status status() const override { return Status::OK(); }

  virtual bool IsKeyPinned() const override {
    // memtable data is always pinned
    return true;
  }

  virtual bool IsValuePinned() const override { return true; }

 private:
  const MemTable::KeyComparator comparator_;
  MemTableRep::Iterator* iter_;
  bool valid_;
  bool arena_mode_;

  // No copying allowed
  MemTableIterator(const MemTableIterator&);
  void operator=(const MemTableIterator&);
};

InternalIterator* MemTable::NewIterator(const ReadOptions& read_options,
                                        Arena* arena) const {
  assert(arena != nullptr);
  if (nullptr != arena) {
    return PLACEMENT_NEW(MemTableIterator, *arena, *this, read_options, arena);
  } else {
    return nullptr;
  }
}

InternalIterator* MemTable::NewDumpIterator(const ReadOptions& read_options,
                                            const SequenceNumber max_seq,
                                            Arena &arena) const {
  InternalIterator *mem_iter = NewIterator(read_options, &arena);
  return PLACEMENT_NEW(DumpMemIterator, arena, max_seq, mem_iter);
}

MemTable::MemTableStats MemTable::ApproximateStats(const Slice& start_ikey,
                                                   const Slice& end_ikey) {
  uint64_t entry_count = table_->ApproximateNumEntries(start_ikey, end_ikey);
  if (entry_count == 0) {
    return {0, 0};
  }
  uint64_t n = num_entries_.load(std::memory_order_relaxed);
  if (n == 0) {
    return {0, 0};
  }
  if (entry_count > n) {
    // table_->ApproximateNumEntries() is just an estimate so it can
    // be larger than actual entries we have. Cap it to entries we have
    // to limit the inaccuracy.
    entry_count = n;
  }
  uint64_t data_size = data_size_.load(std::memory_order_relaxed);
  return {entry_count * (data_size / n), entry_count};
}

void MemTable::Add(SequenceNumber s, ValueType type,
                   const Slice& key, /* user key */
                   const Slice& value, bool allow_concurrent,
                   MemTablePostProcessInfo* post_process_info) {
  // Format of an entry is concatenation of:
  //  key_size     : varint32 of internal_key.size()
  //  key bytes    : char[internal_key.size()]
  //  value_size   : varint32 of value.size()
  //  value bytes  : char[value.size()]
  uint32_t key_size = static_cast<uint32_t>(key.size());
  uint32_t val_size = static_cast<uint32_t>(value.size());
  uint32_t internal_key_size = key_size + 8;
  const uint32_t encoded_len = VarintLength(internal_key_size) +
                               internal_key_size + VarintLength(val_size) +
                               val_size;
  char* buf = nullptr;
  KeyHandle handle = table_->Allocate(encoded_len, &buf);

  char* p = EncodeVarint32(buf, internal_key_size);
  memcpy(p, key.data(), key_size);
  Slice key_slice(p, key_size);
  p += key_size;
  uint64_t packed = PackSequenceAndType(s, type);
  EncodeFixed64(p, packed);
  p += 8;
  p = EncodeVarint32(p, val_size);
  memcpy(p, value.data(), val_size);
  assert((unsigned)(p + val_size - buf) == (unsigned)encoded_len);
  if (!allow_concurrent) {
    table_->Insert(handle);

    // this is a bit ugly, but is the way to avoid locked instructions
    // when incrementing an atomic
    num_entries_.store(num_entries_.load(std::memory_order_relaxed) + 1,
                       std::memory_order_relaxed);
    data_size_.store(data_size_.load(std::memory_order_relaxed) + encoded_len,
                     std::memory_order_relaxed);
    if (type == kTypeDeletion || type == kTypeSingleDeletion) {
      num_deletes_.store(num_deletes_.load(std::memory_order_relaxed) + 1,
                         std::memory_order_relaxed);
    }

    // The first sequence number inserted into the memtable
    // assert(first_seqno_ == 0 || s > first_seqno_);
    if (first_seqno_ == 0) {
      first_seqno_.store(s, std::memory_order_relaxed);

      if (earliest_seqno_ == kMaxSequenceNumber) {
        earliest_seqno_.store(GetFirstSequenceNumber(),
                              std::memory_order_relaxed);
      }
      assert(first_seqno_.load() >= earliest_seqno_.load());
    }
    if (last_seqno_ == 0) {
      last_seqno_.store(s, std::memory_order_relaxed);
    }
    assert(post_process_info == nullptr);
    UpdateFlushState();
  } else {
    table_->InsertConcurrently(handle);

    assert(post_process_info != nullptr);
    post_process_info->num_entries++;
    post_process_info->data_size += encoded_len;
    if (type == kTypeDeletion || type == kTypeSingleDeletion) {
      post_process_info->num_deletes++;
    }

    // atomically update first_seqno_ and earliest_seqno_.
    uint64_t cur_seq_num = first_seqno_.load(std::memory_order_relaxed);
    while ((cur_seq_num == 0 || s < cur_seq_num) &&
           !first_seqno_.compare_exchange_weak(cur_seq_num, s)) {
    }
    uint64_t cur_earliest_seqno =
        earliest_seqno_.load(std::memory_order_relaxed);
    while (
        (cur_earliest_seqno == kMaxSequenceNumber || s < cur_earliest_seqno) &&
        !first_seqno_.compare_exchange_weak(cur_earliest_seqno, s)) {
    }
    uint64_t cur_last_seq_num = last_seqno_.load(std::memory_order_relaxed);
    while ((cur_last_seq_num == 0 || s > cur_last_seq_num) &&
           !last_seqno_.compare_exchange_weak(cur_last_seq_num, s)) {
    }
  }
}

// Callback from MemTable::Get()
namespace {

struct Saver {
  Status* status;
  const LookupKey* key;
  bool* found_final_value;  // Is value set correctly? Used by KeyMayExist
  std::string* value;
  SequenceNumber seq;
  MemTable* mem;
  Statistics* statistics;
  Env* env_;
};
}  // namespace

static bool SaveValue(void* arg, const char* entry) {
  Saver* s = reinterpret_cast<Saver*>(arg);
  se_assert(nullptr != s);

  // entry format is:
  //    klength  varint32
  //    userkey  char[klength-8]
  //    tag      uint64
  //    vlength  varint32
  //    value    char[vlength]
  // Check that it belongs to same user key.  We do not check the
  // sequence number since the Seek() call above should have skipped
  // all entries with overly large sequence numbers.
  uint32_t key_length;
  const char* key_ptr = GetVarint32Ptr(entry, entry + 5, &key_length);
  if (s->mem->GetInternalKeyComparator().user_comparator()->Equal(
          Slice(key_ptr, key_length - 8), s->key->user_key())) {
    // Correct user key
    const uint64_t tag = DecodeFixed64(key_ptr + key_length - 8);
    ValueType type;
    UnPackSequenceAndType(tag, &s->seq, &type);

    switch (type) {
      case kTypeValue: {
        Slice v = GetLengthPrefixedSlice(key_ptr + key_length);
        *(s->status) = Status::OK();
        if (nullptr != s->value) {
          s->value->assign(v.data(), v.size());
        }
        *(s->found_final_value) = true;
        return false;
      }
      case kTypeDeletion:
      case kTypeSingleDeletion: {
        *(s->status) = Status::NotFound();
        *(s->found_final_value) = true;
        return false;
      }
      default:
        se_assert(false);
        return true;
    }
  }

  // s->state could be Corrupt or Notfound
  return false;
}

bool MemTable::Get(LookupKey& key,
                   std::string* value,
                   Status* s,
                   SequenceNumber* seq,
                   const ReadOptions& read_opts)
{
  // The sequence number is updated synchronously in version_set.h
  if (IsEmpty()) {
    // Avoiding recording stats for speed.
    return false;
  }
  QUERY_TRACE_SCOPE(TracePoint::MEMTABLE_GET);

  bool found_final_value = false;
  Saver saver;
  saver.status = s;
  saver.found_final_value = &found_final_value;
  saver.key = &key;
  saver.value = value;
  saver.seq = kMaxSequenceNumber;
  saver.mem = this;
  saver.statistics = moptions_.statistics;
  saver.env_ = env_;
  table_->Get(key, &saver, SaveValue);

  *seq = saver.seq;

  return found_final_value;
}

// We do not check flush_delete_record_trigger here, since this memtable has
// been switched.
void MemTable::update_delete_trigger() {
  if (moptions_.flush_delete_percent > 0 &&
      (num_entries_.load(std::memory_order_relaxed) > 0
       && num_entries_.load(std::memory_order_relaxed) *
       moptions_.flush_delete_percent <= num_deletes() * 100)) {
    delete_triggered_ = true;
  }
}

void MemTable::RefLogContainingPrepSection(uint64_t log) {
  assert(log > 0);
  auto cur = min_prep_log_referenced_.load();
  while ((log < cur || cur == 0) &&
         !min_prep_log_referenced_.compare_exchange_strong(cur, log)) {
    cur = min_prep_log_referenced_.load();
  }
}

uint64_t MemTable::GetMinLogContainingPrepSection() {
  return min_prep_log_referenced_.load();
}

void MemTable::set_log_containing_prepsec(uint64_t log_number, uint64_t &old_value) {
  old_value = min_prep_log_referenced_.exchange(log_number);
}

}
}  // namespace smartengine
