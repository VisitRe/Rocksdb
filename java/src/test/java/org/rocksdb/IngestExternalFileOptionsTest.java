// Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
// This source code is licensed under the BSD-style license found in the
// LICENSE file in the root directory of this source tree. An additional grant
// of patent rights can be found in the PATENTS file in the same directory.

package org.rocksdb;

import org.junit.ClassRule;
import org.junit.Test;

import java.util.Random;

import static org.assertj.core.api.Assertions.assertThat;

public class IngestExternalFileOptionsTest {
  @ClassRule
  public static final RocksMemoryResource rocksMemoryResource
      = new RocksMemoryResource();

  public static final Random rand =
      PlatformRandomHelper.getPlatformSpecificRandomFactory();

  @Test
  public void createExternalSstFileInfoWithoutParameters() {
    try (final IngestExternalFileOptions options =
        new IngestExternalFileOptions()) {
      assertThat(options).isNotNull();
    }
  }

  @Test
  public void createExternalSstFileInfoWithParameters() {
    final boolean moveFiles = rand.nextBoolean();
    final boolean snapshotConsistency = rand.nextBoolean();
    final boolean allowGlobalSeqNo = rand.nextBoolean();
    final boolean allowBlockingFlush = rand.nextBoolean();
    try (final IngestExternalFileOptions options =
        new IngestExternalFileOptions(moveFiles, snapshotConsistency,
        allowGlobalSeqNo, allowBlockingFlush)) {
      assertThat(options).isNotNull();
      assertThat(options.moveFiles()).isEqualTo(moveFiles);
      assertThat(options.snapshotConsistency()).isEqualTo(snapshotConsistency);
      assertThat(options.allowGlobalSeqNo()).isEqualTo(allowGlobalSeqNo);
      assertThat(options.allowBlockingFlush()).isEqualTo(allowBlockingFlush);
    }
  }

  @Test
  public void moveFiles() {
    try (final IngestExternalFileOptions options =
        new IngestExternalFileOptions()) {
      final boolean moveFiles = rand.nextBoolean();
      options.setMoveFiles(moveFiles);
      assertThat(options.moveFiles()).isEqualTo(moveFiles);
    }
  }

  @Test
  public void snapshotConsistency() {
    try (final IngestExternalFileOptions options =
        new IngestExternalFileOptions()) {
      final boolean snapshotConsistency = rand.nextBoolean();
      options.setSnapshotConsistency(snapshotConsistency);
      assertThat(options.snapshotConsistency()).isEqualTo(snapshotConsistency);
    }
  }

  @Test
  public void allowGlobalSeqNo() {
    try (final IngestExternalFileOptions options =
        new IngestExternalFileOptions()) {
      final boolean allowGlobalSeqNo = rand.nextBoolean();
      options.setAllowGlobalSeqNo(allowGlobalSeqNo);
      assertThat(options.allowGlobalSeqNo()).isEqualTo(allowGlobalSeqNo);
    }
  }

  @Test
  public void allowBlockingFlush() {
    try (final IngestExternalFileOptions options =
        new IngestExternalFileOptions()) {
      final boolean allowBlockingFlush = rand.nextBoolean();
      options.setAllowBlockingFlush(allowBlockingFlush);
      assertThat(options.allowBlockingFlush()).isEqualTo(allowBlockingFlush);
    }
  }
}
