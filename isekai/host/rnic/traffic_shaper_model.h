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

#ifndef ISEKAI_HOST_RNIC_TRAFFIC_SHAPER_MODEL_H_
#define ISEKAI_HOST_RNIC_TRAFFIC_SHAPER_MODEL_H_

#include <cstdint>
#include <deque>
#include <memory>
#include <queue>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/time/time.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/environment.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/common/packet.h"

namespace isekai {

// The TrafficShaperModel class implements a rate based traffic pacer using
// timing wheels based on departure timestamps computed by upstream engines.
class TrafficShaperModel : public TrafficShaperInterface {
 public:
  struct Options {
    // Duration of each timing wheel slot.
    absl::Duration timing_wheel_slot_granularity;
    // Total number of timing wheel slots.
    int64_t timing_wheel_number_of_slots;

    explicit Options(const TrafficShaperConfig& config)
        : timing_wheel_slot_granularity(
              absl::Nanoseconds(config.slot_granularity_ns())),
          timing_wheel_number_of_slots(config.timing_wheel_slots()) {}
  };

  TrafficShaperModel(const TrafficShaperConfig& config, Environment* env,
                     StatisticCollectionInterface* stats_collector)
      : options_(config),
        timing_wheel_horizon_(options_.timing_wheel_slot_granularity *
                              options_.timing_wheel_number_of_slots),
        env_(env),
        stats_collector_(stats_collector) {}

  void ConnectPacketBuilder(PacketBuilderInterface* packet_builder) override {
    packet_builder_ = packet_builder;
  }
  // Schedules a packet into the traffic shaper. This function is called by the
  // upstream FXP/FALCON module with a timestamp (embedded in the packet
  // metadata). When the timestamp elapses, the packet is eligible to be
  // dequeued from the traffic shaper.
  void TransferTxPacket(std::unique_ptr<Packet> pkt) override;

 private:
  // Drains all the packets in the current slot when the slot timer expires.
  void DrainTimingWheelSlot();

  // Configuration for the traffic shaper.
  const Options options_;
  // Total time horizon of the timing wheel.
  const absl::Duration timing_wheel_horizon_;
  // A simplified timing wheel with a single level of time granularity. The
  // wheel has fixed number of slots, represented by a vector. All Packets
  // within a slot are stored in list.
  absl::flat_hash_map<int, std::queue<std::unique_ptr<Packet>>> wheel_;

  // Pointer to the environment this TrafficShaper in running within.
  Environment* const env_;
  // Pointer to the packet builder so that traffic shaper could push the
  // ready-to-depart packet to the packet builder.
  PacketBuilderInterface* packet_builder_ = nullptr;

  StatisticCollectionInterface* const stats_collector_;
};

}  // namespace isekai

#endif  // ISEKAI_HOST_RNIC_TRAFFIC_SHAPER_MODEL_H_
