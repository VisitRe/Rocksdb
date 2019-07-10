// Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// This file implements the "bridge" between Java and C++ and enables
// calling C++ rocksdb::SstFileReader methods
// from Java side.

#include <jni.h>
#include <string>

#include "include/org_rocksdb_SstFileReader.h"
#include "rocksdb/comparator.h"
#include "rocksdb/env.h"
#include "rocksdb/options.h"
#include "rocksdb/sst_file_reader.h"
#include "rocksjni/portal.h"

/*
 * Class:     org_rocksdb_SstFileReader
 * Method:    newSstFileReader
 * Signature: (J)J
 */
jlong Java_org_rocksdb_SstFileReader_newSstFileReader(JNIEnv * /*env*/,
                                                         jclass /*jcls*/,
                                                          jlong joptions) {
  auto *options = reinterpret_cast<const rocksdb::Options *>(joptions);
  rocksdb::SstFileReader *sst_file_reader =
      new rocksdb::SstFileReader(*options);
  return reinterpret_cast<jlong>(sst_file_reader);
}

/*
 * Class:     org_rocksdb_SstFileReader
 * Method:    open
 * Signature: (JLjava/lang/String;)V
 */
void Java_org_rocksdb_SstFileReader_open(JNIEnv *env, jobject /*jobj*/,
                                         jlong jhandle, jstring jfile_path) {
  const char *file_path = env->GetStringUTFChars(jfile_path, nullptr);
  if (file_path == nullptr) {
    // exception thrown: OutOfMemoryError
    return;
  }
  rocksdb::Status s =
      reinterpret_cast<rocksdb::SstFileReader *>(jhandle)->Open(file_path);
  env->ReleaseStringUTFChars(jfile_path, file_path);

  if (!s.ok()) {
    rocksdb::RocksDBExceptionJni::ThrowNew(env, s);
  }
}

/*
* Class:     org_rocksdb_SstFileReader
* Method:    newIterator
* Signature: (JJ)J
*/
jlong Java_org_rocksdb_SstFileReader_newIterator(JNIEnv* /*env*/,
                                                 jobject /*jobj*/,
                                                 jlong jhandle,
                                                 jlong jread_options_handle) {
    auto* txn = reinterpret_cast<rocksdb::SstFileReader*>(jhandle);
//    auto* read_options =
//        reinterpret_cast<rocksdb::ReadOptions*>(jread_options_handle);
    //return reinterpret_cast<jlong>(txn->NewIterator(*read_options));
    (void)jread_options_handle;
    return reinterpret_cast<jlong>(txn->NewIterator(rocksdb::ReadOptions()));
}

/*
 * Class:     org_rocksdb_SstFileReader
 * Method:    disposeInternal
 * Signature: (J)V
 */
void Java_org_rocksdb_SstFileReader_disposeInternal(JNIEnv * /*env*/,
                                                    jobject /*jobj*/,
                                                    jlong jhandle) {
    delete reinterpret_cast<rocksdb::SstFileReader *>(jhandle);
}


