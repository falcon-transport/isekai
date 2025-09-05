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

#include "isekai/host/falcon/gen1/round_robin_arbiter.h"

#include <cstdint>
#include <memory>
#include <utility>

#include "absl/container/flat_hash_map.h"
#include "absl/status/statusor.h"
#include "absl/time/time.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/default_config_generator.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/common/packet.h"
#include "isekai/common/simple_environment.h"
#include "isekai/common/testing.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/falcon_connection_state.h"
#include "isekai/host/falcon/falcon_testing_helpers.h"
#include "isekai/host/falcon/gen1/falcon_model.h"
#include "isekai/host/rnic/connection_manager.h"

namespace isekai {
namespace {

class MockTrafficShaper : public TrafficShaperInterface {
 public:
  MOCK_METHOD(void, ConnectPacketBuilder,
              (PacketBuilderInterface * packet_builder), (override));
  MOCK_METHOD(void, TransferTxPacket, (std::unique_ptr<Packet> packet),
              (override));
};

TEST(ProtocolRoundRobinArbiterTest, ArbiterRoundRobinWork) {
  SimpleEnvironment env;
  FalconConfig config = DefaultConfigGenerator::DefaultFalconConfig(1);
  config.set_falcon_tick_time_ns(1);
  MockTrafficShaper shaper;
  FalconModel falcon(config, &env, /*stats_collector=*/nullptr,
                     ConnectionManager::GetConnectionManager(), "falcon-host",
                     /* number of hosts */ 4);
  falcon.ConnectShaper(&shaper);
  // Initialize the connection state.
  ConnectionStateManager* const connection_state_manager =
      falcon.get_state_manager();
  uint32_t source_connection_id = 1;
  auto connection_metadata = FalconTestingHelpers::InitializeConnectionMetadata(
      &falcon, source_connection_id);
  EXPECT_OK(connection_state_manager->InitializeConnectionState(
      std::move(connection_metadata)));

  // Initialize metadata corresponding to the packet metadata.
  auto packet_metadata = std::make_unique<PacketMetadata>();
  packet_metadata->psn = 1;
  packet_metadata->type = falcon::PacketType::kPullRequest;
  packet_metadata->direction = PacketDirection::kOutgoing;
  packet_metadata->active_packet = std::make_unique<Packet>();
  packet_metadata->active_packet->rdma.request_length = 1;
  packet_metadata->active_packet->metadata.scid = source_connection_id;

  // Initialize metadata corresponding to the transaction.
  auto transaction = std::make_unique<TransactionMetadata>();
  transaction->rsn = 1;
  transaction->type = TransactionType::kPull;
  transaction->request_length = 50;
  transaction->location = TransactionLocation::kInitiator;
  transaction->state = TransactionState::kPullReqUlpRx;
  transaction->packets[packet_metadata->type] = std::move(packet_metadata);

  // Add the above metadata to the appropriate connection state.
  absl::StatusOr<ConnectionState*> connection_state =
      connection_state_manager->PerformDirectLookup(source_connection_id);
  EXPECT_OK(connection_state);
  connection_state.value()
      ->transactions[{transaction->rsn, TransactionLocation::kInitiator}] =
      std::move(transaction);
  // Set the retransmission timer such that no retransmissions are triggered.
  connection_state.value()->congestion_control_metadata->retransmit_timeout =
      absl::Nanoseconds(50);

  // Initialize packet metadata corresponding to the retransmission.
  auto retransmission_packet = std::make_unique<PacketMetadata>();
  retransmission_packet->psn = 2;
  retransmission_packet->type = falcon::PacketType::kPullRequest;
  retransmission_packet->direction = PacketDirection::kOutgoing;
  retransmission_packet->active_packet = std::make_unique<Packet>();
  retransmission_packet->active_packet->rdma.request_length = 1;
  retransmission_packet->active_packet->metadata.scid = 1;
  retransmission_packet->transmit_attempts = 1;

  // Initialize metadata corresponding to the transaction.
  auto retransmission = std::make_unique<TransactionMetadata>();
  retransmission->rsn = 2;
  retransmission->type = TransactionType::kPull;
  retransmission->request_length = 50;
  retransmission->location = TransactionLocation::kInitiator;
  retransmission->packets[retransmission_packet->type] =
      std::move(retransmission_packet);

  // Add the above metadata to the appropriate connection state.
  connection_state.value()
      ->transactions[{retransmission->rsn, TransactionLocation::kInitiator}] =
      std::move(retransmission);

  // Add (1,2,2) transaction to retransmission scheduler.
  auto* retransmission_scheduler = falcon.get_retransmission_scheduler();
  EXPECT_OK(retransmission_scheduler->EnqueuePacket(
      1, 2, falcon::PacketType::kPullRequest));
  // Add (1,1,1) transaction to retransmission scheduler.
  auto* connection_scheduler = falcon.get_connection_scheduler();
  EXPECT_OK(connection_scheduler->EnqueuePacket(
      1, 1, falcon::PacketType::kPullRequest));
  // Schedule Arbiter again (this should not schedule an actual event since the
  // retransmission scheduler has already scheduled one).
  falcon.get_arbiter()->ScheduleSchedulerArbiter();

  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(30), [&]() { env.Stop(); }));
  env.Run();
  // Check if the transaction (1,1,kPullRequest) was actually scheduled (PSN=1)
  // .
  EXPECT_OK(connection_state.value()
                ->GetTransaction({1, TransactionLocation::kInitiator})
                .value()
                ->GetPacketMetadata(falcon::PacketType::kPullRequest)
                .value()
                ->schedule_status);
  // Check if the transaction (1,2,kPullRequest) was actually scheduled (PSN=2).
  EXPECT_OK(connection_state.value()
                ->GetTransaction({2, TransactionLocation::kInitiator})
                .value()
                ->GetPacketMetadata(falcon::PacketType::kPullRequest)
                .value()
                ->schedule_status);
  // The two packets should be scheduled at different times. This test is to
  // ensure that the standalone falcon.get_arbiter()->ScheduleSchedulerArbiter()
  // above does not schedule a new event (if it does, two events at time 0 will
  // send two packets at the same time).
  EXPECT_NE(connection_state.value()
                ->GetTransaction({1, TransactionLocation::kInitiator})
                .value()
                ->GetPacketMetadata(falcon::PacketType::kPullRequest)
                .value()
                ->transmission_time,
            connection_state.value()
                ->GetTransaction({2, TransactionLocation::kInitiator})
                .value()
                ->GetPacketMetadata(falcon::PacketType::kPullRequest)
                .value()
                ->transmission_time);
}

}  // namespace
}  // namespace isekai
