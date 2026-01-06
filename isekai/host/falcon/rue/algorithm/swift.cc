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

#include "isekai/host/falcon/rue/algorithm/swift.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_format.h"
#include "absl/strings/substitute.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "isekai/common/status_util.h"
#include "isekai/host/falcon/rue/algorithm/algorithm.pb.h"
#include "isekai/host/falcon/rue/bits.h"
#include "isekai/host/falcon/rue/format_gen1.h"
#include "isekai/host/falcon/rue/format_gen2.h"

namespace isekai {
namespace rue {

template <typename EventT, typename ResponseT>
absl::StatusOr<SwiftParameters>
Swift<EventT, ResponseT>::ConvertProfileToParameters(
    const SwiftConfiguration& config) {
  if ((config.max_fabric_congestion_window() < kMinFabricCongestionWindow) ||
      (config.max_fabric_congestion_window() > kMaxFabricCongestionWindow)) {
    return absl::InvalidArgumentError(
        "max_fabric_congestion_window out of bounds");
  }
  if ((config.min_fabric_congestion_window() < kMinFabricCongestionWindow) ||
      (config.min_fabric_congestion_window() > kMaxFabricCongestionWindow)) {
    return absl::InvalidArgumentError(
        "min_fabric_congestion_window out of bounds");
  }
  if (config.min_fabric_congestion_window() >
      config.max_fabric_congestion_window()) {
    return absl::InvalidArgumentError(
        "fabric_congestion_window min/max reversed");
  }
  if (config.fabric_additive_increment_factor() <= 0.0) {
    return absl::InvalidArgumentError(
        "fabric_additive_increment_factor must be > 0");
  }
  if ((config.fabric_multiplicative_decrease_factor() <= 0.0) ||
      (config.fabric_multiplicative_decrease_factor() >= 1.0)) {
    return absl::InvalidArgumentError(
        "fabric_multiplication_decrease_factor must be > 0 and < 1");
  }
  if ((config.max_fabric_multiplicative_decrease() <= 0.0) ||
      (config.max_fabric_multiplicative_decrease() >= 1.0)) {
    return absl::InvalidArgumentError(
        "max_fabric_multiplication_decrease must be > 0 and < 1");
  }
  absl::Status status = ValidateNicCongestionWindowBounds(config);
  if (!status.ok()) return status;
  if (config.min_nic_congestion_window() > config.max_nic_congestion_window()) {
    return absl::InvalidArgumentError("nic_congestion_window min/max reversed");
  }
  if (config.nic_additive_increment_factor() < 1.0) {
    return absl::InvalidArgumentError(
        "nic_additive_increment_factor must be >= 1");
  }
  //
  if ((config.nic_multiplicative_decrease_factor() <= 0.0) ||
      (config.nic_multiplicative_decrease_factor() >= 1.0)) {
    return absl::InvalidArgumentError(
        "nic_multiplication_decrease_factor must be > 0 and < 1");
  }
  if ((config.max_nic_multiplicative_decrease() <= 0.0) ||
      (config.max_nic_multiplicative_decrease() >= 1.0)) {
    return absl::InvalidArgumentError(
        "max_nic_multiplication_decrease must be > 0 and < 1");
  }
  if ((config.target_rx_buffer_level() < kMinRxBufferLevel) ||
      (config.target_rx_buffer_level() > kMaxRxBufferLevel)) {
    return absl::InvalidArgumentError("target_rx_buffer_level out of bounds");
  }
  if ((config.rtt_smoothing_alpha() < 0.0) ||
      (config.rtt_smoothing_alpha() >= 1.0)) {
    return absl::InvalidArgumentError(
        "rtt_smoothing_alpha must be >= 0 and < 1");
  }
  if ((config.delay_smoothing_alpha() < 0.0) ||
      (config.delay_smoothing_alpha() >= 1.0)) {
    return absl::InvalidArgumentError(
        "delay_smoothing_alpha must be >= 0 and < 1");
  }
  if ((config.topo_scaling_per_hop() == 0) ||
      (config.topo_scaling_per_hop() > kMaxTopoScalingPerHop)) {
    return absl::InvalidArgumentError("topo_scaling_per_hop out of bounds");
  }
  if (config.min_retransmit_timeout() == 0) {
    return absl::InvalidArgumentError("min_retransmit_timeout must be > 0");
  }
  if (config.retransmit_timeout_scalar() < 1.0) {
    return absl::InvalidArgumentError(
        "retransmit_timeout_scalar must be >= 1.0");
  }
  if ((config.retransmit_limit() == 0) ||
      (config.retransmit_limit() > UINT8_MAX)) {
    return absl::InvalidArgumentError("retransmit_limit must be > 0 and < 2^8");
  }
  ASSIGN_OR_RETURN(double flow_scaling_alpha,
                   CalculateFlowScalingAlpha(config.max_flow_scaling(),
                                             config.min_flow_scaling_window(),
                                             config.max_flow_scaling_window()));
  ASSIGN_OR_RETURN(
      double flow_scaling_beta,
      CalculateFlowScalingBeta(flow_scaling_alpha,
                               config.max_fabric_congestion_window()));
  if (config.ipg_time_scalar() <= 0.0) {
    return absl::InvalidArgumentError("ipg_time_scalar must be > 0");
  }
  if ((config.ipg_bits() == 0) ||
      (config.ipg_bits() > falcon_rue::kInterPacketGapBits)) {
    return absl::InvalidArgumentError("ipg_bits out of bounds");
  }

  if (config.randomize_path() &&
      (config.max_fabric_congestion_window() > kMaxPlbAckCount + 1 ||
       config.max_nic_congestion_window() > kMaxPlbAckCount + 1)) {
    return absl::InvalidArgumentError(
        "max fcwnd/ncwnd size must be <= kMaxPlbAckCount + 1");
  }
  if (config.plb_attempt_threshold() > kMaxPlbAttemptCount) {
    return absl::InvalidArgumentError(
        "plb_attempt_threshold should be <= kMaxPlbAttemptCount");
  }

  status = ValidateDelayStateConfig(config);
  if (!status.ok()) return status;

  if (!config.has_fabric_base_delay() || config.fabric_base_delay() == 0) {
    return absl::InvalidArgumentError("fabric_base_delay must be > 0");
  }

  if (config.has_backpressure_alpha()) {
    if (config.backpressure_alpha() < kPerConnectionBackpressureMinAlpha ||
        config.backpressure_alpha() > kPerConnectionBackpressureMaxAlpha) {
      return absl::InvalidArgumentError(
          absl::Substitute("backpressure_alpha must be >= $0 and <= $1",
                           kPerConnectionBackpressureMinAlpha,
                           kPerConnectionBackpressureMaxAlpha));
    }
  }

  if (config.has_backpressure_retransmit_alpha()) {
    if (config.backpressure_retransmit_alpha() <
            kPerConnectionBackpressureMinAlpha ||
        config.backpressure_retransmit_alpha() >
            kPerConnectionBackpressureMaxAlpha) {
      return absl::InvalidArgumentError(
          absl::Substitute("backpressure_retransmit_alpha must be >= $0 and <= "
                           "$1",
                           kPerConnectionBackpressureMinAlpha,
                           kPerConnectionBackpressureMaxAlpha));
    }
  }

  SwiftParameters parameters;
  // Constructs an object, sets its configuration
  parameters.max_fabric_congestion_window =
      config.max_fabric_congestion_window();
  parameters.min_fabric_congestion_window =
      config.min_fabric_congestion_window();
  parameters.fabric_additive_increment_factor =
      config.fabric_additive_increment_factor();
  parameters.fabric_multiplicative_decrease_factor =
      config.fabric_multiplicative_decrease_factor();
  parameters.max_fabric_multiplicative_decrease =
      config.max_fabric_multiplicative_decrease();
  parameters.max_decrease_on_eack_nack_drop =
      config.max_decrease_on_eack_nack_drop();
  parameters.max_nic_congestion_window = config.max_nic_congestion_window();
  parameters.min_nic_congestion_window = config.min_nic_congestion_window();
  parameters.nic_additive_increment_factor =
      config.nic_additive_increment_factor();
  parameters.nic_multiplicative_decrease_factor =
      config.nic_multiplicative_decrease_factor();
  parameters.max_nic_multiplicative_decrease =
      config.max_nic_multiplicative_decrease();
  parameters.target_rx_buffer_level = config.target_rx_buffer_level();
  parameters.rtt_smoothing_alpha = config.rtt_smoothing_alpha();
  parameters.delay_smoothing_alpha = config.delay_smoothing_alpha();
  parameters.topo_scaling_per_hop = config.topo_scaling_per_hop();
  parameters.min_retransmit_timeout = config.min_retransmit_timeout();
  parameters.retransmit_timeout_scalar = config.retransmit_timeout_scalar();
  parameters.retransmit_limit = config.retransmit_limit();
  parameters.flow_scaling_alpha = flow_scaling_alpha;
  parameters.flow_scaling_beta = flow_scaling_beta;
  parameters.max_flow_scaling = config.max_flow_scaling();
  parameters.calc_rtt_smooth = config.calc_rtt_smooth();
  parameters.ipg_time_scalar = config.ipg_time_scalar();
  parameters.ipg_bits = config.ipg_bits();
  parameters.randomize_path = config.randomize_path();
  parameters.plb_target_rtt_multiplier = config.plb_target_rtt_multiplier();
  parameters.plb_congestion_threshold = config.plb_congestion_threshold();
  parameters.plb_attempt_threshold = config.plb_attempt_threshold();
  parameters.fabric_base_delay = config.fabric_base_delay();
  parameters.backpressure_alpha = config.backpressure_alpha();
  parameters.backpressure_retransmit_alpha =
      config.backpressure_retransmit_alpha();

  if (config.has_flow_label_rng_seed()) {
    parameters.flow_label_rng_seed = config.flow_label_rng_seed();
  } else {
    parameters.flow_label_rng_seed = absl::ToUnixNanos(absl::Now());
  }
  return parameters;
}

template <typename EventT, typename ResponseT>
Swift<EventT, ResponseT>::Swift(SwiftParameters default_parameters) {
  for (auto& profile : profiles_) {
    profile.valid = false;
  }
  profiles_[kSwiftDefaultProfileIndex] = default_parameters;
  profiles_[kSwiftDefaultProfileIndex].valid = true;
  parameters_ = &profiles_[kSwiftDefaultProfileIndex];
  uint32_t seed = parameters_->flow_label_rng_seed;
  flow_label_generator_ = std::make_unique<FlowLabelGenerator>(seed);
}

template <typename EventT, typename ResponseT>
absl::StatusOr<double> Swift<EventT, ResponseT>::CalculateFlowScalingAlpha(
    int max_flow_scaling, double min_flow_scaling_window,
    double max_flow_scaling_window) {
  if (min_flow_scaling_window <= 0) {
    return absl::InvalidArgumentError("min_flow_scaling_window must be > 0");
  }
  if (max_flow_scaling_window <= 0) {
    return absl::InvalidArgumentError("max_flow_scaling_window must be > 0");
  }
  if (min_flow_scaling_window >= max_flow_scaling_window) {
    return absl::InvalidArgumentError(
        "min_flow_scaling_window must be < max_flow_scaling_window");
  }
  if (max_flow_scaling < 0) {
    return absl::InvalidArgumentError("max_flow_scaling must be >= 0");
  }

  return max_flow_scaling / (1 / std::sqrt(min_flow_scaling_window) -
                             1 / std::sqrt(max_flow_scaling_window));
}

template <typename EventT, typename ResponseT>
absl::StatusOr<double> Swift<EventT, ResponseT>::CalculateFlowScalingBeta(
    double flow_scaling_alpha, double max_flow_scaling_window) {
  if (flow_scaling_alpha < 0) {
    return absl::InvalidArgumentError("flow_scaling_alpha must be >= 0");
  }
  if (max_flow_scaling_window <= 0) {
    return absl::InvalidArgumentError("max_flow_scaling_window must be > 0");
  }

  return -flow_scaling_alpha / std::sqrt(max_flow_scaling_window);
}

template <typename EventT, typename ResponseT>
SwiftConfiguration Swift<EventT, ResponseT>::DefaultConfiguration() {
  // This configuration assumes a FALCON unit time resolution of
  // 0.131072us (shift picoseconds by 17 bits) and a traffic shaper (TS)
  // resolution of 512ns (shift nanoseconds by 9 bits).

  // Many of the default values comes from Swift in Pony
  //

  constexpr double kFalconUnitTimeUs = 0.131072;
  constexpr double kTimingWheelUnitTimeUs = 0.512;
  constexpr int kTimingWheelMaxDelayUs = 512000;

  SwiftConfiguration configuration;
  configuration.set_max_fabric_congestion_window(256.0);

  // min_fcwnd is used after multiple RTO.
  // Assuming that the last observed RTT when this happens is 500us, to get
  // ~50ms ipg, min_fcwnd should be 0.01.
  configuration.set_min_fabric_congestion_window(0.01);
  configuration.set_fabric_additive_increment_factor(1.0);
  configuration.set_fabric_multiplicative_decrease_factor(0.8);
  configuration.set_max_fabric_multiplicative_decrease(0.5);
  configuration.set_max_decrease_on_eack_nack_drop(false);
  configuration.set_max_nic_congestion_window(256.0);
  configuration.set_min_nic_congestion_window(1.0);
  configuration.set_nic_additive_increment_factor(1.0);
  configuration.set_nic_multiplicative_decrease_factor(0.8);
  configuration.set_max_nic_multiplicative_decrease(0.5);
  configuration.set_target_rx_buffer_level(15);  // in the middle
  configuration.set_rtt_smoothing_alpha(0.75);
  configuration.set_delay_smoothing_alpha(0.0);  // no delay smoothing
  configuration.set_topo_scaling_per_hop(
      std::round(1 / kFalconUnitTimeUs));  // 1us
  configuration.set_min_retransmit_timeout(
      std::round(250 / kFalconUnitTimeUs));  // ~250us
  configuration.set_retransmit_timeout_scalar(2.0);
  configuration.set_retransmit_limit(3);
  configuration.set_min_flow_scaling_window(0.1);
  configuration.set_max_flow_scaling_window(
      256.0);  // Same as max_fabric_congestion_window
  configuration.set_max_flow_scaling(
      std::round(100 / kFalconUnitTimeUs));  // 100us
  configuration.set_calc_rtt_smooth(false);
  configuration.set_ipg_time_scalar(kFalconUnitTimeUs / kTimingWheelUnitTimeUs);
  // TimingWheel min rate is 800Kbps, at 5K MTU, this is close to 50ms.
  // FALCON also caps ipg to ts_time_ipg_overflow_thld
  int ipg_bits = std::min<int>(
      16,  // 2**16*512ns ~ 50ms
      std::floor(std::log2(kTimingWheelMaxDelayUs / kTimingWheelUnitTimeUs)));
  configuration.set_ipg_bits(
      std::min<int>(falcon_rue::kInterPacketGapBits, ipg_bits));
  configuration.set_randomize_path(false);
  configuration.set_plb_target_rtt_multiplier(1.01);
  configuration.set_plb_congestion_threshold(0.5);
  configuration.set_plb_attempt_threshold(5);
  configuration.set_fabric_base_delay(191);  // 25us
  // We use a default base alpha of 1/8 that guarantees a connection can reach
  // max line rate bandwidth.
  configuration.set_backpressure_alpha(0.125);
  // For retransmit events, timing and buffer level information is not
  // available, hence we assign a "safe" default to request/response alpha.
  // Since a retransmit indicates packets are not being sent as expected, we use
  // a lower alpha (1/16) than base alpha.
  configuration.set_backpressure_retransmit_alpha(0.0625);
  return configuration;
}

template <typename EventT, typename ResponseT>
absl::Status Swift<EventT, ResponseT>::InstallAlgorithmProfile(
    int profile_index, AlgorithmConfiguration profile) {
  if (profile_index < 0 || profile_index >= profiles_.size()) {
    return absl::InvalidArgumentError("Invalid profile index.");
  }
  if (profiles_[profile_index].valid) {
    return absl::AlreadyExistsError(absl::StrFormat(
        "Profile with index %d already installed.", profile_index));
  }
  if (!profile.has_swift()) {
    return absl::InvalidArgumentError("Not a Swift profile.");
  }
  ASSIGN_OR_RETURN(profiles_[profile_index],
                   ConvertProfileToParameters(profile.swift()));
  profiles_[profile_index].valid = true;
  return absl::OkStatus();
}

template <typename EventT, typename ResponseT>
absl::Status Swift<EventT, ResponseT>::UninstallAlgorithmProfile(
    int profile_index) {
  if (profile_index < 0 || profile_index >= profiles_.size()) {
    return absl::InvalidArgumentError("Invalid profile index.");
  }
  if (profile_index == kSwiftDefaultProfileIndex) {
    return absl::InvalidArgumentError("Cannot remove default profile.");
  }
  if (!profiles_[profile_index].valid) {
    return absl::NotFoundError(
        absl::StrFormat("Profile not found at %d.", profile_index));
  }
  profiles_[profile_index].valid = false;
  return absl::OkStatus();
}

// Explicit template instantiations because some of the Swift class functions
// are not defined in the header file.
template class Swift<falcon_rue::Event_Gen1, falcon_rue::Response_Gen1>;
template class Swift<falcon_rue::Event_Gen2, falcon_rue::Response_Gen2>;
template class Swift<falcon_rue::EVENT_Gen3, falcon_rue::Response_Gen3>;

}  // namespace rue
}  // namespace isekai
