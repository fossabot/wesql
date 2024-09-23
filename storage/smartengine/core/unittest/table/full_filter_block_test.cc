//  Portions Copyright (c) 2023, ApeCloud Inc Holding Limited
// Portions Copyright (c) 2020, Alibaba Group Holding Limited
//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.

#include "table/full_filter_block.h"

#include "memory/base_malloc.h"
#include "table/table.h"
#include "util/coding.h"
#include "util/hash.h"
#include "util/string_util.h"
#include "util/testharness.h"
#include "util/testutil.h"

namespace smartengine {
using namespace common;
using namespace util;

namespace table {

class TestFilterBitsBuilder : public FilterBitsBuilder {
 public:
  explicit TestFilterBitsBuilder() {}

  // Add Key to filter
  virtual void AddKey(const Slice& key) override {
    hash_entries_.push_back(Hash(key.data(), key.size(), 1));
  }

  // Generate the filter using the keys that are added
  virtual Slice Finish(std::unique_ptr<char[],  
                          memory::ptr_delete<char>>* buf) override {
    uint32_t len = static_cast<uint32_t>(hash_entries_.size()) * 4;
    char* data = reinterpret_cast<char*>(memory::base_malloc(len));
    for (size_t i = 0; i < hash_entries_.size(); i++) {
      EncodeFixed32(data + i * 4, hash_entries_[i]);
    }

    buf->reset(data);
    return Slice(data, len);
  }

 private:
  std::vector<uint32_t> hash_entries_;
};

class TestFilterBitsReader : public FilterBitsReader {
 public:
  explicit TestFilterBitsReader(const Slice& contents)
      : data_(contents.data()), len_(static_cast<uint32_t>(contents.size())) {}

  virtual bool MayMatch(const Slice& entry) override {
    uint32_t h = Hash(entry.data(), entry.size(), 1);
    for (size_t i = 0; i + 4 <= len_; i += 4) {
      if (h == DecodeFixed32(data_ + i)) {
        return true;
      }
    }
    return false;
  }

 private:
  const char* data_;
  uint32_t len_;
};

class TestHashFilter : public FilterPolicy {
 public:
  virtual const char* Name() const override { return "TestHashFilter"; }

  virtual void CreateFilter(const Slice* keys, int n,
                            std::string* dst) const override {
    for (int i = 0; i < n; i++) {
      uint32_t h = Hash(keys[i].data(), keys[i].size(), 1);
      PutFixed32(dst, h);
    }
  }

  virtual bool KeyMayMatch(const Slice& key,
                           const Slice& filter) const override {
    uint32_t h = Hash(key.data(), key.size(), 1);
    for (unsigned int i = 0; i + 4 <= filter.size(); i += 4) {
      if (h == DecodeFixed32(filter.data() + i)) {
        return true;
      }
    }
    return false;
  }

  virtual FilterBitsBuilder* GetFilterBitsBuilder() const override {
    return new TestFilterBitsBuilder();
  }

  virtual FilterBitsReader* GetFilterBitsReader(
      const Slice& contents) const override {
    return new TestFilterBitsReader(contents);
  }
};

class PluginFullFilterBlockTest : public testing::Test {
 public:
  BlockBasedTableOptions table_options_;

  PluginFullFilterBlockTest() {
    table_options_.filter_policy.reset(new TestHashFilter());
  }
};

TEST_F(PluginFullFilterBlockTest, PluginEmptyBuilder) {
  FullFilterBlockBuilder builder(true, table_options_.filter_policy->GetFilterBitsBuilder());
  Slice block = builder.FilterBlockBuilder::Finish();
  ASSERT_EQ("", EscapeString(block));

  FullFilterBlockReader reader(true,
                               block,
                               table_options_.filter_policy->GetFilterBitsReader(block),
                               nullptr);
  // Remain same symantic with blockbased filter
  ASSERT_TRUE(reader.KeyMayMatch("foo"));
}

TEST_F(PluginFullFilterBlockTest, PluginSingleChunk) {
  FullFilterBlockBuilder builder(true, table_options_.filter_policy->GetFilterBitsBuilder());
  builder.Add("foo");
  builder.Add("bar");
  builder.Add("box");
  builder.Add("box");
  builder.Add("hello");
  Slice block = builder.FilterBlockBuilder::Finish();
  FullFilterBlockReader reader(true,
                               block,
                               table_options_.filter_policy->GetFilterBitsReader(block),
                               nullptr);
  ASSERT_TRUE(reader.KeyMayMatch("foo"));
  ASSERT_TRUE(reader.KeyMayMatch("bar"));
  ASSERT_TRUE(reader.KeyMayMatch("box"));
  ASSERT_TRUE(reader.KeyMayMatch("hello"));
  ASSERT_TRUE(reader.KeyMayMatch("foo"));
  ASSERT_TRUE(!reader.KeyMayMatch("missing"));
  ASSERT_TRUE(!reader.KeyMayMatch("other"));
}

class FullFilterBlockTest : public testing::Test {
 public:
  BlockBasedTableOptions table_options_;

  FullFilterBlockTest() {
    table_options_.filter_policy.reset(NewBloomFilterPolicy(10, false));
  }

  ~FullFilterBlockTest() {}
};

TEST_F(FullFilterBlockTest, EmptyBuilder) {
  FullFilterBlockBuilder builder(true, table_options_.filter_policy->GetFilterBitsBuilder());
  Slice block = builder.FilterBlockBuilder::Finish();
  ASSERT_EQ("", EscapeString(block));

  FullFilterBlockReader reader(true,
                               block,
                               table_options_.filter_policy->GetFilterBitsReader(block),
                               nullptr);
  // Remain same symantic with blockbased filter
  ASSERT_TRUE(reader.KeyMayMatch("foo"));
}

TEST_F(FullFilterBlockTest, SingleChunk) {
  FullFilterBlockBuilder builder(true, table_options_.filter_policy->GetFilterBitsBuilder());
  builder.Add("foo");
  builder.Add("bar");
  builder.Add("box");
  builder.Add("box");
  builder.Add("hello");
  Slice block = builder.FilterBlockBuilder::Finish();
  FullFilterBlockReader reader(true,
                               block,
                               table_options_.filter_policy->GetFilterBitsReader(block),
                               nullptr);
  ASSERT_TRUE(reader.KeyMayMatch("foo"));
  ASSERT_TRUE(reader.KeyMayMatch("bar"));
  ASSERT_TRUE(reader.KeyMayMatch("box"));
  ASSERT_TRUE(reader.KeyMayMatch("hello"));
  ASSERT_TRUE(reader.KeyMayMatch("foo"));
  ASSERT_TRUE(!reader.KeyMayMatch("missing"));
  ASSERT_TRUE(!reader.KeyMayMatch("other"));
}

TEST_F(FullFilterBlockTest, FixedSizeTest) {
  FullFilterBlockBuilder builder(true, table_options_.filter_policy->GetFixedBitsBuilder(5));
  builder.Add("foo");
  builder.Add("bar");
  builder.Add("box");
  builder.Add("box");
  builder.Add("hello");
  Slice block = builder.FilterBlockBuilder::Finish();
  FullFilterBlockReader reader(true,
                               block,
                               table_options_.filter_policy->GetFilterBitsReader(block),
                               nullptr);
  ASSERT_TRUE(reader.KeyMayMatch("foo"));
  ASSERT_TRUE(reader.KeyMayMatch("bar"));
  ASSERT_TRUE(reader.KeyMayMatch("box"));
  ASSERT_TRUE(reader.KeyMayMatch("hello"));
  ASSERT_TRUE(reader.KeyMayMatch("foo"));
  ASSERT_TRUE(!reader.KeyMayMatch("missing"));
  ASSERT_TRUE(!reader.KeyMayMatch("other"));
}

TEST_F(FullFilterBlockTest, FixedSizeWrongTest) {
  FullFilterBlockBuilder builder(true, table_options_.filter_policy->GetFixedBitsBuilder(4));
  builder.Add("foo");
  builder.Add("bar");
  builder.Add("box");
  builder.Add("box");
  builder.Add("hello");
  Slice block = builder.FilterBlockBuilder::Finish();
  FullFilterBlockReader reader(true,
                               block,
                               table_options_.filter_policy->GetFilterBitsReader(block),
                               nullptr);
  ASSERT_TRUE(reader.KeyMayMatch("foo"));
  ASSERT_TRUE(reader.KeyMayMatch("bar"));
  ASSERT_TRUE(reader.KeyMayMatch("box"));
  ASSERT_TRUE(reader.KeyMayMatch("hello"));
  ASSERT_TRUE(reader.KeyMayMatch("foo"));
  ASSERT_TRUE(!reader.KeyMayMatch("missing"));
  ASSERT_TRUE(!reader.KeyMayMatch("other"));
}
}  // namespace table
}  // namespace smartengine

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
	smartengine::util::test::init_logger(__FILE__);
  return RUN_ALL_TESTS();
}
