//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#pragma once

// To do: ROCKSDB_LITE ?

#include "rocksdb/db.h"
#include "rocksdb/slice.h"
#include "rocksdb/write_batch.h"

namespace ROCKSDB_NAMESPACE {

// Supported Trace types.
enum TraceType : char {
  kTraceNone = 0,
  kTraceBegin = 1,
  kTraceEnd = 2,
  kTraceWrite = 3,
  kTraceGet = 4,
  kTraceIteratorSeek = 5,
  kTraceIteratorSeekForPrev = 6,
  // Block cache related types.
  kBlockTraceIndexBlock = 7,
  kBlockTraceFilterBlock = 8,
  kBlockTraceDataBlock = 9,
  kBlockTraceUncompressionDictBlock = 10,
  kBlockTraceRangeDeletionBlock = 11,
  // For IOTracing.
  kIOTracer = 12,
  // For query tracing
  kTraceMultiGet = 13,
  // All trace types should be added before kTraceMax
  kTraceMax,
};

// Base class for all types of trace records.
class TraceRecord {
 public:
  explicit TraceRecord(uint64_t ts = 0) : timestamp(ts) {}
  virtual ~TraceRecord() {}

  virtual TraceType GetTraceType() const = 0;

  // Timestamp (in microseconds) of this trace.
  uint64_t timestamp;
};

// Base class for all query types of trace records.
class QueryTraceRecord : public TraceRecord {
 public:
  explicit QueryTraceRecord(uint64_t ts = 0) : TraceRecord(ts) {}
  virtual ~QueryTraceRecord() override {}
};

// Trace record for DB::Write() operation
class WriteQueryTraceRecord : public QueryTraceRecord {
 public:
  explicit WriteQueryTraceRecord(uint64_t ts = 0) : QueryTraceRecord(ts) {}
  virtual ~WriteQueryTraceRecord() override {}

  TraceType GetTraceType() const override { return kTraceWrite; };

  WriteBatch batch;
};

// Trace record for DB::Get() operation
class GetQueryTraceRecord : public QueryTraceRecord {
 public:
  explicit GetQueryTraceRecord(uint64_t ts = 0)
      : QueryTraceRecord(ts), handle(nullptr) {}
  virtual ~GetQueryTraceRecord() override {}

  TraceType GetTraceType() const override { return kTraceGet; };

  // Column family to search.
  ColumnFamilyHandle* handle;

  // Key to get.
  Slice key;
};

// To do: shall we merge IteratorSeekQueryTraceRecord and
// IteratorSeekForPrevQueryTraceRecord into one, and add a SeekType enum to the
// class?

// Trace record for Iterator::Seek() operation
class IteratorSeekQueryTraceRecord : public QueryTraceRecord {
 public:
  explicit IteratorSeekQueryTraceRecord(uint64_t ts = 0)
      : QueryTraceRecord(ts), handle(nullptr) {}
  virtual ~IteratorSeekQueryTraceRecord() override {}

  TraceType GetTraceType() const override { return kTraceIteratorSeek; };

  // Used to create an Iterator object.
  ColumnFamilyHandle* handle;

  // Key to seek to.
  Slice key;
};

// Trace record for Iterator::SeekForPrev() operation
class IteratorSeekForPrevQueryTraceRecord : public QueryTraceRecord {
 public:
  explicit IteratorSeekForPrevQueryTraceRecord(uint64_t ts = 0)
      : QueryTraceRecord(ts) {}
  virtual ~IteratorSeekForPrevQueryTraceRecord() override {}

  TraceType GetTraceType() const override { return kTraceIteratorSeekForPrev; };

  // Used to create an Iterator object.
  ColumnFamilyHandle* handle;

  // Key to seek to.
  Slice key;
};

// Trace record for DB::MultiGet() operation
class MultiGetQueryTraceRecord : public QueryTraceRecord {
 public:
  explicit MultiGetQueryTraceRecord(uint64_t ts = 0) : QueryTraceRecord(ts) {}
  virtual ~MultiGetQueryTraceRecord() override {}

  TraceType GetTraceType() const override { return kTraceMultiGet; };

  // Column families to search.
  std::vector<ColumnFamilyHandle*> handles;

  // Keys to get.
  std::vector<Slice> keys;
};

}  // namespace ROCKSDB_NAMESPACE
