//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#pragma once

#include <cinttypes>
#include <memory>
#include <string>

#include "file/random_access_file_reader.h"
#include "rocksdb/compression_type.h"
#include "rocksdb/rocksdb_namespace.h"

namespace ROCKSDB_NAMESPACE {

class Status;
struct ReadOptions;
struct ImmutableCFOptions;
struct FileOptions;
class HistogramImpl;
class Slice;
class GetContext;
class RandomAccessFileReader;

class BlobFileReader {
 public:
  static Status Create(const ImmutableCFOptions& immutable_cf_options,
                       const FileOptions& file_options,
                       uint32_t column_family_id,
                       HistogramImpl* blob_file_read_hist,
                       uint64_t blob_file_number,
                       std::unique_ptr<BlobFileReader>* reader);

  BlobFileReader(const BlobFileReader&) = delete;
  BlobFileReader& operator=(const BlobFileReader&) = delete;

  ~BlobFileReader();

  Status GetBlob(const ReadOptions& read_options, const Slice& user_key,
                 uint64_t offset, uint64_t value_size,
                 CompressionType compression_type,
                 GetContext* get_context) const;

 private:
  BlobFileReader(std::unique_ptr<RandomAccessFileReader>&& file_reader,
                 CompressionType compression_type);

  static Status OpenFile(const ImmutableCFOptions& immutable_cf_options,
                         const FileOptions& file_opts,
                         HistogramImpl* blob_file_read_hist,
                         uint64_t blob_file_number, uint64_t* file_size,
                         std::unique_ptr<RandomAccessFileReader>* file_reader);

  static Status ReadHeader(RandomAccessFileReader* file_reader,
                           uint32_t column_family_id,
                           CompressionType* compression_type);

  static Status ReadFooter(uint64_t file_size,
                           RandomAccessFileReader* file_reader);

  static Status ReadFromFile(RandomAccessFileReader* file_reader,
                             uint64_t read_offset, size_t read_size,
                             Slice* slice, std::string* buf,
                             AlignedBuf* aligned_buf);

  static Status VerifyBlob(const Slice& record_slice, const Slice& user_key,
                           uint64_t key_size, uint64_t value_size);

  static Status UncompressBlobIfNeeded(const Slice& value_slice,
                                       CompressionType compression_type,
                                       GetContext* get_context);

  std::unique_ptr<RandomAccessFileReader> file_reader_;
  CompressionType compression_type_;
};

}  // namespace ROCKSDB_NAMESPACE
