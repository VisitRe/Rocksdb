package org.rocksdb;

import static org.assertj.core.api.Assertions.assertThat;

import java.beans.BeanInfo;
import java.beans.IntrospectionException;
import java.beans.Introspector;
import java.beans.PropertyDescriptor;
import java.lang.reflect.InvocationTargetException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import org.junit.*;
import org.junit.rules.TemporaryFolder;

public class PerfContextTest {
  @ClassRule
  public static final RocksNativeLibraryResource ROCKS_NATIVE_LIBRARY_RESOURCE =
      new RocksNativeLibraryResource();

  @Rule public TemporaryFolder dbFolder = new TemporaryFolder();

  List<ColumnFamilyDescriptor> cfDescriptors;
  List<ColumnFamilyHandle> columnFamilyHandleList = new ArrayList<>();
  RocksDB db;

  @Before
  public void before() throws RocksDBException {
    cfDescriptors = Arrays.asList(new ColumnFamilyDescriptor(RocksDB.DEFAULT_COLUMN_FAMILY),
        new ColumnFamilyDescriptor("new_cf".getBytes()));
    final DBOptions options =
        new DBOptions().setCreateIfMissing(true).setCreateMissingColumnFamilies(true);

    db = RocksDB.open(
        options, dbFolder.getRoot().getAbsolutePath(), cfDescriptors, columnFamilyHandleList);
  }

  @After
  public void after() {
    for (final ColumnFamilyHandle columnFamilyHandle : columnFamilyHandleList) {
      columnFamilyHandle.close();
    }
    db.close();
  }

  @Test
  public void testReset() {
    db.setPerfLevel(PerfLevel.ENABLE_TIME_AND_CPU_TIME_EXCEPT_FOR_MUTEX);
    PerfContext ctx = db.getPerfContext();
    assertThat(ctx).isNotNull();
    ctx.reset();
  }

  /**
   * Call all properties to check that we don't have problem with UnsatisfiedLinkError.
   */
  @Test
  public void testAllGetters() throws RocksDBException, IntrospectionException,
                                      InvocationTargetException, IllegalAccessException {
    db.setPerfLevel(PerfLevel.ENABLE_TIME_AND_CPU_TIME_EXCEPT_FOR_MUTEX);
    db.put("key".getBytes(), "value".getBytes());
    db.compactRange();
    db.get("key".getBytes());
    PerfContext ctx = db.getPerfContext();

    BeanInfo info = Introspector.getBeanInfo(ctx.getClass(), RocksObject.class);
    for (PropertyDescriptor property : info.getPropertyDescriptors()) {
      if (property.getReadMethod() != null) {
        Object result = property.getReadMethod().invoke(ctx);
        assertThat(result).isNotNull();
        assertThat(result).isInstanceOf(Long.class);
      }
    }
  }

  @Test
  public void testGetBlockReadCpuTime() throws RocksDBException {
    db.setPerfLevel(PerfLevel.ENABLE_TIME_AND_CPU_TIME_EXCEPT_FOR_MUTEX);
    db.put("key".getBytes(), "value".getBytes());
    db.compactRange();
    db.get("key".getBytes());
    PerfContext ctx = db.getPerfContext();
    assertThat(ctx).isNotNull();
    assertThat(ctx.getBlockReadCpuTime()).isGreaterThan(0);
  }

  @Test
  public void testGetPostProcessTime() throws RocksDBException {
    db.setPerfLevel(PerfLevel.ENABLE_TIME_AND_CPU_TIME_EXCEPT_FOR_MUTEX);
    db.put("key".getBytes(), "value".getBytes());
    db.compactRange();
    db.get("key".getBytes());
    PerfContext ctx = db.getPerfContext();
    assertThat(ctx).isNotNull();
    assertThat(ctx.getPostProcessTime()).isGreaterThan(0);
  }
}
