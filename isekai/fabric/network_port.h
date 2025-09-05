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

#ifndef ISEKAI_FABRIC_NETWORK_PORT_H_
#define ISEKAI_FABRIC_NETWORK_PORT_H_

#include <cstdint>
#include <memory>
#include <queue>

#include "gtest/gtest_prod.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/fabric/model_interfaces.h"
#include "omnetpp/cdataratechannel.h"
#include "omnetpp/cmessage.h"
#include "omnetpp/simtime_t.h"

namespace isekai {

// Port has two states. Idle means the port is currently not transmitting any
// packet, while Active means the port is transmitting packet.
enum class PortState {
  kIdle,
  kActive,
};

// The NetworkPort is an inout port, which handles the rx and tx packet.
class NetworkPort : public PortInterface {
 public:
  // clang-format off
  // The port is constructed when calling router's initialize() method.
  NetworkPort(NetworkRouterInterface* router, int port_id, int number_of_queues,
              RouterConfigProfile::ArbitrationScheme arbitration_scheme,
              StatisticCollectionInterface* stats_collection,
              bool per_flow_rr);
  // clang-format on
  // Handles the rx packet.
  void ReceivePacket(omnetpp::cMessage* packet) override;
  // Enqueues a packet passed from routing pipeline to the corresponding queue.
  void EnqueuePacket(omnetpp::cMessage* packet) override;
  // Dequeues a packet from queues and transmits it.
  void ScheduleOutput() override;
  // Generates PFC frame and enqueues it.
  void GeneratePfc(int priority, int16_t pause_units) override;
  // Getters to get the port information.
  PacketQueueInterface* GetPacketQueue(int priority) const override {
    return packet_queues_.at(priority).get();
  }
  omnetpp::cDatarateChannel* GetTransmissionChannel() const override {
    return transmission_channel_;
  }
  // Resumes to send packets out from PFC pause.
  void ResumeOutput() override;

  // Collect statistics at port.
  // 1) total queue bytes of all queues
  // 2) capactiy of the egress link
  void CollectPortStatistics() override;

 private:
  FRIEND_TEST(MmuPfcTest, TriggerXonTest);
  FRIEND_TEST(MmuPfcTest, TriggerXoffTest);

  // For unit test only.
  friend class FakePort;

  // Initializes statistic collection for the port. The number of tx and rx
  // packets are recorded.
  void InitializePortStatsCollection();
  // Transmits the packet to the network.
  void SendPacket(omnetpp::cMessage* packet, bool is_pfc);

  // The port holds a pointer to the router so that it can call functions from
  // other router components.
  NetworkRouterInterface* const router_;
  // The port id should be align with the port index in NED file.
  const int port_id_;
  // Records the packets received and sent by the port.
  StatisticCollectionInterface* const stats_collection_;
  static StatisticsCollectionConfig::RouterFlags stats_collection_flags_;
  // The number of TX packets/bytes.
  uint64_t tx_packets_ = 0;
  uint64_t tx_bytes_ = 0;
  std::string tx_packets_stats_name_;
  std::string tx_bytes_stats_name_;
  std::string capacity_stats_name_;
  // The number of RX packets/bytes.
  uint64_t rx_packets_ = 0;
  uint64_t rx_bytes_ = 0;
  std::string rx_packets_stats_name_;
  std::string rx_bytes_stats_name_;
  std::string queue_bytes_stats_name_;
  // The number of bytes in all packet queues.
  uint64_t queue_bytes_ = 0;
  // The data packet queues of the output port.
  std::vector<std::unique_ptr<PacketQueueInterface>> packet_queues_;
  // A fifo queue to store PFC messages.
  std::queue<omnetpp::cMessage*> pfc_queue_;
  // Arbiter for packet_queues_.
  std::unique_ptr<ArbiterInterface> arbiter_;
  // The transmission channel information of the port.
  omnetpp::cDatarateChannel* transmission_channel_ = nullptr;
  // The interframe gap time delay.
  omnetpp::simtime_t ifg_delay_ = -1;
  PortState port_state_ = PortState::kIdle;
  // The number of ingress packet discards.
  uint64_t ingress_packet_discards_ = 0;
  std::string ingress_packet_discards_name_;
  bool per_flow_rr_ = false;
};

}  // namespace isekai

#endif  // ISEKAI_FABRIC_NETWORK_PORT_H_
