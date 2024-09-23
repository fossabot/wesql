//  Portions Copyright (c) 2023, ApeCloud Inc Holding Limited
// Portions Copyright (c) 2020, Alibaba Group Holding Limited
//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.

#include "db/flush_scheduler.h"

#include <cassert>

#include "db/column_family.h"

namespace smartengine {
namespace db {

void FlushScheduler::ScheduleFlush(ColumnFamilyData* cfd) {
#ifndef NDEBUG
  std::lock_guard<std::mutex> lock(checking_mutex_);
  assert(checking_set_.count(cfd) == 0);
  checking_set_.insert(cfd);
#endif  // NDEBUG
  cfd->Ref();
// Suppress false positive clang analyzer warnings.
#ifndef __clang_analyzer__
  Node* node = new Node{cfd, head_.load(std::memory_order_relaxed)};
  while (!head_.compare_exchange_strong(
      node->next, node, std::memory_order_relaxed, std::memory_order_relaxed)) {
    // failing CAS updates the first param, so we are already set for
    // retry.  TakeNextColumnFamily won't happen until after another
    // inter-thread synchronization, so we don't even need release
    // semantics for this CAS
  }
  size_.fetch_add(1);
#endif  // __clang_analyzer__
}

ColumnFamilyData* FlushScheduler::TakeNextColumnFamily() {
  while (true) {
    if (Empty()) {
      return nullptr;
    }

#ifndef NDEBUG
    std::lock_guard<std::mutex> lock(checking_mutex_);
#endif  // NDEBUG

    // dequeue the head
    Node* node = head_.load(std::memory_order_relaxed);
    head_.store(node->next, std::memory_order_relaxed);
    ColumnFamilyData* cfd = node->column_family;
    delete node;
    size_.fetch_sub(1);

#ifndef NDEBUG
    {
      auto iter = checking_set_.find(cfd);
      assert(iter != checking_set_.end());
      checking_set_.erase(iter);
    }
#endif  // NDEBUG

    if (!cfd->IsDropped()) {
      // success
      return cfd;
    }

    // no longer relevant, retry
    if (cfd->Unref()) {
      MOD_DELETE_OBJECT(ColumnFamilyData, cfd);
    }
  }
}

bool FlushScheduler::Empty() {
  bool rv;
#ifndef NDEBUG
  {
    std::lock_guard<std::mutex> lock(checking_mutex_);
    rv = head_.load(std::memory_order_relaxed) == nullptr;
    assert(rv == checking_set_.empty());
  }
#else
  rv = head_.load(std::memory_order_relaxed) == nullptr;
#endif  // NDEBUG
  return rv;
}

void FlushScheduler::Clear() {
  ColumnFamilyData* cfd;
  while ((cfd = TakeNextColumnFamily()) != nullptr) {
    if (cfd->Unref()) {
      MOD_DELETE_OBJECT(ColumnFamilyData, cfd);
    }
  }
  assert(Empty());
}
}
}  // namespace smartengine
