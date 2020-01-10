//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
#pragma once

#ifndef ROCKSDB_LITE

namespace rocksdb {

namespace blob_db {

/**
 * Statistics related to a single garbage collection pass (i.e. a single compaction).
 */
class BlobDBGarbageCollectionStats {
 public:
   uint64_t AllBlobs() const { return all_blobs_; }
   uint64_t RelocatedBlobs() const { return relocated_blobs_; }
   uint64_t RelocatedBytes() const { return relocated_bytes_; }
   uint64_t NewFiles() const { return new_files_; }

   void AddBlob() { ++all_blobs_; }
   void AddRelocatedBlob(uint64_t size) {
     ++relocated_blobs_;
     relocated_bytes_ += size;
   }
   void AddNewFile() { ++new_files_; }

 private:
   uint64_t all_blobs_ = 0;
   uint64_t relocated_blobs_ = 0;
   uint64_t relocated_bytes_ = 0;
   uint64_t new_files_ = 0;
};

}  // namespace blob_db
}  // namespace rocksdb
#endif  // ROCKSDB_LITE
