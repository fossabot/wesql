/*
 * Portions Copyright (c) 2023, ApeCloud Inc Holding Limited
 * Portions Copyright (c) 2020, Alibaba Group Holding Limited
 */
// Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
// This source code is licensed under the BSD-style license found in the
// LICENSE file in the root directory of this source tree. An additional grant
// of patent rights can be found in the PATENTS file in the same directory.

#ifndef ROCKSDB_LITE

#include "options/options_sanity_check.h"

namespace smartengine {
namespace common {

namespace {
OptionsSanityCheckLevel SanityCheckLevelHelper(
    const std::unordered_map<std::string, OptionsSanityCheckLevel>& smap,
    const std::string& name) {
  auto iter = smap.find(name);
  return iter != smap.end() ? iter->second : kSanityLevelExactMatch;
}
}

OptionsSanityCheckLevel DBOptionSanityCheckLevel(
    const std::string& option_name) {
  return SanityCheckLevelHelper(sanity_level_db_options, option_name);
}

OptionsSanityCheckLevel CFOptionSanityCheckLevel(
    const std::string& option_name) {
  return SanityCheckLevelHelper(sanity_level_cf_options, option_name);
}

OptionsSanityCheckLevel BBTOptionSanityCheckLevel(
    const std::string& option_name) {
  return SanityCheckLevelHelper(sanity_level_bbt_options, option_name);
}

}  // namespace common
}  // namespace smartengine

#endif  // !ROCKSDB_LITE
