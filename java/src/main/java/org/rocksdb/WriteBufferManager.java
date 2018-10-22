package org.rocksdb;

import org.rocksdb.Cache;

/**
 * Java wrapper over native write_buffer_manager class
 */
public class WriteBufferManager extends RocksObject {
  static {
    RocksDB.loadLibrary();
  }

  /**
   * Construct a new instance of WriteBufferManager.
   *
   * Check <a href="https://github.com/facebook/rocksdb/wiki/Write-Buffer-Manager">
   *     https://github.com/facebook/rocksdb/wiki/Write-Buffer-Manager</a>
   * for more details on when to use it
   *
   * @param bufferSizeBytes buffer size(in bytes) to use for native write_buffer_manager
   * @param cache cache whose memory should be bounded by this write buffer manager
   */
  public WriteBufferManager(final long bufferSizeBytes, final Cache cache){
    super(newWriteBufferManager(bufferSizeBytes, cache.nativeHandle_));
  }

  private native static long newWriteBufferManager(final long bufferSizeBytes, final long cacheHandle);
  @Override
  protected native void disposeInternal(final long handle);
}
