package org.rocksdb;

import java.nio.ByteBuffer;

/**
 * Class to parse internal key to extract user key, entry type, sequence number.
 */
public class ParsedEntryInfo extends RocksObject {
  protected ParsedEntryInfo() {
    super(newParseEntryInstance());
  }

  @Override
  protected void disposeInternal(final long handle) {
    disposeInternalJni(handle);
  }

  public EntryType getEntryType() {
    assert (isOwningHandle());
    return EntryType.getEntryType(getEntryTypeJni(nativeHandle_));
  }

  public long getSequenceNumber() {
    return getSequenceNumberJni(nativeHandle_);
  }

  public byte[] getUserKey() {
    assert (isOwningHandle());
    return userKeyJni(nativeHandle_);
  }

  public int userKey(final ByteBuffer key) {
    if (key == null) {
      throw new IllegalArgumentException("ByteBuffer parameters must not be null");
    }
    assert (isOwningHandle());
    final int result;
    if (key.isDirect()) {
      result = userKeyDirect(nativeHandle_, key, key.position(), key.remaining());
    } else {
      result = userKeyByteArray(
          nativeHandle_, key.array(), key.arrayOffset() + key.position(), key.remaining());
    }
    key.limit(Math.min(key.position() + result, key.limit()));
    return result;
  }

  public void parseEntry(Options options, byte[] internalKey) {
    if (options == null || internalKey == null) {
      throw new IllegalArgumentException("ByteBuffer and options parameters must not be null");
    }
    assert (isOwningHandle());
    parseEntry(nativeHandle_, options.getNativeHandle(), internalKey, internalKey.length);
  }

  public void parseEntry(Options options, final ByteBuffer internalKey) {
    if (options == null || internalKey == null) {
      throw new IllegalArgumentException("ByteBuffer and options parameters must not be null");
    }
    assert (isOwningHandle());
    if (internalKey.isDirect()) {
      parseEntryDirect(nativeHandle_, options.getNativeHandle(), internalKey,
          internalKey.position(), internalKey.remaining());
    } else {
      parseEntryByteArray(nativeHandle_, options.getNativeHandle(), internalKey.array(),
          internalKey.arrayOffset() + internalKey.position(), internalKey.remaining());
    }
    internalKey.position(internalKey.limit());
  }

  private static native long newParseEntryInstance();

  private static native void parseEntry(
      long handle, long optionsHandle, byte[] buffer, int bufferLen);

  private static native void parseEntryDirect(
      long handle, long optionsHandle, ByteBuffer buffer, int bufferOffset, int bufferLen);
  private static native void parseEntryByteArray(
      long handle, long optionsHandle, byte[] buffer, int bufferOffset, int bufferLen);
  private static native int userKeyDirect(
      long handle, ByteBuffer target, int bufferOffset, int bufferLen);
  private static native int userKeyByteArray(
      long handle, byte[] target, int bufferOffset, int bufferLen);
  private static native byte[] userKeyJni(final long handle);
  private static native long getSequenceNumberJni(final long handle);
  private static native byte getEntryTypeJni(final long handle);
  private static native void disposeInternalJni(final long handle);
}
