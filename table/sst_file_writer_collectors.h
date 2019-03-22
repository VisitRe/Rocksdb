//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#pragma once
#include <string>
#include "rocksdb/types.h"
#include "util/string_util.h"

namespace rocksdb {

// Table Properties that are specific to tables created by SstFileWriter.
struct ExternalSstFilePropertyNames {
  // value of this property is a fixed uint32 number.
  static const std::string kVersion;
  // value of this property is a fixed uint64 number.
  static const std::string kGlobalSeqno;
};

// PropertiesCollector used to add properties specific to tables
// generated by SstFileWriter
class SstFileWriterPropertiesCollector : public IntTblPropCollector {
 public:
  explicit SstFileWriterPropertiesCollector(int32_t version,
                                            SequenceNumber global_seqno)
      : version_(version), global_seqno_(global_seqno) {}

  virtual Status InternalAdd(const Slice& /*key*/, const Slice& /*value*/,
                             uint64_t /*file_size*/) override {
    // Intentionally left blank. Have no interest in collecting stats for
    // individual key/value pairs.
    return Status::OK();
  }

  virtual void BlockAdd(uint64_t /* blockRawBytes */,
                        uint64_t /* blockCompressedBytesFast */,
                        uint64_t /* blockCompressedBytesSlow */) override {
    // Intentionally left blank. No interest in collecting stats for
    // blocks.
    return;
  }

  virtual Status Finish(UserCollectedProperties* properties) override {
    // File version
    std::string version_val;
    PutFixed32(&version_val, static_cast<uint32_t>(version_));
    properties->insert({ExternalSstFilePropertyNames::kVersion, version_val});

    // Global Sequence number
    std::string seqno_val;
    PutFixed64(&seqno_val, static_cast<uint64_t>(global_seqno_));
    properties->insert({ExternalSstFilePropertyNames::kGlobalSeqno, seqno_val});

    return Status::OK();
  }

  virtual const char* Name() const override {
    return "SstFileWriterPropertiesCollector";
  }

  virtual UserCollectedProperties GetReadableProperties() const override {
    return {{ExternalSstFilePropertyNames::kVersion, ToString(version_)}};
  }

 private:
  int32_t version_;
  SequenceNumber global_seqno_;
};

class SstFileWriterPropertiesCollectorFactory
    : public IntTblPropCollectorFactory {
 public:
  explicit SstFileWriterPropertiesCollectorFactory(int32_t version,
                                                   SequenceNumber global_seqno)
      : version_(version), global_seqno_(global_seqno) {}

  virtual IntTblPropCollector* CreateIntTblPropCollector(
      uint32_t /*column_family_id*/) override {
    return new SstFileWriterPropertiesCollector(version_, global_seqno_);
  }

  virtual const char* Name() const override {
    return "SstFileWriterPropertiesCollector";
  }

 private:
  int32_t version_;
  SequenceNumber global_seqno_;
};

}  // namespace rocksdb
