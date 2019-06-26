//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#ifndef ROCKSDB_LITE
#ifdef GFLAGS
#include "tools/block_cache_trace_analyzer.h"

#include <cinttypes>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include "monitoring/histogram.h"
#include "util/gflags_compat.h"
#include "util/string_util.h"

using GFLAGS_NAMESPACE::ParseCommandLineFlags;

DEFINE_string(block_cache_trace_path, "", "The trace file path.");
DEFINE_string(
    block_cache_sim_config_path, "",
    "The config file path. One cache configuration per line. The format of a "
    "cache configuration is "
    "cache_name,num_shard_bits,ghost_capacity,cache_capacity_1,...,cache_"
    "capacity_N. Supported cache names are lru, lru_priority, lru_hybrid, and "
    "lru_hybrid_no_insert_on_row_miss. User may also add a prefix 'ghost_' to "
    "a cache_name to add a ghost cache in front of the real cache. "
    "ghost_capacity and cache_capacity can be xK, xM or xG where x is a "
    "positive number.");
DEFINE_int32(block_cache_trace_downsample_ratio, 1,
             "The trace collected accesses on one in every "
             "block_cache_trace_downsample_ratio blocks. We scale "
             "down the simulated cache size by this ratio.");
DEFINE_bool(print_block_size_stats, false,
            "Print block size distribution and the distribution break down by "
            "block type and column family.");
DEFINE_bool(print_access_count_stats, false,
            "Print access count distribution and the distribution break down "
            "by block type and column family.");
DEFINE_bool(print_data_block_access_count_stats, false,
            "Print data block accesses by user Get and Multi-Get.");
DEFINE_int32(cache_sim_warmup_seconds, 0,
             "The number of seconds to warmup simulated caches. The hit/miss "
             "counters are reset after the warmup completes.");
DEFINE_int32(analyze_bottom_k_access_count_blocks, 0,
             "Print out detailed access information for blocks with their "
             "number of accesses are the bottom k among all blocks.");
DEFINE_int32(analyze_top_k_access_count_blocks, 0,
             "Print out detailed access information for blocks with their "
             "number of accesses are the top k among all blocks.");
DEFINE_string(
    block_cache_analysis_result_dir, "",
    "The directory that saves block cache analysis results. It contains 1) a "
    "mrc file that saves the computed miss ratios for simulated caches. Its "
    "format is "
    "cache_name,num_shard_bits,capacity,miss_ratio,total_accesses. 2) Several "
    "\"label_access_timeline\" files that contain number of accesses per "
    "second grouped by the label. File format: "
    "time,label_1_access_per_second,label_2_access_per_second,...,label_N_"
    "access_per_second where N is the number of unique labels found in the "
    "trace. 3) Several \"label_reuse_distance\" and \"label_reuse_interval\" "
    "csv files that contain the reuse distance/interval grouped by label. File "
    "format: bucket,label_1,label_2,...,label_N. The first N buckets are "
    "absolute values. The second N buckets are percentage values.");
DEFINE_string(
    timeline_labels, "",
    "Group the number of accesses per block per second using these labels. "
    "Possible labels are a combination of the following: cf (column family), "
    "sst, level, bt (block type), caller, block. For example, label \"cf_bt\" "
    "means the number of acccess per second is grouped by unique pairs of "
    "\"cf_bt\". A label \"all\" contains the aggregated number of accesses per "
    "second across all possible labels.");
DEFINE_string(reuse_distance_labels, "",
              "Group the reuse distance of a block using these labels. Reuse "
              "distance is defined as the cumulated size of unique blocks read "
              "between two consecutive accesses on the same block.");
DEFINE_string(
    reuse_distance_buckets, "",
    "Group blocks by their reuse distances given these buckets. For "
    "example, if 'reuse_distance_buckets' is '1K,1M,1G', we will "
    "create four buckets. The first three buckets contain the number of "
    "blocks with reuse distance less than 1KB, between 1K and 1M, between 1M "
    "and 1G, respectively. The last bucket contains the number of blocks with "
    "reuse distance larger than 1G. ");
DEFINE_string(
    reuse_interval_labels, "",
    "Group the reuse interval of a block using these labels. Reuse "
    "interval is defined as the time between two consecutive accesses "
    "on the same block.");
DEFINE_string(
    reuse_interval_buckets, "",
    "Group blocks by their reuse interval given these buckets. For "
    "example, if 'reuse_distance_buckets' is '1,10,100', we will "
    "create four buckets. The first three buckets contain the number of "
    "blocks with reuse interval less than 1 second, between 1 second and 10 "
    "seconds, between 10 seconds and 100 seconds, respectively. The last "
    "bucket contains the number of blocks with reuse interval longer than 100 "
    "seconds.");
DEFINE_string(
    analyze_callers, "",
    "The list of callers to perform a detailed analysis on. If speicfied, the "
    "analyzer will output a detailed percentage of accesses for each caller "
    "break down by column family, level, and block type. A list of available "
    "callers are: Get, MultiGet, Iterator, ApproximateSize, VerifyChecksum, "
    "SSTDumpTool, ExternalSSTIngestion, Repair, Prefetch, Compaction, "
    "CompactionRefill, Flush, SSTFileReader, Uncategorized.");
DEFINE_string(access_count_buckets, "",
              "Group number of blocks by their access count given these "
              "buckets. If specified, the analyzer will output a detailed "
              "analysis on the number of blocks grouped by their access count "
              "break down by block type and column family.");

namespace rocksdb {
namespace {

const std::string kMissRatioCurveFileName = "mrc";
const std::string kGroupbyBlock = "block";
const std::string kGroupbyColumnFamily = "cf";
const std::string kGroupbySSTFile = "sst";
const std::string kGroupbyBlockType = "bt";
const std::string kGroupbyCaller = "caller";
const std::string kGroupbyLevel = "level";
const std::string kGroupbyAll = "all";
const std::set<std::string> kGroupbyLabels{
    kGroupbyBlock,     kGroupbyColumnFamily, kGroupbySSTFile, kGroupbyLevel,
    kGroupbyBlockType, kGroupbyCaller,       kGroupbyAll};
const std::string kSupportedCacheNames =
    " lru ghost_lru lru_priority ghost_lru_priority lru_hybrid "
    "ghost_lru_hybrid lru_hybrid_no_insert_on_row_miss "
    "ghost_lru_hybrid_no_insert_on_row_miss ";

std::string block_type_to_string(TraceType type) {
  switch (type) {
    case kBlockTraceFilterBlock:
      return "Filter";
    case kBlockTraceDataBlock:
      return "Data";
    case kBlockTraceIndexBlock:
      return "Index";
    case kBlockTraceRangeDeletionBlock:
      return "RangeDeletion";
    case kBlockTraceUncompressionDictBlock:
      return "UncompressionDict";
    default:
      break;
  }
  // This cannot happen.
  return "InvalidType";
}

std::string caller_to_string(TableReaderCaller caller) {
  switch (caller) {
    case kUserGet:
      return "Get";
    case kUserMultiGet:
      return "MultiGet";
    case kUserIterator:
      return "Iterator";
    case kUserApproximateSize:
      return "ApproximateSize";
    case kUserVerifyChecksum:
      return "VerifyChecksum";
    case kSSTDumpTool:
      return "SSTDumpTool";
    case kExternalSSTIngestion:
      return "ExternalSSTIngestion";
    case kRepair:
      return "Repair";
    case kPrefetch:
      return "Prefetch";
    case kCompaction:
      return "Compaction";
    case kCompactionRefill:
      return "CompactionRefill";
    case kFlush:
      return "Flush";
    case kSSTFileReader:
      return "SSTFileReader";
    case kUncategorized:
      return "Uncategorized";
    default:
      break;
  }
  // This cannot happen.
  return "InvalidCaller";
}

TableReaderCaller string_to_caller(std::string caller_str) {
  if (caller_str == "Get") {
    return kUserGet;
  } else if (caller_str == "MultiGet") {
    return kUserMultiGet;
  } else if (caller_str == "Iterator") {
    return kUserIterator;
  } else if (caller_str == "ApproximateSize") {
    return kUserApproximateSize;
  } else if (caller_str == "VerifyChecksum") {
    return kUserVerifyChecksum;
  } else if (caller_str == "SSTDumpTool") {
    return kSSTDumpTool;
  } else if (caller_str == "ExternalSSTIngestion") {
    return kExternalSSTIngestion;
  } else if (caller_str == "Repair") {
    return kRepair;
  } else if (caller_str == "Prefetch") {
    return kPrefetch;
  } else if (caller_str == "Compaction") {
    return kCompaction;
  } else if (caller_str == "CompactionRefill") {
    return kCompactionRefill;
  } else if (caller_str == "Flush") {
    return kFlush;
  } else if (caller_str == "SSTFileReader") {
    return kSSTFileReader;
  } else if (caller_str == "Uncategorized") {
    return kUncategorized;
  }
  return TableReaderCaller::kMaxBlockCacheLookupCaller;
}

bool is_user_access(TableReaderCaller caller) {
  switch (caller) {
    case kUserGet:
    case kUserMultiGet:
    case kUserIterator:
    case kUserApproximateSize:
    case kUserVerifyChecksum:
      return true;
    default:
      break;
  }
  return false;
}

const char kBreakLine[] =
    "***************************************************************\n";

void print_break_lines(uint32_t num_break_lines) {
  for (uint32_t i = 0; i < num_break_lines; i++) {
    fprintf(stdout, kBreakLine);
  }
}

double percent(uint64_t numerator, uint64_t denomenator) {
  if (denomenator == 0) {
    return -1;
  }
  return static_cast<double>(numerator * 100.0 / denomenator);
}

}  // namespace

void BlockCacheTraceAnalyzer::WriteMissRatioCurves() const {
  if (!cache_simulator_) {
    return;
  }
  if (output_dir_.empty()) {
    return;
  }
  const std::string output_miss_ratio_curve_path =
      output_dir_ + "/" + kMissRatioCurveFileName;
  std::ofstream out(output_miss_ratio_curve_path);
  if (!out.is_open()) {
    return;
  }
  // Write header.
  const std::string header =
      "cache_name,num_shard_bits,ghost_capacity,capacity,miss_ratio,total_"
      "accesses";
  out << header << std::endl;
  for (auto const& config_caches : cache_simulator_->sim_caches()) {
    const CacheConfiguration& config = config_caches.first;
    for (uint32_t i = 0; i < config.cache_capacities.size(); i++) {
      double miss_ratio = config_caches.second[i]->miss_ratio();
      // Write the body.
      out << config.cache_name;
      out << ",";
      out << config.num_shard_bits;
      out << ",";
      out << config.ghost_cache_capacity;
      out << ",";
      out << config.cache_capacities[i];
      out << ",";
      out << std::fixed << std::setprecision(4) << miss_ratio;
      out << ",";
      out << config_caches.second[i]->total_accesses();
      out << std::endl;
    }
  }
  out.close();
}

std::set<std::string> BlockCacheTraceAnalyzer::ParseLabelStr(
    const std::string& label_str) const {
  std::stringstream ss(label_str);
  std::set<std::string> labels;
  // label_str is in the form of "label1_label2_label3", e.g., cf_bt.
  while (ss.good()) {
    std::string label_name;
    getline(ss, label_name, '_');
    if (kGroupbyLabels.find(label_name) == kGroupbyLabels.end()) {
      // Unknown label name.
      fprintf(stderr, "Unknown label name %s, label string %s\n",
              label_name.c_str(), label_str.c_str());
      return {};
    }
    labels.insert(label_name);
  }
  return labels;
}

std::string BlockCacheTraceAnalyzer::BuildLabel(
    const std::set<std::string>& labels, const std::string& cf_name,
    uint64_t fd, uint32_t level, TraceType type, TableReaderCaller caller,
    const std::string& block_key) const {
  std::map<std::string, std::string> label_value_map;
  label_value_map[kGroupbyAll] = kGroupbyAll;
  label_value_map[kGroupbyLevel] = std::to_string(level);
  label_value_map[kGroupbyCaller] = caller_to_string(caller);
  label_value_map[kGroupbySSTFile] = std::to_string(fd);
  label_value_map[kGroupbyBlockType] = block_type_to_string(type);
  label_value_map[kGroupbyColumnFamily] = cf_name;
  label_value_map[kGroupbyBlock] = block_key;
  // Concatenate the label values.
  std::string label;
  for (auto const& l : labels) {
    label += label_value_map[l];
    label += "-";
  }
  if (!label.empty()) {
    label.pop_back();
  }
  return label;
}

void BlockCacheTraceAnalyzer::WriteAccessTimeline(
    const std::string& label_str) const {
  std::set<std::string> labels = ParseLabelStr(label_str);
  uint64_t start_time = port::kMaxUint64;
  uint64_t end_time = 0;
  std::map<std::string, std::map<uint64_t, uint64_t>> label_access_timeline;
  for (auto const& cf_aggregates : cf_aggregates_map_) {
    // Stats per column family.
    const std::string& cf_name = cf_aggregates.first;
    for (auto const& file_aggregates : cf_aggregates.second.fd_aggregates_map) {
      // Stats per SST file.
      const uint64_t fd = file_aggregates.first;
      const uint32_t level = file_aggregates.second.level;
      for (auto const& block_type_aggregates :
           file_aggregates.second.block_type_aggregates_map) {
        // Stats per block type.
        const TraceType type = block_type_aggregates.first;
        for (auto const& block_access_info :
             block_type_aggregates.second.block_access_info_map) {
          // Stats per block.
          for (auto const& timeline :
               block_access_info.second.caller_num_accesses_timeline) {
            const TableReaderCaller caller = timeline.first;
            const std::string& block_key = block_access_info.first;
            const std::string label =
                BuildLabel(labels, cf_name, fd, level, type, caller, block_key);
            for (auto const& naccess : timeline.second) {
              const uint64_t timestamp = naccess.first;
              const uint64_t num = naccess.second;
              label_access_timeline[label][timestamp] += num;
              start_time = std::min(start_time, timestamp);
              end_time = std::max(end_time, timestamp);
            }
          }
        }
      }
    }
  }

  // We have label_access_timeline now. Write them into a file.
  const std::string output_path =
      output_dir_ + "/" + label_str + "_access_timeline";
  std::ofstream out(output_path);
  if (!out.is_open()) {
    return;
  }
  std::string header("time");
  for (auto const& label : label_access_timeline) {
    header += ",";
    header += label.first;
  }
  out << header << std::endl;
  std::string row;
  for (uint64_t now = start_time; now <= end_time; now++) {
    row = std::to_string(now);
    for (auto const& label : label_access_timeline) {
      auto it = label.second.find(now);
      row += ",";
      if (it != label.second.end()) {
        row += std::to_string(it->second);
      } else {
        row += "0";
      }
    }
    out << row << std::endl;
  }
  out.close();
}

void BlockCacheTraceAnalyzer::WriteReuseDistance(
    const std::string& label_str,
    const std::vector<uint64_t>& distance_buckets) const {
  std::set<std::string> labels = ParseLabelStr(label_str);
  std::map<std::string, std::map<uint64_t, uint64_t>> label_distance_num_reuses;
  uint64_t total_num_reuses = 0;
  for (auto const& cf_aggregates : cf_aggregates_map_) {
    // Stats per column family.
    const std::string& cf_name = cf_aggregates.first;
    for (auto const& file_aggregates : cf_aggregates.second.fd_aggregates_map) {
      // Stats per SST file.
      const uint64_t fd = file_aggregates.first;
      const uint32_t level = file_aggregates.second.level;
      for (auto const& block_type_aggregates :
           file_aggregates.second.block_type_aggregates_map) {
        // Stats per block type.
        const TraceType type = block_type_aggregates.first;
        for (auto const& block_access_info :
             block_type_aggregates.second.block_access_info_map) {
          // Stats per block.
          const std::string& block_key = block_access_info.first;
          const std::string label = BuildLabel(
              labels, cf_name, fd, level, type,
              TableReaderCaller::kMaxBlockCacheLookupCaller, block_key);
          if (label_distance_num_reuses.find(label) ==
              label_distance_num_reuses.end()) {
            // The first time we encounter this label.
            for (auto const& distance_bucket : distance_buckets) {
              label_distance_num_reuses[label][distance_bucket] = 0;
            }
          }
          for (auto const& reuse_distance :
               block_access_info.second.reuse_distance_count) {
            label_distance_num_reuses[label]
                .upper_bound(reuse_distance.first)
                ->second += reuse_distance.second;
            total_num_reuses += reuse_distance.second;
          }
        }
      }
    }
  }

  // We have label_naccesses and label_distance_num_reuses now. Write them into
  // a file.
  const std::string output_path =
      output_dir_ + "/" + label_str + "_reuse_distance";
  std::ofstream out(output_path);
  if (!out.is_open()) {
    return;
  }
  std::string header("bucket");
  for (auto const& label_it : label_distance_num_reuses) {
    header += ",";
    header += label_it.first;
  }
  out << header << std::endl;
  // Absolute values.
  for (auto const& bucket : distance_buckets) {
    std::string row(std::to_string(bucket));
    for (auto const& label_it : label_distance_num_reuses) {
      auto const& it = label_it.second.find(bucket);
      assert(it != label_it.second.end());
      row += ",";
      row += std::to_string(it->second);
    }
    out << row << std::endl;
  }
  // Percentage values.
  for (auto const& bucket : distance_buckets) {
    std::string row(std::to_string(bucket));
    for (auto const& label_it : label_distance_num_reuses) {
      auto const& it = label_it.second.find(bucket);
      assert(it != label_it.second.end());
      row += ",";
      row += std::to_string(percent(it->second, total_num_reuses));
    }
    out << row << std::endl;
  }
  out.close();
}

void BlockCacheTraceAnalyzer::UpdateReuseIntervalStats(
    const std::string& label, const std::vector<uint64_t>& time_buckets,
    const std::map<uint64_t, uint64_t> timeline,
    std::map<std::string, std::map<uint64_t, uint64_t>>* label_time_num_reuses,
    uint64_t* total_num_reuses) const {
  assert(label_time_num_reuses);
  assert(total_num_reuses);
  if (label_time_num_reuses->find(label) == label_time_num_reuses->end()) {
    // The first time we encounter this label.
    for (auto const& time_bucket : time_buckets) {
      (*label_time_num_reuses)[label][time_bucket] = 0;
    }
  }
  auto it = timeline.begin();
  uint64_t prev_timestamp = it->first;
  const uint64_t prev_num = it->second;
  it++;
  // Reused within one second.
  if (prev_num > 1) {
    (*label_time_num_reuses)[label].upper_bound(0)->second += prev_num - 1;
    *total_num_reuses += prev_num - 1;
  }
  while (it != timeline.end()) {
    const uint64_t timestamp = it->first;
    const uint64_t num = it->second;
    const uint64_t reuse_interval = timestamp - prev_timestamp;
    (*label_time_num_reuses)[label].upper_bound(reuse_interval)->second += 1;
    if (num > 1) {
      (*label_time_num_reuses)[label].upper_bound(0)->second += num - 1;
    }
    prev_timestamp = timestamp;
    *total_num_reuses += num;
    it++;
  }
}

void BlockCacheTraceAnalyzer::WriteReuseInterval(
    const std::string& label_str,
    const std::vector<uint64_t>& time_buckets) const {
  std::set<std::string> labels = ParseLabelStr(label_str);
  std::map<std::string, std::map<uint64_t, uint64_t>> label_time_num_reuses;
  uint64_t total_num_reuses = 0;
  for (auto const& cf_aggregates : cf_aggregates_map_) {
    // Stats per column family.
    const std::string& cf_name = cf_aggregates.first;
    for (auto const& file_aggregates : cf_aggregates.second.fd_aggregates_map) {
      // Stats per SST file.
      const uint64_t fd = file_aggregates.first;
      const uint32_t level = file_aggregates.second.level;
      for (auto const& block_type_aggregates :
           file_aggregates.second.block_type_aggregates_map) {
        // Stats per block type.
        const TraceType type = block_type_aggregates.first;
        for (auto const& block_access_info :
             block_type_aggregates.second.block_access_info_map) {
          // Stats per block.
          const std::string& block_key = block_access_info.first;
          if (labels.find(kGroupbyCaller) != labels.end()) {
            for (auto const& timeline :
                 block_access_info.second.caller_num_accesses_timeline) {
              const TableReaderCaller caller = timeline.first;
              const std::string label = BuildLabel(labels, cf_name, fd, level,
                                                   type, caller, block_key);
              UpdateReuseIntervalStats(label, time_buckets, timeline.second,
                                       &label_time_num_reuses,
                                       &total_num_reuses);
            }
            continue;
          }
          // Does not group by caller so we need to flatten the access timeline.
          const std::string label = BuildLabel(
              labels, cf_name, fd, level, type,
              TableReaderCaller::kMaxBlockCacheLookupCaller, block_key);
          std::map<uint64_t, uint64_t> timeline;
          for (auto const& caller_timeline :
               block_access_info.second.caller_num_accesses_timeline) {
            for (auto const& time_naccess : caller_timeline.second) {
              timeline[time_naccess.first] += time_naccess.second;
            }
          }
          UpdateReuseIntervalStats(label, time_buckets, timeline,
                                   &label_time_num_reuses, &total_num_reuses);
        }
      }
    }
  }

  // We have label_naccesses and label_interval_num_reuses now. Write them into
  // a file.
  const std::string output_path =
      output_dir_ + "/" + label_str + "_reuse_interval";
  std::ofstream out(output_path);
  if (!out.is_open()) {
    return;
  }
  std::string header("bucket");
  for (auto const& label_it : label_time_num_reuses) {
    header += ",";
    header += label_it.first;
  }
  out << header << std::endl;
  // Absolute values.
  for (auto const& bucket : time_buckets) {
    std::string row(std::to_string(bucket));
    for (auto const& label_it : label_time_num_reuses) {
      auto const& it = label_it.second.find(bucket);
      assert(it != label_it.second.end());
      row += ",";
      row += std::to_string(it->second);
    }
    out << row << std::endl;
  }
  // Percentage values.
  for (auto const& bucket : time_buckets) {
    std::string row(std::to_string(bucket));
    for (auto const& label_it : label_time_num_reuses) {
      auto const& it = label_it.second.find(bucket);
      assert(it != label_it.second.end());
      row += ",";
      row += std::to_string(percent(it->second, total_num_reuses));
    }
    out << row << std::endl;
  }
  out.close();
}

std::string BlockCacheTraceAnalyzer::OutputPercentAccessStats(
    uint64_t total_accesses,
    const std::map<std::string, uint64_t>& cf_access_count) const {
  std::string row;
  for (auto const& cf_aggregates : cf_aggregates_map_) {
    const std::string& cf_name = cf_aggregates.first;
    const auto& naccess = cf_access_count.find(cf_name);
    row += ",";
    if (naccess != cf_access_count.end()) {
      row += std::to_string(percent(naccess->second, total_accesses));
    } else {
      row += "0";
    }
  }
  return row;
}

void BlockCacheTraceAnalyzer::WritePercentAccessSummaryStats() const {
  std::map<TableReaderCaller, std::map<std::string, uint64_t>>
      caller_cf_accesses;
  uint64_t total_accesses = 0;
  for (auto const& cf_aggregates : cf_aggregates_map_) {
    const std::string& cf_name = cf_aggregates.first;
    for (auto const& file_aggregates : cf_aggregates.second.fd_aggregates_map) {
      for (auto const& block_type_aggregates :
           file_aggregates.second.block_type_aggregates_map) {
        for (auto const& block_access_info :
             block_type_aggregates.second.block_access_info_map) {
          for (auto const& caller_num :
               block_access_info.second.caller_num_access_map) {
            const TableReaderCaller caller = caller_num.first;
            const uint64_t naccess = caller_num.second;
            caller_cf_accesses[caller][cf_name] += naccess;
            total_accesses += naccess;
          }
        }
      }
    }
  }

  const std::string output_path =
      output_dir_ + "/percentage_of_accesses_summary";
  std::ofstream out(output_path);
  if (!out.is_open()) {
    return;
  }
  std::string header("caller");
  for (auto const& cf_name : cf_aggregates_map_) {
    header += ",";
    header += cf_name.first;
  }
  out << header << std::endl;
  for (auto const& cf_naccess_it : caller_cf_accesses) {
    const TableReaderCaller caller = cf_naccess_it.first;
    std::string row;
    row += caller_to_string(caller);
    row += OutputPercentAccessStats(total_accesses, cf_naccess_it.second);
    out << row << std::endl;
  }
  out.close();
}

void BlockCacheTraceAnalyzer::WriteDetailedPercentAccessSummaryStats(
    TableReaderCaller analyzing_caller) const {
  std::map<uint32_t, std::map<std::string, uint64_t>> level_cf_accesses;
  std::map<TraceType, std::map<std::string, uint64_t>> bt_cf_accesses;
  uint64_t total_accesses = 0;
  for (auto const& cf_aggregates : cf_aggregates_map_) {
    const std::string& cf_name = cf_aggregates.first;
    for (auto const& file_aggregates : cf_aggregates.second.fd_aggregates_map) {
      const uint32_t level = file_aggregates.second.level;
      for (auto const& block_type_aggregates :
           file_aggregates.second.block_type_aggregates_map) {
        const TraceType block_type = block_type_aggregates.first;
        for (auto const& block_access_info :
             block_type_aggregates.second.block_access_info_map) {
          for (auto const& caller_num :
               block_access_info.second.caller_num_access_map) {
            const TableReaderCaller caller = caller_num.first;
            if (caller == analyzing_caller) {
              const uint64_t naccess = caller_num.second;
              level_cf_accesses[level][cf_name] += naccess;
              bt_cf_accesses[block_type][cf_name] += naccess;
              total_accesses += naccess;
            }
          }
        }
      }
    }
  }

  {
    const std::string output_path = output_dir_ + "/" +
                                    caller_to_string(analyzing_caller) +
                                    "_level_percentage_of_accesses_summary";
    std::ofstream out(output_path);
    if (!out.is_open()) {
      return;
    }
    std::string header("level");
    for (auto const& cf_name : cf_aggregates_map_) {
      header += ",";
      header += cf_name.first;
    }
    out << header << std::endl;
    for (auto const& level_naccess_it : level_cf_accesses) {
      const uint32_t level = level_naccess_it.first;
      std::string row;
      row += std::to_string(level);
      row += OutputPercentAccessStats(total_accesses, level_naccess_it.second);
      out << row << std::endl;
    }
    out.close();
  }
  {
    const std::string output_path = output_dir_ + "/" +
                                    caller_to_string(analyzing_caller) +
                                    "_bt_percentage_of_accesses_summary";
    std::ofstream out(output_path);
    if (!out.is_open()) {
      return;
    }
    std::string header("bt");
    for (auto const& cf_name : cf_aggregates_map_) {
      header += ",";
      header += cf_name.first;
    }
    out << header << std::endl;
    for (auto const& bt_naccess_it : bt_cf_accesses) {
      const TraceType bt = bt_naccess_it.first;
      std::string row;
      row += block_type_to_string(bt);
      row += OutputPercentAccessStats(total_accesses, bt_naccess_it.second);
      out << row << std::endl;
    }
    out.close();
  }
}

void BlockCacheTraceAnalyzer::WriteAccessCountSummaryStats(
    const std::vector<uint64_t>& access_count_buckets,
    bool user_access_only) const {
  // x: buckets.
  // y: # of accesses.
  std::map<TraceType, std::map<uint64_t, uint64_t>> bt_access_nblocks;
  std::map<std::string, std::map<uint64_t, uint64_t>> cf_access_nblocks;
  std::set<TraceType> block_types;
  uint64_t total_nblocks = 0;
  for (auto const& cf_aggregates : cf_aggregates_map_) {
    const std::string& cf_name = cf_aggregates.first;
    if (cf_access_nblocks.find(cf_name) == cf_access_nblocks.end()) {
      // initialize.
      for (auto& access : access_count_buckets) {
        cf_access_nblocks[cf_name][access] = 0;
      }
    }
    for (auto const& file_aggregates : cf_aggregates.second.fd_aggregates_map) {
      for (auto const& block_type_aggregates :
           file_aggregates.second.block_type_aggregates_map) {
        const TraceType block_type = block_type_aggregates.first;
        block_types.insert(block_type);
        if (bt_access_nblocks.find(block_type) == bt_access_nblocks.end()) {
          // initialize.
          for (auto& access : access_count_buckets) {
            bt_access_nblocks[block_type][access] = 0;
          }
        }

        for (auto const& block_access_info :
             block_type_aggregates.second.block_access_info_map) {
          total_nblocks += 1;
          uint64_t naccesses = 0;
          if (user_access_only) {
            for (auto const& caller_access :
                 block_access_info.second.caller_num_access_map) {
              if (is_user_access(caller_access.first)) {
                naccesses += caller_access.second;
              }
            }
          } else {
            naccesses = block_access_info.second.num_accesses;
          }
          bt_access_nblocks[block_type].upper_bound(naccesses)->second += 1;
          cf_access_nblocks[cf_name].upper_bound(naccesses)->second += 1;
        }
      }
    }
  }

  const std::string user_access_prefix =
      user_access_only ? "user_access_only_" : "all_access_";
  {
    const std::string output_path =
        output_dir_ + "/" + user_access_prefix + "cf_access_count_summary";
    std::ofstream out(output_path);
    if (!out.is_open()) {
      return;
    }
    std::string header("cf");
    for (auto const& access_count : access_count_buckets) {
      header += ",";
      header += std::to_string(access_count);
    }
    out << header << std::endl;
    for (auto const& cf_access_nblocks_it : cf_access_nblocks) {
      const std::string& cf_name = cf_access_nblocks_it.first;
      std::string row;
      row += cf_name;
      for (auto const& access_count : access_count_buckets) {
        const auto& nblocks = cf_access_nblocks_it.second.find(access_count);
        row += ",";
        if (nblocks != cf_access_nblocks_it.second.end()) {
          row += std::to_string(percent(nblocks->second, total_nblocks));
        } else {
          row += "0";
        }
      }
      out << row << std::endl;
    }
    out.close();
  }
  {
    const std::string output_path =
        output_dir_ + "/" + user_access_prefix + "bt_access_count_summary";
    std::ofstream out(output_path);
    if (!out.is_open()) {
      return;
    }
    std::string header("bt");
    for (auto const& access_count : access_count_buckets) {
      header += ",";
      header += std::to_string(access_count);
    }
    out << header << std::endl;
    for (auto const& bt_access_nblocks_it : bt_access_nblocks) {
      const TraceType block_type = bt_access_nblocks_it.first;
      std::string row;
      row += block_type_to_string(block_type);
      for (auto const& access_count : access_count_buckets) {
        const auto& nblocks = bt_access_nblocks_it.second.find(access_count);
        row += ",";
        if (nblocks != bt_access_nblocks_it.second.end()) {
          row += std::to_string(percent(nblocks->second, total_nblocks));
        } else {
          row += "0";
        }
      }
      out << row << std::endl;
    }
    out.close();
  }
}

BlockCacheTraceAnalyzer::BlockCacheTraceAnalyzer(
    const std::string& trace_file_path, const std::string& output_dir,
    bool compute_reuse_distance,
    std::unique_ptr<BlockCacheTraceSimulator>&& cache_simulator)
    : env_(rocksdb::Env::Default()),
      trace_file_path_(trace_file_path),
      output_dir_(output_dir),
      compute_reuse_distance_(compute_reuse_distance),
      cache_simulator_(std::move(cache_simulator)) {}

void BlockCacheTraceAnalyzer::ComputeReuseDistance(
    BlockAccessInfo* info) const {
  assert(info);
  if (info->num_accesses == 0) {
    return;
  }
  uint64_t reuse_distance = 0;
  for (auto const& block_key : info->unique_blocks_since_last_access) {
    auto const& it = block_info_map_.find(block_key);
    // This block must exist.
    assert(it != block_info_map_.end());
    reuse_distance += it->second->block_size;
  }
  info->reuse_distance_count[reuse_distance] += 1;
  // We clear this hash set since this is the second access on this block.
  info->unique_blocks_since_last_access.clear();
}

void BlockCacheTraceAnalyzer::RecordAccess(
    const BlockCacheTraceRecord& access) {
  ColumnFamilyAccessInfoAggregate& cf_aggr = cf_aggregates_map_[access.cf_name];
  SSTFileAccessInfoAggregate& file_aggr =
      cf_aggr.fd_aggregates_map[access.sst_fd_number];
  file_aggr.level = access.level;
  BlockTypeAccessInfoAggregate& block_type_aggr =
      file_aggr.block_type_aggregates_map[access.block_type];
  BlockAccessInfo& block_access_info =
      block_type_aggr.block_access_info_map[access.block_key];
  if (compute_reuse_distance_) {
    ComputeReuseDistance(&block_access_info);
  }
  block_access_info.AddAccess(access);
  block_info_map_[access.block_key] = &block_access_info;

  if (compute_reuse_distance_) {
    // Add this block to all existing blocks.
    for (auto& cf_aggregates : cf_aggregates_map_) {
      for (auto& file_aggregates : cf_aggregates.second.fd_aggregates_map) {
        for (auto& block_type_aggregates :
             file_aggregates.second.block_type_aggregates_map) {
          for (auto& existing_block :
               block_type_aggregates.second.block_access_info_map) {
            existing_block.second.unique_blocks_since_last_access.insert(
                access.block_key);
          }
        }
      }
    }
  }
}

Status BlockCacheTraceAnalyzer::Analyze() {
  std::unique_ptr<TraceReader> trace_reader;
  Status s =
      NewFileTraceReader(env_, EnvOptions(), trace_file_path_, &trace_reader);
  if (!s.ok()) {
    return s;
  }
  BlockCacheTraceReader reader(std::move(trace_reader));
  s = reader.ReadHeader(&header_);
  if (!s.ok()) {
    return s;
  }
  while (s.ok()) {
    BlockCacheTraceRecord access;
    s = reader.ReadAccess(&access);
    if (!s.ok()) {
      return s;
    }
    RecordAccess(access);
    if (cache_simulator_) {
      cache_simulator_->Access(access);
    }
  }
  return Status::OK();
}

void BlockCacheTraceAnalyzer::PrintBlockSizeStats() const {
  HistogramStat bs_stats;
  std::map<TraceType, HistogramStat> bt_stats_map;
  std::map<std::string, std::map<TraceType, HistogramStat>> cf_bt_stats_map;
  for (auto const& cf_aggregates : cf_aggregates_map_) {
    // Stats per column family.
    const std::string& cf_name = cf_aggregates.first;
    for (auto const& file_aggregates : cf_aggregates.second.fd_aggregates_map) {
      // Stats per SST file.
      for (auto const& block_type_aggregates :
           file_aggregates.second.block_type_aggregates_map) {
        // Stats per block type.
        const TraceType type = block_type_aggregates.first;
        for (auto const& block_access_info :
             block_type_aggregates.second.block_access_info_map) {
          // Stats per block.
          if (block_access_info.second.block_size == 0) {
            // Block size may be 0 when 1) compaction observes a cache miss and
            // does not insert the missing block into the cache again. 2)
            // fetching filter blocks in SST files at the last level.
            continue;
          }
          bs_stats.Add(block_access_info.second.block_size);
          bt_stats_map[type].Add(block_access_info.second.block_size);
          cf_bt_stats_map[cf_name][type].Add(
              block_access_info.second.block_size);
        }
      }
    }
  }
  fprintf(stdout, "Block size stats: \n%s", bs_stats.ToString().c_str());
  for (auto const& bt_stats : bt_stats_map) {
    print_break_lines(/*num_break_lines=*/1);
    fprintf(stdout, "Block size stats for block type %s: \n%s",
            block_type_to_string(bt_stats.first).c_str(),
            bt_stats.second.ToString().c_str());
  }
  for (auto const& cf_bt_stats : cf_bt_stats_map) {
    const std::string& cf_name = cf_bt_stats.first;
    for (auto const& bt_stats : cf_bt_stats.second) {
      print_break_lines(/*num_break_lines=*/1);
      fprintf(stdout,
              "Block size stats for column family %s and block type %s: \n%s",
              cf_name.c_str(), block_type_to_string(bt_stats.first).c_str(),
              bt_stats.second.ToString().c_str());
    }
  }
}

void BlockCacheTraceAnalyzer::PrintAccessCountStats(bool user_access_only,
                                                    uint32_t bottom_k,
                                                    uint32_t top_k) const {
  HistogramStat access_stats;
  std::map<TraceType, HistogramStat> bt_stats_map;
  std::map<std::string, std::map<TraceType, HistogramStat>> cf_bt_stats_map;
  std::map<uint64_t, std::vector<std::string>> access_count_blocks;
  for (auto const& cf_aggregates : cf_aggregates_map_) {
    // Stats per column family.
    const std::string& cf_name = cf_aggregates.first;
    for (auto const& file_aggregates : cf_aggregates.second.fd_aggregates_map) {
      // Stats per SST file.
      for (auto const& block_type_aggregates :
           file_aggregates.second.block_type_aggregates_map) {
        // Stats per block type.
        const TraceType type = block_type_aggregates.first;
        for (auto const& block_access_info :
             block_type_aggregates.second.block_access_info_map) {
          // Stats per block.
          uint64_t naccesses = 0;
          if (user_access_only) {
            for (auto const& caller_access :
                 block_access_info.second.caller_num_access_map) {
              if (is_user_access(caller_access.first)) {
                naccesses += caller_access.second;
              }
            }
          } else {
            naccesses = block_access_info.second.num_accesses;
          }
          if (type == TraceType::kBlockTraceDataBlock) {
            access_count_blocks[naccesses].push_back(block_access_info.first);
          }
          access_stats.Add(naccesses);
          bt_stats_map[type].Add(naccesses);
          cf_bt_stats_map[cf_name][type].Add(naccesses);
        }
      }
    }
  }
  fprintf(stdout,
          "Block access count stats: The number of accesses per block. %s\n%s",
          user_access_only ? "User accesses only" : "All accesses",
          access_stats.ToString().c_str());
  uint32_t bottom_k_index = 0;
  for (auto naccess_it = access_count_blocks.begin();
       naccess_it != access_count_blocks.end(); naccess_it++) {
    bottom_k_index++;
    if (bottom_k_index >= bottom_k) {
      break;
    }
    std::map<TableReaderCaller, uint32_t> caller_naccesses;
    uint64_t naccesses = 0;
    for (auto const& block_id : naccess_it->second) {
      BlockAccessInfo* block = block_info_map_.find(block_id)->second;
      for (auto const& caller_access : block->caller_num_access_map) {
        caller_naccesses[caller_access.first] += caller_access.second;
        naccesses += caller_access.second;
      }
    }
    std::string statistics("Caller:");
    for (auto const& caller_naccessess_it : caller_naccesses) {
      statistics += caller_to_string(caller_naccessess_it.first);
      statistics += ":";
      statistics +=
          std::to_string(percent(caller_naccessess_it.second, naccesses));
      statistics += ",";
    }
    fprintf(stdout,
            "Bottom %" PRIu32 " access count. Access count=%" PRIu64
            " nblocks=%" PRIu64 " %s\n",
            bottom_k, naccess_it->first, naccess_it->second.size(),
            statistics.c_str());
  }

  uint32_t top_k_index = 0;
  for (auto naccess_it = access_count_blocks.rbegin();
       naccess_it != access_count_blocks.rend(); naccess_it++) {
    top_k_index++;
    if (top_k_index >= top_k) {
      break;
    }
    for (auto const& block_id : naccess_it->second) {
      BlockAccessInfo* block = block_info_map_.find(block_id)->second;
      std::string statistics("Caller:");
      for (auto const& caller_access : block->caller_num_access_map) {
        statistics += ",";
        statistics += caller_to_string(caller_access.first);
        statistics += ":";
        statistics +=
            std::to_string(percent(caller_access.second, block->num_accesses));
      }
      statistics += ",num_ref_keys=";
      statistics += std::to_string(block->key_num_access_map.size());
      fprintf(stdout,
              "Top %" PRIu32 " access count blocks access_count=%" PRIu64
              " %s\n",
              top_k, naccess_it->first, statistics.c_str());
    }
  }

  for (auto const& bt_stats : bt_stats_map) {
    print_break_lines(/*num_break_lines=*/1);
    fprintf(stdout, "Break down by block type %s: \n%s",
            block_type_to_string(bt_stats.first).c_str(),
            bt_stats.second.ToString().c_str());
  }
  for (auto const& cf_bt_stats : cf_bt_stats_map) {
    const std::string& cf_name = cf_bt_stats.first;
    for (auto const& bt_stats : cf_bt_stats.second) {
      print_break_lines(/*num_break_lines=*/1);
      fprintf(stdout,
              "Break down by column family %s and block type "
              "%s: \n%s",
              cf_name.c_str(), block_type_to_string(bt_stats.first).c_str(),
              bt_stats.second.ToString().c_str());
    }
  }
}

void BlockCacheTraceAnalyzer::PrintDataBlockAccessStats() const {
  HistogramStat existing_keys_stats;
  std::map<std::string, HistogramStat> cf_existing_keys_stats_map;
  HistogramStat non_existing_keys_stats;
  std::map<std::string, HistogramStat> cf_non_existing_keys_stats_map;
  HistogramStat block_access_stats;
  std::map<std::string, HistogramStat> cf_block_access_info;
  HistogramStat percent_referenced_bytes;
  std::map<std::string, HistogramStat> cf_percent_referenced_bytes;
  // Total number of accesses in a data block / number of keys in a data block.
  HistogramStat avg_naccesses_per_key_in_a_data_block;
  std::map<std::string, HistogramStat> cf_avg_naccesses_per_key_in_a_data_block;
  // The standard deviation on the number of accesses of a key in a data block.
  HistogramStat stdev_naccesses_per_key_in_a_data_block;
  std::map<std::string, HistogramStat>
      cf_stdev_naccesses_per_key_in_a_data_block;

  for (auto const& cf_aggregates : cf_aggregates_map_) {
    // Stats per column family.
    const std::string& cf_name = cf_aggregates.first;
    for (auto const& file_aggregates : cf_aggregates.second.fd_aggregates_map) {
      // Stats per SST file.
      for (auto const& block_type_aggregates :
           file_aggregates.second.block_type_aggregates_map) {
        // Stats per block type.
        for (auto const& block_access_info :
             block_type_aggregates.second.block_access_info_map) {
          // Stats per block.
          if (block_access_info.second.num_keys == 0) {
            continue;
          }
          // Use four decimal points.
          uint64_t percent_referenced_for_existing_keys = (uint64_t)(
              ((double)block_access_info.second.key_num_access_map.size() /
               (double)block_access_info.second.num_keys) *
              10000.0);
          uint64_t percent_referenced_for_non_existing_keys =
              (uint64_t)(((double)block_access_info.second
                              .non_exist_key_num_access_map.size() /
                          (double)block_access_info.second.num_keys) *
                         10000.0);
          uint64_t percent_accesses_for_existing_keys = (uint64_t)(
              ((double)
                   block_access_info.second.num_referenced_key_exist_in_block /
               (double)block_access_info.second.num_accesses) *
              10000.0);

          HistogramStat hist_naccess_per_key;
          for (auto const& key_access :
               block_access_info.second.key_num_access_map) {
            hist_naccess_per_key.Add(key_access.second);
          }
          uint64_t avg_accesses = hist_naccess_per_key.Average();
          uint64_t stdev_accesses = hist_naccess_per_key.StandardDeviation();
          avg_naccesses_per_key_in_a_data_block.Add(avg_accesses);
          cf_avg_naccesses_per_key_in_a_data_block[cf_name].Add(avg_accesses);
          stdev_naccesses_per_key_in_a_data_block.Add(stdev_accesses);
          cf_stdev_naccesses_per_key_in_a_data_block[cf_name].Add(
              stdev_accesses);

          existing_keys_stats.Add(percent_referenced_for_existing_keys);
          cf_existing_keys_stats_map[cf_name].Add(
              percent_referenced_for_existing_keys);
          non_existing_keys_stats.Add(percent_referenced_for_non_existing_keys);
          cf_non_existing_keys_stats_map[cf_name].Add(
              percent_referenced_for_non_existing_keys);
          block_access_stats.Add(percent_accesses_for_existing_keys);
          cf_block_access_info[cf_name].Add(percent_accesses_for_existing_keys);
        }
      }
    }
  }
  fprintf(stdout,
          "Histogram on the number of referenced keys existing in a block over "
          "the total number of keys in a block: \n%s",
          existing_keys_stats.ToString().c_str());
  for (auto const& cf_stats : cf_existing_keys_stats_map) {
    print_break_lines(/*num_break_lines=*/1);
    fprintf(stdout, "Break down by column family %s: \n%s",
            cf_stats.first.c_str(), cf_stats.second.ToString().c_str());
  }
  print_break_lines(/*num_break_lines=*/1);
  fprintf(
      stdout,
      "Histogram on the number of referenced keys DO NOT exist in a block over "
      "the total number of keys in a block: \n%s",
      non_existing_keys_stats.ToString().c_str());
  for (auto const& cf_stats : cf_non_existing_keys_stats_map) {
    print_break_lines(/*num_break_lines=*/1);
    fprintf(stdout, "Break down by column family %s: \n%s",
            cf_stats.first.c_str(), cf_stats.second.ToString().c_str());
  }
  print_break_lines(/*num_break_lines=*/1);
  fprintf(stdout,
          "Histogram on the number of accesses on keys exist in a block over "
          "the total number of accesses in a block: \n%s",
          block_access_stats.ToString().c_str());
  for (auto const& cf_stats : cf_block_access_info) {
    print_break_lines(/*num_break_lines=*/1);
    fprintf(stdout, "Break down by column family %s: \n%s",
            cf_stats.first.c_str(), cf_stats.second.ToString().c_str());
  }
  print_break_lines(/*num_break_lines=*/1);
  fprintf(
      stdout,
      "Histogram on the average number of accesses per key in a block: \n%s",
      avg_naccesses_per_key_in_a_data_block.ToString().c_str());
  for (auto const& cf_stats : cf_avg_naccesses_per_key_in_a_data_block) {
    fprintf(stdout, "Break down by column family %s: \n%s",
            cf_stats.first.c_str(), cf_stats.second.ToString().c_str());
  }
  print_break_lines(/*num_break_lines=*/1);
  fprintf(stdout,
          "Histogram on the standard deviation of the number of accesses per "
          "key in a block: \n%s",
          stdev_naccesses_per_key_in_a_data_block.ToString().c_str());
  for (auto const& cf_stats : cf_stdev_naccesses_per_key_in_a_data_block) {
    fprintf(stdout, "Break down by column family %s: \n%s",
            cf_stats.first.c_str(), cf_stats.second.ToString().c_str());
  }
}

void BlockCacheTraceAnalyzer::PrintStatsSummary() const {
  uint64_t total_num_files = 0;
  uint64_t total_num_blocks = 0;
  uint64_t total_num_accesses = 0;
  std::map<TraceType, uint64_t> bt_num_blocks_map;
  std::map<TableReaderCaller, uint64_t> caller_num_access_map;
  std::map<TableReaderCaller, std::map<TraceType, uint64_t>>
      caller_bt_num_access_map;
  std::map<TableReaderCaller, std::map<uint32_t, uint64_t>>
      caller_level_num_access_map;
  for (auto const& cf_aggregates : cf_aggregates_map_) {
    // Stats per column family.
    const std::string& cf_name = cf_aggregates.first;
    uint64_t cf_num_files = 0;
    uint64_t cf_num_blocks = 0;
    std::map<TraceType, uint64_t> cf_bt_blocks;
    uint64_t cf_num_accesses = 0;
    std::map<TableReaderCaller, uint64_t> cf_caller_num_accesses_map;
    std::map<TableReaderCaller, std::map<uint64_t, uint64_t>>
        cf_caller_level_num_accesses_map;
    std::map<TableReaderCaller, std::map<uint64_t, uint64_t>>
        cf_caller_file_num_accesses_map;
    std::map<TableReaderCaller, std::map<TraceType, uint64_t>>
        cf_caller_bt_num_accesses_map;
    total_num_files += cf_aggregates.second.fd_aggregates_map.size();
    for (auto const& file_aggregates : cf_aggregates.second.fd_aggregates_map) {
      // Stats per SST file.
      const uint64_t fd = file_aggregates.first;
      const uint32_t level = file_aggregates.second.level;
      cf_num_files++;
      for (auto const& block_type_aggregates :
           file_aggregates.second.block_type_aggregates_map) {
        // Stats per block type.
        const TraceType type = block_type_aggregates.first;
        cf_bt_blocks[type] +=
            block_type_aggregates.second.block_access_info_map.size();
        total_num_blocks +=
            block_type_aggregates.second.block_access_info_map.size();
        bt_num_blocks_map[type] +=
            block_type_aggregates.second.block_access_info_map.size();
        for (auto const& block_access_info :
             block_type_aggregates.second.block_access_info_map) {
          // Stats per block.
          cf_num_blocks++;
          for (auto const& stats :
               block_access_info.second.caller_num_access_map) {
            // Stats per caller.
            const TableReaderCaller caller = stats.first;
            const uint64_t num_accesses = stats.second;
            // Overall stats.
            total_num_accesses += num_accesses;
            caller_num_access_map[caller] += num_accesses;
            caller_bt_num_access_map[caller][type] += num_accesses;
            caller_level_num_access_map[caller][level] += num_accesses;
            // Column Family stats.
            cf_num_accesses += num_accesses;
            cf_caller_num_accesses_map[caller] += num_accesses;
            cf_caller_level_num_accesses_map[caller][level] += num_accesses;
            cf_caller_file_num_accesses_map[caller][fd] += num_accesses;
            cf_caller_bt_num_accesses_map[caller][type] += num_accesses;
          }
        }
      }
    }

    // Print stats.
    print_break_lines(/*num_break_lines=*/3);
    fprintf(stdout, "Statistics for column family %s:\n", cf_name.c_str());
    fprintf(stdout,
            " Number of files:%" PRIu64 " Number of blocks: %" PRIu64
            " Number of accesses: %" PRIu64 "\n",
            cf_num_files, cf_num_blocks, cf_num_accesses);
    for (auto block_type : cf_bt_blocks) {
      fprintf(stdout, "Number of %s blocks: %" PRIu64 " Percent: %.2f\n",
              block_type_to_string(block_type.first).c_str(), block_type.second,
              percent(block_type.second, cf_num_blocks));
    }
    for (auto caller : cf_caller_num_accesses_map) {
      const uint64_t naccesses = caller.second;
      print_break_lines(/*num_break_lines=*/1);
      fprintf(stdout,
              "Caller %s: Number of accesses %" PRIu64 " Percent: %.2f\n",
              caller_to_string(caller.first).c_str(), naccesses,
              percent(naccesses, cf_num_accesses));
      fprintf(stdout, "Caller %s: Number of accesses per level break down\n",
              caller_to_string(caller.first).c_str());
      for (auto naccess_level :
           cf_caller_level_num_accesses_map[caller.first]) {
        fprintf(stdout,
                "\t Level %" PRIu64 ": Number of accesses: %" PRIu64
                " Percent: %.2f\n",
                naccess_level.first, naccess_level.second,
                percent(naccess_level.second, naccesses));
      }
      fprintf(stdout, "Caller %s: Number of accesses per file break down\n",
              caller_to_string(caller.first).c_str());
      for (auto naccess_file : cf_caller_file_num_accesses_map[caller.first]) {
        fprintf(stdout,
                "\t File %" PRIu64 ": Number of accesses: %" PRIu64
                " Percent: %.2f\n",
                naccess_file.first, naccess_file.second,
                percent(naccess_file.second, naccesses));
      }
      fprintf(stdout,
              "Caller %s: Number of accesses per block type break down\n",
              caller_to_string(caller.first).c_str());
      for (auto naccess_type : cf_caller_bt_num_accesses_map[caller.first]) {
        fprintf(stdout,
                "\t Block Type %s: Number of accesses: %" PRIu64
                " Percent: %.2f\n",
                block_type_to_string(naccess_type.first).c_str(),
                naccess_type.second, percent(naccess_type.second, naccesses));
      }
    }
  }
  print_break_lines(/*num_break_lines=*/3);
  fprintf(stdout, "Overall statistics:\n");
  fprintf(stdout,
          "Number of files: %" PRIu64 " Number of blocks: %" PRIu64
          " Number of accesses: %" PRIu64 "\n",
          total_num_files, total_num_blocks, total_num_accesses);
  for (auto block_type : bt_num_blocks_map) {
    fprintf(stdout, "Number of %s blocks: %" PRIu64 " Percent: %.2f\n",
            block_type_to_string(block_type.first).c_str(), block_type.second,
            percent(block_type.second, total_num_blocks));
  }
  for (auto caller : caller_num_access_map) {
    print_break_lines(/*num_break_lines=*/1);
    uint64_t naccesses = caller.second;
    fprintf(stdout, "Caller %s: Number of accesses %" PRIu64 " Percent: %.2f\n",
            caller_to_string(caller.first).c_str(), naccesses,
            percent(naccesses, total_num_accesses));
    fprintf(stdout, "Caller %s: Number of accesses per level break down\n",
            caller_to_string(caller.first).c_str());
    for (auto naccess_level : caller_level_num_access_map[caller.first]) {
      fprintf(stdout,
              "\t Level %d: Number of accesses: %" PRIu64 " Percent: %.2f\n",
              naccess_level.first, naccess_level.second,
              percent(naccess_level.second, naccesses));
    }
    fprintf(stdout, "Caller %s: Number of accesses per block type break down\n",
            caller_to_string(caller.first).c_str());
    for (auto naccess_type : caller_bt_num_access_map[caller.first]) {
      fprintf(stdout,
              "\t Block Type %s: Number of accesses: %" PRIu64
              " Percent: %.2f\n",
              block_type_to_string(naccess_type.first).c_str(),
              naccess_type.second, percent(naccess_type.second, naccesses));
    }
  }
}

std::vector<CacheConfiguration> parse_cache_config_file(
    const std::string& config_path) {
  std::ifstream file(config_path);
  if (!file.is_open()) {
    return {};
  }
  std::vector<CacheConfiguration> configs;
  std::string line;
  while (getline(file, line)) {
    CacheConfiguration cache_config;
    std::stringstream ss(line);
    std::vector<std::string> config_strs;
    while (ss.good()) {
      std::string substr;
      getline(ss, substr, ',');
      config_strs.push_back(substr);
    }
    // Sanity checks.
    if (config_strs.size() < 4) {
      fprintf(stderr, "Invalid cache simulator configuration %s\n",
              line.c_str());
      exit(1);
    }
    if (kSupportedCacheNames.find(" " + config_strs[0] + " ") ==
        std::string::npos) {
      fprintf(stderr, "Invalid cache name %s. Supported cache names are %s\n",
              line.c_str(), kSupportedCacheNames.c_str());
      exit(1);
    }
    cache_config.cache_name = config_strs[0];
    cache_config.num_shard_bits = ParseUint32(config_strs[1]);
    cache_config.ghost_cache_capacity = ParseUint64(config_strs[2]);
    for (uint32_t i = 3; i < config_strs.size(); i++) {
      uint64_t capacity = ParseUint64(config_strs[i]);
      if (capacity == 0) {
        fprintf(stderr, "Invalid cache capacity %s, %s\n",
                config_strs[i].c_str(), line.c_str());
        exit(1);
      }
      cache_config.cache_capacities.push_back(capacity);
    }
    configs.push_back(cache_config);
  }
  file.close();
  return configs;
}

std::vector<uint64_t> parse_buckets(const std::string& bucket_str) {
  std::vector<uint64_t> buckets;
  std::stringstream ss(bucket_str);
  while (ss.good()) {
    std::string bucket;
    getline(ss, bucket, ',');
    buckets.push_back(ParseUint64(bucket));
  }
  buckets.push_back(port::kMaxUint64);
  return buckets;
}

int block_cache_trace_analyzer_tool(int argc, char** argv) {
  ParseCommandLineFlags(&argc, &argv, true);
  if (FLAGS_block_cache_trace_path.empty()) {
    fprintf(stderr, "block cache trace path is empty\n");
    exit(1);
  }
  uint64_t warmup_seconds =
      FLAGS_cache_sim_warmup_seconds > 0 ? FLAGS_cache_sim_warmup_seconds : 0;
  uint32_t downsample_ratio = FLAGS_block_cache_trace_downsample_ratio > 0
                                  ? FLAGS_block_cache_trace_downsample_ratio
                                  : 0;
  std::vector<CacheConfiguration> cache_configs =
      parse_cache_config_file(FLAGS_block_cache_sim_config_path);
  std::unique_ptr<BlockCacheTraceSimulator> cache_simulator;
  if (!cache_configs.empty()) {
    cache_simulator.reset(new BlockCacheTraceSimulator(
        warmup_seconds, downsample_ratio, cache_configs));
    Status s = cache_simulator->InitializeCaches();
    if (!s.ok()) {
      fprintf(stderr, "Cannot initialize cache simulators %s\n",
              s.ToString().c_str());
      exit(1);
    }
  }
  BlockCacheTraceAnalyzer analyzer(
      FLAGS_block_cache_trace_path, FLAGS_block_cache_analysis_result_dir,
      !FLAGS_reuse_distance_labels.empty(), std::move(cache_simulator));
  Status s = analyzer.Analyze();
  if (!s.IsIncomplete()) {
    // Read all traces.
    fprintf(stderr, "Cannot process the trace %s\n", s.ToString().c_str());
    exit(1);
  }

  analyzer.PrintStatsSummary();
  if (FLAGS_print_access_count_stats) {
    print_break_lines(/*num_break_lines=*/3);
    analyzer.PrintAccessCountStats(
        /*user_access_only=*/false, FLAGS_analyze_bottom_k_access_count_blocks,
        FLAGS_analyze_top_k_access_count_blocks);
    print_break_lines(/*num_break_lines=*/3);
    analyzer.PrintAccessCountStats(
        /*user_access_only=*/true, FLAGS_analyze_bottom_k_access_count_blocks,
        FLAGS_analyze_top_k_access_count_blocks);
  }
  if (FLAGS_print_block_size_stats) {
    print_break_lines(/*num_break_lines=*/3);
    analyzer.PrintBlockSizeStats();
  }
  if (FLAGS_print_data_block_access_count_stats) {
    print_break_lines(/*num_break_lines=*/3);
    analyzer.PrintDataBlockAccessStats();
  }
  print_break_lines(/*num_break_lines=*/3);
  analyzer.WriteMissRatioCurves();

  if (!FLAGS_timeline_labels.empty()) {
    std::stringstream ss(FLAGS_timeline_labels);
    while (ss.good()) {
      std::string label;
      getline(ss, label, ',');
      analyzer.WriteAccessTimeline(label);
    }
  }

  if (!FLAGS_analyze_callers.empty()) {
    analyzer.WritePercentAccessSummaryStats();
    std::stringstream ss(FLAGS_analyze_callers);
    while (ss.good()) {
      std::string caller;
      getline(ss, caller, ',');
      analyzer.WriteDetailedPercentAccessSummaryStats(string_to_caller(caller));
    }
  }

  if (!FLAGS_access_count_buckets.empty()) {
    std::vector<uint64_t> buckets = parse_buckets(FLAGS_access_count_buckets);
    analyzer.WriteAccessCountSummaryStats(buckets, /*user_access_only=*/true);
    analyzer.WriteAccessCountSummaryStats(buckets, /*user_access_only=*/false);
  }

  if (!FLAGS_reuse_distance_labels.empty() &&
      !FLAGS_reuse_distance_buckets.empty()) {
    std::vector<uint64_t> buckets = parse_buckets(FLAGS_reuse_distance_buckets);
    std::stringstream ss(FLAGS_reuse_distance_labels);
    while (ss.good()) {
      std::string label;
      getline(ss, label, ',');
      analyzer.WriteReuseDistance(label, buckets);
    }
  }

  if (!FLAGS_reuse_interval_labels.empty() &&
      !FLAGS_reuse_interval_buckets.empty()) {
    std::vector<uint64_t> buckets = parse_buckets(FLAGS_reuse_interval_buckets);
    std::stringstream ss(FLAGS_reuse_interval_labels);
    while (ss.good()) {
      std::string label;
      getline(ss, label, ',');
      analyzer.WriteReuseInterval(label, buckets);
    }
  }
  return 0;
}

}  // namespace rocksdb

#endif  // GFLAGS
#endif  // ROCKSDB_LITE
