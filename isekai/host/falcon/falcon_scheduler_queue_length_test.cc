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
#include <utility>
#include <vector>

#include "absl/time/time.h"
#include "gtest/gtest.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/default_config_generator.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/common/packet.h"
#include "isekai/common/simple_environment.h"
#include "isekai/common/testing.h"
#include "isekai/host/falcon/falcon.h"
#include "isekai/host/falcon/falcon_component_test_infrastructure.h"
#include "isekai/host/falcon/falcon_counters.h"
#include "isekai/host/falcon/gen1/falcon_model.h"

namespace isekai {
namespace {

TEST_P(FalconComponentTest, TestSingleUnsolicitedWriteSchedulerCounters) {
  // Initialize both the Falcon hosts.
  InitFalconHosts(
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion()));
  FalconConnectionCounters& initiator_counters =
      host1_->get_falcon()->get_stats_manager()->GetConnectionCounters(
          kHost1Scid);
  FalconConnectionCounters& target_counters =
      host2_->get_falcon()->get_stats_manager()->GetConnectionCounters(
          kHost2Scid);

  // Create and submit a Write request from host1 to host2.
  auto request_packet = CreateWriteRequest(
      /*src_qp=*/kHost1QpId, /*dst_qp=*/kHost2QpId, /*src_cid=*/kHost1Scid,
      /*rsn=*/kRsn, /*op_size=*/32);
  host1_->SubmitTxTransaction(std::move(request_packet));

  // Verify initiator counters on initiating a transaction.
  EXPECT_EQ(initiator_counters.pull_and_ordered_push_request_queue_packets, 0);
  EXPECT_EQ(initiator_counters.unordered_push_request_queue_packets, 0);
  EXPECT_EQ(initiator_counters.push_data_queue_packets, 1);
  EXPECT_EQ(initiator_counters.push_grant_queue_packets, 0);
  EXPECT_EQ(initiator_counters.pull_data_queue_packets, 0);
  env_->RunFor(kFalconProcessingDelay);

  // At this point, host1 should transmit a unsolicited data packet.
  ASSERT_EQ(network_->HeldPacketCount(), 1);
  const Packet* p1 = network_->Peek();
  ASSERT_EQ(p1->packet_type, falcon::PacketType::kPushUnsolicitedData);

  // Deliver the data packet to host2. Host2 receives the packet and sends Ack
  // back. Host1 should get completion for the same write transaction.
  DeliverPushUnsolicitedDataPacketAndCheckCompletionReceival(kRsn);

  EXPECT_EQ(initiator_counters.pull_and_ordered_push_request_queue_packets, 0);
  EXPECT_EQ(initiator_counters.unordered_push_request_queue_packets, 0);
  EXPECT_EQ(initiator_counters.push_data_queue_packets, 0);
  EXPECT_EQ(initiator_counters.push_grant_queue_packets, 0);
  EXPECT_EQ(initiator_counters.pull_data_queue_packets, 0);
  EXPECT_EQ(target_counters.pull_and_ordered_push_request_queue_packets, 0);
  EXPECT_EQ(target_counters.unordered_push_request_queue_packets, 0);
  EXPECT_EQ(target_counters.push_data_queue_packets, 0);
  EXPECT_EQ(target_counters.push_grant_queue_packets, 0);
  EXPECT_EQ(target_counters.pull_data_queue_packets, 0);
}

TEST_P(FalconComponentTest, TestSingleSolicitedWriteSchedulerCounters) {
  // Initialize both the Falcon hosts.
  InitFalconHosts(
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion()));
  FalconConnectionCounters& initiator_counters =
      host1_->get_falcon()->get_stats_manager()->GetConnectionCounters(
          kHost1Scid);
  FalconConnectionCounters& target_counters =
      host2_->get_falcon()->get_stats_manager()->GetConnectionCounters(
          kHost2Scid);

  // Create and submit a Write request from host1 to host2.
  auto request_packet = CreateWriteRequest(
      /*src_qp=*/kHost1QpId, /*dst_qp=*/kHost2QpId, /*src_cid=*/kHost1Scid,
      /*rsn=*/kRsn, /*op_size=*/512);
  host1_->SubmitTxTransaction(std::move(request_packet));

  EXPECT_EQ(initiator_counters.pull_and_ordered_push_request_queue_packets, 1);
  EXPECT_EQ(initiator_counters.unordered_push_request_queue_packets, 0);
  EXPECT_EQ(initiator_counters.push_data_queue_packets, 1);
  EXPECT_EQ(initiator_counters.push_grant_queue_packets, 0);
  EXPECT_EQ(initiator_counters.pull_data_queue_packets, 0);

  env_->RunFor(kFalconProcessingDelay);

  // At this point, host1 should transmit a request packet to the network.
  ASSERT_GE(network_->HeldPacketCount(), 1);
  const Packet* p1 = network_->Peek();
  ASSERT_EQ(p1->packet_type, falcon::PacketType::kPushRequest);

  // Deliver the request packet to host2 (target), and let it process.
  network_->Deliver(kNetworkDelay);
  env_->RunFor(kNetworkDelay);
  EXPECT_EQ(target_counters.pull_and_ordered_push_request_queue_packets, 0);
  EXPECT_EQ(target_counters.unordered_push_request_queue_packets, 0);
  EXPECT_EQ(target_counters.push_data_queue_packets, 0);
  EXPECT_EQ(target_counters.push_grant_queue_packets, 0);  // Sent out
                                                           // immediately.
  EXPECT_EQ(target_counters.pull_data_queue_packets, 0);
  env_->RunFor(kFalconProcessingDelay);

  // Next, host2 should transmit a grant packet.
  ASSERT_GE(network_->HeldPacketCount(), 1);
  const Packet* p2 = network_->Peek();
  ASSERT_EQ(p2->packet_type, falcon::PacketType::kPushGrant);

  // Deliver the grant packet to host1, and let it process.
  network_->Deliver(kNetworkDelay);
  env_->RunFor(kNetworkDelay + kFalconProcessingDelay);

  // Next, host1 should transmit a data packet.
  ASSERT_GE(network_->HeldPacketCount(), 1);
  const Packet* p3 = network_->Peek();
  ASSERT_EQ(p3->packet_type, falcon::PacketType::kPushSolicitedData);

  // Deliver the data packet to host2. Host2 receives the packet and sends Ack
  // back. Host1 should get completion for the same write transaction.
  DeliverPushUnsolicitedDataPacketAndCheckCompletionReceival(kRsn);

  EXPECT_EQ(initiator_counters.pull_and_ordered_push_request_queue_packets, 0);
  EXPECT_EQ(initiator_counters.unordered_push_request_queue_packets, 0);
  EXPECT_EQ(initiator_counters.push_data_queue_packets, 0);
  EXPECT_EQ(initiator_counters.push_grant_queue_packets, 0);
  EXPECT_EQ(initiator_counters.pull_data_queue_packets, 0);
  EXPECT_EQ(target_counters.pull_and_ordered_push_request_queue_packets, 0);
  EXPECT_EQ(target_counters.unordered_push_request_queue_packets, 0);
  EXPECT_EQ(target_counters.push_data_queue_packets, 0);
  EXPECT_EQ(target_counters.push_grant_queue_packets, 0);
  EXPECT_EQ(target_counters.pull_data_queue_packets, 0);
}

TEST_P(FalconComponentTest, TestSingleReadWithAckCoalesceSchedulerCounters) {
  // Initialize both the Falcon hosts.
  InitFalconHosts(
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion()));
  FalconConnectionCounters& initiator_counters =
      host1_->get_falcon()->get_stats_manager()->GetConnectionCounters(
          kHost1Scid);
  FalconConnectionCounters& target_counters =
      host2_->get_falcon()->get_stats_manager()->GetConnectionCounters(
          kHost2Scid);

  // Create and submit an RDMA Read request from host1 to host2.
  auto request_packet = CreateReadRequest(
      /*src_qp=*/kHost1QpId, /*dst_qp=*/kHost2QpId, /*src_cid=*/kHost1Scid,
      /*rsn=*/kRsn, /*op_size=*/32);
  host1_->SubmitTxTransaction(std::move(request_packet));
  EXPECT_EQ(initiator_counters.pull_and_ordered_push_request_queue_packets, 1);
  EXPECT_EQ(initiator_counters.unordered_push_request_queue_packets, 0);
  EXPECT_EQ(initiator_counters.push_data_queue_packets, 0);
  EXPECT_EQ(initiator_counters.push_grant_queue_packets, 0);
  EXPECT_EQ(initiator_counters.pull_data_queue_packets, 0);
  env_->RunFor(kFalconProcessingDelay);

  // At this point, host1 should transmit a pull request packet.
  ASSERT_EQ(network_->HeldPacketCount(), 1);
  const Packet* p1 = network_->Peek();
  ASSERT_EQ(p1->packet_type, falcon::PacketType::kPullRequest);

  // Deliver the pull request packet from host1 to host2 and let it process.
  network_->Deliver(kNetworkDelay);
  env_->RunFor(kNetworkDelay + kFalconProcessingDelay);

  // RDMA on host2 should receive the same read request transaction.
  ASSERT_OK_AND_ASSIGN(auto received_packet, host2_->GetSubmittedTransaction());

  // Ack the read request to FALCON host2 (target).
  host2_->SubmitAckTransaction(kHost2Scid, received_packet->rdma.rsn,
                               Packet::Syndrome::kAck);

  // Create a response packet and send it to FALCON host2 (target), let it
  // process.
  auto response_packet = CreateReadResponse(
      /*src_qp=*/kHost2QpId, /*dst_qp=*/kHost1QpId, /*src_cid=*/kHost2Scid,
      /*rsn=*/received_packet->rdma.rsn,
      /*sgl=*/std::move(received_packet->rdma.sgl));

  // Respond back to FALCON immediately, causing the Ack and PullData to be sent
  // back to the initiator on the packet.
  host2_->SubmitTxTransaction(std::move(response_packet));

  EXPECT_EQ(target_counters.pull_and_ordered_push_request_queue_packets, 0);
  EXPECT_EQ(target_counters.unordered_push_request_queue_packets, 0);
  EXPECT_EQ(target_counters.push_data_queue_packets, 0);
  EXPECT_EQ(target_counters.push_grant_queue_packets, 0);
  EXPECT_EQ(target_counters.pull_data_queue_packets, 1);

  env_->RunFor(kFalconProcessingDelay);

  // Next, host2 should transmit a response packet.
  ASSERT_EQ(network_->HeldPacketCount(), 1);
  const Packet* p2 = network_->Peek();
  ASSERT_EQ(p2->packet_type, falcon::PacketType::kPullData);

  // Deliver the pull data packet from host2 to host1, and let it process.
  network_->Deliver(kNetworkDelay);
  env_->RunFor(kNetworkDelay + kFalconProcessingDelay);

  // RDMA on host1 should get the read response packet from FALCON.
  ASSERT_OK_AND_ASSIGN(auto packet, host1_->GetSubmittedTransaction());
  EXPECT_EQ(packet->falcon.dest_cid, kHost1Scid);
  EXPECT_EQ(packet->falcon.rsn, kRsn);
  EXPECT_EQ(packet->rdma.opcode, Packet::Rdma::Opcode::kReadResponseOnly);
  EXPECT_EQ(packet->packet_type, falcon::PacketType::kPullData);

  // FALCON on host1 should send out an ACK corresponding to the read response
  // packet from FALCON.
  env_->RunFor(kDefaultAckCoalescingTimer + 2 * kFalconProcessingDelay);
  EXPECT_EQ(network_->HeldPacketCount(), 1);

  // Verify that it is an ACK and deliver it back to target FALCON.
  const Packet* p4 = network_->Peek();
  ASSERT_EQ(p4->packet_type, falcon::PacketType::kAck);
  network_->Deliver(kNetworkDelay);
  env_->RunFor(kNetworkDelay + kFalconProcessingDelay);

  EXPECT_EQ(initiator_counters.pull_and_ordered_push_request_queue_packets, 0);
  EXPECT_EQ(initiator_counters.unordered_push_request_queue_packets, 0);
  EXPECT_EQ(initiator_counters.push_data_queue_packets, 0);
  EXPECT_EQ(initiator_counters.push_grant_queue_packets, 0);
  EXPECT_EQ(initiator_counters.pull_data_queue_packets, 0);
  EXPECT_EQ(target_counters.pull_and_ordered_push_request_queue_packets, 0);
  EXPECT_EQ(target_counters.unordered_push_request_queue_packets, 0);
  EXPECT_EQ(target_counters.push_data_queue_packets, 0);
  EXPECT_EQ(target_counters.push_grant_queue_packets, 0);
  EXPECT_EQ(target_counters.pull_data_queue_packets, 0);
}

INSTANTIATE_TEST_SUITE_P(
    ComponentTests, FalconComponentTest,
    testing::Combine(
        testing::Values(FalconConfig::FIRST_PHASE_RESERVATION),
        /*inter_host_rx_scheduler_tick_ns=*/testing::Values(0, 3, 5),
        /*version=*/testing::Values(1, 2)),
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
