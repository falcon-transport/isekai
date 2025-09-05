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

#include "isekai/fabric/memory_management_unit.h"

#include <cstdint>
#include <memory>
#include <random>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/strings/substitute.h"
#include "absl/time/time.h"
#include "absl/types/optional.h"
#include "glog/logging.h"
#include "inet/common/packet/Packet.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/default_config_generator.h"
#include "isekai/common/status_util.h"
#include "isekai/fabric/constants.h"
#include "isekai/fabric/model_interfaces.h"
#include "isekai/fabric/packet_util.h"
#include "isekai/fabric/weighted_random_early_detection.h"
#include "omnetpp/cdataratechannel.h"
#include "omnetpp/checkandcast.h"
#include "omnetpp/csimulation.h"
#include "omnetpp/simtime.h"
#include "omnetpp/simtime_t.h"
#undef ETH_ALEN
#undef ETHER_TYPE_LEN
#undef ETHERTYPE_ARP
#include "inet/linklayer/ethernet/EtherFrame_m.h"

namespace isekai {
namespace {

// Flag: enable_per_port_per_queue_stats
// Records the queue occupancy and dynamic limit.
constexpr std::string_view kStatVectorQueueOccupancy =
    "router.port$0.queue$1.queue_occupancy";
constexpr std::string_view kStatVectorQueueDynamicLimit =
    "router.port$0.queue$1.dynamic_limit";

// Flag: enable_pfc_
// Records the PFC pause triggered and resumed events.
constexpr std::string_view kStatVectorPfcPauseTriggered =
    "router.mmu.port$0.queue$1.triggered_pfc";
constexpr std::string_view kStatVectorPfcPauseResumed =
    "router.mmu.port$0.queue$1.resumed_pfc";

constexpr uint64_t kMaxPoolSize = std::numeric_limits<uint64_t>::max();

const MemoryManagementUnitConfig_QosProfile& GetQosProfile(
    const std::string& profile_name,
    const MemoryManagementUnitConfig_BufferCarving& buffer_carving_config) {
  for (const auto& profile : buffer_carving_config.qos_configs()) {
    if (profile.profile_name() == profile_name) {
      return profile;
    }
  }
  LOG(FATAL) << "QoS profile not found.";
}

const MemoryManagementUnitConfig_CosProfile& GetCosProfile(
    const std::string& profile_name,
    const MemoryManagementUnitConfig_BufferCarving& buffer_carving_config) {
  for (const auto& profile : buffer_carving_config.cos_configs()) {
    if (profile.profile_name() == profile_name) {
      return profile;
    }
  }
  LOG(FATAL) << "CoS profile not found.";
}

const MemoryManagementUnitConfig_QosProfile& GetPortConfig(
    uint32_t port_id,
    const MemoryManagementUnitConfig_BufferCarving& buffer_carving_config) {
  for (int i = 0; i < buffer_carving_config.port_configs_size(); i++) {
    if (buffer_carving_config.port_configs(i).port_id() == port_id) {
      switch (buffer_carving_config.port_configs(i).port_config_case()) {
        case MemoryManagementUnitConfig_Port::PortConfigCase::kQosConfig: {
          return buffer_carving_config.port_configs(i).qos_config();
        }
        case MemoryManagementUnitConfig_Port::PortConfigCase::kQosProfileName: {
          auto qos_profile_name =
              buffer_carving_config.port_configs(i).qos_profile_name();
          return GetQosProfile(qos_profile_name, buffer_carving_config);
        }
        case MemoryManagementUnitConfig_Port::PortConfigCase::
            PORT_CONFIG_NOT_SET: {
          LOG(FATAL) << "no port config provided.";
        }
      }
    }
  }
  LOG(FATAL) << "no port config found.";
}

const MemoryManagementUnitConfig_CosProfile& GetQueueConfig(
    uint32_t port_id, const MemoryManagementUnitConfig_QosProfile& port_config,
    const MemoryManagementUnitConfig_BufferCarving& buffer_carving_config) {
  for (int i = 0; i < port_config.queue_configs_size(); i++) {
    if (port_config.queue_configs(i).queue_id() == port_id) {
      switch (port_config.queue_configs(i).queue_config_case()) {
        case MemoryManagementUnitConfig_PacketQueue::QueueConfigCase::
            kCosConfig: {
          return port_config.queue_configs(i).cos_config();
        }
        case MemoryManagementUnitConfig_PacketQueue::QueueConfigCase::
            kCosProfileName: {
          auto cos_profile_name =
              port_config.queue_configs(i).cos_profile_name();
          return GetCosProfile(cos_profile_name, buffer_carving_config);
        }
        case MemoryManagementUnitConfig_PacketQueue::QueueConfigCase::
            QUEUE_CONFIG_NOT_SET: {
          LOG(FATAL) << "no queue config provided.";
        }
      }
    }
  }
  LOG(FATAL) << "no queue config found.";
}
}  // namespace

StatisticsCollectionConfig::RouterFlags
    MemoryManagementUnit::stats_collection_flags_;

void MemoryManagementUnit::InitializeMaxFrameTransmissionTime(
    int32_t max_frame_size, uint32_t port_id) {
  // The port is not initialized yet, so get the channel information directly
  // from router.
  auto transmission_channel =
      omnetpp::check_and_cast_nullable<omnetpp::cDatarateChannel*>(
          router_->gate("port$o", port_id)->findTransmissionChannel());
  if (transmission_channel) {
    auto max_frame_bits_time =
        omnetpp::simtime_t(max_frame_size / transmission_channel->getDatarate())
            .inUnit(omnetpp::SIMTIME_NS);
    max_frame_transmission_time_[port_id] = max_frame_bits_time;
  }
}

void MemoryManagementUnit::InitializePfcEventMessages(uint32_t port_id,
                                                      int queue_id) {
  int16_t pfc_set_paused_type =
      static_cast<int16_t>(RouterEventType::kSetPfcPaused);
  auto pfc_set_paused_message =
      new PfcMessage(absl::StrCat(pfc_set_paused_type).c_str());
  pfc_set_paused_message->setPortId(port_id);
  pfc_set_paused_message->setPriority(queue_id);
  pfc_set_paused_message->setPauseDuration(0);
  pfc_set_paused_message->setKind(pfc_set_paused_type);
  pfc_set_paused_messages_[port_id][queue_id] = pfc_set_paused_message;

  int16_t pfc_refreshment_type =
      static_cast<int16_t>(RouterEventType::kPfcRefreshment);
  auto pfc_refreshment_message =
      new PfcMessage(absl::StrCat(pfc_refreshment_type).c_str());
  pfc_refreshment_message->setPortId(port_id);
  pfc_refreshment_message->setPriority(queue_id);
  pfc_refreshment_message->setPauseDuration(0);
  pfc_refreshment_message->setKind(pfc_refreshment_type);
  pfc_refreshment_messages_[port_id][queue_id] = pfc_refreshment_message;

  int16_t pfc_send_pause_type =
      static_cast<int16_t>(RouterEventType::kPfcPauseSend);
  auto pfc_send_pause_message =
      new PfcMessage(absl::StrCat(pfc_send_pause_type).c_str());
  pfc_send_pause_message->setPortId(port_id);
  pfc_send_pause_message->setPriority(queue_id);
  pfc_send_pause_message->setPauseDuration(0);
  pfc_send_pause_message->setKind(pfc_send_pause_type);
  pfc_send_pause_messages_[port_id][queue_id] = pfc_send_pause_message;
}

void MemoryManagementUnit::InitializeCosConfig(
    uint32_t port_id, int queue_id,
    const MemoryManagementUnitConfig_CosProfile& cos_config) {
  per_port_per_queue_egress_alpha_[port_id][queue_id] =
      cos_config.egress_alpha();
  if (support_pfc_) {
    // The ingress alpha is only needed for ingress check when PFC is enabled.
    per_port_per_queue_ingress_alpha_[port_id][queue_id] =
        cos_config.ingress_alpha();
  }

  if (cos_config.has_wred_config()) {
    memory_management_policies_[port_id].push_back(WeightedRandomEarlyDetection(
        cos_config.wred_config(), router_->intrand(ULONG_MAX)));
  } else {
    memory_management_policies_[port_id].push_back(absl::nullopt);
  }

  if (cos_config.has_ecn_marking_stage()) {
    ecn_marking_stage_[port_id][queue_id] =
        static_cast<EcnMarkingStage>(cos_config.ecn_marking_stage());
  }

  if (cos_config.has_ingress_minimal_guarantee()) {
    per_port_per_queue_ingress_minimal_guarantee_[port_id][queue_id] =
        cos_config.ingress_minimal_guarantee();
  }

  if (support_pfc_) {
    if (cos_config.has_pfc_headroom()) {
      per_port_per_queue_pfc_headroom_[port_id][queue_id] =
          cos_config.pfc_headroom();
    } else {
      LOG(FATAL) << "no PFC headroom is configured when PFC is enabled.";
    }
  }
}

MemoryManagementUnit::MemoryManagementUnit(
    NetworkRouterInterface* router, uint32_t ports_num, int queues_num,
    const MemoryManagementUnitConfig& mmu_config, uint32_t mtu_size,
    StatisticCollectionInterface* stats_collection)
    : router_(router),
      pfc_sending_triggered_(ports_num),
      pfc_send_pause_messages_(ports_num, std::vector<PfcMessage*>(queues_num)),
      pfc_set_paused_messages_(ports_num, std::vector<PfcMessage*>(queues_num)),
      tx_queue_unpause_times_(ports_num,
                              std::vector<omnetpp::simtime_t>(queues_num, 0)),
      pfc_refreshment_messages_(ports_num,
                                std::vector<PfcMessage*>(queues_num)),
      max_frame_transmission_time_(ports_num, 0),
      memory_management_policies_(ports_num),
      per_port_egress_occupancy_(ports_num, 0),
      per_port_egress_alpha_(ports_num),
      per_port_per_queue_egress_occupancy_(
          ports_num, std::vector<uint64_t>(queues_num, 0)),
      per_port_per_queue_egress_alpha_(ports_num,
                                       std::vector<double>(queues_num)),
      per_port_per_queue_ingress_occupancy_(
          ports_num, std::vector<uint64_t>(queues_num, 0)),
      per_port_per_queue_ingress_alpha_(ports_num,
                                        std::vector<double>(queues_num, 0)),
      stats_collection_(stats_collection),
      pfc_pause_triggered_ids_(ports_num, std::vector<uint64_t>(queues_num, 0)),
      pfc_pause_resumed_ids_(ports_num, std::vector<uint64_t>(queues_num, 0)),
      ecn_marking_stage_(ports_num,
                         std::vector<EcnMarkingStage>(
                             queues_num, EcnMarkingStage::kDequeueMark)),
      per_port_per_queue_ingress_minimal_guarantee_(
          ports_num, std::vector<uint64_t>(queues_num, 0)),
      per_port_per_queue_ingress_minimal_guarantee_occupancy_(
          ports_num, std::vector<uint64_t>(queues_num, 0)),
      per_port_per_queue_pfc_headroom_(ports_num,
                                       std::vector<uint64_t>(queues_num, 0)),
      per_port_per_queue_pfc_headroom_occupancy_(
          ports_num, std::vector<uint64_t>(queues_num, 0)) {
  // Initializes the bitmap for PFC.
  for (auto& bitmap : pfc_sending_triggered_) {
    bitmap.Reset(queues_num);
  }
  // Gets the PFC configs.
  if (mmu_config.has_pfc_config()) {
    support_pfc_ = true;
    pfc_xon_ = mmu_config.pfc_config().pfc_xon();
    rx_pfc_delay_ = omnetpp::simtime_t(mmu_config.pfc_config().rx_pfc_delay(),
                                       omnetpp::SIMTIME_NS);
    pfc_pause_duration_ = mmu_config.pfc_config().pause_duration();
  } else {
    support_pfc_ = false;
  }

  // Initializes per-port max frame bits time.
  for (int port_id = 0; port_id < ports_num; port_id++) {
    InitializeMaxFrameTransmissionTime(mtu_size * 8, port_id);
  }

  // Gets the configs for ingress/egress service pool.
  CHECK(mmu_config.has_buffer_carving_config())
      << "no config for buffer carving.";
  if (support_pfc_) {
    ingress_pool_free_cells_ =
        mmu_config.buffer_carving_config().ingress_service_pool_size();
  } else {
    // If PFC is not enabled, we do not do ingress check. Set the ingress pool
    // size to infinite.
    ingress_pool_free_cells_ = kMaxPoolSize;
  }
  egress_pool_free_cells_ =
      mmu_config.buffer_carving_config().egress_service_pool_size();

  // Gets per-port buffer carving configs.
  CHECK(mmu_config.buffer_carving_config().port_configs_size() >= ports_num)
      << "not enough number of port configs.";
  for (int port_id = 0; port_id < ports_num; port_id++) {
    auto port_config =
        GetPortConfig(port_id, mmu_config.buffer_carving_config());
    per_port_egress_alpha_[port_id] = port_config.egress_alpha();

    // If the port is not connected, no need to allocate resources for it.
    if (!router_->IsPortConnected(port_id)) {
      continue;
    }

    // Gets per-queue buffer carving configs.
    CHECK(port_config.queue_configs_size() >= queues_num)
        << "not enough number of queue configs.";
    for (int queue_id = 0; queue_id < queues_num; queue_id++) {
      auto queue_config = GetQueueConfig(queue_id, port_config,
                                         mmu_config.buffer_carving_config());
      // Initializes per-queue PFC event messages if PFC is supported.
      if (support_pfc_) {
        InitializePfcEventMessages(port_id, queue_id);
      }
      InitializeCosConfig(port_id, queue_id, queue_config);
    }
  }
  if (stats_collection && stats_collection->GetConfig().has_router_flags()) {
    stats_collection_flags_ = stats_collection->GetConfig().router_flags();
  } else {
    stats_collection_flags_ = DefaultConfigGenerator::DefaultRouterStatsFlags();
  }
}

MemoryManagementUnit::~MemoryManagementUnit() {
  for (int i = 0; i < pfc_set_paused_messages_.size(); i++) {
    for (int j = 0; j < pfc_set_paused_messages_[i].size(); j++) {
      router_->cancelAndDelete(pfc_set_paused_messages_[i][j]);
      router_->cancelAndDelete(pfc_refreshment_messages_[i][j]);
      router_->cancelAndDelete(pfc_send_pause_messages_[i][j]);
    }
  }
}

IngressMemoryOccupancy MemoryManagementUnit::ReceivedData(uint32_t port_id,
                                                          uint64_t packet_size,
                                                          int priority) {
  IngressMemoryOccupancy memory_occupancy(true, 0, 0, 0);

  if (!support_pfc_) {
    // If PFC is not supported, we do not do ingress check.
    memory_occupancy.drop = false;
    return memory_occupancy;
  }

  if (!pfc_sending_triggered_[port_id].get(priority)) {
    // PFC Trigger Limit = Dynamic Limit + Min Guarantee
    auto pfc_trigger_limit =
        ingress_pool_free_cells_ *
            per_port_per_queue_ingress_alpha_[port_id][priority] +
        per_port_per_queue_ingress_minimal_guarantee_[port_id][priority];
    if (per_port_per_queue_ingress_occupancy_[port_id][priority] >=
            pfc_trigger_limit ||
        ingress_pool_free_cells_ < packet_size) {
      // Enables continual sending of PFCs
      EnablePfcSending(port_id, priority);
    }
  }

  if (!pfc_sending_triggered_[port_id].get(priority)) {
    // If PFC is not triggered, the received packet should be placed in ingress
    // minimal guarantee or/and ingress pool.
    if (per_port_per_queue_ingress_occupancy_[port_id][priority] >=
        per_port_per_queue_ingress_minimal_guarantee_[port_id][priority]) {
      // The per-queue ingress occupancy exceeds the minimal guarantee, so the
      // whole packet should be placed in shared ingress pool.
      if (ingress_pool_free_cells_ < packet_size) {
        LOG(FATAL) << "PFC is enabled, but not enough ingress pool space for "
                      "ingress traffic."
                   << " ingress_pool_free_cells_: " << ingress_pool_free_cells_
                   << " per_port_per_queue_ingress_occupancy_: "
                   << per_port_per_queue_ingress_occupancy_[port_id][priority]
                   << " @port: " << port_id << " @priority: " << priority;
      }

      ingress_pool_free_cells_ -= packet_size;
      memory_occupancy.shared_pool_occupancy = packet_size;
    } else {
      // The minimal guarantee has room to admit the received packet.
      if (per_port_per_queue_ingress_minimal_guarantee_occupancy_[port_id]
                                                                 [priority] +
              packet_size <=
          per_port_per_queue_ingress_minimal_guarantee_[port_id][priority]) {
        // The whole packet can be placed in the minimal guarantee.
        per_port_per_queue_ingress_minimal_guarantee_occupancy_[port_id]
                                                               [priority] +=
            packet_size;
        memory_occupancy.minimal_guarantee_occupancy = packet_size;
      } else {
        // Part of the packet is placed in the minimal guarantee, while the rest
        // is placed in the shared ingress pool.
        uint64_t pool_occupancy =
            per_port_per_queue_ingress_minimal_guarantee_occupancy_[port_id]
                                                                   [priority] +
            packet_size -
            per_port_per_queue_ingress_minimal_guarantee_[port_id][priority];
        per_port_per_queue_ingress_minimal_guarantee_occupancy_
            [port_id][priority] =
                per_port_per_queue_ingress_minimal_guarantee_[port_id]
                                                             [priority];
        memory_occupancy.shared_pool_occupancy = pool_occupancy;
        memory_occupancy.minimal_guarantee_occupancy =
            per_port_per_queue_ingress_minimal_guarantee_[port_id][priority] -
            per_port_per_queue_ingress_minimal_guarantee_occupancy_[port_id]
                                                                   [priority];
        CHECK_GE(ingress_pool_free_cells_, pool_occupancy);
        ingress_pool_free_cells_ -= pool_occupancy;
      }
    }
  } else {
    // If PFC is already triggered, the received packet should be placed in the
    // headroom.
    if (per_port_per_queue_pfc_headroom_[port_id][priority] <
        per_port_per_queue_pfc_headroom_occupancy_[port_id][priority] +
            packet_size) {
      LOG(WARNING) << "PFC headroom is not big enough to absorb packets.";
      // The memory_occupancy.drop is true, and based on this value, the
      // network_port will drop the packet and record the corresponding ingress
      // packet discards stat.
      return memory_occupancy;
    }

    per_port_per_queue_pfc_headroom_occupancy_[port_id][priority] +=
        packet_size;
    memory_occupancy.headroom_occupancy = packet_size;
  }

  per_port_per_queue_ingress_occupancy_[port_id][priority] += packet_size;
  VLOG(2) << "=======>Received Data @ " << port_id << " : " << priority
          << " @time: " << omnetpp::simTime().str() << " @ingress occupancy: "
          << per_port_per_queue_ingress_occupancy_[port_id][priority]
          << " @ingress_pool_free_cells_:" << ingress_pool_free_cells_;

  memory_occupancy.drop = false;
  return memory_occupancy;
}

void MemoryManagementUnit::ReceivedPfc(uint32_t port_id,
                                       omnetpp::cMessage* packet) {
  auto inet_packet = omnetpp::check_and_cast<inet::Packet*>(packet);
  inet_packet->popAtFront<inet::EthernetMacHeader>();
  const auto& control_frame =
      inet_packet->peekAtFront<inet::EthernetPfcFrame>();

  int16_t class_enable = control_frame->getClassEnable();
  for (int i = 0; i < kClassesOfService; i++) {
    if ((class_enable >> i) & 1) {
      pfc_set_paused_messages_[port_id][i]->setPauseDuration(
          ConvertToPauseDuration(port_id, control_frame->getTimeClass(i)));
      // Tells router to start a pfc_set_paused event (the PFC pause delay is
      // modeled).
      router_->SetPfcPaused(pfc_set_paused_messages_[port_id][i],
                            rx_pfc_delay_);
    }
  }
}

bool MemoryManagementUnit::RequestEnqueue(uint32_t port_id, int priority,
                                          omnetpp::cMessage* packet) {
  if (stats_collection_flags_.enable_per_port_per_queue_stats()) {
    CHECK_OK(stats_collection_->UpdateStatistic(
        absl::Substitute(kStatVectorQueueOccupancy, port_id, priority),
        per_port_per_queue_egress_occupancy_[port_id][priority],
        StatisticsCollectionConfig::TIME_SERIES_STAT));
    CHECK_OK(stats_collection_->UpdateStatistic(
        absl::Substitute(kStatVectorQueueDynamicLimit, port_id, priority),
        egress_pool_free_cells_ *
            per_port_per_queue_egress_alpha_[port_id][priority],
        StatisticsCollectionConfig::TIME_SERIES_STAT));
  }

  auto packet_occupancy = GetPacketOccupancy(packet);
  // First check if there is enough global buffer space to enqueue the packet.
  if (egress_pool_free_cells_ < packet_occupancy) {
    VLOG(2) << "######reach global limit, drop packets!";
    if (support_pfc_) {
      LOG(FATAL) << "packet loss while PFC is enabled (global_limit)."
                 << " egress_pool_free_cells_: " << egress_pool_free_cells_
                 << " packet_occupancy: " << packet_occupancy;
    }
    return false;
  }

  // Second check if port limit is exceeded or not. If the limit is exceeded,
  // meaning the packet can not be enqueued to Tx queue and will be dropped. The
  // corresponding egress packet discard stat is upadated in
  // network_packet_queue.
  if (per_port_egress_occupancy_[port_id] + packet_occupancy >=
      egress_pool_free_cells_ * per_port_egress_alpha_[port_id]) {
    VLOG(2) << "######reach port limit, drop packets!";
    if (support_pfc_) {
      LOG(FATAL) << "packet loss while PFC is enabled (port_limit)."
                 << " per_port_egress_occupancy_: "
                 << per_port_egress_occupancy_[port_id]
                 << " egress_pool_free_cells_: " << egress_pool_free_cells_
                 << " @port: " << port_id;
    }
    return false;
  }

  // Then check if queue limit is exceeded or not.
  if (per_port_per_queue_egress_occupancy_[port_id][priority] +
          packet_occupancy >=
      egress_pool_free_cells_ *
          per_port_per_queue_egress_alpha_[port_id][priority]) {
    VLOG(2) << "######reach queue limit, drop packets!";
    if (support_pfc_) {
      LOG(FATAL) << "packet loss while PFC is enabled (queue_limit)."
                 << " per_port_per_queue_egress_occupancy_: "
                 << per_port_per_queue_egress_occupancy_[port_id][priority]
                 << " egress_pool_free_cells_: " << egress_pool_free_cells_
                 << " @port: " << port_id << " @priority: " << priority;
    }
    return false;
  }

  // At last determine if mark the packet or not based on WRED.
  if (ecn_marking_stage_[port_id][priority] == EcnMarkingStage::kEnqueueMark) {
    PerformEcnMarking(port_id, priority, packet);
  }

  CHECK_GE(egress_pool_free_cells_, packet_occupancy)
      << "Not enough egress free cells.";
  egress_pool_free_cells_ -= packet_occupancy;
  per_port_egress_occupancy_[port_id] += packet_occupancy;
  per_port_per_queue_egress_occupancy_[port_id][priority] += packet_occupancy;
  return true;
}

bool MemoryManagementUnit::CanTransmit(uint32_t port_id, int priority) {
  // Checks if the packet queue is empty.
  auto port = router_->GetPort(port_id);
  auto packet_queue = port->GetPacketQueue(priority);
  if (packet_queue->Size() == 0) {
    return false;
  }

  if (!support_pfc_) {
    return true;
  }

  // The tx queue is in PAUSE state, and thus it can not transmit packets.
  return IsPaused(port_id, priority) ? false : true;
}

void MemoryManagementUnit::DequeuedPacket(uint32_t port_id, int priority,
                                          omnetpp::cMessage* packet) {
  if (ecn_marking_stage_[port_id][priority] == EcnMarkingStage::kDequeueMark) {
    PerformEcnMarking(port_id, priority, packet);
  }

  auto packet_occupancy = GetPacketOccupancy(packet);

  if (support_pfc_) {
    auto ingress_port_id = GetPacketIngressPort(packet);
    auto ingress_priority = GetPacketIngressPriority(packet);
    // Restores the ingress counting.
    IngressAccountingRelease(packet);
    VLOG(2) << "<======Dequeued Packet @ " << ingress_port_id << " : "
            << ingress_priority << " @size: " << packet_occupancy
            << " @time: " << omnetpp::simTime().str() << " @ingress occupancy: "
            << per_port_per_queue_ingress_occupancy_[ingress_port_id]
                                                    [ingress_priority];

    // Checks if need to disale pfc sending.
    if (pfc_sending_triggered_[ingress_port_id].get(ingress_priority) &&
        per_port_per_queue_ingress_occupancy_[ingress_port_id]
                                             [ingress_priority] <
            ingress_pool_free_cells_ *
                    per_port_per_queue_ingress_alpha_[ingress_port_id]
                                                     [ingress_priority] +
                per_port_per_queue_ingress_minimal_guarantee_
                    [ingress_port_id][ingress_priority]) {
      DisablePfcSending(ingress_port_id, ingress_priority);
    }
  }

  // Restores the egress counters.
  egress_pool_free_cells_ += packet_occupancy;
  CHECK_GE(per_port_egress_occupancy_[port_id], packet_occupancy)
      << "wrong port egress counting. Port id: " << port_id;
  per_port_egress_occupancy_[port_id] -= packet_occupancy;
  CHECK_GE(per_port_per_queue_egress_occupancy_[port_id][priority],
           packet_occupancy)
      << "wrong per-port per-queue egress counting. Port(queue)" << port_id
      << " : " << priority;
  per_port_per_queue_egress_occupancy_[port_id][priority] -= packet_occupancy;
  if (memory_management_policies_[port_id][priority].has_value()) {
    if (per_port_per_queue_egress_occupancy_[port_id][priority] == 0) {
      memory_management_policies_[port_id][priority]
          .value()
          .UpdateQueueIdleTime(absl::Nanoseconds(
              omnetpp::simTime().inUnit(omnetpp::SIMTIME_NS)));
    }
  }
}

void MemoryManagementUnit::SetPfcPaused(
    uint32_t port_id, int priority, const omnetpp::simtime_t& unpause_time) {
  auto pfc_refreshment_msg = pfc_refreshment_messages_[port_id][priority];
  if (pfc_refreshment_msg->isScheduled()) {
    router_->cancelEvent(pfc_refreshment_msg);
  }
  tx_queue_unpause_times_[port_id][priority] = unpause_time;
  if (unpause_time != omnetpp::simTime()) {
    // if unpause_time == the current simulation time, it is an X-on PFC frame.
    // So no need to refresh the PFC pause state.
    router_->scheduleAt(unpause_time, pfc_refreshment_msg);
  }
}

void MemoryManagementUnit::EnablePfcSending(uint32_t port_id, int priority) {
  VLOG(2) << "MMU Trigger PFC: " << port_id << " : " << priority
          << " @time: " << omnetpp::simTime().str()
          << " with ingress free cells: " << ingress_pool_free_cells_;
  if (stats_collection_flags_.enable_pfc_stats() && stats_collection_) {
    CHECK_OK(stats_collection_->UpdateStatistic(
        absl::Substitute(kStatVectorPfcPauseTriggered, port_id, priority),
        pfc_pause_triggered_ids_[port_id][priority],
        StatisticsCollectionConfig::TIME_SERIES_STAT));
    pfc_pause_triggered_ids_[port_id][priority] += 1;
  }
  pfc_sending_triggered_[port_id].set(priority);
  SendPfc(port_id, priority);
}

void MemoryManagementUnit::SendPfc(uint32_t port_id, int priority) {
  if (!pfc_sending_triggered_[port_id].get(priority)) {
    return;
  }
  // The next_pfc_sending_time should guarantee that the next pfc pause frame
  // arrives at the sender before the end of the last pause frame's duration.
  omnetpp::simtime_t next_pfc_sending_time =
      omnetpp::simTime() +
      omnetpp::simtime_t(
          pfc_pause_duration_ - max_frame_transmission_time_[port_id],
          omnetpp::SIMTIME_NS);
  // Gets the time to finish transmitting the current frame if there is any.
  auto transmission_channel =
      router_->GetPort(port_id)->GetTransmissionChannel();
  auto transmission_finish_time =
      transmission_channel->getTransmissionFinishTime();
  if (transmission_finish_time > omnetpp::simTime()) {
    next_pfc_sending_time += transmission_finish_time - omnetpp::simTime();
  }

  router_->GetPort(port_id)->GeneratePfc(
      priority, ConvertToPauseQuanta(port_id, pfc_pause_duration_));
  // Schedules the next PFC sending event.
  auto pfc_send_pause_msg = pfc_send_pause_messages_[port_id][priority];
  if (pfc_send_pause_msg->isScheduled()) {
    // This may happen if PFC is triggered again while the previous
    // PFC_send_pause event is not executed yet.
    router_->cancelEvent(pfc_send_pause_msg);
  }
  router_->scheduleAt(next_pfc_sending_time, pfc_send_pause_msg);
}

void MemoryManagementUnit::DisablePfcSending(uint32_t port_id, int priority) {
  VLOG(2) << "MMU Stop PFC " << port_id << " : " << priority
          << " @time: " << omnetpp::simTime().str()
          << " with ingress free cells: " << ingress_pool_free_cells_;
  if (stats_collection_flags_.enable_pfc_stats() && stats_collection_) {
    CHECK_OK(stats_collection_->UpdateStatistic(
        absl::Substitute(kStatVectorPfcPauseResumed, port_id, priority),
        pfc_pause_resumed_ids_[port_id][priority],
        StatisticsCollectionConfig::TIME_SERIES_STAT));
    pfc_pause_resumed_ids_[port_id][priority] += 1;
  }
  pfc_sending_triggered_[port_id].clear(priority);
  if (pfc_xon_) {
    router_->GetPort(port_id)->GeneratePfc(priority, 0);
  }
}

void MemoryManagementUnit::PerformEcnMarking(uint32_t port_id, int priority,
                                             omnetpp::cMessage* packet) {
  EcnCode packet_ecn_code = PacketGetEcn(packet);
  if (memory_management_policies_[port_id][priority].has_value() &&
      packet_ecn_code != EcnCode::kNonEct) {
    if (packet_ecn_code != EcnCode::kCongestionEncountered &&
        memory_management_policies_[port_id][priority].value().PerformWred(
            per_port_per_queue_egress_occupancy_[port_id][priority]) ==
            WredResult::kEcnMark) {
      PacketSetEcn(packet, EcnCode::kCongestionEncountered);
    }
  }
}

void MemoryManagementUnit::IngressAccountingRelease(omnetpp::cMessage* packet) {
  uint32_t ingress_port_id;
  int ingress_priority;
  uint64_t minimal_guarantee_occupancy;
  uint64_t shared_pool_occupancy;
  uint64_t headroom_occupancy;
  GetAndRemoveIngressTag(packet, ingress_port_id, ingress_priority,
                         minimal_guarantee_occupancy, shared_pool_occupancy,
                         headroom_occupancy);
  auto packet_occupancy = GetPacketOccupancy(packet);
  // Check if the ingress packet metadata carries the correct packet memory
  // occupancy.
  CHECK_EQ(packet_occupancy, minimal_guarantee_occupancy +
                                 shared_pool_occupancy + headroom_occupancy);
  // Restores the minimal guarantee occupancy.
  CHECK_GE(
      per_port_per_queue_ingress_minimal_guarantee_occupancy_[ingress_port_id]
                                                             [ingress_priority],
      minimal_guarantee_occupancy);
  per_port_per_queue_ingress_minimal_guarantee_occupancy_[ingress_port_id]
                                                         [ingress_priority] -=
      minimal_guarantee_occupancy;
  // Restores the headroom occupancy.
  CHECK_GE(per_port_per_queue_pfc_headroom_occupancy_[ingress_port_id]
                                                     [ingress_priority],
           headroom_occupancy);
  per_port_per_queue_pfc_headroom_occupancy_[ingress_port_id]
                                            [ingress_priority] -=
      headroom_occupancy;
  // Restores the shared ingress pool occupancy.
  ingress_pool_free_cells_ += shared_pool_occupancy;
  // Restores the overall ingress occupancy counter.
  CHECK_GE(
      per_port_per_queue_ingress_occupancy_[ingress_port_id][ingress_priority],
      packet_occupancy)
      << "wrong per-port per-queue ingress counting. Port(queue)"
      << ingress_port_id << " : " << ingress_priority;
  per_port_per_queue_ingress_occupancy_[ingress_port_id][ingress_priority] -=
      packet_occupancy;
}

}  // namespace isekai
