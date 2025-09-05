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

#include <cstdint>
#include <memory>

#include "absl/status/statusor.h"
#include "absl/time/time.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/default_config_generator.h"
#include "isekai/common/packet.h"
#include "isekai/common/simple_environment.h"
#include "isekai/common/testing.h"
#include "isekai/host/falcon/falcon.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/falcon_connection_state.h"
#include "isekai/host/falcon/falcon_testing_helpers.h"
#include "isekai/host/falcon/gen1/falcon_model.h"

namespace isekai {

namespace {

using ::testing::_;

// This defines all the objects needed for setup and testing.
class Gen1AckCoalescingEngineTest
    : public FalconTestingHelpers::FalconTestSetup,
      public ::testing::Test {};

// Test that the congestion control metadata are correctly reflected in an ack
// from a data packet.
TEST_F(Gen1AckCoalescingEngineTest, AckCongestionControlReflectionTest) {
  FalconConfig config = DefaultConfigGenerator::DefaultFalconConfig(1);
  InitFalcon(config);
  // Initialize the connection state.
  auto connection_metadata =
      FalconTestingHelpers::InitializeConnectionMetadata(falcon_.get());
  // Set ack coalescing threshold to be = 2
  connection_metadata->ack_coalescing_threshold = 2;
  ConnectionState* connection_state =
      FalconTestingHelpers::InitializeConnectionState(
          falcon_.get(), std::move(connection_metadata));
  // Set the flow label values.
  uint32_t congestion_metadata_flowlabel = 1;
  uint32_t rx_packet_flowlabel = 2;
  connection_state->congestion_control_metadata->flow_label =
      congestion_metadata_flowlabel;

  // Create a fake incoming transaction with the required fields setup.
  auto rx_packet = std::make_unique<Packet>();
  rx_packet->packet_type = falcon::PacketType::kPullRequest;
  rx_packet->falcon.dest_cid = 1;
  rx_packet->falcon.psn = 63;  // PSN within the window
  rx_packet->falcon.rsn = 1;
  rx_packet->falcon.ack_req = false;  // Request immediate ACK
  rx_packet->timestamps.sent_timestamp = absl::Microseconds(1);
  rx_packet->timestamps.received_timestamp = absl::Microseconds(2);
  rx_packet->metadata.flow_label = rx_packet_flowlabel;
  rx_packet->metadata.forward_hops = 1;
  EXPECT_OK(reliability_manager_->ReceivePacket(rx_packet.get()));

  EXPECT_CALL(shaper_, TransferTxPacket(_))
      .WillOnce([&](std::unique_ptr<Packet> p) {
        EXPECT_EQ(p->packet_type, falcon::PacketType::kAck);
        EXPECT_EQ(p->ack.timestamp_1, absl::Microseconds(1));
        EXPECT_EQ(p->ack.timestamp_2, absl::Microseconds(2));
        EXPECT_EQ(p->ack.forward_hops, 1);
        // For Gen1, ACK should have congestion_metadata_flowlabel.
        EXPECT_EQ(p->metadata.flow_label, congestion_metadata_flowlabel);
      });
  env_.Run();
}

// Test that an ACK is always transmitted when AckReq bit set.
TEST_F(Gen1AckCoalescingEngineTest, AckRequestBitSetTest) {
  FalconConfig config = DefaultConfigGenerator::DefaultFalconConfig(1);
  InitFalcon(config);
  // Initialize the connection state.
  auto connection_metadata =
      FalconTestingHelpers::InitializeConnectionMetadata(falcon_.get());
  // Set ack coalescing threshold to be = 100
  connection_metadata->ack_coalescing_threshold = 100;
  auto ack_coalescing_timeout = connection_metadata->ack_coalescing_timeout;
  ConnectionState* connection_state =
      FalconTestingHelpers::InitializeConnectionState(
          falcon_.get(), std::move(connection_metadata));

  // Set the flow label values.
  uint32_t congestion_metadata_flowlabel = 1;
  uint32_t rx_packet_flowlabel = 2;
  connection_state->congestion_control_metadata->flow_label =
      congestion_metadata_flowlabel;

  // Create a fake incoming transaction with the required fields setup.
  auto rx_packet = std::make_unique<Packet>();
  rx_packet->packet_type = falcon::PacketType::kPullRequest;
  rx_packet->falcon.dest_cid = 1;
  rx_packet->falcon.psn = 63;  // PSN within the window
  rx_packet->falcon.rsn = 1;
  rx_packet->falcon.ack_req = true;  // Set Request immediate ACK to true.
  rx_packet->timestamps.sent_timestamp = absl::Microseconds(1);
  rx_packet->timestamps.received_timestamp = absl::Microseconds(2);
  rx_packet->metadata.flow_label = rx_packet_flowlabel;
  rx_packet->metadata.forward_hops = 1;
  EXPECT_OK(reliability_manager_->ReceivePacket(rx_packet.get()));

  // ACK should occur because ack_req is set.
  EXPECT_CALL(shaper_, TransferTxPacket(_))
      .WillOnce([&](std::unique_ptr<Packet> p) {
        EXPECT_EQ(p->packet_type, falcon::PacketType::kAck);
        // Before the timer expires.
        EXPECT_LT(env_.ElapsedTime(), ack_coalescing_timeout);
        EXPECT_EQ(p->ack.timestamp_1, absl::Microseconds(1));
        EXPECT_EQ(p->ack.timestamp_2, absl::Microseconds(2));
        EXPECT_EQ(p->ack.forward_hops, 1);
        // For Gen1, ACK should have congestion_metadata_flowlabel.
        EXPECT_EQ(p->metadata.flow_label, congestion_metadata_flowlabel);
      });
  env_.Run();
}

}  // namespace

}  // namespace isekai
