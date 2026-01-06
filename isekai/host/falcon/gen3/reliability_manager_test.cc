#include "isekai/host/falcon/gen3/reliability_manager.h"

#include <cstdint>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "absl/time/time.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "isekai/common/default_config_generator.h"
#include "isekai/common/packet.h"
#include "isekai/common/testing.h"
#include "isekai/host/falcon/falcon.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/falcon_connection_state.h"
#include "isekai/host/falcon/falcon_testing_helpers.h"
#include "isekai/host/falcon/gen1/falcon_types.h"
#include "isekai/host/falcon/gen3/falcon_types.h"
#include "isekai/host/falcon/rue/bits.h"
#include "isekai/host/falcon/rue/fixed.h"

namespace isekai {
namespace {

using ::testing::_;
constexpr uint8_t kNumFlowsPerMultiPathConnection = 4;

using ::testing::AtLeast;
using ::testing::InSequence;
using ::testing::MockFunction;
using ::testing::Return;
using ::testing::StrictMock;

class Gen3ReliabilityManagerTest : public FalconTestingHelpers::FalconTestSetup,
                                   public ::testing::Test {};

class MockRetransmissionPolicy : public RetransmissionPolicy {
 public:
  MOCK_METHOD(RetransmissionPolicyAction, InitConnection,
              (ConnectionState * connection_state), (override));
  MOCK_METHOD(RetransmissionPolicyAction, HandleAck,
              (const Packet* packet, ConnectionState* connection_state),
              (override));
  MOCK_METHOD(RetransmissionPolicyAction, HandleNack,
              (const Packet* packet, ConnectionState* connection_state),
              (override));
  MOCK_METHOD(RetransmissionPolicyAction, HandleAckToUlp,
              (PacketMetadata * packet_metadata,
               ConnectionState* connection_state),
              (override));
  MOCK_METHOD(RetransmissionPolicyAction, HandleRetransmitTimeoutEvent,
              (ConnectionState * connection_state), (override));
  MOCK_METHOD(RetransmissionPolicyAction, SetupRetransmission,
              (PacketMetadata * packet_metadata,
               ConnectionState* connection_state),
              (override));
  MOCK_METHOD(RetransmissionPolicyAction, HandlePiggybackedAck,
              (const Packet* packet, ConnectionState* connection_state),
              (override));
  MOCK_METHOD(RetransmissionPolicyAction, HandleImplicitAck,
              (const Packet* packet, ConnectionState* connection_state),
              (override));
  MOCK_METHOD(RetransmissionPolicyAction, HandleScheduledEvent,
              (ConnectionState * connection_state), (override));
  MOCK_METHOD(absl::Duration, GetTimeoutOfPacket,
              (ConnectionState * connection_state,
               PacketMetadata* packet_metadata),
              (override));
};

TEST_F(Gen3ReliabilityManagerTest, RetransmissionPoliciesInvoked) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(/*version=*/3);
  config.set_inter_host_rx_scheduling_tick_ns(0);
  InitFalcon(config);
  auto gen3_reliability_manager =
      dynamic_cast<Gen3ReliabilityManager*>(reliability_manager_);

  // Run for 5 RTO intervals and expect at least 4 RTO events.
  constexpr int kNumRtoIntervals = 5;

  // Expect handlers to be called on 2 installed policies (returning noops).
  auto mock_retx_policy1 =
      std::make_unique<StrictMock<MockRetransmissionPolicy>>();
  EXPECT_CALL(*mock_retx_policy1, InitConnection);
  EXPECT_CALL(*mock_retx_policy1, SetupRetransmission)
      .Times(AtLeast(kNumRtoIntervals - 1));
  EXPECT_CALL(*mock_retx_policy1, HandleRetransmitTimeoutEvent)
      .Times(AtLeast(kNumRtoIntervals - 1));
  EXPECT_CALL(*mock_retx_policy1, HandleAck);
  EXPECT_CALL(*mock_retx_policy1, HandleAckToUlp);
  EXPECT_CALL(*mock_retx_policy1, GetTimeoutOfPacket)
      .WillRepeatedly(Return(absl::InfiniteDuration()));
  gen3_reliability_manager->InstallRetransmissionPolicyForTesting(
      std::move(mock_retx_policy1));

  auto mock_retx_policy2 =
      std::make_unique<StrictMock<MockRetransmissionPolicy>>();
  EXPECT_CALL(*mock_retx_policy2, InitConnection);
  EXPECT_CALL(*mock_retx_policy2, SetupRetransmission)
      .Times(AtLeast(kNumRtoIntervals - 1));
  EXPECT_CALL(*mock_retx_policy2, HandleRetransmitTimeoutEvent)
      .Times(AtLeast(kNumRtoIntervals - 1));
  EXPECT_CALL(*mock_retx_policy2, HandleAck);
  EXPECT_CALL(*mock_retx_policy2, HandleAckToUlp);
  EXPECT_CALL(*mock_retx_policy2, GetTimeoutOfPacket)
      .WillRepeatedly(Return(absl::InfiniteDuration()));
  gen3_reliability_manager->InstallRetransmissionPolicyForTesting(
      std::move(mock_retx_policy2));

  Packet* packet;
  ConnectionState* connection_state;
  std::tie(packet, connection_state) = FalconTestingHelpers::SetupTransaction(
      falcon_.get(), TransactionType::kPull, TransactionLocation::kInitiator,
      TransactionState::kPullReqUlpRx, falcon::PacketType::kPullRequest, 1, 0,
      0);
  packet->metadata.scid = 1;
  ASSERT_OK(reliability_manager_->TransmitPacket(
      /*scid=*/1, /*rsn=*/0, falcon::PacketType::kPullRequest));

  Gen3CongestionControlMetadata& ccmeta =
      CongestionControlMetadata::DowncastTo<Gen3CongestionControlMetadata>(
          *connection_state->congestion_control_metadata);
  env_.RunFor(ccmeta.retransmit_timeout * kNumRtoIntervals);

  // Send an ACK after several RTO intervals, expecting Ack handlers to be
  // triggered.
  auto ack = std::make_unique<Packet>();
  ack->packet_type = falcon::PacketType::kAck;
  ack->ack.dest_cid = 1;
  ack->ack.rrbpsn = 1;
  ASSERT_OK(reliability_manager_->ReceivePacket(ack.get()));
  env_.RunFor(absl::Nanoseconds(100));
}

TEST_F(Gen3ReliabilityManagerTest, RetransmissionPolicyEventsScheduled) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(/*version=*/3);
  config.set_inter_host_rx_scheduling_tick_ns(0);
  InitFalcon(config);
  auto gen3_reliability_manager =
      dynamic_cast<Gen3ReliabilityManager*>(reliability_manager_);

  // Schedule an RetransmissionPolicy event after 10us.
  constexpr absl::Duration kRetxPolicyEventDuration = absl::Microseconds(10);

  // RetransmissionPolicy InitConnection handler will request a scheduled event.
  auto mock_retx_policy =
      std::make_unique<StrictMock<MockRetransmissionPolicy>>();
  MockFunction<void(std::string checkpoint_name)> mock_checkpoint;

  // GetTimeoutOfPacket will be called during RTO events, ordering is not
  // important for this test.
  EXPECT_CALL(*mock_retx_policy, GetTimeoutOfPacket)
      .WillRepeatedly(Return(absl::InfiniteDuration()));
  {
    // Expect ScheduledEvent handler to be called only after the checkpoint,
    // which will occur after the Isekai env has run for the scheduled duration.
    InSequence s;
    EXPECT_CALL(*mock_retx_policy, InitConnection)
        .WillOnce(Return(RetransmissionPolicyAction{
            .scheduled_event_duration = kRetxPolicyEventDuration,
        }));
    EXPECT_CALL(*mock_retx_policy, SetupRetransmission);
    EXPECT_CALL(mock_checkpoint, Call("Before scheduled event"));
    EXPECT_CALL(*mock_retx_policy, HandleScheduledEvent);
    EXPECT_CALL(mock_checkpoint, Call("After scheduled event"));
  }
  gen3_reliability_manager->InstallRetransmissionPolicyForTesting(
      std::move(mock_retx_policy));

  // Create a transaction to trigger InitConnection and schedule the event.
  Packet* packet;
  ConnectionState* connection_state;
  std::tie(packet, connection_state) = FalconTestingHelpers::SetupTransaction(
      falcon_.get(), TransactionType::kPull, TransactionLocation::kInitiator,
      TransactionState::kPullReqUlpRx, falcon::PacketType::kPullRequest, 1, 0,
      0);
  packet->metadata.scid = 1;
  ASSERT_OK(reliability_manager_->TransmitPacket(
      /*scid=*/1, /*rsn=*/0, falcon::PacketType::kPullRequest));

  env_.RunFor(kRetxPolicyEventDuration - absl::Nanoseconds(1));
  mock_checkpoint.Call("Before scheduled event");
  env_.RunFor(absl::Nanoseconds(1));
  mock_checkpoint.Call("After scheduled event");
}

TEST_F(Gen3ReliabilityManagerTest, EarlyRetxLimitTest) {
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
  // Set early-retx limit to 1.
  early_retx->set_early_retx_threshold(2);
  InitFalcon(config);
  // Steps to initialize a transaction and get handle on the packet and
  // connection state.
  std::vector<Packet*> packet;
  ConnectionState* connection_state;
  // 10 PushUsData, 100ns inter-packet gap.
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
    env_.RunFor(absl::Nanoseconds(100));
  }

  // TLP timer will expire at 50us + 900ns. PSN 0 will be retransmitted.
  env_.RunFor(absl::Microseconds(51) - env_.ElapsedTime());
  for (int i = 0; i < 10; i++) {
    auto transaction = connection_state->GetTransaction(
        TransactionKey(i, TransactionLocation::kInitiator));
    EXPECT_OK(transaction);
    auto packet_metadata = transaction.value()->GetPacketMetadata(
        falcon::PacketType::kPushUnsolicitedData);
    if (i == 0) {
      EXPECT_EQ(packet_metadata.value()->transmit_attempts, 2);
      EXPECT_EQ(packet_metadata.value()->early_retx_attempts, 1);
      EXPECT_GE(packet_metadata.value()->transmission_time,
                absl::Microseconds(50) + absl::Nanoseconds(900));
    } else {
      EXPECT_EQ(packet_metadata.value()->transmit_attempts, 1);
      EXPECT_EQ(packet_metadata.value()->early_retx_attempts, 0);
    }
  }

  // At 70us, EACK arrives. PSNs 0 through 8 should be retransmitted by RACK.
  env_.RunFor(absl::Microseconds(70) - env_.ElapsedTime());
  auto eack = std::make_unique<Packet>();
  eack->packet_type = falcon::PacketType::kAck;
  eack->ack.ack_type = Packet::Ack::kEack;
  eack->ack.dest_cid = 1;
  eack->ack.rdbpsn = 0;
  eack->ack.rrbpsn = 0;
  eack->timestamps.received_timestamp = absl::Microseconds(70);
  eack->ack.timestamp_1 = absl::Microseconds(59);
  for (auto psn : {9}) {
    eack->ack.received_bitmap.Set(psn, true);
  }
  EXPECT_OK(reliability_manager_->ReceivePacket(eack.get()));

  env_.RunFor(absl::Nanoseconds(500));
  for (int i = 0; i < 10; i++) {
    auto transaction = connection_state->GetTransaction(
        TransactionKey(i, TransactionLocation::kInitiator));
    EXPECT_OK(transaction);
    auto packet_metadata = transaction.value()->GetPacketMetadata(
        falcon::PacketType::kPushUnsolicitedData);
    if (i == 0) {
      EXPECT_EQ(packet_metadata.value()->transmit_attempts, 3);
      EXPECT_EQ(packet_metadata.value()->early_retx_attempts, 2);
    } else if (i <= 8) {
      EXPECT_EQ(packet_metadata.value()->transmit_attempts, 2);
      EXPECT_EQ(packet_metadata.value()->early_retx_attempts, 1);
    } else {
      EXPECT_EQ(packet_metadata.value()->transmit_attempts, 1);
      EXPECT_EQ(packet_metadata.value()->early_retx_attempts, 0);
    }
  }

  // TLP timer will expire at 80+50us, but there should be no retransmissions.
  // TLP scan breaks when it encounters a packet which reached early retx limit.
  env_.RunFor(absl::Microseconds(80 + 50) - env_.ElapsedTime() +
              absl::Nanoseconds(100));
  for (int i = 0; i < 10; i++) {
    auto transaction = connection_state->GetTransaction(
        TransactionKey(i, TransactionLocation::kInitiator));
    EXPECT_OK(transaction);
    auto packet_metadata = transaction.value()->GetPacketMetadata(
        falcon::PacketType::kPushUnsolicitedData);
    if (i == 0) {
      EXPECT_EQ(packet_metadata.value()->transmit_attempts, 3);
      EXPECT_EQ(packet_metadata.value()->early_retx_attempts, 2);
    } else if (i <= 8) {
      EXPECT_EQ(packet_metadata.value()->transmit_attempts, 2);
      EXPECT_EQ(packet_metadata.value()->early_retx_attempts, 1);
    } else {
      EXPECT_EQ(packet_metadata.value()->transmit_attempts, 1);
      EXPECT_EQ(packet_metadata.value()->early_retx_attempts, 0);
    }
  }

  // RTO can retx PSN 0 only.
  env_.RunFor(absl::Microseconds(1000));
  for (int i = 0; i < 10; i++) {
    auto transaction = connection_state->GetTransaction(
        TransactionKey(i, TransactionLocation::kInitiator));
    EXPECT_OK(transaction);
    auto packet_metadata = transaction.value()->GetPacketMetadata(
        falcon::PacketType::kPushUnsolicitedData);
    if (i == 0) {
      EXPECT_EQ(packet_metadata.value()->transmit_attempts, 4);
      EXPECT_EQ(packet_metadata.value()->early_retx_attempts, 2);
    } else if (i <= 8) {
      EXPECT_EQ(packet_metadata.value()->transmit_attempts, 2);
      EXPECT_EQ(packet_metadata.value()->early_retx_attempts, 1);
    } else {
      EXPECT_EQ(packet_metadata.value()->transmit_attempts, 1);
      EXPECT_EQ(packet_metadata.value()->early_retx_attempts, 0);
    }
  }

  // Another RTO will retransmit 0-8, but not count towards early retx limit.
  env_.RunFor(absl::Microseconds(1000));
  for (int i = 0; i < 10; i++) {
    auto transaction = connection_state->GetTransaction(
        TransactionKey(i, TransactionLocation::kInitiator));
    EXPECT_OK(transaction);
    auto packet_metadata = transaction.value()->GetPacketMetadata(
        falcon::PacketType::kPushUnsolicitedData);
    if (i == 0) {
      EXPECT_EQ(packet_metadata.value()->transmit_attempts, 5);
      EXPECT_EQ(packet_metadata.value()->early_retx_attempts, 2);
    } else if (i <= 8) {
      EXPECT_EQ(packet_metadata.value()->transmit_attempts, 3);
      EXPECT_EQ(packet_metadata.value()->early_retx_attempts, 1);
    } else {
      EXPECT_EQ(packet_metadata.value()->transmit_attempts, 1);
      EXPECT_EQ(packet_metadata.value()->early_retx_attempts, 0);
    }
  }
}

// Tests CLOSED_LOOP_MAX_OPEN_FCWND_RR_WHEN_TIE Multipath policy correctly
// generates flow labels.
TEST_F(Gen3ReliabilityManagerTest, ClosedLoopKatyFlowScheduling) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(/*FalconVersion=*/3);
  config.mutable_gen2_config_options()
      ->mutable_multipath_config()
      ->set_path_selection_policy(
          FalconConfig::Gen2ConfigOptions::MultipathConfig::
              CLOSED_LOOP_MAX_OPEN_FCWND_RR_WHEN_TIE);
  InitFalcon(config);
  // Initialize the connection state.
  auto connection_metadata =
      FalconTestingHelpers::InitializeConnectionMetadata(falcon_.get());
  uint32_t scid = connection_metadata->scid;
  FalconTestingHelpers::SetMultipathingDegree(connection_metadata.get(),
                                              kNumFlowsPerMultiPathConnection);
  ConnectionState* connection_state =
      FalconTestingHelpers::InitializeConnectionState(
          falcon_.get(), std::move(connection_metadata));
  // Shortcut references
  Gen3CongestionControlMetadata& ccmeta =
      CongestionControlMetadata::DowncastTo<Gen3CongestionControlMetadata>(
          *connection_state->congestion_control_metadata);
  auto& flow_labels = ccmeta.gen2_flow_labels;
  auto& flow_fcwnds = ccmeta.gen3_flow_fcwnds;
  auto& flow_outstanding_count = ccmeta.gen3_outstanding_count;

  ccmeta.fabric_congestion_window = 10;
  flow_fcwnds = {falcon_rue::FloatToFixed<double, uint32_t>(
                     4.0, falcon_rue::kFractionalBits),
                 falcon_rue::FloatToFixed<double, uint32_t>(
                     3.0, falcon_rue::kFractionalBits),
                 falcon_rue::FloatToFixed<double, uint32_t>(
                     2.0, falcon_rue::kFractionalBits),
                 falcon_rue::FloatToFixed<double, uint32_t>(
                     1.0, falcon_rue::kFractionalBits)};
  flow_outstanding_count = {0, 0, 0, 0};
  // MAX_OPEN_FCWND choose the one with the maximum open window. If tie, it
  // uses round robin.
  //  open window   Flow #0    Flow #1   Flow #2   Flow #3  |  expected flow id
  //  -------------------------------------------------------------------------
  //  Round 1       (4 - 0)    (3 - 0)   (2 - 0)   (1 - 0)  |       0
  //  Round 2       (4 - 1)    (3 - 0)   (2 - 0)   (1 - 0)  |       1
  //  Round 3       (4 - 1)    (3 - 1)   (2 - 0)   (1 - 0)  |       0
  //  Round 4       (4 - 2)    (3 - 1)   (2 - 0)   (1 - 0)  |       1
  //  Round 5       (4 - 2)    (3 - 2)   (2 - 0)   (1 - 0)  |       2
  //  Round 6       (4 - 2)    (3 - 2)   (2 - 1)   (1 - 0)  |       0
  //  Round 7       (4 - 3)    (3 - 2)   (2 - 1)   (1 - 0)  |       1
  //  Round 8       (4 - 3)    (3 - 3)   (2 - 1)   (1 - 0)  |       2
  //  Round 9       (4 - 3)    (3 - 3)   (2 - 2)   (1 - 0)  |       3
  //  Round 10      (4 - 3)    (3 - 3)   (2 - 2)   (1 - 1)  |       0
  std::vector expected_flow_id = {0, 1, 0, 1, 2, 0, 1, 2, 3, 0};

  // Total number of packets to transmit in this test.
  int n_tx_packet = 10;

  for (int i = 0; i < n_tx_packet; i++) {
    // Setup the transaction.
    FalconTestingHelpers::SetupTransactionWithConnectionState(
        /*connection_state=*/connection_state,
        /*transaction_type=*/TransactionType::kPushUnsolicited,
        /*transaction_location=*/TransactionLocation::kInitiator,
        /*transaction_state=*/TransactionState::kPushUnsolicitedReqUlpRx,
        /*packet_metadata_type=*/falcon::PacketType::kPushUnsolicitedData,
        /*rsn=*/i,
        /*psn=*/i);
    uint32_t expected_flow_label = flow_labels[expected_flow_id[i]];

    EXPECT_CALL(shaper_, TransferTxPacket(_))
        .WillOnce([expected_flow_label](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->packet_type, falcon::PacketType::kPushUnsolicitedData);
          EXPECT_EQ(p->metadata.flow_label, expected_flow_label);
        });
    // Transmit the packet.
    EXPECT_OK(reliability_manager_->TransmitPacket(
        scid, i, falcon::PacketType::kPushUnsolicitedData));
  }
  // Do not care about any retx in this test, so run for 10us only enough
  // for initial transmissions.
  env_.RunFor(absl::Microseconds(10));
}

TEST_F(Gen3ReliabilityManagerTest, ClosedLoopKatyWithSmallWindow) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(/*FalconVersion=*/3);
  config.mutable_gen2_config_options()
      ->mutable_multipath_config()
      ->set_path_selection_policy(
          FalconConfig::Gen2ConfigOptions::MultipathConfig::
              CLOSED_LOOP_MAX_OPEN_FCWND_RR_WHEN_TIE);
  InitFalcon(config);
  // Initialize the connection state.
  auto connection_metadata =
      FalconTestingHelpers::InitializeConnectionMetadata(falcon_.get());
  uint32_t scid = connection_metadata->scid;
  FalconTestingHelpers::SetMultipathingDegree(connection_metadata.get(),
                                              kNumFlowsPerMultiPathConnection);
  ConnectionState* connection_state =
      FalconTestingHelpers::InitializeConnectionState(
          falcon_.get(), std::move(connection_metadata));
  // Shortcut references
  Gen3CongestionControlMetadata& ccmeta =
      CongestionControlMetadata::DowncastTo<Gen3CongestionControlMetadata>(
          *connection_state->congestion_control_metadata);
  auto& flow_labels = ccmeta.gen2_flow_labels;
  auto& flow_fcwnds = ccmeta.gen3_flow_fcwnds;
  auto& flow_outstanding_count = ccmeta.gen3_outstanding_count;

  ccmeta.fabric_congestion_window = 10;
  flow_fcwnds = {falcon_rue::FloatToFixed<double, uint32_t>(
                     0.1, falcon_rue::kFractionalBits),
                 falcon_rue::FloatToFixed<double, uint32_t>(
                     0.2, falcon_rue::kFractionalBits),
                 falcon_rue::FloatToFixed<double, uint32_t>(
                     0.3, falcon_rue::kFractionalBits),
                 falcon_rue::FloatToFixed<double, uint32_t>(
                     0.4, falcon_rue::kFractionalBits)};
  flow_outstanding_count = {0, 0, 0, 0};

  // MAX_OPEN_FCWND choose the one with the maximum open window. If tie, it
  // chooses the one with a round robin.
  //  open window   Flow #0    Flow #1    Flow #2   Flow #3  |  expected flow id
  //  -------------------------------------------------------------------------
  //  Round 1       (1 - 0)    (1 - 0)   (1 - 0)   (1 - 0)  |       0
  //  Round 2       (1 - 1)    (1 - 0)   (1 - 0)   (1 - 0)  |       1
  //  Round 3       (1 - 1)    (1 - 1)   (1 - 0)   (1 - 0)  |       2
  //  Round 4       (1 - 1)    (1 - 1)   (1 - 1)   (1 - 0)  |       3
  std::vector expected_flow_id = {0, 1, 2, 3};

  // Total number of packets to transmit in this test.
  int n_tx_packet = 4;

  for (int i = 0; i < n_tx_packet; i++) {
    // Setup the transaction.
    FalconTestingHelpers::SetupTransactionWithConnectionState(
        /*connection_state=*/connection_state,
        /*transaction_type=*/TransactionType::kPushUnsolicited,
        /*transaction_location=*/TransactionLocation::kInitiator,
        /*transaction_state=*/TransactionState::kPushUnsolicitedReqUlpRx,
        /*packet_metadata_type=*/falcon::PacketType::kPushUnsolicitedData,
        /*rsn=*/i,
        /*psn=*/i);
    uint32_t expected_flow_label = flow_labels[expected_flow_id[i]];

    EXPECT_CALL(shaper_, TransferTxPacket(_))
        .WillOnce([expected_flow_label](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->packet_type, falcon::PacketType::kPushUnsolicitedData);
          EXPECT_EQ(p->metadata.flow_label, expected_flow_label);
        });
    // Transmit the packet.
    EXPECT_OK(reliability_manager_->TransmitPacket(
        scid, i, falcon::PacketType::kPushUnsolicitedData));
  }
  // Do not care about any retx in this test, so run for 10us only enough
  // for initial transmissions.
  env_.RunFor(absl::Microseconds(10));
}

// Tests that retransmissions use the latest flow label for the flow ID of the
// original initial transmission.
TEST_F(Gen3ReliabilityManagerTest, RetransmissionClosedLoopKaty) {
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(/*FalconVersion=*/3);
  config.mutable_gen2_config_options()
      ->mutable_multipath_config()
      ->set_path_selection_policy(
          FalconConfig::Gen2ConfigOptions::MultipathConfig::
              CLOSED_LOOP_MAX_OPEN_FCWND_RR_WHEN_TIE);
  config.mutable_gen2_config_options()
      ->mutable_multipath_config()
      ->set_retx_flow_label(FalconConfig::Gen2ConfigOptions::MultipathConfig::
                                SAME_FLOW_ID_AS_INITIAL_TX);
  InitFalcon(config);
  // Initialize the connection state.
  auto connection_metadata =
      FalconTestingHelpers::InitializeConnectionMetadata(falcon_.get());
  uint32_t scid = connection_metadata->scid;
  FalconTestingHelpers::SetMultipathingDegree(connection_metadata.get(),
                                              kNumFlowsPerMultiPathConnection);
  ConnectionState* connection_state =
      FalconTestingHelpers::InitializeConnectionState(
          falcon_.get(), std::move(connection_metadata));
  // Shortcut references
  Gen3CongestionControlMetadata& ccmeta =
      CongestionControlMetadata::DowncastTo<Gen3CongestionControlMetadata>(
          *connection_state->congestion_control_metadata);
  auto& flow_labels = ccmeta.gen2_flow_labels;
  auto& flow_fcwnds = ccmeta.gen3_flow_fcwnds;
  auto& flow_outstanding_count = ccmeta.gen3_outstanding_count;

  ccmeta.fabric_congestion_window = 10;
  flow_fcwnds = {falcon_rue::FloatToFixed<double, uint32_t>(
                     4.0, falcon_rue::kFractionalBits),
                 falcon_rue::FloatToFixed<double, uint32_t>(
                     3.0, falcon_rue::kFractionalBits),
                 falcon_rue::FloatToFixed<double, uint32_t>(
                     2.0, falcon_rue::kFractionalBits),
                 falcon_rue::FloatToFixed<double, uint32_t>(
                     1.0, falcon_rue::kFractionalBits)};
  flow_outstanding_count = {0, 0, 0, 0};

  // Total number of packets to transmit in this test.
  int n_tx_packet = 10;

  for (int i = 0; i < n_tx_packet; i++) {
    // Setup the transaction.
    FalconTestingHelpers::SetupTransactionWithConnectionState(
        /*connection_state=*/connection_state,
        /*transaction_type=*/TransactionType::kPushUnsolicited,
        /*transaction_location=*/TransactionLocation::kInitiator,
        /*transaction_state=*/TransactionState::kPushUnsolicitedReqUlpRx,
        /*packet_metadata_type=*/falcon::PacketType::kPushUnsolicitedData,
        /*rsn=*/i,
        /*psn=*/i);

    // Transmit the packet
    EXPECT_OK(reliability_manager_->TransmitPacket(
        scid, i, falcon::PacketType::kPushUnsolicitedData));
  }
  // Do not care about any retx in this test, so run for 10us only enough
  // for initial transmissions.
  env_.RunFor(absl::Microseconds(10));

  // Get RTO expiration time to make sure RTO is triggered.
  absl::Duration rto_expiration_time =
      connection_state->tx_reliability_metadata.GetRtoExpirationTime();

  // Change the flow labels (while keeping the last 2 flow index bits intact) to
  // make sure packets are using the latest flow label for the flow ID.
  for (int idx = 0; idx < kNumFlowsPerMultiPathConnection; idx++) {
    flow_labels[idx] = flow_labels[idx] | (uint32_t)(0b11111100);
  }

  std::vector expected_flow_id = {0, 1, 0, 1, 2, 0, 1, 2, 3, 0};
  {
    ::testing::InSequence sequence;
    for (int i = 0; i < n_tx_packet; i++) {
      uint32_t expected_flow_label = flow_labels[expected_flow_id[i]];
      EXPECT_CALL(shaper_, TransferTxPacket(_))
          .WillOnce([expected_flow_label](std::unique_ptr<Packet> p) {
            // Make sure the flow label of a retransmitted packet is the latest
            // flow label for the flow ID corresponding to the initial original
            // packet.
            EXPECT_EQ(p->metadata.flow_label, expected_flow_label);
          });
    }
  }
  // Run until all packets are retransmitted by RTO.
  env_.RunUntil(rto_expiration_time + absl::Microseconds(10));
}

}  // namespace
}  // namespace isekai
