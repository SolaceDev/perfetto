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

#include "perfetto/protozero/scattered_stream_writer.h"

#include <string.h>

#include <memory>

#include "perfetto/base/logging.h"
#include "src/protozero/test/fake_scattered_buffer.h"
#include "test/gtest_and_gmock.h"

namespace protozero {
namespace {

constexpr size_t kChunkSize = 8;

TEST(ScatteredStreamWriterTest, ScatteredWrites) {
  FakeScatteredBuffer delegate(kChunkSize);
  ScatteredStreamWriter ssw(&delegate);

  const uint8_t kOneByteBuf[] = {0x40};
  const uint8_t kThreeByteBuf[] = {0x50, 0x51, 0x52};
  uint8_t kTwentyByteBuf[20];
  for (uint8_t i = 0; i < sizeof(kTwentyByteBuf); ++i)
    kTwentyByteBuf[i] = 0xA0 + i;

  // Writing up to the chunk size should cause only the initial extension.
  for (uint8_t i = 0; i < kChunkSize; ++i) {
    ssw.WriteByte(i);
    EXPECT_EQ(kChunkSize - i - 1, ssw.bytes_available());
  }
  EXPECT_EQ(1u, delegate.chunks().size());
  EXPECT_EQ(0u, ssw.bytes_available());

  // This extra write will cause the first extension.
  ssw.WriteBytes(kOneByteBuf, sizeof(kOneByteBuf));
  EXPECT_EQ(2u, delegate.chunks().size());
  EXPECT_EQ(7u, ssw.bytes_available());

  // This starts at offset 1, to make sure we don't hardcode any assumption
  // about alignment.
  auto reserved_range_1 = ssw.ReserveBytes(false);
  // Check that the four bytes are reserved correctly.
  EXPECT_EQ(2u, delegate.chunks().size());
  EXPECT_EQ(3u, ssw.bytes_available());

  ssw.WriteByte(0xFF);
  ssw.WriteBytes(kThreeByteBuf, sizeof(kThreeByteBuf));
  // Check that writing past the end of the chunk after the reserved
  // bytes causes another extension, and that the reserved bytes
  // are still not backfilled.
  EXPECT_EQ(3u, delegate.chunks().size());
  EXPECT_EQ(7u, ssw.bytes_available());
  EXPECT_EQ("4000000000FF5051", delegate.GetChunkAsString(1));


  auto reserved_range_2 = ssw.ReserveBytes(false);
  ssw.WriteBytes(kTwentyByteBuf, sizeof(kTwentyByteBuf));
  EXPECT_EQ(6u, delegate.chunks().size());
  EXPECT_EQ(7u, ssw.bytes_available());
  EXPECT_EQ("5200000000A0A1A2", delegate.GetChunkAsString(2));
  

  // Backfilling the reserved bytes should not change the bytes_available().
  reserved_range_1.WriteRedundantVarInt(0x01020304);   // Encodes to 0x84 0x86 0x88 0x08
  reserved_range_2.WriteRedundantVarInt(0x01020304);
  EXPECT_EQ(6u, delegate.chunks().size());
  EXPECT_EQ(7u, ssw.bytes_available());
  
  // Confirm the reserved bytes were backfilled successfully.
  EXPECT_EQ("4084868808FF5051", delegate.GetChunkAsString(1));
  EXPECT_EQ("5284868808A0A1A2", delegate.GetChunkAsString(2));

  // After for-loop, only 2 bytes are left in the current chunk.
  for (uint8_t i = 0; i < 5; ++i)
    ssw.WriteByte(0xFF);
  EXPECT_EQ("B3FFFFFFFFFF0000", delegate.GetChunkAsString(5));

  // Check that reserving more bytes than what left causes reserved bytes to span across
  // multiple chunks. Write to these bytes immediately. Should see that the write
  // (0x84 0x86 0x88 0x08) is split across two chunks.
  ssw.ReserveBytes(false).WriteRedundantVarInt(0x01020304);
  EXPECT_EQ(7u, delegate.chunks().size());
  EXPECT_EQ(6u, ssw.bytes_available());
  EXPECT_EQ("B3FFFFFFFFFF8486", delegate.GetChunkAsString(5));
  EXPECT_EQ("8808000000000000", delegate.GetChunkAsString(6));

  // Finally reset the writer to a new buffer.
  uint8_t other_buffer[8] = {0};
  ssw.Reset({other_buffer, other_buffer + sizeof(other_buffer)});
  EXPECT_EQ(other_buffer, ssw.write_ptr());
  ssw.WriteByte(1);
  ssw.WriteBytes(kThreeByteBuf, sizeof(kThreeByteBuf));
  EXPECT_EQ(1u, other_buffer[0]);
  EXPECT_EQ(0x52u, other_buffer[3]);
}

}  // namespace
}  // namespace protozero
