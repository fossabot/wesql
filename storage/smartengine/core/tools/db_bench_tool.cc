//  Portions Copyright (c) 2023, ApeCloud Inc Holding Limited
//  Portions Copyright (c) 2020, Alibaba Group Holding Limited
//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif

#ifdef GFLAGS
#ifdef NUMA
#include <numa.h>
#include <numaif.h>
#endif
#ifndef OS_WIN
#include <unistd.h>
#endif
#include <fcntl.h>
#include <gflags/gflags.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <random>
#include <thread>
#include <unordered_map>

#include "db/db_impl.h"
#include "db/version_set.h"
#include "monitoring/histogram.h"
#include "monitoring/statistics.h"
#include "port/port.h"
#include "port/stack_trace.h"
#include "util/compression.h"
#include "util/crc32c.h"
#include "util/mutexlock.h"
#include "util/random.h"
#include "util/string_util.h"
#include "util/testutil.h"
#include "util/transaction_test_util.h"
#include "util/xxhash.h"
#include "memory/mod_info.h"
#include "util/lock_free_fixed_queue.h"
#include "smartengine/cache.h"
#include "smartengine/db.h"
#include "smartengine/env.h"
#include "smartengine/filter_policy.h"
#include "smartengine/memtablerep.h"
#include "smartengine/options.h"
// TODO @zhencheng : use QUERY TRACE to print this.
// #include "smartengine/perf_context.h"
#include "smartengine/perf_level.h"
#include "smartengine/rate_limiter.h"
#include "smartengine/slice.h"
#include "smartengine/utilities/object_registry.h"
#include "smartengine/utilities/optimistic_transaction_db.h"
#include "smartengine/utilities/transaction.h"
#include "smartengine/utilities/transaction_db.h"
#include "smartengine/write_batch.h"
#include "fpga/fpga_compaction_job.h"

#ifdef OS_WIN
#include <io.h>  // open/close
#endif

using GFLAGS::ParseCommandLineFlags;
using GFLAGS::RegisterFlagValidator;
using GFLAGS::SetUsageMessage;

using namespace smartengine;
using namespace common;
using namespace table;
using namespace monitor;
using namespace util;
using namespace db;
using namespace cache;
using namespace storage;
using namespace memtable;
using namespace memory;
#define STRESS_CHECK_TIME(OPS)\
do {                                                                        \
  if (OPS % 100 == 0 && FLAGS_env->NowMicros() > stress_start_time &&       \
      (FLAGS_env->NowMicros() - stress_start_time) * 1e-6 > FLAGS_duration) \
    return;                                                                 \
} while(0)

DEFINE_string(
    benchmarks,
    "fillseq,"
    "fillseqdeterministic,"
    "fillsync,"
    "fillrandom,"
    "filluniquerandomdeterministic,"
    "overwrite,"
    "readrandom,"
    "newiterator,"
    "newiteratorwhilewriting,"
    "seekrandom,"
    "seekrandomwhilewriting,"
    "seekrandomwhilemerging,"
    "readseq,"
    "readreverse,"
    "compact,"
    "readrandom,"
    "multireadrandom,"
    "readseq,"
    "readtocache,"
    "readreverse,"
    "readwhilewriting,"
    "readwhilemerging,"
    "readrandomwriterandom,"
    "updaterandom,"
    "randomwithverify,"
    "fill100K,"
    "crc32c,"
    "xxhash,"
    "compress,"
    "uncompress,"
    "acquireload,"
    "fillseekseq,"
    "randomtransaction,"
    "randomreplacekeys,"
    "timeseries,"
    "stress",

    "Comma-separated list of operations to run in the specified"
    " order. Available benchmarks:\n"
    "\tfillseq       -- write N values in sequential key"
    " order in async mode\n"
    "\tfillseqdeterministic       -- write N values in the specified"
    " key order and keep the shape of the LSM tree\n"
    "\tfillrandom    -- write N values in random key order in async"
    " mode\n"
    "\tfilluniquerandomdeterministic       -- write N values in a random"
    " key order and keep the shape of the LSM tree\n"
    "\toverwrite     -- overwrite N values in random key order in"
    " async mode\n"
    "\tfillsync      -- write N/100 values in random key order in "
    "sync mode\n"
    "\tfill100K      -- write N/1000 100K values in random order in"
    " async mode\n"
    "\tdeleteseq     -- delete N keys in sequential order\n"
    "\tdeleterandom  -- delete N keys in random order\n"
    "\treadseq       -- read N times sequentially\n"
    "\treadtocache   -- 1 thread reading database sequentially\n"
    "\treadreverse   -- read N times in reverse order\n"
    "\treadrandom    -- read N times in random order\n"
    "\treadmissing   -- read N missing keys in random order\n"
    "\treadwhilewriting      -- 1 writer, N threads doing random "
    "reads\n"
    "\treadwhilemerging      -- 1 merger, N threads doing random "
    "reads\n"
    "\treadrandomwriterandom -- N threads doing random-read, "
    "random-write\n"
    "\tprefixscanrandom      -- prefix scan N times in random order\n"
    "\tupdaterandom  -- N threads doing read-modify-write for random "
    "keys\n"
    "\tappendrandom  -- N threads doing read-modify-write with "
    "growing values\n"
    "\tmergerandom   -- same as updaterandom/appendrandom using merge"
    " operator. "
    "Must be used with merge_operator\n"
    "\treadrandommergerandom -- perform N random read-or-merge "
    "operations. Must be used with merge_operator\n"
    "\tnewiterator   -- repeated iterator creation\n"
    "\tseekrandom    -- N random seeks, call Next seek_nexts times "
    "per seek\n"
    "\tseekrandomwhilewriting -- seekrandom and 1 thread doing "
    "overwrite\n"
    "\tseekrandomwhilemerging -- seekrandom and 1 thread doing "
    "merge\n"
    "\tcrc32c        -- repeated crc32c of 4K of data\n"
    "\txxhash        -- repeated xxHash of 4K of data\n"
    "\tacquireload   -- load N*1000 times\n"
    "\tfillseekseq   -- write N values in sequential key, then read "
    "them by seeking to each key\n"
    "\trandomtransaction     -- execute N random transactions and "
    "verify correctness\n"
    "\trandomreplacekeys     -- randomly replaces N keys by deleting "
    "the old version and putting the new version\n\n"
    "\ttimeseries            -- 1 writer generates time series data "
    "and multiple readers doing random reads on id\n\n"
    "Meta operations:\n"
    "\tcompact     -- Compact the entire DB\n"
    "\tstats       -- Print DB stats\n"
    "\tresetstats  -- Reset DB stats\n"
    "\tlevelstats  -- Print the number of files and bytes per level\n"
    "\tsstables    -- Print sstable info\n"
    "\theapprofile -- Dump a heap profile (if supported by this"
    " port)\n");

DEFINE_bool(bench_log, true, "Write to a log file what has been done.");

DEFINE_int64(num, 1000000, "Number of key/values to place in database");

DEFINE_int64(numdistinct, 1000,
             "Number of distinct keys to use. Used in RandomWithVerify to "
             "read/write on fewer keys so that gets are more likely to find the"
             " key and puts are more likely to update the same key");

DEFINE_int64(merge_keys, -1,
             "Number of distinct keys to use for MergeRandom and "
             "ReadRandomMergeRandom. "
             "If negative, there will be FLAGS_num keys.");
DEFINE_int32(num_column_families, 1, "Number of Column Families to use.");

DEFINE_int32(info_log_level, 1, "log level.");
DEFINE_uint64(compaction_type, 0, "compaction_type");
DEFINE_uint64(compaction_mode, 0, "compaction_mode");
DEFINE_uint64(max_log_file_size, 0,
              "The maximal size of the info log file. If the log file"
              "is larger than `max_log_file_size`, a new info log file will"
              "be created."
              "if it is 0, all logs will be written to one log file.");
DEFINE_uint64(log_file_time_to_roll, 0,
              "log file will be rolled"
              "if it has been active longer than `log_file_time_to_roll`."
              "0 means disabled.");
DEFINE_uint64(keep_log_file_num, 1000, "Maximal info log files to be kept.");

DEFINE_int32(
    num_hot_column_families, 0,
    "Number of Hot Column Families. If more than 0, only write to this "
    "number of column families. After finishing all the writes to them, "
    "create new set of column families and insert to them. Only used "
    "when num_column_families > 1.");

DEFINE_int64(reads, -1,
             "Number of read operations to do.  "
             "If negative, do FLAGS_num reads.");

DEFINE_int64(deletes, -1,
             "Number of delete operations to do.  "
             "If negative, do FLAGS_num deletions.");

DEFINE_int32(bloom_locality, 0, "Control bloom filter probes locality");

DEFINE_int64(seed, 0,
             "Seed base for random number generators. "
             "When 0 it is deterministic.");

DEFINE_int32(threads, 1, "Number of concurrent threads to run.");

DEFINE_int32(duration, 0,
             "Time in seconds for the random-ops tests to run."
             " When 0 then num & reads determine the test duration");

DEFINE_int32(value_size, 100, "Size of each value");

DEFINE_int32(seek_nexts, 0,
             "How many times to call Next() after Seek() in "
             "fillseekseq, seekrandom, seekrandomwhilewriting and "
             "seekrandomwhilemerging");

DEFINE_bool(reverse_iterator, false,
            "When true use Prev rather than Next for iterators that do "
            "Seek and then Next");

DEFINE_bool(use_uint64_comparator, false, "use Uint64 user comparator");

DEFINE_bool(pin_slice, true, "use pinnable slice for point lookup");

DEFINE_int64(batch_size, 1, "Batch size");

DEFINE_uint64(update_delete_count, 1000,
              "update and delete ops in stress test");

static bool ValidateKeySize(const char* flagname, int32_t value) {
  return true;
}

static bool ValidateUint32Range(const char* flagname, uint64_t value) {
  if (value > std::numeric_limits<uint32_t>::max()) {
    fprintf(stderr, "Invalid value for --%s: %lu, overflow\n", flagname,
            (unsigned long)value);
    return false;
  }
  return true;
}

DEFINE_int32(key_size, 16, "size of each key");

DEFINE_int32(num_multi_db, 0,
             "Number of DBs used in the benchmark. 0 means single DB.");

DEFINE_double(compression_ratio, 0.5,
              "Arrange to generate values that shrink"
              " to this fraction of their original size after compression");

DEFINE_double(read_random_exp_range, 0.0,
              "Read random's key will be generated using distribution of "
              "num * exp(-r) where r is uniform number from 0 to this value. "
              "The larger the number is, the more skewed the reads are. "
              "Only used in readrandom and multireadrandom benchmarks.");

DEFINE_bool(histogram, false, "Print histogram of operation timings");

DEFINE_bool(enable_numa, false,
            "Make operations aware of NUMA architecture and bind memory "
            "and cpus corresponding to nodes together. In NUMA, memory "
            "in same node as CPUs are closer when compared to memory in "
            "other nodes. Reads can be faster when the process is bound to "
            "CPU and memory of same node. Use \"$numactl --hardware\" command "
            "to see NUMA memory architecture.");

DEFINE_int64(db_write_buffer_size, Options().db_write_buffer_size,
             "Number of bytes to buffer in all active memtables");

DEFINE_int64(db_total_write_buffer_size, Options().db_total_write_buffer_size,
             "Number of bytes to buffer in all memtables");

DEFINE_int64(write_buffer_size, Options().write_buffer_size,
             "Number of bytes to buffer in memtable before compacting");

DEFINE_int64(flush_delete_percent, Options().flush_delete_percent,
             "Percent for delete triggered switch and Flush");

DEFINE_int64(compaction_delete_percent, Options().compaction_delete_percent,
             "Percent for delete triggered switch and Flush");

DEFINE_int64(flush_delete_percent_trigger, Options().flush_delete_percent_trigger,
             "Percent trigger for delete triggered switch");

DEFINE_int64(flush_delete_record_trigger, Options().flush_delete_record_trigger,
             "Record trigger for delete triggered switch");

DEFINE_int32(max_write_buffer_number, Options().max_write_buffer_number,
             "The number of in-memory memtables. Each memtable is of size"
             "write_buffer_size.");

DEFINE_int32(min_write_buffer_number_to_merge,
             Options().min_write_buffer_number_to_merge,
             "The minimum number of write buffers that will be merged together"
             "before writing to storage. This is cheap because it is an"
             "in-memory merge. If this feature is not enabled, then all these"
             "write buffers are flushed to L0 as separate files and this "
             "increases read amplification because a get request has to check"
             " in all of these files. Also, an in-memory merge may result in"
             " writing less data to storage if there are duplicate records "
             " in each of these individual write buffers.");

DEFINE_int32(max_write_buffer_number_to_maintain,
             Options().max_write_buffer_number_to_maintain,
             "The total maximum number of write buffers to maintain in memory "
             "including copies of buffers that have already been flushed. "
             "Unlike max_write_buffer_number, this parameter does not affect "
             "flushing. This controls the minimum amount of write history "
             "that will be available in memory for conflict checking when "
             "Transactions are used. If this value is too low, some "
             "transactions may fail at commit time due to not being able to "
             "determine whether there were any write conflicts. Setting this "
             "value to 0 will cause write buffers to be freed immediately "
             "after they are flushed.  If this value is set to -1, "
             "'max_write_buffer_number' will be used.");

DEFINE_int32(max_background_compactions, Options().max_background_compactions,
             "The maximum number of concurrent background compactions"
             " that can occur in parallel.");

DEFINE_int32(base_background_compactions, Options().base_background_compactions,
             "The base number of concurrent background compactions"
             " to occur in parallel.");

DEFINE_uint64(subcompactions, 1,
              "Maximum number of subcompactions to divide L0-L1 compactions "
              "into.");
static const bool FLAGS_subcompactions_dummy __attribute__((unused)) =
    RegisterFlagValidator(&FLAGS_subcompactions, &ValidateUint32Range);

DEFINE_int32(max_background_flushes, Options().max_background_flushes,
             "The maximum number of concurrent background flushes"
             " that can occur in parallel.");

DEFINE_int32(max_batch_group_slot_array_size,
             Options().batch_group_slot_array_size,
             "The maximum number of batch group slot");

DEFINE_bool(async_mode, false, "whether run in async mode defalut false");

DEFINE_int32(max_batch_group_size, Options().batch_group_max_group_size,
             "The maximum number of batch group size");

DEFINE_int32(max_batch_group_leader_wait_time_us,
             Options().batch_group_max_leader_wait_time_us,
             "The maximum batch group leader wait time in us ");

DEFINE_int32(max_log_buffer_num, Options().concurrent_writable_file_buffer_num,
             "The maximum num for concurrent_direct_file_writer");

DEFINE_int32(
    max_single_log_buffer_size,
    Options().concurrent_writable_file_single_buffer_size,
    "The maximum single buffer size for concurrent_direct_file_writer");

DEFINE_int32(
    max_log_buffer_switch_limit,
    Options().concurrent_writable_file_buffer_switch_limit,
    "The maximum switch buffer limit for concurrent_direct_file_writer");

DEFINE_int32(cpu_compaction_thread_num, Options().cpu_compaction_thread_num,
    "cpu compaction thread num");

DEFINE_int32(fpga_compaction_thread_num, Options().fpga_compaction_thread_num,
    "fpga compaction thread num");

DEFINE_int32(fpga_device_id, Options().fpga_device_id,
    "fpga device id");

DEFINE_int32(arena_block_size, Options().arena_block_size,
    "arena_block_size");


DEFINE_bool(use_direct_write_for_wal, true, "wether use direct write for wal");

static CompactionStyle FLAGS_compaction_style_e;
DEFINE_int32(compaction_style, (int32_t)Options().compaction_style,
             "style of compaction: level-based, universal and fifo");

static CompactionPri FLAGS_compaction_pri_e;
DEFINE_int32(compaction_pri, (int32_t)Options().compaction_pri,
             "priority of files to compaction: by size or by data age");

DEFINE_int32(universal_size_ratio, 0,
             "Percentage flexibility while comparing file size"
             " (for universal compaction only).");

DEFINE_int32(universal_min_merge_width, 0,
             "The minimum number of files in a"
             " single compaction run (for universal compaction only).");

DEFINE_int32(universal_max_merge_width, 0,
             "The max number of files to compact"
             " in universal style compaction");

DEFINE_int32(universal_max_size_amplification_percent, 0,
             "The max size amplification for universal style compaction");

DEFINE_int32(universal_compression_size_percent, -1,
             "The percentage of the database to compress for universal "
             "compaction. -1 means compress everything.");

DEFINE_bool(universal_allow_trivial_move, false,
            "Allow trivial move in universal compaction.");

DEFINE_int64(cache_size, 8 << 20,  // 8MB
             "Number of bytes to use as a cache of uncompressed data");

DEFINE_int32(cache_numshardbits, 6,
             "Number of shards for the block cache"
             " is 2 ** cache_numshardbits. Negative means use default settings."
             " This is applied only if FLAGS_cache_size is non-negative.");

DEFINE_double(cache_high_pri_pool_ratio, 0.0,
              "Ratio of block cache reserve for high pri blocks. "
              "If > 0.0, we also enable "
              "cache_index_and_filter_blocks_with_high_priority.");

DEFINE_bool(use_clock_cache, false,
            "Replace default LRU block cache with clock cache.");

DEFINE_bool(use_xcache, false,
            "Replace default LRU block cache with XCache.");

DEFINE_int64(simcache_size, -1,
             "Number of bytes to use as a simcache of "
             "uncompressed data. Nagative value disables simcache.");

DEFINE_bool(cache_index_and_filter_blocks, false,
            "Cache index/filter blocks in block cache.");

DEFINE_bool(pin_l0_filter_and_index_blocks_in_cache, false,
            "Pin index/filter blocks of L0 files in block cache.");

DEFINE_int32(block_size,
             static_cast<int32_t>(BlockBasedTableOptions().block_size),
             "Number of bytes in a block.");

DEFINE_int32(block_restart_interval,
             BlockBasedTableOptions().block_restart_interval,
             "Number of keys between restart points "
             "for delta encoding of keys in data block.");

DEFINE_int32(index_block_restart_interval,
             BlockBasedTableOptions().index_block_restart_interval,
             "Number of keys between restart points "
             "for delta encoding of keys in index block.");

DEFINE_int32(read_amp_bytes_per_bit,
             BlockBasedTableOptions().read_amp_bytes_per_bit,
             "Number of bytes per bit to be used in block read-amp bitmap");

DEFINE_int64(compressed_cache_size, -1,
             "Number of bytes to use as a cache of compressed data.");

DEFINE_int64(row_cache_size, 0,
             "Number of bytes to use as a cache of individual rows"
             " (0 = disabled).");

DEFINE_int32(open_files, Options().max_open_files,
             "Maximum number of files to keep open at the same time"
             " (use default if == 0)");

DEFINE_int32(file_opening_threads, Options().max_file_opening_threads,
             "If open_files is set to -1, this option set the number of "
             "threads that will be used to open files during DB::Open()");

DEFINE_int32(new_table_reader_for_compaction_inputs, true,
             "If true, uses a separate file handle for compaction inputs");

DEFINE_int32(compaction_readahead_size, 0, "Compaction readahead size");

DEFINE_int32(random_access_max_buffer_size, 1024 * 1024,
             "Maximum windows randomaccess buffer size");

DEFINE_int32(writable_file_max_buffer_size, 1024 * 1024,
             "Maximum write buffer for Writable File");

DEFINE_int32(bloom_bits, -1,
             "Bloom filter bits per key. Negative means"
             " use default settings.");

DEFINE_bool(memtable_use_huge_page, false,
            "Try to use huge page in memtables.");

DEFINE_bool(use_existing_db, false,
            "If true, do not destroy the existing"
            " database.  If you set this flag and also specify a benchmark that"
            " wants a fresh database, that benchmark will fail.");

DEFINE_bool(flush_write_buffer_after_test, false,
            "Flush all write buffer to disk before after test");

DEFINE_bool(show_table_properties, false,
            "If true, then per-level table"
            " properties will be printed on every stats-interval when"
            " stats_interval is set and stats_per_interval is on.");

DEFINE_string(db, "", "Use the db with the following name.");

// Read cache flags

DEFINE_string(read_cache_path, "",
              "If not empty string, a read cache will be used in this path");

DEFINE_int64(read_cache_size, 4LL * 1024 * 1024 * 1024,
             "Maximum size of the read cache");

DEFINE_bool(read_cache_direct_write, true,
            "Whether to use Direct IO for writing to the read cache");

DEFINE_bool(read_cache_direct_read, true,
            "Whether to use Direct IO for reading from read cache");

static bool ValidateCacheNumshardbits(const char* flagname, int32_t value) {
  if (value >= 20) {
    fprintf(stderr, "Invalid value for --%s: %d, must be < 20\n", flagname,
            value);
    return false;
  }
  return true;
}

DEFINE_bool(verify_checksum, false,
            "Verify checksum for every block read"
            " from storage");

DEFINE_bool(statistics, false, "Database statistics");
DEFINE_string(statistics_string, "", "Serialized statistics string");
static class std::shared_ptr<Statistics> dbstats;

DEFINE_int64(writes, -1,
             "Number of write operations to do. If negative, do"
             " --num reads.");

DEFINE_bool(finish_after_writes, false,
            "Write thread terminates after all writes are finished");

DEFINE_bool(sync, false, "Sync all writes to disk");

DEFINE_bool(use_fsync, false, "If true, issue fsync instead of fdatasync");

DEFINE_bool(disable_wal, false, "If true, do not write WAL for write.");

DEFINE_string(wal_dir, "", "If not empty, use the given dir for WAL");

DEFINE_int32(num_levels, 7, "The total number of levels");

DEFINE_int64(target_file_size_base, Options().target_file_size_base,
             "Target file size at level-1");

DEFINE_int32(target_file_size_multiplier, Options().target_file_size_multiplier,
             "A multiplier to compute target level-N file size (N >= 2)");

DEFINE_uint64(max_bytes_for_level_base, Options().max_bytes_for_level_base,
              "Max bytes for level-1");

DEFINE_bool(level_compaction_dynamic_level_bytes, false,
            "Whether level size base is dynamic");

DEFINE_double(max_bytes_for_level_multiplier, 10,
              "A multiplier to compute max bytes for level-N (N >= 2)");

static std::vector<int> FLAGS_max_bytes_for_level_multiplier_additional_v;
DEFINE_string(max_bytes_for_level_multiplier_additional, "",
              "A vector that specifies additional fanout per level");

DEFINE_int32(level0_stop_writes_trigger, Options().level0_stop_writes_trigger,
             "Number of files in level-0"
             " that will trigger put stop.");

DEFINE_int32(level0_slowdown_writes_trigger,
             Options().level0_slowdown_writes_trigger,
             "Number of files in level-0"
             " that will slow down writes.");

DEFINE_int32(level0_file_num_compaction_trigger,
             Options().level0_file_num_compaction_trigger,
             "Number of files in level-0"
             " when compactions start");

DEFINE_int32(level1_extents_major_compaction_trigger,
             Options().level1_extents_major_compaction_trigger,
             "Number of extents in level-1"
             " when major compaction start");

DEFINE_int32(level2_usage_percent,
             Options().level2_usage_percent,
             "Usage percent of extents in level-2"
             " when auto major self compaction start");

DEFINE_int32(scan_add_blocks_limit,
             Options().scan_add_blocks_limit,
             "Limit of blocks be into cache"
             " when scan");

DEFINE_int32(level0_layer_num_compaction_trigger,
             Options().level0_layer_num_compaction_trigger,
             "Number of layers in level-0"
             " when compactions start");

DEFINE_int32(minor_window_size,
             Options().minor_window_size,
             "Number of window size for MinorCompaction");

static bool ValidateInt32Percent(const char* flagname, int32_t value) {
  if (value <= 0 || value >= 100) {
    fprintf(stderr, "Invalid value for --%s: %d, 0< pct <100 \n", flagname,
            value);
    return false;
  }
  return true;
}
DEFINE_int32(readwritepercent, 90,
             "Ratio of reads to reads/writes (expressed"
             " as percentage) for the ReadRandomWriteRandom workload. The "
             "default value 90 means 90% operations out of all reads and writes"
             " operations are reads. In other words, 9 gets for every 1 put.");

DEFINE_int32(mergereadpercent, 70,
             "Ratio of merges to merges&reads (expressed"
             " as percentage) for the ReadRandomMergeRandom workload. The"
             " default value 70 means 70% out of all read and merge operations"
             " are merges. In other words, 7 merges for every 3 gets.");

DEFINE_int32(deletepercent, 2,
             "Percentage of deletes out of reads/writes/"
             "deletes (used in RandomWithVerify only). RandomWithVerify "
             "calculates writepercent as (100 - FLAGS_readwritepercent - "
             "deletepercent), so deletepercent must be smaller than (100 - "
             "FLAGS_readwritepercent)");

DEFINE_bool(optimize_filters_for_hits, false,
            "Optimizes bloom filters for workloads for most lookups return "
            "a value. For now this doesn't create bloom filters for the max "
            "level of the LSM to reduce metadata that should fit in RAM. ");

DEFINE_uint64(delete_obsolete_files_period_micros, 0,
              "Ignored. Left here for backward compatibility");

DEFINE_int64(range_tombstone_width, 100, "Number of keys in tombstone's range");

DEFINE_int64(max_num_range_tombstones, 0,
             "Maximum number of range tombstones "
             "to insert.");

#ifndef ROCKSDB_LITE
DEFINE_bool(optimistic_transaction_db, false,
            "Open a OptimisticTransactionDB instance. "
            "Required for randomtransaction benchmark.");

DEFINE_bool(transaction_db, false,
            "Open a TransactionDB instance. "
            "Required for randomtransaction benchmark.");

DEFINE_uint64(transaction_sets, 2,
              "Number of keys each transaction will "
              "modify (use in RandomTransaction only).  Max: 9999");

DEFINE_bool(transaction_set_snapshot, false,
            "Setting to true will have each transaction call SetSnapshot()"
            " upon creation.");

DEFINE_int32(transaction_sleep, 0,
             "Max microseconds to sleep in between "
             "reading and writing a value (used in RandomTransaction only). ");

DEFINE_uint64(transaction_lock_timeout, 100,
              "If using a transaction_db, specifies the lock wait timeout in"
              " milliseconds before failing a transaction waiting on a lock");
DEFINE_string(
    options_file, "",
    "The path to a SE options file.  If specified, then db_bench will "
    "run with the SE options in the default sub table of the "
    "specified options file. "
    "Note that with this setting, db_bench will ONLY accept the following "
    "SE options related command-line arguments, all other arguments "
    "that are related to SE options will be ignored:\n"
    "\t--use_existing_db\n"
    "\t--statistics\n"
    "\t--row_cache_size\n"
    "\t--row_cache_numshardbits\n"
    "\t--enable_io_prio\n"
    "\t--dump_malloc_stats\n"
    "\t--num_multi_db\n");

DEFINE_uint64(fifo_compaction_max_table_files_size_mb, 0,
              "The limit of total table file sizes to trigger FIFO compaction");
#endif  // ROCKSDB_LITE

DEFINE_bool(report_bg_io_stats, false,
            "Measure times spents on I/Os while in compactions. ");

static enum CompressionType StringToCompressionType(const char* ctype) {
  assert(ctype);

  if (!strcasecmp(ctype, "none"))
    return kNoCompression;
  else if (!strcasecmp(ctype, "snappy"))
    return kSnappyCompression;
  else if (!strcasecmp(ctype, "zlib"))
    return kZlibCompression;
  else if (!strcasecmp(ctype, "bzip2"))
    return kBZip2Compression;
  else if (!strcasecmp(ctype, "lz4"))
    return kLZ4Compression;
  else if (!strcasecmp(ctype, "lz4hc"))
    return kLZ4HCCompression;
  else if (!strcasecmp(ctype, "xpress"))
    return kXpressCompression;
  else if (!strcasecmp(ctype, "zstd"))
    return kZSTD;

  fprintf(stdout, "Cannot parse compression type '%s'\n", ctype);
  return kSnappyCompression;  // default value
}

static std::string ColumnFamilyName(size_t i) {
  if (i == 0) {
    return kDefaultColumnFamilyName;
  } else {
    char name[100];
    snprintf(name, sizeof(name), "column_family_name_%06zu", i);
    return std::string(name);
  }
}

DEFINE_string(compression_type, "zlib",
              "Algorithm to use to compress the database");
static enum CompressionType FLAGS_compression_type_e = kZlibCompression;

DEFINE_int32(compression_level, -1,
             "Compression level. For zlib this should be -1 for the "
             "default level, or between 0 and 9.");

DEFINE_int32(compression_max_dict_bytes, 0,
             "Maximum size of dictionary used to prime the compression "
             "library.");

static bool ValidateCompressionLevel(const char* flagname, int32_t value) {
  if (value < -1 || value > 9) {
    fprintf(stderr, "Invalid value for --%s: %d, must be between -1 and 9\n",
            flagname, value);
    return false;
  }
  return true;
}

static const bool FLAGS_compression_level_dummy __attribute__((unused)) =
    RegisterFlagValidator(&FLAGS_compression_level, &ValidateCompressionLevel);

DEFINE_int32(min_level_to_compress, -1,
             "If non-negative, compression starts"
             " from this level. Levels with number < min_level_to_compress are"
             " not compressed. Otherwise, apply compression_type to "
             "all levels.");

static bool ValidateTableCacheNumshardbits(const char* flagname,
                                           int32_t value) {
  if (0 >= value || value > 20) {
    fprintf(stderr, "Invalid value for --%s: %d, must be  0 < val <= 20\n",
            flagname, value);
    return false;
  }
  return true;
}
DEFINE_int32(table_cache_numshardbits, 4, "");

#ifndef ROCKSDB_LITE
DEFINE_string(env_uri, "",
              "URI for registry Env lookup. Mutually exclusive"
              " with --hdfs.");
#endif  // ROCKSDB_LITE
DEFINE_string(hdfs, "",
              "Name of hdfs environment. Mutually exclusive with"
              " --env_uri.");
static Env* FLAGS_env = Env::Default();

DEFINE_int64(stats_interval, 0,
             "Stats are reported every N operations when "
             "this is greater than zero. When 0 the interval grows over time.");

DEFINE_int64(stats_interval_seconds, 0,
             "Report stats every N seconds. This "
             "overrides stats_interval when both are > 0.");

DEFINE_int32(stats_per_interval, 0,
             "Reports additional stats per interval when"
             " this is greater than 0.");

DEFINE_int64(report_interval_seconds, 0,
             "If greater than zero, it will write simple stats in CVS format "
             "to --report_file every N seconds");

DEFINE_string(report_file, "report.csv",
              "Filename where some simple stats are reported to (if "
              "--report_interval_seconds is bigger than 0)");

DEFINE_int32(thread_status_per_interval, 0,
             "Takes and report a snapshot of the current status of each thread"
             " when this is greater than 0.");

DEFINE_int32(perf_level, PerfLevel::kDisable, "Level of perf collection");

static bool ValidateRateLimit(const char* flagname, double value) {
  const double EPSILON = 1e-10;
  if (value < -EPSILON) {
    fprintf(stderr, "Invalid value for --%s: %12.6f, must be >= 0.0\n",
            flagname, value);
    return false;
  }
  return true;
}
DEFINE_double(soft_rate_limit, 0.0, "DEPRECATED");

DEFINE_double(hard_rate_limit, 0.0, "DEPRECATED");

DEFINE_uint64(soft_pending_compaction_bytes_limit, 64ull * 1024 * 1024 * 1024,
              "Slowdown writes if pending compaction bytes exceed this number");

DEFINE_uint64(hard_pending_compaction_bytes_limit, 128ull * 1024 * 1024 * 1024,
              "Stop writes if pending compaction bytes exceed this number");

DEFINE_uint64(delayed_write_rate, 8388608u,
              "Limited bytes allowed to DB when soft_rate_limit or "
              "level0_slowdown_writes_trigger triggers");

DEFINE_bool(allow_concurrent_memtable_write, false,
            "Allow multi-writers to update mem tables in parallel.");

DEFINE_bool(enable_write_thread_adaptive_yield, false,
            "Use a yielding spin loop for brief writer thread waits.");

DEFINE_uint64(
    write_thread_max_yield_usec, 100,
    "Maximum microseconds for enable_write_thread_adaptive_yield operation.");

DEFINE_uint64(write_thread_slow_yield_usec, 3,
              "The threshold at which a slow yield is considered a signal that "
              "other processes or threads want the core.");

DEFINE_int32(rate_limit_delay_max_milliseconds, 1000,
             "When hard_rate_limit is set then this is the max time a put will"
             " be stalled.");

DEFINE_uint64(rate_limiter_bytes_per_sec, 0, "Set options.rate_limiter value.");

DEFINE_uint64(
    benchmark_write_rate_limit, 0,
    "If non-zero, db_bench will rate-limit the writes going into SE. This "
    "is the global rate in bytes/second.");

DEFINE_uint64(
    benchmark_read_rate_limit, 0,
    "If non-zero, db_bench will rate-limit the reads from SE. This "
    "is the global rate in ops/second.");

DEFINE_uint64(max_compaction_bytes, Options().max_compaction_bytes,
              "Max bytes allowed in one compaction");


DEFINE_bool(disable_auto_compactions, false, "Do not auto trigger compactions");

DEFINE_uint64(wal_ttl_seconds, 0, "Set the TTL for the WAL Files in seconds.");
DEFINE_uint64(wal_size_limit_MB, 0,
              "Set the size limit for the WAL Files"
              " in MB.");
DEFINE_uint64(max_total_wal_size, 0, "Set total max WAL size");

DEFINE_bool(mmap_read, Options().allow_mmap_reads,
            "Allow reads to occur via mmap-ing files");

DEFINE_bool(mmap_write, Options().allow_mmap_writes,
            "Allow writes to occur via mmap-ing files");

DEFINE_bool(use_direct_reads, Options().use_direct_reads,
            "Use O_DIRECT for reading data");

DEFINE_bool(use_direct_io_for_flush_and_compaction,
            Options().use_direct_io_for_flush_and_compaction,
            "Use O_DIRECT for background flush and compaction I/O");

DEFINE_bool(advise_random_on_open, Options().advise_random_on_open,
            "Advise random access on table file open");

DEFINE_string(compaction_fadvice, "NORMAL",
              "Access pattern advice when a file is compacted");
static auto FLAGS_compaction_fadvice_e =
    Options().access_hint_on_compaction_start;

DEFINE_bool(use_adaptive_mutex, Options().use_adaptive_mutex,
            "Use adaptive mutex");

DEFINE_uint64(bytes_per_sync, Options().bytes_per_sync,
              "Allows OS to incrementally sync SST files to disk while they are"
              " being written, in the background. Issue one request for every"
              " bytes_per_sync written. 0 turns it off.");

DEFINE_uint64(wal_bytes_per_sync, Options().wal_bytes_per_sync,
              "Allows OS to incrementally sync WAL files to disk while they are"
              " being written, in the background. Issue one request for every"
              " wal_bytes_per_sync written. 0 turns it off.");

DEFINE_bool(use_single_deletes, true,
            "Use single deletes (used in RandomReplaceKeys only).");

DEFINE_double(stddev, 2000.0,
              "Standard deviation of normal distribution used for picking keys"
              " (used in RandomReplaceKeys only).");

DEFINE_int32(key_id_range, 100000,
             "Range of possible value of key id (used in TimeSeries only).");

DEFINE_string(expire_style, "none",
              "Style to remove expired time entries. Can be one of the options "
              "below: none (do not expired data), compaction_filter (use a "
              "compaction filter to remove expired data), delete (seek IDs and "
              "remove expired data) (used in TimeSeries only).");

DEFINE_uint64(
    time_range, 100000,
    "Range of timestamp that store in the database (used in TimeSeries"
    " only).");

DEFINE_int32(num_deletion_threads, 1,
             "Number of threads to do deletion (used in TimeSeries, Stress and "
             "delete expire_style only).");
DEFINE_int32(num_update_threads, 1,
             "Number of threads to do deletion (used in TimeSeries, Stress and "
             "delete expire_style only).");
DEFINE_int32(num_single_delete_threads, 1,
             "Number of threads to do single delete (used in Stress and "
             "delete expire_style only).");
DEFINE_bool(var_len_stress, false, "Use variable-length suffixes (used in Stress only).");
DEFINE_int32(key_id_group_size, 32,
             "Size of group in which all the keys share the same key id "
             "but have variable-length suffixes of zeros (used in Stress only).");

DEFINE_bool(enable_io_prio, false,
            "Lower the background flush/compaction "
            "threads' IO priority");
DEFINE_bool(identity_as_first_hash, false,
            "the first hash function of cuckoo "
            "table becomes an identity function. This is only valid when key "
            "is 8 bytes");

DEFINE_int32(stats_dump_period_sec, 600,
             "Dump status in every stats_dump_period_sec");

DEFINE_bool(dump_malloc_stats, true, "Dump malloc stats in LOG ");

DEFINE_bool(query_trace_print_stats, true, "Dump trace stats in LOG ");

DEFINE_bool(large_kv, false, "Add a few large KV");

DEFINE_bool(medium_kv, false, "Add a few medium KV (50KB 500KB)");

enum RepFactory {
  kSkipList,
  kART,
};

static enum RepFactory StringToRepFactory(const char* ctype) {
  assert(ctype);

  if (!strcasecmp(ctype, "skip_list"))
    return kSkipList;
  else if (!strcasecmp(ctype, "art"))
    return kART;

  fprintf(stdout, "Cannot parse memreptable %s\n", ctype);
  return kSkipList;
}

static enum RepFactory FLAGS_rep_factory;
DEFINE_string(memtablerep, "skip_list", "");
DEFINE_int64(hash_bucket_count, 1024 * 1024, "hash bucket count");
DEFINE_bool(use_plain_table, false,
            "if use plain table "
            "instead of block-based table format");
DEFINE_bool(use_cuckoo_table, false, "if use cuckoo table format");
DEFINE_double(cuckoo_hash_ratio, 0.9, "Hash ratio for Cuckoo SST table.");
DEFINE_bool(use_block_based_table, false, "if use block based table format");
DEFINE_bool(use_extent_based_table, true, "if use extent based table format");
DEFINE_bool(use_hash_search, false,
            "if use kHashSearch "
            "instead of kBinarySearch. "
            "This is valid if only we use BlockTable");
DEFINE_bool(use_block_based_filter, false,
            "if use kBlockBasedFilter "
            "instead of kFullFilter for filter block. "
            "This is valid if only we use BlockTable");
DEFINE_bool(report_file_operations, false,
            "if report number of file "
            "operations");

static const bool FLAGS_soft_rate_limit_dummy __attribute__((unused)) =
    RegisterFlagValidator(&FLAGS_soft_rate_limit, &ValidateRateLimit);

static const bool FLAGS_hard_rate_limit_dummy __attribute__((unused)) =
    RegisterFlagValidator(&FLAGS_hard_rate_limit, &ValidateRateLimit);

static const bool FLAGS_key_size_dummy __attribute__((unused)) =
    RegisterFlagValidator(&FLAGS_key_size, &ValidateKeySize);

static const bool FLAGS_cache_numshardbits_dummy __attribute__((unused)) =
    RegisterFlagValidator(&FLAGS_cache_numshardbits,
                          &ValidateCacheNumshardbits);

static const bool FLAGS_readwritepercent_dummy __attribute__((unused)) =
    RegisterFlagValidator(&FLAGS_readwritepercent, &ValidateInt32Percent);

DEFINE_int32(disable_seek_compaction, false,
             "Not used, left here for backwards compatibility");

static const bool FLAGS_deletepercent_dummy __attribute__((unused)) =
    RegisterFlagValidator(&FLAGS_deletepercent, &ValidateInt32Percent);
static const bool FLAGS_table_cache_numshardbits_dummy __attribute__((unused)) =
    RegisterFlagValidator(&FLAGS_table_cache_numshardbits,
                          &ValidateTableCacheNumshardbits);

namespace smartengine {
namespace tools {

namespace {
struct ReportFileOpCounters {
  std::atomic<int> open_counter_;
  std::atomic<int> read_counter_;
  std::atomic<int> append_counter_;
  std::atomic<uint64_t> bytes_read_;
  std::atomic<uint64_t> bytes_written_;
};

// A special Env to records and report file operations in db_bench
class ReportFileOpEnv : public EnvWrapper {
 public:
  explicit ReportFileOpEnv(Env* base) : EnvWrapper(base) { reset(); }

  void reset() {
    counters_.open_counter_ = 0;
    counters_.read_counter_ = 0;
    counters_.append_counter_ = 0;
    counters_.bytes_read_ = 0;
    counters_.bytes_written_ = 0;
  }

  Status NewSequentialFile(const std::string& f, SequentialFile *&r,
                           const EnvOptions& soptions) override {
    class CountingFile : public SequentialFile {
     private:
      SequentialFile *target_;
      ReportFileOpCounters* counters_;

     public:
      CountingFile(SequentialFile *target,
                   ReportFileOpCounters* counters)
          : target_(std::move(target)), counters_(counters) {}

      virtual Status Read(size_t n, Slice* result, char* scratch) override {
        counters_->read_counter_.fetch_add(1, std::memory_order_relaxed);
        Status rv = target_->Read(n, result, scratch);
        counters_->bytes_read_.fetch_add(result->size(),
                                         std::memory_order_relaxed);
        return rv;
      }

      virtual Status Skip(uint64_t n) override { return target_->Skip(n); }
    };

    Status s = target()->NewSequentialFile(f, r, soptions);
    if (s.ok()) {
      counters()->open_counter_.fetch_add(1, std::memory_order_relaxed);
//      r->reset(new CountingFile(r, counters()));
      r = new CountingFile(r, counters());
    }
    return s;
  }

  Status NewRandomAccessFile(const std::string& f,
                             RandomAccessFile *&r,
                             const EnvOptions& soptions) override {
    class CountingFile : public RandomAccessFile {
     private:
      unique_ptr<RandomAccessFile> target_;
      ReportFileOpCounters* counters_;

     public:
      CountingFile(RandomAccessFile *target,
                   ReportFileOpCounters* counters)
          : target_(target), counters_(counters) {}
      virtual Status Read(uint64_t offset, size_t n, Slice* result,
                          char* scratch) const override {
        counters_->read_counter_.fetch_add(1, std::memory_order_relaxed);
        Status rv = target_->Read(offset, n, result, scratch);
        counters_->bytes_read_.fetch_add(result->size(),
                                         std::memory_order_relaxed);
        return rv;
      }
    };

    Status s = target()->NewRandomAccessFile(f, r, soptions);
    if (s.ok()) {
      counters()->open_counter_.fetch_add(1, std::memory_order_relaxed);
//      r->reset(new CountingFile(r, counters()));
      r = new CountingFile(r, counters());
    }
    return s;
  }

  Status NewWritableFile(const std::string& f, WritableFile *&r,
                         const EnvOptions& soptions) override {
    class CountingFile : public WritableFile {
     private:
      WritableFile *target_;
      ReportFileOpCounters* counters_;

     public:
      CountingFile(WritableFile *target,
                   ReportFileOpCounters* counters)
          : target_(target), counters_(counters) {}

      Status Append(const Slice& data) override {
        counters_->append_counter_.fetch_add(1, std::memory_order_relaxed);
        Status rv = target_->Append(data);
        counters_->bytes_written_.fetch_add(data.size(),
                                            std::memory_order_relaxed);
        return rv;
      }

      Status Truncate(uint64_t size) override {
        return target_->Truncate(size);
      }
      Status Close() override { return target_->Close(); }
      Status Flush() override { return target_->Flush(); }
      Status Sync() override { return target_->Sync(); }
    };

    Status s = target()->NewWritableFile(f, r, soptions);
    if (s.ok()) {
      counters()->open_counter_.fetch_add(1, std::memory_order_relaxed);
//      r->reset(new CountingFile(r, counters()));
      r = new CountingFile(r, counters());
    }
    return s;
  }

  // getter
  ReportFileOpCounters* counters() { return &counters_; }

 private:
  ReportFileOpCounters counters_;
};

}  // namespace

const int MAX_LARGE_VALUE_SIZE = 1048576 * 32;

// Helper for quickly generating random data.
class RandomGenerator {
 private:
  std::string data_;
  unsigned int pos_;

 public:
  RandomGenerator() {
    // We use a limited amount of data over and over again and ensure
    // that it is larger than the compression window (32KB), and also
    // large enough to serve all typical value sizes we want to write.
    Random rnd(301);
    std::string piece;
    while (data_.size() < (unsigned)std::max(MAX_LARGE_VALUE_SIZE, FLAGS_value_size)) {
      // Add a short fragment that is as compressible as specified
      // by FLAGS_compression_ratio.
      test::CompressibleString(&rnd, FLAGS_compression_ratio, 100, &piece);
      data_.append(piece);
    }
    pos_ = 0;
  }

  Slice Generate(unsigned int len) {
    assert(len <= data_.size());
    if (pos_ + len >= data_.size()) {
      pos_ = 0;
    }
    pos_ += len;
    return Slice(data_.data() + pos_ - len, len);
  }
};

static void AppendWithSpace(std::string* str, Slice msg) {
  if (msg.empty()) return;
  if (!str->empty()) {
    str->push_back(' ');
  }
  str->append(msg.data(), msg.size());
}

struct DBWithColumnFamilies {
  std::vector<ColumnFamilyHandle*> cfh;
  DB* db;
#ifndef ROCKSDB_LITE
  OptimisticTransactionDB* opt_txn_db;
#endif                              // ROCKSDB_LITE
  std::atomic<size_t> num_created;  // Need to be updated after all the
                                    // new entries in cfh are set.
  size_t num_hot;  // Number of column families to be queried at each moment.
                   // After each CreateNewCf(), another num_hot number of new
                   // Column families will be created and used to be queried.
  port::Mutex create_cf_mutex;  // Only one thread can execute CreateNewCf()

  DBWithColumnFamilies()
      : db(nullptr)
#ifndef ROCKSDB_LITE
        ,
        opt_txn_db(nullptr)
#endif  // ROCKSDB_LITE
  {
    cfh.clear();
    num_created = 0;
    num_hot = 0;
  }

  DBWithColumnFamilies(const DBWithColumnFamilies& other)
      : cfh(other.cfh),
        db(other.db),
#ifndef ROCKSDB_LITE
        opt_txn_db(other.opt_txn_db),
#endif  // ROCKSDB_LITE
        num_created(other.num_created.load()),
        num_hot(other.num_hot) {
  }

  void DeleteDBs() {
    std::for_each(cfh.begin() + 1, cfh.end(),
                  [](ColumnFamilyHandle* cfhi) { MOD_DELETE_OBJECT(ColumnFamilyHandle, cfhi); });
    cfh.clear();
#ifndef ROCKSDB_LITE
    if (opt_txn_db) {
      delete opt_txn_db;
      opt_txn_db = nullptr;
    } else {
      MOD_DELETE_OBJECT(DB, db);
      db = nullptr;
    }
#else
    delete db;
    db = nullptr;
#endif  // ROCKSDB_LITE
  }

  ColumnFamilyHandle* GetCfh(int64_t rand_num) {
    assert(num_hot > 0);
    return cfh[num_created.load(std::memory_order_acquire) - num_hot +
               rand_num % num_hot];
  }

  ColumnFamilyData *get_cfd(int64_t rand_num) {
    ColumnFamilyHandle *cf_handle = GetCfh(rand_num);
    if (nullptr != cf_handle) {
      ColumnFamilyHandleImpl *cfhi = 
                  reinterpret_cast<ColumnFamilyHandleImpl*>(cf_handle);
      return cfhi->cfd();
    }
    
    return nullptr;
  }

  // stage: assume CF from 0 to stage * num_hot has be created. Need to create
  //        stage * num_hot + 1 to stage * (num_hot + 1).
  void CreateNewCf(ColumnFamilyOptions options, int64_t stage) {
    MutexLock l(&create_cf_mutex);
    if ((stage + 1) * num_hot <= num_created) {
      // Already created.
      return;
    }
    auto new_num_created = num_created + num_hot;
    assert(new_num_created <= cfh.size());
    for (size_t i = num_created; i < new_num_created; i++) {
      //TODO:yuanfeng
      /*
      Status s =
          db->CreateColumnFamily(options, ColumnFamilyName(i), &(cfh[i]));
      if (!s.ok()) {
        fprintf(stderr, "create sub table error: %s\n",
                s.ToString().c_str());
        abort();
      }
      */
    }
    num_created.store(new_num_created, std::memory_order_release);
  }
};

// a class that reports stats to CSV file
class ReporterAgent {
 public:
  ReporterAgent(Env* env, const std::string& fname,
                uint64_t report_interval_secs)
      : env_(env),
        total_ops_done_(0),
        last_report_(0),
        report_interval_secs_(report_interval_secs),
        stop_(false) {
    auto s = env_->NewWritableFile(fname, report_file_, EnvOptions());
    if (s.ok()) {
      s = report_file_->Append(Header() + "\n");
    }
    if (s.ok()) {
      s = report_file_->Flush();
    }
    if (!s.ok()) {
      fprintf(stderr, "Can't open %s: %s\n", fname.c_str(),
              s.ToString().c_str());
      abort();
    }

    reporting_thread_ = port::Thread([&]() { SleepAndReport(); });
  }

  ~ReporterAgent() {
    {
      std::unique_lock<std::mutex> lk(mutex_);
      stop_ = true;
      stop_cv_.notify_all();
    }
    reporting_thread_.join();
  }

  // thread safe
  void ReportFinishedOps(int64_t num_ops) {
    total_ops_done_.fetch_add(num_ops);
  }

 private:
  std::string Header() const { return "secs_elapsed,interval_qps"; }
  void SleepAndReport() {
    uint64_t kMicrosInSecond = 1000 * 1000;
    auto time_started = env_->NowMicros();
    while (true) {
      {
        std::unique_lock<std::mutex> lk(mutex_);
        if (stop_ ||
            stop_cv_.wait_for(lk, std::chrono::seconds(report_interval_secs_),
                              [&]() { return stop_; })) {
          // stopping
          break;
        }
        // else -> timeout, which means time for a report!
      }
      auto total_ops_done_snapshot = total_ops_done_.load();
      // round the seconds elapsed
      auto secs_elapsed =
          (env_->NowMicros() - time_started + kMicrosInSecond / 2) /
          kMicrosInSecond;
      std::string report = ToString(secs_elapsed) + "," +
                           ToString(total_ops_done_snapshot - last_report_) +
                           "\n";
      auto s = report_file_->Append(report);
      if (s.ok()) {
        s = report_file_->Flush();
      }
      if (!s.ok()) {
        fprintf(stderr,
                "Can't write to report file (%s), stopping the reporting\n",
                s.ToString().c_str());
        break;
      }
      last_report_ = total_ops_done_snapshot;
    }
  }

  Env* env_;
//  std::unique_ptr<WritableFile> report_file_;
  WritableFile *report_file_;
  std::atomic<int64_t> total_ops_done_;
  int64_t last_report_;
  const uint64_t report_interval_secs_;
  port::Thread reporting_thread_;
  std::mutex mutex_;
  // will notify on stop
  std::condition_variable stop_cv_;
  bool stop_;
};

enum OperationType : unsigned char {
  kRead = 0,
  kWrite,
  kDelete,
  kSeek,
  kMerge,
  kUpdate,
  kCompress,
  kUncompress,
  kCrc,
  kHash,
  kOthers
};

static std::unordered_map<OperationType, std::string, std::hash<unsigned char>>
    OperationTypeString = {{kRead, "read"},         {kWrite, "write"},
                           {kDelete, "delete"},     {kSeek, "seek"},
                           {kMerge, "merge"},       {kUpdate, "update"},
                           {kCompress, "compress"}, {kCompress, "uncompress"},
                           {kCrc, "crc"},           {kHash, "hash"},
                           {kOthers, "op"}};

class CombinedStats;
class Stats {
 private:
  int id_;
  uint64_t start_;
  uint64_t finish_;
  double seconds_;
  uint64_t done_;
  uint64_t last_report_done_;
  uint64_t next_report_;
  uint64_t bytes_;
  uint64_t last_op_finish_;
  uint64_t last_report_finish_;
  std::unordered_map<OperationType, std::shared_ptr<HistogramImpl>,
                     std::hash<unsigned char>>
      hist_;
  std::string message_;
  bool exclude_from_merge_;
  ReporterAgent* reporter_agent_;  // does not own
  std::chrono::nanoseconds duration_; // for accurate timing
  friend class CombinedStats;

 public:
  Stats() { Start(-1); }

  void SetReporterAgent(ReporterAgent* reporter_agent) {
    reporter_agent_ = reporter_agent;
  }

  void Start(int id) {
    id_ = id;
    next_report_ = FLAGS_stats_interval ? FLAGS_stats_interval : 100;
    last_op_finish_ = start_;
    hist_.clear();
    done_ = 0;
    last_report_done_ = 0;
    bytes_ = 0;
    seconds_ = 0;
    start_ = FLAGS_env->NowMicros();
    finish_ = start_;
    last_report_finish_ = start_;
    message_.clear();
    // When set, stats from this thread won't be merged with others.
    exclude_from_merge_ = false;
    duration_ = std::chrono::nanoseconds::zero();
  }

  void Merge(const Stats& other) {
    if (other.exclude_from_merge_) return;

    for (auto it = other.hist_.begin(); it != other.hist_.end(); ++it) {
      auto this_it = hist_.find(it->first);
      if (this_it != hist_.end()) {
        this_it->second->Merge(*(other.hist_.at(it->first)));
      } else {
        hist_.insert({it->first, it->second});
      }
    }

    done_ += other.done_;
    bytes_ += other.bytes_;
    seconds_ += other.seconds_;
    if (other.start_ < start_) start_ = other.start_;
    if (other.finish_ > finish_) finish_ = other.finish_;

    // Just keep the messages from one thread
    if (message_.empty()) message_ = other.message_;
    if (other.duration_ != std::chrono::nanoseconds::zero()) {
      duration_ += other.duration_;
    }
  }

  void Stop() {
    finish_ = FLAGS_env->NowMicros();
    seconds_ = (finish_ - start_) * 1e-6;
  }

  void AddMessage(Slice msg) { AppendWithSpace(&message_, msg); }

  void SetId(int id) { id_ = id; }
  void SetExcludeFromMerge() { exclude_from_merge_ = true; }

  void PrintThreadStatus() {
    std::vector<ThreadStatus> thread_list;
    FLAGS_env->GetThreadList(&thread_list);

    fprintf(stderr, "\n%18s %10s %12s %20s %13s %45s %12s %s\n", "ThreadID",
            "ThreadType", "cfName", "Operation", "ElapsedTime", "Stage",
            "State", "OperationProperties");

    int64_t current_time = 0;
    Env::Default()->GetCurrentTime(&current_time);
    for (auto ts : thread_list) {
      fprintf(stderr, "%18" PRIu64 " %10s %12s %20s %13s %45s %12s",
              ts.thread_id,
              ThreadStatus::GetThreadTypeName(ts.thread_type).c_str(),
              ts.cf_name.c_str(),
              ThreadStatus::GetOperationName(ts.operation_type).c_str(),
              ThreadStatus::MicrosToString(ts.op_elapsed_micros).c_str(),
              ThreadStatus::GetOperationStageName(ts.operation_stage).c_str(),
              ThreadStatus::GetStateName(ts.state_type).c_str());

      auto op_properties = ThreadStatus::InterpretOperationProperties(
          ts.operation_type, ts.op_properties);
      for (const auto& op_prop : op_properties) {
        fprintf(stderr, " %s %" PRIu64 " |", op_prop.first.c_str(),
                op_prop.second);
      }
      fprintf(stderr, "\n");
    }
  }

  void ResetLastOpTime() {
    // Set to now to avoid latency from calls to SleepForMicroseconds
    last_op_finish_ = FLAGS_env->NowMicros();
  }

  void finished_ops(int64_t num_ops, std::chrono::nanoseconds &dur) {
    done_ += num_ops,
    duration_ += dur;
  }

  // ugly adjust the search level op to one search op
  void adjust_ops(int32_t l) {
    done_ = (done_ / l);
  }

  void FinishedOps(DBWithColumnFamilies* db_with_cfh, DB* db, int64_t num_ops,
                   enum OperationType op_type = kOthers) {
    if (reporter_agent_) {
      reporter_agent_->ReportFinishedOps(num_ops);
    }
    if (FLAGS_histogram) {
      uint64_t now = FLAGS_env->NowMicros();
      uint64_t micros = now - last_op_finish_;

      if (hist_.find(op_type) == hist_.end()) {
        auto hist_temp = std::make_shared<HistogramImpl>();
        hist_.insert({op_type, std::move(hist_temp)});
      }
      hist_[op_type]->Add(micros);

      if (micros > 20000 && !FLAGS_stats_interval) {
        fprintf(stderr, "long op: %" PRIu64 " micros%30s\r", micros, "");
        fflush(stderr);
      }
      last_op_finish_ = now;
    }

    done_ += num_ops;
    if (done_ >= next_report_) {
      if (!FLAGS_stats_interval) {
        if (next_report_ < 1000)
          next_report_ += 100;
        else if (next_report_ < 5000)
          next_report_ += 500;
        else if (next_report_ < 10000)
          next_report_ += 1000;
        else if (next_report_ < 50000)
          next_report_ += 5000;
        else if (next_report_ < 100000)
          next_report_ += 10000;
        else if (next_report_ < 500000)
          next_report_ += 50000;
        else
          next_report_ += 100000;
        fprintf(stderr, "... finished %" PRIu64 " ops%30s\r", done_, "");
      } else {
        uint64_t now = FLAGS_env->NowMicros();
        int64_t usecs_since_last = now - last_report_finish_;

        // Determine whether to print status where interval is either
        // each N operations or each N seconds.

        if (FLAGS_stats_interval_seconds &&
            usecs_since_last < (FLAGS_stats_interval_seconds * 1000000)) {
          // Don't check again for this many operations
          next_report_ += FLAGS_stats_interval;

        } else {
#if 1
          fprintf(stderr, "%s ... thread %d: (%" PRIu64 ",%" PRIu64
                          ") ops and "
                          "(%.1f,%.1f) ops/second in (%.6f,%.6f) seconds\n",
                  FLAGS_env->TimeToString(now / 1000000).c_str(), id_,
                  done_ - last_report_done_, done_,
                  (done_ - last_report_done_) / (usecs_since_last / 1000000.0),
                  done_ / ((now - start_) / 1000000.0),
                  (now - last_report_finish_) / 1000000.0,
                  (now - start_) / 1000000.0);
#endif

          if (id_ == 0 && FLAGS_stats_per_interval) {
            std::string stats;

            if (db_with_cfh && db_with_cfh->num_created.load()) {
              for (size_t i = 0; i < db_with_cfh->num_created.load(); ++i) {
                if (db->GetProperty(db_with_cfh->cfh[i], "smartengine.cfstats",
                                    &stats))
                  fprintf(stderr, "%s\n", stats.c_str());
                if (FLAGS_show_table_properties) {
                  for (int level = 0; level < FLAGS_num_levels; ++level) {
                    if (db->GetProperty(
                            db_with_cfh->cfh[i],
                            "smartengine.aggregated-table-properties-at-level" +
                                ToString(level),
                            &stats)) {
                      if (stats.find("# entries=0") == std::string::npos) {
                        fprintf(stderr, "Level[%d]: %s\n", level,
                                stats.c_str());
                      }
                    }
                  }
                }
              }
            } else if (db) {
              if (db->GetProperty("smartengine.stats", &stats)) {
                fprintf(stderr, "%s\n", stats.c_str());
              }
              if (FLAGS_show_table_properties) {
                for (int level = 0; level < FLAGS_num_levels; ++level) {
                  if (db->GetProperty(
                          "smartengine.aggregated-table-properties-at-level" +
                              ToString(level),
                          &stats)) {
                    if (stats.find("# entries=0") == std::string::npos) {
                      fprintf(stderr, "Level[%d]: %s\n", level, stats.c_str());
                    }
                  }
                }
              }
            }
          }

          next_report_ += FLAGS_stats_interval;
          last_report_finish_ = now;
          last_report_done_ = done_;
        }
      }
      if (id_ == 0 && FLAGS_thread_status_per_interval) {
        PrintThreadStatus();
      }
      fflush(stderr);
    }
  }

  void AddBytes(int64_t n) { bytes_ += n; }

  void Report(const Slice& name) {
    // Pretend at least one op was done in case we are running a benchmark
    // that does not call FinishedOps().
    if (done_ < 1) done_ = 1;

    std::string extra;
    if (bytes_ > 0) {
      // Rate is computed on actual elapsed time, not the sum of per-thread
      // elapsed times.
      double elapsed = (finish_ - start_) * 1e-6;
      char rate[100];
      snprintf(rate, sizeof(rate), "%6.1f MB/s",
               (bytes_ / 1048576.0) / elapsed);
      extra = rate;
    }
    AppendWithSpace(&extra, message_);
    if (duration_ == std::chrono::nanoseconds::zero()) {
      double elapsed = (finish_ - start_) * 1e-6;
      double throughput = (double)done_ / elapsed;
      fprintf(stdout, "%-12s : %11.3f micros/op %ld ops/sec;%s%s\n",
              name.ToString().c_str(), elapsed * 1e6 / done_, (long)throughput,
              (extra.empty() ? "" : " "), extra.c_str());
    } else {
      double throughput = (double)done_ / (duration_.count() * 1e-9); 
      fprintf(stdout, "%-12s : %ld nanos/op %ld ops/sec;%s%s\n",
              name.ToString().c_str(), duration_.count() / done_, (long)throughput,
              (extra.empty() ? "" : " "), extra.c_str()); 
    }
    if (FLAGS_histogram) {
      for (auto it = hist_.begin(); it != hist_.end(); ++it) {
        fprintf(stdout, "Microseconds per %s:\n%s\n",
                OperationTypeString[it->first].c_str(),
                it->second->ToString().c_str());
      }
    }
    if (FLAGS_report_file_operations) {
      ReportFileOpEnv* env = static_cast<ReportFileOpEnv*>(FLAGS_env);
      ReportFileOpCounters* counters = env->counters();
      fprintf(stdout, "Num files opened: %d\n",
              counters->open_counter_.load(std::memory_order_relaxed));
      fprintf(stdout, "Num Read(): %d\n",
              counters->read_counter_.load(std::memory_order_relaxed));
      fprintf(stdout, "Num Append(): %d\n",
              counters->append_counter_.load(std::memory_order_relaxed));
      fprintf(stdout, "Num bytes read: %" PRIu64 "\n",
              counters->bytes_read_.load(std::memory_order_relaxed));
      fprintf(stdout, "Num bytes written: %" PRIu64 "\n",
              counters->bytes_written_.load(std::memory_order_relaxed));
      env->reset();
    }
    fflush(stdout);
  }
};

class CombinedStats {
 public:
  void AddStats(const Stats& stat) {
    uint64_t total_ops = stat.done_;
    uint64_t total_bytes_ = stat.bytes_;
    double elapsed;

    if (total_ops < 1) {
      total_ops = 1;
    }

    elapsed = (stat.finish_ - stat.start_) * 1e-6;
    throughput_ops_.emplace_back(total_ops / elapsed);

    if (total_bytes_ > 0) {
      double mbs = (total_bytes_ / 1048576.0);
      throughput_mbs_.emplace_back(mbs / elapsed);
    }
  }

  void Report(const std::string& bench_name) {
    const char* name = bench_name.c_str();
    int num_runs = static_cast<int>(throughput_ops_.size());

    if (throughput_mbs_.size() == throughput_ops_.size()) {
      fprintf(stdout,
              "%s [AVG    %d runs] : %d ops/sec; %6.1f MB/sec\n"
              "%s [MEDIAN %d runs] : %d ops/sec; %6.1f MB/sec\n",
              name, num_runs, static_cast<int>(CalcAvg(throughput_ops_)),
              CalcAvg(throughput_mbs_), name, num_runs,
              static_cast<int>(CalcMedian(throughput_ops_)),
              CalcMedian(throughput_mbs_));
    } else {
      fprintf(stdout,
              "%s [AVG    %d runs] : %d ops/sec\n"
              "%s [MEDIAN %d runs] : %d ops/sec\n",
              name, num_runs, static_cast<int>(CalcAvg(throughput_ops_)), name,
              num_runs, static_cast<int>(CalcMedian(throughput_ops_)));
    }
  }

 private:
  double CalcAvg(std::vector<double> data) {
    double avg = 0;
    for (double x : data) {
      avg += x;
    }
    avg = avg / data.size();
    return avg;
  }

  double CalcMedian(std::vector<double> data) {
    assert(data.size() > 0);
    std::sort(data.begin(), data.end());

    size_t mid = data.size() / 2;
    if (data.size() % 2 == 1) {
      // Odd number of entries
      return data[mid];
    } else {
      // Even number of entries
      return (data[mid] + data[mid - 1]) / 2;
    }
  }

  std::vector<double> throughput_ops_;
  std::vector<double> throughput_mbs_;
};

class TimestampEmulator {
 private:
  std::atomic<uint64_t> timestamp_;

 public:
  TimestampEmulator() : timestamp_(0) {}
  uint64_t Get() const { return timestamp_.load(); }
  void Inc() { timestamp_++; }
};

// State shared by all concurrent executions of the same benchmark.
struct SharedState {
  port::Mutex mu;
  port::CondVar cv;
  int total;
  int perf_level;
  std::shared_ptr<RateLimiter> write_rate_limiter;
  std::shared_ptr<RateLimiter> read_rate_limiter;

  // Each thread goes through the following states:
  //    (1) initializing
  //    (2) waiting for others to be initialized
  //    (3) running
  //    (4) done

  long num_initialized;
  long num_done;
  bool start;

  SharedState() : cv(&mu), perf_level(FLAGS_perf_level) {}
};

// Per-thread state for concurrent executions of the same benchmark.
struct ThreadState {
  int tid;        // 0..n-1 when running in n threads
  Random64 rand;  // Has different seeds for different threads
  Stats stats;
  SharedState* shared;

  /* implicit */ ThreadState(int index)
      : tid(index), rand((FLAGS_seed ? FLAGS_seed : 1000) + index) {}
};

class Duration {
 public:
  Duration(uint64_t max_seconds, int64_t max_ops, int64_t ops_per_stage = 0) {
    max_seconds_ = max_seconds;
    max_ops_ = max_ops;
    ops_per_stage_ = (ops_per_stage > 0) ? ops_per_stage : max_ops;
    ops_ = 0;
    start_at_ = FLAGS_env->NowMicros();
  }

  int64_t GetStage() { return std::min(ops_, max_ops_ - 1) / ops_per_stage_; }

  bool Done(int64_t increment) {
    if (increment <= 0) increment = 1;  // avoid Done(0) and infinite loops
    ops_ += increment;

    if (max_seconds_) {
      // Recheck every appx 1000 ops (exact iff increment is factor of 1000)
      if ((ops_ / 1000) != ((ops_ - increment) / 1000)) {
        uint64_t now = FLAGS_env->NowMicros();
        return ((now - start_at_) / 1000000) >= max_seconds_;
      } else {
        return false;
      }
    } else {
      return ops_ > max_ops_;
    }
  }

 private:
  uint64_t max_seconds_;
  int64_t max_ops_;
  int64_t ops_per_stage_;
  int64_t ops_;
  uint64_t start_at_;
};

class BenchMarkAsyncCallback : public AsyncCallback {
 public:
  explicit BenchMarkAsyncCallback(WriteBatch* w_batch)
      : AsyncCallback(true), write_batch_(w_batch) {}
  ~BenchMarkAsyncCallback() {
    if (nullptr != write_batch_) {
      write_batch_->Clear();
      delete write_batch_;
    }
  }
  Status call_back() override {
    Status status;
    return status;
  }
 private:
  WriteBatch* write_batch_;
};

struct StressBitMapSnapshot {
  size_t offset_ = 0;
  size_t size_ = 0;
  unsigned char *round_;
  unsigned char *exist_;
};

void init_bitmap_snapshot(size_t offset, size_t size,
                          StressBitMapSnapshot &snapshot) {
  snapshot.offset_ = offset;
  snapshot.size_ = size;
  snapshot.round_ = new unsigned char[size];
  snapshot.exist_ = new unsigned char[size / BITS_PER_BYTE + 1];
}

class StressBitMap {
 public:
  static const size_t MAX_ROUND_NUMBER_BITS = 8;
  static const size_t BITS_PER_BYTE = 8;
  static const unsigned char ALL_ONE_BYTE = 0xFF;

 public:
  StressBitMap() {}
  StressBitMap(const StressBitMap&) = delete;
  StressBitMap(StressBitMap&&) = delete;
  StressBitMap& operator=(const StressBitMap&) = delete;
  StressBitMap& operator=(StressBitMap&&) = delete;
  virtual ~StressBitMap() {}

  size_t get_size() const { return size_; }

  virtual Status get_snapshot(StressBitMapSnapshot &snapshot) const {
    return Status::NotSupported("Can not get a snapshot from this bitmap");
  }
  virtual Status cover(StressBitMapSnapshot &snapshot) = 0;
  virtual Status init(size_t size, Env* env, bool force_refresh,
                      int32_t cf_id) = 0;
  virtual Status get(uint64_t index, uint32_t& result) const = 0;
  virtual Status is_deleted(uint64_t index, bool& delete_flag) const = 0;
  virtual Status set(uint64_t index, uint32_t round_num) = 0;
  virtual Status delete_bit(uint64_t index) = 0;
  virtual void print_to_log(FILE *file) const {}

 protected:
  size_t offset_ = 0;
  size_t size_ = 0;
};

class StressBitMapInFile : public StressBitMap {
 public:
  StressBitMapInFile() {}
  StressBitMapInFile(const StressBitMapInFile&) = delete;
  StressBitMapInFile(StressBitMapInFile&&) = delete;
  StressBitMapInFile& operator=(const StressBitMapInFile&) = delete;
  StressBitMapInFile& operator=(StressBitMapInFile&&) = delete;

  // Firstly extend files to a size large enough and write initialized data.
  // Then renopen them in random rw mode.
  Status init(size_t size, Env* env, bool force_refresh,
              int32_t cf_id) override {
    Status s;
    char cf_id_rep[4];
    snprintf(cf_id_rep, 4, "%03d", cf_id);
    std::string round_file_path = FLAGS_db + "/round.bitmap." + cf_id_rep;
    std::string exist_file_path = FLAGS_db + "/exist.bitmap." + cf_id_rep;
    bool file_can_use = env->FileExists(round_file_path).ok() &&
                        env->FileExists(exist_file_path).ok();
    if (file_can_use) {
      size_t round_file_size, exist_file_size;
      s = env->GetFileSize(round_file_path, &round_file_size);
      if (!s.ok()) return s;
      s = env->GetFileSize(exist_file_path, &exist_file_size);
      if (!s.ok()) return s;
      file_can_use &=
          (round_file_size >= size && exist_file_size * BITS_PER_BYTE >= size);
    }
    if (!file_can_use || force_refresh) {
//      std::unique_ptr<WritableFile> round_file;
//      std::unique_ptr<WritableFile> exist_file;
      WritableFile *round_file = nullptr;
      WritableFile *exist_file = nullptr;
      s = env->NewWritableFile(round_file_path, round_file, EnvOptions());
      if (!s.ok()) return s;
      s = env->NewWritableFile(exist_file_path, exist_file, EnvOptions());
      if (!s.ok()) return s;
      round_file->PrepareWrite(0, size);
      exist_file->PrepareWrite(0, size / BITS_PER_BYTE);
      static size_t BLOCK_SIZE = 4096;
      std::string all_zero_str(BLOCK_SIZE, '\0');
      Slice all_zero_block{all_zero_str};
      std::string all_one_str(BLOCK_SIZE, ALL_ONE_BYTE);
      Slice all_one_block{all_one_str};
      // Reserve one more block.
      for (size_t i = 0; i != size / BLOCK_SIZE + 1; ++i) {
        s = round_file->Append(all_zero_block);
        if (!s.ok()) return s;
      }
      for (size_t i = 0; i != size / BLOCK_SIZE / BITS_PER_BYTE + 1; ++i) {
        s = exist_file->Append(all_zero_block);
        if (!s.ok()) return s;
      }
      s = round_file->Fsync();
      if (!s.ok()) return s;
      s = exist_file->Fsync();
      if (!s.ok()) return s;
      s = round_file->Close();
      if (!s.ok()) return s;
      s = exist_file->Close();
      if (!s.ok()) return s;
    }

    s = env->NewRandomRWFile(round_file_path, round_, EnvOptions());
    if (!s.ok()) return s;
    s = env->NewRandomRWFile(exist_file_path, exist_, EnvOptions());
    if (!s.ok()) return s;
    size_ = size;
    return Status::OK();
  }

  Status get(uint64_t index, uint32_t& result) const override {
    unsigned char result_char = 0;
    Slice data;
    Status s = round_->Read(index - offset_, 1, &data,
                            reinterpret_cast<char*>(&result_char));
    if (!s.ok()) {
      fprintf(stderr, "FAIL to read bitmap %s\n", s.ToString().c_str());
      return s;
    }
    result = static_cast<uint32_t>(result_char);
    return s;
  }

  Status get_snapshot(StressBitMapSnapshot &snapshot) const override {
    Slice round_data;
    Status s = round_->Read(snapshot.offset_, snapshot.size_, &round_data,
                            reinterpret_cast<char*>(snapshot.round_));
    if (!s.ok()) {
      fprintf(stderr, "FAIL to read round_ bitmap %s\n", s.ToString().c_str());
    } else {
      assert(snapshot.offset_ % BITS_PER_BYTE == 0);
      assert(snapshot.size_ % BITS_PER_BYTE == 0);
      Slice exist_data;
      s = exist_->Read(snapshot.offset_ / BITS_PER_BYTE,
                       snapshot.size_ / BITS_PER_BYTE,
                       &exist_data, reinterpret_cast<char*>(snapshot.exist_));
      if (!s.ok()) {
        fprintf(stderr, "FAIL to read exist_ bitmap %s\n", s.ToString().c_str());
      }
    }
    return s;
  }

  Status cover(StressBitMapSnapshot &snapshot) override {
    assert(snapshot.offset_ % BITS_PER_BYTE == 0);
    assert(snapshot.size_ % BITS_PER_BYTE == 0);
    Slice round_data(reinterpret_cast<char*>(snapshot.round_), snapshot.size_);
    Slice exist_data(reinterpret_cast<char*>(snapshot.exist_), snapshot.size_ / BITS_PER_BYTE);
    Status s = round_->Write(snapshot.offset_, round_data);
    if (!s.ok()) {
      fprintf(stderr, "FAIL to write round_ bitmap %s\n", s.ToString().c_str());
    } else {
      s = exist_->Write(snapshot.offset_ / BITS_PER_BYTE, exist_data);
      if (!s.ok()) {
        fprintf(stderr, "FAIL to write exist_ bitmap %s\n", s.ToString().c_str());
      }
    }
    return s;
  }

  Status is_deleted(uint64_t index, bool& delete_flag) const override {
    index -= offset_;
    unsigned char byte;
    Slice data;
    Status s = exist_->Read(index / BITS_PER_BYTE, 1, &data,
                            reinterpret_cast<char*>(&byte));
    if (!s.ok()) return s;
    delete_flag = (byte & (static_cast<unsigned char>(0x80) >>
                           (index % BITS_PER_BYTE))) == 0;
    return Status::OK();
  }

  Status set(uint64_t index, uint32_t round_num) override {
    index -= offset_;
    unsigned char round_byte = static_cast<unsigned char>(round_num);
    Slice round_slice(reinterpret_cast<char*>(&round_byte), 1);
    Status s = round_->Write(index, round_slice);
    if (!s.ok()) return s;

    unsigned char exist_byte;
    Slice exist_data;
    s = exist_->Read(index / BITS_PER_BYTE, 1, &exist_data,
                     reinterpret_cast<char*>(&exist_byte));
    if (!s.ok()) return s;
    exist_byte |= (static_cast<unsigned char>(0x80) >> (index % BITS_PER_BYTE));
    s = exist_->Write(index / BITS_PER_BYTE, exist_data);
    return s;
  }

  Status delete_bit(uint64_t index) override {
    index -= offset_;
    unsigned char exist_byte;
    Slice exist_data;
    Status s = exist_->Read(index / BITS_PER_BYTE, 1, &exist_data,
                            reinterpret_cast<char*>(&exist_byte));
    if (!s.ok()) return s;
    exist_byte &=
        ~(static_cast<unsigned char>(0x80) >> (index % BITS_PER_BYTE));
    s = exist_->Write(index / BITS_PER_BYTE, exist_data);
    return s;
  }

 private:
  // ~RandomRWFile will close them.
//  std::unique_ptr<RandomRWFile> round_;
//  std::unique_ptr<RandomRWFile> exist_;
  RandomRWFile *round_;
  RandomRWFile *exist_;
};

class StressMemoryBitMap : public StressBitMap {
 public:
  StressMemoryBitMap() {}
  StressMemoryBitMap(const StressMemoryBitMap&) = delete;
  StressMemoryBitMap(StressMemoryBitMap&&) = delete;
  StressMemoryBitMap& operator=(const StressMemoryBitMap&) = delete;
  StressMemoryBitMap& operator=(StressMemoryBitMap&&) = delete;

  Status init(size_t size, Env* env, bool force_refresh,
              int32_t cf_id) override {
    if (size == 0) {
      round_.reset();
      exist_.reset();
    } else if (size != size_) {
      unsigned char* round_data = new unsigned char[size];
      unsigned char* exist_data = new unsigned char[size / BITS_PER_BYTE + 1];
      if (round_data == nullptr || exist_data == nullptr) {
        return Status::MemoryLimit("bitmap allocation failed!");
      }
      memset(round_data, 0, size);
      memset(exist_data, 0, size / BITS_PER_BYTE + 1);
      round_.reset(round_data);
      exist_.reset(exist_data);
    } else {
      // if size is same, so avoid allocating and memset directly.
      memset(round_.get(), 0, size);
      memset(exist_.get(), 0, size / BITS_PER_BYTE + 1);
    }
    size_ = size;
    return Status::OK();
  }

  Status init_from_snapshot(StressBitMapSnapshot &snapshot) {
    size_ = snapshot.size_;
    offset_ = snapshot.offset_;
    round_.reset(snapshot.round_);
    exist_.reset(snapshot.exist_);
    return  Status::OK();
  }

  Status get_snapshot(StressBitMapSnapshot &snapshot) const override {
    size_t size = snapshot.size_;
    size_t offset = snapshot.offset_;
    memcpy(snapshot.round_, round_.get() + offset, size);
    memcpy(snapshot.exist_, exist_.get() + offset / BITS_PER_BYTE,
           size / BITS_PER_BYTE);
    return Status::OK();
  }

  Status cover(StressBitMapSnapshot &snapshot) override {
    size_t size = snapshot.size_;
    size_t offset = snapshot.offset_;
    memcpy(round_.get() + offset, snapshot.round_, size);
    memcpy(exist_.get() + offset / BITS_PER_BYTE, snapshot.exist_,
           size / BITS_PER_BYTE);
    return Status::OK();
  }

  Status get(uint64_t index, uint32_t& result) const override {
    result = static_cast<uint32_t>(round_[index - offset_]);
    return Status::OK();
  }

  Status is_deleted(uint64_t index, bool& delete_flag) const override {
    index -= offset_;
    delete_flag =
        (exist_[index / BITS_PER_BYTE] &
         (static_cast<unsigned char>(0x80) >> (index % BITS_PER_BYTE))) == 0;
    return Status::OK();
  }

  Status set(uint64_t index, uint32_t round_num) override {
    index -= offset_;
    round_[index] = static_cast<unsigned char>(round_num);
    exist_[index / BITS_PER_BYTE] |=
        (static_cast<unsigned char>(0x80) >> (index % BITS_PER_BYTE));
    return Status::OK();
  }

  Status delete_bit(uint64_t index) override {
    index -= offset_;
    exist_[index / BITS_PER_BYTE] &=
        ~(static_cast<unsigned char>(0x80) >> (index % BITS_PER_BYTE));
    return Status::OK();
  }

  void print_to_log(FILE *file) const override {
    fprintf(file, "--------- StressMemoryBitMap dump to log ---------\n");
    fprintf(file, "round map:\n");
    for (size_t i = 0; i < size_; ++i) {
      fprintf(file, "%02x ", round_[i]);
      if (i % 60 == 59) {
        fprintf(file, "\n");
      }
    }
    fprintf(file, "\n");
    fprintf(file, "exist map:\n");
    for (size_t i = 0; i < size_ / BITS_PER_BYTE; ++i) {
      fprintf(file, "%02x ", exist_[i]);
      if (i % 60 == 59) {
        fprintf(file, "\n");
      }
    }
    fprintf(file, "\n");
  }

 private:
  std::unique_ptr<unsigned char[]> round_;
  std::unique_ptr<unsigned char[]> exist_;
};

class StressLockMgr {
 public:
  static size_t STRIPES;

 public:
  StressLockMgr(size_t total_num)
      : locks_(STRIPES),
        stripe_locks_(STRIPES),
        stripe_block_(STRIPES),
        stripe_reader_count_(STRIPES),
        stripe_size_(total_num / STRIPES) {
    for (size_t i = 0; i != STRIPES; ++i) {
      stripe_block_[i].store(false);
      stripe_reader_count_[i].store(0);
    }
  }

  size_t get_stripe(uint64_t key_id) {
    return static_cast<size_t>(key_id / stripe_size_);
  }

  void lock_s(uint64_t key) { locks_[get_stripe(key)].ReadLock(); }

  void lock_x(uint64_t key) { locks_[get_stripe(key)].WriteLock(); }

  void unlock_s(uint64_t key) { locks_[get_stripe(key)].ReadUnlock(); }

  void unlock_x(uint64_t key) { locks_[get_stripe(key)].WriteUnlock(); }

  bool lock_stripe_s(size_t stripe) {
    if (stripe_block_[stripe].load()) {
      return false;
    }
    stripe_reader_count_[stripe].fetch_add(1);
    if (stripe_block_[stripe].load()) {
      stripe_reader_count_[stripe].fetch_sub(1);
      return false;
    }
    return true;
  }

  void lock_stripe_x(size_t stripe) {
    stripe_block_[stripe].store(true);
    while (stripe_reader_count_[stripe].load() > 0) {
      port::AsmVolatilePause();
    }
  }

  void unlock_stripe_s(size_t stripe) {
    stripe_reader_count_[stripe].fetch_sub(1);
  }

  void unlock_stripe_x(size_t stripe) { stripe_block_[stripe].store(false); }

 private:
  std::vector<port::RWMutex> locks_;
  std::vector<port::RWMutex> stripe_locks_;

  std::vector<std::atomic<bool>> stripe_block_;
  std::vector<std::atomic<int32_t>> stripe_reader_count_;
  size_t stripe_size_;
};

size_t StressLockMgr::STRIPES;

struct StressHistory {
 public:
  void save_snapshot(DB* db, size_t offset, size_t size, StressBitMap* bitmap) {
    snapshot_ = db->GetSnapshot();
    StressBitMapSnapshot bitmap_snapshot;
    init_bitmap_snapshot(offset, size, bitmap_snapshot);
    Status s = bitmap->get_snapshot(bitmap_snapshot);
    if (!s.ok()) {
      db->ReleaseSnapshot(snapshot_);
      snapshot_ = nullptr;
    } else {
      auto bitmap_to_save = new StressMemoryBitMap();
      bitmap_to_save->init_from_snapshot(bitmap_snapshot);
      bitmap_snapshot_.reset(bitmap_to_save);
      first_key_id_ = offset;
    }
  }
  const Snapshot* snapshot_ = nullptr;
  std::unique_ptr<StressBitMap> bitmap_snapshot_;
  uint64_t first_key_id_;
};

typedef uint64_t StressKeyID;

struct StressErrorKey {
 public:
  int32_t cf_id_;
  StressKeyID key_id_;
};

class Benchmark {
 private:
  std::shared_ptr<Cache> cache_;
  std::shared_ptr<Cache> compressed_cache_;
  std::shared_ptr<const FilterPolicy> filter_policy_;
  DBWithColumnFamilies db_;
  std::vector<DBWithColumnFamilies> multi_dbs_;
  int64_t num_;
  int value_size_;
  int key_size_;
  int64_t entries_per_batch_;
  int64_t range_tombstone_width_;
  int64_t max_num_range_tombstones_;
  WriteOptions write_options_;
  Options open_options_;  // keep options around to properly destroy db later
  int64_t reads_;
  int64_t deletes_;
  double read_random_exp_range_;
  int64_t writes_;
  int64_t readwrites_;
  int64_t merge_keys_;
  bool report_file_operations_;

  // Used for stress test.
  size_t STRESS_VALUE_SIZE;
  size_t STRESS_DATA_SIZE;
  FILE *bench_log;
  std::unordered_map<int32_t, ColumnFamilyHandle*> stress_cf_map_;
  std::vector<std::unique_ptr<StressBitMap>> stress_bitmap_;
  std::vector<std::unique_ptr<StressLockMgr>> stress_lock_mgr_;
  uint64_t stress_start_time;
  std::uniform_int_distribution<size_t> stress_stripe_dist_;
  static thread_local int32_t stress_cf_id_;
  static const size_t STRESS_MAX_STAGE = 10;
  static const size_t STRESS_KEY_SIZE = sizeof(uint64_t) + 1 /* suffix */;
  static const size_t STRESS_FULL_SIZE = STRESS_KEY_SIZE + 1500;
  static size_t stress_stripes_per_read_thread_;
  std::string THREAD_LAST_KEY_PREFIX = std::string(STRESS_KEY_SIZE, 0xff);
  int32_t stress_worker_count_[STRESS_MAX_STAGE];
  port::Mutex stress_worker_mutex_;
  port::CondVar stress_worker_cond_;
  bool is_recover_mode;
  Random64 rand_large_val;

  const uint32_t VALUE_SIZE_SIZE = 4;
  const uint32_t VALUE_META_SIZE = 16;
  // value: [round][thread][valsize][...][crc]
  const uint32_t ROUND_NUM_OFFSET = 0;
  const uint32_t THREAD_ID_OFFSET = 4;
  const uint32_t VALUE_SIZE_OFFSET = 8;
  const uint32_t VALUE_OFFSET = 12;

  bool SanityCheck() {
    if (FLAGS_compression_ratio > 1) {
      fprintf(stderr, "compression_ratio should be between 0 and 1\n");
      return false;
    }
    return true;
  }

  inline bool CompressSlice(const Slice& input, std::string* compressed) {
    bool ok = true;
    switch (FLAGS_compression_type_e) {
      case kSnappyCompression:
        ok = Snappy_Compress(Options().compression_opts, input.data(),
                             input.size(), compressed);
        break;
      case kZlibCompression:
        ok = Zlib_Compress(Options().compression_opts, 2, input.data(),
                           input.size(), compressed);
        break;
      case kBZip2Compression:
        ok = BZip2_Compress(Options().compression_opts, 2, input.data(),
                            input.size(), compressed);
        break;
      case kLZ4Compression:
        ok = LZ4_Compress(Options().compression_opts, 2, input.data(),
                          input.size(), compressed);
        break;
      case kLZ4HCCompression:
        ok = LZ4HC_Compress(Options().compression_opts, 2, input.data(),
                            input.size(), compressed);
        break;
      case kXpressCompression:
        ok = XPRESS_Compress(input.data(), input.size(), compressed);
        break;
      case kZSTD:
        ok = ZSTD_Compress(Options().compression_opts, input.data(),
                           input.size(), compressed);
        break;
      default:
        ok = false;
    }
    return ok;
  }

  void PrintHeader() {
    PrintEnvironment();
    fprintf(stdout, "Keys:       %d bytes each\n", FLAGS_key_size);
    fprintf(stdout, "Values:     %d bytes each (%d bytes after compression)\n",
            FLAGS_value_size,
            static_cast<int>(FLAGS_value_size * FLAGS_compression_ratio + 0.5));
    fprintf(stdout, "Entries:    %" PRIu64 "\n", num_);
    fprintf(stdout, "RawSize:    %.1f MB (estimated)\n",
            ((static_cast<int64_t>(FLAGS_key_size + FLAGS_value_size) * num_) /
             1048576.0));
    fprintf(stdout, "FileSize:   %.1f MB (estimated)\n",
            (((FLAGS_key_size + FLAGS_value_size * FLAGS_compression_ratio) *
              num_) /
             1048576.0));
    fprintf(stdout, "Write rate: %" PRIu64 " bytes/second\n",
            FLAGS_benchmark_write_rate_limit);
    fprintf(stdout, "Read rate: %" PRIu64 " ops/second\n",
            FLAGS_benchmark_read_rate_limit);
    if (FLAGS_enable_numa) {
      fprintf(stderr, "Running in NUMA enabled mode.\n");
#ifndef NUMA
      fprintf(stderr, "NUMA is not defined in the system.\n");
      exit(1);
#else
      if (numa_available() == -1) {
        fprintf(stderr, "NUMA is not supported by the system.\n");
        exit(1);
      }
#endif
    }

    auto compression = CompressionTypeToString(FLAGS_compression_type_e);
    fprintf(stdout, "Compression: %s\n", compression.c_str());

    switch (FLAGS_rep_factory) {
      case kSkipList:
        fprintf(stdout, "Memtablerep: skip_list\n");
        break;
      case kART:
        fprintf(stdout, "Memtablerep: art\n");
        break;
    }
    fprintf(stdout, "Perf Level: %d\n", FLAGS_perf_level);

    PrintWarnings(compression.c_str());
    fprintf(stdout, "------------------------------------------------\n");
  }

  void PrintWarnings(const char* compression) {
#if defined(__GNUC__) && !defined(__OPTIMIZE__)
    fprintf(
        stdout,
        "WARNING: Optimization is disabled: benchmarks unnecessarily slow\n");
#endif
#ifndef NDEBUG
    fprintf(stdout,
            "WARNING: Assertions are enabled; benchmarks unnecessarily slow\n");
#endif
    if (FLAGS_compression_type_e != kNoCompression) {
      // The test string should not be too small.
      const int len = FLAGS_block_size;
      std::string input_str(len, 'y');
      std::string compressed;
      bool result = CompressSlice(Slice(input_str), &compressed);

      if (!result) {
        fprintf(stdout, "WARNING: %s compression is not enabled\n",
                compression);
      } else if (compressed.size() >= input_str.size()) {
        fprintf(stdout, "WARNING: %s compression is not effective\n",
                compression);
      }
    }
  }

// Current the following isn't equivalent to OS_LINUX.
#if defined(__linux)
  static Slice TrimSpace(Slice s) {
    unsigned int start = 0;
    while (start < s.size() && isspace(s[start])) {
      start++;
    }
    unsigned int limit = static_cast<unsigned int>(s.size());
    while (limit > start && isspace(s[limit - 1])) {
      limit--;
    }
    return Slice(s.data() + start, limit - start);
  }
#endif

  void PrintEnvironment() {
    fprintf(stderr, "SE:    version %d.%d\n", kMajorVersion,
            kMinorVersion);

#if defined(__linux)
    time_t now = time(nullptr);
    char buf[52];
    // Lint complains about ctime() usage, so replace it with ctime_r(). The
    // requirement is to provide a buffer which is at least 26 bytes.
    fprintf(stderr, "Date:       %s",
            ctime_r(&now, buf));  // ctime_r() adds newline

    FILE* cpuinfo = fopen("/proc/cpuinfo", "r");
    if (cpuinfo != nullptr) {
      char line[1000];
      int num_cpus = 0;
      std::string cpu_type;
      std::string cache_size;
      while (fgets(line, sizeof(line), cpuinfo) != nullptr) {
        const char* sep = strchr(line, ':');
        if (sep == nullptr) {
          continue;
        }
        Slice key = TrimSpace(Slice(line, sep - 1 - line));
        Slice val = TrimSpace(Slice(sep + 1));
        if (key == "model name") {
          ++num_cpus;
          cpu_type = val.ToString();
        } else if (key == "cache size") {
          cache_size = val.ToString();
        }
      }
      fclose(cpuinfo);
      fprintf(stderr, "CPU:        %d * %s\n", num_cpus, cpu_type.c_str());
      fprintf(stderr, "CPUCache:   %s\n", cache_size.c_str());
    }
#endif
  }

  static bool KeyExpired(const TimestampEmulator* timestamp_emulator,
                         const Slice& key) {
    const char* pos = key.data();
    pos += 8;
    uint64_t timestamp = 0;
    if (port::kLittleEndian) {
      int bytes_to_fill = 8;
      for (int i = 0; i < bytes_to_fill; ++i) {
        timestamp |= (static_cast<uint64_t>(static_cast<unsigned char>(pos[i]))
                      << ((bytes_to_fill - i - 1) << 3));
      }
    } else {
      memcpy(&timestamp, pos, sizeof(timestamp));
    }
    return timestamp_emulator->Get() - timestamp > FLAGS_time_range;
  }

  class ExpiredTimeFilter : public CompactionFilter {
   public:
    explicit ExpiredTimeFilter(
        const std::shared_ptr<TimestampEmulator>& timestamp_emulator)
        : timestamp_emulator_(timestamp_emulator) {}
    bool Filter(int level, const Slice& key, const Slice& existing_value,
                std::string* new_value, bool* value_changed) const override {
      return KeyExpired(timestamp_emulator_.get(), key);
    }
    const char* Name() const override { return "ExpiredTimeFilter"; }

   private:
    std::shared_ptr<TimestampEmulator> timestamp_emulator_;
  };

  std::shared_ptr<Cache> NewCache(int64_t capacity, size_t mod_id) {
    if (capacity <= 0) {
      return nullptr;
    }
    if (FLAGS_use_clock_cache) {
      auto cache = NewClockCache((size_t)capacity, FLAGS_cache_numshardbits);
      if (!cache) {
        fprintf(stderr, "Clock cache not supported.");
        exit(1);
      }
      return cache;
#ifdef TBB
    } else if (FLAGS_use_xcache) {
      return NewXCache();
#endif
    } else {
      return NewLRUCache((size_t)capacity, FLAGS_cache_numshardbits,
                         false /*strict_capacity_limit*/,
                         FLAGS_cache_high_pri_pool_ratio,
                         0.3 /*old_pool_priority*/,
                         mod_id);
    }
  }

 public:
  Benchmark()
      : cache_(NewCache(FLAGS_cache_size, ModId::kDefaultBlockCache)),
        compressed_cache_(NewCache(FLAGS_compressed_cache_size, ModId::kBkCacheCompress)),
        filter_policy_(FLAGS_bloom_bits >= 0
                           ? NewBloomFilterPolicy(FLAGS_bloom_bits,
                                                  FLAGS_use_block_based_filter)
                           : nullptr),
        num_(FLAGS_num),
        value_size_(FLAGS_value_size),
        key_size_(FLAGS_key_size),
        entries_per_batch_(1),
        reads_(FLAGS_reads < 0 ? FLAGS_num : FLAGS_reads),
        read_random_exp_range_(0.0),
        writes_(FLAGS_writes < 0 ? FLAGS_num : FLAGS_writes),
        readwrites_(
            (FLAGS_writes < 0 && FLAGS_reads < 0)
                ? FLAGS_num
                : ((FLAGS_writes > FLAGS_reads) ? FLAGS_writes : FLAGS_reads)),
        merge_keys_(FLAGS_merge_keys < 0 ? FLAGS_num : FLAGS_merge_keys),
        report_file_operations_(FLAGS_report_file_operations),
        STRESS_VALUE_SIZE(value_size_ + sizeof(uint32_t) * 3),
        STRESS_DATA_SIZE(value_size_ + sizeof(uint32_t) * 2),
        bench_log(nullptr),
        stress_lock_mgr_(),
        stress_worker_cond_(&stress_worker_mutex_),
        is_recover_mode(false),
        rand_large_val(1000) {
    if (report_file_operations_) {
      FLAGS_env = new ReportFileOpEnv(Env::Default());
    }

    std::vector<std::string> files;
    FLAGS_env->GetChildren(FLAGS_db, &files);
    for (size_t i = 0; i < files.size(); i++) {
      if (Slice(files[i]).starts_with("heap-")) {
        FLAGS_env->DeleteFile(FLAGS_db + "/" + files[i]);
      }
    }
    if (!FLAGS_use_existing_db) {
      Options options;
      if (!FLAGS_wal_dir.empty()) {
        options.wal_dir = FLAGS_wal_dir;
      }
      DestroyDB(FLAGS_db, options);
      if (!FLAGS_wal_dir.empty()) {
        FLAGS_env->DeleteDir(FLAGS_wal_dir);
      }

      if (FLAGS_num_multi_db > 1) {
        FLAGS_env->CreateDir(FLAGS_db);
        if (!FLAGS_wal_dir.empty()) {
          FLAGS_env->CreateDir(FLAGS_wal_dir);
        }
      }
    }
    for (size_t i = 0; i != STRESS_MAX_STAGE; ++i) {
      stress_worker_count_[i] = FLAGS_threads;
    }
  }

  ~Benchmark() {
    db_.DeleteDBs();
    if (cache_.get() != nullptr) {
      // this will leak, but we're shutting down so nobody cares
      cache_->DisownData();
    }
  }

  Slice AllocateKey(std::unique_ptr<const char[]>* key_guard) {
    char* data = new char[key_size_];
    const char* const_data = data;
    key_guard->reset(const_data);
    return Slice(key_guard->get(), key_size_);
  }

  Slice AllocateKey(std::unique_ptr<const char[]>* key_guard, size_t key_size) {
    char* data = new char[key_size];
    const char* const_data = data;
    key_guard->reset(const_data);
    return Slice(key_guard->get(), key_size);
  }

  // Generate key according to the given specification and random number.
  // The resulting key will have the following format (if keys_per_prefix_
  // is positive), extra trailing bytes are either cut off or padded with '0'.
  // The prefix value is derived from key value.
  //   ----------------------------
  //   | prefix 00000 | key 00000 |
  //   ----------------------------
  // If keys_per_prefix_ is 0, the key is simply a binary representation of
  // random number followed by trailing '0's
  //   ----------------------------
  //   |        key 00000         |
  //   ----------------------------
  void GenerateKeyFromInt(uint64_t v, int64_t num_keys, Slice* key) {
    char* start = const_cast<char*>(key->data());
    char* pos = start;
    int bytes_to_fill = std::min(key_size_ - static_cast<int>(pos - start), 8);
    if (port::kLittleEndian) {
      for (int i = 0; i < bytes_to_fill; ++i) {
        pos[i] = (v >> ((bytes_to_fill - i - 1) << 3)) & 0xFF;
      }
    } else {
      memcpy(pos, static_cast<void*>(&v), bytes_to_fill);
    }
    pos += bytes_to_fill;
    if (key_size_ > pos - start) {
      memset(pos, '0', key_size_ - (pos - start));
    }
  }

  std::string GetPathForMultiple(std::string base_name, size_t id) {
    if (!base_name.empty()) {
#ifndef OS_WIN
      if (base_name.back() != '/') {
        base_name += '/';
      }
#else
      if (base_name.back() != '\\') {
        base_name += '\\';
      }
#endif
    }
    return base_name + ToString(id);
  }

  void Run() {
    if (!SanityCheck()) {
      exit(1);
    }
    Open(&open_options_);
    PrintHeader();
    std::stringstream benchmark_stream(FLAGS_benchmarks);
    std::string name;
    std::unique_ptr<ExpiredTimeFilter> filter;
    while (std::getline(benchmark_stream, name, ',')) {
      time_t now = time(nullptr);
      char buf[52];
      fprintf(stderr, "start case(%s) time: %s\n", name.c_str(),
              ctime_r(&now, buf));  // ctime_r() adds newline
      // Sanitize parameters
      num_ = FLAGS_num;
      reads_ = (FLAGS_reads < 0 ? FLAGS_num : FLAGS_reads);
      writes_ = (FLAGS_writes < 0 ? FLAGS_num : FLAGS_writes);
      deletes_ = (FLAGS_deletes < 0 ? FLAGS_num : FLAGS_deletes);
      value_size_ = FLAGS_value_size;
      key_size_ = FLAGS_key_size;
      entries_per_batch_ = FLAGS_batch_size;
      range_tombstone_width_ = FLAGS_range_tombstone_width;
      max_num_range_tombstones_ = FLAGS_max_num_range_tombstones;
      write_options_ = WriteOptions();
      read_random_exp_range_ = FLAGS_read_random_exp_range;
      if (FLAGS_sync) {
        write_options_.sync = true;
      }
      write_options_.disableWAL = FLAGS_disable_wal;

      void (Benchmark::*method)(ThreadState*) = nullptr;
      void (Benchmark::*post_process_method)() = nullptr;

      bool fresh_db = false;
      int num_threads = FLAGS_threads;

      int num_repeat = 1;
      int num_warmup = 0;
      if (!name.empty() && *name.rbegin() == ']') {
        auto it = name.find('[');
        if (it == std::string::npos) {
          fprintf(stderr, "unknown benchmark arguments '%s'\n", name.c_str());
          exit(1);
        }
        std::string args = name.substr(it + 1);
        args.resize(args.size() - 1);
        name.resize(it);

        std::string bench_arg;
        std::stringstream args_stream(args);
        while (std::getline(args_stream, bench_arg, '-')) {
          if (bench_arg.empty()) {
            continue;
          }
          if (bench_arg[0] == 'X') {
            // Repeat the benchmark n times
            std::string num_str = bench_arg.substr(1);
            num_repeat = std::stoi(num_str);
          } else if (bench_arg[0] == 'W') {
            // Warm up the benchmark for n times
            std::string num_str = bench_arg.substr(1);
            num_warmup = std::stoi(num_str);
          }
        }
      }

      // Both fillseqdeterministic and filluniquerandomdeterministic
      // fill the levels except the max level with UNIQUE_RANDOM
      // and fill the max level with fillseq and filluniquerandom, respectively
      if (name == "fillseqdeterministic" ||
          name == "filluniquerandomdeterministic") {
        if (!FLAGS_disable_auto_compactions) {
          fprintf(stderr,
                  "Please disable_auto_compactions in FillDeterministic "
                  "benchmark\n");
          exit(1);
        }
        if (num_threads > 1) {
          fprintf(stderr,
                  "filldeterministic multithreaded not supported"
                  ", use 1 thread\n");
          num_threads = 1;
        }
        fresh_db = true;
        if (name == "fillseqdeterministic") {
          method = &Benchmark::WriteSeqDeterministic;
        } else {
          method = &Benchmark::WriteUniqueRandomDeterministic;
        }
      } else if (name == "fillseq") {
        fresh_db = true;
        method = &Benchmark::WriteSeq;
      } else if (name == "fillbatch") {
        fresh_db = true;
        entries_per_batch_ = 1000;
        method = &Benchmark::WriteSeq;
      } else if (name == "fillrandom") {
        fresh_db = true;
        method = &Benchmark::WriteRandom;
      } else if (name == "filluniquerandom") {
        fresh_db = true;
        if (num_threads > 1) {
          fprintf(stderr,
                  "filluniquerandom multithreaded not supported"
                  ", use 1 thread");
          num_threads = 1;
        }
        method = &Benchmark::WriteUniqueRandom;
      } else if (name == "overwrite") {
        method = &Benchmark::WriteRandom;
      } else if (name == "fillsync") {
        fresh_db = true;
        num_ /= 1000;
        write_options_.sync = true;
        method = &Benchmark::WriteRandom;
      } else if (name == "fill100K") {
        fresh_db = true;
        num_ /= 1000;
        value_size_ = 100 * 1000;
        method = &Benchmark::WriteRandom;
      } else if (name == "readseq") {
        method = &Benchmark::ReadSequential;
      } else if (name == "readtocache") {
        method = &Benchmark::ReadSequential;
        num_threads = 1;
        reads_ = num_;
      } else if (name == "readreverse") {
        method = &Benchmark::ReadReverse;
      } else if (name == "readrandom") {
        method = &Benchmark::ReadRandom;
      } else if (name == "readrandomfast") {
        method = &Benchmark::ReadRandomFast;
      } else if (name == "multireadrandom") {
        fprintf(stderr, "entries_per_batch = %" PRIi64 "\n",
                entries_per_batch_);
        method = &Benchmark::MultiReadRandom;
      } else if (name == "readmissing") {
        ++key_size_;
        method = &Benchmark::ReadRandom;
      } else if (name == "newiterator") {
        method = &Benchmark::IteratorCreation;
      } else if (name == "newiteratorwhilewriting") {
        num_threads++;  // Add extra thread for writing
        method = &Benchmark::IteratorCreationWhileWriting;
      } else if (name == "seekrandom") {
        method = &Benchmark::SeekRandom;
      } else if (name == "seekrandomwhilewriting") {
        num_threads++;  // Add extra thread for writing
        method = &Benchmark::SeekRandomWhileWriting;
      } else if (name == "seekrandomwhilemerging") {
        num_threads++;  // Add extra thread for merging
        method = &Benchmark::SeekRandomWhileMerging;
      } else if (name == "readrandomsmall") {
        reads_ /= 1000;
        method = &Benchmark::ReadRandom;
      } else if (name == "deleteseq") {
        method = &Benchmark::DeleteSeq;
      } else if (name == "deleterandom") {
        method = &Benchmark::DeleteRandom;
      } else if (name == "readwhilewriting") {
        num_threads++;  // Add extra thread for writing
        method = &Benchmark::ReadWhileWriting;
      } else if (name == "readwhilemerging") {
        num_threads++;  // Add extra thread for writing
        method = &Benchmark::ReadWhileMerging;
      } else if (name == "readrandomwriterandom") {
        method = &Benchmark::ReadRandomWriteRandom;
      } else if (name == "updaterandom") {
        method = &Benchmark::UpdateRandom;
      } else if (name == "appendrandom") {
        method = &Benchmark::AppendRandom;
      } else if (name == "randomwithverify") {
        method = &Benchmark::RandomWithVerify;
      } else if (name == "fillseekseq") {
        method = &Benchmark::WriteSeqSeekSeq;
      } else if (name == "compact") {
        method = &Benchmark::Compact;
      } else if (name == "crc32c") {
        method = &Benchmark::Crc32c;
      } else if (name == "xxhash") {
        method = &Benchmark::xxHash;
      } else if (name == "acquireload") {
        method = &Benchmark::AcquireLoad;
      } else if (name == "compress") {
        method = &Benchmark::Compress;
      } else if (name == "uncompress") {
        method = &Benchmark::Uncompress;
#ifndef ROCKSDB_LITE
      } else if (name == "randomtransaction") {
        method = &Benchmark::RandomTransaction;
        post_process_method = &Benchmark::RandomTransactionVerify;
#endif  // ROCKSDB_LITE
      } else if (name == "randomreplacekeys") {
        fresh_db = true;
        method = &Benchmark::RandomReplaceKeys;
      } else if (name == "stress") {
        method = &Benchmark::stress;
        is_recover_mode = false;
        fresh_db = true;
        stress_init(fresh_db, true /* in_memory */);
        num_threads = FLAGS_threads;
      } else if (name == "stress_recover") {
        method = &Benchmark::stress;
        is_recover_mode = true;
        stress_init(fresh_db, true /* in_memory */);
        stress_load(fresh_db);
        num_threads = FLAGS_threads;
      } else if (name == "stress_durability") {
        method = &Benchmark::stress;
        fresh_db = !FLAGS_use_existing_db;
        stress_init(fresh_db);
        num_threads = FLAGS_threads;
      } else if (name == "stress_check") {
        method = &Benchmark::stress_durability_check;
        fresh_db = false;
        stress_init(fresh_db);
        num_threads = FLAGS_threads;
      } else if (name == "meta_check") {
        // stress for storage meta operation
        method = &Benchmark::meta_check;
        // only one thread
        num_threads = 1; 
      } else if (name == "timeseries") {
        timestamp_emulator_.reset(new TimestampEmulator());
        if (FLAGS_expire_style == "compaction_filter") {
          filter.reset(new ExpiredTimeFilter(timestamp_emulator_));
          fprintf(stdout, "Compaction filter is used to remove expired data");
          open_options_.compaction_filter = filter.get();
        }
        fresh_db = true;
        method = &Benchmark::TimeSeries;
      } else if (name == "stats") {
        PrintStats("smartengine.stats");
      } else if (name == "resetstats") {
        ResetStats();
      } else if (name == "levelstats") {
        PrintStats("smartengine.levelstats");
      } else if (name == "sstables") {
        PrintStats("smartengine.sstables");
      } else if (!name.empty()) {  // No error message for empty name
        fprintf(stderr, "unknown benchmark '%s'\n", name.c_str());
        exit(1);
      }

      //TODO:yuanfeng
      if (fresh_db && false) {
        if (FLAGS_use_existing_db) {
          fprintf(stdout, "%-12s : skipped (--use_existing_db is true)\n",
                  name.c_str());
          method = nullptr;
        } else {
          if (db_.db != nullptr) {
            db_.DeleteDBs();
            DestroyDB(FLAGS_db, open_options_);
          }
          Options options = open_options_;
          for (size_t i = 0; i < multi_dbs_.size(); i++) {
            delete multi_dbs_[i].db;
            if (!open_options_.wal_dir.empty()) {
              options.wal_dir = GetPathForMultiple(open_options_.wal_dir, i);
            }
            DestroyDB(GetPathForMultiple(FLAGS_db, i), options);
          }
          multi_dbs_.clear();
        }
        Open(&open_options_);  // use open_options for the last accessed
      }

      if (method != nullptr) {
        fprintf(stdout, "DB path: [%s]\n", FLAGS_db.c_str());
        if (num_warmup > 0) {
          printf("Warming up benchmark by running %d times\n", num_warmup);
        }

        for (int i = 0; i < num_warmup; i++) {
          RunBenchmark(num_threads, name, method);
        }

        if (num_repeat > 1) {
          printf("Running benchmark for %d times\n", num_repeat);
        }

        CombinedStats combined_stats;
        for (int i = 0; i < num_repeat; i++) {
          Stats stats = RunBenchmark(num_threads, name, method);
          combined_stats.AddStats(stats);
        }
        if (num_repeat > 1) {
          combined_stats.Report(name);
        }
      }
      if (post_process_method != nullptr) {
        (this->*post_process_method)();
      }

      if (FLAGS_statistics) {
        fprintf(stdout, "STATISTICS:\n%s\n", dbstats->ToString().c_str());
      }

      now = time(nullptr);
      if (nullptr != bench_log) {
        stress_log("stress check success.\n");
      }
      std::string time_string(ctime_r(&now, buf));
      fprintf(
          stderr,
          "------- run (%s) case end time(%s), continue next if any -----\n",
          name.c_str(), time_string.substr(0, time_string.size() - 1).c_str());
      if (FLAGS_flush_write_buffer_after_test) {//we can flush for blockcache or rowcache test
        DBImpl *db_impl = (reinterpret_cast<DBImpl *>(db_.db));
        int index = 0;
        fprintf(stderr, "begin to flush total %ld subtable:", db_.cfh.size());
        for (ColumnFamilyHandle* handle : db_.cfh) {
          fprintf(stderr, " %d", index);
          db_impl->Flush(smartengine::common::FlushOptions(), handle);
          index = index + 1;
        }
        fprintf(stderr, "flush total %ld subtable completed\n", db_.cfh.size());
        FLAGS_flush_write_buffer_after_test = false;
      }
      //sleep(3);
    }
  }

 private:
  std::shared_ptr<TimestampEmulator> timestamp_emulator_;

  struct ThreadArg {
    Benchmark* bm;
    SharedState* shared;
    ThreadState* thread;
    void (Benchmark::*method)(ThreadState*);
  };

  static void ThreadBody(void* v) {
    ThreadArg* arg = reinterpret_cast<ThreadArg*>(v);
    SharedState* shared = arg->shared;
    ThreadState* thread = arg->thread;
    {
      MutexLock l(&shared->mu);
      shared->num_initialized++;
      if (shared->num_initialized >= shared->total) {
        shared->cv.SignalAll();
      }
      while (!shared->start) {
        shared->cv.Wait();
      }
    }

    SetPerfLevel(static_cast<PerfLevel>(shared->perf_level));
    thread->stats.Start(thread->tid);
    (arg->bm->*(arg->method))(thread);
    thread->stats.Stop();

    {
      MutexLock l(&shared->mu);
      shared->num_done++;
      if (shared->num_done >= shared->total) {
        shared->cv.SignalAll();
      }
    }
  }

  Stats RunBenchmark(int n, Slice name,
                     void (Benchmark::*method)(ThreadState*)) {
    SharedState shared;
    shared.total = n;
    shared.num_initialized = 0;
    shared.num_done = 0;
    shared.start = false;
    if (FLAGS_benchmark_write_rate_limit > 0) {
      shared.write_rate_limiter.reset(
          NewGenericRateLimiter(FLAGS_benchmark_write_rate_limit));
    }
    if (FLAGS_benchmark_read_rate_limit > 0) {
      shared.read_rate_limiter.reset(
          NewGenericRateLimiter(FLAGS_benchmark_read_rate_limit));
    }

    std::unique_ptr<ReporterAgent> reporter_agent;
    if (FLAGS_report_interval_seconds > 0) {
      reporter_agent.reset(new ReporterAgent(FLAGS_env, FLAGS_report_file,
                                             FLAGS_report_interval_seconds));
    }

    ThreadArg* arg = new ThreadArg[n];

    for (int i = 0; i < n; i++) {
#ifdef NUMA
      if (FLAGS_enable_numa) {
        // Performs a local allocation of memory to threads in numa node.
        int n_nodes = numa_num_task_nodes();  // Number of nodes in NUMA.
        numa_exit_on_error = 1;
        int numa_node = i % n_nodes;
        bitmask* nodes = numa_allocate_nodemask();
        numa_bitmask_clearall(nodes);
        numa_bitmask_setbit(nodes, numa_node);
        // numa_bind() call binds the process to the node and these
        // properties are passed on to the thread that is created in
        // StartThread method called later in the loop.
        numa_bind(nodes);
        numa_set_strict(1);
        numa_free_nodemask(nodes);
      }
#endif
      arg[i].bm = this;
      arg[i].method = method;
      arg[i].shared = &shared;
      arg[i].thread = new ThreadState(i);
      arg[i].thread->stats.SetReporterAgent(reporter_agent.get());
      arg[i].thread->shared = &shared;
      FLAGS_env->StartThread(ThreadBody, &arg[i]);
    }

    shared.mu.Lock();
    while (shared.num_initialized < n) {
      shared.cv.Wait();
    }

    shared.start = true;
    shared.cv.SignalAll();
    while (shared.num_done < n) {
      shared.cv.Wait();
    }
    shared.mu.Unlock();

    // Stats for some threads can be excluded.
    Stats merge_stats;
    for (int i = 0; i < n; i++) {
      merge_stats.Merge(arg[i].thread->stats);
    }
    merge_stats.Report(name);

    for (int i = 0; i < n; i++) {
      delete arg[i].thread;
    }
    delete[] arg;

    return merge_stats;
  }

  void Crc32c(ThreadState* thread) {
    // Checksum about 500MB of data total
    const int size = 4096;
    const char* label = "(4K per op)";
    std::string data(size, 'x');
    int64_t bytes = 0;
    uint32_t crc = 0;
    while (bytes < 500 * 1048576) {
      crc = crc32c::Value(data.data(), size);
      thread->stats.FinishedOps(nullptr, nullptr, 1, kCrc);
      bytes += size;
    }
    // Print so result is not dead
    fprintf(stderr, "... crc=0x%x\r", static_cast<unsigned int>(crc));

    thread->stats.AddBytes(bytes);
    thread->stats.AddMessage(label);
  }

  void xxHash(ThreadState* thread) {
    // Checksum about 500MB of data total
    const int size = 4096;
    const char* label = "(4K per op)";
    std::string data(size, 'x');
    int64_t bytes = 0;
    unsigned int xxh32 = 0;
    while (bytes < 500 * 1048576) {
      xxh32 = XXH32(data.data(), size, 0);
      thread->stats.FinishedOps(nullptr, nullptr, 1, kHash);
      bytes += size;
    }
    // Print so result is not dead
    fprintf(stderr, "... xxh32=0x%x\r", static_cast<unsigned int>(xxh32));

    thread->stats.AddBytes(bytes);
    thread->stats.AddMessage(label);
  }

  void AcquireLoad(ThreadState* thread) {
    int dummy;
    std::atomic<void*> ap(&dummy);
    int count = 0;
    void* ptr = nullptr;
    thread->stats.AddMessage("(each op is 1000 loads)");
    while (count < 100000) {
      for (int i = 0; i < 1000; i++) {
        ptr = ap.load(std::memory_order_acquire);
      }
      count++;
      thread->stats.FinishedOps(nullptr, nullptr, 1, kOthers);
    }
    if (ptr == nullptr) exit(1);  // Disable unused variable warning.
  }

  void Compress(ThreadState* thread) {
    RandomGenerator gen;
    Slice input = gen.Generate(FLAGS_block_size);
    int64_t bytes = 0;
    int64_t produced = 0;
    bool ok = true;
    std::string compressed;

    // Compress 1G
    while (ok && bytes < int64_t(1) << 30) {
      compressed.clear();
      ok = CompressSlice(input, &compressed);
      produced += compressed.size();
      bytes += input.size();
      thread->stats.FinishedOps(nullptr, nullptr, 1, kCompress);
    }

    if (!ok) {
      thread->stats.AddMessage("(compression failure)");
    } else {
      char buf[340];
      snprintf(buf, sizeof(buf), "(output: %.1f%%)",
               (produced * 100.0) / bytes);
      thread->stats.AddMessage(buf);
      thread->stats.AddBytes(bytes);
    }
  }

  void Uncompress(ThreadState* thread) {
    RandomGenerator gen;
    Slice input = gen.Generate(FLAGS_block_size);
    std::string compressed;

    bool ok = CompressSlice(input, &compressed);
    int64_t bytes = 0;
    int decompress_size;
    while (ok && bytes < 1024 * 1048576) {
      char* uncompressed = nullptr;
      switch (FLAGS_compression_type_e) {
        case kSnappyCompression: {
          // get size and allocate here to make comparison fair
          size_t ulength = 0;
          if (!Snappy_GetUncompressedLength(compressed.data(),
                                            compressed.size(), &ulength)) {
            ok = false;
            break;
          }
          uncompressed = new char[ulength];
          ok = Snappy_Uncompress(compressed.data(), compressed.size(),
                                 uncompressed);
          break;
        }
        case kZlibCompression:
          uncompressed = Zlib_Uncompress(compressed.data(), compressed.size(),
                                         &decompress_size, 2);
          ok = uncompressed != nullptr;
          break;
        case kBZip2Compression:
          uncompressed = BZip2_Uncompress(compressed.data(), compressed.size(),
                                          &decompress_size, 2);
          ok = uncompressed != nullptr;
          break;
        case kLZ4Compression:
          uncompressed = LZ4_Uncompress(compressed.data(), compressed.size(),
                                        &decompress_size, 2);
          ok = uncompressed != nullptr;
          break;
        case kLZ4HCCompression:
          uncompressed = LZ4_Uncompress(compressed.data(), compressed.size(),
                                        &decompress_size, 2);
          ok = uncompressed != nullptr;
          break;
        case kXpressCompression:
          uncompressed = XPRESS_Uncompress(compressed.data(), compressed.size(),
                                           &decompress_size);
          ok = uncompressed != nullptr;
          break;
        case kZSTD:
          uncompressed = ZSTD_Uncompress(compressed.data(), compressed.size(),
                                         &decompress_size);
          ok = uncompressed != nullptr;
          break;
        default:
          ok = false;
      }
      delete[] uncompressed;
      bytes += input.size();
      thread->stats.FinishedOps(nullptr, nullptr, 1, kUncompress);
    }

    if (!ok) {
      thread->stats.AddMessage("(compression failure)");
    } else {
      thread->stats.AddBytes(bytes);
    }
  }


  void InitializeOptionsFromFlags(Options* opts) {
    printf("Initializing SE Options from command-line flags\n");
    Options& options = *opts;

    assert(db_.db == nullptr);
    options.arena_block_size = 32768;
    options.info_log_level = static_cast<util::InfoLogLevel>(FLAGS_info_log_level);
    options.max_log_file_size = FLAGS_max_log_file_size;
    options.log_file_time_to_roll = FLAGS_log_file_time_to_roll;
    options.keep_log_file_num = FLAGS_keep_log_file_num;
    options.create_missing_column_families = FLAGS_num_column_families > 1;
    options.max_open_files = FLAGS_open_files;
    options.db_write_buffer_size = FLAGS_db_write_buffer_size;
    options.db_total_write_buffer_size = FLAGS_db_total_write_buffer_size;
    options.write_buffer_size = FLAGS_write_buffer_size;
    options.flush_delete_percent = FLAGS_flush_delete_percent;
    options.compaction_delete_percent = FLAGS_compaction_delete_percent;
    options.flush_delete_percent_trigger = FLAGS_flush_delete_percent_trigger;
    options.flush_delete_record_trigger = FLAGS_flush_delete_record_trigger;
    options.max_write_buffer_number = FLAGS_max_write_buffer_number;
    options.min_write_buffer_number_to_merge =
        FLAGS_min_write_buffer_number_to_merge;
    options.max_write_buffer_number_to_maintain =
        FLAGS_max_write_buffer_number_to_maintain;
    options.base_background_compactions = FLAGS_base_background_compactions;
    options.max_background_compactions = FLAGS_max_background_compactions;
    options.max_subcompactions = static_cast<uint32_t>(FLAGS_subcompactions);
    options.max_background_flushes = FLAGS_max_background_flushes;
    options.compaction_style = FLAGS_compaction_style_e;
    options.compaction_pri = FLAGS_compaction_pri_e;
    options.allow_mmap_reads = FLAGS_mmap_read;
    options.allow_mmap_writes = FLAGS_mmap_write;
    options.use_direct_reads = FLAGS_use_direct_reads;
    options.use_direct_io_for_flush_and_compaction =
        FLAGS_use_direct_io_for_flush_and_compaction;
#ifndef ROCKSDB_LITE
    options.compaction_options_fifo = CompactionOptionsFIFO(
        FLAGS_fifo_compaction_max_table_files_size_mb * 1024 * 1024);
#endif  // ROCKSDB_LITE

    if (FLAGS_use_uint64_comparator) {
      options.comparator = test::Uint64Comparator();
      if (FLAGS_key_size != 8) {
        fprintf(stderr, "Using Uint64 comparator but key size is not 8.\n");
        exit(1);
      }
    }
    options.memtable_huge_page_size = FLAGS_memtable_use_huge_page ? 2048 : 0;
    options.bloom_locality = FLAGS_bloom_locality;
    options.max_file_opening_threads = FLAGS_file_opening_threads;
    options.new_table_reader_for_compaction_inputs =
        FLAGS_new_table_reader_for_compaction_inputs;
    options.compaction_readahead_size = FLAGS_compaction_readahead_size;
    options.random_access_max_buffer_size = FLAGS_random_access_max_buffer_size;
    options.writable_file_max_buffer_size = FLAGS_writable_file_max_buffer_size;
    options.use_fsync = FLAGS_use_fsync;
    options.num_levels = FLAGS_num_levels;
    options.target_file_size_base = FLAGS_target_file_size_base;
    options.target_file_size_multiplier = FLAGS_target_file_size_multiplier;
    options.max_bytes_for_level_base = FLAGS_max_bytes_for_level_base;
    options.level_compaction_dynamic_level_bytes =
        FLAGS_level_compaction_dynamic_level_bytes;
    options.max_bytes_for_level_multiplier =
        FLAGS_max_bytes_for_level_multiplier;
    switch (FLAGS_rep_factory) {
      case kSkipList:
        options.memtable_factory.reset(new SkipListFactory());
        break;
      case kART:
        options.memtable_factory.reset(
            new ARTFactory());
        break;
      default:
        fprintf(stderr, "Only skip list is supported in lite mode\n");
        exit(1);
    }
    BlockBasedTableOptions block_based_options;
    block_based_options.index_type = BlockBasedTableOptions::kBinarySearch;
    if (cache_ == nullptr) {
      block_based_options.no_block_cache = true;
    }
    block_based_options.cache_index_and_filter_blocks =
        FLAGS_cache_index_and_filter_blocks;
    block_based_options.pin_l0_filter_and_index_blocks_in_cache =
        FLAGS_pin_l0_filter_and_index_blocks_in_cache;
    if (FLAGS_cache_high_pri_pool_ratio > 1e-6) {  // > 0.0 + eps
      block_based_options.cache_index_and_filter_blocks_with_high_priority =
          true;
    }
    block_based_options.block_cache = cache_;
    block_based_options.block_cache_compressed = compressed_cache_;
    block_based_options.block_size = FLAGS_block_size;
    block_based_options.block_restart_interval = FLAGS_block_restart_interval;
    block_based_options.index_block_restart_interval =
        FLAGS_index_block_restart_interval;
    block_based_options.filter_policy = filter_policy_;
    block_based_options.format_version = 2;
    block_based_options.read_amp_bytes_per_bit = FLAGS_read_amp_bytes_per_bit;
    assert(FLAGS_use_extent_based_table);
    block_based_options.format_version = 3;
    options.table_factory.reset(table::NewExtentBasedTableFactory(block_based_options));
    if (FLAGS_max_bytes_for_level_multiplier_additional_v.size() > 0) {
      if (FLAGS_max_bytes_for_level_multiplier_additional_v.size() !=
          (unsigned int)FLAGS_num_levels) {
        fprintf(stderr, "Insufficient number of fanouts specified %d\n",
                (int)FLAGS_max_bytes_for_level_multiplier_additional_v.size());
        exit(1);
      }
      options.max_bytes_for_level_multiplier_additional =
          FLAGS_max_bytes_for_level_multiplier_additional_v;
    }
    options.level0_stop_writes_trigger = FLAGS_level0_stop_writes_trigger;
    options.level0_file_num_compaction_trigger =
        FLAGS_level0_file_num_compaction_trigger;
    options.level0_layer_num_compaction_trigger =
        FLAGS_level0_layer_num_compaction_trigger;
    options.minor_window_size =
        FLAGS_minor_window_size;
    options.level1_extents_major_compaction_trigger =
        FLAGS_level1_extents_major_compaction_trigger;
    options.level2_usage_percent = FLAGS_level2_usage_percent;
    options.level0_slowdown_writes_trigger =
        FLAGS_level0_slowdown_writes_trigger;
    options.compression = FLAGS_compression_type_e;
    options.compression_opts.level = FLAGS_compression_level;
    options.compression_opts.max_dict_bytes = FLAGS_compression_max_dict_bytes;
    options.WAL_ttl_seconds = FLAGS_wal_ttl_seconds;
    options.WAL_size_limit_MB = FLAGS_wal_size_limit_MB;
    options.max_total_wal_size = FLAGS_max_total_wal_size;
    options.scan_add_blocks_limit = FLAGS_scan_add_blocks_limit;
    if (FLAGS_min_level_to_compress >= 0) {
      assert(FLAGS_min_level_to_compress <= FLAGS_num_levels);
      options.compression_per_level.resize(FLAGS_num_levels);
      for (int i = 0; i < FLAGS_min_level_to_compress; i++) {
        options.compression_per_level[i] = kNoCompression;
      }
      for (int i = FLAGS_min_level_to_compress; i < FLAGS_num_levels; i++) {
        options.compression_per_level[i] = FLAGS_compression_type_e;
      }
    }
    options.soft_rate_limit = FLAGS_soft_rate_limit;
    options.hard_rate_limit = FLAGS_hard_rate_limit;
    options.soft_pending_compaction_bytes_limit =
        FLAGS_soft_pending_compaction_bytes_limit;
    options.hard_pending_compaction_bytes_limit =
        FLAGS_hard_pending_compaction_bytes_limit;
    options.delayed_write_rate = FLAGS_delayed_write_rate;
    options.allow_concurrent_memtable_write =
        FLAGS_allow_concurrent_memtable_write;
    options.enable_write_thread_adaptive_yield =
        FLAGS_enable_write_thread_adaptive_yield;
    options.write_thread_max_yield_usec = FLAGS_write_thread_max_yield_usec;
    options.write_thread_slow_yield_usec = FLAGS_write_thread_slow_yield_usec;
    options.rate_limit_delay_max_milliseconds =
        FLAGS_rate_limit_delay_max_milliseconds;
    options.table_cache_numshardbits = FLAGS_table_cache_numshardbits;
    options.max_compaction_bytes = FLAGS_max_compaction_bytes;
    options.disable_auto_compactions = FLAGS_disable_auto_compactions;
    options.optimize_filters_for_hits = FLAGS_optimize_filters_for_hits;

    // fill storage options
    options.advise_random_on_open = FLAGS_advise_random_on_open;
    options.access_hint_on_compaction_start = FLAGS_compaction_fadvice_e;
    options.use_adaptive_mutex = FLAGS_use_adaptive_mutex;
    options.bytes_per_sync = FLAGS_bytes_per_sync;
    options.wal_bytes_per_sync = FLAGS_wal_bytes_per_sync;

    options.report_bg_io_stats = FLAGS_report_bg_io_stats;

    // set universal style compaction configurations, if applicable
    if (FLAGS_universal_size_ratio != 0) {
      options.compaction_options_universal.size_ratio =
          FLAGS_universal_size_ratio;
    }
    if (FLAGS_universal_min_merge_width != 0) {
      options.compaction_options_universal.min_merge_width =
          FLAGS_universal_min_merge_width;
    }
    if (FLAGS_universal_max_merge_width != 0) {
      options.compaction_options_universal.max_merge_width =
          FLAGS_universal_max_merge_width;
    }
    if (FLAGS_universal_max_size_amplification_percent != 0) {
      options.compaction_options_universal.max_size_amplification_percent =
          FLAGS_universal_max_size_amplification_percent;
    }
    if (FLAGS_universal_compression_size_percent != -1) {
      options.compaction_options_universal.compression_size_percent =
          FLAGS_universal_compression_size_percent;
    }
    options.compaction_options_universal.allow_trivial_move =
        FLAGS_universal_allow_trivial_move;
    if (FLAGS_thread_status_per_interval > 0) {
      options.enable_thread_tracking = true;
    }
    if (FLAGS_rate_limiter_bytes_per_sec > 0) {
      options.rate_limiter.reset(
          NewGenericRateLimiter(FLAGS_rate_limiter_bytes_per_sec));
    }

    if (FLAGS_max_batch_group_slot_array_size > 2)
      options.batch_group_slot_array_size =
          FLAGS_max_batch_group_slot_array_size;
    if (FLAGS_max_batch_group_size > 1)
      options.batch_group_max_group_size = FLAGS_max_batch_group_size;
    if (FLAGS_max_batch_group_leader_wait_time_us > 1)
      options.batch_group_max_leader_wait_time_us =
          FLAGS_max_batch_group_leader_wait_time_us;

    if (FLAGS_max_log_buffer_num > 2)
      options.concurrent_writable_file_buffer_num = FLAGS_max_log_buffer_num;
    if (FLAGS_max_single_log_buffer_size > 1024 * 1024)
      options.concurrent_writable_file_single_buffer_size =
          FLAGS_max_single_log_buffer_size;
    if (FLAGS_max_log_buffer_switch_limit > 4 * 1024)
      options.concurrent_writable_file_buffer_switch_limit =
          FLAGS_max_log_buffer_switch_limit;

    options.use_direct_write_for_wal = FLAGS_use_direct_write_for_wal;
    options.cpu_compaction_thread_num = FLAGS_cpu_compaction_thread_num;
    options.fpga_compaction_thread_num = FLAGS_fpga_compaction_thread_num;
    options.fpga_device_id = FLAGS_fpga_device_id;
    options.arena_block_size = FLAGS_arena_block_size;

    if (FLAGS_compaction_type < 2)
      options.compaction_type = FLAGS_compaction_type;
    else 
      fprintf(stderr, "Error: invalid compaction_type is (0,1)\n");

    if (FLAGS_compaction_mode < 3)
      options.compaction_mode = FLAGS_compaction_mode;
    else 
      fprintf(stderr,"Error: valid compaction_mode is (0,1,2)\n");

  }

  void InitializeOptionsGeneral(Options* opts) {
    Options& options = *opts;

    options.statistics = dbstats;
    options.wal_dir = FLAGS_wal_dir;
    options.create_if_missing = !FLAGS_use_existing_db;
    options.stats_dump_period_sec = FLAGS_stats_dump_period_sec;
    options.dump_malloc_stats = FLAGS_dump_malloc_stats;
    options.query_trace_print_stats = FLAGS_query_trace_print_stats;

    if (FLAGS_row_cache_size) {
      NewRowCache(FLAGS_row_cache_size, options.row_cache);
    }
    if (FLAGS_enable_io_prio) {
      FLAGS_env->LowerThreadPoolIOPriority(Env::LOW);
      FLAGS_env->LowerThreadPoolIOPriority(Env::HIGH);
    }
    options.env = FLAGS_env;

    if (FLAGS_num_multi_db <= 1) {
      OpenDb(options, FLAGS_db, &db_);
    } else {
      multi_dbs_.clear();
      multi_dbs_.resize(FLAGS_num_multi_db);
      auto wal_dir = options.wal_dir;
      for (int i = 0; i < FLAGS_num_multi_db; i++) {
        if (!wal_dir.empty()) {
          options.wal_dir = GetPathForMultiple(wal_dir, i);
        }
        OpenDb(options, GetPathForMultiple(FLAGS_db, i), &multi_dbs_[i]);
      }
      options.wal_dir = wal_dir;
    }
  }

  void Open(Options* opts)
  {
    InitializeOptionsFromFlags(opts);
    InitializeOptionsGeneral(opts);
  }

  void OpenDb(const Options& options, const std::string& db_name,
              DBWithColumnFamilies* db) {
    Status s;
    // Open with column families if necessary.
    if (FLAGS_num_column_families >= 1) {
      size_t num_hot = FLAGS_num_column_families;
      if (FLAGS_num_hot_column_families > 0 &&
          FLAGS_num_hot_column_families < FLAGS_num_column_families) {
        num_hot = FLAGS_num_hot_column_families;
      } else {
        FLAGS_num_hot_column_families = FLAGS_num_column_families;
      }
      std::vector<ColumnFamilyDescriptor> column_families;
      for (size_t i = 0; i < num_hot; i++) {
        column_families.push_back(ColumnFamilyDescriptor(
            ColumnFamilyName(i), ColumnFamilyOptions(options)));
      }
#ifndef ROCKSDB_LITE
      if (FLAGS_optimistic_transaction_db) {
        s = OptimisticTransactionDB::Open(options, db_name, column_families,
                                          &db->cfh, &db->opt_txn_db);
        if (s.ok()) {
          db->db = db->opt_txn_db->GetBaseDB();
        }
      } else if (FLAGS_transaction_db) {
        TransactionDB* ptr;
        TransactionDBOptions txn_db_options;
        s = TransactionDB::Open(options, txn_db_options, db_name,
                                column_families, &db->cfh, &ptr);
        if (s.ok()) {
          db->db = ptr;
        }
      } else {
        s = DB::Open(options, db_name, column_families, &db->cfh, &db->db);
      }
#else
      s = DB::Open(options, db_name, column_families, &db->cfh, &db->db);
#endif  // ROCKSDB_LITE
      DBImpl *dbimpl = (reinterpret_cast<DBImpl *>(db->db));
      ColumnFamilyHandle *cf_handle = nullptr;
      if (!FLAGS_use_existing_db) {
        db->cfh.clear();
        db->cfh.push_back(dbimpl->DefaultColumnFamily());
        for (uint64_t i = 1; i <= num_hot; ++i) {
          ColumnFamilyOptions cf_options(options);
          CreateSubTableArgs args(i, cf_options, true, i) ;
          s = dbimpl->CreateColumnFamily(args, &cf_handle);
          if (!s.ok()) {
            abort();
          }
          //fprintf(stderr, "db = %p\n", db);
          //fprintf(stderr, "i = %ld, cfh = %p\n", i, cf_handle);
          (reinterpret_cast<ColumnFamilyHandleImpl *>(cf_handle))->cfd();
          db->cfh.push_back(cf_handle);
        }
        fprintf(stderr, "create %ld subtables for test \n", num_hot);
      } else {
        fprintf(stderr, "open use exist db\n");
      }
      //db->cfh.resize(FLAGS_num_column_families);
      db->num_created = num_hot;
      db->num_hot = num_hot;
#ifndef ROCKSDB_LITE
    } else if (FLAGS_optimistic_transaction_db) {
      s = OptimisticTransactionDB::Open(options, db_name, &db->opt_txn_db);
      if (s.ok()) {
        db->db = db->opt_txn_db->GetBaseDB();
      }
    } else if (FLAGS_transaction_db) {
      TransactionDB* ptr;
      TransactionDBOptions txn_db_options;
      s = TransactionDB::Open(options, txn_db_options, db_name, &ptr);
      if (s.ok()) {
        db->db = ptr;
      }
#endif  // ROCKSDB_LITE
    } else {
      s = DB::Open(options, db_name, &db->db);
    }
    if (!s.ok()) {
      fprintf(stderr, "open error: %s\n", s.ToString().c_str());
      exit(1);
    }
  }

  enum WriteMode { RANDOM, SEQUENTIAL, UNIQUE_RANDOM };

  void WriteSeqDeterministic(ThreadState* thread) {
    DoDeterministicCompact(thread, open_options_.compaction_style, SEQUENTIAL);
  }

  void WriteUniqueRandomDeterministic(ThreadState* thread) {
    DoDeterministicCompact(thread, open_options_.compaction_style,
                           UNIQUE_RANDOM);
  }

  void WriteSeq(ThreadState* thread) { DoWrite(thread, SEQUENTIAL); }

  void WriteRandom(ThreadState* thread) { DoWrite(thread, RANDOM); }

  void WriteUniqueRandom(ThreadState* thread) {
    DoWrite(thread, UNIQUE_RANDOM);
  }

  class KeyGenerator {
   public:
    KeyGenerator(Random64* rand, WriteMode mode, uint64_t num,
                 uint64_t num_per_set = 64 * 1024, uint64_t start = 0)
        : rand_(rand), mode_(mode), num_(num), next_(0) {
      if (mode_ == UNIQUE_RANDOM) {
        // NOTE: if memory consumption of this approach becomes a concern,
        // we can either break it into pieces and only random shuffle a section
        // each time. Alternatively, use a bit map implementation
        // (https://reviews.facebook.net/differential/diff/54627/)
        values_.resize(num_);
        for (uint64_t i = 0; i < num_; ++i) {
          values_[i] = i;
        }
        std::shuffle(
            values_.begin(), values_.end(),
            std::default_random_engine(static_cast<unsigned int>(FLAGS_seed)));
      }
      if (mode_ == SEQUENTIAL) {
        next_ = start;
      }
    }

    uint64_t Next() {
      switch (mode_) {
        case SEQUENTIAL:
          return next_++;
        case RANDOM:
          return rand_->Next() % num_;
        case UNIQUE_RANDOM:
          assert(next_ < num_);
          return values_[next_++];
      }
      assert(false);
      return std::numeric_limits<uint64_t>::max();
    }

   private:
    Random64* rand_;
    WriteMode mode_;
    const uint64_t num_;
    uint64_t next_;
    std::vector<uint64_t> values_;
  };

  DB* SelectDB(ThreadState* thread) { return SelectDBWithCfh(thread)->db; }

  DBWithColumnFamilies* SelectDBWithCfh(ThreadState* thread) {
    return SelectDBWithCfh(thread->rand.Next());
  }

  DBWithColumnFamilies* SelectDBWithCfh(uint64_t rand_int) {
    if (db_.db != nullptr) {
      return &db_;
    } else {
      return &multi_dbs_[rand_int % multi_dbs_.size()];
    }
  }

  void DoWrite(ThreadState* thread, WriteMode write_mode) {
    const int test_duration = write_mode == RANDOM ? FLAGS_duration : 0;
    const int64_t num_ops = writes_ == 0 ? num_ : writes_;

    size_t num_key_gens = 1;
    if (db_.db == nullptr) {
      num_key_gens = multi_dbs_.size();
    }
    std::vector<std::unique_ptr<KeyGenerator>> key_gens(num_key_gens);
    int64_t max_ops = num_ops * num_key_gens;
    int64_t ops_per_stage = max_ops;
    if (FLAGS_num_column_families > 1 && FLAGS_num_hot_column_families > 0) {
      ops_per_stage = (max_ops - 1) / (FLAGS_num_column_families /
                                       FLAGS_num_hot_column_families) +
                      1;
    }

    Duration duration(test_duration, max_ops, ops_per_stage);
    for (size_t i = 0; i < num_key_gens; i++) {
      key_gens[i].reset(new KeyGenerator(&(thread->rand), write_mode, num_,
                                         ops_per_stage,
                                         num_ * (thread->tid + 0)));
    }

    if (num_ != FLAGS_num) {
      char msg[100];
      snprintf(msg, sizeof(msg), "(%" PRIu64 " ops)", num_);
      thread->stats.AddMessage(msg);
    }

    RandomGenerator gen;
    Status s;
    int64_t bytes = 0;

    std::unique_ptr<const char[]> key_guard;
    Slice key = AllocateKey(&key_guard);
    int64_t stage = 0;
    int64_t num_written = 0;

    while (!duration.Done(entries_per_batch_)) {
      if (duration.GetStage() != stage) {
        stage = duration.GetStage();
        if (db_.db != nullptr) {
          db_.CreateNewCf(open_options_, stage);
        } else {
          for (auto& db : multi_dbs_) {
            db.CreateNewCf(open_options_, stage);
          }
        }
      }
      WriteBatch* batch = new WriteBatch();
      size_t id = thread->rand.Next() % num_key_gens;
      DBWithColumnFamilies* db_with_cfh = SelectDBWithCfh(id);
      batch->Clear();

      //if (thread->shared->write_rate_limiter.get() != nullptr) {
      //  thread->shared->write_rate_limiter->Request(
      //      entries_per_batch_ * (value_size_ + key_size_), Env::IO_HIGH,
      //      nullptr /* stats */);
      //  // Set time at which last op finished to Now() to hide latency and
      //  // sleep from rate limiter. Also, do the check once per batch, not
      //  // once per write.
      //  thread->stats.ResetLastOpTime();
      //}

      for (int64_t j = 0; j < entries_per_batch_; j++) {
        if (write_mode == UNIQUE_RANDOM && num_written == num_ - 1) {
          break;
        }

        int64_t rand_num = key_gens[id]->Next();
        GenerateKeyFromInt(rand_num, FLAGS_num, &key);
        if (FLAGS_num_column_families <= 1) {
          batch->Put(key, gen.Generate(value_size_));
        } else {
          // We use same rand_num as seed for key and column family so that we
          // can deterministically find the cfh corresponding to a particular
          // key while reading the key.
          batch->Put(db_with_cfh->GetCfh(rand_num), key,
                     gen.Generate(value_size_));
        }
        bytes += value_size_ + key_size_;
        ++num_written;
      }
      // s = db_with_cfh->db->Write(write_options_, &batch);
      BenchMarkAsyncCallback* call_back = new BenchMarkAsyncCallback(batch);
      AsyncCallback* cb_param = nullptr;
      if (FLAGS_async_mode) {
        write_options_.async_commit = true;
        cb_param = call_back;
      }
      s = db_with_cfh->db->WriteAsync(write_options_, batch, cb_param);
      if (!FLAGS_async_mode) {
        call_back->run_call_back();
        delete call_back;
      }
      // delete allocate_option;
      thread->stats.FinishedOps(db_with_cfh, db_with_cfh->db,
                                entries_per_batch_, kWrite);
      if (!s.ok()) {
        fprintf(stderr, "put error: %s\n", s.ToString().c_str());
        exit(1);
      }
      batch = nullptr;
    }
    thread->stats.AddBytes(bytes);
    if (FLAGS_perf_level > PerfLevel::kDisable) {
      // TODO @zhencheng : use QUERY TRACE to print this.
      // thread->stats.AddMessage(perf_context.ToString());
    }
  }

  Status DoDeterministicCompact(ThreadState* thread,
                                CompactionStyle compaction_style,
                                WriteMode write_mode) {
#ifndef ROCKSDB_LITE
    ColumnFamilyMetaData meta;
    std::vector<DB*> db_list;
    if (db_.db != nullptr) {
      db_list.push_back(db_.db);
    } else {
      for (auto& db : multi_dbs_) {
        db_list.push_back(db.db);
      }
    }
    std::vector<Options> options_list;
    for (auto db : db_list) {
      options_list.push_back(db->GetOptions());
      if (compaction_style != kCompactionStyleFIFO) {
        db->SetOptions({{"disable_auto_compactions", "1"},
                        {"level0_slowdown_writes_trigger", "400000000"},
                        {"level0_stop_writes_trigger", "400000000"}});
      } else {
        db->SetOptions({{"disable_auto_compactions", "1"}});
      }
    }

    assert(!db_list.empty());
    auto num_db = db_list.size();
    size_t num_levels = static_cast<size_t>(open_options_.num_levels);
    size_t output_level = open_options_.num_levels - 1;
    std::vector<std::vector<std::vector<SstFileMetaData>>> sorted_runs(num_db);
    std::vector<size_t> num_files_at_level0(num_db, 0);
    if (compaction_style == kCompactionStyleLevel) {
      if (num_levels == 0) {
        return Status::InvalidArgument("num_levels should be larger than 1");
      }
      bool should_stop = false;
      while (!should_stop) {
        if (sorted_runs[0].empty()) {
          DoWrite(thread, write_mode);
        } else {
          DoWrite(thread, UNIQUE_RANDOM);
        }
        for (size_t i = 0; i < num_db; i++) {
          auto db = db_list[i];
          db->Flush(FlushOptions());
          db->GetColumnFamilyMetaData(&meta);
          if (num_files_at_level0[i] == meta.levels[0].files.size() ||
              writes_ == 0) {
            should_stop = true;
            continue;
          }
          sorted_runs[i].emplace_back(
              meta.levels[0].files.begin(),
              meta.levels[0].files.end() - num_files_at_level0[i]);
          num_files_at_level0[i] = meta.levels[0].files.size();
          if (sorted_runs[i].back().size() == 1) {
            should_stop = true;
            continue;
          }
          if (sorted_runs[i].size() == output_level) {
            auto& L1 = sorted_runs[i].back();
            L1.erase(L1.begin(), L1.begin() + L1.size() / 3);
            should_stop = true;
            continue;
          }
        }
        writes_ /=
            static_cast<int64_t>(open_options_.max_bytes_for_level_multiplier);
      }
      for (size_t i = 0; i < num_db; i++) {
        if (sorted_runs[i].size() < num_levels - 1) {
          fprintf(stderr, "n is too small to fill %" ROCKSDB_PRIszt " levels\n",
                  num_levels);
          exit(1);
        }
      }
      for (size_t i = 0; i < num_db; i++) {
        auto db = db_list[i];
        auto compactionOptions = CompactionOptions();
        auto options = db->GetOptions();
        MutableCFOptions mutable_cf_options(options);
        for (size_t j = 0; j < sorted_runs[i].size(); j++) {
          compactionOptions.output_file_size_limit =
              mutable_cf_options.MaxFileSizeForLevel(
                  static_cast<int>(output_level));
          std::cout << sorted_runs[i][j].size() << std::endl;
          db->CompactFiles(compactionOptions, {sorted_runs[i][j].back().name,
                                               sorted_runs[i][j].front().name},
                           static_cast<int>(output_level - j) /*level*/);
        }
      }
    } else if (compaction_style == kCompactionStyleUniversal) {
      auto ratio = open_options_.compaction_options_universal.size_ratio;
      bool should_stop = false;
      while (!should_stop) {
        if (sorted_runs[0].empty()) {
          DoWrite(thread, write_mode);
        } else {
          DoWrite(thread, UNIQUE_RANDOM);
        }
        for (size_t i = 0; i < num_db; i++) {
          auto db = db_list[i];
          db->Flush(FlushOptions());
          db->GetColumnFamilyMetaData(&meta);
          if (num_files_at_level0[i] == meta.levels[0].files.size() ||
              writes_ == 0) {
            should_stop = true;
            continue;
          }
          sorted_runs[i].emplace_back(
              meta.levels[0].files.begin(),
              meta.levels[0].files.end() - num_files_at_level0[i]);
          num_files_at_level0[i] = meta.levels[0].files.size();
          if (sorted_runs[i].back().size() == 1) {
            should_stop = true;
            continue;
          }
          num_files_at_level0[i] = meta.levels[0].files.size();
        }
        writes_ = static_cast<int64_t>(writes_ * static_cast<double>(100) /
                                       (ratio + 200));
      }
      for (size_t i = 0; i < num_db; i++) {
        if (sorted_runs[i].size() < num_levels) {
          fprintf(stderr, "n is too small to fill %" ROCKSDB_PRIszt " levels\n",
                  num_levels);
          exit(1);
        }
      }
      for (size_t i = 0; i < num_db; i++) {
        auto db = db_list[i];
        auto compactionOptions = CompactionOptions();
        auto options = db->GetOptions();
        MutableCFOptions mutable_cf_options(options);
        for (size_t j = 0; j < sorted_runs[i].size(); j++) {
          compactionOptions.output_file_size_limit =
              mutable_cf_options.MaxFileSizeForLevel(
                  static_cast<int>(output_level));
          db->CompactFiles(
              compactionOptions,
              {sorted_runs[i][j].back().name, sorted_runs[i][j].front().name},
              (output_level > j ? static_cast<int>(output_level - j)
                                : 0) /*level*/);
        }
      }
    } else if (compaction_style == kCompactionStyleFIFO) {
      if (num_levels != 1) {
        return Status::InvalidArgument(
            "num_levels should be 1 for FIFO compaction");
      }
      if (FLAGS_num_multi_db != 0) {
        return Status::InvalidArgument("Doesn't support multiDB");
      }
      auto db = db_list[0];
      std::vector<std::string> file_names;
      while (true) {
        if (sorted_runs[0].empty()) {
          DoWrite(thread, write_mode);
        } else {
          DoWrite(thread, UNIQUE_RANDOM);
        }
        db->Flush(FlushOptions());
        db->GetColumnFamilyMetaData(&meta);
        auto total_size = meta.levels[0].size;
        if (total_size >=
            db->GetOptions().compaction_options_fifo.max_table_files_size) {
          for (auto file_meta : meta.levels[0].files) {
            file_names.emplace_back(file_meta.name);
          }
          break;
        }
      }
      // TODO(shuzhang1989): Investigate why CompactFiles not working
      // auto compactionOptions = CompactionOptions();
      // db->CompactFiles(compactionOptions, file_names, 0);
      auto compactionOptions = CompactRangeOptions();
      db->CompactRange(compactionOptions, nullptr, nullptr);
    } else {
      fprintf(stdout,
              "%-12s : skipped (-compaction_stype=kCompactionStyleNone)\n",
              "filldeterministic");
      return Status::InvalidArgument("None compaction is not supported");
    }

// Verify seqno and key range
// Note: the seqno get changed at the max level by implementation
// optimization, so skip the check of the max level.
#ifndef NDEBUG
    for (size_t k = 0; k < num_db; k++) {
      auto db = db_list[k];
      db->GetColumnFamilyMetaData(&meta);
      // verify the number of sorted runs
      if (compaction_style == kCompactionStyleLevel) {
        assert(num_levels - 1 == sorted_runs[k].size());
      } else if (compaction_style == kCompactionStyleUniversal) {
        assert(meta.levels[0].files.size() + num_levels - 1 ==
               sorted_runs[k].size());
      } else if (compaction_style == kCompactionStyleFIFO) {
        // TODO(gzh): FIFO compaction
        db->GetColumnFamilyMetaData(&meta);
        auto total_size = meta.levels[0].size;
        assert(total_size <=
               db->GetOptions().compaction_options_fifo.max_table_files_size);
        break;
      }

      // verify smallest/largest seqno and key range of each sorted run
      auto max_level = num_levels - 1;
      int level;
      for (size_t i = 0; i < sorted_runs[k].size(); i++) {
        level = static_cast<int>(max_level - i);
        SequenceNumber sorted_run_smallest_seqno = kMaxSequenceNumber;
        SequenceNumber sorted_run_largest_seqno = 0;
        std::string sorted_run_smallest_key, sorted_run_largest_key;
        bool first_key = true;
        for (auto fileMeta : sorted_runs[k][i]) {
          sorted_run_smallest_seqno =
              std::min(sorted_run_smallest_seqno, fileMeta.smallest_seqno);
          sorted_run_largest_seqno =
              std::max(sorted_run_largest_seqno, fileMeta.largest_seqno);
          if (first_key ||
              db->DefaultColumnFamily()->GetComparator()->Compare(
                  fileMeta.smallestkey, sorted_run_smallest_key) < 0) {
            sorted_run_smallest_key = fileMeta.smallestkey;
          }
          if (first_key ||
              db->DefaultColumnFamily()->GetComparator()->Compare(
                  fileMeta.largestkey, sorted_run_largest_key) > 0) {
            sorted_run_largest_key = fileMeta.largestkey;
          }
          first_key = false;
        }
        if (compaction_style == kCompactionStyleLevel ||
            (compaction_style == kCompactionStyleUniversal && level > 0)) {
          SequenceNumber level_smallest_seqno = kMaxSequenceNumber;
          SequenceNumber level_largest_seqno = 0;
          for (auto fileMeta : meta.levels[level].files) {
            level_smallest_seqno =
                std::min(level_smallest_seqno, fileMeta.smallest_seqno);
            level_largest_seqno =
                std::max(level_largest_seqno, fileMeta.largest_seqno);
          }
          assert(sorted_run_smallest_key ==
                 meta.levels[level].files.front().smallestkey);
          assert(sorted_run_largest_key ==
                 meta.levels[level].files.back().largestkey);
          if (level != static_cast<int>(max_level)) {
            // compaction at max_level would change sequence number
            assert(sorted_run_smallest_seqno == level_smallest_seqno);
            assert(sorted_run_largest_seqno == level_largest_seqno);
          }
        } else if (compaction_style == kCompactionStyleUniversal) {
          // level <= 0 means sorted runs on level 0
          auto level0_file =
              meta.levels[0].files[sorted_runs[k].size() - 1 - i];
          assert(sorted_run_smallest_key == level0_file.smallestkey);
          assert(sorted_run_largest_key == level0_file.largestkey);
          if (level != static_cast<int>(max_level)) {
            assert(sorted_run_smallest_seqno == level0_file.smallest_seqno);
            assert(sorted_run_largest_seqno == level0_file.largest_seqno);
          }
        }
      }
    }
#endif
    // print the size of each sorted_run
    for (size_t k = 0; k < num_db; k++) {
      auto db = db_list[k];
      fprintf(stdout, "---------------------- DB %" ROCKSDB_PRIszt
                      " LSM ---------------------\n",
              k);
      db->GetColumnFamilyMetaData(&meta);
      for (auto& levelMeta : meta.levels) {
        if (levelMeta.files.empty()) {
          continue;
        }
        if (levelMeta.level == 0) {
          for (auto& fileMeta : levelMeta.files) {
            fprintf(stdout, "Level[%d]: %s(size: %" PRIu64 " bytes)\n",
                    levelMeta.level, fileMeta.name.c_str(), fileMeta.size);
          }
        } else {
          fprintf(stdout, "Level[%d]: %s - %s(total size: %" PRIi64 " bytes)\n",
                  levelMeta.level, levelMeta.files.front().name.c_str(),
                  levelMeta.files.back().name.c_str(), levelMeta.size);
        }
      }
    }
    for (size_t i = 0; i < num_db; i++) {
      db_list[i]->SetOptions(
          {{"disable_auto_compactions",
            std::to_string(options_list[i].disable_auto_compactions)},
           {"level0_slowdown_writes_trigger",
            std::to_string(options_list[i].level0_slowdown_writes_trigger)},
           {"level0_stop_writes_trigger",
            std::to_string(options_list[i].level0_stop_writes_trigger)}});
    }
    return Status::OK();
#else
    fprintf(stderr, "SE Lite doesn't support filldeterministic\n");
    return Status::NotSupported(
        "SE Lite doesn't support filldeterministic");
#endif  // ROCKSDB_LITE
  }

  void ReadSequential(ThreadState* thread) {
    if (db_.db != nullptr) {
      ReadSequential(thread, db_.db);
    } else {
      for (const auto& db_with_cfh : multi_dbs_) {
        ReadSequential(thread, db_with_cfh.db);
      }
    }
  }

  void ReadSequential(ThreadState* thread, DB* db) {
    ReadOptions options(FLAGS_verify_checksum, true);

    Iterator* iter = db->NewIterator(options);
    int64_t i = 0;
    int64_t bytes = 0;
    for (iter->SeekToFirst(); i < reads_ && iter->Valid(); iter->Next()) {
      bytes += iter->key().size() + iter->value().size();
      thread->stats.FinishedOps(nullptr, db, 1, kRead);
      ++i;

      if (thread->shared->read_rate_limiter.get() != nullptr &&
          i % 1024 == 1023) {
        thread->shared->read_rate_limiter->Request(1024, Env::IO_HIGH,
                                                   nullptr /* stats */);
      }
    }

    delete iter;
    thread->stats.AddBytes(bytes);
    if (FLAGS_perf_level > PerfLevel::kDisable) {
      // TODO @zhencheng : use QUERY TRACE to print this.
      // thread->stats.AddMessage(perf_context.ToString());
    }
  }

  void ReadReverse(ThreadState* thread) {
    if (db_.db != nullptr) {
      ReadReverse(thread, db_.db);
    } else {
      for (const auto& db_with_cfh : multi_dbs_) {
        ReadReverse(thread, db_with_cfh.db);
      }
    }
  }

  void ReadReverse(ThreadState* thread, DB* db) {
    Iterator* iter = db->NewIterator(ReadOptions(FLAGS_verify_checksum, true));
    int64_t i = 0;
    int64_t bytes = 0;
    for (iter->SeekToLast(); i < reads_ && iter->Valid(); iter->Prev()) {
      bytes += iter->key().size() + iter->value().size();
      thread->stats.FinishedOps(nullptr, db, 1, kRead);
      ++i;
      if (thread->shared->read_rate_limiter.get() != nullptr &&
          i % 1024 == 1023) {
        thread->shared->read_rate_limiter->Request(1024, Env::IO_HIGH,
                                                   nullptr /* stats */);
      }
    }
    delete iter;
    thread->stats.AddBytes(bytes);
  }

  void ReadRandomFast(ThreadState* thread) {
    int64_t read = 0;
    int64_t found = 0;
    int64_t nonexist = 0;
    ReadOptions options(FLAGS_verify_checksum, true);
    std::unique_ptr<const char[]> key_guard;
    Slice key = AllocateKey(&key_guard);
    std::string value;
    DB* db = SelectDBWithCfh(thread)->db;

    int64_t pot = 1;
    while (pot < FLAGS_num) {
      pot <<= 1;
    }

    Duration duration(FLAGS_duration, reads_);
    do {
      for (int i = 0; i < 100; ++i) {
        int64_t key_rand = thread->rand.Next() & (pot - 1);
        GenerateKeyFromInt(key_rand, FLAGS_num, &key);
        ++read;
        auto status = db->Get(options, key, &value);
        if (status.ok()) {
          ++found;
        } else if (!status.IsNotFound()) {
          fprintf(stderr, "Get returned an error: %s\n",
                  status.ToString().c_str());
          abort();
        }
        if (key_rand >= FLAGS_num) {
          ++nonexist;
        }
      }
      if (thread->shared->read_rate_limiter.get() != nullptr) {
        thread->shared->read_rate_limiter->Request(100, Env::IO_HIGH,
                                                   nullptr /* stats */);
      }

      thread->stats.FinishedOps(nullptr, db, 100, kRead);
    } while (!duration.Done(100));

    char msg[100];
    snprintf(msg, sizeof(msg), "(%" PRIu64 " of %" PRIu64
                               " found, "
                               "issued %" PRIu64 " non-exist keys)\n",
             found, read, nonexist);

    thread->stats.AddMessage(msg);

    if (FLAGS_perf_level > PerfLevel::kDisable) {
      // TODO @zhencheng : use QUERY TRACE to print this.
      // thread->stats.AddMessage(perf_context.ToString());
    }
  }

  int64_t GetRandomKey(Random64* rand, int64_t total_num = 0) {
    uint64_t rand_int = rand->Next();
    int64_t key_rand;
    if (0 == total_num) total_num = FLAGS_num;
    if (read_random_exp_range_ == 0) {
      key_rand = rand_int % total_num;
    } else {
      const uint64_t kBigInt = static_cast<uint64_t>(1U) << 62;
      long double order = -static_cast<long double>(rand_int % kBigInt) /
                          static_cast<long double>(kBigInt) *
                          read_random_exp_range_;
      long double exp_ran = std::exp(order);
      uint64_t rand_num =
          static_cast<int64_t>(exp_ran * static_cast<long double>(total_num));
      // Map to a different number to avoid locality.
      const uint64_t kBigPrime = 0x5bd1e995;
      // Overflow is like %(2^64). Will have little impact of results.
      key_rand = static_cast<int64_t>((rand_num * kBigPrime) % total_num);
    }
    return key_rand;
  }

  void ReadRandom(ThreadState* thread) {
    int64_t read = 0;
    int64_t found = 0;
    int64_t bytes = 0;
    int64_t total_num = (FLAGS_threads - 1) * FLAGS_num;
    ReadOptions options(FLAGS_verify_checksum, true);
    std::unique_ptr<const char[]> key_guard;
    Slice key = AllocateKey(&key_guard);
    std::string value;
    PinnableSlice pinnable_val;

    Duration duration(FLAGS_duration, reads_);
    while (!duration.Done(1)) {
      DBWithColumnFamilies* db_with_cfh = SelectDBWithCfh(thread);
      // We use same key_rand as seed for key and column family so that we can
      // deterministically find the cfh corresponding to a particular key, as it
      // is done in DoWrite method.

      int64_t key_rand = GetRandomKey(&thread->rand, total_num);
      GenerateKeyFromInt(key_rand, total_num, &key);
      read++;
      Status s;
      if (FLAGS_num_column_families > 1) {
        s = db_with_cfh->db->Get(options, db_with_cfh->GetCfh(key_rand), key,
                                 &value);
      } else {
        if (LIKELY(FLAGS_pin_slice == 1)) {
          pinnable_val.Reset();
          s = db_with_cfh->db->Get(options,
                                   db_with_cfh->db->DefaultColumnFamily(), key,
                                   &pinnable_val);
        } else {
          s = db_with_cfh->db->Get(
              options, db_with_cfh->db->DefaultColumnFamily(), key, &value);
        }
      }
      if (s.ok()) {
        found++;
        bytes += key.size() +
                 (FLAGS_pin_slice == 1 ? pinnable_val.size() : value.size());
      } else if (s.IsNotFound()) {
        fprintf(stderr, "Get returned an error: %s\n", s.ToString().c_str());
        abort();
      }

      if (thread->shared->read_rate_limiter.get() != nullptr &&
          read % 256 == 255) {
        thread->shared->read_rate_limiter->Request(256, Env::IO_HIGH,
                                                   nullptr /* stats */);
      }

      thread->stats.FinishedOps(db_with_cfh, db_with_cfh->db, 1, kRead);
    }

    char msg[100];
    snprintf(msg, sizeof(msg), "(%" PRIu64 " of %" PRIu64 " found)\n", found,
             read);
    thread->stats.AddBytes(bytes);
    thread->stats.AddMessage(msg);

    if (FLAGS_perf_level > PerfLevel::kDisable) {
      // TODO @zhencheng : use QUERY TRACE to print this.
      // thread->stats.AddMessage(perf_context.ToString());
    }
  }

  // Calls MultiGet over a list of keys from a random distribution.
  // Returns the total number of keys found.
  void MultiReadRandom(ThreadState* thread) {
    int64_t read = 0;
    int64_t num_multireads = 0;
    int64_t found = 0;
    ReadOptions options(FLAGS_verify_checksum, true);
    std::vector<Slice> keys;
    std::vector<std::unique_ptr<const char[]>> key_guards;
    std::vector<std::string> values(entries_per_batch_);
    while (static_cast<int64_t>(keys.size()) < entries_per_batch_) {
      key_guards.push_back(std::unique_ptr<const char[]>());
      keys.push_back(AllocateKey(&key_guards.back()));
    }

    Duration duration(FLAGS_duration, reads_);
    while (!duration.Done(1)) {
      DB* db = SelectDB(thread);
      for (int64_t i = 0; i < entries_per_batch_; ++i) {
        GenerateKeyFromInt(GetRandomKey(&thread->rand), FLAGS_num, &keys[i]);
      }
      std::vector<Status> statuses = db->MultiGet(options, keys, &values);
      assert(static_cast<int64_t>(statuses.size()) == entries_per_batch_);

      read += entries_per_batch_;
      num_multireads++;
      for (int64_t i = 0; i < entries_per_batch_; ++i) {
        if (statuses[i].ok()) {
          ++found;
        } else if (!statuses[i].IsNotFound()) {
          fprintf(stderr, "MultiGet returned an error: %s\n",
                  statuses[i].ToString().c_str());
          abort();
        }
      }
      if (thread->shared->read_rate_limiter.get() != nullptr &&
          num_multireads % 256 == 255) {
        thread->shared->read_rate_limiter->Request(
            256 * entries_per_batch_, Env::IO_HIGH, nullptr /* stats */);
      }
      thread->stats.FinishedOps(nullptr, db, entries_per_batch_, kRead);
    }

    char msg[100];
    snprintf(msg, sizeof(msg), "(%" PRIu64 " of %" PRIu64 " found)", found,
             read);
    thread->stats.AddMessage(msg);
  }

  void IteratorCreation(ThreadState* thread) {
    Duration duration(FLAGS_duration, reads_);
    ReadOptions options(FLAGS_verify_checksum, true);
    while (!duration.Done(1)) {
      DB* db = SelectDB(thread);
      Iterator* iter = db->NewIterator(options);
      delete iter;
      thread->stats.FinishedOps(nullptr, db, 1, kOthers);
    }
  }

  void IteratorCreationWhileWriting(ThreadState* thread) {
    if (thread->tid > 0) {
      IteratorCreation(thread);
    } else {
      BGWriter(thread, kWrite);
    }
  }

  void SeekRandom(ThreadState* thread) {
    int64_t read = 0;
    int64_t found = 0;
    int64_t bytes = 0;
    ReadOptions options(FLAGS_verify_checksum, true);

    Iterator* single_iter = nullptr;
    std::vector<Iterator*> multi_iters;
    if (db_.db != nullptr) {
      single_iter = db_.db->NewIterator(options);
    } else {
      for (const auto& db_with_cfh : multi_dbs_) {
        multi_iters.push_back(db_with_cfh.db->NewIterator(options));
      }
    }

    std::unique_ptr<const char[]> key_guard;
    Slice key = AllocateKey(&key_guard);

    Duration duration(FLAGS_duration, reads_);
    char value_buffer[256];
    while (!duration.Done(1)) {
      if (db_.db != nullptr) {
        delete single_iter;
        single_iter = db_.db->NewIterator(options);
      } else {
        for (auto iter : multi_iters) {
          delete iter;
        }
        multi_iters.clear();
        for (const auto& db_with_cfh : multi_dbs_) {
          multi_iters.push_back(db_with_cfh.db->NewIterator(options));
        }
      }
      // Pick a Iterator to use
      Iterator* iter_to_use = single_iter;
      if (single_iter == nullptr) {
        iter_to_use = multi_iters[thread->rand.Next() % multi_iters.size()];
      }

      GenerateKeyFromInt(thread->rand.Next() % FLAGS_num, FLAGS_num, &key);
      iter_to_use->Seek(key);
      read++;
      if (iter_to_use->Valid() && iter_to_use->key().compare(key) == 0) {
        found++;
      }

      for (int j = 0; j < FLAGS_seek_nexts && iter_to_use->Valid(); ++j) {
        // Copy out iterator's value to make sure we read them.
        Slice value = iter_to_use->value();
        memcpy(value_buffer, value.data(),
               std::min(value.size(), sizeof(value_buffer)));
        bytes += iter_to_use->key().size() + iter_to_use->value().size();

        if (!FLAGS_reverse_iterator) {
          iter_to_use->Next();
        } else {
          iter_to_use->Prev();
        }
        assert(iter_to_use->status().ok());
      }

      if (thread->shared->read_rate_limiter.get() != nullptr &&
          read % 256 == 255) {
        thread->shared->read_rate_limiter->Request(256, Env::IO_HIGH,
                                                   nullptr /* stats */);
      }

      thread->stats.FinishedOps(&db_, db_.db, 1, kSeek);
    }
    delete single_iter;
    for (auto iter : multi_iters) {
      delete iter;
    }

    char msg[100];
    snprintf(msg, sizeof(msg), "(%" PRIu64 " of %" PRIu64 " found)\n", found,
             read);
    thread->stats.AddBytes(bytes);
    thread->stats.AddMessage(msg);
    if (FLAGS_perf_level > PerfLevel::kDisable) {
      // TODO @zhencheng : use QUERY TRACE to print this.
      // thread->stats.AddMessage(perf_context.ToString());
    }
  }

  void SeekRandomWhileWriting(ThreadState* thread) {
    if (thread->tid > 0) {
      SeekRandom(thread);
    } else {
      BGWriter(thread, kWrite);
    }
  }

  void SeekRandomWhileMerging(ThreadState* thread) {
    if (thread->tid > 0) {
      SeekRandom(thread);
    } else {
      BGWriter(thread, kMerge);
    }
  }

  void DoDelete(ThreadState* thread, bool seq) {
    WriteBatch batch;
    Duration duration(seq ? 0 : FLAGS_duration, deletes_);
    int64_t i = 0;
    std::unique_ptr<const char[]> key_guard;
    Slice key = AllocateKey(&key_guard);

    while (!duration.Done(entries_per_batch_)) {
      DB* db = SelectDB(thread);
      batch.Clear();
      for (int64_t j = 0; j < entries_per_batch_; ++j) {
        const int64_t k = seq ? i + j : (thread->rand.Next() % FLAGS_num);
        GenerateKeyFromInt(k, FLAGS_num, &key);
        batch.Delete(key);
      }
      auto s = db->Write(write_options_, &batch);
      thread->stats.FinishedOps(nullptr, db, entries_per_batch_, kDelete);
      if (!s.ok()) {
        fprintf(stderr, "del error: %s\n", s.ToString().c_str());
        exit(1);
      }
      i += entries_per_batch_;
    }
  }

  void DeleteSeq(ThreadState* thread) { DoDelete(thread, true); }

  void DeleteRandom(ThreadState* thread) { DoDelete(thread, false); }

  void ReadWhileWriting(ThreadState* thread) {
    if (thread->tid > 0) {
      ReadRandom(thread);
    } else {
      BGWriter(thread, kWrite);
    }
  }

  void ReadWhileMerging(ThreadState* thread) {
    if (thread->tid > 0) {
      ReadRandom(thread);
    } else {
      BGWriter(thread, kMerge);
    }
  }

  void BGWriter(ThreadState* thread, enum OperationType write_merge) {
    // Special thread that keeps writing until other threads are done.
    RandomGenerator gen;
    int64_t bytes = 0;

    std::unique_ptr<RateLimiter> write_rate_limiter;
    if (FLAGS_benchmark_write_rate_limit > 0) {
      write_rate_limiter.reset(
          NewGenericRateLimiter(FLAGS_benchmark_write_rate_limit));
    }

    // Don't merge stats from this thread with the readers.
    thread->stats.SetExcludeFromMerge();

    std::unique_ptr<const char[]> key_guard;
    Slice key = AllocateKey(&key_guard);
    uint32_t written = 0;
    bool hint_printed = false;

    while (true) {
      DB* db = SelectDB(thread);
      {
        MutexLock l(&thread->shared->mu);
        if (FLAGS_finish_after_writes && written == writes_) {
          fprintf(stderr, "Exiting the writer after %u writes...\n", written);
          break;
        }
        if (thread->shared->num_done + 1 >= thread->shared->num_initialized) {
          // Other threads have finished
          if (FLAGS_finish_after_writes) {
            // Wait for the writes to be finished
            if (!hint_printed) {
              fprintf(stderr, "Reads are finished. Have %d more writes to do\n",
                      (int)writes_ - written);
              hint_printed = true;
            }
          } else {
            // Finish the write immediately
            break;
          }
        }
      }

      GenerateKeyFromInt(thread->rand.Next() % FLAGS_num, FLAGS_num, &key);
      Status s;

      if (write_merge == kWrite) {
        s = db->Put(write_options_, key, gen.Generate(value_size_));
      } else {
        se_assert(false);
      }
      written++;

      if (!s.ok()) {
        fprintf(stderr, "put or merge error: %s\n", s.ToString().c_str());
        exit(1);
      }
      bytes += key.size() + value_size_;
      thread->stats.FinishedOps(&db_, db_.db, 1, kWrite);

      if (FLAGS_benchmark_write_rate_limit > 0) {
        write_rate_limiter->Request(
            entries_per_batch_ * (value_size_ + key_size_), Env::IO_HIGH,
            nullptr /* stats */);
      }
    }
    thread->stats.AddBytes(bytes);
  }

  // Given a key K and value V, this puts (K+"0", V), (K+"1", V), (K+"2", V)
  // in DB atomically i.e in a single batch. Also refer GetMany.
  Status PutMany(DB* db, const WriteOptions& writeoptions, const Slice& key,
                 const Slice& value) {
    std::string suffixes[3] = {"2", "1", "0"};
    std::string keys[3];

    WriteBatch batch;
    Status s;
    for (int i = 0; i < 3; i++) {
      keys[i] = key.ToString() + suffixes[i];
      batch.Put(keys[i], value);
    }

    s = db->Write(writeoptions, &batch);
    return s;
  }

  // Given a key K, this deletes (K+"0", V), (K+"1", V), (K+"2", V)
  // in DB atomically i.e in a single batch. Also refer GetMany.
  Status DeleteMany(DB* db, const WriteOptions& writeoptions,
                    const Slice& key) {
    std::string suffixes[3] = {"1", "2", "0"};
    std::string keys[3];

    WriteBatch batch;
    Status s;
    for (int i = 0; i < 3; i++) {
      keys[i] = key.ToString() + suffixes[i];
      batch.Delete(keys[i]);
    }

    s = db->Write(writeoptions, &batch);
    return s;
  }

  // Given a key K and value V, this gets values for K+"0", K+"1" and K+"2"
  // in the same snapshot, and verifies that all the values are identical.
  // ASSUMES that PutMany was used to put (K, V) into the DB.
  Status GetMany(DB* db, const ReadOptions& readoptions, const Slice& key,
                 std::string* value) {
    std::string suffixes[3] = {"0", "1", "2"};
    std::string keys[3];
    Slice key_slices[3];
    std::string values[3];
    ReadOptions readoptionscopy = readoptions;
    readoptionscopy.snapshot = db->GetSnapshot();
    Status s;
    for (int i = 0; i < 3; i++) {
      keys[i] = key.ToString() + suffixes[i];
      key_slices[i] = keys[i];
      s = db->Get(readoptionscopy, key_slices[i], value);
      if (!s.ok() && !s.IsNotFound()) {
        fprintf(stderr, "get error: %s\n", s.ToString().c_str());
        values[i] = "";
        // we continue after error rather than exiting so that we can
        // find more errors if any
      } else if (s.IsNotFound()) {
        values[i] = "";
      } else {
        values[i] = *value;
      }
    }
    db->ReleaseSnapshot(readoptionscopy.snapshot);

    if ((values[0] != values[1]) || (values[1] != values[2])) {
      fprintf(stderr, "inconsistent values for key %s: %s, %s, %s\n",
              key.ToString().c_str(), values[0].c_str(), values[1].c_str(),
              values[2].c_str());
      // we continue after error rather than exiting so that we can
      // find more errors if any
    }

    return s;
  }

  // Differs from readrandomwriterandom in the following ways:
  // (a) Uses GetMany/PutMany to read/write key values. Refer to those funcs.
  // (b) Does deletes as well (per FLAGS_deletepercent)
  // (c) In order to achieve high % of 'found' during lookups, and to do
  //     multiple writes (including puts and deletes) it uses upto
  //     FLAGS_numdistinct distinct keys instead of FLAGS_num distinct keys.
  // (d) Does not have a MultiGet option.
  void RandomWithVerify(ThreadState* thread) {
    ReadOptions options(FLAGS_verify_checksum, true);
    RandomGenerator gen;
    std::string value;
    int64_t found = 0;
    int get_weight = 0;
    int put_weight = 0;
    int delete_weight = 0;
    int64_t gets_done = 0;
    int64_t puts_done = 0;
    int64_t deletes_done = 0;

    std::unique_ptr<const char[]> key_guard;
    Slice key = AllocateKey(&key_guard);

    // the number of iterations is the larger of read_ or write_
    for (int64_t i = 0; i < readwrites_; i++) {
      DB* db = SelectDB(thread);
      if (get_weight == 0 && put_weight == 0 && delete_weight == 0) {
        // one batch completed, reinitialize for next batch
        get_weight = FLAGS_readwritepercent;
        delete_weight = FLAGS_deletepercent;
        put_weight = 100 - get_weight - delete_weight;
      }
      GenerateKeyFromInt(thread->rand.Next() % FLAGS_numdistinct,
                         FLAGS_numdistinct, &key);
      if (get_weight > 0) {
        // do all the gets first
        Status s = GetMany(db, options, key, &value);
        if (!s.ok() && !s.IsNotFound()) {
          fprintf(stderr, "getmany error: %s\n", s.ToString().c_str());
          // we continue after error rather than exiting so that we can
          // find more errors if any
        } else if (!s.IsNotFound()) {
          found++;
        }
        get_weight--;
        gets_done++;
        thread->stats.FinishedOps(&db_, db_.db, 1, kRead);
      } else if (put_weight > 0) {
        // then do all the corresponding number of puts
        // for all the gets we have done earlier
        Status s = PutMany(db, write_options_, key, gen.Generate(value_size_));
        if (!s.ok()) {
          fprintf(stderr, "putmany error: %s\n", s.ToString().c_str());
          exit(1);
        }
        put_weight--;
        puts_done++;
        thread->stats.FinishedOps(&db_, db_.db, 1, kWrite);
      } else if (delete_weight > 0) {
        Status s = DeleteMany(db, write_options_, key);
        if (!s.ok()) {
          fprintf(stderr, "deletemany error: %s\n", s.ToString().c_str());
          exit(1);
        }
        delete_weight--;
        deletes_done++;
        thread->stats.FinishedOps(&db_, db_.db, 1, kDelete);
      }
    }
    char msg[130];
    snprintf(msg, sizeof(msg), "( get:%" PRIu64 " put:%" PRIu64 " del:%" PRIu64
                               " total:%" PRIu64 " found:%" PRIu64 ")",
             gets_done, puts_done, deletes_done, readwrites_, found);
    thread->stats.AddMessage(msg);
  }

  // This is different from ReadWhileWriting because it does not use
  // an extra thread.
  void ReadRandomWriteRandom(ThreadState* thread) {
    ReadOptions options(FLAGS_verify_checksum, true);
    RandomGenerator gen;
    std::string value;
    int64_t found = 0;
    int get_weight = 0;
    int put_weight = 0;
    int64_t reads_done = 0;
    int64_t writes_done = 0;
    Duration duration(FLAGS_duration, readwrites_);

    std::unique_ptr<const char[]> key_guard;
    Slice key = AllocateKey(&key_guard);

    // the number of iterations is the larger of read_ or write_
    while (!duration.Done(1)) {
      DB* db = SelectDB(thread);
      GenerateKeyFromInt(thread->rand.Next() % FLAGS_num, FLAGS_num, &key);
      if (get_weight == 0 && put_weight == 0) {
        // one batch completed, reinitialize for next batch
        get_weight = FLAGS_readwritepercent;
        put_weight = 100 - get_weight;
      }
      if (get_weight > 0) {
        // do all the gets first
        Status s = db->Get(options, key, &value);
        if (!s.ok() && !s.IsNotFound()) {
          fprintf(stderr, "get error: %s\n", s.ToString().c_str());
          // we continue after error rather than exiting so that we can
          // find more errors if any
        } else if (!s.IsNotFound()) {
          found++;
        }
        get_weight--;
        reads_done++;
        thread->stats.FinishedOps(nullptr, db, 1, kRead);
      } else if (put_weight > 0) {
        // then do all the corresponding number of puts
        // for all the gets we have done earlier
        Status s = db->Put(write_options_, key, gen.Generate(value_size_));
        if (!s.ok()) {
          fprintf(stderr, "put error: %s\n", s.ToString().c_str());
          exit(1);
        }
        put_weight--;
        writes_done++;
        thread->stats.FinishedOps(nullptr, db, 1, kWrite);
      }
    }
    char msg[100];
    snprintf(msg, sizeof(msg), "( reads:%" PRIu64 " writes:%" PRIu64
                               " total:%" PRIu64 " found:%" PRIu64 ")",
             reads_done, writes_done, readwrites_, found);
    thread->stats.AddMessage(msg);
  }

  //
  // Read-modify-write for random keys
  void UpdateRandom(ThreadState* thread) {
    ReadOptions options(FLAGS_verify_checksum, true);
    RandomGenerator gen;
    std::string value;
    int64_t found = 0;
    int64_t bytes = 0;
    Duration duration(FLAGS_duration, readwrites_);

    std::unique_ptr<const char[]> key_guard;
    Slice key = AllocateKey(&key_guard);
    // the number of iterations is the larger of read_ or write_
    while (!duration.Done(1)) {
      DB* db = SelectDB(thread);
      GenerateKeyFromInt(thread->rand.Next() % FLAGS_num, FLAGS_num, &key);

      auto status = db->Get(options, key, &value);
      if (status.ok()) {
        ++found;
        bytes += key.size() + value.size();
      } else if (!status.IsNotFound()) {
        fprintf(stderr, "Get returned an error: %s\n",
                status.ToString().c_str());
        abort();
      }

      Status s = db->Put(write_options_, key, gen.Generate(value_size_));
      if (!s.ok()) {
        fprintf(stderr, "put error: %s\n", s.ToString().c_str());
        exit(1);
      }
      bytes += key.size() + value_size_;
      thread->stats.FinishedOps(nullptr, db, 1, kUpdate);
    }
    char msg[100];
    snprintf(msg, sizeof(msg), "( updates:%" PRIu64 " found:%" PRIu64 ")",
             readwrites_, found);
    thread->stats.AddBytes(bytes);
    thread->stats.AddMessage(msg);
  }

  // Read-modify-write for random keys.
  // Each operation causes the key grow by value_size (simulating an append).
  // Generally used for benchmarking against merges of similar type
  void AppendRandom(ThreadState* thread) {
    ReadOptions options(FLAGS_verify_checksum, true);
    RandomGenerator gen;
    std::string value;
    int64_t found = 0;
    int64_t bytes = 0;

    std::unique_ptr<const char[]> key_guard;
    Slice key = AllocateKey(&key_guard);
    // The number of iterations is the larger of read_ or write_
    Duration duration(FLAGS_duration, readwrites_);
    while (!duration.Done(1)) {
      DB* db = SelectDB(thread);
      GenerateKeyFromInt(thread->rand.Next() % FLAGS_num, FLAGS_num, &key);

      auto status = db->Get(options, key, &value);
      if (status.ok()) {
        ++found;
        bytes += key.size() + value.size();
      } else if (!status.IsNotFound()) {
        fprintf(stderr, "Get returned an error: %s\n",
                status.ToString().c_str());
        abort();
      } else {
        // If not existing, then just assume an empty string of data
        value.clear();
      }

      // Update the value (by appending data)
      Slice operand = gen.Generate(value_size_);
      if (value.size() > 0) {
        // Use a delimiter to match the semantics for StringAppendOperator
        value.append(1, ',');
      }
      value.append(operand.data(), operand.size());

      // Write back to the database
      Status s = db->Put(write_options_, key, value);
      if (!s.ok()) {
        fprintf(stderr, "put error: %s\n", s.ToString().c_str());
        exit(1);
      }
      bytes += key.size() + value.size();
      thread->stats.FinishedOps(nullptr, db, 1, kUpdate);
    }

    char msg[100];
    snprintf(msg, sizeof(msg), "( updates:%" PRIu64 " found:%" PRIu64 ")",
             readwrites_, found);
    thread->stats.AddBytes(bytes);
    thread->stats.AddMessage(msg);
  }

  void WriteSeqSeekSeq(ThreadState* thread) {
    writes_ = FLAGS_num;
    DoWrite(thread, SEQUENTIAL);
    // exclude writes from the ops/sec calculation
    thread->stats.Start(thread->tid);

    DB* db = SelectDB(thread);
    std::unique_ptr<db::Iterator> iter(
        db->NewIterator(ReadOptions(FLAGS_verify_checksum, true)));

    std::unique_ptr<const char[]> key_guard;
    Slice key = AllocateKey(&key_guard);
    for (int64_t i = 0; i < FLAGS_num; ++i) {
      GenerateKeyFromInt(i, FLAGS_num, &key);
      iter->Seek(key);
      assert(iter->Valid() && iter->key() == key);
      thread->stats.FinishedOps(nullptr, db, 1, kSeek);

      for (int j = 0; j < FLAGS_seek_nexts && i + 1 < FLAGS_num; ++j) {
        if (!FLAGS_reverse_iterator) {
          iter->Next();
        } else {
          iter->Prev();
        }
        GenerateKeyFromInt(++i, FLAGS_num, &key);
        assert(iter->Valid() && iter->key() == key);
        thread->stats.FinishedOps(nullptr, db, 1, kSeek);
      }

      iter->Seek(key);
      assert(iter->Valid() && iter->key() == key);
      thread->stats.FinishedOps(nullptr, db, 1, kSeek);
    }
  }

#ifndef ROCKSDB_LITE
  // This benchmark stress tests Transactions.  For a given --duration (or
  // total number of --writes, a Transaction will perform a read-modify-write
  // to increment the value of a key in each of N(--transaction-sets) sets of
  // keys (where each set has --num keys).  If --threads is set, this will be
  // done in parallel.
  //
  // To test transactions, use --transaction_db=true.  Not setting this
  // parameter
  // will run the same benchmark without transactions.
  //
  // RandomTransactionVerify() will then validate the correctness of the results
  // by checking if the sum of all keys in each set is the same.
  void RandomTransaction(ThreadState* thread) {
    ReadOptions options(FLAGS_verify_checksum, true);
    Duration duration(FLAGS_duration, readwrites_);
    ReadOptions read_options(FLAGS_verify_checksum, true);
    uint16_t num_prefix_ranges = static_cast<uint16_t>(FLAGS_transaction_sets);
    uint64_t transactions_done = 0;

    if (num_prefix_ranges == 0 || num_prefix_ranges > 9999) {
      fprintf(stderr, "invalid value for transaction_sets\n");
      abort();
    }

    TransactionOptions txn_options;
    txn_options.lock_timeout = FLAGS_transaction_lock_timeout;
    txn_options.set_snapshot = FLAGS_transaction_set_snapshot;

    RandomTransactionInserter inserter(&thread->rand, write_options_,
                                       read_options, FLAGS_num,
                                       num_prefix_ranges);

    if (FLAGS_num_multi_db > 1) {
      fprintf(stderr,
              "Cannot run RandomTransaction benchmark with "
              "FLAGS_multi_db > 1.");
      abort();
    }

    while (!duration.Done(1)) {
      bool success;

      // RandomTransactionInserter will attempt to insert a key for each
      // # of FLAGS_transaction_sets
      if (FLAGS_optimistic_transaction_db) {
        success = inserter.OptimisticTransactionDBInsert(db_.opt_txn_db);
      } else if (FLAGS_transaction_db) {
        TransactionDB* txn_db = reinterpret_cast<TransactionDB*>(db_.db);
        success = inserter.TransactionDBInsert(txn_db, txn_options);
      } else {
        success = inserter.DBInsert(db_.db);
      }

      if (!success) {
        fprintf(stderr, "Unexpected error: %s\n",
                inserter.GetLastStatus().ToString().c_str());
        abort();
      }

      thread->stats.FinishedOps(nullptr, db_.db, 1, kOthers);
      transactions_done++;
    }

    char msg[100];
    if (FLAGS_optimistic_transaction_db || FLAGS_transaction_db) {
      snprintf(msg, sizeof(msg),
               "( transactions:%" PRIu64 " aborts:%" PRIu64 ")",
               transactions_done, inserter.GetFailureCount());
    } else {
      snprintf(msg, sizeof(msg), "( batches:%" PRIu64 " )", transactions_done);
    }
    thread->stats.AddMessage(msg);

    if (FLAGS_perf_level > PerfLevel::kDisable) {
      // TODO @zhencheng : use QUERY TRACE to print this.
      // thread->stats.AddMessage(perf_context.ToString());
    }
  }

  // Verifies consistency of data after RandomTransaction() has been run.
  // Since each iteration of RandomTransaction() incremented a key in each set
  // by the same value, the sum of the keys in each set should be the same.
  void RandomTransactionVerify() {
    if (!FLAGS_transaction_db && !FLAGS_optimistic_transaction_db) {
      // transactions not used, nothing to verify.
      return;
    }

    Status s = RandomTransactionInserter::Verify(
        db_.db, static_cast<uint16_t>(FLAGS_transaction_sets));

    if (s.ok()) {
      fprintf(stdout, "RandomTransactionVerify Success.\n");
    } else {
      fprintf(stdout, "RandomTransactionVerify FAILED!!\n");
    }
  }
#endif  // ROCKSDB_LITE

  // Writes and deletes random keys without overwriting keys.
  //
  // This benchmark is intended to partially replicate the behavior of MyRocks
  // secondary indices: All data is stored in keys and updates happen by
  // deleting the old version of the key and inserting the new version.
  void RandomReplaceKeys(ThreadState* thread) {
    std::unique_ptr<const char[]> key_guard;
    Slice key = AllocateKey(&key_guard);
    std::vector<uint32_t> counters(FLAGS_numdistinct, 0);
    size_t max_counter = 50;
    RandomGenerator gen;

    Status s;
    DB* db = SelectDB(thread);
    for (int64_t i = 0; i < FLAGS_numdistinct; i++) {
      GenerateKeyFromInt(i * max_counter, FLAGS_num, &key);
      s = db->Put(write_options_, key, gen.Generate(value_size_));
      if (!s.ok()) {
        fprintf(stderr, "Operation failed: %s\n", s.ToString().c_str());
        exit(1);
      }
    }

    db->GetSnapshot();

    std::default_random_engine generator;
    std::normal_distribution<double> distribution(FLAGS_numdistinct / 2.0,
                                                  FLAGS_stddev);
    Duration duration(FLAGS_duration, FLAGS_num);
    while (!duration.Done(1)) {
      int64_t rnd_id = static_cast<int64_t>(distribution(generator));
      int64_t key_id = std::max(std::min(FLAGS_numdistinct - 1, rnd_id),
                                static_cast<int64_t>(0));
      GenerateKeyFromInt(key_id * max_counter + counters[key_id], FLAGS_num,
                         &key);
      s = FLAGS_use_single_deletes ? db->SingleDelete(write_options_, key)
                                   : db->Delete(write_options_, key);
      if (s.ok()) {
        counters[key_id] = (counters[key_id] + 1) % max_counter;
        GenerateKeyFromInt(key_id * max_counter + counters[key_id], FLAGS_num,
                           &key);
        s = db->Put(write_options_, key, Slice());
      }

      if (!s.ok()) {
        fprintf(stderr, "Operation failed: %s\n", s.ToString().c_str());
        exit(1);
      }

      thread->stats.FinishedOps(nullptr, db, 1, kOthers);
    }

    char msg[200];
    snprintf(msg, sizeof(msg),
             "use single deletes: %d, "
             "standard deviation: %lf\n",
             FLAGS_use_single_deletes, FLAGS_stddev);
    thread->stats.AddMessage(msg);
  }

  void TimeSeriesReadOrDelete(ThreadState* thread, bool do_deletion) {
    ReadOptions options(FLAGS_verify_checksum, true);
    int64_t read = 0;
    int64_t found = 0;
    int64_t bytes = 0;

    Iterator* iter = nullptr;
    // Only work on single database
    assert(db_.db != nullptr);
    iter = db_.db->NewIterator(options);

    std::unique_ptr<const char[]> key_guard;
    Slice key = AllocateKey(&key_guard);

    char value_buffer[256];
    while (true) {
      {
        MutexLock l(&thread->shared->mu);
        if (thread->shared->num_done >= 1) {
          // Write thread have finished
          break;
        }
      }
      delete iter;
      iter = db_.db->NewIterator(options);
      // Pick a Iterator to use

      int64_t key_id = thread->rand.Next() % FLAGS_key_id_range;
      GenerateKeyFromInt(key_id, FLAGS_num, &key);
      // Reset last 8 bytes to 0
      char* start = const_cast<char*>(key.data());
      start += key.size() - 8;
      memset(start, 0, 8);
      ++read;

      bool key_found = false;
      // Seek the prefix
      for (iter->Seek(key); iter->Valid() && iter->key().starts_with(key);
           iter->Next()) {
        key_found = true;
        // Copy out iterator's value to make sure we read them.
        if (do_deletion) {
          bytes += iter->key().size();
          if (KeyExpired(timestamp_emulator_.get(), iter->key())) {
            thread->stats.FinishedOps(&db_, db_.db, 1, kDelete);
            db_.db->Delete(write_options_, iter->key());
          } else {
            break;
          }
        } else {
          bytes += iter->key().size() + iter->value().size();
          thread->stats.FinishedOps(&db_, db_.db, 1, kRead);
          Slice value = iter->value();
          memcpy(value_buffer, value.data(),
                 std::min(value.size(), sizeof(value_buffer)));

          assert(iter->status().ok());
        }
      }
      found += key_found;

      if (thread->shared->read_rate_limiter.get() != nullptr) {
        thread->shared->read_rate_limiter->Request(1, Env::IO_HIGH,
                                                   nullptr /* stats */);
      }
    }
    delete iter;

    char msg[100];
    snprintf(msg, sizeof(msg), "(%" PRIu64 " of %" PRIu64 " found)", found,
             read);
    thread->stats.AddBytes(bytes);
    thread->stats.AddMessage(msg);
    if (FLAGS_perf_level > PerfLevel::kDisable) {
      // TODO @zhencheng : use QUERY TRACE to print this.
      // thread->stats.AddMessage(perf_context.ToString());
    }
  }

  void TimeSeriesWrite(ThreadState* thread) {
    // Special thread that keeps writing until other threads are done.
    RandomGenerator gen;
    int64_t bytes = 0;

    // Don't merge stats from this thread with the readers.
    thread->stats.SetExcludeFromMerge();

    std::unique_ptr<RateLimiter> write_rate_limiter;
    if (FLAGS_benchmark_write_rate_limit > 0) {
      write_rate_limiter.reset(
          NewGenericRateLimiter(FLAGS_benchmark_write_rate_limit));
    }

    std::unique_ptr<const char[]> key_guard;
    Slice key = AllocateKey(&key_guard);

    Duration duration(FLAGS_duration, writes_);
    while (!duration.Done(1)) {
      DB* db = SelectDB(thread);

      uint64_t key_id = thread->rand.Next() % FLAGS_key_id_range;
      // Write key id
      GenerateKeyFromInt(key_id, FLAGS_num, &key);
      // Write timestamp

      char* start = const_cast<char*>(key.data());
      char* pos = start + 8;
      int bytes_to_fill =
          std::min(key_size_ - static_cast<int>(pos - start), 8);
      uint64_t timestamp_value = timestamp_emulator_->Get();
      if (port::kLittleEndian) {
        for (int i = 0; i < bytes_to_fill; ++i) {
          pos[i] = (timestamp_value >> ((bytes_to_fill - i - 1) << 3)) & 0xFF;
        }
      } else {
        memcpy(pos, static_cast<void*>(&timestamp_value), bytes_to_fill);
      }

      timestamp_emulator_->Inc();

      Status s;

      s = db->Put(write_options_, key, gen.Generate(value_size_));

      if (!s.ok()) {
        fprintf(stderr, "put error: %s\n", s.ToString().c_str());
        exit(1);
      }
      bytes = key.size() + value_size_;
      thread->stats.FinishedOps(&db_, db_.db, 1, kWrite);
      thread->stats.AddBytes(bytes);

      if (FLAGS_benchmark_write_rate_limit > 0) {
        write_rate_limiter->Request(
            entries_per_batch_ * (value_size_ + key_size_), Env::IO_HIGH,
            nullptr /* stats */);
      }
    }
  }

  void TimeSeries(ThreadState* thread) {
    if (thread->tid > 0) {
      bool do_deletion = FLAGS_expire_style == "delete" &&
                         thread->tid <= FLAGS_num_deletion_threads;
      TimeSeriesReadOrDelete(thread, do_deletion);
    } else {
      TimeSeriesWrite(thread);
      thread->stats.Stop();
      thread->stats.Report("timeseries write");
    }
  }

  void Compact(ThreadState* thread) {
    DB* db = SelectDB(thread);
    db->CompactRange(CompactRangeOptions(), nullptr, nullptr);
  }

  void ResetStats() {
    if (db_.db != nullptr) {
      db_.db->ResetStats();
    }
    for (const auto& db_with_cfh : multi_dbs_) {
      db_with_cfh.db->ResetStats();
    }
  }

  void PrintStats(const char* key) {
    if (db_.db != nullptr) {
      PrintStats(db_.db, key, false);
    }
    for (const auto& db_with_cfh : multi_dbs_) {
      PrintStats(db_with_cfh.db, key, true);
    }
  }

  void PrintStats(DB* db, const char* key, bool print_header = false) {
    if (print_header) {
      fprintf(stdout, "\n==== DB: %s ===\n", db->GetName().c_str());
    }
    std::string stats;
    if (!db->GetProperty(key, &stats)) {
      stats = "(failed)";
    }
    fprintf(stdout, "\n%s\n", stats.c_str());
  }

  ColumnFamilyHandle *stress_get_cfh(int32_t cf_idx) {
    ColumnFamilyHandle *target_cfh = nullptr;
    auto cf_iter = stress_cf_map_.find(cf_idx + 1);
    if (stress_cf_map_.end() != cf_iter) {
      target_cfh = cf_iter->second;
    }
    return target_cfh;
  }

  StressKeyID stress_key_mix(StressKeyID key_id) {
    key_id ^= key_id >> 33;
    key_id *= 0xff51afd7ed558ccd;
    key_id ^= key_id >> 33;
    key_id *= 0xc4ceb9fe1a85ec53;
    key_id ^= key_id >> 33;
    return key_id;
  }

	void stress_log(const char* format_string, ...) {
    if (nullptr != bench_log) {
      va_list args;
      va_start(args, format_string);
      vfprintf(bench_log, format_string, args);
      va_end(args);
    }
  }

  void stress_encode(Slice& s, uint64_t value, char suffix) {
    if (!FLAGS_var_len_stress) {
      static const char alphanum[] =
          "0123456789"
          "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
          "abcdefghijklmnopqrstuvwxyz";

      char* buf = const_cast<char*>(s.data());
      buf[8] = suffix;
      buf[7] = value & 0xff;
      buf[6] = (value >> 8) & 0xff;
      buf[5] = (value >> 16) & 0xff;
      buf[4] = (value >> 24) & 0xff;
      buf[3] = (value >> 32) & 0xff;
      buf[2] = (value >> 40) & 0xff;
      buf[1] = (value >> 48) & 0xff;
      buf[0] = (value >> 56) & 0xff;
      if (s.size() == STRESS_KEY_SIZE) {
        return;
      }
      uint64_t append_size =
          stress_key_mix(value) % (STRESS_FULL_SIZE - STRESS_KEY_SIZE);
      for (uint64_t i = 0; i != append_size; ++i) {
        buf[STRESS_KEY_SIZE + i] = alphanum[(value * i) % (sizeof(alphanum) - 1)];
      }
      s.remove_suffix(s.size() - STRESS_KEY_SIZE - append_size);
    } else {
      char* buf = const_cast<char*>(s.data());
      uint64_t var_len = (value % FLAGS_key_id_group_size) * 2 + suffix;
      value = value / FLAGS_key_id_group_size * FLAGS_key_id_group_size;
      memset(buf + 32, 0, var_len);
      buf[31] = value & 0xff;
      memset(buf + 19, 0, 12);
      buf[18] = (value >> 8) & 0xff;
      memset(buf + 6, 0, 12);
      buf[5] = (value >> 16) & 0xff;
      buf[4] = (value >> 24) & 0xff;
      buf[3] = (value >> 32) & 0xff;
      buf[2] = (value >> 40) & 0xff;
      buf[1] = (value >> 48) & 0xff;
      buf[0] = (value >> 56) & 0xff;
      s.remove_suffix(s.size() - 32 - var_len);
    }
  }

  uint64_t stress_decode(const Slice& data, char& prefix) {
    if (!FLAGS_var_len_stress) {
      const char* buf = data.data();
      prefix = buf[8];
      return (static_cast<uint64_t>(static_cast<unsigned char>(buf[7]))) |
            (static_cast<uint64_t>(static_cast<unsigned char>(buf[6])) << 8) |
            (static_cast<uint64_t>(static_cast<unsigned char>(buf[5])) << 16) |
            (static_cast<uint64_t>(static_cast<unsigned char>(buf[4])) << 24) |
            (static_cast<uint64_t>(static_cast<unsigned char>(buf[3])) << 32) |
            (static_cast<uint64_t>(static_cast<unsigned char>(buf[2])) << 40) |
            (static_cast<uint64_t>(static_cast<unsigned char>(buf[1])) << 48) |
            (static_cast<uint64_t>(static_cast<unsigned char>(buf[0])) << 56);
    } else {
      const char* buf = data.data();
      uint64_t ret = (static_cast<uint64_t>(static_cast<unsigned char>(buf[31]))) |
                    (static_cast<uint64_t>(static_cast<unsigned char>(buf[18])) << 8) |
                    (static_cast<uint64_t>(static_cast<unsigned char>(buf[5])) << 16) |
                    (static_cast<uint64_t>(static_cast<unsigned char>(buf[4])) << 24) |
                    (static_cast<uint64_t>(static_cast<unsigned char>(buf[3])) << 32) |
                    (static_cast<uint64_t>(static_cast<unsigned char>(buf[2])) << 40) |
                    (static_cast<uint64_t>(static_cast<unsigned char>(buf[1])) << 48) |
                    (static_cast<uint64_t>(static_cast<unsigned char>(buf[0])) << 56);
      ret = ret + (data.size() - 32) / 2;
      prefix = data.size() % 2;
      return ret;
    }
  }

  std::string stress_encode_last_key(const int32_t thread_id) {
    char last_key_flag[] = "LAST KEY FOR THREAD: ";
    const size_t THREAD_ID_SIZE = 10; /* at most 10^10-1 threads */
    char last_key[STRESS_KEY_SIZE + sizeof(last_key_flag) + THREAD_ID_SIZE];
    sprintf(last_key, "%s%s%d", THREAD_LAST_KEY_PREFIX.c_str(), last_key_flag,
            thread_id);
    return std::string(last_key);
  }

  void stress_init(bool fresh_db, bool in_memory = false) {
    FLAGS_num_update_threads =
        (FLAGS_num_update_threads + FLAGS_num_column_families - 1) /
        FLAGS_num_column_families * FLAGS_num_column_families;
    FLAGS_num_deletion_threads =
        (FLAGS_num_deletion_threads + FLAGS_num_column_families - 1) /
        FLAGS_num_column_families * FLAGS_num_column_families;
    FLAGS_num_single_delete_threads =
        (FLAGS_num_single_delete_threads + FLAGS_num_column_families - 1) /
        FLAGS_num_column_families * FLAGS_num_column_families;
    FLAGS_threads = (FLAGS_threads + FLAGS_num_column_families - 1) /
                    FLAGS_num_column_families * FLAGS_num_column_families;
    int32_t num_read_threads_per_cf =
        (FLAGS_threads - FLAGS_num_update_threads - FLAGS_num_deletion_threads -
        FLAGS_num_single_delete_threads) / FLAGS_num_column_families;

    if (num_read_threads_per_cf <= 0) {
      StressLockMgr::STRIPES = 32;
      num_read_threads_per_cf = 1;
    } else {
      while (StressLockMgr::STRIPES < 32) {
        StressLockMgr::STRIPES += num_read_threads_per_cf;
      }
    }
    FLAGS_threads = FLAGS_num_update_threads + FLAGS_num_deletion_threads +
                    FLAGS_num_single_delete_threads +
                    num_read_threads_per_cf * FLAGS_num_column_families;

    size_t NUM_LCM = StressLockMgr::STRIPES * StressBitMap::BITS_PER_BYTE;
    FLAGS_num = (FLAGS_num + NUM_LCM - 1) / NUM_LCM * NUM_LCM;
    size_t stress_stripes_per_read_thread =
        StressLockMgr::STRIPES / static_cast<size_t>(num_read_threads_per_cf);

    stress_stripe_dist_ = std::uniform_int_distribution<size_t>(
        0, stress_stripes_per_read_thread - 1);
   
   for (size_t i = 0; i != STRESS_MAX_STAGE; ++i) {
      stress_worker_count_[i] = FLAGS_threads;
    }

    stress_bitmap_.reserve(FLAGS_num_column_families);
    stress_lock_mgr_.reserve(FLAGS_num_column_families);
    bench_log = nullptr;
    std::string bench_log_path = FLAGS_db;
    bench_log_path += "/bench_log.txt";
    if (FLAGS_bench_log) {
      if (fresh_db) {
        bench_log = fopen(bench_log_path.data(), "w");
      } else {
        bench_log = fopen(bench_log_path.data(), "a+");
      }
    }

    stress_start_time = FLAGS_env->NowMicros();

    stress_log("adjust update threads to %d per CF\n"
            "adjust delete threads to %d per CF\n"
            "adjust read threads to %d per CF\n"
            "adjust key range to [0-%ld) per CF\n"
            "%lu lock stripes per CF\n",
            FLAGS_num_update_threads / FLAGS_num_column_families,
            FLAGS_num_deletion_threads / FLAGS_num_column_families,
            num_read_threads_per_cf, FLAGS_num, StressLockMgr::STRIPES);
    
    for (int32_t i = 0; i < FLAGS_num_column_families; ++i) {
      StressBitMap* new_bitmap = nullptr;
      if (in_memory) {
        new_bitmap = new StressMemoryBitMap();
      } else {
        new_bitmap = new StressBitMapInFile();
      }
      if (!new_bitmap->init(FLAGS_num, FLAGS_env, fresh_db, i).ok()) {
        stress_log("stress bitmap init failed\n");
        exit(1);
      }
      StressLockMgr* new_lock_mgr =
          new StressLockMgr((FLAGS_num + NUM_LCM - 1) / NUM_LCM * NUM_LCM);

      stress_bitmap_.emplace_back(new_bitmap);
      stress_lock_mgr_.emplace_back(new_lock_mgr);
    }
    for (auto &cfh : db_.cfh) {
      stress_cf_map_.emplace(cfh->GetID(), cfh);
    }
  }

  void stress_load(bool fresh_db) {
    stress_start_time += 30 * 24 * 3600 * 1e6;
    for (int32_t i = 0; i < FLAGS_num_column_families; ++i) {
      std::unique_ptr<StressBitMapInFile> file_bitmap(new StressBitMapInFile());
      if (!file_bitmap->init(FLAGS_num, FLAGS_env, fresh_db, i).ok()) {
        stress_log("stress bitmap init failed\n");
        exit(1);
      } else {
        StressBitMapSnapshot bitmap_snapshot;
        init_bitmap_snapshot(0, file_bitmap->get_size(), bitmap_snapshot);
        file_bitmap->get_snapshot(bitmap_snapshot);
        stress_bitmap_[i]->cover(bitmap_snapshot);
        stress_log("stress_load, cf_id:%d\n", i);
        // stress_bitmap_[i]->print_to_log(bench_log);
      }
    }
  }

  void stress_dump() {
    for (int32_t i = 0; i < FLAGS_num_column_families; ++i) {
      std::unique_ptr<StressBitMapInFile> file_bitmap(new StressBitMapInFile());
      if (!file_bitmap->init(FLAGS_num, FLAGS_env, false, i).ok()) {
        stress_log("stress bitmap init failed\n");
        exit(1);
      } else {
        StressBitMapSnapshot bitmap_snapshot;
        init_bitmap_snapshot(0, file_bitmap->get_size(), bitmap_snapshot);
        stress_bitmap_[i]->get_snapshot(bitmap_snapshot);
        file_bitmap->cover(bitmap_snapshot);
        stress_log("stress_dump, cf_id:%d\n", i);
        // stress_bitmap_[i]->print_to_log(bench_log);
      }
    }
  }

  void stress_stage_join(size_t stage) {
    MutexLock l(&stress_worker_mutex_);
    int32_t free_threads = FLAGS_num_single_delete_threads;

    if (--stress_worker_count_[stage] == free_threads) {
      stress_worker_count_[stage] = FLAGS_threads;
      stress_worker_cond_.SignalAll();
    } else {
      while (stress_worker_count_[stage] != FLAGS_threads) {
        stress_worker_cond_.Wait();
      }
    }
  }

  void stress_check_history(StressHistory& history, DB* db, int32_t cf_id,
                            const char* thread_flag) {
    if (history.snapshot_ != nullptr) {
      stress_check(
          db, cf_id, history.first_key_id_, FLAGS_num / StressLockMgr::STRIPES,
          history.bitmap_snapshot_.get(), thread_flag, history.snapshot_);
      db->ReleaseSnapshot(history.snapshot_);
      history.snapshot_ = nullptr;
    }
  }

  void stress_check_status(const Status& s, const char* err_msg,
                           uint64_t key_id, const char* thread_flag) {
    if (!s.ok()) {
      stress_log("[%s] %s, cf_id: %d,  key_id: %lu, status: %s\n",
              thread_flag, err_msg, stress_cf_id_, key_id,
              s.ToString().c_str());
      abort();
    }
  }

  bool stress_check_external(const Status& s, const Slice& key,
                             const std::string& data,
                             const StressBitMap& bitmap,
                             const char* thread_flag) {
    return stress_check_external(s, key, data, bitmap,
                                 [](uint64_t) { abort(); }, thread_flag);
  }

  // Return if should skip this key_id and may ignore some errors.
  bool stress_check_external(const Status& s, const Slice& key,
                             const std::string& result,
                             const StressBitMap& bitmap,
                             std::function<void(uint64_t)> error_func,
                             const char* thread_flag) {
    char prefix;
    uint64_t key_id = stress_decode(key, prefix);
    // Timeout is acceptable if not too many.
    if (s.IsTimedOut()) {
      stress_log("cf_id: %d, timeout\n", stress_cf_id_);
      return true;
    }
    // Verify delete stats.
    bool is_deleted;
    stress_check_status(bitmap.is_deleted(key_id, is_deleted),
                        "bitmap check delete failed", key_id, thread_flag);
    if (is_deleted) {
      if (!s.IsNotFound()) {
        stress_log("[%s] read delete key, cf_id: %d, key_id: %lu\n",
                thread_flag, stress_cf_id_, key_id);
        error_func(key_id);
        return false;
      }
      return true;  // Skip if this key is not in db.
    } else if (s.IsNotFound()) {
      stress_log("[%s] key lost, cf_id: %d, key_id: %lu\n", thread_flag,
              stress_cf_id_, key_id);
      SE_LOG(ERROR, "key lost", K(thread_flag), K_(stress_cf_id), K(key_id));
      error_func(key_id);
      return true;  // Skip if this key is not in db.
    } else {
      stress_check_crc32(key, key_id, result, thread_flag);
      uint32_t value_size = DecodeFixed32(result.data() + VALUE_SIZE_OFFSET);
      // Verify update round stats.
      if (result.size() != value_size + VALUE_META_SIZE) {
        stress_log("[%s] value corrupt, cf_id: %d, key_id: %lu\n",
                thread_flag, stress_cf_id_, key_id);
        error_func(key_id);
      }
      uint32_t round_num = DecodeFixed32(result.data());
      uint32_t expected_round_num;
      stress_check_status(bitmap.get(key_id, expected_round_num),
                          "bitmap get filed", key_id, thread_flag);
      if (round_num > expected_round_num) {
       stress_log(
                "[%s] update happens after the key is locked, "
                "cf_id: %d, key_id: %lu\n",
                thread_flag, stress_cf_id_, key_id);
        error_func(key_id);
      } else if (round_num < expected_round_num) {
        stress_log("[%s] update lost, cf_id: %d, key_id: %lu\n",
                thread_flag, stress_cf_id_, key_id);
        error_func(key_id);
      }
    }
    return false;
  }

  uint64_t stress_check_key(const Slice& key, char expected_suffix,
                            const char* thread_flag) {
    char suffix;
    uint64_t key_id = stress_decode(key, suffix);
    if (suffix != expected_suffix) {
     stress_log(
              "[%s] inconsistency,  batch is incomplete, "
              "cf_id: %d, key_id: %lu\n",
              thread_flag, stress_cf_id_, key_id);
      STRESS_CHECK_PRINT(); 
      abort();
    }
    return key_id;
  }

  void stress_check_consistency(const Slice& key1, const Slice& key2,
                                char suffix1, char suffix2,
                                const std::string& value1,
                                const std::string& value2,
                                const char* thread_flag,
                                uint64_t sequence) {
    uint64_t key_id1 = stress_check_key(key1, suffix1, thread_flag);
    uint64_t key_id2 = stress_check_key(key2, suffix2, thread_flag);
    if (key_id1 != key_id2) {
     stress_log(
              "[%s] inconsistency, batch is incomplete, cf_id: %d, "
              "key_id1: %lu, key_id2: %lu\n",
              thread_flag, stress_cf_id_, key_id1, key_id2);
      // print the extent meta
      ColumnFamilyHandle* cfh = stress_get_cfh(stress_cf_id_);
      //reinterpret_cast<ColumnFamilyHandleImpl*>(cfh)->cfd()->get_storage_manager()->print_raw_meta(false);
      reinterpret_cast<ColumnFamilyHandleImpl*>(cfh)->cfd()->get_storage_manager()->print_raw_meta();
      STRESS_CHECK_PRINT(); 
      abort();
    }
    if (value1 != value2) {
       stress_log(
              "[%s] inconsistency, value mismatch, cf_id: %d,"
              " key_id: %lu, sequence: %lu\n",
              thread_flag, stress_cf_id_, key_id1, sequence);
      // print the extent meta
      ColumnFamilyHandle* cfh = stress_get_cfh(stress_cf_id_);
      reinterpret_cast<ColumnFamilyHandleImpl*>(cfh)->cfd()->get_storage_manager()->print_raw_meta(); 
      STRESS_CHECK_PRINT(); 
      abort();
    }
  }

  void stress_check_crc32(const Slice& key, uint64_t key_id,
                          const std::string& value, const char* thread_flag) {
    uint32_t value_size = DecodeFixed32(value.data() + VALUE_SIZE_OFFSET);
    uint32_t crc_expected = DecodeFixed32(value.data() + VALUE_OFFSET + value_size);
    crc_expected = crc32c::Unmask(crc_expected);
    uint32_t crc = crc32c::Value(value.data(), VALUE_OFFSET + value_size);
    crc = crc32c::Extend(crc, key.data(), key.size());
    if (crc != crc_expected) {
      stress_log("[%s] crc check failed, cf_id: %d, key_id: %lu\n",
              thread_flag, stress_cf_id_, key_id);
      abort();
    }
  }

  void stress_insert(ThreadState* thread) {
    DB* db = SelectDB(thread);

    // Allocate buffer.
    std::unique_ptr<const char[]> key_guard;
    std::unique_ptr<const char[]> reflection_key_guard;
    Slice key = AllocateKey(&key_guard, STRESS_FULL_SIZE);
    Slice reflection_key = AllocateKey(&reflection_key_guard, STRESS_FULL_SIZE);
    //char value[STRESS_VALUE_SIZE];
    std::unique_ptr<char[]> vptr(new char[MAX_LARGE_VALUE_SIZE]);
    char *value = vptr.get();
    const char thread_flag[] = "INSERT";
    std::string last_key = stress_encode_last_key(thread->tid);
    RandomGenerator gen;
    WriteOptions wo;
    // Caculate offset and cf_id.
    size_t num_insert_threads =
        (FLAGS_num_deletion_threads + FLAGS_num_update_threads) /
        FLAGS_num_column_families;
    int32_t cf_id = thread->tid / num_insert_threads;
    stress_cf_id_ = cf_id;
    StressLockMgr* lock_mgr = stress_lock_mgr_[cf_id].get();
    StressBitMap* bitmap = stress_bitmap_[cf_id].get();
    ColumnFamilyHandle* cfh = stress_get_cfh(cf_id);
    size_t key_range_length = FLAGS_num / num_insert_threads;
    size_t key_range_offset =
        key_range_length * (thread->tid % num_insert_threads);

    // Start to write.
    do_random_manual_compaction(cfh, db);
    const size_t ROUND_NUM_SIZE = sizeof(uint32_t);
    for (size_t i = 0; i != key_range_length; ++i) {
      STRESS_CHECK_TIME(i);
      std::unique_ptr<WriteBatch> batch_ptr(new WriteBatch());
      WriteBatch &batch = *(batch_ptr.get());
      uint64_t key_id = 0;
      if (!FLAGS_var_len_stress) {
        key_id = key_range_offset + i;
      } else {
        key_id = i * num_insert_threads + thread->tid % num_insert_threads;
      }
      key = Slice(key.data(), STRESS_FULL_SIZE);
      reflection_key = Slice(reflection_key.data(), STRESS_FULL_SIZE);
      stress_encode(key, key_id, 0);
      stress_encode(reflection_key, key_id, 1);

      int value_size = value_size_;
      if (FLAGS_medium_kv) {
        uint64_t rv = rand_large_val.Uniform(100) + 1;
        if (rv <= 10) {
          value_size = rv * 50 * 1024;
          FLAGS_env->SleepForMicroseconds(rv * 100);
        }
      }
      if (FLAGS_large_kv) {
        uint64_t rv = rand_large_val.Uniform(20000) + 1;
        if (rv <= 15) {
          value_size += rv * (2 * 1024 * 1024);
          FLAGS_env->SleepForMicroseconds(rv * 100);
        }
      }
      EncodeFixed32(value, 0 /* round 1 */);
      EncodeFixed32(value + ROUND_NUM_SIZE, thread->tid);
      EncodeFixed32(value + VALUE_SIZE_OFFSET, value_size);
      Slice random_str = gen.Generate(value_size);
      memcpy(value + VALUE_OFFSET, random_str.data(), value_size);
      uint32_t crc = crc32c::Value(value, VALUE_OFFSET + value_size);
      crc = crc32c::Extend(crc, key.data(), key.size());
      crc = crc32c::Mask(crc);
      EncodeFixed32(value + VALUE_OFFSET + value_size, crc);
      size_t stripe = lock_mgr->get_stripe(key_id);
      if (!lock_mgr->lock_stripe_s(stripe)) {
        continue;
      }

      batch.Put(cfh, key, Slice(value, value_size + VALUE_META_SIZE));
      batch.Put(cfh, reflection_key, Slice(value, value_size + VALUE_META_SIZE));
      batch.Put(cfh, last_key, key);
      db->Write(wo, &batch);
      stress_log("ins %d %lu %u value:%d\n", cf_id, key_id, crc, value_size);
      lock_mgr->lock_x(key_id);
      stress_check_status(bitmap->set(key_id, 0 /* round_num */),
                          "bitmap insert failed", key_id, thread_flag);
      lock_mgr->unlock_x(key_id);
      lock_mgr->unlock_stripe_s(stripe);
    }
    do_random_manual_compaction(cfh, db);
  }

#pragma GCC push_options
#pragma GCC optimize ("O1")
  void stress_single_delete(ThreadState* thread) {
    DB* db = SelectDB(thread);

    // Allocate buffer.
    std::unique_ptr<const char[]> key_guard;
    std::unique_ptr<const char[]> reflection_key_guard;
    Slice key = AllocateKey(&key_guard, STRESS_FULL_SIZE);
    Slice reflection_key = AllocateKey(&reflection_key_guard, STRESS_FULL_SIZE);
    std::unique_ptr<char[]> vptr(new char[MAX_LARGE_VALUE_SIZE]);
    char *value = vptr.get();
    const char thread_flag[] = "SINGLE DELETE";
    std::string last_key = stress_encode_last_key(thread->tid);
    RandomGenerator gen;
    WriteOptions wo;
    ReadOptions ro;
    std::string result;
    std::string reflection_result;
    Status s;
    // Caculate offset and cf_id.
    size_t num_insert_threads =
        FLAGS_num_single_delete_threads / FLAGS_num_column_families;
    size_t first_single_delete_thread = FLAGS_threads - FLAGS_num_single_delete_threads;
    int32_t cf_id = (thread->tid - first_single_delete_thread) / num_insert_threads;
    stress_cf_id_ = cf_id;
    ColumnFamilyHandle* cfh = stress_get_cfh(cf_id);
    size_t key_range_length = FLAGS_num / num_insert_threads;
    size_t key_range_offset =
        key_range_length * ((thread->tid - first_single_delete_thread) % num_insert_threads) + FLAGS_num * 2;

    do_random_manual_compaction(cfh, db);

    // Start to write.
    const size_t ROUND_NUM_SIZE = sizeof(uint32_t);
    for (size_t i = 0; i != key_range_length; ++i) {
      std::unique_ptr<WriteBatch> batch_ptr(new WriteBatch());
      WriteBatch &batch = *(batch_ptr.get());
      STRESS_CHECK_TIME(i);
      uint64_t key_id = key_range_offset + i;
      key = Slice(key.data(), STRESS_FULL_SIZE);
      reflection_key = Slice(reflection_key.data(), STRESS_FULL_SIZE);
      stress_encode(key, key_id, 0);
      stress_encode(reflection_key, key_id, 1);

      int value_size = value_size_;
      if (FLAGS_medium_kv) {
        uint64_t rv = rand_large_val.Uniform(100) + 1;
        if (rv <= 10) {
          value_size = rv * 50 * 1024;
          FLAGS_env->SleepForMicroseconds(rv * 100);
        }
      }
      if (FLAGS_large_kv) {
        uint64_t rv = rand_large_val.Uniform(20000) + 1;
        if (rv <= 5) {
          value_size += rv * (2 * 1024 * 1024);
          FLAGS_env->SleepForMicroseconds(rv * 100);
        }
      }
      EncodeFixed32(value, 0 /* round 1 */);
      EncodeFixed32(value + ROUND_NUM_SIZE, thread->tid);
      EncodeFixed32(value + VALUE_SIZE_OFFSET, value_size);
      Slice random_str = gen.Generate(value_size);
      memcpy(value + VALUE_OFFSET, random_str.data(), value_size);
      uint32_t crc = crc32c::Value(value, VALUE_OFFSET + value_size);
      crc = crc32c::Extend(crc, key.data(), key.size());
      crc = crc32c::Mask(crc);
      EncodeFixed32(value + VALUE_OFFSET + value_size, crc);

      s = batch.Put(cfh, key, Slice(value, value_size + VALUE_META_SIZE));
      stress_check_status(s, "insert for single delete failed\n", key_id, thread_flag);
      s = batch.Put(cfh, reflection_key, Slice(value, value_size + VALUE_META_SIZE));
      stress_check_status(s, "insert for single delete failed\n", key_id, thread_flag);
      s = batch.Put(cfh, last_key, key);
      stress_check_status(s, "insert for single delete failed\n", key_id, thread_flag);
      s = db->Write(wo, &batch);
      stress_check_status(s, "insert for single delete failed\n", key_id, thread_flag);
      result.clear();
      s = db->Get(ro, cfh, key, &result);
      stress_check_status(s, "insert for single delete failed\n", key_id, thread_flag);
      reflection_result.clear();
      s = db->Get(ro, cfh, reflection_key, &reflection_result);
      stress_check_status(s, "insert for single delete failed\n", key_id, thread_flag);
      stress_check_consistency(key, reflection_key, 0, 1, result, reflection_result,
                               thread_flag, 0);
      stress_check_crc32(key, key_id, result, thread_flag);
      stress_log("sd-pre %d %lu %u value:%d\n", cf_id, key_id, crc, value_size);
    }

    // Check if the prepare really success.
    do_random_manual_compaction(cfh, db);
    std::unique_ptr<const char[]> begin_key_guard;
    Slice begin_key = AllocateKey(&begin_key_guard, STRESS_FULL_SIZE);
    stress_encode(begin_key, key_range_offset, 0);
    std::unique_ptr<db::Iterator, ptr_destruct_delete<db::Iterator>> iter{db->NewIterator(ro, cfh)};
    iter->Seek(begin_key);
    for (size_t i = 0; i != key_range_length; ++i) {
      uint64_t key_id = key_range_offset + i;
      key = Slice(key.data(), STRESS_FULL_SIZE);
      reflection_key = Slice(reflection_key.data(), STRESS_FULL_SIZE);
      stress_encode(key, key_id, 0);
      stress_encode(reflection_key, key_id, 1);
      result.clear();
      s = db->Get(ro, cfh, key, &result);
      stress_check_status(s, "single delete prepare check failed\n", key_id, thread_flag);
      reflection_result.clear();
      s = db->Get(ro, cfh, reflection_key, &reflection_result);
      stress_check_status(s, "single delete prepare check failed\n", key_id, thread_flag);
      Slice iter_key = iter->key();
      Slice iter_value = iter->value();
      // Check if Get and Iterator is consistency.
      stress_check_consistency(iter_key, key, 0, 0, iter_value.ToString(), result,
                               thread_flag, 0);
      stress_check_consistency(key, reflection_key, 0, 1, result, reflection_result,
                               thread_flag, 0);
      stress_check_crc32(key, key_id, result, thread_flag);
      iter->Next();
      if (!iter->Valid()) {
        stress_log("[%s] single delete prepare failed, "
                    "iter next 1 and become invalid, "
                    "cf_id: %d, key_id: %lu\n",
                    thread_flag, cf_id, key_id);
        abort();
      }
      iter->Next();
      if (!iter->Valid()) {
        stress_log("[%s] single delete prepare failed "
                    "iter next 2 and become invalid, "
                    "cf_id: %d, key_id: %lu\n",
                    thread_flag, cf_id, key_id);
        abort();
      }
    }
    iter.reset();
    stress_log("sd-pre check cf_id: %d, [%lu, %lu)\n",
                cf_id, key_range_offset, key_range_offset + key_range_length);

    // Start to single delete.
    do_random_manual_compaction(cfh, db);
    for (size_t i = 0; i != key_range_length; ++i) {
      STRESS_CHECK_TIME(i);
      std::unique_ptr<WriteBatch> batch_ptr(new WriteBatch());
      WriteBatch &batch = *(batch_ptr.get());
      uint64_t key_id = key_range_offset + i;
      key = Slice(key.data(), STRESS_FULL_SIZE);
      reflection_key = Slice(reflection_key.data(), STRESS_FULL_SIZE);
      stress_encode(key, key_id, 0);
      stress_encode(reflection_key, key_id, 1);

      s = batch.SingleDelete(cfh, key);
      stress_check_status(s, "single delete failed\n", key_id, thread_flag);
      s = batch.SingleDelete(cfh, reflection_key);
      stress_check_status(s, "single delete failed\n", key_id, thread_flag);
      s = batch.Put(cfh, last_key, key);
      stress_check_status(s, "single delete failed\n", key_id, thread_flag);
      s = db->Write(wo, &batch);
      stress_check_status(s, "single delete failed\n", key_id, thread_flag);
      result.clear();
      s = db->Get(ro, cfh, key, &result);
      if (!s.IsNotFound()) {
        stress_log("[%s] single delete failed, cf_id: %d, key_id: %lu\n",
                    thread_flag, cf_id, key_id);
        abort();
      }
      reflection_result.clear();
      s = db->Get(ro, cfh, reflection_key, &reflection_result);
      if (!s.IsNotFound()) {
        stress_log("[%s] single delete failed, cf_id: %d, key_id: %lu\n",
                    thread_flag, cf_id, key_id);
        abort();
      }
      stress_log("sd-del %d %lu\n", cf_id, key_id);
    }

    // Check if single delete works
    do_random_manual_compaction(cfh, db);
    iter.reset(db->NewIterator(ro, cfh));
    iter->Seek(begin_key);
    char suffix;
    if (stress_decode(iter->key(), suffix) < (key_range_offset + key_range_length)) {
        stress_log("[%s] single delete check failed, "
                    "seek entry single deleted, "
                    "cf_id: %d, key_id: %lu\n",
                    thread_flag, cf_id, stress_decode(iter->key(), suffix));
    }

    for (size_t i = 0; i != key_range_length; ++i) {
      STRESS_CHECK_TIME(i);
      uint64_t key_id = key_range_offset + i;
      key = Slice(key.data(), STRESS_FULL_SIZE);
      reflection_key = Slice(reflection_key.data(), STRESS_FULL_SIZE);
      stress_encode(key, key_id, 0);
      stress_encode(reflection_key, key_id, 1);
      result.clear();
      s = db->Get(ro, cfh, key, &result);
      if (!s.IsNotFound()) {
          stress_log("[%s] single delete check failed, "
                      "get entry single deleted, "
                      "cf_id: %d, key_id: %lu\n",
                      thread_flag, cf_id, key_id);
        abort();
      }
      reflection_result.clear();
      s = db->Get(ro, cfh, reflection_key, &reflection_result);
      if (!s.IsNotFound()) {
          stress_log("[%s] single delete check failed, "
                      "get entry single deleted, "
                      "cf_id: %d, key_id: %lu\n",
                      thread_flag, cf_id, key_id);
        abort();
      }
    }
      stress_log("sd-del check cf_id: %d, [%lu, %lu)\n",
                  cf_id, key_range_offset, key_range_offset + key_range_length);
  }
#pragma GCC pop_options

  std::string string_to_hex(const std::string& input) {
    static const char* const lut = "0123456789ABCDEF";
    size_t len = input.length();

    std::string output;
    output.reserve(3 * len);
    for (size_t i = 0; i < len; ++i) {
      const unsigned char c = input[i];
      output.push_back(lut[c >> 4]);
      output.push_back(lut[c & 15]);
      output.push_back(' ');
    }
    return output;
  }

  void stress_update(ThreadState* thread) {
    DB* db = SelectDB(thread);

    // Allocate buffer.
    std::unique_ptr<const char[]> key_guard;
    std::unique_ptr<const char[]> reflection_key_guard;
    Slice key = AllocateKey(&key_guard, STRESS_FULL_SIZE);
    Slice reflection_key = AllocateKey(&reflection_key_guard, STRESS_FULL_SIZE);
    // Allocate value buffer.
    //char value[STRESS_VALUE_SIZE];
    std::unique_ptr<char[]> vptr(new char[MAX_LARGE_VALUE_SIZE]);
    char *value = vptr.get();
    const char thread_flag[] = "UPD";
    std::string last_key = stress_encode_last_key(thread->tid);
    // Caculate offset and cf_id
    size_t num_update_threads_per_cf =
        FLAGS_num_update_threads / FLAGS_num_column_families;
    int32_t cf_id =
        (thread->tid - FLAGS_num_deletion_threads) / num_update_threads_per_cf;
    stress_cf_id_ = cf_id;
    StressBitMap* bitmap = stress_bitmap_[cf_id].get();
    StressLockMgr* lock_mgr = stress_lock_mgr_[cf_id].get();
    ColumnFamilyHandle* cfh = stress_get_cfh(cf_id);
    // Start to Write.
    do_random_manual_compaction(cfh, db);
    RandomGenerator gen;
    WriteOptions wo;
    ReadOptions ro;
    // result is reused between updates, so result.empty() may show last result.
    std::string result;
    std::string old_result;
    for (size_t i = 0; i != FLAGS_update_delete_count; ++i) {
      STRESS_CHECK_TIME(i);
      std::unique_ptr<WriteBatch> batch_ptr(new WriteBatch());
      WriteBatch &batch = *(batch_ptr.get());
      // Generate key and value.
      uint64_t key_id = thread->rand.Next() % FLAGS_num;
      key = Slice(key.data(), STRESS_FULL_SIZE);
      reflection_key = Slice(reflection_key.data(), STRESS_FULL_SIZE);
      stress_encode(key, key_id, 0);
      stress_encode(reflection_key, key_id, 1);
      const Snapshot* old_snapshot = db->GetSnapshot();
      uint64_t old_seq = old_snapshot->GetSequenceNumber();
      ro.snapshot = old_snapshot;
      Status s = db->Get(ro, cfh, key, &old_result);
      db->ReleaseSnapshot(old_snapshot);
      ro.snapshot = nullptr;
      // Get next round number.
      uint32_t round_num;
      stress_check_status(bitmap->get(key_id, round_num), "bitmap get failed",
                          key_id, thread_flag);
      if (round_num >= ((1 << StressBitMap::MAX_ROUND_NUMBER_BITS) - 1)) {
        continue;
      }
      ++round_num;
      int value_size = value_size_;
      if (FLAGS_medium_kv) {
        uint64_t rv = rand_large_val.Uniform(100) + 1;
        if (rv <= 10) {
          value_size = rv * 50 * 1024;
          FLAGS_env->SleepForMicroseconds(rv * 100);
        }
      }
      if (FLAGS_large_kv) {
        uint64_t rv = rand_large_val.Uniform(20000) + 1;
        if (rv <= 15) {
          value_size += rv * (2 * 1024 * 1024);
          FLAGS_env->SleepForMicroseconds(rv * 100);
        }
      }
      // Generate value.
      EncodeFixed32(value, round_num);
      EncodeFixed32(value + sizeof(round_num), thread->tid);
      EncodeFixed32(value + VALUE_SIZE_OFFSET, value_size);
      Slice random_str = gen.Generate(value_size);
      memcpy(value + VALUE_OFFSET, random_str.data(), value_size);
      uint32_t crc = crc32c::Value(value, VALUE_OFFSET + value_size);
      crc = crc32c::Extend(crc, key.data(), key.size());
      crc = crc32c::Mask(crc);
      EncodeFixed32(value + VALUE_OFFSET + value_size, crc);
      // Put to DB.
      size_t stripe = lock_mgr->get_stripe(key_id);
      if (!lock_mgr->lock_stripe_s(stripe)) {
        continue;
      }
      s = batch.Put(cfh, key, Slice(value, value_size + VALUE_META_SIZE));
      stress_check_status(s, "fail to put to batch", key_id, thread_flag);
      s = batch.Put(cfh, reflection_key, Slice(value, value_size + VALUE_META_SIZE));
      stress_check_status(s, "fail to put to batch", key_id, thread_flag);
      s = batch.Put(cfh, last_key, key);
      stress_check_status(s, "fail to record last key", key_id, thread_flag);
      // stress_lock_mgr_.lock_s(key_id);
      s = db->Write(wo, &batch);
      stress_log("upd %d %lu %u\n", cf_id, key_id, crc);
      stress_check_status(s, "fail to write batch to db", key_id, thread_flag);
      // stress_lock_mgr_.unlock_s(key_id);
      // Verify if this thread success and update bitmap.
      lock_mgr->lock_x(key_id);
      const Snapshot* current_snapshot = db->GetSnapshot();
      uint64_t current_seq = current_snapshot->GetSequenceNumber();
      ro.snapshot = current_snapshot;
      s = db->Get(ro, cfh, key, &result);
      db->ReleaseSnapshot(current_snapshot);
      ro.snapshot = nullptr;
      if (!s.ok() && !s.IsNotFound()) {
       stress_log(
                "[%s] get fail in update thread, cf_id: %d, key_id: %lu, "
                "status: %s\n",
                thread_flag, stress_cf_id_, key_id, s.ToString().c_str());
        abort();
      }
      if (s.ok() && result == old_result) {
        stress_log(
                "[%s] update fail, cf_id: %d, key_id: %lu\nexpected value: %s\n"
                "actual & old value: %s\ncurrent sequence: %lu, "
                "old sequence: %lu, expected round num: %u, "
                "actual round num %u\n",
                thread_flag, stress_cf_id_, key_id,
                string_to_hex(std::string(value, STRESS_VALUE_SIZE)).data(),
                string_to_hex(old_result).data(), current_seq, old_seq,
                round_num, DecodeFixed32(result.data()));
        abort();
      }
      if (s.ok()) {
        uint32_t current_round_num = DecodeFixed32(result.data());
        if (current_round_num == round_num) {
          stress_check_status(bitmap->set(key_id, round_num),
                              "bitmap update failed", key_id, thread_flag);
        } else if (current_round_num < round_num) {
          stress_log("[%s] read old data cf_id: %d, key_id: %lu",
                      stress_cf_id_, key_id);
          abort();
        }
      }
      lock_mgr->unlock_x(key_id);
      lock_mgr->unlock_stripe_s(stripe);
    }
    do_random_manual_compaction(cfh, db);
  }

  void stress_delete(ThreadState* thread) {
    DB* db = SelectDB(thread);

    // Allocate key buffer.
    std::unique_ptr<const char[]> key_guard;
    std::unique_ptr<const char[]> reflection_key_guard;
    Slice key = AllocateKey(&key_guard, STRESS_FULL_SIZE);
    Slice reflection_key = AllocateKey(&reflection_key_guard, STRESS_FULL_SIZE);
    const char thread_flag[] = "DEL";
    std::string last_key = stress_encode_last_key(thread->tid);
    // Caculate offset and cf_id.
    size_t num_delete_threads_per_cf =
        FLAGS_num_deletion_threads / FLAGS_num_column_families;
    int32_t cf_id = thread->tid / num_delete_threads_per_cf;
    stress_cf_id_ = cf_id;
    StressBitMap* bitmap = stress_bitmap_[cf_id].get();
    StressLockMgr* lock_mgr = stress_lock_mgr_[cf_id].get();
    ColumnFamilyHandle* cfh = stress_get_cfh(cf_id);

    // Start to delete.
    do_random_manual_compaction(cfh, db);
    WriteOptions wo;
    ReadOptions ro;
    ReadOptions history_ro;
    // result is reused between deletes, so result.empty() may show last result.
    std::string result;
    std::string old_result;
    Status s;
    for (size_t i = 0; i != FLAGS_update_delete_count; ++i) {
      STRESS_CHECK_TIME(i);
      std::unique_ptr<WriteBatch> batch_ptr(new WriteBatch());
      WriteBatch &batch = *(batch_ptr.get());
      // Generate key.
      uint64_t key_id = thread->rand.Next() % FLAGS_num;
      key = Slice(key.data(), STRESS_FULL_SIZE);
      reflection_key = Slice(reflection_key.data(), STRESS_FULL_SIZE);
      stress_encode(key, key_id, 0);
      stress_encode(reflection_key, key_id, 1);
      // Put to DB.
      size_t stripe = lock_mgr->get_stripe(key_id);
      if (!lock_mgr->lock_stripe_s(stripe)) {
        continue;
      }
      s = batch.Delete(cfh, key);
      stress_check_status(s, "fail to delete in batch", key_id, thread_flag);
      s = batch.Delete(cfh, reflection_key);
      stress_check_status(s, "fail to delete in batch", key_id, thread_flag);
      s = batch.Put(cfh, last_key, key);
      stress_check_status(s, "fail to record last key", key_id, thread_flag);
      s = db->Write(wo, &batch);
      stress_log("del %d %lu\n", cf_id, key_id);
      stress_check_status(s, "fail to write batch to db", key_id, thread_flag);
      // Verify and update bitmap.
      lock_mgr->lock_x(key_id);
      s = db->Get(ro, cfh, key, &result);
      if (!s.ok() && !s.IsNotFound()) {
       stress_log(
                "[%s] get fail in delete thread, cf_id: %d, key_id: %lu, "
                "status: %s\n",
                thread_flag, stress_cf_id_, key_id, s.ToString().c_str());
        abort();
      }
      if (s.IsNotFound()) {
        stress_check_status(bitmap->delete_bit(key_id), "bitmap delete failed",
                            key_id, thread_flag);
      }
      lock_mgr->unlock_x(key_id);
      lock_mgr->unlock_stripe_s(stripe);
    }
    do_random_manual_compaction(cfh, db);
  }

  void stress_trx_update(ThreadState* thread) {
    DB* db = SelectDB(thread);
    TransactionDB* trx_db = reinterpret_cast<TransactionDB*>(db);
    if (trx_db == nullptr) {
      stress_log("TransactionDB is not used\n");
      abort();
    }
    // Allocate buffer.
    std::unique_ptr<const char[]> key_guard;
    std::unique_ptr<const char[]> reflection_key_guard;
    Slice key = AllocateKey(&key_guard, STRESS_FULL_SIZE);
    Slice reflection_key = AllocateKey(&reflection_key_guard, STRESS_FULL_SIZE);
    // Allocate value buffer.
    //char value[STRESS_VALUE_SIZE];
    std::unique_ptr<char[]> vptr(new char[MAX_LARGE_VALUE_SIZE]);
    char *value = vptr.get();
    const char thread_flag[] = "TRX UPD";
    std::string last_key = stress_encode_last_key(thread->tid);
    // Caculate offset and cf_id
    size_t num_update_threads_per_cf =
        FLAGS_num_update_threads / FLAGS_num_column_families;
    int32_t cf_id =
        (thread->tid - FLAGS_num_deletion_threads) / num_update_threads_per_cf;
    stress_cf_id_ = cf_id;
    StressBitMap* bitmap = stress_bitmap_[cf_id].get();
    StressLockMgr* lock_mgr = stress_lock_mgr_[cf_id].get();
    ColumnFamilyHandle* cfh = stress_get_cfh(cf_id);

    // Start to Write.
    RandomGenerator gen;
    WriteOptions wo;
    ReadOptions ro;
    ReadOptions history_ro;
    // result is reused between updates, so result.empty() may show last result.
    std::string result;
    std::string reflection_result;
    for (size_t i = 0; i != FLAGS_update_delete_count; ++i) {
      STRESS_CHECK_TIME(i);
      // Generate key and value.
      uint64_t key_id = thread->rand.Next() % FLAGS_num;
      key = Slice(key.data(), STRESS_FULL_SIZE);
      reflection_key = Slice(reflection_key.data(), STRESS_FULL_SIZE);
      stress_encode(key, key_id, 0);
      stress_encode(reflection_key, key_id, 1);
      // Get next round number.
      std::unique_ptr<Transaction> trx{trx_db->BeginTransaction(wo)};
      Status s = trx->GetForUpdate(ro, cfh, key, &result);
      if (stress_check_external(s, key, result, *bitmap, thread_flag)) {
        trx->Rollback();
        continue;
      }
      s = trx->Get(ro, cfh, key, &result);
      if (stress_check_external(s, key, result, *bitmap, thread_flag)) {
        trx->Rollback();
        continue;
      }
      uint32_t round_num;
      stress_check_status(bitmap->get(key_id, round_num), "bitmap get failed",
                          key_id, thread_flag);
      if (round_num + 1 >= (1U << StressBitMap::MAX_ROUND_NUMBER_BITS)) {
        trx->Rollback();
        continue;
      }
      ++round_num;
      // Generate value.
      int value_size = value_size_;
      if (FLAGS_medium_kv) {
        uint64_t rv = rand_large_val.Uniform(100) + 1;
        if (rv <= 10) {
          value_size = rv * 50 * 1024;
          FLAGS_env->SleepForMicroseconds(rv * 100);
        }
      }
      if (FLAGS_large_kv) {
        uint64_t rv = rand_large_val.Uniform(20000) + 1;
        if (rv <= 15) {
          value_size += rv * (2 * 1024 * 1024);
          FLAGS_env->SleepForMicroseconds(rv * 100);
        }
      }
      EncodeFixed32(value, round_num);
      EncodeFixed32(value + sizeof(round_num), thread->tid);
      EncodeFixed32(value + VALUE_SIZE_OFFSET, value_size);
      Slice random_str = gen.Generate(value_size_);
      memcpy(value + VALUE_OFFSET, random_str.data(), value_size);
      uint32_t crc = crc32c::Value(value, VALUE_OFFSET + value_size);
      crc = crc32c::Extend(crc, key.data(), key.size());
      crc = crc32c::Mask(crc);
      EncodeFixed32(value + VALUE_OFFSET + value_size, crc);
      // Put to DB.
      size_t stripe = lock_mgr->get_stripe(key_id);
      if (!lock_mgr->lock_stripe_s(stripe)) {
        trx->Rollback();
        continue;
      }
      s = trx_db->Put(wo, cfh, last_key, key);
      stress_check_status(s, "fail to record last key", key_id, thread_flag);
      s = trx->Put(cfh, key, Slice(value, value_size + VALUE_META_SIZE));
      stress_check_status(s, "update fail", key_id, thread_flag);
      s = trx->Put(cfh, reflection_key, Slice(value, value_size + VALUE_META_SIZE));
      stress_check_status(s, "update fail", key_id, thread_flag);
      lock_mgr->lock_x(key_id);
      stress_check_status(bitmap->set(key_id, round_num),
                          "bitmap update failed", key_id, thread_flag);
      lock_mgr->unlock_x(key_id);
      // This commit must success, because there should be no conflict.
      s = trx->Commit();
      stress_log("trxupd %d %lu %u\n", cf_id, key_id, crc);
      lock_mgr->unlock_stripe_s(stripe);
      stress_check_status(s, "commit fail", key_id, thread_flag);
    }
  }

  void stress_trx_delete(ThreadState* thread) {
    DB* db = SelectDB(thread);
    TransactionDB* trx_db = reinterpret_cast<TransactionDB*>(db);
    if (trx_db == nullptr) {
      stress_log("TransactionDB is not used\n");
      abort();
    }
    // Allocate key buffer.
    std::unique_ptr<const char[]> key_guard;
    std::unique_ptr<const char[]> reflection_key_guard;
    Slice key = AllocateKey(&key_guard, STRESS_FULL_SIZE);
    Slice reflection_key = AllocateKey(&reflection_key_guard, STRESS_FULL_SIZE);
    const char thread_flag[] = "TRX DEL";
    std::string last_key = stress_encode_last_key(thread->tid);
    // Caculate offset and cf_id.
    size_t num_delete_threads_per_cf =
        FLAGS_num_deletion_threads / FLAGS_num_column_families;
    int32_t cf_id = thread->tid / num_delete_threads_per_cf;
    stress_cf_id_ = cf_id;
    StressBitMap* bitmap = stress_bitmap_[cf_id].get();
    StressLockMgr* lock_mgr = stress_lock_mgr_[cf_id].get();
    ColumnFamilyHandle* cfh = stress_get_cfh(cf_id);
    // Start to delete.
    WriteOptions wo;
    ReadOptions ro;
    ReadOptions history_ro;
    // result is reused between deletes, so result.empty() may show last result.
    std::string result;
    std::string reflection_result;
    for (size_t i = 0; i != FLAGS_update_delete_count; ++i) {
      STRESS_CHECK_TIME(i);
      // Generate key.
      uint64_t key_id = thread->rand.Next() % FLAGS_num;
      key = Slice(key.data(), STRESS_FULL_SIZE);
      reflection_key = Slice(reflection_key.data(), STRESS_FULL_SIZE);
      stress_encode(key, key_id, 0);
      stress_encode(reflection_key, key_id, 1);
      std::unique_ptr<Transaction> trx{trx_db->BeginTransaction(wo)};
      Status s = trx->GetForUpdate(ro, cfh, key, &result);
      if (stress_check_external(s, key, result, *bitmap, thread_flag)) {
        trx->Rollback();
        continue;
      }
      s = trx->Get(ro, cfh, key, &result);
      if (stress_check_external(s, key, result, *bitmap, thread_flag)) {
        trx->Rollback();
        continue;
      }
      // Put to DB.
      size_t stripe = lock_mgr->get_stripe(key_id);
      if (!lock_mgr->lock_stripe_s(stripe)) {
        trx->Rollback();
        continue;
      }
      s = trx_db->Put(wo, cfh, last_key, key);
      stress_check_status(s, "fail to record last key", key_id, thread_flag);
      s = trx->Delete(cfh, key);
      stress_check_status(s, "delete fail", key_id, thread_flag);
      s = trx->Delete(cfh, reflection_key);
      stress_check_status(s, "delete fail", key_id, thread_flag);
      lock_mgr->lock_x(key_id);
      stress_check_status(bitmap->delete_bit(key_id), "bitmap delete failed",
                          key_id, thread_flag);
      lock_mgr->unlock_x(key_id);
      s = trx->Commit();
      stress_log("trxdel %d %lu\n", cf_id, key_id);
      lock_mgr->unlock_stripe_s(stripe);
      stress_check_status(s, "commit fail", key_id, thread_flag);
    }
  }

  void stress_read(ThreadState* thread, const bool& stage_complete) {
    const size_t history_size = 10;
    static thread_local std::random_device stress_rd;
    static thread_local std::mt19937 stress_gen(stress_rd());
    std::unique_ptr<StressHistory[]> history{new StressHistory[history_size]};
    int history_index = 0;
    DB* db = SelectDB(thread);
    const char* thread_flag = "READ";
    // Allocate key buffer.
    std::unique_ptr<const char[]> begin_key_guard;
    Slice begin_key = AllocateKey(&begin_key_guard, STRESS_FULL_SIZE);
    // Caculate offset and cf_id.
    size_t first_tid = FLAGS_num_deletion_threads + FLAGS_num_update_threads;
    size_t num_read_threads =
        (FLAGS_threads - FLAGS_num_single_delete_threads - first_tid) /
        FLAGS_num_column_families;
    int32_t cf_id = (thread->tid - first_tid) / num_read_threads;
    stress_cf_id_ = cf_id;
    StressBitMap* bitmap = stress_bitmap_[cf_id].get();
    StressLockMgr* lock_mgr = stress_lock_mgr_[cf_id].get();
    ColumnFamilyHandle* cfh = stress_get_cfh(cf_id);

    // Seek to start of this partition.
    do_random_manual_compaction(cfh, db);
    size_t key_range_length = FLAGS_num / num_read_threads;
    size_t key_range_offset =
        key_range_length * ((thread->tid - first_tid) % num_read_threads);
    stress_encode(begin_key, key_range_offset, 0);
    std::string last_key;
    std::string last_reflection_key;
    ReadOptions total_order_ro;
    total_order_ro.total_order_seek = true;
    while (!stage_complete) {
      STRESS_CHECK_CLEAR();
      std::unique_ptr<db::Iterator, ptr_destruct_delete<db::Iterator>> iter{db->NewIterator(total_order_ro, cfh)};
      iter->Seek(begin_key);
      for (size_t i = 0; iter->Valid() && i != key_range_length &&
                         !iter->key().starts_with(THREAD_LAST_KEY_PREFIX);
           ++i) {
        STRESS_CHECK_TIME(i);
        // Copy key and value to local string.
        std::string key = iter->key().ToString();
        std::string value = iter->value().ToString();
        iter->Next();
        if (!iter->Valid()) {
          char prefix;
          stress_log(
                  "FAIL to continue, iter invalid, cf_id: %d, key_id: %lu\n",
                  stress_cf_id_, stress_decode(Slice(key), prefix));
          abort();
        }
        std::string reflection_key = iter->key().ToString();
        std::string reflection_value = iter->value().ToString();
        stress_check_consistency(Slice(key), Slice(reflection_key), 0, 1, value,
                                 reflection_value, thread_flag, 0);
        iter->Next();
        last_key.assign(key.data(), key.size());
        last_reflection_key.assign(reflection_key.data(),
                                   reflection_key.size());
      }
      size_t stripe = lock_mgr->get_stripe(key_range_offset) +
                      static_cast<size_t>(stress_stripe_dist_(stress_gen));
      lock_mgr->lock_stripe_x(stripe);
      size_t stripe_size = FLAGS_num / StressLockMgr::STRIPES;
      history[history_index].save_snapshot(db, stripe * stripe_size,
                                           stripe_size, bitmap);
      lock_mgr->unlock_stripe_x(stripe);
      history_index = (history_index + 1) % history_size;
      stress_check_history(history[history_index], db, cf_id, thread_flag);
    }
    for (size_t i = 0; i != history_size; ++i) {
      stress_check_history(history[i], db, cf_id, thread_flag);
    }
    do_random_manual_compaction(cfh, db);
  }

  void stress_check(ThreadState* thread) {
    DB* db = SelectDB(thread);
    size_t num_check_threads = FLAGS_threads / FLAGS_num_column_families;
    size_t scan_length = FLAGS_num / num_check_threads;
    size_t scan_offset = scan_length * (thread->tid % num_check_threads);
    const char* thread_flag = "CHCK";
    int32_t cf_id = thread->tid / num_check_threads;
    stress_cf_id_ = cf_id;
    fprintf(bench_log, "check %d [%lu, %lu)\n", cf_id, scan_offset, scan_offset + scan_length);
    stress_check(db, cf_id, scan_offset, scan_length,
                 stress_bitmap_[cf_id].get(), thread_flag);
    fprintf(bench_log, "check complete %d [%lu, %lu)\n", cf_id, scan_offset, scan_offset + scan_length);
  }

  void stress_check(DB* db, int32_t cf_id, size_t offset, size_t length,
                    const StressBitMap* bitmap, const char* thread_flag,
                    const Snapshot* snapshot = nullptr,
                    size_t* error_counter = nullptr,
                    std::list<StressErrorKey>* error_keys = nullptr) {
    // Allocate key buffer.
    std::unique_ptr<const char[]> begin_key_guard;
    std::unique_ptr<const char[]> key_guard;
    Slice begin_key = AllocateKey(&begin_key_guard, STRESS_FULL_SIZE);
    Slice get_key = AllocateKey(&key_guard, STRESS_FULL_SIZE);
    ColumnFamilyHandle* cfh = stress_get_cfh(cf_id);
    std::function<void(StressKeyID)> error_func = [](StressKeyID key_id) {
      STRESS_CHECK_PRINT();
      abort();
    };

    if (error_counter != nullptr && error_keys != nullptr) {
      error_func = [this, error_counter, error_keys, db,
                    cf_id](StressKeyID key_id) {
        error_keys->push_back({cf_id, key_id});
        ++(*error_counter);
      };
    }

    stress_encode(begin_key, offset, 0);
    std::string result;
    ReadOptions ro;
    ro.snapshot = snapshot;
    uint64_t sequence = 0;
    if (snapshot != nullptr) {
      sequence = snapshot->GetSequenceNumber();
    }
    std::unique_ptr<db::Iterator, ptr_destruct_delete<db::Iterator>> iter{db->NewIterator(ro, cfh)};
    iter->Seek(begin_key);
    Status s;
    if (stress_cf_id_ != cf_id) {
      fprintf(stderr, "db_bench internal error\n");
      abort();
    }
    // Start to iterate DB and compare.
    for (size_t i = 0; iter->Valid() && i != length; ++i) {
      STRESS_CHECK_TIME(i);
      get_key = Slice(get_key.data(), STRESS_FULL_SIZE);
      uint64_t key_id = offset + i;
      stress_encode(get_key, key_id, 0 /* suffix */);
      STRESS_CHECK_CLEAR();
      s = db->Get(ro, cfh, get_key, &result);
      if (stress_check_external(s, get_key, result, *bitmap, error_func,
                                thread_flag)) {
        continue;
      }
      Slice key = iter->key();
      Slice value = iter->value();
      // Check if Get and Iterator is consistency.
      stress_check_consistency(key, get_key, 0, 0, result, value.ToString(),
                               thread_flag, sequence);
      stress_check_crc32(key, key_id, result, thread_flag);
      // Skip the reflection data and go to next key.
      iter->Next();
      iter->Next();
    }
  }

  void do_random_manual_compaction(ColumnFamilyHandle* cfh, DB* db) {
    //TODO yeti add option
    return;
    Random *rand = Random::GetTLSInstance();
    int32_t task_type = rand->Uniform(18);
    uint32_t subtable_id = rand->Uniform(FLAGS_num_column_families) + 1;
    if (0 == subtable_id) {
      db->CompactRange(
          smartengine::common::CompactRangeOptions(), nullptr, nullptr, task_type);
    } else {
      db->CompactRange(
          smartengine::common::CompactRangeOptions(), cfh, nullptr, nullptr, task_type);
    }
  }

  void stress(ThreadState* thread) {
    uint64_t start_time = FLAGS_env->NowMicros();
    static bool complete = false;
    static bool modify_stage_complete = false;
    bool is_delete_thread = thread->tid < FLAGS_num_deletion_threads;
    bool is_write_thread =
        thread->tid < FLAGS_num_deletion_threads + FLAGS_num_update_threads;
    bool is_single_delete_thread =
        thread->tid >= FLAGS_threads - FLAGS_num_single_delete_threads;
    bool first_iterate = true;
    while (true) {
      DB* db = SelectDB(thread);
      if (is_single_delete_thread) {
        stress_single_delete(thread);
        // trigger check time every time.
        STRESS_CHECK_TIME(0);
      } else {
        size_t stage = 5;
        modify_stage_complete = false;

        if (is_recover_mode && first_iterate) {
          ++stage;
          stress_log("check after recovery begin, thread:%d\n", thread->tid);
          stress_check(thread);
          stress_log("check after recovery complete, thread:%d\n", thread->tid);
          stress_stage_join(--stage);
          stress_start_time = FLAGS_env->NowMicros();
          first_iterate = false;
        }

        if (is_write_thread) {
          stress_insert(thread);
        } else {
          stress_read(thread, modify_stage_complete);
        }
        modify_stage_complete = true;
        stress_stage_join(--stage);
        modify_stage_complete = false;
        if (is_delete_thread) {
          if (FLAGS_transaction_db) {
            stress_trx_delete(thread);
          } else {
            stress_delete(thread);
          }
        } else if (is_write_thread) {
          if (FLAGS_transaction_db) {
            stress_trx_update(thread);
          } else {
            stress_update(thread);
          }
        } else {
          stress_read(thread, modify_stage_complete);
        }
        modify_stage_complete = true;
        stress_stage_join(--stage);
        stress_check(thread);
        stress_stage_join(--stage);
        if ((FLAGS_env->NowMicros() - stress_start_time) * 1e-6 > FLAGS_duration) {
          complete = true;
        }
        stress_stage_join(--stage);
        if (complete) {
          if (0 == thread->tid) {
            stress_dump();
          }
          return;
        }
        stress_stage_join(--stage);
      }
    }
  }

  void stress_durability_check(ThreadState* thread) {
    const size_t CHECK_STAGE = 3;
    size_t stage = CHECK_STAGE;
    static std::atomic<size_t> total_error_counter;
    static std::list<StressErrorKey> total_error_list;
    static port::Mutex error_list_mutex;
    static bool should_fix = true;
    total_error_counter.store(0);
    // Stage 1: check and gather all errors.
    stress_stage_join(--stage);
    size_t thread_error_counter = 0;
    DB* db = SelectDB(thread);
    size_t check_threads = FLAGS_threads / FLAGS_num_column_families;
    int32_t cf_id = thread->tid / check_threads;
    stress_cf_id_ = cf_id;
    StressBitMap* bitmap = stress_bitmap_[cf_id].get();
    ColumnFamilyHandle* cfh = stress_get_cfh(cf_id);
    size_t scan_length = FLAGS_num / FLAGS_threads;
    size_t scan_offset = scan_length * (thread->tid % check_threads);
    const char* thread_flag = "CHCK";
    std::list<StressErrorKey> error_list;
    stress_check(db, cf_id, scan_offset, scan_length, bitmap, thread_flag,
                 nullptr /* snapshot */, &thread_error_counter, &error_list);
    total_error_counter.fetch_add(thread_error_counter);
    error_list_mutex.Lock();
    std::copy(error_list.begin(), error_list.end(),
              std::back_inserter(total_error_list));
    error_list_mutex.Unlock();

    // Stage 2: check if errors are reasonable and if yes, fix all errors.
    // This stage is done by thread 0.
    // Don't use stress_cf_id_ here because only thread 0 runs.
    stress_stage_join(--stage);
    if (thread->tid == 0) {
      stress_log("[%s] total error %lu\n", thread_flag,
              total_error_counter.load());
      if (total_error_counter.load() >
          static_cast<size_t>(FLAGS_num_update_threads) +
              static_cast<size_t>(FLAGS_num_deletion_threads)) {
        stress_log("[%s] FAIL more than 1 error/thread, total error %lu\n",
                thread_flag, total_error_counter.load());
        // Too many errors, don't fix to keep errors unchanged.
        should_fix = false;
      } else {
        std::unordered_map<int32_t, std::unordered_set<StressKeyID>>
            last_key_set;
        ReadOptions total_order_ro;
        total_order_ro.total_order_seek = true;
        for (int32_t cf_idx = 0; cf_idx < FLAGS_num_column_families; ++cf_idx) {
          std::unique_ptr<db::Iterator> iter{
              db->NewIterator(total_order_ro, stress_get_cfh(cf_idx))};
          for (iter->Seek(THREAD_LAST_KEY_PREFIX);
               iter->Valid() && iter->key().starts_with(THREAD_LAST_KEY_PREFIX);
               iter->Next()) {
            char suffix;
            last_key_set[cf_idx].insert(stress_decode(iter->value(), suffix));
          }
        }
        for (StressErrorKey& error_key : total_error_list) {
          if (last_key_set[error_key.cf_id_].find(error_key.key_id_) ==
              last_key_set[error_key.cf_id_].end()) {
            stress_log(
                    "[%s] FAIL found error cf_id: %d, key_id: %lu, "
                    "but is not last\n",
                    thread_flag, error_key.cf_id_, error_key.key_id_);
            // Unexpected errors, dont't fix to keep errors unchanged.
            should_fix = false;
          }
        }
      }
      // Fix all errors by thread 0 to avoid race condition.
      if (should_fix) {
        std::unique_ptr<const char[]> key0_guard;
        std::unique_ptr<const char[]> key1_guard;
        Slice key0 = AllocateKey(&key0_guard, STRESS_FULL_SIZE);
        Slice key1 = AllocateKey(&key1_guard, STRESS_FULL_SIZE);
        WriteBatch fix_batch;
        for (StressErrorKey& error_key : total_error_list) {
          // Just Delete both sides to reach consistency.
          key0 = Slice(key0.data(), STRESS_FULL_SIZE);
          key1 = Slice(key1.data(), STRESS_FULL_SIZE);
          stress_encode(key0, error_key.key_id_, 0);
          stress_encode(key1, error_key.key_id_, 1);
          fix_batch.Clear();
          fix_batch.Delete(stress_get_cfh(error_key.cf_id_), key0);
          fix_batch.Delete(stress_get_cfh(error_key.cf_id_), key1);
          db->Write(WriteOptions(), &fix_batch);
          stress_bitmap_[error_key.cf_id_]->delete_bit(error_key.key_id_);
        }
      }
    }  // tid == 0 finish. Can use stress_cf_id_ now.

    // Stage 3: check again to see if all diff have been fixed.
    stress_stage_join(--stage);
    if (should_fix) {
      error_list.clear();
      thread_error_counter = 0;
      stress_check(db, cf_id, scan_offset, scan_length, bitmap, thread_flag,
                   nullptr /* snapshot */, &thread_error_counter, &error_list);
      if (!error_list.empty() || thread_error_counter != 0) {
        stress_log("[%s] FAIL fix fail", thread_flag);
      }
    }
  }

  int insert_one_entry(StorageManager *sm, int32_t level, 
    int64_t seq, int64_t key, ThreadState *Thread, bool del, bool recover, int64_t max) {
    //std::unique_ptr<WriteBatch> batch(new WriteBatch());
    /*
    Slice largest;
    Slice smallest;
    char end_key[16] = {0}; // fake internal key
    char start_key[16] = {0};
    ChangeInfo info;           
    MetaKey meta_key;
    MetaValue meta_value;
    ExtentId extentid;
    autovector<ExtentId> extentids;
    int32_t size_1 = 0;
    int32_t size_2 = 0;
    // small key and value
    char buf[1024];
    int ret = Status::kOk;
    ExtentMeta extentmeta;
    //autovector<ExtentMeta> extentmetas;
    std::chrono::nanoseconds diff;
    std::chrono::high_resolution_clock::time_point begin, over;

    int64_t pos = 0;
    if (0 == max) {
      util::encode_fixed_int64(end_key, 16, pos, 10 * (key + 1));
      largest = Slice(end_key, 16);
      meta_key = MetaKey(0, level, seq, largest); 
      pos = 0;
      util::encode_fixed_int64(start_key, 16, pos, 10 * key + 1);
      smallest = Slice(start_key, 16);

      extentid = ExtentId(key / 512, key % 512);
      meta_value = MetaValue(smallest, extentid);
      size_1 = meta_key.get_serialize_size();
      size_2 = meta_value.get_serialize_size();
      pos = 0; 
      ret = meta_key.serialize(buf, size_1, pos);
      if (ret != Status::kOk) {
        fprintf(stderr, "Serialize key failed\n");
        return ret;
      } 
      ret = meta_value.serialize(buf, size_1 + size_2, pos);
      if (ret != Status::kOk) {
        fprintf(stderr, "Serialize value failed\n");
        return ret;
      }
      //extentmetas.clear();
      if (del) {
        info.batch_.Delete(Slice(buf, size_1));
      } else {
        info.batch_.Put(Slice(buf, size_1), Slice(buf + size_1, size_2)); 
        extentmeta.extent_id_ = extentid;
        extentmeta.largest_key_.DecodeFrom(largest);
        extentmeta.smallest_key_.DecodeFrom(smallest);
        info.extent_meta_.emplace_back(extentmeta);
      }
    } else {
      for (key = 0; key < max; key++) {
        pos = 0;
        util::encode_fixed_int64(end_key, 16, pos, 10 * (key + 1));
        largest = Slice(end_key, 16);
        meta_key = MetaKey(0, level, seq, largest); 
        pos = 0;
        util::encode_fixed_int64(start_key, 16, pos, 10 * key + 1);
        smallest = Slice(start_key, 16);
        extentid = ExtentId(key / 512, key % 512);
        meta_value = MetaValue(smallest, extentid);
        size_1 = meta_key.get_serialize_size();
        size_2 = meta_value.get_serialize_size();
        pos = 0; 
        ret = meta_key.serialize(buf, size_1, pos);
        if (ret != Status::kOk) {
          fprintf(stderr, "Serialize key failed\n");
          return ret;
        } 
        ret = meta_value.serialize(buf, size_1 + size_2, pos);
        if (ret != Status::kOk) {
          fprintf(stderr, "Serialize value failed\n");
          return ret;
        }
        if (del) {
          info.batch_.Delete(Slice(buf, size_1));
        } else {
          info.batch_.Put(Slice(buf, size_1), Slice(buf + size_1, size_2)); 
          extentmeta.extent_id_ = extentid;
          extentmeta.largest_key_.DecodeFrom(largest);
          extentmeta.smallest_key_.DecodeFrom(smallest);
          info.extent_meta_.emplace_back(extentmeta);
        }
      }
    }
    //info = ChangeInfo(*batch, extentmetas);
    begin = std::chrono::high_resolution_clock::now();
    ret = sm->apply(info, recover); // recover = true don't check extent meta
    over = std::chrono::high_resolution_clock::now();
    diff = std::chrono::duration_cast<std::chrono::nanoseconds>(over - begin); 
    Thread->stats.finished_ops(1, diff);
    if (ret != Status::kOk) {
      fprintf(stderr, "Apply meta failed\n");
      return ret;
    }
    return ret;
    */
    return 0;
  }

  int search_one_key(StorageManager *sm, int64_t key, 
                     int32_t &sorted_run, ThreadState *Thread) {
    /*
    char search_key[16] = {0};
    const Snapshot *meta = sm->get_current_version();
    std::vector<MetaEntry> chosen;
    Arena arena;
    Status s;
    // search 100000 times
    std::chrono::nanoseconds diff;
    std::chrono::high_resolution_clock::time_point begin, over;

    int64_t pos = 0;
    util::encode_fixed_int64(search_key, 16, pos, key * 10 + 2);
    chosen.clear();
    begin = std::chrono::high_resolution_clock::now();
    s = sm->search(Slice(search_key, 16), 0, meta, chosen, arena, sorted_run);
    over = std::chrono::high_resolution_clock::now(); 
    if (!s.ok()) {
      fprintf(stderr, "Search storage manager failed %d\n", s.code());
      return s.code();
    }
    if (chosen.empty()) {
      fprintf(stderr, "Can't find the extent of key %ld\n", key);
      return s.code();
    } else {
      std::vector<MetaEntry>::iterator meta_iter = chosen.begin();
      if (meta_iter->value_.extent_id_.file_number != static_cast<int32_t>(key / 512) || 
          meta_iter->value_.extent_id_.offset != static_cast<int64_t>(key % 512)) {
        fprintf(stderr, "Find the wrong extent key %lu, Extent(%d, %d)\n", 
                key, meta_iter->value_.extent_id_.file_number, meta_iter->value_.extent_id_.offset);
        return s.code();
      }
      diff = std::chrono::duration_cast<std::chrono::nanoseconds>(over - begin);
      Thread->stats.finished_ops(1, diff);
    }
    return s.code();
    */
    return 0;
  }

  void meta_check(ThreadState *Thread) {
    /*
    fprintf(stdout, "\n\nPrepare 5000000 meta entries all in level 1:\n");
    int64_t n_meta_entries = 5000000; // 5M meta entries = 10T user data
    int64_t stage = 0;
    if (FLAGS_num_column_families <= 1) {
      fprintf(stderr, 
              "The column families not set correctly, please set num_column_families=2\n");
      return;
    }
    if (db_.db != nullptr) {
      db_.CreateNewCf(open_options_, stage);
    } else {
      fprintf(stderr, "db not create yet\n");
      return;
    }
    size_t id = 0;
    DBWithColumnFamilies* db_with_cfh = SelectDBWithCfh(id);
    if (nullptr == db_with_cfh) {
      fprintf(stderr, "Get the DBWithColumnFamilies failed\n");
      return;
    }
    ColumnFamilyData *cfd = db_with_cfh->get_cfd(0);
    if (nullptr == cfd) {
      fprintf(stderr, "Get the ColumnFamilyData failed\n");
      return;
    }
    StorageManager *sm = cfd->get_storage_manager();
    if (nullptr == sm) {
      fprintf(stderr, "Get the StorageManager failed");
      return;
    }
    // prepare level1 meta data
    int32_t ret = Status::kOk;
    for (int64_t i = 0; i < n_meta_entries; i++) {
      ret = insert_one_entry(sm, 1, 0, i, Thread, false, false, 0);
      if (Status::kOk != ret) {
        return;
      } 
      if (i > 0 && 0 == (i % 100000)) {
        fprintf(stdout, "%ld ins\n", i);
        fflush(stdout);
      }
    }
    fprintf(stdout, "\nInsert %lu entries to storage manager\n", 
            sm->get_mem_table_level2()->num_entries() );
    Thread->stats.Report("meta insert");

    Thread->stats.Start(-1);
    // search level1
    fprintf(stdout, "\n\nSearch 1000000 times of random key:\n");
    uint64_t key = 0;
    int32_t sorted_run = 0;

    for (int64_t i = 0; i < 100000; i++) {
      key = Thread->rand.Next() % n_meta_entries;
      ret = search_one_key(sm, key, sorted_run, Thread);
      if (ret != Status::kOk) {
        return;
      }  
    }
    Thread->stats.Report("meta search only level1");

    // L0 and L1 all have data 
    // different memtable size and different L0 levels
    // memtable 128M, 1G, 8G (64, 512, 4096 extents)
    // L0 level 4, 20, 100
    std::string name;
    for (int32_t l = 4; l <= 100; l *= 5) {
      for (int32_t m = 64; m <= 4096; m *= 8) {
        // prepare L0 data. every L0 layer cover the beginning range
        for (int32_t i = 0; i < l; i++) {
          ret = insert_one_entry(sm, 0, i + 1, 0, Thread, false, true, m);
          if (ret != Status::kOk) {
            return;
          } 
        }

        // search only in level1
        Thread->stats.Start(-1);
        for (int64_t i = 0; i < 10000; i++) {
          key = (Thread->rand.Next() % (n_meta_entries - m)) + m;
          sorted_run = 0;
          ret = search_one_key(sm, key, sorted_run, Thread); 
          if (ret != Status::kOk) {
            return;
          }
        }
        name = "Random meta search only L1 with L0 " + 
               std::to_string(l) + " levels " + 
               std::to_string(m) +" extents/level";
        Thread->stats.Report(name);

        // search through level0 to level1
        Thread->stats.Start(-1);        
        for (int64_t i = 0; i < 10000; i++) {
          key = (Thread->rand.Next() % m);
          sorted_run = 0;
          // up to down
          while (sorted_run < l) {
            ret = search_one_key(sm, key, sorted_run, Thread);
            sorted_run++;
          }
        }
        name = "Random meta search L0 with L0 " + 
               std::to_string(l) + " levels " + 
               std::to_string(m) +" extents/level";
        Thread->stats.adjust_ops(l + 1); // searched l + 1 levels
        Thread->stats.Report(name);

        // cleanup
        fprintf(stdout, "\nDelete the L0 entries ...\n");
        for (int32_t i = 0; i < l; i++) {
          for (int32_t j = 0; j < m; j++) {
            ret = insert_one_entry(sm, 0, i + 1, j, Thread, true, true, 0);
            if (ret != Status::kOk) {
              return;
            } 
          }
        }
        fprintf(stdout, "Recycle delete entries ...\n");
        ret = sm->recycle_delete_entries(
              sm->get_current_version()->GetSequenceNumber());
        if (ret != Status::kOk) {
          fprintf(stderr, "Recycle delete entries failed");
          return;
        } 
      }
    }
    */
  }
};

thread_local int32_t Benchmark::stress_cf_id_ = -1;

int db_bench_tool(int argc, char** argv) {
  port::InstallStackTraceHandler();
  static bool initialized = false;
  if (!initialized) {
    SetUsageMessage(std::string("\nUSAGE:\n") + std::string(argv[0]) +
                    " [OPTIONS]...");
    initialized = true;
  }
  ParseCommandLineFlags(&argc, &argv, true);
  FLAGS_compaction_style_e = (CompactionStyle)FLAGS_compaction_style;
#ifndef ROCKSDB_LITE
  if (FLAGS_statistics && !FLAGS_statistics_string.empty()) {
    fprintf(stderr,
            "Cannot provide both --statistics and --statistics_string.\n");
    exit(1);
  }
  if (!FLAGS_statistics_string.empty()) {
    std::unique_ptr<Statistics> custom_stats_guard;
    dbstats.reset(NewCustomObject<Statistics>(FLAGS_statistics_string,
                                              &custom_stats_guard));
    custom_stats_guard.release();
    if (dbstats == nullptr) {
      fprintf(stderr, "No Statistics registered matching string: %s\n",
              FLAGS_statistics_string.c_str());
      exit(1);
    }
  }
#endif  // ROCKSDB_LITE
  if (FLAGS_statistics) {
    dbstats = CreateDBStatistics();
  }
  FLAGS_compaction_pri_e = (CompactionPri)FLAGS_compaction_pri;

  std::vector<std::string> fanout =
      StringSplit(FLAGS_max_bytes_for_level_multiplier_additional, ',');
  for (size_t j = 0; j < fanout.size(); j++) {
    FLAGS_max_bytes_for_level_multiplier_additional_v.push_back(
#ifndef CYGWIN
        std::stoi(fanout[j]));
#else
        stoi(fanout[j]));
#endif
  }

  FLAGS_compression_type_e =
      StringToCompressionType(FLAGS_compression_type.c_str());

#ifndef ROCKSDB_LITE
  std::unique_ptr<Env> custom_env_guard;
  if (!FLAGS_env_uri.empty()) {
    FLAGS_env = NewCustomObject<Env>(FLAGS_env_uri, &custom_env_guard);
    if (FLAGS_env == nullptr) {
      fprintf(stderr, "No Env registered for URI: %s\n", FLAGS_env_uri.c_str());
      exit(1);
    }
  }
#endif  // ROCKSDB_LITE

  if (!strcasecmp(FLAGS_compaction_fadvice.c_str(), "NONE"))
    FLAGS_compaction_fadvice_e = Options::NONE;
  else if (!strcasecmp(FLAGS_compaction_fadvice.c_str(), "NORMAL"))
    FLAGS_compaction_fadvice_e = Options::NORMAL;
  else if (!strcasecmp(FLAGS_compaction_fadvice.c_str(), "SEQUENTIAL"))
    FLAGS_compaction_fadvice_e = Options::SEQUENTIAL;
  else if (!strcasecmp(FLAGS_compaction_fadvice.c_str(), "WILLNEED"))
    FLAGS_compaction_fadvice_e = Options::WILLNEED;
  else {
    fprintf(stdout, "Unknown compaction fadvice:%s\n",
            FLAGS_compaction_fadvice.c_str());
  }

  FLAGS_rep_factory = StringToRepFactory(FLAGS_memtablerep.c_str());

  // The number of background threads should be at least as much the
  // max number of concurrent compactions.
  // one more for background delete space
  FLAGS_env->SetBackgroundThreads(FLAGS_max_background_compactions + 1);
  // 1 more thread for background create extent space
  FLAGS_env->SetBackgroundThreads(FLAGS_max_background_flushes + 1,
                                  Env::Priority::HIGH);

  // Choose a location for the test database if none given with --db=<path>
  if (FLAGS_db.empty()) {
    std::string default_db_path;
    Env::Default()->GetTestDirectory(&default_db_path);
    default_db_path += "/dbbench";
    FLAGS_db = default_db_path;
  }
  // initialize logger:::Logger
  /*
  if (!logger::Logger::get_log().is_inited()) {
    std::ostringstream oss;
    oss << FLAGS_db << "/Log";
    auto log_level = static_cast<logger::InfoLogLevel>(FLAGS_info_log_level);
    logger::Logger::get_log().init(oss.str().c_str(), log_level,
                                       256 * 1024 * 1024);
  }
  */

  if (FLAGS_stats_interval_seconds > 0) {
    // When both are set then FLAGS_stats_interval determines the frequency
    // at which the timer is checked for FLAGS_stats_interval_seconds
    FLAGS_stats_interval = 1000;
  }

  if (FLAGS_compaction_type == 0) {
    fprintf (stderr, "Use StreamCompaction\n");
  } else if (FLAGS_compaction_type == 1) {
    fprintf (stderr, "Use MinorCompaction for FPGA\n");
  } else {
    fprintf (stderr, "invalid compaction_type, only 0 and 1 is support\n");
  }


  Benchmark benchmark;
  benchmark.Run();
  return 0;
}
}  // namespace tools
}  // namespace smartengine
#endif
