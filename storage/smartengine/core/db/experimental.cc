//  Portions Copyright (c) 2023, ApeCloud Inc Holding Limited
// Portions Copyright (c) 2020, Alibaba Group Holding Limited
//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.

#include "smartengine/experimental.h"

#include "db/db_impl.h"

using namespace smartengine;
using namespace common;

namespace smartengine {
namespace db {
namespace experimental {

#ifndef ROCKSDB_LITE

Status SuggestCompactRange(DB* db, ColumnFamilyHandle* column_family,
                           const Slice* begin, const Slice* end) {
  auto dbimpl = dynamic_cast<DBImpl*>(db);
  if (dbimpl == nullptr) {
    return Status::InvalidArgument("Didn't recognize DB object");
  }

  return dbimpl->SuggestCompactRange(column_family, begin, end);
}

Status PromoteL0(DB* db, ColumnFamilyHandle* column_family, int target_level) {
  auto dbimpl = dynamic_cast<DBImpl*>(db);
  if (dbimpl == nullptr) {
    return Status::InvalidArgument("Didn't recognize DB object");
  }
  return dbimpl->PromoteL0(column_family, target_level);
}

#else  // ROCKSDB_LITE

Status SuggestCompactRange(DB* db, ColumnFamilyHandle* column_family,
                           const Slice* begin, const Slice* end) {
  return Status::NotSupported("Not supported in RocksDB LITE");
}

Status PromoteL0(DB* db, ColumnFamilyHandle* column_family, int target_level) {
  return Status::NotSupported("Not supported in RocksDB LITE");
}

#endif  // ROCKSDB_LITE

Status SuggestCompactRange(DB* db, const Slice* begin, const Slice* end) {
  return SuggestCompactRange(db, db->DefaultColumnFamily(), begin, end);
}

}  // namespace experimental
}  // namespace db
}  // namespace smartengine
