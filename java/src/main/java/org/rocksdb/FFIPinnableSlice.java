// Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

package org.rocksdb;

import java.lang.foreign.MemorySegment;

public record FFIPinnableSlice(MemorySegment data, MemorySegment outputSlice) {
  public void reset() throws RocksDBException {
    try {
      if (isPinned()) {
        FFIMethod.ResetPinnable.invoke(outputSlice.address());
        FFILayout.PinnableSlice.IsPinned.set(outputSlice, false);
      }
    } catch (final Throwable methodException) {
      throw new RocksDBException("Internal error invoking FFI (Java to C++) function call: "
          + methodException.getMessage());
    }
  }

  public boolean isPinned() {
    return (Boolean) FFILayout.PinnableSlice.IsPinned.get(outputSlice);
  }
}
