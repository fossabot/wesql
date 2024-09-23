/*
 * Portions Copyright (c) 2023, ApeCloud Inc Holding Limited
 * Copyright (c) 2020, Alibaba Group Holding Limited
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at

 * http://www.apache.org/licenses/LICENSE-2.0

 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <cstdint>
#include <vector>
#include "db/binlog_position.h"
namespace smartengine
{
using BackupSnapshotId = uint64_t;
namespace db
{
class DB;
}
namespace util
{
static const char *const BACKUP_TMP_DIR = "/hotbackup_tmp";
static const char *const BACKUP_EXTENT_IDS_FILE = "/extent_ids.inc";
static const char *const BACKUP_EXTENTS_FILE = "/extent.inc";

class BackupSnapshot
{
public:
  // Get a backup snapshot instance
  static BackupSnapshot *get_instance();
  // Check backup job and do init
  virtual int init(db::DB *db, const char *backup_tmp_dir_path = nullptr);
  // Create backup tmp dir
  virtual int create_tmp_dir(db::DB *db);
  // Cleanup backup tmp dir
  virtual int cleanup_tmp_dir(db::DB *db);
  // lock backup instance for tools like xtrabackup
  virtual int lock_instance();
  virtual int unlock_instance();
  // lock one backup step for handler interface used by server layer
  virtual int lock_one_step();
  virtual int unlock_one_step();
  virtual int check_lock_status();
  // Get backup status
  virtual int get_backup_status(const char *&status);
  // Set backup status
  virtual int set_backup_status(const char *status);
  // Do a manual checkpoint and flush memtable
  virtual int do_checkpoint(db::DB *db, const char *backup_tmp_dir_path = nullptr);
  // Acquire an backup snapshot and hard-link/copy MANIFEST files
  virtual int accquire_backup_snapshot(db::DB *db, BackupSnapshotId *backup_id, db::BinlogPosition &binlog_pos);
  // Parse incremental MANIFEST files and record the modified extent ids
  virtual int record_incremental_extent_ids(db::DB *db);
  // Release an old backup snapshot
  virtual int release_old_backup_snapshot(db::DB *db, BackupSnapshotId backup_id);
  // List all backup snapshots and return the backup ids
  virtual int list_backup_snapshots(std::vector<BackupSnapshotId> &backup_ids);
  // Release the current backup snapshot
  virtual int release_current_backup_snapshot(db::DB *db);
  // Get temporary current backup id of the backup instance
  virtual BackupSnapshotId current_backup_id();

protected:
  BackupSnapshot() {}
  virtual ~BackupSnapshot() {}
};

} // namespace util
} // namespace smartengine