//  Portions Copyright (c) 2023, ApeCloud Inc Holding Limited
//  Portions Copyright (c) 2020, Alibaba Group Holding Limited
//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
#ifndef ROCKSDB_LITE

#include "smartengine/utilities/ldb_cmd.h"
#include "util/testharness.h"

using std::string;
using std::vector;
using std::map;

using namespace smartengine;
using namespace tools;

class LdbCmdTest : public testing::Test {};

TEST_F(LdbCmdTest, HexToString) {
  // map input to expected outputs.
  // odd number of "hex" half bytes doesn't make sense
  map<string, vector<int>> inputMap = {
      {"0x07", {7}},        {"0x5050", {80, 80}},          {"0xFF", {-1}},
      {"0x1234", {18, 52}}, {"0xaaAbAC", {-86, -85, -84}}, {"0x1203", {18, 3}},
  };

  for (const auto& inPair : inputMap) {
    auto actual = LDBCommand::HexToString(inPair.first);
    auto expected = inPair.second;
    for (unsigned int i = 0; i < actual.length(); i++) {
      EXPECT_EQ(expected[i], static_cast<int>((signed char)actual[i]));
    }
    auto reverse = LDBCommand::StringToHex(actual);
    EXPECT_STRCASEEQ(inPair.first.c_str(), reverse.c_str());
  }
}

TEST_F(LdbCmdTest, HexToStringBadInputs) {
  const vector<string> badInputs = {
      "0xZZ", "123", "0xx5", "0x111G", "0x123", "Ox12", "0xT", "0x1Q1",
  };
  for (const auto badInput : badInputs) {
    try {
      LDBCommand::HexToString(badInput);
      std::cerr << "Should fail on bad hex value: " << badInput << "\n";
      FAIL();
    } catch (...) {
    }
  }
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
	smartengine::util::test::init_logger(__FILE__);
  return RUN_ALL_TESTS();
}
#else
#include <stdio.h>

int main(int argc, char** argv) {
  fprintf(stderr, "SKIPPED as LDBCommand is not supported in ROCKSDB_LITE\n");
  return 0;
}

#endif  // ROCKSDB_LITE
