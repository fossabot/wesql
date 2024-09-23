//  Portions Copyright (c) 2023, ApeCloud Inc Holding Limited
//  Portions Copyright (c) 2020, Alibaba Group Holding Limited
//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
#pragma once
#if !defined(IOS_CROSS_COMPILE)
// if we compile with Xcode, we don't run build_detect_vesion, so we don't
// generate these variables
// this variable tells us about the git revision
extern const char* smartengine_build_git_sha;

// Date on which the code was compiled:
extern const char* smartengine_build_compile_date;
#endif
