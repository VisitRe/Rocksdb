//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2012 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef ROCKSDB_LITE

#include <stdint.h>
#include <unistd.h>
#include <sstream>

#include "db/db_test_util.h"
#include "util/trace_replay.h"
#include "util/testharness.h"
#include "util/testutil.h"
#include "utilities/trace_analyzer_tool_imp.h"
#include "rocksdb/db.h"
#include "rocksdb/env.h"
#include "rocksdb/status.h"



namespace rocksdb {
namespace {
  static const int kMaxArgCount = 100;
  static const size_t kArgBufferSize = 100000;
}  // namespace


// The helper functions for the test
class TraceAnalyzerTest : public testing::Test {
 public:
  TraceAnalyzerTest() : rnd_(0xFB) {
    test_path_ = test::TmpDir() + "trace_analyzer_test";
    env_ = rocksdb::Env::Default();
    env_->CreateDir(test_path_);
    dbname_ = test_path_ + "/db";
  }

  ~TraceAnalyzerTest() {

  }

  void GenerateTrace(std::string trace_path) {
    Options options;
    options.create_if_missing = true;
    options.IncreaseParallelism();
    options.OptimizeLevelStyleCompaction();
    options.merge_operator = MergeOperators::CreatePutOperator();
    ReadOptions ro;
    WriteOptions wo;
    TraceOptions trace_opt;
    DB* db_ = nullptr;
    std::string value;

    ASSERT_OK(DB::Open(options, dbname_, &db_));
    ASSERT_OK(db_->StartTrace(trace_opt, trace_path));

    WriteBatch batch;
    ASSERT_OK(batch.Put("a", "aaaaaaaaa"));
    ASSERT_OK(batch.Merge("b", "aaaaaaaaaaaaaaaaaaaa"));
    ASSERT_OK(batch.Delete("c"));
    ASSERT_OK(batch.SingleDelete("d"));
    ASSERT_OK(batch.DeleteRange("e", "f"));
    ASSERT_OK(db_->Write(wo, &batch));

    ASSERT_OK(db_->Get(ro, "a", &value));
    sleep(1);
    db_->Get(ro, "g", &value);

    ASSERT_OK(db_->EndTrace(trace_opt));

    ASSERT_OK(env_->FileExists(trace_path));

    std::unique_ptr<WritableFile> whole_f;
    std::string whole_path = test_path_ + "/0.txt";
    ASSERT_OK(env_->NewWritableFile(whole_path, &whole_f, env_options_));
    std::ostringstream whole;
    whole << "0x61\n"
          << "0x62\n"
          << "0x63\n"
          << "0x64\n"
          << "0x65\n"
          << "0x66\n";
    std::string whole_str(whole.str());
    ASSERT_OK(whole_f->Append(whole_str));
  }

  void RunTraceAnalyzer(const std::vector<std::string>& args) {
    char arg_buffer[kArgBufferSize];
    char* argv[kMaxArgCount];
    int argc = 0;
    int cursor = 0;

    for (const auto& arg : args) {
      ASSERT_LE(cursor + arg.size() + 1, kArgBufferSize);
      ASSERT_LE(argc + 1, kMaxArgCount);
      snprintf(arg_buffer + cursor, arg.size() + 1, "%s", arg.c_str());

      argv[argc++] = arg_buffer + cursor;
      cursor += arg.size() + 1;
    }

    rocksdb::TraceAnalyzerTool tool;
    ASSERT_EQ(0, tool.Run(argc, argv));
  }

  void CheckFileContent(const std::vector<std::string>& cnt,
                        std::string file_path, bool full_content) {
    ASSERT_OK(env_->FileExists(file_path));
    std::unique_ptr<SequentialFile> f_ptr;
    ASSERT_OK(env_->NewSequentialFile(file_path, &f_ptr, env_options_));

    std::string get_line;
    std::istringstream iss;
    bool has_data = true;
    std::vector<std::string> result;
    uint32_t count;
    Status s;
    for (count = 0; ReadOneLine(&iss, f_ptr.get(), &get_line, &has_data, &s);
         ++count) {
      ASSERT_OK(s);
      result.push_back(get_line);
    }

    ASSERT_EQ(cnt.size(), result.size());
    for (int i = 0; i < static_cast<int>(result.size()); i++) {
      if (full_content) {
        ASSERT_EQ(result[i], cnt[i]);
      } else {
        ASSERT_EQ(result[i][0], cnt[i][0]);
      }
    }

    return;
  }


  rocksdb::Env* env_;
  EnvOptions env_options_;
  std::string test_path_;
  std::string dbname_;
  Random rnd_;

};


TEST_F(TraceAnalyzerTest, Get) {
  std::string trace_path = test_path_ + "/trace";
  std::string output_path = test_path_ + "/get";
  std::string file_path;
  std::vector<std::string> paras = {"./trace_analyzer",
                                    "-use_get",
                                    "-output_trace_sequence",
                                    "-output_key_stats",
                                    "-output_access_count_stats",
                                    "-output_prefix=test",
                                    "-output_prefix_cut=1",
                                    "-output_time_series=10",
                                    "-output_value_distribution",
                                    "-output_qps_stats",
                                    "-no_print"};
  Status s = env_->FileExists(trace_path);
  if (!s.ok()) {
    GenerateTrace(trace_path);
  }
  paras.push_back("-output_dir=" + output_path);
  paras.push_back("-trace_file=" + trace_path);
  paras.push_back("-key_space_dir=" + test_path_);

  env_->CreateDir(output_path);
  RunTraceAnalyzer(paras);

  //check the key_stats file
  std::vector<std::string> k_stats = {"0 10 0 1 1.000000", "0 0 1 1 0.000000"};
  file_path = output_path + "/test-get-0-accessed_key_stats.txt";
  CheckFileContent(k_stats, file_path, true);

  // Check the access count distribution
  std::vector<std::string> k_dist = {"access_count: 1 num: 2"};
  file_path = output_path + "/test-get-0-accessed_key_count_distribution.txt";
  CheckFileContent(k_dist, file_path, true);

  // Check the trace sequence
  std::vector<std::string> k_sequence = {"1", "5", "2", "3", "4", "0", "0"};
  file_path = output_path + "/test-trace_sequence.txt";
  CheckFileContent(k_sequence, file_path, false);

  // Check the prefix
  std::vector<std::string> k_prefix = {"0 0 0 0.000000 -nan 0x30",
                                       "1 1 1 1.000000 1.000000 0x61"};
  file_path = output_path + "/test-get-0-accessed_key_prefix_cut.txt";
  CheckFileContent(k_prefix, file_path, true);

  // Check the time series
  std::vector<std::string> k_series = {"0 1533000630 0", "0 1533000630 1"};
  file_path = output_path + "/test-get-0-time_series.txt";
  CheckFileContent(k_series, file_path, false);

  // Check the accessed key in whole key space
  std::vector<std::string> k_whole_access = {"0 1"};
  file_path = output_path + "/test-get-0-whole_key_stats.txt";
  CheckFileContent(k_whole_access, file_path, true);

  // Check the whole key prefix cut
  std::vector<std::string> k_whole_prefix = {"0 0x61", "1 0x62", "2 0x63",
                                             "3 0x64", "4 0x65", "5 0x66"};
  file_path = output_path + "/test-get-0-whole_key_prefix_cut.txt";
  CheckFileContent(k_whole_prefix, file_path, true);

  // Check the overall qps
  std::vector<std::string> all_qps = {"1 0 0 0 0 0 0 1"};
  file_path = output_path + "/test-qps_stats.txt";
  CheckFileContent(all_qps, file_path, true);

  // Check the qps of get
  std::vector<std::string> get_qps = {"1"};
  file_path = output_path + "/test-get-0-qps_stats.txt";
  CheckFileContent(get_qps, file_path, true);

  // Check the top k qps prefix cut
  std::vector<std::string> top_qps = {"At time: 0 with QPS: 1",
                                      "The prefix: 0x61 Access count: 1"};
  file_path = output_path + "/test-get-0-accessed_top_k_qps_prefix_cut.txt";
  CheckFileContent(top_qps, file_path, true);
}

// Test analyzing of Put
TEST_F(TraceAnalyzerTest, Put) {
  std::string trace_path = test_path_ + "/trace";
  std::string output_path = test_path_ + "/put";
  std::string file_path;
  std::vector<std::string> paras = {"./trace_analyzer",
                                    "-use_get",
                                    "-use_put",
                                    "-output_trace_sequence",
                                    "-output_key_stats",
                                    "-output_access_count_stats",
                                    "-output_prefix=test",
                                    "-output_prefix_cut=1",
                                    "-output_time_series=10",
                                    "-output_value_distribution",
                                    "-output_qps_stats",
                                    "-no_print"};
  Status s = env_->FileExists(trace_path);
  if (!s.ok()) {
    GenerateTrace(trace_path);
  }
  paras.push_back("-output_dir=" + output_path);
  paras.push_back("-trace_file=" + trace_path);
  paras.push_back("-key_space_dir=" + test_path_);

  env_->CreateDir(output_path);
  RunTraceAnalyzer(paras);

  // check the key_stats file
  std::vector<std::string> k_stats = {"0 9 0 1 1.000000"};
  file_path = output_path + "/test-put-0-accessed_key_stats.txt";
  CheckFileContent(k_stats, file_path, true);

  // Check the access count distribution
  std::vector<std::string> k_dist = {"access_count: 1 num: 1"};
  file_path = output_path + "/test-put-0-accessed_key_count_distribution.txt";
  CheckFileContent(k_dist, file_path, true);

  // Check the trace sequence
  std::vector<std::string> k_sequence = {"1", "5", "2", "3", "4", "0", "0"};
  file_path = output_path + "/test-trace_sequence.txt";
  CheckFileContent(k_sequence, file_path, false);

  // Check the prefix
  std::vector<std::string> k_prefix = {"0 0 0 0.000000 -nan 0x30"};
  file_path = output_path + "/test-put-0-accessed_key_prefix_cut.txt";
  CheckFileContent(k_prefix, file_path, true);

  // Check the time series
  std::vector<std::string> k_series = {"1 1533056278 0"};
  file_path = output_path + "/test-put-0-time_series.txt";
  CheckFileContent(k_series, file_path, false);

  // Check the accessed key in whole key space
  std::vector<std::string> k_whole_access = {"0 1"};
  file_path = output_path + "/test-put-0-whole_key_stats.txt";
  CheckFileContent(k_whole_access, file_path, true);

  // Check the whole key prefix cut
  std::vector<std::string> k_whole_prefix = {"0 0x61", "1 0x62", "2 0x63",
                                             "3 0x64", "4 0x65", "5 0x66"};
  file_path = output_path + "/test-put-0-whole_key_prefix_cut.txt";
  CheckFileContent(k_whole_prefix, file_path, true);

  // Check the overall qps
  std::vector<std::string> all_qps = {"1 1 0 0 0 0 0 2"};
  file_path = output_path + "/test-qps_stats.txt";
  CheckFileContent(all_qps, file_path, true);

  // Check the qps of get
  std::vector<std::string> get_qps = {"1"};
  file_path = output_path + "/test-put-0-qps_stats.txt";
  CheckFileContent(get_qps, file_path, true);

  // Check the top k qps prefix cut
  std::vector<std::string> top_qps = {"At time: 0 with QPS: 1",
                                      "The prefix: 0x61 Access count: 1"};
  file_path = output_path + "/test-put-0-accessed_top_k_qps_prefix_cut.txt";
  CheckFileContent(top_qps, file_path, true);

  // Check the value size distribution
  std::vector<std::string> value_dist = {
      "Number_of_value_size_between 0 and 16 is: 1"};
  file_path = output_path + "/test-put-0-accessed_value_size_distribution.txt";
  CheckFileContent(value_dist, file_path, true);
}

// Test analyzing of delete
TEST_F(TraceAnalyzerTest, Delete) {
  std::string trace_path = test_path_ + "/trace";
  std::string output_path = test_path_ + "/delete";
  std::string file_path;
  std::vector<std::string> paras = {"./trace_analyzer",
                                    "-use_get",
                                    "-use_put",
                                    "-use_delete",
                                    "-output_trace_sequence",
                                    "-output_key_stats",
                                    "-output_access_count_stats",
                                    "-output_prefix=test",
                                    "-output_prefix_cut=1",
                                    "-output_time_series=10",
                                    "-output_value_distribution",
                                    "-output_qps_stats",
                                    "-no_print"};
  Status s = env_->FileExists(trace_path);
  if (!s.ok()) {
    GenerateTrace(trace_path);
  }
  paras.push_back("-output_dir=" + output_path);
  paras.push_back("-trace_file=" + trace_path);
  paras.push_back("-key_space_dir=" + test_path_);

  env_->CreateDir(output_path);
  RunTraceAnalyzer(paras);

  // check the key_stats file
  std::vector<std::string> k_stats = {"0 0 0 1 1.000000"};
  file_path = output_path + "/test-delete-0-accessed_key_stats.txt";
  CheckFileContent(k_stats, file_path, true);

  // Check the access count distribution
  std::vector<std::string> k_dist = {"access_count: 1 num: 1"};
  file_path =
      output_path + "/test-delete-0-accessed_key_count_distribution.txt";
  CheckFileContent(k_dist, file_path, true);

  // Check the trace sequence
  std::vector<std::string> k_sequence = {"1", "5", "2", "3", "4", "0", "0"};
  file_path = output_path + "/test-trace_sequence.txt";
  CheckFileContent(k_sequence, file_path, false);

  // Check the prefix
  std::vector<std::string> k_prefix = {"0 0 0 0.000000 -nan 0x30"};
  file_path = output_path + "/test-delete-0-accessed_key_prefix_cut.txt";
  CheckFileContent(k_prefix, file_path, true);

  // Check the time series
  std::vector<std::string> k_series = {"2 1533000630 0"};
  file_path = output_path + "/test-delete-0-time_series.txt";
  CheckFileContent(k_series, file_path, false);

  // Check the accessed key in whole key space
  std::vector<std::string> k_whole_access = {"2 1"};
  file_path = output_path + "/test-delete-0-whole_key_stats.txt";
  CheckFileContent(k_whole_access, file_path, true);

  // Check the whole key prefix cut
  std::vector<std::string> k_whole_prefix = {"0 0x61", "1 0x62", "2 0x63",
                                             "3 0x64", "4 0x65", "5 0x66"};
  file_path = output_path + "/test-delete-0-whole_key_prefix_cut.txt";
  CheckFileContent(k_whole_prefix, file_path, true);

  // Check the overall qps
  std::vector<std::string> all_qps = {"1 1 1 0 0 0 0 3"};
  file_path = output_path + "/test-qps_stats.txt";
  CheckFileContent(all_qps, file_path, true);

  // Check the qps of get
  std::vector<std::string> get_qps = {"1"};
  file_path = output_path + "/test-delete-0-qps_stats.txt";
  CheckFileContent(get_qps, file_path, true);

  // Check the top k qps prefix cut
  std::vector<std::string> top_qps = {"At time: 0 with QPS: 1",
                                      "The prefix: 0x63 Access count: 1"};
  file_path = output_path + "/test-delete-0-accessed_top_k_qps_prefix_cut.txt";
  CheckFileContent(top_qps, file_path, true);
}

// Test analyzing of Merge
TEST_F(TraceAnalyzerTest, Merge) {
  std::string trace_path = test_path_ + "/trace";
  std::string output_path = test_path_ + "/merge";
  std::string file_path;
  std::vector<std::string> paras = {"./trace_analyzer",
                                    "-use_get",
                                    "-use_put",
                                    "-use_delete",
                                    "-use_merge",
                                    "-output_trace_sequence",
                                    "-output_key_stats",
                                    "-output_access_count_stats",
                                    "-output_prefix=test",
                                    "-output_prefix_cut=1",
                                    "-output_time_series=10",
                                    "-output_value_distribution",
                                    "-output_qps_stats",
                                    "-no_print"};
  Status s = env_->FileExists(trace_path);
  if (!s.ok()) {
    GenerateTrace(trace_path);
  }
  paras.push_back("-output_dir=" + output_path);
  paras.push_back("-trace_file=" + trace_path);
  paras.push_back("-key_space_dir=" + test_path_);

  env_->CreateDir(output_path);
  RunTraceAnalyzer(paras);

  // check the key_stats file
  std::vector<std::string> k_stats = {"0 20 0 1 1.000000"};
  file_path = output_path + "/test-merge-0-accessed_key_stats.txt";
  CheckFileContent(k_stats, file_path, true);

  // Check the access count distribution
  std::vector<std::string> k_dist = {"access_count: 1 num: 1"};
  file_path = output_path + "/test-merge-0-accessed_key_count_distribution.txt";
  CheckFileContent(k_dist, file_path, true);

  // Check the trace sequence
  std::vector<std::string> k_sequence = {"1", "5", "2", "3", "4", "0", "0"};
  file_path = output_path + "/test-trace_sequence.txt";
  CheckFileContent(k_sequence, file_path, false);

  // Check the prefix
  std::vector<std::string> k_prefix = {"0 0 0 0.000000 -nan 0x30"};
  file_path = output_path + "/test-merge-0-accessed_key_prefix_cut.txt";
  CheckFileContent(k_prefix, file_path, true);

  // Check the time series
  std::vector<std::string> k_series = {"5 1533000630 0"};
  file_path = output_path + "/test-merge-0-time_series.txt";
  CheckFileContent(k_series, file_path, false);

  // Check the accessed key in whole key space
  std::vector<std::string> k_whole_access = {"1 1"};
  file_path = output_path + "/test-merge-0-whole_key_stats.txt";
  CheckFileContent(k_whole_access, file_path, true);

  // Check the whole key prefix cut
  std::vector<std::string> k_whole_prefix = {"0 0x61", "1 0x62", "2 0x63",
                                             "3 0x64", "4 0x65", "5 0x66"};
  file_path = output_path + "/test-merge-0-whole_key_prefix_cut.txt";
  CheckFileContent(k_whole_prefix, file_path, true);

  // Check the overall qps
  std::vector<std::string> all_qps = {"1 1 1 0 0 1 0 4"};
  file_path = output_path + "/test-qps_stats.txt";
  CheckFileContent(all_qps, file_path, true);

  // Check the qps of get
  std::vector<std::string> get_qps = {"1"};
  file_path = output_path + "/test-merge-0-qps_stats.txt";
  CheckFileContent(get_qps, file_path, true);

  // Check the top k qps prefix cut
  std::vector<std::string> top_qps = {"At time: 0 with QPS: 1",
                                      "The prefix: 0x62 Access count: 1"};
  file_path = output_path + "/test-merge-0-accessed_top_k_qps_prefix_cut.txt";
  CheckFileContent(top_qps, file_path, true);

  // Check the value size distribution
  std::vector<std::string> value_dist = {
      "Number_of_value_size_between 0 and 24 is: 1"};
  file_path =
      output_path + "/test-merge-0-accessed_value_size_distribution.txt";
  CheckFileContent(value_dist, file_path, true);
}

// Test analyzing of SingleDelete
TEST_F(TraceAnalyzerTest, SingleDelete) {
  std::string trace_path = test_path_ + "/trace";
  std::string output_path = test_path_ + "/single_delete";
  std::string file_path;
  std::vector<std::string> paras = {"./trace_analyzer",
                                    "-use_get",
                                    "-use_put",
                                    "-use_delete",
                                    "-use_merge",
                                    "-use_single_delete",
                                    "-output_trace_sequence",
                                    "-output_key_stats",
                                    "-output_access_count_stats",
                                    "-output_prefix=test",
                                    "-output_prefix_cut=1",
                                    "-output_time_series=10",
                                    "-output_value_distribution",
                                    "-output_qps_stats",
                                    "-no_print"};
  Status s = env_->FileExists(trace_path);
  if (!s.ok()) {
    GenerateTrace(trace_path);
  }
  paras.push_back("-output_dir=" + output_path);
  paras.push_back("-trace_file=" + trace_path);
  paras.push_back("-key_space_dir=" + test_path_);

  env_->CreateDir(output_path);
  RunTraceAnalyzer(paras);

  // check the key_stats file
  std::vector<std::string> k_stats = {"0 0 0 1 1.000000"};
  file_path = output_path + "/test-single_delete-0-accessed_key_stats.txt";
  CheckFileContent(k_stats, file_path, true);

  // Check the access count distribution
  std::vector<std::string> k_dist = {"access_count: 1 num: 1"};
  file_path =
      output_path + "/test-single_delete-0-accessed_key_count_distribution.txt";
  CheckFileContent(k_dist, file_path, true);

  // Check the trace sequence
  std::vector<std::string> k_sequence = {"1", "5", "2", "3", "4", "0", "0"};
  file_path = output_path + "/test-trace_sequence.txt";
  CheckFileContent(k_sequence, file_path, false);

  // Check the prefix
  std::vector<std::string> k_prefix = {"0 0 0 0.000000 -nan 0x30"};
  file_path = output_path + "/test-single_delete-0-accessed_key_prefix_cut.txt";
  CheckFileContent(k_prefix, file_path, true);

  // Check the time series
  std::vector<std::string> k_series = {"3 1533000630 0"};
  file_path = output_path + "/test-single_delete-0-time_series.txt";
  CheckFileContent(k_series, file_path, false);

  // Check the accessed key in whole key space
  std::vector<std::string> k_whole_access = {"3 1"};
  file_path = output_path + "/test-single_delete-0-whole_key_stats.txt";
  CheckFileContent(k_whole_access, file_path, true);

  // Check the whole key prefix cut
  std::vector<std::string> k_whole_prefix = {"0 0x61", "1 0x62", "2 0x63",
                                             "3 0x64", "4 0x65", "5 0x66"};
  file_path = output_path + "/test-single_delete-0-whole_key_prefix_cut.txt";
  CheckFileContent(k_whole_prefix, file_path, true);

  // Check the overall qps
  std::vector<std::string> all_qps = {"1 1 1 1 0 1 0 5"};
  file_path = output_path + "/test-qps_stats.txt";
  CheckFileContent(all_qps, file_path, true);

  // Check the qps of get
  std::vector<std::string> get_qps = {"1"};
  file_path = output_path + "/test-single_delete-0-qps_stats.txt";
  CheckFileContent(get_qps, file_path, true);

  // Check the top k qps prefix cut
  std::vector<std::string> top_qps = {"At time: 0 with QPS: 1",
                                      "The prefix: 0x64 Access count: 1"};
  file_path =
      output_path + "/test-single_delete-0-accessed_top_k_qps_prefix_cut.txt";
  CheckFileContent(top_qps, file_path, true);
}

// Test analyzing of delete
TEST_F(TraceAnalyzerTest, DeleteRange) {
  std::string trace_path = test_path_ + "/trace";
  std::string output_path = test_path_ + "/range_delete";
  std::string file_path;
  std::vector<std::string> paras = {"./trace_analyzer",
                                    "-use_get",
                                    "-use_put",
                                    "-use_delete",
                                    "-use_merge",
                                    "-use_single_delete",
                                    "-use_range_delete",
                                    "-output_trace_sequence",
                                    "-output_key_stats",
                                    "-output_access_count_stats",
                                    "-output_prefix=test",
                                    "-output_prefix_cut=1",
                                    "-output_time_series=10",
                                    "-output_value_distribution",
                                    "-output_qps_stats",
                                    "-no_print"};
  Status s = env_->FileExists(trace_path);
  if (!s.ok()) {
    GenerateTrace(trace_path);
  }
  paras.push_back("-output_dir=" + output_path);
  paras.push_back("-trace_file=" + trace_path);
  paras.push_back("-key_space_dir=" + test_path_);

  env_->CreateDir(output_path);
  RunTraceAnalyzer(paras);

  // check the key_stats file
  std::vector<std::string> k_stats = {"0 0 0 1 1.000000", "0 0 1 1 1.000000"};
  file_path = output_path + "/test-range_delete-0-accessed_key_stats.txt";
  CheckFileContent(k_stats, file_path, true);

  // Check the access count distribution
  std::vector<std::string> k_dist = {"access_count: 1 num: 2"};
  file_path =
      output_path + "/test-range_delete-0-accessed_key_count_distribution.txt";
  CheckFileContent(k_dist, file_path, true);

  // Check the trace sequence
  std::vector<std::string> k_sequence = {"1", "5", "2", "3", "4", "0", "0"};
  file_path = output_path + "/test-trace_sequence.txt";
  CheckFileContent(k_sequence, file_path, false);

  // Check the prefix
  std::vector<std::string> k_prefix = {"0 0 0 0.000000 -nan 0x30",
                                       "1 1 1 1.000000 1.000000 0x65"};
  file_path = output_path + "/test-range_delete-0-accessed_key_prefix_cut.txt";
  CheckFileContent(k_prefix, file_path, true);

  // Check the time series
  std::vector<std::string> k_series = {"4 1533000630 0", "4 1533060100 1"};
  file_path = output_path + "/test-range_delete-0-time_series.txt";
  CheckFileContent(k_series, file_path, false);

  // Check the accessed key in whole key space
  std::vector<std::string> k_whole_access = {"4 1", "5 1"};
  file_path = output_path + "/test-range_delete-0-whole_key_stats.txt";
  CheckFileContent(k_whole_access, file_path, true);

  // Check the whole key prefix cut
  std::vector<std::string> k_whole_prefix = {"0 0x61", "1 0x62", "2 0x63",
                                             "3 0x64", "4 0x65", "5 0x66"};
  file_path = output_path + "/test-range_delete-0-whole_key_prefix_cut.txt";
  CheckFileContent(k_whole_prefix, file_path, true);

  // Check the overall qps
  std::vector<std::string> all_qps = {"1 1 1 1 2 1 0 7"};
  file_path = output_path + "/test-qps_stats.txt";
  CheckFileContent(all_qps, file_path, true);

  // Check the qps of get
  std::vector<std::string> get_qps = {"2"};
  file_path = output_path + "/test-range_delete-0-qps_stats.txt";
  CheckFileContent(get_qps, file_path, true);

  // Check the top k qps prefix cut
  std::vector<std::string> top_qps = {"At time: 0 with QPS: 2",
                                      "The prefix: 0x65 Access count: 1",
                                      "The prefix: 0x66 Access count: 1"};
  file_path =
      output_path + "/test-range_delete-0-accessed_top_k_qps_prefix_cut.txt";
  CheckFileContent(top_qps, file_path, true);
}

}  // namespace rocksdb

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

#else
#include <stdio.h>

int main(int /*argc*/, char** /*argv*/) {
  fprintf(stderr, "Trace_analyzer test is not supported in ROCKSDB_LITE\n");
  return 0;
}

#endif  // !ROCKSDB_LITE  return RUN_ALL_TESTS();
