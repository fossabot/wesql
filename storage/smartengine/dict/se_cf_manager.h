/*
   Portions Copyright (c) 2023, ApeCloud Inc Holding Limited
   Portions Copyright (c) 2020, Alibaba Group Holding Limited
   Copyright (c) 2014, SkySQL Ab

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

#pragma once

/* C++ system header files */
#include <map>
#include <string>
#include <vector>

#include "dict/se_cf_options.h"
#include "dict/se_log_ddl.h"
#include "db/column_family.h"
#include "db/db.h"

namespace smartengine
{
class DB;
class ColumnFamilyHandle;

/*
  We need a Column Family (CF) manager. Its functions:
  - create column families (synchronized, don't create the same twice)
  - keep count in each column family.
     = the count is kept on-disk.
     = there are no empty CFs. initially count=1.
     = then, when doing DDL, we increase or decrease it.
       (atomicity is maintained by being in the same WriteBatch with DDLs)
     = if DROP discovers that now count=0, it removes the CF.

  Current state is:
  - CFs are created in a synchronized way. We can't remove them, yet.
*/

class SeSubtableManager
{
  std::map<uint32_t, db::ColumnFamilyHandle *> m_subtable_id_map;

  mutable mysql_mutex_t m_mutex;

  static void get_per_index_cf_name(const std::string &db_table_name,
                                    const char *const index_name,
                                    std::string *const res);

  SeSubtableOptions *m_cf_options = nullptr;

  SeDdlLogManager *m_ddl_log_manager = nullptr;

public:
  SeSubtableManager(const SeSubtableManager &) = delete;
  SeSubtableManager &operator=(const SeSubtableManager &) = delete;
  SeSubtableManager() = default;

  static bool is_cf_name_reverse(const char *const name);

  /*
    This is called right after the DB::Open() call. The parameters describe
    column
    families that are present in the database. The first CF is the default CF.
  */
  void init(SeSubtableOptions *cf_options,
            std::vector<db::ColumnFamilyHandle *> *const handles,
            SeDdlLogManager *ddl_log_manager);
  void cleanup();

  /*
    Used by CREATE TABLE.
    - cf_name=nullptr means use default column family
    - cf_name=_auto_ means use 'dbname.tablename.indexname'
  */
  db::ColumnFamilyHandle *
  get_or_create_cf(db::DB *const se_db,
                   db::WriteBatch *write_batch,
                   ulong thread_id,
                   uint subtable_id,
                   const char *cf_name,
                   const std::string &db_table_name,
                   const char *const index_name,
                   bool *const is_automatic,
                   const common::ColumnFamilyOptions &cf_options,
                   bool create_table_space,
                   int64_t &table_space_id);

  /** create subtable physically */
  bool create_subtable(db::DB *const se_db,
                       db::WriteBatch *xa_batch,
                       ulong thread_id,
                       uint index_number,
                       const common::ColumnFamilyOptions &cf_options,
                       const char *subtable_name,
                       bool create_table_space,
                       int64_t &table_space_id,
                       db::ColumnFamilyHandle **cf_handle);
  /* Used by table open */
  db::ColumnFamilyHandle *get_cf(const char *cf_name,
                                 const std::string &db_table_name,
                                 const char *const index_name,
                                 bool *const is_automatic) const;

  /* Look up cf by id; used by datadic */
  db::ColumnFamilyHandle *get_cf(const uint32_t &id) const;

  /* Used to iterate over column families for show status */
  //std::vector<std::string> get_cf_names(void) const;
  
  /* Used to iterate over column families for show status */
  std::vector<int32_t> get_subtable_ids(void) const;

  /* Used to iterate over column families */
  std::vector<db::ColumnFamilyHandle *> get_all_cf(void) const;

  /* drop column family if necessary */
  void drop_cf(db::DB *const se_db, const uint32_t cf_id);

  void get_cf_options(const std::string &cf_name, common::ColumnFamilyOptions *const opts)
  {
    m_cf_options->get_cf_options(cf_name, opts);
  }
};

} //namespace smartengine
