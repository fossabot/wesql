/*
 * Portions Copyright (c) 2023, ApeCloud Inc Holding Limited
 * Copyright (c) 2020, Alibaba Group Holding Limited
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "logger/log_module.h"
#include <gtest/gtest.h>

using logger::Logger;
int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  //::testing::InitGoogleMock(&argc, argv);
  /*
  const char *log_file_name = "ut.log";
  int ret = Logger::get_log().init(log_file_name);
  assert(0 == ret);
  Logger::get_log().set_log_level(logger::DEBUG_LEVEL);
  */

  return RUN_ALL_TESTS();
}
