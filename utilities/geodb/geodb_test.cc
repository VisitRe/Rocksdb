//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

//
#ifndef ROCKSDB_LITE
#include "utilities/geodb/geodb_impl.h"

#include <cctype>
#include "util/testharness.h"

namespace rocksdb {

class GeoDBTest : public testing::Test {
 public:
  static const std::string kDefaultDbName;
  static Options options;
  DB* db;
  GeoDB* geodb;

  GeoDBTest() {
    GeoDBOptions geodb_options;
    EXPECT_OK(DestroyDB(kDefaultDbName, options));
    options.create_if_missing = true;
    Status status = DB::Open(options, kDefaultDbName, &db);
    geodb =  new GeoDBImpl(db, geodb_options);
  }

  ~GeoDBTest() {
    delete geodb;
  }

  GeoDB* getdb() {
    return geodb;
  }
};

const std::string GeoDBTest::kDefaultDbName = test::TmpDir() + "/geodb_test";
Options GeoDBTest::options = Options();

// Insert, Get and Remove
TEST_F(GeoDBTest, SimpleTest) {
  GeoPosition pos1(100, 101);
  std::string id1("id1");
  std::string value1("value1");

  // insert first object into database
  GeoObject obj1(pos1, id1, value1);
  Status status = getdb()->Insert(obj1);
  ASSERT_TRUE(status.ok());

  // insert second object into database
  GeoPosition pos2(200, 201);
  std::string id2("id2");
  std::string value2 = "value2";
  GeoObject obj2(pos2, id2, value2);
  status = getdb()->Insert(obj2);
  ASSERT_TRUE(status.ok());

  // retrieve first object using position
  std::string value;
  status = getdb()->GetByPosition(pos1, Slice(id1), &value);
  ASSERT_TRUE(status.ok());
  ASSERT_EQ(value, value1);

  // retrieve first object using id
  GeoObject obj;
  status = getdb()->GetById(Slice(id1), &obj);
  ASSERT_TRUE(status.ok());
  ASSERT_EQ(obj.position.latitude, 100);
  ASSERT_EQ(obj.position.longitude, 101);
  ASSERT_EQ(obj.id.compare(id1), 0);
  ASSERT_EQ(obj.value, value1);

  // delete first object
  status = getdb()->Remove(Slice(id1));
  ASSERT_TRUE(status.ok());
  status = getdb()->GetByPosition(pos1, Slice(id1), &value);
  ASSERT_TRUE(status.IsNotFound());
  status = getdb()->GetById(id1, &obj);
  ASSERT_TRUE(status.IsNotFound());

  // check that we can still find second object
  status = getdb()->GetByPosition(pos2, id2, &value);
  ASSERT_TRUE(status.ok());
  ASSERT_EQ(value, value2);
  status = getdb()->GetById(id2, &obj);
  ASSERT_TRUE(status.ok());
}

// Search.
// Verify distances via http://www.stevemorse.org/nearest/distance.php
TEST_F(GeoDBTest, Search) {
  GeoPosition pos1(45, 45);
  std::string id1("mid1");
  std::string value1 = "midvalue1";

  // insert object at 45 degree latitude
  GeoObject obj1(pos1, id1, value1);
  Status status = getdb()->Insert(obj1);
  ASSERT_TRUE(status.ok());

  // search all objects centered at 46 degree latitude with
  // a radius of 200 kilometers. We should find the one object that
  // we inserted earlier.
  GeoIterator* iter1 = getdb()->SearchRadial(GeoPosition(46, 46), 200000);
  ASSERT_TRUE(status.ok());
  ASSERT_EQ(iter1->geo_object().value, "midvalue1");
  uint32_t size = 0;
  while (iter1->Valid()) {
    size++;
    iter1->Next();
  }
  ASSERT_EQ(size, 1U);
  delete iter1;

  // search all objects centered at 46 degree latitude with
  // a radius of 2 kilometers. There should be none.
  GeoIterator* iter2 = getdb()->SearchRadial(GeoPosition(46, 46), 2);
  ASSERT_TRUE(status.ok());
  ASSERT_FALSE(iter2->Valid());
  delete iter2;
}

}  // namespace rocksdb

int main(int argc, char* argv[]) {
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
