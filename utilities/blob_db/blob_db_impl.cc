//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
#ifndef ROCKSDB_LITE

#include "utilities/blob_db/blob_db_impl.h"
#include <algorithm>
#include <cinttypes>
#include <iomanip>
#include <limits>
#include <memory>

#include "db/db_impl.h"
#include "db/write_batch_internal.h"
#include "monitoring/instrumented_mutex.h"
#include "rocksdb/convenience.h"
#include "rocksdb/env.h"
#include "rocksdb/iterator.h"
#include "rocksdb/utilities/stackable_db.h"
#include "rocksdb/utilities/transaction.h"
#include "table/block.h"
#include "table/block_based_table_builder.h"
#include "table/block_builder.h"
#include "table/meta_blocks.h"
#include "util/cast_util.h"
#include "util/crc32c.h"
#include "util/file_reader_writer.h"
#include "util/filename.h"
#include "util/logging.h"
#include "util/mutexlock.h"
#include "util/random.h"
#include "util/sync_point.h"
#include "util/timer_queue.h"
#include "utilities/transactions/optimistic_transaction.h"
#include "utilities/transactions/optimistic_transaction_db_impl.h"

namespace {
int kBlockBasedTableVersionFormat = 2;

void extendTTL(rocksdb::blob_db::ttlrange_t* ttl_range, uint64_t ttl) {
  ttl_range->first = std::min(ttl_range->first, ttl);
  ttl_range->second = std::max(ttl_range->second, ttl);
}

void extendTimestamps(rocksdb::blob_db::tsrange_t* ts_range, uint64_t ts) {
  ts_range->first = std::min(ts_range->first, ts);
  ts_range->second = std::max(ts_range->second, ts);
}

void extendSN(rocksdb::blob_db::snrange_t* sn_range,
              rocksdb::SequenceNumber sn) {
  sn_range->first = std::min(sn_range->first, sn);
  sn_range->second = std::max(sn_range->second, sn);
}
}  // end namespace

namespace rocksdb {

namespace blob_db {

// BlobHandle is a pointer to the blob that is stored in the LSM
class BlobHandle {
 public:
  BlobHandle()
      : file_number_(std::numeric_limits<uint64_t>::max()),
        offset_(std::numeric_limits<uint64_t>::max()),
        size_(std::numeric_limits<uint64_t>::max()),
        compression_(kNoCompression) {}

  uint64_t filenumber() const { return file_number_; }
  void set_filenumber(uint64_t fn) { file_number_ = fn; }

  // The offset of the block in the file.
  uint64_t offset() const { return offset_; }
  void set_offset(uint64_t _offset) { offset_ = _offset; }

  // The size of the stored block
  uint64_t size() const { return size_; }
  void set_size(uint64_t _size) { size_ = _size; }

  CompressionType compression() const { return compression_; }
  void set_compression(CompressionType t) { compression_ = t; }

  void EncodeTo(std::string* dst) const;

  Status DecodeFrom(Slice* input);

  void clear();

 private:
  uint64_t file_number_;
  uint64_t offset_;
  uint64_t size_;
  CompressionType compression_;
};

void BlobHandle::EncodeTo(std::string* dst) const {
  // Sanity check that all fields have been set
  assert(offset_ != std::numeric_limits<uint64_t>::max());
  assert(size_ != std::numeric_limits<uint64_t>::max());
  assert(file_number_ != std::numeric_limits<uint64_t>::max());

  dst->reserve(30);
  PutVarint64(dst, file_number_);
  PutVarint64(dst, offset_);
  PutVarint64(dst, size_);
  dst->push_back(static_cast<unsigned char>(compression_));
}

void BlobHandle::clear() {
  file_number_ = std::numeric_limits<uint64_t>::max();
  offset_ = std::numeric_limits<uint64_t>::max();
  size_ = std::numeric_limits<uint64_t>::max();
  compression_ = kNoCompression;
}

Status BlobHandle::DecodeFrom(Slice* input) {
  if (GetVarint64(input, &file_number_) && GetVarint64(input, &offset_) &&
      GetVarint64(input, &size_)) {
    compression_ = static_cast<CompressionType>(input->data()[0]);
    return Status::OK();
  } else {
    clear();
    return Status::Corruption("bad blob handle");
  }
}

Random blob_rgen(static_cast<uint32_t>(time(nullptr)));

void BlobDBFlushBeginListener::OnFlushBegin(DB* db, const FlushJobInfo& info) {
  if (impl_) impl_->OnFlushBeginHandler(db, info);
}

WalFilter::WalProcessingOption BlobReconcileWalFilter::LogRecordFound(
    unsigned long long log_number, const std::string& log_file_name,
    const WriteBatch& batch, WriteBatch* new_batch, bool* batch_changed) {
  return WalFilter::WalProcessingOption::kContinueProcessing;
}

bool blobf_compare_ttl::operator()(const std::shared_ptr<BlobFile>& lhs,
                                   const std::shared_ptr<BlobFile>& rhs) const {
  if (lhs->ttl_range_.first < rhs->ttl_range_.first) return true;

  if (lhs->ttl_range_.first > rhs->ttl_range_.first) return false;

  return lhs->BlobFileNumber() > rhs->BlobFileNumber();
}

void EvictAllVersionsCompactionListener::InternalListener::OnCompaction(
    int level, const Slice& key,
    CompactionEventListener::CompactionListenerValueType value_type,
    const Slice& existing_value, const SequenceNumber& sn, bool is_new) {
  if (!is_new &&
      value_type ==
          CompactionEventListener::CompactionListenerValueType::kValue) {
    BlobHandle handle;
    Slice lsmval(existing_value);
    Status s = handle.DecodeFrom(&lsmval);
    if (s.ok()) {
      if (impl_->debug_level_ >= 3)
        ROCKS_LOG_INFO(impl_->db_options_.info_log,
                       "CALLBACK COMPACTED OUT KEY: %s SN: %d "
                       "NEW: %d FN: %" PRIu64 " OFFSET: %" PRIu64
                       " SIZE: %" PRIu64,
                       key.ToString().c_str(), sn, is_new, handle.filenumber(),
                       handle.offset(), handle.size());

      impl_->override_vals_q_.enqueue({handle.filenumber(), key.size(),
                                       handle.offset(), handle.size(), sn});
    }
  } else {
    if (impl_->debug_level_ >= 3)
      ROCKS_LOG_INFO(impl_->db_options_.info_log,
                     "CALLBACK NEW KEY: %s SN: %d NEW: %d",
                     key.ToString().c_str(), sn, is_new);
  }
}

BlobDBImpl::BlobDBImpl(const std::string& dbname,
                       const BlobDBOptions& blob_db_options,
                       const DBOptions& db_options)
    : BlobDB(nullptr),
      db_impl_(nullptr),
      env_(db_options.env),
      ttl_extractor_(blob_db_options.ttl_extractor.get()),
      wo_set_(false),
      bdb_options_(blob_db_options),
      db_options_(db_options),
      env_options_(db_options),
      dir_change_(false),
      next_file_number_(1),
      epoch_of_(0),
      shutdown_(false),
      current_epoch_(0),
      open_file_count_(0),
      last_period_write_(0),
      last_period_ampl_(0),
      total_periods_write_(0),
      total_periods_ampl_(0),
      total_blob_space_(0),
      open_p1_done_(false),
      debug_level_(0) {
  blob_dir_ = (bdb_options_.path_relative)
                  ? dbname + "/" + bdb_options_.blob_dir
                  : bdb_options_.blob_dir;
}

Status BlobDBImpl::LinkToBaseDB(DB* db) {
  assert(db_ == nullptr);
  assert(open_p1_done_);

  db_ = db;

  // the Base DB in-itself can be a stackable DB
  db_impl_ = static_cast_with_check<DBImpl, DB>(db_->GetRootDB());

  env_ = db_->GetEnv();

  opt_db_.reset(new OptimisticTransactionDBImpl(db, false));

  Status s = env_->CreateDirIfMissing(blob_dir_);
  if (!s.ok()) {
    ROCKS_LOG_WARN(db_options_.info_log,
                   "Failed to create blob directory: %s status: '%s'",
                   blob_dir_.c_str(), s.ToString().c_str());
  }
  s = env_->NewDirectory(blob_dir_, &dir_ent_);
  if (!s.ok()) {
    ROCKS_LOG_WARN(db_options_.info_log,
                   "Failed to open blob directory: %s status: '%s'",
                   blob_dir_.c_str(), s.ToString().c_str());
  }

  if (!bdb_options_.disable_background_tasks) {
    StartBackgroundTasks();
  }
  return s;
}

BlobDBOptions BlobDBImpl::GetBlobDBOptions() const { return bdb_options_; }

BlobDBImpl::BlobDBImpl(DB* db, const BlobDBOptions& blob_db_options)
    : BlobDB(db),
      db_impl_(static_cast_with_check<DBImpl, DB>(db)),
      opt_db_(new OptimisticTransactionDBImpl(db, false)),
      wo_set_(false),
      bdb_options_(blob_db_options),
      db_options_(db->GetOptions()),
      env_options_(db_->GetOptions()),
      dir_change_(false),
      next_file_number_(1),
      epoch_of_(0),
      shutdown_(false),
      current_epoch_(0),
      open_file_count_(0),
      last_period_write_(0),
      last_period_ampl_(0),
      total_periods_write_(0),
      total_periods_ampl_(0),
      total_blob_space_(0) {
  if (!bdb_options_.blob_dir.empty())
    blob_dir_ = (bdb_options_.path_relative)
                    ? db_->GetName() + "/" + bdb_options_.blob_dir
                    : bdb_options_.blob_dir;
}

BlobDBImpl::~BlobDBImpl() {
  // CancelAllBackgroundWork(db_, true);

  Shutdown();
}

Status BlobDBImpl::OpenPhase1() {
  assert(db_ == nullptr);
  if (blob_dir_.empty())
    return Status::NotSupported("No blob directory in options");

  std::unique_ptr<Directory> dir_ent;
  Status s = env_->NewDirectory(blob_dir_, &dir_ent);
  if (!s.ok()) {
    ROCKS_LOG_WARN(db_options_.info_log,
                   "Failed to open blob directory: %s status: '%s'",
                   blob_dir_.c_str(), s.ToString().c_str());
    open_p1_done_ = true;
    return Status::OK();
  }

  s = OpenAllFiles();
  open_p1_done_ = true;
  return s;
}

void BlobDBImpl::StartBackgroundTasks() {
  // store a call to a member function and object
  tqueue_.add(
      kReclaimOpenFilesPeriodMillisecs,
      std::bind(&BlobDBImpl::ReclaimOpenFiles, this, std::placeholders::_1));
  tqueue_.add(kGCCheckPeriodMillisecs,
              std::bind(&BlobDBImpl::RunGC, this, std::placeholders::_1));
  tqueue_.add(
      kDeleteCheckPeriodMillisecs,
      std::bind(&BlobDBImpl::EvictDeletions, this, std::placeholders::_1));
  tqueue_.add(
      kDeleteCheckPeriodMillisecs,
      std::bind(&BlobDBImpl::EvictCompacted, this, std::placeholders::_1));
  tqueue_.add(
      kDeleteObsoleteFilesPeriodMillisecs,
      std::bind(&BlobDBImpl::DeleteObsoleteFiles, this, std::placeholders::_1));
  tqueue_.add(kSanityCheckPeriodMillisecs,
              std::bind(&BlobDBImpl::SanityCheck, this, std::placeholders::_1));
  tqueue_.add(kWriteAmplificationStatsPeriodMillisecs,
              std::bind(&BlobDBImpl::WaStats, this, std::placeholders::_1));
  tqueue_.add(kFSyncFilesPeriodMillisecs,
              std::bind(&BlobDBImpl::FsyncFiles, this, std::placeholders::_1));
  tqueue_.add(
      kCheckSeqFilesPeriodMillisecs,
      std::bind(&BlobDBImpl::CheckSeqFiles, this, std::placeholders::_1));
}

void BlobDBImpl::Shutdown() { shutdown_.store(true); }

void BlobDBImpl::OnFlushBeginHandler(DB* db, const FlushJobInfo& info) {
  if (shutdown_.load()) return;

  // a callback that happens too soon needs to be ignored
  if (!db_) return;

  FsyncFiles(false);
}

Status BlobDBImpl::GetAllLogFiles(
    std::set<std::pair<uint64_t, std::string>>* file_nums) {
  std::vector<std::string> all_files;
  Status status = env_->GetChildren(blob_dir_, &all_files);
  if (!status.ok()) {
    return status;
  }

  for (const auto& f : all_files) {
    uint64_t number;
    FileType type;
    bool psucc = ParseFileName(f, &number, &type);
    if (psucc && type == kBlobFile) {
      file_nums->insert(std::make_pair(number, f));
    } else {
      ROCKS_LOG_WARN(db_options_.info_log,
                     "Skipping file in blob directory %s parse: %d type: %d",
                     f.c_str(), psucc, ((psucc) ? type : -1));
    }
  }

  return status;
}

Status BlobDBImpl::OpenAllFiles() {
  WriteLock wl(&mutex_);

  std::set<std::pair<uint64_t, std::string>> file_nums;
  Status status = GetAllLogFiles(&file_nums);

  if (!status.ok()) {
    ROCKS_LOG_ERROR(db_options_.info_log,
                    "Failed to collect files from blob dir: %s status: '%s'",
                    blob_dir_.c_str(), status.ToString().c_str());
    return status;
  }

  ROCKS_LOG_INFO(db_options_.info_log,
                 "BlobDir files path: %s count: %d min: %" PRIu64
                 " max: %" PRIu64,
                 blob_dir_.c_str(), static_cast<int>(file_nums.size()),
                 (file_nums.empty()) ? -1 : (file_nums.begin())->first,
                 (file_nums.empty()) ? -1 : (file_nums.end())->first);

  if (!file_nums.empty())
    next_file_number_.store((file_nums.rbegin())->first + 1);

  for (auto f_iter : file_nums) {
    std::string bfpath = BlobFileName(blob_dir_, f_iter.first);
    uint64_t size_bytes;
    Status s1 = env_->GetFileSize(bfpath, &size_bytes);
    if (!s1.ok()) {
      ROCKS_LOG_WARN(
          db_options_.info_log,
          "Unable to get size of %s. File skipped from open status: '%s'",
          bfpath.c_str(), s1.ToString().c_str());
      continue;
    }

    if (debug_level_ >= 1)
      ROCKS_LOG_INFO(db_options_.info_log, "Blob File open: %s size: %" PRIu64,
                     bfpath.c_str(), size_bytes);

    std::shared_ptr<BlobFile> bfptr =
        std::make_shared<BlobFile>(this, blob_dir_, f_iter.first);
    bfptr->SetFileSize(size_bytes);

    // since this file already existed, we will try to reconcile
    // deleted count with LSM
    bfptr->gc_once_after_open_ = true;

    // read header
    std::shared_ptr<Reader> reader;
    reader = bfptr->OpenSequentialReader(env_, db_options_, env_options_);
    s1 = reader->ReadHeader(&bfptr->header_);
    if (!s1.ok()) {
      ROCKS_LOG_ERROR(db_options_.info_log,
                      "Failure to read header for blob-file %s "
                      "status: '%s' size: %" PRIu64,
                      bfpath.c_str(), s1.ToString().c_str(), size_bytes);
      continue;
    }
    bfptr->header_valid_ = true;

    std::shared_ptr<RandomAccessFileReader> ra_reader =
        GetOrOpenRandomAccessReader(bfptr, env_, env_options_);

    BlobLogFooter bf;
    s1 = bfptr->ReadFooter(&bf);

    bfptr->CloseRandomAccessLocked();
    if (s1.ok()) {
      s1 = bfptr->SetFromFooterLocked(bf);
      if (!s1.ok()) {
        ROCKS_LOG_ERROR(db_options_.info_log,
                        "Header Footer mismatch for blob-file %s "
                        "status: '%s' size: %" PRIu64,
                        bfpath.c_str(), s1.ToString().c_str(), size_bytes);
        continue;
      }
    } else {
      ROCKS_LOG_INFO(db_options_.info_log,
                     "File found incomplete (w/o footer) %s", bfpath.c_str());

      // sequentially iterate over the file and read all the records
      ttlrange_t ttl_range(std::numeric_limits<uint32_t>::max(),
                           std::numeric_limits<uint32_t>::min());
      tsrange_t ts_range(std::numeric_limits<uint32_t>::max(),
                         std::numeric_limits<uint32_t>::min());
      snrange_t sn_range(std::numeric_limits<SequenceNumber>::max(),
                         std::numeric_limits<SequenceNumber>::min());

      uint64_t blob_count = 0;
      BlobLogRecord record;
      Reader::ReadLevel shallow = Reader::kReadHdrKeyFooter;

      uint64_t record_start = reader->GetNextByte();
      // TODO(arahut) - when we detect corruption, we should truncate
      while (reader->ReadRecord(&record, shallow).ok()) {
        ++blob_count;
        if (bfptr->HasTTL()) {
          extendTTL(&ttl_range, record.GetTTL());
        }
        if (bfptr->HasTimestamp()) {
          extendTimestamps(&ts_range, record.GetTimeVal());
        }
        extendSN(&sn_range, record.GetSN());
        record_start = reader->GetNextByte();
      }

      if (record_start != bfptr->GetFileSize()) {
        ROCKS_LOG_ERROR(db_options_.info_log,
                        "Blob file is corrupted or crashed during write %s"
                        " good_size: %" PRIu64 " file_size: %" PRIu64,
                        bfpath.c_str(), record_start, bfptr->GetFileSize());
      }

      if (!blob_count) {
        ROCKS_LOG_INFO(db_options_.info_log, "BlobCount = 0 in file %s",
                       bfpath.c_str());
        continue;
      }

      bfptr->SetBlobCount(blob_count);
      bfptr->SetSNRange(sn_range);

      if (bfptr->HasTimestamp()) bfptr->set_time_range(ts_range);

      ROCKS_LOG_INFO(db_options_.info_log,
                     "Blob File: %s blob_count: %" PRIu64
                     " size_bytes: %" PRIu64
                     " sn_range: (%d, %d) ts: %d ttl: %d",
                     bfpath.c_str(), blob_count, size_bytes, sn_range.first,
                     sn_range.second, bfptr->HasTimestamp(), bfptr->HasTTL());

      if (bfptr->HasTTL()) {
        ttl_range.second =
            std::max(ttl_range.second,
                     ttl_range.first + (uint32_t)bdb_options_.ttl_range_secs);
        bfptr->set_ttl_range(ttl_range);

        uint64_t now = EpochNow();
        if (ttl_range.second < now) {
          Status fstatus = CreateWriterLocked(bfptr);
          if (fstatus.ok()) fstatus = bfptr->WriteFooterAndCloseLocked();
          if (!fstatus.ok()) {
            ROCKS_LOG_ERROR(
                db_options_.info_log,
                "Failed to close Blob File: %s status: '%s'. Skipped",
                bfpath.c_str(), fstatus.ToString().c_str());
            continue;
          } else {
            ROCKS_LOG_ERROR(db_options_.info_log,
                            "Blob File Closed: %s now: %d ttl_range: (%d, %d)",
                            bfpath.c_str(), now, ttl_range.first,
                            ttl_range.second);
          }
        } else {
          open_blob_files_.insert(bfptr);
        }
      }
    }

    blob_files_.insert(std::make_pair(f_iter.first, bfptr));
  }

  return status;
}

void BlobDBImpl::CloseRandomAccessLocked(
    const std::shared_ptr<BlobFile>& bfile) {
  bfile->CloseRandomAccessLocked();
  open_file_count_--;
}

std::shared_ptr<RandomAccessFileReader> BlobDBImpl::GetOrOpenRandomAccessReader(
    const std::shared_ptr<BlobFile>& bfile, Env* env,
    const EnvOptions& env_options) {
  bool fresh_open = false;
  auto rar = bfile->GetOrOpenRandomAccessReader(env, env_options, &fresh_open);
  if (fresh_open) open_file_count_++;
  return rar;
}

std::shared_ptr<BlobFile> BlobDBImpl::NewBlobFile(const std::string& reason) {
  uint64_t file_num = next_file_number_++;
  auto bfile = std::make_shared<BlobFile>(this, blob_dir_, file_num);
  ROCKS_LOG_DEBUG(db_options_.info_log, "New blob file created: %s reason='%s'",
                  bfile->PathName().c_str(), reason.c_str());
  LogFlush(db_options_.info_log);
  return bfile;
}

Status BlobDBImpl::CreateWriterLocked(const std::shared_ptr<BlobFile>& bfile) {
  std::string fpath(bfile->PathName());
  std::unique_ptr<WritableFile> wfile;

  Status s = env_->ReopenWritableFile(fpath, &wfile, env_options_);
  if (!s.ok()) {
    ROCKS_LOG_ERROR(db_options_.info_log,
                    "Failed to open blob file for write: %s status: '%s'"
                    " exists: '%s'",
                    fpath.c_str(), s.ToString().c_str(),
                    env_->FileExists(fpath).ToString().c_str());
    return s;
  }

  std::unique_ptr<WritableFileWriter> fwriter;
  fwriter.reset(new WritableFileWriter(std::move(wfile), env_options_));

  uint64_t boffset = bfile->GetFileSize();
  if (debug_level_ >= 2 && boffset) {
    ROCKS_LOG_DEBUG(db_options_.info_log, "Open blob file: %s with offset: %d",
                    fpath.c_str(), boffset);
  }

  Writer::ElemType et = Writer::kEtNone;
  if (bfile->file_size_ == BlobLogHeader::kHeaderSize)
    et = Writer::kEtFileHdr;
  else if (bfile->file_size_ > BlobLogHeader::kHeaderSize)
    et = Writer::kEtFooter;
  else if (bfile->file_size_) {
    ROCKS_LOG_WARN(db_options_.info_log,
                   "Open blob file: %s with wrong size: %d", fpath.c_str(),
                   boffset);
    return Status::Corruption("Invalid blob file size");
  }

  bfile->log_writer_ = std::make_shared<Writer>(
      std::move(fwriter), bfile->file_number_, bdb_options_.bytes_per_sync,
      db_options_.use_fsync, boffset);
  bfile->log_writer_->last_elem_type_ = et;

  return s;
}

std::shared_ptr<BlobFile> BlobDBImpl::FindBlobFileLocked(
    uint64_t expiration) const {
  if (open_blob_files_.empty()) return nullptr;

  std::shared_ptr<BlobFile> tmp = std::make_shared<BlobFile>();
  tmp->ttl_range_ = std::make_pair(expiration, 0);

  auto citr = open_blob_files_.equal_range(tmp);
  if (citr.first == open_blob_files_.end()) {
    assert(citr.second == open_blob_files_.end());

    std::shared_ptr<BlobFile> check = *(open_blob_files_.rbegin());
    return (check->ttl_range_.second < expiration) ? nullptr : check;
  }

  if (citr.first != citr.second) return *(citr.first);

  auto finditr = citr.second;
  if (finditr != open_blob_files_.begin()) --finditr;

  bool b2 = (*finditr)->ttl_range_.second < expiration;
  bool b1 = (*finditr)->ttl_range_.first > expiration;

  return (b1 || b2) ? nullptr : (*finditr);
}

std::shared_ptr<Writer> BlobDBImpl::CheckOrCreateWriterLocked(
    const std::shared_ptr<BlobFile>& bfile) {
  std::shared_ptr<Writer> writer = bfile->GetWriter();
  if (writer) return writer;

  Status s = CreateWriterLocked(bfile);
  if (!s.ok()) return nullptr;

  writer = bfile->GetWriter();
  return writer;
}

void BlobDBImpl::UpdateWriteOptions(const WriteOptions& options) {
  if (!wo_set_.load(std::memory_order_relaxed)) {
    // DCLP
    WriteLock wl(&mutex_);
    if (!wo_set_.load(std::memory_order_acquire)) {
      wo_set_.store(true, std::memory_order_release);
      write_options_ = options;
    }
  }
}

std::shared_ptr<BlobFile> BlobDBImpl::SelectBlobFile() {
  uint32_t val = blob_rgen.Next();
  {
    ReadLock rl(&mutex_);
    if (open_simple_files_.size() == bdb_options_.num_concurrent_simple_blobs)
      return open_simple_files_[val % bdb_options_.num_concurrent_simple_blobs];
  }

  std::shared_ptr<BlobFile> bfile = NewBlobFile("SelectBlobFile");
  assert(bfile);

  // file not visible, hence no lock
  std::shared_ptr<Writer> writer = CheckOrCreateWriterLocked(bfile);
  if (!writer) {
    ROCKS_LOG_ERROR(db_options_.info_log,
                    "Failed to get writer from blob file: %s",
                    bfile->PathName().c_str());
    return nullptr;
  }

  bfile->file_size_ = BlobLogHeader::kHeaderSize;
  bfile->header_.compression_ = bdb_options_.compression;
  bfile->header_valid_ = true;

  // CHECK again
  WriteLock wl(&mutex_);
  if (open_simple_files_.size() == bdb_options_.num_concurrent_simple_blobs) {
    return open_simple_files_[val % bdb_options_.num_concurrent_simple_blobs];
  }

  Status s = writer->WriteHeader(bfile->header_);
  if (!s.ok()) {
    ROCKS_LOG_ERROR(db_options_.info_log,
                    "Failed to write header to new blob file: %s"
                    " status: '%s'",
                    bfile->PathName().c_str(), s.ToString().c_str());
    return nullptr;
  }

  dir_change_.store(true);
  blob_files_.insert(std::make_pair(bfile->BlobFileNumber(), bfile));
  open_simple_files_.push_back(bfile);
  return bfile;
}

std::shared_ptr<BlobFile> BlobDBImpl::SelectBlobFileTTL(uint64_t expiration) {
  assert(expiration != kNoExpiration);
  uint64_t epoch_read = 0;
  std::shared_ptr<BlobFile> bfile;
  {
    ReadLock rl(&mutex_);
    bfile = FindBlobFileLocked(expiration);
    epoch_read = epoch_of_.load();
  }

  if (bfile) {
    assert(!bfile->Immutable());
    return bfile;
  }

  uint64_t exp_low =
      (expiration / bdb_options_.ttl_range_secs) * bdb_options_.ttl_range_secs;
  uint64_t exp_high = exp_low + bdb_options_.ttl_range_secs;
  ttlrange_t ttl_guess = std::make_pair(exp_low, exp_high);

  bfile = NewBlobFile("SelectBlobFileTTL");
  assert(bfile);

  ROCKS_LOG_INFO(db_options_.info_log, "New blob file TTL range: %s %d %d",
                 bfile->PathName().c_str(), exp_low, exp_high);
  LogFlush(db_options_.info_log);

  // we don't need to take lock as no other thread is seeing bfile yet
  std::shared_ptr<Writer> writer = CheckOrCreateWriterLocked(bfile);
  if (!writer) {
    ROCKS_LOG_ERROR(db_options_.info_log,
                    "Failed to get writer from blob file with TTL: %s",
                    bfile->PathName().c_str());
    return nullptr;
  }

  bfile->header_.set_ttl_guess(ttl_guess);
  bfile->header_.compression_ = bdb_options_.compression;
  bfile->header_valid_ = true;
  bfile->file_size_ = BlobLogHeader::kHeaderSize;

  // set the first value of the range, since that is
  // concrete at this time.  also necessary to add to open_blob_files_
  bfile->ttl_range_ = ttl_guess;

  WriteLock wl(&mutex_);
  // in case the epoch has shifted in the interim, then check
  // check condition again - should be rare.
  if (epoch_of_.load() != epoch_read) {
    auto bfile2 = FindBlobFileLocked(expiration);
    if (bfile2) return bfile2;
  }

  Status s = writer->WriteHeader(bfile->header_);
  if (!s.ok()) {
    ROCKS_LOG_ERROR(db_options_.info_log,
                    "Failed to write header to new blob file: %s"
                    " status: '%s'",
                    bfile->PathName().c_str(), s.ToString().c_str());
    return nullptr;
  }

  dir_change_.store(true);
  blob_files_.insert(std::make_pair(bfile->BlobFileNumber(), bfile));
  open_blob_files_.insert(bfile);
  epoch_of_++;

  return bfile;
}

Status BlobDBImpl::Put(const WriteOptions& options, const Slice& key,
                       const Slice& value) {
  std::string new_value;
  Slice value_slice;
  uint64_t expiration = ExtractExpiration(key, value, &value_slice, &new_value);
  return PutUntil(options, key, value_slice, expiration);
}

Status BlobDBImpl::Delete(const WriteOptions& options, const Slice& key) {
  SequenceNumber lsn = db_impl_->GetLatestSequenceNumber();
  Status s = db_->Delete(options, key);

  // add deleted key to list of keys that have been deleted for book-keeping
  delete_keys_q_.enqueue({DefaultColumnFamily(), key.ToString(), lsn});
  return s;
}

Status BlobDBImpl::Write(const WriteOptions& opts, WriteBatch* updates) {
  class BlobInserter : public WriteBatch::Handler {
   private:
    BlobDBImpl* impl_;
    SequenceNumber sequence_;
    WriteBatch updates_blob_;
    Status batch_rewrite_status_;
    std::shared_ptr<BlobFile> last_file_;
    bool has_put_;
    std::string new_value_;
    uint32_t default_cf_id_;

   public:
    explicit BlobInserter(BlobDBImpl* impl, SequenceNumber seq)
        : impl_(impl),
          sequence_(seq),
          has_put_(false),
          default_cf_id_(reinterpret_cast<ColumnFamilyHandleImpl*>(
                             impl_->DefaultColumnFamily())
                             ->cfd()
                             ->GetID()) {}

    WriteBatch& updates_blob() { return updates_blob_; }

    Status batch_rewrite_status() { return batch_rewrite_status_; }

    std::shared_ptr<BlobFile>& last_file() { return last_file_; }

    bool has_put() { return has_put_; }

    virtual Status PutCF(uint32_t column_family_id, const Slice& key,
                         const Slice& value_slice) override {
      if (column_family_id != default_cf_id_) {
        batch_rewrite_status_ = Status::NotSupported(
            "Blob DB doesn't support non-default column family.");
        return batch_rewrite_status_;
      }
      Slice value_unc;
      uint64_t expiration =
          impl_->ExtractExpiration(key, value_slice, &value_unc, &new_value_);

      std::shared_ptr<BlobFile> bfile =
          (expiration != kNoExpiration)
              ? impl_->SelectBlobFileTTL(expiration)
              : ((last_file_) ? last_file_ : impl_->SelectBlobFile());
      if (last_file_ && last_file_ != bfile) {
        batch_rewrite_status_ = Status::NotFound("too many blob files");
        return batch_rewrite_status_;
      }

      if (!bfile) {
        batch_rewrite_status_ = Status::NotFound("blob file not found");
        return batch_rewrite_status_;
      }

      last_file_ = bfile;
      has_put_ = true;

      std::string compression_output;
      Slice value = impl_->GetCompressedSlice(value_unc, &compression_output);

      std::string headerbuf;
      Writer::ConstructBlobHeader(&headerbuf, key, value, expiration, -1);
      std::string index_entry;
      Status st = impl_->AppendBlob(bfile, headerbuf, key, value, &index_entry);
      if (st.ok()) {
        impl_->AppendSN(last_file_, sequence_);
        sequence_++;
      }

      if (expiration != kNoExpiration) {
        extendTTL(&(bfile->ttl_range_), expiration);
      }

      if (!st.ok()) {
        batch_rewrite_status_ = st;
      } else {
        WriteBatchInternal::Put(&updates_blob_, column_family_id, key,
                                index_entry);
      }
      return Status::OK();
    }

    virtual Status DeleteCF(uint32_t column_family_id,
                            const Slice& key) override {
      if (column_family_id != default_cf_id_) {
        batch_rewrite_status_ = Status::NotSupported(
            "Blob DB doesn't support non-default column family.");
        return batch_rewrite_status_;
      }
      WriteBatchInternal::Delete(&updates_blob_, column_family_id, key);
      sequence_++;
      return Status::OK();
    }

    virtual Status DeleteRange(uint32_t column_family_id,
                               const Slice& begin_key, const Slice& end_key) {
      if (column_family_id != default_cf_id_) {
        batch_rewrite_status_ = Status::NotSupported(
            "Blob DB doesn't support non-default column family.");
        return batch_rewrite_status_;
      }
      WriteBatchInternal::DeleteRange(&updates_blob_, column_family_id,
                                      begin_key, end_key);
      return Status::OK();
    }

    virtual Status SingleDeleteCF(uint32_t /*column_family_id*/,
                                  const Slice& /*key*/) override {
      batch_rewrite_status_ =
          Status::NotSupported("Not supported operation in blob db.");
      return batch_rewrite_status_;
    }

    virtual Status MergeCF(uint32_t /*column_family_id*/, const Slice& /*key*/,
                           const Slice& /*value*/) override {
      batch_rewrite_status_ =
          Status::NotSupported("Not supported operation in blob db.");
      return batch_rewrite_status_;
    }

    virtual void LogData(const Slice& blob) override {
      updates_blob_.PutLogData(blob);
    }
  };

  MutexLock l(&write_mutex_);

  SequenceNumber sequence = db_impl_->GetLatestSequenceNumber() + 1;
  BlobInserter blob_inserter(this, sequence);
  updates->Iterate(&blob_inserter);

  if (!blob_inserter.batch_rewrite_status().ok()) {
    return blob_inserter.batch_rewrite_status();
  }

  Status s = db_->Write(opts, &(blob_inserter.updates_blob()));
  if (!s.ok()) {
    return s;
  }

  if (blob_inserter.has_put()) {
    s = CloseBlobFileIfNeeded(blob_inserter.last_file());
    if (!s.ok()) {
      return s;
    }
  }

  // add deleted key to list of keys that have been deleted for book-keeping
  class DeleteBookkeeper : public WriteBatch::Handler {
   public:
    explicit DeleteBookkeeper(BlobDBImpl* impl, const SequenceNumber& seq)
        : impl_(impl), sequence_(seq) {}

    virtual Status PutCF(uint32_t /*column_family_id*/, const Slice& /*key*/,
                         const Slice& /*value*/) override {
      sequence_++;
      return Status::OK();
    }

    virtual Status DeleteCF(uint32_t column_family_id,
                            const Slice& key) override {
      ColumnFamilyHandle* cfh =
          impl_->db_impl_->GetColumnFamilyHandleUnlocked(column_family_id);

      impl_->delete_keys_q_.enqueue({cfh, key.ToString(), sequence_});
      sequence_++;
      return Status::OK();
    }

   private:
    BlobDBImpl* impl_;
    SequenceNumber sequence_;
  };

  // add deleted key to list of keys that have been deleted for book-keeping
  DeleteBookkeeper delete_bookkeeper(this, sequence);
  updates->Iterate(&delete_bookkeeper);

  return Status::OK();
}

Status BlobDBImpl::PutWithTTL(const WriteOptions& options,
                              const Slice& key, const Slice& value,
                              uint64_t ttl) {
  uint64_t now = EpochNow();
  assert(std::numeric_limits<uint64_t>::max() - now > ttl);
  return PutUntil(options, key, value, now + ttl);
}

Slice BlobDBImpl::GetCompressedSlice(const Slice& raw,
                                     std::string* compression_output) const {
  if (bdb_options_.compression == kNoCompression) {
    return raw;
  }
  CompressionType ct = bdb_options_.compression;
  CompressionOptions compression_opts;
  CompressBlock(raw, compression_opts, &ct, kBlockBasedTableVersionFormat,
                Slice(), compression_output);
  return *compression_output;
}

Status BlobDBImpl::PutUntil(const WriteOptions& options, const Slice& key,
                            const Slice& value_unc, uint64_t expiration) {
  TEST_SYNC_POINT("BlobDBImpl::PutUntil:Start");
  MutexLock l(&write_mutex_);
  UpdateWriteOptions(options);

  std::shared_ptr<BlobFile> bfile = (expiration != kNoExpiration)
                                        ? SelectBlobFileTTL(expiration)
                                        : SelectBlobFile();

  if (!bfile) return Status::NotFound("Blob file not found");

  std::string compression_output;
  Slice value = GetCompressedSlice(value_unc, &compression_output);

  std::string headerbuf;
  Writer::ConstructBlobHeader(&headerbuf, key, value, expiration, -1);

  // this is another more safer way to do it, where you keep the writeLock
  // for the entire write path. this will increase latency and reduce
  // throughput
  // WriteLock lockbfile_w(&bfile->mutex_);
  // std::shared_ptr<Writer> writer =
  // CheckOrCreateWriterLocked(bfile);

  if (debug_level_ >= 3)
    ROCKS_LOG_DEBUG(
        db_options_.info_log, ">Adding KEY FILE: %s: KEY: %s VALSZ: %d",
        bfile->PathName().c_str(), key.ToString().c_str(), value.size());

  std::string index_entry;
  Status s = AppendBlob(bfile, headerbuf, key, value, &index_entry);
  if (!s.ok()) {
    ROCKS_LOG_ERROR(db_options_.info_log,
                    "Failed to append blob to FILE: %s: KEY: %s VALSZ: %d"
                    " status: '%s' blob_file: '%s'",
                    bfile->PathName().c_str(), key.ToString().c_str(),
                    value.size(), s.ToString().c_str(),
                    bfile->DumpState().c_str());
    return s;
  }

  WriteBatch batch;
  batch.Put(key, index_entry);

  // this goes to the base db and can be expensive
  s = db_->Write(options, &batch);

  // this is the sequence number of the write.
  SequenceNumber sn = WriteBatchInternal::Sequence(&batch);

  if (debug_level_ >= 3)
    ROCKS_LOG_INFO(db_options_.info_log, "<Adding KEY FILE: %s: KEY: %s SN: %d",
                   bfile->PathName().c_str(), key.ToString().c_str(), sn);

  s = AppendSN(bfile, sn);
  if (!s.ok()) {
    ROCKS_LOG_ERROR(db_options_.info_log,
                    "Failed to append SN to FILE: %s: KEY: %s VALSZ: %d"
                    " status: '%s' blob_file: '%s'",
                    bfile->PathName().c_str(), key.ToString().c_str(),
                    value.size(), s.ToString().c_str(),
                    bfile->DumpState().c_str());
  }

  if (expiration != kNoExpiration) {
    extendTTL(&(bfile->ttl_range_), expiration);
  }

  if (s.ok()) {
    s = CloseBlobFileIfNeeded(bfile);
  }

  TEST_SYNC_POINT("BlobDBImpl::PutUntil:Finish");
  return s;
}

uint64_t BlobDBImpl::ExtractExpiration(const Slice& key, const Slice& value,
                                       Slice* value_slice,
                                       std::string* new_value) {
  uint64_t expiration = kNoExpiration;
  bool has_expiration = false;
  bool value_changed = false;
  if (ttl_extractor_ != nullptr) {
    has_expiration = ttl_extractor_->ExtractExpiration(
        key, value, EpochNow(), &expiration, new_value, &value_changed);
  }
  *value_slice = value_changed ? Slice(*new_value) : value;
  return has_expiration ? expiration : kNoExpiration;
}

Status BlobDBImpl::AppendBlob(const std::shared_ptr<BlobFile>& bfile,
                              const std::string& headerbuf, const Slice& key,
                              const Slice& value, std::string* index_entry) {
  Status s;

  uint64_t blob_offset = 0;
  uint64_t key_offset = 0;
  {
    WriteLock lockbfile_w(&bfile->mutex_);
    std::shared_ptr<Writer> writer = CheckOrCreateWriterLocked(bfile);
    if (!writer) return Status::IOError("Failed to create blob writer");

    // write the blob to the blob log.
    s = writer->EmitPhysicalRecord(headerbuf, key, value, &key_offset,
                                   &blob_offset);
  }

  if (!s.ok()) {
    ROCKS_LOG_ERROR(db_options_.info_log,
                    "Invalid status in AppendBlob: %s status: '%s'",
                    bfile->PathName().c_str(), s.ToString().c_str());
    return s;
  }

  // increment blob count
  bfile->blob_count_++;
  auto size_put = BlobLogRecord::kHeaderSize + key.size() + value.size();

  bfile->file_size_ += size_put;
  last_period_write_ += size_put;
  total_blob_space_ += size_put;

  BlobHandle handle;
  handle.set_filenumber(bfile->BlobFileNumber());
  handle.set_size(value.size());
  handle.set_offset(blob_offset);
  handle.set_compression(bdb_options_.compression);
  handle.EncodeTo(index_entry);

  if (debug_level_ >= 3)
    ROCKS_LOG_INFO(db_options_.info_log,
                   ">Adding KEY FILE: %s: BC: %d OFFSET: %d SZ: %d",
                   bfile->PathName().c_str(), bfile->blob_count_.load(),
                   blob_offset, value.size());

  return s;
}

Status BlobDBImpl::AppendSN(const std::shared_ptr<BlobFile>& bfile,
                            const SequenceNumber& sn) {
  Status s;
  {
    WriteLock lockbfile_w(&bfile->mutex_);
    std::shared_ptr<Writer> writer = CheckOrCreateWriterLocked(bfile);
    if (!writer) return Status::IOError("Failed to create blob writer");

    s = writer->AddRecordFooter(sn);
    if (!s.ok()) {
      ROCKS_LOG_ERROR(db_options_.info_log,
                      "Invalid status in AppendSN: %s status: '%s'",
                      bfile->PathName().c_str(), s.ToString().c_str());
      return s;
    }

    if (sn != std::numeric_limits<SequenceNumber>::max())
      extendSN(&(bfile->sn_range_), sn);
  }

  bfile->file_size_ += BlobLogRecord::kFooterSize;
  last_period_write_ += BlobLogRecord::kFooterSize;
  total_blob_space_ += BlobLogRecord::kFooterSize;
  return s;
}

std::vector<Status> BlobDBImpl::MultiGet(
    const ReadOptions& read_options,
    const std::vector<Slice>& keys, std::vector<std::string>* values) {
  // Get a snapshot to avoid blob file get deleted between we
  // fetch and index entry and reading from the file.
  ReadOptions ro(read_options);
  bool snapshot_created = SetSnapshotIfNeeded(&ro);
  std::vector<std::string> values_lsm;
  values_lsm.resize(keys.size());
  auto statuses = db_->MultiGet(ro, keys, &values_lsm);
  TEST_SYNC_POINT("BlobDBImpl::MultiGet:AfterIndexEntryGet:1");
  TEST_SYNC_POINT("BlobDBImpl::MultiGet:AfterIndexEntryGet:2");

  values->resize(keys.size());
  assert(statuses.size() == keys.size());
  assert(values_lsm.size() == keys.size());
  for (size_t i = 0; i < keys.size(); ++i) {
    if (!statuses[i].ok()) {
      continue;
    }
    Status s = CommonGet(keys[i], values_lsm[i], &((*values)[i]));
    statuses[i] = s;
  }
  if (snapshot_created) {
    db_->ReleaseSnapshot(ro.snapshot);
  }
  return statuses;
}

bool BlobDBImpl::SetSnapshotIfNeeded(ReadOptions* read_options) {
  assert(read_options != nullptr);
  if (read_options->snapshot != nullptr) {
    return false;
  }
  read_options->snapshot = db_->GetSnapshot();
  return true;
}

Status BlobDBImpl::CommonGet(const Slice& key, const std::string& index_entry,
                             std::string* value, SequenceNumber* sequence) {
  Slice index_entry_slice(index_entry);
  BlobHandle handle;
  Status s = handle.DecodeFrom(&index_entry_slice);
  if (!s.ok()) return s;

  // offset has to have certain min, as we will read CRC
  // later from the Blob Header, which needs to be also a
  // valid offset.
  if (handle.offset() <
      (BlobLogHeader::kHeaderSize + BlobLogRecord::kHeaderSize + key.size())) {
    if (debug_level_ >= 2) {
      ROCKS_LOG_ERROR(
          db_options_.info_log,
          "Invalid blob handle file_number: %" PRIu64 " blob_offset: %" PRIu64
          " blob_size: %" PRIu64 " key: %s",
          handle.filenumber(), handle.offset(), handle.size(), key.data());
    }
    return Status::NotFound("Blob Not Found, although found in LSM");
  }

  std::shared_ptr<BlobFile> bfile;
  {
    ReadLock rl(&mutex_);
    auto hitr = blob_files_.find(handle.filenumber());

    // file was deleted
    if (hitr == blob_files_.end()) {
      return Status::NotFound("Blob Not Found as blob file missing");
    }

    bfile = hitr->second;
  }

  // 0 - size
  if (!handle.size() && value != nullptr) {
    value->clear();
    return Status::OK();
  }

  // takes locks when called
  std::shared_ptr<RandomAccessFileReader> reader =
      GetOrOpenRandomAccessReader(bfile, env_, env_options_);

  if (value != nullptr) {
    std::string* valueptr = value;
    std::string value_c;
    if (bdb_options_.compression != kNoCompression) {
      valueptr = &value_c;
    }

    // allocate the buffer. This is safe in C++11
    valueptr->resize(handle.size());
    char* buffer = &(*valueptr)[0];

    Slice blob_value;
    s = reader->Read(handle.offset(), handle.size(), &blob_value, buffer);
    if (!s.ok() || blob_value.size() != handle.size()) {
      if (debug_level_ >= 2) {
        ROCKS_LOG_ERROR(
            db_options_.info_log,
            "Failed to read blob from file: %s blob_offset: %" PRIu64
            " blob_size: %" PRIu64 " read: %d key: %s status: '%s'",
            bfile->PathName().c_str(), handle.offset(), handle.size(),
            static_cast<int>(blob_value.size()), key.data(),
            s.ToString().c_str());
      }
      return Status::NotFound("Blob Not Found as couldnt retrieve Blob");
    }

    Slice crc_slice;
    uint32_t crc_exp;
    std::string crc_str;
    crc_str.resize(sizeof(uint32_t));
    char* crc_buffer = &(crc_str[0]);
    s = reader->Read(handle.offset() - (key.size() + sizeof(uint32_t)),
                     sizeof(uint32_t), &crc_slice, crc_buffer);
    if (!s.ok() || !GetFixed32(&crc_slice, &crc_exp)) {
      if (debug_level_ >= 2) {
        ROCKS_LOG_ERROR(
            db_options_.info_log,
            "Failed to fetch blob crc file: %s blob_offset: %" PRIu64
            " blob_size: %" PRIu64 " key: %s status: '%s'",
            bfile->PathName().c_str(), handle.offset(), handle.size(),
            key.data(), s.ToString().c_str());
      }
      return Status::NotFound("Blob Not Found as couldnt retrieve CRC");
    }

    uint32_t crc = crc32c::Extend(0, blob_value.data(), blob_value.size());
    crc = crc32c::Mask(crc);  // Adjust for storage
    if (crc != crc_exp) {
      if (debug_level_ >= 2) {
        ROCKS_LOG_ERROR(db_options_.info_log,
                        "Blob crc mismatch file: %s blob_offset: %" PRIu64
                        " blob_size: %" PRIu64 " key: %s status: '%s'",
                        bfile->PathName().c_str(), handle.offset(),
                        handle.size(), key.data(), s.ToString().c_str());
      }
      return Status::Corruption("Corruption. Blob CRC mismatch");
    }

    if (bdb_options_.compression != kNoCompression) {
      BlockContents contents;
      auto cfh =
          reinterpret_cast<ColumnFamilyHandleImpl*>(DefaultColumnFamily());
      s = UncompressBlockContentsForCompressionType(
          blob_value.data(), blob_value.size(), &contents,
          kBlockBasedTableVersionFormat, Slice(), bdb_options_.compression,
          *(cfh->cfd()->ioptions()));
      *value = contents.data.ToString();
    }
  }

  if (sequence != nullptr) {
    char buffer[BlobLogRecord::kFooterSize];
    Slice footer_slice;
    s = reader->Read(handle.offset() + handle.size(),
                     BlobLogRecord::kFooterSize, &footer_slice, buffer);
    if (!s.ok()) {
      return s;
    }
    BlobLogRecord record;
    s = record.DecodeFooterFrom(footer_slice);
    if (!s.ok()) {
      return s;
    }
    *sequence = record.GetSN();
  }

  return s;
}

Status BlobDBImpl::Get(const ReadOptions& read_options,
                       ColumnFamilyHandle* column_family, const Slice& key,
                       PinnableSlice* value) {
  if (column_family != DefaultColumnFamily()) {
    return Status::NotSupported(
        "Blob DB doesn't support non-default column family.");
  }
  // Get a snapshot to avoid blob file get deleted between we
  // fetch and index entry and reading from the file.
  // TODO(yiwu): For Get() retry if file not found would be a simpler strategy.
  ReadOptions ro(read_options);
  bool snapshot_created = SetSnapshotIfNeeded(&ro);

  Status s;
  std::string index_entry;
  s = db_->Get(ro, key, &index_entry);
  TEST_SYNC_POINT("BlobDBImpl::Get:AfterIndexEntryGet:1");
  TEST_SYNC_POINT("BlobDBImpl::Get:AfterIndexEntryGet:2");
  if (s.ok()) {
    s = CommonGet(key, index_entry, value->GetSelf());
    value->PinSelf();
  }
  if (snapshot_created) {
    db_->ReleaseSnapshot(ro.snapshot);
  }
  return s;
}

Slice BlobDBIterator::value() const {
  TEST_SYNC_POINT("BlobDBIterator::value:BeforeGetBlob:1");
  TEST_SYNC_POINT("BlobDBIterator::value:BeforeGetBlob:2");
  Slice index_entry = iter_->value();
  Status s =
      db_impl_->CommonGet(iter_->key(), index_entry.ToString(false), &vpart_);
  return Slice(vpart_);
}

std::pair<bool, int64_t> BlobDBImpl::SanityCheck(bool aborted) {
  if (aborted) return std::make_pair(false, -1);

  ROCKS_LOG_INFO(db_options_.info_log, "Starting Sanity Check");

  ROCKS_LOG_INFO(db_options_.info_log, "Number of files %" PRIu64,
                 blob_files_.size());

  ROCKS_LOG_INFO(db_options_.info_log, "Number of open files %" PRIu64,
                 open_blob_files_.size());

  for (auto bfile : open_blob_files_) {
    assert(!bfile->Immutable());
  }

  uint64_t epoch_now = EpochNow();

  for (auto bfile_pair : blob_files_) {
    auto bfile = bfile_pair.second;
    ROCKS_LOG_INFO(
        db_options_.info_log,
        "Blob File %s %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64,
        bfile->PathName().c_str(), bfile->GetFileSize(), bfile->BlobCount(),
        bfile->deleted_count_, bfile->deleted_size_,
        (bfile->ttl_range_.second - epoch_now));
  }

  // reschedule
  return std::make_pair(true, -1);
}

Status BlobDBImpl::CloseBlobFile(std::shared_ptr<BlobFile> bfile) {
  Status s;
  ROCKS_LOG_INFO(db_options_.info_log, "Close blob file %" PRIu64,
                 bfile->BlobFileNumber());
  {
    WriteLock wl(&mutex_);

    if (bfile->HasTTL()) {
      size_t erased __attribute__((__unused__)) = open_blob_files_.erase(bfile);
      assert(erased == 1);
    } else {
      auto iter = std::find(open_simple_files_.begin(),
                            open_simple_files_.end(), bfile);
      assert(iter != open_simple_files_.end());
      open_simple_files_.erase(iter);
    }
  }

  if (!bfile->closed_.load()) {
    WriteLock lockbfile_w(&bfile->mutex_);
    s = bfile->WriteFooterAndCloseLocked();
  }

  if (!s.ok()) {
    ROCKS_LOG_ERROR(db_options_.info_log,
                    "Failed to close blob file %" PRIu64 "with error: %s",
                    bfile->BlobFileNumber(), s.ToString().c_str());
  }

  return s;
}

Status BlobDBImpl::CloseBlobFileIfNeeded(std::shared_ptr<BlobFile>& bfile) {
  // atomic read
  if (bfile->GetFileSize() < bdb_options_.blob_file_size) {
    return Status::OK();
  }
  return CloseBlobFile(bfile);
}

bool BlobDBImpl::FileDeleteOk_SnapshotCheckLocked(
    const std::shared_ptr<BlobFile>& bfile) {
  assert(bfile->Obsolete());

  SequenceNumber esn = bfile->GetSNRange().first;

  // this is not correct.
  // you want to check that there are no snapshots in the
  bool notok = db_impl_->HasActiveSnapshotLaterThanSN(esn);
  if (notok) {
    ROCKS_LOG_INFO(db_options_.info_log,
                   "Could not delete file due to snapshot failure %s",
                   bfile->PathName().c_str());
    return false;
  } else {
    ROCKS_LOG_INFO(db_options_.info_log,
                   "Will delete file due to snapshot success %s",
                   bfile->PathName().c_str());
    return true;
  }
}

bool BlobDBImpl::FindFileAndEvictABlob(uint64_t file_number, uint64_t key_size,
                                       uint64_t blob_offset,
                                       uint64_t blob_size) {
  (void)blob_offset;
  std::shared_ptr<BlobFile> bfile;
  {
    ReadLock rl(&mutex_);
    auto hitr = blob_files_.find(file_number);

    // file was deleted
    if (hitr == blob_files_.end()) {
      return false;
    }

    bfile = hitr->second;
  }

  WriteLock lockbfile_w(&bfile->mutex_);

  bfile->deleted_count_++;
  bfile->deleted_size_ += key_size + blob_size + BlobLogRecord::kHeaderSize +
                          BlobLogRecord::kFooterSize;
  return true;
}

bool BlobDBImpl::MarkBlobDeleted(const Slice& key, const Slice& lsmValue) {
  Slice val(lsmValue);
  BlobHandle handle;
  Status s = handle.DecodeFrom(&val);
  if (!s.ok()) {
    ROCKS_LOG_INFO(db_options_.info_log,
                   "Could not parse lsm val in MarkBlobDeleted %s",
                   lsmValue.ToString().c_str());
    return false;
  }
  bool succ = FindFileAndEvictABlob(handle.filenumber(), key.size(),
                                    handle.offset(), handle.size());
  return succ;
}

std::pair<bool, int64_t> BlobDBImpl::EvictCompacted(bool aborted) {
  if (aborted) return std::make_pair(false, -1);

  override_packet_t packet;
  size_t total_vals = 0;
  size_t mark_evicted = 0;
  while (override_vals_q_.dequeue(&packet)) {
    bool succeeded =
        FindFileAndEvictABlob(packet.file_number_, packet.key_size_,
                              packet.blob_offset_, packet.blob_size_);
    total_vals++;
    if (succeeded) {
      mark_evicted++;
    }
  }
  ROCKS_LOG_INFO(db_options_.info_log,
                 "Mark %" ROCKSDB_PRIszt
                 " values to evict, out of %" ROCKSDB_PRIszt
                 " compacted values.",
                 mark_evicted, total_vals);
  return std::make_pair(true, -1);
}

std::pair<bool, int64_t> BlobDBImpl::EvictDeletions(bool aborted) {
  if (aborted) return std::make_pair(false, -1);

  ColumnFamilyHandle* last_cfh = nullptr;
  Options last_op;

  Arena arena;
  ScopedArenaIterator iter;

  // we will use same RangeDelAggregator for all cf's.
  // essentially we do not support Range Deletes now
  std::unique_ptr<RangeDelAggregator> range_del_agg;
  delete_packet_t dpacket;
  while (delete_keys_q_.dequeue(&dpacket)) {
    if (last_cfh != dpacket.cfh_) {
      if (!range_del_agg) {
        auto cfhi = reinterpret_cast<ColumnFamilyHandleImpl*>(dpacket.cfh_);
        auto cfd = cfhi->cfd();
        range_del_agg.reset(new RangeDelAggregator(cfd->internal_comparator(),
                                                   kMaxSequenceNumber));
      }

      // this can be expensive
      last_cfh = dpacket.cfh_;
      last_op = db_impl_->GetOptions(last_cfh);
      iter.set(db_impl_->NewInternalIterator(&arena, range_del_agg.get(),
                                             dpacket.cfh_));
      // this will not work for multiple CF's.
    }

    Slice user_key(dpacket.key_);
    InternalKey target(user_key, dpacket.dsn_, kTypeValue);

    Slice eslice = target.Encode();
    iter->Seek(eslice);

    if (!iter->status().ok()) {
      ROCKS_LOG_INFO(db_options_.info_log, "Invalid iterator seek %s",
                     dpacket.key_.c_str());
      continue;
    }

    const Comparator* bwc = BytewiseComparator();
    while (iter->Valid()) {
      if (!bwc->Equal(ExtractUserKey(iter->key()), ExtractUserKey(eslice)))
        break;

      ParsedInternalKey ikey(Slice(), 0, kTypeValue);
      if (!ParseInternalKey(iter->key(), &ikey)) {
        continue;
      }

      // once you hit a DELETE, assume the keys below have been
      // processed previously
      if (ikey.type == kTypeDeletion || ikey.type == kTypeSingleDeletion) break;

      Slice val = iter->value();
      MarkBlobDeleted(ikey.user_key, val);

      iter->Next();
    }
  }
  return std::make_pair(true, -1);
}

std::pair<bool, int64_t> BlobDBImpl::CheckSeqFiles(bool aborted) {
  if (aborted) return std::make_pair(false, -1);

  std::vector<std::shared_ptr<BlobFile>> process_files;
  {
    uint64_t epoch_now = EpochNow();

    ReadLock rl(&mutex_);
    for (auto bfile : open_blob_files_) {
      {
        ReadLock lockbfile_r(&bfile->mutex_);

        if (bfile->ttl_range_.second > epoch_now) continue;
        process_files.push_back(bfile);
      }
    }
  }

  for (auto bfile : process_files) {
    CloseBlobFile(bfile);
  }

  return std::make_pair(true, -1);
}

std::pair<bool, int64_t> BlobDBImpl::FsyncFiles(bool aborted) {
  if (aborted) return std::make_pair(false, -1);

  MutexLock l(&write_mutex_);

  std::vector<std::shared_ptr<BlobFile>> process_files;
  {
    ReadLock rl(&mutex_);
    for (auto fitr : open_blob_files_) {
      if (fitr->NeedsFsync(true, bdb_options_.bytes_per_sync))
        process_files.push_back(fitr);
    }

    for (auto fitr : open_simple_files_) {
      if (fitr->NeedsFsync(true, bdb_options_.bytes_per_sync))
        process_files.push_back(fitr);
    }
  }

  for (auto fitr : process_files) {
    if (fitr->NeedsFsync(true, bdb_options_.bytes_per_sync)) fitr->Fsync();
  }

  bool expected = true;
  if (dir_change_.compare_exchange_weak(expected, false)) dir_ent_->Fsync();

  return std::make_pair(true, -1);
}

std::pair<bool, int64_t> BlobDBImpl::ReclaimOpenFiles(bool aborted) {
  if (aborted) return std::make_pair(false, -1);

  if (open_file_count_.load() < kOpenFilesTrigger) {
    return std::make_pair(true, -1);
  }

  // in the future, we should sort by last_access_
  // instead of closing every file
  ReadLock rl(&mutex_);
  for (auto const& ent : blob_files_) {
    auto bfile = ent.second;
    if (bfile->last_access_.load() == -1) continue;

    WriteLock lockbfile_w(&bfile->mutex_);
    CloseRandomAccessLocked(bfile);
  }

  return std::make_pair(true, -1);
}

// TODO(yiwu): correct the stats and expose it.
std::pair<bool, int64_t> BlobDBImpl::WaStats(bool aborted) {
  if (aborted) return std::make_pair(false, -1);

  WriteLock wl(&mutex_);

  if (all_periods_write_.size() >= kWriteAmplificationStatsPeriods) {
    total_periods_write_ -= (*all_periods_write_.begin());
    total_periods_ampl_ = (*all_periods_ampl_.begin());

    all_periods_write_.pop_front();
    all_periods_ampl_.pop_front();
  }

  uint64_t val1 = last_period_write_.load();
  uint64_t val2 = last_period_ampl_.load();

  all_periods_write_.push_back(val1);
  all_periods_ampl_.push_back(val2);

  last_period_write_ = 0;
  last_period_ampl_ = 0;

  total_periods_write_ += val1;
  total_periods_ampl_ += val2;

  return std::make_pair(true, -1);
}

////////////////////////////////////////////////////////////////////////////////
// iterate over the blobs sequentially and check if the blob sequence number
// is the latest. If it is the latest, preserve it, otherwise delete it
// if it is TTL based, and the TTL has expired, then
// we can blow the entity if the key is still the latest or the Key is not
// found
// WHAT HAPPENS IF THE KEY HAS BEEN OVERRIDEN. Then we can drop the blob
// without doing anything if the earliest snapshot is not
// referring to that sequence number, i.e. it is later than the sequence number
// of the new key
//
// if it is not TTL based, then we can blow the key if the key has been
// DELETED in the LSM
////////////////////////////////////////////////////////////////////////////////
Status BlobDBImpl::GCFileAndUpdateLSM(const std::shared_ptr<BlobFile>& bfptr,
                                      GCStats* gc_stats) {
  uint64_t now = EpochNow();

  std::shared_ptr<Reader> reader =
      bfptr->OpenSequentialReader(env_, db_options_, env_options_);
  if (!reader) {
    ROCKS_LOG_ERROR(db_options_.info_log,
                    "File sequential reader could not be opened",
                    bfptr->PathName().c_str());
    return Status::IOError("failed to create sequential reader");
  }

  BlobLogHeader header;
  Status s = reader->ReadHeader(&header);
  if (!s.ok()) {
    ROCKS_LOG_ERROR(db_options_.info_log,
                    "Failure to read header for blob-file %s",
                    bfptr->PathName().c_str());
    return s;
  }

  bool first_gc = bfptr->gc_once_after_open_;

  ColumnFamilyHandle* cfh = bfptr->GetColumnFamily(db_);
  bool has_ttl = header.HasTTL();

  // this reads the key but skips the blob
  Reader::ReadLevel shallow = Reader::kReadHdrKeyFooter;

  assert(opt_db_);

  bool no_relocation_ttl = (has_ttl && now >= bfptr->GetTTLRange().second);

  bool no_relocation_lsmdel = false;
  {
    ReadLock lockbfile_r(&bfptr->mutex_);
    no_relocation_lsmdel = (bfptr->GetFileSize() ==
                            (BlobLogHeader::kHeaderSize + bfptr->deleted_size_ +
                             BlobLogFooter::kFooterSize));
  }

  bool no_relocation = no_relocation_ttl || no_relocation_lsmdel;
  if (!no_relocation) {
    // read the blob because you have to write it back to new file
    shallow = Reader::kReadHdrKeyBlobFooter;
  }

  BlobLogRecord record;
  std::shared_ptr<BlobFile> newfile;
  std::shared_ptr<Writer> new_writer;
  Transaction* transaction = nullptr;
  uint64_t blob_offset = 0;
  bool retry = false;

  static const WriteOptions kGarbageCollectionWriteOptions = []() {
    WriteOptions write_options;
    // TODO(yiwu): Disable WAL for garbage colection to make it compatible with
    // use cases that don't use WAL. However without WAL there are at least
    // two issues with crash:
    // 1. If a key is dropped from blob file (e.g. due to TTL), right before a
    //    crash, the key may still presents in LSM after restart.
    // 2. If a key is relocated to another blob file, right before a crash,
    //    after restart the new offset may be lost with the old offset pointing
    //    to the removed blob file.
    // We need to have better recovery mechanism to address these issues.
    write_options.disableWAL = true;
    // It is ok to ignore column families that were dropped.
    write_options.ignore_missing_column_families = true;
    return write_options;
  }();

  while (true) {
    assert(s.ok());
    if (retry) {
      // Retry in case transaction fail with Status::TryAgain.
      retry = false;
    } else {
      // Read the next blob record.
      Status read_record_status =
          reader->ReadRecord(&record, shallow, &blob_offset);
      // Exit if we reach the end of blob file.
      // TODO(yiwu): properly handle ReadRecord error.
      if (!read_record_status.ok()) {
        break;
      }
      gc_stats->blob_count++;
    }

    transaction =
        opt_db_->BeginTransaction(kGarbageCollectionWriteOptions,
                                  OptimisticTransactionOptions(), transaction);

    std::string index_entry;
    Status get_status = transaction->GetForUpdate(ReadOptions(), cfh,
                                                  record.Key(), &index_entry);
    TEST_SYNC_POINT("BlobDBImpl::GCFileAndUpdateLSM:AfterGetForUpdate");
    if (get_status.IsNotFound()) {
      // Key has been deleted. Drop the blob record.
      continue;
    }
    if (!get_status.ok()) {
      s = get_status;
      ROCKS_LOG_ERROR(db_options_.info_log,
                      "Error while getting index entry: %s",
                      s.ToString().c_str());
      break;
    }

    // TODO(yiwu): We should have an override of GetForUpdate returning a
    // PinnableSlice.
    Slice index_entry_slice(index_entry);
    BlobHandle handle;
    s = handle.DecodeFrom(&index_entry_slice);
    if (!s.ok()) {
      ROCKS_LOG_ERROR(db_options_.info_log,
                      "Error while decoding index entry: %s",
                      s.ToString().c_str());
      break;
    }
    if (handle.filenumber() != bfptr->BlobFileNumber() ||
        handle.offset() != blob_offset) {
      // Key has been overwritten. Drop the blob record.
      continue;
    }

    // If key has expired, remove it from base DB.
    if (no_relocation_ttl || (has_ttl && now >= record.GetTTL())) {
      gc_stats->num_deletes++;
      gc_stats->deleted_size += record.GetBlobSize();
      TEST_SYNC_POINT("BlobDBImpl::GCFileAndUpdateLSM:BeforeDelete");
      transaction->Delete(cfh, record.Key());
      Status delete_status = transaction->Commit();
      if (delete_status.ok()) {
        gc_stats->delete_succeeded++;
      } else if (delete_status.IsBusy()) {
        // The key is overwritten in the meanwhile. Drop the blob record.
        gc_stats->overwritten_while_delete++;
      } else if (delete_status.IsTryAgain()) {
        // Retry the transaction.
        retry = true;
      } else {
        // We hit an error.
        s = delete_status;
        ROCKS_LOG_ERROR(db_options_.info_log,
                        "Error while deleting expired key: %s",
                        s.ToString().c_str());
        break;
      }
      // Continue to next blob record or retry.
      continue;
    }

    if (first_gc) {
      // Do not relocate blob record for initial GC.
      continue;
    }

    // Relocate the blob record to new file.
    if (!newfile) {
      // new file
      std::string reason("GC of ");
      reason += bfptr->PathName();
      newfile = NewBlobFile(reason);
      gc_stats->newfile = newfile;

      new_writer = CheckOrCreateWriterLocked(newfile);
      newfile->header_ = std::move(header);
      // Can't use header beyond this point
      newfile->header_valid_ = true;
      newfile->file_size_ = BlobLogHeader::kHeaderSize;
      s = new_writer->WriteHeader(newfile->header_);

      if (!s.ok()) {
        ROCKS_LOG_ERROR(db_options_.info_log,
                        "File: %s - header writing failed",
                        newfile->PathName().c_str());
        break;
      }

      WriteLock wl(&mutex_);

      dir_change_.store(true);
      blob_files_.insert(std::make_pair(newfile->BlobFileNumber(), newfile));
    }

    gc_stats->num_relocate++;
    std::string new_index_entry;

    uint64_t new_blob_offset = 0;
    uint64_t new_key_offset = 0;
    // write the blob to the blob log.
    s = new_writer->AddRecord(record.Key(), record.Blob(), &new_key_offset,
                              &new_blob_offset, record.GetTTL());

    BlobHandle new_handle;
    new_handle.set_filenumber(newfile->BlobFileNumber());
    new_handle.set_size(record.Blob().size());
    new_handle.set_offset(new_blob_offset);
    new_handle.set_compression(bdb_options_.compression);
    new_handle.EncodeTo(&new_index_entry);

    new_writer->AddRecordFooter(record.GetSN());
    newfile->blob_count_++;
    newfile->file_size_ += BlobLogRecord::kHeaderSize + record.Key().size() +
                           record.Blob().size() + BlobLogRecord::kFooterSize;

    TEST_SYNC_POINT("BlobDBImpl::GCFileAndUpdateLSM:BeforeRelocate");
    transaction->Put(cfh, record.Key(), new_index_entry);
    Status put_status = transaction->Commit();
    if (put_status.ok()) {
      gc_stats->relocate_succeeded++;
    } else if (put_status.IsBusy()) {
      // The key is overwritten in the meanwhile. Drop the blob record.
      gc_stats->overwritten_while_relocate++;
    } else if (put_status.IsTryAgain()) {
      // Retry the transaction.
      // TODO(yiwu): On retry, we can reuse the new blob record.
      retry = true;
    } else {
      // We hit an error.
      s = put_status;
      ROCKS_LOG_ERROR(db_options_.info_log, "Error while relocating key: %s",
                      s.ToString().c_str());
      break;
    }
  }  // end of ReadRecord loop

  if (transaction != nullptr) {
    delete transaction;
  }
  ROCKS_LOG_INFO(
      db_options_.info_log,
      "%s blob file %" PRIu64
      ". Total blob records: %" PRIu64 ", Deletes: %" PRIu64 "/%" PRIu64
      " succeeded, Relocates: %" PRIu64 "/%" PRIu64 " succeeded.",
      s.ok() ? "Successfully garbage collected" : "Failed to garbage collect",
      bfptr->BlobFileNumber(), gc_stats->blob_count, gc_stats->delete_succeeded,
      gc_stats->num_deletes, gc_stats->relocate_succeeded,
      gc_stats->num_relocate);
  if (newfile != nullptr) {
    total_blob_space_ += newfile->file_size_;
    ROCKS_LOG_INFO(db_options_.info_log, "New blob file %" PRIu64 ".",
                   newfile->BlobFileNumber());
  }
  return s;
}

// Ideally we should hold the lock during the entire function,
// but under the asusmption that this is only called when a
// file is Immutable, we can reduce the critical section
bool BlobDBImpl::ShouldGCFile(std::shared_ptr<BlobFile> bfile, uint64_t now,
                              bool is_oldest_simple_blob_file,
                              std::string* reason) {
  if (bfile->HasTTL()) {
    ttlrange_t ttl_range = bfile->GetTTLRange();
    if (now > ttl_range.second) {
      *reason = "entire file ttl expired";
      return true;
    }

    if (!bfile->file_size_.load()) {
      ROCKS_LOG_ERROR(db_options_.info_log, "Invalid file size = 0 %s",
                      bfile->PathName().c_str());
      *reason = "file is empty";
      return false;
    }

    if (bfile->gc_once_after_open_.load()) {
      return true;
    }

    if (bdb_options_.ttl_range_secs < kPartialExpirationGCRangeSecs) {
      *reason = "has ttl but partial expiration not turned on";
      return false;
    }

    ReadLock lockbfile_r(&bfile->mutex_);
    bool ret = ((bfile->deleted_size_ * 100.0 / bfile->file_size_.load()) >
                kPartialExpirationPercentage);
    if (ret) {
      *reason = "deleted blobs beyond threshold";
    } else {
      *reason = "deleted blobs below threshold";
    }
    return ret;
  }

  // when crash happens, we lose the in-memory account of deleted blobs.
  // we are therefore forced to do one GC to make sure delete accounting
  // is OK
  if (bfile->gc_once_after_open_.load()) {
    return true;
  }

  ReadLock lockbfile_r(&bfile->mutex_);

  if ((bfile->deleted_size_ * 100.0 / bfile->file_size_.load()) >
      kPartialExpirationPercentage) {
    *reason = "deleted simple blobs beyond threshold";
    return true;
  }

  // if we haven't reached limits of disk space, don't DELETE
  if (bdb_options_.blob_dir_size == 0 ||
      total_blob_space_.load() < bdb_options_.blob_dir_size) {
    *reason = "disk space not exceeded";
    return false;
  }

  if (is_oldest_simple_blob_file) {
    *reason = "out of space and is the oldest simple blob file";
    return true;
  }
  *reason = "out of space but is not the oldest simple blob file";
  return false;
}

std::pair<bool, int64_t> BlobDBImpl::DeleteObsoleteFiles(bool aborted) {
  if (aborted) return std::make_pair(false, -1);

  {
    ReadLock rl(&mutex_);
    if (obsolete_files_.empty()) return std::make_pair(true, -1);
  }

  std::list<std::shared_ptr<BlobFile>> tobsolete;
  {
    WriteLock wl(&mutex_);
    tobsolete.swap(obsolete_files_);
  }

  bool file_deleted = false;
  for (auto iter = tobsolete.begin(); iter != tobsolete.end();) {
    auto bfile = *iter;
    {
      ReadLock lockbfile_r(&bfile->mutex_);
      if (!FileDeleteOk_SnapshotCheckLocked(bfile)) {
        ++iter;
        continue;
      }
    }

    blob_files_.erase(bfile->BlobFileNumber());
    Status s = env_->DeleteFile(bfile->PathName());
    if (!s.ok()) {
      ROCKS_LOG_ERROR(db_options_.info_log,
                      "File failed to be deleted as obsolete %s",
                      bfile->PathName().c_str());
      ++iter;
      continue;
    }

    file_deleted = true;
    total_blob_space_ -= bfile->file_size_;
    ROCKS_LOG_INFO(db_options_.info_log,
                   "File deleted as obsolete from blob dir %s",
                   bfile->PathName().c_str());

    iter = tobsolete.erase(iter);
  }

  // directory change. Fsync
  if (file_deleted) dir_ent_->Fsync();

  // put files back into obsolete if for some reason, delete failed
  if (!tobsolete.empty()) {
    WriteLock wl(&mutex_);
    for (auto bfile : tobsolete) {
      obsolete_files_.push_front(bfile);
    }
  }

  return std::make_pair(!aborted, -1);
}

bool BlobDBImpl::CallbackEvictsImpl(std::shared_ptr<BlobFile> bfile) {
  std::shared_ptr<Reader> reader =
      bfile->OpenSequentialReader(env_, db_options_, env_options_);
  if (!reader) {
    ROCKS_LOG_ERROR(
        db_options_.info_log,
        "File sequential reader could not be opened for evict callback: %s",
        bfile->PathName().c_str());
    return false;
  }

  ReadLock lockbfile_r(&bfile->mutex_);

  BlobLogHeader header;
  Status s = reader->ReadHeader(&header);
  if (!s.ok()) {
    ROCKS_LOG_ERROR(
        db_options_.info_log,
        "Failure to read header for blob-file during evict callback %s",
        bfile->PathName().c_str());
    return false;
  }

  ColumnFamilyHandle* cfh = bfile->GetColumnFamily(db_);
  BlobLogRecord record;
  Reader::ReadLevel full = Reader::kReadHdrKeyBlobFooter;
  while (reader->ReadRecord(&record, full).ok()) {
    bdb_options_.gc_evict_cb_fn(cfh, record.Key(), record.Blob());
  }

  return true;
}

std::pair<bool, int64_t> BlobDBImpl::RemoveTimerQ(TimerQueue* tq,
                                                  bool aborted) {
  WriteLock wl(&mutex_);
  for (auto itr = cb_threads_.begin(); itr != cb_threads_.end(); ++itr) {
    if ((*itr).get() != tq) continue;

    cb_threads_.erase(itr);
    break;
  }
  return std::make_pair(false, -1);
}

std::pair<bool, int64_t> BlobDBImpl::CallbackEvicts(
    TimerQueue* tq, std::shared_ptr<BlobFile> bfile, bool aborted) {
  if (aborted) return std::make_pair(false, -1);
  bool succ = CallbackEvictsImpl(bfile);
  if (succ) {
    ROCKS_LOG_DEBUG(db_options_.info_log, "Eviction callbacks completed %s",
                    bfile->PathName().c_str());
  }

  WriteLock wl(&mutex_);
  bfile->SetCanBeDeleted();
  obsolete_files_.push_front(bfile);
  if (tq) {
    // all of the callbacks have been processed
    tqueue_.add(0, std::bind(&BlobDBImpl::RemoveTimerQ, this, tq,
                             std::placeholders::_1));
  }
  return std::make_pair(false, -1);
}

void BlobDBImpl::CopyBlobFiles(
    std::vector<std::shared_ptr<BlobFile>>* bfiles_copy) {
  ReadLock rl(&mutex_);

  // take a copy
  bfiles_copy->reserve(blob_files_.size());
  for (auto const& p : blob_files_) {
    bfiles_copy->push_back(p.second);
  }
}

void BlobDBImpl::FilterSubsetOfFiles(
    const std::vector<std::shared_ptr<BlobFile>>& blob_files,
    std::vector<std::shared_ptr<BlobFile>>* to_process, uint64_t epoch,
    size_t files_to_collect) {
  // 100.0 / 15.0 = 7
  uint64_t next_epoch_increment = static_cast<uint64_t>(
      std::ceil(100 / static_cast<double>(kGCFilePercentage)));
  uint64_t now = EpochNow();

  size_t files_processed = 0;
  bool simple_blob_file_found = false;
  for (auto bfile : blob_files) {
    if (files_processed >= files_to_collect) break;
    // if this is the first time processing the file
    // i.e. gc_epoch == -1, process it.
    // else process the file if its processing epoch matches
    // the current epoch. Typically the #of epochs should be
    // around 5-10
    if (bfile->gc_epoch_ != -1 && (uint64_t)bfile->gc_epoch_ != epoch) {
      continue;
    }

    files_processed++;
    // reset the epoch
    bfile->gc_epoch_ = epoch + next_epoch_increment;

    // file has already been GC'd or is still open for append,
    // then it should not be GC'd
    if (bfile->Obsolete() || !bfile->Immutable()) continue;

    bool is_oldest_simple_blob_file = false;
    if (!simple_blob_file_found && !bfile->HasTTL()) {
      is_oldest_simple_blob_file = true;
      simple_blob_file_found = true;
    }

    std::string reason;
    bool shouldgc =
        ShouldGCFile(bfile, now, is_oldest_simple_blob_file, &reason);
    if (!shouldgc) {
      ROCKS_LOG_DEBUG(db_options_.info_log,
                      "File has been skipped for GC ttl %s %" PRIu64 " %" PRIu64
                      " reason='%s'",
                      bfile->PathName().c_str(), now,
                      bfile->GetTTLRange().second, reason.c_str());
      continue;
    }

    ROCKS_LOG_INFO(db_options_.info_log,
                   "File has been chosen for GC ttl %s %" PRIu64 " %" PRIu64
                   " reason='%s'",
                   bfile->PathName().c_str(), now, bfile->GetTTLRange().second,
                   reason.c_str());
    to_process->push_back(bfile);
  }
}

std::pair<bool, int64_t> BlobDBImpl::RunGC(bool aborted) {
  if (aborted) return std::make_pair(false, -1);

  current_epoch_++;

  std::vector<std::shared_ptr<BlobFile>> blob_files;
  CopyBlobFiles(&blob_files);

  if (!blob_files.size()) return std::make_pair(true, -1);

  // 15% of files are collected each call to space out the IO and CPU
  // consumption.
  size_t files_to_collect = (kGCFilePercentage * blob_files.size()) / 100;

  std::vector<std::shared_ptr<BlobFile>> to_process;
  FilterSubsetOfFiles(blob_files, &to_process, current_epoch_,
                      files_to_collect);

  // in this collect the set of files, which became obsolete
  std::vector<std::shared_ptr<BlobFile>> obsoletes;
  for (auto bfile : to_process) {
    GCStats gc_stats;
    Status s = GCFileAndUpdateLSM(bfile, &gc_stats);
    if (!s.ok()) {
      continue;
    }

    if (bfile->gc_once_after_open_.load()) {
      WriteLock lockbfile_w(&bfile->mutex_);

      bfile->deleted_size_ = gc_stats.deleted_size;
      bfile->deleted_count_ = gc_stats.num_deletes;
      bfile->gc_once_after_open_ = false;
    } else {
      obsoletes.push_back(bfile);
    }
  }

  if (!obsoletes.empty()) {
    bool evict_cb = (!!bdb_options_.gc_evict_cb_fn);
    std::shared_ptr<TimerQueue> tq;
    if (evict_cb) tq = std::make_shared<TimerQueue>();

    // if evict callback is present, first schedule the callback thread
    WriteLock wl(&mutex_);
    for (auto bfile : obsoletes) {
      bool last_file = (bfile == obsoletes.back());

      if (!evict_cb) {
        bfile->SetCanBeDeleted();
        obsolete_files_.push_front(bfile);
      } else {
        tq->add(0, std::bind(&BlobDBImpl::CallbackEvicts, this,
                             (last_file) ? tq.get() : nullptr, bfile,
                             std::placeholders::_1));
      }
    }
    if (evict_cb) cb_threads_.emplace_back(tq);
  }

  // reschedule
  return std::make_pair(true, -1);
}

Iterator* BlobDBImpl::NewIterator(const ReadOptions& read_options) {
  // Get a snapshot to avoid blob file get deleted between we
  // fetch and index entry and reading from the file.
  ReadOptions ro(read_options);
  bool snapshot_created = SetSnapshotIfNeeded(&ro);
  return new BlobDBIterator(db_->NewIterator(ro), this, snapshot_created,
                            ro.snapshot);
}

Status DestroyBlobDB(const std::string& dbname, const Options& options,
                     const BlobDBOptions& bdb_options) {
  const ImmutableDBOptions soptions(SanitizeOptions(dbname, options));
  Env* env = soptions.env;

  Status status;
  std::string blobdir;
  blobdir = (bdb_options.path_relative) ? dbname + "/" + bdb_options.blob_dir
                                        : bdb_options.blob_dir;

  std::vector<std::string> filenames;
  env->GetChildren(blobdir, &filenames);

  for (const auto& f : filenames) {
    uint64_t number;
    FileType type;
    if (ParseFileName(f, &number, &type) && type == kBlobFile) {
      Status del = env->DeleteFile(blobdir + "/" + f);
      if (status.ok() && !del.ok()) {
        status = del;
      }
    }
  }
  env->DeleteDir(blobdir);

  Status destroy = DestroyDB(dbname, options);
  if (status.ok() && !destroy.ok()) {
    status = destroy;
  }

  return status;
}

#ifndef NDEBUG
Status BlobDBImpl::TEST_GetSequenceNumber(const Slice& key,
                                          SequenceNumber* sequence) {
  std::string index_entry;
  Status s = db_->Get(ReadOptions(), key, &index_entry);
  if (!s.ok()) {
    return s;
  }
  return CommonGet(key, index_entry, nullptr, sequence);
}

std::vector<std::shared_ptr<BlobFile>> BlobDBImpl::TEST_GetBlobFiles() const {
  ReadLock l(&mutex_);
  std::vector<std::shared_ptr<BlobFile>> blob_files;
  for (auto& p : blob_files_) {
    blob_files.emplace_back(p.second);
  }
  return blob_files;
}

std::vector<std::shared_ptr<BlobFile>> BlobDBImpl::TEST_GetObsoleteFiles()
    const {
  ReadLock l(&mutex_);
  std::vector<std::shared_ptr<BlobFile>> obsolete_files;
  for (auto& bfile : obsolete_files_) {
    obsolete_files.emplace_back(bfile);
  }
  return obsolete_files;
}

void BlobDBImpl::TEST_DeleteObsoleteFiles() {
  DeleteObsoleteFiles(false /*abort*/);
}

Status BlobDBImpl::TEST_CloseBlobFile(std::shared_ptr<BlobFile>& bfile) {
  return CloseBlobFile(bfile);
}

Status BlobDBImpl::TEST_GCFileAndUpdateLSM(std::shared_ptr<BlobFile>& bfile,
                                           GCStats* gc_stats) {
  return GCFileAndUpdateLSM(bfile, gc_stats);
}

void BlobDBImpl::TEST_RunGC() { RunGC(false /*abort*/); }

void BlobDBImpl::TEST_ObsoleteFile(std::shared_ptr<BlobFile>& bfile) {
  uint64_t number = bfile->BlobFileNumber();
  assert(blob_files_.count(number) > 0);
  bfile->SetCanBeDeleted();
  {
    WriteLock l(&mutex_);
    obsolete_files_.push_back(bfile);
  }
}
#endif  //  !NDEBUG

}  // namespace blob_db
}  // namespace rocksdb
#endif  // ROCKSDB_LITE
