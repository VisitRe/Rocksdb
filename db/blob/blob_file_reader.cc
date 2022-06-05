//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include "db/blob/blob_file_reader.h"

#include <cassert>
#include <string>

#include "db/blob/blob_log_format.h"
#include "file/file_prefetch_buffer.h"
#include "file/filename.h"
#include "monitoring/statistics.h"
#include "options/cf_options.h"
#include "rocksdb/file_system.h"
#include "rocksdb/slice.h"
#include "rocksdb/status.h"
#include "test_util/sync_point.h"
#include "util/compression.h"
#include "util/crc32c.h"
#include "util/stop_watch.h"

namespace ROCKSDB_NAMESPACE {

Status BlobFileReader::Create(
    const ImmutableOptions& immutable_options, const FileOptions& file_options,
    uint32_t column_family_id, HistogramImpl* blob_file_read_hist,
    uint64_t blob_file_number, const std::shared_ptr<IOTracer>& io_tracer,
    std::unique_ptr<BlobFileReader>* blob_file_reader) {
  assert(blob_file_reader);
  assert(!*blob_file_reader);

  uint64_t file_size = 0;
  std::unique_ptr<RandomAccessFileReader> file_reader;

  {
    const Status s =
        OpenFile(immutable_options, file_options, blob_file_read_hist,
                 blob_file_number, io_tracer, &file_size, &file_reader);
    if (!s.ok()) {
      return s;
    }
  }

  assert(file_reader);

  Statistics* const statistics = immutable_options.stats;

  CompressionType compression_type = kNoCompression;

  {
    const Status s = ReadHeader(file_reader.get(), column_family_id, statistics,
                                &compression_type);
    if (!s.ok()) {
      return s;
    }
  }

  {
    const Status s = ReadFooter(file_reader.get(), file_size, statistics);
    if (!s.ok()) {
      return s;
    }
  }

  blob_file_reader->reset(new BlobFileReader(std::move(file_reader), file_size,
                                             blob_file_number, compression_type,
                                             immutable_options));

  return Status::OK();
}

Status BlobFileReader::OpenFile(
    const ImmutableOptions& immutable_options, const FileOptions& file_opts,
    HistogramImpl* blob_file_read_hist, uint64_t blob_file_number,
    const std::shared_ptr<IOTracer>& io_tracer, uint64_t* file_size,
    std::unique_ptr<RandomAccessFileReader>* file_reader) {
  assert(file_size);
  assert(file_reader);

  const auto& cf_paths = immutable_options.cf_paths;
  assert(!cf_paths.empty());

  const std::string blob_file_path =
      BlobFileName(cf_paths.front().path, blob_file_number);

  FileSystem* const fs = immutable_options.fs.get();
  assert(fs);

  constexpr IODebugContext* dbg = nullptr;

  {
    TEST_SYNC_POINT("BlobFileReader::OpenFile:GetFileSize");

    const Status s =
        fs->GetFileSize(blob_file_path, IOOptions(), file_size, dbg);
    if (!s.ok()) {
      return s;
    }
  }

  if (*file_size < BlobLogHeader::kSize + BlobLogFooter::kSize) {
    return Status::Corruption("Malformed blob file");
  }

  std::unique_ptr<FSRandomAccessFile> file;

  {
    TEST_SYNC_POINT("BlobFileReader::OpenFile:NewRandomAccessFile");

    const Status s =
        fs->NewRandomAccessFile(blob_file_path, file_opts, &file, dbg);
    if (!s.ok()) {
      return s;
    }
  }

  assert(file);

  if (immutable_options.advise_random_on_open) {
    file->Hint(FSRandomAccessFile::kRandom);
  }

  file_reader->reset(new RandomAccessFileReader(
      std::move(file), blob_file_path, immutable_options.clock, io_tracer,
      immutable_options.stats, BLOB_DB_BLOB_FILE_READ_MICROS,
      blob_file_read_hist, immutable_options.rate_limiter.get(),
      immutable_options.listeners));

  return Status::OK();
}

Status BlobFileReader::ReadHeader(const RandomAccessFileReader* file_reader,
                                  uint32_t column_family_id,
                                  Statistics* statistics,
                                  CompressionType* compression_type) {
  assert(file_reader);
  assert(compression_type);

  Slice header_slice;
  Buffer buf;
  AlignedBuf aligned_buf;

  {
    TEST_SYNC_POINT("BlobFileReader::ReadHeader:ReadFromFile");

    constexpr uint64_t read_offset = 0;
    constexpr size_t read_size = BlobLogHeader::kSize;

    // TODO: rate limit reading headers from blob files.
    const Status s = ReadFromFile(file_reader, read_offset, read_size,
                                  statistics, &header_slice, &buf, &aligned_buf,
                                  Env::IO_TOTAL /* rate_limiter_priority */);
    if (!s.ok()) {
      return s;
    }

    TEST_SYNC_POINT_CALLBACK("BlobFileReader::ReadHeader:TamperWithResult",
                             &header_slice);
  }

  BlobLogHeader header;

  {
    const Status s = header.DecodeFrom(header_slice);
    if (!s.ok()) {
      return s;
    }
  }

  constexpr ExpirationRange no_expiration_range;

  if (header.has_ttl || header.expiration_range != no_expiration_range) {
    return Status::Corruption("Unexpected TTL blob file");
  }

  if (header.column_family_id != column_family_id) {
    return Status::Corruption("Column family ID mismatch");
  }

  *compression_type = header.compression;

  return Status::OK();
}

Status BlobFileReader::ReadFooter(const RandomAccessFileReader* file_reader,
                                  uint64_t file_size, Statistics* statistics) {
  assert(file_size >= BlobLogHeader::kSize + BlobLogFooter::kSize);
  assert(file_reader);

  Slice footer_slice;
  Buffer buf;
  AlignedBuf aligned_buf;

  {
    TEST_SYNC_POINT("BlobFileReader::ReadFooter:ReadFromFile");

    const uint64_t read_offset = file_size - BlobLogFooter::kSize;
    constexpr size_t read_size = BlobLogFooter::kSize;

    // TODO: rate limit reading footers from blob files.
    const Status s = ReadFromFile(file_reader, read_offset, read_size,
                                  statistics, &footer_slice, &buf, &aligned_buf,
                                  Env::IO_TOTAL /* rate_limiter_priority */);
    if (!s.ok()) {
      return s;
    }

    TEST_SYNC_POINT_CALLBACK("BlobFileReader::ReadFooter:TamperWithResult",
                             &footer_slice);
  }

  BlobLogFooter footer;

  {
    const Status s = footer.DecodeFrom(footer_slice);
    if (!s.ok()) {
      return s;
    }
  }

  constexpr ExpirationRange no_expiration_range;

  if (footer.expiration_range != no_expiration_range) {
    return Status::Corruption("Unexpected TTL blob file");
  }

  return Status::OK();
}

Status BlobFileReader::ReadFromFile(const RandomAccessFileReader* file_reader,
                                    uint64_t read_offset, size_t read_size,
                                    Statistics* statistics, Slice* slice,
                                    Buffer* buf, AlignedBuf* aligned_buf,
                                    Env::IOPriority rate_limiter_priority) {
  assert(slice);
  assert(buf);
  assert(aligned_buf);

  assert(file_reader);

  RecordTick(statistics, BLOB_DB_BLOB_FILE_BYTES_READ, read_size);

  Status s;

  if (file_reader->use_direct_io()) {
    constexpr char* scratch = nullptr;

    s = file_reader->Read(IOOptions(), read_offset, read_size, slice, scratch,
                          aligned_buf, rate_limiter_priority);
  } else {
    buf->reset(new char[read_size]);
    constexpr AlignedBuf* aligned_scratch = nullptr;

    s = file_reader->Read(IOOptions(), read_offset, read_size, slice,
                          buf->get(), aligned_scratch, rate_limiter_priority);
  }

  if (!s.ok()) {
    return s;
  }

  if (slice->size() != read_size) {
    return Status::Corruption("Failed to read data from blob file");
  }

  return Status::OK();
}

BlobFileReader::BlobFileReader(
    std::unique_ptr<RandomAccessFileReader>&& file_reader, uint64_t file_size,
    uint64_t file_number, CompressionType compression_type,
    const ImmutableOptions& ioptions)
    : file_reader_(std::move(file_reader)),
      file_size_(file_size),
      compression_type_(compression_type),
      ioptions_(ioptions),
      base_cache_key_(ioptions_.no_blob_cache
                          ? OffsetableCacheKey()
                          : OffsetableCacheKey(ioptions_.db_host_id,
                                               "" /* db session id */,
                                               file_number, file_size)) {
  assert(file_reader_);
}

BlobFileReader::~BlobFileReader() = default;

Status BlobFileReader::GetBlob(const ReadOptions& read_options,
                               const Slice& user_key, uint64_t offset,
                               uint64_t value_size,
                               CompressionType compression_type,
                               FilePrefetchBuffer* prefetch_buffer,
                               PinnableSlice* value,
                               uint64_t* bytes_read) const {
  assert(value);

  const uint64_t key_size = user_key.size();

  if (!IsValidBlobOffset(offset, key_size, value_size, file_size_)) {
    return Status::Corruption("Invalid blob offset");
  }

  if (compression_type != compression_type_) {
    return Status::Corruption("Compression type mismatch when reading blob");
  }

  // Note: if verify_checksum is set, we read the entire blob record to be able
  // to perform the verification; otherwise, we just read the blob itself. Since
  // the offset in BlobIndex actually points to the blob value, we need to make
  // an adjustment in the former case.
  const uint64_t adjustment =
      read_options.verify_checksums
          ? BlobLogRecord::CalculateAdjustmentForRecordHeader(key_size)
          : 0;
  assert(offset >= adjustment);

  const uint64_t record_offset = offset - adjustment;
  const uint64_t record_size = value_size + adjustment;

  Cache* blob_cache = ioptions_.blob_cache.get();
  bool use_cache = !ioptions_.no_blob_cache;

  Statistics* statistics = ioptions_.statistics.get();
  SystemClock* clock = ioptions_.clock;

  Slice* record_slice = nullptr;

  if (use_cache) {
    const Status s =
        MaybeReadBlobAndLoadToCache(prefetch_buffer, read_options, offset,
                                    false /* wait_for_cache */, record_slice);

    if (!s.ok()) {
      return s;
    }

    if (record_slice != nullptr) {
      assert(s.ok());
      return s;
    }
  }

  const bool no_io = read_options.read_tier == kBlockCacheTier;
  if (no_io) {
    return Status::Incomplete("no blocking io");
  }

  // Can't find the blob from the cache. Since I/O is allowed, read from the
  // file.
  Buffer buf;
  AlignedBuf aligned_buf;

  bool prefetched = false;

  if (prefetch_buffer) {
    Status s;
    constexpr bool for_compaction = true;

    prefetched = prefetch_buffer->TryReadFromCache(
        IOOptions(), file_reader_.get(), record_offset,
        static_cast<size_t>(record_size), record_slice, &s,
        read_options.rate_limiter_priority, for_compaction);
    if (!s.ok()) {
      return s;
    }
  }

  if (!prefetched) {
    TEST_SYNC_POINT("BlobFileReader::GetBlob:ReadFromFile");

    const Status s =
        ReadFromFile(file_reader_.get(), record_offset,
                     static_cast<size_t>(record_size), statistics, record_slice,
                     &buf, &aligned_buf, read_options.rate_limiter_priority);
    if (!s.ok()) {
      return s;
    }
  }

  TEST_SYNC_POINT_CALLBACK("BlobFileReader::GetBlob:TamperWithResult",
                           &record_slice);

  if (read_options.verify_checksums) {
    const Status s = VerifyBlob(*record_slice, user_key, value_size);
    if (!s.ok()) {
      return s;
    }
  }

  const Slice value_slice(record_slice->data() + adjustment, value_size);

  {
    const Status s = UncompressBlobIfNeeded(value_slice, compression_type,
                                            clock, statistics, value);
    if (!s.ok()) {
      return s;
    }
  }

  if (bytes_read) {
    *bytes_read = record_size;
  }

  if (read_options.fill_cache) {
    // If filling cache is allowed and a cache is configured, try to put the
    // blob to the cache.
    Slice key = base_cache_key_.WithOffset(offset).AsSlice();
    const Status s = PutDataBlobToCache(
        key, blob_cache, nullptr /* blob_cache_compressed */, value,
        blob_cache ? blob_cache->memory_allocator() : nullptr);
    if (!s.ok()) {
      return s;
    }
  }

  return Status::OK();
}

void BlobFileReader::MultiGetBlob(
    const ReadOptions& read_options,
    const autovector<std::reference_wrapper<const Slice>>& user_keys,
    const autovector<uint64_t>& offsets,
    const autovector<uint64_t>& value_sizes, autovector<Status*>& statuses,
    autovector<PinnableSlice*>& values, uint64_t* bytes_read) const {
  const size_t num_blobs = user_keys.size();
  assert(num_blobs > 0);
  assert(num_blobs == offsets.size());
  assert(num_blobs == value_sizes.size());
  assert(num_blobs == statuses.size());
  assert(num_blobs == values.size());

#ifndef NDEBUG
  for (size_t i = 0; i < offsets.size() - 1; ++i) {
    assert(offsets[i] <= offsets[i + 1]);
  }
#endif  // !NDEBUG

  std::vector<FSReadRequest> read_reqs(num_blobs);
  autovector<uint64_t> adjustments;
  uint64_t total_len = 0;
  for (size_t i = 0; i < num_blobs; ++i) {
    const size_t key_size = user_keys[i].get().size();
    assert(IsValidBlobOffset(offsets[i], key_size, value_sizes[i], file_size_));
    const uint64_t adjustment =
        read_options.verify_checksums
            ? BlobLogRecord::CalculateAdjustmentForRecordHeader(key_size)
            : 0;
    assert(offsets[i] >= adjustment);
    adjustments.push_back(adjustment);
    read_reqs[i].offset = offsets[i] - adjustment;
    read_reqs[i].len = value_sizes[i] + adjustment;
    total_len += read_reqs[i].len;
  }

  Statistics* statistics = ioptions_.statistics.get();
  SystemClock* clock = ioptions_.clock;

  RecordTick(statistics, BLOB_DB_BLOB_FILE_BYTES_READ, total_len);

  Buffer buf;
  AlignedBuf aligned_buf;

  Status s;
  bool direct_io = file_reader_->use_direct_io();
  if (direct_io) {
    for (size_t i = 0; i < read_reqs.size(); ++i) {
      read_reqs[i].scratch = nullptr;
    }
  } else {
    buf.reset(new char[total_len]);
    std::ptrdiff_t pos = 0;
    for (size_t i = 0; i < read_reqs.size(); ++i) {
      read_reqs[i].scratch = buf.get() + pos;
      pos += read_reqs[i].len;
    }
  }
  TEST_SYNC_POINT("BlobFileReader::MultiGetBlob:ReadFromFile");
  s = file_reader_->MultiRead(IOOptions(), read_reqs.data(), read_reqs.size(),
                              direct_io ? &aligned_buf : nullptr,
                              read_options.rate_limiter_priority);
  if (!s.ok()) {
    for (auto& req : read_reqs) {
      req.status.PermitUncheckedError();
    }
    for (size_t i = 0; i < num_blobs; ++i) {
      assert(statuses[i]);
      *statuses[i] = s;
    }
    return;
  }

  assert(s.ok());
  for (size_t i = 0; i < num_blobs; ++i) {
    auto& req = read_reqs[i];
    assert(statuses[i]);
    if (req.status.ok() && req.result.size() != req.len) {
      req.status = IOStatus::Corruption("Failed to read data from blob file");
    }
    *statuses[i] = req.status;
  }

  if (read_options.verify_checksums) {
    for (size_t i = 0; i < num_blobs; ++i) {
      assert(statuses[i]);
      if (!statuses[i]->ok()) {
        continue;
      }
      const Slice& record_slice = read_reqs[i].result;
      s = VerifyBlob(record_slice, user_keys[i], value_sizes[i]);
      if (!s.ok()) {
        assert(statuses[i]);
        *statuses[i] = s;
      }
    }
  }

  for (size_t i = 0; i < num_blobs; ++i) {
    assert(statuses[i]);
    if (!statuses[i]->ok()) {
      continue;
    }
    const Slice& record_slice = read_reqs[i].result;
    const Slice value_slice(record_slice.data() + adjustments[i],
                            value_sizes[i]);
    s = UncompressBlobIfNeeded(value_slice, compression_type_, clock,
                               statistics, values[i]);
    if (!s.ok()) {
      *statuses[i] = s;
    }
  }

  if (bytes_read) {
    uint64_t total_bytes = 0;
    for (const auto& req : read_reqs) {
      total_bytes += req.result.size();
    }
    *bytes_read = total_bytes;
  }
}

Status BlobFileReader::VerifyBlob(const Slice& record_slice,
                                  const Slice& user_key, uint64_t value_size) {
  BlobLogRecord record;

  const Slice header_slice(record_slice.data(), BlobLogRecord::kHeaderSize);

  {
    const Status s = record.DecodeHeaderFrom(header_slice);
    if (!s.ok()) {
      return s;
    }
  }

  if (record.key_size != user_key.size()) {
    return Status::Corruption("Key size mismatch when reading blob");
  }

  if (record.value_size != value_size) {
    return Status::Corruption("Value size mismatch when reading blob");
  }

  record.key =
      Slice(record_slice.data() + BlobLogRecord::kHeaderSize, record.key_size);
  if (record.key != user_key) {
    return Status::Corruption("Key mismatch when reading blob");
  }

  record.value = Slice(record.key.data() + record.key_size, value_size);

  {
    TEST_SYNC_POINT_CALLBACK("BlobFileReader::VerifyBlob:CheckBlobCRC",
                             &record);

    const Status s = record.CheckBlobCRC();
    if (!s.ok()) {
      return s;
    }
  }

  return Status::OK();
}

Status BlobFileReader::UncompressBlobIfNeeded(const Slice& value_slice,
                                              CompressionType compression_type,
                                              SystemClock* clock,
                                              Statistics* statistics,
                                              PinnableSlice* value) {
  assert(value);

  if (compression_type == kNoCompression) {
    SaveValue(value_slice, value);

    return Status::OK();
  }

  UncompressionContext context(compression_type);
  UncompressionInfo info(context, UncompressionDict::GetEmptyDict(),
                         compression_type);

  size_t uncompressed_size = 0;
  constexpr uint32_t compression_format_version = 2;
  constexpr MemoryAllocator* allocator = nullptr;

  CacheAllocationPtr output;

  {
    StopWatch stop_watch(clock, statistics, BLOB_DB_DECOMPRESSION_MICROS);
    output = UncompressData(info, value_slice.data(), value_slice.size(),
                            &uncompressed_size, compression_format_version,
                            allocator);
  }

  TEST_SYNC_POINT_CALLBACK(
      "BlobFileReader::UncompressBlobIfNeeded:TamperWithResult", &output);

  if (!output) {
    return Status::Corruption("Unable to uncompress blob");
  }

  SaveValue(Slice(output.get(), uncompressed_size), value);

  return Status::OK();
}

void BlobFileReader::SaveValue(const Slice& src, PinnableSlice* dst) {
  assert(dst);

  if (dst->IsPinned()) {
    dst->Reset();
  }

  dst->PinSelf(src);
}

Status BlobFileReader::MaybeReadBlobAndLoadToCache(
    FilePrefetchBuffer* prefetch_buffer, const ReadOptions& read_options,
    uint64_t offset, const bool wait, Slice* record_slice) const {
  assert(record_slice == nullptr);

  Cache* blob_cache = ioptions_.blob_cache.get();

  // First, try to get the blo bfrom the cache
  //
  // If either blob cache is enabled, we'll try to read from it.
  Status s;
  Slice key;
  bool is_cache_hit = false;
  if (blob_cache != nullptr) {
    // create key for blob cache
    key = base_cache_key_.WithOffset(offset).AsSlice();
    s = GetDataBlobFromCache(key, blob_cache,
                             nullptr /* blob_cache_compressed */, read_options,
                             record_slice, wait);

    if (!s.ok()) {
      return s;
    }

    if (record_slice != nullptr) {
      is_cache_hit = true;
      if (prefetch_buffer) {
        // Update the blob details so that PrefetchBuffer can use the read
        // pattern to determine if reads are sequential or not for prefetching.
        // It should also take in account blob read from cache.
        prefetch_buffer->UpdateReadPattern(
            offset, record_slice->size(),
            read_options.adaptive_readahead /* decrease_readahead_size */);
      }
    }
  }

  assert(s.ok() || record_slice == nullptr);

  return s;
}

Cache::Handle* BlobFileReader::GetEntryFromCache(
    const CacheTier& cache_tier, Cache* blob_cache, const Slice& key,
    const bool wait, const Cache::CacheItemHelper* cache_helper,
    const Cache::CreateCallback& create_cb, Cache::Priority priority) const {
  assert(blob_cache);

  Cache::Handle* cache_handle = nullptr;

  if (cache_tier == CacheTier::kNonVolatileBlockTier) {
    Status::NotSupported("Blob cache tier is not supported");
  } else {
    cache_handle = blob_cache->Lookup(key, ioptions_.statistics.get());
  }

  return cache_handle;
}

Status BlobFileReader::InsertEntryToCache(
    const CacheTier& cache_tier, Cache* blob_cache, const Slice& key,
    const Cache::CacheItemHelper* cache_helper, const Slice* record_slice,
    size_t charge, Cache::Handle** cache_handle,
    Cache::Priority priority) const {
  Status s;

  if (cache_tier == CacheTier::kNonVolatileBlockTier) {
    s = blob_cache->Insert(
        key, const_cast<void*>(static_cast<const void*>(record_slice->data())),
        cache_helper, charge, cache_handle, priority);
  } else {
    s = blob_cache->Insert(
        key, const_cast<void*>(static_cast<const void*>(record_slice->data())),
        charge, cache_helper->del_cb, cache_handle, priority);
  }

  return s;
}

Status BlobFileReader::GetDataBlobFromCache(
    const Slice& cache_key, Cache* blob_cache, Cache* blob_cache_compressed,
    const ReadOptions& read_options, Slice* record_slice, bool wait) const {
  const Cache::Priority priority = Cache::Priority::LOW;

  // Lookup uncompressed cache first
  if (blob_cache != nullptr) {
    assert(!cache_key.empty());
    Cache::Handle* cache_handle = nullptr;
    cache_handle = GetEntryFromCache(
        ioptions_.lowest_used_cache_tier, blob_cache, cache_key, wait,
        nullptr /* cache_helper */, nullptr /* create_db */, priority);
    if (cache_handle != nullptr) {
      record_slice = reinterpret_cast<Slice*>(blob_cache->Value(cache_handle));
      return Status::OK();
    }
  }

  // TODO: Add support for compressed blob cache.
  // If not found, search from the compressed blob cache.
  assert(record_slice == nullptr);

  return Status::NotFound("Blob record not found in cache");
}

Status BlobFileReader::PutDataBlobToCache(
    const Slice& cache_key, Cache* blob_cache, Cache* blob_cache_compressed,
    const Slice* record_slice, MemoryAllocator* memory_allocator) const {
  assert(record_slice);

  const Cache::Priority priority = Cache::Priority::LOW;

  Status s;
  Statistics* statistics = ioptions_.stats;

  // TODO: Insert compressed blob into compressed blob cache.
  // Release the hold on the compressed cache entry immediately.

  // insert into uncompressed blob cache
  if (blob_cache != nullptr) {
    size_t charge = record_slice->size();
    Cache::Handle* cache_handle = nullptr;
    s = InsertEntryToCache(ioptions_.lowest_used_cache_tier, blob_cache,
                           cache_key, GetCacheItemHelper(), record_slice,
                           charge, &cache_handle, priority);
  }

  return s;
}

}  // namespace ROCKSDB_NAMESPACE
