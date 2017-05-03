//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//  This source code is also licensed under the GPLv2 license found in the
//  COPYING file in the root directory of this source tree.
#pragma once

#include <string>

#include "options/db_options.h"

namespace rocksdb {
void DumpDBFileSummary(const ImmutableDBOptions& options,
                       const std::string& dbname);
}  // namespace rocksdb
