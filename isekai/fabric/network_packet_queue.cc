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

#include "isekai/fabric/network_packet_queue.h"

#include <string_view>

#include "absl/strings/substitute.h"
#include "glog/logging.h"
#include "inet/common/packet/Packet.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/status_util.h"
#include "isekai/fabric/packet_util.h"
#include "omnetpp/csimulation.h"

namespace isekai {
namespace {
// Flag: enable_per_port_per_queue_stats
constexpr std::string_view kStatVectorPacketEnqueueFails =
    "router.port$0.queue$1.enqueue_fails";
constexpr std::string_view kStatVectorQueueLength =
    "router.port$0.queue$1.tx_queue_length";
constexpr std::string_view kStatVectorQueueBytes =
    "router.port$0.queue$1.tx_queue_bytes";
constexpr std::string_view kStatVectorQueueDelay =
    "router.port$0.queue$1.packet_queueing_delay";
constexpr std::string_view kStatVectorQueueTxBytes =
    "router.port$0.queue$1.tx_bytes";
}  // namespace

StatisticsCollectionConfig::RouterFlags
    NetworkPacketQueue::stats_collection_flags_;

NetworkPacketQueue::~NetworkPacketQueue() {
  while (!queue_.empty()) {
    auto packet = queue_.front();
    delete packet;
    queue_.pop();
  }
}

void NetworkPacketQueue::InitializeQueueStatsCollection() {
  enqueue_fail_stats_name_ =
      absl::Substitute(kStatVectorPacketEnqueueFails, port_id_, queue_id_);
  queue_length_stats_name_ =
      absl::Substitute(kStatVectorQueueLength, port_id_, queue_id_);
  queue_bytes_stats_name_ =
      absl::Substitute(kStatVectorQueueBytes, port_id_, queue_id_);
  queue_delay_stats_name_ =
      absl::Substitute(kStatVectorQueueDelay, port_id_, queue_id_);
  queue_tx_bytes_stats_name_ =
      absl::Substitute(kStatVectorQueueTxBytes, port_id_, queue_id_);
}

bool NetworkPacketQueue::Enqueue(omnetpp::cMessage* packet) {
  // Checks with MMU to see if the packet can be enqueued.
  if (memory_management_unit_->RequestEnqueue(port_id_, queue_id_, packet) ==
      false) {
    if (stats_collection_flags_.enable_per_port_per_queue_stats()) {
      CHECK_OK(stats_collection_->UpdateStatistic(
          enqueue_fail_stats_name_, ++packet_enqueue_fails_,
          StatisticsCollectionConfig::TIME_SERIES_STAT));
    }
    delete packet;
    return false;
  }
  // Records the enqueue time.
  packet->setTimestamp();

  if (stats_collection_flags_.enable_per_port_per_queue_stats()) {
    // Collects queue length.
    CHECK_OK(stats_collection_->UpdateStatistic(
        queue_bytes_stats_name_, queue_bytes_,
        StatisticsCollectionConfig::TIME_SERIES_STAT));
    CHECK_OK(stats_collection_->UpdateStatistic(
        queue_length_stats_name_, queue_.size(),
        StatisticsCollectionConfig::TIME_SERIES_STAT));
  }

  queue_.push(packet);
  queue_bytes_ +=
      omnetpp::check_and_cast<inet::Packet*>(packet)->getByteLength();
  return true;
}

omnetpp::cMessage* NetworkPacketQueue::Peek() {
  // Returns nullptr if the queue can not dequeue tx packet.
  if (memory_management_unit_->CanTransmit(port_id_, queue_id_) == false) {
    return nullptr;
  }
  CHECK(Size() != 0) << "No packet for peeking.";
  return queue_.front();
}

omnetpp::cMessage* NetworkPacketQueue::Dequeue() {
  auto dequeued_packet = Peek();
  if (!dequeued_packet) {
    return nullptr;
  }

  queue_.pop();
  // Updates the packet queueing delay.
  if (stats_collection_flags_.enable_per_port_per_queue_stats()) {
    CHECK_OK(stats_collection_->UpdateStatistic(
        queue_delay_stats_name_,
        SIMTIME_DBL(omnetpp::simTime() - dequeued_packet->getTimestamp()),
        StatisticsCollectionConfig::TIME_SERIES_STAT));
  }
  const auto pkt_bytes =
      omnetpp::check_and_cast<inet::Packet*>(dequeued_packet)->getByteLength();

  DCHECK_GE(queue_bytes_, pkt_bytes);
  queue_bytes_ -= pkt_bytes;
  tx_bytes_ += pkt_bytes;

  if (stats_collection_flags_.enable_per_port_per_queue_stats()) {
    CHECK_OK(stats_collection_->UpdateStatistic(
        queue_bytes_stats_name_, queue_bytes_,
        StatisticsCollectionConfig::TIME_SERIES_STAT));
    CHECK_OK(stats_collection_->UpdateStatistic(
        queue_tx_bytes_stats_name_, tx_bytes_,
        StatisticsCollectionConfig::TIME_SERIES_STAT));
  }
  memory_management_unit_->DequeuedPacket(port_id_, queue_id_, dequeued_packet);
  return dequeued_packet;
}

}  // namespace isekai
