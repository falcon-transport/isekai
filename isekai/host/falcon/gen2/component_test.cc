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
#include <cstdint>
#include <memory>
#include <utility>

#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/time/time.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/default_config_generator.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/common/packet.h"
#include "isekai/host/falcon/falcon.h"
#include "isekai/host/falcon/falcon_component_test_infrastructure.h"
#include "isekai/host/falcon/falcon_connection_state.h"
#include "isekai/host/falcon/gen2/falcon_utils.h"

// Tests in this file are for Gen2 only.
namespace isekai {
namespace {

constexpr uint8_t kDegreeOfMultipathing = 4;

// Tests a successful end-to-end message delivery with Gen2 multipathing and
// with no drops. The packets should be transmitted in a round-robin manner
// across all 4 flows in the connection, and the ACKs should use the same flow
// label as the initial packets.
TEST_P(FalconComponentTest, TestGen2MultipathingNoDrops) {
  constexpr int kNumMessages = 32;
  // In this test, we want to set ack_coalescing time to be higher than network
  // delay to get a simpler packet deliver sequence.
  constexpr int kAckCoalescingNumAck = 100;  // more than kNumMessages
  constexpr int kAckCoalescingNs = 15000;    // 15us
  absl::Duration kAckCoalescingTimer = absl::Nanoseconds(kAckCoalescingNs);
  absl::Duration inter_host_rx_scheduler_tick =
      absl::Nanoseconds(std::get<1>(GetParam()));

  FalconConfig falcon_config =
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
  falcon_config.mutable_ack_coalescing_thresholds()->set_count(
      kAckCoalescingNumAck);
  falcon_config.mutable_ack_coalescing_thresholds()->set_timeout_ns(
      kAckCoalescingNs);
  falcon_config.mutable_gen2_config_options()
      ->mutable_multipath_config()
      ->set_path_selection_policy(FalconConfig::Gen2ConfigOptions::
                                      MultipathConfig::OPEN_LOOP_ROUND_ROBIN);
  falcon_config.set_enable_ack_request_bit(false);
  // Initialize both the Falcon hosts.
  InitFalconHosts(falcon_config, kDegreeOfMultipathing);

  // Create and submit RDMA Write request from host1 to host2.
  SubmitMultipleUnsolicitedWriteRequests(kRsn, kNumMessages);
  // Inspect the packets in the network queue.
  for (int i = 0; i < kNumMessages; ++i) {
    const Packet* p = network_->PeekAt(i);
    ASSERT_EQ(p->packet_type, falcon::PacketType::kPushUnsolicitedData);
    // Expect flow labels in round-robin fashion across the 4 flows.
    ASSERT_EQ(
        GetFlowIdFromFlowLabel(p->metadata.flow_label, kDegreeOfMultipathing),
        i % kDegreeOfMultipathing);
    EXPECT_EQ(p->rdma.rsn, i);
  }

  // Deliver all data packets to host2 (target).
  network_->DeliverAll(kNetworkDelay);
  env_->RunFor(kNetworkDelay);
  TargetFalconDeliverPacketsToRdma(kNumMessages, inter_host_rx_scheduler_tick);
  // Wait enough time so that an ACK is generated (reaching ack_coalescing
  // timer).
  env_->RunFor(kAckCoalescingTimer);

  // We expect to have 4 ACKs generated, one for each flow.
  EXPECT_EQ(network_->HeldPacketCount(), 4);
  for (int i = 0; i < network_->HeldPacketCount(); ++i) {
    const Packet* p = network_->PeekAt(i);
    ASSERT_EQ(p->packet_type, falcon::PacketType::kAck);
    ASSERT_EQ(
        GetFlowIdFromFlowLabel(p->metadata.flow_label, kDegreeOfMultipathing),
        i % kDegreeOfMultipathing);
    ASSERT_EQ(p->ack.rdbpsn, kNumMessages);
  }

  // Deliver all the ACKs from Host2 to Host1.
  network_->DeliverAll(kNetworkDelay);
  env_->RunFor(kNetworkDelay + 4 * kFalconProcessingDelay);

  // RDMA on host1 should get completions from Falcon.
  absl::StatusOr<std::pair<QpId, uint32_t>> completion_status;
  for (int i = 0; i < kNumMessages; ++i) {
    completion_status = host1_->GetCompletion();
    ASSERT_OK(completion_status);
    EXPECT_EQ(completion_status.value().first, kHost1QpId);
    EXPECT_EQ(completion_status.value().second, i);
  }
}

// Tests a successful end-to-end message delivery with Gen2 multipathing and
// with drops. The packets should be transmitted in a round-robin manner
// across all 4 flows in the connection, and the ACKs should use the same
// flow label as the initial packets.
TEST_P(FalconComponentTest, TestGen2MultipathingWithDrops) {
  const int num_message = 32;
  // In this test, we want to set ack_coalescing time to be higher than network
  // delay to get a simpler packet deliver sequence.
  int ack_coalescing_num_ack = 100;  // more than kNumMessages
  int ack_coalescing_ns = 15000;     // 15us
  absl::Duration kAckCoalescingTimer = absl::Nanoseconds(ack_coalescing_ns);
  absl::Duration inter_host_rx_scheduler_tick =
      absl::Nanoseconds(std::get<1>(GetParam()));

  FalconConfig falcon_config =
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion());
  falcon_config.mutable_ack_coalescing_thresholds()->set_count(
      ack_coalescing_num_ack);
  falcon_config.mutable_ack_coalescing_thresholds()->set_timeout_ns(
      ack_coalescing_ns);
  falcon_config.mutable_gen2_config_options()
      ->mutable_multipath_config()
      ->set_path_selection_policy(FalconConfig::Gen2ConfigOptions::
                                      MultipathConfig::OPEN_LOOP_ROUND_ROBIN);
  falcon_config.set_enable_ack_request_bit(false);
  // Initialize both the Falcon hosts.
  InitFalconHosts(falcon_config, kDegreeOfMultipathing);

  // Create and submit RDMA Write request from host1 to host2.
  SubmitMultipleUnsolicitedWriteRequests(kRsn, num_message);
  for (int i = 0; i < num_message; ++i) {
    const Packet* p = network_->PeekAt(i);
    ASSERT_EQ(p->packet_type, falcon::PacketType::kPushUnsolicitedData);
    ASSERT_EQ(
        GetFlowIdFromFlowLabel(p->metadata.flow_label, kDegreeOfMultipathing),
        i % kDegreeOfMultipathing);
    EXPECT_EQ(p->rdma.rsn, i);
  }

  // Deliver all data packets to host2 (target).
  network_->Drop();
  network_->DeliverAll(kNetworkDelay);
  // Wait enough time so that
  // 1) Packets are all delivered and processed, and
  // 2) An ACK is generated (reaching ack_coalescing timer).
  env_->RunFor(kNetworkDelay + num_message * kFalconProcessingDelay +
               kAckCoalescingTimer);

  // We expect to have 4 EACKs generated, one for each flow.
  EXPECT_EQ(network_->HeldPacketCount(), 4);
  for (int i = 0; i < network_->HeldPacketCount(); ++i) {
    const Packet* p = network_->PeekAt(i);
    ASSERT_EQ(p->packet_type, falcon::PacketType::kAck);
    EXPECT_EQ(p->ack.received_bitmap.FirstHoleIndex(),
              0);  // First packet in the window was dropped.
    EXPECT_EQ(
        GetFlowIdFromFlowLabel(p->metadata.flow_label, kDegreeOfMultipathing),
        (i + 1) % kDegreeOfMultipathing);
    EXPECT_EQ(p->ack.rdbpsn, 0);
  }

  // Deliver the 4 EACKs from Host2 to Host1.
  network_->DeliverAll(kNetworkDelay);
  env_->RunFor(kNetworkDelay + 4 * kFalconProcessingDelay);

  // Host1 Falcon receives the EACK and retransmits the 1st packet.
  EXPECT_EQ(network_->HeldPacketCount(), 1);
  auto retransmitted_packet = network_->Peek();
  EXPECT_EQ(retransmitted_packet->packet_type,
            falcon::PacketType::kPushUnsolicitedData);
  EXPECT_EQ(GetFlowIdFromFlowLabel(retransmitted_packet->metadata.flow_label,
                                   kDegreeOfMultipathing),
            0);
  EXPECT_EQ(retransmitted_packet->rdma.rsn, 0);

  // Deliver the retx packet from Host 1 to Host 2.
  network_->DeliverAll(kNetworkDelay);
  env_->RunFor(kNetworkDelay + kFalconProcessingDelay);
  TargetFalconDeliverPacketsToRdma(num_message, inter_host_rx_scheduler_tick);

  // Host2 Falcon generates the last ACK.
  env_->RunFor(kAckCoalescingTimer);
  EXPECT_EQ(network_->HeldPacketCount(), 1);
  auto last_ack = network_->Peek();
  EXPECT_EQ(last_ack->packet_type, falcon::PacketType::kAck);
  EXPECT_EQ(last_ack->ack.rdbpsn, num_message);
  // Last ACK belongs to flow ID 0 which the retx packet belongs to.
  EXPECT_EQ(GetFlowIdFromFlowLabel(last_ack->metadata.flow_label,
                                   kDegreeOfMultipathing),
            0);

  // Deliver the last ACK from Host2 to Host1.
  network_->DeliverAll(kNetworkDelay);
  env_->RunFor(kNetworkDelay + num_message * kFalconProcessingDelay);

  // RDMA on host1 should get completions from Falcon.
  absl::StatusOr<std::pair<QpId, uint32_t>> completion_status;
  for (int i = 0; i < num_message; ++i) {
    completion_status = host1_->GetCompletion();
    ASSERT_OK(completion_status);
    EXPECT_EQ(completion_status.value().first, kHost1QpId);
    EXPECT_EQ(completion_status.value().second, i);
  }
}

INSTANTIATE_TEST_SUITE_P(
    ComponentTests, FalconComponentTest,
    testing::Combine(
        testing::Values(FalconConfig::FIRST_PHASE_RESERVATION,
                        FalconConfig::DELAYED_RESERVATION,
                        FalconConfig::ON_DEMAND_RESERVATION),
        /*inter_host_rx_scheduler_tick_ns=*/testing::Values(0, 3, 5),
        /*version=*/testing::Values(2)),
    [](const testing::TestParamInfo<FalconComponentTest::ParamType>& info) {
      const FalconConfig::ResourceReservationMode rsc_reservation_mode =
          std::get<0>(info.param);
      const int rx_tick_ns = std::get<1>(info.param);
      const int version = std::get<2>(info.param);
      return absl::StrCat("RscMode", rsc_reservation_mode, "_RxTick",
                          rx_tick_ns, "_Gen", version);
    });

}  // namespace
}  // namespace isekai
