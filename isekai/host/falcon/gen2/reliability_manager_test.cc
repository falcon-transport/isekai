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

#include "isekai/host/falcon/gen2/reliability_manager.h"

#include <sys/types.h>

#include <cstdint>
#include <memory>
#include <numeric>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/time/time.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/constants.h"
#include "isekai/common/default_config_generator.h"
#include "isekai/common/packet.h"
#include "isekai/common/simple_environment.h"
#include "isekai/common/testing.h"
#include "isekai/host/falcon/connection_state_utils.h"
#include "isekai/host/falcon/falcon.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/falcon_connection_state.h"
#include "isekai/host/falcon/falcon_testing_helpers.h"
#include "isekai/host/falcon/gen1/falcon_types.h"
#include "isekai/host/falcon/gen1/rate_update_engine.h"
#include "isekai/host/falcon/gen2/falcon_types.h"
#include "isekai/host/falcon/gen2/falcon_utils.h"
#include "isekai/host/falcon/rue/bits.h"
#include "isekai/host/falcon/rue/fixed.h"
#include "isekai/host/falcon/rue/format_gen2.h"

namespace isekai {

namespace {

using ::testing::_;

constexpr uint8_t kNumFlowsPerSinglePathConnection = 1;
constexpr uint8_t kNumFlowsPerMultiPathConnection = 4;

// The constants below are assuming the default multipathing degree of 4 flows
// per connection.
constexpr uint32_t kFlowLabelForFlowId0 = 0;
constexpr uint32_t kFlowLabelForFlowId1 = 1;
constexpr uint32_t kFlowLabelForFlowId2 = 2;
constexpr uint32_t kFlowLabelForFlowId3 = 3;
constexpr uint32_t kFlowLabelMaskForFlowId = 0b11;

class Gen2ReliabilityManagerTest
    : public FalconTestingHelpers::FalconTestSetup,
      public ::testing::TestWithParam</*falcon_version*/ int> {
 protected:
  int GetFalconVersion() { return GetParam(); }

  void InstallFakeRueAdapter() {
    // Use the fake RUE adapter to maintain the flow weights in the connection
    // state.
    auto rue_ = dynamic_cast<ProtocolRateUpdateEngine*>(
        falcon_->get_rate_update_engine());
    if (GetFalconVersion() == 2) {
      ProtocolRateUpdateEngineTestPeer<falcon_rue::Event_Gen2,
                                       falcon_rue::Response_Gen2>
          peer_;
      peer_.Set(rue_);
    } else {
      LOG(FATAL) << "Unsupported Falcon version: " << GetFalconVersion();
    }
  }
};

INSTANTIATE_TEST_SUITE_P(Gen2Tests, Gen2ReliabilityManagerTest,
                         /*version=*/testing::Values(2),
                         [](const testing::TestParamInfo<
                             Gen2ReliabilityManagerTest::ParamType>& info) {
                           const int version = info.param;
                           return std::to_string(version);
                         });

TEST_P(Gen2ReliabilityManagerTest, TransactionSlidingWindowChecks) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
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
  // Window range [0, 255].

  // Initialize packet metadata.
  auto packet_metadata = std::make_unique<PacketMetadata>();
  packet_metadata->type = falcon::PacketType::kPullRequest;
  packet_metadata->active_packet = std::make_unique<Packet>();
  // Initialize transaction metadata.
  auto transaction =
      CreateTransactionMetadata(GetFalconVersion(), 0, TransactionType::kPull,
                                TransactionLocation::kTarget);
  transaction->packets[falcon::PacketType::kPullRequest] =
      std::move(packet_metadata);
  // Add the above metadata to the appropriate connection state.
  connection_state
      ->transactions[{transaction->rsn, TransactionLocation::kTarget}] =
      std::move(transaction);

  // Valid PSN (within window)
  rx_packet->falcon.psn = 255;
  EXPECT_OK(reliability_manager_->ReceivePacket(rx_packet.get()));
  // Check if the receive packet context with RSN=0 is created.
  EXPECT_OK(connection_state->rx_reliability_metadata.GetReceivedPacketContext(
      {0, TransactionLocation::kTarget}));
  // Invalid PSN (beyond window)
  rx_packet->falcon.psn = 256;
  EXPECT_EQ(reliability_manager_->ReceivePacket(rx_packet.get()).code(),
            absl::StatusCode::kOutOfRange);
}

TEST_P(Gen2ReliabilityManagerTest, WrapAroundTransactionSlidingWindowChecks) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
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
      4294967294;  // Makes window range [2^32 - 2, 253].

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
  auto transaction =
      CreateTransactionMetadata(GetFalconVersion(), 0, TransactionType::kPull,
                                TransactionLocation::kTarget);
  transaction->packets[falcon::PacketType::kPullRequest] =
      std::move(packet_metadata);
  // Add the above metadata to the appropriate connection state.
  connection_state
      ->transactions[{transaction->rsn, TransactionLocation::kTarget}] =
      std::move(transaction);

  // Valid PSN (within window)
  rx_packet->falcon.psn = 253;
  EXPECT_OK(reliability_manager_->ReceivePacket(rx_packet.get()));

  // Invalid PSN (beyond window)
  rx_packet->falcon.psn = 254;
  EXPECT_EQ(reliability_manager_->ReceivePacket(rx_packet.get()).code(),
            absl::StatusCode::kOutOfRange);

  // Invalid PSN (before window)
  rx_packet->falcon.psn = 4294967292;
  EXPECT_EQ(reliability_manager_->ReceivePacket(rx_packet.get()).code(),
            absl::StatusCode::kAlreadyExists);
}

// Tests that packets are scheduled in a round-robin manner across the flows
// with multipathing.
TEST_P(Gen2ReliabilityManagerTest, RoundRobinFlowScheduling) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
  config.mutable_gen2_config_options()
      ->mutable_multipath_config()
      ->set_path_selection_policy(FalconConfig::Gen2ConfigOptions::
                                      MultipathConfig::OPEN_LOOP_ROUND_ROBIN);
  InitFalcon(config);
  // Initialize the connection state.
  auto connection_metadata =
      FalconTestingHelpers::InitializeConnectionMetadata(falcon_.get());
  uint32_t scid = connection_metadata->scid;
  FalconTestingHelpers::SetMultipathingDegree(connection_metadata.get(),
                                              kNumFlowsPerMultiPathConnection);
  ConnectionState* connection_state =
      FalconTestingHelpers::InitializeConnectionState(
          falcon_.get(), std::move(connection_metadata));
  Gen2CongestionControlMetadata& ccmeta =
      CongestionControlMetadata::DowncastTo<Gen2CongestionControlMetadata>(
          *connection_state->congestion_control_metadata);
  // Total number of packets to transmit in this test.
  int n_tx_packet = 12;
  auto& flow_labels = ccmeta.gen2_flow_labels;

  for (int i = 0; i < n_tx_packet; i++) {
    // Set up the transaction.
    FalconTestingHelpers::SetupTransactionWithConnectionState(
        /*connection_state=*/connection_state,
        /*transaction_type=*/TransactionType::kPushUnsolicited,
        /*transaction_location=*/TransactionLocation::kInitiator,
        /*transaction_state=*/TransactionState::kPushUnsolicitedReqUlpRx,
        /*packet_metadata_type=*/falcon::PacketType::kPushUnsolicitedData,
        /*rsn=*/i,
        /*psn=*/i);

    // Make sure flow label in transmitted packet is set in a round-robin
    // fashion with respect to the initialized flow label values in
    // congestion_control_metadata.
    uint32_t expected_flow_label =
        flow_labels[i % kNumFlowsPerMultiPathConnection];
    EXPECT_CALL(shaper_, TransferTxPacket(_))
        .WillOnce([expected_flow_label](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->packet_type, falcon::PacketType::kPushUnsolicitedData);
          EXPECT_EQ(p->metadata.flow_label, expected_flow_label);
        });
    // Transmit the packet
    EXPECT_OK(reliability_manager_->TransmitPacket(
        scid, i, falcon::PacketType::kPushUnsolicitedData));
  }
  // Do not care about possible retx in this test, so run for 10us only.
  env_.RunFor(absl::Microseconds(10));
}

// Tests that packets are scheduled in a correct weighted round-robin manner
// across the flows with multipathing.
TEST_P(Gen2ReliabilityManagerTest, WeightedRoundRobinFlowScheduling) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
  config.mutable_gen2_config_options()
      ->mutable_multipath_config()
      ->set_path_selection_policy(
          FalconConfig::Gen2ConfigOptions::MultipathConfig::
              OPEN_LOOP_FCWND_WEIGHTED_ROUND_ROBIN);
  InitFalcon(config);
  // Initialize the connection state.
  auto connection_metadata =
      FalconTestingHelpers::InitializeConnectionMetadata(falcon_.get());
  uint32_t scid = connection_metadata->scid;
  FalconTestingHelpers::SetMultipathingDegree(connection_metadata.get(),
                                              kNumFlowsPerMultiPathConnection);
  ConnectionState* connection_state =
      FalconTestingHelpers::InitializeConnectionState(
          falcon_.get(), std::move(connection_metadata));
  Gen2CongestionControlMetadata& ccmeta =
      CongestionControlMetadata::DowncastTo<Gen2CongestionControlMetadata>(
          *connection_state->congestion_control_metadata);
  // Shortcut references to the flow_weight and flow_label vectors.
  auto& weights = ccmeta.gen2_flow_weights;
  auto& flow_labels = ccmeta.gen2_flow_labels;

  weights = {1, 2, 3, 4};
  std::vector<int> packets_sent_per_flow = {0, 0, 0, 0};
  auto total_weights = std::reduce(weights.begin(), weights.end());
  int flow_id_to_tx = 0;
  // Total number of packets to transmit in this test.
  int n_tx_packet = 12;

  for (int i = 0; i < n_tx_packet; i++) {
    // Setup the transaction.
    FalconTestingHelpers::SetupTransactionWithConnectionState(
        /*connection_state=*/connection_state,
        /*transaction_type=*/TransactionType::kPushUnsolicited,
        /*transaction_location=*/TransactionLocation::kInitiator,
        /*transaction_state=*/TransactionState::kPushUnsolicitedReqUlpRx,
        /*packet_metadata_type=*/falcon::PacketType::kPushUnsolicitedData,
        /*rsn=*/i,
        /*psn=*/i);

    // Make sure flow label in transmitted packet is set in a weighted
    // round-robin fashion with respect to the initialized flow label values in
    // congestion_control_metadata.
    if (std::reduce(packets_sent_per_flow.begin(),
                    packets_sent_per_flow.end()) == total_weights) {
      // Reset the round-robin round when we use all the flows' credits for this
      // round.
      packets_sent_per_flow = {0, 0, 0, 0};
      flow_id_to_tx = 0;
    }
    while (packets_sent_per_flow[flow_id_to_tx] == weights[flow_id_to_tx]) {
      // Choose a flow with an available credit.
      flow_id_to_tx = (flow_id_to_tx + 1) % kNumFlowsPerMultiPathConnection;
    }
    // flow_id_to_tx is the now the actual flow we should schedule the Tx packet
    // on.
    packets_sent_per_flow[flow_id_to_tx] += 1;
    uint32_t expected_flow_label = flow_labels[flow_id_to_tx];
    // Increment flow_id_to_tx to the next flow for the next loop iteration.
    flow_id_to_tx = (flow_id_to_tx + 1) % kNumFlowsPerMultiPathConnection;

    EXPECT_CALL(shaper_, TransferTxPacket(_))
        .WillOnce([expected_flow_label](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->packet_type, falcon::PacketType::kPushUnsolicitedData);
          EXPECT_EQ(p->metadata.flow_label, expected_flow_label);
        });
    // Transmit the packet
    EXPECT_OK(reliability_manager_->TransmitPacket(
        scid, i, falcon::PacketType::kPushUnsolicitedData));
  }
  // Do not care about any retx in this test, so run for 10us only enough for
  // initial transmissions.
  env_.RunFor(absl::Microseconds(10));
}

// Tests that packets are scheduled in a correct weighted round-robin manner
// with batching across the flows with multipathing.
TEST_P(Gen2ReliabilityManagerTest,
       WeightedRoundRobinFlowSchedulingWithBatching) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
  auto multipath_config =
      config.mutable_gen2_config_options()->mutable_multipath_config();
  multipath_config->set_path_selection_policy(
      FalconConfig::Gen2ConfigOptions::MultipathConfig::
          OPEN_LOOP_FCWND_WEIGHTED_ROUND_ROBIN);
  // Set batching true for this test.
  multipath_config->set_batched_packet_scheduling(true);
  InitFalcon(config);

  // Initialize the connection state.
  auto connection_metadata =
      FalconTestingHelpers::InitializeConnectionMetadata(falcon_.get());
  uint32_t scid = connection_metadata->scid;
  FalconTestingHelpers::SetMultipathingDegree(connection_metadata.get(),
                                              kNumFlowsPerMultiPathConnection);
  ConnectionState* connection_state =
      FalconTestingHelpers::InitializeConnectionState(
          falcon_.get(), std::move(connection_metadata));
  Gen2CongestionControlMetadata& ccmeta =
      CongestionControlMetadata::DowncastTo<Gen2CongestionControlMetadata>(
          *connection_state->congestion_control_metadata);
  // Shortcut references to the flow_weight and flow_label vectors.
  auto& weights = ccmeta.gen2_flow_weights;
  auto& flow_labels = ccmeta.gen2_flow_labels;

  weights = {1, 2, 3, 4};
  auto total_weights = std::reduce(weights.begin(), weights.end());
  std::vector<int> cumulative_weights(weights.size());
  std::partial_sum(weights.begin(), weights.end(), cumulative_weights.begin());
  // Start with flow ID 0.
  int flow_id_to_tx = 0;
  // Total number of packets to transmit in a single round.
  int n_tx_packet = total_weights;

  // Run for one full round.
  for (int i = 0; i < n_tx_packet; i++) {
    // Setup the transaction.
    FalconTestingHelpers::SetupTransactionWithConnectionState(
        /*connection_state=*/connection_state,
        /*transaction_type=*/TransactionType::kPushUnsolicited,
        /*transaction_location=*/TransactionLocation::kInitiator,
        /*transaction_state=*/TransactionState::kPushUnsolicitedReqUlpRx,
        /*packet_metadata_type=*/falcon::PacketType::kPushUnsolicitedData,
        /*rsn=*/i,
        /*psn=*/i);

    if (i >= cumulative_weights[flow_id_to_tx]) {
      // Choose next flow when batch finishes.
      flow_id_to_tx++;
    }
    // flow_id_to_tx is the now the actual flow we should schedule the Tx packet
    // on.
    uint32_t expected_flow_label = flow_labels[flow_id_to_tx];
    EXPECT_CALL(shaper_, TransferTxPacket(_))
        .WillOnce([expected_flow_label](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->packet_type, falcon::PacketType::kPushUnsolicitedData);
          EXPECT_EQ(p->metadata.flow_label, expected_flow_label);
        });
    // Transmit the packet
    EXPECT_OK(reliability_manager_->TransmitPacket(
        scid, i, falcon::PacketType::kPushUnsolicitedData));
  }

  // After the first round finishes, test that the new round starts with flow ID
  // 0 as expected.
  FalconTestingHelpers::SetupTransactionWithConnectionState(
      /*connection_state=*/connection_state,
      /*transaction_type=*/TransactionType::kPushUnsolicited,
      /*transaction_location=*/TransactionLocation::kInitiator,
      /*transaction_state=*/TransactionState::kPushUnsolicitedReqUlpRx,
      /*packet_metadata_type=*/falcon::PacketType::kPushUnsolicitedData,
      /*rsn=*/n_tx_packet,
      /*psn=*/n_tx_packet);
  uint32_t expected_flow_label = flow_labels[0];
  EXPECT_CALL(shaper_, TransferTxPacket(_))
      .WillOnce([expected_flow_label](std::unique_ptr<Packet> p) {
        EXPECT_EQ(p->packet_type, falcon::PacketType::kPushUnsolicitedData);
        EXPECT_EQ(p->metadata.flow_label, expected_flow_label);
      });
  // Transmit the packet
  EXPECT_OK(reliability_manager_->TransmitPacket(
      scid, n_tx_packet, falcon::PacketType::kPushUnsolicitedData));

  // Do not care about any retx in this test, so run for 10us only enough for
  // initial transmissions.
  env_.RunFor(absl::Microseconds(10));
}

// Tests that retransmissions use the latest flow label for the flow ID of the
// original initial transmission.
TEST_P(Gen2ReliabilityManagerTest, RetransmissionFlowLabelSelection) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
  config.mutable_gen2_config_options()
      ->mutable_multipath_config()
      ->set_path_selection_policy(
          FalconConfig::Gen2ConfigOptions::MultipathConfig::
              OPEN_LOOP_FCWND_WEIGHTED_ROUND_ROBIN);
  InitFalcon(config);
  // Initialize the connection state.
  auto connection_metadata =
      FalconTestingHelpers::InitializeConnectionMetadata(falcon_.get());
  uint32_t scid = connection_metadata->scid;
  FalconTestingHelpers::SetMultipathingDegree(connection_metadata.get(),
                                              kNumFlowsPerMultiPathConnection);
  ConnectionState* connection_state =
      FalconTestingHelpers::InitializeConnectionState(
          falcon_.get(), std::move(connection_metadata));
  Gen2CongestionControlMetadata& ccmeta =
      CongestionControlMetadata::DowncastTo<Gen2CongestionControlMetadata>(
          *connection_state->congestion_control_metadata);

  // Shortcut references to the flow_weight and flow_label vectors.
  auto& weights = ccmeta.gen2_flow_weights;
  auto& flow_labels = ccmeta.gen2_flow_labels;
  weights = {4, 3, 2, 1};

  // Total number of packets to transmit in this test.
  const int n_tx_packet = 12;

  for (int i = 0; i < n_tx_packet; i++) {
    // Setup the transaction.
    FalconTestingHelpers::SetupTransactionWithConnectionState(
        /*connection_state=*/connection_state,
        /*transaction_type=*/TransactionType::kPushUnsolicited,
        /*transaction_location=*/TransactionLocation::kInitiator,
        /*transaction_state=*/TransactionState::kPushUnsolicitedReqUlpRx,
        /*packet_metadata_type=*/falcon::PacketType::kPushUnsolicitedData,
        /*rsn=*/i,
        /*psn=*/i);
    // Transmit the packet
    EXPECT_OK(reliability_manager_->TransmitPacket(
        scid, i, falcon::PacketType::kPushUnsolicitedData));
  }
  // Run until all initial transmissions are complete.
  env_.RunFor(absl::Microseconds(10));

  // Get RTO expiration time to make sure RTO is triggered.
  absl::Duration rto_expiration_time =
      connection_state->tx_reliability_metadata.GetRtoExpirationTime();

  // Change the flow labels (while keeping the last 2 flow index bits intact) to
  // make sure packets are using the latest flow label for the flow ID.
  for (int idx = 0; idx < kNumFlowsPerMultiPathConnection; idx++) {
    flow_labels[idx] = flow_labels[idx] | (uint32_t)(0b11111100);
  }

  // All n_tx_packet packets should be RTO retransmitted. Check flow labels of
  // retx packets.
  auto total_weights = std::reduce(weights.begin(), weights.end());
  std::vector<int> packets_sent_per_flow = {0, 0, 0, 0};
  int flow_id_to_tx = 0;
  {
    ::testing::InSequence sequence;
    for (int i = 0; i < n_tx_packet; i++) {
      if (std::reduce(packets_sent_per_flow.begin(),
                      packets_sent_per_flow.end()) == total_weights) {
        // Reset the round-robin round when we use all the flows' credits for
        // this round.
        packets_sent_per_flow = {0, 0, 0, 0};
        flow_id_to_tx = 0;
      }
      while (packets_sent_per_flow[flow_id_to_tx] == weights[flow_id_to_tx]) {
        // Choose a flow with an available credit.
        flow_id_to_tx = (flow_id_to_tx + 1) % kNumFlowsPerMultiPathConnection;
      }
      // flow_id_to_tx is the now the actual flow we should schedule the retx
      // packet on.
      packets_sent_per_flow[flow_id_to_tx] += 1;
      uint32_t expected_flow_label = flow_labels[flow_id_to_tx];
      // Increment flow_id_to_tx to the next flow for the next loop iteration.
      flow_id_to_tx = (flow_id_to_tx + 1) % kNumFlowsPerMultiPathConnection;

      EXPECT_CALL(shaper_, TransferTxPacket(_))
          .WillOnce([expected_flow_label](std::unique_ptr<Packet> p) {
            // Make sure the flow label of a retransmitted packet is the latest
            // flow label for the flow ID corresponding to the initial original
            // packet.
            EXPECT_EQ(p->metadata.flow_label, expected_flow_label);
          });
    }
  }
  // Run until all packets are retransmitted by RTO.
  env_.RunUntil(rto_expiration_time + absl::Microseconds(10));
}

// Tests the BACK processing for a multipath connection and proper per-flow
// num_acked accounting with ACK unrolling delays.
TEST_P(Gen2ReliabilityManagerTest, MultipathBaseAckProcessing) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
  InitFalcon(config);
  // Initialize the connection state.
  auto connection_metadata =
      FalconTestingHelpers::InitializeConnectionMetadata(falcon_.get());
  uint32_t scid = connection_metadata->scid;
  FalconTestingHelpers::SetMultipathingDegree(connection_metadata.get(),
                                              kNumFlowsPerMultiPathConnection);
  ConnectionState* connection_state =
      FalconTestingHelpers::InitializeConnectionState(
          falcon_.get(), std::move(connection_metadata));

  // Setup transaction.
  FalconTestingHelpers::SetupTransactionWithConnectionState(
      /*connection_state=*/connection_state,
      /*transaction_type=*/TransactionType::kPull,
      /*transaction_location=*/TransactionLocation::kInitiator,
      /*state=*/TransactionState::kPullReqNtwkTx,
      /*packet_metadata_type=*/falcon::PacketType::kPullRequest,
      /*rsn=*/1,
      /*psn=*/1);

  // Create a fake ACK packet with the required fields setup.
  auto ack_packet = std::make_unique<Packet>();
  ack_packet->packet_type = falcon::PacketType::kAck;
  ack_packet->ack.ack_type = Packet::Ack::kEack;
  ack_packet->ack.dest_cid = scid;
  ack_packet->ack.rrbpsn = 100;
  ack_packet->ack.rdbpsn = 50;
  ack_packet->metadata.flow_label = kFlowLabelForFlowId0;

  // Initialize TX BPSN as same as in ACK packet to see if bitmap is processed
  // as expected.
  connection_state->tx_reliability_metadata.request_window_metadata
      .base_packet_sequence_number = 98;
  connection_state->tx_reliability_metadata.data_window_metadata
      .base_packet_sequence_number = 49;
  // Creates outstanding packet context for the packets that are being ACKed and
  // add to the outstanding_packets list as well.
  // Request Window.
  AddOutstandingPacket(
      /*connection_state=*/connection_state,
      /*psn=*/98,
      /*rsn=*/1,
      /*packet_type=*/falcon::PacketType::kPullRequest,
      /*scid=*/scid,
      /*flow_label=*/kFlowLabelForFlowId1);

  AddOutstandingPacket(
      /*connection_state=*/connection_state,
      /*psn=*/99,
      /*rsn=*/1,
      /*packet_type=*/falcon::PacketType::kPullRequest,
      /*scid=*/scid,
      /*flow_label=*/kFlowLabelForFlowId2);

  AddOutstandingPacket(
      /*connection_state=*/connection_state,
      /*psn=*/49,
      /*rsn=*/2,
      /*packet_type=*/falcon::PacketType::kPullData,
      /*scid=*/scid,
      /*flow_label=*/kFlowLabelForFlowId3);

  // Also initialize the transaction metadata corresponding to rsn=2.
  FalconTestingHelpers::SetupTransactionWithConnectionState(
      /*connection_state=*/connection_state,
      /*transaction_type=*/TransactionType::kPull,
      /*transaction_location=*/TransactionLocation::kTarget,
      /*state=*/TransactionState::kPullDataNtwkTx,
      /*packet_metadata_type=*/falcon::PacketType::kPullData,
      /*rsn=*/2,
      /*psn=*/2);

  ASSERT_OK(reliability_manager_->ReceivePacket(ack_packet.get()));
  ASSERT_EQ(connection_state->tx_reliability_metadata.request_window_metadata
                .base_packet_sequence_number,
            100);
  CHECK_EQ(connection_state->tx_reliability_metadata.data_window_metadata
               .base_packet_sequence_number,
           50);
  Gen2CongestionControlMetadata& ccmeta =
      CongestionControlMetadata::DowncastTo<Gen2CongestionControlMetadata>(
          *connection_state->congestion_control_metadata);
  EXPECT_THAT(ccmeta.gen2_num_acked, ::testing::ElementsAre(0, 1, 1, 1));

  // Create a fake ACK packet with BPSN lesser than transmitter BPSN.
  ack_packet->ack.rrbpsn = 90;
  CHECK_EQ(reliability_manager_->ReceivePacket(ack_packet.get()).code(),
           absl::StatusCode::kAlreadyExists);
}

// Tests the EACK processing for a multipath connection and proper per-flow
// num_acked accounting with ACK unrolling delays.
TEST_P(Gen2ReliabilityManagerTest, MultipathEackProcessing) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
  InitFalcon(config);
  // Initialize the connection state.
  auto connection_metadata =
      FalconTestingHelpers::InitializeConnectionMetadata(falcon_.get());
  uint32_t scid = connection_metadata->scid;
  FalconTestingHelpers::SetMultipathingDegree(connection_metadata.get(),
                                              kNumFlowsPerMultiPathConnection);
  ConnectionState* connection_state =
      FalconTestingHelpers::InitializeConnectionState(
          falcon_.get(), std::move(connection_metadata));

  // Setup transaction.
  FalconTestingHelpers::SetupTransactionWithConnectionState(
      /*connection_state=*/connection_state,
      /*transaction_type=*/TransactionType::kPull,
      /*transaction_location=*/TransactionLocation::kInitiator,
      /*state=*/TransactionState::kPullReqNtwkTx,
      /*packet_metadata_type=*/falcon::PacketType::kPullRequest,
      /*rsn=*/1,
      /*psn=*/1);

  // Create a fake ACK packet with the required fields setup.
  auto ack_packet = std::make_unique<Packet>();
  ack_packet->packet_type = falcon::PacketType::kAck;
  ack_packet->ack.ack_type = Packet::Ack::kEack;
  ack_packet->ack.dest_cid = scid;
  ack_packet->ack.rrbpsn = 100;
  ack_packet->ack.rdbpsn = 50;
  auto ack_request_bitmap = FalconAckPacketBitmap(kRxRequestWindowSize);
  ack_request_bitmap.Set(5, true);
  ack_packet->ack.receiver_request_bitmap = ack_request_bitmap;
  auto ack_data_bitmap = FalconAckPacketBitmap(kRxDataWindowSize);
  ack_data_bitmap.Set(4, true);
  ack_packet->ack.receiver_data_bitmap = ack_data_bitmap;
  ack_packet->metadata.flow_label = kFlowLabelForFlowId0;

  // Initialize TX BPSN as same as in ACK packet to see if bitmap is processed
  // as expected.
  connection_state->tx_reliability_metadata.request_window_metadata
      .base_packet_sequence_number = 100;
  connection_state->tx_reliability_metadata.data_window_metadata
      .base_packet_sequence_number = 50;
  // Creates outstanding packet context for the packets that are being ACKed and
  // add to the outstanding_packets list as well.
  AddOutstandingPacket(
      /*connection_state=*/connection_state,
      /*psn=*/105,
      /*rsn=*/1,
      /*packet_type=*/falcon::PacketType::kPullRequest,
      /*scid=*/scid,
      /*flow_label=*/kFlowLabelForFlowId2);
  AddOutstandingPacket(
      /*connection_state=*/connection_state,
      /*psn=*/54,
      /*rsn=*/2,
      /*packet_type=*/falcon::PacketType::kPullData,
      /*scid=*/scid,
      /*flow_label=*/kFlowLabelForFlowId3);

  // Also initialize the transaction metadata corresponding to rsn=2.
  // Setup transaction.
  FalconTestingHelpers::SetupTransactionWithConnectionState(
      /*connection_state=*/connection_state,
      /*transaction_type=*/TransactionType::kPull,
      /*transaction_location=*/TransactionLocation::kTarget,
      /*state=*/TransactionState::kPullDataNtwkTx,
      /*packet_metadata_type=*/falcon::PacketType::kPullData,
      /*rsn=*/2,
      /*psn=*/2);

  ASSERT_OK(reliability_manager_->ReceivePacket(ack_packet.get()));
  ASSERT_EQ(connection_state->tx_reliability_metadata.request_window_metadata
                .base_packet_sequence_number,
            100);
  CHECK_EQ(connection_state->tx_reliability_metadata.request_window_metadata
               .window->Get(5),
           true);
  CHECK_EQ(connection_state->tx_reliability_metadata.data_window_metadata
               .base_packet_sequence_number,
           50);
  CHECK_EQ(connection_state->tx_reliability_metadata.data_window_metadata
               .window->Get(4),
           true);
  Gen2CongestionControlMetadata& ccmeta =
      CongestionControlMetadata::DowncastTo<Gen2CongestionControlMetadata>(
          *connection_state->congestion_control_metadata);
  EXPECT_THAT(ccmeta.gen2_num_acked, ::testing::ElementsAre(0, 0, 1, 1));
}

// Tests the ACK processing for a single path connection with ACK unrolling
// delays.
TEST_P(Gen2ReliabilityManagerTest, SinglePathAckProcessing) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
  InitFalcon(config);
  // Initialize the connection state.
  auto connection_metadata =
      FalconTestingHelpers::InitializeConnectionMetadata(falcon_.get());
  uint32_t scid = connection_metadata->scid;
  FalconTestingHelpers::SetMultipathingDegree(connection_metadata.get(),
                                              kNumFlowsPerSinglePathConnection);
  ConnectionState* connection_state =
      FalconTestingHelpers::InitializeConnectionState(
          falcon_.get(), std::move(connection_metadata));

  // Initialize packet metadata.
  auto packet_metadata = std::make_unique<PacketMetadata>();
  packet_metadata->type = falcon::PacketType::kPullRequest;

  // Initialize transaction metadata.
  auto transaction =
      CreateTransactionMetadata(GetFalconVersion(), 1, TransactionType::kPull,
                                TransactionLocation::kInitiator);
  transaction->state = TransactionState::kPullReqNtwkTx;
  transaction->packets[falcon::PacketType::kPullRequest] =
      std::move(packet_metadata);

  // Add the above metadata to the appropriate connection state.
  connection_state
      ->transactions[{transaction->rsn, TransactionLocation::kInitiator}] =
      std::move(transaction);

  // Create a fake ACK packet with the required fields setup.
  auto ack_packet = std::make_unique<Packet>();
  ack_packet->packet_type = falcon::PacketType::kAck;
  ack_packet->ack.ack_type = Packet::Ack::kEack;
  ack_packet->ack.dest_cid = scid;
  ack_packet->ack.rrbpsn = 100;
  auto ack_request_bitmap = FalconAckPacketBitmap(kRxRequestWindowSize);
  ack_request_bitmap.Set(5, true);
  ack_packet->ack.receiver_request_bitmap = ack_request_bitmap;
  ack_packet->metadata.flow_label = kFlowLabelForFlowId0;

  // Initialize TX BPSN as same as in ACK packet to see if bitmap is processed
  // as expected.
  connection_state->tx_reliability_metadata.request_window_metadata
      .base_packet_sequence_number = 100;
  // Creates outstanding packet context for the packets that are being ACKed and
  // add to the outstanding_packets list as well.
  AddOutstandingPacket(
      /*connection_state=*/connection_state,
      /*psn=*/105,
      /*rsn=*/1,
      /*packet_type=*/falcon::PacketType::kPullRequest,
      /*scid=*/scid,
      /*flow_label=*/kFlowLabelForFlowId2);

  ASSERT_OK(reliability_manager_->ReceivePacket(ack_packet.get()));
  ASSERT_EQ(connection_state->tx_reliability_metadata.request_window_metadata
                .base_packet_sequence_number,
            100);
  CHECK_EQ(connection_state->tx_reliability_metadata.request_window_metadata
               .window->Get(5),
           true);
  Gen2CongestionControlMetadata& ccmeta =
      CongestionControlMetadata::DowncastTo<Gen2CongestionControlMetadata>(
          *connection_state->congestion_control_metadata);
  EXPECT_THAT(ccmeta.gen2_num_acked, ::testing::ElementsAre(0));
}

// Tests how a stale ACK is handled for multipath connections.
TEST_P(Gen2ReliabilityManagerTest, MultipathStaleAckProcessing) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
  // Configure multipath connections to accept stale ACKs.
  config.mutable_gen2_config_options()
      ->mutable_multipath_config()
      ->set_multipath_connection_accept_stale_acks(true);
  InitFalcon(config);

  // Initialize the connection state.
  auto connection_metadata =
      FalconTestingHelpers::InitializeConnectionMetadata(falcon_.get());
  uint32_t scid = connection_metadata->scid;
  FalconTestingHelpers::SetMultipathingDegree(connection_metadata.get(),
                                              kNumFlowsPerMultiPathConnection);
  ConnectionState* connection_state =
      FalconTestingHelpers::InitializeConnectionState(
          falcon_.get(), std::move(connection_metadata));

  // Create a fake ACK packet with the required fields setup.
  auto ack_packet = std::make_unique<Packet>();
  ack_packet->packet_type = falcon::PacketType::kAck;
  ack_packet->ack.ack_type = Packet::Ack::kEack;
  ack_packet->ack.dest_cid = scid;
  ack_packet->ack.rrbpsn = 50;
  ack_packet->metadata.flow_label = kFlowLabelForFlowId0;

  // Initialize TX BPSN to be larger than ACK BPSN. Since
  // multipath_connection_accept_stale_acks is configured to be true,
  // ReceivePacket() will return with an OK status and not a kAlreadyExists
  // status when num_acked for flow 0 is > 0, else a
  // absl::FailedPreconditionError will be returned.
  Gen2CongestionControlMetadata& ccmeta =
      CongestionControlMetadata::DowncastTo<Gen2CongestionControlMetadata>(
          *connection_state->congestion_control_metadata);
  ccmeta.gen2_num_acked[0] = 1;
  connection_state->tx_reliability_metadata.request_window_metadata
      .base_packet_sequence_number = ack_packet->ack.rrbpsn + 10;
  EXPECT_OK(reliability_manager_->ReceivePacket(ack_packet.get()));
  // Expect that the num_acked for flow ID 0 is reset to 0.
  EXPECT_EQ(ccmeta.gen2_num_acked[0], 0);
  EXPECT_TRUE(absl::IsFailedPrecondition(
      reliability_manager_->ReceivePacket(ack_packet.get())));
}

// Tests how a stale ACK is handled for single path connections.
TEST_P(Gen2ReliabilityManagerTest, SinglePathStaleAckProcessing) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
  // Configure single path connections to accept stale ACKs.
  config.mutable_gen2_config_options()
      ->mutable_multipath_config()
      ->set_single_path_connection_accept_stale_acks(true);
  InitFalcon(config);

  // Initialize the connection state.
  auto connection_metadata =
      FalconTestingHelpers::InitializeConnectionMetadata(falcon_.get());
  uint32_t scid = connection_metadata->scid;
  FalconTestingHelpers::SetMultipathingDegree(connection_metadata.get(),
                                              kNumFlowsPerSinglePathConnection);
  ConnectionState* connection_state =
      FalconTestingHelpers::InitializeConnectionState(
          falcon_.get(), std::move(connection_metadata));

  // Create a fake ACK packet with the required fields setup.
  auto ack_packet = std::make_unique<Packet>();
  ack_packet->packet_type = falcon::PacketType::kAck;
  ack_packet->ack.ack_type = Packet::Ack::kEack;
  ack_packet->ack.dest_cid = scid;
  ack_packet->ack.rrbpsn = 50;
  ack_packet->metadata.flow_label = kFlowLabelForFlowId0;

  // Initialize TX BPSN to be larger than ACK BPSN. Since
  // single_path_connection_accept_stale_acks is configured to be true,
  // ReceivePacket() will return with an OK status and not a kAlreadyExists
  // status when num_acked for flow 0 is > 0, else a
  // absl::FailedPreconditionError will be returned.
  Gen2CongestionControlMetadata& ccmeta =
      CongestionControlMetadata::DowncastTo<Gen2CongestionControlMetadata>(
          *connection_state->congestion_control_metadata);
  ccmeta.gen2_num_acked[0] = 1;
  connection_state->tx_reliability_metadata.request_window_metadata
      .base_packet_sequence_number = ack_packet->ack.rrbpsn + 10;
  EXPECT_OK(reliability_manager_->ReceivePacket(ack_packet.get()));
  // Expect that the num_acked for flow ID 0 is reset to 0.
  EXPECT_EQ(ccmeta.gen2_num_acked[0], 0);
  EXPECT_TRUE(absl::IsFailedPrecondition(
      reliability_manager_->ReceivePacket(ack_packet.get())));
}

// Tests the WRR reset API.
TEST_P(Gen2ReliabilityManagerTest, WeightedRoundRobinReset) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
  config.mutable_gen2_config_options()
      ->mutable_multipath_config()
      ->set_path_selection_policy(
          FalconConfig::Gen2ConfigOptions::MultipathConfig::
              OPEN_LOOP_FCWND_WEIGHTED_ROUND_ROBIN);
  InitFalcon(config);
  // Initialize the connection state.
  auto connection_metadata =
      FalconTestingHelpers::InitializeConnectionMetadata(falcon_.get());
  uint32_t scid = connection_metadata->scid;
  FalconTestingHelpers::SetMultipathingDegree(connection_metadata.get(),
                                              kNumFlowsPerMultiPathConnection);
  ConnectionState* connection_state =
      FalconTestingHelpers::InitializeConnectionState(
          falcon_.get(), std::move(connection_metadata));
  // Shortcut references to the flow_weight and flow_label vectors.
  Gen2CongestionControlMetadata& ccmeta =
      CongestionControlMetadata::DowncastTo<Gen2CongestionControlMetadata>(
          *connection_state->congestion_control_metadata);
  auto& weights = ccmeta.gen2_flow_weights;
  weights = {1, 1, 1, 3};
  // Total number of packets to transmit in this test.
  int n_tx_packet = 6;

  // Setup all n_tx_packet transactions.
  for (int i = 0; i < n_tx_packet; i++) {
    // Setup the transaction.
    FalconTestingHelpers::SetupTransactionWithConnectionState(
        /*connection_state=*/connection_state,
        /*transaction_type=*/TransactionType::kPushUnsolicited,
        /*transaction_location=*/TransactionLocation::kInitiator,
        /*transaction_state=*/TransactionState::kPushUnsolicitedReqUlpRx,
        /*packet_metadata_type=*/falcon::PacketType::kPushUnsolicitedData,
        /*rsn=*/i,
        /*psn=*/i);
  }
  // We will stop after n_tx_packet - 2 packets, when it is time for the 4th
  // flow (flow_id = 3) to send its last two packets in the round.
  for (int i = 0; i < n_tx_packet - 2; i++) {
    // Transmit the packet with rsn = i.
    EXPECT_OK(reliability_manager_->TransmitPacket(
        scid, i, falcon::PacketType::kPushUnsolicitedData));
  }
  EXPECT_CALL(shaper_, TransferTxPacket(_))
      .WillOnce([](std::unique_ptr<Packet> p) {
        // Make sure the flow label of this packet has flow_id = 3 before reset.
        EXPECT_EQ(p->metadata.flow_label & kFlowLabelMaskForFlowId,
                  kFlowLabelForFlowId3);
      });
  EXPECT_OK(reliability_manager_->TransmitPacket(
      scid, n_tx_packet - 2, falcon::PacketType::kPushUnsolicitedData));

  // Change the weights of the WRR policy and make the weights take effect
  // immediately by resetting the WRR policy.
  auto gen2_reliability_manager =
      dynamic_cast<Gen2ReliabilityManager*>(reliability_manager_);
  weights = {0, 1, 1, 1};
  gen2_reliability_manager->ResetWrrForConnection(scid);

  EXPECT_CALL(shaper_, TransferTxPacket(_))
      .WillOnce([](std::unique_ptr<Packet> p) {
        // After the reset with the given weights, expect that the flow label of
        // this packet has flow_id = 1 after reset. If the reset was not
        // successful, the 4th flow's (flow_id=3) last packet in this round
        // would have been the one transmitted.
        EXPECT_EQ(p->metadata.flow_label & kFlowLabelMaskForFlowId,
                  kFlowLabelForFlowId1);
      });
  EXPECT_OK(reliability_manager_->TransmitPacket(
      scid, n_tx_packet - 1, falcon::PacketType::kPushUnsolicitedData));
  // Do not care about any retx in this test, so run for 10us only enough
  // for initial transmissions.
  env_.RunFor(absl::Microseconds(10));
}

TEST_P(Gen2ReliabilityManagerTest, PullDataORCDecrementTest) {
  // Get handle on Falcon config and enable decrement ORC on Pull Data.
  FalconConfig config = DefaultConfigGenerator::DefaultFalconConfig(2);
  config.mutable_gen2_config_options()->set_decrement_orc_on_pull_response(
      true);
  InitFalcon(config);

  // Initialize a pull transaction and get a handle on the Pull Request packet.
  Packet* packet;
  ConnectionState* connection_state;
  std::tie(packet, connection_state) = FalconTestingHelpers::SetupTransaction(
      falcon_.get(), TransactionType::kPull, TransactionLocation::kInitiator,
      TransactionState::kPullReqUlpRx, falcon::PacketType::kPullRequest);

  EXPECT_OK(reliability_manager_->TransmitPacket(
      1, 0, falcon::PacketType::kPullRequest));

  // ORC = 1 as we sent out Pull Request.
  EXPECT_EQ(connection_state->tx_reliability_metadata.request_window_metadata
                .outstanding_requests_counter,
            1);

  // Create an ACK packet that ACKs the Pull Request.
  auto ack_packet = std::make_unique<Packet>();
  ack_packet->packet_type = falcon::PacketType::kAck;
  ack_packet->ack.ack_type = Packet::Ack::kEack;
  ack_packet->ack.dest_cid = 1;
  ack_packet->ack.rrbpsn = 0;
  ack_packet->ack.rdbpsn = 0;
  auto ack_request_bitmap = FalconAckPacketBitmap(kRxRequestWindowSize);
  ack_request_bitmap.Set(0, true);
  ack_packet->ack.receiver_request_bitmap = ack_request_bitmap;

  falcon_->TransferRxPacket(std::move(ack_packet));

  // ORC is still = 1 as the decrement does not happen when we get Pull Request
  // ACK.
  EXPECT_EQ(connection_state->tx_reliability_metadata.request_window_metadata
                .outstanding_requests_counter,
            1);

  // Create and transfer an incoming Pull Data packet.
  auto rx_packet = std::make_unique<Packet>();
  rx_packet->packet_type = falcon::PacketType::kPullData;
  rx_packet->falcon.dest_cid = 1;
  rx_packet->falcon.psn = 0;
  rx_packet->falcon.rsn = 0;
  rx_packet->falcon.ack_req = false;
  auto reliability_manager = falcon_->get_packet_reliability_manager();
  EXPECT_OK(reliability_manager->ReceivePacket(rx_packet.get()));

  // ORC = 0 as reliability manager has processed the incoming Pull Data.
  EXPECT_EQ(connection_state->tx_reliability_metadata.request_window_metadata
                .outstanding_requests_counter,
            0);
}

TEST_P(Gen2ReliabilityManagerTest, PullDataORRCDecrementTest) {
  // Get handle on Falcon config and enable decrement ORC on Pull Data.
  FalconConfig config = DefaultConfigGenerator::DefaultFalconConfig(2);
  config.mutable_gen2_config_options()->set_decrement_orc_on_pull_response(
      true);
  InitFalcon(config);

  // Initialize a pull transaction and get a handle on the Pull Request packet.
  Packet* packet;
  ConnectionState* connection_state;
  std::tie(packet, connection_state) = FalconTestingHelpers::SetupTransaction(
      falcon_.get(), TransactionType::kPull, TransactionLocation::kInitiator,
      TransactionState::kPullReqUlpRx, falcon::PacketType::kPullRequest);

  // Explicitly setting the maximum retries to 2 and timer to 10ns.
  connection_state->connection_metadata->max_retransmit_attempts = 2;
  Gen2CongestionControlMetadata& ccmeta =
      CongestionControlMetadata::DowncastTo<Gen2CongestionControlMetadata>(
          *connection_state->congestion_control_metadata);
  ccmeta.retransmit_timeout = absl::Nanoseconds(10);
  // Ensures the packet has its connection ID set up appropriately.
  packet->metadata.scid = 1;

  EXPECT_OK(reliability_manager_->TransmitPacket(
      1, 0, falcon::PacketType::kPullRequest));
  env_.RunFor(absl::Nanoseconds(14));

  // ORRC = 1 as we sent out Pull Request.
  EXPECT_EQ(connection_state->tx_reliability_metadata.request_window_metadata
                .outstanding_retransmission_requests_counter,
            1);

  // Create an ACK packet that ACKs the Pull Request.
  auto ack_packet = std::make_unique<Packet>();
  ack_packet->packet_type = falcon::PacketType::kAck;
  ack_packet->ack.ack_type = Packet::Ack::kEack;
  ack_packet->ack.dest_cid = 1;
  ack_packet->ack.rrbpsn = 0;
  ack_packet->ack.rdbpsn = 0;
  auto ack_request_bitmap = FalconAckPacketBitmap(kRxRequestWindowSize);
  ack_request_bitmap.Set(0, true);
  ack_packet->ack.receiver_request_bitmap = ack_request_bitmap;

  falcon_->TransferRxPacket(std::move(ack_packet));

  // ORRC is still = 1 as the decrement does not happen when we get Pull Request
  // ACK.
  EXPECT_EQ(connection_state->tx_reliability_metadata.request_window_metadata
                .outstanding_retransmission_requests_counter,
            1);

  // Create and transfer an incoming Pull Data packet.
  auto rx_packet = std::make_unique<Packet>();
  rx_packet->packet_type = falcon::PacketType::kPullData;
  rx_packet->falcon.dest_cid = 1;
  rx_packet->falcon.psn = 0;
  rx_packet->falcon.rsn = 0;
  rx_packet->falcon.ack_req = false;
  auto reliability_manager = falcon_->get_packet_reliability_manager();
  EXPECT_OK(reliability_manager->ReceivePacket(rx_packet.get()));

  // ORC = 0 as reliability manager has processed the incoming Pull Data.
  EXPECT_EQ(connection_state->tx_reliability_metadata.request_window_metadata
                .outstanding_retransmission_requests_counter,
            0);
}
}  // namespace

}  // namespace isekai
