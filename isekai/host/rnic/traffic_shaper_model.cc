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

#include "isekai/host/rnic/traffic_shaper_model.h"

#include <deque>
#include <memory>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/time/time.h"
#include "glog/logging.h"
#include "isekai/common/environment.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/common/packet.h"
#include "isekai/common/status_util.h"

namespace isekai {

void TrafficShaperModel::TransferTxPacket(std::unique_ptr<Packet> pkt) {
  absl::Duration now = env_->ElapsedTime();
  absl::Duration pkt_timestamp = pkt->metadata.timing_wheel_timestamp;

  // Timestamp is all zeros, bypass traffic shaper.
  if (pkt->metadata.timing_wheel_timestamp == absl::ZeroDuration()) {
    packet_builder_->EnqueuePacket(std::move(pkt));
    return;
  }

  // Timestamps can be at most one horizon in the past or future.
  CHECK(pkt_timestamp + timing_wheel_horizon_ >= now)
      << "Packet departure time is in the past by more than a time horizon.\n"
      << pkt->DebugString() << "now: " << now << "\n"
      << "horizon: " << timing_wheel_horizon_ << "\n";

  CHECK(pkt_timestamp <= now + timing_wheel_horizon_)
      << "Packet departure time is in the future by more than a time horizon.\n"
      << pkt->DebugString() << "now: " << now << "\n"
      << "horizon: " << timing_wheel_horizon_ << "\n";

  // Determine the enqueue slot for this packet by rounding up departure
  // timestamp to timing wheel granularity, dividing by granularity modulo
  // number of timing wheel slots.
  absl::Duration departure_roundup_time;
  if (pkt_timestamp < now) {
    // Packet is before current time, enqueue in the current slot.
    departure_roundup_time =
        absl::Ceil(now, options_.timing_wheel_slot_granularity);
  } else {
    // Packet is within the horizon, enqueue in future slot.
    departure_roundup_time =
        absl::Ceil(pkt_timestamp, options_.timing_wheel_slot_granularity);
  }
  int enqueue_slot =
      (departure_roundup_time / options_.timing_wheel_slot_granularity) %
      options_.timing_wheel_number_of_slots;

  if (wheel_.contains(enqueue_slot)) {
    wheel_.at(enqueue_slot).push(std::move(pkt));
  } else {
    wheel_[enqueue_slot].push(std::move(pkt));
    // If the slot was empty and we enqueue a packet into it, we schedule the
    // traffic shaper to drain that slot at the appropriate time. If it was
    // non-empty, it must have been scheduled already.
    CHECK_OK(env_->ScheduleEvent(departure_roundup_time - now,
                                 [this]() { DrainTimingWheelSlot(); }));
  }
}

void TrafficShaperModel::DrainTimingWheelSlot() {
  int current_slot =
      (env_->ElapsedTime() / options_.timing_wheel_slot_granularity) %
      options_.timing_wheel_number_of_slots;

  auto it = wheel_.extract(current_slot);
  CHECK(!it.empty()) << "No such time slot.";
  auto slot_pkts = std::move(it.mapped());
  CHECK(!slot_pkts.empty()) << "Scheduled a slot without any packets in it.";
  while (!slot_pkts.empty()) {
    packet_builder_->EnqueuePacket(std::move(slot_pkts.front()));
    slot_pkts.pop();
  }
}

}  // namespace isekai
