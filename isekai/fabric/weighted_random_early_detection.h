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

#ifndef ISEKAI_FABRIC_WEIGHTED_RANDOM_EARLY_DETECTION_H_
#define ISEKAI_FABRIC_WEIGHTED_RANDOM_EARLY_DETECTION_H_

#include <cstdint>
#include <random>

#include "absl/random/random.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "isekai/fabric/memory_management_config.pb.h"
#include "omnetpp/csimulation.h"

namespace isekai {

enum class WredResult {
  kEnqueue,
  kEcnMark,
};

// Implementation for WRED (weighted random early detection) for ECN marking.
// Details about WRED can be found in paper: Floyd, S. and Jacobson, V., 1993.
// Random early detection gateways for congestion avoidance. IEEE/ACM
// Transactions on networking, 1(4), pp.397-413.
class WeightedRandomEarlyDetection {
 public:
  WeightedRandomEarlyDetection(
      const MemoryManagementUnitConfig::Wred& configuration, uint32_t seed);
  WredResult PerformWred(uint64_t egress_occupancy);

  absl::Duration GetCurrentTime() {
    return absl::Nanoseconds(omnetpp::simTime().inUnit(omnetpp::SIMTIME_NS));
  }
  void UpdateQueueIdleTime(absl::Duration time) { queue_idle_time_ = time; }

 private:
  // The unit of queue size is in bytes.
  double avg_queue_size_ = 0.0;
  // Number of packets since last marked packet.
  double count_ = -1;
  // Records the simulation time when queue size equals 0.
  absl::Duration queue_idle_time_ = absl::ZeroDuration();

  // Belows are the configurable parameters for WRED.
  double weight_;
  uint32_t min_avg_queue_size_;
  uint32_t max_avg_queue_size_;
  double max_mark_prob_;
  double rate_;

  std::mt19937 rng_;
};

}  // namespace isekai

#endif  // ISEKAI_FABRIC_WEIGHTED_RANDOM_EARLY_DETECTION_H_
