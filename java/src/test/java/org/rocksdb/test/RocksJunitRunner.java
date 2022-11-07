// Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
package org.rocksdb.test;

import static org.rocksdb.test.RocksJunitRunner.RocksJunitListener.Status.*;

import java.io.PrintStream;
import java.text.DecimalFormat;
import java.text.NumberFormat;
import java.util.ArrayList;
import java.util.List;
import org.junit.internal.JUnitSystem;
import org.junit.internal.RealSystem;
import org.junit.runner.JUnitCore;
import org.junit.runner.notification.Failure;

/**
 * Custom Junit Runner to print also Test classes
 * and executed methods to command prompt.
 */
public class RocksJunitRunner {

  /**
   * Listener which overrides default functionality
   * to print class and method to system out.
   */
  static class RocksJunitListener extends org.junit.internal.TextListener {
    private static final NumberFormat secsFormat = new DecimalFormat("###,###.###");

    private final PrintStream writer;

    private String currentClassName;
    private String currentMethodName;
    private Status currentStatus;
    private long currentTestsStartTime = System.currentTimeMillis();
    private int currentTestsCount;
    private int currentTestsIgnoredCount;
    private int currentTestsFailureCount;
    private int currentTestsErrorCount;

    enum Status {
      IGNORED,
      FAILURE,
      ERROR,
      OK
    }

    /**
     * RocksJunitListener constructor
     *
     * @param system JUnitSystem
     */
    public RocksJunitListener(final JUnitSystem system) {
      this(system.out());
    }

    public RocksJunitListener(final PrintStream writer) {
      super(writer);
      this.writer = writer;
    }

    @Override
    public void testRunStarted(final org.junit.runner.Description description) {
      writer.format("Starting RocksJava Tests...%n");
    }

    private void changeCurrentTestClass(final org.junit.runner.Description description) {
      writer.format("%nRunning: %s%n", description.getClassName());
      currentClassName = description.getClassName();
    }

    @SuppressWarnings("CallToSuspiciousStringMethod")
    @Override
    public void testStarted(final org.junit.runner.Description description) {
      if (currentClassName == null) {
        currentTestsStartTime = System.currentTimeMillis();
        changeCurrentTestClass(description);
      } else if (!currentClassName.equals(description.getClassName())) {
        printTestsSummary();
        changeCurrentTestClass(description);
      }

      currentMethodName = description.getMethodName();
      currentStatus = OK;
      currentTestsCount++;
    }

    private void printTestsSummary() {
      // print summary of last test set
      writer.format("Tests run: %d, Failures: %d, Errors: %d, Ignored: %d, Time elapsed: %s sec%n",
          currentTestsCount,
          currentTestsFailureCount,
          currentTestsErrorCount,
          currentTestsIgnoredCount,
          formatSecs(System.currentTimeMillis() - currentTestsStartTime));

      // reset counters
      currentTestsCount = 0;
      currentTestsFailureCount = 0;
      currentTestsErrorCount = 0;
      currentTestsIgnoredCount = 0;
      currentTestsStartTime = System.currentTimeMillis();
    }

    @SuppressWarnings("AccessToNonThreadSafeStaticField")
    private static String formatSecs(final double milliseconds) {
      final double seconds = milliseconds / 1000;
      return secsFormat.format(seconds);
    }

    @Override
    public void testFailure(final Failure failure) {
      if (failure.getException() != null
          && failure.getException() instanceof AssertionError) {
        currentStatus = FAILURE;
        currentTestsFailureCount++;
      } else {
        currentStatus = ERROR;
        currentTestsErrorCount++;
      }
    }

    @Override
    public void testIgnored(final org.junit.runner.Description description) {
      currentStatus = IGNORED;
      currentTestsIgnoredCount++;
    }

    @Override
    public void testFinished(final org.junit.runner.Description description) {
      if(currentStatus == OK) {
        writer.format("\t%s OK%n",currentMethodName);
      } else {
        writer.format("  [%s] %s%n", currentStatus.name(), currentMethodName);
      }
    }

    @Override
    public void testRunFinished(final org.junit.runner.Result result) {
      printTestsSummary();
      super.testRunFinished(result);
    }
  }

  private static final Class<?>[] EMPTY_CLASS_ARRAY = new Class<?>[ 0 ];

  /**
   * Main method to execute tests
   *
   * @param args Test classes as String names
   */
  public static void main(final String[] args){
    final JUnitCore runner = new JUnitCore();
    final JUnitSystem system = new RealSystem();
    runner.addListener(new RocksJunitListener(system));
    try {
      final List<Class<?>> classes = new ArrayList<>();
      for (final String arg : args) {
        classes.add(Class.forName(arg));
      }
      final Class<?>[] clazzes = classes.toArray(EMPTY_CLASS_ARRAY);
      final org.junit.runner.Result result = runner.run(clazzes);
      if(!result.wasSuccessful()) {
        System.exit(-1);
      }
    } catch (final ClassNotFoundException e) {
      e.printStackTrace();
      System.exit(-2);
    }
  }
}
