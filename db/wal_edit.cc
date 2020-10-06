// Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
// This source code is licensed under both the GPLv2 (found in the
// COPYING file in the root directory) and Apache 2.0 License
// (found in the LICENSE.Apache file in the root directory).

#include "db/wal_edit.h"

#include "rocksdb/slice.h"
#include "rocksdb/status.h"
#include "util/coding.h"

namespace ROCKSDB_NAMESPACE {

void WalAddition::EncodeTo(std::string* dst) const {
  PutVarint64(dst, number_);

  if (metadata_.HasSyncedSize()) {
    PutVarint32(dst, static_cast<uint32_t>(WalAdditionTag::kSyncedSize));
    PutVarint64(dst, metadata_.GetSyncedSizeInBytes());
  }

  if (metadata_.IsClosed()) {
    PutVarint32(dst, static_cast<uint32_t>(WalAdditionTag::kClosed));
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
      case WalAdditionTag::kSyncedSize: {
        uint64_t size = 0;
        if (!GetVarint64(src, &size)) {
          return Status::Corruption(class_name, "Error decoding WAL file size");
        }
        metadata_.SetSyncedSizeInBytes(size);
        break;
      }
      case WalAdditionTag::kClosed: {
        metadata_.SetClosed();
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
  jw << "LogNumber" << wal.GetLogNumber() << "SyncedSizeInBytes"
     << wal.GetMetadata().GetSyncedSizeInBytes() << "Closed"
     << wal.GetMetadata().IsClosed();
  return jw;
}

std::ostream& operator<<(std::ostream& os, const WalAddition& wal) {
  os << "log_number: " << wal.GetLogNumber()
     << " synced_size_in_bytes: " << wal.GetMetadata().GetSyncedSizeInBytes()
     << " closed: " << wal.GetMetadata().IsClosed();
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

Status WalSet::AddWal(const WalAddition& wal, bool recovery) {
  auto it = wals_.lower_bound(wal.GetLogNumber());
  bool existing = it != wals_.end() && it->first == wal.GetLogNumber();
  if (wal.GetMetadata().IsClosed()) {
    // Recovery mode: the WAL may not exist.
    // Non-recovery mode: the WAL must exist and not closed.
    if (!recovery && !existing) {
      std::stringstream ss;
      ss << "WAL " << wal.GetLogNumber() << " is not created before closing";
      return Status::Corruption("WalSet", ss.str());
    }
    if (existing && it->second.IsClosed()) {
      std::stringstream ss;
      ss << "WAL " << wal.GetLogNumber() << " is closed more than once";
      return Status::Corruption("WalSet", ss.str());
    }
  } else {
    if (existing && !wal.GetMetadata().HasSyncedSize()) {
      std::stringstream ss;
      ss << "WAL " << wal.GetLogNumber() << " is created more than once";
      return Status::Corruption("WalSet", ss.str());
    }
  }
  // If the WAL has synced size, it must >= the previous size.
  if (wal.GetMetadata().HasSyncedSize() && existing &&
      it->second.HasSyncedSize() &&
      wal.GetMetadata().GetSyncedSizeInBytes() <
          it->second.GetSyncedSizeInBytes()) {
    std::stringstream ss;
    ss << "WAL " << wal.GetLogNumber()
       << " must not have smaller synced size than previous one";
    return Status::Corruption("WalSet", ss.str());
  }
  if (existing) {
    it->second = wal.GetMetadata();
  } else {
    wals_.insert(it, {wal.GetLogNumber(), wal.GetMetadata()});
  }
  return Status::OK();
}

Status WalSet::AddWals(const WalAdditions& wals, bool recovery) {
  Status s;
  for (const WalAddition& wal : wals) {
    s = AddWal(wal, recovery);
    if (!s.ok()) {
      break;
    }
  }
  return s;
}

Status WalSet::DeleteWal(const WalDeletion& wal) {
  auto it = wals_.find(wal.GetLogNumber());
  // The WAL must exist and has been closed.
  if (it == wals_.end()) {
    std::stringstream ss;
    ss << "WAL " << wal.GetLogNumber() << " must exist before deletion";
    return Status::Corruption("WalSet", ss.str());
  }
  if (!it->second.IsClosed()) {
    std::stringstream ss;
    ss << "WAL " << wal.GetLogNumber() << " must be closed before deletion";
    return Status::Corruption("WalSet", ss.str());
  }
  wals_.erase(it);
  return Status::OK();
}

Status WalSet::DeleteWals(const WalDeletions& wals) {
  Status s;
  for (const WalDeletion& wal : wals) {
    s = DeleteWal(wal);
    if (!s.ok()) {
      break;
    }
  }
  return s;
}

void WalSet::Reset() { wals_.clear(); }

Status WalSet::CheckWals(
    Env* env,
    const std::unordered_map<WalNumber, std::string>& logs_on_disk) const {
  assert(env != nullptr);

  Status s;
  for (const auto& wal : wals_) {
    const uint64_t log_number = wal.first;
    const WalMetadata& wal_meta = wal.second;

    if (!wal_meta.HasSyncedSize()) {
      // The WAL and WAL directory is not even synced,
      // so the WAL's inode may not be persisted,
      // then the WAL might not show up when listing WAL directory.
      continue;
    }

    if (logs_on_disk.find(log_number) == logs_on_disk.end()) {
      std::stringstream ss;
      ss << "Missing WAL with log number: " << log_number << ".";
      s = Status::Corruption(ss.str());
      break;
    }

    uint64_t log_file_size = 0;
    s = env->GetFileSize(logs_on_disk.at(log_number), &log_file_size);
    if (!s.ok()) {
      break;
    }
    if (log_file_size < wal_meta.GetSyncedSizeInBytes()) {
      std::stringstream ss;
      ss << "Size mismatch: WAL (log number: " << log_number
         << ") in MANIFEST is " << wal_meta.GetSyncedSizeInBytes()
         << " bytes , but actually is " << log_file_size << " bytes on disk.";
      s = Status::Corruption(ss.str());
      break;
    }
  }

  return s;
}

}  // namespace ROCKSDB_NAMESPACE
