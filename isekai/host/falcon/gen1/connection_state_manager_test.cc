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

#include "isekai/host/falcon/gen1/connection_state_manager.h"

#include <cstdint>
#include <functional>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/time/time.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "isekai/common/common_util.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/default_config_generator.h"
#include "isekai/common/simple_environment.h"
#include "isekai/common/testing.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/falcon_connection_state.h"
#include "isekai/host/falcon/falcon_testing_helpers.h"
#include "isekai/host/falcon/gen1/falcon_utils.h"
#include "isekai/host/rnic/connection_manager.h"

constexpr int kScid = 1;

namespace isekai {
namespace {

// This defines all the objects needed for setup and testing. The test fixture
// is parameterized on the Falcon version.
class FalconConnectionStateManagerTest
    : public FalconTestingHelpers::FalconTestSetup,
      public ::testing::TestWithParam<int /* version */> {
 protected:
  int GetFalconVersion() { return GetParam(); }
};

INSTANTIATE_TEST_SUITE_P(
    ConnectionStateManagerTest, FalconConnectionStateManagerTest,
    /*version=*/testing::Values(1, 2),
    [](const testing::TestParamInfo<
        FalconConnectionStateManagerTest::ParamType>& info) {
      const int version = static_cast<int>(info.param);
      return absl::StrCat("Gen", version);
    });

TEST_P(FalconConnectionStateManagerTest, ConnectionInitialization) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
  InitFalcon(config);

  auto connection_metadata =
      FalconTestingHelpers::InitializeConnectionMetadata(falcon_.get());
  EXPECT_OK(connection_state_manager_->InitializeConnectionState(
      std::move(connection_metadata)));
}

TEST_P(FalconConnectionStateManagerTest, DuplicateConnectionInitialization) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
  InitFalcon(config);
  auto connection_metadata =
      FalconTestingHelpers::InitializeConnectionMetadata(falcon_.get());
  EXPECT_OK(connection_state_manager_->InitializeConnectionState(
      std::move(connection_metadata)));
  auto duplicate_connection_metadata =
      FalconTestingHelpers::InitializeConnectionMetadata(falcon_.get());
  EXPECT_EQ(
      connection_state_manager_
          ->InitializeConnectionState(std::move(duplicate_connection_metadata))
          .code(),
      absl::StatusCode::kAlreadyExists);
}

TEST_P(FalconConnectionStateManagerTest, ValidPerformDirectLookup) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
  InitFalcon(config);
  auto connection_metadata =
      FalconTestingHelpers::InitializeConnectionMetadata(falcon_.get(), kScid);
  EXPECT_OK(connection_state_manager_->InitializeConnectionState(
      std::move(connection_metadata)));
  EXPECT_EQ(
      connection_state_manager_->PerformDirectLookup(kScid).status().code(),
      absl::StatusCode::kOk);
}

TEST_P(FalconConnectionStateManagerTest, InvalidPerformDirectLookup) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
  InitFalcon(config);

  EXPECT_EQ(
      connection_state_manager_->PerformDirectLookup(kScid).status().code(),
      absl::StatusCode::kNotFound);
}

}  // namespace
}  // namespace isekai
