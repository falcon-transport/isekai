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
#include <string>
#include <utility>

#include "absl/container/flat_hash_map.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/time/time.h"
#include "gmock/gmock.h"
#include "google/protobuf/text_format.h"
#include "gtest/gtest.h"
#include "isekai/common/config.pb.h"
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

namespace isekai {
namespace {

// This defines all the objects needed for setup and testing. The test fixture
// is parameterized on the Falcon version.
class FalconAdmissionControlTest
    : public FalconTestingHelpers::FalconTestSetup,
      public ::testing::TestWithParam<int /* version */> {
 protected:
  int GetFalconVersion() { return GetParam(); }
};

INSTANTIATE_TEST_SUITE_P(AdmissionControlTest, FalconAdmissionControlTest,
                         /*version=*/testing::Values(1, 2),
                         [](const testing::TestParamInfo<
                             FalconAdmissionControlTest::ParamType>& info) {
                           const int version = static_cast<int>(info.param);
                           return absl::StrCat("Gen", version);
                         });

std::unique_ptr<TransactionMetadata> MakeTransactionWithPullRequestPacket(
    uint8_t falcon_version, uint32_t request_length) {
  // Initializes metadata corresponding to a kPullRequest packet.
  auto packet_metadata = std::make_unique<PacketMetadata>();
  packet_metadata->psn = 1;
  packet_metadata->type = falcon::PacketType::kPullRequest;
  packet_metadata->direction = PacketDirection::kOutgoing;
  packet_metadata->active_packet = std::make_unique<Packet>();
  packet_metadata->active_packet->rdma.request_length = 50;

  // Initializes metadata corresponding to the transaction.
  auto transaction =
      CreateTransactionMetadata(falcon_version, 1, TransactionType::kPull,
                                TransactionLocation::kInitiator);
  transaction->request_length = 50;
  transaction->packets[packet_metadata->type] = std::move(packet_metadata);

  return transaction;
}

std::unique_ptr<TransactionMetadata> MakeTransactionWithPullDataPacket(
    uint8_t falcon_version, uint32_t request_length) {
  // Initializes metadata corresponding to a kPullData packet.
  auto packet_metadata = std::make_unique<PacketMetadata>();
  packet_metadata->psn = 1;
  packet_metadata->direction = PacketDirection::kOutgoing;
  packet_metadata->active_packet = std::make_unique<Packet>();
  packet_metadata->active_packet->rdma.request_length = request_length;

  // Initializes metadata corresponding to the transaction.
  auto transaction = CreateTransactionMetadata(
      falcon_version, 1, TransactionType::kPull, TransactionLocation::kTarget);
  transaction->request_length = request_length;
  transaction->packets[falcon::PacketType::kPullData] =
      std::move(packet_metadata);

  return transaction;
}

TEST_P(FalconAdmissionControlTest, WindowBasedControlPullRequest) {
  // Initializes the admission control manager.
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
  config.set_admission_control_policy(FalconConfig::RX_WINDOW_BASED);
  config.set_admission_window_bytes(50);
  // Set the parametrized test values in FalconConfig and initialize Falcon test
  // setup.
  InitFalcon(config);

  uint32_t source_connection_id = 1;
  auto connection_metadata = FalconTestingHelpers::InitializeConnectionMetadata(
      falcon_.get(), source_connection_id);
  EXPECT_OK(connection_state_manager_->InitializeConnectionState(
      std::move(connection_metadata)));

  auto transaction =
      MakeTransactionWithPullRequestPacket(GetFalconVersion(), 50);

  // Adds the above metadata to the appropriate connection state.
  absl::StatusOr<ConnectionState*> connection_state =
      connection_state_manager_->PerformDirectLookup(source_connection_id);
  EXPECT_OK(connection_state);
  connection_state.value()
      ->transactions[{transaction->rsn, TransactionLocation::kInitiator}] =
      std::move(transaction);

  // Window size is 50 bytes and this transaction is also 50 bytes
  EXPECT_EQ(admission_control_manager_->MeetsAdmissionControlCriteria(
                1, 1, falcon::PacketType::kPullRequest),
            true);
  // Reserves the resources.
  EXPECT_OK(admission_control_manager_->ReserveAdmissionControlResource(
      1, 1, falcon::PacketType::kPullRequest));
  // This should return false as we do not have enough window capacity.
  EXPECT_EQ(admission_control_manager_->MeetsAdmissionControlCriteria(
                1, 1, falcon::PacketType::kPullRequest),
            false);
  EXPECT_OK(admission_control_manager_->RefundAdmissionControlResource(
      1, {1, TransactionLocation::kInitiator}));
  // This should return true as we refund the capacity and now have enough
  // admission window capacity.
  EXPECT_EQ(admission_control_manager_->MeetsAdmissionControlCriteria(
                1, 1, falcon::PacketType::kPullRequest),
            true);
}

TEST_P(FalconAdmissionControlTest, WindowBasedControlPullData) {
  // Initializes the admission control manager.
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
  config.set_admission_control_policy(FalconConfig::RX_WINDOW_BASED);
  config.set_admission_window_bytes(0);
  // Set the parametrized test values in FalconConfig and initialize Falcon test
  // setup.
  InitFalcon(config);

  uint32_t source_connection_id = 1;
  auto connection_metadata = FalconTestingHelpers::InitializeConnectionMetadata(
      falcon_.get(), source_connection_id);
  EXPECT_OK(connection_state_manager_->InitializeConnectionState(
      std::move(connection_metadata)));

  auto transaction = MakeTransactionWithPullDataPacket(GetFalconVersion(), 50);

  // Adds the above metadata to the appropriate connection state.
  absl::StatusOr<ConnectionState*> connection_state =
      connection_state_manager_->PerformDirectLookup(source_connection_id);
  EXPECT_OK(connection_state);
  connection_state.value()
      ->transactions[{transaction->rsn, TransactionLocation::kTarget}] =
      std::move(transaction);

  // Though the window size is 0, this succeeds as this transaction does not
  // require reserving any capacity in the window.
  EXPECT_EQ(admission_control_manager_->MeetsAdmissionControlCriteria(
                1, 1, falcon::PacketType::kPullData),
            true);
}

TEST_P(FalconAdmissionControlTest, RxRateBasedControlPullRequest) {
  // Initializes the admission control manager.
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
  // Configure to use the RX_RATE_BASED and provide the required metadata.
  config.set_admission_control_policy(FalconConfig::RX_RATE_BASED);
  const std::string kRxRateLimiterMetadata = R"pb(solicitation_rate_gbps: 1
                                                  burst_size_bytes: 50
                                                  refill_interval_ns: 5000)pb";
  ASSERT_TRUE(google::protobuf::TextFormat::ParseFromString(
      kRxRateLimiterMetadata, config.mutable_rx_rate_limiter_metadata()));
  // Set the parametrized test values in FalconConfig and initialize Falcon test
  // setup.
  InitFalcon(config);

  uint32_t source_connection_id = 1;
  auto connection_metadata = FalconTestingHelpers::InitializeConnectionMetadata(
      falcon_.get(), source_connection_id);
  EXPECT_OK(connection_state_manager_->InitializeConnectionState(
      std::move(connection_metadata)));

  auto transaction =
      MakeTransactionWithPullRequestPacket(GetFalconVersion(), 50);

  // Adds the above metadata to the appropriate connection state.
  absl::StatusOr<ConnectionState*> connection_state =
      connection_state_manager_->PerformDirectLookup(source_connection_id);
  EXPECT_OK(connection_state);
  connection_state.value()
      ->transactions[{transaction->rsn, TransactionLocation::kInitiator}] =
      std::move(transaction);

  // Initial tokens is 50 bytes and this transaction is also 50 bytes
  EXPECT_EQ(admission_control_manager_->MeetsAdmissionControlCriteria(
                1, 1, falcon::PacketType::kPullRequest),
            true);
  // Reserves the token from the token bucket.
  EXPECT_OK(admission_control_manager_->ReserveAdmissionControlResource(
      1, 1, falcon::PacketType::kPullRequest));
  // This should return false as we do not have enough tokens.
  EXPECT_EQ(admission_control_manager_->MeetsAdmissionControlCriteria(
                1, 1, falcon::PacketType::kPullRequest),
            false);

  EXPECT_OK(admission_control_manager_->RefundAdmissionControlResource(
      1, {1, TransactionLocation::kTarget}));
  // This should continue to return false as rate-bsaed is open loop and we
  // haven't proceeded in time to refill tokens in the bucket.
  EXPECT_EQ(admission_control_manager_->MeetsAdmissionControlCriteria(
                1, 1, falcon::PacketType::kPullRequest),
            false);

  // Try to schedule the request after the token bucket is refilled and check
  // status.
  bool admission_status = false;
  EXPECT_OK(env_.ScheduleEvent(absl::Nanoseconds(5000), [&]() {
    admission_status =
        admission_control_manager_->MeetsAdmissionControlCriteria(
            1, 1, falcon::PacketType::kPullRequest);
  }));
  EXPECT_OK(
      env_.ScheduleEvent(absl::Nanoseconds(5001), [&]() { env_.Stop(); }));
  env_.Run();
  EXPECT_TRUE(admission_status);
}

TEST_P(FalconAdmissionControlTest, TxRateBasedControlPushRequest) {
  // Initializes the admission control manager.
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
  // Configure to use the TX_RATE_BASED and provide the required metadata.
  config.set_admission_control_policy(FalconConfig::TX_RATE_BASED);
  const std::string kTxRateLimiterMetadata = R"pb(solicitation_rate_gbps: 1
                                                  burst_size_bytes: 50
                                                  refill_interval_ns: 5000)pb";
  ASSERT_TRUE(google::protobuf::TextFormat::ParseFromString(
      kTxRateLimiterMetadata, config.mutable_tx_rate_limiter_metadata()));
  // Set the parametrized test values in FalconConfig and initialize Falcon test
  // setup.
  InitFalcon(config);

  uint32_t source_connection_id = 1;
  auto connection_metadata = FalconTestingHelpers::InitializeConnectionMetadata(
      falcon_.get(), source_connection_id);
  EXPECT_OK(connection_state_manager_->InitializeConnectionState(
      std::move(connection_metadata)));

  // Initializes metadata corresponding to a kPushRequest packet.
  auto packet_metadata = std::make_unique<PacketMetadata>();
  packet_metadata->psn = 1;
  packet_metadata->type = falcon::PacketType::kPushRequest;
  packet_metadata->direction = PacketDirection::kOutgoing;
  packet_metadata->active_packet = std::make_unique<Packet>();
  packet_metadata->active_packet->rdma.request_length = 50;

  // Initializes metadata corresponding to the transaction.
  auto transaction = CreateTransactionMetadata(GetFalconVersion(), 1,
                                               TransactionType::kPushSolicited,
                                               TransactionLocation::kInitiator);
  transaction->request_length = 50;
  transaction->packets[packet_metadata->type] = std::move(packet_metadata);

  // Adds the above metadata to the appropriate connection state.
  absl::StatusOr<ConnectionState*> connection_state =
      connection_state_manager_->PerformDirectLookup(source_connection_id);
  EXPECT_OK(connection_state);
  connection_state.value()
      ->transactions[{transaction->rsn, TransactionLocation::kInitiator}] =
      std::move(transaction);

  // Initial tokens is 50 bytes and this transaction is also 50 bytes
  EXPECT_EQ(admission_control_manager_->MeetsAdmissionControlCriteria(
                1, 1, falcon::PacketType::kPushRequest),
            true);
  // Reserves the token from the token bucket.
  EXPECT_OK(admission_control_manager_->ReserveAdmissionControlResource(
      1, 1, falcon::PacketType::kPushRequest));
  // This should return false as we do not have enough tokens.
  EXPECT_EQ(admission_control_manager_->MeetsAdmissionControlCriteria(
                1, 1, falcon::PacketType::kPushRequest),
            false);

  EXPECT_OK(admission_control_manager_->RefundAdmissionControlResource(
      1, {1, TransactionLocation::kInitiator}));
  // This should continue to return false as rate-based is open loop and we
  // haven't proceeded in time to refill tokens in the bucket.
  EXPECT_EQ(admission_control_manager_->MeetsAdmissionControlCriteria(
                1, 1, falcon::PacketType::kPushRequest),
            false);
  // Try to schedule the request after the token bucket is refilled and check
  // status.
  bool admission_status = false;
  EXPECT_OK(env_.ScheduleEvent(absl::Nanoseconds(5000), [&]() {
    admission_status =
        admission_control_manager_->MeetsAdmissionControlCriteria(
            1, 1, falcon::PacketType::kPushRequest);
  }));
  EXPECT_OK(
      env_.ScheduleEvent(absl::Nanoseconds(5001), [&]() { env_.Stop(); }));
  env_.Run();
  EXPECT_TRUE(admission_status);
}

}  // namespace
}  // namespace isekai
