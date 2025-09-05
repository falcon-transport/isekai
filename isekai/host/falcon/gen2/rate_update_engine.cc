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

#include "isekai/host/falcon/gen2/rate_update_engine.h"

#include <cstdint>
#include <memory>
#include <string_view>

#include "absl/log/check.h"
#include "absl/numeric/bits.h"
#include "absl/strings/substitute.h"
#include "absl/time/time.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/common/packet.h"
#include "isekai/common/status_util.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/falcon_connection_state.h"
#include "isekai/host/falcon/gen1/falcon_types.h"
#include "isekai/host/falcon/gen1/rate_update_engine.h"
#include "isekai/host/falcon/gen1/rate_update_engine_adapter.h"
#include "isekai/host/falcon/gen2/falcon_types.h"
#include "isekai/host/falcon/gen2/falcon_utils.h"
#include "isekai/host/falcon/rue/format_gen2.h"

namespace isekai {

// Flag: enable_rue_cc_metrics
// Get from RUE events.
constexpr std::string_view kStatVectorRueFabricRttFlowUs =
    "falcon.rue.fabric_rtt_us.cid$0.flowId$1";
constexpr std::string_view kStatVectorRueForwardOwdFlowUs =
    "falcon.rue.forward_owd_us.cid$0.flowId$1";
constexpr std::string_view kStatVectorRueReverseOwdFlowUs =
    "falcon.rue.reverse_owd_us.cid$0.flowId$1";
constexpr std::string_view kStatVectorRueTotalRttFlowUs =
    "falcon.rue.total_rtt_us.cid$0.flowId$1";
constexpr std::string_view kStatVectorRueNumAckedFlow =
    "falcon.rue.num_acked_count.cid$0.flowId$1";
// Get from RUE responses.
constexpr std::string_view kStatVectorRueFlowWeight =
    "falcon.rue.flow_weight.cid$0.flowId$1";

Gen2RateUpdateEngine::Gen2RateUpdateEngine(FalconModelInterface* falcon)
    : ProtocolRateUpdateEngine(falcon) {}

void Gen2RateUpdateEngine::InitializeGenSpecificMetadata(
    CongestionControlMetadata& gen1_metadata) {
  Gen2CongestionControlMetadata& metadata =
      CongestionControlMetadata::DowncastTo<Gen2CongestionControlMetadata>(
          gen1_metadata);
  //
  // here when the Gen2 connection state is implemented.
  // An initial value of 0 for PLB state sets the fields of the
  // isekai::rue::PlbState struct (packets_congestion_acknowledged,
  // packets_acknowledged, plb_reroute_attempted) to 0.
  metadata.gen2_plb_state = 0;
  // Initialize the Gen2 flow label and flow weight vectors. The function
  // expects the size of these vectors to be the number of available flows/paths
  // for that connection.
  CHECK_EQ(metadata.gen2_flow_labels.size(), metadata.gen2_flow_weights.size());
  uint32_t num_flows = metadata.gen2_flow_labels.size();
  for (int idx = 0; idx < num_flows; idx++) {
    metadata.gen2_flow_labels[idx] =
        EncodeFlowIdBitsInFlowLabel(GenerateRandomFlowLabel(), idx, num_flows);
    // Initialize all weights to same value (equal to 1) to cause vanilla
    // round-robin packet scheduling behavior across the flows. As the sender
    // receives ACKs with congestion signals from each path, these weights are
    // adjusted accordingly.
    metadata.gen2_flow_weights[idx] = 1;
    // Initialize RUE-specific fields in the connection state.
    metadata.gen2_last_rue_event_time[idx] = -absl::InfiniteDuration();
    metadata.gen2_num_acked[idx] = 0;
    metadata.gen2_outstanding_rue_event[idx] = false;
  }
}

void Gen2RateUpdateEngine::PacketTimeoutRetransmitted(
    uint32_t cid, const Packet* packet, uint8_t retransmit_count) {
  auto rue_key = std::make_unique<Gen2RueKey>(
      cid, GetFlowIdFromFlowLabel(packet->metadata.flow_label, falcon_, cid));
  ProtocolRateUpdateEngine::HandlePacketTimeoutRetransmitted(
      rue_key.get(), packet, retransmit_count);
}

uint32_t Gen2RateUpdateEngine::EncodeFlowIdBitsInFlowLabel(uint32_t flow_label,
                                                           uint8_t flow_id,
                                                           uint32_t num_flows) {
  CHECK_GT(num_flows, 0);
  CHECK_GT(num_flows, flow_id);
  // The number of fields reserved for the flow_id so that it is enough to
  // encode all num_flows.
  uint32_t flow_id_field_width = absl::bit_width(num_flows - 1);
  // A mask with its flow_id_field_width least significant bits all 1.
  uint32_t mask = ((uint32_t)1 << flow_id_field_width) - 1;
  // Set the flow_id_field_width least significant bits of flow_label to be
  // flow_id.
  return (flow_label & ~mask) | (uint32_t)flow_id;
}

absl::Duration Gen2RateUpdateEngine::GetLastEventTime(
    const RueKey* rue_key) const {
  auto gen2_rue_key = dynamic_cast<const Gen2RueKey*>(rue_key);
  CHECK_OK_THEN_ASSIGN(
      ConnectionState * connection_state,
      falcon_->get_state_manager()->PerformDirectLookup(gen2_rue_key->scid));
  Gen2CongestionControlMetadata& ccmeta =
      CongestionControlMetadata::DowncastTo<Gen2CongestionControlMetadata>(
          *connection_state->congestion_control_metadata);
  return ccmeta.gen2_last_rue_event_time[gen2_rue_key->flow_id];
}

void Gen2RateUpdateEngine::UpdateLastEventTime(const RueKey* rue_key) const {
  auto gen2_rue_key = dynamic_cast<const Gen2RueKey*>(rue_key);
  CHECK_OK_THEN_ASSIGN(
      ConnectionState * connection_state,
      falcon_->get_state_manager()->PerformDirectLookup(gen2_rue_key->scid));
  Gen2CongestionControlMetadata& ccmeta =
      CongestionControlMetadata::DowncastTo<Gen2CongestionControlMetadata>(
          *connection_state->congestion_control_metadata);
  ccmeta.gen2_last_rue_event_time[gen2_rue_key->flow_id] =
      falcon_->get_environment()->ElapsedTime();
}

bool Gen2RateUpdateEngine::GetOutstandingEvent(const RueKey* rue_key) const {
  auto gen2_rue_key = dynamic_cast<const Gen2RueKey*>(rue_key);
  CHECK_OK_THEN_ASSIGN(
      ConnectionState * connection_state,
      falcon_->get_state_manager()->PerformDirectLookup(gen2_rue_key->scid));
  Gen2CongestionControlMetadata& ccmeta =
      CongestionControlMetadata::DowncastTo<Gen2CongestionControlMetadata>(
          *connection_state->congestion_control_metadata);
  return ccmeta.gen2_outstanding_rue_event[gen2_rue_key->flow_id];
}

bool Gen2RateUpdateEngine::UpdateOutstandingEvent(const RueKey* rue_key,
                                                  bool value) const {
  auto gen2_rue_key = dynamic_cast<const Gen2RueKey*>(rue_key);
  CHECK_OK_THEN_ASSIGN(
      ConnectionState * connection_state,
      falcon_->get_state_manager()->PerformDirectLookup(gen2_rue_key->scid));
  Gen2CongestionControlMetadata& ccmeta =
      CongestionControlMetadata::DowncastTo<Gen2CongestionControlMetadata>(
          *connection_state->congestion_control_metadata);
  bool old_value = ccmeta.gen2_outstanding_rue_event[gen2_rue_key->flow_id];
  ccmeta.gen2_outstanding_rue_event[gen2_rue_key->flow_id] = value;
  return old_value != value;
}

uint32_t Gen2RateUpdateEngine::GetNumAcked(const RueKey* rue_key) const {
  auto gen2_rue_key = dynamic_cast<const Gen2RueKey*>(rue_key);
  CHECK_OK_THEN_ASSIGN(
      ConnectionState * connection_state,
      falcon_->get_state_manager()->PerformDirectLookup(gen2_rue_key->scid));
  Gen2CongestionControlMetadata& ccmeta =
      CongestionControlMetadata::DowncastTo<Gen2CongestionControlMetadata>(
          *connection_state->congestion_control_metadata);
  return ccmeta.gen2_num_acked[gen2_rue_key->flow_id];
}

void Gen2RateUpdateEngine::ResetNumAcked(const RueKey* rue_key) const {
  auto gen2_rue_key = dynamic_cast<const Gen2RueKey*>(rue_key);
  CHECK_OK_THEN_ASSIGN(
      ConnectionState * connection_state,
      falcon_->get_state_manager()->PerformDirectLookup(gen2_rue_key->scid));
  Gen2CongestionControlMetadata& ccmeta =
      CongestionControlMetadata::DowncastTo<Gen2CongestionControlMetadata>(
          *connection_state->congestion_control_metadata);
  ccmeta.gen2_num_acked[gen2_rue_key->flow_id] = 0;
}

void Gen2RateUpdateEngine::InitializeDelayState(
    CongestionControlMetadata& metadata) const {
  metadata.delay_state = ToFalconTimeUnits(kDefaultDelayState);
}

void Gen2RateUpdateEngine::CollectAckStats(const RueKey* rue_key,
                                           const Packet* packet) {
  auto gen2_rue_key = dynamic_cast<const Gen2RueKey*>(rue_key);
  uint32_t cid = gen2_rue_key->scid;
  uint8_t flow_id = gen2_rue_key->flow_id;
  StatisticCollectionInterface* stats_collector =
      falcon_->get_stats_collector();
  bool collect_cc_metrics =
      falcon_->get_stats_manager()->GetStatsConfig().enable_rue_cc_metrics();
  if (stats_collector && collect_cc_metrics) {
    double total_rtt_us = absl::ToDoubleMicroseconds(
        packet->timestamps.received_timestamp - packet->ack.timestamp_1);
    double forward_owd_us = absl::ToDoubleMicroseconds(packet->ack.timestamp_2 -
                                                       packet->ack.timestamp_1);
    double reverse_owd_us =
        absl::ToDoubleMicroseconds(packet->timestamps.received_timestamp -
                                   packet->timestamps.sent_timestamp);
    double fabric_rtt_us = forward_owd_us + reverse_owd_us;
    CHECK_OK(stats_collector->UpdateStatistic(
        absl::Substitute(kStatVectorRueFabricRttFlowUs, cid, flow_id),
        fabric_rtt_us, StatisticsCollectionConfig::TIME_SERIES_STAT));
    CHECK_OK(stats_collector->UpdateStatistic(
        absl::Substitute(kStatVectorRueForwardOwdFlowUs, cid, flow_id),
        forward_owd_us, StatisticsCollectionConfig::TIME_SERIES_STAT));
    CHECK_OK(stats_collector->UpdateStatistic(
        absl::Substitute(kStatVectorRueReverseOwdFlowUs, cid, flow_id),
        reverse_owd_us, StatisticsCollectionConfig::TIME_SERIES_STAT));
    CHECK_OK(stats_collector->UpdateStatistic(
        absl::Substitute(kStatVectorRueTotalRttFlowUs, cid, flow_id),
        total_rtt_us, StatisticsCollectionConfig::TIME_SERIES_STAT));
  }
  // Call Gen1 stat collection function.
  ProtocolRateUpdateEngine::CollectAckStats(rue_key, packet);
}

void Gen2RateUpdateEngine::CollectNumAckedStats(const RueKey* rue_key,
                                                uint32_t num_packets_acked) {
  auto gen2_rue_key = dynamic_cast<const Gen2RueKey*>(rue_key);
  uint32_t cid = gen2_rue_key->scid;
  uint8_t flow_id = gen2_rue_key->flow_id;
  StatisticCollectionInterface* stats_collector =
      falcon_->get_stats_collector();
  bool collect_cc_metrics =
      falcon_->get_stats_manager()->GetStatsConfig().enable_rue_cc_metrics();
  if (stats_collector && collect_cc_metrics) {
    CHECK_OK(stats_collector->UpdateStatistic(
        absl::Substitute(kStatVectorRueNumAckedFlow, cid, flow_id),
        num_packets_acked, StatisticsCollectionConfig::TIME_SERIES_STAT));
  }
  // Call Gen1 stat collection function.
  ProtocolRateUpdateEngine::CollectNumAckedStats(rue_key, num_packets_acked);
}

void Gen2RateUpdateEngine::CollectCongestionControlMetricsAfterResponse(
    const RueKey* rue_key, const CongestionControlMetadata& gen1_metadata,
    const ResponseMetadata& response_metadata) {
  auto gen2_rue_key = dynamic_cast<const Gen2RueKey*>(rue_key);
  const Gen2CongestionControlMetadata& metadata =
      CongestionControlMetadata::DowncastTo<Gen2CongestionControlMetadata>(
          gen1_metadata);
  uint32_t cid = gen2_rue_key->scid;
  StatisticCollectionInterface* stats_collector =
      falcon_->get_stats_collector();
  bool collect_cc_metrics =
      falcon_->get_stats_manager()->GetStatsConfig().enable_rue_cc_metrics();
  if (stats_collector && collect_cc_metrics) {
    uint32_t num_flows = metadata.gen2_flow_labels.size();
    for (int idx = 0; idx < num_flows; idx++) {
      CHECK_OK(stats_collector->UpdateStatistic(
          absl::Substitute(kStatVectorRueFlowWeight, cid, idx),
          metadata.gen2_flow_weights[idx],
          StatisticsCollectionConfig::TIME_SERIES_STAT));
    }
  }
  // Call Gen1 stat collection function.
  ProtocolRateUpdateEngine::CollectCongestionControlMetricsAfterResponse(
      rue_key, metadata, response_metadata);
}

}  // namespace isekai
