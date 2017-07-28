//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
#include "ttl_extractor.h"

#include "util/coding.h"

namespace rocksdb {
namespace blob_db {

bool TTLExtractor::ExtractTTL(const Slice& /*key*/, const Slice& /*value*/,
                              uint64_t* /*ttl*/, std::string* /*new_value*/,
                              bool* /*value_changed*/) {
  return false;
}

bool TTLExtractor::ExtractExpiration(const Slice& key, const Slice& value,
                                     uint64_t now, uint64_t* expiration,
                                     std::string* new_value,
                                     bool* value_changed) {
  uint64_t ttl;
  bool has_ttl = ExtractTTL(key, value, &ttl, new_value, value_changed);
  if (has_ttl) {
    *expiration = now + ttl;
  }
  return has_ttl;
}

namespace {
class DefaultTTLExtractor : public TTLExtractor {
 public:
  const Slice kTTLSuffix = Slice("ttl:");

  bool ExtractTTL(const Slice& /*key*/, const Slice& value, uint64_t* ttl,
                  std::string* new_value, bool* value_changed) override {
    if (value.size() < 12) {
      return false;
    }
    const char* p = value.data() + value.size() - 12;
    if (kTTLSuffix != Slice(p, 4)) {
      return false;
    }
    *ttl = DecodeFixed64(p + 4);
    *new_value = Slice(value.data(), value.size() - 12).ToString();
    *value_changed = true;
    return true;
  }
};
}  // anonymous namespace

std::shared_ptr<TTLExtractor> NewDefaultTTLExtractor() {
  return std::make_shared<DefaultTTLExtractor>();
}

}  // namespace blob_db
}  // namespace rocksdb
