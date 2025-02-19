//  Portions Copyright (c) 2023, ApeCloud Inc Holding Limited
//  Portions Copyright (c) 2020, Alibaba Group Holding Limited
//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.

#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif

#include "transactions/transaction_util.h"

#include <inttypes.h>
#include <string>

#include "db/db_impl.h"
#include "util/string_util.h"
#include "write_batch/write_batch_with_index.h"

namespace smartengine {
using namespace common;
using namespace db;

namespace util {

Status TransactionUtil::CheckKeyForConflicts(DBImpl* db_impl,
                                             ColumnFamilyHandle* column_family,
                                             const std::string& key,
                                             SequenceNumber key_seq,
                                             bool cache_only,
                                             const bool lock_uk,
                                             const ReadOptions *read_opts) {
  Status result;

  if (!lock_uk) {
    auto cfh = reinterpret_cast<ColumnFamilyHandleImpl*>(column_family);
    auto cfd = cfh->cfd();
    SuperVersion* sv = db_impl->GetAndRefSuperVersion(cfd);

    if (sv == nullptr) {
      result = Status::InvalidArgument("Could not access sub table " + std::to_string(cfh->cfd()->get_table_schema().get_index_id()));
    }else{
      sv->cfd_ = cfd;
    }

    if (result.ok()) {
      SequenceNumber earliest_seq =
        db_impl->GetEarliestMemTableSequenceNumber(sv, true);

      result = CheckKey(db_impl, sv, earliest_seq, key_seq, key, cache_only);

      db_impl->ReturnAndCleanupSuperVersion(cfd, sv);
    }
  } else {
    result = check_unique_key(db_impl, column_family, key, key_seq, read_opts);
  }
  return result;
}

Status TransactionUtil::check_unique_key(DBImpl* db_impl,
                                         ColumnFamilyHandle *column_family,
                                         const std::string &key,
                                         const SequenceNumber &key_seq,
                                         const ReadOptions *read_opts)
{
  Status ret;
  SequenceNumber scanned_seq = kMaxSequenceNumber;

  ret = Status(db_impl->get_latest_seq_for_uk(column_family, read_opts, key, scanned_seq));

  if (ret.ok() && scanned_seq > key_seq) {
    // write conflict
    ret = Status::Busy();
  } else if (ret.IsNotFound()) {
    // overwrite ret
    ret = Status::OK();
  }

  return ret;
}

Status TransactionUtil::CheckKey(DBImpl* db_impl, SuperVersion* sv,
                                 SequenceNumber earliest_seq,
                                 SequenceNumber key_seq, const std::string& key,
                                 bool cache_only) {
  Status result;
  bool need_to_read_sst = false;

  // Since it would be too slow to check the SST files, we will only use
  // the memtables to check whether there have been any recent writes
  // to this key after it was accessed in this transaction.  But if the
  // Memtables do not contain a long enough history, we must fail the
  // transaction.
  if (earliest_seq == kMaxSequenceNumber) {
    // The age of this memtable is unknown.  Cannot rely on it to check
    // for recent writes.  This error shouldn't happen often in practice as
    // the Memtable should have a valid earliest sequence number except in some
    // corner cases (such as error cases during recovery).
    need_to_read_sst = true;

    if (cache_only) {
      result = Status::TryAgain(
          "Transaction ould not check for conflicts as the MemTable does not "
          "countain a long enough history to check write at SequenceNumber: ",
          ToString(key_seq));
    }
  } else if (key_seq < earliest_seq) {
    need_to_read_sst = true;

    if (cache_only) {
      // The age of this memtable is too new to use to check for recent
      // writes.
      char msg[300];
      snprintf(msg, sizeof(msg),
               "Transaction could not check for conflicts for operation at "
               "SequenceNumber %" PRIu64
               " as the MemTable only contains changes newer than "
               "SequenceNumber %" PRIu64
               ".  Increasing the value of the "
               "max_write_buffer_number_to_maintain option could reduce the "
               "frequency "
               "of this error.",
               key_seq, earliest_seq);
      result = Status::TryAgain(msg);
    }
  }

  if (result.ok()) {
    SequenceNumber seq = kMaxSequenceNumber;
    bool found_record_for_key = false;

    Status s = db_impl->GetLatestSequenceForKey(sv, key, !need_to_read_sst,
                                                &seq, &found_record_for_key);

    if (!(s.ok() || s.IsNotFound())) {
      result = s;
    } else if (found_record_for_key && (seq > key_seq)) {
      // Write Conflict
      result = Status::Busy();
    }
  }

  return result;
}

Status TransactionUtil::CheckKeysForConflicts(DBImpl* db_impl,
                                              const TransactionKeyMap& key_map,
                                              bool cache_only) {
  Status result;

  for (auto& key_map_iter : key_map) {
    uint32_t cf_id = key_map_iter.first;
    const auto& keys = key_map_iter.second;

    SuperVersion* sv = db_impl->GetAndRefSuperVersion(cf_id);
    if (sv == nullptr) {
      result = Status::InvalidArgument("Could not access sub table " +
                                       ToString(cf_id));
      break;
    }

    SequenceNumber earliest_seq =
        db_impl->GetEarliestMemTableSequenceNumber(sv, true);

    // For each of the keys in this transaction, check to see if someone has
    // written to this key since the start of the transaction.
    for (const auto& key_iter : keys) {
      const auto& key = key_iter.first;
      const SequenceNumber key_seq = key_iter.second.seq;

      result = CheckKey(db_impl, sv, earliest_seq, key_seq, key, cache_only);

      if (!result.ok()) {
        break;
      }
    }

    db_impl->ReturnAndCleanupSuperVersion(cf_id, sv);

    if (!result.ok()) {
      break;
    }
  }

  return result;
}

}  //  namespace util
}  //  namespace smartengine