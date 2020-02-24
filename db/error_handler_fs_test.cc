//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
#ifndef ROCKSDB_LITE

#include "db/db_test_util.h"
#include "port/stack_trace.h"
#include "rocksdb/io_status.h"
#include "rocksdb/perf_context.h"
#include "rocksdb/sst_file_manager.h"
#include "test_util/fault_injection_test_env.h"
#include "test_util/fault_injection_test_fs.h"
#if !defined(ROCKSDB_LITE)
#include "test_util/sync_point.h"
#endif

namespace ROCKSDB_NAMESPACE {

class DBErrorHandlingFSTest : public DBTestBase {
 public:
  DBErrorHandlingFSTest() : DBTestBase("/db_error_handling_fs_test") {}

  std::string GetManifestNameFromLiveFiles() {
    std::vector<std::string> live_files;
    uint64_t manifest_size;

    dbfull()->GetLiveFiles(live_files, &manifest_size, false);
    for (auto& file : live_files) {
      uint64_t num = 0;
      FileType type;
      if (ParseFileName(file, &num, &type) && type == kDescriptorFile) {
        return file;
      }
    }
    return "";
  }
};

class DBErrorHandlingFS : public FileSystemWrapper {
 public:
  DBErrorHandlingFS()
      : FileSystemWrapper(FileSystem::Default().get()),
        trig_no_space(false),
        trig_io_error(false) {}

  void SetTrigNoSpace() { trig_no_space = true; }
  void SetTrigIoError() { trig_io_error = true; }

 private:
  bool trig_no_space;
  bool trig_io_error;
};

class ErrorHandlerFSListener : public EventListener {
 public:
  ErrorHandlerFSListener()
      : mutex_(),
        cv_(&mutex_),
        no_auto_recovery_(false),
        recovery_complete_(false),
        file_creation_started_(false),
        override_bg_error_(false),
        file_count_(0),
        fault_fs_(nullptr) {}

  void OnTableFileCreationStarted(
      const TableFileCreationBriefInfo& /*ti*/) override {
    InstrumentedMutexLock l(&mutex_);
    file_creation_started_ = true;
    if (file_count_ > 0) {
      if (--file_count_ == 0) {
        fault_fs_->SetFilesystemActive(false, file_creation_error_);
        file_creation_error_ = IOStatus::OK();
      }
    }
    cv_.SignalAll();
  }

  void OnErrorRecoveryBegin(BackgroundErrorReason /*reason*/,
                            Status /*bg_error*/, bool* auto_recovery) override {
    if (*auto_recovery && no_auto_recovery_) {
      *auto_recovery = false;
    }
  }

  void OnErrorRecoveryCompleted(Status /*old_bg_error*/) override {
    InstrumentedMutexLock l(&mutex_);
    recovery_complete_ = true;
    cv_.SignalAll();
  }

  bool WaitForRecovery(uint64_t /*abs_time_us*/) {
    InstrumentedMutexLock l(&mutex_);
    while (!recovery_complete_) {
      cv_.Wait(/*abs_time_us*/);
    }
    if (recovery_complete_) {
      recovery_complete_ = false;
      return true;
    }
    return false;
  }

  void WaitForTableFileCreationStarted(uint64_t /*abs_time_us*/) {
    InstrumentedMutexLock l(&mutex_);
    while (!file_creation_started_) {
      cv_.Wait(/*abs_time_us*/);
    }
    file_creation_started_ = false;
  }

  void OnBackgroundError(BackgroundErrorReason /*reason*/,
                         Status* bg_error) override {
    if (override_bg_error_) {
      *bg_error = bg_error_;
      override_bg_error_ = false;
    }
  }

  void EnableAutoRecovery(bool enable = true) { no_auto_recovery_ = !enable; }

  void OverrideBGError(Status bg_err) {
    bg_error_ = bg_err;
    override_bg_error_ = true;
  }

  void InjectFileCreationError(FaultInjectionTestFS* fs, int file_count,
                               IOStatus io_s) {
    fault_fs_ = fs;
    file_count_ = file_count;
    file_creation_error_ = io_s;
  }

 private:
  InstrumentedMutex mutex_;
  InstrumentedCondVar cv_;
  bool no_auto_recovery_;
  bool recovery_complete_;
  bool file_creation_started_;
  bool override_bg_error_;
  int file_count_;
  IOStatus file_creation_error_;
  Status bg_error_;
  FaultInjectionTestFS* fault_fs_;
};

TEST_F(DBErrorHandlingFSTest, FLushWriteError) {
  FaultInjectionTestFS* fault_fs =
      new FaultInjectionTestFS(FileSystem::Default().get());
  std::shared_ptr<ErrorHandlerFSListener> listener(
      new ErrorHandlerFSListener());
  Options options = GetDefaultOptions();
  options.file_system.reset(fault_fs);
  options.create_if_missing = true;
  options.listeners.emplace_back(listener);
  Status s;

  listener->EnableAutoRecovery(false);
  DestroyAndReopen(options);

  Put(Key(0), "val");
  SyncPoint::GetInstance()->SetCallBack("FlushJob::Start", [&](void*) {
    fault_fs->SetFilesystemActive(false, IOStatus::NoSpace("Out of space"));
  });
  SyncPoint::GetInstance()->EnableProcessing();
  s = Flush();
  ASSERT_EQ(s.severity(), rocksdb::Status::Severity::kHardError);
  SyncPoint::GetInstance()->DisableProcessing();
  fault_fs->SetFilesystemActive(true);
  s = dbfull()->Resume();
  ASSERT_EQ(s, Status::OK());

  Reopen(options);
  ASSERT_EQ("val", Get(Key(0)));
  Destroy(options);
}

TEST_F(DBErrorHandlingFSTest, ManifestWriteError) {
  FaultInjectionTestFS* fault_fs =
      new FaultInjectionTestFS(FileSystem::Default().get());
  std::shared_ptr<ErrorHandlerFSListener> listener(
      new ErrorHandlerFSListener());
  Options options = GetDefaultOptions();
  options.file_system.reset(fault_fs);
  options.create_if_missing = true;
  options.listeners.emplace_back(listener);
  Status s;
  std::string old_manifest;
  std::string new_manifest;

  listener->EnableAutoRecovery(false);
  DestroyAndReopen(options);
  old_manifest = GetManifestNameFromLiveFiles();

  Put(Key(0), "val");
  Flush();
  Put(Key(1), "val");
  SyncPoint::GetInstance()->SetCallBack(
      "VersionSet::LogAndApply:WriteManifest", [&](void*) {
        fault_fs->SetFilesystemActive(false, IOStatus::NoSpace("Out of space"));
      });
  SyncPoint::GetInstance()->EnableProcessing();
  s = Flush();
  ASSERT_EQ(s.severity(), rocksdb::Status::Severity::kHardError);
  SyncPoint::GetInstance()->ClearAllCallBacks();
  SyncPoint::GetInstance()->DisableProcessing();
  fault_fs->SetFilesystemActive(true);
  s = dbfull()->Resume();
  ASSERT_EQ(s, Status::OK());

  new_manifest = GetManifestNameFromLiveFiles();
  ASSERT_NE(new_manifest, old_manifest);

  Reopen(options);
  ASSERT_EQ("val", Get(Key(0)));
  ASSERT_EQ("val", Get(Key(1)));
  Close();
}

TEST_F(DBErrorHandlingFSTest, DoubleManifestWriteError) {
  FaultInjectionTestFS* fault_fs =
      new FaultInjectionTestFS(FileSystem::Default().get());
  std::shared_ptr<ErrorHandlerFSListener> listener(
      new ErrorHandlerFSListener());
  Options options = GetDefaultOptions();
  options.file_system.reset(fault_fs);
  options.create_if_missing = true;
  options.listeners.emplace_back(listener);
  Status s;
  std::string old_manifest;
  std::string new_manifest;

  listener->EnableAutoRecovery(false);
  DestroyAndReopen(options);
  old_manifest = GetManifestNameFromLiveFiles();

  Put(Key(0), "val");
  Flush();
  Put(Key(1), "val");
  SyncPoint::GetInstance()->SetCallBack(
      "VersionSet::LogAndApply:WriteManifest", [&](void*) {
        fault_fs->SetFilesystemActive(false, IOStatus::NoSpace("Out of space"));
      });
  SyncPoint::GetInstance()->EnableProcessing();
  s = Flush();
  ASSERT_EQ(s.severity(), rocksdb::Status::Severity::kHardError);
  fault_fs->SetFilesystemActive(true);

  // This Resume() will attempt to create a new manifest file and fail again
  s = dbfull()->Resume();
  ASSERT_EQ(s.severity(), rocksdb::Status::Severity::kHardError);
  fault_fs->SetFilesystemActive(true);
  SyncPoint::GetInstance()->ClearAllCallBacks();
  SyncPoint::GetInstance()->DisableProcessing();

  // A successful Resume() will create a new manifest file
  s = dbfull()->Resume();
  ASSERT_EQ(s, Status::OK());

  new_manifest = GetManifestNameFromLiveFiles();
  ASSERT_NE(new_manifest, old_manifest);

  Reopen(options);
  ASSERT_EQ("val", Get(Key(0)));
  ASSERT_EQ("val", Get(Key(1)));
  Close();
}

TEST_F(DBErrorHandlingFSTest, CompactionManifestWriteError) {
  FaultInjectionTestFS* fault_fs =
      new FaultInjectionTestFS(FileSystem::Default().get());
  std::shared_ptr<ErrorHandlerFSListener> listener(
      new ErrorHandlerFSListener());
  Options options = GetDefaultOptions();
  options.file_system.reset(fault_fs);
  options.create_if_missing = true;
  options.level0_file_num_compaction_trigger = 2;
  options.listeners.emplace_back(listener);
  Status s;
  std::string old_manifest;
  std::string new_manifest;
  std::atomic<bool> fail_manifest(false);
  DestroyAndReopen(options);
  old_manifest = GetManifestNameFromLiveFiles();

  Put(Key(0), "val");
  Put(Key(2), "val");
  s = Flush();
  ASSERT_EQ(s, Status::OK());

  rocksdb::SyncPoint::GetInstance()->LoadDependency(
      // Wait for flush of 2nd L0 file before starting compaction
      {{"DBImpl::FlushMemTable:FlushMemTableFinished",
        "BackgroundCallCompaction:0"},
       // Wait for compaction to detect manifest write error
       {"BackgroundCallCompaction:1", "CompactionManifestWriteError:0"},
       // Make compaction thread wait for error to be cleared
       {"CompactionManifestWriteError:1",
        "DBImpl::BackgroundCallCompaction:FoundObsoleteFiles"},
       // Wait for DB instance to clear bg_error before calling
       // TEST_WaitForCompact
       {"SstFileManagerImpl::ErrorCleared", "CompactionManifestWriteError:2"}});
  // trigger manifest write failure in compaction thread
  rocksdb::SyncPoint::GetInstance()->SetCallBack(
      "BackgroundCallCompaction:0", [&](void*) { fail_manifest.store(true); });
  rocksdb::SyncPoint::GetInstance()->SetCallBack(
      "VersionSet::LogAndApply:WriteManifest", [&](void*) {
        if (fail_manifest.load()) {
          fault_fs->SetFilesystemActive(false,
                                        IOStatus::NoSpace("Out of space"));
        }
      });
  rocksdb::SyncPoint::GetInstance()->EnableProcessing();

  Put(Key(1), "val");
  // This Flush will trigger a compaction, which will fail when appending to
  // the manifest
  s = Flush();
  ASSERT_EQ(s, Status::OK());

  TEST_SYNC_POINT("CompactionManifestWriteError:0");
  // Clear all errors so when the compaction is retried, it will succeed
  fault_fs->SetFilesystemActive(true);
  rocksdb::SyncPoint::GetInstance()->ClearAllCallBacks();
  TEST_SYNC_POINT("CompactionManifestWriteError:1");
  TEST_SYNC_POINT("CompactionManifestWriteError:2");

  s = dbfull()->TEST_WaitForCompact();
  rocksdb::SyncPoint::GetInstance()->DisableProcessing();
  ASSERT_EQ(s, Status::OK());

  new_manifest = GetManifestNameFromLiveFiles();
  ASSERT_NE(new_manifest, old_manifest);
  Reopen(options);
  ASSERT_EQ("val", Get(Key(0)));
  ASSERT_EQ("val", Get(Key(1)));
  ASSERT_EQ("val", Get(Key(2)));
  Close();
}

TEST_F(DBErrorHandlingFSTest, CompactionWriteError) {
  FaultInjectionTestFS* fault_fs =
      new FaultInjectionTestFS(FileSystem::Default().get());
  std::shared_ptr<ErrorHandlerFSListener> listener(
      new ErrorHandlerFSListener());
  Options options = GetDefaultOptions();
  options.file_system.reset(fault_fs);
  options.create_if_missing = true;
  options.level0_file_num_compaction_trigger = 2;
  options.listeners.emplace_back(listener);
  Status s;
  DestroyAndReopen(options);

  Put(Key(0), "va;");
  Put(Key(2), "va;");
  s = Flush();
  ASSERT_EQ(s, Status::OK());

  listener->OverrideBGError(
      Status(Status::NoSpace(), Status::Severity::kHardError));
  listener->EnableAutoRecovery(false);
  rocksdb::SyncPoint::GetInstance()->LoadDependency(
      {{"DBImpl::FlushMemTable:FlushMemTableFinished",
        "BackgroundCallCompaction:0"}});
  rocksdb::SyncPoint::GetInstance()->SetCallBack(
      "BackgroundCallCompaction:0", [&](void*) {
        fault_fs->SetFilesystemActive(false, IOStatus::NoSpace("Out of space"));
      });
  rocksdb::SyncPoint::GetInstance()->EnableProcessing();

  Put(Key(1), "val");
  s = Flush();
  ASSERT_EQ(s, Status::OK());

  s = dbfull()->TEST_WaitForCompact();
  ASSERT_EQ(s.severity(), rocksdb::Status::Severity::kHardError);

  fault_fs->SetFilesystemActive(true);
  s = dbfull()->Resume();
  ASSERT_EQ(s, Status::OK());
  Destroy(options);
}

TEST_F(DBErrorHandlingFSTest, CorruptionError) {
  FaultInjectionTestFS* fault_fs =
      new FaultInjectionTestFS(FileSystem::Default().get());
  Options options = GetDefaultOptions();
  options.file_system.reset(fault_fs);
  options.create_if_missing = true;
  options.level0_file_num_compaction_trigger = 2;
  Status s;
  DestroyAndReopen(options);

  Put(Key(0), "va;");
  Put(Key(2), "va;");
  s = Flush();
  ASSERT_EQ(s, Status::OK());

  rocksdb::SyncPoint::GetInstance()->LoadDependency(
      {{"DBImpl::FlushMemTable:FlushMemTableFinished",
        "BackgroundCallCompaction:0"}});
  rocksdb::SyncPoint::GetInstance()->SetCallBack(
      "BackgroundCallCompaction:0", [&](void*) {
        fault_fs->SetFilesystemActive(false,
                                      IOStatus::Corruption("Corruption"));
      });
  rocksdb::SyncPoint::GetInstance()->EnableProcessing();

  Put(Key(1), "val");
  s = Flush();
  ASSERT_EQ(s, Status::OK());

  s = dbfull()->TEST_WaitForCompact();
  ASSERT_EQ(s.severity(), rocksdb::Status::Severity::kUnrecoverableError);

  fault_fs->SetFilesystemActive(true);
  s = dbfull()->Resume();
  ASSERT_NE(s, Status::OK());
  Destroy(options);
}

TEST_F(DBErrorHandlingFSTest, AutoRecoverFlushError) {
  FaultInjectionTestFS* fault_fs =
      new FaultInjectionTestFS(FileSystem::Default().get());
  std::shared_ptr<ErrorHandlerFSListener> listener(
      new ErrorHandlerFSListener());
  Options options = GetDefaultOptions();
  options.file_system.reset(fault_fs);
  options.create_if_missing = true;
  options.listeners.emplace_back(listener);
  Status s;

  listener->EnableAutoRecovery();
  DestroyAndReopen(options);

  Put(Key(0), "val");
  SyncPoint::GetInstance()->SetCallBack("FlushJob::Start", [&](void*) {
    fault_fs->SetFilesystemActive(false, IOStatus::NoSpace("Out of space"));
  });
  SyncPoint::GetInstance()->EnableProcessing();
  s = Flush();
  ASSERT_EQ(s.severity(), rocksdb::Status::Severity::kHardError);
  SyncPoint::GetInstance()->DisableProcessing();
  fault_fs->SetFilesystemActive(true);
  ASSERT_EQ(listener->WaitForRecovery(5000000), true);

  s = Put(Key(1), "val");
  ASSERT_EQ(s, Status::OK());

  Reopen(options);
  ASSERT_EQ("val", Get(Key(0)));
  ASSERT_EQ("val", Get(Key(1)));
  Destroy(options);
}

TEST_F(DBErrorHandlingFSTest, FailRecoverFlushError) {
  FaultInjectionTestFS* fault_fs =
      new FaultInjectionTestFS(FileSystem::Default().get());
  std::shared_ptr<ErrorHandlerFSListener> listener(
      new ErrorHandlerFSListener());
  Options options = GetDefaultOptions();
  options.file_system.reset(fault_fs);
  options.create_if_missing = true;
  options.listeners.emplace_back(listener);
  Status s;

  listener->EnableAutoRecovery();
  DestroyAndReopen(options);

  Put(Key(0), "val");
  SyncPoint::GetInstance()->SetCallBack("FlushJob::Start", [&](void*) {
    fault_fs->SetFilesystemActive(false, IOStatus::NoSpace("Out of space"));
  });
  SyncPoint::GetInstance()->EnableProcessing();
  s = Flush();
  ASSERT_EQ(s.severity(), rocksdb::Status::Severity::kHardError);
  // We should be able to shutdown the database while auto recovery is going
  // on in the background
  Close();
  DestroyDB(dbname_, options);
}

TEST_F(DBErrorHandlingFSTest, WALWriteError) {
  FaultInjectionTestFS* fault_fs =
      new FaultInjectionTestFS(FileSystem::Default().get());
  std::shared_ptr<ErrorHandlerFSListener> listener(
      new ErrorHandlerFSListener());
  Options options = GetDefaultOptions();
  options.file_system.reset(fault_fs);
  options.create_if_missing = true;
  options.writable_file_max_buffer_size = 32768;
  options.listeners.emplace_back(listener);
  Status s;
  Random rnd(301);

  listener->EnableAutoRecovery();
  DestroyAndReopen(options);

  {
    WriteBatch batch;

    for (auto i = 0; i < 100; ++i) {
      batch.Put(Key(i), RandomString(&rnd, 1024));
    }

    WriteOptions wopts;
    wopts.sync = true;
    ASSERT_EQ(dbfull()->Write(wopts, &batch), Status::OK());
  };

  {
    WriteBatch batch;
    int write_error = 0;

    for (auto i = 100; i < 199; ++i) {
      batch.Put(Key(i), RandomString(&rnd, 1024));
    }

    SyncPoint::GetInstance()->SetCallBack(
        "WritableFileWriter::Append:BeforePrepareWrite", [&](void*) {
          write_error++;
          if (write_error > 2) {
            fault_fs->SetFilesystemActive(false,
                                          IOStatus::NoSpace("Out of space"));
          }
        });
    SyncPoint::GetInstance()->EnableProcessing();
    WriteOptions wopts;
    wopts.sync = true;
    s = dbfull()->Write(wopts, &batch);
    ASSERT_EQ(s, s.NoSpace());
  }
  SyncPoint::GetInstance()->DisableProcessing();
  fault_fs->SetFilesystemActive(true);
  ASSERT_EQ(listener->WaitForRecovery(5000000), true);
  for (auto i = 0; i < 199; ++i) {
    if (i < 100) {
      ASSERT_NE(Get(Key(i)), "NOT_FOUND");
    } else {
      ASSERT_EQ(Get(Key(i)), "NOT_FOUND");
    }
  }
  Reopen(options);
  for (auto i = 0; i < 199; ++i) {
    if (i < 100) {
      ASSERT_NE(Get(Key(i)), "NOT_FOUND");
    } else {
      ASSERT_EQ(Get(Key(i)), "NOT_FOUND");
    }
  }
  Close();
}

TEST_F(DBErrorHandlingFSTest, MultiCFWALWriteError) {
  FaultInjectionTestFS* fault_fs =
      new FaultInjectionTestFS(FileSystem::Default().get());
  std::shared_ptr<ErrorHandlerFSListener> listener(
      new ErrorHandlerFSListener());
  Options options = GetDefaultOptions();
  options.file_system.reset(fault_fs);
  options.create_if_missing = true;
  options.writable_file_max_buffer_size = 32768;
  options.listeners.emplace_back(listener);
  Status s;
  Random rnd(301);

  listener->EnableAutoRecovery();
  CreateAndReopenWithCF({"one", "two", "three"}, options);

  {
    WriteBatch batch;

    for (auto i = 1; i < 4; ++i) {
      for (auto j = 0; j < 100; ++j) {
        batch.Put(handles_[i], Key(j), RandomString(&rnd, 1024));
      }
    }

    WriteOptions wopts;
    wopts.sync = true;
    ASSERT_EQ(dbfull()->Write(wopts, &batch), Status::OK());
  };

  {
    WriteBatch batch;
    int write_error = 0;

    // Write to one CF
    for (auto i = 100; i < 199; ++i) {
      batch.Put(handles_[2], Key(i), RandomString(&rnd, 1024));
    }

    SyncPoint::GetInstance()->SetCallBack(
        "WritableFileWriter::Append:BeforePrepareWrite", [&](void*) {
          write_error++;
          if (write_error > 2) {
            fault_fs->SetFilesystemActive(false,
                                          IOStatus::NoSpace("Out of space"));
          }
        });
    SyncPoint::GetInstance()->EnableProcessing();
    WriteOptions wopts;
    wopts.sync = true;
    s = dbfull()->Write(wopts, &batch);
    ASSERT_EQ(s, s.NoSpace());
  }
  SyncPoint::GetInstance()->DisableProcessing();
  fault_fs->SetFilesystemActive(true);
  ASSERT_EQ(listener->WaitForRecovery(5000000), true);

  for (auto i = 1; i < 4; ++i) {
    // Every CF should have been flushed
    ASSERT_EQ(NumTableFilesAtLevel(0, i), 1);
  }

  for (auto i = 1; i < 4; ++i) {
    for (auto j = 0; j < 199; ++j) {
      if (j < 100) {
        ASSERT_NE(Get(i, Key(j)), "NOT_FOUND");
      } else {
        ASSERT_EQ(Get(i, Key(j)), "NOT_FOUND");
      }
    }
  }
  ReopenWithColumnFamilies({"default", "one", "two", "three"}, options);
  for (auto i = 1; i < 4; ++i) {
    for (auto j = 0; j < 199; ++j) {
      if (j < 100) {
        ASSERT_NE(Get(i, Key(j)), "NOT_FOUND");
      } else {
        ASSERT_EQ(Get(i, Key(j)), "NOT_FOUND");
      }
    }
  }
  Close();
}

}  // namespace ROCKSDB_NAMESPACE

int main(int argc, char** argv) {
  rocksdb::port::InstallStackTraceHandler();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

#else
#include <stdio.h>

int main(int /*argc*/, char** /*argv*/) {
  fprintf(stderr, "SKIPPED as Cuckoo table is not supported in ROCKSDB_LITE\n");
  return 0;
}

#endif  // ROCKSDB_LITE
