//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef ROCKSDB_LITE

#include "rocksdb/utilities/write_batch_with_index.h"
#include <map>
#include <memory>
#include "db/column_family.h"
#include "port/stack_trace.h"
#include "test_util/testharness.h"
#include "util/random.h"
#include "util/string_util.h"
#include "utilities/merge_operators.h"
#include "utilities/merge_operators/string_append/stringappend.h"

namespace ROCKSDB_NAMESPACE {

namespace {
class ColumnFamilyHandleImplDummy : public ColumnFamilyHandleImpl {
 public:
  explicit ColumnFamilyHandleImplDummy(int id, const Comparator* comparator)
      : ColumnFamilyHandleImpl(nullptr, nullptr, nullptr),
        id_(id),
        comparator_(comparator) {}
  uint32_t GetID() const override { return id_; }
  const Comparator* GetComparator() const override { return comparator_; }

 private:
  uint32_t id_;
  const Comparator* comparator_;
};

struct Entry {
  std::string key;
  std::string value;
  WriteType type;
};

struct TestHandler : public WriteBatch::Handler {
  std::map<uint32_t, std::vector<Entry>> seen;
  Status PutCF(uint32_t column_family_id, const Slice& key,
               const Slice& value) override {
    Entry e;
    e.key = key.ToString();
    e.value = value.ToString();
    e.type = kPutRecord;
    seen[column_family_id].push_back(e);
    return Status::OK();
  }
  Status MergeCF(uint32_t column_family_id, const Slice& key,
                 const Slice& value) override {
    Entry e;
    e.key = key.ToString();
    e.value = value.ToString();
    e.type = kMergeRecord;
    seen[column_family_id].push_back(e);
    return Status::OK();
  }
  void LogData(const Slice& /*blob*/) override {}
  Status DeleteCF(uint32_t column_family_id, const Slice& key) override {
    Entry e;
    e.key = key.ToString();
    e.value = "";
    e.type = kDeleteRecord;
    seen[column_family_id].push_back(e);
    return Status::OK();
  }
};
}  // namespace anonymous

class WriteBatchWithIndexTest : public testing::Test {};

void TestValueAsSecondaryIndexHelper(std::vector<Entry> entries,
                                     WriteBatchWithIndex* batch) {
  // In this test, we insert <key, value> to column family `data`, and
  // <value, key> to column family `index`. Then iterator them in order
  // and seek them by key.

  // Sort entries by key
  std::map<std::string, std::vector<Entry*>> data_map;
  // Sort entries by value
  std::map<std::string, std::vector<Entry*>> index_map;
  for (auto& e : entries) {
    data_map[e.key].push_back(&e);
    index_map[e.value].push_back(&e);
  }

  ColumnFamilyHandleImplDummy data(6, BytewiseComparator());
  ColumnFamilyHandleImplDummy index(8, BytewiseComparator());
  for (auto& e : entries) {
    if (e.type == kPutRecord) {
      ASSERT_OK(batch->Put(&data, e.key, e.value));
      ASSERT_OK(batch->Put(&index, e.value, e.key));
    } else if (e.type == kMergeRecord) {
      ASSERT_OK(batch->Merge(&data, e.key, e.value));
      ASSERT_OK(batch->Put(&index, e.value, e.key));
    } else {
      assert(e.type == kDeleteRecord);
      std::unique_ptr<WBWIIterator> iter(batch->NewIterator(&data));
      iter->Seek(e.key);
      ASSERT_OK(iter->status());
      auto write_entry = iter->Entry();
      ASSERT_EQ(e.key, write_entry.key.ToString());
      ASSERT_EQ(e.value, write_entry.value.ToString());
      ASSERT_OK(batch->Delete(&data, e.key));
      ASSERT_OK(batch->Put(&index, e.value, ""));
    }
  }

  // Iterator all keys
  {
    std::unique_ptr<WBWIIterator> iter(batch->NewIterator(&data));
    for (int seek_to_first : {0, 1}) {
      if (seek_to_first) {
        iter->SeekToFirst();
      } else {
        iter->Seek("");
      }
      for (auto pair : data_map) {
        for (auto v : pair.second) {
          ASSERT_OK(iter->status());
          ASSERT_TRUE(iter->Valid());
          auto write_entry = iter->Entry();
          ASSERT_EQ(pair.first, write_entry.key.ToString());
          ASSERT_EQ(v->type, write_entry.type);
          if (write_entry.type != kDeleteRecord) {
            ASSERT_EQ(v->value, write_entry.value.ToString());
          }
          iter->Next();
        }
      }
      ASSERT_TRUE(!iter->Valid());
    }
    iter->SeekToLast();
    for (auto pair = data_map.rbegin(); pair != data_map.rend(); ++pair) {
      for (auto v = pair->second.rbegin(); v != pair->second.rend(); v++) {
        ASSERT_OK(iter->status());
        ASSERT_TRUE(iter->Valid());
        auto write_entry = iter->Entry();
        ASSERT_EQ(pair->first, write_entry.key.ToString());
        ASSERT_EQ((*v)->type, write_entry.type);
        if (write_entry.type != kDeleteRecord) {
          ASSERT_EQ((*v)->value, write_entry.value.ToString());
        }
        iter->Prev();
      }
    }
    ASSERT_TRUE(!iter->Valid());
  }

  // Iterator all indexes
  {
    std::unique_ptr<WBWIIterator> iter(batch->NewIterator(&index));
    for (int seek_to_first : {0, 1}) {
      if (seek_to_first) {
        iter->SeekToFirst();
      } else {
        iter->Seek("");
      }
      for (auto pair : index_map) {
        for (auto v : pair.second) {
          ASSERT_OK(iter->status());
          ASSERT_TRUE(iter->Valid());
          auto write_entry = iter->Entry();
          ASSERT_EQ(pair.first, write_entry.key.ToString());
          if (v->type != kDeleteRecord) {
            ASSERT_EQ(v->key, write_entry.value.ToString());
            ASSERT_EQ(v->value, write_entry.key.ToString());
          }
          iter->Next();
        }
      }
      ASSERT_TRUE(!iter->Valid());
    }

    iter->SeekToLast();
    for (auto pair = index_map.rbegin(); pair != index_map.rend(); ++pair) {
      for (auto v = pair->second.rbegin(); v != pair->second.rend(); v++) {
        ASSERT_OK(iter->status());
        ASSERT_TRUE(iter->Valid());
        auto write_entry = iter->Entry();
        ASSERT_EQ(pair->first, write_entry.key.ToString());
        if ((*v)->type != kDeleteRecord) {
          ASSERT_EQ((*v)->key, write_entry.value.ToString());
          ASSERT_EQ((*v)->value, write_entry.key.ToString());
        }
        iter->Prev();
      }
    }
    ASSERT_TRUE(!iter->Valid());
  }

  // Seek to every key
  {
    std::unique_ptr<WBWIIterator> iter(batch->NewIterator(&data));

    // Seek the keys one by one in reverse order
    for (auto pair = data_map.rbegin(); pair != data_map.rend(); ++pair) {
      iter->Seek(pair->first);
      ASSERT_OK(iter->status());
      for (auto v : pair->second) {
        ASSERT_TRUE(iter->Valid());
        auto write_entry = iter->Entry();
        ASSERT_EQ(pair->first, write_entry.key.ToString());
        ASSERT_EQ(v->type, write_entry.type);
        if (write_entry.type != kDeleteRecord) {
          ASSERT_EQ(v->value, write_entry.value.ToString());
        }
        iter->Next();
        ASSERT_OK(iter->status());
      }
    }
  }

  // Seek to every index
  {
    std::unique_ptr<WBWIIterator> iter(batch->NewIterator(&index));

    // Seek the keys one by one in reverse order
    for (auto pair = index_map.rbegin(); pair != index_map.rend(); ++pair) {
      iter->Seek(pair->first);
      ASSERT_OK(iter->status());
      for (auto v : pair->second) {
        ASSERT_TRUE(iter->Valid());
        auto write_entry = iter->Entry();
        ASSERT_EQ(pair->first, write_entry.key.ToString());
        ASSERT_EQ(v->value, write_entry.key.ToString());
        if (v->type != kDeleteRecord) {
          ASSERT_EQ(v->key, write_entry.value.ToString());
        }
        iter->Next();
        ASSERT_OK(iter->status());
      }
    }
  }

  // Verify WriteBatch can be iterated
  TestHandler handler;
  ASSERT_OK(batch->GetWriteBatch()->Iterate(&handler));

  // Verify data column family
  {
    ASSERT_EQ(entries.size(), handler.seen[data.GetID()].size());
    size_t i = 0;
    for (auto e : handler.seen[data.GetID()]) {
      auto write_entry = entries[i++];
      ASSERT_EQ(e.type, write_entry.type);
      ASSERT_EQ(e.key, write_entry.key);
      if (e.type != kDeleteRecord) {
        ASSERT_EQ(e.value, write_entry.value);
      }
    }
  }

  // Verify index column family
  {
    ASSERT_EQ(entries.size(), handler.seen[index.GetID()].size());
    size_t i = 0;
    for (auto e : handler.seen[index.GetID()]) {
      auto write_entry = entries[i++];
      ASSERT_EQ(e.key, write_entry.value);
      if (write_entry.type != kDeleteRecord) {
        ASSERT_EQ(e.value, write_entry.key);
      }
    }
  }
}

TEST_F(WriteBatchWithIndexTest, TestValueAsSecondaryIndex) {
  Entry entries[] = {
      {"aaa", "0005", kPutRecord},
      {"b", "0002", kPutRecord},
      {"cdd", "0002", kMergeRecord},
      {"aab", "00001", kPutRecord},
      {"cc", "00005", kPutRecord},
      {"cdd", "0002", kPutRecord},
      {"aab", "0003", kPutRecord},
      {"cc", "00005", kDeleteRecord},
  };
  std::vector<Entry> entries_list(entries, entries + 8);

  WriteBatchWithIndex batch(nullptr, 20);

  TestValueAsSecondaryIndexHelper(entries_list, &batch);

  // Clear batch and re-run test with new values
  batch.Clear();

  Entry new_entries[] = {
      {"aaa", "0005", kPutRecord},
      {"e", "0002", kPutRecord},
      {"add", "0002", kMergeRecord},
      {"aab", "00001", kPutRecord},
      {"zz", "00005", kPutRecord},
      {"add", "0002", kPutRecord},
      {"aab", "0003", kPutRecord},
      {"zz", "00005", kDeleteRecord},
  };

  entries_list = std::vector<Entry>(new_entries, new_entries + 8);

  TestValueAsSecondaryIndexHelper(entries_list, &batch);
}

TEST_F(WriteBatchWithIndexTest, TestComparatorForCF) {
  ColumnFamilyHandleImplDummy cf1(6, nullptr);
  ColumnFamilyHandleImplDummy reverse_cf(66, ReverseBytewiseComparator());
  ColumnFamilyHandleImplDummy cf2(88, BytewiseComparator());
  WriteBatchWithIndex batch(BytewiseComparator(), 20);

  ASSERT_OK(batch.Put(&cf1, "ddd", ""));
  ASSERT_OK(batch.Put(&cf2, "aaa", ""));
  ASSERT_OK(batch.Put(&cf2, "eee", ""));
  ASSERT_OK(batch.Put(&cf1, "ccc", ""));
  ASSERT_OK(batch.Put(&reverse_cf, "a11", ""));
  ASSERT_OK(batch.Put(&cf1, "bbb", ""));

  Slice key_slices[] = {"a", "3", "3"};
  Slice value_slice = "";
  ASSERT_OK(batch.Put(&reverse_cf, SliceParts(key_slices, 3),
                      SliceParts(&value_slice, 1)));
  ASSERT_OK(batch.Put(&reverse_cf, "a22", ""));

  {
    std::unique_ptr<WBWIIterator> iter(batch.NewIterator(&cf1));
    iter->Seek("");
    ASSERT_OK(iter->status());
    ASSERT_TRUE(iter->Valid());
    ASSERT_EQ("bbb", iter->Entry().key.ToString());
    iter->Next();
    ASSERT_OK(iter->status());
    ASSERT_TRUE(iter->Valid());
    ASSERT_EQ("ccc", iter->Entry().key.ToString());
    iter->Next();
    ASSERT_OK(iter->status());
    ASSERT_TRUE(iter->Valid());
    ASSERT_EQ("ddd", iter->Entry().key.ToString());
    iter->Next();
    ASSERT_OK(iter->status());
    ASSERT_TRUE(!iter->Valid());
  }

  {
    std::unique_ptr<WBWIIterator> iter(batch.NewIterator(&cf2));
    iter->Seek("");
    ASSERT_OK(iter->status());
    ASSERT_TRUE(iter->Valid());
    ASSERT_EQ("aaa", iter->Entry().key.ToString());
    iter->Next();
    ASSERT_OK(iter->status());
    ASSERT_TRUE(iter->Valid());
    ASSERT_EQ("eee", iter->Entry().key.ToString());
    iter->Next();
    ASSERT_OK(iter->status());
    ASSERT_TRUE(!iter->Valid());
  }

  {
    std::unique_ptr<WBWIIterator> iter(batch.NewIterator(&reverse_cf));
    iter->Seek("");
    ASSERT_OK(iter->status());
    ASSERT_TRUE(!iter->Valid());

    iter->Seek("z");
    ASSERT_OK(iter->status());
    ASSERT_TRUE(iter->Valid());
    ASSERT_EQ("a33", iter->Entry().key.ToString());
    iter->Next();
    ASSERT_OK(iter->status());
    ASSERT_TRUE(iter->Valid());
    ASSERT_EQ("a22", iter->Entry().key.ToString());
    iter->Next();
    ASSERT_OK(iter->status());
    ASSERT_TRUE(iter->Valid());
    ASSERT_EQ("a11", iter->Entry().key.ToString());
    iter->Next();
    ASSERT_OK(iter->status());
    ASSERT_TRUE(!iter->Valid());

    iter->Seek("a22");
    ASSERT_OK(iter->status());
    ASSERT_TRUE(iter->Valid());
    ASSERT_EQ("a22", iter->Entry().key.ToString());

    iter->Seek("a13");
    ASSERT_OK(iter->status());
    ASSERT_TRUE(iter->Valid());
    ASSERT_EQ("a11", iter->Entry().key.ToString());
  }
}

TEST_F(WriteBatchWithIndexTest, TestOverwriteKey) {
  ColumnFamilyHandleImplDummy cf1(6, nullptr);
  ColumnFamilyHandleImplDummy reverse_cf(66, ReverseBytewiseComparator());
  ColumnFamilyHandleImplDummy cf2(88, BytewiseComparator());
  WriteBatchWithIndex batch(BytewiseComparator(), 20, true);

  ASSERT_OK(batch.Put(&cf1, "ddd", ""));
  ASSERT_OK(batch.Merge(&cf1, "ddd", ""));
  ASSERT_OK(batch.Delete(&cf1, "ddd"));
  ASSERT_OK(batch.Put(&cf2, "aaa", ""));
  ASSERT_OK(batch.Delete(&cf2, "aaa"));
  ASSERT_OK(batch.Put(&cf2, "aaa", "aaa"));
  ASSERT_OK(batch.Put(&cf2, "eee", "eee"));
  ASSERT_OK(batch.Put(&cf1, "ccc", ""));
  ASSERT_OK(batch.Put(&reverse_cf, "a11", ""));
  ASSERT_OK(batch.Delete(&cf1, "ccc"));
  ASSERT_OK(batch.Put(&reverse_cf, "a33", "a33"));
  ASSERT_OK(batch.Put(&reverse_cf, "a11", "a11"));
  Slice slices[] = {"a", "3", "3"};
  ASSERT_OK(batch.Delete(&reverse_cf, SliceParts(slices, 3)));

  {
    std::unique_ptr<WBWIIterator> iter(batch.NewIterator(&cf1));
    iter->Seek("");
    ASSERT_OK(iter->status());
    ASSERT_TRUE(iter->Valid());
    ASSERT_EQ("ccc", iter->Entry().key.ToString());
    ASSERT_TRUE(iter->Entry().type == WriteType::kDeleteRecord);
    iter->Next();
    ASSERT_OK(iter->status());
    ASSERT_TRUE(iter->Valid());
    ASSERT_EQ("ddd", iter->Entry().key.ToString());
    ASSERT_TRUE(iter->Entry().type == WriteType::kDeleteRecord);
    iter->Next();
    ASSERT_OK(iter->status());
    ASSERT_TRUE(!iter->Valid());
  }

  {
    std::unique_ptr<WBWIIterator> iter(batch.NewIterator(&cf2));
    iter->SeekToLast();
    ASSERT_OK(iter->status());
    ASSERT_TRUE(iter->Valid());
    ASSERT_EQ("eee", iter->Entry().key.ToString());
    ASSERT_EQ("eee", iter->Entry().value.ToString());
    iter->Prev();
    ASSERT_OK(iter->status());
    ASSERT_TRUE(iter->Valid());
    ASSERT_EQ("aaa", iter->Entry().key.ToString());
    ASSERT_EQ("aaa", iter->Entry().value.ToString());
    iter->Prev();
    ASSERT_OK(iter->status());
    ASSERT_TRUE(!iter->Valid());

    iter->SeekToFirst();
    ASSERT_OK(iter->status());
    ASSERT_TRUE(iter->Valid());
    ASSERT_EQ("aaa", iter->Entry().key.ToString());
    ASSERT_EQ("aaa", iter->Entry().value.ToString());
    iter->Next();
    ASSERT_OK(iter->status());
    ASSERT_TRUE(iter->Valid());
    ASSERT_EQ("eee", iter->Entry().key.ToString());
    ASSERT_EQ("eee", iter->Entry().value.ToString());
    iter->Next();
    ASSERT_OK(iter->status());
    ASSERT_TRUE(!iter->Valid());
  }

  {
    std::unique_ptr<WBWIIterator> iter(batch.NewIterator(&reverse_cf));
    iter->Seek("");
    ASSERT_OK(iter->status());
    ASSERT_TRUE(!iter->Valid());

    iter->Seek("z");
    ASSERT_OK(iter->status());
    ASSERT_TRUE(iter->Valid());
    ASSERT_EQ("a33", iter->Entry().key.ToString());
    ASSERT_TRUE(iter->Entry().type == WriteType::kDeleteRecord);
    iter->Next();
    ASSERT_OK(iter->status());
    ASSERT_TRUE(iter->Valid());
    ASSERT_EQ("a11", iter->Entry().key.ToString());
    ASSERT_EQ("a11", iter->Entry().value.ToString());
    iter->Next();
    ASSERT_OK(iter->status());
    ASSERT_TRUE(!iter->Valid());

    iter->SeekToLast();
    ASSERT_TRUE(iter->Valid());
    ASSERT_EQ("a11", iter->Entry().key.ToString());
    ASSERT_EQ("a11", iter->Entry().value.ToString());
    iter->Prev();

    ASSERT_OK(iter->status());
    ASSERT_TRUE(iter->Valid());
    ASSERT_EQ("a33", iter->Entry().key.ToString());
    ASSERT_TRUE(iter->Entry().type == WriteType::kDeleteRecord);
    iter->Prev();
    ASSERT_TRUE(!iter->Valid());
  }
}

namespace {
typedef std::map<std::string, std::string> KVMap;

class KVIter : public Iterator {
 public:
  explicit KVIter(const KVMap* map, const Comparator* comparator = nullptr,
                  ReadOptions* read_options = nullptr)
      : map_(map),
        iter_(map_->end()),
        comparator_(comparator),
        read_options_(read_options) {}
  bool Valid() const override {
    return iter_ != map_->end() && IsWithinBounds();
  }
  void SeekToFirst() override { iter_ = map_->begin(); }
  void SeekToLast() override {
    if (map_->empty()) {
      iter_ = map_->end();
    } else {
      if (read_options_ != nullptr &&
          read_options_->iterate_upper_bound != nullptr) {
        // we can seek to before the iterate_upper_bound

        // NOTE: std::map::lower_bound is equivalent to RocksDB's
        // `iterate_upper_bound`
        iter_ =
            map_->lower_bound(read_options_->iterate_upper_bound->ToString());
        if (iter_ != map_->begin()) {
          // lower_bound gives us the first element not
          // less than the `iterate_upper_bound` so we have
          // to move back one, unless we are already at the beginning of the map
          iter_--;
        }
      } else {
        iter_ = map_->find(map_->rbegin()->first);
      }
    }
  }
  void Seek(const Slice& k) override {
    iter_ = map_->lower_bound(k.ToString());
  }
  void SeekForPrev(const Slice& k) override {
    iter_ = map_->upper_bound(k.ToString());
    Prev();
  }
  void Next() override { ++iter_; }
  void Prev() override {
    if (iter_ == map_->begin()) {
      iter_ = map_->end();
      return;
    }
    --iter_;
  }

  Slice key() const override {
    assert(Valid());
    return iter_->first;
  }
  Slice value() const override { return iter_->second; }
  Status status() const override { return Status::OK(); }

  bool check_lower_bound() const override {
    return read_options_ != nullptr &&
           read_options_->iterate_lower_bound != nullptr;
  }

  const Slice* iterate_lower_bound() const override {
    if (check_lower_bound()) {
      return read_options_->iterate_lower_bound;
    }
    return nullptr;
  }

  bool check_upper_bound() const override {
    return read_options_ != nullptr &&
           read_options_->iterate_upper_bound != nullptr;
  }

  const Slice* iterate_upper_bound() const override {
    if (check_upper_bound()) {
      return read_options_->iterate_upper_bound;
    }
    return nullptr;
  }

 private:
  const KVMap* const map_;
  KVMap::const_iterator iter_;
  const Comparator* comparator_;
  const ReadOptions* read_options_;

  bool IsWithinBounds() const {
    if (read_options_ != nullptr) {
      // TODO(AR) should this only be used when moving backward?
      if (read_options_->iterate_lower_bound != nullptr) {
        return comparator_->Compare(iter_->first,
                                    *(read_options_->iterate_lower_bound)) >= 0;
      }

      // TODO(AR) should this only be used when moving forward?
      if (read_options_->iterate_upper_bound != nullptr) {
        return comparator_->Compare(iter_->first,
                                    *(read_options_->iterate_upper_bound)) < 0;
      }
    }

    return true;
  }
};

::testing::AssertionResult IterEquals(Iterator* iter, const std::string& key,
                                      const std::string& value) {
  auto s = iter->status();
  if (!s.ok()) {
    return ::testing::AssertionFailure()
           << "Iterator NOT OK; status is: " << s.ToString();
  }

  if (!iter->Valid()) {
    return ::testing::AssertionFailure() << "Iterator is invalid";
  }

  if (key != iter->key()) {
    return ::testing::AssertionFailure()
           << "Iterator::key(): '" << iter->key().ToString(false)
           << "' is not equal to '" << key << "'";
  }

  if (value != iter->value()) {
    return ::testing::AssertionFailure()
           << "Iterator::value(): '" << iter->value().ToString(false)
           << "' is not equal to '" << value << "'";
  }

  return ::testing::AssertionSuccess();
}

void AssertItersEqual(Iterator* iter1, Iterator* iter2) {
  ASSERT_EQ(iter1->Valid(), iter2->Valid());
  if (iter1->Valid()) {
    ASSERT_EQ(iter1->key().ToString(), iter2->key().ToString());
    ASSERT_EQ(iter1->value().ToString(), iter2->value().ToString());
  }
}
}  // namespace

TEST_F(WriteBatchWithIndexTest, TestRandomIteraratorWithBase) {
  std::vector<std::string> source_strings = {"a", "b", "c", "d", "e",
                                             "f", "g", "h", "i", "j"};
  for (int rand_seed = 301; rand_seed < 366; rand_seed++) {
    Random rnd(rand_seed);

    ColumnFamilyHandleImplDummy cf1(6, BytewiseComparator());
    ColumnFamilyHandleImplDummy cf2(2, BytewiseComparator());
    ColumnFamilyHandleImplDummy cf3(8, BytewiseComparator());

    WriteBatchWithIndex batch(BytewiseComparator(), 20, true);

    if (rand_seed % 2 == 0) {
      ASSERT_OK(batch.Put(&cf2, "zoo", "bar"));
    }
    if (rand_seed % 4 == 1) {
      ASSERT_OK(batch.Put(&cf3, "zoo", "bar"));
    }

    KVMap map;
    KVMap merged_map;
    for (auto key : source_strings) {
      std::string value = key + key;
      int type = rnd.Uniform(6);
      switch (type) {
        case 0:
          // only base has it
          map[key] = value;
          merged_map[key] = value;
          break;
        case 1:
          // only delta has it
          ASSERT_OK(batch.Put(&cf1, key, value));
          map[key] = value;
          merged_map[key] = value;
          break;
        case 2:
          // both has it. Delta should win
          ASSERT_OK(batch.Put(&cf1, key, value));
          map[key] = "wrong_value";
          merged_map[key] = value;
          break;
        case 3:
          // both has it. Delta is delete
          ASSERT_OK(batch.Delete(&cf1, key));
          map[key] = "wrong_value";
          break;
        case 4:
          // only delta has it. Delta is delete
          ASSERT_OK(batch.Delete(&cf1, key));
          map[key] = "wrong_value";
          break;
        default:
          // Neither iterator has it.
          break;
      }
    }

    std::unique_ptr<Iterator> iter(
        batch.NewIteratorWithBase(&cf1, new KVIter(&map)));
    std::unique_ptr<Iterator> result_iter(new KVIter(&merged_map));

    bool is_valid = false;
    for (int i = 0; i < 128; i++) {
      // Random walk and make sure iter and result_iter returns the
      // same key and value
      int type = rnd.Uniform(6);
      ASSERT_OK(iter->status());
      switch (type) {
        case 0:
          // Seek to First
          iter->SeekToFirst();
          result_iter->SeekToFirst();
          break;
        case 1:
          // Seek to last
          iter->SeekToLast();
          result_iter->SeekToLast();
          break;
        case 2: {
          // Seek to random key
          auto key_idx = rnd.Uniform(static_cast<int>(source_strings.size()));
          auto key = source_strings[key_idx];
          iter->Seek(key);
          result_iter->Seek(key);
          break;
        }
        case 3: {
          // SeekForPrev to random key
          auto key_idx = rnd.Uniform(static_cast<int>(source_strings.size()));
          auto key = source_strings[key_idx];
          iter->SeekForPrev(key);
          result_iter->SeekForPrev(key);
          break;
        }
        case 4:
          // Next
          if (is_valid) {
            iter->Next();
            result_iter->Next();
          } else {
            continue;
          }
          break;
        default:
          assert(type == 5);
          // Prev
          if (is_valid) {
            iter->Prev();
            result_iter->Prev();
          } else {
            continue;
          }
          break;
      }
      AssertItersEqual(iter.get(), result_iter.get());
      is_valid = iter->Valid();
    }

    ASSERT_OK(iter->status());
  }
}

TEST_F(WriteBatchWithIndexTest, TestIteraratorWithBaseBatchEmpty) {
  ColumnFamilyHandleImplDummy cf1(6, BytewiseComparator());
  ColumnFamilyHandleImplDummy cf2(2, BytewiseComparator());
  WriteBatchWithIndex batch(BytewiseComparator(), 20, true);

  KVMap map;
  map["a"] = "aa";
  map["c"] = "cc";
  map["e"] = "ee";
  std::unique_ptr<Iterator> iter(
      batch.NewIteratorWithBase(&cf1, new KVIter(&map)));

  iter->SeekToFirst();
  ASSERT_TRUE(IterEquals(iter.get(), "a", "aa"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "c", "cc"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "e", "ee"));
  iter->Next();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(!iter->Valid());

  iter->SeekToLast();
  ASSERT_TRUE(IterEquals(iter.get(), "e", "ee"));
  iter->Prev();
  ASSERT_TRUE(IterEquals(iter.get(), "c", "cc"));
  iter->Prev();
  ASSERT_TRUE(IterEquals(iter.get(), "a", "aa"));
  iter->Prev();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("b");
  ASSERT_TRUE(IterEquals(iter.get(), "c", "cc"));

  iter->Prev();
  ASSERT_TRUE(IterEquals(iter.get(), "a", "aa"));

  iter->Seek("a");
  ASSERT_TRUE(IterEquals(iter.get(), "a", "aa"));
}

TEST_F(WriteBatchWithIndexTest, TestIteraratorWithBaseBatchOne) {
  ColumnFamilyHandleImplDummy cf1(6, BytewiseComparator());
  ColumnFamilyHandleImplDummy cf2(2, BytewiseComparator());
  WriteBatchWithIndex batch(BytewiseComparator(), 20, true);

  // Test the case that there is one element in the write batch for each cf
  ASSERT_OK(batch.Put(&cf1, "a", "aa"));
  ASSERT_OK(batch.Put(&cf2, "zoo", "bar"));
  KVMap empty_map;
  std::unique_ptr<Iterator> iter(
      batch.NewIteratorWithBase(&cf1, new KVIter(&empty_map)));

  iter->SeekToFirst();
  ASSERT_TRUE(IterEquals(iter.get(), "a", "aa"));
  iter->Next();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(!iter->Valid());
}

TEST_F(WriteBatchWithIndexTest, TestIteraratorWithBaseBatchInterleaved) {
  ColumnFamilyHandleImplDummy cf1(6, BytewiseComparator());
  ColumnFamilyHandleImplDummy cf2(2, BytewiseComparator());
  WriteBatchWithIndex batch(BytewiseComparator(), 20, true);

  ASSERT_OK(batch.Put(&cf1, "a", "aa"));
  ASSERT_OK(batch.Put(&cf2, "zoo", "bar"));  // note this is cf2!
  ASSERT_OK(batch.Delete(&cf1, "b"));
  ASSERT_OK(batch.Put(&cf1, "c", "cc"));
  ASSERT_OK(batch.Put(&cf1, "d", "dd"));
  ASSERT_OK(batch.Delete(&cf1, "e"));

  /* At this point batch/cf1 should contain:
    a -> aa
    c -> cc
    d -> dd
  */

  KVMap map;
  map["b"] = "";
  map["cc"] = "cccc";
  map["f"] = "ff";
  std::unique_ptr<Iterator> iter(
      batch.NewIteratorWithBase(&cf1, new KVIter(&map)));

  iter->SeekToFirst();
  ASSERT_TRUE(IterEquals(iter.get(), "a", "aa"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "c", "cc"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "cc", "cccc"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "d", "dd"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "f", "ff"));
  iter->Next();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(!iter->Valid());

  iter->SeekToLast();
  ASSERT_TRUE(IterEquals(iter.get(), "f", "ff"));
  iter->Prev();
  ASSERT_TRUE(IterEquals(iter.get(), "d", "dd"));
  iter->Prev();
  ASSERT_TRUE(IterEquals(iter.get(), "cc", "cccc"));
  iter->Prev();
  ASSERT_TRUE(IterEquals(iter.get(), "c", "cc"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "cc", "cccc"));
  iter->Prev();
  ASSERT_TRUE(IterEquals(iter.get(), "c", "cc"));
  iter->Prev();
  ASSERT_TRUE(IterEquals(iter.get(), "a", "aa"));
  iter->Prev();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("c");
  ASSERT_TRUE(IterEquals(iter.get(), "c", "cc"));

  iter->Seek("cb");
  ASSERT_TRUE(IterEquals(iter.get(), "cc", "cccc"));

  iter->Seek("cc");
  ASSERT_TRUE(IterEquals(iter.get(), "cc", "cccc"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "d", "dd"));

  iter->Seek("e");
  ASSERT_TRUE(IterEquals(iter.get(), "f", "ff"));

  iter->Prev();
  ASSERT_TRUE(IterEquals(iter.get(), "d", "dd"));

  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "f", "ff"));
}

TEST_F(WriteBatchWithIndexTest, TestIteraratorWithEmptyBaseBatch) {
  ColumnFamilyHandleImplDummy cf1(6, BytewiseComparator());
  ColumnFamilyHandleImplDummy cf2(2, BytewiseComparator());
  WriteBatchWithIndex batch(BytewiseComparator(), 20, true);

  ASSERT_OK(batch.Put(&cf1, "a", "aa"));
  ASSERT_OK(batch.Put(&cf2, "zoo", "bar"));  // note this is cf2!
  ASSERT_OK(batch.Delete(&cf1, "b"));
  ASSERT_OK(batch.Put(&cf1, "c", "cc"));
  ASSERT_OK(batch.Put(&cf1, "d", "dd"));
  ASSERT_OK(batch.Delete(&cf1, "e"));

  /* At this point batch/cf1 should contain:
    a -> aa
    c -> cc
    d -> dd
  */
  KVMap empty_map;
  std::unique_ptr<Iterator> iter(
      batch.NewIteratorWithBase(&cf1, new KVIter(&empty_map)));

  iter->SeekToFirst();
  ASSERT_TRUE(IterEquals(iter.get(), "a", "aa"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "c", "cc"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "d", "dd"));
  iter->Next();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(!iter->Valid());

  iter->SeekToLast();
  ASSERT_TRUE(IterEquals(iter.get(), "d", "dd"));
  iter->Prev();
  ASSERT_TRUE(IterEquals(iter.get(), "c", "cc"));
  iter->Prev();
  ASSERT_TRUE(IterEquals(iter.get(), "a", "aa"));

  iter->Prev();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(!iter->Valid());

  iter->Seek("aa");
  ASSERT_TRUE(IterEquals(iter.get(), "c", "cc"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "d", "dd"));

  iter->Seek("ca");
  ASSERT_TRUE(IterEquals(iter.get(), "d", "dd"));

  iter->Prev();
  ASSERT_TRUE(IterEquals(iter.get(), "c", "cc"));
}

TEST_F(WriteBatchWithIndexTest,
       TestIteraratorWithBaseSeekToLast1OnBaseAndBatch) {
  ColumnFamilyHandleImplDummy cf1(6, BytewiseComparator());
  WriteBatchWithIndex batch(BytewiseComparator(), 0, true);

  KVMap base;
  base["k01"] = "v01";
  base["k02"] = "v02";
  base["k03"] = "v03";

  batch.Put(&cf1, "k04", "v04");
  batch.Put(&cf1, "k05", "v05");
  batch.Put(&cf1, "k06", "v06");

  ReadOptions read_options;

  std::unique_ptr<Iterator> iter(batch.NewIteratorWithBase(
      &cf1, new KVIter(&base, BytewiseComparator(), &read_options),
      &read_options));

  ASSERT_OK(iter->status());

  iter->SeekToLast();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());
  ASSERT_TRUE(IterEquals(iter.get(), "k06", "v06"));

  iter->Next();
  ASSERT_OK(iter->status());
  ASSERT_FALSE(iter->Valid()) << "Should have reached upper_bound";
}

TEST_F(WriteBatchWithIndexTest,
       TestIteraratorWithBaseSeekToLast1OnBatchAndBase) {
  ColumnFamilyHandleImplDummy cf1(6, BytewiseComparator());
  WriteBatchWithIndex batch(BytewiseComparator(), 0, true);

  KVMap base;
  base["k04"] = "v04";
  base["k05"] = "v05";
  base["k06"] = "v06";

  batch.Put(&cf1, "k01", "v01");
  batch.Put(&cf1, "k02", "v02");
  batch.Put(&cf1, "k03", "v03");

  ReadOptions read_options;

  std::unique_ptr<Iterator> iter(batch.NewIteratorWithBase(
      &cf1, new KVIter(&base, BytewiseComparator(), &read_options),
      &read_options));

  ASSERT_OK(iter->status());

  iter->SeekToLast();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());
  ASSERT_TRUE(IterEquals(iter.get(), "k06", "v06"));

  iter->Next();
  ASSERT_OK(iter->status());
  ASSERT_FALSE(iter->Valid()) << "Should have reached upper_bound";
}

TEST_F(WriteBatchWithIndexTest,
       TestIteraratorWithBaseSeekToLast1OnBaseAndBatchWithBounds) {
  ColumnFamilyHandleImplDummy cf1(6, BytewiseComparator());
  WriteBatchWithIndex batch(BytewiseComparator(), 0, true);

  KVMap base;
  base["k01"] = "v01";
  base["k02"] = "v02";
  base["k03"] = "v03";

  batch.Put(&cf1, "k04", "v04");
  batch.Put(&cf1, "k05", "v05");
  batch.Put(&cf1, "k06", "v06");

  ReadOptions read_options;

  std::unique_ptr<Iterator> iter(batch.NewIteratorWithBase(
      &cf1, new KVIter(&base, BytewiseComparator(), &read_options),
      &read_options));

  ASSERT_OK(iter->status());

  Slice upper_bound_batch("k06");
  read_options.iterate_upper_bound = &upper_bound_batch;

  iter->SeekToLast();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());
  ASSERT_TRUE(IterEquals(iter.get(), "k05", "v05"));

  iter->Next();
  ASSERT_OK(iter->status());
  ASSERT_FALSE(iter->Valid()) << "Should have reached upper_bound";
}

TEST_F(WriteBatchWithIndexTest,
       TestIteraratorWithBaseSeekToLast1OnBaseAndBatchUnbalanced1WithBounds) {
  ColumnFamilyHandleImplDummy cf1(6, BytewiseComparator());
  WriteBatchWithIndex batch(BytewiseComparator(), 0, true);

  KVMap base;
  base["k01"] = "v01";
  base["k02"] = "v02";

  batch.Put(&cf1, "k03", "v03");
  batch.Put(&cf1, "k04", "v04");
  batch.Put(&cf1, "k05", "v05");
  batch.Put(&cf1, "k06", "v06");

  ReadOptions read_options;

  std::unique_ptr<Iterator> iter(batch.NewIteratorWithBase(
      &cf1, new KVIter(&base, BytewiseComparator(), &read_options),
      &read_options));

  ASSERT_OK(iter->status());

  Slice upper_bound_batch("k06");
  read_options.iterate_upper_bound = &upper_bound_batch;

  iter->SeekToLast();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());
  ASSERT_TRUE(IterEquals(iter.get(), "k05", "v05"));

  iter->Next();
  ASSERT_OK(iter->status());
  ASSERT_FALSE(iter->Valid()) << "Should have reached upper_bound";
}

TEST_F(WriteBatchWithIndexTest,
       TestIteraratorWithBaseSeekToLast1OnBaseAndBatchUnbalanced2WithBounds) {
  ColumnFamilyHandleImplDummy cf1(6, BytewiseComparator());
  WriteBatchWithIndex batch(BytewiseComparator(), 0, true);

  KVMap base;
  base["k01"] = "v01";
  base["k02"] = "v02";
  base["k02"] = "v03";
  base["k04"] = "v04";

  batch.Put(&cf1, "k05", "v05");
  batch.Put(&cf1, "k06", "v06");

  ReadOptions read_options;

  std::unique_ptr<Iterator> iter(batch.NewIteratorWithBase(
      &cf1, new KVIter(&base, BytewiseComparator(), &read_options),
      &read_options));

  ASSERT_OK(iter->status());

  Slice upper_bound_batch("k06");
  read_options.iterate_upper_bound = &upper_bound_batch;

  iter->SeekToLast();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());
  ASSERT_TRUE(IterEquals(iter.get(), "k05", "v05"));

  iter->Next();
  ASSERT_OK(iter->status());
  ASSERT_FALSE(iter->Valid()) << "Should have reached upper_bound";
}

TEST_F(WriteBatchWithIndexTest,
       TestIteraratorWithBaseSeekToLast1OnBatchAndBaseWithBounds) {
  ColumnFamilyHandleImplDummy cf1(6, BytewiseComparator());
  WriteBatchWithIndex batch(BytewiseComparator(), 0, true);

  KVMap base;
  base["k04"] = "v04";
  base["k05"] = "v05";
  base["k06"] = "v06";

  batch.Put(&cf1, "k01", "v01");
  batch.Put(&cf1, "k02", "v02");
  batch.Put(&cf1, "k03", "v03");

  ReadOptions read_options;

  std::unique_ptr<Iterator> iter(batch.NewIteratorWithBase(
      &cf1, new KVIter(&base, BytewiseComparator(), &read_options),
      &read_options));

  ASSERT_OK(iter->status());

  Slice upper_bound_batch("k06");
  read_options.iterate_upper_bound = &upper_bound_batch;

  iter->SeekToLast();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());
  ASSERT_TRUE(IterEquals(iter.get(), "k05", "v05"));

  iter->Next();
  ASSERT_OK(iter->status());
  ASSERT_FALSE(iter->Valid()) << "Should have reached upper_bound";
}

TEST_F(WriteBatchWithIndexTest,
       TestIteraratorWithBaseSeekToLast1OnBatchAndBaseUnbalanced1WithBounds) {
  ColumnFamilyHandleImplDummy cf1(6, BytewiseComparator());
  WriteBatchWithIndex batch(BytewiseComparator(), 0, true);

  KVMap base;
  base["k05"] = "v05";
  base["k06"] = "v06";

  batch.Put(&cf1, "k01", "v01");
  batch.Put(&cf1, "k02", "v02");
  batch.Put(&cf1, "k03", "v03");
  batch.Put(&cf1, "k04", "v04");

  ReadOptions read_options;

  std::unique_ptr<Iterator> iter(batch.NewIteratorWithBase(
      &cf1, new KVIter(&base, BytewiseComparator(), &read_options),
      &read_options));

  ASSERT_OK(iter->status());

  Slice upper_bound_batch("k06");
  read_options.iterate_upper_bound = &upper_bound_batch;

  iter->SeekToLast();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());
  ASSERT_TRUE(IterEquals(iter.get(), "k05", "v05"));

  iter->Next();
  ASSERT_OK(iter->status());
  ASSERT_FALSE(iter->Valid()) << "Should have reached upper_bound";
}

TEST_F(WriteBatchWithIndexTest,
       TestIteraratorWithBaseSeekToLast1OnBatchAndBaseUnbalanced2WithBounds) {
  ColumnFamilyHandleImplDummy cf1(6, BytewiseComparator());
  WriteBatchWithIndex batch(BytewiseComparator(), 0, true);

  KVMap base;
  base["k03"] = "v03";
  base["k04"] = "v04";
  base["k05"] = "v05";
  base["k06"] = "v06";

  batch.Put(&cf1, "k01", "v01");
  batch.Put(&cf1, "k02", "v02");

  ReadOptions read_options;

  std::unique_ptr<Iterator> iter(batch.NewIteratorWithBase(
      &cf1, new KVIter(&base, BytewiseComparator(), &read_options),
      &read_options));

  ASSERT_OK(iter->status());

  Slice upper_bound_batch("k06");
  read_options.iterate_upper_bound = &upper_bound_batch;

  iter->SeekToLast();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());
  ASSERT_TRUE(IterEquals(iter.get(), "k05", "v05"));

  iter->Next();
  ASSERT_OK(iter->status());
  ASSERT_FALSE(iter->Valid()) << "Should have reached upper_bound";
}

TEST_F(WriteBatchWithIndexTest,
       TestIteraratorWithBaseSeekToLastOnBaseAndBatch) {
  ColumnFamilyHandleImplDummy cf1(6, BytewiseComparator());
  WriteBatchWithIndex batch(BytewiseComparator(), 0, true);

  KVMap base;
  base["k01"] = "v01";
  base["k02"] = "v02";
  base["k03"] = "v03";

  batch.Put(&cf1, "k04", "v04");
  batch.Put(&cf1, "k05", "v05");
  batch.Put(&cf1, "k06", "v06");

  std::unique_ptr<Iterator> iter(
      batch.NewIteratorWithBase(&cf1, new KVIter(&base, BytewiseComparator())));

  ASSERT_OK(iter->status());

  iter->SeekToLast();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());
  ASSERT_TRUE(IterEquals(iter.get(), "k06", "v06"));

  iter->Next();
  ASSERT_OK(iter->status());
  ASSERT_FALSE(iter->Valid()) << "Should have reached end";

  iter->SeekToLast();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());
  ASSERT_TRUE(IterEquals(iter.get(), "k06", "v06"));

  iter->Prev();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());
  ASSERT_TRUE(IterEquals(iter.get(), "k05", "v05"));

  iter->Prev();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());
  ASSERT_TRUE(IterEquals(iter.get(), "k04", "v04"));

  iter->Prev();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());
  ASSERT_TRUE(IterEquals(iter.get(), "k03", "v03"));

  iter->Prev();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());
  ASSERT_TRUE(IterEquals(iter.get(), "k02", "v02"));

  iter->Prev();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());
  ASSERT_TRUE(IterEquals(iter.get(), "k01", "v01"));

  iter->Prev();
  ASSERT_OK(iter->status());
  ASSERT_FALSE(iter->Valid()) << "Should have reached start";

  iter->SeekToLast();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());
  ASSERT_TRUE(IterEquals(iter.get(), "k06", "v06"));

  // random seek forward
  iter->Seek("k04");
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());
  ASSERT_TRUE(IterEquals(iter.get(), "k04", "v04"));

  iter->Next();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());
  ASSERT_TRUE(IterEquals(iter.get(), "k05", "v05"));

  iter->Next();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());
  ASSERT_TRUE(IterEquals(iter.get(), "k06", "v06"));

  iter->Next();
  ASSERT_OK(iter->status());
  ASSERT_FALSE(iter->Valid()) << "Should have reached end";

  iter->SeekToLast();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());
  ASSERT_TRUE(IterEquals(iter.get(), "k06", "v06"));

  iter->Next();
  ASSERT_OK(iter->status());
  ASSERT_FALSE(iter->Valid()) << "Should have reached end";
}

TEST_F(WriteBatchWithIndexTest,
       TestIteraratorWithBaseSeekToLastOnBatchAndBase) {
  ColumnFamilyHandleImplDummy cf1(6, BytewiseComparator());
  WriteBatchWithIndex batch(BytewiseComparator(), 0, true);

  KVMap base;
  base["k04"] = "v04";
  base["k05"] = "v05";
  base["k06"] = "v06";

  batch.Put(&cf1, "k01", "v01");
  batch.Put(&cf1, "k02", "v02");
  batch.Put(&cf1, "k03", "v03");

  std::unique_ptr<Iterator> iter(
      batch.NewIteratorWithBase(&cf1, new KVIter(&base, BytewiseComparator())));

  ASSERT_OK(iter->status());

  iter->SeekToLast();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());
  ASSERT_TRUE(IterEquals(iter.get(), "k06", "v06"));

  iter->Next();
  ASSERT_OK(iter->status());
  ASSERT_FALSE(iter->Valid()) << "Should have reached end";

  iter->SeekToLast();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());
  ASSERT_TRUE(IterEquals(iter.get(), "k06", "v06"));

  iter->Prev();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());
  ASSERT_TRUE(IterEquals(iter.get(), "k05", "v05"));

  iter->Prev();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());
  ASSERT_TRUE(IterEquals(iter.get(), "k04", "v04"));

  iter->Prev();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());
  ASSERT_TRUE(IterEquals(iter.get(), "k03", "v03"));

  iter->Prev();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());
  ASSERT_TRUE(IterEquals(iter.get(), "k02", "v02"));

  iter->Prev();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());
  ASSERT_TRUE(IterEquals(iter.get(), "k01", "v01"));

  iter->Prev();
  ASSERT_OK(iter->status());
  ASSERT_FALSE(iter->Valid()) << "Should have reached start";

  iter->SeekToLast();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());
  ASSERT_TRUE(IterEquals(iter.get(), "k06", "v06"));

  // random seek forward
  iter->Seek("k04");
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());
  ASSERT_TRUE(IterEquals(iter.get(), "k04", "v04"));

  iter->Next();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());
  ASSERT_TRUE(IterEquals(iter.get(), "k05", "v05"));

  iter->Next();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());
  ASSERT_TRUE(IterEquals(iter.get(), "k06", "v06"));

  iter->Next();
  ASSERT_OK(iter->status());
  ASSERT_FALSE(iter->Valid()) << "Should have reached end";

  iter->SeekToLast();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());
  ASSERT_TRUE(IterEquals(iter.get(), "k06", "v06"));
}

TEST_F(WriteBatchWithIndexTest,
       TestIteraratorWithBaseUpperBoundOnBaseWithoutBaseConstraint) {
  ColumnFamilyHandleImplDummy cf1(6, BytewiseComparator());
  WriteBatchWithIndex batch(BytewiseComparator(), 0, true);

  KVMap base;
  base["k1"] = "v1";
  base["k2"] = "v2";
  base["k3"] = "v3";
  base["k4"] = "v4";
  base["k5"] = "v5";
  base["k6"] = "v6";

  Slice upper_bound("k4");

  ReadOptions read_options;
  read_options.iterate_upper_bound = &upper_bound;

  // NOTE: read_options are NOT passed to KVIter, so WBWIIterator imposes
  // iterate_upper_bound on base
  std::unique_ptr<Iterator> iter(batch.NewIteratorWithBase(
      &cf1, new KVIter(&base, BytewiseComparator()), &read_options));

  ASSERT_OK(iter->status());

  iter->SeekToFirst();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());

  ASSERT_TRUE(IterEquals(iter.get(), "k1", "v1"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "k2", "v2"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "k3", "v3"));
  iter->Next();
  ASSERT_OK(iter->status());
  ASSERT_FALSE(iter->Valid()) << "Should have reached upper_bound";

  iter->SeekToLast();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());

  ASSERT_TRUE(IterEquals(iter.get(), "k3", "v3"));
  iter->Next();
  ASSERT_OK(iter->status());
  ASSERT_FALSE(iter->Valid()) << "Should have reached upper_bound";

  iter->SeekToLast();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());
  iter->Prev();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());

  ASSERT_TRUE(IterEquals(iter.get(), "k2", "v2"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "k3", "v3"));
  iter->Next();
  ASSERT_OK(iter->status());
  ASSERT_FALSE(iter->Valid()) << "Should have reached upper_bound";
}

TEST_F(WriteBatchWithIndexTest,
       TestIteraratorWithBaseUpperBoundOnBaseWithBaseConstraint) {
  ColumnFamilyHandleImplDummy cf1(6, BytewiseComparator());
  WriteBatchWithIndex batch(BytewiseComparator(), 0, true);

  KVMap base;
  base["k1"] = "v1";
  base["k2"] = "v2";
  base["k3"] = "v3";
  base["k4"] = "v4";
  base["k5"] = "v5";
  base["k6"] = "v6";

  Slice upper_bound("k4");

  ReadOptions read_options;
  read_options.iterate_upper_bound = &upper_bound;

  // NOTE: read_options are also passed to KVIter, so KVIter imposes
  // iterate_upper_bound on base
  std::unique_ptr<Iterator> iter(batch.NewIteratorWithBase(
      &cf1, new KVIter(&base, BytewiseComparator(), &read_options),
      &read_options));

  ASSERT_OK(iter->status());

  iter->SeekToFirst();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());

  ASSERT_TRUE(IterEquals(iter.get(), "k1", "v1"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "k2", "v2"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "k3", "v3"));
  iter->Next();
  ASSERT_OK(iter->status());
  ASSERT_FALSE(iter->Valid()) << "Should have reached upper_bound";

  iter->SeekToLast();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());

  ASSERT_TRUE(IterEquals(iter.get(), "k3", "v3"));
  iter->Next();
  ASSERT_OK(iter->status());
  ASSERT_FALSE(iter->Valid()) << "Should have reached upper_bound";

  iter->SeekToLast();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());
  iter->Prev();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());

  ASSERT_TRUE(IterEquals(iter.get(), "k2", "v2"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "k3", "v3"));
  iter->Next();
  ASSERT_OK(iter->status());
  ASSERT_FALSE(iter->Valid()) << "Should have reached upper_bound";
}

TEST_F(WriteBatchWithIndexTest, TestIteraratorWithBaseUpperBoundOnBatch) {
  ColumnFamilyHandleImplDummy cf1(6, BytewiseComparator());
  WriteBatchWithIndex batch(BytewiseComparator(), 0, true);

  batch.Put(&cf1, "k1", "v1");
  batch.Put(&cf1, "k2", "v2");
  batch.Put(&cf1, "k3", "v3");
  batch.Put(&cf1, "k4", "v4");
  batch.Put(&cf1, "k5", "v5");
  batch.Put(&cf1, "k6", "v6");

  Slice upper_bound("k4");

  ReadOptions read_options;
  read_options.iterate_upper_bound = &upper_bound;

  KVMap empty_map;
  std::unique_ptr<Iterator> iter(batch.NewIteratorWithBase(
      &cf1, new KVIter(&empty_map, BytewiseComparator(), &read_options),
      &read_options));

  ASSERT_OK(iter->status());

  iter->SeekToFirst();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());

  ASSERT_TRUE(IterEquals(iter.get(), "k1", "v1"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "k2", "v2"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "k3", "v3"));
  iter->Next();
  ASSERT_OK(iter->status());
  ASSERT_FALSE(iter->Valid()) << "Should have reached upper_bound";

  iter->SeekToLast();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());

  ASSERT_TRUE(IterEquals(iter.get(), "k3", "v3"));
  iter->Next();
  ASSERT_OK(iter->status());
  ASSERT_FALSE(iter->Valid()) << "Should have reached upper_bound";

  iter->SeekToLast();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());
  iter->Prev();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());

  ASSERT_TRUE(IterEquals(iter.get(), "k2", "v2"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "k3", "v3"));
  iter->Next();
  ASSERT_OK(iter->status());
  ASSERT_FALSE(iter->Valid()) << "Should have reached upper_bound";
}

TEST_F(WriteBatchWithIndexTest,
       TestIteraratorWithBaseUpperBoundOnBaseAndBatch) {
  ColumnFamilyHandleImplDummy cf1(6, BytewiseComparator());
  WriteBatchWithIndex batch(BytewiseComparator(), 0, true);

  KVMap base;
  base["k01"] = "v01";
  base["k02"] = "v02";
  base["k03"] = "v03";
  base["k04"] = "v04";

  batch.Put(&cf1, "k05", "v05");
  batch.Put(&cf1, "k06", "v06");
  batch.Put(&cf1, "k07", "v07");
  batch.Put(&cf1, "k08", "v08");

  ReadOptions read_options;

  // scan over base
  Slice upper_bound_base("k04");
  read_options.iterate_upper_bound = &upper_bound_base;

  std::unique_ptr<Iterator> iter(batch.NewIteratorWithBase(
      &cf1, new KVIter(&base, BytewiseComparator(), &read_options),
      &read_options));

  ASSERT_OK(iter->status());

  iter->SeekToFirst();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());

  ASSERT_TRUE(IterEquals(iter.get(), "k01", "v01"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "k02", "v02"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "k03", "v03"));
  iter->Next();
  ASSERT_OK(iter->status());
  ASSERT_FALSE(iter->Valid()) << "Should have reached upper_bound";

  iter->SeekToLast();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());

  ASSERT_TRUE(IterEquals(iter.get(), "k03", "v03"));
  iter->Next();
  ASSERT_OK(iter->status());
  ASSERT_FALSE(iter->Valid()) << "Should have reached upper_bound";

  iter->SeekToLast();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());
  iter->Prev();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());

  ASSERT_TRUE(IterEquals(iter.get(), "k02", "v02"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "k03", "v03"));
  iter->Next();
  ASSERT_OK(iter->status());
  ASSERT_FALSE(iter->Valid()) << "Should have reached upper_bound";

  // scan over batch
  Slice upper_bound_batch("k08");
  read_options.iterate_upper_bound = &upper_bound_batch;

  iter->Seek("k05");
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());

  ASSERT_TRUE(IterEquals(iter.get(), "k05", "v05"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "k06", "v06"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "k07", "v07"));
  iter->Next();
  ASSERT_OK(iter->status());
  ASSERT_FALSE(iter->Valid()) << "Should have reached upper_bound";

  iter->SeekToLast();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());
  ASSERT_TRUE(IterEquals(iter.get(), "k07", "v07"));

  iter->Next();
  ASSERT_OK(iter->status());
  ASSERT_FALSE(iter->Valid()) << "Should have reached upper_bound";

  iter->SeekToLast();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());
  iter->Prev();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());

  ASSERT_TRUE(IterEquals(iter.get(), "k06", "v06"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "k07", "v07"));
  iter->Next();
  ASSERT_OK(iter->status());
  ASSERT_FALSE(iter->Valid()) << "Should have reached upper_bound";
}

TEST_F(WriteBatchWithIndexTest,
       TestIteraratorWithBaseNoSuchUpperBoundOnBaseAndBatch) {
  ColumnFamilyHandleImplDummy cf1(6, BytewiseComparator());
  WriteBatchWithIndex batch(BytewiseComparator(), 0, true);

  KVMap base;
  base["k01"] = "v01";
  base["k02"] = "v02";
  base["k03"] = "v03";
  base["k04"] = "v04";

  batch.Put(&cf1, "k05", "v05");
  batch.Put(&cf1, "k06", "v06");
  batch.Put(&cf1, "k07", "v07");
  batch.Put(&cf1, "k08", "v08");

  ReadOptions read_options;

  // scan over base
  // upper bound k033 does exist, but comes between k03 and k04
  Slice upper_bound_base("k033");
  read_options.iterate_upper_bound = &upper_bound_base;

  std::unique_ptr<Iterator> iter(batch.NewIteratorWithBase(
      &cf1, new KVIter(&base, BytewiseComparator(), &read_options),
      &read_options));

  ASSERT_OK(iter->status());

  iter->SeekToFirst();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());

  ASSERT_TRUE(IterEquals(iter.get(), "k01", "v01"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "k02", "v02"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "k03", "v03"));
  iter->Next();
  ASSERT_OK(iter->status());
  ASSERT_FALSE(iter->Valid()) << "Should have reached upper_bound";

  iter->SeekToLast();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());

  ASSERT_TRUE(IterEquals(iter.get(), "k03", "v03"));
  iter->Next();
  ASSERT_OK(iter->status());
  ASSERT_FALSE(iter->Valid()) << "Should have reached upper_bound";

  iter->SeekToLast();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());
  iter->Prev();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());

  ASSERT_TRUE(IterEquals(iter.get(), "k02", "v02"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "k03", "v03"));
  iter->Next();
  ASSERT_OK(iter->status());
  ASSERT_FALSE(iter->Valid()) << "Should have reached upper_bound";

  // scan over batch
  // upper bound k077 does exist, but comes between k07 and k08
  Slice upper_bound_batch("k077");
  read_options.iterate_upper_bound = &upper_bound_batch;

  iter->Seek("k05");
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());

  ASSERT_TRUE(IterEquals(iter.get(), "k05", "v05"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "k06", "v06"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "k07", "v07"));
  iter->Next();
  ASSERT_OK(iter->status());
  ASSERT_FALSE(iter->Valid()) << "Should have reached upper_bound";

  iter->SeekToLast();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());
  ASSERT_TRUE(IterEquals(iter.get(), "k07", "v07"));

  iter->Next();
  ASSERT_OK(iter->status());
  ASSERT_FALSE(iter->Valid()) << "Should have reached upper_bound";

  iter->SeekToLast();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());
  iter->Prev();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());

  ASSERT_TRUE(IterEquals(iter.get(), "k06", "v06"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "k07", "v07"));
  iter->Next();
  ASSERT_OK(iter->status());
  ASSERT_FALSE(iter->Valid()) << "Should have reached upper_bound";
}

TEST_F(WriteBatchWithIndexTest,
       TestIteraratorWithBaseOverUpperBoundOnBaseWithBaseConstraintAndBatch) {
  ColumnFamilyHandleImplDummy cf1(6, BytewiseComparator());
  WriteBatchWithIndex batch(BytewiseComparator(), 0, true);

  KVMap base;
  base["k01"] = "v01";
  base["k02"] = "v02";
  base["k03"] = "v03";
  base["k04"] = "v04";

  batch.Put(&cf1, "k05", "v05");
  batch.Put(&cf1, "k06", "v06");
  batch.Put(&cf1, "k07", "v07");
  batch.Put(&cf1, "k08", "v08");

  ReadOptions read_options;

  // scan over base
  // upper bound k044 is beyond the keys in the base
  Slice upper_bound_base("k044");
  read_options.iterate_upper_bound = &upper_bound_base;

  // NOTE: KVIter also has read_options::iterate_upper_bound constraint
  std::unique_ptr<Iterator> iter(batch.NewIteratorWithBase(
      &cf1, new KVIter(&base, BytewiseComparator(), &read_options),
      &read_options));

  ASSERT_OK(iter->status());

  iter->SeekToFirst();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());

  ASSERT_TRUE(IterEquals(iter.get(), "k01", "v01"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "k02", "v02"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "k03", "v03"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "k04", "v04"));
  iter->Next();
  ASSERT_OK(iter->status());
  ASSERT_FALSE(iter->Valid()) << "Should have reached upper_bound";

  iter->SeekToLast();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());

  ASSERT_TRUE(IterEquals(iter.get(), "k04", "v04"));
  iter->Next();
  ASSERT_OK(iter->status());
  ASSERT_FALSE(iter->Valid()) << "Should have reached upper_bound";

  iter->SeekToLast();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());
  iter->Prev();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());

  ASSERT_TRUE(IterEquals(iter.get(), "k03", "v03"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "k04", "v04"));
  iter->Next();
  ASSERT_OK(iter->status());
  ASSERT_FALSE(iter->Valid()) << "Should have reached upper_bound";

  // scan over batch
  // upper bound k09 is beyond the keys in the batch
  Slice upper_bound_batch("k09");
  read_options.iterate_upper_bound = &upper_bound_batch;

  iter->Seek("k05");
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());

  ASSERT_TRUE(IterEquals(iter.get(), "k05", "v05"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "k06", "v06"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "k07", "v07"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "k08", "v08"));
  iter->Next();
  ASSERT_OK(iter->status());
  ASSERT_FALSE(iter->Valid()) << "Should have reached upper_bound";

  iter->SeekToLast();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());
  ASSERT_TRUE(IterEquals(iter.get(), "k08", "v08"));

  iter->Next();
  ASSERT_OK(iter->status());
  ASSERT_FALSE(iter->Valid()) << "Should have reached upper_bound";

  iter->SeekToLast();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());
  iter->Prev();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());

  ASSERT_TRUE(IterEquals(iter.get(), "k07", "v07"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "k08", "v08"));
  iter->Next();
  ASSERT_OK(iter->status());
  ASSERT_FALSE(iter->Valid()) << "Should have reached upper_bound";
}

TEST_F(
    WriteBatchWithIndexTest,
    TestIteraratorWithBaseOverUpperBoundOnBaseWithoutBaseConstraintAndBatch) {
  ColumnFamilyHandleImplDummy cf1(6, BytewiseComparator());
  WriteBatchWithIndex batch(BytewiseComparator(), 0, true);

  KVMap base;
  base["k01"] = "v01";
  base["k02"] = "v02";
  base["k03"] = "v03";
  base["k04"] = "v04";

  batch.Put(&cf1, "k05", "v05");
  batch.Put(&cf1, "k06", "v06");
  batch.Put(&cf1, "k07", "v07");
  batch.Put(&cf1, "k08", "v08");

  ReadOptions read_options;

  // scan over base
  // upper bound k044 is beyond the keys in the base
  Slice upper_bound_base("k044");
  read_options.iterate_upper_bound = &upper_bound_base;

  // NOTE: KVIter DOES NOT have read_options::iterate_upper_bound constraint
  std::unique_ptr<Iterator> iter(batch.NewIteratorWithBase(
      &cf1, new KVIter(&base, BytewiseComparator(), nullptr), &read_options));

  ASSERT_OK(iter->status());

  iter->SeekToFirst();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());

  ASSERT_TRUE(IterEquals(iter.get(), "k01", "v01"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "k02", "v02"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "k03", "v03"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "k04", "v04"));
  iter->Next();
  ASSERT_OK(iter->status());
  ASSERT_FALSE(iter->Valid()) << "Should have reached upper_bound";

  iter->SeekToLast();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());

  ASSERT_TRUE(IterEquals(iter.get(), "k04", "v04"));
  iter->Next();
  ASSERT_OK(iter->status());
  ASSERT_FALSE(iter->Valid()) << "Should have reached upper_bound";

  iter->SeekToLast();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());
  iter->Prev();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());

  ASSERT_TRUE(IterEquals(iter.get(), "k03", "v03"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "k04", "v04"));
  iter->Next();
  ASSERT_OK(iter->status());
  ASSERT_FALSE(iter->Valid()) << "Should have reached upper_bound";

  // scan over batch
  // upper bound k09 is beyond the keys in the batch
  Slice upper_bound_batch("k09");
  read_options.iterate_upper_bound = &upper_bound_batch;

  iter->Seek("k05");
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());

  ASSERT_TRUE(IterEquals(iter.get(), "k05", "v05"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "k06", "v06"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "k07", "v07"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "k08", "v08"));
  iter->Next();
  ASSERT_OK(iter->status());
  ASSERT_FALSE(iter->Valid()) << "Should have reached upper_bound";

  iter->SeekToLast();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());
  ASSERT_TRUE(IterEquals(iter.get(), "k08", "v08"));

  iter->Next();
  ASSERT_OK(iter->status());
  ASSERT_FALSE(iter->Valid()) << "Should have reached upper_bound";

  iter->SeekToLast();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());
  iter->Prev();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());

  ASSERT_TRUE(IterEquals(iter.get(), "k07", "v07"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "k08", "v08"));
  iter->Next();
  ASSERT_OK(iter->status());
  ASSERT_FALSE(iter->Valid()) << "Should have reached upper_bound";
}

TEST_F(WriteBatchWithIndexTest,
       TestIteraratorWithBaseUpperBoundOnBaseAndDifferentUpperBoundOnBatch) {
  ColumnFamilyHandleImplDummy cf1(6, BytewiseComparator());
  WriteBatchWithIndex batch(BytewiseComparator(), 0, true);

  KVMap base;
  base["k01"] = "v01";
  base["k02"] = "v02";
  base["k03"] = "v03";
  base["k04"] = "v04";

  batch.Put(&cf1, "k05", "v05");
  batch.Put(&cf1, "k06", "v06");
  batch.Put(&cf1, "k07", "v07");
  batch.Put(&cf1, "k08", "v08");

  // upper bound for base
  ReadOptions read_options_base;
  Slice upper_bound_base("k04");
  read_options_base.iterate_upper_bound = &upper_bound_base;

  // upper bound for base
  ReadOptions read_options_batch;
  Slice upper_bound_batch("k08");
  read_options_batch.iterate_upper_bound = &upper_bound_batch;

  std::unique_ptr<Iterator> iter(batch.NewIteratorWithBase(
      &cf1, new KVIter(&base, BytewiseComparator(), &read_options_base),
      &read_options_batch));

  ASSERT_OK(iter->status());

  iter->SeekToFirst();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());

  ASSERT_TRUE(IterEquals(iter.get(), "k01", "v01"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "k02", "v02"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "k03", "v03"));
  iter->Next();
  // NOTE: that k04 is skpped over as that is >= upper_bound_base
  ASSERT_TRUE(IterEquals(iter.get(), "k05", "v05"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "k06", "v06"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "k07", "v07"));
  iter->Next();
  // NOTE: that k08 is skpped over as that is >= upper_bound_batch
  ASSERT_OK(iter->status());
  ASSERT_FALSE(iter->Valid()) << "Should have reached upper_bound";

  iter->SeekToLast();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());

  // NOTE: this is the upper_bound_batch as it is < upper_bound_base
  ASSERT_TRUE(IterEquals(iter.get(), "k07", "v07"));
  iter->Next();
  ASSERT_OK(iter->status());
  ASSERT_FALSE(iter->Valid()) << "Should have reached upper_bound";

  iter->SeekToLast();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());

  iter->Prev();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());
  ASSERT_TRUE(IterEquals(iter.get(), "k06", "v06"));

  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "k07", "v07"));

  iter->Next();
  ASSERT_OK(iter->status());
  ASSERT_FALSE(iter->Valid()) << "Should have reached upper_bound";
}

TEST_F(WriteBatchWithIndexTest,
       TestIteraratorWithBaseUpperBoundOnBaseAndBatchInterleaved) {
  ColumnFamilyHandleImplDummy cf1(6, BytewiseComparator());
  WriteBatchWithIndex batch(BytewiseComparator(), 0, true);

  KVMap base;
  base["k01"] = "v01";
  base["k02"] = "v02";
  base["k03"] = "v03";
  base["k04"] = "v04";
  base["k09"] = "v09";
  base["k0C"] = "v0C";
  base["k0D"] = "v0D";

  batch.Put(&cf1, "k05", "v05");
  batch.Put(&cf1, "k06", "v06");
  batch.Put(&cf1, "k07", "v07");
  batch.Put(&cf1, "k08", "v08");
  batch.Put(&cf1, "k0A", "v0A");
  batch.Put(&cf1, "k0B", "v0B");

  Slice upper_bound("k0B");

  ReadOptions read_options;
  read_options.iterate_upper_bound = &upper_bound;

  std::unique_ptr<Iterator> iter(batch.NewIteratorWithBase(
      &cf1, new KVIter(&base, BytewiseComparator(), &read_options),
      &read_options));

  ASSERT_OK(iter->status());

  iter->SeekToFirst();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());

  ASSERT_TRUE(IterEquals(iter.get(), "k01", "v01"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "k02", "v02"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "k03", "v03"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "k04", "v04"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "k05", "v05"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "k06", "v06"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "k07", "v07"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "k08", "v08"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "k09", "v09"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "k0A", "v0A"));
  iter->Next();
  ASSERT_OK(iter->status());
  ASSERT_FALSE(iter->Valid()) << "Should have reached upper_bound";
}

TEST_F(WriteBatchWithIndexTest,
       TestIteraratorWithBaseUpperBoundOnBatchAndBaseInterleaved) {
  ColumnFamilyHandleImplDummy cf1(6, BytewiseComparator());
  WriteBatchWithIndex batch(BytewiseComparator(), 0, true);

  KVMap base;
  base["k01"] = "v01";
  base["k02"] = "v02";
  base["k03"] = "v03";
  base["k04"] = "v04";
  base["k09"] = "v09";
  base["k0C"] = "v0C";
  base["k0D"] = "v0D";

  batch.Put(&cf1, "k05", "v05");
  batch.Put(&cf1, "k06", "v06");
  batch.Put(&cf1, "k07", "v07");
  batch.Put(&cf1, "k08", "v08");
  batch.Put(&cf1, "k0A", "v0A");
  batch.Put(&cf1, "k0B", "v0B");

  Slice upper_bound("k0B");

  ReadOptions read_options;
  read_options.iterate_upper_bound = &upper_bound;

  std::unique_ptr<Iterator> iter(batch.NewIteratorWithBase(
      &cf1, new KVIter(&base, BytewiseComparator(), &read_options),
      &read_options));

  ASSERT_OK(iter->status());

  iter->SeekToFirst();
  ASSERT_OK(iter->status());
  ASSERT_TRUE(iter->Valid());

  ASSERT_TRUE(IterEquals(iter.get(), "k01", "v01"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "k02", "v02"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "k03", "v03"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "k04", "v04"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "k05", "v05"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "k06", "v06"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "k07", "v07"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "k08", "v08"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "k09", "v09"));
  iter->Next();
  ASSERT_TRUE(IterEquals(iter.get(), "k0A", "v0A"));
  iter->Next();
  ASSERT_OK(iter->status());
  ASSERT_FALSE(iter->Valid()) << "Should have reached upper_bound";
}

TEST_F(WriteBatchWithIndexTest, TestIteraratorWithBaseReverseCmp) {
  ColumnFamilyHandleImplDummy cf1(6, ReverseBytewiseComparator());
  ColumnFamilyHandleImplDummy cf2(2, ReverseBytewiseComparator());
  WriteBatchWithIndex batch(BytewiseComparator(), 20, true);

  // Test the case that there is one element in the write batch
  ASSERT_OK(batch.Put(&cf2, "zoo", "bar"));
  ASSERT_OK(batch.Put(&cf1, "a", "aa"));
  {
    KVMap empty_map;
    std::unique_ptr<Iterator> iter(
        batch.NewIteratorWithBase(&cf1, new KVIter(&empty_map)));

    iter->SeekToFirst();
    ASSERT_TRUE(IterEquals(iter.get(), "a", "aa"));
    iter->Next();
    ASSERT_OK(iter->status());
    ASSERT_TRUE(!iter->Valid());
  }

  ASSERT_OK(batch.Put(&cf1, "c", "cc"));
  {
    KVMap map;
    std::unique_ptr<Iterator> iter(
        batch.NewIteratorWithBase(&cf1, new KVIter(&map)));

    iter->SeekToFirst();
    ASSERT_TRUE(IterEquals(iter.get(), "c", "cc"));
    iter->Next();
    ASSERT_TRUE(IterEquals(iter.get(), "a", "aa"));
    iter->Next();
    ASSERT_OK(iter->status());
    ASSERT_TRUE(!iter->Valid());

    iter->SeekToLast();
    ASSERT_TRUE(IterEquals(iter.get(), "a", "aa"));
    iter->Prev();
    ASSERT_TRUE(IterEquals(iter.get(), "c", "cc"));
    iter->Prev();
    ASSERT_OK(iter->status());
    ASSERT_TRUE(!iter->Valid());

    iter->Seek("b");
    ASSERT_TRUE(IterEquals(iter.get(), "a", "aa"));

    iter->Prev();
    ASSERT_TRUE(IterEquals(iter.get(), "c", "cc"));

    iter->Seek("a");
    ASSERT_TRUE(IterEquals(iter.get(), "a", "aa"));
  }

  // default column family
  ASSERT_OK(batch.Put("a", "b"));
  {
    KVMap map;
    map["b"] = "";
    std::unique_ptr<Iterator> iter(batch.NewIteratorWithBase(new KVIter(&map)));

    iter->SeekToFirst();
    ASSERT_TRUE(IterEquals(iter.get(), "a", "b"));
    iter->Next();
    ASSERT_TRUE(IterEquals(iter.get(), "b", ""));
    iter->Next();
    ASSERT_OK(iter->status());
    ASSERT_TRUE(!iter->Valid());

    iter->SeekToLast();
    ASSERT_TRUE(IterEquals(iter.get(), "b", ""));
    iter->Prev();
    ASSERT_TRUE(IterEquals(iter.get(), "a", "b"));
    iter->Prev();
    ASSERT_OK(iter->status());
    ASSERT_TRUE(!iter->Valid());

    iter->Seek("b");
    ASSERT_TRUE(IterEquals(iter.get(), "b", ""));

    iter->Prev();
    ASSERT_TRUE(IterEquals(iter.get(), "a", "b"));

    iter->Seek("0");
    ASSERT_TRUE(IterEquals(iter.get(), "a", "b"));
  }
}

TEST_F(WriteBatchWithIndexTest, TestGetFromBatch) {
  Options options;
  WriteBatchWithIndex batch;
  Status s;
  std::string value;

  s = batch.GetFromBatch(options, "b", &value);
  ASSERT_TRUE(s.IsNotFound());

  ASSERT_OK(batch.Put("a", "a"));
  ASSERT_OK(batch.Put("b", "b"));
  ASSERT_OK(batch.Put("c", "c"));
  ASSERT_OK(batch.Put("a", "z"));
  ASSERT_OK(batch.Delete("c"));
  ASSERT_OK(batch.Delete("d"));
  ASSERT_OK(batch.Delete("e"));
  ASSERT_OK(batch.Put("e", "e"));

  s = batch.GetFromBatch(options, "b", &value);
  ASSERT_OK(s);
  ASSERT_EQ("b", value);

  s = batch.GetFromBatch(options, "a", &value);
  ASSERT_OK(s);
  ASSERT_EQ("z", value);

  s = batch.GetFromBatch(options, "c", &value);
  ASSERT_TRUE(s.IsNotFound());

  s = batch.GetFromBatch(options, "d", &value);
  ASSERT_TRUE(s.IsNotFound());

  s = batch.GetFromBatch(options, "x", &value);
  ASSERT_TRUE(s.IsNotFound());

  s = batch.GetFromBatch(options, "e", &value);
  ASSERT_OK(s);
  ASSERT_EQ("e", value);

  ASSERT_OK(batch.Merge("z", "z"));

  s = batch.GetFromBatch(options, "z", &value);
  ASSERT_NOK(s);  // No merge operator specified.

  s = batch.GetFromBatch(options, "b", &value);
  ASSERT_OK(s);
  ASSERT_EQ("b", value);
}

TEST_F(WriteBatchWithIndexTest, TestGetFromBatchMerge) {
  DB* db;
  Options options;
  options.merge_operator = MergeOperators::CreateFromStringId("stringappend");
  options.create_if_missing = true;

  std::string dbname = test::PerThreadDBPath("write_batch_with_index_test");

  EXPECT_OK(DestroyDB(dbname, options));
  Status s = DB::Open(options, dbname, &db);
  ASSERT_OK(s);

  ColumnFamilyHandle* column_family = db->DefaultColumnFamily();
  WriteBatchWithIndex batch;
  std::string value;

  s = batch.GetFromBatch(options, "x", &value);
  ASSERT_TRUE(s.IsNotFound());

  ASSERT_OK(batch.Put("x", "X"));
  std::string expected = "X";

  for (int i = 0; i < 5; i++) {
    ASSERT_OK(batch.Merge("x", ToString(i)));
    expected = expected + "," + ToString(i);

    if (i % 2 == 0) {
      ASSERT_OK(batch.Put("y", ToString(i / 2)));
    }

    ASSERT_OK(batch.Merge("z", "z"));

    s = batch.GetFromBatch(column_family, options, "x", &value);
    ASSERT_OK(s);
    ASSERT_EQ(expected, value);

    s = batch.GetFromBatch(column_family, options, "y", &value);
    ASSERT_OK(s);
    ASSERT_EQ(ToString(i / 2), value);

    s = batch.GetFromBatch(column_family, options, "z", &value);
    ASSERT_TRUE(s.IsMergeInProgress());
  }

  delete db;
  EXPECT_OK(DestroyDB(dbname, options));
}

TEST_F(WriteBatchWithIndexTest, TestGetFromBatchMerge2) {
  DB* db;
  Options options;
  options.merge_operator = MergeOperators::CreateFromStringId("stringappend");
  options.create_if_missing = true;

  std::string dbname = test::PerThreadDBPath("write_batch_with_index_test");

  EXPECT_OK(DestroyDB(dbname, options));
  Status s = DB::Open(options, dbname, &db);
  ASSERT_OK(s);

  ColumnFamilyHandle* column_family = db->DefaultColumnFamily();

  // Test batch with overwrite_key=true
  WriteBatchWithIndex batch(BytewiseComparator(), 0, true);
  std::string value;

  s = batch.GetFromBatch(column_family, options, "X", &value);
  ASSERT_TRUE(s.IsNotFound());

  ASSERT_OK(batch.Put(column_family, "X", "x"));
  s = batch.GetFromBatch(column_family, options, "X", &value);
  ASSERT_OK(s);
  ASSERT_EQ("x", value);

  ASSERT_OK(batch.Put(column_family, "X", "x2"));
  s = batch.GetFromBatch(column_family, options, "X", &value);
  ASSERT_OK(s);
  ASSERT_EQ("x2", value);

  ASSERT_OK(batch.Merge(column_family, "X", "aaa"));
  s = batch.GetFromBatch(column_family, options, "X", &value);
  ASSERT_TRUE(s.IsMergeInProgress());

  ASSERT_OK(batch.Merge(column_family, "X", "bbb"));
  s = batch.GetFromBatch(column_family, options, "X", &value);
  ASSERT_TRUE(s.IsMergeInProgress());

  ASSERT_OK(batch.Put(column_family, "X", "x3"));
  s = batch.GetFromBatch(column_family, options, "X", &value);
  ASSERT_OK(s);
  ASSERT_EQ("x3", value);

  ASSERT_OK(batch.Merge(column_family, "X", "ccc"));
  s = batch.GetFromBatch(column_family, options, "X", &value);
  ASSERT_TRUE(s.IsMergeInProgress());

  ASSERT_OK(batch.Delete(column_family, "X"));
  s = batch.GetFromBatch(column_family, options, "X", &value);
  ASSERT_TRUE(s.IsNotFound());

  ASSERT_OK(batch.Merge(column_family, "X", "ddd"));
  s = batch.GetFromBatch(column_family, options, "X", &value);
  ASSERT_TRUE(s.IsMergeInProgress());

  delete db;
  EXPECT_OK(DestroyDB(dbname, options));
}

TEST_F(WriteBatchWithIndexTest, TestGetFromBatchAndDB) {
  DB* db;
  Options options;
  options.create_if_missing = true;
  std::string dbname = test::PerThreadDBPath("write_batch_with_index_test");

  EXPECT_OK(DestroyDB(dbname, options));
  Status s = DB::Open(options, dbname, &db);
  ASSERT_OK(s);

  WriteBatchWithIndex batch;
  ReadOptions read_options;
  WriteOptions write_options;
  std::string value;

  s = db->Put(write_options, "a", "a");
  ASSERT_OK(s);

  s = db->Put(write_options, "b", "b");
  ASSERT_OK(s);

  s = db->Put(write_options, "c", "c");
  ASSERT_OK(s);

  ASSERT_OK(batch.Put("a", "batch.a"));
  ASSERT_OK(batch.Delete("b"));

  s = batch.GetFromBatchAndDB(db, read_options, "a", &value);
  ASSERT_OK(s);
  ASSERT_EQ("batch.a", value);

  s = batch.GetFromBatchAndDB(db, read_options, "b", &value);
  ASSERT_TRUE(s.IsNotFound());

  s = batch.GetFromBatchAndDB(db, read_options, "c", &value);
  ASSERT_OK(s);
  ASSERT_EQ("c", value);

  s = batch.GetFromBatchAndDB(db, read_options, "x", &value);
  ASSERT_TRUE(s.IsNotFound());

  ASSERT_OK(db->Delete(write_options, "x"));

  s = batch.GetFromBatchAndDB(db, read_options, "x", &value);
  ASSERT_TRUE(s.IsNotFound());

  delete db;
  EXPECT_OK(DestroyDB(dbname, options));
}

TEST_F(WriteBatchWithIndexTest, TestGetFromBatchAndDBMerge) {
  DB* db;
  Options options;

  options.create_if_missing = true;
  std::string dbname = test::PerThreadDBPath("write_batch_with_index_test");

  options.merge_operator = MergeOperators::CreateFromStringId("stringappend");

  EXPECT_OK(DestroyDB(dbname, options));
  Status s = DB::Open(options, dbname, &db);
  ASSERT_OK(s);

  WriteBatchWithIndex batch;
  ReadOptions read_options;
  WriteOptions write_options;
  std::string value;

  s = db->Put(write_options, "a", "a0");
  ASSERT_OK(s);

  s = db->Put(write_options, "b", "b0");
  ASSERT_OK(s);

  s = db->Merge(write_options, "b", "b1");
  ASSERT_OK(s);

  s = db->Merge(write_options, "c", "c0");
  ASSERT_OK(s);

  s = db->Merge(write_options, "d", "d0");
  ASSERT_OK(s);

  ASSERT_OK(batch.Merge("a", "a1"));
  ASSERT_OK(batch.Merge("a", "a2"));
  ASSERT_OK(batch.Merge("b", "b2"));
  ASSERT_OK(batch.Merge("d", "d1"));
  ASSERT_OK(batch.Merge("e", "e0"));

  s = batch.GetFromBatchAndDB(db, read_options, "a", &value);
  ASSERT_OK(s);
  ASSERT_EQ("a0,a1,a2", value);

  s = batch.GetFromBatchAndDB(db, read_options, "b", &value);
  ASSERT_OK(s);
  ASSERT_EQ("b0,b1,b2", value);

  s = batch.GetFromBatchAndDB(db, read_options, "c", &value);
  ASSERT_OK(s);
  ASSERT_EQ("c0", value);

  s = batch.GetFromBatchAndDB(db, read_options, "d", &value);
  ASSERT_OK(s);
  ASSERT_EQ("d0,d1", value);

  s = batch.GetFromBatchAndDB(db, read_options, "e", &value);
  ASSERT_OK(s);
  ASSERT_EQ("e0", value);

  s = db->Delete(write_options, "x");
  ASSERT_OK(s);

  s = batch.GetFromBatchAndDB(db, read_options, "x", &value);
  ASSERT_TRUE(s.IsNotFound());

  const Snapshot* snapshot = db->GetSnapshot();
  ReadOptions snapshot_read_options;
  snapshot_read_options.snapshot = snapshot;

  s = db->Delete(write_options, "a");
  ASSERT_OK(s);

  s = batch.GetFromBatchAndDB(db, read_options, "a", &value);
  ASSERT_OK(s);
  ASSERT_EQ("a1,a2", value);

  s = batch.GetFromBatchAndDB(db, snapshot_read_options, "a", &value);
  ASSERT_OK(s);
  ASSERT_EQ("a0,a1,a2", value);

  ASSERT_OK(batch.Delete("a"));

  s = batch.GetFromBatchAndDB(db, read_options, "a", &value);
  ASSERT_TRUE(s.IsNotFound());

  s = batch.GetFromBatchAndDB(db, snapshot_read_options, "a", &value);
  ASSERT_TRUE(s.IsNotFound());

  s = db->Merge(write_options, "c", "c1");
  ASSERT_OK(s);

  s = batch.GetFromBatchAndDB(db, read_options, "c", &value);
  ASSERT_OK(s);
  ASSERT_EQ("c0,c1", value);

  s = batch.GetFromBatchAndDB(db, snapshot_read_options, "c", &value);
  ASSERT_OK(s);
  ASSERT_EQ("c0", value);

  s = db->Put(write_options, "e", "e1");
  ASSERT_OK(s);

  s = batch.GetFromBatchAndDB(db, read_options, "e", &value);
  ASSERT_OK(s);
  ASSERT_EQ("e1,e0", value);

  s = batch.GetFromBatchAndDB(db, snapshot_read_options, "e", &value);
  ASSERT_OK(s);
  ASSERT_EQ("e0", value);

  s = db->Delete(write_options, "e");
  ASSERT_OK(s);

  s = batch.GetFromBatchAndDB(db, read_options, "e", &value);
  ASSERT_OK(s);
  ASSERT_EQ("e0", value);

  s = batch.GetFromBatchAndDB(db, snapshot_read_options, "e", &value);
  ASSERT_OK(s);
  ASSERT_EQ("e0", value);

  db->ReleaseSnapshot(snapshot);
  delete db;
  EXPECT_OK(DestroyDB(dbname, options));
}

TEST_F(WriteBatchWithIndexTest, TestGetFromBatchAndDBMerge2) {
  DB* db;
  Options options;

  options.create_if_missing = true;
  std::string dbname = test::PerThreadDBPath("write_batch_with_index_test");

  options.merge_operator = MergeOperators::CreateFromStringId("stringappend");

  EXPECT_OK(DestroyDB(dbname, options));
  Status s = DB::Open(options, dbname, &db);
  ASSERT_OK(s);

  // Test batch with overwrite_key=true
  WriteBatchWithIndex batch(BytewiseComparator(), 0, true);

  ReadOptions read_options;
  WriteOptions write_options;
  std::string value;

  s = batch.GetFromBatchAndDB(db, read_options, "A", &value);
  ASSERT_TRUE(s.IsNotFound());

  ASSERT_OK(batch.Merge("A", "xxx"));

  s = batch.GetFromBatchAndDB(db, read_options, "A", &value);
  ASSERT_TRUE(s.IsMergeInProgress());

  ASSERT_OK(batch.Merge("A", "yyy"));

  s = batch.GetFromBatchAndDB(db, read_options, "A", &value);
  ASSERT_TRUE(s.IsMergeInProgress());

  s = db->Put(write_options, "A", "a0");
  ASSERT_OK(s);

  s = batch.GetFromBatchAndDB(db, read_options, "A", &value);
  ASSERT_TRUE(s.IsMergeInProgress());

  ASSERT_OK(batch.Delete("A"));

  s = batch.GetFromBatchAndDB(db, read_options, "A", &value);
  ASSERT_TRUE(s.IsNotFound());

  delete db;
  EXPECT_OK(DestroyDB(dbname, options));
}

TEST_F(WriteBatchWithIndexTest, TestGetFromBatchAndDBMerge3) {
  DB* db;
  Options options;

  options.create_if_missing = true;
  std::string dbname = test::PerThreadDBPath("write_batch_with_index_test");

  options.merge_operator = MergeOperators::CreateFromStringId("stringappend");

  EXPECT_OK(DestroyDB(dbname, options));
  Status s = DB::Open(options, dbname, &db);
  ASSERT_OK(s);

  ReadOptions read_options;
  WriteOptions write_options;
  FlushOptions flush_options;
  std::string value;

  WriteBatchWithIndex batch;

  ASSERT_OK(db->Put(write_options, "A", "1"));
  ASSERT_OK(db->Flush(flush_options, db->DefaultColumnFamily()));
  ASSERT_OK(batch.Merge("A", "2"));

  ASSERT_OK(batch.GetFromBatchAndDB(db, read_options, "A", &value));
  ASSERT_EQ(value, "1,2");

  delete db;
  EXPECT_OK(DestroyDB(dbname, options));
}

void AssertKey(std::string key, WBWIIterator* iter) {
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(key, iter->Entry().key.ToString());
}

void AssertValue(std::string value, WBWIIterator* iter) {
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(value, iter->Entry().value.ToString());
}

// Tests that we can write to the WBWI while we iterate (from a single thread).
// iteration should see the newest writes
TEST_F(WriteBatchWithIndexTest, MutateWhileIteratingCorrectnessTest) {
  WriteBatchWithIndex batch(BytewiseComparator(), 0, true);
  for (char c = 'a'; c <= 'z'; ++c) {
    ASSERT_OK(batch.Put(std::string(1, c), std::string(1, c)));
  }

  std::unique_ptr<WBWIIterator> iter(batch.NewIterator());
  iter->Seek("k");
  AssertKey("k", iter.get());
  iter->Next();
  AssertKey("l", iter.get());
  ASSERT_OK(batch.Put("ab", "cc"));
  iter->Next();
  AssertKey("m", iter.get());
  ASSERT_OK(batch.Put("mm", "kk"));
  iter->Next();
  AssertKey("mm", iter.get());
  AssertValue("kk", iter.get());
  ASSERT_OK(batch.Delete("mm"));

  iter->Next();
  AssertKey("n", iter.get());
  iter->Prev();
  AssertKey("mm", iter.get());
  ASSERT_EQ(kDeleteRecord, iter->Entry().type);

  iter->Seek("ab");
  AssertKey("ab", iter.get());
  ASSERT_OK(batch.Delete("x"));
  iter->Seek("x");
  AssertKey("x", iter.get());
  ASSERT_EQ(kDeleteRecord, iter->Entry().type);
  iter->Prev();
  AssertKey("w", iter.get());
}

void AssertIterKey(std::string key, Iterator* iter) {
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(key, iter->key().ToString());
}

void AssertIterValue(std::string value, Iterator* iter) {
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(value, iter->value().ToString());
}

// same thing as above, but testing IteratorWithBase
TEST_F(WriteBatchWithIndexTest, MutateWhileIteratingBaseCorrectnessTest) {
  WriteBatchWithIndex batch(BytewiseComparator(), 0, true);
  for (char c = 'a'; c <= 'z'; ++c) {
    ASSERT_OK(batch.Put(std::string(1, c), std::string(1, c)));
  }

  KVMap map;
  map["aa"] = "aa";
  map["cc"] = "cc";
  map["ee"] = "ee";
  map["em"] = "me";

  std::unique_ptr<Iterator> iter(
      batch.NewIteratorWithBase(new KVIter(&map)));
  iter->Seek("k");
  AssertIterKey("k", iter.get());
  iter->Next();
  AssertIterKey("l", iter.get());
  ASSERT_OK(batch.Put("ab", "cc"));
  iter->Next();
  AssertIterKey("m", iter.get());
  ASSERT_OK(batch.Put("mm", "kk"));
  iter->Next();
  AssertIterKey("mm", iter.get());
  AssertIterValue("kk", iter.get());
  ASSERT_OK(batch.Delete("mm"));
  iter->Next();
  AssertIterKey("n", iter.get());
  iter->Prev();
  // "mm" is deleted, so we're back at "m"
  AssertIterKey("m", iter.get());

  iter->Seek("ab");
  AssertIterKey("ab", iter.get());
  iter->Prev();
  AssertIterKey("aa", iter.get());
  iter->Prev();
  AssertIterKey("a", iter.get());
  ASSERT_OK(batch.Delete("aa"));
  iter->Next();
  AssertIterKey("ab", iter.get());
  iter->Prev();
  AssertIterKey("a", iter.get());

  ASSERT_OK(batch.Delete("x"));
  iter->Seek("x");
  AssertIterKey("y", iter.get());
  iter->Next();
  AssertIterKey("z", iter.get());
  iter->Prev();
  iter->Prev();
  AssertIterKey("w", iter.get());

  ASSERT_OK(batch.Delete("e"));
  iter->Seek("e");
  AssertIterKey("ee", iter.get());
  AssertIterValue("ee", iter.get());
  ASSERT_OK(batch.Put("ee", "xx"));
  // still the same value
  AssertIterValue("ee", iter.get());
  iter->Next();
  AssertIterKey("em", iter.get());
  iter->Prev();
  // new value
  AssertIterValue("xx", iter.get());

  ASSERT_OK(iter->status());
}

// stress testing mutations with IteratorWithBase
TEST_F(WriteBatchWithIndexTest, MutateWhileIteratingBaseStressTest) {
  WriteBatchWithIndex batch(BytewiseComparator(), 0, true);
  for (char c = 'a'; c <= 'z'; ++c) {
    ASSERT_OK(batch.Put(std::string(1, c), std::string(1, c)));
  }

  KVMap map;
  for (char c = 'a'; c <= 'z'; ++c) {
    map[std::string(2, c)] = std::string(2, c);
  }

  std::unique_ptr<Iterator> iter(
      batch.NewIteratorWithBase(new KVIter(&map)));

  Random rnd(301);
  for (int i = 0; i < 1000000; ++i) {
    int random = rnd.Uniform(8);
    char c = static_cast<char>(rnd.Uniform(26) + 'a');
    switch (random) {
      case 0:
        ASSERT_OK(batch.Put(std::string(1, c), "xxx"));
        break;
      case 1:
        ASSERT_OK(batch.Put(std::string(2, c), "xxx"));
        break;
      case 2:
        ASSERT_OK(batch.Delete(std::string(1, c)));
        break;
      case 3:
        ASSERT_OK(batch.Delete(std::string(2, c)));
        break;
      case 4:
        iter->Seek(std::string(1, c));
        break;
      case 5:
        iter->Seek(std::string(2, c));
        break;
      case 6:
        if (iter->Valid()) {
          iter->Next();
        }
        break;
      case 7:
        if (iter->Valid()) {
          iter->Prev();
        }
        break;
      default:
        assert(false);
    }
  }
  ASSERT_OK(iter->status());
}

static void PrintContents(WriteBatchWithIndex* batch,
                          ColumnFamilyHandle* column_family,
                          std::string* result) {
  WBWIIterator* iter;
  if (column_family == nullptr) {
    iter = batch->NewIterator();
  } else {
    iter = batch->NewIterator(column_family);
  }

  iter->SeekToFirst();
  while (iter->Valid()) {
    ASSERT_OK(iter->status());

    WriteEntry e = iter->Entry();

    if (e.type == kPutRecord) {
      result->append("PUT(");
      result->append(e.key.ToString());
      result->append("):");
      result->append(e.value.ToString());
    } else if (e.type == kMergeRecord) {
      result->append("MERGE(");
      result->append(e.key.ToString());
      result->append("):");
      result->append(e.value.ToString());
    } else if (e.type == kSingleDeleteRecord) {
      result->append("SINGLE-DEL(");
      result->append(e.key.ToString());
      result->append(")");
    } else {
      assert(e.type == kDeleteRecord);
      result->append("DEL(");
      result->append(e.key.ToString());
      result->append(")");
    }

    result->append(",");
    iter->Next();
  }

  ASSERT_OK(iter->status());

  delete iter;
}

static std::string PrintContents(WriteBatchWithIndex* batch,
                                 ColumnFamilyHandle* column_family) {
  std::string result;
  PrintContents(batch, column_family, &result);
  return result;
}

static void PrintContents(WriteBatchWithIndex* batch, KVMap* base_map,
                          ColumnFamilyHandle* column_family,
                          std::string* result) {
  Iterator* iter;
  if (column_family == nullptr) {
    iter = batch->NewIteratorWithBase(new KVIter(base_map));
  } else {
    iter = batch->NewIteratorWithBase(column_family, new KVIter(base_map));
  }

  iter->SeekToFirst();
  while (iter->Valid()) {
    ASSERT_OK(iter->status());

    Slice key = iter->key();
    Slice value = iter->value();

    result->append(key.ToString());
    result->append(":");
    result->append(value.ToString());
    result->append(",");

    iter->Next();
  }

  ASSERT_OK(iter->status());

  delete iter;
}

static std::string PrintContents(WriteBatchWithIndex* batch, KVMap* base_map,
                                 ColumnFamilyHandle* column_family) {
  std::string result;
  PrintContents(batch, base_map, column_family, &result);
  return result;
}

TEST_F(WriteBatchWithIndexTest, SavePointTest) {
  WriteBatchWithIndex batch;
  ColumnFamilyHandleImplDummy cf1(1, BytewiseComparator());
  Status s;

  ASSERT_OK(batch.Put("A", "a"));
  ASSERT_OK(batch.Put("B", "b"));
  ASSERT_OK(batch.Put("A", "aa"));
  ASSERT_OK(batch.Put(&cf1, "A", "a1"));
  ASSERT_OK(batch.Delete(&cf1, "B"));
  ASSERT_OK(batch.Put(&cf1, "C", "c1"));
  ASSERT_OK(batch.Put(&cf1, "E", "e1"));

  batch.SetSavePoint();  // 1

  ASSERT_OK(batch.Put("C", "cc"));
  ASSERT_OK(batch.Put("B", "bb"));
  ASSERT_OK(batch.Delete("A"));
  ASSERT_OK(batch.Put(&cf1, "B", "b1"));
  ASSERT_OK(batch.Delete(&cf1, "A"));
  ASSERT_OK(batch.SingleDelete(&cf1, "E"));
  batch.SetSavePoint();  // 2

  ASSERT_OK(batch.Put("A", "aaa"));
  ASSERT_OK(batch.Put("A", "xxx"));
  ASSERT_OK(batch.Delete("B"));
  ASSERT_OK(batch.Put(&cf1, "B", "b2"));
  ASSERT_OK(batch.Delete(&cf1, "C"));
  batch.SetSavePoint();  // 3
  batch.SetSavePoint();  // 4
  ASSERT_OK(batch.SingleDelete("D"));
  ASSERT_OK(batch.Delete(&cf1, "D"));
  ASSERT_OK(batch.Delete(&cf1, "E"));

  ASSERT_EQ(
      "PUT(A):a,PUT(A):aa,DEL(A),PUT(A):aaa,PUT(A):xxx,PUT(B):b,PUT(B):bb,DEL("
      "B)"
      ",PUT(C):cc,SINGLE-DEL(D),",
      PrintContents(&batch, nullptr));

  ASSERT_EQ(
      "PUT(A):a1,DEL(A),DEL(B),PUT(B):b1,PUT(B):b2,PUT(C):c1,DEL(C),"
      "DEL(D),PUT(E):e1,SINGLE-DEL(E),DEL(E),",
      PrintContents(&batch, &cf1));

  ASSERT_OK(batch.RollbackToSavePoint());  // rollback to 4
  ASSERT_EQ(
      "PUT(A):a,PUT(A):aa,DEL(A),PUT(A):aaa,PUT(A):xxx,PUT(B):b,PUT(B):bb,DEL("
      "B)"
      ",PUT(C):cc,",
      PrintContents(&batch, nullptr));

  ASSERT_EQ(
      "PUT(A):a1,DEL(A),DEL(B),PUT(B):b1,PUT(B):b2,PUT(C):c1,DEL(C),"
      "PUT(E):e1,SINGLE-DEL(E),",
      PrintContents(&batch, &cf1));

  ASSERT_OK(batch.RollbackToSavePoint());  // rollback to 3
  ASSERT_EQ(
      "PUT(A):a,PUT(A):aa,DEL(A),PUT(A):aaa,PUT(A):xxx,PUT(B):b,PUT(B):bb,DEL("
      "B)"
      ",PUT(C):cc,",
      PrintContents(&batch, nullptr));

  ASSERT_EQ(
      "PUT(A):a1,DEL(A),DEL(B),PUT(B):b1,PUT(B):b2,PUT(C):c1,DEL(C),"
      "PUT(E):e1,SINGLE-DEL(E),",
      PrintContents(&batch, &cf1));

  ASSERT_OK(batch.RollbackToSavePoint());  // rollback to 2
  ASSERT_EQ("PUT(A):a,PUT(A):aa,DEL(A),PUT(B):b,PUT(B):bb,PUT(C):cc,",
            PrintContents(&batch, nullptr));

  ASSERT_EQ(
      "PUT(A):a1,DEL(A),DEL(B),PUT(B):b1,PUT(C):c1,"
      "PUT(E):e1,SINGLE-DEL(E),",
      PrintContents(&batch, &cf1));

  batch.SetSavePoint();  // 5
  ASSERT_OK(batch.Put("X", "x"));

  ASSERT_EQ("PUT(A):a,PUT(A):aa,DEL(A),PUT(B):b,PUT(B):bb,PUT(C):cc,PUT(X):x,",
            PrintContents(&batch, nullptr));

  ASSERT_OK(batch.RollbackToSavePoint());  // rollback to 5
  ASSERT_EQ("PUT(A):a,PUT(A):aa,DEL(A),PUT(B):b,PUT(B):bb,PUT(C):cc,",
            PrintContents(&batch, nullptr));

  ASSERT_EQ(
      "PUT(A):a1,DEL(A),DEL(B),PUT(B):b1,PUT(C):c1,"
      "PUT(E):e1,SINGLE-DEL(E),",
      PrintContents(&batch, &cf1));

  ASSERT_OK(batch.RollbackToSavePoint());  // rollback to 1
  ASSERT_EQ("PUT(A):a,PUT(A):aa,PUT(B):b,", PrintContents(&batch, nullptr));

  ASSERT_EQ("PUT(A):a1,DEL(B),PUT(C):c1,PUT(E):e1,",
            PrintContents(&batch, &cf1));

  s = batch.RollbackToSavePoint();  // no savepoint found
  ASSERT_TRUE(s.IsNotFound());
  ASSERT_EQ("PUT(A):a,PUT(A):aa,PUT(B):b,", PrintContents(&batch, nullptr));

  ASSERT_EQ("PUT(A):a1,DEL(B),PUT(C):c1,PUT(E):e1,",
            PrintContents(&batch, &cf1));

  batch.SetSavePoint();  // 6

  batch.Clear();
  ASSERT_EQ("", PrintContents(&batch, nullptr));
  ASSERT_EQ("", PrintContents(&batch, &cf1));

  s = batch.RollbackToSavePoint();  // rollback to 6
  ASSERT_TRUE(s.IsNotFound());
}

TEST_F(WriteBatchWithIndexTest, SingleDeleteTest) {
  WriteBatchWithIndex batch;
  Status s;
  std::string value;
  DBOptions db_options;

  ASSERT_OK(batch.SingleDelete("A"));

  s = batch.GetFromBatch(db_options, "A", &value);
  ASSERT_TRUE(s.IsNotFound());
  s = batch.GetFromBatch(db_options, "B", &value);
  ASSERT_TRUE(s.IsNotFound());
  value = PrintContents(&batch, nullptr);
  ASSERT_EQ("SINGLE-DEL(A),", value);

  batch.Clear();
  ASSERT_OK(batch.Put("A", "a"));
  ASSERT_OK(batch.Put("A", "a2"));
  ASSERT_OK(batch.Put("B", "b"));
  ASSERT_OK(batch.SingleDelete("A"));

  s = batch.GetFromBatch(db_options, "A", &value);
  ASSERT_TRUE(s.IsNotFound());
  s = batch.GetFromBatch(db_options, "B", &value);
  ASSERT_OK(s);
  ASSERT_EQ("b", value);

  value = PrintContents(&batch, nullptr);
  ASSERT_EQ("PUT(A):a,PUT(A):a2,SINGLE-DEL(A),PUT(B):b,", value);

  ASSERT_OK(batch.Put("C", "c"));
  ASSERT_OK(batch.Put("A", "a3"));
  ASSERT_OK(batch.Delete("B"));
  ASSERT_OK(batch.SingleDelete("B"));
  ASSERT_OK(batch.SingleDelete("C"));

  s = batch.GetFromBatch(db_options, "A", &value);
  ASSERT_OK(s);
  ASSERT_EQ("a3", value);
  s = batch.GetFromBatch(db_options, "B", &value);
  ASSERT_TRUE(s.IsNotFound());
  s = batch.GetFromBatch(db_options, "C", &value);
  ASSERT_TRUE(s.IsNotFound());
  s = batch.GetFromBatch(db_options, "D", &value);
  ASSERT_TRUE(s.IsNotFound());

  value = PrintContents(&batch, nullptr);
  ASSERT_EQ(
      "PUT(A):a,PUT(A):a2,SINGLE-DEL(A),PUT(A):a3,PUT(B):b,DEL(B),SINGLE-DEL(B)"
      ",PUT(C):c,SINGLE-DEL(C),",
      value);

  ASSERT_OK(batch.Put("B", "b4"));
  ASSERT_OK(batch.Put("C", "c4"));
  ASSERT_OK(batch.Put("D", "d4"));
  ASSERT_OK(batch.SingleDelete("D"));
  ASSERT_OK(batch.SingleDelete("D"));
  ASSERT_OK(batch.Delete("A"));

  s = batch.GetFromBatch(db_options, "A", &value);
  ASSERT_TRUE(s.IsNotFound());
  s = batch.GetFromBatch(db_options, "B", &value);
  ASSERT_OK(s);
  ASSERT_EQ("b4", value);
  s = batch.GetFromBatch(db_options, "C", &value);
  ASSERT_OK(s);
  ASSERT_EQ("c4", value);
  s = batch.GetFromBatch(db_options, "D", &value);
  ASSERT_TRUE(s.IsNotFound());

  value = PrintContents(&batch, nullptr);
  ASSERT_EQ(
      "PUT(A):a,PUT(A):a2,SINGLE-DEL(A),PUT(A):a3,DEL(A),PUT(B):b,DEL(B),"
      "SINGLE-DEL(B),PUT(B):b4,PUT(C):c,SINGLE-DEL(C),PUT(C):c4,PUT(D):d4,"
      "SINGLE-DEL(D),SINGLE-DEL(D),",
      value);
}

TEST_F(WriteBatchWithIndexTest, SingleDeleteDeltaIterTest) {
  std::string value;
  DBOptions db_options;
  WriteBatchWithIndex batch(BytewiseComparator(), 20, true /* overwrite_key */);

  ASSERT_OK(batch.Put("A", "a"));
  ASSERT_OK(batch.Put("A", "a2"));
  ASSERT_OK(batch.Put("B", "b"));
  ASSERT_OK(batch.SingleDelete("A"));
  ASSERT_OK(batch.Delete("B"));

  KVMap map;
  value = PrintContents(&batch, &map, nullptr);
  ASSERT_EQ("", value);

  map["A"] = "aa";
  map["C"] = "cc";
  map["D"] = "dd";

  ASSERT_OK(batch.SingleDelete("B"));
  ASSERT_OK(batch.SingleDelete("C"));
  ASSERT_OK(batch.SingleDelete("Z"));

  value = PrintContents(&batch, &map, nullptr);
  ASSERT_EQ("D:dd,", value);

  ASSERT_OK(batch.Put("A", "a3"));
  ASSERT_OK(batch.Put("B", "b3"));
  ASSERT_OK(batch.SingleDelete("A"));
  ASSERT_OK(batch.SingleDelete("A"));
  ASSERT_OK(batch.SingleDelete("D"));
  ASSERT_OK(batch.SingleDelete("D"));
  ASSERT_OK(batch.Delete("D"));

  map["E"] = "ee";

  value = PrintContents(&batch, &map, nullptr);
  ASSERT_EQ("B:b3,E:ee,", value);
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
