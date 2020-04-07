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

enum class OptionType {
  kBoolean,
  kInt,
  kInt32T,
  kInt64T,
  kVectorInt,
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
  kVectorCompressionType,
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

  kMutable = 0x0100,     // Option is mutable
  kStringNone = 0x2000,  // Don't serialize the option
};

inline OptionTypeFlags operator|(const OptionTypeFlags& a,
                                 const OptionTypeFlags& b) {
  return static_cast<OptionTypeFlags>(static_cast<uint32_t>(a) |
                                      static_cast<uint32_t>(b));
}

inline OptionTypeFlags operator&(const OptionTypeFlags& a,
                                 const OptionTypeFlags& b) {
  return static_cast<OptionTypeFlags>(static_cast<uint32_t>(a) &
                                      static_cast<uint32_t>(b));
}

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

// Function for converting a option "value" into its underlying
// representation in "addr"
using ParserFunc = std::function<Status(
    const std::string& /*name*/, const std::string& /*value*/,
    const ConfigOptions& /*opts*/, char* /*addr*/)>;

// Function for converting an option "addr" into its
// string "value" representation
using StringFunc = std::function<Status(
    const std::string& /*name*/, const char* /*address*/,
    const ConfigOptions& /*opts*/, std::string* /*value*/)>;

// Function for comparing the option at address1 to adddress2
// If they are not equal, updates "mismatch" with the name of the bad option
using EqualsFunc =
    std::function<bool(const std::string& /*name*/, const char* /*address1*/,
                       const char* /*address2*/, const ConfigOptions& /*opts*/,
                       std::string* mismatch)>;

// A struct for storing constant option information such as option name,
// option type, and offset.
class OptionTypeInfo {
 public:
  int offset;
  int mutable_offset;

  // A simple "normal", non-mutable Type "_type" at _offset
  OptionTypeInfo(int _offset, OptionType _type)
      : offset(_offset),
        mutable_offset(0),
        parser_func(nullptr),
        string_func(nullptr),
        equals_func(nullptr),
        type(_type),
        verification(OptionVerificationType::kNormal),
        flags(OptionTypeFlags::kNone) {}

  // A simple "normal", mutable Type "_type" at _offset
  OptionTypeInfo(int _offset, OptionType _type, int _mutable_offset)
      : offset(_offset),
        mutable_offset(_mutable_offset),
        parser_func(nullptr),
        string_func(nullptr),
        equals_func(nullptr),
        type(_type),
        verification(OptionVerificationType::kNormal),
        flags(OptionTypeFlags::kMutable) {}

  OptionTypeInfo(int _offset, OptionType _type,
                 OptionVerificationType _verification, OptionTypeFlags _flags,
                 int _mutable_offset)
      : offset(_offset),
        mutable_offset(_mutable_offset),
        parser_func(nullptr),
        string_func(nullptr),
        equals_func(nullptr),
        type(_type),
        verification(_verification),
        flags(_flags) {}

  OptionTypeInfo(int _offset, OptionType _type,
                 OptionVerificationType _verification, OptionTypeFlags _flags,
                 int _mutable_offset, const ParserFunc& _pfunc)
      : offset(_offset),
        mutable_offset(_mutable_offset),
        parser_func(_pfunc),
        string_func(nullptr),
        equals_func(nullptr),
        type(_type),
        verification(_verification),
        flags(_flags) {}

  OptionTypeInfo(int _offset, OptionType _type,
                 OptionVerificationType _verification, OptionTypeFlags _flags,
                 int _mutable_offset, const ParserFunc& _pfunc,
                 const StringFunc& _sfunc, const EqualsFunc& _efunc)
      : offset(_offset),
        mutable_offset(_mutable_offset),
        parser_func(_pfunc),
        string_func(_sfunc),
        equals_func(_efunc),
        type(_type),
        verification(_verification),
        flags(_flags) {}

  template <typename T>
  static OptionTypeInfo Enum(
      int _offset, const std::unordered_map<std::string, T>* const map) {
    return OptionTypeInfo(
        _offset, OptionType::kEnum, OptionVerificationType::kNormal,
        OptionTypeFlags::kNone, 0,
        [map](const std::string& name, const std::string& value,
              const ConfigOptions&, char* addr) {
          if (map == nullptr) {
            return Status::NotSupported("No enum mapping ", name);
          } else if (ParseEnum<T>(*map, value, reinterpret_cast<T*>(addr))) {
            return Status::OK();
          } else {
            return Status::InvalidArgument("No mapping for enum ", name);
          }
        },
        [map](const std::string& name, const char* addr, const ConfigOptions&,
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
        [](const std::string&, const char* addr1, const char* addr2,
           const ConfigOptions&, std::string*) {
          return (*reinterpret_cast<const T*>(addr1) ==
                  *reinterpret_cast<const T*>(addr2));
        });
  }

  static OptionTypeInfo Struct(
      const std::string& struct_name,
      const std::unordered_map<std::string, OptionTypeInfo>* struct_map,
      int _offset, OptionVerificationType _verification, OptionTypeFlags _flags,
      int _mutable_offset) {
    return OptionTypeInfo(
        _offset, OptionType::kStruct, _verification, _flags, _mutable_offset,
        [struct_name, struct_map](const std::string& name,
                                  const std::string& value,
                                  const ConfigOptions& opts, char* addr) {
          return ParseStruct(struct_name, struct_map, name, value, opts, addr);
        },
        [struct_name, struct_map](const std::string& name, const char* addr,
                                  const ConfigOptions& opts,
                                  std::string* value) {
          return SerializeStruct(struct_name, struct_map, name, addr, opts,
                                 value);
        },
        [struct_name, struct_map](const std::string& name, const char* addr1,
                                  const char* addr2, const ConfigOptions& opts,
                                  std::string* mismatch) {
          return MatchesStruct(struct_name, struct_map, name, addr1, addr2,
                               opts, mismatch);
        });
  }

  bool IsEnabled(OptionTypeFlags otf) const { return (flags & otf) == otf; }

  bool IsMutable() const { return IsEnabled(OptionTypeFlags::kMutable); }

  bool IsDeprecated() const {
    return IsEnabled(OptionVerificationType::kDeprecated);
  }

  bool IsAlias() const { return IsEnabled(OptionVerificationType::kAlias); }

  bool IsEnabled(OptionVerificationType ovf) const {
    return verification == ovf;
  }

  // Returns the sanity level for comparing the option.
  // If the options should not be compared, returns None
  // If the option has a compare flag, returns it.
  // Otherwise, returns "exact"
  ConfigOptions::SanityLevel GetSanityLevel() const {
    if (IsDeprecated() || IsAlias()) {
      return ConfigOptions::SanityLevel::kSanityLevelNone;
    } else {
      auto match = (flags & OptionTypeFlags::kCompareExact);
      if (match == OptionTypeFlags::kCompareDefault) {
        return ConfigOptions::SanityLevel::kSanityLevelExactMatch;
      } else {
        return (ConfigOptions::SanityLevel)match;
      }
    }
  }

  // Returns true if the option should be serialized.
  // Options should be serialized if the are not deprecated, aliases,
  // or marked as "Don't Serialize"
  bool ShouldSerialize() const {
    if (IsDeprecated() || IsAlias()) {
      return false;
    } else if (IsEnabled(OptionTypeFlags::kStringNone)) {
      return false;
    } else {
      return true;
    }
  }

  bool IsByName() const {
    return (verification == OptionVerificationType::kByName ||
            verification == OptionVerificationType::kByNameAllowNull ||
            verification == OptionVerificationType::kByNameAllowFromNull);
  }

  bool IsStruct() const { return (type == OptionType::kStruct); }

  // Parses the option in "opt_value" according to the rules of this class
  // and updates the value at "opt_addr".
  Status ParseOption(const std::string& opt_name, const std::string& opt_value,
                     const ConfigOptions& options, char* opt_addr) const;

  // Serializes the option in "opt_addr" according to the rules of this class
  // into the value at "opt_value".
  Status SerializeOption(const std::string& opt_name, const char* opt_addr,
                         const ConfigOptions& options,
                         std::string* opt_value) const;

  // Compares the "addr1" and "addr2" values according to the rules of this
  // class and returns true if they match.  On a failed match, mismatch is the
  // name of the option that failed to match.
  bool MatchesOption(const std::string& opt_name, const char* addr1,
                     const char* addr2, const ConfigOptions& options,
                     std::string* mismatch) const;

  // Used to override the match rules for "ByName" options.
  bool CheckByName(const std::string& opt_name, const char* this_offset,
                   const char* that_offset, const ConfigOptions& options) const;
  bool CheckByName(const std::string& opt_name, const char* this_ptr,
                   const std::string& that_value,
                   const ConfigOptions& options) const;

  // Parses the input value according to the map for the struct at opt_addr
  // struct_name is the name of the struct option as registered
  // opt_name is the name of the option being evaluated.  This may
  // be the whole struct or a sub-element of it
  static Status ParseStruct(
      const std::string& struct_name,
      const std::unordered_map<std::string, OptionTypeInfo>* map,
      const std::string& opt_name, const std::string& value,
      const ConfigOptions& opts, char* opt_addr);

  // Serializes the input addr according to the map for the struct to value.
  // struct_name is the name of the struct option as registered
  // opt_name is the name of the option being evaluated.  This may
  // be the whole struct or a sub-element of it
  static Status SerializeStruct(
      const std::string& struct_name,
      const std::unordered_map<std::string, OptionTypeInfo>* map,
      const std::string& opt_name, const char* opt_addr,
      const ConfigOptions& opts, std::string* value);

  // Matches the input offsets according to the map for the struct.
  // struct_name is the name of the struct option as registered
  // opt_name is the name of the option being evaluated.  This may
  // be the whole struct or a sub-element of it
  static bool MatchesStruct(
      const std::string& struct_name,
      const std::unordered_map<std::string, OptionTypeInfo>* map,
      const std::string& opt_name, const char* this_offset,
      const char* that_offset, const ConfigOptions& opts,
      std::string* mismatch);

  // Finds the entry for the opt_name in the opt_map, returning
  // nullptr if not found.
  // If found, elem_name will be the name of option to find.
  // This may be opt_name, or a substring of opt_name.
  static const OptionTypeInfo* FindOption(
      const std::string& opt_name,
      const std::unordered_map<std::string, OptionTypeInfo>& opt_map,
      std::string* elem_name);

 private:
  // The optional function to convert a string to its representation
  ParserFunc parser_func;

  // The optional function to convert a value to its string representation
  StringFunc string_func;

  // The optional function to convert a match to option values
  EqualsFunc equals_func;

  OptionType type;
  OptionVerificationType verification;
  OptionTypeFlags flags;
};
}  // namespace ROCKSDB_NAMESPACE
