//  Copyright (c) 2013, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "port/port_posix.h"

#include <cstdlib>
#include <stdio.h>
#include <string.h>
#include "util/logging.h"

#if defined(LZ4) || defined(LZ4HC)
#include "../lz4/lz4.c"
#endif

namespace rocksdb {
namespace port {

static void PthreadCall(const char* label, int result) {
  if (result != 0) {
    fprintf(stderr, "pthread %s: %s\n", label, strerror(result));
    abort();
  }
}

Mutex::Mutex(bool adaptive) {
#ifdef OS_LINUX
  if (!adaptive) {
    PthreadCall("init mutex", pthread_mutex_init(&mu_, NULL));
  } else {
    pthread_mutexattr_t mutex_attr;
    PthreadCall("init mutex attr", pthread_mutexattr_init(&mutex_attr));
    PthreadCall("set mutex attr",
                pthread_mutexattr_settype(&mutex_attr,
                                          PTHREAD_MUTEX_ADAPTIVE_NP));
    PthreadCall("init mutex", pthread_mutex_init(&mu_, &mutex_attr));
    PthreadCall("destroy mutex attr",
                pthread_mutexattr_destroy(&mutex_attr));
  }
#else // ignore adaptive for non-linux platform
  PthreadCall("init mutex", pthread_mutex_init(&mu_, NULL));
#endif // OS_LINUX
}

Mutex::~Mutex() { PthreadCall("destroy mutex", pthread_mutex_destroy(&mu_)); }

void Mutex::Lock() { PthreadCall("lock", pthread_mutex_lock(&mu_)); }

void Mutex::Unlock() { PthreadCall("unlock", pthread_mutex_unlock(&mu_)); }

CondVar::CondVar(Mutex* mu)
    : mu_(mu) {
    PthreadCall("init cv", pthread_cond_init(&cv_, NULL));
}

CondVar::~CondVar() { PthreadCall("destroy cv", pthread_cond_destroy(&cv_)); }

void CondVar::Wait() {
  PthreadCall("wait", pthread_cond_wait(&cv_, &mu_->mu_));
}

void CondVar::Signal() {
  PthreadCall("signal", pthread_cond_signal(&cv_));
}

void CondVar::SignalAll() {
  PthreadCall("broadcast", pthread_cond_broadcast(&cv_));
}

RWMutex::RWMutex() { PthreadCall("init mutex", pthread_rwlock_init(&mu_, NULL)); }

RWMutex::~RWMutex() { PthreadCall("destroy mutex", pthread_rwlock_destroy(&mu_)); }

void RWMutex::ReadLock() { PthreadCall("read lock", pthread_rwlock_rdlock(&mu_)); }

void RWMutex::WriteLock() { PthreadCall("write lock", pthread_rwlock_wrlock(&mu_)); }

void RWMutex::Unlock() { PthreadCall("unlock", pthread_rwlock_unlock(&mu_)); }

void InitOnce(OnceType* once, void (*initializer)()) {
  PthreadCall("once", pthread_once(once, initializer));
}

bool LZ4_Compress(const CompressionOptions& opts, const char* input,
                  size_t length, ::std::string* output) {
#ifdef LZ4
  int compressBound = LZ4_compressBound(length);
  output->resize(8 + compressBound);
  char* p = const_cast<char*>(output->c_str());
  memcpy(p, &length, sizeof(length));
  size_t outlen;
  outlen = LZ4_compress_limitedOutput(input, p + 8, length, compressBound);
  if (outlen == 0) {
    return false;
  }
  output->resize(8 + outlen);
  return true;
#endif
  return false;
}

char* LZ4_Uncompress(const char* input_data, size_t input_length,
                     int* decompress_size) {
#ifdef LZ4
  if (input_length < 8) {
    return nullptr;
  }
  int output_len;
  memcpy(&output_len, input_data, sizeof(output_len));
  char* output = new char[output_len];
  *decompress_size = LZ4_decompress_safe_partial(input_data + 8, output, input_length - 8, output_len, output_len);
  if (*decompress_size < 0) {
    delete[] output;
    return nullptr;
  }
  return output;
#endif
  return nullptr;
}

}  // namespace port
}  // namespace rocksdb
