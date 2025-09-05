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

#include "isekai/host/falcon/gen2/ulp_backpressure_policy_alpha_carving.h"

#include <cstdint>
#include <memory>
#include <utility>

#include "absl/status/statusor.h"
#include "gmock/gmock.h"
#include "google/protobuf/text_format.h"
#include "gtest/gtest.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/default_config_generator.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/common/packet.h"
#include "isekai/common/simple_environment.h"
#include "isekai/common/testing.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/falcon_connection_state.h"
#include "isekai/host/falcon/gen1/falcon_model.h"
#include "isekai/host/falcon/gen2/connection_state.h"
#include "isekai/host/falcon/gen2/falcon_types.h"
#include "isekai/host/rnic/connection_manager.h"
namespace isekai {
namespace {

using ::testing::NiceMock;

class MockTrafficShaper : public TrafficShaperInterface {
 public:
  MOCK_METHOD(void, ConnectPacketBuilder,
              (PacketBuilderInterface * packet_builder), (override));
  MOCK_METHOD(void, TransferTxPacket, (std::unique_ptr<Packet> packet),
              (override));
};

class MockRdma : public RdmaFalconInterface {
 public:
  MOCK_METHOD(void, HandleRxTransaction,
              (std::unique_ptr<Packet> packet,
               std::unique_ptr<OpaqueCookie> cookie),
              (override));
  MOCK_METHOD(void, HandleCompletion,
              (QpId qp_id, uint32_t rsn, Packet::Syndrome syndrome,
               uint8_t destination_bifurcation_id),
              (override));
  MOCK_METHOD(void, ReturnFalconCredit,
              (QpId qp_id, const FalconCredit& ulp_credit), (override));
  MOCK_METHOD(void, SetXoff, (bool request_xoff, bool global_xoff), (override));
  MOCK_METHOD(void, BackpressureQP,
              (QpId qp_id, BackpressureType type, BackpressureData data),
              (override));
};

TEST(UlpBackpressureAlphaCarvingTest, ComputeAlphaCarvingTest) {
  SimpleEnvironment env;
  uint32_t source_connection_id = 1;

  FalconConfig config = DefaultConfigGenerator::DefaultFalconConfig(2);
  // Explicitly setting the target buffer level to 1.
  auto swift = config.mutable_rue()->mutable_swift();
  swift->set_target_rx_buffer_level(1);
  // Explicitly setting the available credit number.
  auto falcon_credits = config.mutable_resource_credits();
  falcon_credits->mutable_tx_packet_credits()->set_ulp_requests(4);
  falcon_credits->mutable_tx_buffer_credits()->set_ulp_requests(8);
  falcon_credits->mutable_rx_packet_credits()->set_ulp_requests(12);
  falcon_credits->mutable_rx_buffer_credits()->set_ulp_requests(16);
  falcon_credits->mutable_tx_packet_credits()->set_ulp_data(20);
  falcon_credits->mutable_tx_buffer_credits()->set_ulp_data(24);

  NiceMock<MockRdma> rdma;
  NiceMock<MockTrafficShaper> shaper;
  FalconModel falcon(config, &env, /*stats_collector=*/nullptr,
                     ConnectionManager::GetConnectionManager(), "falcon-host",
                     /* number of hosts */ 4);
  falcon.ConnectShaper(&shaper);
  falcon.ConnectRdma(&rdma);

  // Initialize the connection state.
  ConnectionStateManager* const connection_state_manager =
      falcon.get_state_manager();
  std::unique_ptr<Gen2ConnectionMetadata> connection_metadata =
      std::make_unique<Gen2ConnectionMetadata>();
  connection_metadata->scid = source_connection_id;
  EXPECT_OK(connection_state_manager->InitializeConnectionState(
      std::move(connection_metadata)));

  absl::StatusOr<ConnectionState*> connection_state =
      connection_state_manager->PerformDirectLookup(/*scid*/ 1);
  EXPECT_OK(connection_state);

  auto gen2_connection_state =
      dynamic_cast<Gen2ConnectionState*>(connection_state.value());

  auto policy = std::make_unique<AlphaCarvingUlpBackpressurePolicy>(&falcon, 0);

  // Test whether alpha carving can calculate limits according to Swift's
  // response. Swift's alpha_request/alpha_response is stored in the connection
  // context.

  // Set alpha_request in the connection context to 4.
  // The recovered alpha_request should be 2 ^ (3 - 4) = 0.5.
  gen2_connection_state->connection_xoff_metadata.alpha_request = 4;

  // Set alpha_response in the connection context to 5.
  // The recovered alpha for this should be 2 ^ (3 - 5) = 0.25.
  gen2_connection_state->connection_xoff_metadata.alpha_response = 5;

  FalconCredit result;
  result = policy->ComputeAlphaCarvingForTesting(/*scid*/ 1);
  EXPECT_EQ(result.request_tx_packet, 2);
  EXPECT_EQ(result.request_tx_buffer, 4);
  EXPECT_EQ(result.request_rx_packet, 6);
  EXPECT_EQ(result.request_rx_buffer, 8);
  EXPECT_EQ(result.response_tx_packet, 5);
  EXPECT_EQ(result.response_tx_buffer, 6);
}

TEST(UlpBackpressureAlphaCarvingTest, ComputeAlphaCarvingHeadroomTest) {
  SimpleEnvironment env;
  uint32_t source_connection_id = 1;

  FalconConfig config = DefaultConfigGenerator::DefaultFalconConfig(2);

  // Explicitly setting the available credit number.
  auto falcon_credits = config.mutable_resource_credits();
  falcon_credits->mutable_tx_packet_credits()->set_ulp_requests(4);
  falcon_credits->mutable_tx_buffer_credits()->set_ulp_requests(8);
  falcon_credits->mutable_rx_packet_credits()->set_ulp_requests(12);
  falcon_credits->mutable_rx_buffer_credits()->set_ulp_requests(16);
  falcon_credits->mutable_tx_packet_credits()->set_ulp_data(20);
  falcon_credits->mutable_tx_buffer_credits()->set_ulp_data(24);

  NiceMock<MockRdma> rdma;
  NiceMock<MockTrafficShaper> shaper;
  FalconModel falcon(config, &env, /*stats_collector=*/nullptr,
                     ConnectionManager::GetConnectionManager(), "falcon-host",
                     /* number of hosts */ 4);
  falcon.ConnectShaper(&shaper);
  falcon.ConnectRdma(&rdma);

  // Initialize the connection state.
  ConnectionStateManager* const connection_state_manager =
      falcon.get_state_manager();
  std::unique_ptr<Gen2ConnectionMetadata> connection_metadata =
      std::make_unique<Gen2ConnectionMetadata>();
  connection_metadata->scid = source_connection_id;
  EXPECT_OK(connection_state_manager->InitializeConnectionState(
      std::move(connection_metadata)));

  absl::StatusOr<ConnectionState*> connection_state =
      connection_state_manager->PerformDirectLookup(/*scid*/ 1);
  EXPECT_OK(connection_state);

  // Explicitly setting the policy metadata.
  auto gen2_connection_state =
      dynamic_cast<Gen2ConnectionState*>(connection_state.value());
  gen2_connection_state->connection_xoff_metadata.alpha_request = 4;
  gen2_connection_state->connection_xoff_metadata.alpha_response = 4;

  auto policy = std::make_unique<AlphaCarvingUlpBackpressurePolicy>(&falcon, 8);
  // Test alpha carving can calculate limits according to the fabric RTT.
  // Take request_tx_packet's dynamic limit as an example, according to the
  // alpha carving algorithm, connection 1's dynamic resource limit is:
  // (avaliable_tx_resource - headroom) * base_alpha * base_delay/rtt_state =
  // max(4*(2-8)*1/4, 1) = 1.
  FalconCredit result;
  result = policy->ComputeAlphaCarvingForTesting(/*scid*/ 1);
  EXPECT_EQ(result.request_tx_packet, 1);
  EXPECT_EQ(result.request_tx_buffer, 4);
  EXPECT_EQ(result.request_rx_packet, 6);
  EXPECT_EQ(result.request_rx_buffer, 8);
  EXPECT_EQ(result.response_tx_packet, 6);
  EXPECT_EQ(result.response_tx_buffer, 12);
}

}  // namespace
}  // namespace isekai
