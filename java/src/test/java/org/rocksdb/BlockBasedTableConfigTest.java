// Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

package org.rocksdb;

import static org.assertj.core.api.Assertions.assertThat;
import static org.junit.Assert.fail;

import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.Random;
import org.junit.ClassRule;
import org.junit.Ignore;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;

public class BlockBasedTableConfigTest {

  @ClassRule
  public static final RocksNativeLibraryResource ROCKS_NATIVE_LIBRARY_RESOURCE =
      new RocksNativeLibraryResource();

  @Rule public TemporaryFolder dbFolder = new TemporaryFolder();

  @Test
  public void cacheIndexAndFilterBlocks() {
    final BlockBasedTableConfig blockBasedTableConfig = new BlockBasedTableConfig();
    blockBasedTableConfig.setCacheIndexAndFilterBlocks(true);
    assertThat(blockBasedTableConfig.cacheIndexAndFilterBlocks()).
        isTrue();

  }

  @Test
  public void cacheIndexAndFilterBlocksWithHighPriority() {
    final BlockBasedTableConfig blockBasedTableConfig = new BlockBasedTableConfig();
    assertThat(blockBasedTableConfig.cacheIndexAndFilterBlocksWithHighPriority()).
        isTrue();
    blockBasedTableConfig.setCacheIndexAndFilterBlocksWithHighPriority(false);
    assertThat(blockBasedTableConfig.cacheIndexAndFilterBlocksWithHighPriority()).isFalse();
  }

  @Test
  public void pinL0FilterAndIndexBlocksInCache() {
    final BlockBasedTableConfig blockBasedTableConfig = new BlockBasedTableConfig();
    blockBasedTableConfig.setPinL0FilterAndIndexBlocksInCache(true);
    assertThat(blockBasedTableConfig.pinL0FilterAndIndexBlocksInCache()).
        isTrue();
  }

  @Test
  public void pinTopLevelIndexAndFilter() {
    final BlockBasedTableConfig blockBasedTableConfig = new BlockBasedTableConfig();
    blockBasedTableConfig.setPinTopLevelIndexAndFilter(false);
    assertThat(blockBasedTableConfig.pinTopLevelIndexAndFilter()).
        isFalse();
  }

  @Test
  public void indexType() {
    final BlockBasedTableConfig blockBasedTableConfig = new BlockBasedTableConfig();
    assertThat(IndexType.values().length).isEqualTo(4);
    blockBasedTableConfig.setIndexType(IndexType.kHashSearch);
    assertThat(blockBasedTableConfig.indexType()).isEqualTo(IndexType.kHashSearch);
    assertThat(IndexType.valueOf("kBinarySearch")).isNotNull();
    blockBasedTableConfig.setIndexType(IndexType.valueOf("kBinarySearch"));
    assertThat(blockBasedTableConfig.indexType()).isEqualTo(IndexType.kBinarySearch);
  }

  @Test
  public void dataBlockIndexType() {
    final BlockBasedTableConfig blockBasedTableConfig = new BlockBasedTableConfig();
    blockBasedTableConfig.setDataBlockIndexType(DataBlockIndexType.kDataBlockBinaryAndHash);
    assertThat(blockBasedTableConfig.dataBlockIndexType())
        .isEqualTo(DataBlockIndexType.kDataBlockBinaryAndHash);
    blockBasedTableConfig.setDataBlockIndexType(DataBlockIndexType.kDataBlockBinarySearch);
    assertThat(blockBasedTableConfig.dataBlockIndexType())
        .isEqualTo(DataBlockIndexType.kDataBlockBinarySearch);
  }

  @Test
  public void checksumType() {
    final BlockBasedTableConfig blockBasedTableConfig = new BlockBasedTableConfig();
    assertThat(ChecksumType.values().length).isEqualTo(5);
    assertThat(ChecksumType.valueOf("kxxHash")).
        isEqualTo(ChecksumType.kxxHash);
    blockBasedTableConfig.setChecksumType(ChecksumType.kNoChecksum);
    assertThat(blockBasedTableConfig.checksumType()).isEqualTo(ChecksumType.kNoChecksum);
    blockBasedTableConfig.setChecksumType(ChecksumType.kxxHash);
    assertThat(blockBasedTableConfig.checksumType()).isEqualTo(ChecksumType.kxxHash);
    blockBasedTableConfig.setChecksumType(ChecksumType.kxxHash64);
    assertThat(blockBasedTableConfig.checksumType()).isEqualTo(ChecksumType.kxxHash64);
    blockBasedTableConfig.setChecksumType(ChecksumType.kXXH3);
    assertThat(blockBasedTableConfig.checksumType()).isEqualTo(ChecksumType.kXXH3);
  }

  @Test
  public void noBlockCache() {
    final BlockBasedTableConfig blockBasedTableConfig = new BlockBasedTableConfig();
    blockBasedTableConfig.setNoBlockCache(true);
    assertThat(blockBasedTableConfig.noBlockCache()).isTrue();
  }

  @Test
  public void blockCache() {
    try (
        final Cache cache = new LRUCache(17 * 1024 * 1024);
        final Options options = new Options().setTableFormatConfig(
            new BlockBasedTableConfig().setBlockCache(cache))) {
      assertThat(options.tableFactoryName()).isEqualTo("BlockBasedTable");
    }
  }

  @Test
  public void blockCacheIntegration() throws RocksDBException {
    try (final Cache cache = new LRUCache(8 * 1024 * 1024);
         final Statistics statistics = new Statistics()) {
      for (int shard = 0; shard < 8; shard++) {
        try (final Options options =
                 new Options()
                     .setCreateIfMissing(true)
                     .setStatistics(statistics)
                     .setTableFormatConfig(new BlockBasedTableConfig().setBlockCache(cache));
             final RocksDB db =
                 RocksDB.open(options, dbFolder.getRoot().getAbsolutePath() + "/" + shard)) {
          final byte[] key = "some-key".getBytes(StandardCharsets.UTF_8);
          final byte[] value = "some-value".getBytes(StandardCharsets.UTF_8);

          db.put(key, value);
          db.flush(new FlushOptions());
          db.get(key);

          assertThat(statistics.getTickerCount(TickerType.BLOCK_CACHE_ADD)).isEqualTo(shard + 1);
        }
      }
    }
  }

  @Test
  public void persistentCache() throws RocksDBException {
    try (final DBOptions dbOptions = new DBOptions().
        setInfoLogLevel(InfoLogLevel.INFO_LEVEL).
        setCreateIfMissing(true);
        final Logger logger = new Logger(dbOptions) {
      @Override
      protected void log(final InfoLogLevel infoLogLevel, final String logMsg) {
        System.out.println(infoLogLevel.name() + ": " + logMsg);
      }
    }) {
      try (final PersistentCache persistentCache =
               new PersistentCache(Env.getDefault(), dbFolder.getRoot().getPath(), 1024 * 1024 * 100, logger, false);
           final Options options = new Options().setTableFormatConfig(
               new BlockBasedTableConfig().setPersistentCache(persistentCache))) {
        assertThat(options.tableFactoryName()).isEqualTo("BlockBasedTable");
      }
    }
  }

  @Test
  public void blockCacheCompressed() {
    try (final Cache cache = new LRUCache(17 * 1024 * 1024);
         final Options options = new Options().setTableFormatConfig(
        new BlockBasedTableConfig().setBlockCacheCompressed(cache))) {
      assertThat(options.tableFactoryName()).isEqualTo("BlockBasedTable");
    }
  }

  @Ignore("See issue: https://github.com/facebook/rocksdb/issues/4822")
  @Test
  public void blockCacheCompressedIntegration() throws RocksDBException {
    final byte[] key1 = "some-key1".getBytes(StandardCharsets.UTF_8);
    final byte[] key2 = "some-key1".getBytes(StandardCharsets.UTF_8);
    final byte[] key3 = "some-key1".getBytes(StandardCharsets.UTF_8);
    final byte[] key4 = "some-key1".getBytes(StandardCharsets.UTF_8);
    final byte[] value = "some-value".getBytes(StandardCharsets.UTF_8);

    try (final Cache compressedCache = new LRUCache(8 * 1024 * 1024);
         final Statistics statistics = new Statistics()) {

      final BlockBasedTableConfig blockBasedTableConfig = new BlockBasedTableConfig()
          .setNoBlockCache(true)
          .setBlockCache(null)
          .setBlockCacheCompressed(compressedCache)
          .setFormatVersion(4);

      try (final Options options = new Options()
             .setCreateIfMissing(true)
             .setStatistics(statistics)
             .setTableFormatConfig(blockBasedTableConfig)) {

        for (int shard = 0; shard < 8; shard++) {
          try (final FlushOptions flushOptions = new FlushOptions();
               final WriteOptions writeOptions = new WriteOptions();
               final ReadOptions readOptions = new ReadOptions();
               final RocksDB db =
                   RocksDB.open(options, dbFolder.getRoot().getAbsolutePath() + "/" + shard)) {

            db.put(writeOptions, key1, value);
            db.put(writeOptions, key2, value);
            db.put(writeOptions, key3, value);
            db.put(writeOptions, key4, value);
            db.flush(flushOptions);

            db.get(readOptions, key1);
            db.get(readOptions, key2);
            db.get(readOptions, key3);
            db.get(readOptions, key4);

            assertThat(statistics.getTickerCount(TickerType.BLOCK_CACHE_COMPRESSED_ADD)).isEqualTo(shard + 1);
          }
        }
      }
    }
  }

  @Test
  public void blockSize() {
    final BlockBasedTableConfig blockBasedTableConfig = new BlockBasedTableConfig();
    blockBasedTableConfig.setBlockSize(10);
    assertThat(blockBasedTableConfig.blockSize()).isEqualTo(10);
  }

  @Test
  public void blockSizeDeviation() {
    final BlockBasedTableConfig blockBasedTableConfig = new BlockBasedTableConfig();
    blockBasedTableConfig.setBlockSizeDeviation(12);
    assertThat(blockBasedTableConfig.blockSizeDeviation()).
        isEqualTo(12);
  }

  @Test
  public void blockRestartInterval() {
    final BlockBasedTableConfig blockBasedTableConfig = new BlockBasedTableConfig();
    blockBasedTableConfig.setBlockRestartInterval(15);
    assertThat(blockBasedTableConfig.blockRestartInterval()).
        isEqualTo(15);
  }

  @Test
  public void indexBlockRestartInterval() {
    final BlockBasedTableConfig blockBasedTableConfig = new BlockBasedTableConfig();
    blockBasedTableConfig.setIndexBlockRestartInterval(15);
    assertThat(blockBasedTableConfig.indexBlockRestartInterval()).
        isEqualTo(15);
  }

  @Test
  public void metadataBlockSize() {
    final BlockBasedTableConfig blockBasedTableConfig = new BlockBasedTableConfig();
    blockBasedTableConfig.setMetadataBlockSize(1024);
    assertThat(blockBasedTableConfig.metadataBlockSize()).
        isEqualTo(1024);
  }

  @Test
  public void partitionFilters() {
    final BlockBasedTableConfig blockBasedTableConfig = new BlockBasedTableConfig();
    blockBasedTableConfig.setPartitionFilters(true);
    assertThat(blockBasedTableConfig.partitionFilters()).
        isTrue();
  }

  @Test
  public void optimizeFiltersForMemory() {
    final BlockBasedTableConfig blockBasedTableConfig = new BlockBasedTableConfig();
    blockBasedTableConfig.setOptimizeFiltersForMemory(true);
    assertThat(blockBasedTableConfig.optimizeFiltersForMemory()).isTrue();
  }

  @Test
  public void useDeltaEncoding() {
    final BlockBasedTableConfig blockBasedTableConfig = new BlockBasedTableConfig();
    blockBasedTableConfig.setUseDeltaEncoding(false);
    assertThat(blockBasedTableConfig.useDeltaEncoding()).
        isFalse();
  }

  @Test
  public void blockBasedTableWithFilterPolicy() {
    try(final Options options = new Options()
        .setTableFormatConfig(new BlockBasedTableConfig()
            .setFilterPolicy(new BloomFilter(10)))) {
      assertThat(options.tableFactoryName()).
          isEqualTo("BlockBasedTable");
    }
  }

  @Test
  public void blockBasedTableWithoutFilterPolicy() {
    try(final Options options = new Options().setTableFormatConfig(
        new BlockBasedTableConfig().setFilterPolicy(null))) {
      assertThat(options.tableFactoryName()).
          isEqualTo("BlockBasedTable");
    }
  }

  @Test
  public void wholeKeyFiltering() {
    final BlockBasedTableConfig blockBasedTableConfig = new BlockBasedTableConfig();
    blockBasedTableConfig.setWholeKeyFiltering(false);
    assertThat(blockBasedTableConfig.wholeKeyFiltering()).
        isFalse();
  }

  @Test
  public void verifyCompression() {
    final BlockBasedTableConfig blockBasedTableConfig = new BlockBasedTableConfig();
    assertThat(blockBasedTableConfig.verifyCompression()).isFalse();
    blockBasedTableConfig.setVerifyCompression(true);
    assertThat(blockBasedTableConfig.verifyCompression()).
        isTrue();
  }

  @Test
  public void readAmpBytesPerBit() {
    final BlockBasedTableConfig blockBasedTableConfig = new BlockBasedTableConfig();
    blockBasedTableConfig.setReadAmpBytesPerBit(2);
    assertThat(blockBasedTableConfig.readAmpBytesPerBit()).
        isEqualTo(2);
  }

  @Test
  public void formatVersion() {
    final BlockBasedTableConfig blockBasedTableConfig = new BlockBasedTableConfig();
    for (int version = 0; version <= 5; version++) {
      blockBasedTableConfig.setFormatVersion(version);
      assertThat(blockBasedTableConfig.formatVersion()).isEqualTo(version);
    }
  }

  @Test(expected = AssertionError.class)
  public void formatVersionFailNegative() {
    final BlockBasedTableConfig blockBasedTableConfig = new BlockBasedTableConfig();
    blockBasedTableConfig.setFormatVersion(-1);
  }

  @Test(expected = RocksDBException.class)
  public void invalidFormatVersion() throws RocksDBException {
    final BlockBasedTableConfig blockBasedTableConfig =
        new BlockBasedTableConfig().setFormatVersion(99999);

    try (final Options options = new Options().setTableFormatConfig(blockBasedTableConfig);
         final RocksDB db = RocksDB.open(options, dbFolder.getRoot().getAbsolutePath())) {
      fail("Opening the database with an invalid format_version should have raised an exception");
    }
  }

  @Test
  public void enableIndexCompression() {
    final BlockBasedTableConfig blockBasedTableConfig = new BlockBasedTableConfig();
    blockBasedTableConfig.setEnableIndexCompression(false);
    assertThat(blockBasedTableConfig.enableIndexCompression()).
        isFalse();
  }

  @Test
  public void blockAlign() {
    final BlockBasedTableConfig blockBasedTableConfig = new BlockBasedTableConfig();
    blockBasedTableConfig.setBlockAlign(true);
    assertThat(blockBasedTableConfig.blockAlign()).
        isTrue();
  }

  @Test
  public void indexShortening() {
    final BlockBasedTableConfig blockBasedTableConfig = new BlockBasedTableConfig();
    blockBasedTableConfig.setIndexShortening(IndexShorteningMode.kShortenSeparatorsAndSuccessor);
    assertThat(blockBasedTableConfig.indexShortening())
        .isEqualTo(IndexShorteningMode.kShortenSeparatorsAndSuccessor);
  }

  @Deprecated
  @Test
  public void hashIndexAllowCollision() {
    final BlockBasedTableConfig blockBasedTableConfig = new BlockBasedTableConfig();
    blockBasedTableConfig.setHashIndexAllowCollision(false);
    assertThat(blockBasedTableConfig.hashIndexAllowCollision()).
        isTrue();  // NOTE: setHashIndexAllowCollision should do nothing!
  }

  @Deprecated
  @Test
  public void blockCacheSize() {
    final BlockBasedTableConfig blockBasedTableConfig = new BlockBasedTableConfig();
    blockBasedTableConfig.setBlockCacheSize(8 * 1024);
    assertThat(blockBasedTableConfig.blockCacheSize()).
        isEqualTo(8 * 1024);
  }

  @Deprecated
  @Test
  public void blockCacheNumShardBits() {
    final BlockBasedTableConfig blockBasedTableConfig = new BlockBasedTableConfig();
    blockBasedTableConfig.setCacheNumShardBits(5);
    assertThat(blockBasedTableConfig.cacheNumShardBits()).
        isEqualTo(5);
  }

  @Deprecated
  @Test
  public void blockCacheCompressedSize() {
    final BlockBasedTableConfig blockBasedTableConfig = new BlockBasedTableConfig();
    blockBasedTableConfig.setBlockCacheCompressedSize(40);
    assertThat(blockBasedTableConfig.blockCacheCompressedSize()).
        isEqualTo(40);
  }

  @Deprecated
  @Test
  public void blockCacheCompressedNumShardBits() {
    final BlockBasedTableConfig blockBasedTableConfig = new BlockBasedTableConfig();
    blockBasedTableConfig.setBlockCacheCompressedNumShardBits(4);
    assertThat(blockBasedTableConfig.blockCacheCompressedNumShardBits()).
        isEqualTo(4);
  }

  @Test
  public void maxIndexSize() {
    final BlockBasedTableConfig blockBasedTableConfig = new BlockBasedTableConfig();
    blockBasedTableConfig.setMaxIndexSize(1024 * 1024);
    assertThat(blockBasedTableConfig.maxIndexSize()).isEqualTo(1024 * 1024);
  }

  @Test
  public void maxTopLevelIndexRawKeySize() {
    final BlockBasedTableConfig blockBasedTableConfig = new BlockBasedTableConfig();
    blockBasedTableConfig.setMaxTopLevelIndexRawKeySize(2 * 1024 * 1024L);
    assertThat(blockBasedTableConfig.maxTopLevelIndexRawKeySize()).isEqualTo(2 * 1024 * 1024L);
  }

  @Test
  public void maxIndexSizeUse() throws RocksDBException {
    BlockBasedTableConfig blockConfig = new BlockBasedTableConfig();
    blockConfig.setMaxIndexSize(8192);
    blockConfig.setFormatVersion(5);
    blockConfig.setIndexType(IndexType.kBinarySearch);
    String absolutePath = dbFolder.getRoot().getAbsolutePath();
    try (final Options opt =
             new Options().setCreateIfMissing(true).setTableFormatConfig(blockConfig);
         final RocksDB db = RocksDB.open(opt, absolutePath);
         FlushOptions flush = new FlushOptions()) {
      // writing very long keys
      byte[] key = new byte[16384];
      for (int i = 0; i < 3; i++) {
        key[16383] = (byte) i;
        db.put(key, new byte[0]);
      }
      db.flush(flush);
      db.compactRange();

      // Here we have one file as real compaction job did not happen. Let's overwrite to cause
      // recompaction.

      for (int i = 0; i < 3; i++) {
        key[16383] = (byte) i;
        db.put(key, new byte[0]);
      }
      db.flush(flush);
      db.compactRange();

      List<LiveFileMetaData> metadata = db.getLiveFilesMetaData();
      assertThat(metadata.size()).isEqualTo(2);
    }
  }

  @Test
  public void maxTopLevelIndexRawKeySizeUse() throws RocksDBException {
    BlockBasedTableConfig blockConfig = new BlockBasedTableConfig();
    blockConfig.setMaxTopLevelIndexRawKeySize(8192);
    blockConfig.setFormatVersion(5);
    blockConfig.setIndexType(IndexType.kTwoLevelIndexSearch);
    String absolutePath = dbFolder.getRoot().getAbsolutePath();
    try (final Options opt =
             new Options().setCreateIfMissing(true).setTableFormatConfig(blockConfig);
         final RocksDB db = RocksDB.open(opt, absolutePath);
         FlushOptions flush = new FlushOptions()) {
      // writing very long keys
      byte[] key = new byte[16384];
      for (int i = 0; i < 4; i++) {
        key[16383] = (byte) i;
        db.put(key, new byte[0]);
      }
      db.flush(flush);
      db.compactRange();

      // Here we have one file as real compaction job did not happen. Let's overwrite to cause
      // recompaction.

      for (int i = 0; i < 4; i++) {
        key[16383] = (byte) i;
        db.put(key, new byte[0]);
      }
      db.flush(flush);
      db.compactRange();

      List<LiveFileMetaData> metadata = db.getLiveFilesMetaData();
      assertThat(metadata.size()).isEqualTo(2);
    }
  }
}
