//  Copyright (c) Meta Platforms, Inc. and affiliates. All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#ifdef ROCKSDB_LITE
#include <cstdio>

int main(int /*argc*/, char** /*argv*/) {
  fprintf(stderr, "SKIPPED as Transactions are not supported in LITE mode\n");
  return 0;
}
#else  // ROCKSDB_LITE
#include <cassert>

#include "util/cast_util.h"
#include "utilities/transactions/transaction_test.h"

namespace ROCKSDB_NAMESPACE {
INSTANTIATE_TEST_CASE_P(
    Unsupported, TimestampedSnapshotWithTsSanityCheck,
    ::testing::Values(
        std::make_tuple(false, false, WRITE_PREPARED, kOrderedWrite),
        std::make_tuple(false, true, WRITE_PREPARED, kUnorderedWrite),
        std::make_tuple(false, false, WRITE_UNPREPARED, kOrderedWrite)));

INSTANTIATE_TEST_CASE_P(WriteCommitted, TransactionTest,
                        ::testing::Combine(::testing::Bool(), ::testing::Bool(),
                                           ::testing::Values(WRITE_COMMITTED),
                                           ::testing::Values(kOrderedWrite)));

namespace {
// Not thread-safe. Caller needs to provide external synchronization.
class TsCheckingTxnNotifier : public TransactionNotifier {
 public:
  explicit TsCheckingTxnNotifier() = default;

  ~TsCheckingTxnNotifier() override {}

  void SnapshotCreated(const Snapshot* new_snapshot) override {
    assert(new_snapshot);
    if (prev_snapshot_seq_ != kMaxSequenceNumber) {
      assert(prev_snapshot_seq_ <= new_snapshot->GetSequenceNumber());
    }
    prev_snapshot_seq_ = new_snapshot->GetSequenceNumber();
    if (prev_snapshot_ts_ != kMaxTxnTimestamp) {
      assert(prev_snapshot_ts_ <= new_snapshot->GetTimestamp());
    }
    prev_snapshot_ts_ = new_snapshot->GetTimestamp();
  }

  TxnTimestamp prev_snapshot_ts() const { return prev_snapshot_ts_; }

 private:
  SequenceNumber prev_snapshot_seq_ = kMaxSequenceNumber;
  TxnTimestamp prev_snapshot_ts_ = kMaxTxnTimestamp;
};
}  // anonymous namespace

TEST_P(TimestampedSnapshotWithTsSanityCheck, WithoutCommitTs) {
  std::unique_ptr<Transaction> txn(
      db->BeginTransaction(WriteOptions(), TransactionOptions()));
  assert(txn);
  ASSERT_OK(txn->SetName("txn0"));
  ASSERT_OK(txn->Put("a", "v"));
  ASSERT_OK(txn->Prepare());
  Status s = txn->CommitAndCreateSnapshot();
  ASSERT_TRUE(s.IsInvalidArgument());
  ASSERT_OK(txn->Rollback());

  txn.reset(db->BeginTransaction(WriteOptions(), TransactionOptions()));
  assert(txn);
  ASSERT_OK(txn->SetName("txn0"));
  ASSERT_OK(txn->Put("a", "v"));
  s = txn->CommitAndCreateSnapshot();
  ASSERT_TRUE(s.IsInvalidArgument());
}

TEST_P(TimestampedSnapshotWithTsSanityCheck, SetCommitTs) {
  std::unique_ptr<Transaction> txn(
      db->BeginTransaction(WriteOptions(), TransactionOptions()));
  assert(txn);
  ASSERT_OK(txn->SetName("txn0"));
  ASSERT_OK(txn->Put("a", "v"));
  ASSERT_OK(txn->Prepare());
  Status s = txn->CommitAndCreateSnapshot(nullptr, 10);
  ASSERT_TRUE(s.IsNotSupported());
  ASSERT_OK(txn->Rollback());

  txn.reset(db->BeginTransaction(WriteOptions(), TransactionOptions()));
  assert(txn);
  ASSERT_OK(txn->SetName("txn0"));
  ASSERT_OK(txn->Put("a", "v"));
  s = txn->CommitAndCreateSnapshot(nullptr, 10);
  ASSERT_TRUE(s.IsNotSupported());
}

TEST_P(TransactionTest, WithoutCommitTs) {
  std::unique_ptr<Transaction> txn(
      db->BeginTransaction(WriteOptions(), TransactionOptions()));
  assert(txn);
  ASSERT_OK(txn->SetName("txn0"));
  ASSERT_OK(txn->Put("a", "v"));
  ASSERT_OK(txn->Prepare());
  Status s = txn->CommitAndCreateSnapshot();
  ASSERT_TRUE(s.IsInvalidArgument());
  ASSERT_OK(txn->Rollback());

  txn.reset(db->BeginTransaction(WriteOptions(), TransactionOptions()));
  assert(txn);
  ASSERT_OK(txn->SetName("txn0"));
  ASSERT_OK(txn->Put("a", "v"));
  s = txn->CommitAndCreateSnapshot();
  ASSERT_TRUE(s.IsInvalidArgument());
}

TEST_P(TransactionTest, CreateSnapshotWhenCommit) {
  std::unique_ptr<Transaction> txn(
      db->BeginTransaction(WriteOptions(), TransactionOptions()));
  assert(txn);

  constexpr int batch_size = 10;
  for (int i = 0; i < batch_size; ++i) {
    ASSERT_OK(db->Put(WriteOptions(), "k" + std::to_string(i), "v0"));
  }
  const SequenceNumber seq0 = db->GetLatestSequenceNumber();
  ASSERT_EQ(static_cast<SequenceNumber>(batch_size), seq0);

  txn->SetSnapshot();
  {
    const Snapshot* const snapshot = txn->GetSnapshot();
    assert(snapshot);
    ASSERT_EQ(seq0, snapshot->GetSequenceNumber());
  }

  for (int i = 0; i < batch_size; ++i) {
    ASSERT_OK(txn->Put("k" + std::to_string(i), "v1"));
  }
  ASSERT_OK(txn->SetName("txn0"));
  ASSERT_OK(txn->Prepare());

  std::shared_ptr<const Snapshot> snapshot;
  constexpr TxnTimestamp timestamp = 1;
  auto notifier = std::make_shared<TsCheckingTxnNotifier>();
  Status s = txn->CommitAndCreateSnapshot(notifier, timestamp, &snapshot);
  ASSERT_OK(s);
  ASSERT_LT(notifier->prev_snapshot_ts(), kMaxTxnTimestamp);
  assert(snapshot);
  ASSERT_EQ(timestamp, snapshot->GetTimestamp());
  ASSERT_EQ(seq0 + batch_size, snapshot->GetSequenceNumber());
  const Snapshot* const raw_snapshot_ptr = txn->GetSnapshot();
  ASSERT_EQ(raw_snapshot_ptr, snapshot.get());
  ASSERT_EQ(snapshot, txn->GetTimestampedSnapshot());

  {
    std::shared_ptr<const Snapshot> snapshot1 =
        db->GetLatestTimestampedSnapshot();
    ASSERT_EQ(snapshot, snapshot1);
  }
  {
    std::shared_ptr<const Snapshot> snapshot1 =
        db->GetTimestampedSnapshot(timestamp);
    ASSERT_EQ(snapshot, snapshot1);
  }
  {
    std::vector<std::shared_ptr<const Snapshot> > snapshots;
    s = db->GetAllTimestampedSnapshots(snapshots);
    ASSERT_OK(s);
    ASSERT_EQ(std::vector<std::shared_ptr<const Snapshot> >{snapshot},
              snapshots);
  }
}

TEST_P(TransactionTest, CreateSnapshot) {
  // First create a non-timestamped snapshot
  ManagedSnapshot snapshot_guard(db);
  for (int i = 0; i < 10; ++i) {
    ASSERT_OK(db->Put(WriteOptions(), "k" + std::to_string(i),
                      "v0_" + std::to_string(i)));
  }
  {
    auto snapshot = db->CreateTimestampedSnapshot(kMaxTxnTimestamp);
    ASSERT_EQ(nullptr, snapshot.get());
  }
  constexpr TxnTimestamp timestamp = 100;
  std::shared_ptr<const Snapshot> ts_snap0 =
      db->CreateTimestampedSnapshot(timestamp);
  assert(ts_snap0);
  ASSERT_EQ(timestamp, ts_snap0->GetTimestamp());
  for (int i = 0; i < 10; ++i) {
    ASSERT_OK(db->Delete(WriteOptions(), "k" + std::to_string(i)));
  }
  {
    ReadOptions read_opts;
    read_opts.snapshot = ts_snap0.get();
    for (int i = 0; i < 10; ++i) {
      std::string value;
      Status s = db->Get(read_opts, "k" + std::to_string(i), &value);
      ASSERT_EQ("v0_" + std::to_string(i), value);
    }
  }
  {
    std::shared_ptr<const Snapshot> snapshot =
        db->GetLatestTimestampedSnapshot();
    ASSERT_EQ(ts_snap0, snapshot);
  }
  {
    std::shared_ptr<const Snapshot> snapshot =
        db->GetTimestampedSnapshot(timestamp);
    ASSERT_EQ(ts_snap0, snapshot);
  }
  {
    std::vector<std::shared_ptr<const Snapshot> > snapshots;
    const Status s = db->GetAllTimestampedSnapshots(snapshots);
    ASSERT_OK(s);
    ASSERT_EQ(std::vector<std::shared_ptr<const Snapshot> >{ts_snap0},
              snapshots);
  }
}

TEST_P(TransactionTest, CloseDbWithSnapshots) {
  std::unique_ptr<Transaction> txn(
      db->BeginTransaction(WriteOptions(), TransactionOptions()));
  ASSERT_OK(txn->SetName("txn0"));
  ASSERT_OK(txn->Put("foo", "v"));
  ASSERT_OK(txn->Prepare());
  std::shared_ptr<const Snapshot> snapshot;
  constexpr TxnTimestamp timestamp = 121;
  auto notifier = std::make_shared<TsCheckingTxnNotifier>();
  ASSERT_OK(txn->CommitAndCreateSnapshot(notifier, timestamp, &snapshot));
  assert(snapshot);
  ASSERT_LT(notifier->prev_snapshot_ts(), kMaxTxnTimestamp);
  ASSERT_EQ(timestamp, snapshot->GetTimestamp());
  ASSERT_TRUE(db->Close().IsAborted());
}

TEST_P(TransactionTest, MultipleTimestampedSnapshots) {
  auto* dbimpl = static_cast_with_check<DBImpl>(db->GetRootDB());
  assert(dbimpl);
  const bool seq_per_batch = dbimpl->seq_per_batch();
  // TODO: remove the following assert(!seq_per_batch) once timestamped snapshot
  // is supported in write-prepared/write-unprepared transactions.
  assert(!seq_per_batch);
  constexpr size_t txn_size = 10;
  constexpr TxnTimestamp ts_delta = 10;
  constexpr size_t num_txns = 100;
  std::vector<std::shared_ptr<const Snapshot> > snapshots(num_txns);
  constexpr TxnTimestamp start_ts = 10000;
  auto notifier = std::make_shared<TsCheckingTxnNotifier>();
  for (size_t i = 0; i < num_txns; ++i) {
    std::unique_ptr<Transaction> txn(
        db->BeginTransaction(WriteOptions(), TransactionOptions()));
    ASSERT_OK(txn->SetName("txn" + std::to_string(i)));
    for (size_t j = 0; j < txn_size; ++j) {
      ASSERT_OK(txn->Put("k" + std::to_string(j),
                         "v" + std::to_string(j) + "_" + std::to_string(i)));
    }
    if (0 == (i % 2)) {
      ASSERT_OK(txn->Prepare());
    }
    ASSERT_OK(txn->CommitAndCreateSnapshot(notifier, start_ts + i * ts_delta,
                                           &snapshots[i]));
    assert(snapshots[i]);
    ASSERT_LT(notifier->prev_snapshot_ts(), kMaxTxnTimestamp);
    ASSERT_EQ(start_ts + i * ts_delta, snapshots[i]->GetTimestamp());
  }

  {
    auto snapshot = db->GetTimestampedSnapshot(start_ts + 1);
    ASSERT_EQ(nullptr, snapshot);
  }

  constexpr TxnTimestamp max_ts = start_ts + num_txns * ts_delta;
  for (size_t i = 0; i < num_txns; ++i) {
    auto snapshot = db->GetTimestampedSnapshot(start_ts + i * ts_delta);
    ASSERT_EQ(snapshots[i], snapshot);

    std::vector<std::shared_ptr<const Snapshot> > tmp_snapshots;
    Status s = db->GetTimestampedSnapshots(max_ts, start_ts + i * ts_delta,
                                           tmp_snapshots);
    ASSERT_TRUE(s.IsInvalidArgument());
    ASSERT_TRUE(tmp_snapshots.empty());

    for (size_t j = i; j < num_txns; ++j) {
      std::vector<std::shared_ptr<const Snapshot> > expected_snapshots(
          snapshots.begin() + i, snapshots.begin() + j);
      tmp_snapshots.clear();
      s = db->GetTimestampedSnapshots(start_ts + i * ts_delta,
                                      start_ts + j * ts_delta, tmp_snapshots);
      if (i < j) {
        ASSERT_OK(s);
      } else {
        ASSERT_TRUE(s.IsInvalidArgument());
      }
      ASSERT_EQ(expected_snapshots, tmp_snapshots);
    }
  }

  {
    std::vector<std::shared_ptr<const Snapshot> > tmp_snapshots;
    const Status s = db->GetAllTimestampedSnapshots(tmp_snapshots);
    ASSERT_OK(s);
    ASSERT_EQ(snapshots, tmp_snapshots);

    const std::shared_ptr<const Snapshot> latest_snapshot =
        db->GetLatestTimestampedSnapshot();
    ASSERT_EQ(snapshots.back(), latest_snapshot);
  }

  for (size_t i = 0; i <= num_txns; ++i) {
    std::vector<std::shared_ptr<const Snapshot> > snapshots1(
        snapshots.begin() + i, snapshots.end());
    if (i > 0) {
      auto snapshot1 =
          db->GetTimestampedSnapshot(start_ts + (i - 1) * ts_delta);
      assert(snapshot1);
      ASSERT_EQ(start_ts + (i - 1) * ts_delta, snapshot1->GetTimestamp());
    }

    db->ReleaseTimestampedSnapshotsOlderThan(start_ts + i * ts_delta);

    if (i > 0) {
      auto snapshot1 =
          db->GetTimestampedSnapshot(start_ts + (i - 1) * ts_delta);
      ASSERT_EQ(nullptr, snapshot1);
    }

    std::vector<std::shared_ptr<const Snapshot> > tmp_snapshots;
    const Status s = db->GetAllTimestampedSnapshots(tmp_snapshots);
    ASSERT_OK(s);
    ASSERT_EQ(snapshots1, tmp_snapshots);
  }

  // Even after released by db, the applications still hold reference to shared
  // snapshots.
  for (size_t i = 0; i < num_txns; ++i) {
    assert(snapshots[i]);
    ASSERT_EQ(start_ts + i * ts_delta, snapshots[i]->GetTimestamp());
  }

  snapshots.clear();
  ASSERT_OK(db->Close());
  delete db;
  db = nullptr;
}

}  // namespace ROCKSDB_NAMESPACE

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
#endif  // !ROCKSDB_LITE
