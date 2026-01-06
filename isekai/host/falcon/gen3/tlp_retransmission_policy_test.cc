#include <cstdint>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>

#include "absl/container/btree_set.h"
#include "absl/time/time.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/default_config_generator.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/common/packet.h"
#include "isekai/common/testing.h"
#include "isekai/host/falcon/falcon.h"
#include "isekai/host/falcon/falcon_connection_state.h"
#include "isekai/host/falcon/falcon_testing_helpers.h"
#include "isekai/host/falcon/gen1/connection_scheduler_types.h"
#include "isekai/host/falcon/gen1/falcon_types.h"

namespace isekai {
namespace {

class TlpRetransmissionPolicyTest
    : public FalconTestingHelpers::FalconTestSetup,
      public ::testing::Test {};

void CheckOutstandingPacketList(
    const absl::btree_set<RetransmissionWorkId> outstanding,
    std::vector<uint32_t> psn_list) {
  ASSERT_EQ(outstanding.size(), psn_list.size());
  int i = 0;
  for (auto it : outstanding) {
    ASSERT_EQ(it.psn, psn_list[i]);
    i++;
  }
}

TEST_F(TlpRetransmissionPolicyTest, TlpFirstUnreceivedTest) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(/*version=*/3);
  auto early_retx = config.mutable_early_retx();
  early_retx->set_ooo_distance_threshold(3);
  early_retx->set_enable_ooo_count(false);
  early_retx->set_enable_ooo_distance(false);
  // Run with RACK.
  early_retx->set_enable_rack(true);
  early_retx->set_enable_tlp(true);
  // Set rack window to 2us always.
  early_retx->set_rack_time_window_rtt_factor(0);
  early_retx->set_min_rack_time_window_ns(2000);
  // Set TLP timeout to 50us always.
  early_retx->set_tlp_timeout_rtt_factor(0);
  early_retx->set_min_tlp_timeout_ns(50000);
  InitFalcon(config);

  std::vector<Packet*> packet;
  ConnectionState* connection_state;
  // 10 PushUsData interleaves, 1000ns inter-packet gap.
  for (int i = 0; i < 10; i++) {
    packet.emplace_back();
    std::tie(packet[i], connection_state) =
        FalconTestingHelpers::SetupTransaction(
            falcon_.get(), TransactionType::kPushUnsolicited,
            TransactionLocation::kInitiator,
            TransactionState::kPushUnsolicitedReqUlpRx,
            falcon::PacketType::kPushUnsolicitedData, 1, i, i);
    EXPECT_OK(reliability_manager_->TransmitPacket(
        1, i, falcon::PacketType::kPushUnsolicitedData));
    env_.RunFor(absl::Nanoseconds(1000));
  }

  // At 20us, EACK arrives. 0, 1, 2, 3 are received.
  env_.RunFor(absl::Microseconds(20) - env_.ElapsedTime());
  auto eack = std::make_unique<Packet>();
  eack->packet_type = falcon::PacketType::kAck;
  eack->ack.ack_type = Packet::Ack::kEack;
  eack->ack.dest_cid = 1;
  eack->ack.rdbpsn = 0;
  eack->ack.rrbpsn = 0;
  eack->timestamps.received_timestamp = absl::Microseconds(20);
  eack->ack.timestamp_1 = absl::Microseconds(3);
  for (auto psn : {0, 1, 2, 3}) {
    eack->ack.received_bitmap.Set(psn, true);
  }
  EXPECT_OK(reliability_manager_->ReceivePacket(eack.get()));

  // TLP timer will not expire until 20+50us.
  env_.RunFor(absl::Microseconds(20 + 50) - env_.ElapsedTime() +
              absl::Nanoseconds(100));
  for (int i = 0; i < 10; i++) {
    auto transaction = connection_state->GetTransaction(
        TransactionKey(i, TransactionLocation::kInitiator));
    EXPECT_OK(transaction);
    auto packet_metadata = transaction.value()->GetPacketMetadata(
        falcon::PacketType::kPushUnsolicitedData);
    if (i == 4) {
      EXPECT_EQ(packet_metadata.value()->transmit_attempts, 2);
      EXPECT_EQ(packet_metadata.value()->early_retx_attempts, 1);
      EXPECT_GE(packet_metadata.value()->transmission_time,
                absl::Microseconds(70));
    } else {
      EXPECT_EQ(packet_metadata.value()->transmit_attempts, 1);
      EXPECT_EQ(packet_metadata.value()->early_retx_attempts, 0);
    }
  }
}

TEST_F(TlpRetransmissionPolicyTest, TlpFirstPerWindowTest) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(/*version=*/3);
  config.set_inter_host_rx_scheduling_tick_ns(0);
  auto early_retx = config.mutable_early_retx();
  early_retx->set_ooo_distance_threshold(3);
  early_retx->set_enable_ooo_count(false);
  early_retx->set_enable_ooo_distance(false);
  // Run with RACK.
  early_retx->set_enable_rack(true);
  early_retx->set_enable_tlp(true);
  // Set rack window to 2us always.
  early_retx->set_rack_time_window_rtt_factor(0);
  early_retx->set_min_rack_time_window_ns(2000);
  // Set TLP timeout to 50us always.
  early_retx->set_tlp_timeout_rtt_factor(0);
  early_retx->set_min_tlp_timeout_ns(50000);
  early_retx->set_tlp_type(FalconConfig::EarlyRetx::FIRST_UNRECEIVED);
  early_retx->set_tlp_per_window_probe(true);
  InitFalcon(config);

  std::vector<Packet*> packet;
  ConnectionState* connection_state;
  // 5 PullReq and 5 PushUsData interleaves, 1000ns inter-packet gap.
  for (int i = 0; i < 10; i++) {
    packet.emplace_back();
    if (i % 2 == 0) {
      std::tie(packet[i], connection_state) =
          FalconTestingHelpers::SetupTransaction(
              falcon_.get(), TransactionType::kPull,
              TransactionLocation::kInitiator, TransactionState::kPullReqUlpRx,
              falcon::PacketType::kPullRequest, 1, i, i / 2);
      EXPECT_OK(reliability_manager_->TransmitPacket(
          1, i, falcon::PacketType::kPullRequest));
    } else {
      std::tie(packet[i], connection_state) =
          FalconTestingHelpers::SetupTransaction(
              falcon_.get(), TransactionType::kPushUnsolicited,
              TransactionLocation::kInitiator,
              TransactionState::kPushUnsolicitedReqUlpRx,
              falcon::PacketType::kPushUnsolicitedData, 1, i, i / 2);
      EXPECT_OK(reliability_manager_->TransmitPacket(
          1, i, falcon::PacketType::kPushUnsolicitedData));
    }
    env_.RunFor(absl::Nanoseconds(1000));
  }

  auto& data_metadata =
      connection_state->tx_reliability_metadata.data_window_metadata;
  CheckOutstandingPacketList(data_metadata.outstanding_packets,
                             {0, 1, 2, 3, 4});
  auto& req_metadata =
      connection_state->tx_reliability_metadata.request_window_metadata;
  CheckOutstandingPacketList(req_metadata.outstanding_packets, {0, 1, 2, 3, 4});

  // TLP timer will not expire until 9+50us.
  env_.RunFor(absl::Microseconds(9 + 50) - env_.ElapsedTime() +
              absl::Nanoseconds(100));
  for (int i = 0; i < 10; i++) {
    auto transaction = connection_state->GetTransaction(
        TransactionKey(i, TransactionLocation::kInitiator));
    EXPECT_OK(transaction);
    auto packet_metadata = transaction.value()->GetPacketMetadata(
        i % 2 == 0 ? falcon::PacketType::kPullRequest
                   : falcon::PacketType::kPushUnsolicitedData);
    if (i == 0 || i == 1) {
      EXPECT_EQ(packet_metadata.value()->transmit_attempts, 2);
      EXPECT_EQ(packet_metadata.value()->early_retx_attempts, 1);
      EXPECT_GE(packet_metadata.value()->transmission_time,
                absl::Microseconds(59));
    } else {
      EXPECT_EQ(packet_metadata.value()->transmit_attempts, 1);
      EXPECT_EQ(packet_metadata.value()->early_retx_attempts, 0);
    }
  }
}

TEST_F(TlpRetransmissionPolicyTest, TlpLastUnreceivedTest) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(/*version=*/3);
  config.set_inter_host_rx_scheduling_tick_ns(0);
  auto early_retx = config.mutable_early_retx();
  early_retx->set_ooo_distance_threshold(3);
  early_retx->set_enable_ooo_count(false);
  early_retx->set_enable_ooo_distance(false);
  // Run with RACK.
  early_retx->set_enable_rack(true);
  early_retx->set_enable_tlp(true);
  // Set rack window to 2us always.
  early_retx->set_rack_time_window_rtt_factor(0);
  early_retx->set_min_rack_time_window_ns(2000);
  // Set TLP timeout to 50us always.
  early_retx->set_tlp_timeout_rtt_factor(0);
  early_retx->set_min_tlp_timeout_ns(50000);
  early_retx->set_tlp_type(FalconConfig::EarlyRetx::LAST_UNRECEIVED);
  early_retx->set_tlp_per_window_probe(true);
  InitFalcon(config);

  std::vector<Packet*> packet;
  ConnectionState* connection_state;
  // 5 PullReq and 5 PushUsData interleaves, 1000ns inter-packet gap.
  for (int i = 0; i < 10; i++) {
    packet.emplace_back();
    if (i % 2 == 0) {
      std::tie(packet[i], connection_state) =
          FalconTestingHelpers::SetupTransaction(
              falcon_.get(), TransactionType::kPull,
              TransactionLocation::kInitiator, TransactionState::kPullReqUlpRx,
              falcon::PacketType::kPullRequest, 1, i, i / 2);
      EXPECT_OK(reliability_manager_->TransmitPacket(
          1, i, falcon::PacketType::kPullRequest));
    } else {
      std::tie(packet[i], connection_state) =
          FalconTestingHelpers::SetupTransaction(
              falcon_.get(), TransactionType::kPushUnsolicited,
              TransactionLocation::kInitiator,
              TransactionState::kPushUnsolicitedReqUlpRx,
              falcon::PacketType::kPushUnsolicitedData, 1, i, i / 2);
      EXPECT_OK(reliability_manager_->TransmitPacket(
          1, i, falcon::PacketType::kPushUnsolicitedData));
    }
    env_.RunFor(absl::Nanoseconds(1000));
  }

  auto& data_metadata =
      connection_state->tx_reliability_metadata.data_window_metadata;
  CheckOutstandingPacketList(data_metadata.outstanding_packets,
                             {0, 1, 2, 3, 4});
  auto& req_metadata =
      connection_state->tx_reliability_metadata.request_window_metadata;
  CheckOutstandingPacketList(req_metadata.outstanding_packets, {0, 1, 2, 3, 4});

  // TLP timer will not expire until 9+50us.
  env_.RunFor(absl::Microseconds(9 + 50) - env_.ElapsedTime() +
              absl::Nanoseconds(100));
  for (int i = 0; i < 10; i++) {
    auto transaction = connection_state->GetTransaction(
        TransactionKey(i, TransactionLocation::kInitiator));
    EXPECT_OK(transaction);
    auto packet_metadata = transaction.value()->GetPacketMetadata(
        i % 2 == 0 ? falcon::PacketType::kPullRequest
                   : falcon::PacketType::kPushUnsolicitedData);
    if (i == 8 || i == 9) {
      EXPECT_EQ(packet_metadata.value()->transmit_attempts, 2);
      EXPECT_EQ(packet_metadata.value()->early_retx_attempts, 1);
      EXPECT_GE(packet_metadata.value()->transmission_time,
                absl::Microseconds(59));
    } else {
      EXPECT_EQ(packet_metadata.value()->transmit_attempts, 1);
      EXPECT_EQ(packet_metadata.value()->early_retx_attempts, 0);
    }
  }

  // At 70us, ACK arrives.
  env_.RunFor(absl::Microseconds(70) - env_.ElapsedTime());
  auto ack = std::make_unique<Packet>();
  ack->packet_type = falcon::PacketType::kAck;
  ack->ack.ack_type = Packet::Ack::kEack;
  ack->ack.dest_cid = 1;
  ack->ack.rdbpsn = 0;
  ack->ack.rrbpsn = 0;
  ack->ack.received_bitmap.Set(4, true);
  ack->timestamps.received_timestamp = absl::Microseconds(70);
  ack->ack.timestamp_1 = absl::Microseconds(59);
  EXPECT_OK(reliability_manager_->ReceivePacket(ack.get()));

  // All packets shall be retransmitted by RACK.
  env_.RunFor(absl::Nanoseconds(250));
  for (int i = 1; i < 10; i++) {
    auto transaction = connection_state->GetTransaction(
        TransactionKey(i, TransactionLocation::kInitiator));
    EXPECT_OK(transaction);
    auto packet_metadata = transaction.value()->GetPacketMetadata(
        i % 2 == 0 ? falcon::PacketType::kPullRequest
                   : falcon::PacketType::kPushUnsolicitedData);
    if (i == 8 || i == 9) {
      EXPECT_EQ(packet_metadata.value()->transmit_attempts, 2);
      EXPECT_EQ(packet_metadata.value()->early_retx_attempts, 1);
    } else {
      EXPECT_EQ(packet_metadata.value()->transmit_attempts, 2);
      EXPECT_EQ(packet_metadata.value()->early_retx_attempts, 1);
    }
  }

  // At 80us, ACK arrives with rrbpsn=2, rdbpsn=1
  env_.RunFor(absl::Microseconds(80) - env_.ElapsedTime());
  ack->packet_type = falcon::PacketType::kAck;
  ack->ack.ack_type = Packet::Ack::kAck;
  ack->ack.dest_cid = 1;
  ack->ack.rdbpsn = 1;
  ack->ack.rrbpsn = 2;
  ack->ack.received_bitmap.Set(4, false);
  ack->ack.received_bitmap.Set(3, true);
  ack->timestamps.received_timestamp = absl::Microseconds(80);
  ack->ack.timestamp_1 = absl::Microseconds(70);
  EXPECT_OK(reliability_manager_->ReceivePacket(ack.get()));

  // TLP timer expire at 80+50us.
  env_.RunFor(absl::Microseconds(80 + 50) - env_.ElapsedTime() +
              absl::Nanoseconds(100));
  for (int i = 3; i < 10; i++) {
    auto transaction = connection_state->GetTransaction(
        TransactionKey(i, TransactionLocation::kInitiator));
    auto packet_metadata = transaction.value()->GetPacketMetadata(
        i % 2 == 0 ? falcon::PacketType::kPullRequest
                   : falcon::PacketType::kPushUnsolicitedData);
    if (i == 7 || i == 8) {
      EXPECT_EQ(packet_metadata.value()->transmit_attempts, 3);
      EXPECT_EQ(packet_metadata.value()->early_retx_attempts, 2);
      EXPECT_GE(packet_metadata.value()->transmission_time,
                absl::Microseconds(130));
    } else {
      EXPECT_EQ(packet_metadata.value()->transmit_attempts, 2);
      EXPECT_EQ(packet_metadata.value()->early_retx_attempts, 1);
    }
  }
}

TEST_F(TlpRetransmissionPolicyTest, TlpLastPerWindowTest) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(/*version=*/3);
  config.set_inter_host_rx_scheduling_tick_ns(0);
  auto early_retx = config.mutable_early_retx();
  early_retx->set_ooo_distance_threshold(3);
  early_retx->set_enable_ooo_count(false);
  early_retx->set_enable_ooo_distance(false);
  // Run with RACK.
  early_retx->set_enable_rack(true);
  early_retx->set_enable_tlp(true);
  // Set rack window to 2us always.
  early_retx->set_rack_time_window_rtt_factor(0);
  early_retx->set_min_rack_time_window_ns(2000);
  // Set TLP timeout to 50us always.
  early_retx->set_tlp_timeout_rtt_factor(0);
  early_retx->set_min_tlp_timeout_ns(50000);
  early_retx->set_tlp_type(FalconConfig::EarlyRetx::LAST_UNRECEIVED);
  early_retx->set_tlp_per_window_probe(true);
  InitFalcon(config);

  std::vector<Packet*> packet;
  ConnectionState* connection_state;
  // 5 PullReq and 5 PushUsData interleaves, 1000ns inter-packet gap.
  for (int i = 0; i < 10; i++) {
    packet.emplace_back();
    if (i % 2 == 0) {
      std::tie(packet[i], connection_state) =
          FalconTestingHelpers::SetupTransaction(
              falcon_.get(), TransactionType::kPull,
              TransactionLocation::kInitiator, TransactionState::kPullReqUlpRx,
              falcon::PacketType::kPullRequest, 1, i, i / 2);
      EXPECT_OK(reliability_manager_->TransmitPacket(
          1, i, falcon::PacketType::kPullRequest));
    } else {
      std::tie(packet[i], connection_state) =
          FalconTestingHelpers::SetupTransaction(
              falcon_.get(), TransactionType::kPushUnsolicited,
              TransactionLocation::kInitiator,
              TransactionState::kPushUnsolicitedReqUlpRx,
              falcon::PacketType::kPushUnsolicitedData, 1, i, i / 2);
      EXPECT_OK(reliability_manager_->TransmitPacket(
          1, i, falcon::PacketType::kPushUnsolicitedData));
    }
    env_.RunFor(absl::Nanoseconds(1000));
  }

  auto& data_metadata =
      connection_state->tx_reliability_metadata.data_window_metadata;
  CheckOutstandingPacketList(data_metadata.outstanding_packets,
                             {0, 1, 2, 3, 4});
  auto& req_metadata =
      connection_state->tx_reliability_metadata.request_window_metadata;
  CheckOutstandingPacketList(req_metadata.outstanding_packets, {0, 1, 2, 3, 4});

  // TLP timer will not expire until 9+50us.
  env_.RunFor(absl::Microseconds(9 + 50) - env_.ElapsedTime() +
              absl::Nanoseconds(100));
  for (int i = 0; i < 10; i++) {
    auto transaction = connection_state->GetTransaction(
        TransactionKey(i, TransactionLocation::kInitiator));
    EXPECT_OK(transaction);
    auto packet_metadata = transaction.value()->GetPacketMetadata(
        i % 2 == 0 ? falcon::PacketType::kPullRequest
                   : falcon::PacketType::kPushUnsolicitedData);
    if (i == 8 || i == 9) {
      EXPECT_EQ(packet_metadata.value()->transmit_attempts, 2);
      EXPECT_EQ(packet_metadata.value()->early_retx_attempts, 1);
      EXPECT_GE(packet_metadata.value()->transmission_time,
                absl::Microseconds(59));
    } else {
      EXPECT_EQ(packet_metadata.value()->transmit_attempts, 1);
      EXPECT_EQ(packet_metadata.value()->early_retx_attempts, 0);
    }
  }
}

TEST_F(TlpRetransmissionPolicyTest, TlpLastPerWindowRangeLimitTest) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(/*version=*/3);
  auto early_retx = config.mutable_early_retx();
  early_retx->set_enable_ooo_count(false);
  early_retx->set_enable_ooo_distance(false);
  early_retx->set_enable_rack(false);
  early_retx->set_enable_tlp(true);
  // Set TLP timeout to 50us always.
  early_retx->set_tlp_timeout_rtt_factor(0);
  early_retx->set_min_tlp_timeout_ns(50000);
  early_retx->set_tlp_type(FalconConfig::EarlyRetx::LAST_UNRECEIVED);
  early_retx->set_tlp_per_window_probe(true);
  early_retx->set_tlp_bypass_scan_range_limit(false);
  InitFalcon(config);

  std::vector<Packet*> packet;
  ConnectionState* connection_state;
  // Using large packet count such that the scan PSN range limits (64/128) will
  // apply and affect TLP selection. Every other packet is PullReq/PushData.
  constexpr int kNumPackets = 400;
  for (int i = 0; i < kNumPackets; i++) {
    packet.emplace_back();
    if (i % 2 == 0) {
      std::tie(packet[i], connection_state) =
          FalconTestingHelpers::SetupTransaction(
              falcon_.get(), TransactionType::kPull,
              TransactionLocation::kInitiator, TransactionState::kPullReqUlpRx,
              falcon::PacketType::kPullRequest, 1, i, i / 2);
      EXPECT_OK(reliability_manager_->TransmitPacket(
          1, i, falcon::PacketType::kPullRequest));
    } else {
      std::tie(packet[i], connection_state) =
          FalconTestingHelpers::SetupTransaction(
              falcon_.get(), TransactionType::kPushUnsolicited,
              TransactionLocation::kInitiator,
              TransactionState::kPushUnsolicitedReqUlpRx,
              falcon::PacketType::kPushUnsolicitedData, 1, i, i / 2);
      EXPECT_OK(reliability_manager_->TransmitPacket(
          1, i, falcon::PacketType::kPushUnsolicitedData));
    }
    env_.RunFor(absl::Nanoseconds(200));
  }

  // TLP timer will expire after additional 50us TLP PTO duration.
  env_.RunFor(absl::Microseconds(50));
  for (int i = 0; i < kNumPackets; i++) {
    auto transaction = connection_state->GetTransaction(
        TransactionKey(i, TransactionLocation::kInitiator));
    EXPECT_OK(transaction);
    auto packet_metadata = transaction.value()->GetPacketMetadata(
        i % 2 == 0 ? falcon::PacketType::kPullRequest
                   : falcon::PacketType::kPushUnsolicitedData);
    // Last packet in each respective ACK bitmap range should be selected for
    // retransmission (request PSN 63, data PSN 127). All others should not have
    // an early retransmission reason set (default value kTimeout).
    if (i == 126 || i == 255) {
      EXPECT_EQ(packet_metadata.value()->retransmission_reason,
                RetransmitReason::kEarlyTlp);
    } else {
      EXPECT_EQ(packet_metadata.value()->retransmission_reason,
                RetransmitReason::kTimeout);
    }
  }
}

TEST_F(TlpRetransmissionPolicyTest, TlpLastPerWindowRangeLimitBypassTest) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(/*version=*/3);
  auto early_retx = config.mutable_early_retx();
  early_retx->set_enable_ooo_count(false);
  early_retx->set_enable_ooo_distance(false);
  early_retx->set_enable_rack(false);
  early_retx->set_enable_tlp(true);
  // Set TLP timeout to 50us always.
  early_retx->set_tlp_timeout_rtt_factor(0);
  early_retx->set_min_tlp_timeout_ns(50000);
  early_retx->set_tlp_type(FalconConfig::EarlyRetx::LAST_UNRECEIVED);
  early_retx->set_tlp_per_window_probe(true);
  early_retx->set_tlp_bypass_scan_range_limit(true);
  InitFalcon(config);

  std::vector<Packet*> packet;
  ConnectionState* connection_state;
  // Using large packet count such that the scan PSN range limits (64/128) would
  // apply and affect TLP selection (though limit is bypassed in this test).
  // Every other packet is PullReq/PushData.
  constexpr int kNumPackets = 400;
  for (int i = 0; i < kNumPackets; i++) {
    packet.emplace_back();
    if (i % 2 == 0) {
      std::tie(packet[i], connection_state) =
          FalconTestingHelpers::SetupTransaction(
              falcon_.get(), TransactionType::kPull,
              TransactionLocation::kInitiator, TransactionState::kPullReqUlpRx,
              falcon::PacketType::kPullRequest, 1, i, i / 2);
      EXPECT_OK(reliability_manager_->TransmitPacket(
          1, i, falcon::PacketType::kPullRequest));
    } else {
      std::tie(packet[i], connection_state) =
          FalconTestingHelpers::SetupTransaction(
              falcon_.get(), TransactionType::kPushUnsolicited,
              TransactionLocation::kInitiator,
              TransactionState::kPushUnsolicitedReqUlpRx,
              falcon::PacketType::kPushUnsolicitedData, 1, i, i / 2);
      EXPECT_OK(reliability_manager_->TransmitPacket(
          1, i, falcon::PacketType::kPushUnsolicitedData));
    }
    env_.RunFor(absl::Nanoseconds(200));
  }

  // TLP timer will expire after additional 50us TLP PTO duration.
  env_.RunFor(absl::Microseconds(50));
  for (int i = 0; i < kNumPackets; i++) {
    auto transaction = connection_state->GetTransaction(
        TransactionKey(i, TransactionLocation::kInitiator));
    EXPECT_OK(transaction);
    auto packet_metadata = transaction.value()->GetPacketMetadata(
        i % 2 == 0 ? falcon::PacketType::kPullRequest
                   : falcon::PacketType::kPushUnsolicitedData);
    // Last packet in each TX window should be selected for retransmission
    // (request PSN 199, data PSN 199). All others should not have an early
    // retransmission reason set (default value kTimeout).
    if (i == 398 || i == 399) {
      EXPECT_EQ(packet_metadata.value()->retransmission_reason,
                RetransmitReason::kEarlyTlp);
    } else {
      EXPECT_EQ(packet_metadata.value()->retransmission_reason,
                RetransmitReason::kTimeout);
    }
  }
}

}  // namespace
}  // namespace isekai
