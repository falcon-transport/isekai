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

#include "isekai/host/falcon/gen1/packet_reliability_manager.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>

#include "absl/container/btree_set.h"
#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/time/time.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/constants.h"
#include "isekai/common/default_config_generator.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/common/packet.h"
#include "isekai/common/simple_environment.h"
#include "isekai/common/testing.h"
#include "isekai/host/falcon/connection_state_utils.h"
#include "isekai/host/falcon/falcon.h"
#include "isekai/host/falcon/falcon_bitmap.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/falcon_connection_state.h"
#include "isekai/host/falcon/falcon_testing_helpers.h"
#include "isekai/host/falcon/gen1/connection_scheduler_types.h"
#include "isekai/host/falcon/gen1/falcon_model.h"
#include "isekai/host/falcon/gen1/falcon_types.h"
#include "isekai/host/falcon/rue/fixed.h"

namespace isekai {

namespace {

using ::testing::_;
using ::testing::StrictMock;

class ProtocolPacketReliabilityManagerTest
    : public FalconTestingHelpers::FalconTestSetup,
      public ::testing::TestWithParam</*falcon_version*/ int> {
 protected:
  int GetFalconVersion() { return GetParam(); }
};

INSTANTIATE_TEST_SUITE_P(
    Gen1Gen2CommonTests, ProtocolPacketReliabilityManagerTest,
    /*version=*/testing::Values(1, 2),
    [](const testing::TestParamInfo<
        ProtocolPacketReliabilityManagerTest::ParamType>& info) {
      const int version = info.param;
      return absl::StrCat("Gen", version);
    });

void CheckOutstandingPacketList(
    const absl::btree_set<RetransmissionWorkId> outstanding,
    std::vector<uint32_t> psn_list) {
  ASSERT_EQ(outstanding.size(), psn_list.size());
  int i = 0;
  for (auto it : outstanding) {
    ASSERT_EQ(it.psn, psn_list[i]);
    i++;
  }
}

TEST_P(ProtocolPacketReliabilityManagerTest, AssignRequestPSN) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
  config.set_inter_host_rx_scheduling_tick_ns(0);
  InitFalcon(config);

  // Steps to initialize a transaction and get handle on the packet and
  // connection state.
  Packet* packet;
  ConnectionState* connection_state;
  std::tie(packet, connection_state) = FalconTestingHelpers::SetupTransaction(
      falcon_.get(), TransactionType::kPull, TransactionLocation::kInitiator,
      TransactionState::kPullReqUlpRx, falcon::PacketType::kPullRequest);

  // Expect call to traffic shaper_ to send packet (with PSN=0) over network.
  EXPECT_CALL(shaper_, TransferTxPacket)
      .WillOnce([&](std::unique_ptr<Packet> p) {
        EXPECT_EQ(p->falcon.psn, 0);
        // Check bifurcation ids of the outgoing packet.
        EXPECT_EQ(p->metadata.source_bifurcation_id,
                  FalconTestingHelpers::kSourceBifurcationId);
        EXPECT_EQ(p->metadata.destination_bifurcation_id,
                  FalconTestingHelpers::kDestinationBifurcationId);
      });

  EXPECT_OK(reliability_manager_->TransmitPacket(
      1, 0, falcon::PacketType::kPullRequest));
  // Given that this is a pull request, the request window should be used and
  // its NSN should be 1.
  EXPECT_EQ(connection_state->tx_reliability_metadata.request_window_metadata
                .next_packet_sequence_number,
            1);
  // Check if the receive packet context with RSN=1 is created.
  EXPECT_OK(connection_state->rx_reliability_metadata.GetReceivedPacketContext(
      {0, TransactionLocation::kInitiator}));
}

TEST_P(ProtocolPacketReliabilityManagerTest, AssignPullRequestRolloverPSN) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
  config.set_inter_host_rx_scheduling_tick_ns(0);
  InitFalcon(config);

  // Steps to initialize a transaction and get handle on the packet and
  // connection state.
  Packet* packet;
  ConnectionState* connection_state;
  std::tie(packet, connection_state) = FalconTestingHelpers::SetupTransaction(
      falcon_.get(), TransactionType::kPull, TransactionLocation::kInitiator,
      TransactionState::kPullReqUlpRx, falcon::PacketType::kPullRequest);

  // Set the next PSN to be the max value, to check if the NSN looping logic
  // works.
  connection_state->tx_reliability_metadata.request_window_metadata
      .next_packet_sequence_number = std::numeric_limits<uint32_t>::max();

  // Expect call to traffic shaper_ to send packet (with PSN=max) over network.
  EXPECT_CALL(shaper_, TransferTxPacket)
      .WillOnce([&](std::unique_ptr<Packet> p) {
        EXPECT_EQ(p->falcon.psn, std::numeric_limits<uint32_t>::max());
      });

  EXPECT_OK(reliability_manager_->TransmitPacket(
      1, 0, falcon::PacketType::kPullRequest));
  // Given that this is a pull request, the data window should be used and its
  // NSN should be 0.
  EXPECT_EQ(connection_state->tx_reliability_metadata.request_window_metadata
                .next_packet_sequence_number,
            0);
}

TEST_P(ProtocolPacketReliabilityManagerTest,
       AccurateRetransmissionTimeoutTest) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
  config.set_inter_host_rx_scheduling_tick_ns(0);
  InitFalcon(config);

  // Steps to initialize a transaction and get handle on the packet and
  // connection state.
  Packet* packet;
  ConnectionState* connection_state;
  std::tie(packet, connection_state) = FalconTestingHelpers::SetupTransaction(
      falcon_.get(), TransactionType::kPull, TransactionLocation::kInitiator,
      TransactionState::kPullReqUlpRx, falcon::PacketType::kPullRequest);

  // Explicitly setting the maximum retries to 1 and timer to 10ns.
  connection_state->connection_metadata->max_retransmit_attempts = 1;
  connection_state->congestion_control_metadata->retransmit_timeout =
      absl::Nanoseconds(10);
  // Ensures the packet has its connection ID set up appropriately.
  packet->metadata.scid = 1;

  EXPECT_OK(reliability_manager_->TransmitPacket(
      1, 0, falcon::PacketType::kPullRequest));
  EXPECT_OK(env_.ScheduleEvent(absl::Nanoseconds(10), [&]() { env_.Stop(); }));
  env_.Run();

  // Get a handle on packet to verify retransmit count.
  absl::StatusOr<PacketMetadata*> packet_metadata =
      connection_state->GetTransaction({0, TransactionLocation::kInitiator})
          .value()
          ->GetPacketMetadata(falcon::PacketType::kPullRequest);

  // Indicates retransmission was initiated.
  EXPECT_EQ(packet_metadata.value()->transmit_attempts, 1);
  // Indicates that the timer was not rescheduled as there were no outstanding
  // packets.
  EXPECT_EQ(connection_state->tx_reliability_metadata.rto_expiration_base_time,
            absl::ZeroDuration());
}

TEST_P(ProtocolPacketReliabilityManagerTest, ExplicitAckTest) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
  config.set_inter_host_rx_scheduling_tick_ns(0);
  InitFalcon(config);

  // Steps to initialize a transaction and get handle on the packet and
  // connection state.
  Packet* packet;
  ConnectionState* connection_state;
  std::tie(packet, connection_state) = FalconTestingHelpers::SetupTransaction(
      falcon_.get(), TransactionType::kPull, TransactionLocation::kTarget,
      TransactionState::kPullReqUlpRx, falcon::PacketType::kPullRequest);

  // Create a fake incoming transaction with the required fields setup.
  auto rx_packet = std::make_unique<Packet>();
  rx_packet->packet_type = falcon::PacketType::kPullRequest;
  rx_packet->falcon.dest_cid = 1;
  rx_packet->falcon.psn = 63;        // PSN within the window
  rx_packet->falcon.ack_req = true;  // Request immediate ACK
  rx_packet->falcon.rsn = 1;

  EXPECT_CALL(shaper_, TransferTxPacket(_))
      .WillOnce([&](std::unique_ptr<Packet> p) {
        EXPECT_EQ(p->packet_type, falcon::PacketType::kAck);
      });

  EXPECT_OK(reliability_manager_->ReceivePacket(rx_packet.get()));
  env_.Run();
}

TEST_P(ProtocolPacketReliabilityManagerTest, ExplicitACKProcessingTest) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
  config.set_inter_host_rx_scheduling_tick_ns(0);
  InitFalcon(config);

  constexpr uint32_t kFlowLabel = 1;

  // Initialize the connection state and get a handle on it.
  uint32_t source_connection_id = 1;
  auto connection_metadata = FalconTestingHelpers::InitializeConnectionMetadata(
      falcon_.get(), source_connection_id);
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
  ack_packet->ack.dest_cid = 1;
  ack_packet->ack.rrbpsn = 100;
  ack_packet->ack.rdbpsn = 50;
  auto ack_request_bitmap = FalconAckPacketBitmap(kRxRequestWindowSize);
  ack_request_bitmap.Set(5, true);
  ack_packet->ack.receiver_request_bitmap = ack_request_bitmap;
  auto ack_data_bitmap = FalconAckPacketBitmap(kRxDataWindowSize);
  ack_data_bitmap.Set(4, true);
  ack_packet->ack.receiver_data_bitmap = ack_data_bitmap;
  ack_packet->metadata.flow_label = kFlowLabel;

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
      /*scid=*/source_connection_id,
      /*flow_label=*/kFlowLabel);
  AddOutstandingPacket(
      /*connection_state=*/connection_state,
      /*psn=*/54,
      /*rsn=*/2,
      /*packet_type=*/falcon::PacketType::kPullData,
      /*scid=*/source_connection_id,
      /*flow_label=*/kFlowLabel);

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
  CHECK_EQ(connection_state->tx_reliability_metadata.request_window_metadata
               .window->Get(5),
           true);
  CHECK_EQ(connection_state->tx_reliability_metadata.data_window_metadata
               .base_packet_sequence_number,
           50);
  CHECK_EQ(connection_state->tx_reliability_metadata.data_window_metadata
               .window->Get(4),
           true);

  // Create a fake ACK packet with BPSN lesser than transmitter BPSN.
  ack_packet->ack.rrbpsn = 90;
  CHECK_EQ(reliability_manager_->ReceivePacket(ack_packet.get()).code(),
           absl::StatusCode::kAlreadyExists);
}

TEST_P(ProtocolPacketReliabilityManagerTest,
       InitialTransmissionGatingCriteria) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
  config.set_inter_host_rx_scheduling_tick_ns(0);
  InitFalcon(config);

  // Initialize the connection state and get a handle on it.
  uint32_t source_connection_id = 1;
  auto connection_metadata = FalconTestingHelpers::InitializeConnectionMetadata(
      falcon_.get(), source_connection_id);
  ConnectionState* connection_state =
      FalconTestingHelpers::InitializeConnectionState(
          falcon_.get(), std::move(connection_metadata));

  // Initialize FCWND to be 2, RBPSN = 0, NBPSN = 1 (meets transmission criteria
  // of PSN < DBPSN + fcwnd)
  connection_state->congestion_control_metadata->fabric_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(2.0,
                                                 falcon_rue::kFractionalBits);
  connection_state->tx_reliability_metadata.data_window_metadata
      .next_packet_sequence_number = 1;
  CHECK_EQ(reliability_manager_->MeetsInitialTransmissionCCTxGatingCriteria(
               1, falcon::PacketType::kPushSolicitedData),
           true);

  // Initialize FCWND to be 1, RBPSN = 0, NBPSN = 1 (does not meets
  // transmission criteria of PSN < DBPSN + fcwnd )
  connection_state->congestion_control_metadata->fabric_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(1.0,
                                                 falcon_rue::kFractionalBits);
  connection_state->tx_reliability_metadata.data_window_metadata
      .next_packet_sequence_number = 1;
  CHECK_EQ(reliability_manager_->MeetsInitialTransmissionCCTxGatingCriteria(
               1, falcon::PacketType::kPushSolicitedData),
           false);

  // Initialize FCWND to be 2, RBPSN = 0, NBPSN = 1 (does not meets
  // transmission criteria of PSN < DBPSN + fcwnd && ORC < ncwnd)
  connection_state->congestion_control_metadata->fabric_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(0.0,
                                                 falcon_rue::kFractionalBits);
  // connection_state->congestion_control_metadata.fabric_congestion_window = 0;
  connection_state->tx_reliability_metadata.data_window_metadata
      .next_packet_sequence_number = 1;
  CHECK_EQ(reliability_manager_->MeetsInitialTransmissionCCTxGatingCriteria(
               1, falcon::PacketType::kPushUnsolicitedData),
           false);

  // Initialize FCWND = NCWND=1.
  connection_state->congestion_control_metadata->fabric_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(2.0,
                                                 falcon_rue::kFractionalBits);
  connection_state->congestion_control_metadata->nic_congestion_window = 1;
  connection_state->tx_reliability_metadata.data_window_metadata
      .next_packet_sequence_number = 1;
  connection_state->tx_reliability_metadata.data_window_metadata
      .outstanding_requests_counter = 1;
  connection_state->tx_reliability_metadata.request_window_metadata
      .next_packet_sequence_number = 0;
  connection_state->tx_reliability_metadata.request_window_metadata
      .outstanding_requests_counter = 0;
  // Data window does not meet CC criteria, because limited by ncwnd.
  CHECK_EQ(reliability_manager_->MeetsInitialTransmissionCCTxGatingCriteria(
               1, falcon::PacketType::kPushUnsolicitedData),
           false);
  // Request window meets CC criteria.
  CHECK_EQ(reliability_manager_->MeetsInitialTransmissionCCTxGatingCriteria(
               1, falcon::PacketType::kPullRequest),
           true);
  connection_state->tx_reliability_metadata.request_window_metadata
      .outstanding_requests_counter = 1;
  connection_state->tx_reliability_metadata.request_window_metadata
      .next_packet_sequence_number = 1;
  // After request NPSN=1, request window does not meet CC criteria.
  CHECK_EQ(reliability_manager_->MeetsInitialTransmissionCCTxGatingCriteria(
               1, falcon::PacketType::kPullRequest),
           false);
}

TEST_P(ProtocolPacketReliabilityManagerTest, RetransmissionGatingCriteria) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
  InitFalcon(config);

  // Initialize the connection state and get a handle on it.
  uint32_t source_connection_id = 1;
  auto connection_metadata = FalconTestingHelpers::InitializeConnectionMetadata(
      falcon_.get(), source_connection_id);
  ConnectionState* connection_state =
      FalconTestingHelpers::InitializeConnectionState(
          falcon_.get(), std::move(connection_metadata));

  // Initialize FCWND to be 2, RBPSN = 0, NBPSN = 1 (meets transmission criteria
  // of PSN < RBPSN + fcwnd)
  connection_state->congestion_control_metadata->fabric_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(2.0,
                                                 falcon_rue::kFractionalBits);
  CHECK_EQ(reliability_manager_->MeetsRetransmissionCCTxGatingCriteria(
               1, 1, falcon::PacketType::kPullRequest),
           true);

  // Initialize FCWND to be 1, RBPSN = 0, NBPSN = 1 (does not meets transmission
  // criteria of PSN < RBPSN + fcwnd )
  connection_state->congestion_control_metadata->fabric_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(1.0,
                                                 falcon_rue::kFractionalBits);
  CHECK_EQ(reliability_manager_->MeetsRetransmissionCCTxGatingCriteria(
               1, 1, falcon::PacketType::kPullRequest),
           false);
}

TEST_P(ProtocolPacketReliabilityManagerTest, HandleAckFromUlpTest) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
  config.set_inter_host_rx_scheduling_tick_ns(0);
  InitFalcon(config);

  // Initialize the connection state and get a handle on it.
  uint32_t source_connection_id = 1;
  auto connection_metadata = FalconTestingHelpers::InitializeConnectionMetadata(
      falcon_.get(), source_connection_id);
  ConnectionState* connection_state =
      FalconTestingHelpers::InitializeConnectionState(
          falcon_.get(), std::move(connection_metadata));

  // Create a hypothetical receive context for a push request with RSN = 1.
  connection_state->rx_reliability_metadata
      .received_packet_contexts[{1, TransactionLocation::kTarget}] =
      FalconTestingHelpers::CreateReceivedPacketContext(GetFalconVersion());
  connection_state->rx_reliability_metadata
      .received_packet_contexts[{1, TransactionLocation::kTarget}]
      ->type = falcon::PacketType::kPushRequest;

  // Handle ACK received from ULP.
  std::unique_ptr<OpaqueCookie> cookie =
      CreateOpaqueCookie(source_connection_id);
  EXPECT_OK(reliability_manager_->HandleAckFromUlp(1, 1, *cookie));

  // Check that the receiver context does not exist anymore.
  ASSERT_EQ(connection_state->rx_reliability_metadata
                .GetReceivedPacketContext({1, TransactionLocation::kTarget})
                .status()
                .code(),
            absl::StatusCode::kNotFound);
}

TEST_P(ProtocolPacketReliabilityManagerTest, TransmitRnrNackTest) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
  config.set_inter_host_rx_scheduling_tick_ns(0);
  InitFalcon(config);
  // Use fake traffic shaper instead of the default mock traffic shaper.
  FalconTestingHelpers::FakeTrafficShaper fake_shaper;
  falcon_->ConnectShaper(&fake_shaper);

  // Initialize the connection state.
  uint32_t source_connection_id = 1;
  auto connection_metadata = FalconTestingHelpers::InitializeConnectionMetadata(
      falcon_.get(), source_connection_id);
  FalconTestingHelpers::InitializeConnectionState(
      falcon_.get(), std::move(connection_metadata));

  // A PushData RSN=0.
  {
    auto push_us = std::make_unique<Packet>();
    push_us->packet_type = falcon::PacketType::kPushUnsolicitedData;
    push_us->falcon.dest_cid = 1;
    push_us->falcon.psn = 0;
    push_us->falcon.rsn = 0;
    falcon_->TransferRxPacket(std::move(push_us));
  }

  // A PullReq RSN=1;
  {
    auto pull_req = std::make_unique<Packet>();
    pull_req->packet_type = falcon::PacketType::kPullRequest;
    pull_req->falcon.dest_cid = 1;
    pull_req->falcon.psn = 0;
    pull_req->falcon.rsn = 1;
    falcon_->TransferRxPacket(std::move(pull_req));
  }

  std::unique_ptr<OpaqueCookie> cookie1 =
      CreateOpaqueCookie(source_connection_id);
  env_.RunFor(absl::ZeroDuration());
  // ULP RNR-NACKs both.
  falcon_->AckTransaction(1, 0, Packet::Syndrome::kRnrNak,
                          absl::Microseconds(10), std::move(cookie1));
  std::unique_ptr<OpaqueCookie> cookie2 =
      CreateOpaqueCookie(source_connection_id);
  falcon_->AckTransaction(1, 1, Packet::Syndrome::kRnrNak,
                          absl::Microseconds(10), std::move(cookie2));

  // Another PushData RSN=2 arrive after RNR NACK to RSN 0 and 1. It should
  // trigger RNR NACK without getting RNR NACK from ULP.
  {
    auto push_us2 = std::make_unique<Packet>();
    push_us2->packet_type = falcon::PacketType::kPushUnsolicitedData;
    push_us2->falcon.dest_cid = 1;
    push_us2->falcon.psn = 1;
    push_us2->falcon.rsn = 2;
    falcon_->TransferRxPacket(std::move(push_us2));
  }

  env_.RunFor(absl::Microseconds(1));

  // 2 RNR NACK should be sent, one for each PushData.
  EXPECT_EQ(fake_shaper.packet_list.size(), 2);
  EXPECT_EQ(fake_shaper.packet_list[0].packet_type, falcon::PacketType::kNack);
  EXPECT_EQ(fake_shaper.packet_list[0].nack.nack_psn, 0);
  EXPECT_EQ(fake_shaper.packet_list[1].packet_type, falcon::PacketType::kNack);
  EXPECT_EQ(fake_shaper.packet_list[1].nack.nack_psn, 1);
  fake_shaper.packet_list.clear();

  // Receive duplicate PushData RSN=0. Should trigger retry without sending RNR
  // NACK.
  {
    auto push_us = std::make_unique<Packet>();
    push_us->packet_type = falcon::PacketType::kPushUnsolicitedData;
    push_us->falcon.dest_cid = 1;
    push_us->falcon.psn = 0;
    push_us->falcon.rsn = 0;
    falcon_->TransferRxPacket(std::move(push_us));
  }
  env_.RunFor(absl::Microseconds(1));
  EXPECT_EQ(fake_shaper.packet_list.size(), 0);

  // Receive duplicate PushData RSN=2. Should not send to ULP as RSN 1 is
  // waiting. Should send RNR NACK.
  {
    auto push_us2 = std::make_unique<Packet>();
    push_us2->packet_type = falcon::PacketType::kPushUnsolicitedData;
    push_us2->falcon.dest_cid = 1;
    push_us2->falcon.psn = 1;
    push_us2->falcon.rsn = 2;
    falcon_->TransferRxPacket(std::move(push_us2));
  }
  env_.RunFor(absl::Microseconds(1));
  EXPECT_EQ(fake_shaper.packet_list.size(), 1);
  EXPECT_EQ(fake_shaper.packet_list[0].packet_type, falcon::PacketType::kNack);
  EXPECT_EQ(fake_shaper.packet_list[0].nack.nack_psn, 1);
}

TEST_P(ProtocolPacketReliabilityManagerTest, ARBitSetFcwndTest) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
  config.set_inter_host_rx_scheduling_tick_ns(0);
  config.set_ack_request_fcwnd_threshold(5);
  config.set_ack_request_percent(10);
  InitFalcon(config);

  auto gen1_reliability_manager =
      dynamic_cast<ProtocolPacketReliabilityManager*>(reliability_manager_);
  int ar_bit_set_count = 0;
  for (int i = 0; i < 100; i++) {
    if (gen1_reliability_manager->MeetsAckRequestedBitSetCriteriaForTesting(
            /*fcwnd=*/20)) {
      ++ar_bit_set_count;
    }
  }

  // Given fcwnd > ar_threshold, we expect bit to be set with ar_percent
  // probability.
  EXPECT_NEAR(ar_bit_set_count, 10, 4);

  ar_bit_set_count = 0;
  for (int i = 0; i < 100; i++) {
    if (gen1_reliability_manager->MeetsAckRequestedBitSetCriteriaForTesting(
            /*fcwnd=*/5)) {
      ++ar_bit_set_count;
    }
  }

  // Given fcwnd <= ar_threshold, the bit should be set always.
  EXPECT_EQ(ar_bit_set_count, 100);
}

TEST_P(ProtocolPacketReliabilityManagerTest, IgnoreArBitDueToOowDrops) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
  auto early_retx = config.mutable_early_retx();
  early_retx->set_enable_eack_own(true);
  auto eack_own_config = early_retx->mutable_eack_own_metadata();
  eack_own_config->set_ignore_incoming_ar_bit(true);
  InitFalcon(config);
  StrictMock<FalconTestingHelpers::MockTrafficShaper> shaper;
  falcon_->ConnectShaper(&shaper);

  // Initialize connection metadata and connection state.
  auto connection_metadata =
      FalconTestingHelpers::InitializeConnectionMetadata(falcon_.get());
  FalconTestingHelpers::InitializeConnectionState(
      falcon_.get(), std::move(connection_metadata));

  // Receive an OOW packet in the request window with AR-bit set.
  auto rx_packet = std::make_unique<Packet>();
  rx_packet->packet_type = falcon::PacketType::kPullRequest;
  rx_packet->falcon.dest_cid = 1;
  rx_packet->falcon.psn = 280;  // PSN outside the window
  rx_packet->falcon.rsn = 1;
  rx_packet->falcon.ack_req = true;  // AR-bit set.
  EXPECT_EQ(reliability_manager_->ReceivePacket(rx_packet.get()).code(),
            absl::StatusCode::kOutOfRange);

  env_.RunFor(absl::Nanoseconds(5));
}

TEST_P(ProtocolPacketReliabilityManagerTest, ARBitSetTest) {
  constexpr int kNumPackets = 1000;
  int ar_percents[] = {
      100, 66, 40,
      22,  12, 6};  // some possible values in hardware 200/(1+2^#bits)
  for (const auto ar_percent : ar_percents) {
    FalconConfig config =
        DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
    config.set_inter_host_rx_scheduling_tick_ns(0);
    config.set_ack_request_fcwnd_threshold(5);
    config.set_ack_request_percent(ar_percent);
    InitFalcon(config);
    auto gen1_reliability_manager =
        dynamic_cast<ProtocolPacketReliabilityManager*>(reliability_manager_);

    int ar_bit_set_count = 0;
    for (int i = 0; i < kNumPackets; i++) {
      if (gen1_reliability_manager->MeetsAckRequestedBitSetCriteriaForTesting(
              /*fcwnd=*/20)) {
        ++ar_bit_set_count;
      }
    }

    // Given fcwnd > ar_threshold, we expect bit to be set with ar_percent
    // probability.
    EXPECT_NEAR(ar_bit_set_count, ar_percent / 100.0 * kNumPackets,
                0.03 * kNumPackets);  // 3% error
  }
}

TEST_P(ProtocolPacketReliabilityManagerTest, DisableARBitTest) {
  constexpr int kNumPackets = 1000;
  int ar_percents[] = {100, 66, 40, 22, 12, 6};
  for (const auto ar_percent : ar_percents) {
    FalconConfig config =
        DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
    // If the enable flag is set to false, Falcon will not send out any packets
    // with AR-bit, regardless of the percentage set.
    config.set_enable_ack_request_bit(false);
    config.set_ack_request_percent(ar_percent);
    InitFalcon(config);
    auto gen1_reliability_manager =
        dynamic_cast<ProtocolPacketReliabilityManager*>(reliability_manager_);

    int ar_bit_set_count = 0;
    for (int i = 0; i < kNumPackets; i++) {
      if (gen1_reliability_manager->MeetsAckRequestedBitSetCriteriaForTesting(
              /*fcwnd=*/20)) {
        ++ar_bit_set_count;
      }
    }

    EXPECT_EQ(ar_bit_set_count, 0);
  }
}

TEST_P(ProtocolPacketReliabilityManagerTest, RxWindowShiftByRxWindowSize) {
  ::testing::InSequence s;
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
  config.set_inter_host_rx_scheduling_tick_ns(0);
  InitFalcon(config);

  // Steps to initialize a transaction and get handle on the packet and
  // connection state.
  Packet* packet;
  ConnectionState* connection_state;
  std::tie(packet, connection_state) = FalconTestingHelpers::SetupTransaction(
      falcon_.get(), TransactionType::kPushUnsolicited,
      TransactionLocation::kInitiator,
      TransactionState::kPushUnsolicitedReqUlpRx,
      falcon::PacketType::kPushUnsolicitedData, 1);

  auto rx_packet = std::make_unique<Packet>();
  rx_packet->packet_type = falcon::PacketType::kPullRequest;
  rx_packet->falcon.dest_cid = 1;
  // PSN 1~63 arrives.
  for (int i = 1; i < 64; i++) {
    rx_packet->falcon.psn = i;
    rx_packet->falcon.rsn = i;
    EXPECT_OK(reliability_manager_->ReceivePacket(rx_packet.get()));
  }
  // The BPSN should be 0.
  CHECK_EQ(connection_state->rx_reliability_metadata.request_window_metadata
               .base_packet_sequence_number,
           0);

  // PSN 0 arrives.
  rx_packet->falcon.psn = 0;
  rx_packet->falcon.rsn = 0;
  EXPECT_OK(reliability_manager_->ReceivePacket(rx_packet.get()));

  // The BPSN should be 64.
  CHECK_EQ(connection_state->rx_reliability_metadata.request_window_metadata
               .base_packet_sequence_number,
           64);
}

TEST_P(ProtocolPacketReliabilityManagerTest, NackBasedEarlyRetxTest) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
  config.set_inter_host_rx_scheduling_tick_ns(0);
  auto early_retx = config.mutable_early_retx();
  early_retx->set_enable_ooo_count(false);
  early_retx->set_enable_ooo_distance(false);
  InitFalcon(config);

  int n_packet = 10;

  // Steps to initialize a transaction and get handle on the packet and
  // connection state.
  std::vector<Packet*> packet;
  ConnectionState* connection_state;
  for (int i = 0; i < n_packet; i++) {
    packet.emplace_back();
    std::tie(packet[i], connection_state) =
        FalconTestingHelpers::SetupTransaction(
            falcon_.get(), TransactionType::kPushUnsolicited,
            TransactionLocation::kInitiator,
            TransactionState::kPushUnsolicitedReqUlpRx,
            falcon::PacketType::kPushUnsolicitedData, 1, i, i);

    // Explicitly setting the maximum retries to 8.
    connection_state->connection_metadata->max_retransmit_attempts = 8;
    // Ensures the packet has its connection ID set up appropriately.
    packet[i]->metadata.scid = 1;
    // Transmit the packet
    EXPECT_OK(reliability_manager_->TransmitPacket(
        1, i, falcon::PacketType::kPushUnsolicitedData));
  }
  auto& metadata =
      connection_state->tx_reliability_metadata.data_window_metadata;
  CheckOutstandingPacketList(metadata.outstanding_packets,
                             {0, 1, 2, 3, 4, 5, 6, 7, 8, 9});

  // At 100us, a NACK arrives.
  auto nack = std::make_unique<Packet>();
  nack->packet_type = falcon::PacketType::kNack;
  nack->nack.code = falcon::NackCode::kReserved;
  nack->nack.dest_cid = 1;
  nack->nack.rdbpsn = 0;
  nack->nack.rrbpsn = 0;
  nack->nack.nack_psn = 129;
  // Set T4=50us, T1=0, so instant_rtt=50.
  nack->timestamps.received_timestamp = absl::Microseconds(50);
  nack->nack.timestamp_1 = absl::ZeroDuration();
  EXPECT_OK(env_.ScheduleEvent(absl::Microseconds(100), [&]() {
    EXPECT_OK(reliability_manager_->ReceivePacket(nack.get()));
  }));
  env_.RunFor(absl::Microseconds(100));

  // Test BPSN.
  EXPECT_EQ(metadata.base_packet_sequence_number, 0);
  // All packets are scheduled for early-retransmission (and the first one
  // retransmitted immediately).
  CheckOutstandingPacketList(metadata.outstanding_packets, {0});
  // After retransmitted,
  for (uint32_t rsn = 0; rsn < n_packet; rsn++) {
    EXPECT_OK(reliability_manager_->TransmitPacket(
        1, rsn, falcon::PacketType::kPushUnsolicitedData));
  }
  // all PSNs should be in outstanding packets.
  CheckOutstandingPacketList(metadata.outstanding_packets,
                             {0, 1, 2, 3, 4, 5, 6, 7, 8, 9});

  // At 110us, another NACK. Recency check should avoid retx.
  EXPECT_OK(env_.ScheduleEvent(absl::Microseconds(10), [&]() {
    EXPECT_OK(reliability_manager_->ReceivePacket(nack.get()));
  }));
  env_.RunFor(absl::Microseconds(10));
  // Test BPSN.
  EXPECT_EQ(metadata.base_packet_sequence_number, 0);
  // All packets should remain in outstanding_packets.
  CheckOutstandingPacketList(metadata.outstanding_packets,
                             {0, 1, 2, 3, 4, 5, 6, 7, 8, 9});

  // Timeout based retransmission attempts should be 0 for outstanding packets.
  for (uint32_t rsn = 0; rsn < n_packet; rsn++) {
    // Get a handle on packet to verify retransmit count.
    EXPECT_EQ(
        connection_state->GetTransaction({rsn, TransactionLocation::kInitiator})
            .value()
            ->GetPacketMetadata(falcon::PacketType::kPushUnsolicitedData)
            .value()
            ->timeout_retransmission_attempts,
        0);
  }
}

TEST_P(ProtocolPacketReliabilityManagerTest, RnrNackRetxTest) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
  config.set_inter_host_rx_scheduling_tick_ns(0);
  InitFalcon(config);

  int n_packet = 10;

  // Steps to initialize a transaction and get handle on the packet and
  // connection state.
  std::vector<Packet*> packet;
  ConnectionState* connection_state;
  for (int i = 0; i < n_packet; i++) {
    packet.emplace_back();
    std::tie(packet[i], connection_state) =
        FalconTestingHelpers::SetupTransaction(
            falcon_.get(), TransactionType::kPushUnsolicited,
            TransactionLocation::kInitiator,
            TransactionState::kPushUnsolicitedReqUlpRx,
            falcon::PacketType::kPushUnsolicitedData, 1, i, i);

    // Explicitly setting the maximum retries to 8.
    connection_state->connection_metadata->max_retransmit_attempts = 8;
    // Ensures the packet has its connection ID set up appropriately.
    packet[i]->metadata.scid = 1;
    // Transmit the packet
    EXPECT_OK(reliability_manager_->TransmitPacket(
        1, i, falcon::PacketType::kPushUnsolicitedData));
  }
  auto& metadata =
      connection_state->tx_reliability_metadata.data_window_metadata;
  CheckOutstandingPacketList(metadata.outstanding_packets,
                             {0, 1, 2, 3, 4, 5, 6, 7, 8, 9});

  // At 10us, a NACK arrives.
  auto nack = std::make_unique<Packet>();
  nack->packet_type = falcon::PacketType::kNack;
  nack->nack.code = falcon::NackCode::kUlpReceiverNotReady;
  nack->nack.dest_cid = 1;
  nack->nack.rdbpsn = 2;
  nack->nack.rrbpsn = 0;
  nack->nack.nack_psn = 2;
  nack->nack.rnr_timeout = absl::Milliseconds(10);
  // Set T4=50us, T1=0, so instant_rtt=50.
  nack->timestamps.received_timestamp = absl::Microseconds(50);
  nack->nack.timestamp_1 = absl::ZeroDuration();
  env_.RunFor(absl::Microseconds(10));
  EXPECT_OK(reliability_manager_->ReceivePacket(nack.get()));

  // All packets should be retx at 10ms.
  env_.RunFor(absl::Milliseconds(10) - absl::Microseconds(10) -
              absl::Nanoseconds(1));
  CheckOutstandingPacketList(metadata.outstanding_packets,
                             {2, 3, 4, 5, 6, 7, 8, 9});
  env_.RunFor(absl::Nanoseconds(1));
  // First packet should be sent out.
  CheckOutstandingPacketList(metadata.outstanding_packets, {2});
}

TEST_P(ProtocolPacketReliabilityManagerTest, RnrNackSmallTimeoutRetxTest) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
  config.set_inter_host_rx_scheduling_tick_ns(0);
  InitFalcon(config);

  int n_packet = 10;

  // Steps to initialize a transaction and get handle on the packet and
  // connection state.
  std::vector<Packet*> packet;
  ConnectionState* connection_state;
  for (int i = 0; i < n_packet; i++) {
    packet.emplace_back();
    std::tie(packet[i], connection_state) =
        FalconTestingHelpers::SetupTransaction(
            falcon_.get(), TransactionType::kPushUnsolicited,
            TransactionLocation::kInitiator,
            TransactionState::kPushUnsolicitedReqUlpRx,
            falcon::PacketType::kPushUnsolicitedData, 1, i, i);

    // Explicitly setting the maximum retries to 8.
    connection_state->connection_metadata->max_retransmit_attempts = 8;
    // Ensures the packet has its connection ID set up appropriately.
    packet[i]->metadata.scid = 1;
    // Transmit the packet
    EXPECT_OK(reliability_manager_->TransmitPacket(
        1, i, falcon::PacketType::kPushUnsolicitedData));
  }
  auto& metadata =
      connection_state->tx_reliability_metadata.data_window_metadata;
  CheckOutstandingPacketList(metadata.outstanding_packets,
                             {0, 1, 2, 3, 4, 5, 6, 7, 8, 9});

  // At 10us, a NACK arrives.
  auto nack = std::make_unique<Packet>();
  nack->packet_type = falcon::PacketType::kNack;
  nack->nack.code = falcon::NackCode::kUlpReceiverNotReady;
  nack->nack.dest_cid = 1;
  nack->nack.rdbpsn = 2;
  nack->nack.rrbpsn = 0;
  nack->nack.nack_psn = 2;
  nack->nack.rnr_timeout = absl::Microseconds(100);
  // Set T4=50us, T1=0, so instant_rtt=50.
  nack->timestamps.received_timestamp = absl::Microseconds(50);
  nack->nack.timestamp_1 = absl::ZeroDuration();
  env_.RunFor(absl::Microseconds(10));
  EXPECT_OK(reliability_manager_->ReceivePacket(nack.get()));

  // Only RSN 2 should be retx at 100us.
  env_.RunFor(absl::Microseconds(100) - absl::Microseconds(10) -
              absl::Nanoseconds(1));
  CheckOutstandingPacketList(metadata.outstanding_packets,
                             {2, 3, 4, 5, 6, 7, 8, 9});
  env_.RunFor(absl::Nanoseconds(1));
  EXPECT_EQ(metadata.outstanding_retransmission_requests_counter, 1);
}

TEST_P(ProtocolPacketReliabilityManagerTest, OooCountTest) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
  config.set_inter_host_rx_scheduling_tick_ns(0);
  auto early_retx = config.mutable_early_retx();
  early_retx->set_ooo_count_threshold(3);
  early_retx->set_enable_ooo_count(true);
  early_retx->set_enable_ooo_distance(false);
  InitFalcon(config);

  int n_packet = 10;

  // Steps to initialize a transaction and get handle on the packet and
  // connection state.
  std::vector<Packet*> packet;
  ConnectionState* connection_state;
  for (int i = 0; i < n_packet; i++) {
    packet.emplace_back();
    std::tie(packet[i], connection_state) =
        FalconTestingHelpers::SetupTransaction(
            falcon_.get(), TransactionType::kPushUnsolicited,
            TransactionLocation::kInitiator,
            TransactionState::kPushUnsolicitedReqUlpRx,
            falcon::PacketType::kPushUnsolicitedData, 1, i, i);

    // Explicitly setting the maximum retries to 8.
    connection_state->connection_metadata->max_retransmit_attempts = 8;
    // Ensures the packet has its connection ID set up appropriately.
    packet[i]->metadata.scid = 1;
    // Transmit the packet
    EXPECT_OK(reliability_manager_->TransmitPacket(
        1, i, falcon::PacketType::kPushUnsolicitedData));
  }
  auto& metadata =
      connection_state->tx_reliability_metadata.data_window_metadata;
  CheckOutstandingPacketList(metadata.outstanding_packets,
                             {0, 1, 2, 3, 4, 5, 6, 7, 8, 9});

  // At 100us, an E-ACK with received-bitmap 0b0001011010.
  auto eack = std::make_unique<Packet>();
  eack->packet_type = falcon::PacketType::kAck;
  eack->ack.ack_type = Packet::Ack::kEack;
  eack->ack.dest_cid = 1;
  eack->ack.rdbpsn = 0;
  eack->ack.rrbpsn = 0;
  // Set T4=50us, T1=0, so instant_rtt=50.
  eack->timestamps.received_timestamp = absl::Microseconds(50);
  eack->ack.timestamp_1 = absl::ZeroDuration();
  for (auto psn : {1, 3, 4, 6}) {
    eack->ack.received_bitmap.Set(psn, true);
  }
  EXPECT_OK(env_.ScheduleEvent(absl::Microseconds(100), [&]() {
    EXPECT_OK(reliability_manager_->ReceivePacket(eack.get()));
  }));
  env_.RunFor(absl::Microseconds(100));

  // Test BPSN.
  EXPECT_EQ(metadata.base_packet_sequence_number, 0);
  // 0,2 are scheduled for early-retransmission, and 0 is sent out immediately.
  CheckOutstandingPacketList(metadata.outstanding_packets,
                             {0, 1, 3, 4, 5, 6, 7, 8, 9});
  EXPECT_EQ(metadata.outstanding_retransmission_requests_counter, 1);
  // After 0,2 are retransmitted,
  for (uint32_t rsn : {0, 2}) {
    EXPECT_OK(reliability_manager_->TransmitPacket(
        1, rsn, falcon::PacketType::kPushUnsolicitedData));
  }
  // all PSNs should be in outstanding packets.
  CheckOutstandingPacketList(metadata.outstanding_packets,
                             {0, 1, 2, 3, 4, 5, 6, 7, 8, 9});

  // At 110us, another E-ACK, with DBPSN=2 and received-bitmap 0b01110110 (0 is
  // Acked, 2 is received but not Acked, 7 and 8 are newly received).
  eack->ack.rdbpsn = 2;
  eack->ack.received_bitmap = FalconAckPacketBitmap(kRxDataWindowSize);
  for (auto index : {1, 2, 4, 5, 6}) {
    eack->ack.received_bitmap.Set(index, true);
  }
  EXPECT_OK(env_.ScheduleEvent(absl::Microseconds(10), [&]() {
    EXPECT_OK(reliability_manager_->ReceivePacket(eack.get()));
  }));
  env_.RunFor(absl::Microseconds(10));
  // Test BPSN.
  EXPECT_EQ(metadata.base_packet_sequence_number, 2);
  // 2 should not be early-retx again, because only 10us (<instant_rtt) has
  // passed since it is retxed. So recency check stops, and nothing retx'ed.
  CheckOutstandingPacketList(metadata.outstanding_packets,
                             {2, 3, 4, 5, 6, 7, 8, 9});

  // At 110.1us, another E-ACK, with DBPSN=9 and received-bitmap 0b0 (9 is not
  // received).
  eack->ack.rdbpsn = 9;
  eack->ack.received_bitmap = FalconAckPacketBitmap(kRxDataWindowSize);
  EXPECT_OK(env_.ScheduleEvent(absl::Nanoseconds(100), [&]() {
    EXPECT_OK(reliability_manager_->ReceivePacket(eack.get()));
  }));
  env_.RunFor(absl::Nanoseconds(100));
  // Test BPSN.
  EXPECT_EQ(metadata.base_packet_sequence_number, 9);
  // only 9 should remain in the outstanding packet list.
  CheckOutstandingPacketList(metadata.outstanding_packets, {9});
}

TEST_P(ProtocolPacketReliabilityManagerTest, OooDistanceTest) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
  config.set_inter_host_rx_scheduling_tick_ns(0);
  auto early_retx = config.mutable_early_retx();
  early_retx->set_ooo_distance_threshold(3);
  early_retx->set_enable_ooo_count(false);
  early_retx->set_enable_ooo_distance(true);
  InitFalcon(config);

  int n_packet = 10;

  // Steps to initialize a transaction and get handle on the packet and
  // connection state.
  std::vector<Packet*> packet;
  ConnectionState* connection_state;
  for (int i = 0; i < n_packet; i++) {
    packet.emplace_back();
    std::tie(packet[i], connection_state) =
        FalconTestingHelpers::SetupTransaction(
            falcon_.get(), TransactionType::kPushUnsolicited,
            TransactionLocation::kInitiator,
            TransactionState::kPushUnsolicitedReqUlpRx,
            falcon::PacketType::kPushUnsolicitedData, 1, i, i);

    // Explicitly setting the maximum retries to 8.
    connection_state->connection_metadata->max_retransmit_attempts = 8;
    // Ensures the packet has its connection ID set up appropriately.
    packet[i]->metadata.scid = 1;
    // Transmit the packet
    EXPECT_OK(reliability_manager_->TransmitPacket(
        1, i, falcon::PacketType::kPushUnsolicitedData));
  }
  auto& metadata =
      connection_state->tx_reliability_metadata.data_window_metadata;
  CheckOutstandingPacketList(metadata.outstanding_packets,
                             {0, 1, 2, 3, 4, 5, 6, 7, 8, 9});
  EXPECT_EQ(metadata.outstanding_retransmission_requests_counter, 0);

  // At 100us, an E-ACK with received-bitmap 0b0000010000.
  auto eack = std::make_unique<Packet>();
  eack->packet_type = falcon::PacketType::kAck;
  eack->ack.ack_type = Packet::Ack::kEack;
  eack->ack.dest_cid = 1;
  eack->ack.rdbpsn = 0;
  eack->ack.rrbpsn = 0;
  // Set T4=50us, T1=0, so instant_rtt=50.
  eack->timestamps.received_timestamp = absl::Microseconds(50);
  eack->ack.timestamp_1 = absl::ZeroDuration();
  for (auto psn : {4}) {
    eack->ack.received_bitmap.Set(psn, true);
  }
  EXPECT_OK(env_.ScheduleEvent(absl::Microseconds(100), [&]() {
    EXPECT_OK(reliability_manager_->ReceivePacket(eack.get()));
  }));
  env_.RunFor(absl::Microseconds(100));

  // Test BPSN.
  EXPECT_EQ(metadata.base_packet_sequence_number, 0);
  // 0, 1 are scheduled for early-retransmission, and 0 sent out immediately.
  CheckOutstandingPacketList(metadata.outstanding_packets,
                             {0, 2, 3, 4, 5, 6, 7, 8, 9});
  EXPECT_EQ(metadata.outstanding_retransmission_requests_counter, 1);
  // After 0, 1 are retransmitted,
  for (uint32_t rsn : {0, 1}) {
    EXPECT_OK(reliability_manager_->TransmitPacket(
        1, rsn, falcon::PacketType::kPushUnsolicitedData));
  }
  // all PSNs should be in outstanding packets.
  CheckOutstandingPacketList(metadata.outstanding_packets,
                             {0, 1, 2, 3, 4, 5, 6, 7, 8, 9});

  // At 110us, another E-ACK, with DBPSN=0 and received-bitmap 0b0000110000.
  eack->ack.rdbpsn = 0;
  eack->ack.received_bitmap = FalconAckPacketBitmap(kRxDataWindowSize);
  for (auto index : {4, 5}) {
    eack->ack.received_bitmap.Set(index, true);
  }
  EXPECT_OK(env_.ScheduleEvent(absl::Microseconds(10), [&]() {
    EXPECT_OK(reliability_manager_->ReceivePacket(eack.get()));
  }));
  env_.RunFor(absl::Microseconds(10));
  // Test BPSN.
  EXPECT_EQ(metadata.base_packet_sequence_number, 0);
  // 0 should not be early-retx again, because only 10us (<instant_rtt) has
  // passed since it is retxed. So recency check stops, and nothing retx'ed.
  CheckOutstandingPacketList(metadata.outstanding_packets,
                             {0, 1, 2, 3, 4, 5, 6, 7, 8, 9});

  // At 120us, another E-ACK, with DBPSN=2 and received-bitmap 0b10001100.
  eack->ack.rdbpsn = 2;
  eack->ack.received_bitmap = FalconAckPacketBitmap(kRxDataWindowSize);
  for (auto index : {2, 3, 7}) {
    eack->ack.received_bitmap.Set(index, true);
  }
  EXPECT_OK(env_.ScheduleEvent(absl::Microseconds(10), [&]() {
    EXPECT_OK(reliability_manager_->ReceivePacket(eack.get()));
  }));
  env_.RunFor(absl::Microseconds(10));
  // Test BPSN.
  EXPECT_EQ(metadata.base_packet_sequence_number, 2);
  // 2, 3, 6 should be retx, and 2 sent out immediately.
  CheckOutstandingPacketList(metadata.outstanding_packets, {2, 4, 5, 7, 8, 9});
  EXPECT_EQ(metadata.outstanding_retransmission_requests_counter, 3);
  // After 2, 3, 6 are retransmitted,
  for (uint32_t rsn : {2, 3, 6}) {
    EXPECT_OK(reliability_manager_->TransmitPacket(
        1, rsn, falcon::PacketType::kPushUnsolicitedData));
  }
  // all PSNs should be in outstanding packets.
  CheckOutstandingPacketList(metadata.outstanding_packets,
                             {2, 3, 4, 5, 6, 7, 8, 9});

  // Timeout based retransmission attempts should be 0 for outstanding packets.
  for (uint32_t rsn = 2; rsn < n_packet; rsn++) {
    // Get a handle on packet to verify retransmit count.
    EXPECT_EQ(
        connection_state->GetTransaction({rsn, TransactionLocation::kInitiator})
            .value()
            ->GetPacketMetadata(falcon::PacketType::kPushUnsolicitedData)
            .value()
            ->timeout_retransmission_attempts,
        0);
  }
}

//
// EACK-OWN implementation is done.
TEST_P(ProtocolPacketReliabilityManagerTest, DISABLED_OooDistanceWithOowTest) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
  config.set_inter_host_rx_scheduling_tick_ns(0);
  auto early_retx = config.mutable_early_retx();
  early_retx->set_ooo_distance_threshold(3);
  early_retx->set_enable_ooo_count(false);
  early_retx->set_enable_ooo_distance(true);
  early_retx->set_enable_eack_own(true);
  InitFalcon(config);

  int n_packet = 10;

  // Steps to initialize a transaction and get handle on the packet and
  // connection state.
  std::vector<Packet*> packet;
  ConnectionState* connection_state;
  for (int i = 0; i < n_packet; i++) {
    packet.emplace_back();
    std::tie(packet[i], connection_state) =
        FalconTestingHelpers::SetupTransaction(
            falcon_.get(), TransactionType::kPushUnsolicited,
            TransactionLocation::kInitiator,
            TransactionState::kPushUnsolicitedReqUlpRx,
            falcon::PacketType::kPushUnsolicitedData, 1, i, i);

    // Explicitly setting the maximum retries to 8.
    connection_state->connection_metadata->max_retransmit_attempts = 8;
    // Ensures the packet has its connection ID set up appropriately.
    packet[i]->metadata.scid = 1;
    // Transmit the packet
    EXPECT_OK(reliability_manager_->TransmitPacket(
        1, i, falcon::PacketType::kPushUnsolicitedData));
  }
  auto& metadata =
      connection_state->tx_reliability_metadata.data_window_metadata;
  CheckOutstandingPacketList(metadata.outstanding_packets,
                             {0, 1, 2, 3, 4, 5, 6, 7, 8, 9});

  // At 100us, an E-ACK with received-bitmap 0b0000010000, with OOW bit.
  auto eack = std::make_unique<Packet>();
  eack->packet_type = falcon::PacketType::kAck;
  eack->ack.ack_type = Packet::Ack::kEack;
  eack->ack.dest_cid = 1;
  eack->ack.rdbpsn = 0;
  eack->ack.rrbpsn = 0;
  // Set T4=50us, T1=0, so instant_rtt=50.
  eack->timestamps.received_timestamp = absl::Microseconds(50);
  eack->ack.timestamp_1 = absl::ZeroDuration();
  for (auto psn : {4}) {
    eack->ack.received_bitmap.Set(psn, true);
  }
  eack->ack.data_own = true;
  EXPECT_OK(env_.ScheduleEvent(absl::Microseconds(100), [&]() {
    EXPECT_OK(reliability_manager_->ReceivePacket(eack.get()));
  }));
  env_.RunFor(absl::Microseconds(100));

  // Test BPSN.
  EXPECT_EQ(metadata.base_packet_sequence_number, 0);
  // All PSNs except 4 are scheduled for early-retransmission, and 0 sent out
  // immediately.
  CheckOutstandingPacketList(metadata.outstanding_packets, {0, 4});
  // After all retransmissions,
  for (uint32_t rsn : {0, 1, 2, 3, 5, 6, 7, 8, 9}) {
    EXPECT_OK(reliability_manager_->TransmitPacket(
        1, rsn, falcon::PacketType::kPushUnsolicitedData));
  }
  // all PSNs should be in outstanding packets.
  CheckOutstandingPacketList(metadata.outstanding_packets,
                             {0, 1, 2, 3, 4, 5, 6, 7, 8, 9});

  // Timeout based retransmission attempts should be 0 for outstanding packets.
  for (uint32_t rsn = 0; rsn < n_packet; rsn++) {
    // Get a handle on packet_metadata to verify retransmit count.
    EXPECT_EQ(
        connection_state->GetTransaction({rsn, TransactionLocation::kInitiator})
            .value()
            ->GetPacketMetadata(falcon::PacketType::kPushUnsolicitedData)
            .value()
            ->timeout_retransmission_attempts,
        0);
  }
}

TEST_P(ProtocolPacketReliabilityManagerTest, TimeoutRetransmissionCountTest) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
  config.set_inter_host_rx_scheduling_tick_ns(0);
  InitFalcon(config);

  // Steps to initialize a transaction and get handle on the packet and
  // connection state.
  Packet* packet;
  ConnectionState* connection_state;
  std::tie(packet, connection_state) = FalconTestingHelpers::SetupTransaction(
      falcon_.get(), TransactionType::kPull, TransactionLocation::kInitiator,
      TransactionState::kPullReqUlpRx, falcon::PacketType::kPullRequest);

  // Explicitly setting the maximum retries to 1 and timer to 10ns.
  connection_state->connection_metadata->max_retransmit_attempts = 1;
  connection_state->congestion_control_metadata->retransmit_timeout =
      absl::Nanoseconds(10);
  // Ensures the packet has its connection ID set up appropriately.
  packet->metadata.scid = 1;

  EXPECT_OK(reliability_manager_->TransmitPacket(
      1, 0, falcon::PacketType::kPullRequest));
  EXPECT_OK(env_.ScheduleEvent(absl::Nanoseconds(15), [&]() { env_.Stop(); }));
  env_.Run();

  // Get a handle on packet_metadata to verify retransmit count.
  absl::StatusOr<PacketMetadata*> packet_metadata =
      connection_state->GetTransaction({0, TransactionLocation::kInitiator})
          .value()
          ->GetPacketMetadata(falcon::PacketType::kPullRequest);

  // Indicates retransmission was initiated.
  EXPECT_EQ(packet_metadata.value()->timeout_retransmission_attempts, 1);
}

TEST_P(ProtocolPacketReliabilityManagerTest, RetransmittedPullRequestORRCTest) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
  config.set_inter_host_rx_scheduling_tick_ns(0);
  InitFalcon(config);

  // Initialize a pull transaction and get a handle on the Pull Request packet.
  Packet* packet;
  ConnectionState* connection_state;
  std::tie(packet, connection_state) = FalconTestingHelpers::SetupTransaction(
      falcon_.get(), TransactionType::kPull, TransactionLocation::kInitiator,
      TransactionState::kPullReqUlpRx, falcon::PacketType::kPullRequest);

  // Explicitly setting the maximum retries to 2 and timer to 10ns.
  connection_state->connection_metadata->max_retransmit_attempts = 2;
  connection_state->congestion_control_metadata->retransmit_timeout =
      absl::Nanoseconds(10);
  // Ensures the packet has its connection ID set up appropriately.
  packet->metadata.scid = 1;

  EXPECT_OK(reliability_manager_->TransmitPacket(
      1, 0, falcon::PacketType::kPullRequest));
  env_.RunFor(absl::Nanoseconds(14));

  // ORRC = 1 as we expect the packet to be retransmitted once.
  EXPECT_EQ(connection_state->tx_reliability_metadata.request_window_metadata
                .outstanding_retransmission_requests_counter,
            1);

  env_.RunFor(absl::Nanoseconds(25));

  // ORRC = 1 as the same packet is being retransmitted.
  EXPECT_EQ(connection_state->tx_reliability_metadata.request_window_metadata
                .outstanding_retransmission_requests_counter,
            1);

  // Create a fake ACK packet with the required fields setup.
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

  // ORRC = 0 as the retransmitted packet has been ACKed.
  EXPECT_EQ(connection_state->tx_reliability_metadata.request_window_metadata
                .outstanding_retransmission_requests_counter,
            0);
}

TEST_P(ProtocolPacketReliabilityManagerTest,
       RetransmittedPushSolicitedORRCTest) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
  config.set_inter_host_rx_scheduling_tick_ns(0);
  InitFalcon(config);

  constexpr uint32_t kFlowLabel = 1;

  // Initialize a push transaction and get a handle on the Push Request packet.
  Packet* packet;
  ConnectionState* connection_state;
  std::tie(packet, connection_state) = FalconTestingHelpers::SetupTransaction(
      falcon_.get(), TransactionType::kPushSolicited,
      TransactionLocation::kInitiator, TransactionState::kPushSolicitedReqUlpRx,
      falcon::PacketType::kPushRequest);
  packet->metadata.flow_label = kFlowLabel;
  AddOutstandingPacket(
      /*connection_state=*/connection_state,
      /*psn=*/0,
      /*rsn=*/0,
      /*packet_type=*/falcon::PacketType::kPushRequest,
      /*scid=*/connection_state->connection_metadata->scid,
      /*flow_label=*/kFlowLabel);

  // Explicitly setting the maximum retries to 2 and timer to 10ns.
  connection_state->connection_metadata->max_retransmit_attempts = 2;
  connection_state->congestion_control_metadata->retransmit_timeout =
      absl::Nanoseconds(10);
  // Ensures the packet has its connection ID set up appropriately.
  packet->metadata.scid = 1;

  EXPECT_OK(reliability_manager_->TransmitPacket(
      1, 0, falcon::PacketType::kPushRequest));
  env_.RunFor(absl::Nanoseconds(14));

  // ORRC = 1 as we expect the packet to be retransmitted once.
  EXPECT_EQ(connection_state->tx_reliability_metadata.request_window_metadata
                .outstanding_retransmission_requests_counter,
            1);

  env_.RunFor(absl::Nanoseconds(25));

  // ORRC = 1 as the same packet is being retransmitted.
  EXPECT_EQ(connection_state->tx_reliability_metadata.request_window_metadata
                .outstanding_retransmission_requests_counter,
            1);

  // Create a fake ACK packet with the required fields setup.
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

  // ORRC = 1 as the Push Solicited Data corresponding to retransmitted Push
  // Request is yet to be ACKed.
  EXPECT_EQ(connection_state->tx_reliability_metadata.request_window_metadata
                .outstanding_retransmission_requests_counter,
            1);

  // Add in Push Solicited Data metadata to the connection state.
  auto packet_metadata = std::make_unique<PacketMetadata>();
  packet_metadata->psn = 0;
  packet_metadata->type = falcon::PacketType::kPushSolicitedData;
  packet_metadata->direction = PacketDirection::kOutgoing;
  packet_metadata->active_packet = std::make_unique<Packet>();
  packet_metadata->active_packet->falcon.rsn = 0;
  packet_metadata->active_packet->metadata.flow_label = kFlowLabel;

  CHECK_OK_THEN_ASSIGN(TransactionMetadata* const transaction,
                       connection_state->GetTransaction(
                           TransactionKey(0, TransactionLocation::kInitiator)));

  transaction->packets[falcon::PacketType::kPushSolicitedData] =
      std::move(packet_metadata);
  transaction->state = TransactionState::kPushSolicitedDataNtwkTx;

  AddOutstandingPacket(
      /*connection_state=*/connection_state,
      /*psn=*/0,
      /*rsn=*/0,
      /*packet_type=*/falcon::PacketType::kPushSolicitedData,
      /*scid=*/connection_state->connection_metadata->scid,
      /*flow_label=*/kFlowLabel);

  // Create a fake ACK packet which ACK the data.
  ack_packet = std::make_unique<Packet>();
  ack_packet->packet_type = falcon::PacketType::kAck;
  ack_packet->ack.ack_type = Packet::Ack::kEack;
  ack_packet->ack.dest_cid = 1;
  ack_packet->ack.rrbpsn = 0;
  ack_packet->ack.rdbpsn = 0;
  auto ack_data_bitmap = FalconAckPacketBitmap(kRxDataWindowSize);
  ack_data_bitmap.Set(0, true);
  ack_packet->ack.receiver_data_bitmap = ack_data_bitmap;

  falcon_->TransferRxPacket(std::move(ack_packet));

  // ORRC = 0 as the retransmitted packet has been ACKed.
  EXPECT_EQ(connection_state->tx_reliability_metadata.request_window_metadata
                .outstanding_retransmission_requests_counter,
            0);
}

TEST_P(ProtocolPacketReliabilityManagerTest, OwnRecencyCheckBypassTest) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
  config.set_inter_host_rx_scheduling_tick_ns(0);
  auto early_retx = config.mutable_early_retx();
  early_retx->set_ooo_count_threshold(3);
  early_retx->set_enable_ooo_count(false);
  early_retx->set_enable_ooo_distance(true);
  // Enable EACK-OWN and the recency check bypass flag;
  early_retx->set_enable_eack_own(true);
  auto eack_own_config = early_retx->mutable_eack_own_metadata();
  eack_own_config->set_enable_recency_check_bypass(true);
  eack_own_config->set_enable_scanning_exit_criteria_bypass(false);
  eack_own_config->set_enable_scanning_exit_criteria_bypass(false);

  InitFalcon(config);
  int n_packet = 2;
  // Initialize the corresponding transactions, and send the packets out every
  // 10us.
  std::vector<Packet*> packet;
  ConnectionState* connection_state;
  for (int i = 0; i < n_packet; i++) {
    packet.emplace_back();
    std::tie(packet[i], connection_state) =
        FalconTestingHelpers::SetupTransaction(
            falcon_.get(), TransactionType::kPushUnsolicited,
            TransactionLocation::kInitiator,
            TransactionState::kPushUnsolicitedReqUlpRx,
            falcon::PacketType::kPushUnsolicitedData, 1, i, i);

    // Ensures the packet has its connection ID set up appropriately.
    packet[i]->metadata.scid = 1;
    // Transmit the packet at 10us intervals.
    EXPECT_OK(env_.ScheduleEvent(absl::Microseconds(i * 10), [i, this]() {
      EXPECT_OK(reliability_manager_->TransmitPacket(
          1, i, falcon::PacketType::kPushUnsolicitedData));
    }));
  }

  // Run the simulation to allow the packets to be sent out.
  env_.RunFor(absl::Microseconds((n_packet - 1) * 10));

  // Check that the packets are in the outstanding list.
  auto& metadata =
      connection_state->tx_reliability_metadata.data_window_metadata;
  CheckOutstandingPacketList(metadata.outstanding_packets, {0, 1});

  // At 20us, an E-ACK OWN with received-bitmap 0b0000000000. Note that in
  // actual scenarios an EACK-OWN can be achieved by sending out >128 data
  // packets.  Set T4=20us, T1=0, so instant_rtt=20. Thus, 2nd packet does not
  // pass the recency check as it its transmission time (10us) is less than the
  // instant_rtt.
  auto eack = FalconTestingHelpers::CreateEackPacket(
      /*dest_cid=*/1, /*rdbpsn=*/0, /*rrbpsn=*/0, /*t4=*/absl::Microseconds(20),
      /*t1=*/absl::ZeroDuration(), /*data_own=*/true);

  // Schedule the ACK to be processed at 20us.
  EXPECT_OK(env_.ScheduleEvent(
      absl::Microseconds(20) - env_.ElapsedTime(),
      [&]() { EXPECT_OK(reliability_manager_->ReceivePacket(eack.get())); }));

  // Run until all the retransmissions can be sent out -- they are set out based
  // on the max(configured falcon tick time, per_connection_inter_op_gap).
  env_.RunFor(absl::Microseconds(10) + (n_packet * kPerConnectionInterOpGap));
  // All packets should have been retransmitted with a gap of the scheduler
  // tick. The 2nd packet is retransmitted inspite of not meeting the recency
  // check as its allowed to bypass once in an RTO period.
  for (int i = 0; i < n_packet; i++) {
    // Get a handle on packet to verify retransmit count.
    auto packet_metadata =
        connection_state
            ->GetTransaction(TransactionKey(i, TransactionLocation::kInitiator))
            .value()
            ->GetPacketMetadata(falcon::PacketType::kPushUnsolicitedData);
    EXPECT_EQ(packet_metadata.value()->transmission_time,
              absl::Microseconds(20) + (i * kPerConnectionInterOpGap));
  }

  // At 25us, an E-ACK OWN is again received. Set T4=25us, T1=0, so
  // instant_rtt=25.
  eack = FalconTestingHelpers::CreateEackPacket(
      /*dest_cid=*/1, /*rdbpsn=*/0, /*rrbpsn=*/0, /*t4=*/absl::Microseconds(25),
      /*t1=*/absl::ZeroDuration(), /*data_own=*/true);

  EXPECT_OK(env_.ScheduleEvent(absl::Microseconds(4), [&]() {
    EXPECT_OK(reliability_manager_->ReceivePacket(eack.get()));
  }));
  env_.RunFor(absl::Microseconds(25) - env_.ElapsedTime());

  // None of the packets should be retransmitted again as they have used their
  // recency bypass, and hence their transmission time should still be the same
  // as during the first time.
  env_.RunFor(absl::Microseconds(10));
  for (int i = 0; i < n_packet; i++) {
    // Get a handle on packet to verify retransmit count.
    auto packet_metadata =
        connection_state
            ->GetTransaction(TransactionKey(i, TransactionLocation::kInitiator))
            .value()
            ->GetPacketMetadata(falcon::PacketType::kPushUnsolicitedData);
    EXPECT_EQ(packet_metadata.value()->transmission_time,
              absl::Microseconds(20) + (i * kPerConnectionInterOpGap));
  }
}

TEST_P(ProtocolPacketReliabilityManagerTest, OwnScanExitCriteriaBypassTest) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
  config.set_inter_host_rx_scheduling_tick_ns(0);
  auto early_retx = config.mutable_early_retx();
  early_retx->set_ooo_distance_threshold(3);
  early_retx->set_enable_ooo_count(false);
  early_retx->set_enable_ooo_distance(true);

  // Enable EACK-OWN and the scan exit bypass flag along with recency check
  // bypass.
  early_retx->set_enable_eack_own(true);
  auto eack_own_config = early_retx->mutable_eack_own_metadata();
  eack_own_config->set_enable_recency_check_bypass(true);
  eack_own_config->set_enable_scanning_exit_criteria_bypass(true);
  eack_own_config->set_enable_smaller_psn_recency_check_bypass(false);

  // Set CWND=130 (greater than 128 to cause OOW drops).
  auto rue_config = config.mutable_rue();
  rue_config->set_initial_fcwnd(130);
  rue_config->set_initial_ncwnd(130);

  InitFalcon(config);
  int n_packet = 130;
  // Initialize the corresponding transactions, and send out the packets every
  // 50ns.
  std::vector<Packet*> packet;
  ConnectionState* connection_state;
  std::vector<uint32_t> expected_oustanding_psns(n_packet);
  for (int i = 0; i < n_packet; i++) {
    packet.emplace_back();
    std::tie(packet[i], connection_state) =
        FalconTestingHelpers::SetupTransaction(
            falcon_.get(), TransactionType::kPushUnsolicited,
            TransactionLocation::kInitiator,
            TransactionState::kPushUnsolicitedReqUlpRx,
            falcon::PacketType::kPushUnsolicitedData, 1, i, i);

    // Ensures the packet has its connection ID set up appropriately.
    packet[i]->metadata.scid = 1;
    expected_oustanding_psns[i] = i;
    // Transmit the packet at 50ns intervals.
    EXPECT_OK(env_.ScheduleEvent(absl::Microseconds(i * 0.05), [i, this]() {
      EXPECT_OK(reliability_manager_->TransmitPacket(
          1, i, falcon::PacketType::kPushUnsolicitedData));
    }));
  }

  // Run the simulation to allow the packets to be sent out.
  env_.RunFor(absl::Microseconds((n_packet - 1) * 0.05));

  // Check that the packets are in the outstanding list.
  auto& metadata =
      connection_state->tx_reliability_metadata.data_window_metadata;

  CheckOutstandingPacketList(metadata.outstanding_packets,
                             expected_oustanding_psns);

  // At 20us, an EACK with received bitmap indicating PSN=3 is received. Also,
  // set T4=20us, T1=2, so instant_rtt=18.
  auto eack = FalconTestingHelpers::CreateEackPacket(
      /*dest_cid=*/1, /*rdbpsn=*/0, /*rrbpsn=*/0, /*t4=*/absl::Microseconds(20),
      /*t1=*/absl::Microseconds(2));
  eack->ack.received_bitmap.Set(3, true);

  // Schedule the EACK to be processed at 20us.
  EXPECT_OK(env_.ScheduleEvent(
      absl::Microseconds(20) - env_.ElapsedTime(),
      [&]() { EXPECT_OK(reliability_manager_->ReceivePacket(eack.get())); }));
  env_.RunFor(absl::Microseconds(20) - env_.ElapsedTime());

  // Run until PSN=0 can be sent out as it meets the ooo-threshold and recency
  // check passes.
  const auto scheduler_tick_time_us = config.falcon_tick_time_ns() / 1e3;
  env_.RunFor(absl::Microseconds(scheduler_tick_time_us));
  auto packet_0_metadata =
      connection_state
          ->GetTransaction(TransactionKey(0, TransactionLocation::kInitiator))
          .value()
          ->GetPacketMetadata(falcon::PacketType::kPushUnsolicitedData);
  EXPECT_EQ(packet_0_metadata.value()->transmission_time,
            absl::Microseconds(20));

  // At 25us, an EACK-OWN with received bitmap indicating PSN=3 is received.
  // Also, set T4=25us, T1=2, so instant_rtt=23.
  eack = FalconTestingHelpers::CreateEackPacket(
      /*dest_cid=*/1, /*rdbpsn=*/0, /*rrbpsn=*/0, /*t4=*/absl::Microseconds(25),
      /*t1=*/absl::Microseconds(2), /*data_own=*/true);
  eack->ack.received_bitmap.Set(3, true);

  // Schedule the EACK to be processed at 25us.
  EXPECT_OK(env_.ScheduleEvent(
      absl::Microseconds(25) - env_.ElapsedTime(),
      [&]() { EXPECT_OK(reliability_manager_->ReceivePacket(eack.get())); }));
  env_.RunFor(absl::Microseconds(25) - env_.ElapsedTime());
  env_.RunFor(absl::Microseconds(25) + (n_packet * kPerConnectionInterOpGap));

  // Check that packet with PSN = 0 is not retransmitted as it was retransmitted
  // less than an RTT ago (20us).
  EXPECT_EQ(packet_0_metadata.value()->transmission_time,
            absl::Microseconds(20));

  // However, because the scanning exit bypass is turned on, even though PSN=0
  // does not meet the retransmission criteria, scanning continues and all
  // packets should have been retransmitted with a gap of the scheduler (leaving
  // PSN=3 as it has been received).
  auto inter_packet_gap = absl::ZeroDuration();
  for (int i = 1; i < n_packet; i++) {
    // Get a handle on packet to verify retransmit count.
    auto packet_metadata =
        connection_state
            ->GetTransaction(TransactionKey(i, TransactionLocation::kInitiator))
            .value()
            ->GetPacketMetadata(falcon::PacketType::kPushUnsolicitedData);
    if (i == 3) {
      // Check PSN=3 is not retransmitted again.
      EXPECT_EQ(packet_metadata.value()->transmission_time,
                absl::Microseconds(i * 0.05));
    } else {
      // Check that all other packets are retransmitted at the appropriate time.
      EXPECT_EQ(packet_metadata.value()->transmission_time,
                absl::Microseconds(25) + inter_packet_gap);
      inter_packet_gap += kPerConnectionInterOpGap;
    }
  }
}

TEST_P(ProtocolPacketReliabilityManagerTest, SmallerPsnRecencyCheckBypassTest) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
  config.set_inter_host_rx_scheduling_tick_ns(0);
  auto early_retx = config.mutable_early_retx();
  early_retx->set_ooo_distance_threshold(3);
  early_retx->set_enable_ooo_count(false);
  early_retx->set_enable_ooo_distance(true);

  // Enable EACK-OWN and the smaller PSN recency check bypass.
  early_retx->set_enable_eack_own(true);
  auto eack_own_config = early_retx->mutable_eack_own_metadata();
  eack_own_config->set_enable_recency_check_bypass(false);
  eack_own_config->set_enable_scanning_exit_criteria_bypass(false);
  eack_own_config->set_enable_smaller_psn_recency_check_bypass(true);

  // Set CWND=130 (greater than 128 to cause OOW drops).
  auto rue_config = config.mutable_rue();
  rue_config->set_initial_fcwnd(200);
  rue_config->set_initial_ncwnd(200);

  InitFalcon(config);
  int n_packet = 200;
  // Initialize the corresponding transactions, and send out the packets every
  // 50ns.
  std::vector<Packet*> packet;
  ConnectionState* connection_state;
  std::vector<uint32_t> expected_oustanding_psns(n_packet);
  for (int i = 0; i < n_packet; i++) {
    packet.emplace_back();
    std::tie(packet[i], connection_state) =
        FalconTestingHelpers::SetupTransaction(
            falcon_.get(), TransactionType::kPushUnsolicited,
            TransactionLocation::kInitiator,
            TransactionState::kPushUnsolicitedReqUlpRx,
            falcon::PacketType::kPushUnsolicitedData, 1, i, i);

    // Ensures the packet has its connection ID set up appropriately.
    packet[i]->metadata.scid = 1;
    expected_oustanding_psns[i] = i;
    // Transmit the packet at 50ns intervals.
    EXPECT_OK(env_.ScheduleEvent(absl::Microseconds(i * 0.05), [i, this]() {
      EXPECT_OK(reliability_manager_->TransmitPacket(
          1, i, falcon::PacketType::kPushUnsolicitedData));
    }));
  }

  // Run the simulation to allow the packets to be sent out.
  env_.RunFor(absl::Microseconds((n_packet - 1) * 0.05));

  // Check that the packets are in the outstanding list.
  auto& metadata =
      connection_state->tx_reliability_metadata.data_window_metadata;

  CheckOutstandingPacketList(metadata.outstanding_packets,
                             expected_oustanding_psns);

  // At 11us, an EACK-OWN with received bitmap indicating PSN=3 is received.
  // Also, set T4=11us, T1=2.5us, so instant_rtt=8.5.
  auto eack = FalconTestingHelpers::CreateEackPacket(
      /*dest_cid=*/1, /*rdbpsn=*/0, /*rrbpsn=*/0, /*t4=*/absl::Microseconds(11),
      /*t1=*/absl::Microseconds(2.5), /*data_own=*/true);
  eack->ack.received_bitmap.Set(3, true);

  // Schedule the EACK to be processed at 25us.
  EXPECT_OK(env_.ScheduleEvent(
      absl::Microseconds(11) - env_.ElapsedTime(),
      [&]() { EXPECT_OK(reliability_manager_->ReceivePacket(eack.get())); }));
  env_.RunFor(absl::Microseconds(11) - env_.ElapsedTime());
  // Run until all the retransmissions can be sent out -- they are set out based
  // on the configured falcon tick time.
  env_.RunFor(absl::Microseconds(11) + (n_packet * kPerConnectionInterOpGap));

  // All packets (leaving PSN=3 which is received) should have been
  // retransmitted with a gap of the scheduler as PSN=0 meets the recency check
  // and thus all other packet's recency check is assumed to be passed.
  auto inter_packet_gap = absl::ZeroDuration();
  for (int i = 0; i < n_packet; i++) {
    // Get a handle on packet to verify retransmit count.
    auto packet_metadata =
        connection_state
            ->GetTransaction(TransactionKey(i, TransactionLocation::kInitiator))
            .value()
            ->GetPacketMetadata(falcon::PacketType::kPushUnsolicitedData);
    if (i == 3) {
      // Check PSN=3 is not retransmitted again.
      EXPECT_EQ(packet_metadata.value()->transmission_time,
                absl::Microseconds(i * 0.05));
    } else {
      // Check that all other packets are retransmitted at the appropriate time.
      EXPECT_EQ(packet_metadata.value()->transmission_time,
                absl::Microseconds(11) + inter_packet_gap);
      inter_packet_gap += kPerConnectionInterOpGap;
    }
  }
}

TEST_P(ProtocolPacketReliabilityManagerTest,
       PauseInitTransmissionOnOowDropTest) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
  config.set_inter_host_rx_scheduling_tick_ns(0);
  auto early_retx = config.mutable_early_retx();
  early_retx->set_ooo_distance_threshold(3);
  early_retx->set_enable_ooo_count(false);
  early_retx->set_enable_ooo_distance(true);

  // Enable EACK and pausing initial transmissions on OOW drops.
  early_retx->set_enable_eack_own(true);
  auto eack_own_config = early_retx->mutable_eack_own_metadata();
  eack_own_config->set_enable_recency_check_bypass(false);
  eack_own_config->set_enable_scanning_exit_criteria_bypass(false);
  eack_own_config->set_enable_smaller_psn_recency_check_bypass(false);
  eack_own_config->set_enable_pause_initial_transmission_on_oow_drops(true);

  InitFalcon(config);
  // Setup the required metadata for the connection and send configured packets.
  uint32_t n_packet = 200;
  ConnectionState* connection_state;
  Packet* packet;
  for (int i = 0; i < n_packet; i++) {
    std::tie(packet, connection_state) = FalconTestingHelpers::SetupTransaction(
        falcon_.get(), TransactionType::kPushUnsolicited,
        TransactionLocation::kInitiator,
        TransactionState::kPushUnsolicitedReqUlpRx,
        falcon::PacketType::kPushUnsolicitedData, 1, i, i);

    // Ensures the packet has its connection ID set up appropriately.
    packet->metadata.scid = 1;
    EXPECT_OK(reliability_manager_->TransmitPacket(
        1, i, falcon::PacketType::kPushUnsolicitedData));
  }

  // Meets the criteria as no EACK-OWN is received to influence gating.
  EXPECT_TRUE(reliability_manager_->MeetsInitialTransmissionOowTxGatingCriteria(
      1, falcon::PacketType::kPushUnsolicitedData));

  // Set the next packet sequence number on the data window to be 400.
  connection_state->tx_reliability_metadata.data_window_metadata
      .next_packet_sequence_number = 200;

  // Process EACK-OWN with BPSN=100.
  auto eack = FalconTestingHelpers::CreateEackPacket(
      /*dest_cid=*/1, /*rdbpsn=*/100, /*rrbpsn=*/0,
      /*t4=*/absl::Microseconds(20),
      /*t1=*/absl::ZeroDuration(), /*data_own=*/true);
  EXPECT_OK(reliability_manager_->ReceivePacket(eack.get()));

  // The gating criteria should fail as the difference between NPSN (200) and
  // BPSN (100) in the EACK-OWN > slack (64).
  EXPECT_FALSE(
      reliability_manager_->MeetsInitialTransmissionOowTxGatingCriteria(
          1, falcon::PacketType::kPushUnsolicitedData));

  // Process another EACK with BPSN=150.
  auto another_eack = FalconTestingHelpers::CreateEackPacket(
      /*dest_cid=*/1, /*rdbpsn=*/150, /*rrbpsn=*/0,
      /*t4=*/absl::Microseconds(20),
      /*t1=*/absl::ZeroDuration());
  EXPECT_OK(reliability_manager_->ReceivePacket(another_eack.get()));

  // The gating criteria passes as the difference between NPSN (200) and
  // BPSN (150) in the EACK > slack (64).
  EXPECT_TRUE(reliability_manager_->MeetsInitialTransmissionOowTxGatingCriteria(
      1, falcon::PacketType::kPushUnsolicitedData));
}

TEST_P(ProtocolPacketReliabilityManagerTest, UnorderedConnection) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
  config.set_inter_host_rx_scheduling_tick_ns(0);
  auto early_retx = config.mutable_early_retx();
  early_retx->set_ooo_distance_threshold(3);
  early_retx->set_enable_ooo_count(false);
  early_retx->set_enable_ooo_distance(true);
  InitFalcon(config);

  // Steps to initialize a transaction and get handle on the packet and
  // connection state.
  std::vector<Packet*> packet;
  ConnectionState* connection_state;
  for (int i = 0; i < 2; i++) {
    packet.emplace_back();
    std::tie(packet[i], connection_state) =
        FalconTestingHelpers::SetupTransaction(
            falcon_.get(), TransactionType::kPushUnsolicited,
            TransactionLocation::kInitiator,
            TransactionState::kPushUnsolicitedReqUlpRx,
            falcon::PacketType::kPushUnsolicitedData, 1, i, i,
            OrderingMode::kUnordered);

    // Ensures the packet has its connection ID set up appropriately.
    packet[i]->metadata.scid = 1;
    // Transmit the packet
    EXPECT_OK(reliability_manager_->TransmitPacket(
        1, i, falcon::PacketType::kPushUnsolicitedData));
  }
  for (int i = 2; i < 4; i++) {
    packet.emplace_back();
    std::tie(packet[i], connection_state) =
        FalconTestingHelpers::SetupTransaction(
            falcon_.get(), TransactionType::kPull,
            TransactionLocation::kInitiator, TransactionState::kPullReqNtwkTx,
            falcon::PacketType::kPullRequest, 1, i, 0,
            OrderingMode::kUnordered);
  }
  // Set to unordered connection.
  connection_state->tx_reliability_metadata.request_window_metadata
      .base_packet_sequence_number = 1;

  auto eack = std::make_unique<Packet>();
  eack->packet_type = falcon::PacketType::kAck;
  eack->ack.ack_type = Packet::Ack::kEack;
  eack->ack.dest_cid = 1;
  eack->ack.rdbpsn = 0;
  eack->ack.rrbpsn = 1;
  eack->timestamps.received_timestamp = absl::Microseconds(50);
  eack->ack.timestamp_1 = absl::ZeroDuration();
  for (auto psn : {1}) {
    eack->ack.receiver_data_bitmap.Set(psn, true);
    eack->ack.received_bitmap.Set(psn, true);
  }
  EXPECT_OK(env_.ScheduleEvent(absl::Microseconds(100), [&]() {
    falcon_->TransferRxPacket(std::move(eack));
  }));
  EXPECT_CALL(rdma_, HandleCompletion).Times(1);

  auto pulldata = std::make_unique<Packet>();
  pulldata->packet_type = falcon::PacketType::kPullData;
  pulldata->falcon.rsn = 3;
  pulldata->falcon.psn = 1;
  pulldata->falcon.dest_cid = 1;
  pulldata->falcon.payload_length = 1;
  EXPECT_OK(env_.ScheduleEvent(absl::Microseconds(100), [&]() {
    falcon_->TransferRxPacket(std::move(pulldata));
  }));
  EXPECT_CALL(rdma_, HandleRxTransaction).Times(1);
  env_.RunFor(absl::Microseconds(100));
}

}  // namespace
}  // namespace isekai
