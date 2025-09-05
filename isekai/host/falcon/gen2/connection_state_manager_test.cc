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

#include "isekai/host/falcon/gen2/connection_state_manager.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/default_config_generator.h"
#include "isekai/common/simple_environment.h"
#include "isekai/common/testing.h"
#include "isekai/host/falcon/falcon_connection_state.h"
#include "isekai/host/falcon/falcon_testing_helpers.h"
#include "isekai/host/falcon/gen1/falcon_model.h"
#include "isekai/host/falcon/gen2/falcon_model.h"
#include "isekai/host/falcon/gen2/falcon_types.h"
#include "isekai/host/rnic/connection_manager.h"

namespace isekai {

namespace {

constexpr uint32_t kCid1 = 123;

class Gen2ConnectionStateManagerTest
    : public FalconTestingHelpers::FalconTestSetup,
      public ::testing::TestWithParam</*falcon_version*/ int> {
 protected:
  int GetFalconVersion() { return GetParam(); }
};

INSTANTIATE_TEST_SUITE_P(
    ConnectionStateManagerTest, Gen2ConnectionStateManagerTest,
    /*version=*/testing::Values(2),
    [](const testing::TestParamInfo<Gen2ConnectionStateManagerTest::ParamType>&
           info) {
      const int version = info.param;
      return std::to_string(version);
    });

// Tests that the connection state manager only accepts 1 or 4 flows as degree
// of multipathing for a connection.
TEST_P(Gen2ConnectionStateManagerTest, ConnectionStateDegreeOfMultipathing) {
  // Setup FalconModel.
  SimpleEnvironment env;
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
  std::unique_ptr<FalconModel> falcon;
  if (GetFalconVersion() >= 2) {
    falcon = std::make_unique<Gen2FalconModel>(
        config, &env, /*stats_collector=*/nullptr,
        ConnectionManager::GetConnectionManager(), "falcon-host",
        /* number of hosts */ 4);
  } else {
    LOG(FATAL) << "Unsupported Falcon version: " << GetFalconVersion();
  }

  // Initialize a connection with a valid degree_of_multipathing (1 or 4).
  auto metadata_allowed_1 =
      FalconTestingHelpers::InitializeConnectionMetadata(falcon.get(), kCid1);
  auto gen2_metadata_allowed_1 =
      dynamic_cast<Gen2ConnectionMetadata*>(metadata_allowed_1.get());
  gen2_metadata_allowed_1->degree_of_multipathing = 1;
  EXPECT_OK(falcon->get_state_manager()->InitializeConnectionState(
      std::move(metadata_allowed_1)));

  auto metadata_allowed_4 = FalconTestingHelpers::InitializeConnectionMetadata(
      falcon.get(), kCid1 + 1);
  auto gen2_metadata_allowed_4 =
      dynamic_cast<Gen2ConnectionMetadata*>(metadata_allowed_4.get());
  gen2_metadata_allowed_4->degree_of_multipathing = 4;
  EXPECT_OK(falcon->get_state_manager()->InitializeConnectionState(
      std::move(metadata_allowed_4)));

  // Initialize a connection with an invalid degree_of_multipathing (2).
  auto metadata_disallowed = FalconTestingHelpers::InitializeConnectionMetadata(
      falcon.get(), kCid1 + 2);
  auto gen2_metadata_disallowed =
      dynamic_cast<Gen2ConnectionMetadata*>(metadata_disallowed.get());
  gen2_metadata_disallowed->degree_of_multipathing = 2;
  EXPECT_EQ(falcon->get_state_manager()
                ->InitializeConnectionState(std::move(metadata_disallowed))
                .code(),
            absl::StatusCode::kInvalidArgument);
}

}  // namespace

}  // namespace isekai
