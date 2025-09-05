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

#include "isekai/host/falcon/gen1/buffer_reorder_engine.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "absl/random/random.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "isekai/common/common_util.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/default_config_generator.h"
#include "isekai/common/environment.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/common/packet.h"
#include "isekai/common/simple_environment.h"
#include "isekai/common/testing.h"
#include "isekai/host/falcon/falcon.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/falcon_connection_state.h"
#include "isekai/host/falcon/falcon_counters.h"
#include "isekai/host/falcon/falcon_testing_helpers.h"
#include "isekai/host/falcon/gen1/falcon_model.h"
#include "isekai/host/falcon/gen1/falcon_utils.h"
#include "isekai/host/rnic/connection_manager.h"

namespace isekai {
class FalconLatencyStatsCollector;

namespace {

constexpr uint32_t kScid = 1;
constexpr uint32_t kDcid = 2;

// This class serves the test cases that test the buffer reorder engine
// component. We use a fake Falcon and the test fixture is parameterized on the
// reorder engine version.
class FakeFalconReorderBufferEngineTest
    : public ::testing::TestWithParam<int /* version */> {
 protected:
  int GetFalconVersion() { return GetParam(); }
  void SetUp() override {
    falcon_ =
        std::make_unique<FalconTestingHelpers::FakeFalconModel>(config_, env_);
    reorder_engine =
        std::make_unique<ProtocolBufferReorderEngine>(falcon_.get());
  }
  SimpleEnvironment env_;
  FalconConfig config_;
  std::unique_ptr<FalconTestingHelpers::FakeFalconModel> falcon_;
  std::unique_ptr<BufferReorderEngine> reorder_engine;
};

INSTANTIATE_TEST_SUITE_P(
    FakeReorderBufferEngineTest, FakeFalconReorderBufferEngineTest,
    /*version=*/testing::Values(1, 2),
    [](const testing::TestParamInfo<
        FakeFalconReorderBufferEngineTest::ParamType>& info) {
      const int version = static_cast<int>(info.param);
      return absl::StrCat("Gen", version);
    });

// This class serves the test cases that test the interaction between buffer
// reorder engine and other Falcon components. We use a real Falcon and the
// test fixture is parameterized on the reorder engine version.
class FalconReorderBufferEngineTest
    : public FalconTestingHelpers::FalconTestSetup,
      public ::testing::TestWithParam<int /* version */> {
 protected:
  int GetFalconVersion() { return GetParam(); }
};

INSTANTIATE_TEST_SUITE_P(ReorderBufferEngineTest, FalconReorderBufferEngineTest,
                         /*version=*/testing::Values(1, 2),
                         [](const testing::TestParamInfo<
                             FalconReorderBufferEngineTest::ParamType>& info) {
                           const int version = static_cast<int>(info.param);
                           return absl::StrCat("Gen", version);
                         });

TEST_P(FakeFalconReorderBufferEngineTest, TestInitiatorRsnOrder) {
  EXPECT_OK(reorder_engine->InitializeConnection(kScid, /*initiator_rsn=*/1,
                                                 /*target_rsn=*/1,
                                                 /*initiator_ssn=*/1,
                                                 /*target_ssn=*/1,
                                                 OrderingMode::kOrdered));

  EXPECT_OK(reorder_engine->InsertPacket(falcon::PacketType::kPullData, kScid,
                                         /*rsn=*/1, /*ssn=*/0));

  EXPECT_THAT(falcon_->rsn_order_[kScid], testing::ElementsAre(1));

  EXPECT_OK(reorder_engine->InsertPacket(falcon::PacketType::kAck, kScid,
                                         /*rsn=*/3, /*ssn=*/0));

  EXPECT_OK(reorder_engine->InsertPacket(falcon::PacketType::kAck, kScid,
                                         /*rsn=*/4, /*ssn=*/0));

  // RSN 3 and 4 should not show up since they are out of order.
  EXPECT_THAT(falcon_->rsn_order_[kScid], testing::ElementsAre(1));

  EXPECT_OK(reorder_engine->InsertPacket(falcon::PacketType::kPullData, kScid,
                                         /*rsn=*/2, /*ssn=*/0));

  // All 2, 3 and 4 should show up.
  EXPECT_THAT(falcon_->rsn_order_[kScid], testing::ElementsAre(1, 2, 3, 4));
}

TEST_P(FakeFalconReorderBufferEngineTest, TestTargetRsnOrder) {
  EXPECT_OK(reorder_engine->InitializeConnection(kScid, /*initiator_rsn=*/1,
                                                 /*target_rsn=*/1,
                                                 /*initiator_ssn=*/1,
                                                 /*target_ssn=*/1,
                                                 OrderingMode::kOrdered));

  EXPECT_OK(reorder_engine->InsertPacket(falcon::PacketType::kPullRequest,
                                         kScid,
                                         /*rsn=*/1, /*ssn=*/0));

  EXPECT_THAT(falcon_->rsn_order_[kScid], testing::ElementsAre(1));

  EXPECT_OK(reorder_engine->InsertPacket(falcon::PacketType::kPushSolicitedData,
                                         kScid, /*rsn=*/3, /*ssn=*/0));

  EXPECT_OK(reorder_engine->InsertPacket(falcon::PacketType::kPushSolicitedData,
                                         kScid, /*rsn=*/4, /*ssn=*/0));

  EXPECT_THAT(falcon_->rsn_order_[kScid], testing::ElementsAre(1));

  EXPECT_OK(reorder_engine->InsertPacket(
      falcon::PacketType::kPushUnsolicitedData, kScid, /*rsn=*/2, /*ssn=*/0));

  EXPECT_THAT(falcon_->rsn_order_[kScid], testing::ElementsAre(1, 2, 3, 4));
}

TEST_P(FakeFalconReorderBufferEngineTest, TestInitiatorSsnOrder) {
  EXPECT_OK(reorder_engine->InitializeConnection(kScid, /*initiator_rsn=*/1,
                                                 /*target_rsn=*/1,
                                                 /*initiator_ssn=*/1,
                                                 /*target_ssn=*/1,
                                                 OrderingMode::kOrdered));

  EXPECT_OK(reorder_engine->InsertPacket(falcon::PacketType::kPushGrant, kScid,
                                         /*rsn=*/4, /*ssn=*/1));

  EXPECT_THAT(falcon_->rsn_order_[kScid], testing::ElementsAre(4));

  EXPECT_OK(reorder_engine->InsertPacket(falcon::PacketType::kPushGrant, kScid,
                                         /*rsn=*/6, /*ssn=*/3));

  EXPECT_OK(reorder_engine->InsertPacket(falcon::PacketType::kPushGrant, kScid,
                                         /*rsn=*/7, /*ssn=*/4));

  EXPECT_THAT(falcon_->rsn_order_[kScid], testing::ElementsAre(4));

  EXPECT_OK(reorder_engine->InsertPacket(falcon::PacketType::kPushGrant, kScid,
                                         /*rsn=*/5, /*ssn=*/2));

  EXPECT_THAT(falcon_->rsn_order_[kScid], testing::ElementsAre(4, 5, 6, 7));
}

TEST_P(FakeFalconReorderBufferEngineTest, TestTargetSsnOrder) {
  EXPECT_OK(reorder_engine->InitializeConnection(kScid, /*initiator_rsn=*/1,
                                                 /*target_rsn=*/1,
                                                 /*initiator_ssn=*/1,
                                                 /*target_ssn=*/1,
                                                 OrderingMode::kOrdered));

  EXPECT_OK(reorder_engine->InsertPacket(falcon::PacketType::kPushRequest,
                                         kScid,
                                         /*rsn=*/4, /*ssn=*/1));

  EXPECT_THAT(falcon_->rsn_order_[kScid], testing::ElementsAre(4));

  EXPECT_OK(reorder_engine->InsertPacket(falcon::PacketType::kPushRequest,
                                         kScid,
                                         /*rsn=*/6, /*ssn=*/3));

  EXPECT_OK(reorder_engine->InsertPacket(falcon::PacketType::kPushRequest,
                                         kScid,
                                         /*rsn=*/7, /*ssn=*/4));

  EXPECT_THAT(falcon_->rsn_order_[kScid], testing::ElementsAre(4));

  EXPECT_OK(reorder_engine->InsertPacket(falcon::PacketType::kPushRequest,
                                         kScid,
                                         /*rsn=*/5, /*ssn=*/2));

  EXPECT_THAT(falcon_->rsn_order_[kScid], testing::ElementsAre(4, 5, 6, 7));
}

TEST_P(FakeFalconReorderBufferEngineTest, DeleteCidTest) {
  EXPECT_OK(reorder_engine->InitializeConnection(kScid, /*initiator_rsn=*/1,
                                                 /*target_rsn=*/1,
                                                 /*initiator_ssn=*/1,
                                                 /*target_ssn=*/1,
                                                 OrderingMode::kOrdered));

  EXPECT_OK(reorder_engine->InsertPacket(falcon::PacketType::kPullData, kScid,
                                         /*rsn=*/2, /*ssn=*/0));

  EXPECT_THAT(falcon_->rsn_order_[kScid], testing::ElementsAre());

  EXPECT_OK(reorder_engine->DeleteConnection(1));

  EXPECT_OK(reorder_engine->InitializeConnection(kScid, /*initiator_rsn=*/2,
                                                 /*target_rsn=*/1,
                                                 /*initiator_ssn=*/1,
                                                 /*target_ssn=*/1,
                                                 OrderingMode::kOrdered));

  EXPECT_OK(reorder_engine->InsertPacket(falcon::PacketType::kPullData, kScid,
                                         /*rsn=*/3, /*ssn=*/0));

  EXPECT_THAT(falcon_->rsn_order_[kScid], testing::ElementsAre());

  EXPECT_OK(reorder_engine->InsertPacket(falcon::PacketType::kPullData, kScid,
                                         /*rsn=*/2, /*ssn=*/0));

  EXPECT_THAT(falcon_->rsn_order_[kScid], testing::ElementsAre(2, 3));
}

TEST_P(FakeFalconReorderBufferEngineTest, RnrNackTestOrdered) {
  // Initialize the connection state.
  EXPECT_OK(reorder_engine->InitializeConnection(kScid, /*initiator_rsn=*/1,
                                                 /*target_rsn=*/1,
                                                 /*initiator_ssn=*/1,
                                                 /*target_ssn=*/1,
                                                 OrderingMode::kOrdered));

  // Send RSN 1,2,3.
  EXPECT_OK(
      reorder_engine->InsertPacket(falcon::PacketType::kPullRequest, 1, 1, 0));
  EXPECT_OK(reorder_engine->InsertPacket(
      falcon::PacketType::kPushUnsolicitedData, 1, 2, 0));
  EXPECT_OK(
      reorder_engine->InsertPacket(falcon::PacketType::kPullRequest, 1, 3, 0));
  env_.RunFor(absl::Microseconds(1));
  EXPECT_THAT(falcon_->rsn_order_[kScid], testing::ElementsAre(1, 2, 3));
  falcon_->rsn_order_[kScid].clear();

  // RNR NACK 1,2,3. All should be retried after 10us.
  reorder_engine->HandleRnrNackFromUlp(1, 1, absl::Microseconds(10));
  reorder_engine->HandleRnrNackFromUlp(1, 2, absl::Microseconds(10));
  reorder_engine->HandleRnrNackFromUlp(1, 3, absl::Microseconds(10));
  env_.RunFor(absl::Microseconds(10) - absl::Nanoseconds(1));
  EXPECT_THAT(falcon_->rsn_order_[kScid], testing::ElementsAre());
  env_.RunFor(absl::Nanoseconds(1));
  EXPECT_THAT(falcon_->rsn_order_[kScid], testing::ElementsAre(1, 2, 3));
  falcon_->rsn_order_[kScid].clear();

  // Out-of-order RNR NACK: 2, 1, 3.
  reorder_engine->HandleRnrNackFromUlp(1, 2, absl::Microseconds(10));
  env_.RunFor(absl::Microseconds(1));
  reorder_engine->HandleRnrNackFromUlp(1, 1, absl::Microseconds(10));
  env_.RunFor(absl::Microseconds(1));
  reorder_engine->HandleRnrNackFromUlp(1, 3, absl::Microseconds(10));
  env_.RunFor(absl::Microseconds(8));
  EXPECT_THAT(falcon_->rsn_order_[kScid], testing::ElementsAre());
  env_.RunFor(absl::Microseconds(1));
  EXPECT_THAT(falcon_->rsn_order_[kScid], testing::ElementsAre(1, 2));
  env_.RunFor(absl::Microseconds(1));
  EXPECT_THAT(falcon_->rsn_order_[kScid], testing::ElementsAre(1, 2, 3));
  falcon_->rsn_order_[kScid].clear();

  // RSN 4 arrive, should be sent to ULP.
  EXPECT_OK(
      reorder_engine->InsertPacket(falcon::PacketType::kPullRequest, 1, 4, 0));
  EXPECT_THAT(falcon_->rsn_order_[kScid], testing::ElementsAre(4));
  falcon_->rsn_order_[kScid].clear();

  // RNR NACK 1,2,3,4, then after 5us, RSN 5 (PushUsData), 6 (PullReq) arrive.
  // Reorder engine should trigger RNR-NACK for 5, without sending 5 to ULP.
  // Reorder engine should NOT trigger RNR-NACK for 6, and not send 6 to ULP.
  reorder_engine->HandleRnrNackFromUlp(1, 1, absl::Microseconds(10));
  reorder_engine->HandleRnrNackFromUlp(1, 2, absl::Microseconds(10));
  reorder_engine->HandleRnrNackFromUlp(1, 3, absl::Microseconds(10));
  reorder_engine->HandleRnrNackFromUlp(1, 4, absl::Microseconds(10));
  env_.RunFor(absl::Microseconds(5));
  EXPECT_EQ(
      reorder_engine
          ->InsertPacket(falcon::PacketType::kPushUnsolicitedData, 1, 5, 0)
          .code(),
      absl::StatusCode::kFailedPrecondition);
  EXPECT_OK(
      reorder_engine->InsertPacket(falcon::PacketType::kPullRequest, 1, 6, 0));
  EXPECT_THAT(falcon_->rsn_order_[kScid], testing::ElementsAre());
  falcon_->rsn_order_[kScid].clear();

  // After 5us, 1,2,3,4,5,6 should be retried.
  env_.RunFor(absl::Microseconds(5));
  EXPECT_THAT(falcon_->rsn_order_[kScid],
              testing::ElementsAre(1, 2, 3, 4, 5, 6));
  falcon_->rsn_order_[kScid].clear();

  // RSN 2 arrives again. Should be ignored.
  EXPECT_OK(reorder_engine->RetryRnrNackedPacket(1, 2));
  EXPECT_THAT(falcon_->rsn_order_[kScid], testing::ElementsAre());

  // RNR-NACK 1, 2. Then RSN 2 arrives again. Should trigger RNR-NACK for 2,
  // without sending 2 to ULP.
  reorder_engine->HandleRnrNackFromUlp(1, 1, absl::Microseconds(10));
  reorder_engine->HandleRnrNackFromUlp(1, 2, absl::Microseconds(10));
  EXPECT_EQ(reorder_engine->RetryRnrNackedPacket(1, 2).code(),
            absl::StatusCode::kFailedPrecondition);
  EXPECT_THAT(falcon_->rsn_order_[kScid], testing::ElementsAre());

  // RNR-NACK 3,4, after 5us, RNR-NACK 5,6.
  reorder_engine->HandleRnrNackFromUlp(1, 3, absl::Microseconds(10));
  reorder_engine->HandleRnrNackFromUlp(1, 4, absl::Microseconds(10));
  env_.RunFor(absl::Microseconds(5));
  reorder_engine->HandleRnrNackFromUlp(1, 5, absl::Microseconds(10));
  reorder_engine->HandleRnrNackFromUlp(1, 6, absl::Microseconds(10));

  // After 5us, 1,2,3,4 should be retried.
  env_.RunFor(absl::Microseconds(5));
  EXPECT_THAT(falcon_->rsn_order_[kScid], testing::ElementsAre(1, 2, 3, 4));
  falcon_->rsn_order_[kScid].clear();

  // 5 arrive again. Should trigger retry 5.
  EXPECT_OK(reorder_engine->RetryRnrNackedPacket(1, 5));
  EXPECT_THAT(falcon_->rsn_order_[kScid], testing::ElementsAre(5));
  falcon_->rsn_order_[kScid].clear();

  // 6 still retried after 5us.
  env_.RunFor(absl::Microseconds(5));
  EXPECT_THAT(falcon_->rsn_order_[kScid], testing::ElementsAre(6));
  falcon_->rsn_order_[kScid].clear();
}

TEST_P(FakeFalconReorderBufferEngineTest, RnrNackTestUnordered) {
  // Initialize the connection state.
  EXPECT_OK(reorder_engine->InitializeConnection(kScid, /*initiator_rsn=*/1,
                                                 /*target_rsn=*/1,
                                                 /*initiator_ssn=*/1,
                                                 /*target_ssn=*/1,
                                                 OrderingMode::kUnordered));

  // Send RSN 3, 2, 1
  EXPECT_OK(reorder_engine->InsertPacket(
      falcon::PacketType::kPushUnsolicitedData, 1, 3, 0));
  EXPECT_OK(reorder_engine->InsertPacket(
      falcon::PacketType::kPushUnsolicitedData, 1, 2, 0));
  EXPECT_OK(reorder_engine->InsertPacket(
      falcon::PacketType::kPushUnsolicitedData, 1, 1, 0));
  env_.RunFor(absl::Microseconds(1));
  EXPECT_THAT(falcon_->rsn_order_[kScid], testing::ElementsAre(3, 2, 1));
  falcon_->rsn_order_[kScid].clear();

  // RNR NACK 3, 2, 1. They should be retried at different time.
  reorder_engine->HandleRnrNackFromUlp(1, 3, absl::Microseconds(10));
  env_.RunFor(absl::Microseconds(1));
  reorder_engine->HandleRnrNackFromUlp(1, 2, absl::Microseconds(10));
  env_.RunFor(absl::Microseconds(1));
  reorder_engine->HandleRnrNackFromUlp(1, 1, absl::Microseconds(10));
  env_.RunFor(absl::Microseconds(8) - absl::Nanoseconds(1));
  EXPECT_THAT(falcon_->rsn_order_[kScid], testing::ElementsAre());
  env_.RunFor(absl::Nanoseconds(1));
  EXPECT_THAT(falcon_->rsn_order_[kScid], testing::ElementsAre(3));
  env_.RunFor(absl::Microseconds(1));
  EXPECT_THAT(falcon_->rsn_order_[kScid], testing::ElementsAre(3, 2));
  env_.RunFor(absl::Microseconds(1));
  EXPECT_THAT(falcon_->rsn_order_[kScid], testing::ElementsAre(3, 2, 1));
  falcon_->rsn_order_[kScid].clear();

  // After RNR-NACK 2, receive 1, 2, 3 again. Only 2 should be retried.
  reorder_engine->HandleRnrNackFromUlp(1, 2, absl::Microseconds(10));
  EXPECT_OK(reorder_engine->RetryRnrNackedPacket(1, 3));
  EXPECT_OK(reorder_engine->RetryRnrNackedPacket(1, 2));
  EXPECT_OK(reorder_engine->RetryRnrNackedPacket(1, 1));
  EXPECT_THAT(falcon_->rsn_order_[kScid], testing::ElementsAre(2));
  falcon_->rsn_order_[kScid].clear();
}

TEST_P(FalconReorderBufferEngineTest,
       IsHoLNetworkRequestOrderedConnectionTest) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
  InitFalcon(config);
  auto connection_metadata = FalconTestingHelpers::InitializeConnectionMetadata(
      falcon_.get(), kScid, OrderingMode::kOrdered);
  connection_metadata->dcid = kDcid;
  EXPECT_OK(connection_state_manager_->InitializeConnectionState(
      std::move(connection_metadata)));

  // Given the current target RSN = 0, RSN = 2 represents non-HoL request.
  EXPECT_FALSE(reorder_engine_->IsHeadOfLineNetworkRequest(kScid, /*rsn=*/2));
  // Given the current target RSN = 0, RSN = 0 represents HoL request.
  EXPECT_TRUE(reorder_engine_->IsHeadOfLineNetworkRequest(kScid, /*rsn=*/0));
  // Given the current target RSN = 0 as we haven't received ACK from ULP, RSN =
  // 1 represents non-HoL request.
  EXPECT_FALSE(reorder_engine_->IsHeadOfLineNetworkRequest(kScid, /*rsn=*/1));
  reorder_engine_->HandleAckFromUlp(kScid, /*rsn=*/1);
  // Given the current target RSN = 1 as we received ACK from ULP, RSN =
  // 1 represents non-HoL request.
  EXPECT_TRUE(reorder_engine_->IsHeadOfLineNetworkRequest(kScid, /*rsn=*/1));
}

TEST_P(FalconReorderBufferEngineTest,
       IsHoLNetworkRequestUnorderedConnectionTest) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
  InitFalcon(config);
  // Initialize an unordered connection.
  auto connection_metadata = FalconTestingHelpers::InitializeConnectionMetadata(
      falcon_.get(), kScid, OrderingMode::kUnordered);
  connection_metadata->dcid = kDcid;
  EXPECT_OK(connection_state_manager_->InitializeConnectionState(
      std::move(connection_metadata)));

  // Given that the connection is unordered, any request is considered HoL.
  EXPECT_TRUE(reorder_engine_->IsHeadOfLineNetworkRequest(kScid, /*rsn=*/3));
}

}  // namespace
}  // namespace isekai
