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

#ifndef ISEKAI_HOST_FALCON_RUE_ALGORITHM_SWIFT_H_
#define ISEKAI_HOST_FALCON_RUE_ALGORITHM_SWIFT_H_

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <random>
#include <type_traits>

#include "absl/base/attributes.h"
#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "isekai/common/status_util.h"
#include "isekai/host/falcon/falcon.h"
#include "isekai/host/falcon/rue/algorithm/algorithm.pb.h"
#include "isekai/host/falcon/rue/algorithm/connection_state.h"
#include "isekai/host/falcon/rue/algorithm/hardware_state.h"
#include "isekai/host/falcon/rue/algorithm/swift.pb.h"
#include "isekai/host/falcon/rue/bits.h"
#include "isekai/host/falcon/rue/fixed.h"
#include "isekai/host/falcon/rue/format.h"
#include "isekai/host/falcon/rue/format_gen1.h"
#include "isekai/host/falcon/rue/format_gen2.h"
#include "isekai/host/falcon/rue/util.h"

namespace isekai {
namespace rue {

// Configures maximum number of profiles supported by Swift.
// Profile is stored in the base_delay field which is 24 bits. Profile bits can
// be increased up to 24 bits if other bits in base_delay are unused.
constexpr uint8_t kSwiftNumProfileBits = 3;
constexpr uint32_t kSwiftMaxNumProfiles = (1 << kSwiftNumProfileBits);
constexpr uint32_t kSwiftDefaultProfileIndex = 0;
// Assuming profile_index is stored in the upper most bits of base_delay which
// is falcon_rue::kTimeBits wide.
constexpr uint32_t kSwiftProfileIndexShift =
    falcon_rue::kTimeBits - kSwiftNumProfileBits;

constexpr double kMaxFabricCongestionWindow =
    falcon_rue::FixedToFloat<uint32_t, double>(
        falcon_rue::ValidMask<uint32_t>(
            falcon_rue::kFabricCongestionWindowBits),
        falcon_rue::kFractionalBits);
constexpr double kMinFabricCongestionWindow =
    falcon_rue::FixedToFloat<uint32_t, double>(
        falcon_rue::ValidMask<uint32_t>(0x1), falcon_rue::kFractionalBits);
//
// Swift's open-sourced version in Isekai.
constexpr double kMaxGen1NicCongestionWindow =
    falcon_rue::ValidMask<uint32_t>(falcon_rue::kGen1NicCongestionWindowBits);
constexpr double kMinGen1NicCongestionWindow =
    falcon_rue::FixedToFloat<uint32_t, double>(
        falcon_rue::ValidMask<uint32_t>(0x1), falcon_rue::kFractionalBits);
constexpr double kMaxGen2NicCongestionWindow =
    falcon_rue::FixedToFloat<uint32_t, double>(
        falcon_rue::ValidMask<uint32_t>(
            falcon_rue::kGen2NicCongestionWindowBits),
        falcon_rue::kFractionalBits);
constexpr double kMinGen2NicCongestionWindow =
    falcon_rue::FixedToFloat<uint32_t, double>(
        falcon_rue::ValidMask<uint32_t>(0x1), falcon_rue::kFractionalBits);
constexpr uint8_t kMaxRxBufferLevel =
    falcon_rue::ValidMask<uint8_t>(falcon_rue::kRxBufferLevelBits);
constexpr uint8_t kMinRxBufferLevel = 0;
constexpr uint16_t kMaxTopoScalingPerHop = falcon_rue::ValidMask<uint16_t>(12);

constexpr uint64_t kRandomizePathEnableMask = 0x1;
// The mask to get the flow ID which is embedded in the last two bits of the
// flow label.
constexpr uint64_t kFlowIdMask = 0b11;

constexpr uint32_t kMaxPlbAckCount =
    falcon_rue::ValidMask<uint32_t>(falcon_rue::kBitsOfPlbAckCounter);
constexpr uint32_t kMaxPlbAttemptCount =
    falcon_rue::ValidMask<uint32_t>(falcon_rue::kBitsOfPlbAttemptCounter);

constexpr double kMaxArCwndThreshold = 2.0;

// Min and max values of alpha supported by the hardware. The hardware accepts a
// 4 bit value (alpha_select) and computes alpha = 2 ^ (3 - alpha_select). Here,
// 3 is the alpha_shift.
constexpr uint8_t kPerConnectionBackpressureAlphaShift = 3;
constexpr double kPerConnectionBackpressureMaxAlpha = 8;  // 1 >> 3.
constexpr double kPerConnectionBackpressureMinAlpha = 1.0 / (1 << 12);

// The maximum and minimum flow weight value for multipath connections.
constexpr uint8_t kMinFlowWeight = 1;
constexpr uint8_t kMaxFlowWeight = (1 << falcon_rue::kFlowLabelWeightBits) - 1;

// The only requirement for these default flow label values is
// that the least significant 2 bits should correctly represent the flow ID for
// the flow, and that they should be at most 20 bits long.
constexpr uint32_t kDefaultFlowLabel1 = 0x01230;
constexpr uint32_t kDefaultFlowLabel2 = 0x45671;
constexpr uint32_t kDefaultFlowLabel3 = 0x89012;
constexpr uint32_t kDefaultFlowLabel4 = 0x23453;

struct __attribute__((packed)) SwiftParameters {
  bool valid;
  bool max_decrease_on_eack_nack_drop;
  bool calc_rtt_smooth;
  bool randomize_path;

  uint8_t plb_attempt_threshold;
  uint8_t retransmit_limit;
  uint8_t target_rx_buffer_level;
  uint8_t ipg_bits;

  // Fabric window settings
  double max_fabric_congestion_window;
  double min_fabric_congestion_window;
  double fabric_additive_increment_factor;
  double fabric_multiplicative_decrease_factor;
  double max_fabric_multiplicative_decrease;

  // NIC window settings
  double max_nic_congestion_window;
  double min_nic_congestion_window;
  double nic_additive_increment_factor;
  double nic_multiplicative_decrease_factor;
  double max_nic_multiplicative_decrease;

  // Common settings
  double rtt_smoothing_alpha;
  double delay_smoothing_alpha;
  double retransmit_timeout_scalar;
  double flow_scaling_alpha;
  double flow_scaling_beta;
  double ipg_time_scalar;

  double plb_target_rtt_multiplier;
  double plb_congestion_threshold;

  uint32_t min_retransmit_timeout;
  uint32_t max_flow_scaling;
  uint32_t fabric_base_delay;

  // Setting to ensure swift is deterministic.
  uint32_t flow_label_rng_seed;

  // Per-connection backpressure settings.
  double backpressure_alpha;
  double backpressure_retransmit_alpha;

  uint16_t topo_scaling_per_hop;
};

// The RUE swift class generates random flow labels under certain conditions.
// FlowLabelGenerator is a helper class that handles the random number
// generation.

class FlowLabelGenerator {
 public:
  explicit FlowLabelGenerator(uint32_t seed) : rng_(seed) {}
  uint32_t GetFlowLabel() { return dist_(rng_); }

 private:
  std::mt19937 rng_;
  // The valid random flow label range is [0, 2^(kFlowLabelBits-kFlowIdBits)).
  std::uniform_int_distribution<uint32_t> dist_{
      0, (1 << (falcon_rue::kFlowLabelBits - falcon_rue::kFlowIdBits)) - 1};
};

// The RUE swift class is the core algorithm of the overall software RUE. It
// is responsible for the logic and arithmetic of the congestion control
// algorithm, of which it is implementing the Swift algorithm.
// The RUE receives RUE events and returns RUE responses. The object is
// explicitly given the current FALCON unit time by the outer RUE framework
// before events are processed. This implementation supports multiple profiles
// of Swift for different connections. In case the profile is removed, and we
// get an event from the connection that uses the profile, the default profile
// installed at initialization will be used.
template <typename EventT, typename ResponseT>
class Swift {
 public:
  // The algorithm class has to expose the template arguments as EventType and
  // ResponseType so that the StatefulAlgorithm class can internally deduce
  // them.
  using EventType = EventT;
  using ResponseType = ResponseT;

  ~Swift() = default;
  Swift(const Swift&) = delete;
  Swift& operator=(const Swift&) = delete;
  explicit Swift(SwiftParameters default_parameters);

  // Returns default values for Swift configuration
  static SwiftConfiguration DefaultConfiguration();

  // Users must use this factory function to create the object
  template <typename ChildT = Swift>
  static absl::StatusOr<std::unique_ptr<ChildT>> Create(
      const SwiftConfiguration& config);

  // Helper functions to get correct values for swift flow scaling parameters
  static absl::StatusOr<double> CalculateFlowScalingAlpha(
      int max_flow_scaling, double min_flow_scaling_window,
      double max_flow_scaling_window);
  static absl::StatusOr<double> CalculateFlowScalingBeta(
      double flow_scaling_alpha, double max_flow_scaling_window);

  // Processes an event and generates the response.
  void Process(const EventT& event, ResponseT& response, uint32_t now,
               ConnectionState<EventT>* state);

  // Fallback in stateless mode when state is not passed - provides nullptr to
  // the process function.
  void Process(const EventT& event, ResponseT& response, uint32_t now) {
    return Process(event, response, now, /*state=*/nullptr);
  }

  // Processes an event for a multipath connection and generates the response.
  //
  // ProcessStateful().
  void ProcessMultipath(const EventT& event, ResponseT& response,
                        ConnectionState<EventT>& state, uint32_t now) {
    // Do nothing by default for Gen_1.
  }

  absl::Status InstallAlgorithmProfile(int profile_index,
                                       AlgorithmConfiguration profile);
  absl::Status UninstallAlgorithmProfile(int profile_index);

  static uint32_t MakeBaseDelayField(uint8_t profile_index,
                                     uint32_t state_table_index = 0);
  static int GetProfileIndex(uint32_t event_base_delay);
  static uint32_t GetStateTableIndex(uint32_t event_base_delay);

  // Given a previously smoothed value and a new value, this function returns
  // the new smoothed value based on the specified EWMA smoothing alpha.
  // Warning: alpha must be specified between [0, 1]
  template <typename T>
  static T GetSmoothed(double alpha, T smoothed_value, T value) {
    return static_cast<T>(smoothed_value * alpha + value * (1.0 - alpha));
  }

  // Main event processors
  void ProcessAckNack(const EventT& event, ResponseT& response, uint32_t now,
                      ConnectionState<EventT>* state);

  void ProcessRetransmit(const EventT& event, ResponseT& response, uint32_t now,
                         ConnectionState<EventT>* state) const;

  // Main event processors for multipath-enabled connections.
  void ProcessAckNackMultipath(const EventT& event, ResponseT& response,
                               ConnectionState<EventT>& state, uint32_t now) {
    // Do nothing by default for Gen_1.
  }
  void ProcessRetransmitMultipath(const EventT& event, ResponseT& response,
                                  ConnectionState<EventT>& state,
                                  uint32_t now) const {
    // Do nothing by default for Gen_1.
  }

  // Computes the fabric congestion window for ACK events
  double ComputeAckFabricCongestionWindow(
      const EventT& event, double fabric_congestion_window,
      uint32_t smoothed_delay, uint32_t target_delay,
      const falcon_rue::PacketTiming& timing,
      uint32_t fabric_decrease_delta) const;

  // Computes the fabric congestion window for NACK events
  double ComputeNackFabricCongestionWindow(
      const EventT& event, double fabric_congestion_window,
      const falcon_rue::PacketTiming& timing,
      uint32_t fabric_decrease_delta) const;

  // Computes the fabric congestion window for timeout events
  double ComputeTimeoutFabricCongestionWindow(
      const EventT& event, double fabric_congestion_window,
      uint32_t fabric_decrease_delta) const;

  // Computes the nic congestion window for ACK events
  double ComputeAckNicCongestionWindow(
      const EventT& event, double nic_congestion_window,
      uint32_t nic_change_delta, const falcon_rue::PacketTiming& timing) const;

  // Computes the nic congestion window for NACK events
  double ComputeNackNicCongestionWindow(
      const EventT& event, double nic_congestion_window,
      uint32_t nic_change_delta, const falcon_rue::PacketTiming& timing) const;

  // Computes the PLB decision. The plb_state will be modified by the function.
  bool ComputePlb(const EventT& event, uint32_t smoothed_delay,
                  uint32_t target_delay, PlbState& plb_state,
                  double old_window_size);

  // Computes the randomize path flag for the response
  bool ComputeRandomizePath(const EventT& event) const;

  // Updates the multipath flow labels and valid bits.
  void UpdateFlowLabels(const EventT& event, bool reroute, uint8_t flow_id,
                        // Outputs passed by reference.
                        bool& flow_label_1_valid, bool& flow_label_2_valid,
                        bool& flow_label_3_valid, bool& flow_label_4_valid,
                        uint32_t& flow_label_1, uint32_t& flow_label_2,
                        uint32_t& flow_label_3, uint32_t& flow_label_4) {
    // Do nothing by default for Gen_1.
  }

  // Computes the flow weight given the flow fcwnd in float format, and the
  // connection fcwnd in float format.
  static uint8_t ComputeFlowWeight(double flow_fcwnd, double connection_fcwnd);

  // Updates CSIG variables.
  void ComputeCsigVariables(const EventT& event,
                            // Outputs passed by reference.
                            bool& csig_enable, uint8_t& csig_select) const {
    // Do nothing by default for Gen_1.
  }

  // Updates per-connection backpressure variables.
  void ComputePerConnectionBackpressureVariables(
      const EventT& event, const falcon_rue::PacketTiming& packet_timing,
      uint32_t target_delay,
      // Outputs passed by reference.
      uint8_t& alpha_request, uint8_t& alpha_response) const {
    // Do nothing by default for Gen_1.
  }

  // Computes AR rate.
  void ComputeArRate(const EventT& event, double fcwnd, double ncwnd,
                     // Outputs passed by reference.
                     uint8_t& ar_rate) const {
    // Do nothing by default for Gen_1.
  }

  // Returns the target delay for the packet.
  // All time values (topo_scaling_per_hop, flow_scaling_alpha,
  // flow_scaling_beta, max_flow_scaling, base_delay, and the return value) are
  // all abstract time values. SW RUE uses FALCON unit time for these values.
  static uint32_t GetTargetDelay(uint16_t topo_scaling_per_hop,
                                 double flow_scaling_alpha,
                                 double flow_scaling_beta,
                                 uint32_t max_flow_scaling, uint32_t base_delay,
                                 double congestion_window, uint8_t hops);

  // Computes the inter packet gap based on the current congestion window and
  // the RTT. ipg_time_scalar converts from the input time units (RTT) to the
  // output time units. SW RUE uses this functionality to convert from FALCON
  // unit time to traffic shaper unit time which is required by the RUE
  // interface API. ipg_bits is the maximum number of bits that can be used in
  // the output IPG value. The output of this function performs saturating
  // arithmetic.
  static uint32_t GetInterPacketGap(double ipg_time_scalar, uint8_t ipg_bits,
                                    double congestion_window, uint32_t rtt);

  // Determines the retransmit timeout value based on the given RTT.
  // min_retransmit_timeout, rtt, and the return value are in abstract time
  // units. SW RUE uses FALCON unit time for these values.
  static uint32_t GetRetransmitTimeout(uint32_t min_retransmit_timeout,
                                       uint32_t rtt, double rtt_scalar,
                                       uint32_t inter_packet_gap,
                                       double ipg_time_scalar);

  // Generates a random flow label belonging with a specific flow ID.
  uint32_t RandomFlowLabel(uint8_t flow_id);

  // Converts a per-connection backpressure alpha into alpha_shift that is
  // required by the hardware.
  uint8_t ConvertPerConnectionAlphaToShift(double alpha) const;

  // Extracts the flow ID from the flow label. The flow ID is stored as the last
  // two bits in the flow label.
  static uint8_t GetFlowIdFromEvent(const EventT& event);

 private:
  // Validates the Swift configuration affecting the delay_state field in
  // the Event/Response.
  static absl::Status ValidateDelayStateConfig(
      const SwiftConfiguration& config);

  // Validates the Swift configuration affecting the max and min NIC congestion
  // window values.
  static absl::Status ValidateNicCongestionWindowBounds(
      const SwiftConfiguration& config);

  // Extracts the PLB state stored in the event.
  template <typename EventU = EventT>
  static std::enable_if_t<!std::is_same_v<EventU, falcon_rue::Event_GEN1>,
                          uint32_t>
  GetPlbStateFromEvent(const EventT& event);

  // Extracts the PLB state stored in the connection state.
  template <typename EventU = EventT>
  static std::enable_if_t<std::is_same_v<EventU, falcon_rue::Event_GEN1>,
                          uint32_t>
  GetPlbStateFromConnectionState(ConnectionState<EventT>* state);

  // Converts the NIC congestion window field in an event to a float. If NCWND
  // fractional bits are included in event, then state parameter is unused.
  // Otherwise, the fractional part is added from the value in ConnectionState.
  double GetFloatNcwnd(const EventT& event,
                       const ConnectionState<EventT>* state) const;

  // Converts the float NIC congestion window to the right format for the
  // response.
  uint32_t ConvertFloatNcwndToFixed(double nic_congestion_window) const;

  // Computes the NIC inter packet gap based on the current NIC congestion
  // window and the RTT. ipg_time_scalar converts from the input time units
  // (RTT) to the output time units. SW RUE uses this functionality to convert
  // from FALCON unit time to traffic shaper unit time which is required by the
  // RUE interface API. ipg_bits is the maximum number of bits that can be used
  // in the output IPG value. The output of this function performs saturating
  // arithmetic.
  static uint32_t GetNicInterPacketGap(double ipg_time_scalar, uint8_t ipg_bits,
                                       double congestion_window, uint32_t rtt);

  // Derives the value of the NIC inter packet gap (nipg) from an event and
  // connection state (if NCWND fractional bits are stored in ConnectionState).
  uint32_t GetNipg(const EventT& event,
                   const ConnectionState<EventT>* state) const;

  // Stores fractional part of NCWND in ConnectionState when event format does
  // not include NCWND fractional bits.
  void StoreFloatNcwndFraction(ConnectionState<EventT>* state,
                               double nic_congestion_window);

  bool UseMaxFabricDecrease(const EventT& event) const;

  falcon::WindowDirection GetWindowDirectionFromEvent(
      const EventT& event) const {
    return event.nic_window_direction;
  }

  falcon_rue::NicWindowGuardInfo GetNicWindowGuardInfo(
      const EventT& event, uint32_t now, uint32_t calc_rtt,
      double old_nic_congestion_window, double new_nic_congestion_window) const;

  static absl::StatusOr<SwiftParameters> ConvertProfileToParameters(
      const SwiftConfiguration& config);

  void PickProfile(const EventT& event);

  // clang-format off
  void SetResponse(
      uint32_t connection_id,
      bool randomize_path,                        // Removed in Gen_2.
      uint32_t cc_metadata,
      uint32_t fabric_congestion_window,
      uint32_t fabric_inter_packet_gap,
      uint32_t nic_congestion_window,
      uint32_t retransmit_timeout,
      uint32_t fabric_window_time_marker,
      uint32_t nic_window_time_marker,
      falcon::WindowDirection nic_window_direction,  // Removed in Gen_2.
      uint8_t event_queue_select,
      falcon::DelaySelect delay_select,
      uint32_t base_delay,
      uint32_t delay_state,
      uint32_t rtt_state,
      uint32_t cc_opaque,
      // Fields below added in Gen_2.
      uint32_t plb_state,
      uint8_t ar_rate,
      uint8_t alpha_request,
      uint8_t alpha_response,
      uint32_t nic_inter_packet_gap,
      uint32_t flow_label_1,
      uint32_t flow_label_2,
      uint32_t flow_label_3,
      uint32_t flow_label_4,
      bool flow_label_1_valid,
      bool flow_label_2_valid,
      bool flow_label_3_valid,
      bool flow_label_4_valid,
      uint8_t flow_label_1_weight,
      uint8_t flow_label_2_weight,
      uint8_t flow_label_3_weight,
      uint8_t flow_label_4_weight,
      bool wrr_restart_round,
      uint8_t flow_id,
      bool csig_enable,
      uint8_t csig_select,
      ResponseT& response) const;
  // clang-format on

  SwiftParameters* parameters_ = nullptr;
  std::array<SwiftParameters, kSwiftMaxNumProfiles> profiles_;
  // Uniform random bit generator for generating random flow labels.
  std::unique_ptr<FlowLabelGenerator> flow_label_generator_;
};

template <typename EventT, typename ResponseT>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE void Swift<EventT, ResponseT>::PickProfile(
    const EventT& event) {
  int profile_index = GetProfileIndex(event.base_delay);
  if (profile_index < 0 || profile_index >= profiles_.size() ||
      !profiles_[profile_index].valid) {
    profile_index = kSwiftDefaultProfileIndex;
  }
  parameters_ = &profiles_[profile_index];
  //
  DCHECK(parameters_->valid);
}

template <typename EventT, typename ResponseT>
template <typename ChildT>
absl::StatusOr<std::unique_ptr<ChildT>> Swift<EventT, ResponseT>::Create(
    const SwiftConfiguration& config) {
  ASSIGN_OR_RETURN(SwiftParameters parameters,
                   ConvertProfileToParameters(config));
  return std::make_unique<ChildT>(parameters);
}

template <typename EventT, typename ResponseT>
void Swift<EventT, ResponseT>::Process(const EventT& event, ResponseT& response,
                                       uint32_t now,
                                       ConnectionState<EventT>* state) {
  PickProfile(event);
  DCHECK(parameters_ != nullptr);

  switch (event.event_type) {
    case (falcon::RueEventType::kAck):
    case (falcon::RueEventType::kNack):
      ProcessAckNack(event, response, now, state);
      break;
    case (falcon::RueEventType::kRetransmit):
      ProcessRetransmit(event, response, now, state);
      break;
  }
}

template <typename EventT, typename ResponseT>
void Swift<EventT, ResponseT>::ProcessAckNack(const EventT& event,
                                              ResponseT& response, uint32_t now,
                                              ConnectionState<EventT>* state) {
  // The Swift processing is performed via "modules" consisting of functions
  // templated on EventT which compute one or more related variables. Details of

  // Module 0: Compute common variables required by multiple subsequent modules.
  falcon_rue::PacketTiming timing = falcon_rue::GetPacketTiming(event);
  double old_fabric_congestion_window =
      falcon_rue::FixedToFloat<uint32_t, double>(event.fabric_congestion_window,
                                                 falcon_rue::kFractionalBits);
  double old_nic_congestion_window = GetFloatNcwnd(event, state);
  uint32_t target_delay = GetTargetDelay(
      parameters_->topo_scaling_per_hop, parameters_->flow_scaling_alpha,
      parameters_->flow_scaling_beta, parameters_->max_flow_scaling,
      parameters_->fabric_base_delay, old_fabric_congestion_window,
      event.forward_hops);
  uint32_t smoothed_rtt = GetSmoothed(parameters_->rtt_smoothing_alpha,
                                      event.rtt_state, timing.rtt);
  uint32_t smoothed_delay = GetSmoothed(parameters_->delay_smoothing_alpha,
                                        event.delay_state, timing.delay);
  // Determines which RTT to use for IPG and window time_marker calculation.
  uint32_t calc_rtt = parameters_->calc_rtt_smooth ? smoothed_rtt : timing.rtt;

  // Module 1: Calculates fabric cwnd, fabric ipg, rto and fabric time marker.
  // Outputs:
  //  - new_fabric_congestion_window (fixed)
  //  - fabric_inter_packet_gap
  double new_fabric_congestion_window;
  uint32_t fabric_decrease_delta =
      falcon_rue::GetWindowDelta(now, event.fabric_window_time_marker);
  if (UseMaxFabricDecrease(event)) {
    new_fabric_congestion_window = ComputeNackFabricCongestionWindow(
        event, old_fabric_congestion_window, timing, fabric_decrease_delta);
  } else {
    // All ACKs and NACKs are treated as ACKs for the fabric congestion window
    // besides the kReserved nack code.
    new_fabric_congestion_window = ComputeAckFabricCongestionWindow(
        event, old_fabric_congestion_window, smoothed_delay, target_delay,
        timing, fabric_decrease_delta);
  }
  uint32_t fabric_window_time_marker = falcon_rue::GetFabricWindowTimeMarker(
      now, event.fabric_window_time_marker, calc_rtt,
      old_fabric_congestion_window, new_fabric_congestion_window,
      parameters_->min_fabric_congestion_window);
  uint32_t fabric_congestion_window_fixed =
      falcon_rue::FloatToFixed<double, uint32_t>(new_fabric_congestion_window,
                                                 falcon_rue::kFractionalBits);
  uint32_t fabric_inter_packet_gap =
      GetInterPacketGap(parameters_->ipg_time_scalar, parameters_->ipg_bits,
                        new_fabric_congestion_window, calc_rtt);

  // Module 2: Calculates NIC cwnd, NIC ipg, NIC time marker and direction.
  // Outputs:
  //  - new_nic_congestion_window (fixed)
  //  - nic_inter_packet_gap
  //  - nic_guard_info
  //  - retransmit_timeout
  double new_nic_congestion_window;
  uint32_t nic_change_delta =
      falcon_rue::GetWindowDelta(now, event.nic_window_time_marker);
  if (event.event_type == falcon::RueEventType::kNack &&
      event.nack_code == falcon::NackCode::kRxResourceExhaustion) {
    new_nic_congestion_window = ComputeNackNicCongestionWindow(
        event, old_nic_congestion_window, nic_change_delta, timing);
  } else {
    // All ACKs and NACKs are treated as ACKs for the NIC congestion window
    // besides the kResourceExhaustion nack code.
    new_nic_congestion_window = ComputeAckNicCongestionWindow(
        event, old_nic_congestion_window, nic_change_delta, timing);
  }
  uint32_t nic_congestion_window_fixed =
      ConvertFloatNcwndToFixed(new_nic_congestion_window);
  // Store ncwnd fractional part in ConnectionState if not included in response.
  StoreFloatNcwndFraction(state, new_nic_congestion_window);

  // Calculates the window time_marker and window direction
  falcon_rue::NicWindowGuardInfo nic_guard_info =
      GetNicWindowGuardInfo(event, now, calc_rtt, old_nic_congestion_window,
                            new_nic_congestion_window);
  uint32_t nic_inter_packet_gap =
      GetNicInterPacketGap(parameters_->ipg_time_scalar, parameters_->ipg_bits,
                           new_nic_congestion_window, calc_rtt);

  // As the effective inter_packet_gap, use the larger of the
  // fabric_inter_packet_gap and the nic_inter_packet_gap.
  uint32_t retransmit_timeout = GetRetransmitTimeout(
      parameters_->min_retransmit_timeout, smoothed_rtt,
      parameters_->retransmit_timeout_scalar,
      std::max(fabric_inter_packet_gap, nic_inter_packet_gap),
      parameters_->ipg_time_scalar);

  // Module 3: Calculates PLB state.
  // Outputs:
  //  - plb_state
  //  - reroute
  PlbState plb_state;
  if constexpr (std::is_same_v<EventT, falcon_rue::Event_GEN1>) {
    plb_state.value = GetPlbStateFromConnectionState(state);
  } else {
    plb_state.value = GetPlbStateFromEvent(event);
  }
  bool reroute =
      ComputePlb(event, smoothed_delay, target_delay, plb_state,
                 fmin(old_fabric_congestion_window, old_nic_congestion_window));

  // Module 4: Update multi-path flow labels and weights.
  // Outputs:
  uint32_t flow_label_1 = 0;
  uint32_t flow_label_2 = 0;
  uint32_t flow_label_3 = 0;
  uint32_t flow_label_4 = 0;
  bool flow_label_1_valid = false;
  bool flow_label_2_valid = false;
  bool flow_label_3_valid = false;
  bool flow_label_4_valid = false;
  uint8_t flow_label_1_weight = 0;
  uint8_t flow_label_2_weight = 0;
  uint8_t flow_label_3_weight = 0;
  uint8_t flow_label_4_weight = 0;
  bool wrr_restart_round = false;
  uint8_t cwnd_carryover = 0;
  uint8_t flow_id = GetFlowIdFromEvent(event);
  UpdateFlowLabels(event, reroute, flow_id, flow_label_1_valid,
                   flow_label_2_valid, flow_label_3_valid, flow_label_4_valid,
                   flow_label_1, flow_label_2, flow_label_3, flow_label_4);

  // Module 5: Updates CSIG variables.
  bool csig_enable = false;
  uint8_t csig_select = 0;
  ComputeCsigVariables(event, csig_enable, csig_select);

  // Module 6: Updates per-connection backpressure variables.
  uint8_t request_alpha = 0;
  uint8_t response_alpha = 0;
  ComputePerConnectionBackpressureVariables(event, timing, target_delay,
                                            request_alpha, response_alpha);

  // Module 7: Computes AR rate.
  uint8_t ar_rate = 0;
  ComputeArRate(event, new_fabric_congestion_window, new_nic_congestion_window,
                ar_rate);

  // Update any RUE-state if necessary and Response.
  if constexpr (std::is_same_v<EventT, falcon_rue::Event_GEN1>) {
    state->plb_state.value = plb_state.value;
  }
  SetResponse(
      /*connection_id=*/event.connection_id,
      /*randomize_path=*/reroute || ComputeRandomizePath(event),
      /*cc_metadata=*/event.cc_metadata,
      /*fabric_congestion_window=*/fabric_congestion_window_fixed,
      /*fabric_inter_packet_gap=*/fabric_inter_packet_gap,
      /*nic_congestion_window=*/nic_congestion_window_fixed,
      /*retransmit_timeout=*/retransmit_timeout,
      /*fabric_window_time_marker=*/fabric_window_time_marker,
      /*nic_window_time_marker=*/nic_guard_info.time_marker,
      /*nic_window_direction=*/nic_guard_info.direction,
      /*event_queue_select=*/event.event_queue_select,
      /*delay_select=*/event.delay_select,
      /*base_delay=*/event.base_delay,
      /*delay_state=*/smoothed_delay,
      /*rtt_state=*/smoothed_rtt,
      /*cc_opaque=*/event.cc_opaque,
      /*plb_state=*/plb_state.value,
      /*ar_rate=*/ar_rate,
      /*alpha_request=*/request_alpha,
      /*alpha_response=*/response_alpha,
      /*nic_inter_packet_gap=*/nic_inter_packet_gap,
      /*flow_label_1=*/flow_label_1,
      /*flow_label_2=*/flow_label_2,
      /*flow_label_3=*/flow_label_3,
      /*flow_label_4=*/flow_label_4,
      /*flow_label_1_valid=*/flow_label_1_valid,
      /*flow_label_2_valid=*/flow_label_2_valid,
      /*flow_label_3_valid=*/flow_label_3_valid,
      /*flow_label_4_valid=*/flow_label_4_valid,
      /*flow_label_1_weight=*/flow_label_1_weight,
      /*flow_label_2_weight=*/flow_label_2_weight,
      /*flow_label_3_weight=*/flow_label_3_weight,
      /*flow_label_4_weight=*/flow_label_4_weight,
      /*wrr_restart_round=*/wrr_restart_round,
      /*flow_id=*/flow_id,
      /*csig_enable=*/csig_enable,
      /*csig_select=*/csig_select,
      /*response=*/response);
}

template <typename EventT, typename ResponseT>
void Swift<EventT, ResponseT>::ProcessRetransmit(
    const EventT& event, ResponseT& response, uint32_t now,
    ConnectionState<EventT>* state) const {
  // Pulls the needed values from the event.
  uint32_t fabric_decrease_delta =
      falcon_rue::GetWindowDelta(now, event.fabric_window_time_marker);
  double fabric_congestion_window = falcon_rue::FixedToFloat<uint32_t, double>(
      event.fabric_congestion_window, falcon_rue::kFractionalBits);

  // Determines the new congestion window.
  double last_fabric_congestion_window = fabric_congestion_window;
  fabric_congestion_window = ComputeTimeoutFabricCongestionWindow(
      event, fabric_congestion_window, fabric_decrease_delta);

  // Calculates the IPG, RTO, and time_marker for the response
  uint32_t nic_inter_packet_gap = GetNipg(event, state);
  uint32_t fabric_inter_packet_gap =
      GetInterPacketGap(parameters_->ipg_time_scalar, parameters_->ipg_bits,
                        fabric_congestion_window, event.rtt_state);
  // As the effective inter_packet_gap, use the larger of the
  // fabric_inter_packet_gap and the nic_inter_packet_gap.
  uint32_t retransmit_timeout = GetRetransmitTimeout(
      parameters_->min_retransmit_timeout, event.rtt_state,
      parameters_->retransmit_timeout_scalar,
      std::max(fabric_inter_packet_gap, nic_inter_packet_gap),
      parameters_->ipg_time_scalar);
  uint32_t fabric_window_time_marker = falcon_rue::GetFabricWindowTimeMarker(
      now, event.fabric_window_time_marker, event.rtt_state,
      last_fabric_congestion_window, fabric_congestion_window,
      parameters_->min_fabric_congestion_window);
  uint32_t fabric_congestion_window_fixed =
      falcon_rue::FloatToFixed<double, uint32_t>(fabric_congestion_window,
                                                 falcon_rue::kFractionalBits);

  // Note: no nic congestion window alteration is performed
  falcon::WindowDirection direction = GetWindowDirectionFromEvent(event);

  // Compute ar_rate.
  double nic_congestion_window = GetFloatNcwnd(event, state);
  uint8_t ar_rate;
  ComputeArRate(event, fabric_congestion_window, nic_congestion_window,
                ar_rate);

  // Compute per-connection backpressure variables for Gen_2. Since we don't
  // have packet timing or rx_buffer_level information, we use a "safe" default
  // value in case of retransmits.
  uint8_t request_alpha = ConvertPerConnectionAlphaToShift(
      parameters_->backpressure_retransmit_alpha);
  uint8_t response_alpha = request_alpha;

  // PLB state is not changed for retransmit events. Therefore, no repath
  // decision will happen here, and the flow label valid bits are all unset.
  uint32_t flow_label_1 = 0;
  uint32_t flow_label_2 = 0;
  uint32_t flow_label_3 = 0;
  uint32_t flow_label_4 = 0;
  bool flow_label_1_valid = false;
  bool flow_label_2_valid = false;
  bool flow_label_3_valid = false;
  bool flow_label_4_valid = false;
  // Flow weights for single-path connections are not relevant and can all be 0.
  uint8_t flow_label_1_weight = 0;
  uint8_t flow_label_2_weight = 0;
  uint8_t flow_label_3_weight = 0;
  uint8_t flow_label_4_weight = 0;
  bool wrr_restart_round = false;
  uint8_t cwnd_carryover = 0;
  uint8_t flow_id = GetFlowIdFromEvent(event);

  PlbState plb_state;
  if constexpr (std::is_same_v<EventT, falcon_rue::Event_GEN1>) {
    plb_state.value = GetPlbStateFromConnectionState(state);
  } else {
    plb_state.value = GetPlbStateFromEvent(event);
  }
  //
  bool csig_enable = false;
  uint8_t csig_select = 0;
  ComputeCsigVariables(event, csig_enable, csig_select);

  // Update any RUE-state if necessary and Response.
  if constexpr (std::is_same_v<EventT, falcon_rue::Event_GEN1>) {
    state->plb_state.value = plb_state.value;
  }
  SetResponse(
      /*connection_id=*/event.connection_id,
      /*randomize_path=*/ComputeRandomizePath(event),
      /*cc_metadata=*/event.cc_metadata,
      /*fabric_congestion_window=*/fabric_congestion_window_fixed,
      /*fabric_inter_packet_gap=*/fabric_inter_packet_gap,
      /*nic_congestion_window=*/event.nic_congestion_window,
      /*retransmit_timeout=*/retransmit_timeout,
      /*fabric_window_time_marker=*/fabric_window_time_marker,
      /*nic_window_time_marker=*/event.nic_window_time_marker,
      /*nic_window_direction=*/direction,
      /*event_queue_select=*/event.event_queue_select,
      /*delay_select=*/event.delay_select,
      /*base_delay=*/event.base_delay,
      /*delay_state=*/event.delay_state,
      /*rtt_state=*/event.rtt_state,
      /*cc_opaque=*/event.cc_opaque,
      /*plb_state=*/plb_state.value,
      /*ar_rate=*/ar_rate,
      /*alpha_request=*/request_alpha,
      /*alpha_response=*/response_alpha,
      /*nic_inter_packet_gap=*/nic_inter_packet_gap,
      /*flow_label_1=*/flow_label_1,
      /*flow_label_2=*/flow_label_2,
      /*flow_label_3=*/flow_label_3,
      /*flow_label_4=*/flow_label_4,
      /*flow_label_1_valid=*/flow_label_1_valid,
      /*flow_label_2_valid=*/flow_label_2_valid,
      /*flow_label_3_valid=*/flow_label_3_valid,
      /*flow_label_4_valid=*/flow_label_4_valid,
      /*flow_label_1_weight=*/flow_label_1_weight,
      /*flow_label_2_weight=*/flow_label_2_weight,
      /*flow_label_3_weight=*/flow_label_3_weight,
      /*flow_label_4_weight=*/flow_label_4_weight,
      /*wrr_restart_round=*/wrr_restart_round,
      /*flow_id=*/flow_id,
      /*csig_enable=*/csig_enable,
      /*csig_select=*/csig_select,
      /*response=*/response);
}

template <typename EventT, typename ResponseT>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE double
Swift<EventT, ResponseT>::ComputeAckFabricCongestionWindow(
    const EventT& event, double fabric_congestion_window,
    uint32_t smoothed_delay, uint32_t target_delay,
    const falcon_rue::PacketTiming& timing,
    uint32_t fabric_decrease_delta) const {
  // Determines the new fabric congestion window. The fabric congestion window
  // is adjusted based on additive increment and multiplicative decrease (AIMD)
  // algorithm. The multiplicative decrease factor is scaled by the delta
  // between the actual and target delay. The additive increment factor is
  // proportional to the number of packets ack'd. The algorithm ensures
  // that there is at most one decrease per round-trip time. Increments may
  // occur on every ACK.

  if (smoothed_delay < target_delay) {
    // Increases the congestion window.
    double increase =
        parameters_->fabric_additive_increment_factor * event.num_packets_acked;
    if (fabric_congestion_window >= 1.0) {
      increase /= fabric_congestion_window;
    }
    fabric_congestion_window += increase;
  } else {
    if (fabric_decrease_delta >= timing.rtt) {
      // Decreases the congestion window.
      uint32_t delay_delta = smoothed_delay - target_delay;
      double decrease_scale = static_cast<double>(delay_delta) / smoothed_delay;
      double decrease_amount =
          decrease_scale * parameters_->fabric_multiplicative_decrease_factor;
      decrease_amount = std::min(
          parameters_->max_fabric_multiplicative_decrease, decrease_amount);
      double decrease_factor = 1.0 - decrease_amount;
      fabric_congestion_window *= decrease_factor;
    }
  }
  return std::clamp(fabric_congestion_window,
                    parameters_->min_fabric_congestion_window,
                    parameters_->max_fabric_congestion_window);
}

template <typename EventT, typename ResponseT>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE double
Swift<EventT, ResponseT>::ComputeNackFabricCongestionWindow(
    const EventT& event, double fabric_congestion_window,
    const falcon_rue::PacketTiming& timing,
    uint32_t fabric_decrease_delta) const {
  // Determines the new fabric congestion window. The fabric congestion window
  // is adjusted based on multiplicative decrease algorithm. The algorithm
  // ensures that there is at most decrease per round-trip time.
  if (fabric_decrease_delta >= timing.rtt) {
    double decrease_factor =
        1.0 - parameters_->max_fabric_multiplicative_decrease;
    fabric_congestion_window *= decrease_factor;
  }
  return std::clamp(fabric_congestion_window,
                    parameters_->min_fabric_congestion_window,
                    parameters_->max_fabric_congestion_window);
}

template <typename EventT, typename ResponseT>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE double
Swift<EventT, ResponseT>::ComputeTimeoutFabricCongestionWindow(
    const EventT& event, double fabric_congestion_window,
    uint32_t fabric_decrease_delta) const {
  // Determines the new congestion window. Decreases window by MDF on first
  // timeout and sets to parameters_->min_fabric_congestion_window on all
  // subsequent timeouts above the configured limit.
  if (fabric_decrease_delta >= event.rtt_state) {
    if (event.retransmit_count == 1) {
      double decrease_factor =
          1.0 - parameters_->max_fabric_multiplicative_decrease;
      fabric_congestion_window *= decrease_factor;
    } else if (event.retransmit_count >= parameters_->retransmit_limit) {
      fabric_congestion_window = parameters_->min_fabric_congestion_window;
    }
  }
  return std::clamp(fabric_congestion_window,
                    parameters_->min_fabric_congestion_window,
                    parameters_->max_fabric_congestion_window);
}

template <typename EventT, typename ResponseT>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE double
Swift<EventT, ResponseT>::ComputeAckNicCongestionWindow(
    const EventT& event, double nic_congestion_window,
    uint32_t nic_change_delta, const falcon_rue::PacketTiming& timing) const {
  // Determines the new nic congestion window. The NIC congestion window is
  // adjusted based on additive increment and multiplicative decrease (AIMD)
  // algorithm. The multiplicative decrease factor is scaled by the delta
  // between the actual and target buffer levels for the remote NIC. The
  // additive increment factor is fixed. The algorithm further ensures that
  // there is at most one increase and at most one decrease per round-trip time.
  uint32_t rx_buffer_level = event.rx_buffer_level;
  falcon::WindowDirection nic_window_direction = event.nic_window_direction;
  if (rx_buffer_level < parameters_->target_rx_buffer_level) {
    // Increases the ncwnd if the rx_buffer_level is below the target under the
    // conditions that the previous ncwnd change was a decrease or an RTT has
    // passed since last ncwnd change. The ideal behavior is documented in
    // which gives preference to ncwnd increases versus decreases without
    // changing the rue mailbox queue format.
    if ((nic_window_direction == falcon::WindowDirection::kDecrease) ||
        (nic_change_delta >= timing.rtt)) {
      nic_congestion_window += parameters_->nic_additive_increment_factor;
    }
  } else {
    // Decreases the nic congestion window if last window change was more than
    // one RTT ago.
    if (nic_change_delta >= timing.rtt) {
      uint32_t level_delta =
          rx_buffer_level - parameters_->target_rx_buffer_level;
      double decrease_scale =
          static_cast<double>(level_delta) / rx_buffer_level;
      double decrease_amount =
          decrease_scale * parameters_->nic_multiplicative_decrease_factor;
      decrease_amount = std::min(parameters_->max_nic_multiplicative_decrease,
                                 decrease_amount);
      double decrease_factor = 1.0 - decrease_amount;
      nic_congestion_window *= decrease_factor;
    }
  }
  return std::clamp(nic_congestion_window,
                    parameters_->min_nic_congestion_window,
                    parameters_->max_nic_congestion_window);
}

template <>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE double
Swift<falcon_rue::Event_Gen2, falcon_rue::Response_Gen2>::
    ComputeAckNicCongestionWindow(
        const falcon_rue::Event_Gen2& event, double nic_congestion_window,
        uint32_t nic_change_delta,
        const falcon_rue::PacketTiming& timing) const {
  // Determines the new nic congestion window. The NIC congestion
  // window is adjusted based on additive increment and multiplicative decrease
  // (AIMD) algorithm. The multiplicative decrease factor is scaled by the delta
  // between the actual and target buffer levels for the remote NIC. The
  // additive increment factor is fixed. The algorithm ensures that there is at
  // most one decrease per round-trip time. Increments may occur on every ACK.
  uint32_t rx_buffer_level = event.rx_buffer_level;
  if (rx_buffer_level < parameters_->target_rx_buffer_level) {
    // Increases the ncwnd if the rx_buffer_level is below the target.
    nic_congestion_window += parameters_->nic_additive_increment_factor;
  } else {
    // Decreases the nic congestion window if last window change was more than
    // one RTT ago.
    if (nic_change_delta >= timing.rtt) {
      uint32_t level_delta =
          rx_buffer_level - parameters_->target_rx_buffer_level;
      double decrease_scale =
          static_cast<double>(level_delta) / rx_buffer_level;
      double decrease_amount =
          decrease_scale * parameters_->nic_multiplicative_decrease_factor;
      decrease_amount = std::min(parameters_->max_nic_multiplicative_decrease,
                                 decrease_amount);
      double decrease_factor = 1.0 - decrease_amount;
      nic_congestion_window *= decrease_factor;
    }
  }
  return std::clamp(nic_congestion_window,
                    parameters_->min_nic_congestion_window,
                    parameters_->max_nic_congestion_window);
}

template <typename EventT, typename ResponseT>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE double
Swift<EventT, ResponseT>::ComputeNackNicCongestionWindow(
    const EventT& event, double nic_congestion_window,
    uint32_t nic_change_delta, const falcon_rue::PacketTiming& timing) const {
  // Decreases the nic congestion window if it previously was increased or the
  // last window decrease was more than one RTT ago. The NIC congestion window
  // is adjusted based using the maximum multiplicative decrease.
  falcon::WindowDirection nic_window_direction = event.nic_window_direction;
  if ((nic_window_direction == falcon::WindowDirection::kIncrease) ||
      (nic_change_delta >= timing.rtt)) {
    double decrease_factor = 1.0 - parameters_->max_nic_multiplicative_decrease;
    nic_congestion_window *= decrease_factor;
  }
  return std::clamp(nic_congestion_window,
                    parameters_->min_nic_congestion_window,
                    parameters_->max_nic_congestion_window);
}

template <>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE double
Swift<falcon_rue::Event_Gen2, falcon_rue::Response_Gen2>::
    ComputeNackNicCongestionWindow(
        const falcon_rue::Event_Gen2& event, double nic_congestion_window,
        uint32_t nic_change_delta,
        const falcon_rue::PacketTiming& timing) const {
  // Determines the new nic congestion window. The nic congestion window
  // is adjusted using the maximum multiplicative decrease. The algorithm
  // ensures that there is at most decrease per round-trip time.
  if (nic_change_delta >= timing.rtt) {
    double decrease_factor = 1.0 - parameters_->max_nic_multiplicative_decrease;
    nic_congestion_window *= decrease_factor;
  }
  return std::clamp(nic_congestion_window,
                    parameters_->min_nic_congestion_window,
                    parameters_->max_nic_congestion_window);
}

template <typename EventT, typename ResponseT>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE bool Swift<EventT, ResponseT>::ComputePlb(
    const EventT& event, uint32_t smoothed_delay, uint32_t target_delay,
    PlbState& plb_state, double old_window_size) {
  // Do not do PLB on NACK.
  if (event.nack_code != falcon::NackCode::kNotANack) return false;

  bool reroute = false;
  if (parameters_->randomize_path) {
    uint32_t new_packets_acknowledged =
        plb_state.packets_acknowledged + event.num_packets_acked;
    uint32_t new_packets_congestion_acknowledged =
        plb_state.packets_congestion_acknowledged;
    if (smoothed_delay > target_delay * parameters_->plb_target_rtt_multiplier)
      new_packets_congestion_acknowledged += event.num_packets_acked;
    if (new_packets_acknowledged >= old_window_size) {
      double congested_frac = new_packets_congestion_acknowledged /
                              static_cast<double>(new_packets_acknowledged);
      if (congested_frac < parameters_->plb_congestion_threshold) {
        plb_state.plb_reroute_attempted = 0;
      } else {
        plb_state.plb_reroute_attempted++;
        if (plb_state.plb_reroute_attempted >=
            parameters_->plb_attempt_threshold) {
          reroute = true;
          plb_state.plb_reroute_attempted = 0;
        }
      }
      plb_state.packets_congestion_acknowledged = 0;
      plb_state.packets_acknowledged = 0;
    } else {
      plb_state.packets_congestion_acknowledged =
          new_packets_congestion_acknowledged;
      plb_state.packets_acknowledged = new_packets_acknowledged;
    }
  }
  return reroute;
}

template <typename EventT, typename ResponseT>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE bool
Swift<EventT, ResponseT>::ComputeRandomizePath(const EventT& event) const {
  if (parameters_->randomize_path) {
    return (event.cc_opaque & kRandomizePathEnableMask) > 0;
  } else {
    return false;
  }
}

template <>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE bool
Swift<falcon_rue::Event_Gen2, falcon_rue::Response_Gen2>::ComputeRandomizePath(
    const falcon_rue::Event_Gen2& event) const {
  // Unlike Gen_1, Gen_2 does not support forcing a randomize_path by setting a
  // bit in cc_opaque.
  return false;
}

template <typename EventT, typename ResponseT>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE uint32_t
Swift<EventT, ResponseT>::MakeBaseDelayField(uint8_t profile_index,
                                             uint32_t state_table_index) {
  // Encodes the profile_index and state_table_index in 24 bit base_delay field.
  // base_delay[23:21] = profile_index (3 bits).
  // base_delay[21:0] = state_table_index (21 bits).

  // Assumes profile_index and state_table_index fit in 3, 21 bits respectively.
  return (profile_index << kSwiftProfileIndexShift) | state_table_index;
}

template <typename EventT, typename ResponseT>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE int
Swift<EventT, ResponseT>::GetProfileIndex(uint32_t event_base_delay) {
  return event_base_delay >> kSwiftProfileIndexShift;
}

template <typename EventT, typename ResponseT>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE uint32_t
Swift<EventT, ResponseT>::GetStateTableIndex(uint32_t event_base_delay) {
  return event_base_delay & 0x001FFFFF;  // Last 21 bits.
}

template <typename EventT, typename ResponseT>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE uint32_t
Swift<EventT, ResponseT>::GetTargetDelay(
    uint16_t topo_scaling_per_hop, double flow_scaling_alpha,
    double flow_scaling_beta, uint32_t max_flow_scaling, uint32_t base_delay,
    double congestion_window, uint8_t hops) {
  // Target delay combines base delay, flow scaling, and topology scaling.
  uint32_t target_delay = base_delay;
  uint32_t flow_scaling =
      flow_scaling_alpha / std::sqrt(congestion_window) + flow_scaling_beta;

  flow_scaling = std::clamp(flow_scaling, 0u, max_flow_scaling);
  target_delay += flow_scaling;
  target_delay += topo_scaling_per_hop * hops;
  return falcon_rue::SaturateHigh(falcon_rue::kTimeBits, target_delay);
}

template <typename EventT, typename ResponseT>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE uint32_t
Swift<EventT, ResponseT>::GetRetransmitTimeout(uint32_t min_retransmit_timeout,
                                               uint32_t rtt, double rtt_scalar,
                                               uint32_t inter_packet_gap,
                                               double ipg_time_scalar) {
  DCHECK_GT(ipg_time_scalar, 0);
  uint32_t rto = static_cast<uint32_t>(rtt * rtt_scalar);
  // RTO should start after pacing finishes.
  rto += inter_packet_gap / ipg_time_scalar;
  rto = falcon_rue::SaturateHigh(falcon_rue::kTimeBits, rto);
  return std::max(rto, min_retransmit_timeout);
}

template <typename EventT, typename ResponseT>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE uint32_t
Swift<EventT, ResponseT>::GetInterPacketGap(double ipg_time_scalar,
                                            uint8_t ipg_bits,
                                            double congestion_window,
                                            uint32_t rtt) {
  // IPG is always 0 when congestion_window >= 1.0. Otherwise, the
  // inter-packet gap is calculated as rtt / congestion_window.
  static_assert((falcon_rue::kTimeBits + falcon_rue::kFractionalBits) < 64);
  static_assert(falcon_rue::kInterPacketGapBits < 32);

  // Pacing is not employed when the congestion window is >= 1.0.
  CHECK_GT(congestion_window, 0.0);  // Crash OK
  if (congestion_window >= 1.0) {
    return 0;
  }

  // Computes the IPG in FALCON unit time.
  double ipg_fp = rtt / congestion_window;

  // The IPG returned to FALCON is used to compute the Timing Wheel (TW) exit
  // timestamp as (now + IPG), effectively asking the TW to hold the packet for
  // IPG. This is because FALCON doesn't store the last_packet_send timestamp
  // for each connection. The ideal exit timestamp is (last_packet_send_time +
  // IPG). If we return the actual IPG, we'll send a packet every (IPG + rtt)
  // time interval instead of IPG, therefore we must subtract rtt from ipg_fp.
  ipg_fp = ipg_fp - rtt;

  // Converts IPG to TS unit time
  ipg_fp *= ipg_time_scalar;

  // Converts IPG to unsigned and saturate at ipg_bits
  uint64_t ipg = static_cast<uint64_t>(std::round(ipg_fp));
  return falcon_rue::SaturateHigh(ipg_bits, ipg);
}

template <typename EventT, typename ResponseT>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE bool
Swift<EventT, ResponseT>::UseMaxFabricDecrease(const EventT& event) const {
  return parameters_->max_decrease_on_eack_nack_drop &&
         event.event_type == falcon::RueEventType::kNack &&
         event.nack_code == falcon::NackCode::kReserved;
}

// Specialization for GEN1 and later generations which add EACK feature.
template <>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE bool
Swift<falcon_rue::Event_GEN1, falcon_rue::Response_GEN1>::UseMaxFabricDecrease(
    const falcon_rue::Event_GEN1& event) const {
  return (parameters_->max_decrease_on_eack_nack_drop &&
          ((event.event_type == falcon::RueEventType::kNack &&
            event.nack_code == falcon::NackCode::kReserved) ||
           (event.event_type == falcon::RueEventType::kAck && event.eack &&
            event.eack_drop)));
}

// Specialization for Gen_2 which adds EACK feature.
template <>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE bool
Swift<falcon_rue::Event_Gen2, falcon_rue::Response_Gen2>::UseMaxFabricDecrease(
    const falcon_rue::Event_Gen2& event) const {
  return (parameters_->max_decrease_on_eack_nack_drop &&
          ((event.event_type == falcon::RueEventType::kNack &&
            event.nack_code == falcon::NackCode::kReserved) ||
           (event.event_type == falcon::RueEventType::kAck && event.eack &&
            event.eack_drop)));
}

template <>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE falcon::WindowDirection
Swift<falcon_rue::Event_Gen2, falcon_rue::Response_Gen2>::
    GetWindowDirectionFromEvent(const falcon_rue::Event_Gen2& event) const {
  return falcon::WindowDirection::kIncrease;
}

template <typename EventT, typename ResponseT>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE falcon_rue::NicWindowGuardInfo
Swift<EventT, ResponseT>::GetNicWindowGuardInfo(
    const EventT& event, uint32_t now, uint32_t calc_rtt,
    double old_nic_congestion_window, double new_nic_congestion_window) const {
  return falcon_rue::GetNicWindowGuardInfo(
      now, event.nic_window_time_marker, calc_rtt, event.nic_window_direction,
      old_nic_congestion_window, new_nic_congestion_window,
      parameters_->min_nic_congestion_window,
      parameters_->max_nic_congestion_window);
}

template <>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE falcon_rue::NicWindowGuardInfo
Swift<falcon_rue::Event_Gen2, falcon_rue::Response_Gen2>::GetNicWindowGuardInfo(
    const falcon_rue::Event_Gen2& event, uint32_t now, uint32_t calc_rtt,
    double old_nic_congestion_window, double new_nic_congestion_window) const {
  falcon_rue::NicWindowGuardInfo info;
  // In Gen_2, the NIC window direction is not used anymore.
  info.direction = falcon::WindowDirection::kDecrease;
  info.time_marker = falcon_rue::GetFabricWindowTimeMarker(
      now, event.nic_window_time_marker, calc_rtt, old_nic_congestion_window,
      new_nic_congestion_window, parameters_->min_nic_congestion_window);
  return info;
}

// Template specialization to invoke multi-pathing code in Gen_2.
template <>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE void
Swift<falcon_rue::Event_Gen2, falcon_rue::Response_Gen2>::UpdateFlowLabels(
    const falcon_rue::Event_Gen2& event, bool reroute, uint8_t flow_id,
    // Outputs passed by reference.
    bool& flow_label_1_valid, bool& flow_label_2_valid,
    bool& flow_label_3_valid, bool& flow_label_4_valid, uint32_t& flow_label_1,
    uint32_t& flow_label_2, uint32_t& flow_label_3, uint32_t& flow_label_4) {
  if (reroute) {
    uint32_t new_flow_label = RandomFlowLabel(flow_id);
    switch (flow_id) {
      case 0:
        flow_label_1_valid = true;
        flow_label_1 = new_flow_label;
        break;
      case 1:
        flow_label_2_valid = true;
        flow_label_2 = new_flow_label;
        break;
      case 2:
        flow_label_3_valid = true;
        flow_label_3 = new_flow_label;
        break;
      case 3:
        flow_label_4_valid = true;
        flow_label_4 = new_flow_label;
        break;
      default:
        break;
    }
  }
}

// Template specialization to invoke CSIG code in Gen_2.
template <>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE void
Swift<falcon_rue::Event_Gen2, falcon_rue::Response_Gen2>::ComputeCsigVariables(
    const falcon_rue::Event_Gen2& event, bool& csig_enable,
    uint8_t& csig_select) const {
  //
  csig_enable = event.csig_enable;
  csig_select = 0;
}

// Template specialization to invoke per-connection backpressure management.
template <>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE void
Swift<falcon_rue::Event_Gen2, falcon_rue::Response_Gen2>::
    ComputePerConnectionBackpressureVariables(
        const falcon_rue::Event_Gen2& event,
        const falcon_rue::PacketTiming& packet_timing, uint32_t target_delay,
        uint8_t& alpha_request, uint8_t& alpha_response) const {
  // Calculate rtt_beta as target_delay / current_fabric_delay.
  double rtt_beta =
      static_cast<double>(target_delay) / std::max({packet_timing.delay, 1U});
  // Calculate buffer_level_beta as target_buffer_level / current_buffer_level.
  double current_buffer_level =
      event.rx_buffer_level > 0 ? event.rx_buffer_level : 1;
  double buffer_level_beta =
      parameters_->target_rx_buffer_level / current_buffer_level;

  // For requests, the alpha considers both rtt and buffer_level.
  alpha_request = ConvertPerConnectionAlphaToShift(
      parameters_->backpressure_alpha *
      std::min<double>({1, rtt_beta, buffer_level_beta}));

  // For responses, the alpha considers only rtt.
  alpha_response = ConvertPerConnectionAlphaToShift(
      parameters_->backpressure_alpha * std::min<double>({1, rtt_beta}));
}

// Template specialization to calculate per-connection ar_rate.
template <>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE void
Swift<falcon_rue::Event_Gen2, falcon_rue::Response_Gen2>::ComputeArRate(
    const falcon_rue::Event_Gen2& event, double fcwnd, double ncwnd,
    uint8_t& ar_rate) const {
  // In Gen_1 the AR threshold was only applied to fcwnd, whereas in Gen_2 it
  // will be applied to both fcwnd and ncwnd.
  double effective_cwnd = std::min(fcwnd, ncwnd);
  if (effective_cwnd <= kMaxArCwndThreshold) {
    ar_rate = 0;  // 100%
  } else {
    //
    // of keeping it flat at ~10%.
    ar_rate = 4;  // 11.76% (1 every 8.5 packets)
  }
}

template <typename EventT, typename ResponseT>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE absl::Status
Swift<EventT, ResponseT>::ValidateDelayStateConfig(
    const SwiftConfiguration& config) {
  if (config.randomize_path() && config.delay_smoothing_alpha() > 0) {
    // Delay smoothing state is reused for PLB, so delay smoothing and PLB
    // cannot both be enabled.
    return absl::InvalidArgumentError(" Cannot smooth delay while PLB is on");
  }
  return absl::OkStatus();
}

template <>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE absl::Status
Swift<falcon_rue::Event_Gen2, falcon_rue::Response_Gen2>::
    ValidateDelayStateConfig(const SwiftConfiguration& config) {
  return absl::OkStatus();
}

template <typename EventT, typename ResponseT>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE absl::Status
Swift<EventT, ResponseT>::ValidateNicCongestionWindowBounds(
    const SwiftConfiguration& config) {
  if ((config.max_nic_congestion_window() < kMinGen1NicCongestionWindow) ||
      (config.max_nic_congestion_window() > kMaxGen1NicCongestionWindow)) {
    return absl::InvalidArgumentError(
        "max_nic_congestion_window out of bounds");
  }
  if ((config.min_nic_congestion_window() < kMinGen1NicCongestionWindow) ||
      (config.min_nic_congestion_window() > kMaxGen1NicCongestionWindow)) {
    return absl::InvalidArgumentError(
        "min_nic_congestion_window out of bounds");
  }
  return absl::OkStatus();
}

template <>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE absl::Status
Swift<falcon_rue::Event_Gen2, falcon_rue::Response_Gen2>::
    ValidateNicCongestionWindowBounds(const SwiftConfiguration& config) {
  if ((config.max_nic_congestion_window() < kMinGen2NicCongestionWindow) ||
      (config.max_nic_congestion_window() > kMaxGen2NicCongestionWindow)) {
    return absl::InvalidArgumentError(
        "max_nic_congestion_window out of bounds");
  }
  if ((config.min_nic_congestion_window() < kMinGen2NicCongestionWindow) ||
      (config.min_nic_congestion_window() > kMaxGen2NicCongestionWindow)) {
    return absl::InvalidArgumentError(
        "min_nic_congestion_window out of bounds");
  }
  return absl::OkStatus();
}

template <typename EventT, typename ResponseT>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE uint8_t
Swift<EventT, ResponseT>::GetFlowIdFromEvent(const EventT& event) {
  // Flow ID is a multipathing concept that does not apply to Gen_1.
  return 0;
}

template <>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE uint8_t
Swift<falcon_rue::Event_Gen2, falcon_rue::Response_Gen2>::GetFlowIdFromEvent(
    const falcon_rue::Event_Gen2& event) {
  // Flow ID is the last 2 bits of the flow label if multipathing is enabled for
  // the connection.
  return event.multipath_enable ? event.flow_label & kFlowIdMask : 0;
}

template <typename EventT, typename ResponseT>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE uint32_t
Swift<EventT, ResponseT>::RandomFlowLabel(uint8_t flow_id) {
  //
  // SW-RUE for Swift-Gen_2.
  auto random_flow_label = flow_label_generator_->GetFlowLabel();
  // Concatenate the flow ID as the last 2 bits of the kFlowLabelBits-long flow
  // label.
  return (random_flow_label << falcon_rue::kFlowIdBits) | flow_id;
}

template <typename EventT, typename ResponseT>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE uint8_t
Swift<EventT, ResponseT>::ConvertPerConnectionAlphaToShift(double alpha) const {
  if (alpha >= kPerConnectionBackpressureMaxAlpha) {
    return 0;
  }
  if (alpha <= kPerConnectionBackpressureMinAlpha) {
    return (1 << falcon_rue::kPerConnectionBackpressureAlphaBits) - 1;
  }
  //
  // versus a lookup table approach.
  int alpha_shift = ceil(log2(alpha));
  return kPerConnectionBackpressureAlphaShift - alpha_shift;
}

// Extracts the PLB state stored in the event.
template <typename EventT, typename ResponseT>
template <typename EventU>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE
    std::enable_if_t<!std::is_same_v<EventU, falcon_rue::Event_GEN1>, uint32_t>
    Swift<EventT, ResponseT>::GetPlbStateFromEvent(const EventT& event) {
  // In Gen_2, PLB state is stored in its separate plb_state field in the event.
  return event.plb_state;
}

template <typename EventT, typename ResponseT>
template <typename EventU>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE
    std::enable_if_t<std::is_same_v<EventU, falcon_rue::Event_GEN1>, uint32_t>
    Swift<EventT, ResponseT>::GetPlbStateFromConnectionState(
        ConnectionState<EventT>* state) {
  // Return PLB state from RUE's connection state.
  return state->plb_state.value;
}

template <typename EventT, typename ResponseT>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE double
Swift<EventT, ResponseT>::GetFloatNcwnd(
    const EventT& event, const ConnectionState<EventT>* state) const {
  double fractional_ncwnd = falcon_rue::FixedToFloat<uint16_t, double>(
      state->ncwnd_fraction, falcon_rue::kFractionalBits);
  return event.nic_congestion_window + fractional_ncwnd;
}

template <>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE double
Swift<falcon_rue::Event_Gen2, falcon_rue::Response_Gen2>::GetFloatNcwnd(
    const falcon_rue::Event_Gen2& event,
    const ABSL_ATTRIBUTE_UNUSED ConnectionState<falcon_rue::Event_Gen2>* state)
    const {
  return falcon_rue::FixedToFloat<uint32_t, double>(
      event.nic_congestion_window, falcon_rue::kFractionalBits);
}

template <typename EventT, typename ResponseT>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE uint32_t
Swift<EventT, ResponseT>::ConvertFloatNcwndToFixed(
    double nic_congestion_window) const {
  // Gen_1 event does not include NCWND fractional bits, so they are stored in
  // the connection state. The integer part is returned here for the response.
  return std::floor(nic_congestion_window);
}

template <>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE uint32_t
Swift<falcon_rue::Event_Gen2, falcon_rue::Response_Gen2>::
    ConvertFloatNcwndToFixed(double nic_congestion_window) const {
  return falcon_rue::FloatToFixed<double, uint32_t>(
      nic_congestion_window, falcon_rue::kFractionalBits);
}

template <typename EventT, typename ResponseT>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE uint32_t
Swift<EventT, ResponseT>::GetNicInterPacketGap(double ipg_time_scalar,
                                               uint8_t ipg_bits,
                                               double congestion_window,
                                               uint32_t rtt) {
  return GetInterPacketGap(ipg_time_scalar, ipg_bits, congestion_window, rtt);
  return 0;
}

template <typename EventT, typename ResponseT>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE uint32_t Swift<EventT, ResponseT>::GetNipg(
    const EventT& event, const ConnectionState<EventT>* state) const {
  double nic_congestion_window = std::clamp(
      GetFloatNcwnd(event, state), parameters_->min_nic_congestion_window,
      parameters_->max_nic_congestion_window);
  uint32_t nic_inter_packet_gap =
      GetNicInterPacketGap(parameters_->ipg_time_scalar, parameters_->ipg_bits,
                           nic_congestion_window, event.rtt_state);
  return nic_inter_packet_gap;
}

template <typename EventT, typename ResponseT>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE void
Swift<EventT, ResponseT>::StoreFloatNcwndFraction(
    ConnectionState<EventT>* state, double nic_congestion_window) {
  // Gen_1 event does not include NCWND fractional bits, so they are stored in
  // the connection state. This is a no-op for other variants.
  if constexpr (std::is_same_v<EventT, falcon_rue::Event_GEN1>) {
    double fractional_part =
        nic_congestion_window - std::floor(nic_congestion_window);
    state->ncwnd_fraction = falcon_rue::FloatToFixed<double, uint16_t>(
        fractional_part, falcon_rue::kFractionalBits);
  }
}

// Computes the flow weight given the flow fcwnd in float format, and the
// connection fcwnd in float format.
template <typename EventT, typename ResponseT>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE uint8_t
Swift<EventT, ResponseT>::ComputeFlowWeight(double flow_fcwnd,
                                            double connection_fcwnd) {
  // Scale the ratio of the flow fcwnd and the connection fcwnd by the maximum
  // weight value.
  double scaled_ratio = kMaxFlowWeight * flow_fcwnd / connection_fcwnd;
  uint32_t scaled_ratio_saturated =
      falcon_rue::SaturateHigh(falcon_rue::kFlowLabelWeightBits,
                               static_cast<uint32_t>(std::round(scaled_ratio)));
  return std::clamp<uint8_t>(scaled_ratio_saturated, kMinFlowWeight,
                             kMaxFlowWeight);
}

// Sets the response for Gen_1.
template <typename EventT, typename ResponseT>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE void Swift<EventT, ResponseT>::SetResponse(
    uint32_t connection_id, bool randomize_path, uint32_t cc_metadata,
    uint32_t fabric_congestion_window, uint32_t fabric_inter_packet_gap,
    uint32_t nic_congestion_window, uint32_t retransmit_timeout,
    uint32_t fabric_window_time_marker, uint32_t nic_window_time_marker,
    falcon::WindowDirection nic_window_direction, uint8_t event_queue_select,
    falcon::DelaySelect delay_select, uint32_t base_delay, uint32_t delay_state,
    uint32_t rtt_state, uint32_t cc_opaque, uint32_t plb_state, uint8_t ar_rate,
    uint8_t alpha_request, uint8_t alpha_response,
    uint32_t nic_inter_packet_gap, uint32_t flow_label_1, uint32_t flow_label_2,
    uint32_t flow_label_3, uint32_t flow_label_4, bool flow_label_1_valid,
    bool flow_label_2_valid, bool flow_label_3_valid, bool flow_label_4_valid,
    uint8_t flow_label_1_weight, uint8_t flow_label_2_weight,
    uint8_t flow_label_3_weight, uint8_t flow_label_4_weight,
    bool wrr_restart_round, uint8_t flow_id, bool csig_enable,
    uint8_t csig_select, ResponseT& response) const {
  // For Gen_1, response IPG is the maximum of fabric and NIC IPG, since there
  // is only one response field.
  uint32_t max_inter_packet_gap =
      std::max(fabric_inter_packet_gap, nic_inter_packet_gap);
  falcon_rue::SetResponse(
      connection_id, randomize_path, cc_metadata, fabric_congestion_window,
      max_inter_packet_gap, nic_congestion_window, retransmit_timeout,
      fabric_window_time_marker, nic_window_time_marker, nic_window_direction,
      event_queue_select, delay_select, base_delay, delay_state, rtt_state,
      cc_opaque, response);
}

// Sets the response for Gen_2.
template <>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE void
Swift<falcon_rue::Event_Gen2, falcon_rue::Response_Gen2>::SetResponse(
    uint32_t connection_id, bool randomize_path, uint32_t cc_metadata,
    uint32_t fabric_congestion_window, uint32_t fabric_inter_packet_gap,
    uint32_t nic_congestion_window, uint32_t retransmit_timeout,
    uint32_t fabric_window_time_marker, uint32_t nic_window_time_marker,
    falcon::WindowDirection nic_window_direction, uint8_t event_queue_select,
    falcon::DelaySelect delay_select, uint32_t base_delay, uint32_t delay_state,
    uint32_t rtt_state, uint32_t cc_opaque, uint32_t plb_state, uint8_t ar_rate,
    uint8_t alpha_request, uint8_t alpha_response,
    uint32_t nic_inter_packet_gap, uint32_t flow_label_1, uint32_t flow_label_2,
    uint32_t flow_label_3, uint32_t flow_label_4, bool flow_label_1_valid,
    bool flow_label_2_valid, bool flow_label_3_valid, bool flow_label_4_valid,
    uint8_t flow_label_1_weight, uint8_t flow_label_2_weight,
    uint8_t flow_label_3_weight, uint8_t flow_label_4_weight,
    bool wrr_restart_round, uint8_t flow_id, bool csig_enable,
    uint8_t csig_select, falcon_rue::Response_Gen2& response) const {
  falcon_rue::SetResponse(
      connection_id, cc_metadata, fabric_congestion_window,
      fabric_inter_packet_gap, nic_congestion_window, retransmit_timeout,
      fabric_window_time_marker, nic_window_time_marker, event_queue_select,
      delay_select, base_delay, delay_state, rtt_state, cc_opaque, plb_state,
      alpha_request, alpha_response, nic_inter_packet_gap, flow_label_1,
      flow_label_2, flow_label_3, flow_label_4, flow_label_1_valid,
      flow_label_2_valid, flow_label_3_valid, flow_label_4_valid,
      flow_label_1_weight, flow_label_2_weight, flow_label_3_weight,
      flow_label_4_weight, wrr_restart_round, flow_id, csig_enable, csig_select,
      ar_rate, response);
}

// Processes an ACK/NACK event for a multipath-enabled connection and generates
// the response.
template <>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE void
Swift<falcon_rue::Event_Gen2, falcon_rue::Response_Gen2>::
    ProcessAckNackMultipath(const falcon_rue::Event_Gen2& event,
                            falcon_rue::Response_Gen2& response,
                            ConnectionState<falcon_rue::Event_Gen2>& state,
                            uint32_t now) {
  uint8_t flow_id = GetFlowIdFromEvent(event);

  // Gather any required state from the event or the RUE state.
  uint32_t old_rtt_state, old_fabric_window_time_marker_flow;
  PlbState plb_state;
  if (flow_id == 0) {
    old_rtt_state = event.rtt_state;
    old_fabric_window_time_marker_flow = event.fabric_window_time_marker;
    plb_state.value = GetPlbStateFromEvent(event);
  } else {
    old_rtt_state = state.GetFlowState(flow_id).rtt_state;
    old_fabric_window_time_marker_flow =
        state.GetFlowState(flow_id).fcwnd_time_marker;
    plb_state.value = state.GetFlowState(flow_id).plb_state;
  }

  //
  // events.
  falcon_rue::PacketTiming timing = falcon_rue::GetPacketTiming(event);
  uint32_t new_rtt_state =
      GetSmoothed(parameters_->rtt_smoothing_alpha, old_rtt_state, timing.rtt);
  // Determines which RTT to use for window time_marker calculation. Unlike
  // stateless Gen_2 connections, calc_rtt_smooth does not affect what RTT is
  // used for IPG which will always be the maximum smoothed RTT across all
  // flows.
  uint32_t calc_rtt = parameters_->calc_rtt_smooth ? new_rtt_state : timing.rtt;
  // Currently, we assume delay smoothing will not be enabled so we do not keep
  // the smoothed_delay state for each flow. ConnectionState needs to add that
  // state for delay (like it does with rtt) to support delay smoothing for
  // multipath connections.
  uint32_t flow_delay = timing.delay;

  // We use the maximum smoothed RTT across all flows for calculating fipg,
  // nipg, and rto because they are applied at the connection level and not at
  // the flow level.
  //
  std::array<uint32_t, 4> rtt_states = {
      event.rtt_state, state.GetFlowState(1).rtt_state,
      state.GetFlowState(2).rtt_state, state.GetFlowState(3).rtt_state};
  // Use the new rtt_state value for the current flow ID.
  rtt_states[flow_id] = new_rtt_state;
  uint32_t max_rtt_state =
      *std::max_element(rtt_states.begin(), rtt_states.end());

  // Update fcwnd: to do that, first get the old flow fcwnd, the old
  // connection fcwnd, and the target delay.
  double old_fabric_congestion_window_flow =
      falcon_rue::FixedToFloat<uint32_t, double>(state.fcwnd[flow_id],
                                                 falcon_rue::kFractionalBits);
  uint32_t target_delay = GetTargetDelay(
      parameters_->topo_scaling_per_hop, parameters_->flow_scaling_alpha,
      parameters_->flow_scaling_beta, parameters_->max_flow_scaling,
      parameters_->fabric_base_delay, old_fabric_congestion_window_flow,
      event.forward_hops);
  // Get the new flow fcwnd based on whether the event requires a maximum fcwnd
  // decrease or not.
  double new_fabric_congestion_window_flow;
  uint32_t fabric_decrease_delta =
      falcon_rue::GetWindowDelta(now, old_fabric_window_time_marker_flow);
  if (UseMaxFabricDecrease(event)) {
    // With the max_decrease_on_eack_nack_drop flag set, and under some
    // conditions (e.g., EACK drop or NACK RX window drop), fcwnd is directly
    // reduced by the maximum MD factor.
    new_fabric_congestion_window_flow = ComputeNackFabricCongestionWindow(
        event, old_fabric_congestion_window_flow, timing,
        fabric_decrease_delta);
  } else {
    // All ACKs and NACKs are treated as ACKs for the fabric congestion window
    // besides the kReserved nack code.
    new_fabric_congestion_window_flow = ComputeAckFabricCongestionWindow(
        event, old_fabric_congestion_window_flow, flow_delay, target_delay,
        timing, fabric_decrease_delta);
  }
  // The fcwnd of all the flows are stored in RUE state.
  state.fcwnd[flow_id] = falcon_rue::FloatToFixed<double, uint32_t>(
      new_fabric_congestion_window_flow, falcon_rue::kFractionalBits);
  // Get the new fcwnd time marker value.
  uint32_t new_fabric_window_time_marker_flow =
      falcon_rue::GetFabricWindowTimeMarker(
          now, old_fabric_window_time_marker_flow, calc_rtt,
          old_fabric_congestion_window_flow, new_fabric_congestion_window_flow,
          parameters_->min_fabric_congestion_window);
  // The new_fabric_congestion_window_connection is the new sum of all flow
  // fcwnds.
  double new_fabric_congestion_window_connection =
      falcon_rue::FixedToFloat<uint32_t, double>(
          state.fcwnd[0] + state.fcwnd[1] + state.fcwnd[2] + state.fcwnd[3],
          falcon_rue::kFractionalBits);
  new_fabric_congestion_window_connection =
      std::clamp(new_fabric_congestion_window_connection,
                 parameters_->min_fabric_congestion_window,
                 parameters_->max_fabric_congestion_window);
  // The connection's fcwnd in fixed format will be returned in the response.
  uint32_t new_fabric_congestion_window_connection_fixed =
      falcon_rue::FloatToFixed<double, uint32_t>(
          new_fabric_congestion_window_connection, falcon_rue::kFractionalBits);

  // Get fipg for the connection. Use max_rtt_state as the RTT value for
  // calculation.
  uint32_t fabric_inter_packet_gap =
      GetInterPacketGap(parameters_->ipg_time_scalar, parameters_->ipg_bits,
                        new_fabric_congestion_window_connection, max_rtt_state);

  // Update ncwnd.
  double old_nic_congestion_window = GetFloatNcwnd(event, &state);
  uint32_t nic_change_delta =
      falcon_rue::GetWindowDelta(now, event.nic_window_time_marker);
  double new_nic_congestion_window;
  if (event.event_type == falcon::RueEventType::kNack &&
      event.nack_code == falcon::NackCode::kRxResourceExhaustion) {
    new_nic_congestion_window = ComputeNackNicCongestionWindow(
        event, old_nic_congestion_window, nic_change_delta, timing);
  } else {
    // All ACKs and NACKs are treated as ACKs for the NIC congestion window
    // besides the kResourceExhaustion nack code.
    new_nic_congestion_window = ComputeAckNicCongestionWindow(
        event, old_nic_congestion_window, nic_change_delta, timing);
  }
  // The connection's ncwnd in fixed format will be returned in the response.
  uint32_t new_nic_congestion_window_fixed =
      ConvertFloatNcwndToFixed(new_nic_congestion_window);
  // Store ncwnd fractional part in ConnectionState if not included in response.
  StoreFloatNcwndFraction(&state, new_nic_congestion_window);

  // Calculates the window time_marker and window direction
  falcon_rue::NicWindowGuardInfo nic_guard_info =
      GetNicWindowGuardInfo(event, now, calc_rtt, old_nic_congestion_window,
                            new_nic_congestion_window);

  // Get nipg for the connection. Use max_rtt_state as the RTT value for
  // calculation.
  uint32_t nic_inter_packet_gap =
      GetNicInterPacketGap(parameters_->ipg_time_scalar, parameters_->ipg_bits,
                           new_nic_congestion_window, max_rtt_state);

  // Calculate rto. Use max_rtt_state as the RTT value for calculation.
  uint32_t retransmit_timeout = GetRetransmitTimeout(
      parameters_->min_retransmit_timeout, max_rtt_state,
      parameters_->retransmit_timeout_scalar,
      std::max(fabric_inter_packet_gap, nic_inter_packet_gap),
      parameters_->ipg_time_scalar);

  // Update PLB state.
  bool reroute = ComputePlb(
      event, flow_delay, target_delay, plb_state,
      fmin(old_fabric_congestion_window_flow, old_nic_congestion_window));
  bool flow_label_1_valid = false;
  bool flow_label_2_valid = false;
  bool flow_label_3_valid = false;
  bool flow_label_4_valid = false;
  uint32_t flow_label_1 = kDefaultFlowLabel1;
  uint32_t flow_label_2 = kDefaultFlowLabel2;
  uint32_t flow_label_3 = kDefaultFlowLabel3;
  uint32_t flow_label_4 = kDefaultFlowLabel4;
  UpdateFlowLabels(event, reroute, flow_id, flow_label_1_valid,
                   flow_label_2_valid, flow_label_3_valid, flow_label_4_valid,
                   flow_label_1, flow_label_2, flow_label_3, flow_label_4);

  uint8_t ar_rate = 0;
  ComputeArRate(event, new_fabric_congestion_window_connection,
                new_nic_congestion_window, ar_rate);

  // Compute per-connection backpressure variables for Gen_2.
  uint8_t alpha_request = 0;
  uint8_t alpha_response = 0;
  ComputePerConnectionBackpressureVariables(event, timing, target_delay,
                                            alpha_request, alpha_response);

  //
  bool csig_enable = false;
  uint8_t csig_select = 0;
  ComputeCsigVariables(event, csig_enable, csig_select);

  // Write back any state to the response or to the RUE state.
  uint32_t rtt_state_in_response, new_fabric_window_time_marker_in_response,
      plb_state_in_response;
  if (flow_id == 0) {
    rtt_state_in_response = new_rtt_state;
    new_fabric_window_time_marker_in_response =
        new_fabric_window_time_marker_flow;
    plb_state_in_response = plb_state.value;
  } else {
    rtt_state_in_response = event.rtt_state;
    state.GetFlowState(flow_id).rtt_state = new_rtt_state;
    new_fabric_window_time_marker_in_response = event.fabric_window_time_marker;
    state.GetFlowState(flow_id).fcwnd_time_marker =
        new_fabric_window_time_marker_flow;
    plb_state_in_response = event.plb_state;
    state.GetFlowState(flow_id).plb_state = plb_state.value;
  }

  // Update flow weights and wrr_restart_round.
  uint8_t flow_label_1_weight =
      ComputeFlowWeight(falcon_rue::FixedToFloat<uint32_t, double>(
                            state.fcwnd[0], falcon_rue::kFractionalBits),
                        new_fabric_congestion_window_connection);
  uint8_t flow_label_2_weight =
      ComputeFlowWeight(falcon_rue::FixedToFloat<uint32_t, double>(
                            state.fcwnd[1], falcon_rue::kFractionalBits),
                        new_fabric_congestion_window_connection);
  uint8_t flow_label_3_weight =
      ComputeFlowWeight(falcon_rue::FixedToFloat<uint32_t, double>(
                            state.fcwnd[2], falcon_rue::kFractionalBits),
                        new_fabric_congestion_window_connection);
  uint8_t flow_label_4_weight =
      ComputeFlowWeight(falcon_rue::FixedToFloat<uint32_t, double>(
                            state.fcwnd[3], falcon_rue::kFractionalBits),
                        new_fabric_congestion_window_connection);
  // Fow now, keep wrr_restart_round always false.
  bool wrr_restart_round = false;

  SetResponse(
      /*connection_id=*/event.connection_id,
      /*randomize_path=*/false,           // Gen_1-specific field
      /*cc_metadata=*/event.cc_metadata,  // cc_metadata reflected as is from
                                          // event
      /*fabric_congestion_window=*/
      new_fabric_congestion_window_connection_fixed,
      /*fabric_inter_packet_gap=*/fabric_inter_packet_gap,
      /*nic_congestion_window=*/new_nic_congestion_window_fixed,
      /*retransmit_timeout=*/retransmit_timeout,
      /*fabric_window_time_marker=*/new_fabric_window_time_marker_in_response,
      /*nic_window_time_marker=*/nic_guard_info.time_marker,
      /*nic_window_direction=*/nic_guard_info.direction,  // Gen_1-specific
                                                          // field
      /*event_queue_select=*/event.event_queue_select,    // event_queue_select
                                                        // reflected as is from
                                                        // event
      /*delay_select=*/event.delay_select,  // delay_select reflected as is
                                            // from event
      /*base_delay=*/event.base_delay,      // base_delay reflected as is
                                            // from event
      /*delay_state=*/0,  // delay_state is currently not supported for
                          // multipath connections
      /*rtt_state=*/rtt_state_in_response,
      /*cc_opaque=*/event.cc_opaque,  // cc_opaque reflected as is
                                      // from event
      /*plb_state=*/plb_state_in_response,
      /*ar_rate=*/ar_rate,
      /*alpha_request=*/alpha_request,
      /*alpha_response=*/alpha_response,
      /*nic_inter_packet_gap=*/nic_inter_packet_gap,
      /*flow_label_1=*/flow_label_1,
      /*flow_label_2=*/flow_label_2,
      /*flow_label_3=*/flow_label_3,
      /*flow_label_4=*/flow_label_4,
      /*flow_label_1_valid=*/flow_label_1_valid,
      /*flow_label_2_valid=*/flow_label_2_valid,
      /*flow_label_3_valid=*/flow_label_3_valid,
      /*flow_label_4_valid=*/flow_label_4_valid,
      /*flow_label_1_weight=*/flow_label_1_weight,
      /*flow_label_2_weight=*/flow_label_2_weight,
      /*flow_label_3_weight=*/flow_label_3_weight,
      /*flow_label_4_weight=*/flow_label_4_weight,
      /*wrr_restart_round=*/wrr_restart_round,
      /*flow_id=*/flow_id,
      /*csig_enable=*/csig_enable,
      /*csig_select=*/csig_select,
      /*response=*/response);
}

// Processes a Retransmit event for a multipath-enabled connection and generates
// the response.
template <>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE void
Swift<falcon_rue::Event_Gen2, falcon_rue::Response_Gen2>::
    ProcessRetransmitMultipath(const falcon_rue::Event_Gen2& event,
                               falcon_rue::Response_Gen2& response,
                               ConnectionState<falcon_rue::Event_Gen2>& state,
                               uint32_t now) const {
  uint8_t flow_id = GetFlowIdFromEvent(event);

  //
  // events.
  // Update fcwnd for multipath retransmit events.
  // Gather old fcwnd-related state. For Flow ID 0, the rtt_state and
  // fabric_window_time_marker_flow are stored in the event itself.
  uint32_t rtt_state, old_fabric_window_time_marker_flow;
  if (flow_id == 0) {
    rtt_state = event.rtt_state;
    old_fabric_window_time_marker_flow = event.fabric_window_time_marker;
  } else {
    rtt_state = state.GetFlowState(flow_id).rtt_state;
    old_fabric_window_time_marker_flow =
        state.GetFlowState(flow_id).fcwnd_time_marker;
  }
  double old_fabric_congestion_window_flow =
      falcon_rue::FixedToFloat<uint32_t, double>(state.fcwnd[flow_id],
                                                 falcon_rue::kFractionalBits);
  // Calculate updates to fcwnd-related state.
  uint32_t fabric_decrease_delta =
      falcon_rue::GetWindowDelta(now, old_fabric_window_time_marker_flow);
  double new_fabric_congestion_window_flow =
      ComputeTimeoutFabricCongestionWindow(
          event, old_fabric_congestion_window_flow, fabric_decrease_delta);
  // The fcwnd of all the flows are stored in RUE state.
  state.fcwnd[flow_id] = falcon_rue::FloatToFixed<double, uint32_t>(
      new_fabric_congestion_window_flow, falcon_rue::kFractionalBits);
  // The connection's fcwnd is the sum of the new fcwnd values of all the flows.
  double new_fabric_congestion_window_connection =
      falcon_rue::FixedToFloat<uint32_t, double>(
          state.fcwnd[0] + state.fcwnd[1] + state.fcwnd[2] + state.fcwnd[3],
          falcon_rue::kFractionalBits);
  new_fabric_congestion_window_connection =
      std::clamp(new_fabric_congestion_window_connection,
                 parameters_->min_fabric_congestion_window,
                 parameters_->max_fabric_congestion_window);
  // The connection's fcwnd in fixed format will be returned in the response.
  uint32_t new_fabric_congestion_window_connection_fixed =
      falcon_rue::FloatToFixed<double, uint32_t>(
          new_fabric_congestion_window_connection, falcon_rue::kFractionalBits);
  // Get the new fcwnd time marker value.
  uint32_t new_fabric_window_time_marker_flow =
      falcon_rue::GetFabricWindowTimeMarker(
          now, old_fabric_window_time_marker_flow, rtt_state,
          old_fabric_congestion_window_flow, new_fabric_congestion_window_flow,
          parameters_->min_fabric_congestion_window);
  uint32_t new_fabric_window_time_marker_in_response =
      event.fabric_window_time_marker;
  // Only return flow 0's fcwnd time marker in the response.

  if (flow_id == 0) {
    new_fabric_window_time_marker_in_response =
        new_fabric_window_time_marker_flow;
  } else {
    // Write any new state back to RUE's state. For flow ID 0, do not store the
    // new fcwnd guard in RUE state, but in the response instead.
    state.GetFlowState(flow_id).fcwnd_time_marker =
        new_fabric_window_time_marker_flow;
  }

  // Update flow weights and wrr_restart_round.
  uint8_t flow_label_1_weight =
      ComputeFlowWeight(falcon_rue::FixedToFloat<uint32_t, double>(
                            state.fcwnd[0], falcon_rue::kFractionalBits),
                        new_fabric_congestion_window_connection);
  uint8_t flow_label_2_weight =
      ComputeFlowWeight(falcon_rue::FixedToFloat<uint32_t, double>(
                            state.fcwnd[1], falcon_rue::kFractionalBits),
                        new_fabric_congestion_window_connection);
  uint8_t flow_label_3_weight =
      ComputeFlowWeight(falcon_rue::FixedToFloat<uint32_t, double>(
                            state.fcwnd[2], falcon_rue::kFractionalBits),
                        new_fabric_congestion_window_connection);
  uint8_t flow_label_4_weight =
      ComputeFlowWeight(falcon_rue::FixedToFloat<uint32_t, double>(
                            state.fcwnd[3], falcon_rue::kFractionalBits),
                        new_fabric_congestion_window_connection);
  // Fow now, keep wrr_restart_round always false.
  bool wrr_restart_round = false;

  // We use the maximum smoothed RTT across all flows for calculating fipg,
  // nipg, and rto because they are applied at the connection level and not at
  // the flow level.
  //
  uint32_t max_rtt_state = std::max<uint32_t>(
      {event.rtt_state, state.GetFlowState(1).rtt_state,
       state.GetFlowState(2).rtt_state, state.GetFlowState(3).rtt_state});
  uint32_t fabric_inter_packet_gap =
      GetInterPacketGap(parameters_->ipg_time_scalar, parameters_->ipg_bits,
                        new_fabric_congestion_window_connection, max_rtt_state);

  // To calculate rto for multipath retransmit events, first get the value of
  // nipg from the event and the flow's smoothed rtt.
  double ncwnd_float = std::clamp(GetFloatNcwnd(event, &state),
                                  parameters_->min_nic_congestion_window,
                                  parameters_->max_nic_congestion_window);
  uint32_t nic_inter_packet_gap =
      GetNicInterPacketGap(parameters_->ipg_time_scalar, parameters_->ipg_bits,
                           ncwnd_float, max_rtt_state);
  uint32_t retransmit_timeout = GetRetransmitTimeout(
      parameters_->min_retransmit_timeout, max_rtt_state,
      parameters_->retransmit_timeout_scalar,
      std::max(fabric_inter_packet_gap, nic_inter_packet_gap),
      parameters_->ipg_time_scalar);

  // PLB state is not changed for retransmit events. Therefore, no repath
  // decision will happen here, and the flow label valid bits are all unset, and
  // the flow labels are all set to their default values.
  bool flow_label_1_valid = false;
  bool flow_label_2_valid = false;
  bool flow_label_3_valid = false;
  bool flow_label_4_valid = false;
  uint32_t flow_label_1 = kDefaultFlowLabel1;
  uint32_t flow_label_2 = kDefaultFlowLabel2;
  uint32_t flow_label_3 = kDefaultFlowLabel3;
  uint32_t flow_label_4 = kDefaultFlowLabel4;

  uint8_t ar_rate = 0;
  ComputeArRate(event, new_fabric_congestion_window_connection, ncwnd_float,
                ar_rate);

  // Compute per-connection backpressure variables for Gen_2. Since we don't
  // have packet timing or rx_buffer_level information, we use a "safe" default
  // value in case of retransmits.
  uint8_t alpha_request = ConvertPerConnectionAlphaToShift(
      parameters_->backpressure_retransmit_alpha);
  uint8_t alpha_response = alpha_request;

  //
  bool csig_enable = false;
  uint8_t csig_select = 0;
  ComputeCsigVariables(event, csig_enable, csig_select);

  SetResponse(
      /*connection_id=*/event.connection_id,
      /*randomize_path=*/false,           // Gen_1-specific field
      /*cc_metadata=*/event.cc_metadata,  // cc_metadata reflected as is from
                                          // event
      /*fabric_congestion_window=*/
      new_fabric_congestion_window_connection_fixed,
      /*fabric_inter_packet_gap=*/fabric_inter_packet_gap,
      /*nic_congestion_window=*/event.nic_congestion_window,  // ncwnd reflected
                                                              // as is from
                                                              // event
      /*retransmit_timeout=*/retransmit_timeout,
      /*fabric_window_time_marker=*/new_fabric_window_time_marker_in_response,
      /*nic_window_time_marker=*/
      event.nic_window_time_marker,  // ncwnd time marker reflected as is from
                                     // event
                                     /*nic_window_direction=*/
      falcon::WindowDirection::kDecrease,               // Gen_1-specific
                                                        // field
      /*event_queue_select=*/event.event_queue_select,  // event_queue_select
                                                        // reflected as is from
                                                        // event
      /*delay_select=*/event.delay_select,  // delay_select reflected as is
                                            // from event
      /*base_delay=*/event.base_delay,      // base_delay reflected as is
                                            // from event
      /*delay_state=*/event.delay_state,    // delay_state reflected as is
                                            // from event
      /*rtt_state=*/event.rtt_state,        // rtt_state reflected as is
                                            // from event
      /*cc_opaque=*/event.cc_opaque,        // cc_opaque reflected as is
                                            // from event
      /*plb_state=*/GetPlbStateFromEvent(event),  // plb_state reflected as is
                                                  // from event
      /*ar_rate=*/ar_rate,
      /*alpha_request=*/alpha_request,
      /*alpha_response=*/alpha_response,
      /*nic_inter_packet_gap=*/nic_inter_packet_gap,
      /*flow_label_1=*/flow_label_1,
      /*flow_label_2=*/flow_label_2,
      /*flow_label_3=*/flow_label_3,
      /*flow_label_4=*/flow_label_4,
      /*flow_label_1_valid=*/flow_label_1_valid,
      /*flow_label_2_valid=*/flow_label_2_valid,
      /*flow_label_3_valid=*/flow_label_3_valid,
      /*flow_label_4_valid=*/flow_label_4_valid,
      /*flow_label_1_weight=*/flow_label_1_weight,
      /*flow_label_2_weight=*/flow_label_2_weight,
      /*flow_label_3_weight=*/flow_label_3_weight,
      /*flow_label_4_weight=*/flow_label_4_weight,
      /*wrr_restart_round=*/wrr_restart_round,
      /*flow_id=*/flow_id,
      /*csig_enable=*/csig_enable,
      /*csig_select=*/csig_select,
      /*response=*/response);
}

// Processes an event for a multipath-enabled connection and generates the
// response.
//
template <>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE void
Swift<falcon_rue::Event_Gen2, falcon_rue::Response_Gen2>::ProcessMultipath(
    const falcon_rue::Event_Gen2& event, falcon_rue::Response_Gen2& response,
    ConnectionState<falcon_rue::Event_Gen2>& state, uint32_t now) {
  PickProfile(event);
  DCHECK(parameters_ != nullptr);

  switch (event.event_type) {
    case (falcon::RueEventType::kAck):
    case (falcon::RueEventType::kNack):
      ProcessAckNackMultipath(event, response, state, now);
      break;
    case (falcon::RueEventType::kRetransmit):
      ProcessRetransmitMultipath(event, response, state, now);
      break;
  }
}

typedef Swift<falcon_rue::Event, falcon_rue::Response> SwiftGen1;
typedef Swift<falcon_rue::Event_Gen2, falcon_rue::Response_Gen2> SwiftGen2;

}  // namespace rue
}  // namespace isekai

#endif  // ISEKAI_HOST_FALCON_RUE_ALGORITHM_SWIFT_H_
