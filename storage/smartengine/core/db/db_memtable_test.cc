//  Portions Copyright (c) 2023, ApeCloud Inc Holding Limited
// Portions Copyright (c) 2020, Alibaba Group Holding Limited
//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.

#include <memory>
#include <string>

#include "db/db_test_util.h"
#include "db/memtable.h"
#include "port/stack_trace.h"
#include "smartengine/memtablerep.h"
#include "smartengine/slice_transform.h"

using namespace smartengine;
using namespace common;
using namespace util;
using namespace table;
using namespace cache;
using namespace memtable;
using namespace monitor;
using namespace storage;
namespace smartengine {
namespace db {

class DBMemTableTest : public DBTestBase {
 public:
  DBMemTableTest() : DBTestBase("/db_memtable_test") {}
};

class MockMemTableRep : public MemTableRep {
 public:
  explicit MockMemTableRep(MemTableAllocator* allocator, MemTableRep* rep)
      : MemTableRep(allocator), rep_(rep), num_insert_with_hint_(0) {}

  virtual KeyHandle Allocate(const size_t len, char** buf) override {
    return rep_->Allocate(len, buf);
  }

  virtual void Insert(KeyHandle handle) override {
    return rep_->Insert(handle);
  }

  virtual void InsertWithHint(KeyHandle handle, void** hint) override {
    num_insert_with_hint_++;
    ASSERT_NE(nullptr, hint);
    last_hint_in_ = *hint;
    rep_->InsertWithHint(handle, hint);
    last_hint_out_ = *hint;
  }

  virtual bool Contains(const char* key) const override {
    return rep_->Contains(key);
  }

  virtual void Get(const LookupKey& k, void* callback_args,
                   bool (*callback_func)(void* arg,
                                         const char* entry)) override {
    rep_->Get(k, callback_args, callback_func);
  }

  virtual size_t ApproximateMemoryUsage() override {
    return rep_->ApproximateMemoryUsage();
  }

  virtual Iterator* GetIterator(Arena* arena) override {
    return rep_->GetIterator(arena);
  }

  void* last_hint_in() { return last_hint_in_; }
  void* last_hint_out() { return last_hint_out_; }
  int num_insert_with_hint() { return num_insert_with_hint_; }

 private:
  std::unique_ptr<MemTableRep> rep_;
  void* last_hint_in_;
  void* last_hint_out_;
  int num_insert_with_hint_;
};

class MockMemTableRepFactory : public MemTableRepFactory {
 public:
  virtual MemTableRep* CreateMemTableRep(const MemTableRep::KeyComparator& cmp,
                                         MemTableAllocator* allocator,
                                         const SliceTransform* transform) override {
    SkipListFactory factory;
    MemTableRep* skiplist_rep =
        factory.CreateMemTableRep(cmp, allocator, transform);
    mock_rep_ = new MockMemTableRep(allocator, skiplist_rep);
    return mock_rep_;
  }

  virtual const char* Name() const override { return "MockMemTableRepFactory"; }

  MockMemTableRep* rep() { return mock_rep_; }

  bool IsInsertConcurrentlySupported() const override { return false; }

 private:
  MockMemTableRep* mock_rep_;
};

class TestPrefixExtractor : public SliceTransform {
 public:
  virtual const char* Name() const override { return "TestPrefixExtractor"; }

  virtual Slice Transform(const Slice& key) const override {
    const char* p = separator(key);
    if (p == nullptr) {
      return Slice();
    }
    return Slice(key.data(), p - key.data() + 1);
  }

  virtual bool InDomain(const Slice& key) const override {
    return separator(key) != nullptr;
  }

  virtual bool InRange(const Slice& key) const override { return false; }

 private:
  const char* separator(const Slice& key) const {
    return reinterpret_cast<const char*>(memchr(key.data(), '_', key.size()));
  }
};

TEST_F(DBMemTableTest, InsertWithHint) {
  Options options;
  options.allow_concurrent_memtable_write = false;
  options.create_if_missing = true;
  options.memtable_factory.reset(new MockMemTableRepFactory());
  options.memtable_insert_with_hint_prefix_extractor.reset(
      new TestPrefixExtractor());
  options.env = env_;
  Reopen(options);
  MockMemTableRep* rep =
      reinterpret_cast<MockMemTableRepFactory*>(options.memtable_factory.get())
          ->rep();
  ASSERT_OK(Put("foo_k1", "foo_v1"));
  ASSERT_EQ(nullptr, rep->last_hint_in());
  void* hint_foo = rep->last_hint_out();
  ASSERT_OK(Put("foo_k2", "foo_v2"));
  ASSERT_EQ(hint_foo, rep->last_hint_in());
  ASSERT_EQ(hint_foo, rep->last_hint_out());
  ASSERT_OK(Put("foo_k3", "foo_v3"));
  ASSERT_EQ(hint_foo, rep->last_hint_in());
  ASSERT_EQ(hint_foo, rep->last_hint_out());
  ASSERT_OK(Put("bar_k1", "bar_v1"));
  ASSERT_EQ(nullptr, rep->last_hint_in());
  void* hint_bar = rep->last_hint_out();
  ASSERT_NE(hint_foo, hint_bar);
  ASSERT_OK(Put("bar_k2", "bar_v2"));
  ASSERT_EQ(hint_bar, rep->last_hint_in());
  ASSERT_EQ(hint_bar, rep->last_hint_out());
  ASSERT_EQ(5, rep->num_insert_with_hint());
  ASSERT_OK(Put("whitelisted", "vvv"));
  ASSERT_EQ(5, rep->num_insert_with_hint());
  ASSERT_EQ("foo_v1", Get("foo_k1"));
  ASSERT_EQ("foo_v2", Get("foo_k2"));
  ASSERT_EQ("foo_v3", Get("foo_k3"));
  ASSERT_EQ("bar_v1", Get("bar_k1"));
  ASSERT_EQ("bar_v2", Get("bar_k2"));
  ASSERT_EQ("vvv", Get("whitelisted"));
}
}
}  // namespace smartengine

int main(int argc, char** argv) {
  port::InstallStackTraceHandler();
  ::testing::InitGoogleTest(&argc, argv);
	smartengine::util::test::init_logger(__FILE__);
  return RUN_ALL_TESTS();
}
