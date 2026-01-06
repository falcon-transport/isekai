// Copyright 2024 Google LLC

// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//     http://www.apache.org/licenses/LICENSE-2.0

// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "isekai/host/rnic/traffic_shaper_model.h"

#include <cstdint>
#include <memory>
#include <queue>
#include <utility>

#include "absl/time/time.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/common/packet.h"
#include "isekai/common/simple_environment.h"
#include "isekai/common/testing.h"
#include "omnetpp/cmessage.h"

namespace isekai {
namespace {

class FakePacketBuilder : public PacketBuilderInterface {
 public:
  void EnqueuePacket(std::unique_ptr<Packet> packet) override {
    tx_buffer_.push(std::move(packet));
  }
  void EncapsulateAndSendTransportPacket(uint16_t priority) override {}
  void ExtractAndHandleTransportPacket(
      omnetpp::cMessage* network_packet) override {}
  std::unique_ptr<Packet> DequeuePacket() {
    if (tx_buffer_.empty()) {
      return nullptr;
    }
    std::unique_ptr<Packet> pkt = std::move(tx_buffer_.front());
    tx_buffer_.pop();
    return pkt;
  }

 private:
  std::queue<std::unique_ptr<Packet>> tx_buffer_;
};

TEST(TrafficShaperModelTest, TestTransferTxPacket) {
  SimpleEnvironment env;
  FakePacketBuilder packet_builder;
  TrafficShaperConfig ts_config;
  ts_config.set_slot_granularity_ns(1024);
  ts_config.set_timing_wheel_slots(1024);
  TrafficShaperModel ts =
      TrafficShaperModel(ts_config, &env, /*stats_collector=*/nullptr);
  ts.ConnectPacketBuilder(&packet_builder);

  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(512), [&]() {
    // Schedule multiple packets at various times w.r.t to current time (512ns)
    // and default timing wheel horizon (1.048576ms).
    auto p1 = std::make_unique<Packet>();
    auto p2 = std::make_unique<Packet>();
    auto p3 = std::make_unique<Packet>();
    auto p4 = std::make_unique<Packet>();

    // p1 is non-paced (all zeros, should bypass)
    p1->falcon.psn = 1;
    p1->metadata.timing_wheel_timestamp = absl::ZeroDuration();

    // p2 is before current (scheduled at 128ns)
    p2->falcon.psn = 2;
    p2->metadata.timing_wheel_timestamp = absl::Nanoseconds(128);

    // p3 is within horizon (scheduled at 5120ns)
    p3->falcon.psn = 3;
    p3->metadata.timing_wheel_timestamp = absl::Nanoseconds(5120);

    ts.TransferTxPacket(std::move(p1));
    ts.TransferTxPacket(std::move(p2));
    ts.TransferTxPacket(std::move(p3));
  }));

  // At 1us, only p1 (bypassed packet should be available to dequeue).
  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(1000), [&]() {
    std::unique_ptr<Packet> p = packet_builder.DequeuePacket();
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->falcon.psn, 1);

    ASSERT_EQ(packet_builder.DequeuePacket(), nullptr);
  }));

  // At 1.1us, packet p2 should be available (only packet in "now" slot).
  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(1100), [&]() {
    std::unique_ptr<Packet> p = packet_builder.DequeuePacket();
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->falcon.psn, 2);

    ASSERT_EQ(packet_builder.DequeuePacket(), nullptr);
  }));

  // At 6us, packet p3 should be available.
  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(6000), [&]() {
    std::unique_ptr<Packet> p = packet_builder.DequeuePacket();
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->falcon.psn, 3);

    ASSERT_EQ(packet_builder.DequeuePacket(), nullptr);
  }));

  // Stop simulation after all packets have drained.
  EXPECT_OK(env.ScheduleEvent(absl::Microseconds(3000), [&]() { env.Stop(); }));

  env.Run();
}

TEST(TrafficShaperModelTest, TestTimingWheelWrapAround) {
  SimpleEnvironment env;
  FakePacketBuilder packet_builder;
  TrafficShaperConfig ts_config;
  ts_config.set_slot_granularity_ns(1024);
  ts_config.set_timing_wheel_slots(1024);
  TrafficShaperModel ts =
      TrafficShaperModel(ts_config, &env, /*stats_collector=*/nullptr);
  ts.ConnectPacketBuilder(&packet_builder);

  EXPECT_OK(env.ScheduleEvent(absl::ZeroDuration(), [&]() {
    for (uint64_t i = 0; i < ts_config.timing_wheel_slots(); i++) {
      auto p = std::make_unique<Packet>();
      p->falcon.psn = i;
      p->metadata.timing_wheel_timestamp =
          i * absl::Nanoseconds(ts_config.slot_granularity_ns());

      ts.TransferTxPacket(std::move(p));
    }
  }));

  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(1024 * 1024), [&]() {
    for (uint64_t i = ts_config.timing_wheel_slots();
         i < 2 * ts_config.timing_wheel_slots(); i++) {
      auto p = std::make_unique<Packet>();
      p->falcon.psn = i;
      p->metadata.timing_wheel_timestamp =
          i * absl::Nanoseconds(ts_config.slot_granularity_ns());

      ts.TransferTxPacket(std::move(p));
    }
  }));

  EXPECT_OK(env.ScheduleEvent(absl::Microseconds(4096), [&]() { env.Stop(); }));

  EXPECT_OK(env.ScheduleEvent(absl::Microseconds(2100), [&]() {
    for (uint64_t i = 0; i < 2 * ts_config.timing_wheel_slots(); i++) {
      std::unique_ptr<Packet> p = packet_builder.DequeuePacket();
      ASSERT_NE(p, nullptr);
      EXPECT_EQ(p->falcon.psn, i);
    }
  }));

  env.Run();
}

}  // namespace
}  // namespace isekai
