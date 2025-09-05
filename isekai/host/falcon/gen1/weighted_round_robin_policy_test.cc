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

#include "isekai/host/falcon/gen1/weighted_round_robin_policy.h"

#include <utility>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace isekai {
namespace {

using testing::status::IsOkAndHolds;
using testing::status::StatusIs;

TEST(WeightedRoundRobinPolicyTest, Initialization) {
  WeightedRoundRobinPolicy<int> policy(nullptr);

  EXPECT_OK(policy.InitializeEntity(1));
  policy.SanityCheckForTesting();

  const WeightedRoundRobinPolicy<int>::EntityMetadata& metadata =
      policy.GetEntityMetadataForTesting(1);
  EXPECT_EQ(metadata.state, WeightedRoundRobinPolicy<int>::EntityState::kIdle);
  policy.SanityCheckForTesting();

  EXPECT_THAT(policy.InitializeEntity(1),
              StatusIs(absl::StatusCode::kInvalidArgument));

  EXPECT_OK(policy.InitializeEntity(2));
  policy.SanityCheckForTesting();

  const WeightedRoundRobinPolicy<int>::EntityMetadata& metadata2 =
      policy.GetEntityMetadataForTesting(2);
  EXPECT_EQ(metadata2.state, WeightedRoundRobinPolicy<int>::EntityState::kIdle);
  policy.SanityCheckForTesting();
}

TEST(WeightedRoundRobinPolicyTest, MarkActiveIdleTransitions) {
  WeightedRoundRobinPolicy<int> policy(nullptr);
  EXPECT_OK(policy.InitializeEntity(1));
  EXPECT_OK(policy.InitializeEntity(2));
  policy.SanityCheckForTesting();

  // Idle to Idle transition.
  EXPECT_FALSE(policy.MarkEntityInactive(1));
  const WeightedRoundRobinPolicy<int>::EntityMetadata& metadata1 =
      policy.GetEntityMetadataForTesting(1);
  EXPECT_EQ(metadata1.state, WeightedRoundRobinPolicy<int>::EntityState::kIdle);
  policy.SanityCheckForTesting();

  // Idle to Eligible transition.
  EXPECT_TRUE(policy.MarkEntityActive(1));
  const WeightedRoundRobinPolicy<int>::EntityMetadata& metadata2 =
      policy.GetEntityMetadataForTesting(1);
  EXPECT_EQ(metadata2.state,
            WeightedRoundRobinPolicy<int>::EntityState::kEligible);
  policy.SanityCheckForTesting();

  // Eligible to Eligible transition.
  EXPECT_FALSE(policy.MarkEntityActive(1));
  const WeightedRoundRobinPolicy<int>::EntityMetadata& metadata3 =
      policy.GetEntityMetadataForTesting(1);
  EXPECT_EQ(metadata3.state,
            WeightedRoundRobinPolicy<int>::EntityState::kEligible);
  policy.SanityCheckForTesting();

  // Eligible to Idle transition.
  EXPECT_TRUE(policy.MarkEntityInactive(1));
  const WeightedRoundRobinPolicy<int>::EntityMetadata& metadata4 =
      policy.GetEntityMetadataForTesting(1);
  EXPECT_EQ(metadata4.state, WeightedRoundRobinPolicy<int>::EntityState::kIdle);
  policy.SanityCheckForTesting();

  // Eligible to Waiting transition.
  EXPECT_TRUE(policy.MarkEntityActive(1));
  EXPECT_THAT(policy.GetNextEntity(), IsOkAndHolds(1));  // Processed.

  const WeightedRoundRobinPolicy<int>::EntityMetadata& metadata5 =
      policy.GetEntityMetadataForTesting(1);
  EXPECT_EQ(metadata5.state,
            WeightedRoundRobinPolicy<int>::EntityState::kWaiting);
  policy.SanityCheckForTesting();

  // Waiting to Waiting transition.
  EXPECT_FALSE(policy.MarkEntityActive(1));
  const WeightedRoundRobinPolicy<int>::EntityMetadata& metadata6 =
      policy.GetEntityMetadataForTesting(1);
  EXPECT_EQ(metadata6.state,
            WeightedRoundRobinPolicy<int>::EntityState::kWaiting);
  policy.SanityCheckForTesting();

  // Waiting to WaitingGoingIdle transition.
  EXPECT_TRUE(policy.MarkEntityInactive(1));
  const WeightedRoundRobinPolicy<int>::EntityMetadata& metadata7 =
      policy.GetEntityMetadataForTesting(1);
  EXPECT_EQ(metadata7.state,
            WeightedRoundRobinPolicy<int>::EntityState::kWaitingGoingIdle);
  policy.SanityCheckForTesting();

  // WaitingGoingIdle to WaitingGoingIdle transition.
  EXPECT_FALSE(policy.MarkEntityInactive(1));
  const WeightedRoundRobinPolicy<int>::EntityMetadata& metadata8 =
      policy.GetEntityMetadataForTesting(1);
  EXPECT_EQ(metadata8.state,
            WeightedRoundRobinPolicy<int>::EntityState::kWaitingGoingIdle);
  policy.SanityCheckForTesting();

  // WaitingGoingIdle to Idle transition.
  EXPECT_TRUE(policy.MarkEntityActive(2));
  EXPECT_THAT(policy.GetNextEntity(), IsOkAndHolds(2));  // Processed.
  EXPECT_THAT(policy.GetNextEntity(), IsOkAndHolds(2));  // New round.

  const WeightedRoundRobinPolicy<int>::EntityMetadata& metadata9 =
      policy.GetEntityMetadataForTesting(1);
  EXPECT_EQ(metadata9.state, WeightedRoundRobinPolicy<int>::EntityState::kIdle);
  policy.SanityCheckForTesting();
}

TEST(WeightedRoundRobinPolicyTest, VanillaRoundRobin) {
  for (bool batch : {false, true}) {
    WeightedRoundRobinPolicy<int> policy(nullptr, batch);
    for (int i = 1; i <= 5; i++) {
      EXPECT_OK(policy.InitializeEntity(i));
    }

    // None of the entities are active.
    EXPECT_THAT(policy.GetNextEntity(),
                StatusIs(absl::StatusCode::kUnavailable));
    policy.SanityCheckForTesting();

    // Mark selected entities active.
    EXPECT_TRUE(policy.MarkEntityActive(1));
    EXPECT_TRUE(policy.MarkEntityActive(3));
    EXPECT_TRUE(policy.MarkEntityActive(5));
    policy.SanityCheckForTesting();

    // The first two entities in the order they were marked active.
    EXPECT_THAT(policy.GetNextEntity(), IsOkAndHolds(1));
    EXPECT_THAT(policy.GetNextEntity(), IsOkAndHolds(3));
    policy.SanityCheckForTesting();

    // Mark another entity active.
    EXPECT_TRUE(policy.MarkEntityActive(4));
    policy.SanityCheckForTesting();

    // 4 comes before 5 as the HW scans things in sorted order.
    EXPECT_THAT(policy.GetNextEntity(), IsOkAndHolds(4));
    EXPECT_THAT(policy.GetNextEntity(), IsOkAndHolds(5));
    policy.SanityCheckForTesting();

    // Round has finished, so first entity is scheduled again.
    EXPECT_THAT(policy.GetNextEntity(), IsOkAndHolds(1));
    policy.SanityCheckForTesting();

    // Remove the next active entity, the one after should be scheduled.
    EXPECT_TRUE(policy.MarkEntityInactive(3));
    EXPECT_THAT(policy.GetNextEntity(), IsOkAndHolds(4));
    EXPECT_THAT(policy.GetNextEntity(), IsOkAndHolds(5));
    policy.SanityCheckForTesting();

    // At this point, active list should be empty. Mark an idle entity as
    // active.
    EXPECT_TRUE(policy.MarkEntityActive(2));
    policy.SanityCheckForTesting();

    // The newly marked entity should be scheduled next to maintain round-robin.
    EXPECT_THAT(policy.GetNextEntity(), IsOkAndHolds(2));
    EXPECT_THAT(policy.GetNextEntity(), IsOkAndHolds(1));
    policy.SanityCheckForTesting();
  }
}

TEST(WeightedRoundRobinPolicyTest, WeightedRoundRobin) {
  absl::flat_hash_map<int, int> kWeights({{1, 1}, {2, 2}, {3, 3}});
  WeightedRoundRobinPolicy<int>::WeightFetcher weight_fetcher =
      [&kWeights](int entity) -> int { return kWeights[entity]; };

  WeightedRoundRobinPolicy<int> policy(std::move(weight_fetcher));

  for (int i = 1; i <= 3; ++i) {
    EXPECT_OK(policy.InitializeEntity(i));
    EXPECT_TRUE(policy.MarkEntityActive(i));
  }
  policy.SanityCheckForTesting();

  // Run for 3 rounds.
  for (int i = 0; i < 3; ++i) {
    EXPECT_THAT(policy.GetNextEntity(), IsOkAndHolds(1));
    policy.SanityCheckForTesting();
    EXPECT_THAT(policy.GetNextEntity(), IsOkAndHolds(2));
    EXPECT_THAT(policy.GetNextEntity(), IsOkAndHolds(3));
    policy.SanityCheckForTesting();

    EXPECT_THAT(policy.GetNextEntity(), IsOkAndHolds(2));
    EXPECT_THAT(policy.GetNextEntity(), IsOkAndHolds(3));

    EXPECT_THAT(policy.GetNextEntity(), IsOkAndHolds(3));
    policy.SanityCheckForTesting();
  }

  // Update the weights, and test zero weight as well.
  kWeights[1] = 3;
  kWeights[2] = 2;
  kWeights[3] = 0;

  // Run for 3 rounds.
  for (int i = 0; i < 3; ++i) {
    EXPECT_THAT(policy.GetNextEntity(), IsOkAndHolds(1));
    EXPECT_THAT(policy.GetNextEntity(), IsOkAndHolds(2));

    EXPECT_THAT(policy.GetNextEntity(), IsOkAndHolds(1));
    policy.SanityCheckForTesting();
    EXPECT_THAT(policy.GetNextEntity(), IsOkAndHolds(2));

    EXPECT_THAT(policy.GetNextEntity(), IsOkAndHolds(1));
    policy.SanityCheckForTesting();
  }

  // Update the weights back to non-zero, and test zero weight as well.
  kWeights[1] = 1;
  kWeights[2] = 1;
  kWeights[3] = 1;

  // Run for 3 rounds.
  for (int i = 0; i < 3; ++i) {
    EXPECT_THAT(policy.GetNextEntity(), IsOkAndHolds(1));
    policy.SanityCheckForTesting();
    EXPECT_THAT(policy.GetNextEntity(), IsOkAndHolds(2));
    policy.SanityCheckForTesting();
    EXPECT_THAT(policy.GetNextEntity(), IsOkAndHolds(3));
    policy.SanityCheckForTesting();
  }
}

TEST(WeightedRoundRobinPolicyTest, WeightedRoundRobinBatch) {
  absl::flat_hash_map<int, int> kWeights({{1, 2}, {2, 1}, {3, 3}});
  WeightedRoundRobinPolicy<int>::WeightFetcher weight_fetcher =
      [&kWeights](int entity) -> int { return kWeights[entity]; };

  WeightedRoundRobinPolicy<int> policy(std::move(weight_fetcher), true);

  for (int i = 1; i <= 3; ++i) {
    EXPECT_OK(policy.InitializeEntity(i));
    EXPECT_TRUE(policy.MarkEntityActive(i));
  }
  policy.SanityCheckForTesting();

  // Run for 3 rounds.
  for (int i = 0; i < 3; ++i) {
    EXPECT_THAT(policy.GetNextEntity(), IsOkAndHolds(1));
    EXPECT_THAT(policy.GetNextEntity(), IsOkAndHolds(1));
    policy.SanityCheckForTesting();
    EXPECT_THAT(policy.GetNextEntity(), IsOkAndHolds(2));
    policy.SanityCheckForTesting();
    EXPECT_THAT(policy.GetNextEntity(), IsOkAndHolds(3));
    EXPECT_THAT(policy.GetNextEntity(), IsOkAndHolds(3));
    EXPECT_THAT(policy.GetNextEntity(), IsOkAndHolds(3));
    policy.SanityCheckForTesting();
  }

  // Update the weights, and test zero weight as well.
  kWeights[1] = 3;
  kWeights[2] = 0;
  kWeights[3] = 2;

  // Run for 3 rounds.
  for (int i = 0; i < 3; ++i) {
    EXPECT_THAT(policy.GetNextEntity(), IsOkAndHolds(1));
    EXPECT_THAT(policy.GetNextEntity(), IsOkAndHolds(1));
    EXPECT_THAT(policy.GetNextEntity(), IsOkAndHolds(1));
    policy.SanityCheckForTesting();
    EXPECT_THAT(policy.GetNextEntity(), IsOkAndHolds(3));
    EXPECT_THAT(policy.GetNextEntity(), IsOkAndHolds(3));
    policy.SanityCheckForTesting();
  }

  // Update the weights back to non-zero, and test zero weight as well.
  kWeights[1] = 1;
  kWeights[2] = 1;
  kWeights[3] = 1;

  // Run for 3 rounds.
  for (int i = 0; i < 3; ++i) {
    EXPECT_THAT(policy.GetNextEntity(), IsOkAndHolds(1));
    policy.SanityCheckForTesting();
    EXPECT_THAT(policy.GetNextEntity(), IsOkAndHolds(2));
    policy.SanityCheckForTesting();
    EXPECT_THAT(policy.GetNextEntity(), IsOkAndHolds(3));
    policy.SanityCheckForTesting();
  }
}

TEST(WeightedRoundRobinPolicyTest, TestHasWork) {
  absl::flat_hash_map<int, int> kWeights({{1, 1}, {2, 2}});
  WeightedRoundRobinPolicy<int>::WeightFetcher weight_fetcher =
      [&kWeights](int entity) -> int { return kWeights[entity]; };

  WeightedRoundRobinPolicy<int> policy(std::move(weight_fetcher));

  for (int i = 1; i <= 2; ++i) {
    EXPECT_OK(policy.InitializeEntity(i));
    EXPECT_TRUE(policy.MarkEntityActive(i));
  }
  policy.SanityCheckForTesting();

  EXPECT_TRUE(policy.HasWork());
  policy.SanityCheckForTesting();

  EXPECT_THAT(policy.GetNextEntity(), IsOkAndHolds(1));
  EXPECT_THAT(policy.GetNextEntity(), IsOkAndHolds(2));
  policy.SanityCheckForTesting();

  EXPECT_TRUE(policy.HasWork());

  EXPECT_THAT(policy.GetNextEntity(), IsOkAndHolds(2));
  policy.SanityCheckForTesting();

  // Update the weights to zero.
  kWeights[1] = 0;
  kWeights[2] = 0;

  policy.SanityCheckForTesting();
  EXPECT_FALSE(policy.HasWork());
  policy.SanityCheckForTesting();

  // Update the weights to non-zero.
  kWeights[1] = 1;
  kWeights[2] = 0;

  EXPECT_TRUE(policy.HasWork());
  policy.SanityCheckForTesting();
}

TEST(WeightedRoundRobinPolicyTest, ActiveIdleDuringRound) {
  absl::flat_hash_map<int, int> kWeights({{1, 1}, {2, 2}, {3, 3}});
  WeightedRoundRobinPolicy<int>::WeightFetcher weight_fetcher =
      [&kWeights](int entity) -> int { return kWeights[entity]; };

  WeightedRoundRobinPolicy<int> policy(std::move(weight_fetcher));

  for (int i = 1; i <= 3; ++i) {
    EXPECT_OK(policy.InitializeEntity(i));
  }
  EXPECT_TRUE(policy.MarkEntityActive(1));
  EXPECT_TRUE(policy.MarkEntityActive(3));
  policy.SanityCheckForTesting();

  EXPECT_THAT(policy.GetNextEntity(), IsOkAndHolds(1));
  EXPECT_TRUE(policy.MarkEntityActive(2));
  EXPECT_THAT(policy.GetNextEntity(), IsOkAndHolds(2));
  EXPECT_THAT(policy.GetNextEntity(), IsOkAndHolds(3));
  policy.SanityCheckForTesting();

  // This should not have any effect as 1 has exhausted it's credits.
  EXPECT_TRUE(policy.MarkEntityInactive(1));
  policy.SanityCheckForTesting();
  EXPECT_TRUE(policy.MarkEntityActive(1));
  policy.SanityCheckForTesting();

  EXPECT_THAT(policy.GetNextEntity(), IsOkAndHolds(2));
  EXPECT_THAT(policy.GetNextEntity(), IsOkAndHolds(3));

  // Marking 3 as idle and then active again should make it eligible.
  EXPECT_TRUE(policy.MarkEntityInactive(3));
  policy.SanityCheckForTesting();
  EXPECT_TRUE(policy.MarkEntityActive(3));
  policy.SanityCheckForTesting();

  EXPECT_THAT(policy.GetNextEntity(), IsOkAndHolds(3));
  policy.SanityCheckForTesting();
}

TEST(WeightedRoundRobinPolicyTest, DeleteEntity) {
  absl::flat_hash_map<int, int> kWeights({{1, 1}, {2, 2}, {3, 3}});
  WeightedRoundRobinPolicy<int>::WeightFetcher weight_fetcher =
      [&kWeights](int entity) -> int { return kWeights[entity]; };

  WeightedRoundRobinPolicy<int> policy(std::move(weight_fetcher));

  for (int i = 1; i <= 3; ++i) {
    EXPECT_OK(policy.InitializeEntity(i));
  }
  EXPECT_OK(policy.InitializeEntity(4));
  EXPECT_OK(policy.DeleteEntity(4));

  EXPECT_TRUE(policy.MarkEntityActive(1));
  EXPECT_TRUE(policy.MarkEntityActive(3));
  policy.SanityCheckForTesting();

  EXPECT_THAT(policy.GetNextEntity(), IsOkAndHolds(1));
  EXPECT_TRUE(policy.MarkEntityActive(2));
  EXPECT_OK(policy.DeleteEntity(3));
  EXPECT_THAT(policy.GetNextEntity(), IsOkAndHolds(2));
  policy.SanityCheckForTesting();

  // This should not have any effect as 1 has exhausted it's credits.
  EXPECT_TRUE(policy.MarkEntityInactive(1));
  policy.SanityCheckForTesting();
  EXPECT_TRUE(policy.MarkEntityActive(1));
  policy.SanityCheckForTesting();
  EXPECT_OK(policy.DeleteEntity(1));
  policy.SanityCheckForTesting();

  EXPECT_THAT(policy.GetNextEntity(), IsOkAndHolds(2));

  EXPECT_THAT(policy.DeleteEntity(3),
              StatusIs(absl::StatusCode::kInvalidArgument));
  policy.SanityCheckForTesting();
}

TEST(WeightedRoundRobinPolicyTest, EnforceOrder) {
  absl::flat_hash_map<int, int> kWeights({{1, 1}, {2, 2}, {3, 3}});
  WeightedRoundRobinPolicy<int>::WeightFetcher weight_fetcher =
      [&kWeights](int entity) -> int { return kWeights[entity]; };

  WeightedRoundRobinPolicy<int> policy(
      std::move(weight_fetcher), /*batched=*/false, /*enforce_order=*/true);

  for (int i = 1; i <= 3; ++i) {
    EXPECT_OK(policy.InitializeEntity(i));
  }

  EXPECT_TRUE(policy.MarkEntityActive(1));
  EXPECT_TRUE(policy.MarkEntityActive(2));
  EXPECT_TRUE(policy.MarkEntityActive(3));
  policy.SanityCheckForTesting();

  EXPECT_TRUE(policy.HasWork());

  EXPECT_THAT(policy.GetNextEntity(), IsOkAndHolds(1));
  policy.SanityCheckForTesting();

  EXPECT_TRUE(policy.MarkEntityInactive(1));
  EXPECT_TRUE(policy.MarkEntityInactive(2));
  EXPECT_TRUE(policy.MarkEntityInactive(3));
  policy.SanityCheckForTesting();

  EXPECT_FALSE(policy.HasWork());
  policy.SanityCheckForTesting();

  EXPECT_TRUE(policy.MarkEntityActive(1));
  EXPECT_TRUE(policy.MarkEntityActive(2));
  EXPECT_TRUE(policy.MarkEntityActive(3));
  policy.SanityCheckForTesting();

  // Even though a new round has started, the policy should remember to schedule
  // the next entity, instead of scheduling 1 again.
  EXPECT_THAT(policy.GetNextEntity(), IsOkAndHolds(2));
  policy.SanityCheckForTesting();

  EXPECT_THAT(policy.GetNextEntity(), IsOkAndHolds(3));
  policy.SanityCheckForTesting();

  EXPECT_THAT(policy.GetNextEntity(), IsOkAndHolds(1));
  policy.SanityCheckForTesting();
}

}  // namespace
}  // namespace isekai
