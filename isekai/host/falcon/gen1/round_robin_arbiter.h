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

#ifndef ISEKAI_HOST_FALCON_FALCON_PROTOCOL_ROUND_ROBIN_ARBITER_H_
#define ISEKAI_HOST_FALCON_FALCON_PROTOCOL_ROUND_ROBIN_ARBITER_H_

#include <cstdint>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/time/time.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"

namespace isekai {

// Represents the scheduler types within FALCON.
enum class SchedulerType : uint32_t {
  kConnection = 0,
  kRetransmission = 1,
  kAckNack = 2,
  kNumSchedulers = 3,
};

// Arbitrates between the two schedulers within FALCON and picks work for the
// sliding window layer.
class ProtocolRoundRobinArbiter : public Arbiter {
 public:
  explicit ProtocolRoundRobinArbiter(FalconModelInterface* falcon);
  // Schedules the arbiter only when there is outstanding work in either of the
  // schedulers.
  void ScheduleSchedulerArbiter() override;
  // Returns true if either the connection or retransmission scheduler has
  // outstanding work.
  bool HasWork() override;

 private:
  // Performs one unit of work from the connection scheduler or retransmission
  // scheduler.
  void DoArbitration();
  // Gets the scheduler weight.
  uint32_t GetSchedulerWeight(const SchedulerType& scheduler_type) const;

  FalconModelInterface* const falcon_;
  // Indicates the order in which the arbiter chooses schedulers.
  std::vector<SchedulerType> scheduler_ordering_;
  // Map of connection type to connection scheduler handlers.
  absl::flat_hash_map<SchedulerType, Scheduler* const> scheduler_handlers_;
  // Represents the index of the candidate scheduler.
  int candidate_scheduler_index_ = 0;
  // Tick time of the arbiter.
  const absl::Duration tick_time_;
  // Flag to indicate if an arbitration event is scheduled or not.
  bool scheduled_ = false;
  // The last time the arbiter was run.
  absl::Duration last_run_time_;
};

}  // namespace isekai

#endif  // ISEKAI_HOST_FALCON_FALCON_PROTOCOL_ROUND_ROBIN_ARBITER_H_
