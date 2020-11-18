//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
#include "db/memtable_list.h"

#include <cinttypes>
#include <limits>
#include <queue>
#include <string>
#include "db/db_impl/db_impl.h"
#include "db/memtable.h"
#include "db/range_tombstone_fragmenter.h"
#include "db/version_set.h"
#include "logging/log_buffer.h"
#include "monitoring/thread_status_util.h"
#include "rocksdb/db.h"
#include "rocksdb/env.h"
#include "rocksdb/iterator.h"
#include "table/merging_iterator.h"
#include "test_util/sync_point.h"
#include "util/coding.h"

namespace ROCKSDB_NAMESPACE {

class InternalKeyComparator;
class Mutex;
class VersionSet;

void MemTableListVersion::AddMemTable(MemTable* m) {
  memlist_.push_front(m);
  *parent_memtable_list_memory_usage_ += m->ApproximateMemoryUsage();
}

void MemTableListVersion::UnrefMemTable(autovector<MemTable*>* to_delete,
                                        MemTable* m) {
  if (m->Unref()) {
    to_delete->push_back(m);
    assert(*parent_memtable_list_memory_usage_ >= m->ApproximateMemoryUsage());
    *parent_memtable_list_memory_usage_ -= m->ApproximateMemoryUsage();
  }
}

MemTableListVersion::MemTableListVersion(
    size_t* parent_memtable_list_memory_usage, const MemTableListVersion& old)
    : max_write_buffer_number_to_maintain_(
          old.max_write_buffer_number_to_maintain_),
      max_write_buffer_size_to_maintain_(
          old.max_write_buffer_size_to_maintain_),
      parent_memtable_list_memory_usage_(parent_memtable_list_memory_usage) {
  memlist_ = old.memlist_;
  for (auto& m : memlist_) {
    m->Ref();
  }

  memlist_history_ = old.memlist_history_;
  for (auto& m : memlist_history_) {
    m->Ref();
  }
}

MemTableListVersion::MemTableListVersion(
    size_t* parent_memtable_list_memory_usage,
    int max_write_buffer_number_to_maintain,
    int64_t max_write_buffer_size_to_maintain)
    : max_write_buffer_number_to_maintain_(max_write_buffer_number_to_maintain),
      max_write_buffer_size_to_maintain_(max_write_buffer_size_to_maintain),
      parent_memtable_list_memory_usage_(parent_memtable_list_memory_usage) {}

void MemTableListVersion::Ref() { ++refs_; }

// called by superversion::clean()
void MemTableListVersion::Unref(autovector<MemTable*>* to_delete) {
  assert(refs_ >= 1);
  --refs_;
  if (refs_ == 0) {
    // if to_delete is equal to nullptr it means we're confident
    // that refs_ will not be zero
    assert(to_delete != nullptr);
    for (const auto& m : memlist_) {
      UnrefMemTable(to_delete, m);
    }
    for (const auto& m : memlist_history_) {
      UnrefMemTable(to_delete, m);
    }
    delete this;
  }
}

int MemTableList::NumNotFlushed() const {
  int size = static_cast<int>(current_->memlist_.size());
  assert(num_flush_not_started_ <= size);
  return size;
}

int MemTableList::NumFlushed() const {
  return static_cast<int>(current_->memlist_history_.size());
}

// Search all the memtables starting from the most recent one.
// Return the most recent value found, if any.
// Operands stores the list of merge operations to apply, so far.
bool MemTableListVersion::Get(const LookupKey& key, std::string* value,
                              std::string* timestamp, Status* s,
                              MergeContext* merge_context,
                              SequenceNumber* max_covering_tombstone_seq,
                              SequenceNumber* seq, const ReadOptions& read_opts,
                              ReadCallback* callback, bool* is_blob_index) {
  return GetFromList(&memlist_, key, value, timestamp, s, merge_context,
                     max_covering_tombstone_seq, seq, read_opts, callback,
                     is_blob_index);
}

void MemTableListVersion::MultiGet(const ReadOptions& read_options,
                                   MultiGetRange* range, ReadCallback* callback,
                                   bool* is_blob) {
  for (auto memtable : memlist_) {
    memtable->MultiGet(read_options, range, callback, is_blob);
    if (range->empty()) {
      return;
    }
  }
}

bool MemTableListVersion::GetMergeOperands(
    const LookupKey& key, Status* s, MergeContext* merge_context,
    SequenceNumber* max_covering_tombstone_seq, const ReadOptions& read_opts) {
  for (MemTable* memtable : memlist_) {
    bool done = memtable->Get(key, /*value*/ nullptr, /*timestamp*/ nullptr, s,
                              merge_context, max_covering_tombstone_seq,
                              read_opts, nullptr, nullptr, false);
    if (done) {
      return true;
    }
  }
  return false;
}

bool MemTableListVersion::GetFromHistory(
    const LookupKey& key, std::string* value, std::string* timestamp, Status* s,
    MergeContext* merge_context, SequenceNumber* max_covering_tombstone_seq,
    SequenceNumber* seq, const ReadOptions& read_opts, bool* is_blob_index) {
  return GetFromList(&memlist_history_, key, value, timestamp, s, merge_context,
                     max_covering_tombstone_seq, seq, read_opts,
                     nullptr /*read_callback*/, is_blob_index);
}

bool MemTableListVersion::GetFromList(
    std::list<MemTable*>* list, const LookupKey& key, std::string* value,
    std::string* timestamp, Status* s, MergeContext* merge_context,
    SequenceNumber* max_covering_tombstone_seq, SequenceNumber* seq,
    const ReadOptions& read_opts, ReadCallback* callback, bool* is_blob_index) {
  *seq = kMaxSequenceNumber;

  for (auto& memtable : *list) {
    SequenceNumber current_seq = kMaxSequenceNumber;

    bool done = memtable->Get(key, value, timestamp, s, merge_context,
                              max_covering_tombstone_seq, &current_seq,
                              read_opts, callback, is_blob_index);
    if (*seq == kMaxSequenceNumber) {
      // Store the most recent sequence number of any operation on this key.
      // Since we only care about the most recent change, we only need to
      // return the first operation found when searching memtables in
      // reverse-chronological order.
      // current_seq would be equal to kMaxSequenceNumber if the value was to be
      // skipped. This allows seq to be assigned again when the next value is
      // read.
      *seq = current_seq;
    }

    if (done) {
      assert(*seq != kMaxSequenceNumber || s->IsNotFound());
      return true;
    }
    if (!done && !s->ok() && !s->IsMergeInProgress() && !s->IsNotFound()) {
      return false;
    }
  }
  return false;
}

Status MemTableListVersion::AddRangeTombstoneIterators(
    const ReadOptions& read_opts, Arena* /*arena*/,
    RangeDelAggregator* range_del_agg) {
  assert(range_del_agg != nullptr);
  // Except for snapshot read, using kMaxSequenceNumber is OK because these
  // are immutable memtables.
  SequenceNumber read_seq = read_opts.snapshot != nullptr
                                ? read_opts.snapshot->GetSequenceNumber()
                                : kMaxSequenceNumber;
  for (auto& m : memlist_) {
    std::unique_ptr<FragmentedRangeTombstoneIterator> range_del_iter(
        m->NewRangeTombstoneIterator(read_opts, read_seq));
    range_del_agg->AddTombstones(std::move(range_del_iter));
  }
  return Status::OK();
}

void MemTableListVersion::AddIterators(
    const ReadOptions& options, std::vector<InternalIterator*>* iterator_list,
    Arena* arena) {
  for (auto& m : memlist_) {
    iterator_list->push_back(m->NewIterator(options, arena));
  }
}

void MemTableListVersion::AddIterators(
    const ReadOptions& options, MergeIteratorBuilder* merge_iter_builder) {
  for (auto& m : memlist_) {
    merge_iter_builder->AddIterator(
        m->NewIterator(options, merge_iter_builder->GetArena()));
  }
}

uint64_t MemTableListVersion::GetTotalNumEntries() const {
  uint64_t total_num = 0;
  for (auto& m : memlist_) {
    total_num += m->num_entries();
  }
  return total_num;
}

MemTable::MemTableStats MemTableListVersion::ApproximateStats(
    const Slice& start_ikey, const Slice& end_ikey) {
  MemTable::MemTableStats total_stats = {0, 0};
  for (auto& m : memlist_) {
    auto mStats = m->ApproximateStats(start_ikey, end_ikey);
    total_stats.size += mStats.size;
    total_stats.count += mStats.count;
  }
  return total_stats;
}

uint64_t MemTableListVersion::GetTotalNumDeletes() const {
  uint64_t total_num = 0;
  for (auto& m : memlist_) {
    total_num += m->num_deletes();
  }
  return total_num;
}

SequenceNumber MemTableListVersion::GetEarliestSequenceNumber(
    bool include_history) const {
  if (include_history && !memlist_history_.empty()) {
    return memlist_history_.back()->GetEarliestSequenceNumber();
  } else if (!memlist_.empty()) {
    return memlist_.back()->GetEarliestSequenceNumber();
  } else {
    return kMaxSequenceNumber;
  }
}

// caller is responsible for referencing m
void MemTableListVersion::Add(MemTable* m, autovector<MemTable*>* to_delete) {
  assert(refs_ == 1);  // only when refs_ == 1 is MemTableListVersion mutable
  AddMemTable(m);

  TrimHistory(to_delete, m->ApproximateMemoryUsage());
}

// Removes m from list of memtables not flushed.  Caller should NOT Unref m.
void MemTableListVersion::Remove(MemTable* m,
                                 autovector<MemTable*>* to_delete) {
  assert(refs_ == 1);  // only when refs_ == 1 is MemTableListVersion mutable
  memlist_.remove(m);

  m->MarkFlushed();
  if (max_write_buffer_size_to_maintain_ > 0 ||
      max_write_buffer_number_to_maintain_ > 0) {
    memlist_history_.push_front(m);
    // Unable to get size of mutable memtable at this point, pass 0 to
    // TrimHistory as a best effort.
    TrimHistory(to_delete, 0);
  } else {
    UnrefMemTable(to_delete, m);
  }
}

// return the total memory usage assuming the oldest flushed memtable is dropped
size_t MemTableListVersion::ApproximateMemoryUsageExcludingLast() const {
  size_t total_memtable_size = 0;
  for (auto& memtable : memlist_) {
    total_memtable_size += memtable->ApproximateMemoryUsage();
  }
  for (auto& memtable : memlist_history_) {
    total_memtable_size += memtable->ApproximateMemoryUsage();
  }
  if (!memlist_history_.empty()) {
    total_memtable_size -= memlist_history_.back()->ApproximateMemoryUsage();
  }
  return total_memtable_size;
}

bool MemTableListVersion::MemtableLimitExceeded(size_t usage) {
  if (max_write_buffer_size_to_maintain_ > 0) {
    // calculate the total memory usage after dropping the oldest flushed
    // memtable, compare with max_write_buffer_size_to_maintain_ to decide
    // whether to trim history
    return ApproximateMemoryUsageExcludingLast() + usage >=
           static_cast<size_t>(max_write_buffer_size_to_maintain_);
  } else if (max_write_buffer_number_to_maintain_ > 0) {
    return memlist_.size() + memlist_history_.size() >
           static_cast<size_t>(max_write_buffer_number_to_maintain_);
  } else {
    return false;
  }
}

// Make sure we don't use up too much space in history
bool MemTableListVersion::TrimHistory(autovector<MemTable*>* to_delete,
                                      size_t usage) {
  bool ret = false;
  while (MemtableLimitExceeded(usage) && !memlist_history_.empty()) {
    MemTable* x = memlist_history_.back();
    memlist_history_.pop_back();

    UnrefMemTable(to_delete, x);
    ret = true;
  }
  return ret;
}

// Returns true if there is at least one memtable on which flush has
// not yet started.
bool MemTableList::IsFlushPending() const {
  if ((flush_requested_ && num_flush_not_started_ > 0) ||
      (num_flush_not_started_ >= min_write_buffer_number_to_merge_)) {
    assert(imm_flush_needed.load(std::memory_order_relaxed));
    return true;
  }
  return false;
}

// Returns the memtables that need to be flushed.
void MemTableList::PickMemtablesToFlush(const uint64_t* max_memtable_id,
                                        autovector<MemTable*>* ret) {
  AutoThreadOperationStageUpdater stage_updater(
      ThreadStatus::STAGE_PICK_MEMTABLES_TO_FLUSH);
  const auto& memlist = current_->memlist_;
  bool atomic_flush = false;
  for (auto it = memlist.rbegin(); it != memlist.rend(); ++it) {
    MemTable* m = *it;
    if (!atomic_flush && m->atomic_flush_seqno_ != kMaxSequenceNumber) {
      atomic_flush = true;
    }
    if (max_memtable_id != nullptr && m->GetID() > *max_memtable_id) {
      break;
    }
    if (!m->flush_in_progress_) {
      assert(!m->flush_completed_);
      num_flush_not_started_--;
      if (num_flush_not_started_ == 0) {
        imm_flush_needed.store(false, std::memory_order_release);
      }
      m->flush_in_progress_ = true;  // flushing will start very soon
      ret->push_back(m);
    }
  }
  if (!atomic_flush || num_flush_not_started_ == 0) {
    flush_requested_ = false;  // start-flush request is complete
  }
}

void MemTableList::RollbackMemtableFlush(const autovector<MemTable*>& mems,
                                         uint64_t /*file_number*/) {
  AutoThreadOperationStageUpdater stage_updater(
      ThreadStatus::STAGE_MEMTABLE_ROLLBACK);
  assert(!mems.empty());

  // If the flush was not successful, then just reset state.
  // Maybe a succeeding attempt to flush will be successful.
  for (MemTable* m : mems) {
    assert(m->flush_in_progress_);
    assert(m->file_number_ == 0);

    m->flush_in_progress_ = false;
    m->flush_completed_ = false;
    m->edit_.Clear();
    num_flush_not_started_++;
  }
  imm_flush_needed.store(true, std::memory_order_release);
}

// Try record a successful flush in the manifest file. It might just return
// Status::OK letting a concurrent flush to do actual the recording..
Status MemTableList::TryInstallMemtableFlushResults(
    ColumnFamilyData* cfd, const MutableCFOptions& mutable_cf_options,
    const autovector<MemTable*>& mems, LogsWithPrepTracker* prep_tracker,
    VersionSet* vset, InstrumentedMutex* mu, uint64_t file_number,
    autovector<MemTable*>* to_delete, FSDirectory* db_directory,
    LogBuffer* log_buffer,
    std::list<std::unique_ptr<FlushJobInfo>>* committed_flush_jobs_info,
    IOStatus* io_s) {
  AutoThreadOperationStageUpdater stage_updater(
      ThreadStatus::STAGE_MEMTABLE_INSTALL_FLUSH_RESULTS);
  mu->AssertHeld();

  // Flush was successful
  // Record the status on the memtable object. Either this call or a call by a
  // concurrent flush thread will read the status and write it to manifest.
  for (size_t i = 0; i < mems.size(); ++i) {
    // All the edits are associated with the first memtable of this batch.
    assert(i == 0 || mems[i]->GetEdits()->NumEntries() == 0);

    mems[i]->flush_completed_ = true;
    mems[i]->file_number_ = file_number;
  }

  // if some other thread is already committing, then return
  Status s;
  if (commit_in_progress_) {
    TEST_SYNC_POINT("MemTableList::TryInstallMemtableFlushResults:InProgress");
    return s;
  }

  // Only a single thread can be executing this piece of code
  commit_in_progress_ = true;

  // Retry until all completed flushes are committed. New flushes can finish
  // while the current thread is writing manifest where mutex is released.
  while (s.ok()) {
    auto& memlist = current_->memlist_;
    // The back is the oldest; if flush_completed_ is not set to it, it means
    // that we were assigned a more recent memtable. The memtables' flushes must
    // be recorded in manifest in order. A concurrent flush thread, who is
    // assigned to flush the oldest memtable, will later wake up and does all
    // the pending writes to manifest, in order.
    if (memlist.empty() || !memlist.back()->flush_completed_) {
      break;
    }
    // scan all memtables from the earliest, and commit those
    // (in that order) that have finished flushing. Memtables
    // are always committed in the order that they were created.
    uint64_t batch_file_number = 0;
    size_t batch_count = 0;
    autovector<VersionEdit*> edit_list;
    autovector<MemTable*> memtables_to_flush;
    // enumerate from the last (earliest) element to see how many batch finished
    for (auto it = memlist.rbegin(); it != memlist.rend(); ++it) {
      MemTable* m = *it;
      if (!m->flush_completed_) {
        break;
      }
      if (it == memlist.rbegin() || batch_file_number != m->file_number_) {
        batch_file_number = m->file_number_;
        if (m->edit_.GetBlobFileAdditions().empty()) {
          ROCKS_LOG_BUFFER(log_buffer,
                           "[%s] Level-0 commit table #%" PRIu64 " started",
                           cfd->GetName().c_str(), m->file_number_);
        } else {
          ROCKS_LOG_BUFFER(log_buffer,
                           "[%s] Level-0 commit table #%" PRIu64
                           " (+%zu blob files) started",
                           cfd->GetName().c_str(), m->file_number_,
                           m->edit_.GetBlobFileAdditions().size());
        }

        edit_list.push_back(&m->edit_);
        memtables_to_flush.push_back(m);
#ifndef ROCKSDB_LITE
        std::unique_ptr<FlushJobInfo> info = m->ReleaseFlushJobInfo();
        if (info != nullptr) {
          committed_flush_jobs_info->push_back(std::move(info));
        }
#else
        (void)committed_flush_jobs_info;
#endif  // !ROCKSDB_LITE
      }
      batch_count++;
    }

    // TODO(myabandeh): Not sure how batch_count could be 0 here.
    if (batch_count > 0) {
      uint64_t min_wal_number_to_keep = 0;
      if (vset->db_options()->allow_2pc) {
        assert(edit_list.size() > 0);
        min_wal_number_to_keep = PrecomputeMinLogNumberToKeep2PC(
            vset, *cfd, edit_list, memtables_to_flush, prep_tracker);
        // We piggyback the information of  earliest log file to keep in the
        // manifest entry for the last file flushed.
        edit_list.back()->SetMinLogNumberToKeep(min_wal_number_to_keep);
      } else {
        min_wal_number_to_keep =
            PrecomputeMinLogNumberToKeepNon2PC(vset, *cfd, edit_list);
      }

      std::unique_ptr<VersionEdit> wal_deletion;
      if (vset->db_options()->track_and_verify_wals_in_manifest) {
        const auto& wals = vset->GetWalSet().GetWals();
        if (!wals.empty() && min_wal_number_to_keep > wals.begin()->first) {
          wal_deletion.reset(new VersionEdit);
          wal_deletion->DeleteWalsBefore(min_wal_number_to_keep);
          edit_list.push_back(wal_deletion.get());
        }
      }

      const auto manifest_write_cb = [this, cfd, batch_count, log_buffer,
                                      to_delete, mu](const Status& status) {
        RemoveMemTablesOrRestoreFlags(status, cfd, batch_count, log_buffer,
                                      to_delete, mu);
      };

      // this can release and reacquire the mutex.
      s = vset->LogAndApply(cfd, mutable_cf_options, edit_list, mu,
                            db_directory, /*new_descriptor_log=*/false,
                            /*column_family_options=*/nullptr,
                            manifest_write_cb);
      *io_s = vset->io_status();
    }
  }
  commit_in_progress_ = false;
  return s;
}

// New memtables are inserted at the front of the list.
void MemTableList::Add(MemTable* m, autovector<MemTable*>* to_delete) {
  assert(static_cast<int>(current_->memlist_.size()) >= num_flush_not_started_);
  InstallNewVersion();
  // this method is used to move mutable memtable into an immutable list.
  // since mutable memtable is already refcounted by the DBImpl,
  // and when moving to the imutable list we don't unref it,
  // we don't have to ref the memtable here. we just take over the
  // reference from the DBImpl.
  current_->Add(m, to_delete);
  m->MarkImmutable();
  num_flush_not_started_++;
  if (num_flush_not_started_ == 1) {
    imm_flush_needed.store(true, std::memory_order_release);
  }
  UpdateCachedValuesFromMemTableListVersion();
  ResetTrimHistoryNeeded();
}

bool MemTableList::TrimHistory(autovector<MemTable*>* to_delete, size_t usage) {
  InstallNewVersion();
  bool ret = current_->TrimHistory(to_delete, usage);
  UpdateCachedValuesFromMemTableListVersion();
  ResetTrimHistoryNeeded();
  return ret;
}

// Returns an estimate of the number of bytes of data in use.
size_t MemTableList::ApproximateUnflushedMemTablesMemoryUsage() {
  size_t total_size = 0;
  for (auto& memtable : current_->memlist_) {
    total_size += memtable->ApproximateMemoryUsage();
  }
  return total_size;
}

size_t MemTableList::ApproximateMemoryUsage() { return current_memory_usage_; }

size_t MemTableList::ApproximateMemoryUsageExcludingLast() const {
  const size_t usage =
      current_memory_usage_excluding_last_.load(std::memory_order_relaxed);
  return usage;
}

bool MemTableList::HasHistory() const {
  const bool has_history = current_has_history_.load(std::memory_order_relaxed);
  return has_history;
}

void MemTableList::UpdateCachedValuesFromMemTableListVersion() {
  const size_t total_memtable_size =
      current_->ApproximateMemoryUsageExcludingLast();
  current_memory_usage_excluding_last_.store(total_memtable_size,
                                             std::memory_order_relaxed);

  const bool has_history = current_->HasHistory();
  current_has_history_.store(has_history, std::memory_order_relaxed);
}

uint64_t MemTableList::ApproximateOldestKeyTime() const {
  if (!current_->memlist_.empty()) {
    return current_->memlist_.back()->ApproximateOldestKeyTime();
  }
  return std::numeric_limits<uint64_t>::max();
}

void MemTableList::InstallNewVersion() {
  if (current_->refs_ == 1) {
    // we're the only one using the version, just keep using it
  } else {
    // somebody else holds the current version, we need to create new one
    MemTableListVersion* version = current_;
    current_ = new MemTableListVersion(&current_memory_usage_, *version);
    current_->Ref();
    version->Unref();
  }
}

void MemTableList::RemoveMemTablesOrRestoreFlags(
    const Status& s, ColumnFamilyData* cfd, size_t batch_count,
    LogBuffer* log_buffer, autovector<MemTable*>* to_delete,
    InstrumentedMutex* mu) {
  assert(mu);
  mu->AssertHeld();
  assert(to_delete);
  // we will be changing the version in the next code path,
  // so we better create a new one, since versions are immutable
  InstallNewVersion();

  // All the later memtables that have the same filenum
  // are part of the same batch. They can be committed now.
  uint64_t mem_id = 1;  // how many memtables have been flushed.

  // commit new state only if the column family is NOT dropped.
  // The reason is as follows (refer to
  // ColumnFamilyTest.FlushAndDropRaceCondition).
  // If the column family is dropped, then according to LogAndApply, its
  // corresponding flush operation is NOT written to the MANIFEST. This
  // means the DB is not aware of the L0 files generated from the flush.
  // By committing the new state, we remove the memtable from the memtable
  // list. Creating an iterator on this column family will not be able to
  // read full data since the memtable is removed, and the DB is not aware
  // of the L0 files, causing MergingIterator unable to build child
  // iterators. RocksDB contract requires that the iterator can be created
  // on a dropped column family, and we must be able to
  // read full data as long as column family handle is not deleted, even if
  // the column family is dropped.
  if (s.ok() && !cfd->IsDropped()) {  // commit new state
    while (batch_count-- > 0) {
      MemTable* m = current_->memlist_.back();
      if (m->edit_.GetBlobFileAdditions().empty()) {
        ROCKS_LOG_BUFFER(log_buffer,
                         "[%s] Level-0 commit table #%" PRIu64
                         ": memtable #%" PRIu64 " done",
                         cfd->GetName().c_str(), m->file_number_, mem_id);
      } else {
        ROCKS_LOG_BUFFER(log_buffer,
                         "[%s] Level-0 commit table #%" PRIu64
                         " (+%zu blob files)"
                         ": memtable #%" PRIu64 " done",
                         cfd->GetName().c_str(), m->file_number_,
                         m->edit_.GetBlobFileAdditions().size(), mem_id);
      }

      assert(m->file_number_ > 0);
      current_->Remove(m, to_delete);
      UpdateCachedValuesFromMemTableListVersion();
      ResetTrimHistoryNeeded();
      ++mem_id;
    }
  } else {
    for (auto it = current_->memlist_.rbegin(); batch_count-- > 0; ++it) {
      MemTable* m = *it;
      // commit failed. setup state so that we can flush again.
      if (m->edit_.GetBlobFileAdditions().empty()) {
        ROCKS_LOG_BUFFER(log_buffer,
                         "Level-0 commit table #%" PRIu64 ": memtable #%" PRIu64
                         " failed",
                         m->file_number_, mem_id);
      } else {
        ROCKS_LOG_BUFFER(log_buffer,
                         "Level-0 commit table #%" PRIu64
                         " (+%zu blob files)"
                         ": memtable #%" PRIu64 " failed",
                         m->file_number_,
                         m->edit_.GetBlobFileAdditions().size(), mem_id);
      }

      m->flush_completed_ = false;
      m->flush_in_progress_ = false;
      m->edit_.Clear();
      num_flush_not_started_++;
      m->file_number_ = 0;
      imm_flush_needed.store(true, std::memory_order_release);
      ++mem_id;
    }
  }
}

uint64_t MemTableList::PrecomputeMinLogContainingPrepSection(
    const autovector<MemTable*>& memtables_to_flush) {
  uint64_t min_log = 0;

  for (auto& m : current_->memlist_) {
    // Assume the list is very short, we can live with O(m*n). We can optimize
    // if the performance has some problem.
    bool should_skip = false;
    for (MemTable* m_to_flush : memtables_to_flush) {
      if (m == m_to_flush) {
        should_skip = true;
        break;
      }
    }
    if (should_skip) {
      continue;
    }

    auto log = m->GetMinLogContainingPrepSection();

    if (log > 0 && (min_log == 0 || log < min_log)) {
      min_log = log;
    }
  }

  return min_log;
}

// Commit a successful atomic flush in the manifest file.
Status InstallMemtableAtomicFlushResults(
    const autovector<MemTableList*>* imm_lists,
    const autovector<ColumnFamilyData*>& cfds,
    const autovector<const MutableCFOptions*>& mutable_cf_options_list,
    const autovector<const autovector<MemTable*>*>& mems_list, VersionSet* vset,
    LogsWithPrepTracker* prep_tracker, InstrumentedMutex* mu,
    const autovector<FileMetaData*>& file_metas,
    autovector<MemTable*>* to_delete, FSDirectory* db_directory,
    LogBuffer* log_buffer) {
  AutoThreadOperationStageUpdater stage_updater(
      ThreadStatus::STAGE_MEMTABLE_INSTALL_FLUSH_RESULTS);
  mu->AssertHeld();

  size_t num = mems_list.size();
  assert(cfds.size() == num);
  if (imm_lists != nullptr) {
    assert(imm_lists->size() == num);
  }
  if (num == 0) {
    return Status::OK();
  }

  for (size_t k = 0; k != num; ++k) {
#ifndef NDEBUG
    const auto* imm =
        (imm_lists == nullptr) ? cfds[k]->imm() : imm_lists->at(k);
    if (!mems_list[k]->empty()) {
      assert((*mems_list[k])[0]->GetID() == imm->GetEarliestMemTableID());
    }
#endif
    assert(nullptr != file_metas[k]);
    for (size_t i = 0; i != mems_list[k]->size(); ++i) {
      assert(i == 0 || (*mems_list[k])[i]->GetEdits()->NumEntries() == 0);
      (*mems_list[k])[i]->SetFlushCompleted(true);
      (*mems_list[k])[i]->SetFileNumber(file_metas[k]->fd.GetNumber());
    }
  }

  Status s;

  autovector<autovector<VersionEdit*>> edit_lists;
  uint32_t num_entries = 0;
  for (const auto mems : mems_list) {
    assert(mems != nullptr);
    autovector<VersionEdit*> edits;
    assert(!mems->empty());
    edits.emplace_back((*mems)[0]->GetEdits());
    ++num_entries;
    edit_lists.emplace_back(edits);
  }

  // TODO(cc): after https://github.com/facebook/rocksdb/pull/7570, handle 2pc
  // here.
  std::unique_ptr<VersionEdit> wal_deletion;
  if (vset->db_options()->track_and_verify_wals_in_manifest) {
    uint64_t min_wal_number_to_keep =
        PrecomputeMinLogNumberToKeepNon2PC(vset, cfds, edit_lists);
    const auto& wals = vset->GetWalSet().GetWals();
    if (!wals.empty() && min_wal_number_to_keep > wals.begin()->first) {
      wal_deletion.reset(new VersionEdit);
      wal_deletion->DeleteWalsBefore(min_wal_number_to_keep);
      edit_lists.back().push_back(wal_deletion.get());
      ++num_entries;
    }
  }

  // Mark the version edits as an atomic group if the number of version edits
  // exceeds 1.
  if (cfds.size() > 1) {
    for (size_t i = 0; i < edit_lists.size(); i++) {
      assert((edit_lists[i].size() == 1) ||
             ((edit_lists[i].size() == 2) && (i == edit_lists.size() - 1)));
      for (auto& e : edit_lists[i]) {
        e->MarkAtomicGroup(--num_entries);
      }
    }
    assert(0 == num_entries);
  }

  if (vset->db_options()->allow_2pc) {
    uint64_t min_log_number_to_keep = port::kMaxUint64;
    for (size_t i = 0; i < cfds.size(); i++) {
      min_log_number_to_keep =
          std::min(min_log_number_to_keep,
                   PrecomputeMinLogNumberToKeep(vset, *cfds[i], edit_lists[i],
                                                *mems_list[i], prep_tracker));
    }
    edit_lists.back().back()->SetMinLogNumberToKeep(min_log_number_to_keep);
  }

  // this can release and reacquire the mutex.
  s = vset->LogAndApply(cfds, mutable_cf_options_list, edit_lists, mu,
                        db_directory);

  for (size_t k = 0; k != cfds.size(); ++k) {
    auto* imm = (imm_lists == nullptr) ? cfds[k]->imm() : imm_lists->at(k);
    imm->InstallNewVersion();
  }

  if (s.ok() || s.IsColumnFamilyDropped()) {
    for (size_t i = 0; i != cfds.size(); ++i) {
      if (cfds[i]->IsDropped()) {
        continue;
      }
      auto* imm = (imm_lists == nullptr) ? cfds[i]->imm() : imm_lists->at(i);
      for (auto m : *mems_list[i]) {
        assert(m->GetFileNumber() > 0);
        uint64_t mem_id = m->GetID();

        const VersionEdit* const edit = m->GetEdits();
        assert(edit);

        if (edit->GetBlobFileAdditions().empty()) {
          ROCKS_LOG_BUFFER(log_buffer,
                           "[%s] Level-0 commit table #%" PRIu64
                           ": memtable #%" PRIu64 " done",
                           cfds[i]->GetName().c_str(), m->GetFileNumber(),
                           mem_id);
        } else {
          ROCKS_LOG_BUFFER(log_buffer,
                           "[%s] Level-0 commit table #%" PRIu64
                           " (+%zu blob files)"
                           ": memtable #%" PRIu64 " done",
                           cfds[i]->GetName().c_str(), m->GetFileNumber(),
                           edit->GetBlobFileAdditions().size(), mem_id);
        }

        imm->current_->Remove(m, to_delete);
        imm->UpdateCachedValuesFromMemTableListVersion();
        imm->ResetTrimHistoryNeeded();
      }
    }
  } else {
    for (size_t i = 0; i != cfds.size(); ++i) {
      auto* imm = (imm_lists == nullptr) ? cfds[i]->imm() : imm_lists->at(i);
      for (auto m : *mems_list[i]) {
        uint64_t mem_id = m->GetID();

        const VersionEdit* const edit = m->GetEdits();
        assert(edit);

        if (edit->GetBlobFileAdditions().empty()) {
          ROCKS_LOG_BUFFER(log_buffer,
                           "[%s] Level-0 commit table #%" PRIu64
                           ": memtable #%" PRIu64 " failed",
                           cfds[i]->GetName().c_str(), m->GetFileNumber(),
                           mem_id);
        } else {
          ROCKS_LOG_BUFFER(log_buffer,
                           "[%s] Level-0 commit table #%" PRIu64
                           " (+%zu blob files)"
                           ": memtable #%" PRIu64 " failed",
                           cfds[i]->GetName().c_str(), m->GetFileNumber(),
                           edit->GetBlobFileAdditions().size(), mem_id);
        }

        m->SetFlushCompleted(false);
        m->SetFlushInProgress(false);
        m->GetEdits()->Clear();
        m->SetFileNumber(0);
        imm->num_flush_not_started_++;
      }
      imm->imm_flush_needed.store(true, std::memory_order_release);
    }
  }

  return s;
}

void MemTableList::RemoveOldMemTables(uint64_t log_number,
                                      autovector<MemTable*>* to_delete) {
  assert(to_delete != nullptr);
  InstallNewVersion();
  auto& memlist = current_->memlist_;
  autovector<MemTable*> old_memtables;
  for (auto it = memlist.rbegin(); it != memlist.rend(); ++it) {
    MemTable* mem = *it;
    if (mem->GetNextLogNumber() > log_number) {
      break;
    }
    old_memtables.push_back(mem);
  }

  for (auto it = old_memtables.begin(); it != old_memtables.end(); ++it) {
    MemTable* mem = *it;
    current_->Remove(mem, to_delete);
    --num_flush_not_started_;
    if (0 == num_flush_not_started_) {
      imm_flush_needed.store(false, std::memory_order_release);
    }
  }

  UpdateCachedValuesFromMemTableListVersion();
  ResetTrimHistoryNeeded();
}

}  // namespace ROCKSDB_NAMESPACE
