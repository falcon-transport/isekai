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

#ifndef ISEKAI_HOST_FALCON_GEN3_STRICT_PRIORITY_POLICY_H_
#define ISEKAI_HOST_FALCON_GEN3_STRICT_PRIORITY_POLICY_H_

#include <functional>
#include <utility>

#include "absl/container/btree_set.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"

namespace isekai {

// The Entity type <T> must be hashable by Abseil and have comparison operators
// defined for ordering.
template <typename T>
class StrictPriorityPolicy : public PathSelectionPolicy<T> {
 public:
  // A function that the policy can call to get the priority of an entity.
  using PriorityFetcher = std::function<int(const T&)>;

  // Constructor that takes in a PriorityFetcher function.
  // The policy will call this PriorityFetcher function to get the latest
  // priority when making a scheduling decision.
  explicit StrictPriorityPolicy(PriorityFetcher fetcher)
      : priority_fetcher_(std::move(fetcher)) {
    if (!priority_fetcher_) {
      priority_fetcher_ = [](const T& entity) -> int { return 0; };
    }
  }

  // Initializes an entity in the Strict Priority policy by adding the entity to
  // entities_ set.
  absl::Status InitializeEntity(const T& entity) override {
    if (entities_.contains(entity)) {
      return absl::InvalidArgumentError("Entity already exists in policy.");
    }
    entities_.insert(entity);
    last_serviced_entity_ = entity;
    return absl::OkStatus();
  }

  // Removes an entity from Strict Priority policy by deleting the entity from
  // the entities_ set.
  absl::Status DeleteEntity(const T& entity) override {
    if (!entities_.contains(entity)) {
      return absl::InvalidArgumentError("Entity does not exist in policy.");
    }
    entities_.erase(entity);
    return absl::OkStatus();
  }

  // Returns the next entity to be serviced as dictated by the Strict Priority
  // policy. If there is no eligible entity, returns an error.
  absl::StatusOr<T> GetNextEntity() override;

  // Returns if an entity is schedulable.
  bool IsActive(const T& entity) const { return entities_.contains(entity); }

  // Returns true if the policy has an entity that can perform work.
  bool HasWork() override { return !entities_.empty(); }

  // There is no per-entity state in strict priority policy.
  void Reset() override {}

  // Method only for testing.
  int GetPriorityForTesting(const T& entity) const {
    return priority_fetcher_(entity);
  }

 private:
  // Set of schedulable entities. We use btree_sets to maintain order.
  absl::btree_set<T> entities_;
  T last_serviced_entity_;
  PriorityFetcher priority_fetcher_ = nullptr;
};

////////////////////////////////////////////////////////////////////////////////

// Returns the next entity to be serviced as dictated by the Strict Priority
// policy. If there is no eligible entity, returns an error.
// Higher numerical value means higher priority.
template <typename T>
absl::StatusOr<T> StrictPriorityPolicy<T>::GetNextEntity() {
  if (entities_.empty()) {
    return absl::UnavailableError("No active entities in the policy.");
  }

  auto it = entities_.upper_bound(last_serviced_entity_);
  if (it == entities_.end()) {
    it = entities_.begin();
  }
  int next_entity = *it;
  int max_prio = priority_fetcher_(next_entity);
  for (int i = 1; i < entities_.size(); ++i) {
    ++it;
    if (it == entities_.end()) {
      it = entities_.begin();
    }
    int prio = priority_fetcher_(*it);
    if (prio > max_prio) {
      next_entity = *it;
      max_prio = prio;
    }
  }

  last_serviced_entity_ = next_entity;
  return next_entity;
}

}  // namespace isekai

#endif  // ISEKAI_HOST_FALCON_GEN3_STRICT_PRIORITY_POLICY_H_
