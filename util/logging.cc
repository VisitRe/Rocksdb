//  Copyright (c) 2013, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "util/logging.h"

#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif

#include <inttypes.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include "rocksdb/env.h"
#include "rocksdb/slice.h"

namespace rocksdb {


// for sizes >=10TB, print "XXTB"
// for sizes >=10GB, print "XXGB"
// etc.
// append file size summary to output and return the len
int AppendHumanBytes(uint64_t bytes, char* output, int len) {
  const uint64_t ull10 = 10;
  if (bytes >= ull10 << 40) {
    return snprintf(output, len, "%" PRIu64 "TB", bytes >> 40);
  } else if (bytes >= ull10 << 30) {
    return snprintf(output, len, "%" PRIu64 "GB", bytes >> 30);
  } else if (bytes >= ull10 << 20) {
    return snprintf(output, len, "%" PRIu64 "MB", bytes >> 20);
  } else if (bytes >= ull10 << 10) {
    return snprintf(output, len, "%" PRIu64 "KB", bytes >> 10);
  } else {
    return snprintf(output, len, "%" PRIu64 "B", bytes);
  }
}

void AppendNumberTo(std::string* str, uint64_t num) {
  char buf[30];
  snprintf(buf, sizeof(buf), "%" PRIu64, num);
  str->append(buf);
}

void AppendEscapedStringTo(std::string* str, const Slice& value) {
  for (size_t i = 0; i < value.size(); i++) {
    char c = value[i];
    if (c >= ' ' && c <= '~') {
      str->push_back(c);
    } else {
      char buf[10];
      snprintf(buf, sizeof(buf), "\\x%02x",
               static_cast<unsigned int>(c) & 0xff);
      str->append(buf);
    }
  }
}

std::string NumberToString(uint64_t num) {
  std::string r;
  AppendNumberTo(&r, num);
  return r;
}

std::string EscapeString(const Slice& value) {
  std::string r;
  AppendEscapedStringTo(&r, value);
  return r;
}

bool ConsumeDecimalNumber(Slice* in, uint64_t* val) {
  uint64_t v = 0;
  int digits = 0;
  while (!in->empty()) {
    char c = (*in)[0];
    if (c >= '0' && c <= '9') {
      ++digits;
      const unsigned int delta = (c - '0');
      static const uint64_t kMaxUint64 = ~static_cast<uint64_t>(0);
      if (v > kMaxUint64/10 ||
          (v == kMaxUint64/10 && delta > kMaxUint64%10)) {
        // Overflow
        return false;
      }
      v = (v * 10) + delta;
      in->remove_prefix(1);
    } else {
      break;
    }
  }
  *val = v;
  return (digits > 0);
}

}  // namespace rocksdb
