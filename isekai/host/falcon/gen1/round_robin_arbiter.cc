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

#include "isekai/host/falcon/gen1/round_robin_arbiter.h"

#include <cstdint>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/time/time.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/environment.h"
#include "isekai/common/status_util.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"

namespace isekai {

ProtocolRoundRobinArbiter::ProtocolRoundRobinArbiter(
    FalconModelInterface* falcon)
    : falcon_(falcon),
      scheduler_handlers_(
          {{SchedulerType::kConnection, falcon_->get_connection_scheduler()},
           {SchedulerType::kRetransmission,
            falcon_->get_retransmission_scheduler()},
           {SchedulerType::kAckNack, falcon_->get_ack_nack_scheduler()}}),
      tick_time_(
          absl::Nanoseconds(falcon->get_config()->falcon_tick_time_ns())),
      last_run_time_(-absl::InfiniteDuration()) {
  for (uint32_t i = 0; i < static_cast<uint32_t>(SchedulerType::kNumSchedulers);
       ++i) {
    auto scheduler_type = static_cast<SchedulerType>(i);
    for (uint32_t j = 0; j < GetSchedulerWeight(scheduler_type); ++j) {
      scheduler_ordering_.push_back(scheduler_type);
    }
  }
  CHECK(!scheduler_ordering_.empty()) << "Scheduler weights not configured.";
}

// Schedules the arbiter only when there is outstanding work in either of the
// schedulers.
void ProtocolRoundRobinArbiter::ScheduleSchedulerArbiter() {
  // If there is aready an arbitration event scheduled, don't schedule another.
  if (scheduled_ || !HasWork()) {
    return;
  }
  // Schedule the arbitration event one tick from last_run_time. If one tick has
  // elapsed, schedule arbitration immediately.
  absl::Duration next_run_time = last_run_time_ + tick_time_;
  absl::Duration now = falcon_->get_environment()->ElapsedTime();
  if (now >= next_run_time) {
    next_run_time = now;
  }
  scheduled_ = true;
  CHECK_OK(falcon_->get_environment()->ScheduleEvent(
      next_run_time - now, [this]() { DoArbitration(); }));
}

// Performs one unit of work from the connection scheduler or retransmission
// scheduler.
void ProtocolRoundRobinArbiter::DoArbitration() {
  if (!HasWork()) {
    scheduled_ = false;
    return;
  }
  last_run_time_ = falcon_->get_environment()->ElapsedTime();
  int current_scheduler_index = candidate_scheduler_index_;
  while (true) {
    auto* scheduler =
        scheduler_handlers_[scheduler_ordering_[current_scheduler_index]];

    if (scheduler->HasWork() && scheduler->ScheduleWork()) {
      candidate_scheduler_index_ =
          (current_scheduler_index + 1) % scheduler_ordering_.size();
      break;
    }
    current_scheduler_index =
        (current_scheduler_index + 1) % scheduler_ordering_.size();
    if (current_scheduler_index == candidate_scheduler_index_) {
      break;
    }
  }
  if (HasWork()) {
    CHECK_OK(falcon_->get_environment()->ScheduleEvent(
        tick_time_, [this]() { DoArbitration(); }));
  } else {
    scheduled_ = false;
  }
}

// Returns true if either the connection or retransmission scheduler has
// outstanding work.
bool ProtocolRoundRobinArbiter::HasWork() {
  return falcon_->get_connection_scheduler()->HasWork() ||
         falcon_->get_retransmission_scheduler()->HasWork() ||
         falcon_->get_ack_nack_scheduler()->HasWork();
}

uint32_t ProtocolRoundRobinArbiter::GetSchedulerWeight(
    const SchedulerType& scheduler_type) const {
  switch (scheduler_type) {
    case SchedulerType::kConnection:
      return falcon_->get_config()->connection_scheduler_weight();
    case SchedulerType::kRetransmission:
      return falcon_->get_config()->retransmission_scheduler_weight();
    case SchedulerType::kAckNack:
      return falcon_->get_config()->ack_nack_scheduler_weight();
    default:
      LOG(FATAL) << "Unknown scheduler type.";
  }
}

}  // namespace isekai
