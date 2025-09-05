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

#include "isekai/host/falcon/gen1/inter_host_rx_scheduler.h"

#include <algorithm>
#include <cstdint>
#include <queue>
#include <string_view>
#include <utility>

#include "absl/functional/any_invocable.h"
#include "absl/status/statusor.h"
#include "absl/strings/substitute.h"
#include "absl/time/time.h"
#include "glog/logging.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"

namespace isekai {
namespace {
// Flag: enable_inter_host_rx_scheduler_queue_length
constexpr std::string_view kStatVectorInterHostRxSchedulerQueueLength =
    "falcon.inter_host_rx_scheduler.bifurcation$0.outstanding_packets";
}  // namespace

// Constructor for inter host rx scheduler.
ProtocolInterHostRxScheduler::ProtocolInterHostRxScheduler(
    FalconModelInterface* falcon, uint8_t number_of_hosts)
    : falcon_(falcon),
      is_running_(false),
      inter_host_rx_policy_(/*fetcher=*/nullptr, /*batched=*/false,
                            /*enforce_order=*/true) {
  CHECK(falcon_) << "Falcon cannot be nullptr";
  CHECK(number_of_hosts > 0) << "Number of hosts must be greater than 0";
  rx_link_bandwidth_gpbs_ = falcon_->get_config()->rx_falcon_ulp_link_gbps();
  scheduler_cycle_time_ns_ = absl::Nanoseconds(
      falcon_->get_config()->inter_host_rx_scheduling_tick_ns());
  // Initialize the inter-packet gap to the minimum possible value, which is the
  // scheduler cycle time.
  inter_packet_gap_ns_ = scheduler_cycle_time_ns_;
  xoff_status_.resize(number_of_hosts);
  for (uint8_t host_idx = 0; host_idx < number_of_hosts; host_idx++) {
    CHECK_OK(inter_host_rx_policy_.InitializeEntity(host_idx));
    xoff_status_[host_idx] = false;
    hosts_queue_[host_idx] = std::queue<absl::AnyInvocable<void()>>();
  }
  collect_inter_host_scheduler_queue_length_ =
      falcon->get_stats_manager()
          ->GetStatsConfig()
          .enable_inter_host_rx_scheduler_queue_length();
}

void ProtocolInterHostRxScheduler::Enqueue(uint8_t bifurcation_id,
                                           absl::AnyInvocable<void()> cb) {
  auto host_queue_iter = hosts_queue_.find(bifurcation_id);
  CHECK(host_queue_iter != hosts_queue_.end()) << "Unknown bifurcation id";
  auto& host_queue = host_queue_iter->second;

  host_queue.push(std::move(cb));
  // Record new outstanding Falcon packets to process.
  StatisticCollectionInterface* stats_collector =
      falcon_->get_stats_collector();
  if (collect_inter_host_scheduler_queue_length_ && stats_collector) {
    // Records TX Packet Credits available.
    CHECK_OK(stats_collector->UpdateStatistic(
        absl::Substitute(kStatVectorInterHostRxSchedulerQueueLength,
                         bifurcation_id),
        host_queue.size(), StatisticsCollectionConfig::TIME_SERIES_STAT));
  }

  if (!xoff_status_[bifurcation_id]) {
    inter_host_rx_policy_.MarkEntityActive(bifurcation_id);
  }
  // if the scheduler is already not running, start it.
  if (!is_running_) {
    auto current_time = falcon_->get_environment()->ElapsedTime();
    absl::Duration next_run_time =
        std::max(last_scheduler_run_time_ + inter_packet_gap_ns_, current_time);
    is_running_ = true;
    CHECK_OK(falcon_->get_environment()->ScheduleEvent(
        (next_run_time - current_time), [this]() { ScheduleWork(); }));
  }
}

// Checks if there is any outstanding work to do, if yes, perform it and
// schedule another event.
void ProtocolInterHostRxScheduler::ScheduleWork() {
  is_running_ = false;
  absl::StatusOr<uint8_t> candidate_status_or_id =
      inter_host_rx_policy_.GetNextEntity();

  if (!candidate_status_or_id.ok()) {
    return;
  }

  uint8_t candidate_bifurcation_id = candidate_status_or_id.value();
  auto host_queue_iter = hosts_queue_.find(candidate_bifurcation_id);
  CHECK(host_queue_iter != hosts_queue_.end()) << "Unknown bifurcation id";
  auto& host_queue = host_queue_iter->second;

  auto& callback = host_queue.front();
  last_scheduler_run_time_ = falcon_->get_environment()->ElapsedTime();
  callback();
  host_queue.pop();
  // Record new outstanding Falcon packets to process.
  StatisticCollectionInterface* stats_collector =
      falcon_->get_stats_collector();
  if (collect_inter_host_scheduler_queue_length_ && stats_collector) {
    // Records TX Packet Credits available.
    CHECK_OK(stats_collector->UpdateStatistic(
        absl::Substitute(kStatVectorInterHostRxSchedulerQueueLength,
                         candidate_bifurcation_id),
        host_queue.size(), StatisticsCollectionConfig::TIME_SERIES_STAT));
  }

  if (host_queue.empty()) {
    inter_host_rx_policy_.MarkEntityInactive(candidate_bifurcation_id);
  }

  if (HasWork()) {
    is_running_ = true;
    CHECK_OK(falcon_->get_environment()->ScheduleEvent(
        inter_packet_gap_ns_, [this]() { ScheduleWork(); }));
  }
}

// Xoffs and Xons the provided host Rx queue.
void ProtocolInterHostRxScheduler::SetXoff(uint8_t bifurcation_id, bool xoff) {
  auto host_queue_iter = hosts_queue_.find(bifurcation_id);
  CHECK(host_queue_iter != hosts_queue_.end()) << "Unknown bifurcation id";
  auto& host_queue = host_queue_iter->second;

  xoff_status_[bifurcation_id] = xoff;
  if (xoff) {
    inter_host_rx_policy_.MarkEntityInactive(bifurcation_id);
  } else if (!host_queue.empty()) {
    inter_host_rx_policy_.MarkEntityActive(bifurcation_id);
    if (HasWork() && !is_running_) {
      auto current_time = falcon_->get_environment()->ElapsedTime();
      absl::Duration next_run_time = std::max(
          last_scheduler_run_time_ + inter_packet_gap_ns_, current_time);
      is_running_ = true;
      CHECK_OK(falcon_->get_environment()->ScheduleEvent(
          (next_run_time - current_time), [this]() { ScheduleWork(); }));
    }
  }
}

// Returns true if the host scheduler has outstanding work.
bool ProtocolInterHostRxScheduler::HasWork() {
  return inter_host_rx_policy_.HasWork();
}

void ProtocolInterHostRxScheduler::UpdateInterPacketGap(uint32_t packet_size) {
  auto packet_serialization_delay =
      absl::Nanoseconds(packet_size * 8.0 / rx_link_bandwidth_gpbs_);
  inter_packet_gap_ns_ =
      std::max(scheduler_cycle_time_ns_, packet_serialization_delay);
}

}  // namespace isekai
