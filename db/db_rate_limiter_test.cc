//  Copyright (c) 2022-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include "db/db_test_util.h"
#include "port/stack_trace.h"

namespace ROCKSDB_NAMESPACE {

class DBRateLimiterTest
    : public DBTestBase,
      public ::testing::WithParamInterface<std::tuple<bool, bool, bool>> {
 public:
  DBRateLimiterTest()
      : DBTestBase("db_rate_limiter_test", /*env_do_fsync=*/false),
        use_direct_io_(std::get<0>(GetParam())),
        use_block_cache_(std::get<1>(GetParam())),
        use_readahead_(std::get<2>(GetParam())) {}

  void SetUp() override {
    options_ = GetOptions();
    Reopen(options_);
    for (int i = 0; i < kNumFiles; ++i) {
      for (int j = 0; j < kNumKeysPerFile; ++j) {
        ASSERT_OK(Put(Key(i * kNumKeysPerFile + j), "val"));
      }
      ASSERT_OK(Flush());
    }
    MoveFilesToLevel(1);
  }

  BlockBasedTableOptions GetTableOptions() {
    BlockBasedTableOptions table_options;
    table_options.no_block_cache = !use_block_cache_;
    return table_options;
  }

  ReadOptions GetReadOptions() {
    ReadOptions read_options;
    read_options.priority = Env::IO_USER;
    read_options.readahead_size = use_readahead_ ? kReadaheadBytes : 0;
    return read_options;
  }

  Options GetOptions() {
    Options options = CurrentOptions();
    options.disable_auto_compactions = true;
    options.rate_limiter.reset(NewGenericRateLimiter(
        1 << 20 /* rate_bytes_per_sec */, 100 * 1000 /* refill_period_us */,
        10 /* fairness */, RateLimiter::Mode::kAllIo));
    options.table_factory.reset(NewBlockBasedTableFactory(GetTableOptions()));
    options.use_direct_reads = use_direct_io_;
    return options;
  }

 protected:
  const static int kNumKeysPerFile = 1;
  const static int kNumFiles = 3;
  const static int kReadaheadBytes = 32 << 10;  // 32KB

  Options options_;
  const bool use_direct_io_;
  const bool use_block_cache_;
  const bool use_readahead_;
};

std::string GetTestNameSuffix(
    ::testing::TestParamInfo<std::tuple<bool, bool, bool>> info) {
  std::ostringstream oss;
  if (std::get<0>(info.param)) {
    oss << "DirectIO";
  } else {
    oss << "BufferedIO";
  }
  if (std::get<1>(info.param)) {
    oss << "_BlockCache";
  } else {
    oss << "_NoBlockCache";
  }
  if (std::get<2>(info.param)) {
    oss << "_Readahead";
  } else {
    oss << "_NoReadahead";
  }
  return oss.str();
}

#ifndef ROCKSDB_LITE
INSTANTIATE_TEST_CASE_P(DBRateLimiterTest, DBRateLimiterTest,
                        ::testing::Combine(::testing::Bool(), ::testing::Bool(),
                                           ::testing::Bool()),
                        GetTestNameSuffix);
#else   // ROCKSDB_LITE
// Cannot use direct I/O in lite mode.
INSTANTIATE_TEST_CASE_P(DBRateLimiterTest, DBRateLimiterTest,
                        ::testing::Combine(::testing::Values(false),
                                           ::testing::Bool(),
                                           ::testing::Bool()),
                        GetTestNameSuffix);
#endif  // ROCKSDB_LITE

TEST_P(DBRateLimiterTest, Get) {
  ASSERT_EQ(0, options_.rate_limiter->GetTotalRequests(Env::IO_USER));

  int expected = 0;
  for (int i = 0; i < kNumFiles; ++i) {
    {
      std::string value;
      ASSERT_OK(db_->Get(GetReadOptions(), Key(i * kNumKeysPerFile), &value));
      ++expected;
    }
    ASSERT_EQ(expected, options_.rate_limiter->GetTotalRequests(Env::IO_USER));

    {
      std::string value;
      ASSERT_OK(db_->Get(GetReadOptions(), Key(i * kNumKeysPerFile), &value));
      if (!use_block_cache_) {
        ++expected;
      }
    }
    ASSERT_EQ(expected, options_.rate_limiter->GetTotalRequests(Env::IO_USER));
  }
}

TEST_P(DBRateLimiterTest, Iterator) {
  std::unique_ptr<Iterator> iter(db_->NewIterator(GetReadOptions()));
  ASSERT_EQ(0, options_.rate_limiter->GetTotalRequests(Env::IO_USER));

  int expected = 0;
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    ++expected;
    ASSERT_EQ(expected, options_.rate_limiter->GetTotalRequests(Env::IO_USER));
  }

  for (iter->SeekToLast(); iter->Valid(); iter->Prev()) {
    if (!use_block_cache_) {
      ++expected;
    }
  }
  // Reverse scan does not read evenly (one block per iteration) due to
  // descending seqno ordering, so wait until after the loop to check total.
  ASSERT_EQ(expected, options_.rate_limiter->GetTotalRequests(Env::IO_USER));
}

}  // namespace ROCKSDB_NAMESPACE

int main(int argc, char** argv) {
  ROCKSDB_NAMESPACE::port::InstallStackTraceHandler();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
