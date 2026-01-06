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

#include "isekai/host/falcon/event_response_format_adapter.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>

#include "absl/time/time.h"
#include "gtest/gtest.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/default_config_generator.h"
#include "isekai/common/packet.h"
#include "isekai/host/falcon/falcon.h"
#include "isekai/host/falcon/falcon_connection_state.h"
#include "isekai/host/falcon/falcon_testing_helpers.h"
#include "isekai/host/falcon/gen1/falcon_types.h"
#include "isekai/host/falcon/gen2/connection_state.h"
#include "isekai/host/falcon/gen2/falcon_types.h"
#include "isekai/host/falcon/gen3/falcon_types.h"
#include "isekai/host/falcon/rue/algorithm/swift.h"
#include "isekai/host/falcon/rue/bits.h"
#include "isekai/host/falcon/rue/fixed.h"
#include "isekai/host/falcon/rue/format.h"
#include "isekai/host/falcon/rue/format_gen3.h"

namespace isekai {
namespace {

constexpr int kFalconVersion3 = 3;
constexpr uint32_t kCid1 = 123;
constexpr uint32_t kCid2 = 234;
constexpr uint8_t kFlowId0 = 0;
constexpr uint8_t kFlowId1 = 1;
constexpr uint32_t kFlowLabel0 = 0x120 | kFlowId0;
constexpr uint32_t kFlowLabel1 = 0x120 | kFlowId1;

void PopulateNackPacket(Packet* packet) {
  packet->nack.code = falcon::NackCode::kRxResourceExhaustion;
  packet->nack.timestamp_1 = absl::Microseconds(1);
  packet->nack.timestamp_2 = absl::Microseconds(2);
  packet->timestamps.sent_timestamp = absl::Microseconds(3);
  packet->timestamps.received_timestamp = absl::Microseconds(4);
  packet->nack.forward_hops = 1;
  packet->nack.rx_buffer_level = 3;
  packet->nack.cc_metadata = 2;
}

void PopulateAckPacket(Packet* packet) {
  packet->ack.timestamp_1 = absl::Microseconds(1);
  packet->ack.timestamp_2 = absl::Microseconds(2);
  packet->timestamps.sent_timestamp = absl::Microseconds(3);
  packet->timestamps.received_timestamp = absl::Microseconds(4);
  packet->ack.forward_hops = 1;
  packet->ack.rx_buffer_level = 3;
  packet->ack.cc_metadata = 2;
}

// Test fixture for the Gen3 EventResponseFormatAdapter class.
class Gen3FormatAdapterTest : public FalconTestingHelpers::FalconTestSetup,
                              public ::testing::Test {
 protected:
  void SetUp() override {
    FalconConfig config =
        DefaultConfigGenerator::DefaultFalconConfig(kFalconVersion3);
    InitFalcon(config);
    auto single_path_metadata =
        FalconTestingHelpers::InitializeConnectionMetadata(falcon_.get(),
                                                           kCid1);
    connection_state_single_path_ = dynamic_cast<Gen2ConnectionState*>(
        FalconTestingHelpers::InitializeConnectionState(
            falcon_.get(), std::move(single_path_metadata)));
    auto multipath_metadata =
        FalconTestingHelpers::InitializeConnectionMetadata(falcon_.get(),
                                                           kCid1);
    auto gen2_multipath_metadata =
        dynamic_cast<Gen2ConnectionMetadata*>(multipath_metadata.get());
    // Multipath connection with degree_of_multipathing = 4.
    gen2_multipath_metadata->degree_of_multipathing = 4;
    gen2_multipath_metadata->scid = kCid2;
    connection_state_multipath_ = dynamic_cast<Gen2ConnectionState*>(
        FalconTestingHelpers::InitializeConnectionState(
            falcon_.get(), std::move(multipath_metadata)));
    format_adapter_ = std::make_unique<EventResponseFormatAdapter<
        falcon_rue::EVENT_Gen3, falcon_rue::Response_Gen3>>(falcon_.get());
  }

  std::unique_ptr<EventResponseFormatAdapter<falcon_rue::EVENT_Gen3,
                                             falcon_rue::Response_Gen3>>
      format_adapter_;
  Gen2ConnectionState* connection_state_multipath_;
  Gen2ConnectionState* connection_state_single_path_;
};

// Tests that the RTO event is populated properly.
TEST_F(Gen3FormatAdapterTest, FillTimeoutRetransmittedEvent) {
  falcon_rue::EVENT_Gen3 event;
  memset(&event, 0, sizeof(falcon_rue::EVENT_Gen3));
  Gen2RueKey rue_key(kCid1, kFlowId1);
  Packet packet;
  packet.metadata.flow_label = kFlowLabel1;

  Gen3CongestionControlMetadata& ccmeta =
      CongestionControlMetadata::DowncastTo<Gen3CongestionControlMetadata>(
          *connection_state_multipath_->congestion_control_metadata);
  format_adapter_->FillTimeoutRetransmittedEvent(event, &rue_key, &packet,
                                                 ccmeta, 4);

  EXPECT_EQ(event.connection_id, rue_key.scid);
  EXPECT_EQ(event.flow_label, packet.metadata.flow_label);
  EXPECT_EQ(event.multipath_enable, true);
  EXPECT_EQ(event.event_type, falcon::RueEventType::kRetransmit);
  EXPECT_EQ(event.retransmit_count, 4);
  EXPECT_EQ(event.retransmit_reason, falcon::RetransmitReason::kTimeout);
  EXPECT_EQ(event.nack_code, falcon::NackCode::kNotANack);
  EXPECT_EQ(event.timestamp_1, 0);
  EXPECT_EQ(event.timestamp_2, 0);
  EXPECT_EQ(event.timestamp_3, 0);
  EXPECT_EQ(event.timestamp_4, 0);
  EXPECT_EQ(event.forward_hops, 0);
  EXPECT_EQ(event.rx_buffer_level, 0);
  EXPECT_EQ(event.cc_metadata, 0);
  EXPECT_EQ(event.num_packets_acked, 0);
  EXPECT_EQ(event.event_queue_select, 0);
  EXPECT_EQ(event.fabric_congestion_window, ccmeta.gen3_flow_fcwnds[kFlowId1]);
  EXPECT_EQ(event.nic_congestion_window,
            (falcon_rue::UintToFixed<uint32_t, uint32_t>(
                ccmeta.nic_congestion_window, falcon_rue::kFractionalBits)));
  EXPECT_EQ(event.delay_select, ccmeta.delay_select);
  EXPECT_EQ(event.fabric_window_time_marker, ccmeta.fabric_window_time_marker);
  EXPECT_EQ(event.nic_window_time_marker, ccmeta.nic_window_time_marker);
  EXPECT_EQ(event.base_delay, ccmeta.base_delay);
  EXPECT_EQ(event.delay_state, ccmeta.delay_state);
  EXPECT_EQ(event.rtt_state, ccmeta.rtt_state);
  EXPECT_EQ(event.rtt_state, ccmeta.gen3_flow_rtt_state[kFlowId1]);
  EXPECT_EQ(event.cc_opaque, ccmeta.cc_opaque);
  EXPECT_EQ(event.eack, false);
  EXPECT_EQ(event.eack_drop, false);
  EXPECT_EQ(event.eack_own, 0);
  EXPECT_EQ(event.reserved_0, 0);
  EXPECT_EQ(event.reserved_1, 0);
  EXPECT_EQ(event.gen_bit, 0);
  EXPECT_EQ(event.plb_state, ccmeta.gen2_plb_state);

  // Testing the FillTimeoutRetransmittedEvent() function with the single path
  // connection.
  rue_key = Gen2RueKey(
      connection_state_single_path_->connection_metadata->scid,
      kFlowId0);  // can only use kFlowId0 for single path connections.
  packet.metadata.flow_label = kFlowLabel0;
  Gen3CongestionControlMetadata& ccmeta_single_path =
      CongestionControlMetadata::DowncastTo<Gen3CongestionControlMetadata>(
          *connection_state_single_path_->congestion_control_metadata);
  format_adapter_->FillTimeoutRetransmittedEvent(event, &rue_key, &packet,
                                                 ccmeta_single_path, 4);

  EXPECT_EQ(event.connection_id, rue_key.scid);
  EXPECT_EQ(event.flow_label, packet.metadata.flow_label);
  EXPECT_EQ(event.multipath_enable, false);
}

// Tests that the NACK event is populated properly.
TEST_F(Gen3FormatAdapterTest, FillNackEvent) {
  falcon_rue::EVENT_Gen3 event;
  memset(&event, 0, sizeof(falcon_rue::EVENT_Gen3));
  Gen2RueKey rue_key(kCid1, kFlowId1);
  Packet packet;
  PopulateNackPacket(&packet);
  packet.metadata.flow_label = kFlowLabel1;

  Gen3CongestionControlMetadata& ccmeta =
      CongestionControlMetadata::DowncastTo<Gen3CongestionControlMetadata>(
          *connection_state_multipath_->congestion_control_metadata);
  format_adapter_->FillNackEvent(event, &rue_key, &packet, ccmeta, 4);

  EXPECT_EQ(event.connection_id, rue_key.scid);
  EXPECT_EQ(event.flow_label, packet.metadata.flow_label);
  EXPECT_EQ(event.multipath_enable, true);
  EXPECT_EQ(event.event_type, falcon::RueEventType::kNack);
  EXPECT_EQ(event.timestamp_1,
            falcon_->get_rate_update_engine()->ToFalconTimeUnits(
                packet.nack.timestamp_1));
  EXPECT_EQ(event.timestamp_2,
            falcon_->get_rate_update_engine()->ToFalconTimeUnits(
                packet.nack.timestamp_2));
  EXPECT_EQ(event.timestamp_3,
            falcon_->get_rate_update_engine()->ToFalconTimeUnits(
                packet.timestamps.sent_timestamp));
  EXPECT_EQ(event.timestamp_4,
            falcon_->get_rate_update_engine()->ToFalconTimeUnits(
                packet.timestamps.received_timestamp));
  EXPECT_EQ(event.retransmit_count, 0);
  EXPECT_EQ(event.retransmit_reason, falcon::RetransmitReason::kEarly);
  EXPECT_EQ(event.nack_code, packet.nack.code);
  EXPECT_EQ(event.forward_hops, packet.nack.forward_hops);
  EXPECT_EQ(event.rx_buffer_level, packet.nack.rx_buffer_level);
  EXPECT_EQ(event.cc_metadata, packet.nack.cc_metadata);
  EXPECT_EQ(event.num_packets_acked, 4);
  EXPECT_EQ(event.event_queue_select, 0);
  EXPECT_EQ(event.fabric_congestion_window, ccmeta.gen3_flow_fcwnds[kFlowId1]);
  EXPECT_EQ(event.nic_congestion_window,
            (falcon_rue::UintToFixed<uint32_t, uint32_t>(
                ccmeta.nic_congestion_window, falcon_rue::kFractionalBits)));
  EXPECT_EQ(event.delay_select, ccmeta.delay_select);
  EXPECT_EQ(event.fabric_window_time_marker, ccmeta.fabric_window_time_marker);
  EXPECT_EQ(event.nic_window_time_marker, ccmeta.nic_window_time_marker);
  EXPECT_EQ(event.base_delay, ccmeta.base_delay);
  EXPECT_EQ(event.delay_state, ccmeta.delay_state);
  EXPECT_EQ(event.rtt_state, ccmeta.rtt_state);
  EXPECT_EQ(event.rtt_state, ccmeta.gen3_flow_rtt_state[kFlowId1]);
  EXPECT_EQ(event.cc_opaque, ccmeta.cc_opaque);
  EXPECT_EQ(event.eack, false);
  EXPECT_EQ(event.eack_drop, false);
  EXPECT_EQ(event.eack_own, 0);
  EXPECT_EQ(event.reserved_0, 0);
  EXPECT_EQ(event.reserved_1, 0);
  EXPECT_EQ(event.gen_bit, 0);
  EXPECT_EQ(event.plb_state, ccmeta.gen2_plb_state);

  // Testing the FillNackEvent() function with the single path connection.
  rue_key = Gen2RueKey(
      connection_state_single_path_->connection_metadata->scid,
      kFlowId0);  // can only use kFlowId0 for single path connections.
  packet.metadata.flow_label = kFlowLabel0;
  Gen3CongestionControlMetadata& ccmeta_single_path =
      CongestionControlMetadata::DowncastTo<Gen3CongestionControlMetadata>(
          *connection_state_single_path_->congestion_control_metadata);
  format_adapter_->FillNackEvent(event, &rue_key, &packet, ccmeta_single_path,
                                 4);

  EXPECT_EQ(event.connection_id, rue_key.scid);
  EXPECT_EQ(event.flow_label, packet.metadata.flow_label);
  EXPECT_EQ(event.multipath_enable, false);
}

// Tests that the explicit ACK event is populated properly.
TEST_F(Gen3FormatAdapterTest, FillExplicitAckEvent) {
  falcon_rue::EVENT_Gen3 event;
  memset(&event, 0, sizeof(falcon_rue::EVENT_Gen3));
  Gen2RueKey rue_key(kCid1, kFlowId1);
  Packet packet;
  PopulateAckPacket(&packet);
  packet.metadata.flow_label = kFlowLabel1;
  Gen3CongestionControlMetadata& ccmeta =
      CongestionControlMetadata::DowncastTo<Gen3CongestionControlMetadata>(
          *connection_state_multipath_->congestion_control_metadata);

  format_adapter_->FillExplicitAckEvent(event, &rue_key, &packet, ccmeta, 4,
                                        true, false);

  EXPECT_EQ(event.connection_id, rue_key.scid);
  EXPECT_EQ(event.flow_label, packet.metadata.flow_label);
  EXPECT_EQ(event.multipath_enable, true);
  EXPECT_EQ(event.event_type, falcon::RueEventType::kAck);
  EXPECT_EQ(event.timestamp_1,
            falcon_->get_rate_update_engine()->ToFalconTimeUnits(
                packet.ack.timestamp_1));
  EXPECT_EQ(event.timestamp_2,
            falcon_->get_rate_update_engine()->ToFalconTimeUnits(
                packet.ack.timestamp_2));
  EXPECT_EQ(event.timestamp_3,
            falcon_->get_rate_update_engine()->ToFalconTimeUnits(
                packet.timestamps.sent_timestamp));
  EXPECT_EQ(event.timestamp_4,
            falcon_->get_rate_update_engine()->ToFalconTimeUnits(
                packet.timestamps.received_timestamp));
  EXPECT_EQ(event.retransmit_count, 0);
  EXPECT_EQ(event.retransmit_reason, falcon::RetransmitReason::kEarly);
  EXPECT_EQ(event.nack_code, falcon::NackCode::kNotANack);
  EXPECT_EQ(event.forward_hops, packet.ack.forward_hops);
  EXPECT_EQ(event.rx_buffer_level, packet.ack.rx_buffer_level);
  EXPECT_EQ(event.cc_metadata, packet.ack.cc_metadata);
  EXPECT_EQ(event.num_packets_acked, 4);
  EXPECT_EQ(event.event_queue_select, 0);
  EXPECT_EQ(event.fabric_congestion_window, ccmeta.gen3_flow_fcwnds[kFlowId1]);
  EXPECT_EQ(event.nic_congestion_window,
            (falcon_rue::UintToFixed<uint32_t, uint32_t>(
                ccmeta.nic_congestion_window, falcon_rue::kFractionalBits)));
  EXPECT_EQ(event.delay_select, ccmeta.delay_select);
  EXPECT_EQ(event.fabric_window_time_marker, ccmeta.fabric_window_time_marker);
  EXPECT_EQ(event.nic_window_time_marker, ccmeta.nic_window_time_marker);
  EXPECT_EQ(event.base_delay, ccmeta.base_delay);
  EXPECT_EQ(event.delay_state, ccmeta.delay_state);
  EXPECT_EQ(event.rtt_state, ccmeta.rtt_state);
  EXPECT_EQ(event.rtt_state, ccmeta.gen3_flow_rtt_state[kFlowId1]);
  EXPECT_EQ(event.cc_opaque, ccmeta.cc_opaque);
  EXPECT_EQ(event.eack, true);
  EXPECT_EQ(event.eack_drop, false);
  EXPECT_EQ(event.eack_own, 0);
  EXPECT_EQ(event.reserved_0, 0);
  EXPECT_EQ(event.reserved_1, 0);
  EXPECT_EQ(event.gen_bit, 0);
  EXPECT_EQ(event.plb_state, ccmeta.gen2_plb_state);

  // Testing the FillExplicitAckEvent() function with the single path
  // connection.
  rue_key = Gen2RueKey(
      connection_state_single_path_->connection_metadata->scid,
      kFlowId0);  // can only use kFlowId0 for single path connections.
  packet.metadata.flow_label = kFlowLabel0;
  Gen3CongestionControlMetadata& ccmeta_single_path =
      CongestionControlMetadata::DowncastTo<Gen3CongestionControlMetadata>(
          *connection_state_single_path_->congestion_control_metadata);
  format_adapter_->FillExplicitAckEvent(event, &rue_key, &packet,
                                        ccmeta_single_path, 4, true, false);

  EXPECT_EQ(event.connection_id, rue_key.scid);
  EXPECT_EQ(event.flow_label, packet.metadata.flow_label);
  EXPECT_EQ(event.multipath_enable, false);
}

// Tests the IsRandomizePath() function.
TEST_F(Gen3FormatAdapterTest, IsRandomizePath) {
  falcon_rue::Response_Gen3 response;
  memset(&response, 0, sizeof(falcon_rue::Response));
  EXPECT_EQ(format_adapter_->IsRandomizePath(&response), false);

  response.flow_label_1_valid = true;
  EXPECT_EQ(format_adapter_->IsRandomizePath(&response), true);

  memset(&response, 0, sizeof(falcon_rue::Response));
  response.flow_label_2_valid = true;
  EXPECT_EQ(format_adapter_->IsRandomizePath(&response), true);

  memset(&response, 0, sizeof(falcon_rue::Response));
  response.flow_label_3_valid = true;
  EXPECT_EQ(format_adapter_->IsRandomizePath(&response), true);

  memset(&response, 0, sizeof(falcon_rue::Response));
  response.flow_label_4_valid = true;
  EXPECT_EQ(format_adapter_->IsRandomizePath(&response), true);
}

// Tests that the connection Xoff metadata is updated as expected by
// the response.
TEST_F(Gen3FormatAdapterTest, UpdateConnectionStateFromResponseXoffMetadata) {
  falcon_rue::Response_Gen3 response;
  memset(&response, 0, sizeof(falcon_rue::Response));
  response.alpha_request = 1;
  response.alpha_response = 2;

  // Expect failure when fabric congestion window is zero.
  EXPECT_DEATH(format_adapter_->UpdateConnectionStateFromResponse(
                   connection_state_single_path_, &response),
               "");

  response.fabric_congestion_window = 12;
  format_adapter_->UpdateConnectionStateFromResponse(
      connection_state_single_path_, &response);
  EXPECT_EQ(
      connection_state_single_path_->connection_xoff_metadata.alpha_request, 1);
  EXPECT_EQ(
      connection_state_single_path_->connection_xoff_metadata.alpha_response,
      2);
}

// Tests that the congestion control metadata is updated as expected by the
// response for Gen3 multipath connections.
TEST_F(Gen3FormatAdapterTest, UpdateConnectionStateFromResponseMultipath) {
  falcon_rue::Response_Gen3 response;
  memset(&response, 0, sizeof(falcon_rue::Response));
  response.flow_label_1 = kFlowLabel0;
  response.flow_label_1_valid = true;
  response.rtt_state = 1000;
  response.retransmit_timeout = 1200;
  response.nic_congestion_window = 3;  // in fixed format
  response.plb_state = 10;

  // Expect failure when fabric congestion window is zero.
  EXPECT_DEATH(format_adapter_->UpdateConnectionStateFromResponse(
                   connection_state_multipath_, &response),
               "");

  Gen3CongestionControlMetadata& ccmeta =
      CongestionControlMetadata::DowncastTo<Gen3CongestionControlMetadata>(
          *connection_state_multipath_->congestion_control_metadata);
  int num_flows = ccmeta.gen2_flow_labels.size();
  double initial_fcwnd_per_flow = falcon_rue::FixedToFloat<uint32_t, double>(
      ccmeta.gen3_flow_fcwnds[0], falcon_rue::kFractionalBits);

  // Increase the window size of flow 0 by 4
  response.fabric_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(4 * initial_fcwnd_per_flow,
                                                 falcon_rue::kFractionalBits);

  format_adapter_->UpdateConnectionStateFromResponse(
      connection_state_multipath_, &response);
  EXPECT_EQ(ccmeta.gen2_flow_labels[0], kFlowLabel0);
  EXPECT_EQ(ccmeta.retransmit_timeout,
            falcon_->get_rate_update_engine()->FromFalconTimeUnits(1200));
  EXPECT_EQ(ccmeta.rtt_state, 1000);
  EXPECT_EQ(ccmeta.gen3_flow_rtt_state[0], 1000);
  EXPECT_EQ(ccmeta.gen2_plb_state, 10);
  EXPECT_EQ(ccmeta.nic_congestion_window,
            std::max<uint32_t>(1, falcon_rue::FixedToUint<uint32_t, uint32_t>(
                                      response.nic_congestion_window,
                                      falcon_rue::kFractionalBits)));

  // Flow 0's fcwnd should be updated.
  uint32_t expected_fcwnd = falcon_rue::FloatToFixed<double, uint32_t>(
      4 * initial_fcwnd_per_flow, falcon_rue::kFractionalBits);
  EXPECT_EQ(ccmeta.gen3_flow_fcwnds[0], expected_fcwnd);

  expected_fcwnd = falcon_rue::FloatToFixed<double, uint32_t>(
      initial_fcwnd_per_flow, falcon_rue::kFractionalBits);
  for (int i = 1; i < num_flows; ++i) {
    EXPECT_EQ(ccmeta.gen3_flow_fcwnds[i], expected_fcwnd);
  }

  // flow0's weight should be four times larger of other flows.
  uint8_t flow0_weight = ccmeta.gen2_flow_weights[0];

  for (int i = 1; i < num_flows; ++i) {
    EXPECT_EQ(4 * ccmeta.gen2_flow_weights[i] + 1, flow0_weight);
  }

  // Set All fcwnd to 1.
  response.flow_label_1 = 0;
  response.flow_label_1_valid = false;

  response.fabric_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(1.0,
                                                 falcon_rue::kFractionalBits);
  format_adapter_->UpdateConnectionStateFromResponse(
      connection_state_multipath_, &response);

  response.flow_id = 1;
  response.fabric_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(1.0,
                                                 falcon_rue::kFractionalBits);
  format_adapter_->UpdateConnectionStateFromResponse(
      connection_state_multipath_, &response);

  response.flow_id = 2;
  response.fabric_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(1.0,
                                                 falcon_rue::kFractionalBits);
  format_adapter_->UpdateConnectionStateFromResponse(
      connection_state_multipath_, &response);

  response.flow_id = 3;
  response.fabric_congestion_window =
      falcon_rue::FloatToFixed<double, uint32_t>(1.0,
                                                 falcon_rue::kFractionalBits);
  format_adapter_->UpdateConnectionStateFromResponse(
      connection_state_multipath_, &response);

  // All weights should be the same.
  for (int i = 1; i < num_flows; ++i) {
    EXPECT_EQ(ccmeta.gen2_flow_weights[i],
              (isekai::rue::kMaxFlowWeight + 1) / num_flows);
  }

  // Check whether window sizes are valid
  double conn_fcwnd = falcon_rue::FixedToFloat<uint32_t, double>(
      ccmeta.fabric_congestion_window, falcon_rue::kFractionalBits);
  EXPECT_LE(conn_fcwnd, falcon_->get_config()->rue().swift().max_fcwnd());
}

// Tests that the congestion control metadata is updated as expected by the
// response for Gen3 single path connections.
TEST_F(Gen3FormatAdapterTest, UpdateConnectionStateFromResponseSinglePath) {
  falcon_rue::Response_Gen3 response;
  memset(&response, 0, sizeof(falcon_rue::Response));
  response.flow_label_1 = kFlowLabel0;
  response.flow_label_1_valid = true;
  response.flow_label_2 = kFlowLabel1;
  response.flow_label_2_valid = true;
  response.rtt_state = 1000;
  response.retransmit_timeout = 1200;
  response.nic_congestion_window = 3;  // in fixed format
  response.plb_state = 10;

  Gen3CongestionControlMetadata& ccmeta =
      CongestionControlMetadata::DowncastTo<Gen3CongestionControlMetadata>(
          *connection_state_single_path_->congestion_control_metadata);
  ccmeta.gen2_flow_labels[0] = 0;
  response.fabric_congestion_window = 2;

  // No failure even when all flow weights are zero.
  format_adapter_->UpdateConnectionStateFromResponse(
      connection_state_single_path_, &response);
  EXPECT_EQ(ccmeta.gen2_flow_labels[0],
            kFlowLabel0);  // only flow label 0 is updated
  EXPECT_EQ(ccmeta.retransmit_timeout,
            falcon_->get_rate_update_engine()->FromFalconTimeUnits(1200));
  EXPECT_EQ(ccmeta.rtt_state, 1000);
  EXPECT_EQ(ccmeta.gen3_flow_rtt_state[0], 1000);
  EXPECT_EQ(ccmeta.gen2_plb_state, 10);
  EXPECT_EQ(ccmeta.nic_congestion_window,
            std::max<uint32_t>(1, falcon_rue::FixedToUint<uint32_t, uint32_t>(
                                      response.nic_congestion_window,
                                      falcon_rue::kFractionalBits)));
}

}  // namespace
}  // namespace isekai
