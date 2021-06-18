//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#pragma once

#include <cassert>
#include <cstdint>
#include <unordered_map>

#include "db/blob/blob_constants.h"
#include "rocksdb/rocksdb_namespace.h"
#include "rocksdb/status.h"

namespace ROCKSDB_NAMESPACE {

// A class that can be used to compute the amount of additional garbage
// generated by a compaction. It parses the keys and blob references in the
// input and output of a compaction, and aggregates the "inflow" and "outflow"
// on a per-blob file basis. The amount of additional garbage for any given blob
// file can then be computed by subtracting the outflow from the inflow.
class BlobGarbageMeter {
 public:
  class BlobStats {
   public:
    void Add(uint64_t bytes) {
      ++count_;
      bytes_ += bytes;
    }
    void Add(uint64_t count, uint64_t bytes) {
      count_ += count;
      bytes_ += bytes;
    }

    uint64_t GetCount() const { return count_; }
    uint64_t GetBytes() const { return bytes_; }

   private:
    uint64_t count_ = 0;
    uint64_t bytes_ = 0;
  };

  class BlobInOutFlow {
   public:
    void AddInFlow(uint64_t bytes) {
      in_flow_.Add(bytes);
      assert(IsValid());
    }
    void AddOutFlow(uint64_t bytes) {
      out_flow_.Add(bytes);
      assert(IsValid());
    }

    const BlobStats& GetInFlow() const { return in_flow_; }
    const BlobStats& GetOutFlow() const { return out_flow_; }

    bool IsValid() const {
      return in_flow_.GetCount() >= out_flow_.GetCount() &&
             in_flow_.GetBytes() >= out_flow_.GetBytes();
    }
    bool HasGarbage() const {
      assert(IsValid());
      return in_flow_.GetCount() > out_flow_.GetCount();
    }
    uint64_t GetGarbageCount() const {
      assert(IsValid());
      assert(HasGarbage());
      return in_flow_.GetCount() - out_flow_.GetCount();
    }
    uint64_t GetGarbageBytes() const {
      assert(IsValid());
      assert(HasGarbage());
      return in_flow_.GetBytes() - out_flow_.GetBytes();
    }

   private:
    BlobStats in_flow_;
    BlobStats out_flow_;
  };

  Status ProcessInFlow(const Slice& key, const Slice& value);
  Status ProcessOutFlow(const Slice& key, const Slice& value);

  const std::unordered_map<uint64_t, BlobInOutFlow>& flows() const {
    return flows_;
  }

 private:
  static Status Parse(const Slice& key, const Slice& value,
                      uint64_t* blob_file_number, uint64_t* bytes);

  std::unordered_map<uint64_t, BlobInOutFlow> flows_;
};

}  // namespace ROCKSDB_NAMESPACE
