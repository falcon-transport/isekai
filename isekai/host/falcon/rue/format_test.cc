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

#include "isekai/host/falcon/rue/format.h"

#include "gtest/gtest.h"
#include "isekai/host/falcon/falcon.h"

//

#if !defined(GEN_2)
TEST(format, SetResponse) {
  falcon_rue::Response resp;
  falcon_rue::SetResponse(
      /*connection_id=*/123,
      /*randomize_path=*/true,
      /*cc_metadata=*/456,
      /*fabric_congestion_window=*/7890,
      /*inter_packet_gap=*/135,
      /*nic_congestion_window=*/576,
      /*retransmit_timeout=*/246,
      /*fabric_window_time_marker=*/791,
      /*nic_window_time_marker=*/857,
      /*nic_window_direction=*/falcon::WindowDirection::kIncrease,
      /*event_queue_select=*/2,
      /*delay_select=*/falcon::DelaySelect::kReverse,
      /*base_delay=*/802,
      /*delay_state=*/132,
      /*rtt_state=*/243,
      /*cc_opaque=*/3,
      /*response=*/resp);
  EXPECT_EQ(resp.connection_id, 123);
  EXPECT_EQ(resp.randomize_path, true);
  EXPECT_EQ(resp.cc_metadata, 456);
  EXPECT_EQ(resp.fabric_congestion_window, 7890);
  EXPECT_EQ(resp.inter_packet_gap, 135);
  EXPECT_EQ(resp.nic_congestion_window, 576);
  EXPECT_EQ(resp.retransmit_timeout, 246);
  EXPECT_EQ(resp.fabric_window_time_marker, 791);
  EXPECT_EQ(resp.nic_window_time_marker, 857);
  EXPECT_EQ(resp.nic_window_direction, falcon::WindowDirection::kIncrease);
  EXPECT_EQ(resp.event_queue_select, 2);
  EXPECT_EQ(resp.delay_select, falcon::DelaySelect::kReverse);
  EXPECT_EQ(resp.base_delay, 802);
  EXPECT_EQ(resp.delay_state, 132);
  EXPECT_EQ(resp.rtt_state, 243);
  EXPECT_EQ(resp.cc_opaque, 3);
}
#endif
