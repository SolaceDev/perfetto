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

#ifndef INCLUDE_PERFETTO_PROTOZERO_MESSAGE_H_
#define INCLUDE_PERFETTO_PROTOZERO_MESSAGE_H_

#include <assert.h>
#include <stdint.h>
#include <string.h>

#include <string>
#include <type_traits>

#include "perfetto/base/export.h"
#include "perfetto/base/logging.h"
#include "perfetto/protozero/contiguous_memory_range.h"
#include "perfetto/protozero/proto_utils.h"
#include "perfetto/protozero/scattered_stream_writer.h"

namespace perfetto {
namespace shm_fuzz {
class FakeProducer;
}  // namespace shm_fuzz
}  // namespace perfetto

namespace protozero {

class MessageArena;
class MessageHandleBase;

// Base class extended by the proto C++ stubs generated by the ProtoZero
// compiler. This class provides the minimal runtime required to support
// append-only operations and is designed for performance. None of the methods
// require any dynamic memory allocation, unless more than 16 nested messages
// are created via BeginNestedMessage() calls.
class PERFETTO_EXPORT Message {
 public:
  friend class MessageHandleBase;

  // The ctor is deliberately a no-op to avoid forwarding args from all
  // subclasses. The real initialization is performed by Reset().
  // Nested messages are allocated via placement new by MessageArena and
  // implictly destroyed when the RootMessage's arena goes away. This is
  // fine as long as all the fields are PODs, which is checked by the
  // static_assert()s in the Reset() method.
  Message() = default;

  // Clears up the state, allowing the message to be reused as a fresh one.
  void Reset(ScatteredStreamWriter*, MessageArena*);

  // Commits all the changes to the buffer (backfills the size field of this and
  // all nested messages) and seals the message. Returns the size of the message
  // (and all nested sub-messages), without taking into account any chunking.
  // Finalize is idempotent and can be called several times w/o side effects.
  uint32_t Finalize();

  // Optional. If is_valid() == true, the corresponding memory region (its
  // length == proto_utils::kMessageLengthFieldSize) is backfilled with the size
  // of this message (minus |size_already_written| below). This is the mechanism
  // used by messages to backfill their corresponding size field in the parent
  // message.
  const ScatteredStreamWriter::ReservedBytes& size_field() const { return size_field_; }
  void set_size_field(ScatteredStreamWriter::ReservedBytes size_field)
    { size_field_ = size_field; }
  void reset_size_field() { size_field_.Reset(); }
  void WriteReservedSize(uint32_t value) { size_field_.WriteRedundantVarInt(value); }
  void SetPrereservedSizeContiguousMemoryRange(uint8_t* mem) {
    size_field_.SetPrereservedContiguousMemoryRange(mem);
  }

  // This is to deal with case of backfilling the size of a root (non-nested)
  // message which is split into multiple chunks. Upon finalization only the
  // partial size that lies in the last chunk has to be backfilled.
  void inc_size_already_written(uint32_t sz) { size_already_written_ += sz; }

  Message* nested_message() { return nested_message_; }

  bool is_finalized() const { return finalized_; }

#if PERFETTO_DCHECK_IS_ON()
  void set_handle(MessageHandleBase* handle) { handle_ = handle; }
#endif

  // Proto types: uint64, uint32, int64, int32, bool, enum.
  template <typename T>
  void AppendVarInt(uint32_t field_id, T value) {
    if (nested_message_)
      EndNestedMessage();

    uint8_t buffer[proto_utils::kMaxSimpleFieldEncodedSize];
    uint8_t* pos = buffer;

    pos = proto_utils::WriteVarInt(proto_utils::MakeTagVarInt(field_id), pos);
    // WriteVarInt encodes signed values in two's complement form.
    pos = proto_utils::WriteVarInt(value, pos);
    WriteToStream(buffer, pos);
  }

  // Proto types: sint64, sint32.
  template <typename T>
  void AppendSignedVarInt(uint32_t field_id, T value) {
    AppendVarInt(field_id, proto_utils::ZigZagEncode(value));
  }

  // Proto types: bool, enum (small).
  // Faster version of AppendVarInt for tiny numbers.
  void AppendTinyVarInt(uint32_t field_id, int32_t value) {
    PERFETTO_DCHECK(0 <= value && value < 0x80);
    if (nested_message_)
      EndNestedMessage();

    uint8_t buffer[proto_utils::kMaxSimpleFieldEncodedSize];
    uint8_t* pos = buffer;
    // MakeTagVarInt gets super optimized here for constexpr.
    pos = proto_utils::WriteVarInt(proto_utils::MakeTagVarInt(field_id), pos);
    *pos++ = static_cast<uint8_t>(value);
    WriteToStream(buffer, pos);
  }

  // Proto types: fixed64, sfixed64, fixed32, sfixed32, double, float.
  template <typename T>
  void AppendFixed(uint32_t field_id, T value) {
    if (nested_message_)
      EndNestedMessage();

    uint8_t buffer[proto_utils::kMaxSimpleFieldEncodedSize];
    uint8_t* pos = buffer;

    pos = proto_utils::WriteVarInt(proto_utils::MakeTagFixed<T>(field_id), pos);
    memcpy(pos, &value, sizeof(T));
    pos += sizeof(T);
    // TODO: Optimize memcpy performance, see http://crbug.com/624311 .
    WriteToStream(buffer, pos);
  }

  void AppendString(uint32_t field_id, const char* str);

  void AppendString(uint32_t field_id, const std::string& str) {
    AppendBytes(field_id, str.data(), str.size());
  }

  void AppendBytes(uint32_t field_id, const void* value, size_t size);

  // Append raw bytes for a field, using the supplied |ranges| to
  // copy from |num_ranges| individual buffers.
  size_t AppendScatteredBytes(uint32_t field_id,
                              ContiguousMemoryRange* ranges,
                              size_t num_ranges);

  // Begins a nested message. The returned object is owned by the MessageArena
  // of the root message. The nested message ends either when Finalize() is
  // called or when any other Append* method is called in the parent class.
  // The template argument T is supposed to be a stub class auto generated from
  // a .proto, hence a subclass of Message.
  template <class T>
  T* BeginNestedMessage(uint32_t field_id) {
    // This is to prevent subclasses (which should be autogenerated, though), to
    // introduce extra state fields (which wouldn't be initialized by Reset()).
    static_assert(std::is_base_of<Message, T>::value,
                  "T must be a subclass of Message");
    static_assert(sizeof(T) == sizeof(Message),
                  "Message subclasses cannot introduce extra state.");
    return static_cast<T*>(BeginNestedMessageInternal(field_id));
  }

  // Gives read-only access to the underlying stream_writer. This is used only
  // by few internals to query the state of the underlying buffer. It is almost
  // always a bad idea to poke at the stream_writer() internals.
  const ScatteredStreamWriter* stream_writer() const { return stream_writer_; }

  // Appends some raw bytes to the message. The use-case for this is preserving
  // unknown fields in the decode -> re-encode path of xxx.gen.cc classes
  // generated by the cppgen_plugin.cc.
  // The caller needs to guarantee that the appended data is properly
  // proto-encoded and each field has a proto preamble.
  void AppendRawProtoBytes(const void* data, size_t size) {
    const uint8_t* src = reinterpret_cast<const uint8_t*>(data);
    WriteToStream(src, src + size);
  }

 private:
  Message(const Message&) = delete;
  Message& operator=(const Message&) = delete;

  Message* BeginNestedMessageInternal(uint32_t field_id);

  // Called by Finalize and Append* methods.
  void EndNestedMessage();

  void WriteToStream(const uint8_t* src_begin, const uint8_t* src_end) {
    PERFETTO_DCHECK(!finalized_);
    PERFETTO_DCHECK(src_begin <= src_end);
    const uint32_t size = static_cast<uint32_t>(src_end - src_begin);
    stream_writer_->WriteBytes(src_begin, size);
    size_ += size;
  }

  // Only POD fields are allowed. This class's dtor is never called.
  // See the comment on the static_assert in the corresponding .cc file.

  // The stream writer interface used for the serialization.
  ScatteredStreamWriter* stream_writer_;

  // The storage used to allocate nested Message objects.
  // This is owned by RootMessage<T>.
  MessageArena* arena_;

  // Pointer to the last child message created through BeginNestedMessage(), if
  // any, nullptr otherwise. There is no need to keep track of more than one
  // message per nesting level as the proto-zero API contract mandates that
  // nested fields can be filled only in a stacked fashion. In other words,
  // nested messages are finalized and sealed when any other field is set in the
  // parent message (or the parent message itself is finalized) and cannot be
  // accessed anymore afterwards.
  Message* nested_message_;

  // [optional] Pointer to a non-aligned pre-reserved var-int slot of
  // kMessageLengthFieldSize bytes. When set, the Finalize() method will write
  // the size of proto-encoded message in the pointed memory region.
  ScatteredStreamWriter::ReservedBytes size_field_;

  // Keeps track of the size of the current message.
  uint32_t size_;

  // See comment for inc_size_already_written().
  uint32_t size_already_written_;

  // When true, no more changes to the message are allowed. This is to DCHECK
  // attempts of writing to a message which has been Finalize()-d.
  bool finalized_;

#if PERFETTO_DCHECK_IS_ON()
  // Current generation of message. Incremented on Reset.
  // Used to detect stale handles.
  uint32_t generation_;

  MessageHandleBase* handle_;
#endif
};

}  // namespace protozero

#endif  // INCLUDE_PERFETTO_PROTOZERO_MESSAGE_H_
