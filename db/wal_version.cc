// Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
// This source code is licensed under both the GPLv2 (found in the
// COPYING file in the root directory) and Apache 2.0 License
// (found in the LICENSE.Apache file in the root directory).

#include "db/wal_version.h"

#include "rocksdb/slice.h"
#include "rocksdb/status.h"
#include "util/coding.h"

namespace ROCKSDB_NAMESPACE {

namespace {

enum class WalAdditionTag : uint32_t {
  // Indicates that there are no more tags.
  kTerminate = 1,
  // Size in bytes.
  kSize = 2,
  // Add tags in the future, such as checksum?
};

}  // anonymous namespace

void WalAddition::EncodeTo(std::string* dst) const {
  PutVarint64(dst, number_);

  if (metadata_.HasSize()) {
    PutVarint32(dst, static_cast<uint32_t>(WalAdditionTag::kSize));
    PutVarint64(dst, metadata_.GetSizeInBytes());
  }

  PutVarint32(dst, static_cast<uint32_t>(WalAdditionTag::kTerminate));
}

Status WalAddition::DecodeFrom(Slice* src) {
  constexpr char class_name[] = "WalAddition";

  if (!GetVarint64(src, &number_)) {
    return Status::Corruption(class_name, "Error decoding WAL log number");
  }

  while (true) {
    uint32_t tag_value = 0;
    if (!GetVarint32(src, &tag_value)) {
      return Status::Corruption(class_name, "Error decoding tag");
    }
    WalAdditionTag tag = static_cast<WalAdditionTag>(tag_value);
    switch (tag) {
      case WalAdditionTag::kSize: {
        uint64_t size = 0;
        if (!GetVarint64(src, &size)) {
          return Status::Corruption(class_name, "Error decoding WAL file size");
        }
        metadata_.SetSizeInBytes(size);
        break;
      }
      // TODO: process future tags such as checksum.
      case WalAdditionTag::kTerminate:
        return Status::OK();
      default: {
        std::stringstream ss;
        ss << "Unknown tag " << tag_value;
        return Status::Corruption(class_name, ss.str());
      }
    }
  }
}

JSONWriter& operator<<(JSONWriter& jw, const WalAddition& wal) {
  jw << "LogNumber" << wal.GetLogNumber() << "SizeInBytes"
     << wal.GetMetadata().GetSizeInBytes();
  return jw;
}

std::ostream& operator<<(std::ostream& os, const WalAddition& wal) {
  os << "log_number: " << wal.GetLogNumber()
     << " size_in_bytes: " << wal.GetMetadata().GetSizeInBytes();
  return os;
}

std::string WalAddition::DebugString() const {
  std::ostringstream oss;
  oss << *this;
  return oss.str();
}

void WalDeletion::EncodeTo(std::string* dst) const {
  PutVarint64(dst, number_);
}

Status WalDeletion::DecodeFrom(Slice* src) {
  constexpr char class_name[] = "WalDeletion";

  if (!GetVarint64(src, &number_)) {
    return Status::Corruption(class_name, "Error decoding WAL log number");
  }

  return Status::OK();
}

JSONWriter& operator<<(JSONWriter& jw, const WalDeletion& wal) {
  jw << "LogNumber" << wal.GetLogNumber();
  return jw;
}

std::ostream& operator<<(std::ostream& os, const WalDeletion& wal) {
  os << "log_number: " << wal.GetLogNumber();
  return os;
}

std::string WalDeletion::DebugString() const {
  std::ostringstream oss;
  oss << *this;
  return oss.str();
}

void WalSet::AddWal(const WalAddition& wal) {
  wals_[wal.GetLogNumber()] = wal.GetMetadata();
}

void WalSet::AddWals(const WalAdditions& wals) {
  for (const WalAddition& wal : wals) {
    AddWal(wal);
  }
}

void WalSet::DeleteWal(const WalDeletion& wal) {
  assert(wals_.find(wal.GetLogNumber()) != wals_.end());
  wals_.erase(wal.GetLogNumber());
}

void WalSet::DeleteWals(const WalDeletions& wals) {
  for (const WalDeletion& wal : wals) {
    DeleteWal(wal);
  }
}

void WalSet::Reset() { wals_.clear(); }

}  // namespace ROCKSDB_NAMESPACE
