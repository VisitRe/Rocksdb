//  Copyright (c) 2013, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "table/format.h"

#include <string>
#include <inttypes.h>

#include "port/port.h"
#include "rocksdb/env.h"
#include "table/block.h"
#include "util/coding.h"
#include "util/crc32c.h"
#include "util/perf_context_imp.h"
#include "util/xxhash.h"

namespace rocksdb {

void BlockHandle::EncodeTo(std::string* dst) const {
  // Sanity check that all fields have been set
  assert(offset_ != ~static_cast<uint64_t>(0));
  assert(size_ != ~static_cast<uint64_t>(0));
  PutVarint64(dst, offset_);
  PutVarint64(dst, size_);
}

Status BlockHandle::DecodeFrom(Slice* input) {
  if (GetVarint64(input, &offset_) &&
      GetVarint64(input, &size_)) {
    return Status::OK();
  } else {
    return Status::Corruption("bad block handle");
  }
}
const BlockHandle BlockHandle::kNullBlockHandle(0, 0);

void Footer::EncodeTo(std::string *dst) const {
  const size_t original_size = dst->size();
  PutVarint32(dst, static_cast<uint32_t>(checksum_));
  metaindex_handle_.EncodeTo(dst);
  index_handle_.EncodeTo(dst);
  dst->resize(original_size + kVersion1EncodedLength - 12); // Padding
  PutFixed32(dst, kFooterVersion);
  PutFixed32(dst, static_cast<uint32_t>(table_magic_number() & 0xffffffffu));
  PutFixed32(dst, static_cast<uint32_t>(table_magic_number() >> 32));
  assert(dst->size() == original_size + kVersion1EncodedLength);
}

Status Footer::DecodeFrom(Slice* input) {
  assert(input != nullptr);
  assert(input->size() >= kMinEncodedLength);

  const char *magic_ptr =
      input->data() + input->size() - kMagicNumberLengthByte;
  const uint32_t magic_lo = DecodeFixed32(magic_ptr);
  const uint32_t magic_hi = DecodeFixed32(magic_ptr + 4);
  uint64_t magic = ((static_cast<uint64_t>(magic_hi) << 32) |
                    (static_cast<uint64_t>(magic_lo)));

  // We check for legacy formats here and silently upconvert them
  bool legacy = false;
  if (magic == 0xdb4775248b80fb57ull) {
    legacy = true;
    extern uint64_t kBlockBasedTableMagicNumber;
    magic = kBlockBasedTableMagicNumber;
  } else if (magic == 0x4f3418eb7a8f13b8ull) {
    legacy = true;
    extern uint64_t kPlainTableMagicNumber;
    magic = kPlainTableMagicNumber;
  }
  if (HasInitializedTableMagicNumber()) {
    if (magic != table_magic_number()) {
      char buffer[80];
      snprintf(buffer, sizeof(buffer) - 1,
               "not an sstable (bad magic number --- %lx)",
               (long)magic);
      return Status::InvalidArgument(buffer);
    }
  } else {
    set_table_magic_number(magic);
  }

  if (legacy) {
    // Footer version 0 (legacy) will always occupy exactly this many bytes.
    // It consists of two block handles, padding, and a magic number.
    if (input->size() < kVersion0EncodedLength) {
      return Status::InvalidArgument("input is too short to be a legacy sstable");
    } else {
      input->remove_prefix(input->size() - kVersion0EncodedLength);
    }
    // we use version 0 to denore the legacy footer format
    version_ = 0;
    checksum_ = kCRC32c;
  } else {
    version_ = DecodeFixed32(magic_ptr - 4);
    if (version_ != kFooterVersion) {
      return Status::Corruption("bad footer version");
    }
    // Footer version 1 will always occupy exactly this many bytes.
    // It consists of the checksum type, two block handles, padding,
    // a version number, and a magic number
    if (input->size() < kVersion1EncodedLength) {
      return Status::InvalidArgument("input is too short to be an sstable");
    } else {
      input->remove_prefix(input->size() - kVersion1EncodedLength);
    }
    uint32_t checksum;
    if (!GetVarint32(input, &checksum)) {
      return Status::Corruption("bad checksum type");
    }
    checksum_ = static_cast<ChecksumType>(checksum);
  }

  Status result = metaindex_handle_.DecodeFrom(input);
  if (result.ok()) {
    result = index_handle_.DecodeFrom(input);
  }
  if (result.ok()) {
    // We skip over any leftover data (just padding for now) in "input"
    const char *end = magic_ptr + kMagicNumberLengthByte;
    *input = Slice(end, input->data() + input->size() - end);
  }
  return result;
}

Status ReadFooterFromFile(RandomAccessFile* file,
                          uint64_t file_size,
                          Footer* footer) {
  if (file_size < Footer::kMinEncodedLength) {
    return Status::InvalidArgument("file is too short to be an sstable");
  }

  char footer_space[Footer::kMaxEncodedLength];
  Slice footer_input;
  Status s = file->Read(file_size - Footer::kMaxEncodedLength,
                        Footer::kMaxEncodedLength,
                        &footer_input,
                        footer_space);
  if (!s.ok()) return s;

  // Check that we actually read the whole footer from the file. It may be
  // that size isn't correct.
  if (footer_input.size() < Footer::kMinEncodedLength) {
    return Status::InvalidArgument("file is too short to be an sstable");
  }

  return footer->DecodeFrom(&footer_input);
}

Status ReadBlockContents(RandomAccessFile* file,
                         const Footer& footer,
                         const ReadOptions& options,
                         const BlockHandle& handle,
                         BlockContents* result,
                         Env* env,
                         bool do_uncompress) {
  result->data = Slice();
  result->cachable = false;
  result->heap_allocated = false;

  // Read the block contents as well as the type/crc footer.
  // See table_builder.cc for the code that built this structure.
  size_t n = static_cast<size_t>(handle.size());
  char* buf = new char[n + kBlockTrailerSize];
  Slice contents;

  StopWatchNano timer(env);
  StartPerfTimer(&timer);
  Status s = file->Read(handle.offset(), n + kBlockTrailerSize, &contents, buf);
  BumpPerfCount(&perf_context.block_read_count);
  BumpPerfCount(&perf_context.block_read_byte, n + kBlockTrailerSize);
  BumpPerfTime(&perf_context.block_read_time, &timer);

  if (!s.ok()) {
    delete[] buf;
    return s;
  }
  if (contents.size() != n + kBlockTrailerSize) {
    delete[] buf;
    return Status::Corruption("truncated block read");
  }

  // Check the crc of the type and the block contents
  const char* data = contents.data();    // Pointer to where Read put the data
  if (options.verify_checksums) {
    uint32_t value = DecodeFixed32(data + n + 1);
    uint32_t actual;
    switch (footer.checksum()) {
    case kCRC32c:
      value = crc32c::Unmask(value);
      actual = crc32c::Value(data, n + 1);
      break;
    case kxxHash:
      actual = XXH32(data, n + 1, 0);
      break;
    default:
      // FIXME warn about unknown checksum type
      actual = ~value;
    }
    if (actual != value) {
      delete[] buf;
      s = Status::Corruption("block checksum mismatch");
      return s;
    }
    BumpPerfTime(&perf_context.block_checksum_time, &timer);
  }

  rocksdb::CompressionType type = (rocksdb::CompressionType)(data[n]);
  // If the caller has requested that the block not be uncompressed
  if (!do_uncompress || type == kNoCompression) {
    if (data != buf) {
      // File implementation gave us pointer to some other data.
      // Use it directly under the assumption that it will be live
      // while the file is open.
      delete[] buf;
      result->data = Slice(data, n);
      result->heap_allocated = false;
      result->cachable = false;  // Do not double-cache
    } else {
      result->data = Slice(buf, n);
      result->heap_allocated = true;
      result->cachable = true;
    }
    result->compression_type = type;
    s = Status::OK();
  } else {
    s = UncompressBlockContents(data, n, result);
    delete[] buf;
  }
  BumpPerfTime(&perf_context.block_decompress_time, &timer);
  return s;
}

//
// The 'data' points to the raw block contents that was read in from file.
// This method allocates a new heap buffer and the raw block
// contents are uncompresed into this buffer. This
// buffer is returned via 'result' and it is upto the caller to
// free this buffer.
Status UncompressBlockContents(const char* data, size_t n,
                               BlockContents* result) {
  char* ubuf = nullptr;
  int decompress_size = 0;
  assert(data[n] != kNoCompression);
  switch (data[n]) {
    case kSnappyCompression: {
      size_t ulength = 0;
      static char snappy_corrupt_msg[] =
        "Snappy not supported or corrupted Snappy compressed block contents";
      if (!port::Snappy_GetUncompressedLength(data, n, &ulength)) {
        return Status::Corruption(snappy_corrupt_msg);
      }
      ubuf = new char[ulength];
      if (!port::Snappy_Uncompress(data, n, ubuf)) {
        delete[] ubuf;
        return Status::Corruption(snappy_corrupt_msg);
      }
      result->data = Slice(ubuf, ulength);
      result->heap_allocated = true;
      result->cachable = true;
      break;
    }
    case kZlibCompression:
      ubuf = port::Zlib_Uncompress(data, n, &decompress_size);
      static char zlib_corrupt_msg[] =
        "Zlib not supported or corrupted Zlib compressed block contents";
      if (!ubuf) {
        return Status::Corruption(zlib_corrupt_msg);
      }
      result->data = Slice(ubuf, decompress_size);
      result->heap_allocated = true;
      result->cachable = true;
      break;
    case kBZip2Compression:
      ubuf = port::BZip2_Uncompress(data, n, &decompress_size);
      static char bzip2_corrupt_msg[] =
        "Bzip2 not supported or corrupted Bzip2 compressed block contents";
      if (!ubuf) {
        return Status::Corruption(bzip2_corrupt_msg);
      }
      result->data = Slice(ubuf, decompress_size);
      result->heap_allocated = true;
      result->cachable = true;
      break;
    case kLZ4Compression:
      ubuf = port::LZ4_Uncompress(data, n, &decompress_size);
      static char lz4_corrupt_msg[] =
          "LZ4 not supported or corrupted LZ4 compressed block contents";
      if (!ubuf) {
        return Status::Corruption(lz4_corrupt_msg);
      }
      result->data = Slice(ubuf, decompress_size);
      result->heap_allocated = true;
      result->cachable = true;
      break;
    case kLZ4HCCompression:
      ubuf = port::LZ4_Uncompress(data, n, &decompress_size);
      static char lz4hc_corrupt_msg[] =
          "LZ4HC not supported or corrupted LZ4HC compressed block contents";
      if (!ubuf) {
        return Status::Corruption(lz4hc_corrupt_msg);
      }
      result->data = Slice(ubuf, decompress_size);
      result->heap_allocated = true;
      result->cachable = true;
      break;
    default:
      return Status::Corruption("bad block type");
  }
  result->compression_type = kNoCompression;  // not compressed any more
  return Status::OK();
}

}  // namespace rocksdb
