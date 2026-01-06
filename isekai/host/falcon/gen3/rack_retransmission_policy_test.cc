#include "isekai/host/falcon/gen3/rack_retransmission_policy.h"

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
#include "isekai/host/falcon/falcon_testing_helpers.h"
#include "isekai/host/falcon/gen1/connection_scheduler_types.h"
#include "isekai/host/falcon/gen1/falcon_types.h"
#include "isekai/host/falcon/gen3/connection_state.h"
#include "isekai/host/falcon/gen3/falcon_types.h"

namespace isekai {
namespace {

class RackRetransmissionPolicyTest
    : public FalconTestingHelpers::FalconTestSetup,
      public ::testing::Test {};

void CheckOutstandingPacketList(
    const absl::btree_set<RetransmissionWorkId> outstanding,
    std::vector<uint32_t> psn_list) {
  ASSERT_EQ(outstanding.size(), psn_list.size());
  int i = 0;
  for (auto it : outstanding) {
    ASSERT_EQ(it.psn, psn_list[i]);
    ++i;
  }
}

TEST_F(RackRetransmissionPolicyTest, RackCalculationTest) {
  constexpr double kDefaultRackTimeWindowRttFactor = 0.25;

  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(/*version=*/3);
  auto early_retx = config.mutable_early_retx();
  early_retx->set_ooo_distance_threshold(3);
  early_retx->set_enable_ooo_count(false);
  early_retx->set_enable_ooo_distance(false);
  early_retx->set_enable_rack(true);
  early_retx->set_enable_tlp(false);
  early_retx->set_rack_time_window_rtt_factor(kDefaultRackTimeWindowRttFactor);
  early_retx->set_min_rack_time_window_ns(0);
  InitFalcon(config);

  RackRetransmissionPolicy rack_policy(falcon_.get());

  // Create dummy connection state with init rtt_state and RACK values.
  Gen3ConnectionState connection_state(
      std::make_unique<ConnectionMetadata>(),
      std::make_unique<Gen3CongestionControlMetadata>(),
      /*version=*/3);
  Gen3CongestionControlMetadata& ccmeta =
      CongestionControlMetadata::DowncastTo<Gen3CongestionControlMetadata>(
          *connection_state.congestion_control_metadata);
  connection_state.rack.xmit_ts = absl::ZeroDuration();
  connection_state.rack.rto = absl::InfiniteDuration();
  ccmeta.rtt_state = falcon_->get_rate_update_engine()->ToFalconTimeUnits(
      absl::Microseconds(8));
  ccmeta.gen3_flow_rtt_state = {
      falcon_->get_rate_update_engine()->ToFalconTimeUnits(
          absl::Microseconds(8))};

  // Invoke RACK events and check resulting rack.rto and rack.xmit_ts values.
  env_.RunFor(absl::Microseconds(10));
  Packet ack;
  ack.packet_type = falcon::PacketType::kAck;
  ack.ack.ack_type = Packet::Ack::kAck;
  ack.ack.timestamp_1 = absl::Microseconds(9);
  rack_policy.HandleAck(&ack, &connection_state);
  EXPECT_EQ(connection_state.rack.xmit_ts, absl::Microseconds(9));
  // elapsed - tx_time + (rtt_state * rtt_factor)
  // 10 - 9 + (8 * 0.25) = 3 us
  EXPECT_NEAR(absl::ToDoubleNanoseconds(connection_state.rack.rto), 3000, 1);

  env_.RunFor(absl::Microseconds(15));
  ack.ack.timestamp_1 = absl::Microseconds(7);
  rack_policy.HandleAck(&ack, &connection_state);
  // No change given ack.t1 (7) < xmit_ts (9)
  EXPECT_EQ(connection_state.rack.xmit_ts, absl::Microseconds(9));
  EXPECT_NEAR(absl::ToDoubleNanoseconds(connection_state.rack.rto), 3000, 1);

  env_.RunFor(absl::Microseconds(15));
  Packet nack;
  nack.nack.timestamp_1 = absl::Microseconds(11);
  rack_policy.HandleNack(&nack, &connection_state);
  // xmit_ts should be updated.
  EXPECT_EQ(connection_state.rack.xmit_ts, absl::Microseconds(11));
  // elapsed - tx_time + (rtt_state * rtt_factor)
  // 40 - 11 + (8 * 0.25) = 31 us
  EXPECT_NEAR(absl::ToDoubleNanoseconds(connection_state.rack.rto), 31000, 1);
}

TEST_F(RackRetransmissionPolicyTest, RackGetTimeoutTest) {
  constexpr double kDefaultRackTimeWindowRttFactor = 0.25;
  constexpr absl::Duration kTestRackRto = absl::Microseconds(10);
  constexpr absl::Duration kTestRackXmitTs = absl::Microseconds(1);
  constexpr int kTestEarlyRetxThreshold = 3;

  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(/*version=*/3);
  auto early_retx = config.mutable_early_retx();
  early_retx->set_ooo_distance_threshold(3);
  early_retx->set_enable_ooo_count(false);
  early_retx->set_enable_ooo_distance(false);
  early_retx->set_enable_rack(true);
  early_retx->set_enable_tlp(false);
  early_retx->set_rack_time_window_rtt_factor(kDefaultRackTimeWindowRttFactor);
  early_retx->set_min_rack_time_window_ns(0);
  early_retx->set_early_retx_threshold(kTestEarlyRetxThreshold);
  InitFalcon(config);

  RackRetransmissionPolicy rack_policy(falcon_.get());

  // Create dummy connection state with init rtt_state and RACK values.;
  Gen3ConnectionState connection_state(
      std::make_unique<ConnectionMetadata>(),
      std::make_unique<Gen3CongestionControlMetadata>(),
      /*version=*/3);
  Gen3CongestionControlMetadata& ccmeta =
      CongestionControlMetadata::DowncastTo<Gen3CongestionControlMetadata>(
          *connection_state.congestion_control_metadata);
  connection_state.rack.xmit_ts = kTestRackXmitTs;
  connection_state.rack.rto = kTestRackRto;
  connection_state.rack.rack_enabled_for_window[0] = true;
  connection_state.rack.rack_enabled_for_window[1] = true;

  ccmeta.rtt_state = falcon_->get_rate_update_engine()->ToFalconTimeUnits(
      absl::Microseconds(8));
  ccmeta.gen3_flow_rtt_state = {
      falcon_->get_rate_update_engine()->ToFalconTimeUnits(
          absl::Microseconds(8))};

  // Check GetTimeoutOfPacket with various packet metadata values.
  PacketMetadata packet;
  packet.psn = 1;
  packet.type = falcon::PacketType::kPullRequest;
  // Unreceived packet with transmission time below xmit_ts, no previous early
  // retx attempts.
  packet.transmission_time = kTestRackXmitTs - absl::Nanoseconds(1);
  packet.received = false;
  packet.early_retx_attempts = 0;
  // RACK_RTO applies to this packet.
  EXPECT_EQ(rack_policy.GetTimeoutOfPacket(&connection_state, &packet),
            absl::Microseconds(10));

  // Packets with transmission_time == xmit_ts.
  packet.transmission_time = kTestRackXmitTs;
  packet.received = false;
  packet.early_retx_attempts = 0;
  // RACK_RTO applies to this packet.
  EXPECT_EQ(rack_policy.GetTimeoutOfPacket(&connection_state, &packet),
            absl::Microseconds(10));

  // Packets with transmission_time >= xmit_ts.
  packet.transmission_time = kTestRackXmitTs + absl::Nanoseconds(1);
  packet.received = false;
  packet.early_retx_attempts = 0;
  // RACK_RTO does not apply to this packet.
  EXPECT_EQ(rack_policy.GetTimeoutOfPacket(&connection_state, &packet),
            absl::InfiniteDuration());

  // Packet with received state.
  packet.transmission_time = kTestRackXmitTs - absl::Nanoseconds(1);
  packet.received = true;
  packet.early_retx_attempts = 0;
  // RACK_RTO does not apply to this packet.
  EXPECT_EQ(rack_policy.GetTimeoutOfPacket(&connection_state, &packet),
            absl::InfiniteDuration());

  // Packet with early retx attempts reaching threshold.
  packet.transmission_time = kTestRackXmitTs - absl::Nanoseconds(1);
  packet.received = false;
  packet.early_retx_attempts = kTestEarlyRetxThreshold;
  // RACK_RTO does not apply to this packet.
  EXPECT_EQ(rack_policy.GetTimeoutOfPacket(&connection_state, &packet),
            absl::InfiniteDuration());
}

TEST_F(RackRetransmissionPolicyTest, RackRetransmissionTest) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(/*version=*/3);
  auto early_retx = config.mutable_early_retx();
  early_retx->set_ooo_distance_threshold(3);
  early_retx->set_enable_ooo_count(false);
  early_retx->set_enable_ooo_distance(false);
  early_retx->set_enable_rack(true);
  early_retx->set_enable_tlp(false);
  // Set rack window to 2us always.
  early_retx->set_rack_time_window_rtt_factor(0);
  early_retx->set_min_rack_time_window_ns(2000);
  InitFalcon(config);

  std::vector<Packet*> packet;
  ConnectionState* connection_state;
  // 5 PullReq and 5 PushUsData interleaves, 1000ns inter-packet gap.
  for (int i = 0; i < 10; ++i) {
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

  auto gen3_connection_state =
      static_cast<Gen3ConnectionState*>(connection_state);
  // At 20us, E-ACK with req: 11000, data: 00000.
  auto eack = std::make_unique<Packet>();
  eack->packet_type = falcon::PacketType::kAck;
  eack->ack.ack_type = Packet::Ack::kEack;
  eack->ack.dest_cid = 1;
  eack->ack.rdbpsn = 0;
  eack->ack.rrbpsn = 0;
  eack->ack.timestamp_1 = absl::Microseconds(8);
  for (auto psn : {3, 4}) {
    eack->ack.receiver_request_bitmap.Set(psn, true);
  }
  env_.RunFor(absl::Microseconds(20) - env_.ElapsedTime());
  EXPECT_OK(reliability_manager_->ReceivePacket(eack.get()));
  EXPECT_EQ(gen3_connection_state->rack_pkt_scan_count, 9);

  // Req PSN 0, 1, 2 and data PSN 0, 1, 2 should be immediately retransmitted.
  env_.RunFor(absl::Nanoseconds(200));
  for (int i = 0; i < 10; ++i) {
    ASSERT_OK_AND_ASSIGN(auto transaction,
                         connection_state->GetTransaction(TransactionKey(
                             i, TransactionLocation::kInitiator)));

    ASSERT_OK_AND_ASSIGN(
        auto packet,
        transaction->GetPacketMetadata(
            i % 2 == 0 ? falcon::PacketType::kPullRequest
                       : falcon::PacketType::kPushUnsolicitedData));
    if (i < 6) {
      EXPECT_EQ(packet->transmit_attempts, 2);
      EXPECT_EQ(packet->early_retx_attempts, 1);
    } else {
      EXPECT_EQ(packet->transmit_attempts, 1);
      EXPECT_EQ(packet->early_retx_attempts, 0);
    }
  }

  // At 30us, data PSN 0 is NACKed.
  auto nack = std::make_unique<Packet>();
  nack->packet_type = falcon::PacketType::kNack;
  nack->nack.nack_psn = 0;
  nack->nack.code = falcon::NackCode::kRxResourceExhaustion;
  nack->nack.rdbpsn = 0;
  nack->nack.rrbpsn = 0;
  nack->nack.dest_cid = 1;
  nack->nack.request_window = false;
  ASSERT_OK_AND_ASSIGN(auto transaction,
                       connection_state->GetTransaction(
                           TransactionKey(2, TransactionLocation::kInitiator)));
  ASSERT_OK_AND_ASSIGN(
      auto packet_metadata,
      transaction->GetPacketMetadata(falcon::PacketType::kPullRequest));
  nack->nack.timestamp_1 = packet_metadata->transmission_time;
  gen3_connection_state->rack_pkt_scan_count = 0;
  env_.RunFor(absl::Microseconds(10));
  EXPECT_OK(reliability_manager_->ReceivePacket(nack.get()));
  // Req PSN 0 and Data PSN 0 should not be retransmitted until 32us.
  env_.RunFor(absl::Microseconds(2) + absl::Nanoseconds(100));
  EXPECT_EQ(gen3_connection_state->rack_pkt_scan_count, 10);
  for (int i = 0; i < 10; ++i) {
    ASSERT_OK_AND_ASSIGN(auto transaction,
                         connection_state->GetTransaction(TransactionKey(
                             i, TransactionLocation::kInitiator)));
    ASSERT_OK_AND_ASSIGN(
        auto packet,
        transaction->GetPacketMetadata(
            i % 2 == 0 ? falcon::PacketType::kPullRequest
                       : falcon::PacketType::kPushUnsolicitedData));
    if (i < 2) {
      EXPECT_EQ(packet->transmit_attempts, 3);
      EXPECT_EQ(packet->early_retx_attempts, 2);
      EXPECT_GE(packet->transmission_time, absl::Microseconds(32));
    } else if (i == 6 || i == 8) {
      // Req PSN 3, 4 are set in receive bitmap.
      EXPECT_EQ(packet->transmit_attempts, 1);
      EXPECT_EQ(packet->early_retx_attempts, 0);
    } else {
      EXPECT_EQ(packet->transmit_attempts, 2);
      EXPECT_EQ(packet->early_retx_attempts, 1);
    }
  }

  // At 50us, E-ACK with req: 11000, data: 0000 (rdbpsn=1).
  auto eack2 = std::make_unique<Packet>();
  eack2->packet_type = falcon::PacketType::kAck;
  eack2->ack.ack_type = Packet::Ack::kEack;
  eack2->ack.dest_cid = 1;
  eack2->ack.rdbpsn = 1;
  eack2->ack.rrbpsn = 0;
  eack2->ack.timestamp_1 = absl::Microseconds(30);
  for (auto psn : {3, 4}) {
    eack->ack.receiver_request_bitmap.Set(psn, true);
  }

  gen3_connection_state->rack_pkt_scan_count = 0;
  env_.RunFor(absl::Microseconds(50) - env_.ElapsedTime());
  EXPECT_OK(reliability_manager_->ReceivePacket(eack2.get()));
  // It should be a full scan.
  EXPECT_EQ(gen3_connection_state->rack_pkt_scan_count, 8);
}

TEST_F(RackRetransmissionPolicyTest, RackSimplifiedCriteriaRetransmissionTest) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(/*version=*/3);
  auto early_retx = config.mutable_early_retx();
  early_retx->set_ooo_distance_threshold(3);
  early_retx->set_enable_ooo_count(false);
  early_retx->set_enable_ooo_distance(false);
  early_retx->set_enable_rack(true);
  early_retx->set_enable_tlp(false);
  // Set rack window to 2us always.
  early_retx->set_rack_time_window_rtt_factor(0);
  early_retx->set_min_rack_time_window_ns(2000);
  // Set simplified RACK mode.
  early_retx->set_rack_bypass_rto_check(true);
  InitFalcon(config);

  std::vector<Packet*> packet;
  ConnectionState* connection_state;
  // 5 PullReq and 5 PushUsData interleaves, 1000ns inter-packet gap.
  for (int i = 0; i < 10; ++i) {
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

  // At 20us, E-ACK with req: 00001, data: 00000, T1=900ns.
  auto eack = std::make_unique<Packet>();
  eack->packet_type = falcon::PacketType::kAck;
  eack->ack.ack_type = Packet::Ack::kEack;
  eack->ack.dest_cid = 1;
  eack->ack.rdbpsn = 0;
  eack->ack.rrbpsn = 0;
  eack->ack.timestamp_1 = absl::Nanoseconds(900);
  for (auto psn : {0}) {
    eack->ack.receiver_request_bitmap.Set(psn, true);
  }
  env_.RunFor(absl::Microseconds(20) - env_.ElapsedTime());
  EXPECT_OK(reliability_manager_->ReceivePacket(eack.get()));
  env_.RunFor(absl::Nanoseconds(200));

  // Expect no retransmissions; all outstanding packets do not meet
  // xmit_ts=900ns criteria.
  for (int i = 0; i < 10; ++i) {
    ASSERT_OK_AND_ASSIGN(auto transaction,
                         connection_state->GetTransaction(TransactionKey(
                             i, TransactionLocation::kInitiator)));
    ASSERT_OK_AND_ASSIGN(
        auto packet,
        transaction->GetPacketMetadata(
            i % 2 == 0 ? falcon::PacketType::kPullRequest
                       : falcon::PacketType::kPushUnsolicitedData));

    EXPECT_EQ(packet->transmit_attempts, 1);
    EXPECT_EQ(packet->early_retx_attempts, 0);
  }

  // At 30us, E-ACK with req: 01100, data: 00000, T1=6000ns.
  auto eack2 = std::make_unique<Packet>();
  eack2->packet_type = falcon::PacketType::kAck;
  eack2->ack.ack_type = Packet::Ack::kEack;
  eack2->ack.dest_cid = 1;
  eack2->ack.rdbpsn = 0;
  eack2->ack.rrbpsn = 0;
  eack2->ack.timestamp_1 = absl::Nanoseconds(6000);
  for (auto psn : {3, 4}) {
    eack2->ack.receiver_request_bitmap.Set(psn, true);
  }
  env_.RunFor(absl::Microseconds(30) - env_.ElapsedTime());
  EXPECT_OK(reliability_manager_->ReceivePacket(eack2.get()));
  env_.RunFor(absl::Nanoseconds(200));

  // Expect retransmissions of outstanding packets with tx before RACK
  // xmit_ts=6000ns.
  for (int i = 0; i < 10; ++i) {
    ASSERT_OK_AND_ASSIGN(auto transaction,
                         connection_state->GetTransaction(TransactionKey(
                             i, TransactionLocation::kInitiator)));
    ASSERT_OK_AND_ASSIGN(
        auto packet,
        transaction->GetPacketMetadata(
            i % 2 == 0 ? falcon::PacketType::kPullRequest
                       : falcon::PacketType::kPushUnsolicitedData));
    if (i > 0 && i <= 5) {
      EXPECT_EQ(packet->transmit_attempts, 2);
      EXPECT_EQ(packet->early_retx_attempts, 1);
      EXPECT_EQ(packet->retransmission_reason, RetransmitReason::kEarlyRack);
    } else {
      EXPECT_EQ(packet->transmit_attempts, 1);
      EXPECT_EQ(packet->early_retx_attempts, 0);
      EXPECT_NE(packet->retransmission_reason, RetransmitReason::kEarlyRack);
    }
  }

  // RTO at 1.001ms should not result in any further RACK retransmissions.
  env_.RunFor(absl::Milliseconds(1.001) + absl::Nanoseconds(100) -
              env_.ElapsedTime());
  for (int i = 0; i < 10; ++i) {
    ASSERT_OK_AND_ASSIGN(auto transaction,
                         connection_state->GetTransaction(TransactionKey(
                             i, TransactionLocation::kInitiator)));
    ASSERT_OK_AND_ASSIGN(
        auto packet,
        transaction->GetPacketMetadata(
            i % 2 == 0 ? falcon::PacketType::kPullRequest
                       : falcon::PacketType::kPushUnsolicitedData));
    if (i > 0 && i <= 5) {
      EXPECT_EQ(packet->transmit_attempts, 2);
      EXPECT_EQ(packet->early_retx_attempts, 1);
      EXPECT_EQ(packet->retransmission_reason, RetransmitReason::kEarlyRack);
    } else {
      EXPECT_EQ(packet->transmit_attempts, 1);
      EXPECT_EQ(packet->early_retx_attempts, 0);
      EXPECT_NE(packet->retransmission_reason, RetransmitReason::kEarlyRack);
    }
  }
}

}  // namespace
}  // namespace isekai
