/*
 * Portions Copyright (c) 2023, ApeCloud Inc Holding Limited
 * Portions Copyright (c) 2020, Alibaba Group Holding Limited
 */
// Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
// This source code is licensed under the BSD-style license found in the
// LICENSE file in the root directory of this source tree. An additional grant
// of patent rights can be found in the PATENTS file in the same directory.

#pragma once

#include "smartengine/db.h"
#include "smartengine/status.h"

namespace smartengine {
namespace db {
namespace experimental {

// Supported only for Leveled compaction
common::Status SuggestCompactRange(DB* db, ColumnFamilyHandle* column_family,
                                   const common::Slice* begin,
                                   const common::Slice* end);
common::Status SuggestCompactRange(DB* db, const common::Slice* begin,
                                   const common::Slice* end);

// Move all L0 files to target_level skipping compaction.
// This operation succeeds only if the files in L0 have disjoint ranges; this
// is guaranteed to happen, for instance, if keys are inserted in sorted
// order. Furthermore, all levels between 1 and target_level must be empty.
// If any of the above condition is violated, InvalidArgument will be
// returned.
common::Status PromoteL0(DB* db, ColumnFamilyHandle* column_family,
                         int target_level = 1);

}  // namespace experimental
}  // namespace db
}  // namespace smartengine
