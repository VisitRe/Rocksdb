// Copyright (c) 2011-present, Facebook, Inc. All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include "rocksdb/customizable.h"

#include "rocksdb/convenience.h"
#include "rocksdb/status.h"
#include "util/string_util.h"

namespace ROCKSDB_NAMESPACE {

Status Customizable::PrepareOptions(const ConfigOptions& opts) {
  Status status = Configurable::PrepareOptions(opts);
  if (status.ok()) {
    auto inner = Inner();
    if (inner != nullptr) {
      status = inner->PrepareOptions(opts);
    }
  }
  return status;
}

bool Customizable::IsPrepared() const {
  bool is_prepared = Configurable::IsPrepared();
  if (is_prepared) {
    const auto inner = Inner();
    if (inner != nullptr) {
      is_prepared = inner->IsPrepared();
    }
  }
  return is_prepared;
}

Status Customizable::ValidateOptions(const DBOptions& db_opts,
                                     const ColumnFamilyOptions& cf_opts) const {
  Status status = Configurable::ValidateOptions(db_opts, cf_opts);
  if (status.ok()) {
    const auto inner = Inner();
    if (inner != nullptr) {
      status = inner->ValidateOptions(db_opts, cf_opts);
    }
  }
  return status;
}

const void* Customizable::GetOptionsPtr(const std::string& name) const {
  const void* result = Configurable::GetOptionsPtr(name);
  if (result == nullptr) {
    auto inner = Inner();
    if (inner != nullptr) {
      result = inner->GetOptionsPtr(name);
    }
  }
  return result;
}

std::string Customizable::GetOptionName(const std::string& long_name) const {
  const std::string& name = Name();
  size_t name_len = name.size();
  if (long_name.size() > name_len + 1 &&
      long_name.compare(0, name_len, name) == 0 &&
      long_name.at(name_len) == '.') {
    return long_name.substr(name_len + 1);
  } else {
    return Configurable::GetOptionName(long_name);
  }
}

#ifndef ROCKSDB_LITE
Status Customizable::GetOption(const ConfigOptions& config_options,
                               const std::string& opt_name,
                               std::string* value) const {
  if (opt_name == kIdPropName) {
    *value = GetId();
    return Status::OK();
  } else {
    return Configurable::GetOption(config_options, opt_name, value);
  }
}

std::string Customizable::SerializeOptions(const ConfigOptions& config_options,
                                           const std::string& prefix) const {
  std::string result;
  std::string parent;
  if (!config_options.IsShallow()) {
    parent = Configurable::SerializeOptions(config_options, "");
  }
  if (parent.empty()) {
    result = GetId();
  } else {
    result.append(prefix + kIdPropName + "=" + GetId() +
                  config_options.delimiter);
    result.append(parent);
  }
  return result;
}

#endif  // ROCKSDB_LITE

bool Customizable::AreEquivalent(const ConfigOptions& config_options,
                                 const Configurable* other,
                                 std::string* mismatch) const {
  if (config_options.sanity_level > ConfigOptions::kSanityLevelNone &&
      this != other) {
    const Customizable* custom = reinterpret_cast<const Customizable*>(other);
    if (GetId() != custom->GetId()) {
      *mismatch = kIdPropName;
      return false;
    } else if (config_options.sanity_level >
               ConfigOptions::kSanityLevelLooselyCompatible) {
      bool matches =
          Configurable::AreEquivalent(config_options, other, mismatch);
      return matches;
    }
  }
  return true;
}

}  // namespace ROCKSDB_NAMESPACE
