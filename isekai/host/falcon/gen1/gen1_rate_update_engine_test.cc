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
#include <string>

#include "absl/container/flat_hash_map.h"
#include "absl/time/time.h"
#include "gtest/gtest.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/default_config_generator.h"
#include "isekai/common/simple_environment.h"
#include "isekai/common/testing.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/falcon_connection_state.h"
#include "isekai/host/falcon/falcon_testing_helpers.h"
#include "isekai/host/falcon/gen1/falcon_model.h"
#include "isekai/host/falcon/gen1/falcon_types.h"
#include "isekai/host/falcon/gen1/rate_update_engine.h"
#include "isekai/host/falcon/rue/format.h"
#include "isekai/host/rnic/connection_manager.h"

namespace isekai {

namespace {

constexpr int kFalconVersion = 1;
constexpr uint32_t kCid1 = 123;

void CheckDefaultMetadata(
    const CongestionControlMetadata& congestion_control_metadata) {
  EXPECT_LT(congestion_control_metadata.flow_label, 1 << kIpv6FlowLabelNumBits);
  EXPECT_EQ(congestion_control_metadata.fabric_congestion_window,
            Gen1RateUpdateEngine::kDefaultFabricCongestionWindow);
  EXPECT_EQ(congestion_control_metadata.inter_packet_gap,
            Gen1RateUpdateEngine::kDefaultInterPacketGap);
  EXPECT_EQ(congestion_control_metadata.nic_congestion_window,
            Gen1RateUpdateEngine::kDefaultNicCongestionWindow);
  EXPECT_EQ(congestion_control_metadata.last_rue_event_time,
            -absl::InfiniteDuration());
  EXPECT_EQ(congestion_control_metadata.num_acked, 0);
  EXPECT_EQ(congestion_control_metadata.outstanding_rue_event, false);
}

// Tests that the delay_state is properly initialized with Swift depending on
// whether PLB is enabled or not.
TEST(Gen1RateUpdateEngineStandaloneTests, InitializDelayState) {
  SimpleEnvironment env;
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(kFalconVersion);
  // Set Swift as the CC algorithm.
  config.mutable_rue()->set_algorithm("swift");
  // Disable PLB. Expect delay_state to be initialized to kDefaultDelayState.
  config.mutable_rue()->mutable_swift()->set_randomize_path(false);
  FalconModel falcon_no_plb(config, &env, /*stats_collector=*/nullptr,
                            ConnectionManager::GetConnectionManager(),
                            "falcon-host",
                            /* number of hosts */ 4);
  auto metadata_no_plb =
      FalconTestingHelpers::InitializeConnectionMetadata(&falcon_no_plb, 1);
  ConnectionState* connection_state_no_plb =
      FalconTestingHelpers::InitializeConnectionState(
          &falcon_no_plb, std::move(metadata_no_plb));
  EXPECT_EQ(connection_state_no_plb->congestion_control_metadata->delay_state,
            falcon_no_plb.get_rate_update_engine()->ToFalconTimeUnits(
                ProtocolRateUpdateEngine::kDefaultDelayState));
  // Enable PLB. Expect delay_state to be initialized to 0.
  config.mutable_rue()->mutable_swift()->set_randomize_path(true);
  FalconModel falcon_plb(config, &env, /*stats_collector=*/nullptr,
                         ConnectionManager::GetConnectionManager(),
                         "falcon-host",
                         /* number of hosts */ 4);
  auto metadata_plb =
      FalconTestingHelpers::InitializeConnectionMetadata(&falcon_plb, 1);
  ConnectionState* connection_state_plb =
      FalconTestingHelpers::InitializeConnectionState(&falcon_plb,
                                                      std::move(metadata_plb));
  EXPECT_EQ(connection_state_plb->congestion_control_metadata->delay_state, 0);
}

// This defines all the objects needed for setup and testing
class Gen1RateUpdateEngineTest : public FalconTestingHelpers::FalconTestSetup,
                                 public testing::Test {
 protected:
  void SetUp() override {
    FalconConfig config =
        DefaultConfigGenerator::DefaultFalconConfig(kFalconVersion);
    InitFalcon(config);
    rue_ = falcon_->get_rate_update_engine();

    auto metadata_1 = FalconTestingHelpers::InitializeConnectionMetadata(
        falcon_.get(), kCid1);
    connection_state_1_ = FalconTestingHelpers::InitializeConnectionState(
        falcon_.get(), std::move(metadata_1));
  }
  ConnectionState* connection_state_1_;
  RateUpdateEngine* rue_;
};

// Tests the RUE's default constructed values as well as tests the
// flow label randomization.
TEST_F(Gen1RateUpdateEngineTest, InitializeMetadataDirect) {
  absl::flat_hash_map<uint32_t, uint32_t> flow_label_counts;
  constexpr int kTestRounds = 100000;
  static_assert(kTestRounds <= 1 << kIpv6FlowLabelNumBits);
  for (int test = 0; test < kTestRounds; test++) {
    CongestionControlMetadata& congestion_control_metadata =
        *connection_state_1_->congestion_control_metadata;
    rue_->InitializeMetadata(congestion_control_metadata);

    CheckDefaultMetadata(congestion_control_metadata);
    EXPECT_EQ(congestion_control_metadata.retransmit_timeout,
              rue_->FromFalconTimeUnits(rue_->ToFalconTimeUnits(
                  Gen1RateUpdateEngine::kDefaultRetransmitTimeout)));
    flow_label_counts[congestion_control_metadata.flow_label] += 1;
  }

  // Verifies that the flow label assignment randomly distributes the values
  // within 5% accuracy.
  EXPECT_LE(flow_label_counts.size(), kTestRounds);
  EXPECT_GT(flow_label_counts.size(), static_cast<int>(kTestRounds * 0.95));
}

// Tests that the connect state manager utilizes the RUE in the
// initialization of congestion control metadata.
TEST_F(Gen1RateUpdateEngineTest, InitializeMetadataIndirect) {
  CongestionControlMetadata& congestion_control_metadata =
      *connection_state_1_->congestion_control_metadata;
  CheckDefaultMetadata(congestion_control_metadata);
  EXPECT_EQ(congestion_control_metadata.retransmit_timeout,
            rue_->FromFalconTimeUnits(rue_->ToFalconTimeUnits(
                Gen1RateUpdateEngine::kDefaultRetransmitTimeout)));
}
}  // namespace
}  // namespace isekai
