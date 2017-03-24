// Copyright (c) 2017 Rockset

#ifndef ROCKSDB_LITE

#ifdef USE_AWS

#include "rocksdb/status.h"
#include "rocksdb/options.h"
#include "rocksdb/cloud/db_cloud.h"
#include "util/testharness.h"
#include "util/logging.h"
#include "cloud/aws/aws_env.h"
#include "cloud/db_cloud_impl.h"
#ifndef OS_WIN
#include <unistd.h>
#endif

namespace rocksdb {

class CloudTest : public testing::Test {
 public:
  CloudTest() {
    base_env_ = Env::Default();
    dbname_ = test::TmpDir() + "/db_cloud";
    clone_dir_ = test::TmpDir() + "/ctest";
    src_bucket_prefix_ = "dbcloud." + AwsEnv::GetTestBucketSuffix();
    src_object_prefix_ = dbname_;
    options_.create_if_missing = true;
    db_ = nullptr;
    aenv_ = nullptr;
    DestroyDB(dbname_, Options());
    CreateLoggerFromOptions(dbname_, options_, &options_.info_log);

    // Get cloud credentials
    AwsEnv::GetTestCredentials(
              &cloud_env_options_.credentials.access_key_id,
              &cloud_env_options_.credentials.secret_key,
              &cloud_env_options_.region);
    Cleanup();
  }

  void Cleanup() {
    ASSERT_TRUE(!aenv_);

    // create a dummy aws env 
    ASSERT_OK(CloudEnv::NewAwsEnv(base_env_,
			          src_bucket_prefix_,
			          src_object_prefix_,
			          dest_bucket_prefix_,
			          dest_object_prefix_,
		                  cloud_env_options_,
				  options_.info_log,
				  &aenv_));
    // delete all pre-existing contents from the bucket
    Status st = aenv_->EmptyBucket(src_bucket_prefix_);
    ASSERT_TRUE(st.ok() || st.IsNotFound());
    delete aenv_;
    aenv_ = nullptr;

    // delete and create directory where clones reside
    DestroyDir(clone_dir_);
    ASSERT_OK(base_env_->CreateDir(clone_dir_));
  }

  void DestroyDir(const std::string& dir) {
    std::string cmd = "rm -rf " + dir;
    int rc = system(cmd.c_str());
    ASSERT_EQ(rc, 0);
  }

  virtual ~CloudTest() {
    CloseDB();
    DestroyDB(dbname_, Options());
    DestroyDir(clone_dir_);
  }

  // Open database via the cloud interface
  void OpenDB() {
    ASSERT_NE(cloud_env_options_.credentials.access_key_id.size(), 0);
    ASSERT_NE(cloud_env_options_.credentials.secret_key.size(), 0);

    // Create new AWS env
    ASSERT_OK(CloudEnv::NewAwsEnv(base_env_,
			          src_bucket_prefix_,
			          src_object_prefix_,
			          src_bucket_prefix_,
			          src_object_prefix_,
		                  cloud_env_options_,
				  options_.info_log,
				  &aenv_));
    options_.env = aenv_;

    // default column family
    ColumnFamilyOptions cfopt = options_;
    std::vector<ColumnFamilyDescriptor> column_families;
    column_families.emplace_back(
      ColumnFamilyDescriptor(kDefaultColumnFamilyName, cfopt));
    std::vector<ColumnFamilyHandle*> handles;

    ASSERT_TRUE(db_ == nullptr);
    ASSERT_OK(DBCloud::Open(options_, dbname_,
			    column_families, &handles,
			    &db_));
    ASSERT_OK(db_->GetDbIdentity(dbid_));

    // Delete the handle for the default column family because the DBImpl
    // always holds a reference to it.
    ASSERT_TRUE(handles.size() > 0);
    delete handles[0];
  }

  // Creates and Opens a clone
  void CloneDB(const std::string& clone_name,
	       const std::string& src_bucket,
	       const std::string& src_object_path,
	       const std::string& dest_bucket,
	       const std::string& dest_object_path,
	       std::unique_ptr<DBCloud>* cloud_db,
	       std::unique_ptr<CloudEnv>* cloud_env) {

    // The local directory where the clone resides
    std::string cname = clone_dir_ + "/" + clone_name;

    CloudEnv* cenv;
    DBCloud* clone_db;

    // Create new AWS env
    ASSERT_OK(CloudEnv::NewAwsEnv(base_env_,
			          src_bucket,
			          src_object_path,
			          dest_bucket,
			          dest_object_path,
		                  cloud_env_options_,
				  options_.info_log,
				  &cenv));

    // sets the cloud env to be used by the env wrapper
    options_.env = cenv;

    // Returns the cloud env that was created
    cloud_env->reset(cenv);

    // default column family
    ColumnFamilyOptions cfopt = options_;

    std::vector<ColumnFamilyDescriptor> column_families;
    column_families.emplace_back(
      ColumnFamilyDescriptor(kDefaultColumnFamilyName, cfopt));
    std::vector<ColumnFamilyHandle*> handles;

    ASSERT_OK(DBCloud::Open(options_, cname,
			    column_families, &handles,
			    &clone_db));
    cloud_db->reset(clone_db);

    // Delete the handle for the default column family because the DBImpl
    // always holds a reference to it.
    ASSERT_TRUE(handles.size() > 0);
    delete handles[0];
  }

  void CloseDB() {
    if (db_) {
      db_->Flush(FlushOptions());  // convert pending writes to sst files
      delete db_;
      db_ = nullptr;
    }
    if (aenv_) {
      delete aenv_;
      aenv_ = nullptr;
    }
  }

 protected:
  Env* base_env_;
  Options options_;
  std::string dbname_;
  std::string clone_dir_;
  std::string src_bucket_prefix_;
  std::string src_object_prefix_;
  std::string dest_bucket_prefix_;
  std::string dest_object_prefix_;
  CloudEnvOptions cloud_env_options_;
  std::string dbid_;
  DBCloud* db_;
  CloudEnv* aenv_;
};

//
// Most basic test. Create DB, write one key, close it and then check to see
// that the key exists.
//
TEST_F(CloudTest, BasicTest) {

  // Put one key-value
  OpenDB();
  std::string value;
  ASSERT_OK(db_->Put(WriteOptions(), "Hello", "World"));
  ASSERT_OK(db_->Get(ReadOptions(), "Hello", &value));
  ASSERT_TRUE(value.compare("World") == 0);
  CloseDB();
  value.clear();

  // Reopen and validate
  OpenDB();
  ASSERT_OK(db_->Get(ReadOptions(), "Hello", &value));
  ASSERT_EQ(value, "World");
  CloseDB();
}

//
// Create and read from a clone.
//
TEST_F(CloudTest, Newdb) {
  std::string master_dbid;
  std::string newdb1_dbid;
  std::string newdb2_dbid;

  // Put one key-value
  OpenDB();
  std::string value;
  ASSERT_OK(db_->Put(WriteOptions(), "Hello", "World"));
  ASSERT_OK(db_->Get(ReadOptions(), "Hello", &value));
  ASSERT_TRUE(value.compare("World") == 0);
  ASSERT_OK(db_->GetDbIdentity(master_dbid));
  CloseDB();
  value.clear();

  {
    // Create and Open  a new instance
    std::unique_ptr<CloudEnv> cloud_env;
    std::unique_ptr<DBCloud> cloud_db;
    CloneDB("newdb1",
	    src_bucket_prefix_, src_object_prefix_,
	    dest_bucket_prefix_, dest_object_prefix_,
	    &cloud_db, &cloud_env);

    // Retrieve the id of the first reopen
    ASSERT_OK(cloud_db->GetDbIdentity(newdb1_dbid));

    // This reopen has the same src and destination paths, so it is
    // not a clone, but just a reopen.
    ASSERT_EQ(newdb1_dbid, master_dbid);

    ASSERT_OK(cloud_db->Get(ReadOptions(), "Hello", &value));
    ASSERT_TRUE(value.compare("World") == 0);

    // Open master and write one more kv to it. The dest bukcet is emty,
    // so writes go to local dir only.
    OpenDB();
    ASSERT_OK(db_->Put(WriteOptions(), "Dhruba", "Borthakur"));

    // check that the newly written kv exists
    value.clear();
    ASSERT_OK(db_->Get(ReadOptions(), "Dhruba", &value));
    ASSERT_TRUE(value.compare("Borthakur") == 0);

    // check that the earlier kv exists too
    value.clear();
    ASSERT_OK(db_->Get(ReadOptions(), "Hello", &value));
    ASSERT_TRUE(value.compare("World") == 0);
    CloseDB();

    // Assert  that newdb1 cannot see the second kv because the second kv
    // was written to local dir only.
    ASSERT_TRUE(cloud_db->Get(ReadOptions(), "Dhruba", &value).IsNotFound());
  }
  {
    // Create another instance using a different local dir but the same two
    // buckets as newdb1. This should be identical in contents with newdb1.
    std::unique_ptr<CloudEnv> cloud_env;
    std::unique_ptr<DBCloud> cloud_db;
    CloneDB("newdb2",
	    src_bucket_prefix_, src_object_prefix_,
	    dest_bucket_prefix_, dest_object_prefix_,
            &cloud_db, &cloud_env);

    // Retrieve the id of the second clone db
    ASSERT_OK(cloud_db->GetDbIdentity(newdb2_dbid));

    // Since we used the same src and destination buckets & paths for both
    // newdb1 and newdb2, we should get the same dbid as newdb1
    ASSERT_EQ(newdb1_dbid, newdb2_dbid);

    // check that both the kvs appear in the clone
    value.clear();
    ASSERT_OK(cloud_db->Get(ReadOptions(), "Hello", &value));
    ASSERT_TRUE(value.compare("World") == 0);
    value.clear();
    ASSERT_OK(cloud_db->Get(ReadOptions(), "Dhruba", &value));
    ASSERT_TRUE(value.compare("Borthakur") == 0);
  }
}

//
// Create and read from a clone.
//
TEST_F(CloudTest, TrueClone) {
  std::string master_dbid;
  std::string newdb1_dbid;
  std::string newdb2_dbid;
  std::string newdb3_dbid;

  // Put one key-value
  OpenDB();
  std::string value;
  ASSERT_OK(db_->Put(WriteOptions(), "Hello", "World"));
  ASSERT_OK(db_->Get(ReadOptions(), "Hello", &value));
  ASSERT_TRUE(value.compare("World") == 0);
  ASSERT_OK(db_->GetDbIdentity(master_dbid));
  CloseDB();
  value.clear();
  {
    // Create a new instance with different src and destination paths.
    // This is true clone and should have all the contents of the masterdb
    std::unique_ptr<CloudEnv> cloud_env;
    std::unique_ptr<DBCloud> cloud_db;
    CloneDB("localpath1",
	    src_bucket_prefix_, src_object_prefix_,
	    src_bucket_prefix_, "clone1_path",
            &cloud_db, &cloud_env);

    // Retrieve the id of the clone db
    ASSERT_OK(cloud_db->GetDbIdentity(newdb1_dbid));

    // Since we used the different src and destination paths for both
    // the master and clone1, the clone should have its own identity.
    ASSERT_NE(master_dbid, newdb1_dbid);

    // check that the original kv appears in the clone
    value.clear();
    ASSERT_OK(cloud_db->Get(ReadOptions(), "Hello", &value));
    ASSERT_TRUE(value.compare("World") == 0);

    // write a new value to the clone
    ASSERT_OK(cloud_db->Put(WriteOptions(), "Hello", "Clone1"));
    value.clear();
    ASSERT_OK(cloud_db->Get(ReadOptions(), "Hello", &value));
    ASSERT_TRUE(value.compare("Clone1") == 0);
  }
  {
    // Reopen clone1 with a different local path
    std::unique_ptr<CloudEnv> cloud_env;
    std::unique_ptr<DBCloud> cloud_db;
    CloneDB("localpath2",
	    src_bucket_prefix_, src_object_prefix_,
	    src_bucket_prefix_, "clone1_path",
            &cloud_db, &cloud_env);

    // Retrieve the id of the clone db
    ASSERT_OK(cloud_db->GetDbIdentity(newdb2_dbid));
    ASSERT_EQ(newdb1_dbid, newdb2_dbid);
    value.clear();
    ASSERT_OK(cloud_db->Get(ReadOptions(), "Hello", &value));
    ASSERT_TRUE(value.compare("Clone1") == 0);
  }
  {
    // Create clone2
    std::unique_ptr<CloudEnv> cloud_env;
    std::unique_ptr<DBCloud> cloud_db;
    CloneDB("localpath3", // xxx try with localpath2
	    src_bucket_prefix_, src_object_prefix_,
	    src_bucket_prefix_, "clone2_path",
            &cloud_db, &cloud_env);

    // Retrieve the id of the clone db
    ASSERT_OK(cloud_db->GetDbIdentity(newdb3_dbid));
    ASSERT_NE(newdb2_dbid, newdb3_dbid);

    // verify that data is still as it was in the original db.
    value.clear();
    ASSERT_OK(cloud_db->Get(ReadOptions(), "Hello", &value));
    ASSERT_TRUE(value.compare("World") == 0);
  }
}

//
// verify that dbid registry is appropriately handled
//
TEST_F(CloudTest, DbidRegistry) {

  // Put one key-value
  OpenDB();
  std::string value;
  ASSERT_OK(db_->Put(WriteOptions(), "Hello", "World"));
  ASSERT_OK(db_->Get(ReadOptions(), "Hello", &value));
  ASSERT_TRUE(value.compare("World") == 0);

  // Assert that there is one db in the registry
  while (true) {
    DbidList dbs;
    ASSERT_OK(aenv_->GetDbidList(src_bucket_prefix_, &dbs));
    if (dbs.size() == 0) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  CloseDB();
}

} //  namespace rocksdb

// A black-box test for the cloud wrapper around rocksdb
int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

#else // USE_AWS

#include <stdio.h>

int main(int argc, char** argv) {
  fprintf(stderr,
          "SKIPPED as DBCloud is supported only when USE_AWS is defined.\n");
  return 0;
}
#endif

#else // ROCKSDB_LITE

#include <stdio.h>

int main(int argc, char** argv) {
  fprintf(stderr, "SKIPPED as DBCloud is not supported in ROCKSDB_LITE\n");
  return 0;
}

#endif  // !ROCKSDB_LITE
