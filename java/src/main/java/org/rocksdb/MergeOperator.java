// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.
// Copyright (c) 2014, Vlad Balan (vlad.gm@gmail.com).  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

package org.rocksdb;

/**
 * MergeOperator holds an operator to be applied when compacting
 * two merge operands held under the same key in order to obtain a single
 * value.
 */
public abstract class MergeOperator extends RocksObject {
  /**
   * Constructs a MergeOperator.
   *
   * @param nativeHandle reference to the value of the C++ pointer pointing to the underlying native
   *     RocksDB C++ MergeOperator.
   */
  protected MergeOperator(final long nativeHandle) {
    super(nativeHandle);
  }
}
