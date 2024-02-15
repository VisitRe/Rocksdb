//  Copyright (c) Meta Platforms, Inc. and affiliates.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include <memory>

#include "db/db_test_util.h"

namespace ROCKSDB_NAMESPACE {

class MultiCfIteratorTest : public DBTestBase {
 public:
  MultiCfIteratorTest()
      : DBTestBase("multi_cf_iterator_test", /*env_do_fsync=*/true) {}
};

TEST_F(MultiCfIteratorTest, SimpleValues) {
  Options options = GetDefaultOptions();
  auto verify = [&](const std::vector<ColumnFamilyHandle*>& cfhs,
                    const std::vector<Slice>& expected_keys,
                    const std::vector<Slice>& expected_values) {
    int i = 0;
    std::unique_ptr<MultiCfIterator> iter =
        db_->NewMultiCfIterator(ReadOptions(), cfhs);
    for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
      ASSERT_EQ(expected_keys[i], iter->key());
      ASSERT_EQ(expected_values[i], iter->value());
      ++i;
    }
    ASSERT_EQ(expected_keys.size(), i);
  };

  {
    // Case 1: Unique key per CF
    CreateAndReopenWithCF({"cf_1", "cf_2", "cf_3"}, options);

    ASSERT_OK(Put(0, "key_1", "key_1_cf_0_val"));
    ASSERT_OK(Put(1, "key_2", "key_2_cf_1_val"));
    ASSERT_OK(Put(2, "key_3", "key_3_cf_2_val"));
    ASSERT_OK(Put(3, "key_4", "key_4_cf_3_val"));

    std::vector<Slice> expected_keys = {"key_1", "key_2", "key_3", "key_4"};
    std::vector<Slice> expected_values = {"key_1_cf_0_val", "key_2_cf_1_val",
                                          "key_3_cf_2_val", "key_4_cf_3_val"};

    // Test for iteration over CF default->1->2->3
    std::vector<ColumnFamilyHandle*> cfhs_order_0_1_2_3 = {
        handles_[0], handles_[1], handles_[2], handles_[3]};
    verify(cfhs_order_0_1_2_3, expected_keys, expected_values);

    // Test for iteration over CF 3->1->default_cf->2
    std::vector<ColumnFamilyHandle*> cfhs_order_3_1_0_2 = {
        handles_[3], handles_[1], handles_[0], handles_[2]};
    // Iteration order and the return values should be the same since keys are
    // unique per CF
    verify(cfhs_order_3_1_0_2, expected_keys, expected_values);
  }
  {
    // Case 2: Same key in multiple CFs
    options = CurrentOptions(options);
    DestroyAndReopen(options);
    CreateAndReopenWithCF({"cf_1", "cf_2", "cf_3"}, options);

    ASSERT_OK(Put(0, "key_1", "key_1_cf_0_val"));
    ASSERT_OK(Put(3, "key_1", "key_1_cf_3_val"));
    ASSERT_OK(Put(1, "key_2", "key_2_cf_1_val"));
    ASSERT_OK(Put(2, "key_2", "key_2_cf_2_val"));
    ASSERT_OK(Put(0, "key_3", "key_3_cf_0_val"));
    ASSERT_OK(Put(1, "key_3", "key_3_cf_1_val"));
    ASSERT_OK(Put(3, "key_3", "key_3_cf_3_val"));

    std::vector<Slice> expected_keys = {"key_1", "key_2", "key_3"};

    // Test for iteration over CFs default->1->2->3
    std::vector<ColumnFamilyHandle*> cfhs_order_0_1_2_3 = {
        handles_[0], handles_[1], handles_[2], handles_[3]};
    std::vector<Slice> expected_values = {"key_1_cf_0_val", "key_2_cf_1_val",
                                          "key_3_cf_0_val"};
    verify(cfhs_order_0_1_2_3, expected_keys, expected_values);

    // Test for iteration over CFs 3->2->default_cf->1
    std::vector<ColumnFamilyHandle*> cfhs_order_3_2_0_1 = {
        handles_[3], handles_[2], handles_[0], handles_[1]};
    expected_values = {"key_1_cf_3_val", "key_2_cf_2_val", "key_3_cf_3_val"};
    verify(cfhs_order_3_2_0_1, expected_keys, expected_values);
  }
}

TEST_F(MultiCfIteratorTest, DISABLED_IterateAttributeGroups) {
  // Set up the DB and Column Families
  Options options = GetDefaultOptions();
  CreateAndReopenWithCF({"cf_1", "cf_2", "cf_3"}, options);

  constexpr char key_1[] = "key_1";
  WideColumns key_1_columns_in_cf_2{
      {kDefaultWideColumnName, "cf_2_col_val_0_key_1"},
      {"cf_2_col_name_1", "cf_2_col_val_1_key_1"},
      {"cf_2_col_name_2", "cf_2_col_val_2_key_1"}};
  WideColumns key_1_columns_in_cf_3{
      {"cf_3_col_name_1", "cf_3_col_val_1_key_1"},
      {"cf_3_col_name_2", "cf_3_col_val_2_key_1"},
      {"cf_3_col_name_3", "cf_3_col_val_3_key_1"}};

  constexpr char key_2[] = "key_2";
  WideColumns key_2_columns_in_cf_1{
      {"cf_1_col_name_1", "cf_1_col_val_1_key_2"}};
  WideColumns key_2_columns_in_cf_2{
      {"cf_2_col_name_1", "cf_2_col_val_1_key_2"},
      {"cf_2_col_name_2", "cf_2_col_val_2_key_2"}};

  constexpr char key_3[] = "key_3";
  WideColumns key_3_columns_in_cf_1{
      {"cf_1_col_name_1", "cf_1_col_val_1_key_3"}};
  WideColumns key_3_columns_in_cf_3{
      {"cf_3_col_name_1", "cf_3_col_val_1_key_3"}};

  constexpr char key_4[] = "key_4";
  WideColumns key_4_columns_in_cf_0{
      {"cf_0_col_name_1", "cf_0_col_val_1_key_4"}};
  WideColumns key_4_columns_in_cf_2{
      {"cf_2_col_name_1", "cf_2_col_val_1_key_4"}};

  AttributeGroups key_1_attribute_groups{
      AttributeGroup(handles_[2], key_1_columns_in_cf_2),
      AttributeGroup(handles_[3], key_1_columns_in_cf_3)};
  AttributeGroups key_2_attribute_groups{
      AttributeGroup(handles_[1], key_2_columns_in_cf_1),
      AttributeGroup(handles_[2], key_2_columns_in_cf_2)};
  AttributeGroups key_3_attribute_groups{
      AttributeGroup(handles_[1], key_3_columns_in_cf_1),
      AttributeGroup(handles_[3], key_3_columns_in_cf_3)};
  AttributeGroups key_4_attribute_groups{
      AttributeGroup(handles_[0], key_4_columns_in_cf_0),
      AttributeGroup(handles_[2], key_4_columns_in_cf_2)};

  ASSERT_OK(db_->PutEntity(WriteOptions(), key_1, key_1_attribute_groups));
  ASSERT_OK(db_->PutEntity(WriteOptions(), key_2, key_2_attribute_groups));
  ASSERT_OK(db_->PutEntity(WriteOptions(), key_3, key_3_attribute_groups));
  ASSERT_OK(db_->PutEntity(WriteOptions(), key_4, key_4_attribute_groups));

  auto verify =
      [&](const std::vector<ColumnFamilyHandle*>& cfhs,
          const std::vector<Slice>& expected_keys,
          const std::vector<Slice>& expected_values,
          const std::vector<WideColumns>& expected_wide_columns,
          const std::vector<AttributeGroups>& expected_attribute_groups) {
        int i = 0;
        std::unique_ptr<MultiCfIterator> iter =
            db_->NewMultiCfIterator(ReadOptions(), cfhs);
        for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
          ASSERT_EQ(expected_keys[i], iter->key());
          ASSERT_EQ(expected_values[i], iter->value());
          ASSERT_EQ(expected_wide_columns[i], iter->columns());
          ASSERT_EQ(expected_attribute_groups[i], iter->attribute_groups());
          ++i;
        }
        // TODO - Check when implementation is done
        // ASSERT_EQ(expected_keys.size(), i);
      };

  // Test for iteration over CF default->1->2->3
  std::vector<ColumnFamilyHandle*> cfhs_order_0_1_2_3 = {
      handles_[0], handles_[1], handles_[2], handles_[3]};
  std::vector<Slice> expected_keys = {key_1, key_2, key_3, key_4};
  // Pick what DBIter would return for value() in the first CF that key exists
  // Since value for kDefaultWideColumnName only exists for key_1, rest will
  // return empty value
  std::vector<Slice> expected_values = {"cf_2_col_val_0_key_1", "", "", ""};

  // Merge columns from all CFs that key exists and value is stored as wide
  // column
  std::vector<WideColumns> expected_wide_columns = {
      {{kDefaultWideColumnName, "cf_2_col_val_0_key_1"},
       {"cf_2_col_name_1", "cf_2_col_val_1_key_1"},
       {"cf_2_col_name_2", "cf_2_col_val_2_key_1"},
       {"cf_3_col_name_1", "cf_3_col_val_1_key_1"},
       {"cf_3_col_name_2", "cf_3_col_val_2_key_1"},
       {"cf_3_col_name_3", "cf_3_col_val_3_key_1"}},
      {{"cf_1_col_name_1", "cf_1_col_val_1_key_2"},
       {"cf_2_col_name_1", "cf_2_col_val_1_key_2"},
       {"cf_2_col_name_2", "cf_2_col_val_2_key_2"}},
      {{"cf_1_col_name_1", "cf_1_col_val_1_key_3"},
       {"cf_3_col_name_1", "cf_3_col_val_1_key_3"}},
      {{"cf_0_col_name_1", "cf_0_col_val_1_key_4"},
       {"cf_2_col_name_1", "cf_2_col_val_1_key_4"}}};
  std::vector<AttributeGroups> expected_attribute_groups = {
      key_1_attribute_groups, key_2_attribute_groups, key_3_attribute_groups,
      key_4_attribute_groups};
  verify(cfhs_order_0_1_2_3, expected_keys, expected_values,
         expected_wide_columns, expected_attribute_groups);
}

}  // namespace ROCKSDB_NAMESPACE

int main(int argc, char** argv) {
  ROCKSDB_NAMESPACE::port::InstallStackTraceHandler();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
