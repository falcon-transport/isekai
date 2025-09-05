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

#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/time/time.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "isekai/common/common_util.h"  // IWYU pragma: keep
#include "isekai/common/config.pb.h"
#include "isekai/common/default_config_generator.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/common/packet.h"
#include "isekai/common/simple_environment.h"
#include "isekai/common/testing.h"
#include "isekai/host/falcon/falcon.h"
#include "isekai/host/falcon/falcon_component_test_infrastructure.h"
#include "isekai/host/falcon/falcon_connection_state.h"
#include "isekai/host/falcon/falcon_counters.h"
#include "isekai/host/falcon/falcon_resource_credits.h"
#include "isekai/host/falcon/gen1/falcon_model.h"
#include "isekai/host/falcon/gen1/falcon_types.h"
#include "isekai/host/falcon/gen1/falcon_utils.h"
#include "isekai/host/falcon/gen1/resource_manager.h"

namespace isekai {
namespace {

// Tests a single unsolicited write.
TEST_P(FalconComponentTest, TestSingleUnsolicitedWrite) {
  // Initialize both the Falcon hosts.
  InitFalconHosts(
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion()));
  // Create and submit a Write request from host1 to host2.
  auto request_packet = CreateWriteRequest(
      /*src_qp=*/kHost1QpId, /*dst_qp=*/kHost2QpId, /*src_cid=*/kHost1Scid,
      /*rsn=*/kRsn, /*op_size=*/32);
  host1_->SubmitTxTransaction(std::move(request_packet));
  env_->RunFor(kFalconProcessingDelay);

  // At this point, host1 should transmit a unsolicited data packet.
  ASSERT_EQ(network_->HeldPacketCount(), 1);
  const Packet* p1 = network_->Peek();
  ASSERT_EQ(p1->packet_type, falcon::PacketType::kPushUnsolicitedData);

  // Deliver the data packet to host2. Host2 receives the packet and sends Ack
  // back. Host1 should get completion for the same write transaction.
  DeliverPushUnsolicitedDataPacketAndCheckCompletionReceival(kRsn);

  // Check correctness of counters.
  FalconConnectionCounters initiator_counters =
      host1_->get_falcon()->get_stats_manager()->GetConnectionCounters(
          kHost1Scid);
  EXPECT_EQ(initiator_counters.push_request_from_ulp, 1);
  EXPECT_EQ(initiator_counters.completions_to_ulp, 1);
  EXPECT_EQ(initiator_counters.initiator_tx_push_unsolicited_data, 1);
  EXPECT_EQ(initiator_counters.rx_acks, 1);
  EXPECT_EQ(initiator_counters.tx_packets, 1);
  EXPECT_EQ(initiator_counters.rx_packets, 1);

  FalconConnectionCounters target_counters =
      host2_->get_falcon()->get_stats_manager()->GetConnectionCounters(
          kHost2Scid);
  EXPECT_EQ(target_counters.target_rx_push_unsolicited_data, 1);
  EXPECT_EQ(target_counters.push_data_to_ulp, 1);
  EXPECT_EQ(target_counters.acks_from_ulp, 1);
  EXPECT_EQ(target_counters.tx_acks, 1);
  EXPECT_EQ(target_counters.rx_packets, 1);
  EXPECT_EQ(target_counters.tx_packets, 1);

  // Run for an additional timeout delay duration and ensure no more packets
  // have been transmitted by either the initiator or target.
  env_->RunFor(kRetransmissionDelay);
  EXPECT_EQ(network_->HeldPacketCount(), 0);

  // Check if all the Falcon resources have been released on both the hosts.
  EXPECT_TRUE(host1_->CheckIfAllFalconResourcesReleasedForTesting());
  EXPECT_TRUE(host2_->CheckIfAllFalconResourcesReleasedForTesting());
  // Check if credits related to the RDMA managed Falcon resources are returned.
  EXPECT_TRUE(host1_->CheckIfAllRdmaManagedFalconResourcesReleasedForTesting());
  EXPECT_TRUE(host2_->CheckIfAllRdmaManagedFalconResourcesReleasedForTesting());
}

// Tests a single solicited write.
TEST_P(FalconComponentTest, TestSingleSolicitedWrite) {
  // Initialize both the Falcon hosts.
  InitFalconHosts(
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion()));
  // Create and submit a Write request from host1 to host2.
  auto request_packet = CreateWriteRequest(
      /*src_qp=*/kHost1QpId, /*dst_qp=*/kHost2QpId, /*src_cid=*/kHost1Scid,
      /*rsn=*/kRsn, /*op_size=*/512);
  host1_->SubmitTxTransaction(std::move(request_packet));
  env_->RunFor(kFalconProcessingDelay);

  // At this point, host1 should transmit a request packet to the network.
  ASSERT_GE(network_->HeldPacketCount(), 1);
  const Packet* p1 = network_->Peek();
  ASSERT_EQ(p1->packet_type, falcon::PacketType::kPushRequest);

  // Deliver the request packet to host2 (target), and let it process.
  network_->Deliver(kNetworkDelay);
  env_->RunFor(kNetworkDelay + kFalconProcessingDelay);

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

  // Check correctness of counters.
  FalconConnectionCounters initiator_counters =
      host1_->get_falcon()->get_stats_manager()->GetConnectionCounters(
          kHost1Scid);
  EXPECT_EQ(initiator_counters.push_request_from_ulp, 1);
  EXPECT_EQ(initiator_counters.completions_to_ulp, 1);
  EXPECT_EQ(initiator_counters.initiator_tx_push_solicited_request, 1);
  EXPECT_EQ(initiator_counters.initiator_rx_push_grant, 1);
  EXPECT_EQ(initiator_counters.initiator_tx_push_solicited_data, 1);
  EXPECT_EQ(initiator_counters.rx_acks, 1);
  EXPECT_EQ(initiator_counters.tx_packets, 2);
  EXPECT_EQ(initiator_counters.rx_packets, 2);

  FalconConnectionCounters target_counters =
      host2_->get_falcon()->get_stats_manager()->GetConnectionCounters(
          kHost2Scid);
  EXPECT_EQ(target_counters.target_rx_push_solicited_request, 1);
  EXPECT_EQ(target_counters.target_tx_push_grant, 1);
  EXPECT_EQ(target_counters.target_rx_push_solicited_data, 1);
  EXPECT_EQ(target_counters.push_data_to_ulp, 1);
  EXPECT_EQ(target_counters.acks_from_ulp, 1);
  EXPECT_EQ(target_counters.tx_acks, 1);
  EXPECT_EQ(target_counters.rx_packets, 2);
  EXPECT_EQ(target_counters.tx_packets, 2);

  // Run for an additional timeout delay duration and ensure no more packets
  // have been transmitted by either the initiator or target.
  env_->RunFor(kRetransmissionDelay);
  EXPECT_EQ(network_->HeldPacketCount(), 0);

  // Check if all the Falcon resources have been released on both the hosts.
  EXPECT_TRUE(host1_->CheckIfAllFalconResourcesReleasedForTesting());
  EXPECT_TRUE(host2_->CheckIfAllFalconResourcesReleasedForTesting());
  // Check if credits related to the RDMA managed Falcon resources are returned.
  EXPECT_TRUE(host1_->CheckIfAllRdmaManagedFalconResourcesReleasedForTesting());
  EXPECT_TRUE(host2_->CheckIfAllRdmaManagedFalconResourcesReleasedForTesting());
}

// Tests a single read request with ack coalesce/piggyback.
TEST_P(FalconComponentTest, TestSingleReadWithAckCoalesce) {
  // Initialize both the Falcon hosts.
  InitFalconHosts(
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion()));
  // Create and submit an RDMA Read request from host1 to host2.
  auto request_packet = CreateReadRequest(
      /*src_qp=*/kHost1QpId, /*dst_qp=*/kHost2QpId, /*src_cid=*/kHost1Scid,
      /*rsn=*/kRsn, /*op_size=*/32);
  host1_->SubmitTxTransaction(std::move(request_packet));
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

  OpaqueCookie cookie;
  // Ack the read request to Falcon host2 (target).
  host2_->SubmitAckTransaction(kHost2Scid, received_packet->rdma.rsn,
                               Packet::Syndrome::kAck);

  // Create a response packet and send it to Falcon host2 (target), let it
  // process.
  auto response_packet = CreateReadResponse(
      /*src_qp=*/kHost2QpId, /*dst_qp=*/kHost1QpId, /*src_cid=*/kHost2Scid,
      /*rsn=*/received_packet->rdma.rsn,
      /*sgl=*/std::move(received_packet->rdma.sgl));

  // Respond back to Falcon immediately, causing the Ack and PullData to be sent
  // back to the initiator on the packet.
  host2_->SubmitTxTransaction(std::move(response_packet));
  env_->RunFor(kFalconProcessingDelay);

  // Next, host2 should transmit a response packet.
  ASSERT_EQ(network_->HeldPacketCount(), 1);
  const Packet* p2 = network_->Peek();
  ASSERT_EQ(p2->packet_type, falcon::PacketType::kPullData);

  // Deliver the pull data packet from host2 to host1, and let it process.
  network_->Deliver(kNetworkDelay);
  env_->RunFor(kNetworkDelay + kFalconProcessingDelay);

  // RDMA on host1 should get the read response packet from Falcon.
  ASSERT_OK_AND_ASSIGN(auto packet, host1_->GetSubmittedTransaction());
  EXPECT_EQ(packet->falcon.dest_cid, kHost1Scid);
  EXPECT_EQ(packet->falcon.rsn, kRsn);
  EXPECT_EQ(packet->rdma.opcode, Packet::Rdma::Opcode::kReadResponseOnly);
  EXPECT_EQ(packet->packet_type, falcon::PacketType::kPullData);

  // Falcon on host1 should send out an ACK corresponding to the read response
  // packet from Falcon.
  env_->RunFor(kDefaultAckCoalescingTimer + 2 * kFalconProcessingDelay);
  EXPECT_EQ(network_->HeldPacketCount(), 1);

  // Verify that it is an ACK and deliver it back to target Falcon.
  const Packet* p4 = network_->Peek();
  ASSERT_EQ(p4->packet_type, falcon::PacketType::kAck);
  network_->Deliver(kNetworkDelay);
  env_->RunFor(kNetworkDelay + kFalconProcessingDelay);

  // Check correctness of counters.
  FalconConnectionCounters initiator_counters =
      host1_->get_falcon()->get_stats_manager()->GetConnectionCounters(
          kHost1Scid);
  EXPECT_EQ(initiator_counters.pull_request_from_ulp, 1);
  EXPECT_EQ(initiator_counters.pull_response_to_ulp, 1);
  EXPECT_EQ(initiator_counters.initiator_tx_pull_request, 1);
  EXPECT_EQ(initiator_counters.initiator_rx_pull_data, 1);
  EXPECT_EQ(initiator_counters.tx_packets, 2);
  EXPECT_EQ(initiator_counters.rx_packets, 1);

  FalconConnectionCounters target_counters =
      host2_->get_falcon()->get_stats_manager()->GetConnectionCounters(
          kHost2Scid);
  EXPECT_EQ(target_counters.target_rx_pull_request, 1);
  EXPECT_EQ(target_counters.pull_request_to_ulp, 1);
  EXPECT_EQ(target_counters.pull_response_from_ulp, 1);
  EXPECT_EQ(target_counters.target_tx_pull_data, 1);
  EXPECT_EQ(target_counters.rx_packets, 2);
  EXPECT_EQ(target_counters.tx_packets, 1);

  // Check if all the Falcon resources have been released on both the hosts.
  EXPECT_TRUE(host1_->CheckIfAllFalconResourcesReleasedForTesting());
  EXPECT_TRUE(host2_->CheckIfAllFalconResourcesReleasedForTesting());
  // Check if credits related to the RDMA managed Falcon resources are returned.
  EXPECT_TRUE(host1_->CheckIfAllRdmaManagedFalconResourcesReleasedForTesting());
  EXPECT_TRUE(host2_->CheckIfAllRdmaManagedFalconResourcesReleasedForTesting());
}

// Tests a single read request without ack coalesce/piggyback.
TEST_P(FalconComponentTest, TestSingleReadWithoutAckCoalesce) {
  // Initialize both the Falcon hosts.
  InitFalconHosts(
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion()));
  // Create and submit an RDMA Read request from host1 to host2.
  auto request_packet = CreateReadRequest(
      /*src_qp=*/kHost1QpId, /*dst_qp=*/kHost2QpId, /*src_cid=*/kHost1Scid,
      /*rsn=*/kRsn, /*op_size=*/32);
  host1_->SubmitTxTransaction(std::move(request_packet));
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

  OpaqueCookie cookie;
  // Ack the read request to Falcon host2 (target).
  host2_->SubmitAckTransaction(kHost2Scid, received_packet->rdma.rsn,
                               Packet::Syndrome::kAck);

  // Create a response packet and send it to Falcon host2 (target), let it
  // process.
  auto response_packet = CreateReadResponse(
      /*src_qp=*/kHost2QpId, /*dst_qp=*/kHost1QpId, /*src_cid=*/kHost2Scid,
      /*rsn=*/received_packet->rdma.rsn,
      /*sgl=*/std::move(received_packet->rdma.sgl));

  // Respond back to Falcon after at least ack_coalescing_timeout, causing the
  // Ack and PullData to be sent out separately in different packets.
  EXPECT_OK(env_->ScheduleEvent(kDefaultAckCoalescingTimer, [&]() {
    host2_->SubmitTxTransaction(std::move(response_packet));
  }));
  env_->RunFor(kDefaultAckCoalescingTimer + 2 * kFalconProcessingDelay);

  // Next, host2 should transmit two packets -- an ack, then a pull response.
  EXPECT_EQ(network_->HeldPacketCount(), 2);

  // Verify they are correct and deliver them back to initiator Falcon.
  const Packet* p2 = network_->Peek();
  ASSERT_EQ(p2->packet_type, falcon::PacketType::kAck);
  network_->Deliver(kNetworkDelay);
  env_->RunFor(kNetworkDelay + kFalconProcessingDelay);

  const Packet* p3 = network_->Peek();
  ASSERT_EQ(p3->packet_type, falcon::PacketType::kPullData);
  network_->Deliver(kNetworkDelay);
  env_->RunFor(kNetworkDelay + kFalconProcessingDelay);

  // RDMA on host1 should get the read response packet from Falcon.
  ASSERT_OK_AND_ASSIGN(auto packet, host1_->GetSubmittedTransaction());
  EXPECT_EQ(packet->falcon.dest_cid, kHost1Scid);
  EXPECT_EQ(packet->falcon.rsn, kRsn);
  EXPECT_EQ(packet->rdma.opcode, Packet::Rdma::Opcode::kReadResponseOnly);
  EXPECT_EQ(packet->packet_type, falcon::PacketType::kPullData);

  // Falcon on host1 should send out an ACK corresponding to the read response
  // packet from Falcon.
  env_->RunFor(kDefaultAckCoalescingTimer + 2 * kFalconProcessingDelay);
  EXPECT_EQ(network_->HeldPacketCount(), 1);

  // Verify that it is an ACK and deliver it back to target Falcon.
  const Packet* p4 = network_->Peek();
  ASSERT_EQ(p4->packet_type, falcon::PacketType::kAck);
  network_->Deliver(kNetworkDelay);
  env_->RunFor(kNetworkDelay + kFalconProcessingDelay);

  // Check correctness of counters.
  FalconConnectionCounters initiator_counters =
      host1_->get_falcon()->get_stats_manager()->GetConnectionCounters(
          kHost1Scid);
  EXPECT_EQ(initiator_counters.pull_request_from_ulp, 1);
  EXPECT_EQ(initiator_counters.pull_response_to_ulp, 1);
  EXPECT_EQ(initiator_counters.initiator_tx_pull_request, 1);
  EXPECT_EQ(initiator_counters.initiator_rx_pull_data, 1);
  EXPECT_EQ(initiator_counters.tx_packets, 2);
  EXPECT_EQ(initiator_counters.rx_packets, 2);

  FalconConnectionCounters target_counters =
      host2_->get_falcon()->get_stats_manager()->GetConnectionCounters(
          kHost2Scid);
  EXPECT_EQ(target_counters.target_rx_pull_request, 1);
  EXPECT_EQ(target_counters.pull_request_to_ulp, 1);
  EXPECT_EQ(target_counters.pull_response_from_ulp, 1);
  EXPECT_EQ(target_counters.target_tx_pull_data, 1);
  EXPECT_EQ(target_counters.rx_packets, 2);
  EXPECT_EQ(target_counters.tx_acks, 1);
  EXPECT_EQ(target_counters.tx_packets, 2);

  // Check if all the Falcon resources have been released on both the hosts.
  EXPECT_TRUE(host1_->CheckIfAllFalconResourcesReleasedForTesting());
  EXPECT_TRUE(host2_->CheckIfAllFalconResourcesReleasedForTesting());
  // Check if credits related to the RDMA managed Falcon resources are returned.
  EXPECT_TRUE(host1_->CheckIfAllRdmaManagedFalconResourcesReleasedForTesting());
  EXPECT_TRUE(host2_->CheckIfAllRdmaManagedFalconResourcesReleasedForTesting());
}

// Tests a single unsolicited write, where the request is dropped.
TEST_P(FalconComponentTest, TestSingleUnsolicitedWriteDropRequest) {
  // Initialize both the Falcon hosts.
  InitFalconHosts(
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion()));
  // Create and submit and RDMA Write request from host1 to host2.
  auto request_packet = CreateWriteRequest(
      /*src_qp=*/kHost1QpId, /*dst_qp=*/kHost2QpId, /*src_cid=*/kHost1Scid,
      /*rsn=*/kRsn, /*op_size=*/32);
  host1_->SubmitTxTransaction(std::move(request_packet));
  env_->RunFor(kFalconProcessingDelay);

  // At this point, host1 should transmit a unsolicited data packet.
  ASSERT_EQ(network_->HeldPacketCount(), 1);
  const Packet* p1 = network_->Peek();
  ASSERT_EQ(p1->packet_type, falcon::PacketType::kPushUnsolicitedData);

  // Drop the packet, let the simulation run for at least retransmission delay.
  network_->Drop();
  env_->RunFor(kRetransmissionDelay);

  // At this point, host1 should retransmit an unsolicited data packet.
  ASSERT_EQ(network_->HeldPacketCount(), 1);
  const Packet* p1_retransmit = network_->Peek();
  ASSERT_EQ(p1_retransmit->packet_type,
            falcon::PacketType::kPushUnsolicitedData);

  // Deliver the retransmitted data packet to host2. Host2 receives the packet
  // and sends Ack back. Host1 should get completion for the same write
  // transaction.
  DeliverPushUnsolicitedDataPacketAndCheckCompletionReceival(kRsn);

  // Check correctness of counters.
  FalconConnectionCounters initiator_counters =
      host1_->get_falcon()->get_stats_manager()->GetConnectionCounters(
          kHost1Scid);
  EXPECT_EQ(initiator_counters.push_request_from_ulp, 1);
  EXPECT_EQ(initiator_counters.completions_to_ulp, 1);
  EXPECT_EQ(initiator_counters.initiator_tx_push_unsolicited_data, 2);
  EXPECT_EQ(initiator_counters.rx_acks, 1);
  EXPECT_EQ(initiator_counters.tx_packets, 2);
  EXPECT_EQ(initiator_counters.rx_packets, 1);
  // Ensure that the retransmitted counter is 1.
  EXPECT_EQ(initiator_counters.tx_timeout_retransmitted, 1);

  FalconConnectionCounters target_counters =
      host2_->get_falcon()->get_stats_manager()->GetConnectionCounters(
          kHost2Scid);
  EXPECT_EQ(target_counters.target_rx_push_unsolicited_data, 1);
  EXPECT_EQ(target_counters.push_data_to_ulp, 1);
  EXPECT_EQ(target_counters.acks_from_ulp, 1);
  EXPECT_EQ(target_counters.tx_acks, 1);
  EXPECT_EQ(target_counters.rx_packets, 1);
  EXPECT_EQ(target_counters.tx_packets, 1);

  // Run for an additional timeout delay duration and ensure no more packets
  // have been transmitted by either the initiator or target.
  env_->RunFor(kRetransmissionDelay);
  EXPECT_EQ(network_->HeldPacketCount(), 0);

  // Check if all the Falcon resources have been released on both the hosts.
  EXPECT_TRUE(host1_->CheckIfAllFalconResourcesReleasedForTesting());
  EXPECT_TRUE(host2_->CheckIfAllFalconResourcesReleasedForTesting());
  // Check if credits related to the RDMA managed Falcon resources are returned.
  EXPECT_TRUE(host1_->CheckIfAllRdmaManagedFalconResourcesReleasedForTesting());
  EXPECT_TRUE(host2_->CheckIfAllRdmaManagedFalconResourcesReleasedForTesting());
}

// Tests a single unsolicited write, where the returning ack is dropped.
TEST_P(FalconComponentTest, TestSingleUnsolicitedWriteDropAck) {
  // Initialize both the Falcon hosts.
  InitFalconHosts(
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion()));
  // Create and submit an RDMA Write request from host1 to host2.
  auto request_packet = CreateWriteRequest(
      /*src_qp=*/kHost1QpId, /*dst_qp=*/kHost2QpId, /*src_cid=*/kHost1Scid,
      /*rsn=*/kRsn, /*op_size=*/32);
  host1_->SubmitTxTransaction(std::move(request_packet));
  env_->RunFor(kFalconProcessingDelay);

  // At this point, host1 should transmit a unsolicited data packet.
  ASSERT_EQ(network_->HeldPacketCount(), 1);
  const Packet* p1 = network_->Peek();
  ASSERT_EQ(p1->packet_type, falcon::PacketType::kPushUnsolicitedData);

  // Deliver data packet from host1 to host2 and let it process.
  network_->Deliver(kNetworkDelay);
  env_->RunFor(kNetworkDelay + kFalconProcessingDelay);

  // RDMA on host2 should receive the same write transaction.
  ASSERT_OK_AND_ASSIGN(auto received_packet, host2_->GetSubmittedTransaction());

  OpaqueCookie cookie;
  // Ack the Write request to Falcon host2 (target), and let it process.
  host2_->SubmitAckTransaction(kHost2Scid, received_packet->rdma.rsn,
                               Packet::Syndrome::kAck);
  env_->RunFor(kDefaultAckCoalescingTimer + kFalconProcessingDelay);

  // Next, host2 should transmit an ack packet.
  ASSERT_EQ(network_->HeldPacketCount(), 1);
  const Packet* p2 = network_->Peek();
  ASSERT_EQ(p2->packet_type, falcon::PacketType::kAck);

  // Drop the ack packet, let the simulation run for retransmission delay.
  network_->Drop();
  env_->RunFor(kNetworkDelay + kRetransmissionDelay);

  // At this point, host1 should re-transmit the request.
  ASSERT_EQ(network_->HeldPacketCount(), 1);
  const Packet* p1_retransmit = network_->Peek();
  ASSERT_EQ(p1_retransmit->packet_type,
            falcon::PacketType::kPushUnsolicitedData);

  // Deliver the request packet to host2 (target), and let it process.
  network_->Deliver(kNetworkDelay);
  env_->RunFor(kNetworkDelay + kDefaultAckCoalescingTimer +
               kFalconProcessingDelay);

  // Next, host2 should re-transmit an ack packet, (this time without any
  // involvement from the ULP).
  ASSERT_EQ(network_->HeldPacketCount(), 1);
  const Packet* p2_retransmit = network_->Peek();
  ASSERT_EQ(p2_retransmit->packet_type, falcon::PacketType::kAck);

  // Deliver ack from host2 to host1, and let it process.
  network_->Deliver(kNetworkDelay);
  env_->RunFor(kNetworkDelay + kFalconProcessingDelay);

  // RDMA on host1 should get a completion from Falcon.
  ASSERT_OK_AND_ASSIGN(auto completion, host1_->GetCompletion());
  EXPECT_EQ(completion.first, kHost1QpId);
  EXPECT_EQ(completion.second, kRsn);

  // Check correctness of counters.
  FalconConnectionCounters initiator_counters =
      host1_->get_falcon()->get_stats_manager()->GetConnectionCounters(
          kHost1Scid);
  EXPECT_EQ(initiator_counters.push_request_from_ulp, 1);
  EXPECT_EQ(initiator_counters.completions_to_ulp, 1);
  EXPECT_EQ(initiator_counters.initiator_tx_push_unsolicited_data, 2);
  EXPECT_EQ(initiator_counters.rx_acks, 1);
  EXPECT_EQ(initiator_counters.tx_packets, 2);
  EXPECT_EQ(initiator_counters.rx_packets, 1);
  // Ensure that the retransmitted counter is 1.
  EXPECT_EQ(initiator_counters.tx_timeout_retransmitted, 1);

  FalconConnectionCounters target_counters =
      host2_->get_falcon()->get_stats_manager()->GetConnectionCounters(
          kHost2Scid);
  EXPECT_EQ(target_counters.target_rx_push_unsolicited_data, 2);
  EXPECT_EQ(target_counters.push_data_to_ulp, 1);
  EXPECT_EQ(target_counters.acks_from_ulp, 1);
  EXPECT_EQ(target_counters.tx_acks, 2);
  EXPECT_EQ(target_counters.rx_packets, 2);
  EXPECT_EQ(target_counters.tx_packets, 2);

  // Run for an additional timeout delay duration and ensure no more packets
  // have been transmitted by either the initiator or target.
  env_->RunFor(kRetransmissionDelay);
  EXPECT_EQ(network_->HeldPacketCount(), 0);

  // Check if all the Falcon resources have been released on both the hosts.
  EXPECT_TRUE(host1_->CheckIfAllFalconResourcesReleasedForTesting());
  EXPECT_TRUE(host2_->CheckIfAllFalconResourcesReleasedForTesting());
  // Check if credits related to the RDMA managed Falcon resources are returned.
  EXPECT_TRUE(host1_->CheckIfAllRdmaManagedFalconResourcesReleasedForTesting());
  EXPECT_TRUE(host2_->CheckIfAllRdmaManagedFalconResourcesReleasedForTesting());
}

// Tests multiple unsolicited writes, where the first request is dropped. This
// exercises the retransmit and reorder behavior in Falcon.
TEST_P(FalconComponentTest, TestMultipleUnsolicitedWriteDropRequest) {
  // Initialize both the Falcon hosts.
  InitFalconHosts(
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion()));
  auto params = GetParam();
  absl::Duration inter_host_rx_scheduler_tick =
      absl::Nanoseconds(std::get<1>(params));
  // Create and submit multiple RDMA Write requests from host1 to host2.
  auto request_packet0 = CreateWriteRequest(
      /*src_qp=*/kHost1QpId, /*dst_qp=*/kHost2QpId, /*src_cid=*/kHost1Scid,
      /*rsn=*/kRsn, /*op_size=*/32);

  auto request_packet1 = CreateWriteRequest(
      /*src_qp=*/kHost1QpId, /*dst_qp=*/kHost2QpId, /*src_cid=*/kHost1Scid,
      /*rsn=*/kRsn + 1, /*op_size=*/32);

  auto request_packet2 = CreateWriteRequest(
      /*src_qp=*/kHost1QpId, /*dst_qp=*/kHost2QpId, /*src_cid=*/kHost1Scid,
      /*rsn=*/kRsn + 2, /*op_size=*/32);

  // At time = 5ns, submits push unsolicited data (request_packet0). The phantom
  // request of request_packet0 will be removed and request_packet0 is marked as
  // eligible to send.
  EXPECT_OK(env_->ScheduleEvent(absl::Nanoseconds(5), [&]() {
    host1_->SubmitTxTransaction(std::move(request_packet0));
  }));
  // At time = 30ns, submits push unsolicited data (request_packet1). The
  // phantom request of request_packet1 will be removed and request_packet1 is
  // marked as eligible to send.
  EXPECT_OK(env_->ScheduleEvent(absl::Nanoseconds(30), [&]() {
    host1_->SubmitTxTransaction(std::move(request_packet1));
  }));
  // At time = 60ns, submits push unsolicited data (request_packet2). The
  // phantom request of request_packet2 will be removed and request_packet2 is
  // marked as eligible to send.
  EXPECT_OK(env_->ScheduleEvent(absl::Nanoseconds(60), [&]() {
    host1_->SubmitTxTransaction(std::move(request_packet2));
  }));
  env_->RunUntil(absl::Nanoseconds(60) + kFalconProcessingDelay);
  ASSERT_EQ(network_->HeldPacketCount(), 3);
  ASSERT_EQ(network_->PeekAt(0)->packet_type,
            falcon::PacketType::kPushUnsolicitedData);
  ASSERT_EQ(network_->PeekAt(0)->falcon.rsn, 0);
  ASSERT_EQ(network_->PeekAt(1)->packet_type,
            falcon::PacketType::kPushUnsolicitedData);
  ASSERT_EQ(network_->PeekAt(1)->falcon.rsn, 1);
  ASSERT_EQ(network_->PeekAt(2)->packet_type,
            falcon::PacketType::kPushUnsolicitedData);
  ASSERT_EQ(network_->PeekAt(2)->falcon.rsn, 2);

  // Drop the first packet, deliver the other 2 and let the simulation run for
  // at least retransmission delay.
  network_->Drop();
  network_->DeliverAll(kNetworkDelay);
  env_->RunFor(kRetransmissionDelay + kFalconProcessingDelay);

  // At this point host2 RDMA should not have received any requests (held by
  // reorder engine), but host1 should have retransmitted the first request.
  EXPECT_EQ(host2_->GetSubmittedTransaction().status(),
            absl::InternalError("No rx packets."));
  // These two are the E-ACK from host2, and retransmitted packet from host 1
  ASSERT_EQ(network_->HeldPacketCount(), 2);

  // Deliver the retransmitted request to host2.
  network_->DeliverAll(kNetworkDelay);
  env_->RunFor(kNetworkDelay + kFalconProcessingDelay);

  // RDMA on host2 should receive all 3 write requests. We ack all of them.
  ASSERT_OK_AND_ASSIGN(auto received_packet0,
                       host2_->GetSubmittedTransaction());
  EXPECT_EQ(received_packet0->rdma.rsn, kRsn);
  host2_->SubmitAckTransaction(kHost2Scid, received_packet0->rdma.rsn,
                               Packet::Syndrome::kAck);

  env_->RunFor(inter_host_rx_scheduler_tick);
  ASSERT_OK_AND_ASSIGN(auto received_packet1,
                       host2_->GetSubmittedTransaction());
  EXPECT_EQ(received_packet1->rdma.rsn, kRsn + 1);
  host2_->SubmitAckTransaction(kHost2Scid, received_packet1->rdma.rsn,
                               Packet::Syndrome::kAck);

  env_->RunFor(inter_host_rx_scheduler_tick);
  ASSERT_OK_AND_ASSIGN(auto received_packet2,
                       host2_->GetSubmittedTransaction());
  EXPECT_EQ(received_packet2->rdma.rsn, kRsn + 2);
  host2_->SubmitAckTransaction(kHost2Scid, received_packet2->rdma.rsn,
                               Packet::Syndrome::kAck);

  // Let Falcon process all the acks from RDMA.
  env_->RunFor(kDefaultAckCoalescingTimer + 3 * kFalconProcessingDelay);

  // Next, host2 should transmit a single (coalesced) ack packet.
  ASSERT_EQ(network_->HeldPacketCount(), 1);

  // Deliver ack from host2 to host1, and let it process.
  network_->Deliver(kNetworkDelay);
  env_->RunFor(kNetworkDelay + kFalconProcessingDelay);

  // RDMA on host1 should get multiple completions from Falcon.
  ASSERT_OK_AND_ASSIGN(auto completion1, host1_->GetCompletion());
  EXPECT_EQ(completion1.first, kHost1QpId);
  EXPECT_EQ(completion1.second, kRsn);
  env_->RunFor(inter_host_rx_scheduler_tick);

  ASSERT_OK_AND_ASSIGN(auto completion2, host1_->GetCompletion());
  EXPECT_EQ(completion2.first, kHost1QpId);
  EXPECT_EQ(completion2.second, kRsn + 1);
  env_->RunFor(inter_host_rx_scheduler_tick);

  ASSERT_OK_AND_ASSIGN(auto completion3, host1_->GetCompletion());
  EXPECT_EQ(completion3.first, kHost1QpId);
  EXPECT_EQ(completion3.second, kRsn + 2);

  // Run for an additional timeout delay duration and ensure no more packets
  // have been transmitted by either the initiator or target.
  env_->RunFor(kRetransmissionDelay);
  EXPECT_EQ(network_->HeldPacketCount(), 0);

  // Check correctness of counters.
  FalconConnectionCounters initiator_counters =
      host1_->get_falcon()->get_stats_manager()->GetConnectionCounters(
          kHost1Scid);
  EXPECT_EQ(initiator_counters.push_request_from_ulp, 3);
  EXPECT_EQ(initiator_counters.completions_to_ulp, 3);
  EXPECT_EQ(initiator_counters.initiator_tx_push_unsolicited_data, 4);
  EXPECT_EQ(initiator_counters.rx_acks, 2);
  EXPECT_EQ(initiator_counters.tx_packets, 4);
  EXPECT_EQ(initiator_counters.rx_packets, 2);
  EXPECT_EQ(initiator_counters.tx_timeout_retransmitted, 1);

  FalconConnectionCounters target_counters =
      host2_->get_falcon()->get_stats_manager()->GetConnectionCounters(
          kHost2Scid);
  EXPECT_EQ(target_counters.target_rx_push_unsolicited_data, 3);
  EXPECT_EQ(target_counters.push_data_to_ulp, 3);
  EXPECT_EQ(target_counters.acks_from_ulp, 3);
  EXPECT_EQ(target_counters.tx_acks, 2);
  EXPECT_EQ(target_counters.rx_packets, 3);
  EXPECT_EQ(target_counters.tx_packets, 2);

  // Run for an additional timeout delay duration and ensure no more packets
  // have been transmitted by either the initiator or target.
  env_->RunFor(kRetransmissionDelay);
  EXPECT_EQ(network_->HeldPacketCount(), 0);

  // Check if all the Falcon resources have been released on both the hosts.
  EXPECT_TRUE(host1_->CheckIfAllFalconResourcesReleasedForTesting());
  EXPECT_TRUE(host2_->CheckIfAllFalconResourcesReleasedForTesting());
}

// Tests multiple Reads, where the first read request is dropped. This verifies
// the retransmit and reorder behavior in Falcon for Reads.
TEST_P(FalconComponentTest, TestMultipleReadsDropRequest) {
  // Initialize both the Falcon hosts.
  InitFalconHosts(
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion()));
  auto params = GetParam();
  absl::Duration inter_host_rx_scheduler_tick =
      absl::Nanoseconds(std::get<1>(params));
  // Create and submit multiple RDMA Read requests from host1 to host2.
  auto request_packet0 = CreateReadRequest(
      /*src_qp=*/kHost1QpId, /*dst_qp=*/kHost2QpId, /*src_cid=*/kHost1Scid,
      /*rsn=*/kRsn, /*op_size=*/32);

  auto request_packet1 = CreateReadRequest(
      /*src_qp=*/kHost1QpId, /*dst_qp=*/kHost2QpId, /*src_cid=*/kHost1Scid,
      /*rsn=*/kRsn + 1, /*op_size=*/32);

  auto request_packet2 = CreateReadRequest(
      /*src_qp=*/kHost1QpId, /*dst_qp=*/kHost2QpId, /*src_cid=*/kHost1Scid,
      /*rsn=*/kRsn + 2, /*op_size=*/32);

  // Submit the transactions to Falcon host1 (initiator), and let it process.
  EXPECT_OK(env_->ScheduleEvent(absl::Nanoseconds(5), [&]() {
    host1_->SubmitTxTransaction(std::move(request_packet0));
  }));
  EXPECT_OK(env_->ScheduleEvent(absl::Nanoseconds(30), [&]() {
    host1_->SubmitTxTransaction(std::move(request_packet1));
  }));
  EXPECT_OK(env_->ScheduleEvent(absl::Nanoseconds(60), [&]() {
    host1_->SubmitTxTransaction(std::move(request_packet2));
  }));

  env_->RunUntil(absl::Nanoseconds(60) + kFalconProcessingDelay);

  // At this point, host1 should transmit 3 pull request packets.
  ASSERT_EQ(network_->HeldPacketCount(), 3);
  ASSERT_EQ(network_->PeekAt(0)->packet_type, falcon::PacketType::kPullRequest);
  ASSERT_EQ(network_->PeekAt(1)->packet_type, falcon::PacketType::kPullRequest);
  ASSERT_EQ(network_->PeekAt(2)->packet_type, falcon::PacketType::kPullRequest);

  // Drop the first packet, deliver the other 2 and let the simulation run for
  // at least retransmission delay.
  network_->Drop();
  network_->DeliverAll(kNetworkDelay);
  env_->RunFor(kRetransmissionDelay + kFalconProcessingDelay);

  // At this point host2 RDMA should not have received any requests (held by
  // reorder engine), and host1 should have retransmitted the first request.
  EXPECT_EQ(host2_->GetSubmittedTransaction().status(),
            absl::InternalError("No rx packets."));
  // This is the expected retransmitted packet from host1 and an ACK from host2.
  EXPECT_EQ(network_->HeldPacketCount(), 2);
  ASSERT_EQ(network_->PeekAt(0)->packet_type, falcon::PacketType::kAck);
  ASSERT_EQ(network_->PeekAt(1)->packet_type, falcon::PacketType::kPullRequest);

  // Deliver the retransmitted request to host2.
  network_->DeliverAll(kNetworkDelay);
  env_->RunFor(kNetworkDelay + kFalconProcessingDelay);

  // RDMA on host2 should receive all 3 read requests. We ack all of them.
  ASSERT_OK_AND_ASSIGN(auto received_packet0,
                       host2_->GetSubmittedTransaction());
  EXPECT_EQ(received_packet0->rdma.rsn, kRsn);
  host2_->SubmitAckTransaction(kHost2Scid, received_packet0->rdma.rsn,
                               Packet::Syndrome::kAck);

  env_->RunFor(inter_host_rx_scheduler_tick);
  ASSERT_OK_AND_ASSIGN(auto received_packet1,
                       host2_->GetSubmittedTransaction());
  EXPECT_EQ(received_packet1->rdma.rsn, kRsn + 1);
  host2_->SubmitAckTransaction(kHost2Scid, received_packet1->rdma.rsn,
                               Packet::Syndrome::kAck);

  env_->RunFor(inter_host_rx_scheduler_tick);
  ASSERT_OK_AND_ASSIGN(auto received_packet2,
                       host2_->GetSubmittedTransaction());
  EXPECT_EQ(received_packet2->rdma.rsn, kRsn + 2);
  host2_->SubmitAckTransaction(kHost2Scid, received_packet2->rdma.rsn,
                               Packet::Syndrome::kAck);

  // Let Falcon process all the acks from RDMA.
  env_->RunFor(kDefaultAckCoalescingTimer + 3 * kFalconProcessingDelay);

  env_->RunFor(inter_host_rx_scheduler_tick);
  // RDMA sends all 3 response packets to host2 Falcon at different times.
  auto response_packet0 = CreateReadResponse(
      /*src_qp=*/kHost2QpId, /*dst_qp=*/kHost1QpId, /*src_cid=*/kHost2Scid,
      /*rsn=*/received_packet0->rdma.rsn,
      /*sgl=*/std::move(received_packet0->rdma.sgl));
  auto response_packet1 = CreateReadResponse(
      /*src_qp=*/kHost2QpId, /*dst_qp=*/kHost1QpId, /*src_cid=*/kHost2Scid,
      /*rsn=*/received_packet1->rdma.rsn,
      /*sgl=*/std::move(received_packet1->rdma.sgl));
  auto response_packet2 = CreateReadResponse(
      /*src_qp=*/kHost2QpId, /*dst_qp=*/kHost1QpId, /*src_cid=*/kHost2Scid,
      /*rsn=*/received_packet2->rdma.rsn,
      /*sgl=*/std::move(received_packet2->rdma.sgl));

  EXPECT_OK(env_->ScheduleEvent(absl::Microseconds(1), [&]() {
    host2_->SubmitTxTransaction(std::move(response_packet0));
  }));
  EXPECT_OK(env_->ScheduleEvent(absl::Microseconds(5), [&]() {
    host2_->SubmitTxTransaction(std::move(response_packet1));
  }));
  EXPECT_OK(env_->ScheduleEvent(absl::Microseconds(10), [&]() {
    host2_->SubmitTxTransaction(std::move(response_packet2));
  }));

  env_->RunFor(absl::Microseconds(11));

  // Next, host2 should transmit all read responses (and an extra ack for
  // reflecting the delivery of 2nd and 3rd requests).
  ASSERT_EQ(network_->HeldPacketCount(), 4);
  ASSERT_EQ(network_->PeekAt(0)->packet_type, falcon::PacketType::kAck);
  ASSERT_EQ(network_->PeekAt(1)->packet_type, falcon::PacketType::kPullData);
  ASSERT_EQ(network_->PeekAt(2)->packet_type, falcon::PacketType::kPullData);
  ASSERT_EQ(network_->PeekAt(3)->packet_type, falcon::PacketType::kPullData);

  // Deliver ack from host2 to host1, and let it process.
  network_->DeliverAll(kNetworkDelay);
  env_->RunFor(kNetworkDelay + kFalconProcessingDelay);

  // RDMA on host1 should get pull responses from Falcon.
  ASSERT_OK_AND_ASSIGN(auto response1, host1_->GetSubmittedTransaction());
  EXPECT_EQ(response1->falcon.dest_cid, kHost1Scid);
  EXPECT_EQ(response1->falcon.rsn, kRsn);
  EXPECT_EQ(response1->rdma.opcode, Packet::Rdma::Opcode::kReadResponseOnly);
  EXPECT_EQ(response1->packet_type, falcon::PacketType::kPullData);

  ASSERT_OK_AND_ASSIGN(auto response2, host1_->GetSubmittedTransaction());
  EXPECT_EQ(response2->falcon.dest_cid, kHost1Scid);
  EXPECT_EQ(response2->falcon.rsn, kRsn + 1);
  EXPECT_EQ(response2->rdma.opcode, Packet::Rdma::Opcode::kReadResponseOnly);
  EXPECT_EQ(response2->packet_type, falcon::PacketType::kPullData);

  ASSERT_OK_AND_ASSIGN(auto response3, host1_->GetSubmittedTransaction());
  EXPECT_EQ(response3->falcon.dest_cid, kHost1Scid);
  EXPECT_EQ(response3->falcon.rsn, kRsn + 2);
  EXPECT_EQ(response3->rdma.opcode, Packet::Rdma::Opcode::kReadResponseOnly);
  EXPECT_EQ(response3->packet_type, falcon::PacketType::kPullData);

  // Run for an additional timeout delay duration to ensure host1 sends out an
  // ack for the data packets.
  env_->RunFor(kDefaultAckCoalescingTimer);
  EXPECT_EQ(network_->HeldPacketCount(), 1);
  ASSERT_EQ(network_->PeekAt(0)->packet_type, falcon::PacketType::kAck);
  network_->Deliver(kNetworkDelay);

  // Run for an additional timeout delay duration and ensure no more packets
  // have been transmitted by either the initiator or target.
  env_->RunFor(kRetransmissionDelay);
  EXPECT_EQ(network_->HeldPacketCount(), 0);

  // Check correctness of counters.
  FalconConnectionCounters initiator_counters =
      host1_->get_falcon()->get_stats_manager()->GetConnectionCounters(
          kHost1Scid);
  EXPECT_EQ(initiator_counters.pull_request_from_ulp, 3);
  EXPECT_EQ(initiator_counters.pull_response_to_ulp, 3);
  EXPECT_EQ(initiator_counters.initiator_tx_pull_request, 4);
  EXPECT_EQ(initiator_counters.rx_acks, 2);
  EXPECT_EQ(initiator_counters.tx_packets, 5);
  EXPECT_EQ(initiator_counters.rx_packets, 5);
  EXPECT_EQ(initiator_counters.tx_timeout_retransmitted, 1);

  FalconConnectionCounters target_counters =
      host2_->get_falcon()->get_stats_manager()->GetConnectionCounters(
          kHost2Scid);
  EXPECT_EQ(target_counters.target_rx_pull_request, 3);
  EXPECT_EQ(target_counters.pull_request_to_ulp, 3);
  EXPECT_EQ(target_counters.pull_response_from_ulp, 3);
  EXPECT_EQ(target_counters.acks_from_ulp, 3);
  EXPECT_EQ(target_counters.tx_acks, 2);
  EXPECT_EQ(target_counters.rx_packets, 4);
  EXPECT_EQ(target_counters.tx_packets, 5);

  // Run for an additional timeout delay duration and ensure no more packets
  // have been transmitted by either the initiator or target.
  env_->RunFor(kRetransmissionDelay);
  EXPECT_EQ(network_->HeldPacketCount(), 0);

  // Check if all the Falcon resources have been released on both the hosts.
  EXPECT_TRUE(host1_->CheckIfAllFalconResourcesReleasedForTesting());
  EXPECT_TRUE(host2_->CheckIfAllFalconResourcesReleasedForTesting());
  // Check if credits related to the RDMA managed Falcon resources are returned.
  EXPECT_TRUE(host1_->CheckIfAllRdmaManagedFalconResourcesReleasedForTesting());
  EXPECT_TRUE(host2_->CheckIfAllRdmaManagedFalconResourcesReleasedForTesting());
}

TEST_P(FalconComponentTest, TestFalconXoff) {
  // Initialize both the Falcon hosts.
  InitFalconHosts(
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion()));
  // Create and submit and RDMA Write request from host1 to host2.
  auto request_packet0 = CreateWriteRequest(
      /*src_qp=*/kHost1QpId, /*dst_qp=*/kHost2QpId, /*src_cid=*/kHost1Scid,
      /*rsn=*/kRsn, /*op_size=*/32);
  // host1 is Xon, it should be able to submit the TX transaction to the
  // network.
  host1_->SubmitTxTransaction(std::move(request_packet0));
  env_->RunFor(kFalconProcessingDelay);

  // At this point, host1 should transmit a unsolicited data packet.
  ASSERT_EQ(network_->HeldPacketCount(), 1);
  const Packet* p1 = network_->Peek();
  ASSERT_EQ(p1->packet_type, falcon::PacketType::kPushUnsolicitedData);
  ASSERT_EQ(p1->falcon.rsn, kRsn);

  // Triggers Xoff in host1, so after this point host1 can not submit packets to
  // the network.
  host1_->SetFalconXoff(true);
  auto request_packet1 = CreateReadRequest(
      /*src_qp=*/kHost1QpId, /*dst_qp=*/kHost2QpId, /*src_cid=*/kHost1Scid,
      /*rsn=*/kRsn + 1, /*op_size=*/32);
  host1_->SubmitTxTransaction(std::move(request_packet1));

  // Let the simulation run for at least retransmission delay without devliering
  // request_packet0.
  env_->RunFor(kRetransmissionDelay);

  // host1 is Xoff, so the retransmission of request_packet0 and request_packet1
  // should not be submitted to the network.
  ASSERT_EQ(network_->HeldPacketCount(), 1);
  ASSERT_EQ(network_->Peek()->falcon.rsn, kRsn);

  // Triggers Xoff on host2, so host2 should not submit any packets to the
  // network.
  host2_->SetFalconXoff(true);
  // Deliver request_packet0 to host2 (target), and let it process.
  network_->Deliver(absl::ZeroDuration());
  env_->RunFor(kFalconProcessingDelay);

  // RDMA on host2 should receive the same write transaction.
  ASSERT_OK_AND_ASSIGN(auto received_packet, host2_->GetSubmittedTransaction());

  OpaqueCookie cookie;
  // Ack the Write request to Falcon host2 (target), and let it process.
  host2_->SubmitAckTransaction(kHost2Scid, received_packet->rdma.rsn,
                               Packet::Syndrome::kAck);
  // At this point, host2 should generate an ACK packet. But host2 is Xoff, so
  // the ACK can not be submitted to the network.
  env_->RunFor(kDefaultAckCoalescingTimer + kFalconProcessingDelay);
  ASSERT_EQ(network_->HeldPacketCount(), 0);

  // Triggers Xon on host2, so that the ACK can be submitted to the network.
  host2_->SetFalconXoff(false);
  env_->RunFor(kFalconProcessingDelay / 2);
  ASSERT_EQ(network_->HeldPacketCount(), 1);
  ASSERT_EQ(network_->Peek()->packet_type, falcon::PacketType::kAck);
  // Drops the ACK from host2.
  network_->Drop();

  // Triggers Xon on host1, so that request_packet1 and the retransmission
  // should be able to submit to the network.
  host1_->SetFalconXoff(false);

  // host1 submited a packet from connection scheduler at the beginning. So the
  // RR arbiter should pick the packet from retransmission scheduler and the new
  // read request from the connection scheduler.
  env_->RunFor(kFalconProcessingDelay / 2 + kPerConnectionInterOpGap);
  ASSERT_EQ(network_->HeldPacketCount(), 2);
  ASSERT_EQ(network_->Peek()->packet_type,
            falcon::PacketType::kPushUnsolicitedData);
  ASSERT_EQ(network_->Peek()->falcon.rsn, kRsn);
  network_->Drop();

  // Submites request_packet1 in the connection scheduler.
  env_->RunFor(kFalconProcessingDelay / 2);
  ASSERT_EQ(network_->HeldPacketCount(), 1);
  ASSERT_EQ(network_->Peek()->packet_type, falcon::PacketType::kPullRequest);
  ASSERT_EQ(network_->Peek()->falcon.rsn, kRsn + 1);
}

TEST_P(FalconComponentTest, TestFalconWrrArbiter) {
  // Initialize both the Falcon hosts.
  InitFalconHosts(
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion()));
  auto request_packet0 = CreateWriteRequest(
      /*src_qp=*/kHost1QpId, /*dst_qp=*/kHost2QpId, /*src_cid=*/kHost1Scid,
      /*rsn=*/kRsn, /*op_size=*/32);
  host1_->SubmitTxTransaction(std::move(request_packet0));
  env_->RunFor(kFalconProcessingDelay + kPerConnectionInterOpGap);

  auto request_packet1 = CreateWriteRequest(
      /*src_qp=*/kHost1QpId, /*dst_qp=*/kHost2QpId, /*src_cid=*/kHost1Scid,
      /*rsn=*/kRsn + 1, /*op_size=*/32);
  host1_->SubmitTxTransaction(std::move(request_packet1));
  env_->RunFor(kFalconProcessingDelay);
  // Triggers Xoff in host1 so that it will not submit any packets to the
  // network.
  host1_->SetFalconXoff(true);

  host2_->SetFalconXoff(true);
  network_->Deliver(absl::ZeroDuration());
  env_->RunFor(kFalconProcessingDelay);
  // RDMA on host2 should receive the same write transaction.
  ASSERT_OK_AND_ASSIGN(auto received_packet0,
                       host2_->GetSubmittedTransaction());
  ASSERT_EQ(received_packet0->rdma.rsn, kRsn);
  // Ack the Write request to Falcon host2 (target), and let it process.
  host2_->SubmitAckTransaction(kHost2Scid, received_packet0->rdma.rsn,
                               Packet::Syndrome::kAck);
  // Generates ACK for received_packet0.
  env_->RunFor(kDefaultAckCoalescingTimer + kFalconProcessingDelay);

  network_->Deliver(absl::ZeroDuration());
  env_->RunFor(kFalconProcessingDelay);
  // RDMA on host2 should receive the same write transaction.
  ASSERT_OK_AND_ASSIGN(auto received_packet1,
                       host2_->GetSubmittedTransaction());
  ASSERT_EQ(received_packet1->rdma.rsn, kRsn + 1);
  // Ack the Write request to Falcon host2 (target), and let it process.
  host2_->SubmitAckTransaction(kHost2Scid, received_packet1->rdma.rsn,
                               Packet::Syndrome::kAck);
  // Generates ACK for received_packet1.
  env_->RunFor(kDefaultAckCoalescingTimer + kFalconProcessingDelay);

  auto request_packet2 = CreateReadRequest(
      /*src_qp=*/kHost2QpId, /*dst_qp=*/kHost1QpId, /*src_cid=*/kHost2Scid,
      /*rsn=*/kRsn + 2, /*op_size=*/32);
  host2_->SubmitTxTransaction(std::move(request_packet2));
  env_->RunFor(kFalconProcessingDelay);

  auto request_packet3 = CreateReadRequest(
      /*src_qp=*/kHost2QpId, /*dst_qp=*/kHost1QpId, /*src_cid=*/kHost2Scid,
      /*rsn=*/kRsn + 3, /*op_size=*/32);
  host2_->SubmitTxTransaction(std::move(request_packet3));
  env_->RunFor(kFalconProcessingDelay);

  host2_->SetFalconXoff(false);
  env_->RunFor(2 * kFalconProcessingDelay + kPerConnectionInterOpGap);
  ASSERT_EQ(network_->HeldPacketCount(), 4);
  // host2 has 2 pull requests (in connection scheduler) and 2 Acks (in ACK/NACK
  // scheduler) to send out. Given the weights for connection scheduler and ACK
  // scheduler are 1 and 2, resepctively, the WRR arbiter should arbitrate 1
  // packet from connection sheduler, then 2 packets from ACK scheduler, at last
  // 1 packet from connection scheduler.
  ASSERT_EQ(network_->PeekAt(0)->packet_type, falcon::PacketType::kPullRequest);
  ASSERT_EQ(network_->PeekAt(1)->packet_type, falcon::PacketType::kAck);
  ASSERT_EQ(network_->PeekAt(2)->packet_type, falcon::PacketType::kAck);
  ASSERT_EQ(network_->PeekAt(3)->packet_type, falcon::PacketType::kPullRequest);
}

// Tests multiple Reads, where the first read response is dropped. This verifies
// the retransmit and reorder behavior in Falcon for Reads on the initiator.
TEST_P(FalconComponentTest, TestMultipleReadsDropResponse) {
  // Initialize both the Falcon hosts.
  InitFalconHosts(
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion()));
  auto params = GetParam();
  absl::Duration inter_host_rx_scheduler_tick =
      absl::Nanoseconds(std::get<1>(params));
  env_->RunFor(inter_host_rx_scheduler_tick);
  // Create and submit multiple RDMA Read requests from host1 to host2.
  auto request_packet0 = CreateReadRequest(
      /*src_qp=*/kHost1QpId, /*dst_qp=*/kHost2QpId, /*src_cid=*/kHost1Scid,
      /*rsn=*/kRsn, /*op_size=*/32);

  auto request_packet1 = CreateReadRequest(
      /*src_qp=*/kHost1QpId, /*dst_qp=*/kHost2QpId, /*src_cid=*/kHost1Scid,
      /*rsn=*/kRsn + 1, /*op_size=*/32);

  auto request_packet2 = CreateReadRequest(
      /*src_qp=*/kHost1QpId, /*dst_qp=*/kHost2QpId, /*src_cid=*/kHost1Scid,
      /*rsn=*/kRsn + 2, /*op_size=*/32);

  // Submit the transactions to Falcon host1 (initiator), and let it process.
  EXPECT_OK(env_->ScheduleEvent(absl::Nanoseconds(5), [&]() {
    host1_->SubmitTxTransaction(std::move(request_packet0));
  }));
  EXPECT_OK(env_->ScheduleEvent(absl::Nanoseconds(30), [&]() {
    host1_->SubmitTxTransaction(std::move(request_packet1));
  }));
  EXPECT_OK(env_->ScheduleEvent(absl::Nanoseconds(60), [&]() {
    host1_->SubmitTxTransaction(std::move(request_packet2));
  }));

  env_->RunUntil(absl::Nanoseconds(60) + kFalconProcessingDelay);

  env_->RunFor(inter_host_rx_scheduler_tick);
  // At this point, host1 should transmit 3 pull request packets.
  ASSERT_EQ(network_->HeldPacketCount(), 3);
  ASSERT_EQ(network_->PeekAt(0)->packet_type, falcon::PacketType::kPullRequest);
  ASSERT_EQ(network_->PeekAt(1)->packet_type, falcon::PacketType::kPullRequest);
  ASSERT_EQ(network_->PeekAt(2)->packet_type, falcon::PacketType::kPullRequest);

  // Deliver all packets to host2.
  network_->DeliverAll(kNetworkDelay);
  env_->RunFor(kNetworkDelay + kFalconProcessingDelay);

  env_->RunFor(inter_host_rx_scheduler_tick);
  // RDMA on host2 should receive all 3 read requests. We ack all of them.
  ASSERT_OK_AND_ASSIGN(auto received_packet0,
                       host2_->GetSubmittedTransaction());
  EXPECT_EQ(received_packet0->rdma.rsn, kRsn);
  host2_->SubmitAckTransaction(kHost2Scid, received_packet0->rdma.rsn,
                               Packet::Syndrome::kAck);

  env_->RunFor(inter_host_rx_scheduler_tick);
  ASSERT_OK_AND_ASSIGN(auto received_packet1,
                       host2_->GetSubmittedTransaction());
  EXPECT_EQ(received_packet1->rdma.rsn, kRsn + 1);
  host2_->SubmitAckTransaction(kHost2Scid, received_packet1->rdma.rsn,
                               Packet::Syndrome::kAck);

  env_->RunFor(inter_host_rx_scheduler_tick);
  ASSERT_OK_AND_ASSIGN(auto received_packet2,
                       host2_->GetSubmittedTransaction());
  EXPECT_EQ(received_packet2->rdma.rsn, kRsn + 2);
  host2_->SubmitAckTransaction(kHost2Scid, received_packet2->rdma.rsn,
                               Packet::Syndrome::kAck);

  // Let Falcon process all the acks from RDMA.
  env_->RunFor(kDefaultAckCoalescingTimer + 3 * kFalconProcessingDelay);

  // Falcon host2 should return a coalesced ack for all 3 requests.
  EXPECT_EQ(network_->HeldPacketCount(), 1);
  ASSERT_EQ(network_->Peek()->packet_type, falcon::PacketType::kAck);
  network_->Deliver(kNetworkDelay);
  env_->RunFor(kNetworkDelay + kFalconProcessingDelay);

  // RDMA sends all 3 response packets to host2 Falcon at different times.
  auto response_packet0 = CreateReadResponse(
      /*src_qp=*/kHost2QpId, /*dst_qp=*/kHost1QpId, /*src_cid=*/kHost2Scid,
      /*rsn=*/received_packet0->rdma.rsn,
      /*sgl=*/std::move(received_packet0->rdma.sgl));
  auto response_packet1 = CreateReadResponse(
      /*src_qp=*/kHost2QpId, /*dst_qp=*/kHost1QpId, /*src_cid=*/kHost2Scid,
      /*rsn=*/received_packet1->rdma.rsn,
      /*sgl=*/std::move(received_packet1->rdma.sgl));
  auto response_packet2 = CreateReadResponse(
      /*src_qp=*/kHost2QpId, /*dst_qp=*/kHost1QpId, /*src_cid=*/kHost2Scid,
      /*rsn=*/received_packet2->rdma.rsn,
      /*sgl=*/std::move(received_packet2->rdma.sgl));

  EXPECT_OK(env_->ScheduleEvent(kRdmaProcessingDelay, [&]() {
    host2_->SubmitTxTransaction(std::move(response_packet0));
  }));
  EXPECT_OK(env_->ScheduleEvent(2 * kRdmaProcessingDelay, [&]() {
    host2_->SubmitTxTransaction(std::move(response_packet1));
  }));
  EXPECT_OK(env_->ScheduleEvent(3 * kRdmaProcessingDelay, [&]() {
    host2_->SubmitTxTransaction(std::move(response_packet2));
  }));

  env_->RunFor(3 * (kRdmaProcessingDelay + kPerConnectionInterOpGap +
                    kFalconProcessingDelay));

  // All 3 responses sent out.
  EXPECT_EQ(network_->HeldPacketCount(), 3);
  ASSERT_EQ(network_->PeekAt(0)->packet_type, falcon::PacketType::kPullData);
  ASSERT_EQ(network_->PeekAt(1)->packet_type, falcon::PacketType::kPullData);
  ASSERT_EQ(network_->PeekAt(2)->packet_type, falcon::PacketType::kPullData);

  // Drop the first response and deliver the other 2, run for RTO atleast.
  network_->Drop();
  network_->DeliverAll(kNetworkDelay);
  env_->RunFor(kNetworkDelay + kRetransmissionDelay + kFalconProcessingDelay);

  // Next, host1 should ack the 2 responses that were delivered and host2 should
  // re-transmit the first response.
  ASSERT_EQ(network_->HeldPacketCount(), 2);
  ASSERT_EQ(network_->PeekAt(0)->packet_type, falcon::PacketType::kAck);
  ASSERT_EQ(network_->PeekAt(1)->packet_type, falcon::PacketType::kPullData);

  // Deliver all packets.
  network_->DeliverAll(kNetworkDelay);
  env_->RunFor(kNetworkDelay + kFalconProcessingDelay);

  env_->RunFor(inter_host_rx_scheduler_tick);
  // RDMA on host1 should get multiple completions from Falcon.
  ASSERT_OK_AND_ASSIGN(auto response1, host1_->GetSubmittedTransaction());
  EXPECT_EQ(response1->falcon.dest_cid, kHost1Scid);
  EXPECT_EQ(response1->falcon.rsn, kRsn);
  EXPECT_EQ(response1->rdma.opcode, Packet::Rdma::Opcode::kReadResponseOnly);
  EXPECT_EQ(response1->packet_type, falcon::PacketType::kPullData);

  env_->RunFor(inter_host_rx_scheduler_tick);
  ASSERT_OK_AND_ASSIGN(auto response2, host1_->GetSubmittedTransaction());
  EXPECT_EQ(response2->falcon.dest_cid, kHost1Scid);
  EXPECT_EQ(response2->falcon.rsn, kRsn + 1);
  EXPECT_EQ(response2->rdma.opcode, Packet::Rdma::Opcode::kReadResponseOnly);
  EXPECT_EQ(response2->packet_type, falcon::PacketType::kPullData);

  env_->RunFor(inter_host_rx_scheduler_tick);
  ASSERT_OK_AND_ASSIGN(auto response3, host1_->GetSubmittedTransaction());
  EXPECT_EQ(response3->falcon.dest_cid, kHost1Scid);
  EXPECT_EQ(response3->falcon.rsn, kRsn + 2);
  EXPECT_EQ(response3->rdma.opcode, Packet::Rdma::Opcode::kReadResponseOnly);
  EXPECT_EQ(response3->packet_type, falcon::PacketType::kPullData);

  // Run for an additional ack coalescing delay to ensure host1 sends out an ack
  // for the data packets.
  env_->RunFor(kDefaultAckCoalescingTimer + kFalconProcessingDelay);
  EXPECT_EQ(network_->HeldPacketCount(), 1);
  ASSERT_EQ(network_->PeekAt(0)->packet_type, falcon::PacketType::kAck);
  network_->Deliver(kNetworkDelay);
  env_->RunFor(kNetworkDelay + kFalconProcessingDelay);

  // Run for an additional timeout delay duration and ensure no more packets
  // have been transmitted by either the initiator or target.
  env_->RunFor(kRetransmissionDelay + kFalconProcessingDelay);
  EXPECT_EQ(network_->HeldPacketCount(), 0);

  // Check correctness of counters.
  FalconConnectionCounters initiator_counters =
      host1_->get_falcon()->get_stats_manager()->GetConnectionCounters(
          kHost1Scid);
  EXPECT_EQ(initiator_counters.pull_request_from_ulp, 3);
  EXPECT_EQ(initiator_counters.pull_response_to_ulp, 3);
  EXPECT_EQ(initiator_counters.initiator_tx_pull_request, 3);
  EXPECT_EQ(initiator_counters.rx_acks, 1);
  EXPECT_EQ(initiator_counters.tx_packets, 5);
  EXPECT_EQ(initiator_counters.rx_packets, 4);
  EXPECT_EQ(initiator_counters.tx_timeout_retransmitted, 0);

  FalconConnectionCounters target_counters =
      host2_->get_falcon()->get_stats_manager()->GetConnectionCounters(
          kHost2Scid);
  EXPECT_EQ(target_counters.target_rx_pull_request, 3);
  EXPECT_EQ(target_counters.pull_request_to_ulp, 3);
  EXPECT_EQ(target_counters.pull_response_from_ulp, 3);
  EXPECT_EQ(target_counters.acks_from_ulp, 3);
  EXPECT_EQ(target_counters.tx_acks, 1);
  EXPECT_EQ(target_counters.rx_packets, 5);
  EXPECT_EQ(target_counters.tx_packets, 5);
  EXPECT_EQ(target_counters.tx_timeout_retransmitted, 1);

  // Run for an additional timeout delay duration and ensure no more packets
  // have been transmitted by either the initiator or target.
  env_->RunFor(kRetransmissionDelay);
  EXPECT_EQ(network_->HeldPacketCount(), 0);

  // Check if all the Falcon resources have been released on both the hosts.
  EXPECT_TRUE(host1_->CheckIfAllFalconResourcesReleasedForTesting());
  EXPECT_TRUE(host2_->CheckIfAllFalconResourcesReleasedForTesting());
  // Check if credits related to the RDMA managed Falcon resources are returned.
  EXPECT_TRUE(host1_->CheckIfAllRdmaManagedFalconResourcesReleasedForTesting());
  EXPECT_TRUE(host2_->CheckIfAllRdmaManagedFalconResourcesReleasedForTesting());
}

// Tests a single unsolicited write flow with resource exhaustion NACKs.
TEST_P(FalconComponentTest, TestUnsolicitedWriteWithResourceExhaustedNACKs) {
  // Initialize both the Falcon hosts.
  InitFalconHosts(
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion()));
  // Create and submit a Write request from host1 to host2.
  auto request_packet = CreateWriteRequest(
      /*src_qp=*/kHost1QpId, /*dst_qp=*/kHost2QpId, /*src_cid=*/kHost1Scid,
      /*rsn=*/kRsn, /*op_size=*/32);
  host1_->SubmitTxTransaction(std::move(request_packet));
  env_->RunFor(kFalconProcessingDelay);

  // At this point, host1 should transmit a unsolicited data packet.
  ASSERT_EQ(network_->HeldPacketCount(), 1);
  const Packet* p1 = network_->Peek();
  ASSERT_EQ(p1->packet_type, falcon::PacketType::kPushUnsolicitedData);

  // Decrease the Falcon resources of host2 (to trigger a resource exhausted
  // NACK).
  auto& host2_falcon_credits = down_cast<ProtocolResourceManager*>(
                                   host2_->get_falcon()->get_resource_manager())
                                   ->GetAvailableResourceCreditsForTesting();
  host2_falcon_credits.rx_packet_credits.network_requests = 0;

  // Deliver data packet from host1 to host2 and let it process.
  network_->Deliver(kNetworkDelay);
  env_->RunFor(kNetworkDelay + kFalconProcessingDelay);

  // At this point, host2 should transmit a resource exhaustion NACK.
  ASSERT_EQ(network_->HeldPacketCount(), 1);
  const Packet* p2 = network_->Peek();
  ASSERT_EQ(p2->packet_type, falcon::PacketType::kNack);
  ASSERT_EQ(p2->nack.code, falcon::NackCode::kRxResourceExhaustion);

  // Deliver resource exhaustion NACK to the host 1, and let it process.
  network_->Deliver(kNetworkDelay);
  env_->RunFor(kNetworkDelay + kFalconProcessingDelay);

  // Increase the Falcon resources of host2 so that it can accept transactions.
  host2_falcon_credits.rx_packet_credits.network_requests =
      std::numeric_limits<int>::max();

  // Wait until retransmission, and then check if host2 received the request.
  env_->RunFor(kRetransmissionDelay);

  // Deliver the retransmitted packet to host2. Host2 receives the packet
  // and sends Ack back. Host1 should get completion for the same write
  // transaction.
  DeliverPushUnsolicitedDataPacketAndCheckCompletionReceival(kRsn);

  // Check correctness of counters.
  FalconConnectionCounters initiator_counters =
      host1_->get_falcon()->get_stats_manager()->GetConnectionCounters(
          kHost1Scid);
  EXPECT_EQ(initiator_counters.push_request_from_ulp, 1);
  EXPECT_EQ(initiator_counters.completions_to_ulp, 1);
  EXPECT_EQ(initiator_counters.initiator_tx_push_unsolicited_data, 2);
  EXPECT_EQ(initiator_counters.rx_acks, 1);
  EXPECT_EQ(initiator_counters.rx_nacks, 1);
  EXPECT_EQ(initiator_counters.tx_packets, 2);
  EXPECT_EQ(initiator_counters.rx_packets, 2);

  FalconConnectionCounters target_counters =
      host2_->get_falcon()->get_stats_manager()->GetConnectionCounters(
          kHost2Scid);
  EXPECT_EQ(target_counters.target_rx_push_unsolicited_data, 2);
  EXPECT_EQ(target_counters.push_data_to_ulp, 1);
  EXPECT_EQ(target_counters.acks_from_ulp, 1);
  EXPECT_EQ(target_counters.tx_acks, 1);
  EXPECT_EQ(target_counters.tx_nacks, 1);
  EXPECT_EQ(target_counters.rx_packets, 2);
  EXPECT_EQ(target_counters.tx_packets, 2);

  // Run for an additional timeout delay duration and ensure no more packets
  // have been transmitted by either the initiator or target.
  env_->RunFor(kRetransmissionDelay);
  EXPECT_EQ(network_->HeldPacketCount(), 0);

  // Check if all the Falcon resources have been released on both the hosts.
  EXPECT_TRUE(host1_->CheckIfAllFalconResourcesReleasedForTesting());
  EXPECT_TRUE(host2_->CheckIfAllFalconResourcesReleasedForTesting());
  // Check if credits related to the RDMA managed Falcon resources are returned.
  EXPECT_TRUE(host1_->CheckIfAllRdmaManagedFalconResourcesReleasedForTesting());
  EXPECT_TRUE(host2_->CheckIfAllRdmaManagedFalconResourcesReleasedForTesting());
}

// Tests a single solicited write with resource exhaustion NACKs.
TEST_P(FalconComponentTest, TestSolicitedWriteWithResourceExhaustedNACKs) {
  // Initialize both the Falcon hosts.
  InitFalconHosts(
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion()));
  // Create and submit a Write request from host1 to host2.
  auto request_packet = CreateWriteRequest(
      /*src_qp=*/kHost1QpId, /*dst_qp=*/kHost2QpId, /*src_cid=*/kHost1Scid,
      /*rsn=*/kRsn, /*op_size=*/512);
  host1_->SubmitTxTransaction(std::move(request_packet));
  env_->RunFor(kFalconProcessingDelay);

  // At this point, host1 should transmit a request packet to the network.
  ASSERT_GE(network_->HeldPacketCount(), 1);
  const Packet* p1 = network_->Peek();
  ASSERT_EQ(p1->packet_type, falcon::PacketType::kPushRequest);

  // Decrease the Falcon resources of host2 (to trigger a resource exhausted
  // NACK).
  auto& host2_falcon_credits = down_cast<ProtocolResourceManager*>(
                                   host2_->get_falcon()->get_resource_manager())
                                   ->GetAvailableResourceCreditsForTesting();
  host2_falcon_credits.rx_packet_credits.network_requests = 0;

  // Deliver the request packet to host2, and let it process.
  network_->Deliver(kNetworkDelay);
  env_->RunFor(kNetworkDelay + kFalconProcessingDelay);

  // Next, host2 should have generated a NACK corresponding to the request
  // packet.
  ASSERT_EQ(network_->HeldPacketCount(), 1);
  const Packet* p2 = network_->Peek();
  ASSERT_EQ(p2->packet_type, falcon::PacketType::kNack);
  ASSERT_EQ(p2->nack.code, falcon::NackCode::kRxResourceExhaustion);

  // Deliver resource exhaustion NACK to the host 1, and let it process.
  network_->Deliver(kNetworkDelay);
  env_->RunFor(kNetworkDelay + kFalconProcessingDelay);

  // Increase the Falcon resources of host1.
  host2_falcon_credits.rx_packet_credits.network_requests =
      std::numeric_limits<int>::max();

  // Wait until retransmission, and then check if host1 retransmitted request
  // packet.
  env_->RunFor(kRetransmissionDelay);
  ASSERT_EQ(network_->HeldPacketCount(), 1);
  const Packet* p3 = network_->Peek();
  ASSERT_EQ(p3->packet_type, falcon::PacketType::kPushRequest);

  // Deliver the request packet to host 2 and let it process.
  network_->Deliver(kNetworkDelay);
  env_->RunFor(kNetworkDelay + kFalconProcessingDelay);

  // Next, host2 should transmit a grant packet.
  ASSERT_GE(network_->HeldPacketCount(), 1);
  const Packet* p4 = network_->Peek();
  ASSERT_EQ(p4->packet_type, falcon::PacketType::kPushGrant);
  env_->RunFor(kNetworkDelay + kFalconProcessingDelay);

  // Deliver the grant packet to host1 (initiator), and let it process.
  network_->Deliver(kNetworkDelay);
  env_->RunFor(kNetworkDelay + kFalconProcessingDelay);

  // Next, host1 should transmit a data packet.
  ASSERT_GE(network_->HeldPacketCount(), 1);
  const Packet* p5 = network_->Peek();
  ASSERT_EQ(p5->packet_type, falcon::PacketType::kPushSolicitedData);

  // Deliver the data packet to host2. Host2 receives the packet and sends Ack
  // back. Host1 should get completion for the same write transaction.
  DeliverPushUnsolicitedDataPacketAndCheckCompletionReceival(kRsn);

  // Check correctness of counters.
  FalconConnectionCounters initiator_counters =
      host1_->get_falcon()->get_stats_manager()->GetConnectionCounters(
          kHost1Scid);
  EXPECT_EQ(initiator_counters.push_request_from_ulp, 1);
  EXPECT_EQ(initiator_counters.completions_to_ulp, 1);
  EXPECT_EQ(initiator_counters.initiator_tx_push_solicited_request, 2);
  EXPECT_EQ(initiator_counters.initiator_rx_push_grant, 1);
  EXPECT_EQ(initiator_counters.initiator_tx_push_solicited_data, 1);
  EXPECT_EQ(initiator_counters.rx_acks, 1);
  EXPECT_EQ(initiator_counters.rx_nacks, 1);
  EXPECT_EQ(initiator_counters.tx_packets, 3);
  EXPECT_EQ(initiator_counters.rx_packets, 3);

  FalconConnectionCounters target_counters =
      host2_->get_falcon()->get_stats_manager()->GetConnectionCounters(
          kHost2Scid);
  EXPECT_EQ(target_counters.target_rx_push_solicited_request, 2);
  EXPECT_EQ(target_counters.target_tx_push_grant, 1);
  EXPECT_EQ(target_counters.target_rx_push_solicited_data, 1);
  EXPECT_EQ(target_counters.push_data_to_ulp, 1);
  EXPECT_EQ(target_counters.acks_from_ulp, 1);
  EXPECT_EQ(target_counters.tx_acks, 1);
  EXPECT_EQ(target_counters.tx_nacks, 1);
  EXPECT_EQ(target_counters.rx_packets, 3);
  EXPECT_EQ(target_counters.tx_packets, 3);

  // Run for an additional timeout delay duration and ensure no more packets
  // have been transmitted by either the initiator or target.
  env_->RunFor(kRetransmissionDelay);
  EXPECT_EQ(network_->HeldPacketCount(), 0);

  // Check if all the Falcon resources have been released on both the hosts.
  EXPECT_TRUE(host1_->CheckIfAllFalconResourcesReleasedForTesting());
  EXPECT_TRUE(host2_->CheckIfAllFalconResourcesReleasedForTesting());
  // Check if credits related to the RDMA managed Falcon resources are returned.
  EXPECT_TRUE(host1_->CheckIfAllRdmaManagedFalconResourcesReleasedForTesting());
  EXPECT_TRUE(host2_->CheckIfAllRdmaManagedFalconResourcesReleasedForTesting());
}

// Tests a single read request with resource exhaustion NACKs.
TEST_P(FalconComponentTest, TestSingleReadWithResourceExhaustedNACKs) {
  // Initialize both the Falcon hosts.
  InitFalconHosts(
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion()));
  // Create and submit an RDMA Read request from host1 to host2.
  auto request_packet = CreateReadRequest(
      /*src_qp=*/kHost1QpId, /*dst_qp=*/kHost2QpId, /*src_cid=*/kHost1Scid,
      /*rsn=*/kRsn, /*op_size=*/32);
  host1_->SubmitTxTransaction(std::move(request_packet));
  env_->RunFor(kFalconProcessingDelay);

  // At this point, host1 should transmit a pull request packet.
  ASSERT_EQ(network_->HeldPacketCount(), 1);
  const Packet* p1 = network_->Peek();
  ASSERT_EQ(p1->packet_type, falcon::PacketType::kPullRequest);

  // Decrease the Falcon resources of host2 (to trigger a resource exhausted
  // NACK).
  auto& host2_falcon_credits = down_cast<ProtocolResourceManager*>(
                                   host2_->get_falcon()->get_resource_manager())
                                   ->GetAvailableResourceCreditsForTesting();
  host2_falcon_credits.rx_packet_credits.network_requests = 0;

  // Deliver the pull request packet from host1 to host2 and let it process.
  network_->Deliver(kNetworkDelay);
  env_->RunFor(kNetworkDelay + kFalconProcessingDelay);

  // Host2 should have generated a NACK corresponding to the request packet.
  ASSERT_EQ(network_->HeldPacketCount(), 1);
  const Packet* p2 = network_->Peek();
  ASSERT_EQ(p2->packet_type, falcon::PacketType::kNack);
  ASSERT_EQ(p2->nack.code, falcon::NackCode::kRxResourceExhaustion);

  // Deliver resource exhaustion NACK to the host 1, and let it process.
  network_->Deliver(kNetworkDelay);
  env_->RunFor(kNetworkDelay + kFalconProcessingDelay);

  // Increase the Falcon resources of host1.
  host2_falcon_credits.rx_packet_credits.network_requests =
      std::numeric_limits<int>::max();

  // Wait until retransmission, and then check if host1 retransmitted request
  // packet.
  env_->RunFor(kRetransmissionDelay);
  ASSERT_EQ(network_->HeldPacketCount(), 1);
  const Packet* p3 = network_->Peek();
  ASSERT_EQ(p3->packet_type, falcon::PacketType::kPullRequest);

  // Deliver the pull request packet from host1 to host2 and let it process.
  network_->Deliver(kNetworkDelay);
  env_->RunFor(kNetworkDelay + kFalconProcessingDelay);

  // RDMA on host2 should receive the same read request transaction.
  ASSERT_OK_AND_ASSIGN(auto received_packet, host2_->GetSubmittedTransaction());

  OpaqueCookie cookie;
  // Ack the read request to Falcon host2 (target).
  host2_->SubmitAckTransaction(kHost2Scid, received_packet->rdma.rsn,
                               Packet::Syndrome::kAck);

  // Create a response packet and send it to Falcon host2 (target), let it
  // process.
  auto response_packet = CreateReadResponse(
      /*src_qp=*/kHost2QpId, /*dst_qp=*/kHost1QpId, /*src_cid=*/kHost2Scid,
      /*rsn=*/received_packet->rdma.rsn,
      /*sgl=*/std::move(received_packet->rdma.sgl));

  // Respond back to Falcon immediately, causing the Ack and PullData to be sent
  // back to the initiator on the packet.
  host2_->SubmitTxTransaction(std::move(response_packet));
  env_->RunFor(kFalconProcessingDelay);

  // Next, host2 should transmit a response packet.
  ASSERT_EQ(network_->HeldPacketCount(), 1);
  const Packet* p4 = network_->Peek();
  ASSERT_EQ(p4->packet_type, falcon::PacketType::kPullData);

  // Deliver the pull data packet from host2 to host1, and let it process.
  network_->Deliver(kNetworkDelay);
  env_->RunFor(kNetworkDelay + kFalconProcessingDelay);

  // RDMA on host1 should get the read response packet from Falcon.
  ASSERT_OK_AND_ASSIGN(auto packet, host1_->GetSubmittedTransaction());
  EXPECT_EQ(packet->falcon.dest_cid, kHost1Scid);
  EXPECT_EQ(packet->falcon.rsn, kRsn);
  EXPECT_EQ(packet->rdma.opcode, Packet::Rdma::Opcode::kReadResponseOnly);
  EXPECT_EQ(packet->packet_type, falcon::PacketType::kPullData);

  // Falcon on host1 should send out an ACK corresponding to the read response
  // packet from Falcon.
  env_->RunFor(kDefaultAckCoalescingTimer + 2 * kFalconProcessingDelay);
  EXPECT_EQ(network_->HeldPacketCount(), 1);

  // Verify that it is an ACK and deliver it back to target Falcon.
  const Packet* p5 = network_->Peek();
  ASSERT_EQ(p5->packet_type, falcon::PacketType::kAck);
  network_->Deliver(kNetworkDelay);
  env_->RunFor(kNetworkDelay + kFalconProcessingDelay);

  // Check correctness of counters.
  FalconConnectionCounters initiator_counters =
      host1_->get_falcon()->get_stats_manager()->GetConnectionCounters(
          kHost1Scid);
  EXPECT_EQ(initiator_counters.pull_request_from_ulp, 1);
  EXPECT_EQ(initiator_counters.pull_response_to_ulp, 1);
  EXPECT_EQ(initiator_counters.initiator_tx_pull_request, 2);
  EXPECT_EQ(initiator_counters.initiator_rx_pull_data, 1);
  EXPECT_EQ(initiator_counters.tx_packets, 3);
  EXPECT_EQ(initiator_counters.rx_packets, 2);

  FalconConnectionCounters target_counters =
      host2_->get_falcon()->get_stats_manager()->GetConnectionCounters(
          kHost2Scid);
  EXPECT_EQ(target_counters.target_rx_pull_request, 2);
  EXPECT_EQ(target_counters.pull_request_to_ulp, 1);
  EXPECT_EQ(target_counters.pull_response_from_ulp, 1);
  EXPECT_EQ(target_counters.target_tx_pull_data, 1);
  EXPECT_EQ(target_counters.rx_packets, 3);
  EXPECT_EQ(target_counters.tx_packets, 2);

  // Check if all the Falcon resources have been released on both the hosts.
  EXPECT_TRUE(host1_->CheckIfAllFalconResourcesReleasedForTesting());
  EXPECT_TRUE(host2_->CheckIfAllFalconResourcesReleasedForTesting());
}

// Tests if solicited data and unsolicited data are in RSN order in ordered
// connections.
TEST_P(FalconComponentTest, TestSolicitedAndUnsolicitedDataOrdering) {
  // Initialize both the Falcon hosts.
  InitFalconHosts(
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion()));
  // Creates a push solicited request with rsn = 0.
  auto solicited_request_packet = CreateWriteRequest(
      /*src_qp=*/kHost1QpId, /*dst_qp=*/kHost2QpId, /*src_cid=*/kHost1Scid,
      /*rsn=*/kRsn, /*op_size=*/512);
  // Once the solicited request is submitted, a corresponding solicited data
  // will be inserted in the data queue and marked as "hold".
  host1_->SubmitTxTransaction(std::move(solicited_request_packet));

  // Creates a push unsolicited data with rsn = 1. This unsolicited data should
  // not be transmitted until the push solicited data with rsn = 0 is
  // transmitted.
  auto unsolicited_data_packet = CreateWriteRequest(
      /*src_qp=*/kHost1QpId, /*dst_qp=*/kHost2QpId, /*src_cid=*/kHost1Scid,
      /*rsn=*/kRsn + 1, /*op_size=*/32);
  host1_->SubmitTxTransaction(std::move(unsolicited_data_packet));

  env_->RunFor(kFalconProcessingDelay * 2);
  // Host1 only transmits the solicited request to the network.
  ASSERT_EQ(network_->HeldPacketCount(), 1);
  ASSERT_EQ(network_->Peek()->packet_type, falcon::PacketType::kPushRequest);

  // Delivers the solicited request to host2.
  network_->Deliver(kNetworkDelay);
  env_->RunFor(kNetworkDelay + kFalconProcessingDelay);

  // Host2 processes the solicited request and transmits a grant packet.
  ASSERT_EQ(network_->HeldPacketCount(), 1);
  ASSERT_EQ(network_->Peek()->packet_type, falcon::PacketType::kPushGrant);

  // Delivers the push grant packet to host1.
  network_->Deliver(kNetworkDelay);
  env_->RunFor(kNetworkDelay + kFalconProcessingDelay +
               kPerConnectionInterOpGap);

  // Host1 can transmit a push solicited data with rsn = 0 after receiving
  // the grant packet, and then push unsolicited data with rsn = 1.
  ASSERT_EQ(network_->HeldPacketCount(), 2);
  ASSERT_EQ(network_->Peek()->packet_type,
            falcon::PacketType::kPushSolicitedData);
  ASSERT_EQ(network_->Peek()->falcon.rsn, 0);
  network_->Drop();

  ASSERT_EQ(network_->HeldPacketCount(), 1);
  ASSERT_EQ(network_->Peek()->packet_type,
            falcon::PacketType::kPushUnsolicitedData);
  ASSERT_EQ(network_->Peek()->falcon.rsn, 1);
}

// Tests if pull request with larger RSN is not allowed to bypass push
// unsolicited data with smaller RSN, and vice-versa (in absence of push
// request).
TEST_P(FalconComponentTest,
       TestPullRequestAndPushUnsolicitedDataOrderWithoutPushRequest) {
  // Initialize both the Falcon hosts.
  InitFalconHosts(
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion()));
  // To test that later pull request does not bypass earlier push unsolicited
  // data - host1 submits unsolicited_data_packet0 (rsn = 0),
  // unsolicited_data_packet1 (rsn = 1), and pull_request_packet0 (rsn = 2)
  // in order. The packet transmission sequence is expected to be in the same
  // order.
  auto unsolicited_data_packet0 = CreateWriteRequest(
      /*src_qp=*/kHost1QpId, /*dst_qp=*/kHost2QpId, /*src_cid=*/kHost1Scid,
      /*rsn=*/kRsn, /*op_size=*/32);
  host1_->SubmitTxTransaction(std::move(unsolicited_data_packet0));

  auto unsolicited_data_packet1 = CreateWriteRequest(
      /*src_qp=*/kHost1QpId, /*dst_qp=*/kHost2QpId,
      /*src_cid=*/kHost1Scid,
      /*rsn=*/kRsn + 1, /*op_size=*/32);
  host1_->SubmitTxTransaction(std::move(unsolicited_data_packet1));

  auto pull_request_packet0 = CreateReadRequest(
      /*src_qp=*/kHost1QpId, /*dst_qp=*/kHost2QpId, /*src_cid=*/kHost1Scid,
      /*rsn=*/kRsn + 2, /*op_size=*/32);
  host1_->SubmitTxTransaction(std::move(pull_request_packet0));

  env_->RunFor((kFalconProcessingDelay + kPerConnectionInterOpGap) * 3);
  ASSERT_EQ(network_->HeldPacketCount(), 3);

  ASSERT_EQ(network_->Peek()->packet_type,
            falcon::PacketType::kPushUnsolicitedData);
  ASSERT_EQ(network_->Peek()->falcon.rsn, 0);
  network_->Drop();

  ASSERT_EQ(network_->Peek()->packet_type,
            falcon::PacketType::kPushUnsolicitedData);
  ASSERT_EQ(network_->Peek()->falcon.rsn, 1);
  network_->Drop();

  ASSERT_EQ(network_->Peek()->packet_type, falcon::PacketType::kPullRequest);
  ASSERT_EQ(network_->Peek()->falcon.rsn, 2);
  network_->Drop();

  // To test that later push unsolicited data does not bypass earlier pull
  // request - host1 submits pull_request_packet1 (rsn = 3),
  // pull_request_packet2 (rsn = 4), and unsolicited_data_packet2 (rsn = 5)
  // in order. The packet transmission sequence is expected to be in the same
  // order.
  auto pull_request_packet1 = CreateReadRequest(
      /*src_qp=*/kHost1QpId, /*dst_qp=*/kHost2QpId, /*src_cid=*/kHost1Scid,
      /*rsn=*/kRsn + 3, /*op_size=*/32);
  host1_->SubmitTxTransaction(std::move(pull_request_packet1));

  auto pull_request_packet2 = CreateReadRequest(
      /*src_qp=*/kHost1QpId, /*dst_qp=*/kHost2QpId, /*src_cid=*/kHost1Scid,
      /*rsn=*/kRsn + 4, /*op_size=*/32);
  host1_->SubmitTxTransaction(std::move(pull_request_packet2));

  auto unsolicited_data_packet2 = CreateWriteRequest(
      /*src_qp=*/kHost1QpId, /*dst_qp=*/kHost2QpId,
      /*src_cid=*/kHost1Scid,
      /*rsn=*/kRsn + 5, /*op_size=*/32);
  host1_->SubmitTxTransaction(std::move(unsolicited_data_packet2));

  env_->RunFor((kFalconProcessingDelay + kPerConnectionInterOpGap) * 3);
  ASSERT_EQ(network_->HeldPacketCount(), 3);

  ASSERT_EQ(network_->Peek()->packet_type, falcon::PacketType::kPullRequest);
  ASSERT_EQ(network_->Peek()->falcon.rsn, 3);
  network_->Drop();

  ASSERT_EQ(network_->Peek()->packet_type, falcon::PacketType::kPullRequest);
  ASSERT_EQ(network_->Peek()->falcon.rsn, 4);
  network_->Drop();

  ASSERT_EQ(network_->Peek()->packet_type,
            falcon::PacketType::kPushUnsolicitedData);
  ASSERT_EQ(network_->Peek()->falcon.rsn, 5);
  network_->Drop();
}

// Tests if pull request with larger RSN can bypass push
// unsolicited data with smaller RSN (in presence of push request).
TEST_P(FalconComponentTest,
       TestPullRequestAndPushUnsolicitedDataOrderWithPushRequest) {
  // Initialize both the Falcon hosts.
  InitFalconHosts(
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion()));
  // host1 submits push_solicited_packet (rsn = 0),
  // unsolicited_data_packet (rsn = 1), and pull_request_packet (rsn = 2)
  // in order. The packet transmission sequence is expected to be: push
  // solicited request and pull request. This is because before push grant is
  // not received yet, the unsolicited data is blocked behind solicited data.
  auto push_solicited_packet = CreateWriteRequest(
      /*src_qp=*/kHost1QpId, /*dst_qp=*/kHost2QpId, /*src_cid=*/kHost1Scid,
      /*rsn=*/kRsn, /*op_size=*/512);
  host1_->SubmitTxTransaction(std::move(push_solicited_packet));

  auto unsolicited_data_packet = CreateWriteRequest(
      /*src_qp=*/kHost1QpId, /*dst_qp=*/kHost2QpId,
      /*src_cid=*/kHost1Scid,
      /*rsn=*/kRsn + 1, /*op_size=*/32);
  host1_->SubmitTxTransaction(std::move(unsolicited_data_packet));

  auto pull_request_packet = CreateReadRequest(
      /*src_qp=*/kHost1QpId, /*dst_qp=*/kHost2QpId, /*src_cid=*/kHost1Scid,
      /*rsn=*/kRsn + 2, /*op_size=*/32);
  host1_->SubmitTxTransaction(std::move(pull_request_packet));

  env_->RunFor((kFalconProcessingDelay + kPerConnectionInterOpGap) * 3);
  ASSERT_EQ(network_->HeldPacketCount(), 2);

  ASSERT_EQ(network_->Peek()->packet_type, falcon::PacketType::kPushRequest);
  ASSERT_EQ(network_->Peek()->falcon.rsn, 0);
  network_->Drop();

  ASSERT_EQ(network_->Peek()->packet_type, falcon::PacketType::kPullRequest);
  ASSERT_EQ(network_->Peek()->falcon.rsn, 2);
  network_->Drop();
}

// Tests if pull request and push request are in RSN order.
TEST_P(FalconComponentTest, TestPullRequestAndPushRequestOrdering) {
  // Initialize both the Falcon hosts.
  InitFalconHosts(
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion()));
  auto push_request_packet = CreateWriteRequest(
      /*src_qp=*/kHost1QpId, /*dst_qp=*/kHost2QpId, /*src_cid=*/kHost1Scid,
      /*rsn=*/kRsn + 1, /*op_size=*/512);
  host1_->SubmitTxTransaction(std::move(push_request_packet));

  auto pull_request_packet = CreateReadRequest(
      /*src_qp=*/kHost1QpId, /*dst_qp=*/kHost2QpId, /*src_cid=*/kHost1Scid,
      /*rsn=*/kRsn, /*op_size=*/32);
  host1_->SubmitTxTransaction(std::move(pull_request_packet));

  env_->RunFor(kFalconProcessingDelay * 2);
  // Pull request and push request go to the same queue, so they should be
  // transmitted in order.
  for (int i = 0; i < network_->HeldPacketCount(); i++) {
    ASSERT_EQ(network_->PeekAt(i)->falcon.rsn, i);
  }
}

// Tests if push request with larger RSN is allowed to bypass push
// solicited/unsolicited data with smaller RSN.
TEST_P(FalconComponentTest, TestPushRequestBypass) {
  // Initialize both the Falcon hosts.
  InitFalconHosts(
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion()));
  const int fcwnd = 32;
  for (int i = 0; i < fcwnd; i++) {
    auto unsolicited_data_packet = CreateWriteRequest(
        /*src_qp=*/kHost1QpId, /*dst_qp=*/kHost2QpId, /*src_cid=*/kHost1Scid,
        /*rsn=*/kRsn + i, /*op_size=*/32);
    host1_->SubmitTxTransaction(std::move(unsolicited_data_packet));
    env_->RunFor(kFalconProcessingDelay + kPerConnectionInterOpGap);
    network_->Drop();
  }

  // Creates an unsolicited data with rsn = 32. However, this unsolicited data
  // can not be transmitted due to fcwnd gating.
  auto unsolicited_data_packet = CreateWriteRequest(
      /*src_qp=*/kHost1QpId, /*dst_qp=*/kHost2QpId, /*src_cid=*/kHost1Scid,
      /*rsn=*/kRsn + fcwnd, /*op_size=*/32);
  host1_->SubmitTxTransaction(std::move(unsolicited_data_packet));

  // Creates a push request with rsn = 33. This push request is expected to be
  // transmitted even if its RSN is larger.
  auto push_request_packet = CreateWriteRequest(
      /*src_qp=*/kHost1QpId, /*dst_qp=*/kHost2QpId, /*src_cid=*/kHost1Scid,
      /*rsn=*/kRsn + fcwnd + 1, /*op_size=*/512);
  host1_->SubmitTxTransaction(std::move(push_request_packet));

  env_->RunFor(kFalconProcessingDelay * 2);
  ASSERT_EQ(network_->HeldPacketCount(), 1);
  ASSERT_EQ(network_->Peek()->packet_type, falcon::PacketType::kPushRequest);
  ASSERT_EQ(network_->Peek()->falcon.rsn, kRsn + fcwnd + 1);
}

TEST_P(FalconComponentTest, TestAckOnARbit) {
  // Initialize both the Falcon hosts.
  InitFalconHosts(
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion()));
  // Create and submit a Write request from host1 to host2.
  auto request_packet = CreateWriteRequest(
      /*src_qp=*/kHost1QpId, /*dst_qp=*/kHost2QpId, /*src_cid=*/kHost1Scid,
      /*rsn=*/kRsn, /*op_size=*/32);
  request_packet->falcon.ack_req = true;
  host1_->SubmitTxTransaction(std::move(request_packet));
  env_->RunFor(kFalconProcessingDelay);

  ASSERT_EQ(network_->HeldPacketCount(), 1);

  // Deliver the data packet to host2. Host2 receives the packet and sends Ack
  // back. Host1 should get completion for the same write transaction.
  DeliverPushUnsolicitedDataPacketAndCheckCompletionReceival(kRsn);

  // Check correctness of counters.
  FalconConnectionCounters initiator_counters =
      host1_->get_falcon()->get_stats_manager()->GetConnectionCounters(
          kHost1Scid);
  EXPECT_EQ(initiator_counters.push_request_from_ulp, 1);
  EXPECT_EQ(initiator_counters.completions_to_ulp, 1);
  EXPECT_EQ(initiator_counters.initiator_tx_push_unsolicited_data, 1);
  EXPECT_EQ(initiator_counters.rx_acks, 1);
  EXPECT_EQ(initiator_counters.tx_packets, 1);
  EXPECT_EQ(initiator_counters.rx_packets, 1);

  FalconConnectionCounters target_counters =
      host2_->get_falcon()->get_stats_manager()->GetConnectionCounters(
          kHost2Scid);
  EXPECT_EQ(target_counters.target_rx_push_unsolicited_data, 1);
  EXPECT_EQ(target_counters.push_data_to_ulp, 1);
  EXPECT_EQ(target_counters.acks_from_ulp, 1);
  EXPECT_EQ(target_counters.tx_acks, 1);
  EXPECT_EQ(target_counters.rx_packets, 1);
  EXPECT_EQ(target_counters.tx_packets, 1);

  // Run for an additional timeout delay duration and ensure no more packets
  // have been transmitted by either the initiator or target.
  env_->RunFor(kRetransmissionDelay);
  EXPECT_EQ(network_->HeldPacketCount(), 0);

  // Check if all the Falcon resources have been released on both the hosts.
  EXPECT_TRUE(host1_->CheckIfAllFalconResourcesReleasedForTesting());
  EXPECT_TRUE(host2_->CheckIfAllFalconResourcesReleasedForTesting());
  // Check if credits related to the RDMA managed Falcon resources are
  // returned.
  EXPECT_TRUE(host1_->CheckIfAllRdmaManagedFalconResourcesReleasedForTesting());
  EXPECT_TRUE(host2_->CheckIfAllRdmaManagedFalconResourcesReleasedForTesting());
}
TEST_P(FalconComponentTest, TestPushSolicitedDataWithoutGrantAck) {
  // Initialize both the Falcon hosts.
  InitFalconHosts(
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion()));
  // Create and submit a Write request from host1 to host2.
  auto request_packet1 = CreateWriteRequest(
      /*src_qp=*/kHost1QpId, /*dst_qp=*/kHost2QpId, /*src_cid=*/kHost1Scid,
      /*rsn=*/kRsn, /*op_size=*/512);
  host1_->SubmitTxTransaction(std::move(request_packet1));
  env_->RunFor(kFalconProcessingDelay);

  // At this point, host1 should transmit a request packet to the network.
  ASSERT_EQ(network_->HeldPacketCount(), 1);
  const Packet* p1 = network_->Peek();
  ASSERT_EQ(p1->packet_type, falcon::PacketType::kPushRequest);

  // Deliver the request packet.
  network_->DeliverAll(kNetworkDelay);
  env_->RunFor(kNetworkDelay + kFalconProcessingDelay);

  // Create another request from hos2 to host1.
  auto request_packet2 = CreateWriteRequest(
      /*src_qp=*/kHost2QpId, /*dst_qp=*/kHost1QpId, /*src_cid=*/kHost2Scid,
      /*rsn=*/kRsn, /*op_size=*/512);
  host2_->SubmitTxTransaction(std::move(request_packet2));

  env_->RunFor(kNetworkDelay + kFalconProcessingDelay);

  // Next, host2 should transmit a grant packet and the request packet.
  ASSERT_EQ(network_->HeldPacketCount(), 2);
  const Packet* p2 = network_->Peek();
  ASSERT_EQ(p2->packet_type, falcon::PacketType::kPushGrant);
  const Packet* p3 = network_->PeekAt(1);
  ASSERT_EQ(p3->packet_type, falcon::PacketType::kPushRequest);

  // Deliver the request and grant packet to host1, and let it process.
  network_->DeliverAll(kNetworkDelay);
  env_->RunFor(kNetworkDelay + kFalconProcessingDelay);

  // Next, host1 should transmit a data packet and a grant packet.
  ASSERT_EQ(network_->HeldPacketCount(), 2);
  const Packet* p4 = network_->Peek();
  ASSERT_EQ(p4->packet_type, falcon::PacketType::kPushSolicitedData);
  const Packet* p5 = network_->PeekAt(1);
  ASSERT_EQ(p5->packet_type, falcon::PacketType::kPushGrant);

  // Deliver the data packet to host2, and let it process.
  network_->DeliverAll(kNetworkDelay);
  env_->RunFor(kNetworkDelay + kFalconProcessingDelay);

  // RDMA on host2 should receive the data packet.
  ASSERT_OK_AND_ASSIGN(auto received_pkt1, host2_->GetSubmittedTransaction());

  // Next, host2 should transmit a data packet (which does not contain the ack
  // of the corresponding grant).
  ASSERT_EQ(network_->HeldPacketCount(), 1);
  const Packet* p6 = network_->Peek();
  ASSERT_EQ(p6->packet_type, falcon::PacketType::kPushSolicitedData);

  network_->DeliverAll(kNetworkDelay);
  env_->RunFor(kNetworkDelay + kFalconProcessingDelay);

  // RDMA on host1 should receive the data packet.
  ASSERT_OK_AND_ASSIGN(auto received_pkt2, host1_->GetSubmittedTransaction());

  // Ack the push data on host1 as well as host2.
  host1_->SubmitAckTransaction(kHost1Scid, received_pkt2->rdma.rsn,
                               Packet::Syndrome::kAck);
  host2_->SubmitAckTransaction(kHost2Scid, received_pkt1->rdma.rsn,
                               Packet::Syndrome::kAck);
  env_->RunFor(kNetworkDelay + kDefaultAckCoalescingTimer +
               kFalconProcessingDelay);

  // Next, host1 should transmit an ack packet.
  ASSERT_GE(network_->HeldPacketCount(), 2);
  const Packet* p7 = network_->Peek();
  ASSERT_EQ(p7->packet_type, falcon::PacketType::kAck);
  const Packet* p8 = network_->Peek();
  ASSERT_EQ(p8->packet_type, falcon::PacketType::kAck);

  // Deliver the acks to both hosts, and let it process.
  network_->DeliverAll(kNetworkDelay);
  env_->RunFor(kNetworkDelay + kFalconProcessingDelay);

  // RDMA on host1 should get a completion from Falcon.
  ASSERT_OK_AND_ASSIGN(auto completion1, host1_->GetCompletion());
  EXPECT_EQ(completion1.first, kHost1QpId);
  EXPECT_EQ(completion1.second, kRsn);

  // RDMA on host2 should get a completion from Falcon.
  ASSERT_OK_AND_ASSIGN(auto completion2, host2_->GetCompletion());
  EXPECT_EQ(completion2.first, kHost2QpId);
  EXPECT_EQ(completion2.second, kRsn);

  // Run for an additional timeout delay duration and ensure no more packets
  // have been transmitted by either the initiator or target.
  env_->RunFor(kRetransmissionDelay);
  EXPECT_EQ(network_->HeldPacketCount(), 0);

  // Check if all the Falcon resources have been released on both the hosts.
  EXPECT_TRUE(host1_->CheckIfAllFalconResourcesReleasedForTesting());
  EXPECT_TRUE(host2_->CheckIfAllFalconResourcesReleasedForTesting());
  // Check if credits related to the RDMA managed Falcon resources are
  // returned.
  EXPECT_TRUE(host1_->CheckIfAllRdmaManagedFalconResourcesReleasedForTesting());
  EXPECT_TRUE(host2_->CheckIfAllRdmaManagedFalconResourcesReleasedForTesting());
}

// Tests the behavior when a ULP is not ready and delivers RNR to the falcon in
// the first WRITE request. After a RNR timeout, receiver's falcon
// retries and successfully delivers the packet to ULP.
TEST_P(FalconComponentTest, TestUnsolicitedWriteWithRNR) {
  // Initialize both the Falcon hosts.
  InitFalconHosts(
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion()));

  // Creates and submit a Write request from host1 to host2.
  auto request_packet = CreateWriteRequest(
      /*src_qp=*/kHost1QpId, /*dst_qp=*/kHost2QpId, /*src_cid=*/kHost1Scid,
      /*rsn=*/0, /*op_size=*/32);
  host1_->SubmitTxTransaction(std::move(request_packet));
  env_->RunFor(kFalconProcessingDelay);

  // At this point, host1 should transmit a unsolicited data packet.
  ASSERT_EQ(network_->HeldPacketCount(), 1);
  ASSERT_EQ(network_->Peek()->packet_type,
            falcon::PacketType::kPushUnsolicitedData);

  // Delivers data packet from host1 to host2 and let it process.
  network_->Deliver(kNetworkDelay);
  env_->RunFor(kNetworkDelay + kFalconProcessingDelay);

  // RDMA on host2 should receive the same write transaction.
  ASSERT_OK_AND_ASSIGN(auto received_packet, host2_->GetSubmittedTransaction());

  // Submits Nack from ULP to host2's falcon, and let it process.
  host2_->SubmitAckTransaction(kHost2Scid, received_packet->rdma.rsn,
                               Packet::Syndrome::kRnrNak, kRnrTimeout);
  env_->RunFor(kFalconProcessingDelay);

  // For write requests, after host2 ULP sends back a RNR packet to host2
  // Falcon, there are two consequent operations:
  // (1) Host2 sends a Nack packet to host1. Host1 retries remotely after
  // kRnrTimeout.
  // (2) Host2 retries locally after kRnrTimeout.

  // Host2 should deliver the Nack packet to the network to host1 immediately.
  ASSERT_EQ(network_->HeldPacketCount(), 1);
  ASSERT_EQ(network_->Peek()->packet_type, falcon::PacketType::kNack);

  // We temporarily hold the Nack packet being sent to host 1, and let host 2
  // (the receiver) retry after a RNR timeout locally.
  env_->RunFor(kRnrTimeout);

  // RDMA on host2 should receive the same write transaction again after RNR
  // timeout.
  ASSERT_OK_AND_ASSIGN(auto received_packet2,
                       host2_->GetSubmittedTransaction());

  // Submits Ack from ULP to host2's falcon, and let it process.
  host2_->SubmitAckTransaction(kHost2Scid, received_packet2->rdma.rsn,
                               Packet::Syndrome::kAck);
  env_->RunFor(kFalconProcessingDelay + kDefaultAckCoalescingTimer);

  // In the network, there are two packets sent from host2: 1. Nack and 2. Ack.
  ASSERT_EQ(network_->HeldPacketCount(), 2);
  ASSERT_EQ(network_->PeekAt(0)->packet_type, falcon::PacketType::kNack);
  ASSERT_EQ(network_->PeekAt(1)->packet_type, falcon::PacketType::kAck);

  // The first Nack packet will be delivered in the 5th microsecond, followed by
  // an ACK packet after the 10th microsecond. A 5us delay will occur between
  // these two packets. During these 5us periods, 5 kRnrTimeouts (kRnrTimeout =
  // 1us) will be experienced, and so the initiator should retransmit 5 packets.
  network_->DeliverAll(kNetworkDelay);
  env_->RunFor(kNetworkDelay + kFalconProcessingDelay);

  // 5 retransmitted packets are sent from host1 with a period of 5us,
  // triggered by the initiator kRnrTimeout (kRnrTimeout = 1us).
  ASSERT_EQ(network_->HeldPacketCount(), 5);
  ASSERT_EQ(network_->PeekAt(0)->packet_type,
            falcon::PacketType::kPushUnsolicitedData);
  ASSERT_EQ(network_->PeekAt(1)->packet_type,
            falcon::PacketType::kPushUnsolicitedData);
  ASSERT_EQ(network_->PeekAt(2)->packet_type,
            falcon::PacketType::kPushUnsolicitedData);
  ASSERT_EQ(network_->PeekAt(3)->packet_type,
            falcon::PacketType::kPushUnsolicitedData);
  ASSERT_EQ(network_->PeekAt(4)->packet_type,
            falcon::PacketType::kPushUnsolicitedData);

  // RDMA on host1 should get a completion from Falcon.
  ASSERT_OK_AND_ASSIGN(auto completion, host1_->GetCompletion());
  EXPECT_EQ(completion.first, kHost1QpId);
  EXPECT_EQ(completion.second, kRsn);

  // Delivers all retransmitted packets.
  network_->DeliverAll(kNetworkDelay);
  env_->RunFor(kNetworkDelay + kFalconProcessingDelay);

  // Host2 sends back Ack to host1.
  ASSERT_EQ(network_->HeldPacketCount(), 1);
  ASSERT_EQ(network_->PeekAt(0)->packet_type, falcon::PacketType::kAck);
  network_->Deliver(kNetworkDelay);
  env_->RunFor(kNetworkDelay + kFalconProcessingDelay);

  // Runs for an additional timeout delay duration and ensure no more packets
  // have been transmitted by either the initiator or target.
  env_->RunFor(kRetransmissionDelay);
  EXPECT_EQ(network_->HeldPacketCount(), 0);

  // Checks if all the Falcon resources have been released on both the hosts.
  EXPECT_TRUE(host1_->CheckIfAllFalconResourcesReleasedForTesting());
  EXPECT_TRUE(host2_->CheckIfAllFalconResourcesReleasedForTesting());
  // Checks if credits related to the RDMA managed Falcon resources are
  // returned.
  EXPECT_TRUE(host1_->CheckIfAllRdmaManagedFalconResourcesReleasedForTesting());
  EXPECT_TRUE(host2_->CheckIfAllRdmaManagedFalconResourcesReleasedForTesting());

  // Checks correctness of counters.
  FalconConnectionCounters initiator_counters =
      host1_->get_falcon()->get_stats_manager()->GetConnectionCounters(
          kHost1Scid);
  EXPECT_EQ(initiator_counters.push_request_from_ulp, 1);
  EXPECT_EQ(initiator_counters.completions_to_ulp, 1);
  EXPECT_EQ(initiator_counters.initiator_tx_push_unsolicited_data, 6);
  EXPECT_EQ(initiator_counters.rx_acks, 2);
  EXPECT_EQ(initiator_counters.rx_nacks, 1);
  EXPECT_EQ(initiator_counters.rx_packets, 3);
  EXPECT_EQ(initiator_counters.tx_packets, 6);
  EXPECT_EQ(initiator_counters.tx_timeout_retransmitted, 5);

  FalconConnectionCounters target_counters =
      host2_->get_falcon()->get_stats_manager()->GetConnectionCounters(
          kHost2Scid);
  EXPECT_EQ(target_counters.target_rx_push_unsolicited_data, 6);
  EXPECT_EQ(target_counters.push_data_to_ulp, 2);
  EXPECT_EQ(target_counters.acks_from_ulp, 1);
  EXPECT_EQ(target_counters.nacks_from_ulp, 1);
  EXPECT_EQ(target_counters.tx_acks, 2);
  EXPECT_EQ(target_counters.tx_nacks, 1);
  EXPECT_EQ(target_counters.rx_packets, 6);
  EXPECT_EQ(target_counters.tx_packets, 3);
}

// Tests the behavior when the target ULP is not ready and delivers RNR to the
// target Falcon in a read request. After a RNR timeout, Falcon retries and
// succeeds in delivering the packet to the ULP.
TEST_P(FalconComponentTest, TestSingleReadRequestWithRNR) {
  // Initialize both the Falcon hosts.
  InitFalconHosts(
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion()));

  // Creates and submit an RDMA Read request from host1 to host2.
  auto request_packet = CreateReadRequest(
      /*src_qp=*/kHost1QpId, /*dst_qp=*/kHost2QpId, /*src_cid=*/kHost1Scid,
      /*rsn=*/kRsn, /*op_size=*/32);
  host1_->SubmitTxTransaction(std::move(request_packet));
  env_->RunFor(kFalconProcessingDelay);

  // At this point, host1 should transmit a pull request packet.
  ASSERT_EQ(network_->HeldPacketCount(), 1);
  ASSERT_EQ(network_->Peek()->packet_type, falcon::PacketType::kPullRequest);

  // Delivers the pull request packet from host1 to host2 and let it process.
  network_->Deliver(kNetworkDelay);
  env_->RunFor(kNetworkDelay + kFalconProcessingDelay);

  // RDMA on host2 should receive the same read request transaction.
  ASSERT_OK_AND_ASSIGN(auto received_packet1,
                       host2_->GetSubmittedTransaction());

  // ULP Nack the read request to host2's Falcon.
  host2_->SubmitAckTransaction(kHost2Scid, received_packet1->rdma.rsn,
                               Packet::Syndrome::kRnrNak, kRnrTimeout);

  // After a RNR timeout, the request retries.
  env_->RunFor(kFalconProcessingDelay + kRnrTimeout);

  // RDMA on host2 should receive the same read request transaction after
  // RnrTimeout. Note that we do not ACK the request here because we want to
  // test if host2 will send an ACK even though RDMA is not yet ready.
  ASSERT_OK_AND_ASSIGN(auto received_packet2,
                       host2_->GetSubmittedTransaction());

  // No packet in the network
  ASSERT_EQ(network_->HeldPacketCount(), 0);

  // Falcon on host2 should send an ACK corresponding to host1 for the read
  // request after kDefaultAckCoalescingTimer even though host2 RDMA replies RNR
  // to host2 Falcon.
  env_->RunFor(kDefaultAckCoalescingTimer + kFalconProcessingDelay);
  EXPECT_EQ(network_->HeldPacketCount(), 1);
  ASSERT_EQ(network_->Peek()->packet_type, falcon::PacketType::kAck);

  // Delivers the request ack from host2 to host1 and let it process.
  network_->Deliver(kNetworkDelay);
  env_->RunFor(kNetworkDelay + kFalconProcessingDelay);

  // Checks correctness of counters.
  FalconConnectionCounters initiator_counters =
      host1_->get_falcon()->get_stats_manager()->GetConnectionCounters(
          kHost1Scid);
  EXPECT_EQ(initiator_counters.pull_request_from_ulp, 1);
  EXPECT_EQ(initiator_counters.initiator_tx_pull_request, 1);
  EXPECT_EQ(initiator_counters.tx_packets, 1);
  EXPECT_EQ(initiator_counters.rx_packets, 1);

  FalconConnectionCounters target_counters =
      host2_->get_falcon()->get_stats_manager()->GetConnectionCounters(
          kHost2Scid);
  EXPECT_EQ(target_counters.target_rx_pull_request, 1);
  EXPECT_EQ(target_counters.pull_request_to_ulp, 2);
  EXPECT_EQ(target_counters.nacks_from_ulp, 1);
  EXPECT_EQ(target_counters.rx_packets, 1);
  EXPECT_EQ(target_counters.tx_packets, 1);
}

// Tests multiple write that all of which are dropped and validate that
// retransmissions are op-rate limited.
TEST_P(FalconComponentTest,
       TestPerConnectionOpRateLimitationDuringRetransmissions) {
  // Initialize both the Falcon hosts.
  InitFalconHosts(
      DefaultConfigGenerator::DefaultFalconConfig(GetFalconVersion()));
  auto params = GetParam();
  absl::Duration inter_host_rx_scheduler_tick =
      absl::Nanoseconds(std::get<2>(params));
  env_->RunFor(inter_host_rx_scheduler_tick);
  ASSERT_OK_AND_ASSIGN(
      auto connection_state,
      host1_->get_falcon()->get_state_manager()->PerformDirectLookup(
          kHost1Scid));
  CongestionControlMetadata& ccmeta =
      *connection_state->congestion_control_metadata;
  ccmeta.fabric_congestion_window = 131072;
  ccmeta.nic_congestion_window = 128;
  constexpr int kManyOps = 128;
  // Create and submit multiple RDMA Read requests from host1 to host2.
  for (int i = 0; i < kManyOps; ++i) {
    auto request_packet0 = CreateWriteRequest(
        /*src_qp=*/kHost1QpId, /*dst_qp=*/kHost2QpId, /*src_cid=*/kHost1Scid,
        /*rsn=*/i, /*op_size=*/8);
    // Submit the transactions to FALCON host1 (initiator), and let it process.
    host1_->SubmitTxTransaction(std::move(request_packet0));
  }
  ASSERT_EQ(0, network_->HeldPacketCount());

  // Run for 4 microseconds to make sure all packets are sent to the network.
  env_->RunFor(absl::Microseconds(4));
  ASSERT_EQ(network_->HeldPacketCount(), kManyOps);
  for (int i = 0; i < kManyOps; ++i) {
    ASSERT_EQ(network_->PeekAt(i)->packet_type,
              falcon::PacketType::kPushUnsolicitedData);
  }

  // Drop all packets except 1 packet so that an EACK can be generated and
  // retransmits all 127 packets.
  for (int i = 0; i < kManyOps - 1; ++i) {
    network_->Drop();
  }
  ASSERT_EQ(1, network_->HeldPacketCount());
  network_->Deliver(absl::ZeroDuration());
  ASSERT_EQ(0, network_->HeldPacketCount());

  env_->RunFor(kNetworkDelay + kFalconProcessingDelay);
  // An Eack will be sent
  ASSERT_EQ(1, network_->HeldPacketCount());
  ASSERT_EQ(network_->Peek()->packet_type, falcon::PacketType::kAck)
      << network_->Peek()->DebugString();
  // Make Sure it is an Eack.
  ASSERT_EQ(network_->Peek()->ack.ack_type, Packet::Ack::kEack)
      << network_->Peek()->DebugString();
  network_->Deliver(absl::ZeroDuration());
  ASSERT_EQ(0, network_->HeldPacketCount());

  // In 1 microsecond, 40 or fewer packets are retransmitted by host1, plus 1 if
  // a packet is transmitted at time 0 too.
  env_->RunFor(absl::Microseconds(1));
  ASSERT_LE(network_->HeldPacketCount(), 41);
  env_->RunFor(absl::Microseconds(1));
  ASSERT_LE(network_->HeldPacketCount(), 81);
  env_->RunFor(absl::Microseconds(1));
  ASSERT_LE(network_->HeldPacketCount(), 121);
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
