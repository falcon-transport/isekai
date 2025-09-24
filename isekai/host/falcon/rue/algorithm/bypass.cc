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

#include "isekai/host/falcon/rue/algorithm/bypass.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_split.h"
#include "absl/strings/string_view.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "isekai/host/falcon/rue/algorithm/algorithm.pb.h"
#include "isekai/host/falcon/rue/algorithm/swift.h"
#include "isekai/host/falcon/rue/bits.h"
#include "isekai/host/falcon/rue/format.h"
#include "isekai/host/falcon/rue/format_gen1.h"
#include "isekai/host/falcon/rue/format_gen2.h"

namespace isekai {
namespace rue {

std::array<bool, 4> GetAlwaysRepathFromFlag(absl::string_view flag_str) {
  std::array<bool, 4> always_repath_arr = {false, false, false, false};
  std::vector<std::string_view> always_repath_str =
      absl::StrSplit(flag_str, ',');
  if (always_repath_str.size() != 4) {
    return always_repath_arr;
  }
  for (int i = 0; i < 4; ++i) {
    bool parsed;
    if (absl::SimpleAtob(always_repath_str[i], &parsed)) {
      always_repath_arr[i] = parsed;
    }
  }
  return always_repath_arr;
}

std::array<int, 4> GetFlowWeightsFromString(std::string_view str) {
  std::array<int, 4> flow_weights_int = {
      kBypassDefaultFlowWeight, kBypassDefaultFlowWeight,
      kBypassDefaultFlowWeight, kBypassDefaultFlowWeight};
  std::vector<std::string_view> flow_weights_str = absl::StrSplit(str, ',');
  if (flow_weights_str.size() != 4) {
    return flow_weights_int;
  }
  for (int i = 0; i < 4; ++i) {
    int weight;
    if (absl::SimpleAtoi(flow_weights_str[i], &weight) &&
        weight >= kBypassMinFlowWeight && weight <= kBypassMaxFlowWeight) {
      flow_weights_int[i] = weight;
    }
  }
  return flow_weights_int;
}

template <typename EventT, typename ResponseT>
Bypass<EventT, ResponseT>::Bypass(const BypassConfiguration& config) {
  if (config.has_gen2_test_only()) {
    if (!config.gen2_test_only().has_override_flow_weight_flow1() ||
        !config.gen2_test_only().has_override_flow_weight_flow2() ||
        !config.gen2_test_only().has_override_flow_weight_flow3() ||
        !config.gen2_test_only().has_override_flow_weight_flow4()) {
      LOG(ERROR) << "All flow weights should be specified for overriding flow "
                    "weights.";
    }
    uint8_t flow_weight_flow1 =
        config.gen2_test_only().override_flow_weight_flow1();
    uint8_t flow_weight_flow2 =
        config.gen2_test_only().override_flow_weight_flow2();
    uint8_t flow_weight_flow3 =
        config.gen2_test_only().override_flow_weight_flow3();
    uint8_t flow_weight_flow4 =
        config.gen2_test_only().override_flow_weight_flow4();
    bool flow_weight_flow1_valid = flow_weight_flow1 <= kBypassMaxFlowWeight;
    bool flow_weight_flow2_valid = flow_weight_flow2 <= kBypassMaxFlowWeight;
    bool flow_weight_flow3_valid = flow_weight_flow3 <= kBypassMaxFlowWeight;
    bool flow_weight_flow4_valid = flow_weight_flow4 <= kBypassMaxFlowWeight;

    LOG_IF(ERROR, !flow_weight_flow1_valid)
        << "Flow weights should be within the range of ["
        << kBypassMinFlowWeight << ", " << kBypassMaxFlowWeight << "].";

    LOG_IF(ERROR, !flow_weight_flow2_valid)
        << "Flow weights should be within the range of ["
        << kBypassMinFlowWeight << ", " << kBypassMaxFlowWeight << "].";

    LOG_IF(ERROR, !flow_weight_flow3_valid)
        << "Flow weights should be within the range of ["
        << kBypassMinFlowWeight << ", " << kBypassMaxFlowWeight << "].";

    LOG_IF(ERROR, !flow_weight_flow4_valid)
        << "Flow weights should be within the range of ["
        << kBypassMinFlowWeight << ", " << kBypassMaxFlowWeight << "].";

    if (flow_weight_flow1_valid) {
      flow_weight_flow1_ = flow_weight_flow1;
    }
    if (flow_weight_flow2_valid) {
      flow_weight_flow2_ = flow_weight_flow2;
    }
    if (flow_weight_flow3_valid) {
      flow_weight_flow3_ = flow_weight_flow3;
    }
    if (flow_weight_flow4_valid) {
      flow_weight_flow4_ = flow_weight_flow4;
    }

    if (config.gen2_test_only().has_always_repath_flow1()) {
      always_repath_flow1_ = config.gen2_test_only().always_repath_flow1();
    }
    if (config.gen2_test_only().has_always_repath_flow2()) {
      always_repath_flow2_ = config.gen2_test_only().always_repath_flow2();
    }
    if (config.gen2_test_only().has_always_repath_flow3()) {
      always_repath_flow3_ = config.gen2_test_only().always_repath_flow3();
    }
    if (config.gen2_test_only().has_always_repath_flow4()) {
      always_repath_flow4_ = config.gen2_test_only().always_repath_flow4();
    }

    if (config.gen2_test_only().has_always_wrr_restart()) {
      always_wrr_restart_ = config.gen2_test_only().always_wrr_restart();
    }

    if (config.gen2_test_only().has_override_alpha_request()) {
      alpha_request_ = config.gen2_test_only().override_alpha_request();
    }
    if (config.gen2_test_only().has_override_alpha_response()) {
      alpha_response_ = config.gen2_test_only().override_alpha_response();
    }

    if (config.gen2_test_only().has_override_fipg()) {
      fipg_ = config.gen2_test_only().override_fipg();
    }
    if (config.gen2_test_only().has_override_nipg()) {
      nipg_ = config.gen2_test_only().override_nipg();
    }
  }
  flow_label_generator_ =
      std::make_unique<FlowLabelGenerator>(absl::ToUnixNanos(absl::Now()));
}

template <typename EventT, typename ResponseT>
absl::Status Bypass<EventT, ResponseT>::InstallAlgorithmProfile(
    int profile_index, AlgorithmConfiguration profile) {
  if (!profile.has_bypass()) {
    return absl::InvalidArgumentError("Not a bypass profile");
  }
  return absl::OkStatus();
}

template <typename EventT, typename ResponseT>
absl::Status Bypass<EventT, ResponseT>::UninstallAlgorithmProfile(
    int profile_index) {
  return absl::OkStatus();
}

template <>
void Bypass<falcon_rue::Event_Gen2, falcon_rue::Response_Gen2>::Process(
    const falcon_rue::Event_Gen2& event, falcon_rue::Response_Gen2& response,
    uint32_t now) const {
  constexpr double kFalconUnitTimeUs = 0.131072;
  const uint32_t kDefaultRetransmissionTimeout =
      std::round(1000 / kFalconUnitTimeUs);  // ~1ms.
  uint8_t flow_id = Swift<falcon_rue::Event_Gen2,
                          falcon_rue::Response_Gen2>::GetFlowIdFromEvent(event);
  uint32_t flow_label_1 = 0;
  uint32_t flow_label_2 = 0;
  uint32_t flow_label_3 = 0;
  uint32_t flow_label_4 = 0;
  uint32_t rand_flow_label = flow_label_generator_->GetFlowLabel();

  if (always_repath_flow1_) {
    flow_label_1 = (rand_flow_label << falcon_rue::kFlowIdBits) | 0x0;
  }
  if (always_repath_flow2_) {
    flow_label_2 = (rand_flow_label << falcon_rue::kFlowIdBits) | 0x1;
  }
  if (always_repath_flow3_) {
    flow_label_3 = (rand_flow_label << falcon_rue::kFlowIdBits) | 0x2;
  }
  if (always_repath_flow4_) {
    flow_label_4 = (rand_flow_label << falcon_rue::kFlowIdBits) | 0x3;
  }

  // Writes the values to the response.
  falcon_rue::SetResponse(
      /*connection_id=*/event.connection_id,
      /*cc_metadata=*/event.cc_metadata,
      /*fabric_congestion_window=*/
      (fipg_ > 0 ? 1 : event.fabric_congestion_window),
      /*fabric_inter_packet_gap=*/fipg_,
      /*nic_congestion_window=*/(nipg_ > 0 ? 1 : event.nic_congestion_window),
      /*retransmit_timeout=*/kDefaultRetransmissionTimeout,
      /*fabric_window_time_marker=*/0,
      /*nic_window_time_marker=*/event.nic_window_time_marker,
      /*event_queue_select=*/event.event_queue_select,
      /*delay_select=*/event.delay_select,
      /*base_delay=*/event.base_delay,
      /*delay_state=*/event.delay_state,
      /*rtt_state=*/event.rtt_state,
      /*cc_opaque=*/event.cc_opaque,
      /*plb_state=*/event.plb_state,
      /*alpha_request=*/alpha_request_,
      /*alpha_response=*/alpha_response_,
      /*nic_inter_packet_gap=*/nipg_,
      /*flow_label_1=*/flow_label_1,
      /*flow_label_2=*/flow_label_2,
      /*flow_label_3=*/flow_label_3,
      /*flow_label_4=*/flow_label_4,
      /*flow_label_1_valid=*/always_repath_flow1_,
      /*flow_label_2_valid=*/always_repath_flow2_,
      /*flow_label_3_valid=*/always_repath_flow3_,
      /*flow_label_4_valid=*/always_repath_flow4_,
      /*flow_label_1_weight=*/flow_weight_flow1_,
      /*flow_label_2_weight=*/flow_weight_flow2_,
      /*flow_label_3_weight=*/flow_weight_flow3_,
      /*flow_label_4_weight=*/flow_weight_flow4_,
      /*wrr_restart_round=*/always_wrr_restart_,
      /*flow_id=*/flow_id,
      /*csig_enable=*/event.csig_enable,
      /*csig_select=*/0,
      /*ar_rate=*/0,
      /*response=*/response);
}

// Explicit template instantiations.
template class Bypass<falcon_rue::Event_GEN1, falcon_rue::Response_GEN1>;
template class Bypass<falcon_rue::Event_Gen2, falcon_rue::Response_Gen2>;

}  // namespace rue
}  // namespace isekai
