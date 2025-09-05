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

#include "isekai/fabric/weighted_random_early_detection.h"

#include <cmath>
#include <cstdint>
#include <memory>
#include <random>

#include "absl/random/distributions.h"
#include "absl/time/time.h"
#include "glog/logging.h"
#include "omnetpp/distrib.h"

namespace isekai {

WeightedRandomEarlyDetection::WeightedRandomEarlyDetection(
    const MemoryManagementUnitConfig::Wred& configuration, uint32_t seed) {
  rng_.seed(seed);
  weight_ = configuration.weight();
  min_avg_queue_size_ = configuration.min_avg_queue_size();
  max_avg_queue_size_ = configuration.max_avg_queue_size();
  max_mark_prob_ = configuration.max_mark_prob();
  rate_ = configuration.rate();
}

// Details about WRED can be found in paper: Floyd, S. and Jacobson, V., 1993.
// Random early detection gateways for congestion avoidance. IEEE/ACM
// Transactions on networking, 1(4), pp.397-413
// (https://ieeexplore.ieee.org/document/251892)
WredResult WeightedRandomEarlyDetection::PerformWred(
    uint64_t egress_occupancy) {
  if (egress_occupancy > 0) {
    avg_queue_size_ =
        (1 - weight_) * avg_queue_size_ + weight_ * egress_occupancy;
  } else {
    double m =
        absl::ToDoubleSeconds(GetCurrentTime() - queue_idle_time_) * rate_;
    avg_queue_size_ = std::pow(1 - weight_, m) * avg_queue_size_;
  }

  if (avg_queue_size_ >= min_avg_queue_size_ &&
      avg_queue_size_ <= max_avg_queue_size_) {
    count_++;
    double pb = max_mark_prob_ * (avg_queue_size_ - min_avg_queue_size_) /
                (max_avg_queue_size_ - avg_queue_size_);
    double pa = pb / (1 - count_ * pb);
    if (std::uniform_real_distribution<double>{0, 1.0}(rng_) < pa) {
      count_ = 0;
      return WredResult::kEcnMark;
    } else {
      return WredResult::kEnqueue;
    }
  } else if (avg_queue_size_ > max_avg_queue_size_) {
    count_ = 0;
    return WredResult::kEcnMark;
  } else {
    count_ = -1;
  }

  return WredResult::kEnqueue;
}

}  // namespace isekai
