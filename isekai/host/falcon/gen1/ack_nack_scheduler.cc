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

#include "isekai/host/falcon/gen1/ack_nack_scheduler.h"

#include <cstdint>
#include <memory>
#include <utility>

#include "absl/log/check.h"
#include "absl/status/status.h"
#include "glog/logging.h"
#include "isekai/common/packet.h"
#include "isekai/host/falcon/falcon.h"
#include "isekai/host/falcon/falcon_histograms.h"

namespace isekai {

absl::Status ProtocolAckNackScheduler::InitConnectionSchedulerQueues(
    uint32_t scid) {
  return absl::OkStatus();
}

absl::Status ProtocolAckNackScheduler::EnqueuePacket(uint32_t scid,
                                                     uint32_t rsn,
                                                     falcon::PacketType type) {
  return absl::OkStatus();
}

absl::Status ProtocolAckNackScheduler::DequeuePacket(uint32_t scid,
                                                     uint32_t rsn,
                                                     falcon::PacketType type) {
  return absl::OkStatus();
}

absl::Status ProtocolAckNackScheduler::EnqueuePacket(
    std::unique_ptr<Packet> ack_nack_packet) {
  CHECK(ack_nack_packet->packet_type == falcon::PacketType::kAck ||
        ack_nack_packet->packet_type == falcon::PacketType::kNack)
      << "Enqueued message is not Ack or Nack";
  falcon_->get_stats_manager()->UpdateSchedulerCounters(
      SchedulerTypes::kAckNack, false);

  ack_nack_queue_.push(std::move(ack_nack_packet));
  // Schedule the work scheduler in case this is the only outstanding work item.
  falcon_->get_arbiter()->ScheduleSchedulerArbiter();
  return absl::OkStatus();
}

bool ProtocolAckNackScheduler::HasWork() {
  return falcon_->CanSendPacket() && !ack_nack_queue_.empty();
}

bool ProtocolAckNackScheduler::ScheduleWork() {
  CHECK(!ack_nack_queue_.empty())
      << "no work to schedule in Ack/Nack scheduler";
  auto ack_nack_packet = std::move(ack_nack_queue_.front());
  ack_nack_queue_.pop();
  uint32_t scid = ack_nack_packet->metadata.scid;
  falcon_->get_stats_manager()->UpdateSchedulerCounters(
      SchedulerTypes::kAckNack, true);
  falcon_->get_packet_metadata_transformer()->TransferTxPacket(
      std::move(ack_nack_packet), scid);
  return true;
}

}  // namespace isekai
