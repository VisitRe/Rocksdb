// Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#pragma once

#include <functional>
#include <memory>
#include <unordered_map>

#include "rocksdb/convenience.h"
#include "rocksdb/rocksdb_namespace.h"
#include "rocksdb/status.h"

namespace ROCKSDB_NAMESPACE {
class OptionTypeInfo;

enum class OptionType {
  kBoolean,
  kInt,
  kInt32T,
  kInt64T,
  kUInt,
  kUInt32T,
  kUInt64T,
  kSizeT,
  kString,
  kDouble,
  kCompactionStyle,
  kCompactionPri,
  kSliceTransform,
  kCompressionType,
  kTableFactory,
  kComparator,
  kCompactionFilter,
  kCompactionFilterFactory,
  kCompactionStopStyle,
  kMergeOperator,
  kMemTableRepFactory,
  kFilterPolicy,
  kFlushBlockPolicyFactory,
  kChecksumType,
  kEncodingType,
  kEnv,
  kEnum,
  kStruct,
  kVector,
  kUnknown,
};

enum class OptionVerificationType {
  kNormal,
  kByName,               // The option is pointer typed so we can only verify
                         // based on it's name.
  kByNameAllowNull,      // Same as kByName, but it also allows the case
                         // where one of them is a nullptr.
  kByNameAllowFromNull,  // Same as kByName, but it also allows the case
                         // where the old option is nullptr.
  kDeprecated,           // The option is no longer used in rocksdb. The RocksDB
                         // OptionsParser will still accept this option if it
                         // happen to exists in some Options file.  However,
                         // the parser will not include it in serialization
                         // and verification processes.
  kAlias,                // This option represents is a name/shortcut for
                         // another option and should not be written or verified
                         // independently
};

enum class OptionTypeFlags : uint32_t {
  kNone = 0x00,  // No flags
  kCompareDefault = 0x0,
  kCompareNever = ConfigOptions::kSanityLevelNone,
  kCompareLoose = ConfigOptions::kSanityLevelLooselyCompatible,
  kCompareExact = ConfigOptions::kSanityLevelExactMatch,

  kMutable = 0x0100,        // Option is mutable
  kDontSerialize = 0x2000,  // Don't serialize the option
};

inline OptionTypeFlags operator|(const OptionTypeFlags &a,
                                 const OptionTypeFlags &b) {
  return static_cast<OptionTypeFlags>(static_cast<uint32_t>(a) |
                                      static_cast<uint32_t>(b));
}

inline OptionTypeFlags operator&(const OptionTypeFlags &a,
                                 const OptionTypeFlags &b) {
  return static_cast<OptionTypeFlags>(static_cast<uint32_t>(a) &
                                      static_cast<uint32_t>(b));
}

// Converts an string into its enumerated value.
// @param type_map Mapping between strings and enum values
// @param type The string representation of the enum
// @param value Returns the enum value represented by the string
// @return true if the string was found in the enum map, false otherwise.
template <typename T>
bool ParseEnum(const std::unordered_map<std::string, T>& type_map,
               const std::string& type, T* value) {
  auto iter = type_map.find(type);
  if (iter != type_map.end()) {
    *value = iter->second;
    return true;
  }
  return false;
}

// Converts an enum into its string representation.
// @param type_map Mapping between strings and enum values
// @param type The enum
// @param value Returned as the string representation of the enum
// @return true if the enum was found in the enum map, false otherwise.
template <typename T>
bool SerializeEnum(const std::unordered_map<std::string, T>& type_map,
                   const T& type, std::string* value) {
  for (const auto& pair : type_map) {
    if (pair.second == type) {
      *value = pair.first;
      return true;
    }
  }
  return false;
}

template <typename T>
Status ParseVector(const ConfigOptions& config_options,
                   const OptionTypeInfo& elem_info, char separator,
                   const std::string& name, const std::string& value,
                   std::vector<T>* result);

template <typename T>
Status SerializeVector(const ConfigOptions& config_options,
                       const OptionTypeInfo& elem_info, char separator,
                       const std::string& name, const std::vector<T>& vec,
                       std::string* value);
template <typename T>
bool VectorsAreEqual(const ConfigOptions& config_options,
                     const OptionTypeInfo& elem_info, const std::string& name,
                     const std::vector<T>& vec1, const std::vector<T>& vec2,
                     std::string* mismatch);

// Function for converting a option string value into its underlying
// representation in "addr"
// On success, Status::OK is returned and addr is set to the parsed form
// On failure, a non-OK status is returned
// @param opts  The ConfigOptions controlling how the value is parsed
// @param name  The name of the options being parsed
// @param value The string representation of the option
// @param addr  Pointer to the object
using ParseFunc = std::function<Status(
    const ConfigOptions& /*opts*/, const std::string& /*name*/,
    const std::string& /*value*/, char* /*addr*/)>;

// Function for converting an option "addr" into its string representation.
// On success, Status::OK is returned and value is the serialized form.
// On failure, a non-OK status is returned
// @param opts  The ConfigOptions controlling how the values are serialized
// @param name  The name of the options being serialized
// @param addr  Pointer to the value being serialized
// @param value The result of the serialization.
using SerializeFunc = std::function<Status(
    const ConfigOptions& /*opts*/, const std::string& /*name*/,
    const char* /*addr*/, std::string* /*value*/)>;

// Function for comparing two option values
// If they are not equal, updates "mismatch" with the name of the bad option
// @param opts  The ConfigOptions controlling how the values are compared
// @param name  The name of the options being compared
// @param addr1 The first address to compare
// @param addr2 The address to compare to
// @param mismatch If the values are not equal, the name of the option that
// first differs
using EqualsFunc = std::function<bool(
    const ConfigOptions& /*opts*/, const std::string& /*name*/,
    const char* /*addr1*/, const char* /*addr2*/, std::string* mismatch)>;

// A struct for storing constant option information such as option name,
// option type, and offset.
class OptionTypeInfo {
 public:
  int offset_;
  int mutable_offset_;

  // A simple "normal", non-mutable Type "type" at offset
  OptionTypeInfo(int offset, OptionType type)
      : offset_(offset),
        mutable_offset_(0),
        parse_func_(nullptr),
        serialize_func_(nullptr),
        equals_func_(nullptr),
        type_(type),
        verification_(OptionVerificationType::kNormal),
        flags_(OptionTypeFlags::kNone) {}

  // A simple "normal", mutable Type "type" at offset
  OptionTypeInfo(int offset, OptionType type, int mutable_offset)
      : offset_(offset),
        mutable_offset_(mutable_offset),
        parse_func_(nullptr),
        serialize_func_(nullptr),
        equals_func_(nullptr),
        type_(type),
        verification_(OptionVerificationType::kNormal),
        flags_(OptionTypeFlags::kMutable) {}

  OptionTypeInfo(int offset, OptionType type,
                 OptionVerificationType verification, OptionTypeFlags flags,
                 int mutable_offset)
      : offset_(offset),
        mutable_offset_(mutable_offset),
        parse_func_(nullptr),
        serialize_func_(nullptr),
        equals_func_(nullptr),
        type_(type),
        verification_(verification),
        flags_(flags) {}

  OptionTypeInfo(int offset, OptionType type,
                 OptionVerificationType verification, OptionTypeFlags flags,
                 int mutable_offset, const ParseFunc& parse_func)
      : offset_(offset),
        mutable_offset_(mutable_offset),
        parse_func_(parse_func),
        serialize_func_(nullptr),
        equals_func_(nullptr),
        type_(type),
        verification_(verification),
        flags_(flags) {}

  OptionTypeInfo(int offset, OptionType type,
                 OptionVerificationType verification, OptionTypeFlags flags,
                 int mutable_offset, const ParseFunc& parse_func,
                 const SerializeFunc& serialize_func,
                 const EqualsFunc& equals_func)
      : offset_(offset),
        mutable_offset_(mutable_offset),
        parse_func_(parse_func),
        serialize_func_(serialize_func),
        equals_func_(equals_func),
        type_(type),
        verification_(verification),
        flags_(flags) {}

  // Creates an OptionTypeInfo for an enum type.  Enums use an additional
  // map to convert the enums to/from their string representation.
  // To create an OptionTypeInfo that is an Enum, one should:
  // - Create a static map of string values to the corresponding enum value
  // - Call this method passing the static map in as a parameter.
  // Note that it is not necessary to add a new OptionType or make any
  // other changes -- the returned object handles parsing, serialiation, and
  // comparisons.
  //
  // @param offset The offset in the option object for this enum
  // @param map The string to enum mapping for this enum
  template <typename T>
  static OptionTypeInfo Enum(
      int offset, const std::unordered_map<std::string, T>* const map) {
    return OptionTypeInfo(
        offset, OptionType::kEnum, OptionVerificationType::kNormal,
        OptionTypeFlags::kNone, 0,
        // Uses the map argument to convert the input string into
        // its corresponding enum value.  If value is found in the map,
        // addr is updated to the corresponding map entry.
        // @return OK if the value is found in the map
        // @return InvalidArgument if the value is not found in the map
        [map](const ConfigOptions&, const std::string& name,
              const std::string& value, char* addr) {
          if (map == nullptr) {
            return Status::NotSupported("No enum mapping ", name);
          } else if (ParseEnum<T>(*map, value, reinterpret_cast<T*>(addr))) {
            return Status::OK();
          } else {
            return Status::InvalidArgument("No mapping for enum ", name);
          }
        },
        // Uses the map argument to convert the input enum into
        // its corresponding string value.  If enum value is found in the map,
        // value is updated to the corresponding string value in the map.
        // @return OK if the enum is found in the map
        // @return InvalidArgument if the enum is not found in the map
        [map](const ConfigOptions&, const std::string& name, const char* addr,
              std::string* value) {
          if (map == nullptr) {
            return Status::NotSupported("No enum mapping ", name);
          } else if (SerializeEnum<T>(*map, (*reinterpret_cast<const T*>(addr)),
                                      value)) {
            return Status::OK();
          } else {
            return Status::InvalidArgument("No mapping for enum ", name);
          }
        },
        // Casts addr1 and addr2 to the enum type and returns true if
        // they are equal, false otherwise.
        [](const ConfigOptions&, const std::string&, const char* addr1,
           const char* addr2, std::string*) {
          return (*reinterpret_cast<const T*>(addr1) ==
                  *reinterpret_cast<const T*>(addr2));
        });
  }  // End OptionTypeInfo::Enum

  // Creates an OptionTypeInfo for a Struct type.  Structs have a
  // map of string-OptionTypeInfo associated with them that describes how
  // to process the object for parsing, serializing, and matching.
  // Structs also have a struct_name, which is the name of the object
  // as registered in the parent map.
  // When processing a struct, the option name can be specified as:
  //   - <struct_name>       Meaning to process the entire struct.
  //   - <struct_name.field> Meaning to process the single field
  //   - <field>             Process the single fields
  // The CompactionOptionsFIFO, CompactionOptionsUniversal, and LRUCacheOptions
  // are all examples of Struct options.
  //
  // To create an OptionTypeInfo that is a Struct, one should:
  // - Create a static map of string-OptionTypeInfo corresponding to the
  //   properties of the object that can be set via the options.
  // - Call this method passing the name and map in as parameters.
  // Note that it is not necessary to add a new OptionType or make any
  // other changes -- the returned object handles parsing, serialization, and
  // comparisons.
  //
  // @param offset The offset in the option object for this enum
  // @param map The string to enum mapping for this enum
  static OptionTypeInfo Struct(
      const std::string& struct_name,
      const std::unordered_map<std::string, OptionTypeInfo>* struct_map,
      int offset, OptionVerificationType verification, OptionTypeFlags flags,
      int mutable_offset) {
    return OptionTypeInfo(
        offset, OptionType::kStruct, verification, flags, mutable_offset,
        // Parses the struct and updates the fields at addr
        [struct_name, struct_map](const ConfigOptions& opts,
                                  const std::string& name,
                                  const std::string& value, char* addr) {
          return ParseStruct(opts, struct_name, struct_map, name, value, addr);
        },
        // Serializes the struct options into value
        [struct_name, struct_map](const ConfigOptions& opts,
                                  const std::string& name, const char* addr,
                                  std::string* value) {
          return SerializeStruct(opts, struct_name, struct_map, name, addr,
                                 value);
        },
        // Compares the struct fields of addr1 and addr2 for equality
        [struct_name, struct_map](const ConfigOptions& opts,
                                  const std::string& name, const char* addr1,
                                  const char* addr2, std::string* mismatch) {
          return StructsAreEqual(opts, struct_name, struct_map, name, addr1,
                                 addr2, mismatch);
        });
  }
  static OptionTypeInfo Struct(
      const std::string& struct_name,
      const std::unordered_map<std::string, OptionTypeInfo>* struct_map,
      int offset, OptionVerificationType verification, OptionTypeFlags flags,
      int mutable_offset, const ParseFunc& parse_func) {
    return OptionTypeInfo(
        offset, OptionType::kStruct, verification, flags, mutable_offset,
        parse_func,
        [struct_name, struct_map](const ConfigOptions& opts,
                                  const std::string& name, const char* addr,
                                  std::string* value) {
          return SerializeStruct(opts, struct_name, struct_map, name, addr,
                                 value);
        },
        [struct_name, struct_map](const ConfigOptions& opts,
                                  const std::string& name, const char* addr1,
                                  const char* addr2, std::string* mismatch) {
          return StructsAreEqual(opts, struct_name, struct_map, name, addr1,
                                 addr2, mismatch);
        });
  }

  template <typename T>
  static OptionTypeInfo Vector(int _offset,
                               OptionVerificationType _verification,
                               OptionTypeFlags _flags, int _mutable_offset,
                               const OptionTypeInfo& elem_info,
                               char separator = ':') {
    return OptionTypeInfo(
        _offset, OptionType::kVector, _verification, _flags, _mutable_offset,
        [elem_info, separator](const ConfigOptions& opts,
                               const std::string& name,
                               const std::string& value, char* addr) {
          auto result = reinterpret_cast<std::vector<T>*>(addr);
          return ParseVector<T>(opts, elem_info, separator, name, value,
                                result);
        },
        [elem_info, separator](const ConfigOptions& opts,
                               const std::string& name, const char* addr,
                               std::string* value) {
          const auto& vec = *(reinterpret_cast<const std::vector<T>*>(addr));
          return SerializeVector<T>(opts, elem_info, separator, name, vec,
                                    value);
        },
        [elem_info](const ConfigOptions& opts, const std::string& name,
                    const char* addr1, const char* addr2,
                    std::string* mismatch) {
          const auto& vec1 = *(reinterpret_cast<const std::vector<T>*>(addr1));
          const auto& vec2 = *(reinterpret_cast<const std::vector<T>*>(addr2));
          return VectorsAreEqual<T>(opts, elem_info, name, vec1, vec2,
                                    mismatch);
        });
  }

  bool IsEnabled(OptionTypeFlags otf) const { return (flags_ & otf) == otf; }

  bool IsMutable() const { return IsEnabled(OptionTypeFlags::kMutable); }

  bool IsDeprecated() const {
    return IsEnabled(OptionVerificationType::kDeprecated);
  }

  // Returns true if the option is marked as an Alias.
  // Aliases are valid options that are parsed but are not converted to strings
  // or compared.
  bool IsAlias() const { return IsEnabled(OptionVerificationType::kAlias); }

  bool IsEnabled(OptionVerificationType ovf) const {
    return verification_ == ovf;
  }

  // Returns the sanity level for comparing the option.
  // If the options should not be compared, returns None
  // If the option has a compare flag, returns it.
  // Otherwise, returns "exact"
  ConfigOptions::SanityLevel GetSanityLevel() const {
    if (IsDeprecated() || IsAlias()) {
      return ConfigOptions::SanityLevel::kSanityLevelNone;
    } else {
      auto match = (flags_ & OptionTypeFlags::kCompareExact);
      if (match == OptionTypeFlags::kCompareDefault) {
        return ConfigOptions::SanityLevel::kSanityLevelExactMatch;
      } else {
        return (ConfigOptions::SanityLevel)match;
      }
    }
  }

  // Returns true if the option should be serialized.
  // Options should be serialized if the are not deprecated, aliases,
  // or marked as "Don't Serialize".
  bool ShouldSerialize() const {
    if (IsDeprecated() || IsAlias()) {
      return false;
    } else if (IsEnabled(OptionTypeFlags::kDontSerialize)) {
      return false;
    } else {
      return true;
    }
  }

  bool IsByName() const {
    return (verification_ == OptionVerificationType::kByName ||
            verification_ == OptionVerificationType::kByNameAllowNull ||
            verification_ == OptionVerificationType::kByNameAllowFromNull);
  }

  bool IsStruct() const { return (type_ == OptionType::kStruct); }

  // Parses the option in "opt_value" according to the rules of this class
  // and updates the value at "opt_addr".
  // On success, Status::OK() is returned.  On failure:
  // NotFound means the opt_name is not valid for this option
  // NotSupported means we do not know how to parse the value for this option
  // InvalidArgument means the opt_value is not valid for this option.
  Status Parse(const ConfigOptions& config_options, const std::string& opt_name,
               const std::string& opt_value, char* opt_addr) const;

  // Serializes the option in "opt_addr" according to the rules of this class
  // into the value at "opt_value".
  Status Serialize(const ConfigOptions& config_options,
                   const std::string& opt_name, const char* opt_addr,
                   std::string* opt_value) const;

  // Compares the "addr1" and "addr2" values according to the rules of this
  // class and returns true if they match.  On a failed match, mismatch is the
  // name of the option that failed to match.
  bool AreEqual(const ConfigOptions& config_options,
                const std::string& opt_name, const char* addr1,
                const char* addr2, std::string* mismatch) const;

  // Used to override the match rules for "ByName" options.
  bool AreEqualByName(const ConfigOptions& config_options,
                      const std::string& opt_name, const char* this_offset,
                      const char* that_offset) const;
  bool AreEqualByName(const ConfigOptions& config_options,
                      const std::string& opt_name, const char* this_ptr,
                      const std::string& that_value) const;

  // Parses the input value according to the map for the struct at opt_addr
  // struct_name is the name of the struct option as registered
  // opt_name is the name of the option being evaluated.  This may
  // be the whole struct or a sub-element of it, based on struct_name and
  // opt_name.
  static Status ParseStruct(
      const ConfigOptions& config_options, const std::string& struct_name,
      const std::unordered_map<std::string, OptionTypeInfo>* map,
      const std::string& opt_name, const std::string& value, char* opt_addr);

  // Serializes the input addr according to the map for the struct to value.
  // struct_name is the name of the struct option as registered
  // opt_name is the name of the option being evaluated.  This may
  // be the whole struct or a sub-element of it
  static Status SerializeStruct(
      const ConfigOptions& config_options, const std::string& struct_name,
      const std::unordered_map<std::string, OptionTypeInfo>* map,
      const std::string& opt_name, const char* opt_addr, std::string* value);

  // Compares the input offsets according to the map for the struct and returns
  // true if they are equivalent, false otherwise.
  // struct_name is the name of the struct option as registered
  // opt_name is the name of the option being evaluated.  This may
  // be the whole struct or a sub-element of it
  static bool StructsAreEqual(
      const ConfigOptions& config_options, const std::string& struct_name,
      const std::unordered_map<std::string, OptionTypeInfo>* map,
      const std::string& opt_name, const char* this_offset,
      const char* that_offset, std::string* mismatch);

  // Finds the entry for the opt_name in the opt_map, returning
  // nullptr if not found.
  // If found, elem_name will be the name of option to find.
  // This may be opt_name, or a substring of opt_name.
  // For "simple" options, opt_name will be equal to elem_name.  Given the
  // opt_name "opt", elem_name will equal "opt".
  // For "embedded" options (like structs), elem_name may be opt_name
  // or a field within the opt_name.  For example, given the struct "struct",
  // and opt_name of "struct.field", elem_name will be "field"
  static const OptionTypeInfo* Find(
      const std::string& opt_name,
      const std::unordered_map<std::string, OptionTypeInfo>& opt_map,
      std::string* elem_name);

  // Returns the next token marked by the delimiter from "opts" after start in
  // token and updates end to point to where that token stops. Delimiters inside
  // of braces are ignored. Returns OK if a token is found and an error if the
  // input opts string is mis-formatted.
  // Given "a=AA;b=BB;" start=2 and delimiter=";", token is "AA" and end points
  // to "b" Given "{a=A;b=B}", the token would be "a=A;b=B"
  //
  // @param opts The string in which to find the next token
  // @param delimiter The delimiter between tokens
  // @param start     The position in opts to start looking for the token
  // @parem ed        Returns the end position in opts of the token
  // @param token     Returns the token
  // @returns OK if a token was found or InvalidArgument if the input is
  // mal-formed.
  static Status NextToken(const std::string& opts, char delimiter, size_t start,
                          size_t* end, std::string* token);

 private:
  // The optional function to convert a string to its representation
  ParseFunc parse_func_;

  // The optional function to convert a value to its string representation
  SerializeFunc serialize_func_;

  // The optional function to match two option values
  EqualsFunc equals_func_;

  OptionType type_;
  OptionVerificationType verification_;
  OptionTypeFlags flags_;
};

// Parses the input value into elements of the result vector.  This method
// will break the input value into the individual tokens (based on the
// separator), where each of those tokens will be parsed based on the rules of
// elem_info. The result vector will be populated with elements based on the
// input tokens. For example, if the value=1:2:3:4:5 and elem_info parses
// integers, the result vector will contain the integers 1,2,3,4,5
// @param config_options Controls how the option value is parsed.
// @param elem_info Controls how individual tokens in value are parsed
// @param separator Character separating tokens in values (':' in the above
// example)
// @param name      The name associated with this vector option
// @param value     The input string to parse into tokens
// @param result    Returns the results of parsing value into its elements.
// @return OK if the value was successfully parse
// @return InvalidArgument if the value is improperly formed or if the token
//                          could not be parsed
// @return NotFound         If the tokenized value contains unknown options for
// its type
template <typename T>
Status ParseVector(const ConfigOptions& config_options,
                   const OptionTypeInfo& elem_info, char separator,
                   const std::string& name, const std::string& value,
                   std::vector<T>* result) {
  result->clear();
  Status status;

  for (size_t start = 0, end = 0;
       status.ok() && start < value.size() && end != std::string::npos;
       start = end + 1) {
    std::string token;
    status = OptionTypeInfo::NextToken(value, separator, start, &end, &token);
    if (status.ok()) {
      T elem;
      status = elem_info.Parse(config_options, name, token,
                               reinterpret_cast<char*>(&elem));
      if (status.ok()) {
        result->emplace_back(elem);
      }
    }
  }
  return status;
}

// Serializes the input vector into its output value.  Elements are
// separated by the separator character.  This element will convert all of the
// elements in vec into their serialized form, using elem_info to perform the
// serialization.
// For example, if the vec contains the integers 1,2,3,4,5 and elem_info
// serializes the output would be 1:2:3:4:5 for separator ":".
// @param config_options Controls how the option value is serialized.
// @param elem_info Controls how individual tokens in value are serialized
// @param separator Character separating tokens in value (':' in the above
// example)
// @param name      The name associated with this vector option
// @param vec       The input vector to serialize
// @param value     The output string of serialized options
// @return OK if the value was successfully parse
// @return InvalidArgument if the value is improperly formed or if the token
//                          could not be parsed
// @return NotFound         If the tokenized value contains unknown options for
// its type
template <typename T>
Status SerializeVector(const ConfigOptions& config_options,
                       const OptionTypeInfo& elem_info, char separator,
                       const std::string& name, const std::vector<T>& vec,
                       std::string* value) {
  std::string result;
  ConfigOptions embedded = config_options;
  embedded.delimiter = ";";
  for (size_t i = 0; i < vec.size(); ++i) {
    std::string elem_str;
    Status s = elem_info.Serialize(
        embedded, name, reinterpret_cast<const char*>(&vec[i]), &elem_str);
    if (!s.ok()) {
      return s;
    } else {
      if (i > 0) {
        result += separator;
      }
      result += elem_str;
    }
  }
  if (result.find("=") != std::string::npos) {
    *value = "{" + result + "}";
  } else {
    *value = result;
  }
  return Status::OK();
}

// Compares the input vectors vec1 and vec2 for equality
// If the vectors are the same size, elements of the vectors are compared one by
// one using elem_info to perform the comparison.
//
// @param config_options Controls how the vectors are compared.
// @param elem_info Controls how individual elements in the vectors are compared
// @param name      The name associated with this vector option
// @param vec1,vec2 The vectors to compare.
// @param mismatch  If the vectors are not equivalent, mismatch will point to
// the first
//                  element of the comparison tht did not match.
// @return true     If vec1 and vec2 are "equal", false otherwise
template <typename T>
bool VectorsAreEqual(const ConfigOptions& config_options,
                     const OptionTypeInfo& elem_info, const std::string& name,
                     const std::vector<T>& vec1, const std::vector<T>& vec2,
                     std::string* mismatch) {
  if (vec1.size() != vec2.size()) {
    *mismatch = name;
    return false;
  } else {
    for (size_t i = 0; i < vec1.size(); ++i) {
      if (!elem_info.AreEqual(
              config_options, name, reinterpret_cast<const char*>(&vec1[i]),
              reinterpret_cast<const char*>(&vec2[i]), mismatch)) {
        return false;
      }
    }
    return true;
  }
}
}  // namespace ROCKSDB_NAMESPACE
