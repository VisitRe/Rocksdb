//  Copyright (c) 2013, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "utilities/backupable_db.h"
#include "db/filename.h"
#include "util/coding.h"
#include "rocksdb/transaction_log.h"

#define __STDC_FORMAT_MACROS

#include <inttypes.h>
#include <algorithm>
#include <vector>
#include <map>
#include <string>
#include <limits>

namespace rocksdb {

// -------- BackupEngine class ---------
class BackupEngine {
 public:
  BackupEngine(Env* db_env, const BackupableDBOptions& options);
  ~BackupEngine();
  Status CreateNewBackup(DB* db, bool flush_before_backup = false);
  Status PurgeOldBackups(uint32_t num_backups_to_keep);
  Status DeleteBackup(BackupID backup_id);

  void GetBackupInfo(std::vector<BackupInfo>* backup_info);
  Status RestoreDBFromBackup(BackupID backup_id, const std::string &db_dir,
                             const std::string &wal_dir);
  Status RestoreDBFromLatestBackup(const std::string &db_dir,
                                   const std::string &wal_dir) {
    return RestoreDBFromBackup(latest_backup_id_, db_dir, wal_dir);
  }

  void DeleteBackupsNewerThan(uint64_t sequence_number);

 private:
  class BackupMeta {
   public:
    BackupMeta(const std::string& meta_filename,
        std::unordered_map<std::string, int>* file_refs, Env* env)
      : timestamp_(0), size_(0), meta_filename_(meta_filename),
        file_refs_(file_refs), env_(env) {}

    ~BackupMeta() {}

    void RecordTimestamp() {
      env_->GetCurrentTime(&timestamp_);
    }
    int64_t GetTimestamp() const {
      return timestamp_;
    }
    uint64_t GetSize() const {
      return size_;
    }
    void SetSequenceNumber(uint64_t sequence_number) {
      sequence_number_ = sequence_number;
    }
    uint64_t GetSequenceNumber() {
      return sequence_number_;
    }

    void AddFile(const std::string& filename, uint64_t size);
    void Delete();

    bool Empty() {
      return files_.empty();
    }

    const std::vector<std::string>& GetFiles() {
      return files_;
    }

    Status LoadFromFile(const std::string& backup_dir);
    Status StoreToFile(bool sync);

   private:
    int64_t timestamp_;
    // sequence number is only approximate, should not be used
    // by clients
    uint64_t sequence_number_;
    uint64_t size_;
    std::string const meta_filename_;
    // files with relative paths (without "/" prefix!!)
    std::vector<std::string> files_;
    std::unordered_map<std::string, int>* file_refs_;
    Env* env_;

    static const size_t max_backup_meta_file_size_ = 10 * 1024 * 1024; // 10MB
  }; // BackupMeta

  inline std::string GetAbsolutePath(
      const std::string &relative_path = "") const {
    assert(relative_path.size() == 0 || relative_path[0] != '/');
    return options_.backup_dir + "/" + relative_path;
  }
  inline std::string GetPrivateDirRel() const {
    return "private";
  }
  inline std::string GetPrivateFileRel(BackupID backup_id,
                                       const std::string &file = "") const {
    assert(file.size() == 0 || file[0] != '/');
    return GetPrivateDirRel() + "/" + std::to_string(backup_id) + "/" + file;
  }
  inline std::string GetSharedFileRel(const std::string& file = "") const {
    assert(file.size() == 0 || file[0] != '/');
    return "shared/" + file;
  }
  inline std::string GetLatestBackupFile(bool tmp = false) const {
    return GetAbsolutePath(std::string("LATEST_BACKUP") + (tmp ? ".tmp" : ""));
  }
  inline std::string GetBackupMetaDir() const {
    return GetAbsolutePath("meta");
  }
  inline std::string GetBackupMetaFile(BackupID backup_id) const {
    return GetBackupMetaDir() + "/" + std::to_string(backup_id);
  }

  Status GetLatestBackupFileContents(uint32_t* latest_backup);
  Status PutLatestBackupFileContents(uint32_t latest_backup);
  // if size_limit == 0, there is no size limit, copy everything
  Status CopyFile(const std::string& src,
                  const std::string& dst,
                  Env* src_env,
                  Env* dst_env,
                  bool sync,
                  uint64_t* size = nullptr,
                  uint64_t size_limit = 0);
  // if size_limit == 0, there is no size limit, copy everything
  Status BackupFile(BackupID backup_id,
                    BackupMeta* backup,
                    bool shared,
                    const std::string& src_dir,
                    const std::string& src_fname, // starts with "/"
                    uint64_t size_limit = 0);
  // Will delete all the files we don't need anymore
  // If full_scan == true, it will do the full scan of files/ directory
  // and delete all the files that are not referenced from backuped_file_refs_
  void GarbageCollection(bool full_scan);

  // backup state data
  BackupID latest_backup_id_;
  std::map<BackupID, BackupMeta> backups_;
  std::unordered_map<std::string, int> backuped_file_refs_;
  std::vector<BackupID> obsolete_backups_;

  // options data
  BackupableDBOptions options_;
  Env* db_env_;
  Env* backup_env_;

  static const size_t copy_file_buffer_size_ = 5 * 1024 * 1024LL; // 5MB
};

BackupEngine::BackupEngine(Env* db_env, const BackupableDBOptions& options)
  : options_(options),
    db_env_(db_env),
    backup_env_(options.backup_env != nullptr ? options.backup_env : db_env_) {

  // create all the dirs we need
  backup_env_->CreateDirIfMissing(GetAbsolutePath());
  backup_env_->CreateDirIfMissing(GetAbsolutePath(GetSharedFileRel()));
  backup_env_->CreateDirIfMissing(GetAbsolutePath(GetPrivateDirRel()));
  backup_env_->CreateDirIfMissing(GetBackupMetaDir());

  std::vector<std::string> backup_meta_files;
  backup_env_->GetChildren(GetBackupMetaDir(), &backup_meta_files);
  // create backups_ structure
  for (auto& file : backup_meta_files) {
    BackupID backup_id = 0;
    sscanf(file.c_str(), "%u", &backup_id);
    if (backup_id == 0 || file != std::to_string(backup_id)) {
      // invalid file name, delete that
      backup_env_->DeleteFile(GetBackupMetaDir() + "/" + file);
      continue;
    }
    assert(backups_.find(backup_id) == backups_.end());
    backups_.insert(std::make_pair(
        backup_id, BackupMeta(GetBackupMetaFile(backup_id),
                              &backuped_file_refs_, backup_env_)));
  }

  if (options_.destroy_old_data) { // Destory old data
    for (auto& backup : backups_) {
      backup.second.Delete();
      obsolete_backups_.push_back(backup.first);
    }
    backups_.clear();
    // start from beginning
    latest_backup_id_ = 0;
    // GarbageCollection() will do the actual deletion
  } else { // Load data from storage
    // load the backups if any
    for (auto& backup : backups_) {
      Status s = backup.second.LoadFromFile(options_.backup_dir);
      if (!s.ok()) {
        Log(options_.info_log, "Backup %u corrupted - deleting -- %s",
            backup.first, s.ToString().c_str());
        backup.second.Delete();
        obsolete_backups_.push_back(backup.first);
      }
    }
    // delete obsolete backups from the structure
    for (auto ob : obsolete_backups_) {
      backups_.erase(ob);
    }

    Status s = GetLatestBackupFileContents(&latest_backup_id_);
    // If latest backup file is corrupted or non-existent
    // set latest backup as the biggest backup we have
    // or 0 if we have no backups
    if (!s.ok() ||
        backups_.find(latest_backup_id_) == backups_.end()) {
      auto itr = backups_.end();
      latest_backup_id_ = (itr == backups_.begin()) ? 0 : (--itr)->first;
    }
  }

  // delete any backups that claim to be later than latest
  for (auto itr = backups_.upper_bound(latest_backup_id_);
       itr != backups_.end();) {
    itr->second.Delete();
    obsolete_backups_.push_back(itr->first);
    itr = backups_.erase(itr);
  }

  PutLatestBackupFileContents(latest_backup_id_); // Ignore errors
  GarbageCollection(true);
  Log(options_.info_log,
      "Initialized BackupEngine, the latest backup is %u.",
      latest_backup_id_);
}

BackupEngine::~BackupEngine() {
  LogFlush(options_.info_log);
}

void BackupEngine::DeleteBackupsNewerThan(uint64_t sequence_number) {
  for (auto backup : backups_) {
    if (backup.second.GetSequenceNumber() > sequence_number) {
      Log(options_.info_log,
          "Deleting backup %u because sequence number (%llu) is newer than %llu",
          backup.first, backup.second.GetSequenceNumber(), sequence_number);
      backup.second.Delete();
      obsolete_backups_.push_back(backup.first);
    }
  }
  for (auto ob : obsolete_backups_) {
    backups_.erase(backups_.find(ob));
  }
  auto itr = backups_.end();
  latest_backup_id_ = (itr == backups_.begin()) ? 0 : (--itr)->first;
  PutLatestBackupFileContents(latest_backup_id_); // Ignore errors
  GarbageCollection(false);
}

Status BackupEngine::CreateNewBackup(DB* db, bool flush_before_backup) {
  Status s;
  std::vector<std::string> live_files;
  VectorLogPtr live_wal_files;
  uint64_t manifest_file_size = 0;
  uint64_t sequence_number = db->GetLatestSequenceNumber();

  s = db->DisableFileDeletions();
  if (s.ok()) {
    // this will return live_files prefixed with "/"
    s = db->GetLiveFiles(live_files, &manifest_file_size, flush_before_backup);
  }
  // if we didn't flush before backup, we need to also get WAL files
  if (s.ok() && !flush_before_backup) {
    // returns file names prefixed with "/"
    s = db->GetSortedWalFiles(live_wal_files);
  }
  if (!s.ok()) {
    db->EnableFileDeletions();
    return s;
  }

  BackupID new_backup_id = latest_backup_id_ + 1;
  assert(backups_.find(new_backup_id) == backups_.end());
  auto ret = backups_.insert(std::make_pair(
      new_backup_id, BackupMeta(GetBackupMetaFile(new_backup_id),
                                &backuped_file_refs_, backup_env_)));
  assert(ret.second == true);
  auto& new_backup = ret.first->second;
  new_backup.RecordTimestamp();
  new_backup.SetSequenceNumber(sequence_number);

  Log(options_.info_log, "Started the backup process -- creating backup %u",
      new_backup_id);

  // create private dir
  s = backup_env_->CreateDir(GetAbsolutePath(GetPrivateFileRel(new_backup_id)));

  // copy live_files
  for (size_t i = 0; s.ok() && i < live_files.size(); ++i) {
    uint64_t number;
    FileType type;
    bool ok = ParseFileName(live_files[i], &number, &type);
    if (!ok) {
      assert(false);
      return Status::Corruption("Can't parse file name. This is very bad");
    }
    // we should only get sst, manifest and current files here
    assert(type == kTableFile ||
             type == kDescriptorFile ||
             type == kCurrentFile);

    // rules:
    // * if it's kTableFile, than it's shared
    // * if it's kDescriptorFile, limit the size to manifest_file_size
    s = BackupFile(new_backup_id,
                   &new_backup,
                   type == kTableFile,       /* shared  */
                   db->GetName(),            /* src_dir */
                   live_files[i],            /* src_fname */
                   (type == kDescriptorFile) ? manifest_file_size : 0);
  }

  // copy WAL files
  for (size_t i = 0; s.ok() && i < live_wal_files.size(); ++i) {
    if (live_wal_files[i]->Type() == kAliveLogFile) {
      // we only care about live log files
      // copy the file into backup_dir/files/<new backup>/
      s = BackupFile(new_backup_id,
                     &new_backup,
                     false, /* not shared */
                     db->GetOptions().wal_dir,
                     live_wal_files[i]->PathName());
    }
  }

  // we copied all the files, enable file deletions
  db->EnableFileDeletions();

  if (s.ok()) {
    // persist the backup metadata on the disk
    s = new_backup.StoreToFile(options_.sync);
  }
  if (s.ok()) {
    // install the newly created backup meta! (atomic)
    s = PutLatestBackupFileContents(new_backup_id);
  }
  if (!s.ok()) {
    // clean all the files we might have created
    Log(options_.info_log, "Backup failed -- %s", s.ToString().c_str());
    backups_.erase(new_backup_id);
    GarbageCollection(true);
    return s;
  }

  // here we know that we succeeded and installed the new backup
  // in the LATEST_BACKUP file
  latest_backup_id_ = new_backup_id;
  Log(options_.info_log, "Backup DONE. All is good");
  return s;
}

Status BackupEngine::PurgeOldBackups(uint32_t num_backups_to_keep) {
  Log(options_.info_log, "Purging old backups, keeping %u",
      num_backups_to_keep);
  while (num_backups_to_keep < backups_.size()) {
    Log(options_.info_log, "Deleting backup %u", backups_.begin()->first);
    backups_.begin()->second.Delete();
    obsolete_backups_.push_back(backups_.begin()->first);
    backups_.erase(backups_.begin());
  }
  GarbageCollection(false);
  return Status::OK();
}

Status BackupEngine::DeleteBackup(BackupID backup_id) {
  Log(options_.info_log, "Deleting backup %u", backup_id);
  auto backup = backups_.find(backup_id);
  if (backup == backups_.end()) {
    return Status::NotFound("Backup not found");
  }
  backup->second.Delete();
  obsolete_backups_.push_back(backup_id);
  backups_.erase(backup);
  GarbageCollection(false);
  return Status::OK();
}

void BackupEngine::GetBackupInfo(std::vector<BackupInfo>* backup_info) {
  backup_info->reserve(backups_.size());
  for (auto& backup : backups_) {
    if (!backup.second.Empty()) {
      backup_info->push_back(BackupInfo(
          backup.first, backup.second.GetTimestamp(), backup.second.GetSize()));
    }
  }
}

Status BackupEngine::RestoreDBFromBackup(BackupID backup_id,
                                         const std::string &db_dir,
                                         const std::string &wal_dir) {
  auto backup_itr = backups_.find(backup_id);
  if (backup_itr == backups_.end()) {
    return Status::NotFound("Backup not found");
  }
  auto& backup = backup_itr->second;
  if (backup.Empty()) {
    return Status::NotFound("Backup not found");
  }

  Log(options_.info_log, "Restoring backup id %u\n", backup_id);

  // just in case. Ignore errors
  db_env_->CreateDirIfMissing(db_dir);
  db_env_->CreateDirIfMissing(wal_dir);

  // delete log files that might have been already in wal_dir.
  // This is important since they might get replayed to the restored DB,
  // which will then differ from the backuped DB
  std::vector<std::string> wal_dir_children;
  db_env_->GetChildren(wal_dir, &wal_dir_children); // ignore errors
  for (auto f : wal_dir_children) {
    db_env_->DeleteFile(wal_dir + "/" + f); // ignore errors
  }

  Status s;
  for (auto& file : backup.GetFiles()) {
    std::string dst;
    // 1. extract the filename
    size_t slash = file.find_last_of('/');
    // file will either be shared/<file> or private/<number>/<file>
    assert(slash != std::string::npos);
    dst = file.substr(slash + 1);

    // 2. find the filetype
    uint64_t number;
    FileType type;
    bool ok = ParseFileName(dst, &number, &type);
    if (!ok) {
      return Status::Corruption("Backup corrupted");
    }
    // 3. Construct the final path
    // kLogFile lives in wal_dir and all the rest live in db_dir
    dst = ((type == kLogFile) ? wal_dir : db_dir) +
      "/" + dst;

    Log(options_.info_log, "Restoring %s to %s\n", file.c_str(), dst.c_str());
    s = CopyFile(GetAbsolutePath(file), dst, backup_env_, db_env_, false);
    if (!s.ok()) {
      break;
    }
  }

  Log(options_.info_log, "Restoring done -- %s\n", s.ToString().c_str());
  return s;
}

// latest backup id is an ASCII representation of latest backup id
Status BackupEngine::GetLatestBackupFileContents(uint32_t* latest_backup) {
  Status s;
  unique_ptr<SequentialFile> file;
  s = backup_env_->NewSequentialFile(GetLatestBackupFile(),
                                     &file,
                                     EnvOptions());
  if (!s.ok()) {
    return s;
  }

  char buf[11];
  Slice data;
  s = file->Read(10, &data, buf);
  if (!s.ok() || data.size() == 0) {
    return s.ok() ? Status::Corruption("Latest backup file corrupted") : s;
  }
  buf[data.size()] = 0;

  *latest_backup = 0;
  sscanf(data.data(), "%u", latest_backup);
  if (backup_env_->FileExists(GetBackupMetaFile(*latest_backup)) == false) {
    s = Status::Corruption("Latest backup file corrupted");
  }
  return Status::OK();
}

// this operation HAS to be atomic
// writing 4 bytes to the file is atomic alright, but we should *never*
// do something like 1. delete file, 2. write new file
// We write to a tmp file and then atomically rename
Status BackupEngine::PutLatestBackupFileContents(uint32_t latest_backup) {
  Status s;
  unique_ptr<WritableFile> file;
  EnvOptions env_options;
  env_options.use_mmap_writes = false;
  s = backup_env_->NewWritableFile(GetLatestBackupFile(true),
                                   &file,
                                   env_options);
  if (!s.ok()) {
    backup_env_->DeleteFile(GetLatestBackupFile(true));
    return s;
  }

  char file_contents[10];
  int len = sprintf(file_contents, "%u\n", latest_backup);
  s = file->Append(Slice(file_contents, len));
  if (s.ok() && options_.sync) {
    file->Sync();
  }
  if (s.ok()) {
    s = file->Close();
  }
  if (s.ok()) {
    // atomically replace real file with new tmp
    s = backup_env_->RenameFile(GetLatestBackupFile(true),
                                GetLatestBackupFile(false));
  }
  return s;
}

Status BackupEngine::CopyFile(const std::string& src,
                              const std::string& dst,
                              Env* src_env,
                              Env* dst_env,
                              bool sync,
                              uint64_t* size,
                              uint64_t size_limit) {
  Status s;
  unique_ptr<WritableFile> dst_file;
  unique_ptr<SequentialFile> src_file;
  EnvOptions env_options;
  env_options.use_mmap_writes = false;
  if (size != nullptr) {
    *size = 0;
  }

  // Check if size limit is set. if not, set it to very big number
  if (size_limit == 0) {
    size_limit = std::numeric_limits<uint64_t>::max();
  }

  s = src_env->NewSequentialFile(src, &src_file, env_options);
  if (s.ok()) {
    s = dst_env->NewWritableFile(dst, &dst_file, env_options);
  }
  if (!s.ok()) {
    return s;
  }

  unique_ptr<char[]> buf(new char[copy_file_buffer_size_]);
  Slice data;

  do {
    size_t buffer_to_read = (copy_file_buffer_size_ < size_limit) ?
      copy_file_buffer_size_ : size_limit;
    s = src_file->Read(buffer_to_read, &data, buf.get());
    size_limit -= data.size();
    if (size != nullptr) {
      *size += data.size();
    }
    if (s.ok()) {
      s = dst_file->Append(data);
    }
  } while (s.ok() && data.size() > 0 && size_limit > 0);

  if (s.ok() && sync) {
    s = dst_file->Sync();
  }

  return s;
}

// src_fname will always start with "/"
Status BackupEngine::BackupFile(BackupID backup_id,
                                BackupMeta* backup,
                                bool shared,
                                const std::string& src_dir,
                                const std::string& src_fname,
                                uint64_t size_limit) {

  assert(src_fname.size() > 0 && src_fname[0] == '/');
  std::string dst_relative = src_fname.substr(1);
  if (shared) {
    dst_relative = GetSharedFileRel(dst_relative);
  } else {
    dst_relative = GetPrivateFileRel(backup_id, dst_relative);
  }
  std::string dst_path = GetAbsolutePath(dst_relative);
  Status s;
  uint64_t size;

  // if it's shared, we also need to check if it exists -- if it does,
  // no need to copy it again
  if (shared && backup_env_->FileExists(dst_path)) {
    backup_env_->GetFileSize(dst_path, &size); // Ignore error
    Log(options_.info_log, "%s already present", src_fname.c_str());
  } else {
    Log(options_.info_log, "Copying %s", src_fname.c_str());
    s = CopyFile(src_dir + src_fname,
                 dst_path,
                 db_env_,
                 backup_env_,
                 options_.sync,
                 &size,
                 size_limit);
  }
  if (s.ok()) {
    backup->AddFile(dst_relative, size);
  }
  return s;
}

void BackupEngine::GarbageCollection(bool full_scan) {
  Log(options_.info_log, "Starting garbage collection");
  std::vector<std::string> to_delete;
  for (auto& itr : backuped_file_refs_) {
    if (itr.second == 0) {
      Status s = backup_env_->DeleteFile(GetAbsolutePath(itr.first));
      Log(options_.info_log, "Deleting %s -- %s", itr.first.c_str(),
          s.ToString().c_str());
      to_delete.push_back(itr.first);
    }
  }
  for (auto& td : to_delete) {
    backuped_file_refs_.erase(td);
  }
  if (!full_scan) {
    // take care of private dirs -- if full_scan == true, then full_scan will
    // take care of them
    for (auto backup_id : obsolete_backups_) {
      std::string private_dir = GetPrivateFileRel(backup_id);
      Status s = backup_env_->DeleteDir(GetAbsolutePath(private_dir));
      Log(options_.info_log, "Deleting private dir %s -- %s",
          private_dir.c_str(), s.ToString().c_str());
    }
  }
  obsolete_backups_.clear();

  if (full_scan) {
    Log(options_.info_log, "Starting full scan garbage collection");
    // delete obsolete shared files
    std::vector<std::string> shared_children;
    backup_env_->GetChildren(GetAbsolutePath(GetSharedFileRel()),
                             &shared_children);
    for (auto& child : shared_children) {
      std::string rel_fname = GetSharedFileRel(child);
      // if it's not refcounted, delete it
      if (backuped_file_refs_.find(rel_fname) == backuped_file_refs_.end()) {
        // this might be a directory, but DeleteFile will just fail in that
        // case, so we're good
        Status s = backup_env_->DeleteFile(GetAbsolutePath(rel_fname));
        if (s.ok()) {
          Log(options_.info_log, "Deleted %s", rel_fname.c_str());
        }
      }
    }

    // delete obsolete private files
    std::vector<std::string> private_children;
    backup_env_->GetChildren(GetAbsolutePath(GetPrivateDirRel()),
                             &private_children);
    for (auto& child : private_children) {
      BackupID backup_id = 0;
      sscanf(child.c_str(), "%u", &backup_id);
      if (backup_id == 0 || backups_.find(backup_id) != backups_.end()) {
        // it's either not a number or it's still alive. continue
        continue;
      }
      // here we have to delete the dir and all its children
      std::string full_private_path =
          GetAbsolutePath(GetPrivateFileRel(backup_id));
      std::vector<std::string> subchildren;
      backup_env_->GetChildren(full_private_path, &subchildren);
      for (auto& subchild : subchildren) {
        Status s = backup_env_->DeleteFile(full_private_path + subchild);
        if (s.ok()) {
          Log(options_.info_log, "Deleted %s",
              (full_private_path + subchild).c_str());
        }
      }
      // finally delete the private dir
      Status s = backup_env_->DeleteDir(full_private_path);
      Log(options_.info_log, "Deleted dir %s -- %s", full_private_path.c_str(),
          s.ToString().c_str());
    }
  }
}

// ------- BackupMeta class --------

void BackupEngine::BackupMeta::AddFile(const std::string& filename,
                                       uint64_t size) {
  size_ += size;
  files_.push_back(filename);
  auto itr = file_refs_->find(filename);
  if (itr == file_refs_->end()) {
    file_refs_->insert(std::make_pair(filename, 1));
  } else {
    ++itr->second; // increase refcount if already present
  }
}

void BackupEngine::BackupMeta::Delete() {
  for (auto& file : files_) {
    auto itr = file_refs_->find(file);
    assert(itr != file_refs_->end());
    --(itr->second); // decrease refcount
  }
  files_.clear();
  // delete meta file
  env_->DeleteFile(meta_filename_);
  timestamp_ = 0;
}

// each backup meta file is of the format:
// <timestamp>
// <seq number>
// <number of files>
// <file1>
// <file2>
// ...
// TODO: maybe add checksum?
Status BackupEngine::BackupMeta::LoadFromFile(const std::string& backup_dir) {
  assert(Empty());
  Status s;
  unique_ptr<SequentialFile> backup_meta_file;
  s = env_->NewSequentialFile(meta_filename_, &backup_meta_file, EnvOptions());
  if (!s.ok()) {
    return s;
  }

  unique_ptr<char[]> buf(new char[max_backup_meta_file_size_ + 1]);
  Slice data;
  s = backup_meta_file->Read(max_backup_meta_file_size_, &data, buf.get());

  if (!s.ok() || data.size() == max_backup_meta_file_size_) {
    return s.ok() ? Status::IOError("File size too big") : s;
  }
  buf[data.size()] = 0;

  uint32_t num_files = 0;
  int bytes_read = 0;
  sscanf(data.data(), "%lld%n", &timestamp_, &bytes_read);
  data.remove_prefix(bytes_read + 1); // +1 for '\n'
  sscanf(data.data(), "%llu%n", &sequence_number_, &bytes_read);
  data.remove_prefix(bytes_read + 1); // +1 for '\n'
  sscanf(data.data(), "%u%n", &num_files, &bytes_read);
  data.remove_prefix(bytes_read + 1); // +1 for '\n'

  std::vector<std::pair<std::string, uint64_t>> files;

  for (uint32_t i = 0; s.ok() && i < num_files; ++i) {
    std::string filename = GetSliceUntil(&data, '\n').ToString();
    uint64_t size;
    s = env_->GetFileSize(backup_dir + "/" + filename, &size);
    files.push_back(std::make_pair(filename, size));
  }

  if (s.ok()) {
    for (auto file : files) {
      AddFile(file.first, file.second);
    }
  }

  return s;
}

Status BackupEngine::BackupMeta::StoreToFile(bool sync) {
  Status s;
  unique_ptr<WritableFile> backup_meta_file;
  EnvOptions env_options;
  env_options.use_mmap_writes = false;
  s = env_->NewWritableFile(meta_filename_ + ".tmp", &backup_meta_file,
                            env_options);
  if (!s.ok()) {
    return s;
  }

  unique_ptr<char[]> buf(new char[max_backup_meta_file_size_]);
  int len = 0, buf_size = max_backup_meta_file_size_;
  len += snprintf(buf.get(), buf_size, "%" PRId64 "\n", timestamp_);
  len += snprintf(buf.get() + len, buf_size - len, "%" PRIu64 "\n",
                  sequence_number_);
  len += snprintf(buf.get() + len, buf_size - len, "%zu\n", files_.size());
  for (size_t i = 0; i < files_.size(); ++i) {
    len += snprintf(buf.get() + len, buf_size - len, "%s\n", files_[i].c_str());
  }

  s = backup_meta_file->Append(Slice(buf.get(), (size_t)len));
  if (s.ok() && sync) {
    s = backup_meta_file->Sync();
  }
  if (s.ok()) {
    s = backup_meta_file->Close();
  }
  if (s.ok()) {
    s = env_->RenameFile(meta_filename_ + ".tmp", meta_filename_);
  }
  return s;
}

// --- BackupableDB methods --------

BackupableDB::BackupableDB(DB* db, const BackupableDBOptions& options)
    : StackableDB(db), backup_engine_(new BackupEngine(db->GetEnv(), options)) {
  backup_engine_->DeleteBackupsNewerThan(GetLatestSequenceNumber());
}

BackupableDB::~BackupableDB() {
  delete backup_engine_;
}

Status BackupableDB::CreateNewBackup(bool flush_before_backup) {
  return backup_engine_->CreateNewBackup(this, flush_before_backup);
}

void BackupableDB::GetBackupInfo(std::vector<BackupInfo>* backup_info) {
  backup_engine_->GetBackupInfo(backup_info);
}

Status BackupableDB::PurgeOldBackups(uint32_t num_backups_to_keep) {
  return backup_engine_->PurgeOldBackups(num_backups_to_keep);
}

Status BackupableDB::DeleteBackup(BackupID backup_id) {
  return backup_engine_->DeleteBackup(backup_id);
}

// --- RestoreBackupableDB methods ------

RestoreBackupableDB::RestoreBackupableDB(Env* db_env,
                                         const BackupableDBOptions& options)
    : backup_engine_(new BackupEngine(db_env, options)) {}

RestoreBackupableDB::~RestoreBackupableDB() {
  delete backup_engine_;
}

void
RestoreBackupableDB::GetBackupInfo(std::vector<BackupInfo>* backup_info) {
  backup_engine_->GetBackupInfo(backup_info);
}

Status RestoreBackupableDB::RestoreDBFromBackup(BackupID backup_id,
                                                const std::string& db_dir,
                                                const std::string& wal_dir) {
  return backup_engine_->RestoreDBFromBackup(backup_id, db_dir, wal_dir);
}

Status
RestoreBackupableDB::RestoreDBFromLatestBackup(const std::string& db_dir,
                                               const std::string& wal_dir) {
  return backup_engine_->RestoreDBFromLatestBackup(db_dir, wal_dir);
}

Status RestoreBackupableDB::PurgeOldBackups(uint32_t num_backups_to_keep) {
  return backup_engine_->PurgeOldBackups(num_backups_to_keep);
}

Status RestoreBackupableDB::DeleteBackup(BackupID backup_id) {
  return backup_engine_->DeleteBackup(backup_id);
}

}  // namespace rocksdb
