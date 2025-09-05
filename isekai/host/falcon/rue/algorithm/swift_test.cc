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
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>

#include "absl/log/log.h"
#include "absl/random/random.h"
#include "absl/status/status.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "isekai/common/status_util.h"
#include "isekai/common/testing.h"
#include "isekai/host/falcon/falcon.h"
#include "isekai/host/falcon/rue/algorithm/algorithm.pb.h"
#include "isekai/host/falcon/rue/algorithm/connection_state.h"
#include "isekai/host/falcon/rue/algorithm/dram_state_manager.h"
#include "isekai/host/falcon/rue/algorithm/stateful_algorithm.h"
#include "isekai/host/falcon/rue/bits.h"
#include "isekai/host/falcon/rue/fixed.h"
#include "isekai/host/falcon/rue/format_gen1.h"
#include "isekai/host/falcon/rue/state_manager.h"

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

template <typename EventT, typename ResponseT>
struct StatefulAlgorithmTypeHelper {
  using DramStateManagerT =
      MarkDramStateManagerAsHavingOwner<StateManager<EventT>>;
  using AlgorithmT = Swift<EventT, ResponseT>;
  using StatefulAlgorithmT = StatefulAlgorithm<AlgorithmT, DramStateManagerT,
                                               /*ForBenchmaring=*/false>;
};

template <typename EventT, typename ResponseT>
absl::StatusOr<std::pair<std::unique_ptr<typename StatefulAlgorithmTypeHelper<
                             EventT, ResponseT>::StatefulAlgorithmT>,
                         std::unique_ptr<typename StatefulAlgorithmTypeHelper<
                             EventT, ResponseT>::DramStateManagerT>>>
CreateStatefulSwiftAlgorithm(const SwiftConfiguration& config) {
  using StatefulAlgorithmTypeHelper =
      StatefulAlgorithmTypeHelper<EventT, ResponseT>;
  using AlgorithmT = typename StatefulAlgorithmTypeHelper::AlgorithmT;
  using StatefulAlgorithmT =
      typename StatefulAlgorithmTypeHelper::StatefulAlgorithmT;
  using DramStateManagerT =
      typename StatefulAlgorithmTypeHelper::DramStateManagerT;
  ASSIGN_OR_RETURN(auto swift,
                   AlgorithmT::template Create<StatefulAlgorithmT>(config));
  constexpr size_t kMaxConnections = (1 << 20);  // 1 million.
  auto state_manager = std::make_unique<DramStateManagerT>(kMaxConnections);
  auto _ = state_manager->Allocate(/*connection_id=*/1234, /*reallocate=*/true);
  swift->set_dram_state_manager(state_manager.get());
  return std::make_pair(std::move(swift), std::move(state_manager));
}

}  // namespace

TEST(SwiftTest, GetSmoothed) {
  uint32_t s = 10000;
  uint32_t r = 5000;
  EXPECT_EQ(SwiftGen1::GetSmoothed(0.0, s, r), static_cast<uint32_t>(r));
  EXPECT_EQ(SwiftGen1::GetSmoothed(0.5, s, r),
            static_cast<uint32_t>(s * 0.5 + r * 0.5));
  EXPECT_EQ(SwiftGen1::GetSmoothed(0.75, s, r),
            static_cast<uint32_t>(s * 0.75 + r * 0.25));
  EXPECT_EQ(SwiftGen1::GetSmoothed(0.875, s, r),
            static_cast<uint32_t>(s * 0.875 + r * 0.125));

  double s2 = 10000;
  double r2 = 5000;
  EXPECT_EQ(SwiftGen1::GetSmoothed(0.0, s2, r2), r2);
  EXPECT_EQ(SwiftGen1::GetSmoothed(0.5, s2, r2), s2 * 0.5 + r2 * 0.5);
  EXPECT_EQ(SwiftGen1::GetSmoothed(0.75, s2, r2), s2 * 0.75 + r2 * 0.25);
  EXPECT_EQ(SwiftGen1::GetSmoothed(0.875, s2, r2), s2 * 0.875 + r2 * 0.125);
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
    uint32_t act = SwiftGen1::GetTargetDelay(topo_scaling_units_per_hop, alpha,
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
    uint32_t ipg = SwiftGen1::GetInterPacketGap(ipg_time_scalar, ipg_bits,
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
    uint32_t ipg = SwiftGen1::GetInterPacketGap(ipg_time_scalar, ipg_bits,
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

    uint32_t act = SwiftGen1::GetRetransmitTimeout(
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
  uint32_t act = SwiftGen1::GetRetransmitTimeout(
      /*min_retransmit_timeout=*/0, rtt, scalar,
      /*inter_packet_gap=*/5,
      /*ipg_time_scalar=*/2.0);
  EXPECT_NEAR(act, rtt * scalar + 5 / 2.0, 1.0);

  act = SwiftGen1::GetRetransmitTimeout(
      /*min_retransmit_timeout=*/0, rtt, scalar,
      /*inter_packet_gap=*/(1 << falcon_rue::kTimeBits) - 1,
      /*ipg_time_scalar=*/1.0);
  EXPECT_EQ(act, falcon_rue::ValidMask<uint32_t>(falcon_rue::kTimeBits));
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
    std::tuple<falcon_rue::Event_GEN1, falcon_rue::Response_GEN1>>;
TYPED_TEST_SUITE(SwiftTypedTest, Event_Response_Types);

TYPED_TEST(SwiftTypedTest, ProcessAckMd150p) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;
  // Tests the ACK case of multiplicative decrease where the actual delay is
  // greater than the target delay. The congestion is reduced but it does
  // not hit the maximum reduction.
  const auto config = MakeConfig<EventT, ResponseT>(100);
  auto status_or_stateful_components =
      CreateStatefulSwiftAlgorithm<EventT, ResponseT>(config);
  ASSERT_OK(status_or_stateful_components);
  auto swift = std::move(status_or_stateful_components->first);
  auto state_manager = std::move(status_or_stateful_components->second);

  const auto new_now = 101000;

  this->event_.connection_id = 1234;
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
  this->event_.inter_packet_gap = 0;
  this->event_.retransmit_timeout = 1000;
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

  swift->Process(this->event_, this->response_, new_now);

  EXPECT_EQ(this->response_.connection_id, this->event_.connection_id);
  EXPECT_EQ(this->response_.randomize_path, false);
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
  EXPECT_EQ(this->response_.inter_packet_gap, this->event_.inter_packet_gap);
  const auto rtt = this->event_.timestamp_4 - this->event_.timestamp_1;
  uint32_t exp_rtt_state =
      this->event_.rtt_state * config.rtt_smoothing_alpha() +
      rtt * (1 - config.rtt_smoothing_alpha());
  uint32_t exp_rto = std::max(exp_rtt_state, config.min_retransmit_timeout());
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

TYPED_TEST(SwiftTypedTest, ProcessAckMdEqual) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;
  // Tests the ACK case of multiplicative decrease where the target delay
  // and the actual delay are the same. In this case the output congestion
  // window is the same as the input congestion window.
  const auto config = MakeConfig<EventT, ResponseT>(100);
  auto status_or_stateful_components =
      CreateStatefulSwiftAlgorithm<EventT, ResponseT>(config);
  ASSERT_OK(status_or_stateful_components);
  auto swift = std::move(status_or_stateful_components->first);
  auto state_manager = std::move(status_or_stateful_components->second);

  const auto new_now = 10100;

  this->event_.connection_id = 1234;
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
  this->event_.inter_packet_gap = 0;
  this->event_.retransmit_timeout = 1000;
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
  EXPECT_EQ(this->response_.randomize_path, false);
  EXPECT_EQ(this->response_.cc_metadata, this->event_.cc_metadata);
  EXPECT_EQ(this->response_.fabric_congestion_window,
            this->event_.fabric_congestion_window);  // no change
  EXPECT_EQ(this->response_.inter_packet_gap, this->event_.inter_packet_gap);
  const auto rtt = this->event_.timestamp_4 - this->event_.timestamp_1;
  uint32_t exp_rtt_state =
      this->event_.rtt_state * config.rtt_smoothing_alpha() +
      rtt * (1 - config.rtt_smoothing_alpha());
  uint32_t exp_rto = std::max(exp_rtt_state, config.min_retransmit_timeout());
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
  // expect (2**11)-1
  EXPECT_NEAR(kMaxGen1NicCongestionWindow, 2047,
              std::numeric_limits<double>::epsilon());
  // expect 1/(2**10)
  EXPECT_NEAR(kMinGen1NicCongestionWindow, 0.000976562500000,
              std::numeric_limits<double>::epsilon());
  // expect (2**5)-1
  EXPECT_EQ(kMaxRxBufferLevel, 31);
  // expect 0
  EXPECT_EQ(kMinRxBufferLevel, 0);
  // expect 12 bits
  EXPECT_EQ(kMaxTopoScalingPerHop, 4095);
}

TYPED_TEST(SwiftTypedTest, ProcessAckAiOverCwnd) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;
  // Tests the ACK case of additive increment where the input
  // congestion window is greater than 1. In this case the output congestion
  // window has an addition of the additive increase times the number of
  // acked packets and the increase is scaled by the congestion window.
  const auto config = MakeConfig<EventT, ResponseT>(100);
  auto status_or_stateful_components =
      CreateStatefulSwiftAlgorithm<EventT, ResponseT>(config);
  ASSERT_OK(status_or_stateful_components);
  auto swift = std::move(status_or_stateful_components->first);
  auto state_manager = std::move(status_or_stateful_components->second);
  const auto new_now = 100400;

  const double fcwnd = 8.0;
  this->event_.connection_id = 1234;
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
  this->event_.inter_packet_gap = 0;
  this->event_.retransmit_timeout = 1000;
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
  EXPECT_EQ(this->response_.randomize_path, false);
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

TYPED_TEST(SwiftTypedTest, ProcessAckAi) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;
  // Tests the ACK case of additive increment where the input
  // congestion window is less than 1. In this case the output congestion
  // window has an addition of the additive increase times the number of
  // acked packets.
  const auto config = MakeConfig<EventT, ResponseT>(100);
  auto status_or_stateful_components =
      CreateStatefulSwiftAlgorithm<EventT, ResponseT>(config);
  ASSERT_OK(status_or_stateful_components);
  auto swift = std::move(status_or_stateful_components->first);
  auto state_manager = std::move(status_or_stateful_components->second);

  const auto new_now = 100400;
  const double fcwnd = 0.5;  // a value < 1 as something paces if fcwnd<1

  this->event_.connection_id = 1234;
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
  this->event_.inter_packet_gap = 0;
  this->event_.retransmit_timeout = 1000;
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
  EXPECT_EQ(this->response_.randomize_path, false);
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
  uint32_t exp_rto = std::max(exp_rtt_state, config.min_retransmit_timeout());
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

TYPED_TEST(SwiftTypedTest, ProcessAckMaxMd) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;
  // Tests the ACK case of multiplicative decrease where the actual delay is
  // greater than the target delay. The congestion is reduced and it uses the
  // maximum reduction.
  const auto config = MakeConfig<EventT, ResponseT>(100);
  auto status_or_stateful_components =
      CreateStatefulSwiftAlgorithm<EventT, ResponseT>(config);
  ASSERT_OK(status_or_stateful_components);
  auto swift = std::move(status_or_stateful_components->first);
  auto state_manager = std::move(status_or_stateful_components->second);

  const auto new_now = 102000;

  const double fcwnd = 2.75;
  this->event_.connection_id = 1234;
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
  this->event_.inter_packet_gap = 0;
  this->event_.retransmit_timeout = 1000;
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
  EXPECT_EQ(this->response_.randomize_path, false);
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

TYPED_TEST(SwiftTypedTest, ProcessAckSkipMd) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;
  // Tests the ACK case where multiplicative decrease would be used but not
  // enough time has passed since the last decrease so no change is made.
  const auto config = MakeConfig<EventT, ResponseT>(100);
  auto status_or_stateful_components =
      CreateStatefulSwiftAlgorithm<EventT, ResponseT>(config);
  ASSERT_OK(status_or_stateful_components);
  auto swift = std::move(status_or_stateful_components->first);
  auto state_manager = std::move(status_or_stateful_components->second);

  this->event_.connection_id = 1234;
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
  this->event_.inter_packet_gap = 0;
  this->event_.retransmit_timeout = 1000;
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
  EXPECT_EQ(this->response_.randomize_path, false);
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
  auto status_or_stateful_components =
      CreateStatefulSwiftAlgorithm<EventT, ResponseT>(config);
  ASSERT_OK(status_or_stateful_components);
  auto swift = std::move(status_or_stateful_components->first);
  auto state_manager = std::move(status_or_stateful_components->second);

  const auto new_now = 102000;

  const double fcwnd = 2.75;
  this->event_.connection_id = 1234;
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
  this->event_.inter_packet_gap = 0;
  this->event_.retransmit_timeout = 1000;
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
  EXPECT_EQ(this->response_.randomize_path, false);
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

TYPED_TEST(SwiftTypedTest, ProcessNackNoMaxDecr) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;
  // Tests the case of NACK where the max congestion window decrease is not
  // performed because a decrease has occurred too recently.
  auto config = MakeConfig<EventT, ResponseT>(100);
  config.set_max_decrease_on_eack_nack_drop(true);
  auto status_or_stateful_components =
      CreateStatefulSwiftAlgorithm<EventT, ResponseT>(config);
  ASSERT_OK(status_or_stateful_components);
  auto swift = std::move(status_or_stateful_components->first);
  auto state_manager = std::move(status_or_stateful_components->second);

  this->event_.connection_id = 1234;
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
  this->event_.inter_packet_gap = 0;
  this->event_.retransmit_timeout = 1000;
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
  EXPECT_EQ(this->response_.randomize_path, false);
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

TYPED_TEST(SwiftTypedTest, ProcessNackDecreaseReactToDelay) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;
  // Tests the case of NACK where the congestion window is decreased based on
  // delay.
  auto config = MakeConfig<EventT, ResponseT>(100);
  config.set_max_decrease_on_eack_nack_drop(false);
  config.set_max_flow_scaling(1);
  auto status_or_stateful_components =
      CreateStatefulSwiftAlgorithm<EventT, ResponseT>(config);
  ASSERT_OK(status_or_stateful_components);
  auto swift = std::move(status_or_stateful_components->first);
  auto state_manager = std::move(status_or_stateful_components->second);

  const auto new_now = 100200;

  const double fcwnd = 32;
  this->event_.connection_id = 1234;
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
  this->event_.inter_packet_gap = 0;
  this->event_.retransmit_timeout = 1000;
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
  EXPECT_EQ(this->response_.randomize_path, false);
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
  uint32_t exp_rto = std::max(exp_rtt_state, config.min_retransmit_timeout());
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

TYPED_TEST(SwiftTypedTest, ProcessNackIncreaseReactToDelay) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;
  // Tests the case of NACK where the congestion window is increased based on
  // delay.
  auto config = MakeConfig<EventT, ResponseT>(100);
  config.set_max_decrease_on_eack_nack_drop(false);
  config.set_max_flow_scaling(1);
  auto status_or_stateful_components =
      CreateStatefulSwiftAlgorithm<EventT, ResponseT>(config);
  ASSERT_OK(status_or_stateful_components);
  auto swift = std::move(status_or_stateful_components->first);
  auto state_manager = std::move(status_or_stateful_components->second);

  const auto new_now = 100020;

  const double fcwnd = 32;
  this->event_.connection_id = 1234;
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
  this->event_.inter_packet_gap = 0;
  this->event_.retransmit_timeout = 1000;
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
  EXPECT_EQ(this->response_.randomize_path, false);
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
  uint32_t exp_rto = std::max(exp_rtt_state, config.min_retransmit_timeout());
  EXPECT_EQ(this->response_.retransmit_timeout, exp_rto);
  EXPECT_EQ(this->response_.event_queue_select, 0);
  EXPECT_EQ(this->response_.delay_select, falcon::DelaySelect::kForward);
  EXPECT_EQ(this->response_.base_delay, this->event_.base_delay);
  EXPECT_EQ(this->response_.delay_state,
            this->event_.timestamp_2 - this->event_.timestamp_1);
  EXPECT_NEAR(this->response_.rtt_state, exp_rtt_state, 1);
  EXPECT_EQ(this->response_.cc_opaque, this->event_.cc_opaque);
}

TYPED_TEST(SwiftTypedTest, ProcessRetransmitNoDecr) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;
  // Tests the case of retransmit where the congestion window decrease is not
  // performed because a decrease has occurred too recently.
  const auto config = MakeConfig<EventT, ResponseT>(100);
  auto status_or_stateful_components =
      CreateStatefulSwiftAlgorithm<EventT, ResponseT>(config);
  ASSERT_OK(status_or_stateful_components);
  auto swift = std::move(status_or_stateful_components->first);
  auto state_manager = std::move(status_or_stateful_components->second);

  this->event_.connection_id = 1234;
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
  this->event_.inter_packet_gap = 0;
  this->event_.retransmit_timeout = 1000;
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
  EXPECT_EQ(this->response_.randomize_path, false);
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

TYPED_TEST(SwiftTypedTest, ProcessRetransmitFirst) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;
  // Tests the case of retransmit where the congestion window decrease is
  // performed because it is the first retransmission.
  const auto config = MakeConfig<EventT, ResponseT>(100);
  auto status_or_stateful_components =
      CreateStatefulSwiftAlgorithm<EventT, ResponseT>(config);
  ASSERT_OK(status_or_stateful_components);
  auto swift = std::move(status_or_stateful_components->first);
  auto state_manager = std::move(status_or_stateful_components->second);

  const auto new_now = 103000;

  const double fcwnd = 2.75;
  this->event_.connection_id = 1234;
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
  this->event_.inter_packet_gap = 0;
  this->event_.retransmit_timeout = 1000;
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
  EXPECT_EQ(this->response_.randomize_path, false);
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

TYPED_TEST(SwiftTypedTest, ProcessRetransmitSecond) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;
  // Tests the case of retransmit where the congestion window decrease is
  // not performed because it is the second retransmission.
  const auto config = MakeConfig<EventT, ResponseT>(100);
  auto status_or_stateful_components =
      CreateStatefulSwiftAlgorithm<EventT, ResponseT>(config);
  ASSERT_OK(status_or_stateful_components);
  auto swift = std::move(status_or_stateful_components->first);
  auto state_manager = std::move(status_or_stateful_components->second);

  const auto new_now = 10300;

  this->event_.connection_id = 1234;
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
  this->event_.inter_packet_gap = 0;
  this->event_.retransmit_timeout = 1000;
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
  EXPECT_EQ(this->response_.randomize_path, false);
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

TYPED_TEST(SwiftTypedTest, ProcessRetransmitLimit) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;
  // Tests the case of retransmit where the congestion window decrease is
  // performed because it is the last retransmission. The congestion is set
  // to the minimum congestion window value.
  const auto config = MakeConfig<EventT, ResponseT>(100);
  auto status_or_stateful_components =
      CreateStatefulSwiftAlgorithm<EventT, ResponseT>(config);
  ASSERT_OK(status_or_stateful_components);
  auto swift = std::move(status_or_stateful_components->first);
  auto state_manager = std::move(status_or_stateful_components->second);

  const auto new_now = 103000;

  this->event_.connection_id = 1234;
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
  this->event_.inter_packet_gap = 0;
  this->event_.retransmit_timeout = 1000;
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
  EXPECT_EQ(this->response_.randomize_path, false);
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

TYPED_TEST(SwiftTypedTest, NicCongestionWindowIncrease) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;
  // Tests the ACK case of additive increment on the nic congestion window.
  const auto config = MakeConfig<EventT, ResponseT>(100);
  auto status_or_stateful_components =
      CreateStatefulSwiftAlgorithm<EventT, ResponseT>(config);
  ASSERT_OK(status_or_stateful_components);
  auto swift = std::move(status_or_stateful_components->first);
  auto state_manager = std::move(status_or_stateful_components->second);
  const auto new_now = 100400;

  this->event_.connection_id = 1234;
  this->event_.event_type = falcon::RueEventType::kAck;
  this->event_.timestamp_1 = 100000;
  this->event_.timestamp_2 = 100153;
  this->event_.timestamp_3 = 100200;
  this->event_.timestamp_4 = 100315;
  this->event_.retransmit_count = 0;
  this->event_.retransmit_reason = falcon::RetransmitReason::kTimeout;
  this->event_.nack_code = falcon::NackCode::kNotANack;
  this->event_.forward_hops = 5;
  this->event_.rx_buffer_level = 7;
  this->event_.cc_metadata = 0;
  this->event_.fabric_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(0.5,
                                                 falcon_rue::kFractionalBits);
  this->event_.inter_packet_gap = 0;
  this->event_.nic_congestion_window = 100;
  this->event_.retransmit_timeout = 1000;
  this->event_.num_packets_acked = 3;
  this->event_.event_queue_select = 0;
  this->event_.delay_select = falcon::DelaySelect::kForward;
  this->event_.fabric_window_time_marker = 99999;
  this->event_.nic_window_time_marker = 99999;
  this->event_.nic_window_direction = falcon::WindowDirection::kIncrease;
  this->event_.base_delay = SwiftTyped::MakeBaseDelayField(
      /*profile_index=*/kSwiftDefaultProfileIndex);
  this->event_.delay_state = 150;
  this->event_.rtt_state = 310;
  this->event_.cc_opaque = 3;
  this->event_.gen_bit = 0;

  swift->Process(this->event_, this->response_, new_now);

  EXPECT_EQ(this->response_.connection_id, this->event_.connection_id);
  EXPECT_EQ(this->response_.nic_congestion_window,
            this->event_.nic_congestion_window +
                config.nic_additive_increment_factor());       // updated
  EXPECT_EQ(this->response_.nic_window_time_marker, new_now);  // updated
  EXPECT_EQ(this->response_.nic_window_direction,
            falcon::WindowDirection::kIncrease);
}

TYPED_TEST(SwiftTypedTest, NicCongestionWindowNoIncrease) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;
  // Tests the ACK case of additive increment on the nic congestion window.
  // No increase because of NIC time marker.
  auto status_or_stateful_components =
      CreateStatefulSwiftAlgorithm<EventT, ResponseT>(
          MakeConfig<EventT, ResponseT>(100));
  ASSERT_OK(status_or_stateful_components);
  auto swift = std::move(status_or_stateful_components->first);
  auto state_manager = std::move(status_or_stateful_components->second);

  this->event_.connection_id = 1234;
  this->event_.event_type = falcon::RueEventType::kAck;
  this->event_.timestamp_1 = 100000;
  this->event_.timestamp_2 = 100153;
  this->event_.timestamp_3 = 100200;
  this->event_.timestamp_4 = 100315;  // rtt=315
  this->event_.retransmit_count = 0;
  this->event_.retransmit_reason = falcon::RetransmitReason::kTimeout;
  this->event_.nack_code = falcon::NackCode::kNotANack;
  this->event_.forward_hops = 5;
  this->event_.rx_buffer_level = 7;
  this->event_.cc_metadata = 0;
  this->event_.fabric_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(0.5,
                                                 falcon_rue::kFractionalBits);
  this->event_.inter_packet_gap = 0;
  this->event_.nic_congestion_window = 100;
  this->event_.retransmit_timeout = 1000;
  this->event_.num_packets_acked = 3;
  this->event_.event_queue_select = 0;
  this->event_.delay_select = falcon::DelaySelect::kForward;
  this->event_.fabric_window_time_marker = 99999;
  this->event_.nic_window_time_marker =
      100100;  // guard is within one RTT (based on timestamps)
  this->event_.nic_window_direction = falcon::WindowDirection::kIncrease;
  this->event_.base_delay = SwiftTyped::MakeBaseDelayField(
      /*profile_index=*/kSwiftDefaultProfileIndex);
  this->event_.delay_state = 150;
  this->event_.rtt_state = 10;
  this->event_.cc_opaque = 3;
  this->event_.gen_bit = 0;

  swift->Process(this->event_, this->response_, 100400);

  EXPECT_EQ(this->response_.connection_id, this->event_.connection_id);
  EXPECT_EQ(this->response_.nic_congestion_window,
            this->event_.nic_congestion_window);  // no change
  EXPECT_EQ(this->response_.nic_window_time_marker,
            this->event_.nic_window_time_marker);  //  no change
  EXPECT_EQ(this->response_.nic_window_direction,
            falcon::WindowDirection::kIncrease);
}

TYPED_TEST(SwiftTypedTest, NicCongestionWindowIncrease2) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;
  // Tests the ACK case of additive increment on the nic congestion window.
  // Increase because of NIC direction is decrease.
  const auto config = MakeConfig<EventT, ResponseT>(100);
  auto status_or_stateful_components =
      CreateStatefulSwiftAlgorithm<EventT, ResponseT>(config);
  ASSERT_OK(status_or_stateful_components);
  auto swift = std::move(status_or_stateful_components->first);
  auto state_manager = std::move(status_or_stateful_components->second);

  const auto new_now = 100400;

  this->event_.connection_id = 1234;
  this->event_.event_type = falcon::RueEventType::kAck;
  this->event_.timestamp_1 = 100000;
  this->event_.timestamp_2 = 100153;
  this->event_.timestamp_3 = 100200;
  this->event_.timestamp_4 = 100315;  // rtt=315
  this->event_.retransmit_count = 0;
  this->event_.retransmit_reason = falcon::RetransmitReason::kTimeout;
  this->event_.nack_code = falcon::NackCode::kNotANack;
  this->event_.forward_hops = 5;
  this->event_.rx_buffer_level = 7;
  this->event_.cc_metadata = 0;
  this->event_.fabric_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(0.5,
                                                 falcon_rue::kFractionalBits);
  this->event_.inter_packet_gap = 0;
  this->event_.nic_congestion_window = 100;
  this->event_.retransmit_timeout = 1000;
  this->event_.num_packets_acked = 3;
  this->event_.event_queue_select = 0;
  this->event_.delay_select = falcon::DelaySelect::kForward;
  this->event_.fabric_window_time_marker = 99999;
  this->event_.nic_window_time_marker =
      100100;  // guard is within one RTT based on timestamps
  this->event_.nic_window_direction = falcon::WindowDirection::kDecrease;
  this->event_.base_delay = SwiftTyped::MakeBaseDelayField(
      /*profile_index=*/kSwiftDefaultProfileIndex);
  this->event_.delay_state = 150;
  this->event_.rtt_state = 10;
  this->event_.cc_opaque = 3;
  this->event_.gen_bit = 0;

  swift->Process(this->event_, this->response_, new_now);

  EXPECT_EQ(this->response_.connection_id, this->event_.connection_id);
  EXPECT_EQ(this->response_.nic_congestion_window,
            this->event_.nic_congestion_window +
                config.nic_additive_increment_factor());       // updated
  EXPECT_EQ(this->response_.nic_window_time_marker, new_now);  // updated
  EXPECT_EQ(this->response_.nic_window_direction,
            falcon::WindowDirection::kIncrease);  // updated
}

TYPED_TEST(SwiftTypedTest, NicCongestionWindowDecrease1) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;
  // Tests the ACK case of multiplicative decrease on the nic congestion window.
  const auto config = MakeConfig<EventT, ResponseT>(100);
  auto status_or_stateful_components =
      CreateStatefulSwiftAlgorithm<EventT, ResponseT>(config);
  ASSERT_OK(status_or_stateful_components);
  auto swift = std::move(status_or_stateful_components->first);
  auto state_manager = std::move(status_or_stateful_components->second);

  const auto new_now = 100400;

  this->event_.connection_id = 1234;
  this->event_.event_type = falcon::RueEventType::kAck;
  this->event_.timestamp_1 = 100000;
  this->event_.timestamp_2 = 100153;
  this->event_.timestamp_3 = 100200;
  this->event_.timestamp_4 = 100315;  // rtt=315
  this->event_.retransmit_count = 0;
  this->event_.retransmit_reason = falcon::RetransmitReason::kTimeout;
  this->event_.nack_code = falcon::NackCode::kNotANack;
  this->event_.forward_hops = 5;
  this->event_.rx_buffer_level =
      static_cast<uint8_t>(config.target_rx_buffer_level() + 1),
  this->event_.cc_metadata = 0;
  this->event_.fabric_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(0.5,
                                                 falcon_rue::kFractionalBits);
  this->event_.inter_packet_gap = 0;
  this->event_.nic_congestion_window = 100;
  this->event_.retransmit_timeout = 1000;
  this->event_.num_packets_acked = 3;
  this->event_.event_queue_select = 0;
  this->event_.delay_select = falcon::DelaySelect::kForward;
  this->event_.fabric_window_time_marker = 99999;
  this->event_.nic_window_time_marker =
      99999;  // older than new_now-RTT based on timestamps
  this->event_.nic_window_direction = falcon::WindowDirection::kIncrease;
  this->event_.base_delay = SwiftTyped::MakeBaseDelayField(
      /*profile_index=*/kSwiftDefaultProfileIndex);
  this->event_.delay_state = 150;
  this->event_.rtt_state = 10;
  this->event_.cc_opaque = 3;
  this->event_.gen_bit = 0;

  swift->Process(this->event_, this->response_, new_now);

  EXPECT_EQ(this->response_.connection_id, this->event_.connection_id);
  double decrease_scale = static_cast<double>(this->event_.rx_buffer_level -
                                              config.target_rx_buffer_level()) /
                          this->event_.rx_buffer_level;
  decrease_scale *= config.nic_multiplicative_decrease_factor();
  uint8_t exp_cwnd =
      std::round(this->event_.nic_congestion_window * (1 - decrease_scale));
  EXPECT_EQ(this->response_.nic_congestion_window,
            exp_cwnd);                                         // updated
  EXPECT_EQ(this->response_.nic_window_time_marker, new_now);  // updated
  EXPECT_EQ(this->response_.nic_window_direction,
            falcon::WindowDirection::kDecrease);  // updated
}

TYPED_TEST(SwiftTypedTest, NicCongestionWindowDecrease2) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;
  // Tests the ACK case of multiplicative decrease on the nic congestion window.
  const auto config = MakeConfig<EventT, ResponseT>(100);
  auto status_or_stateful_components =
      CreateStatefulSwiftAlgorithm<EventT, ResponseT>(config);
  ASSERT_OK(status_or_stateful_components);
  auto swift = std::move(status_or_stateful_components->first);
  auto state_manager = std::move(status_or_stateful_components->second);

  const auto new_now = 100400;

  this->event_.connection_id = 1234;
  this->event_.event_type = falcon::RueEventType::kAck;
  this->event_.timestamp_1 = 100000;
  this->event_.timestamp_2 = 100153;
  this->event_.timestamp_3 = 100200;
  this->event_.timestamp_4 = 100315;  // rtt=315
  this->event_.retransmit_count = 0;
  this->event_.retransmit_reason = falcon::RetransmitReason::kTimeout;
  this->event_.nack_code = falcon::NackCode::kNotANack;
  this->event_.forward_hops = 5;
  this->event_.rx_buffer_level =
      static_cast<uint8_t>(config.target_rx_buffer_level() + 2),
  this->event_.cc_metadata = 0;
  this->event_.fabric_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(0.5,
                                                 falcon_rue::kFractionalBits);
  this->event_.inter_packet_gap = 0;
  this->event_.nic_congestion_window = 100;
  this->event_.retransmit_timeout = 1000;
  this->event_.num_packets_acked = 3;
  this->event_.event_queue_select = 0;
  this->event_.delay_select = falcon::DelaySelect::kForward;
  this->event_.fabric_window_time_marker = 99999;
  this->event_.nic_window_time_marker =
      99999;  // older than new_now-RTT based on timestamps
  this->event_.nic_window_direction = falcon::WindowDirection::kIncrease;
  this->event_.base_delay = SwiftTyped::MakeBaseDelayField(
      /*profile_index=*/kSwiftDefaultProfileIndex);
  this->event_.delay_state = 150;
  this->event_.rtt_state = 10;
  this->event_.cc_opaque = 3;
  this->event_.gen_bit = 0;

  swift->Process(this->event_, this->response_, new_now);

  EXPECT_EQ(this->response_.connection_id, this->event_.connection_id);
  double decrease_scale = static_cast<double>(this->event_.rx_buffer_level -
                                              config.target_rx_buffer_level()) /
                          this->event_.rx_buffer_level;
  decrease_scale *= config.nic_multiplicative_decrease_factor();
  uint8_t exp_cwnd =
      std::floor(this->event_.nic_congestion_window * (1 - decrease_scale));
  EXPECT_EQ(this->response_.nic_congestion_window,
            exp_cwnd);                                         // updated
  EXPECT_EQ(this->response_.nic_window_time_marker, new_now);  // updated
  EXPECT_EQ(this->response_.nic_window_direction,
            falcon::WindowDirection::kDecrease);  // updated
}

TYPED_TEST(SwiftTypedTest, NicCongestionWindowNoDecrease2) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;
  // Tests the ACK case of multiplicative decrease on the nic congestion window.
  // Within one RTT thus no decreases even though direction was increase.
  const auto config = MakeConfig<EventT, ResponseT>(100);
  auto status_or_stateful_components =
      CreateStatefulSwiftAlgorithm<EventT, ResponseT>(config);
  ASSERT_OK(status_or_stateful_components);
  auto swift = std::move(status_or_stateful_components->first);
  auto state_manager = std::move(status_or_stateful_components->second);

  this->event_.connection_id = 1234;
  this->event_.event_type = falcon::RueEventType::kAck;
  this->event_.timestamp_1 = 100000;
  this->event_.timestamp_2 = 100153;
  this->event_.timestamp_3 = 100200;
  this->event_.timestamp_4 = 100315;  // rtt=315
  this->event_.retransmit_count = 0;
  this->event_.retransmit_reason = falcon::RetransmitReason::kTimeout;
  this->event_.nack_code = falcon::NackCode::kNotANack;
  this->event_.forward_hops = 5;
  this->event_.rx_buffer_level =
      static_cast<uint8_t>(config.target_rx_buffer_level() + 2),
  this->event_.cc_metadata = 0;
  this->event_.fabric_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(0.5,
                                                 falcon_rue::kFractionalBits);
  this->event_.inter_packet_gap = 0;
  this->event_.nic_congestion_window = 100;
  this->event_.retransmit_timeout = 1000;
  this->event_.num_packets_acked = 3;
  this->event_.event_queue_select = 0;
  this->event_.delay_select = falcon::DelaySelect::kForward;
  this->event_.fabric_window_time_marker = 99999;
  this->event_.nic_window_time_marker =
      100100;  // guard is within one RTT based on timestamps
  this->event_.nic_window_direction = falcon::WindowDirection::kIncrease;
  this->event_.base_delay = SwiftTyped::MakeBaseDelayField(
      /*profile_index=*/kSwiftDefaultProfileIndex);
  this->event_.delay_state = 150;
  this->event_.rtt_state = 10;
  this->event_.cc_opaque = 3;
  this->event_.gen_bit = 0;

  swift->Process(this->event_, this->response_, 100400);

  EXPECT_EQ(this->response_.connection_id, this->event_.connection_id);
  EXPECT_EQ(this->response_.nic_congestion_window,
            this->event_.nic_congestion_window);
  EXPECT_EQ(this->response_.nic_window_time_marker,
            this->event_.nic_window_time_marker);  // no change
  EXPECT_EQ(this->response_.nic_window_direction,
            falcon::WindowDirection::kIncrease);  // no change
}

TYPED_TEST(SwiftTypedTest, NicCongestionWindowNoDecrease) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;
  // Tests the ACK case of multiplicative decrease on the nic congestion window.
  // This doesn't run because the direction is the same.
  const auto config = MakeConfig<EventT, ResponseT>(100);
  auto status_or_stateful_components =
      CreateStatefulSwiftAlgorithm<EventT, ResponseT>(config);
  ASSERT_OK(status_or_stateful_components);
  auto swift = std::move(status_or_stateful_components->first);
  auto state_manager = std::move(status_or_stateful_components->second);

  this->event_.connection_id = 1234;
  this->event_.event_type = falcon::RueEventType::kAck;
  this->event_.timestamp_1 = 100000;
  this->event_.timestamp_2 = 100153;
  this->event_.timestamp_3 = 100200;
  this->event_.timestamp_4 = 100315;
  this->event_.retransmit_count = 0;
  this->event_.retransmit_reason = falcon::RetransmitReason::kTimeout;
  this->event_.nack_code = falcon::NackCode::kNotANack;
  this->event_.forward_hops = 5;
  this->event_.rx_buffer_level =
      static_cast<uint8_t>(config.target_rx_buffer_level() + 2),
  this->event_.cc_metadata = 0;
  this->event_.fabric_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(0.5,
                                                 falcon_rue::kFractionalBits);
  this->event_.inter_packet_gap = 0;
  this->event_.nic_congestion_window = 100;
  this->event_.retransmit_timeout = 1000;
  this->event_.num_packets_acked = 3;
  this->event_.event_queue_select = 0;
  this->event_.delay_select = falcon::DelaySelect::kForward;
  this->event_.fabric_window_time_marker = 99999;
  this->event_.nic_window_time_marker = 100100;
  this->event_.nic_window_direction = falcon::WindowDirection::kDecrease;
  this->event_.base_delay = SwiftTyped::MakeBaseDelayField(
      /*profile_index=*/kSwiftDefaultProfileIndex);
  this->event_.delay_state = 150;
  this->event_.rtt_state = 310;
  this->event_.cc_opaque = 3;
  this->event_.gen_bit = 0;

  swift->Process(this->event_, this->response_, 100400);

  EXPECT_EQ(this->response_.connection_id, this->event_.connection_id);
  EXPECT_EQ(this->response_.nic_congestion_window,
            this->event_.nic_congestion_window);  // no change
  EXPECT_EQ(this->response_.nic_window_time_marker,
            this->response_.nic_window_time_marker);  // no change
  EXPECT_EQ(this->response_.nic_window_direction,
            falcon::WindowDirection::kDecrease);
}

TYPED_TEST(SwiftTypedTest, NicCongestionWindowFractionalDecrease) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;
  // Tests NCWND decrease below 1. This requires ConnectionState for Gen_1,
  // where event/response NCWND is integer-only.
  auto config = MakeConfig<EventT, ResponseT>(100);

  // Enable fractional NCWND for this test.
  config.set_min_nic_congestion_window(0.01);

  auto status_or_stateful_components =
      CreateStatefulSwiftAlgorithm<EventT, ResponseT>(config);
  ASSERT_OK(status_or_stateful_components);
  auto swift = std::move(status_or_stateful_components->first);
  auto state_manager = std::move(status_or_stateful_components->second);

  const auto new_now = 100400;

  auto* connection_state = reinterpret_cast<ConnectionState<EventT>*>(
      state_manager->GetRawStateForEvent(this->event_));

  EXPECT_EQ(connection_state->ncwnd_fraction, 0);

  this->event_.connection_id = 1234;
  this->event_.event_type = falcon::RueEventType::kAck;
  this->event_.timestamp_1 = 100000;
  this->event_.timestamp_2 = 100153;
  this->event_.timestamp_3 = 100200;
  this->event_.timestamp_4 = 100315;  // rtt=315
  this->event_.retransmit_count = 0;
  this->event_.retransmit_reason = falcon::RetransmitReason::kTimeout;
  this->event_.nack_code = falcon::NackCode::kNotANack;
  this->event_.forward_hops = 5;
  this->event_.rx_buffer_level =
      static_cast<uint8_t>(config.target_rx_buffer_level() + 2),
  this->event_.cc_metadata = 0;
  this->event_.fabric_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(128,
                                                 falcon_rue::kFractionalBits);
  this->event_.inter_packet_gap = 0;
  this->event_.nic_congestion_window = 1;
  this->event_.retransmit_timeout = 1000;
  this->event_.num_packets_acked = 3;
  this->event_.event_queue_select = 0;
  this->event_.delay_select = falcon::DelaySelect::kForward;
  this->event_.fabric_window_time_marker = 99999;
  this->event_.nic_window_time_marker =
      99999;  // older than new_now-RTT based on timestamps
  this->event_.nic_window_direction = falcon::WindowDirection::kIncrease;
  this->event_.base_delay = SwiftTyped::MakeBaseDelayField(
      /*profile_index=*/kSwiftDefaultProfileIndex);
  this->event_.delay_state = 150;
  this->event_.rtt_state = 10;
  this->event_.cc_opaque = 3;
  this->event_.gen_bit = 0;

  swift->Process(this->event_, this->response_, new_now);

  EXPECT_EQ(this->response_.connection_id, this->event_.connection_id);
  EXPECT_EQ(this->response_.nic_window_direction,
            falcon::WindowDirection::kDecrease);  // updated

  // NCWND in response is integer-only, so it should be 0.
  EXPECT_EQ(this->response_.nic_congestion_window, 0);
  // NCWND fraction should be be equal to the multiplicative decrease factor
  // since initial NCWND is 1.0.
  double decrease_scale =
      (static_cast<double>(this->event_.rx_buffer_level -
                           config.target_rx_buffer_level()) /
       this->event_.rx_buffer_level) *
      config.nic_multiplicative_decrease_factor();

  // Previous NCWND was integer-only (1.0).
  double decreased_ncwnd =
      this->event_.nic_congestion_window * (1.0 - decrease_scale);

  uint16_t ncwnd_fraction_uint16 = falcon_rue::FloatToFixed<double, uint16_t>(
      decreased_ncwnd, falcon_rue::kFractionalBits);
  EXPECT_EQ(connection_state->ncwnd_fraction, ncwnd_fraction_uint16);

  // With NCWND fraction (and FCWND > 1), IPG is set based on NCWND.
  uint32_t rtt = this->event_.timestamp_4 - this->event_.timestamp_1;
  double expected_ipg_float =
      ((rtt / decreased_ncwnd) - rtt) * config.ipg_time_scalar();
  EXPECT_EQ(this->response_.inter_packet_gap, std::round(expected_ipg_float));
}

TYPED_TEST(SwiftTypedTest, NicCongestionWindowFractionalIncrease) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;
  // Tests the ACK case of additive increment on the nic congestion window.
  auto config = MakeConfig<EventT, ResponseT>(100);

  // Enable fractional NCWND for this test.
  config.set_min_nic_congestion_window(0.01);

  auto status_or_stateful_components =
      CreateStatefulSwiftAlgorithm<EventT, ResponseT>(config);
  ASSERT_OK(status_or_stateful_components);
  auto swift = std::move(status_or_stateful_components->first);
  auto state_manager = std::move(status_or_stateful_components->second);

  const auto new_now = 100400;

  auto* connection_state = reinterpret_cast<ConnectionState<EventT>*>(
      state_manager->GetRawStateForEvent(this->event_));

  // Initial NCWND is 0.8 (zero value in event, 0.8 in state).
  double ncwnd_before = 0.8;
  connection_state->ncwnd_fraction = falcon_rue::FloatToFixed<double, uint16_t>(
      ncwnd_before, falcon_rue::kFractionalBits);

  this->event_.connection_id = 1234;
  this->event_.event_type = falcon::RueEventType::kAck;
  this->event_.timestamp_1 = 100000;
  this->event_.timestamp_2 = 100153;
  this->event_.timestamp_3 = 100200;
  this->event_.timestamp_4 = 100315;
  this->event_.retransmit_count = 0;
  this->event_.retransmit_reason = falcon::RetransmitReason::kTimeout;
  this->event_.nack_code = falcon::NackCode::kNotANack;
  this->event_.forward_hops = 5;
  this->event_.rx_buffer_level = 7;
  this->event_.cc_metadata = 0;
  this->event_.fabric_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(128,
                                                 falcon_rue::kFractionalBits);
  this->event_.inter_packet_gap = 0;
  this->event_.nic_congestion_window = std::floor(ncwnd_before);
  this->event_.retransmit_timeout = 1000;
  this->event_.num_packets_acked = 3;
  this->event_.event_queue_select = 0;
  this->event_.delay_select = falcon::DelaySelect::kForward;
  this->event_.fabric_window_time_marker = 99999;
  this->event_.nic_window_time_marker = 99999;
  this->event_.nic_window_direction = falcon::WindowDirection::kIncrease;
  this->event_.base_delay = SwiftTyped::MakeBaseDelayField(
      /*profile_index=*/kSwiftDefaultProfileIndex);
  this->event_.delay_state = 150;
  this->event_.rtt_state = 310;
  this->event_.cc_opaque = 3;
  this->event_.gen_bit = 0;

  swift->Process(this->event_, this->response_, new_now);

  double ncwnd_after = ncwnd_before + config.nic_additive_increment_factor();
  uint32_t ncwnd_after_integer = std::floor(ncwnd_after);
  uint16_t ncwnd_after_fraction = falcon_rue::FloatToFixed<double, uint16_t>(
      ncwnd_after - ncwnd_after_integer, falcon_rue::kFractionalBits);

  EXPECT_EQ(this->response_.connection_id, this->event_.connection_id);
  EXPECT_EQ(this->response_.nic_window_direction,
            falcon::WindowDirection::kIncrease);

  EXPECT_EQ(this->response_.nic_congestion_window, ncwnd_after_integer);
  EXPECT_EQ(connection_state->ncwnd_fraction, ncwnd_after_fraction);

  // IPG is zero after NCWND increases above 1.0.
  EXPECT_EQ(this->response_.inter_packet_gap, 0);
}

TYPED_TEST(SwiftTypedTest, ProcessNackNicDecrease1) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;
  // Tests the case of NACK where the nic congestion window decreases
  // Decrease after decrease past one RTT
  const auto config = MakeConfig<EventT, ResponseT>(100);
  auto status_or_stateful_components =
      CreateStatefulSwiftAlgorithm<EventT, ResponseT>(config);
  ASSERT_OK(status_or_stateful_components);
  auto swift = std::move(status_or_stateful_components->first);
  auto state_manager = std::move(status_or_stateful_components->second);

  const auto new_now = 102000;
  this->event_.connection_id = 1234;
  this->event_.event_type = falcon::RueEventType::kNack;
  this->event_.timestamp_1 = 100000;
  this->event_.timestamp_2 = 100879;
  this->event_.timestamp_3 = 101000;
  this->event_.timestamp_4 = 101200;  // rtt=1200
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
  this->event_.inter_packet_gap = 0;
  this->event_.nic_congestion_window = 100;
  this->event_.retransmit_timeout = 1000;
  this->event_.num_packets_acked = 3;
  this->event_.event_queue_select = 0;
  this->event_.delay_select = falcon::DelaySelect::kForward;
  this->event_.fabric_window_time_marker = 99999;
  this->event_.nic_window_time_marker =
      99999;  // smaller than new_now - rtt (based on timestamps)
  this->event_.nic_window_direction = falcon::WindowDirection::kDecrease;
  this->event_.base_delay = SwiftTyped::MakeBaseDelayField(
      /*profile_index=*/kSwiftDefaultProfileIndex);
  this->event_.delay_state = 150;
  this->event_.rtt_state = 10;
  this->event_.cc_opaque = 3;
  this->event_.gen_bit = 0;

  swift->Process(this->event_, this->response_, new_now);

  EXPECT_EQ(this->response_.connection_id, this->event_.connection_id);
  EXPECT_EQ(this->response_.nic_congestion_window,
            this->event_.nic_congestion_window *
                config.max_nic_multiplicative_decrease());     // updated
  EXPECT_EQ(this->response_.nic_window_time_marker, new_now);  // updated
  EXPECT_EQ(this->response_.nic_window_direction,
            falcon::WindowDirection::kDecrease);
}

TYPED_TEST(SwiftTypedTest, ProcessNackNicDecrease2) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;
  // Tests the case of NACK where the nic congestion window decreases
  // Decrease after increase within one RTT
  const auto config = MakeConfig<EventT, ResponseT>(100);
  auto status_or_stateful_components =
      CreateStatefulSwiftAlgorithm<EventT, ResponseT>(config);
  ASSERT_OK(status_or_stateful_components);
  auto swift = std::move(status_or_stateful_components->first);
  auto state_manager = std::move(status_or_stateful_components->second);

  const auto new_now = 102000;

  this->event_.connection_id = 1234;
  this->event_.event_type = falcon::RueEventType::kNack;
  this->event_.timestamp_1 = 100000;
  this->event_.timestamp_2 = 100879;
  this->event_.timestamp_3 = 101000;
  this->event_.timestamp_4 = 101200;  // rtt=1200
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
  this->event_.inter_packet_gap = 0;
  this->event_.nic_congestion_window = 100;
  this->event_.retransmit_timeout = 1000;
  this->event_.num_packets_acked = 3;
  this->event_.event_queue_select = 0;
  this->event_.delay_select = falcon::DelaySelect::kForward;
  this->event_.fabric_window_time_marker = 99999;
  this->event_.nic_window_time_marker = 101000;
  this->event_.nic_window_direction = falcon::WindowDirection::kIncrease;
  this->event_.base_delay = SwiftTyped::MakeBaseDelayField(
      /*profile_index=*/kSwiftDefaultProfileIndex);
  this->event_.delay_state = 150;
  this->event_.rtt_state = 10;
  this->event_.cc_opaque = 3;
  this->event_.gen_bit = 0;

  swift->Process(this->event_, this->response_, new_now);

  EXPECT_EQ(this->response_.connection_id, this->event_.connection_id);
  EXPECT_EQ(this->response_.nic_congestion_window,
            this->event_.nic_congestion_window *
                config.max_nic_multiplicative_decrease());     // updated
  EXPECT_EQ(this->response_.nic_window_time_marker, new_now);  // updated
  EXPECT_EQ(this->response_.nic_window_direction,
            falcon::WindowDirection::kDecrease);  // updated
}

TYPED_TEST(SwiftTypedTest, ProcessNackNicNoDecrease1) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;
  // Tests the case of NACK where the nic congestion window decrease is blocked
  // Decrease after decrease within one RTT
  const auto config = MakeConfig<EventT, ResponseT>(100);
  auto status_or_stateful_components =
      CreateStatefulSwiftAlgorithm<EventT, ResponseT>(config);
  ASSERT_OK(status_or_stateful_components);
  auto swift = std::move(status_or_stateful_components->first);
  auto state_manager = std::move(status_or_stateful_components->second);

  this->event_.connection_id = 1234;
  this->event_.event_type = falcon::RueEventType::kNack;
  this->event_.timestamp_1 = 100000;
  this->event_.timestamp_2 = 100879;
  this->event_.timestamp_3 = 101000;
  this->event_.timestamp_4 = 101200;
  this->event_.retransmit_count = 0;
  this->event_.retransmit_reason = falcon::RetransmitReason::kTimeout;
  this->event_.nack_code = falcon::NackCode::kRxResourceExhaustion;
  this->event_.forward_hops = 5;
  this->event_.rx_buffer_level = 16;
  this->event_.cc_metadata = 0;
  this->event_.fabric_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(2.75,
                                                 falcon_rue::kFractionalBits);
  this->event_.inter_packet_gap = 0;
  this->event_.nic_congestion_window = 100;
  this->event_.retransmit_timeout = 1000;
  this->event_.num_packets_acked = 3;
  this->event_.event_queue_select = 0;
  this->event_.delay_select = falcon::DelaySelect::kForward;
  this->event_.fabric_window_time_marker = 99999;
  this->event_.nic_window_time_marker = 101000;
  this->event_.nic_window_direction = falcon::WindowDirection::kDecrease;
  this->event_.base_delay = SwiftTyped::MakeBaseDelayField(
      /*profile_index=*/kSwiftDefaultProfileIndex);
  this->event_.delay_state = 150;
  this->event_.rtt_state = 1000;
  this->event_.cc_opaque = 3;
  this->event_.gen_bit = 0;

  swift->Process(this->event_, this->response_, 102000);

  EXPECT_EQ(this->response_.connection_id, this->event_.connection_id);
  EXPECT_EQ(this->response_.nic_congestion_window,
            this->event_.nic_congestion_window);  // no change
  EXPECT_EQ(this->response_.nic_window_time_marker,
            this->event_.nic_window_time_marker);  // no change
  EXPECT_EQ(this->response_.nic_window_direction,
            falcon::WindowDirection::kDecrease);
}

TYPED_TEST(SwiftTypedTest, ProcessNackNicDecreaseWrongNackCode) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;
  // Tests the case of NACK where the nic congestion window is not decreased
  // because the NACK type is not resource based.
  const auto config = MakeConfig<EventT, ResponseT>(100);
  auto status_or_stateful_components =
      CreateStatefulSwiftAlgorithm<EventT, ResponseT>(config);
  ASSERT_OK(status_or_stateful_components);
  auto swift = std::move(status_or_stateful_components->first);
  auto state_manager = std::move(status_or_stateful_components->second);

  const auto new_now = 102000;

  this->event_.connection_id = 1234;
  this->event_.event_type = falcon::RueEventType::kNack;
  this->event_.timestamp_1 = 100000;
  this->event_.timestamp_2 = 100879;
  this->event_.timestamp_3 = 101000;
  this->event_.timestamp_4 = 101200;
  this->event_.retransmit_count = 0;
  this->event_.retransmit_reason = falcon::RetransmitReason::kTimeout;
  this->event_.nack_code =
      falcon::NackCode::kReserved;  // not a nic buffer related nak.
  this->event_.forward_hops = 5;
  this->event_.rx_buffer_level = 16;
  this->event_.cc_metadata = 0;
  this->event_.fabric_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(2.75,
                                                 falcon_rue::kFractionalBits);
  this->event_.inter_packet_gap = 0;
  this->event_.nic_congestion_window = 100;
  this->event_.retransmit_timeout = 1000;
  this->event_.num_packets_acked = 3;
  this->event_.event_queue_select = 0;
  this->event_.delay_select = falcon::DelaySelect::kForward;
  this->event_.fabric_window_time_marker = 99999;
  this->event_.nic_window_time_marker = 99999;
  this->event_.nic_window_direction = falcon::WindowDirection::kDecrease;
  this->event_.base_delay = SwiftTyped::MakeBaseDelayField(
      /*profile_index=*/kSwiftDefaultProfileIndex);
  this->event_.delay_state = 150;
  this->event_.rtt_state = 1000;
  this->event_.cc_opaque = 3;
  this->event_.gen_bit = 0;

  swift->Process(this->event_, this->response_, new_now);

  EXPECT_EQ(this->response_.connection_id, this->event_.connection_id);
  double decrease_scale = static_cast<double>(this->event_.rx_buffer_level -
                                              config.target_rx_buffer_level()) /
                          this->event_.rx_buffer_level;
  decrease_scale *= config.nic_multiplicative_decrease_factor();
  uint8_t exp_cwnd =
      std::round(this->event_.nic_congestion_window * (1 - decrease_scale));
  EXPECT_EQ(this->response_.nic_congestion_window,
            exp_cwnd);  // updated based on delay not nak
  EXPECT_EQ(this->response_.nic_window_time_marker, new_now);  // updated
  EXPECT_EQ(this->response_.nic_window_direction,
            falcon::WindowDirection::kDecrease);
}

TYPED_TEST(SwiftTypedTest, ProcessAckRandomizePath) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;
  // Tests path randomize flag
  SwiftConfiguration config = MakeConfig<EventT, ResponseT>(100);
  config.set_randomize_path(true);
  auto status_or_stateful_components =
      CreateStatefulSwiftAlgorithm<EventT, ResponseT>(config);
  ASSERT_OK(status_or_stateful_components);
  auto swift = std::move(status_or_stateful_components->first);
  auto state_manager = std::move(status_or_stateful_components->second);

  this->event_.connection_id = 1234;
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
  this->event_.inter_packet_gap = 0;
  this->event_.retransmit_timeout = 1000;
  this->event_.num_packets_acked = 3;
  this->event_.event_queue_select = 0;
  this->event_.delay_select = falcon::DelaySelect::kForward;
  this->event_.fabric_window_time_marker = 101000;
  this->event_.base_delay = SwiftTyped::MakeBaseDelayField(
      /*profile_index=*/kSwiftDefaultProfileIndex);
  this->event_.delay_state = 150;
  this->event_.rtt_state = 1000;
  this->event_.cc_opaque = 1;  // set only bit 0
  this->event_.gen_bit = 0;

  swift->Process(this->event_, this->response_, 102000);

  EXPECT_EQ(this->response_.connection_id, this->event_.connection_id);
  EXPECT_EQ(this->response_.randomize_path, true);
}

TYPED_TEST(SwiftTypedTest, ProcessAckRandomizePathByProfile) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;
  constexpr int kProfileIndex = 1;
  // Tests path randomize flag
  SwiftConfiguration config = MakeConfig<EventT, ResponseT>(100);
  config.set_randomize_path(false);
  auto status_or_stateful_components =
      CreateStatefulSwiftAlgorithm<EventT, ResponseT>(config);
  ASSERT_OK(status_or_stateful_components);
  auto swift = std::move(status_or_stateful_components->first);
  auto state_manager = std::move(status_or_stateful_components->second);

  config.set_randomize_path(true);
  AlgorithmConfiguration algorithm_config;
  *algorithm_config.mutable_swift() = config;
  EXPECT_OK(swift->InstallAlgorithmProfile(kProfileIndex, algorithm_config));

  this->event_.connection_id = 1234;
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
  this->event_.inter_packet_gap = 0;
  this->event_.retransmit_timeout = 1000;
  this->event_.num_packets_acked = 3;
  this->event_.event_queue_select = 0;
  this->event_.delay_select = falcon::DelaySelect::kForward;
  this->event_.fabric_window_time_marker = 101000;
  this->event_.base_delay =
      SwiftTyped::MakeBaseDelayField(kSwiftDefaultProfileIndex);
  this->event_.delay_state = 150;
  this->event_.rtt_state = 1000;
  this->event_.cc_opaque = 1;  // set only bit 0
  this->event_.gen_bit = 0;

  swift->Process(this->event_, this->response_, 102000);
  EXPECT_EQ(this->response_.randomize_path,
            false);  // As it used the default profile
  EXPECT_EQ(this->response_.base_delay, this->event_.base_delay);

  this->event_.base_delay = SwiftTyped::MakeBaseDelayField(
      /*profile_index=*/-1);  // An invalid profile
  swift->Process(this->event_, this->response_, 102000);
  EXPECT_EQ(this->response_.randomize_path,
            false);  // As it used the default profile
  EXPECT_EQ(this->response_.base_delay, this->event_.base_delay);

  this->event_.base_delay = SwiftTyped::MakeBaseDelayField(
      /*profile_index=*/3);  // Not installed profile
  swift->Process(this->event_, this->response_, 102000);
  EXPECT_EQ(this->response_.randomize_path,
            false);  // As it used the default profile
  EXPECT_EQ(this->response_.base_delay, this->event_.base_delay);

  this->event_.base_delay = SwiftTyped::MakeBaseDelayField(kProfileIndex);
  swift->Process(this->event_, this->response_, 102000);
  EXPECT_EQ(this->response_.randomize_path, true);
  EXPECT_EQ(this->response_.base_delay, this->event_.base_delay);
}

TYPED_TEST(SwiftTypedTest, ProcessAckDontRandomizePath) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;
  // Tests path randomize flag
  SwiftConfiguration config = MakeConfig<EventT, ResponseT>(100);
  config.set_randomize_path(true);
  auto status_or_stateful_components =
      CreateStatefulSwiftAlgorithm<EventT, ResponseT>(config);
  ASSERT_OK(status_or_stateful_components);
  auto swift = std::move(status_or_stateful_components->first);
  auto state_manager = std::move(status_or_stateful_components->second);

  this->event_.connection_id = 1234;
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
  this->event_.inter_packet_gap = 0;
  this->event_.retransmit_timeout = 1000;
  this->event_.num_packets_acked = 3;
  this->event_.event_queue_select = 0;
  this->event_.delay_select = falcon::DelaySelect::kForward;
  this->event_.fabric_window_time_marker = 101000;
  this->event_.base_delay = SwiftTyped::MakeBaseDelayField(
      /*profile_index=*/kSwiftDefaultProfileIndex);
  this->event_.delay_state = 150;
  this->event_.rtt_state = 1000;
  this->event_.cc_opaque = 2;  // don't set bit zero (kRandomizePathEnableMask)
  this->event_.gen_bit = 0;

  swift->Process(this->event_, this->response_, 102000);

  EXPECT_EQ(this->response_.connection_id, this->event_.connection_id);
  EXPECT_EQ(this->response_.randomize_path, false);
}

TYPED_TEST(SwiftTypedTest, PlbTest) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  using SwiftTyped = Swift<EventT, ResponseT>;
  // Tests path randomize flag
  const uint32_t plb_attempt_threshold = 5;
  SwiftConfiguration config = MakeConfig<EventT, ResponseT>(100);
  config.set_randomize_path(true);
  config.set_max_fabric_congestion_window(64);
  config.set_max_nic_congestion_window(64);
  config.set_plb_attempt_threshold(plb_attempt_threshold);
  config.set_plb_congestion_threshold(0.5);
  config.set_plb_target_rtt_multiplier(1.2);
  auto status_or_stateful_components =
      CreateStatefulSwiftAlgorithm<EventT, ResponseT>(config);
  ASSERT_OK(status_or_stateful_components);
  auto swift = std::move(status_or_stateful_components->first);
  auto state_manager = std::move(status_or_stateful_components->second);

  auto now = 102200;
  const auto long_delay_timestamp = 100879;
  const auto short_delay_timestamp = 100100;

  this->event_.connection_id = 1234;
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
  this->event_.inter_packet_gap = 0;
  this->event_.nic_congestion_window = 64;
  this->event_.retransmit_timeout = 1000;
  this->event_.num_packets_acked = 40;
  this->event_.event_queue_select = 0;
  this->event_.delay_select = falcon::DelaySelect::kForward;
  this->event_.fabric_window_time_marker = 101000;
  this->event_.base_delay = SwiftTyped::MakeBaseDelayField(
      /*profile_index=*/kSwiftDefaultProfileIndex);
  this->event_.delay_state = 0;
  this->event_.rtt_state = 1000;
  this->event_.cc_opaque = 0;  // don't set bit zero (kRandomizePathEnableMask)
  this->event_.gen_bit = 0;

  swift->Process(this->event_, this->response_, now);
  auto* connection_state = reinterpret_cast<ConnectionState<EventT>*>(
      state_manager->GetRawStateForEvent(this->event_));

  // 40 Acked with big fabric delay.
  EXPECT_EQ(this->response_.connection_id, this->event_.connection_id);
  EXPECT_EQ(this->response_.randomize_path, false);
  EXPECT_EQ(connection_state->plb_state.packets_congestion_acknowledged, 40);
  EXPECT_EQ(connection_state->plb_state.plb_reroute_attempted, 0);
  EXPECT_EQ(connection_state->plb_state.packets_congestion_acknowledged, 40);

  // 25 Acked with small fabric delay. Should increment plb_reroute_attempted as
  // 40/(40+25) > 0.5.
  // This also tests no-integer-overflow, as 40+25=65 > 6bits can represent.
  this->event_.num_packets_acked = 25;
  this->event_.timestamp_2 = short_delay_timestamp;
  swift->Process(this->event_, this->response_, now);
  EXPECT_EQ(this->response_.randomize_path, false);
  EXPECT_EQ(connection_state->plb_state.packets_congestion_acknowledged, 0);
  EXPECT_EQ(connection_state->plb_state.plb_reroute_attempted, 1);
  EXPECT_EQ(connection_state->plb_state.packets_congestion_acknowledged, 0);

  // 70 Acked with small fabric delay (70 > 64=fcwnd). Should clear
  // plb_reroute_attempted.
  this->event_.num_packets_acked = 70;
  this->event_.timestamp_2 = short_delay_timestamp;
  swift->Process(this->event_, this->response_, now);
  EXPECT_EQ(this->response_.randomize_path, false);
  EXPECT_EQ(connection_state->plb_state.packets_congestion_acknowledged, 0);
  EXPECT_EQ(connection_state->plb_state.plb_reroute_attempted, 0);
  EXPECT_EQ(connection_state->plb_state.packets_congestion_acknowledged, 0);

  // For consecutive plb_attempt_threshold rounds.
  for (int i = 0; i < plb_attempt_threshold; i++) {
    // 70 Acked with big fabric delay. Each round should increment
    // plb_reroute_attempted.
    this->event_.num_packets_acked = 70;  // 70 > 64=fcwnd
    this->event_.timestamp_2 = long_delay_timestamp;
    swift->Process(this->event_, this->response_, now);
    // The last round should indicate randomize_path.
    EXPECT_EQ(this->response_.randomize_path, i + 1 == plb_attempt_threshold);
    EXPECT_EQ(connection_state->plb_state.packets_congestion_acknowledged, 0);
    // The last round should clear plb_reroute_attempted.
    EXPECT_EQ(connection_state->plb_state.plb_reroute_attempted,
              i + 1 == plb_attempt_threshold ? 0 : i + 1);
    EXPECT_EQ(connection_state->plb_state.packets_congestion_acknowledged, 0);
  }

  // 33 Acked with big fabric delay.
  this->event_.num_packets_acked = 33;
  this->event_.timestamp_2 = long_delay_timestamp;
  swift->Process(this->event_, this->response_, now);
  EXPECT_EQ(this->response_.randomize_path, false);
  EXPECT_EQ(connection_state->plb_state.packets_congestion_acknowledged, 33);
  EXPECT_EQ(connection_state->plb_state.plb_reroute_attempted, 0);
  EXPECT_EQ(connection_state->plb_state.packets_congestion_acknowledged, 33);

  // 34 Acked with small fabric delay. Should clear plb_reroute_attempted as
  // 33/(33+34) < 0.5.
  this->event_.num_packets_acked = 34;
  this->event_.timestamp_2 = short_delay_timestamp;
  swift->Process(this->event_, this->response_, now);
  // connection_state->plb_state.value = this->response_.delay_state;
  EXPECT_EQ(this->response_.randomize_path, false);
  EXPECT_EQ(connection_state->plb_state.packets_congestion_acknowledged, 0);
  EXPECT_EQ(connection_state->plb_state.plb_reroute_attempted, 0);
  EXPECT_EQ(connection_state->plb_state.packets_congestion_acknowledged, 0);
}

TYPED_TEST(SwiftTypedTest, InvalidProfileIndex) {
  using EventT = typename SwiftTypedTest<TypeParam>::EventT;
  using ResponseT = typename SwiftTypedTest<TypeParam>::ResponseT;
  SwiftConfiguration config = MakeConfig<EventT, ResponseT>(100);
  auto status_or_stateful_components =
      CreateStatefulSwiftAlgorithm<EventT, ResponseT>(config);
  ASSERT_OK(status_or_stateful_components);
  auto swift = std::move(status_or_stateful_components->first);
  auto state_manager = std::move(status_or_stateful_components->second);
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

class SwiftTestGen1
    : public SwiftTypedTest<
          std::tuple<falcon_rue::Event_GEN1, falcon_rue::Response_GEN1>> {
 public:
  ~SwiftTestGen1() override = default;
};

TEST_F(SwiftTestGen1, ProcessEackDropMaxDecrease) {
  using SwiftGen1Type =
      Swift<falcon_rue::Event_GEN1, falcon_rue::Response_GEN1>;
  // Tests the case of EACK drop where the max congestion window decrease is
  // performed.
  auto config =
      MakeConfig<falcon_rue::Event_GEN1, falcon_rue::Response_GEN1>(100);
  config.set_max_decrease_on_eack_nack_drop(true);
  auto status_or_stateful_components =
      CreateStatefulSwiftAlgorithm<EventT, ResponseT>(config);
  ASSERT_OK(status_or_stateful_components);
  auto swift = std::move(status_or_stateful_components->first);
  auto state_manager = std::move(status_or_stateful_components->second);

  const auto new_now = 100200;

  const double fcwnd = 32;
  this->event_.connection_id = 1234;
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
  this->event_.inter_packet_gap = 0;
  this->event_.retransmit_timeout = 1000;
  this->event_.num_packets_acked = 3;
  this->event_.event_queue_select = 0;
  this->event_.delay_select = falcon::DelaySelect::kForward;
  this->event_.fabric_window_time_marker = 99999;
  this->event_.base_delay = SwiftGen1Type::MakeBaseDelayField(
      /*profile_index=*/kSwiftDefaultProfileIndex);
  this->event_.delay_state = 150;
  this->event_.rtt_state = 1000;
  this->event_.cc_opaque = 3;
  this->event_.gen_bit = 0;

  swift->Process(this->event_, this->response_, new_now);

  EXPECT_EQ(this->response_.connection_id, this->event_.connection_id);
  EXPECT_EQ(this->response_.randomize_path, false);
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
  uint32_t exp_rto = std::max(exp_rtt_state, config.min_retransmit_timeout());
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

TEST_F(SwiftTestGen1, ProcessEackDropNoMaxDecrease) {
  using SwiftGen1Type =
      Swift<falcon_rue::Event_GEN1, falcon_rue::Response_GEN1>;
  // Tests the case of EACK drop where the max congestion window decrease is not
  // performed because a decrease has occurred too recently.
  auto config =
      MakeConfig<falcon_rue::Event_GEN1, falcon_rue::Response_GEN1>(100);
  config.set_max_decrease_on_eack_nack_drop(true);
  auto status_or_stateful_components =
      CreateStatefulSwiftAlgorithm<EventT, ResponseT>(config);
  ASSERT_OK(status_or_stateful_components);
  auto swift = std::move(status_or_stateful_components->first);
  auto state_manager = std::move(status_or_stateful_components->second);

  const auto new_now = 100200;

  this->event_.connection_id = 1234;
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
  this->event_.inter_packet_gap = 0;
  this->event_.retransmit_timeout = 1000;
  this->event_.num_packets_acked = 3;
  this->event_.event_queue_select = 0;
  this->event_.delay_select = falcon::DelaySelect::kForward;
  this->event_.fabric_window_time_marker = 100100;
  this->event_.base_delay = SwiftGen1Type::MakeBaseDelayField(
      /*profile_index=*/kSwiftDefaultProfileIndex);
  this->event_.delay_state = 150;
  this->event_.rtt_state = 1000;
  this->event_.cc_opaque = 3;
  this->event_.gen_bit = 0;

  swift->Process(this->event_, this->response_, new_now);

  EXPECT_EQ(this->response_.connection_id, this->event_.connection_id);
  EXPECT_EQ(this->response_.randomize_path, false);
  EXPECT_EQ(this->response_.cc_metadata, 0);
  EXPECT_EQ(this->event_.fabric_congestion_window,
            this->response_.fabric_congestion_window);
  EXPECT_EQ(this->response_.inter_packet_gap, 0);
  const auto rtt = this->event_.timestamp_4 - this->event_.timestamp_1;
  uint32_t exp_rtt_state =
      this->event_.rtt_state * config.rtt_smoothing_alpha() +
      rtt * (1 - config.rtt_smoothing_alpha());
  uint32_t exp_rto = std::max(exp_rtt_state, config.min_retransmit_timeout());
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

}  // namespace
}  // namespace rue
}  // namespace isekai
