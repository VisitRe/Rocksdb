//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#ifndef OS_WIN
#include <unistd.h>
#endif  // ! OS_WIN

#include "benchmark/benchmark.h"
#include "db/db_impl/db_impl.h"
#include "rocksdb/db.h"
#include "rocksdb/filter_policy.h"
#include "rocksdb/options.h"
#include "table/block_based/block.h"
#include "table/block_based/block_builder.h"
#include "util/random.h"

namespace ROCKSDB_NAMESPACE {

class KeyGenerator {
 public:
  // Generate next key
  // buff: the caller needs to make sure there's enough space for generated key
  // offset: to control the group of the key, 0 means normal key, 1 means
  // non-existing key, 2 is reserved prefix_only: only return a prefix
  Slice Next(char* buff, int8_t offset = 0, bool prefix_only = false) {
    assert(max_key_ < std::numeric_limits<uint32_t>::max() /
                          MULTIPLIER);  // TODO: add large key support

    uint32_t k;
    if (is_sequential_) {
      assert(next_sequential_key_ < max_key_);
      k = (next_sequential_key_ % max_key_) * MULTIPLIER + offset;
      next_sequential_key_++;
    } else {
      k = (rnd_->Next() % max_key_) * MULTIPLIER + offset;
    }
    // TODO: make sure the buff is large enough
    memset(buff, 0, key_size_);
    if (prefix_num_ > 0) {
      uint32_t prefix = (k % prefix_num_) * MULTIPLIER + offset;
      Encode(buff, prefix);
      if (prefix_only) {
        return {buff, prefix_size_};
      }
    }
    Encode(buff + prefix_size_, k);
    return {buff, key_size_};
  }

  // use internal buffer for generated key, make sure there's only one caller in
  // single thread
  Slice Next() { return Next(buff_); }

  // user internal buffer for generated prefix
  Slice NextPrefix() {
    assert(prefix_num_ > 0);
    return Next(buff_, 0, true);
  }

  // helper function to get non exist key
  Slice NextNonExist() { return Next(buff_, 1); }

  Slice MaxKey(char* buff) const {
    memset(buff, 0xff, key_size_);
    return {buff, key_size_};
  }

  Slice MinKey(char* buff) const {
    memset(buff, 0, key_size_);
    return {buff, key_size_};
  }

  // max_key: the max key that it could generate
  // prefix_num: the max prefix number
  // key_size: in bytes
  explicit KeyGenerator(Random* rnd, uint64_t max_key = 100 * 1024 * 1024,
                        size_t prefix_num = 0, size_t key_size = 10) {
    prefix_num_ = prefix_num;
    key_size_ = key_size;
    max_key_ = max_key;
    rnd_ = rnd;
    if (prefix_num > 0) {
      prefix_size_ = 4;  // TODO: support different prefix_size
    }
  }

  // generate sequential keys
  explicit KeyGenerator(uint64_t max_key = 100 * 1024 * 1024,
                        size_t key_size = 10) {
    key_size_ = key_size;
    max_key_ = max_key;
    rnd_ = nullptr;
    is_sequential_ = true;
  }

 private:
  Random* rnd_;
  size_t prefix_num_ = 0;
  size_t prefix_size_ = 0;
  size_t key_size_;
  uint64_t max_key_;
  bool is_sequential_ = false;
  uint32_t next_sequential_key_ = 0;
  char buff_[256] = {0};
  const int MULTIPLIER = 3;

  void static Encode(char* buf, uint32_t value) {
    if (port::kLittleEndian) {
      buf[0] = static_cast<char>((value >> 24) & 0xff);
      buf[1] = static_cast<char>((value >> 16) & 0xff);
      buf[2] = static_cast<char>((value >> 8) & 0xff);
      buf[3] = static_cast<char>(value & 0xff);
    } else {
      memcpy(buf, &value, sizeof(value));
    }
  }
};

static void SetupDB(benchmark::State& state, Options& options, DB** dpptr,
                    const std::string& test_name = "") {
  options.create_if_missing = true;
  auto env = Env::Default();
  std::string db_path;
  Status s = env->GetTestDirectory(&db_path);
  if (!s.ok()) {
    state.SkipWithError(s.ToString().c_str());
    return;
  }
  std::string db_name = db_path + "/" + test_name + std::to_string(getpid());
  DestroyDB(db_name, options);

  s = DB::Open(options, db_name, dpptr);
  if (!s.ok()) {
    state.SkipWithError(s.ToString().c_str());
    return;
  }
}

static void TeardownDB(benchmark::State& state, DB* db, const Options& options,
                       KeyGenerator& kg) {
  char min_buff[256], max_buff[256];
  const Range r(kg.MinKey(min_buff), kg.MaxKey(max_buff));
  uint64_t size;
  Status s = db->GetApproximateSizes(&r, 1, &size);
  if (!s.ok()) {
    state.SkipWithError(s.ToString().c_str());
  }
  state.counters["db_size"] = static_cast<double>(size);

  std::string db_name = db->GetName();
  s = db->Close();
  if (!s.ok()) {
    state.SkipWithError(s.ToString().c_str());
  }
  DestroyDB(db_name, options);
}

static void DBOpen(benchmark::State& state) {
  // create DB
  DB* db = nullptr;
  Options options;
  SetupDB(state, options, &db, "DBOpen");

  std::string db_name = db->GetName();
  db->Close();

  options.create_if_missing = false;

  auto rnd = Random(123);

  for (auto _ : state) {
    Status s = DB::Open(options, db_name, &db);
    if (!s.ok()) {
      state.SkipWithError(s.ToString().c_str());
    }
    state.PauseTiming();
    auto wo = WriteOptions();
    for (int i = 0; i < 2; i++) {
      for (int j = 0; j < 100; j++) {
        s = db->Put(wo, rnd.RandomString(10), rnd.RandomString(100));
        if (!s.ok()) {
          state.SkipWithError(s.ToString().c_str());
        }
      }
      s = db->Flush(FlushOptions());
    }
    if (!s.ok()) {
      state.SkipWithError(s.ToString().c_str());
    }
    s = db->Close();
    if (!s.ok()) {
      state.SkipWithError(s.ToString().c_str());
    }
    state.ResumeTiming();
  }
  DestroyDB(db_name, options);
}

BENCHMARK(DBOpen)->Iterations(200);  // specify iteration number as the db size
                                     // is impacted by iteration number

static void DBClose(benchmark::State& state) {
  // create DB
  DB* db;
  Options options;
  SetupDB(state, options, &db, "DBClose");

  std::string db_name = db->GetName();
  db->Close();

  options.create_if_missing = false;

  auto rnd = Random(12345);

  for (auto _ : state) {
    state.PauseTiming();
    Status s = DB::Open(options, db_name, &db);
    if (!s.ok()) {
      state.SkipWithError(s.ToString().c_str());
    }
    auto wo = WriteOptions();
    for (int i = 0; i < 2; i++) {
      for (int j = 0; j < 100; j++) {
        s = db->Put(wo, rnd.RandomString(10), rnd.RandomString(100));
        if (!s.ok()) {
          state.SkipWithError(s.ToString().c_str());
        }
      }
      s = db->Flush(FlushOptions());
    }
    if (!s.ok()) {
      state.SkipWithError(s.ToString().c_str());
    }
    state.ResumeTiming();
    s = db->Close();
    if (!s.ok()) {
      state.SkipWithError(s.ToString().c_str());
    }
  }
  DestroyDB(db_name, options);
}

BENCHMARK(DBClose)->Iterations(200);  // specify iteration number as the db size
                                      // is impacted by iteration number

static void DBPut(benchmark::State& state) {
  auto compaction_style = static_cast<CompactionStyle>(state.range(0));
  uint64_t max_data = state.range(1);
  uint64_t per_key_size = state.range(2);
  bool enable_statistics = state.range(3);
  bool enable_wal = state.range(4);
  uint64_t key_num = max_data / per_key_size;

  // setup DB
  static DB* db = nullptr;
  Options options;
  if (enable_statistics) {
    options.statistics = CreateDBStatistics();
  }
  options.compaction_style = compaction_style;

  auto rnd = Random(301 + state.thread_index);
  KeyGenerator kg(&rnd, key_num);

  if (state.thread_index == 0) {
    SetupDB(state, options, &db, "DBPut");
  }

  auto wo = WriteOptions();
  wo.disableWAL = !enable_wal;

  for (auto _ : state) {
    state.PauseTiming();
    Slice key = kg.Next();
    std::string val = rnd.RandomString(static_cast<int>(per_key_size));
    state.ResumeTiming();
    Status s = db->Put(wo, key, val);
    if (!s.ok()) {
      state.SkipWithError(s.ToString().c_str());
    }
  }

  if (state.thread_index == 0) {
    auto db_full = static_cast_with_check<DBImpl>(db);
    Status s = db_full->WaitForCompact(true);
    if (!s.ok()) {
      state.SkipWithError(s.ToString().c_str());
      return;
    }
    if (enable_statistics) {
      HistogramData histogram_data;
      options.statistics->histogramData(DB_WRITE, &histogram_data);
      state.counters["put_mean"] = histogram_data.average;
      state.counters["put_p95"] = histogram_data.percentile95;
      state.counters["put_p99"] = histogram_data.percentile99;
    }

    TeardownDB(state, db, options, kg);
  }
}

static void DBPutArguments(benchmark::internal::Benchmark* b) {
  for (int comp_style : {kCompactionStyleLevel, kCompactionStyleUniversal,
                         kCompactionStyleFIFO}) {
    for (int64_t max_data : {100l << 30}) {
      for (int64_t per_key_size : {256, 1024}) {
        for (bool enable_statistics : {false, true}) {
          for (bool wal : {false, true}) {
            b->Args(
                {comp_style, max_data, per_key_size, enable_statistics, wal});
          }
        }
      }
    }
  }
  b->ArgNames(
      {"comp_style", "max_data", "per_key_size", "enable_statistics", "wal"});
}

static const uint64_t DBPutNum = 10l << 10;
BENCHMARK(DBPut)->Threads(1)->Iterations(DBPutNum)->Apply(DBPutArguments);
BENCHMARK(DBPut)->Threads(8)->Iterations(DBPutNum / 8)->Apply(DBPutArguments);

static void ManualCompaction(benchmark::State& state) {
  auto compaction_style = static_cast<CompactionStyle>(state.range(0));
  uint64_t max_data = state.range(1);
  uint64_t per_key_size = state.range(2);
  bool enable_statistics = state.range(3);
  uint64_t key_num = max_data / per_key_size;

  // setup DB
  static DB* db;
  Options options;
  if (enable_statistics) {
    options.statistics = CreateDBStatistics();
  }
  options.compaction_style = compaction_style;
  // No auto compaction
  options.disable_auto_compactions = true;
  options.level0_file_num_compaction_trigger = (1 << 30);
  options.level0_slowdown_writes_trigger = (1 << 30);
  options.level0_stop_writes_trigger = (1 << 30);
  options.soft_pending_compaction_bytes_limit = 0;
  options.hard_pending_compaction_bytes_limit = 0;

  auto rnd = Random(301 + state.thread_index);
  KeyGenerator kg(&rnd, key_num);

  if (state.thread_index == 0) {
    SetupDB(state, options, &db, "ManualCompaction");
  }

  auto wo = WriteOptions();
  wo.disableWAL = true;
  uint64_t flush_mod = key_num / 4;  // at least generate 4 files for compaction
  for (uint64_t i = 0; i < key_num; i++) {
    Status s = db->Put(wo, kg.Next(),
                       rnd.RandomString(static_cast<int>(per_key_size)));
    if (!s.ok()) {
      state.SkipWithError(s.ToString().c_str());
    }
    if (i + 1 % flush_mod == 0) {
      s = db->Flush(FlushOptions());
    }
  }
  FlushOptions fo;
  Status s = db->Flush(fo);
  if (!s.ok()) {
    state.SkipWithError(s.ToString().c_str());
  }
  std::vector<LiveFileMetaData> files_meta;
  db->GetLiveFilesMetaData(&files_meta);
  std::vector<std::string> files_before_compact;
  files_before_compact.reserve(files_meta.size());
  for (const LiveFileMetaData& file : files_meta) {
    files_before_compact.emplace_back(file.name);
  }

  SetPerfLevel(kEnableTime);
  get_perf_context()->EnablePerLevelPerfContext();
  get_perf_context()->Reset();
  CompactionOptions co;
  for (auto _ : state) {
    s = db->CompactFiles(co, files_before_compact, 1);
    if (!s.ok()) {
      state.SkipWithError(s.ToString().c_str());
    }
  }

  if (state.thread_index == 0) {
    auto db_full = static_cast_with_check<DBImpl>(db);
    s = db_full->WaitForCompact(true);
    if (!s.ok()) {
      state.SkipWithError(s.ToString().c_str());
      return;
    }
    if (enable_statistics) {
      HistogramData histogram_data;
      options.statistics->histogramData(COMPACTION_TIME, &histogram_data);
      state.counters["comp_time"] = histogram_data.average;
      options.statistics->histogramData(COMPACTION_CPU_TIME, &histogram_data);
      state.counters["comp_cpu_time"] = histogram_data.average;
      options.statistics->histogramData(COMPACTION_OUTFILE_SYNC_MICROS,
                                        &histogram_data);
      state.counters["comp_outfile_sync"] = histogram_data.average;

      state.counters["comp_read"] = static_cast<double>(
          options.statistics->getTickerCount(COMPACT_READ_BYTES));
      state.counters["comp_write"] = static_cast<double>(
          options.statistics->getTickerCount(COMPACT_WRITE_BYTES));

      state.counters["user_key_comparison_count"] =
          static_cast<double>(get_perf_context()->user_key_comparison_count);
      state.counters["block_read_count"] =
          static_cast<double>(get_perf_context()->block_read_count);
      state.counters["block_read_time"] =
          static_cast<double>(get_perf_context()->block_read_time);
      state.counters["block_checksum_time"] =
          static_cast<double>(get_perf_context()->block_checksum_time);
      state.counters["new_table_block_iter_nanos"] =
          static_cast<double>(get_perf_context()->new_table_block_iter_nanos);
      state.counters["new_table_iterator_nanos"] =
          static_cast<double>(get_perf_context()->new_table_iterator_nanos);
      state.counters["find_table_nanos"] =
          static_cast<double>(get_perf_context()->find_table_nanos);
    }

    TeardownDB(state, db, options, kg);
  }
}

static void ManualCompactionArguments(benchmark::internal::Benchmark* b) {
  for (int comp_style : {kCompactionStyleLevel, kCompactionStyleUniversal}) {
    for (int64_t max_data : {32l << 20, 128l << 20}) {
      for (int64_t per_key_size : {256, 1024}) {
        for (bool enable_statistics : {false, true}) {
          b->Args({comp_style, max_data, per_key_size, enable_statistics});
        }
      }
    }
  }
  b->ArgNames({"comp_style", "max_data", "per_key_size", "enable_statistics"});
}

BENCHMARK(ManualCompaction)->Iterations(1)->Apply(ManualCompactionArguments);

static void ManualFlush(benchmark::State& state) {
  uint64_t key_num = state.range(0);
  uint64_t per_key_size = state.range(1);
  bool enable_statistics = true;

  // setup DB
  static DB* db;
  Options options;
  if (enable_statistics) {
    options.statistics = CreateDBStatistics();
  }
  options.disable_auto_compactions = true;
  options.level0_file_num_compaction_trigger = (1 << 30);
  options.level0_slowdown_writes_trigger = (1 << 30);
  options.level0_stop_writes_trigger = (1 << 30);
  options.soft_pending_compaction_bytes_limit = 0;
  options.hard_pending_compaction_bytes_limit = 0;
  options.write_buffer_size = 2l << 30;  // 2G to avoid auto flush

  auto rnd = Random(301 + state.thread_index);
  KeyGenerator kg(&rnd, key_num);

  if (state.thread_index == 0) {
    SetupDB(state, options, &db, "ManualFlush");
  }

  auto wo = WriteOptions();
  for (auto _ : state) {
    state.PauseTiming();
    for (uint64_t i = 0; i < key_num; i++) {
      Status s = db->Put(wo, kg.Next(),
                         rnd.RandomString(static_cast<int>(per_key_size)));
    }
    FlushOptions fo;
    state.ResumeTiming();
    Status s = db->Flush(fo);
    if (!s.ok()) {
      state.SkipWithError(s.ToString().c_str());
    }
  }

  if (state.thread_index == 0) {
    auto db_full = static_cast_with_check<DBImpl>(db);
    Status s = db_full->WaitForCompact(true);
    if (!s.ok()) {
      state.SkipWithError(s.ToString().c_str());
      return;
    }
    if (enable_statistics) {
      HistogramData histogram_data;
      options.statistics->histogramData(FLUSH_TIME, &histogram_data);
      state.counters["flush_time"] = histogram_data.average;
      state.counters["flush_write_bytes"] = static_cast<double>(
          options.statistics->getTickerCount(FLUSH_WRITE_BYTES));
    }

    TeardownDB(state, db, options, kg);
  }
}

static void ManualFlushArguments(benchmark::internal::Benchmark* b) {
  for (int64_t key_num : {1l << 10, 8l << 10, 64l << 10}) {
    for (int64_t per_key_size : {256, 1024}) {
      b->Args({key_num, per_key_size});
    }
  }
  b->ArgNames({"key_num", "per_key_size"});
}

BENCHMARK(ManualFlush)->Iterations(1)->Apply(ManualFlushArguments);

static void DBGet(benchmark::State& state) {
  auto compaction_style = static_cast<CompactionStyle>(state.range(0));
  uint64_t max_data = state.range(1);
  uint64_t per_key_size = state.range(2);
  bool enable_statistics = state.range(3);
  bool negative_query = state.range(4);
  bool enable_filter = state.range(5);
  uint64_t key_num = max_data / per_key_size;

  // setup DB
  static DB* db;
  Options options;
  if (enable_statistics) {
    options.statistics = CreateDBStatistics();
  }
  options.compaction_style = compaction_style;

  if (enable_filter) {
    BlockBasedTableOptions table_options;
    table_options.filter_policy.reset(NewBloomFilterPolicy(10, false));
    options.table_factory.reset(NewBlockBasedTableFactory(table_options));
  }

  auto rnd = Random(301 + state.thread_index);
  KeyGenerator kg(&rnd, key_num);

  if (state.thread_index == 0) {
    SetupDB(state, options, &db, "DBGet");

    // load db
    auto wo = WriteOptions();
    wo.disableWAL = true;
    for (uint64_t i = 0; i < key_num; i++) {
      Status s = db->Put(wo, kg.Next(),
                         rnd.RandomString(static_cast<int>(per_key_size)));
      if (!s.ok()) {
        state.SkipWithError(s.ToString().c_str());
      }
    }

    FlushOptions fo;
    Status s = db->Flush(fo);
    if (!s.ok()) {
      state.SkipWithError(s.ToString().c_str());
    }

    auto db_full = static_cast_with_check<DBImpl>(db);
    s = db_full->WaitForCompact(true);
    if (!s.ok()) {
      state.SkipWithError(s.ToString().c_str());
      return;
    }
  }

  auto ro = ReadOptions();
  size_t not_found = 0;
  if (negative_query) {
    for (auto _ : state) {
      std::string val;
      Status s = db->Get(ro, kg.NextNonExist(), &val);
      if (s.IsNotFound()) {
        not_found++;
      }
    }
  } else {
    for (auto _ : state) {
      std::string val;
      Status s = db->Get(ro, kg.Next(), &val);
      if (s.IsNotFound()) {
        not_found++;
      }
    }
  }

  state.counters["neg_qu_pct"] = benchmark::Counter(
      static_cast<double>(not_found * 100), benchmark::Counter::kAvgIterations);

  if (state.thread_index == 0) {
    if (enable_statistics) {
      HistogramData histogram_data;
      options.statistics->histogramData(DB_GET, &histogram_data);
      state.counters["get_mean"] = histogram_data.average;
      state.counters["get_p95"] = histogram_data.percentile95;
      state.counters["get_p99"] = histogram_data.percentile99;
    }

    TeardownDB(state, db, options, kg);
  }
}

static void DBGetArguments(benchmark::internal::Benchmark* b) {
  for (int comp_style : {kCompactionStyleLevel, kCompactionStyleUniversal,
                         kCompactionStyleFIFO}) {
    for (int64_t max_data : {128l << 20, 512l << 20}) {
      for (int64_t per_key_size : {256, 1024}) {
        for (bool enable_statistics : {false, true}) {
          for (bool negative_query : {false, true}) {
            for (bool enable_filter : {false, true}) {
              b->Args({comp_style, max_data, per_key_size, enable_statistics,
                       negative_query, enable_filter});
            }
          }
        }
      }
    }
  }
  b->ArgNames({"comp_style", "max_data", "per_key_size", "enable_statistics",
               "negative_query", "enable_filter"});
}

static constexpr uint64_t kDBGetNum = 10l << 10;
BENCHMARK(DBGet)->Threads(1)->Iterations(kDBGetNum)->Apply(DBGetArguments);
BENCHMARK(DBGet)->Threads(8)->Iterations(kDBGetNum / 8)->Apply(DBGetArguments);

static void SimpleGetWithPerfContext(benchmark::State& state) {
  // setup DB
  static DB* db;
  std::string db_name;
  Options options;
  options.create_if_missing = true;
  options.arena_block_size = 8 << 20;

  auto rnd = Random(301 + state.thread_index);
  KeyGenerator kg(&rnd, 1024);

  if (state.thread_index == 0) {
    auto env = Env::Default();
    std::string db_path;
    Status s = env->GetTestDirectory(&db_path);
    if (!s.ok()) {
      state.SkipWithError(s.ToString().c_str());
      return;
    }
    db_name = db_path + "/simple_get_" + std::to_string(getpid());
    DestroyDB(db_name, options);

    s = DB::Open(options, db_name, &db);
    if (!s.ok()) {
      state.SkipWithError(s.ToString().c_str());
      return;
    }
    // load db
    auto wo = WriteOptions();
    wo.disableWAL = true;
    for (uint64_t i = 0; i < 1024; i++) {
      s = db->Put(wo, kg.Next(), rnd.RandomString(1024));
      if (!s.ok()) {
        state.SkipWithError(s.ToString().c_str());
      }
    }
    auto db_full = static_cast_with_check<DBImpl>(db);
    s = db_full->WaitForCompact(true);
    if (!s.ok()) {
      state.SkipWithError(s.ToString().c_str());
      return;
    }
    FlushOptions fo;
    s = db->Flush(fo);
    if (!s.ok()) {
      state.SkipWithError(s.ToString().c_str());
    }
  }

  auto ro = ReadOptions();
  size_t not_found = 0;
  uint64_t user_key_comparison_count = 0;
  uint64_t block_read_time = 0;
  uint64_t block_checksum_time = 0;
  uint64_t get_snapshot_time = 0;
  uint64_t get_post_process_time = 0;
  uint64_t get_from_output_files_time = 0;
  uint64_t new_table_block_iter_nanos = 0;
  uint64_t block_seek_nanos = 0;
  uint64_t get_cpu_nanos = 0;
  uint64_t get_from_table_nanos = 0;
  SetPerfLevel(kEnableTime);
  get_perf_context()->EnablePerLevelPerfContext();
  for (auto _ : state) {
    std::string val;
    get_perf_context()->Reset();
    Status s = db->Get(ro, kg.NextNonExist(), &val);
    if (s.IsNotFound()) {
      not_found++;
    }
    user_key_comparison_count += get_perf_context()->user_key_comparison_count;
    block_read_time += get_perf_context()->block_read_time;
    block_checksum_time += get_perf_context()->block_checksum_time;
    get_snapshot_time += get_perf_context()->get_snapshot_time;
    get_post_process_time += get_perf_context()->get_post_process_time;
    get_from_output_files_time +=
        get_perf_context()->get_from_output_files_time;
    new_table_block_iter_nanos +=
        get_perf_context()->new_table_block_iter_nanos;
    block_seek_nanos += get_perf_context()->block_seek_nanos;
    get_cpu_nanos += get_perf_context()->get_cpu_nanos;
    get_from_table_nanos +=
        (*(get_perf_context()->level_to_perf_context))[0].get_from_table_nanos;
  }

  state.counters["neg_qu_pct"] = benchmark::Counter(
      static_cast<double>(not_found * 100), benchmark::Counter::kAvgIterations);
  state.counters["user_key_comparison_count"] =
      benchmark::Counter(static_cast<double>(user_key_comparison_count),
                         benchmark::Counter::kAvgIterations);
  state.counters["block_read_time"] = benchmark::Counter(
      static_cast<double>(block_read_time), benchmark::Counter::kAvgIterations);
  state.counters["block_checksum_time"] =
      benchmark::Counter(static_cast<double>(block_checksum_time),
                         benchmark::Counter::kAvgIterations);
  state.counters["get_snapshot_time"] =
      benchmark::Counter(static_cast<double>(get_snapshot_time),
                         benchmark::Counter::kAvgIterations);
  state.counters["get_post_process_time"] =
      benchmark::Counter(static_cast<double>(get_post_process_time),
                         benchmark::Counter::kAvgIterations);
  state.counters["get_from_output_files_time"] =
      benchmark::Counter(static_cast<double>(get_from_output_files_time),
                         benchmark::Counter::kAvgIterations);
  state.counters["new_table_block_iter_nanos"] =
      benchmark::Counter(static_cast<double>(new_table_block_iter_nanos),
                         benchmark::Counter::kAvgIterations);
  state.counters["block_seek_nanos"] =
      benchmark::Counter(static_cast<double>(block_seek_nanos),
                         benchmark::Counter::kAvgIterations);
  state.counters["get_cpu_nanos"] = benchmark::Counter(
      static_cast<double>(get_cpu_nanos), benchmark::Counter::kAvgIterations);
  state.counters["get_from_table_nanos"] =
      benchmark::Counter(static_cast<double>(get_from_table_nanos),
                         benchmark::Counter::kAvgIterations);

  if (state.thread_index == 0) {
    TeardownDB(state, db, options, kg);
  }
}

BENCHMARK(SimpleGetWithPerfContext)->Iterations(1000000);

std::string GenerateKey(int primary_key, int secondary_key, int padding_size,
                        Random* rnd) {
  char buf[50];
  char* p = &buf[0];
  snprintf(buf, sizeof(buf), "%6d%4d", primary_key, secondary_key);
  std::string k(p);
  if (padding_size) {
    k += rnd->RandomString(padding_size);
  }

  return k;
}

void GenerateRandomKVs(std::vector<std::string>* keys,
                       std::vector<std::string>* values, const int from,
                       const int len, const int step = 1,
                       const int padding_size = 0,
                       const int keys_share_prefix = 1) {
  Random rnd(302);

  // generate different prefix
  for (int i = from; i < from + len; i += step) {
    // generating keys that share the prefix
    for (int j = 0; j < keys_share_prefix; ++j) {
      keys->emplace_back(GenerateKey(i, j, padding_size, &rnd));
      // 100 bytes values
      values->emplace_back(rnd.RandomString(100));
    }
  }
}

static void DataBlockSeek(benchmark::State& state) {
  Random rnd(301);
  Options options = Options();

  BlockBuilder builder(16, true, false,
                       BlockBasedTableOptions::kDataBlockBinarySearch);

  int num_records = 500;
  std::vector<std::string> keys;
  std::vector<std::string> values;

  GenerateRandomKVs(&keys, &values, 0, num_records);

  for (int i = 0; i < num_records; i++) {
    std::string ukey(keys[i] + "1");
    InternalKey ikey(ukey, 0, kTypeValue);
    builder.Add(ikey.Encode().ToString(), values[i]);
  }

  Slice rawblock = builder.Finish();

  BlockContents contents;
  contents.data = rawblock;
  Block reader(std::move(contents));

  SetPerfLevel(kEnableTime);
  uint64_t total = 0;
  for (auto _ : state) {
    DataBlockIter* iter = reader.NewDataIterator(options.comparator,
                                                 kDisableGlobalSequenceNumber);
    uint32_t index = rnd.Uniform(static_cast<int>(num_records));
    std::string ukey(keys[index] + "1");
    InternalKey ikey(ukey, 0, kTypeValue);
    get_perf_context()->Reset();
    bool may_exist = iter->SeekForGet(ikey.Encode().ToString());
    if (!may_exist) {
      state.SkipWithError("key not found");
    }
    total += get_perf_context()->block_seek_nanos;
    delete iter;
  }
  state.counters["seek_ns"] = benchmark::Counter(
      static_cast<double>(total), benchmark::Counter::kAvgIterations);
}

BENCHMARK(DataBlockSeek)->Iterations(1000000);

static void IteratorSeek(benchmark::State& state) {
  auto compaction_style = static_cast<CompactionStyle>(state.range(0));
  uint64_t max_data = state.range(1);
  uint64_t per_key_size = state.range(2);
  bool enable_statistics = state.range(3);
  bool negative_query = state.range(4);
  bool enable_filter = state.range(5);
  uint64_t key_num = max_data / per_key_size;

  // setup DB
  static DB* db;
  Options options;
  if (enable_statistics) {
    options.statistics = CreateDBStatistics();
  }
  options.compaction_style = compaction_style;

  if (enable_filter) {
    BlockBasedTableOptions table_options;
    table_options.filter_policy.reset(NewBloomFilterPolicy(10, false));
    options.table_factory.reset(NewBlockBasedTableFactory(table_options));
  }

  auto rnd = Random(301 + state.thread_index);
  KeyGenerator kg(&rnd, key_num);

  if (state.thread_index == 0) {
    SetupDB(state, options, &db, "IteratorSeek");

    // load db
    auto wo = WriteOptions();
    wo.disableWAL = true;
    for (uint64_t i = 0; i < key_num; i++) {
      Status s = db->Put(wo, kg.Next(),
                         rnd.RandomString(static_cast<int>(per_key_size)));
      if (!s.ok()) {
        state.SkipWithError(s.ToString().c_str());
      }
    }

    FlushOptions fo;
    Status s = db->Flush(fo);
    if (!s.ok()) {
      state.SkipWithError(s.ToString().c_str());
    }

    auto db_full = static_cast_with_check<DBImpl>(db);
    s = db_full->WaitForCompact(true);
    if (!s.ok()) {
      state.SkipWithError(s.ToString().c_str());
      return;
    }
  }

  {
    std::unique_ptr<Iterator> iter{nullptr};
    for (auto _ : state) {
      state.PauseTiming();
      if (!iter) {
        iter.reset(db->NewIterator(ReadOptions()));
      }
      Slice key = negative_query ? kg.NextNonExist() : kg.Next();
      if (!iter->status().ok()) {
        state.SkipWithError(iter->status().ToString().c_str());
        return;
      }
      state.ResumeTiming();
      iter->Seek(key);
    }
  }

  if (state.thread_index == 0) {
    TeardownDB(state, db, options, kg);
  }
}

static void IteratorSeekArguments(benchmark::internal::Benchmark* b) {
  for (int comp_style : {kCompactionStyleLevel, kCompactionStyleUniversal,
                         kCompactionStyleFIFO}) {
    for (int64_t max_data : {128l << 20, 512l << 20}) {
      for (int64_t per_key_size : {256, 1024}) {
        for (bool enable_statistics : {false, true}) {
          for (bool negative_query : {false, true}) {
            for (bool enable_filter : {false, true}) {
              b->Args({comp_style, max_data, per_key_size, enable_statistics,
                       negative_query, enable_filter});
            }
          }
        }
      }
    }
  }
  b->ArgNames({"comp_style", "max_data", "per_key_size", "enable_statistics",
               "negative_query", "enable_filter"});
}

static constexpr uint64_t kDBSeekNum = 10l << 10;
BENCHMARK(IteratorSeek)
    ->Threads(1)
    ->Iterations(kDBSeekNum)
    ->Apply(IteratorSeekArguments);
BENCHMARK(IteratorSeek)
    ->Threads(8)
    ->Iterations(kDBSeekNum / 8)
    ->Apply(IteratorSeekArguments);

static void IteratorNext(benchmark::State& state) {
  auto compaction_style = static_cast<CompactionStyle>(state.range(0));
  uint64_t max_data = state.range(1);
  uint64_t per_key_size = state.range(2);
  uint64_t key_num = max_data / per_key_size;

  // setup DB
  static DB* db;
  Options options;
  options.compaction_style = compaction_style;

  auto rnd = Random(301 + state.thread_index);
  KeyGenerator kg(&rnd, key_num);

  if (state.thread_index == 0) {
    SetupDB(state, options, &db, "IteratorNext");
    // load db
    auto wo = WriteOptions();
    wo.disableWAL = true;
    for (uint64_t i = 0; i < key_num; i++) {
      Status s = db->Put(wo, kg.Next(),
                         rnd.RandomString(static_cast<int>(per_key_size)));
      if (!s.ok()) {
        state.SkipWithError(s.ToString().c_str());
      }
    }

    FlushOptions fo;
    Status s = db->Flush(fo);
    if (!s.ok()) {
      state.SkipWithError(s.ToString().c_str());
    }

    auto db_full = static_cast_with_check<DBImpl>(db);
    s = db_full->WaitForCompact(true);
    if (!s.ok()) {
      state.SkipWithError(s.ToString().c_str());
      return;
    }
  }

  {
    std::unique_ptr<Iterator> iter{nullptr};
    for (auto _ : state) {
      state.PauseTiming();
      if (!iter) {
        iter.reset(db->NewIterator(ReadOptions()));
      }
      while (!iter->Valid()) {
        iter->Seek(kg.Next());
        if (!iter->status().ok()) {
          state.SkipWithError(iter->status().ToString().c_str());
        }
      }
      state.ResumeTiming();
      iter->Next();
    }
  }

  if (state.thread_index == 0) {
    TeardownDB(state, db, options, kg);
  }
}

static void IteratorNextArguments(benchmark::internal::Benchmark* b) {
  for (int comp_style : {kCompactionStyleLevel, kCompactionStyleUniversal,
                         kCompactionStyleFIFO}) {
    for (int64_t max_data : {128l << 20, 512l << 20}) {
      for (int64_t per_key_size : {256, 1024}) {
        b->Args({comp_style, max_data, per_key_size});
      }
    }
  }
  b->ArgNames({"comp_style", "max_data", "per_key_size"});
}
static constexpr uint64_t kIteratorNextNum = 10l << 10;
BENCHMARK(IteratorNext)
    ->Iterations(kIteratorNextNum)
    ->Apply(IteratorNextArguments);

static void IteratorNextWithPerfContext(benchmark::State& state) {
  // setup DB
  static DB* db;
  Options options;

  auto rnd = Random(301 + state.thread_index);
  KeyGenerator kg(&rnd, 1024);

  if (state.thread_index == 0) {
    SetupDB(state, options, &db, "IteratorNextWithPerfContext");
    // load db
    auto wo = WriteOptions();
    wo.disableWAL = true;
    for (uint64_t i = 0; i < 1024; i++) {
      Status s = db->Put(wo, kg.Next(), rnd.RandomString(1024));
      if (!s.ok()) {
        state.SkipWithError(s.ToString().c_str());
      }
    }
    auto db_full = static_cast_with_check<DBImpl>(db);
    Status s = db_full->WaitForCompact(true);
    if (!s.ok()) {
      state.SkipWithError(s.ToString().c_str());
      return;
    }
    FlushOptions fo;
    s = db->Flush(fo);
    if (!s.ok()) {
      state.SkipWithError(s.ToString().c_str());
    }
  }

  uint64_t user_key_comparison_count = 0;
  uint64_t internal_key_skipped_count = 0;
  uint64_t find_next_user_entry_time = 0;
  uint64_t iter_next_cpu_nanos = 0;

  SetPerfLevel(kEnableTime);
  get_perf_context()->EnablePerLevelPerfContext();

  {
    std::unique_ptr<Iterator> iter{nullptr};
    for (auto _ : state) {
      state.PauseTiming();
      if (!iter) {
        iter.reset(db->NewIterator(ReadOptions()));
      }
      while (!iter->Valid()) {
        iter->Seek(kg.Next());
        if (!iter->status().ok()) {
          state.SkipWithError(iter->status().ToString().c_str());
        }
      }
      get_perf_context()->Reset();
      state.ResumeTiming();

      iter->Next();
      user_key_comparison_count +=
          get_perf_context()->user_key_comparison_count;
      internal_key_skipped_count +=
          get_perf_context()->internal_key_skipped_count;
      find_next_user_entry_time +=
          get_perf_context()->find_next_user_entry_time;
      iter_next_cpu_nanos += get_perf_context()->iter_next_cpu_nanos;
    }
  }

  state.counters["user_key_comparison_count"] =
      benchmark::Counter(static_cast<double>(user_key_comparison_count),
                         benchmark::Counter::kAvgIterations);
  state.counters["internal_key_skipped_count"] =
      benchmark::Counter(static_cast<double>(internal_key_skipped_count),
                         benchmark::Counter::kAvgIterations);
  state.counters["find_next_user_entry_time"] =
      benchmark::Counter(static_cast<double>(find_next_user_entry_time),
                         benchmark::Counter::kAvgIterations);
  state.counters["iter_next_cpu_nanos"] =
      benchmark::Counter(static_cast<double>(iter_next_cpu_nanos),
                         benchmark::Counter::kAvgIterations);

  if (state.thread_index == 0) {
    TeardownDB(state, db, options, kg);
  }
}

BENCHMARK(IteratorNextWithPerfContext)->Iterations(100000);

static void IteratorPrev(benchmark::State& state) {
  auto compaction_style = static_cast<CompactionStyle>(state.range(0));
  uint64_t max_data = state.range(1);
  uint64_t per_key_size = state.range(2);
  uint64_t key_num = max_data / per_key_size;

  // setup DB
  static DB* db;
  std::string db_name;
  Options options;
  options.compaction_style = compaction_style;

  auto rnd = Random(301 + state.thread_index);
  KeyGenerator kg(&rnd, key_num);

  if (state.thread_index == 0) {
    SetupDB(state, options, &db, "IteratorPrev");
    // load db
    auto wo = WriteOptions();
    wo.disableWAL = true;
    for (uint64_t i = 0; i < key_num; i++) {
      Status s = db->Put(wo, kg.Next(),
                         rnd.RandomString(static_cast<int>(per_key_size)));
      if (!s.ok()) {
        state.SkipWithError(s.ToString().c_str());
      }
    }

    FlushOptions fo;
    Status s = db->Flush(fo);
    if (!s.ok()) {
      state.SkipWithError(s.ToString().c_str());
    }

    auto db_full = static_cast_with_check<DBImpl>(db);
    s = db_full->WaitForCompact(true);
    if (!s.ok()) {
      state.SkipWithError(s.ToString().c_str());
      return;
    }
  }

  {
    std::unique_ptr<Iterator> iter{nullptr};
    for (auto _ : state) {
      state.PauseTiming();
      if (!iter) {
        iter.reset(db->NewIterator(ReadOptions()));
      }
      while (!iter->Valid()) {
        iter->Seek(kg.Next());
        if (!iter->status().ok()) {
          state.SkipWithError(iter->status().ToString().c_str());
        }
      }
      state.ResumeTiming();
      iter->Prev();
    }
  }

  if (state.thread_index == 0) {
    TeardownDB(state, db, options, kg);
  }
}

static void IteratorPrevArguments(benchmark::internal::Benchmark* b) {
  for (int comp_style : {kCompactionStyleLevel, kCompactionStyleUniversal,
                         kCompactionStyleFIFO}) {
    for (int64_t max_data : {128l << 20, 512l << 20}) {
      for (int64_t per_key_size : {256, 1024}) {
        b->Args({comp_style, max_data, per_key_size});
      }
    }
  }
  b->ArgNames({"comp_style", "max_data", "per_key_size"});
}

static constexpr uint64_t kIteratorPrevNum = 10l << 10;
BENCHMARK(IteratorPrev)
    ->Iterations(kIteratorPrevNum)
    ->Apply(IteratorPrevArguments);

static void PrefixSeek(benchmark::State& state) {
  auto compaction_style = static_cast<CompactionStyle>(state.range(0));
  uint64_t max_data = state.range(1);
  uint64_t per_key_size = state.range(2);
  bool enable_statistics = state.range(3);
  bool enable_filter = state.range(4);
  uint64_t key_num = max_data / per_key_size;

  // setup DB
  static DB* db;
  Options options;
  if (enable_statistics) {
    options.statistics = CreateDBStatistics();
  }
  options.compaction_style = compaction_style;
  options.prefix_extractor.reset(NewFixedPrefixTransform(4));

  if (enable_filter) {
    BlockBasedTableOptions table_options;
    table_options.filter_policy.reset(NewBloomFilterPolicy(10, false));
    options.table_factory.reset(NewBlockBasedTableFactory(table_options));
  }

  auto rnd = Random(301 + state.thread_index);
  KeyGenerator kg(&rnd, key_num, key_num / 100);

  if (state.thread_index == 0) {
    SetupDB(state, options, &db, "PrefixSeek");

    // load db
    auto wo = WriteOptions();
    wo.disableWAL = true;
    for (uint64_t i = 0; i < key_num; i++) {
      Status s = db->Put(wo, kg.Next(),
                         rnd.RandomString(static_cast<int>(per_key_size)));
      if (!s.ok()) {
        state.SkipWithError(s.ToString().c_str());
      }
    }

    FlushOptions fo;
    Status s = db->Flush(fo);
    if (!s.ok()) {
      state.SkipWithError(s.ToString().c_str());
    }

    auto db_full = static_cast_with_check<DBImpl>(db);
    s = db_full->WaitForCompact(true);
    if (!s.ok()) {
      state.SkipWithError(s.ToString().c_str());
      return;
    }
  }

  {
    std::unique_ptr<Iterator> iter{nullptr};
    for (auto _ : state) {
      state.PauseTiming();
      if (!iter) {
        iter.reset(db->NewIterator(ReadOptions()));
      }
      state.ResumeTiming();
      iter->Seek(kg.NextPrefix());
      if (!iter->status().ok()) {
        state.SkipWithError(iter->status().ToString().c_str());
        return;
      }
    }
  }

  if (state.thread_index == 0) {
    TeardownDB(state, db, options, kg);
  }
}

static void PrefixSeekArguments(benchmark::internal::Benchmark* b) {
  for (int comp_style : {kCompactionStyleLevel, kCompactionStyleUniversal,
                         kCompactionStyleFIFO}) {
    for (int64_t max_data : {128l << 20, 512l << 20}) {
      for (int64_t per_key_size : {256, 1024}) {
        for (bool enable_statistics : {false, true}) {
          for (bool enable_filter : {false, true}) {
            b->Args({comp_style, max_data, per_key_size, enable_statistics,
                     enable_filter});
          }
        }
      }
    }
  }
  b->ArgNames({"comp_style", "max_data", "per_key_size", "enable_statistics",
               "enable_filter"});
}

static constexpr uint64_t kPrefixSeekNum = 10l << 10;
BENCHMARK(PrefixSeek)->Iterations(kPrefixSeekNum)->Apply(PrefixSeekArguments);
BENCHMARK(PrefixSeek)
    ->Threads(8)
    ->Iterations(kPrefixSeekNum / 8)
    ->Apply(PrefixSeekArguments);

}  // namespace ROCKSDB_NAMESPACE

BENCHMARK_MAIN();
