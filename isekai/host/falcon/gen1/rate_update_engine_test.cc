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

#include "isekai/host/falcon/gen1/rate_update_engine.h"

#include <cstdint>

#include "absl/time/time.h"
#include "gtest/gtest.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/default_config_generator.h"
#include "isekai/common/packet.h"
#include "isekai/common/testing.h"
#include "isekai/host/falcon/falcon.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/falcon_connection_state.h"
#include "isekai/host/falcon/falcon_counters.h"
#include "isekai/host/falcon/falcon_testing_helpers.h"
#include "isekai/host/falcon/gen1/falcon_model.h"
#include "isekai/host/falcon/gen1/falcon_utils.h"
#include "isekai/host/falcon/rue/bits.h"
#include "isekai/host/falcon/rue/format.h"
#include "isekai/host/falcon/rue/format_gen2.h"

namespace isekai {

namespace {

constexpr uint32_t kCid1 = 123;
constexpr uint32_t kCid2 = 456;
constexpr uint64_t kEventQueueLimit = 100;

// Parameters for Gen1 typed test. We need to define the Event format, Response
// format, and the falcon version.
struct Gen1Parameters {
  typedef falcon_rue::Event EventT;
  typedef falcon_rue::Response ResponseT;
  static constexpr int kFalconVersion = 1;
};

// Parameters for Gen2 typed test. We need to define the Event format, Response
// format, and the falcon version.
struct Gen2Parameters {
  typedef falcon_rue::Event_Gen2 EventT;
  typedef falcon_rue::Response_Gen2 ResponseT;
  static constexpr int kFalconVersion = 2;
};

// This defines all the objects needed for setup and testing
template <typename TypeParam>
class ProtocolRateUpdateEngineTest
    : public FalconTestingHelpers::FalconTestSetup,
      public testing::Test {
  typedef typename TypeParam::EventT EventT;
  typedef typename TypeParam::ResponseT ResponseT;

 protected:
  void SetUp() override {
    int falcon_version = TypeParam::kFalconVersion;
    FalconConfig config =
        DefaultConfigGenerator::DefaultFalconConfig(falcon_version);
    config.mutable_rue()->set_event_queue_size(kEventQueueLimit);
    config.mutable_rue()->set_event_queue_threshold_1(kEventQueueLimit);
    config.mutable_rue()->set_event_queue_threshold_2(kEventQueueLimit);
    config.mutable_rue()->set_event_queue_threshold_3(kEventQueueLimit);
    InitFalcon(config);

    rue_ = dynamic_cast<ProtocolRateUpdateEngine*>(
        falcon_->get_rate_update_engine());
    peer_.Set(rue_);

    auto metadata_1 = FalconTestingHelpers::InitializeConnectionMetadata(
        falcon_.get(), kCid1);
    FalconTestingHelpers::InitializeConnectionState(falcon_.get(),
                                                    std::move(metadata_1));
    auto metadata_2 = FalconTestingHelpers::InitializeConnectionMetadata(
        falcon_.get(), kCid2);
    FalconTestingHelpers::InitializeConnectionState(falcon_.get(),
                                                    std::move(metadata_2));
  }

  ProtocolRateUpdateEngine* rue_;
  ProtocolRateUpdateEngineTestPeer<EventT, ResponseT> peer_;
};

// Instantiate test suite for Gen1 and Gen2.
using Parameters = ::testing::Types<Gen1Parameters, Gen2Parameters>;
TYPED_TEST_SUITE(ProtocolRateUpdateEngineTest, Parameters);

// Tests the front end ACK processing of the RUE. This tests both the
// piggybacked ACKs and the explicit ACKs. Then it tests execution of those
// events on the congestion control algorithm.
// NOTE: This test verifies implementation details.
TYPED_TEST(ProtocolRateUpdateEngineTest, AckProcessing) {
  // Tests expected initial conditions
  EXPECT_FALSE(this->peer_.IsEventQueueScheduled());
  EXPECT_EQ(this->peer_.GetNumEvents(), 0);
  EXPECT_EQ(this->peer_.GetNumResponses(), 0);
  EXPECT_FALSE(this->peer_.IsConnectionOutstanding(kCid1));
  EXPECT_FALSE(this->peer_.IsConnectionOutstanding(kCid2));
  EXPECT_EQ(this->peer_.GetAccumulatedAcks(kCid1), 0);
  EXPECT_EQ(this->peer_.GetAccumulatedAcks(kCid2), 0);
  EXPECT_EQ(this->env_.ScheduledEvents(), 0);

  // Notifies the RUE of a piggybacked ACK
  Packet piggy_packet;
  piggy_packet.packet_type = falcon::PacketType::kPullRequest;
  piggy_packet.falcon.dest_cid = kCid1;
  piggy_packet.metadata.flow_label = 1;
  uint32_t pkts_acked1 = 18;
  this->peer_.IncrementNumAcked(&piggy_packet, pkts_acked1);

  // Tests conditions after the single piggybacked ACK
  EXPECT_FALSE(this->peer_.IsEventQueueScheduled());
  EXPECT_EQ(this->peer_.GetNumEvents(), 0);
  EXPECT_EQ(this->peer_.GetNumResponses(), 0);
  EXPECT_FALSE(this->peer_.IsConnectionOutstanding(kCid1));
  EXPECT_FALSE(this->peer_.IsConnectionOutstanding(kCid2));
  EXPECT_EQ(this->peer_.GetAccumulatedAcks(kCid1), pkts_acked1);
  EXPECT_EQ(this->peer_.GetAccumulatedAcks(kCid2), 0);
  EXPECT_EQ(this->env_.ScheduledEvents(), 0);

  // Notifies of a second piggybacked ACK on the same connection
  uint32_t pkts_acked2 = 7;
  this->peer_.IncrementNumAcked(&piggy_packet, pkts_acked2);

  // Tests conditions after the second piggybacked ACK
  EXPECT_FALSE(this->peer_.IsEventQueueScheduled());
  EXPECT_EQ(this->peer_.GetNumEvents(), 0);
  EXPECT_EQ(this->peer_.GetNumResponses(), 0);
  EXPECT_FALSE(this->peer_.IsConnectionOutstanding(kCid1));
  EXPECT_FALSE(this->peer_.IsConnectionOutstanding(kCid2));
  EXPECT_EQ(this->peer_.GetAccumulatedAcks(kCid1), pkts_acked1 + pkts_acked2);
  EXPECT_EQ(this->peer_.GetAccumulatedAcks(kCid2), 0);
  EXPECT_EQ(this->env_.ScheduledEvents(), 0);

  // Notifies the RUE of an explicit ACK
  Packet explicit_packet;
  explicit_packet.packet_type = falcon::PacketType::kAck;
  explicit_packet.ack.dest_cid = kCid1;
  explicit_packet.ack.timestamp_1 = absl::Nanoseconds(100001);
  explicit_packet.ack.timestamp_2 = absl::Nanoseconds(200001);
  explicit_packet.timestamps.sent_timestamp = absl::Nanoseconds(300001);
  explicit_packet.timestamps.received_timestamp = absl::Nanoseconds(400001);
  static_assert(falcon_rue::kForwardHopsBits >= 3);
  explicit_packet.ack.forward_hops = 7;
  static_assert(falcon_rue::kRxBufferLevelBits >= 3);
  explicit_packet.ack.rx_buffer_level = 4;
  static_assert(falcon_rue::kCcMetadataBits >= 16);
  explicit_packet.ack.cc_metadata = 0xDEAD;
  explicit_packet.metadata.flow_label = 1;
  uint32_t pkts_acked3 = 18;
  this->peer_.IncrementNumAcked(&explicit_packet, pkts_acked3);
  this->rue_->ExplicitAckReceived(&explicit_packet, false, false);

  // Tests conditions after the first explicit ACK
  EXPECT_TRUE(this->peer_.IsEventQueueScheduled());
  ASSERT_EQ(this->peer_.GetNumEvents(), 1);
  EXPECT_EQ(this->peer_.GetNumResponses(), 0);
  EXPECT_TRUE(this->peer_.IsConnectionOutstanding(kCid1));
  EXPECT_FALSE(this->peer_.IsConnectionOutstanding(kCid2));
  EXPECT_EQ(this->peer_.GetAccumulatedAcks(kCid1), 0);
  EXPECT_EQ(this->peer_.GetAccumulatedAcks(kCid2), 0);
  const auto queued_event = this->peer_.FrontEvent();
  EXPECT_EQ(queued_event.num_packets_acked,
            pkts_acked1 + pkts_acked2 + pkts_acked3);
  // EXPECT_EQ(queued_event.ecn_counter, ???);  //
  EXPECT_EQ(this->env_.ScheduledEvents(), 1);

  // Notifies the RUE of another explicit ACK
  uint32_t pkts_acked4 = 6;
  this->peer_.IncrementNumAcked(&explicit_packet, pkts_acked4);
  this->rue_->ExplicitAckReceived(&explicit_packet, false, false);

  // Tests conditions after the second explicit ACK
  EXPECT_TRUE(this->peer_.IsEventQueueScheduled());
  ASSERT_EQ(this->peer_.GetNumEvents(), 1);
  EXPECT_EQ(this->peer_.GetNumResponses(), 0);
  EXPECT_TRUE(this->peer_.IsConnectionOutstanding(kCid1));
  EXPECT_FALSE(this->peer_.IsConnectionOutstanding(kCid2));
  EXPECT_EQ(this->peer_.GetAccumulatedAcks(kCid1), pkts_acked4);
  EXPECT_EQ(this->peer_.GetAccumulatedAcks(kCid2), 0);
  const auto queued_event2 = this->peer_.FrontEvent();
  EXPECT_EQ(queued_event2.num_packets_acked,
            pkts_acked1 + pkts_acked2 + pkts_acked3);
  EXPECT_EQ(this->env_.ScheduledEvents(), 1);

  // Notifies of a second piggybacked ACK on the same connection
  uint32_t pkts_acked5 = 9;
  this->peer_.IncrementNumAcked(&piggy_packet, pkts_acked5);

  // Tests conditions after the third piggybacked ACK
  EXPECT_TRUE(this->peer_.IsEventQueueScheduled());
  ASSERT_EQ(this->peer_.GetNumEvents(), 1);
  EXPECT_EQ(this->peer_.GetNumResponses(), 0);
  EXPECT_TRUE(this->peer_.IsConnectionOutstanding(kCid1));
  EXPECT_FALSE(this->peer_.IsConnectionOutstanding(kCid2));
  EXPECT_EQ(this->peer_.GetAccumulatedAcks(kCid1), pkts_acked4 + pkts_acked5);
  EXPECT_EQ(this->peer_.GetAccumulatedAcks(kCid2), 0);
  const auto queued_event3 = this->peer_.FrontEvent();
  EXPECT_EQ(queued_event3.num_packets_acked,
            pkts_acked1 + pkts_acked2 + pkts_acked3);
  EXPECT_EQ(this->env_.ScheduledEvents(), 1);

  // Runs the simulator to allow the RUE to process the outstanding event
  EXPECT_EQ(this->env_.ExecutedEvents(), 0);
  this->env_.Run();

  // Two more events, one for queue processing, one for handling the response
  EXPECT_EQ(this->env_.ScheduledEvents(), 3);
  EXPECT_EQ(this->env_.ExecutedEvents(), 3);

  // Tests conditions after the RUE ACK event was executed
  EXPECT_FALSE(this->peer_.IsEventQueueScheduled());
  EXPECT_EQ(this->peer_.GetNumEvents(), 0);
  EXPECT_EQ(this->peer_.GetNumResponses(), 0);
  EXPECT_FALSE(this->peer_.IsConnectionOutstanding(kCid1));
  EXPECT_FALSE(this->peer_.IsConnectionOutstanding(kCid2));
  EXPECT_EQ(this->peer_.GetAccumulatedAcks(kCid1), pkts_acked4 + pkts_acked5);
  EXPECT_EQ(this->peer_.GetAccumulatedAcks(kCid2), 0);

  // Notifies the RUE of another explicit ACK
  uint32_t pkts_acked6 = 9;
  explicit_packet.ack.timestamp_1 = absl::Nanoseconds(100002);
  explicit_packet.ack.cc_metadata = 0xBEEF;
  this->peer_.IncrementNumAcked(&explicit_packet, pkts_acked6);
  this->rue_->ExplicitAckReceived(&explicit_packet, false, false);

  // Tests conditions after a new explicit ACK
  EXPECT_TRUE(this->peer_.IsEventQueueScheduled());
  ASSERT_EQ(this->peer_.GetNumEvents(), 1);
  EXPECT_EQ(this->peer_.GetNumResponses(), 0);
  EXPECT_TRUE(this->peer_.IsConnectionOutstanding(kCid1));
  EXPECT_FALSE(this->peer_.IsConnectionOutstanding(kCid2));
  EXPECT_EQ(this->peer_.GetAccumulatedAcks(kCid1), 0);
  EXPECT_EQ(this->peer_.GetAccumulatedAcks(kCid2), 0);
  const auto queued_event4 = this->peer_.FrontEvent();
  EXPECT_EQ(queued_event4.num_packets_acked,
            pkts_acked4 + pkts_acked5 + pkts_acked6);
  EXPECT_EQ(this->env_.ScheduledEvents(), 4);  // +1 from before

  // Runs the simulator to allow the RUE to process the outstanding event
  EXPECT_EQ(this->env_.ExecutedEvents(), 3);
  this->env_.Run();
  // Two more events, one for queue processing, one for handling the response
  EXPECT_EQ(this->env_.ScheduledEvents(), 6);
  EXPECT_EQ(this->env_.ExecutedEvents(), 6);

  // Tests conditions after the RUE ACK event was executed
  EXPECT_FALSE(this->peer_.IsEventQueueScheduled());
  EXPECT_EQ(this->peer_.GetNumEvents(), 0);
  EXPECT_EQ(this->peer_.GetNumResponses(), 0);
  EXPECT_FALSE(this->peer_.IsConnectionOutstanding(kCid1));
  EXPECT_FALSE(this->peer_.IsConnectionOutstanding(kCid2));
  EXPECT_EQ(this->peer_.GetAccumulatedAcks(kCid1), 0);
  EXPECT_EQ(this->peer_.GetAccumulatedAcks(kCid2), 0);

  // Test RUE counters.
  FalconConnectionCounters host_counters =
      this->falcon_->get_stats_manager()->GetConnectionCounters(kCid1);
  EXPECT_EQ(host_counters.rue_enqueue_attempts, 3);
  EXPECT_EQ(host_counters.rue_ack_events, 2);
  EXPECT_EQ(host_counters.rue_event_drops_ack, 1);
}

// Tests the front end EACK processing of the RUE.
TYPED_TEST(ProtocolRateUpdateEngineTest, EackProcessing) {
  // Tests expected initial conditions
  EXPECT_FALSE(this->peer_.IsEventQueueScheduled());
  EXPECT_EQ(this->peer_.GetNumEvents(), 0);
  EXPECT_EQ(this->peer_.GetNumResponses(), 0);
  EXPECT_FALSE(this->peer_.IsConnectionOutstanding(kCid1));
  EXPECT_FALSE(this->peer_.IsConnectionOutstanding(kCid2));
  EXPECT_EQ(this->peer_.GetAccumulatedAcks(kCid1), 0);
  EXPECT_EQ(this->peer_.GetAccumulatedAcks(kCid2), 0);
  EXPECT_EQ(this->env_.ScheduledEvents(), 0);

  // Notifies the RUE of an explicit EACK on kCid1.
  Packet explicit_packet;
  explicit_packet.packet_type = falcon::PacketType::kAck;
  explicit_packet.ack.dest_cid = kCid1;
  explicit_packet.ack.timestamp_1 = absl::Nanoseconds(100001);
  explicit_packet.ack.timestamp_2 = absl::Nanoseconds(200001);
  explicit_packet.timestamps.sent_timestamp = absl::Nanoseconds(300001);
  explicit_packet.timestamps.received_timestamp = absl::Nanoseconds(400001);
  static_assert(falcon_rue::kForwardHopsBits >= 3);
  explicit_packet.ack.forward_hops = 7;
  static_assert(falcon_rue::kRxBufferLevelBits >= 3);
  explicit_packet.ack.rx_buffer_level = 4;
  static_assert(falcon_rue::kCcMetadataBits >= 16);
  explicit_packet.ack.cc_metadata = 0xDEAD;
  explicit_packet.metadata.flow_label = 1;
  uint32_t pkts_acked1 = 18;
  this->peer_.IncrementNumAcked(&explicit_packet, pkts_acked1);
  this->rue_->ExplicitAckReceived(&explicit_packet, /*eack=*/true,
                                  /*eack_drop*/ false);

  // Tests conditions after the first EACK on kCid1.
  EXPECT_TRUE(this->peer_.IsEventQueueScheduled());
  ASSERT_EQ(this->peer_.GetNumEvents(), 1);
  EXPECT_EQ(this->peer_.GetNumResponses(), 0);
  EXPECT_TRUE(this->peer_.IsConnectionOutstanding(kCid1));
  EXPECT_FALSE(this->peer_.IsConnectionOutstanding(kCid2));
  EXPECT_EQ(this->peer_.GetAccumulatedAcks(kCid1), 0);
  EXPECT_EQ(this->peer_.GetAccumulatedAcks(kCid2), 0);
  const auto queued_event = this->peer_.FrontEvent();
  EXPECT_EQ(queued_event.num_packets_acked, pkts_acked1);
  EXPECT_EQ(this->env_.ScheduledEvents(), 1);

  // Notifies the RUE of another EACK on kCid1. This event shall be dropped.
  uint32_t pkts_acked2 = 6;
  this->peer_.IncrementNumAcked(&explicit_packet, pkts_acked2);
  this->rue_->ExplicitAckReceived(&explicit_packet, /*eack=*/true,
                                  /*eack_drop=*/false);

  // Tests conditions after the second EACK. The event queue should have the
  // first EACK.
  EXPECT_TRUE(this->peer_.IsEventQueueScheduled());
  ASSERT_EQ(this->peer_.GetNumEvents(), 1);
  EXPECT_EQ(this->peer_.GetNumResponses(), 0);
  EXPECT_TRUE(this->peer_.IsConnectionOutstanding(kCid1));
  EXPECT_FALSE(this->peer_.IsConnectionOutstanding(kCid2));
  EXPECT_EQ(this->peer_.GetAccumulatedAcks(kCid1), pkts_acked2);
  EXPECT_EQ(this->peer_.GetAccumulatedAcks(kCid2), 0);
  const auto queued_event2 = this->peer_.FrontEvent();
  EXPECT_EQ(queued_event2.num_packets_acked, pkts_acked1);
  EXPECT_EQ(this->env_.ScheduledEvents(), 1);

  // Runs the simulator to allow the RUE to process the outstanding event
  EXPECT_EQ(this->env_.ExecutedEvents(), 0);
  this->env_.Run();
  // Two more events, one for queue processing, one for handling the response
  EXPECT_EQ(this->env_.ScheduledEvents(), 3);
  EXPECT_EQ(this->env_.ExecutedEvents(), 3);

  // Tests conditions after the RUE EACK event was executed
  EXPECT_FALSE(this->peer_.IsEventQueueScheduled());
  EXPECT_EQ(this->peer_.GetNumEvents(), 0);
  EXPECT_EQ(this->peer_.GetNumResponses(), 0);
  EXPECT_FALSE(this->peer_.IsConnectionOutstanding(kCid1));
  EXPECT_FALSE(this->peer_.IsConnectionOutstanding(kCid2));
  EXPECT_EQ(this->peer_.GetAccumulatedAcks(kCid1), pkts_acked2);
  EXPECT_EQ(this->peer_.GetAccumulatedAcks(kCid2), 0);

  // Test RUE counters.
  FalconConnectionCounters host_counters =
      this->falcon_->get_stats_manager()->GetConnectionCounters(kCid1);
  EXPECT_EQ(host_counters.rue_enqueue_attempts, 2);
  EXPECT_EQ(host_counters.rue_eack_events, 1);
  EXPECT_EQ(host_counters.rue_event_drops_eack, 1);
}

// Tests the front end EACK-DROP processing of the RUE.
TYPED_TEST(ProtocolRateUpdateEngineTest, EackDropProcessing) {
  // Tests expected initial conditions
  EXPECT_FALSE(this->peer_.IsEventQueueScheduled());
  EXPECT_EQ(this->peer_.GetNumEvents(), 0);
  EXPECT_EQ(this->peer_.GetNumResponses(), 0);
  EXPECT_FALSE(this->peer_.IsConnectionOutstanding(kCid1));
  EXPECT_FALSE(this->peer_.IsConnectionOutstanding(kCid2));
  EXPECT_EQ(this->peer_.GetAccumulatedAcks(kCid1), 0);
  EXPECT_EQ(this->peer_.GetAccumulatedAcks(kCid2), 0);
  EXPECT_EQ(this->env_.ScheduledEvents(), 0);

  // Notifies the RUE of an explicit EACK on kCid1.
  Packet explicit_packet;
  explicit_packet.packet_type = falcon::PacketType::kAck;
  explicit_packet.ack.dest_cid = kCid1;
  explicit_packet.ack.timestamp_1 = absl::Nanoseconds(100001);
  explicit_packet.ack.timestamp_2 = absl::Nanoseconds(200001);
  explicit_packet.timestamps.sent_timestamp = absl::Nanoseconds(300001);
  explicit_packet.timestamps.received_timestamp = absl::Nanoseconds(400001);
  static_assert(falcon_rue::kForwardHopsBits >= 3);
  explicit_packet.ack.forward_hops = 7;
  static_assert(falcon_rue::kRxBufferLevelBits >= 3);
  explicit_packet.ack.rx_buffer_level = 4;
  static_assert(falcon_rue::kCcMetadataBits >= 16);
  explicit_packet.ack.cc_metadata = 0xDEAD;
  explicit_packet.metadata.flow_label = 1;
  uint32_t pkts_acked1 = 18;
  this->peer_.IncrementNumAcked(&explicit_packet, pkts_acked1);
  this->rue_->ExplicitAckReceived(&explicit_packet, /*eack=*/true,
                                  /*eack_drop*/ true);

  // Tests conditions after the first EACK on kCid1.
  EXPECT_TRUE(this->peer_.IsEventQueueScheduled());
  ASSERT_EQ(this->peer_.GetNumEvents(), 1);
  EXPECT_EQ(this->peer_.GetNumResponses(), 0);
  EXPECT_TRUE(this->peer_.IsConnectionOutstanding(kCid1));
  EXPECT_FALSE(this->peer_.IsConnectionOutstanding(kCid2));
  EXPECT_EQ(this->peer_.GetAccumulatedAcks(kCid1), 0);
  EXPECT_EQ(this->peer_.GetAccumulatedAcks(kCid2), 0);
  const auto queued_event = this->peer_.FrontEvent();
  EXPECT_EQ(queued_event.num_packets_acked, pkts_acked1);
  EXPECT_EQ(this->env_.ScheduledEvents(), 1);

  // Notifies the RUE of another EACK on kCid1. This event shall be dropped.
  uint32_t pkts_acked2 = 6;
  this->peer_.IncrementNumAcked(&explicit_packet, pkts_acked2);
  this->rue_->ExplicitAckReceived(&explicit_packet, /*eack=*/true,
                                  /*eack_drop=*/true);

  // Tests conditions after the second EACK. The event queue should have the
  // first EACK.
  EXPECT_TRUE(this->peer_.IsEventQueueScheduled());
  ASSERT_EQ(this->peer_.GetNumEvents(), 1);
  EXPECT_EQ(this->peer_.GetNumResponses(), 0);
  EXPECT_TRUE(this->peer_.IsConnectionOutstanding(kCid1));
  EXPECT_FALSE(this->peer_.IsConnectionOutstanding(kCid2));
  EXPECT_EQ(this->peer_.GetAccumulatedAcks(kCid1), pkts_acked2);
  EXPECT_EQ(this->peer_.GetAccumulatedAcks(kCid2), 0);
  const auto queued_event2 = this->peer_.FrontEvent();
  EXPECT_EQ(queued_event2.num_packets_acked, pkts_acked1);
  EXPECT_EQ(this->env_.ScheduledEvents(), 1);

  // Runs the simulator to allow the RUE to process the outstanding event
  EXPECT_EQ(this->env_.ExecutedEvents(), 0);
  this->env_.Run();
  // Two more events, one for queue processing, one for handling the response
  EXPECT_EQ(this->env_.ScheduledEvents(), 3);
  EXPECT_EQ(this->env_.ExecutedEvents(), 3);

  // Tests conditions after the RUE EACK event was executed
  EXPECT_FALSE(this->peer_.IsEventQueueScheduled());
  EXPECT_EQ(this->peer_.GetNumEvents(), 0);
  EXPECT_EQ(this->peer_.GetNumResponses(), 0);
  EXPECT_FALSE(this->peer_.IsConnectionOutstanding(kCid1));
  EXPECT_FALSE(this->peer_.IsConnectionOutstanding(kCid2));
  EXPECT_EQ(this->peer_.GetAccumulatedAcks(kCid1), pkts_acked2);
  EXPECT_EQ(this->peer_.GetAccumulatedAcks(kCid2), 0);

  // Test RUE counters.
  FalconConnectionCounters host_counters =
      this->falcon_->get_stats_manager()->GetConnectionCounters(kCid1);
  EXPECT_EQ(host_counters.rue_enqueue_attempts, 2);
  EXPECT_EQ(host_counters.rue_eack_drop_events, 1);
  EXPECT_EQ(host_counters.rue_event_drops_eack_drop, 1);
}

// Tests the front end NACK processing of the RUE. Then it tests execution of
// those events on the congestion control algorithm.
// NOTE: This test verifies implementation details.
TYPED_TEST(ProtocolRateUpdateEngineTest, NackProcessing) {
  // Tests expected initial conditions
  EXPECT_FALSE(this->peer_.IsEventQueueScheduled());
  EXPECT_EQ(this->peer_.GetNumEvents(), 0);
  EXPECT_EQ(this->peer_.GetNumResponses(), 0);
  EXPECT_FALSE(this->peer_.IsConnectionOutstanding(kCid1));
  EXPECT_FALSE(this->peer_.IsConnectionOutstanding(kCid2));
  EXPECT_EQ(this->peer_.GetAccumulatedAcks(kCid1), 0);
  EXPECT_EQ(this->peer_.GetAccumulatedAcks(kCid2), 0);
  EXPECT_EQ(this->env_.ScheduledEvents(), 0);

  // Notifies the RUE of a NACK
  uint32_t num_acked = 5;
  Packet nack_packet;
  nack_packet.packet_type = falcon::PacketType::kNack;
  nack_packet.nack.dest_cid = kCid1;
  nack_packet.nack.code = falcon::NackCode::kReserved;
  nack_packet.nack.timestamp_1 = absl::Nanoseconds(100001);
  nack_packet.nack.timestamp_2 = absl::Nanoseconds(200001);
  nack_packet.timestamps.sent_timestamp = absl::Nanoseconds(300001);
  nack_packet.timestamps.received_timestamp = absl::Nanoseconds(400001);
  static_assert(falcon_rue::kForwardHopsBits >= 3);
  nack_packet.nack.forward_hops = 7;
  static_assert(falcon_rue::kRxBufferLevelBits >= 3);
  nack_packet.nack.rx_buffer_level = 4;
  static_assert(falcon_rue::kCcMetadataBits >= 16);
  nack_packet.nack.cc_metadata = 0xDEAD;
  nack_packet.metadata.flow_label = 1;
  this->peer_.IncrementNumAcked(&nack_packet, num_acked);
  this->rue_->NackReceived(&nack_packet);

  // Tests conditions after the first NACK
  EXPECT_TRUE(this->peer_.IsEventQueueScheduled());
  ASSERT_EQ(this->peer_.GetNumEvents(), 1);
  EXPECT_EQ(this->peer_.GetNumResponses(), 0);
  EXPECT_TRUE(this->peer_.IsConnectionOutstanding(kCid1));
  EXPECT_FALSE(this->peer_.IsConnectionOutstanding(kCid2));
  EXPECT_EQ(this->peer_.GetAccumulatedAcks(kCid1), 0);
  EXPECT_EQ(this->peer_.GetAccumulatedAcks(kCid2), 0);
  const auto queued_event = this->peer_.FrontEvent();
  EXPECT_EQ(queued_event.num_packets_acked, num_acked);
  // EXPECT_EQ(queued_event.ecn_counter, ???);  //
  EXPECT_EQ(this->env_.ScheduledEvents(), 1);

  // Notifies the RUE of another NACK which should be ignored
  nack_packet.nack.timestamp_1 = absl::Nanoseconds(102001);
  this->peer_.IncrementNumAcked(&nack_packet, num_acked);
  this->rue_->NackReceived(&nack_packet);

  // Tests conditions after the second NACK
  EXPECT_TRUE(this->peer_.IsEventQueueScheduled());
  ASSERT_EQ(this->peer_.GetNumEvents(), 1);
  EXPECT_EQ(this->peer_.GetNumResponses(), 0);
  EXPECT_TRUE(this->peer_.IsConnectionOutstanding(kCid1));
  EXPECT_FALSE(this->peer_.IsConnectionOutstanding(kCid2));
  EXPECT_EQ(this->peer_.GetAccumulatedAcks(kCid1), num_acked);
  EXPECT_EQ(this->peer_.GetAccumulatedAcks(kCid2), 0);
  const auto queued_event2 = this->peer_.FrontEvent();
  EXPECT_EQ(queued_event2.num_packets_acked, num_acked);
  EXPECT_EQ(this->env_.ScheduledEvents(), 1);

  // Runs the simulator to allow the RUE to process the outstanding event
  EXPECT_EQ(this->env_.ExecutedEvents(), 0);
  this->env_.Run();
  // Two more events, one for queue processing, one for handling the response
  EXPECT_EQ(this->env_.ScheduledEvents(), 3);
  EXPECT_EQ(this->env_.ExecutedEvents(), 3);

  // Tests conditions after the RUE NACK event was executed
  EXPECT_FALSE(this->peer_.IsEventQueueScheduled());
  EXPECT_EQ(this->peer_.GetNumEvents(), 0);
  EXPECT_EQ(this->peer_.GetNumResponses(), 0);
  EXPECT_FALSE(this->peer_.IsConnectionOutstanding(kCid1));
  EXPECT_FALSE(this->peer_.IsConnectionOutstanding(kCid2));
  EXPECT_EQ(this->peer_.GetAccumulatedAcks(kCid1), num_acked);
  EXPECT_EQ(this->peer_.GetAccumulatedAcks(kCid2), 0);

  // Notifies the RUE of another NACK, this one should go through
  nack_packet.nack.timestamp_1 = absl::Nanoseconds(103001);
  nack_packet.nack.cc_metadata = 0xBEEF;
  this->rue_->NackReceived(&nack_packet);

  // Tests conditions after a new NACK
  EXPECT_TRUE(this->peer_.IsEventQueueScheduled());
  ASSERT_EQ(this->peer_.GetNumEvents(), 1);
  EXPECT_EQ(this->peer_.GetNumResponses(), 0);
  EXPECT_TRUE(this->peer_.IsConnectionOutstanding(kCid1));
  EXPECT_FALSE(this->peer_.IsConnectionOutstanding(kCid2));
  EXPECT_EQ(this->peer_.GetAccumulatedAcks(kCid1), 0);
  EXPECT_EQ(this->peer_.GetAccumulatedAcks(kCid2), 0);
  const auto queued_event4 = this->peer_.FrontEvent();
  EXPECT_EQ(queued_event4.num_packets_acked, num_acked);
  EXPECT_EQ(this->env_.ScheduledEvents(), 4);  // +1 from before

  // Runs the simulator to allow the RUE to process the outstanding event
  EXPECT_EQ(this->env_.ExecutedEvents(), 3);
  this->env_.Run();
  // Two more events, one for queue processing, one for handling the response
  EXPECT_EQ(this->env_.ScheduledEvents(), 6);
  EXPECT_EQ(this->env_.ExecutedEvents(), 6);

  // Tests conditions after the RUE NACK event was executed
  EXPECT_FALSE(this->peer_.IsEventQueueScheduled());
  EXPECT_EQ(this->peer_.GetNumEvents(), 0);
  EXPECT_EQ(this->peer_.GetNumResponses(), 0);
  EXPECT_FALSE(this->peer_.IsConnectionOutstanding(kCid1));
  EXPECT_FALSE(this->peer_.IsConnectionOutstanding(kCid2));
  EXPECT_EQ(this->peer_.GetAccumulatedAcks(kCid1), 0);
  EXPECT_EQ(this->peer_.GetAccumulatedAcks(kCid2), 0);

  // Test RUE counters.
  FalconConnectionCounters host_counters =
      this->falcon_->get_stats_manager()->GetConnectionCounters(kCid1);
  EXPECT_EQ(host_counters.rue_enqueue_attempts, 3);
  EXPECT_EQ(host_counters.rue_nack_events, 2);
  EXPECT_EQ(host_counters.rue_event_drops_nack, 1);
}

// Tests the following sequence: piggybacked ACK, explicit ACK, piggybacked ACK,
// NACK
// NOTE: This test verifies implementation details.
TYPED_TEST(ProtocolRateUpdateEngineTest, PackEackPackNack) {
  // Tests expected initial conditions
  EXPECT_FALSE(this->peer_.IsEventQueueScheduled());
  EXPECT_EQ(this->peer_.GetNumEvents(), 0);
  EXPECT_EQ(this->peer_.GetNumResponses(), 0);
  EXPECT_FALSE(this->peer_.IsConnectionOutstanding(kCid1));
  EXPECT_FALSE(this->peer_.IsConnectionOutstanding(kCid2));
  EXPECT_EQ(this->peer_.GetAccumulatedAcks(kCid1), 0);
  EXPECT_EQ(this->peer_.GetAccumulatedAcks(kCid2), 0);
  EXPECT_EQ(this->env_.ScheduledEvents(), 0);

  // Notifies the RUE of a piggybacked ACK
  Packet piggy_packet;
  piggy_packet.packet_type = falcon::PacketType::kPullRequest;
  piggy_packet.falcon.dest_cid = kCid1;
  piggy_packet.metadata.flow_label = 1;
  uint32_t pkts_acked1 = 18;
  this->peer_.IncrementNumAcked(&piggy_packet, pkts_acked1);

  // Tests conditions after the single piggybacked ACK
  EXPECT_FALSE(this->peer_.IsEventQueueScheduled());
  EXPECT_EQ(this->peer_.GetNumEvents(), 0);
  EXPECT_EQ(this->peer_.GetNumResponses(), 0);
  EXPECT_FALSE(this->peer_.IsConnectionOutstanding(kCid1));
  EXPECT_FALSE(this->peer_.IsConnectionOutstanding(kCid2));
  EXPECT_EQ(this->peer_.GetAccumulatedAcks(kCid1), pkts_acked1);
  EXPECT_EQ(this->peer_.GetAccumulatedAcks(kCid2), 0);
  EXPECT_EQ(this->env_.ScheduledEvents(), 0);

  // Notifies the RUE of an explicit ACK
  Packet explicit_packet;
  explicit_packet.packet_type = falcon::PacketType::kAck;
  explicit_packet.ack.dest_cid = kCid1;
  explicit_packet.ack.timestamp_1 = absl::Nanoseconds(100001);
  explicit_packet.ack.timestamp_2 = absl::Nanoseconds(200001);
  explicit_packet.timestamps.sent_timestamp = absl::Nanoseconds(300001);
  explicit_packet.timestamps.received_timestamp = absl::Nanoseconds(400001);
  static_assert(falcon_rue::kForwardHopsBits >= 3);
  explicit_packet.ack.forward_hops = 7;
  static_assert(falcon_rue::kRxBufferLevelBits >= 3);
  explicit_packet.ack.rx_buffer_level = 4;
  static_assert(falcon_rue::kCcMetadataBits >= 16);
  explicit_packet.ack.cc_metadata = 0xDEAD;
  explicit_packet.metadata.flow_label = 1;
  uint32_t pkts_acked2 = 18;
  this->peer_.IncrementNumAcked(&explicit_packet, pkts_acked2);
  this->rue_->ExplicitAckReceived(&explicit_packet, false, false);

  // Tests conditions after the explicit ACK
  EXPECT_TRUE(this->peer_.IsEventQueueScheduled());
  ASSERT_EQ(this->peer_.GetNumEvents(), 1);
  EXPECT_EQ(this->peer_.GetNumResponses(), 0);
  EXPECT_TRUE(this->peer_.IsConnectionOutstanding(kCid1));
  EXPECT_FALSE(this->peer_.IsConnectionOutstanding(kCid2));
  EXPECT_EQ(this->peer_.GetAccumulatedAcks(kCid1), 0);
  EXPECT_EQ(this->peer_.GetAccumulatedAcks(kCid2), 0);
  const auto queued_event = this->peer_.FrontEvent();
  EXPECT_EQ(queued_event.num_packets_acked, pkts_acked1 + pkts_acked2);
  // EXPECT_EQ(queued_event.ecn_counter, ???);  //
  EXPECT_EQ(this->env_.ScheduledEvents(), 1);

  // Notifies of a second piggybacked ACK on the same connection
  uint32_t pkts_acked3 = 7;
  this->peer_.IncrementNumAcked(&piggy_packet, pkts_acked3);

  // Tests conditions after the second piggybacked ACK
  EXPECT_TRUE(this->peer_.IsEventQueueScheduled());
  ASSERT_EQ(this->peer_.GetNumEvents(), 1);
  EXPECT_EQ(this->peer_.GetNumResponses(), 0);
  EXPECT_TRUE(this->peer_.IsConnectionOutstanding(kCid1));
  EXPECT_FALSE(this->peer_.IsConnectionOutstanding(kCid2));
  EXPECT_EQ(this->peer_.GetAccumulatedAcks(kCid1), pkts_acked3);
  EXPECT_EQ(this->peer_.GetAccumulatedAcks(kCid2), 0);
  const auto queued_event2 = this->peer_.FrontEvent();
  EXPECT_EQ(queued_event2.num_packets_acked, pkts_acked1 + pkts_acked2);
  // EXPECT_EQ(queued_event2.ecn_counter, ???);  //
  EXPECT_EQ(this->env_.ScheduledEvents(), 1);

  // Notifies the RUE of a NACK, which should be ignored but the accumulator
  // will be kept.
  Packet nack_packet;
  nack_packet.packet_type = falcon::PacketType::kNack;
  nack_packet.nack.dest_cid = kCid1;
  nack_packet.nack.code = falcon::NackCode::kReserved;
  nack_packet.nack.timestamp_1 = absl::Nanoseconds(500001);
  nack_packet.nack.timestamp_2 = absl::Nanoseconds(600001);
  nack_packet.timestamps.sent_timestamp = absl::Nanoseconds(700001);
  nack_packet.timestamps.received_timestamp = absl::Nanoseconds(800001);
  static_assert(falcon_rue::kForwardHopsBits >= 3);
  nack_packet.nack.forward_hops = 7;
  static_assert(falcon_rue::kRxBufferLevelBits >= 3);
  nack_packet.nack.rx_buffer_level = 4;
  static_assert(falcon_rue::kCcMetadataBits >= 16);
  nack_packet.nack.cc_metadata = 0xDEAD;
  nack_packet.metadata.flow_label = 1;
  this->rue_->NackReceived(&nack_packet);

  // Tests conditions after the NACK
  EXPECT_TRUE(this->peer_.IsEventQueueScheduled());
  ASSERT_EQ(this->peer_.GetNumEvents(), 1);
  EXPECT_EQ(this->peer_.GetNumResponses(), 0);
  EXPECT_TRUE(this->peer_.IsConnectionOutstanding(kCid1));
  EXPECT_FALSE(this->peer_.IsConnectionOutstanding(kCid2));
  EXPECT_EQ(this->peer_.GetAccumulatedAcks(kCid1), pkts_acked3);
  EXPECT_EQ(this->peer_.GetAccumulatedAcks(kCid2), 0);
  const auto queued_event3 = this->peer_.FrontEvent();
  EXPECT_EQ(queued_event3.num_packets_acked, pkts_acked1 + pkts_acked2);
  // EXPECT_EQ(queued_event3.ecn_counter, ???);  //
  EXPECT_EQ(this->env_.ScheduledEvents(), 1);

  // Runs the simulator to allow the RUE to process the outstanding event
  EXPECT_EQ(this->env_.ExecutedEvents(), 0);
  this->env_.Run();
  // Two more events, one for queue processing, one for handling the response
  EXPECT_EQ(this->env_.ScheduledEvents(), 3);
  EXPECT_EQ(this->env_.ExecutedEvents(), 3);

  // Tests conditions after the RUE ACK event was executed
  EXPECT_FALSE(this->peer_.IsEventQueueScheduled());
  EXPECT_EQ(this->peer_.GetNumEvents(), 0);
  EXPECT_EQ(this->peer_.GetNumResponses(), 0);
  EXPECT_FALSE(this->peer_.IsConnectionOutstanding(kCid1));
  EXPECT_FALSE(this->peer_.IsConnectionOutstanding(kCid2));
  EXPECT_EQ(this->peer_.GetAccumulatedAcks(kCid1), pkts_acked3);
  EXPECT_EQ(this->peer_.GetAccumulatedAcks(kCid2), 0);
}

// Tests the front end timeout processing of the RUE. Then it tests execution of
// those events on the congestion control algorithm.
// NOTE: This test verifies implementation details.
TYPED_TEST(ProtocolRateUpdateEngineTest, TimeoutProcessing) {
  // Tests expected initial conditions
  EXPECT_FALSE(this->peer_.IsEventQueueScheduled());
  EXPECT_EQ(this->peer_.GetNumEvents(), 0);
  EXPECT_EQ(this->peer_.GetNumResponses(), 0);
  EXPECT_FALSE(this->peer_.IsConnectionOutstanding(kCid1));
  EXPECT_FALSE(this->peer_.IsConnectionOutstanding(kCid2));
  EXPECT_EQ(this->peer_.GetAccumulatedAcks(kCid1), 0);
  EXPECT_EQ(this->peer_.GetAccumulatedAcks(kCid2), 0);
  EXPECT_EQ(this->env_.ScheduledEvents(), 0);

  // Notifies the RUE of a piggybacked ACK
  Packet piggy_packet;
  piggy_packet.packet_type = falcon::PacketType::kPullRequest;
  piggy_packet.falcon.dest_cid = kCid1;
  piggy_packet.metadata.flow_label = 1;
  uint32_t pkts_acked1 = 18;
  this->peer_.IncrementNumAcked(&piggy_packet, pkts_acked1);

  // Tests conditions after the single piggybacked ACK
  EXPECT_FALSE(this->peer_.IsEventQueueScheduled());
  EXPECT_EQ(this->peer_.GetNumEvents(), 0);
  EXPECT_EQ(this->peer_.GetNumResponses(), 0);
  EXPECT_FALSE(this->peer_.IsConnectionOutstanding(kCid1));
  EXPECT_FALSE(this->peer_.IsConnectionOutstanding(kCid2));
  EXPECT_EQ(this->peer_.GetAccumulatedAcks(kCid1), pkts_acked1);
  EXPECT_EQ(this->peer_.GetAccumulatedAcks(kCid2), 0);
  EXPECT_EQ(this->env_.ScheduledEvents(), 0);

  // Notifies the RUE of a timeout retransmission
  Packet retx_packet;
  retx_packet.packet_type = falcon::PacketType::kPullRequest;
  retx_packet.metadata.scid = kCid1;
  retx_packet.metadata.flow_label = 1;
  this->rue_->PacketTimeoutRetransmitted(kCid1, &retx_packet, 2);

  // Tests conditions after the first NACK
  EXPECT_TRUE(this->peer_.IsEventQueueScheduled());
  ASSERT_EQ(this->peer_.GetNumEvents(), 1);
  EXPECT_EQ(this->peer_.GetNumResponses(), 0);
  EXPECT_TRUE(this->peer_.IsConnectionOutstanding(kCid1));
  EXPECT_FALSE(this->peer_.IsConnectionOutstanding(kCid2));
  EXPECT_EQ(this->peer_.GetAccumulatedAcks(kCid1), 0);  // cleared
  EXPECT_EQ(this->peer_.GetAccumulatedAcks(kCid2), 0);
  // EXPECT_EQ(queued_event.ecn_counter, ???);  //
  EXPECT_EQ(this->env_.ScheduledEvents(), 1);

  // Notifies the RUE of another NACK which should be ignored
  this->rue_->PacketTimeoutRetransmitted(kCid1, &retx_packet, 3);

  // Tests conditions after the second timeout
  EXPECT_TRUE(this->peer_.IsEventQueueScheduled());
  ASSERT_EQ(this->peer_.GetNumEvents(), 1);
  EXPECT_EQ(this->peer_.GetNumResponses(), 0);
  EXPECT_TRUE(this->peer_.IsConnectionOutstanding(kCid1));
  EXPECT_FALSE(this->peer_.IsConnectionOutstanding(kCid2));
  EXPECT_EQ(this->peer_.GetAccumulatedAcks(kCid1), 0);
  EXPECT_EQ(this->peer_.GetAccumulatedAcks(kCid2), 0);
  const auto queued_event2 = this->peer_.FrontEvent();
  EXPECT_EQ(queued_event2.retransmit_count, 2);
  EXPECT_EQ(this->env_.ScheduledEvents(), 1);

  // Runs the simulator to allow the RUE to process the outstanding event
  EXPECT_EQ(this->env_.ExecutedEvents(), 0);
  this->env_.Run();
  // Two more events, one for queue processing, one for handling the response
  EXPECT_EQ(this->env_.ScheduledEvents(), 3);
  EXPECT_EQ(this->env_.ExecutedEvents(), 3);

  // Test RUE counters.
  FalconConnectionCounters host_counters =
      this->falcon_->get_stats_manager()->GetConnectionCounters(kCid1);
  EXPECT_EQ(host_counters.rue_enqueue_attempts, 2);
  EXPECT_EQ(host_counters.rue_retransmit_events, 1);
  EXPECT_EQ(host_counters.rue_event_drops_retransmit, 1);
}

TYPED_TEST(ProtocolRateUpdateEngineTest, EventQueueOverflow) {
  // Tests expected initial conditions
  EXPECT_FALSE(this->peer_.IsEventQueueScheduled());
  EXPECT_EQ(this->peer_.GetNumEvents(), 0);
  EXPECT_EQ(this->peer_.GetNumResponses(), 0);
  EXPECT_EQ(this->env_.ScheduledEvents(), 0);

  // Insert events to fully fill the event queue.
  int cid = 1;
  for (; cid <= kEventQueueLimit; cid++) {
    auto metadata = FalconTestingHelpers::InitializeConnectionMetadata(
        this->falcon_.get(), cid);
    FalconTestingHelpers::InitializeConnectionState(this->falcon_.get(),
                                                    std::move(metadata));
    Packet explicit_packet;
    explicit_packet.packet_type = falcon::PacketType::kAck;
    explicit_packet.ack.dest_cid = cid;
    explicit_packet.metadata.flow_label = 1;
    this->peer_.IncrementNumAcked(&explicit_packet, 1);
    this->rue_->ExplicitAckReceived(&explicit_packet, false, false);
    EXPECT_EQ(this->peer_.GetNumEvents(), cid);
    EXPECT_TRUE(this->peer_.IsConnectionOutstanding(cid));
    // Test RUE counters.
    FalconConnectionCounters host_counters =
        this->falcon_->get_stats_manager()->GetConnectionCounters(cid);
    EXPECT_EQ(host_counters.rue_enqueue_attempts, 1);
    EXPECT_EQ(host_counters.rue_ack_events, 1);
    EXPECT_EQ(host_counters.rue_event_drops_ack, 0);
  }
  // Insert one more event and expect drop.
  auto metadata = FalconTestingHelpers::InitializeConnectionMetadata(
      this->falcon_.get(), cid);
  FalconTestingHelpers::InitializeConnectionState(this->falcon_.get(),
                                                  std::move(metadata));
  EXPECT_FALSE(this->peer_.IsConnectionOutstanding(cid));
  Packet explicit_packet;
  explicit_packet.packet_type = falcon::PacketType::kAck;
  explicit_packet.ack.dest_cid = cid;
  explicit_packet.metadata.flow_label = 1;
  this->peer_.IncrementNumAcked(&explicit_packet, 1);
  this->rue_->ExplicitAckReceived(&explicit_packet, false, false);
  EXPECT_EQ(this->peer_.GetNumEvents(), kEventQueueLimit);
  EXPECT_FALSE(this->peer_.IsConnectionOutstanding(cid));
  // Test RUE counters.
  FalconConnectionCounters host_counters =
      this->falcon_->get_stats_manager()->GetConnectionCounters(cid);
  EXPECT_EQ(host_counters.rue_enqueue_attempts, 1);
  EXPECT_EQ(host_counters.rue_ack_events, 0);
  EXPECT_EQ(host_counters.rue_event_drops_ack, 1);
}

TYPED_TEST(ProtocolRateUpdateEngineTest, EventQueueRateLimiterThreshold1) {
  // Config the rate limiter.
  uint64_t threshold1 = 60, threshold2 = 80, threshold3 = 90;
  this->peer_.set_event_queue_thresholds(threshold1, threshold2, threshold3);
  absl::Duration time_threshold = absl::Microseconds(25);
  this->peer_.set_predicate_1_time_threshold(time_threshold);
  uint32_t packet_count_threshold = 10;
  this->peer_.set_predicate_2_packet_count_threshold(packet_count_threshold);

  // Tests expected initial conditions
  EXPECT_FALSE(this->peer_.IsEventQueueScheduled());
  EXPECT_EQ(this->peer_.GetNumEvents(), 0);
  EXPECT_EQ(this->peer_.GetNumResponses(), 0);
  EXPECT_EQ(this->env_.ScheduledEvents(), 0);

  // Insert events to reach threshold1.
  int cid = 1;
  for (; cid <= threshold1; cid++) {
    auto metadata = FalconTestingHelpers::InitializeConnectionMetadata(
        this->falcon_.get(), cid);
    FalconTestingHelpers::InitializeConnectionState(this->falcon_.get(),
                                                    std::move(metadata));
    Packet explicit_packet;
    explicit_packet.packet_type = falcon::PacketType::kAck;
    explicit_packet.ack.dest_cid = cid;
    explicit_packet.metadata.flow_label = 1;
    this->peer_.IncrementNumAcked(&explicit_packet, 1);
    this->rue_->ExplicitAckReceived(&explicit_packet, false, false);
    EXPECT_EQ(this->peer_.GetNumEvents(), cid);
    EXPECT_TRUE(this->peer_.IsConnectionOutstanding(cid));
    // Test RUE counters.
    FalconConnectionCounters host_counters =
        this->falcon_->get_stats_manager()->GetConnectionCounters(cid);
    EXPECT_EQ(host_counters.rue_enqueue_attempts, 1);
    EXPECT_EQ(host_counters.rue_ack_events, 1);
    EXPECT_EQ(host_counters.rue_event_drops_ack, 0);
  }

  // Prepare to test p1 || p2 between threshold1 and threshold2.
  // All events finishes. But just before time_threshold.
  this->env_.RunFor(time_threshold - absl::Nanoseconds(1));
  EXPECT_EQ(this->peer_.GetNumEvents(), 0);
  // Insert events to reach threshold1. New : existing connections = 1:1.
  for (cid = threshold1 / 2; cid <= threshold1 / 2 + threshold1; cid++) {
    auto metadata = FalconTestingHelpers::InitializeConnectionMetadata(
        this->falcon_.get(), cid);
    FalconTestingHelpers::InitializeConnectionState(this->falcon_.get(),
                                                    std::move(metadata),
                                                    /*expect_ok=*/false);
    Packet explicit_packet;
    explicit_packet.packet_type = falcon::PacketType::kAck;
    explicit_packet.ack.dest_cid = cid;
    explicit_packet.metadata.flow_label = 1;
    this->peer_.IncrementNumAcked(&explicit_packet, 1);
    this->rue_->ExplicitAckReceived(&explicit_packet, false, false);
    EXPECT_EQ(this->peer_.GetNumEvents(), cid - threshold1 / 2 + 1);
    EXPECT_TRUE(this->peer_.IsConnectionOutstanding(cid));
    // Test RUE counters.
    FalconConnectionCounters host_counters =
        this->falcon_->get_stats_manager()->GetConnectionCounters(cid);
    EXPECT_EQ(host_counters.rue_enqueue_attempts, cid <= threshold1 ? 2 : 1);
    EXPECT_EQ(host_counters.rue_ack_events, cid <= threshold1 ? 2 : 1);
    EXPECT_EQ(host_counters.rue_event_drops_ack, 0);
  }
  // cid=1 with num_ack < 10 should fail.
  {
    cid = 1;
    // cid=1 should have already finished.
    EXPECT_FALSE(this->peer_.IsConnectionOutstanding(cid));
    Packet explicit_packet;
    explicit_packet.packet_type = falcon::PacketType::kAck;
    explicit_packet.ack.dest_cid = cid;
    explicit_packet.metadata.flow_label = 1;
    this->peer_.IncrementNumAcked(&explicit_packet, 4);
    this->rue_->ExplicitAckReceived(&explicit_packet, false, false);
    // Should fail to insert.
    EXPECT_FALSE(this->peer_.IsConnectionOutstanding(cid));
    EXPECT_EQ(this->peer_.GetNumEvents(), threshold1 + 1);
    // Test RUE counters.
    FalconConnectionCounters host_counters =
        this->falcon_->get_stats_manager()->GetConnectionCounters(cid);
    EXPECT_EQ(host_counters.rue_enqueue_attempts, 2);
    EXPECT_EQ(host_counters.rue_ack_events, 1);
    EXPECT_EQ(host_counters.rue_event_drops_ack, 1);
  }
  // cid=1000 should pass p1.
  {
    cid = 1000;
    auto metadata = FalconTestingHelpers::InitializeConnectionMetadata(
        this->falcon_.get(), cid);
    FalconTestingHelpers::InitializeConnectionState(this->falcon_.get(),
                                                    std::move(metadata));
    Packet explicit_packet;
    explicit_packet.packet_type = falcon::PacketType::kAck;
    explicit_packet.ack.dest_cid = cid;
    explicit_packet.metadata.flow_label = 1;
    this->peer_.IncrementNumAcked(&explicit_packet, 1);
    this->rue_->ExplicitAckReceived(&explicit_packet, false, false);
    // Should pass.
    EXPECT_TRUE(this->peer_.IsConnectionOutstanding(cid));
    EXPECT_EQ(this->peer_.GetNumEvents(), threshold1 + 2);
    // Test RUE counters.
    FalconConnectionCounters host_counters =
        this->falcon_->get_stats_manager()->GetConnectionCounters(cid);
    EXPECT_EQ(host_counters.rue_enqueue_attempts, 1);
    EXPECT_EQ(host_counters.rue_ack_events, 1);
    EXPECT_EQ(host_counters.rue_event_drops_ack, 0);
  }
  // cid = 2 with num_ack = 10 should pass p2.
  {
    cid = 2;
    // cid=2 should have already finished.
    EXPECT_FALSE(this->peer_.IsConnectionOutstanding(cid));
    Packet explicit_packet;
    explicit_packet.packet_type = falcon::PacketType::kAck;
    explicit_packet.ack.dest_cid = cid;
    explicit_packet.metadata.flow_label = 1;
    this->peer_.IncrementNumAcked(&explicit_packet, 10);
    this->rue_->ExplicitAckReceived(&explicit_packet, false, false);
    // Should pass.
    EXPECT_TRUE(this->peer_.IsConnectionOutstanding(cid));
    EXPECT_EQ(this->peer_.GetNumEvents(), threshold1 + 3);
    // Test RUE counters.
    FalconConnectionCounters host_counters =
        this->falcon_->get_stats_manager()->GetConnectionCounters(cid);
    EXPECT_EQ(host_counters.rue_enqueue_attempts, 2);
    EXPECT_EQ(host_counters.rue_ack_events, 2);
    EXPECT_EQ(host_counters.rue_event_drops_ack, 0);
  }
  // cid=1 with num_ack < 10 should pass p1 when time_threshold has passed.
  this->env_.RunFor(
      absl::Nanoseconds(1));  // Just enough to reach time_threshold.
  EXPECT_EQ(this->peer_.GetNumEvents(),
            threshold1 + 2);  // 1st event is processed after 1 ns.
  {
    cid = 1;
    // cid=1 should have already finished.
    EXPECT_FALSE(this->peer_.IsConnectionOutstanding(cid));
    Packet explicit_packet;
    explicit_packet.packet_type = falcon::PacketType::kAck;
    explicit_packet.ack.dest_cid = cid;
    explicit_packet.metadata.flow_label = 1;
    this->peer_.IncrementNumAcked(&explicit_packet, 1);
    this->rue_->ExplicitAckReceived(&explicit_packet, false, false);
    // Should pass.
    EXPECT_TRUE(this->peer_.IsConnectionOutstanding(cid));
    EXPECT_EQ(this->peer_.GetNumEvents(), threshold1 + 3);
    // Test RUE counters.
    FalconConnectionCounters host_counters =
        this->falcon_->get_stats_manager()->GetConnectionCounters(cid);
    EXPECT_EQ(host_counters.rue_enqueue_attempts, 3);
    EXPECT_EQ(host_counters.rue_ack_events, 2);
    EXPECT_EQ(host_counters.rue_event_drops_ack, 1);
  }
}

TYPED_TEST(ProtocolRateUpdateEngineTest, EventQueueRateLimiterThreshold2) {
  // Config the rate limiter.
  uint64_t threshold1 = 60, threshold2 = 80, threshold3 = 90;
  this->peer_.set_event_queue_thresholds(threshold1, threshold2, threshold3);
  absl::Duration time_threshold = absl::Microseconds(25);
  this->peer_.set_predicate_1_time_threshold(time_threshold);
  uint32_t packet_count_threshold = 10;
  this->peer_.set_predicate_2_packet_count_threshold(packet_count_threshold);

  // Tests expected initial conditions
  EXPECT_FALSE(this->peer_.IsEventQueueScheduled());
  EXPECT_EQ(this->peer_.GetNumEvents(), 0);
  EXPECT_EQ(this->peer_.GetNumResponses(), 0);
  EXPECT_EQ(this->env_.ScheduledEvents(), 0);

  // Insert events to reach threshold1.
  int cid = 1;
  for (; cid <= threshold1; cid++) {
    auto metadata = FalconTestingHelpers::InitializeConnectionMetadata(
        this->falcon_.get(), cid);
    FalconTestingHelpers::InitializeConnectionState(this->falcon_.get(),
                                                    std::move(metadata));
    Packet explicit_packet;
    explicit_packet.packet_type = falcon::PacketType::kAck;
    explicit_packet.ack.dest_cid = cid;
    explicit_packet.metadata.flow_label = 1;
    this->peer_.IncrementNumAcked(&explicit_packet, 1);
    this->rue_->ExplicitAckReceived(&explicit_packet, false, false);
    EXPECT_EQ(this->peer_.GetNumEvents(), cid);
    EXPECT_TRUE(this->peer_.IsConnectionOutstanding(cid));
    // Test RUE counters.
    FalconConnectionCounters host_counters =
        this->falcon_->get_stats_manager()->GetConnectionCounters(cid);
    EXPECT_EQ(host_counters.rue_enqueue_attempts, 1);
    EXPECT_EQ(host_counters.rue_ack_events, 1);
    EXPECT_EQ(host_counters.rue_event_drops_ack, 0);
  }

  // Prepare to test p1 between threshold2 and threshold3.
  // All events finishes. But just before time_threshold.
  this->env_.RunFor(time_threshold - absl::Nanoseconds(2));
  EXPECT_EQ(this->peer_.GetNumEvents(), 0);
  // Insert events to reach threshold2.
  for (cid = threshold1 / 2 + 1; cid <= threshold1 / 2 + threshold2 + 1;
       cid++) {
    auto metadata = FalconTestingHelpers::InitializeConnectionMetadata(
        this->falcon_.get(), cid);
    FalconTestingHelpers::InitializeConnectionState(this->falcon_.get(),
                                                    std::move(metadata),
                                                    /*expect_ok=*/false);
    Packet explicit_packet;
    explicit_packet.packet_type = falcon::PacketType::kAck;
    explicit_packet.ack.dest_cid = cid;
    explicit_packet.metadata.flow_label = 1;
    this->peer_.IncrementNumAcked(&explicit_packet, 1);
    this->rue_->ExplicitAckReceived(&explicit_packet, false, false);
    EXPECT_EQ(this->peer_.GetNumEvents(), cid - threshold1 / 2);
    EXPECT_TRUE(this->peer_.IsConnectionOutstanding(cid));
    // Test RUE counters.
    FalconConnectionCounters host_counters =
        this->falcon_->get_stats_manager()->GetConnectionCounters(cid);
    auto attempts = cid <= threshold1 ? 2 : 1;
    EXPECT_EQ(host_counters.rue_enqueue_attempts, attempts);
    EXPECT_EQ(host_counters.rue_ack_events, attempts);
    EXPECT_EQ(host_counters.rue_event_drops_ack, 0);
  }
  // cid=1 with num_ack < 10 should fail.
  {
    cid = 1;
    // cid=1 should have already finished.
    EXPECT_FALSE(this->peer_.IsConnectionOutstanding(cid));
    Packet explicit_packet;
    explicit_packet.packet_type = falcon::PacketType::kAck;
    explicit_packet.ack.dest_cid = cid;
    explicit_packet.metadata.flow_label = 1;
    this->peer_.IncrementNumAcked(&explicit_packet, 4);
    this->rue_->ExplicitAckReceived(&explicit_packet, false, false);
    // Should fail to insert.
    EXPECT_FALSE(this->peer_.IsConnectionOutstanding(cid));
    EXPECT_EQ(this->peer_.GetNumEvents(), threshold2 + 1);
    // Test RUE counters.
    FalconConnectionCounters host_counters =
        this->falcon_->get_stats_manager()->GetConnectionCounters(cid);
    EXPECT_EQ(host_counters.rue_enqueue_attempts, 2);
    EXPECT_EQ(host_counters.rue_ack_events, 1);
    EXPECT_EQ(host_counters.rue_event_drops_ack, 1);
  }
  // cid=1001 should pass p1.
  {
    cid = 1001;
    auto metadata = FalconTestingHelpers::InitializeConnectionMetadata(
        this->falcon_.get(), cid);
    FalconTestingHelpers::InitializeConnectionState(this->falcon_.get(),
                                                    std::move(metadata));
    Packet explicit_packet;
    explicit_packet.packet_type = falcon::PacketType::kAck;
    explicit_packet.ack.dest_cid = cid;
    explicit_packet.metadata.flow_label = 1;
    this->peer_.IncrementNumAcked(&explicit_packet, 1);
    this->rue_->ExplicitAckReceived(&explicit_packet, false, false);
    // Should pass.
    EXPECT_TRUE(this->peer_.IsConnectionOutstanding(cid));
    EXPECT_EQ(this->peer_.GetNumEvents(), threshold2 + 2);
    // Test RUE counters.
    FalconConnectionCounters host_counters =
        this->falcon_->get_stats_manager()->GetConnectionCounters(cid);
    EXPECT_EQ(host_counters.rue_enqueue_attempts, 1);
    EXPECT_EQ(host_counters.rue_ack_events, 1);
    EXPECT_EQ(host_counters.rue_event_drops_ack, 0);
  }
  // cid = 2 with num_ack = 10 should fail.
  {
    cid = 2;
    // cid=2 should have already finished.
    EXPECT_FALSE(this->peer_.IsConnectionOutstanding(cid));
    Packet explicit_packet;
    explicit_packet.packet_type = falcon::PacketType::kAck;
    explicit_packet.ack.dest_cid = cid;
    explicit_packet.metadata.flow_label = 1;
    this->peer_.IncrementNumAcked(&explicit_packet, 10);
    this->rue_->ExplicitAckReceived(&explicit_packet, false, false);
    // Should fail.
    EXPECT_FALSE(this->peer_.IsConnectionOutstanding(cid));
    EXPECT_EQ(this->peer_.GetNumEvents(), threshold2 + 2);
    // Test RUE counters.
    FalconConnectionCounters host_counters =
        this->falcon_->get_stats_manager()->GetConnectionCounters(cid);
    EXPECT_EQ(host_counters.rue_enqueue_attempts, 2);
    EXPECT_EQ(host_counters.rue_ack_events, 1);
    EXPECT_EQ(host_counters.rue_event_drops_ack, 1);
  }
  // cid=1 with num_ack < 10 should pass p1 when time_threshold has passed.
  this->env_.RunFor(
      absl::Nanoseconds(2));  // Just enough to reach time_threshold.
  EXPECT_EQ(this->peer_.GetNumEvents(),
            threshold2 + 1);  // 1st event is processed after 1 ns.
  {
    cid = 1;
    // cid=1 should have already finished.
    EXPECT_FALSE(this->peer_.IsConnectionOutstanding(cid));
    Packet explicit_packet;
    explicit_packet.packet_type = falcon::PacketType::kAck;
    explicit_packet.ack.dest_cid = cid;
    explicit_packet.metadata.flow_label = 1;
    this->peer_.IncrementNumAcked(&explicit_packet, 1);
    this->rue_->ExplicitAckReceived(&explicit_packet, false, false);
    // Should pass.
    EXPECT_TRUE(this->peer_.IsConnectionOutstanding(cid));
    EXPECT_EQ(this->peer_.GetNumEvents(), threshold2 + 2);
    // Test RUE counters.
    FalconConnectionCounters host_counters =
        this->falcon_->get_stats_manager()->GetConnectionCounters(cid);
    EXPECT_EQ(host_counters.rue_enqueue_attempts, 3);
    EXPECT_EQ(host_counters.rue_ack_events, 2);
    EXPECT_EQ(host_counters.rue_event_drops_ack, 1);
  }
}

TYPED_TEST(ProtocolRateUpdateEngineTest, EventQueueRateLimiterThreshold3) {
  // Config the rate limiter.
  uint64_t threshold1 = 60, threshold2 = 80, threshold3 = 90;
  this->peer_.set_event_queue_thresholds(threshold1, threshold2, threshold3);
  absl::Duration time_threshold = absl::Microseconds(25);
  this->peer_.set_predicate_1_time_threshold(time_threshold);
  uint32_t packet_count_threshold = 10;
  this->peer_.set_predicate_2_packet_count_threshold(packet_count_threshold);

  // Tests expected initial conditions
  EXPECT_FALSE(this->peer_.IsEventQueueScheduled());
  EXPECT_EQ(this->peer_.GetNumEvents(), 0);
  EXPECT_EQ(this->peer_.GetNumResponses(), 0);
  EXPECT_EQ(this->env_.ScheduledEvents(), 0);

  // Insert events to reach threshold1.
  int cid = 1;
  for (; cid <= threshold1; cid++) {
    auto metadata = FalconTestingHelpers::InitializeConnectionMetadata(
        this->falcon_.get(), cid);
    FalconTestingHelpers::InitializeConnectionState(this->falcon_.get(),
                                                    std::move(metadata));
    Packet explicit_packet;
    explicit_packet.packet_type = falcon::PacketType::kAck;
    explicit_packet.ack.dest_cid = cid;
    explicit_packet.metadata.flow_label = 1;
    this->peer_.IncrementNumAcked(&explicit_packet, 1);
    explicit_packet.metadata.flow_label = 1;
    this->rue_->ExplicitAckReceived(&explicit_packet, false, false);
    EXPECT_EQ(this->peer_.GetNumEvents(), cid);
    EXPECT_TRUE(this->peer_.IsConnectionOutstanding(cid));
    // Test RUE counters.
    FalconConnectionCounters host_counters =
        this->falcon_->get_stats_manager()->GetConnectionCounters(cid);
    EXPECT_EQ(host_counters.rue_enqueue_attempts, 1);
    EXPECT_EQ(host_counters.rue_ack_events, 1);
    EXPECT_EQ(host_counters.rue_event_drops_ack, 0);
  }

  // Prepare to test p1&&p2 between threshold3 and queue size.
  // All events finishes. But just before time_threshold.
  this->env_.RunFor(time_threshold - absl::Nanoseconds(3));
  EXPECT_EQ(this->peer_.GetNumEvents(), 0);
  // Insert events to reach threshold3.
  for (cid = 10000 + 1; cid <= 10000 + threshold3 + 1; cid++) {
    auto metadata = FalconTestingHelpers::InitializeConnectionMetadata(
        this->falcon_.get(), cid);
    FalconTestingHelpers::InitializeConnectionState(this->falcon_.get(),
                                                    std::move(metadata));
    Packet explicit_packet;
    explicit_packet.packet_type = falcon::PacketType::kAck;
    explicit_packet.ack.dest_cid = cid;
    explicit_packet.metadata.flow_label = 1;
    this->peer_.IncrementNumAcked(&explicit_packet, 10);
    this->rue_->ExplicitAckReceived(&explicit_packet, false, false);
    EXPECT_EQ(this->peer_.GetNumEvents(), cid - 10000);
    EXPECT_TRUE(this->peer_.IsConnectionOutstanding(cid));
    // Test RUE counters.
    FalconConnectionCounters host_counters =
        this->falcon_->get_stats_manager()->GetConnectionCounters(cid);
    EXPECT_EQ(host_counters.rue_enqueue_attempts, 1);
    EXPECT_EQ(host_counters.rue_ack_events, 1);
    EXPECT_EQ(host_counters.rue_event_drops_ack, 0);
  }
  // cid=1002 should fail.
  {
    cid = 1002;
    auto metadata = FalconTestingHelpers::InitializeConnectionMetadata(
        this->falcon_.get(), cid);
    FalconTestingHelpers::InitializeConnectionState(this->falcon_.get(),
                                                    std::move(metadata));
    Packet explicit_packet;
    explicit_packet.packet_type = falcon::PacketType::kAck;
    explicit_packet.ack.dest_cid = cid;
    explicit_packet.metadata.flow_label = 1;
    this->peer_.IncrementNumAcked(&explicit_packet, 1);
    this->rue_->ExplicitAckReceived(&explicit_packet, false, false);
    // Should fail.
    EXPECT_FALSE(this->peer_.IsConnectionOutstanding(cid));
    EXPECT_EQ(this->peer_.GetNumEvents(), threshold3 + 1);
    // Test RUE counters.
    FalconConnectionCounters host_counters =
        this->falcon_->get_stats_manager()->GetConnectionCounters(cid);
    EXPECT_EQ(host_counters.rue_enqueue_attempts, 1);
    EXPECT_EQ(host_counters.rue_ack_events, 0);
    EXPECT_EQ(host_counters.rue_event_drops_ack, 1);
  }
  // cid = 3 with num_ack = 10 should fail.
  {
    cid = 3;
    // cid=3 should have already finished.
    EXPECT_FALSE(this->peer_.IsConnectionOutstanding(cid));
    Packet explicit_packet;
    explicit_packet.packet_type = falcon::PacketType::kAck;
    explicit_packet.ack.dest_cid = cid;
    explicit_packet.metadata.flow_label = 1;
    this->peer_.IncrementNumAcked(&explicit_packet, 10);
    this->rue_->ExplicitAckReceived(&explicit_packet, false, false);
    // Should fail.
    EXPECT_FALSE(this->peer_.IsConnectionOutstanding(cid));
    EXPECT_EQ(this->peer_.GetNumEvents(), threshold3 + 1);
    // Test RUE counters.
    FalconConnectionCounters host_counters =
        this->falcon_->get_stats_manager()->GetConnectionCounters(cid);
    EXPECT_EQ(host_counters.rue_enqueue_attempts, 2);
    EXPECT_EQ(host_counters.rue_ack_events, 1);
    EXPECT_EQ(host_counters.rue_event_drops_ack, 1);
  }
  // cid=1 with num_ack < 10 should fail.
  this->env_.RunFor(
      absl::Nanoseconds(3));  // Just enough to reach time_threshold.
  {
    cid = 1;
    // cid=1 should have already finished.
    EXPECT_FALSE(this->peer_.IsConnectionOutstanding(cid));
    Packet explicit_packet;
    explicit_packet.packet_type = falcon::PacketType::kAck;
    explicit_packet.ack.dest_cid = cid;
    explicit_packet.metadata.flow_label = 1;
    this->peer_.IncrementNumAcked(&explicit_packet, 1);
    this->rue_->ExplicitAckReceived(&explicit_packet, false, false);
    // Should fail.
    EXPECT_FALSE(this->peer_.IsConnectionOutstanding(cid));
    EXPECT_EQ(this->peer_.GetNumEvents(), threshold3);
    // Test RUE counters.
    FalconConnectionCounters host_counters =
        this->falcon_->get_stats_manager()->GetConnectionCounters(cid);
    EXPECT_EQ(host_counters.rue_enqueue_attempts, 2);
    EXPECT_EQ(host_counters.rue_ack_events, 1);
    EXPECT_EQ(host_counters.rue_event_drops_ack, 1);

    // another cid=1 with num_ack==9 should pass.
    this->peer_.IncrementNumAcked(&explicit_packet, 9);
    this->rue_->ExplicitAckReceived(&explicit_packet, false, false);
    EXPECT_TRUE(this->peer_.IsConnectionOutstanding(cid));
    EXPECT_EQ(this->peer_.GetNumEvents(), threshold3 + 1);
    // Test RUE counters.
    host_counters =
        this->falcon_->get_stats_manager()->GetConnectionCounters(cid);
    EXPECT_EQ(host_counters.rue_enqueue_attempts, 3);
    EXPECT_EQ(host_counters.rue_ack_events, 2);
    EXPECT_EQ(host_counters.rue_event_drops_ack, 1);
  }
}

TYPED_TEST(ProtocolRateUpdateEngineTest, EventQueueRateLimiterQueueSize) {
  // Config the rate limiter.
  uint64_t threshold1 = 60, threshold2 = 80, threshold3 = 90;
  this->peer_.set_event_queue_thresholds(threshold1, threshold2, threshold3);
  absl::Duration time_threshold = absl::Microseconds(25);
  this->peer_.set_predicate_1_time_threshold(time_threshold);
  uint32_t packet_count_threshold = 10;
  this->peer_.set_predicate_2_packet_count_threshold(packet_count_threshold);

  // Tests expected initial conditions
  EXPECT_FALSE(this->peer_.IsEventQueueScheduled());
  EXPECT_EQ(this->peer_.GetNumEvents(), 0);
  EXPECT_EQ(this->peer_.GetNumResponses(), 0);
  EXPECT_EQ(this->env_.ScheduledEvents(), 0);

  // Insert events to reach threshold1.
  int cid = 1;
  for (; cid <= threshold1; cid++) {
    auto metadata = FalconTestingHelpers::InitializeConnectionMetadata(
        this->falcon_.get(), cid);
    FalconTestingHelpers::InitializeConnectionState(this->falcon_.get(),
                                                    std::move(metadata));
    Packet explicit_packet;
    explicit_packet.packet_type = falcon::PacketType::kAck;
    explicit_packet.ack.dest_cid = cid;
    explicit_packet.metadata.flow_label = 1;
    this->peer_.IncrementNumAcked(&explicit_packet, 1);
    this->rue_->ExplicitAckReceived(&explicit_packet, false, false);
    EXPECT_EQ(this->peer_.GetNumEvents(), cid);
    EXPECT_TRUE(this->peer_.IsConnectionOutstanding(cid));
    // Test RUE counters.
    FalconConnectionCounters host_counters =
        this->falcon_->get_stats_manager()->GetConnectionCounters(cid);
    EXPECT_EQ(host_counters.rue_enqueue_attempts, 1);
    EXPECT_EQ(host_counters.rue_ack_events, 1);
    EXPECT_EQ(host_counters.rue_event_drops_ack, 0);
  }

  // Prepare to test p1&&p2 between threshold3 and queue size.
  // All events finishes.
  this->env_.RunFor(time_threshold);
  EXPECT_EQ(this->peer_.GetNumEvents(), 0);
  // Insert events to reach queue_size.
  for (cid = 20000 + 1; cid <= 20000 + kEventQueueLimit; cid++) {
    auto metadata = FalconTestingHelpers::InitializeConnectionMetadata(
        this->falcon_.get(), cid);
    FalconTestingHelpers::InitializeConnectionState(this->falcon_.get(),
                                                    std::move(metadata));
    Packet explicit_packet;
    explicit_packet.packet_type = falcon::PacketType::kAck;
    explicit_packet.ack.dest_cid = cid;
    explicit_packet.metadata.flow_label = 1;
    this->peer_.IncrementNumAcked(&explicit_packet, 10);
    this->rue_->ExplicitAckReceived(&explicit_packet, false, false);
    EXPECT_EQ(this->peer_.GetNumEvents(), cid - 20000);
    EXPECT_TRUE(this->peer_.IsConnectionOutstanding(cid));
    // Test RUE counters.
    FalconConnectionCounters host_counters =
        this->falcon_->get_stats_manager()->GetConnectionCounters(cid);
    EXPECT_EQ(host_counters.rue_enqueue_attempts, 1);
    EXPECT_EQ(host_counters.rue_ack_events, 1);
    EXPECT_EQ(host_counters.rue_event_drops_ack, 0);
  }
  // cid=1 with num_ack == 10 should fail.
  {
    cid = 1;
    // cid=1 should have already finished.
    EXPECT_FALSE(this->peer_.IsConnectionOutstanding(cid));
    Packet explicit_packet;
    explicit_packet.packet_type = falcon::PacketType::kAck;
    explicit_packet.ack.dest_cid = cid;
    explicit_packet.metadata.flow_label = 1;
    this->peer_.IncrementNumAcked(&explicit_packet, 10);
    this->rue_->ExplicitAckReceived(&explicit_packet, false, false);
    // Should fail.
    EXPECT_FALSE(this->peer_.IsConnectionOutstanding(cid));
    EXPECT_EQ(this->peer_.GetNumEvents(), kEventQueueLimit);
    // Test RUE counters.
    FalconConnectionCounters host_counters =
        this->falcon_->get_stats_manager()->GetConnectionCounters(cid);
    EXPECT_EQ(host_counters.rue_enqueue_attempts, 2);
    EXPECT_EQ(host_counters.rue_ack_events, 1);
    EXPECT_EQ(host_counters.rue_event_drops_ack, 1);
  }
  // cid=1003 with num_ack == 10 should fail.
  {
    cid = 1003;
    auto metadata = FalconTestingHelpers::InitializeConnectionMetadata(
        this->falcon_.get(), cid);
    FalconTestingHelpers::InitializeConnectionState(this->falcon_.get(),
                                                    std::move(metadata));
    Packet explicit_packet;
    explicit_packet.packet_type = falcon::PacketType::kAck;
    explicit_packet.ack.dest_cid = cid;
    explicit_packet.metadata.flow_label = 1;
    this->peer_.IncrementNumAcked(&explicit_packet, 10);
    this->rue_->ExplicitAckReceived(&explicit_packet, false, false);
    // Should fail.
    EXPECT_FALSE(this->peer_.IsConnectionOutstanding(cid));
    EXPECT_EQ(this->peer_.GetNumEvents(), kEventQueueLimit);
    // Test RUE counters.
    FalconConnectionCounters host_counters =
        this->falcon_->get_stats_manager()->GetConnectionCounters(cid);
    EXPECT_EQ(host_counters.rue_enqueue_attempts, 1);
    EXPECT_EQ(host_counters.rue_ack_events, 0);
    EXPECT_EQ(host_counters.rue_event_drops_ack, 1);
  }
}

}  // namespace

}  // namespace isekai
