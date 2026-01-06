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

#ifndef ISEKAI_HOST_FALCON_RUE_ALGORITHM_BYPASS_H_
#define ISEKAI_HOST_FALCON_RUE_ALGORITHM_BYPASS_H_

#include <array>
#include <cstdint>
#include <memory>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "isekai/host/falcon/falcon.h"
#include "isekai/host/falcon/rue/algorithm/algorithm.pb.h"
#include "isekai/host/falcon/rue/algorithm/bypass.pb.h"
#include "isekai/host/falcon/rue/algorithm/connection_state.h"
#include "isekai/host/falcon/rue/algorithm/hardware_state.h"
#include "isekai/host/falcon/rue/algorithm/swift.h"
#include "isekai/host/falcon/rue/format.h"
#include "isekai/host/falcon/rue/format_gen2.h"
#include "isekai/host/falcon/rue/util.h"

namespace isekai {
namespace rue {

//
// kMinFlowWeight once Swift implements flow banning.
constexpr int kBypassDefaultFlowWeight = 1;
// Having minimum flow weight of 0 in order to test flow weight of zero is
// effectively banning the flows for Gen_2 bughunt purpopse.
constexpr int kBypassMinFlowWeight = 0;
constexpr int kBypassMaxFlowWeight = kMaxFlowWeight;

constexpr uint32_t kBypassDefaultFlowLabel = 0;

// Utility function to convert always_repath flag string to array of booleans.
std::array<bool, 4> GetAlwaysRepathFromFlag(absl::string_view flag_str);

// Utility function to convert flow weight string to array of flow weights.
std::array<int, 4> GetFlowWeightsFromString(std::string_view flow_weights_str);

// Utility function to convert flow label string to array of flow labels.
std::array<uint32_t, 4> GetFlowLabelsFromString(
    std::string_view flow_labels_str);

// This is a simple RUE algorithm module that only keeps the
// fabric congestion window, NIC congestion window, and the inter packet gap
// at their default values.

template <typename EventT, typename ResponseT>
class Bypass {
 public:
  // The algorithm class has to expose the template arguments as EventType and
  // ResponseType so that the StatefulAlgorithm class can internally deduce
  // them.
  using EventType = EventT;
  using ResponseType = ResponseT;

  ~Bypass() = default;
  Bypass() = default;
  Bypass(const Bypass&) = delete;
  Bypass& operator=(const Bypass&) = delete;
  explicit Bypass(const BypassConfiguration& config);

  template <typename ChildT = Bypass>
  static absl::StatusOr<std::unique_ptr<ChildT>> Create(
      const BypassConfiguration& config);

  // Processes an event and generates the response.
  void Process(const EventT& event, ResponseT& response, uint32_t now) const;

  // Fallback for compatibility with Swift arguments (hw_state and state args
  // are unused).
  void Process(const EventT& event, ResponseT& response, uint32_t now,
               ConnectionState<EventType>* state) {
    //
    // to explicitly pass nullptr for hw_state or state.
    return Process(event, response, now);
  }

  // Processes an event for a multipath connection and generates the response.
  void ProcessMultipath(const EventT& event, ResponseT& response,
                        ConnectionState<EventType>& rue_connection_state,
                        uint32_t now) {
    // For bypass, there is no difference between Process() and
    // ProcessMultipath().
    Process(event, response, now);
  }
  absl::Status InstallAlgorithmProfile(int profile_index,
                                       AlgorithmConfiguration profile);
  absl::Status UninstallAlgorithmProfile(int profile_index);
  void PickProfile(const EventT& event) {}

  uint8_t flow_weight_flow1_ = kBypassDefaultFlowWeight;
  uint8_t flow_weight_flow2_ = kBypassDefaultFlowWeight;
  uint8_t flow_weight_flow3_ = kBypassDefaultFlowWeight;
  uint8_t flow_weight_flow4_ = kBypassDefaultFlowWeight;
  bool always_repath_flow1_ = false;
  bool always_repath_flow2_ = false;
  bool always_repath_flow3_ = false;
  bool always_repath_flow4_ = false;
  bool always_wrr_restart_ = false;
  int alpha_request_ = 0;
  int alpha_response_ = 0;
  uint32_t flow_label_1_ = 0;
  uint32_t flow_label_2_ = 0;
  uint32_t flow_label_3_ = 0;
  uint32_t flow_label_4_ = 0;

  std::unique_ptr<FlowLabelGenerator> flow_label_generator_;
  int retransmit_timeout_;
  const BypassConfiguration config_;
};

bool ShouldDisableCsig(uint32_t connection_id,
                       const BypassConfiguration& config);

template <typename EventT, typename ResponseT>
template <typename ChildT>
absl::StatusOr<std::unique_ptr<ChildT>> Bypass<EventT, ResponseT>::Create(
    const BypassConfiguration& config) {
  return std::make_unique<ChildT>(config);
}

template <typename EventT, typename ResponseT>
void Bypass<EventT, ResponseT>::Process(const EventT& event,
                                        ResponseT& response,
                                        uint32_t now) const {
  falcon_rue::PacketTiming timing = falcon_rue::GetPacketTiming(event);
  // Writes the values to the response
  falcon_rue::SetResponse(
      /*connection_id=*/event.connection_id,
      /*randomize_path=*/false,
      /*cc_metadata=*/event.cc_metadata,
      /*fabric_congestion_window=*/event.fabric_congestion_window,
      /*inter_packet_gap=*/event.inter_packet_gap,
      /*nic_congestion_window=*/event.nic_congestion_window,
      /*retransmit_timeout=*/event.retransmit_timeout,
      /*fabric_window_time_marker=*/0,
      /*nic_window_time_marker=*/0,
      /*nic_window_direction=*/falcon::WindowDirection::kDecrease,
      /*event_queue_select=*/event.event_queue_select,
      /*delay_select=*/event.delay_select,
      /*base_delay=*/event.base_delay,
      /*delay_state=*/timing.delay,
      /*rtt_state=*/timing.rtt,
      /*cc_opaque=*/event.cc_opaque,
      /*response=*/response);
}

// Template specialization for Gen_2.
template <>
void Bypass<falcon_rue::Event_Gen2, falcon_rue::Response_Gen2>::Process(
    const falcon_rue::Event_Gen2& event, falcon_rue::Response_Gen2& response,
    uint32_t now) const;

typedef Bypass<falcon_rue::Event, falcon_rue::Response> BypassGen1;
typedef Bypass<falcon_rue::Event_Gen2, falcon_rue::Response_Gen2> BypassGen2;

}  // namespace rue
}  // namespace isekai

#endif  // ISEKAI_HOST_FALCON_RUE_ALGORITHM_BYPASS_H_
