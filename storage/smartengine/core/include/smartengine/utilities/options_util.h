/*
 * Portions Copyright (c) 2023, ApeCloud Inc Holding Limited
 * Portions Copyright (c) 2020, Alibaba Group Holding Limited
 */
// Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
// This source code is licensed under the BSD-style license found in the
// LICENSE file in the root directory of this source tree. An additional grant
// of patent rights can be found in the PATENTS file in the same directory.

// This file contains utility functions for RocksDB Options.
#pragma once

#ifndef ROCKSDB_LITE

#include <string>
#include <vector>

#include "smartengine/db.h"
#include "smartengine/env.h"
#include "smartengine/options.h"
#include "smartengine/status.h"

namespace smartengine {
namespace common {
// Constructs the DBOptions and db::ColumnFamilyDescriptors by loading the
// latest RocksDB options file stored in the specified rocksdb database.
//
// Note that the all the pointer options (except table_factory, which will
// be described in more details below) will be initialized with the default
// values.  Developers can further initialize them after this function call.
// Below is an example list of pointer options which will be initialized
//
// * env
// * memtable_factory
// * compaction_filter_factory
// * prefix_extractor
// * comparator
// * merge_operator
// * compaction_filter
//
// For table_factory, this function further supports deserializing
// BlockBasedTableFactory and its BlockBasedTableOptions except the
// pointer options of BlockBasedTableOptions (flush_block_policy_factory,
// block_cache, and block_cache_compressed), which will be initialized with
// default values.  Developers can further specify these three options by
// casting the return value of TableFactoroy::GetOptions() to
// BlockBasedTableOptions and making necessary changes.
//
// examples/options_file_example.cc demonstrates how to use this function
// to open a RocksDB instance.
//
// @return the function returns an OK status when it went successfully.  If
//     the specified "dbpath" does not contain any option file, then a
//     Status::NotFound will be returned.  A return value other than
//     Status::OK or Status::NotFound indicates there're some error related
//     to the options file itself.
//
// @see LoadOptionsFromFile
Status LoadLatestOptions(const std::string& dbpath, util::Env* env,
                         DBOptions* db_options,
                         std::vector<db::ColumnFamilyDescriptor>* cf_descs);

// Similar to LoadLatestOptions, this function constructs the DBOptions
// and db::ColumnFamilyDescriptors based on the specified RocksDB Options file.
//
// @see LoadLatestOptions
Status LoadOptionsFromFile(const std::string& options_file_name, util::Env* env,
                           DBOptions* db_options,
                           std::vector<db::ColumnFamilyDescriptor>* cf_descs);

// Returns the latest options file name under the specified db path.
Status GetLatestOptionsFileName(const std::string& dbpath, util::Env* env,
                                std::string* options_file_name);

// Returns Status::OK if the input DBOptions and db::ColumnFamilyDescriptors
// are compatible with the latest options stored in the specified DB path.
//
// If the return status is non-ok, it means the specified RocksDB instance
// might not be correctly opened with the input set of options.  Currently,
// changing one of the following options will fail the compatibility check:
//
// * comparator
// * prefix_extractor
// * table_factory
// * merge_operator
Status CheckOptionsCompatibility(
    const std::string& dbpath, util::Env* env, const DBOptions& db_options,
    const std::vector<db::ColumnFamilyDescriptor>& cf_descs);

}  // namespace common
}  // namespace smartengine
#endif  // !ROCKSDB_LITE
