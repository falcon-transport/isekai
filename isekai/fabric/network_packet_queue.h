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

#ifndef ISEKAI_FABRIC_NETWORK_PACKET_QUEUE_H_
#define ISEKAI_FABRIC_NETWORK_PACKET_QUEUE_H_

#include <cstdint>
#include <queue>

#include "isekai/common/config.pb.h"
#include "isekai/common/default_config_generator.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/fabric/model_interfaces.h"

namespace isekai {

// Network packet queue communicates with MMU to determine packet
// enqueue/dequeue.
class NetworkPacketQueue : public PacketQueueInterface {
 public:
  // clang-format off
  NetworkPacketQueue(int queue_id, int port_id,
                     MemoryManagementUnitInterface* memory_management_unit,
                     StatisticCollectionInterface* stats_collection)
      // clang-format on
      : queue_id_(queue_id),
        port_id_(port_id),
        memory_management_unit_(memory_management_unit),
        stats_collection_(stats_collection) {
    // Initializes statistic collection.
    if (stats_collection_) {
      InitializeQueueStatsCollection();
      if (stats_collection_->GetConfig().has_router_flags()) {
        stats_collection_flags_ = stats_collection_->GetConfig().router_flags();
      } else {
        stats_collection_flags_ =
            DefaultConfigGenerator::DefaultRouterStatsFlags();
      }
    }
  }
  ~NetworkPacketQueue() override;
  // Return false if MMU tells the packet can not be enqueued.
  bool Enqueue(omnetpp::cMessage* packet) override;
  // Return nullptr if MMU tells that no packet is available to dequeue,
  // otherwise returns the dequeued packet.
  omnetpp::cMessage* Dequeue() override;
  size_t Size() override { return queue_.size(); }
  size_t Bytes() override { return queue_bytes_; }
  // Return nullptr if no packet is available to be dequeued.
  omnetpp::cMessage* Peek() override;

 private:
  // Initializes statistic collection for the queue.
  void InitializeQueueStatsCollection();

 private:
  const int queue_id_;
  const int port_id_;
  MemoryManagementUnitInterface* const memory_management_unit_;
  std::queue<omnetpp::cMessage*> queue_;
  // Packet queue length in bytes.
  uint32_t queue_bytes_ = 0;
  uint64_t packet_enqueue_fails_ = 0;
  uint64_t tx_bytes_ = 0;
  // Records the packets dropped by this queue.
  StatisticCollectionInterface* const stats_collection_;
  static StatisticsCollectionConfig::RouterFlags stats_collection_flags_;
  std::string enqueue_fail_stats_name_;
  std::string queue_length_stats_name_;
  std::string queue_bytes_stats_name_;
  std::string queue_delay_stats_name_;
  std::string queue_tx_bytes_stats_name_;
};

}  // namespace isekai

#endif  // ISEKAI_FABRIC_NETWORK_PACKET_QUEUE_H_
