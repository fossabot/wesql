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

#include "util/coding.h"

namespace smartengine {
namespace util {

// This function may intentionally do a left shift on a -ve number
//#if __clang_major__ > 3 || (__clang_major__ == 3 && __clang_minor__ >= 9)
//__attribute__((__no_sanitize__("undefined")))
//#elif __GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 9)
//__attribute__((__no_sanitize_undefined__))
//#endif
uint32_t
Hash(const char* data, size_t n, uint32_t seed) {
  // Similar to murmur hash
  const uint32_t m = 0xc6a4a793;
  const uint32_t r = 24;
  const char* limit = data + n;
  uint32_t h = static_cast<uint32_t>(seed ^ (n * m));

  // Pick up four bytes at a time
  while (data + 4 <= limit) {
    uint32_t w = DecodeFixed32(data);
    data += 4;
    h += w;
    h *= m;
    h ^= (h >> 16);
  }

  // Pick up remaining bytes
  switch (limit - data) {
    // Note: It would be better if this was cast to unsigned char, but that
    // would be a disk format change since we previously didn't have any cast
    // at all (so gcc used signed char).
    // To understand the difference between shifting unsigned and signed chars,
    // let's use 250 as an example. unsigned char will be 250, while signed char
    // will be -6. Bit-wise, they are equivalent: 11111010. However, when
    // converting negative number (signed char) to int, it will be converted
    // into negative int (of equivalent value, which is -6), while converting
    // positive number (unsigned char) will be converted to 250. Bitwise,
    // this looks like this:
    // signed char 11111010 -> int 11111111111111111111111111111010
    // unsigned char 11111010 -> int 00000000000000000000000011111010
    case 3:
      h += static_cast<uint32_t>(static_cast<signed char>(data[2]) << 16);
      [[fallthrough]];
    // fall through
    case 2:
      h += static_cast<uint32_t>(static_cast<signed char>(data[1]) << 8);
      [[fallthrough]];
    // fall through
    case 1:
      h += static_cast<uint32_t>(static_cast<signed char>(data[0]));
      h *= m;
      h ^= (h >> r);
      break;
  }
  return h;
}

}  // namespace util
}  // namespace smartengine
