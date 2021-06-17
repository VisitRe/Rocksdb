// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "table/plain/plain_table_factory.h"

#include <stdint.h>

#include <memory>

#include "db/dbformat.h"
#include "options/configurable_helper.h"
#include "port/port.h"
#include "rocksdb/convenience.h"
#include "rocksdb/utilities/object_registry.h"
#include "rocksdb/utilities/options_type.h"
#include "table/plain/plain_table_builder.h"
#include "table/plain/plain_table_reader.h"
#include "util/string_util.h"

namespace ROCKSDB_NAMESPACE {
#ifndef ROCKSDB_LITE
static std::unordered_map<std::string, OptionTypeInfo> plain_table_type_info = {
    {"user_key_len",
     {offsetof(struct PlainTableOptions, user_key_len), OptionType::kUInt32T,
      OptionVerificationType::kNormal, OptionTypeFlags::kNone}},
    {"bloom_bits_per_key",
     {offsetof(struct PlainTableOptions, bloom_bits_per_key), OptionType::kInt,
      OptionVerificationType::kNormal, OptionTypeFlags::kNone}},
    {"hash_table_ratio",
     {offsetof(struct PlainTableOptions, hash_table_ratio), OptionType::kDouble,
      OptionVerificationType::kNormal, OptionTypeFlags::kNone}},
    {"index_sparseness",
     {offsetof(struct PlainTableOptions, index_sparseness), OptionType::kSizeT,
      OptionVerificationType::kNormal, OptionTypeFlags::kNone}},
    {"huge_page_tlb_size",
     {offsetof(struct PlainTableOptions, huge_page_tlb_size),
      OptionType::kSizeT, OptionVerificationType::kNormal,
      OptionTypeFlags::kNone}},
    {"encoding_type",
     {offsetof(struct PlainTableOptions, encoding_type),
      OptionType::kEncodingType, OptionVerificationType::kNormal,
      OptionTypeFlags::kNone}},
    {"full_scan_mode",
     {offsetof(struct PlainTableOptions, full_scan_mode), OptionType::kBoolean,
      OptionVerificationType::kNormal, OptionTypeFlags::kNone}},
    {"store_index_in_file",
     {offsetof(struct PlainTableOptions, store_index_in_file),
      OptionType::kBoolean, OptionVerificationType::kNormal,
      OptionTypeFlags::kNone}},
};

PlainTableFactory::PlainTableFactory(const PlainTableOptions& options)
    : table_options_(options) {
  RegisterOptions(&table_options_, &plain_table_type_info);
}

Status PlainTableFactory::NewTableReader(
    const ReadOptions& /*ro*/, const TableReaderOptions& table_reader_options,
    std::unique_ptr<RandomAccessFileReader>&& file, uint64_t file_size,
    std::unique_ptr<TableReader>* table,
    bool /*prefetch_index_and_filter_in_cache*/) const {
  return PlainTableReader::Open(
      table_reader_options.ioptions, table_reader_options.env_options,
      table_reader_options.internal_comparator, std::move(file), file_size,
      table, table_options_.bloom_bits_per_key, table_options_.hash_table_ratio,
      table_options_.index_sparseness, table_options_.huge_page_tlb_size,
      table_options_.full_scan_mode, table_reader_options.immortal,
      table_reader_options.prefix_extractor);
}

TableBuilder* PlainTableFactory::NewTableBuilder(
    const TableBuilderOptions& table_builder_options,
    WritableFileWriter* file) const {
  // Ignore the skip_filters flag. PlainTable format is optimized for small
  // in-memory dbs. The skip_filters optimization is not useful for plain
  // tables
  //
  return new PlainTableBuilder(
      table_builder_options.ioptions, table_builder_options.moptions,
      table_builder_options.int_tbl_prop_collector_factories,
      table_builder_options.column_family_id, file, table_options_.user_key_len,
      table_options_.encoding_type, table_options_.index_sparseness,
      table_options_.bloom_bits_per_key,
      table_builder_options.column_family_name, 6,
      table_options_.huge_page_tlb_size, table_options_.hash_table_ratio,
      table_options_.store_index_in_file, table_builder_options.db_id,
      table_builder_options.db_session_id);
}

std::string PlainTableFactory::GetPrintableOptions() const {
  std::string ret;
  ret.reserve(20000);
  const int kBufferSize = 200;
  char buffer[kBufferSize];

  snprintf(buffer, kBufferSize, "  user_key_len: %u\n",
           table_options_.user_key_len);
  ret.append(buffer);
  snprintf(buffer, kBufferSize, "  bloom_bits_per_key: %d\n",
           table_options_.bloom_bits_per_key);
  ret.append(buffer);
  snprintf(buffer, kBufferSize, "  hash_table_ratio: %lf\n",
           table_options_.hash_table_ratio);
  ret.append(buffer);
  snprintf(buffer, kBufferSize, "  index_sparseness: %" ROCKSDB_PRIszt "\n",
           table_options_.index_sparseness);
  ret.append(buffer);
  snprintf(buffer, kBufferSize, "  huge_page_tlb_size: %" ROCKSDB_PRIszt "\n",
           table_options_.huge_page_tlb_size);
  ret.append(buffer);
  snprintf(buffer, kBufferSize, "  encoding_type: %d\n",
           table_options_.encoding_type);
  ret.append(buffer);
  snprintf(buffer, kBufferSize, "  full_scan_mode: %d\n",
           table_options_.full_scan_mode);
  ret.append(buffer);
  snprintf(buffer, kBufferSize, "  store_index_in_file: %d\n",
           table_options_.store_index_in_file);
  ret.append(buffer);
  return ret;
}

Status GetPlainTableOptionsFromString(const PlainTableOptions& table_options,
                                      const std::string& opts_str,
                                      PlainTableOptions* new_table_options) {
  ConfigOptions config_options;
  config_options.input_strings_escaped = false;
  config_options.ignore_unknown_options = false;
  config_options.invoke_prepare_options = false;
  return GetPlainTableOptionsFromString(config_options, table_options, opts_str,
                                        new_table_options);
}

Status GetPlainTableOptionsFromString(const ConfigOptions& config_options,
                                      const PlainTableOptions& table_options,
                                      const std::string& opts_str,
                                      PlainTableOptions* new_table_options) {
  std::unordered_map<std::string, std::string> opts_map;
  Status s = StringToMap(opts_str, &opts_map);
  if (!s.ok()) {
    return s;
  }

  s = GetPlainTableOptionsFromMap(config_options, table_options, opts_map,
                                  new_table_options);
  // Translate any errors (NotFound, NotSupported, to InvalidArgument
  if (s.ok() || s.IsInvalidArgument()) {
    return s;
  } else {
    return Status::InvalidArgument(s.getState());
  }
}
#endif  // ROCKSDB_LITE

#ifndef ROCKSDB_LITE
static int RegisterBuiltinMemTableRepFactory(ObjectLibrary& library,
                                             const std::string& /*arg*/) {
  auto AsRegex = [](const std::string& name, const std::string& alt) {
    std::string regex;
    regex.append("(").append(name);
    regex.append("|").append(alt).append(")(:[0-9]*)?");
    return regex;
  };

  library.Register<MemTableRepFactory>(
      AsRegex(VectorRepFactory::kClassName(), "vector"),
      [](const std::string& uri, std::unique_ptr<MemTableRepFactory>* guard,
         std::string* /*errmsg*/) {
        auto colon = uri.find(":");
        if (colon != std::string::npos) {
          size_t count = ParseSizeT(uri.substr(colon + 1));
          guard->reset(new VectorRepFactory(count));
        } else {
          guard->reset(new VectorRepFactory());
        }
        return guard->get();
      });
  library.Register<MemTableRepFactory>(
      AsRegex(SkipListFactory::kClassName(), "skip_list"),
      [](const std::string& uri, std::unique_ptr<MemTableRepFactory>* guard,
         std::string* /*errmsg*/) {
        auto colon = uri.find(":");
        if (colon != std::string::npos) {
          size_t lookahead = ParseSizeT(uri.substr(colon + 1));
          guard->reset(new SkipListFactory(lookahead));
        } else {
          guard->reset(new SkipListFactory());
        }
        return guard->get();
      });
  library.Register<MemTableRepFactory>(
      AsRegex("HashLinkListRepFactory", "hash_linkedlist"),
      [](const std::string& uri, std::unique_ptr<MemTableRepFactory>* guard,
         std::string* /*errmsg*/) {
        // Expecting format: hash_linkedlist:<hash_bucket_count>
        auto colon = uri.find(":");
        if (colon != std::string::npos) {
          size_t hash_bucket_count = ParseSizeT(uri.substr(colon + 1));
          guard->reset(NewHashLinkListRepFactory(hash_bucket_count));
        } else {
          guard->reset(NewHashLinkListRepFactory());
        }
        return guard->get();
      });
  library.Register<MemTableRepFactory>(
      AsRegex("HashSkipListRepFactory", "prefix_hash"),
      [](const std::string& uri, std::unique_ptr<MemTableRepFactory>* guard,
         std::string* /*errmsg*/) {
        // Expecting format: prefix_hash:<hash_bucket_count>
        auto colon = uri.find(":");
        if (colon != std::string::npos) {
          size_t hash_bucket_count = ParseSizeT(uri.substr(colon + 1));
          guard->reset(NewHashSkipListRepFactory(hash_bucket_count));
        } else {
          guard->reset(NewHashSkipListRepFactory());
        }
        return guard->get();
      });
  library.Register<MemTableRepFactory>(
      AsRegex("cuckoo", "cuckoo"),
      [](const std::string& /*uri*/,
         std::unique_ptr<MemTableRepFactory>* /*guard*/, std::string* errmsg) {
        *errmsg = "cuckoo hash memtable is not supported anymore.";
        return nullptr;
      });

  return 5;
}
#endif  // ROCKSDB_LITE
Status GetMemTableRepFactoryFromString(
    const std::string& opts_str, std::unique_ptr<MemTableRepFactory>* result) {
  ConfigOptions config_options;
  config_options.ignore_unsupported_options = false;
  config_options.ignore_unknown_options = false;
  return MemTableRepFactory::CreateFromString(config_options, opts_str, result);
}

Status MemTableRepFactory::CreateFromString(
    const ConfigOptions& config_options, const std::string& value,
    std::unique_ptr<MemTableRepFactory>* result) {
#ifndef ROCKSDB_LITE
  static std::once_flag once;
  std::call_once(once, [&]() {
    RegisterBuiltinMemTableRepFactory(*(ObjectLibrary::Default().get()), "");
  });
#endif  // ROCKSDB_LITE
  std::string id;
  std::unordered_map<std::string, std::string> opt_map;
  Status status =
      ConfigurableHelper::GetOptionsMap(value, result->get(), &id, &opt_map);
  if (!status.ok()) {  // GetOptionsMap failed
    return status;
  }
  std::string curr_opts;
#ifndef ROCKSDB_LITE
  if (*result != nullptr && (*result)->GetId() == id) {
    // Try to get the existing options, ignoring any errors
    ConfigOptions embedded = config_options;
    embedded.delimiter = ";";
    (*result)->GetOptionString(embedded, &curr_opts).PermitUncheckedError();
  }
#endif
  if (value.empty()) {
    // No Id and no options.  Clear the object
    result->reset();
    return Status::OK();
  } else if (id.empty()) {  // We have no Id but have options.  Not good
    return Status::NotSupported("Cannot reset object ", id);
  } else {
#ifndef ROCKSDB_LITE
    status = config_options.registry->NewUniqueObject(id, result);
#else
    std::vector<std::string> opts_list = StringSplit(id, ':');
    if (opts_list.empty() || opts_list.size() > 2) {
      status = Status::InvalidArgument("Can't parse memtable_factory option ",
                                       value);
    } else if (opts_list[0] == "skip_list" ||
               opts_list[0] == SkipListFactory::kClassName()) {
      // Expecting format
      // skip_list:<lookahead>
      if (opts_list.size() == 2) {
        size_t lookahead = ParseSizeT(opts_list[1]);
        result->reset(new SkipListFactory(lookahead));
      } else {
        result->reset(new SkipListFactory());
      }
    } else if (opts_list[0] == "cuckoo") {
      status = Status::NotSupported(
          "cuckoo hash memtable is not supported anymore.");
    } else {
      status = Status::NotSupported("Cannot load object in LITE mode ", id);
    }
#endif  // ROCKSDB_LITE
  }
  if (!status.ok()) {
    if (config_options.ignore_unsupported_options && status.IsNotSupported()) {
      return Status::OK();
    } else {
      return status;
    }
  } else if (!curr_opts.empty() || !opt_map.empty()) {
    status = ConfigurableHelper::ConfigureNewObject(
        config_options, result->get(), id, curr_opts, opt_map);
  }
  return status;
}

#ifndef ROCKSDB_LITE
Status GetPlainTableOptionsFromMap(
    const PlainTableOptions& table_options,
    const std::unordered_map<std::string, std::string>& opts_map,
    PlainTableOptions* new_table_options, bool input_strings_escaped,
    bool ignore_unknown_options) {
  ConfigOptions config_options;
  config_options.input_strings_escaped = input_strings_escaped;
  config_options.ignore_unknown_options = ignore_unknown_options;
  return GetPlainTableOptionsFromMap(config_options, table_options, opts_map,
                                     new_table_options);
}

Status GetPlainTableOptionsFromMap(
    const ConfigOptions& config_options, const PlainTableOptions& table_options,
    const std::unordered_map<std::string, std::string>& opts_map,
    PlainTableOptions* new_table_options) {
  assert(new_table_options);
  PlainTableFactory ptf(table_options);
  Status s = ptf.ConfigureFromMap(config_options, opts_map);
  if (s.ok()) {
    *new_table_options = *(ptf.GetOptions<PlainTableOptions>());
  } else {
    // Restore "new_options" to the default "base_options".
    *new_table_options = table_options;
  }
  return s;
}

extern TableFactory* NewPlainTableFactory(const PlainTableOptions& options) {
  return new PlainTableFactory(options);
}

const std::string PlainTablePropertyNames::kEncodingType =
    "rocksdb.plain.table.encoding.type";

const std::string PlainTablePropertyNames::kBloomVersion =
    "rocksdb.plain.table.bloom.version";

const std::string PlainTablePropertyNames::kNumBloomBlocks =
    "rocksdb.plain.table.bloom.numblocks";

#endif  // ROCKSDB_LITE
}  // namespace ROCKSDB_NAMESPACE
