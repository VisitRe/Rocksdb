//  Copyright (c) Meta Platforms, Inc. and affiliates.
//
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include "util/udt_util.h"

#include <gtest/gtest.h>

#include "db/dbformat.h"
#include "test_util/testharness.h"
#include "test_util/testutil.h"

namespace ROCKSDB_NAMESPACE {
namespace {
static const std::string kTestKeyWithoutTs = "key";
static const std::string kValuePlaceHolder = "value";
}  // namespace

class HandleTimestampSizeDifferenceTest : public testing::Test {
 public:
  HandleTimestampSizeDifferenceTest() {}

  // Test handler used to collect the column family id and user keys contained
  // in a WriteBatch for test verification. And verifies the value part stays
  // the same if it's available.
  class KeyCollector : public WriteBatch::Handler {
   public:
    explicit KeyCollector() {}

    ~KeyCollector() override {}

    Status PutCF(uint32_t cf, const Slice& key, const Slice& value) override {
      if (value.compare(kValuePlaceHolder) != 0) {
        return Status::InvalidArgument();
      }
      return AddKey(cf, key);
    }

    Status DeleteCF(uint32_t cf, const Slice& key) override {
      return AddKey(cf, key);
    }

    Status SingleDeleteCF(uint32_t cf, const Slice& key) override {
      return AddKey(cf, key);
    }

    Status DeleteRangeCF(uint32_t cf, const Slice& begin_key,
                         const Slice& end_key) override {
      Status status = AddKey(cf, begin_key);
      if (!status.ok()) {
        return status;
      }
      return AddKey(cf, end_key);
    }

    Status MergeCF(uint32_t cf, const Slice& key, const Slice& value) override {
      if (value.compare(kValuePlaceHolder) != 0) {
        return Status::InvalidArgument();
      }
      return AddKey(cf, key);
    }

    Status PutBlobIndexCF(uint32_t cf, const Slice& key,
                          const Slice& value) override {
      if (value.compare(kValuePlaceHolder) != 0) {
        return Status::InvalidArgument();
      }
      return AddKey(cf, key);
    }

    Status MarkBeginPrepare(bool) override { return Status::OK(); }

    Status MarkEndPrepare(const Slice&) override { return Status::OK(); }

    Status MarkRollback(const Slice&) override { return Status::OK(); }

    Status MarkCommit(const Slice&) override { return Status::OK(); }

    Status MarkCommitWithTimestamp(const Slice&, const Slice&) override {
      return Status::OK();
    }

    Status MarkNoop(bool) override { return Status::OK(); }

    const std::vector<std::pair<uint32_t, const Slice>>& GetKeys() const {
      return keys_;
    }

   private:
    Status AddKey(uint32_t cf, const Slice& key) {
      keys_.push_back(std::make_pair(cf, key));
      return Status::OK();
    }
    std::vector<std::pair<uint32_t, const Slice>> keys_;
  };

  void CreateKey(std::string* key_buf, size_t ts_sz) {
    if (ts_sz > 0) {
      AppendKeyWithMinTimestamp(key_buf, kTestKeyWithoutTs, ts_sz);
    } else {
      key_buf->assign(kTestKeyWithoutTs);
    }
  }

  void CreateWriteBatch(
      const std::unordered_map<uint32_t, size_t>& ts_sz_for_batch,
      std::unique_ptr<WriteBatch>& batch) {
    for (const auto& [cf_id, ts_sz] : ts_sz_for_batch) {
      std::string key;
      CreateKey(&key, ts_sz);
      ASSERT_OK(
          WriteBatchInternal::Put(batch.get(), cf_id, key, kValuePlaceHolder));
      ASSERT_OK(WriteBatchInternal::Delete(batch.get(), cf_id, key));
      ASSERT_OK(WriteBatchInternal::SingleDelete(batch.get(), cf_id, key));
      ASSERT_OK(WriteBatchInternal::DeleteRange(batch.get(), cf_id, key, key));
      ASSERT_OK(WriteBatchInternal::Merge(batch.get(), cf_id, key,
                                          kValuePlaceHolder));
      ASSERT_OK(WriteBatchInternal::PutBlobIndex(batch.get(), cf_id, key,
                                                 kValuePlaceHolder));
    }
  }

  void CheckSequenceEqual(const WriteBatch& orig_batch,
                          const WriteBatch& new_batch) {
    ASSERT_EQ(WriteBatchInternal::Sequence(&orig_batch),
              WriteBatchInternal::Sequence(&new_batch));
  }
  void CheckCountEqual(const WriteBatch& orig_batch,
                       const WriteBatch& new_batch) {
    ASSERT_EQ(WriteBatchInternal::Count(&orig_batch),
              WriteBatchInternal::Count(&new_batch));
  }

  void VerifyKeys(
      const std::vector<std::pair<uint32_t, const Slice>>& keys_with_ts,
      const std::vector<std::pair<uint32_t, const Slice>>& keys_without_ts,
      size_t ts_sz, std::optional<uint32_t> dropped_cf) {
    ASSERT_EQ(keys_with_ts.size(), keys_without_ts.size());
    const std::string kTsMin(ts_sz, static_cast<unsigned char>(0));
    for (size_t i = 0; i < keys_with_ts.size(); i++) {
      // TimestampRecoveryHandler ignores dropped column family and copy it over
      // as is. Check the keys stay the same.
      if (dropped_cf.has_value() &&
          keys_with_ts[i].first == dropped_cf.value()) {
        ASSERT_EQ(keys_with_ts[i].first, keys_without_ts[i].first);
        ASSERT_EQ(keys_with_ts[i].second, keys_without_ts[i].second);
        continue;
      }
      const Slice& key_with_ts = keys_with_ts[i].second;
      const Slice& key_without_ts = keys_without_ts[i].second;
      ASSERT_TRUE(key_with_ts.starts_with(key_without_ts));
      ASSERT_EQ(key_with_ts.size() - key_without_ts.size(), ts_sz);
      ASSERT_TRUE(key_with_ts.ends_with(kTsMin));
    }
  }

  void CheckContentsWithTimestampStripping(const WriteBatch& orig_batch,
                                           const WriteBatch& new_batch,
                                           size_t ts_sz,
                                           std::optional<uint32_t> dropped_cf) {
    CheckSequenceEqual(orig_batch, new_batch);
    CheckCountEqual(orig_batch, new_batch);
    KeyCollector collector_for_orig_batch;
    ASSERT_OK(orig_batch.Iterate(&collector_for_orig_batch));
    KeyCollector collector_for_new_batch;
    ASSERT_OK(new_batch.Iterate(&collector_for_new_batch));
    VerifyKeys(collector_for_orig_batch.GetKeys(),
               collector_for_new_batch.GetKeys(), ts_sz, dropped_cf);
  }

  void CheckContentsWithTimestampPadding(const WriteBatch& orig_batch,
                                         const WriteBatch& new_batch,
                                         size_t ts_sz) {
    CheckSequenceEqual(orig_batch, new_batch);
    CheckCountEqual(orig_batch, new_batch);
    KeyCollector collector_for_orig_batch;
    ASSERT_OK(orig_batch.Iterate(&collector_for_orig_batch));
    KeyCollector collector_for_new_batch;
    ASSERT_OK(new_batch.Iterate(&collector_for_new_batch));
    VerifyKeys(collector_for_new_batch.GetKeys(),
               collector_for_orig_batch.GetKeys(), ts_sz,
               std::nullopt /* dropped_cf */);
  }
};

TEST_F(HandleTimestampSizeDifferenceTest, AllColumnFamiliesConsistent) {
  std::unordered_map<uint32_t, size_t> running_ts_sz = {{1, sizeof(uint64_t)},
                                                        {2, 0}};
  std::unordered_map<uint32_t, size_t> record_ts_sz = {{1, sizeof(uint64_t)}};
  std::unique_ptr<WriteBatch> batch(new WriteBatch());
  CreateWriteBatch(running_ts_sz, batch);
  const WriteBatch* orig_batch = batch.get();
  bool batch_updated = false;

  // All `check_mode` pass with OK status and `batch` not checked or updated.
  ASSERT_OK(HandleWriteBatchTimestampSizeDifference(
      running_ts_sz, record_ts_sz,
      TimestampSizeConsistencyMode::kVerifyConsistency, batch, &batch_updated));
  ASSERT_EQ(orig_batch, batch.get());
  ASSERT_FALSE(batch_updated);
  ASSERT_OK(HandleWriteBatchTimestampSizeDifference(
      running_ts_sz, record_ts_sz,
      TimestampSizeConsistencyMode::kReconcileInconsistency, batch,
      &batch_updated));
  ASSERT_FALSE(batch_updated);
  ASSERT_EQ(orig_batch, batch.get());
}

TEST_F(HandleTimestampSizeDifferenceTest,
       AllInconsistentColumnFamiliesDropped) {
  std::unordered_map<uint32_t, size_t> running_ts_sz = {{2, 0}};
  std::unordered_map<uint32_t, size_t> record_ts_sz = {{1, sizeof(uint64_t)},
                                                       {3, sizeof(char)}};
  std::unique_ptr<WriteBatch> batch(new WriteBatch());
  CreateWriteBatch(record_ts_sz, batch);
  const WriteBatch* orig_batch = batch.get();
  bool batch_updated = false;

  // All `check_mode` pass with OK status and `batch` not checked or updated.
  ASSERT_OK(HandleWriteBatchTimestampSizeDifference(
      running_ts_sz, record_ts_sz,
      TimestampSizeConsistencyMode::kVerifyConsistency, batch, &batch_updated));
  ASSERT_EQ(orig_batch, batch.get());
  ASSERT_FALSE(batch_updated);
  ASSERT_OK(HandleWriteBatchTimestampSizeDifference(
      running_ts_sz, record_ts_sz,
      TimestampSizeConsistencyMode::kReconcileInconsistency, batch,
      &batch_updated));
  ASSERT_EQ(orig_batch, batch.get());
  ASSERT_FALSE(batch_updated);
}

TEST_F(HandleTimestampSizeDifferenceTest, InvolvedColumnFamiliesConsistent) {
  std::unordered_map<uint32_t, size_t> running_ts_sz = {{1, sizeof(uint64_t)},
                                                        {2, sizeof(char)}};
  std::unordered_map<uint32_t, size_t> record_ts_sz = {{1, sizeof(uint64_t)}};
  std::unique_ptr<WriteBatch> batch(new WriteBatch());
  CreateWriteBatch(record_ts_sz, batch);
  const WriteBatch* orig_batch = batch.get();
  bool batch_updated = false;

  // All `check_mode` pass with OK status and `batch` not updated.
  ASSERT_OK(HandleWriteBatchTimestampSizeDifference(
      running_ts_sz, record_ts_sz,
      TimestampSizeConsistencyMode::kVerifyConsistency, batch, &batch_updated));
  ASSERT_EQ(orig_batch, batch.get());
  ASSERT_FALSE(batch_updated);
  ASSERT_OK(HandleWriteBatchTimestampSizeDifference(
      running_ts_sz, record_ts_sz,
      TimestampSizeConsistencyMode::kReconcileInconsistency, batch,
      &batch_updated));
  ASSERT_EQ(orig_batch, batch.get());
  ASSERT_FALSE(batch_updated);
}

TEST_F(HandleTimestampSizeDifferenceTest,
       InconsistentColumnFamilyNeedsTimestampStripping) {
  std::unordered_map<uint32_t, size_t> running_ts_sz = {{1, 0},
                                                        {2, sizeof(char)}};
  std::unordered_map<uint32_t, size_t> record_ts_sz = {{1, sizeof(uint64_t)}};
  std::unique_ptr<WriteBatch> batch(new WriteBatch());
  CreateWriteBatch(record_ts_sz, batch);
  const WriteBatch* orig_batch = batch.get();
  WriteBatch orig_batch_copy(*batch);
  bool batch_updated = false;

  // kVerifyConsistency doesn't tolerate inconsistency for running column
  // families.
  ASSERT_TRUE(HandleWriteBatchTimestampSizeDifference(
                  running_ts_sz, record_ts_sz,
                  TimestampSizeConsistencyMode::kVerifyConsistency, batch,
                  &batch_updated)
                  .IsInvalidArgument());
  ASSERT_FALSE(batch_updated);

  ASSERT_OK(HandleWriteBatchTimestampSizeDifference(
      running_ts_sz, record_ts_sz,
      TimestampSizeConsistencyMode::kReconcileInconsistency, batch,
      &batch_updated));
  ASSERT_TRUE(batch_updated);
  ASSERT_NE(orig_batch, batch.get());
  CheckContentsWithTimestampStripping(orig_batch_copy, *batch, sizeof(uint64_t),
                                      std::nullopt /* dropped_cf */);
}

TEST_F(HandleTimestampSizeDifferenceTest,
       InconsistentColumnFamilyNeedsTimestampPadding) {
  std::unordered_map<uint32_t, size_t> running_ts_sz = {{1, sizeof(uint64_t)}};
  // Make `record_ts_sz` not contain zero timestamp size entries to follow the
  // behavior of actual WAL log timestamp size record.
  std::unordered_map<uint32_t, size_t> record_ts_sz;
  std::unique_ptr<WriteBatch> batch(new WriteBatch());
  CreateWriteBatch({{1, 0}}, batch);
  const WriteBatch* orig_batch = batch.get();
  WriteBatch orig_batch_copy(*batch);
  bool batch_updated = false;

  // kVerifyConsistency doesn't tolerate inconsistency for running column
  // families.
  ASSERT_TRUE(HandleWriteBatchTimestampSizeDifference(
                  running_ts_sz, record_ts_sz,
                  TimestampSizeConsistencyMode::kVerifyConsistency, batch,
                  &batch_updated)
                  .IsInvalidArgument());
  ASSERT_FALSE(batch_updated);
  ASSERT_OK(HandleWriteBatchTimestampSizeDifference(
      running_ts_sz, record_ts_sz,
      TimestampSizeConsistencyMode::kReconcileInconsistency, batch,
      &batch_updated));
  ASSERT_NE(orig_batch, batch.get());
  ASSERT_TRUE(batch_updated);
  CheckContentsWithTimestampPadding(orig_batch_copy, *batch, sizeof(uint64_t));
}

TEST_F(HandleTimestampSizeDifferenceTest,
       InconsistencyReconcileCopyOverDroppedColumnFamily) {
  std::unordered_map<uint32_t, size_t> running_ts_sz = {{1, 0}};
  std::unordered_map<uint32_t, size_t> record_ts_sz = {{1, sizeof(uint64_t)},
                                                       {2, sizeof(char)}};
  std::unique_ptr<WriteBatch> batch(new WriteBatch());
  CreateWriteBatch(record_ts_sz, batch);
  const WriteBatch* orig_batch = batch.get();
  WriteBatch orig_batch_copy(*batch);
  bool batch_updated = false;

  // kReconcileInconsistency tolerate inconsistency for dropped column family
  // and all related entries copied over to the new WriteBatch.
  ASSERT_OK(HandleWriteBatchTimestampSizeDifference(
      running_ts_sz, record_ts_sz,
      TimestampSizeConsistencyMode::kReconcileInconsistency, batch,
      &batch_updated));
  ASSERT_TRUE(batch_updated);
  ASSERT_NE(orig_batch, batch.get());
  CheckContentsWithTimestampStripping(orig_batch_copy, *batch, sizeof(uint64_t),
                                      std::optional<uint32_t>(2));
}

TEST_F(HandleTimestampSizeDifferenceTest, UnrecoverableInconsistency) {
  std::unordered_map<uint32_t, size_t> running_ts_sz = {{1, sizeof(char)}};
  std::unordered_map<uint32_t, size_t> record_ts_sz = {{1, sizeof(uint64_t)}};
  std::unique_ptr<WriteBatch> batch(new WriteBatch());
  CreateWriteBatch(record_ts_sz, batch);
  bool batch_updated = false;

  ASSERT_TRUE(HandleWriteBatchTimestampSizeDifference(
                  running_ts_sz, record_ts_sz,
                  TimestampSizeConsistencyMode::kVerifyConsistency, batch,
                  &batch_updated)
                  .IsInvalidArgument());
  ASSERT_FALSE(batch_updated);

  ASSERT_TRUE(HandleWriteBatchTimestampSizeDifference(
                  running_ts_sz, record_ts_sz,
                  TimestampSizeConsistencyMode::kReconcileInconsistency, batch,
                  &batch_updated)
                  .IsInvalidArgument());
  ASSERT_FALSE(batch_updated);
}
}  // namespace ROCKSDB_NAMESPACE

int main(int argc, char** argv) {
  ROCKSDB_NAMESPACE::port::InstallStackTraceHandler();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
