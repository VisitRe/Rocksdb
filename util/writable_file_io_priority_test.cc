//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
#include <algorithm>
#include <vector>

#include "db/db_test_util.h"
#include "env/mock_env.h"
#include "file/line_file_reader.h"
#include "file/random_access_file_reader.h"
#include "file/read_write_util.h"
#include "file/readahead_raf.h"
#include "file/sequence_file_reader.h"
#include "file/writable_file_writer.h"
#include "rocksdb/file_system.h"
#include "test_util/testharness.h"
#include "test_util/testutil.h"
#include "util/crc32c.h"
#include "util/random.h"
#include "utilities/fault_injection_fs.h"

namespace ROCKSDB_NAMESPACE {

class WritableFileWriterIOPriorityTest : public testing::Test {
 public:
  WritableFileWriterIOPriorityTest() = default;
  ~WritableFileWriterIOPriorityTest() = default;
protected:
// This test is to check whether the rate limiter priority can be passed
  // correctly from WritableFileWriter functions to FSWritableFile functions.
  // Assume rate_limiter is not set.
  // There are two major scenarios:
  // 1. When op_rate_limiter_priority parameter in WritableFileWriter functions
  // is the default (Env::IO_TOTAL).
  // 2. When op_rate_limiter_priority parameter in WritableFileWriter
  // functions is NOT the default.
  class FakeWF : public FSWritableFile {
   public:
    // The op_rate_limiter_priority_ is to mock the op_rate_limiter_priority
    // parameter in some WritableFileWriter functions, e.g. Append.
    explicit FakeWF(Env::IOPriority io_priority,
                    Env::IOPriority op_rate_limiter_priority = Env::IO_TOTAL)
        : op_rate_limiter_priority_(op_rate_limiter_priority) {
      SetIOPriority(io_priority);
    }
    ~FakeWF() override {}

    IOStatus Append(const Slice& /*data*/, const IOOptions& options,
                    IODebugContext* /*dbg*/) override {
      CheckRateLimiterPriority(options);
      return IOStatus::OK();
    }
    IOStatus Append(const Slice& data, const IOOptions& options,
                    const DataVerificationInfo& /* verification_info */,
                    IODebugContext* dbg) override {
      return Append(data, options, dbg);
    }
    IOStatus PositionedAppend(const Slice& /*data*/, uint64_t /*offset*/,
                              const IOOptions& options,
                              IODebugContext* /*dbg*/) override {
      CheckRateLimiterPriority(options);
      return IOStatus::OK();
    }
    IOStatus PositionedAppend(
        const Slice& /* data */, uint64_t /* offset */,
        const IOOptions& options,
        const DataVerificationInfo& /* verification_info */,
        IODebugContext* /*dbg*/) override {
      CheckRateLimiterPriority(options);
      return IOStatus::OK();
    }
    IOStatus Truncate(uint64_t /*size*/, const IOOptions& options,
                      IODebugContext* /*dbg*/) override {
      EXPECT_EQ(options.rate_limiter_priority, io_priority_);
      return IOStatus::OK();
    }
    IOStatus Close(const IOOptions& options, IODebugContext* /*dbg*/) override {
      EXPECT_EQ(options.rate_limiter_priority, io_priority_);
      return IOStatus::OK();
    }
    IOStatus Flush(const IOOptions& options, IODebugContext* /*dbg*/) override {
      CheckRateLimiterPriority(options);
      return IOStatus::OK();
    }
    IOStatus Sync(const IOOptions& options, IODebugContext* /*dbg*/) override {
      std::cout << "writer->Sync()" << std::endl;
      EXPECT_EQ(options.rate_limiter_priority, io_priority_);
      return IOStatus::OK();
    }
    IOStatus Fsync(const IOOptions& options, IODebugContext* /*dbg*/) override {
      std::cout << "writer->Fsync()" << std::endl;
      EXPECT_EQ(options.rate_limiter_priority, io_priority_);
      return IOStatus::OK();
    }
    // void SetIOPriority(Env::IOPriority /*pri*/) override {}
    uint64_t GetFileSize(const IOOptions& options,
                         IODebugContext* /*dbg*/) override {
      CheckRateLimiterPriority(options);
      return 0;
    }
    void GetPreallocationStatus(size_t* /*block_size*/,
                                size_t* /*last_allocated_block*/) override {}
    size_t GetUniqueId(char* /*id*/, size_t /*max_size*/) const override {
      return 0;
    }
    IOStatus InvalidateCache(size_t /*offset*/, size_t /*length*/) override {
      return IOStatus::OK();
    }

    IOStatus Allocate(uint64_t /*offset*/, uint64_t /*len*/,
                      const IOOptions& options,
                      IODebugContext* /*dbg*/) override {
      CheckRateLimiterPriority(options);
      return IOStatus::OK();
    }
    IOStatus RangeSync(uint64_t /*offset*/, uint64_t /*nbytes*/,
                       const IOOptions& options,
                       IODebugContext* /*dbg*/) override {
      EXPECT_EQ(options.rate_limiter_priority, io_priority_);
      return IOStatus::OK();
    }

    void PrepareWrite(size_t /*offset*/, size_t /*len*/,
                      const IOOptions& options,
                      IODebugContext* /*dbg*/) override {
      CheckRateLimiterPriority(options);
    }

   protected:
    void CheckRateLimiterPriority(const IOOptions& options) {
      // The expected rate limiter priority is decided
      // by WritableFileWriter::DecideRateLimiterPriority.
      if (io_priority_ == Env::IO_TOTAL) {
        EXPECT_EQ(options.rate_limiter_priority, op_rate_limiter_priority_);
      } else if (op_rate_limiter_priority_ == Env::IO_TOTAL) {
        EXPECT_EQ(options.rate_limiter_priority, io_priority_);
      } else {
        EXPECT_EQ(options.rate_limiter_priority, op_rate_limiter_priority_);
      }
    }

    Env::IOPriority op_rate_limiter_priority_;
  };

};

TEST_F(WritableFileWriterIOPriorityTest, RateLimiterPriority) {
  // When op_rate_limiter_priority parameter in WritableFileWriter functions
  // is the default (Env::IO_TOTAL).
  FileOptions file_options;
  Env::IOPriority op_rate_limiter_priority = Env::IO_TOTAL;
  std::unique_ptr<FakeWF> wf{
      new FakeWF(Env::IO_HIGH, op_rate_limiter_priority)};
  std::unique_ptr<WritableFileWriter> writer(
      new WritableFileWriter(std::move(wf), "" /* don't care */, file_options));
  writer->Append(Slice("abc"));
  writer->Pad(10);
  writer->Flush();
  writer->Close();

  // writer->SyncInternal(false);
  // std::cout << "SyncInternal(false)" << std::endl;
  // writer->SyncInternal(true);
  // writer->WriteBuffered("abc", 1, Env::IO_TOTAL);
  // writer->WriteBufferedWithChecksum("abc", 1, Env::IO_TOTAL);
  // writer->WriteDirect(Env::IO_TOTAL);
  // writer->WriteDirectWithChecksum(Env::IO_TOTAL);
  // writer->RangeSync(1, 10);

  // When op_rate_limiter_priority parameter in WritableFileWriter functions
  // is NOT the default (Env::IO_TOTAL).
  // op_rate_limiter_priority = Env::IO_MID;
  // wf.reset(new FakeWF(Env::IO_USER, op_rate_limiter_priority));
  // writer.reset(
  //     new WritableFileWriter(std::move(wf), "" /* don't care */, file_options));
  // writer->Append(Slice("abc"), 0, op_rate_limiter_priority);
  // writer->Pad(10, op_rate_limiter_priority);
  // writer->Flush(op_rate_limiter_priority);
  // writer->Close();
  // writer->SyncInternal(false);
  // writer->SyncInternal(true);
  // writer->WriteBuffered("abc", 1, op_rate_limiter_priority);
  // writer->WriteBufferedWithChecksum("abc", 1, op_rate_limiter_priority);
  // writer->WriteDirect(op_rate_limiter_priority);
  // writer->WriteDirectWithChecksum(op_rate_limiter_priority);
  // writer->RangeSync(1, 10);
}

TEST_F(WritableFileWriterIOPriorityTest, RateLimiterPriority2) {
  // When op_rate_limiter_priority parameter in WritableFileWriter functions
  // is NOT the default (Env::IO_TOTAL).
  FileOptions file_options;
  Env::IOPriority op_rate_limiter_priority = Env::IO_MID;
  std::unique_ptr<FakeWF> wf{
      new FakeWF(Env::IO_USER, op_rate_limiter_priority)};
  std::unique_ptr<WritableFileWriter> writer(
      new WritableFileWriter(std::move(wf), "" /* don't care */, file_options));
  // writer->Append(Slice("abc"), 0, op_rate_limiter_priority);
  // writer->Pad(10, op_rate_limiter_priority);
  // writer->Flush(op_rate_limiter_priority);
  // writer->Close();
  // writer->SyncInternal(false);
  // writer->SyncInternal(true);
  // writer->WriteBuffered("abc", 1, op_rate_limiter_priority);
  // writer->WriteBufferedWithChecksum("abc", 1, op_rate_limiter_priority);
  // writer->WriteDirect(op_rate_limiter_priority);
  // writer->WriteDirectWithChecksum(op_rate_limiter_priority);
  // writer->RangeSync(1, 10);
}

}  // namespace ROCKSDB_NAMESPACE

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
