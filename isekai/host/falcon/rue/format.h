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

#ifndef ISEKAI_HOST_FALCON_RUE_FORMAT_H_
#define ISEKAI_HOST_FALCON_RUE_FORMAT_H_

#include <cstdint>

#include "isekai/host/falcon/falcon.h"
#include "isekai/host/falcon/rue/format_gen1.h"
#include "isekai/host/falcon/rue/format_gen2.h"
#include "isekai/host/falcon/rue/format_gen3.h"

namespace falcon_rue {

// Sets GEN1 Response.
inline void SetResponse(
    uint32_t connection_id, bool randomize_path, uint32_t cc_metadata,
    uint32_t fabric_congestion_window, uint32_t inter_packet_gap,
    uint32_t nic_congestion_window, uint32_t retransmit_timeout,
    uint32_t fabric_window_time_marker, uint32_t nic_window_time_marker,
    falcon::WindowDirection nic_window_direction, uint8_t event_queue_select,
    falcon::DelaySelect delay_select, uint32_t base_delay, uint32_t delay_state,
    uint32_t rtt_state, uint8_t cc_opaque, Response_Gen1& response) {
  response = (Response_Gen1){
      .connection_id = connection_id,
      .randomize_path = randomize_path,
      .eack_own = 0,  // 2 bits unused by FALCON.
      .cc_metadata = cc_metadata,
      .fabric_congestion_window = fabric_congestion_window,
      .inter_packet_gap = inter_packet_gap,
      .nic_congestion_window = nic_congestion_window,
      .retransmit_timeout = retransmit_timeout,
      .event_queue_select = event_queue_select,
      .delay_select = delay_select,
      .fabric_window_time_marker = fabric_window_time_marker,
      .nic_window_time_marker = nic_window_time_marker,
      .nic_window_direction = nic_window_direction,
      .base_delay = base_delay,  // Doesn't change.
      .delay_state = delay_state,
      .rtt_state = rtt_state,
      .cc_opaque = cc_opaque,
  };
}

// Sets Gen_2 Response.
inline void SetResponse(
    uint32_t connection_id, uint32_t cc_metadata,
    uint32_t fabric_congestion_window, uint32_t fabric_inter_packet_gap,
    uint32_t nic_congestion_window, uint32_t retransmit_timeout,
    uint32_t fabric_window_time_marker, uint32_t nic_window_time_marker,
    uint8_t event_queue_select, falcon::DelaySelect delay_select,
    uint32_t base_delay, uint32_t delay_state, uint32_t rtt_state,
    uint32_t cc_opaque, uint32_t plb_state, uint8_t alpha_request,
    uint8_t alpha_response, uint32_t nic_inter_packet_gap,
    uint32_t flow_label_1, uint32_t flow_label_2, uint32_t flow_label_3,
    uint32_t flow_label_4, bool flow_label_1_valid, bool flow_label_2_valid,
    bool flow_label_3_valid, bool flow_label_4_valid,
    uint8_t flow_label_1_weight, uint8_t flow_label_2_weight,
    uint8_t flow_label_3_weight, uint8_t flow_label_4_weight,
    bool wrr_restart_round, uint8_t flow_id, bool csig_enable,
    uint8_t csig_select, uint8_t ar_rate, Response_Gen2& response) {
  response = (Response_Gen2){
      .connection_id = connection_id,
      .fabric_congestion_window = fabric_congestion_window,
      .inter_packet_gap = fabric_inter_packet_gap,
      .nic_congestion_window = nic_congestion_window,
      .nic_inter_packet_gap = nic_inter_packet_gap,
      .cc_metadata = cc_metadata,
      .alpha_request = alpha_request,
      .alpha_response = alpha_response,
      .fabric_window_time_marker = fabric_window_time_marker,
      .nic_window_time_marker = nic_window_time_marker,
      .base_delay = base_delay,
      .delay_state = delay_state,
      .plb_state = plb_state,
      .cc_opaque = cc_opaque,
      .delay_select = delay_select,
      .ar_rate = ar_rate,
      .rtt_state = rtt_state,
      .flow_label_1 = flow_label_1,
      .flow_label_2 = flow_label_2,
      .flow_label_3 = flow_label_3,
      .flow_label_4 = flow_label_4,
      .flow_label_1_weight = flow_label_1_weight,
      .flow_label_2_weight = flow_label_2_weight,
      .flow_label_3_weight = flow_label_3_weight,
      .flow_label_4_weight = flow_label_4_weight,
      .flow_label_1_valid = flow_label_1_valid,
      .flow_label_2_valid = flow_label_2_valid,
      .flow_label_3_valid = flow_label_3_valid,
      .flow_label_4_valid = flow_label_4_valid,
      .wrr_restart_round = wrr_restart_round,
      .flow_id = flow_id,
      .csig_enable = csig_enable,
      .csig_select = csig_select,
      .retransmit_timeout = retransmit_timeout,
      .event_queue_select = event_queue_select,
  };
}

// Sets Gen_3 Response.
inline void SetResponse(
    uint32_t connection_id, uint32_t fabric_congestion_window,
    uint32_t fabric_inter_packet_gap, uint32_t nic_congestion_window,
    uint32_t nic_inter_packet_gap, uint32_t cc_metadata, uint8_t alpha_request,
    uint8_t alpha_response, uint32_t fabric_window_time_marker,
    uint32_t nic_window_time_marker, uint32_t base_delay, uint32_t delay_state,
    uint32_t plb_state, uint32_t cc_opaque, falcon::DelaySelect delay_select,
    uint8_t ar_rate, uint32_t rtt_state, uint32_t flow_label_1,
    uint32_t flow_label_2, uint32_t flow_label_3, uint32_t flow_label_4,
    uint8_t flow_weight_1, uint8_t flow_weight_2, uint8_t flow_weight_3,
    uint8_t flow_weight_4, bool flow_label_1_valid, bool flow_label_2_valid,
    bool flow_label_3_valid, bool flow_label_4_valid, bool wrr_restart,
    uint8_t flow_id, bool csig_enable, uint8_t csig_select,
    uint32_t retransmit_timeout, uint8_t event_queue_select,
    uint32_t tlp_probe_timeout, uint32_t rack_retransmit_timeout,
    uint8_t cwnd_carryover, Response_Gen3& response) {
  response = (Response_Gen3){
      .connection_id = connection_id,
      .fabric_congestion_window = fabric_congestion_window,
      .inter_packet_gap = fabric_inter_packet_gap,
      .nic_congestion_window = nic_congestion_window,
      .nic_inter_packet_gap = nic_inter_packet_gap,
      .cc_metadata = cc_metadata,
      .alpha_request = alpha_request,
      .alpha_response = alpha_response,
      .fabric_window_time_marker = fabric_window_time_marker,
      .nic_window_time_marker = nic_window_time_marker,
      .base_delay = base_delay,
      .delay_state = delay_state,
      .plb_state = plb_state,
      .cc_opaque = cc_opaque,
      .delay_select = delay_select,
      .ar_rate = ar_rate,
      .rtt_state = rtt_state,
      .flow_label_1 = flow_label_1,
      .flow_label_2 = flow_label_2,
      .flow_label_3 = flow_label_3,
      .flow_label_4 = flow_label_4,
      .flow_weight_1 = flow_weight_1,
      .flow_weight_2 = flow_weight_2,
      .flow_weight_3 = flow_weight_3,
      .flow_weight_4 = flow_weight_4,
      .flow_label_1_valid = flow_label_1_valid,
      .flow_label_2_valid = flow_label_2_valid,
      .flow_label_3_valid = flow_label_3_valid,
      .flow_label_4_valid = flow_label_4_valid,
      .wrr_restart = wrr_restart,
      .flow_id = flow_id,
      .csig_enable = csig_enable,
      .csig_select = csig_select,
      .retransmit_timeout = retransmit_timeout,
      .event_queue_select = event_queue_select,
      .tlp_probe_timeout = tlp_probe_timeout,
      .rack_retransmit_timeout = rack_retransmit_timeout,
      .cwnd_carryover = cwnd_carryover,
  };
}

//
#ifdef GEN_3
using Event = EVENT_Gen3;
using Response = Response_Gen3;
#endif

#ifdef GEN_2
using Event = Event_Gen2;
using Response = Response_Gen2;
#endif

#if !defined(GEN_2) && !defined(GEN_3)
using Event = Event_Gen1;
using Response = Response_Gen1;
#endif
}  // namespace falcon_rue

#endif  // ISEKAI_HOST_FALCON_RUE_FORMAT_H_
