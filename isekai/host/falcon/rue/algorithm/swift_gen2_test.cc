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

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <numeric>
#include <tuple>
#include <utility>
#include <vector>

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/random/random.h"
#include "absl/status/status.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "isekai/common/testing.h"
#include "isekai/host/falcon/falcon.h"
#include "isekai/host/falcon/rue/algorithm/algorithm.pb.h"
#include "isekai/host/falcon/rue/algorithm/connection_state.h"
#include "isekai/host/falcon/rue/algorithm/dram_state_manager.h"
#include "isekai/host/falcon/rue/algorithm/stateful_algorithm.h"
#include "isekai/host/falcon/rue/algorithm/swift.h"
#include "isekai/host/falcon/rue/bits.h"
#include "isekai/host/falcon/rue/fixed.h"
#include "isekai/host/falcon/rue/format_gen2.h"
#include "isekai/host/falcon/rue/util.h"

namespace isekai {
namespace rue {

using ::isekai::internal::StatusIs;

namespace {

template <typename EventT, typename ResponseT>
SwiftConfiguration MakeConfig(uint32_t fabric_base_delay) {
  auto config = Swift<EventT, ResponseT>::DefaultConfiguration();
  config.set_fabric_base_delay(fabric_base_delay);
  return config;
}

// Checks that no repath event has occurred in the given Response.
template <typename EventT, typename ResponseT>
bool IsResponseRepath(ResponseT response) {
  return response.flow_label_1_valid || response.flow_label_2_valid ||
         response.flow_label_3_valid || response.flow_label_4_valid;
}

// Sets the flow fcwnd values in RUE state to be equal to the given `flow_fcwnd`
// value. This function is useful for Stateful multipath Swift because the fcwnd
// in the event is the connection fcwnd which is not used in the Multipath
// processing functions. Instead,  it will be recalculated as the sum of the
// flow fcwnd values in RUE state.
template <typename EventT, typename ResponseT>
void SetFlowFcwndsInRueState(uint32_t flow_fcwnd,
                             ConnectionState<EventT>& state) {
  for (uint8_t flow_id = 0; flow_id < 4; flow_id++) {
    state.fcwnd[flow_id] = flow_fcwnd;
  }
}

// Gets the expected ncwnd_fixed for an increase.
template <typename EventT, typename ResponseT>
uint32_t GetExpectedIncreasedNcwndFixed(double ncwnd, double ai_factor) {
  double expected_ncwnd_float = ncwnd + ai_factor;
  uint32_t expected_ncwnd_fixed = falcon_rue::FloatToFixed<double, uint32_t>(
      expected_ncwnd_float, falcon_rue::kFractionalBits);
  return expected_ncwnd_fixed;
}

// Gets the expected ncwnd_fixed for a decrease that is proportional to the
// rx_buffer_level.
template <typename EventT, typename ResponseT>
uint32_t GetExpectedDecreasedNcwndFixed(double ncwnd, double md_factor,
                                        uint32_t rx_buffer_level,
                                        uint32_t target_rx_buffer_level) {
  double decrease_scale =
      static_cast<double>(rx_buffer_level - target_rx_buffer_level) /
      rx_buffer_level;
  decrease_scale *= md_factor;
  double expected_ncwnd_float = ncwnd * (1 - decrease_scale);
  uint32_t expected_ncwnd_fixed = falcon_rue::FloatToFixed<double, uint32_t>(
      expected_ncwnd_float, falcon_rue::kFractionalBits);
  return expected_ncwnd_fixed;
}

// Calculates the connection fcwnd, which is the sum of the flow fcwnd values.
// The returned connection fcwnd will be in fixed format, and the flow fcwnd
// values will be passed in as a vector in float format.
template <typename EventT, typename ResponseT>
uint32_t GetFixedConnectionFcwndFromFloatFlowFcwnds(
    std::vector<double> fcwnds) {
  CHECK_EQ(fcwnds.size(), 4);
  uint32_t fixed_connection_fcwnd = 0;
  for (double fcwnd : fcwnds) {
    fixed_connection_fcwnd += falcon_rue::FloatToFixed<double, uint32_t>(
        fcwnd, falcon_rue::kFractionalBits);
  }
  return fixed_connection_fcwnd;
}

// Compares the weights in the response with the expected weights given all the
// flow fcwnd values for a multipath connection.
template <typename EventT, typename ResponseT>
void CheckFlowWeightsGivenFlowFcwnds(const ResponseT& response,
                                     std::vector<double> fcwnds) {
  CHECK_EQ(fcwnds.size(), 4);
  double connection_fcwnd = std::accumulate(fcwnds.begin(), fcwnds.end(), 0.0);

  uint8_t expected_flow_label_1_weight =
      Swift<EventT, ResponseT>::ComputeFlowWeight(fcwnds[0], connection_fcwnd);
  uint8_t expected_flow_label_2_weight =
      Swift<EventT, ResponseT>::ComputeFlowWeight(fcwnds[1], connection_fcwnd);
  uint8_t expected_flow_label_3_weight =
      Swift<EventT, ResponseT>::ComputeFlowWeight(fcwnds[2], connection_fcwnd);
  uint8_t expected_flow_label_4_weight =
      Swift<EventT, ResponseT>::ComputeFlowWeight(fcwnds[3], connection_fcwnd);
  EXPECT_EQ(response.flow_label_1_weight, expected_flow_label_1_weight);
  EXPECT_EQ(response.flow_label_2_weight, expected_flow_label_2_weight);
  EXPECT_EQ(response.flow_label_3_weight, expected_flow_label_3_weight);
  EXPECT_EQ(response.flow_label_4_weight, expected_flow_label_4_weight);
}

}  // namespace

TEST(SwiftTest, GetSmoothed) {
  uint32_t s = 10000;
  uint32_t r = 5000;
  EXPECT_EQ(SwiftGen2::GetSmoothed(0.0, s, r), static_cast<uint32_t>(r));
  EXPECT_EQ(SwiftGen2::GetSmoothed(0.5, s, r),
            static_cast<uint32_t>(s * 0.5 + r * 0.5));
  EXPECT_EQ(SwiftGen2::GetSmoothed(0.75, s, r),
            static_cast<uint32_t>(s * 0.75 + r * 0.25));
  EXPECT_EQ(SwiftGen2::GetSmoothed(0.875, s, r),
            static_cast<uint32_t>(s * 0.875 + r * 0.125));

  double s2 = 10000;
  double r2 = 5000;
  EXPECT_EQ(SwiftGen2::GetSmoothed(0.0, s2, r2), r2);
  EXPECT_EQ(SwiftGen2::GetSmoothed(0.5, s2, r2), s2 * 0.5 + r2 * 0.5);
  EXPECT_EQ(SwiftGen2::GetSmoothed(0.75, s2, r2), s2 * 0.75 + r2 * 0.25);
  EXPECT_EQ(SwiftGen2::GetSmoothed(0.875, s2, r2), s2 * 0.875 + r2 * 0.125);
}

TEST(SwiftTest, GetTargetDelay) {
  uint32_t topo_scaling_units_per_hop = 8;
  // first=base, second=cwndf
  std::vector<std::pair<uint32_t, double>> configs = {std::make_pair(400, 2.0)};
  for (const auto& config : configs) {
    // uint32 base = 400;
    // double cwndf = 2.0;
    uint32_t base = config.first;
    double cwnd = config.second;
    uint32_t hops = 4;
    const double alpha = 0.4387;
    const double beta = 20000000.00 / 131072;  // 20us
    uint32_t act = SwiftGen2::GetTargetDelay(topo_scaling_units_per_hop, alpha,
                                             beta, 99999999, base, cwnd, hops);
    double expf =
        base + (0.4387 / std::sqrt(cwnd) + (20000000.0 / 131072)) + (8 * hops);
    EXPECT_NEAR(act, expf, 1.0);
  }
}

TEST(SwiftTest, GetInterPacketGap) {
  absl::BitGen gen;

  // cwnd >= 1.0 tests, all IPG time shift values result in the same output
  for (uint64_t test = 0; test < 1e5; test++) {
    double ipg_time_scalar = absl::Uniform(gen, 0.00001, 1024.0);
    uint8_t ipg_bits = absl::Uniform(gen, 10, 20);
    double congestion_window = absl::Uniform(gen, 1.0, 1024.0);
    uint32_t rtt = absl::Uniform(gen, 0u, 1u << falcon_rue::kTimeBits);
    uint32_t ipg = SwiftGen2::GetInterPacketGap(ipg_time_scalar, ipg_bits,
                                                congestion_window, rtt);
    EXPECT_EQ(ipg, 0);
  }

  // cwnd < 1.0 tests
  static_assert(falcon_rue::kInterPacketGapBits > 10 &&
                falcon_rue::kInterPacketGapBits < 32);
  uint32_t c1 = 0, c2 = 0;  // Used to ensure test distribution
  for (uint64_t test = 0; test < 1e6; test++) {
    double ipg_time_scalar;
    if (absl::Bernoulli(gen, 0.5)) {
      ipg_time_scalar = absl::Uniform(gen, 0.25, 1.0);
    } else {
      ipg_time_scalar = absl::Uniform(gen, 1.0, 8.0);
    }
    uint8_t ipg_bits;
    if (absl::Bernoulli(gen, 0.5)) {
      ipg_bits = absl::Uniform(gen, 10, falcon_rue::kInterPacketGapBits);
    } else {
      ipg_bits = falcon_rue::kInterPacketGapBits;
    }
    double congestion_window;
    if (absl::Bernoulli(gen, 0.5)) {
      congestion_window = absl::Uniform(gen, 1.0 / 128, 1.0 / 8);
    } else {
      congestion_window = absl::Uniform(gen, 1.0 / 8, 1.0);
    }
    uint32_t rtt;
    if (absl::Bernoulli(gen, 0.5)) {
      rtt = absl::Uniform(gen, 1u, 400u);
    } else {
      rtt = absl::Uniform(gen, 400u, (1u << falcon_rue::kTimeBits) / 2);
    }
    uint32_t ipg = SwiftGen2::GetInterPacketGap(ipg_time_scalar, ipg_bits,
                                                congestion_window, rtt);
    double ideal = (rtt / congestion_window - rtt) * ipg_time_scalar;
    uint32_t max = (1lu << ipg_bits) - 1;
    if (ideal > max) {
      EXPECT_EQ(ipg, max);
      c1++;
    } else {
      EXPECT_NEAR(ipg, ideal, 1.0);
      c2++;
    }
  }
  uint32_t diff = (c1 > c2) ? (c1 - c2) : (c2 - c1);
  EXPECT_LT(diff, 1e5);  // Verifies test distribution
}

TEST(SwiftTest, GetRetransmitTimeout) {
  absl::BitGen gen;
  for (uint32_t test = 0; test < 1e6; test++) {
    // The valid retransmit timeout range is [0, 2^kTimeBits).
    uint32_t min_rto = absl::Uniform(gen, 0, 1 << falcon_rue::kTimeBits);

    // The valid RTT range is [0, 2^kTimeBits).
    uint32_t rtt = absl::Uniform(gen, 0, 1 << falcon_rue::kTimeBits);

    // Gets a random RTT scalar value.
    double scalar = absl::Uniform(gen, 1.0, 3.0);

    uint32_t act = SwiftGen2::GetRetransmitTimeout(
        min_rto, rtt, scalar, /*inter_packet_gap=*/0, /*ipg_time_scalar=*/1);
    if ((rtt * scalar) <= min_rto) {
      EXPECT_EQ(act, min_rto);
    } else if ((rtt * scalar) >=
               falcon_rue::ValidMask<uint32_t>(falcon_rue::kTimeBits)) {
      EXPECT_EQ(act, falcon_rue::ValidMask<uint32_t>(falcon_rue::kTimeBits));
    } else {
      EXPECT_NEAR(act, rtt * scalar, 1.0);
    }
  }

  uint32_t rtt = 10;
  double scalar = 3;
  uint32_t act = SwiftGen2::GetRetransmitTimeout(
      /*min_retransmit_timeout=*/0, rtt, scalar,
      /*inter_packet_gap=*/5,
      /*ipg_time_scalar=*/2.0);
  EXPECT_NEAR(act, rtt * scalar + 5 / 2.0, 1.0);

  act = SwiftGen2::GetRetransmitTimeout(
      /*min_retransmit_timeout=*/0, rtt, scalar,
      /*inter_packet_gap=*/(1 << falcon_rue::kTimeBits) - 1,
      /*ipg_time_scalar=*/1.0);
  EXPECT_EQ(act, falcon_rue::ValidMask<uint32_t>(falcon_rue::kTimeBits));
}

// Tests that both delay smoothing and PLB can be enabled at the same time in
// Gen_2.
TEST(SwiftTest, DelaySmoothingAndPlbConfig) {
  SwiftConfiguration config = SwiftGen2::DefaultConfiguration();
  config.set_delay_smoothing_alpha(0.5);
  config.set_randomize_path(true);
  ASSERT_OK(SwiftGen2::Create(config));
}

// Tests the ComputeFlowWeight() function to make sure it calculating the
// weights as expected.
TEST(SwiftTest, ComputeFlowWeight) {
  constexpr double kConnectionFcnwd = 100;

  uint8_t actual_weight = SwiftGen2::ComputeFlowWeight(20, kConnectionFcnwd);
  uint8_t expected_weight =
      static_cast<uint32_t>(kMaxFlowWeight * 20 / kConnectionFcnwd);
  EXPECT_EQ(actual_weight, expected_weight);

  actual_weight = SwiftGen2::ComputeFlowWeight(40, kConnectionFcnwd);
  expected_weight =
      static_cast<uint32_t>(kMaxFlowWeight * 40 / kConnectionFcnwd);
  EXPECT_EQ(actual_weight, expected_weight);

  actual_weight = SwiftGen2::ComputeFlowWeight(100, kConnectionFcnwd);
  expected_weight =
      static_cast<uint32_t>(kMaxFlowWeight * 100 / kConnectionFcnwd);
  EXPECT_EQ(actual_weight, expected_weight);

  // Need to round first before static_cast to uint8_t.
  actual_weight = SwiftGen2::ComputeFlowWeight(30, kConnectionFcnwd);
  expected_weight =
      static_cast<uint32_t>(std::round(kMaxFlowWeight * 30 / kConnectionFcnwd));
  EXPECT_EQ(actual_weight, expected_weight);
}

namespace {

template <typename TypeParam>
class SwiftTypedTest : public testing::Test {
 public:
  typedef typename std::tuple_element<0, TypeParam>::type EventT;
  typedef typename std::tuple_element<1, TypeParam>::type ResponseT;
  EventT event_;
  ResponseT response_;

  SwiftTypedTest() {
    memset(&event_, 0, sizeof(EventT));
    memset(&response_, 0, sizeof(EventT));
  }

  ~SwiftTypedTest() override = default;
};

using Event_Response_Types = ::testing::Types<
    std::tuple<falcon_rue::Event_Gen2, falcon_rue::Response_Gen2>>;
TYPED_TEST_SUITE(SwiftTypedTest, Event_Response_Types);

// Tests the ACK case of multiplicative decrease where the actual delay is
// greater than the target delay. The congestion is reduced but it does
// not hit the maximum reduction.
TYPED_TEST(SwiftTypedTest, ProcessAckMd150p) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;
  const auto config = MakeConfig<EventT, ResponseT>(100);
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<SwiftTyped> swift,
                       SwiftTyped::Create(config));

  const auto new_now = 101000;

  this->event_.connection_id = 1234;
  this->event_.multipath_enable = false;
  this->event_.event_type = falcon::RueEventType::kAck;
  this->event_.timestamp_1 = 100000;
  this->event_.timestamp_2 = 100440;
  this->event_.timestamp_3 = 100600;
  this->event_.timestamp_4 = 100800;
  this->event_.retransmit_count = 0;
  this->event_.retransmit_reason = falcon::RetransmitReason::kTimeout;
  this->event_.nack_code = falcon::NackCode::kNotANack;
  this->event_.forward_hops = 5;
  this->event_.cc_metadata = 0;
  this->event_.fabric_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(4.0,
                                                 falcon_rue::kFractionalBits);
  this->event_.num_packets_acked = 3;
  this->event_.event_queue_select = 0;
  this->event_.delay_select = falcon::DelaySelect::kForward;
  this->event_.fabric_window_time_marker = 99000;  // < now - rtt_state
  this->event_.base_delay = SwiftTyped::MakeBaseDelayField(
      /*profile_index=*/kSwiftDefaultProfileIndex);
  this->event_.delay_state = 150;
  this->event_.rtt_state = 710;
  this->event_.cc_opaque = 3;
  this->event_.gen_bit = 0;
  LOG(INFO) << (this->event_.delay_select == falcon::DelaySelect::kForward);

  swift->Process(this->event_, this->response_, new_now);

  EXPECT_EQ(this->response_.connection_id, this->event_.connection_id);
  EXPECT_EQ((IsResponseRepath<EventT, ResponseT>(this->response_)), false);
  EXPECT_EQ(this->response_.cc_metadata, 0);
  const double delay = GetPacketTiming(this->event_).delay;
  const double fabric_congestion_window =
      falcon_rue::FixedToFloat<uint32_t, double>(
          this->event_.fabric_congestion_window, falcon_rue::kFractionalBits);
  ASSERT_OK_AND_ASSIGN(
      double flow_scaling_alpha,
      SwiftTyped::CalculateFlowScalingAlpha(config.max_flow_scaling(),
                                            config.min_flow_scaling_window(),
                                            config.max_flow_scaling_window()));
  ASSERT_OK_AND_ASSIGN(
      double flow_scaling_beta,
      SwiftTyped::CalculateFlowScalingBeta(
          flow_scaling_alpha, config.max_fabric_congestion_window()));
  const auto target_delay = SwiftTyped::GetTargetDelay(
      config.topo_scaling_per_hop(), flow_scaling_alpha, flow_scaling_beta,
      config.max_flow_scaling(), config.fabric_base_delay(),
      fabric_congestion_window, this->event_.forward_hops);
  const double exp_cwnd =
      fabric_congestion_window *
      (1.0 - ((delay - target_delay) / delay) *
                 config.fabric_multiplicative_decrease_factor());
  const double act_cwnd = falcon_rue::FixedToFloat<uint32_t, double>(
      this->response_.fabric_congestion_window, falcon_rue::kFractionalBits);
  EXPECT_NEAR(act_cwnd, exp_cwnd, 0.01);  // updated
  EXPECT_EQ(this->response_.inter_packet_gap, 0);
  const auto rtt = this->event_.timestamp_4 - this->event_.timestamp_1;
  uint32_t exp_rtt_state =
      this->event_.rtt_state * config.rtt_smoothing_alpha() +
      rtt * (1 - config.rtt_smoothing_alpha());
  uint32_t exp_rto = std::max(
      config.min_retransmit_timeout(),
      static_cast<uint32_t>(exp_rtt_state * config.retransmit_timeout_scalar() +
                            this->response_.inter_packet_gap *
                                config.ipg_time_scalar()));
  EXPECT_EQ(this->response_.retransmit_timeout, exp_rto);
  EXPECT_EQ(this->response_.fabric_window_time_marker,
            new_now);  //  decreased now!
  EXPECT_EQ(this->response_.event_queue_select, 0);
  EXPECT_EQ(this->response_.delay_select, falcon::DelaySelect::kForward);
  EXPECT_EQ(this->response_.base_delay, this->event_.base_delay);
  EXPECT_EQ(this->response_.delay_state, delay);
  EXPECT_NEAR(this->response_.rtt_state, exp_rtt_state, 1);
  EXPECT_EQ(this->response_.cc_opaque, this->event_.cc_opaque);
}

TYPED_TEST(SwiftTypedTest, DefaultConfigRandomizesFlowLabels) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;
  auto config = SwiftTyped::DefaultConfiguration();
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<SwiftTyped> swift1,
                       SwiftTyped::Create(config));
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<SwiftTyped> swift2,
                       SwiftTyped::Create(config));

  std::vector<uint32_t> swift1_flow_labels;
  for (int i = 0; i < 100; ++i)
    swift1_flow_labels.push_back(swift1->RandomFlowLabel(0));

  std::vector<uint32_t> swift2_flow_labels;
  for (int i = 0; i < 100; ++i)
    swift2_flow_labels.push_back(swift2->RandomFlowLabel(0));

  EXPECT_NE(swift1_flow_labels, swift2_flow_labels);
}

TYPED_TEST(SwiftTypedTest, SeededSwiftHasDeterministicFlowLabels) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;
  auto config = SwiftTyped::DefaultConfiguration();
  config.set_flow_label_rng_seed(42);
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<SwiftTyped> swift1,
                       SwiftTyped::Create(config));
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<SwiftTyped> swift2,
                       SwiftTyped::Create(config));

  std::vector<uint32_t> swift1_flow_labels;
  for (int i = 0; i < 100; ++i)
    swift1_flow_labels.push_back(swift1->RandomFlowLabel(0));

  std::vector<uint32_t> swift2_flow_labels;
  for (int i = 0; i < 100; ++i)
    swift2_flow_labels.push_back(swift2->RandomFlowLabel(0));

  EXPECT_EQ(swift1_flow_labels, swift2_flow_labels);
}

// Tests the ACK case of multiplicative decrease where the target delay
// and the actual delay are the same. In this case the output congestion
// window is the same as the input congestion window.
TYPED_TEST(SwiftTypedTest, ProcessAckMdEqual) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;
  const auto config = MakeConfig<EventT, ResponseT>(100);
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<SwiftTyped> swift,
                       SwiftTyped::Create(config));

  const auto new_now = 10100;

  this->event_.connection_id = 1234;
  this->event_.multipath_enable = false;
  this->event_.event_type = falcon::RueEventType::kAck;
  this->event_.timestamp_1 = 100000;
  this->event_.timestamp_2 =
      100292;  // will be set to a calculated target delay later.
  this->event_.timestamp_3 = 100600;
  this->event_.timestamp_4 = 100715;
  this->event_.retransmit_count = 0;
  this->event_.retransmit_reason = falcon::RetransmitReason::kTimeout;
  this->event_.nack_code = falcon::NackCode::kNotANack;
  this->event_.forward_hops = 5;
  this->event_.cc_metadata = 0;
  this->event_.fabric_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(3.0,
                                                 falcon_rue::kFractionalBits);
  this->event_.num_packets_acked = 3;
  this->event_.event_queue_select = 0;
  this->event_.delay_select = falcon::DelaySelect::kForward;
  this->event_.fabric_window_time_marker = 99999;
  this->event_.base_delay = SwiftTyped::MakeBaseDelayField(
      /*profile_index=*/kSwiftDefaultProfileIndex);
  this->event_.delay_state = 150;
  this->event_.rtt_state = 600;
  this->event_.cc_opaque = 3;
  this->event_.gen_bit = 0;

  const double fabric_congestion_window =
      falcon_rue::FixedToFloat<uint32_t, double>(
          this->event_.fabric_congestion_window, falcon_rue::kFractionalBits);
  ASSERT_OK_AND_ASSIGN(
      double flow_scaling_alpha,
      SwiftTyped::CalculateFlowScalingAlpha(config.max_flow_scaling(),
                                            config.min_flow_scaling_window(),
                                            config.max_flow_scaling_window()));
  ASSERT_OK_AND_ASSIGN(
      double flow_scaling_beta,
      SwiftTyped::CalculateFlowScalingBeta(
          flow_scaling_alpha, config.max_fabric_congestion_window()));

  const auto target_delay = SwiftTyped::GetTargetDelay(
      config.topo_scaling_per_hop(), flow_scaling_alpha, flow_scaling_beta,
      config.max_flow_scaling(), config.fabric_base_delay(),
      fabric_congestion_window, this->event_.forward_hops);
  this->event_.timestamp_2 = this->event_.timestamp_1 + target_delay;

  swift->Process(this->event_, this->response_, new_now);

  EXPECT_EQ(this->response_.connection_id, this->event_.connection_id);
  EXPECT_EQ((IsResponseRepath<EventT, ResponseT>(this->response_)), false);
  EXPECT_EQ(this->response_.cc_metadata, this->event_.cc_metadata);
  EXPECT_EQ(this->response_.fabric_congestion_window,
            this->event_.fabric_congestion_window);  // no change
  EXPECT_EQ(this->response_.inter_packet_gap, 0);
  const auto rtt = this->event_.timestamp_4 - this->event_.timestamp_1;
  uint32_t exp_rtt_state =
      this->event_.rtt_state * config.rtt_smoothing_alpha() +
      rtt * (1 - config.rtt_smoothing_alpha());
  uint32_t exp_rto = std::max(
      config.min_retransmit_timeout(),
      static_cast<uint32_t>(exp_rtt_state * config.retransmit_timeout_scalar() +
                            this->response_.inter_packet_gap *
                                config.ipg_time_scalar()));
  EXPECT_EQ(this->response_.retransmit_timeout, exp_rto);
  EXPECT_EQ(this->response_.fabric_window_time_marker,
            new_now - rtt);  // updated
  EXPECT_EQ(this->response_.event_queue_select, 0);
  EXPECT_EQ(this->response_.delay_select, falcon::DelaySelect::kForward);
  EXPECT_EQ(this->response_.base_delay, this->event_.base_delay);
  EXPECT_EQ(this->response_.delay_state,
            this->event_.timestamp_2 - this->event_.timestamp_1);
  EXPECT_NEAR(this->response_.rtt_state, exp_rtt_state, 1);
  EXPECT_EQ(this->response_.cc_opaque, this->event_.cc_opaque);
}

TYPED_TEST(SwiftTypedTest, GetAlphaBeta) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;
  const auto config = MakeConfig<EventT, ResponseT>(100);
  ASSERT_OK_AND_ASSIGN(double alpha, SwiftTyped::CalculateFlowScalingAlpha(
                                         config.max_flow_scaling(),
                                         config.min_flow_scaling_window(),
                                         config.max_flow_scaling_window()));
  ASSERT_OK_AND_ASSIGN(double beta,
                       SwiftTyped::CalculateFlowScalingBeta(
                           alpha, config.max_fabric_congestion_window()));

  const auto min_flow_scale =
      alpha / std::sqrt(config.max_flow_scaling_window()) + beta;
  const auto max_flow_scale =
      alpha / std::sqrt(config.min_flow_scaling_window()) + beta;
  // Alpha and beta should create a line between these two points in <window,
  // flow_scaling> plane: <min_window, max_flow_scaling> and <max_window, 0>.
  // This if checks if the endpoints are close.
  EXPECT_NEAR(max_flow_scale, config.max_flow_scaling(), 0.5);

  EXPECT_NEAR(min_flow_scale, 0, 0.5);
  // 1/(RSqrt(0.25)-RSqrt(1)) = 1/(2-1) = 1
  ASSERT_OK_AND_ASSIGN(alpha, SwiftTyped::CalculateFlowScalingAlpha(
                                  /*/*max_flow_scaling=*/1,
                                  /*min_flow_scaling_window=*/0.25,
                                  /*max_flow_scaling_window=*/1));
  EXPECT_NEAR(1, alpha, 0.01);

  // 2/(RSqrt(0.25)-RSqrt(1)) = 2/(2-1) = 2
  ASSERT_OK_AND_ASSIGN(alpha, SwiftTyped::CalculateFlowScalingAlpha(
                                  /*max_flow_scaling=*/2,
                                  /*min_flow_scaling_window=*/0.25,
                                  /*max_flow_scaling_window=*/1));
  EXPECT_NEAR(2, alpha, 0.01);

  // zero max_flow_scaling is ok
  EXPECT_OK(
      SwiftTyped::CalculateFlowScalingAlpha(/*max_flow_scaling=*/0,
                                            /*min_flow_scaling_window=*/1,
                                            /*max_flow_scaling_window=*/2));
  // equal min and max windows
  EXPECT_THAT(SwiftTyped::CalculateFlowScalingAlpha(1, 1, 1),
              StatusIs(absl::StatusCode::kInvalidArgument));
  // negative min window
  EXPECT_THAT(SwiftTyped::CalculateFlowScalingAlpha(1, -1, 1),
              StatusIs(absl::StatusCode::kInvalidArgument));
  // negative max window
  EXPECT_THAT(SwiftTyped::CalculateFlowScalingAlpha(1, 1, -1),
              StatusIs(absl::StatusCode::kInvalidArgument));
  // zero min window
  EXPECT_THAT(SwiftTyped::CalculateFlowScalingAlpha(1, 0, 1),
              StatusIs(absl::StatusCode::kInvalidArgument));
  // zero max window
  EXPECT_THAT(SwiftTyped::CalculateFlowScalingAlpha(1, 1, 0),
              StatusIs(absl::StatusCode::kInvalidArgument));
  // min > max window
  EXPECT_THAT(SwiftTyped::CalculateFlowScalingAlpha(1, 2, 1),
              StatusIs(absl::StatusCode::kInvalidArgument));
  // negative max_flow_scaling
  EXPECT_THAT(SwiftTyped::CalculateFlowScalingAlpha(-1, 1, 2),
              StatusIs(absl::StatusCode::kInvalidArgument));

  // negative alpha
  EXPECT_THAT(
      SwiftTyped::CalculateFlowScalingBeta(/*flow_scaling_alpha=*/-1,
                                           /*max_flow_scaling_window=*/1),
      StatusIs(absl::StatusCode::kInvalidArgument));
  // negative max window
  EXPECT_THAT(SwiftTyped::CalculateFlowScalingBeta(1, -1),
              StatusIs(absl::StatusCode::kInvalidArgument));
  // zero max window
  EXPECT_THAT(SwiftTyped::CalculateFlowScalingBeta(1, 0),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TYPED_TEST(SwiftTypedTest, MinMaxBounds) {
  // expect ((2**21)-1)/(2**10)
  EXPECT_NEAR(kMaxFabricCongestionWindow, 2047.9990234375000,
              std::numeric_limits<double>::epsilon());
  // expect 1/(2**10)
  EXPECT_NEAR(kMinFabricCongestionWindow, 0.000976562500000,
              std::numeric_limits<double>::epsilon());
  // expect ((2**21)-1)/(2**10)
  EXPECT_NEAR(kMaxGen2NicCongestionWindow, 2047.9990234375000,
              std::numeric_limits<double>::epsilon());
  // expect 1/(2**10)
  EXPECT_NEAR(kMinGen2NicCongestionWindow, 0.000976562500000,
              std::numeric_limits<double>::epsilon());
  // expect (2**5)-1
  EXPECT_EQ(kMaxRxBufferLevel, 31);
  // expect 0
  EXPECT_EQ(kMinRxBufferLevel, 0);
  // expect 12 bits
  EXPECT_EQ(kMaxTopoScalingPerHop, 4095);
}

// Tests the ACK case of additive increment where the input
// congestion window is greater than 1. In this case the output congestion
// window has an addition of the additive increase times the number of
// acked packets and the increase is scaled by the congestion window.
TYPED_TEST(SwiftTypedTest, ProcessAckAiOverCwnd) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;

  const auto config = MakeConfig<EventT, ResponseT>(100);
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<SwiftTyped> swift,
                       SwiftTyped::Create(config));
  const auto new_now = 100400;

  const double fcwnd = 8.0;
  this->event_.connection_id = 1234;
  this->event_.multipath_enable = false;
  this->event_.event_type = falcon::RueEventType::kAck;
  this->event_.timestamp_1 = 100000;
  this->event_.timestamp_2 = 100153;
  this->event_.timestamp_3 = 100200;
  this->event_.timestamp_4 = 100315;
  this->event_.retransmit_count = 0;
  this->event_.retransmit_reason = falcon::RetransmitReason::kTimeout;
  this->event_.nack_code = falcon::NackCode::kNotANack;
  this->event_.forward_hops = 5;
  this->event_.cc_metadata = 0;
  this->event_.fabric_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(fcwnd,
                                                 falcon_rue::kFractionalBits);

  this->event_.num_packets_acked = 3;
  this->event_.event_queue_select = 0;
  this->event_.delay_select = falcon::DelaySelect::kForward;
  this->event_.fabric_window_time_marker = 99999;
  this->event_.base_delay = SwiftTyped::MakeBaseDelayField(
      /*profile_index=*/kSwiftDefaultProfileIndex);
  this->event_.delay_state = 150;
  this->event_.rtt_state = 310;
  this->event_.cc_opaque = 3;
  this->event_.gen_bit = 0;

  swift->Process(this->event_, this->response_, new_now);

  EXPECT_EQ(this->response_.connection_id, this->event_.connection_id);
  EXPECT_EQ((IsResponseRepath<EventT, ResponseT>(this->response_)), false);
  EXPECT_EQ(this->response_.cc_metadata, 0);
  const double exp_cwnd_f = fcwnd + (config.fabric_additive_increment_factor() *
                                     this->event_.num_packets_acked) /
                                        fcwnd;
  const uint32_t exp_cwnd = falcon_rue::FloatToFixed<double, uint32_t>(
      exp_cwnd_f, falcon_rue::kFractionalBits);
  EXPECT_EQ(this->response_.fabric_congestion_window, exp_cwnd);  // updated
  EXPECT_EQ(this->response_.inter_packet_gap, 0);
  const auto rtt = this->event_.timestamp_4 - this->event_.timestamp_1;
  uint32_t exp_rtt_state =
      this->event_.rtt_state * config.rtt_smoothing_alpha() +
      rtt * (1 - config.rtt_smoothing_alpha());
  uint32_t exp_rto =
      std::max(exp_rtt_state * 2, config.min_retransmit_timeout());
  EXPECT_EQ(this->response_.retransmit_timeout, exp_rto);
  EXPECT_EQ(this->response_.fabric_window_time_marker,
            new_now - rtt);  // updated
  EXPECT_EQ(this->response_.event_queue_select, 0);
  EXPECT_EQ(this->response_.delay_select, falcon::DelaySelect::kForward);
  EXPECT_EQ(this->response_.base_delay, this->event_.base_delay);
  EXPECT_EQ(this->response_.delay_state,
            this->event_.timestamp_2 - this->event_.timestamp_1);
  EXPECT_NEAR(this->response_.rtt_state, exp_rtt_state, 1);
  EXPECT_EQ(this->response_.cc_opaque, this->event_.cc_opaque);
}

// Tests the ACK case of additive increment where the input
// congestion window is less than 1. In this case the output congestion
// window has an addition of the additive increase times the number of
// acked packets.
TYPED_TEST(SwiftTypedTest, ProcessAckAi) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;
  const auto config = MakeConfig<EventT, ResponseT>(100);
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<SwiftTyped> swift,
                       SwiftTyped::Create(config));

  const auto new_now = 100400;
  const double fcwnd = 0.5;  // a value < 1 as something paces if fcwnd<1

  this->event_.connection_id = 1234;
  this->event_.multipath_enable = false;
  this->event_.event_type = falcon::RueEventType::kAck;
  this->event_.timestamp_1 = 100000;
  this->event_.timestamp_2 = 100153;
  this->event_.timestamp_3 = 100200;
  this->event_.timestamp_4 = 100315;
  this->event_.retransmit_count = 0;
  this->event_.retransmit_reason = falcon::RetransmitReason::kTimeout;
  this->event_.nack_code = falcon::NackCode::kNotANack;
  this->event_.forward_hops = 5;
  this->event_.cc_metadata = 0;
  this->event_.fabric_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(fcwnd,
                                                 falcon_rue::kFractionalBits);
  this->event_.num_packets_acked = 3;
  this->event_.event_queue_select = 0;
  this->event_.delay_select = falcon::DelaySelect::kForward;
  this->event_.fabric_window_time_marker = 99999;
  this->event_.base_delay = SwiftTyped::MakeBaseDelayField(
      /*profile_index=*/kSwiftDefaultProfileIndex);
  this->event_.delay_state = 150;
  this->event_.rtt_state = 310;
  this->event_.cc_opaque = 3;
  this->event_.gen_bit = 0;

  swift->Process(this->event_, this->response_, new_now);

  EXPECT_EQ(this->response_.connection_id, this->event_.connection_id);
  EXPECT_EQ((IsResponseRepath<EventT, ResponseT>(this->response_)), false);
  EXPECT_EQ(this->response_.cc_metadata, 0);
  const double exp_cwnd_f = fcwnd + (config.fabric_additive_increment_factor() *
                                     this->event_.num_packets_acked);
  const uint32_t exp_cwnd = falcon_rue::FloatToFixed<double, uint32_t>(
      exp_cwnd_f, falcon_rue::kFractionalBits);
  EXPECT_EQ(this->response_.fabric_congestion_window, exp_cwnd);  // updated
  EXPECT_EQ(this->response_.inter_packet_gap, 0);
  const auto rtt = this->event_.timestamp_4 - this->event_.timestamp_1;
  uint32_t exp_rtt_state =
      this->event_.rtt_state * config.rtt_smoothing_alpha() +
      rtt * (1 - config.rtt_smoothing_alpha());
  uint32_t exp_rto = std::max(
      config.min_retransmit_timeout(),
      static_cast<uint32_t>(exp_rtt_state * config.retransmit_timeout_scalar() +
                            this->response_.inter_packet_gap *
                                config.ipg_time_scalar()));
  EXPECT_EQ(this->response_.retransmit_timeout, exp_rto);
  EXPECT_EQ(this->response_.fabric_window_time_marker, new_now - rtt);
  EXPECT_EQ(this->response_.event_queue_select, 0);
  EXPECT_EQ(this->response_.delay_select, falcon::DelaySelect::kForward);
  EXPECT_EQ(this->response_.base_delay, this->event_.base_delay);
  EXPECT_EQ(this->response_.delay_state,
            this->event_.timestamp_2 - this->event_.timestamp_1);
  EXPECT_NEAR(this->response_.rtt_state, exp_rtt_state, 1);
  EXPECT_EQ(this->response_.cc_opaque, this->event_.cc_opaque);
}

// Tests the ACK case of multiplicative decrease where the actual delay is
// greater than the target delay. The congestion is reduced and it uses the
// maximum reduction.
TYPED_TEST(SwiftTypedTest, ProcessAckMaxMd) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;
  const auto config = MakeConfig<EventT, ResponseT>(100);
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<SwiftTyped> swift,
                       SwiftTyped::Create(config));

  const auto new_now = 102000;

  const double fcwnd = 2.75;
  this->event_.connection_id = 1234;
  this->event_.multipath_enable = false;
  this->event_.event_type = falcon::RueEventType::kAck;
  this->event_.timestamp_1 = 100000;
  this->event_.timestamp_2 = 100879;
  this->event_.timestamp_3 = 101000;
  this->event_.timestamp_4 = 101200;
  this->event_.retransmit_count = 0;
  this->event_.retransmit_reason = falcon::RetransmitReason::kTimeout;
  this->event_.nack_code = falcon::NackCode::kNotANack;
  this->event_.forward_hops = 5;
  this->event_.cc_metadata = 0;
  this->event_.fabric_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(fcwnd,
                                                 falcon_rue::kFractionalBits);
  this->event_.num_packets_acked = 3;
  this->event_.event_queue_select = 0;
  this->event_.delay_select = falcon::DelaySelect::kForward;
  this->event_.fabric_window_time_marker = 99999;
  this->event_.base_delay = SwiftTyped::MakeBaseDelayField(
      /*profile_index=*/kSwiftDefaultProfileIndex);
  this->event_.delay_state = 150;
  this->event_.rtt_state = 1000;
  this->event_.cc_opaque = 3;
  this->event_.gen_bit = 0;

  swift->Process(this->event_, this->response_, new_now);

  EXPECT_EQ(this->response_.connection_id, this->event_.connection_id);
  EXPECT_EQ((IsResponseRepath<EventT, ResponseT>(this->response_)), false);
  EXPECT_EQ(this->response_.cc_metadata, 0);
  const double exp_cwnd =
      fcwnd * (1.0 - config.max_fabric_multiplicative_decrease());
  const double act_cwnd = falcon_rue::FixedToFloat<uint32_t, double>(
      this->response_.fabric_congestion_window, falcon_rue::kFractionalBits);
  EXPECT_NEAR(act_cwnd, exp_cwnd, exp_cwnd * 0.01);  // maximum decrease
  EXPECT_EQ(this->response_.inter_packet_gap, 0);
  const auto rtt = this->event_.timestamp_4 - this->event_.timestamp_1;
  uint32_t exp_rtt_state =
      this->event_.rtt_state * config.rtt_smoothing_alpha() +
      rtt * (1 - config.rtt_smoothing_alpha());
  uint32_t exp_rto = std::max(
      config.min_retransmit_timeout(),
      static_cast<uint32_t>(exp_rtt_state * config.retransmit_timeout_scalar() +
                            this->response_.inter_packet_gap *
                                config.ipg_time_scalar()));
  EXPECT_EQ(this->response_.retransmit_timeout, exp_rto);
  EXPECT_EQ(this->response_.fabric_window_time_marker, new_now);  // updated
  EXPECT_EQ(this->response_.event_queue_select, 0);
  EXPECT_EQ(this->response_.delay_select, falcon::DelaySelect::kForward);
  EXPECT_EQ(this->response_.base_delay, this->event_.base_delay);
  EXPECT_EQ(this->response_.delay_state,
            this->event_.timestamp_2 - this->event_.timestamp_1);
  EXPECT_NEAR(this->response_.rtt_state, exp_rtt_state, 1);
  EXPECT_EQ(this->response_.cc_opaque, this->event_.cc_opaque);
}

// Tests the ACK case where multiplicative decrease would be used but not
// enough time has passed since the last decrease so no change is made.
TYPED_TEST(SwiftTypedTest, ProcessAckSkipMd) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;
  const auto config = MakeConfig<EventT, ResponseT>(100);
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<SwiftTyped> swift,
                       SwiftTyped::Create(config));

  this->event_.connection_id = 1234;
  this->event_.multipath_enable = false;
  this->event_.event_type = falcon::RueEventType::kAck;
  this->event_.timestamp_1 = 100000;
  this->event_.timestamp_2 = 100879;
  this->event_.timestamp_3 = 101000;
  this->event_.timestamp_4 = 101200;
  this->event_.retransmit_count = 0;
  this->event_.retransmit_reason = falcon::RetransmitReason::kTimeout;
  this->event_.nack_code = falcon::NackCode::kNotANack;
  this->event_.forward_hops = 5;
  this->event_.cc_metadata = 0;
  this->event_.fabric_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(2.75,
                                                 falcon_rue::kFractionalBits);
  this->event_.num_packets_acked = 3;
  this->event_.event_queue_select = 0;
  this->event_.delay_select = falcon::DelaySelect::kForward;
  this->event_.fabric_window_time_marker = 101000;
  this->event_.base_delay = SwiftTyped::MakeBaseDelayField(
      /*profile_index=*/kSwiftDefaultProfileIndex);
  this->event_.delay_state = 150;
  this->event_.rtt_state = 1000;
  this->event_.cc_opaque = 3;
  this->event_.gen_bit = 0;

  swift->Process(this->event_, this->response_, 102000);

  EXPECT_EQ(this->response_.connection_id, this->event_.connection_id);
  EXPECT_EQ((IsResponseRepath<EventT, ResponseT>(this->response_)), false);
  EXPECT_EQ(this->response_.cc_metadata, 0);
  EXPECT_EQ(this->response_.fabric_congestion_window,
            this->event_.fabric_congestion_window);  // no change
  EXPECT_EQ(this->response_.inter_packet_gap, 0);
  const auto rtt = this->event_.timestamp_4 - this->event_.timestamp_1;
  uint32_t exp_rtt_state =
      this->event_.rtt_state * config.rtt_smoothing_alpha() +
      rtt * (1 - config.rtt_smoothing_alpha());
  uint32_t exp_rto = std::max(
      config.min_retransmit_timeout(),
      static_cast<uint32_t>(exp_rtt_state * config.retransmit_timeout_scalar() +
                            this->response_.inter_packet_gap *
                                config.ipg_time_scalar()));
  EXPECT_EQ(this->response_.retransmit_timeout, exp_rto);
  EXPECT_EQ(this->response_.fabric_window_time_marker,
            this->event_.fabric_window_time_marker);  // no change
  EXPECT_EQ(this->response_.event_queue_select, 0);
  EXPECT_EQ(this->response_.delay_select, falcon::DelaySelect::kForward);
  EXPECT_EQ(this->response_.base_delay, this->event_.base_delay);
  EXPECT_EQ(this->response_.delay_state,
            this->event_.timestamp_2 - this->event_.timestamp_1);
  EXPECT_NEAR(this->response_.rtt_state, exp_rtt_state, 1);
  EXPECT_EQ(this->response_.cc_opaque, this->event_.cc_opaque);
}

TYPED_TEST(SwiftTypedTest, ProcessNackMaxDecr) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;
  // Tests the case of NACK where the max congestion window decrease is
  // performed.
  auto config = MakeConfig<EventT, ResponseT>(100);
  config.set_max_decrease_on_eack_nack_drop(true);
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<SwiftTyped> swift,
                       SwiftTyped::Create(config));

  const auto new_now = 102000;

  const double fcwnd = 2.75;
  this->event_.connection_id = 1234;
  this->event_.multipath_enable = false;
  this->event_.event_type = falcon::RueEventType::kNack;
  this->event_.timestamp_1 = 100000;
  this->event_.timestamp_2 = 100879;
  this->event_.timestamp_3 = 101000;
  this->event_.timestamp_4 = 101200;
  this->event_.retransmit_count = 0;
  this->event_.retransmit_reason = falcon::RetransmitReason::kTimeout;
  this->event_.nack_code = falcon::NackCode::kReserved;
  this->event_.forward_hops = 5;
  this->event_.cc_metadata = 0;
  this->event_.fabric_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(fcwnd,
                                                 falcon_rue::kFractionalBits);
  this->event_.num_packets_acked = 3;
  this->event_.event_queue_select = 0;
  this->event_.delay_select = falcon::DelaySelect::kForward;
  this->event_.fabric_window_time_marker = 99999;
  this->event_.base_delay = SwiftTyped::MakeBaseDelayField(
      /*profile_index=*/kSwiftDefaultProfileIndex);
  this->event_.delay_state = 150;
  this->event_.rtt_state = 1000;
  this->event_.cc_opaque = 3;
  this->event_.gen_bit = 0;

  swift->Process(this->event_, this->response_, new_now);

  EXPECT_EQ(this->response_.connection_id, this->event_.connection_id);
  EXPECT_EQ((IsResponseRepath<EventT, ResponseT>(this->response_)), false);
  EXPECT_EQ(this->response_.cc_metadata, 0);
  const double exp_cwnd = fcwnd * config.max_fabric_multiplicative_decrease();
  const double act_cwnd = falcon_rue::FixedToFloat<uint32_t, double>(
      this->response_.fabric_congestion_window, falcon_rue::kFractionalBits);
  EXPECT_NEAR(act_cwnd, exp_cwnd, exp_cwnd * 0.01);
  EXPECT_EQ(this->response_.inter_packet_gap, 0);
  const auto rtt = this->event_.timestamp_4 - this->event_.timestamp_1;
  uint32_t exp_rtt_state =
      this->event_.rtt_state * config.rtt_smoothing_alpha() +
      rtt * (1 - config.rtt_smoothing_alpha());
  uint32_t exp_rto = std::max(
      config.min_retransmit_timeout(),
      static_cast<uint32_t>(exp_rtt_state * config.retransmit_timeout_scalar() +
                            this->response_.inter_packet_gap *
                                config.ipg_time_scalar()));
  EXPECT_EQ(this->response_.retransmit_timeout, exp_rto);
  EXPECT_EQ(this->response_.fabric_window_time_marker, new_now);  // updated
  EXPECT_EQ(this->response_.event_queue_select, 0);
  EXPECT_EQ(this->response_.delay_select, falcon::DelaySelect::kForward);
  EXPECT_EQ(this->response_.base_delay, this->event_.base_delay);
  EXPECT_EQ(this->response_.delay_state,
            this->event_.timestamp_2 - this->event_.timestamp_1);
  EXPECT_NEAR(this->response_.rtt_state, exp_rtt_state, 1);
  EXPECT_EQ(this->response_.cc_opaque, this->event_.cc_opaque);
}

// Tests the case of NACK where the max congestion window decrease is not
// performed because a decrease has occurred too recently.
TYPED_TEST(SwiftTypedTest, ProcessNackNoMaxDecr) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;
  auto config = MakeConfig<EventT, ResponseT>(100);
  config.set_max_decrease_on_eack_nack_drop(true);
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<SwiftTyped> swift,
                       SwiftTyped::Create(config));

  this->event_.connection_id = 1234;
  this->event_.multipath_enable = false;
  this->event_.event_type = falcon::RueEventType::kNack;
  this->event_.timestamp_1 = 100000;
  this->event_.timestamp_2 = 100879;
  this->event_.timestamp_3 = 101000;
  this->event_.timestamp_4 = 101200;
  this->event_.retransmit_count = 0;
  this->event_.retransmit_reason = falcon::RetransmitReason::kTimeout;
  this->event_.nack_code = falcon::NackCode::kReserved;
  this->event_.forward_hops = 5;
  this->event_.cc_metadata = 0;
  this->event_.fabric_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(2.75,
                                                 falcon_rue::kFractionalBits);
  this->event_.num_packets_acked = 3;
  this->event_.event_queue_select = 0;
  this->event_.delay_select = falcon::DelaySelect::kForward;
  this->event_.fabric_window_time_marker = 101000;
  this->event_.base_delay = SwiftTyped::MakeBaseDelayField(
      /*profile_index=*/kSwiftDefaultProfileIndex);
  this->event_.delay_state = 150;
  this->event_.rtt_state = 1000;
  this->event_.cc_opaque = 3;
  this->event_.gen_bit = 0;

  swift->Process(this->event_, this->response_, 102000);

  EXPECT_EQ(this->response_.connection_id, this->event_.connection_id);
  EXPECT_EQ((IsResponseRepath<EventT, ResponseT>(this->response_)), false);
  EXPECT_EQ(this->response_.cc_metadata, 0);
  EXPECT_EQ(this->event_.fabric_congestion_window,
            this->response_.fabric_congestion_window);
  EXPECT_EQ(this->response_.inter_packet_gap, 0);
  const auto rtt = this->event_.timestamp_4 - this->event_.timestamp_1;
  uint32_t exp_rtt_state =
      this->event_.rtt_state * config.rtt_smoothing_alpha() +
      rtt * (1 - config.rtt_smoothing_alpha());
  uint32_t exp_rto = std::max(
      config.min_retransmit_timeout(),
      static_cast<uint32_t>(exp_rtt_state * config.retransmit_timeout_scalar() +
                            this->response_.inter_packet_gap *
                                config.ipg_time_scalar()));
  EXPECT_EQ(this->response_.retransmit_timeout, exp_rto);
  EXPECT_EQ(this->response_.fabric_window_time_marker,
            this->event_.fabric_window_time_marker);  // no change
  EXPECT_EQ(this->response_.event_queue_select, 0);
  EXPECT_EQ(this->response_.delay_select, falcon::DelaySelect::kForward);
  EXPECT_EQ(this->response_.base_delay, this->event_.base_delay);
  EXPECT_EQ(this->response_.delay_state,
            this->event_.timestamp_2 - this->event_.timestamp_1);
  EXPECT_NEAR(this->response_.rtt_state, exp_rtt_state, 1);
  EXPECT_EQ(this->response_.cc_opaque, this->event_.cc_opaque);
}

// Tests the case of NACK where the congestion window is decreased based on
// delay.
TYPED_TEST(SwiftTypedTest, ProcessNackDecreaseReactToDelay) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;
  auto config = MakeConfig<EventT, ResponseT>(100);
  config.set_max_decrease_on_eack_nack_drop(false);
  config.set_max_flow_scaling(1);
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<SwiftTyped> swift,
                       SwiftTyped::Create(config));

  const auto new_now = 100200;

  const double fcwnd = 32;
  this->event_.connection_id = 1234;
  this->event_.multipath_enable = false;
  this->event_.event_type = falcon::RueEventType::kNack;
  this->event_.timestamp_1 = 100000;
  this->event_.timestamp_2 = 100110;
  this->event_.timestamp_3 = 100111;
  this->event_.timestamp_4 = new_now;
  this->event_.retransmit_count = 0;
  this->event_.retransmit_reason = falcon::RetransmitReason::kTimeout;
  this->event_.nack_code = falcon::NackCode::kReserved;
  this->event_.forward_hops = 0;
  this->event_.cc_metadata = 0;
  this->event_.fabric_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(fcwnd,
                                                 falcon_rue::kFractionalBits);
  this->event_.num_packets_acked = 3;
  this->event_.event_queue_select = 0;
  this->event_.delay_select = falcon::DelaySelect::kForward;
  this->event_.fabric_window_time_marker = 99999;
  this->event_.base_delay = SwiftTyped::MakeBaseDelayField(
      /*profile_index=*/kSwiftDefaultProfileIndex);
  this->event_.delay_state = 150;
  this->event_.rtt_state = 1000;
  this->event_.cc_opaque = 3;
  this->event_.gen_bit = 0;

  swift->Process(this->event_, this->response_, new_now);

  EXPECT_EQ(this->response_.connection_id, this->event_.connection_id);
  EXPECT_EQ((IsResponseRepath<EventT, ResponseT>(this->response_)), false);
  EXPECT_EQ(this->response_.cc_metadata, 0);
  const double max_reduced_cwnd =
      fcwnd * config.max_fabric_multiplicative_decrease();
  const double act_cwnd = falcon_rue::FixedToFloat<uint32_t, double>(
      this->response_.fabric_congestion_window, falcon_rue::kFractionalBits);
  // forward delay==110 > target == 101 (topo_scaling = 0, flow_scaling = 1), so
  // cwnd should be reduced, but not by maximum.
  EXPECT_GT(act_cwnd, max_reduced_cwnd);
  EXPECT_LT(act_cwnd, fcwnd);
  EXPECT_EQ(this->response_.inter_packet_gap, 0);
  const auto rtt = this->event_.timestamp_4 - this->event_.timestamp_1;
  uint32_t exp_rtt_state =
      this->event_.rtt_state * config.rtt_smoothing_alpha() +
      rtt * (1 - config.rtt_smoothing_alpha());
  uint32_t exp_rto = std::max(
      config.min_retransmit_timeout(),
      static_cast<uint32_t>(exp_rtt_state * config.retransmit_timeout_scalar() +
                            this->response_.inter_packet_gap *
                                config.ipg_time_scalar()));
  EXPECT_EQ(this->response_.retransmit_timeout, exp_rto);
  EXPECT_EQ(this->response_.fabric_window_time_marker, new_now);  // updated
  EXPECT_EQ(this->response_.event_queue_select, 0);
  EXPECT_EQ(this->response_.delay_select, falcon::DelaySelect::kForward);
  EXPECT_EQ(this->response_.base_delay, this->event_.base_delay);
  EXPECT_EQ(this->response_.delay_state,
            this->event_.timestamp_2 - this->event_.timestamp_1);
  EXPECT_NEAR(this->response_.rtt_state, exp_rtt_state, 1);
  EXPECT_EQ(this->response_.cc_opaque, this->event_.cc_opaque);
}

// Tests the case of NACK where the congestion window is increased based on
// delay.
TYPED_TEST(SwiftTypedTest, ProcessNackIncreaseReactToDelay) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;
  auto config = MakeConfig<EventT, ResponseT>(100);
  config.set_max_decrease_on_eack_nack_drop(false);
  config.set_max_flow_scaling(1);
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<SwiftTyped> swift,
                       SwiftTyped::Create(config));

  const auto new_now = 100020;

  const double fcwnd = 32;
  this->event_.connection_id = 1234;
  this->event_.multipath_enable = false;
  this->event_.event_type = falcon::RueEventType::kNack;
  this->event_.timestamp_1 = 100000;
  this->event_.timestamp_2 = 100010;
  this->event_.timestamp_3 = 100011;
  this->event_.timestamp_4 = new_now;
  this->event_.retransmit_count = 0;
  this->event_.retransmit_reason = falcon::RetransmitReason::kTimeout;
  this->event_.nack_code = falcon::NackCode::kReserved;
  this->event_.forward_hops = 0;
  this->event_.cc_metadata = 0;
  this->event_.fabric_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(fcwnd,
                                                 falcon_rue::kFractionalBits);
  this->event_.num_packets_acked = 3;
  this->event_.event_queue_select = 0;
  this->event_.delay_select = falcon::DelaySelect::kForward;
  this->event_.fabric_window_time_marker = 99999;
  this->event_.base_delay = SwiftTyped::MakeBaseDelayField(
      /*profile_index=*/kSwiftDefaultProfileIndex);
  this->event_.delay_state = 150;
  this->event_.rtt_state = 1000;
  this->event_.cc_opaque = 3;
  this->event_.gen_bit = 0;

  swift->Process(this->event_, this->response_, new_now);

  EXPECT_EQ(this->response_.connection_id, this->event_.connection_id);
  EXPECT_EQ((IsResponseRepath<EventT, ResponseT>(this->response_)), false);
  EXPECT_EQ(this->response_.cc_metadata, 0);
  const double act_cwnd = falcon_rue::FixedToFloat<uint32_t, double>(
      this->response_.fabric_congestion_window, falcon_rue::kFractionalBits);
  // forward delay==11 < target, so cwnd should be increased.
  EXPECT_GT(act_cwnd, fcwnd);
  EXPECT_EQ(this->response_.inter_packet_gap, 0);
  const auto rtt = this->event_.timestamp_4 - this->event_.timestamp_1;
  uint32_t exp_rtt_state =
      this->event_.rtt_state * config.rtt_smoothing_alpha() +
      rtt * (1 - config.rtt_smoothing_alpha());
  uint32_t exp_rto = std::max(
      config.min_retransmit_timeout(),
      static_cast<uint32_t>(exp_rtt_state * config.retransmit_timeout_scalar() +
                            this->response_.inter_packet_gap *
                                config.ipg_time_scalar()));
  EXPECT_EQ(this->response_.retransmit_timeout, exp_rto);
  EXPECT_EQ(this->response_.event_queue_select, 0);
  EXPECT_EQ(this->response_.delay_select, falcon::DelaySelect::kForward);
  EXPECT_EQ(this->response_.base_delay, this->event_.base_delay);
  EXPECT_EQ(this->response_.delay_state,
            this->event_.timestamp_2 - this->event_.timestamp_1);
  EXPECT_NEAR(this->response_.rtt_state, exp_rtt_state, 1);
  EXPECT_EQ(this->response_.cc_opaque, this->event_.cc_opaque);
}

// Tests the case of retransmit where the congestion window decrease is not
// performed because a decrease has occurred too recently.
TYPED_TEST(SwiftTypedTest, ProcessRetransmitNoDecr) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;
  const auto config = MakeConfig<EventT, ResponseT>(100);
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<SwiftTyped> swift,
                       SwiftTyped::Create(config));

  this->event_.connection_id = 1234;
  this->event_.multipath_enable = false;
  this->event_.event_type = falcon::RueEventType::kRetransmit;
  this->event_.timestamp_1 = 0;
  this->event_.timestamp_2 = 0;
  this->event_.timestamp_3 = 0;
  this->event_.timestamp_4 = 0;
  this->event_.retransmit_count = 1;
  this->event_.retransmit_reason = falcon::RetransmitReason::kTimeout;
  this->event_.nack_code = falcon::NackCode::kNotANack;
  this->event_.forward_hops = 5;
  this->event_.cc_metadata = 0;
  this->event_.fabric_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(2.75,
                                                 falcon_rue::kFractionalBits);
  this->event_.num_packets_acked = 3;
  this->event_.event_queue_select = 0;
  this->event_.delay_select = falcon::DelaySelect::kForward;
  this->event_.fabric_window_time_marker = 101000;
  this->event_.base_delay = SwiftTyped::MakeBaseDelayField(
      /*profile_index=*/kSwiftDefaultProfileIndex);
  this->event_.delay_state = 150;
  this->event_.rtt_state = 1200;
  this->event_.cc_opaque = 3;
  this->event_.gen_bit = 0;

  swift->Process(this->event_, this->response_, 101000);

  EXPECT_EQ(this->response_.connection_id, this->event_.connection_id);
  EXPECT_EQ((IsResponseRepath<EventT, ResponseT>(this->response_)), false);
  EXPECT_EQ(this->response_.cc_metadata, 0);
  EXPECT_EQ(this->event_.fabric_congestion_window,
            this->response_.fabric_congestion_window);
  EXPECT_EQ(this->response_.inter_packet_gap, 0);
  EXPECT_GE(this->response_.retransmit_timeout,
            config.min_retransmit_timeout());
  EXPECT_EQ(this->response_.fabric_window_time_marker,
            this->event_.fabric_window_time_marker);  // no change
  EXPECT_EQ(this->response_.event_queue_select, 0);
  EXPECT_EQ(this->response_.delay_select, falcon::DelaySelect::kForward);
  EXPECT_EQ(this->response_.base_delay, this->event_.base_delay);
  EXPECT_EQ(this->response_.delay_state,
            this->event_.delay_state);                           // no change
  EXPECT_EQ(this->response_.rtt_state, this->event_.rtt_state);  // no change
  EXPECT_EQ(this->response_.cc_opaque, this->event_.cc_opaque);
}

// Tests the case of retransmit where the congestion window decrease is
// performed because it is the first retransmission.
TYPED_TEST(SwiftTypedTest, ProcessRetransmitFirst) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;
  const auto config = MakeConfig<EventT, ResponseT>(100);
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<SwiftTyped> swift,
                       SwiftTyped::Create(config));

  const auto new_now = 103000;

  const double fcwnd = 2.75;
  this->event_.connection_id = 1234;
  this->event_.multipath_enable = false;
  this->event_.event_type = falcon::RueEventType::kRetransmit;
  this->event_.timestamp_1 = 0;
  this->event_.timestamp_2 = 0;
  this->event_.timestamp_3 = 0;
  this->event_.timestamp_4 = 0;
  this->event_.retransmit_count = 1;
  this->event_.retransmit_reason = falcon::RetransmitReason::kTimeout;
  this->event_.nack_code = falcon::NackCode::kNotANack;
  this->event_.forward_hops = 5;
  this->event_.cc_metadata = 0;
  this->event_.fabric_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(fcwnd,
                                                 falcon_rue::kFractionalBits);
  this->event_.num_packets_acked = 3;
  this->event_.event_queue_select = 0;
  this->event_.delay_select = falcon::DelaySelect::kForward;
  this->event_.fabric_window_time_marker = 101000;
  this->event_.base_delay = SwiftTyped::MakeBaseDelayField(
      /*profile_index=*/kSwiftDefaultProfileIndex);
  this->event_.delay_state = 150;
  this->event_.rtt_state =
      1200;  // smaller than now - fabric_window_time_marker
  this->event_.cc_opaque = 3;
  this->event_.gen_bit = 0;

  swift->Process(this->event_, this->response_, new_now);

  EXPECT_EQ(this->response_.connection_id, this->event_.connection_id);
  EXPECT_EQ((IsResponseRepath<EventT, ResponseT>(this->response_)), false);
  EXPECT_EQ(this->response_.cc_metadata, 0);
  const double exp_cwnd = fcwnd * config.max_fabric_multiplicative_decrease();
  const double act_cwnd = falcon_rue::FixedToFloat<uint32_t, double>(
      this->response_.fabric_congestion_window, falcon_rue::kFractionalBits);
  EXPECT_NEAR(act_cwnd, exp_cwnd, exp_cwnd * 0.01);  // updated
  EXPECT_EQ(this->response_.inter_packet_gap, 0);
  EXPECT_GE(this->response_.retransmit_timeout,
            config.min_retransmit_timeout());
  EXPECT_EQ(this->response_.fabric_window_time_marker,
            new_now);  // updated
  EXPECT_EQ(this->response_.event_queue_select, 0);
  EXPECT_EQ(this->response_.delay_select, falcon::DelaySelect::kForward);
  EXPECT_EQ(this->response_.base_delay, this->event_.base_delay);
  EXPECT_EQ(this->response_.delay_state,
            this->event_.delay_state);                           // no change
  EXPECT_EQ(this->response_.rtt_state, this->event_.rtt_state);  // no change
  EXPECT_EQ(this->response_.cc_opaque, this->event_.cc_opaque);
}

// Tests the case of retransmit where the congestion window decrease is
// not performed because it is the second retransmission.
TYPED_TEST(SwiftTypedTest, ProcessRetransmitSecond) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;
  const auto config = MakeConfig<EventT, ResponseT>(100);
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<SwiftTyped> swift,
                       SwiftTyped::Create(config));

  const auto new_now = 10300;

  this->event_.connection_id = 1234;
  this->event_.multipath_enable = false;
  this->event_.event_type = falcon::RueEventType::kRetransmit;
  this->event_.timestamp_1 = 0;
  this->event_.timestamp_2 = 0;
  this->event_.timestamp_3 = 0;
  this->event_.timestamp_4 = 0;
  this->event_.retransmit_count = 2;
  this->event_.retransmit_reason = falcon::RetransmitReason::kTimeout;
  this->event_.nack_code = falcon::NackCode::kNotANack;
  this->event_.forward_hops = 5;
  this->event_.cc_metadata = 0;
  this->event_.fabric_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(2.75,
                                                 falcon_rue::kFractionalBits);
  this->event_.num_packets_acked = 3;
  this->event_.event_queue_select = 0;
  this->event_.delay_select = falcon::DelaySelect::kForward;
  this->event_.fabric_window_time_marker = 101000;
  this->event_.base_delay = SwiftTyped::MakeBaseDelayField(
      /*profile_index=*/kSwiftDefaultProfileIndex);
  this->event_.delay_state = 150;
  this->event_.rtt_state =
      1200;  // smaller than now - fabric_window_time_marker
  this->event_.cc_opaque = 3;
  this->event_.gen_bit = 0;

  swift->Process(this->event_, this->response_, new_now);

  EXPECT_EQ(this->response_.connection_id, this->event_.connection_id);
  EXPECT_EQ((IsResponseRepath<EventT, ResponseT>(this->response_)), false);
  EXPECT_EQ(this->response_.cc_metadata, 0);
  EXPECT_EQ(this->event_.fabric_congestion_window,
            this->response_.fabric_congestion_window);
  EXPECT_EQ(this->response_.inter_packet_gap, 0);
  EXPECT_GE(this->response_.retransmit_timeout,
            config.min_retransmit_timeout());
  EXPECT_EQ(this->response_.fabric_window_time_marker,
            new_now - this->event_.rtt_state);  // tail RTT
  EXPECT_EQ(this->response_.event_queue_select, 0);
  EXPECT_EQ(this->response_.delay_select, falcon::DelaySelect::kForward);
  EXPECT_EQ(this->response_.base_delay, this->event_.base_delay);
  EXPECT_EQ(this->response_.delay_state,
            this->event_.delay_state);                           // no change
  EXPECT_EQ(this->response_.rtt_state, this->event_.rtt_state);  // no change
  EXPECT_EQ(this->response_.cc_opaque, this->event_.cc_opaque);
}

// Tests the case of retransmit where the congestion window decrease is
// performed because it is the last retransmission. The congestion is set
// to the minimum congestion window value.
TYPED_TEST(SwiftTypedTest, ProcessRetransmitLimit) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;
  const auto config = MakeConfig<EventT, ResponseT>(100);
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<SwiftTyped> swift,
                       SwiftTyped::Create(config));

  const auto new_now = 103000;

  this->event_.connection_id = 1234;
  this->event_.multipath_enable = false;
  this->event_.event_type = falcon::RueEventType::kRetransmit;
  this->event_.timestamp_1 = 0;
  this->event_.timestamp_2 = 0;
  this->event_.timestamp_3 = 0;
  this->event_.timestamp_4 = 0;
  this->event_.retransmit_count = 4;
  this->event_.retransmit_reason = falcon::RetransmitReason::kTimeout;
  this->event_.nack_code = falcon::NackCode::kNotANack;
  this->event_.forward_hops = 5;
  this->event_.cc_metadata = 0;
  this->event_.fabric_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(2.75,
                                                 falcon_rue::kFractionalBits);
  this->event_.num_packets_acked = 3;
  this->event_.event_queue_select = 0;
  this->event_.delay_select = falcon::DelaySelect::kForward;
  this->event_.fabric_window_time_marker = 101000;
  this->event_.base_delay = SwiftTyped::MakeBaseDelayField(
      /*profile_index=*/kSwiftDefaultProfileIndex);
  this->event_.delay_state = 150;
  this->event_.rtt_state = 1200;
  this->event_.cc_opaque = 3;
  this->event_.gen_bit = 0;

  swift->Process(this->event_, this->response_, new_now);

  EXPECT_EQ(this->response_.connection_id, this->event_.connection_id);
  EXPECT_EQ((IsResponseRepath<EventT, ResponseT>(this->response_)), false);
  EXPECT_EQ(this->response_.cc_metadata, 0);
  uint32_t exp_cwnd = falcon_rue::FloatToFixed<double, uint32_t>(
      config.min_fabric_congestion_window(), falcon_rue::kFractionalBits);
  EXPECT_EQ(this->response_.fabric_congestion_window, exp_cwnd);
  double exp_ipg = std::round(
      (this->event_.rtt_state / config.min_fabric_congestion_window() -
       this->event_.rtt_state) *
      config.ipg_time_scalar());
  EXPECT_EQ(this->response_.inter_packet_gap, static_cast<uint64_t>(exp_ipg));
  EXPECT_NEAR(this->response_.retransmit_timeout,
              std::min<uint32_t>(
                  falcon_rue::ValidMask<uint32_t>(falcon_rue::kTimeBits),
                  this->event_.rtt_state * config.retransmit_timeout_scalar() +
                      exp_ipg / config.ipg_time_scalar()),
              1);
  EXPECT_EQ(this->response_.fabric_window_time_marker, new_now);  // updated
  EXPECT_EQ(this->response_.event_queue_select, 0);
  EXPECT_EQ(this->response_.delay_select, falcon::DelaySelect::kForward);
  EXPECT_EQ(this->response_.base_delay, this->event_.base_delay);
  EXPECT_EQ(this->response_.delay_state,
            this->event_.delay_state);                           // no change
  EXPECT_EQ(this->response_.rtt_state, this->event_.rtt_state);  // no change
  EXPECT_EQ(this->response_.cc_opaque, this->event_.cc_opaque);
}

// Tests the case of EACK drop where the max congestion window decrease is
// performed.
TYPED_TEST(SwiftTypedTest, ProcessEackDropMaxDecrease) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;
  SwiftConfiguration config = MakeConfig<EventT, ResponseT>(100);
  config.set_max_decrease_on_eack_nack_drop(true);
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<SwiftTyped> swift,
                       SwiftTyped::Create(config));

  const auto new_now = 100200;

  const double fcwnd = 32;
  this->event_.connection_id = 1234;
  this->event_.multipath_enable = false;
  this->event_.event_type = falcon::RueEventType::kAck;
  this->event_.eack = true;
  this->event_.eack_drop = true;
  this->event_.timestamp_1 = 100000;
  this->event_.timestamp_2 = 100110;
  this->event_.timestamp_3 = 100111;
  this->event_.timestamp_4 = new_now;
  this->event_.retransmit_count = 0;
  this->event_.retransmit_reason = falcon::RetransmitReason::kTimeout;
  this->event_.nack_code = falcon::NackCode::kNotANack;
  this->event_.forward_hops = 5;
  this->event_.cc_metadata = 0;
  this->event_.fabric_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(fcwnd,
                                                 falcon_rue::kFractionalBits);
  this->event_.num_packets_acked = 3;
  this->event_.event_queue_select = 0;
  this->event_.delay_select = falcon::DelaySelect::kForward;
  this->event_.fabric_window_time_marker = 99999;
  this->event_.base_delay = SwiftTyped::MakeBaseDelayField(
      /*profile_index=*/kSwiftDefaultProfileIndex);
  this->event_.delay_state = 150;
  this->event_.rtt_state = 1000;
  this->event_.cc_opaque = 3;
  this->event_.gen_bit = 0;

  swift->Process(this->event_, this->response_, new_now);

  EXPECT_EQ(this->response_.connection_id, this->event_.connection_id);
  EXPECT_EQ((IsResponseRepath<EventT, ResponseT>(this->response_)), false);
  EXPECT_EQ(this->response_.cc_metadata, 0);
  const double exp_cwnd = fcwnd * config.max_fabric_multiplicative_decrease();
  const double act_cwnd = falcon_rue::FixedToFloat<uint32_t, double>(
      this->response_.fabric_congestion_window, falcon_rue::kFractionalBits);
  EXPECT_NEAR(act_cwnd, exp_cwnd, exp_cwnd * 0.01);
  EXPECT_EQ(this->response_.inter_packet_gap, 0);
  const auto rtt = this->event_.timestamp_4 - this->event_.timestamp_1;
  uint32_t exp_rtt_state =
      this->event_.rtt_state * config.rtt_smoothing_alpha() +
      rtt * (1 - config.rtt_smoothing_alpha());
  uint32_t exp_rto = std::max(
      config.min_retransmit_timeout(),
      static_cast<uint32_t>(exp_rtt_state * config.retransmit_timeout_scalar() +
                            this->response_.inter_packet_gap *
                                config.ipg_time_scalar()));
  EXPECT_EQ(this->response_.retransmit_timeout, exp_rto);
  EXPECT_EQ(this->response_.fabric_window_time_marker, new_now);  // updated
  EXPECT_EQ(this->response_.event_queue_select, 0);
  EXPECT_EQ(this->response_.delay_select, falcon::DelaySelect::kForward);
  EXPECT_EQ(this->response_.base_delay, this->event_.base_delay);
  EXPECT_EQ(this->response_.delay_state,
            this->event_.timestamp_2 - this->event_.timestamp_1);
  EXPECT_NEAR(this->response_.rtt_state, exp_rtt_state, 1);
  EXPECT_EQ(this->response_.cc_opaque, this->event_.cc_opaque);
}

// Tests the case of EACK drop where the max congestion window decrease is not
// performed because a decrease has occurred too recently.
TYPED_TEST(SwiftTypedTest, ProcessEackDropNoMaxDecrease) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;
  SwiftConfiguration config = MakeConfig<EventT, ResponseT>(100);
  config.set_max_decrease_on_eack_nack_drop(true);
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<SwiftTyped> swift,
                       SwiftTyped::Create(config));

  const auto new_now = 100200;

  this->event_.connection_id = 1234;
  this->event_.multipath_enable = false;
  this->event_.event_type = falcon::RueEventType::kAck;
  this->event_.eack = true;
  this->event_.eack_drop = true;
  this->event_.timestamp_1 = 100000;
  this->event_.timestamp_2 = 100110;
  this->event_.timestamp_3 = 100111;
  this->event_.timestamp_4 = new_now;
  this->event_.retransmit_count = 0;
  this->event_.retransmit_reason = falcon::RetransmitReason::kTimeout;
  this->event_.nack_code = falcon::NackCode::kNotANack;
  this->event_.forward_hops = 5;
  this->event_.cc_metadata = 0;
  this->event_.fabric_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(32,
                                                 falcon_rue::kFractionalBits);
  this->event_.num_packets_acked = 3;
  this->event_.event_queue_select = 0;
  this->event_.delay_select = falcon::DelaySelect::kForward;
  this->event_.fabric_window_time_marker = 100100;
  this->event_.base_delay = SwiftTyped::MakeBaseDelayField(
      /*profile_index=*/kSwiftDefaultProfileIndex);
  this->event_.delay_state = 150;
  this->event_.rtt_state = 1000;
  this->event_.cc_opaque = 3;
  this->event_.gen_bit = 0;

  swift->Process(this->event_, this->response_, new_now);

  EXPECT_EQ(this->response_.connection_id, this->event_.connection_id);
  EXPECT_EQ((IsResponseRepath<EventT, ResponseT>(this->response_)), false);
  EXPECT_EQ(this->response_.cc_metadata, 0);
  EXPECT_EQ(this->event_.fabric_congestion_window,
            this->response_.fabric_congestion_window);
  EXPECT_EQ(this->response_.inter_packet_gap, 0);
  const auto rtt = this->event_.timestamp_4 - this->event_.timestamp_1;
  uint32_t exp_rtt_state =
      this->event_.rtt_state * config.rtt_smoothing_alpha() +
      rtt * (1 - config.rtt_smoothing_alpha());
  uint32_t exp_rto = std::max(
      config.min_retransmit_timeout(),
      static_cast<uint32_t>(exp_rtt_state * config.retransmit_timeout_scalar() +
                            this->response_.inter_packet_gap *
                                config.ipg_time_scalar()));
  EXPECT_EQ(this->response_.retransmit_timeout, exp_rto);
  EXPECT_EQ(this->response_.fabric_window_time_marker,
            this->event_.fabric_window_time_marker);  // no change
  EXPECT_EQ(this->response_.event_queue_select, 0);
  EXPECT_EQ(this->response_.delay_select, falcon::DelaySelect::kForward);
  EXPECT_EQ(this->response_.base_delay, this->event_.base_delay);
  EXPECT_EQ(this->response_.delay_state,
            this->event_.timestamp_2 - this->event_.timestamp_1);
  EXPECT_NEAR(this->response_.rtt_state, exp_rtt_state, 1);
  EXPECT_EQ(this->response_.cc_opaque, this->event_.cc_opaque);
}

// Tests that the flow ID field in the response is calculated correctly from the
// flow label field in the event.
TYPED_TEST(SwiftTypedTest, GetFlowId) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;
  const auto config = MakeConfig<EventT, ResponseT>(100);
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<SwiftTyped> swift,
                       SwiftTyped::Create(config));

  const auto new_now = 100200;

  this->event_.connection_id = 1234;
  this->event_.multipath_enable = false;
  this->event_.flow_label = 0x12;
  this->event_.event_type = falcon::RueEventType::kAck;
  this->event_.timestamp_1 = 100000;
  this->event_.timestamp_2 = 100879;
  this->event_.timestamp_3 = 101000;
  this->event_.timestamp_4 = 101200;
  this->event_.retransmit_count = 0;
  this->event_.retransmit_reason = falcon::RetransmitReason::kTimeout;
  this->event_.nack_code = falcon::NackCode::kNotANack;
  this->event_.forward_hops = 5;
  this->event_.cc_metadata = 0;
  this->event_.fabric_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(2.75,
                                                 falcon_rue::kFractionalBits);
  this->event_.num_packets_acked = 3;
  this->event_.event_queue_select = 0;
  this->event_.delay_select = falcon::DelaySelect::kForward;
  this->event_.fabric_window_time_marker = 101000;
  this->event_.base_delay = SwiftTyped::MakeBaseDelayField(
      /*profile_index=*/kSwiftDefaultProfileIndex);
  this->event_.delay_state = 150;
  this->event_.rtt_state = 1000;
  this->event_.cc_opaque = 3;
  this->event_.gen_bit = 0;

  // Since this connection is not multipath-enabled, the flow ID should be 0 no
  // matter what the flow ID is.
  swift->Process(this->event_, this->response_, new_now);
  EXPECT_EQ(this->response_.connection_id, this->event_.connection_id);
  EXPECT_EQ(this->response_.flow_id, 0);

  // Since this connection is not multipath-enabled, the flow ID should be 0 no
  // matter what the flow ID is.
  this->event_.flow_label = 0x13;
  swift->Process(this->event_, this->response_, new_now);
  EXPECT_EQ(this->response_.connection_id, this->event_.connection_id);
  EXPECT_EQ(this->response_.flow_id, 0);

  // For multipath-enabled connections, the flow_id in the response should be
  // the last 2 bits in the flow label.
  this->event_.connection_id = 1235;
  this->event_.multipath_enable = true;
  this->event_.flow_label = 0x11;
  swift->Process(this->event_, this->response_, new_now);
  EXPECT_EQ(this->response_.connection_id, this->event_.connection_id);
  EXPECT_EQ(this->response_.flow_id, 1);

  this->event_.connection_id = 1235;
  this->event_.flow_label = 0x12;
  swift->Process(this->event_, this->response_, new_now);
  EXPECT_EQ(this->response_.connection_id, this->event_.connection_id);
  EXPECT_EQ(this->response_.flow_id, 2);
}

// Tests that the generated flow label has the right flow ID as the two least
// significant bits.
TYPED_TEST(SwiftTypedTest, GenerateFlowLabel) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;
  const auto config = MakeConfig<EventT, ResponseT>(100);
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<SwiftTyped> swift,
                       SwiftTyped::Create(config));
  for (int flow_id = 0; flow_id < 4; flow_id++) {
    uint32_t generated_flow_label = swift->RandomFlowLabel(flow_id);
    // The last kFlowIdMask bits in the flow label should be the flow ID.
    EXPECT_EQ(generated_flow_label & kFlowIdMask, flow_id);
    // The generated flow label should be >= 1
    EXPECT_GE(generated_flow_label, 1);
    // The generated flow label should be at most kFlowLabelBits long.
    falcon_rue::CheckBits(falcon_rue::kFlowLabelBits, generated_flow_label);
  }
}

// Tests that delay_state is properly maintained even when randomize_path is
// enabled.
TYPED_TEST(SwiftTypedTest, DelayState) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;
  const double delay_smoothing_alpha = 0.4;

  SwiftConfiguration config = MakeConfig<EventT, ResponseT>(100);
  config.set_randomize_path(true);
  config.set_delay_smoothing_alpha(delay_smoothing_alpha);
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<SwiftTyped> swift,
                       SwiftTyped::Create(config));

  auto now = 102200;

  this->event_.connection_id = 1234;
  this->event_.multipath_enable = false;
  this->event_.event_type = falcon::RueEventType::kAck;
  this->event_.timestamp_1 = 100000;
  this->event_.timestamp_2 = 100879;
  this->event_.timestamp_3 = 101000;
  this->event_.timestamp_4 = 101200;
  this->event_.retransmit_count = 0;
  this->event_.retransmit_reason = falcon::RetransmitReason::kTimeout;
  this->event_.nack_code = falcon::NackCode::kNotANack;
  this->event_.forward_hops = 5;
  this->event_.cc_metadata = 0;
  this->event_.fabric_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(64,
                                                 falcon_rue::kFractionalBits);
  this->event_.nic_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(64,
                                                 falcon_rue::kFractionalBits);
  this->event_.num_packets_acked = 40;
  this->event_.event_queue_select = 0;
  this->event_.delay_select = falcon::DelaySelect::kForward;
  this->event_.fabric_window_time_marker = 101000;
  this->event_.base_delay = SwiftTyped::MakeBaseDelayField(
      /*profile_index=*/kSwiftDefaultProfileIndex);
  this->event_.delay_state = 150;
  this->event_.plb_state = 0;
  this->event_.rtt_state = 1000;
  this->event_.cc_opaque = 0;
  this->event_.gen_bit = 0;

  falcon_rue::PacketTiming timing = falcon_rue::GetPacketTiming(this->event_);
  uint32_t smoothed_delay_expected = SwiftTyped::GetSmoothed(
      delay_smoothing_alpha, this->event_.delay_state, timing.delay);
  swift->Process(this->event_, this->response_, now);
  EXPECT_EQ(smoothed_delay_expected, this->response_.delay_state);

  this->event_.delay_state = this->response_.delay_state;
  smoothed_delay_expected = SwiftTyped::GetSmoothed(
      delay_smoothing_alpha, this->event_.delay_state, timing.delay);
  swift->Process(this->event_, this->response_, now);
  EXPECT_EQ(smoothed_delay_expected, this->response_.delay_state);
}

// Tests PLB implementation.
TYPED_TEST(SwiftTypedTest, PlbTest) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;
  const uint32_t plb_attempt_threshold = 5;
  SwiftConfiguration config = MakeConfig<EventT, ResponseT>(100);
  config.set_randomize_path(true);
  config.set_max_fabric_congestion_window(64);
  config.set_max_nic_congestion_window(64);
  config.set_plb_attempt_threshold(plb_attempt_threshold);
  config.set_plb_congestion_threshold(0.5);
  config.set_plb_target_rtt_multiplier(1.2);
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<SwiftTyped> swift,
                       SwiftTyped::Create(config));

  auto now = 102200;
  const auto long_delay_timestamp = 100879;
  const auto short_delay_timestamp = 100100;

  this->event_.connection_id = 1234;
  this->event_.multipath_enable = false;
  this->event_.event_type = falcon::RueEventType::kAck;
  this->event_.timestamp_1 = 100000;
  this->event_.timestamp_2 = long_delay_timestamp;
  this->event_.timestamp_3 = 101000;
  this->event_.timestamp_4 = 101200;
  this->event_.retransmit_count = 0;
  this->event_.retransmit_reason = falcon::RetransmitReason::kTimeout;
  this->event_.nack_code = falcon::NackCode::kNotANack;
  this->event_.forward_hops = 5;
  this->event_.cc_metadata = 0;
  this->event_.fabric_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(64,
                                                 falcon_rue::kFractionalBits);
  this->event_.nic_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(64,
                                                 falcon_rue::kFractionalBits);
  this->event_.num_packets_acked = 40;
  this->event_.event_queue_select = 0;
  this->event_.delay_select = falcon::DelaySelect::kForward;
  this->event_.fabric_window_time_marker = 101000;
  this->event_.base_delay = SwiftTyped::MakeBaseDelayField(
      /*profile_index=*/kSwiftDefaultProfileIndex);
  this->event_.delay_state = 0;
  this->event_.plb_state = 0;
  this->event_.rtt_state = 1000;
  this->event_.cc_opaque = 0;  // don't set bit zero (kRandomizePathEnableMask)
  this->event_.gen_bit = 0;

  swift->Process(this->event_, this->response_, now);

  PlbState plb_states;

  // 40 Acked with big fabric delay.
  EXPECT_EQ(this->response_.connection_id, this->event_.connection_id);
  EXPECT_EQ((IsResponseRepath<EventT, ResponseT>(this->response_)), false);
  plb_states.value = this->response_.plb_state;
  EXPECT_EQ(plb_states.packets_acknowledged, 40);
  EXPECT_EQ(plb_states.plb_reroute_attempted, 0);
  EXPECT_EQ(plb_states.packets_congestion_acknowledged, 40);

  // 25 Acked with small fabric delay. Should increment plb_reroute_attempted as
  // 40/(40+25) > 0.5.
  // This also tests no-integer-overflow, as 40+25=65 > 6bits can represent.
  this->event_.num_packets_acked = 25;
  this->event_.timestamp_2 = short_delay_timestamp;
  this->event_.plb_state = plb_states.value;
  swift->Process(this->event_, this->response_, now);
  plb_states.value = this->response_.plb_state;
  EXPECT_EQ((IsResponseRepath<EventT, ResponseT>(this->response_)), false);
  EXPECT_EQ(plb_states.packets_acknowledged, 0);
  EXPECT_EQ(plb_states.plb_reroute_attempted, 1);
  EXPECT_EQ(plb_states.packets_congestion_acknowledged, 0);

  // 70 Acked with small fabric delay (70 > 64=fcwnd). Should clear
  // plb_reroute_attempted.
  this->event_.num_packets_acked = 70;
  this->event_.timestamp_2 = short_delay_timestamp;
  this->event_.plb_state = plb_states.value;
  swift->Process(this->event_, this->response_, now);
  plb_states.value = this->response_.plb_state;
  EXPECT_EQ((IsResponseRepath<EventT, ResponseT>(this->response_)), false);
  EXPECT_EQ(plb_states.packets_acknowledged, 0);
  EXPECT_EQ(plb_states.plb_reroute_attempted, 0);
  EXPECT_EQ(plb_states.packets_congestion_acknowledged, 0);

  // For consecutive plb_attempt_threshold rounds.
  for (int i = 0; i < plb_attempt_threshold; i++) {
    // 70 Acked with big fabric delay. Each round should increment
    // plb_reroute_attempted.
    this->event_.num_packets_acked = 70;  // 70 > 64=fcwnd
    this->event_.timestamp_2 = long_delay_timestamp;
    this->event_.plb_state = plb_states.value;
    swift->Process(this->event_, this->response_, now);
    plb_states.value = this->response_.plb_state;
    // The last round should randomize flow_label_1.
    EXPECT_EQ(this->response_.flow_label_1_valid,
              i + 1 == plb_attempt_threshold);
    EXPECT_EQ(plb_states.packets_acknowledged, 0);
    // The last round should clear plb_reroute_attempted.
    EXPECT_EQ(plb_states.plb_reroute_attempted,
              i + 1 == plb_attempt_threshold ? 0 : i + 1);
    EXPECT_EQ(plb_states.packets_congestion_acknowledged, 0);
  }

  // 33 Acked with big fabric delay.
  this->event_.num_packets_acked = 33;
  this->event_.timestamp_2 = long_delay_timestamp;
  this->event_.plb_state = plb_states.value;
  swift->Process(this->event_, this->response_, now);
  plb_states.value = this->response_.plb_state;
  EXPECT_EQ((IsResponseRepath<EventT, ResponseT>(this->response_)), false);
  EXPECT_EQ(plb_states.packets_acknowledged, 33);
  EXPECT_EQ(plb_states.plb_reroute_attempted, 0);
  EXPECT_EQ(plb_states.packets_congestion_acknowledged, 33);

  // 34 Acked with small fabric delay. Should clear plb_reroute_attempted as
  // 33/(33+34) < 0.5.
  this->event_.num_packets_acked = 34;
  this->event_.timestamp_2 = short_delay_timestamp;
  this->event_.plb_state = plb_states.value;
  swift->Process(this->event_, this->response_, now);
  plb_states.value = this->response_.plb_state;
  EXPECT_EQ((IsResponseRepath<EventT, ResponseT>(this->response_)), false);
  EXPECT_EQ(plb_states.packets_acknowledged, 0);
  EXPECT_EQ(plb_states.plb_reroute_attempted, 0);
  EXPECT_EQ(plb_states.packets_congestion_acknowledged, 0);
}

// Tests path randomize flag in cc_opaque is not supported in Gen_2.
TYPED_TEST(SwiftTypedTest, ProcessAckRandomizePathBitNotSupported) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;

  SwiftConfiguration config = MakeConfig<EventT, ResponseT>(100);
  config.set_randomize_path(true);
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<SwiftTyped> swift,
                       SwiftTyped::Create(config));

  this->event_.connection_id = 1234;
  this->event_.multipath_enable = false;
  this->event_.flow_label = 0x10;
  this->event_.event_type = falcon::RueEventType::kAck;
  this->event_.timestamp_1 = 100000;
  this->event_.timestamp_2 = 100879;
  this->event_.timestamp_3 = 101000;
  this->event_.timestamp_4 = 101200;
  this->event_.retransmit_count = 0;
  this->event_.retransmit_reason = falcon::RetransmitReason::kTimeout;
  this->event_.nack_code = falcon::NackCode::kNotANack;
  this->event_.forward_hops = 5;
  this->event_.cc_metadata = 0;
  this->event_.fabric_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(2.75,
                                                 falcon_rue::kFractionalBits);

  this->event_.num_packets_acked = 3;
  this->event_.event_queue_select = 0;
  this->event_.delay_select = falcon::DelaySelect::kForward;
  this->event_.fabric_window_time_marker = 101000;
  this->event_.base_delay = SwiftTyped::MakeBaseDelayField(
      /*profile_index=*/kSwiftDefaultProfileIndex);
  this->event_.delay_state = 150;
  this->event_.rtt_state = 1000;
  this->event_.cc_opaque =
      kRandomizePathEnableMask;  // set the bit that would set the
                                 // randomize flag in Gen_1
  this->event_.gen_bit = 0;

  swift->Process(this->event_, this->response_, 102000);

  EXPECT_EQ(this->response_.connection_id, this->event_.connection_id);
  // Unlike Gen_1, Gen_2 does not support forcing a randomize_path by setting a
  // bit in cc_opaque.
  EXPECT_EQ((IsResponseRepath<EventT, ResponseT>(this->response_)), false);
}

// Tests the case of ACK additive increment on the nic congestion window no
// matter what the ncwnd time marker value is (like fcwnd).
TYPED_TEST(SwiftTypedTest, NicCongestionWindowIncrease) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;
  SwiftConfiguration config = MakeConfig<EventT, ResponseT>(100);
  config.set_calc_rtt_smooth(false);
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<SwiftTyped> swift,
                       SwiftTyped::Create(config));
  const auto new_now = 100400;
  const double ncwnd = 100.0;
  const uint32_t event_rtt = 315;

  this->event_.connection_id = 1234;
  this->event_.multipath_enable = false;
  this->event_.event_type = falcon::RueEventType::kAck;
  this->event_.timestamp_1 = 100000;
  this->event_.timestamp_2 = 100153;
  this->event_.timestamp_3 = 100200;
  this->event_.timestamp_4 = event_rtt + this->event_.timestamp_1;
  this->event_.retransmit_count = 0;
  this->event_.retransmit_reason = falcon::RetransmitReason::kTimeout;
  this->event_.nack_code = falcon::NackCode::kNotANack;
  this->event_.forward_hops = 5;
  this->event_.rx_buffer_level = static_cast<uint8_t>(
      config.target_rx_buffer_level() - 1);  // rx_buffer_level less than target
  this->event_.cc_metadata = 0;
  this->event_.fabric_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(0.5,
                                                 falcon_rue::kFractionalBits);
  this->event_.nic_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(ncwnd,
                                                 falcon_rue::kFractionalBits);
  this->event_.num_packets_acked = 3;
  this->event_.event_queue_select = 0;
  this->event_.delay_select = falcon::DelaySelect::kForward;
  this->event_.fabric_window_time_marker = 99999;
  this->event_.nic_window_time_marker =
      new_now - event_rtt - 10;  // ncwnd time marker is more than one RTT away
  this->event_.base_delay = SwiftTyped::MakeBaseDelayField(
      /*profile_index=*/kSwiftDefaultProfileIndex);
  this->event_.delay_state = 150;
  this->event_.rtt_state = event_rtt;
  this->event_.cc_opaque = 3;
  this->event_.gen_bit = 0;

  swift->Process(this->event_, this->response_, new_now);

  uint32_t expected_ncwnd_fixed =
      GetExpectedIncreasedNcwndFixed<EventT, ResponseT>(
          ncwnd, config.nic_additive_increment_factor());
  EXPECT_EQ(this->response_.connection_id, this->event_.connection_id);
  EXPECT_EQ(this->response_.nic_congestion_window, expected_ncwnd_fixed);
  // ncwnd time marker is not changed even though the ncwnd increased because
  // the ncwnd time marker is more than one RTT away.
  EXPECT_EQ(this->response_.nic_window_time_marker, new_now - event_rtt);

  // We still expect ncwnd increases even when the ncwnd time marker is less
  // than one RTT away.
  this->event_.nic_window_time_marker =
      new_now - event_rtt + 1;  // ncwnd time marker is less than one RTT away
  swift->Process(this->event_, this->response_, new_now);
  EXPECT_EQ(this->response_.connection_id, this->event_.connection_id);
  EXPECT_EQ(this->response_.nic_congestion_window, expected_ncwnd_fixed);
  // ncwnd time marker is not changed since the ncwnd increased and the time
  // marker is less than one RTT away.
  EXPECT_EQ(this->response_.nic_window_time_marker,
            this->event_.nic_window_time_marker);
}

// Tests the ACK case of multiplicative decrease on the nic congestion window.
TYPED_TEST(SwiftTypedTest, NicCongestionWindowDecrease) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;

  const auto config = MakeConfig<EventT, ResponseT>(100);
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<SwiftTyped> swift,
                       SwiftTyped::Create(config));

  const auto new_now = 100400;
  const uint32_t event_rtt = 315;
  const double ncwnd = 100.0;

  this->event_.connection_id = 1234;
  this->event_.multipath_enable = false;
  this->event_.event_type = falcon::RueEventType::kAck;
  this->event_.timestamp_1 = 100000;
  this->event_.timestamp_2 = 100153;
  this->event_.timestamp_3 = 100200;
  this->event_.timestamp_4 = event_rtt + this->event_.timestamp_1;
  this->event_.retransmit_count = 0;
  this->event_.retransmit_reason = falcon::RetransmitReason::kTimeout;
  this->event_.nack_code = falcon::NackCode::kNotANack;
  this->event_.forward_hops = 5;
  this->event_.rx_buffer_level =
      static_cast<uint8_t>(config.target_rx_buffer_level() +
                           1),  // rx_buffer_level larger than target
      this->event_.cc_metadata = 0;
  this->event_.fabric_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(0.5,
                                                 falcon_rue::kFractionalBits);
  this->event_.nic_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(ncwnd,
                                                 falcon_rue::kFractionalBits);
  this->event_.num_packets_acked = 3;
  this->event_.event_queue_select = 0;
  this->event_.delay_select = falcon::DelaySelect::kForward;
  this->event_.fabric_window_time_marker = 99999;
  this->event_.nic_window_time_marker =
      new_now - event_rtt - 10;  // ncwnd time marker is more than one RTT away
  this->event_.base_delay = SwiftTyped::MakeBaseDelayField(
      /*profile_index=*/kSwiftDefaultProfileIndex);
  this->event_.delay_state = 150;
  this->event_.rtt_state = event_rtt;
  this->event_.cc_opaque = 3;
  this->event_.gen_bit = 0;

  swift->Process(this->event_, this->response_, new_now);

  uint32_t expected_ncwnd_fixed =
      GetExpectedDecreasedNcwndFixed<EventT, ResponseT>(
          ncwnd, config.nic_multiplicative_decrease_factor(),
          this->event_.rx_buffer_level, config.target_rx_buffer_level());
  EXPECT_EQ(this->response_.connection_id, this->event_.connection_id);
  EXPECT_EQ(this->response_.nic_congestion_window,
            expected_ncwnd_fixed);                             // updated
  EXPECT_EQ(this->response_.nic_window_time_marker, new_now);  // updated

  // Test with a different rx_buffer_level that is still larger than the target.
  this->event_.rx_buffer_level = static_cast<uint8_t>(
      config.target_rx_buffer_level() + 3),  // + 3 instead of + 1
      swift->Process(this->event_, this->response_, new_now);

  expected_ncwnd_fixed = GetExpectedDecreasedNcwndFixed<EventT, ResponseT>(
      ncwnd, config.nic_multiplicative_decrease_factor(),
      this->event_.rx_buffer_level, config.target_rx_buffer_level());
  EXPECT_EQ(this->response_.connection_id, this->event_.connection_id);
  EXPECT_EQ(this->response_.nic_congestion_window,
            expected_ncwnd_fixed);                             // updated
  EXPECT_EQ(this->response_.nic_window_time_marker, new_now);  // updated
}

// Tests the ACK case of multiplicative decrease on the nic congestion
// window. ncwnd time marker is less than one RTT away thus the ncwnd should not
// decrease.
TYPED_TEST(SwiftTypedTest, NicCongestionWindowNoDecrease) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;

  const auto config = MakeConfig<EventT, ResponseT>(100);
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<SwiftTyped> swift,
                       SwiftTyped::Create(config));

  const auto new_now = 100400;
  const uint32_t event_rtt = 315;
  const double ncwnd = 100.0;

  this->event_.connection_id = 1234;
  this->event_.multipath_enable = false;
  this->event_.event_type = falcon::RueEventType::kAck;
  this->event_.timestamp_1 = 100000;
  this->event_.timestamp_2 = 100153;
  this->event_.timestamp_3 = 100200;
  this->event_.timestamp_4 = event_rtt + this->event_.timestamp_1;  // rtt=315
  this->event_.retransmit_count = 0;
  this->event_.retransmit_reason = falcon::RetransmitReason::kTimeout;
  this->event_.nack_code = falcon::NackCode::kNotANack;
  this->event_.forward_hops = 5;
  this->event_.rx_buffer_level =
      static_cast<uint8_t>(config.target_rx_buffer_level() +
                           2),  // rx_buffer_level larger than target
      this->event_.cc_metadata = 0;
  this->event_.fabric_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(0.5,
                                                 falcon_rue::kFractionalBits);
  this->event_.nic_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(ncwnd,
                                                 falcon_rue::kFractionalBits);
  this->event_.num_packets_acked = 3;
  this->event_.event_queue_select = 0;
  this->event_.delay_select = falcon::DelaySelect::kForward;
  this->event_.fabric_window_time_marker = 99999;
  this->event_.nic_window_time_marker =
      new_now - event_rtt + 10;  // guard is within one RTT based on timestamps
  this->event_.base_delay = SwiftTyped::MakeBaseDelayField(
      /*profile_index=*/kSwiftDefaultProfileIndex);
  this->event_.delay_state = 150;
  this->event_.rtt_state = event_rtt;
  this->event_.cc_opaque = 3;
  this->event_.gen_bit = 0;

  swift->Process(this->event_, this->response_, new_now);

  EXPECT_EQ(this->response_.connection_id, this->event_.connection_id);
  EXPECT_EQ(this->response_.nic_congestion_window,
            this->event_.nic_congestion_window);  // no change
  EXPECT_EQ(this->response_.nic_window_time_marker,
            this->event_.nic_window_time_marker);  // no change
}

// Tests the case of NACK where the nic congestion window decreases after a
// decrease more than one RTT away.
TYPED_TEST(SwiftTypedTest, ProcessNackNicDecrease) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;

  const auto config = MakeConfig<EventT, ResponseT>(100);
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<SwiftTyped> swift,
                       SwiftTyped::Create(config));

  const auto new_now = 100400;
  const uint32_t event_rtt = 315;
  const double ncwnd = 100.0;

  this->event_.connection_id = 1234;
  this->event_.multipath_enable = false;
  this->event_.event_type = falcon::RueEventType::kNack;
  this->event_.timestamp_1 = 100000;
  this->event_.timestamp_2 = 100879;
  this->event_.timestamp_3 = 101000;
  this->event_.timestamp_4 = event_rtt + this->event_.timestamp_1;
  this->event_.retransmit_count = 0;
  this->event_.retransmit_reason = falcon::RetransmitReason::kTimeout;
  this->event_.nack_code = falcon::NackCode::kRxResourceExhaustion;
  this->event_.forward_hops = 5;
  this->event_.rx_buffer_level =
      static_cast<uint8_t>(config.target_rx_buffer_level() + 2),
  this->event_.cc_metadata = 0;
  this->event_.fabric_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(2.75,
                                                 falcon_rue::kFractionalBits);
  this->event_.nic_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(ncwnd,
                                                 falcon_rue::kFractionalBits);
  this->event_.num_packets_acked = 3;
  this->event_.event_queue_select = 0;
  this->event_.delay_select = falcon::DelaySelect::kForward;
  this->event_.fabric_window_time_marker = 99999;
  this->event_.nic_window_time_marker =
      new_now - event_rtt - 10;  // ncwnd time marker is more than one RTT away
  this->event_.base_delay = SwiftTyped::MakeBaseDelayField(
      /*profile_index=*/kSwiftDefaultProfileIndex);
  this->event_.delay_state = 150;
  this->event_.rtt_state = event_rtt;
  this->event_.cc_opaque = 3;
  this->event_.gen_bit = 0;

  swift->Process(this->event_, this->response_, new_now);

  EXPECT_EQ(this->response_.connection_id, this->event_.connection_id);
  // With a NACK, the ncwnd should decrease with the maximum multiplicative
  // decrease factor.
  double expected_ncwnd_float =
      ncwnd * config.max_nic_multiplicative_decrease();
  uint32_t expected_ncwnd_fixed = falcon_rue::FloatToFixed<double, uint32_t>(
      expected_ncwnd_float, falcon_rue::kFractionalBits);
  EXPECT_EQ(this->response_.nic_congestion_window,
            expected_ncwnd_fixed);                             // updated
  EXPECT_EQ(this->response_.nic_window_time_marker, new_now);  // updated
}

// Tests the case of NACK where the nic congestion window decrease is
// blocked because the last decrease is within one RTT.
TYPED_TEST(SwiftTypedTest, ProcessNackNicNoDecrease) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;

  const auto config = MakeConfig<EventT, ResponseT>(100);
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<SwiftTyped> swift,
                       SwiftTyped::Create(config));

  const auto new_now = 100400;
  const uint32_t event_rtt = 315;
  const double ncwnd = 100.0;

  this->event_.connection_id = 1234;
  this->event_.multipath_enable = false;
  this->event_.event_type = falcon::RueEventType::kNack;
  this->event_.timestamp_1 = 100000;
  this->event_.timestamp_2 = 100879;
  this->event_.timestamp_3 = 101000;
  this->event_.timestamp_4 = event_rtt + this->event_.timestamp_1;
  this->event_.retransmit_count = 0;
  this->event_.retransmit_reason = falcon::RetransmitReason::kTimeout;
  this->event_.nack_code = falcon::NackCode::kRxResourceExhaustion;
  this->event_.forward_hops = 5;
  this->event_.rx_buffer_level = 16;
  this->event_.cc_metadata = 0;
  this->event_.fabric_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(2.75,
                                                 falcon_rue::kFractionalBits);
  this->event_.nic_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(ncwnd,
                                                 falcon_rue::kFractionalBits);
  this->event_.num_packets_acked = 3;
  this->event_.event_queue_select = 0;
  this->event_.delay_select = falcon::DelaySelect::kForward;
  this->event_.fabric_window_time_marker = 99999;
  this->event_.nic_window_time_marker =
      new_now - event_rtt + 10;  // ncwnd time marker is less than one RTT away
  this->event_.base_delay = SwiftTyped::MakeBaseDelayField(
      /*profile_index=*/kSwiftDefaultProfileIndex);
  this->event_.delay_state = 150;
  this->event_.rtt_state = event_rtt;
  this->event_.cc_opaque = 3;
  this->event_.gen_bit = 0;

  swift->Process(this->event_, this->response_, new_now);

  EXPECT_EQ(this->response_.connection_id, this->event_.connection_id);
  EXPECT_EQ(this->response_.nic_congestion_window,
            this->event_.nic_congestion_window);  // no change
  EXPECT_EQ(this->response_.nic_window_time_marker,
            this->event_.nic_window_time_marker);  // no change
}

// Tests the case of NACK with a non resource-based type where the nic
// congestion window is still decreased.
TYPED_TEST(SwiftTypedTest, ProcessNackNicDecreaseWrongNackCode) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;

  const auto config = MakeConfig<EventT, ResponseT>(100);
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<SwiftTyped> swift,
                       SwiftTyped::Create(config));

  const auto new_now = 100400;
  const uint32_t event_rtt = 315;
  const double ncwnd = 100.0;

  this->event_.connection_id = 1234;
  this->event_.multipath_enable = false;
  this->event_.event_type = falcon::RueEventType::kNack;
  this->event_.timestamp_1 = 100000;
  this->event_.timestamp_2 = 100879;
  this->event_.timestamp_3 = 101000;
  this->event_.timestamp_4 = event_rtt + this->event_.timestamp_1;
  this->event_.retransmit_count = 0;
  this->event_.retransmit_reason = falcon::RetransmitReason::kTimeout;
  this->event_.nack_code =
      falcon::NackCode::kReserved;  // not a nic buffer related NACK.
  this->event_.forward_hops = 5;
  this->event_.rx_buffer_level = 16;
  this->event_.cc_metadata = 0;
  this->event_.fabric_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(2.75,
                                                 falcon_rue::kFractionalBits);
  this->event_.nic_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(ncwnd,
                                                 falcon_rue::kFractionalBits);
  this->event_.num_packets_acked = 3;
  this->event_.event_queue_select = 0;
  this->event_.delay_select = falcon::DelaySelect::kForward;
  this->event_.fabric_window_time_marker =
      new_now - event_rtt - 10;  // ncwnd time marker is more than one RTT away
  this->event_.nic_window_time_marker = 99999;
  this->event_.base_delay = SwiftTyped::MakeBaseDelayField(
      /*profile_index=*/kSwiftDefaultProfileIndex);
  this->event_.delay_state = 150;
  this->event_.rtt_state = event_rtt;
  this->event_.cc_opaque = 3;
  this->event_.gen_bit = 0;

  swift->Process(this->event_, this->response_, new_now);

  uint32_t expected_ncwnd_fixed =
      GetExpectedDecreasedNcwndFixed<EventT, ResponseT>(
          ncwnd, config.nic_multiplicative_decrease_factor(),
          this->event_.rx_buffer_level, config.target_rx_buffer_level());
  EXPECT_EQ(this->response_.connection_id, this->event_.connection_id);
  EXPECT_EQ(this->response_.nic_congestion_window,
            expected_ncwnd_fixed);  // updated based on rx_buffer_level, not
                                    // NACK resource-based type.
  EXPECT_EQ(this->response_.nic_window_time_marker, new_now);  // updated
}

// Tests that the NIC inter packet gap is correctly calculated after a maximum
// ncwnd decrease, and that the RTO timeout calculation in that case is
// correct.
TYPED_TEST(SwiftTypedTest, GetNicInterPacketGap) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;

  SwiftConfiguration config = MakeConfig<EventT, ResponseT>(100);
  config.set_min_nic_congestion_window(0.1);
  config.set_max_nic_multiplicative_decrease(0.9);

  ASSERT_OK_AND_ASSIGN(std::unique_ptr<SwiftTyped> swift,
                       SwiftTyped::Create(config));

  const auto new_now = 100400;
  const uint32_t event_rtt = 315;
  const double ncwnd = 5;

  this->event_.connection_id = 1234;
  this->event_.multipath_enable = false;
  this->event_.event_type = falcon::RueEventType::kNack;
  this->event_.timestamp_1 = 100000;
  this->event_.timestamp_2 = 100879;
  this->event_.timestamp_3 = 101000;
  this->event_.timestamp_4 = event_rtt + this->event_.timestamp_1;
  this->event_.retransmit_count = 0;
  this->event_.retransmit_reason = falcon::RetransmitReason::kTimeout;
  this->event_.nack_code = falcon::NackCode::kRxResourceExhaustion;
  this->event_.forward_hops = 5;
  this->event_.rx_buffer_level =
      static_cast<uint8_t>(config.target_rx_buffer_level() + 2),
  this->event_.cc_metadata = 0;
  this->event_.fabric_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(100,
                                                 falcon_rue::kFractionalBits);
  this->event_.nic_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(ncwnd,
                                                 falcon_rue::kFractionalBits);
  this->event_.num_packets_acked = 3;
  this->event_.event_queue_select = 0;
  this->event_.delay_select = falcon::DelaySelect::kForward;
  this->event_.fabric_window_time_marker = 99999;
  this->event_.nic_window_time_marker =
      new_now - event_rtt - 10;  // ncwnd time marker is more than one RTT away
  this->event_.base_delay = SwiftTyped::MakeBaseDelayField(
      /*profile_index=*/kSwiftDefaultProfileIndex);
  this->event_.delay_state = 150;
  this->event_.rtt_state = event_rtt;
  this->event_.cc_opaque = 3;
  this->event_.gen_bit = 0;

  swift->Process(this->event_, this->response_, new_now);

  EXPECT_EQ(this->response_.connection_id, this->event_.connection_id);
  EXPECT_EQ(this->response_.inter_packet_gap, 0);
  EXPECT_EQ(this->response_.rtt_state, event_rtt);
  // With a NACK, the ncwnd should decrease with the maximum multiplicative
  // decrease factor.
  double expected_ncwnd_float =
      ncwnd * (1 - config.max_nic_multiplicative_decrease());
  uint32_t expected_ncwnd_fixed = falcon_rue::FloatToFixed<double, uint32_t>(
      expected_ncwnd_float, falcon_rue::kFractionalBits);
  EXPECT_EQ(this->response_.nic_congestion_window, expected_ncwnd_fixed);
  double expected_nipg = std::round(
      (this->event_.rtt_state / expected_ncwnd_float - this->event_.rtt_state) *
      config.ipg_time_scalar());
  EXPECT_EQ(this->response_.nic_inter_packet_gap,
            static_cast<uint64_t>(expected_nipg));

  uint32_t retransmit_timeout_expected = SwiftTyped::GetRetransmitTimeout(
      config.min_retransmit_timeout(), event_rtt,
      config.retransmit_timeout_scalar(), expected_nipg,
      config.ipg_time_scalar());
  EXPECT_NEAR(this->response_.retransmit_timeout, retransmit_timeout_expected,
              1);
}

// Tests that the NIC inter packet gap is correctly reflected back after a
// retransmit RUE event, and that the RTO timeout calculation in that case is
// correct.
TYPED_TEST(SwiftTypedTest, NicInterPacketGapReflectionInRetransmitEvent) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;

  SwiftConfiguration config = MakeConfig<EventT, ResponseT>(100);
  config.set_min_nic_congestion_window(0.2);
  config.set_min_fabric_congestion_window(0.1);
  config.set_max_fabric_multiplicative_decrease(0.5);
  config.set_retransmit_limit(2);

  ASSERT_OK_AND_ASSIGN(std::unique_ptr<SwiftTyped> swift,
                       SwiftTyped::Create(config));

  const auto new_now = 100400;
  const uint32_t event_rtt = 315;
  const double fcwnd = 2;
  const double ncwnd = config.min_nic_congestion_window();

  this->event_.connection_id = 1234;
  this->event_.multipath_enable = false;
  this->event_.event_type = falcon::RueEventType::kRetransmit;
  // First retransmit, so fcwnd just decreases by the maximum MD.
  this->event_.retransmit_count = 1;
  this->event_.retransmit_reason = falcon::RetransmitReason::kTimeout;
  this->event_.nack_code = falcon::NackCode::kNotANack;
  this->event_.fabric_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(fcwnd,
                                                 falcon_rue::kFractionalBits);
  this->event_.nic_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(ncwnd,
                                                 falcon_rue::kFractionalBits);
  this->event_.rtt_state = event_rtt;
  this->event_.cc_opaque = 3;
  this->event_.gen_bit = 0;

  swift->Process(this->event_, this->response_, new_now);
  EXPECT_EQ(this->response_.connection_id, this->event_.connection_id);
  uint32_t expected_fcwnd_fixed = falcon_rue::FloatToFixed<double, uint32_t>(
      fcwnd * (1 - config.max_fabric_multiplicative_decrease()),
      falcon_rue::kFractionalBits);
  EXPECT_EQ(this->response_.fabric_congestion_window, expected_fcwnd_fixed);
  EXPECT_EQ(this->response_.inter_packet_gap, 0);
  EXPECT_EQ(this->response_.rtt_state, event_rtt);
  EXPECT_EQ(this->response_.nic_congestion_window,
            this->event_.nic_congestion_window);
  double expected_nipg =
      std::floor((this->event_.rtt_state / ncwnd - this->event_.rtt_state) *
                 config.ipg_time_scalar());
  EXPECT_EQ(this->response_.nic_inter_packet_gap,
            static_cast<uint64_t>(expected_nipg));
  // RTO calculation should use the NIC ipg.
  uint32_t retransmit_timeout_expected = SwiftTyped::GetRetransmitTimeout(
      config.min_retransmit_timeout(), event_rtt,
      config.retransmit_timeout_scalar(), expected_nipg,
      config.ipg_time_scalar());
  EXPECT_NEAR(this->response_.retransmit_timeout, retransmit_timeout_expected,
              1);

  // In this second event, fcwnd should drop to its min value because
  // retransmit_limit is set to 2.
  this->event_.retransmit_count = config.retransmit_limit();
  swift->Process(this->event_, this->response_, new_now);
  EXPECT_EQ(this->response_.connection_id, this->event_.connection_id);
  EXPECT_EQ(this->response_.nic_congestion_window,
            this->event_.nic_congestion_window);
  EXPECT_EQ(this->response_.nic_inter_packet_gap, expected_nipg);

  expected_fcwnd_fixed = falcon_rue::FloatToFixed<double, uint32_t>(
      config.min_fabric_congestion_window(), falcon_rue::kFractionalBits);
  EXPECT_EQ(this->response_.fabric_congestion_window, expected_fcwnd_fixed);
  double expected_fipg = std::round(
      (this->event_.rtt_state / config.min_fabric_congestion_window() -
       this->event_.rtt_state) *
      config.ipg_time_scalar());
  EXPECT_EQ(this->response_.inter_packet_gap, expected_fipg);
  EXPECT_EQ(this->response_.rtt_state, event_rtt);

  // RTO should now use the fabric ipg instead of the NIC ipg because the fabric
  // ipg is larger.
  retransmit_timeout_expected = SwiftTyped::GetRetransmitTimeout(
      config.min_retransmit_timeout(), event_rtt,
      config.retransmit_timeout_scalar(), expected_fipg,
      config.ipg_time_scalar());
  EXPECT_NEAR(this->response_.retransmit_timeout, retransmit_timeout_expected,
              1);
}

// Tests that the ar_rate is computed correctly for ACK events.
TYPED_TEST(SwiftTypedTest, ArRateAckEvents) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;

  SwiftConfiguration config = MakeConfig<EventT, ResponseT>(100);
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<SwiftTyped> swift,
                       SwiftTyped::Create(config));

  const auto new_now = 101000;

  this->event_.connection_id = 1234;
  this->event_.multipath_enable = false;
  this->event_.event_type = falcon::RueEventType::kAck;
  this->event_.timestamp_1 = 100000;
  this->event_.timestamp_2 = 100440;
  this->event_.timestamp_3 = 100600;
  this->event_.timestamp_4 = 100800;
  this->event_.retransmit_count = 0;
  this->event_.retransmit_reason = falcon::RetransmitReason::kTimeout;
  this->event_.nack_code = falcon::NackCode::kNotANack;
  this->event_.forward_hops = 5;
  this->event_.cc_metadata = 0;
  this->event_.num_packets_acked = 1;
  this->event_.event_queue_select = 0;
  this->event_.delay_select = falcon::DelaySelect::kForward;
  this->event_.fabric_window_time_marker = 99000;  // < now - rtt_state
  this->event_.base_delay = SwiftTyped::MakeBaseDelayField(
      /*profile_index=*/kSwiftDefaultProfileIndex);
  this->event_.delay_state = 150;
  this->event_.rtt_state = 710;
  this->event_.cc_opaque = 3;
  this->event_.gen_bit = 0;

  // fcwnd < kMaxArCwndThreshold, ncwnd < kMaxArCwndThreshold
  // ar_rate should be 0.
  this->event_.fabric_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(kMaxArCwndThreshold - 1,
                                                 falcon_rue::kFractionalBits);
  this->event_.nic_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(kMaxArCwndThreshold - 1,
                                                 falcon_rue::kFractionalBits);
  swift->Process(this->event_, this->response_, new_now);
  EXPECT_EQ(this->response_.ar_rate, 0);

  // fcwnd < kMaxArCwndThreshold, ncwnd > kMaxArCwndThreshold
  // ar_rate should be 0.
  this->event_.fabric_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(kMaxArCwndThreshold - 1,
                                                 falcon_rue::kFractionalBits);
  this->event_.nic_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(kMaxArCwndThreshold + 1,
                                                 falcon_rue::kFractionalBits);
  swift->Process(this->event_, this->response_, new_now);
  EXPECT_EQ(this->response_.ar_rate, 0);

  // fcwnd > kMaxArCwndThreshold, ncwnd < kMaxArCwndThreshold
  // ar_rate should be 0.
  this->event_.fabric_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(kMaxArCwndThreshold + 1,
                                                 falcon_rue::kFractionalBits);
  this->event_.nic_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(kMaxArCwndThreshold - 1,
                                                 falcon_rue::kFractionalBits);
  swift->Process(this->event_, this->response_, new_now);
  EXPECT_EQ(this->response_.ar_rate, 0);

  // fcwnd > kMaxArCwndThreshold, ncwnd > kMaxArCwndThreshold
  // ar_rate should be 4.
  this->event_.fabric_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(kMaxArCwndThreshold + 1,
                                                 falcon_rue::kFractionalBits);
  this->event_.nic_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(kMaxArCwndThreshold + 1,
                                                 falcon_rue::kFractionalBits);
  swift->Process(this->event_, this->response_, new_now);
  EXPECT_EQ(this->response_.ar_rate, 4);
}

// Tests that the ar_rate is computed correctly for NACK events.
TYPED_TEST(SwiftTypedTest, ArRateNackEvents) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;

  SwiftConfiguration config = MakeConfig<EventT, ResponseT>(100);
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<SwiftTyped> swift,
                       SwiftTyped::Create(config));

  const auto new_now = 101000;

  this->event_.connection_id = 1234;
  this->event_.multipath_enable = false;
  this->event_.event_type = falcon::RueEventType::kNack;
  this->event_.timestamp_1 = 100000;
  this->event_.timestamp_2 = 100440;
  this->event_.timestamp_3 = 100600;
  this->event_.timestamp_4 = 100800;
  this->event_.retransmit_count = 0;
  this->event_.retransmit_reason = falcon::RetransmitReason::kTimeout;
  this->event_.nack_code = falcon::NackCode::kRxResourceExhaustion;
  this->event_.forward_hops = 5;
  this->event_.cc_metadata = 0;
  this->event_.num_packets_acked = 1;
  this->event_.event_queue_select = 0;
  this->event_.delay_select = falcon::DelaySelect::kForward;
  this->event_.fabric_window_time_marker = 99000;  // < now - rtt_state
  this->event_.base_delay = SwiftTyped::MakeBaseDelayField(
      /*profile_index=*/kSwiftDefaultProfileIndex);
  this->event_.delay_state = 150;
  this->event_.rtt_state = 710;
  this->event_.cc_opaque = 3;
  this->event_.gen_bit = 0;

  // fcwnd < kMaxArCwndThreshold, ncwnd < kMaxArCwndThreshold
  // ar_rate should be 0.
  this->event_.fabric_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(kMaxArCwndThreshold - 1,
                                                 falcon_rue::kFractionalBits);
  this->event_.nic_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(kMaxArCwndThreshold - 1,
                                                 falcon_rue::kFractionalBits);
  swift->Process(this->event_, this->response_, new_now);
  EXPECT_EQ(this->response_.ar_rate, 0);

  // fcwnd < kMaxArCwndThreshold, ncwnd > kMaxArCwndThreshold
  // ar_rate should be 0.
  this->event_.fabric_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(kMaxArCwndThreshold - 1,
                                                 falcon_rue::kFractionalBits);
  this->event_.nic_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(kMaxArCwndThreshold + 1,
                                                 falcon_rue::kFractionalBits);
  swift->Process(this->event_, this->response_, new_now);
  EXPECT_EQ(this->response_.ar_rate, 0);

  // fcwnd > kMaxArCwndThreshold, ncwnd < kMaxArCwndThreshold
  // ar_rate should be 0.
  this->event_.fabric_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(kMaxArCwndThreshold + 1,
                                                 falcon_rue::kFractionalBits);
  this->event_.nic_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(kMaxArCwndThreshold - 1,
                                                 falcon_rue::kFractionalBits);
  swift->Process(this->event_, this->response_, new_now);
  EXPECT_EQ(this->response_.ar_rate, 0);

  // fcwnd > kMaxArCwndThreshold, ncwnd > kMaxArCwndThreshold
  // ar_rate should be 4.
  this->event_.fabric_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(kMaxArCwndThreshold + 1,
                                                 falcon_rue::kFractionalBits);
  this->event_.nic_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(kMaxArCwndThreshold * 2 + 1,
                                                 falcon_rue::kFractionalBits);
  swift->Process(this->event_, this->response_, new_now);
  EXPECT_EQ(this->response_.ar_rate, 4);
}

// Tests that the ar_rate is computed correctly for retransmit events.
TYPED_TEST(SwiftTypedTest, ArRateRetransmitEvents) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;

  SwiftConfiguration config = MakeConfig<EventT, ResponseT>(100);
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<SwiftTyped> swift,
                       SwiftTyped::Create(config));

  const auto new_now = 101000;

  this->event_.connection_id = 1234;
  this->event_.multipath_enable = false;
  this->event_.event_type = falcon::RueEventType::kRetransmit;
  this->event_.timestamp_1 = 100000;
  this->event_.timestamp_2 = 100440;
  this->event_.timestamp_3 = 100600;
  this->event_.timestamp_4 = 100800;
  this->event_.retransmit_count = 0;
  this->event_.retransmit_reason = falcon::RetransmitReason::kTimeout;
  this->event_.nack_code = falcon::NackCode::kNotANack;
  this->event_.forward_hops = 5;
  this->event_.cc_metadata = 0;
  this->event_.num_packets_acked = 1;
  this->event_.event_queue_select = 0;
  this->event_.delay_select = falcon::DelaySelect::kForward;
  this->event_.fabric_window_time_marker = 99000;  // < now - rtt_state
  this->event_.base_delay = SwiftTyped::MakeBaseDelayField(
      /*profile_index=*/kSwiftDefaultProfileIndex);
  this->event_.delay_state = 150;
  this->event_.rtt_state = 710;
  this->event_.cc_opaque = 3;
  this->event_.gen_bit = 0;

  // fcwnd < kMaxArCwndThreshold, ncwnd < kMaxArCwndThreshold
  // ar_rate should be 0.
  this->event_.fabric_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(kMaxArCwndThreshold - 1,
                                                 falcon_rue::kFractionalBits);
  this->event_.nic_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(kMaxArCwndThreshold - 1,
                                                 falcon_rue::kFractionalBits);
  swift->Process(this->event_, this->response_, new_now);
  EXPECT_EQ(this->response_.ar_rate, 0);

  // fcwnd < kMaxArCwndThreshold, ncwnd > kMaxArCwndThreshold
  // ar_rate should be 0.
  this->event_.fabric_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(kMaxArCwndThreshold - 1,
                                                 falcon_rue::kFractionalBits);
  this->event_.nic_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(kMaxArCwndThreshold + 1,
                                                 falcon_rue::kFractionalBits);
  swift->Process(this->event_, this->response_, new_now);
  EXPECT_EQ(this->response_.ar_rate, 0);

  // fcwnd > kMaxArCwndThreshold, ncwnd < kMaxArCwndThreshold
  // ar_rate should be 0.
  this->event_.fabric_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(kMaxArCwndThreshold + 1,
                                                 falcon_rue::kFractionalBits);
  this->event_.nic_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(kMaxArCwndThreshold - 1,
                                                 falcon_rue::kFractionalBits);
  swift->Process(this->event_, this->response_, new_now);
  EXPECT_EQ(this->response_.ar_rate, 0);

  // fcwnd > kMaxArCwndThreshold, ncwnd > kMaxArCwndThreshold
  // ar_rate should be 4.
  this->event_.fabric_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(kMaxArCwndThreshold + 1,
                                                 falcon_rue::kFractionalBits);
  this->event_.nic_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(kMaxArCwndThreshold + 1,
                                                 falcon_rue::kFractionalBits);
  swift->Process(this->event_, this->response_, new_now);
  EXPECT_EQ(this->response_.ar_rate, 4);
}

// Tests that the ar_rate is computed correctly for multipath ACK events.
TYPED_TEST(SwiftTypedTest, MultipathArRateAckEvents) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using StateT = ConnectionState<EventT>;
  using DramStateManagerT = ConnectionBitsDramStateManager<EventT>;
  using AlgorithmT = Swift<EventT, ResponseT>;
  using StatefulAlgorithmT =
      StatefulAlgorithm</*Algorithm=*/AlgorithmT,
                        /*DramStateManagerT=*/DramStateManagerT,
                        /*ForBenchmarking=*/false>;
  SwiftConfiguration config = AlgorithmT::DefaultConfiguration();
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<StatefulAlgorithmT> swift,
                       AlgorithmT::template Create<StatefulAlgorithmT>(config));

  const uint32_t new_now = 100400;
  const uint8_t kMaxConnectionBits = 5;
  const auto kDefaultStateValue = ConnectionState<EventT>{};
  auto manager = std::make_unique<ConnectionBitsDramStateManager<EventT>>(
      sizeof(StateT), &kDefaultStateValue, kMaxConnectionBits);
  swift->set_dram_state_manager(std::move(manager));
  DramStateManagerT* manager_ptr = swift->get_dram_state_manager();

  this->event_.connection_id = 1;
  this->event_.flow_label = 0x11;  // flow ID is 1
  this->event_.multipath_enable = true;
  this->event_.event_type = falcon::RueEventType::kAck;
  this->event_.retransmit_reason = falcon::RetransmitReason::kTimeout;
  this->event_.nack_code = falcon::NackCode::kNotANack;

  // fcwnd < kMaxArCwndThreshold, ncwnd < kMaxArCwndThreshold
  // ar_rate should be 0.
  StateT& state = manager_ptr->template GetStateForEvent<StateT>(this->event_);
  uint32_t connection_fcwnd = falcon_rue::FloatToFixed<double, uint32_t>(
      kMaxArCwndThreshold - 1, falcon_rue::kFractionalBits);
  uint32_t flow_fcwnd = connection_fcwnd / 4;
  SetFlowFcwndsInRueState<EventT, ResponseT>(flow_fcwnd, state);
  this->event_.fabric_congestion_window = connection_fcwnd;
  this->event_.nic_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(kMaxArCwndThreshold - 1,
                                                 falcon_rue::kFractionalBits);
  swift->Process(this->event_, this->response_, new_now);
  EXPECT_EQ(this->response_.ar_rate, 0);

  // fcwnd < kMaxArCwndThreshold, ncwnd > kMaxArCwndThreshold
  // ar_rate should be 0.
  SetFlowFcwndsInRueState<EventT, ResponseT>(flow_fcwnd, state);
  this->event_.nic_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(kMaxArCwndThreshold + 1,
                                                 falcon_rue::kFractionalBits);
  swift->Process(this->event_, this->response_, new_now);
  EXPECT_EQ(this->response_.ar_rate, 0);

  // fcwnd > kMaxArCwndThreshold, ncwnd < kMaxArCwndThreshold
  // ar_rate should be 0.
  connection_fcwnd = falcon_rue::FloatToFixed<double, uint32_t>(
      kMaxArCwndThreshold + 5, falcon_rue::kFractionalBits);
  flow_fcwnd = connection_fcwnd / 4;
  SetFlowFcwndsInRueState<EventT, ResponseT>(flow_fcwnd, state);
  this->event_.nic_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(kMaxArCwndThreshold - 1,
                                                 falcon_rue::kFractionalBits);
  swift->Process(this->event_, this->response_, new_now);
  EXPECT_EQ(this->response_.ar_rate, 0);

  // fcwnd > kMaxArCwndThreshold, ncwnd > kMaxArCwndThreshold
  // ar_rate should be 4.
  SetFlowFcwndsInRueState<EventT, ResponseT>(flow_fcwnd, state);
  this->event_.nic_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(kMaxArCwndThreshold + 1,
                                                 falcon_rue::kFractionalBits);
  swift->Process(this->event_, this->response_, new_now);
  EXPECT_EQ(this->response_.ar_rate, 4);
}

// Tests that the ar_rate is computed correctly for multipath retransmit events.
TYPED_TEST(SwiftTypedTest, MultipathArRateRetransmitEvents) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using StateT = ConnectionState<EventT>;
  using DramStateManagerT = ConnectionBitsDramStateManager<EventT>;
  using AlgorithmT = Swift<EventT, ResponseT>;
  using StatefulAlgorithmT =
      StatefulAlgorithm</*Algorithm=*/AlgorithmT,
                        /*DramStateManagerT=*/DramStateManagerT,
                        /*ForBenchmarking=*/false>;
  SwiftConfiguration config = AlgorithmT::DefaultConfiguration();
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<StatefulAlgorithmT> swift,
                       AlgorithmT::template Create<StatefulAlgorithmT>(config));

  const uint32_t new_now = 100400;
  const uint8_t kMaxConnectionBits = 5;
  const auto kDefaultStateValue = ConnectionState<EventT>{};
  auto manager = std::make_unique<ConnectionBitsDramStateManager<EventT>>(
      sizeof(StateT), &kDefaultStateValue, kMaxConnectionBits);
  swift->set_dram_state_manager(std::move(manager));
  DramStateManagerT* manager_ptr = swift->get_dram_state_manager();

  this->event_.connection_id = 1;
  this->event_.flow_label = 0x11;  // flow ID is 1
  this->event_.multipath_enable = true;
  this->event_.event_type = falcon::RueEventType::kRetransmit;
  this->event_.retransmit_reason = falcon::RetransmitReason::kTimeout;
  this->event_.nack_code = falcon::NackCode::kNotANack;

  // fcwnd < kMaxArCwndThreshold, ncwnd < kMaxArCwndThreshold
  // ar_rate should be 0.
  StateT& state = manager_ptr->template GetStateForEvent<StateT>(this->event_);
  uint32_t connection_fcwnd = falcon_rue::FloatToFixed<double, uint32_t>(
      kMaxArCwndThreshold - 1, falcon_rue::kFractionalBits);
  uint32_t flow_fcwnd = connection_fcwnd / 4;
  SetFlowFcwndsInRueState<EventT, ResponseT>(flow_fcwnd, state);
  this->event_.fabric_congestion_window = connection_fcwnd;
  this->event_.nic_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(kMaxArCwndThreshold - 1,
                                                 falcon_rue::kFractionalBits);
  swift->Process(this->event_, this->response_, new_now);
  EXPECT_EQ(this->response_.ar_rate, 0);

  // fcwnd < kMaxArCwndThreshold, ncwnd > kMaxArCwndThreshold
  // ar_rate should be 0.
  SetFlowFcwndsInRueState<EventT, ResponseT>(flow_fcwnd, state);
  this->event_.nic_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(kMaxArCwndThreshold + 1,
                                                 falcon_rue::kFractionalBits);
  swift->Process(this->event_, this->response_, new_now);
  EXPECT_EQ(this->response_.ar_rate, 0);

  // fcwnd > kMaxArCwndThreshold, ncwnd < kMaxArCwndThreshold
  // ar_rate should be 0.
  connection_fcwnd = falcon_rue::FloatToFixed<double, uint32_t>(
      kMaxArCwndThreshold + 5, falcon_rue::kFractionalBits);
  flow_fcwnd = connection_fcwnd / 4;
  SetFlowFcwndsInRueState<EventT, ResponseT>(flow_fcwnd, state);
  this->event_.nic_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(kMaxArCwndThreshold - 1,
                                                 falcon_rue::kFractionalBits);
  swift->Process(this->event_, this->response_, new_now);
  EXPECT_EQ(this->response_.ar_rate, 0);

  // fcwnd > kMaxArCwndThreshold, ncwnd > kMaxArCwndThreshold
  // ar_rate should be 4.
  SetFlowFcwndsInRueState<EventT, ResponseT>(flow_fcwnd, state);
  this->event_.nic_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(kMaxArCwndThreshold + 1,
                                                 falcon_rue::kFractionalBits);
  swift->Process(this->event_, this->response_, new_now);
  EXPECT_EQ(this->response_.ar_rate, 4);
}

// Tests that the per-connection alpha values are computed correctly.
TYPED_TEST(SwiftTypedTest, PerConnectionBackpressureAlpha) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;

  SwiftConfiguration config = MakeConfig<EventT, ResponseT>(100);
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<SwiftTyped> swift,
                       SwiftTyped::Create(config));

  const auto new_now = 101000;

  this->event_.connection_id = 1234;
  this->event_.multipath_enable = false;
  this->event_.event_type = falcon::RueEventType::kAck;
  this->event_.fabric_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(32,
                                                 falcon_rue::kFractionalBits);
  this->event_.nic_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(32,
                                                 falcon_rue::kFractionalBits);
  this->event_.retransmit_count = 0;
  this->event_.retransmit_reason = falcon::RetransmitReason::kTimeout;
  this->event_.nack_code = falcon::NackCode::kNotANack;
  this->event_.forward_hops = 5;
  this->event_.cc_metadata = 0;
  this->event_.num_packets_acked = 1;
  this->event_.event_queue_select = 0;
  this->event_.delay_select = falcon::DelaySelect::kForward;
  this->event_.fabric_window_time_marker = 99000;  // < now - rtt_state
  this->event_.base_delay = SwiftTyped::MakeBaseDelayField(
      /*profile_index=*/kSwiftDefaultProfileIndex);
  this->event_.delay_state = 150;
  this->event_.rtt_state = 710;
  this->event_.cc_opaque = 3;
  this->event_.gen_bit = 0;

  // Fabric delay is < target_delay and rx_buffer_level < target_level, so alpha
  // should be equal to base_alpha of 1/8. Therefore, shift should be shift 3 +
  // 3 = 6.
  this->event_.timestamp_1 = 100000;
  this->event_.timestamp_2 = 100001;
  this->event_.timestamp_3 = 100002;
  this->event_.timestamp_4 = 100003;
  this->event_.rx_buffer_level = 1;
  swift->Process(this->event_, this->response_, new_now);
  EXPECT_EQ(this->response_.alpha_request, 6);
  EXPECT_EQ(this->response_.alpha_response, 6);

  // Fabric delay is < target_delay and rx_buffer_level > target_level, so
  // alpha_request should be equal less than base_alpha of 1/8, but
  // alpha_response should remain unchanged.
  this->event_.rx_buffer_level = 31;
  swift->Process(this->event_, this->response_, new_now);
  EXPECT_EQ(this->response_.alpha_request, 7);
  EXPECT_EQ(this->response_.alpha_response, 6);

  // Fabric delay is > target_delay and rx_buffer_level < target_level, so
  // both alpha_request and alpha_response should be equal less than base_alpha
  // of 1/8.
  this->event_.timestamp_1 = 100000;
  this->event_.timestamp_2 = 100400;
  this->event_.timestamp_3 = 100401;
  this->event_.timestamp_4 = 100402;
  this->event_.rx_buffer_level = 1;
  swift->Process(this->event_, this->response_, new_now);
  EXPECT_EQ(this->response_.alpha_request, 7);
  EXPECT_EQ(this->response_.alpha_response, 7);
}

// Tests Swift profile configuration in Gen_2.
TYPED_TEST(SwiftTypedTest, InvalidProfileIndex) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;
  SwiftConfiguration config = MakeConfig<EventT, ResponseT>(100);
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<SwiftTyped> swift,
                       SwiftTyped::Create(config));
  constexpr int kValidProfileIndex = 1;

  AlgorithmConfiguration algorithm_config;
  EXPECT_THAT(
      swift->InstallAlgorithmProfile(kValidProfileIndex, algorithm_config),
      StatusIs(absl::StatusCode::kInvalidArgument));
  *algorithm_config.mutable_swift() = config;
  EXPECT_THAT(swift->InstallAlgorithmProfile(-1, algorithm_config),
              StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(swift->InstallAlgorithmProfile(kSwiftDefaultProfileIndex,
                                             algorithm_config),
              StatusIs(absl::StatusCode::kAlreadyExists));

  EXPECT_OK(swift->InstallAlgorithmProfile(1, algorithm_config));

  EXPECT_THAT(
      swift->InstallAlgorithmProfile(kValidProfileIndex, algorithm_config),
      StatusIs(absl::StatusCode::kAlreadyExists));

  EXPECT_THAT(swift->UninstallAlgorithmProfile(kSwiftDefaultProfileIndex),
              StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(swift->UninstallAlgorithmProfile(-1),
              StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_OK(swift->UninstallAlgorithmProfile(kValidProfileIndex));
  EXPECT_THAT(
      swift->UninstallAlgorithmProfile(kValidProfileIndex),
      StatusIs(absl::StatusCode::kNotFound));  // it is truly uninstalled.
}

// Tests that the  stateful algorithm code path in Swift is executed for
// multipath connections.
TYPED_TEST(SwiftTypedTest, BasicMultipathConnection) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using StateT = ConnectionState<EventT>;
  using DramStateManagerT = ConnectionBitsDramStateManager<EventT>;
  using AlgorithmT = Swift<EventT, ResponseT>;
  using StatefulAlgorithmT =
      StatefulAlgorithm</*Algorithm=*/AlgorithmT,
                        /*DramStateManagerT=*/DramStateManagerT,
                        /*ForBenchmarking=*/false>;
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<StatefulAlgorithmT> swift,
                       AlgorithmT::template Create<StatefulAlgorithmT>(
                           AlgorithmT::DefaultConfiguration()));

  const uint32_t new_now = 100400;
  const uint8_t kMaxConnectionBits = 5;
  const auto kDefaultStateValue = ConnectionState<EventT>{};
  auto manager = std::make_unique<ConnectionBitsDramStateManager<EventT>>(
      sizeof(StateT), &kDefaultStateValue, kMaxConnectionBits);
  swift->set_dram_state_manager(std::move(manager));

  this->event_.connection_id = 1;
  this->event_.flow_label = 0x11;  // flow ID is 1
  this->event_.multipath_enable = false;
  this->event_.event_type = falcon::RueEventType::kAck;
  this->event_.retransmit_reason = falcon::RetransmitReason::kTimeout;
  this->event_.nack_code = falcon::NackCode::kNotANack;
  this->event_.fabric_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(10,
                                                 falcon_rue::kFractionalBits);
  this->event_.nic_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(10,
                                                 falcon_rue::kFractionalBits);
  swift->Process(this->event_, this->response_, new_now);
  // For non-multipath connections, the flow ID returned in the Response should
  // always be 0.
  EXPECT_EQ(this->response_.flow_id, 0);

  this->event_.multipath_enable = true;
  swift->Process(this->event_, this->response_, new_now);
  // For multipath connections, the flow ID returned in the Response should
  // be the last 2 bits in flow label inside the event.
  EXPECT_EQ(this->response_.flow_id, 1);
}

// Tests that the fcwnd and flow weights are properly calculated for multipath
// connections during retransmit events.
TYPED_TEST(SwiftTypedTest, ProcessRetransmitMultipathFcwndAndWeights) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using StateT = ConnectionState<EventT>;
  using DramStateManagerT = ConnectionBitsDramStateManager<EventT>;
  using AlgorithmT = Swift<EventT, ResponseT>;
  using StatefulAlgorithmT =
      StatefulAlgorithm</*Algorithm=*/AlgorithmT,
                        /*DramStateManagerT=*/DramStateManagerT,
                        /*ForBenchmarking=*/false>;
  SwiftConfiguration config = AlgorithmT::DefaultConfiguration();
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<StatefulAlgorithmT> swift,
                       AlgorithmT::template Create<StatefulAlgorithmT>(config));

  const uint32_t new_now = 100400;
  const uint32_t rtt_state = kInitialFlowRttState;
  // All flows initially have rtt_state values = kInitialFlowRttState, and fcwnd
  // = kInitialFlowFcwnd.

  const uint8_t kMaxConnectionBits = 5;
  const uint32_t kInitialConnectionFcwndFixed = kInitialFlowFcwnd * 4;
  const double kInitialFlowFcwndFloat =
      falcon_rue::FixedToFloat<uint32_t, double>(kInitialFlowFcwnd,
                                                 falcon_rue::kFractionalBits);
  const auto kDefaultStateValue = ConnectionState<EventT>{};
  auto manager = std::make_unique<ConnectionBitsDramStateManager<EventT>>(
      sizeof(StateT), &kDefaultStateValue, kMaxConnectionBits);
  swift->set_dram_state_manager(std::move(manager));

  // Event 1: No decrease in fcwnd because this event belongs to flow 0, and
  // the fcwnd guard is less than an RTT away.
  this->event_.connection_id = 1;
  this->event_.flow_label = 0x10;  // flow ID is 0
  this->event_.multipath_enable = true;
  this->event_.event_type = falcon::RueEventType::kRetransmit;
  this->event_.retransmit_reason = falcon::RetransmitReason::kTimeout;
  this->event_.retransmit_count = 1;
  this->event_.nack_code = falcon::NackCode::kNotANack;
  this->event_.fabric_congestion_window = kInitialConnectionFcwndFixed;
  this->event_.fabric_window_time_marker =
      new_now - rtt_state + 10;  // fcwnd decrease should not happen.
  this->event_.nic_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(10,
                                                 falcon_rue::kFractionalBits);
  this->event_.rtt_state = rtt_state;
  swift->Process(this->event_, this->response_, new_now);
  EXPECT_EQ(this->response_.flow_id, 0);
  EXPECT_EQ(this->response_.fabric_congestion_window,
            this->event_.fabric_congestion_window);
  EXPECT_EQ(this->response_.fabric_window_time_marker,
            this->event_.fabric_window_time_marker);
  EXPECT_EQ(this->response_.rtt_state, this->event_.rtt_state);
  std::vector<double> expected_fcwnds = {32, 32, 32, 32};
  CheckFlowWeightsGivenFlowFcwnds<ResponseT>(this->response_, expected_fcwnds);

  // Event 2: A decrease in fcwnd will take place because this event belongs to
  // flow 0, and the fcwnd guard in the RUE event is more than an RTT away.
  this->event_.fabric_window_time_marker =
      new_now - rtt_state - 10;  // fcwnd decrease should now happen.
  swift->Process(this->event_, this->response_, new_now);
  // First retransmit count, so fcwnd just decreases by the maximum MD.
  double expected_fcwnd_flow_0 =
      kInitialFlowFcwndFloat *
      (1 - config.max_fabric_multiplicative_decrease());
  double expected_fcwnd_connection =
      falcon_rue::FixedToFloat<uint32_t, double>(kInitialConnectionFcwndFixed,
                                                 falcon_rue::kFractionalBits) -
      kInitialFlowFcwndFloat + expected_fcwnd_flow_0;
  uint32_t expected_fcwnd_connection_fixed =
      falcon_rue::FloatToFixed<double, uint32_t>(expected_fcwnd_connection,
                                                 falcon_rue::kFractionalBits);
  EXPECT_EQ(this->response_.flow_id, 0);
  EXPECT_EQ(this->response_.fabric_congestion_window,
            expected_fcwnd_connection_fixed);
  EXPECT_EQ(this->response_.fabric_window_time_marker, new_now);
  expected_fcwnds[0] = expected_fcwnd_flow_0;
  CheckFlowWeightsGivenFlowFcwnds<ResponseT>(this->response_, expected_fcwnds);

  // Event 3: A decrease in fcwnd will take place because this event belongs to
  // flow 1, and the fcwnd guard in RUE state (0) is more than an RTT away.
  this->event_.fabric_congestion_window =
      this->response_.fabric_congestion_window;
  this->event_.flow_label = 0x11;  // now an event for flow ID 1 arrives
  swift->Process(this->event_, this->response_, new_now);
  EXPECT_EQ(this->response_.flow_id, 1);
  // Expect same connection fcwnd decrease as in Event 2.
  uint32_t connection_fcwnd_decrease =
      kInitialConnectionFcwndFixed - expected_fcwnd_connection_fixed;
  EXPECT_EQ(this->response_.fabric_congestion_window,
            this->event_.fabric_congestion_window - connection_fcwnd_decrease);
  // The fcwnd guard in the response belongs to flow 0, so expect no change when
  // the event belongs to flow 1.
  EXPECT_EQ(this->response_.fabric_window_time_marker,
            this->event_.fabric_window_time_marker);
  expected_fcwnds[1] = expected_fcwnd_flow_0;
  CheckFlowWeightsGivenFlowFcwnds<ResponseT>(this->response_, expected_fcwnds);

  // Event 4: No decrease in fcwnd because this event belongs to flow 1, and the
  // fcwnd guard in RUE state (new_now) is less than an RTT away.
  this->event_.fabric_congestion_window =
      this->response_.fabric_congestion_window;
  swift->Process(this->event_, this->response_, new_now);
  EXPECT_EQ(this->response_.flow_id, 1);
  EXPECT_EQ(this->response_.fabric_congestion_window,
            this->event_.fabric_congestion_window);
  EXPECT_EQ(this->response_.fabric_window_time_marker,
            this->event_.fabric_window_time_marker);
  CheckFlowWeightsGivenFlowFcwnds<ResponseT>(this->response_, expected_fcwnds);
}

// Tests that the fcwnd and flow weights are properly calculated for multipath
// connections during retransmit events, and that the RTO timeout calculation in
// that case is correct.
TYPED_TEST(SwiftTypedTest, ProcessRetransmitMultipathFipgAndRto) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using StateT = ConnectionState<EventT>;
  using DramStateManagerT = ConnectionBitsDramStateManager<EventT>;
  using AlgorithmT = Swift<EventT, ResponseT>;
  using StatefulAlgorithmT =
      StatefulAlgorithm</*Algorithm=*/AlgorithmT,
                        /*DramStateManagerT=*/DramStateManagerT,
                        /*ForBenchmarking=*/false>;
  const double kMinFcwnd = 0.1;
  SwiftConfiguration config = AlgorithmT::DefaultConfiguration();
  config.set_min_fabric_congestion_window(kMinFcwnd);
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<StatefulAlgorithmT> swift,
                       AlgorithmT::template Create<StatefulAlgorithmT>(config));

  uint32_t new_now = 100400;
  uint32_t rtt_state = kInitialFlowRttState;
  // All flows initially have rtt_state values = kInitialFlowRttState, and fcwnd
  // = kInitialFlowFcwnd.

  const uint8_t kMaxConnectionBits = 5;
  const uint32_t kInitialConnectionFcwndFixed = kInitialFlowFcwnd * 4;
  const auto kDefaultStateValue = ConnectionState<EventT>{};
  auto manager = std::make_unique<ConnectionBitsDramStateManager<EventT>>(
      sizeof(StateT), &kDefaultStateValue, kMaxConnectionBits);
  swift->set_dram_state_manager(std::move(manager));

  // Event 1: fcwnd for flow ID 0 will drop to kMinFcwnd because
  // retransmit_count = retransmit_limit(). fipg will still be 0 because
  // connection fcnwd will still be more than 1.
  this->event_.connection_id = 1;
  this->event_.flow_label = 0x10;  // flow ID is 0
  this->event_.multipath_enable = true;
  this->event_.event_type = falcon::RueEventType::kRetransmit;
  this->event_.retransmit_reason = falcon::RetransmitReason::kTimeout;
  this->event_.retransmit_count =
      config.retransmit_limit();  // fcwnd for flow ID 0 will drop to kMinFcwnd
  this->event_.nack_code = falcon::NackCode::kNotANack;
  this->event_.fabric_congestion_window = kInitialConnectionFcwndFixed;
  this->event_.fabric_window_time_marker =
      new_now - rtt_state - 10;  // fcwnd decrease will happen.
  this->event_.nic_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(10,
                                                 falcon_rue::kFractionalBits);
  this->event_.rtt_state = rtt_state;
  swift->Process(this->event_, this->response_, new_now);
  EXPECT_EQ(this->response_.flow_id, 0);
  std::vector<double> expected_fcwnds = {kMinFcwnd, 32, 32, 32};
  uint32_t expected_connection_fcwnd_fixed =
      GetFixedConnectionFcwndFromFloatFlowFcwnds<EventT, ResponseT>(
          expected_fcwnds);
  EXPECT_EQ(this->response_.fabric_congestion_window,
            expected_connection_fcwnd_fixed);
  EXPECT_EQ(this->response_.fabric_window_time_marker, new_now);
  EXPECT_EQ(this->response_.inter_packet_gap, 0);
  CheckFlowWeightsGivenFlowFcwnds<ResponseT>(this->response_, expected_fcwnds);

  // Event 2: fcwnd for flow ID 1 will drop to kMinFcwnd because
  // retransmit_count = retransmit_limit().  fipg will still be 0 because
  // connection fcnwd will still be more than 1.
  this->event_.flow_label = 0x11;  // flow ID is 1
  this->event_.fabric_congestion_window =
      this->response_.fabric_congestion_window;
  swift->Process(this->event_, this->response_, new_now);
  EXPECT_EQ(this->response_.flow_id, 1);
  expected_fcwnds[1] = kMinFcwnd;
  expected_connection_fcwnd_fixed =
      GetFixedConnectionFcwndFromFloatFlowFcwnds<EventT, ResponseT>(
          expected_fcwnds);
  EXPECT_EQ(this->response_.fabric_congestion_window,
            expected_connection_fcwnd_fixed);
  EXPECT_EQ(this->response_.inter_packet_gap, 0);
  CheckFlowWeightsGivenFlowFcwnds<ResponseT>(this->response_, expected_fcwnds);

  // Event 3: fcwnd for flow ID 2 will drop to kMinFcwnd because
  // retransmit_count = retransmit_limit().  fipg will still be 0 because
  // connection fcnwd will still be more than 1.
  this->event_.flow_label = 0x12;  // flow ID is 2
  this->event_.fabric_congestion_window =
      this->response_.fabric_congestion_window;
  swift->Process(this->event_, this->response_, new_now);
  EXPECT_EQ(this->response_.flow_id, 2);
  expected_fcwnds[2] = kMinFcwnd;
  expected_connection_fcwnd_fixed =
      GetFixedConnectionFcwndFromFloatFlowFcwnds<EventT, ResponseT>(
          expected_fcwnds);
  EXPECT_EQ(this->response_.fabric_congestion_window,
            expected_connection_fcwnd_fixed);
  EXPECT_EQ(this->response_.inter_packet_gap, 0);
  CheckFlowWeightsGivenFlowFcwnds<ResponseT>(this->response_, expected_fcwnds);

  // Event 4: fcwnd for flow ID 3 will drop to kMinFcwnd because
  // retransmit_count = retransmit_limit().  fipg will now be nonzero because
  // connection fcnwd will be ~(4*kMinFcwnd)< 1.
  this->event_.flow_label = 0x13;  // flow ID is 3
  this->event_.fabric_congestion_window =
      this->response_.fabric_congestion_window;
  swift->Process(this->event_, this->response_, new_now);
  EXPECT_EQ(this->response_.flow_id, 3);
  expected_fcwnds[3] = kMinFcwnd;
  expected_connection_fcwnd_fixed =
      GetFixedConnectionFcwndFromFloatFlowFcwnds<EventT, ResponseT>(
          expected_fcwnds);
  EXPECT_EQ(this->response_.fabric_congestion_window,
            expected_connection_fcwnd_fixed);
  uint32_t expected_fipg = SwiftGen2::GetInterPacketGap(
      config.ipg_time_scalar(), config.ipg_bits(),
      falcon_rue::FixedToFloat<uint32_t, double>(
          expected_connection_fcwnd_fixed, falcon_rue::kFractionalBits),
      rtt_state);
  EXPECT_EQ(this->response_.inter_packet_gap, expected_fipg);
  CheckFlowWeightsGivenFlowFcwnds<ResponseT>(this->response_, expected_fcwnds);
  // RTO calculation should use fipg.
  uint32_t retransmit_timeout_expected =
      StatefulAlgorithmT::GetRetransmitTimeout(
          config.min_retransmit_timeout(), rtt_state,
          config.retransmit_timeout_scalar(), expected_fipg,
          config.ipg_time_scalar());
  EXPECT_NEAR(this->response_.retransmit_timeout, retransmit_timeout_expected,
              1);

  // Event 5: No change in terms of fcwnds for flows and connection compared to
  // Event 4. However, the rtt_state used to calculate fipg and rto will now be
  // that of flow 0 because it is the maximum rtt_state across all flows.
  this->event_.flow_label = 0x10;  // flow ID is 0
  this->event_.fabric_congestion_window =
      this->response_.fabric_congestion_window;
  // fipg and rto should use the max rtt across all flows - in this case the
  // rtt_state in the event belonging to flow 0.
  this->event_.rtt_state = rtt_state * 2;
  swift->Process(this->event_, this->response_, new_now);
  EXPECT_EQ(this->response_.flow_id, 0);
  EXPECT_EQ(this->response_.fabric_congestion_window,
            expected_connection_fcwnd_fixed);
  expected_fipg = SwiftGen2::GetInterPacketGap(
      config.ipg_time_scalar(), config.ipg_bits(),
      falcon_rue::FixedToFloat<uint32_t, double>(
          expected_connection_fcwnd_fixed, falcon_rue::kFractionalBits),
      rtt_state * 2);
  EXPECT_EQ(this->response_.inter_packet_gap, expected_fipg);
  // RTO calculation should use fipg and (2*rtt_state).
  retransmit_timeout_expected = StatefulAlgorithmT::GetRetransmitTimeout(
      config.min_retransmit_timeout(), 2 * rtt_state,
      config.retransmit_timeout_scalar(), expected_fipg,
      config.ipg_time_scalar());
  EXPECT_NEAR(this->response_.retransmit_timeout, retransmit_timeout_expected,
              1);
}

// Tests that the NIC inter packet gap is correctly calculated for a
// retransmit RUE event for a multipath connection, and that the RTO timeout
// calculation in that case is correct.
TYPED_TEST(SwiftTypedTest, ProcessRetransmitMultipathNipgAndRto) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using StateT = ConnectionState<EventT>;
  using DramStateManagerT = ConnectionBitsDramStateManager<EventT>;
  using AlgorithmT = Swift<EventT, ResponseT>;
  using StatefulAlgorithmT =
      StatefulAlgorithm</*Algorithm=*/AlgorithmT,
                        /*DramStateManagerT=*/DramStateManagerT,
                        /*ForBenchmarking=*/false>;
  SwiftConfiguration config = AlgorithmT::DefaultConfiguration();
  config.set_min_nic_congestion_window(
      0.2);  // set min ncwnd less than 1 to enable nipg pacing
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<StatefulAlgorithmT> swift,
                       AlgorithmT::template Create<StatefulAlgorithmT>(config));

  uint32_t new_now = 100400;
  uint32_t rtt_state = kInitialFlowRttState;
  double ncwnd = config.min_nic_congestion_window();
  // All flows initially have rtt_state values = kInitialFlowRttState, and fcwnd
  // = kInitialFlowFcwnd.

  const uint8_t kMaxConnectionBits = 5;
  const uint32_t kInitialConnectionFcwndFixed = kInitialFlowFcwnd * 4;
  const auto kDefaultStateValue = ConnectionState<EventT>{};
  auto manager = std::make_unique<ConnectionBitsDramStateManager<EventT>>(
      sizeof(StateT), &kDefaultStateValue, kMaxConnectionBits);
  swift->set_dram_state_manager(std::move(manager));

  // Event 1: ncwnd for the connection is less than 1. Therefore, nipg will be
  // nonzero.
  this->event_.connection_id = 1;
  this->event_.flow_label = 0x10;  // flow ID 0
  this->event_.multipath_enable = true;
  this->event_.event_type = falcon::RueEventType::kRetransmit;
  this->event_.retransmit_count = 1;
  this->event_.retransmit_reason = falcon::RetransmitReason::kTimeout;
  this->event_.nack_code = falcon::NackCode::kNotANack;
  this->event_.fabric_congestion_window = kInitialConnectionFcwndFixed;
  this->event_.nic_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(ncwnd,
                                                 falcon_rue::kFractionalBits);
  this->event_.rtt_state = rtt_state;

  swift->Process(this->event_, this->response_, new_now);
  EXPECT_EQ(this->response_.flow_id, 0);
  EXPECT_EQ(this->response_.inter_packet_gap,
            0);  // connection fcwnd still larger than 1, so fipg should be 0
  EXPECT_EQ(this->response_.rtt_state, rtt_state);
  EXPECT_EQ(this->response_.nic_congestion_window,
            this->event_.nic_congestion_window);  // ncwnd reflected as is for
                                                  // retransmit events
  double expected_nipg =
      std::round((rtt_state / ncwnd - rtt_state) * config.ipg_time_scalar());
  EXPECT_NEAR(this->response_.nic_inter_packet_gap,
              static_cast<uint64_t>(expected_nipg), 1);
  // RTO calculation should use the NIC ipg.
  uint32_t retransmit_timeout_expected =
      StatefulAlgorithmT::GetRetransmitTimeout(
          config.min_retransmit_timeout(), rtt_state,
          config.retransmit_timeout_scalar(), expected_nipg,
          config.ipg_time_scalar());
  EXPECT_NEAR(this->response_.retransmit_timeout, retransmit_timeout_expected,
              1);

  // Event 2: No change in terms of ncwnd compared to Event 1. However, the
  // rtt_state used to calculate nipg and rto will now be that of flow 0 because
  // it is the maximum rtt_state across all flows.
  this->event_.rtt_state = 2 * rtt_state;
  swift->Process(this->event_, this->response_, new_now);
  // nipg and rto should use the max rtt across all flows - in this case the
  // rtt_state in the event belonging to flow 0.
  expected_nipg = std::round((2 * rtt_state / ncwnd - 2 * rtt_state) *
                             config.ipg_time_scalar());
  EXPECT_NEAR(this->response_.nic_inter_packet_gap,
              static_cast<uint64_t>(expected_nipg), 1);
  // RTO calculation should use the NIC ipg and (2*rtt_state).
  retransmit_timeout_expected = StatefulAlgorithmT::GetRetransmitTimeout(
      config.min_retransmit_timeout(), 2 * rtt_state,
      config.retransmit_timeout_scalar(), expected_nipg,
      config.ipg_time_scalar());
  EXPECT_NEAR(this->response_.retransmit_timeout, retransmit_timeout_expected,
              1);
}

// Tests that the flow labels for retransmit events with multipath connections
// are the default flow label values.
TYPED_TEST(SwiftTypedTest, ProcessRetransmitMultipathFlowLabels) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using StateT = ConnectionState<EventT>;
  using DramStateManagerT = ConnectionBitsDramStateManager<EventT>;
  using AlgorithmT = Swift<EventT, ResponseT>;
  using StatefulAlgorithmT =
      StatefulAlgorithm</*Algorithm=*/AlgorithmT,
                        /*DramStateManagerT=*/DramStateManagerT,
                        /*ForBenchmarking=*/false>;
  SwiftConfiguration config = AlgorithmT::DefaultConfiguration();
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<StatefulAlgorithmT> swift,
                       AlgorithmT::template Create<StatefulAlgorithmT>(config));

  uint32_t new_now = 100400;
  uint32_t rtt_state = kInitialFlowRttState;
  // All flows initially have rtt_state values = kInitialFlowRttState, and fcwnd
  // = kInitialFlowFcwnd.

  const uint8_t kMaxConnectionBits = 5;
  const uint32_t kInitialConnectionFcwndFixed = kInitialFlowFcwnd * 4;
  const auto kDefaultStateValue = ConnectionState<EventT>{};
  auto manager = std::make_unique<ConnectionBitsDramStateManager<EventT>>(
      sizeof(StateT), &kDefaultStateValue, kMaxConnectionBits);
  swift->set_dram_state_manager(std::move(manager));

  this->event_.connection_id = 1;
  this->event_.flow_label = 0x10;  // flow ID 0
  this->event_.multipath_enable = true;
  this->event_.event_type = falcon::RueEventType::kRetransmit;
  this->event_.retransmit_count = 1;
  this->event_.retransmit_reason = falcon::RetransmitReason::kTimeout;
  this->event_.nack_code = falcon::NackCode::kNotANack;
  this->event_.fabric_congestion_window = kInitialConnectionFcwndFixed;
  this->event_.nic_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(32,
                                                 falcon_rue::kFractionalBits);
  this->event_.rtt_state = rtt_state;

  swift->Process(this->event_, this->response_, new_now);
  EXPECT_EQ(this->response_.flow_id, 0);
  // Flow labels for retransmit events with multipath connections should be the
  // default flow labels for each flow, and the corresponding flow label valid
  // bit should be false.
  EXPECT_EQ(this->response_.flow_label_1, kDefaultFlowLabel1);
  EXPECT_EQ(this->response_.flow_label_2, kDefaultFlowLabel2);
  EXPECT_EQ(this->response_.flow_label_3, kDefaultFlowLabel3);
  EXPECT_EQ(this->response_.flow_label_4, kDefaultFlowLabel4);
  EXPECT_EQ(this->response_.flow_label_1_valid, false);
  EXPECT_EQ(this->response_.flow_label_2_valid, false);
  EXPECT_EQ(this->response_.flow_label_3_valid, false);
  EXPECT_EQ(this->response_.flow_label_4_valid, false);
}

// Tests that fcwnd and fcwnd_guard are properly calculated for multipath
// connections when processing ACK events that should cause fcwnd to decrease.
TYPED_TEST(SwiftTypedTest, ProcessAckMultipathFcwndDecrease) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using StateT = ConnectionState<EventT>;
  using DramStateManagerT = ConnectionBitsDramStateManager<EventT>;
  using AlgorithmT = Swift<EventT, ResponseT>;
  using StatefulAlgorithmT =
      StatefulAlgorithm</*Algorithm=*/AlgorithmT,
                        /*DramStateManagerT=*/DramStateManagerT,
                        /*ForBenchmarking=*/false>;
  SwiftConfiguration config = AlgorithmT::DefaultConfiguration();
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<StatefulAlgorithmT> swift,
                       AlgorithmT::template Create<StatefulAlgorithmT>(config));

  uint32_t new_now = 100400;
  uint32_t rtt_state_flow_0 = kInitialFlowRttState;
  uint32_t rtt_state_event = 2 * kInitialFlowRttState;
  // All flows initially have rtt_state values = kInitialFlowRttState, and fcwnd
  // = kInitialFlowFcwnd.

  const uint8_t kMaxConnectionBits = 5;
  const uint32_t kInitialConnectionFcwndFixed = kInitialFlowFcwnd * 4;
  const double kInitialFlowFcwndFloat =
      falcon_rue::FixedToFloat<uint32_t, double>(kInitialFlowFcwnd,
                                                 falcon_rue::kFractionalBits);
  const auto kDefaultStateValue = ConnectionState<EventT>{};
  auto manager = std::make_unique<ConnectionBitsDramStateManager<EventT>>(
      sizeof(StateT), &kDefaultStateValue, kMaxConnectionBits);
  swift->set_dram_state_manager(std::move(manager));

  // Event 1: No decrease in fcwnd because this event belongs to flow 0, and
  // the fcwnd guard is less than an RTT away.
  this->event_.connection_id = 1;
  this->event_.flow_label = 0x10;  // flow ID is 0
  this->event_.multipath_enable = true;
  this->event_.event_type = falcon::RueEventType::kAck;
  this->event_.nack_code = falcon::NackCode::kNotANack;
  this->event_.timestamp_1 = 100000;
  this->event_.timestamp_2 = 100879;
  this->event_.timestamp_3 = 101000;
  this->event_.timestamp_4 = this->event_.timestamp_1 + rtt_state_event;
  this->event_.fabric_congestion_window = kInitialConnectionFcwndFixed;
  this->event_.fabric_window_time_marker =
      new_now - rtt_state_event + 10;  // fcwnd decrease should not happen.
  this->event_.nic_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(10,
                                                 falcon_rue::kFractionalBits);
  this->event_.num_packets_acked = 3;
  this->event_.delay_select = falcon::DelaySelect::kForward;
  this->event_.forward_hops = 5;
  this->event_.base_delay = AlgorithmT::MakeBaseDelayField(
      /*profile_index=*/kSwiftDefaultProfileIndex);
  this->event_.rtt_state = rtt_state_flow_0;
  this->event_.delay_state = 0;
  this->event_.cc_metadata = 0;

  swift->Process(this->event_, this->response_, new_now);
  EXPECT_EQ(this->response_.flow_id, 0);
  EXPECT_EQ(this->response_.fabric_congestion_window,
            this->event_.fabric_congestion_window);
  EXPECT_EQ(this->response_.fabric_window_time_marker,
            this->event_.fabric_window_time_marker);
  uint32_t expected_rtt_state_flow0 = AlgorithmT::GetSmoothed(
      config.rtt_smoothing_alpha(), rtt_state_flow_0, rtt_state_event);
  EXPECT_EQ(this->response_.rtt_state, expected_rtt_state_flow0);
  EXPECT_EQ(this->response_.inter_packet_gap, 0);  // fcwnd > 1
  std::vector<double> expected_fcwnds = {
      kInitialFlowFcwndFloat, kInitialFlowFcwndFloat, kInitialFlowFcwndFloat,
      kInitialFlowFcwndFloat};
  CheckFlowWeightsGivenFlowFcwnds<ResponseT>(this->response_, expected_fcwnds);

  // Event 2: A decrease in fcwnd will take place because this event belongs to
  // flow 0, and the fcwnd guard in the RUE event is more than an RTT away.
  this->event_.fabric_window_time_marker =
      new_now - rtt_state_event - 10;  // fcwnd decrease should now happen.
  swift->Process(this->event_, this->response_, new_now);
  // With the given parameters, fcwnd for flow 0 decreases by the maximum MD.
  double expected_fcwnd_flow_0 =
      kInitialFlowFcwndFloat *
      (1 - config.max_fabric_multiplicative_decrease());
  double expected_fcwnd_connection =
      falcon_rue::FixedToFloat<uint32_t, double>(kInitialConnectionFcwndFixed,
                                                 falcon_rue::kFractionalBits) -
      kInitialFlowFcwndFloat + expected_fcwnd_flow_0;
  uint32_t expected_fcwnd_connection_fixed =
      falcon_rue::FloatToFixed<double, uint32_t>(expected_fcwnd_connection,
                                                 falcon_rue::kFractionalBits);
  EXPECT_EQ(this->response_.flow_id, 0);
  EXPECT_EQ(this->response_.fabric_congestion_window,
            expected_fcwnd_connection_fixed);
  EXPECT_EQ(this->response_.rtt_state, expected_rtt_state_flow0);
  EXPECT_EQ(this->response_.fabric_window_time_marker, new_now);
  EXPECT_EQ(this->response_.inter_packet_gap, 0);  // fcwnd > 1
  expected_fcwnds[0] = expected_fcwnd_flow_0;
  CheckFlowWeightsGivenFlowFcwnds<ResponseT>(this->response_, expected_fcwnds);

  // Event 3: A decrease in fcwnd will take place because this event belongs to
  // flow 1, and the fcwnd guard in RUE state (0) is more than an RTT away.
  this->event_.eack = true;
  this->event_.eack_drop = true;  // max fcwnd decrease should happen when eack
                                  // and eack_drop are true.
  this->event_.fabric_congestion_window =
      this->response_.fabric_congestion_window;
  this->event_.flow_label = 0x11;  // now an event for flow ID 1 arrives
  swift->Process(this->event_, this->response_, new_now);
  EXPECT_EQ(this->response_.flow_id, 1);
  // Expect same connection fcwnd decrease as in Event 2.
  uint32_t connection_fcwnd_decrease =
      kInitialConnectionFcwndFixed - expected_fcwnd_connection_fixed;
  EXPECT_EQ(this->response_.fabric_congestion_window,
            this->event_.fabric_congestion_window - connection_fcwnd_decrease);
  // The fcwnd guard and rtt_state in the response belong to flow 0, so expect
  // no change when the event belongs to flow 1.
  EXPECT_EQ(this->response_.fabric_window_time_marker,
            this->event_.fabric_window_time_marker);
  EXPECT_EQ(this->response_.rtt_state, this->event_.rtt_state);
  EXPECT_EQ(this->response_.inter_packet_gap, 0);  // fcwnd > 1
  expected_fcwnds[1] = expected_fcwnd_flow_0;
  CheckFlowWeightsGivenFlowFcwnds<ResponseT>(this->response_, expected_fcwnds);

  // Event 4: No decrease in fcwnd because this event belongs to flow 1, and the
  // fcwnd guard in RUE state (new_now) is less than an RTT away.
  this->event_.fabric_congestion_window =
      this->response_.fabric_congestion_window;
  swift->Process(this->event_, this->response_, new_now);
  EXPECT_EQ(this->response_.flow_id, 1);
  EXPECT_EQ(this->response_.fabric_congestion_window,
            this->event_.fabric_congestion_window);
  // The fcwnd guard and rtt_state in the response belong to flow 0, so expect
  // no change when the event belongs to flow 1.
  EXPECT_EQ(this->response_.fabric_window_time_marker,
            this->event_.fabric_window_time_marker);
  EXPECT_EQ(this->response_.rtt_state, this->event_.rtt_state);
  EXPECT_EQ(this->response_.inter_packet_gap, 0);  // fcwnd > 1
  CheckFlowWeightsGivenFlowFcwnds<ResponseT>(this->response_, expected_fcwnds);
}

// Tests that fcwnd and fcwnd_guard are properly calculated for multipath
// connections when processing ACK events that should cause fcwnd to increase.
TYPED_TEST(SwiftTypedTest, ProcessAckMultipathFcwndIncrease) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using StateT = ConnectionState<EventT>;
  using DramStateManagerT = ConnectionBitsDramStateManager<EventT>;
  using AlgorithmT = Swift<EventT, ResponseT>;
  using StatefulAlgorithmT =
      StatefulAlgorithm</*Algorithm=*/AlgorithmT,
                        /*DramStateManagerT=*/DramStateManagerT,
                        /*ForBenchmarking=*/false>;
  SwiftConfiguration config = AlgorithmT::DefaultConfiguration();
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<StatefulAlgorithmT> swift,
                       AlgorithmT::template Create<StatefulAlgorithmT>(config));

  uint32_t new_now = 100400;
  uint32_t rtt_state_flow_0 = kInitialFlowRttState;
  uint32_t rtt_state_event = 2 * kInitialFlowRttState;
  // All flows initially have rtt_state values = kInitialFlowRttState, and
  // fcwnd = kInitialFlowFcwnd.

  const uint8_t kMaxConnectionBits = 5;
  const uint32_t kInitialConnectionFcwndFixed = kInitialFlowFcwnd * 4;
  const double kInitialFlowFcwndFloat =
      falcon_rue::FixedToFloat<uint32_t, double>(kInitialFlowFcwnd,
                                                 falcon_rue::kFractionalBits);
  const auto kDefaultStateValue = ConnectionState<EventT>{};
  auto manager = std::make_unique<ConnectionBitsDramStateManager<EventT>>(
      sizeof(StateT), &kDefaultStateValue, kMaxConnectionBits);
  swift->set_dram_state_manager(std::move(manager));

  // Event 1: Additive increase in flow 0 fcwnd.
  this->event_.connection_id = 1;
  this->event_.flow_label = 0x10;  // flow ID is 0
  this->event_.multipath_enable = true;
  this->event_.event_type = falcon::RueEventType::kAck;
  this->event_.nack_code = falcon::NackCode::kNotANack;
  this->event_.timestamp_1 = 100000;
  this->event_.timestamp_2 = 100005;
  this->event_.timestamp_3 = 100010;
  this->event_.timestamp_4 = this->event_.timestamp_1 + rtt_state_event;
  this->event_.fabric_congestion_window = kInitialConnectionFcwndFixed;
  this->event_.fabric_window_time_marker = new_now - 2 * rtt_state_event;
  this->event_.nic_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(10,
                                                 falcon_rue::kFractionalBits);
  this->event_.num_packets_acked = 4;
  this->event_.delay_select = falcon::DelaySelect::kForward;
  this->event_.forward_hops = 5;
  this->event_.base_delay = AlgorithmT::MakeBaseDelayField(
      /*profile_index=*/kSwiftDefaultProfileIndex);
  this->event_.rtt_state = rtt_state_flow_0;
  this->event_.delay_state = 0;
  this->event_.cc_metadata = 0;

  swift->Process(this->event_, this->response_, new_now);
  EXPECT_EQ(this->response_.flow_id, 0);
  uint32_t expected_rtt_state_flow0 = AlgorithmT::GetSmoothed(
      config.rtt_smoothing_alpha(), rtt_state_flow_0, rtt_state_event);
  EXPECT_EQ(this->response_.rtt_state, expected_rtt_state_flow0);

  double increase = config.fabric_additive_increment_factor() *
                    this->event_.num_packets_acked;
  increase /= kInitialFlowFcwndFloat;
  double expected_fcwnd_flow_0 = kInitialFlowFcwndFloat + increase;
  double expected_fcwnd_connection =
      falcon_rue::FixedToFloat<uint32_t, double>(kInitialConnectionFcwndFixed,
                                                 falcon_rue::kFractionalBits) -
      kInitialFlowFcwndFloat + expected_fcwnd_flow_0;
  uint32_t expected_fcwnd_connection_fixed =
      falcon_rue::FloatToFixed<double, uint32_t>(expected_fcwnd_connection,
                                                 falcon_rue::kFractionalBits);
  EXPECT_EQ(this->response_.fabric_congestion_window,
            expected_fcwnd_connection_fixed);
  EXPECT_EQ(this->response_.inter_packet_gap, 0);  // fcwnd > 1
  // Even though no decrease in fcwnd occurs, the fcwnd guard is updated to be
  // now - instantaneous_rtt.
  EXPECT_EQ(this->response_.fabric_window_time_marker,
            new_now - rtt_state_event);
  std::vector<double> expected_fcwnds = {
      expected_fcwnd_flow_0, kInitialFlowFcwndFloat, kInitialFlowFcwndFloat,
      kInitialFlowFcwndFloat};
  CheckFlowWeightsGivenFlowFcwnds<ResponseT>(this->response_, expected_fcwnds);

  // Event 2: Additive increase in flow 2 fcwnd.
  this->event_.fabric_congestion_window =
      this->response_.fabric_congestion_window;
  this->event_.fabric_window_time_marker =
      this->response_.fabric_window_time_marker;
  this->event_.num_packets_acked = 16;
  this->event_.flow_label = 0x12;  // now an event for flow ID 2 arrives
  swift->Process(this->event_, this->response_, new_now);
  EXPECT_EQ(this->response_.flow_id, 2);
  increase = config.fabric_additive_increment_factor() *
             this->event_.num_packets_acked;
  increase /= kInitialFlowFcwndFloat;
  double expected_fcwnd_flow_2 = kInitialFlowFcwndFloat + increase;
  expected_fcwnd_connection = expected_fcwnd_connection -
                              kInitialFlowFcwndFloat + expected_fcwnd_flow_2;
  expected_fcwnd_connection_fixed = falcon_rue::FloatToFixed<double, uint32_t>(
      expected_fcwnd_connection, falcon_rue::kFractionalBits);
  EXPECT_EQ(this->response_.fabric_congestion_window,
            expected_fcwnd_connection_fixed);
  // The fcwnd guard and rtt_state in the response belong to flow 0, so expect
  // no change when the event belongs to flow 2.
  EXPECT_EQ(this->response_.fabric_window_time_marker,
            this->event_.fabric_window_time_marker);
  EXPECT_EQ(this->response_.rtt_state, this->event_.rtt_state);
  EXPECT_EQ(this->response_.inter_packet_gap, 0);  // fcwnd > 1
  expected_fcwnds[2] = expected_fcwnd_flow_2;
  CheckFlowWeightsGivenFlowFcwnds<ResponseT>(this->response_, expected_fcwnds);
}

// Tests that fcwnd and fcwnd_guard are properly calculated for multipath
// connections when processing NACK events that should cause fcwnd to decrease.
TYPED_TEST(SwiftTypedTest, ProcessNackMultipathFcwndDecrease) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using StateT = ConnectionState<EventT>;
  using DramStateManagerT = ConnectionBitsDramStateManager<EventT>;
  using AlgorithmT = Swift<EventT, ResponseT>;
  using StatefulAlgorithmT =
      StatefulAlgorithm</*Algorithm=*/AlgorithmT,
                        /*DramStateManagerT=*/DramStateManagerT,
                        /*ForBenchmarking=*/false>;
  SwiftConfiguration config = AlgorithmT::DefaultConfiguration();
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<StatefulAlgorithmT> swift,
                       AlgorithmT::template Create<StatefulAlgorithmT>(config));

  uint32_t new_now = 100400;
  uint32_t rtt_state_flow_0 = kInitialFlowRttState;
  uint32_t rtt_state_event = 2 * kInitialFlowRttState;
  // All flows initially have rtt_state values = kInitialFlowRttState, and fcwnd
  // = kInitialFlowFcwnd.

  const uint8_t kMaxConnectionBits = 5;
  const uint32_t kInitialConnectionFcwndFixed = kInitialFlowFcwnd * 4;
  const double kInitialFlowFcwndFloat =
      falcon_rue::FixedToFloat<uint32_t, double>(kInitialFlowFcwnd,
                                                 falcon_rue::kFractionalBits);
  const auto kDefaultStateValue = ConnectionState<EventT>{};
  auto manager = std::make_unique<ConnectionBitsDramStateManager<EventT>>(
      sizeof(StateT), &kDefaultStateValue, kMaxConnectionBits);
  swift->set_dram_state_manager(std::move(manager));

  // Event 1: No decrease in fcwnd because this event belongs to flow 0, and
  // the fcwnd guard is less than an RTT away.
  this->event_.connection_id = 1;
  this->event_.flow_label = 0x10;  // flow ID is 0
  this->event_.multipath_enable = true;
  this->event_.event_type = falcon::RueEventType::kNack;
  this->event_.nack_code =
      falcon::NackCode::kReserved;  // use max fcwnd decrease
  this->event_.timestamp_1 = 100000;
  this->event_.timestamp_2 = 100879;
  this->event_.timestamp_3 = 101000;
  this->event_.timestamp_4 = this->event_.timestamp_1 + rtt_state_event;
  this->event_.fabric_congestion_window = kInitialConnectionFcwndFixed;
  this->event_.fabric_window_time_marker =
      new_now - rtt_state_event + 10;  // fcwnd decrease should not happen.
  this->event_.nic_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(10,
                                                 falcon_rue::kFractionalBits);
  this->event_.num_packets_acked = 3;
  this->event_.delay_select = falcon::DelaySelect::kForward;
  this->event_.forward_hops = 5;
  this->event_.base_delay = AlgorithmT::MakeBaseDelayField(
      /*profile_index=*/kSwiftDefaultProfileIndex);
  this->event_.rtt_state = rtt_state_flow_0;
  this->event_.delay_state = 0;
  this->event_.cc_metadata = 0;

  swift->Process(this->event_, this->response_, new_now);
  EXPECT_EQ(this->response_.flow_id, 0);
  EXPECT_EQ(this->response_.fabric_congestion_window,
            this->event_.fabric_congestion_window);
  EXPECT_EQ(this->response_.fabric_window_time_marker,
            this->event_.fabric_window_time_marker);
  uint32_t expected_rtt_state_flow0 = AlgorithmT::GetSmoothed(
      config.rtt_smoothing_alpha(), rtt_state_flow_0, rtt_state_event);
  EXPECT_EQ(this->response_.rtt_state, expected_rtt_state_flow0);
  EXPECT_EQ(this->response_.inter_packet_gap, 0);  // fcwnd > 1
  std::vector<double> expected_fcwnds = {
      kInitialFlowFcwndFloat, kInitialFlowFcwndFloat, kInitialFlowFcwndFloat,
      kInitialFlowFcwndFloat};
  CheckFlowWeightsGivenFlowFcwnds<ResponseT>(this->response_, expected_fcwnds);

  // Event 2: A decrease in fcwnd will take place because this event belongs to
  // flow 0, and the fcwnd guard in the RUE event is more than an RTT away.
  this->event_.fabric_window_time_marker =
      new_now - rtt_state_event - 10;  // fcwnd decrease should now happen.
  swift->Process(this->event_, this->response_, new_now);
  // With the given parameters, fcwnd for flow 0 decreases by the maximum MD.
  double expected_fcwnd_flow_0 =
      kInitialFlowFcwndFloat *
      (1 - config.max_fabric_multiplicative_decrease());
  double expected_fcwnd_connection =
      falcon_rue::FixedToFloat<uint32_t, double>(kInitialConnectionFcwndFixed,
                                                 falcon_rue::kFractionalBits) -
      kInitialFlowFcwndFloat + expected_fcwnd_flow_0;
  uint32_t expected_fcwnd_connection_fixed =
      falcon_rue::FloatToFixed<double, uint32_t>(expected_fcwnd_connection,
                                                 falcon_rue::kFractionalBits);
  EXPECT_EQ(this->response_.flow_id, 0);
  EXPECT_EQ(this->response_.fabric_congestion_window,
            expected_fcwnd_connection_fixed);
  EXPECT_EQ(this->response_.rtt_state, expected_rtt_state_flow0);
  EXPECT_EQ(this->response_.fabric_window_time_marker, new_now);
  EXPECT_EQ(this->response_.inter_packet_gap, 0);  // fcwnd > 1
  expected_fcwnds[0] = expected_fcwnd_flow_0;
  CheckFlowWeightsGivenFlowFcwnds<ResponseT>(this->response_, expected_fcwnds);

  // Event 3: A decrease in fcwnd will take place because this event belongs to
  // flow 1, and the fcwnd guard in RUE state (0) is more than an RTT away.
  this->event_.fabric_congestion_window =
      this->response_.fabric_congestion_window;
  this->event_.flow_label = 0x11;  // now an event for flow ID 1 arrives
  swift->Process(this->event_, this->response_, new_now);
  EXPECT_EQ(this->response_.flow_id, 1);
  // Expect same connection fcwnd decrease as in Event 2.
  uint32_t connection_fcwnd_decrease =
      kInitialConnectionFcwndFixed - expected_fcwnd_connection_fixed;
  EXPECT_EQ(this->response_.fabric_congestion_window,
            this->event_.fabric_congestion_window - connection_fcwnd_decrease);
  // The fcwnd guard and rtt_state in the response belong to flow 0, so expect
  // no change when the event belongs to flow 1.
  EXPECT_EQ(this->response_.fabric_window_time_marker,
            this->event_.fabric_window_time_marker);
  EXPECT_EQ(this->response_.rtt_state, this->event_.rtt_state);
  EXPECT_EQ(this->response_.inter_packet_gap, 0);  // fcwnd > 1
  expected_fcwnds[1] = expected_fcwnd_flow_0;
  CheckFlowWeightsGivenFlowFcwnds<ResponseT>(this->response_, expected_fcwnds);
}

// Tests that fipg is properly calculated for multipath connections during
// ACK/NACK events, and that the RTO timeout calculation in that case is
// correct.
TYPED_TEST(SwiftTypedTest, ProcessNackMultipathFipgAndRto) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using StateT = ConnectionState<EventT>;
  using DramStateManagerT = ConnectionBitsDramStateManager<EventT>;
  using AlgorithmT = Swift<EventT, ResponseT>;
  using StatefulAlgorithmT =
      StatefulAlgorithm</*Algorithm=*/AlgorithmT,
                        /*DramStateManagerT=*/DramStateManagerT,
                        /*ForBenchmarking=*/false>;
  const double kMinFcwnd = 0.1;
  SwiftConfiguration config = AlgorithmT::DefaultConfiguration();
  config.set_min_fabric_congestion_window(kMinFcwnd);
  config.set_max_fabric_multiplicative_decrease(
      0.995);  // making the max MD large enough so that one such decrease
  // brings the fcwnd to less than 1
  config.set_max_decrease_on_eack_nack_drop(true);
  config.set_rtt_smoothing_alpha(0);  // use latest rtt sample as rtt_state
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<StatefulAlgorithmT> swift,
                       AlgorithmT::template Create<StatefulAlgorithmT>(config));

  uint32_t new_now = 100400;
  // All flows initially have rtt_state values = kInitialFlowRttState, and fcwnd
  // = kInitialFlowFcwnd.

  const uint8_t kMaxConnectionBits = 5;
  const uint32_t kInitialConnectionFcwndFixed = kInitialFlowFcwnd * 4;
  const double kInitialFlowFcwndFloat =
      falcon_rue::FixedToFloat<uint32_t, double>(kInitialFlowFcwnd,
                                                 falcon_rue::kFractionalBits);
  const auto kDefaultStateValue = ConnectionState<EventT>{};
  auto manager = std::make_unique<ConnectionBitsDramStateManager<EventT>>(
      sizeof(StateT), &kDefaultStateValue, kMaxConnectionBits);
  swift->set_dram_state_manager(std::move(manager));

  // Event 1: fcwnd for flow ID 0 will drop to below 1 because of the max fcwnd
  // decrease factor. fipg will still be 0 because connection fcnwd will still
  // be more than 1.
  this->event_.connection_id = 1;
  this->event_.flow_label = 0x10;  // flow ID is 0
  this->event_.multipath_enable = true;
  this->event_.event_type = falcon::RueEventType::kAck;
  this->event_.eack = true;
  this->event_.eack_drop = true;  // use max fcwnd decrease
  this->event_.nack_code = falcon::NackCode::kNotANack;
  this->event_.timestamp_1 = 100000;
  this->event_.timestamp_2 = 100879;
  this->event_.timestamp_3 = 101000;
  this->event_.timestamp_4 = this->event_.timestamp_1 + kInitialFlowRttState;
  this->event_.fabric_congestion_window = kInitialConnectionFcwndFixed;
  this->event_.fabric_window_time_marker =
      new_now - kInitialFlowRttState - 10;  // fcwnd decrease should happen.
  this->event_.nic_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(10,
                                                 falcon_rue::kFractionalBits);
  this->event_.num_packets_acked = 3;
  this->event_.delay_select = falcon::DelaySelect::kForward;
  this->event_.forward_hops = 5;
  this->event_.base_delay = AlgorithmT::MakeBaseDelayField(
      /*profile_index=*/kSwiftDefaultProfileIndex);
  this->event_.rtt_state = kInitialFlowRttState;
  this->event_.delay_state = 0;
  this->event_.cc_metadata = 0;
  swift->Process(this->event_, this->response_, new_now);
  EXPECT_EQ(this->response_.flow_id, 0);
  // With the given parameters, fcwnd for flow 0 decreases by the maximum MD.
  double expected_fcwnd_flow_0 =
      kInitialFlowFcwndFloat *
      (1 - config.max_fabric_multiplicative_decrease());
  std::vector<double> expected_fcwnds = {
      expected_fcwnd_flow_0, kInitialFlowFcwndFloat, kInitialFlowFcwndFloat,
      kInitialFlowFcwndFloat};
  uint32_t expected_connection_fcwnd_fixed =
      GetFixedConnectionFcwndFromFloatFlowFcwnds<EventT, ResponseT>(
          expected_fcwnds);
  EXPECT_EQ(this->response_.fabric_congestion_window,
            expected_connection_fcwnd_fixed);
  EXPECT_EQ(this->response_.fabric_window_time_marker, new_now);
  EXPECT_EQ(this->response_.inter_packet_gap, 0);
  CheckFlowWeightsGivenFlowFcwnds<ResponseT>(this->response_, expected_fcwnds);

  // Event 2: fcwnd for flow ID 1 will drop to below 1 because of the max fcwnd
  // decrease factor. fipg will still be 0 because connection fcnwd will still
  // be more than 1.
  this->event_.fabric_congestion_window =
      this->response_.fabric_congestion_window;
  this->event_.flow_label = 0x11;  // flow ID is 1
  swift->Process(this->event_, this->response_, new_now);
  EXPECT_EQ(this->response_.flow_id, 1);
  expected_fcwnds[1] = expected_fcwnd_flow_0;
  expected_connection_fcwnd_fixed =
      GetFixedConnectionFcwndFromFloatFlowFcwnds<EventT, ResponseT>(
          expected_fcwnds);
  EXPECT_EQ(this->response_.fabric_congestion_window,
            expected_connection_fcwnd_fixed);
  EXPECT_EQ(this->response_.inter_packet_gap, 0);
  CheckFlowWeightsGivenFlowFcwnds<ResponseT>(this->response_, expected_fcwnds);

  // Event 3: fcwnd for flow ID 2 will drop to below 1 because of the max fcwnd
  // decrease factor. fipg will still be 0 because connection fcnwd will still
  // be more than 1.
  this->event_.fabric_congestion_window =
      this->response_.fabric_congestion_window;
  this->event_.flow_label = 0x12;  // flow ID is 2
  this->event_.timestamp_4 =
      this->event_.timestamp_1 + 3 * kInitialFlowRttState;  // max rtt_state
  swift->Process(this->event_, this->response_, new_now);
  EXPECT_EQ(this->response_.flow_id, 2);
  expected_fcwnds[2] = expected_fcwnd_flow_0;
  expected_connection_fcwnd_fixed =
      GetFixedConnectionFcwndFromFloatFlowFcwnds<EventT, ResponseT>(
          expected_fcwnds);
  EXPECT_EQ(this->response_.fabric_congestion_window,
            expected_connection_fcwnd_fixed);
  EXPECT_EQ(this->response_.inter_packet_gap, 0);
  CheckFlowWeightsGivenFlowFcwnds<ResponseT>(this->response_, expected_fcwnds);

  // Event 4: fcwnd for flow ID 3 will drop to below 1 because of the max fcwnd
  // decrease factor. fipg will now be more than 0 because connection fcnwd will
  // be less than 1.
  this->event_.fabric_congestion_window =
      this->response_.fabric_congestion_window;
  this->event_.flow_label = 0x13;  // flow ID is 3
  this->event_.timestamp_4 =
      this->event_.timestamp_1 + 2 * kInitialFlowRttState;  // 2nd max rtt_state
  swift->Process(this->event_, this->response_, new_now);
  EXPECT_EQ(this->response_.flow_id, 3);
  expected_fcwnds[3] = expected_fcwnd_flow_0;
  expected_connection_fcwnd_fixed =
      GetFixedConnectionFcwndFromFloatFlowFcwnds<EventT, ResponseT>(
          expected_fcwnds);
  EXPECT_EQ(this->response_.fabric_congestion_window,
            expected_connection_fcwnd_fixed);
  EXPECT_GT(this->response_.inter_packet_gap, 0);
  uint32_t expected_max_rtt_state =
      3 * kInitialFlowRttState;  // belongs to flow 2 in Event 3
  uint32_t expected_fipg = SwiftGen2::GetInterPacketGap(
      config.ipg_time_scalar(), config.ipg_bits(),
      falcon_rue::FixedToFloat<uint32_t, double>(
          expected_connection_fcwnd_fixed, falcon_rue::kFractionalBits),
      expected_max_rtt_state);
  EXPECT_EQ(this->response_.inter_packet_gap, expected_fipg);
  CheckFlowWeightsGivenFlowFcwnds<ResponseT>(this->response_, expected_fcwnds);
  // RTO calculation should use fipg and should use the max rtt across all
  // flows, in this case the rtt_state in Event 3 belonging to flow 2.
  uint32_t retransmit_timeout_expected =
      StatefulAlgorithmT::GetRetransmitTimeout(
          config.min_retransmit_timeout(), expected_max_rtt_state,
          config.retransmit_timeout_scalar(), expected_fipg,
          config.ipg_time_scalar());
  EXPECT_NEAR(this->response_.retransmit_timeout, retransmit_timeout_expected,
              1);
}

// Tests the case where ncwnd should increase when processing an ACK for
// multipath connections.
TYPED_TEST(SwiftTypedTest, ProcessAckMultipathNcwndIncrease) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using StateT = ConnectionState<EventT>;
  using DramStateManagerT = ConnectionBitsDramStateManager<EventT>;
  using AlgorithmT = Swift<EventT, ResponseT>;
  using StatefulAlgorithmT =
      StatefulAlgorithm</*Algorithm=*/AlgorithmT,
                        /*DramStateManagerT=*/DramStateManagerT,
                        /*ForBenchmarking=*/false>;
  SwiftConfiguration config = AlgorithmT::DefaultConfiguration();
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<StatefulAlgorithmT> swift,
                       AlgorithmT::template Create<StatefulAlgorithmT>(config));

  uint32_t new_now = 100400;
  // All flows initially have rtt_state values = kInitialFlowRttState, and fcwnd
  // = kInitialFlowFcwnd.

  const uint8_t kMaxConnectionBits = 5;
  const uint32_t kInitialConnectionFcwndFixed = kInitialFlowFcwnd * 4;
  const auto kDefaultStateValue = ConnectionState<EventT>{};
  auto manager = std::make_unique<ConnectionBitsDramStateManager<EventT>>(
      sizeof(StateT), &kDefaultStateValue, kMaxConnectionBits);
  swift->set_dram_state_manager(std::move(manager));
  const double ncwnd = 100.0;

  // Event 1: rx_buffer_level is less than the target, so ncwnd will increase.
  // The ncwnd guard will also be advanced to be one RTT away when it is more
  // than one RTT away.
  this->event_.connection_id = 1;
  this->event_.flow_label = 0x10;  // flow ID is 0
  this->event_.multipath_enable = true;
  this->event_.event_type = falcon::RueEventType::kAck;
  this->event_.timestamp_1 = 100000;
  this->event_.timestamp_2 = 100153;
  this->event_.timestamp_3 = 100200;
  this->event_.timestamp_4 = kInitialFlowRttState + this->event_.timestamp_1;
  this->event_.nack_code = falcon::NackCode::kNotANack;
  this->event_.forward_hops = 5;
  this->event_.rx_buffer_level = static_cast<uint8_t>(
      config.target_rx_buffer_level() - 1);  // rx_buffer_level less than target
  this->event_.fabric_congestion_window = kInitialConnectionFcwndFixed;
  this->event_.nic_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(ncwnd,
                                                 falcon_rue::kFractionalBits);
  this->event_.num_packets_acked = 3;
  this->event_.delay_select = falcon::DelaySelect::kForward;
  this->event_.nic_window_time_marker =
      new_now - kInitialFlowRttState -
      10;  // ncwnd time marker is more than one RTT away
  this->event_.base_delay = AlgorithmT::MakeBaseDelayField(
      /*profile_index=*/kSwiftDefaultProfileIndex);
  this->event_.rtt_state = kInitialFlowRttState;

  swift->Process(this->event_, this->response_, new_now);
  uint32_t expected_ncwnd_fixed =
      GetExpectedIncreasedNcwndFixed<EventT, ResponseT>(
          ncwnd, config.nic_additive_increment_factor());
  EXPECT_EQ(this->response_.flow_id, 0);
  EXPECT_EQ(this->response_.nic_congestion_window, expected_ncwnd_fixed);
  // Even though ncwnd did not decrease, the ncwnd guard is advanced to be one
  // RTT away after it was more than one RTT away.
  EXPECT_EQ(this->response_.nic_window_time_marker,
            new_now - kInitialFlowRttState);

  // Event 2: ncwnd increases like in Event 1 even when the ncwnd time marker is
  // less than one RTT away.
  this->event_.nic_window_time_marker =
      new_now - kInitialFlowRttState +
      5;  // ncwnd time marker is less than one RTT away
  swift->Process(this->event_, this->response_, new_now);
  EXPECT_EQ(this->response_.connection_id, this->event_.connection_id);
  EXPECT_EQ(this->response_.nic_congestion_window,
            expected_ncwnd_fixed);  // same increase as Event 1
  // ncwnd time marker is not changed since the ncwnd increased and the time
  // marker is less than one RTT away.
  EXPECT_EQ(this->response_.nic_window_time_marker,
            this->event_.nic_window_time_marker);  // no change
}

// Tests the multiplicative decrease on the nic congestion window for multipath
// connections during ACK/NACK events.
TYPED_TEST(SwiftTypedTest, ProcessAckMultipathNcwndDecrease) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using StateT = ConnectionState<EventT>;
  using DramStateManagerT = ConnectionBitsDramStateManager<EventT>;
  using AlgorithmT = Swift<EventT, ResponseT>;
  using StatefulAlgorithmT =
      StatefulAlgorithm</*Algorithm=*/AlgorithmT,
                        /*DramStateManagerT=*/DramStateManagerT,
                        /*ForBenchmarking=*/false>;
  SwiftConfiguration config = AlgorithmT::DefaultConfiguration();
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<StatefulAlgorithmT> swift,
                       AlgorithmT::template Create<StatefulAlgorithmT>(config));

  uint32_t new_now = 100400;
  // All flows initially have rtt_state values = kInitialFlowRttState, and fcwnd
  // = kInitialFlowFcwnd.

  const uint8_t kMaxConnectionBits = 5;
  const uint32_t kInitialConnectionFcwndFixed = kInitialFlowFcwnd * 4;
  const auto kDefaultStateValue = ConnectionState<EventT>{};
  auto manager = std::make_unique<ConnectionBitsDramStateManager<EventT>>(
      sizeof(StateT), &kDefaultStateValue, kMaxConnectionBits);
  swift->set_dram_state_manager(std::move(manager));
  const double ncwnd = 100.0;

  // Event 1: rx_buffer_level is larger than target and ncwnd guard is more than
  // one RTT away. Expect ncwnd decrease.
  this->event_.connection_id = 1;
  this->event_.flow_label = 0x10;  // flow ID is 0
  this->event_.multipath_enable = true;
  this->event_.event_type = falcon::RueEventType::kAck;
  this->event_.timestamp_1 = 100000;
  this->event_.timestamp_2 = 100153;
  this->event_.timestamp_3 = 100200;
  this->event_.timestamp_4 = kInitialFlowRttState + this->event_.timestamp_1;
  this->event_.nack_code = falcon::NackCode::kNotANack;
  this->event_.forward_hops = 5;
  this->event_.rx_buffer_level =
      static_cast<uint8_t>(config.target_rx_buffer_level() +
                           1);  // rx_buffer_level larger than target
  this->event_.fabric_congestion_window = kInitialConnectionFcwndFixed;
  this->event_.nic_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(ncwnd,
                                                 falcon_rue::kFractionalBits);
  this->event_.num_packets_acked = 3;
  this->event_.delay_select = falcon::DelaySelect::kForward;
  this->event_.nic_window_time_marker =
      new_now - kInitialFlowRttState -
      10;  // ncwnd time marker is more than one RTT away
  this->event_.base_delay = AlgorithmT::MakeBaseDelayField(
      /*profile_index=*/kSwiftDefaultProfileIndex);
  this->event_.rtt_state = kInitialFlowRttState;

  swift->Process(this->event_, this->response_, new_now);

  uint32_t expected_ncwnd_fixed =
      GetExpectedDecreasedNcwndFixed<EventT, ResponseT>(
          ncwnd, config.nic_multiplicative_decrease_factor(),
          this->event_.rx_buffer_level, config.target_rx_buffer_level());
  EXPECT_EQ(this->response_.flow_id, 0);
  EXPECT_EQ(this->response_.nic_congestion_window,
            expected_ncwnd_fixed);                             // updated
  EXPECT_EQ(this->response_.nic_window_time_marker, new_now);  // updated

  // Event 2: Test with a different rx_buffer_level that is still larger than
  // the target and with a different flow_id. The expected behaviour is the same
  // as Event 1 because ncwnd updates are the same across all flows.
  this->event_.flow_label = 0x11;  // flow ID is 1
  this->event_.rx_buffer_level = static_cast<uint8_t>(
      config.target_rx_buffer_level() + 3);  // + 3 instead of + 1
  swift->Process(this->event_, this->response_, new_now);
  expected_ncwnd_fixed = GetExpectedDecreasedNcwndFixed<EventT, ResponseT>(
      ncwnd, config.nic_multiplicative_decrease_factor(),
      this->event_.rx_buffer_level, config.target_rx_buffer_level());
  EXPECT_EQ(this->response_.flow_id, 1);
  EXPECT_EQ(this->response_.nic_congestion_window,
            expected_ncwnd_fixed);                             // updated
  EXPECT_EQ(this->response_.nic_window_time_marker, new_now);  // updated
}

// Tests the case where no multiplicative decrease should happen on the nic
// congestion window for multipath connections during ACK/NACK events because
// of ncwnd guard.
TYPED_TEST(SwiftTypedTest, ProcessAckMultipathNcwndNoDecrease) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using StateT = ConnectionState<EventT>;
  using DramStateManagerT = ConnectionBitsDramStateManager<EventT>;
  using AlgorithmT = Swift<EventT, ResponseT>;
  using StatefulAlgorithmT =
      StatefulAlgorithm</*Algorithm=*/AlgorithmT,
                        /*DramStateManagerT=*/DramStateManagerT,
                        /*ForBenchmarking=*/false>;
  SwiftConfiguration config = AlgorithmT::DefaultConfiguration();
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<StatefulAlgorithmT> swift,
                       AlgorithmT::template Create<StatefulAlgorithmT>(config));

  uint32_t new_now = 100400;
  // All flows initially have rtt_state values = kInitialFlowRttState, and fcwnd
  // = kInitialFlowFcwnd.

  const uint8_t kMaxConnectionBits = 5;
  const uint32_t kInitialConnectionFcwndFixed = kInitialFlowFcwnd * 4;
  const auto kDefaultStateValue = ConnectionState<EventT>{};
  auto manager = std::make_unique<ConnectionBitsDramStateManager<EventT>>(
      sizeof(StateT), &kDefaultStateValue, kMaxConnectionBits);
  swift->set_dram_state_manager(std::move(manager));
  const double ncwnd = 100.0;

  // Event 1: Even with an rx_buffer_level that is larger than the target, the
  // ncwnd guard is less than an RTT away. Expect no change in ncwnd.
  this->event_.connection_id = 1;
  this->event_.flow_label = 0x10;  // flow ID is 0
  this->event_.multipath_enable = true;
  this->event_.event_type = falcon::RueEventType::kAck;
  this->event_.timestamp_1 = 100000;
  this->event_.timestamp_2 = 100153;
  this->event_.timestamp_3 = 100200;
  this->event_.timestamp_4 = kInitialFlowRttState + this->event_.timestamp_1;
  this->event_.nack_code = falcon::NackCode::kNotANack;
  this->event_.forward_hops = 5;
  this->event_.rx_buffer_level =
      static_cast<uint8_t>(config.target_rx_buffer_level() +
                           1);  // rx_buffer_level larger than target
  this->event_.fabric_congestion_window = kInitialConnectionFcwndFixed;
  this->event_.nic_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(ncwnd,
                                                 falcon_rue::kFractionalBits);
  this->event_.num_packets_acked = 3;
  this->event_.delay_select = falcon::DelaySelect::kForward;
  this->event_.nic_window_time_marker =
      new_now - kInitialFlowRttState +
      10;  // ncwnd time marker is less than one RTT away
  this->event_.base_delay = AlgorithmT::MakeBaseDelayField(
      /*profile_index=*/kSwiftDefaultProfileIndex);
  this->event_.rtt_state = kInitialFlowRttState;

  swift->Process(this->event_, this->response_, new_now);

  EXPECT_EQ(this->response_.flow_id, 0);
  EXPECT_EQ(this->response_.nic_congestion_window,
            this->event_.nic_congestion_window);  // no change
  EXPECT_EQ(this->response_.nic_window_time_marker,
            this->event_.nic_window_time_marker);  // no change
}

// Tests the ncwnd updates with a NACK event and a ResourceExhaustion NACK code.
TYPED_TEST(SwiftTypedTest, ProcessNackMultipathNcwndRxResourceExhaustion) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using StateT = ConnectionState<EventT>;
  using DramStateManagerT = ConnectionBitsDramStateManager<EventT>;
  using AlgorithmT = Swift<EventT, ResponseT>;
  using StatefulAlgorithmT =
      StatefulAlgorithm</*Algorithm=*/AlgorithmT,
                        /*DramStateManagerT=*/DramStateManagerT,
                        /*ForBenchmarking=*/false>;
  SwiftConfiguration config = AlgorithmT::DefaultConfiguration();
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<StatefulAlgorithmT> swift,
                       AlgorithmT::template Create<StatefulAlgorithmT>(config));

  uint32_t new_now = 100400;
  // All flows initially have rtt_state values = kInitialFlowRttState, and fcwnd
  // = kInitialFlowFcwnd.

  const uint8_t kMaxConnectionBits = 5;
  const uint32_t kInitialConnectionFcwndFixed = kInitialFlowFcwnd * 4;
  const auto kDefaultStateValue = ConnectionState<EventT>{};
  auto manager = std::make_unique<ConnectionBitsDramStateManager<EventT>>(
      sizeof(StateT), &kDefaultStateValue, kMaxConnectionBits);
  swift->set_dram_state_manager(std::move(manager));
  const double ncwnd = 100.0;

  // Event 1: The rx_buffer_level that is larger than the target, and the
  // ncwnd guard is more than an RTT away. Expect ncnwd to decrease with the
  // maximum NIC MD because of the NACK kRxResourceExhaustion code.
  this->event_.connection_id = 1;
  this->event_.flow_label = 0x142;  // flow ID is 2
  this->event_.multipath_enable = true;
  this->event_.event_type = falcon::RueEventType::kNack;
  this->event_.nack_code = falcon::NackCode::kRxResourceExhaustion;
  this->event_.timestamp_1 = 100000;
  this->event_.timestamp_2 = 100153;
  this->event_.timestamp_3 = 100200;
  this->event_.timestamp_4 = kInitialFlowRttState + this->event_.timestamp_1;
  this->event_.forward_hops = 5;
  this->event_.rx_buffer_level =
      static_cast<uint8_t>(config.target_rx_buffer_level() +
                           1);  // rx_buffer_level larger than target
  this->event_.fabric_congestion_window = kInitialConnectionFcwndFixed;
  this->event_.nic_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(ncwnd,
                                                 falcon_rue::kFractionalBits);
  this->event_.num_packets_acked = 3;
  this->event_.delay_select = falcon::DelaySelect::kForward;
  this->event_.nic_window_time_marker =
      new_now - kInitialFlowRttState -
      10;  // ncwnd time marker is more than one RTT away
  this->event_.base_delay = AlgorithmT::MakeBaseDelayField(
      /*profile_index=*/kSwiftDefaultProfileIndex);
  this->event_.rtt_state = kInitialFlowRttState;

  swift->Process(this->event_, this->response_, new_now);
  EXPECT_EQ(this->response_.flow_id, 2);
  // With a NACK, the ncwnd should decrease with the maximum multiplicative
  // decrease factor.
  double expected_ncwnd_float =
      ncwnd * config.max_nic_multiplicative_decrease();
  uint32_t expected_ncwnd_fixed = falcon_rue::FloatToFixed<double, uint32_t>(
      expected_ncwnd_float, falcon_rue::kFractionalBits);
  EXPECT_EQ(this->response_.nic_congestion_window,
            expected_ncwnd_fixed);                             // updated
  EXPECT_EQ(this->response_.nic_window_time_marker, new_now);  // updated

  // Event 2: Same setup as Event 1, but with an ncwnd guard that is less than
  // an RTT away. Expect no change in ncwnd.
  this->event_.flow_label = 0x111;  // flow ID is 1
  this->event_.nic_window_time_marker =
      new_now - kInitialFlowRttState +
      10;  // ncwnd time marker is less than one RTT away
  swift->Process(this->event_, this->response_, new_now);
  EXPECT_EQ(this->response_.flow_id, 1);
  EXPECT_EQ(this->response_.nic_congestion_window,
            this->event_.nic_congestion_window);  // no change
  EXPECT_EQ(this->response_.nic_window_time_marker,
            this->event_.nic_window_time_marker);  // no change
}

// Tests the ncwnd updates with a NACK event and a non-ResourceExhaustion NACK
// code.
TYPED_TEST(SwiftTypedTest, ProcessNackMultipathNcwndNonRxResourceExhaustion) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using StateT = ConnectionState<EventT>;
  using DramStateManagerT = ConnectionBitsDramStateManager<EventT>;
  using AlgorithmT = Swift<EventT, ResponseT>;
  using StatefulAlgorithmT =
      StatefulAlgorithm</*Algorithm=*/AlgorithmT,
                        /*DramStateManagerT=*/DramStateManagerT,
                        /*ForBenchmarking=*/false>;
  SwiftConfiguration config = AlgorithmT::DefaultConfiguration();
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<StatefulAlgorithmT> swift,
                       AlgorithmT::template Create<StatefulAlgorithmT>(config));

  uint32_t new_now = 100400;
  // All flows initially have rtt_state values = kInitialFlowRttState, and fcwnd
  // = kInitialFlowFcwnd.

  const uint8_t kMaxConnectionBits = 5;
  const uint32_t kInitialConnectionFcwndFixed = kInitialFlowFcwnd * 4;
  const auto kDefaultStateValue = ConnectionState<EventT>{};
  auto manager = std::make_unique<ConnectionBitsDramStateManager<EventT>>(
      sizeof(StateT), &kDefaultStateValue, kMaxConnectionBits);
  swift->set_dram_state_manager(std::move(manager));
  const double ncwnd = 100.0;

  // Event 1: The rx_buffer_level is larger than the target, and the
  // ncwnd guard is more than an RTT away. Expect ncnwd to decrease based on
  // rx_buffer_level like with an ACK because the NACK code is not
  // resource-related.
  this->event_.connection_id = 1;
  this->event_.flow_label = 0x5142;  // flow ID is 2
  this->event_.multipath_enable = true;
  this->event_.event_type = falcon::RueEventType::kNack;
  this->event_.nack_code =
      falcon::NackCode::kReserved;  // non-ResourceExhaustion NACK code
  this->event_.timestamp_1 = 100000;
  this->event_.timestamp_2 = 100153;
  this->event_.timestamp_3 = 100200;
  this->event_.timestamp_4 = kInitialFlowRttState + this->event_.timestamp_1;
  this->event_.forward_hops = 5;
  this->event_.rx_buffer_level =
      static_cast<uint8_t>(config.target_rx_buffer_level() +
                           1);  // rx_buffer_level larger than target
  this->event_.fabric_congestion_window = kInitialConnectionFcwndFixed;
  this->event_.nic_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(ncwnd,
                                                 falcon_rue::kFractionalBits);
  this->event_.num_packets_acked = 3;
  this->event_.delay_select = falcon::DelaySelect::kForward;
  this->event_.nic_window_time_marker =
      new_now - kInitialFlowRttState -
      10;  // ncwnd time marker is more than one RTT away
  this->event_.base_delay = AlgorithmT::MakeBaseDelayField(
      /*profile_index=*/kSwiftDefaultProfileIndex);
  this->event_.rtt_state = kInitialFlowRttState;

  swift->Process(this->event_, this->response_, new_now);
  EXPECT_EQ(this->response_.flow_id, 2);
  // With a NACK and a non-ResourceExhaustion NACK code, the ncwnd should
  // decrease based on rx_buffer_level like with an ACK.
  uint32_t expected_ncwnd_fixed =
      GetExpectedDecreasedNcwndFixed<EventT, ResponseT>(
          ncwnd, config.nic_multiplicative_decrease_factor(),
          this->event_.rx_buffer_level, config.target_rx_buffer_level());
  EXPECT_EQ(this->response_.connection_id, this->event_.connection_id);
  EXPECT_EQ(this->response_.nic_congestion_window,
            expected_ncwnd_fixed);  // updated based on rx_buffer_level, not
                                    // NACK resource-based type.
  EXPECT_EQ(this->response_.nic_window_time_marker, new_now);  // updated

  // Event 2: The rx_buffer_level is less than the target. Expect ncnwd to
  // increase based on rx_buffer_level like with an ACK because the NACK code
  // is not resource-related.
  this->event_.flow_label = 0x543;  // flow ID is 3
  this->event_.rx_buffer_level = static_cast<uint8_t>(
      config.target_rx_buffer_level() - 1);  // rx_buffer_level less than target
  swift->Process(this->event_, this->response_, new_now);
  EXPECT_EQ(this->response_.flow_id, 3);
  expected_ncwnd_fixed = GetExpectedIncreasedNcwndFixed<EventT, ResponseT>(
      ncwnd, config.nic_additive_increment_factor());
  EXPECT_EQ(this->response_.nic_window_time_marker,
            new_now - kInitialFlowRttState);  // ncwnd guard advanced to be at
                                              // most one RTT away from now.
}

// Tests that the NIC inter packet gap is correctly calculated for a
// NACK/ACK RUE event for a multipath connection, and that the RTO timeout
// calculation in that case is correct.
TYPED_TEST(SwiftTypedTest, ProcessAckNackMultipathNipgAndRto) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using StateT = ConnectionState<EventT>;
  using DramStateManagerT = ConnectionBitsDramStateManager<EventT>;
  using AlgorithmT = Swift<EventT, ResponseT>;
  using StatefulAlgorithmT =
      StatefulAlgorithm</*Algorithm=*/AlgorithmT,
                        /*DramStateManagerT=*/DramStateManagerT,
                        /*ForBenchmarking=*/false>;
  SwiftConfiguration config = AlgorithmT::DefaultConfiguration();
  config.set_min_nic_congestion_window(
      0.2);  // set min ncwnd less than 1 to enable nipg pacing
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<StatefulAlgorithmT> swift,
                       AlgorithmT::template Create<StatefulAlgorithmT>(config));

  uint32_t new_now = 100400;
  double ncwnd = config.min_nic_congestion_window();
  // All flows initially have rtt_state values = kInitialFlowRttState, and fcwnd
  // = kInitialFlowFcwnd.

  const uint8_t kMaxConnectionBits = 5;
  const uint32_t kInitialConnectionFcwndFixed = kInitialFlowFcwnd * 4;
  const auto kDefaultStateValue = ConnectionState<EventT>{};
  auto manager = std::make_unique<ConnectionBitsDramStateManager<EventT>>(
      sizeof(StateT), &kDefaultStateValue, kMaxConnectionBits);
  swift->set_dram_state_manager(std::move(manager));

  // Event 1: ncwnd for the connection is less than 1. Therefore, nipg will be
  // nonzero.
  this->event_.connection_id = 1;
  this->event_.flow_label = 0x5140;  // flow ID is 0
  this->event_.multipath_enable = true;
  this->event_.event_type = falcon::RueEventType::kNack;
  this->event_.nack_code =
      falcon::NackCode::kReserved;  // non-ResourceExhaustion NACK code
  this->event_.timestamp_1 = 100000;
  this->event_.timestamp_2 = 100153;
  this->event_.timestamp_3 = 100200;
  this->event_.timestamp_4 = kInitialFlowRttState + this->event_.timestamp_1;
  this->event_.forward_hops = 5;
  this->event_.rx_buffer_level =
      static_cast<uint8_t>(config.target_rx_buffer_level() +
                           1);  // rx_buffer_level larger than target
  this->event_.fabric_congestion_window = kInitialConnectionFcwndFixed;
  this->event_.nic_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(ncwnd,
                                                 falcon_rue::kFractionalBits);
  this->event_.num_packets_acked = 3;
  this->event_.delay_select = falcon::DelaySelect::kForward;
  this->event_.nic_window_time_marker =
      new_now - kInitialFlowRttState -
      10;  // ncwnd time marker is more than one RTT away
  this->event_.base_delay = AlgorithmT::MakeBaseDelayField(
      /*profile_index=*/kSwiftDefaultProfileIndex);
  this->event_.rtt_state = kInitialFlowRttState;

  swift->Process(this->event_, this->response_, new_now);
  EXPECT_EQ(this->response_.flow_id, 0);
  EXPECT_EQ(this->response_.inter_packet_gap,
            0);  // connection fcwnd still larger than 1, so fipg should be 0
  EXPECT_EQ(
      this->response_.nic_congestion_window,
      this->event_.nic_congestion_window);  // ncwnd should decrease but it is
                                            // already at its minimum
  double expected_nipg =
      std::round((kInitialFlowRttState / ncwnd - kInitialFlowRttState) *
                 config.ipg_time_scalar());
  EXPECT_NEAR(this->response_.nic_inter_packet_gap,
              static_cast<uint64_t>(expected_nipg), 1);
  // RTO calculation should use the NIC ipg.
  uint32_t retransmit_timeout_expected =
      StatefulAlgorithmT::GetRetransmitTimeout(
          config.min_retransmit_timeout(), kInitialFlowRttState,
          config.retransmit_timeout_scalar(), expected_nipg,
          config.ipg_time_scalar());
  EXPECT_NEAR(this->response_.retransmit_timeout, retransmit_timeout_expected,
              1);

  // Event 2: No change in terms of ncwnd compared to Event 1. However, the
  // rtt_state used to calculate nipg and rto will now be that of flow 0 because
  // it is the maximum rtt_state across all flows.
  this->event_.timestamp_4 =
      2 * kInitialFlowRttState + this->event_.timestamp_1;
  this->event_.rtt_state = 2 * kInitialFlowRttState;
  swift->Process(this->event_, this->response_, new_now);
  // nipg and rto should use the max rtt across all flows - in this case the
  // rtt_state in the event belonging to flow 0.
  expected_nipg =
      std::round((2 * kInitialFlowRttState / ncwnd - 2 * kInitialFlowRttState) *
                 config.ipg_time_scalar());
  EXPECT_NEAR(this->response_.nic_inter_packet_gap,
              static_cast<uint64_t>(expected_nipg), 1);
  // RTO calculation should use the NIC ipg and (2*rtt_state).
  retransmit_timeout_expected = StatefulAlgorithmT::GetRetransmitTimeout(
      config.min_retransmit_timeout(), 2 * kInitialFlowRttState,
      config.retransmit_timeout_scalar(), expected_nipg,
      config.ipg_time_scalar());
  EXPECT_NEAR(this->response_.retransmit_timeout, retransmit_timeout_expected,
              1);
}

// Tests PLB implementation for multipath connections for flow 0 which stores
// its PLB state in the response and not in RUE state.
TYPED_TEST(SwiftTypedTest, MultipathPlbFlow0Test) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using StateT = ConnectionState<EventT>;
  using DramStateManagerT = ConnectionBitsDramStateManager<EventT>;
  using AlgorithmT = Swift<EventT, ResponseT>;
  using StatefulAlgorithmT =
      StatefulAlgorithm</*Algorithm=*/AlgorithmT,
                        /*DramStateManagerT=*/DramStateManagerT,
                        /*ForBenchmarking=*/false>;

  constexpr uint32_t kPlbAttemptThreshold = 3;

  SwiftConfiguration config = MakeConfig<EventT, ResponseT>(100);
  config.set_randomize_path(true);  // enable PLB
  config.set_max_fabric_congestion_window(64);
  config.set_max_nic_congestion_window(64);
  config.set_plb_attempt_threshold(kPlbAttemptThreshold);
  config.set_plb_congestion_threshold(0.5);
  config.set_plb_target_rtt_multiplier(1.2);

  ASSERT_OK_AND_ASSIGN(std::unique_ptr<StatefulAlgorithmT> swift,
                       AlgorithmT::template Create<StatefulAlgorithmT>(config));

  uint32_t now = 102200;
  // All flows initially have rtt_state values = kInitialFlowRttState, and fcwnd
  // = kInitialFlowFcwnd.

  const uint8_t kMaxConnectionBits = 5;
  const uint32_t kInitialConnectionFcwndFixed = kInitialFlowFcwnd * 4;
  const auto kDefaultStateValue = ConnectionState<EventT>{};
  auto manager = std::make_unique<ConnectionBitsDramStateManager<EventT>>(
      sizeof(StateT), &kDefaultStateValue, kMaxConnectionBits);
  swift->set_dram_state_manager(std::move(manager));

  const auto long_delay_timestamp = 100879;
  const auto short_delay_timestamp = 100100;

  this->event_.connection_id = 1234;
  this->event_.multipath_enable = true;
  this->event_.flow_label = 0x10;  // flow ID 0
  this->event_.event_type = falcon::RueEventType::kAck;
  this->event_.timestamp_1 = 100000;
  this->event_.timestamp_2 = long_delay_timestamp;
  this->event_.timestamp_3 = 101000;
  this->event_.timestamp_4 = 101200;
  this->event_.retransmit_count = 0;
  this->event_.retransmit_reason = falcon::RetransmitReason::kTimeout;
  this->event_.nack_code = falcon::NackCode::kNotANack;
  this->event_.forward_hops = 5;
  this->event_.cc_metadata = 0;
  this->event_.fabric_congestion_window = kInitialConnectionFcwndFixed;
  this->event_.nic_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(64,
                                                 falcon_rue::kFractionalBits);
  this->event_.num_packets_acked = 30;
  this->event_.event_queue_select = 0;
  this->event_.delay_select = falcon::DelaySelect::kForward;
  this->event_.fabric_window_time_marker = 101000;
  this->event_.base_delay = AlgorithmT::MakeBaseDelayField(
      /*profile_index=*/kSwiftDefaultProfileIndex);
  this->event_.delay_state = 0;
  this->event_.plb_state = 0;
  this->event_.rtt_state = 1000;
  this->event_.cc_opaque = 0;  // don't set bit zero (kRandomizePathEnableMask)
  this->event_.gen_bit = 0;

  swift->Process(this->event_, this->response_, now);

  PlbState plb_states;

  // In all the following event processing, fcwnd_flow_ID_0 starts at 32.0 and
  // is updated with every event in the RUE state.

  // 30 Acked with big fabric delay.
  EXPECT_EQ(this->response_.connection_id, this->event_.connection_id);
  EXPECT_EQ((IsResponseRepath<EventT, ResponseT>(this->response_)), false);
  plb_states.value = this->response_.plb_state;
  EXPECT_EQ(plb_states.packets_acknowledged, 30);
  EXPECT_EQ(plb_states.plb_reroute_attempted, 0);
  EXPECT_EQ(plb_states.packets_congestion_acknowledged, 30);

  // 10 Acked with small fabric delay. Should increment
  // plb_reroute_attempted as 30/(30+10) > 0.5.
  this->event_.num_packets_acked = 10;
  this->event_.timestamp_2 = short_delay_timestamp;
  this->event_.plb_state = plb_states.value;
  swift->Process(this->event_, this->response_, now);
  plb_states.value = this->response_.plb_state;
  EXPECT_EQ((IsResponseRepath<EventT, ResponseT>(this->response_)), false);
  EXPECT_EQ(plb_states.packets_acknowledged, 0);
  EXPECT_EQ(plb_states.plb_reroute_attempted, 1);
  EXPECT_EQ(plb_states.packets_congestion_acknowledged, 0);

  // 33 Acked with small fabric delay (33 > fcwnd_flow_ID_0). Should clear
  // plb_reroute_attempted.
  this->event_.num_packets_acked = 33;
  this->event_.timestamp_2 = short_delay_timestamp;
  this->event_.plb_state = plb_states.value;
  swift->Process(this->event_, this->response_, now);
  plb_states.value = this->response_.plb_state;
  EXPECT_EQ((IsResponseRepath<EventT, ResponseT>(this->response_)), false);
  EXPECT_EQ(plb_states.packets_acknowledged, 0);
  EXPECT_EQ(plb_states.plb_reroute_attempted, 0);
  EXPECT_EQ(plb_states.packets_congestion_acknowledged, 0);

  // For consecutive kPlbAttemptThreshold rounds.
  for (int i = 0; i < kPlbAttemptThreshold; i++) {
    // 33 Acked with big fabric delay. Each round should increment
    // plb_reroute_attempted.
    this->event_.num_packets_acked = 33;  // 33 > fcwnd_flow_ID_0
    this->event_.timestamp_2 = long_delay_timestamp;
    this->event_.plb_state = plb_states.value;
    swift->Process(this->event_, this->response_, now);
    plb_states.value = this->response_.plb_state;
    // The last round should randomize flow_label_1.
    EXPECT_EQ(this->response_.flow_label_1_valid,
              i + 1 == kPlbAttemptThreshold);
    EXPECT_EQ(this->response_.flow_label_2_valid, false);
    EXPECT_EQ(this->response_.flow_label_3_valid, false);
    EXPECT_EQ(this->response_.flow_label_4_valid, false);
    EXPECT_EQ(plb_states.packets_acknowledged, 0);
    // The last round should clear plb_reroute_attempted.
    EXPECT_EQ(plb_states.plb_reroute_attempted,
              i + 1 == kPlbAttemptThreshold ? 0 : i + 1);
    EXPECT_EQ(plb_states.packets_congestion_acknowledged, 0);
  }

  // 2 Acked with big fabric delay. 2 < fcwnd_flow_ID_0.
  this->event_.num_packets_acked = 2;
  this->event_.timestamp_2 = long_delay_timestamp;
  this->event_.plb_state = plb_states.value;
  swift->Process(this->event_, this->response_, now);
  plb_states.value = this->response_.plb_state;
  EXPECT_EQ((IsResponseRepath<EventT, ResponseT>(this->response_)), false);
  EXPECT_EQ(plb_states.packets_acknowledged, 2);
  EXPECT_EQ(plb_states.plb_reroute_attempted, 0);
  EXPECT_EQ(plb_states.packets_congestion_acknowledged, 2);

  // 3 Acked with small fabric delay. Should clear plb_reroute_attempted as
  // 2/(3+2) < 0.5.
  this->event_.num_packets_acked = 3;
  this->event_.timestamp_2 = short_delay_timestamp;
  this->event_.plb_state = plb_states.value;
  swift->Process(this->event_, this->response_, now);
  plb_states.value = this->response_.plb_state;
  EXPECT_EQ((IsResponseRepath<EventT, ResponseT>(this->response_)), false);
  EXPECT_EQ(plb_states.packets_acknowledged, 0);
  EXPECT_EQ(plb_states.plb_reroute_attempted, 0);
  EXPECT_EQ(plb_states.packets_congestion_acknowledged, 0);
}

// Tests PLB implementation for multipath connections for flows 1 to 3 which
// store their PLB state in the RUE state.
TYPED_TEST(SwiftTypedTest, MultipathPlbNonFlow0Test) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using StateT = ConnectionState<EventT>;
  using DramStateManagerT = ConnectionBitsDramStateManager<EventT>;
  using AlgorithmT = Swift<EventT, ResponseT>;
  using StatefulAlgorithmT =
      StatefulAlgorithm</*Algorithm=*/AlgorithmT,
                        /*DramStateManagerT=*/DramStateManagerT,
                        /*ForBenchmarking=*/false>;

  constexpr uint32_t kPlbAttemptThreshold = 3;

  SwiftConfiguration config = MakeConfig<EventT, ResponseT>(100);
  config.set_randomize_path(true);  // enable PLB
  config.set_max_fabric_congestion_window(64);
  config.set_max_nic_congestion_window(64);
  config.set_plb_attempt_threshold(kPlbAttemptThreshold);
  config.set_plb_congestion_threshold(0.5);
  config.set_plb_target_rtt_multiplier(1.2);

  ASSERT_OK_AND_ASSIGN(std::unique_ptr<StatefulAlgorithmT> swift,
                       AlgorithmT::template Create<StatefulAlgorithmT>(config));

  uint32_t now = 102200;
  // All flows initially have rtt_state values = kInitialFlowRttState, and fcwnd
  // = kInitialFlowFcwnd.

  const uint8_t kMaxConnectionBits = 5;
  const uint32_t kInitialConnectionFcwndFixed = kInitialFlowFcwnd * 4;
  const auto kDefaultStateValue = ConnectionState<EventT>{};
  auto manager = std::make_unique<ConnectionBitsDramStateManager<EventT>>(
      sizeof(StateT), &kDefaultStateValue, kMaxConnectionBits);
  swift->set_dram_state_manager(std::move(manager));

  const auto long_delay_timestamp = 100879;

  this->event_.connection_id = 1234;
  this->event_.multipath_enable = true;
  this->event_.flow_label = 0x11;  // flow ID 1
  this->event_.event_type = falcon::RueEventType::kAck;
  this->event_.timestamp_1 = 100000;
  this->event_.timestamp_2 = long_delay_timestamp;
  this->event_.timestamp_3 = 101000;
  this->event_.timestamp_4 = 101200;
  this->event_.retransmit_count = 0;
  this->event_.retransmit_reason = falcon::RetransmitReason::kTimeout;
  this->event_.nack_code = falcon::NackCode::kNotANack;
  this->event_.forward_hops = 5;
  this->event_.cc_metadata = 0;
  this->event_.fabric_congestion_window = kInitialConnectionFcwndFixed;
  this->event_.nic_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(64,
                                                 falcon_rue::kFractionalBits);
  this->event_.num_packets_acked = 30;
  this->event_.event_queue_select = 0;
  this->event_.delay_select = falcon::DelaySelect::kForward;
  this->event_.fabric_window_time_marker = 101000;
  this->event_.base_delay = AlgorithmT::MakeBaseDelayField(
      /*profile_index=*/kSwiftDefaultProfileIndex);
  this->event_.delay_state = 0;
  this->event_.plb_state = 0;
  this->event_.rtt_state = 1000;
  this->event_.cc_opaque = 0;  // don't set bit zero (kRandomizePathEnableMask)
  this->event_.gen_bit = 0;

  swift->Process(this->event_, this->response_, now);

  // For consecutive kPlbAttemptThreshold rounds.
  for (int i = 0; i < kPlbAttemptThreshold; i++) {
    // 33 Acked with big fabric delay. Each round should increment
    // plb_reroute_attempted.
    this->event_.num_packets_acked = 33;  // 33 > fcwnd_flow_ID_1
    this->event_.timestamp_2 = long_delay_timestamp;
    swift->Process(this->event_, this->response_, now);
    EXPECT_EQ(this->response_.plb_state,
              this->event_.plb_state);  // no change for flow PLB state
    // The last round should randomize flow_label_2 (corresponding to flow ID
    // 1).
    EXPECT_EQ(this->response_.flow_label_2_valid,
              i + 1 == kPlbAttemptThreshold);
    EXPECT_EQ(this->response_.flow_label_1_valid, false);
    EXPECT_EQ(this->response_.flow_label_3_valid, false);
    EXPECT_EQ(this->response_.flow_label_4_valid, false);
  }

  this->event_.flow_label = 0x12;  // flow ID 2
  // For consecutive kPlbAttemptThreshold rounds.
  for (int i = 0; i < kPlbAttemptThreshold; i++) {
    // 33 Acked with big fabric delay. Each round should increment
    // plb_reroute_attempted.
    this->event_.num_packets_acked = 33;  // 33 > fcwnd_flow_ID_2
    this->event_.timestamp_2 = long_delay_timestamp;
    swift->Process(this->event_, this->response_, now);
    EXPECT_EQ(this->response_.plb_state,
              this->event_.plb_state);  // no change for flow PLB state
    // The last round should randomize flow_label_3 (corresponding to flow ID
    // 2).
    EXPECT_EQ(this->response_.flow_label_3_valid,
              i + 1 == kPlbAttemptThreshold);
    EXPECT_EQ(this->response_.flow_label_1_valid, false);
    EXPECT_EQ(this->response_.flow_label_2_valid, false);
    EXPECT_EQ(this->response_.flow_label_4_valid, false);
  }

  this->event_.flow_label = 0x13;  // flow ID 3
  // For consecutive kPlbAttemptThreshold rounds.
  for (int i = 0; i < kPlbAttemptThreshold; i++) {
    // 33 Acked with big fabric delay. Each round should increment
    // plb_reroute_attempted.
    this->event_.num_packets_acked = 33;  // 33 > fcwnd_flow_ID_3
    this->event_.timestamp_2 = long_delay_timestamp;
    swift->Process(this->event_, this->response_, now);
    EXPECT_EQ(this->response_.plb_state,
              this->event_.plb_state);  // no change for flow PLB state
    // The last round should randomize flow_label_4 (corresponding to flow ID
    // 3).
    EXPECT_EQ(this->response_.flow_label_4_valid,
              i + 1 == kPlbAttemptThreshold);
    EXPECT_EQ(this->response_.flow_label_1_valid, false);
    EXPECT_EQ(this->response_.flow_label_2_valid, false);
    EXPECT_EQ(this->response_.flow_label_3_valid, false);
  }
}
}  // namespace
}  // namespace rue
}  // namespace isekai
