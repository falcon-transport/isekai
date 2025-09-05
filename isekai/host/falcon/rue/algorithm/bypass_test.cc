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

#include <cmath>
#include <cstdint>
#include <memory>
#include <tuple>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "isekai/common/testing.h"
#include "isekai/host/falcon/falcon.h"
#include "isekai/host/falcon/rue/algorithm/bypass.pb.h"
#include "isekai/host/falcon/rue/format_gen1.h"
#include "isekai/host/falcon/rue/format_gen2.h"

template <typename EventT, typename ResponseT>
class GenericBypassTest : public testing::Test {
 public:
  GenericBypassTest() {
    memset(&event_, 0, sizeof(event_));
    memset(&response_, 0, sizeof(response_));
  }

  ~GenericBypassTest() override = default;
  EventT event_;
  ResponseT response_;
};

template <typename TypeParam>
class Gen1BypassTest : public GenericBypassTest<
                           typename std::tuple_element<0, TypeParam>::type,
                           typename std::tuple_element<1, TypeParam>::type> {};

using Event_Response_Types = ::testing::Types<
    std::tuple<falcon_rue::Event_GEN1, falcon_rue::Response_GEN1>>;

TYPED_TEST_SUITE(Gen1BypassTest, Event_Response_Types);

TYPED_TEST(Gen1BypassTest, NoCongestionControl) {
  typedef typename std::tuple_element<0, TypeParam>::type EventT;
  typedef typename std::tuple_element<1, TypeParam>::type ResponseT;
  using BypassTyped = isekai::rue::Bypass<EventT, ResponseT>;

  ASSERT_OK_AND_ASSIGN(std::unique_ptr<BypassTyped> bypass,
                       BypassTyped::Create(isekai::rue::BypassConfiguration()));
  int now = 100400;

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
  this->event_.fabric_congestion_window = 8642;
  this->event_.inter_packet_gap = 0;
  this->event_.retransmit_timeout = 1000;
  this->event_.num_packets_acked = 3;
  this->event_.event_queue_select = 0;
  this->event_.delay_select = falcon::DelaySelect::kForward;
  this->event_.fabric_window_time_marker = 99999;
  this->event_.base_delay = 100;
  this->event_.delay_state = 150;
  this->event_.rtt_state = 310;
  this->event_.cc_opaque = 3;
  this->event_.gen_bit = 0;

  bypass->Process(this->event_, this->response_, now);

  EXPECT_EQ(this->response_.connection_id, 1234);
  EXPECT_EQ(this->response_.randomize_path, false);
  EXPECT_EQ(this->response_.cc_metadata, 0);
  EXPECT_EQ(this->response_.fabric_congestion_window, 8642);
  EXPECT_EQ(this->response_.inter_packet_gap, 0);
  EXPECT_EQ(this->response_.retransmit_timeout, 1000);
  EXPECT_EQ(this->response_.fabric_window_time_marker, 0);
  EXPECT_EQ(this->response_.event_queue_select, 0);
  EXPECT_EQ(this->response_.delay_select, falcon::DelaySelect::kForward);
  EXPECT_EQ(this->response_.base_delay, 100);
  EXPECT_EQ(this->response_.delay_state, 150);
  EXPECT_EQ(this->response_.rtt_state, 310);
  EXPECT_EQ(this->response_.cc_opaque, 3);
}

class Gen2BypassTest : public GenericBypassTest<falcon_rue::Event_Gen2,
                                                falcon_rue::Response_Gen2> {};

TEST_F(Gen2BypassTest, NoCongestionControl) {
  using BypassTyped =
      isekai::rue::Bypass<falcon_rue::Event_Gen2, falcon_rue::Response_Gen2>;
  ASSERT_OK_AND_ASSIGN(std::unique_ptr<BypassTyped> bypass,
                       BypassTyped::Create(isekai::rue::BypassConfiguration()));
  int now = 100400;

  this->event_.connection_id = 1234;
  this->event_.multipath_enable = true;
  this->event_.flow_label = 0x12;
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
  this->event_.fabric_congestion_window = 8642;
  this->event_.num_packets_acked = 3;
  this->event_.event_queue_select = 0;
  this->event_.delay_select = falcon::DelaySelect::kForward;
  this->event_.fabric_window_time_marker = 99999;
  this->event_.base_delay = 100;
  this->event_.delay_state = 150;
  this->event_.rtt_state = 310;
  this->event_.cc_opaque = 3;
  this->event_.gen_bit = 0;

  bypass->Process(this->event_, this->response_, now);

  EXPECT_EQ(this->response_.connection_id, 1234);
  EXPECT_EQ(this->response_.cc_metadata, 0);
  EXPECT_EQ(this->response_.fabric_congestion_window, 8642);
  EXPECT_EQ(this->response_.fabric_window_time_marker, 0);
  EXPECT_EQ(this->response_.event_queue_select, 0);
  EXPECT_EQ(this->response_.delay_select, falcon::DelaySelect::kForward);
  EXPECT_EQ(this->response_.base_delay, 100);
  EXPECT_EQ(this->response_.delay_state, 150);
  EXPECT_EQ(this->response_.rtt_state, 310);
  EXPECT_EQ(this->response_.cc_opaque, 3);
  EXPECT_EQ(this->response_.flow_id, 2);
  EXPECT_EQ(this->response_.flow_label_1_weight, 1);
  EXPECT_EQ(this->response_.flow_label_2_weight, 1);
  EXPECT_EQ(this->response_.flow_label_3_weight, 1);
  EXPECT_EQ(this->response_.flow_label_4_weight, 1);
  EXPECT_EQ(this->response_.flow_label_1_valid, false);
  EXPECT_EQ(this->response_.flow_label_2_valid, false);
  EXPECT_EQ(this->response_.flow_label_3_valid, false);
  EXPECT_EQ(this->response_.flow_label_4_valid, false);
  constexpr double kFalconUnitTimeUs = 0.131072;
  const uint32_t k1ms = std::round(1000 / kFalconUnitTimeUs);  // ~1ms
  EXPECT_EQ(this->response_.retransmit_timeout, k1ms);

  // Test an event for a non-multipath connection.
  this->event_.multipath_enable = false;
  bypass->Process(this->event_, this->response_, now);
  EXPECT_EQ(this->response_.flow_id, 0);
}

TEST_F(Gen2BypassTest, OverrideFlowWeights) {
  using BypassTyped =
      isekai::rue::Bypass<falcon_rue::Event_Gen2, falcon_rue::Response_Gen2>;
  isekai::rue::BypassConfiguration config;
  config.mutable_gen2_test_only()->set_override_flow_weight_flow1(1);
  config.mutable_gen2_test_only()->set_override_flow_weight_flow2(2);
  config.mutable_gen2_test_only()->set_override_flow_weight_flow3(3);
  config.mutable_gen2_test_only()->set_override_flow_weight_flow4(4);

  ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<BypassTyped> bypass,
      BypassTyped::Create(isekai::rue::BypassConfiguration(config)));
  int now = 100400;

  event_.connection_id = 1234;
  event_.multipath_enable = true;
  event_.flow_label = 0x12;
  event_.event_type = falcon::RueEventType::kAck;
  event_.timestamp_1 = 100000;
  event_.timestamp_2 = 100153;
  event_.timestamp_3 = 100200;
  event_.timestamp_4 = 100315;
  event_.retransmit_count = 0;
  event_.retransmit_reason = falcon::RetransmitReason::kTimeout;
  event_.nack_code = falcon::NackCode::kNotANack;
  event_.forward_hops = 5;
  event_.cc_metadata = 0;
  event_.fabric_congestion_window = 8642;
  event_.num_packets_acked = 3;
  event_.event_queue_select = 0;
  event_.delay_select = falcon::DelaySelect::kForward;
  event_.fabric_window_time_marker = 99999;
  event_.base_delay = 100;
  event_.delay_state = 150;
  event_.rtt_state = 310;
  event_.cc_opaque = 3;
  event_.gen_bit = 0;

  bypass->Process(event_, response_, now);

  EXPECT_EQ(response_.connection_id, 1234);
  EXPECT_EQ(response_.cc_metadata, 0);
  EXPECT_EQ(response_.fabric_congestion_window, 8642);
  EXPECT_EQ(response_.fabric_window_time_marker, 0);
  EXPECT_EQ(response_.event_queue_select, 0);
  EXPECT_EQ(response_.delay_select, falcon::DelaySelect::kForward);
  EXPECT_EQ(response_.base_delay, 100);
  EXPECT_EQ(response_.delay_state, 150);
  EXPECT_EQ(response_.rtt_state, 310);
  EXPECT_EQ(response_.cc_opaque, 3);
  EXPECT_EQ(response_.flow_id, 2);
  EXPECT_EQ(response_.flow_label_1_weight, 1);
  EXPECT_EQ(response_.flow_label_2_weight, 2);
  EXPECT_EQ(response_.flow_label_3_weight, 3);
  EXPECT_EQ(response_.flow_label_4_weight, 4);
  EXPECT_EQ(response_.flow_label_1_valid, false);
  EXPECT_EQ(response_.flow_label_2_valid, false);
  EXPECT_EQ(response_.flow_label_3_valid, false);
  EXPECT_EQ(response_.flow_label_4_valid, false);
  constexpr double kFalconUnitTimeUs = 0.131072;
  const uint32_t k1ms = std::round(1000 / kFalconUnitTimeUs);  // ~1ms
  EXPECT_EQ(response_.retransmit_timeout, k1ms);
}

// With invalid flow weights, the flow weights should be set to the default
// flow weights.
TEST_F(Gen2BypassTest, InvalidFlowWeights) {
  using BypassTyped =
      isekai::rue::Bypass<falcon_rue::Event_Gen2, falcon_rue::Response_Gen2>;
  isekai::rue::BypassConfiguration config;
  // Invalid flow weights for flow 1 and flow 4.
  config.mutable_gen2_test_only()->set_override_flow_weight_flow1(103);
  config.mutable_gen2_test_only()->set_override_flow_weight_flow2(2);
  config.mutable_gen2_test_only()->set_override_flow_weight_flow3(3);
  config.mutable_gen2_test_only()->set_override_flow_weight_flow4(104);

  ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<BypassTyped> bypass,
      BypassTyped::Create(isekai::rue::BypassConfiguration(config)));
  int now = 100400;

  event_.connection_id = 1234;
  event_.multipath_enable = true;
  event_.flow_label = 0x12;
  event_.event_type = falcon::RueEventType::kAck;
  event_.timestamp_1 = 100000;
  event_.timestamp_2 = 100153;
  event_.timestamp_3 = 100200;
  event_.timestamp_4 = 100315;
  event_.retransmit_count = 0;
  event_.retransmit_reason = falcon::RetransmitReason::kTimeout;
  event_.nack_code = falcon::NackCode::kNotANack;
  event_.forward_hops = 5;
  event_.cc_metadata = 0;
  event_.fabric_congestion_window = 8642;
  event_.num_packets_acked = 3;
  event_.event_queue_select = 0;
  event_.delay_select = falcon::DelaySelect::kForward;
  event_.fabric_window_time_marker = 99999;
  event_.base_delay = 100;
  event_.delay_state = 150;
  event_.rtt_state = 310;
  event_.cc_opaque = 3;
  event_.gen_bit = 0;

  bypass->Process(event_, response_, now);

  EXPECT_EQ(response_.connection_id, 1234);
  EXPECT_EQ(response_.cc_metadata, 0);
  EXPECT_EQ(response_.fabric_congestion_window, 8642);
  EXPECT_EQ(response_.fabric_window_time_marker, 0);
  EXPECT_EQ(response_.event_queue_select, 0);
  EXPECT_EQ(response_.delay_select, falcon::DelaySelect::kForward);
  EXPECT_EQ(response_.base_delay, 100);
  EXPECT_EQ(response_.delay_state, 150);
  EXPECT_EQ(response_.rtt_state, 310);
  EXPECT_EQ(response_.cc_opaque, 3);
  EXPECT_EQ(response_.flow_id, 2);
  EXPECT_EQ(response_.flow_label_1_weight,
            isekai::rue::kBypassDefaultFlowWeight);
  EXPECT_EQ(response_.flow_label_2_weight, 2);
  EXPECT_EQ(response_.flow_label_3_weight, 3);
  EXPECT_EQ(response_.flow_label_4_weight,
            isekai::rue::kBypassDefaultFlowWeight);
  EXPECT_EQ(response_.flow_label_1_valid, false);
  EXPECT_EQ(response_.flow_label_2_valid, false);
  EXPECT_EQ(response_.flow_label_3_valid, false);
  EXPECT_EQ(response_.flow_label_4_valid, false);
  constexpr double kFalconUnitTimeUs = 0.131072;
  const uint32_t k1ms = std::round(1000 / kFalconUnitTimeUs);  // ~1ms
  EXPECT_EQ(response_.retransmit_timeout, k1ms);
}

TEST_F(Gen2BypassTest, AlwaysRepathFlow1AndFlow3) {
  using BypassTyped =
      isekai::rue::Bypass<falcon_rue::Event_Gen2, falcon_rue::Response_Gen2>;
  isekai::rue::BypassConfiguration config;
  config.mutable_gen2_test_only()->set_override_flow_weight_flow1(1);
  config.mutable_gen2_test_only()->set_override_flow_weight_flow2(2);
  config.mutable_gen2_test_only()->set_override_flow_weight_flow3(3);
  config.mutable_gen2_test_only()->set_override_flow_weight_flow4(4);
  config.mutable_gen2_test_only()->set_always_repath_flow1(true);
  config.mutable_gen2_test_only()->set_always_repath_flow3(true);

  ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<BypassTyped> bypass,
      BypassTyped::Create(isekai::rue::BypassConfiguration(config)));
  int now = 100400;

  event_.connection_id = 1234;
  event_.multipath_enable = true;
  event_.flow_label = 0x12;
  event_.event_type = falcon::RueEventType::kAck;
  event_.timestamp_1 = 100000;
  event_.timestamp_2 = 100153;
  event_.timestamp_3 = 100200;
  event_.timestamp_4 = 100315;
  event_.retransmit_count = 0;
  event_.retransmit_reason = falcon::RetransmitReason::kTimeout;
  event_.nack_code = falcon::NackCode::kNotANack;
  event_.forward_hops = 5;
  event_.cc_metadata = 0;
  event_.fabric_congestion_window = 8642;
  event_.num_packets_acked = 3;
  event_.event_queue_select = 0;
  event_.delay_select = falcon::DelaySelect::kForward;
  event_.fabric_window_time_marker = 99999;
  event_.base_delay = 100;
  event_.delay_state = 150;
  event_.rtt_state = 310;
  event_.cc_opaque = 3;
  event_.gen_bit = 0;

  bypass->Process(event_, response_, now);

  EXPECT_EQ(response_.connection_id, 1234);
  EXPECT_EQ(response_.cc_metadata, 0);
  EXPECT_EQ(response_.fabric_congestion_window, 8642);
  EXPECT_EQ(response_.fabric_window_time_marker, 0);
  EXPECT_EQ(response_.event_queue_select, 0);
  EXPECT_EQ(response_.delay_select, falcon::DelaySelect::kForward);
  EXPECT_EQ(response_.base_delay, 100);
  EXPECT_EQ(response_.delay_state, 150);
  EXPECT_EQ(response_.rtt_state, 310);
  EXPECT_EQ(response_.cc_opaque, 3);
  EXPECT_EQ(response_.flow_id, 2);
  EXPECT_EQ(response_.flow_label_1_weight, 1);
  EXPECT_EQ(response_.flow_label_2_weight, 2);
  EXPECT_EQ(response_.flow_label_3_weight, 3);
  EXPECT_EQ(response_.flow_label_4_weight, 4);
  EXPECT_EQ(response_.flow_label_1_valid, true);
  EXPECT_EQ(response_.flow_label_2_valid, false);
  EXPECT_EQ(response_.flow_label_3_valid, true);
  EXPECT_EQ(response_.flow_label_4_valid, false);
  EXPECT_NE(response_.flow_label_1, 0);
  EXPECT_EQ(response_.flow_label_2, 0);
  EXPECT_NE(response_.flow_label_3, 0);
  EXPECT_EQ(response_.flow_label_4, 0);
  constexpr double kFalconUnitTimeUs = 0.131072;
  const uint32_t k1ms = std::round(1000 / kFalconUnitTimeUs);  // ~1ms
  EXPECT_EQ(response_.retransmit_timeout, k1ms);
}

TEST_F(Gen2BypassTest, AlwaysWrrRestart) {
  using BypassTyped =
      isekai::rue::Bypass<falcon_rue::Event_Gen2, falcon_rue::Response_Gen2>;
  isekai::rue::BypassConfiguration config;
  config.mutable_gen2_test_only()->set_always_wrr_restart(true);

  ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<BypassTyped> bypass,
      BypassTyped::Create(isekai::rue::BypassConfiguration(config)));
  int now = 100400;

  event_.connection_id = 1234;
  event_.multipath_enable = true;
  event_.flow_label = 0x12;
  event_.event_type = falcon::RueEventType::kAck;
  event_.timestamp_1 = 100000;
  event_.timestamp_2 = 100153;
  event_.timestamp_3 = 100200;
  event_.timestamp_4 = 100315;
  event_.retransmit_count = 0;
  event_.retransmit_reason = falcon::RetransmitReason::kTimeout;
  event_.nack_code = falcon::NackCode::kNotANack;
  event_.forward_hops = 5;
  event_.cc_metadata = 0;
  event_.fabric_congestion_window = 8642;
  event_.num_packets_acked = 3;
  event_.event_queue_select = 0;
  event_.delay_select = falcon::DelaySelect::kForward;
  event_.fabric_window_time_marker = 99999;
  event_.base_delay = 100;
  event_.delay_state = 150;
  event_.rtt_state = 310;
  event_.cc_opaque = 3;
  event_.gen_bit = 0;

  bypass->Process(event_, response_, now);

  EXPECT_TRUE(response_.wrr_restart_round);
}
