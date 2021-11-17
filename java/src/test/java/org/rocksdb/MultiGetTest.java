// Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
package org.rocksdb;

import static org.assertj.core.api.Assertions.assertThat;
import static org.assertj.core.api.Assertions.fail;

import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;

public class MultiGetTest {
  @Rule public TemporaryFolder dbFolder = new TemporaryFolder();

  @Test
  public void putNThenMultiGet() throws RocksDBException {
    try (final Options opt = new Options().setCreateIfMissing(true);
         final RocksDB db = RocksDB.open(opt, dbFolder.getRoot().getAbsolutePath())) {
      db.put("key1".getBytes(), "value1ForKey1".getBytes());
      db.put("key2".getBytes(), "value2ForKey2".getBytes());
      db.put("key3".getBytes(), "value3ForKey3".getBytes());
      final List<byte[]> keys =
          Arrays.asList("key1".getBytes(), "key2".getBytes(), "key3".getBytes());
      final List<byte[]> values = db.multiGetAsList(keys);
      assertThat(values.size()).isEqualTo(keys.size());
      assertThat(values.get(0)).isEqualTo("value1ForKey1".getBytes());
      assertThat(values.get(1)).isEqualTo("value2ForKey2".getBytes());
      assertThat(values.get(2)).isEqualTo("value3ForKey3".getBytes());
    }
  }

  private byte[] bufferBytes(final ByteBuffer byteBuffer) {
    byteBuffer.flip();
    final byte[] result = new byte[byteBuffer.limit()];
    byteBuffer.get(result);
    return result;
  }

  @Test
  public void putNThenMultiGetDirect() throws RocksDBException {
    try (final Options opt = new Options().setCreateIfMissing(true);
         final RocksDB db = RocksDB.open(opt, dbFolder.getRoot().getAbsolutePath())) {
      db.put("key1".getBytes(), "value1ForKey1".getBytes());
      db.put("key2".getBytes(), "value2ForKey2".getBytes());
      db.put("key3".getBytes(), "value3ForKey3".getBytes());

      final List<ByteBuffer> keys = new ArrayList<>();
      keys.add(ByteBuffer.allocateDirect(12).put("key1".getBytes()).flip());
      keys.add(ByteBuffer.allocateDirect(12).put("key2".getBytes()).flip());
      keys.add(ByteBuffer.allocateDirect(12).put("key3".getBytes()).flip());
      final List<ByteBuffer> values = new ArrayList<>();
      for (int i = 0; i < keys.size(); i++) {
        values.add(ByteBuffer.allocateDirect(24));
      }

      {
        final List<RocksDB.MultiGetInstance> results = db.multiGetByteBuffers(keys, values);

        assertThat(results.get(0).status.getCode()).isEqualTo(Status.Code.Ok);
        assertThat(results.get(1).status.getCode()).isEqualTo(Status.Code.Ok);
        assertThat(results.get(2).status.getCode()).isEqualTo(Status.Code.Ok);

        assertThat(results.get(0).valueSize).isEqualTo("value1ForKey1".getBytes().length);
        assertThat(results.get(1).valueSize).isEqualTo("value2ForKey2".getBytes().length);
        assertThat(results.get(2).valueSize).isEqualTo("value3ForKey3".getBytes().length);

        assertThat(bufferBytes(results.get(0).value)).isEqualTo("value1ForKey1".getBytes());
        assertThat(bufferBytes(results.get(1).value)).isEqualTo("value2ForKey2".getBytes());
        assertThat(bufferBytes(results.get(2).value)).isEqualTo("value3ForKey3".getBytes());
      }

      {
        final List<RocksDB.MultiGetInstance> results =
            db.multiGetByteBuffers(new ReadOptions(), keys, values);

        assertThat(results.get(0).status.getCode()).isEqualTo(Status.Code.Ok);
        assertThat(results.get(1).status.getCode()).isEqualTo(Status.Code.Ok);
        assertThat(results.get(2).status.getCode()).isEqualTo(Status.Code.Ok);

        assertThat(results.get(0).valueSize).isEqualTo("value1ForKey1".getBytes().length);
        assertThat(results.get(1).valueSize).isEqualTo("value2ForKey2".getBytes().length);
        assertThat(results.get(2).valueSize).isEqualTo("value3ForKey3".getBytes().length);

        assertThat(bufferBytes(results.get(0).value)).isEqualTo("value1ForKey1".getBytes());
        assertThat(bufferBytes(results.get(1).value)).isEqualTo("value2ForKey2".getBytes());
        assertThat(bufferBytes(results.get(2).value)).isEqualTo("value3ForKey3".getBytes());
      }
    }
  }

  @Test
  public void putNThenMultiGetDirectSliced() throws RocksDBException {
    try (final Options opt = new Options().setCreateIfMissing(true);
         final RocksDB db = RocksDB.open(opt, dbFolder.getRoot().getAbsolutePath())) {
      db.put("key1".getBytes(), "value1ForKey1".getBytes());
      db.put("key2".getBytes(), "value2ForKey2".getBytes());
      db.put("key3".getBytes(), "value3ForKey3".getBytes());

      final List<ByteBuffer> keys = new ArrayList<>();
      keys.add(ByteBuffer.allocateDirect(12).put("key2".getBytes()).flip());
      keys.add(ByteBuffer.allocateDirect(12).put("key3".getBytes()).flip());
      keys.add(ByteBuffer.allocateDirect(12).put("prefix1".getBytes()).slice().put("key1".getBytes()).flip());
      final List<ByteBuffer> values = new ArrayList<>();
      for (int i = 0; i < keys.size(); i++) {
        values.add(ByteBuffer.allocateDirect(24));
      }

      {
        final List<RocksDB.MultiGetInstance> results = db.multiGetByteBuffers(keys, values);

        assertThat(results.get(0).status.getCode()).isEqualTo(Status.Code.Ok);
        assertThat(results.get(1).status.getCode()).isEqualTo(Status.Code.Ok);
        assertThat(results.get(2).status.getCode()).isEqualTo(Status.Code.Ok);

        assertThat(results.get(1).valueSize).isEqualTo("value2ForKey2".getBytes().length);
        assertThat(results.get(2).valueSize).isEqualTo("value3ForKey3".getBytes().length);
        assertThat(results.get(0).valueSize).isEqualTo("value1ForKey1".getBytes().length);

        assertThat(bufferBytes(results.get(0).value)).isEqualTo("value2ForKey2".getBytes());
        assertThat(bufferBytes(results.get(1).value)).isEqualTo("value3ForKey3".getBytes());
        assertThat(bufferBytes(results.get(2).value)).isEqualTo("value1ForKey1".getBytes());
      }
    }
  }

  @Test
  public void putNThenMultiGetDirectBadValuesArray() throws RocksDBException {
    try (final Options opt = new Options().setCreateIfMissing(true);
         final RocksDB db = RocksDB.open(opt, dbFolder.getRoot().getAbsolutePath())) {
      db.put("key1".getBytes(), "value1ForKey1".getBytes());
      db.put("key2".getBytes(), "value2ForKey2".getBytes());
      db.put("key3".getBytes(), "value3ForKey3".getBytes());

      final List<ByteBuffer> keys = new ArrayList<>();
      keys.add(ByteBuffer.allocateDirect(12).put("key1".getBytes()).flip());
      keys.add(ByteBuffer.allocateDirect(12).put("key2".getBytes()).flip());
      keys.add(ByteBuffer.allocateDirect(12).put("key3".getBytes()).flip());

      {
        final List<ByteBuffer> values = new ArrayList<>();
        for (int i = 0; i < keys.size(); i++) {
          values.add(ByteBuffer.allocateDirect(24));
        }

        values.remove(0);

        try {
          db.multiGetByteBuffers(keys, values);
          fail("Expected exception when not enough value ByteBuffers supplied");
        } catch (final IllegalArgumentException e) {
          assertThat(e.getMessage()).contains("For each key there must be a corresponding value");
        }
      }

      {
        final List<ByteBuffer> values = new ArrayList<>();
        for (int i = 0; i < keys.size(); i++) {
          values.add(ByteBuffer.allocateDirect(24));
        }

        values.add(ByteBuffer.allocateDirect(24));

        try {
          db.multiGetByteBuffers(keys, values);
          fail("Expected exception when too many value ByteBuffers supplied");
        } catch (final IllegalArgumentException e) {
          assertThat(e.getMessage()).contains("For each key there must be a corresponding value");
        }
      }
    }
  }

  @Test
  public void putNThenMultiGetDirectNondefaultCF() throws RocksDBException {
    try (final Options opt = new Options().setCreateIfMissing(true);
         final RocksDB db = RocksDB.open(opt, dbFolder.getRoot().getAbsolutePath())) {
      final List<ColumnFamilyDescriptor> cfDescriptors =
          List.of(new ColumnFamilyDescriptor("cf0".getBytes()),
              new ColumnFamilyDescriptor("cf1".getBytes()),
              new ColumnFamilyDescriptor("cf2".getBytes()));

      final List<ColumnFamilyHandle> cf = db.createColumnFamilies(cfDescriptors);

      db.put(cf.get(0), "key1".getBytes(), "value1ForKey1".getBytes());
      db.put(cf.get(0), "key2".getBytes(), "value2ForKey2".getBytes());
      db.put(cf.get(0), "key3".getBytes(), "value3ForKey3".getBytes());

      final List<ByteBuffer> keys = new ArrayList<>();
      keys.add(ByteBuffer.allocateDirect(12).put("key1".getBytes()).flip());
      keys.add(ByteBuffer.allocateDirect(12).put("key2".getBytes()).flip());
      keys.add(ByteBuffer.allocateDirect(12).put("key3".getBytes()).flip());
      final List<ByteBuffer> values = new ArrayList<>();
      for (int i = 0; i < keys.size(); i++) {
        values.add(ByteBuffer.allocateDirect(24));
      }

      {
        final List<RocksDB.MultiGetInstance> results = db.multiGetByteBuffers(keys, values);

        assertThat(results.get(0).status.getCode()).isEqualTo(Status.Code.NotFound);
        assertThat(results.get(1).status.getCode()).isEqualTo(Status.Code.NotFound);
        assertThat(results.get(2).status.getCode()).isEqualTo(Status.Code.NotFound);
      }

      {
        final List<ColumnFamilyHandle> columnFamilyHandles = List.of(cf.get(0));
        final List<RocksDB.MultiGetInstance> results =
            db.multiGetByteBuffers(columnFamilyHandles, keys, values);

        assertThat(results.get(0).status.getCode()).isEqualTo(Status.Code.Ok);
        assertThat(results.get(1).status.getCode()).isEqualTo(Status.Code.Ok);
        assertThat(results.get(2).status.getCode()).isEqualTo(Status.Code.Ok);

        assertThat(results.get(0).valueSize).isEqualTo("value1ForKey1".getBytes().length);
        assertThat(results.get(1).valueSize).isEqualTo("value2ForKey2".getBytes().length);
        assertThat(results.get(2).valueSize).isEqualTo("value3ForKey3".getBytes().length);

        assertThat(bufferBytes(results.get(0).value)).isEqualTo("value1ForKey1".getBytes());
        assertThat(bufferBytes(results.get(1).value)).isEqualTo("value2ForKey2".getBytes());
        assertThat(bufferBytes(results.get(2).value)).isEqualTo("value3ForKey3".getBytes());
      }

      {
        final List<ColumnFamilyHandle> columnFamilyHandles =
            List.of(cf.get(0), cf.get(0), cf.get(0));
        final List<RocksDB.MultiGetInstance> results =
            db.multiGetByteBuffers(columnFamilyHandles, keys, values);

        assertThat(results.get(0).status.getCode()).isEqualTo(Status.Code.Ok);
        assertThat(results.get(1).status.getCode()).isEqualTo(Status.Code.Ok);
        assertThat(results.get(2).status.getCode()).isEqualTo(Status.Code.Ok);

        assertThat(results.get(0).valueSize).isEqualTo("value1ForKey1".getBytes().length);
        assertThat(results.get(1).valueSize).isEqualTo("value2ForKey2".getBytes().length);
        assertThat(results.get(2).valueSize).isEqualTo("value3ForKey3".getBytes().length);

        assertThat(bufferBytes(results.get(0).value)).isEqualTo("value1ForKey1".getBytes());
        assertThat(bufferBytes(results.get(1).value)).isEqualTo("value2ForKey2".getBytes());
        assertThat(bufferBytes(results.get(2).value)).isEqualTo("value3ForKey3".getBytes());
      }
    }
  }

  @Test
  public void putNThenMultiGetDirectCFParams() throws RocksDBException {
    try (final Options opt = new Options().setCreateIfMissing(true);
         final RocksDB db = RocksDB.open(opt, dbFolder.getRoot().getAbsolutePath())) {
      db.put("key1".getBytes(), "value1ForKey1".getBytes());
      db.put("key2".getBytes(), "value2ForKey2".getBytes());
      db.put("key3".getBytes(), "value3ForKey3".getBytes());

      final List<ColumnFamilyHandle> columnFamilyHandles = new ArrayList<>();
      columnFamilyHandles.add(db.getDefaultColumnFamily());
      columnFamilyHandles.add(db.getDefaultColumnFamily());

      final List<ByteBuffer> keys = new ArrayList<>();
      keys.add(ByteBuffer.allocateDirect(12).put("key1".getBytes()).flip());
      keys.add(ByteBuffer.allocateDirect(12).put("key2".getBytes()).flip());
      keys.add(ByteBuffer.allocateDirect(12).put("key3".getBytes()).flip());
      final List<ByteBuffer> values = new ArrayList<>();
      for (int i = 0; i < keys.size(); i++) {
        values.add(ByteBuffer.allocateDirect(24));
      }
      try {
        db.multiGetByteBuffers(columnFamilyHandles, keys, values);
        fail("Expected exception when 2 column families supplied");
      } catch (final IllegalArgumentException e) {
        assertThat(e.getMessage()).contains("Wrong number of ColumnFamilyHandle(s) supplied");
      }

      columnFamilyHandles.clear();
      columnFamilyHandles.add(db.getDefaultColumnFamily());
      final List<RocksDB.MultiGetInstance> results =
          db.multiGetByteBuffers(columnFamilyHandles, keys, values);

      assertThat(results.get(0).status.getCode()).isEqualTo(Status.Code.Ok);
      assertThat(results.get(1).status.getCode()).isEqualTo(Status.Code.Ok);
      assertThat(results.get(2).status.getCode()).isEqualTo(Status.Code.Ok);

      assertThat(results.get(0).valueSize).isEqualTo("value1ForKey1".getBytes().length);
      assertThat(results.get(1).valueSize).isEqualTo("value2ForKey2".getBytes().length);
      assertThat(results.get(2).valueSize).isEqualTo("value3ForKey3".getBytes().length);

      assertThat(bufferBytes(results.get(0).value)).isEqualTo("value1ForKey1".getBytes());
      assertThat(bufferBytes(results.get(1).value)).isEqualTo("value2ForKey2".getBytes());
      assertThat(bufferBytes(results.get(2).value)).isEqualTo("value3ForKey3".getBytes());
    }
  }

  @Test
  public void putNThenMultiGetDirectMixedCF() throws RocksDBException {
    try (final Options opt = new Options().setCreateIfMissing(true);
         final RocksDB db = RocksDB.open(opt, dbFolder.getRoot().getAbsolutePath())) {
      final List<ColumnFamilyDescriptor> cfDescriptors =
          List.of(new ColumnFamilyDescriptor("cf0".getBytes()),
              new ColumnFamilyDescriptor("cf1".getBytes()),
              new ColumnFamilyDescriptor("cf2".getBytes()),
              new ColumnFamilyDescriptor("cf3".getBytes()));

      final List<ColumnFamilyHandle> cf = db.createColumnFamilies(cfDescriptors);

      db.put(cf.get(1), "key1".getBytes(), "value1ForKey1".getBytes());
      db.put("key2".getBytes(), "value2ForKey2".getBytes());
      db.put(cf.get(3), "key3".getBytes(), "value3ForKey3".getBytes());

      final List<ByteBuffer> keys = new ArrayList<>();
      keys.add(ByteBuffer.allocateDirect(12).put("key1".getBytes()).flip());
      keys.add(ByteBuffer.allocateDirect(12).put("key2".getBytes()).flip());
      keys.add(ByteBuffer.allocateDirect(12).put("key3".getBytes()).flip());
      final List<ByteBuffer> values = new ArrayList<>();
      for (int i = 0; i < keys.size(); i++) {
        values.add(ByteBuffer.allocateDirect(24));
      }

      {
        final List<ColumnFamilyHandle> columnFamilyHandles = new ArrayList<>();
        columnFamilyHandles.add(db.getDefaultColumnFamily());

        final List<RocksDB.MultiGetInstance> results =
            db.multiGetByteBuffers(columnFamilyHandles, keys, values);

        assertThat(results.get(0).status.getCode()).isEqualTo(Status.Code.NotFound);
        assertThat(results.get(1).status.getCode()).isEqualTo(Status.Code.Ok);
        assertThat(results.get(2).status.getCode()).isEqualTo(Status.Code.NotFound);

        assertThat(results.get(1).valueSize).isEqualTo("value2ForKey2".getBytes().length);

        assertThat(bufferBytes(results.get(1).value)).isEqualTo("value2ForKey2".getBytes());
      }

      {
        final List<ColumnFamilyHandle> columnFamilyHandles = new ArrayList<>();
        columnFamilyHandles.add(cf.get(1));

        final List<RocksDB.MultiGetInstance> results =
            db.multiGetByteBuffers(columnFamilyHandles, keys, values);

        assertThat(results.get(0).status.getCode()).isEqualTo(Status.Code.Ok);
        assertThat(results.get(1).status.getCode()).isEqualTo(Status.Code.NotFound);
        assertThat(results.get(2).status.getCode()).isEqualTo(Status.Code.NotFound);

        assertThat(results.get(0).valueSize).isEqualTo("value2ForKey2".getBytes().length);

        assertThat(bufferBytes(results.get(0).value)).isEqualTo("value1ForKey1".getBytes());
      }

      {
        final List<ColumnFamilyHandle> columnFamilyHandles = new ArrayList<>();
        columnFamilyHandles.add(cf.get(1));
        columnFamilyHandles.add(db.getDefaultColumnFamily());
        columnFamilyHandles.add(cf.get(3));

        final List<RocksDB.MultiGetInstance> results =
            db.multiGetByteBuffers(columnFamilyHandles, keys, values);

        assertThat(results.get(0).status.getCode()).isEqualTo(Status.Code.Ok);
        assertThat(results.get(1).status.getCode()).isEqualTo(Status.Code.Ok);
        assertThat(results.get(2).status.getCode()).isEqualTo(Status.Code.Ok);

        assertThat(results.get(0).valueSize).isEqualTo("value1ForKey1".getBytes().length);
        assertThat(results.get(1).valueSize).isEqualTo("value2ForKey2".getBytes().length);
        assertThat(results.get(2).valueSize).isEqualTo("value3ForKey3".getBytes().length);

        assertThat(bufferBytes(results.get(0).value)).isEqualTo("value1ForKey1".getBytes());
        assertThat(bufferBytes(results.get(1).value)).isEqualTo("value2ForKey2".getBytes());
        assertThat(bufferBytes(results.get(2).value)).isEqualTo("value3ForKey3".getBytes());
      }

      {
        final List<ColumnFamilyHandle> columnFamilyHandles = new ArrayList<>();
        columnFamilyHandles.add(db.getDefaultColumnFamily());
        columnFamilyHandles.add(cf.get(1));
        columnFamilyHandles.add(cf.get(3));

        final List<RocksDB.MultiGetInstance> results =
            db.multiGetByteBuffers(columnFamilyHandles, keys, values);

        assertThat(results.get(0).status.getCode()).isEqualTo(Status.Code.NotFound);
        assertThat(results.get(1).status.getCode()).isEqualTo(Status.Code.NotFound);
        assertThat(results.get(2).status.getCode()).isEqualTo(Status.Code.Ok);

        assertThat(results.get(2).valueSize).isEqualTo("value3ForKey3".getBytes().length);

        assertThat(bufferBytes(results.get(2).value)).isEqualTo("value3ForKey3".getBytes());
      }
    }
  }

  @Test
  public void putNThenMultiGetDirectTruncateCF() throws RocksDBException {
    try (final Options opt = new Options().setCreateIfMissing(true);
         final RocksDB db = RocksDB.open(opt, dbFolder.getRoot().getAbsolutePath())) {
      final List<ColumnFamilyDescriptor> cfDescriptors =
          List.of(new ColumnFamilyDescriptor("cf0".getBytes()));

      final List<ColumnFamilyHandle> cf = db.createColumnFamilies(cfDescriptors);

      db.put(cf.get(0), "key1".getBytes(), "value1ForKey1".getBytes());
      db.put(cf.get(0), "key2".getBytes(), "value2ForKey2WithLotsOfTrailingGarbage".getBytes());
      db.put(cf.get(0), "key3".getBytes(), "value3ForKey3".getBytes());

      final List<ByteBuffer> keys = new ArrayList<>();
      keys.add(ByteBuffer.allocateDirect(12).put("key1".getBytes()).flip());
      keys.add(ByteBuffer.allocateDirect(12).put("key2".getBytes()).flip());
      keys.add(ByteBuffer.allocateDirect(12).put("key3".getBytes()).flip());
      final List<ByteBuffer> values = new ArrayList<>();
      for (int i = 0; i < keys.size(); i++) {
        values.add(ByteBuffer.allocateDirect(24));
      }

      {
        final List<ColumnFamilyHandle> columnFamilyHandles = List.of(cf.get(0));
        final List<RocksDB.MultiGetInstance> results =
            db.multiGetByteBuffers(columnFamilyHandles, keys, values);

        assertThat(results.get(0).status.getCode()).isEqualTo(Status.Code.Ok);
        assertThat(results.get(1).status.getCode()).isEqualTo(Status.Code.Ok);
        assertThat(results.get(2).status.getCode()).isEqualTo(Status.Code.Ok);

        assertThat(results.get(0).valueSize).isEqualTo("value1ForKey1".getBytes().length);
        assertThat(results.get(1).valueSize)
            .isEqualTo("value2ForKey2WithLotsOfTrailingGarbage".getBytes().length);
        assertThat(results.get(2).valueSize).isEqualTo("value3ForKey3".getBytes().length);

        assertThat(bufferBytes(results.get(0).value)).isEqualTo("value1ForKey1".getBytes());
        assertThat(bufferBytes(results.get(1).value))
            .isEqualTo("valu e2Fo rKey 2Wit hLot sOfT".replace(" ", "").getBytes());
        assertThat(bufferBytes(results.get(2).value)).isEqualTo("value3ForKey3".getBytes());
      }
    }
  }
}
