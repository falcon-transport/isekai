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

#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>

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
#include "isekai/host/falcon/rue/format.h"

namespace isekai {
namespace {

constexpr int kFalconVersion1 = 1;
constexpr uint32_t kCid1 = 123;

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

// Test fixture for the Gen1 EventResponseFormatAdapter class.
class Gen1FormatAdapterTest : public FalconTestingHelpers::FalconTestSetup,
                              public ::testing::Test {
 protected:
  void SetUp() override {
    FalconConfig config =
        DefaultConfigGenerator::DefaultFalconConfig(kFalconVersion1);
    InitFalcon(config);
    std::unique_ptr<ConnectionMetadata> metadata_1 =
        FalconTestingHelpers::InitializeConnectionMetadata(falcon_.get(),
                                                           kCid1);
    connection_state_ = FalconTestingHelpers::InitializeConnectionState(
        falcon_.get(), std::move(metadata_1));
    format_adapter_ = std::make_unique<
        EventResponseFormatAdapter<falcon_rue::Event, falcon_rue::Response>>(
        falcon_.get());
  }

  std::unique_ptr<
      EventResponseFormatAdapter<falcon_rue::Event, falcon_rue::Response>>
      format_adapter_;
  ConnectionState* connection_state_;
};

// Tests the IsRandomizePath() function.
TEST_F(Gen1FormatAdapterTest, IsRandomizePath) {
  falcon_rue::Response response;
  memset(&response, 0, sizeof(falcon_rue::Response));
  response.randomize_path = true;
  EXPECT_EQ(format_adapter_->IsRandomizePath(&response), true);

  response.randomize_path = false;
  EXPECT_EQ(format_adapter_->IsRandomizePath(&response), false);
}

// Tests that the connection state is updated as expected by the
// response.
TEST_F(Gen1FormatAdapterTest, UpdateConnectionStateFromResponse) {
  falcon_rue::Response response;
  memset(&response, 0, sizeof(falcon_rue::Response));
  response.randomize_path = true;
  response.rtt_state = 1000;
  response.retransmit_timeout = 1200;
  CongestionControlMetadata& ccmeta =
      *connection_state_->congestion_control_metadata;
  ccmeta.flow_label = std::numeric_limits<uint32_t>::max();

  format_adapter_->UpdateConnectionStateFromResponse(connection_state_,
                                                     &response);
  EXPECT_NE(ccmeta.flow_label, std::numeric_limits<uint32_t>::max());
  EXPECT_EQ(ccmeta.retransmit_timeout,
            falcon_->get_rate_update_engine()->FromFalconTimeUnits(1200));
  EXPECT_EQ(ccmeta.rtt_state, 1000);
}

// Tests that the RTO event is populated properly.
TEST_F(Gen1FormatAdapterTest, FillTimeoutRetransmittedEvent) {
  falcon_rue::Event event;
  memset(&event, 0, sizeof(falcon_rue::Event));
  RueKey rue_key(kCid1);
  Packet packet;
  CongestionControlMetadata& ccmeta =
      *connection_state_->congestion_control_metadata;
  format_adapter_->FillTimeoutRetransmittedEvent(event, &rue_key, &packet,
                                                 ccmeta, 4);

  EXPECT_EQ(event.connection_id, rue_key.scid);
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
  EXPECT_EQ(event.inter_packet_gap,
            falcon_->get_rate_update_engine()->ToTimingWheelTimeUnits(
                ccmeta.inter_packet_gap));
  EXPECT_EQ(event.nic_congestion_window, ccmeta.nic_congestion_window);
  EXPECT_EQ(event.retransmit_timeout,
            falcon_->get_rate_update_engine()->ToFalconTimeUnits(
                ccmeta.retransmit_timeout));
  EXPECT_EQ(event.delay_select, ccmeta.delay_select);
  EXPECT_EQ(event.fabric_window_time_marker, ccmeta.fabric_window_time_marker);
  EXPECT_EQ(event.nic_window_time_marker, ccmeta.nic_window_time_marker);
  EXPECT_EQ(event.nic_window_direction, ccmeta.nic_window_direction);
  EXPECT_EQ(event.base_delay, ccmeta.base_delay);
  EXPECT_EQ(event.delay_state, ccmeta.delay_state);
  EXPECT_EQ(event.rtt_state, ccmeta.rtt_state);
  EXPECT_EQ(event.cc_opaque, ccmeta.cc_opaque);
}

// Tests that the NACK event is populated properly.
TEST_F(Gen1FormatAdapterTest, FillNackEvent) {
  falcon_rue::Event event;
  memset(&event, 0, sizeof(falcon_rue::Event));
  RueKey rue_key(kCid1);
  Packet packet;
  PopulateNackPacket(&packet);
  CongestionControlMetadata& ccmeta =
      *connection_state_->congestion_control_metadata;
  format_adapter_->FillNackEvent(event, &rue_key, &packet, ccmeta, 4);

  EXPECT_EQ(event.connection_id, rue_key.scid);
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
  EXPECT_EQ(event.inter_packet_gap,
            falcon_->get_rate_update_engine()->ToTimingWheelTimeUnits(
                ccmeta.inter_packet_gap));
  EXPECT_EQ(event.nic_congestion_window, ccmeta.nic_congestion_window);
  EXPECT_EQ(event.retransmit_timeout,
            falcon_->get_rate_update_engine()->ToFalconTimeUnits(
                ccmeta.retransmit_timeout));
  EXPECT_EQ(event.delay_select, ccmeta.delay_select);
  EXPECT_EQ(event.fabric_window_time_marker, ccmeta.fabric_window_time_marker);
  EXPECT_EQ(event.nic_window_time_marker, ccmeta.nic_window_time_marker);
  EXPECT_EQ(event.nic_window_direction, ccmeta.nic_window_direction);
  EXPECT_EQ(event.base_delay, ccmeta.base_delay);
  EXPECT_EQ(event.delay_state, ccmeta.delay_state);
  EXPECT_EQ(event.rtt_state, ccmeta.rtt_state);
  EXPECT_EQ(event.cc_opaque, ccmeta.cc_opaque);
  EXPECT_EQ(event.reserved_1, 0);
  EXPECT_EQ(event.reserved_2, 0);
  EXPECT_EQ(event.gen_bit, 0);
}

// Tests that the explicit ACK event is populated properly.
TEST_F(Gen1FormatAdapterTest, FillExplicitAckEvent) {
  falcon_rue::Event event;
  memset(&event, 0, sizeof(falcon_rue::Event));
  RueKey rue_key(kCid1);
  Packet packet;
  PopulateAckPacket(&packet);
  CongestionControlMetadata& ccmeta =
      *connection_state_->congestion_control_metadata;
  format_adapter_->FillExplicitAckEvent(event, &rue_key, &packet, ccmeta, 4,
                                        true, false);

  EXPECT_EQ(event.connection_id, rue_key.scid);
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
  EXPECT_EQ(event.inter_packet_gap,
            falcon_->get_rate_update_engine()->ToTimingWheelTimeUnits(
                ccmeta.inter_packet_gap));
  EXPECT_EQ(event.nic_congestion_window, ccmeta.nic_congestion_window);
  EXPECT_EQ(event.retransmit_timeout,
            falcon_->get_rate_update_engine()->ToFalconTimeUnits(
                ccmeta.retransmit_timeout));
  EXPECT_EQ(event.delay_select, ccmeta.delay_select);
  EXPECT_EQ(event.fabric_window_time_marker, ccmeta.fabric_window_time_marker);
  EXPECT_EQ(event.nic_window_time_marker, ccmeta.nic_window_time_marker);
  EXPECT_EQ(event.nic_window_direction, ccmeta.nic_window_direction);
  EXPECT_EQ(event.base_delay, ccmeta.base_delay);
  EXPECT_EQ(event.delay_state, ccmeta.delay_state);
  EXPECT_EQ(event.rtt_state, ccmeta.rtt_state);
  EXPECT_EQ(event.cc_opaque, ccmeta.cc_opaque);
  EXPECT_EQ(event.reserved_1, 0);
  EXPECT_EQ(event.reserved_2, 0);
  EXPECT_EQ(event.gen_bit, 0);
  EXPECT_EQ(event.eack, true);
  EXPECT_EQ(event.eack_drop, false);
}

}  // namespace
}  // namespace isekai
