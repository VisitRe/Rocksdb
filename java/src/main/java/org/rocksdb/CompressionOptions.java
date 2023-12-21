// Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

package org.rocksdb;

/**
 * Options for Compression
 */
public class CompressionOptions extends RocksObject {
  /**
   * RocksDB's generic default compression level. Internally it'll be translated
   * to the default compression level specific to the library being used.
   */
  public static final int DEFAULT_COMPRESSION_LEVEL = 32_767;

  /**
   * Constructs a new CompressionOptions.
   */
  public CompressionOptions() {
    super(newCompressionOptions());
  }

  /**
   * Set the Window size.
   * Zlib only.
   *
   * @param windowBits the size of the window.
   *
   * @return the reference to the current compression options.
   */
  public CompressionOptions setWindowBits(final int windowBits) {
    setWindowBits(nativeHandle_, windowBits);
    return this;
  }

  /**
   * Get the Window size.
   * Zlib only.
   *
   * @return the size of the window.
   */
  public int windowBits() {
    return windowBits(nativeHandle_);
  }

  /**
   * Compression "level" applicable to zstd, zlib, LZ4, and LZ4HC. Except for
   * {@link #DEFAULT_COMPRESSION_LEVEL}, the meaning of each value depends
   * on the compression algorithm. Decreasing across non-
   * {@link #DEFAULT_COMPRESSION_LEVEL} values will either favor speed over
   * compression ratio or have no effect.
   * <p>
   * In LZ4 specifically, the absolute value of a negative `level` internally
   * configures the `acceleration` parameter. For example, set `level=-10` for
   * `acceleration=10`. This negation is necessary to ensure decreasing `level`
   * values favor speed over compression ratio.
   *
   * @param level the compression level.
   *
   * @return the reference to the current compression options.
   */
  public CompressionOptions setLevel(final int level) {
    setLevel(nativeHandle_, level);
    return this;
  }

  /**
   * Get the Compression "level".
   * <p>
   * See {@link #setLevel(int)}
   *
   * @return the compression level.
   */
  public int level() {
    return level(nativeHandle_);
  }

  /**
   * Set the compression strategy.
   * Zlib only.
   *
   * @param strategy the strategy.
   *
   * @return the reference to the current compression options.
   */
  public CompressionOptions setStrategy(final int strategy) {
    setStrategy(nativeHandle_, strategy);
    return this;
  }

  /**
   * Get the compression strategy.
   * Zlib only.
   *
   * @return the strategy.
   */
  public int strategy() {
    return strategy(nativeHandle_);
  }

  /**
   * Maximum size of dictionary used to prime the compression library. Currently
   * this dictionary will be constructed by sampling the first output file in a
   * subcompaction when the target level is bottommost. This dictionary will be
   * loaded into the compression library before compressing/uncompressing each
   * data block of subsequent files in the subcompaction. Effectively, this
   * improves compression ratios when there are repetitions across data blocks.
   * <p>
   * A value of 0 indicates the feature is disabled.
   * <p>
   * Default: 0.
   *
   * @param maxDictBytes Maximum bytes to use for the dictionary
   *
   * @return the reference to the current options
   */
  public CompressionOptions setMaxDictBytes(final int maxDictBytes) {
    setMaxDictBytes(nativeHandle_, maxDictBytes);
    return this;
  }

  /**
   * Maximum size of dictionary used to prime the compression library.
   *
   * @return The maximum bytes to use for the dictionary
   */
  public int maxDictBytes() {
    return maxDictBytes(nativeHandle_);
  }

  /**
   * Maximum size of training data passed to zstd's dictionary trainer. Using
   * zstd's dictionary trainer can achieve even better compression ratio
   * improvements than using {@link #setMaxDictBytes(int)} alone.
   * <p>
   * The training data will be used to generate a dictionary
   * of {@link #maxDictBytes()}.
   * <p>
   * Default: 0.
   *
   * @param zstdMaxTrainBytes Maximum bytes to use for training ZStd.
   *
   * @return the reference to the current options
   */
  public CompressionOptions setZStdMaxTrainBytes(final int zstdMaxTrainBytes) {
    setZstdMaxTrainBytes(nativeHandle_, zstdMaxTrainBytes);
    return this;
  }

  /**
   * Maximum size of training data passed to zstd's dictionary trainer.
   *
   * @return Maximum bytes to use for training ZStd
   */
  public int zstdMaxTrainBytes() {
    return zstdMaxTrainBytes(nativeHandle_);
  }

  /**
   * When the compression options are set by the user, it will be set to "true".
   * For bottommost_compression_opts, to enable it, user must set enabled=true.
   * Otherwise, bottommost compression will use compression_opts as default
   * compression options.
   * <p>
   * For compression_opts, if compression_opts.enabled=false, it is still
   * used as compression options for compression process.
   * <p>
   * Default: false.
   *
   * @param enabled true to use these compression options
   *     for the bottommost_compression_opts, false otherwise
   *
   * @return the reference to the current options
   */
  public CompressionOptions setEnabled(final boolean enabled) {
    setEnabled(nativeHandle_, enabled);
    return this;
  }

  /**
   * Determine whether these compression options
   * are used for the bottommost_compression_opts.
   *
   * @return true if these compression options are used
   *     for the bottommost_compression_opts, false otherwise
   */
  public boolean enabled() {
    return enabled(nativeHandle_);
  }

  private static native long newCompressionOptions();
  @Override
  protected final void disposeInternal(final long handle) {
    disposeInternalJni(handle);
  }
  private static native void disposeInternalJni(final long handle);

  private static native void setWindowBits(final long handle, final int windowBits);
  private static native int windowBits(final long handle);
  private static native void setLevel(final long handle, final int level);
  private static native int level(final long handle);
  private static native void setStrategy(final long handle, final int strategy);
  private static native int strategy(final long handle);
  private static native void setMaxDictBytes(final long handle, final int maxDictBytes);
  private static native int maxDictBytes(final long handle);
  private static native void setZstdMaxTrainBytes(final long handle, final int zstdMaxTrainBytes);
  private static native int zstdMaxTrainBytes(final long handle);
  private static native void setEnabled(final long handle, final boolean enabled);
  private static native boolean enabled(final long handle);
}
