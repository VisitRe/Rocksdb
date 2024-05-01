package org.rocksdb;

import java.nio.ByteBuffer;

public class TypeUtil {

  public static byte[] getInternalKey(byte[] userKey, Options options) {
    return getInternalKeyJni(userKey, userKey.length, options.getNativeHandle());
  }

  public static int getInternalKey(ByteBuffer userKey, ByteBuffer internalKey, Options options) {
    int result;
    if (userKey.isDirect()) {
      if (internalKey.isDirect()) {
        result = getInternalKeyDirect0(userKey, userKey.position(), userKey.remaining(), internalKey,
            internalKey.position(), internalKey.remaining(), options.getNativeHandle());
      } else {
        result = getInternalKeyDirect1(userKey, userKey.position(), userKey.remaining(), internalKey.array(),
            internalKey.arrayOffset() + internalKey.position(), internalKey.remaining(),
            options.getNativeHandle());
      }
    } else {
      if (internalKey.isDirect()) {
        result = getInternalKeyByteArray0(userKey.array(), userKey.arrayOffset() + userKey.position(),
                userKey.remaining(), internalKey, internalKey.position(), internalKey.remaining(),
            options.getNativeHandle());
      } else {
        result = getInternalKeyByteArray1(userKey.array(), userKey.arrayOffset() + userKey.position(),
            userKey.remaining(), internalKey.array(), internalKey.arrayOffset() + internalKey.position(),
            internalKey.remaining(), options.getNativeHandle());
      }
    }
    internalKey.limit(Math.min(internalKey.position() + result, internalKey.limit()));
    return result;
  }

  public static byte[] getInternalKeyForPrev(byte[] userKey, Options options) {
    return getInternalKeyForPrevJni(userKey, userKey.length, options.getNativeHandle());
  }

  public static int getInternalKeyForPrev(ByteBuffer userKey, ByteBuffer internalKey, Options options) {
    int result;
    if (userKey.isDirect()) {
      if (internalKey.isDirect()) {
        result = getInternalKeyDirectForPrev0(userKey, userKey.position(), userKey.remaining(), internalKey,
            internalKey.position(), internalKey.remaining(), options.getNativeHandle());
      } else {
        result = getInternalKeyDirectForPrev1(userKey, userKey.position(), userKey.remaining(), internalKey.array(),
            internalKey.arrayOffset() + internalKey.position(), internalKey.remaining(),
            options.getNativeHandle());
      }
    } else {
      if (internalKey.isDirect()) {
        result = getInternalKeyByteArrayForPrev0(userKey.array(), userKey.arrayOffset() + userKey.position(),
            userKey.remaining(), internalKey, internalKey.position(), internalKey.remaining(),
            options.getNativeHandle());
      } else {
        result = getInternalKeyByteArrayForPrev1(userKey.array(), userKey.arrayOffset() + userKey.position(),
            userKey.remaining(), internalKey.array(), internalKey.arrayOffset() + internalKey.position(),
            internalKey.remaining(), options.getNativeHandle());
      }
    }
    internalKey.limit(Math.min(internalKey.position() + result, internalKey.limit()));
    return result;
  }

  private static native int getInternalKeyDirect0(ByteBuffer userKey, int userKeyOffset, int userKeyLen,
                                                  ByteBuffer internalKey, int internalKeyOffset, int internalKeyLen,
                                                  long optionsHandle);
  private static native int getInternalKeyByteArray0(byte[] userKey, int userKeyOffset, int userKeyLen,
                                                     ByteBuffer internalKey, int internalKeyOffset, int internalKeyLen,
                                                     long optionsHandle);
  private static native int getInternalKeyDirect1(ByteBuffer userKey, int userKeyOffset, int userKeyLen,
                                                  byte[] internalKey, int internalKeyOffset, int internalKeyLen,
                                                  long optionsHandle);
  private static native int getInternalKeyByteArray1(byte[] userKey, int userKeyOffset, int userKeyLen,
                                                     byte[] internalKey, int internalKeyOffset, int internalKeyLen,
                                                     long optionsHandle);
  private static native byte[] getInternalKeyJni(byte[] userKey, int userKeyLen, long optionsHandle);

  private static native int getInternalKeyDirectForPrev0(ByteBuffer userKey, int userKeyOffset, int userKeyLen,
                                                         ByteBuffer internalKey, int internalKeyOffset,
                                                         int internalKeyLen, long optionsHandle);
  private static native int getInternalKeyByteArrayForPrev0(byte[] userKey, int userKeyOffset, int userKeyLen,
                                                            ByteBuffer internalKey, int internalKeyOffset,
                                                            int internalKeyLen, long optionsHandle);
  private static native int getInternalKeyDirectForPrev1(ByteBuffer userKey, int userKeyOffset, int userKeyLen,
                                                         byte[] internalKey, int internalKeyOffset,
                                                         int internalKeyLen, long optionsHandle);
  private static native int getInternalKeyByteArrayForPrev1(byte[] userKey, int userKeyOffset, int userKeyLen,
                                                            byte[] internalKey, int internalKeyOffset,
                                                            int internalKeyLen, long optionsHandle);
  private static native byte[] getInternalKeyForPrevJni(byte[] userKey, int userKeyLen, long optionsHandle);
}
