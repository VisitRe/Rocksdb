//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include "file/random_access_file_reader.h"

#include <algorithm>

#include "file/file_util.h"
#include "port/port.h"
#include "port/stack_trace.h"
#include "rocksdb/file_system.h"
#include "test_util/sync_point.h"
#include "test_util/testharness.h"
#include "test_util/testutil.h"
#include "util/random.h"

namespace ROCKSDB_NAMESPACE {

class RandomAccessFileReaderTest : public testing::Test {
 public:
  void SetUp() override {
    SetupSyncPointsToMockDirectIO();
    env_ = Env::Default();
    fs_ = FileSystem::Default();
    test_dir_ = test::PerThreadDBPath("random_access_file_reader_test");
    ASSERT_OK(fs_->CreateDir(test_dir_, IOOptions(), nullptr));
    ComputeAndSetAlignment();
  }

  void TearDown() override { EXPECT_OK(DestroyDir(env_, test_dir_)); }

  void Write(const std::string& fname, const std::string& content) {
    std::unique_ptr<FSWritableFile> f;
    ASSERT_OK(fs_->NewWritableFile(Path(fname), FileOptions(), &f, nullptr));
    ASSERT_OK(f->Append(content, IOOptions(), nullptr));
    ASSERT_OK(f->Close(IOOptions(), nullptr));
  }

  void Read(const std::string& fname, const FileOptions& opts,
                std::unique_ptr<RandomAccessFileReader>* reader) {
    std::string fpath = Path(fname);
    std::unique_ptr<FSRandomAccessFile> f;
    ASSERT_OK(fs_->NewRandomAccessFile(fpath, opts, &f, nullptr));
    (*reader).reset(new RandomAccessFileReader(std::move(f), fpath, env_));
  }

  void AssertResult(const std::string& content,
                    const std::vector<FSReadRequest>& reqs) {
    for (const auto& r : reqs) {
      ASSERT_OK(r.status);
      ASSERT_EQ(r.len, r.result.size());
      ASSERT_EQ(content.substr(r.offset, r.len), r.result.ToString());
    }
  }

  size_t alignment() const { return alignment_; }

 private:
  Env* env_;
  std::shared_ptr<FileSystem> fs_;
  std::string test_dir_;
  size_t alignment_;

  std::string Path(const std::string& fname) {
    return test_dir_ + "/" + fname;
  }

  void ComputeAndSetAlignment() {
    std::string f = "get_alignment";
    Write(f, "");
    std::unique_ptr<RandomAccessFileReader> r;
    Read(f, FileOptions(), &r);
    alignment_ = r->file()->GetRequiredBufferAlignment();
    EXPECT_OK(fs_->DeleteFile(Path(f), IOOptions(), nullptr));
  }
};

// Skip the following tests in lite mode since direct I/O is unsupported.
#ifndef ROCKSDB_LITE

TEST_F(RandomAccessFileReaderTest, ReadDirectIO) {
  std::string fname = "read-direct-io";
  Random rand(0);
  std::string content = rand.RandomString(static_cast<int>(alignment()));
  Write(fname, content);

  FileOptions opts;
  opts.use_direct_reads = true;
  std::unique_ptr<RandomAccessFileReader> r;
  Read(fname, opts, &r);
  ASSERT_TRUE(r->use_direct_io());

  size_t offset = alignment() / 2;
  size_t len = alignment() / 3;
  Slice result;
  AlignedBuf buf;
  for (bool for_compaction : {true, false}) {
    ASSERT_OK(r->Read(IOOptions(), offset, len, &result, nullptr, &buf,
                      for_compaction));
    ASSERT_EQ(result.ToString(), content.substr(offset, len));
  }
}

TEST_F(RandomAccessFileReaderTest, MultiReadDirectIO) {
  std::vector<FSReadRequest> aligned_reqs;
  ROCKSDB_NAMESPACE::SyncPoint::GetInstance()->SetCallBack(
      "RandomAccessFileReader::MultiRead:AlignedReqs", [&](void* reqs) {
        // Copy reqs, since it's allocated on stack inside MultiRead, which will
        // be deallocated after MultiRead returns.
        aligned_reqs = *reinterpret_cast<std::vector<FSReadRequest>*>(reqs);
      });
  ROCKSDB_NAMESPACE::SyncPoint::GetInstance()->EnableProcessing();

  // Creates a file with 3 pages.
  std::string fname = "multi-read-direct-io";
  Random rand(0);
  std::string content = rand.RandomString(3 * static_cast<int>(alignment()));
  Write(fname, content);

  FileOptions opts;
  opts.use_direct_reads = true;
  std::unique_ptr<RandomAccessFileReader> r;
  Read(fname, opts, &r);
  ASSERT_TRUE(r->use_direct_io());

  {
    // Reads 2 blocks in the 1st page.
    // The results should be SharedSlices of the same underlying buffer.
    //
    // Illustration (each x is a 1/4 page)
    // First page: xxxx
    // 1st block:  x
    // 2nd block:    xx
    FSReadRequest r0;
    r0.offset = 0;
    r0.len = alignment() / 4;
    r0.scratch = nullptr;

    FSReadRequest r1;
    r1.offset = alignment() / 2;
    r1.len = alignment() / 2;
    r1.scratch = nullptr;

    std::vector<FSReadRequest> reqs;
    reqs.push_back(std::move(r0));
    reqs.push_back(std::move(r1));
    AlignedBuf aligned_buf;
    ASSERT_OK(
        r->MultiRead(IOOptions(), reqs.data(), reqs.size(), &aligned_buf));

    AssertResult(content, reqs);

    // Reads the first page internally.
    ASSERT_EQ(aligned_reqs.size(), 1);
    const FSReadRequest& aligned_r = aligned_reqs[0];
    ASSERT_EQ(aligned_r.offset, 0);
    ASSERT_EQ(aligned_r.len, alignment());
  }

  {
    // Reads 3 blocks:
    // 1st block in the 1st page;
    // 2nd block from the middle of the 1st page to the middle of the 2nd page;
    // 3rd block in the 2nd page.
    // The results should be SharedSlices of the same underlying buffer.
    //
    // Illustration (each x is a 1/4 page)
    // 2 pages:   xxxxxxxx
    // 1st block: x
    // 2nd block:   xxxx
    // 3rd block:        x
    FSReadRequest r0;
    r0.offset = 0;
    r0.len = alignment() / 4;
    r0.scratch = nullptr;

    FSReadRequest r1;
    r1.offset = alignment() / 2;
    r1.len = alignment();
    r1.scratch = nullptr;

    FSReadRequest r2;
    r2.offset = 2 * alignment() - alignment() / 4;
    r2.len = alignment() / 4;
    r2.scratch = nullptr;

    std::vector<FSReadRequest> reqs;
    reqs.push_back(std::move(r0));
    reqs.push_back(std::move(r1));
    reqs.push_back(std::move(r2));
    AlignedBuf aligned_buf;
    ASSERT_OK(
        r->MultiRead(IOOptions(), reqs.data(), reqs.size(), &aligned_buf));

    AssertResult(content, reqs);

    // Reads the first two pages in one request internally.
    ASSERT_EQ(aligned_reqs.size(), 1);
    const FSReadRequest& aligned_r = aligned_reqs[0];
    ASSERT_EQ(aligned_r.offset, 0);
    ASSERT_EQ(aligned_r.len, 2 * alignment());
  }

  {
    // Reads 3 blocks:
    // 1st block in the middle of the 1st page;
    // 2nd block in the middle of the 2nd page;
    // 3rd block in the middle of the 3rd page.
    // The results should be SharedSlices of the same underlying buffer.
    //
    // Illustration (each x is a 1/4 page)
    // 3 pages:   xxxxxxxxxxxx
    // 1st block:  xx
    // 2nd block:      xx
    // 3rd block:          xx
    FSReadRequest r0;
    r0.offset = alignment() / 4;
    r0.len = alignment() / 2;
    r0.scratch = nullptr;

    FSReadRequest r1;
    r1.offset = alignment() + alignment() / 4;
    r1.len = alignment() / 2;
    r1.scratch = nullptr;

    FSReadRequest r2;
    r2.offset = 2 * alignment() + alignment() / 4;
    r2.len = alignment() / 2;
    r2.scratch = nullptr;

    std::vector<FSReadRequest> reqs;
    reqs.push_back(std::move(r0));
    reqs.push_back(std::move(r1));
    reqs.push_back(std::move(r2));
    AlignedBuf aligned_buf;
    ASSERT_OK(
        r->MultiRead(IOOptions(), reqs.data(), reqs.size(), &aligned_buf));

    AssertResult(content, reqs);

    // Reads the first 3 pages in one request internally.
    ASSERT_EQ(aligned_reqs.size(), 1);
    const FSReadRequest& aligned_r = aligned_reqs[0];
    ASSERT_EQ(aligned_r.offset, 0);
    ASSERT_EQ(aligned_r.len, 3 * alignment());
  }

  {
    // Reads 2 blocks:
    // 1st block in the middle of the 1st page;
    // 2nd block in the middle of the 3rd page.
    // The results are two different buffers.
    //
    // Illustration (each x is a 1/4 page)
    // 3 pages:   xxxxxxxxxxxx
    // 1st block:  xx
    // 2nd block:          xx
    FSReadRequest r0;
    r0.offset = alignment() / 4;
    r0.len = alignment() / 2;
    r0.scratch = nullptr;

    FSReadRequest r1;
    r1.offset = 2 * alignment() + alignment() / 4;
    r1.len = alignment() / 2;
    r1.scratch = nullptr;

    std::vector<FSReadRequest> reqs;
    reqs.push_back(std::move(r0));
    reqs.push_back(std::move(r1));
    AlignedBuf aligned_buf;
    ASSERT_OK(
        r->MultiRead(IOOptions(), reqs.data(), reqs.size(), &aligned_buf));

    AssertResult(content, reqs);

    // Reads the 1st and 3rd pages in two requests internally.
    ASSERT_EQ(aligned_reqs.size(), 2);
    const FSReadRequest& aligned_r0 = aligned_reqs[0];
    const FSReadRequest& aligned_r1 = aligned_reqs[1];
    ASSERT_EQ(aligned_r0.offset, 0);
    ASSERT_EQ(aligned_r0.len, alignment());
    ASSERT_EQ(aligned_r1.offset, 2 * alignment());
    ASSERT_EQ(aligned_r1.len, alignment());
  }

  ROCKSDB_NAMESPACE::SyncPoint::GetInstance()->DisableProcessing();
  ROCKSDB_NAMESPACE::SyncPoint::GetInstance()->ClearAllCallBacks();
}

#endif  // ROCKSDB_LITE

TEST(FSReadRequest, Align) {
  FSReadRequest r;
  r.offset = 2000;
  r.len = 2000;
  r.scratch = nullptr;

  FSReadRequest aligned_r = Align(r, 1024);
  ASSERT_EQ(aligned_r.offset, 1024);
  ASSERT_EQ(aligned_r.len, 3072);
}

TEST(FSReadRequest, TryMerge) {
  // reverse means merging dest into src.
  for (bool reverse : {true, false}) {
    {
      // dest: [ ]
      //  src:      [ ]
      FSReadRequest dest;
      dest.offset = 0;
      dest.len = 10;
      dest.scratch = nullptr;

      FSReadRequest src;
      src.offset = 15;
      src.len = 10;
      src.scratch = nullptr;

      if (reverse) std::swap(dest, src);
      ASSERT_FALSE(TryMerge(&dest, src));
    }

    {
      // dest: [ ]
      //  src:   [ ]
      FSReadRequest dest;
      dest.offset = 0;
      dest.len = 10;
      dest.scratch = nullptr;

      FSReadRequest src;
      src.offset = 10;
      src.len = 10;
      src.scratch = nullptr;

      if (reverse) std::swap(dest, src);
      ASSERT_TRUE(TryMerge(&dest, src));
      ASSERT_EQ(dest.offset, 0);
      ASSERT_EQ(dest.len, 20);
    }

    {
      // dest: [    ]
      //  src:   [    ]
      FSReadRequest dest;
      dest.offset = 0;
      dest.len = 10;
      dest.scratch = nullptr;

      FSReadRequest src;
      src.offset = 5;
      src.len = 10;
      src.scratch = nullptr;

      if (reverse) std::swap(dest, src);
      ASSERT_TRUE(TryMerge(&dest, src));
      ASSERT_EQ(dest.offset, 0);
      ASSERT_EQ(dest.len, 15);
    }

    {
      // dest: [    ]
      //  src:   [  ]
      FSReadRequest dest;
      dest.offset = 0;
      dest.len = 10;
      dest.scratch = nullptr;

      FSReadRequest src;
      src.offset = 5;
      src.len = 5;
      src.scratch = nullptr;

      if (reverse) std::swap(dest, src);
      ASSERT_TRUE(TryMerge(&dest, src));
      ASSERT_EQ(dest.offset, 0);
      ASSERT_EQ(dest.len, 10);
    }

    {
      // dest: [     ]
      //  src:   [ ]
      FSReadRequest dest;
      dest.offset = 0;
      dest.len = 10;
      dest.scratch = nullptr;

      FSReadRequest src;
      src.offset = 5;
      src.len = 1;
      src.scratch = nullptr;

      if (reverse) std::swap(dest, src);
      ASSERT_TRUE(TryMerge(&dest, src));
      ASSERT_EQ(dest.offset, 0);
      ASSERT_EQ(dest.len, 10);
    }

    {
      // dest: [ ]
      //  src: [ ]
      FSReadRequest dest;
      dest.offset = 0;
      dest.len = 10;
      dest.scratch = nullptr;

      FSReadRequest src;
      src.offset = 0;
      src.len = 10;
      src.scratch = nullptr;

      if (reverse) std::swap(dest, src);
      ASSERT_TRUE(TryMerge(&dest, src));
      ASSERT_EQ(dest.offset, 0);
      ASSERT_EQ(dest.len, 10);
    }

    {
      // dest: [   ]
      //  src: [ ]
      FSReadRequest dest;
      dest.offset = 0;
      dest.len = 10;
      dest.scratch = nullptr;

      FSReadRequest src;
      src.offset = 0;
      src.len = 5;
      src.scratch = nullptr;

      if (reverse) std::swap(dest, src);
      ASSERT_TRUE(TryMerge(&dest, src));
      ASSERT_EQ(dest.offset, 0);
      ASSERT_EQ(dest.len, 10);
    }
  }
}

}  // namespace ROCKSDB_NAMESPACE

int main(int argc, char** argv) {
  ROCKSDB_NAMESPACE::port::InstallStackTraceHandler();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
