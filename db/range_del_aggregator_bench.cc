//  Copyright (c) 2018-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif
#ifndef GFLAGS
#include <cstdio>
int main() {
  fprintf(stderr, "Please install gflags to run rocksdb tools\n");
  return 1;
}
#else

#include <iostream>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "db/range_del_aggregator.h"
#include "rocksdb/comparator.h"
#include "rocksdb/env.h"
#include "util/coding.h"
#include "util/random.h"
#include "util/stop_watch.h"
#include "util/testutil.h"

#include "util/gflags_compat.h"

using GFLAGS_NAMESPACE::ParseCommandLineFlags;

DEFINE_int32(num_range_tombstones, 1000, "number of range tombstones created");

DEFINE_int32(num_runs, 10000, "number of test runs");

DEFINE_int32(tombstone_start_upper_bound, 1000,
             "exclusive upper bound on range tombstone start keys");

DEFINE_int32(should_delete_upper_bound, 1000,
             "exclusive upper bound on keys passed to ShouldDelete");

DEFINE_double(tombstone_width_mean, 100.0, "average range tombstone width");

DEFINE_double(tombstone_width_stddev, 0.0,
              "standard deviation of range tombstone width");

DEFINE_bool(use_collapsed, true, "use the collapsed range tombstone map");

DEFINE_int32(seed, 0, "random number generator seed");

struct Stats {
  uint64_t time_add_tombstones = 0;
  uint64_t time_should_delete = 0;
};

std::ostream& operator<<(std::ostream& os, const Stats& s) {
  os << "AddTombstones:\t\t" << s.time_add_tombstones / (FLAGS_num_runs * 1.0e3)
     << " us\n";
  os << "ShouldDelete:\t\t" << s.time_should_delete / (FLAGS_num_runs * 1.0e3)
     << " us\n";
  return os;
}

namespace rocksdb {

// A wrapper around RangeTombstones and the underlying data of its start and end
// keys.
struct PersistentRangeTombstone {
  std::string start_key;
  std::string end_key;
  RangeTombstone tombstone;

  PersistentRangeTombstone(std::string start, std::string end,
                           SequenceNumber seq)
      : start_key(start), end_key(end) {
    tombstone = RangeTombstone(start_key, end_key, seq);
  }

  PersistentRangeTombstone() = default;

  PersistentRangeTombstone(const PersistentRangeTombstone& t) { *this = t; }

  PersistentRangeTombstone& operator=(const PersistentRangeTombstone& t) {
    start_key = t.start_key;
    end_key = t.end_key;
    tombstone = RangeTombstone(start_key, end_key, t.tombstone.seq_);

    return *this;
  }

  PersistentRangeTombstone(PersistentRangeTombstone&& t) { *this = t; }

  PersistentRangeTombstone& operator=(PersistentRangeTombstone&& t) {
    start_key = std::move(t.start_key);
    end_key = std::move(t.end_key);
    tombstone = RangeTombstone(start_key, end_key, t.tombstone.seq_);

    return *this;
  }
};

struct TombstoneStartKeyComparator {
  TombstoneStartKeyComparator(const Comparator* c) : cmp(c) {}

  bool operator()(const RangeTombstone& a, const RangeTombstone& b) const {
    return cmp->Compare(a.start_key_, b.start_key_) < 0;
  }

  const Comparator* cmp;
};

void AddTombstones(RangeDelAggregator* range_del_agg,
                   const std::vector<PersistentRangeTombstone>& range_dels) {
  std::vector<std::string> keys, values;
  for (const auto& range_del : range_dels) {
    auto key_and_value = range_del.tombstone.Serialize();
    keys.push_back(key_and_value.first.Encode().ToString());
    values.push_back(key_and_value.second.ToString());
  }
  std::unique_ptr<test::VectorIterator> range_del_iter(
      new test::VectorIterator(keys, values));
  range_del_agg->AddTombstones(std::move(range_del_iter));
}

// convert long to a big-endian slice key
static std::string Key(int64_t val) {
  std::string little_endian_key;
  std::string big_endian_key;
  PutFixed64(&little_endian_key, val);
  assert(little_endian_key.size() == sizeof(val));
  big_endian_key.resize(sizeof(val));
  for (size_t i = 0; i < sizeof(val); ++i) {
    big_endian_key[i] = little_endian_key[sizeof(val) - 1 - i];
  }
  return big_endian_key;
}

}  // namespace rocksdb

int main(int argc, char** argv) {
  ParseCommandLineFlags(&argc, &argv, true);

  Stats stats;
  rocksdb::Random64 rnd(FLAGS_seed);
  std::default_random_engine random_gen(FLAGS_seed);
  std::normal_distribution<double> normal_dist(FLAGS_tombstone_width_mean,
                                               FLAGS_tombstone_width_stddev);
  std::vector<rocksdb::PersistentRangeTombstone> persistent_range_tombstones(
      FLAGS_num_range_tombstones);
  auto mode =
      FLAGS_use_collapsed
          ? rocksdb::RangeDelPositioningMode::kForwardTraversal
          : rocksdb::RangeDelPositioningMode::kFullScan;

  for (int i = 0; i < FLAGS_num_runs; i++) {
    auto icmp = rocksdb::InternalKeyComparator(rocksdb::BytewiseComparator());
    rocksdb::RangeDelAggregator range_del_agg(icmp, {} /* snapshots */,
                                              FLAGS_use_collapsed);

    for (int j = 0; j < FLAGS_num_range_tombstones; j++) {
      uint64_t start = rnd.Uniform(FLAGS_tombstone_start_upper_bound);
      uint64_t end = start + std::max(1.0, normal_dist(random_gen));
      persistent_range_tombstones[j] = rocksdb::PersistentRangeTombstone(
          rocksdb::Key(start), rocksdb::Key(end), j);
    }

    rocksdb::StopWatchNano stop_watch_add_tombstones(rocksdb::Env::Default(),
                                                     true /* auto_start */);
    rocksdb::AddTombstones(&range_del_agg, persistent_range_tombstones);
    stats.time_add_tombstones += stop_watch_add_tombstones.ElapsedNanos();

    uint64_t key = rnd.Uniform(FLAGS_should_delete_upper_bound);
    rocksdb::ParsedInternalKey parsed_key;
    std::string key_string = rocksdb::Key(key);
    parsed_key.user_key = key_string;
    parsed_key.sequence = FLAGS_num_range_tombstones / 2;
    parsed_key.type = rocksdb::kTypeValue;

    rocksdb::StopWatchNano stop_watch_should_delete(rocksdb::Env::Default(),
                                                    true /* auto_start */);
    range_del_agg.ShouldDelete(parsed_key, mode);
    stats.time_should_delete += stop_watch_should_delete.ElapsedNanos();
  }

  std::cout << "=======================\n"
            << "Results:\n"
            << "=======================\n"
            << stats;

  return 0;
}

#endif  // GFLAGS
