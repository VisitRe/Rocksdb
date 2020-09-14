// Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

package org.rocksdb;

public class MemTableInfo {
  private final byte[] columnFamilyName;
  private final long firstSeqno;
  private final long earliestSeqno;
  private final long numEntries;
  private final long numDeletes;

  /**
   * Access is private as this will only be constructed from
   * C++ via JNI.
   */
  private MemTableInfo(final byte[] columnFamilyName, final long firstSeqno,
      final long earliestSeqno, final long numEntries, final long numDeletes) {
    this.columnFamilyName = columnFamilyName;
    this.firstSeqno = firstSeqno;
    this.earliestSeqno = earliestSeqno;
    this.numEntries = numEntries;
    this.numDeletes = numDeletes;
  }

  /**
   * Get the name of the column family to which memtable belongs.
   *
   * @return the name of the column family.
   */
  public byte[] getColumnFamilyName() {
    return columnFamilyName;
  }

  /**
   * Get the Sequence number of the first element that was inserted into the
   * memtable.
   *
   * @return the sequence number of the first inserted element.
   */
  public long getFirstSeqno() {
    return firstSeqno;
  }

  /**
   * Get the Sequence number that is guaranteed to be smaller than or equal
   * to the sequence number of any key that could be inserted into this
   * memtable. It can then be assumed that any write with a larger(or equal)
   * sequence number will be present in this memtable or a later memtable.
   *
   * @return the earliest sequence number.
   */
  public long getEarliestSeqno() {
    return earliestSeqno;
  }

  /**
   * Get the total number of entries in memtable.
   *
   * @return the total number of entries.
   */
  public long getNumEntries() {
    return numEntries;
  }

  /**
   * Get the total number of deletes in memtable.
   *
   * @return the total number of deletes.
   */
  public long getNumDeletes() {
    return numDeletes;
  }
}