// Copyright (c) 2011-present, Facebook, Inc. All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include <cstdlib>
#include <string>
#include <unordered_map>

#include "rocksdb/slice.h"
#include "table/block_suffix_index.h"
#include "util/testharness.h"
#include "util/testutil.h"

namespace rocksdb {

bool SearchForOffset(BlockSuffixIndex& index, Slice& key,
                     uint32_t& restart_point) {
  std::vector<uint32_t> bucket;
  index.Seek(key, bucket);
  for (auto& e : bucket) {
    if (e == restart_point) {
      return true;
    }
  }
  return false;
}

TEST(BlockTest, BlockSuffixTest) {
  // bucket_num = 200, #keys = 100. 50% utilization
  BlockSuffixIndexBuilder builder(200);

  for (uint32_t i = 0; i < 100; i++) {
    Slice key("key" + std::to_string(i));
    uint32_t restart_point = i;
    builder.Add(key, restart_point);
  }

  std::string buffer, buffer2;
  builder.Finish(buffer);
  buffer2 = buffer; // test for the correctness of relative offset

  BlockSuffixIndex index(buffer2);

  for (uint32_t i = 0; i < 100; i++) {
    Slice key("key" + std::to_string(i));
    uint32_t restart_point = i;
    ASSERT_TRUE(SearchForOffset(index, key, restart_point));
  }
}

TEST(BlockTest, BlockSuffixTestCollision) {
  // bucket_num = 2. There will be intense hash collisions
  BlockSuffixIndexBuilder builder(2);

  for (uint32_t i = 0; i < 100; i++) {
    Slice key("key" + std::to_string(i));
    uint32_t restart_point = i;
    builder.Add(key, restart_point);
  }

  std::string buffer, buffer2;
  builder.Finish(buffer);
  buffer2 = buffer; // test for the correctness of relative offset

  builder.Finish(buffer2);

  BlockSuffixIndex index(buffer);

  for (uint32_t i = 0; i < 100; i++) {
    Slice key("key" + std::to_string(i));
    uint32_t restart_point = i;
    ASSERT_TRUE(SearchForOffset(index, key, restart_point));
  }
}

TEST(BlockTest, BlockSuffixTestLarge) {
  BlockSuffixIndexBuilder builder(1000);
  std::unordered_map<std::string, uint32_t> m;

  for (uint32_t i = 0; i < 10000000; i++) {
    if (std::rand() % 2) continue;  // randomly leave half of the keys out
    std::string key_str = "key" + std::to_string(i);
    Slice key(key_str);
    uint32_t restart_point = i;
    builder.Add(key, restart_point);
    m[key_str] = restart_point;
  }

  std::string buffer, buffer2;
  builder.Finish(buffer);
  buffer2 = buffer; // test for the correctness of relative offset

  builder.Finish(buffer2);

  BlockSuffixIndex index(buffer);

  for (uint32_t i = 0; i < 100; i++) {
    std::string key_str = "key" + std::to_string(i);
    Slice key(key_str);
    uint32_t restart_point = i;
    if (m.count(key_str)) {
      ASSERT_TRUE(m[key_str] == restart_point);
      ASSERT_TRUE(SearchForOffset(index, key, restart_point));
    } else {
      // we allow false positve, so don't test the nonexisting keys.
      // when false positive happens, the search will continue to the
      // restart intervals to see if the key really exist.
    }
  }
}

}  // namespace rocksdb

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
