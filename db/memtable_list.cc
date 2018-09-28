//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
#include "db/memtable_list.h"

#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif

#include <inttypes.h>
#include <limits>
#include <string>
#include "db/db_impl.h"
#include "db/memtable.h"
#include "db/version_set.h"
#include "monitoring/thread_status_util.h"
#include "rocksdb/db.h"
#include "rocksdb/env.h"
#include "rocksdb/iterator.h"
#include "table/merging_iterator.h"
#include "util/coding.h"
#include "util/log_buffer.h"
#include "util/sync_point.h"

namespace rocksdb {

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
    size_t* parent_memtable_list_memory_usage, MemTableListVersion* old)
    : max_write_buffer_number_to_maintain_(
          old->max_write_buffer_number_to_maintain_),
      parent_memtable_list_memory_usage_(parent_memtable_list_memory_usage) {
  if (old != nullptr) {
    memlist_ = old->memlist_;
    for (auto& m : memlist_) {
      m->Ref();
    }

    memlist_history_ = old->memlist_history_;
    for (auto& m : memlist_history_) {
      m->Ref();
    }
  }
}

MemTableListVersion::MemTableListVersion(
    size_t* parent_memtable_list_memory_usage,
    int max_write_buffer_number_to_maintain)
    : max_write_buffer_number_to_maintain_(max_write_buffer_number_to_maintain),
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
                              Status* s, MergeContext* merge_context,
                              RangeDelAggregator* range_del_agg,
                              SequenceNumber* seq, const ReadOptions& read_opts,
                              ReadCallback* callback, bool* is_blob_index) {
  return GetFromList(&memlist_, key, value, s, merge_context, range_del_agg,
                     seq, read_opts, callback, is_blob_index);
}

bool MemTableListVersion::GetFromHistory(
    const LookupKey& key, std::string* value, Status* s,
    MergeContext* merge_context, RangeDelAggregator* range_del_agg,
    SequenceNumber* seq, const ReadOptions& read_opts, bool* is_blob_index) {
  return GetFromList(&memlist_history_, key, value, s, merge_context,
                     range_del_agg, seq, read_opts, nullptr /*read_callback*/,
                     is_blob_index);
}

bool MemTableListVersion::GetFromList(
    std::list<MemTable*>* list, const LookupKey& key, std::string* value,
    Status* s, MergeContext* merge_context, RangeDelAggregator* range_del_agg,
    SequenceNumber* seq, const ReadOptions& read_opts, ReadCallback* callback,
    bool* is_blob_index) {
  *seq = kMaxSequenceNumber;

  for (auto& memtable : *list) {
    SequenceNumber current_seq = kMaxSequenceNumber;

    bool done = memtable->Get(key, value, s, merge_context, range_del_agg,
                              &current_seq, read_opts, callback, is_blob_index);
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
      assert(*seq != kMaxSequenceNumber);
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
  for (auto& m : memlist_) {
    std::unique_ptr<InternalIterator> range_del_iter(
        m->NewRangeTombstoneIterator(read_opts));
    Status s = range_del_agg->AddTombstones(std::move(range_del_iter));
    if (!s.ok()) {
      return s;
    }
  }
  return Status::OK();
}

Status MemTableListVersion::AddRangeTombstoneIterators(
    const ReadOptions& read_opts,
    std::vector<InternalIterator*>* range_del_iters) {
  for (auto& m : memlist_) {
    auto* range_del_iter = m->NewRangeTombstoneIterator(read_opts);
    if (range_del_iter != nullptr) {
      range_del_iters->push_back(range_del_iter);
    }
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

  TrimHistory(to_delete);
}

// Removes m from list of memtables not flushed.  Caller should NOT Unref m.
void MemTableListVersion::Remove(MemTable* m,
                                 autovector<MemTable*>* to_delete) {
  assert(refs_ == 1);  // only when refs_ == 1 is MemTableListVersion mutable
  memlist_.remove(m);

  m->MarkFlushed();
  if (max_write_buffer_number_to_maintain_ > 0) {
    memlist_history_.push_front(m);
    TrimHistory(to_delete);
  } else {
    UnrefMemTable(to_delete, m);
  }
}

// Make sure we don't use up too much space in history
void MemTableListVersion::TrimHistory(autovector<MemTable*>* to_delete) {
  while (memlist_.size() + memlist_history_.size() >
             static_cast<size_t>(max_write_buffer_number_to_maintain_) &&
         !memlist_history_.empty()) {
    MemTable* x = memlist_history_.back();
    memlist_history_.pop_back();

    UnrefMemTable(to_delete, x);
  }
}

Status MemTableList::InstallMemtableFlushResults(
    autovector<MemTableList*>& imm_lists,
    const autovector<ColumnFamilyData*>& cfds,
    const autovector<const MutableCFOptions*>& mutable_cf_options_list,
    const autovector<const autovector<MemTable*>*>& mems_list,
    bool* atomic_flush_commit_in_progress, LogsWithPrepTracker* prep_tracker,
    VersionSet* vset, InstrumentedMutex* mu,
    const autovector<FileMetaData>& file_meta, autovector<MemTable*>* to_delete,
    Directory* db_directory, LogBuffer* log_buffer) {
  AutoThreadOperationStageUpdater stage_updater(
      ThreadStatus::STAGE_MEMTABLE_INSTALL_FLUSH_RESULTS);
  mu->AssertHeld();
  {
    // Link the (first) memtables to indicate that they should be committed to
    // the MANIFEST together.
    // Let N = imm_lists.size(), M = mems_list.size()
    // All memtables in mems_list form a group that
    // should be committed together. Since mems_list[i][0] (i = 0..M-1) store
    // the version edit information in edit_, we need to link only the first
    // element in mems_list.
    //
    // imm_lists[0]
    // ...
    //                            +-----------+
    //                            V           |
    //                    +-----------------+ |   +-----------------+
    // imm_lists[n1]:     | mems_list[0][0] | +-> | mems_list[0][1] | ...
    // ...                +-----------------+ |   +-----------------+
    //                            |           |
    //                            V           |
    //                    +-----------------+ |   +-----------------+
    // imm_lists[n2]:     | mems_list[1][0] | +-> | mems_list[1][1] | ...
    //                    +-----------------+ |   +-----------------+
    //                            |           |
    //                            V           |
    //                    +-----------------+ |
    // imm_lists[n3]:     |mems_list[M-1][0]| |
    // ...                +-----------------+ |
    //                            |           |
    //                            +-----------+
    // imm_lists[N - 1]:
    //
    MemTable* mem = nullptr;
    MemTable* head = nullptr;
    for (const auto& mems : mems_list) {
      if (!mems->empty()) {
        if (head == nullptr) {
          head = mems->front();
        }
        if (mem != nullptr) {
          mem->next_ = mems->front();
        }
        mem = mems->front();
      }
    }
    if (mem != nullptr) {
      mem->next_ = head;
    }
  }

  int num = static_cast<int>(imm_lists.size());
  assert(num == static_cast<int>(cfds.size()));
  for (int k = 0; k != num; ++k) {
    for (int i = 0; i != static_cast<int>(mems_list[k]->size()); ++i) {
      assert(i == 0 || (*mems_list[k])[i]->GetEdits()->NumEntries() == 0);
      (*mems_list[k])[i]->flush_completed_ = true;
      (*mems_list[k])[i]->file_number_ = file_meta[k].fd.GetNumber();
    }
  }

  assert(atomic_flush_commit_in_progress != nullptr);
  Status s;
  if (*atomic_flush_commit_in_progress) {
    return s;
  }

  *atomic_flush_commit_in_progress = true;

  while (s.ok()) {
    std::unordered_map<MemTable*, int> mem_to_idx;
    autovector<int> next;
    for (int i = 0; i != num; ++i) {
      const std::list<MemTable*>& memlist = imm_lists[i]->current_->memlist_;
      if (!memlist.empty()) {
        const auto it = memlist.rbegin();
        if ((*it)->flush_completed_) {
          mem_to_idx.insert({*it, i});
        }
      }
      next.push_back(-1);
    }
    if (mem_to_idx.empty()) {
      // Nothing to commit
      break;
    }
    for (int i = 0; i != num; ++i) {
      const std::list<MemTable*>& memlist = imm_lists[i]->current_->memlist_;
      if (!memlist.empty()) {
        const auto it = memlist.rbegin();
        assert((*it)->next_ != nullptr);
        if ((*it)->flush_completed_) {
          const auto next_it = mem_to_idx.find((*it)->next_);
          if (next_it != mem_to_idx.end()) {
            next[i] = next_it->second;
          }
        }
      }
    }
    while (true) {
      // Try to find a linked list formed by memtables at the rbegin side of
      // each imm_lists. If such a linked list exist, we have found a new group
      // to commit to MANIFEST.
      int k = 0;
      while (k < num && next[k] < 0) {
        ++k;
      }
      if (k == num) {
        break;
      }

      autovector<int> idx;
      int head = k;
      do {
        idx.push_back(k);
        k = next[k];
      } while (k != head && k >= 0);

      for (auto i : idx) {
        next[i] = -1;
      }

      if (k < 0) {
        continue;
      }

      uint32_t num_entries = 0;
      autovector<ColumnFamilyData*> tmp_cfds;
      autovector<const MutableCFOptions*> tmp_mutable_cf_options_list;
      std::vector<autovector<MemTable*>> memtables_to_flush;
      autovector<autovector<VersionEdit*>> edit_lists;
      for (auto i : idx) {
        tmp_cfds.push_back(cfds[i]);
        tmp_mutable_cf_options_list.push_back(mutable_cf_options_list[i]);
        const std::list<MemTable*>& memlist = imm_lists[i]->current_->memlist_;
        uint64_t batch_file_number = 0;
        autovector<MemTable*> tmp_mems;
        autovector<VersionEdit*> edits;
        for (auto it = memlist.rbegin(); it != memlist.rend(); ++it) {
          MemTable* m = *it;
          if (!m->flush_completed_ || (it != memlist.rbegin() &&
                                       m->file_number_ != batch_file_number)) {
            break;
          }
          if (it == memlist.rbegin()) {
            batch_file_number = m->file_number_;
            edits.push_back(m->GetEdits());
            ++num_entries;
          }
          tmp_mems.push_back(m);
        }
        edit_lists.push_back(edits);
        memtables_to_flush.push_back(tmp_mems);
      }

      uint32_t remaining = num_entries;
      for (auto& edit_list : edit_lists) {
        assert(edit_list.size() == 1);
        edit_list[0]->MarkAtomicGroup(--remaining);
      }
      assert(remaining == 0);

      size_t batch_sz = idx.size();
      assert(batch_sz > 0);
      assert(batch_sz == memtables_to_flush.size());
      assert(batch_sz == tmp_cfds.size());
      assert(batch_sz == edit_lists.size());

      if (vset->db_options()->allow_2pc) {
        for (int i = 0; i != static_cast<int>(batch_sz); ++i) {
          auto& edit_list = edit_lists[i];
          assert(!edit_list.empty());
          edit_list.back()->SetMinLogNumberToKeep(PrecomputeMinLogNumberToKeep(
              vset, *tmp_cfds[i], edit_list, memtables_to_flush[i],
              prep_tracker));
        }
      }
      s = vset->LogAndApply(tmp_cfds, tmp_mutable_cf_options_list, edit_lists,
                            mu, db_directory);
      for (auto i : idx) {
        imm_lists[i]->InstallNewVersion();
      }
      if (s.ok()) {
        for (size_t i = 0; i != batch_sz; ++i) {
          if (tmp_cfds[i]->IsDropped()) {
            continue;
          }
          for (auto m : memtables_to_flush[i]) {
            assert(m->file_number_ > 0);
            uint64_t mem_id = m->GetID();
            ROCKS_LOG_BUFFER(log_buffer,
                             "[%s] Level-0 commit table #%" PRIu64
                             ": memtable #%" PRIu64 " done",
                             tmp_cfds[i]->GetName().c_str(), m->file_number_,
                             mem_id);
            imm_lists[idx[i]]->current_->Remove(m, to_delete);
          }
        }
      } else {
        for (size_t i = 0; i != batch_sz; ++i) {
          for (auto m : memtables_to_flush[i]) {
            uint64_t mem_id = m->GetID();
            ROCKS_LOG_BUFFER(log_buffer,
                             "[%s] Level-0 commit table #%" PRIu64
                             ": memtable #%" PRIu64 " failed",
                             tmp_cfds[i]->GetName().c_str(), m->file_number_,
                             mem_id);
            m->flush_completed_ = false;
            m->flush_in_progress_ = false;
            m->edit_.Clear();
            m->file_number_ = 0;
            imm_lists[idx[i]]->num_flush_not_started_++;
          }
          imm_lists[idx[i]]->imm_flush_needed.store(true,
                                                    std::memory_order_release);
        }
      }
    }
  }

  *atomic_flush_commit_in_progress = false;
  return s;
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
  for (auto it = memlist.rbegin(); it != memlist.rend(); ++it) {
    MemTable* m = *it;
    if (max_memtable_id != nullptr && m->GetID() > *max_memtable_id) {
      continue;
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
  flush_requested_ = false;  // start-flush request is complete
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

// Record a successful flush in the manifest file
Status MemTableList::InstallMemtableFlushResults(
    ColumnFamilyData* cfd, const MutableCFOptions& mutable_cf_options,
    const autovector<MemTable*>& mems, LogsWithPrepTracker* prep_tracker,
    VersionSet* vset, InstrumentedMutex* mu, uint64_t file_number,
    autovector<MemTable*>* to_delete, Directory* db_directory,
    LogBuffer* log_buffer) {
  AutoThreadOperationStageUpdater stage_updater(
      ThreadStatus::STAGE_MEMTABLE_INSTALL_FLUSH_RESULTS);
  mu->AssertHeld();

  // flush was successful
  for (size_t i = 0; i < mems.size(); ++i) {
    // All the edits are associated with the first memtable of this batch.
    assert(i == 0 || mems[i]->GetEdits()->NumEntries() == 0);

    mems[i]->flush_completed_ = true;
    mems[i]->file_number_ = file_number;
  }

  // if some other thread is already committing, then return
  Status s;
  if (commit_in_progress_) {
    TEST_SYNC_POINT("MemTableList::InstallMemtableFlushResults:InProgress");
    return s;
  }

  // Only a single thread can be executing this piece of code
  commit_in_progress_ = true;

  // Retry until all completed flushes are committed. New flushes can finish
  // while the current thread is writing manifest where mutex is released.
  while (s.ok()) {
    auto& memlist = current_->memlist_;
    if (memlist.empty() || !memlist.back()->flush_completed_) {
      break;
    }
    // scan all memtables from the earliest, and commit those
    // (in that order) that have finished flushing. Memetables
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
        ROCKS_LOG_BUFFER(log_buffer,
                         "[%s] Level-0 commit table #%" PRIu64 " started",
                         cfd->GetName().c_str(), m->file_number_);
        edit_list.push_back(&m->edit_);
        memtables_to_flush.push_back(m);
      }
      batch_count++;
    }

    if (batch_count > 0) {
      if (vset->db_options()->allow_2pc) {
        assert(edit_list.size() > 0);
        // We piggyback the information of  earliest log file to keep in the
        // manifest entry for the last file flushed.
        edit_list.back()->SetMinLogNumberToKeep(PrecomputeMinLogNumberToKeep(
            vset, *cfd, edit_list, memtables_to_flush, prep_tracker));
      }

      // this can release and reacquire the mutex.
      s = vset->LogAndApply(cfd, mutable_cf_options, edit_list, mu,
                            db_directory);

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
      // corrresponding flush operation is NOT written to the MANIFEST. This
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
          ROCKS_LOG_BUFFER(log_buffer, "[%s] Level-0 commit table #%" PRIu64
                                       ": memtable #%" PRIu64 " done",
                           cfd->GetName().c_str(), m->file_number_, mem_id);
          assert(m->file_number_ > 0);
          current_->Remove(m, to_delete);
          ++mem_id;
        }
      } else {
        for (auto it = current_->memlist_.rbegin(); batch_count-- > 0; it++) {
          MemTable* m = *it;
          // commit failed. setup state so that we can flush again.
          ROCKS_LOG_BUFFER(log_buffer, "Level-0 commit table #%" PRIu64
                                       ": memtable #%" PRIu64 " failed",
                           m->file_number_, mem_id);
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
    current_ = new MemTableListVersion(&current_memory_usage_, current_);
    current_->Ref();
    version->Unref();
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

}  // namespace rocksdb
