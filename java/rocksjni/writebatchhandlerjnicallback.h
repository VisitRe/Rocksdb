// Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
// This source code is licensed under the BSD-style license found in the
// LICENSE file in the root directory of this source tree. An additional grant
// of patent rights can be found in the PATENTS file in the same directory.
//
// This file implements the callback "bridge" between Java and C++ for
// rocksdb::WriteBatch::Handler.

#ifndef JAVA_ROCKSJNI_WRITEBATCHHANDLERJNICALLBACK_H_
#define JAVA_ROCKSJNI_WRITEBATCHHANDLERJNICALLBACK_H_

#include <jni.h>
#include "rocksdb/write_batch.h"

namespace rocksdb {
/**
 * This class acts as a bridge between C++
 * and Java. The methods in this class will be
 * called back from the RocksDB storage engine (C++)
 * which calls the appropriate Java method.
 * This enables Write Batch Handlers to be implemented in Java.
 */
class WriteBatchHandlerJniCallback : public WriteBatch::Handler {
 public:
    WriteBatchHandlerJniCallback(
      JNIEnv* env, jobject jWriteBackHandler);
    ~WriteBatchHandlerJniCallback();
    void Put(const Slice& key, const Slice& value);
    void Merge(const Slice& key, const Slice& value);
    void Delete(const Slice& key);
    void LogData(const Slice& blob);
    bool Continue();
    void SingleDelete(const Slice& key);
    Status PutCF(uint32_t column_family_id, const Slice& key,
                 const Slice& value);
    Status DeleteCF(uint32_t column_family_id, const Slice& key);
    Status MergeCF(uint32_t column_family_id, const Slice& key,
                   const Slice& value);
    Status SingleDeleteCF(uint32_t column_family_id, const Slice& key);

 private:
    JNIEnv* m_env;
    jobject m_jWriteBatchHandler;
    jbyteArray sliceToJArray(const Slice& s);
    jmethodID m_jPutMethodId;
    jmethodID m_jMergeMethodId;
    jmethodID m_jDeleteMethodId;
    jmethodID m_jLogDataMethodId;
    jmethodID m_jContinueMethodId;
    jmethodID m_jPutCFMethodId;
    jmethodID m_jMergeCFMethodId;
    jmethodID m_jDeleteCFMethodId;
    jmethodID m_jSingleDeleteMethodId;
    jmethodID m_jSingleDeleteCFMethodId;
};
}  // namespace rocksdb

#endif  // JAVA_ROCKSJNI_WRITEBATCHHANDLERJNICALLBACK_H_
