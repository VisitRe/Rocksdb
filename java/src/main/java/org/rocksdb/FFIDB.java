// Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

package org.rocksdb;

import java.lang.foreign.*;
import java.util.ArrayList;
import java.util.List;
import java.util.Optional;

/**
 * The object on which all FFI RocksDB methods exist. It wraps a JNI-style {@link RocksDB object}
 * as an initial convenience. A fully-fledged FFI RocksDB could and should be made not to depend on
 * that, which will need to happen as and when FFI completely replaces JNI.
 */
public class FFIDB implements AutoCloseable {
  static {
    RocksDB.loadLibrary();
  }

  private final RocksDB rocksDB;
  private final List<ColumnFamilyHandle> columnFamilyHandleList;

  private final ReadOptions readOptions = new ReadOptions();

  private final MemorySession memorySession;
  private final SegmentAllocator segmentAllocator;

  private int pinnedCount = 0;
  private int unpinnedCount = 0;

  public FFIDB(final RocksDB rocksDB, final List<ColumnFamilyHandle> columnFamilyHandleList)
      throws RocksDBException {
    this.rocksDB = rocksDB;
    this.columnFamilyHandleList = new ArrayList<>(columnFamilyHandleList);

    // The allocator is "just the session". This should mean that allocated memory is cleaned using
    // the built in {@link Cleaner} when GC gets to it.
    //
    // We have experimented with a static SegmentAllocator.newNativeArena(memorySession)
    // but that runs out of memory - there's no cleanup, and no explicit free.
    // It's possible to imagine efficiencies like cycling through a pair of native arenas and
    // closing them in turn; that way we deterministically dispose of the memory we allocated.
    //
    // To do this, we would need to worry about {@link MemorySegments} which live beyond the API
    // call frame. At present our {@link OutputSlice} has this lifetime, because it is returned as
    // part of the pinnable slice object in {@code getPinnable()} and passed again to {@link
    // FFIPinnableSlice#reset}. This could be fixed.
    //segmentAllocator = new FFIAllocator();
    memorySession = MemorySession.openConfined();
    segmentAllocator = SegmentAllocator.newNativeArena(memorySession);
  }

  public RocksDB getRocksDB() {
    return this.rocksDB;
  }

  public MemorySegment allocateSegment(final long size) {
    return segmentAllocator.allocate(size);
  }

  public MemorySegment allocateSegment(final MemoryLayout layout) {
    return segmentAllocator.allocate(layout);
  }

  public List<ColumnFamilyHandle> getColumnFamilies() {
    return this.columnFamilyHandleList;
  }

  /**
   * A memory session may or may not be closeable.
   */
  @Override
  public void close() {
    memorySession.close();
    System.err.println("DB pinned count: " + pinnedCount + ", unpinned count: " + unpinnedCount);
  }

  public MemorySegment copy(final String s) {
    return copy(s.getBytes());
  }

  public MemorySegment copy(final byte[] array) {
    final MemorySegment segment = segmentAllocator.allocate(array.length);
    segment.copyFrom(MemorySegment.ofArray(array));
    return segment;
  }

  public record GetBytes(Status.Code code, byte[] value, long size) {
    /**
     * Convert the pinnable slice based result into a byte[]-based result
     * <p/>
     * Reset the pinnable slice returned, as it does not need to be held any more;
     * the caller should not use it after this point as it is not valid.
     *
     * @param slice to copy from
     * @param value to copy to
     * @return an object containing status (and the byte[] if the status is ok)
     * @throws RocksDBException if an error is reported by the underlying RocksDB
     */
    static GetBytes fromPinnable(final GetPinnableSlice slice, final byte[] value)
        throws RocksDBException {
      if (slice.code == Status.Code.Ok) {
        final var pinnableSlice = slice.pinnableSlice().get();
        final var size = pinnableSlice.data().byteSize();
        pinnableSlice.data().asByteBuffer().get(0, value, 0, (int) Math.min(size, value.length));
        pinnableSlice.reset();
        return new GetBytes(slice.code, value, size);
      } else {
        return new GetBytes(slice.code, value, 0);
      }
    }
  }

  public record GetParams(MemorySegment memorySegment) {
    public static GetParams create(final FFIDB dbFFI) throws RocksDBException {
      final GetParams getParams =
          new GetParams(dbFFI.allocateSegment(FFILayout.GetParamsSegment.Layout));

      try {
        // Create a new pinnable slice which we want to use repeatedly
        final Object result = FFIMethod.NewPinnable.invoke(getParams.outputPinnable().address());
        final Status.Code code = Status.Code.values()[(Integer) result];
        if (code == Status.Code.Ok) {
          return getParams;
        }
        throw new RocksDBException(new Status(code, Status.SubCode.None,
            "[Rocks FFI - could not create pinnable slice - no detailed reason provided]"));
      } catch (final Throwable methodException) {
        throw new RocksDBException("Internal error invoking FFI (Java to C++) function call: "
            + methodException.getMessage());
      }
    }

    MemorySegment inputSlice() {
      return memorySegment.asSlice(
          FFILayout.GetParamsSegment.InputStructOffset, FFILayout.InputSlice.Layout.byteSize());
    }

    MemorySegment outputPinnable() {
      return memorySegment.asSlice(FFILayout.GetParamsSegment.PinnableStructOffset,
          FFILayout.PinnableSlice.Layout.byteSize());
    }
  }

  /**
   *
   * @param columnFamilyHandle the column family in which to find the key
   * @param keySegment a segment holding the key to fetch
   * @param value the first {@code value.length} bytes of the value at the key will be copied into
   *     here
   * @return status, including the value if the fetch was successful
   * @throws RocksDBException if there is a problem during the get
   */
  public GetBytes get(final ColumnFamilyHandle columnFamilyHandle, final MemorySegment keySegment,
      final GetParams getParams, final byte[] value) throws RocksDBException {
    return GetBytes.fromPinnable(
        getPinnableSlice(readOptions, columnFamilyHandle, keySegment, getParams), value);
  }

  public GetBytes get(final ColumnFamilyHandle columnFamilyHandle, final MemorySegment keySegment,
      final GetParams getParams) throws RocksDBException {
    final var pinnable = getPinnableSlice(readOptions, columnFamilyHandle, keySegment, getParams);
    byte[] value = null;
    if (pinnable.code == Status.Code.Ok) {
      final var pinnableSlice = pinnable.pinnableSlice().get();
      value = new byte[(int) pinnableSlice.data().byteSize()];
    }
    return GetBytes.fromPinnable(pinnable, value);
  }

  public GetBytes get(final MemorySegment keySegment, final GetParams getParams)
      throws RocksDBException {
    return get(rocksDB.getDefaultColumnFamily(), keySegment, getParams);
  }

  public record GetPinnableSlice(Status.Code code, Optional<FFIPinnableSlice> pinnableSlice) {}

  public GetPinnableSlice getPinnableSlice(
      final MemorySegment keySegment, final GetParams getParams) throws RocksDBException {
    return getPinnableSlice(readOptions, rocksDB.getDefaultColumnFamily(), keySegment, getParams);
  }

  /**
   * Get the value of the supplied key in the indicated column family from RocksDB
   *
   * @param columnFamilyHandle column family containing the value to read
   * @param keySegment of the value to read
   * @return an object wrapping status and (if the status is ok) a pinnable slice referring to the
   *     value of the key
   * @throws RocksDBException if there is a problem during the get
   */
  public GetPinnableSlice getPinnableSlice(final ReadOptions readOptions,
      final ColumnFamilyHandle columnFamilyHandle, final MemorySegment keySegment,
      final GetParams getParams) throws RocksDBException {
    final MemorySegment inputSlice = getParams.inputSlice();
    FFILayout.InputSlice.Data.set(inputSlice, keySegment.address());
    FFILayout.InputSlice.Size.set(inputSlice, keySegment.byteSize());

    final MemorySegment outputPinnable = getParams.outputPinnable();

    final Object result;
    try {
      result = FFIMethod.GetIntoPinnable.invoke(MemoryAddress.ofLong(rocksDB.nativeHandle_),
          MemoryAddress.ofLong(readOptions.nativeHandle_),
          MemoryAddress.ofLong(columnFamilyHandle.nativeHandle_), inputSlice.address(),
          outputPinnable.address());
    } catch (final Throwable methodException) {
      throw new RocksDBException("Internal error invoking FFI (Java to C++) function call: "
          + methodException.getMessage());
    }
    if (!(result instanceof Integer)) {
      throw new RocksDBException("rocksdb_ffi_get.invokeExact returned: " + result);
    }
    final Status.Code code = Status.Code.values()[(Integer) result];
    switch (code) {
      case NotFound -> { return new GetPinnableSlice(code, Optional.empty()); }
      case Ok -> {
        final MemoryAddress data = (MemoryAddress) FFILayout.PinnableSlice.Data.get(outputPinnable);
        final Long size = (Long) FFILayout.PinnableSlice.Size.get(outputPinnable);

        //TODO (AP) Review whether this is the correct session to use
        //The "never closed" global() session may well be correct,
        //because the underlying pinnable slice should get explicitly cleared by us, not by the session
        final MemorySegment valueSegment = MemorySegment.ofAddress(data, size, MemorySession.global());
        final FFIPinnableSlice pinnableSlice = new FFIPinnableSlice(valueSegment, outputPinnable);

        if (pinnableSlice.isPinned()) {
          pinnedCount++;
        } else {
          unpinnedCount++;
        }

        return new GetPinnableSlice(code, Optional.of(pinnableSlice));
      }
      default -> throw new RocksDBException(new Status(code, Status.SubCode.None, "[Rocks FFI - no detailed reason provided]"));
    }
  }

  public record OutputSlice(long outputSize, MemorySegment outputSegment) {}
  public record GetOutputSlice(Status.Code code, Optional<OutputSlice> outputSlice) {}

  public GetOutputSlice getOutputSlice(final MemorySegment outputSegment, final MemorySegment keySegment) throws RocksDBException {
    return getOutputSlice(readOptions, rocksDB.getDefaultColumnFamily(), outputSegment, keySegment);
  }

  public GetOutputSlice getOutputSlice(
      final ReadOptions readOptions,
      final ColumnFamilyHandle columnFamilyHandle, final MemorySegment outputSegment, final MemorySegment keySegment) throws RocksDBException {

    final MemorySegment inputSlice = segmentAllocator.allocate(FFILayout.InputSlice.Layout);
    FFILayout.InputSlice.Data.set(inputSlice, keySegment.address());
    FFILayout.InputSlice.Size.set(
        inputSlice, keySegment.byteSize());

    final MemorySegment outputSlice = segmentAllocator.allocate(FFILayout.OutputSlice.Layout);
    FFILayout.OutputSlice.Data.set(outputSlice, outputSegment.address());
    FFILayout.OutputSlice.Capacity.set(outputSlice, outputSegment.byteSize());
    FFILayout.OutputSlice.Size.set(outputSlice, 0);

    final Object result;
    try {
      result = FFIMethod.GetOutput.invoke(MemoryAddress.ofLong(rocksDB.nativeHandle_),
          MemoryAddress.ofLong(readOptions.nativeHandle_),
          MemoryAddress.ofLong(columnFamilyHandle.nativeHandle_), inputSlice.address(),
          outputSlice.address());
    } catch (final Throwable methodException) {
      throw new RocksDBException("Internal error invoking FFI (Java to C++) function call: "
          + methodException.getMessage());
    }
    if (!(result instanceof Integer)) {
      throw new RocksDBException("rocksdb_ffi_get.invokeExact returned: " + result);
    }
    final Status.Code code = Status.Code.values()[(Integer) result];
    switch (code) {
      case NotFound -> { return new GetOutputSlice(code, Optional.empty()); }
      case Ok -> {
        final Long size = (Long) FFILayout.OutputSlice.Size.get(outputSlice);
        return new GetOutputSlice(code, Optional.of(new OutputSlice(size, outputSegment)));
      }
      default -> throw new RocksDBException(new Status(code, Status.SubCode.None, "[Rocks FFI - no detailed reason provided]"));
    }
  }

  public int identity(final int input) throws RocksDBException {
    try {
      return (int) FFIMethod.Identity.invoke(input);
    } catch (final Throwable methodException) {
      throw new RocksDBException("Internal error invoking FFI (Java to C++) function call: "
          + methodException.getMessage());
    }
  }
}
