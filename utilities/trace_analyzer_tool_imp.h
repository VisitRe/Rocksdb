//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#pragma once
#ifndef ROCKSDB_LITE

#include "rocksdb/trace_analyzer_tool.h"

#include <stdio.h>
#include <fstream>
#include <list>
#include <map>
#include <queue>
#include <set>
#include <utility>
#include <vector>

#include "rocksdb/env.h"
#include "rocksdb/write_batch.h"
#include "util/trace_replay.h"

namespace rocksdb {

class DBImpl;
class WriteBatch;
class AnalyzerOptions;
class TraceWriteHandler;

bool ReadOneLine(std::istringstream* iss, SequentialFile* seq_file,
                     std::string* output, bool* has_data, Status* result);

enum TraceOperationType : int {
  kGet = 0,
  kPut = 1,
  kDelete = 2,
  kSingleDelete = 3,
  kRangeDelete = 4,
  kMerge = 5,
  kIter = 6,
  kTaTypeNum = 7
};

struct TraceUnit {
  uint64_t ts;
  uint32_t type;
  uint32_t cf_id;
  size_t value_size;
  std::string key;
};

struct TypeCorre {
  uint64_t count;
  uint64_t total_ts;
};

struct StatsUnit {
  uint64_t key_id;
  uint64_t access_count;
  uint64_t latest_ts;
  uint64_t succ_count;  // current only used to count Get if key found
  uint32_t cf_id;
  size_t value_size;
  std::vector<TypeCorre> v_corre;
};

class AnalyzerOptions {
 public:
  std::vector<std::vector<int>> corre_map;
  std::vector<std::pair<int, int>> corre_list;

  AnalyzerOptions();

  ~AnalyzerOptions();

  void SparseCorreInput(const std::string& in_str);
};

struct TraceStats {
  uint32_t cf_id;
  std::string cf_name;
  uint64_t a_count;
  uint64_t a_succ_count;
  uint64_t akey_id;
  uint64_t a_key_size_sqsum;
  uint64_t a_key_size_sum;
  uint64_t a_key_mid;
  uint64_t a_value_size_sqsum;
  uint64_t a_value_size_sum;
  uint64_t a_value_mid;
  uint32_t a_peak_qps;
  double a_ave_qps;
  std::map<std::string, StatsUnit> a_key_stats;
  std::map<uint64_t, uint64_t> a_count_stats;
  std::map<uint64_t, uint64_t> a_key_size_stats;
  std::map<uint64_t, uint64_t> a_value_size_stats;
  std::map<uint32_t, uint32_t> a_qps_stats;
  std::map<uint32_t, std::map<std::string, uint32_t>> a_qps_prefix_stats;
  std::priority_queue<std::pair<uint64_t, std::string>,
                      std::vector<std::pair<uint64_t, std::string>>,
                      std::greater<std::pair<uint64_t, std::string>>>
      top_k_queue;
  std::priority_queue<std::pair<uint64_t, std::string>,
                      std::vector<std::pair<uint64_t, std::string>>,
                      std::greater<std::pair<uint64_t, std::string>>>
      top_k_prefix_access;
  std::priority_queue<std::pair<double, std::string>,
                      std::vector<std::pair<double, std::string>>,
                      std::greater<std::pair<double, std::string>>>
      top_k_prefix_ave;
  std::priority_queue<std::pair<uint32_t, uint32_t>,
                      std::vector<std::pair<uint32_t, uint32_t>>,
                      std::greater<std::pair<uint32_t, uint32_t>>>
      top_k_qps_sec;
  std::list<TraceUnit> time_series;
  std::vector<std::pair<uint64_t, uint64_t>> corre_output;

  std::unique_ptr<rocksdb::WritableFile> time_series_f;
  std::unique_ptr<rocksdb::WritableFile> a_key_f;
  std::unique_ptr<rocksdb::WritableFile> a_count_dist_f;
  std::unique_ptr<rocksdb::WritableFile> a_prefix_cut_f;
  std::unique_ptr<rocksdb::WritableFile> a_value_size_f;
  std::unique_ptr<rocksdb::WritableFile> a_qps_f;
  std::unique_ptr<rocksdb::WritableFile> a_top_qps_prefix_f;
  std::unique_ptr<rocksdb::WritableFile> w_key_f;
  std::unique_ptr<rocksdb::WritableFile> w_prefix_cut_f;

  TraceStats();
  ~TraceStats();
};

struct TypeUnit {
  std::string type_name;
  bool enabled;
  uint64_t total_keys;
  uint64_t total_access;
  uint64_t total_succ_access;
  std::map<uint32_t, TraceStats> stats;
};

struct CfUnit {
  uint32_t cf_id;
  uint64_t w_count;  // total keys in this cf if we use the whole key space
  uint64_t a_count;  // the total keys in this cf that are accessed
  std::map<uint64_t, uint64_t> w_key_size_stats;  // whole key space key size
                                                  // statistic this cf
};

class TraceAnalyzer {
 public:
  TraceAnalyzer(std::string &trace_path, std::string &output_path,
                AnalyzerOptions _analyzer_opts);
  ~TraceAnalyzer();

  Status PrepareProcessing();

  Status StartProcessing();

  Status MakeStatistics();

  Status ReProcessing();

  Status EndProcessing();

  Status WriteTraceUnit(TraceUnit &unit);

  // The trace  processing functions for different type
  Status HandleGetCF(uint32_t column_family_id, const std::string& key,
                     const uint64_t& ts, const uint32_t& get_ret);
  Status HandlePutCF(uint32_t column_family_id, const Slice& key,
                     const Slice& value);
  Status HandleDeleteCF(uint32_t column_family_id, const Slice& key);
  Status HandleSingleDeleteCF(uint32_t column_family_id, const Slice& key);
  Status HandleDeleteRangeCF(uint32_t column_family_id, const Slice& begin_key,
                             const Slice& end_key);
  Status HandleMergeCF(uint32_t column_family_id, const Slice& key,
                       const Slice& value);
  Status HandleIterCF(uint32_t column_family_id, const std::string& key,
                      const uint64_t& ts);
  std::vector<TypeUnit>& GetTaVector() { return ta_; }

 private:
  rocksdb::Env* env_;
  EnvOptions env_options_;
  unique_ptr<rocksdb::TraceReader> trace_reader_;
  size_t offset_;
  char *buffer_;
  uint64_t c_time_;
  std::string trace_name_;
  std::string output_path_;
  AnalyzerOptions analyzer_opts_;
  uint64_t total_requests_;
  uint64_t total_access_keys_;
  uint64_t total_gets_;
  uint64_t total_writes_;
  uint64_t begin_time_;
  uint64_t end_time_;
  std::unique_ptr<rocksdb::WritableFile> trace_sequence_f_;  // readable trace
  std::unique_ptr<rocksdb::WritableFile> qps_f_;             // overall qps
  std::unique_ptr<rocksdb::SequentialFile> wkey_input_f_;
  std::vector<TypeUnit> ta_;  // The main statistic collecting data structure
  std::map<uint32_t, CfUnit> cfs_;  // All the cf_id appears in this trace;
  std::vector<uint32_t> qps_peak_;
  std::vector<double> qps_ave_;

  Status KeyStatsInsertion(const uint32_t& type, const uint32_t& cf_id,
                           const std::string& key, const size_t value_size,
                           const uint64_t ts);
  Status StatsUnitCorreUpdate(StatsUnit& unit, const uint32_t& type,
                              const uint64_t& ts, const std::string& key);
  Status OpenStatsOutputFiles(const std::string& type, TraceStats& new_stats);
  Status CreateOutputFile(const std::string& type, const std::string& cf_name,
                          const std::string& ending,
                          std::unique_ptr<rocksdb::WritableFile>* f_ptr);
  void CloseOutputFiles();

  void PrintGetStatistics();
  Status TraceUnitWriter(std::unique_ptr<rocksdb::WritableFile>& f_ptr,
                         TraceUnit& unit);
  std::string MicrosdToDate(uint64_t time);

  Status WriteTraceSequence(const uint32_t& type, const uint32_t& cf_id,
                            const std::string& key, const size_t value_size,
                            const uint64_t ts);
  Status MakeStatisticKeyStatsOrPrefix(TraceStats& stats);
  Status MakeStatisticCorrelation(TraceStats& stats, StatsUnit& unit);
  Status MakeStatisticQPS();
};

// write bach handler to be used for WriteBache iterator
// when processing the write trace
class TraceWriteHandler : public WriteBatch::Handler {
 private:
  TraceAnalyzer* ta_ptr;
  std::string tmp_use;

 public:
  TraceWriteHandler() { ta_ptr = nullptr; }
  TraceWriteHandler(TraceAnalyzer* _ta_ptr) { ta_ptr = _ta_ptr; }
  ~TraceWriteHandler() {}

  virtual Status PutCF(uint32_t column_family_id, const Slice& key,
                       const Slice& value) override {
    return ta_ptr->HandlePutCF(column_family_id, key, value);
  }
  virtual Status DeleteCF(uint32_t column_family_id,
                          const Slice& key) override {
    return ta_ptr->HandleDeleteCF(column_family_id, key);
  }
  virtual Status SingleDeleteCF(uint32_t column_family_id,
                                const Slice& key) override {
    return ta_ptr->HandleSingleDeleteCF(column_family_id, key);
  }
  virtual Status DeleteRangeCF(uint32_t column_family_id,
                               const Slice& begin_key,
                               const Slice& end_key) override {
    return ta_ptr->HandleDeleteRangeCF(column_family_id, begin_key, end_key);
  }
  virtual Status MergeCF(uint32_t column_family_id, const Slice& key,
                         const Slice& value) override {
    return ta_ptr->HandleMergeCF(column_family_id, key, value);
  }
};

}  // namespace rocksdb

#endif  // ROCKSDB_LITE
