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

#ifndef ISEKAI_HOST_FALCON_FALCON_PROTOCOL_CONNECTION_SCHEDULER_H_
#define ISEKAI_HOST_FALCON_FALCON_PROTOCOL_CONNECTION_SCHEDULER_H_

#include <cstdint>
#include <string_view>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/time/time.h"
#include "isekai/host/falcon/falcon.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/gen1/connection_scheduler_types.h"
#include "isekai/host/falcon/gen1/falcon_types.h"
#include "isekai/host/falcon/gen1/weighted_round_robin_policy.h"

namespace isekai {

class ProtocolConnectionScheduler : public Scheduler {
 public:
  // Maximum queuing delay time for each packet type ($0).
  static constexpr std::string_view kMaxQueueingDelayByType =
      "host.falcon.max_queueing_delay_by_type.$0";
  // Scheduled packet count by type ($1) during the queuing of the above
  // max-delayed packet in each type ($0).
  static constexpr std::string_view
      kSerializedPacketCountByTypeForMaxQueuedPacket =
          "host.falcon.serialized_packet_count_by_type_for_max_queued_"
          "packet.$0.$1";
  // Queue length per scheduler queue type ($1) when the queuing of the above
  // max-delayed packet in each type ($0) is enqueued.
  static constexpr std::string_view kEnqueueTimeQueueLengthForMaxQueuedPacket =
      "host.falcon.enqueue_time_queue_length_for_max_queued_"
      "packet.$0.$1";

  explicit ProtocolConnectionScheduler(FalconModelInterface* falcon);
  absl::Status InitConnectionSchedulerQueues(uint32_t scid) override;
  absl::Status EnqueuePacket(uint32_t scid, uint32_t rsn,
                             falcon::PacketType type) override;
  absl::Status DequeuePacket(uint32_t scid, uint32_t rsn,
                             falcon::PacketType type) override {
    return absl::OkStatus();
  }
  bool HasWork() override;
  bool ScheduleWork() override;

  // Recomputes the transmission eligibility for all packet types and given
  // connection. It is automatically called when any gating variable is updated.
  void RecomputeEligibility(uint32_t scid) override;
  uint32_t GetConnectionQueueLength(uint32_t scid) override;
  uint64_t GetQueueTypeBasedOutstandingPacketCount(
      PacketTypeQueue queue_type) override {
    return packet_queue_wise_pkt_count_[queue_type];
  }

 private:
  // Add a packet to the relevant queue for transmitting over the network and
  // returns the queue to which the packet was added.
  PacketTypeQueue AddPacketToQueue(uint32_t scid, uint32_t rsn,
                                   falcon::PacketType type);

  // Schedules the given packet by handing it off to the downstream blocks.
  void SchedulePacket(uint32_t scid, PacketTypeQueue queue_type,
                      const WorkId& work_id);

  // Checks if a packet meets its congestion window and admission criteria.
  bool CanSend(uint32_t scid, uint32_t rsn, falcon::PacketType type);

  // Handles the phantom request in the connection scheduler.
  virtual void HandlePhantomRequest(uint32_t scid, const WorkId& work_id,
                                    PacketTypeQueue candidate_queue_type);

  // Returns the weight of the given packet type for WRR arbitration. Called by
  // the WRR policy every time a new round starts.
  int ComputePacketTypeWeight(const PacketTypeQueue& packet_type);

  void UpdateOutstandingPacketCount(PacketTypeQueue queue_type, bool is_dequed);
  // Update queuing delay stats, including those of the max-delayed packets.
  void UpdateQueuingDelayStatistics(
      PacketMetadata::PacketQueuingDelayMetadata* queuing_delay_metadata,
      uint32_t scid, bool is_ordered_connection,
      falcon::PacketType packet_type);

  void RecordRsnDiffMeasure(uint32_t scid, const WorkId& work_id);

  uint32_t last_push_unsolicited_data_rsn = 0;
  bool sent_out_push_unsolicited_data = false;

  absl::flat_hash_map<PacketTypeQueue, uint64_t> packet_queue_wise_pkt_count_;
  absl::flat_hash_map<falcon::PacketType, uint64_t> packet_type_wise_tx_count_;
  bool collect_max_delay_stats_ = false;
  absl::flat_hash_map<falcon::PacketType, absl::Duration>
      packet_type_wise_max_queueing_delay_;

  // We store the top level packet_type policy, and for each packet_type queue,
  // we store the connection policy.
  WeightedRoundRobinPolicy<PacketTypeQueue> packet_type_policy_;
  absl::flat_hash_map<PacketTypeQueue, WeightedRoundRobinPolicy<uint32_t> >
      connection_policy_;
  // We also store separate policies for phantom requests and ordered push
  // solicited requests to quickly determine if a connection is eligible to send
  // one of these packet types.
  WeightedRoundRobinPolicy<uint32_t> phantom_connection_policy_;
  WeightedRoundRobinPolicy<uint32_t> ordered_push_req_connection_policy_;

  // Stores the per-packet_type queues for each connection.
  absl::flat_hash_map<uint32_t, ConnectionSchedulerQueues> connection_queues_;

  FalconModelInterface* const falcon_;
};

}  // namespace isekai

#endif  // ISEKAI_HOST_FALCON_FALCON_PROTOCOL_CONNECTION_SCHEDULER_H_
