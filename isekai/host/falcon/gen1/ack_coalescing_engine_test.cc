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

#include "isekai/host/falcon/gen1/ack_coalescing_engine.h"

#include <sys/types.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/time/time.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/default_config_generator.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/common/packet.h"
#include "isekai/common/simple_environment.h"
#include "isekai/common/testing.h"
#include "isekai/host/falcon/falcon.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/falcon_connection_state.h"
#include "isekai/host/falcon/falcon_testing_helpers.h"
#include "isekai/host/falcon/gen1/falcon_model.h"
#include "isekai/host/falcon/gen1/falcon_types.h"

namespace isekai {

namespace {

using ::testing::_;

// This defines all the objects needed for setup and testing. The test fixture
// is parameterized on the Falcon version.
class AckCoalescingEngineTest
    : public FalconTestingHelpers::FalconTestSetup,
      public testing::TestWithParam</*falcon_version*/ int> {
 protected:
  int GetFalconVersion() { return GetParam(); }
};

// ACK is sent when ACK coalescing count threshold is reached for pull
// requests.
TEST_P(AckCoalescingEngineTest, AckCoalescingPullRequestCountTest) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
  // Set the parametrized test values in FalconConfig and initialize Falcon test
  // setup.
  InitFalcon(config);

  // Initialize the connection state.
  auto connection_metadata =
      FalconTestingHelpers::InitializeConnectionMetadata(falcon_.get());
  // Set ack coalescing threshold to be = 1 and timeout = 50us.
  connection_metadata->ack_coalescing_threshold = 1;
  uint64_t ack_coalescing_timeout_us = 50;
  connection_metadata->ack_coalescing_timeout =
      absl::Microseconds(ack_coalescing_timeout_us);
  FalconTestingHelpers::InitializeConnectionState(
      falcon_.get(), std::move(connection_metadata));

  // Create a fake incoming transaction with the required fields setup.
  auto rx_packet = std::make_unique<Packet>();
  rx_packet->packet_type = falcon::PacketType::kPullRequest;
  rx_packet->falcon.dest_cid = 1;
  rx_packet->falcon.psn = 63;  // PSN within the window
  rx_packet->falcon.rsn = 1;
  rx_packet->falcon.ack_req = false;  // Request immediate ACK
  EXPECT_OK(reliability_manager_->ReceivePacket(rx_packet.get()));

  EXPECT_CALL(shaper_, TransferTxPacket(_))
      .WillOnce([&](std::unique_ptr<Packet> p) {
        EXPECT_EQ(p->packet_type, falcon::PacketType::kAck);
        EXPECT_EQ(env_.ElapsedTime(), absl::ZeroDuration());
      });
  env_.Run();
}

// ACK is sent when ACK coalescing count threshold is reached for incoming
// push unsolicited data (after the ULP ACK).
TEST_P(AckCoalescingEngineTest, AckCoalescingPushUnsolicitedCountTest) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
  // Set the parametrized test values in FalconConfig and initialize Falcon test
  // setup.
  InitFalcon(config);
  // Initialize the connection state.
  auto connection_metadata =
      FalconTestingHelpers::InitializeConnectionMetadata(falcon_.get());
  uint32_t source_connection_id = connection_metadata->scid;
  // Set ack coalescing threshold to be = 1 and timeout = 50us.
  connection_metadata->ack_coalescing_threshold = 1;
  uint64_t ack_coalescing_timeout_us = 50;
  connection_metadata->ack_coalescing_timeout =
      absl::Microseconds(ack_coalescing_timeout_us);
  FalconTestingHelpers::InitializeConnectionState(
      falcon_.get(), std::move(connection_metadata));

  // Create a fake incoming transaction with the required fields setup.
  auto rx_packet = std::make_unique<Packet>();
  rx_packet->packet_type = falcon::PacketType::kPushUnsolicitedData;
  rx_packet->falcon.dest_cid = 1;
  rx_packet->falcon.psn = 63;  // PSN within the window
  rx_packet->falcon.rsn = 1;
  rx_packet->falcon.ack_req = false;  // Request immediate ACK
  std::unique_ptr<OpaqueCookie> cookie =
      falcon_->CreateCookieForTesting(*rx_packet);

  EXPECT_OK(reliability_manager_->ReceivePacket(rx_packet.get()));
  // ULP Acks data at 1us.
  EXPECT_OK(env_.ScheduleEvent(absl::Microseconds(1), [&]() {
    EXPECT_OK(reliability_manager_->HandleAckFromUlp(source_connection_id, 1,
                                                     *cookie));
  }));

  EXPECT_CALL(shaper_, TransferTxPacket(_))
      .WillOnce([&](std::unique_ptr<Packet> p) {
        EXPECT_EQ(p->packet_type, falcon::PacketType::kAck);
        // For push unsolicited, ACK coalescing count is incremented when the
        // ULP ACK is received.
        EXPECT_EQ(env_.ElapsedTime(), absl::Microseconds(1));
      });
  env_.Run();
}

// In presence of OOW drops, ACK coalescing count is not mutated on sending out
// piggybacked ACKs.
TEST_P(AckCoalescingEngineTest, AckCoalescingOowDropTest) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
  // Set the parametrized test values in FalconConfig and initialize Falcon test
  // setup.
  InitFalcon(config);
  // Initialize connection metadata with the relevant ACK coalescing parameters.
  auto connection_metadata =
      FalconTestingHelpers::InitializeConnectionMetadata(falcon_.get());
  uint32_t source_connection_id = connection_metadata->scid;
  // Set the ACK coalescing counter to be 2.
  connection_metadata->ack_coalescing_threshold = 2;
  // Initialize connection state.
  auto connection_state = FalconTestingHelpers::InitializeConnectionState(
      falcon_.get(), std::move(connection_metadata));

  // Receive an OOW packet in the request window.
  auto rx_packet = std::make_unique<Packet>();
  rx_packet->packet_type = falcon::PacketType::kPullRequest;
  rx_packet->falcon.dest_cid = 1;
  rx_packet->falcon.psn = 270;  // PSN outside the window
  rx_packet->falcon.rsn = 1;
  EXPECT_EQ(reliability_manager_->ReceivePacket(rx_packet.get()).code(),
            absl::StatusCode::kOutOfRange);

  // Create and send a Pull Request on the same connection which carries
  // Piggybacked ACKs.
  FalconTestingHelpers::SetupTransactionWithConnectionState(
      connection_state, TransactionType::kPull, TransactionLocation::kInitiator,
      TransactionState::kPullReqUlpRx, falcon::PacketType::kPullRequest, 0, 0);
  EXPECT_OK(reliability_manager_->TransmitPacket(
      source_connection_id, 0, falcon::PacketType::kPullRequest));

  env_.RunFor(absl::Nanoseconds(5));

  // Receive another packet, but within the request window.
  auto another_rx_packet = std::make_unique<Packet>();
  another_rx_packet->packet_type = falcon::PacketType::kPullRequest;
  another_rx_packet->falcon.dest_cid = 1;
  another_rx_packet->falcon.psn = 60;
  another_rx_packet->falcon.rsn = 0;
  EXPECT_OK(env_.ScheduleEvent(absl::Nanoseconds(10), [&]() {
    EXPECT_OK(reliability_manager_->ReceivePacket(another_rx_packet.get()));
  }));

  // Inspite of the piggybacked ACK, we expect an explicit ACK to be sent as
  // the ACK coalescing counter is not reset while sending out piggybacked ACKs
  // if it experiences OOW drops.
  EXPECT_CALL(shaper_, TransferTxPacket(_))
      .WillOnce([&](std::unique_ptr<Packet> p) {
        EXPECT_EQ(p->packet_type, falcon::PacketType::kAck);
        EXPECT_EQ(env_.ElapsedTime(), absl::Nanoseconds(15));
      });
  env_.RunFor(absl::Nanoseconds(15));
}

// ACK is sent when ACK coalescing timer expires.
TEST_P(AckCoalescingEngineTest, AckCoalescingTimerTest) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
  // Set the parametrized test values in FalconConfig and initialize Falcon test
  // setup.
  InitFalcon(config);
  // Initialize the connection state.
  auto connection_metadata =
      FalconTestingHelpers::InitializeConnectionMetadata(falcon_.get());
  // Set ack coalescing threshold to be = 100 and timeout = 50ns.
  connection_metadata->ack_coalescing_threshold = 100;
  uint64_t ack_coalescing_timeout_ns = 50;
  connection_metadata->ack_coalescing_timeout =
      absl::Nanoseconds(ack_coalescing_timeout_ns);
  FalconTestingHelpers::InitializeConnectionState(
      falcon_.get(), std::move(connection_metadata));

  // Create a fake incoming transaction with the required fields setup.
  auto rx_packet = std::make_unique<Packet>();
  rx_packet->packet_type = falcon::PacketType::kPullRequest;
  rx_packet->falcon.dest_cid = 1;
  rx_packet->falcon.psn = 63;         // PSN within the window
  rx_packet->falcon.ack_req = false;  // Request immediate ACK
  rx_packet->falcon.rsn = 0;

  EXPECT_OK(reliability_manager_->ReceivePacket(rx_packet.get()));

  EXPECT_CALL(shaper_, TransferTxPacket(_))
      .WillOnce([&](std::unique_ptr<Packet> p) {
        EXPECT_EQ(p->packet_type, falcon::PacketType::kAck);
        EXPECT_EQ(env_.ElapsedTime(),
                  absl::Nanoseconds(ack_coalescing_timeout_ns));
      });
  env_.Run();
}

// NACK can be successfully transmitted with the right fields.
TEST_P(AckCoalescingEngineTest, TransmitNackTest) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
  // Set the parametrized test values in FalconConfig and initialize Falcon test
  // setup.
  InitFalcon(config);
  // Initialize the connection state.
  auto connection_metadata =
      FalconTestingHelpers::InitializeConnectionMetadata(falcon_.get());
  uint32_t source_connection_id = connection_metadata->scid;
  FalconTestingHelpers::InitializeConnectionState(
      falcon_.get(), std::move(connection_metadata));

  std::unique_ptr<AckCoalescingKey> ack_coalescing_key =
      CreateAckCoalescingKey(source_connection_id);
  ASSERT_OK(ack_coalescing_engine_->TransmitNACK(
      *ack_coalescing_key, 1, true, falcon::NackCode::kReserved, nullptr));

  EXPECT_CALL(shaper_, TransferTxPacket(_))
      .WillOnce([&](std::unique_ptr<Packet> p) {
        EXPECT_EQ(p->packet_type, falcon::PacketType::kNack);
        EXPECT_EQ(p->nack.rrbpsn, 0);
        EXPECT_EQ(p->nack.rdbpsn, 0);
        EXPECT_EQ(p->nack.request_window, true);
        EXPECT_EQ(p->nack.code, falcon::NackCode::kReserved);
        EXPECT_EQ(p->nack.nack_psn, 1);
        EXPECT_EQ(p->nack.timestamp_1, absl::ZeroDuration());
        EXPECT_EQ(p->nack.timestamp_2, absl::ZeroDuration());
        EXPECT_EQ(p->nack.rx_buffer_level, 0);
        EXPECT_EQ(p->nack.cc_metadata, 0);
        EXPECT_EQ(p->nack.forward_hops, 0);
      });
  env_.Run();
}

// ACK is sent when ACK coalescing count threshold is reached for incoming
// push unsolicited data (without ULP ACK getting received).
TEST_P(AckCoalescingEngineTest, AckCoalescingPushUnsolicitedCountNoUlpAckTest) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
  // Set the parametrized test values in FalconConfig and initialize Falcon test
  // setup.
  InitFalcon(config);
  // Initialize the connection state.
  auto connection_metadata =
      FalconTestingHelpers::InitializeConnectionMetadata(falcon_.get());
  // Set ack coalescing threshold to be = 1 and timeout = 50us.
  connection_metadata->ack_coalescing_threshold = 1;
  uint64_t ack_coalescing_timeout_us = 50;
  connection_metadata->ack_coalescing_timeout =
      absl::Microseconds(ack_coalescing_timeout_us);

  FalconTestingHelpers::InitializeConnectionState(
      falcon_.get(), std::move(connection_metadata));

  // Create a fake incoming transaction with the required fields setup.
  auto rx_packet = std::make_unique<Packet>();
  rx_packet->packet_type = falcon::PacketType::kPushUnsolicitedData;
  rx_packet->falcon.dest_cid = 1;
  rx_packet->falcon.psn = 3;  // PSN within the window
  rx_packet->falcon.rsn = 1;
  rx_packet->falcon.ack_req = false;  // Request immediate ACK

  // Create a fake incoming transaction with the required fields setup.
  auto rx_packet2 = std::make_unique<Packet>();
  rx_packet2->packet_type = falcon::PacketType::kPushUnsolicitedData;
  rx_packet2->falcon.dest_cid = 1;
  rx_packet2->falcon.psn = 5;  // PSN within the window
  rx_packet2->falcon.rsn = 3;
  rx_packet2->falcon.ack_req = false;  // Request immediate ACK

  // Create a fake incoming transaction with the required fields setup.
  auto rx_packet3 = std::make_unique<Packet>();
  rx_packet3->packet_type = falcon::PacketType::kPullRequest;
  rx_packet3->falcon.dest_cid = 1;
  rx_packet3->falcon.psn = 6;  // PSN within the window
  rx_packet3->falcon.rsn = 4;
  rx_packet3->falcon.ack_req = false;  // Request immediate ACK

  EXPECT_OK(reliability_manager_->ReceivePacket(rx_packet.get()));
  EXPECT_OK(env_.ScheduleEvent(absl::Microseconds(10), [&]() {
    EXPECT_OK(reliability_manager_->ReceivePacket(rx_packet2.get()));
  }));
  // There will be no ULP ACK for the push unsolicited data in
  // rx_packet and rx_packet2 in this test setup.
  EXPECT_OK(env_.ScheduleEvent(absl::Microseconds(20), [&]() {
    EXPECT_OK(reliability_manager_->ReceivePacket(rx_packet3.get()));
  }));

  EXPECT_CALL(shaper_, TransferTxPacket(_))
      .WillOnce([&](std::unique_ptr<Packet> p) {
        EXPECT_EQ(p->packet_type, falcon::PacketType::kAck);
        // Counter reaches threshold after second unsolicited push data.
        EXPECT_EQ(env_.ElapsedTime(), absl::Microseconds(10));
      })
      .WillOnce([&](std::unique_ptr<Packet> p) {
        EXPECT_EQ(p->packet_type, falcon::PacketType::kAck);
        // Counter reaches threshold again after third pull request.
        EXPECT_EQ(env_.ElapsedTime(), absl::Microseconds(20));
      });
  env_.Run();
}

// Tests ACK behaviour with OOO packets received, especially as it relates to
// the expected values in Rx and ACK bitmaps.
TEST_P(AckCoalescingEngineTest, AckCoalescingWithOOO) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
  // Set the parametrized test values in FalconConfig and initialize Falcon test
  // setup.
  InitFalcon(config);
  // Initialize the connection state.
  auto connection_metadata =
      FalconTestingHelpers::InitializeConnectionMetadata(falcon_.get());
  // Set ack coalescing threshold to be = 2
  connection_metadata->ack_coalescing_threshold = 2;
  auto ack_coalescing_timeout = connection_metadata->ack_coalescing_timeout;
  ConnectionState* connection_state =
      FalconTestingHelpers::InitializeConnectionState(
          falcon_.get(), std::move(connection_metadata));

  // We need to setup the transactions containing Pull Data.
  for (auto rsn : {0, 1, 4, 6}) {
    auto psn = 0;
    FalconTestingHelpers::SetupTransactionWithConnectionState(
        connection_state, TransactionType::kPull,
        TransactionLocation::kInitiator, TransactionState::kPullReqNtwkTx,
        falcon::PacketType::kPullRequest, rsn, psn);
  }

  connection_state->tx_reliability_metadata.request_window_metadata
      .base_packet_sequence_number = 1;

  // In order pull data at PSN 0 arrive at 0us.
  auto rx_packet = std::make_unique<Packet>();
  rx_packet->packet_type = falcon::PacketType::kPullData;
  rx_packet->falcon.dest_cid = 1;
  rx_packet->falcon.psn = 0;  // PSN within the window
  rx_packet->falcon.rsn = 0;
  rx_packet->falcon.ack_req = false;  // Request immediate ACK
  EXPECT_OK(reliability_manager_->ReceivePacket(rx_packet.get()));

  // OOO push data at PSN 2 arrive at 1us.
  auto rx_packet2 = std::make_unique<Packet>();
  rx_packet2->packet_type = falcon::PacketType::kPushUnsolicitedData;
  rx_packet2->falcon.dest_cid = 1;
  rx_packet2->falcon.psn = 2;  // PSN within the window
  rx_packet2->falcon.rsn = 2;
  rx_packet2->falcon.ack_req = false;  // Request immediate ACK
  EXPECT_OK(env_.ScheduleEvent(absl::Microseconds(1), [&]() {
    EXPECT_OK(reliability_manager_->ReceivePacket(rx_packet2.get()));
  }));

  // OOO push data at PSN 3 arrive at 2us.
  auto rx_packet3 = std::make_unique<Packet>();
  rx_packet3->packet_type = falcon::PacketType::kPushUnsolicitedData;
  rx_packet3->falcon.dest_cid = 1;
  rx_packet3->falcon.psn = 3;  // PSN within the window
  rx_packet3->falcon.rsn = 3;
  rx_packet3->falcon.ack_req = false;  // Request immediate ACK
  EXPECT_OK(env_.ScheduleEvent(absl::Microseconds(2), [&]() {
    EXPECT_OK(reliability_manager_->ReceivePacket(rx_packet3.get()));
  }));

  // Pull data at PSN 1 fills the holes, at 3us.
  auto rx_packet4 = std::make_unique<Packet>();
  rx_packet4->packet_type = falcon::PacketType::kPullData;
  rx_packet4->falcon.dest_cid = 1;
  rx_packet4->falcon.psn = 1;  // PSN within the window
  rx_packet4->falcon.rsn = 1;
  rx_packet4->falcon.ack_req = false;  // Request immediate ACK
  auto cookie4 = falcon_->CreateCookieForTesting(*rx_packet4);
  EXPECT_OK(env_.ScheduleEvent(absl::Microseconds(3), [&]() {
    EXPECT_OK(reliability_manager_->ReceivePacket(rx_packet4.get()));
  }));

  // ULP Acks data of RSN 2 (PSN 2), at 20us.
  EXPECT_OK(env_.ScheduleEvent(absl::Microseconds(20), [&]() {
    EXPECT_OK(reliability_manager_->HandleAckFromUlp(1, 2, *cookie4));
  }));

  // OOO push data at PSN 5 arrives at 21us.
  auto rx_packet5 = std::make_unique<Packet>();
  rx_packet5->packet_type = falcon::PacketType::kPushUnsolicitedData;
  rx_packet5->falcon.dest_cid = 1;
  rx_packet5->falcon.psn = 5;  // PSN within the window
  rx_packet5->falcon.rsn = 5;
  rx_packet5->falcon.ack_req = false;  // Request immediate ACK
  auto cookie5 = falcon_->CreateCookieForTesting(*rx_packet5);
  EXPECT_OK(env_.ScheduleEvent(absl::Microseconds(21), [&]() {
    EXPECT_OK(reliability_manager_->ReceivePacket(rx_packet5.get()));
  }));

  // ULP Acks data of RSN 3 (PSN 3), at 30us.
  EXPECT_OK(env_.ScheduleEvent(absl::Microseconds(30), [&]() {
    EXPECT_OK(reliability_manager_->HandleAckFromUlp(1, 3, *cookie5));
  }));

  // Pull data at PSN 4 fills the holes, at 31us.
  auto rx_packet6 = std::make_unique<Packet>();
  rx_packet6->packet_type = falcon::PacketType::kPullData;
  rx_packet6->falcon.dest_cid = 1;
  rx_packet6->falcon.psn = 4;  // PSN within the window
  rx_packet6->falcon.rsn = 4;
  rx_packet6->falcon.ack_req = false;  // Request immediate ACK
  EXPECT_OK(env_.ScheduleEvent(absl::Microseconds(31), [&]() {
    EXPECT_OK(reliability_manager_->ReceivePacket(rx_packet6.get()));
  }));

  // In-order pull data at PSN 6 arrive at 32us.
  auto rx_packet7 = std::make_unique<Packet>();
  rx_packet7->packet_type = falcon::PacketType::kPullData;
  rx_packet7->falcon.dest_cid = 1;
  rx_packet7->falcon.psn = 6;  // PSN within the window
  rx_packet7->falcon.rsn = 6;
  rx_packet7->falcon.ack_req = false;  // Request immediate ACK
  EXPECT_OK(env_.ScheduleEvent(absl::Microseconds(32), [&]() {
    EXPECT_OK(reliability_manager_->ReceivePacket(rx_packet7.get()));
  }));

  EXPECT_CALL(shaper_, TransferTxPacket(_))
      .WillOnce([&](std::unique_ptr<Packet> p) {
        EXPECT_EQ(p->packet_type, falcon::PacketType::kAck);
        // BPSN == 1.
        EXPECT_EQ(p->ack.rdbpsn, 1);
        // PSN 2 and 3 should be received, PSN 1 is not.
        EXPECT_FALSE(p->ack.received_bitmap.Get(0));
        EXPECT_TRUE(p->ack.received_bitmap.Get(1));
        EXPECT_TRUE(p->ack.received_bitmap.Get(2));
        // ACK bitmap should be 0.
        EXPECT_TRUE(p->ack.receiver_data_bitmap.Empty());
        // An E-ACK.
        EXPECT_EQ(p->ack.ack_type, Packet::Ack::kEack);
        // After the 3rd packet.
        EXPECT_GE(env_.ElapsedTime(), absl::Microseconds(2));
        // Before the timer expires.
        EXPECT_LT(env_.ElapsedTime(), ack_coalescing_timeout);
        // Check bifurcation ids of the outgoing packet.
        EXPECT_EQ(p->metadata.source_bifurcation_id,
                  FalconTestingHelpers::kSourceBifurcationId);
        EXPECT_EQ(p->metadata.destination_bifurcation_id,
                  FalconTestingHelpers::kDestinationBifurcationId);
      })
      .WillOnce([&](std::unique_ptr<Packet> p) {
        EXPECT_EQ(p->packet_type, falcon::PacketType::kAck);
        // BPSN == 2.
        EXPECT_EQ(p->ack.rdbpsn, 2);
        // An ACK, because data ack bitmap is 0 (PSN2~4 are waiting for ULP
        // ACK).
        EXPECT_EQ(p->ack.ack_type, Packet::Ack::kAck);
        // When timer expires.
        EXPECT_EQ(env_.ElapsedTime(),
                  absl::Microseconds(3) + ack_coalescing_timeout);
        // Check bifurcation ids of the outgoing packet.
        EXPECT_EQ(p->metadata.source_bifurcation_id,
                  FalconTestingHelpers::kSourceBifurcationId);
        EXPECT_EQ(p->metadata.destination_bifurcation_id,
                  FalconTestingHelpers::kDestinationBifurcationId);
      })
      .WillOnce([&](std::unique_ptr<Packet> p) {
        EXPECT_EQ(p->packet_type, falcon::PacketType::kAck);
        // BPSN == 3.
        EXPECT_EQ(p->ack.rdbpsn, 3);
        // PSN 3,5 are received, PSN 4 is not received
        EXPECT_TRUE(p->ack.received_bitmap.Get(0));
        EXPECT_FALSE(p->ack.received_bitmap.Get(1));
        EXPECT_TRUE(p->ack.received_bitmap.Get(2));
        // ACK bitmap should be 0.
        EXPECT_TRUE(p->ack.receiver_data_bitmap.Empty());
        // An E-ACK.
        EXPECT_EQ(p->ack.ack_type, Packet::Ack::kEack);
        // When timer expires.
        EXPECT_EQ(env_.ElapsedTime(),
                  absl::Microseconds(20) + ack_coalescing_timeout);
        // Check bifurcation ids of the outgoing packet.
        EXPECT_EQ(p->metadata.source_bifurcation_id,
                  FalconTestingHelpers::kSourceBifurcationId);
        EXPECT_EQ(p->metadata.destination_bifurcation_id,
                  FalconTestingHelpers::kDestinationBifurcationId);
      })
      .WillOnce([&](std::unique_ptr<Packet> p) {
        EXPECT_EQ(p->packet_type, falcon::PacketType::kAck);
        // BPSN == 5.
        EXPECT_EQ(p->ack.rdbpsn, 5);
        // PSN 5 should not be ACKed, waiting for ULP ACK.
        EXPECT_FALSE(p->ack.receiver_data_bitmap.Get(0));
        // PSN 6 should be ACKed, since it's pull data.
        EXPECT_TRUE(p->ack.receiver_data_bitmap.Get(1));
        // An E-ACK.
        EXPECT_EQ(p->ack.ack_type, Packet::Ack::kEack);
        // After the 7-th packet.
        EXPECT_GE(env_.ElapsedTime(), absl::Microseconds(32));
        // Before the timer expires.
        EXPECT_LT(env_.ElapsedTime(),
                  absl::Microseconds(30) + ack_coalescing_timeout);
        // Check bifurcation ids of the outgoing packet.
        EXPECT_EQ(p->metadata.source_bifurcation_id,
                  FalconTestingHelpers::kSourceBifurcationId);
        EXPECT_EQ(p->metadata.destination_bifurcation_id,
                  FalconTestingHelpers::kDestinationBifurcationId);
      });
  env_.Run();
}

// ack_coalescing_counter should not underflow.
TEST_P(AckCoalescingEngineTest, AckCoalescingCounterNotUnderflow) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
  // Set the parametrized test values in FalconConfig and initialize Falcon test
  // setup.
  InitFalcon(config);
  // Initialize the connection state.
  auto connection_metadata =
      FalconTestingHelpers::InitializeConnectionMetadata(falcon_.get());
  uint32_t scid = connection_metadata->scid;
  // Set ack coalescing threshold to be = 2
  connection_metadata->ack_coalescing_threshold = 2;
  ConnectionState* connection_state =
      FalconTestingHelpers::InitializeConnectionState(
          falcon_.get(), std::move(connection_metadata));

  connection_state->rx_reliability_metadata.request_window_metadata
      .base_packet_sequence_number = 0;

  // In order pull request at PSN 0 arrive at 0us.
  auto rx_packet = std::make_unique<Packet>();
  rx_packet->packet_type = falcon::PacketType::kPullRequest;
  rx_packet->falcon.dest_cid = 1;
  rx_packet->falcon.psn = 0;
  rx_packet->falcon.rsn = 0;
  rx_packet->falcon.ack_req = false;
  EXPECT_OK(reliability_manager_->ReceivePacket(rx_packet.get()));

  // OOO pull request at PSN 2 arrive at 1us.
  auto rx_packet2 = std::make_unique<Packet>();
  rx_packet2->packet_type = falcon::PacketType::kPullRequest;
  rx_packet2->falcon.dest_cid = 1;
  rx_packet2->falcon.psn = 2;
  rx_packet2->falcon.rsn = 2;
  rx_packet2->falcon.ack_req = false;
  EXPECT_OK(env_.ScheduleEvent(absl::Microseconds(1), [&]() {
    EXPECT_OK(reliability_manager_->ReceivePacket(rx_packet2.get()));
  }));

  auto rx_packet3 = std::make_unique<Packet>();
  rx_packet3->packet_type = falcon::PacketType::kPullRequest;
  rx_packet3->falcon.dest_cid = 1;
  rx_packet3->falcon.psn = 1;
  rx_packet3->falcon.rsn = 1;
  rx_packet3->falcon.ack_req = false;
  EXPECT_OK(env_.ScheduleEvent(absl::Microseconds(2), [&]() {
    EXPECT_OK(reliability_manager_->ReceivePacket(rx_packet3.get()));
  }));

  auto tx_packet = std::make_unique<Packet>();
  std::unique_ptr<AckCoalescingKey> ack_coalescing_key =
      CreateAckCoalescingKey(scid);
  EXPECT_OK(env_.ScheduleEvent(absl::Microseconds(3), [&]() {
    EXPECT_OK(ack_coalescing_engine_->PiggybackAck(*ack_coalescing_key,
                                                   tx_packet.get()));
  }));

  // After the 3rd packet rx.
  env_.RunFor(absl::Microseconds(2));
  absl::StatusOr<const AckCoalescingEntry*> ack_coalescing_entry =
      GetAckCoalescingEntry(ack_coalescing_key.get());
  EXPECT_OK(ack_coalescing_entry);
  // The ACK coalescing counter should be 2 (1 packet * 2).
  EXPECT_EQ(ack_coalescing_entry.value()->coalescing_counter, 1 * 2);
  // Implicitly_acked_counter should be 2.
  EXPECT_EQ(connection_state->rx_reliability_metadata.implicitly_acked_counter,
            2);

  // After the piggyback ACK.
  env_.RunFor(absl::Microseconds(1));
  // ACK coalescing counter should be 0.
  EXPECT_EQ(ack_coalescing_entry.value()->coalescing_counter, 0);
  // Implicitly_acked_counter should be 0.
  EXPECT_EQ(connection_state->rx_reliability_metadata.implicitly_acked_counter,
            0);
}

// Instantiate test suite for Gen1 and Gen2.
INSTANTIATE_TEST_SUITE_P(
    ComponentTests, AckCoalescingEngineTest,
    /*version=*/testing::Values(1, 2),
    [](const testing::TestParamInfo<AckCoalescingEngineTest::ParamType>& info) {
      const int version = info.param;
      return absl::StrCat("Gen", version);
    });

}  // namespace

}  // namespace isekai
