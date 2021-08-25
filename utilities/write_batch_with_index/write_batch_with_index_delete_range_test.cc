//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef ROCKSDB_LITE

#include <map>
#include <memory>

#include "db/column_family.h"
#include "memtable/skiplist.h"
#include "port/stack_trace.h"
#include "rocksdb/utilities/write_batch_with_index.h"
#include "test_util/testharness.h"
#include "util/random.h"
#include "util/string_util.h"
#include "write_batch_with_index_test.h"

namespace ROCKSDB_NAMESPACE {

class WriteBatchWithIndexDeleteRangeTest : public WBWIOverwriteTest {
 public:
  WriteBatchWithIndexDeleteRangeTest() : WBWIOverwriteTest() {}

  std::unique_ptr<ColumnFamilyHandle> makeCF(std::string familyName) {
    ColumnFamilyHandle* cf;
    EXPECT_OK(db_->CreateColumnFamily(ColumnFamilyOptions(), familyName, &cf));
    return std::unique_ptr<ColumnFamilyHandle>(cf);
  }
};

static std::vector<std::string> GetValuesFromBatch(
    WriteBatchWithIndex* batch, ColumnFamilyHandle* column_family,
    std::vector<std::string> keys) {
  DBOptions db_options;
  std::vector<std::string> result;
  for (std::string key : keys) {
    std::string value;
    Status s = batch->GetFromBatch(column_family, db_options, key, &value);
    if (s.IsNotFound()) {
      result.push_back("" + key + "={}");
    } else {
      result.push_back("" + key + "=" + value);
    }
  }
  return result;
};

static std::vector<std::string> GetValuesFromBatch(
    WriteBatchWithIndex* batch, std::vector<std::string> keys) {
  return GetValuesFromBatch(batch, nullptr, keys);
};

void AssertKey(std::string key, WBWIIterator* iter) {
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(key, iter->Entry().key.ToString());
}

void AssertValue(std::string value, WBWIIterator* iter) {
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(value, iter->Entry().value.ToString());
}

/**
 * Test that DeleteRange is unsupported
 * when WBWI overwrite_key is false.
 */
TEST_F(WriteBatchWithIndexDeleteRangeTest,
       DeleteRangeTestBatchOverWriteKeyIsFalseUnsupportedOption) {
  const bool overwrite_key = false;
  batch_.reset(
      new WriteBatchWithIndex(BytewiseComparator(), 20, overwrite_key));

  Status s = batch_->DeleteRange("B", "C");
  ASSERT_TRUE(s.IsNotSupported());
}

/**
 * Test that DeleteRange on Column Family is unsupported
 * when WBWI overwrite_key is false.
 */
TEST_F(WriteBatchWithIndexDeleteRangeTest,
       DeleteRangeCFTestBatchOverWriteKeyIsFalseUnsupportedOption) {
  ColumnFamilyHandleImplDummy cf1(6, BytewiseComparator());

  const bool overwrite_key = false;
  batch_.reset(
      new WriteBatchWithIndex(BytewiseComparator(), 20, overwrite_key));

  Status s = batch_->DeleteRange(&cf1, "B", "C");
  ASSERT_TRUE(s.IsNotSupported());
}

/**
 * Test that DeleteRange returns Status::Code::kInvalidArgument
 * for invalid ranges, but otherwise functions correctly.
 */
TEST_F(WriteBatchWithIndexDeleteRangeTest, BatchBadRange) {
  const bool overwrite_key = true;
  batch_.reset(new WriteBatchWithIndex(BytewiseComparator(), 0, overwrite_key));
  Status s;
  std::string value;

  ASSERT_OK(OpenDB());

  s = batch_->Put("EE", "ee");
  ASSERT_OK(s);
  s = batch_->Put("G", "g");
  ASSERT_OK(s);

  // Test that D..C is invalid, as D should come after C!
  s = batch_->DeleteRange("D", "C");
  ASSERT_TRUE(s.IsInvalidArgument());

  // Test that E..E is invalid, as ..E is exclusive!
  s = batch_->DeleteRange("E", "E");
  ASSERT_TRUE(s.IsInvalidArgument());
  s = batch_->GetFromBatch(options_, "EE", &value);
  ASSERT_OK(s);
  ASSERT_EQ("ee", value);

  // Test that DeleteRange is still functional
  s = batch_->DeleteRange("E", "EEEE");
  ASSERT_OK(s);
  s = batch_->GetFromBatch(options_, "EE", &value);
  ASSERT_NOT_FOUND(s);

  // Test that writing the batch to the db,
  // writes only those that are not in DeleteRange
  value.clear();
  s = db_->Write(write_opts_, batch_->GetWriteBatch());
  ASSERT_OK(s);

  s = db_->Get(read_opts_, "EE", &value);
  ASSERT_NOT_FOUND(s);

  s = db_->Get(read_opts_, "G", &value);
  ASSERT_OK(s);
  ASSERT_EQ("g", value);
}

/**
 * Test that DeleteRange on Column Family returns
 * Status::Code::kInvalidArgument for invalid ranges,
 * but otherwise functions correctly.
 */
TEST_F(WriteBatchWithIndexDeleteRangeTest, BatchBadRangeCF) {
  ASSERT_OK(OpenDB());
  auto cf1 = makeCF("First Family");

  const bool overwrite_key = true;
  batch_.reset(new WriteBatchWithIndex(BytewiseComparator(), 0, overwrite_key));
  Status s;
  std::string value;

  s = batch_->Put(cf1.get(), "EE", "ee");
  ASSERT_OK(s);
  s = batch_->Put(cf1.get(), "G", "g");
  ASSERT_OK(s);

  // Test that D..C is invalid, as D should come after C!
  s = batch_->DeleteRange(cf1.get(), "D", "C");
  ASSERT_TRUE(s.IsInvalidArgument());

  // Test that E..E is invalid, as ..E is exclusive!
  s = batch_->DeleteRange(cf1.get(), "E", "E");
  ASSERT_TRUE(s.IsInvalidArgument());
  s = batch_->GetFromBatch(cf1.get(), options_, "EE", &value);
  ASSERT_OK(s);
  ASSERT_EQ("ee", value);

  // Test that DeleteRange is still functional
  s = batch_->DeleteRange(cf1.get(), "E", "EEEE");
  ASSERT_OK(s);
  s = batch_->GetFromBatch(cf1.get(), options_, "EE", &value);
  ASSERT_NOT_FOUND(s);

  // Test that writing the batch to the db,
  // writes only those that are not in DeleteRange
  value = "";
  s = db_->Write(write_opts_, batch_->GetWriteBatch());
  ASSERT_OK(s);

  s = db_->Get(read_opts_, cf1.get(), "EE", &value);
  ASSERT_NOT_FOUND(s);

  s = db_->Get(read_opts_, cf1.get(), "G", &value);
  ASSERT_OK(s);
  ASSERT_EQ("g", value);
}

/**
 * Tests a single DeleteRange in the middle of
 * some existing keys, and makes sure only those
 * outside of the range are still accessible.
 */
TEST_F(WriteBatchWithIndexDeleteRangeTest, DeleteSingleRange) {
  const bool overwrite_key = true;
  batch_.reset(new WriteBatchWithIndex(BytewiseComparator(), 0, overwrite_key));
  Status s;
  std::string value;

  // Delete range with nothing in the range is OK,
  ASSERT_OK(batch_->DeleteRange("B", "C"));

  // Read a bunch of values, ensure it's all not there OK
  s = batch_->GetFromBatch(options_, "A", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(options_, "B", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(options_, "C", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(options_, "D", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(options_, "E", &value);
  ASSERT_NOT_FOUND(s);

  // Simple range deletion in centre of A-E
  batch_->Clear();
  ASSERT_OK(batch_->Put("A", "a"));
  ASSERT_OK(batch_->Put("B", "b"));
  ASSERT_OK(batch_->Put("C", "c"));
  ASSERT_OK(batch_->Put("D", "d"));
  ASSERT_OK(batch_->Put("E", "e"));

  ASSERT_OK(batch_->DeleteRange("B", "D"));

  s = batch_->GetFromBatch(options_, "A", &value);
  ASSERT_OK(s);
  ASSERT_EQ("a", value);
  s = batch_->GetFromBatch(options_, "B", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(options_, "C", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(options_, "D", &value);
  ASSERT_OK(s);
  ASSERT_EQ("d", value);
  s = batch_->GetFromBatch(options_, "E", &value);
  ASSERT_OK(s);
  ASSERT_EQ("e", value);
  s = batch_->GetFromBatch(options_, "F", &value);
  ASSERT_NOT_FOUND(s);
}

/**
 * Tests a single DeleteRange on a Column Family
 * in the middle of some existing keys, and makes
 * sure only those outside of the range are still accessible.
 */
TEST_F(WriteBatchWithIndexDeleteRangeTest, DeleteSingleRangeCF) {
  ASSERT_OK(OpenDB());
  auto cf1 = makeCF("First Family");

  const bool overwrite_key = true;
  batch_.reset(new WriteBatchWithIndex(BytewiseComparator(), 0, overwrite_key));
  Status s;
  std::string value;

  // Delete range with nothing in the range is OK,
  ASSERT_OK(batch_->DeleteRange(cf1.get(), "B", "C"));

  // Read a bunch of values, ensure it's all not there OK
  s = batch_->GetFromBatch(cf1.get(), options_, "A", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(cf1.get(), options_, "B", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(cf1.get(), options_, "C", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(cf1.get(), options_, "D", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(cf1.get(), options_, "E", &value);
  ASSERT_NOT_FOUND(s);

  // Simple range deletion in centre of A-E
  batch_->Clear();
  ASSERT_OK(batch_->Put(cf1.get(), "A", "a"));
  ASSERT_OK(batch_->Put(cf1.get(), "B", "b"));
  ASSERT_OK(batch_->Put(cf1.get(), "C", "c"));
  ASSERT_OK(batch_->Put(cf1.get(), "D", "d"));
  ASSERT_OK(batch_->Put(cf1.get(), "E", "e"));

  ASSERT_OK(batch_->DeleteRange(cf1.get(), "B", "D"));

  s = batch_->GetFromBatch(cf1.get(), options_, "A", &value);
  ASSERT_OK(s);
  ASSERT_EQ("a", value);
  s = batch_->GetFromBatch(cf1.get(), options_, "B", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(cf1.get(), options_, "C", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(cf1.get(), options_, "D", &value);
  ASSERT_OK(s);
  ASSERT_EQ("d", value);
  s = batch_->GetFromBatch(cf1.get(), options_, "E", &value);
  ASSERT_OK(s);
  ASSERT_EQ("e", value);
  s = batch_->GetFromBatch(cf1.get(), options_, "F", &value);
  ASSERT_NOT_FOUND(s);
}

/**
 * Tests putting a key into a WBWI, deleting it with Delete Range,
 * and then putting it again.
 */
TEST_F(WriteBatchWithIndexDeleteRangeTest, PutDeleteRangePutAgain) {
  const bool overwrite_key = true;
  batch_.reset(new WriteBatchWithIndex(BytewiseComparator(), 0, overwrite_key));
  Status s;
  std::string value;

  // Put C, and check it exists
  ASSERT_OK(batch_->Put("C", "c0"));
  s = batch_->GetFromBatch(options_, "C", &value);
  ASSERT_OK(s);
  ASSERT_EQ("c0", value);

  // Delete B..D (i.e. C), and make sure C does not exist
  ASSERT_OK(batch_->DeleteRange("B", "D"));
  s = batch_->GetFromBatch(options_, "C", &value);
  ASSERT_NOT_FOUND(s);

  // Put C again, and check it exists
  ASSERT_OK(batch_->Put("C", "c1"));
  s = batch_->GetFromBatch(options_, "C", &value);
  ASSERT_OK(s);
  ASSERT_EQ("c1", value);
}

/**
 * Tests putting a key in a Column Family into a WBWI,
 * deleting it with Delete Range, and then putting it again.
 */
TEST_F(WriteBatchWithIndexDeleteRangeTest, PutDeleteRangePutAgainCF) {
  ASSERT_OK(OpenDB());
  auto cf1 = makeCF("First Family");

  const bool overwrite_key = true;
  batch_.reset(new WriteBatchWithIndex(BytewiseComparator(), 0, overwrite_key));

  Status s;
  std::string value;

  // Put C, and check it exists
  ASSERT_OK(batch_->Put(cf1.get(), "C", "c0"));
  s = batch_->GetFromBatch(cf1.get(), options_, "C", &value);
  ASSERT_OK(s);
  ASSERT_EQ("c0", value);

  // Delete B..D (i.e. C), and make sure C does not exist
  ASSERT_OK(batch_->DeleteRange(cf1.get(), "B", "D"));
  s = batch_->GetFromBatch(cf1.get(), options_, "C", &value);
  ASSERT_NOT_FOUND(s);

  // Put C again, and check it exists
  ASSERT_OK(batch_->Put(cf1.get(), "C", "c1"));
  s = batch_->GetFromBatch(cf1.get(), options_, "C", &value);
  ASSERT_OK(s);
  ASSERT_EQ("c1", value);
}

/**
 * Tests Delete Range followed by Delete
 */
TEST_F(WriteBatchWithIndexDeleteRangeTest, DeleteRangeThenDelete) {
  const bool overwrite_key = true;
  batch_.reset(new WriteBatchWithIndex(BytewiseComparator(), 0, overwrite_key));
  Status s;
  std::string value;

  std::string contents;

  // Put C, Delete A..M (i.e. C)
  ASSERT_OK(batch_->Put("C", "c0"));
  s = batch_->GetFromBatch(options_, "C", &value);
  ASSERT_OK(s);
  ASSERT_EQ("c0", value);
  contents = PrintContents(batch_.get(), nullptr);
  ASSERT_GT(std::string::npos, contents.find("PUT(C):c0"));
  ASSERT_OK(batch_->DeleteRange("A", "M"));
  contents = PrintContents(batch_.get(), nullptr);
  ASSERT_EQ(std::string::npos, contents.find("PUT(C):c0"));
  s = batch_->GetFromBatch(options_, "C", &value);
  ASSERT_NOT_FOUND(s);

  // Put E
  ASSERT_OK(batch_->Put("E", "e0"));
  contents = PrintContents(batch_.get(), nullptr);
  ASSERT_GT(std::string::npos, contents.find("PUT(E):e0"));
  s = batch_->GetFromBatch(options_, "E", &value);
  ASSERT_OK(s);
  ASSERT_EQ("e0", value);

  // Delete C
  ASSERT_OK(batch_->Delete("C"));
  s = batch_->GetFromBatch(options_, "C", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(options_, "E", &value);
  ASSERT_OK(s);
  ASSERT_EQ("e0", value);

  // Delete E
  ASSERT_OK(batch_->Delete("E"));
  s = batch_->GetFromBatch(options_, "C", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(options_, "E", &value);
  ASSERT_NOT_FOUND(s);

  // Put E again
  ASSERT_OK(batch_->Put("E", "e1"));
  s = batch_->GetFromBatch(options_, "C", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(options_, "E", &value);
  ASSERT_OK(s);
  ASSERT_EQ("e1", value);

  // Put C again
  ASSERT_OK(batch_->Put("C", "c1"));
  s = batch_->GetFromBatch(options_, "C", &value);
  ASSERT_OK(s);
  ASSERT_EQ("c1", value);
  s = batch_->GetFromBatch(options_, "E", &value);
  ASSERT_OK(s);
  ASSERT_EQ("e1", value);
}

/**
 * Tests Delete Range followed by Delete
 * on a Column Family.
 */
TEST_F(WriteBatchWithIndexDeleteRangeTest, DeleteRangeThenDeleteCF) {
  ASSERT_OK(OpenDB());
  auto cf1 = makeCF("First Family");

  const bool overwrite_key = true;
  batch_.reset(new WriteBatchWithIndex(BytewiseComparator(), 0, overwrite_key));
  Status s;
  std::string value;

  std::string contents;

  // Put C, Delete A..M (i.e. C)
  ASSERT_OK(batch_->Put(cf1.get(), "C", "c0"));
  s = batch_->GetFromBatch(cf1.get(), options_, "C", &value);
  ASSERT_OK(s);
  ASSERT_EQ("c0", value);
  contents = PrintContents(batch_.get(), cf1.get());
  ASSERT_GT(std::string::npos, contents.find("PUT(C):c0"));
  ASSERT_OK(batch_->DeleteRange(cf1.get(), "A", "M"));
  contents = PrintContents(batch_.get(), cf1.get());
  ASSERT_EQ(std::string::npos, contents.find("PUT(C):c0"));
  s = batch_->GetFromBatch(cf1.get(), options_, "C", &value);
  ASSERT_NOT_FOUND(s);

  // Put E
  ASSERT_OK(batch_->Put(cf1.get(), "E", "e0"));
  s = batch_->GetFromBatch(cf1.get(), options_, "E", &value);
  ASSERT_OK(s);
  ASSERT_EQ("e0", value);

  // Delete C
  ASSERT_OK(batch_->Delete(cf1.get(), "C"));
  s = batch_->GetFromBatch(cf1.get(), options_, "C", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(cf1.get(), options_, "E", &value);
  ASSERT_OK(s);
  ASSERT_EQ("e0", value);

  // Delete E
  ASSERT_OK(batch_->Delete(cf1.get(), "E"));
  s = batch_->GetFromBatch(cf1.get(), options_, "C", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(cf1.get(), options_, "E", &value);
  ASSERT_NOT_FOUND(s);

  // Put E again
  ASSERT_OK(batch_->Put(cf1.get(), "E", "e1"));
  s = batch_->GetFromBatch(cf1.get(), options_, "C", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(cf1.get(), options_, "E", &value);
  ASSERT_OK(s);
  ASSERT_EQ("e1", value);

  // Put C again
  ASSERT_OK(batch_->Put(cf1.get(), "C", "c1"));
  s = batch_->GetFromBatch(cf1.get(), options_, "C", &value);
  ASSERT_OK(s);
  ASSERT_EQ("c1", value);
  s = batch_->GetFromBatch(cf1.get(), options_, "E", &value);
  ASSERT_OK(s);
  ASSERT_EQ("e1", value);
}

/**
 * Tests Delete followed by Delete Range
 */
TEST_F(WriteBatchWithIndexDeleteRangeTest, DeleteThenDeleteRange) {
  const bool overwrite_key = true;
  batch_.reset(new WriteBatchWithIndex(BytewiseComparator(), 0, overwrite_key));
  Status s;
  std::string value;

  // Put A, B, C, D
  ASSERT_OK(batch_->Put("A", "a0"));
  ASSERT_OK(batch_->Put("B", "b0"));
  ASSERT_OK(batch_->Put("C", "c0"));
  ASSERT_OK(batch_->Put("D", "d0"));
  s = batch_->GetFromBatch(options_, "A", &value);
  ASSERT_OK(s);
  ASSERT_EQ("a0", value);
  s = batch_->GetFromBatch(options_, "B", &value);
  ASSERT_OK(s);
  ASSERT_EQ("b0", value);
  s = batch_->GetFromBatch(options_, "C", &value);
  ASSERT_OK(s);
  ASSERT_EQ("c0", value);
  s = batch_->GetFromBatch(options_, "D", &value);
  ASSERT_OK(s);
  ASSERT_EQ("d0", value);

  // Delete B and C
  ASSERT_OK(batch_->Delete("B"));
  ASSERT_OK(batch_->Delete("C"));
  s = batch_->GetFromBatch(options_, "B", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(options_, "C", &value);
  ASSERT_NOT_FOUND(s);

  // Delete Range C..E
  ASSERT_OK(batch_->DeleteRange("C", "E"));
  s = batch_->GetFromBatch(options_, "C", &value);
  ASSERT_NOT_FOUND(s);

  // Check only A exists
  s = batch_->GetFromBatch(options_, "A", &value);
  ASSERT_OK(s);
  ASSERT_EQ("a0", value);
  s = batch_->GetFromBatch(options_, "B", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(options_, "C", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(options_, "D", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(options_, "E", &value);
  ASSERT_NOT_FOUND(s);

  // Put C again
  ASSERT_OK(batch_->Put("C", "c1"));

  // Check only A and C exist
  s = batch_->GetFromBatch(options_, "A", &value);
  ASSERT_OK(s);
  ASSERT_EQ("a0", value);
  s = batch_->GetFromBatch(options_, "B", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(options_, "C", &value);
  ASSERT_OK(s);
  ASSERT_EQ("c1", value);
  s = batch_->GetFromBatch(options_, "D", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(options_, "E", &value);
  ASSERT_NOT_FOUND(s);

  // Put B again
  ASSERT_OK(batch_->Put("B", "b1"));

  // Check only A, B and C exist
  s = batch_->GetFromBatch(options_, "A", &value);
  ASSERT_OK(s);
  ASSERT_EQ("a0", value);
  s = batch_->GetFromBatch(options_, "B", &value);
  ASSERT_OK(s);
  ASSERT_EQ("b1", value);
  s = batch_->GetFromBatch(options_, "C", &value);
  ASSERT_OK(s);
  ASSERT_EQ("c1", value);
  s = batch_->GetFromBatch(options_, "D", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(options_, "E", &value);
  ASSERT_NOT_FOUND(s);
}

/**
 * Tests Delete followed by Delete Range
 * on a Column Family.
 */
TEST_F(WriteBatchWithIndexDeleteRangeTest, DeleteThenDeleteRangeCF) {
  ASSERT_OK(OpenDB());
  auto cf1 = makeCF("First Family");

  const bool overwrite_key = true;
  batch_.reset(new WriteBatchWithIndex(BytewiseComparator(), 0, overwrite_key));
  Status s;
  std::string value;

  // Put A, B, C, D
  ASSERT_OK(batch_->Put(cf1.get(), "A", "a0"));
  ASSERT_OK(batch_->Put(cf1.get(), "B", "b0"));
  ASSERT_OK(batch_->Put(cf1.get(), "C", "c0"));
  ASSERT_OK(batch_->Put(cf1.get(), "D", "d0"));
  s = batch_->GetFromBatch(cf1.get(), options_, "A", &value);
  ASSERT_OK(s);
  ASSERT_EQ("a0", value);
  s = batch_->GetFromBatch(cf1.get(), options_, "B", &value);
  ASSERT_OK(s);
  ASSERT_EQ("b0", value);
  s = batch_->GetFromBatch(cf1.get(), options_, "C", &value);
  ASSERT_OK(s);
  ASSERT_EQ("c0", value);
  s = batch_->GetFromBatch(cf1.get(), options_, "D", &value);
  ASSERT_OK(s);
  ASSERT_EQ("d0", value);

  // Delete B and C
  ASSERT_OK(batch_->Delete(cf1.get(), "B"));
  ASSERT_OK(batch_->Delete(cf1.get(), "C"));
  s = batch_->GetFromBatch(cf1.get(), options_, "B", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(cf1.get(), options_, "C", &value);
  ASSERT_NOT_FOUND(s);

  // Delete Range C..E
  ASSERT_OK(batch_->DeleteRange(cf1.get(), "C", "E"));
  s = batch_->GetFromBatch(cf1.get(), options_, "C", &value);
  ASSERT_NOT_FOUND(s);

  // Check only A exists
  s = batch_->GetFromBatch(cf1.get(), options_, "A", &value);
  ASSERT_OK(s);
  ASSERT_EQ("a0", value);
  s = batch_->GetFromBatch(cf1.get(), options_, "B", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(cf1.get(), options_, "C", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(cf1.get(), options_, "D", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(cf1.get(), options_, "E", &value);
  ASSERT_NOT_FOUND(s);

  // Put C again
  ASSERT_OK(batch_->Put(cf1.get(), "C", "c1"));

  // Check only A and C exist
  s = batch_->GetFromBatch(cf1.get(), options_, "A", &value);
  ASSERT_OK(s);
  ASSERT_EQ("a0", value);
  s = batch_->GetFromBatch(cf1.get(), options_, "B", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(cf1.get(), options_, "C", &value);
  ASSERT_OK(s);
  ASSERT_EQ("c1", value);
  s = batch_->GetFromBatch(cf1.get(), options_, "D", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(cf1.get(), options_, "E", &value);
  ASSERT_NOT_FOUND(s);

  // Put B again
  ASSERT_OK(batch_->Put(cf1.get(), "B", "b1"));

  // Check only A, B and C exist
  s = batch_->GetFromBatch(cf1.get(), options_, "A", &value);
  ASSERT_OK(s);
  ASSERT_EQ("a0", value);
  s = batch_->GetFromBatch(cf1.get(), options_, "B", &value);
  ASSERT_OK(s);
  ASSERT_EQ("b1", value);
  s = batch_->GetFromBatch(cf1.get(), options_, "C", &value);
  ASSERT_OK(s);
  ASSERT_EQ("c1", value);
  s = batch_->GetFromBatch(cf1.get(), options_, "D", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(cf1.get(), options_, "E", &value);
  ASSERT_NOT_FOUND(s);
}

/**
 * Tests Delete Range followed by Single Delete
 */
TEST_F(WriteBatchWithIndexDeleteRangeTest, DeleteRangeThenSingleDelete) {
  const bool overwrite_key = true;
  batch_.reset(new WriteBatchWithIndex(BytewiseComparator(), 0, overwrite_key));
  Status s;
  std::string value;
  std::string contents;

  // Put C, Delete A..M (i.e. C)
  ASSERT_OK(batch_->Put("C", "c0"));
  s = batch_->GetFromBatch(options_, "C", &value);
  ASSERT_OK(s);
  ASSERT_EQ("c0", value);
  contents = PrintContents(batch_.get(), nullptr);
  ASSERT_GT(std::string::npos, contents.find("PUT(C):c0"));
  ASSERT_OK(batch_->DeleteRange("A", "M"));
  contents = PrintContents(batch_.get(), nullptr);
  ASSERT_EQ(std::string::npos, contents.find("PUT(C):c0"));
  s = batch_->GetFromBatch(options_, "C", &value);
  ASSERT_NOT_FOUND(s);

  // Put E
  ASSERT_OK(batch_->Put("E", "e0"));
  contents = PrintContents(batch_.get(), nullptr);
  ASSERT_GT(std::string::npos, contents.find("PUT(E):e0"));
  s = batch_->GetFromBatch(options_, "E", &value);
  ASSERT_OK(s);
  ASSERT_EQ("e0", value);

  // Single Delete C
  ASSERT_OK(batch_->SingleDelete("C"));
  s = batch_->GetFromBatch(options_, "C", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(options_, "E", &value);
  ASSERT_OK(s);
  ASSERT_EQ("e0", value);

  // Single Delete E
  ASSERT_OK(batch_->SingleDelete("E"));
  s = batch_->GetFromBatch(options_, "C", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(options_, "E", &value);
  ASSERT_NOT_FOUND(s);

  // Put E again
  ASSERT_OK(batch_->Put("E", "e1"));
  s = batch_->GetFromBatch(options_, "C", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(options_, "E", &value);
  ASSERT_OK(s);
  ASSERT_EQ("e1", value);

  // Put C again
  ASSERT_OK(batch_->Put("C", "c1"));
  s = batch_->GetFromBatch(options_, "C", &value);
  ASSERT_OK(s);
  ASSERT_EQ("c1", value);
  s = batch_->GetFromBatch(options_, "E", &value);
  ASSERT_OK(s);
  ASSERT_EQ("e1", value);
}

/**
 * Tests Delete Range followed by Single Delete
 * on a Column Family.
 */
TEST_F(WriteBatchWithIndexDeleteRangeTest, DeleteRangeThenSingleDeleteCF) {
  ASSERT_OK(OpenDB());
  auto cf1 = makeCF("First Family");

  const bool overwrite_key = true;
  batch_.reset(new WriteBatchWithIndex(BytewiseComparator(), 0, overwrite_key));
  Status s;
  std::string value;
  std::string contents;

  // Put C, Delete A..M (i.e. C)
  ASSERT_OK(batch_->Put(cf1.get(), "C", "c0"));
  s = batch_->GetFromBatch(cf1.get(), options_, "C", &value);
  ASSERT_OK(s);
  ASSERT_EQ("c0", value);
  contents = PrintContents(batch_.get(), cf1.get());
  ASSERT_GT(std::string::npos, contents.find("PUT(C):c0"));
  ASSERT_OK(batch_->DeleteRange(cf1.get(), "A", "M"));
  contents = PrintContents(batch_.get(), cf1.get());
  ASSERT_EQ(std::string::npos, contents.find("PUT(C):c0"));
  s = batch_->GetFromBatch(cf1.get(), options_, "C", &value);
  ASSERT_NOT_FOUND(s);

  // Put E
  ASSERT_OK(batch_->Put(cf1.get(), "E", "e0"));
  s = batch_->GetFromBatch(cf1.get(), options_, "E", &value);
  ASSERT_OK(s);
  ASSERT_EQ("e0", value);

  // Single Delete C
  ASSERT_OK(batch_->SingleDelete(cf1.get(), "C"));
  s = batch_->GetFromBatch(cf1.get(), options_, "C", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(cf1.get(), options_, "E", &value);
  ASSERT_OK(s);
  ASSERT_EQ("e0", value);

  // Single Delete E
  ASSERT_OK(batch_->SingleDelete(cf1.get(), "E"));
  s = batch_->GetFromBatch(cf1.get(), options_, "C", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(cf1.get(), options_, "E", &value);
  ASSERT_NOT_FOUND(s);

  // Put E again
  ASSERT_OK(batch_->Put(cf1.get(), "E", "e1"));
  s = batch_->GetFromBatch(cf1.get(), options_, "C", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(cf1.get(), options_, "E", &value);
  ASSERT_OK(s);
  ASSERT_EQ("e1", value);

  // Put C again
  ASSERT_OK(batch_->Put(cf1.get(), "C", "c1"));
  s = batch_->GetFromBatch(cf1.get(), options_, "C", &value);
  ASSERT_OK(s);
  ASSERT_EQ("c1", value);
  s = batch_->GetFromBatch(cf1.get(), options_, "E", &value);
  ASSERT_OK(s);
  ASSERT_EQ("e1", value);
}

/**
 * Tests Single Delete followed by Delete Range
 */
TEST_F(WriteBatchWithIndexDeleteRangeTest, SingleDeleteThenDeleteRange) {
  const bool overwrite_key = true;
  batch_.reset(new WriteBatchWithIndex(BytewiseComparator(), 0, overwrite_key));
  Status s;
  std::string value;

  // Put A, B, C, D
  ASSERT_OK(batch_->Put("A", "a0"));
  ASSERT_OK(batch_->Put("B", "b0"));
  ASSERT_OK(batch_->Put("C", "c0"));
  ASSERT_OK(batch_->Put("D", "d0"));
  s = batch_->GetFromBatch(options_, "A", &value);
  ASSERT_OK(s);
  ASSERT_EQ("a0", value);
  s = batch_->GetFromBatch(options_, "B", &value);
  ASSERT_OK(s);
  ASSERT_EQ("b0", value);
  s = batch_->GetFromBatch(options_, "C", &value);
  ASSERT_OK(s);
  ASSERT_EQ("c0", value);
  s = batch_->GetFromBatch(options_, "D", &value);
  ASSERT_OK(s);
  ASSERT_EQ("d0", value);

  // Single Delete B and C
  ASSERT_OK(batch_->SingleDelete("B"));
  ASSERT_OK(batch_->SingleDelete("C"));
  s = batch_->GetFromBatch(options_, "B", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(options_, "C", &value);
  ASSERT_NOT_FOUND(s);

  // Delete Range C..E
  ASSERT_OK(batch_->DeleteRange("C", "E"));
  s = batch_->GetFromBatch(options_, "C", &value);
  ASSERT_NOT_FOUND(s);

  // Check only A exists
  s = batch_->GetFromBatch(options_, "A", &value);
  ASSERT_OK(s);
  ASSERT_EQ("a0", value);
  s = batch_->GetFromBatch(options_, "B", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(options_, "C", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(options_, "D", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(options_, "E", &value);
  ASSERT_NOT_FOUND(s);

  // Put C again
  ASSERT_OK(batch_->Put("C", "c1"));

  // Check only A and C exist
  s = batch_->GetFromBatch(options_, "A", &value);
  ASSERT_OK(s);
  ASSERT_EQ("a0", value);
  s = batch_->GetFromBatch(options_, "B", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(options_, "C", &value);
  ASSERT_OK(s);
  ASSERT_EQ("c1", value);
  s = batch_->GetFromBatch(options_, "D", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(options_, "E", &value);
  ASSERT_NOT_FOUND(s);

  // Put B again
  ASSERT_OK(batch_->Put("B", "b1"));

  // Check only A, B and C exist
  s = batch_->GetFromBatch(options_, "A", &value);
  ASSERT_OK(s);
  ASSERT_EQ("a0", value);
  s = batch_->GetFromBatch(options_, "B", &value);
  ASSERT_OK(s);
  ASSERT_EQ("b1", value);
  s = batch_->GetFromBatch(options_, "C", &value);
  ASSERT_OK(s);
  ASSERT_EQ("c1", value);
  s = batch_->GetFromBatch(options_, "D", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(options_, "E", &value);
  ASSERT_NOT_FOUND(s);
}

/**
 * Tests Single Delete followed by Delete Range
 * on a Column Family.
 */
TEST_F(WriteBatchWithIndexDeleteRangeTest, SingleDeleteThenDeleteRangeCF) {
  ASSERT_OK(OpenDB());
  auto cf1 = makeCF("First Family");

  const bool overwrite_key = true;
  batch_.reset(new WriteBatchWithIndex(BytewiseComparator(), 0, overwrite_key));

  Status s;
  std::string value;

  // Put A, B, C, D
  ASSERT_OK(batch_->Put(cf1.get(), "A", "a0"));
  ASSERT_OK(batch_->Put(cf1.get(), "B", "b0"));
  ASSERT_OK(batch_->Put(cf1.get(), "C", "c0"));
  ASSERT_OK(batch_->Put(cf1.get(), "D", "d0"));
  s = batch_->GetFromBatch(cf1.get(), options_, "A", &value);
  ASSERT_OK(s);
  ASSERT_EQ("a0", value);
  s = batch_->GetFromBatch(cf1.get(), options_, "B", &value);
  ASSERT_OK(s);
  ASSERT_EQ("b0", value);
  s = batch_->GetFromBatch(cf1.get(), options_, "C", &value);
  ASSERT_OK(s);
  ASSERT_EQ("c0", value);
  s = batch_->GetFromBatch(cf1.get(), options_, "D", &value);
  ASSERT_OK(s);
  ASSERT_EQ("d0", value);

  // Single Delete B and C
  ASSERT_OK(batch_->SingleDelete(cf1.get(), "B"));
  ASSERT_OK(batch_->SingleDelete(cf1.get(), "C"));
  s = batch_->GetFromBatch(cf1.get(), options_, "B", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(cf1.get(), options_, "C", &value);
  ASSERT_NOT_FOUND(s);

  // Delete Range C..E
  ASSERT_OK(batch_->DeleteRange(cf1.get(), "C", "E"));
  s = batch_->GetFromBatch(cf1.get(), options_, "C", &value);
  ASSERT_NOT_FOUND(s);

  // Check only A exists
  s = batch_->GetFromBatch(cf1.get(), options_, "A", &value);
  ASSERT_OK(s);
  ASSERT_EQ("a0", value);
  s = batch_->GetFromBatch(cf1.get(), options_, "B", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(cf1.get(), options_, "C", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(cf1.get(), options_, "D", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(cf1.get(), options_, "E", &value);
  ASSERT_NOT_FOUND(s);

  // Put C again
  ASSERT_OK(batch_->Put(cf1.get(), "C", "c1"));

  // Check only A and C exist
  s = batch_->GetFromBatch(cf1.get(), options_, "A", &value);
  ASSERT_OK(s);
  ASSERT_EQ("a0", value);
  s = batch_->GetFromBatch(cf1.get(), options_, "B", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(cf1.get(), options_, "C", &value);
  ASSERT_OK(s);
  ASSERT_EQ("c1", value);
  s = batch_->GetFromBatch(cf1.get(), options_, "D", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(cf1.get(), options_, "E", &value);
  ASSERT_NOT_FOUND(s);

  // Put B again
  ASSERT_OK(batch_->Put(cf1.get(), "B", "b1"));

  // Check only A, B and C exist
  s = batch_->GetFromBatch(cf1.get(), options_, "A", &value);
  ASSERT_OK(s);
  ASSERT_EQ("a0", value);
  s = batch_->GetFromBatch(cf1.get(), options_, "B", &value);
  ASSERT_OK(s);
  ASSERT_EQ("b1", value);
  s = batch_->GetFromBatch(cf1.get(), options_, "C", &value);
  ASSERT_OK(s);
  ASSERT_EQ("c1", value);
  s = batch_->GetFromBatch(cf1.get(), options_, "D", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(cf1.get(), options_, "E", &value);
  ASSERT_NOT_FOUND(s);
}

// TODO(AR) do the above need to be repeated for BatchAndDB?

/**
 * Checks that DeleteRange on a WBWI works correctly
 * for wbwi->GetFromBatchAndDB and db_->Write.
 */
TEST_F(WriteBatchWithIndexDeleteRangeTest, BatchAndDB) {
  ASSERT_OK(OpenDB());
  Status s;

  // Put A, B, CC, C into the Database
  s = db_->Put(write_opts_, "A", "a0");
  ASSERT_OK(s);
  s = db_->Put(write_opts_, "B", "b0");
  ASSERT_OK(s);
  s = db_->Put(write_opts_, "BB", "bb0");
  ASSERT_OK(s);
  s = db_->Put(write_opts_, "C", "c0");
  ASSERT_OK(s);

  const bool overwrite_key = true;
  batch_.reset(new WriteBatchWithIndex(BytewiseComparator(), 0, overwrite_key));
  std::string value;

  // Put B, D, E into the WBWI
  ASSERT_OK(batch_->Put("B", "b"));
  ASSERT_OK(batch_->Put("D", "d"));
  ASSERT_OK(batch_->Put("E", "e"));

  // Check that only A, B (from WBWI), BB, C, D and E, are visible
  s = batch_->GetFromBatchAndDB(db_, read_opts_, "A", &value);
  ASSERT_OK(s);
  ASSERT_EQ("a0", value);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, "B", &value);
  ASSERT_OK(s);
  ASSERT_EQ("b", value);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, "BB", &value);
  ASSERT_OK(s);
  ASSERT_EQ("bb0", value);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, "C", &value);
  ASSERT_OK(s);
  ASSERT_EQ("c0", value);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, "D", &value);
  ASSERT_OK(s);
  ASSERT_EQ("d", value);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, "E", &value);
  ASSERT_OK(s);
  ASSERT_EQ("e", value);

  // Delete B..D for the WBWI (i.e. B in the WBWI, and hides B, BB, and C from
  // the db)
  ASSERT_OK(batch_->DeleteRange("B", "D"));

  // Check that only A, D and E, are visible
  s = batch_->GetFromBatchAndDB(db_, read_opts_, "A", &value);
  ASSERT_OK(s);
  ASSERT_EQ("a0", value);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, "B", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, "BB", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, "C", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, "D", &value);
  ASSERT_OK(s);
  ASSERT_EQ("d", value);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, "E", &value);
  ASSERT_OK(s);
  ASSERT_EQ("e", value);

  // Write the WBWI to the Database
  ASSERT_OK(db_->Write(write_opts_, batch_->GetWriteBatch()));

  // Check that only A, D and E, are in the database now
  s = db_->Get(read_opts_, "A", &value);
  ASSERT_OK(s);
  ASSERT_EQ("a0", value);
  s = db_->Get(read_opts_, "B", &value);
  ASSERT_NOT_FOUND(s);
  s = db_->Get(read_opts_, "BB", &value);
  ASSERT_NOT_FOUND(s);
  s = db_->Get(read_opts_, "C", &value);
  ASSERT_NOT_FOUND(s);
  s = db_->Get(read_opts_, "D", &value);
  ASSERT_OK(s);
  ASSERT_EQ("d", value);
  s = db_->Get(read_opts_, "E", &value);
  ASSERT_OK(s);
  ASSERT_EQ("e", value);
  s = db_->Get(read_opts_, "F", &value);
  ASSERT_NOT_FOUND(s);

  // Check that WBWI hasn't changed since db_->Write
  // So... Check that only A, D and E, are visible
  s = batch_->GetFromBatchAndDB(db_, read_opts_, "A", &value);
  ASSERT_OK(s);
  ASSERT_EQ("a0", value);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, "B", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, "BB", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, "C", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, "D", &value);
  ASSERT_OK(s);
  ASSERT_EQ("d", value);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, "E", &value);
  ASSERT_OK(s);
  ASSERT_EQ("e", value);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, "F", &value);
  ASSERT_NOT_FOUND(s);
}

/**
 * Checks that DeleteRange on a WBWI works correctly
 * on a Column Family for wbwi->GetFromBatchAndDB and db_->Write.
 */
TEST_F(WriteBatchWithIndexDeleteRangeTest, BatchAndDBCF) {
  ASSERT_OK(OpenDB());
  auto cf1 = makeCF("First Family");

  ReadOptions read_opts_;
  WriteOptions write_opts_;
  Status s;

  // Put A, B, CC, C into the DB
  s = db_->Put(write_opts_, cf1.get(), "A", "a0");
  ASSERT_OK(s);
  s = db_->Put(write_opts_, cf1.get(), "B", "b0");
  ASSERT_OK(s);
  s = db_->Put(write_opts_, cf1.get(), "BB", "bb0");
  ASSERT_OK(s);
  s = db_->Put(write_opts_, cf1.get(), "C", "c0");
  ASSERT_OK(s);

  const bool overwrite_key = true;
  batch_.reset(new WriteBatchWithIndex(BytewiseComparator(), 0, overwrite_key));

  std::string value;

  // Put B, D, E into the WBWI
  ASSERT_OK(batch_->Put(cf1.get(), "B", "b"));
  ASSERT_OK(batch_->Put(cf1.get(), "D", "d"));
  ASSERT_OK(batch_->Put(cf1.get(), "E", "e"));

  // Check that only A, B (from WBWI), BB, C, D and E, are visible
  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf1.get(), "A", &value);
  ASSERT_OK(s);
  ASSERT_EQ("a0", value);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf1.get(), "B", &value);
  ASSERT_OK(s);
  ASSERT_EQ("b", value);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf1.get(), "BB", &value);
  ASSERT_OK(s);
  ASSERT_EQ("bb0", value);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf1.get(), "C", &value);
  ASSERT_OK(s);
  ASSERT_EQ("c0", value);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf1.get(), "D", &value);
  ASSERT_OK(s);
  ASSERT_EQ("d", value);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf1.get(), "E", &value);
  ASSERT_OK(s);
  ASSERT_EQ("e", value);

  // Delete B..D for the WBWI (i.e. B in the WBWI, and hides B, BB, and C from
  // the db)
  ASSERT_OK(batch_->DeleteRange(cf1.get(), "B", "D"));

  // Check that only A, D and E, are visible
  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf1.get(), "A", &value);
  ASSERT_OK(s);
  ASSERT_EQ("a0", value);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf1.get(), "B", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf1.get(), "BB", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf1.get(), "C", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf1.get(), "D", &value);
  ASSERT_OK(s);
  ASSERT_EQ("d", value);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf1.get(), "E", &value);
  ASSERT_OK(s);
  ASSERT_EQ("e", value);

  // Write the WBWI to the Database
  ASSERT_OK(db_->Write(write_opts_, batch_->GetWriteBatch()));

  // Check that only A, D and E, are in the database now
  s = db_->Get(read_opts_, cf1.get(), "A", &value);
  ASSERT_OK(s);
  ASSERT_EQ("a0", value);
  s = db_->Get(read_opts_, cf1.get(), "B", &value);
  ASSERT_NOT_FOUND(s);
  s = db_->Get(read_opts_, cf1.get(), "BB", &value);
  ASSERT_NOT_FOUND(s);
  s = db_->Get(read_opts_, cf1.get(), "C", &value);
  ASSERT_NOT_FOUND(s);
  s = db_->Get(read_opts_, cf1.get(), "D", &value);
  ASSERT_OK(s);
  ASSERT_EQ("d", value);
  s = db_->Get(read_opts_, cf1.get(), "E", &value);
  ASSERT_OK(s);
  ASSERT_EQ("e", value);
  s = db_->Get(read_opts_, cf1.get(), "F", &value);
  ASSERT_NOT_FOUND(s);

  // Check that WBWI hasn't changed since db_->Write
  // So... Check that only A, D and E, are visible
  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf1.get(), "A", &value);
  ASSERT_OK(s);
  ASSERT_EQ("a0", value);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf1.get(), "B", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf1.get(), "BB", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf1.get(), "C", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf1.get(), "D", &value);
  ASSERT_OK(s);
  ASSERT_EQ("d", value);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf1.get(), "E", &value);
  ASSERT_OK(s);
  ASSERT_EQ("e", value);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf1.get(), "F", &value);
  ASSERT_NOT_FOUND(s);
}

/**
 * Range deletion using the batch
 * Check get with batch and underlying database
 */
TEST_F(WriteBatchWithIndexDeleteRangeTest, DeletedRangeRemembered) {
  ASSERT_OK(OpenDB());
  Status s;

  // Put A, B, C into the DB
  s = db_->Put(write_opts_, "A", "a0");
  ASSERT_OK(s);
  s = db_->Put(write_opts_, "B", "b0");
  ASSERT_OK(s);
  s = db_->Put(write_opts_, "C", "c0");
  ASSERT_OK(s);

  const bool overwrite_key = true;
  batch_.reset(new WriteBatchWithIndex(BytewiseComparator(), 0, overwrite_key));

  std::string value;

  // Put B, D, E into the WBWI
  ASSERT_OK(batch_->Put("B", "b"));
  ASSERT_OK(batch_->Put("D", "d"));
  ASSERT_OK(batch_->Put("E", "e"));

  // Delete B..D
  ASSERT_OK(batch_->DeleteRange("B", "D"));

  s = batch_->GetFromBatchAndDB(db_, read_opts_, "A", &value);
  ASSERT_OK(s);
  ASSERT_EQ("a0", value);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, "B", &value);
  ASSERT_NOT_FOUND(s);

  // This checks the range map recording explicit deletion
  // "deletes" the C in the underlying database
  s = batch_->GetFromBatchAndDB(db_, read_opts_, "C", &value);
  ASSERT_NOT_FOUND(s);

  s = batch_->GetFromBatch(options_, "D", &value);
  ASSERT_OK(s);
  ASSERT_EQ("d", value);
  s = batch_->GetFromBatch(options_, "E", &value);
  ASSERT_OK(s);
  ASSERT_EQ("e", value);
  s = batch_->GetFromBatch(options_, "F", &value);
  ASSERT_NOT_FOUND(s);
}

/**
 * Range deletion using the batch
 * Check get with batch and underlying database
 * for a Column Family
 */
TEST_F(WriteBatchWithIndexDeleteRangeTest, DeletedRangeRememberedCF) {
  ASSERT_OK(OpenDB());
  auto cf1 = makeCF("First Family");

  Status s;

  // Put A, B, C into the DB
  s = db_->Put(write_opts_, cf1.get(), "A", "a0");
  ASSERT_OK(s);
  s = db_->Put(write_opts_, cf1.get(), "B", "b0");
  ASSERT_OK(s);
  s = db_->Put(write_opts_, cf1.get(), "C", "c0");
  ASSERT_OK(s);

  const bool overwrite_key = true;
  batch_.reset(new WriteBatchWithIndex(BytewiseComparator(), 0, overwrite_key));

  std::string value;

  // Put B, D, E into the WBWI
  ASSERT_OK(batch_->Put(cf1.get(), "B", "b"));
  ASSERT_OK(batch_->Put(cf1.get(), "D", "d"));
  ASSERT_OK(batch_->Put(cf1.get(), "E", "e"));

  // Delete B..D
  ASSERT_OK(batch_->DeleteRange(cf1.get(), "B", "D"));

  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf1.get(), "A", &value);
  ASSERT_OK(s);
  ASSERT_EQ("a0", value);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf1.get(), "B", &value);
  ASSERT_NOT_FOUND(s);

  // This checks the range map recording explicit deletion
  // "deletes" the C in the underlying database
  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf1.get(), "C", &value);
  ASSERT_NOT_FOUND(s);

  s = batch_->GetFromBatch(cf1.get(), options_, "D", &value);
  ASSERT_OK(s);
  ASSERT_EQ("d", value);
  s = batch_->GetFromBatch(cf1.get(), options_, "E", &value);
  ASSERT_OK(s);
  ASSERT_EQ("e", value);
  s = batch_->GetFromBatch(cf1.get(), options_, "F", &value);
  ASSERT_NOT_FOUND(s);
}

TEST_F(WriteBatchWithIndexDeleteRangeTest, RollbackDeleteRange) {
  ASSERT_OK(OpenDB());
  Status s;

  // Put A, B, C into the DB
  s = db_->Put(write_opts_, "A", "a0");
  ASSERT_OK(s);
  s = db_->Put(write_opts_, "B", "b0");
  ASSERT_OK(s);
  s = db_->Put(write_opts_, "C", "c0");
  ASSERT_OK(s);

  const bool overwrite_key = true;
  batch_.reset(new WriteBatchWithIndex(BytewiseComparator(), 0, overwrite_key));

  std::string value;

  // Put B, D, E into the WBWI
  ASSERT_OK(batch_->Put("B", "b"));
  ASSERT_OK(batch_->Put("D", "d"));
  ASSERT_OK(batch_->Put("E", "e"));

  // SAVE POINT
  batch_->SetSavePoint();

  // Put B, CC, D, E into the WBWI
  ASSERT_OK(batch_->Put("B", "b2"));
  ASSERT_OK(batch_->Put("CC", "cc2"));
  ASSERT_OK(batch_->Put("D", "d2"));
  ASSERT_OK(batch_->Put("E", "e2"));

  // Delete B..D
  ASSERT_OK(batch_->DeleteRange("B", "D"));

  s = batch_->GetFromBatchAndDB(db_, read_opts_, "A", &value);
  ASSERT_OK(s);
  ASSERT_EQ("a0", value);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, "B", &value);
  ASSERT_NOT_FOUND(s);

  // This checks the range map recording explicit deletion
  // "deletes" the C in the underlying database
  s = batch_->GetFromBatchAndDB(db_, read_opts_, "C", &value);
  ASSERT_NOT_FOUND(s);

  s = batch_->GetFromBatchAndDB(db_, read_opts_, "CC", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(options_, "D", &value);
  ASSERT_OK(s);
  ASSERT_EQ("d2", value);
  s = batch_->GetFromBatch(options_, "E", &value);
  ASSERT_OK(s);
  ASSERT_EQ("e2", value);
  s = batch_->GetFromBatch(options_, "F", &value);
  ASSERT_NOT_FOUND(s);

  // ROLLBACK SAVE POINT
  ASSERT_OK(batch_->RollbackToSavePoint());

  // Check the deleted range B..D is no longer deleted
  // along with everything else being rolled back to the SP

  s = batch_->GetFromBatchAndDB(db_, read_opts_, "A", &value);
  ASSERT_OK(s);
  ASSERT_EQ("a0", value);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, "B", &value);
  ASSERT_OK(s);
  ASSERT_EQ("b", value);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, "C", &value);
  ASSERT_OK(s);
  ASSERT_EQ("c0", value);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, "CC", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(options_, "D", &value);
  ASSERT_OK(s);
  ASSERT_EQ("d", value);
  s = batch_->GetFromBatch(options_, "E", &value);
  ASSERT_OK(s);
  ASSERT_EQ("e", value);
  s = batch_->GetFromBatch(options_, "F", &value);
  ASSERT_NOT_FOUND(s);
}

TEST_F(WriteBatchWithIndexDeleteRangeTest, RollbackDeleteRangeCF) {
  ASSERT_OK(OpenDB());
  auto cf1 = makeCF("First Family");

  Status s;

  // Put A, B, C into the DB
  s = db_->Put(write_opts_, cf1.get(), "A", "a0");
  ASSERT_OK(s);
  s = db_->Put(write_opts_, cf1.get(), "B", "b0");
  ASSERT_OK(s);
  s = db_->Put(write_opts_, cf1.get(), "C", "c0");
  ASSERT_OK(s);

  const bool overwrite_key = true;
  batch_.reset(new WriteBatchWithIndex(BytewiseComparator(), 0, overwrite_key));

  std::string value;

  // Put B, D, E into the WBWI
  ASSERT_OK(batch_->Put(cf1.get(), "B", "b"));
  ASSERT_OK(batch_->Put(cf1.get(), "D", "d"));
  ASSERT_OK(batch_->Put(cf1.get(), "E", "e"));

  // SAVE POINT
  batch_->SetSavePoint();

  // Put B, CC, D, E into the WBWI
  ASSERT_OK(batch_->Put(cf1.get(), "B", "b2"));
  ASSERT_OK(batch_->Put(cf1.get(), "CC", "cc2"));
  ASSERT_OK(batch_->Put(cf1.get(), "D", "d2"));
  ASSERT_OK(batch_->Put(cf1.get(), "E", "e2"));

  // Delete B..D
  ASSERT_OK(batch_->DeleteRange(cf1.get(), "B", "D"));

  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf1.get(), "A", &value);
  ASSERT_OK(s);
  ASSERT_EQ("a0", value);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf1.get(), "B", &value);
  ASSERT_NOT_FOUND(s);

  // This checks the range map recording explicit deletion
  // "deletes" the C in the underlying database
  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf1.get(), "C", &value);
  ASSERT_NOT_FOUND(s);

  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf1.get(), "CC", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(cf1.get(), options_, "D", &value);
  ASSERT_OK(s);
  ASSERT_EQ("d2", value);
  s = batch_->GetFromBatch(cf1.get(), options_, "E", &value);
  ASSERT_OK(s);
  ASSERT_EQ("e2", value);
  s = batch_->GetFromBatch(cf1.get(), options_, "F", &value);
  ASSERT_NOT_FOUND(s);

  // ROLLBACK SAVE POINT
  ASSERT_OK(batch_->RollbackToSavePoint());

  // Check the deleted range B..D is no longer deleted
  // along with everything else being rolled back to the SP

  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf1.get(), "A", &value);
  ASSERT_OK(s);
  ASSERT_EQ("a0", value);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf1.get(), "B", &value);
  ASSERT_OK(s);
  ASSERT_EQ("b", value);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf1.get(), "C", &value);
  ASSERT_OK(s);
  ASSERT_EQ("c0", value);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf1.get(), "CC", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(cf1.get(), options_, "D", &value);
  ASSERT_OK(s);
  ASSERT_EQ("d", value);
  s = batch_->GetFromBatch(cf1.get(), options_, "E", &value);
  ASSERT_OK(s);
  ASSERT_EQ("e", value);
  s = batch_->GetFromBatch(cf1.get(), options_, "F", &value);
  ASSERT_NOT_FOUND(s);
}

TEST_F(WriteBatchWithIndexDeleteRangeTest, RedoDeleteRange) {
  ASSERT_OK(OpenDB());
  Status s;

  // Put A, B, C into the DB
  s = db_->Put(write_opts_, "A", "a0");
  ASSERT_OK(s);
  s = db_->Put(write_opts_, "B", "b0");
  ASSERT_OK(s);
  s = db_->Put(write_opts_, "C", "c0");
  ASSERT_OK(s);

  const bool overwrite_key = true;
  batch_.reset(new WriteBatchWithIndex(BytewiseComparator(), 0, overwrite_key));

  std::string value;

  // Put B, CC, D, E into the WBWI
  ASSERT_OK(batch_->Put("B", "b2"));
  ASSERT_OK(batch_->Put("CC", "cc2"));
  ASSERT_OK(batch_->Put("D", "d2"));
  ASSERT_OK(batch_->Put("E", "e2"));

  // Delete B..D
  ASSERT_OK(batch_->DeleteRange("B", "D"));

  // Put CCC into the WBWI
  ASSERT_OK(batch_->Put("CCC", "ccc2"));

  s = batch_->GetFromBatchAndDB(db_, read_opts_, "A", &value);
  ASSERT_OK(s);
  ASSERT_EQ("a0", value);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, "B", &value);
  ASSERT_NOT_FOUND(s);

  // This checks the range map recording explicit deletion
  // "deletes" the C in the underlying database
  s = batch_->GetFromBatchAndDB(db_, read_opts_, "C", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, "CC", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(options_, "D", &value);
  ASSERT_OK(s);
  ASSERT_EQ("d2", value);
  s = batch_->GetFromBatch(options_, "E", &value);
  ASSERT_OK(s);
  ASSERT_EQ("e2", value);
  s = batch_->GetFromBatch(options_, "F", &value);
  ASSERT_NOT_FOUND(s);
  // Check the write *after* the Delete Range is still there
  s = batch_->GetFromBatchAndDB(db_, read_opts_, "CCC", &value);
  ASSERT_OK(s);
  ASSERT_EQ("ccc2", value);

  // We check that redo rolls the delete range forward to here
  // SAVE POINT
  batch_->SetSavePoint();
  ASSERT_OK(batch_->Put("CC", "cc3"));

  // Check the deleted range B..D is deleted again
  // along with everything else being rolled back to the SP
  // ROLLBACK SAVE POINT
  ASSERT_OK(batch_->RollbackToSavePoint());

  s = batch_->GetFromBatchAndDB(db_, read_opts_, "A", &value);
  ASSERT_OK(s);
  ASSERT_EQ("a0", value);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, "C", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, "B", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, "CC", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(options_, "D", &value);
  ASSERT_OK(s);
  ASSERT_EQ("d2", value);
  s = batch_->GetFromBatch(options_, "E", &value);
  ASSERT_OK(s);
  ASSERT_EQ("e2", value);
  s = batch_->GetFromBatch(options_, "F", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, "CCC", &value);
  ASSERT_OK(s);
  ASSERT_EQ("ccc2", value);
}

TEST_F(WriteBatchWithIndexDeleteRangeTest, RedoDeleteRangeCF) {
  ASSERT_OK(OpenDB());
  auto cf1 = makeCF("First Family");

  ReadOptions read_opts_;
  WriteOptions write_opts_;
  Status s;

  // Put A, B, C into the DB
  s = db_->Put(write_opts_, cf1.get(), "A", "a0");
  ASSERT_OK(s);
  s = db_->Put(write_opts_, cf1.get(), "B", "b0");
  ASSERT_OK(s);
  s = db_->Put(write_opts_, cf1.get(), "C", "c0");
  ASSERT_OK(s);

  const bool overwrite_key = true;
  batch_.reset(new WriteBatchWithIndex(BytewiseComparator(), 0, overwrite_key));

  std::string value;

  // Put B, CC, D, E into the WBWI
  ASSERT_OK(batch_->Put(cf1.get(), "B", "b2"));
  ASSERT_OK(batch_->Put(cf1.get(), "CC", "cc2"));
  ASSERT_OK(batch_->Put(cf1.get(), "D", "d2"));
  ASSERT_OK(batch_->Put(cf1.get(), "E", "e2"));

  // Delete B..D
  ASSERT_OK(batch_->DeleteRange(cf1.get(), "B", "D"));

  // Put CCC into the WBWI
  ASSERT_OK(batch_->Put(cf1.get(), "CCC", "ccc2"));

  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf1.get(), "A", &value);
  ASSERT_OK(s);
  ASSERT_EQ("a0", value);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf1.get(), "B", &value);
  ASSERT_NOT_FOUND(s);

  // This checks the range map recording explicit deletion
  // "deletes" the C in the underlying database
  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf1.get(), "C", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf1.get(), "CC", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(cf1.get(), options_, "D", &value);
  ASSERT_OK(s);
  ASSERT_EQ("d2", value);
  s = batch_->GetFromBatch(cf1.get(), options_, "E", &value);
  ASSERT_OK(s);
  ASSERT_EQ("e2", value);
  s = batch_->GetFromBatch(cf1.get(), options_, "F", &value);
  ASSERT_NOT_FOUND(s);
  // Check the write *after* the Delete Range is still there
  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf1.get(), "CCC", &value);
  ASSERT_OK(s);
  ASSERT_EQ("ccc2", value);

  // We check that redo rolls the delete range forward to here
  // SAVE POINT
  batch_->SetSavePoint();
  ASSERT_OK(batch_->Put(cf1.get(), "CC", "cc3"));

  // Check the deleted range [B,D) is deleted again
  // along with everything else being rolled back to the SP
  // ROLLBACK SAVE POINT
  ASSERT_OK(batch_->RollbackToSavePoint());

  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf1.get(), "A", &value);
  ASSERT_OK(s);
  ASSERT_EQ("a0", value);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf1.get(), "C", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf1.get(), "B", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf1.get(), "CC", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(cf1.get(), options_, "D", &value);
  ASSERT_OK(s);
  ASSERT_EQ("d2", value);
  s = batch_->GetFromBatch(cf1.get(), options_, "E", &value);
  ASSERT_OK(s);
  ASSERT_EQ("e2", value);
  s = batch_->GetFromBatch(cf1.get(), options_, "F", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf1.get(), "CCC", &value);
  ASSERT_OK(s);
  ASSERT_EQ("ccc2", value);
}

TEST_F(WriteBatchWithIndexDeleteRangeTest, MultipleRanges) {
  ASSERT_OK(OpenDB());
  Status s;

  // Put D into the DB
  s = db_->Put(write_opts_, "D", "d0");
  ASSERT_OK(s);

  const bool overwrite_key = true;
  batch_.reset(new WriteBatchWithIndex(BytewiseComparator(), 0, overwrite_key));

  std::string value;

  // Delete B..C and F..G
  ASSERT_OK(batch_->DeleteRange("B", "C"));
  ASSERT_OK(batch_->DeleteRange("F", "G"));

  s = batch_->GetFromBatchAndDB(db_, read_opts_, "D", &value);
  ASSERT_OK(s);
  ASSERT_EQ("d0", value);

  ASSERT_OK(batch_->DeleteRange("A", "H"));

  s = batch_->GetFromBatchAndDB(db_, read_opts_, "D", &value);
  ASSERT_NOT_FOUND(s);
}

TEST_F(WriteBatchWithIndexDeleteRangeTest, MultipleRangesCF) {
  ASSERT_OK(OpenDB());
  auto cf1 = makeCF("First Family");
  Status s;

  // Put D into the DB
  s = db_->Put(write_opts_, cf1.get(), "D", "d0");
  ASSERT_OK(s);

  const bool overwrite_key = true;
  batch_.reset(new WriteBatchWithIndex(BytewiseComparator(), 0, overwrite_key));

  std::string value;

  // Delete B..C and F..G
  ASSERT_OK(batch_->DeleteRange(cf1.get(), "B", "C"));
  ASSERT_OK(batch_->DeleteRange(cf1.get(), "F", "G"));

  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf1.get(), "D", &value);
  ASSERT_OK(s);
  ASSERT_EQ("d0", value);

  ASSERT_OK(batch_->DeleteRange(cf1.get(), "A", "H"));

  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf1.get(), "D", &value);
  ASSERT_NOT_FOUND(s);
}

TEST_F(WriteBatchWithIndexDeleteRangeTest, MoreRanges) {
  ReadOptions read_opts_;
  WriteOptions write_opts_;

  const bool overwrite_key = true;
  batch_.reset(new WriteBatchWithIndex(BytewiseComparator(), 0, overwrite_key));

  std::string value;

  std::vector<std::string> entries;
  std::vector<std::string> expect;

  std::vector<std::string> all_keys = {"A",  "B",  "BA", "BB", "BC", "BD", "BE",
                                       "C",  "CA", "CB", "CC", "D",  "DA", "DB",
                                       "DC", "DD", "E",  "EA", "EB", "EF", "EG",
                                       "F",  "FA", "FB", "G",  "GA", "GB"};
  expect = {"A=a", "B={}", "C=c"};

  ASSERT_OK(batch_->Put("A", "a"));
  ASSERT_OK(batch_->Put("B", "b"));
  ASSERT_OK(batch_->Put("BA", "ba"));
  ASSERT_OK(batch_->Put("BB", "bb"));
  ASSERT_OK(batch_->Put("BC", "bc"));
  ASSERT_OK(batch_->Put("BD", "bd"));
  ASSERT_OK(batch_->Put("BE", "be"));
  ASSERT_OK(batch_->Put("C", "c"));
  ASSERT_OK(batch_->Put("CA", "ca"));
  ASSERT_OK(batch_->Put("CB", "cb"));
  ASSERT_OK(batch_->Put("CC", "cc"));
  ASSERT_OK(batch_->Put("D", "d"));
  ASSERT_OK(batch_->Put("DA", "da"));
  ASSERT_OK(batch_->Put("DB", "db"));
  ASSERT_OK(batch_->Put("DC", "dc"));
  ASSERT_OK(batch_->Put("DD", "dd"));
  ASSERT_OK(batch_->Put("E", "e"));
  ASSERT_OK(batch_->Put("EA", "ea"));
  ASSERT_OK(batch_->Put("EB", "eb"));
  ASSERT_OK(batch_->Put("G", "g"));
  ASSERT_OK(batch_->Put("GA", "ga"));
  ASSERT_OK(batch_->Put("GB", "gb"));

  ASSERT_OK(batch_->DeleteRange("B", "BE"));
  ASSERT_OK(batch_->DeleteRange("D", "DE"));

  entries = GetValuesFromBatch(batch_.get(), all_keys);
  expect = {"A=a",   "B={}",  "BA={}", "BB={}", "BC={}", "BD={}", "BE=be",
            "C=c",   "CA=ca", "CB=cb", "CC=cc", "D={}",  "DA={}", "DB={}",
            "DC={}", "DD={}", "E=e",   "EA=ea", "EB=eb", "EF={}", "EG={}",
            "F={}",  "FA={}", "FB={}", "G=g",   "GA=ga", "GB=gb"};
  ASSERT_EQ(expect, entries);

  ASSERT_OK(batch_->Put("F", "f"));
  ASSERT_OK(batch_->Put("FA", "fa"));
  ASSERT_OK(batch_->Put("FB", "fb"));

  ASSERT_OK(batch_->DeleteRange("BC", "DC"));

  ASSERT_OK(batch_->DeleteRange("DA", "F"));

  entries = GetValuesFromBatch(batch_.get(), all_keys);
  expect = {"A=a",   "B={}",  "BA={}", "BB={}", "BC={}", "BD={}", "BE={}",
            "C={}",  "CA={}", "CB={}", "CC={}", "D={}",  "DA={}", "DB={}",
            "DC={}", "DD={}", "E={}",  "EA={}", "EB={}", "EF={}", "EG={}",
            "F=f",   "FA=fa", "FB=fb", "G=g",   "GA=ga", "GB=gb"};
  ASSERT_EQ(expect, entries);

  ASSERT_OK(batch_->DeleteRange("BC", "G"));

  entries = GetValuesFromBatch(batch_.get(), all_keys);
  expect = {"A=a",   "B={}",  "BA={}", "BB={}", "BC={}", "BD={}", "BE={}",
            "C={}",  "CA={}", "CB={}", "CC={}", "D={}",  "DA={}", "DB={}",
            "DC={}", "DD={}", "E={}",  "EA={}", "EB={}", "EF={}", "EG={}",
            "F={}",  "FA={}", "FB={}", "G=g",   "GA=ga", "GB=gb"};
  ASSERT_EQ(expect, entries);

  ASSERT_OK(batch_->Put("C", "c2"));
  ASSERT_OK(batch_->Put("CA", "ca2"));
  ASSERT_OK(batch_->Put("CC", "cc2"));
  ASSERT_OK(batch_->Put("D", "d2"));
  ASSERT_OK(batch_->Put("DA", "da2"));
  ASSERT_OK(batch_->Put("DC", "dc2"));
  ASSERT_OK(batch_->Put("DD", "dd2"));
  ASSERT_OK(batch_->Put("E", "e2"));
  ASSERT_OK(batch_->Put("EF", "ef2"));
  ASSERT_OK(batch_->Put("EG", "eg2"));
  ASSERT_OK(batch_->Put("F", "f2"));
  ASSERT_OK(batch_->Put("FB", "fb2"));
  ASSERT_OK(batch_->Put("GA", "ga2"));
  ASSERT_OK(batch_->Put("GB", "gb2"));

  entries = GetValuesFromBatch(batch_.get(), all_keys);
  expect = {"A=a",    "B={}",   "BA={}",  "BB={}",  "BC={}",  "BD={}",
            "BE={}",  "C=c2",   "CA=ca2", "CB={}",  "CC=cc2", "D=d2",
            "DA=da2", "DB={}",  "DC=dc2", "DD=dd2", "E=e2",   "EA={}",
            "EB={}",  "EF=ef2", "EG=eg2", "F=f2",   "FA={}",  "FB=fb2",
            "G=g",    "GA=ga2", "GB=gb2"};
  ASSERT_EQ(expect, entries);
}

TEST_F(WriteBatchWithIndexDeleteRangeTest, MoreRangesCF) {
  ASSERT_OK(OpenDB());
  auto cf1 = makeCF("First Family");

  ReadOptions read_opts_;
  WriteOptions write_opts_;

  const bool overwrite_key = true;
  batch_.reset(new WriteBatchWithIndex(BytewiseComparator(), 0, overwrite_key));

  std::string value;

  std::vector<std::string> entries;
  std::vector<std::string> expect;

  std::vector<std::string> all_keys = {"A",  "B",  "BA", "BB", "BC", "BD", "BE",
                                       "C",  "CA", "CB", "CC", "D",  "DA", "DB",
                                       "DC", "DD", "E",  "EA", "EB", "EF", "EG",
                                       "F",  "FA", "FB", "G",  "GA", "GB"};
  expect = {"A=a", "B={}", "C=c"};

  ASSERT_OK(batch_->Put(cf1.get(), "A", "a"));
  ASSERT_OK(batch_->Put(cf1.get(), "B", "b"));
  ASSERT_OK(batch_->Put(cf1.get(), "BA", "ba"));
  ASSERT_OK(batch_->Put(cf1.get(), "BB", "bb"));
  ASSERT_OK(batch_->Put(cf1.get(), "BC", "bc"));
  ASSERT_OK(batch_->Put(cf1.get(), "BD", "bd"));
  ASSERT_OK(batch_->Put(cf1.get(), "BE", "be"));
  ASSERT_OK(batch_->Put(cf1.get(), "C", "c"));
  ASSERT_OK(batch_->Put(cf1.get(), "CA", "ca"));
  ASSERT_OK(batch_->Put(cf1.get(), "CB", "cb"));
  ASSERT_OK(batch_->Put(cf1.get(), "CC", "cc"));
  ASSERT_OK(batch_->Put(cf1.get(), "D", "d"));
  ASSERT_OK(batch_->Put(cf1.get(), "DA", "da"));
  ASSERT_OK(batch_->Put(cf1.get(), "DB", "db"));
  ASSERT_OK(batch_->Put(cf1.get(), "DC", "dc"));
  ASSERT_OK(batch_->Put(cf1.get(), "DD", "dd"));
  ASSERT_OK(batch_->Put(cf1.get(), "E", "e"));
  ASSERT_OK(batch_->Put(cf1.get(), "EA", "ea"));
  ASSERT_OK(batch_->Put(cf1.get(), "EB", "eb"));
  ASSERT_OK(batch_->Put(cf1.get(), "G", "g"));
  ASSERT_OK(batch_->Put(cf1.get(), "GA", "ga"));
  ASSERT_OK(batch_->Put(cf1.get(), "GB", "gb"));

  ASSERT_OK(batch_->DeleteRange(cf1.get(), "B", "BE"));
  ASSERT_OK(batch_->DeleteRange(cf1.get(), "D", "DE"));

  entries = GetValuesFromBatch(batch_.get(), cf1.get(), all_keys);
  expect = {"A=a",   "B={}",  "BA={}", "BB={}", "BC={}", "BD={}", "BE=be",
            "C=c",   "CA=ca", "CB=cb", "CC=cc", "D={}",  "DA={}", "DB={}",
            "DC={}", "DD={}", "E=e",   "EA=ea", "EB=eb", "EF={}", "EG={}",
            "F={}",  "FA={}", "FB={}", "G=g",   "GA=ga", "GB=gb"};
  ASSERT_EQ(expect, entries);

  ASSERT_OK(batch_->Put(cf1.get(), "F", "f"));
  ASSERT_OK(batch_->Put(cf1.get(), "FA", "fa"));
  ASSERT_OK(batch_->Put(cf1.get(), "FB", "fb"));

  ASSERT_OK(batch_->DeleteRange(cf1.get(), "BC", "DC"));

  ASSERT_OK(batch_->DeleteRange(cf1.get(), "DA", "F"));

  entries = GetValuesFromBatch(batch_.get(), cf1.get(), all_keys);
  expect = {"A=a",   "B={}",  "BA={}", "BB={}", "BC={}", "BD={}", "BE={}",
            "C={}",  "CA={}", "CB={}", "CC={}", "D={}",  "DA={}", "DB={}",
            "DC={}", "DD={}", "E={}",  "EA={}", "EB={}", "EF={}", "EG={}",
            "F=f",   "FA=fa", "FB=fb", "G=g",   "GA=ga", "GB=gb"};
  ASSERT_EQ(expect, entries);

  ASSERT_OK(batch_->DeleteRange(cf1.get(), "BC", "G"));

  entries = GetValuesFromBatch(batch_.get(), cf1.get(), all_keys);
  expect = {"A=a",   "B={}",  "BA={}", "BB={}", "BC={}", "BD={}", "BE={}",
            "C={}",  "CA={}", "CB={}", "CC={}", "D={}",  "DA={}", "DB={}",
            "DC={}", "DD={}", "E={}",  "EA={}", "EB={}", "EF={}", "EG={}",
            "F={}",  "FA={}", "FB={}", "G=g",   "GA=ga", "GB=gb"};
  ASSERT_EQ(expect, entries);

  ASSERT_OK(batch_->Put(cf1.get(), "C", "c2"));
  ASSERT_OK(batch_->Put(cf1.get(), "CA", "ca2"));
  ASSERT_OK(batch_->Put(cf1.get(), "CC", "cc2"));
  ASSERT_OK(batch_->Put(cf1.get(), "D", "d2"));
  ASSERT_OK(batch_->Put(cf1.get(), "DA", "da2"));
  ASSERT_OK(batch_->Put(cf1.get(), "DC", "dc2"));
  ASSERT_OK(batch_->Put(cf1.get(), "DD", "dd2"));
  ASSERT_OK(batch_->Put(cf1.get(), "E", "e2"));
  ASSERT_OK(batch_->Put(cf1.get(), "EF", "ef2"));
  ASSERT_OK(batch_->Put(cf1.get(), "EG", "eg2"));
  ASSERT_OK(batch_->Put(cf1.get(), "F", "f2"));
  ASSERT_OK(batch_->Put(cf1.get(), "FB", "fb2"));
  ASSERT_OK(batch_->Put(cf1.get(), "GA", "ga2"));
  ASSERT_OK(batch_->Put(cf1.get(), "GB", "gb2"));

  entries = GetValuesFromBatch(batch_.get(), cf1.get(), all_keys);
  expect = {"A=a",    "B={}",   "BA={}",  "BB={}",  "BC={}",  "BD={}",
            "BE={}",  "C=c2",   "CA=ca2", "CB={}",  "CC=cc2", "D=d2",
            "DA=da2", "DB={}",  "DC=dc2", "DD=dd2", "E=e2",   "EA={}",
            "EB={}",  "EF=ef2", "EG=eg2", "F=f2",   "FA={}",  "FB=fb2",
            "G=g",    "GA=ga2", "GB=gb2"};
  ASSERT_EQ(expect, entries);
}

TEST_F(WriteBatchWithIndexDeleteRangeTest, BatchFlushDBRead) {
  ASSERT_OK(OpenDB());
  Status s;

  const bool overwrite_key = true;
  batch_.reset(new WriteBatchWithIndex(BytewiseComparator(), 0, overwrite_key));
  std::string value;

  // Put A, B, C into the WBWI
  ASSERT_OK(batch_->Put("A", "a0"));
  ASSERT_OK(batch_->Put("B", "b0"));
  ASSERT_OK(batch_->Put("C", "c0"));

  // Delete B..D
  ASSERT_OK(batch_->DeleteRange("B", "D"));

  db_->Write(write_opts_, batch_->GetWriteBatch());

  //
  // Check nothing is in the flushed batch
  //
  batch_->Clear();
  s = batch_->GetFromBatch(options_, "A", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(options_, "B", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(options_, "C", &value);
  ASSERT_NOT_FOUND(s);

  //
  // Check the Put(s) and DeleteRange(s) got into the DB
  //
  s = batch_->GetFromBatchAndDB(db_, read_opts_, "A", &value);
  ASSERT_OK(s);
  ASSERT_EQ("a0", value);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, "B", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, "C", &value);
  ASSERT_NOT_FOUND(s);

  //
  // Start a new batch
  // Check GetFromBatchAndDB gets from DB where there's a value there
  //
  batch_->Clear();
  ASSERT_OK(batch_->Put("B", "b"));
  ASSERT_OK(batch_->Put("D", "d"));
  ASSERT_OK(batch_->Put("E", "e"));
  ASSERT_OK(batch_->DeleteRange("B", "D"));
  db_->Write(write_opts_, batch_->GetWriteBatch());

  // Do the same set of checks twice,
  // second time clear the batch (which has already been written).
  //
  s = batch_->GetFromBatchAndDB(db_, read_opts_, "A", &value);
  ASSERT_OK(s);
  ASSERT_EQ("a0", value);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, "B", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, "C", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(options_, "D", &value);
  ASSERT_OK(s);
  ASSERT_EQ("d", value);
  s = batch_->GetFromBatch(options_, "E", &value);
  ASSERT_OK(s);
  ASSERT_EQ("e", value);
  s = batch_->GetFromBatch(options_, "F", &value);
  ASSERT_NOT_FOUND(s);

  batch_->Clear();

  s = batch_->GetFromBatchAndDB(db_, read_opts_, "A", &value);
  ASSERT_OK(s);
  ASSERT_EQ("a0", value);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, "B", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, "C", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(options_, "D", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(options_, "E", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(options_, "F", &value);
  ASSERT_NOT_FOUND(s);
}

TEST_F(WriteBatchWithIndexDeleteRangeTest, BatchFlushDBReadCF) {
  ASSERT_OK(OpenDB());
  auto cf1 = makeCF("First Family");

  ReadOptions read_opts_;
  WriteOptions write_opts_;
  Status s;

  const bool overwrite_key = true;
  batch_.reset(new WriteBatchWithIndex(BytewiseComparator(), 0, overwrite_key));

  std::string value;

  // Put A, B, C into the WBWI
  ASSERT_OK(batch_->Put(cf1.get(), "A", "a0"));
  ASSERT_OK(batch_->Put(cf1.get(), "B", "b0"));
  ASSERT_OK(batch_->Put(cf1.get(), "C", "c0"));

  // Delete B..D
  ASSERT_OK(batch_->DeleteRange(cf1.get(), "B", "D"));

  db_->Write(write_opts_, batch_->GetWriteBatch());

  //
  // Check nothing is in the flushed batch
  //
  batch_->Clear();
  s = batch_->GetFromBatch(cf1.get(), options_, "A", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(cf1.get(), options_, "B", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(cf1.get(), options_, "C", &value);
  ASSERT_NOT_FOUND(s);

  //
  // Check the Put(s) and DeleteRange(s) got into the DB
  //
  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf1.get(), "A", &value);
  ASSERT_OK(s);
  ASSERT_EQ("a0", value);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf1.get(), "B", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf1.get(), "C", &value);
  ASSERT_NOT_FOUND(s);

  //
  // Start a new batch
  // Check GetFromBatchAndDB gets from DB where there's a value there
  //
  batch_->Clear();
  ASSERT_OK(batch_->Put(cf1.get(), "B", "b"));
  ASSERT_OK(batch_->Put(cf1.get(), "D", "d"));
  ASSERT_OK(batch_->Put(cf1.get(), "E", "e"));
  ASSERT_OK(batch_->DeleteRange(cf1.get(), "B", "D"));
  db_->Write(write_opts_, batch_->GetWriteBatch());

  // Do the same set of checks twice,
  // second time clear the batch (which has already been written).
  //
  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf1.get(), "A", &value);
  ASSERT_OK(s);
  ASSERT_EQ("a0", value);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf1.get(), "B", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf1.get(), "C", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(cf1.get(), options_, "D", &value);
  ASSERT_OK(s);
  ASSERT_EQ("d", value);
  s = batch_->GetFromBatch(cf1.get(), options_, "E", &value);
  ASSERT_OK(s);
  ASSERT_EQ("e", value);
  s = batch_->GetFromBatch(cf1.get(), options_, "F", &value);
  ASSERT_NOT_FOUND(s);

  batch_->Clear();

  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf1.get(), "A", &value);
  ASSERT_OK(s);
  ASSERT_EQ("a0", value);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf1.get(), "B", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf1.get(), "C", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(cf1.get(), options_, "D", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(cf1.get(), options_, "E", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatch(cf1.get(), options_, "F", &value);
  ASSERT_NOT_FOUND(s);
}

TEST_F(WriteBatchWithIndexDeleteRangeTest, MultipleColumnFamilies) {
  ASSERT_OK(OpenDB());
  auto cf1 = makeCF("First Family");
  auto cf2 = makeCF("Second Family");
  Status s;

  const bool overwrite_key = true;
  batch_.reset(new WriteBatchWithIndex(BytewiseComparator(), 0, overwrite_key));

  std::string value;

  // Put Default={A,Z}, cf1.get()={A,Z} CF2={A,Z}
  ASSERT_OK(batch_->Put(cf1.get(), "A", "a_cf1.get()_0"));
  ASSERT_OK(batch_->Put(cf2.get(), "A", "a_cf2_0"));
  ASSERT_OK(batch_->Put("A", "a_cf0_0"));
  ASSERT_OK(batch_->Put(cf1.get(), "Z", "z_cf1.get()_0"));
  ASSERT_OK(batch_->Put(cf2.get(), "Z", "z_cf2_0"));
  ASSERT_OK(batch_->Put("Z", "z_cf0_0"));

  s = db_->Write(write_opts_, batch_->GetWriteBatch());
  ASSERT_OK(s);
  batch_->Clear();

  s = batch_->GetFromBatchAndDB(db_, read_opts_, "A", &value);
  ASSERT_OK(s);
  ASSERT_EQ("a_cf0_0", value);

  ASSERT_OK(batch_->Put(cf1.get(), "A", "a_cf1.get()"));
  ASSERT_OK(batch_->Put(cf2.get(), "A", "a_cf2"));
  ASSERT_OK(batch_->Put("A", "a_cf0"));
  ASSERT_OK(batch_->Put(cf1.get(), "B", "b_cf1.get()"));
  ASSERT_OK(batch_->Put(cf2.get(), "B", "b_cf2"));
  ASSERT_OK(batch_->Put("B", "b_cf0"));

  ASSERT_OK(batch_->DeleteRange("A", "M"));
  ASSERT_OK(batch_->DeleteRange(cf2.get(), "N", "ZZ"));

  s = batch_->GetFromBatchAndDB(db_, read_opts_, "A", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, "B", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, "Z", &value);
  ASSERT_OK(s);
  ASSERT_EQ("z_cf0_0", value);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf1.get(), "A", &value);
  ASSERT_OK(s);
  ASSERT_EQ("a_cf1.get()", value);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf1.get(), "B", &value);
  ASSERT_OK(s);
  ASSERT_EQ("b_cf1.get()", value);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf1.get(), "Z", &value);
  ASSERT_OK(s);
  ASSERT_EQ("z_cf1.get()_0", value);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf2.get(), "A", &value);
  ASSERT_OK(s);
  ASSERT_EQ("a_cf2", value);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf2.get(), "B", &value);
  ASSERT_OK(s);
  ASSERT_EQ("b_cf2", value);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf2.get(), "Z", &value);
  ASSERT_NOT_FOUND(s);

  // We will re-check these values when we roll back
  batch_->SetSavePoint();

  // Make some changes on top of the savepoint
  ASSERT_OK(batch_->Put(cf1.get(), "A", "a_cf1.get()_2"));
  ASSERT_OK(batch_->Put(cf2.get(), "A", "a_cf2_2"));
  ASSERT_OK(batch_->Put("A", "a_cf0_2"));
  ASSERT_OK(batch_->Put(cf1.get(), "B", "b_cf1.get()_2"));
  ASSERT_OK(batch_->Put(cf2.get(), "B", "b_cf2_2"));
  ASSERT_OK(batch_->Put("B", "b_cf0_2"));

  ASSERT_OK(batch_->DeleteRange("A", "M"));
  ASSERT_OK(batch_->DeleteRange(cf1.get(), "N", "ZZ"));

  s = batch_->GetFromBatchAndDB(db_, read_opts_, "A", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, "B", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, "Z", &value);
  ASSERT_OK(s);
  ASSERT_EQ("z_cf0_0", value);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf1.get(), "A", &value);
  ASSERT_OK(s);
  ASSERT_EQ("a_cf1.get()_2", value);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf1.get(), "B", &value);
  ASSERT_OK(s);
  ASSERT_EQ("b_cf1.get()_2", value);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf1.get(), "Z", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf2.get(), "A", &value);
  ASSERT_OK(s);
  ASSERT_EQ("a_cf2_2", value);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf2.get(), "B", &value);
  ASSERT_OK(s);
  ASSERT_EQ("b_cf2_2", value);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf2.get(), "Z", &value);
  ASSERT_NOT_FOUND(s);

  // Roll back, and do the original checks
  batch_->RollbackToSavePoint();

  s = batch_->GetFromBatchAndDB(db_, read_opts_, "A", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, "B", &value);
  ASSERT_NOT_FOUND(s);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, "Z", &value);
  ASSERT_OK(s);
  ASSERT_EQ("z_cf0_0", value);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf1.get(), "A", &value);
  ASSERT_OK(s);
  ASSERT_EQ("a_cf1.get()", value);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf1.get(), "B", &value);
  ASSERT_OK(s);
  ASSERT_EQ("b_cf1.get()", value);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf1.get(), "Z", &value);
  ASSERT_OK(s);
  ASSERT_EQ("z_cf1.get()_0", value);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf2.get(), "A", &value);
  ASSERT_OK(s);
  ASSERT_EQ("a_cf2", value);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf2.get(), "B", &value);
  ASSERT_OK(s);
  ASSERT_EQ("b_cf2", value);
  s = batch_->GetFromBatchAndDB(db_, read_opts_, cf2.get(), "Z", &value);
  ASSERT_NOT_FOUND(s);
}

}  // namespace ROCKSDB_NAMESPACE

int main(int argc, char** argv) {
  ROCKSDB_NAMESPACE::port::InstallStackTraceHandler();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

#else
#include <stdio.h>

int main() {
  fprintf(stderr, "SKIPPED\n");
  return 0;
}

#endif  // !ROCKSDB_LITE
