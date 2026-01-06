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

#include <utility>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/constants.h"
#include "isekai/common/default_config_generator.h"
#include "isekai/common/testing.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/falcon_testing_helpers.h"

constexpr int kScid = 1;
constexpr int kFalconGen3 = 3;

namespace isekai {
namespace {

// This defines all the objects needed for setup and testing. The test fixture
// is parameterized on the Falcon version.
class Gen3ConnectionStateManagerTest
    : public FalconTestingHelpers::FalconTestSetup,
      public ::testing::Test {};

TEST_F(Gen3ConnectionStateManagerTest, Gen3AlternateWindowBitmapSize512) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(kFalconGen3);
  config.mutable_gen3_config_options()->set_rx_window_size(kGen3RxBitmapWidth);
  InitFalcon(config);
  auto connection_metadata =
      FalconTestingHelpers::InitializeConnectionMetadata(falcon_.get(), kScid);
  ASSERT_OK(connection_state_manager_->InitializeConnectionState(
      std::move(connection_metadata)));

  ASSERT_OK_AND_ASSIGN(auto connection_state,
                       connection_state_manager_->PerformDirectLookup(kScid));
  EXPECT_EQ(connection_state->rx_reliability_metadata.request_window_metadata
                .ack_window->Size(),
            kGen3RxBitmapWidth);
  EXPECT_EQ(connection_state->rx_reliability_metadata.request_window_metadata
                .receive_window->Size(),
            kGen3RxBitmapWidth);
  EXPECT_EQ(connection_state->rx_reliability_metadata.data_window_metadata
                .ack_window->Size(),
            kGen3RxBitmapWidth);
  EXPECT_EQ(connection_state->rx_reliability_metadata.data_window_metadata
                .receive_window->Size(),
            kGen3RxBitmapWidth);
}

TEST_F(Gen3ConnectionStateManagerTest, Gen3DefaultWindowBitmapSize256) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(kFalconGen3);
  InitFalcon(config);
  auto connection_metadata =
      FalconTestingHelpers::InitializeConnectionMetadata(falcon_.get(), kScid);
  ASSERT_OK(connection_state_manager_->InitializeConnectionState(
      std::move(connection_metadata)));

  ASSERT_OK_AND_ASSIGN(auto connection_state,
                       connection_state_manager_->PerformDirectLookup(kScid));
  EXPECT_EQ(connection_state->rx_reliability_metadata.request_window_metadata
                .ack_window->Size(),
            kGen2RxBitmapWidth);
  EXPECT_EQ(connection_state->rx_reliability_metadata.request_window_metadata
                .receive_window->Size(),
            kGen2RxBitmapWidth);
  EXPECT_EQ(connection_state->rx_reliability_metadata.data_window_metadata
                .ack_window->Size(),
            kGen2RxBitmapWidth);
  EXPECT_EQ(connection_state->rx_reliability_metadata.data_window_metadata
                .receive_window->Size(),
            kGen2RxBitmapWidth);
}

}  // namespace
}  // namespace isekai
