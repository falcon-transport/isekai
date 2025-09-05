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

#ifndef ISEKAI_FABRIC_MEMORY_MANAGEMENT_UNIT_H_
#define ISEKAI_FABRIC_MEMORY_MANAGEMENT_UNIT_H_

#include <cstdint>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "absl/types/optional.h"
#include "gtest/gtest_prod.h"
#include "isekai/common/bitmap.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/fabric/constants.h"
#include "isekai/fabric/memory_management_config.pb.h"
#include "isekai/fabric/model_interfaces.h"
#include "isekai/fabric/weighted_random_early_detection.h"
#include "omnetpp/cmessage.h"
#include "omnetpp/csimulation.h"
#include "omnetpp/simtime.h"
#include "omnetpp/simtime_t.h"

namespace isekai {

enum class EcnMarkingStage : uint16_t {
  kEnqueueMark = 0,
  kDequeueMark = 1,
};

// The memory management unit should support (1) PFC; (2) ECN marking.
class MemoryManagementUnit : public MemoryManagementUnitInterface {
 public:
  MemoryManagementUnit(NetworkRouterInterface* router, uint32_t ports_num,
                       int queues_num,
                       const MemoryManagementUnitConfig& mmu_config,
                       uint32_t mtu_size,
                       StatisticCollectionInterface* stats_collection);
  ~MemoryManagementUnit() override;
  // This function is called by network port once a data packet is received. The
  // port_id and priority are ingress direction.
  IngressMemoryOccupancy ReceivedData(uint32_t port_id, uint64_t packet_size,
                                      int priority) override;
  // This function is called by network port once a PFC frame is received.
  void ReceivedPfc(uint32_t port_id, omnetpp::cMessage* packet) override;
  // The port_id and priority are egress direction.
  bool RequestEnqueue(uint32_t port_id, int priority,
                      omnetpp::cMessage* packet) override;
  bool CanTransmit(uint32_t port_id, int priority) override;
  // This function is called by network packet queue once a packet is poped from
  // the queue. The port_id and priority are egress direction.
  void DequeuedPacket(uint32_t port_id, int priority,
                      omnetpp::cMessage* packet) override;
  void SetPfcPaused(uint32_t port_id, int priority,
                    const omnetpp::simtime_t& unpause_time) override;
  // Starts to send PFC pause frames periodically.
  void SendPfc(uint32_t port_id, int priority) override;

 private:
  FRIEND_TEST(MmuPfcTest, HandleXonTest);
  FRIEND_TEST(MmuPfcTest, HandleXoffTest);
  FRIEND_TEST(MmuPfcTest, TurnPfsomethingdOff);
  FRIEND_TEST(MmuPfcTest, TriggerXonTest);
  FRIEND_TEST(MmuPfcTest, TriggerXoffTest);
  FRIEND_TEST(MmuTest, IngressPacketDropTest);
  FRIEND_TEST(MmuTest, EgressPacketDropTest);

  // Enables continual sending of PFCs.
  void EnablePfcSending(uint32_t port_id, int priority);
  // Turns off continual sending of PFCs.
  void DisablePfcSending(uint32_t port_id, int priority);

  bool IsPaused(uint32_t port_id, int priority) {
    return tx_queue_unpause_times_[port_id][priority] > omnetpp::simTime();
  }

  int64_t ConvertToPauseDuration(uint32_t port_id, int pause_quanta) {
    omnetpp::simtime_t pause_duration =
        (pause_quanta * kPauseUnitBits) /
        router_->GetPort(port_id)->GetTransmissionChannel()->getDatarate();
    return pause_duration.inUnit(omnetpp::SIMTIME_NS);
  }

  int16_t ConvertToPauseQuanta(uint32_t port_id, int64_t pause_duration) {
    return std::ceil(
        router_->GetPort(port_id)->GetTransmissionChannel()->getDatarate() *
        pause_duration * 1e-9 / kPauseUnitBits);
  }

  // Restores the ingress counting if packet is dropped.
  void IngressAccountingRelease(omnetpp::cMessage* packet);
  void InitializeMaxFrameTransmissionTime(int32_t max_frame_size,
                                          uint32_t port_id);
  void InitializePfcEventMessages(uint32_t port_id, int queue_id);
  void InitializeCosConfig(
      uint32_t port_id, int queue_id,
      const MemoryManagementUnitConfig_CosProfile& cos_config);
  void PerformEcnMarking(uint32_t port_id, int priority,
                         omnetpp::cMessage* packet);

  NetworkRouterInterface* const router_;
  // A configurable parameter to turn on/off PFC support in MMU.
  bool support_pfc_ = false;
  // Per-port per-queue continual sending of PFCs triggered.
  std::vector<Bitmap> pfc_sending_triggered_;
  // The OMNest self-message to schedule the event of sending PFC pause frames
  // (per-port per-queue).
  std::vector<std::vector<PfcMessage*>> pfc_send_pause_messages_;
  // The OMNest self-message to schedule the event of setting PFC pause duration
  // (per-port per-queue).
  std::vector<std::vector<PfcMessage*>> pfc_set_paused_messages_;
  // Per-port per-queue pfc unpause time.
  std::vector<std::vector<omnetpp::simtime_t>> tx_queue_unpause_times_;
  // The OMNest self-message to schedule the event of refreshing the PFC pause
  // end time (per-port per-queue).
  std::vector<std::vector<PfcMessage*>> pfc_refreshment_messages_;
  // The value is true if explicitly sending out PFC X-on frame, otherwise
  // false.
  bool pfc_xon_;
  // The delay to set PFC pause.
  omnetpp::simtime_t rx_pfc_delay_;
  // The predefined PFC pause duration (in nanoseconds).
  // The pfc_pause_duration_ > max_frame_transmission_time_ + rx_pfc_delay_.
  int64_t pfc_pause_duration_;
  // The time to transmit a frame with max size in nanoseconds (the max frame
  // size is configurable).
  std::vector<int64_t> max_frame_transmission_time_;
  // Per-queue memory management policy (Weighted Random Early Detection - WRED)
  // for ECN marking.
  std::vector<std::vector<absl::optional<WeightedRandomEarlyDetection>>>
      memory_management_policies_;
  uint64_t ingress_pool_free_cells_;
  uint64_t egress_pool_free_cells_;
  // Per-port egress alpha and occupancy.
  std::vector<uint64_t> per_port_egress_occupancy_;
  std::vector<double> per_port_egress_alpha_;
  // Per-port per-queue egress alpha and occupancy.
  std::vector<std::vector<uint64_t>> per_port_per_queue_egress_occupancy_;
  std::vector<std::vector<double>> per_port_per_queue_egress_alpha_;
  // Per-port per-queue ingress alpha and occupancy.
  std::vector<std::vector<uint64_t>> per_port_per_queue_ingress_occupancy_;
  std::vector<std::vector<double>> per_port_per_queue_ingress_alpha_;
  // Records MMU stats, e.g., PFC events.
  StatisticCollectionInterface* const stats_collection_;
  static StatisticsCollectionConfig::RouterFlags stats_collection_flags_;
  std::vector<std::vector<uint64_t>> pfc_pause_triggered_ids_;
  std::vector<std::vector<uint64_t>> pfc_pause_resumed_ids_;
  // The packet can be marked in either enqueue or dequeue time.
  std::vector<std::vector<EcnMarkingStage>> ecn_marking_stage_;
  // Per-port per-queue ingress minimal guarantee (in cells).
  std::vector<std::vector<uint64_t>>
      per_port_per_queue_ingress_minimal_guarantee_;
  std::vector<std::vector<uint64_t>>
      per_port_per_queue_ingress_minimal_guarantee_occupancy_;
  // Per-port per-queue pfc headroom to admit the packets that may be received
  // after sending PFC message.
  std::vector<std::vector<uint64_t>> per_port_per_queue_pfc_headroom_;
  std::vector<std::vector<uint64_t>> per_port_per_queue_pfc_headroom_occupancy_;
};

}  // namespace isekai

#endif  // ISEKAI_FABRIC_MEMORY_MANAGEMENT_UNIT_H_
