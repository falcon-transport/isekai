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

#include "isekai/host/falcon/event_response_format_adapter.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string_view>

#include "absl/log/check.h"
#include "absl/strings/substitute.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/common/packet.h"
#include "isekai/host/falcon/falcon.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/falcon_connection_state.h"
#include "isekai/host/falcon/gen1/falcon_types.h"
#include "isekai/host/falcon/gen2/connection_state.h"
#include "isekai/host/falcon/gen2/falcon_types.h"
#include "isekai/host/falcon/gen2/reliability_manager.h"
#include "isekai/host/falcon/gen3/falcon_types.h"
#include "isekai/host/falcon/rue/algorithm/swift.h"
#include "isekai/host/falcon/rue/algorithm/swift.pb.h"
#include "isekai/host/falcon/rue/bits.h"
#include "isekai/host/falcon/rue/fixed.h"
#include "isekai/host/falcon/rue/format.h"
#include "isekai/host/falcon/rue/format_gen2.h"
#include "isekai/host/falcon/rue/format_gen3.h"

namespace isekai {

// Flag: enable_rue_cc_metrics
// Get from RUE responses.
constexpr std::string_view kStatVectorRerouteCountFlow =
    "falcon.rue.reroute_count.cid$0.flowId$1";

// Convert Gen_3 Response to Gen_2 Response to avoid code repetition. Some
// fields are not convertible. Gen2-specific fields (flow_weights,
// wrr_restart_round) are filled with default values.
static void ConvertGen3ResponseToGen2Response(
    falcon_rue::Response_Gen2* response_gen2,
    const falcon_rue::Response_Gen3* response,
    const CongestionControlMetadata& gen1_cc_metadata) {
  const Gen3CongestionControlMetadata& cc_metadata =
      CongestionControlMetadata::DowncastTo<Gen3CongestionControlMetadata>(
          gen1_cc_metadata);
  response_gen2->connection_id = response->connection_id;
  response_gen2->fabric_congestion_window = response->fabric_congestion_window;
  response_gen2->inter_packet_gap = response->inter_packet_gap;
  response_gen2->nic_congestion_window = response->nic_congestion_window;
  response_gen2->nic_inter_packet_gap = response->nic_inter_packet_gap;
  response_gen2->cc_metadata = response->cc_metadata;
  response_gen2->alpha_request = response->alpha_request;
  response_gen2->alpha_response = response->alpha_response;
  response_gen2->fabric_window_time_marker =
      response->fabric_window_time_marker;
  response_gen2->nic_window_time_marker = response->nic_window_time_marker;
  response_gen2->base_delay = response->base_delay;
  response_gen2->delay_state = response->delay_state;
  response_gen2->plb_state = response->plb_state;
  response_gen2->cc_opaque = response->cc_opaque;
  response_gen2->delay_select = response->delay_select;
  response_gen2->ar_rate = response->ar_rate;
  response_gen2->rtt_state = response->rtt_state;
  response_gen2->flow_label_1 = response->flow_label_1;
  response_gen2->flow_label_2 = response->flow_label_2;
  response_gen2->flow_label_3 = response->flow_label_3;
  response_gen2->flow_label_4 = response->flow_label_4;
  response_gen2->flow_label_1_valid = response->flow_label_1_valid;
  response_gen2->flow_label_2_valid = response->flow_label_2_valid;
  response_gen2->flow_label_3_valid = response->flow_label_3_valid;
  response_gen2->flow_label_4_valid = response->flow_label_4_valid;
  response_gen2->flow_id = response->flow_id;
  response_gen2->csig_enable = response->csig_enable;
  response_gen2->csig_select = response->csig_select;
  response_gen2->retransmit_timeout = response->retransmit_timeout;
  response_gen2->event_queue_select = response->event_queue_select;

  uint8_t num_flows = cc_metadata.gen2_flow_labels.size();
  if (num_flows == 1) {
    return;
  }
  // Recover Gen_2-specific fields (flow weights, wrr_restart_round) from Gen_3.
  // This is needed to support WeightedRoundRobinFlowScheduling policy in Gen3
  // (for evaluation purpose as a baseline).
  //
  // For Gen_2, Swift keeps wrr_restart_round always false.
  //
  response_gen2->wrr_restart_round = false;
  double fcwnd_per_flow[4] = {
      falcon_rue::FixedToFloat<uint32_t, double>(
          cc_metadata.gen3_flow_fcwnds[0], falcon_rue::kFractionalBits),
      falcon_rue::FixedToFloat<uint32_t, double>(
          cc_metadata.gen3_flow_fcwnds[1], falcon_rue::kFractionalBits),
      falcon_rue::FixedToFloat<uint32_t, double>(
          cc_metadata.gen3_flow_fcwnds[2], falcon_rue::kFractionalBits),
      falcon_rue::FixedToFloat<uint32_t, double>(
          cc_metadata.gen3_flow_fcwnds[3], falcon_rue::kFractionalBits)};
  fcwnd_per_flow[response->flow_id] =
      falcon_rue::FixedToFloat<uint32_t, double>(
          response->fabric_congestion_window, falcon_rue::kFractionalBits);
  double fcwnd_per_conn = fcwnd_per_flow[0] + fcwnd_per_flow[1] +
                          fcwnd_per_flow[2] + fcwnd_per_flow[3];

  response_gen2->flow_label_1_weight =
      isekai::rue::Swift<falcon_rue::EVENT_Gen3, falcon_rue::Response_Gen3>::
          ComputeFlowWeight(fcwnd_per_flow[0], fcwnd_per_conn);
  response_gen2->flow_label_2_weight =
      isekai::rue::Swift<falcon_rue::EVENT_Gen3, falcon_rue::Response_Gen3>::
          ComputeFlowWeight(fcwnd_per_flow[1], fcwnd_per_conn);
  response_gen2->flow_label_3_weight =
      isekai::rue::Swift<falcon_rue::EVENT_Gen3, falcon_rue::Response_Gen3>::
          ComputeFlowWeight(fcwnd_per_flow[2], fcwnd_per_conn);
  response_gen2->flow_label_4_weight =
      isekai::rue::Swift<falcon_rue::EVENT_Gen3, falcon_rue::Response_Gen3>::
          ComputeFlowWeight(fcwnd_per_flow[3], fcwnd_per_conn);
}

// Convert Gen_2 Event to Gen_3 Event. Newly added field in Gen_3 (pto,
// rack_rto) are not set by this method.
static void ConvertGen2EventToGen3Event(
    falcon_rue::EVENT_Gen3& event, const falcon_rue::Event_Gen2& event_gen2) {
  event.connection_id = event_gen2.connection_id;
  event.event_type = event_gen2.event_type;
  event.timestamp_1 = event_gen2.timestamp_1;
  event.timestamp_2 = event_gen2.timestamp_2;
  event.timestamp_3 = event_gen2.timestamp_3;
  event.timestamp_4 = event_gen2.timestamp_4;
  event.rx_buffer_level = event_gen2.rx_buffer_level;
  event.eack = event_gen2.eack;
  event.nack_code = event_gen2.nack_code;
  event.forward_hops = event_gen2.forward_hops;
  event.retransmit_count = event_gen2.retransmit_count;
  event.retransmit_reason = event_gen2.retransmit_reason;
  event.eack_drop = event_gen2.eack_drop;
  event.eack_own = event_gen2.eack_own;
  event.cc_metadata = event_gen2.cc_metadata;
  event.fabric_congestion_window = event_gen2.fabric_congestion_window;
  event.nic_congestion_window = event_gen2.nic_congestion_window;
  event.is_ipv4 = event_gen2.is_ipv4;
  event.multipath_enable = event_gen2.multipath_enable;
  event.flow_label = event_gen2.flow_label;
  event.cc_opaque = event_gen2.cc_opaque;
  event.fabric_window_time_marker = event_gen2.fabric_window_time_marker;
  event.nic_window_time_marker = event_gen2.nic_window_time_marker;
  event.base_delay = event_gen2.base_delay;
  event.delay_state = event_gen2.delay_state;
  event.plb_state = event_gen2.plb_state;
  event.delay_select = event_gen2.delay_select;
  event.event_queue_select = event_gen2.event_queue_select;
  event.rtt_state = event_gen2.rtt_state;
  event.ecn_accumulated = event_gen2.ecn_accumulated;
  event.num_packets_acked = event_gen2.num_packets_acked;
  event.csig_enable = event_gen2.csig_enable;
  event.csig = event_gen2.csig;
  event.gen_bit = event_gen2.gen_bit;
}

// Changes the connection state as a result of a received RUE response.
template <typename EventT, typename ResponseT>
void EventResponseFormatAdapter<EventT, ResponseT>::
    UpdateConnectionStateFromResponse(ConnectionState* connection_state,
                                      const ResponseT* response) const {
  CongestionControlMetadata& congestion_control_metadata =
      *connection_state->congestion_control_metadata;
  // Updates the congestion control metadata.
  if (response->randomize_path) {
    congestion_control_metadata.flow_label =
        falcon_->get_rate_update_engine()->GenerateRandomFlowLabel();
  }
  congestion_control_metadata.fabric_congestion_window =
      response->fabric_congestion_window;
  congestion_control_metadata.inter_packet_gap =
      falcon_->get_rate_update_engine()->FromTimingWheelTimeUnits(
          response->inter_packet_gap);
  congestion_control_metadata.nic_congestion_window =
      response->nic_congestion_window;
  congestion_control_metadata.retransmit_timeout =
      falcon_->get_rate_update_engine()->FromFalconTimeUnits(
          response->retransmit_timeout);
  congestion_control_metadata.cc_metadata = response->cc_metadata;
  congestion_control_metadata.fabric_window_time_marker =
      response->fabric_window_time_marker;
  congestion_control_metadata.nic_window_time_marker =
      response->nic_window_time_marker;
  congestion_control_metadata.nic_window_direction =
      response->nic_window_direction;
  congestion_control_metadata.delay_select = response->delay_select;
  congestion_control_metadata.delay_state = response->delay_state;
  congestion_control_metadata.rtt_state = response->rtt_state;
  congestion_control_metadata.cc_opaque = congestion_control_metadata.cc_opaque;
}

template <>
void EventResponseFormatAdapter<falcon_rue::Event_Gen2,
                                falcon_rue::Response_Gen2>::
    UpdateConnectionStateFromResponse(
        ConnectionState* connection_state,
        const falcon_rue::Response_Gen2* response) const {
  Gen2CongestionControlMetadata& congestion_control_metadata =
      CongestionControlMetadata::DowncastTo<Gen2CongestionControlMetadata>(
          *connection_state->congestion_control_metadata);
  // Updates the congestion control metadata.
  uint8_t num_flows = congestion_control_metadata.gen2_flow_labels.size();
  if (num_flows == 1) {
    // Only update flow label for flow ID 0 for single path connections. Flow
    // weights and other flow labels are not relevant for single path
    // connections.
    if (response->flow_label_1_valid) {
      congestion_control_metadata.gen2_flow_labels[0] = response->flow_label_1;
    }
    // Update CC metrics from response.
    StatisticCollectionInterface* stats_collector =
        falcon_->get_stats_collector();
    bool collect_cc_metrics =
        falcon_->get_stats_manager()->GetStatsConfig().enable_rue_cc_metrics();
    if (stats_collector && collect_cc_metrics) {
      if (response->flow_label_1_valid) {
        CHECK_OK(stats_collector->UpdateStatistic(
            absl::Substitute(kStatVectorRerouteCountFlow,
                             response->connection_id, 0),
            1, StatisticsCollectionConfig::TIME_SERIES_STAT));
      }
    }
  } else {
    // Update the flow labels if any of them is valid.
    if (response->flow_label_1_valid) {
      congestion_control_metadata.gen2_flow_labels[0] = response->flow_label_1;
    }
    if (response->flow_label_2_valid) {
      congestion_control_metadata.gen2_flow_labels[1] = response->flow_label_2;
    }
    if (response->flow_label_3_valid) {
      congestion_control_metadata.gen2_flow_labels[2] = response->flow_label_3;
    }
    if (response->flow_label_4_valid) {
      congestion_control_metadata.gen2_flow_labels[3] = response->flow_label_4;
    }
    // Update CC metrics from response.
    StatisticCollectionInterface* stats_collector =
        falcon_->get_stats_collector();
    bool collect_cc_metrics =
        falcon_->get_stats_manager()->GetStatsConfig().enable_rue_cc_metrics();
    if (stats_collector && collect_cc_metrics) {
      if (response->flow_label_1_valid) {
        CHECK_OK(stats_collector->UpdateStatistic(
            absl::Substitute(kStatVectorRerouteCountFlow,
                             response->connection_id, 0),
            1, StatisticsCollectionConfig::TIME_SERIES_STAT));
      }
      if (response->flow_label_2_valid) {
        CHECK_OK(stats_collector->UpdateStatistic(
            absl::Substitute(kStatVectorRerouteCountFlow,
                             response->connection_id, 1),
            1, StatisticsCollectionConfig::TIME_SERIES_STAT));
      }
      if (response->flow_label_3_valid) {
        CHECK_OK(stats_collector->UpdateStatistic(
            absl::Substitute(kStatVectorRerouteCountFlow,
                             response->connection_id, 2),
            1, StatisticsCollectionConfig::TIME_SERIES_STAT));
      }
      if (response->flow_label_4_valid) {
        CHECK_OK(stats_collector->UpdateStatistic(
            absl::Substitute(kStatVectorRerouteCountFlow,
                             response->connection_id, 3),
            1, StatisticsCollectionConfig::TIME_SERIES_STAT));
      }
    }
    // The datapath expects some weights to be nonzero for multipath
    // connections. If all weights are zero, exit with an error.
    CHECK_GT(response->flow_label_1_weight + response->flow_label_2_weight +
                 response->flow_label_3_weight + response->flow_label_4_weight,
             0);
    congestion_control_metadata.gen2_flow_weights[0] =
        response->flow_label_1_weight;
    congestion_control_metadata.gen2_flow_weights[1] =
        response->flow_label_2_weight;
    congestion_control_metadata.gen2_flow_weights[2] =
        response->flow_label_3_weight;
    congestion_control_metadata.gen2_flow_weights[3] =
        response->flow_label_4_weight;
    if (response->wrr_restart_round) {
      dynamic_cast<Gen2ReliabilityManager*>(
          falcon_->get_packet_reliability_manager())
          ->ResetWrrForConnection(response->connection_id);
    }
  }

  congestion_control_metadata.fabric_congestion_window =
      response->fabric_congestion_window;
  congestion_control_metadata.inter_packet_gap =
      falcon_->get_rate_update_engine()->FromTimingWheelTimeUnits(
          response->inter_packet_gap);  // fipg
  //
  // response.
  // Until ncwnd pacing is supported, the minimum value allowed for an integer
  // ncwnd is 1.
  congestion_control_metadata.nic_congestion_window = std::max<uint32_t>(
      1, falcon_rue::FixedToUint<uint32_t, uint32_t>(
             response->nic_congestion_window, falcon_rue::kFractionalBits));

  // Skip updating RTO if using bypass algorithm, to preserve the RTO value
  // specified in the config (for bypass, response contains hardcoded RTO).
  if (falcon_->get_config()->rue().algorithm() != "bypass") {
    congestion_control_metadata.retransmit_timeout =
        falcon_->get_rate_update_engine()->FromFalconTimeUnits(
            response->retransmit_timeout);
  }
  congestion_control_metadata.cc_metadata = response->cc_metadata;
  congestion_control_metadata.fabric_window_time_marker =
      response->fabric_window_time_marker;
  congestion_control_metadata.nic_window_time_marker =
      response->nic_window_time_marker;
  congestion_control_metadata.delay_select = response->delay_select;
  congestion_control_metadata.delay_state = response->delay_state;
  congestion_control_metadata.rtt_state = response->rtt_state;
  congestion_control_metadata.cc_opaque = congestion_control_metadata.cc_opaque;
  congestion_control_metadata.gen2_plb_state = response->plb_state;

  // Update per-per connection XoFF metadata.
  auto& connection_rdma_xoff_metadata =
      dynamic_cast<Gen2ConnectionState*>(connection_state)
          ->connection_xoff_metadata;
  connection_rdma_xoff_metadata.alpha_request = response->alpha_request;
  connection_rdma_xoff_metadata.alpha_response = response->alpha_response;

  //
  //
  // response->csig_select).
}

template <>
void EventResponseFormatAdapter<falcon_rue::EVENT_Gen3,
                                falcon_rue::Response_Gen3>::
    UpdateConnectionStateFromResponse(
        ConnectionState* connection_state,
        const falcon_rue::Response_Gen3* response) const {
  Gen3CongestionControlMetadata& congestion_control_metadata =
      CongestionControlMetadata::DowncastTo<Gen3CongestionControlMetadata>(
          *connection_state->congestion_control_metadata);
  falcon_rue::Response_Gen2 response_gen2;
  ConvertGen3ResponseToGen2Response(&response_gen2, response,
                                    congestion_control_metadata);
  auto gen2_adapter = std::make_unique<EventResponseFormatAdapter<
      falcon_rue::Event_Gen2, falcon_rue::Response_Gen2>>(falcon_);
  gen2_adapter->UpdateConnectionStateFromResponse(connection_state,
                                                  &response_gen2);

  // Updates Gen3 specific data
  // Updates the congestion control metadata.
  uint8_t num_flows = congestion_control_metadata.gen2_flow_labels.size();
  // In Gen3, flow fcwnd is per-flow, and it should be larger than zero.
  // If fabric window is zero, exit with an error.
  CHECK_GT(response->fabric_congestion_window, 0);
  if (num_flows == 1) {
    congestion_control_metadata.gen3_flow_fcwnds[0] =
        response->fabric_congestion_window;
    // In Gen3, per-connection fabric window is the sum of all flow fcwnds.
    congestion_control_metadata.fabric_congestion_window =
        congestion_control_metadata.gen3_flow_fcwnds[0];
    // Update per-flow RTT state.
    congestion_control_metadata.gen3_flow_rtt_state[0] = response->rtt_state;
  } else {
    congestion_control_metadata.gen3_flow_fcwnds[response->flow_id] =
        response->fabric_congestion_window;
    double fcwnd_per_flow_0 = falcon_rue::FixedToFloat<uint32_t, double>(
        congestion_control_metadata.gen3_flow_fcwnds[0],
        falcon_rue::kFractionalBits);
    double fcwnd_per_flow_1 = falcon_rue::FixedToFloat<uint32_t, double>(
        congestion_control_metadata.gen3_flow_fcwnds[1],
        falcon_rue::kFractionalBits);
    double fcwnd_per_flow_2 = falcon_rue::FixedToFloat<uint32_t, double>(
        congestion_control_metadata.gen3_flow_fcwnds[2],
        falcon_rue::kFractionalBits);
    double fcwnd_per_flow_3 = falcon_rue::FixedToFloat<uint32_t, double>(
        congestion_control_metadata.gen3_flow_fcwnds[3],
        falcon_rue::kFractionalBits);
    // In Gen3, per-connection fabric window is the sum of all flow fcwnds.
    congestion_control_metadata.fabric_congestion_window =
        falcon_rue::FloatToFixed<double, uint32_t>(
            fcwnd_per_flow_0 + fcwnd_per_flow_1 + fcwnd_per_flow_2 +
                fcwnd_per_flow_3,
            falcon_rue::kFractionalBits);
    // Update per-flow RTT state.
    congestion_control_metadata.gen3_flow_rtt_state[response->flow_id] =
        response->rtt_state;
  }
}

template <typename EventT, typename ResponseT>
bool EventResponseFormatAdapter<EventT, ResponseT>::IsRandomizePath(
    const ResponseT* response) const {
  return response->randomize_path;
}

template <>
bool EventResponseFormatAdapter<falcon_rue::Event_Gen2,
                                falcon_rue::Response_Gen2>::
    IsRandomizePath(const falcon_rue::Response_Gen2* response) const {
  // For Gen2, if any of the valid bits are set then the response would be
  // signaling a randomize path signal to the datapath. The RUE class which is
  // keeping stats of path changes would then record this change by incrementing
  // a counter.
  return response->flow_label_1_valid || response->flow_label_2_valid ||
         response->flow_label_3_valid || response->flow_label_4_valid;
}

template <>
bool EventResponseFormatAdapter<falcon_rue::EVENT_Gen3,
                                falcon_rue::Response_Gen3>::
    IsRandomizePath(const falcon_rue::Response_Gen3* response) const {
  // For Gen3, if any of the valid bits are set then the response would be
  // signaling a randomize path signal to the datapath. The RUE class which is
  // keeping stats of path changes would then record this change by incrementing
  // a counter.
  return response->flow_label_1_valid || response->flow_label_2_valid ||
         response->flow_label_3_valid || response->flow_label_4_valid;
}

template <typename EventT, typename ResponseT>
void EventResponseFormatAdapter<EventT, ResponseT>::
    FillTimeoutRetransmittedEvent(EventT& event, const RueKey* rue_key,
                                  const Packet* packet,
                                  const CongestionControlMetadata& ccmeta,
                                  uint8_t retransmit_count) const {
  event.connection_id = rue_key->scid;
  event.event_type = falcon::RueEventType::kRetransmit;
  event.timestamp_1 = 0;  // not used
  event.timestamp_2 = 0;  // not used
  event.timestamp_3 = 0;  // not used
  event.timestamp_4 = 0;  // not used
  event.retransmit_count = retransmit_count;
  event.retransmit_reason = falcon::RetransmitReason::kTimeout;
  event.nack_code = falcon::NackCode::kNotANack;  // not used
  event.forward_hops = 0;                         // not used
  event.rx_buffer_level = 0;                      // not used
  event.cc_metadata = 0;                          // not used
  event.fabric_congestion_window = ccmeta.fabric_congestion_window;
  event.inter_packet_gap =
      falcon_->get_rate_update_engine()->ToTimingWheelTimeUnits(
          ccmeta.inter_packet_gap);
  event.nic_congestion_window = ccmeta.nic_congestion_window;
  event.retransmit_timeout =
      falcon_->get_rate_update_engine()->ToFalconTimeUnits(
          ccmeta.retransmit_timeout);
  event.num_packets_acked = 0;   // not used
  event.event_queue_select = 0;  // not used
  event.delay_select = ccmeta.delay_select;
  event.fabric_window_time_marker = ccmeta.fabric_window_time_marker;
  event.nic_window_time_marker = ccmeta.nic_window_time_marker;
  event.nic_window_direction = ccmeta.nic_window_direction;
  event.base_delay = ccmeta.base_delay;
  event.delay_state = ccmeta.delay_state;
  event.rtt_state = ccmeta.rtt_state;
  event.cc_opaque = ccmeta.cc_opaque;
  event.eack = false;       // not used
  event.eack_drop = false;  // not used
  event.reserved_0 = 0;     // not used
  event.reserved_1 = 0;     // not used
  event.reserved_2 = 0;     // not used
  event.gen_bit = 0;        // not used
}

template <>
void EventResponseFormatAdapter<falcon_rue::Event_Gen2,
                                falcon_rue::Response_Gen2>::
    FillTimeoutRetransmittedEvent(falcon_rue::Event_Gen2& event,
                                  const RueKey* rue_key, const Packet* packet,
                                  const CongestionControlMetadata& gen1_ccmeta,
                                  uint8_t retransmit_count) const {
  const Gen2CongestionControlMetadata& ccmeta =
      Gen2CongestionControlMetadata::DowncastTo<Gen2CongestionControlMetadata>(
          gen1_ccmeta);
  auto gen2_rue_key = dynamic_cast<const Gen2RueKey*>(rue_key);
  uint32_t num_flows = ccmeta.gen2_flow_labels.size();

  event.multipath_enable = (num_flows == 1) ? false : true;
  event.connection_id = gen2_rue_key->scid;
  // The flow label in the event uses the flow label from the retx packet, not
  // the latest flow label for the flow.
  event.flow_label = packet->metadata.flow_label;
  event.event_type = falcon::RueEventType::kRetransmit;
  event.timestamp_1 = 0;  // not used
  event.timestamp_2 = 0;  // not used
  event.timestamp_3 = 0;  // not used
  event.timestamp_4 = 0;  // not used
  event.retransmit_count = retransmit_count;
  event.retransmit_reason = falcon::RetransmitReason::kTimeout;
  event.nack_code = falcon::NackCode::kNotANack;  // not used
  event.forward_hops = 0;                         // not used
  event.rx_buffer_level = 0;                      // not used
  event.cc_metadata = 0;                          // not used
  event.fabric_congestion_window = ccmeta.fabric_congestion_window;
  //
  event.nic_congestion_window = falcon_rue::UintToFixed<uint32_t, uint32_t>(
      ccmeta.nic_congestion_window, falcon_rue::kFractionalBits);
  event.num_packets_acked = 0;   // not used
  event.event_queue_select = 0;  // not used
  event.delay_select = ccmeta.delay_select;
  event.fabric_window_time_marker = ccmeta.fabric_window_time_marker;
  event.nic_window_time_marker = ccmeta.nic_window_time_marker;
  event.base_delay = ccmeta.base_delay;
  event.delay_state = ccmeta.delay_state;
  event.rtt_state = ccmeta.rtt_state;
  event.cc_opaque = ccmeta.cc_opaque;
  event.eack = false;       // not used
  event.eack_drop = false;  // not used
  event.eack_own = 0;       // not used
  event.reserved_0 = 0;     // not used
  event.reserved_1 = 0;     // not used
  event.gen_bit = 0;        // not used
  event.plb_state = ccmeta.gen2_plb_state;
  //
}

template <>
void EventResponseFormatAdapter<falcon_rue::EVENT_Gen3,
                                falcon_rue::Response_Gen3>::
    FillTimeoutRetransmittedEvent(falcon_rue::EVENT_Gen3& event,
                                  const RueKey* rue_key, const Packet* packet,
                                  const CongestionControlMetadata& gen1_ccmeta,
                                  uint8_t retransmit_count) const {
  Gen3CongestionControlMetadata ccmeta =
      Gen3CongestionControlMetadata::DowncastTo<Gen3CongestionControlMetadata>(
          gen1_ccmeta);
  auto gen2_rue_key = dynamic_cast<const Gen2RueKey*>(rue_key);
  falcon_rue::Event_Gen2 gen2_event;
  auto gen2_adapter = std::make_unique<EventResponseFormatAdapter<
      falcon_rue::Event_Gen2, falcon_rue::Response_Gen2>>(falcon_);

  gen2_adapter->FillTimeoutRetransmittedEvent(gen2_event, rue_key, packet,
                                              ccmeta, retransmit_count);
  ConvertGen2EventToGen3Event(event, gen2_event);
  // In Gen3, fcwnd is per-flow.
  event.fabric_congestion_window =
      ccmeta.gen3_flow_fcwnds[gen2_rue_key->flow_id];
}

template <typename EventT, typename ResponseT>
void EventResponseFormatAdapter<EventT, ResponseT>::FillNackEvent(
    EventT& event, const RueKey* rue_key, const Packet* packet,
    const CongestionControlMetadata& ccmeta, uint32_t num_packets_acked) const {
  event.connection_id = rue_key->scid;
  event.event_type = falcon::RueEventType::kNack;
  event.timestamp_1 = falcon_->get_rate_update_engine()->ToFalconTimeUnits(
      packet->nack.timestamp_1);
  event.timestamp_2 = falcon_->get_rate_update_engine()->ToFalconTimeUnits(
      packet->nack.timestamp_2);
  event.timestamp_3 = falcon_->get_rate_update_engine()->ToFalconTimeUnits(
      packet->timestamps.sent_timestamp);
  event.timestamp_4 = falcon_->get_rate_update_engine()->ToFalconTimeUnits(
      packet->timestamps.received_timestamp);
  event.retransmit_count = 0;                                  // not used
  event.retransmit_reason = falcon::RetransmitReason::kEarly;  // not used
  event.nack_code = packet->nack.code;
  event.forward_hops = packet->nack.forward_hops;
  event.rx_buffer_level = packet->nack.rx_buffer_level;
  event.cc_metadata = packet->nack.cc_metadata;
  event.fabric_congestion_window = ccmeta.fabric_congestion_window;
  event.inter_packet_gap =
      falcon_->get_rate_update_engine()->ToTimingWheelTimeUnits(
          ccmeta.inter_packet_gap);
  event.nic_congestion_window = ccmeta.nic_congestion_window;
  event.retransmit_timeout =
      falcon_->get_rate_update_engine()->ToFalconTimeUnits(
          ccmeta.retransmit_timeout);
  event.num_packets_acked = static_cast<uint16_t>(num_packets_acked);
  event.event_queue_select = 0;  // not used
  event.delay_select = ccmeta.delay_select;
  event.fabric_window_time_marker = ccmeta.fabric_window_time_marker;
  event.nic_window_time_marker = ccmeta.nic_window_time_marker;
  event.nic_window_direction = ccmeta.nic_window_direction;
  event.base_delay = ccmeta.base_delay;
  event.delay_state = ccmeta.delay_state;
  event.rtt_state = ccmeta.rtt_state;
  event.cc_opaque = ccmeta.cc_opaque;
  event.eack = false;       // not used
  event.eack_drop = false;  // not used
  event.reserved_0 = 0;     // not used
  event.reserved_1 = 0;     // not used
  event.reserved_2 = 0;     // not used
  event.gen_bit = 0;        // not used
}

template <>
void EventResponseFormatAdapter<falcon_rue::Event_Gen2,
                                falcon_rue::Response_Gen2>::
    FillNackEvent(falcon_rue::Event_Gen2& event, const RueKey* rue_key,
                  const Packet* packet,
                  const CongestionControlMetadata& gen1_ccmeta,
                  uint32_t num_packets_acked) const {
  Gen2CongestionControlMetadata ccmeta =
      Gen2CongestionControlMetadata::DowncastTo<Gen2CongestionControlMetadata>(
          gen1_ccmeta);
  auto gen2_rue_key = dynamic_cast<const Gen2RueKey*>(rue_key);
  uint32_t num_flows = ccmeta.gen2_flow_labels.size();

  event.multipath_enable = (num_flows == 1) ? false : true;
  event.connection_id = gen2_rue_key->scid;
  // The flow label in the event uses the flow label from the
  // NACK, not the latest flow label for the flow.
  event.flow_label = packet->metadata.flow_label;
  event.event_type = falcon::RueEventType::kNack;
  event.timestamp_1 = falcon_->get_rate_update_engine()->ToFalconTimeUnits(
      packet->nack.timestamp_1);
  event.timestamp_2 = falcon_->get_rate_update_engine()->ToFalconTimeUnits(
      packet->nack.timestamp_2);
  event.timestamp_3 = falcon_->get_rate_update_engine()->ToFalconTimeUnits(
      packet->timestamps.sent_timestamp);
  event.timestamp_4 = falcon_->get_rate_update_engine()->ToFalconTimeUnits(
      packet->timestamps.received_timestamp);
  event.retransmit_count = 0;                                  // not used
  event.retransmit_reason = falcon::RetransmitReason::kEarly;  // not used
  event.nack_code = packet->nack.code;
  event.forward_hops = packet->nack.forward_hops;
  event.rx_buffer_level = packet->nack.rx_buffer_level;
  event.cc_metadata = packet->nack.cc_metadata;
  event.fabric_congestion_window = ccmeta.fabric_congestion_window;
  //
  event.nic_congestion_window = falcon_rue::UintToFixed<uint32_t, uint32_t>(
      ccmeta.nic_congestion_window, falcon_rue::kFractionalBits);
  event.num_packets_acked = static_cast<uint16_t>(num_packets_acked);
  event.event_queue_select = 0;  // not used
  event.delay_select = ccmeta.delay_select;
  event.fabric_window_time_marker = ccmeta.fabric_window_time_marker;
  event.nic_window_time_marker = ccmeta.nic_window_time_marker;
  event.base_delay = ccmeta.base_delay;
  event.delay_state = ccmeta.delay_state;
  event.rtt_state = ccmeta.rtt_state;
  event.cc_opaque = ccmeta.cc_opaque;
  event.eack = false;       // not used
  event.eack_drop = false;  // not used
  event.eack_own = 0;       // not used
  event.reserved_0 = 0;     // not used
  event.reserved_1 = 0;     // not used
  event.gen_bit = 0;        // not used
  event.plb_state = ccmeta.gen2_plb_state;
  //
}

template <>
void EventResponseFormatAdapter<falcon_rue::EVENT_Gen3,
                                falcon_rue::Response_Gen3>::
    FillNackEvent(falcon_rue::EVENT_Gen3& event, const RueKey* rue_key,
                  const Packet* packet,
                  const CongestionControlMetadata& gen1_ccmeta,
                  uint32_t num_packets_acked) const {
  Gen3CongestionControlMetadata ccmeta =
      Gen3CongestionControlMetadata::DowncastTo<Gen3CongestionControlMetadata>(
          gen1_ccmeta);
  auto gen2_rue_key = dynamic_cast<const Gen2RueKey*>(rue_key);
  falcon_rue::Event_Gen2 gen2_event;
  auto gen2_adapter = std::make_unique<EventResponseFormatAdapter<
      falcon_rue::Event_Gen2, falcon_rue::Response_Gen2>>(falcon_);

  gen2_adapter->FillNackEvent(gen2_event, rue_key, packet, ccmeta,
                              num_packets_acked);
  ConvertGen2EventToGen3Event(event, gen2_event);
  // In Gen3, fcwnd is per-flow.
  event.fabric_congestion_window =
      ccmeta.gen3_flow_fcwnds[gen2_rue_key->flow_id];
}

template <typename EventT, typename ResponseT>
void EventResponseFormatAdapter<EventT, ResponseT>::FillExplicitAckEvent(
    EventT& event, const RueKey* rue_key, const Packet* packet,
    const CongestionControlMetadata& ccmeta, uint32_t num_packets_acked,
    bool eack, bool eack_drop) const {
  event.connection_id = rue_key->scid;
  event.event_type = falcon::RueEventType::kAck;
  event.timestamp_1 = falcon_->get_rate_update_engine()->ToFalconTimeUnits(
      packet->ack.timestamp_1);
  event.timestamp_2 = falcon_->get_rate_update_engine()->ToFalconTimeUnits(
      packet->ack.timestamp_2);
  event.timestamp_3 = falcon_->get_rate_update_engine()->ToFalconTimeUnits(
      packet->timestamps.sent_timestamp);
  event.timestamp_4 = falcon_->get_rate_update_engine()->ToFalconTimeUnits(
      packet->timestamps.received_timestamp);
  event.retransmit_count = 0;                                  // not used
  event.retransmit_reason = falcon::RetransmitReason::kEarly;  // not used
  event.nack_code = falcon::NackCode::kNotANack;               // not used
  event.forward_hops = packet->ack.forward_hops;
  event.rx_buffer_level = packet->ack.rx_buffer_level;
  event.cc_metadata = packet->ack.cc_metadata;
  event.fabric_congestion_window = ccmeta.fabric_congestion_window;
  event.inter_packet_gap =
      falcon_->get_rate_update_engine()->ToTimingWheelTimeUnits(
          ccmeta.inter_packet_gap);
  event.nic_congestion_window = ccmeta.nic_congestion_window;
  event.retransmit_timeout =
      falcon_->get_rate_update_engine()->ToFalconTimeUnits(
          ccmeta.retransmit_timeout);
  event.num_packets_acked = static_cast<uint16_t>(num_packets_acked);
  event.event_queue_select = 0;  // not used
  event.delay_select = ccmeta.delay_select;
  event.fabric_window_time_marker = ccmeta.fabric_window_time_marker;
  event.nic_window_time_marker = ccmeta.nic_window_time_marker;
  event.nic_window_direction = ccmeta.nic_window_direction;
  event.base_delay = ccmeta.base_delay;
  event.delay_state = ccmeta.delay_state;
  event.rtt_state = ccmeta.rtt_state;
  event.cc_opaque = ccmeta.cc_opaque;
  event.reserved_0 = 0;  // not used
  event.reserved_1 = 0;  // not used
  event.reserved_2 = 0;  // not used
  event.gen_bit = 0;     // not used
  event.eack = eack;
  event.eack_drop = eack_drop;
}

template <>
void EventResponseFormatAdapter<falcon_rue::Event_Gen2,
                                falcon_rue::Response_Gen2>::
    FillExplicitAckEvent(falcon_rue::Event_Gen2& event, const RueKey* rue_key,
                         const Packet* packet,
                         const CongestionControlMetadata& gen1_ccmeta,
                         uint32_t num_packets_acked, bool eack,
                         bool eack_drop) const {
  Gen2CongestionControlMetadata ccmeta =
      Gen2CongestionControlMetadata::DowncastTo<Gen2CongestionControlMetadata>(
          gen1_ccmeta);
  auto gen2_rue_key = dynamic_cast<const Gen2RueKey*>(rue_key);
  uint32_t num_flows = ccmeta.gen2_flow_labels.size();

  event.multipath_enable = (num_flows == 1) ? false : true;
  event.connection_id = gen2_rue_key->scid;
  // The flow label in the event uses the flow label from the ACK, not the
  // latest flow label for the flow.
  event.flow_label = packet->metadata.flow_label;
  event.event_type = falcon::RueEventType::kAck;
  event.timestamp_1 = falcon_->get_rate_update_engine()->ToFalconTimeUnits(
      packet->ack.timestamp_1);
  event.timestamp_2 = falcon_->get_rate_update_engine()->ToFalconTimeUnits(
      packet->ack.timestamp_2);
  event.timestamp_3 = falcon_->get_rate_update_engine()->ToFalconTimeUnits(
      packet->timestamps.sent_timestamp);
  event.timestamp_4 = falcon_->get_rate_update_engine()->ToFalconTimeUnits(
      packet->timestamps.received_timestamp);
  event.retransmit_count = 0;                                  // not used
  event.retransmit_reason = falcon::RetransmitReason::kEarly;  // not used
  event.nack_code = falcon::NackCode::kNotANack;               // not used
  event.forward_hops = packet->ack.forward_hops;
  event.rx_buffer_level = packet->ack.rx_buffer_level;
  event.cc_metadata = packet->ack.cc_metadata;
  event.fabric_congestion_window = ccmeta.fabric_congestion_window;
  //
  event.nic_congestion_window = falcon_rue::UintToFixed<uint32_t, uint32_t>(
      ccmeta.nic_congestion_window, falcon_rue::kFractionalBits);
  event.num_packets_acked = static_cast<uint16_t>(num_packets_acked);
  event.event_queue_select = 0;  // not used
  event.delay_select = ccmeta.delay_select;
  event.fabric_window_time_marker = ccmeta.fabric_window_time_marker;
  event.nic_window_time_marker = ccmeta.nic_window_time_marker;
  event.base_delay = ccmeta.base_delay;
  event.delay_state = ccmeta.delay_state;
  event.rtt_state = ccmeta.rtt_state;
  event.cc_opaque = ccmeta.cc_opaque;
  event.reserved_0 = 0;  // not used
  event.reserved_1 = 0;  // not used
  event.gen_bit = 0;     // not used
  event.eack_own = 0;    // not used
  event.eack = eack;
  event.eack_drop = eack_drop;
  event.plb_state = ccmeta.gen2_plb_state;
  //
}

template <>
void EventResponseFormatAdapter<falcon_rue::EVENT_Gen3,
                                falcon_rue::Response_Gen3>::
    FillExplicitAckEvent(falcon_rue::EVENT_Gen3& event, const RueKey* rue_key,
                         const Packet* packet,
                         const CongestionControlMetadata& gen1_ccmeta,
                         uint32_t num_packets_acked, bool eack,
                         bool eack_drop) const {
  Gen3CongestionControlMetadata ccmeta =
      Gen3CongestionControlMetadata::DowncastTo<Gen3CongestionControlMetadata>(
          gen1_ccmeta);
  auto gen2_rue_key = dynamic_cast<const Gen2RueKey*>(rue_key);
  falcon_rue::Event_Gen2 gen2_event;
  auto gen2_adapter = std::make_unique<EventResponseFormatAdapter<
      falcon_rue::Event_Gen2, falcon_rue::Response_Gen2>>(falcon_);

  gen2_adapter->FillExplicitAckEvent(gen2_event, rue_key, packet, ccmeta,
                                     num_packets_acked, eack, eack_drop);
  ConvertGen2EventToGen3Event(event, gen2_event);
  // In Gen3, fcwnd is per-flow.
  event.fabric_congestion_window =
      ccmeta.gen3_flow_fcwnds[gen2_rue_key->flow_id];
}

// Explicit template instantiations.
template class EventResponseFormatAdapter<falcon_rue::Event,
                                          falcon_rue::Response>;
template class EventResponseFormatAdapter<falcon_rue::Event_Gen2,
                                          falcon_rue::Response_Gen2>;

}  // namespace isekai
