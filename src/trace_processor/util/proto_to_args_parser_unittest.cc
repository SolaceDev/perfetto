/*
 * Copyright (C) 2021 The Android Open Source Project
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

#include "src/trace_processor/util/proto_to_args_parser.h"

#include "perfetto/ext/base/string_view.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "protos/perfetto/common/descriptor.pbzero.h"
#include "protos/perfetto/trace/track_event/source_location.pbzero.h"
#include "src/protozero/test/example_proto/test_messages.pbzero.h"
#include "src/trace_processor/test_messages.descriptor.h"
#include "test/gtest_and_gmock.h"

#include <sstream>

namespace perfetto {
namespace trace_processor {
namespace util {
namespace {

constexpr size_t kChunkSize = 42;

using ::testing::_;
using ::testing::Eq;
using ::testing::Invoke;
using ::testing::NiceMock;

class ProtoToArgsParserTest : public ::testing::Test,
                              public ProtoToArgsParser::Delegate {
 protected:
  ProtoToArgsParserTest() {}

  const std::vector<std::string>& args() const { return args_; }

 private:
  using Key = ProtoToArgsParser::Key;

  void AddInteger(const Key& key, int64_t value) override {
    std::stringstream ss;
    ss << key.flat_key << " " << key.key << " " << value;
    args_.push_back(ss.str());
  }

  void AddUnsignedInteger(const Key& key, uint64_t value) override {
    std::stringstream ss;
    ss << key.flat_key << " " << key.key << " " << value;
    args_.push_back(ss.str());
  }

  void AddString(const Key& key, const protozero::ConstChars& value) override {
    std::stringstream ss;
    ss << key.flat_key << " " << key.key << " " << value.ToStdString();
    args_.push_back(ss.str());
  }

  void AddDouble(const Key& key, double value) override {
    std::stringstream ss;
    ss << key.flat_key << " " << key.key << " " << value;
    args_.push_back(ss.str());
  }

  void AddPointer(const Key& key, const void* value) override {
    std::stringstream ss;
    ss << key.flat_key << " " << key.key << " " << std::hex
       << reinterpret_cast<uintptr_t>(value) << std::dec;
    args_.push_back(ss.str());
  }

  void AddBoolean(const Key& key, bool value) override {
    std::stringstream ss;
    ss << key.flat_key << " " << key.key << " " << (value ? "true" : "false");
    args_.push_back(ss.str());
  }

  void AddJson(const Key& key, const protozero::ConstChars& value) override {
    std::stringstream ss;
    ss << key.flat_key << " " << key.key << " " << std::hex
       << value.ToStdString() << std::dec;
    args_.push_back(ss.str());
  }

  std::vector<std::string> args_;
};

TEST_F(ProtoToArgsParserTest, EnsureTestMessageProtoParses) {
  DescriptorPool pool;
  auto status = pool.AddFromFileDescriptorSet(kTestMessagesDescriptor.data(),
                                              kTestMessagesDescriptor.size());
  ProtoToArgsParser parser(pool);
  EXPECT_TRUE(status.ok()) << "Failed to parse kTestMessagesDescriptor: "
                           << status.message();
}

TEST_F(ProtoToArgsParserTest, BasicSingleLayerProto) {
  using namespace protozero::test::protos::pbzero;
  protozero::HeapBuffered<EveryField> msg{kChunkSize, kChunkSize};
  msg->set_field_int32(-1);
  msg->set_field_int64(-333123456789ll);
  msg->set_field_uint32(600);
  msg->set_field_uint64(333123456789ll);
  msg->set_field_sint32(-5);
  msg->set_field_sint64(-9000);
  msg->set_field_fixed32(12345);
  msg->set_field_fixed64(444123450000ll);
  msg->set_field_sfixed32(-69999);
  msg->set_field_sfixed64(-200);
  msg->set_field_double(0.5555);
  msg->set_field_bool(true);
  msg->set_small_enum(SmallEnum::TO_BE);
  msg->set_signed_enum(SignedEnum::NEGATIVE);
  msg->set_big_enum(BigEnum::BEGIN);
  msg->set_nested_enum(EveryField::PONG);
  msg->set_field_float(3.14f);
  msg->set_field_string("FizzBuzz");
  msg->add_repeated_int32(1);
  msg->add_repeated_int32(-1);
  msg->add_repeated_int32(100);
  msg->add_repeated_int32(2000000);

  auto binary_proto = msg.SerializeAsArray();

  DescriptorPool pool;
  auto status = pool.AddFromFileDescriptorSet(kTestMessagesDescriptor.data(),
                                              kTestMessagesDescriptor.size());
  ProtoToArgsParser parser(pool);
  ASSERT_TRUE(status.ok()) << "Failed to parse kTestMessagesDescriptor: "
                           << status.message();

  status = parser.ParseMessage(
      protozero::ConstBytes{binary_proto.data(), binary_proto.size()},
      ".protozero.test.protos.EveryField", nullptr, *this);

  EXPECT_TRUE(status.ok())
      << "InternProtoFieldsIntoArgsTable failed with error: "
      << status.message();

  EXPECT_THAT(
      args(),
      testing::ElementsAre(
          "field_int32 field_int32 -1", "field_int64 field_int64 -333123456789",
          "field_uint32 field_uint32 600",
          "field_uint64 field_uint64 333123456789",
          "field_sint32 field_sint32 -5", "field_sint64 field_sint64 -9000",
          "field_fixed32 field_fixed32 12345",
          "field_fixed64 field_fixed64 444123450000",
          "field_sfixed32 field_sfixed32 -69999",
          "field_sfixed64 field_sfixed64 -200",
          "field_double field_double 0.5555", "field_bool field_bool true",
          "small_enum small_enum TO_BE", "signed_enum signed_enum NEGATIVE",
          "big_enum big_enum BEGIN", "nested_enum nested_enum PONG",
          "field_float field_float 3.14", "field_string field_string FizzBuzz",
          "repeated_int32 repeated_int32[0] 1",
          "repeated_int32 repeated_int32[1] -1",
          "repeated_int32 repeated_int32[2] 100",
          "repeated_int32 repeated_int32[3] 2000000"));
}

TEST_F(ProtoToArgsParserTest, NestedProto) {
  using namespace protozero::test::protos::pbzero;
  protozero::HeapBuffered<NestedA> msg{kChunkSize, kChunkSize};
  msg->set_super_nested()->set_value_c(3);

  auto binary_proto = msg.SerializeAsArray();

  DescriptorPool pool;
  auto status = pool.AddFromFileDescriptorSet(kTestMessagesDescriptor.data(),
                                              kTestMessagesDescriptor.size());
  ProtoToArgsParser parser(pool);
  ASSERT_TRUE(status.ok()) << "Failed to parse kTestMessagesDescriptor: "
                           << status.message();

  status = parser.ParseMessage(
      protozero::ConstBytes{binary_proto.data(), binary_proto.size()},
      ".protozero.test.protos.NestedA", nullptr, *this);
  EXPECT_TRUE(status.ok())
      << "InternProtoFieldsIntoArgsTable failed with error: "
      << status.message();
  EXPECT_THAT(args(), testing::ElementsAre(
                          "super_nested.value_c super_nested.value_c 3"));
}

TEST_F(ProtoToArgsParserTest, CamelCaseFieldsProto) {
  using namespace protozero::test::protos::pbzero;
  protozero::HeapBuffered<CamelCaseFields> msg{kChunkSize, kChunkSize};
  msg->set_barbaz(true);
  msg->set_moomoo(true);
  msg->set___bigbang(true);

  auto binary_proto = msg.SerializeAsArray();

  DescriptorPool pool;
  auto status = pool.AddFromFileDescriptorSet(kTestMessagesDescriptor.data(),
                                              kTestMessagesDescriptor.size());
  ProtoToArgsParser parser(pool);
  ASSERT_TRUE(status.ok()) << "Failed to parse kTestMessagesDescriptor: "
                           << status.message();

  status = parser.ParseMessage(
      protozero::ConstBytes{binary_proto.data(), binary_proto.size()},
      ".protozero.test.protos.CamelCaseFields", nullptr, *this);
  EXPECT_TRUE(status.ok())
      << "InternProtoFieldsIntoArgsTable failed with error: "
      << status.message();
  EXPECT_THAT(args(),
              testing::ElementsAre("barBaz barBaz true", "MooMoo MooMoo true",
                                   "__bigBang __bigBang true"));
}

TEST_F(ProtoToArgsParserTest, NestedProtoParsingOverrideHandled) {
  using namespace protozero::test::protos::pbzero;
  protozero::HeapBuffered<NestedA> msg{kChunkSize, kChunkSize};
  msg->set_super_nested()->set_value_c(3);

  auto binary_proto = msg.SerializeAsArray();

  DescriptorPool pool;
  auto status = pool.AddFromFileDescriptorSet(kTestMessagesDescriptor.data(),
                                              kTestMessagesDescriptor.size());
  ProtoToArgsParser parser(pool);
  ASSERT_TRUE(status.ok()) << "Failed to parse kTestMessagesDescriptor: "
                           << status.message();

  parser.AddParsingOverride(
      "super_nested.value_c",
      [](const protozero::Field& field, ProtoToArgsParser::Delegate& writer) {
        EXPECT_EQ(field.type(), protozero::proto_utils::ProtoWireType::kVarInt);
        std::string key = "super_nested.value_b.replaced";
        writer.AddInteger({key, key}, field.as_int32());
        // We've handled this field by adding the desired args.
        return base::OkStatus();
      });

  status = parser.ParseMessage(
      protozero::ConstBytes{binary_proto.data(), binary_proto.size()},
      ".protozero.test.protos.NestedA", nullptr, *this);
  EXPECT_TRUE(status.ok())
      << "InternProtoFieldsIntoArgsTable failed with error: "
      << status.message();
  EXPECT_THAT(
      args(),
      testing::ElementsAre(
          "super_nested.value_b.replaced super_nested.value_b.replaced 3"));
}

TEST_F(ProtoToArgsParserTest, NestedProtoParsingOverrideSkipped) {
  using namespace protozero::test::protos::pbzero;
  protozero::HeapBuffered<NestedA> msg{kChunkSize, kChunkSize};
  msg->set_super_nested()->set_value_c(3);

  auto binary_proto = msg.SerializeAsArray();

  DescriptorPool pool;
  auto status = pool.AddFromFileDescriptorSet(kTestMessagesDescriptor.data(),
                                              kTestMessagesDescriptor.size());
  ProtoToArgsParser parser(pool);
  ASSERT_TRUE(status.ok()) << "Failed to parse kTestMessagesDescriptor: "
                           << status.message();

  parser.AddParsingOverride(
      "super_nested.value_c",
      [](const protozero::Field& field, ProtoToArgsParser::Delegate&) {
        static int val = 0;
        ++val;
        EXPECT_EQ(1, val);
        EXPECT_EQ(field.type(), protozero::proto_utils::ProtoWireType::kVarInt);
        return base::nullopt;
      });

  status = parser.ParseMessage(
      protozero::ConstBytes{binary_proto.data(), binary_proto.size()},
      ".protozero.test.protos.NestedA", nullptr, *this);
  EXPECT_TRUE(status.ok())
      << "InternProtoFieldsIntoArgsTable failed with error: "
      << status.message();
  EXPECT_THAT(args(), testing::ElementsAre(
                          "super_nested.value_c super_nested.value_c 3"));
}

}  // namespace
}  // namespace util
}  // namespace trace_processor
}  // namespace perfetto