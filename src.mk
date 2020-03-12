# These are the sources from which librocksdb.a is built:
LIB_SOURCES =                                                   \
  cache/clock_cache.cc                                          \
  cache/lru_cache.cc                                            \
  cache/sharded_cache.cc                                        \
  db/arena_wrapped_db_iter.cc                                   \
  db/blob_file_addition.cc                                      \
  db/blob_file_garbage.cc                                       \
  db/builder.cc                                                 \
  db/c.cc                                                       \
  db/column_family.cc                                           \
  db/compacted_db_impl.cc                                       \
  db/compaction/compaction.cc                                 	\
  db/compaction/compaction_iterator.cc                          \
  db/compaction/compaction_job.cc                               \
  db/compaction/compaction_picker.cc                            \
  db/compaction/compaction_picker_fifo.cc                       \
  db/compaction/compaction_picker_level.cc                      \
  db/compaction/compaction_picker_universal.cc                 	\
  db/convenience.cc                                             \
  db/db_filesnapshot.cc                                         \
  db/db_impl/db_impl.cc                                         \
  db/db_impl/db_impl_compaction_flush.cc                        \
  db/db_impl/db_impl_debug.cc                                   \
  db/db_impl/db_impl_experimental.cc                            \
  db/db_impl/db_impl_files.cc                                   \
  db/db_impl/db_impl_open.cc                                    \
  db/db_impl/db_impl_readonly.cc                                \
  db/db_impl/db_impl_secondary.cc                               \
  db/db_impl/db_impl_write.cc                                   \
  db/db_info_dumper.cc                                          \
  db/db_iter.cc                                                 \
  db/dbformat.cc                                                \
  db/error_handler.cc						                                \
  db/event_helpers.cc                                           \
  db/experimental.cc                                            \
  db/external_sst_file_ingestion_job.cc                         \
  db/file_indexer.cc                                            \
  db/flush_job.cc                                               \
  db/flush_scheduler.cc                                         \
  db/forward_iterator.cc                                        \
  db/import_column_family_job.cc                                \
  db/internal_stats.cc                                          \
  db/logs_with_prep_tracker.cc                                  \
  db/log_reader.cc                                              \
  db/log_writer.cc                                              \
  db/malloc_stats.cc                                            \
  db/memtable.cc                                                \
  db/memtable_list.cc                                           \
  db/merge_helper.cc                                            \
  db/merge_operator.cc                                          \
  db/range_del_aggregator.cc                                    \
  db/range_tombstone_fragmenter.cc                              \
  db/repair.cc                                                  \
  db/snapshot_impl.cc                                           \
  db/table_cache.cc                                             \
  db/table_properties_collector.cc                              \
  db/transaction_log_impl.cc                                    \
  db/trim_history_scheduler.cc                                  \
  db/version_builder.cc                                         \
  db/version_edit.cc                                            \
  db/version_set.cc                                             \
  db/wal_manager.cc                                             \
  db/write_batch.cc                                             \
  db/write_batch_base.cc                                        \
  db/write_controller.cc                                        \
  db/write_thread.cc                                            \
  env/env.cc                                                    \
  env/env_chroot.cc                                             \
  env/env_encryption.cc                                         \
  env/env_hdfs.cc                                               \
  env/env_posix.cc                                              \
  env/file_system.cc                                            \
  env/fs_posix.cc                                           	  \
  env/io_posix.cc                                               \
  env/mock_env.cc                                               \
  file/delete_scheduler.cc                                      \
  file/file_prefetch_buffer.cc                                  \
  file/file_util.cc                                             \
  file/filename.cc                                              \
  file/random_access_file_reader.cc                             \
  file/read_write_util.cc                                       \
  file/readahead_raf.cc                                         \
  file/sequence_file_reader.cc                                  \
  file/sst_file_manager_impl.cc                                 \
  file/writable_file_writer.cc                                  \
  logging/auto_roll_logger.cc                                   \
  logging/event_logger.cc                                       \
  logging/log_buffer.cc                                         \
  memory/arena.cc                                               \
  memory/concurrent_arena.cc                                    \
  memory/jemalloc_nodump_allocator.cc                           \
  memtable/alloc_tracker.cc                                     \
  memtable/hash_linklist_rep.cc                                 \
  memtable/hash_skiplist_rep.cc                                 \
  memtable/skiplistrep.cc                                       \
  memtable/vectorrep.cc                                         \
  memtable/write_buffer_manager.cc                              \
  monitoring/histogram.cc                                       \
  monitoring/histogram_windowing.cc                             \
  monitoring/in_memory_stats_history.cc                         \
  monitoring/instrumented_mutex.cc                              \
  monitoring/iostats_context.cc                                 \
  monitoring/perf_context.cc                                    \
  monitoring/perf_level.cc                                      \
  monitoring/persistent_stats_history.cc                        \
  monitoring/statistics.cc                                      \
  monitoring/thread_status_impl.cc                              \
  monitoring/thread_status_updater.cc                           \
  monitoring/thread_status_updater_debug.cc                     \
  monitoring/thread_status_util.cc                              \
  monitoring/thread_status_util_debug.cc                        \
  options/cf_options.cc                                         \
  options/db_options.cc                                         \
  options/options.cc                                            \
  options/options_helper.cc                                     \
  options/options_parser.cc                                     \
  options/options_sanity_check.cc                               \
  port/port_posix.cc                                            \
  port/stack_trace.cc                                           \
  table/adaptive/adaptive_table_factory.cc                      \
  table/block_based/block.cc                                    \
  table/block_based/block_based_filter_block.cc                 \
  table/block_based/block_based_table_builder.cc                \
  table/block_based/block_based_table_factory.cc                \
  table/block_based/block_based_table_reader.cc                 \
  table/block_based/binary_search_index_reader.cc               \
  table/block_based/block_builder.cc                            \
  table/block_based/block_prefix_index.cc                       \
  table/block_based/data_block_hash_index.cc                    \
  table/block_based/data_block_footer.cc                        \
  table/block_based/filter_block_reader_common.cc               \
  table/block_based/filter_policy.cc                            \
  table/block_based/flush_block_policy.cc                       \
  table/block_based/full_filter_block.cc                        \
  table/block_based/hash_index_reader.cc                        \
  table/block_based/index_builder.cc                            \
  table/block_based/index_reader_common.cc                      \
  table/block_based/parsed_full_filter_block.cc                 \
  table/block_based/partitioned_filter_block.cc                 \
  table/block_based/partitioned_index_reader.cc                 \
  table/block_based/reader_common.cc                            \
  table/block_based/uncompression_dict_reader.cc                \
  table/block_fetcher.cc                             		        \
  table/cuckoo/cuckoo_table_builder.cc                          \
  table/cuckoo/cuckoo_table_factory.cc                          \
  table/cuckoo/cuckoo_table_reader.cc                           \
  table/format.cc                                               \
  table/get_context.cc                                          \
  table/iterator.cc                                             \
  table/merging_iterator.cc                                     \
  table/meta_blocks.cc                                          \
  table/persistent_cache_helper.cc                              \
  table/plain/plain_table_bloom.cc                              \
  table/plain/plain_table_builder.cc                            \
  table/plain/plain_table_factory.cc                            \
  table/plain/plain_table_index.cc                              \
  table/plain/plain_table_key_coding.cc                         \
  table/plain/plain_table_reader.cc                             \
  table/sst_file_reader.cc                                      \
  table/sst_file_writer.cc                                      \
  table/table_properties.cc                                     \
  table/two_level_iterator.cc                                   \
  test_util/sync_point.cc                                       \
  test_util/sync_point_impl.cc                                  \
  test_util/transaction_test_util.cc                            \
  tools/dump/db_dump_tool.cc                                    \
  trace_replay/trace_replay.cc                                  \
  trace_replay/block_cache_tracer.cc                            \
  util/build_version.cc                                         \
  util/coding.cc                                                \
  util/compaction_job_stats_impl.cc                             \
  util/comparator.cc                                            \
  util/compression_context_cache.cc                             \
  util/concurrent_task_limiter_impl.cc                          \
  util/crc32c.cc                                                \
  util/dynamic_bloom.cc                                         \
  util/hash.cc                                                  \
  util/murmurhash.cc                                            \
  util/random.cc                                                \
  util/rate_limiter.cc                                          \
  util/slice.cc                                                 \
  util/file_checksum_helper.cc      				\
  util/status.cc                                                \
  util/string_util.cc                                           \
  util/thread_local.cc                                          \
  util/threadpool_imp.cc                                        \
  util/xxhash.cc                                                \
  utilities/backupable/backupable_db.cc                         \
  utilities/blob_db/blob_compaction_filter.cc                   \
  utilities/blob_db/blob_db.cc                                  \
  utilities/blob_db/blob_db_impl.cc                             \
  utilities/blob_db/blob_db_impl_filesnapshot.cc                \
  utilities/blob_db/blob_file.cc                                \
  utilities/blob_db/blob_log_format.cc                          \
  utilities/blob_db/blob_log_reader.cc                          \
  utilities/blob_db/blob_log_writer.cc                          \
  utilities/cassandra/cassandra_compaction_filter.cc            \
  utilities/cassandra/format.cc                                 \
  utilities/cassandra/merge_operator.cc                         \
  utilities/checkpoint/checkpoint_impl.cc                       \
  utilities/compaction_filters/remove_emptyvalue_compactionfilter.cc    \
  utilities/convenience/info_log_finder.cc                      \
  utilities/debug.cc                                            \
  utilities/env_mirror.cc                                       \
  utilities/env_timed.cc                                        \
  utilities/leveldb_options/leveldb_options.cc                  \
  utilities/memory/memory_util.cc                               \
  utilities/merge_operators/max.cc                              \
  utilities/merge_operators/put.cc                              \
  utilities/merge_operators/sortlist.cc                  		    \
  utilities/merge_operators/string_append/stringappend.cc       \
  utilities/merge_operators/string_append/stringappend2.cc      \
  utilities/merge_operators/uint64add.cc                        \
  utilities/merge_operators/bytesxor.cc                         \
  utilities/object_registry.cc                                  \
  utilities/option_change_migration/option_change_migration.cc  \
  utilities/options/options_util.cc                             \
  utilities/persistent_cache/block_cache_tier.cc                \
  utilities/persistent_cache/block_cache_tier_file.cc           \
  utilities/persistent_cache/block_cache_tier_metadata.cc       \
  utilities/persistent_cache/persistent_cache_tier.cc           \
  utilities/persistent_cache/volatile_tier_impl.cc              \
  utilities/simulator_cache/cache_simulator.cc                  \
  utilities/simulator_cache/sim_cache.cc                        \
  utilities/table_properties_collectors/compact_on_deletion_collector.cc \
  utilities/trace/file_trace_reader_writer.cc                   \
  utilities/transactions/optimistic_transaction.cc              \
  utilities/transactions/optimistic_transaction_db_impl.cc      \
  utilities/transactions/pessimistic_transaction.cc             \
  utilities/transactions/pessimistic_transaction_db.cc          \
  utilities/transactions/snapshot_checker.cc                    \
  utilities/transactions/transaction_base.cc                    \
  utilities/transactions/transaction_db_mutex_impl.cc           \
  utilities/transactions/transaction_lock_mgr.cc                \
  utilities/transactions/transaction_util.cc                    \
  utilities/transactions/write_prepared_txn.cc                  \
  utilities/transactions/write_prepared_txn_db.cc               \
  utilities/transactions/write_unprepared_txn.cc                \
  utilities/transactions/write_unprepared_txn_db.cc             \
  utilities/ttl/db_ttl_impl.cc                                  \
  utilities/write_batch_with_index/write_batch_with_index.cc    \
  utilities/write_batch_with_index/write_batch_with_index_internal.cc    \

ifeq ($(ARMCRC_SOURCE),1)
LIB_SOURCES +=\
  util/crc32c_arm64.cc
endif

ifeq (,$(shell $(CXX) -fsyntax-only -maltivec -xc /dev/null 2>&1))
LIB_SOURCES_ASM =\
  util/crc32c_ppc_asm.S
LIB_SOURCES_C = \
  util/crc32c_ppc.c
else
LIB_SOURCES_ASM =
LIB_SOURCES_C =
endif

TOOL_LIB_SOURCES =                                              \
  tools/ldb_cmd.cc                                              \
  tools/ldb_tool.cc                                             \
  tools/sst_dump_tool.cc                                        \
  utilities/blob_db/blob_dump_tool.cc                           \

ANALYZER_LIB_SOURCES =                                          \
  tools/block_cache_analyzer/block_cache_trace_analyzer.cc      \
  tools/trace_analyzer_tool.cc                                  \

MOCK_LIB_SOURCES =                                              \
  table/mock_table.cc                                           \
  test_util/fault_injection_test_fs.cc				\
  test_util/fault_injection_test_env.cc

BENCH_LIB_SOURCES =                                             \
  tools/db_bench_tool.cc                                        \

STRESS_LIB_SOURCES =                                            \
  db_stress_tool/batched_ops_stress.cc                         \
  db_stress_tool/cf_consistency_stress.cc                      \
  db_stress_tool/db_stress_common.cc                           \
  db_stress_tool/db_stress_driver.cc                           \
  db_stress_tool/db_stress_test_base.cc                        \
  db_stress_tool/db_stress_gflags.cc                           \
  db_stress_tool/db_stress_shared_state.cc                     \
  db_stress_tool/db_stress_tool.cc                             \
  db_stress_tool/no_batched_ops_stress.cc                      \

TEST_LIB_SOURCES =                                              \
  db/db_test_util.cc                                            \
  test_util/testharness.cc                                      \
  test_util/testutil.cc                                         \
  utilities/cassandra/test_utils.cc                             \

FOLLY_SOURCES = \
  third-party/folly/folly/detail/Futex.cpp                                     \
  third-party/folly/folly/synchronization/AtomicNotification.cpp               \
  third-party/folly/folly/synchronization/DistributedMutex.cpp                 \
  third-party/folly/folly/synchronization/ParkingLot.cpp                       \
  third-party/folly/folly/synchronization/WaitOptions.cpp                      \

MAIN_SOURCES =                                                          \
  cache/cache_bench.cc                                                  \
  cache/cache_test.cc                                                   \
  db_stress_tool/db_stress.cc                                           \
  db/blob_file_addition_test.cc                                         \
  db/blob_file_garbage_test.cc                                          \
  db/column_family_test.cc                                              \
  db/compact_files_test.cc                                              \
  db/compaction/compaction_iterator_test.cc                             \
  db/compaction/compaction_job_test.cc                                  \
  db/compaction/compaction_job_stats_test.cc                            \
  db/compaction/compaction_picker_test.cc                               \
  db/comparator_db_test.cc                                              \
  db/corruption_test.cc                                                 \
  db/cuckoo_table_db_test.cc                                            \
  db/db_basic_test.cc                                                   \
  db/db_blob_index_test.cc                                              \
  db/db_block_cache_test.cc                                             \
  db/db_bloom_filter_test.cc                                            \
  db/db_compaction_filter_test.cc                                       \
  db/db_compaction_test.cc                                              \
  db/db_dynamic_level_test.cc                                           \
  db/db_encryption_test.cc                                              \
  db/db_flush_test.cc                                                   \
  db/db_inplace_update_test.cc                                          \
  db/db_io_failure_test.cc                                              \
  db/db_iter_test.cc                                                    \
  db/db_iter_stress_test.cc                                             \
  db/db_iterator_test.cc                                                \
  db/db_log_iter_test.cc                                                \
  db/db_memtable_test.cc                                                \
  db/db_merge_operator_test.cc                                          \
  db/db_merge_operand_test.cc                                          	\
  db/db_options_test.cc                                                 \
  db/db_properties_test.cc                                              \
  db/db_range_del_test.cc                                               \
  db/db_impl/db_secondary_test.cc                                       \
  db/db_sst_test.cc                                                     \
  db/db_statistics_test.cc                                              \
  db/db_table_properties_test.cc                                        \
  db/db_tailing_iter_test.cc                                            \
  db/db_test.cc                                                         \
  db/db_test2.cc                                                        \
  db/db_universal_compaction_test.cc                                    \
  db/db_wal_test.cc                                                     \
  db/db_write_test.cc                                                   \
  db/dbformat_test.cc                                                   \
  db/deletefile_test.cc                                                 \
  db/env_timed_test.cc                                                  \
  db/error_handler_fs_test.cc						\
  db/external_sst_file_basic_test.cc                                    \
  db/external_sst_file_test.cc                                          \
  db/fault_injection_test.cc                                            \
  db/file_indexer_test.cc                                               \
  db/file_reader_writer_test.cc                                         \
  db/filename_test.cc                                                   \
  db/flush_job_test.cc                                                  \
  db/hash_table_test.cc                                                 \
  db/hash_test.cc                                                       \
  db/heap_test.cc                                                       \
  db/listener_test.cc                                                   \
  db/log_test.cc                                                        \
  db/lru_cache_test.cc                                                  \
  db/manual_compaction_test.cc                                          \
  db/memtable_list_test.cc                                              \
  db/merge_helper_test.cc                                               \
  db/merge_test.cc                                                      \
  db/obsolete_files_test.cc						                                  \
  db/options_settable_test.cc                                           \
  db/options_file_test.cc                                               \
  db/perf_context_test.cc                                               \
  db/persistent_cache_test.cc                                           \
  db/plain_table_db_test.cc                                             \
  db/prefix_test.cc                                                     \
  db/repair_test.cc                                                     \
  db/range_del_aggregator_test.cc                                       \
  db/range_del_aggregator_bench.cc                                      \
  db/range_tombstone_fragmenter_test.cc                                 \
  db/table_properties_collector_test.cc                                 \
  db/util_merge_operators_test.cc                                       \
  db/version_builder_test.cc                                            \
  db/version_edit_test.cc                                               \
  db/version_set_test.cc                                                \
  db/wal_manager_test.cc                                                \
  db/write_batch_test.cc                                                \
  db/write_callback_test.cc                                             \
  db/write_controller_test.cc                                           \
  env/env_basic_test.cc                                                 \
  env/env_test.cc                                                       \
  env/mock_env_test.cc                                                  \
  logging/auto_roll_logger_test.cc                                      \
  logging/env_logger_test.cc                                            \
  logging/event_logger_test.cc                                          \
  memory/arena_test.cc                                                  \
  memtable/inlineskiplist_test.cc                                       \
  memtable/memtablerep_bench.cc                                         \
  memtable/skiplist_test.cc                                             \
  memtable/write_buffer_manager_test.cc                                 \
  monitoring/histogram_test.cc                                          \
  monitoring/iostats_context_test.cc                                    \
  monitoring/statistics_test.cc                                         \
  monitoring/stats_history_test.cc                                      \
  options/options_test.cc                                               \
  table/block_based/block_based_filter_block_test.cc                    \
  table/block_based/block_test.cc                                       \
  table/block_based/data_block_hash_index_test.cc                       \
  table/block_based/full_filter_block_test.cc                           \
  table/block_based/partitioned_filter_block_test.cc                    \
  table/cleanable_test.cc                                               \
  table/cuckoo/cuckoo_table_builder_test.cc                             \
  table/cuckoo/cuckoo_table_reader_test.cc                              \
  table/merger_test.cc                                                  \
  table/sst_file_reader_test.cc                                         \
  table/table_reader_bench.cc                                           \
  table/table_test.cc                                                   \
  third-party/gtest-1.7.0/fused-src/gtest/gtest-all.cc                  \
  tools/block_cache_analyzer/block_cache_trace_analyzer_test.cc         \
  tools/block_cache_analyzer/block_cache_trace_analyzer_tool.cc         \
  tools/db_bench.cc                                                     \
  tools/db_bench_tool_test.cc                                           \
  tools/db_sanity_test.cc                                               \
  tools/ldb_cmd_test.cc                                                 \
  tools/reduce_levels_test.cc                                           \
  tools/sst_dump_test.cc                                                \
  tools/trace_analyzer_test.cc				             	                    \
  trace_replay/block_cache_tracer_test.cc                               \
  util/autovector_test.cc                                               \
  util/bloom_test.cc                                                    \
  util/coding_test.cc                                                   \
  util/crc32c_test.cc                                                   \
  util/defer_test.cc                                                    \
  util/dynamic_bloom_test.cc                                            \
  util/filelock_test.cc                                                 \
  util/log_write_bench.cc                                               \
  util/rate_limiter_test.cc                                             \
  util/random_test.cc                                                   \
  util/repeatable_thread_test.cc                                        \
  util/slice_test.cc                                                    \
  util/slice_transform_test.cc                                          \
  util/timer_queue_test.cc                                              \
  util/thread_list_test.cc                                              \
  util/thread_local_test.cc                                             \
  utilities/backupable/backupable_db_test.cc                            \
  utilities/blob_db/blob_db_test.cc                                     \
  utilities/cassandra/cassandra_format_test.cc                          \
  utilities/cassandra/cassandra_functional_test.cc                      \
  utilities/cassandra/cassandra_row_merge_test.cc                       \
  utilities/cassandra/cassandra_serialize_test.cc                       \
  utilities/checkpoint/checkpoint_test.cc                               \
  utilities/memory/memory_test.cc                                       \
  utilities/merge_operators/string_append/stringappend_test.cc          \
  utilities/object_registry_test.cc                                     \
  utilities/option_change_migration/option_change_migration_test.cc     \
  utilities/options/options_util_test.cc                                \
  utilities/simulator_cache/cache_simulator_test.cc                     \
  utilities/simulator_cache/sim_cache_test.cc                           \
  utilities/table_properties_collectors/compact_on_deletion_collector_test.cc  \
  utilities/transactions/optimistic_transaction_test.cc                 \
  utilities/transactions/transaction_test.cc                            \
  utilities/transactions/write_prepared_transaction_test.cc             \
  utilities/transactions/write_unprepared_transaction_test.cc           \
  utilities/ttl/ttl_test.cc                                             \
  utilities/write_batch_with_index/write_batch_with_index_test.cc       \

JNI_NATIVE_SOURCES =                                          \
  java/rocksjni/backupenginejni.cc                            \
  java/rocksjni/backupablejni.cc                              \
  java/rocksjni/checkpoint.cc                                 \
  java/rocksjni/clock_cache.cc                                \
  java/rocksjni/columnfamilyhandle.cc                         \
  java/rocksjni/compact_range_options.cc                      \
  java/rocksjni/compaction_filter.cc                          \
  java/rocksjni/compaction_filter_factory.cc                  \
  java/rocksjni/compaction_filter_factory_jnicallback.cc      \
  java/rocksjni/compaction_job_info.cc                        \
  java/rocksjni/compaction_job_stats.cc                       \
  java/rocksjni/compaction_options.cc                         \
  java/rocksjni/compaction_options_fifo.cc                    \
  java/rocksjni/compaction_options_universal.cc               \
  java/rocksjni/comparator.cc                                 \
  java/rocksjni/comparatorjnicallback.cc                      \
  java/rocksjni/compression_options.cc                        \
  java/rocksjni/env.cc                                        \
  java/rocksjni/env_options.cc                                \
  java/rocksjni/ingest_external_file_options.cc               \
  java/rocksjni/filter.cc                                     \
  java/rocksjni/iterator.cc                                   \
  java/rocksjni/jnicallback.cc                                \
  java/rocksjni/loggerjnicallback.cc                          \
  java/rocksjni/lru_cache.cc                                  \
  java/rocksjni/memtablejni.cc                                \
  java/rocksjni/memory_util.cc                                \
  java/rocksjni/merge_operator.cc                             \
  java/rocksjni/native_comparator_wrapper_test.cc             \
  java/rocksjni/optimistic_transaction_db.cc                  \
  java/rocksjni/optimistic_transaction_options.cc             \
  java/rocksjni/options.cc                                    \
  java/rocksjni/options_util.cc                               \
  java/rocksjni/persistent_cache.cc                           \
  java/rocksjni/ratelimiterjni.cc                             \
  java/rocksjni/remove_emptyvalue_compactionfilterjni.cc      \
  java/rocksjni/cassandra_compactionfilterjni.cc              \
  java/rocksjni/cassandra_value_operator.cc                   \
  java/rocksjni/restorejni.cc                                 \
  java/rocksjni/rocks_callback_object.cc                      \
  java/rocksjni/rocksjni.cc                                   \
  java/rocksjni/rocksdb_exception_test.cc                     \
  java/rocksjni/slice.cc                                      \
  java/rocksjni/snapshot.cc                                   \
  java/rocksjni/sst_file_manager.cc                           \
  java/rocksjni/sst_file_writerjni.cc                         \
  java/rocksjni/sst_file_readerjni.cc                         \
  java/rocksjni/sst_file_reader_iterator.cc                   \
  java/rocksjni/statistics.cc                                 \
  java/rocksjni/statisticsjni.cc                              \
  java/rocksjni/table.cc                                      \
  java/rocksjni/table_filter.cc                               \
  java/rocksjni/table_filter_jnicallback.cc                   \
  java/rocksjni/thread_status.cc                              \
  java/rocksjni/trace_writer.cc                               \
  java/rocksjni/trace_writer_jnicallback.cc                   \
  java/rocksjni/transaction.cc                                \
  java/rocksjni/transaction_db.cc                             \
  java/rocksjni/transaction_options.cc                        \
  java/rocksjni/transaction_db_options.cc                     \
  java/rocksjni/transaction_log.cc                            \
  java/rocksjni/transaction_notifier.cc                       \
  java/rocksjni/transaction_notifier_jnicallback.cc           \
  java/rocksjni/ttl.cc                                        \
  java/rocksjni/wal_filter.cc                                 \
  java/rocksjni/wal_filter_jnicallback.cc                     \
  java/rocksjni/write_batch.cc                                \
  java/rocksjni/writebatchhandlerjnicallback.cc               \
  java/rocksjni/write_batch_test.cc                           \
  java/rocksjni/write_batch_with_index.cc                     \
  java/rocksjni/write_buffer_manager.cc
