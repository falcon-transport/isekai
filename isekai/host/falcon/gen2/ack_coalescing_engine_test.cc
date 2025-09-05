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

#include "isekai/host/falcon/gen2/ack_coalescing_engine.h"

#include <cstdint>
#include <memory>

#include "absl/log/check.h"
#include "absl/time/time.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/constants.h"
#include "isekai/common/default_config_generator.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/common/packet.h"
#include "isekai/common/testing.h"
#include "isekai/host/falcon/falcon.h"
#include "isekai/host/falcon/falcon_bitmap.h"
#include "isekai/host/falcon/falcon_connection_state.h"
#include "isekai/host/falcon/falcon_testing_helpers.h"
#include "isekai/host/falcon/gen1/ack_coalescing_engine.h"
#include "isekai/host/falcon/gen1/falcon_model.h"
#include "isekai/host/falcon/gen1/falcon_types.h"

namespace isekai {
namespace {

using ::testing::_;

// This defines all the objects needed for setup and testing.
class Gen2AckCoalescingEngineTest
    : public FalconTestingHelpers::FalconTestSetup,
      public ::testing::Test {};

// Helper to manually allocate ReceiverReliabilityMetadata bitmaps when
// ReceiverReliabilityMetadata is not created in a ConnectionState.
void AllocateReceiveBitmaps(
    ConnectionState::ReceiverReliabilityMetadata& rx_reliability_metadata) {
  rx_reliability_metadata.request_window_metadata.ack_window =
      std::make_unique<Gen2FalconRxBitmap>(kGen2RxBitmapWidth);
  rx_reliability_metadata.request_window_metadata.receive_window =
      std::make_unique<Gen2FalconRxBitmap>(kGen2RxBitmapWidth);
  rx_reliability_metadata.data_window_metadata.ack_window =
      std::make_unique<Gen2FalconRxBitmap>(kGen2RxBitmapWidth);
  rx_reliability_metadata.data_window_metadata.receive_window =
      std::make_unique<Gen2FalconRxBitmap>(kGen2RxBitmapWidth);
}

TEST_F(Gen2AckCoalescingEngineTest, Gen2FillAckBitmap) {
  constexpr int kFalconVersion = 2;
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(kFalconVersion);
  InitFalcon(config);

  Gen2AckCoalescingEngine* gen2_ack_coalescing_engine =
      static_cast<Gen2AckCoalescingEngine*>(ack_coalescing_engine_);

  // Prepare rx_window data.
  ConnectionState::ReceiverReliabilityMetadata rx_reliability_metadata;
  AllocateReceiveBitmaps(rx_reliability_metadata);
  rx_reliability_metadata.request_window_metadata.ack_window->Set(1, 1);
  rx_reliability_metadata.request_window_metadata.ack_window->Set(129, 1);
  rx_reliability_metadata.data_window_metadata.ack_window->Set(2, 1);
  rx_reliability_metadata.data_window_metadata.receive_window->Set(3, 1);

  // Fill ack packet and check the results.
  auto ack_packet = std::make_unique<Packet>();
  gen2_ack_coalescing_engine->FillInAckBitmaps(ack_packet.get(),
                                               rx_reliability_metadata);
  EXPECT_EQ(ack_packet->ack.receiver_request_bitmap.Get(1), 1);
  EXPECT_EQ(ack_packet->ack.receiver_data_bitmap.Get(2), 1);
  EXPECT_EQ(ack_packet->ack.received_bitmap.Get(3), 1);
}

// Tests that ACK is sent when ACK coalescing count threshold for the
// (connection, flow) ACK coalescing entry is reached for pull requests.
TEST_F(Gen2AckCoalescingEngineTest, AckCoalescingTimeouts) {
  constexpr int kFalconVersion = 2;
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(kFalconVersion);
  // Set the parametrized test values in FalconConfig and initialize Falcon test
  // setup.
  constexpr uint8_t kNumFlowsPerConnection = 4;
  InitFalcon(config);

  // Initialize the connection state.
  auto connection_metadata =
      FalconTestingHelpers::InitializeConnectionMetadata(falcon_.get());
  // Set ACK coalescing threshold to be = 1 and timeout = 50us.
  connection_metadata->ack_coalescing_threshold = 2;
  uint64_t ack_coalescing_timeout_us = 50;
  connection_metadata->ack_coalescing_timeout =
      absl::Microseconds(ack_coalescing_timeout_us);
  FalconTestingHelpers::SetMultipathingDegree(connection_metadata.get(),
                                              kNumFlowsPerConnection);

  FalconTestingHelpers::InitializeConnectionState(
      falcon_.get(), std::move(connection_metadata));

  // Packet 1 with flow_id = 1 arrives at 0us.
  std::unique_ptr<Packet> rx_packet1 = FalconTestingHelpers::CreatePacket(
      /*packet_type=*/falcon::PacketType::kPullRequest, /*dest_cid=*/1,
      /*psn=*/1,
      /*rsn=*/1, /*ack_req=*/false, /*flow_label=*/1);
  EXPECT_OK(reliability_manager_->ReceivePacket(rx_packet1.get()));

  // Packet 2 with flow_id = 2 arrives at 5us. This should not trigger an ACK Tx
  // because the packet is on a different flow compared to Packet 1.
  std::unique_ptr<Packet> rx_packet2 = FalconTestingHelpers::CreatePacket(
      /*packet_type=*/falcon::PacketType::kPullRequest, /*dest_cid=*/1,
      /*psn=*/2,
      /*rsn=*/1, /*ack_req=*/false, /*flow_label=*/2);
  EXPECT_OK(env_.ScheduleEvent(absl::Microseconds(5), [&]() {
    EXPECT_OK(reliability_manager_->ReceivePacket(rx_packet2.get()));
  }));

  // Packet 3 with flow_id = 1 arrives at 5us. This should trigger an ACK Tx
  // because the ACK threshold of 2 is reached for flow_id = 1.
  std::unique_ptr<Packet> rx_packet3 = FalconTestingHelpers::CreatePacket(
      /*packet_type=*/falcon::PacketType::kPullRequest, /*dest_cid=*/1,
      /*psn=*/3,
      /*rsn=*/1, /*ack_req=*/false,
      /*flow_label=*/1 + kNumFlowsPerConnection);
  EXPECT_OK(env_.ScheduleEvent(absl::Microseconds(10), [&]() {
    EXPECT_OK(reliability_manager_->ReceivePacket(rx_packet3.get()));
  }));

  EXPECT_CALL(shaper_, TransferTxPacket(_))
      .WillOnce([&](std::unique_ptr<Packet> p) {
        EXPECT_EQ(p->packet_type, falcon::PacketType::kAck);
        // ACK should get out when Packet 3 is processed, at 10us.
        EXPECT_EQ(env_.ElapsedTime(), absl::Microseconds(10));
        // ACK should be from flow_id = 2, and more specifically with the same
        // flow_label as Packet 3.
        EXPECT_EQ(p->metadata.flow_label, rx_packet3->metadata.flow_label);
      });

  // Run only for 20us, less than the ACK coalescing timeout of 50us. We only
  // want to test ACK coalescing due to counts.
  env_.RunFor(absl::Microseconds(20));
}

// Tests that ACK is sent when ACK coalescing timeout for the (connection, flow)
// ACK coalescing entry is reached.
TEST_F(Gen2AckCoalescingEngineTest, AckCoalescingPullRequestCountTest) {
  constexpr int kFalconVersion = 2;
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(kFalconVersion);
  // Set the parametrized test values in FalconConfig and initialize Falcon test
  // setup.
  constexpr uint8_t kNumFlowsPerConnection = 4;
  InitFalcon(config);

  // Initialize the connection state.
  auto connection_metadata =
      FalconTestingHelpers::InitializeConnectionMetadata(falcon_.get());
  // Set ACK coalescing threshold to be = 1 and timeout = 50us.
  connection_metadata->ack_coalescing_threshold = 100;
  uint64_t ack_coalescing_timeout_us = 10;
  connection_metadata->ack_coalescing_timeout =
      absl::Microseconds(ack_coalescing_timeout_us);
  FalconTestingHelpers::SetMultipathingDegree(connection_metadata.get(),
                                              kNumFlowsPerConnection);
  FalconTestingHelpers::InitializeConnectionState(
      falcon_.get(), std::move(connection_metadata));

  // Packet 1 with flow_id = 1 arrives at 0us. ACK coalescing threshold is not
  // reached.
  std::unique_ptr<Packet> rx_packet1 = FalconTestingHelpers::CreatePacket(
      /*packet_type=*/falcon::PacketType::kPullRequest, /*dest_cid=*/1,
      /*psn=*/1,
      /*rsn=*/1, /*ack_req=*/false, /*flow_label=*/1);
  EXPECT_OK(reliability_manager_->ReceivePacket(rx_packet1.get()));

  // Packet 2 with flow_id = 2 arrives at 1us. ACK coalescing threshold is not
  // reached.
  std::unique_ptr<Packet> rx_packet2 = FalconTestingHelpers::CreatePacket(
      /*packet_type=*/falcon::PacketType::kPullRequest, /*dest_cid=*/1,
      /*psn=*/2,
      /*rsn=*/1, /*ack_req=*/false, /*flow_label=*/2);
  EXPECT_OK(env_.ScheduleEvent(absl::Microseconds(1), [&]() {
    EXPECT_OK(reliability_manager_->ReceivePacket(rx_packet2.get()));
  }));

  // All 2 ACK coalescing entries should timeout after ack_coalescing_timeout.
  EXPECT_CALL(shaper_, TransferTxPacket(_))
      .WillOnce([&](std::unique_ptr<Packet> p) {
        EXPECT_EQ(p->packet_type, falcon::PacketType::kAck);
        // ACK should get out when Packet 3 is processed, at 10us.
        EXPECT_EQ(env_.ElapsedTime(),
                  absl::Microseconds(ack_coalescing_timeout_us));
        // ACK should be from flow_id = 1, and more specifically with the same
        // flow_label as Packet 1.
        EXPECT_EQ(p->metadata.flow_label, rx_packet1->metadata.flow_label);
      })
      .WillOnce([&](std::unique_ptr<Packet> p) {
        EXPECT_EQ(p->packet_type, falcon::PacketType::kAck);
        // ACK should get out when Packet 3 is processed, at 10us.
        EXPECT_EQ(env_.ElapsedTime(),
                  absl::Microseconds(1 + ack_coalescing_timeout_us));
        // ACK should be from flow_id = 2, and more specifically with the same
        // flow_label as Packet 2.
        EXPECT_EQ(p->metadata.flow_label, rx_packet2->metadata.flow_label);
      });

  // Run for completion.
  env_.Run();
}

// Tests that  ACK is sent and ACK coalescing entry is reset when the AckReqBit
// is set on a packet.
TEST_F(Gen2AckCoalescingEngineTest, AckReqBitSet) {
  constexpr int kFalconVersion = 2;
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(kFalconVersion);
  // Set the parametrized test values in FalconConfig and initialize Falcon test
  // setup.
  constexpr uint8_t kNumFlowsPerConnection = 4;
  InitFalcon(config);

  // Initialize the connection state.
  auto connection_metadata =
      FalconTestingHelpers::InitializeConnectionMetadata(falcon_.get());
  // Set ACK coalescing threshold to be = 1 and timeout = 50us.
  connection_metadata->ack_coalescing_threshold = 100;
  uint64_t ack_coalescing_timeout_us = 100;
  connection_metadata->ack_coalescing_timeout =
      absl::Microseconds(ack_coalescing_timeout_us);
  FalconTestingHelpers::SetMultipathingDegree(connection_metadata.get(),
                                              kNumFlowsPerConnection);
  FalconTestingHelpers::InitializeConnectionState(
      falcon_.get(), std::move(connection_metadata));

  // Packet 1 with flow_id = 1 arrives at 0us. ACK coalescing threshold is not
  // reached.
  std::unique_ptr<Packet> rx_packet1 = FalconTestingHelpers::CreatePacket(
      /*packet_type=*/falcon::PacketType::kPullRequest, /*dest_cid=*/1,
      /*psn=*/1,
      /*rsn=*/1, /*ack_req=*/false, /*flow_label=*/1);
  EXPECT_OK(reliability_manager_->ReceivePacket(rx_packet1.get()));

  // Packet 2 with flow_id = 2 arrives at 1us. ACK coalescing threshold is not
  // reached.
  std::unique_ptr<Packet> rx_packet2 = FalconTestingHelpers::CreatePacket(
      /*packet_type=*/falcon::PacketType::kPullRequest, /*dest_cid=*/1,
      /*psn=*/2,
      /*rsn=*/1, /*ack_req=*/false, /*flow_label=*/2);
  EXPECT_OK(env_.ScheduleEvent(absl::Microseconds(5), [&]() {
    EXPECT_OK(reliability_manager_->ReceivePacket(rx_packet2.get()));
  }));

  // Packet 3 with flow_id = 1 arrives at 2us. This should trigger an ACK Tx
  // because the ACK request bit is set.
  std::unique_ptr<Packet> rx_packet3 = FalconTestingHelpers::CreatePacket(
      /*packet_type=*/falcon::PacketType::kPullRequest, /*dest_cid=*/1,
      /*psn=*/3,
      /*rsn=*/1, /*ack_req=*/true, /*flow_label=*/1 + kNumFlowsPerConnection);
  EXPECT_OK(env_.ScheduleEvent(absl::Microseconds(10), [&]() {
    EXPECT_OK(reliability_manager_->ReceivePacket(rx_packet3.get()));
  }));

  // 2 ACK packets should be sent out.
  EXPECT_CALL(shaper_, TransferTxPacket(_))
      .WillOnce([&](std::unique_ptr<Packet> p) {
        EXPECT_EQ(p->packet_type, falcon::PacketType::kAck);
        // ACK should get out when Packet 3 is processed, at 10us.
        EXPECT_EQ(env_.ElapsedTime(), absl::Microseconds(10));
        // ACK should be from flow_id = 1, and more specifically with the same
        // flow_label as Packet 3.
        EXPECT_EQ(p->metadata.flow_label, rx_packet3->metadata.flow_label);
      })
      .WillOnce([&](std::unique_ptr<Packet> p) {
        EXPECT_EQ(p->packet_type, falcon::PacketType::kAck);
        // ACK should get out when the ACK coalescing timer for flow_id=2 times
        // out.
        EXPECT_EQ(env_.ElapsedTime(),
                  absl::Microseconds(5 + ack_coalescing_timeout_us));
        // ACK should be from flow_id = 2, and more specifically with the same
        // flow_label as Packet 2.
        EXPECT_EQ(p->metadata.flow_label, rx_packet2->metadata.flow_label);
      });

  // Run for completion.
  env_.Run();
}

// Test that the congestion control metadata are correctly reflected in an ack
// from a data packet.
TEST_F(Gen2AckCoalescingEngineTest, AckCongestionControlReflectionTest) {
  constexpr int kFalconVersion = 2;
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(kFalconVersion);
  // Set the parametrized test values in FalconConfig and initialize Falcon test
  // setup.
  constexpr uint8_t kNumFlowsPerConnection = 4;
  InitFalcon(config);
  // Initialize the connection state.
  auto connection_metadata =
      FalconTestingHelpers::InitializeConnectionMetadata(falcon_.get());
  FalconTestingHelpers::SetMultipathingDegree(connection_metadata.get(),
                                              kNumFlowsPerConnection);
  ConnectionState* connection_state =
      FalconTestingHelpers::InitializeConnectionState(
          falcon_.get(), std::move(connection_metadata));
  // Set the flow label values.
  uint32_t congestion_metadata_flowlabel = 1;
  uint32_t rx_packet_flowlabel = 2;
  CongestionControlMetadata& ccmeta =
      *connection_state->congestion_control_metadata;
  ccmeta.flow_label = congestion_metadata_flowlabel;

  // Create a fake incoming transaction with the required fields setup.
  std::unique_ptr<Packet> rx_packet = FalconTestingHelpers::CreatePacket(
      /*packet_type=*/falcon::PacketType::kPullRequest, /*dest_cid=*/1,
      /*psn=*/63, /*rsn=*/1, /*ack_req=*/false,
      /*flow_label=*/rx_packet_flowlabel);
  rx_packet->timestamps.sent_timestamp = absl::Microseconds(1);
  rx_packet->timestamps.received_timestamp = absl::Microseconds(2);
  rx_packet->metadata.forward_hops = 1;
  EXPECT_OK(reliability_manager_->ReceivePacket(rx_packet.get()));

  EXPECT_CALL(shaper_, TransferTxPacket(_))
      .WillOnce([&](std::unique_ptr<Packet> p) {
        EXPECT_EQ(p->packet_type, falcon::PacketType::kAck);
        EXPECT_EQ(p->ack.timestamp_1, absl::Microseconds(1));
        EXPECT_EQ(p->ack.timestamp_2, absl::Microseconds(2));
        EXPECT_EQ(p->ack.forward_hops, 1);
        // For Gen2, ACK should have rx_packet_flowlabel.
        EXPECT_EQ(p->metadata.flow_label, rx_packet_flowlabel);
      });
  env_.Run();
}

// Tests that an ACK piggyback is happening per <connection, flow> entry.
TEST_F(Gen2AckCoalescingEngineTest, AckCoalescingWithPiggyback) {
  constexpr int kFalconVersion = 2;
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(kFalconVersion);
  // Set the parametrized test values in FalconConfig and initialize Falcon test
  // setup.
  constexpr uint8_t kNumFlowsPerConnection = 4;
  InitFalcon(config);

  // Initialize the connection state.
  auto connection_metadata =
      FalconTestingHelpers::InitializeConnectionMetadata(falcon_.get());
  uint32_t scid = connection_metadata->scid;
  // Set ack coalescing threshold to be = 10.
  connection_metadata->ack_coalescing_threshold = 10;
  connection_metadata->ack_coalescing_timeout = absl::Microseconds(50);
  FalconTestingHelpers::SetMultipathingDegree(connection_metadata.get(),
                                              kNumFlowsPerConnection);
  ConnectionState* connection_state =
      FalconTestingHelpers::InitializeConnectionState(
          falcon_.get(), std::move(connection_metadata));

  connection_state->rx_reliability_metadata.request_window_metadata
      .base_packet_sequence_number = 0;

  // In order pull request at PSN 0 arrive at 0us on flow_id 1.
  std::unique_ptr<Packet> rx_packet1 = FalconTestingHelpers::CreatePacket(
      /*packet_type=*/falcon::PacketType::kPullRequest, /*dest_cid=*/1,
      /*psn=*/0,
      /*rsn=*/0, /*ack_req=*/false, /*flow_label=*/1);
  EXPECT_OK(reliability_manager_->ReceivePacket(rx_packet1.get()));

  // OOO pull request at PSN 2 arrive at 1us on flow_id 0.
  std::unique_ptr<Packet> rx_packet2 = FalconTestingHelpers::CreatePacket(
      /*packet_type=*/falcon::PacketType::kPullRequest, /*dest_cid=*/1,
      /*psn=*/2,
      /*rsn=*/2, /*ack_req=*/false, /*flow_label=*/0);
  EXPECT_OK(env_.ScheduleEvent(absl::Microseconds(1), [&]() {
    EXPECT_OK(reliability_manager_->ReceivePacket(rx_packet2.get()));
  }));

  // Packet 3 fills OOO gap on flow_id 1.
  std::unique_ptr<Packet> rx_packet3 = FalconTestingHelpers::CreatePacket(
      /*packet_type=*/falcon::PacketType::kPullRequest, /*dest_cid=*/1,
      /*psn=*/1,
      /*rsn=*/1, /*ack_req=*/false,
      /*flow_label=*/1 + kNumFlowsPerConnection);
  EXPECT_OK(env_.ScheduleEvent(absl::Microseconds(2), [&]() {
    EXPECT_OK(reliability_manager_->ReceivePacket(rx_packet3.get()));
  }));

  // TX packet on flow_id 1 piggybacks that flow's ACK coalescing entry.
  std::unique_ptr<AckCoalescingKey> ack_coalescing_key_0 =
      CreateAckCoalescingKey(scid, 0);
  std::unique_ptr<AckCoalescingKey> ack_coalescing_key_1 =
      CreateAckCoalescingKey(scid, 1);
  auto tx_packet = std::make_unique<Packet>();
  EXPECT_OK(env_.ScheduleEvent(absl::Microseconds(3), [&]() {
    EXPECT_OK(ack_coalescing_engine_->PiggybackAck(*ack_coalescing_key_1,
                                                   tx_packet.get()));
  }));

  // After the 3rd packet rx.
  env_.RunFor(absl::Microseconds(2));
  // ACK coalescing entry counter should be 2 (2 packets * 1) for flow_id 0.
  absl::StatusOr<const AckCoalescingEntry*> ack_coalescing_entry_0 =
      GetAckCoalescingEntry(ack_coalescing_key_0.get());
  EXPECT_OK(ack_coalescing_entry_0);
  EXPECT_EQ(ack_coalescing_entry_0.value()->coalescing_counter, 1 * 2);
  // ACK coalescing entry counter should be 4 (2 packets * 2) for flow_id 1.
  absl::StatusOr<const AckCoalescingEntry*> ack_coalescing_entry_1 =
      GetAckCoalescingEntry(ack_coalescing_key_1.get());
  EXPECT_OK(ack_coalescing_entry_1);
  EXPECT_EQ(ack_coalescing_entry_1.value()->coalescing_counter, 2 * 2);
  // For the connection: implicitly_acked_counter should be 3.
  EXPECT_EQ(connection_state->rx_reliability_metadata.implicitly_acked_counter,
            3);

  // After the piggyback ACK.
  env_.RunFor(absl::Microseconds(1));
  // ACK coalescing counter should still be 2 for flow_id 0.
  EXPECT_EQ(ack_coalescing_entry_0.value()->coalescing_counter, 1 * 2);
  // ACK coalescing counter should be 0 for flow_id 1.
  EXPECT_EQ(ack_coalescing_entry_1.value()->coalescing_counter, 0);
  // For the connection: implicitly_acked_counter should be 0.
  EXPECT_EQ(connection_state->rx_reliability_metadata.implicitly_acked_counter,
            0);
}

// Tests that an ACK is sent when ACK coalescing count threshold is reached for
// incoming push unsolicited data after the ULP ACK is received (but not after
// a ULP NACK which should only cause a NACK packet to be transmitted).
TEST_F(Gen2AckCoalescingEngineTest, AckCoalescingPushUnsolicitedAckAndNack) {
  constexpr int kFalconVersion = 2;
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(kFalconVersion);
  // Set the parametrized test values in FalconConfig and initialize Falcon test
  // setup.
  constexpr uint8_t kNumFlowsPerConnection = 4;
  InitFalcon(config);

  // Initialize the connection state.
  auto connection_metadata =
      FalconTestingHelpers::InitializeConnectionMetadata(falcon_.get());
  uint32_t scid = connection_metadata->scid;
  // Set ACK coalescing threshold to be = 1 and timeout = 50us.
  connection_metadata->ack_coalescing_threshold = 1;
  uint64_t ack_coalescing_timeout_us = 50;
  connection_metadata->ack_coalescing_timeout =
      absl::Microseconds(ack_coalescing_timeout_us);
  FalconTestingHelpers::SetMultipathingDegree(connection_metadata.get(),
                                              kNumFlowsPerConnection);
  auto connection_state = FalconTestingHelpers::InitializeConnectionState(
      falcon_.get(), std::move(connection_metadata));

  // We need to setup the transaction with rsn=1 that will be NACKed.
  uint32_t rsn = 1;
  FalconTestingHelpers::SetupTransactionWithConnectionState(
      connection_state, TransactionType::kPushUnsolicited,
      TransactionLocation::kTarget,
      TransactionState::kPushUnsolicitedDataNtwkRx,
      falcon::PacketType::kPushUnsolicitedData, rsn, 0);

  // Packet 1 with flow_id = 1 arrives at 0us. ACK coalescing threshold is not
  // reached.
  std::unique_ptr<Packet> rx_packet1 = FalconTestingHelpers::CreatePacket(
      /*packet_type=*/falcon::PacketType::kPushUnsolicitedData, /*dest_cid=*/1,
      /*psn=*/1, /*rsn=*/1, /*ack_req=*/false, /*flow_label=*/1);
  rx_packet1->metadata.forward_hops = 4;
  std::unique_ptr<OpaqueCookie> cookie1 =
      falcon_->CreateCookieForTesting(*rx_packet1);
  EXPECT_OK(reliability_manager_->ReceivePacket(rx_packet1.get()));

  // Packet 2 with flow_id = 2 arrives at 1us. ACK coalescing threshold is not
  // reached.
  std::unique_ptr<Packet> rx_packet2 = FalconTestingHelpers::CreatePacket(
      /*packet_type=*/falcon::PacketType::kPushUnsolicitedData, /*dest_cid=*/1,
      /*psn=*/2, /*rsn=*/2, /*ack_req=*/false, /*flow_label=*/2);
  rx_packet2->metadata.forward_hops = 8;
  std::unique_ptr<OpaqueCookie> cookie2 =
      falcon_->CreateCookieForTesting(*rx_packet2);
  EXPECT_OK(env_.ScheduleEvent(absl::Microseconds(1), [&]() {
    EXPECT_OK(reliability_manager_->ReceivePacket(rx_packet2.get()));
  }));

  // ULP ACKs Packet 2 data at 2us. ACK coalescing threshold is now reached.
  EXPECT_OK(env_.ScheduleEvent(absl::Microseconds(2), [&]() {
    EXPECT_OK(reliability_manager_->HandleAckFromUlp(
        scid, rx_packet2->falcon.rsn, *cookie2));
  }));

  // ULP NACKs Packet 1 data at 3us. ACK coalescing threshold is still not
  // reached, but NACK has to be transmitted. ACK will be transmitted after the
  // ACK coalescing timeout.
  UlpNackMetadata nack_metadata = UlpNackMetadata();
  nack_metadata.rnr_timeout = absl::Microseconds(10);
  nack_metadata.ulp_nack_code = Packet::Syndrome::kRnrNak;
  EXPECT_OK(env_.ScheduleEvent(absl::Microseconds(3), [&]() {
    EXPECT_OK(reliability_manager_->HandleNackFromUlp(
        scid, {rx_packet1->falcon.rsn, TransactionLocation::kTarget},
        &nack_metadata, *cookie1));
  }));

  EXPECT_CALL(shaper_, TransferTxPacket(_))
      .WillOnce([&](std::unique_ptr<Packet> p) {
        EXPECT_EQ(p->packet_type, falcon::PacketType::kAck);
        // For push unsolicited, ACK coalescing count is incremented when the
        // ULP ACK is received.
        EXPECT_EQ(env_.ElapsedTime(), absl::Microseconds(2));
        // Correct flow metadata gets reflected.
        EXPECT_EQ(p->metadata.flow_label, rx_packet2->metadata.flow_label);
        EXPECT_EQ(p->ack.forward_hops, rx_packet2->metadata.forward_hops);
      })
      .WillOnce([&](std::unique_ptr<Packet> p) {
        EXPECT_EQ(p->packet_type, falcon::PacketType::kNack);
        // For flow 1, a NACK packet will be transmitted when the packet
        // reliability manager handles the ULP NACK.
        EXPECT_EQ(env_.ElapsedTime(), absl::Microseconds(3));
        // Correct flow metadata gets reflected.
        EXPECT_EQ(p->metadata.flow_label, rx_packet1->metadata.flow_label);
        EXPECT_EQ(p->nack.forward_hops, rx_packet1->metadata.forward_hops);
      })
      .WillOnce([&](std::unique_ptr<Packet> p) {
        EXPECT_EQ(p->packet_type, falcon::PacketType::kAck);
        // ACK coalescing timeout will trigger an ACK TX for flow 1 even after
        // NACK is transmitted.
        EXPECT_EQ(env_.ElapsedTime(),
                  absl::Microseconds(ack_coalescing_timeout_us));
        // Correct flow metadata gets reflected.
        EXPECT_EQ(p->metadata.flow_label, rx_packet1->metadata.flow_label);
        EXPECT_EQ(p->ack.forward_hops, rx_packet1->metadata.forward_hops);
      });
  env_.Run();
}

}  // namespace
}  // namespace isekai
