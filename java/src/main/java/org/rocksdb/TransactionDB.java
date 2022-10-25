// Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

package org.rocksdb;

import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import org.rocksdb.RocksNative;

/**
 * Database with Transaction support
 */
public class TransactionDB extends RocksDB
    implements TransactionalDB<TransactionOptions> {

  private TransactionDBOptions transactionDbOptions_;

  /**
   * Private constructor.
   *
   * @param nativeHandle The native handle of the C++ TransactionDB object
   */
  private TransactionDB(final long nativeHandle) {
    super(nativeHandle);
  }

  /**
   * Open a TransactionDB, similar to {@link RocksDB#open(Options, String)}.
   *
   * @param options {@link org.rocksdb.Options} instance.
   * @param transactionDbOptions {@link org.rocksdb.TransactionDBOptions}
   *     instance.
   * @param path the path to the rocksdb.
   *
   * @return a {@link TransactionDB} instance on success, null if the specified
   *     {@link TransactionDB} can not be opened.
   *
   * @throws RocksDBException if an error occurs whilst opening the database.
   */
  public static TransactionDB open(final Options options,
      final TransactionDBOptions transactionDbOptions, final String path)
      throws RocksDBException {
    final TransactionDB tdb = new TransactionDB(open(options.nativeHandle_,
        transactionDbOptions.nativeHandle_, path));

    // when non-default Options is used, keeping an Options reference
    // in RocksDB can prevent Java to GC during the life-time of
    // the currently-created RocksDB.
    tdb.storeOptionsInstance(options);
    tdb.storeTransactionDbOptions(transactionDbOptions);

    return tdb;
  }

  /**
   * Open a TransactionDB, similar to
   * {@link RocksDB#open(DBOptions, String, List, List)}.
   *
   * @param dbOptions {@link org.rocksdb.DBOptions} instance.
   * @param transactionDbOptions {@link org.rocksdb.TransactionDBOptions}
   *     instance.
   * @param path the path to the rocksdb.
   * @param columnFamilyDescriptors list of column family descriptors
   * @param columnFamilyHandles will be filled with ColumnFamilyHandle instances
   *
   * @return a {@link TransactionDB} instance on success, null if the specified
   *     {@link TransactionDB} can not be opened.
   *
   * @throws RocksDBException if an error occurs whilst opening the database.
   */
  public static TransactionDB open(final DBOptions dbOptions,
      final TransactionDBOptions transactionDbOptions,
      final String path,
      final List<ColumnFamilyDescriptor> columnFamilyDescriptors,
      final List<ColumnFamilyHandle> columnFamilyHandles)
      throws RocksDBException {

    final byte[][] cfNames = new byte[columnFamilyDescriptors.size()][];
    final long[] cfOptionHandles = new long[columnFamilyDescriptors.size()];
    for (int i = 0; i < columnFamilyDescriptors.size(); i++) {
      final ColumnFamilyDescriptor cfDescriptor = columnFamilyDescriptors
          .get(i);
      cfNames[i] = cfDescriptor.getName();
      cfOptionHandles[i] = cfDescriptor.getOptions().nativeHandle_;
    }

    final long[] handles = open(dbOptions.nativeHandle_,
        transactionDbOptions.nativeHandle_, path, cfNames, cfOptionHandles);
    final TransactionDB tdb = new TransactionDB(handles[0]);

    // when non-default Options is used, keeping an Options reference
    // in RocksDB can prevent Java to GC during the life-time of
    // the currently-created RocksDB.
    tdb.storeOptionsInstance(dbOptions);
    tdb.storeTransactionDbOptions(transactionDbOptions);

    for (int i = 1; i < handles.length; i++) {
      columnFamilyHandles.add(new ColumnFamilyHandle(handles[i]));
    }

    return tdb;
  }

  @Override protected native void nativeClose(long nativeReference);
  // TODO (AP) in native, duplicate the closeDatabase() functionality and the reference counted API
  // dance

  @Override
  public Transaction beginTransaction(final WriteOptions writeOptions) {
    return new Transaction(this, beginTransaction(getNative(), writeOptions.nativeHandle_));
  }

  @Override
  public Transaction beginTransaction(final WriteOptions writeOptions,
      final TransactionOptions transactionOptions) {
    return new Transaction(this,
        beginTransaction(
            getNative(), writeOptions.nativeHandle_, transactionOptions.nativeHandle_));
  }

  // TODO(AR) consider having beingTransaction(... oldTransaction) set a
  // reference count inside Transaction, so that we can always call
  // Transaction#close but the object is only disposed when there are as many
  // closes as beginTransaction. Makes the try-with-resources paradigm easier for
  // java developers

  @Override
  public Transaction beginTransaction(final WriteOptions writeOptions,
      final Transaction oldTransaction) {
    final long jtxnHandle = beginTransaction_withOld(
        getNative(), writeOptions.nativeHandle_, oldTransaction.getNative());

    // RocksJava relies on the assumption that
    // we do not allocate a new Transaction object
    // when providing an old_txn
    assert (jtxnHandle == oldTransaction.getNative());

    return oldTransaction;
  }

  @Override
  public Transaction beginTransaction(final WriteOptions writeOptions,
      final TransactionOptions transactionOptions,
      final Transaction oldTransaction) {
    final long jtxn_handle = beginTransaction_withOld(getNative(), writeOptions.nativeHandle_,
        transactionOptions.nativeHandle_, oldTransaction.getNative());

    // RocksJava relies on the assumption that
    // we do not allocate a new Transaction object
    // when providing an old_txn
    assert (jtxn_handle == oldTransaction.getNative());

    return oldTransaction;
  }

  public Transaction getTransactionByName(final String transactionName) {
    final long jtxnHandle = getTransactionByName(getNative(), transactionName);
    if(jtxnHandle == 0) {
      return null;
    }

    final Transaction txn = new Transaction(this, jtxnHandle);

    // this instance doesn't own the underlying C++ object
    // txn.disOwnNativeHandle();
    throw new UnsupportedOperationException("Not implemented in experimental ref-counting");

    // return txn;
  }

  public List<Transaction> getAllPreparedTransactions() {
    final long[] jtxnHandles = getAllPreparedTransactions(getNative());

    final List<Transaction> txns = new ArrayList<>();
    for(final long jtxnHandle : jtxnHandles) {
      final Transaction txn = new Transaction(this, jtxnHandle);

      // this instance doesn't own the underlying C++ object
      // txn.disOwnNativeHandle();

      txns.add(txn);
    }
    throw new UnsupportedOperationException("Not implemented in experimental ref-counting");
    // return txns;
  }

  public static class KeyLockInfo {
    private final String key;
    private final long[] transactionIDs;
    private final boolean exclusive;

    public KeyLockInfo(final String key, final long transactionIDs[],
        final boolean exclusive) {
      this.key = key;
      this.transactionIDs = transactionIDs;
      this.exclusive = exclusive;
    }

    /**
     * Get the key.
     *
     * @return the key
     */
    public String getKey() {
      return key;
    }

    /**
     * Get the Transaction IDs.
     *
     * @return the Transaction IDs.
     */
    public long[] getTransactionIDs() {
      return transactionIDs;
    }

    /**
     * Get the Lock status.
     *
     * @return true if the lock is exclusive, false if the lock is shared.
     */
    public boolean isExclusive() {
      return exclusive;
    }
  }

  /**
   * Returns map of all locks held.
   *
   * @return a map of all the locks held.
   */
  public Map<Long, KeyLockInfo> getLockStatusData() {
    return getLockStatusData(getNative());
  }

  /**
   * Called from C++ native method {@link #getDeadlockInfoBuffer(long)}
   * to construct a DeadlockInfo object.
   *
   * @param transactionID The transaction id
   * @param columnFamilyId The id of the {@link ColumnFamilyHandle}
   * @param waitingKey the key that we are waiting on
   * @param exclusive true if the lock is exclusive, false if the lock is shared
   *
   * @return The waiting transactions
   */
  private DeadlockInfo newDeadlockInfo(
      final long transactionID, final long columnFamilyId,
      final String waitingKey, final boolean exclusive) {
    return new DeadlockInfo(transactionID, columnFamilyId,
        waitingKey, exclusive);
  }

  public static class DeadlockInfo {
    private final long transactionID;
    private final long columnFamilyId;
    private final String waitingKey;
    private final boolean exclusive;

    private DeadlockInfo(final long transactionID, final long columnFamilyId,
      final String waitingKey, final boolean exclusive) {
      this.transactionID = transactionID;
      this.columnFamilyId = columnFamilyId;
      this.waitingKey = waitingKey;
      this.exclusive = exclusive;
    }

    /**
     * Get the Transaction ID.
     *
     * @return the transaction ID
     */
    public long getTransactionID() {
      return transactionID;
    }

    /**
     * Get the Column Family ID.
     *
     * @return The column family ID
     */
    public long getColumnFamilyId() {
      return columnFamilyId;
    }

    /**
     * Get the key that we are waiting on.
     *
     * @return the key that we are waiting on
     */
    public String getWaitingKey() {
      return waitingKey;
    }

    /**
     * Get the Lock status.
     *
     * @return true if the lock is exclusive, false if the lock is shared.
     */
    public boolean isExclusive() {
      return exclusive;
    }
  }

  public static class DeadlockPath {
    final DeadlockInfo[] path;
    final boolean limitExceeded;

    public DeadlockPath(final DeadlockInfo[] path, final boolean limitExceeded) {
      this.path = path;
      this.limitExceeded = limitExceeded;
    }

    public boolean isEmpty() {
      return path.length == 0 && !limitExceeded;
    }
  }

  public DeadlockPath[] getDeadlockInfoBuffer() {
    return getDeadlockInfoBuffer(getNative());
  }

  public void setDeadlockInfoBufferSize(final int targetSize) {
    setDeadlockInfoBufferSize(getNative(), targetSize);
  }

  private void storeTransactionDbOptions(
      final TransactionDBOptions transactionDbOptions) {
    this.transactionDbOptions_ = transactionDbOptions;
  }

  private static native long open(final long optionsHandle,
      final long transactionDbOptionsHandle, final String path)
      throws RocksDBException;
  private static native long[] open(final long dbOptionsHandle,
      final long transactionDbOptionsHandle, final String path,
      final byte[][] columnFamilyNames, final long[] columnFamilyOptions);
  private native static void closeDatabase(final long handle)
      throws RocksDBException;
  private native long beginTransaction(final long handle,
      final long writeOptionsHandle);
  private native long beginTransaction(final long handle,
      final long writeOptionsHandle, final long transactionOptionsHandle);
  private native long beginTransaction_withOld(final long handle,
      final long writeOptionsHandle, final long oldTransactionHandle);
  private native long beginTransaction_withOld(final long handle,
      final long writeOptionsHandle, final long transactionOptionsHandle,
      final long oldTransactionHandle);
  private native long getTransactionByName(final long handle,
      final String name);
  private native long[] getAllPreparedTransactions(final long handle);
  private native Map<Long, KeyLockInfo> getLockStatusData(
      final long handle);
  private native DeadlockPath[] getDeadlockInfoBuffer(final long handle);
  private native void setDeadlockInfoBufferSize(final long handle,
      final int targetSize);
}
