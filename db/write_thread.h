//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//  This source code is also licensed under the GPLv2 license found in the
//  COPYING file in the root directory of this source tree.

#pragma once

#include <assert.h>
#include <stdint.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <type_traits>
#include <vector>

#include "db/write_callback.h"
#include "monitoring/instrumented_mutex.h"
#include "rocksdb/options.h"
#include "rocksdb/status.h"
#include "rocksdb/types.h"
#include "rocksdb/write_batch.h"
#include "util/autovector.h"

namespace rocksdb {

class WriteThread {
 public:
  enum State : uint8_t {
    // The initial state of a writer.  This is a Writer that is
    // waiting in JoinBatchGroup.  This state can be left when another
    // thread informs the waiter that it has become a group leader
    // (-> STATE_GROUP_LEADER), when a leader that has chosen to be
    // non-parallel informs a follower that its writes have been committed
    // (-> STATE_COMPLETED), or when a leader that has chosen to perform
    // updates in parallel and needs this Writer to apply its batch (->
    // STATE_PARALLEL_FOLLOWER).
    STATE_INIT = 1,

    // The state used to inform a waiting Writer that it has become the
    // leader, and it should now build a write batch group.  Tricky:
    // this state is not used if newest_writer_ is empty when a writer
    // enqueues itself, because there is no need to wait (or even to
    // create the mutex and condvar used to wait) in that case.  This is
    // a terminal state unless the leader chooses to make this a parallel
    // batch, in which case the last parallel worker to finish will move
    // the leader to STATE_COMPLETED.
    STATE_GROUP_LEADER = 2,

    STATE_WAL_WRITER = 4,

    STATE_MEMTABLE_WRITER = 8,

    // A Writer that has returned as a follower in a parallel group.
    // It should apply its batch to the memtable and then call
    // CompleteParallelWorker.  When someone calls ExitAsBatchGroupLeader
    // or EarlyExitParallelGroup this state will get transitioned to
    // STATE_COMPLETED.
    STATE_PARALLEL_FOLLOWER = 16,

    // A follower whose writes have been applied, or a parallel leader
    // whose followers have all finished their work.  This is a terminal
    // state.
    STATE_COMPLETED = 32,

    // A state indicating that the thread may be waiting using StateMutex()
    // and StateCondVar()
    STATE_LOCKED_WAITING = 64,
  };

  struct Writer;

  struct WriteGroup {
    Writer* leader = nullptr;
    Writer* last_writer = nullptr;
    SequenceNumber last_sequence;
    // before running goes to zero, status needs leader->StateMutex()
    Status status;
    std::atomic<uint32_t> running;
    size_t size = 0;

    struct Iterator {
      Writer* writer;
      Writer* last_writer;

      explicit Iterator(Writer* w, Writer* last)
          : writer(w), last_writer(last) {}

      Writer* operator*() const { return writer; }

      Iterator& operator++() {
        assert(writer != nullptr);
        if (writer == last_writer) {
          writer = nullptr;
        } else {
          writer = writer->link_newer;
        }
        return *this;
      }

      bool operator!=(const Iterator& other) const {
        return writer != other.writer;
      }
    };

    Iterator begin() const { return Iterator(leader, last_writer); }
    Iterator end() const { return Iterator(nullptr, nullptr); }

    autovector<Writer*> ToVector() {
      autovector<Writer*> v;
      for (auto w : *this) {
        v.push_back(w);
      }
      return v;
    }
  };

  // Information kept for every waiting writer.
  struct Writer {
    WriteBatch* batch;
    bool sync;
    bool no_slowdown;
    bool disable_wal;
    bool disable_memtable;
    uint64_t log_used;  // log number that this batch was inserted into
    uint64_t log_ref;   // log number that memtable insert should reference
    WriteCallback* callback;
    bool made_waitable;          // records lazy construction of mutex and cv
    std::atomic<uint8_t> state;  // write under StateMutex() or pre-link
    WriteGroup* write_group;
    SequenceNumber sequence;  // the sequence number to use for the first key
    Status status;            // status of memtable inserter
    Status callback_status;   // status returned by callback->Callback()
    std::aligned_storage<sizeof(std::mutex)>::type state_mutex_bytes;
    std::aligned_storage<sizeof(std::condition_variable)>::type state_cv_bytes;
    Writer* link_older;  // read/write only before linking, or as leader
    Writer* link_newer;  // lazy, read/write only before linking, or as leader

    Writer()
        : batch(nullptr),
          sync(false),
          no_slowdown(false),
          disable_wal(false),
          disable_memtable(false),
          log_used(0),
          log_ref(0),
          callback(nullptr),
          made_waitable(false),
          state(STATE_INIT),
          write_group(nullptr),
          link_older(nullptr),
          link_newer(nullptr) {}

    Writer(const WriteOptions& write_options, WriteBatch* _batch,
           WriteCallback* _callback, uint64_t _log_ref, bool _disable_memtable)
        : batch(_batch),
          sync(write_options.sync),
          no_slowdown(write_options.no_slowdown),
          disable_wal(write_options.disableWAL),
          disable_memtable(_disable_memtable),
          log_used(0),
          log_ref(_log_ref),
          callback(_callback),
          made_waitable(false),
          state(STATE_INIT),
          write_group(nullptr),
          link_older(nullptr),
          link_newer(nullptr) {}

    ~Writer() {
      if (made_waitable) {
        StateMutex().~mutex();
        StateCV().~condition_variable();
      }
    }

    bool CheckCallback(DB* db) {
      if (callback != nullptr) {
        callback_status = callback->Callback(db);
      }
      return callback_status.ok();
    }

    void CreateMutex() {
      if (!made_waitable) {
        // Note that made_waitable is tracked separately from state
        // transitions, because we can't atomically create the mutex and
        // link into the list.
        made_waitable = true;
        new (&state_mutex_bytes) std::mutex;
        new (&state_cv_bytes) std::condition_variable;
      }
    }

    // returns the aggregate status of this Writer
    Status FinalStatus() {
      if (!status.ok()) {
        // a non-ok memtable write status takes presidence
        // assert(callback == nullptr || callback_status.ok());
        return status;
      } else if (!callback_status.ok()) {
        // if the callback failed then that is the status we want
        // because a memtable insert should not have been attempted
        assert(callback != nullptr);
        assert(status.ok());
        return callback_status;
      } else {
        // if there is no callback then we only care about
        // the memtable insert status
        assert(callback == nullptr || callback_status.ok());
        return status;
      }
    }

    bool CallbackFailed() {
      return (callback != nullptr) && !callback_status.ok();
    }

    bool ShouldWriteToMemtable() {
      return status.ok() && !CallbackFailed() && !disable_memtable;
    }

    bool ShouldWriteToWAL() {
      return status.ok() && !CallbackFailed() && !disable_wal;
    }

    // No other mutexes may be acquired while holding StateMutex(), it is
    // always last in the order
    std::mutex& StateMutex() {
      assert(made_waitable);
      return *static_cast<std::mutex*>(static_cast<void*>(&state_mutex_bytes));
    }

    std::condition_variable& StateCV() {
      assert(made_waitable);
      return *static_cast<std::condition_variable*>(
                 static_cast<void*>(&state_cv_bytes));
    }
  };

  struct AdaptationContext {
    const char* name;
    std::atomic<int32_t> value;

    explicit AdaptationContext(const char* name0) : name(name0), value(0) {}
  };

  WriteThread(uint64_t max_yield_usec, uint64_t slow_yield_usec);

  virtual ~WriteThread() = default;

  // Waits for w->state & goal_mask using w->StateMutex().  Returns
  // the state that satisfies goal_mask.
  uint8_t BlockingAwaitState(Writer* w, uint8_t goal_mask);

  // Blocks until w->state & goal_mask, returning the state value
  // that satisfied the predicate.  Uses ctx to adaptively use
  // std::this_thread::yield() to avoid mutex overheads.  ctx should be
  // a context-dependent static.
  uint8_t AwaitState(Writer* w, uint8_t goal_mask, AdaptationContext* ctx);

  void SetState(Writer* w, uint8_t new_state);

  // Waits for all preceding writers (unlocking mu while waiting), then
  // registers w as the currently proceeding writer.
  //
  // Writer* w:              A Writer not eligible for batching
  // InstrumentedMutex* mu:  The db mutex, to unlock while waiting
  // REQUIRES: db mutex held
  virtual void EnterUnbatched(Writer* w, InstrumentedMutex* mu) = 0;

  // Completes a Writer begun with EnterUnbatched, unblocking subsequent
  // writers.
  virtual void ExitUnbatched(Writer* w) = 0;

 private:
  uint64_t max_yield_usec_;
  uint64_t slow_yield_usec_;
};

class WriteThreadImpl : public WriteThread {
 public:
  WriteThreadImpl(uint64_t max_yield_usec, uint64_t slow_yield_usec);

  virtual ~WriteThreadImpl() = default;

  // IMPORTANT: None of the methods in this class rely on the db mutex
  // for correctness. All of the methods except JoinBatchGroup and
  // EnterUnbatched may be called either with or without the db mutex held.
  // Correctness is maintained by ensuring that only a single thread is
  // a leader at a time.

  // Registers w as ready to become part of a batch group, waits until the
  // caller should perform some work, and returns the current state of the
  // writer.  If w has become the leader of a write batch group, returns
  // STATE_GROUP_LEADER.  If w has been made part of a sequential batch
  // group and the leader has performed the write, returns STATE_DONE.
  // If w has been made part of a parallel batch group and is responsible
  // for updating the memtable, returns STATE_PARALLEL_FOLLOWER.
  //
  // The db mutex SHOULD NOT be held when calling this function, because
  // it will block.
  //
  // Writer* w:        Writer to be executed as part of a batch group
  void JoinBatchGroup(Writer* w);

  // Constructs a write batch group led by leader, which should be a
  // Writer passed to JoinBatchGroup on the current thread.
  //
  // Writer* leader:          Writer that is STATE_GROUP_LEADER
  // WriteGroup* write_group: Out-param of group members
  // returns:                 Total batch group byte size
  size_t EnterAsBatchGroupLeader(Writer* leader, WriteGroup* write_group);

  // Causes JoinBatchGroup to return STATE_PARALLEL_FOLLOWER for all of the
  // non-leader members of this write batch group.  Sets Writer::sequence
  // before waking them up.
  //
  // WriteGroup* write_group: Extra state used to coordinate the parallel add
  // SequenceNumber sequence:    Starting sequence number to assign to Writer-s
  void LaunchParallelFollowers(WriteGroup& write_group,
                               SequenceNumber sequence);

  // Reports the completion of w's batch to the parallel group leader, and
  // waits for the rest of the parallel batch to complete.  Returns true
  // if this thread is the last to complete, and hence should advance
  // the sequence number and then call EarlyExitParallelGroup, false if
  // someone else has already taken responsibility for that.
  bool CompleteParallelWorker(Writer* w);

  // Exit batch group on behalf of batch group leader.
  void ExitAsBatchGroupFollower(Writer* w);

  // Unlinks the Writer-s in a batch group, wakes up the non-leaders,
  // and wakes up the next leader (if any).
  //
  // WriteGroup* write_group: the write group
  // Status status:           Status of write operation
  void ExitAsBatchGroupLeader(WriteGroup& write_group, Status status);

  virtual void EnterUnbatched(Writer* w, InstrumentedMutex* mu) override;

  virtual void ExitUnbatched(Writer* w) override;

 private:
  // Points to the newest pending Writer.  Only leader can remove
  // elements, adding can be done lock-free by anybody
  std::atomic<Writer*> newest_writer_;

  // Links w into the newest_writer_ list. Sets *linked_as_leader to
  // true if w was linked directly into the leader position.  Safe to
  // call from multiple threads without external locking.
  void LinkOne(Writer* w, bool* linked_as_leader);

  // Computes any missing link_newer links.  Should not be called
  // concurrently with itself.
  void CreateMissingNewerLinks(Writer* head);
};

}  // namespace rocksdb
