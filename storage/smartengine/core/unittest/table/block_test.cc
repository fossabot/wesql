//  Portions Copyright (c) 2023, ApeCloud Inc Holding Limited
// Portions Copyright (c) 2020, Alibaba Group Holding Limited
//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
#include <stdio.h>
#include <algorithm>
#include <set>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "db/dbformat.h"
#include "db/memtable.h"
#include "db/db_test_util.h"
#include "db/write_batch_internal.h"
#include "table/block.h"
#include "table/block_builder.h"
#include "table/format.h"
#include "util/random.h"
#include "util/testharness.h"
#include "util/testutil.h"
#include "smartengine/db.h"
#include "smartengine/env.h"
#include "smartengine/iterator.h"
#include "smartengine/slice_transform.h"
#include "smartengine/table.h"

using namespace smartengine;
using namespace common;
using namespace util;
using namespace db;
using namespace monitor;

namespace smartengine {
namespace table {

static std::string RandomString(Random *rnd, int len) {
  std::string r;
  test::RandomString(rnd, len, &r);
  return r;
}
std::string GenerateKey(int primary_key, int secondary_key, int padding_size,
                        Random *rnd) {
  char buf[50];
  char *p = &buf[0];
  snprintf(buf, sizeof(buf), "%6d%4d", primary_key, secondary_key);
  std::string k(p);
  if (padding_size) {
    k += RandomString(rnd, padding_size);
  }

  return k;
}

// Generate random key value pairs.
// The generated key will be sorted. You can tune the parameters to generated
// different kinds of test key/value pairs for different scenario.
void GenerateRandomKVs(std::vector<std::string> *keys,
                       std::vector<std::string> *values, const int from,
                       const int len, const int step = 1,
                       const int padding_size = 0,
                       const int keys_share_prefix = 1) {
  Random rnd(302);

  // generate different prefix
  for (int i = from; i < from + len; i += step) {
    // generating keys that shares the prefix
    for (int j = 0; j < keys_share_prefix; ++j) {
      keys->emplace_back(GenerateKey(i, j, padding_size, &rnd));

      // 100 bytes values
      values->emplace_back(RandomString(&rnd, 100));
    }
  }
}

class BlockTest : public testing::Test {};

// block test
TEST_F(BlockTest, SimpleTest) {
  Random rnd(301);
  Options options = Options();
  std::unique_ptr<InternalKeyComparator> ic;
  ic.reset(new test::PlainInternalKeyComparator(options.comparator));

  std::vector<std::string> keys;
  std::vector<std::string> values;
  BlockBuilder builder(16);
  int num_records = 100000;

  GenerateRandomKVs(&keys, &values, 0, num_records);
  // add a bunch of records to a block
  for (int i = 0; i < num_records; i++) {
    builder.Add(keys[i], values[i]);
  }

  // read serialized contents of the block
  Slice rawblock = builder.Finish();

  // create block reader
  BlockContents contents;
  contents.data = rawblock;
  contents.cachable = false;
  Block reader(std::move(contents), kDisableGlobalSequenceNumber);

  // read contents of block sequentially
  int count = 0;
  InternalIterator *iter = reader.NewIterator(options.comparator);
  for (iter->SeekToFirst(); iter->Valid(); count++, iter->Next()) {
    // read kv from block
    Slice k = iter->key();
    Slice v = iter->value();

    // compare with lookaside array
    ASSERT_EQ(k.ToString().compare(keys[count]), 0);
    ASSERT_EQ(v.ToString().compare(values[count]), 0);
  }
  delete iter;

  // read block contents randomly
  iter = reader.NewIterator(options.comparator);
  for (int i = 0; i < num_records; i++) {
    // find a random key in the lookaside array
    int index = rnd.Uniform(num_records);
    Slice k(keys[index]);

    // search in block for this key
    iter->Seek(k);
    ASSERT_TRUE(iter->Valid());
    Slice v = iter->value();
    ASSERT_EQ(v.ToString().compare(values[index]), 0);
  }
  delete iter;
}

// return the block contents
BlockContents GetBlockContents(std::unique_ptr<BlockBuilder> *builder,
                               const std::vector<std::string> &keys,
                               const std::vector<std::string> &values,
                               const int prefix_group_size = 1) {
  builder->reset(new BlockBuilder(1 /* restart interval */));

  // Add only half of the keys
  for (size_t i = 0; i < keys.size(); ++i) {
    (*builder)->Add(keys[i], values[i]);
  }
  Slice rawblock = (*builder)->Finish();

  BlockContents contents;
  contents.data = rawblock;
  contents.cachable = false;

  return contents;
}

void CheckBlockContents(BlockContents contents, const int max_key,
                        const std::vector<std::string> &keys,
                        const std::vector<std::string> &values) {
  const size_t prefix_size = 6;
  // create block reader
  BlockContents contents_ref(contents.data, contents.cachable,
                             contents.compression_type);
  Block reader1(std::move(contents), kDisableGlobalSequenceNumber);
  Block reader2(std::move(contents_ref), kDisableGlobalSequenceNumber);

  std::unique_ptr<const SliceTransform> prefix_extractor(
      NewFixedPrefixTransform(prefix_size));

  std::unique_ptr<InternalIterator> regular_iter(
      reader2.NewIterator(BytewiseComparator()));

  // Seek existent keys
  for (size_t i = 0; i < keys.size(); i++) {
    regular_iter->Seek(keys[i]);
    ASSERT_OK(regular_iter->status());
    ASSERT_TRUE(regular_iter->Valid());

    Slice v = regular_iter->value();
    ASSERT_EQ(v.ToString().compare(values[i]), 0);
  }

  // Seek non-existent keys.
  // For hash index, if no key with a given prefix is not found, iterator will
  // simply be set as invalid; whereas the binary search based iterator will
  // return the one that is closest.
  for (int i = 1; i < max_key - 1; i += 2) {
    auto key = GenerateKey(i, 0, 0, nullptr);
    regular_iter->Seek(key);
    ASSERT_TRUE(regular_iter->Valid());
  }
}

// In this test case, no two key share same prefix.
TEST_F(BlockTest, SimpleIndexHash) {
  const int kMaxKey = 100000;
  std::vector<std::string> keys;
  std::vector<std::string> values;
  GenerateRandomKVs(&keys, &values, 0 /* first key id */,
                    kMaxKey /* last key id */, 2 /* step */,
                    8 /* padding size (8 bytes randomly generated suffix) */);

  std::unique_ptr<BlockBuilder> builder;
  auto contents = GetBlockContents(&builder, keys, values);

  CheckBlockContents(std::move(contents), kMaxKey, keys, values);
}

TEST_F(BlockTest, IndexHashWithSharedPrefix) {
  const int kMaxKey = 100000;
  // for each prefix, there will be 5 keys starts with it.
  const int kPrefixGroup = 5;
  std::vector<std::string> keys;
  std::vector<std::string> values;
  // Generate keys with same prefix.
  GenerateRandomKVs(&keys, &values, 0,  // first key id
                    kMaxKey,            // last key id
                    2,                  // step
                    10,                 // padding size,
                    kPrefixGroup);

  std::unique_ptr<BlockBuilder> builder;
  auto contents = GetBlockContents(&builder, keys, values, kPrefixGroup);

  CheckBlockContents(std::move(contents), kMaxKey, keys, values);
}

// A slow and accurate version of BlockReadAmpBitmap that simply store
// all the marked ranges in a set.
class BlockReadAmpBitmapSlowAndAccurate {
 public:
  void Mark(size_t start_offset, size_t end_offset) {
    assert(end_offset >= start_offset);

    marked_ranges_.emplace(end_offset, start_offset);
  }

  // Return true if any byte in this range was Marked
  bool IsAnyInRangeMarked(size_t start_offset, size_t end_offset) {
    auto it = marked_ranges_.lower_bound(
        std::make_pair(start_offset, static_cast<size_t>(0)));
    if (it == marked_ranges_.end()) {
      return false;
    }
    return start_offset <= it->first && end_offset >= it->second;
  }

 private:
  std::set<std::pair<size_t, size_t>> marked_ranges_;
};

TEST_F(BlockTest, BlockReadAmpBitmap) {
  std::vector<size_t> block_sizes = {
      1,                 // 1 byte
      32,                // 32 bytes
      61,                // 61 bytes
      64,                // 64 bytes
      512,               // 0.5 KB
      1024,              // 1 KB
      1024 * 4,          // 4 KB
      1024 * 10,         // 10 KB
      1024 * 50,         // 50 KB
      1024 * 1024,       // 1 MB
      1024 * 1024 * 4,   // 4 MB
      1024 * 1024 * 50,  // 10 MB
      777,
      124653,
  };
  const size_t kBytesPerBit = 64;
  size_t expected_estimate_useful = 0;
  size_t begin_total_read = 0;

  Random rnd(301);
  for (size_t block_size : block_sizes) {
    std::shared_ptr<Statistics> stats = CreateDBStatistics();
    BlockReadAmpBitmap read_amp_bitmap(block_size, kBytesPerBit);
    BlockReadAmpBitmapSlowAndAccurate read_amp_slow_and_accurate;
    size_t start_useful =
          TestGetThreadCount(CountPoint::READ_AMP_ESTIMATE_USEFUL_BYTES);

    size_t needed_bits = (block_size / kBytesPerBit);
    if (block_size % kBytesPerBit != 0) {
      needed_bits++;
    }
    size_t bitmap_size = needed_bits / 32;
    if (needed_bits % 32 != 0) {
      bitmap_size++;
    }
    size_t bits_in_bitmap = bitmap_size * 32;
    begin_total_read += needed_bits * kBytesPerBit;
    ASSERT_EQ(TestGetThreadCount(CountPoint::READ_AMP_TOTAL_READ_BYTES),
              begin_total_read);

    // Generate some random entries
    std::vector<size_t> random_entry_offsets;
    for (int i = 0; i < 1000; i++) {
      random_entry_offsets.push_back(rnd.Next() % block_size);
    }
    std::sort(random_entry_offsets.begin(), random_entry_offsets.end());
    auto it =
        std::unique(random_entry_offsets.begin(), random_entry_offsets.end());
    random_entry_offsets.resize(
        std::distance(random_entry_offsets.begin(), it));

    std::vector<std::pair<size_t, size_t>> random_entries;
    for (size_t i = 0; i < random_entry_offsets.size(); i++) {
      size_t entry_start = random_entry_offsets[i];
      size_t entry_end;
      if (i + 1 < random_entry_offsets.size()) {
        entry_end = random_entry_offsets[i + 1] - 1;
      } else {
        entry_end = block_size - 1;
      }
      random_entries.emplace_back(entry_start, entry_end);
    }
    // expected_estimate_useful = 0;
    for (size_t i = 0; i < random_entries.size(); i++) {
      auto &current_entry = random_entries[rnd.Next() % random_entries.size()];

      read_amp_bitmap.Mark(static_cast<uint32_t>(current_entry.first),
                           static_cast<uint32_t>(current_entry.second));
      read_amp_slow_and_accurate.Mark(current_entry.first,
                                      current_entry.second);

      size_t total_bits = 0;
      for (size_t bit_idx = 0; bit_idx < bits_in_bitmap; bit_idx++) {
        size_t start_rng = bit_idx * kBytesPerBit;
        size_t end_rng = (start_rng + kBytesPerBit) - 1;

        total_bits +=
            read_amp_slow_and_accurate.IsAnyInRangeMarked(start_rng, end_rng);
      }
      expected_estimate_useful = total_bits * kBytesPerBit;
      size_t got_estimate_useful = TestGetThreadCount(
              CountPoint::READ_AMP_ESTIMATE_USEFUL_BYTES) - start_useful;

      ASSERT_EQ(expected_estimate_useful, got_estimate_useful);
    }
  }
}

TEST_F(BlockTest, BlockWithReadAmpBitmap) {
  Random rnd(301);
  Options options = Options();
  std::unique_ptr<InternalKeyComparator> ic;
  ic.reset(new test::PlainInternalKeyComparator(options.comparator));
  QUERY_TRACE_RESET(true);
  auto perf_ctx = get_tls_query_perf_context();
  double start_useful_bytes = static_cast<double>(TestGetThreadCount(
          CountPoint::READ_AMP_ESTIMATE_USEFUL_BYTES));
  size_t start_total_bytes = TestGetThreadCount(
          CountPoint::READ_AMP_TOTAL_READ_BYTES);
  std::vector<std::string> keys;
  std::vector<std::string> values;
  BlockBuilder builder(16);
  int num_records = 10000;

  GenerateRandomKVs(&keys, &values, 0, num_records, 1);
  // add a bunch of records to a block
  for (int i = 0; i < num_records; i++) {
    builder.Add(keys[i], values[i]);
  }

  Slice rawblock = builder.Finish();
  const size_t kBytesPerBit = 8;
  double semi_acc_read_amp = 0.0;
  size_t read_bytes = 0;
  size_t block_size = 0;

  // Read the block sequentially using Next()
  {
    std::shared_ptr<Statistics> stats = CreateDBStatistics();

    // create block reader
    BlockContents contents;
    contents.data = rawblock;
    contents.cachable = true;
    Block reader(std::move(contents), kDisableGlobalSequenceNumber,
                 kBytesPerBit);

    // read contents of block sequentially
    BlockIter *iter = static_cast<BlockIter *>(
        reader.NewIterator(options.comparator, nullptr, true, stats.get()));
    block_size += rawblock.size();
    for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
      iter->value();
      read_bytes += iter->TEST_CurrentEntrySize();
      semi_acc_read_amp = static_cast<double>(read_bytes) / block_size;
      double useful_bytes = static_cast<double>(TestGetThreadCount(
          CountPoint::READ_AMP_ESTIMATE_USEFUL_BYTES)) - start_useful_bytes;
      uint64_t total_bytes = TestGetThreadCount(
          CountPoint::READ_AMP_TOTAL_READ_BYTES) - start_total_bytes;
      double read_amp = useful_bytes / total_bytes;

      // Error in read amplification will be less than 1% if we are reading
      // sequentially
      double error_pct = fabs(semi_acc_read_amp - read_amp) * 100;
      EXPECT_LT(error_pct, 1);
    }

    delete iter;
  }

  // Read the block sequentially using Seek()
  {
    std::shared_ptr<Statistics> stats = CreateDBStatistics();

    // create block reader
    BlockContents contents;
    contents.data = rawblock;
    contents.cachable = true;
    Block reader(std::move(contents), kDisableGlobalSequenceNumber,
                 kBytesPerBit);

    BlockIter *iter = static_cast<BlockIter *>(
        reader.NewIterator(options.comparator, nullptr, true, stats.get()));
    block_size += rawblock.size();
    for (int i = 0; i < num_records; i++) {
      Slice k(keys[i]);

      // search in block for this key
      iter->Seek(k);
      iter->value();
      read_bytes += iter->TEST_CurrentEntrySize();
      semi_acc_read_amp = static_cast<double>(read_bytes) / block_size;

      double useful_bytes = static_cast<double>(TestGetThreadCount(
          CountPoint::READ_AMP_ESTIMATE_USEFUL_BYTES)) - start_useful_bytes;;
      uint64_t total_bytes = TestGetThreadCount(
          CountPoint::READ_AMP_TOTAL_READ_BYTES) - start_total_bytes;
      double read_amp = useful_bytes / total_bytes;

      // Error in read amplification will be less than 1% if we are reading
      // sequentially
      double error_pct = fabs(semi_acc_read_amp - read_amp) * 100;
      EXPECT_LT(error_pct, 1);
    }
    delete iter;
  }

  // Read the block randomly
  {
    std::shared_ptr<Statistics> stats = CreateDBStatistics();

    // create block reader
    BlockContents contents;
    contents.data = rawblock;
    contents.cachable = true;
    Block reader(std::move(contents), kDisableGlobalSequenceNumber,
                 kBytesPerBit);

    BlockIter *iter = static_cast<BlockIter *>(
        reader.NewIterator(options.comparator, nullptr, true, stats.get()));
    block_size += rawblock.size();
    std::unordered_set<int> read_keys;
    for (int i = 0; i < num_records; i++) {
      int index = rnd.Uniform(num_records);
      Slice k(keys[index]);

      iter->Seek(k);
      iter->value();
      if (read_keys.find(index) == read_keys.end()) {
        read_keys.insert(index);
        read_bytes += iter->TEST_CurrentEntrySize();
      }

      semi_acc_read_amp = static_cast<double>(read_bytes) / block_size;
      double useful_bytes = static_cast<double>(TestGetThreadCount(
          CountPoint::READ_AMP_ESTIMATE_USEFUL_BYTES)) - start_useful_bytes;
      uint64_t total_bytes = TestGetThreadCount(
          CountPoint::READ_AMP_TOTAL_READ_BYTES) - start_total_bytes;
      double read_amp = useful_bytes / total_bytes;

      double error_pct = fabs(semi_acc_read_amp - read_amp) * 100;
      // Error in read amplification will be less than 2% if we are reading
      // randomly
      EXPECT_LT(error_pct, 2);
    }
    delete iter;
  }
}

TEST_F(BlockTest, ReadAmpBitmapPow2) {
  std::shared_ptr<Statistics> stats = CreateDBStatistics();
  ASSERT_EQ(BlockReadAmpBitmap(100, 1).GetBytesPerBit(), 1);
  ASSERT_EQ(BlockReadAmpBitmap(100, 2).GetBytesPerBit(), 2);
  ASSERT_EQ(BlockReadAmpBitmap(100, 4).GetBytesPerBit(), 4);
  ASSERT_EQ(BlockReadAmpBitmap(100, 8).GetBytesPerBit(), 8);
  ASSERT_EQ(BlockReadAmpBitmap(100, 16).GetBytesPerBit(), 16);
  ASSERT_EQ(BlockReadAmpBitmap(100, 32).GetBytesPerBit(), 32);

  ASSERT_EQ(BlockReadAmpBitmap(100, 3).GetBytesPerBit(), 2);
  ASSERT_EQ(BlockReadAmpBitmap(100, 7).GetBytesPerBit(), 4);
  ASSERT_EQ(BlockReadAmpBitmap(100, 11).GetBytesPerBit(), 8);
  ASSERT_EQ(BlockReadAmpBitmap(100, 17).GetBytesPerBit(), 16);
  ASSERT_EQ(BlockReadAmpBitmap(100, 33).GetBytesPerBit(), 32);
  ASSERT_EQ(BlockReadAmpBitmap(100, 35).GetBytesPerBit(), 32);
}

}  // namespace table
}  // namespace smartengine

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
	smartengine::util::test::init_logger(__FILE__);
  return RUN_ALL_TESTS();
}
