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

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/default_config_generator.h"
#include "isekai/common/packet.h"
#include "isekai/common/testing.h"
#include "isekai/host/falcon/falcon.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/falcon_connection_state.h"
#include "isekai/host/falcon/falcon_testing_helpers.h"
#include "isekai/host/falcon/gen1/falcon_model.h"

namespace isekai {

namespace {

class ProtocolPacketReliabilityManagerTest
    : public FalconTestingHelpers::FalconTestSetup,
      public ::testing::Test {};

TEST_F(ProtocolPacketReliabilityManagerTest, TransactionSlidingWindowChecks) {
  FalconConfig config = DefaultConfigGenerator::DefaultFalconConfig(1);
  config.set_inter_host_rx_scheduling_tick_ns(0);
  InitFalcon(config);

  // Initialize the connection state.
  uint32_t source_connection_id = 1;
  auto connection_metadata = FalconTestingHelpers::InitializeConnectionMetadata(
      falcon_.get(), source_connection_id);
  ConnectionState* connection_state =
      FalconTestingHelpers::InitializeConnectionState(
          falcon_.get(), std::move(connection_metadata));

  // Create a fake incoming transaction with the required fields setup.
  auto rx_packet = std::make_unique<Packet>();
  rx_packet->packet_type = falcon::PacketType::kPullRequest;
  rx_packet->falcon.dest_cid = 1;
  rx_packet->falcon.rsn = 0;
  // Window range [0, 63].

  // Initialize packet metadata.
  auto packet_metadata = std::make_unique<PacketMetadata>();
  packet_metadata->type = falcon::PacketType::kPullRequest;
  packet_metadata->active_packet = std::make_unique<Packet>();
  // Initialize transaction metadata.
  auto transaction = std::make_unique<TransactionMetadata>(
      0, TransactionType::kPull, TransactionLocation::kTarget);
  transaction->packets[falcon::PacketType::kPullRequest] =
      std::move(packet_metadata);
  // Add the above metadata to the appropriate connection state.
  connection_state
      ->transactions[{transaction->rsn, TransactionLocation::kTarget}] =
      std::move(transaction);

  // Valid PSN (within window)
  rx_packet->falcon.psn = 63;
  EXPECT_OK(reliability_manager_->ReceivePacket(rx_packet.get()));
  // Check if the receive packet context with RSN=0 is created.
  EXPECT_OK(connection_state->rx_reliability_metadata.GetReceivedPacketContext(
      {0, TransactionLocation::kTarget}));
  // Invalid PSN (beyond window)
  rx_packet->falcon.psn = 64;
  EXPECT_EQ(reliability_manager_->ReceivePacket(rx_packet.get()).code(),
            absl::StatusCode::kOutOfRange);
}

TEST_F(ProtocolPacketReliabilityManagerTest,
       WrapAroundTransactionSlidingWindowChecks) {
  FalconConfig config = DefaultConfigGenerator::DefaultFalconConfig(1);
  config.set_inter_host_rx_scheduling_tick_ns(0);
  InitFalcon(config);

  // Initialize the connection state.
  uint32_t source_connection_id = 1;
  auto connection_metadata = FalconTestingHelpers::InitializeConnectionMetadata(
      falcon_.get(), source_connection_id);
  ConnectionState* connection_state =
      FalconTestingHelpers::InitializeConnectionState(
          falcon_.get(), std::move(connection_metadata));

  // Set the RBPSN > PSN (due to wrap  around)
  connection_state->rx_reliability_metadata.request_window_metadata
      .base_packet_sequence_number =
      4294967294;  // Makes window range [2^32 - 2, 61].

  // Create a fake incoming transaction with the required fields setup.
  auto rx_packet = std::make_unique<Packet>();
  rx_packet->packet_type = falcon::PacketType::kPullRequest;
  rx_packet->falcon.dest_cid = 1;
  rx_packet->falcon.rsn = 0;

  // Initialize packet metadata.
  auto packet_metadata = std::make_unique<PacketMetadata>();
  packet_metadata->type = falcon::PacketType::kPullRequest;
  packet_metadata->active_packet = std::make_unique<Packet>();
  // Initialize transaction metadata.
  auto transaction = std::make_unique<TransactionMetadata>(
      0, TransactionType::kPull, TransactionLocation::kTarget);
  transaction->packets[falcon::PacketType::kPullRequest] =
      std::move(packet_metadata);
  // Add the above metadata to the appropriate connection state.
  connection_state
      ->transactions[{transaction->rsn, TransactionLocation::kTarget}] =
      std::move(transaction);

  // Valid PSN (within window)
  rx_packet->falcon.psn = 61;
  EXPECT_OK(reliability_manager_->ReceivePacket(rx_packet.get()));

  // Invalid PSN (beyond window)
  rx_packet->falcon.psn = 62;
  EXPECT_EQ(reliability_manager_->ReceivePacket(rx_packet.get()).code(),
            absl::StatusCode::kOutOfRange);

  // Invalid PSN (before window)
  rx_packet->falcon.psn = 4294967292;
  EXPECT_EQ(reliability_manager_->ReceivePacket(rx_packet.get()).code(),
            absl::StatusCode::kAlreadyExists);
}

}  // namespace

}  // namespace isekai
