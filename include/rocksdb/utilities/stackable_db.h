// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#pragma once
#include <map>
#include <memory>
#include <string>

#include "rocksdb/db.h"

#ifdef _WIN32
// Windows API macro interference
#undef DeleteFile
#endif

namespace ROCKSDB_NAMESPACE {

// This class contains APIs to stack rocksdb wrappers.Eg. Stack TTL over base d
class StackableDB : public DB {
 public:
  // StackableDB take sole ownership of the underlying db.
  explicit StackableDB(DB* db) : db_(db) {}

  // StackableDB take shared ownership of the underlying db.
  explicit StackableDB(std::shared_ptr<DB> db)
      : db_(db.get()), shared_db_ptr_(db) {}

  ~StackableDB() {
    if (shared_db_ptr_ == nullptr) {
      delete db_;
    } else {
      assert(shared_db_ptr_.get() == db_);
    }
    db_ = nullptr;
  }

  virtual Status Close() override { return db_->Close(); }

  virtual DB* GetBaseDB() { return db_; }

  virtual DB* GetRootDB() override { return db_->GetRootDB(); }

  virtual Status CreateColumnFamily(const ColumnFamilyOptions& options,
                                    const std::string& column_family_name,
                                    ColumnFamilyHandle** handle) override {
    return db_->CreateColumnFamily(options, column_family_name, handle);
  }

  virtual Status CreateColumnFamilies(
      const ColumnFamilyOptions& options,
      const std::vector<std::string>& column_family_names,
      std::vector<ColumnFamilyHandle*>* handles) override {
    return db_->CreateColumnFamilies(options, column_family_names, handles);
  }

  virtual Status CreateColumnFamilies(
      const std::vector<ColumnFamilyDescriptor>& column_families,
      std::vector<ColumnFamilyHandle*>* handles) override {
    return db_->CreateColumnFamilies(column_families, handles);
  }

  virtual Status DropColumnFamily(ColumnFamilyHandle* column_family) override {
    return db_->DropColumnFamily(column_family);
  }

  virtual Status DropColumnFamilies(
      const std::vector<ColumnFamilyHandle*>& column_families) override {
    return db_->DropColumnFamilies(column_families);
  }

  virtual Status DestroyColumnFamilyHandle(
      ColumnFamilyHandle* column_family) override {
    return db_->DestroyColumnFamilyHandle(column_family);
  }

  using DB::Put;
  virtual Status Put(const WriteOptions& options,
                     ColumnFamilyHandle* column_family, const Slice& key,
                     const Slice& val) override {
    return db_->Put(options, column_family, key, val);
  }
  Status Put(const WriteOptions& options, ColumnFamilyHandle* column_family,
             const Slice& key, const Slice& ts, const Slice& val) override {
    return db_->Put(options, column_family, key, ts, val);
  }

  using DB::PutEntity;
  Status PutEntity(const WriteOptions& options,
                   ColumnFamilyHandle* column_family, const Slice& key,
                   const WideColumns& columns) override {
    return db_->PutEntity(options, column_family, key, columns);
  }

  using DB::Get;
  virtual Status Get(const ReadOptions& options,
                     ColumnFamilyHandle* column_family, const Slice& key,
                     PinnableSlice* value) override {
    return db_->Get(options, column_family, key, value);
  }

  using DB::GetEntity;
  Status GetEntity(const ReadOptions& options,
                   ColumnFamilyHandle* column_family, const Slice& key,
                   PinnableWideColumns* columns) override {
    return db_->GetEntity(options, column_family, key, columns);
  }

  using DB::GetMergeOperands;
  virtual Status GetMergeOperands(
      const ReadOptions& options, ColumnFamilyHandle* column_family,
      const Slice& key, PinnableSlice* slice,
      GetMergeOperandsOptions* get_merge_operands_options,
      int* number_of_operands) override {
    return db_->GetMergeOperands(options, column_family, key, slice,
                                 get_merge_operands_options,
                                 number_of_operands);
  }

  using DB::MultiGet;
  virtual std::vector<Status> MultiGet(
      const ReadOptions& options,
      const std::vector<ColumnFamilyHandle*>& column_family,
      const std::vector<Slice>& keys,
      std::vector<std::string>* values) override {
    return db_->MultiGet(options, column_family, keys, values);
  }

  virtual void MultiGet(const ReadOptions& options,
                        ColumnFamilyHandle* column_family,
                        const size_t num_keys, const Slice* keys,
                        PinnableSlice* values, Status* statuses,
                        const bool sorted_input = false) override {
    return db_->MultiGet(options, column_family, num_keys, keys, values,
                         statuses, sorted_input);
  }

  using DB::MultiGetEntity;
  void MultiGetEntity(const ReadOptions& options, size_t num_keys,
                      ColumnFamilyHandle** column_families, const Slice* keys,
                      PinnableWideColumns* results, Status* statuses) override {
    db_->MultiGetEntity(options, num_keys, column_families, keys, results,
                        statuses);
  }

  using DB::IngestExternalFile;
  virtual Status IngestExternalFile(
      ColumnFamilyHandle* column_family,
      const std::vector<std::string>& external_files,
      const IngestExternalFileOptions& options) override {
    return db_->IngestExternalFile(column_family, external_files, options);
  }

  using DB::IngestExternalFiles;
  virtual Status IngestExternalFiles(
      const std::vector<IngestExternalFileArg>& args) override {
    return db_->IngestExternalFiles(args);
  }

  using DB::CreateColumnFamilyWithImport;
  virtual Status CreateColumnFamilyWithImport(
      const ColumnFamilyOptions& options, const std::string& column_family_name,
      const ImportColumnFamilyOptions& import_options,
      const ExportImportFilesMetaData& metadata,
      ColumnFamilyHandle** handle) override {
    return db_->CreateColumnFamilyWithImport(options, column_family_name,
                                             import_options, metadata, handle);
  }

  using DB::VerifyFileChecksums;
  Status VerifyFileChecksums(const ReadOptions& read_opts) override {
    return db_->VerifyFileChecksums(read_opts);
  }

  virtual Status VerifyChecksum() override { return db_->VerifyChecksum(); }

  virtual Status VerifyChecksum(const ReadOptions& options) override {
    return db_->VerifyChecksum(options);
  }

  using DB::KeyMayExist;
  virtual bool KeyMayExist(const ReadOptions& options,
                           ColumnFamilyHandle* column_family, const Slice& key,
                           std::string* value,
                           bool* value_found = nullptr) override {
    return db_->KeyMayExist(options, column_family, key, value, value_found);
  }

  using DB::Delete;
  virtual Status Delete(const WriteOptions& wopts,
                        ColumnFamilyHandle* column_family,
                        const Slice& key) override {
    return db_->Delete(wopts, column_family, key);
  }
  Status Delete(const WriteOptions& wopts, ColumnFamilyHandle* column_family,
                const Slice& key, const Slice& ts) override {
    return db_->Delete(wopts, column_family, key, ts);
  }

  using DB::SingleDelete;
  virtual Status SingleDelete(const WriteOptions& wopts,
                              ColumnFamilyHandle* column_family,
                              const Slice& key) override {
    return db_->SingleDelete(wopts, column_family, key);
  }
  Status SingleDelete(const WriteOptions& wopts,
                      ColumnFamilyHandle* column_family, const Slice& key,
                      const Slice& ts) override {
    return db_->SingleDelete(wopts, column_family, key, ts);
  }

  using DB::DeleteRange;
  Status DeleteRange(const WriteOptions& wopts,
                     ColumnFamilyHandle* column_family, const Slice& start_key,
                     const Slice& end_key) override {
    return db_->DeleteRange(wopts, column_family, start_key, end_key);
  }

  using DB::Merge;
  virtual Status Merge(const WriteOptions& options,
                       ColumnFamilyHandle* column_family, const Slice& key,
                       const Slice& value) override {
    return db_->Merge(options, column_family, key, value);
  }
  Status Merge(const WriteOptions& options, ColumnFamilyHandle* column_family,
               const Slice& key, const Slice& ts, const Slice& value) override {
    return db_->Merge(options, column_family, key, ts, value);
  }

  virtual Status Write(const WriteOptions& opts, WriteBatch* updates) override {
    return db_->Write(opts, updates);
  }

  using DB::NewIterator;
  virtual Iterator* NewIterator(const ReadOptions& opts,
                                ColumnFamilyHandle* column_family) override {
    return db_->NewIterator(opts, column_family);
  }

  virtual Status NewIterators(
      const ReadOptions& options,
      const std::vector<ColumnFamilyHandle*>& column_families,
      std::vector<Iterator*>* iterators) override {
    return db_->NewIterators(options, column_families, iterators);
  }

  virtual const Snapshot* GetSnapshot() override { return db_->GetSnapshot(); }

  virtual void ReleaseSnapshot(const Snapshot* snapshot) override {
    return db_->ReleaseSnapshot(snapshot);
  }

  using DB::GetMapProperty;
  using DB::GetProperty;
  virtual bool GetProperty(ColumnFamilyHandle* column_family,
                           const Slice& property, std::string* value) override {
    return db_->GetProperty(column_family, property, value);
  }
  virtual bool GetMapProperty(
      ColumnFamilyHandle* column_family, const Slice& property,
      std::map<std::string, std::string>* value) override {
    return db_->GetMapProperty(column_family, property, value);
  }

  using DB::GetIntProperty;
  virtual bool GetIntProperty(ColumnFamilyHandle* column_family,
                              const Slice& property, uint64_t* value) override {
    return db_->GetIntProperty(column_family, property, value);
  }

  using DB::GetAggregatedIntProperty;
  virtual bool GetAggregatedIntProperty(const Slice& property,
                                        uint64_t* value) override {
    return db_->GetAggregatedIntProperty(property, value);
  }

  using DB::GetApproximateSizes;
  virtual Status GetApproximateSizes(const SizeApproximationOptions& options,
                                     ColumnFamilyHandle* column_family,
                                     const Range* r, int n,
                                     uint64_t* sizes) override {
    return db_->GetApproximateSizes(options, column_family, r, n, sizes);
  }

  using DB::GetApproximateMemTableStats;
  virtual void GetApproximateMemTableStats(ColumnFamilyHandle* column_family,
                                           const Range& range,
                                           uint64_t* const count,
                                           uint64_t* const size) override {
    return db_->GetApproximateMemTableStats(column_family, range, count, size);
  }

  using DB::CompactRange;
  virtual Status CompactRange(const CompactRangeOptions& options,
                              ColumnFamilyHandle* column_family,
                              const Slice* begin, const Slice* end) override {
    return db_->CompactRange(options, column_family, begin, end);
  }

  using DB::CompactFiles;
  virtual Status CompactFiles(
      const CompactionOptions& compact_options,
      ColumnFamilyHandle* column_family,
      const std::vector<std::string>& input_file_names, const int output_level,
      const int output_path_id = -1,
      std::vector<std::string>* const output_file_names = nullptr,
      CompactionJobInfo* compaction_job_info = nullptr) override {
    return db_->CompactFiles(compact_options, column_family, input_file_names,
                             output_level, output_path_id, output_file_names,
                             compaction_job_info);
  }

  virtual Status PauseBackgroundWork() override {
    return db_->PauseBackgroundWork();
  }
  virtual Status ContinueBackgroundWork() override {
    return db_->ContinueBackgroundWork();
  }

  virtual Status EnableAutoCompaction(
      const std::vector<ColumnFamilyHandle*>& column_family_handles) override {
    return db_->EnableAutoCompaction(column_family_handles);
  }

  virtual void EnableManualCompaction() override {
    return db_->EnableManualCompaction();
  }
  virtual void DisableManualCompaction() override {
    return db_->DisableManualCompaction();
  }

  using DB::NumberLevels;
  virtual int NumberLevels(ColumnFamilyHandle* column_family) override {
    return db_->NumberLevels(column_family);
  }

  using DB::MaxMemCompactionLevel;
  virtual int MaxMemCompactionLevel(
      ColumnFamilyHandle* column_family) override {
    return db_->MaxMemCompactionLevel(column_family);
  }

  using DB::Level0StopWriteTrigger;
  virtual int Level0StopWriteTrigger(
      ColumnFamilyHandle* column_family) override {
    return db_->Level0StopWriteTrigger(column_family);
  }

  virtual const std::string& GetName() const override { return db_->GetName(); }

  virtual Env* GetEnv() const override { return db_->GetEnv(); }

  virtual FileSystem* GetFileSystem() const override {
    return db_->GetFileSystem();
  }

  using DB::GetOptions;
  virtual Options GetOptions(ColumnFamilyHandle* column_family) const override {
    return db_->GetOptions(column_family);
  }

  using DB::GetDBOptions;
  virtual DBOptions GetDBOptions() const override {
    return db_->GetDBOptions();
  }

  using DB::Flush;
  virtual Status Flush(const FlushOptions& fopts,
                       ColumnFamilyHandle* column_family) override {
    return db_->Flush(fopts, column_family);
  }
  virtual Status Flush(
      const FlushOptions& fopts,
      const std::vector<ColumnFamilyHandle*>& column_families) override {
    return db_->Flush(fopts, column_families);
  }

  virtual Status SyncWAL() override { return db_->SyncWAL(); }

  virtual Status FlushWAL(bool sync) override { return db_->FlushWAL(sync); }

  virtual Status LockWAL() override { return db_->LockWAL(); }

  virtual Status UnlockWAL() override { return db_->UnlockWAL(); }


  virtual Status DisableFileDeletions() override {
    return db_->DisableFileDeletions();
  }

  virtual Status EnableFileDeletions(bool force) override {
    return db_->EnableFileDeletions(force);
  }

  virtual void GetLiveFilesMetaData(
      std::vector<LiveFileMetaData>* metadata) override {
    db_->GetLiveFilesMetaData(metadata);
  }

  virtual Status GetLiveFilesChecksumInfo(
      FileChecksumList* checksum_list) override {
    return db_->GetLiveFilesChecksumInfo(checksum_list);
  }

  virtual Status GetLiveFilesStorageInfo(
      const LiveFilesStorageInfoOptions& opts,
      std::vector<LiveFileStorageInfo>* files) override {
    return db_->GetLiveFilesStorageInfo(opts, files);
  }

  virtual void GetColumnFamilyMetaData(ColumnFamilyHandle* column_family,
                                       ColumnFamilyMetaData* cf_meta) override {
    db_->GetColumnFamilyMetaData(column_family, cf_meta);
  }

  using DB::StartBlockCacheTrace;
  Status StartBlockCacheTrace(
      const TraceOptions& trace_options,
      std::unique_ptr<TraceWriter>&& trace_writer) override {
    return db_->StartBlockCacheTrace(trace_options, std::move(trace_writer));
  }

  Status StartBlockCacheTrace(
      const BlockCacheTraceOptions& options,
      std::unique_ptr<BlockCacheTraceWriter>&& trace_writer) override {
    return db_->StartBlockCacheTrace(options, std::move(trace_writer));
  }

  using DB::EndBlockCacheTrace;
  Status EndBlockCacheTrace() override { return db_->EndBlockCacheTrace(); }

  using DB::StartIOTrace;
  Status StartIOTrace(const TraceOptions& options,
                      std::unique_ptr<TraceWriter>&& trace_writer) override {
    return db_->StartIOTrace(options, std::move(trace_writer));
  }

  using DB::EndIOTrace;
  Status EndIOTrace() override { return db_->EndIOTrace(); }

  using DB::StartTrace;
  Status StartTrace(const TraceOptions& options,
                    std::unique_ptr<TraceWriter>&& trace_writer) override {
    return db_->StartTrace(options, std::move(trace_writer));
  }

  using DB::EndTrace;
  Status EndTrace() override { return db_->EndTrace(); }

  using DB::NewDefaultReplayer;
  Status NewDefaultReplayer(const std::vector<ColumnFamilyHandle*>& handles,
                            std::unique_ptr<TraceReader>&& reader,
                            std::unique_ptr<Replayer>* replayer) override {
    return db_->NewDefaultReplayer(handles, std::move(reader), replayer);
  }


  virtual Status GetLiveFiles(std::vector<std::string>& vec, uint64_t* mfs,
                              bool flush_memtable = true) override {
    return db_->GetLiveFiles(vec, mfs, flush_memtable);
  }

  virtual SequenceNumber GetLatestSequenceNumber() const override {
    return db_->GetLatestSequenceNumber();
  }

  Status IncreaseFullHistoryTsLow(ColumnFamilyHandle* column_family,
                                  std::string ts_low) override {
    return db_->IncreaseFullHistoryTsLow(column_family, ts_low);
  }

  Status GetFullHistoryTsLow(ColumnFamilyHandle* column_family,
                             std::string* ts_low) override {
    return db_->GetFullHistoryTsLow(column_family, ts_low);
  }

  virtual Status GetSortedWalFiles(VectorLogPtr& files) override {
    return db_->GetSortedWalFiles(files);
  }

  virtual Status GetCurrentWalFile(
      std::unique_ptr<LogFile>* current_log_file) override {
    return db_->GetCurrentWalFile(current_log_file);
  }

  virtual Status GetCreationTimeOfOldestFile(uint64_t* creation_time) override {
    return db_->GetCreationTimeOfOldestFile(creation_time);
  }

  // WARNING: This API is planned for removal in RocksDB 7.0 since it does not
  // operate at the proper level of abstraction for a key-value store, and its
  // contract/restrictions are poorly documented. For example, it returns non-OK
  // `Status` for non-bottommost files and files undergoing compaction. Since we
  // do not plan to maintain it, the contract will likely remain underspecified
  // until its removal. Any user is encouraged to read the implementation
  // carefully and migrate away from it when possible.
  virtual Status DeleteFile(std::string name) override {
    return db_->DeleteFile(name);
  }

  virtual Status GetDbIdentity(std::string& identity) const override {
    return db_->GetDbIdentity(identity);
  }

  virtual Status GetDbSessionId(std::string& session_id) const override {
    return db_->GetDbSessionId(session_id);
  }

  using DB::SetOptions;
  virtual Status SetOptions(ColumnFamilyHandle* column_family_handle,
                            const std::unordered_map<std::string, std::string>&
                                new_options) override {
    return db_->SetOptions(column_family_handle, new_options);
  }

  virtual Status SetDBOptions(
      const std::unordered_map<std::string, std::string>& new_options)
      override {
    return db_->SetDBOptions(new_options);
  }

  using DB::ResetStats;
  virtual Status ResetStats() override { return db_->ResetStats(); }

  using DB::GetPropertiesOfAllTables;
  virtual Status GetPropertiesOfAllTables(
      ColumnFamilyHandle* column_family,
      TablePropertiesCollection* props) override {
    return db_->GetPropertiesOfAllTables(column_family, props);
  }

  using DB::GetPropertiesOfTablesInRange;
  virtual Status GetPropertiesOfTablesInRange(
      ColumnFamilyHandle* column_family, const Range* range, std::size_t n,
      TablePropertiesCollection* props) override {
    return db_->GetPropertiesOfTablesInRange(column_family, range, n, props);
  }

  virtual Status GetUpdatesSince(
      SequenceNumber seq_number, std::unique_ptr<TransactionLogIterator>* iter,
      const TransactionLogIterator::ReadOptions& read_options) override {
    return db_->GetUpdatesSince(seq_number, iter, read_options);
  }

  virtual Status SuggestCompactRange(ColumnFamilyHandle* column_family,
                                     const Slice* begin,
                                     const Slice* end) override {
    return db_->SuggestCompactRange(column_family, begin, end);
  }

  virtual Status PromoteL0(ColumnFamilyHandle* column_family,
                           int target_level) override {
    return db_->PromoteL0(column_family, target_level);
  }

  virtual ColumnFamilyHandle* DefaultColumnFamily() const override {
    return db_->DefaultColumnFamily();
  }

  Status TryCatchUpWithPrimary() override {
    return db_->TryCatchUpWithPrimary();
  }

 protected:
  DB* db_;
  std::shared_ptr<DB> shared_db_ptr_;
};

}  // namespace ROCKSDB_NAMESPACE
