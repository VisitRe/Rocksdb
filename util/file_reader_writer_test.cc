//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
#include <vector>
#include "util/file_reader_writer.h"
#include "util/random.h"
#include "util/testharness.h"

namespace rocksdb {

class WritableFileWriterTest : public testing::Test {};

const uint32_t kMb = 1 << 20;

TEST_F(WritableFileWriterTest, RangeSync) {
  class FakeWF : public WritableFile {
   public:
    explicit FakeWF() : size_(0), last_synced_(0) {}
    ~FakeWF() {}

    Status Append(const Slice& data) override {
      size_ += data.size();
      return Status::OK();
    }
    virtual Status Truncate(uint64_t size) override {
      return Status::OK();
    }
    Status Close() override {
      EXPECT_GE(size_, last_synced_ + kMb);
      EXPECT_LT(size_, last_synced_ + 2 * kMb);
      // Make sure random writes generated enough writes.
      EXPECT_GT(size_, 10 * kMb);
      return Status::OK();
    }
    Status Flush() override { return Status::OK(); }
    Status Sync() override { return Status::OK(); }
    Status Fsync() override { return Status::OK(); }
    void SetIOPriority(Env::IOPriority pri) override {}
    uint64_t GetFileSize() override { return size_; }
    void GetPreallocationStatus(size_t* block_size,
                                size_t* last_allocated_block) override {}
    size_t GetUniqueId(char* id, size_t max_size) const override { return 0; }
    Status InvalidateCache(size_t offset, size_t length) override {
      return Status::OK();
    }

   protected:
    Status Allocate(uint64_t offset, uint64_t len) override { return Status::OK(); }
    Status RangeSync(uint64_t offset, uint64_t nbytes) override {
      EXPECT_EQ(offset % 4096, 0u);
      EXPECT_EQ(nbytes % 4096, 0u);

      EXPECT_EQ(offset, last_synced_);
      last_synced_ = offset + nbytes;
      EXPECT_GE(size_, last_synced_ + kMb);
      if (size_ > 2 * kMb) {
        EXPECT_LT(size_, last_synced_ + 2 * kMb);
      }
      return Status::OK();
    }

    uint64_t size_;
    uint64_t last_synced_;
  };

  EnvOptions env_options;
  env_options.bytes_per_sync = kMb;
  unique_ptr<FakeWF> wf(new FakeWF);
  unique_ptr<WritableFileWriter> writer(
      new WritableFileWriter(std::move(wf), env_options));
  Random r(301);
  std::unique_ptr<char[]> large_buf(new char[10 * kMb]);
  for (int i = 0; i < 1000; i++) {
    int skew_limit = (i < 700) ? 10 : 15;
    uint32_t num = r.Skewed(skew_limit) * 100 + r.Uniform(100);
    writer->Append(Slice(large_buf.get(), num));

    // Flush in a chance of 1/10.
    if (r.Uniform(10) == 0) {
      writer->Flush();
    }
  }
  writer->Close();
}

TEST_F(WritableFileWriterTest, AppendStatusReturn) {
  class FakeWF : public WritableFile {
   public:
    explicit FakeWF() : use_direct_io_(false), io_error_(false) {}

    virtual bool use_direct_io() const override { return use_direct_io_; }
    Status Append(const Slice& data) override {
      if (io_error_) {
        return Status::IOError("Fake IO error");
      }
      return Status::OK();
    }
    Status PositionedAppend(const Slice& data, uint64_t) override {
      if (io_error_) {
        return Status::IOError("Fake IO error");
      }
      return Status::OK();
    }
    Status Close() override { return Status::OK(); }
    Status Flush() override { return Status::OK(); }
    Status Sync() override { return Status::OK(); }
    void Setuse_direct_io(bool val) { use_direct_io_ = val; }
    void SetIOError(bool val) { io_error_ = val; }

   protected:
    bool use_direct_io_;
    bool io_error_;
  };
  unique_ptr<FakeWF> wf(new FakeWF());
  wf->Setuse_direct_io(true);
  unique_ptr<WritableFileWriter> writer(
      new WritableFileWriter(std::move(wf), EnvOptions()));

  ASSERT_OK(writer->Append(std::string(2 * kMb, 'a')));

  // Next call to WritableFile::Append() should fail
  dynamic_cast<FakeWF*>(writer->writable_file())->SetIOError(true);
  ASSERT_NOK(writer->Append(std::string(2 * kMb, 'b')));
}

}  // namespace rocksdb

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
