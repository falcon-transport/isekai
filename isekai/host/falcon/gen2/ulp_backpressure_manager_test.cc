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

#include "absl/algorithm/container.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/time/time.h"
#include "gmock/gmock.h"
#include "google/protobuf/text_format.h"
#include "gtest/gtest.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/constants.h"
#include "isekai/common/default_config_generator.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/common/packet.h"
#include "isekai/common/simple_environment.h"
#include "isekai/common/testing.h"
#include "isekai/host/falcon/falcon.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/falcon_connection_state.h"
#include "isekai/host/falcon/falcon_counters.h"
#include "isekai/host/falcon/gen2/connection_state.h"
#include "isekai/host/falcon/gen2/falcon_model.h"
#include "isekai/host/falcon/gen2/falcon_types.h"
#include "isekai/host/rnic/connection_manager.h"

namespace isekai {

namespace {

using ::testing::_;
using ::testing::NiceMock;

class MockTrafficShaper : public TrafficShaperInterface {
 public:
  MOCK_METHOD(void, ConnectPacketBuilder,
              (PacketBuilderInterface * packet_builder), (override));
  MOCK_METHOD(void, TransferTxPacket, (std::unique_ptr<Packet> packet),
              (override));
};

class MockRdma : public RdmaFalconInterface {
 public:
  MOCK_METHOD(void, HandleRxTransaction,
              (std::unique_ptr<Packet> packet,
               std::unique_ptr<OpaqueCookie> cookie),
              (override));
  MOCK_METHOD(void, HandleCompletion,
              (QpId qp_id, uint32_t rsn, Packet::Syndrome syndrome,
               uint8_t destination_bifurcation_id),
              (override));
  MOCK_METHOD(void, ReturnFalconCredit,
              (QpId qp_id, const FalconCredit& ulp_credit), (override));
  MOCK_METHOD(void, SetXoff, (bool request_xoff, bool global_xoff), (override));
  MOCK_METHOD(void, BackpressureQP,
              (QpId qp_id, BackpressureType type, BackpressureData data),
              (override));
};

constexpr int kFalconTickTimeNs = 5;

std::unique_ptr<Packet> CreateReadRequest(QpId src_qp, QpId dst_qp,
                                          uint32_t src_cid, uint32_t rsn,
                                          uint32_t op_size) {
  auto packet = std::make_unique<Packet>();
  packet->rdma.opcode = Packet::Rdma::Opcode::kReadRequest;
  packet->rdma.dest_qp_id = dst_qp;
  packet->rdma.rsn = rsn;
  packet->rdma.sgl = {op_size};
  packet->rdma.data_length =
      kReadHeaderSize;  // kCbth + kReth + kSeth + kSteth;
  packet->rdma.request_length =
      kResponseHeaderSize +
      absl::c_accumulate(packet->rdma.sgl, 0U);  // kCbth + kSteth;
  packet->metadata.rdma_src_qp_id = src_qp;
  packet->metadata.sgl_length =
      packet->rdma.sgl.size() * kSglFragmentHeaderSize;
  packet->metadata.scid = src_cid;
  return packet;
}

TEST(UlpBackpressureManagerTest, AlphaCarvingXoffPolicy) {
  SimpleEnvironment env;
  uint32_t source_connection_id = 1;
  QpId source_qp_id = 0;

  FalconConfig config = DefaultConfigGenerator::DefaultFalconConfig(2);
  config.set_falcon_tick_time_ns(kFalconTickTimeNs);
  config.set_resource_reservation_mode(FalconConfig::FIRST_PHASE_RESERVATION);

  // Explicitly setting the avaliable tx packet credit number into 4.
  auto falcon_credits = config.mutable_resource_credits();
  falcon_credits->mutable_tx_packet_credits()->set_ulp_requests(4);

  auto per_connection_backpressure =
      config.mutable_gen2_config_options()
          ->mutable_per_connection_ulp_backpressure();

  per_connection_backpressure->set_enable_backpressure(true);
  auto per_connection_xoff = per_connection_backpressure->mutable_xoff_policy();

  // Config the base alpha.
  auto connection_dynamic_buffer_limit =
      per_connection_xoff->mutable_alpha_carving_configuration();
  connection_dynamic_buffer_limit->set_base_alpha(2.0);
  connection_dynamic_buffer_limit->set_enable_bound(false);

  NiceMock<MockRdma> rdma;
  NiceMock<MockTrafficShaper> shaper;
  Gen2FalconModel falcon(config, &env, /*stats_collector=*/nullptr,
                         ConnectionManager::GetConnectionManager(),
                         "falcon-host", /* number of hosts */ 4);
  falcon.ConnectShaper(&shaper);
  falcon.ConnectRdma(&rdma);

  // Initialize the connection state.
  ConnectionStateManager* const connection_state_manager =
      falcon.get_state_manager();
  std::unique_ptr<Gen2ConnectionMetadata> connection_metadata =
      std::make_unique<Gen2ConnectionMetadata>();
  connection_metadata->scid = source_connection_id;
  EXPECT_OK(connection_state_manager->InitializeConnectionState(
      std::move(connection_metadata)));

  absl::StatusOr<ConnectionState*> connection_state =
      connection_state_manager->PerformDirectLookup(/*scid*/ 1);
  EXPECT_OK(connection_state);

  // Initialize QP ID.
  falcon.SetupNewQp(source_connection_id, source_qp_id, QpType::kRC,
                    OrderingMode::kOrdered);

  // Explicitly set the alpha_request.
  auto gen2_connection_state =
      dynamic_cast<Gen2ConnectionState*>(connection_state.value());
  gen2_connection_state->connection_xoff_metadata.alpha_request = 4;
  gen2_connection_state->connection_xoff_metadata.alpha_response = 0;

  // Explicitly setting connection 1's tx packet credit usage into 1.
  FalconConnectionCounters& counters =
      falcon.get_stats_manager()->GetConnectionCounters(/*scid*/ 1);
  counters.tx_pkt_credits_ulp_requests = 1;

  auto request_1 = CreateReadRequest(
      /*src_qp=*/source_qp_id, /*dst_qp=*/0, /*src_cid=*/source_connection_id,
      /*rsn=*/0, /*op_size=*/32);
  auto request_2 = CreateReadRequest(
      /*src_qp=*/source_qp_id, /*dst_qp=*/0, /*src_cid=*/source_connection_id,
      /*rsn=*/1, /*op_size=*/32);
  auto request_3 = CreateReadRequest(
      /*src_qp=*/source_qp_id, /*dst_qp=*/0, /*src_cid=*/source_connection_id,
      /*rsn=*/2, /*op_size=*/32);

  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(10), [&]() {
    falcon.InitiateTransaction(std::move(request_1));
    falcon.InitiateTransaction(std::move(request_2));
    falcon.InitiateTransaction(std::move(request_3));
  }));

  auto reliability_manager = falcon.get_packet_reliability_manager();

  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(60), [&]() {
    gen2_connection_state->connection_xoff_metadata.alpha_request = 0;
    gen2_connection_state->connection_xoff_metadata.alpha_response = 0;

    // Receive pull data.
    auto rx_packet = std::make_unique<Packet>();
    rx_packet->packet_type = falcon::PacketType::kPullData;
    rx_packet->falcon.dest_cid = source_connection_id;
    rx_packet->falcon.psn = 0;
    rx_packet->falcon.rsn = 0;
    rx_packet->falcon.ack_req = false;
    EXPECT_OK(reliability_manager->ReceivePacket(rx_packet.get()));
  }));

  // Three transactions are enqueued at 10ns. According to the alpha carving
  // algorithm, connection 1's initial dynamic resource limit is:
  // avaliable_tx_resource * alpha_request = 4 * 1/4 = 1
  // Since connection 1's tx packet credit usage is 4 after three transactions
  // enqueued, xoff is asserted. At 60 ns, PullData is received, and
  // alpha_request = 1. Connection 1's dynamic resource limit becomes 4, and the
  // tx packet credit usage is 3, thus xoff is de-asserted.
  EXPECT_CALL(rdma, BackpressureQP(_, _, _))
      .WillOnce([&](const QpId qp_id, const BackpressureType type,
                    const BackpressureData& data) {
        EXPECT_EQ(qp_id, source_qp_id);
        EXPECT_EQ(type, BackpressureType::kXoff);
        EXPECT_EQ(data.xoff.request, true);
        EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(10));
        return absl::OkStatus();
      })
      .WillOnce([&](const QpId qp_id, const BackpressureType type,
                    const BackpressureData& data) {
        EXPECT_EQ(qp_id, source_qp_id);
        EXPECT_EQ(type, BackpressureType::kXoff);
        EXPECT_EQ(data.xoff.request, true);
        EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(10));
        return absl::OkStatus();
      })
      .WillOnce([&](const QpId qp_id, const BackpressureType type,
                    const BackpressureData& data) {
        EXPECT_EQ(qp_id, source_qp_id);
        EXPECT_EQ(type, BackpressureType::kXoff);
        EXPECT_EQ(data.xoff.request, true);
        EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(10));
        return absl::OkStatus();
      })
      .WillOnce([&](const QpId qp_id, const BackpressureType type,
                    const BackpressureData& data) {
        EXPECT_EQ(qp_id, source_qp_id);
        EXPECT_EQ(type, BackpressureType::kXoff);
        EXPECT_EQ(data.xoff.request, false);
        EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(60));
        return absl::OkStatus();
      });

  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(100), [&]() { env.Stop(); }));
  env.Run();
}

}  // namespace
}  // namespace isekai
