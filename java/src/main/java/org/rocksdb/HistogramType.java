// Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

package org.rocksdb;

/**
 * The types of histogram.
 */
public enum HistogramType {
  /**
   * DB Get.
   */
  DB_GET((byte) 0x0),

  /**
   * DB Write.
   */
  DB_WRITE((byte) 0x1),

  /**
   * Time spent in compaction.
   */
  COMPACTION_TIME((byte) 0x2),

  /**
   * Time spent in setting up sub-compaction.
   */
  SUBCOMPACTION_SETUP_TIME((byte) 0x3),

  /**
   * Time spent in IO during table sync.
   * Measured in microseconds.
   */
  TABLE_SYNC_MICROS((byte) 0x4),

  /**
   * Time spent in IO during compaction of outfile.
   * Measured in microseconds.
   */
  COMPACTION_OUTFILE_SYNC_MICROS((byte) 0x5),

  /**
   * Time spent in IO during WAL file sync.
   * Measured in microseconds.
   */
  WAL_FILE_SYNC_MICROS((byte) 0x6),

  /**
   * Time spent in IO during manifest file sync.
   * Measured in microseconds.
   */
  MANIFEST_FILE_SYNC_MICROS((byte) 0x7),

  /**
   * Time spent in IO during table open.
   * Measured in microseconds.
   */
  TABLE_OPEN_IO_MICROS((byte) 0x8),

  /**
   * DB Multi-Get.
   */
  DB_MULTIGET((byte) 0x9),

  /**
   * Time spent in block reads during compaction.
   * Measured in microseconds.
   */
  READ_BLOCK_COMPACTION_MICROS((byte) 0xA),

  /**
   * Time spent in block reads.
   * Measured in microseconds.
   */
  READ_BLOCK_GET_MICROS((byte) 0xB),

  /**
   * Time spent in raw block writes.
   * Measured in microseconds.
   */
  WRITE_RAW_BLOCK_MICROS((byte) 0xC),

  /**
   * Number of files in a single compaction.
   */
  NUM_FILES_IN_SINGLE_COMPACTION((byte) 0x12),

  /**
   * DB Seek.
   */
  DB_SEEK((byte) 0x13),

  /**
   * Write stall.
   */
  WRITE_STALL((byte) 0x14),

  /**
   * Time spent in SST reads.
   * Measured in microseconds.
   */
  SST_READ_MICROS((byte) 0x15),

  /**
   * The number of sub-compactions actually scheduled during a compaction.
   */
  NUM_SUBCOMPACTIONS_SCHEDULED((byte) 0x16),

  /**
   * Bytes per read.
   * Value size distribution in each operation.
   */
  BYTES_PER_READ((byte) 0x17),

  /**
   * Bytes per write.
   * Value size distribution in each operation.
   */
  BYTES_PER_WRITE((byte) 0x18),

  /**
   * Bytes per Multi-Get.
   * Value size distribution in each operation.
   */
  BYTES_PER_MULTIGET((byte) 0x19),

  /**
   * Number of bytes compressed.
   */
  BYTES_COMPRESSED((byte) 0x1A),

  /**
   * Number of bytes decompressed.
   * Number of bytes is when uncompressed; i.e. before/after respectively
   */
  BYTES_DECOMPRESSED((byte) 0x1B),

  /**
   * Time spent in compression.
   * Measured in nanoseconds.
   */
  COMPRESSION_TIMES_NANOS((byte) 0x1C),

  /**
   * Time spent in decompression.
   * Measured in nanoseconds.
   */
  DECOMPRESSION_TIMES_NANOS((byte) 0x1D),

  /**
   * Number of merge operands for read.
   */
  READ_NUM_MERGE_OPERANDS((byte) 0x1E),

  /**
   * Time spent flushing Memtable to disk.
   */
  FLUSH_TIME((byte) 0x20),

  /**
   * Size of keys written to BlobDB.
   */
  BLOB_DB_KEY_SIZE((byte) 0x21),

  /**
   * Size of values written to BlobDB.
   */
  BLOB_DB_VALUE_SIZE((byte) 0x22),

  /**
   * BlobDB Put/PutWithTTL/PutUntil/Write latency.
   * Measured in microseconds.
   */
  BLOB_DB_WRITE_MICROS((byte) 0x23),

  /**
   * BlobDB Get lagency.
   * Measured in microseconds.
   */
  BLOB_DB_GET_MICROS((byte) 0x24),

  /**
   * BlobDB MultiGet latency.
   * Measured in microseconds.
   */
  BLOB_DB_MULTIGET_MICROS((byte) 0x25),

  /**
   * BlobDB Seek/SeekToFirst/SeekToLast/SeekForPrev latency.
   * Measured in microseconds.
   */
  BLOB_DB_SEEK_MICROS((byte) 0x26),

  /**
   * BlobDB Next latency.
   * Measured in microseconds.
   */
  BLOB_DB_NEXT_MICROS((byte) 0x27),

  /**
   * BlobDB Prev latency.
   * Measured in microseconds.
   */
  BLOB_DB_PREV_MICROS((byte) 0x28),

  /**
   * Blob file write latency.
   * Measured in microseconds.
   */
  BLOB_DB_BLOB_FILE_WRITE_MICROS((byte) 0x29),

  /**
   * Blob file read latency.
   * Measured in microseconds.
   */
  BLOB_DB_BLOB_FILE_READ_MICROS((byte) 0x2A),

  /**
   * Blob file sync latency.
   * Measured in microseconds.
   */
  BLOB_DB_BLOB_FILE_SYNC_MICROS((byte) 0x2B),

  /**
   * BlobDB compression time.
   * Measured in microseconds.
   */
  BLOB_DB_COMPRESSION_MICROS((byte) 0x2D),

  /**
   * BlobDB decompression time.
   * Measured in microseconds.
   */
  BLOB_DB_DECOMPRESSION_MICROS((byte) 0x2E),

  /**
   * Num of Index and Filter blocks read from file system per level in MultiGet
   * request.
   */
  NUM_INDEX_AND_FILTER_BLOCKS_READ_PER_LEVEL((byte) 0x2F),

  /**
   * Num of SST files read from file system per level in MultiGet request.
   */
  NUM_SST_READ_PER_LEVEL((byte) 0x31),

  /**
   * The number of retry in auto resume
   */
  ERROR_HANDLER_AUTORESUME_RETRY_COUNT((byte) 0x32),

  /**
   * Bytes read asynchronously.
   */
  ASYNC_READ_BYTES((byte) 0x33),

  /**
   * Number of bytes read for RocksDB's prefetching contents
   * (as opposed to file system's prefetch)
   * from the end of SST table during block based table open
   */
  TABLE_OPEN_PREFETCH_TAIL_READ_BYTES((byte) 0x39),

  /**
   * File read during flush.
   * Measured in microseconds.
   */
  FILE_READ_FLUSH_MICROS((byte) 0x3A),

  /**
   * File read during compaction.
   * Measured in microseconds.
   */
  FILE_READ_COMPACTION_MICROS((byte) 0x3B),

  /**
   * File read during DB Open.
   * Measured in microseconds.
   */
  FILE_READ_DB_OPEN_MICROS((byte) 0x3C),

  /**
   * File read during DB Get.
   * Measured in microseconds.
   */
  FILE_READ_GET_MICROS((byte) 0x3D),

  /**
   * File read during DB Multi-Get.
   * Measured in microseconds.
   */
  FILE_READ_MULTIGET_MICROS((byte) 0x3E),

  /**
   * File read during DB Iterator.
   * Measured in microseconds.
   */
  FILE_READ_DB_ITERATOR_MICROS((byte) 0x3F),

  /**
   * File read during DB checksum validation.
   * Measured in microseconds.
   */
  FILE_READ_VERIFY_DB_CHECKSUM_MICROS((byte) 0x40),

  /**
   * File read during file checksum validation.
   * Measured in microseconds.
   */
  FILE_READ_VERIFY_FILE_CHECKSUMS_MICROS((byte) 0x41),

  /**
   * Time spent writing SST files.
   * Measured in microseconds.
   */
  SST_WRITE_MICROS((byte) 0x42),

  /**
   * Time spent in writing SST table (currently only block-based table) or blob file for flush.
   * Measured in microseconds.
   */
  FILE_WRITE_FLUSH_MICROS((byte) 0x43),

  /**
   * Time spent in writing SST table (currently only block-based table) for compaction.
   * Measured in microseconds.
   */
  FILE_WRITE_COMPACTION_MICROS((byte) 0x44),

  /**
   * Time spent in writing SST table (currently only block-based table) or blob file for db open.
   * Measured in microseconds.
   */
  FILE_WRITE_DB_OPEN_MICROS((byte) 0x45),

  /**
   * The number of histogram types available.
   */
  HISTOGRAM_ENUM_MAX((byte) 0x46);

  private final byte value;

  HistogramType(final byte value) {
    this.value = value;
  }

  /**
   * Returns the byte value of the enumerations value
   *
   * @return byte representation
   */
  public byte getValue() {
    return value;
  }

  /**
   * Get Histogram type by byte value.
   *
   * @param value byte representation of HistogramType.
   *
   * @return {@link org.rocksdb.HistogramType} instance.
   * @throws java.lang.IllegalArgumentException if an invalid
   *     value is provided.
   */
  public static HistogramType getHistogramType(final byte value) {
    for (final HistogramType histogramType : HistogramType.values()) {
      if (histogramType.getValue() == value) {
        return histogramType;
      }
    }
    throw new IllegalArgumentException(
        "Illegal value provided for HistogramType.");
  }
}
