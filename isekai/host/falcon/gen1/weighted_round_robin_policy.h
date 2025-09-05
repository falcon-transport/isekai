/*
 * Copyright 2024 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ISEKAI_HOST_FALCON_WEIGHTED_ROUND_ROBIN_POLICY_H_
#define ISEKAI_HOST_FALCON_WEIGHTED_ROUND_ROBIN_POLICY_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

#include "absl/container/btree_set.h"
#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "glog/logging.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/gen1/connection_scheduler_types.h"

namespace isekai {

// This class implements a deficit-based weighted round-robin algorithm over
// multiple entities. It proceeds over multiple rounds with each round
// consisting of sub-rounds.

// At the beginning of each round, each entity is initialized with credit equal
// to the configured weight and the total global credits (for the round) is
// initialized to the sum of the weights. In each sub-round, one credit of work
// is done from each active entity, and sub-rounds continue until the round
// ends.
// A round ends if either of the following happens,
// - None of the entities are active, or
// - Total global credits becomes 0.

// The user of the policy is allowed to mark an entity as active or inactive at
// any point during the execution of the policy. An active entity is one that
// can perform a unit of work (e.g., meets all the gating criteria), whereas an
// inactive entity cannot perform work (e.g. gated by cwnd, solicitation window
// or op rate).

// The Entity type <T> must be hashable by Abseil and have comparison operators
// defined for ordering.
template <typename T>
class WeightedRoundRobinPolicy : public PathSelectionPolicy<T> {
 public:
  // A function that the policy can call to update entity weights.
  using WeightFetcher = std::function<int(const T&)>;

  // List of possible states for an entity. Comments describe when an entity
  // resides in the given state.
  enum class EntityState {
    kIdle,  // Entity that is marked inactive, and may or may not have credits
            // for the current round.
    kEligible,  // Entity that is active, AND has credits for the current round.
    kWaiting,   // Entity that is active, but has been serviced in the current
                // sub-round or has no credits remaining for the current round.
    kWaitingGoingIdle,  // Entity that was serviced in the current sub-round,
                        // but is now marked inactive.
  };

  struct EntityMetadata {
    EntityState state;
    // Weight assigned to this entity.
    int weight;
    // Remaining credits while a round is in progress.
    int remaining_credits;
    // The most recent round number in which this entity's credits were reset.
    int round_number;
  };

  // Constructor that takes in a WeightFetcher function and batched
  // configuration.
  // The policy will call this WeightFetcherfunction to get the
  // latest weight when starting a new round. If the fetcher is set to nullptr,
  // the policy uses the default weight of 1 for all entities, effectively
  // making this a vanilla round robin policy with equal weights.
  // As for the batched configuration, if set to true, all credits for an
  // eligible entity will be consumed before going over to the next entity. When
  // batched is false, a subround will return eligible entities with non zero
  // credits in a round-robin manner.
  // The enforce_order parameter makes the policy perform a "fairer" round robin
  // by remembering the last entity serviced, and resuming the next round from
  // the next entity, instead of the first entity.
  explicit WeightedRoundRobinPolicy(WeightFetcher fetcher, bool batched = false,
                                    bool enforce_order = false)
      : weight_fetcher_(std::move(fetcher)),
        batched_(batched),
        enforce_order_(enforce_order) {}

  // Initializes an entity in the WRR policy. The entity is marked as inactive,
  // and assigned a weight of 1 if no WeightFetcher function is provided, else
  // the weight is determined by calling the WeightFetcher.
  absl::Status InitializeEntity(const T& entity) override;

  // Removes an entity from the WRR policy and deletes all associated state.
  absl::Status DeleteEntity(const T& entity) override;

  // Returns the next entity to be serviced as dictated by the WRR policy. If
  // none of the entities are active, returns an error.
  absl::StatusOr<T> GetNextEntity() override;

  // Marks the given entity as active which implies that the entity is available
  // to perform work. Return true if the state was indeed changed from inactive
  // to active, else returns false.
  bool MarkEntityActive(const T& entity);

  // Marks the given entity as inactive which implies the entity is not
  // available to perform any work. Returns true if the state was indeed changed
  // from active to inactive, else returns false.
  bool MarkEntityInactive(const T& entity);

  // Returns if an entity has been marked active.
  bool IsActive(const T& entity) const {
    return eligible_.contains(entity) || waiting_.contains(entity);
  }

  // Returns true if the policy has an entity that can perform work. If
  // necessary, this call will start a new sub-round or round.
  bool HasWork() override;

  // Resets all entities in the policy.
  void Reset() { ResetAndRestartRound(); }

  // Methods only for testing.
  const EntityMetadata& GetEntityMetadataForTesting(const T& entity) const {
    return *metadata_.at(entity);
  }
  // Checks if all entities are in lists corresponding to their state.
  void SanityCheckForTesting();

 private:
  // Checks if the current round is over.
  bool IsRoundOver();
  // Starts a new sub-round.
  void StartSubRound();
  // Stops the current round, and resets all entities (including their credits),
  // and starts a new round.
  void ResetAndRestartRound();

  // List of entities based on their state. We use btree_sets to maintain order.
  absl::btree_set<T> idle_;
  absl::btree_set<T> eligible_;
  absl::btree_set<T> waiting_;
  absl::btree_set<T> waiting_going_idle_;

  // Metadata of all entities stored in a map.
  absl::flat_hash_map<T, std::unique_ptr<EntityMetadata>> metadata_;
  // Variables to keep track of how many rounds have elapsed and the amount of
  // credits remaining in the current round.
  int global_round_number_ = 0;
  int global_credits_remaining_ = 0;

  WeightFetcher weight_fetcher_ = nullptr;
  // If set to true, all credits for an eligible entity will be consumed before
  // going over to the next entity. If set to false, a subround will return
  // eligible entities with non zero credits in a round-robin manner.
  bool batched_;

  // If set to true, the policy remembers the last serviced entity and services
  // the next entity even across rounds.
  bool enforce_order_;
  T last_serviced_entity_;
};

////////////////////////////////////////////////////////////////////////////////

template <typename T>
absl::Status WeightedRoundRobinPolicy<T>::InitializeEntity(const T& entity) {
  std::unique_ptr<EntityMetadata>& entity_metadata = metadata_[entity];
  if (entity_metadata != nullptr) {
    return absl::InvalidArgumentError("Entity already exists in policy.");
  }

  idle_.insert(entity);
  entity_metadata = std::make_unique<EntityMetadata>();
  entity_metadata->state = EntityState::kIdle;
  if (weight_fetcher_) {
    entity_metadata->weight = weight_fetcher_(entity);
  } else {
    entity_metadata->weight = 1;
  }
  entity_metadata->remaining_credits = 0;
  entity_metadata->round_number = -1;
  last_serviced_entity_ = entity;
  return absl::OkStatus();
}

template <typename T>
absl::Status WeightedRoundRobinPolicy<T>::DeleteEntity(const T& entity) {
  auto entity_metadata_iter = metadata_.find(entity);
  if (entity_metadata_iter == metadata_.end()) {
    return absl::InvalidArgumentError("Entity does not exist in policy.");
  }

  EntityMetadata& entity_metadata = *entity_metadata_iter->second;
  switch (entity_metadata.state) {
    case EntityState::kIdle:
      idle_.erase(entity);
      break;
    case EntityState::kEligible:
      eligible_.erase(entity);
      break;
    case EntityState::kWaiting:
      waiting_.erase(entity);
      break;
    case EntityState::kWaitingGoingIdle:
      waiting_going_idle_.erase(entity);
      break;
  }
  metadata_.erase(entity_metadata_iter);
  return absl::OkStatus();
}

template <typename T>
absl::StatusOr<T> WeightedRoundRobinPolicy<T>::GetNextEntity() {
  if (eligible_.empty() && waiting_.empty()) {
    return absl::UnavailableError("No active entities in the policy.");
  }

  if (IsRoundOver()) {
    VLOG(2) << "Round is over. Starting a new one.";
    ResetAndRestartRound();
  }

  if (eligible_.empty()) {
    return absl::UnavailableError("No eligible entities in the policy.");
  }

  T next_entity = *eligible_.begin();
  // In enforce fair order is set, then we pick the entity after the last
  // serviced entity, instead of simply picking the first eligible entity. While
  // this is slighty more expensive (logN compared to constant time), it is
  // sometimes important to preserve this behavior.
  if (enforce_order_) {
    auto next_entity_iter = eligible_.upper_bound(last_serviced_entity_);
    if (next_entity_iter != eligible_.end()) {
      next_entity = *next_entity_iter;
    }
    last_serviced_entity_ = next_entity;
  }

  EntityMetadata& next_entity_metadata = *metadata_.at(next_entity);
  CHECK(next_entity_metadata.remaining_credits > 0)
      << "Trying to schedule an entity with zero remaining credits.";
  CHECK(global_credits_remaining_ > 0)
      << "Trying to schedule an entity when global remaining credits are zero.";
  --next_entity_metadata.remaining_credits;
  --global_credits_remaining_;

  if (!batched_ || next_entity_metadata.remaining_credits == 0) {
    eligible_.erase(next_entity);
    waiting_.insert(next_entity);
    next_entity_metadata.state = EntityState::kWaiting;
  }

  return next_entity;
}

template <typename T>
bool WeightedRoundRobinPolicy<T>::MarkEntityActive(const T& entity) {
  auto entity_metadata_iter = metadata_.find(entity);
  CHECK(entity_metadata_iter != metadata_.end());  // Crash OK.

  EntityMetadata& entity_metadata = *entity_metadata_iter->second;

  // If entity is Eligible or Waiting, there is nothing to do.
  if (entity_metadata.state == EntityState::kEligible ||
      entity_metadata.state == EntityState::kWaiting) {
    return false;
  }

  // If Idle, move to Eligible or Waiting depending on credits available.
  if (entity_metadata.state == EntityState::kIdle) {
    // Initialize credits if this is the first time this entity is marked
    // eligible for the current round.
    if (entity_metadata.round_number < global_round_number_) {
      entity_metadata.remaining_credits = entity_metadata.weight;
      entity_metadata.round_number = global_round_number_;
    }
    if (entity_metadata.remaining_credits == 0) {
      waiting_.insert(entity);
      entity_metadata.state = EntityState::kWaiting;
    } else {
      eligible_.insert(entity);
      entity_metadata.state = EntityState::kEligible;
    }
    idle_.erase(entity);
  } else {
    // If in WaitingGoingIdle state, send it back to Waiting.
    waiting_.insert(entity);
    entity_metadata.state = EntityState::kWaiting;
    waiting_going_idle_.erase(entity);
  }
  return true;
}

template <typename T>
bool WeightedRoundRobinPolicy<T>::MarkEntityInactive(const T& entity) {
  auto entity_metadata_iter = metadata_.find(entity);
  CHECK(entity_metadata_iter != metadata_.end());  // Crash OK.

  EntityMetadata& entity_metadata = *entity_metadata_iter->second;

  // If entity is Idle or GoingIdle, there is nothing to do.
  if (entity_metadata.state == EntityState::kIdle ||
      entity_metadata.state == EntityState::kWaitingGoingIdle) {
    return false;
  }

  // If Eligible, send to Idle. If Waiting, send to WaitingGoingIdle.
  if (entity_metadata.state == EntityState::kEligible) {
    idle_.insert(entity);
    entity_metadata.state = EntityState::kIdle;
    eligible_.erase(entity);
  } else if (entity_metadata.state == EntityState::kWaiting) {
    waiting_going_idle_.insert(entity);
    entity_metadata.state = EntityState::kWaitingGoingIdle;
    waiting_.erase(entity);
  }
  return true;
}

template <typename T>
bool WeightedRoundRobinPolicy<T>::HasWork() {
  // If the current round is not over, then we still have work to do. Else we
  // restart the round (to get the latest weights) and check the eligible list.
  if (IsRoundOver()) {
    ResetAndRestartRound();
    return !eligible_.empty();
  }
  return true;
}

template <typename T>
void WeightedRoundRobinPolicy<T>::ResetAndRestartRound() {
  // Fetch new weights for all entities if a WeightFetcher is provided.
  if (weight_fetcher_ != nullptr) {
    int weight_sum = 0;
    for (const auto& [entity, metadata] : metadata_) {
      metadata->weight = weight_fetcher_(entity);
      weight_sum += metadata->weight;
    }
    global_credits_remaining_ = weight_sum;
  } else {
    // Vanilla round-robin behavior with equal weights for all entities.
    global_credits_remaining_ = metadata_.size();
  }
  ++global_round_number_;

  // Update credits for all Eligible entities, if the weight is greater than 0.
  // If credit == 0, move them to Waiting list.
  for (auto it = eligible_.begin(); it != eligible_.end();) {
    EntityMetadata& metadata = *metadata_.at(*it);
    metadata.remaining_credits = metadata.weight;
    metadata.round_number = global_round_number_;
    if (metadata.weight <= 0) {
      metadata.state = EntityState::kWaiting;
      waiting_.insert(*it);
      it = eligible_.erase(it);
    } else {
      ++it;
    }
  }

  // Update the credits for all Waiting entities and move them to Eligible list
  // if credits are greater than 0.
  for (auto it = waiting_.begin(); it != waiting_.end();) {
    EntityMetadata& metadata = *metadata_.at(*it);
    metadata.remaining_credits = metadata.weight;
    metadata.round_number = global_round_number_;
    if (metadata.weight > 0) {
      metadata.state = EntityState::kEligible;
      eligible_.insert(*it);
      it = waiting_.erase(it);
    } else {
      ++it;
    }
  }

  // Move all entities from WaitingGoingIdle to Idle list.
  for (const auto& entity : waiting_going_idle_) {
    EntityMetadata& metadata = *metadata_.at(entity);
    metadata.state = EntityState::kIdle;
    idle_.insert(entity);
  }
  waiting_going_idle_.clear();
}

template <typename T>
bool WeightedRoundRobinPolicy<T>::IsRoundOver() {
  // Zero credits remaining, so round is over.
  if (global_credits_remaining_ == 0) {
    VLOG(2) << "Round over due to global credits = 0.";
    return true;
  }
  // Eligible list is not empty, and we have credits, so round not over.
  if (!eligible_.empty()) {
    return false;
  }
  // Both Eligible and Waiting list are empty, so round is over.
  if (waiting_.empty()) {
    VLOG(2) << "Round over because eligible and waiting are empty.";
    return true;
  }
  // Eligible empty, Waiting non-empty, so start a new sub-round.
  StartSubRound();
  // If Eligible is still empty, round is over.
  if (eligible_.empty()) {
    VLOG(2) << "Round over because all entities have zero credits.";
    return true;
  }
  return false;
}

template <typename T>
void WeightedRoundRobinPolicy<T>::StartSubRound() {
  // Move all entities in Waiting list with non-zero credits to Eligible list.
  for (auto it = waiting_.begin(); it != waiting_.end();) {
    EntityMetadata& metadata = *metadata_.at(*it);
    if (metadata.remaining_credits > 0) {
      metadata.state = EntityState::kEligible;
      eligible_.insert(*it);
      it = waiting_.erase(it);
    } else {
      ++it;
    }
  }

  // Move all entities from WaitingGoingIdle to Idle list.
  for (const auto& entity : waiting_going_idle_) {
    EntityMetadata& metadata = *metadata_.at(entity);
    metadata.state = EntityState::kIdle;
    idle_.insert(entity);
  }
  waiting_going_idle_.clear();
}

template <typename T>
void WeightedRoundRobinPolicy<T>::SanityCheckForTesting() {
  for (const auto& [entity, metadata] : metadata_) {
    switch (metadata->state) {
      case EntityState::kIdle:
        CHECK(idle_.contains(entity) && !eligible_.contains(entity) &&
              !waiting_.contains(entity) &&
              !waiting_going_idle_.contains(entity))
            << "Idle entity not in correct list.";
        break;
      case EntityState::kEligible:
        CHECK(!idle_.contains(entity) && eligible_.contains(entity) &&
              !waiting_.contains(entity) &&
              !waiting_going_idle_.contains(entity))
            << "Eligible entity not in correct list.";
        break;
      case EntityState::kWaiting:
        CHECK(!idle_.contains(entity) && !eligible_.contains(entity) &&
              waiting_.contains(entity) &&
              !waiting_going_idle_.contains(entity))
            << "Waiting entity not in correct list.";
        break;
      case EntityState::kWaitingGoingIdle:
        CHECK(!idle_.contains(entity) && !eligible_.contains(entity) &&
              !waiting_.contains(entity) &&
              waiting_going_idle_.contains(entity))
            << "GoingIdle entity not in correct list.";
        break;
    }
  }
}

template class WeightedRoundRobinPolicy<uint32_t>;  // For connection ids.
template class WeightedRoundRobinPolicy<PacketTypeQueue>;
template class WeightedRoundRobinPolicy<WindowTypeQueue>;

}  // namespace isekai

#endif  // ISEKAI_HOST_FALCON_WEIGHTED_ROUND_ROBIN_POLICY_H_
