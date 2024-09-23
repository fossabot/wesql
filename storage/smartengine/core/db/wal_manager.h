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
#pragma once

#include <memory>
#include <string>

#include "db/version_set.h"
#include "env/env.h"
#include "options/db_options.h"
#include "transactions/transaction_log.h"

namespace smartengine
{
namespace db
{

class WalManager {
 public:
  WalManager(const common::ImmutableDBOptions& db_options,
             const util::EnvOptions& env_options)
      : db_options_(db_options),
        env_options_(env_options),
        env_(db_options.env),
        purge_wal_files_last_run_(0) {}

  common::Status GetSortedWalFiles(VectorLogPtr& files);

  common::Status GetUpdatesSince(
      common::SequenceNumber seq_number,
      std::unique_ptr<TransactionLogIterator>* iter,
      const db::TransactionLogIterator::ReadOptions& read_options,
      VersionSet* version_set);

  void PurgeObsoleteWALFiles();

  void ArchiveWALFile(const std::string& fname, uint64_t number);

  common::Status TEST_ReadFirstRecord(const WalFileType type,
                                      const uint64_t number,
                                      common::SequenceNumber* sequence) {
    return ReadFirstRecord(type, number, sequence);
  }

  common::Status TEST_ReadFirstLine(const std::string& fname,
                                    const uint64_t number,
                                    common::SequenceNumber* sequence) {
    return ReadFirstLine(fname, number, sequence);
  }

 private:
  common::Status GetSortedWalsOfType(const std::string& path,
                                     VectorLogPtr& log_files, WalFileType type);
  // Requires: all_logs should be sorted with earliest log file first
  // Retains all log files in all_logs which contain updates with seq no.
  // Greater Than or Equal to the requested SequenceNumber.
  common::Status RetainProbableWalFiles(VectorLogPtr& all_logs,
                                        const common::SequenceNumber target);

  common::Status ReadFirstRecord(const WalFileType type, const uint64_t number,
                                 common::SequenceNumber* sequence);

  common::Status ReadFirstLine(const std::string& fname, const uint64_t number,
                               common::SequenceNumber* sequence);

  // ------- state from DBImpl ------
  const common::ImmutableDBOptions& db_options_;
  const util::EnvOptions& env_options_;
  util::Env* env_;

  // ------- WalManager state -------
  // cache for ReadFirstRecord() calls
  std::unordered_map<uint64_t, common::SequenceNumber> read_first_record_cache_;
  port::Mutex read_first_record_cache_mutex_;

  // last time when PurgeObsoleteWALFiles ran.
  uint64_t purge_wal_files_last_run_;

  // obsolete files will be deleted every this seconds if ttl deletion is
  // enabled and archive size_limit is disabled.
  static const uint64_t kDefaultIntervalToDeleteObsoleteWAL = 600;
};

} //namespace db
} //namespace smartengine
