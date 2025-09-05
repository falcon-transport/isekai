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

#ifndef ISEKAI_HOST_FALCON_STATS_MANAGER_H_
#define ISEKAI_HOST_FALCON_STATS_MANAGER_H_

#include <cstdint>
#include <string_view>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/common/packet.h"
#include "isekai/host/falcon/falcon.h"
#include "isekai/host/falcon/falcon_bitmap.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/falcon_counters.h"
#include "isekai/host/falcon/falcon_histograms.h"
#include "isekai/host/falcon/falcon_resource_credits.h"
#include "isekai/host/falcon/gen1/connection_scheduler_types.h"
#include "isekai/host/falcon/gen1/falcon_types.h"

namespace isekai {

class FalconStatsManager : public StatsManager {
 public:
  explicit FalconStatsManager(FalconModelInterface* falcon);

  FalconConnectionCounters& GetConnectionCounters(uint32_t cid) {
    return connection_counters_[cid];
  }

  FalconHostCounters& GetHostCounters() { return host_counters_; }

  FalconHistogramCollector* GetHistogramCollector() {
    return &histogram_collector_;
  }

  StatisticsCollectionConfig::FalconFlags& GetStatsConfig() {
    return stats_collection_flags_;
  }

  void UpdateUlpRxCounters(Packet::Rdma::Opcode opcode, uint32_t cid) override;
  void UpdateUlpTxCounters(Packet::Rdma::Opcode opcode, uint32_t cid) override;
  void UpdateNetworkRxCounters(falcon::PacketType type, uint32_t cid) override;
  void UpdateNetworkTxCounters(falcon::PacketType type, uint32_t cid,
                               bool is_retransmission,
                               RetransmitReason retx_reason) override;
  void UpdateMaxTransmissionCount(uint32_t attempts) override;
  void UpdateRueEventCounters(uint32_t cid, falcon::RueEventType event,
                              bool eack, bool eack_drop) override;
  void UpdateRueResponseCounters(uint32_t cid) override;
  void UpdateRueDroppedEventCounters(uint32_t cid, falcon::RueEventType event,
                                     bool eack, bool eack_drop) override;
  void UpdateRueEnqueueAttempts(uint32_t cid) override;
  void UpdateNetworkRxDropCounters(falcon::PacketType type, uint32_t cid,
                                   absl::Status) override;
  void UpdateSolicitationCounters(uint32_t cid, uint64_t window_bytes,
                                  bool is_release) override;
  void UpdateRequestOrDataWindowUsage(WindowType type, uint32_t cid,
                                      uint64_t occupied_bytes) override;
  void UpdateAlphaCarvingDynamicLimitCounters(
      uint32_t cid, const FalconCredit& credit_limits) override;
  void UpdateResourceCounters(uint32_t cid, FalconResourceCredits credit,
                              bool is_release) override;
  void UpdateSchedulerCounters(SchedulerTypes scheduler_type,
                               bool is_dequed) override;
  void UpdateIntraConnectionSchedulerCounters(uint32_t cid,
                                              PacketTypeQueue queue_type,
                                              bool is_dequed) override;
  void UpdateCwndPauseCounters(uint32_t cid, bool is_paused) override;
  void UpdateAcksGeneratedCounterDueToAR(uint32_t cid) override;
  void UpdateAcksGeneratedCounterDueToTimeout(uint32_t cid) override;
  void UpdateAcksGeneratedCounterDueToCoalescingCounter(uint32_t cid) override;

  void UpdateInitialTxRsnSeries(uint32_t cid, uint32_t rsn,
                                falcon::PacketType type) override;
  void UpdateRxFromUlpRsnSeries(uint32_t cid, uint32_t rsn,
                                falcon::PacketType type) override;
  void UpdateRetxRsnSeries(uint32_t cid, uint32_t rsn, falcon::PacketType type,
                           RetransmitReason retx_reason) override;
  void UpdateNetworkAcceptedRsnSeries(uint32_t cid, uint32_t accepted_rsn,
                                      falcon::PacketType type) override;
  void UpdateMaxRsnDistance(uint32_t cid, uint32_t rsn_difference) override;

  void UpdatePacketBuilderXoff(bool xoff) override;
  void UpdateRdmaXoff(uint8_t bifurcation_id, bool xoff) override;
  void UpdatePacketBuilderTxBytes(uint32_t cid,
                                  uint32_t pkt_size_bytes) override;
  void UpdatePacketBuilderRxBytes(uint32_t cid,
                                  uint32_t pkt_size_bytes) override;

 protected:
  FalconModelInterface* const falcon_;

 private:
  void CollectScalarStats(std::string_view stat_name, double value);
  void CollectVectorStats(std::string_view stat_name, double value);

  FalconHistogramCollector histogram_collector_;

  // Set of counters for each connection.
  absl::flat_hash_map<uint32_t, FalconConnectionCounters> connection_counters_;
  // Host level Falcon counters.
  FalconHostCounters host_counters_;

  StatisticCollectionInterface* const stats_collector_;
  static StatisticsCollectionConfig::FalconFlags stats_collection_flags_;
};

}  // namespace isekai

#endif  // ISEKAI_HOST_FALCON_STATS_MANAGER_H_
