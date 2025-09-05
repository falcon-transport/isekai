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

#include "isekai/host/falcon/gen1/rate_update_engine_adapter.h"

#include <cstdint>
#include <memory>
#include <utility>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "isekai/common/common_util.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/default_config_generator.h"
#include "isekai/common/packet.h"
#include "isekai/common/simple_environment.h"
#include "isekai/common/testing.h"
#include "isekai/host/falcon/falcon.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/falcon_testing_helpers.h"
#include "isekai/host/falcon/gen1/falcon_model.h"
#include "isekai/host/falcon/gen1/falcon_types.h"
#include "isekai/host/falcon/gen1/rate_update_engine.h"
#include "isekai/host/falcon/rue/format.h"
#include "isekai/host/rnic/connection_manager.h"

namespace isekai {

// This is a peer class that is defined to allow access to private functions
// and variables in ProtocolRateUpdateAdapter for testing purposes.
class ProtocolRateUpdateAdapterTestPeer {
 public:
  void Set(Gen1RateUpdateEngine* rue) {
    rue_ = rue;
    auto rue_adapter = std::make_unique<
        MockRueAdapter<falcon_rue::Event, falcon_rue::Response>>();
    rue_adapter_ = rue_adapter.get();
    rue_->rue_adapter_ = std::move(rue_adapter);
  }
  MockRueAdapter<falcon_rue::Event, falcon_rue::Response>& mock_rue_adapter() {
    return *rue_adapter_;
  }

 private:
  Gen1RateUpdateEngine* rue_;
  MockRueAdapter<falcon_rue::Event, falcon_rue::Response>* rue_adapter_;
};
namespace {

constexpr uint32_t kCid1 = 123;
constexpr uint32_t kCid2 = 456;
constexpr uint64_t kEventQueueLimit = 100;

// This defines all the objects needed for setup and testing
class ProtocolRateUpdateAdapterTest : public testing::Test {
 protected:
  void SetUp() override {
    config_.mutable_rue()->set_event_queue_size(kEventQueueLimit);
    config_.mutable_rue()->set_event_queue_threshold_1(kEventQueueLimit);
    config_.mutable_rue()->set_event_queue_threshold_2(kEventQueueLimit);
    config_.mutable_rue()->set_event_queue_threshold_3(kEventQueueLimit);
    falcon_ = std::make_unique<FalconModel>(
        config_, &env_, /*stats_collector=*/nullptr,
        ConnectionManager::GetConnectionManager(), "falcon-host",
        /* number of hosts */ 4);
    connection_state_manager_ = falcon_->get_state_manager();
    rue_ = down_cast<Gen1RateUpdateEngine*>(falcon_->get_rate_update_engine());
    ASSERT_NE(rue_, nullptr);
    peer_.Set(rue_);
    EXPECT_CALL(peer_.mock_rue_adapter(), InitializeMetadata).Times(2);
    ASSERT_OK(connection_state_manager_->InitializeConnectionState(
        FalconTestingHelpers::InitializeConnectionMetadata(falcon_.get(),
                                                           kCid1)));
    ASSERT_OK(connection_state_manager_->InitializeConnectionState(
        FalconTestingHelpers::InitializeConnectionMetadata(falcon_.get(),
                                                           kCid2)));
  }

  SimpleEnvironment env_;
  FalconConfig config_ = DefaultConfigGenerator::DefaultFalconConfig(1);
  std::unique_ptr<FalconModel> falcon_;
  ConnectionStateManager* connection_state_manager_;
  Gen1RateUpdateEngine* rue_;
  ProtocolRateUpdateAdapterTestPeer peer_;
};

TEST_F(ProtocolRateUpdateAdapterTest, AckProcessing) {
  // Notifies the RUE of an explicit ACK
  Packet explicit_packet;
  explicit_packet.packet_type = falcon::PacketType::kAck;
  explicit_packet.ack.dest_cid = kCid1;
  explicit_packet.metadata.flow_label = 1;

  EXPECT_CALL(peer_.mock_rue_adapter(), EnqueueAck).Times(1);
  EXPECT_CALL(peer_.mock_rue_adapter(), GetNumEvents)
      .WillRepeatedly(testing::Return(0));
  rue_->ExplicitAckReceived(&explicit_packet, false, false);
  EXPECT_EQ(env_.ScheduledEvents(), 1);

  // Run and validate response
  EXPECT_CALL(peer_.mock_rue_adapter(), ProcessNextEvent).Times(1);

  EXPECT_CALL(peer_.mock_rue_adapter(), DequeueResponse)
      .Times(1)
      .WillOnce(testing::Return(testing::ByMove(
          ResponseMetadata{.randomize_path = false,
                           .has_rto_decreased = false,
                           .rue_key = std::make_unique<RueKey>(kCid1)})));
  EXPECT_CALL(peer_.mock_rue_adapter(), GetNumEvents)
      .WillOnce(testing::Return(1))
      .WillOnce(testing::Return(0));
  env_.Run();
  EXPECT_EQ(env_.ExecutedEvents(), 3);
}

TEST_F(ProtocolRateUpdateAdapterTest, NackProcessing) {
  // Notifies the RUE of a NACK
  Packet nack_packet;
  nack_packet.packet_type = falcon::PacketType::kNack;
  nack_packet.nack.dest_cid = kCid1;
  nack_packet.metadata.flow_label = 1;

  EXPECT_CALL(peer_.mock_rue_adapter(), EnqueueNack).Times(1);
  EXPECT_CALL(peer_.mock_rue_adapter(), GetNumEvents)
      .WillRepeatedly(testing::Return(0));
  rue_->NackReceived(&nack_packet);
  EXPECT_EQ(env_.ScheduledEvents(), 1);

  // Run and validate response
  EXPECT_CALL(peer_.mock_rue_adapter(), ProcessNextEvent).Times(1);
  EXPECT_CALL(peer_.mock_rue_adapter(), DequeueResponse)
      .Times(1)
      .WillOnce(testing::Return(testing::ByMove(
          ResponseMetadata{.randomize_path = false,
                           .has_rto_decreased = false,
                           .rue_key = std::make_unique<RueKey>(kCid1)})));
  EXPECT_CALL(peer_.mock_rue_adapter(), GetNumEvents)
      .WillOnce(testing::Return(1))
      .WillOnce(testing::Return(0));
  env_.Run();
  EXPECT_EQ(env_.ExecutedEvents(), 3);
}

TEST_F(ProtocolRateUpdateAdapterTest, TimeoutProcessing) {
  // Notifies the RUE of a timeout retransmission
  Packet retx_packet;
  retx_packet.packet_type = falcon::PacketType::kPullRequest;
  retx_packet.metadata.scid = kCid1;
  retx_packet.metadata.flow_label = 1;

  EXPECT_CALL(peer_.mock_rue_adapter(), EnqueueTimeoutRetransmit).Times(1);
  EXPECT_CALL(peer_.mock_rue_adapter(), GetNumEvents)
      .WillRepeatedly(testing::Return(0));
  rue_->PacketTimeoutRetransmitted(kCid1, &retx_packet, 2);

  // Run and validate response
  EXPECT_CALL(peer_.mock_rue_adapter(), ProcessNextEvent).Times(1);
  EXPECT_CALL(peer_.mock_rue_adapter(), DequeueResponse)
      .Times(1)
      .WillOnce(testing::Return(testing::ByMove(
          ResponseMetadata{.randomize_path = false,
                           .has_rto_decreased = false,
                           .rue_key = std::make_unique<RueKey>(kCid1)})));
  EXPECT_CALL(peer_.mock_rue_adapter(), GetNumEvents)
      .WillOnce(testing::Return(1))
      .WillOnce(testing::Return(0));
  env_.Run();
  EXPECT_EQ(env_.ExecutedEvents(), 3);
}

}  // namespace
}  // namespace isekai
