//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#ifndef ROCKSDB_LITE

#include <algorithm>
#include <cctype>
#include <iostream>

#include "rocksdb/env_encryption.h"
#include "rocksdb/extension_loader.h"
#include "rocksdb/options.h"
#include "util/aligned_buffer.h"
#include "util/coding.h"
#include "util/random.h"
#include "util/string_util.h"

#endif

namespace rocksdb {

const std::string EncryptionConsts::kTypeProvider = "provider";
const std::string EncryptionConsts::kTypeBlockCipher = "block_cipher";
const std::string EncryptionConsts::kEnvEncrypted = "encrypted";
const std::string EncryptionConsts::kProviderCTR = "CTR";
const std::string EncryptionConsts::kCipherROT13 = "ROT13";
const std::string EncryptionConsts::kEnvEncryptedPropPrefix = "rocksdb.encrypted.";

#ifndef ROCKSDB_LITE

class EncryptedSequentialFile : public SequentialFile {
  private:
    std::unique_ptr<SequentialFile> file_;
    std::unique_ptr<BlockAccessCipherStream> stream_;
    uint64_t offset_;
    size_t prefixLength_;

     public:
  // Default ctor. Given underlying sequential file is supposed to be at
  // offset == prefixLength.
  EncryptedSequentialFile(SequentialFile* f, BlockAccessCipherStream* s, size_t prefixLength)
      : file_(f), stream_(s), offset_(prefixLength), prefixLength_(prefixLength) {
  }

  // Read up to "n" bytes from the file.  "scratch[0..n-1]" may be
  // written by this routine.  Sets "*result" to the data that was
  // read (including if fewer than "n" bytes were successfully read).
  // May set "*result" to point at data in "scratch[0..n-1]", so
  // "scratch[0..n-1]" must be live when "*result" is used.
  // If an error was encountered, returns a non-OK status.
  //
  // REQUIRES: External synchronization
  virtual Status Read(size_t n, Slice* result, char* scratch) override {
    assert(scratch);
    Status status = file_->Read(n, result, scratch);
    if (!status.ok()) {
      return status;
    }
    status = stream_->Decrypt(offset_, (char*)result->data(), result->size());
    offset_ += result->size(); // We've already ready data from disk, so update offset_ even if decryption fails.
    return status;
  }

  // Skip "n" bytes from the file. This is guaranteed to be no
  // slower that reading the same data, but may be faster.
  //
  // If end of file is reached, skipping will stop at the end of the
  // file, and Skip will return OK.
  //
  // REQUIRES: External synchronization
  virtual Status Skip(uint64_t n) override {
    auto status = file_->Skip(n);
    if (!status.ok()) {
      return status;
    }
    offset_ += n;
    return status;
  }

  // Indicates the upper layers if the current SequentialFile implementation
  // uses direct IO.
  virtual bool use_direct_io() const override { 
    return file_->use_direct_io(); 
  }

  // Use the returned alignment value to allocate
  // aligned buffer for Direct I/O
  virtual size_t GetRequiredBufferAlignment() const override { 
    return file_->GetRequiredBufferAlignment(); 
  }

  // Remove any kind of caching of data from the offset to offset+length
  // of this file. If the length is 0, then it refers to the end of file.
  // If the system is not caching the file contents, then this is a noop.
  virtual Status InvalidateCache(size_t offset, size_t length) override {
    return file_->InvalidateCache(offset + prefixLength_, length);
  }

  // Positioned Read for direct I/O
  // If Direct I/O enabled, offset, n, and scratch should be properly aligned
  virtual Status PositionedRead(uint64_t offset, size_t n, Slice* result, char* scratch) override {
    assert(scratch);
    offset += prefixLength_; // Skip prefix
    auto status = file_->PositionedRead(offset, n, result, scratch);
    if (!status.ok()) {
      return status;
    }
    offset_ = offset + result->size();
    status = stream_->Decrypt(offset, (char*)result->data(), result->size());
    return status;
  }

};

// A file abstraction for randomly reading the contents of a file.
class EncryptedRandomAccessFile : public RandomAccessFile {
  private:
    std::unique_ptr<RandomAccessFile> file_;
    std::unique_ptr<BlockAccessCipherStream> stream_;
    size_t prefixLength_;

 public:
  EncryptedRandomAccessFile(RandomAccessFile* f, BlockAccessCipherStream* s, size_t prefixLength)
    : file_(f), stream_(s), prefixLength_(prefixLength) { }

  // Read up to "n" bytes from the file starting at "offset".
  // "scratch[0..n-1]" may be written by this routine.  Sets "*result"
  // to the data that was read (including if fewer than "n" bytes were
  // successfully read).  May set "*result" to point at data in
  // "scratch[0..n-1]", so "scratch[0..n-1]" must be live when
  // "*result" is used.  If an error was encountered, returns a non-OK
  // status.
  //
  // Safe for concurrent use by multiple threads.
  // If Direct I/O enabled, offset, n, and scratch should be aligned properly.
  virtual Status Read(uint64_t offset, size_t n, Slice* result, char* scratch) const override {
    assert(scratch);
    offset += prefixLength_;
    auto status = file_->Read(offset, n, result, scratch);
    if (!status.ok()) {
      return status;
    }
    status = stream_->Decrypt(offset, (char*)result->data(), result->size());
    return status;
  }

  // Readahead the file starting from offset by n bytes for caching.
  virtual Status Prefetch(uint64_t offset, size_t n) override {
    //return Status::OK();
    return file_->Prefetch(offset + prefixLength_, n);
  }

  // Tries to get an unique ID for this file that will be the same each time
  // the file is opened (and will stay the same while the file is open).
  // Furthermore, it tries to make this ID at most "max_size" bytes. If such an
  // ID can be created this function returns the length of the ID and places it
  // in "id"; otherwise, this function returns 0, in which case "id"
  // may not have been modified.
  //
  // This function guarantees, for IDs from a given environment, two unique ids
  // cannot be made equal to each other by adding arbitrary bytes to one of
  // them. That is, no unique ID is the prefix of another.
  //
  // This function guarantees that the returned ID will not be interpretable as
  // a single varint.
  //
  // Note: these IDs are only valid for the duration of the process.
  virtual size_t GetUniqueId(char* id, size_t max_size) const override {
    return file_->GetUniqueId(id, max_size);
  };

  virtual void Hint(AccessPattern pattern) override {
    file_->Hint(pattern);
  }

  // Indicates the upper layers if the current RandomAccessFile implementation
  // uses direct IO.
  virtual bool use_direct_io() const override {
     return file_->use_direct_io(); 
  }

  // Use the returned alignment value to allocate
  // aligned buffer for Direct I/O
  virtual size_t GetRequiredBufferAlignment() const override { 
    return file_->GetRequiredBufferAlignment(); 
  }

  // Remove any kind of caching of data from the offset to offset+length
  // of this file. If the length is 0, then it refers to the end of file.
  // If the system is not caching the file contents, then this is a noop.
  virtual Status InvalidateCache(size_t offset, size_t length) override {
    return file_->InvalidateCache(offset + prefixLength_, length);
  }
};

// A file abstraction for sequential writing.  The implementation
// must provide buffering since callers may append small fragments
// at a time to the file.
class EncryptedWritableFile : public WritableFileWrapper {
  private:
    std::unique_ptr<WritableFile> file_;
    std::unique_ptr<BlockAccessCipherStream> stream_;
    size_t prefixLength_;

 public:
  // Default ctor. Prefix is assumed to be written already.
  EncryptedWritableFile(WritableFile* f, BlockAccessCipherStream* s, size_t prefixLength)
    : WritableFileWrapper(f), file_(f), stream_(s), prefixLength_(prefixLength) { }

  Status Append(const Slice& data) override { 
    AlignedBuffer buf;
    Status status;
    Slice dataToAppend(data); 
    if (data.size() > 0) {
      auto offset = file_->GetFileSize(); // size including prefix
      // Encrypt in cloned buffer
      buf.Alignment(GetRequiredBufferAlignment());
      buf.AllocateNewBuffer(data.size());
      memmove(buf.BufferStart(), data.data(), data.size());
      status = stream_->Encrypt(offset, buf.BufferStart(), data.size());
      if (!status.ok()) {
        return status;
      }
      dataToAppend = Slice(buf.BufferStart(), data.size());
    }
    status = file_->Append(dataToAppend); 
    if (!status.ok()) {
      return status;
    }
    return status;
  }

  Status PositionedAppend(const Slice& data, uint64_t offset) override {
    AlignedBuffer buf;
    Status status;
    Slice dataToAppend(data); 
    offset += prefixLength_;
    if (data.size() > 0) {
      // Encrypt in cloned buffer
      buf.Alignment(GetRequiredBufferAlignment());
      buf.AllocateNewBuffer(data.size());
      memmove(buf.BufferStart(), data.data(), data.size());
      status = stream_->Encrypt(offset, buf.BufferStart(), data.size());
      if (!status.ok()) {
        return status;
      }
      dataToAppend = Slice(buf.BufferStart(), data.size());
    }
    status = file_->PositionedAppend(dataToAppend, offset);
    if (!status.ok()) {
      return status;
    }
    return status;
  }

  // Indicates the upper layers if the current WritableFile implementation
  // uses direct IO.
  virtual bool use_direct_io() const override { return file_->use_direct_io(); }

  // Use the returned alignment value to allocate
  // aligned buffer for Direct I/O
  virtual size_t GetRequiredBufferAlignment() const override { return file_->GetRequiredBufferAlignment(); } 

    /*
   * Get the size of valid data in the file.
   */
  virtual uint64_t GetFileSize() override {
    return file_->GetFileSize() - prefixLength_;
  }

  // Truncate is necessary to trim the file to the correct size
  // before closing. It is not always possible to keep track of the file
  // size due to whole pages writes. The behavior is undefined if called
  // with other writes to follow.
  virtual Status Truncate(uint64_t size) override {
    return file_->Truncate(size + prefixLength_);
  }

    // Remove any kind of caching of data from the offset to offset+length
  // of this file. If the length is 0, then it refers to the end of file.
  // If the system is not caching the file contents, then this is a noop.
  // This call has no effect on dirty pages in the cache.
  virtual Status InvalidateCache(size_t offset, size_t length) override {
    return file_->InvalidateCache(offset + prefixLength_, length);
  }

  // Sync a file range with disk.
  // offset is the starting byte of the file range to be synchronized.
  // nbytes specifies the length of the range to be synchronized.
  // This asks the OS to initiate flushing the cached data to disk,
  // without waiting for completion.
  // Default implementation does nothing.
  virtual Status RangeSync(uint64_t offset, uint64_t nbytes) override { 
    return file_->RangeSync(offset + prefixLength_, nbytes);
  }

  // PrepareWrite performs any necessary preparation for a write
  // before the write actually occurs.  This allows for pre-allocation
  // of space on devices where it can result in less file
  // fragmentation and/or less waste from over-zealous filesystem
  // pre-allocation.
  virtual void PrepareWrite(size_t offset, size_t len) override {
    file_->PrepareWrite(offset + prefixLength_, len);
  }

  // Pre-allocates space for a file.
  virtual Status Allocate(uint64_t offset, uint64_t len) override {
    return file_->Allocate(offset + prefixLength_, len);
  }
};

// A file abstraction for random reading and writing.
class EncryptedRandomRWFile : public RandomRWFile {
  private:
    std::unique_ptr<RandomRWFile> file_;
    std::unique_ptr<BlockAccessCipherStream> stream_;
    size_t prefixLength_;

 public:
  EncryptedRandomRWFile(RandomRWFile* f, BlockAccessCipherStream* s, size_t prefixLength)
    : file_(f), stream_(s), prefixLength_(prefixLength) {}

  // Indicates if the class makes use of direct I/O
  // If false you must pass aligned buffer to Write()
  virtual bool use_direct_io() const override { return file_->use_direct_io(); }

  // Use the returned alignment value to allocate
  // aligned buffer for Direct I/O
  virtual size_t GetRequiredBufferAlignment() const override { 
    return file_->GetRequiredBufferAlignment(); 
  }

  // Write bytes in `data` at  offset `offset`, Returns Status::OK() on success.
  // Pass aligned buffer when use_direct_io() returns true.
  virtual Status Write(uint64_t offset, const Slice& data) override {
    AlignedBuffer buf;
    Status status;
    Slice dataToWrite(data); 
    offset += prefixLength_;
    if (data.size() > 0) {
      // Encrypt in cloned buffer
      buf.Alignment(GetRequiredBufferAlignment());
      buf.AllocateNewBuffer(data.size());
      memmove(buf.BufferStart(), data.data(), data.size());
      status = stream_->Encrypt(offset, buf.BufferStart(), data.size());
      if (!status.ok()) {
        return status;
      }
      dataToWrite = Slice(buf.BufferStart(), data.size());
    }
    status = file_->Write(offset, dataToWrite);
    return status;
  }

  // Read up to `n` bytes starting from offset `offset` and store them in
  // result, provided `scratch` size should be at least `n`.
  // Returns Status::OK() on success.
  virtual Status Read(uint64_t offset, size_t n, Slice* result, char* scratch) const override { 
    assert(scratch);
    offset += prefixLength_;
    auto status = file_->Read(offset, n, result, scratch);
    if (!status.ok()) {
      return status;
    }
    status = stream_->Decrypt(offset, (char*)result->data(), result->size());
    return status;
  }

  virtual Status Flush() override {
    return file_->Flush();
  }

  virtual Status Sync() override {
    return file_->Sync();
  }

  virtual Status Fsync() override { 
    return file_->Fsync();
  }

  virtual Status Close() override {
    return file_->Close();
  }
};

static const std::string EncryptedProviderNameProp =
  EncryptionConsts::kEnvEncryptedPropPrefix + "env.provider.name";
static const std::string EncryptedProviderProp =
  EncryptionConsts::kEnvEncryptedPropPrefix + "env.provider";

static Status NewEncryptionProvider(const DBOptions & dbOpts,
				    const ColumnFamilyOptions * cfOpts,
				    const std::string & name,
				    std::shared_ptr<EncryptionProvider> *result) {
  Status s = Status::OK();
  if (! result->get() || result->get()->Name() != name) {
    std::shared_ptr<EncryptionProvider> provider;
    s = dbOpts.NewSharedExtension(EncryptionConsts::kTypeProvider,
				  name, cfOpts, &provider);
    if (s.ok()) {
      *result = provider;
    }
  }
  return s;
}

// EncryptedEnv implements an Env wrapper that adds encryption to files stored on disk.
class EncryptedEnv : public EnvWrapper {
 public:
  EncryptedEnv(Env* base_env, const std::shared_ptr<EncryptionProvider> &provider)
    : EnvWrapper(base_env, EncryptionConsts::kEnvEncryptedPropPrefix) {
    provider_ = provider;
  }

  const char *Name() const override {
    return EncryptionConsts::kEnvEncrypted.c_str();
  }

  virtual Status SetOption(const std::string & name,
			   const std::string & value,
			   bool ignore_unknown_options,
			   bool input_strings_escaped) override {
    if (! provider_) {
      return EnvWrapper::SetOption(name, value,
				   ignore_unknown_options,
				   input_strings_escaped);
    } else {
      Status s = EnvWrapper::SetOption(name, value, false, input_strings_escaped);
      if (s.IsInvalidArgument()) {
	s = provider_->SetOption(name, value,
				 ignore_unknown_options, input_strings_escaped);
      }
      return s;
    }
  }
  virtual Status SetOption(const std::string & name,
			   const std::string & value,
			   const DBOptions & dbOpts,
			   bool ignore_unknown_options,
			   bool input_strings_escaped) override {
    return EnvWrapper::SetOption(name, value, dbOpts, ignore_unknown_options, input_strings_escaped);
  }
  
  virtual Status SetOption(const std::string & name,
			   const std::string & value,
			   const DBOptions & dbOpts,
			   const ColumnFamilyOptions *cfOpts,
			   bool ignore_unknown_options,
			   bool input_strings_escaped) override {
    if (name == EncryptedProviderNameProp) {
      return NewEncryptionProvider(dbOpts, cfOpts, value, &provider_);
    } else if (name == EncryptedProviderProp) {
      return Status::OK(); // MJR: TODO: Extract provider name and properties from value
    } else if (! provider_) { // No provider, run the super SetOption
      Status s = EnvWrapper::SetOption(name, value, dbOpts, cfOpts,
				       ignore_unknown_options,
				       input_strings_escaped);
      return s;
    } else {
      Status s = EnvWrapper::SetOption(name, value, dbOpts, cfOpts, false,
					       input_strings_escaped);
      if (s.IsInvalidArgument()) {
	s =  provider_->SetOption(name, value, dbOpts, cfOpts,
				  ignore_unknown_options,
				  input_strings_escaped);
      }
      return s;
    }
  }

  virtual Status SanitizeOptions(const DBOptions & dbOpts) const override {
    Status status = EnvWrapper::SanitizeOptions(dbOpts);
    if (! status.ok()) {
      return status;
    } else if (provider_) {
      return provider_->SanitizeOptions(dbOpts);
    } else {
      return Status::InvalidArgument("Empty Provider", Name());
    }
  }
  
  // NewSequentialFile opens a file for sequential reading.
  virtual Status NewSequentialFile(const std::string& fname,
                                   std::unique_ptr<SequentialFile>* result,
                                   const EnvOptions& options) override {
    result->reset();
    if (options.use_mmap_reads) {
      return Status::InvalidArgument();
    }
    // Open file using underlying Env implementation
    std::unique_ptr<SequentialFile> underlying;
    auto status = EnvWrapper::NewSequentialFile(fname, &underlying, options);
    if (!status.ok()) {
      return status;
    }
    // Read prefix (if needed)
    AlignedBuffer prefixBuf;
    Slice prefixSlice;
    size_t prefixLength = provider_->GetPrefixLength();
    if (prefixLength > 0) {
      // Read prefix 
      prefixBuf.Alignment(underlying->GetRequiredBufferAlignment());
      prefixBuf.AllocateNewBuffer(prefixLength);
      status = underlying->Read(prefixLength, &prefixSlice, prefixBuf.BufferStart());
      if (!status.ok()) {
        return status;
      }
    }
    // Create cipher stream
    std::unique_ptr<BlockAccessCipherStream> stream;
    status = provider_->CreateCipherStream(fname, options, prefixSlice, &stream);
    if (!status.ok()) {
      return status;
    }
    (*result) = std::unique_ptr<SequentialFile>(new EncryptedSequentialFile(underlying.release(), stream.release(), prefixLength));
    return Status::OK();
  }

  // NewRandomAccessFile opens a file for random read access.
  virtual Status NewRandomAccessFile(const std::string& fname,
                                     unique_ptr<RandomAccessFile>* result,
                                     const EnvOptions& options) override {
    result->reset();
    if (options.use_mmap_reads) {
      return Status::InvalidArgument();
    }
    // Open file using underlying Env implementation
    std::unique_ptr<RandomAccessFile> underlying;
    auto status = EnvWrapper::NewRandomAccessFile(fname, &underlying, options);
    if (!status.ok()) {
      return status;
    }
    // Read prefix (if needed)
    AlignedBuffer prefixBuf;
    Slice prefixSlice;
    size_t prefixLength = provider_->GetPrefixLength();
    if (prefixLength > 0) {
      // Read prefix 
      prefixBuf.Alignment(underlying->GetRequiredBufferAlignment());
      prefixBuf.AllocateNewBuffer(prefixLength);
      status = underlying->Read(0, prefixLength, &prefixSlice, prefixBuf.BufferStart());
      if (!status.ok()) {
        return status;
      }
    }
    // Create cipher stream
    std::unique_ptr<BlockAccessCipherStream> stream;
    status = provider_->CreateCipherStream(fname, options, prefixSlice, &stream);
    if (!status.ok()) {
      return status;
    }
    (*result) = std::unique_ptr<RandomAccessFile>(new EncryptedRandomAccessFile(underlying.release(), stream.release(), prefixLength));
    return Status::OK();
  }
  
  // NewWritableFile opens a file for sequential writing.
  virtual Status NewWritableFile(const std::string& fname,
                                 unique_ptr<WritableFile>* result,
                                 const EnvOptions& options) override {
    result->reset();
    if (options.use_mmap_writes) {
      return Status::InvalidArgument();
    }
    // Open file using underlying Env implementation
    std::unique_ptr<WritableFile> underlying;
    Status status = EnvWrapper::NewWritableFile(fname, &underlying, options);
    if (!status.ok()) {
      return status;
    }
    // Initialize & write prefix (if needed)
    AlignedBuffer prefixBuf;
    Slice prefixSlice;
    size_t prefixLength = provider_->GetPrefixLength();
    if (prefixLength > 0) {
      // Initialize prefix 
      prefixBuf.Alignment(underlying->GetRequiredBufferAlignment());
      prefixBuf.AllocateNewBuffer(prefixLength);
      provider_->CreateNewPrefix(fname, prefixBuf.BufferStart(), prefixLength);
      prefixSlice = Slice(prefixBuf.BufferStart(), prefixLength);
      // Write prefix 
      status = underlying->Append(prefixSlice);
      if (!status.ok()) {
        return status;
      }
    }
    // Create cipher stream
    std::unique_ptr<BlockAccessCipherStream> stream;
    status = provider_->CreateCipherStream(fname, options, prefixSlice, &stream);
    if (!status.ok()) {
      return status;
    }
    (*result) = std::unique_ptr<WritableFile>(new EncryptedWritableFile(underlying.release(), stream.release(), prefixLength));
    return Status::OK();
  }

  // Create an object that writes to a new file with the specified
  // name.  Deletes any existing file with the same name and creates a
  // new file.  On success, stores a pointer to the new file in
  // *result and returns OK.  On failure stores nullptr in *result and
  // returns non-OK.
  //
  // The returned file will only be accessed by one thread at a time.
  virtual Status ReopenWritableFile(const std::string& fname,
                                   unique_ptr<WritableFile>* result,
                                   const EnvOptions& options) override {
    result->reset();
    if (options.use_mmap_writes) {
      return Status::InvalidArgument();
    }
    // Open file using underlying Env implementation
    std::unique_ptr<WritableFile> underlying;
    Status status = EnvWrapper::ReopenWritableFile(fname, &underlying, options);
    if (!status.ok()) {
      return status;
    }
    // Initialize & write prefix (if needed)
    AlignedBuffer prefixBuf;
    Slice prefixSlice;
    size_t prefixLength = provider_->GetPrefixLength();
    if (prefixLength > 0) {
      // Initialize prefix 
      prefixBuf.Alignment(underlying->GetRequiredBufferAlignment());
      prefixBuf.AllocateNewBuffer(prefixLength);
      provider_->CreateNewPrefix(fname, prefixBuf.BufferStart(), prefixLength);
      prefixSlice = Slice(prefixBuf.BufferStart(), prefixLength);
      // Write prefix 
      status = underlying->Append(prefixSlice);
      if (!status.ok()) {
        return status;
      }
    }
    // Create cipher stream
    std::unique_ptr<BlockAccessCipherStream> stream;
    status = provider_->CreateCipherStream(fname, options, prefixSlice, &stream);
    if (!status.ok()) {
      return status;
    }
    (*result) = std::unique_ptr<WritableFile>(new EncryptedWritableFile(underlying.release(), stream.release(), prefixLength));
    return Status::OK();
  }

  // Reuse an existing file by renaming it and opening it as writable.
  virtual Status ReuseWritableFile(const std::string& fname,
                                   const std::string& old_fname,
                                   unique_ptr<WritableFile>* result,
                                   const EnvOptions& options) override {
    result->reset();
    if (options.use_mmap_writes) {
      return Status::InvalidArgument();
    }
    // Open file using underlying Env implementation
    std::unique_ptr<WritableFile> underlying;
    Status status = EnvWrapper::ReuseWritableFile(fname, old_fname, &underlying, options);
    if (!status.ok()) {
      return status;
    }
    // Initialize & write prefix (if needed)
    AlignedBuffer prefixBuf;
    Slice prefixSlice;
    size_t prefixLength = provider_->GetPrefixLength();
    if (prefixLength > 0) {
      // Initialize prefix 
      prefixBuf.Alignment(underlying->GetRequiredBufferAlignment());
      prefixBuf.AllocateNewBuffer(prefixLength);
      provider_->CreateNewPrefix(fname, prefixBuf.BufferStart(), prefixLength);
      prefixSlice = Slice(prefixBuf.BufferStart(), prefixLength);
      // Write prefix 
      status = underlying->Append(prefixSlice);
      if (!status.ok()) {
        return status;
      }
    }
    // Create cipher stream
    std::unique_ptr<BlockAccessCipherStream> stream;
    status = provider_->CreateCipherStream(fname, options, prefixSlice, &stream);
    if (!status.ok()) {
      return status;
    }
    (*result) = std::unique_ptr<WritableFile>(new EncryptedWritableFile(underlying.release(), stream.release(), prefixLength));
    return Status::OK();
  }

  // Open `fname` for random read and write, if file doesn't exist the file
  // will be created.  On success, stores a pointer to the new file in
  // *result and returns OK.  On failure returns non-OK.
  //
  // The returned file will only be accessed by one thread at a time.
  virtual Status NewRandomRWFile(const std::string& fname,
                                 unique_ptr<RandomRWFile>* result,
                                 const EnvOptions& options) override {
    result->reset();
    if (options.use_mmap_reads || options.use_mmap_writes) {
      return Status::InvalidArgument();
    }
    // Check file exists
    bool isNewFile = !FileExists(fname).ok();

    // Open file using underlying Env implementation
    std::unique_ptr<RandomRWFile> underlying;
    Status status = EnvWrapper::NewRandomRWFile(fname, &underlying, options);
    if (!status.ok()) {
      return status;
    }
    // Read or Initialize & write prefix (if needed)
    AlignedBuffer prefixBuf;
    Slice prefixSlice;
    size_t prefixLength = provider_->GetPrefixLength();
    if (prefixLength > 0) {
      prefixBuf.Alignment(underlying->GetRequiredBufferAlignment());
      prefixBuf.AllocateNewBuffer(prefixLength);
      if (!isNewFile) {
        // File already exists, read prefix
        status = underlying->Read(0, prefixLength, &prefixSlice, prefixBuf.BufferStart());
        if (!status.ok()) {
          return status;
        }
      } else {
        // File is new, initialize & write prefix 
        provider_->CreateNewPrefix(fname, prefixBuf.BufferStart(), prefixLength);
        prefixSlice = Slice(prefixBuf.BufferStart(), prefixLength);
        // Write prefix 
        status = underlying->Write(0, prefixSlice);
        if (!status.ok()) {
          return status;
        }
      }
    }
    // Create cipher stream
    std::unique_ptr<BlockAccessCipherStream> stream;
    status = provider_->CreateCipherStream(fname, options, prefixSlice, &stream);
    if (!status.ok()) {
      return status;
    }
    (*result) = std::unique_ptr<RandomRWFile>(new EncryptedRandomRWFile(underlying.release(), stream.release(), prefixLength));
    return Status::OK();
  }

    // Store in *result the attributes of the children of the specified directory.
  // In case the implementation lists the directory prior to iterating the files
  // and files are concurrently deleted, the deleted files will be omitted from
  // result.
  // The name attributes are relative to "dir".
  // Original contents of *results are dropped.
  // Returns OK if "dir" exists and "*result" contains its children.
  //         NotFound if "dir" does not exist, the calling process does not have
  //                  permission to access "dir", or if "dir" is invalid.
  //         IOError if an IO Error was encountered
  virtual Status GetChildrenFileAttributes(const std::string& dir, std::vector<FileAttributes>* result) override {
    auto status = EnvWrapper::GetChildrenFileAttributes(dir, result);
    if (!status.ok()) {
      return status;
    }
    size_t prefixLength = provider_->GetPrefixLength();
    for (auto it = std::begin(*result); it!=std::end(*result); ++it) {
      assert(it->size_bytes >= prefixLength);
      it->size_bytes -= prefixLength;
    }
    return Status::OK();
 }

  // Store the size of fname in *file_size.
  virtual Status GetFileSize(const std::string& fname, uint64_t* file_size) override {
    auto status = EnvWrapper::GetFileSize(fname, file_size);
    if (!status.ok()) {
      return status;
    }
    size_t prefixLength = provider_->GetPrefixLength();
    assert(*file_size >= prefixLength);
    *file_size -= prefixLength;
    return Status::OK();    
  }

 private:
  std::shared_ptr<EncryptionProvider> provider_;
};


// Returns an Env that encrypts data when stored on disk and decrypts data when 
// read from disk.
Env* NewEncryptedEnv(Env* base_env, const std::shared_ptr<EncryptionProvider> & provider) {
  return new EncryptedEnv(base_env, provider);
}

static ExtensionLoader::FactoryFunction EncryptedEnvFactory =
  ExtensionLoader::Default()->RegisterFactory(
				 Env::kTypeEnvironment,
				 EncryptionConsts::kEnvEncrypted,
				 [](const std::string &,
				    const DBOptions & dbOpts,
				    const ColumnFamilyOptions *,
				    std::unique_ptr<Extension> * guard) {
				   guard->reset();
				   return new EncryptedEnv(dbOpts.env, nullptr);
				 });

// Encrypt one or more (partial) blocks of data at the file offset.
// Length of data is given in dataSize.
Status BlockAccessCipherStream::Encrypt(uint64_t fileOffset, char *data, size_t dataSize) {
  // Calculate block index
  auto blockSize = BlockSize();
  uint64_t blockIndex = fileOffset / blockSize;
  size_t blockOffset = fileOffset % blockSize;
  unique_ptr<char[]> blockBuffer;

  std::string scratch;
  AllocateScratch(scratch);

  // Encrypt individual blocks.
  while (1) {
    char *block = data;
    size_t n = std::min(dataSize, blockSize - blockOffset);
    if (n != blockSize) {
      // We're not encrypting a full block. 
      // Copy data to blockBuffer
      if (!blockBuffer.get()) {
        // Allocate buffer 
        blockBuffer = unique_ptr<char[]>(new char[blockSize]);
      }
      block = blockBuffer.get();
      // Copy plain data to block buffer 
      memmove(block + blockOffset, data, n);
    }
    auto status = EncryptBlock(blockIndex, block, (char*)scratch.data());
    if (!status.ok()) {
      return status;
    }
    if (block != data) {
      // Copy encrypted data back to `data`.
      memmove(data, block + blockOffset, n);
    }
    dataSize -= n;
    if (dataSize == 0) {
      return Status::OK();
    }
    data += n;
    blockOffset = 0;
    blockIndex++;
  }
}

// Decrypt one or more (partial) blocks of data at the file offset.
// Length of data is given in dataSize.
Status BlockAccessCipherStream::Decrypt(uint64_t fileOffset, char *data, size_t dataSize) {
  // Calculate block index
  auto blockSize = BlockSize();
  uint64_t blockIndex = fileOffset / blockSize;
  size_t blockOffset = fileOffset % blockSize;
  unique_ptr<char[]> blockBuffer;

  std::string scratch;
  AllocateScratch(scratch);

  // Decrypt individual blocks.
  while (1) {
    char *block = data;
    size_t n = std::min(dataSize, blockSize - blockOffset);
    if (n != blockSize) {
      // We're not decrypting a full block. 
      // Copy data to blockBuffer
      if (!blockBuffer.get()) {
        // Allocate buffer 
        blockBuffer = unique_ptr<char[]>(new char[blockSize]);
      }
      block = blockBuffer.get();
      // Copy encrypted data to block buffer 
      memmove(block + blockOffset, data, n);
    }
    auto status = DecryptBlock(blockIndex, block, (char*)scratch.data());
    if (!status.ok()) {
      return status;
    }
    if (block != data) {
      // Copy decrypted data back to `data`.
      memmove(data, block + blockOffset, n);
    }
    dataSize -= n;
    if (dataSize == 0) {
      return Status::OK();
    }
    data += n;
    blockOffset = 0;
    blockIndex++;
  }
}

static const std::string kROT13BlockSizeProp =
  EncryptionConsts::kEnvEncryptedPropPrefix + "cipher.rot13.blocksize";
  
// Implements a BlockCipher using ROT13.
//
// Note: This is a sample implementation of BlockCipher, 
// it is NOT considered safe and should NOT be used in production.
class ROT13BlockCipher : public BlockCipher {
private: 
  size_t blockSize_;
public:
  ROT13BlockCipher(size_t blockSize) 
    : blockSize_(blockSize) {}
  virtual ~ROT13BlockCipher() {};
  virtual const char *Name() const override {
    return EncryptionConsts::kCipherROT13.c_str();
  }

  virtual Status SetOption(const std::string & name,
			   const std::string & value,
			   bool ignore_unknown_options,
			   bool input_strings_escaped) override {
    if (name == kROT13BlockSizeProp) {
      blockSize_ = ParseSizeT(trim(value));
      return Status::OK();
    } else {
      return BlockCipher::SetOption(name, value,
				    ignore_unknown_options, input_strings_escaped);
    }
  }

  virtual Status SanitizeOptions(const DBOptions & dbOpts) const override {
    if (blockSize_ == 0) {
      return Status::InvalidArgument("Cipher block size must be > 0");
    } else {
      return BlockCipher::SanitizeOptions(dbOpts);
    }
  }
  
  // BlockSize returns the size of each block supported by this cipher stream.
  virtual size_t BlockSize() override { return blockSize_; }
  
  // Encrypt a block of data.
  // Length of data is equal to BlockSize().
  virtual Status Encrypt(char *data) override {
    for (size_t i = 0; i < blockSize_; ++i) {
      data[i] += 13;
    }
    return Status::OK();
  }
  
  // Decrypt a block of data.
  // Length of data is equal to BlockSize().
  virtual Status Decrypt(char *data) override {
    return Encrypt(data);
  }
};

static ExtensionLoader::FactoryFunction ROT13BlockCipherFactory =
  ExtensionLoader::Default()->RegisterFactory(
				 EncryptionConsts::kTypeBlockCipher,
				 EncryptionConsts::kCipherROT13,
				 [](const std::string &,
				    const DBOptions &,
				    const ColumnFamilyOptions *,
				    std::unique_ptr<Extension> * guard) {
				   guard->reset(new ROT13BlockCipher(0));
				   return guard->get();
				 });

// CTRCipherStream implements BlockAccessCipherStream using an 
// Counter operations mode. 
// See https://en.wikipedia.org/wiki/Block_cipher_mode_of_operation
//
// Note: This is a possible implementation of BlockAccessCipherStream, 
// it is considered suitable for use.
class CTRCipherStream final : public BlockAccessCipherStream {
private:
  std::shared_ptr<BlockCipher> cipher_;
  std::string iv_;
  uint64_t initialCounter_;
public:
  CTRCipherStream(std::shared_ptr<BlockCipher> & c, const char *iv, uint64_t initialCounter) 
    : cipher_(c), iv_(iv, c->BlockSize()), initialCounter_(initialCounter) {};
  virtual ~CTRCipherStream() {};
  
  // BlockSize returns the size of each block supported by this cipher stream.
  virtual size_t BlockSize() override { return cipher_->BlockSize(); }
  
protected:
  // Allocate scratch space which is passed to EncryptBlock/DecryptBlock.
  virtual void AllocateScratch(std::string& scratch) override {
    auto blockSize = cipher_->BlockSize();
    scratch.reserve(blockSize);
  }

  // Encrypt a block of data at the given block index.
  // Length of data is equal to BlockSize();
  virtual Status EncryptBlock(uint64_t blockIndex, char *data, char *scratch) override {
    // Create nonce + counter
    auto blockSize = cipher_->BlockSize();
    memmove(scratch, iv_.data(), blockSize);
    EncodeFixed64(scratch, blockIndex + initialCounter_);
    
    // Encrypt nonce+counter 
    auto status = cipher_->Encrypt(scratch);
    if (!status.ok()) {
      return status;
    }
    
    // XOR data with ciphertext.
    for (size_t i = 0; i < blockSize; i++) {
      data[i] = data[i] ^ scratch[i];
    }
    return Status::OK();
  }    

  // Decrypt a block of data at the given block index.
  // Length of data is equal to BlockSize();
  virtual Status DecryptBlock(uint64_t blockIndex, char *data, char *scratch) override {
    // For CTR decryption & encryption are the same 
    return EncryptBlock(blockIndex, data, scratch);
  }
};

  
// decodeCTRParameters decodes the initial counter & IV from the given
// (plain text) prefix.
static void decodeCTRParameters(const char *prefix, size_t blockSize, uint64_t &initialCounter, Slice &iv) {
  // First block contains 64-bit initial counter
  initialCounter = DecodeFixed64(prefix);
  // Second block contains IV
  iv = Slice(prefix + blockSize, blockSize);
}
  
// This encryption provider uses a CTR cipher stream, with a given block cipher 
// and IV.
//
// Note: This is a possible implementation of EncryptionProvider, 
// it is considered suitable for use, provided a safe BlockCipher is used.
static const std::string CTRCipherNameProp =
  EncryptionConsts::kEnvEncryptedPropPrefix + "provider.ctr.cipher.name";
static const std::string CTRCipherProp =
  EncryptionConsts::kEnvEncryptedPropPrefix + "provider.ctr.cipher";

static Status NewBlockCipher(const DBOptions & dbOpts,
			     const ColumnFamilyOptions * cfOpts,
			     const std::string & name,
			     std::shared_ptr<BlockCipher> *result) {
  Status s = Status::OK();
  if (! result->get() || result->get()->Name() != name) {
    std::shared_ptr<BlockCipher> cipher;
    s = dbOpts.NewSharedExtension(EncryptionConsts::kTypeBlockCipher,
				  name, cfOpts, &cipher);
    if (s.ok()) {
      *result = cipher;
    }
  }
  return s;
}
  
class CTREncryptionProvider : public EncryptionProvider {
private:
  std::shared_ptr<BlockCipher> cipher_;
protected:
  const static size_t defaultPrefixLength = 4096;
  
public:
  CTREncryptionProvider(const std::shared_ptr<BlockCipher> & c) 
    : cipher_(c) {};
  virtual ~CTREncryptionProvider() {}
  virtual const char *Name() const override {
    return EncryptionConsts::kProviderCTR.c_str();
  }

  virtual Status SanitizeOptions(const DBOptions & dbOpts) const override {
    Status status = EncryptionProvider::SanitizeOptions(dbOpts);
    if (! status.ok()) {
      return status;
    } else if (cipher_) {
      return cipher_->SanitizeOptions(dbOpts);
    } else {
      return Status::InvalidArgument("Block Cipher not configured");
    }
  }

  virtual Status SetOption(const std::string & name,
			   const std::string & value,
			   bool ignore_unknown_options,
			   bool input_strings_escaped) override {
    if (! cipher_) {
      return EncryptionProvider::SetOption(name, value,
					   ignore_unknown_options,
					   input_strings_escaped);
    } else {
      Status s = EncryptionProvider::SetOption(name, value, false,
					       input_strings_escaped);
      if (s.IsInvalidArgument()) {
	s =  cipher_->SetOption(name, value,
				ignore_unknown_options,
				input_strings_escaped);
      }
      return s;
    }
  }
  
  virtual Status SetOption(const std::string & name,
			   const std::string & value,
			   const DBOptions & dbOpts,
			   const ColumnFamilyOptions *cfOpts,
			   bool ignore_unknown_options,
			   bool input_strings_escaped) override {

    if (name == CTRCipherNameProp) {
      return NewBlockCipher(dbOpts, cfOpts, value, &cipher_);
    } else if (name == CTRCipherProp) {
      return Status::OK(); // MJR: TODO: Extract cipher name and properties from value
    } else if (! cipher_) { // No cipher, run the super SetOption
      Status s = EncryptionProvider::SetOption(name, value, dbOpts, cfOpts,
					   ignore_unknown_options,
					   input_strings_escaped);
      return s;
    } else {
      Status s = EncryptionProvider::SetOption(name, value, dbOpts, cfOpts, false,
					       input_strings_escaped);
      if (s.IsInvalidArgument()) {
	s =  cipher_->SetOption(name, value, dbOpts, cfOpts,
				ignore_unknown_options,
				input_strings_escaped);
      }
      return s;
    }
  }
  
  // GetPrefixLength returns the length of the prefix that is added to every file
  // and used for storing encryption options.
  // For optimal performance, the prefix length should be a multiple of 
  // the page size.
  virtual size_t GetPrefixLength() override {
    return defaultPrefixLength;
  }

  // CreateNewPrefix initialized an allocated block of prefix memory 
  // for a new file.
  virtual Status CreateNewPrefix(const std::string& /*fname */,
				 char *prefix, size_t prefixLength) override {
    // Create & seed rnd.
    Random rnd((uint32_t)Env::Default()->NowMicros());
    // Fill entire prefix block with random values.
    for (size_t i = 0; i < prefixLength; i++) {
      prefix[i] = rnd.Uniform(256) & 0xFF;
    }
    // Take random data to extract initial counter & IV
    auto blockSize = cipher_->BlockSize();
    uint64_t initialCounter;
    Slice prefixIV;
    decodeCTRParameters(prefix, blockSize, initialCounter, prefixIV);
    
    // Now populate the rest of the prefix, starting from the third block.
    PopulateSecretPrefixPart(prefix + (2 * blockSize), prefixLength - (2 * blockSize), blockSize);
    
    // Encrypt the prefix, starting from block 2 (leave block 0, 1 with initial counter & IV unencrypted)
    CTRCipherStream cipherStream(cipher_, prefixIV.data(), initialCounter);
    auto status = cipherStream.Encrypt(0, prefix + (2 * blockSize), prefixLength - (2 * blockSize));
    if (!status.ok()) {
      return status;
    }
    return Status::OK();
  }

  // CreateCipherStream creates a block access cipher stream for a file given
  // given name and options.
  virtual Status CreateCipherStream(
			     const std::string& fname,
			     const EnvOptions& options,
			     Slice& prefix,
			     unique_ptr<BlockAccessCipherStream>* result) override {
    // Read plain text part of prefix.
    auto blockSize = cipher_->BlockSize();
    uint64_t initialCounter;
    Slice iv;
    decodeCTRParameters(prefix.data(), blockSize, initialCounter, iv);
    
    // Decrypt the encrypted part of the prefix, starting from block 2 (block 0, 1 with initial counter & IV are unencrypted)
    CTRCipherStream cipherStream(cipher_, iv.data(), initialCounter);
    auto status = cipherStream.Decrypt(0, (char*)prefix.data() + (2 * blockSize), prefix.size() - (2 * blockSize));
    if (!status.ok()) {
      return status;
    }
    
    // Create cipher stream 
    return CreateCipherStreamFromPrefix(fname, options, initialCounter, iv, prefix, result);
  }

protected:
  // PopulateSecretPrefixPart initializes the data into a new prefix block 
  // that will be encrypted. This function will store the data in plain text. 
  // It will be encrypted later (before written to disk).
  // Returns the amount of space (starting from the start of the prefix)
  // that has been initialized.
  virtual size_t PopulateSecretPrefixPart(char * /*prefix */,
					  size_t /* prefixLength */,
					  size_t /*blockSize */) {
    // Nothing to do here, put in custom data in override when needed.
    return 0;
  }
  
  // CreateCipherStreamFromPrefix creates a block access cipher stream for a file given
  // given name and options. The given prefix is already decrypted.
  virtual Status CreateCipherStreamFromPrefix(
 		             const std::string& /* fname */,
			     const EnvOptions&  /* options */,
			     uint64_t initialCounter,
			     const Slice& iv,
			     const Slice& /* prefix */,
			     unique_ptr<BlockAccessCipherStream>* result) {
    (*result) = unique_ptr<BlockAccessCipherStream>(new CTRCipherStream(cipher_, iv.data(), initialCounter));
    return Status::OK();
  }
};

static ExtensionLoader::FactoryFunction CTREncryptionProviderFactory =
  ExtensionLoader::Default()->RegisterFactory(
				 EncryptionConsts::kTypeProvider,
				 EncryptionConsts::kProviderCTR,
				 [](const std::string &,
				    const DBOptions &,
				    const ColumnFamilyOptions *,
				    std::unique_ptr<Extension> * guard) {
				   std::shared_ptr<BlockCipher> cipher;
				   guard->reset(new CTREncryptionProvider(cipher));
				   return guard->get();
				 });

Status NewEncryptionProvider(const std::shared_ptr<BlockCipher> & cipher,
			       std::shared_ptr<EncryptionProvider> * provider) {
  provider->reset(new CTREncryptionProvider(cipher));
  return Status::OK();
}

  
  
#endif // ROCKSDB_LITE

}  // namespace rocksdb
