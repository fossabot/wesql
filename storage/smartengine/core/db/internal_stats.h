//  Portions Copyright (c) 2023, ApeCloud Inc Holding Limited
// Portions Copyright (c) 2020, Alibaba Group Holding Limited
//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//

#pragma once
#include <map>
#include <string>
#include <vector>

#include "compact/compaction.h"
#include "db/version_set.h"

namespace smartengine {
namespace db {
class ColumnFamilyData;
class MemTableList;
class DBImpl;

// Config for retrieving a property's value.
struct DBPropertyInfo {
  bool need_out_of_mutex;

  // gcc had an internal error for initializing union of pointer-to-member-
  // functions. Workaround is to populate exactly one of the following function
  // pointers with a non-nullptr value.

  // @param value Value-result argument for storing the property's string value
  // @param suffix Argument portion of the property. For example, suffix would
  //      be "5" for the property "rocksdb.num-files-at-level5". So far, only
  //      certain string properties take an argument.
  bool (InternalStats::*handle_string)(std::string* value, common::Slice suffix,
                                       DBImpl* db);

  // @param value Value-result argument for storing the property's uint64 value
  // @param db Many of the int properties rely on DBImpl methods.
  // @param version Version is needed in case the property is retrieved without
  //      holding db mutex, which is only supported for int properties.
  bool (InternalStats::*handle_int)(uint64_t* value, DBImpl* db);
  bool (InternalStats::*handle_map)(
      std::map<std::string, double>* compaction_stats);
};

extern const DBPropertyInfo* GetPropertyInfo(const common::Slice& property);

#ifndef ROCKSDB_LITE
enum class LevelStatType {
  INVALID = 0,
  NUM_FILES,
  COMPACTED_FILES,
  SIZE_BYTES,
  SCORE,
  READ_GB,
  RN_GB,
  RNP1_GB,
  WRITE_GB,
  W_NEW_GB,
  MOVED_GB,
  WRITE_AMP,
  READ_MBPS,
  WRITE_MBPS,
  COMP_SEC,
  COMP_COUNT,
  AVG_SEC,
  KEY_IN,
  KEY_DROP,
  TOTAL  // total number of types
};

enum class ExtentLevelStatType {
  INVALID = 0,
  NUM_EXTENTS,
  NUM_LAYERS,
  COMPACTED_EXTENTS,
  SIZE_BYTES,
  DATA_BYTES,
  INDEX_BYTES,
  READ_GB,
  WRITE_GB,
  MERGEDIN_RE,
  MERGEOUT_RE,
  MERGEOUT_EXT,
  REUSED_EXT,
  MERGED_EXT,
  REUSED_BL,
  MERGED_BL,
  TOTAL
};

struct LevelStat {
  // This what will be L?.property_name in the flat map returned to the user
  std::string property_name;
  // This will be what we will print in the header in the cli
  std::string header_name;
};

class InternalStats {
 public:
  static const std::map<LevelStatType, LevelStat> compaction_level_stats;
  static const std::map<ExtentLevelStatType, LevelStat> extent_compaction_stats;

  enum InternalCFStatsType {
    LEVEL0_SLOWDOWN_TOTAL,
    LEVEL0_SLOWDOWN_WITH_COMPACTION,
    MEMTABLE_COMPACTION,
    MEMTABLE_SLOWDOWN,
    LEVEL0_NUM_FILES_TOTAL,
    LEVEL0_NUM_FILES_WITH_COMPACTION,
    SOFT_PENDING_COMPACTION_BYTES_LIMIT,
    HARD_PENDING_COMPACTION_BYTES_LIMIT,
    WRITE_STALLS_ENUM_MAX,
    BYTES_FLUSHED,
    BYTES_INGESTED_ADD_FILE,
    INGESTED_NUM_FILES_TOTAL,
    INGESTED_LEVEL0_NUM_FILES_TOTAL,
    INGESTED_NUM_KEYS_TOTAL,
    BYTES_READ,
    BYTES_WRITE,
    ACCESS_CNT,
    INTERNAL_CF_STATS_ENUM_MAX,
  };

  enum InternalDBStatsType {
    WAL_FILE_BYTES,
    WAL_FILE_SYNCED,
    BYTES_WRITTEN,
    NUMBER_KEYS_WRITTEN,
    WRITE_DONE_BY_OTHER,
    WRITE_DONE_BY_SELF,
    WRITE_WITH_WAL,
    WRITE_STALL_MICROS,
    INTERNAL_DB_STATS_ENUM_MAX,
  };

  InternalStats(util::Env* env, ColumnFamilyData* cfd)
      : db_stats_{},
        cf_stats_value_{},
        cf_stats_count_{},
        bg_error_count_(0),
        env_(env),
        cfd_(cfd),
        started_at_(env->NowMicros()) {}

  // Per level compaction stats.  comp_stats_[level] stores the stats for
  // flush that produced data for the specified "level" and current we use
  // this struct to store flush stats.
  // For do not block flush progress and allow stale stat, we do not add mutex
  // here, but you should know that it is possible that master thread Clear()
  // and flush job set() are invoked at the same time.
  struct CompactionStats {
    uint64_t micros;

    // The number of bytes read from all non-output levels
    uint64_t bytes_read_non_output_levels;

    // The number of bytes read from the compaction output level.
    uint64_t bytes_read_output_level;

    // Total number of bytes written during compaction
    uint64_t bytes_written;

    // Total number of bytes moved to the output level
    uint64_t bytes_moved;

    // The number of compaction input files in all non-output levels.
    int num_input_files_in_non_output_levels;

    // The number of compaction input files in the output level.
    int num_input_files_in_output_level;

    // The number of compaction output files.
    int num_output_files;

    // Total incoming entries during compaction between levels N and N+1
    uint64_t num_input_records;

    // Accumulated diff number of entries
    // (num input entries - num output entires) for compaction  levels N and N+1
    uint64_t num_dropped_records;

    // Number of compactions done
    int count;

    explicit CompactionStats(int _count = 0)
        : micros(0),
          bytes_read_non_output_levels(0),
          bytes_read_output_level(0),
          bytes_written(0),
          bytes_moved(0),
          num_input_files_in_non_output_levels(0),
          num_input_files_in_output_level(0),
          num_output_files(0),
          num_input_records(0),
          num_dropped_records(0),
          count(_count) {}

    explicit CompactionStats(const CompactionStats& c)
        : micros(c.micros),
          bytes_read_non_output_levels(c.bytes_read_non_output_levels),
          bytes_read_output_level(c.bytes_read_output_level),
          bytes_written(c.bytes_written),
          bytes_moved(c.bytes_moved),
          num_input_files_in_non_output_levels(
              c.num_input_files_in_non_output_levels),
          num_input_files_in_output_level(c.num_input_files_in_output_level),
          num_output_files(c.num_output_files),
          num_input_records(c.num_input_records),
          num_dropped_records(c.num_dropped_records),
          count(c.count) {}

    void Clear() {
      this->micros = 0;
      this->bytes_read_non_output_levels = 0;
      this->bytes_read_output_level = 0;
      this->bytes_written = 0;
      this->bytes_moved = 0;
      this->num_input_files_in_non_output_levels = 0;
      this->num_input_files_in_output_level = 0;
      this->num_output_files = 0;
      this->num_input_records = 0;
      this->num_dropped_records = 0;
      this->count = 0;
    }

    void Add(const CompactionStats& c) {
      this->micros += c.micros;
      this->bytes_read_non_output_levels += c.bytes_read_non_output_levels;
      this->bytes_read_output_level += c.bytes_read_output_level;
      this->bytes_written += c.bytes_written;
      this->bytes_moved += c.bytes_moved;
      this->num_input_files_in_non_output_levels +=
          c.num_input_files_in_non_output_levels;
      this->num_input_files_in_output_level +=
          c.num_input_files_in_output_level;
      this->num_output_files += c.num_output_files;
      this->num_input_records += c.num_input_records;
      this->num_dropped_records += c.num_dropped_records;
      this->count += c.count;
    }

    void Subtract(const CompactionStats& c) {
      this->micros -= c.micros;
      this->bytes_read_non_output_levels -= c.bytes_read_non_output_levels;
      this->bytes_read_output_level -= c.bytes_read_output_level;
      this->bytes_written -= c.bytes_written;
      this->bytes_moved -= c.bytes_moved;
      this->num_input_files_in_non_output_levels -=
          c.num_input_files_in_non_output_levels;
      this->num_input_files_in_output_level -=
          c.num_input_files_in_output_level;
      this->num_output_files -= c.num_output_files;
      this->num_input_records -= c.num_input_records;
      this->num_dropped_records -= c.num_dropped_records;
      this->count -= c.count;
    }
  };

  void Clear() {
    for (int i = 0; i < INTERNAL_DB_STATS_ENUM_MAX; i++) {
      db_stats_[i].store(0);
    }
    for (int i = 0; i < INTERNAL_CF_STATS_ENUM_MAX; i++) {
      cf_stats_count_[i] = 0;
      cf_stats_value_[i] = 0;
    }
//    for (auto& h : file_read_latency_) {
//      h.Clear();
//    }
    cf_stats_snapshot_.Clear();
    db_stats_snapshot_.Clear();
    bg_error_count_ = 0;
    started_at_ = env_->NowMicros();
    extent_comp_stats_.record_stats_.reset();
    extent_comp_stats_.perf_stats_.reset();
  }

  void add_compaction_stats(const storage::Compaction::Statstics& statstics) {
    extent_comp_stats_.record_stats_.add(statstics.record_stats_);
    extent_comp_stats_.perf_stats_.add(statstics.perf_stats_);
  }

  void AddCFStats(InternalCFStatsType type, uint64_t value) {
    cf_stats_value_[type] += value;
    ++cf_stats_count_[type];
  }

  uint64_t get_cf_stats_value(InternalCFStatsType type) {
    return cf_stats_value_[type];
  }

  uint64_t get_cf_stats_count(InternalCFStatsType type) {
    return cf_stats_count_[type];
  }

  void AddDBStats(InternalDBStatsType type, uint64_t value) {
    auto& v = db_stats_[type];
    v.store(v.load(std::memory_order_relaxed) + value,
            std::memory_order_relaxed);
  }

  uint64_t GetDBStats(InternalDBStatsType type) {
    return db_stats_[type].load(std::memory_order_relaxed);
  }

  monitor::HistogramImpl* GetFileReadHist(int level) {
//    return &file_read_latency_[level];
    return nullptr;
  }

  uint64_t GetBackgroundErrorCount() const { return bg_error_count_; }

  uint64_t BumpAndGetBackgroundErrorCount() { return ++bg_error_count_; }

  bool GetStringProperty(const DBPropertyInfo& property_info,
                         const common::Slice& property, std::string* value,
                         DBImpl* db);

  bool GetMapProperty(const DBPropertyInfo& property_info,
                      const common::Slice& property,
                      std::map<std::string, double>* value);

  bool GetIntProperty(const DBPropertyInfo& property_info, uint64_t* value,
                      DBImpl* db);

  bool GetIntPropertyOutOfMutex(const DBPropertyInfo& property_info, uint64_t* value);

  // Store a mapping from the user-facing DB::Properties string to our
  // DBPropertyInfo struct used internally for retrieving properties.
  static const std::unordered_map<std::string, DBPropertyInfo> ppt_name_to_info;

 private:
  void DumpDBStats(std::string* value);
  void DumpCFMapStats(std::map<std::string, double>* cf_stats);
  void dump_cfmap_stats(
      std::map<int, std::map<ExtentLevelStatType, double>>* level_stats,
      CompactionStats* compaction_stats_sum);
  void DumpCFStats(std::string* value, DBImpl* db = nullptr);
  void dump_cfstats_nofile_histogram(std::string* value);
  void DumpCFFileHistogram(std::string* value);
  void DumpMemoryStats(std::string* value);

  // Per-DB stats
  std::atomic<uint64_t> db_stats_[INTERNAL_DB_STATS_ENUM_MAX];
  // Per-ColumnFamily stats
  uint64_t cf_stats_value_[INTERNAL_CF_STATS_ENUM_MAX];
  uint64_t cf_stats_count_[INTERNAL_CF_STATS_ENUM_MAX];
//  std::vector<monitor::HistogramImpl> file_read_latency_;

  // Per-ColumnFamily/level compaction stats for smartengine
  storage::Compaction::Statstics extent_comp_stats_;

  // Used to compute per-interval statistics
  struct CFStatsSnapshot {
    // ColumnFamily-level stats
    CompactionStats comp_stats;
    uint64_t ingest_bytes_flush;  // Bytes written to L0 (Flush)
    uint64_t stall_count;         // Stall count
    // Stats from compaction jobs - bytes written, bytes read, duration.
    uint64_t compact_bytes_write;
    uint64_t compact_bytes_read;
    uint64_t compact_micros;
    double seconds_up;

    // AddFile specific stats
    uint64_t ingest_bytes_addfile;     // Total Bytes ingested
    uint64_t ingest_files_addfile;     // Total number of files ingested
    uint64_t ingest_l0_files_addfile;  // Total number of files ingested to L0
    uint64_t ingest_keys_addfile;      // Total number of keys ingested

    CFStatsSnapshot()
        : comp_stats(0),
          ingest_bytes_flush(0),
          stall_count(0),
          compact_bytes_write(0),
          compact_bytes_read(0),
          compact_micros(0),
          seconds_up(0),
          ingest_bytes_addfile(0),
          ingest_files_addfile(0),
          ingest_l0_files_addfile(0),
          ingest_keys_addfile(0) {}

    void Clear() {
      comp_stats.Clear();
      ingest_bytes_flush = 0;
      stall_count = 0;
      compact_bytes_write = 0;
      compact_bytes_read = 0;
      compact_micros = 0;
      seconds_up = 0;
      ingest_bytes_addfile = 0;
      ingest_files_addfile = 0;
      ingest_l0_files_addfile = 0;
      ingest_keys_addfile = 0;
    }
  } cf_stats_snapshot_;

  struct DBStatsSnapshot {
    // DB-level stats
    uint64_t ingest_bytes;    // Bytes written by user
    uint64_t wal_bytes;       // Bytes written to WAL
    uint64_t wal_synced;      // Number of times WAL is synced
    uint64_t write_with_wal;  // Number of writes that request WAL
    // These count the number of writes processed by the calling thread or
    // another thread.
    uint64_t write_other;
    uint64_t write_self;
    // Total number of keys written. write_self and write_other measure number
    // of write requests written, Each of the write request can contain updates
    // to multiple keys. num_keys_written is total number of keys updated by all
    // those writes.
    uint64_t num_keys_written;
    // Total time writes delayed by stalls.
    uint64_t write_stall_micros;
    double seconds_up;

    DBStatsSnapshot()
        : ingest_bytes(0),
          wal_bytes(0),
          wal_synced(0),
          write_with_wal(0),
          write_other(0),
          write_self(0),
          num_keys_written(0),
          write_stall_micros(0),
          seconds_up(0) {}

    void Clear() {
      ingest_bytes = 0;
      wal_bytes = 0;
      wal_synced = 0;
      write_with_wal = 0;
      write_other = 0;
      write_self = 0;
      num_keys_written = 0;
      write_stall_micros = 0;
      seconds_up = 0;
    }
  } db_stats_snapshot_;

  // Handler functions for getting property values. They use "value" as a value-
  // result argument, and return true upon successfully setting "value".
  bool HandleNumExtentsAtLevel(std::string* value, common::Slice suffix,
                               DBImpl* db);
  bool HandleCompressionRatioAtLevelPrefix(std::string* value,
                                           common::Slice suffix, DBImpl* db);
  bool HandleLevelStats(std::string* value, common::Slice suffix, DBImpl* db);
  bool HandleStats(std::string* value, common::Slice suffix, DBImpl* db);
  bool HandleCFMapStats(std::map<std::string, double>* compaction_stats);
  bool HandleCFStats(std::string* value, common::Slice suffix, DBImpl* db);
  bool HandleCFStatsNoFileHistogram(std::string* value, common::Slice suffix,
                                    DBImpl* db);
  bool HandleCFFileHistogram(std::string* value, common::Slice suffix,
                             DBImpl* db);
  bool HandleDBStats(std::string* value, common::Slice suffix, DBImpl* db);
  bool HandleMeta(std::string* value, common::Slice suffix,DBImpl* db);
  bool HandleSsTables(std::string* value, common::Slice suffix, DBImpl* db);
  bool HandleAggregatedTableProperties(std::string* value, common::Slice suffix,
                                       DBImpl* db);
  //TODO: @yuanfeng unused code, delete future
  bool HandleNumImmutableMemTable(uint64_t* value, DBImpl* db);
  bool HandleNumImmutableMemTableFlushed(uint64_t* value, DBImpl* db);
  bool HandleMemTableFlushPending(uint64_t* value, DBImpl* db);
  bool HandleNumRunningFlushes(uint64_t* value, DBImpl* db);
  bool HandleCompactionPending(uint64_t* value, DBImpl* db);
  bool HandleNumRunningCompactions(uint64_t* value, DBImpl* db);
  bool HandleBackgroundErrors(uint64_t* value, DBImpl* db);
  bool HandleCurSizeActiveMemTable(uint64_t* value, DBImpl* db);
  bool HandleCurSizeAllMemTables(uint64_t* value, DBImpl* db);
  bool HandleSizeAllMemTables(uint64_t* value, DBImpl* db);
  bool HandleNumEntriesActiveMemTable(uint64_t* value, DBImpl* db);
  bool HandleNumEntriesImmMemTables(uint64_t* value, DBImpl* db);
  bool HandleNumDeletesActiveMemTable(uint64_t* value, DBImpl* db);
  bool HandleNumDeletesImmMemTables(uint64_t* value, DBImpl* db);
  bool HandleEstimateNumKeys(uint64_t* value, DBImpl* db);
  bool HandleNumSnapshots(uint64_t* value, DBImpl* db);
  bool HandleOldestSnapshotTime(uint64_t* value, DBImpl* db);
  bool HandleNumLiveVersions(uint64_t* value, DBImpl* db);
  bool HandleCurrentSuperVersionNumber(uint64_t* value, DBImpl* db);
  bool HandleIsFileDeletionsEnabled(uint64_t* value, DBImpl* db);
  bool HandleBaseLevel(uint64_t* value, DBImpl* db);
  bool HandleTotalSstFilesSize(uint64_t* value, DBImpl* db);
  bool HandleEstimatePendingCompactionBytes(uint64_t* value, DBImpl* db);
  bool HandleEstimateTableReadersMem(uint64_t* value, DBImpl* db);
  bool HandleEstimateLiveDataSize(uint64_t* value, DBImpl* db);
  bool HandleMinLogNumberToKeep(uint64_t* value, DBImpl* db);

  bool HandleDBMemoryStats(std::string* value, common::Slice suffix,
                           DBImpl* db);

  bool HandleActiveMemTableTotalNumber(uint64_t* value, DBImpl* db);
  bool HandleActiveMemTableTotalMemoryAllocated(uint64_t* value, DBImpl* db);
  bool HandleActiveMemTableTotalMemoryUsed(uint64_t* value, DBImpl* db);
  bool HandleUnflushedImmTableTotalNumber(uint64_t* value, DBImpl* db);
  bool HandleUnflushedImmTableTotalMemoryAllocated(uint64_t* value, DBImpl* db);
  bool HandleUnflushedImmTableTotalMemoryUsed(uint64_t* value, DBImpl* db);
  bool HandleTableReaderTotalNumber(uint64_t* value, DBImpl* db);
  bool HandleTableReaderTotalMemoryUsed(uint64_t* value, DBImpl* db);
  bool HandleBlockCacheTotalPinnedMemory(uint64_t* value, DBImpl* db);
  bool HandleBlockCacheTotalMemoryUsed(uint64_t* value, DBImpl* db);
  bool HandleDBTotalMemoryAllocated(uint64_t* value, DBImpl* db);

  // Total number of background errors encountered. Every time a flush task
  // or compaction task fails, this counter is incremented. The failure can
  // be caused by any possible reason, including file system errors, out of
  // resources, or input file corruption. Failing when retrying the same flush
  // or compaction will cause the counter to increase too.
  uint64_t bg_error_count_;

  util::Env* env_;
  ColumnFamilyData* cfd_;
  uint64_t started_at_;
};

#else

class InternalStats {
 public:
  enum InternalCFStatsType {
    LEVEL0_SLOWDOWN_TOTAL,
    LEVEL0_SLOWDOWN_WITH_COMPACTION,
    MEMTABLE_COMPACTION,
    MEMTABLE_SLOWDOWN,
    LEVEL0_NUM_FILES_TOTAL,
    LEVEL0_NUM_FILES_WITH_COMPACTION,
    SOFT_PENDING_COMPACTION_BYTES_LIMIT,
    HARD_PENDING_COMPACTION_BYTES_LIMIT,
    WRITE_STALLS_ENUM_MAX,
    BYTES_FLUSHED,
    BYTES_INGESTED_ADD_FILE,
    INGESTED_NUM_FILES_TOTAL,
    INGESTED_LEVEL0_NUM_FILES_TOTAL,
    INGESTED_NUM_KEYS_TOTAL,
    INTERNAL_CF_STATS_ENUM_MAX,
  };

  enum InternalDBStatsType {
    WAL_FILE_BYTES,
    WAL_FILE_SYNCED,
    BYTES_WRITTEN,
    NUMBER_KEYS_WRITTEN,
    WRITE_DONE_BY_OTHER,
    WRITE_DONE_BY_SELF,
    WRITE_WITH_WAL,
    WRITE_STALL_MICROS,
    INTERNAL_DB_STATS_ENUM_MAX,
  };

  InternalStats(util::Env* env, ColumnFamilyData* cfd) {}

  struct CompactionStats {
    uint64_t micros;
    uint64_t bytes_read_non_output_levels;
    uint64_t bytes_read_output_level;
    uint64_t bytes_written;
    uint64_t bytes_moved;
    int num_input_files_in_non_output_levels;
    int num_input_files_in_output_level;
    int num_output_files;
    uint64_t num_input_records;
    uint64_t num_dropped_records;
    int count;

    explicit CompactionStats(int _count = 0) {}

    explicit CompactionStats(const CompactionStats& c) {}

    void Add(const CompactionStats& c) {}

    void Subtract(const CompactionStats& c) {}
  };

  void AddCompactionStats(int level, const CompactionStats& stats) {}

  void IncBytesMoved(int level, uint64_t amount) {}

  void AddCFStats(InternalCFStatsType type, uint64_t value) {}

  void AddDBStats(InternalDBStatsType type, uint64_t value) {}

  monitor::HistogramImpl* GetFileReadHist(int level) { return nullptr; }

  uint64_t GetBackgroundErrorCount() const { return 0; }

  uint64_t BumpAndGetBackgroundErrorCount() { return 0; }

  bool GetStringProperty(const DBPropertyInfo& property_info,
                         const common::Slice& property, std::string* value,
                         DBImpl* db) {
    return false;
  }

  bool GetMapProperty(const DBPropertyInfo& property_info,
                      const common::Slice& property,
                      std::map<std::string, double>* value) {
    return false;
  }

  bool GetIntProperty(const DBPropertyInfo& property_info, uint64_t* value,
                      DBImpl* db) const {
    return false;
  }

  bool GetIntPropertyOutOfMutex(const DBPropertyInfo& property_info,
                                Version* version, uint64_t* value) const {
    return false;
  }
};
#endif  // !ROCKSDB_LITE
}
}  // namespace smartengine
