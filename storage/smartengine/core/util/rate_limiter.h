//  Portions Copyright (c) 2023, ApeCloud Inc Holding Limited
//  Portions Copyright (c) 2020, Alibaba Group Holding Limited
//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#pragma once

#include <atomic>
#include <deque>
#include "env/env.h"
#include "monitoring/statistics.h"
#include "util/mutexlock.h"
#include "util/random.h"

namespace smartengine {
namespace util {

class RateLimiter {
 public:
  virtual ~RateLimiter() {}

  // This API allows user to dynamically change rate limiter's bytes per second.
  // REQUIRED: bytes_per_second > 0
  virtual void SetBytesPerSecond(int64_t bytes_per_second) = 0;

  // Request for token to write bytes. If this request can not be satisfied,
  // the call is blocked. Caller is responsible to make sure
  // bytes <= GetSingleBurstBytes()
  virtual void Request(const int64_t bytes, const Env::IOPriority pri) {
    // Deprecated. New RateLimiter derived classes should override
    // Request(const int64_t, const Env::IOPriority, Statistics*) instead.
    assert(false);
  }

  // Request for token to write bytes and potentially update statistics. If this
  // request can not be satisfied, the call is blocked. Caller is responsible to
  // make sure bytes <= GetSingleBurstBytes().
  virtual void Request(const int64_t bytes, const Env::IOPriority pri,
                       monitor::Statistics* /* stats */) {
    // For API compatibility, default implementation calls the older API in
    // which statistics are unsupported.
    Request(bytes, pri);
  }

  // Max bytes can be granted in a single burst
  virtual int64_t GetSingleBurstBytes() const = 0;

  // Total bytes that go though rate limiter
  virtual int64_t GetTotalBytesThrough(
      const Env::IOPriority pri = Env::IO_TOTAL) const = 0;

  // Total # of requests that go though rate limiter
  virtual int64_t GetTotalRequests(
      const Env::IOPriority pri = Env::IO_TOTAL) const = 0;
};

class GenericRateLimiter : public RateLimiter {
 public:
  GenericRateLimiter(int64_t refill_bytes, int64_t refill_period_us,
                     int32_t fairness);

  virtual ~GenericRateLimiter();

  // This API allows user to dynamically change rate limiter's bytes per second.
  virtual void SetBytesPerSecond(int64_t bytes_per_second) override;

  // Request for token to write bytes. If this request can not be satisfied,
  // the call is blocked. Caller is responsible to make sure
  // bytes <= GetSingleBurstBytes()
  using RateLimiter::Request;
  virtual void Request(const int64_t bytes, const Env::IOPriority pri,
                       monitor::Statistics* stats) override;

  virtual int64_t GetSingleBurstBytes() const override {
    return refill_bytes_per_period_.load(std::memory_order_relaxed);
  }

  virtual int64_t GetTotalBytesThrough(
      const Env::IOPriority pri = Env::IO_TOTAL) const override {
    MutexLock g(&request_mutex_);
    if (pri == Env::IO_TOTAL) {
      return total_bytes_through_[Env::IO_LOW] +
             total_bytes_through_[Env::IO_HIGH];
    }
    return total_bytes_through_[pri];
  }

  virtual int64_t GetTotalRequests(
      const Env::IOPriority pri = Env::IO_TOTAL) const override {
    MutexLock g(&request_mutex_);
    if (pri == Env::IO_TOTAL) {
      return total_requests_[Env::IO_LOW] + total_requests_[Env::IO_HIGH];
    }
    return total_requests_[pri];
  }

 private:
  void Refill();
  int64_t CalculateRefillBytesPerPeriod(int64_t rate_bytes_per_sec);
  uint64_t NowMicrosMonotonic(Env* env) {
    return env->NowNanos() / std::milli::den;
  }

  // This mutex guard all internal states
  mutable port::Mutex request_mutex_;

  const int64_t kMinRefillBytesPerPeriod = 100;

  const int64_t refill_period_us_;
  // This variable can be changed dynamically.
  std::atomic<int64_t> refill_bytes_per_period_;
  Env* const env_;

  bool stop_;
  port::CondVar exit_cv_;
  int32_t requests_to_wait_;

  int64_t total_requests_[Env::IO_TOTAL];
  int64_t total_bytes_through_[Env::IO_TOTAL];
  int64_t available_bytes_;
  int64_t next_refill_us_;

  int32_t fairness_;
  Random rnd_;

  struct Req;
  Req* leader_;
  std::deque<Req*> queue_[Env::IO_TOTAL];
};

// Create a RateLimiter object, which can be shared among RocksDB instances to
// control write rate of flush and compaction.
// @rate_bytes_per_sec: this is the only parameter you want to set most of the
// time. It controls the total write rate of compaction and flush in bytes per
// second. Currently, RocksDB does not enforce rate limit for anything other
// than flush and compaction, e.g. write to WAL.
// @refill_period_us: this controls how often tokens are refilled. For example,
// when rate_bytes_per_sec is set to 10MB/s and refill_period_us is set to
// 100ms, then 1MB is refilled every 100ms internally. Larger value can lead to
// burstier writes while smaller value introduces more CPU overhead.
// The default should work for most cases.
// @fairness: RateLimiter accepts high-pri requests and low-pri requests.
// A low-pri request is usually blocked in favor of hi-pri request. Currently,
// RocksDB assigns low-pri to request from compaciton and high-pri to request
// from flush. Low-pri requests can get blocked if flush requests come in
// continuouly. This fairness parameter grants low-pri requests permission by
// 1/fairness chance even though high-pri requests exist to avoid starvation.
// You should be good by leaving it at default 10.
extern RateLimiter* NewGenericRateLimiter(int64_t rate_bytes_per_sec,
                                          int64_t refill_period_us = 100 * 1000,
                                          int32_t fairness = 10);

}  // namespace util
}  // namespace smartengine
