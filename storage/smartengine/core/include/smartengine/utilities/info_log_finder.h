/*
 * Portions Copyright (c) 2023, ApeCloud Inc Holding Limited
 * Portions Copyright (c) 2020, Alibaba Group Holding Limited
 */
// Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
// This source code is licensed under the BSD-style license found in the
// LICENSE file in the root directory of this source tree. An additional grant
// of patent rights can be found in the PATENTS file in the same directory.

#pragma once

#include <string>
#include <vector>

#include "smartengine/db.h"
#include "smartengine/options.h"

namespace smartengine {
namespace common {

// This function can be used to list the Information logs,
// given the db pointer.
Status GetInfoLogList(db::DB* db, std::vector<std::string>* info_log_list);
}  // namespace common
}  // namespace smartengine
