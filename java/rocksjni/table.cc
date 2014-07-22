// Copyright (c) 2014, Facebook, Inc.  All rights reserved.
// This source code is licensed under the BSD-style license found in the
// LICENSE file in the root directory of this source tree. An additional grant
// of patent rights can be found in the PATENTS file in the same directory.
//
// This file implements the "bridge" between Java and C++ for rocksdb::Options.

#include <jni.h>
#include "include/org_rocksdb_PlainTableConfig.h"
#include "rocksdb/table.h"

/*
 * Class:     org_rocksdb_PlainTableConfig
 * Method:    newTableFactoryHandle
 * Signature: (IIDI)J
 */
jlong Java_org_rocksdb_PlainTableConfig_newTableFactoryHandle(
    JNIEnv* env, jobject jobj, jint jkey_size, jint jbloom_bits_per_key,
    jdouble jhash_table_ratio, jint jindex_sparseness) {
  rocksdb::PlainTableOptions options = rocksdb::PlainTableOptions();
  options.user_key_len = jkey_size;
  options.bloom_bits_per_key = jbloom_bits_per_key;
  options.hash_table_ratio = jhash_table_ratio;
  options.index_sparseness = jindex_sparseness;
  return reinterpret_cast<jlong>(rocksdb::NewPlainTableFactory(options));
}
