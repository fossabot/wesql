//  Portions Copyright (c) 2023, ApeCloud Inc Holding Limited
// Portions Copyright (c) 2020, Alibaba Group Holding Limited
// Copyright (c) 2011-present, Facebook, Inc. All rights reserved.
// This source code is licensed under the BSD-style license found in the
// LICENSE file in the root directory of this source tree. An additional grant
// of patent rights can be found in the PATENTS file in the same directory.

#ifndef ROCKSDB_LITE
#include "table/cuckoo_table_factory.h"

#include "db/dbformat.h"
#include "table/cuckoo_table_builder.h"
#include "table/cuckoo_table_reader.h"

using namespace smartengine;
using namespace common;
using namespace util;

namespace smartengine {
namespace table {

Status CuckooTableFactory::NewTableReader(
    const TableReaderOptions& table_reader_options,
    RandomAccessFileReader *file, uint64_t file_size,
    TableReader *&table,
    bool prefetch_index_and_filter_in_cache,
    memory::SimpleAllocator *arena) const {
  UNUSED(arena);
  // no use now, just fit func's arguments, need update if use one day
  unique_ptr<RandomAccessFileReader> file_ptr(file);
  std::unique_ptr<CuckooTableReader> new_reader(new CuckooTableReader(
      table_reader_options.ioptions, std::move(file_ptr), file_size,
      table_reader_options.internal_comparator.user_comparator(), nullptr));
  Status s = new_reader->status();
  if (s.ok()) {
//    *table = std::move(new_reader);
    table = new_reader.release();
  }
  return s;
}

TableBuilder* CuckooTableFactory::NewTableBuilder(
    const TableBuilderOptions& table_builder_options, uint32_t column_family_id,
    WritableFileWriter* file) const {
  // Ignore the skipFIlters flag. Does not apply to this file format
  //

  // TODO: change builder to take the option struct
  return new CuckooTableBuilder(
      file, table_options_.hash_table_ratio, 64,
      table_options_.max_search_depth,
      table_builder_options.internal_comparator.user_comparator(),
      table_options_.cuckoo_block_size, table_options_.use_module_hash,
      table_options_.identity_as_first_hash, nullptr /* get_slice_hash */,
      column_family_id, table_builder_options.column_family_name);
}

std::string CuckooTableFactory::GetPrintableTableOptions() const {
  std::string ret;
  ret.reserve(2000);
  const int kBufferSize = 200;
  char buffer[kBufferSize];

  snprintf(buffer, kBufferSize, "  hash_table_ratio: %lf\n",
           table_options_.hash_table_ratio);
  ret.append(buffer);
  snprintf(buffer, kBufferSize, "  max_search_depth: %u\n",
           table_options_.max_search_depth);
  ret.append(buffer);
  snprintf(buffer, kBufferSize, "  cuckoo_block_size: %u\n",
           table_options_.cuckoo_block_size);
  ret.append(buffer);
  snprintf(buffer, kBufferSize, "  identity_as_first_hash: %d\n",
           table_options_.identity_as_first_hash);
  ret.append(buffer);
  return ret;
}

TableFactory* NewCuckooTableFactory(const CuckooTableOptions& table_options) {
  return new CuckooTableFactory(table_options);
}

}  // namespace table
}  // namespace smartengine
#endif  // ROCKSDB_LITE
