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

#include "isekai/host/falcon/gen2/rate_update_engine.h"

#include <cstdint>
#include <memory>
#include <string>

#include "absl/time/time.h"
#include "gtest/gtest.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/default_config_generator.h"
#include "isekai/common/packet.h"
#include "isekai/common/simple_environment.h"
#include "isekai/host/falcon/falcon.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/falcon_connection_state.h"
#include "isekai/host/falcon/falcon_counters.h"
#include "isekai/host/falcon/falcon_testing_helpers.h"
#include "isekai/host/falcon/gen1/falcon_model.h"
#include "isekai/host/falcon/gen1/falcon_types.h"
#include "isekai/host/falcon/gen1/rate_update_engine.h"
#include "isekai/host/falcon/gen2/falcon_types.h"
#include "isekai/host/falcon/gen2/falcon_utils.h"
#include "isekai/host/falcon/rue/bits.h"
#include "isekai/host/falcon/rue/fixed.h"
#include "isekai/host/falcon/rue/format_gen2.h"
#include "isekai/host/rnic/connection_manager.h"

namespace isekai {

namespace {

constexpr int kFalconVersion = 2;
constexpr uint8_t kNumMultipathFlows = 4;
constexpr uint32_t kCidSinglePath = 123;
constexpr uint32_t kCidMultiPath = 153;

// This defines all the objects needed for setup and testing
class Gen2RateUpdateEngineTest : public FalconTestingHelpers::FalconTestSetup,
                                 public testing::Test {
 protected:
  void SetUp() override {
    FalconConfig config =
        DefaultConfigGenerator::DefaultFalconConfig(kFalconVersion);
    InitFalcon(config);
    rue_ = dynamic_cast<ProtocolRateUpdateEngine*>(
        falcon_->get_rate_update_engine());
    peer_.Set(rue_);

    auto metadata_single_path =
        FalconTestingHelpers::InitializeConnectionMetadata(falcon_.get(),
                                                           kCidSinglePath);
    connection_state_single_path_ =
        FalconTestingHelpers::InitializeConnectionState(
            falcon_.get(), std::move(metadata_single_path));

    auto metadata_multipath =
        FalconTestingHelpers::InitializeConnectionMetadata(falcon_.get(),
                                                           kCidMultiPath);
    auto gen2_multipath_metadata =
        dynamic_cast<Gen2ConnectionMetadata*>(metadata_multipath.get());
    gen2_multipath_metadata->degree_of_multipathing = kNumMultipathFlows;
    connection_state_multipath_ =
        FalconTestingHelpers::InitializeConnectionState(
            falcon_.get(), std::move(metadata_multipath));
  }
  ConnectionState* connection_state_single_path_;
  ConnectionState* connection_state_multipath_;

  ProtocolRateUpdateEngine* rue_;
  ProtocolRateUpdateEngineTestPeer<falcon_rue::Event_Gen2,
                                   falcon_rue::Response_Gen2>
      peer_;
};

// Tests the initialization of the Gen2 fields in CongestionControlMetadata.
TEST_F(Gen2RateUpdateEngineTest, CongestionControlMetadataInitialization) {
  Gen2CongestionControlMetadata& ccmeta =
      CongestionControlMetadata::DowncastTo<Gen2CongestionControlMetadata>(
          *connection_state_multipath_->congestion_control_metadata);
  Gen2CongestionControlMetadata& ccmeta_single_path =
      CongestionControlMetadata::DowncastTo<Gen2CongestionControlMetadata>(
          *connection_state_single_path_->congestion_control_metadata);
  auto& flow_weights = ccmeta.gen2_flow_weights;
  // Check initial fcwnd for single path connections.
  uint32_t expected_initial_fcwnd = falcon_rue::FloatToFixed<double, uint32_t>(
      falcon_->get_config()->rue().initial_fcwnd(),
      falcon_rue::kFractionalBits);
  EXPECT_EQ(ccmeta_single_path.fabric_congestion_window,
            expected_initial_fcwnd);

  auto& flow_labels = ccmeta.gen2_flow_labels;
  auto& num_acked = ccmeta.gen2_num_acked;
  auto& last_rue_event_time = ccmeta.gen2_last_rue_event_time;
  auto& outstanding_rue_event = ccmeta.gen2_outstanding_rue_event;
  uint32_t num_flows = flow_weights.size();

  // Check ConnectionState::CongestionControlMetadata initialization is as
  // expected.
  EXPECT_EQ(ccmeta.gen2_plb_state, 0);
  EXPECT_EQ(flow_weights.size(), flow_labels.size());
  EXPECT_EQ(num_flows,
            GetGen2MultipathingNumFlows(
                connection_state_multipath_->connection_metadata.get()));
  // Check initial fcwnd for multipath connections.
  EXPECT_EQ(ccmeta.fabric_congestion_window, expected_initial_fcwnd);
  for (int idx = 0; idx < num_flows; idx++) {
    uint32_t mask = 3;  // Get only last 2 bits of flow label.
    EXPECT_EQ(flow_labels[idx] & mask, idx);
    EXPECT_EQ(flow_weights[idx], 1);
    EXPECT_EQ(num_acked[idx], 0);
    EXPECT_EQ(last_rue_event_time[idx], -absl::InfiniteDuration());
    EXPECT_EQ(outstanding_rue_event[idx], false);
  }
}

// Tests that rate control in Gen2 RUE is at the <connection, flow> control
// level.
TEST_F(Gen2RateUpdateEngineTest, RateControlIsAtConnectionFlowLevel) {
  // Tests expected initial conditions
  EXPECT_FALSE(peer_.IsEventQueueScheduled());
  EXPECT_EQ(peer_.GetNumEvents(), 0);
  EXPECT_EQ(peer_.GetNumResponses(), 0);
  EXPECT_EQ(env_.ScheduledEvents(), 0);

  // Insert one event for a <connection, flow> pair - all events should be
  // enqueued successfully.
  for (uint8_t flow_id = 0; flow_id < kNumMultipathFlows; flow_id++) {
    Packet explicit_packet;
    explicit_packet.packet_type = falcon::PacketType::kAck;
    explicit_packet.ack.dest_cid = kCidMultiPath;
    explicit_packet.metadata.flow_label = 0x10 + flow_id;
    peer_.IncrementNumAcked(&explicit_packet, 1);
    EXPECT_EQ(peer_.GetLastEventTime(kCidMultiPath, flow_id),
              -absl::InfiniteDuration());
    rue_->ExplicitAckReceived(&explicit_packet, false, false);
    uint8_t num_events = 1 + flow_id;
    EXPECT_EQ(peer_.GetNumEvents(), num_events);
    EXPECT_TRUE(peer_.IsConnectionOutstanding(kCidMultiPath, flow_id));
    EXPECT_EQ(peer_.GetLastEventTime(kCidMultiPath, flow_id),
              env_.ElapsedTime());
    // Num acked for the <connection, flow> pair is  reset because the ACK
    // event was successfully enqueued.
    EXPECT_EQ(peer_.GetAccumulatedAcks(kCidMultiPath, flow_id), 0);
    // Test RUE counters.
    FalconConnectionCounters host_counters =
        falcon_->get_stats_manager()->GetConnectionCounters(kCidMultiPath);
    EXPECT_EQ(host_counters.rue_enqueue_attempts, num_events);
    EXPECT_EQ(host_counters.rue_ack_events, num_events);
    // No dropped ACK events.
    EXPECT_EQ(host_counters.rue_event_drops_ack, 0);
  }

  // Try to insert another event for the same <connection, flow> pair which
  // already have an outstanding event from the loop above - all events should
  // not be enqueued.
  absl::Duration last_successful_enqueue = env_.ElapsedTime();
  env_.RunFor(absl::Nanoseconds(1));  // Smaller than the duration from event
                                      // processing to response handling.
  EXPECT_EQ(env_.ElapsedTime(), last_successful_enqueue + absl::Nanoseconds(1));
  for (uint8_t flow_id = 0; flow_id < kNumMultipathFlows; flow_id++) {
    Packet explicit_packet;
    explicit_packet.packet_type = falcon::PacketType::kAck;
    explicit_packet.ack.dest_cid = kCidMultiPath;
    explicit_packet.metadata.flow_label = 0x10 + flow_id;
    peer_.IncrementNumAcked(&explicit_packet, 1);
    EXPECT_EQ(peer_.GetLastEventTime(kCidMultiPath, flow_id),
              last_successful_enqueue);
    rue_->ExplicitAckReceived(&explicit_packet, false, false);
    EXPECT_TRUE(peer_.IsConnectionOutstanding(kCidMultiPath, flow_id));
    // Num acked for the <connection, flow> pair is not reset because the ACK
    // event was not enqueued.
    EXPECT_EQ(peer_.GetAccumulatedAcks(kCidMultiPath, flow_id), 1);
    EXPECT_EQ(peer_.GetLastEventTime(kCidMultiPath, flow_id),
              last_successful_enqueue);
    // Test RUE counters.
    FalconConnectionCounters host_counters =
        falcon_->get_stats_manager()->GetConnectionCounters(kCidMultiPath);
    EXPECT_EQ(host_counters.rue_enqueue_attempts,
              1 + kNumMultipathFlows + flow_id);
    EXPECT_EQ(host_counters.rue_ack_events, kNumMultipathFlows);
    //  1 + flow_id dropped ACK events.
    EXPECT_EQ(host_counters.rue_event_drops_ack, 1 + flow_id);
  }
}

}  // namespace

}  // namespace isekai
