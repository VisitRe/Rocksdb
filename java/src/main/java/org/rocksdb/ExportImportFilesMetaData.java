//  Copyright (c) Meta Platforms, Inc. and affiliates.
//
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

package org.rocksdb;

import java.util.Arrays;
import java.util.List;

/**
 * The metadata that describes a column family.
 */
public class ExportImportFilesMetaData extends RocksObject {
  private ExportImportFilesMetaData(final long nativeHandle) {
    super(nativeHandle);
    // We do not own the native object!
    disOwnNativeHandle();
  }

  @Override protected native void disposeInternal(final long handle);
}
