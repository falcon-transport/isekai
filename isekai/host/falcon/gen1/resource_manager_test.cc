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

#include "isekai/host/falcon/gen1/resource_manager.h"

#include <cstdint>
#include <memory>
#include <string>
#include <tuple>
#include <utility>

#include "absl/container/flat_hash_map.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/substitute.h"
#include "gmock/gmock.h"
#include "google/protobuf/text_format.h"
#include "gtest/gtest.h"
#include "isekai/common/common_util.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/default_config_generator.h"
#include "isekai/common/packet.h"
#include "isekai/common/simple_environment.h"
#include "isekai/common/testing.h"
#include "isekai/fabric/constants.h"
#include "isekai/host/falcon/connection_state_utils.h"
#include "isekai/host/falcon/falcon.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/falcon_connection_state.h"
#include "isekai/host/falcon/falcon_resource_credits.h"
#include "isekai/host/falcon/falcon_testing_helpers.h"
#include "isekai/host/falcon/gen1/falcon_model.h"
#include "isekai/host/falcon/gen1/falcon_types.h"
#include "isekai/host/falcon/gen1/falcon_utils.h"
#include "isekai/host/falcon/gen2/resource_manager.h"
#include "isekai/host/falcon/rue/fixed.h"
#include "isekai/host/rdma/rdma_falcon_model.h"
#include "isekai/host/rnic/connection_manager.h"

namespace isekai {
namespace {

// This defines all the objects needed for setup and testing. The test fixture
// is parameterized on the Falcon version.
class FalconResourceManagerTest
    : public FalconTestingHelpers::FalconTestSetup,
      public ::testing::TestWithParam<int /* version */> {
 protected:
  int GetFalconVersion() { return GetParam(); }

  FalconResourceCredits&
  ResourceManagerGetAvailableResourceCreditsForTesting() {
    return dynamic_cast<ProtocolResourceManager*>(resource_manager_)
        ->GetAvailableResourceCreditsForTesting();
  }

  bool ResourceManagerGetRequestXoffForTesting() {
    return dynamic_cast<ProtocolResourceManager*>(resource_manager_)
        ->GetRequestXoffForTesting();
  }

  bool ResourceManagerGetGlobalXoffForTesting() {
    return dynamic_cast<ProtocolResourceManager*>(resource_manager_)
        ->GetGlobalXoffForTesting();
  }

  NetworkRegionEmaOccupancy
  ResourceManagerUpdateNetworkRegionEmaOccupancyForTesting() {
    return dynamic_cast<ProtocolResourceManager*>(resource_manager_)
        ->UpdateNetworkRegionEmaOccupancyForTesting();
  }
};

INSTANTIATE_TEST_SUITE_P(ResourceManagerTest, FalconResourceManagerTest,
                         /*version=*/testing::Values(1, 2),
                         [](const testing::TestParamInfo<
                             FalconResourceManagerTest::ParamType>& info) {
                           const int version = static_cast<int>(info.param);
                           return absl::StrCat("Gen", version);
                         });

// Converts EMA occupancies from fixed-point representation to floating-point.
std::tuple<double, double, double> GetFloatNetworkRegionRMAOccupancyFromFixed(
    NetworkRegionEmaOccupancy ema_occupancies) {
  auto float_rx_buffer_pool_occupancy =
      falcon_rue::FixedToFloat<uint32_t, double>(
          ema_occupancies.rx_buffer_pool_occupancy_ema,
          ProtocolResourceManager::kEmaOccupancyFractionalBits);
  auto float_rx_packet_pool_occupancy =
      falcon_rue::FixedToFloat<uint32_t, double>(
          ema_occupancies.rx_packet_pool_occupancy_ema,
          ProtocolResourceManager::kEmaOccupancyFractionalBits);
  auto float_tx_packet_pool_occupancy =
      falcon_rue::FixedToFloat<uint32_t, double>(
          ema_occupancies.tx_packet_pool_occupancy_ema,
          ProtocolResourceManager::kEmaOccupancyFractionalBits);

  return std::tuple<double, double, double>(float_rx_buffer_pool_occupancy,
                                            float_rx_packet_pool_occupancy,
                                            float_tx_packet_pool_occupancy);
}

std::unique_ptr<TransactionMetadata> MakePullRequestTransaction(
    uint8_t falcon_version, uint32_t rsn) {
  // Initializes metadata corresponding to a kPullRequest packet.
  auto packet_metadata = std::make_unique<PacketMetadata>();
  packet_metadata->psn = 1;
  packet_metadata->type = falcon::PacketType::kPullRequest;
  packet_metadata->direction = PacketDirection::kOutgoing;
  packet_metadata->active_packet = std::make_unique<Packet>();
  packet_metadata->active_packet->rdma.request_length = 1;
  packet_metadata->active_packet->rdma.opcode =
      Packet::Rdma::Opcode::kReadRequest;

  // Initializes metadata corresponding to the transaction.
  auto transaction =
      CreateTransactionMetadata(falcon_version, rsn, TransactionType::kPull,
                                TransactionLocation::kInitiator);
  transaction->packets[packet_metadata->type] = std::move(packet_metadata);

  return transaction;
}

TEST_P(FalconResourceManagerTest, ReserveReleaseOutgoingPullRequest) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
  config.set_resource_reservation_mode(FalconConfig::FIRST_PHASE_RESERVATION);

  // Initial FALCON resource credits.
  const std::string kConfigCredits =
      R"pb(
    tx_packet_credits { ulp_requests: 1 ulp_data: 1 network_requests: 1 }
    tx_buffer_credits { ulp_requests: 1 ulp_data: 1 network_requests: 1 }
    rx_packet_credits { ulp_requests: 1 network_requests: 1 }
    rx_buffer_credits { ulp_requests: 1 network_requests: 1 })pb";
  ASSERT_TRUE(google::protobuf::TextFormat::ParseFromString(
      kConfigCredits, config.mutable_resource_credits()));

  // Expected FALCON credits for the transaction below.
  FalconResourceCredits expected_credits = {
      .tx_packet_credits =
          {
              .ulp_requests = 0,
              .ulp_data = 1,
              .network_requests = 1,
          },
      .tx_buffer_credits =
          {
              .ulp_requests = 1,
              .ulp_data = 1,
              .network_requests = 1,
          },
      .rx_packet_credits =
          {
              .ulp_requests = 0,
              .network_requests = 1,
          },
      .rx_buffer_credits =
          {
              .ulp_requests = 0,
              .network_requests = 1,
          },
  };

  // Set the parametrized test values in FalconConfig and initialize Falcon test
  // setup.
  InitFalcon(config);

  uint32_t source_connection_id = 1;
  auto connection_metadata = FalconTestingHelpers::InitializeConnectionMetadata(
      falcon_.get(), source_connection_id);
  EXPECT_OK(connection_state_manager_->InitializeConnectionState(
      std::move(connection_metadata)));

  // Initializes a pull request transaction
  auto transaction = MakePullRequestTransaction(GetFalconVersion(), 1);
  auto packet = transaction->GetPacketMetadata(falcon::PacketType::kPullRequest)
                    .value()
                    ->active_packet.get();

  // Add the above metadata to the appropriate connection state.
  absl::StatusOr<ConnectionState*> connection_state =
      connection_state_manager_->PerformDirectLookup(source_connection_id);
  EXPECT_OK(connection_state);
  connection_state.value()
      ->transactions[{transaction->rsn, TransactionLocation::kInitiator}] =
      std::move(transaction);

  // Reserve resources and check if the remaining resources are per expectation.
  EXPECT_OK(resource_manager_->VerifyResourceAvailabilityOrReserveResources(
      1, packet, PacketDirection::kOutgoing, true));
  EXPECT_EQ(expected_credits,
            ResourceManagerGetAvailableResourceCreditsForTesting());

  // Release resources and check if the available resources are per expectation.
  EXPECT_OK(resource_manager_->ReleaseResources(
      1, {1, TransactionLocation::kInitiator},
      falcon::PacketType::kPullRequest));

  // Expected resources once resources correponding to pull request are
  // released.
  expected_credits = {
      .tx_packet_credits =
          {
              .ulp_requests = 1,
              .ulp_data = 1,
              .network_requests = 1,
          },
      .tx_buffer_credits =
          {
              .ulp_requests = 1,
              .ulp_data = 1,
              .network_requests = 1,
          },
      .rx_packet_credits =
          {
              .ulp_requests = 0,
              .network_requests = 1,
          },
      .rx_buffer_credits =
          {
              .ulp_requests = 0,
              .network_requests = 1,
          },
  };

  EXPECT_EQ(ResourceManagerGetAvailableResourceCreditsForTesting(),
            expected_credits);

  // Release resources again on the same transaction and verify the outcome.
  EXPECT_EQ(resource_manager_
                ->ReleaseResources(1, {1, TransactionLocation::kInitiator},
                                   falcon::PacketType::kPullRequest)
                .code(),
            absl::StatusCode::kResourceExhausted);
}

TEST_P(FalconResourceManagerTest, RequestXoffTest) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
  config.set_resource_reservation_mode(FalconConfig::FIRST_PHASE_RESERVATION);

  // Initial FALCON resource credits.
  const std::string kConfigCredits =
      R"pb(
    tx_packet_credits { ulp_requests: 1 ulp_data: 1 network_requests: 1 }
    tx_buffer_credits { ulp_requests: 1 ulp_data: 1 network_requests: 1 }
    rx_packet_credits { ulp_requests: 1 network_requests: 1 }
    rx_buffer_credits { ulp_requests: 1 network_requests: 1 })pb";
  const std::string kXoffThresholds =
      R"pb(
    tx_packet_request: 1
    tx_buffer_request: 1
    tx_packet_data: 0
    tx_buffer_data: 0
    rx_packet_request: 0
    rx_buffer_request: 0
      )pb";
  ASSERT_TRUE(google::protobuf::TextFormat::ParseFromString(
      kConfigCredits, config.mutable_resource_credits()));
  ASSERT_TRUE(google::protobuf::TextFormat::ParseFromString(
      kXoffThresholds, config.mutable_ulp_xoff_thresholds()));

  // Set the parametrized test values in FalconConfig and initialize Falcon test
  // setup.
  InitFalcon(config);

  uint32_t source_connection_id = 1;
  auto connection_metadata = FalconTestingHelpers::InitializeConnectionMetadata(
      falcon_.get(), source_connection_id);
  EXPECT_OK(connection_state_manager_->InitializeConnectionState(
      std::move(connection_metadata)));

  // Initializes a pull request transaction
  auto transaction = MakePullRequestTransaction(GetFalconVersion(), 1);
  auto packet = transaction->GetPacketMetadata(falcon::PacketType::kPullRequest)
                    .value()
                    ->active_packet.get();

  // Add the above metadata to the appropriate connection state.
  absl::StatusOr<ConnectionState*> connection_state =
      connection_state_manager_->PerformDirectLookup(source_connection_id);
  EXPECT_OK(connection_state);
  connection_state.value()
      ->transactions[{transaction->rsn, TransactionLocation::kInitiator}] =
      std::move(transaction);

  // Reserve resources and check if the remaining resources are per expectation.
  EXPECT_OK(resource_manager_->VerifyResourceAvailabilityOrReserveResources(
      1, packet, PacketDirection::kOutgoing, true));
  EXPECT_TRUE(ResourceManagerGetRequestXoffForTesting());

  // Release resources and check if the available resources are per expectation.
  EXPECT_OK(resource_manager_->ReleaseResources(
      1, {1, TransactionLocation::kInitiator},
      falcon::PacketType::kPullRequest));
  EXPECT_FALSE(ResourceManagerGetRequestXoffForTesting());
}

TEST_P(FalconResourceManagerTest, GlobalXoffTest) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
  config.set_resource_reservation_mode(FalconConfig::FIRST_PHASE_RESERVATION);

  // Initial FALCON resource credits.
  const std::string kConfigCredits =
      R"pb(
    tx_packet_credits { ulp_requests: 1 ulp_data: 1 network_requests: 1 }
    tx_buffer_credits { ulp_requests: 1 ulp_data: 1 network_requests: 1 }
    rx_packet_credits { ulp_requests: 1 network_requests: 1 }
    rx_buffer_credits { ulp_requests: 1 network_requests: 1 })pb";
  const std::string kGlobalXoffThresholds =
      R"pb(
    tx_packet_request: 1
    tx_buffer_request: 1
    tx_packet_data: 1
    tx_buffer_data: 1
    rx_packet_request: 0
    rx_buffer_request: 0
      )pb";
  ASSERT_TRUE(google::protobuf::TextFormat::ParseFromString(
      kConfigCredits, config.mutable_resource_credits()));
  ASSERT_TRUE(google::protobuf::TextFormat::ParseFromString(
      kGlobalXoffThresholds, config.mutable_ulp_xoff_thresholds()));

  // Set the parametrized test values in FalconConfig and initialize Falcon test
  // setup.
  InitFalcon(config);

  uint32_t source_connection_id = 1;
  auto connection_metadata = FalconTestingHelpers::InitializeConnectionMetadata(
      falcon_.get(), source_connection_id);
  EXPECT_OK(connection_state_manager_->InitializeConnectionState(
      std::move(connection_metadata)));

  // Initializes a pull response transaction.
  Packet* packet;
  ConnectionState* connection_state;
  std::tie(packet, connection_state) = FalconTestingHelpers::SetupTransaction(
      falcon_.get(), TransactionType::kPull, TransactionLocation::kTarget,
      TransactionState::kPullDataUlpRx, falcon::PacketType::kPullData,
      /*scid=*/1,
      /*rsn=*/1, /*psn=*/1);
  packet->rdma.opcode = Packet::Rdma::Opcode::kReadResponseOnly;
  // Setup the received packet context appropriately.
  const TransactionKey transaction_key(1, TransactionLocation::kTarget);
  connection_state->rx_reliability_metadata
      .received_packet_contexts[transaction_key] =
      std::make_unique<ReceivedPacketContext>();
  connection_state->rx_reliability_metadata
      .received_packet_contexts[transaction_key]
      ->qp_id = 1;

  // Reserve resources and check if the remaining resources are per expectation.
  EXPECT_OK(resource_manager_->VerifyResourceAvailabilityOrReserveResources(
      1, packet, PacketDirection::kOutgoing, true));
  EXPECT_TRUE(ResourceManagerGetGlobalXoffForTesting());

  // Release resources and check if the available resources are per expectation.
  EXPECT_OK(resource_manager_->ReleaseResources(
      1, {1, TransactionLocation::kTarget}, falcon::PacketType::kPullData));
  EXPECT_FALSE(ResourceManagerGetGlobalXoffForTesting());
}

TEST_P(FalconResourceManagerTest, ByPassResourceReservation) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());

  // Initial FALCON resource credits.
  const std::string kConfigCredits =
      R"pb(
    tx_packet_credits { ulp_requests: 1 ulp_data: 1 network_requests: 1 }
    tx_buffer_credits { ulp_requests: 1 ulp_data: 1 network_requests: 1 }
    rx_packet_credits { ulp_requests: 1 network_requests: 1 }
    rx_buffer_credits { ulp_requests: 1 network_requests: 1 })pb";
  ASSERT_TRUE(google::protobuf::TextFormat::ParseFromString(
      kConfigCredits, config.mutable_resource_credits()));

  // Set the parametrized test values in FalconConfig and initialize Falcon test
  // setup.
  InitFalcon(config);

  uint32_t source_connection_id = 1;
  auto connection_metadata = FalconTestingHelpers::InitializeConnectionMetadata(
      falcon_.get(), source_connection_id);
  EXPECT_OK(connection_state_manager_->InitializeConnectionState(
      std::move(connection_metadata)));

  // Initializes a pull request transaction
  auto transaction = MakePullRequestTransaction(GetFalconVersion(), 1);
  auto packet = transaction->GetPacketMetadata(falcon::PacketType::kPullRequest)
                    .value()
                    ->active_packet.get();

  // Add the above metadata to the appropriate connection state.
  absl::StatusOr<ConnectionState*> connection_state =
      connection_state_manager_->PerformDirectLookup(source_connection_id);
  EXPECT_OK(connection_state);
  connection_state.value()
      ->transactions[{transaction->rsn, TransactionLocation::kInitiator}] =
      std::move(transaction);

  // Reserve resources and check if the remaining resources are per expectation.
  EXPECT_OK(resource_manager_->VerifyResourceAvailabilityOrReserveResources(
      1, packet, PacketDirection::kOutgoing, true));
  EXPECT_EQ(FalconResourceCredits::Create(config.resource_credits()),
            ResourceManagerGetAvailableResourceCreditsForTesting());

  // Release resources and check if the available resources are per expectation.
  EXPECT_OK(resource_manager_->ReleaseResources(
      1, {1, TransactionLocation::kInitiator},
      falcon::PacketType::kPullRequest));
  EXPECT_EQ(FalconResourceCredits::Create(config.resource_credits()),
            ResourceManagerGetAvailableResourceCreditsForTesting());

  // Release resources again on the same transaction and verify the outcome.
  EXPECT_EQ(resource_manager_
                ->ReleaseResources(1, {1, TransactionLocation::kInitiator},
                                   falcon::PacketType::kPullRequest)
                .code(),
            absl::StatusCode::kOk);
}

TEST_P(FalconResourceManagerTest, FalconNetworkRequestPrioritization) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
  config.set_resource_reservation_mode(FalconConfig::FIRST_PHASE_RESERVATION);

  // Initial FALCON resource credits.
  const std::string kConfigCredits =
      R"pb(
    tx_packet_credits { ulp_requests: 5 ulp_data: 5 network_requests: 5 }
    tx_buffer_credits { ulp_requests: 5 ulp_data: 5 network_requests: 5 }
    rx_packet_credits { ulp_requests: 5 network_requests: 60 }
    rx_buffer_credits { ulp_requests: 5 network_requests: 60 })pb";
  ASSERT_TRUE(google::protobuf::TextFormat::ParseFromString(
      kConfigCredits, config.mutable_resource_credits()));

  config.mutable_falcon_network_requests_rx_buffer_pool_thresholds()
      ->set_green_zone_end(25);
  config.mutable_falcon_network_requests_rx_buffer_pool_thresholds()
      ->set_yellow_zone_end(40);

  // Set the parametrized test values in FalconConfig and initialize Falcon test
  // setup.
  InitFalcon(config);

  uint32_t source_connection_id = 1;
  auto connection_metadata = FalconTestingHelpers::InitializeConnectionMetadata(
      falcon_.get(), source_connection_id);
  EXPECT_OK(connection_state_manager_->InitializeConnectionState(
      std::move(connection_metadata)));

  // Create an incoming push request corresponding to a solicited write.
  auto push_request_packet = std::make_unique<Packet>();
  push_request_packet->packet_type = falcon::PacketType::kPushRequest;
  push_request_packet->falcon.dest_cid = 1;
  push_request_packet->falcon.rsn = 1;
  push_request_packet->falcon.request_length = 25;

  // Current occupancy is green zone, so request will be admitted.
  EXPECT_OK(resource_manager_->VerifyResourceAvailabilityOrReserveResources(
      1, push_request_packet.get(), PacketDirection::kIncoming, true));

  // Change RSN to reflect non-HoL request.
  push_request_packet->falcon.rsn = 3;
  // Now occupancy is in yellow zone, so only HoL request will be admitted.
  EXPECT_EQ(
      resource_manager_
          ->VerifyResourceAvailabilityOrReserveResources(
              1, push_request_packet.get(), PacketDirection::kIncoming, true)
          .code(),
      absl::StatusCode::kResourceExhausted);

  // Change RSN to reflect HoL request.
  push_request_packet->falcon.rsn = 0;
  EXPECT_OK(resource_manager_->VerifyResourceAvailabilityOrReserveResources(
      1, push_request_packet.get(), PacketDirection::kIncoming, true));

  // Now occupancy is in red zone, so Push requests not allowed. Only Pull or
  // Unsolicited Data HoL request is allowed.
  EXPECT_EQ(
      resource_manager_
          ->VerifyResourceAvailabilityOrReserveResources(
              1, push_request_packet.get(), PacketDirection::kIncoming, true)
          .code(),
      absl::StatusCode::kResourceExhausted);

  push_request_packet->falcon.rsn = 0;
  push_request_packet->packet_type = falcon::PacketType::kPushUnsolicitedData;
  EXPECT_OK(resource_manager_->VerifyResourceAvailabilityOrReserveResources(
      1, push_request_packet.get(), PacketDirection::kIncoming, true));
}

TEST_P(FalconResourceManagerTest, NetworkRegionEMACalculationTest) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
  config.set_resource_reservation_mode(FalconConfig::FIRST_PHASE_RESERVATION);

  // Initial FALCON resource credits.
  const std::string kConfigCredits =
      R"pb(
    tx_packet_credits { ulp_requests: 5 ulp_data: 5 network_requests: 5 }
    tx_buffer_credits { ulp_requests: 5 ulp_data: 5 network_requests: 5 }
    rx_packet_credits { ulp_requests: 5 network_requests: 60 }
    rx_buffer_credits { ulp_requests: 5 network_requests: 60 })pb";
  ASSERT_TRUE(google::protobuf::TextFormat::ParseFromString(
      kConfigCredits, config.mutable_resource_credits()));

  // Initialize the EMA coefficients.
  config.mutable_ema_coefficients()->set_rx_buffer(1);
  config.mutable_ema_coefficients()->set_rx_context(1);
  config.mutable_ema_coefficients()->set_tx_context(1);

  // Change the first few values of the quantization table of the RX buffer
  // resource.
  config.mutable_quantization_tables()
      ->mutable_rx_buffer()
      ->set_quantization_level_0_threshold(15);
  config.mutable_quantization_tables()
      ->mutable_rx_buffer()
      ->set_quantization_level_1_threshold(30);
  config.mutable_quantization_tables()
      ->mutable_rx_buffer()
      ->set_quantization_level_2_threshold(45);

  // Set the parametrized test values in FalconConfig and initialize Falcon test
  // setup.
  InitFalcon(config);

  auto ema_occupancies =
      ResourceManagerUpdateNetworkRegionEmaOccupancyForTesting();
  auto [float_rx_buffer_pool_occupancy, float_rx_packet_pool_occupancy,
        float_tx_packet_pool_occupancy] =
      GetFloatNetworkRegionRMAOccupancyFromFixed(ema_occupancies);

  // With no resources used, the EMA values are expected to be 0;
  EXPECT_EQ(float_rx_buffer_pool_occupancy, 0.0);
  EXPECT_EQ(float_rx_packet_pool_occupancy, 0.0);
  EXPECT_EQ(float_tx_packet_pool_occupancy, 0.0);

  uint32_t source_connection_id = 1;
  auto connection_metadata = FalconTestingHelpers::InitializeConnectionMetadata(
      falcon_.get(), source_connection_id);
  EXPECT_OK(connection_state_manager_->InitializeConnectionState(
      std::move(connection_metadata)));

  // Create an incoming push request corresponding to a solicited write.
  auto push_request_packet = std::make_unique<Packet>();
  push_request_packet->packet_type = falcon::PacketType::kPushRequest;
  push_request_packet->falcon.dest_cid = 1;
  push_request_packet->falcon.rsn = 1;
  push_request_packet->falcon.request_length = 25;

  // This push request will end up consuming 1 RX/TX packet pool credits and 25
  // RX buffer pool credits.
  EXPECT_OK(resource_manager_->VerifyResourceAvailabilityOrReserveResources(
      1, push_request_packet.get(), PacketDirection::kIncoming, true));

  ema_occupancies = ResourceManagerUpdateNetworkRegionEmaOccupancyForTesting();
  std::tie(float_rx_buffer_pool_occupancy, float_rx_packet_pool_occupancy,
           float_tx_packet_pool_occupancy) =
      GetFloatNetworkRegionRMAOccupancyFromFixed(ema_occupancies);

  // With first time resources being used, the EMA values are expected to be
  // equal to the current used up resources;
  EXPECT_EQ(float_rx_buffer_pool_occupancy, 25.0);
  EXPECT_EQ(float_rx_packet_pool_occupancy, 1.0);
  EXPECT_EQ(float_tx_packet_pool_occupancy, 1.0);

  // This push request will end up consuming 1 RX/TX packet pool credits and 25
  // RX buffer pool credits.
  EXPECT_OK(resource_manager_->VerifyResourceAvailabilityOrReserveResources(
      1, push_request_packet.get(), PacketDirection::kIncoming, true));

  ema_occupancies = ResourceManagerUpdateNetworkRegionEmaOccupancyForTesting();
  std::tie(float_rx_buffer_pool_occupancy, float_rx_packet_pool_occupancy,
           float_tx_packet_pool_occupancy) =
      GetFloatNetworkRegionRMAOccupancyFromFixed(ema_occupancies);

  // The EMA values should give equal weight to the previous EMA value and
  // current resource occupancy as the ema_coefficients are set to 1 above.
  EXPECT_EQ(float_rx_buffer_pool_occupancy, 37.5);
  EXPECT_EQ(float_rx_packet_pool_occupancy, 1.5);
  EXPECT_EQ(float_tx_packet_pool_occupancy, 1.5);

  // We expect the occupancy to reflect the quantized value of RX-Buffer (as it
  // has the maximum quantized value);
  EXPECT_EQ(resource_manager_->GetNetworkRegionOccupancy(), 2);
}

}  // namespace
}  // namespace isekai
