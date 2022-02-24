/*
 * Copyright (C) 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef INCLUDE_PERFETTO_PROTOZERO_SCATTERED_STREAM_WRITER_H_
#define INCLUDE_PERFETTO_PROTOZERO_SCATTERED_STREAM_WRITER_H_

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "perfetto/base/compiler.h"
#include "perfetto/base/export.h"
#include "perfetto/protozero/contiguous_memory_range.h"
#include "perfetto/protozero/proto_utils.h"

namespace protozero {

// This class deals with the following problem: append-only proto messages want
// to write a stream of bytes, without caring about the implementation of the
// underlying buffer (which concretely will be either the trace ring buffer
// or a heap-allocated buffer). The main deal is: proto messages don't know in
// advance what their size will be.
// Due to the tracing buffer being split into fixed-size chunks, on some
// occasions, these writes need to be spread over two (or more) non-contiguous
// chunks of memory. Similarly, when the buffer is backed by the heap, we want
// to avoid realloc() calls, as they might cause a full copy of the contents
// of the buffer.
// The purpose of this class is to abstract away the non-contiguous write logic.
// This class knows how to deal with writes as long as they fall in the same
// ContiguousMemoryRange and defers the chunk-chaining logic to the Delegate.
class PERFETTO_EXPORT ScatteredStreamWriter {
 public:
  class PERFETTO_EXPORT Delegate {
   public:
    virtual ~Delegate();
    virtual ContiguousMemoryRange GetNewBuffer() = 0;
  };

  struct ReservedBytes {
    uint8_t *buf_[2];
    size_t firstSz_;

    // Maximum message value supported: 256 MiB (4 x 7-bit due to varint encoding).
    static constexpr size_t kFieldSize = proto_utils::kMessageLengthFieldSize;

    inline void Reset() { buf_[0] = nullptr; buf_[1] = nullptr; firstSz_ = 0; }

    // Writes a fixed-size redundant encoding of the given |value|. This is
    // used to backfill fixed-size reservations for the length field using a
    // non-canonical varint encoding (e.g. \x81\x80\x80\x00 instead of \x01).
    // See https://github.com/google/protobuf/issues/1530.
    // This is used mainly in two cases:
    // 1) At trace writing time, when starting a nested messages. The size of a
    //    nested message is not known until all its field have been written.
    //    |kMessageLengthFieldSize| bytes are reserved to encode the size field and
    //    backfilled at the end.
    // 2) When rewriting a message at trace filtering time, in protozero/filtering.
    //    At that point we know only the upper bound of the length (a filtered
    //    message is <= the original one) and we backfill after the message has been
    //    filtered.
    inline void WriteRedundantVarInt(uint32_t value) {
      uint8_t* dst = buf_[0];
      for (size_t i=0; i < kFieldSize; ++i) {
        const uint8_t msb = (i == 3)? 0 : 0x80;
        if (firstSz_ == i) dst = buf_[1];
        *(dst++) = static_cast<uint8_t>(value | msb);
        value >>= 7;
      }
      Reset();
    }
    inline bool IsNull() const { return buf_[0] == nullptr; }
    inline void SetPrereservedContiguousMemoryRange(uint8_t* mem) {
      buf_[0] = mem;
      firstSz_ = kFieldSize;
    }
    inline bool IsContiguousWithinRange(const uint8_t* begin, const uint8_t* end, ptrdiff_t* offset=nullptr) const {
      if (buf_[1]) return false;   // not contiguous
      const bool ret = (begin >= buf_[0]) && (end >= (buf_[0] + firstSz_));
      if (ret) *offset = buf_[0] - begin;
      return ret;
    }
  };

  explicit ScatteredStreamWriter(Delegate* delegate);
  ~ScatteredStreamWriter();

  inline void WriteByte(uint8_t value) {
    if (write_ptr_ >= cur_range_.end)
      Extend();
    *write_ptr_++ = value;
  }

  // Assumes that the caller checked that there is enough headroom.
  // TODO(primiano): perf optimization, this is a tracing hot path. The
  // compiler can make strong optimization on memcpy if the size arg is a
  // constexpr. Make a templated variant of this for fixed-size writes.
  // TODO(primiano): restrict / noalias might also help.
  inline void WriteBytesUnsafe(const uint8_t* src, size_t size) {
    uint8_t* const end = write_ptr_ + size;
    assert(end <= cur_range_.end);
    memcpy(write_ptr_, src, size);
    write_ptr_ = end;
  }

  inline void WriteBytes(const uint8_t* src, size_t size) {
    uint8_t* const end = write_ptr_ + size;
    if (PERFETTO_LIKELY(end <= cur_range_.end))
      return WriteBytesUnsafe(src, size);
    WriteBytesSlowPath(src, size);
  }

  void WriteBytesSlowPath(const uint8_t* src, size_t size);

  // Reserves a fixed amount of bytes to be backfilled later. The reserved range
  // is guaranteed to be contiguous and not span across chunks. |size| has to be
  // <= than the size of a new buffer returned by the Delegate::GetNewBuffer().
  ReservedBytes ReserveBytes(bool zeroReservedBytes);

  // Fast (but unsafe) version of the above. The caller must have previously
  // checked that there are at least |size| contiguous bytes available.
  // Returns only the start pointer of the reservation.
  uint8_t* ReserveBytesUnsafe(size_t size) {
    uint8_t* begin = write_ptr_;
    write_ptr_ += size;
    assert(write_ptr_ <= cur_range_.end);
    return begin;
  }

  // Resets the buffer boundaries and the write pointer to the given |range|.
  // Subsequent WriteByte(s) will write into |range|.
  void Reset(ContiguousMemoryRange range);

  // Number of contiguous free bytes in |cur_range_| that can be written without
  // requesting a new buffer.
  size_t bytes_available() const {
    return static_cast<size_t>(cur_range_.end - write_ptr_);
  }

  uint8_t* write_ptr() const { return write_ptr_; }

  uint64_t written() const {
    return written_previously_ +
           static_cast<uint64_t>(write_ptr_ - cur_range_.begin);
  }

 private:
  ScatteredStreamWriter(const ScatteredStreamWriter&) = delete;
  ScatteredStreamWriter& operator=(const ScatteredStreamWriter&) = delete;

  void Extend();

  Delegate* const delegate_;
  ContiguousMemoryRange cur_range_;
  uint8_t* write_ptr_;
  uint64_t written_previously_ = 0;
};

}  // namespace protozero

#endif  // INCLUDE_PERFETTO_PROTOZERO_SCATTERED_STREAM_WRITER_H_
