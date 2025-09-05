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
#include "isekai/host/falcon/falcon.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/falcon_connection_state.h"
#include "isekai/host/falcon/falcon_testing_helpers.h"
#include "isekai/host/falcon/gen1/falcon_model.h"
#include "isekai/host/falcon/gen1/falcon_types.h"
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

TEST(ProtocolRetransmissionSchedulerTest, WorkScheduler) {
  SimpleEnvironment env;
  FalconConfig config = DefaultConfigGenerator::DefaultFalconConfig(1);
  NiceMock<MockTrafficShaper> shaper;
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

  // Initialize metadata corresponding an kPullRequest packet.
  auto packet_metadata = std::make_unique<PacketMetadata>();
  packet_metadata->psn = 1;
  packet_metadata->type = falcon::PacketType::kPullRequest;
  packet_metadata->direction = PacketDirection::kOutgoing;
  packet_metadata->active_packet = std::make_unique<Packet>();
  packet_metadata->active_packet->metadata.scid = 1;
  packet_metadata->active_packet->rdma.request_length = 1;
  packet_metadata->transmit_attempts = 1;

  // Initialize metadata corresponding to the transaction.
  auto transaction = std::make_unique<TransactionMetadata>();
  transaction->rsn = 1;
  transaction->type = TransactionType::kPull;
  transaction->location = TransactionLocation::kInitiator;
  transaction->packets[packet_metadata->type] = std::move(packet_metadata);

  // Add the above metadata to the appropriate connection state and set the
  // retransmission timeout to non-zero.
  absl::StatusOr<ConnectionState*> connection_state =
      connection_state_manager->PerformDirectLookup(source_connection_id);
  EXPECT_OK(connection_state);
  connection_state.value()
      ->transactions[{transaction->rsn, TransactionLocation::kInitiator}] =
      std::move(transaction);
  const absl::Duration kRetransmissionTimer = absl::Nanoseconds(20);
  connection_state.value()->congestion_control_metadata->retransmit_timeout =
      kRetransmissionTimer;

  // Add one transaction and this should trigger the scheduler.
  auto* retransmission_scheduler = falcon.get_retransmission_scheduler();
  EXPECT_OK(retransmission_scheduler->EnqueuePacket(
      1, 1, falcon::PacketType::kPullRequest));
  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(11), [&]() { env.Stop(); }));
  env.Run();
  // Check if the transaction was actually scheduled.
  EXPECT_OK(connection_state.value()
                ->GetTransaction({1, TransactionLocation::kInitiator})
                .value()
                ->GetPacketMetadata(falcon::PacketType::kPullRequest)
                .value()
                ->schedule_status);
}

TEST(ProtocolRetransmissionSchedulerTest, TestOpRateWorkScheduler) {
  SimpleEnvironment env;
  FalconConfig config = DefaultConfigGenerator::DefaultFalconConfig(1);
  constexpr int kFalconCycleTime = 5;
  config.set_falcon_tick_time_ns(kFalconCycleTime);
  config.mutable_rue()->set_initial_fcwnd(256);
  config.mutable_rue()->set_initial_ncwnd(256);
  config.mutable_rue()->set_initial_retransmit_timeout_ns(10000);
  NiceMock<MockTrafficShaper> shaper;
  FalconModel falcon(config, &env, /*stats_collector=*/nullptr,
                     ConnectionManager::GetConnectionManager(), "falcon-host",
                     /* number of hosts */ 4);
  falcon.ConnectShaper(&shaper);

  // Initialize the connection state.
  auto connection_metadata =
      FalconTestingHelpers::InitializeConnectionMetadata(&falcon);
  auto connection_state = FalconTestingHelpers::InitializeConnectionState(
      &falcon, std::move(connection_metadata));
  connection_state->last_packet_send_time = absl::ZeroDuration();

  constexpr int kManyOps = 128;
  constexpr int kExpectedOps = 40;
  for (int i = 0; i < kManyOps; ++i) {
    FalconTestingHelpers::SetupTransactionWithConnectionState(
        connection_state, TransactionType::kPull,
        TransactionLocation::kInitiator, TransactionState::kPullReqUlpRx,
        falcon::PacketType::kPullRequest, /*rsn=*/i, /*psn=*/i);

    // Increment transmit attempts to 1 before enqueuing in retx scheduler.
    ASSERT_OK_AND_ASSIGN(auto transaction,
                         connection_state->GetTransaction(TransactionKey(
                             /*rsn=*/i, TransactionLocation::kInitiator)));
    ASSERT_OK_AND_ASSIGN(
        auto packet_metadata,
        transaction->GetPacketMetadata(falcon::PacketType::kPullRequest));
    packet_metadata->transmit_attempts = 1;
    packet_metadata->retransmission_reason = RetransmitReason::kTimeout;

    // Add one transaction and this should trigger the scheduler.
    auto retx_scheduler = falcon.get_connection_scheduler();
    EXPECT_OK(retx_scheduler->EnqueuePacket(
        /*scid=*/1, /*rsn=*/i, falcon::PacketType::kPullRequest));
  }

  // After one microsecond, 40 transactions are retransmitted, plus 1 at t = 0.
  env.RunFor(absl::Microseconds(1));
  auto counters = falcon.get_stats_manager()->GetConnectionCounters(/*cid=*/1);
  EXPECT_EQ(kExpectedOps + 1, counters.tx_timeout_retransmitted);

  // After one microsecond, 40 more transactions are retransmitted.
  env.RunFor(absl::Microseconds(1));
  counters = falcon.get_stats_manager()->GetConnectionCounters(/*cid=*/1);
  EXPECT_EQ(2 * kExpectedOps + 1, counters.tx_timeout_retransmitted);

  // After one microsecond, 40 more transactions are retransmitted.
  env.RunFor(absl::Microseconds(1));
  counters = falcon.get_stats_manager()->GetConnectionCounters(/*cid=*/1);
  EXPECT_EQ(3 * kExpectedOps + 1, counters.tx_timeout_retransmitted);
}

}  // namespace
}  // namespace isekai
