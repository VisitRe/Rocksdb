//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//

#pragma once

#include "rocksdb/async/callables.h"


#include "monitoring/iostats_context_imp.h"
#include "monitoring/perf_context_imp.h"

#include "rocksdb/status.h"
#include "table/format.h"
#include "util/aligned_buffer.h"
#include "util/file_reader_writer.h"
#include "util/stop_watch.h"

#include <type_traits>

namespace rocksdb {

class Env;
class HistogramImpl;
class RandomAccessFile;
class RandomAccessFileReader;
class Statistics;

namespace async {

using
RandomAccessCallback = RandomAccessFile::RandomAccessCallback;

class RandomFileReadContext {

  // Callback is empty when request is sync
  RandomAccessFile* ra_file_;
  Statistics*       stats_;
  HistogramImpl*    hist_;
  StopWatch         sw_;
  uint64_t          elapsed_;
  IOSTATS_METER_DECL(read_nanos);
  bool              direct_io_;

  Slice*            result_;
  char*             result_buffer_;
  uint64_t          read_offset_;    // disk offset to read
  uint64_t          offset_advance_; // offset in the direct_io buffer if used
  size_t            read_size_;  // how much to read
  size_t            n_;          // actual requested read size
  // Needed as intermediate for direct reads only
  AlignedBuffer     buf_;

 public:

  // Constructor argument values come from the reader class
  RandomFileReadContext(RandomAccessFile* file, Env* env, Statistics* stats,
                        HistogramImpl* hist, uint32_t hist_type,
                        bool direct_io,
                        size_t alignment) :
    ra_file_(file),
    stats_(stats),
    hist_(hist),
    sw_(env, stats, hist_type, (stats_ != nullptr) ? &elapsed_ : nullptr),
    elapsed_(0),
    IOSTATS_METER_INIT(read_nanos),
    direct_io_(direct_io),
    result_(nullptr),
    result_buffer_(nullptr),
    read_offset_(0),
    offset_advance_(0),
    read_size_(0),
    n_(0) {
    buf_.Alignment(alignment);
  }

  const Slice& GetResult() const {
    return *result_;
  }

  size_t GetRequestedSize() const {
    return n_;
  }

  void PrepareRead(uint64_t offset, size_t n, Slice* result, char* buffer) {
    result_ = result;
    result_buffer_ = buffer;
    n_ = n;

    IOSTATS_METER_START(read_nanos);

    if (direct_io_) {
      auto alignment = buf_.Alignment();
      read_offset_ = TruncateToPageBoundary(alignment, offset);
      offset_advance_ = offset - read_offset_;
      read_size_ = Roundup(offset + n, alignment) - read_offset_;

      buf_.AllocateNewBuffer(read_size_);
    } else {
      read_offset_ = offset;
      read_size_ = n;
    }
  }

  // Sync read
  Status RandomRead() {
    Status s;
    if (direct_io_) {
      assert(buf_.Capacity() >= read_size_);
      s = ra_file_->Read(read_offset_, read_size_, result_, buf_.BufferStart());
    } else {
      s = ra_file_->Read(read_offset_, read_size_, result_, result_buffer_);
    }
    return s;
  }

  Status RequestRandomRead(const RandomAccessCallback& iocb) {
    Status s;
    IOSTATS_METER_MEASURE(read_nanos);
    if (direct_io_) {
      assert(buf_.Capacity() >= read_size_);
      s = ra_file_->RequestRead(iocb, read_offset_, read_size_, result_,
                         buf_.BufferStart());
    } else {
      s = ra_file_->RequestRead(iocb, read_offset_, read_size_, result_, result_buffer_);
    }
    return s;
  }

  ~RandomFileReadContext() {}

  // Must be called by the supplied iocb no matter what
  void  OnRandomReadComplete(const Status& status, const Slice& slice);
};

// This is a base class for all random-reads
// async reads
// At present the context does not own
// the file since it is expected to be cached
// with a newly created TableReader
class RandomReadContext {
 public:

  RandomReadContext(const RandomReadContext&) = delete;
  RandomReadContext& operator-(const RandomReadContext&) = delete;

 private:

  std::aligned_storage<sizeof(RandomFileReadContext)>::type ra_context_;

  RandomFileReadContext& GetCtxRef() {
    return *reinterpret_cast<RandomFileReadContext*>(&ra_context_);
  }

 protected:

  RandomReadContext(RandomAccessFileReader* file,
                    uint64_t offset, size_t n,
                    Slice* result, char* buf) {
    auto data = file->GetReadContextData();
    new (&ra_context_) RandomFileReadContext(file->file(), data.env_, data.stats_,
        data.file_read_hist_, data.hist_type_, file->use_direct_io(),
        file->file()->GetRequiredBufferAlignment());

    GetCtxRef().PrepareRead(offset, n, result, buf);
  }

  // Helpers for containing or derived class use only
  const Slice& GetResult() {
    return GetCtxRef().GetResult();
  }

  size_t GetRequestedSize() {
    return GetCtxRef().GetRequestedSize();
  }

  Status RequestRead(const RandomAccessCallback& iocb) {
    return GetCtxRef().RequestRandomRead(iocb);
  }

  Status Read() {
    return GetCtxRef().RandomRead();
  }


  void  OnRandomReadComplete(const Status& status, const Slice& slice) {
    GetCtxRef().OnRandomReadComplete(status, slice);
  }

  // Non-virtual because protected
  // we want to be lightweight
  ~RandomReadContext()  {
    GetCtxRef().~RandomFileReadContext();
  }
};

class ReadFooterContext : protected RandomReadContext {
 public:
  using
  FooterReadCallback = async::Callable<Status, const Status&>;

 protected:

  Slice& GetFooterInput() {
    return footer_input_;
  }

  Status OnReadFooterComplete(const Status& status, const Slice& slice) {
    OnRandomReadComplete(status, slice);
    if (!status.ok()) return status;
    // Check that we actually read the whole footer from the file. It may be
    // that size isn't correct.
    if (footer_input_.size() < Footer::kMinEncodedLength) {
      return Status::Corruption("file is too short to be an sstable");
    }

    Status s = footer_->DecodeFrom(&footer_input_);
    if (!s.ok()) {
      return s;
    }

    if (enforce_table_magic_number_ != 0 &&
        enforce_table_magic_number_ != footer_->table_magic_number()) {
      return Status::Corruption("Bad table magic number");
    }
    return Status::OK();
  }

  ReadFooterContext(const FooterReadCallback& footer_cb,
                    RandomAccessFileReader* file, uint64_t offset,
                    Footer* footer, uint64_t enforce_table_magic_number) :
    RandomReadContext(file, offset, Footer::kMaxEncodedLength,
                      &footer_input_, footer_space_),
    footer_cb_(footer_cb),
    footer_(footer),
    enforce_table_magic_number_(enforce_table_magic_number) {
  }


 public:

  ReadFooterContext(const ReadFooterContext&) = delete;
  ReadFooterContext& operator=(const ReadFooterContext&) = delete;

  static
  Status RequestFooterRead(const FooterReadCallback& footer_cb,
                           RandomAccessFileReader* file, uint64_t file_size,
                           Footer* footer, uint64_t enforce_table_magic_number) {

    if (file_size < Footer::kMinEncodedLength) {
      return Status::Corruption("file is too short to be an sstable");
    }

    size_t read_offset =
      (file_size > Footer::kMaxEncodedLength) ?
      static_cast<size_t>(file_size - Footer::kMaxEncodedLength)
      : 0;

    std::unique_ptr<ReadFooterContext> context(new ReadFooterContext(footer_cb,
        file,
        read_offset, footer,
        enforce_table_magic_number));

    auto iocb = context->GetIOCallback();
    Status s = context->RequestRead(iocb);

    if (s.IsIOPending()) {
      context.release();
      return s;
    }
    return context->OnReadFooterComplete(s, context->GetFooterInput());
  }

  static
  Status ReadFooter(RandomAccessFileReader * file, uint64_t file_size,
                    Footer * footer, uint64_t enforce_table_magic_number) {

    if (file_size < Footer::kMinEncodedLength) {
      return Status::Corruption("file is too short to be an sstable");
    }

    size_t read_offset =
      (file_size > Footer::kMaxEncodedLength)
      ? static_cast<size_t>(file_size - Footer::kMaxEncodedLength)
      : 0;

    ReadFooterContext context(FooterReadCallback(), file, read_offset, footer,
                              enforce_table_magic_number);
    Status s = context.Read();
    return context.OnReadFooterComplete(s, context.GetFooterInput());
  }


 private:

  RandomAccessCallback GetIOCallback() {
    async::CallableFactory<ReadFooterContext, Status, const Status&,
          const Slice&> fac(this);
    return fac.GetCallable<&ReadFooterContext::OnIOCompletion>();
  }

  Status OnIOCompletion(const Status&, const Slice&);

  FooterReadCallback footer_cb_;
  Footer*            footer_;     // Pointer to class to decode to
  Slice              footer_input_;
  uint64_t           enforce_table_magic_number_;
  char               footer_space_[Footer::kMaxEncodedLength];
};

// This class specialized on read an entire block
class ReadBlockContext : protected RandomReadContext {
 public:
  using
  ReadBlockCallback = async::Callable<Status, const Status&, const Slice&>;

  ReadBlockContext(const ReadBlockCallback& client_cb,
                   RandomAccessFileReader* file, ChecksumType checksum_type,
                   bool verify_check_sum, const BlockHandle& handle, Slice* result, char* buf) :
    RandomReadContext(file, handle.offset(), static_cast<size_t>(handle.size())
                      + kBlockTrailerSize,
                      result, buf),
    PERF_METER_INIT(block_read_time),
    checksum_type_(checksum_type),
    verify_checksums_(verify_check_sum) {
  }

  // Expose protected members for the benefit of containing classes
  Status Read() {
    PERF_METER_START(block_read_time);
    return RandomReadContext::Read();
  }

  Status RequestRead(const RandomAccessCallback& iocb) {
    PERF_METER_START(block_read_time);
    return RandomReadContext::RequestRead(iocb);
  }

  // Performs after read tasks in both sync and async cases
  Status OnReadBlockComplete(const Status& s, const Slice&);

  ReadBlockContext(const ReadBlockContext&) = delete;
  ReadBlockContext& operator=(const ReadBlockContext&) = delete;

  ~ReadBlockContext() {
  }

  // Async block read request
  static
  Status RequestBlockRead(const ReadBlockCallback& cb,
                          RandomAccessFileReader* file, const Footer& footer,
                          const ReadOptions& options, const BlockHandle& handle,
                          Slice* contents, /* result of reading */ char* buf) {
    std::unique_ptr<ReadBlockContext> ctx(new ReadBlockContext(cb, file,
                                          footer.checksum(), options.verify_checksums,
                                          handle, contents, buf));
    auto iocb = ctx->GetIOCallback();
    Status s = ctx->RequestRead(iocb);
    if (s.IsIOPending()) {
      ctx.release();
      return s;
    }
    ctx->OnReadBlockComplete(s, *contents);
    return s;
  }

  // Sync version of the API
  static
  Status ReadBlock(RandomAccessFileReader* file, const Footer& footer,
                   const ReadOptions& options, const BlockHandle& handle,
                   Slice* contents, /* result of reading */ char* buf) {
    ReadBlockContext ctx(ReadBlockCallback(), file, footer.checksum(),
                         options.verify_checksums, handle,
                         contents, buf);
    Status s = ctx.Read();
    s = ctx.OnReadBlockComplete(s, *contents);
    return s;
  }

 private:

  // This is invoked only if this class represents
  // the concrete class and is to be destroyed after the callback
  // completes. Derived classes supply their own callback where they
  // dictate their own policies but can reuse the logic of this class
  Status OnIoCompletion(const Status&, const Slice&);

  RandomAccessCallback GetIOCallback() {
    async::CallableFactory<ReadBlockContext, Status, const Status&, const Slice&>
    factory(this);
    return factory.GetCallable<&ReadBlockContext::OnIoCompletion>();
  }

  ReadBlockCallback  client_cb_;
  PERF_METER_DECL(block_read_time);
  ChecksumType       checksum_type_;
  bool               verify_checksums_;
};

class ReadBlockContentsContext {
 public:

  // BlockContents pointer will be populated
  using
  ReadBlockContCallback = async::Callable<Status, const Status&>;

  ReadBlockContentsContext(
    const ReadBlockContCallback& client_cb,
    const Footer& footer,
    const ReadOptions& read_options,
    const BlockHandle& handle, BlockContents* contents,
    const ImmutableCFOptions &ioptions,
    bool decompression_requested,
    const Slice& compression_dict,
    const PersistentCacheOptions& cache_options) :
    client_cb_(client_cb),
    footer_(&footer),
    read_options_(&read_options),
    handle_(handle),
    ioptions_(&ioptions),
    decompression_requested_(decompression_requested),
    compression_dict_(compression_dict),
    cache_options_(&cache_options),
    contents_(contents),
    is_read_block_(false) {
  }

  ~ReadBlockContentsContext() {
    if (is_read_block_) {
      GetReadBlock()->~ReadBlockContext();
    }
  }

  ReadBlockContentsContext(const ReadBlockContentsContext&) = delete;
  ReadBlockContentsContext& operator=(const ReadBlockContentsContext&) = delete;

 public:

  static
  Status RequestContentstRead(
    const ReadBlockContCallback& client_cb_,
    RandomAccessFileReader* file, const Footer& footer,
    const ReadOptions& read_options,
    const BlockHandle& handle, BlockContents* contents,
    const ImmutableCFOptions &ioptions,
    bool decompression_requested,
    const Slice& compression_dict,
    const PersistentCacheOptions& cache_options) {
    std::unique_ptr<ReadBlockContentsContext> context(new ReadBlockContentsContext(
          client_cb_, footer,
          read_options, handle, contents, ioptions, decompression_requested,
          compression_dict, cache_options));
    bool need_decompression = false;
    Status status = context->CheckPersistentCache(need_decompression);

    if (status.ok()) {
      if (need_decompression) {
        return context->OnReadBlockContentsComplete(status, context->result_);
      }
      return status;
    }
    // Proceed with reading the block from disk
    context->ConstructReadBlockContext(file);
    auto iocb = context->GetIOCallback();
    status = context->RequestRead(iocb);
    if (status.IsIOPending()) {
      context.release();
      return status;
    }
    return context->OnReadBlockContentsComplete(status, context->result_);
  }

  static
  Status ReadContents(RandomAccessFileReader* file, const Footer& footer,
                      const ReadOptions& read_options,
                      const BlockHandle& handle, BlockContents* contents,
                      const ImmutableCFOptions &ioptions,
                      bool decompression_requested,
                      const Slice& compression_dict,
                      const PersistentCacheOptions& cache_options) {

    ReadBlockContentsContext context(ReadBlockContCallback(), footer, read_options,
                                     handle, contents, ioptions, decompression_requested,
                                     compression_dict, cache_options);

    bool need_decompression = false;
    Status status = context.CheckPersistentCache(need_decompression);
    if (status.ok()) {
      if (need_decompression) {
        return context.OnReadBlockContentsComplete(status, context.result_);
      }
      return status;
    }
    // Proceed with reading the block from disk
    context.ConstructReadBlockContext(file);
    status = context.Read();
    return context.OnReadBlockContentsComplete(status, context.result_);
  }

 private:

  // Status::OK() or Status::NotFound()
  Status CheckPersistentCache(bool& need_decompression);

  Status OnReadBlockContentsComplete(const Status& s, const Slice& slice);

  Status OnIoCompletion(const Status&, const Slice&);

  RandomAccessCallback GetIOCallback() {
    async::CallableFactory<ReadBlockContentsContext, Status, const Status&, const Slice&>
    fac(this);
    return fac.GetCallable<&ReadBlockContentsContext::OnIoCompletion>();
  }

  Status Read() {
    assert(is_read_block_);
    return GetReadBlock()->Read();
  }

  Status RequestRead(const RandomAccessCallback& iocb) {
    assert(is_read_block_);
    return GetReadBlock()->RequestRead(iocb);
  }

  ReadBlockContext* GetReadBlock() {
    assert(is_read_block_);
    return reinterpret_cast<ReadBlockContext*>(&read_block_);
  }

  void ConstructReadBlockContext(RandomAccessFileReader* reader) {

    // Figure out if we can use in-class buffer
    char* used_buf = nullptr;
    size_t n = GetN();

    if (decompression_requested_ &&
        n + kBlockTrailerSize < DefaultStackBufferSize) {
      used_buf = inclass_buf_;
    } else {
      heap_buf_.reset(new char[n + kBlockTrailerSize]);
      used_buf = heap_buf_.get();
    }

    new (&read_block_) ReadBlockContext(ReadBlockContext::ReadBlockCallback(),
                                        reader,
                                        footer_->checksum(), read_options_->verify_checksums, handle_,
                                        &result_, used_buf);
    is_read_block_ = true;
  }

  size_t GetN() const {
    return static_cast<size_t>(handle_.size());
  }

  ReadBlockContCallback         client_cb_;
  const Footer*                 footer_;
  const ReadOptions*            read_options_;
  BlockHandle                   handle_;
  const ImmutableCFOptions*     ioptions_;
  bool                          decompression_requested_;
  Slice                         compression_dict_;
  const PersistentCacheOptions* cache_options_;
  // This is the out parameter
  BlockContents*                contents_;
  Slice                         result_;
  // We attempt to avoid extra allocation and reserve
  // some space within the class for reading
  bool                          is_read_block_;
  std::unique_ptr<char[]>       heap_buf_;
  char                          inclass_buf_[DefaultStackBufferSize];
  // Construct this only if needed
  std::aligned_storage<sizeof(ReadBlockContext)>::type read_block_;
};

} // namespace async
} // namespace rocksdb
