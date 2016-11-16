# These are the sources from which librocksdb.a is built:
LIB_SOURCES =                                                   \
  db/auto_roll_logger.cc                                        \
  db/builder.cc                                                 \
  db/c.cc                                                       \
  db/column_family.cc                                           \
  db/compacted_db_impl.cc                                       \
  db/compaction.cc                                              \
  db/compaction_iterator.cc                                     \
  db/compaction_job.cc                                          \
  db/compaction_picker.cc                                       \
  db/convenience.cc                                             \
  db/range_del_aggregator.cc                                    \
  db/db_filesnapshot.cc                                         \
  db/dbformat.cc                                                \
  db/db_impl.cc                                                 \
  db/db_impl_debug.cc                                           \
  db/db_impl_readonly.cc                                        \
  db/db_impl_experimental.cc                                    \
  db/db_info_dumper.cc                                          \
  db/db_iter.cc                                                 \
  db/external_sst_file_ingestion_job.cc                         \
  db/experimental.cc                                            \
  db/event_helpers.cc                                           \
  db/file_indexer.cc                                            \
  db/filename.cc                                                \
  db/flush_job.cc                                               \
  db/flush_scheduler.cc                                         \
  db/forward_iterator.cc                                        \
  db/internal_stats.cc                                          \
  db/log_reader.cc                                              \
  db/log_writer.cc                                              \
  db/managed_iterator.cc                                        \
  db/memtable_allocator.cc                                      \
  db/memtable.cc                                                \
  db/memtable_list.cc                                           \
  db/merge_helper.cc                                            \
  db/merge_operator.cc                                          \
  db/repair.cc                                                  \
  db/snapshot_impl.cc                                           \
  db/table_cache.cc                                             \
  db/table_properties_collector.cc                              \
  db/transaction_log_impl.cc                                    \
  db/version_builder.cc                                         \
  db/version_edit.cc                                            \
  db/version_set.cc                                             \
  db/wal_manager.cc                                             \
  db/write_batch.cc                                             \
  db/write_batch_base.cc                                        \
  db/write_controller.cc                                        \
  db/write_thread.cc                                            \
  db/xfunc_test_points.cc                                       \
  memtable/hash_cuckoo_rep.cc                                   \
  memtable/hash_linklist_rep.cc                                 \
  memtable/hash_skiplist_rep.cc                                 \
  memtable/skiplistrep.cc                                       \
  memtable/vectorrep.cc                                         \
  port/stack_trace.cc                                           \
  port/port_posix.cc                                            \
  table/adaptive_table_factory.cc                               \
  table/block_based_filter_block.cc                             \
  table/block_based_table_builder.cc                            \
  table/block_based_table_factory.cc                            \
  table/block_based_table_reader.cc                             \
  table/block_builder.cc                                        \
  table/block.cc                                                \
  table/block_prefix_index.cc                                   \
  table/bloom_block.cc                                          \
  table/cuckoo_table_builder.cc                                 \
  table/cuckoo_table_factory.cc                                 \
  table/cuckoo_table_reader.cc                                  \
  table/flush_block_policy.cc                                   \
  table/format.cc                                               \
  table/full_filter_block.cc                                    \
  table/get_context.cc                                          \
  table/iterator.cc                                             \
  table/merger.cc                                               \
  table/meta_blocks.cc                                          \
  table/sst_file_writer.cc                                      \
  table/plain_table_builder.cc                                  \
  table/plain_table_factory.cc                                  \
  table/plain_table_index.cc                                    \
  table/plain_table_key_coding.cc                               \
  table/plain_table_reader.cc                                   \
  table/persistent_cache_helper.cc                              \
  table/table_properties.cc                                     \
  table/two_level_iterator.cc                                   \
  tools/dump/db_dump_tool.cc                                    \
  util/arena.cc                                                 \
  util/bloom.cc                                                 \
  util/build_version.cc                                         \
  util/cf_options.cc                                            \
  util/clock_cache.cc                                           \
  util/coding.cc                                                \
  util/comparator.cc                                            \
  util/compaction_job_stats_impl.cc                             \
  util/concurrent_arena.cc                                      \
  util/crc32c.cc                                                \
	util/db_options.cc                                            \
  util/delete_scheduler.cc                                      \
  util/dynamic_bloom.cc                                         \
  util/env.cc                                                   \
  util/env_chroot.cc                                            \
  util/env_hdfs.cc                                              \
  util/env_posix.cc                                             \
  util/event_logger.cc                                          \
  util/file_util.cc                                             \
  util/file_reader_writer.cc                                    \
  util/filter_policy.cc                                         \
  util/hash.cc                                                  \
  util/histogram.cc                                             \
  util/histogram_windowing.cc                                   \
  util/instrumented_mutex.cc                                    \
  util/iostats_context.cc                                       \
  util/io_posix.cc                                              \
  util/log_buffer.cc                                            \
  util/logging.cc                                               \
  util/lru_cache.cc                                             \
  util/memenv.cc                                                \
  util/murmurhash.cc                                            \
  util/options.cc                                               \
  util/options_helper.cc                                        \
  util/options_parser.cc                                        \
  util/options_sanity_check.cc                                  \
  util/perf_context.cc                                          \
  util/perf_level.cc                                            \
  util/random.cc                                                \
  util/rate_limiter.cc                                          \
	util/sharded_cache.cc       																	\
  util/slice.cc                                                 \
  util/sst_file_manager_impl.cc                                 \
  util/statistics.cc                                            \
  util/status.cc                                                \
  util/status_message.cc                                        \
  util/string_util.cc                                           \
  util/sync_point.cc                                            \
  util/thread_local.cc                                          \
  util/thread_status_impl.cc                                    \
  util/thread_status_updater.cc                                 \
  util/thread_status_updater_debug.cc                           \
  util/thread_status_util.cc                                    \
  util/thread_status_util_debug.cc                              \
  util/threadpool_imp.cc                                        \
  util/transaction_test_util.cc                                 \
  util/xfunc.cc                                                 \
  util/xxhash.cc                                                \
  utilities/backupable/backupable_db.cc                         \
  utilities/blob_db/blob_db.cc                                  \
  utilities/convenience/info_log_finder.cc                      \
  utilities/checkpoint/checkpoint.cc                            \
  utilities/compaction_filters/remove_emptyvalue_compactionfilter.cc    \
  utilities/document/document_db.cc                             \
  utilities/document/json_document_builder.cc                   \
  utilities/document/json_document.cc                           \
  utilities/env_mirror.cc                                       \
  utilities/env_registry.cc                                     \
  utilities/flashcache/flashcache.cc                            \
  utilities/geodb/geodb_impl.cc                                 \
  utilities/leveldb_options/leveldb_options.cc                  \
  utilities/lua/rocks_lua_compaction_filter.cc                  \
  utilities/memory/memory_util.cc                               \
  utilities/merge_operators/put.cc                              \
  utilities/merge_operators/max.cc                              \
  utilities/merge_operators/string_append/stringappend2.cc      \
  utilities/merge_operators/string_append/stringappend.cc       \
  utilities/merge_operators/uint64add.cc                        \
  utilities/option_change_migration/option_change_migration.cc  \
  utilities/options/options_util.cc                             \
  utilities/persistent_cache/persistent_cache_tier.cc           \
  utilities/persistent_cache/volatile_tier_impl.cc              \
  utilities/persistent_cache/block_cache_tier_file.cc           \
  utilities/persistent_cache/block_cache_tier_metadata.cc       \
  utilities/persistent_cache/block_cache_tier.cc                \
  utilities/redis/redis_lists.cc                                \
  utilities/simulator_cache/sim_cache.cc                        \
  utilities/spatialdb/spatial_db.cc                             \
  utilities/table_properties_collectors/compact_on_deletion_collector.cc \
  utilities/transactions/optimistic_transaction_impl.cc         \
  utilities/transactions/optimistic_transaction_db_impl.cc      \
  utilities/transactions/transaction_base.cc                    \
  utilities/transactions/transaction_db_impl.cc                 \
  utilities/transactions/transaction_db_mutex_impl.cc           \
  utilities/transactions/transaction_lock_mgr.cc                \
  utilities/transactions/transaction_impl.cc                    \
  utilities/transactions/transaction_util.cc                    \
  utilities/ttl/db_ttl_impl.cc                                  \
  utilities/date_tiered/date_tiered_db_impl.cc                  \
  utilities/write_batch_with_index/write_batch_with_index.cc    \
  utilities/write_batch_with_index/write_batch_with_index_internal.cc    \

TOOL_LIB_SOURCES = \
  tools/ldb_cmd.cc                                               \
  tools/ldb_tool.cc                                              \
  tools/sst_dump_tool.cc                                         \

MOCK_LIB_SOURCES = \
  table/mock_table.cc \
  util/mock_env.cc \
  util/fault_injection_test_env.cc

BENCH_LIB_SOURCES = \
  tools/db_bench_tool.cc                                        \

EXP_LIB_SOURCES = \
  utilities/col_buf_encoder.cc                                          \
  utilities/col_buf_decoder.cc                                          \
  utilities/column_aware_encoding_util.cc

TEST_LIB_SOURCES = \
  util/testharness.cc                                                   \
  util/testutil.cc                                                      \
  db/db_test_util.cc

MAIN_SOURCES =                                                    \
  third-party/gtest-1.7.0/fused-src/gtest/gtest-all.cc                  \
  db/auto_roll_logger_test.cc                                           \
  db/column_family_test.cc                                              \
  db/compaction_job_test.cc                                             \
  db/compaction_job_stats_test.cc                                       \
  db/compaction_picker_test.cc                                          \
  db/comparator_db_test.cc                                              \
  db/corruption_test.cc                                                 \
  db/cuckoo_table_db_test.cc                                            \
  db/dbformat_test.cc                                                   \
  db/db_iter_test.cc                                                    \
  db/db_test.cc                                                         \
  db/db_block_cache_test.cc						\
  db/db_io_failure_test.cc                                              \
  db/db_bloom_filter_test.cc                                            \
  db/db_compaction_filter_test.cc                                       \
  db/db_compaction_test.cc                                              \
  db/db_dynamic_level_test.cc                                           \
  db/db_flush_test.cc							\
  db/db_inplace_update_test.cc                                          \
  db/db_iterator_test.cc						\
  db/db_log_iter_test.cc                                                \
  db/db_options_test.cc                                                 \
  db/db_sst_test.cc                                                     \
  db/external_sst_file_test.cc                                          \
  db/db_tailing_iter_test.cc                                            \
  db/db_universal_compaction_test.cc                                    \
  db/db_wal_test.cc                                                     \
  db/db_table_properties_test.cc                                        \
  db/deletefile_test.cc                                                 \
  db/fault_injection_test.cc                                            \
  db/file_indexer_test.cc                                               \
  db/filename_test.cc                                                   \
  db/flush_job_test.cc                                                  \
  db/inlineskiplist_test.cc                                             \
  db/listener_test.cc                                                   \
  db/log_test.cc                                                        \
  db/manual_compaction_test.cc                                          \
  db/memtablerep_bench.cc                                               \
  db/merge_test.cc                                                      \
  db/options_file_test.cc                                               \
  db/perf_context_test.cc                                               \
  db/plain_table_db_test.cc                                             \
  db/prefix_test.cc                                                     \
  db/skiplist_test.cc                                                   \
  db/table_properties_collector_test.cc                                 \
  db/version_builder_test.cc                                            \
  db/version_edit_test.cc                                               \
  db/version_set_test.cc                                                \
  db/wal_manager_test.cc                                                \
  db/write_batch_test.cc                                                \
  db/write_controller_test.cc                                           \
  db/write_callback_test.cc                                             \
  table/block_based_filter_block_test.cc                                \
  table/block_test.cc                                                   \
  table/cuckoo_table_builder_test.cc                                    \
  table/cuckoo_table_reader_test.cc                                     \
  table/full_filter_block_test.cc                                       \
  table/merger_test.cc                                                  \
  table/table_reader_bench.cc                                           \
  table/table_test.cc                                                   \
  tools/db_bench.cc                                                     \
  tools/db_bench_tool_test.cc                                           \
  tools/db_sanity_test.cc                                               \
  tools/ldb_cmd_test.cc                                                 \
  tools/reduce_levels_test.cc                                           \
  tools/sst_dump_test.cc                                                \
  util/arena_test.cc                                                    \
  util/autovector_test.cc                                               \
  util/bloom_test.cc                                                    \
  util/cache_bench.cc                                                   \
  util/cache_test.cc                                                    \
  util/coding_test.cc                                                   \
  util/crc32c_test.cc                                                   \
  util/dynamic_bloom_test.cc                                            \
  util/env_basic_test.cc                                                \
  util/env_test.cc                                                      \
  util/filelock_test.cc                                                 \
  util/histogram_test.cc                                                \
  util/statistics_test.cc                                               \
  utilities/backupable/backupable_db_test.cc                            \
  utilities/blob_db/blob_db_test.cc                                     \
  utilities/checkpoint/checkpoint_test.cc                               \
  utilities/document/document_db_test.cc                                \
  utilities/document/json_document_test.cc                              \
  utilities/env_registry_test.cc                                        \
  utilities/geodb/geodb_test.cc                                         \
  utilities/memory/memory_test.cc                                       \
  utilities/merge_operators/string_append/stringappend_test.cc          \
  utilities/option_change_migration/option_change_migration_test.cc           \
  utilities/options/options_util_test.cc                                \
  utilities/redis/redis_lists_test.cc                                   \
  utilities/simulator_cache/sim_cache_test.cc                           \
  utilities/spatialdb/spatial_db_test.cc                                \
  utilities/table_properties_collectors/compact_on_deletion_collector_test.cc  \
  utilities/transactions/optimistic_transaction_test.cc                 \
  utilities/transactions/transaction_test.cc                            \
  utilities/ttl/ttl_test.cc                                             \
  utilities/date_tiered/date_tiered_test.cc                             \
  utilities/write_batch_with_index/write_batch_with_index_test.cc       \
  utilities/column_aware_encoding_test.cc                               \
  utilities/lua/rocks_lua_test.cc                                       \
	util/iostats_context_test.cc																					\
  util/log_write_bench.cc                                               \
  util/mock_env_test.cc                                                 \
  util/options_test.cc                                                  \
  util/event_logger_test.cc                                             \
  util/rate_limiter_test.cc                                             \
  util/slice_transform_test.cc                                          \
  util/thread_list_test.cc                                              \
  util/thread_local_test.cc                                             \
  utilities/column_aware_encoding_exp.cc

JNI_NATIVE_SOURCES =                                          \
  java/rocksjni/backupenginejni.cc                            \
  java/rocksjni/backupablejni.cc                              \
  java/rocksjni/checkpoint.cc                                 \
  java/rocksjni/columnfamilyhandle.cc                         \
  java/rocksjni/compaction_filter.cc                          \
  java/rocksjni/comparator.cc                                 \
  java/rocksjni/comparatorjnicallback.cc                      \
  java/rocksjni/env.cc                                        \
  java/rocksjni/env_options.cc                                \
  java/rocksjni/external_sst_file_info.cc                     \
  java/rocksjni/filter.cc                                     \
  java/rocksjni/iterator.cc                                   \
  java/rocksjni/loggerjnicallback.cc                          \
  java/rocksjni/memtablejni.cc                                \
  java/rocksjni/merge_operator.cc                             \
  java/rocksjni/options.cc                                    \
  java/rocksjni/ratelimiterjni.cc                             \
  java/rocksjni/remove_emptyvalue_compactionfilterjni.cc      \
  java/rocksjni/restorejni.cc                                 \
  java/rocksjni/rocksjni.cc                                   \
  java/rocksjni/rocksdb_exception_test.cc                     \
  java/rocksjni/slice.cc                                      \
  java/rocksjni/snapshot.cc                                   \
  java/rocksjni/sst_file_writerjni.cc                         \
  java/rocksjni/statistics.cc                                 \
  java/rocksjni/table.cc                                      \
  java/rocksjni/transaction_log.cc                            \
  java/rocksjni/ttl.cc                                        \
  java/rocksjni/write_batch.cc                                \
  java/rocksjni/writebatchhandlerjnicallback.cc               \
  java/rocksjni/write_batch_test.cc                           \
  java/rocksjni/write_batch_with_index.cc

# Currently, we do not generate dependencies for
# java/rocksjni/write_batch_test.cc, because its dependent,
# java/include/org_rocksdb_WriteBatch.h is generated.
# TODO/FIXME: fix the above.  Otherwise, the current rules would fail:
#   java/rocksjni/write_batch_test.cc:13:44: fatal error: include/org_rocksdb_WriteBatch.h: No such file or directory
#    #include "include/org_rocksdb_WriteBatch.h"

# These are the xfunc tests run :
XFUNC_TESTS =                                                   \
  "managed_new"                                                 \
  "managed_xftest_dropold"                                      \
  "managed_xftest_release"                                      \
  "inplace_lock_test"                                           \
  "transaction"
