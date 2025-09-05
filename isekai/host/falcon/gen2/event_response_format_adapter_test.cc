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
#include <string>
#include <utility>

#include "absl/time/time.h"
#include "gtest/gtest.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/default_config_generator.h"
#include "isekai/common/packet.h"
#include "isekai/common/testing.h"
#include "isekai/host/falcon/falcon.h"
#include "isekai/host/falcon/falcon_connection_state.h"
#include "isekai/host/falcon/falcon_testing_helpers.h"
#include "isekai/host/falcon/gen1/falcon_model.h"
#include "isekai/host/falcon/gen1/falcon_types.h"
#include "isekai/host/falcon/gen2/connection_state.h"
#include "isekai/host/falcon/gen2/falcon_types.h"
#include "isekai/host/falcon/rue/bits.h"
#include "isekai/host/falcon/rue/fixed.h"
#include "isekai/host/falcon/rue/format.h"
#include "isekai/host/falcon/rue/format_gen2.h"

namespace isekai {
namespace {

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

// Test fixture for the Gen2 EventResponseFormatAdapter class.
class Gen2FormatAdapterTest
    : public FalconTestingHelpers::FalconTestSetup,
      public ::testing::TestWithParam</*falcon_version*/ int> {
 protected:
  void SetUp() override {
    FalconConfig config =
        DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
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
        falcon_rue::Event_Gen2, falcon_rue::Response_Gen2>>(falcon_.get());
  }

  int GetFalconVersion() { return GetParam(); }

  std::unique_ptr<EventResponseFormatAdapter<falcon_rue::Event_Gen2,
                                             falcon_rue::Response_Gen2>>
      format_adapter_;
  Gen2ConnectionState* connection_state_multipath_;
  Gen2ConnectionState* connection_state_single_path_;
};

INSTANTIATE_TEST_SUITE_P(
    FormatAdapterTest, Gen2FormatAdapterTest,
    /*version=*/testing::Values(2),
    [](const testing::TestParamInfo<Gen2FormatAdapterTest::ParamType>& info) {
      const int version = info.param;
      return std::to_string(version);
    });

// Tests that the RTO event is populated properly.
TEST_P(Gen2FormatAdapterTest, FillTimeoutRetransmittedEvent) {
  falcon_rue::Event_Gen2 event;
  memset(&event, 0, sizeof(falcon_rue::Event_Gen2));
  Gen2RueKey rue_key(kCid1, kFlowId1);
  Packet packet;
  packet.metadata.flow_label = kFlowLabel1;

  Gen2CongestionControlMetadata& ccmeta =
      CongestionControlMetadata::DowncastTo<Gen2CongestionControlMetadata>(
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
  EXPECT_EQ(event.fabric_congestion_window, ccmeta.fabric_congestion_window);
  EXPECT_EQ(event.nic_congestion_window,
            (falcon_rue::UintToFixed<uint32_t, uint32_t>(
                ccmeta.nic_congestion_window, falcon_rue::kFractionalBits)));
  EXPECT_EQ(event.delay_select, ccmeta.delay_select);
  EXPECT_EQ(event.fabric_window_time_marker, ccmeta.fabric_window_time_marker);
  EXPECT_EQ(event.nic_window_time_marker, ccmeta.nic_window_time_marker);
  EXPECT_EQ(event.base_delay, ccmeta.base_delay);
  EXPECT_EQ(event.delay_state, ccmeta.delay_state);
  EXPECT_EQ(event.rtt_state, ccmeta.rtt_state);
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
  Gen2CongestionControlMetadata& ccmeta_single_path =
      CongestionControlMetadata::DowncastTo<Gen2CongestionControlMetadata>(
          *connection_state_single_path_->congestion_control_metadata);
  format_adapter_->FillTimeoutRetransmittedEvent(event, &rue_key, &packet,
                                                 ccmeta_single_path, 4);

  EXPECT_EQ(event.connection_id, rue_key.scid);
  EXPECT_EQ(event.flow_label, packet.metadata.flow_label);
  EXPECT_EQ(event.multipath_enable, false);
}

// Tests that the NACK event is populated properly.
TEST_P(Gen2FormatAdapterTest, FillNackEvent) {
  falcon_rue::Event_Gen2 event;
  memset(&event, 0, sizeof(falcon_rue::Event_Gen2));
  Gen2RueKey rue_key(kCid1, kFlowId1);
  Packet packet;
  PopulateNackPacket(&packet);
  packet.metadata.flow_label = kFlowLabel1;

  Gen2CongestionControlMetadata& ccmeta =
      CongestionControlMetadata::DowncastTo<Gen2CongestionControlMetadata>(
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
  EXPECT_EQ(event.fabric_congestion_window, ccmeta.fabric_congestion_window);
  EXPECT_EQ(event.nic_congestion_window,
            (falcon_rue::UintToFixed<uint32_t, uint32_t>(
                ccmeta.nic_congestion_window, falcon_rue::kFractionalBits)));
  EXPECT_EQ(event.delay_select, ccmeta.delay_select);
  EXPECT_EQ(event.fabric_window_time_marker, ccmeta.fabric_window_time_marker);
  EXPECT_EQ(event.nic_window_time_marker, ccmeta.nic_window_time_marker);
  EXPECT_EQ(event.base_delay, ccmeta.base_delay);
  EXPECT_EQ(event.delay_state, ccmeta.delay_state);
  EXPECT_EQ(event.rtt_state, ccmeta.rtt_state);
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
  Gen2CongestionControlMetadata& ccmeta_single_path =
      CongestionControlMetadata::DowncastTo<Gen2CongestionControlMetadata>(
          *connection_state_single_path_->congestion_control_metadata);
  format_adapter_->FillNackEvent(event, &rue_key, &packet, ccmeta_single_path,
                                 4);

  EXPECT_EQ(event.connection_id, rue_key.scid);
  EXPECT_EQ(event.flow_label, packet.metadata.flow_label);
  EXPECT_EQ(event.multipath_enable, false);
}

// Tests that the explicit ACK event is populated properly.
TEST_P(Gen2FormatAdapterTest, FillExplicitAckEvent) {
  falcon_rue::Event_Gen2 event;
  memset(&event, 0, sizeof(falcon_rue::Event_Gen2));
  Gen2RueKey rue_key(kCid1, kFlowId1);
  Packet packet;
  PopulateAckPacket(&packet);
  packet.metadata.flow_label = kFlowLabel1;

  Gen2CongestionControlMetadata& ccmeta =
      CongestionControlMetadata::DowncastTo<Gen2CongestionControlMetadata>(
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
  EXPECT_EQ(event.fabric_congestion_window, ccmeta.fabric_congestion_window);
  EXPECT_EQ(event.nic_congestion_window,
            (falcon_rue::UintToFixed<uint32_t, uint32_t>(
                ccmeta.nic_congestion_window, falcon_rue::kFractionalBits)));
  EXPECT_EQ(event.delay_select, ccmeta.delay_select);
  EXPECT_EQ(event.fabric_window_time_marker, ccmeta.fabric_window_time_marker);
  EXPECT_EQ(event.nic_window_time_marker, ccmeta.nic_window_time_marker);
  EXPECT_EQ(event.base_delay, ccmeta.base_delay);
  EXPECT_EQ(event.delay_state, ccmeta.delay_state);
  EXPECT_EQ(event.rtt_state, ccmeta.rtt_state);
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
  Gen2CongestionControlMetadata& ccmeta_single_path =
      CongestionControlMetadata::DowncastTo<Gen2CongestionControlMetadata>(
          *connection_state_single_path_->congestion_control_metadata);
  format_adapter_->FillExplicitAckEvent(event, &rue_key, &packet,
                                        ccmeta_single_path, 4, true, false);
  EXPECT_EQ(event.connection_id, rue_key.scid);
  EXPECT_EQ(event.flow_label, packet.metadata.flow_label);
  EXPECT_EQ(event.multipath_enable, false);
}

// Tests the IsRandomizePath() function.
TEST_P(Gen2FormatAdapterTest, IsRandomizePath) {
  falcon_rue::Response_Gen2 response;
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
TEST_P(Gen2FormatAdapterTest, UpdateConnectionStateFromResponseXoffMetadata) {
  falcon_rue::Response_Gen2 response;
  memset(&response, 0, sizeof(falcon_rue::Response));
  response.alpha_request = 1;
  response.alpha_response = 2;

  format_adapter_->UpdateConnectionStateFromResponse(
      connection_state_single_path_, &response);
  EXPECT_EQ(
      connection_state_single_path_->connection_xoff_metadata.alpha_request, 1);
  EXPECT_EQ(
      connection_state_single_path_->connection_xoff_metadata.alpha_response,
      2);
}

// Tests that the congestion control metadata is updated as expected by the
// response for Gen2 multipath connections.
TEST_P(Gen2FormatAdapterTest, UpdateConnectionStateFromResponseMultipath) {
  falcon_rue::Response_Gen2 response;
  memset(&response, 0, sizeof(falcon_rue::Response));
  response.flow_label_1 = kFlowLabel0;
  response.flow_label_1_valid = true;
  response.rtt_state = 1000;
  response.retransmit_timeout = 1200;
  response.nic_congestion_window = 3;  // in fixed format
  response.plb_state = 10;

  // Expect failure when all flow weights are zero.
  EXPECT_DEATH(format_adapter_->UpdateConnectionStateFromResponse(
                   connection_state_multipath_, &response),
               "");

  response.flow_label_2_weight = 1;

  format_adapter_->UpdateConnectionStateFromResponse(
      connection_state_multipath_, &response);
  Gen2CongestionControlMetadata& ccmeta =
      CongestionControlMetadata::DowncastTo<Gen2CongestionControlMetadata>(
          *connection_state_multipath_->congestion_control_metadata);
  EXPECT_EQ(ccmeta.gen2_flow_labels[0], kFlowLabel0);
  EXPECT_EQ(ccmeta.gen2_flow_weights[1], 1);
  EXPECT_EQ(ccmeta.retransmit_timeout,
            falcon_->get_rate_update_engine()->FromFalconTimeUnits(1200));
  EXPECT_EQ(ccmeta.rtt_state, 1000);
  EXPECT_EQ(ccmeta.gen2_plb_state, 10);
  EXPECT_EQ(ccmeta.nic_congestion_window,
            std::max<uint32_t>(1, falcon_rue::FixedToUint<uint32_t, uint32_t>(
                                      response.nic_congestion_window,
                                      falcon_rue::kFractionalBits)));
}

// Tests that the congestion control metadata is updated as expected by the
// response for Gen2 single path connections.
TEST_P(Gen2FormatAdapterTest, UpdateConnectionStateFromResponseSinglePath) {
  falcon_rue::Response_Gen2 response;
  memset(&response, 0, sizeof(falcon_rue::Response));
  response.flow_label_1 = kFlowLabel0;
  response.flow_label_1_valid = true;
  response.flow_label_2 = kFlowLabel1;
  response.flow_label_2_valid = true;
  response.rtt_state = 1000;
  response.retransmit_timeout = 1200;
  response.nic_congestion_window = 3;  // in fixed format
  response.plb_state = 10;

  Gen2CongestionControlMetadata& ccmeta =
      CongestionControlMetadata::DowncastTo<Gen2CongestionControlMetadata>(
          *connection_state_single_path_->congestion_control_metadata);
  ccmeta.gen2_flow_labels[0] = 0;
  // No failure even when all flow weights are zero.
  format_adapter_->UpdateConnectionStateFromResponse(
      connection_state_single_path_, &response);
  EXPECT_EQ(ccmeta.gen2_flow_labels[0],
            kFlowLabel0);  // only flow label 0 is updated
  EXPECT_EQ(ccmeta.retransmit_timeout,
            falcon_->get_rate_update_engine()->FromFalconTimeUnits(1200));
  EXPECT_EQ(ccmeta.rtt_state, 1000);
  EXPECT_EQ(ccmeta.gen2_plb_state, 10);
  EXPECT_EQ(ccmeta.nic_congestion_window,
            std::max<uint32_t>(1, falcon_rue::FixedToUint<uint32_t, uint32_t>(
                                      response.nic_congestion_window,
                                      falcon_rue::kFractionalBits)));
}

}  // namespace
}  // namespace isekai
