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

#include "table/block_struct.h"
#include "table/bloom_filter.h"
#include "logger/log_module.h"
#include "storage/io_extent.h"
#include "table/row_block.h"
#include "util/compress/compressor_helper.h"
#include "util/crc32c.h"
#include "util/se_constants.h"

namespace smartengine
{
using namespace common;
using namespace memory;
using namespace storage;
using namespace util;

namespace table
{

BlockHandle::BlockHandle()
    : offset_(0),
      size_(0),
      raw_size_(0),
      checksum_(0),
      compress_type_(kNoCompression)
{}

BlockHandle::BlockHandle(const BlockHandle &block_handle)
    : offset_(block_handle.offset_),
      size_(block_handle.size_),
      raw_size_(block_handle.raw_size_),
      checksum_(block_handle.checksum_),
      compress_type_(block_handle.compress_type_)
{}

BlockHandle::~BlockHandle() {}

BlockHandle &BlockHandle::operator=(const BlockHandle &block_handle)
{
  offset_ = block_handle.offset_;
  size_ = block_handle.size_;
  raw_size_ = block_handle.raw_size_;
  checksum_ = block_handle.checksum_;
  compress_type_ = block_handle.compress_type_;

  return *this;
}

void BlockHandle::reset()
{
  offset_ = 0;
  size_ = 0;
  raw_size_ = 0;
  checksum_ = 0;
  compress_type_ = kNoCompression;
}

bool BlockHandle::is_valid() const
{
  return offset_ >= 0 && size_ > 0 && raw_size_ > 0 &&
         ((kNoCompression != compress_type_ && (raw_size_ > size_)) ||
          (kNoCompression == compress_type_ && (raw_size_ == size_)));
}

int BlockHandle::serialize(char *buf, int64_t buf_len, int64_t &pos) const
{
  int ret = Status::kOk;
  int32_t header_size = get_serialize_size();
  int32_t header_version = BLOCK_HANDLE_VERSION;
  int8_t compress_type_value = static_cast<int8_t>(compress_type_);

  if (IS_NULL(buf) || UNLIKELY(buf_len <= 0) || UNLIKELY(pos < 0) || UNLIKELY(pos >= buf_len) ) {
    ret = Status::kInvalidArgument;
    SE_LOG(WARN, "invalid argument", K(ret), KP(buf), K(buf_len), K(pos));
  } else if (FAILED(util::encode_fixed_int32(buf, buf_len, pos, header_size))) {
    SE_LOG(WARN, "fail to encode header size", K(ret), K(header_size));
  } else if (FAILED(util::encode_fixed_int32(buf, buf_len, pos, header_version))) {
    SE_LOG(WARN, "fail to encode header version", K(ret), K(header_version));
  } else if (FAILED(util::encode_fixed_int32(buf, buf_len, pos, offset_))) {
    SE_LOG(WARN, "fail to encode offset", K(ret), K_(offset));
  } else if (FAILED(util::serialize(buf, buf_len, pos, size_))) {
    SE_LOG(WARN, "fail to serialize size", K(ret), K_(size));
  } else if (FAILED(util::serialize(buf, buf_len, pos, raw_size_))) {
    SE_LOG(WARN, "fail to serialize raw size", K(ret), K_(raw_size));
  } else if (FAILED(util::serialize(buf, buf_len, pos, checksum_))) {
    SE_LOG(WARN, "fail to serialize checksum", K(ret), K_(checksum));
  } else if (FAILED(encode_fixed_int8(buf, buf_len, pos, compress_type_value))) {
    SE_LOG(WARN, "fail to serilize compress type", K(ret), KE_(compress_type));
  } else {
    // succeed
  }

  return ret;
}

int BlockHandle::deserialize(const char *buf, int64_t buf_len, int64_t &pos)
{
  int ret = Status::kOk;
  int32_t header_size = 0;
  int32_t header_version = 0;
  int8_t compress_type_value = 0;

  if (IS_NULL(buf) || UNLIKELY(buf_len <= 0) || UNLIKELY(pos < 0) || UNLIKELY(pos >= buf_len)) {
    ret = Status::kInvalidArgument;
    SE_LOG(WARN, "invalid argument", K(ret), KP(buf), K(buf_len), K(pos));
  } else if (FAILED(util::decode_fixed_int32(buf, buf_len, pos, header_size))) {
    SE_LOG(WARN, "fail to decode header size", K(ret));
  } else if (FAILED(util::decode_fixed_int32(buf, buf_len, pos, header_version))) {
    SE_LOG(WARN, "fail to decode header version", K(ret));
  } else if (FAILED(util::decode_fixed_int32(buf, buf_len, pos, offset_))) {
    SE_LOG(WARN, "fail to deserialize offset", K(ret));
  } else if (FAILED(util::deserialize(buf, buf_len, pos, size_))) {
    SE_LOG(WARN, "fail to deserialize size", K(ret));
  } else if (FAILED(util::deserialize(buf, buf_len, pos, raw_size_))) {
    SE_LOG(WARN, "fail to deserialize raw size", K(ret));
  } else if (FAILED(util::deserialize(buf, buf_len, pos, checksum_))) {
    SE_LOG(WARN, "fail to deserialize checksum", K(ret));
  } else if (FAILED(util::decode_fixed_int8(buf, buf_len, pos, compress_type_value))) {
    SE_LOG(WARN, "fail to decode compress type", K(ret));
  } else {
    compress_type_ = static_cast<CompressionType>(compress_type_value);
  }

  return ret;
}

int64_t BlockHandle::get_serialize_size() const
{
  // header size and header version
  int64_t size = sizeof(int32_t) + sizeof(int32_t);

  size += sizeof(int32_t); // offset_
  size += util::get_serialize_size(size_);
  size += util::get_serialize_size(raw_size_);
  size += util::get_serialize_size(checksum_);
  size += sizeof(int8_t); // compress type

  return size;
}

DEFINE_TO_STRING(BlockHandle, KV_(offset), KV_(size), KV_(raw_size), KV_(checksum), KVE_(compress_type))

BlockInfo::BlockInfo()
    : handle_(),
      first_key_(),
      row_count_(0),
      delete_row_count_(0),
      single_delete_row_count_(0),
      smallest_seq_(common::kMaxSequenceNumber),
      largest_seq_(0),
      attr_(0),
      per_key_bits_(0),
      probe_num_(0),
      bloom_filter_(),
      unit_infos_()
{}

BlockInfo::BlockInfo(const BlockInfo &block_info)
    : handle_(block_info.handle_),
      first_key_(block_info.first_key_),
      row_count_(block_info.row_count_),
      delete_row_count_(block_info.delete_row_count_),
      single_delete_row_count_(block_info.single_delete_row_count_),
      smallest_seq_(block_info.smallest_seq_),
      largest_seq_(block_info.largest_seq_),
      attr_(block_info.attr_),
      per_key_bits_(block_info.per_key_bits_),
      probe_num_(block_info.probe_num_),
      bloom_filter_(block_info.bloom_filter_),
      unit_infos_(block_info.unit_infos_)
{}

BlockInfo::~BlockInfo() {}

BlockInfo &BlockInfo::operator=(const BlockInfo &block_info)
{
  handle_ = block_info.handle_;
  first_key_ = block_info.first_key_;
  row_count_ = block_info.row_count_;
  delete_row_count_ = block_info.delete_row_count_;
  single_delete_row_count_ = block_info.single_delete_row_count_;
  smallest_seq_ = block_info.smallest_seq_;
  largest_seq_ = block_info.largest_seq_;
  attr_ = block_info.attr_;
  per_key_bits_ = block_info.per_key_bits_;
  probe_num_ = block_info.probe_num_;
  bloom_filter_ = block_info.bloom_filter_;
  unit_infos_ = block_info.unit_infos_;

  return *this;
}

void BlockInfo::reset()
{
  handle_.reset();
  first_key_.clear();
  row_count_ = 0;
  delete_row_count_ = 0;
  single_delete_row_count_ = 0;
  smallest_seq_ = common::kMaxSequenceNumber;
  largest_seq_ = 0;
  attr_ = 0;
  per_key_bits_ = 0;
  probe_num_ = 0;
  bloom_filter_.clear();
  unit_infos_.clear();
}

bool BlockInfo::is_valid() const
{
  return handle_.is_valid() && !first_key_.empty() && row_count_ > 0 &&
         delete_row_count_ >= 0 && single_delete_row_count_ >= 0 &&
         (row_count_ >= (delete_row_count_ + single_delete_row_count_)) &&
         (!has_bloom_filter() || (has_bloom_filter() && per_key_bits_ > 0 && probe_num_ > 0 && !bloom_filter_.empty())) &&
         (!is_columnar_format() || (is_columnar_format() && !unit_infos_.empty()));
}

void BlockInfo::set_bloom_filter(const Slice &bloom_filter)
{
  bloom_filter_ = bloom_filter;
  set_has_bloom_filter();
}

int64_t BlockInfo::get_row_format_raw_size() const
{
  int64_t raw_size = 0;

  if (is_columnar_format()) {
    for (auto &unit_info : unit_infos_) {
      raw_size += unit_info.raw_data_size_;
    }
  } else {
    raw_size = handle_.get_raw_size();
  }

  return raw_size;
}

int BlockInfo::serialize(char *buf, int64_t buf_len, int64_t &pos) const
{
  int ret = Status::kOk;
  int32_t header_size = get_serialize_size();
  int32_t header_version = BLOCK_INFO_VERSION;

  if (IS_NULL(buf) || UNLIKELY(buf_len <= 0) || UNLIKELY(pos < 0) || UNLIKELY(pos >= buf_len)) {
    ret = Status::kInvalidArgument;
    SE_LOG(WARN, "invalid argument", K(ret), KP(buf), K(buf_len), K(pos));
  } else if (FAILED(util::encode_fixed_int32(buf, buf_len, pos, header_size))) {
    SE_LOG(WARN, "fail to encode header size", K(ret), K(header_size));
  } else if (FAILED(util::encode_fixed_int32(buf, buf_len, pos, header_version))) {
    SE_LOG(WARN, "fail to encode header version", K(ret), K(header_version));
  } else if (FAILED(handle_.serialize(buf, buf_len, pos))) {
    SE_LOG(WARN, "fail to serialize handle", K(ret), K_(handle));
  } else if (FAILED(util::serialize(buf, buf_len, pos, first_key_))) {
    SE_LOG(WARN, "fail to serialize first key", K(ret), K_(first_key));
  } else if (FAILED(util::serialize(buf, buf_len, pos, row_count_))) {
    SE_LOG(WARN, "fail to serialize row count", K(ret), K_(row_count));
  } else if (FAILED(util::serialize(buf, buf_len, pos, delete_row_count_))) {
    SE_LOG(WARN, "fail to serialize row count", K(ret), K_(delete_row_count));
  } else if (FAILED(util::serialize(buf, buf_len, pos, single_delete_row_count_))) {
    SE_LOG(WARN, "fail to serialize single delete row count", K(ret), K_(single_delete_row_count));
  } else if (FAILED(util::serialize(buf, buf_len, pos, smallest_seq_))) {
    SE_LOG(WARN, "fail to serialize smallest sequence", K(ret), K_(smallest_seq));
  } else if (FAILED(util::serialize(buf, buf_len, pos, largest_seq_))) {
    SE_LOG(WARN, "fail to serialize largest sequence", K(ret), K_(largest_seq));
  } else if (FAILED(util::serialize(buf, buf_len, pos, attr_))) {
    SE_LOG(WARN, "fail to serialize attr", K(ret), K_(attr));
  } else {
    if (SUCCED(ret) && has_bloom_filter()) {
      if (FAILED(util::serialize(buf, buf_len, pos, per_key_bits_))) {
        SE_LOG(WARN, "fail to serialize per_key_bits", K(ret), K_(per_key_bits));
      } else if (FAILED(util::serialize(buf, buf_len, pos, probe_num_))) {
        SE_LOG(WARN, "fail to serialize probe_num", K(ret));
      } else if (FAILED(util::serialize(buf, buf_len, pos, bloom_filter_))) {
        SE_LOG(WARN, "fail to serialize bloom filter", K(ret), K_(bloom_filter));
      }
    }

    if (SUCCED(ret) && is_columnar_format()) {
      if (FAILED(util::serialize_v(buf, buf_len, pos, unit_infos_))) {
        SE_LOG(WARN, "fail to serialize unit infos", K(ret));
      }
    }
  }

  return ret;
}

int BlockInfo::deserialize(const char *buf, int64_t buf_len, int64_t &pos)
{
  int ret = Status::kOk;
  int32_t header_size = 0;
  int32_t header_version = 0;

  if (IS_NULL(buf) || UNLIKELY(buf_len <= 0) || UNLIKELY(pos < 0) || UNLIKELY(pos >= buf_len)) {
    ret = Status::kInvalidArgument;
    SE_LOG(WARN, "invalid argument", K(ret), KP(buf), K(buf_len), K(pos));
  } else if (FAILED(util::decode_fixed_int32(buf, buf_len, pos, header_size))) {
    SE_LOG(WARN, "fail to decode header size", K(ret));
  } else if (FAILED(util::decode_fixed_int32(buf, buf_len, pos, header_version))) {
    SE_LOG(WARN, "fail to decode header version", K(ret));
  } else if (FAILED(handle_.deserialize(buf, buf_len, pos))) {
    SE_LOG(WARN, "fail to deserialize handle", K(ret));
  } else if (FAILED(util::deserialize(buf, buf_len, pos, first_key_))) {
    SE_LOG(WARN, "fail to deserialize first key", K(ret));
  } else if (FAILED(util::deserialize(buf, buf_len, pos, row_count_))) {
    SE_LOG(WARN, "fail to deserialize row count", K(ret));
  } else if (FAILED(util::deserialize(buf, buf_len, pos, delete_row_count_))) {
    SE_LOG(WARN, "fail to deserialize single delete row count", K(ret));
  } else if (FAILED(util::deserialize(buf, buf_len, pos, single_delete_row_count_))) {
    SE_LOG(WARN, "fail to deserialize single delete row count", K(ret));
  } else if (FAILED(util::deserialize(buf, buf_len, pos, smallest_seq_))) {
    SE_LOG(WARN, "fail to deserialize smallest sequence", K(ret));
  } else if (FAILED(util::deserialize(buf, buf_len, pos, largest_seq_))) {
    SE_LOG(WARN, "fail to deserialize largest sequence", K(ret));
  } else if (FAILED(util::deserialize(buf, buf_len, pos, attr_))) {
    SE_LOG(WARN, "fail to deserialize attr", K(ret));
  } else {
    if (SUCCED(ret) && has_bloom_filter()) {
      if (FAILED(util::deserialize(buf, buf_len, pos, per_key_bits_))) {
        SE_LOG(WARN, "fail to deserialize per key bits", K(ret));
      } else if (FAILED(util::deserialize(buf, buf_len, pos, probe_num_))) {
        SE_LOG(WARN, "fail to deserialize probe num", K(ret));
      } else if (FAILED(util::deserialize(buf, buf_len, pos, bloom_filter_))) {
        SE_LOG(WARN, "fail to deserialize bloom filter", K(ret));
      }
    }

    if (SUCCED(ret) && is_columnar_format()) {
      if (FAILED(util::deserialize_v(buf, buf_len, pos, unit_infos_))) {
        SE_LOG(WARN, "fail to deserialize unit infos", K(ret));
      }
    }
  }

  return ret;
}

int64_t BlockInfo::get_serialize_size() const
{
  // header size and header version
  int64_t size = sizeof(int32_t) + sizeof(int32_t);
  size += handle_.get_serialize_size(); // handle_
  size += util::get_serialize_size(first_key_);
  size += util::get_serialize_size(row_count_);
  size += util::get_serialize_size(delete_row_count_);
  size += util::get_serialize_size(single_delete_row_count_);
  size += util::get_serialize_size(smallest_seq_);
  size += util::get_serialize_size(largest_seq_);
  size += util::get_serialize_size(attr_);
  if (has_bloom_filter()) {
    size += util::get_serialize_size(per_key_bits_);
    size += util::get_serialize_size(probe_num_);
    size += util::get_serialize_size(bloom_filter_);
  }
  if (is_columnar_format()) {
    size += util::get_serialize_v_size(unit_infos_);
  }

  return size;
}

DEFINE_TO_STRING(BlockInfo, KV_(handle), KV_(first_key), KV_(handle), KV_(row_count),
    KV_(delete_row_count), KV_(single_delete_row_count), KV_(smallest_seq), KV_(per_key_bits),
    KV_(attr), KV_(probe_num), KV_(bloom_filter), KV_(largest_seq), K_(unit_infos))

} // namespace table
} // namespace smartengine