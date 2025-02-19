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

#include "util/file_reader_writer.h"

#include <algorithm>
#include <mutex>

#include "monitoring/iostats_context_imp.h"
#include "util/rate_limiter.h"
#include "util/sync_point.h"
#include "util/aio_wrapper.h"
#include "util/misc_utility.h"

namespace smartengine {
using namespace monitor;
using namespace port;
using namespace common;
using namespace memory;

namespace util {

SequentialFileReader::~SequentialFileReader() {
  if (use_allocator_) {
    file_->~SequentialFile();
  } else {
    MOD_DELETE_OBJECT(SequentialFile, file_);
  }
}

Status SequentialFileReader::Read(size_t n, Slice* result, char* scratch) {
  Status s;
  if (use_direct_io()) {
    size_t offset = offset_.fetch_add(n);
    size_t alignment = file_->GetRequiredBufferAlignment();
    size_t aligned_offset = TruncateToPageBoundary(alignment, offset);
    size_t offset_advance = offset - aligned_offset;
    size_t size = Roundup(offset + n, alignment) - aligned_offset;
    size_t r = 0;
    AlignedBuffer buf;
    buf.Alignment(alignment);
    buf.AllocateNewBuffer(size);
    Slice tmp;
    s = file_->PositionedRead(aligned_offset, size, &tmp, buf.BufferStart());
    if (s.ok() && offset_advance < tmp.size()) {
      buf.Size(tmp.size());
      r = buf.Read(scratch, offset_advance,
                   std::min(tmp.size() - offset_advance, n));
    }
    *result = Slice(scratch, r);
  } else {
    s = file_->Read(n, result, scratch);
  }
  IOSTATS_ADD(bytes_read, result->size());
  return s;
}

int SequentialFileReader::prefetch(size_t n, AIOHandle *aio_handle) {
  int ret = Status::kOk;
  if (IS_NULL(aio_handle)) {
    SE_LOG(WARN, "aio handle is null");
    ret = Status::kInvalidArgument;
  } else {
    size_t offset = prefetch_offset_.fetch_add(n);
    AIOInfo aio_info;
    if (FAILED(file_->fill_aio_info(offset, n, aio_info))) {
      SE_LOG(WARN, "failed to fill aio info", K(ret), K(offset), K(n));
    } else if (FAILED(aio_handle->prefetch(aio_info))) {
      SE_LOG(WARN, "aio handle prefetch failed", K(ret), K(aio_info));
    }
    if (FAILED(ret)) {
      aio_handle->aio_req_->status_ = Status::kErrorUnexpected;
    }
  }
  return ret;
}

int SequentialFileReader::read(size_t n, common::Slice* result,
                                  char* scratch, AIOHandle *aio_handle) {
  int ret = Status::kOk;
  if (IS_NULL(aio_handle)) {
    SE_LOG(WARN, "aio handle is null");
    ret = Status::kInvalidArgument;
  } else {
    auto offset = offset_.fetch_add(n);
    AIOInfo aio_info;
    if (FAILED(file_->fill_aio_info(offset, n, aio_info))) {
      SE_LOG(WARN, "failed to fill io info", K(ret), K(offset), K(n));
    }  else if (FAILED(aio_handle->read_by_actual_size(aio_info.offset_,
                                            aio_info.size_, result, scratch))) {
      SE_LOG(WARN, "aio handle read failed", K(ret), K(aio_info));
    }
  }
  IOSTATS_ADD(bytes_read, result->size());
  return ret;
}

Status SequentialFileReader::Skip(uint64_t n) {
  if (use_direct_io()) {
    offset_ += n;
    return Status::OK();
  }
  return file_->Skip(n);
}

RandomAccessFileReader::RandomAccessFileReader(RandomAccessFile *raf, bool use_allocator)
    : file_(raf),
      use_allocator_(use_allocator)
{}

RandomAccessFileReader::~RandomAccessFileReader()
{
  if (use_allocator_) {
    file_->~RandomAccessFile();
    file_ = nullptr;
  } else {
    MOD_DELETE_OBJECT(RandomAccessFile, file_);
  }
}

RandomAccessFileReader &RandomAccessFileReader::operator=(RandomAccessFileReader&& o) noexcept
{
  file_ = o.release_file();
  return *this;
}

Status RandomAccessFileReader::Read(uint64_t offset,
                                    size_t n,
                                    Slice* result,
                                    char* scratch) const
{
  Status s;
  uint64_t elapsed = 0;
  {
    IOSTATS_TIMER_GUARD(read_nanos);
    if (use_direct_io()) {
      size_t alignment = file_->GetRequiredBufferAlignment();
      size_t aligned_offset = TruncateToPageBoundary(alignment, offset);
      size_t offset_advance = offset - aligned_offset;
      size_t size = Roundup(offset + n, alignment) - aligned_offset;
      size_t r = 0;
      AlignedBuffer buf;
      buf.Alignment(alignment);
      buf.AllocateNewBuffer(size);
      Slice tmp;
      s = file_->Read(aligned_offset, size, &tmp, buf.BufferStart());
      if (s.ok() && offset_advance < tmp.size()) {
        buf.Size(tmp.size());
        r = buf.Read(scratch, offset_advance,
                     std::min(tmp.size() - offset_advance, n));
      }
      *result = Slice(scratch, r);
    } else {
      s = file_->Read(offset, n, result, scratch);
    }
    IOSTATS_ADD_IF_POSITIVE(bytes_read, result->size());
  }
  return s;
}

int RandomAccessFileReader::read(int64_t offset,
                                 int64_t size,
                                 common::Slice *result,
                                 char *scratch,
                                 AIOHandle *aio_handle)
{
  int ret = Status::kOk;
  if (nullptr != aio_handle) {
    // try aio
    AIOInfo aio_info;
    if (FAILED(file_->fill_aio_info(offset, size, aio_info))) {
      SE_LOG(WARN, "failed to fill io info", K(ret), K(offset), K(size));
    } else if (FAILED(aio_handle->read(aio_info.offset_, aio_info.size_, result, scratch))) {
      SE_LOG(WARN, "aio handle read failed", K(ret), K(offset), K(size), K(aio_info));
      BACKTRACE(ERROR, "aio handle read failed");
    }
  }
  // use sync IO or aio read failed
  if (nullptr == aio_handle || FAILED(ret)) {
    // overwrite ret
    ret = Read(offset, size, result, scratch).code();
  }
  return ret;
}

int RandomAccessFileReader::prefetch(const int64_t offset,
                                     const int64_t size,
                                     AIOHandle *aio_handle)
{
  int ret = Status::kOk;
  if (IS_NULL(aio_handle)) {
    ret = Status::kInvalidArgument;
    SE_LOG(WARN, "aio handle is nullptr", K(ret));
  } else {
    AIOInfo aio_info;
    if (FAILED(file_->fill_aio_info(offset, size, aio_info))) {
      SE_LOG(WARN, "failed to fill io info", K(ret), K(offset), K(size));
    } else if (FAILED(aio_handle->prefetch(aio_info))) {
      SE_LOG(WARN, "aio handle prefetch failed", K(ret), K(aio_info));
    }
    if (FAILED(ret)) {
      // prefetch failed, will try sync IO, overwrite ret
      ret = Status::kOk;
      aio_handle->aio_req_->status_ = Status::kErrorUnexpected;
      SE_LOG(INFO, "aio prefetch failed, will try sync IO!");
    }
  }
  return ret;
}

RandomAccessFile* RandomAccessFileReader::release_file()
{
  auto rfile = file_;
  file_ = nullptr;
  return rfile;
}

Status WritableFileWriter::Append(const Slice& data) {
  const char* src = data.data();
  size_t left = data.size();
  Status s;
  pending_sync_ = true;

  TEST_KILL_RANDOM("WritableFileWriter::Append:0",
                   rocksdb_kill_odds * REDUCE_ODDS2);

  {
    IOSTATS_TIMER_GUARD(prepare_write_nanos);
    TEST_SYNC_POINT("WritableFileWriter::Append:BeforePrepareWrite");
    writable_file_->PrepareWrite(static_cast<size_t>(GetFileSize()), left);
  }

  // Flush only when buffered I/O
  if (!use_direct_io() && (buf_.Capacity() - buf_.CurrentSize()) < left) {
    if (buf_.CurrentSize() > 0) {
      s = Flush();
      if (!s.ok()) {
        return s;
      }
    }

    if (buf_.Capacity() < max_buffer_size_) {
      size_t desiredCapacity = buf_.Capacity() * 2;
      desiredCapacity = std::min(desiredCapacity, max_buffer_size_);
      buf_.AllocateNewBuffer(desiredCapacity);
    }
    assert(buf_.CurrentSize() == 0);
  }

  // We never write directly to disk with direct I/O on.
  // or we simply use it for its original purpose to accumulate many small
  // chunks
  if (use_direct_io() || (buf_.Capacity() >= left)) {
    while (left > 0) {
      size_t appended = buf_.Append(src, left);
      left -= appended;
      src += appended;

      if (left > 0) {
        s = Flush();
        if (!s.ok()) {
          break;
        }

        // We double the buffer here because
        // Flush calls do not keep up with the incoming bytes
        // This is the only place when buffer is changed with direct I/O
        if (buf_.Capacity() < max_buffer_size_) {
          size_t desiredCapacity = buf_.Capacity() * 2;
          desiredCapacity = std::min(desiredCapacity, max_buffer_size_);
          buf_.AllocateNewBuffer(desiredCapacity);
        }
      }
    }
  } else {
    // Writing directly to file bypassing the buffer
    assert(buf_.CurrentSize() == 0);
    s = WriteBuffered(src, left);
  }

  TEST_KILL_RANDOM("WritableFileWriter::Append:1", rocksdb_kill_odds);
  if (s.ok()) {
    filesize_ += data.size();
  }
  return s;
}

Status WritableFileWriter::Close() {
  // Do not quit immediately on failure the file MUST be closed
  Status s;

  // Possible to close it twice now as we MUST close
  // in __dtor, simply flushing is not enough
  // Windows when pre-allocating does not fill with zeros
  // also with unbuffered access we also set the end of data.
  if (!writable_file_) {
    return s;
  }

  s = Flush();  // flush cache to OS

  Status interim;
  // In direct I/O mode we write whole pages so
  // we need to let the file know where data ends.
  if (use_direct_io()) {
    interim = writable_file_->Truncate(filesize_);
    if (!interim.ok() && s.ok()) {
      s = interim;
    }
  }

  TEST_KILL_RANDOM("WritableFileWriter::Close:0", rocksdb_kill_odds);
  interim = writable_file_->Close();
  if (!interim.ok() && s.ok()) {
    s = interim;
  }

//  writable_file_.reset();
  if (!use_allocator_) {
    MOD_DELETE_OBJECT(WritableFile, writable_file_);
  } else { // use allocator
    writable_file_->~WritableFile();
  }
  TEST_KILL_RANDOM("WritableFileWriter::Close:1", rocksdb_kill_odds);

  return s;
}

// write out the cached data to the OS cache or storage if direct I/O
// enabled
Status WritableFileWriter::Flush() {
  Status s;
  TEST_KILL_RANDOM("WritableFileWriter::Flush:0",
                   rocksdb_kill_odds * REDUCE_ODDS2);

  if (buf_.CurrentSize() > 0) {
    if (use_direct_io()) {
      s = WriteDirect();
    } else {
      s = WriteBuffered(buf_.BufferStart(), buf_.CurrentSize());
    }
    if (!s.ok()) {
      return s;
    }
  }

  s = writable_file_->Flush();

  if (!s.ok()) {
    return s;
  }

  // sync OS cache to disk for every bytes_per_sync_
  // TODO: give log file and sst file different options (log
  // files could be potentially cached in OS for their whole
  // life time, thus we might not want to flush at all).

  // We try to avoid sync to the last 1MB of data. For two reasons:
  // (1) avoid rewrite the same page that is modified later.
  // (2) for older version of OS, write can block while writing out
  //     the page.
  // Xfs does neighbor page flushing outside of the specified ranges. We
  // need to make sure sync range is far from the write offset.
  if (!use_direct_io() && bytes_per_sync_) {
    const uint64_t kBytesNotSyncRange =
        1024 * 1024;                                // recent 1MB is not synced.
    const uint64_t kBytesAlignWhenSync = 4 * 1024;  // Align 4KB.
    if (filesize_ > kBytesNotSyncRange) {
      uint64_t offset_sync_to = filesize_ - kBytesNotSyncRange;
      offset_sync_to -= offset_sync_to % kBytesAlignWhenSync;
      assert(offset_sync_to >= last_sync_size_);
      if (offset_sync_to > 0 &&
          offset_sync_to - last_sync_size_ >= bytes_per_sync_) {
        s = RangeSync(last_sync_size_, offset_sync_to - last_sync_size_);
        last_sync_size_ = offset_sync_to;
      }
    }
  }

  return s;
}

Status WritableFileWriter::Sync(bool use_fsync) {
  Status s = Flush();
  if (!s.ok()) {
    return s;
  }
  TEST_KILL_RANDOM("WritableFileWriter::Sync:0", rocksdb_kill_odds);
  if (!use_direct_io() && pending_sync_) {
    s = SyncInternal(use_fsync);
    if (!s.ok()) {
      return s;
    }
  }
  TEST_KILL_RANDOM("WritableFileWriter::Sync:1", rocksdb_kill_odds);
  pending_sync_ = false;
  return Status::OK();
}

Status WritableFileWriter::SyncWithoutFlush(bool use_fsync) {
  if (!writable_file_->IsSyncThreadSafe()) {
    return Status::NotSupported(
        "Can't WritableFileWriter::SyncWithoutFlush() because "
        "WritableFile::IsSyncThreadSafe() is false");
  }
  TEST_SYNC_POINT("WritableFileWriter::SyncWithoutFlush:1");
  Status s = SyncInternal(use_fsync);
  TEST_SYNC_POINT("WritableFileWriter::SyncWithoutFlush:2");
  return s;
}

Status WritableFileWriter::SyncInternal(bool use_fsync) {
  Status s;
  IOSTATS_TIMER_GUARD(fsync_nanos);
  TEST_SYNC_POINT("WritableFileWriter::SyncInternal:0");
  if (use_fsync) {
    s = writable_file_->Fsync();
  } else {
    s = writable_file_->Sync();
  }
  return s;
}

Status WritableFileWriter::RangeSync(uint64_t offset, uint64_t nbytes) {
  IOSTATS_TIMER_GUARD(range_sync_nanos);
  TEST_SYNC_POINT("WritableFileWriter::RangeSync:0");
  return writable_file_->RangeSync(offset, nbytes);
}

size_t WritableFileWriter::RequestToken(size_t bytes, bool align) {
  Env::IOPriority io_priority;
  if (rate_limiter_ &&
      (io_priority = writable_file_->GetIOPriority()) < Env::IO_TOTAL) {
    bytes = std::min(bytes,
                     static_cast<size_t>(rate_limiter_->GetSingleBurstBytes()));

    if (align) {
      // Here we may actually require more than burst and block
      // but we can not write less than one page at a time on direct I/O
      // thus we may want not to use ratelimiter
      size_t alignment = buf_.Alignment();
      bytes = std::max(alignment, TruncateToPageBoundary(alignment, bytes));
    }
    rate_limiter_->Request(bytes, io_priority, stats_);
  }
  return bytes;
}

// This method writes to disk the specified data and makes use of the rate
// limiter if available
Status WritableFileWriter::WriteBuffered(const char* data, size_t size) {
  Status s;
  assert(!use_direct_io());
  const char* src = data;
  size_t left = size;

  while (left > 0) {
    size_t allowed = RequestToken(left, false);

    {
      IOSTATS_TIMER_GUARD(write_nanos);
      TEST_SYNC_POINT("WritableFileWriter::Flush:BeforeAppend");
      s = writable_file_->Append(Slice(src, allowed));
      if (!s.ok()) {
        return s;
      }
    }

    IOSTATS_ADD(bytes_written, allowed);
    TEST_KILL_RANDOM("WritableFileWriter::WriteBuffered:0", rocksdb_kill_odds);

    left -= allowed;
    src += allowed;
  }
  buf_.Size(0);
  return s;
}

// This flushes the accumulated data in the buffer. We pad data with zeros if
// necessary to the whole page.
// However, during automatic flushes padding would not be necessary.
// We always use RateLimiter if available. We move (Refit) any buffer bytes
// that are left over the
// whole number of pages to be written again on the next flush because we can
// only write on aligned
// offsets.
Status WritableFileWriter::WriteDirect() {
  assert(use_direct_io());
  Status s;
  const size_t alignment = buf_.Alignment();
  assert((next_write_offset_ % alignment) == 0);

  // Calculate whole page final file advance if all writes succeed
  size_t file_advance = TruncateToPageBoundary(alignment, buf_.CurrentSize());

  // Calculate the leftover tail, we write it here padded with zeros BUT we
  // will write
  // it again in the future either on Close() OR when the current whole page
  // fills out
  size_t leftover_tail = buf_.CurrentSize() - file_advance;

  // Round up and pad
  buf_.PadToAlignmentWith(0);

  const char* src = buf_.BufferStart();
  uint64_t write_offset = next_write_offset_;
  size_t left = buf_.CurrentSize();

  while (left > 0) {
    // Check how much is allowed
    size_t size = RequestToken(left, true);

    {
      IOSTATS_TIMER_GUARD(write_nanos);
      TEST_SYNC_POINT("WritableFileWriter::Flush:BeforeAppend");
      // direct writes must be positional
      s = writable_file_->PositionedAppend(Slice(src, size), write_offset);
      if (!s.ok()) {
        buf_.Size(file_advance + leftover_tail);
        return s;
      }
    }

    IOSTATS_ADD(bytes_written, size);
    left -= size;
    src += size;
    write_offset += size;
    assert((next_write_offset_ % alignment) == 0);
  }

  if (s.ok()) {
    // Move the tail to the beginning of the buffer
    // This never happens during normal Append but rather during
    // explicit call to Flush()/Sync() or Close()
    buf_.RefitTail(file_advance, leftover_tail);
    // This is where we start writing next time which may or not be
    // the actual file size on disk. They match if the buffer size
    // is a multiple of whole pages otherwise filesize_ is leftover_tail
    // behind
    next_write_offset_ += file_advance;
  }
  return s;
}

namespace {
class ReadaheadRandomAccessFile : public RandomAccessFile {
 public:
  ReadaheadRandomAccessFile(std::unique_ptr<RandomAccessFile, ptr_destruct_delete<RandomAccessFile>>&& file,
                            size_t readahead_size)
      : file_(std::move(file)),
        alignment_(file_->GetRequiredBufferAlignment()),
        readahead_size_(Roundup(readahead_size, alignment_)),
        forward_calls_(file_->ShouldForwardRawRequest()),
        buffer_(),
        buffer_offset_(0),
        buffer_len_(0) {
    if (!forward_calls_) {
      buffer_.Alignment(alignment_);
      buffer_.AllocateNewBuffer(readahead_size_);
    } else if (readahead_size_ > 0) {
      file_->EnableReadAhead();
    }
  }

  ReadaheadRandomAccessFile(const ReadaheadRandomAccessFile&) = delete;

  ReadaheadRandomAccessFile& operator=(const ReadaheadRandomAccessFile&) =
      delete;

  virtual Status Read(uint64_t offset, size_t n, Slice* result,
                      char* scratch) const override {
    if (n + alignment_ >= readahead_size_) {
      return file_->Read(offset, n, result, scratch);
    }

    // On Windows in unbuffered mode this will lead to double buffering
    // and double locking so we avoid that.
    // In normal mode Windows caches so much data from disk that we do
    // not need readahead.
    if (forward_calls_) {
      return file_->Read(offset, n, result, scratch);
    }

    std::unique_lock<std::mutex> lk(lock_);

    size_t cached_len = 0;
    // Check if there is a cache hit, means that [offset, offset + n) is either
    // complitely or partially in the buffer
    // If it's completely cached, including end of file case when offset + n is
    // greater than EOF, return
    if (TryReadFromCache(offset, n, &cached_len, scratch) &&
        (cached_len == n ||
         // End of file
         buffer_len_ < readahead_size_)) {
      *result = Slice(scratch, cached_len);
      return Status::OK();
    }
    size_t advanced_offset = offset + cached_len;
    // In the case of cache hit advanced_offset is already aligned, means that
    // chunk_offset equals to advanced_offset
    size_t chunk_offset = TruncateToPageBoundary(alignment_, advanced_offset);
    Slice readahead_result;

    Status s = ReadIntoBuffer(chunk_offset, readahead_size_);
    if (s.ok()) {
      // In the case of cache miss, i.e. when cached_len equals 0, an offset can
      // exceed the file end position, so the following check is required
      if (advanced_offset < chunk_offset + buffer_len_) {
        // In the case of cache miss, the first chunk_padding bytes in buffer_
        // are
        // stored for alignment only and must be skipped
        size_t chunk_padding = advanced_offset - chunk_offset;
        auto remaining_len =
            std::min(buffer_len_ - chunk_padding, n - cached_len);
        memcpy(scratch + cached_len, buffer_.BufferStart() + chunk_padding,
               remaining_len);
        *result = Slice(scratch, cached_len + remaining_len);
      } else {
        *result = Slice(scratch, cached_len);
      }
    }
    return s;
  }

  virtual Status Prefetch(uint64_t offset, size_t n) override {
    size_t prefetch_offset = TruncateToPageBoundary(alignment_, offset);
    if (prefetch_offset == buffer_offset_) {
      return Status::OK();
    }
    return ReadIntoBuffer(prefetch_offset, offset - prefetch_offset + n);
  }

  virtual size_t GetUniqueId(char* id, size_t max_size) const override {
    return file_->GetUniqueId(id, max_size);
  }

  virtual void Hint(AccessPattern pattern) override { file_->Hint(pattern); }

  virtual Status InvalidateCache(size_t offset, size_t length) override {
    return file_->InvalidateCache(offset, length);
  }

  virtual bool use_direct_io() const override { return file_->use_direct_io(); }

 private:
  bool TryReadFromCache(uint64_t offset, size_t n, size_t* cached_len,
                        char* scratch) const {
    if (offset < buffer_offset_ || offset >= buffer_offset_ + buffer_len_) {
      *cached_len = 0;
      return false;
    }
    uint64_t offset_in_buffer = offset - buffer_offset_;
    *cached_len =
        std::min(buffer_len_ - static_cast<size_t>(offset_in_buffer), n);
    memcpy(scratch, buffer_.BufferStart() + offset_in_buffer, *cached_len);
    return true;
  }

  Status ReadIntoBuffer(uint64_t offset, size_t n) const {
    if (n > buffer_.Capacity()) {
      n = buffer_.Capacity();
    }
    Slice result;
    Status s = file_->Read(offset, n, &result, buffer_.BufferStart());
    if (s.ok()) {
      buffer_offset_ = offset;
      buffer_len_ = result.size();
    }
    return s;
  }

  std::unique_ptr<RandomAccessFile, ptr_destruct_delete<RandomAccessFile>> file_;
  const size_t alignment_;
  size_t readahead_size_;
  const bool forward_calls_;

  mutable std::mutex lock_;
  mutable AlignedBuffer buffer_;
  mutable uint64_t buffer_offset_;
  mutable size_t buffer_len_;
};
}  // namespace

std::unique_ptr<RandomAccessFile, ptr_destruct_delete<RandomAccessFile>> NewReadaheadRandomAccessFile(
    std::unique_ptr<RandomAccessFile, ptr_destruct_delete<RandomAccessFile>>&& file, size_t readahead_size) {
  std::unique_ptr<RandomAccessFile, ptr_destruct_delete<RandomAccessFile>> result(
      MOD_NEW_OBJECT(ModId::kDefaultMod, ReadaheadRandomAccessFile, std::move(file), readahead_size));
  return result;
}

Status NewWritableFile(Env* env, const std::string& fname,
                       WritableFile *&result,
                       const EnvOptions& options) {
  Status s = env->NewWritableFile(fname, result, options);
  TEST_KILL_RANDOM("NewWritableFile:0", rocksdb_kill_odds * REDUCE_ODDS2);
  return s;
}

}  // namespace util
}  // namespace smartengine
