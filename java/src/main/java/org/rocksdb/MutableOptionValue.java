// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.
package org.rocksdb;

import static org.rocksdb.AbstractMutableOptions.INT_ARRAY_INT_SEPARATOR;

public abstract class MutableOptionValue<T> {
  /**
   * @return
   * @throws NumberFormatException
   */
  @SuppressWarnings("JavaDoc") abstract double asDouble();
  /**
   * @return
   * @throws NumberFormatException
   */
  @SuppressWarnings("JavaDoc") abstract long asLong();
  /**
   * @return
   * @throws NumberFormatException
   */
  @SuppressWarnings("JavaDoc") abstract int asInt();
  /**
   * @return
   * @throws IllegalStateException
   */
  @SuppressWarnings("JavaDoc") abstract boolean asBoolean();
  /**
   * @return
   * @throws IllegalStateException
   */
  @SuppressWarnings("JavaDoc") abstract int[] asIntArray();
  abstract String asString();
  abstract T asObject();

  private abstract static class MutableOptionValueObject<T> extends MutableOptionValue<T> {
    protected final T value;

    protected MutableOptionValueObject(final T value) {
      this.value = value;
    }

    @Override T asObject() {
      return value;
    }
  }

  @SuppressWarnings("unused")
  static MutableOptionValue<String> fromString(final String s) {
    return new MutableOptionStringValue(s);
  }

  static MutableOptionValue<Double> fromDouble(final double d) {
    return new MutableOptionDoubleValue(d);
  }

  static MutableOptionValue<Long> fromLong(final long d) {
    return new MutableOptionLongValue(d);
  }

  static MutableOptionValue<Integer> fromInt(final int i) {
    return new MutableOptionIntValue(i);
  }

  static MutableOptionValue<Boolean> fromBoolean(final boolean b) {
    return new MutableOptionBooleanValue(b);
  }

  static MutableOptionValue<int[]> fromIntArray(final int[] ix) {
    return new MutableOptionIntArrayValue(ix);
  }

  static <N extends Enum<N>> MutableOptionValue<N> fromEnum(final N value) {
    return new MutableOptionEnumValue<>(value);
  }

  static class MutableOptionStringValue
      extends MutableOptionValueObject<String> {
    MutableOptionStringValue(final String value) {
      super(value);
    }

    /**
     * @return
     * @throws NumberFormatException
     */
    @SuppressWarnings("JavaDoc")
    @Override
    double asDouble() {
      return Double.parseDouble(value);
    }

    /**
     * @return
     * @throws NumberFormatException
     */
    @SuppressWarnings("JavaDoc")
    @Override
    long asLong() {
      return Long.parseLong(value);
    }

    /**
     * @return
     * @throws NumberFormatException
     */
    @SuppressWarnings("JavaDoc")
    @Override
    int asInt() {
      return Integer.parseInt(value);
    }

    /**
     * @return
     * @throws IllegalStateException
     */
    @SuppressWarnings("JavaDoc")
    @Override
    boolean asBoolean() {
      return Boolean.parseBoolean(value);
    }

    /**
     * @return
     * @throws IllegalStateException
     */
    @SuppressWarnings("JavaDoc")
    @Override
    int[] asIntArray() {
      throw new IllegalStateException("String is not applicable as int[]");
    }

    @Override
    String asString() {
      return value;
    }
  }

  static class MutableOptionDoubleValue
      extends MutableOptionValue<Double> {
    private final double value;
    MutableOptionDoubleValue(final double value) {
      this.value = value;
    }

    @Override
    double asDouble() {
      return value;
    }

    /**
     * @return
     * @throws NumberFormatException
     */
    @SuppressWarnings("JavaDoc")
    @Override
    long asLong() {
      return Double.valueOf(value).longValue();
    }

    /**
     * @return
     * @throws NumberFormatException
     */
    @SuppressWarnings("JavaDoc")
    @Override
    int asInt() {
      if(value > Integer.MAX_VALUE || value < Integer.MIN_VALUE) {
        throw new NumberFormatException(
            "double value lies outside the bounds of int");
      }
      return Double.valueOf(value).intValue();
    }

    /**
     * @return
     * @throws IllegalStateException
     */
    @SuppressWarnings("JavaDoc")
    @Override
    boolean asBoolean() {
      throw new IllegalStateException(
          "double is not applicable as boolean");
    }

    /**
     * @return
     * @throws IllegalStateException
     */
    @SuppressWarnings("JavaDoc")
    @Override
    int[] asIntArray() {
      if(value > Integer.MAX_VALUE || value < Integer.MIN_VALUE) {
        throw new NumberFormatException(
            "double value lies outside the bounds of int");
      }
      return new int[] { Double.valueOf(value).intValue() };
    }

    @Override
    String asString() {
      return String.valueOf(value);
    }

    @Override
    Double asObject() {
      return value;
    }
  }

  static class MutableOptionLongValue
      extends MutableOptionValue<Long> {
    private final long value;

    MutableOptionLongValue(final long value) {
      this.value = value;
    }

    @Override
    double asDouble() {
      if (value > Integer.MAX_VALUE || value < Integer.MIN_VALUE) {
        throw new NumberFormatException("long value lies outside the bounds of int");
      }
      return Long.valueOf(value).doubleValue();
    }

    /**
     * @return
     * @throws NumberFormatException
     */
    @SuppressWarnings("JavaDoc")
    @Override
    long asLong() {
      return value;
    }

    /**
     * @return
     * @throws NumberFormatException
     */
    @SuppressWarnings("JavaDoc")
    @Override
    int asInt() {
      if(value > Integer.MAX_VALUE || value < Integer.MIN_VALUE) {
        throw new NumberFormatException(
            "long value lies outside the bounds of int");
      }
      return Long.valueOf(value).intValue();
    }

    /**
     * @return
     * @throws IllegalStateException
     */
    @SuppressWarnings("JavaDoc")
    @Override
    boolean asBoolean() {
      throw new IllegalStateException(
          "long is not applicable as boolean");
    }

    /**
     * @return
     * @throws IllegalStateException
     */
    @SuppressWarnings("JavaDoc")
    @Override
    int[] asIntArray() {
      if(value > Integer.MAX_VALUE || value < Integer.MIN_VALUE) {
        throw new NumberFormatException(
            "long value lies outside the bounds of int");
      }
      return new int[] { Long.valueOf(value).intValue() };
    }

    @Override
    String asString() {
      return String.valueOf(value);
    }

    @Override
    Long asObject() {
      return value;
    }
  }

  static class MutableOptionIntValue
      extends MutableOptionValue<Integer> {
    private final int value;

    MutableOptionIntValue(final int value) {
      this.value = value;
    }

    @Override
    double asDouble() {
      return Integer.valueOf(value).doubleValue();
    }

    /**
     * @return
     * @throws NumberFormatException
     */
    @SuppressWarnings("JavaDoc")
    @Override
    long asLong() {
      return value;
    }

    /**
     * @return
     * @throws NumberFormatException
     */
    @SuppressWarnings("JavaDoc")
    @Override
    int asInt() {
      return value;
    }

    /**
     * @return
     * @throws IllegalStateException
     */
    @SuppressWarnings("JavaDoc")
    @Override
    boolean asBoolean() {
      throw new IllegalStateException("int is not applicable as boolean");
    }

    /**
     * @return
     * @throws IllegalStateException
     */
    @SuppressWarnings("JavaDoc")
    @Override
    int[] asIntArray() {
      return new int[] { value };
    }

    @Override
    String asString() {
      return String.valueOf(value);
    }

    @Override
    Integer asObject() {
      return value;
    }
  }

  static class MutableOptionBooleanValue
      extends MutableOptionValue<Boolean> {
    private final boolean value;

    MutableOptionBooleanValue(final boolean value) {
      this.value = value;
    }

    @Override
    double asDouble() {
      throw new NumberFormatException("boolean is not applicable as double");
    }

    /**
     * @return
     * @throws NumberFormatException
     */
    @SuppressWarnings("JavaDoc")
    @Override
    long asLong() {
      throw new NumberFormatException("boolean is not applicable as Long");
    }

    /**
     * @return
     * @throws NumberFormatException
     */
    @SuppressWarnings("JavaDoc")
    int asInt() {
      throw new NumberFormatException("boolean is not applicable as int");
    }

    @Override
    boolean asBoolean() {
      return value;
    }

    /**
     * @return
     * @throws IllegalStateException
     */
    @SuppressWarnings("JavaDoc")
    @Override
    int[] asIntArray() {
      throw new IllegalStateException("boolean is not applicable as int[]");
    }

    @Override
    String asString() {
      return String.valueOf(value);
    }

    @Override
    Boolean asObject() {
      return value;
    }
  }

  static class MutableOptionIntArrayValue
      extends MutableOptionValueObject<int[]> {
    MutableOptionIntArrayValue(final int[] value) {
      super(value);
    }

    @Override
    double asDouble() {
      throw new NumberFormatException("int[] is not applicable as double");
    }

    /**
     * @return
     * @throws NumberFormatException
     */
    @SuppressWarnings("JavaDoc")
    @Override
    long asLong() {
      throw new NumberFormatException("int[] is not applicable as Long");
    }

    /**
     * @return
     * @throws NumberFormatException
     */
    @SuppressWarnings("JavaDoc")
    @Override
    int asInt() {
      throw new NumberFormatException("int[] is not applicable as int");
    }

    @Override
    boolean asBoolean() {
      throw new NumberFormatException("int[] is not applicable as boolean");
    }

    /**
     * @return
     * @throws IllegalStateException
     */
    @SuppressWarnings("JavaDoc")
    @Override
    int[] asIntArray() {
      return value;
    }

    @Override
    String asString() {
      final StringBuilder builder = new StringBuilder();
      for(int i = 0; i < value.length; i++) {
        builder.append(value[i]);
        if(i + 1 < value.length) {
          builder.append(INT_ARRAY_INT_SEPARATOR);
        }
      }
      return builder.toString();
    }
  }

  static class MutableOptionEnumValue<T extends Enum<T>>
      extends MutableOptionValueObject<T> {

    MutableOptionEnumValue(final T value) {
      super(value);
    }

    /**
     * @return
     * @throws NumberFormatException
     */
    @SuppressWarnings("JavaDoc")
    @Override
    double asDouble() {
      throw new NumberFormatException("Enum is not applicable as double");
    }

    /**
     * @return
     * @throws NumberFormatException
     */
    @SuppressWarnings("JavaDoc")
    @Override
    long asLong() {
      throw new NumberFormatException("Enum is not applicable as long");
    }

    /**
     * @return
     * @throws NumberFormatException
     */
    @SuppressWarnings("JavaDoc")
    @Override
    int asInt() {
      throw new NumberFormatException("Enum is not applicable as int");
    }

    /**
     * @return
     * @throws IllegalStateException
     */
    @SuppressWarnings("JavaDoc")
    @Override
    boolean asBoolean() {
      throw new NumberFormatException("Enum is not applicable as boolean");
    }

    /**
     * @return
     * @throws IllegalStateException
     */
    @SuppressWarnings("JavaDoc")
    @Override
    int[] asIntArray() {
      throw new NumberFormatException("Enum is not applicable as int[]");
    }

    @Override
    String asString() {
      return value.name();
    }
  }

}
