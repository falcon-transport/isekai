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

#include "isekai/fabric/network_port.h"

#include <cstdint>
#include <memory>
#include <string_view>

#include "absl/strings/substitute.h"
#include "glog/logging.h"
#include "inet/common/Units.h"
#include "inet/common/packet/Packet.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/default_config_generator.h"
#include "isekai/common/status_util.h"
#include "isekai/fabric/arbiter.h"
#include "isekai/fabric/network_packet_queue.h"
#include "isekai/fabric/packet_util.h"
#include "omnetpp/checkandcast.h"
#include "omnetpp/cmessage.h"
#include "omnetpp/csimulation.h"
#include "omnetpp/simtime.h"
#include "omnetpp/simtime_t.h"

namespace isekai {
namespace {
// Flag: enable_scalar_per_port_tx_rx_packets
constexpr std::string_view kStatScalarPortTxPackets =
    "router.port$0.tx_packets";
constexpr std::string_view kStatScalarPortRxPackets =
    "router.port$0.rx_packets";

// Flag: enable_vector_per_port_tx_rx_bytes
constexpr std::string_view kStatVectorPortTxBytes = "router.port$0.tx_bytes";
constexpr std::string_view kStatVectorPortRxBytes = "router.port$0.rx_bytes";

// Flag: enable_port_stats_collection
// Counter for capacity of the egress link of the port.
constexpr std::string_view kStatVectorPortCapacity = "router.port$0.capacity";
// Counter for total bytes of all queues at the port.
constexpr std::string_view kStatVectorPortQueueBytes =
    "router.port$0.queue_bytes";

// Flag: enable_per_port_ingress_discards
// Counter for ingress packet discards.
constexpr std::string_view kStatScalarPortIngressPacketDiscards =
    "router.port$0.ingress_packet_discards";

// The default delay for enqueueing one packet in ns.
constexpr uint32_t kEnqueueDelay = 10;

constexpr uint32_t kInterFrameGapBits = 96;

constexpr uint32_t kOneKbps = 1'000U;

}  // namespace

StatisticsCollectionConfig::RouterFlags NetworkPort::stats_collection_flags_;

// For unit test, the router and stats_collection could be NULL in order to
// create a fake port object.
NetworkPort::NetworkPort(
    NetworkRouterInterface* router, int port_id, int number_of_queues,
    RouterConfigProfile::ArbitrationScheme arbitration_scheme,
    StatisticCollectionInterface* stats_collection, bool per_flow_rr)
    : router_(router), port_id_(port_id), stats_collection_(stats_collection) {
  if (stats_collection && stats_collection->GetConfig().has_router_flags()) {
    stats_collection_flags_ = stats_collection->GetConfig().router_flags();
  } else {
    stats_collection_flags_ = DefaultConfigGenerator::DefaultRouterStatsFlags();
  }
  per_flow_rr_ = per_flow_rr;
  // Initializes packet queues of the port.
  for (int i = 0; i < number_of_queues; i++) {
    packet_queues_.push_back(std::make_unique<NetworkPacketQueue>(
        i, port_id, router->GetMemoryManagementUnit(), stats_collection));
  }
  // Initializes the arbiter of the TX queues.
  arbiter_ = CreateArbiter(arbitration_scheme);
  // Gets the transmission channel.
  if (router) {
    transmission_channel_ =
        omnetpp::check_and_cast_nullable<omnetpp::cDatarateChannel*>(
            router->gate("port$o", port_id)->findTransmissionChannel());
  }
  // Calculates the interframe gap.
  if (transmission_channel_) {
    ifg_delay_ = inet::units::values::b(kInterFrameGapBits).get() /
                 transmission_channel_->getNominalDatarate();
  }
  // Initializes statistic collection.
  if (stats_collection_) {
    InitializePortStatsCollection();
  }
}

void NetworkPort::InitializePortStatsCollection() {
  tx_packets_stats_name_ = absl::Substitute(kStatScalarPortTxPackets, port_id_);
  tx_bytes_stats_name_ = absl::Substitute(kStatVectorPortTxBytes, port_id_);
  rx_packets_stats_name_ = absl::Substitute(kStatScalarPortRxPackets, port_id_);
  rx_bytes_stats_name_ = absl::Substitute(kStatVectorPortRxBytes, port_id_);

  queue_bytes_stats_name_ =
      absl::Substitute(kStatVectorPortQueueBytes, port_id_);

  capacity_stats_name_ = absl::Substitute(kStatVectorPortCapacity, port_id_);
  ingress_packet_discards_name_ =
      absl::Substitute(kStatScalarPortIngressPacketDiscards, port_id_);
}

void NetworkPort::CollectPortStatistics() {
  // Sum up queue bytes from all packet queues.
  uint64_t queue_bytes = 0;
  for (const auto& pkt_queue : packet_queues_) {
    queue_bytes += pkt_queue->Bytes();
  }
  queue_bytes_ = queue_bytes;
  if (stats_collection_flags_.enable_port_stats_collection() &&
      stats_collection_) {
    CHECK_OK(stats_collection_->UpdateStatistic(
        queue_bytes_stats_name_, queue_bytes_,
        StatisticsCollectionConfig::TIME_SERIES_STAT));
  }
}

void NetworkPort::SendPacket(omnetpp::cMessage* packet, bool is_pfc) {
  // Adds frame header for the sent-out data packet.
  if (!is_pfc) {
    EncapsulateMacHeader(packet);
  }
  router_->SendPacket(packet, port_id_, transmission_channel_);
  // Updates the TX packets statistics.
  if (stats_collection_flags_.enable_scalar_per_port_tx_rx_packets()) {
    CHECK_OK(stats_collection_->UpdateStatistic(
        tx_packets_stats_name_, ++tx_packets_,
        StatisticsCollectionConfig::SCALAR_MAX_STAT));
  }
  tx_bytes_ += omnetpp::check_and_cast<inet::Packet*>(packet)->getByteLength();
  if (stats_collection_flags_.enable_vector_per_port_tx_rx_bytes()) {
    CHECK_OK(stats_collection_->UpdateStatistic(
        tx_bytes_stats_name_, tx_bytes_,
        StatisticsCollectionConfig::TIME_SERIES_STAT));
  }
  // Gets the transmission delay of the packet.
  auto transmission_delay =
      transmission_channel_->getTransmissionFinishTime() - omnetpp::simTime();
  // Schedule the next output event.
  router_->ScheduleOutput(port_id_, transmission_delay + ifg_delay_);
}

void NetworkPort::ReceivePacket(omnetpp::cMessage* packet) {
  auto received_packet = DecapsulateEthernetSignal(packet);
  // Checks against MTU size.
  auto packet_size =
      omnetpp::check_and_cast<inet::Packet*>(received_packet)->getByteLength();
  if (packet_size > router_->GetMtuSize()) {
    LOG(ERROR) << "Receiving packet size is larger than MTU: " << packet_size;
    if (stats_collection_flags_.enable_per_port_ingress_discards()) {
      CHECK_OK(stats_collection_->UpdateStatistic(
          ingress_packet_discards_name_, ++ingress_packet_discards_,
          StatisticsCollectionConfig::SCALAR_MAX_STAT));
    }
    delete received_packet;
    return;
  } else {
    // Updates the RX packets statistics.
    if (stats_collection_flags_.enable_scalar_per_port_tx_rx_packets()) {
      CHECK_OK(stats_collection_->UpdateStatistic(
          rx_packets_stats_name_, ++rx_packets_,
          StatisticsCollectionConfig::SCALAR_MAX_STAT));
    }
    rx_bytes_ += omnetpp::check_and_cast<inet::Packet*>(received_packet)
                     ->getByteLength();
    if (stats_collection_flags_.enable_vector_per_port_tx_rx_bytes()) {
      CHECK_OK(stats_collection_->UpdateStatistic(
          rx_bytes_stats_name_, rx_bytes_,
          StatisticsCollectionConfig::TIME_SERIES_STAT));
    }
    RemovePhyHeader(received_packet);
  }

  if (IsFlowControlPacket(received_packet)) {
    router_->GetMemoryManagementUnit()->ReceivedPfc(port_id_, received_packet);
    delete received_packet;
  } else {
    // Removes the frame header of the packet and forwards it to routing
    // pipeline.
    DecapsulateMacHeader(received_packet);
    auto priority = GetPacketPriority(received_packet);
    IngressMemoryOccupancy memory_occupancy =
        router_->GetMemoryManagementUnit()->ReceivedData(
            port_id_, GetPacketOccupancy(received_packet), priority);

    if (!memory_occupancy.drop) {
      // Adds ingress information for the data packet, so that we can restore
      // the correct ingress counters when dequeueing the packet.
      AddIngressTag(received_packet, port_id_, priority,
                    memory_occupancy.minimal_guarantee_occupancy,
                    memory_occupancy.shared_pool_occupancy,
                    memory_occupancy.headroom_occupancy);
      router_->RoutePacket(received_packet);
    } else {
      // Updates the ingress packet discards statistic.
      if (stats_collection_flags_.enable_per_port_ingress_discards()) {
        CHECK_OK(stats_collection_->UpdateStatistic(
            ingress_packet_discards_name_, ++ingress_packet_discards_,
            StatisticsCollectionConfig::SCALAR_MAX_STAT));
      }
      delete received_packet;
    }
  }
}

void NetworkPort::EnqueuePacket(omnetpp::cMessage* packet) {
  int packet_priority = GetPacketPriority(packet);
  bool packet_enqueued = packet_queues_[packet_priority]->Enqueue(packet);

  // If the port is idle, then transitions to active and schedule an output
  // event. The port actually sends packet after a short delay because all ports
  // share the memory in router.
  if (packet_enqueued && port_state_ == PortState::kIdle) {
    port_state_ = PortState::kActive;
    router_->ScheduleOutput(
        port_id_, omnetpp::simtime_t(kEnqueueDelay, omnetpp::SIMTIME_NS));
  }
}

// if there has no packets available for transmission, then enter Idle state and
// return. Otherwise, send out the packet to the network.
void NetworkPort::ScheduleOutput() {
  CHECK(port_state_ == PortState::kActive)
      << "Wrong port state for scheduling output.";

  // First checks if we have PFC message for sending out first.
  if (!pfc_queue_.empty()) {
    auto pfc_packet = pfc_queue_.front();
    pfc_queue_.pop();
    SendPacket(pfc_packet, true);
  } else {
    // No PFC message needs to be sent out. Schedules a data packet from packet
    // queues.
    // Constructs the list of available packets from all queues for arbiter.
    std::vector<const omnetpp::cMessage*> packet_list(packet_queues_.size());
    for (int index = 0; index < packet_list.size(); index++) {
      packet_list[index] = packet_queues_[index]->Peek();
    }
    // If there has no available packet for transmitting, then set the port
    // state to Idle.
    int winner = arbiter_->Arbitrate(packet_list);
    if (winner != -1) {
      auto packet = packet_queues_[winner]->Dequeue();
      SendPacket(packet, false);
    } else {
      port_state_ = PortState::kIdle;
    }
  }
}

void NetworkPort::GeneratePfc(int priority, int16_t pause_units) {
  // If PFC queue is empty, generate a new PFC frame. Otherwise, we update the
  // existing frame.
  if (pfc_queue_.empty()) {
    omnetpp::cMessage* pfc = CreatePfcPacket(priority, pause_units);
    pfc_queue_.push(pfc);
  } else {
    CHECK(pfc_queue_.size() == 1) << "wrong PFC generation logic.";
    PfcFrameUpdate(pfc_queue_.front(), priority, pause_units);
  }

  if (port_state_ == PortState::kIdle) {
    port_state_ = PortState::kActive;
    router_->ScheduleOutput(
        port_id_, omnetpp::simtime_t(kEnqueueDelay, omnetpp::SIMTIME_NS));
  }
}

void NetworkPort::ResumeOutput() {
  if (port_state_ == PortState::kIdle) {
    port_state_ = PortState::kActive;
    ScheduleOutput();
  }
}

}  // namespace isekai
