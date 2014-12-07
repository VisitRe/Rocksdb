// Copyright (c) 2014, Facebook, Inc.  All rights reserved.
// This source code is licensed under the BSD-style license found in the
// LICENSE file in the root directory of this source tree. An additional grant
// of patent rights can be found in the PATENTS file in the same directory.
//
// This file implements the "bridge" between Java and C++ for
// rocksdb::Slice.

#include <stdio.h>
#include <stdlib.h>
#include <jni.h>
#include <string>

#include "include/org_rocksdb_AbstractSlice.h"
#include "include/org_rocksdb_Slice.h"
#include "include/org_rocksdb_DirectSlice.h"
#include "rocksdb/slice.h"
#include "rocksjni/portal.h"

/*
 * Class:     org_rocksdb_AbstractSlice
 * Method:    createNewSliceFromString
 * Signature: (Ljava/lang/String;)V
 */
void Java_org_rocksdb_AbstractSlice_createNewSliceFromString(
    JNIEnv* env, jobject jobj, jstring str) {
  const std::string s = rocksdb::JniUtil::copyString(env, str);
  const rocksdb::Slice* slice = new rocksdb::Slice(s);
  rocksdb::AbstractSliceJni::setHandle(env, jobj, slice);
}

/*
 * Class:     org_rocksdb_AbstractSlice
 * Method:    size0
 * Signature: (J)I
 */
jint Java_org_rocksdb_AbstractSlice_size0(
    JNIEnv* env, jobject jobj, jlong handle) {
  const rocksdb::Slice* slice = reinterpret_cast<rocksdb::Slice*>(handle);
  return static_cast<jint>(slice->size());
}

/*
 * Class:     org_rocksdb_AbstractSlice
 * Method:    empty0
 * Signature: (J)Z
 */
jboolean Java_org_rocksdb_AbstractSlice_empty0(
    JNIEnv* env, jobject jobj, jlong handle) {
  const rocksdb::Slice* slice = reinterpret_cast<rocksdb::Slice*>(handle);
  return slice->empty();
}

/*
 * Class:     org_rocksdb_AbstractSlice
 * Method:    toString0
 * Signature: (JZ)Ljava/lang/String;
 */
jstring Java_org_rocksdb_AbstractSlice_toString0(
    JNIEnv* env, jobject jobj, jlong handle, jboolean hex) {
  const rocksdb::Slice* slice = reinterpret_cast<rocksdb::Slice*>(handle);
  const std::string s = slice->ToString(hex);
  return env->NewStringUTF(s.c_str());
}

/*
 * Class:     org_rocksdb_AbstractSlice
 * Method:    compare0
 * Signature: (JJ)I;
 */
jint Java_org_rocksdb_AbstractSlice_compare0(
    JNIEnv* env, jobject jobj, jlong handle, jlong otherHandle) {
  const rocksdb::Slice* slice = reinterpret_cast<rocksdb::Slice*>(handle);
  const rocksdb::Slice* otherSlice =
    reinterpret_cast<rocksdb::Slice*>(otherHandle);
  return slice->compare(*otherSlice);
}

/*
 * Class:     org_rocksdb_AbstractSlice
 * Method:    startsWith0
 * Signature: (JJ)Z;
 */
jboolean Java_org_rocksdb_AbstractSlice_startsWith0(
    JNIEnv* env, jobject jobj, jlong handle, jlong otherHandle) {
  const rocksdb::Slice* slice = reinterpret_cast<rocksdb::Slice*>(handle);
  const rocksdb::Slice* otherSlice =
    reinterpret_cast<rocksdb::Slice*>(otherHandle);
  return slice->starts_with(*otherSlice);
}

/*
 * Class:     org_rocksdb_AbstractSlice
 * Method:    disposeInternal
 * Signature: (J)V
 */
void Java_org_rocksdb_AbstractSlice_disposeInternal(
    JNIEnv* env, jobject jobj, jlong handle) {
  delete reinterpret_cast<rocksdb::Slice*>(handle);
}

/*
 * Class:     org_rocksdb_Slice
 * Method:    createNewSlice0
 * Signature: ([BI)V
 */
void Java_org_rocksdb_Slice_createNewSlice0(
    JNIEnv * env, jobject jobj, jbyteArray data, jint offset) {

  const jsize dataSize = env->GetArrayLength(data);
  const int len = dataSize - offset;
  jbyte* ptrData = new jbyte[len];
  env->GetByteArrayRegion(data, offset, len, ptrData);

  const rocksdb::Slice* slice = new rocksdb::Slice((const char*)ptrData, len);
  rocksdb::AbstractSliceJni::setHandle(env, jobj, slice);
}

/*
 * Class:     org_rocksdb_Slice
 * Method:    createNewSlice1
 * Signature: ([B)V
 */
void Java_org_rocksdb_Slice_createNewSlice1(
    JNIEnv * env, jobject jobj, jbyteArray jdata) {

  const int len = env->GetArrayLength(jdata);

  jboolean isCopy;
  jbyte* ptrData = env->GetByteArrayElements(jdata, &isCopy);
  const char* buf = new char[len];
  memcpy(const_cast<char*>(buf), ptrData, len);

  const rocksdb::Slice* slice =
    new rocksdb::Slice(buf, env->GetArrayLength(jdata));
  rocksdb::AbstractSliceJni::setHandle(env, jobj, slice);

  env->ReleaseByteArrayElements(jdata, ptrData, JNI_ABORT);
  env->DeleteLocalRef(jdata);
  // NOTE: buf will be deleted in the org.rocksdb.Slice#dispose method
}

/*
 * Class:     org_rocksdb_Slice
 * Method:    data0
 * Signature: (J)[B
 */
jbyteArray Java_org_rocksdb_Slice_data0(
    JNIEnv* env, jobject jobj, jlong handle) {
  const rocksdb::Slice* slice = reinterpret_cast<rocksdb::Slice*>(handle);
  const int len = static_cast<int>(slice->size());
  const jbyteArray data = env->NewByteArray(len);
  env->SetByteArrayRegion(data, 0, len,
    reinterpret_cast<jbyte*>(const_cast<char*>(slice->data())));
  return data;
}

/*
 * Class:     org_rocksdb_Slice
 * Method:    disposeInternalBuf
 * Signature: (J)V
 */
void Java_org_rocksdb_Slice_disposeInternalBuf(
    JNIEnv * env, jobject jobj, jlong handle) {
    const rocksdb::Slice* slice = reinterpret_cast<rocksdb::Slice*>(handle);
    delete [] slice->data_;
}

/*
 * Class:     org_rocksdb_DirectSlice
 * Method:    createNewDirectSlice0
 * Signature: (Ljava/nio/ByteBuffer;I)V
 */
void Java_org_rocksdb_DirectSlice_createNewDirectSlice0(
    JNIEnv* env, jobject jobj, jobject data, jint length) {
  const char* ptrData =
    reinterpret_cast<char*>(env->GetDirectBufferAddress(data));
  const rocksdb::Slice* slice = new rocksdb::Slice(ptrData, length);
  rocksdb::AbstractSliceJni::setHandle(env, jobj, slice);
}

/*
 * Class:     org_rocksdb_DirectSlice
 * Method:    createNewDirectSlice1
 * Signature: (Ljava/nio/ByteBuffer;)V
 */
void Java_org_rocksdb_DirectSlice_createNewDirectSlice1(
    JNIEnv* env, jobject jobj, jobject data) {
  const char* ptrData =
    reinterpret_cast<char*>(env->GetDirectBufferAddress(data));
  const rocksdb::Slice* slice = new rocksdb::Slice(ptrData);
  rocksdb::AbstractSliceJni::setHandle(env, jobj, slice);
}

/*
 * Class:     org_rocksdb_DirectSlice
 * Method:    data0
 * Signature: (J)Ljava/lang/Object;
 */
jobject Java_org_rocksdb_DirectSlice_data0(
    JNIEnv* env, jobject jobj, jlong handle) {
  const rocksdb::Slice* slice = reinterpret_cast<rocksdb::Slice*>(handle);
  return env->NewDirectByteBuffer(const_cast<char*>(slice->data()),
    slice->size());
}

/*
 * Class:     org_rocksdb_DirectSlice
 * Method:    get0
 * Signature: (JI)B
 */
jbyte Java_org_rocksdb_DirectSlice_get0(
    JNIEnv* env, jobject jobj, jlong handle, jint offset) {
  rocksdb::Slice* slice = reinterpret_cast<rocksdb::Slice*>(handle);
  return (*slice)[offset];
}

/*
 * Class:     org_rocksdb_DirectSlice
 * Method:    clear0
 * Signature: (J)V
 */
void Java_org_rocksdb_DirectSlice_clear0(
    JNIEnv* env, jobject jobj, jlong handle) {
  rocksdb::Slice* slice = reinterpret_cast<rocksdb::Slice*>(handle);
  delete [] slice->data_;
  slice->clear();
}

/*
 * Class:     org_rocksdb_DirectSlice
 * Method:    removePrefix0
 * Signature: (JI)V
 */
void Java_org_rocksdb_DirectSlice_removePrefix0(
    JNIEnv* env, jobject jobj, jlong handle, jint length) {
  rocksdb::Slice* slice = reinterpret_cast<rocksdb::Slice*>(handle);
  slice->remove_prefix(length);
}
