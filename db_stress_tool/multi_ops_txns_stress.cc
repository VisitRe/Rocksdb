//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifdef GFLAGS
#include "db_stress_tool/multi_ops_txns_stress.h"

#include "rocksdb/utilities/write_batch_with_index.h"
#include "util/defer.h"
#ifndef NDEBUG
#include "utilities/fault_injection_fs.h"
#endif  // NDEBUG

namespace ROCKSDB_NAMESPACE {

// The description of A and C can be found in multi_ops_txns_stress.h
DEFINE_int32(lb_a, 0, "(Inclusive) lower bound of A");
DEFINE_int32(ub_a, 1000, "(Exclusive) upper bound of A");
DEFINE_int32(lb_c, 0, "(Inclusive) lower bound of C");
DEFINE_int32(ub_c, 1000, "(Exclusive) upper bound of C");

DEFINE_string(key_spaces_path, "",
              "Path to file describing the lower and upper bounds of A and C");

DEFINE_int32(delay_snapshot_read_one_in, 0,
             "With a chance of 1/N, inject a random delay between taking "
             "snapshot and read.");

DEFINE_int32(rollback_one_in, 0,
             "If non-zero, rollback each one out of this number of "
             "non-read-only transactions.");

// MultiOpsTxnsStressTest can either operate on a database with pre-populated
// data (possibly from previous ones), or create a new db and preload it with
// data specified via `-lb_a`, `-ub_a`, `-lb_c`, `-ub_c`, etc. Among these, we
// define the test key spaces as two key ranges: [lb_a, ub_a) and [lb_c, ub_c).
// The key spaces specification is persisted in a file whose absolute path can
// be specified via `-key_spaces_path`.
//
// Whether an existing db is used or a new one is created, key_spaces_path will
// be used. In the former case, the test reads the key spaces specification
// from `-key_spaces_path` and decodes [lb_a, ub_a) and [lb_c, ub_c). In the
// latter case, the test writes a key spaces specification to a file at the
// location, and this file will be used by future runs until a new db is
// created.
//
// Create a fresh new database (-destroy_db_initially=1 or there is no database
// in the location specified by -db). See PreloadDb().
//
// Use an existing, non-empty database. See ScanExistingDb().
//
// This test is multi-threaded, and thread count can be specified via
// `-threads`. For simplicity, we partition the key ranges and each thread
// operates on a subrange independently.
// Within each subrange, a KeyGenerator object is responsible for key
// generation. A KeyGenerator maintains two sets: set of existing keys within
// [low, high), set of non-existing keys within [low, high). [low, high) is the
// subrange. The test initialization makes sure there is at least one
// non-existing key, otherwise the test will return an error and exit before
// any test thread is spawned.

void MultiOpsTxnsStressTest::KeyGenerator::FinishInit() {
  assert(existing_.empty());
  assert(!existing_uniq_.empty());
  assert(low_ < high_);
  for (auto v : existing_uniq_) {
    assert(low_ <= v);
    assert(high_ > v);
    existing_.push_back(v);
  }
  if (non_existing_uniq_.empty()) {
    fprintf(
        stderr,
        "Cannot allocate key in [%u, %u)\nStart with a new DB or try change "
        "the number of threads for testing via -threads=<#threads>\n",
        static_cast<unsigned int>(low_), static_cast<unsigned int>(high_));
    fflush(stderr);
    std::terminate();
  }
  initialized_ = true;
}

std::pair<uint32_t, uint32_t>
MultiOpsTxnsStressTest::KeyGenerator::ChooseExisting() {
  assert(initialized_);
  const size_t N = existing_.size();
  assert(N > 0);
  uint32_t rnd = rand_.Uniform(static_cast<int>(N));
  assert(rnd < N);
  return std::make_pair(existing_[rnd], rnd);
}

uint32_t MultiOpsTxnsStressTest::KeyGenerator::Allocate() {
  assert(initialized_);
  auto it = non_existing_uniq_.begin();
  assert(non_existing_uniq_.end() != it);
  uint32_t ret = *it;
  // Remove this element from non_existing_.
  // Need to call UndoAllocation() if the calling transaction does not commit.
  non_existing_uniq_.erase(it);
  return ret;
}

void MultiOpsTxnsStressTest::KeyGenerator::Replace(uint32_t old_val,
                                                   uint32_t old_pos,
                                                   uint32_t new_val) {
  assert(initialized_);
  {
    auto it = existing_uniq_.find(old_val);
    assert(it != existing_uniq_.end());
    existing_uniq_.erase(it);
  }

  {
    assert(0 == existing_uniq_.count(new_val));
    existing_uniq_.insert(new_val);
    existing_[old_pos] = new_val;
  }

  {
    assert(0 == non_existing_uniq_.count(old_val));
    non_existing_uniq_.insert(old_val);
  }
}

void MultiOpsTxnsStressTest::KeyGenerator::UndoAllocation(uint32_t new_val) {
  assert(initialized_);
  assert(0 == non_existing_uniq_.count(new_val));
  non_existing_uniq_.insert(new_val);
}

std::string MultiOpsTxnsStressTest::Record::EncodePrimaryKey(uint32_t a) {
  char buf[8];
  EncodeFixed32(buf, kPrimaryIndexId);
  std::reverse(buf, buf + 4);
  EncodeFixed32(buf + 4, a);
  std::reverse(buf + 4, buf + 8);
  return std::string(buf, sizeof(buf));
}

std::string MultiOpsTxnsStressTest::Record::EncodeSecondaryKey(uint32_t c) {
  char buf[8];
  EncodeFixed32(buf, kSecondaryIndexId);
  std::reverse(buf, buf + 4);
  EncodeFixed32(buf + 4, c);
  std::reverse(buf + 4, buf + 8);
  return std::string(buf, sizeof(buf));
}

std::string MultiOpsTxnsStressTest::Record::EncodeSecondaryKey(uint32_t c,
                                                               uint32_t a) {
  char buf[12];
  EncodeFixed32(buf, kSecondaryIndexId);
  std::reverse(buf, buf + 4);
  EncodeFixed32(buf + 4, c);
  EncodeFixed32(buf + 8, a);
  std::reverse(buf + 4, buf + 8);
  std::reverse(buf + 8, buf + 12);
  return std::string(buf, sizeof(buf));
}

std::tuple<Status, uint32_t, uint32_t>
MultiOpsTxnsStressTest::Record::DecodePrimaryIndexValue(
    Slice primary_index_value) {
  if (primary_index_value.size() != 8) {
    return std::tuple<Status, uint32_t, uint32_t>{Status::Corruption(""), 0, 0};
  }
  uint32_t b = 0;
  uint32_t c = 0;
  if (!GetFixed32(&primary_index_value, &b) ||
      !GetFixed32(&primary_index_value, &c)) {
    assert(false);
    return std::tuple<Status, uint32_t, uint32_t>{Status::Corruption(""), 0, 0};
  }
  return std::tuple<Status, uint32_t, uint32_t>{Status::OK(), b, c};
}

std::pair<Status, uint32_t>
MultiOpsTxnsStressTest::Record::DecodeSecondaryIndexValue(
    Slice secondary_index_value) {
  if (secondary_index_value.size() != 4) {
    return std::make_pair(Status::Corruption(""), 0);
  }
  uint32_t crc = 0;
  bool result __attribute__((unused)) =
      GetFixed32(&secondary_index_value, &crc);
  assert(result);
  return std::make_pair(Status::OK(), crc);
}

std::pair<std::string, std::string>
MultiOpsTxnsStressTest::Record::EncodePrimaryIndexEntry() const {
  std::string primary_index_key = EncodePrimaryKey();
  std::string primary_index_value = EncodePrimaryIndexValue();
  return std::make_pair(primary_index_key, primary_index_value);
}

std::string MultiOpsTxnsStressTest::Record::EncodePrimaryKey() const {
  return EncodePrimaryKey(a_);
}

std::string MultiOpsTxnsStressTest::Record::EncodePrimaryIndexValue() const {
  char buf[8];
  EncodeFixed32(buf, b_);
  EncodeFixed32(buf + 4, c_);
  return std::string(buf, sizeof(buf));
}

std::pair<std::string, std::string>
MultiOpsTxnsStressTest::Record::EncodeSecondaryIndexEntry() const {
  std::string secondary_index_key;
  char buf[12];
  EncodeFixed32(buf, kSecondaryIndexId);
  std::reverse(buf, buf + 4);
  EncodeFixed32(buf + 4, c_);
  EncodeFixed32(buf + 8, a_);
  std::reverse(buf + 4, buf + 8);
  std::reverse(buf + 8, buf + 12);
  secondary_index_key.assign(buf, sizeof(buf));

  // Secondary index value is always 4-byte crc32 of the secondary key
  std::string secondary_index_value;
  uint32_t crc = crc32c::Value(buf, sizeof(buf));
  PutFixed32(&secondary_index_value, crc);
  return std::make_pair(secondary_index_key, secondary_index_value);
}

std::string MultiOpsTxnsStressTest::Record::EncodeSecondaryKey() const {
  char buf[12];
  EncodeFixed32(buf, kSecondaryIndexId);
  std::reverse(buf, buf + 4);
  EncodeFixed32(buf + 4, c_);
  EncodeFixed32(buf + 8, a_);
  std::reverse(buf + 4, buf + 8);
  std::reverse(buf + 8, buf + 12);
  return std::string(buf, sizeof(buf));
}

Status MultiOpsTxnsStressTest::Record::DecodePrimaryIndexEntry(
    Slice primary_index_key, Slice primary_index_value) {
  if (primary_index_key.size() != 8) {
    assert(false);
    return Status::Corruption("Primary index key length is not 8");
  }

  const char* const index_id_buf = primary_index_key.data();
  uint32_t index_id =
      static_cast<uint32_t>(static_cast<unsigned char>(index_id_buf[0])) << 24;
  index_id += static_cast<uint32_t>(static_cast<unsigned char>(index_id_buf[1]))
              << 16;
  index_id += static_cast<uint32_t>(static_cast<unsigned char>(index_id_buf[2]))
              << 8;
  index_id +=
      static_cast<uint32_t>(static_cast<unsigned char>(index_id_buf[3]));
  primary_index_key.remove_prefix(sizeof(uint32_t));
  if (index_id != kPrimaryIndexId) {
    std::ostringstream oss;
    oss << "Unexpected primary index id: " << index_id;
    return Status::Corruption(oss.str());
  }

  const char* const buf = primary_index_key.data();
  a_ = static_cast<uint32_t>(static_cast<unsigned char>(buf[0])) << 24;
  a_ += static_cast<uint32_t>(static_cast<unsigned char>(buf[1])) << 16;
  a_ += static_cast<uint32_t>(static_cast<unsigned char>(buf[2])) << 8;
  a_ += static_cast<uint32_t>(static_cast<unsigned char>(buf[3]));

  if (primary_index_value.size() != 8) {
    return Status::Corruption("Primary index value length is not 8");
  }
  GetFixed32(&primary_index_value, &b_);
  GetFixed32(&primary_index_value, &c_);
  return Status::OK();
}

Status MultiOpsTxnsStressTest::Record::DecodeSecondaryIndexEntry(
    Slice secondary_index_key, Slice secondary_index_value) {
  if (secondary_index_key.size() != 12) {
    return Status::Corruption("Secondary index key length is not 12");
  }
  uint32_t crc =
      crc32c::Value(secondary_index_key.data(), secondary_index_key.size());

  const char* const index_id_buf = secondary_index_key.data();
  uint32_t index_id =
      static_cast<uint32_t>(static_cast<unsigned char>(index_id_buf[0])) << 24;
  index_id += static_cast<uint32_t>(static_cast<unsigned char>(index_id_buf[1]))
              << 16;
  index_id += static_cast<uint32_t>(static_cast<unsigned char>(index_id_buf[2]))
              << 8;
  index_id +=
      static_cast<uint32_t>(static_cast<unsigned char>(index_id_buf[3]));
  secondary_index_key.remove_prefix(sizeof(uint32_t));
  if (index_id != kSecondaryIndexId) {
    std::ostringstream oss;
    oss << "Unexpected secondary index id: " << index_id;
    return Status::Corruption(oss.str());
  }

  const char* const buf = secondary_index_key.data();
  assert(secondary_index_key.size() == 8);
  c_ = static_cast<uint32_t>(static_cast<unsigned char>(buf[0])) << 24;
  c_ += static_cast<uint32_t>(static_cast<unsigned char>(buf[1])) << 16;
  c_ += static_cast<uint32_t>(static_cast<unsigned char>(buf[2])) << 8;
  c_ += static_cast<uint32_t>(static_cast<unsigned char>(buf[3]));

  a_ = static_cast<uint32_t>(static_cast<unsigned char>(buf[4])) << 24;
  a_ += static_cast<uint32_t>(static_cast<unsigned char>(buf[5])) << 16;
  a_ += static_cast<uint32_t>(static_cast<unsigned char>(buf[6])) << 8;
  a_ += static_cast<uint32_t>(static_cast<unsigned char>(buf[7]));

  if (secondary_index_value.size() != 4) {
    return Status::Corruption("Secondary index value length is not 4");
  }
  uint32_t val = 0;
  GetFixed32(&secondary_index_value, &val);
  if (val != crc) {
    std::ostringstream oss;
    oss << "Secondary index key checksum mismatch, stored: " << val
        << ", recomputed: " << crc;
    return Status::Corruption(oss.str());
  }
  return Status::OK();
}

void MultiOpsTxnsStressTest::FinishInitDb(SharedState* shared) {
  if (FLAGS_enable_compaction_filter) {
    // TODO (yanqin) enable compaction filter
  }
  ReopenAndPreloadDbIfNeeded(shared);
  // TODO (yanqin) parallelize if key space is large
  for (auto& key_gen : key_gen_for_a_) {
    assert(key_gen);
    key_gen->FinishInit();
  }
  // TODO (yanqin) parallelize if key space is large
  for (auto& key_gen : key_gen_for_c_) {
    assert(key_gen);
    key_gen->FinishInit();
  }
}

void MultiOpsTxnsStressTest::ReopenAndPreloadDbIfNeeded(SharedState* shared) {
  (void)shared;
#ifndef ROCKSDB_LITE
  bool db_empty = false;
  {
    std::unique_ptr<Iterator> iter(db_->NewIterator(ReadOptions()));
    iter->SeekToFirst();
    if (!iter->Valid()) {
      db_empty = true;
    }
  }

  if (db_empty) {
    PreloadDb(shared, FLAGS_threads, FLAGS_lb_a, FLAGS_ub_a, FLAGS_lb_c,
              FLAGS_ub_c);
  } else {
    fprintf(stdout,
            "Key ranges will be read from %s.\n-lb_a, -ub_a, -lb_c, -ub_c will "
            "be ignored\n",
            FLAGS_key_spaces_path.c_str());
    fflush(stdout);
    ScanExistingDb(shared, FLAGS_threads);
  }
#endif  // !ROCKSDB_LITE
}

// Used for point-lookup transaction
Status MultiOpsTxnsStressTest::TestGet(
    ThreadState* thread, const ReadOptions& read_opts,
    const std::vector<int>& /*rand_column_families*/,
    const std::vector<int64_t>& /*rand_keys*/) {
  uint32_t a = 0;
  uint32_t pos = 0;
  std::tie(a, pos) = ChooseExistingA(thread);
  return PointLookupTxn(thread, read_opts, a);
}

// Not used.
std::vector<Status> MultiOpsTxnsStressTest::TestMultiGet(
    ThreadState* /*thread*/, const ReadOptions& /*read_opts*/,
    const std::vector<int>& /*rand_column_families*/,
    const std::vector<int64_t>& /*rand_keys*/) {
  return std::vector<Status>{Status::NotSupported()};
}

Status MultiOpsTxnsStressTest::TestPrefixScan(
    ThreadState* thread, const ReadOptions& read_opts,
    const std::vector<int>& rand_column_families,
    const std::vector<int64_t>& rand_keys) {
  (void)thread;
  (void)read_opts;
  (void)rand_column_families;
  (void)rand_keys;
  return Status::OK();
}

// Given a key K, this creates an iterator which scans to K and then
// does a random sequence of Next/Prev operations.
Status MultiOpsTxnsStressTest::TestIterate(
    ThreadState* thread, const ReadOptions& read_opts,
    const std::vector<int>& /*rand_column_families*/,
    const std::vector<int64_t>& /*rand_keys*/) {
  uint32_t c = 0;
  uint32_t pos = 0;
  std::tie(c, pos) = ChooseExistingC(thread);
  return RangeScanTxn(thread, read_opts, c);
}

// Not intended for use.
Status MultiOpsTxnsStressTest::TestPut(ThreadState* /*thread*/,
                                       WriteOptions& /*write_opts*/,
                                       const ReadOptions& /*read_opts*/,
                                       const std::vector<int>& /*cf_ids*/,
                                       const std::vector<int64_t>& /*keys*/,
                                       char (&value)[100],
                                       std::unique_ptr<MutexLock>& /*lock*/) {
  (void)value;
  return Status::NotSupported();
}

// Not intended for use.
Status MultiOpsTxnsStressTest::TestDelete(
    ThreadState* /*thread*/, WriteOptions& /*write_opts*/,
    const std::vector<int>& /*rand_column_families*/,
    const std::vector<int64_t>& /*rand_keys*/,
    std::unique_ptr<MutexLock>& /*lock*/) {
  return Status::NotSupported();
}

// Not intended for use.
Status MultiOpsTxnsStressTest::TestDeleteRange(
    ThreadState* /*thread*/, WriteOptions& /*write_opts*/,
    const std::vector<int>& /*rand_column_families*/,
    const std::vector<int64_t>& /*rand_keys*/,
    std::unique_ptr<MutexLock>& /*lock*/) {
  return Status::NotSupported();
}

void MultiOpsTxnsStressTest::TestIngestExternalFile(
    ThreadState* thread, const std::vector<int>& rand_column_families,
    const std::vector<int64_t>& /*rand_keys*/,
    std::unique_ptr<MutexLock>& /*lock*/) {
  // TODO (yanqin)
  (void)thread;
  (void)rand_column_families;
}

void MultiOpsTxnsStressTest::TestCompactRange(
    ThreadState* thread, int64_t /*rand_key*/, const Slice& /*start_key*/,
    ColumnFamilyHandle* column_family) {
  // TODO (yanqin).
  // May use GetRangeHash() for validation before and after DB::CompactRange()
  // completes.
  (void)thread;
  (void)column_family;
}

Status MultiOpsTxnsStressTest::TestBackupRestore(
    ThreadState* thread, const std::vector<int>& rand_column_families,
    const std::vector<int64_t>& /*rand_keys*/) {
  // TODO (yanqin)
  (void)thread;
  (void)rand_column_families;
  return Status::OK();
}

Status MultiOpsTxnsStressTest::TestCheckpoint(
    ThreadState* thread, const std::vector<int>& rand_column_families,
    const std::vector<int64_t>& /*rand_keys*/) {
  // TODO (yanqin)
  (void)thread;
  (void)rand_column_families;
  return Status::OK();
}

#ifndef ROCKSDB_LITE
Status MultiOpsTxnsStressTest::TestApproximateSize(
    ThreadState* thread, uint64_t iteration,
    const std::vector<int>& rand_column_families,
    const std::vector<int64_t>& /*rand_keys*/) {
  // TODO (yanqin)
  (void)thread;
  (void)iteration;
  (void)rand_column_families;
  return Status::OK();
}
#endif  // !ROCKSDB_LITE

Status MultiOpsTxnsStressTest::TestCustomOperations(
    ThreadState* thread, const std::vector<int>& rand_column_families) {
  (void)rand_column_families;
  // Randomly choose from 0, 1, and 2.
  // TODO (yanqin) allow user to configure probability of each operation.
  uint32_t rand = thread->rand.Uniform(3);
  Status s;
  if (0 == rand) {
    // Update primary key.
    uint32_t old_a = 0;
    uint32_t pos = 0;
    std::tie(old_a, pos) = ChooseExistingA(thread);
    uint32_t new_a = GenerateNextA(thread);
    s = PrimaryKeyUpdateTxn(thread, old_a, pos, new_a);
  } else if (1 == rand) {
    // Update secondary key.
    uint32_t old_c = 0;
    uint32_t pos = 0;
    std::tie(old_c, pos) = ChooseExistingC(thread);
    uint32_t new_c = GenerateNextC(thread);
    s = SecondaryKeyUpdateTxn(thread, old_c, pos, new_c);
  } else if (2 == rand) {
    // Update primary index value.
    uint32_t a = 0;
    uint32_t pos = 0;
    std::tie(a, pos) = ChooseExistingA(thread);
    s = UpdatePrimaryIndexValueTxn(thread, a, /*b_delta=*/1);
  } else {
    // Should never reach here.
    assert(false);
  }
  return s;
}

Status MultiOpsTxnsStressTest::PrimaryKeyUpdateTxn(ThreadState* thread,
                                                   uint32_t old_a,
                                                   uint32_t old_a_pos,
                                                   uint32_t new_a) {
#ifdef ROCKSDB_LITE
  (void)thread;
  (void)old_a;
  (void)old_a_pos;
  (void)new_a;
  return Status::NotSupported();
#else
  std::string old_pk = Record::EncodePrimaryKey(old_a);
  std::string new_pk = Record::EncodePrimaryKey(new_a);
  Transaction* txn = nullptr;
  WriteOptions wopts;
  Status s = NewTxn(wopts, &txn);
  if (!s.ok()) {
    assert(!txn);
    thread->stats.AddErrors(1);
    return s;
  }

  assert(txn);
  txn->SetSnapshotOnNextOperation(/*notifier=*/nullptr);

  const Defer cleanup([new_a, &s, thread, txn, this]() {
    if (s.ok()) {
      // Two gets, one for existing pk, one for locking potential new pk.
      thread->stats.AddGets(/*ngets=*/2, /*nfounds=*/1);
      thread->stats.AddDeletes(1);
      thread->stats.AddBytesForWrites(
          /*nwrites=*/2,
          Record::kPrimaryIndexEntrySize + Record::kSecondaryIndexEntrySize);
      thread->stats.AddSingleDeletes(1);
      return;
    }
    if (s.IsNotFound()) {
      thread->stats.AddGets(/*ngets=*/1, /*nfounds=*/0);
    } else if (s.IsBusy() || s.IsIncomplete()) {
      // ignore.
      // Incomplete also means rollback by application. See the transaction
      // implementations.
    } else {
      thread->stats.AddErrors(1);
    }
    auto& key_gen = key_gen_for_a_[thread->tid];
    key_gen->UndoAllocation(new_a);
    RollbackTxn(txn).PermitUncheckedError();
  });

  ReadOptions ropts;
  ropts.rate_limiter_priority =
      FLAGS_rate_limit_user_ops ? Env::IO_USER : Env::IO_TOTAL;
  std::string value;
  s = txn->GetForUpdate(ropts, old_pk, &value);
  if (!s.ok()) {
    return s;
  }
  std::string empty_value;
  s = txn->GetForUpdate(ropts, new_pk, &empty_value);
  if (s.ok()) {
    assert(!empty_value.empty());
    s = Status::Busy();
    return s;
  } else if (!s.IsNotFound()) {
    return s;
  }

  auto result = Record::DecodePrimaryIndexValue(value);
  s = std::get<0>(result);
  if (!s.ok()) {
    return s;
  }
  uint32_t b = std::get<1>(result);
  uint32_t c = std::get<2>(result);

  ColumnFamilyHandle* cf = db_->DefaultColumnFamily();
  s = txn->Delete(cf, old_pk, /*assume_tracked=*/true);
  if (!s.ok()) {
    return s;
  }
  s = txn->Put(cf, new_pk, value, /*assume_tracked=*/true);
  if (!s.ok()) {
    return s;
  }

  auto* wb = txn->GetWriteBatch();
  assert(wb);

  std::string old_sk = Record::EncodeSecondaryKey(c, old_a);
  s = wb->SingleDelete(old_sk);
  if (!s.ok()) {
    return s;
  }

  Record record(new_a, b, c);
  std::string new_sk;
  std::string new_crc;
  std::tie(new_sk, new_crc) = record.EncodeSecondaryIndexEntry();
  s = wb->Put(new_sk, new_crc);
  if (!s.ok()) {
    return s;
  }

  s = txn->Prepare();

  if (!s.ok()) {
    return s;
  }

  if (FLAGS_rollback_one_in > 0 && thread->rand.OneIn(FLAGS_rollback_one_in)) {
    s = Status::Incomplete();
    return s;
  }

  s = txn->Commit();

  auto& key_gen = key_gen_for_a_.at(thread->tid);
  if (s.ok()) {
    delete txn;
    key_gen->Replace(old_a, old_a_pos, new_a);
  }
  return s;
#endif  // !ROCKSDB_LITE
}

Status MultiOpsTxnsStressTest::SecondaryKeyUpdateTxn(ThreadState* thread,
                                                     uint32_t old_c,
                                                     uint32_t old_c_pos,
                                                     uint32_t new_c) {
#ifdef ROCKSDB_LITE
  (void)thread;
  (void)old_c;
  (void)old_c_pos;
  (void)new_c;
  return Status::NotSupported();
#else
  Transaction* txn = nullptr;
  WriteOptions wopts;
  Status s = NewTxn(wopts, &txn);
  if (!s.ok()) {
    assert(!txn);
    thread->stats.AddErrors(1);
    return s;
  }

  assert(txn);

  Iterator* it = nullptr;
  long iterations = 0;
  const Defer cleanup([new_c, &s, thread, &it, txn, this, &iterations]() {
    delete it;
    if (s.ok()) {
      thread->stats.AddIterations(iterations);
      thread->stats.AddGets(/*ngets=*/1, /*nfounds=*/1);
      thread->stats.AddSingleDeletes(1);
      thread->stats.AddBytesForWrites(
          /*nwrites=*/2,
          Record::kPrimaryIndexEntrySize + Record::kSecondaryIndexEntrySize);
      return;
    } else if (s.IsBusy() || s.IsTimedOut() || s.IsTryAgain() ||
               s.IsMergeInProgress() || s.IsIncomplete()) {
      // ww-conflict detected, or
      // lock cannot be acquired, or
      // memtable history is not large enough for conflict checking, or
      // Merge operation cannot be resolved, or
      // application rollback.
      // TODO (yanqin) add stats for other cases?
    } else if (s.IsNotFound()) {
      // ignore.
    } else {
      thread->stats.AddErrors(1);
    }
    auto& key_gen = key_gen_for_c_[thread->tid];
    key_gen->UndoAllocation(new_c);
    RollbackTxn(txn).PermitUncheckedError();
  });

  // TODO (yanqin) try SetSnapshotOnNextOperation(). We currently need to take
  // a snapshot here because we will later verify that point lookup in the
  // primary index using GetForUpdate() returns the same value for 'c' as the
  // iterator. The iterator does not need a snapshot though, because it will be
  // assigned the current latest (published) sequence in the db, which will be
  // no smaller than the snapshot created here. The GetForUpdate will perform
  // ww conflict checking to ensure GetForUpdate() (using the snapshot) sees
  // the same data as this iterator.
  txn->SetSnapshot();
  std::string old_sk_prefix = Record::EncodeSecondaryKey(old_c);
  std::string iter_ub_str = Record::EncodeSecondaryKey(old_c + 1);
  Slice iter_ub = iter_ub_str;
  ReadOptions ropts;
  ropts.snapshot = txn->GetSnapshot();
  ropts.total_order_seek = true;
  ropts.iterate_upper_bound = &iter_ub;
  ropts.rate_limiter_priority =
      FLAGS_rate_limit_user_ops ? Env::IO_USER : Env::IO_TOTAL;
  it = txn->GetIterator(ropts);

  assert(it);
  it->Seek(old_sk_prefix);
  if (!it->Valid()) {
    s = Status::NotFound();
    return s;
  }
  auto* wb = txn->GetWriteBatch();
  assert(wb);

  do {
    ++iterations;
    Record record;
    s = record.DecodeSecondaryIndexEntry(it->key(), it->value());
    if (!s.ok()) {
      fprintf(stderr, "Cannot decode secondary key: %s\n",
              s.ToString().c_str());
      assert(false);
      break;
    }
    // At this point, record.b is not known yet, thus we need to access
    // primary index.
    std::string pk = Record::EncodePrimaryKey(record.a_value());
    std::string value;
    ReadOptions read_opts;
    read_opts.rate_limiter_priority =
        FLAGS_rate_limit_user_ops ? Env::IO_USER : Env::IO_TOTAL;
    read_opts.snapshot = txn->GetSnapshot();
    s = txn->GetForUpdate(read_opts, pk, &value);
    if (s.IsBusy() || s.IsTimedOut() || s.IsTryAgain() ||
        s.IsMergeInProgress()) {
      // Write conflict, or cannot acquire lock, or memtable size is not large
      // enough, or merge cannot be resolved.
      break;
    } else if (s.IsNotFound()) {
      // We can also fail verification here.
      std::ostringstream oss;
      auto* dbimpl = static_cast_with_check<DBImpl>(db_->GetRootDB());
      assert(dbimpl);
      oss << "Under snap " << read_opts.snapshot->GetSequenceNumber()
          << " (last published " << dbimpl->GetLastPublishedSequence()
          << "), pk should exist: " << Slice(pk).ToString(true);
      fprintf(stderr, "%s\n", oss.str().c_str());
      assert(false);
      break;
    }
    if (!s.ok()) {
      std::ostringstream oss;
      auto* dbimpl = static_cast_with_check<DBImpl>(db_->GetRootDB());
      assert(dbimpl);
      oss << "Under snap " << read_opts.snapshot->GetSequenceNumber()
          << "(last published " << dbimpl->GetLastPublishedSequence() << "), "
          << s.ToString();
      fprintf(stderr, "%s\n", oss.str().c_str());
      assert(false);
      break;
    }
    auto result = Record::DecodePrimaryIndexValue(value);
    s = std::get<0>(result);
    if (!s.ok()) {
      fprintf(stderr, "Cannot decode primary index value: %s\n",
              s.ToString().c_str());
      assert(false);
      break;
    }
    uint32_t b = std::get<1>(result);
    uint32_t c = std::get<2>(result);
    if (c != old_c) {
      std::ostringstream oss;
      auto* dbimpl = static_cast_with_check<DBImpl>(db_->GetRootDB());
      assert(dbimpl);
      oss << "Under snap " << read_opts.snapshot->GetSequenceNumber()
          << "(last published " << dbimpl->GetLastPublishedSequence()
          << "), pk/sk mismatch. pk: (c=" << c << "), sk: (c=" << old_c << ")";
      s = Status::Corruption();
      fprintf(stderr, "%s\n", oss.str().c_str());
      assert(false);
      break;
    }
    Record new_rec(record.a_value(), b, new_c);
    std::string new_primary_index_value = new_rec.EncodePrimaryIndexValue();
    ColumnFamilyHandle* cf = db_->DefaultColumnFamily();
    s = txn->Put(cf, pk, new_primary_index_value, /*assume_tracked=*/true);
    if (!s.ok()) {
      break;
    }
    std::string old_sk = it->key().ToString(/*hex=*/false);
    std::string new_sk;
    std::string new_crc;
    std::tie(new_sk, new_crc) = new_rec.EncodeSecondaryIndexEntry();
    s = wb->SingleDelete(old_sk);
    if (!s.ok()) {
      break;
    }
    s = wb->Put(new_sk, new_crc);
    if (!s.ok()) {
      break;
    }

    it->Next();
  } while (it->Valid());

  if (!s.ok()) {
    return s;
  }

  s = txn->Prepare();

  if (!s.ok()) {
    return s;
  }

  if (FLAGS_rollback_one_in > 0 && thread->rand.OneIn(FLAGS_rollback_one_in)) {
    s = Status::Incomplete();
    return s;
  }

  s = txn->Commit();

  if (s.ok()) {
    delete txn;
    auto& key_gen = key_gen_for_c_.at(thread->tid);
    key_gen->Replace(old_c, old_c_pos, new_c);
  }

  return s;
#endif  // !ROCKSDB_LITE
}

Status MultiOpsTxnsStressTest::UpdatePrimaryIndexValueTxn(ThreadState* thread,
                                                          uint32_t a,
                                                          uint32_t b_delta) {
#ifdef ROCKSDB_LITE
  (void)thread;
  (void)a;
  (void)b_delta;
  return Status::NotSupported();
#else
  std::string pk_str = Record::EncodePrimaryKey(a);
  Transaction* txn = nullptr;
  WriteOptions wopts;
  Status s = NewTxn(wopts, &txn);
  if (!s.ok()) {
    assert(!txn);
    thread->stats.AddErrors(1);
    return s;
  }

  assert(txn);

  const Defer cleanup([&s, thread, txn, this]() {
    if (s.ok()) {
      thread->stats.AddGets(/*ngets=*/1, /*nfounds=*/1);
      thread->stats.AddBytesForWrites(
          /*nwrites=*/1, /*nbytes=*/Record::kPrimaryIndexEntrySize);
      return;
    }
    if (s.IsNotFound()) {
      thread->stats.AddGets(/*ngets=*/1, /*nfounds=*/0);
    } else if (s.IsInvalidArgument()) {
      // ignored.
    } else if (s.IsBusy() || s.IsTimedOut() || s.IsTryAgain() ||
               s.IsMergeInProgress() || s.IsIncomplete()) {
      // ignored.
    } else {
      thread->stats.AddErrors(1);
    }
    RollbackTxn(txn).PermitUncheckedError();
  });
  ReadOptions ropts;
  ropts.rate_limiter_priority =
      FLAGS_rate_limit_user_ops ? Env::IO_USER : Env::IO_TOTAL;
  std::string value;
  s = txn->GetForUpdate(ropts, pk_str, &value);
  if (!s.ok()) {
    return s;
  }
  auto result = Record::DecodePrimaryIndexValue(value);
  if (!std::get<0>(result).ok()) {
    s = std::get<0>(result);
    fprintf(stderr, "Cannot decode primary index value: %s\n",
            s.ToString().c_str());
    assert(false);
    return s;
  }
  uint32_t b = std::get<1>(result) + b_delta;
  uint32_t c = std::get<2>(result);
  Record record(a, b, c);
  std::string primary_index_value = record.EncodePrimaryIndexValue();
  ColumnFamilyHandle* cf = db_->DefaultColumnFamily();
  s = txn->Put(cf, pk_str, primary_index_value, /*assume_tracked=*/true);
  if (!s.ok()) {
    return s;
  }
  s = txn->Prepare();
  if (!s.ok()) {
    return s;
  }

  if (FLAGS_rollback_one_in > 0 && thread->rand.OneIn(FLAGS_rollback_one_in)) {
    s = Status::Incomplete();
    return s;
  }

  s = txn->Commit();
  if (s.ok()) {
    delete txn;
  }
  return s;
#endif  // !ROCKSDB_LITE
}

Status MultiOpsTxnsStressTest::PointLookupTxn(ThreadState* thread,
                                              ReadOptions ropts, uint32_t a) {
#ifdef ROCKSDB_LITE
  (void)thread;
  (void)ropts;
  (void)a;
  return Status::NotSupported();
#else
  std::string pk_str = Record::EncodePrimaryKey(a);
  // pk may or may not exist
  PinnableSlice value;

  Transaction* txn = nullptr;
  WriteOptions wopts;
  Status s = NewTxn(wopts, &txn);
  if (!s.ok()) {
    assert(!txn);
    thread->stats.AddErrors(1);
    return s;
  }

  assert(txn);

  const Defer cleanup([&s, thread, txn, this]() {
    if (s.ok()) {
      thread->stats.AddGets(/*ngets=*/1, /*nfounds=*/1);
      return;
    } else if (s.IsNotFound()) {
      thread->stats.AddGets(/*ngets=*/1, /*nfounds=*/0);
    } else {
      thread->stats.AddErrors(1);
    }
    RollbackTxn(txn).PermitUncheckedError();
  });

  txn->SetSnapshot();
  ropts.snapshot = txn->GetSnapshot();

  if (FLAGS_delay_snapshot_read_one_in > 0 &&
      thread->rand.OneIn(FLAGS_delay_snapshot_read_one_in)) {
    uint64_t delay_ms = thread->rand.Uniform(100) + 1;
    db_->GetDBOptions().env->SleepForMicroseconds(
        static_cast<int>(delay_ms * 1000));
  }

  s = txn->Get(ropts, db_->DefaultColumnFamily(), pk_str, &value);
  if (s.ok()) {
    s = txn->Commit();
  }
  if (s.ok()) {
    delete txn;
  }
  return s;
#endif  // !ROCKSDB_LITE
}

Status MultiOpsTxnsStressTest::RangeScanTxn(ThreadState* thread,
                                            ReadOptions ropts, uint32_t c) {
#ifdef ROCKSDB_LITE
  (void)thread;
  (void)ropts;
  (void)c;
  return Status::NotSupported();
#else
  std::string sk = Record::EncodeSecondaryKey(c);

  Transaction* txn = nullptr;
  WriteOptions wopts;
  Status s = NewTxn(wopts, &txn);
  if (!s.ok()) {
    assert(!txn);
    thread->stats.AddErrors(1);
    return s;
  }

  assert(txn);

  const Defer cleanup([&s, thread, txn, this]() {
    if (s.ok()) {
      thread->stats.AddIterations(1);
      return;
    }
    thread->stats.AddErrors(1);
    RollbackTxn(txn).PermitUncheckedError();
  });

  txn->SetSnapshot();
  ropts.snapshot = txn->GetSnapshot();

  if (FLAGS_delay_snapshot_read_one_in > 0 &&
      thread->rand.OneIn(FLAGS_delay_snapshot_read_one_in)) {
    uint64_t delay_ms = thread->rand.Uniform(100) + 1;
    db_->GetDBOptions().env->SleepForMicroseconds(
        static_cast<int>(delay_ms * 1000));
  }

  std::unique_ptr<Iterator> iter(txn->GetIterator(ropts));

  constexpr size_t total_nexts = 10;
  size_t nexts = 0;
  for (iter->Seek(sk);
       iter->Valid() && nexts < total_nexts && iter->status().ok();
       iter->Next(), ++nexts) {
  }

  if (iter->status().ok()) {
    s = txn->Commit();
  } else {
    s = iter->status();
  }

  if (s.ok()) {
    delete txn;
  }

  return s;
#endif  // !ROCKSDB_LITE
}

void MultiOpsTxnsStressTest::VerifyDb(ThreadState* thread) const {
  if (thread->shared->HasVerificationFailedYet()) {
    return;
  }
  const Snapshot* const snapshot = db_->GetSnapshot();
  assert(snapshot);
  ManagedSnapshot snapshot_guard(db_, snapshot);

  std::ostringstream oss;
  oss << "[snap=" << snapshot->GetSequenceNumber() << ",";

  auto* dbimpl = static_cast_with_check<DBImpl>(db_->GetRootDB());
  assert(dbimpl);

  oss << " last_published=" << dbimpl->GetLastPublishedSequence() << "] ";

  if (FLAGS_delay_snapshot_read_one_in > 0 &&
      thread->rand.OneIn(FLAGS_delay_snapshot_read_one_in)) {
    uint64_t delay_ms = thread->rand.Uniform(100) + 1;
    db_->GetDBOptions().env->SleepForMicroseconds(
        static_cast<int>(delay_ms * 1000));
  }

  // TODO (yanqin) with a probability, we can use either forward or backward
  // iterator in subsequent checks. We can also use more advanced features in
  // range scan. For now, let's just use simple forward iteration with
  // total_order_seek = true.

  // First, iterate primary index.
  size_t primary_index_entries_count = 0;
  {
    char buf[4];
    EncodeFixed32(buf, Record::kPrimaryIndexId + 1);
    std::reverse(buf, buf + sizeof(buf));
    std::string iter_ub_str(buf, sizeof(buf));
    Slice iter_ub = iter_ub_str;

    // This `ReadOptions` is for validation purposes. Ignore
    // `FLAGS_rate_limit_user_ops` to avoid slowing any validation.
    ReadOptions ropts;
    ropts.snapshot = snapshot;
    ropts.total_order_seek = true;
    ropts.iterate_upper_bound = &iter_ub;

    std::unique_ptr<Iterator> it(db_->NewIterator(ropts));
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      Record record;
      Status s = record.DecodePrimaryIndexEntry(it->key(), it->value());
      if (!s.ok()) {
        oss << "Cannot decode primary index entry " << it->key().ToString(true)
            << "=>" << it->value().ToString(true);
        VerificationAbort(thread->shared, oss.str(), s);
        assert(false);
        return;
      }
      ++primary_index_entries_count;

      // Search secondary index.
      uint32_t a = record.a_value();
      uint32_t c = record.c_value();
      char sk_buf[12];
      EncodeFixed32(sk_buf, Record::kSecondaryIndexId);
      std::reverse(sk_buf, sk_buf + sizeof(uint32_t));
      EncodeFixed32(sk_buf + sizeof(uint32_t), c);
      std::reverse(sk_buf + sizeof(uint32_t), sk_buf + 2 * sizeof(uint32_t));
      EncodeFixed32(sk_buf + 2 * sizeof(uint32_t), a);
      std::reverse(sk_buf + 2 * sizeof(uint32_t), sk_buf + sizeof(sk_buf));
      Slice sk(sk_buf, sizeof(sk_buf));
      std::string value;
      s = db_->Get(ropts, sk, &value);
      if (!s.ok()) {
        oss << "Cannot find secondary index entry " << sk.ToString(true);
        VerificationAbort(thread->shared, oss.str(), s);
        assert(false);
        return;
      }
    }
  }

  // Second, iterate secondary index.
  size_t secondary_index_entries_count = 0;
  {
    char buf[4];
    EncodeFixed32(buf, Record::kSecondaryIndexId);
    std::reverse(buf, buf + sizeof(buf));
    const std::string start_key(buf, sizeof(buf));

    // This `ReadOptions` is for validation purposes. Ignore
    // `FLAGS_rate_limit_user_ops` to avoid slowing any validation.
    ReadOptions ropts;
    ropts.snapshot = snapshot;
    ropts.total_order_seek = true;

    std::unique_ptr<Iterator> it(db_->NewIterator(ropts));
    for (it->Seek(start_key); it->Valid(); it->Next()) {
      ++secondary_index_entries_count;
      Record record;
      Status s = record.DecodeSecondaryIndexEntry(it->key(), it->value());
      if (!s.ok()) {
        oss << "Cannot decode secondary index entry";
        VerificationAbort(thread->shared, oss.str(), s);
        assert(false);
        return;
      }
      // After decoding secondary index entry, we know a and c. Crc is verified
      // in decoding phase.
      //
      // Form a primary key and search in the primary index.
      std::string pk = Record::EncodePrimaryKey(record.a_value());
      std::string value;
      s = db_->Get(ropts, pk, &value);
      if (!s.ok()) {
        oss << "Error searching pk " << Slice(pk).ToString(true) << ". "
            << s.ToString();
        VerificationAbort(thread->shared, oss.str(), s);
        assert(false);
        return;
      }
      auto result = Record::DecodePrimaryIndexValue(value);
      s = std::get<0>(result);
      if (!s.ok()) {
        oss << "Error decoding primary index value "
            << Slice(value).ToString(true) << ". " << s.ToString();
        VerificationAbort(thread->shared, oss.str(), s);
        assert(false);
        return;
      }
      uint32_t c_in_primary = std::get<2>(result);
      if (c_in_primary != record.c_value()) {
        oss << "Pk/sk mismatch. pk: (c=" << c_in_primary
            << "), sk: (c=" << record.c_value() << ")";
        VerificationAbort(thread->shared, oss.str(), s);
        assert(false);
        return;
      }
    }
  }

  if (secondary_index_entries_count != primary_index_entries_count) {
    oss << "Pk/sk mismatch: primary index has " << primary_index_entries_count
        << " entries. Secondary index has " << secondary_index_entries_count
        << " entries.";
    VerificationAbort(thread->shared, oss.str(), Status::OK());
    assert(false);
    return;
  }
}

std::pair<uint32_t, uint32_t> MultiOpsTxnsStressTest::ChooseExistingA(
    ThreadState* thread) {
  uint32_t tid = thread->tid;
  auto& key_gen = key_gen_for_a_.at(tid);
  return key_gen->ChooseExisting();
}

uint32_t MultiOpsTxnsStressTest::GenerateNextA(ThreadState* thread) {
  uint32_t tid = thread->tid;
  auto& key_gen = key_gen_for_a_.at(tid);
  return key_gen->Allocate();
}

std::pair<uint32_t, uint32_t> MultiOpsTxnsStressTest::ChooseExistingC(
    ThreadState* thread) {
  uint32_t tid = thread->tid;
  auto& key_gen = key_gen_for_c_.at(tid);
  return key_gen->ChooseExisting();
}

uint32_t MultiOpsTxnsStressTest::GenerateNextC(ThreadState* thread) {
  uint32_t tid = thread->tid;
  auto& key_gen = key_gen_for_c_.at(tid);
  return key_gen->Allocate();
}

std::string MultiOpsTxnsStressTest::KeySpaces::EncodeTo() const {
  std::string result;
  PutFixed32(&result, lb_a);
  PutFixed32(&result, ub_a);
  PutFixed32(&result, lb_c);
  PutFixed32(&result, ub_c);
  return result;
}

bool MultiOpsTxnsStressTest::KeySpaces::DecodeFrom(Slice data) {
  if (!GetFixed32(&data, &lb_a) || !GetFixed32(&data, &ub_a) ||
      !GetFixed32(&data, &lb_c) || !GetFixed32(&data, &ub_c)) {
    return false;
  }
  return true;
}

void MultiOpsTxnsStressTest::PersistKeySpacesDesc(
    const std::string& key_spaces_path, uint32_t lb_a, uint32_t ub_a,
    uint32_t lb_c, uint32_t ub_c) {
  KeySpaces key_spaces(lb_a, ub_a, lb_c, ub_c);
  std::string key_spaces_rep = key_spaces.EncodeTo();

  std::unique_ptr<WritableFile> wfile;
  Status s1 =
      Env::Default()->NewWritableFile(key_spaces_path, &wfile, EnvOptions());
  assert(s1.ok());
  assert(wfile);
  s1 = wfile->Append(key_spaces_rep);
  assert(s1.ok());
}

MultiOpsTxnsStressTest::KeySpaces MultiOpsTxnsStressTest::ReadKeySpacesDesc(
    const std::string& key_spaces_path) {
  KeySpaces key_spaces;
  std::unique_ptr<SequentialFile> sfile;
  Status s1 =
      Env::Default()->NewSequentialFile(key_spaces_path, &sfile, EnvOptions());
  assert(s1.ok());
  assert(sfile);
  char buf[16];
  Slice result;
  s1 = sfile->Read(sizeof(buf), &result, buf);
  assert(s1.ok());
  if (!key_spaces.DecodeFrom(result)) {
    assert(false);
  }
  return key_spaces;
}

// Create an empty database if necessary and preload it with initial test data.
// Key range [lb_a, ub_a), [lb_c, ub_c). The key ranges will be shared by
// 'threads' threads.
// PreloadDb() also sets up KeyGenerator objects for each sub key range
// operated on by each thread.
// Both [lb_a, ub_a) and [lb_c, ub_c) are partitioned. Each thread operates on
// one sub range, using KeyGenerators to generate keys.
// For example, we choose a from [0, 10000) and c from [0, 100). Number of
// threads is 32, their tids range from 0 to 31.
// Thread k chooses a from [312*k,312*(k+1)) and c from [3*k,3*(k+1)) if k<31.
// Thread 31 chooses a from [9672, 10000) and c from [93, 100).
// Within each subrange: a from [low1, high1), c from [low2, high2).
// high1 - low1 > high2 - low2
// We reserve {high1 - 1} and {high2 - 1} as unallocated.
// The records are <low1,low2>, <low1+1,low2+1>, ...,
// <low1+k,low2+k%(high2-low2-1), <low1+k+1,low2+(k+1)%(high2-low2-1)>, ...
void MultiOpsTxnsStressTest::PreloadDb(SharedState* shared, int threads,
                                       uint32_t lb_a, uint32_t ub_a,
                                       uint32_t lb_c, uint32_t ub_c) {
#ifdef ROCKSDB_LITE
  (void)shared;
  (void)threads;
  (void)lb_a;
  (void)ub_a;
  (void)lb_c;
  (void)ub_c;
#else
  key_gen_for_a_.resize(threads);
  key_gen_for_c_.resize(threads);

  assert(ub_a > lb_a && ub_a > lb_a + threads);
  assert(ub_c > lb_c && ub_c > lb_c + threads);

  PersistKeySpacesDesc(FLAGS_key_spaces_path, lb_a, ub_a, lb_c, ub_c);

  fprintf(stdout, "a from [%u, %u), c from [%u, %u)\n",
          static_cast<unsigned int>(lb_a), static_cast<unsigned int>(ub_a),
          static_cast<unsigned int>(lb_c), static_cast<unsigned int>(ub_c));

  const uint32_t num_c = ub_c - lb_c;
  const uint32_t num_c_per_thread = num_c / threads;
  const uint32_t num_a = ub_a - lb_a;
  const uint32_t num_a_per_thread = num_a / threads;

  WriteOptions wopts;
  wopts.disableWAL = FLAGS_disable_wal;
  Random rnd(shared->GetSeed());
  assert(txn_db_);

  std::vector<KeySet> existing_a_uniqs(threads);
  std::vector<KeySet> non_existing_a_uniqs(threads);
  std::vector<KeySet> existing_c_uniqs(threads);
  std::vector<KeySet> non_existing_c_uniqs(threads);

  for (uint32_t a = lb_a; a < ub_a; ++a) {
    uint32_t tid = (a - lb_a) / num_a_per_thread;
    if (tid >= static_cast<uint32_t>(threads)) {
      tid = threads - 1;
    }

    uint32_t a_base = lb_a + tid * num_a_per_thread;
    uint32_t a_hi = (tid < static_cast<uint32_t>(threads - 1))
                        ? (a_base + num_a_per_thread)
                        : ub_a;
    uint32_t a_delta = a - a_base;

    if (a == a_hi - 1) {
      non_existing_a_uniqs[tid].insert(a);
      continue;
    }

    uint32_t c_base = lb_c + tid * num_c_per_thread;
    uint32_t c_hi = (tid < static_cast<uint32_t>(threads - 1))
                        ? (c_base + num_c_per_thread)
                        : ub_c;
    uint32_t c_delta = a_delta % (c_hi - c_base - 1);
    uint32_t c = c_base + c_delta;

    uint32_t b = rnd.Next();
    Record record(a, b, c);
    WriteBatch wb;
    const auto primary_index_entry = record.EncodePrimaryIndexEntry();
    Status s = wb.Put(primary_index_entry.first, primary_index_entry.second);
    assert(s.ok());

    const auto secondary_index_entry = record.EncodeSecondaryIndexEntry();
    s = wb.Put(secondary_index_entry.first, secondary_index_entry.second);
    assert(s.ok());

    s = txn_db_->Write(wopts, &wb);
    assert(s.ok());

    // TODO (yanqin): make the following check optional, especially when data
    // size is large.
    Record tmp_rec;
    tmp_rec.SetB(record.b_value());
    s = tmp_rec.DecodeSecondaryIndexEntry(secondary_index_entry.first,
                                          secondary_index_entry.second);
    assert(s.ok());
    assert(tmp_rec == record);

    existing_a_uniqs[tid].insert(a);
    existing_c_uniqs[tid].insert(c);
  }

  for (int i = 0; i < threads; ++i) {
    uint32_t my_seed = i + shared->GetSeed();

    auto& key_gen_for_a = key_gen_for_a_[i];
    assert(!key_gen_for_a);
    uint32_t low = lb_a + i * num_a_per_thread;
    uint32_t high = (i < threads - 1) ? (low + num_a_per_thread) : ub_a;
    assert(existing_a_uniqs[i].size() == high - low - 1);
    assert(non_existing_a_uniqs[i].size() == 1);
    key_gen_for_a = std::make_unique<KeyGenerator>(
        my_seed, low, high, std::move(existing_a_uniqs[i]),
        std::move(non_existing_a_uniqs[i]));

    auto& key_gen_for_c = key_gen_for_c_[i];
    assert(!key_gen_for_c);
    low = lb_c + i * num_c_per_thread;
    high = (i < threads - 1) ? (low + num_c_per_thread) : ub_c;
    non_existing_c_uniqs[i].insert(high - 1);
    assert(existing_c_uniqs[i].size() == high - low - 1);
    assert(non_existing_c_uniqs[i].size() == 1);
    key_gen_for_c = std::make_unique<KeyGenerator>(
        my_seed, low, high, std::move(existing_c_uniqs[i]),
        std::move(non_existing_c_uniqs[i]));
  }
#endif  // !ROCKSDB_LITE
}

// Scan an existing, non-empty database.
// Set up [lb_a, ub_a) and [lb_c, ub_c) as test key ranges.
// Set up KeyGenerator objects for each sub key range operated on by each
// thread.
// Scan the entire database and for each subrange, populate the existing keys
// and non-existing keys. We currently require the non-existing keys be
// non-empty after initialization.
void MultiOpsTxnsStressTest::ScanExistingDb(SharedState* shared, int threads) {
  key_gen_for_a_.resize(threads);
  key_gen_for_c_.resize(threads);

  KeySpaces key_spaces = ReadKeySpacesDesc(FLAGS_key_spaces_path);

  const uint32_t lb_a = key_spaces.lb_a;
  const uint32_t ub_a = key_spaces.ub_a;
  const uint32_t lb_c = key_spaces.lb_c;
  const uint32_t ub_c = key_spaces.ub_c;

  assert(lb_a < ub_a && lb_c < ub_c);

  fprintf(stdout, "a from [%u, %u), c from [%u, %u)\n",
          static_cast<unsigned int>(lb_a), static_cast<unsigned int>(ub_a),
          static_cast<unsigned int>(lb_c), static_cast<unsigned int>(ub_c));

  assert(ub_a > lb_a && ub_a > lb_a + threads);
  assert(ub_c > lb_c && ub_c > lb_c + threads);

  const uint32_t num_c = ub_c - lb_c;
  const uint32_t num_c_per_thread = num_c / threads;
  const uint32_t num_a = ub_a - lb_a;
  const uint32_t num_a_per_thread = num_a / threads;

  assert(db_);
  ReadOptions ropts;
  std::vector<KeySet> existing_a_uniqs(threads);
  std::vector<KeySet> non_existing_a_uniqs(threads);
  std::vector<KeySet> existing_c_uniqs(threads);
  std::vector<KeySet> non_existing_c_uniqs(threads);
  {
    std::string pk_lb_str = Record::EncodePrimaryKey(0);
    std::string pk_ub_str =
        Record::EncodePrimaryKey(std::numeric_limits<uint32_t>::max());
    Slice pk_lb = pk_lb_str;
    Slice pk_ub = pk_ub_str;
    ropts.iterate_lower_bound = &pk_lb;
    ropts.iterate_upper_bound = &pk_ub;
    ropts.total_order_seek = true;
    std::unique_ptr<Iterator> it(db_->NewIterator(ropts));

    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      Record record;
      Status s = record.DecodePrimaryIndexEntry(it->key(), it->value());
      if (!s.ok()) {
        fprintf(stderr, "Cannot decode primary index entry: %s\n",
                s.ToString().c_str());
        assert(false);
      }
      uint32_t a = record.a_value();
      assert(a >= lb_a);
      assert(a < ub_a);
      uint32_t tid = (a - lb_a) / num_a_per_thread;
      if (tid >= static_cast<uint32_t>(threads)) {
        tid = threads - 1;
      }

      existing_a_uniqs[tid].insert(a);

      uint32_t c = record.c_value();
      assert(c >= lb_c);
      assert(c < ub_c);
      tid = (c - lb_c) / num_c_per_thread;
      if (tid >= static_cast<uint32_t>(threads)) {
        tid = threads - 1;
      }
      auto& existing_c_uniq = existing_c_uniqs[tid];
      existing_c_uniq.insert(c);
    }

    for (uint32_t a = lb_a; a < ub_a; ++a) {
      uint32_t tid = (a - lb_a) / num_a_per_thread;
      if (tid >= static_cast<uint32_t>(threads)) {
        tid = threads - 1;
      }
      if (0 == existing_a_uniqs[tid].count(a)) {
        non_existing_a_uniqs[tid].insert(a);
      }
    }

    for (uint32_t c = lb_c; c < ub_c; ++c) {
      uint32_t tid = (c - lb_c) / num_c_per_thread;
      if (tid >= static_cast<uint32_t>(threads)) {
        tid = threads - 1;
      }
      if (0 == existing_c_uniqs[tid].count(c)) {
        non_existing_c_uniqs[tid].insert(c);
      }
    }

    for (int i = 0; i < threads; ++i) {
      uint32_t my_seed = i + shared->GetSeed();
      auto& key_gen_for_a = key_gen_for_a_[i];
      assert(!key_gen_for_a);
      uint32_t low = lb_a + i * num_a_per_thread;
      uint32_t high = (i < threads - 1) ? (low + num_a_per_thread) : ub_a;

      // The following two assertions assume the test thread count and key
      // space remain the same across different runs. Will need to relax.
      assert(existing_a_uniqs[i].size() == high - low - 1);
      assert(non_existing_a_uniqs[i].size() == 1);

      key_gen_for_a = std::make_unique<KeyGenerator>(
          my_seed, low, high, std::move(existing_a_uniqs[i]),
          std::move(non_existing_a_uniqs[i]));

      auto& key_gen_for_c = key_gen_for_c_[i];
      assert(!key_gen_for_c);
      low = lb_c + i * num_c_per_thread;
      high = (i < threads - 1) ? (low + num_c_per_thread) : ub_c;

      // The following two assertions assume the test thread count and key
      // space remain the same across different runs. Will need to relax.
      assert(existing_c_uniqs[i].size() == high - low - 1);
      assert(non_existing_c_uniqs[i].size() == 1);

      key_gen_for_c = std::make_unique<KeyGenerator>(
          my_seed, low, high, std::move(existing_c_uniqs[i]),
          std::move(non_existing_c_uniqs[i]));
    }
  }
}

StressTest* CreateMultiOpsTxnsStressTest() {
  return new MultiOpsTxnsStressTest();
}

void CheckAndSetOptionsForMultiOpsTxnStressTest() {
#ifndef ROCKSDB_LITE
  if (FLAGS_test_batches_snapshots || FLAGS_test_cf_consistency) {
    fprintf(stderr,
            "-test_multi_ops_txns is not compatible with "
            "-test_bathces_snapshots and -test_cf_consistency\n");
    exit(1);
  }
  if (!FLAGS_use_txn) {
    fprintf(stderr, "-use_txn must be true if -test_multi_ops_txns\n");
    exit(1);
  } else if (FLAGS_test_secondary > 0) {
    fprintf(
        stderr,
        "secondary instance does not support replaying logs (MANIFEST + WAL) "
        "of TransactionDB with write-prepared/write-unprepared policy\n");
    exit(1);
  }
  if (FLAGS_clear_column_family_one_in > 0) {
    fprintf(stderr,
            "-test_multi_ops_txns is not compatible with clearing column "
            "families\n");
    exit(1);
  }
  if (FLAGS_column_families > 1) {
    // TODO (yanqin) support separating primary index and secondary index in
    // different column families.
    fprintf(stderr,
            "-test_multi_ops_txns currently does not use more than one column "
            "family\n");
    exit(1);
  }
  if (FLAGS_writepercent > 0 || FLAGS_delpercent > 0 ||
      FLAGS_delrangepercent > 0) {
    fprintf(stderr,
            "-test_multi_ops_txns requires that -writepercent, -delpercent and "
            "-delrangepercent be 0\n");
    exit(1);
  }
  if (FLAGS_key_spaces_path.empty()) {
    fprintf(stderr,
            "Must specify a file to store ranges of A and C via "
            "-key_spaces_path\n");
    exit(1);
  }
#else
  fprintf(stderr, "-test_multi_ops_txns not supported in ROCKSDB_LITE mode\n");
  exit(1);
#endif  // !ROCKSDB_LITE
}
}  // namespace ROCKSDB_NAMESPACE

#endif  // GFLAGS
