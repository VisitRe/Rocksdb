// Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

package org.rocksdb;


import org.junit.ClassRule;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;

import static org.assertj.core.api.Assertions.assertThat;

public class CheckPointTest {

  @ClassRule
  public static final RocksNativeLibraryResource ROCKS_NATIVE_LIBRARY_RESOURCE =
      new RocksNativeLibraryResource();

  @Rule
  public TemporaryFolder dbFolder = new TemporaryFolder();

  @Rule
  public TemporaryFolder checkpointFolder = new TemporaryFolder();

  @Test
  public void checkPoint() throws RocksDBException {
    try (final Options options = new Options().
        setCreateIfMissing(true)) {

      try (final RocksDB db = RocksDB.open(options,
          dbFolder.getRoot().getAbsolutePath())) {
        db.put("key".getBytes(), "value".getBytes());
        try (final Checkpoint checkpoint = Checkpoint.create(db)) {
          checkpoint.createCheckpoint(checkpointFolder.
              getRoot().getAbsolutePath() + "/snapshot1");
          db.put("key2".getBytes(), "value2".getBytes());
          checkpoint.createCheckpoint(checkpointFolder.
              getRoot().getAbsolutePath() + "/snapshot2");
        }
      }

      try (final RocksDB db = RocksDB.open(options,
          checkpointFolder.getRoot().getAbsolutePath() +
              "/snapshot1")) {
        assertThat(new String(db.get("key".getBytes()))).
            isEqualTo("value");
        assertThat(db.get("key2".getBytes())).isNull();
      }

      try (final RocksDB db = RocksDB.open(options,
          checkpointFolder.getRoot().getAbsolutePath() +
              "/snapshot2")) {
        assertThat(new String(db.get("key".getBytes()))).
            isEqualTo("value");
        assertThat(new String(db.get("key2".getBytes()))).
            isEqualTo("value2");
      }
    }
  }

  @Test(expected = IllegalArgumentException.class)
  public void failIfDbIsNull() {
    try (final Checkpoint checkpoint = Checkpoint.create(null)) {

    }
  }

  @Test(expected = IllegalStateException.class)
  public void failIfDbNotInitialized() throws RocksDBException {
    try (final RocksDB db = RocksDB.open(
        dbFolder.getRoot().getAbsolutePath())) {
      db.close();
      Checkpoint.create(db);
    }
  }

  @Test(expected = RocksDBException.class)
  public void failWithIllegalPath() throws RocksDBException {
    try (final RocksDB db = RocksDB.open(dbFolder.getRoot().getAbsolutePath());
         final Checkpoint checkpoint = Checkpoint.create(db)) {
      checkpoint.createCheckpoint("/Z:///:\\C:\\TZ/-");
    }
  }
}
