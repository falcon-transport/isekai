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

#include "isekai/host/falcon/gen2/ulp_backpressure_policy_alpha_carving.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/falcon_connection_state.h"
#include "isekai/host/falcon/falcon_counters.h"
#include "isekai/host/falcon/falcon_resource_credits.h"
#include "isekai/host/falcon/gen2/connection_state.h"
#include "isekai/host/falcon/rue/algorithm/swift.h"

namespace isekai {

AlphaCarvingUlpBackpressurePolicy::AlphaCarvingUlpBackpressurePolicy(
    FalconModelInterface* falcon, uint32_t tx_packet_headroom)
    : falcon_(falcon), tx_packet_headroom_(tx_packet_headroom) {}

// We compute per-connection dynamic buffer limits based on the alpha carving
// algorithm. When FALCON resource is under pressure, or when a connection is
// slow, we reduce this connection's buffer limit, to avoid a global Xoff.
FalconCredit AlphaCarvingUlpBackpressurePolicy::ComputeAlphaCarving(
    uint32_t scid) {
  ConnectionStateManager* const state_manager = falcon_->get_state_manager();
  auto lookup_result = state_manager->PerformDirectLookup(scid);
  CHECK_OK(lookup_result.status());
  ConnectionState* const base_connection_state = lookup_result.value();
  auto connection_state =
      dynamic_cast<Gen2ConnectionState*>(base_connection_state);

  FalconResourceCredits credits =
      falcon_->get_resource_manager()->GetAvailableResourceCredits();
  FalconCredit dynamic_limit;
  dynamic_limit = credits;

  // Reserve a small amount of request_tx_packet and response_tx_packet
  // resources in FALCON. This part of the resource is not visible to the alpha
  // carving. Headroom is used to absorb the overflowed packets and reduces the
  // global xoff.
  if (tx_packet_headroom_ != 0) {
    if (dynamic_limit.request_tx_packet > tx_packet_headroom_)
      dynamic_limit.request_tx_packet -= tx_packet_headroom_;
    else
      dynamic_limit.request_tx_packet = 0;

    if (dynamic_limit.response_tx_packet > tx_packet_headroom_)
      dynamic_limit.response_tx_packet -= tx_packet_headroom_;
    else
      dynamic_limit.response_tx_packet = 0;
  }

  // Assuming we are calculating the dynamic limit for tx packet request
  // credits. The equation for a connection’s dynamic limit would be:
  // dynamic_limit = alpha * Tx_Packet_Request_Avaliable_Credits
  // We want to recover the alpha from Swift's alpha_select. Alpha_select is
  // stored in the connection context. Alpha can be obtained using the formula
  // alpha = 2 ^ (alpha_shift - alpha_select).
  double recovered_alpha_request =
      pow(2, (isekai::rue::kPerConnectionBackpressureAlphaShift -
              connection_state->connection_xoff_metadata.alpha_request));
  double recovered_alpha_response =
      pow(2, (isekai::rue::kPerConnectionBackpressureAlphaShift -
              connection_state->connection_xoff_metadata.alpha_response));

  VLOG(2) << "\nFree Resource Credits: " << dynamic_limit.ToString()
          << "\nAlpha Shift (request/response): "
          << static_cast<uint32_t>(
                 connection_state->connection_xoff_metadata.alpha_request)
          << " "
          << static_cast<uint32_t>(
                 connection_state->connection_xoff_metadata.alpha_response)
          << "\nAlpha Recoverd (request/response): " << recovered_alpha_request
          << " " << recovered_alpha_response;

  double min_value = 1, max_value = std::numeric_limits<uint32_t>::max();

  dynamic_limit.request_rx_buffer =
      std::clamp(dynamic_limit.request_rx_buffer * recovered_alpha_request,
                 min_value, max_value);
  dynamic_limit.request_tx_buffer =
      std::clamp(dynamic_limit.request_tx_buffer * recovered_alpha_request,
                 min_value, max_value);
  dynamic_limit.request_rx_packet =
      std::clamp(dynamic_limit.request_rx_packet * recovered_alpha_request,
                 min_value, max_value);
  dynamic_limit.request_tx_packet =
      std::clamp(dynamic_limit.request_tx_packet * recovered_alpha_request,
                 min_value, max_value);

  dynamic_limit.response_tx_packet =
      std::clamp(dynamic_limit.response_tx_packet * recovered_alpha_response,
                 min_value, max_value);
  dynamic_limit.response_tx_buffer =
      std::clamp(dynamic_limit.response_tx_buffer * recovered_alpha_response,
                 min_value, max_value);

  falcon_->get_stats_manager()->UpdateAlphaCarvingDynamicLimitCounters(
      scid, dynamic_limit);
  return dynamic_limit;
}

UlpXoff AlphaCarvingUlpBackpressurePolicy::ComputeXoffStatus(uint32_t scid) {
  FalconCredit dynamic_limit = ComputeAlphaCarving(scid);

  FalconConnectionCounters connection_counters =
      falcon_->get_stats_manager()->GetConnectionCounters(scid);

  UlpXoff ulp_xoff;
  bool request_xoff, response_xoff;

  request_xoff = !(connection_counters.tx_pkt_credits_ulp_requests <
                       dynamic_limit.request_tx_packet &&
                   connection_counters.tx_buf_credits_ulp_requests <
                       dynamic_limit.request_tx_buffer &&
                   connection_counters.rx_pkt_credits_ulp_requests <
                       dynamic_limit.request_rx_packet &&
                   connection_counters.rx_buf_credits_ulp_requests <
                       dynamic_limit.request_rx_buffer);

  response_xoff = !(connection_counters.tx_pkt_credits_ulp_data <
                        dynamic_limit.response_tx_packet &&
                    connection_counters.tx_buf_credits_ulp_data <
                        dynamic_limit.response_tx_buffer);

  VLOG(2) << "\nDynamic Limit: " << dynamic_limit.ToString()
          << "\nConnection Usage: "
          << connection_counters.tx_pkt_credits_ulp_requests << " "
          << connection_counters.tx_buf_credits_ulp_requests << " "
          << connection_counters.rx_pkt_credits_ulp_requests << " "
          << connection_counters.rx_buf_credits_ulp_requests << " "
          << connection_counters.tx_pkt_credits_ulp_data << " "
          << connection_counters.tx_buf_credits_ulp_data
          << "\nRequest/Response xoff: " << request_xoff << " "
          << response_xoff;

  ulp_xoff.request = request_xoff;
  ulp_xoff.response = response_xoff;
  return ulp_xoff;
}

}  // namespace isekai
