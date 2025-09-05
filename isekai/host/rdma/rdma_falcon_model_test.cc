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

#include "isekai/host/rdma/rdma_falcon_model.h"

#include <cstdint>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "isekai/common/common_util.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/constants.h"
#include "isekai/common/default_config_generator.h"
#include "isekai/common/environment.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/common/packet.h"
#include "isekai/common/simple_environment.h"
#include "isekai/common/testing.h"
#include "isekai/host/falcon/falcon_histograms.h"
#include "isekai/host/rdma/rdma_component_interfaces.h"
#include "isekai/host/rnic/memory_interface.h"

namespace isekai {
class FalconLatencyStatsCollector;

class RdmaFalconModelPeer {
 public:
  static FalconQpContext* GetQpContext(RdmaFalconModel& rdma, QpId qp_id) {
    return down_cast<FalconQpContext*>(rdma.qp_manager_.DirectQpLookup(qp_id));
  }
  static void SetWriteRandomRnrProbability(RdmaFalconModel& rdma, double prob) {
    rdma.write_random_rnr_probability_ = prob;
  }
};

namespace {

using std::make_pair;
using ::testing::_;
using ::testing::ElementsAre;
using ::testing::InSequence;
using ::testing::MockFunction;
using ::testing::NiceMock;

class MockFalcon : public FalconInterface {
 public:
  MOCK_METHOD(int, GetVersion, (), (const, override));
  MOCK_METHOD(void, InitiateTransaction, (std::unique_ptr<Packet> packet),
              (override));
  MOCK_METHOD(void, TransferRxPacket, (std::unique_ptr<Packet> packet),
              (override));
  MOCK_METHOD(void, AckTransaction,
              (uint32_t scid, uint32_t rsn, Packet::Syndrome ack_code,
               absl::Duration rnr_timeout,
               std::unique_ptr<OpaqueCookie> cookie),
              (override));
  MOCK_METHOD(uint32_t, SetupNewQp,
              (uint32_t scid, QpId qp_id, QpType type, OrderingMode mode),
              (override));
  MOCK_METHOD(absl::Status, EstablishConnection,
              (uint32_t scid, uint32_t dcid, uint8_t source_bifurcation_id,
               uint8_t destination_bifurcation_id,
               absl::string_view dst_ip_address, OrderingMode ordering_mode,
               const FalconConnectionOptions& connection_options),
              (override));
  MOCK_METHOD(const FalconConfig*, get_config, (), (const, override));
  MOCK_METHOD(Environment*, get_environment, (), (const, override));
  MOCK_METHOD(std::string_view, get_host_id, (), (const, override));
  MOCK_METHOD(StatisticCollectionInterface*, get_stats_collector, (),
              (const, override));
  MOCK_METHOD(FalconHistogramCollector*, get_histogram_collector, (),
              (const, override));
  MOCK_METHOD(void, SetXoffByPacketBuilder, (bool xoff), (override));
  MOCK_METHOD(bool, CanSendPacket, (), (const, override));
  MOCK_METHOD(void, SetXoffByRdma, (uint8_t bifurcation_id, bool xoff),
              (override));
  MOCK_METHOD(void, UpdateRxBytes,
              (std::unique_ptr<Packet> packet, uint32_t pkt_size_bytes),
              (override));
  MOCK_METHOD(void, UpdateTxBytes,
              (std::unique_ptr<Packet> packet, uint32_t pkt_size_bytes),
              (override));
};

class FakeConnectionManager : public ConnectionManagerInterface {
 public:
  absl::StatusOr<std::tuple<QpId, QpId>> CreateConnection(
      absl::string_view host_id1, absl::string_view host_id2,
      uint8_t source_bifurcation_id, uint8_t destination_bifurcation_id,
      QpOptions& options, FalconConnectionOptions& connection_options,
      RdmaConnectedMode rc_mode) override {
    static uint32_t qp_id1 = 1;
    static uint32_t qp_id2 = 100;
    return std::make_tuple(qp_id1++, qp_id2++);
  }
  absl::StatusOr<std::tuple<QpId, QpId>> GetOrCreateBidirectionalConnection(
      absl::string_view host_id1, absl::string_view host_id2,
      uint8_t source_bifurcation_id, uint8_t destination_bifurcation_id,
      QpOptions& options, FalconConnectionOptions& connection_options,
      RdmaConnectedMode rc_mode) override {
    static uint32_t qp_id1 = 1;
    static uint32_t qp_id2 = 100;
    return std::make_tuple(qp_id1++, qp_id2++);
  }
  absl::StatusOr<std::tuple<QpId, QpId>> CreateUnidirectionalConnection(
      absl::string_view host_id1, absl::string_view host_id2,
      uint8_t source_bifurcation_id, uint8_t destination_bifurcation_id,
      QpOptions& options, FalconConnectionOptions& connection_options,
      RdmaConnectedMode rc_mode) override {
    static uint32_t qp_id1 = 1;
    static uint32_t qp_id2 = 100;
    return std::make_tuple(qp_id1++, qp_id2++);
  }

  void RegisterRdma(absl::string_view host_id,
                    RdmaBaseInterface* rdma) override {}
  void RegisterFalcon(absl::string_view host_id,
                      FalconInterface* falcon) override {}
  void PopulateHostInfo(const NetworkConfig& simulation_network) override {}
  absl::StatusOr<std::string> GetIpAddress(absl::string_view host_id) override {
    return "::1";
  }
};

TEST(RdmaFalconModelTest, WorkScheduler) {
  SimpleEnvironment env;

  RdmaConfig config = DefaultConfigGenerator::DefaultRdmaConfig();
  FakeConnectionManager connection_manager;
  RdmaFalconModel rdma(config, &env,
                       /*stats_collector=*/nullptr, &connection_manager);

  NiceMock<MockFalcon> falcon;
  rdma.ConnectFalcon(&falcon);
  isekai::RNicConfig rnic_config =
      isekai::DefaultConfigGenerator::DefaultRNicConfig();
  auto hifs =
      std::make_unique<std::vector<std::unique_ptr<isekai::MemoryInterface>>>();
  for (const auto& host_intf_config : rnic_config.host_interface_config()) {
    auto hif =
        std::make_unique<isekai::MemoryInterface>(host_intf_config, &env);
    hifs->push_back(std::move(hif));
  }
  rdma.ConnectHostInterface(hifs.get());

  QpOptions qp_options;
  qp_options.dst_ip = "::1";

  std::pair<QpId, QpId> qp1 = make_pair(1, 100);
  rdma.CreateRcQp(qp1.first, qp1.second, qp_options,
                  RdmaConnectedMode::kOrderedRc);
  std::pair<QpId, QpId> qp2 = make_pair(2, 101);
  rdma.CreateRcQp(qp2.first, qp2.second, qp_options,
                  RdmaConnectedMode::kOrderedRc);

  CompletionCallback completion_callback = [](Packet::Syndrome syndrome) {};
  // With the quanta-based round-robin, QP 1's ops should be first scheduled,
  // as long as the request size does not exceed the work_scheduler_quanta.
  rdma.PerformOp(/* qp_id = */ qp1.first,
                 /* opcode = */ RdmaOpcode::kWrite,
                 /* sgl = */ {10}, /* is_inline = */ true, completion_callback,
                 kInvalidQpId);
  rdma.PerformOp(/* qp_id = */ qp2.first,
                 /* opcode = */ RdmaOpcode::kWrite,
                 /* sgl = */ {11}, /* is_inline = */ true, completion_callback,
                 kInvalidQpId);
  rdma.PerformOp(/* qp_id = */ qp1.first,
                 /* opcode = */ RdmaOpcode::kWrite,
                 /* sgl = */ {12}, /* is_inline = */ true, completion_callback,
                 kInvalidQpId);
  rdma.PerformOp(/* qp_id = */ qp2.first,
                 /* opcode = */ RdmaOpcode::kWrite,
                 /* sgl = */ {13}, /* is_inline = */ true, completion_callback,
                 kInvalidQpId);
  rdma.PerformOp(/* qp_id = */ qp1.first,
                 /* opcode = */ RdmaOpcode::kWrite,
                 /* sgl = */ {14}, /* is_inline = */ true, completion_callback,
                 kInvalidQpId);
  rdma.PerformOp(/* qp_id = */ qp2.first,
                 /* opcode = */ RdmaOpcode::kWrite,
                 /* sgl = */ {15}, /* is_inline = */ true, completion_callback,
                 kInvalidQpId);

  {
    InSequence seq;

    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.dest_qp_id, qp1.second);
          EXPECT_EQ(p->rdma.inline_payload_length, 10);
          EXPECT_EQ(p->rdma.rsn, 0);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(10));
        })
        .RetiresOnSaturation();

    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.dest_qp_id, qp1.second);
          EXPECT_EQ(p->rdma.inline_payload_length, 12);
          EXPECT_EQ(p->rdma.rsn, 1);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(20));
        })
        .RetiresOnSaturation();

    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.dest_qp_id, qp1.second);
          EXPECT_EQ(p->rdma.inline_payload_length, 14);
          EXPECT_EQ(p->rdma.rsn, 2);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(30));
        })
        .RetiresOnSaturation();

    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.dest_qp_id, qp2.second);
          EXPECT_EQ(p->rdma.inline_payload_length, 11);
          EXPECT_EQ(p->rdma.rsn, 0);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(40));
        })
        .RetiresOnSaturation();

    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.dest_qp_id, qp2.second);
          EXPECT_EQ(p->rdma.inline_payload_length, 13);
          EXPECT_EQ(p->rdma.rsn, 1);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(50));
        })
        .RetiresOnSaturation();

    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.dest_qp_id, qp2.second);
          EXPECT_EQ(p->rdma.inline_payload_length, 15);
          EXPECT_EQ(p->rdma.rsn, 2);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(60));
        })
        .RetiresOnSaturation();
  }

  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(100), [&]() { env.Stop(); }));
  env.Run();
}

TEST(RdmaFalconModelTest, LimitedPerQpOpRate) {
  SimpleEnvironment env;

  RdmaConfig config = DefaultConfigGenerator::DefaultRdmaConfig();
  FakeConnectionManager connection_manager;
  RdmaFalconModel rdma(config, &env,
                       /*stats_collector=*/nullptr, &connection_manager);

  NiceMock<MockFalcon> falcon;
  rdma.ConnectFalcon(&falcon);

  QpOptions qp_options;
  qp_options.dst_ip = "::1";

  std::pair<QpId, QpId> qp1 = make_pair(1, 100);
  rdma.CreateRcQp(qp1.first, qp1.second, qp_options,
                  RdmaConnectedMode::kOrderedRc);

  CompletionCallback completion_callback = [](Packet::Syndrome syndrome) {};
  constexpr int kManyOps = 128;
  constexpr int kExpectedOps = 40;
  for (int i = 0; i < kManyOps; ++i) {
    rdma.PerformOp(/* qp_id = */ qp1.first,
                   /* opcode = */ RdmaOpcode::kWrite,
                   /* sgl = */ {10}, /* is_inline = */ true,
                   completion_callback, kInvalidQpId);
  }

  // In the first one microseconds, only 40 ops are scheduled.
  {
    InSequence seq;
    EXPECT_CALL(falcon, InitiateTransaction(_))
        .Times(kExpectedOps)
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.dest_qp_id, qp1.second);
          EXPECT_EQ(p->rdma.inline_payload_length, 10);
        })
        .RetiresOnSaturation();
  }
  constexpr int kOneMicrosecondInNanoseconds = 1e3 - 1;
  env.RunUntil(absl::Nanoseconds(kOneMicrosecondInNanoseconds));

  // After 3 more microseconds, the rest of ops are scheduled.
  {
    InSequence seq;
    EXPECT_CALL(falcon, InitiateTransaction(_))
        .Times(kManyOps - kExpectedOps)
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.dest_qp_id, qp1.second);
          EXPECT_EQ(p->rdma.inline_payload_length, 10);
        })
        .RetiresOnSaturation();
  }
  env.RunUntil(absl::Nanoseconds(4 * kOneMicrosecondInNanoseconds));
}

TEST(RdmaFalconModelTest, LimitedMultiplePerQpOpRate) {
  SimpleEnvironment env;
  RdmaConfig config = DefaultConfigGenerator::DefaultRdmaConfig();
  FakeConnectionManager connection_manager;
  RdmaFalconModel rdma(config, &env,
                       /*stats_collector=*/nullptr, &connection_manager);

  NiceMock<MockFalcon> falcon;
  rdma.ConnectFalcon(&falcon);

  QpOptions qp_options;
  qp_options.dst_ip = "::1";

  std::pair<QpId, QpId> qp1 = make_pair(1, 101);
  std::pair<QpId, QpId> qp2 = make_pair(2, 102);
  rdma.CreateRcQp(qp1.first, qp1.second, qp_options,
                  RdmaConnectedMode::kOrderedRc);
  rdma.CreateRcQp(qp2.first, qp2.second, qp_options,
                  RdmaConnectedMode::kOrderedRc);

  CompletionCallback completion_callback = [](Packet::Syndrome syndrome) {};

  constexpr int kManyOps = 80;
  for (int i = 0; i < kManyOps; ++i) {
    EXPECT_OK(env.ScheduleEvent(absl::ZeroDuration(), [&]() {
      rdma.PerformOp(/* qp_id = */ qp1.first,
                     /* opcode = */ RdmaOpcode::kWrite,
                     /* sgl = */ {10}, /* is_inline = */ false,
                     completion_callback, kInvalidQpId);
    }));
    EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(500), [&]() {
      rdma.PerformOp(/* qp_id = */ qp2.first,
                     /* opcode = */ RdmaOpcode::kWrite,
                     /* sgl = */ {10}, /* is_inline = */ true,
                     completion_callback, kInvalidQpId);
    }));
  }

  int qp1_ops = 0;
  int qp2_ops = 0;
  constexpr int kExpectedOpsHalfUs = 20;
  constexpr int kExpectedOpsOneUs = 40;
  constexpr int kOpsPerQuanta = 8;
  // Run for 1us. qp1 should have send out 40 ops, qp2 at most 1.
  {
    InSequence seq;
    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillRepeatedly([&](std::unique_ptr<Packet> p) {
          if (p->rdma.dest_qp_id == qp1.second) {
            ++qp1_ops;
          } else {
            ++qp2_ops;
          }
        })
        .RetiresOnSaturation();
  }
  env.RunUntil(absl::Microseconds(1));
  EXPECT_NEAR(qp1_ops, kExpectedOpsOneUs, kOpsPerQuanta);
  EXPECT_NEAR(qp2_ops, kExpectedOpsHalfUs, kOpsPerQuanta);

  // Run for another 1us. qp1 should have send out total 80 ops, qp2 at most 41.
  constexpr int kExpectedOpsTwoUs = 80;
  {
    InSequence seq;
    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillRepeatedly([&](std::unique_ptr<Packet> p) {
          if (p->rdma.dest_qp_id == qp1.second) {
            ++qp1_ops;
          } else {
            ++qp2_ops;
          }
        })
        .RetiresOnSaturation();
  }
  env.RunUntil(absl::Microseconds(2));
  EXPECT_NEAR(qp1_ops, kExpectedOpsTwoUs, kOpsPerQuanta);
  EXPECT_NEAR(qp2_ops, kExpectedOpsOneUs + kExpectedOpsHalfUs, kOpsPerQuanta);

  // Run for another 1us. qp1 should have send out total 80 ops, qp2 at most 80.
  {
    InSequence seq;
    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillRepeatedly([&](std::unique_ptr<Packet> p) {
          if (p->rdma.dest_qp_id == qp1.second) {
            ++qp1_ops;
          } else {
            ++qp2_ops;
          }
        })
        .RetiresOnSaturation();
  }
  env.RunUntil(absl::Microseconds(3));
  EXPECT_EQ(qp1_ops, kExpectedOpsTwoUs);
  EXPECT_EQ(qp2_ops, kExpectedOpsTwoUs);
}

TEST(RdmaFalconModelTest, QuantaRoundRobinRead) {
  SimpleEnvironment env;

  RdmaConfig config = DefaultConfigGenerator::DefaultRdmaConfig();
  config.set_max_qp_oprate_million_per_sec(200);
  config.set_work_scheduler_quanta(256);
  FakeConnectionManager connection_manager;
  RdmaFalconModel rdma(config, &env,
                       /*stats_collector=*/nullptr, &connection_manager);

  NiceMock<MockFalcon> falcon;
  rdma.ConnectFalcon(&falcon);
  isekai::RNicConfig rnic_config =
      isekai::DefaultConfigGenerator::DefaultRNicConfig();
  auto hifs =
      std::make_unique<std::vector<std::unique_ptr<isekai::MemoryInterface>>>();
  for (const auto& host_intf_config : rnic_config.host_interface_config()) {
    auto hif =
        std::make_unique<isekai::MemoryInterface>(host_intf_config, &env);
    hifs->push_back(std::move(hif));
  }
  rdma.ConnectHostInterface(hifs.get());

  QpOptions qp_options;
  qp_options.dst_ip = "::1";

  std::pair<QpId, QpId> qp1 = make_pair(1, 100);
  rdma.CreateRcQp(qp1.first, qp1.second, qp_options,
                  RdmaConnectedMode::kOrderedRc);
  std::pair<QpId, QpId> qp2 = make_pair(2, 101);
  rdma.CreateRcQp(qp2.first, qp2.second, qp_options,
                  RdmaConnectedMode::kOrderedRc);

  CompletionCallback completion_callback = [](Packet::Syndrome syndrome) {};
  // Scheduled with the quanta-based round-robin between QPs.
  rdma.PerformOp(/* qp_id = */ qp1.first,
                 /* opcode = */ RdmaOpcode::kRead,
                 /* sgl = */ {3000},
                 /* is_inline = */ false, completion_callback, kInvalidQpId);
  rdma.PerformOp(/* qp_id = */ qp1.first,
                 /* opcode = */ RdmaOpcode::kRead,
                 /* sgl = */ {3000},
                 /* is_inline = */ false, completion_callback, kInvalidQpId);
  rdma.PerformOp(/* qp_id = */ qp1.first,
                 /* opcode = */ RdmaOpcode::kRead,
                 /* sgl = */ {1000},
                 /* is_inline = */ false, completion_callback, kInvalidQpId);
  rdma.PerformOp(/* qp_id = */ qp2.first,
                 /* opcode = */ RdmaOpcode::kRead,
                 /* sgl = */ {3000},
                 /* is_inline = */ false, completion_callback, kInvalidQpId);
  {
    InSequence seq;

    // Since the quanta is 400, it should be enough to send out just two read
    // requests, although the read request itself is of size 3000.
    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.dest_qp_id, qp1.second);
          EXPECT_EQ(p->rdma.inline_payload_length, 0);
          EXPECT_THAT(p->rdma.sgl, ElementsAre(3000));
          EXPECT_EQ(p->rdma.rsn, 0);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(10));
        })
        .RetiresOnSaturation();
    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.dest_qp_id, qp1.second);
          EXPECT_EQ(p->rdma.inline_payload_length, 0);
          EXPECT_THAT(p->rdma.sgl, ElementsAre(3000));
          EXPECT_EQ(p->rdma.rsn, 1);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(20));
        })
        .RetiresOnSaturation();
    // QP1's quanta is used up, thus it switches to QP2
    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.dest_qp_id, qp2.second);
          EXPECT_EQ(p->rdma.inline_payload_length, 0);
          EXPECT_THAT(p->rdma.sgl, ElementsAre(3000));
          EXPECT_EQ(p->rdma.rsn, 0);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(30));
        })
        .RetiresOnSaturation();
    // QP2 has no more remaining ops, so it switches to QP1.
    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.dest_qp_id, qp1.second);
          EXPECT_EQ(p->rdma.inline_payload_length, 0);
          EXPECT_THAT(p->rdma.sgl, ElementsAre(1000));
          EXPECT_EQ(p->rdma.rsn, 2);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(40));
        })
        .RetiresOnSaturation();
  }

  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(100), [&]() { env.Stop(); }));
  env.Run();
}

TEST(RdmaFalconModelTest, QuantaPacketsRoundRobinRead) {
  const int kNumOps = 10;
  SimpleEnvironment env;

  RdmaConfig config = DefaultConfigGenerator::DefaultRdmaConfig();
  config.set_max_qp_oprate_million_per_sec(200);
  config.set_work_scheduler_quanta(4096);
  FakeConnectionManager connection_manager;
  RdmaFalconModel rdma(config, &env,
                       /*stats_collector=*/nullptr, &connection_manager);

  NiceMock<MockFalcon> falcon;
  rdma.ConnectFalcon(&falcon);
  isekai::RNicConfig rnic_config =
      isekai::DefaultConfigGenerator::DefaultRNicConfig();
  auto hifs =
      std::make_unique<std::vector<std::unique_ptr<isekai::MemoryInterface>>>();
  for (const auto& host_intf_config : rnic_config.host_interface_config()) {
    auto hif =
        std::make_unique<isekai::MemoryInterface>(host_intf_config, &env);
    hifs->push_back(std::move(hif));
  }
  rdma.ConnectHostInterface(hifs.get());

  QpOptions qp_options;
  qp_options.dst_ip = "::1";

  std::pair<QpId, QpId> qp1 = make_pair(1, 100);
  rdma.CreateRcQp(qp1.first, qp1.second, qp_options,
                  RdmaConnectedMode::kOrderedRc);
  std::pair<QpId, QpId> qp2 = make_pair(2, 101);
  rdma.CreateRcQp(qp2.first, qp2.second, qp_options,
                  RdmaConnectedMode::kOrderedRc);

  CompletionCallback completion_callback = [](Packet::Syndrome syndrome) {};
  // Post kNumOps ops on each qp.
  for (int i = 0; i < kNumOps; ++i) {
    rdma.PerformOp(/* qp_id = */ qp1.first,
                   /* opcode = */ RdmaOpcode::kRead,
                   /* sgl = */ {4},
                   /* is_inline = */ false, completion_callback, kInvalidQpId);
    rdma.PerformOp(/* qp_id = */ qp2.first,
                   /* opcode = */ RdmaOpcode::kRead,
                   /* sgl = */ {4},
                   /* is_inline = */ false, completion_callback, kInvalidQpId);
  }
  {
    // Expect 8 (default packet quanta) ops from qp1, followed by 8 ops from qp2
    // and finally the 2 remaining ops from each qp.
    InSequence seq;

    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.dest_qp_id, qp1.second);
          EXPECT_THAT(p->rdma.sgl, ElementsAre(4));
          EXPECT_EQ(p->rdma.rsn, 0);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(10));
        })
        .RetiresOnSaturation();
    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.dest_qp_id, qp1.second);
          EXPECT_THAT(p->rdma.sgl, ElementsAre(4));
          EXPECT_EQ(p->rdma.rsn, 1);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(20));
        })
        .RetiresOnSaturation();
    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.dest_qp_id, qp1.second);
          EXPECT_THAT(p->rdma.sgl, ElementsAre(4));
          EXPECT_EQ(p->rdma.rsn, 2);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(30));
        })
        .RetiresOnSaturation();
    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.dest_qp_id, qp1.second);
          EXPECT_THAT(p->rdma.sgl, ElementsAre(4));
          EXPECT_EQ(p->rdma.rsn, 3);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(40));
        })
        .RetiresOnSaturation();
    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.dest_qp_id, qp1.second);
          EXPECT_THAT(p->rdma.sgl, ElementsAre(4));
          EXPECT_EQ(p->rdma.rsn, 4);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(50));
        })
        .RetiresOnSaturation();
    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.dest_qp_id, qp1.second);
          EXPECT_THAT(p->rdma.sgl, ElementsAre(4));
          EXPECT_EQ(p->rdma.rsn, 5);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(60));
        })
        .RetiresOnSaturation();
    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.dest_qp_id, qp1.second);
          EXPECT_THAT(p->rdma.sgl, ElementsAre(4));
          EXPECT_EQ(p->rdma.rsn, 6);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(70));
        })
        .RetiresOnSaturation();
    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.dest_qp_id, qp1.second);
          EXPECT_THAT(p->rdma.sgl, ElementsAre(4));
          EXPECT_EQ(p->rdma.rsn, 7);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(80));
        })
        .RetiresOnSaturation();

    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.dest_qp_id, qp2.second);
          EXPECT_THAT(p->rdma.sgl, ElementsAre(4));
          EXPECT_EQ(p->rdma.rsn, 0);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(90));
        })
        .RetiresOnSaturation();
    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.dest_qp_id, qp2.second);
          EXPECT_THAT(p->rdma.sgl, ElementsAre(4));
          EXPECT_EQ(p->rdma.rsn, 1);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(100));
        })
        .RetiresOnSaturation();
    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.dest_qp_id, qp2.second);
          EXPECT_THAT(p->rdma.sgl, ElementsAre(4));
          EXPECT_EQ(p->rdma.rsn, 2);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(110));
        })
        .RetiresOnSaturation();
    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.dest_qp_id, qp2.second);
          EXPECT_THAT(p->rdma.sgl, ElementsAre(4));
          EXPECT_EQ(p->rdma.rsn, 3);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(120));
        })
        .RetiresOnSaturation();
    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.dest_qp_id, qp2.second);
          EXPECT_THAT(p->rdma.sgl, ElementsAre(4));
          EXPECT_EQ(p->rdma.rsn, 4);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(130));
        })
        .RetiresOnSaturation();
    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.dest_qp_id, qp2.second);
          EXPECT_THAT(p->rdma.sgl, ElementsAre(4));
          EXPECT_EQ(p->rdma.rsn, 5);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(140));
        })
        .RetiresOnSaturation();
    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.dest_qp_id, qp2.second);
          EXPECT_THAT(p->rdma.sgl, ElementsAre(4));
          EXPECT_EQ(p->rdma.rsn, 6);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(150));
        })
        .RetiresOnSaturation();
    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.dest_qp_id, qp2.second);
          EXPECT_THAT(p->rdma.sgl, ElementsAre(4));
          EXPECT_EQ(p->rdma.rsn, 7);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(160));
        })
        .RetiresOnSaturation();

    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.dest_qp_id, qp1.second);
          EXPECT_THAT(p->rdma.sgl, ElementsAre(4));
          EXPECT_EQ(p->rdma.rsn, 8);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(170));
        })
        .RetiresOnSaturation();
    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.dest_qp_id, qp1.second);
          EXPECT_THAT(p->rdma.sgl, ElementsAre(4));
          EXPECT_EQ(p->rdma.rsn, 9);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(180));
        })
        .RetiresOnSaturation();
    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.dest_qp_id, qp2.second);
          EXPECT_THAT(p->rdma.sgl, ElementsAre(4));
          EXPECT_EQ(p->rdma.rsn, 8);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(190));
        })
        .RetiresOnSaturation();
    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.dest_qp_id, qp2.second);
          EXPECT_THAT(p->rdma.sgl, ElementsAre(4));
          EXPECT_EQ(p->rdma.rsn, 9);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(200));
        })
        .RetiresOnSaturation();
  }

  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(250), [&]() { env.Stop(); }));
  env.Run();
}

TEST(RdmaFalconModelTest, QuantaRoundRobinWrite) {
  SimpleEnvironment env;

  RdmaConfig config = DefaultConfigGenerator::DefaultRdmaConfig();
  config.set_max_qp_oprate_million_per_sec(200);
  FakeConnectionManager connection_manager;
  RdmaFalconModel rdma(config, &env,
                       /*stats_collector=*/nullptr, &connection_manager);

  NiceMock<MockFalcon> falcon;
  rdma.ConnectFalcon(&falcon);
  isekai::RNicConfig rnic_config =
      isekai::DefaultConfigGenerator::DefaultRNicConfig();
  auto hifs =
      std::make_unique<std::vector<std::unique_ptr<isekai::MemoryInterface>>>();
  for (const auto& host_intf_config : rnic_config.host_interface_config()) {
    auto hif =
        std::make_unique<isekai::MemoryInterface>(host_intf_config, &env);
    hifs->push_back(std::move(hif));
  }
  rdma.ConnectHostInterface(hifs.get());

  QpOptions qp_options;
  qp_options.dst_ip = "::1";

  std::pair<QpId, QpId> qp1 = make_pair(1, 100);
  rdma.CreateRcQp(qp1.first, qp1.second, qp_options,
                  RdmaConnectedMode::kOrderedRc);
  std::pair<QpId, QpId> qp2 = make_pair(2, 101);
  rdma.CreateRcQp(qp2.first, qp2.second, qp_options,
                  RdmaConnectedMode::kOrderedRc);

  CompletionCallback completion_callback = [](Packet::Syndrome syndrome) {};
  // Scheduled with the quanta-based round-robin between QPs.
  rdma.PerformOp(/* qp_id = */ qp1.first,
                 /* opcode = */ RdmaOpcode::kWrite,
                 /* sgl = */ {1500},
                 /* is_inline = */ false, completion_callback, kInvalidQpId);
  rdma.PerformOp(/* qp_id = */ qp1.first,
                 /* opcode = */ RdmaOpcode::kWrite,
                 /* sgl = */ {3000},
                 /* is_inline = */ false, completion_callback, kInvalidQpId);
  rdma.PerformOp(/* qp_id = */ qp1.first,
                 /* opcode = */ RdmaOpcode::kWrite,
                 /* sgl = */ {2000},
                 /* is_inline = */ false, completion_callback, kInvalidQpId);
  rdma.PerformOp(/* qp_id = */ qp2.first,
                 /* opcode = */ RdmaOpcode::kWrite,
                 /* sgl = */ {1500},
                 /* is_inline = */ false, completion_callback, kInvalidQpId);
  rdma.PerformOp(/* qp_id = */ qp2.first,
                 /* opcode = */ RdmaOpcode::kWrite,
                 /* sgl = */ {3000},
                 /* is_inline = */ false, completion_callback, kInvalidQpId);
  rdma.PerformOp(/* qp_id = */ qp2.first,
                 /* opcode = */ RdmaOpcode::kWrite,
                 /* sgl = */ {2000},
                 /* is_inline = */ false, completion_callback, kInvalidQpId);
  {
    InSequence seq;

    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.dest_qp_id, qp1.second);
          EXPECT_EQ(p->rdma.inline_payload_length, 0);
          EXPECT_THAT(p->rdma.sgl, ElementsAre(1500));
          EXPECT_EQ(p->rdma.rsn, 0);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(10));
        })
        .RetiresOnSaturation();

    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.dest_qp_id, qp1.second);
          EXPECT_EQ(p->rdma.inline_payload_length, 0);
          EXPECT_THAT(p->rdma.sgl, ElementsAre(3000));
          EXPECT_EQ(p->rdma.rsn, 1);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(20));
        })
        .RetiresOnSaturation();
    // QP1's quanta is used up, thus it switches to QP2
    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.dest_qp_id, qp2.second);
          EXPECT_EQ(p->rdma.inline_payload_length, 0);
          EXPECT_THAT(p->rdma.sgl, ElementsAre(1500));
          EXPECT_EQ(p->rdma.rsn, 0);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(30));
        })
        .RetiresOnSaturation();

    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.dest_qp_id, qp2.second);
          EXPECT_EQ(p->rdma.inline_payload_length, 0);
          EXPECT_THAT(p->rdma.sgl, ElementsAre(3000));
          EXPECT_EQ(p->rdma.rsn, 1);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(40));
        })
        .RetiresOnSaturation();
    // QP2's quanta is used up, thus it switches to QP1
    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.dest_qp_id, qp1.second);
          EXPECT_EQ(p->rdma.inline_payload_length, 0);
          EXPECT_THAT(p->rdma.sgl, ElementsAre(2000));
          EXPECT_EQ(p->rdma.rsn, 2);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(50));
        })
        .RetiresOnSaturation();
    // No packet in QP１, thus it switches to QP２
    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.dest_qp_id, qp2.second);
          EXPECT_EQ(p->rdma.inline_payload_length, 0);
          EXPECT_THAT(p->rdma.sgl, ElementsAre(2000));
          EXPECT_EQ(p->rdma.rsn, 2);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(60));
        })
        .RetiresOnSaturation();
  }

  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(100), [&]() { env.Stop(); }));
  env.Run();
}

TEST(RdmaFalconModelTest, QuantaPacketsRoundRobinWrite) {
  const int kNumOps = 10;
  SimpleEnvironment env;

  RdmaConfig config = DefaultConfigGenerator::DefaultRdmaConfig();
  config.set_max_qp_oprate_million_per_sec(200);
  config.set_work_scheduler_quanta(4096);
  FakeConnectionManager connection_manager;
  RdmaFalconModel rdma(config, &env,
                       /*stats_collector=*/nullptr, &connection_manager);

  NiceMock<MockFalcon> falcon;
  rdma.ConnectFalcon(&falcon);
  isekai::RNicConfig rnic_config =
      isekai::DefaultConfigGenerator::DefaultRNicConfig();
  auto hifs =
      std::make_unique<std::vector<std::unique_ptr<isekai::MemoryInterface>>>();
  for (const auto& host_intf_config : rnic_config.host_interface_config()) {
    auto hif =
        std::make_unique<isekai::MemoryInterface>(host_intf_config, &env);
    hifs->push_back(std::move(hif));
  }
  rdma.ConnectHostInterface(hifs.get());

  QpOptions qp_options;
  qp_options.dst_ip = "::1";

  std::pair<QpId, QpId> qp1 = make_pair(1, 100);
  rdma.CreateRcQp(qp1.first, qp1.second, qp_options,
                  RdmaConnectedMode::kOrderedRc);
  std::pair<QpId, QpId> qp2 = make_pair(2, 101);
  rdma.CreateRcQp(qp2.first, qp2.second, qp_options,
                  RdmaConnectedMode::kOrderedRc);

  CompletionCallback completion_callback = [](Packet::Syndrome syndrome) {};
  // Post kNumOps ops on each qp.
  for (int i = 0; i < kNumOps; ++i) {
    rdma.PerformOp(/* qp_id = */ qp1.first,
                   /* opcode = */ RdmaOpcode::kWrite,
                   /* sgl = */ {4},
                   /* is_inline = */ false, completion_callback, kInvalidQpId);
    rdma.PerformOp(/* qp_id = */ qp2.first,
                   /* opcode = */ RdmaOpcode::kWrite,
                   /* sgl = */ {4},
                   /* is_inline = */ false, completion_callback, kInvalidQpId);
  }
  {
    // Expect 8 (default packet quanta) ops from qp1, followed by 8 ops from qp2
    // and finally the 2 remaining ops from each qp.
    InSequence seq;

    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.dest_qp_id, qp1.second);
          EXPECT_THAT(p->rdma.sgl, ElementsAre(4));
          EXPECT_EQ(p->rdma.rsn, 0);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(10));
        })
        .RetiresOnSaturation();
    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.dest_qp_id, qp1.second);
          EXPECT_THAT(p->rdma.sgl, ElementsAre(4));
          EXPECT_EQ(p->rdma.rsn, 1);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(20));
        })
        .RetiresOnSaturation();
    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.dest_qp_id, qp1.second);
          EXPECT_THAT(p->rdma.sgl, ElementsAre(4));
          EXPECT_EQ(p->rdma.rsn, 2);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(30));
        })
        .RetiresOnSaturation();
    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.dest_qp_id, qp1.second);
          EXPECT_THAT(p->rdma.sgl, ElementsAre(4));
          EXPECT_EQ(p->rdma.rsn, 3);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(40));
        })
        .RetiresOnSaturation();
    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.dest_qp_id, qp1.second);
          EXPECT_THAT(p->rdma.sgl, ElementsAre(4));
          EXPECT_EQ(p->rdma.rsn, 4);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(50));
        })
        .RetiresOnSaturation();
    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.dest_qp_id, qp1.second);
          EXPECT_THAT(p->rdma.sgl, ElementsAre(4));
          EXPECT_EQ(p->rdma.rsn, 5);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(60));
        })
        .RetiresOnSaturation();
    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.dest_qp_id, qp1.second);
          EXPECT_THAT(p->rdma.sgl, ElementsAre(4));
          EXPECT_EQ(p->rdma.rsn, 6);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(70));
        })
        .RetiresOnSaturation();
    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.dest_qp_id, qp1.second);
          EXPECT_THAT(p->rdma.sgl, ElementsAre(4));
          EXPECT_EQ(p->rdma.rsn, 7);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(80));
        })
        .RetiresOnSaturation();

    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.dest_qp_id, qp2.second);
          EXPECT_THAT(p->rdma.sgl, ElementsAre(4));
          EXPECT_EQ(p->rdma.rsn, 0);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(90));
        })
        .RetiresOnSaturation();
    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.dest_qp_id, qp2.second);
          EXPECT_THAT(p->rdma.sgl, ElementsAre(4));
          EXPECT_EQ(p->rdma.rsn, 1);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(100));
        })
        .RetiresOnSaturation();
    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.dest_qp_id, qp2.second);
          EXPECT_THAT(p->rdma.sgl, ElementsAre(4));
          EXPECT_EQ(p->rdma.rsn, 2);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(110));
        })
        .RetiresOnSaturation();
    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.dest_qp_id, qp2.second);
          EXPECT_THAT(p->rdma.sgl, ElementsAre(4));
          EXPECT_EQ(p->rdma.rsn, 3);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(120));
        })
        .RetiresOnSaturation();
    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.dest_qp_id, qp2.second);
          EXPECT_THAT(p->rdma.sgl, ElementsAre(4));
          EXPECT_EQ(p->rdma.rsn, 4);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(130));
        })
        .RetiresOnSaturation();
    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.dest_qp_id, qp2.second);
          EXPECT_THAT(p->rdma.sgl, ElementsAre(4));
          EXPECT_EQ(p->rdma.rsn, 5);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(140));
        })
        .RetiresOnSaturation();
    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.dest_qp_id, qp2.second);
          EXPECT_THAT(p->rdma.sgl, ElementsAre(4));
          EXPECT_EQ(p->rdma.rsn, 6);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(150));
        })
        .RetiresOnSaturation();
    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.dest_qp_id, qp2.second);
          EXPECT_THAT(p->rdma.sgl, ElementsAre(4));
          EXPECT_EQ(p->rdma.rsn, 7);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(160));
        })
        .RetiresOnSaturation();

    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.dest_qp_id, qp1.second);
          EXPECT_THAT(p->rdma.sgl, ElementsAre(4));
          EXPECT_EQ(p->rdma.rsn, 8);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(170));
        })
        .RetiresOnSaturation();
    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.dest_qp_id, qp1.second);
          EXPECT_THAT(p->rdma.sgl, ElementsAre(4));
          EXPECT_EQ(p->rdma.rsn, 9);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(180));
        })
        .RetiresOnSaturation();
    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.dest_qp_id, qp2.second);
          EXPECT_THAT(p->rdma.sgl, ElementsAre(4));
          EXPECT_EQ(p->rdma.rsn, 8);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(190));
        })
        .RetiresOnSaturation();
    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.dest_qp_id, qp2.second);
          EXPECT_THAT(p->rdma.sgl, ElementsAre(4));
          EXPECT_EQ(p->rdma.rsn, 9);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(200));
        })
        .RetiresOnSaturation();
  }

  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(250), [&]() { env.Stop(); }));
  env.Run();
}

TEST(RdmaFalconModelTest, ScatterGatherList) {
  SimpleEnvironment env;

  RdmaConfig config = DefaultConfigGenerator::DefaultRdmaConfig();
  FakeConnectionManager connection_manager;
  RdmaFalconModel rdma(config, &env,
                       /*stats_collector=*/nullptr, &connection_manager);

  NiceMock<MockFalcon> falcon;
  rdma.ConnectFalcon(&falcon);
  isekai::RNicConfig rnic_config =
      isekai::DefaultConfigGenerator::DefaultRNicConfig();
  auto hifs =
      std::make_unique<std::vector<std::unique_ptr<isekai::MemoryInterface>>>();
  for (const auto& host_intf_config : rnic_config.host_interface_config()) {
    auto hif =
        std::make_unique<isekai::MemoryInterface>(host_intf_config, &env);
    hifs->push_back(std::move(hif));
  }
  rdma.ConnectHostInterface(hifs.get());

  QpOptions qp_options;
  qp_options.dst_ip = "::1";

  std::pair<QpId, QpId> qp = make_pair(1, 100);
  rdma.CreateRcQp(qp.first, qp.second, qp_options,
                  RdmaConnectedMode::kOrderedRc);

  CompletionCallback completion_callback = [](Packet::Syndrome syndrome) {};
  std::vector<uint32_t> sgl;

  sgl = std::vector<uint32_t>(1, 10);
  rdma.PerformOp(/* qp_id = */ qp.first,
                 /* opcode = */ RdmaOpcode::kWrite,
                 /* sgl = */ {10}, /* is_inline = */ false, completion_callback,
                 kInvalidQpId);

  sgl = std::vector<uint32_t>(4, 10);
  rdma.PerformOp(/* qp_id = */ qp.first,
                 /* opcode = */ RdmaOpcode::kWrite,
                 /* sgl = */ sgl,
                 /* is_inline = */ false, completion_callback, kInvalidQpId);

  sgl = std::vector<uint32_t>(8, 10);
  rdma.PerformOp(/* qp_id = */ qp.first,
                 /* opcode = */ RdmaOpcode::kWrite,
                 /* sgl = */ sgl,
                 /* is_inline = */ false, completion_callback, kInvalidQpId);

  sgl = std::vector<uint32_t>(16, 10);
  rdma.PerformOp(/* qp_id = */ qp.first,
                 /* opcode = */ RdmaOpcode::kWrite,
                 /* sgl = */ sgl,
                 /* is_inline = */ false, completion_callback, kInvalidQpId);

  sgl = std::vector<uint32_t>(32, 10);
  rdma.PerformOp(/* qp_id = */ qp.first,
                 /* opcode = */ RdmaOpcode::kWrite,
                 /* sgl = */ sgl,
                 /* is_inline = */ false, completion_callback, kInvalidQpId);

  EXPECT_CALL(falcon, InitiateTransaction(_))
      .WillOnce([&](std::unique_ptr<Packet> p) {
        EXPECT_EQ(p->rdma.dest_qp_id, qp.second);
        EXPECT_EQ(p->rdma.inline_payload_length, 0);
        EXPECT_THAT(p->rdma.sgl, ElementsAre(10));
        EXPECT_EQ(p->rdma.rsn, 0);
        EXPECT_EQ(p->metadata.sgl_length, 24);
        EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(10));
      })
      .WillOnce([&](std::unique_ptr<Packet> p) {
        EXPECT_EQ(p->rdma.dest_qp_id, qp.second);
        EXPECT_EQ(p->rdma.inline_payload_length, 0);
        EXPECT_THAT(p->rdma.sgl, ElementsAre(10, 10, 10, 10));
        EXPECT_EQ(p->rdma.rsn, 1);
        EXPECT_EQ(p->metadata.sgl_length, 72);
        EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(20));
      })
      .WillOnce([&](std::unique_ptr<Packet> p) {
        EXPECT_EQ(p->rdma.dest_qp_id, qp.second);
        EXPECT_EQ(p->rdma.inline_payload_length, 0);
        EXPECT_THAT(p->rdma.sgl, ElementsAre(10, 10, 10, 10, 10, 10, 10, 10));
        EXPECT_EQ(p->rdma.rsn, 2);
        EXPECT_EQ(p->metadata.sgl_length, 136);
        EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(30));
      })
      .WillOnce([&](std::unique_ptr<Packet> p) {
        EXPECT_EQ(p->rdma.dest_qp_id, qp.second);
        EXPECT_EQ(p->rdma.inline_payload_length, 0);
        EXPECT_EQ(p->rdma.rsn, 3);
        EXPECT_EQ(p->metadata.sgl_length, 264);
        EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(40));
      })
      .WillOnce([&](std::unique_ptr<Packet> p) {
        EXPECT_EQ(p->rdma.dest_qp_id, qp.second);
        EXPECT_EQ(p->rdma.inline_payload_length, 0);
        EXPECT_EQ(p->rdma.rsn, 4);
        EXPECT_EQ(p->metadata.sgl_length, 520);
        EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(50));
      });

  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(100), [&]() { env.Stop(); }));
  env.Run();
}

TEST(RdmaFalconModelTest, PacketSplit) {
  SimpleEnvironment env;

  RdmaConfig config = DefaultConfigGenerator::DefaultRdmaConfig();
  config.set_max_qp_oprate_million_per_sec(200);
  FakeConnectionManager connection_manager;
  RdmaFalconModel rdma(config, &env, /*stats_collector=*/nullptr,
                       &connection_manager);

  NiceMock<MockFalcon> falcon;
  rdma.ConnectFalcon(&falcon);
  isekai::RNicConfig rnic_config =
      isekai::DefaultConfigGenerator::DefaultRNicConfig();
  auto hifs =
      std::make_unique<std::vector<std::unique_ptr<isekai::MemoryInterface>>>();
  for (const auto& host_intf_config : rnic_config.host_interface_config()) {
    auto hif =
        std::make_unique<isekai::MemoryInterface>(host_intf_config, &env);
    hifs->push_back(std::move(hif));
  }
  rdma.ConnectHostInterface(hifs.get());

  QpOptions qp_options;
  qp_options.dst_ip = "::1";

  std::pair<QpId, QpId> qp = make_pair(1, 100);
  rdma.CreateRcQp(qp.first, qp.second, qp_options,
                  RdmaConnectedMode::kOrderedRc);

  CompletionCallback completion_callback = [](Packet::Syndrome syndrome) {};
  rdma.PerformOp(/* qp_id = */ qp.first,
                 /* opcode = */ RdmaOpcode::kWrite,
                 /* sgl = */ {1000, 1000, 1000, 1000, 1000, 5000},
                 /* is_inline = */ false, completion_callback, kInvalidQpId);

  rdma.PerformOp(/* qp_id = */ qp.first,
                 /* opcode = */ RdmaOpcode::kWrite,
                 /* sgl = */ {2000},
                 /* is_inline = */ false, completion_callback, kInvalidQpId);

  EXPECT_CALL(falcon, InitiateTransaction(_))
      .WillOnce([&](std::unique_ptr<Packet> p) {
        EXPECT_EQ(p->rdma.dest_qp_id, qp.second);
        EXPECT_EQ(p->rdma.inline_payload_length, 0);
        EXPECT_THAT(p->rdma.sgl, ElementsAre(1000, 1000, 1000, 1000, 96));
        EXPECT_EQ(p->rdma.rsn, 0);
        EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(10));
      })
      .WillOnce([&](std::unique_ptr<Packet> p) {
        EXPECT_EQ(p->rdma.dest_qp_id, qp.second);
        EXPECT_EQ(p->rdma.inline_payload_length, 0);
        EXPECT_THAT(p->rdma.sgl, ElementsAre(904, 3192));
        EXPECT_EQ(p->rdma.rsn, 1);
        EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(20));
      })
      .WillOnce([&](std::unique_ptr<Packet> p) {
        EXPECT_EQ(p->rdma.dest_qp_id, qp.second);
        EXPECT_EQ(p->rdma.inline_payload_length, 0);
        EXPECT_THAT(p->rdma.sgl, ElementsAre(1808));
        EXPECT_EQ(p->rdma.rsn, 2);
        EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(30));
      })
      .WillOnce([&](std::unique_ptr<Packet> p) {
        EXPECT_EQ(p->rdma.dest_qp_id, qp.second);
        EXPECT_EQ(p->rdma.inline_payload_length, 0);
        EXPECT_THAT(p->rdma.sgl, ElementsAre(2000));
        EXPECT_EQ(p->rdma.rsn, 3);
        EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(40));
      });

  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(100), [&]() { env.Stop(); }));
  env.Run();
}

TEST(RdmaFalconModelTest, InlinePayload) {
  SimpleEnvironment env;

  RdmaConfig config = DefaultConfigGenerator::DefaultRdmaConfig();
  FakeConnectionManager connection_manager;
  RdmaFalconModel rdma(config, &env,
                       /*stats_collector=*/nullptr, &connection_manager);

  NiceMock<MockFalcon> falcon;
  rdma.ConnectFalcon(&falcon);
  isekai::RNicConfig rnic_config =
      isekai::DefaultConfigGenerator::DefaultRNicConfig();
  auto hifs =
      std::make_unique<std::vector<std::unique_ptr<isekai::MemoryInterface>>>();
  for (const auto& host_intf_config : rnic_config.host_interface_config()) {
    auto hif =
        std::make_unique<isekai::MemoryInterface>(host_intf_config, &env);
    hifs->push_back(std::move(hif));
  }
  rdma.ConnectHostInterface(hifs.get());

  QpOptions qp_options;
  qp_options.dst_ip = "::1";

  std::pair<QpId, QpId> qp = make_pair(1, 100);
  rdma.CreateRcQp(qp.first, qp.second, qp_options,
                  RdmaConnectedMode::kOrderedRc);

  CompletionCallback completion_callback = [](Packet::Syndrome syndrome) {};
  rdma.PerformOp(/* qp_id = */ qp.first,
                 /* opcode = */ RdmaOpcode::kWrite,
                 /* sgl = */ {10, 10}, /* is_inline = */ true,
                 completion_callback, kInvalidQpId);

  EXPECT_CALL(falcon, InitiateTransaction(_))
      .WillOnce([&](std::unique_ptr<Packet> p) {
        EXPECT_EQ(p->rdma.dest_qp_id, qp.second);
        EXPECT_EQ(p->rdma.inline_payload_length, 20);
        EXPECT_THAT(p->rdma.sgl, ElementsAre());
        EXPECT_EQ(p->rdma.rsn, 0);
        EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(10));
      });

  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(100), [&]() { env.Stop(); }));
  env.Run();
}

TEST(RdmaFalconModelTest, Completions) {
  SimpleEnvironment env;

  RdmaConfig config = DefaultConfigGenerator::DefaultRdmaConfig();
  config.set_max_qp_oprate_million_per_sec(200);
  FakeConnectionManager connection_manager;
  RdmaFalconModel rdma(config, &env,
                       /*stats_collector=*/nullptr, &connection_manager);

  NiceMock<MockFalcon> falcon;
  rdma.ConnectFalcon(&falcon);
  isekai::RNicConfig rnic_config =
      isekai::DefaultConfigGenerator::DefaultRNicConfig();
  auto hifs =
      std::make_unique<std::vector<std::unique_ptr<isekai::MemoryInterface>>>();
  for (const auto& host_intf_config : rnic_config.host_interface_config()) {
    auto hif =
        std::make_unique<isekai::MemoryInterface>(host_intf_config, &env);
    hifs->push_back(std::move(hif));
  }
  rdma.ConnectHostInterface(hifs.get());

  QpOptions qp_options;
  qp_options.dst_ip = "::1";

  std::pair<QpId, QpId> qp = make_pair(1, 100);
  rdma.CreateRcQp(qp.first, qp.second, qp_options,
                  RdmaConnectedMode::kOrderedRc);

  MockFunction<void(Packet::Syndrome syndrome)> mock_completion_callback;
  rdma.PerformOp(/* qp_id = */ qp.first, /* opcode = */ RdmaOpcode::kWrite,
                 /* sgl = */ {10}, /* is_inline = */ true,
                 mock_completion_callback.AsStdFunction(), kInvalidQpId);

  MockFunction<void(Packet::Syndrome syndrome)> mock_completion_callback2;
  rdma.PerformOp(/* qp_id = */ qp.first, /* opcode = */ RdmaOpcode::kWrite,
                 /* sgl = */ {10}, /* is_inline = */ true,
                 mock_completion_callback2.AsStdFunction(), kInvalidQpId);

  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(50), [&]() {
    rdma.HandleCompletion(qp.first, /*rsn=*/0, Packet::Syndrome::kAck, 0);
  }));

  EXPECT_CALL(mock_completion_callback, Call(Packet::Syndrome::kAck));

  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(60), [&]() {
    rdma.HandleCompletion(qp.first, /*rsn=*/1, Packet::Syndrome::kAck, 0);
  }));

  EXPECT_CALL(mock_completion_callback2, Call(Packet::Syndrome::kAck));

  // Completion on split packets.
  MockFunction<void(Packet::Syndrome syndrome)> mock_completion_callback3;
  rdma.PerformOp(/* qp_id = */ qp.first, /* opcode = */ RdmaOpcode::kWrite,
                 /* sgl = */ {5000}, /* is_inline = */ false,
                 mock_completion_callback3.AsStdFunction(), kInvalidQpId);

  // Rsn 2 should not trigger the completion.
  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(70), [&]() {
    rdma.HandleCompletion(qp.first, /*rsn=*/2, Packet::Syndrome::kAck, 0);
  }));

  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(80), [&]() {
    rdma.HandleCompletion(qp.first, /*rsn=*/3, Packet::Syndrome::kAck, 0);
  }));

  EXPECT_CALL(mock_completion_callback3, Call(Packet::Syndrome::kAck)).Times(1);

  // Completion on HandleCompletion interface.
  MockFunction<void(Packet::Syndrome syndrome)> mock_completion_callback4;
  rdma.PerformOp(/* qp_id = */ qp.first, /* opcode = */ RdmaOpcode::kWrite,
                 /* sgl = */ {100}, /* is_inline = */ false,
                 mock_completion_callback4.AsStdFunction(), kInvalidQpId);

  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(90), [&]() {
    rdma.HandleCompletion(qp.first, /*rsn=*/4, Packet::Syndrome::kAck, 0);
  }));

  EXPECT_CALL(mock_completion_callback4, Call(Packet::Syndrome::kAck)).Times(1);

  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(1000), [&]() { env.Stop(); }));
  env.Run();
}

TEST(RdmaFalconModelTest, UnorderedCompletions) {
  SimpleEnvironment env;

  RdmaConfig config = DefaultConfigGenerator::DefaultRdmaConfig();
  FakeConnectionManager connection_manager;
  RdmaFalconModel rdma(config, &env,
                       /*stats_collector=*/nullptr, &connection_manager);

  NiceMock<MockFalcon> falcon;
  rdma.ConnectFalcon(&falcon);
  isekai::RNicConfig rnic_config =
      isekai::DefaultConfigGenerator::DefaultRNicConfig();
  auto hifs =
      std::make_unique<std::vector<std::unique_ptr<isekai::MemoryInterface>>>();
  for (const auto& host_intf_config : rnic_config.host_interface_config()) {
    auto hif =
        std::make_unique<isekai::MemoryInterface>(host_intf_config, &env);
    hifs->push_back(std::move(hif));
  }
  rdma.ConnectHostInterface(hifs.get());

  QpOptions qp_options;
  qp_options.dst_ip = "::1";

  std::pair<QpId, QpId> qp = make_pair(1, 100);
  // Unorderd RC
  rdma.CreateRcQp(qp.first, qp.second, qp_options,
                  RdmaConnectedMode::kUnorderedRc);

  std::vector<std::pair<int, absl::Duration>> completion_order;
  MockFunction<void(Packet::Syndrome syndrome)> mock_completion_callback;
  ON_CALL(mock_completion_callback, Call(Packet::Syndrome::kAck))
      .WillByDefault([&](Packet::Syndrome unused) -> void {
        completion_order.push_back(
            std::pair<int, absl::Duration>(1, env.ElapsedTime()));
      });

  rdma.PerformOp(/* qp_id = */ qp.first, /* opcode = */ RdmaOpcode::kWrite,
                 /* sgl = */ {10}, /* is_inline = */ true,
                 mock_completion_callback.AsStdFunction(), kInvalidQpId);

  MockFunction<void(Packet::Syndrome syndrome)> mock_completion_callback2;
  ON_CALL(mock_completion_callback2, Call(Packet::Syndrome::kAck))
      .WillByDefault([&](Packet::Syndrome unused) -> void {
        completion_order.push_back(
            std::pair<int, absl::Duration>(2, env.ElapsedTime()));
      });

  rdma.PerformOp(/* qp_id = */ qp.first, /* opcode = */ RdmaOpcode::kRead,
                 /* sgl = */ {5000}, /* is_inline = */ false,
                 mock_completion_callback2.AsStdFunction(), kInvalidQpId);

  // Completion on split packets.
  MockFunction<void(Packet::Syndrome syndrome)> mock_completion_callback3;
  ON_CALL(mock_completion_callback3, Call(Packet::Syndrome::kAck))
      .WillByDefault([&](Packet::Syndrome unused) -> void {
        completion_order.push_back(
            std::pair<int, absl::Duration>(3, env.ElapsedTime()));
      });

  rdma.PerformOp(/* qp_id = */ qp.first, /* opcode = */ RdmaOpcode::kWrite,
                 /* sgl = */ {10}, /* is_inline = */ true,
                 mock_completion_callback3.AsStdFunction(), kInvalidQpId);

  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(50), [&rdma, &qp]() {
    auto packet = std::make_unique<Packet>();
    packet->rdma.opcode = Packet::Rdma::Opcode::kReadResponseOnly;
    packet->rdma.dest_qp_id = qp.first;
    packet->rdma.rsn = 2;
    packet->rdma.sgl = {1000};
    packet->metadata.destination_bifurcation_id = 0;
    packet->falcon.payload_length = 0;
    auto cookie = std::make_unique<OpaqueCookie>();
    rdma.HandleRxTransaction(std::move(packet), std::move(cookie));
  }));

  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(60), [&]() {
    rdma.HandleCompletion(qp.first, /*rsn=*/3, Packet::Syndrome::kAck, 0);
  }));

  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(70), [&]() {
    rdma.HandleCompletion(qp.first, /*rsn=*/0, Packet::Syndrome::kAck, 0);
  }));

  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(80), [&rdma, &qp]() {
    auto packet = std::make_unique<Packet>();
    packet->rdma.opcode = Packet::Rdma::Opcode::kReadResponseOnly;
    packet->rdma.dest_qp_id = qp.first;
    packet->rdma.rsn = 1;
    packet->rdma.sgl = {4000};
    packet->metadata.destination_bifurcation_id = 0;
    packet->falcon.payload_length = 0;
    auto cookie = std::make_unique<OpaqueCookie>();
    rdma.HandleRxTransaction(std::move(packet), std::move(cookie));
  }));

  env.Run();
  EXPECT_EQ(completion_order.size(), 3);
  EXPECT_EQ(completion_order[0].first, 3);
  EXPECT_EQ(completion_order[0].second, absl::Nanoseconds(60));
  EXPECT_EQ(completion_order[1].first, 1);
  EXPECT_EQ(completion_order[1].second, absl::Nanoseconds(70));
  EXPECT_EQ(completion_order[2].first, 2);
  EXPECT_EQ(completion_order[2].second, absl::Nanoseconds(80));
}

TEST(RdmaFalconModelTest, TargetRecv) {
  SimpleEnvironment env;

  RdmaConfig config = DefaultConfigGenerator::DefaultRdmaConfig();
  FakeConnectionManager connection_manager;
  RdmaFalconModel rdma(config, &env, /*stats_collector=*/nullptr,
                       &connection_manager);

  NiceMock<MockFalcon> falcon;
  rdma.ConnectFalcon(&falcon);
  isekai::RNicConfig rnic_config =
      isekai::DefaultConfigGenerator::DefaultRNicConfig();
  auto hifs =
      std::make_unique<std::vector<std::unique_ptr<isekai::MemoryInterface>>>();
  for (const auto& host_intf_config : rnic_config.host_interface_config()) {
    auto hif =
        std::make_unique<isekai::MemoryInterface>(host_intf_config, &env);
    hifs->push_back(std::move(hif));
  }
  rdma.ConnectHostInterface(hifs.get());

  QpOptions qp_options;
  qp_options.dst_ip = "::1";

  std::pair<QpId, QpId> qp = make_pair(1, 100);
  rdma.CreateRcQp(qp.first, qp.second, qp_options,
                  RdmaConnectedMode::kOrderedRc);

  MockFunction<void(Packet::Syndrome syndrome)> mock_completion_callback;

  rdma.PerformOp(/* qp_id = */ qp.first, /* opcode = */ RdmaOpcode::kRecv,
                 /* sgl = */ {10}, /* is_inline = */ false,
                 mock_completion_callback.AsStdFunction(), kInvalidQpId);
  MockFunction<void(Packet::Syndrome sydrome)> mock_completion_callback2;

  rdma.PerformOp(/* qp_id = */ qp.first, /* opcode = */ RdmaOpcode::kRecv,
                 /* sgl = */ {5000}, /* is_inline = */ false,
                 mock_completion_callback2.AsStdFunction(), kInvalidQpId);

  OpaqueCookie cookie;
  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(50), [&rdma, &qp]() {
    auto packet = std::make_unique<Packet>();
    packet->rdma.opcode = Packet::Rdma::Opcode::kSendOnly;
    packet->rdma.dest_qp_id = qp.first;
    packet->rdma.rsn = 0;
    packet->rdma.inline_payload_length = 10;
    rdma.HandleRxTransaction(std::move(packet),
                             std::make_unique<OpaqueCookie>());
  }));

  EXPECT_CALL(falcon, AckTransaction(qp_options.scid, 0, Packet::Syndrome::kAck,
                                     absl::ZeroDuration(), _));
  EXPECT_CALL(mock_completion_callback, Call(Packet::Syndrome::kAck));

  // Split recv entry - unsuccessful
  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(60), [&rdma, &qp]() {
    auto packet = std::make_unique<Packet>();
    packet->rdma.opcode = Packet::Rdma::Opcode::kSendOnly;
    packet->rdma.dest_qp_id = qp.first;
    packet->rdma.rsn = 1;
    packet->rdma.sgl = {4000};
    rdma.HandleRxTransaction(std::move(packet),
                             std::make_unique<OpaqueCookie>());
  }));

  EXPECT_CALL(falcon,
              AckTransaction(qp_options.scid, 1, Packet::Syndrome::kRnrNak,
                             absl::ZeroDuration(), _));

  // Split recv entry - successful
  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(70), [&rdma, &qp]() {
    auto packet = std::make_unique<Packet>();
    packet->rdma.opcode = Packet::Rdma::Opcode::kSendOnly;
    packet->rdma.dest_qp_id = qp.first;
    packet->rdma.rsn = 1;
    packet->rdma.sgl = {4096};
    rdma.HandleRxTransaction(std::move(packet),
                             std::make_unique<OpaqueCookie>());
  }));
  EXPECT_CALL(falcon, AckTransaction(qp_options.scid, 1, Packet::Syndrome::kAck,
                                     absl::ZeroDuration(), _));

  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(80), [&rdma, &qp]() {
    auto packet = std::make_unique<Packet>();
    packet->rdma.opcode = Packet::Rdma::Opcode::kSendOnly;
    packet->rdma.dest_qp_id = qp.first;
    packet->rdma.rsn = 2;
    packet->rdma.sgl = {904};
    rdma.HandleRxTransaction(std::move(packet),
                             std::make_unique<OpaqueCookie>());
  }));

  EXPECT_CALL(falcon, AckTransaction(qp_options.scid, 2, Packet::Syndrome::kAck,
                                     absl::ZeroDuration(), _));
  EXPECT_CALL(mock_completion_callback2, Call(Packet::Syndrome::kAck));

  // No recv entry
  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(90), [&rdma, &qp]() {
    auto packet = std::make_unique<Packet>();
    packet->rdma.opcode = Packet::Rdma::Opcode::kSendOnly;
    packet->rdma.dest_qp_id = qp.first;
    packet->rdma.rsn = 3;
    rdma.HandleRxTransaction(std::move(packet),
                             std::make_unique<OpaqueCookie>());
  }));

  EXPECT_CALL(falcon,
              AckTransaction(qp_options.scid, 3, Packet::Syndrome::kRnrNak,
                             absl::ZeroDuration(), _));

  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(101), [&]() { env.Stop(); }));
  env.Run();
}

TEST(RdmaFalconModelTest, TargetRead) {
  SimpleEnvironment env;

  RdmaConfig config = DefaultConfigGenerator::DefaultRdmaConfig();
  FakeConnectionManager connection_manager;
  RdmaFalconModel rdma(config, &env,
                       /*stats_collector=*/nullptr, &connection_manager);

  NiceMock<MockFalcon> falcon;
  rdma.ConnectFalcon(&falcon);
  isekai::RNicConfig rnic_config =
      isekai::DefaultConfigGenerator::DefaultRNicConfig();
  auto hifs =
      std::make_unique<std::vector<std::unique_ptr<isekai::MemoryInterface>>>();
  for (const auto& host_intf_config : rnic_config.host_interface_config()) {
    auto hif =
        std::make_unique<isekai::MemoryInterface>(host_intf_config, &env);
    hifs->push_back(std::move(hif));
  }
  rdma.ConnectHostInterface(hifs.get());

  QpOptions qp_options;
  qp_options.dst_ip = "::1";

  std::pair<QpId, QpId> qp = make_pair(1, 100);
  rdma.CreateRcQp(qp.first, qp.second, qp_options,
                  RdmaConnectedMode::kOrderedRc);

  auto packet = std::make_unique<Packet>();
  packet->rdma.opcode = Packet::Rdma::Opcode::kReadRequest;
  packet->rdma.sgl = {20, 50, 70};
  packet->rdma.dest_qp_id = qp.first;
  packet->rdma.rsn = 0;
  auto cookie = std::make_unique<OpaqueCookie>();
  rdma.HandleRxTransaction(std::move(packet), std::move(cookie));

  EXPECT_CALL(falcon, InitiateTransaction(_))
      .WillOnce([&](std::unique_ptr<Packet> p) {
        EXPECT_EQ(p->metadata.scid, qp_options.scid);
        EXPECT_EQ(p->rdma.opcode, Packet::Rdma::Opcode::kReadResponseOnly);
        EXPECT_THAT(p->rdma.sgl, ElementsAre(20, 50, 70));
        EXPECT_EQ(p->rdma.dest_qp_id, qp.second);
        EXPECT_EQ(p->rdma.rsn, 0);
        EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(10));
      });

  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(100), [&]() { env.Stop(); }));
  env.Run();
}

TEST(RdmaFalconModelTest, TargetWrite) {
  SimpleEnvironment env;

  RdmaConfig config = DefaultConfigGenerator::DefaultRdmaConfig();
  FakeConnectionManager connection_manager;
  RdmaFalconModel rdma(config, &env,
                       /*stats_collector=*/nullptr, &connection_manager);

  NiceMock<MockFalcon> falcon;
  rdma.ConnectFalcon(&falcon);
  isekai::RNicConfig rnic_config =
      isekai::DefaultConfigGenerator::DefaultRNicConfig();
  auto hifs =
      std::make_unique<std::vector<std::unique_ptr<isekai::MemoryInterface>>>();
  for (const auto& host_intf_config : rnic_config.host_interface_config()) {
    auto hif =
        std::make_unique<isekai::MemoryInterface>(host_intf_config, &env);
    hifs->push_back(std::move(hif));
  }
  rdma.ConnectHostInterface(hifs.get());

  QpOptions qp_options;
  qp_options.dst_ip = "::1";

  std::pair<QpId, QpId> qp = make_pair(1, 100);
  rdma.CreateRcQp(qp.first, qp.second, qp_options,
                  RdmaConnectedMode::kOrderedRc);

  auto packet = std::make_unique<Packet>();
  packet->rdma.opcode = Packet::Rdma::Opcode::kWriteOnly;
  packet->rdma.dest_qp_id = qp.first;
  packet->rdma.rsn = 0;
  OpaqueCookie cookie;
  rdma.HandleRxTransaction(std::move(packet), std::make_unique<OpaqueCookie>());
  EXPECT_CALL(falcon, AckTransaction(qp_options.scid, 0, Packet::Syndrome::kAck,
                                     absl::ZeroDuration(), _));

  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(100), [&]() { env.Stop(); }));
  env.Run();
}

TEST(RdmaFalconModelTest, TargetRnrOrdered) {
  SimpleEnvironment env;

  RdmaConfig config = DefaultConfigGenerator::DefaultRdmaConfig();
  FakeConnectionManager connection_manager;
  RdmaFalconModel rdma(config, &env,
                       /*stats_collector=*/nullptr, &connection_manager);

  NiceMock<MockFalcon> falcon;
  rdma.ConnectFalcon(&falcon);
  isekai::RNicConfig rnic_config =
      isekai::DefaultConfigGenerator::DefaultRNicConfig();
  auto hifs =
      std::make_unique<std::vector<std::unique_ptr<isekai::MemoryInterface>>>();
  for (const auto& host_intf_config : rnic_config.host_interface_config()) {
    auto hif =
        std::make_unique<isekai::MemoryInterface>(host_intf_config, &env);
    hifs->push_back(std::move(hif));
  }
  rdma.ConnectHostInterface(hifs.get());

  QpOptions qp_options;
  qp_options.dst_ip = "::1";

  std::pair<QpId, QpId> qp = make_pair(1, 100);
  rdma.CreateRcQp(qp.first, qp.second, qp_options,
                  RdmaConnectedMode::kOrderedRc);

  // Set the the Qp in RNR state.
  auto qp_context = RdmaFalconModelPeer::GetQpContext(rdma, 1);
  qp_context->next_rx_rsn = 0;

  OpaqueCookie cookie;
  // Force RNR-NACK RSN 0.
  RdmaFalconModelPeer::SetWriteRandomRnrProbability(rdma, 1);
  {
    auto packet = std::make_unique<Packet>();
    packet->rdma.opcode = Packet::Rdma::Opcode::kWriteOnly;
    packet->rdma.dest_qp_id = qp.first;
    packet->rdma.rsn = 0;
    rdma.HandleRxTransaction(std::move(packet),
                             std::make_unique<OpaqueCookie>());
  }
  EXPECT_CALL(falcon,
              AckTransaction(qp_options.scid, 0, Packet::Syndrome::kRnrNak,
                             absl::ZeroDuration(), _));
  env.RunFor(absl::Nanoseconds(20));

  // No forced RNR NACKs any more.
  RdmaFalconModelPeer::SetWriteRandomRnrProbability(rdma, 0);

  // Non-HoL write will be RNR-NACKed.
  {
    auto packet = std::make_unique<Packet>();
    packet->rdma.opcode = Packet::Rdma::Opcode::kWriteOnly;
    packet->rdma.dest_qp_id = qp.first;
    packet->rdma.rsn = 1;
    rdma.HandleRxTransaction(std::move(packet),
                             std::make_unique<OpaqueCookie>());
  }
  EXPECT_CALL(falcon,
              AckTransaction(qp_options.scid, 1, Packet::Syndrome::kRnrNak,
                             absl::ZeroDuration(), _));

  // Non-HoL read will also be RNR-NACKed.
  {
    auto packet = std::make_unique<Packet>();
    packet->rdma.opcode = Packet::Rdma::Opcode::kReadRequest;
    packet->rdma.dest_qp_id = qp.first;
    packet->rdma.rsn = 2;
    rdma.HandleRxTransaction(std::move(packet),
                             std::make_unique<OpaqueCookie>());
  }
  EXPECT_CALL(falcon,
              AckTransaction(qp_options.scid, 2, Packet::Syndrome::kRnrNak,
                             absl::ZeroDuration(), _));

  // HoL write will be accepted. This should bring the Qp out of RNR state.
  {
    auto packet = std::make_unique<Packet>();
    packet->rdma.opcode = Packet::Rdma::Opcode::kWriteOnly;
    packet->rdma.dest_qp_id = qp.first;
    packet->rdma.rsn = 0;
    rdma.HandleRxTransaction(std::move(packet),
                             std::make_unique<OpaqueCookie>());
  }
  EXPECT_CALL(falcon, AckTransaction(qp_options.scid, 0, Packet::Syndrome::kAck,
                                     absl::ZeroDuration(), _));

  // Next RSN will be accepted too.
  {
    auto packet = std::make_unique<Packet>();
    packet->rdma.opcode = Packet::Rdma::Opcode::kWriteOnly;
    packet->rdma.dest_qp_id = qp.first;
    packet->rdma.rsn = 1;
    rdma.HandleRxTransaction(std::move(packet),
                             std::make_unique<OpaqueCookie>());
  }
  EXPECT_CALL(falcon, AckTransaction(qp_options.scid, 1, Packet::Syndrome::kAck,
                                     absl::ZeroDuration(), _));
  env.RunFor(absl::Nanoseconds(200));
}

TEST(RdmaFalconModelTest, TargetRnrUnordered) {
  SimpleEnvironment env;

  RdmaConfig config = DefaultConfigGenerator::DefaultRdmaConfig();
  FakeConnectionManager connection_manager;
  RdmaFalconModel rdma(config, &env,
                       /*stats_collector=*/nullptr, &connection_manager);

  NiceMock<MockFalcon> falcon;
  rdma.ConnectFalcon(&falcon);
  isekai::RNicConfig rnic_config =
      isekai::DefaultConfigGenerator::DefaultRNicConfig();
  auto hifs =
      std::make_unique<std::vector<std::unique_ptr<isekai::MemoryInterface>>>();
  for (const auto& host_intf_config : rnic_config.host_interface_config()) {
    auto hif =
        std::make_unique<isekai::MemoryInterface>(host_intf_config, &env);
    hifs->push_back(std::move(hif));
  }
  rdma.ConnectHostInterface(hifs.get());

  QpOptions qp_options;
  qp_options.dst_ip = "::1";

  std::pair<QpId, QpId> qp = make_pair(1, 100);
  rdma.CreateRcQp(qp.first, qp.second, qp_options,
                  RdmaConnectedMode::kUnorderedRc);

  // RNR-NACK the next RSN.
  RdmaFalconModelPeer::SetWriteRandomRnrProbability(rdma, 1);
  OpaqueCookie cookie;
  // RSN 0 will be RNR-NACKed.
  {
    auto packet = std::make_unique<Packet>();
    packet->rdma.opcode = Packet::Rdma::Opcode::kWriteOnly;
    packet->rdma.dest_qp_id = qp.first;
    packet->rdma.rsn = 0;
    rdma.HandleRxTransaction(std::move(packet),
                             std::make_unique<OpaqueCookie>());
  }
  EXPECT_CALL(falcon,
              AckTransaction(qp_options.scid, 0, Packet::Syndrome::kRnrNak,
                             absl::ZeroDuration(), _));
  env.RunFor(absl::Nanoseconds(20));

  // No forced RNR NACKs any more.
  RdmaFalconModelPeer::SetWriteRandomRnrProbability(rdma, 0);

  // RSN 1 write will be ACKed.
  {
    auto packet = std::make_unique<Packet>();
    packet->rdma.opcode = Packet::Rdma::Opcode::kWriteOnly;
    packet->rdma.dest_qp_id = qp.first;
    packet->rdma.rsn = 1;
    rdma.HandleRxTransaction(std::move(packet),
                             std::make_unique<OpaqueCookie>());
  }
  EXPECT_CALL(falcon, AckTransaction(qp_options.scid, 1, Packet::Syndrome::kAck,
                                     absl::ZeroDuration(), _));

  // RSN 2 read req will be ACKed.
  {
    auto packet = std::make_unique<Packet>();
    packet->rdma.opcode = Packet::Rdma::Opcode::kReadRequest;
    packet->rdma.dest_qp_id = qp.first;
    packet->rdma.rsn = 2;
    rdma.HandleRxTransaction(std::move(packet),
                             std::make_unique<OpaqueCookie>());
  }
  EXPECT_CALL(falcon, AckTransaction(qp_options.scid, 2, Packet::Syndrome::kAck,
                                     absl::ZeroDuration(), _));

  // RSN 0 write will be ACKed.
  {
    auto packet = std::make_unique<Packet>();
    packet->rdma.opcode = Packet::Rdma::Opcode::kWriteOnly;
    packet->rdma.dest_qp_id = qp.first;
    packet->rdma.rsn = 0;
    rdma.HandleRxTransaction(std::move(packet),
                             std::make_unique<OpaqueCookie>());
  }
  EXPECT_CALL(falcon, AckTransaction(qp_options.scid, 0, Packet::Syndrome::kAck,
                                     absl::ZeroDuration(), _));

  env.RunFor(absl::Nanoseconds(200));
}

TEST(RdmaFalconModelTest, RequestTxPacketCredit) {
  SimpleEnvironment env;

  RdmaConfig config = DefaultConfigGenerator::DefaultRdmaConfig();
  config.mutable_per_qp_credits()->set_tx_packet_request(1);
  FakeConnectionManager connection_manager;
  RdmaFalconModel rdma(config, &env,
                       /*stats_collector=*/nullptr, &connection_manager);

  NiceMock<MockFalcon> falcon;
  rdma.ConnectFalcon(&falcon);
  isekai::RNicConfig rnic_config =
      isekai::DefaultConfigGenerator::DefaultRNicConfig();
  auto hifs =
      std::make_unique<std::vector<std::unique_ptr<isekai::MemoryInterface>>>();
  for (const auto& host_intf_config : rnic_config.host_interface_config()) {
    auto hif =
        std::make_unique<isekai::MemoryInterface>(host_intf_config, &env);
    hifs->push_back(std::move(hif));
  }
  rdma.ConnectHostInterface(hifs.get());

  QpOptions qp_options;
  qp_options.dst_ip = "::1";

  std::pair<QpId, QpId> qp = make_pair(1, 100);
  rdma.CreateRcQp(qp.first, qp.second, qp_options,
                  RdmaConnectedMode::kOrderedRc);

  CompletionCallback completion_callback = [](Packet::Syndrome syndrome) {};
  // This op should be sent soon.
  rdma.PerformOp(/* qp_id = */ qp.first,
                 /* opcode = */ RdmaOpcode::kWrite,
                 /* sgl = */ {10}, /* is_inline = */ true, completion_callback,
                 kInvalidQpId);
  // This op needs to wait for credit return.
  rdma.PerformOp(/* qp_id = */ qp.first,
                 /* opcode = */ RdmaOpcode::kWrite,
                 /* sgl = */ {11}, /* is_inline = */ true, completion_callback,
                 kInvalidQpId);

  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(100), [&]() {
    rdma.IncreaseFalconCreditLimitForTesting(
        /* qp_id = */ qp.first, FalconCredit{.request_tx_packet = 1});
  }));

  {
    InSequence seq;

    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.rsn, 0);
          EXPECT_LT(env.ElapsedTime(), absl::Nanoseconds(100));
        })
        .RetiresOnSaturation();

    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.rsn, 1);
          EXPECT_GE(env.ElapsedTime(), absl::Nanoseconds(100));
        })
        .RetiresOnSaturation();
  }

  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(200), [&]() { env.Stop(); }));
  env.Run();
}

TEST(RdmaFalconModelTest, RequestTxBufferCredit) {
  SimpleEnvironment env;

  RdmaConfig config = DefaultConfigGenerator::DefaultRdmaConfig();
  config.mutable_per_qp_credits()->set_tx_buffer_request(5);
  FakeConnectionManager connection_manager;
  RdmaFalconModel rdma(config, &env,
                       /*stats_collector=*/nullptr, &connection_manager);

  NiceMock<MockFalcon> falcon;
  rdma.ConnectFalcon(&falcon);
  isekai::RNicConfig rnic_config =
      isekai::DefaultConfigGenerator::DefaultRNicConfig();
  auto hifs =
      std::make_unique<std::vector<std::unique_ptr<isekai::MemoryInterface>>>();
  for (const auto& host_intf_config : rnic_config.host_interface_config()) {
    auto hif =
        std::make_unique<isekai::MemoryInterface>(host_intf_config, &env);
    hifs->push_back(std::move(hif));
  }
  rdma.ConnectHostInterface(hifs.get());

  QpOptions qp_options;
  qp_options.dst_ip = "::1";

  std::pair<QpId, QpId> qp = make_pair(1, 100);
  rdma.CreateRcQp(qp.first, qp.second, qp_options,
                  RdmaConnectedMode::kOrderedRc);

  CompletionCallback completion_callback = [](Packet::Syndrome syndrome) {};
  // This op should be sent soon.
  rdma.PerformOp(/* qp_id = */ qp.first,
                 /* opcode = */ RdmaOpcode::kWrite,
                 /* sgl = */ {224}, /* is_inline = */ true, completion_callback,
                 kInvalidQpId);
  // This op needs to wait for credit return.
  rdma.PerformOp(/* qp_id = */ qp.first,
                 /* opcode = */ RdmaOpcode::kWrite,
                 /* sgl = */ {224}, /* is_inline = */ true, completion_callback,
                 kInvalidQpId);

  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(100), [&]() {
    rdma.IncreaseFalconCreditLimitForTesting(
        /* qp_id = */ qp.first, FalconCredit{.request_tx_buffer = 5});
  }));

  {
    InSequence seq;

    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.rsn, 0);
          EXPECT_LT(env.ElapsedTime(), absl::Nanoseconds(100));
        })
        .RetiresOnSaturation();

    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.rsn, 1);
          EXPECT_GE(env.ElapsedTime(), absl::Nanoseconds(100));
        })
        .RetiresOnSaturation();
  }

  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(200), [&]() { env.Stop(); }));
  env.Run();
}

TEST(RdmaFalconModelTest, RequestRxPacketCredit) {
  SimpleEnvironment env;

  RdmaConfig config = DefaultConfigGenerator::DefaultRdmaConfig();
  config.mutable_per_qp_credits()->set_rx_packet_request(1);
  FakeConnectionManager connection_manager;
  RdmaFalconModel rdma(config, &env,
                       /*stats_collector=*/nullptr, &connection_manager);

  NiceMock<MockFalcon> falcon;
  rdma.ConnectFalcon(&falcon);
  isekai::RNicConfig rnic_config =
      isekai::DefaultConfigGenerator::DefaultRNicConfig();
  auto hifs =
      std::make_unique<std::vector<std::unique_ptr<isekai::MemoryInterface>>>();
  for (const auto& host_intf_config : rnic_config.host_interface_config()) {
    auto hif =
        std::make_unique<isekai::MemoryInterface>(host_intf_config, &env);
    hifs->push_back(std::move(hif));
  }
  rdma.ConnectHostInterface(hifs.get());

  QpOptions qp_options;
  qp_options.dst_ip = "::1";

  std::pair<QpId, QpId> qp = make_pair(1, 100);
  rdma.CreateRcQp(qp.first, qp.second, qp_options,
                  RdmaConnectedMode::kOrderedRc);

  CompletionCallback completion_callback = [](Packet::Syndrome syndrome) {};
  // This op should be sent soon.
  rdma.PerformOp(/* qp_id = */ qp.first,
                 /* opcode = */ RdmaOpcode::kRead,
                 /* sgl = */ {200, 500, 700}, /* is_inline */ false,
                 completion_callback, kInvalidQpId);
  // This op needs to wait for credit return.
  rdma.PerformOp(/* qp_id = */ qp.first,
                 /* opcode = */ RdmaOpcode::kRead,
                 /* sgl = */ {200, 500, 700}, /* is_inline */ false,
                 completion_callback, kInvalidQpId);

  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(100), [&]() {
    rdma.IncreaseFalconCreditLimitForTesting(
        /* qp_id = */ qp.first, FalconCredit{.request_rx_packet = 1});
  }));

  {
    InSequence seq;

    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.rsn, 0);
          EXPECT_LT(env.ElapsedTime(), absl::Nanoseconds(100));
        })
        .RetiresOnSaturation();

    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.rsn, 1);
          EXPECT_GE(env.ElapsedTime(), absl::Nanoseconds(100));
        })
        .RetiresOnSaturation();
  }

  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(200), [&]() { env.Stop(); }));
  env.Run();
}

TEST(RdmaFalconModelTest, RequestRxBufferCredit) {
  SimpleEnvironment env;

  RdmaConfig config = DefaultConfigGenerator::DefaultRdmaConfig();
  config.mutable_per_qp_credits()->set_rx_buffer_request(15);
  FakeConnectionManager connection_manager;
  RdmaFalconModel rdma(config, &env,
                       /*stats_collector=*/nullptr, &connection_manager);

  NiceMock<MockFalcon> falcon;
  rdma.ConnectFalcon(&falcon);
  isekai::RNicConfig rnic_config =
      isekai::DefaultConfigGenerator::DefaultRNicConfig();
  auto hifs =
      std::make_unique<std::vector<std::unique_ptr<isekai::MemoryInterface>>>();
  for (const auto& host_intf_config : rnic_config.host_interface_config()) {
    auto hif =
        std::make_unique<isekai::MemoryInterface>(host_intf_config, &env);
    hifs->push_back(std::move(hif));
  }
  rdma.ConnectHostInterface(hifs.get());

  QpOptions qp_options;
  qp_options.dst_ip = "::1";

  std::pair<QpId, QpId> qp = make_pair(1, 100);
  rdma.CreateRcQp(qp.first, qp.second, qp_options,
                  RdmaConnectedMode::kOrderedRc);

  CompletionCallback completion_callback = [](Packet::Syndrome syndrome) {};
  // This op should be sent soon.
  rdma.PerformOp(/* qp_id = */ qp.first,
                 /* opcode = */ RdmaOpcode::kRead,
                 /* sgl = */ {200, 500, 700}, /* is_inline = */ false,
                 completion_callback, kInvalidQpId);
  // This op needs to wait for credit return.
  rdma.PerformOp(/* qp_id = */ qp.first,
                 /* opcode = */ RdmaOpcode::kRead,
                 /* sgl = */ {200, 500, 700}, /* is_inline = */ false,
                 completion_callback, kInvalidQpId);

  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(100), [&]() {
    rdma.IncreaseFalconCreditLimitForTesting(
        /* qp_id = */ qp.first, FalconCredit{.request_rx_buffer = 15});
  }));

  {
    InSequence seq;

    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.rsn, 0);
          EXPECT_LT(env.ElapsedTime(), absl::Nanoseconds(100));
        })
        .RetiresOnSaturation();

    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.rsn, 1);
          EXPECT_GE(env.ElapsedTime(), absl::Nanoseconds(100));
        })
        .RetiresOnSaturation();
  }

  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(200), [&]() { env.Stop(); }));
  env.Run();
}

TEST(RdmaFalconModelTest, ResponseTxPacketCredit) {
  SimpleEnvironment env;

  RdmaConfig config = DefaultConfigGenerator::DefaultRdmaConfig();
  config.mutable_per_qp_credits()->set_tx_packet_data(1);
  FakeConnectionManager connection_manager;
  RdmaFalconModel rdma(config, &env,
                       /*stats_collector=*/nullptr, &connection_manager);

  NiceMock<MockFalcon> falcon;
  rdma.ConnectFalcon(&falcon);
  isekai::RNicConfig rnic_config =
      isekai::DefaultConfigGenerator::DefaultRNicConfig();
  auto hifs =
      std::make_unique<std::vector<std::unique_ptr<isekai::MemoryInterface>>>();
  for (const auto& host_intf_config : rnic_config.host_interface_config()) {
    auto hif =
        std::make_unique<isekai::MemoryInterface>(host_intf_config, &env);
    hifs->push_back(std::move(hif));
  }
  rdma.ConnectHostInterface(hifs.get());

  QpOptions qp_options;
  qp_options.dst_ip = "::1";

  std::pair<QpId, QpId> qp = make_pair(1, 100);
  rdma.CreateRcQp(qp.first, qp.second, qp_options,
                  RdmaConnectedMode::kOrderedRc);

  // The response to this request should be sent soon.
  auto packet = std::make_unique<Packet>();
  packet->rdma.opcode = Packet::Rdma::Opcode::kReadRequest;
  packet->rdma.sgl = {1, 2, 3, 4, 5, 6, 7, 8};
  packet->rdma.dest_qp_id = qp.first;
  packet->rdma.rsn = 0;

  auto cookie1 = std::make_unique<OpaqueCookie>();
  rdma.HandleRxTransaction(std::move(packet), std::move(cookie1));

  // The response to this request needs to wait for credit return.
  packet = std::make_unique<Packet>();
  packet->rdma.opcode = Packet::Rdma::Opcode::kReadRequest;
  packet->rdma.sgl = {1, 2, 3, 4, 5, 6, 7, 8};
  packet->rdma.dest_qp_id = qp.first;
  packet->rdma.rsn = 1;
  auto cookie2 = std::make_unique<OpaqueCookie>();
  rdma.HandleRxTransaction(std::move(packet), std::move(cookie2));

  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(100), [&]() {
    rdma.IncreaseFalconCreditLimitForTesting(
        /* qp_id = */ qp.first, FalconCredit{.response_tx_packet = 1});
  }));

  {
    InSequence seq;

    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.rsn, 0);
          EXPECT_LT(env.ElapsedTime(), absl::Nanoseconds(100));
        })
        .RetiresOnSaturation();

    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.rsn, 1);
          EXPECT_GE(env.ElapsedTime(), absl::Nanoseconds(100));
        })
        .RetiresOnSaturation();
  }

  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(200), [&]() { env.Stop(); }));
  env.Run();
}

TEST(RdmaFalconModelTest, ResponseTxBufferCredit) {
  SimpleEnvironment env;

  RdmaConfig config = DefaultConfigGenerator::DefaultRdmaConfig();
  config.mutable_per_qp_credits()->set_tx_buffer_data(3);
  FakeConnectionManager connection_manager;
  RdmaFalconModel rdma(config, &env,
                       /*stats_collector=*/nullptr, &connection_manager);

  NiceMock<MockFalcon> falcon;
  rdma.ConnectFalcon(&falcon);
  isekai::RNicConfig rnic_config =
      isekai::DefaultConfigGenerator::DefaultRNicConfig();
  auto hifs =
      std::make_unique<std::vector<std::unique_ptr<isekai::MemoryInterface>>>();
  for (const auto& host_intf_config : rnic_config.host_interface_config()) {
    auto hif =
        std::make_unique<isekai::MemoryInterface>(host_intf_config, &env);
    hifs->push_back(std::move(hif));
  }
  rdma.ConnectHostInterface(hifs.get());

  QpOptions qp_options;
  qp_options.dst_ip = "::1";

  std::pair<QpId, QpId> qp = make_pair(1, 100);
  rdma.CreateRcQp(qp.first, qp.second, qp_options,
                  RdmaConnectedMode::kOrderedRc);

  // The response to this request should be sent soon.
  auto packet = std::make_unique<Packet>();
  packet->rdma.opcode = Packet::Rdma::Opcode::kReadRequest;
  packet->rdma.sgl = {1, 2, 3, 4, 5, 6, 7, 8};
  packet->rdma.dest_qp_id = qp.first;
  packet->rdma.rsn = 0;
  auto cookie1 = std::make_unique<OpaqueCookie>();
  rdma.HandleRxTransaction(std::move(packet), std::move(cookie1));

  // The response to this request needs to wait for credit return.
  packet = std::make_unique<Packet>();
  packet->rdma.opcode = Packet::Rdma::Opcode::kReadRequest;
  packet->rdma.sgl = {1, 2, 3, 4, 5, 6, 7, 8};
  packet->rdma.dest_qp_id = qp.first;
  packet->rdma.rsn = 1;
  auto cookie2 = std::make_unique<OpaqueCookie>();
  rdma.HandleRxTransaction(std::move(packet), std::move(cookie2));

  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(100), [&]() {
    rdma.IncreaseFalconCreditLimitForTesting(
        /* qp_id = */ qp.first, FalconCredit{.response_tx_buffer = 3});
  }));

  {
    InSequence seq;

    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.rsn, 0);
          EXPECT_LT(env.ElapsedTime(), absl::Nanoseconds(100));
        })
        .RetiresOnSaturation();

    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.rsn, 1);
          EXPECT_GE(env.ElapsedTime(), absl::Nanoseconds(100));
        })
        .RetiresOnSaturation();
  }

  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(200), [&]() { env.Stop(); }));
  env.Run();
}

TEST(RdmaFalconModelTest, RequestXoff) {
  SimpleEnvironment env;

  RdmaConfig config = DefaultConfigGenerator::DefaultRdmaConfig();
  config.mutable_per_qp_credits()->set_tx_buffer_data(3);
  FakeConnectionManager connection_manager;
  RdmaFalconModel rdma(config, &env,
                       /*stats_collector=*/nullptr, &connection_manager);

  NiceMock<MockFalcon> falcon;
  rdma.ConnectFalcon(&falcon);
  isekai::RNicConfig rnic_config =
      isekai::DefaultConfigGenerator::DefaultRNicConfig();
  auto hifs =
      std::make_unique<std::vector<std::unique_ptr<isekai::MemoryInterface>>>();
  for (const auto& host_intf_config : rnic_config.host_interface_config()) {
    auto hif =
        std::make_unique<isekai::MemoryInterface>(host_intf_config, &env);
    hifs->push_back(std::move(hif));
  }
  rdma.ConnectHostInterface(hifs.get());

  QpOptions qp_options;
  qp_options.dst_ip = "::1";

  std::pair<QpId, QpId> qp = make_pair(1, 100);
  rdma.CreateRcQp(qp.first, qp.second, qp_options,
                  RdmaConnectedMode::kOrderedRc);

  rdma.SetXoff(/*request_xoff=*/true, /*global_xoff=*/false);

  CompletionCallback completion_callback = [](Packet::Syndrome syndrome) {};
  // This request needs to wait until request_xoff = false.
  rdma.PerformOp(/* qp_id = */ qp.first,
                 /* opcode = */ RdmaOpcode::kWrite,
                 /* sgl = */ {200, 500, 700}, /* is_inline = */ false,
                 completion_callback, kInvalidQpId);

  // The response to this request should be sent soon.
  auto packet = std::make_unique<Packet>();
  packet->rdma.opcode = Packet::Rdma::Opcode::kReadRequest;
  packet->rdma.sgl = {1, 2, 3, 4, 5, 6, 7, 8};
  packet->rdma.dest_qp_id = qp.first;
  packet->rdma.rsn = 0;
  auto cookie = std::make_unique<OpaqueCookie>();
  rdma.HandleRxTransaction(std::move(packet), std::move(cookie));

  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(100), [&]() {
    rdma.SetXoff(/*request_xoff=*/false, /*global_xoff=*/false);
  }));

  {
    InSequence seq;

    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.opcode, Packet::Rdma::Opcode::kReadResponseOnly);
          EXPECT_LT(env.ElapsedTime(), absl::Nanoseconds(100));
        })
        .RetiresOnSaturation();

    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.opcode, Packet::Rdma::Opcode::kWriteOnly);
          EXPECT_GE(env.ElapsedTime(), absl::Nanoseconds(100));
        })
        .RetiresOnSaturation();
  }

  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(200), [&]() { env.Stop(); }));
  env.Run();
}

TEST(RdmaFalconModelTest, GlobalXoff) {
  SimpleEnvironment env;

  RdmaConfig config = DefaultConfigGenerator::DefaultRdmaConfig();
  config.mutable_per_qp_credits()->set_tx_buffer_data(3);
  FakeConnectionManager connection_manager;
  RdmaFalconModel rdma(config, &env,
                       /*stats_collector=*/nullptr, &connection_manager);

  NiceMock<MockFalcon> falcon;
  rdma.ConnectFalcon(&falcon);
  isekai::RNicConfig rnic_config =
      isekai::DefaultConfigGenerator::DefaultRNicConfig();
  auto hifs =
      std::make_unique<std::vector<std::unique_ptr<isekai::MemoryInterface>>>();
  for (const auto& host_intf_config : rnic_config.host_interface_config()) {
    auto hif =
        std::make_unique<isekai::MemoryInterface>(host_intf_config, &env);
    hifs->push_back(std::move(hif));
  }
  rdma.ConnectHostInterface(hifs.get());

  QpOptions qp_options;
  qp_options.dst_ip = "::1";

  std::pair<QpId, QpId> qp = make_pair(1, 100);
  rdma.CreateRcQp(qp.first, qp.second, qp_options,
                  RdmaConnectedMode::kOrderedRc);

  rdma.SetXoff(/*request_xoff=*/false, /*global_xoff=*/true);

  CompletionCallback completion_callback = [](Packet::Syndrome syndrome) {};
  // This request needs to wait until request_xoff = false.
  rdma.PerformOp(/* qp_id = */ qp.first,
                 /* opcode = */ RdmaOpcode::kWrite,
                 /* sgl = */ {200, 500, 700}, /* is_inline = */ false,
                 completion_callback, kInvalidQpId);

  // The response to this request should also be blocked by global xoff.
  auto packet = std::make_unique<Packet>();
  packet->rdma.opcode = Packet::Rdma::Opcode::kReadRequest;
  packet->rdma.sgl = {1, 2, 3, 4, 5, 6, 7, 8};
  packet->rdma.dest_qp_id = qp.first;
  packet->rdma.rsn = 0;
  auto cookie = std::make_unique<OpaqueCookie>();
  rdma.HandleRxTransaction(std::move(packet), std::move(cookie));

  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(100), [&]() {
    rdma.SetXoff(/*request_xoff=*/false, /*global_xoff=*/false);
  }));

  {
    InSequence seq;

    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.opcode, Packet::Rdma::Opcode::kWriteOnly);
          EXPECT_GE(env.ElapsedTime(), absl::Nanoseconds(100));
        })
        .RetiresOnSaturation();

    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.opcode, Packet::Rdma::Opcode::kReadResponseOnly);
          EXPECT_GE(env.ElapsedTime(), absl::Nanoseconds(100));
        })
        .RetiresOnSaturation();
  }

  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(200), [&]() { env.Stop(); }));
  env.Run();
}

TEST(RdmaFalconModelTest, QpRequestXoff) {
  SimpleEnvironment env;

  RdmaConfig config = DefaultConfigGenerator::DefaultRdmaConfig();
  config.mutable_per_qp_credits()->set_tx_buffer_data(3);
  FakeConnectionManager connection_manager;
  RdmaFalconModel rdma(config, &env,
                       /*stats_collector=*/nullptr, &connection_manager);

  NiceMock<MockFalcon> falcon;
  rdma.ConnectFalcon(&falcon);
  isekai::RNicConfig rnic_config =
      isekai::DefaultConfigGenerator::DefaultRNicConfig();
  auto hifs =
      std::make_unique<std::vector<std::unique_ptr<isekai::MemoryInterface>>>();
  for (const auto& host_intf_config : rnic_config.host_interface_config()) {
    auto hif =
        std::make_unique<isekai::MemoryInterface>(host_intf_config, &env);
    hifs->push_back(std::move(hif));
  }
  rdma.ConnectHostInterface(hifs.get());

  QpOptions qp_options;
  qp_options.dst_ip = "::1";

  std::pair<QpId, QpId> qp_1 = make_pair(1, 100);
  std::pair<QpId, QpId> qp_2 = make_pair(2, 101);
  rdma.CreateRcQp(qp_1.first, qp_1.second, qp_options,
                  RdmaConnectedMode::kOrderedRc);
  rdma.CreateRcQp(qp_2.first, qp_2.second, qp_options,
                  RdmaConnectedMode::kOrderedRc);

  BackpressureData backpressure_xoff_true;
  backpressure_xoff_true.xoff.request = true;
  backpressure_xoff_true.xoff.response = false;
  rdma.BackpressureQP(qp_1.first, BackpressureType::kXoff,
                      backpressure_xoff_true);
  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(10), [&]() {
    rdma.BackpressureQP(qp_2.first, BackpressureType::kXoff,
                        backpressure_xoff_true);
  }));

  CompletionCallback completion_callback = [](Packet::Syndrome syndrome) {};
  // This request needs to wait until xoff = false.
  rdma.PerformOp(/* qp_id = */ qp_1.first,
                 /* opcode = */ RdmaOpcode::kWrite,
                 /* sgl = */ {10}, /* is_inline = */ true, completion_callback,
                 kInvalidQpId);
  rdma.PerformOp(/* qp_id = */ qp_2.first,
                 /* opcode = */ RdmaOpcode::kWrite,
                 /* sgl = */ {11}, /* is_inline = */ true, completion_callback,
                 kInvalidQpId);
  rdma.PerformOp(/* qp_id = */ qp_2.first,
                 /* opcode = */ RdmaOpcode::kWrite,
                 /* sgl = */ {12}, /* is_inline = */ true, completion_callback,
                 kInvalidQpId);

  // The response to this request should be sent soon.
  auto packet = std::make_unique<Packet>();
  packet->rdma.opcode = Packet::Rdma::Opcode::kReadRequest;
  packet->rdma.sgl = {1, 2, 3, 4, 5, 6, 7, 8};
  packet->rdma.dest_qp_id = qp_1.first;
  packet->rdma.rsn = 0;
  auto cookie = std::make_unique<OpaqueCookie>();
  rdma.HandleRxTransaction(std::move(packet), std::move(cookie));

  BackpressureData backpressure_xoff_false;
  backpressure_xoff_false.xoff.request = false;
  backpressure_xoff_false.xoff.response = false;
  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(100), [&]() {
    rdma.BackpressureQP(qp_1.first, BackpressureType::kXoff,
                        backpressure_xoff_false);
  }));
  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(200), [&]() {
    rdma.BackpressureQP(qp_2.first, BackpressureType::kXoff,
                        backpressure_xoff_false);
  }));

  {
    InSequence seq;
    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.opcode, Packet::Rdma::Opcode::kWriteOnly);
          EXPECT_EQ(p->rdma.inline_payload_length, 11);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(10));
        })
        .RetiresOnSaturation();
    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.opcode, Packet::Rdma::Opcode::kReadResponseOnly);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(20));
        })
        .RetiresOnSaturation();
    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.opcode, Packet::Rdma::Opcode::kWriteOnly);
          EXPECT_EQ(p->rdma.inline_payload_length, 10);
          EXPECT_GE(env.ElapsedTime(), absl::Nanoseconds(100));
        })
        .RetiresOnSaturation();
    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.opcode, Packet::Rdma::Opcode::kWriteOnly);
          EXPECT_EQ(p->rdma.inline_payload_length, 12);
          EXPECT_GE(env.ElapsedTime(), absl::Nanoseconds(200));
        })
        .RetiresOnSaturation();
  }

  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(300), [&]() { env.Stop(); }));
  env.Run();
}

TEST(RdmaFalconModelTest, QpResponseXoff) {
  SimpleEnvironment env;

  RdmaConfig config = DefaultConfigGenerator::DefaultRdmaConfig();
  config.mutable_per_qp_credits()->set_tx_buffer_data(3);
  FakeConnectionManager connection_manager;
  RdmaFalconModel rdma(config, &env,
                       /*stats_collector=*/nullptr, &connection_manager);

  NiceMock<MockFalcon> falcon;
  rdma.ConnectFalcon(&falcon);
  isekai::RNicConfig rnic_config =
      isekai::DefaultConfigGenerator::DefaultRNicConfig();
  auto hifs =
      std::make_unique<std::vector<std::unique_ptr<isekai::MemoryInterface>>>();
  for (const auto& host_intf_config : rnic_config.host_interface_config()) {
    auto hif =
        std::make_unique<isekai::MemoryInterface>(host_intf_config, &env);
    hifs->push_back(std::move(hif));
  }
  rdma.ConnectHostInterface(hifs.get());

  QpOptions qp_options;
  qp_options.dst_ip = "::1";

  std::pair<QpId, QpId> qp_1 = make_pair(1, 100);
  std::pair<QpId, QpId> qp_2 = make_pair(2, 101);
  rdma.CreateRcQp(qp_1.first, qp_1.second, qp_options,
                  RdmaConnectedMode::kOrderedRc);
  rdma.CreateRcQp(qp_2.first, qp_2.second, qp_options,
                  RdmaConnectedMode::kOrderedRc);

  BackpressureData backpressure_xoff_true;
  backpressure_xoff_true.xoff.request = false;
  backpressure_xoff_true.xoff.response = true;
  rdma.BackpressureQP(qp_1.first, BackpressureType::kXoff,
                      backpressure_xoff_true);
  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(10), [&]() {
    rdma.BackpressureQP(qp_2.first, BackpressureType::kXoff,
                        backpressure_xoff_true);
  }));

  CompletionCallback completion_callback = [](Packet::Syndrome syndrome) {};

  rdma.PerformOp(/* qp_id = */ qp_1.first,
                 /* opcode = */ RdmaOpcode::kWrite,
                 /* sgl = */ {10}, /* is_inline = */ true, completion_callback,
                 kInvalidQpId);
  rdma.PerformOp(/* qp_id = */ qp_2.first,
                 /* opcode = */ RdmaOpcode::kWrite,
                 /* sgl = */ {11}, /* is_inline = */ true, completion_callback,
                 kInvalidQpId);
  rdma.PerformOp(/* qp_id = */ qp_2.first,
                 /* opcode = */ RdmaOpcode::kWrite,
                 /* sgl = */ {12}, /* is_inline = */ true, completion_callback,
                 kInvalidQpId);

  auto packet1 = std::make_unique<Packet>();
  packet1->rdma.opcode = Packet::Rdma::Opcode::kReadRequest;
  packet1->rdma.sgl = {1};
  packet1->rdma.dest_qp_id = qp_1.first;
  packet1->rdma.rsn = 0;
  auto cookie1 = std::make_unique<OpaqueCookie>();
  rdma.HandleRxTransaction(std::move(packet1), std::move(cookie1));

  auto packet2 = std::make_unique<Packet>();
  packet2->rdma.opcode = Packet::Rdma::Opcode::kReadRequest;
  packet2->rdma.sgl = {1};
  packet2->rdma.dest_qp_id = qp_2.first;
  packet2->rdma.rsn = 0;
  auto cookie2 = std::make_unique<OpaqueCookie>();
  rdma.HandleRxTransaction(std::move(packet2), std::move(cookie2));

  BackpressureData backpressure_xoff_false;
  backpressure_xoff_false.xoff.request = false;
  backpressure_xoff_false.xoff.response = false;
  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(100), [&]() {
    rdma.BackpressureQP(qp_1.first, BackpressureType::kXoff,
                        backpressure_xoff_false);
  }));
  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(200), [&]() {
    rdma.BackpressureQP(qp_2.first, BackpressureType::kXoff,
                        backpressure_xoff_false);
  }));

  {
    InSequence seq;
    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.opcode, Packet::Rdma::Opcode::kWriteOnly);
          EXPECT_EQ(p->rdma.inline_payload_length, 10);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(10));
        })
        .RetiresOnSaturation();
    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.opcode, Packet::Rdma::Opcode::kWriteOnly);
          EXPECT_EQ(p->rdma.inline_payload_length, 11);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(20));
        })
        .RetiresOnSaturation();
    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.opcode, Packet::Rdma::Opcode::kWriteOnly);
          EXPECT_EQ(p->rdma.inline_payload_length, 12);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(30));
        })
        .RetiresOnSaturation();
    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.opcode, Packet::Rdma::Opcode::kReadResponseOnly);
          EXPECT_EQ(p->rdma.dest_qp_id, qp_1.second);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(100));
        })
        .RetiresOnSaturation();
    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.opcode, Packet::Rdma::Opcode::kReadResponseOnly);
          EXPECT_EQ(p->rdma.dest_qp_id, qp_2.second);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(200));
        })
        .RetiresOnSaturation();
  }

  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(300), [&]() { env.Stop(); }));
  env.Run();
}

TEST(RdmaFalconModelTest, QpDynamicLimit) {
  SimpleEnvironment env;

  RdmaConfig config = DefaultConfigGenerator::DefaultRdmaConfig();
  config.mutable_per_qp_credits()->set_tx_packet_request(10);
  config.mutable_per_qp_credits()->set_tx_buffer_request(10);
  config.mutable_per_qp_credits()->set_rx_packet_request(10);
  config.mutable_per_qp_credits()->set_rx_buffer_request(10);
  config.mutable_per_qp_credits()->set_tx_packet_data(10);
  config.mutable_per_qp_credits()->set_tx_buffer_data(10);
  FakeConnectionManager connection_manager;
  RdmaFalconModel rdma(config, &env,
                       /*stats_collector=*/nullptr, &connection_manager);

  NiceMock<MockFalcon> falcon;
  rdma.ConnectFalcon(&falcon);
  isekai::RNicConfig rnic_config =
      isekai::DefaultConfigGenerator::DefaultRNicConfig();
  auto hifs =
      std::make_unique<std::vector<std::unique_ptr<isekai::MemoryInterface>>>();
  for (const auto& host_intf_config : rnic_config.host_interface_config()) {
    auto hif =
        std::make_unique<isekai::MemoryInterface>(host_intf_config, &env);
    hifs->push_back(std::move(hif));
  }
  rdma.ConnectHostInterface(hifs.get());

  QpOptions qp_options;
  qp_options.dst_ip = "::1";

  std::pair<QpId, QpId> qp_1 = make_pair(1, 100);
  rdma.CreateRcQp(qp_1.first, qp_1.second, qp_options,
                  RdmaConnectedMode::kOrderedRc);

  // We update QP 1's dynamic limits into 2.
  BackpressureData backpressure_data_1;
  FalconCredit dynamic_limit_1;
  dynamic_limit_1.request_tx_packet = 2;
  dynamic_limit_1.request_tx_buffer = 2;
  dynamic_limit_1.request_rx_packet = 2;
  dynamic_limit_1.request_rx_buffer = 2;
  dynamic_limit_1.response_tx_packet = 2;
  dynamic_limit_1.response_tx_buffer = 2;
  backpressure_data_1.credit_limit = dynamic_limit_1;

  rdma.BackpressureQP(qp_1.first, BackpressureType::kDynamicCredit,
                      backpressure_data_1);

  CompletionCallback completion_callback = [](Packet::Syndrome syndrome) {};
  rdma.PerformOp(/* qp_id = */ qp_1.first,
                 /* opcode = */ RdmaOpcode::kWrite,
                 /* sgl = */ {10}, /* is_inline = */ true, completion_callback,
                 kInvalidQpId);
  rdma.PerformOp(/* qp_id = */ qp_1.first,
                 /* opcode = */ RdmaOpcode::kWrite,
                 /* sgl = */ {11}, /* is_inline = */ true, completion_callback,
                 kInvalidQpId);
  rdma.PerformOp(/* qp_id = */ qp_1.first,
                 /* opcode = */ RdmaOpcode::kWrite,
                 /* sgl = */ {12}, /* is_inline = */ true, completion_callback,
                 kInvalidQpId);

  BackpressureData backpressure_data_2;
  FalconCredit dynamic_limit_2;
  dynamic_limit_2.request_tx_packet = 3;
  dynamic_limit_2.request_tx_buffer = 3;
  dynamic_limit_2.request_rx_packet = 3;
  dynamic_limit_2.request_rx_buffer = 3;
  dynamic_limit_2.response_tx_packet = 3;
  dynamic_limit_2.response_tx_buffer = 3;
  backpressure_data_2.credit_limit = dynamic_limit_2;

  // At 100ns, we update QP 1's dynamic limits into 3.
  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(100), [&]() {
    rdma.BackpressureQP(qp_1.first, BackpressureType::kDynamicCredit,
                        backpressure_data_2);
  }));

  // In the beginning, QP 1's dynamic limit is 2, thus this QP schedules two
  // transactions at 10 ns and 20 ns. At 100ns, QP's dynamic limit is 3, thus
  // the third transaction is scheduled.
  {
    InSequence seq;
    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.opcode, Packet::Rdma::Opcode::kWriteOnly);
          EXPECT_EQ(p->rdma.inline_payload_length, 10);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(10));
        })
        .RetiresOnSaturation();
    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.opcode, Packet::Rdma::Opcode::kWriteOnly);
          EXPECT_EQ(p->rdma.inline_payload_length, 11);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(20));
        })
        .RetiresOnSaturation();
    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.opcode, Packet::Rdma::Opcode::kWriteOnly);
          EXPECT_EQ(p->rdma.inline_payload_length, 12);
          EXPECT_GE(env.ElapsedTime(), absl::Nanoseconds(100));
        })
        .RetiresOnSaturation();
  }

  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(300), [&]() { env.Stop(); }));
  env.Run();
}

TEST(RdmaFalconModelTest, GlobalCreditExhaustionTxPacketRequest) {
  SimpleEnvironment env;

  RdmaConfig config = DefaultConfigGenerator::DefaultRdmaConfig();
  // Reduce request tx packet credit to single transaction.
  config.mutable_global_credits()->set_tx_packet_request(1);

  FakeConnectionManager connection_manager;
  RdmaFalconModel rdma(config, &env,
                       /*stats_collector=*/nullptr, &connection_manager);

  NiceMock<MockFalcon> falcon;
  rdma.ConnectFalcon(&falcon);
  isekai::RNicConfig rnic_config =
      isekai::DefaultConfigGenerator::DefaultRNicConfig();
  auto hifs =
      std::make_unique<std::vector<std::unique_ptr<isekai::MemoryInterface>>>();
  for (const auto& host_intf_config : rnic_config.host_interface_config()) {
    auto hif =
        std::make_unique<isekai::MemoryInterface>(host_intf_config, &env);
    hifs->push_back(std::move(hif));
  }
  rdma.ConnectHostInterface(hifs.get());

  QpOptions qp_options;
  qp_options.dst_ip = "::1";

  std::pair<QpId, QpId> qp = make_pair(1, 100);
  rdma.CreateRcQp(qp.first, qp.second, qp_options,
                  RdmaConnectedMode::kOrderedRc);

  CompletionCallback completion_callback = [](Packet::Syndrome syndrome) {};
  // This request should be sent out immediately.
  rdma.PerformOp(/* qp_id = */ qp.first,
                 /* opcode = */ RdmaOpcode::kWrite,
                 /* sgl = */ {100}, /* is_inline = */ false,
                 completion_callback, kInvalidQpId);

  // This request needs to wait for credit return.
  rdma.PerformOp(/* qp_id = */ qp.first,
                 /* opcode = */ RdmaOpcode::kWrite,
                 /* sgl = */ {100}, /* is_inline = */ false,
                 completion_callback, kInvalidQpId);

  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(100), [&]() {
    rdma.IncreaseFalconCreditLimitForTesting(
        /* qp_id = */ qp.first, FalconCredit{.request_tx_packet = 1});
  }));

  {
    InSequence seq;

    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.rsn, 0);
          EXPECT_LT(env.ElapsedTime(), absl::Nanoseconds(100));
        })
        .RetiresOnSaturation();

    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.rsn, 1);
          EXPECT_GE(env.ElapsedTime(), absl::Nanoseconds(100));
        })
        .RetiresOnSaturation();
  }

  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(200), [&]() { env.Stop(); }));
  env.Run();
}

TEST(RdmaFalconModelTest, GlobalCreditExhaustionTxBufferRequest) {
  SimpleEnvironment env;

  RdmaConfig config = DefaultConfigGenerator::DefaultRdmaConfig();
  // Reduce request tx buffer credit to exactly single transaction of max size.
  config.mutable_global_credits()->set_tx_buffer_request(14);

  FakeConnectionManager connection_manager;
  RdmaFalconModel rdma(config, &env,
                       /*stats_collector=*/nullptr, &connection_manager);

  NiceMock<MockFalcon> falcon;
  rdma.ConnectFalcon(&falcon);
  isekai::RNicConfig rnic_config =
      isekai::DefaultConfigGenerator::DefaultRNicConfig();
  auto hifs =
      std::make_unique<std::vector<std::unique_ptr<isekai::MemoryInterface>>>();
  for (const auto& host_intf_config : rnic_config.host_interface_config()) {
    auto hif =
        std::make_unique<isekai::MemoryInterface>(host_intf_config, &env);
    hifs->push_back(std::move(hif));
  }
  rdma.ConnectHostInterface(hifs.get());

  QpOptions qp_options;
  qp_options.dst_ip = "::1";

  std::pair<QpId, QpId> qp = make_pair(1, 100);
  rdma.CreateRcQp(qp.first, qp.second, qp_options,
                  RdmaConnectedMode::kOrderedRc);

  CompletionCallback completion_callback = [](Packet::Syndrome syndrome) {};
  // This request should be sent out immediately.
  rdma.PerformOp(/* qp_id = */ qp.first,
                 /* opcode = */ RdmaOpcode::kWrite,
                 /* sgl = */ {3900}, /* is_inline = */ false,
                 completion_callback, kInvalidQpId);

  // This request needs to wait for credit return.
  rdma.PerformOp(/* qp_id = */ qp.first,
                 /* opcode = */ RdmaOpcode::kWrite,
                 /* sgl = */ {3900}, /* is_inline = */ false,
                 completion_callback, kInvalidQpId);

  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(100), [&]() {
    rdma.IncreaseFalconCreditLimitForTesting(
        /* qp_id = */ qp.first, FalconCredit{.request_tx_buffer = 14});
  }));

  {
    InSequence seq;

    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.rsn, 0);
          EXPECT_LT(env.ElapsedTime(), absl::Nanoseconds(100));
        })
        .RetiresOnSaturation();

    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.rsn, 1);
          EXPECT_GE(env.ElapsedTime(), absl::Nanoseconds(100));
        })
        .RetiresOnSaturation();
  }

  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(200), [&]() { env.Stop(); }));
  env.Run();
}

TEST(RdmaFalconModelTest, GlobalCreditExhaustionRxPacketRequest) {
  SimpleEnvironment env;

  RdmaConfig config = DefaultConfigGenerator::DefaultRdmaConfig();
  // Reduce request rx packet credit to single transaction.
  config.mutable_global_credits()->set_rx_packet_request(1);

  FakeConnectionManager connection_manager;
  RdmaFalconModel rdma(config, &env,
                       /*stats_collector=*/nullptr, &connection_manager);

  NiceMock<MockFalcon> falcon;
  rdma.ConnectFalcon(&falcon);
  isekai::RNicConfig rnic_config =
      isekai::DefaultConfigGenerator::DefaultRNicConfig();
  auto hifs =
      std::make_unique<std::vector<std::unique_ptr<isekai::MemoryInterface>>>();
  for (const auto& host_intf_config : rnic_config.host_interface_config()) {
    auto hif =
        std::make_unique<isekai::MemoryInterface>(host_intf_config, &env);
    hifs->push_back(std::move(hif));
  }
  rdma.ConnectHostInterface(hifs.get());

  QpOptions qp_options;
  qp_options.dst_ip = "::1";

  std::pair<QpId, QpId> qp = make_pair(1, 100);
  rdma.CreateRcQp(qp.first, qp.second, qp_options,
                  RdmaConnectedMode::kOrderedRc);

  CompletionCallback completion_callback = [](Packet::Syndrome syndrome) {};
  // This request should be sent out immediately.
  rdma.PerformOp(/* qp_id = */ qp.first,
                 /* opcode = */ RdmaOpcode::kWrite,
                 /* sgl = */ {100}, /* is_inline = */ false,
                 completion_callback, kInvalidQpId);

  // This request needs to wait for credit return.
  rdma.PerformOp(/* qp_id = */ qp.first,
                 /* opcode = */ RdmaOpcode::kWrite,
                 /* sgl = */ {100}, /* is_inline = */ false,
                 completion_callback, kInvalidQpId);

  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(100), [&]() {
    rdma.IncreaseFalconCreditLimitForTesting(
        /* qp_id = */ qp.first, FalconCredit{.request_rx_packet = 1});
  }));

  {
    InSequence seq;

    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.rsn, 0);
          EXPECT_LT(env.ElapsedTime(), absl::Nanoseconds(100));
        })
        .RetiresOnSaturation();

    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.rsn, 1);
          EXPECT_GE(env.ElapsedTime(), absl::Nanoseconds(100));
        })
        .RetiresOnSaturation();
  }

  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(200), [&]() { env.Stop(); }));
  env.Run();
}

TEST(RdmaFalconModelTest, GlobalCreditExhaustionRxBufferRequest) {
  SimpleEnvironment env;

  RdmaConfig config = DefaultConfigGenerator::DefaultRdmaConfig();
  // Reduce request rx buffer credit to single transaction.
  config.mutable_global_credits()->set_rx_buffer_request(32);

  FakeConnectionManager connection_manager;
  RdmaFalconModel rdma(config, &env,
                       /*stats_collector=*/nullptr, &connection_manager);

  NiceMock<MockFalcon> falcon;
  rdma.ConnectFalcon(&falcon);
  isekai::RNicConfig rnic_config =
      isekai::DefaultConfigGenerator::DefaultRNicConfig();
  auto hifs =
      std::make_unique<std::vector<std::unique_ptr<isekai::MemoryInterface>>>();
  for (const auto& host_intf_config : rnic_config.host_interface_config()) {
    auto hif =
        std::make_unique<isekai::MemoryInterface>(host_intf_config, &env);
    hifs->push_back(std::move(hif));
  }
  rdma.ConnectHostInterface(hifs.get());

  QpOptions qp_options;
  qp_options.dst_ip = "::1";

  std::pair<QpId, QpId> qp = make_pair(1, 100);
  rdma.CreateRcQp(qp.first, qp.second, qp_options,
                  RdmaConnectedMode::kOrderedRc);

  CompletionCallback completion_callback = [](Packet::Syndrome syndrome) {};
  // This request should be sent out immediately.
  rdma.PerformOp(/* qp_id = */ qp.first,
                 /* opcode = */ RdmaOpcode::kRead,
                 /* sgl = */ {3900}, /* is_inline = */ false,
                 completion_callback, kInvalidQpId);

  // This request needs to wait for credit return.
  rdma.PerformOp(/* qp_id = */ qp.first,
                 /* opcode = */ RdmaOpcode::kRead,
                 /* sgl = */ {3900}, /* is_inline = */ false,
                 completion_callback, kInvalidQpId);

  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(100), [&]() {
    rdma.IncreaseFalconCreditLimitForTesting(
        /* qp_id = */ qp.first, FalconCredit{.request_rx_buffer = 31});
  }));

  {
    InSequence seq;

    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.rsn, 0);
          EXPECT_LT(env.ElapsedTime(), absl::Nanoseconds(100));
        })
        .RetiresOnSaturation();

    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.rsn, 1);
          EXPECT_GE(env.ElapsedTime(), absl::Nanoseconds(100));
        })
        .RetiresOnSaturation();
  }

  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(200), [&]() { env.Stop(); }));
  env.Run();
}

TEST(RdmaFalconModelTest, GlobalCreditExhaustionTxPacketResponse) {
  SimpleEnvironment env;

  RdmaConfig config = DefaultConfigGenerator::DefaultRdmaConfig();
  // Reduce response tx packet credit to single transaction.
  config.mutable_global_credits()->set_tx_packet_data(1);

  FakeConnectionManager connection_manager;
  RdmaFalconModel rdma(config, &env,
                       /*stats_collector=*/nullptr, &connection_manager);

  NiceMock<MockFalcon> falcon;
  rdma.ConnectFalcon(&falcon);
  isekai::RNicConfig rnic_config =
      isekai::DefaultConfigGenerator::DefaultRNicConfig();
  auto hifs =
      std::make_unique<std::vector<std::unique_ptr<isekai::MemoryInterface>>>();
  for (const auto& host_intf_config : rnic_config.host_interface_config()) {
    auto hif =
        std::make_unique<isekai::MemoryInterface>(host_intf_config, &env);
    hifs->push_back(std::move(hif));
  }
  rdma.ConnectHostInterface(hifs.get());

  QpOptions qp_options;
  qp_options.dst_ip = "::1";

  std::pair<QpId, QpId> qp = make_pair(1, 100);
  rdma.CreateRcQp(qp.first, qp.second, qp_options,
                  RdmaConnectedMode::kOrderedRc);

  // The response to this request should be sent out immediately.
  auto packet = std::make_unique<Packet>();
  packet->rdma.opcode = Packet::Rdma::Opcode::kReadRequest;
  packet->rdma.sgl = {3900};
  packet->rdma.dest_qp_id = qp.first;
  packet->rdma.rsn = 0;
  auto cookie1 = std::make_unique<OpaqueCookie>();
  rdma.HandleRxTransaction(std::move(packet), std::move(cookie1));

  // The response to this request needs to wait for credit return.
  packet = std::make_unique<Packet>();
  packet->rdma.opcode = Packet::Rdma::Opcode::kReadRequest;
  packet->rdma.sgl = {3900};
  packet->rdma.dest_qp_id = qp.first;
  packet->rdma.rsn = 1;
  auto cookie2 = std::make_unique<OpaqueCookie>();
  rdma.HandleRxTransaction(std::move(packet), std::move(cookie2));

  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(100), [&]() {
    rdma.IncreaseFalconCreditLimitForTesting(
        /* qp_id = */ qp.first, FalconCredit{.response_tx_packet = 1});
  }));

  {
    InSequence seq;

    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.rsn, 0);
          EXPECT_LT(env.ElapsedTime(), absl::Nanoseconds(100));
        })
        .RetiresOnSaturation();

    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.rsn, 1);
          EXPECT_GE(env.ElapsedTime(), absl::Nanoseconds(100));
        })
        .RetiresOnSaturation();
  }

  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(200), [&]() { env.Stop(); }));
  env.Run();
}

TEST(RdmaFalconModelTest, GlobalCreditExhaustionTxBufferResponse) {
  SimpleEnvironment env;

  RdmaConfig config = DefaultConfigGenerator::DefaultRdmaConfig();
  // Reduce response tx buffer credit to single transaction.
  config.mutable_global_credits()->set_tx_buffer_data(14);
  FakeConnectionManager connection_manager;
  RdmaFalconModel rdma(config, &env,
                       /*stats_collector=*/nullptr, &connection_manager);

  NiceMock<MockFalcon> falcon;
  rdma.ConnectFalcon(&falcon);
  isekai::RNicConfig rnic_config =
      isekai::DefaultConfigGenerator::DefaultRNicConfig();
  auto hifs =
      std::make_unique<std::vector<std::unique_ptr<isekai::MemoryInterface>>>();
  for (const auto& host_intf_config : rnic_config.host_interface_config()) {
    auto hif =
        std::make_unique<isekai::MemoryInterface>(host_intf_config, &env);
    hifs->push_back(std::move(hif));
  }
  rdma.ConnectHostInterface(hifs.get());

  QpOptions qp_options;
  qp_options.dst_ip = "::1";

  std::pair<QpId, QpId> qp = make_pair(1, 100);
  rdma.CreateRcQp(qp.first, qp.second, qp_options,
                  RdmaConnectedMode::kOrderedRc);

  // The response to this request should be sent soon.
  auto packet = std::make_unique<Packet>();
  packet->rdma.opcode = Packet::Rdma::Opcode::kReadRequest;
  packet->rdma.sgl = {3900};
  packet->rdma.dest_qp_id = qp.first;
  packet->rdma.rsn = 0;
  auto cookie1 = std::make_unique<OpaqueCookie>();
  rdma.HandleRxTransaction(std::move(packet), std::move(cookie1));

  // The response to this request needs to wait for credit return.
  packet = std::make_unique<Packet>();
  packet->rdma.opcode = Packet::Rdma::Opcode::kReadRequest;
  packet->rdma.sgl = {3900};
  packet->rdma.dest_qp_id = qp.first;
  packet->rdma.rsn = 1;
  auto cookie2 = std::make_unique<OpaqueCookie>();
  rdma.HandleRxTransaction(std::move(packet), std::move(cookie2));

  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(100), [&]() {
    rdma.IncreaseFalconCreditLimitForTesting(
        /* qp_id = */ qp.first, FalconCredit{.response_tx_buffer = 14});
  }));

  {
    InSequence seq;

    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.rsn, 0);
          EXPECT_LT(env.ElapsedTime(), absl::Nanoseconds(100));
        })
        .RetiresOnSaturation();

    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.rsn, 1);
          EXPECT_GE(env.ElapsedTime(), absl::Nanoseconds(100));
        })
        .RetiresOnSaturation();
  }

  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(200), [&]() { env.Stop(); }));
  env.Run();
}

TEST(RdmaFalconModelTest, RDMASchedulerPipelineDelay) {
  SimpleEnvironment env;

  RdmaConfig config = DefaultConfigGenerator::DefaultRdmaConfig();
  config.set_scheduler_pipeline_delay_in_cycles(10);
  uint32_t scheduler_pipeline_delay_time_ns =
      config.scheduler_pipeline_delay_in_cycles() * config.chip_cycle_time_ns();
  FakeConnectionManager connection_manager;
  RdmaFalconModel rdma(config, &env,
                       /*stats_collector=*/nullptr, &connection_manager);

  NiceMock<MockFalcon> falcon;
  rdma.ConnectFalcon(&falcon);
  isekai::RNicConfig rnic_config =
      isekai::DefaultConfigGenerator::DefaultRNicConfig();
  auto hifs =
      std::make_unique<std::vector<std::unique_ptr<isekai::MemoryInterface>>>();
  for (const auto& host_intf_config : rnic_config.host_interface_config()) {
    auto hif =
        std::make_unique<isekai::MemoryInterface>(host_intf_config, &env);
    hifs->push_back(std::move(hif));
  }
  rdma.ConnectHostInterface(hifs.get());

  QpOptions qp_options;
  qp_options.dst_ip = "::1";

  std::pair<QpId, QpId> qp1 = make_pair(1, 100);
  rdma.CreateRcQp(qp1.first, qp1.second, qp_options,
                  RdmaConnectedMode::kOrderedRc);

  CompletionCallback completion_callback = [](Packet::Syndrome syndrome) {};

  rdma.PerformOp(/* qp_id = */ qp1.first,
                 /* opcode = */ RdmaOpcode::kWrite,
                 /* sgl = */ {10}, /* is_inline = */ true, completion_callback,
                 kInvalidQpId);
  rdma.PerformOp(/* qp_id = */ qp1.first,
                 /* opcode = */ RdmaOpcode::kWrite,
                 /* sgl = */ {11}, /* is_inline = */ true, completion_callback,
                 kInvalidQpId);

  // With the pipeline delay between RDMA and Falcon, Falcon will receive the
  // transaction after chip_cycle_time_ns * pipeline_delay_in_cycles.
  {
    InSequence seq;

    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.dest_qp_id, qp1.second);
          EXPECT_EQ(p->rdma.inline_payload_length, 10);
          EXPECT_EQ(p->rdma.rsn, 0);
          EXPECT_EQ(env.ElapsedTime(),
                    absl::Nanoseconds(scheduler_pipeline_delay_time_ns + 10));
        })
        .RetiresOnSaturation();

    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.dest_qp_id, qp1.second);
          EXPECT_EQ(p->rdma.inline_payload_length, 11);
          EXPECT_EQ(p->rdma.rsn, 1);
          EXPECT_EQ(env.ElapsedTime(),
                    absl::Nanoseconds(scheduler_pipeline_delay_time_ns + 20));
        })
        .RetiresOnSaturation();
  }

  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(300), [&]() { env.Stop(); }));
  env.Run();
}

TEST(RdmaFalconModelTest, TestTxRateLimiterReadWriteRequests) {
  SimpleEnvironment env;

  RdmaConfig config = DefaultConfigGenerator::DefaultRdmaConfig();
  config.mutable_tx_rate_limiter()->set_refill_interval_ns(10000);
  config.mutable_tx_rate_limiter()->set_burst_size_bytes(kDefaultRdmaMtu +
                                                         kRdmaHeader);
  const int kPerQpInterOpGapNs = 1000 / config.max_qp_oprate_million_per_sec();

  FakeConnectionManager connection_manager;
  RdmaFalconModel rdma(config, &env,
                       /*stats_collector=*/nullptr, &connection_manager);

  NiceMock<MockFalcon> falcon;
  rdma.ConnectFalcon(&falcon);
  isekai::RNicConfig rnic_config =
      isekai::DefaultConfigGenerator::DefaultRNicConfig();
  auto hifs =
      std::make_unique<std::vector<std::unique_ptr<isekai::MemoryInterface>>>();
  for (const auto& host_intf_config : rnic_config.host_interface_config()) {
    auto hif =
        std::make_unique<isekai::MemoryInterface>(host_intf_config, &env);
    hifs->push_back(std::move(hif));
  }
  rdma.ConnectHostInterface(hifs.get());

  QpOptions qp_options;
  qp_options.dst_ip = "::1";

  std::pair<QpId, QpId> qp1 = make_pair(1, 100);
  rdma.CreateRcQp(qp1.first, qp1.second, qp_options,
                  RdmaConnectedMode::kOrderedRc);

  CompletionCallback completion_callback = [](Packet::Syndrome syndrome) {};

  // This OP should result in two Write packets within Falcon chip cycle time,
  // since the tx rate limiter size is 2 MTUs.
  rdma.PerformOp(/* qp_id = */ qp1.first,
                 /* opcode = */ RdmaOpcode::kWrite,
                 /* sgl = */ {7830}, /* is_inline = */ false,
                 completion_callback, kInvalidQpId);
  // These Read ops will be delayed till the next replenish cycle that takes
  // available token to more than MTU. Once replenished, all Read ops are sent
  // out in a short amount of time because they don't consume many TX tokens.
  rdma.PerformOp(/* qp_id = */ qp1.first,
                 /* opcode = */ RdmaOpcode::kRead,
                 /* sgl = */ {16000}, /* is_inline = */ false,
                 completion_callback, kInvalidQpId);

  // With the pipeline delay between RDMA and Falcon, Falcon will receive the
  // transaction after chip_cycle_time_ns * pipeline_delay_in_cycles.
  {
    InSequence seq;

    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.opcode, Packet::Rdma::Opcode::kWriteOnly);
          EXPECT_EQ(p->rdma.dest_qp_id, qp1.second);
          EXPECT_EQ(p->rdma.rsn, 0);
          EXPECT_EQ(env.ElapsedTime(),
                    absl::Nanoseconds(config.chip_cycle_time_ns()));
        })
        .RetiresOnSaturation();

    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.opcode, Packet::Rdma::Opcode::kWriteOnly);
          EXPECT_EQ(p->rdma.dest_qp_id, qp1.second);
          EXPECT_EQ(p->rdma.rsn, 1);
          EXPECT_EQ(env.ElapsedTime(),
                    absl::Nanoseconds(config.chip_cycle_time_ns() +
                                      kPerQpInterOpGapNs));
        })
        .RetiresOnSaturation();

    // The following Read requests will be delayed till the next replenish
    // cycle.
    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.opcode, Packet::Rdma::Opcode::kReadRequest);
          EXPECT_EQ(p->rdma.dest_qp_id, qp1.second);
          EXPECT_EQ(p->rdma.rsn, 2);
          EXPECT_GE(
              env.ElapsedTime(),
              absl::Nanoseconds(config.tx_rate_limiter().refill_interval_ns()));
        })
        .RetiresOnSaturation();

    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.opcode, Packet::Rdma::Opcode::kReadRequest);
          EXPECT_EQ(p->rdma.dest_qp_id, qp1.second);
          EXPECT_EQ(p->rdma.rsn, 3);
          EXPECT_GE(
              env.ElapsedTime(),
              absl::Nanoseconds(config.tx_rate_limiter().refill_interval_ns() +
                                config.chip_cycle_time_ns()));
        })
        .RetiresOnSaturation();

    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.opcode, Packet::Rdma::Opcode::kReadRequest);
          EXPECT_EQ(p->rdma.dest_qp_id, qp1.second);
          EXPECT_EQ(p->rdma.rsn, 4);
          EXPECT_GE(
              env.ElapsedTime(),
              absl::Nanoseconds(config.tx_rate_limiter().refill_interval_ns() +
                                2 * config.chip_cycle_time_ns()));
        })
        .RetiresOnSaturation();

    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.opcode, Packet::Rdma::Opcode::kReadRequest);
          EXPECT_EQ(p->rdma.dest_qp_id, qp1.second);
          EXPECT_EQ(p->rdma.rsn, 5);
          EXPECT_GE(
              env.ElapsedTime(),
              absl::Nanoseconds(config.tx_rate_limiter().refill_interval_ns() +
                                3 * config.chip_cycle_time_ns()));
        })
        .RetiresOnSaturation();
  }

  EXPECT_OK(env.ScheduleEvent(absl::Microseconds(200), [&]() { env.Stop(); }));
  env.Run();
}

TEST(RdmaFalconModelTest, TestTxRateLimiterReadResponse) {
  SimpleEnvironment env;

  RdmaConfig config = DefaultConfigGenerator::DefaultRdmaConfig();
  config.set_max_qp_oprate_million_per_sec(200);
  config.mutable_tx_rate_limiter()->set_refill_interval_ns(10000);
  config.mutable_tx_rate_limiter()->set_burst_size_bytes(7500);

  FakeConnectionManager connection_manager;
  RdmaFalconModel rdma(config, &env,
                       /*stats_collector=*/nullptr, &connection_manager);

  NiceMock<MockFalcon> falcon;
  rdma.ConnectFalcon(&falcon);
  isekai::RNicConfig rnic_config =
      isekai::DefaultConfigGenerator::DefaultRNicConfig();
  auto hifs =
      std::make_unique<std::vector<std::unique_ptr<isekai::MemoryInterface>>>();
  for (const auto& host_intf_config : rnic_config.host_interface_config()) {
    auto hif =
        std::make_unique<isekai::MemoryInterface>(host_intf_config, &env);
    hifs->push_back(std::move(hif));
  }
  rdma.ConnectHostInterface(hifs.get());

  QpOptions qp_options;
  qp_options.dst_ip = "::1";

  std::pair<QpId, QpId> qp = make_pair(1, 100);
  rdma.CreateRcQp(qp.first, qp.second, qp_options,
                  RdmaConnectedMode::kOrderedRc);

  auto packet1 = std::make_unique<Packet>();
  packet1->rdma.opcode = Packet::Rdma::Opcode::kReadRequest;
  packet1->rdma.sgl = {3800};
  packet1->rdma.dest_qp_id = qp.first;
  packet1->rdma.rsn = 0;
  auto cookie1 = std::make_unique<OpaqueCookie>();
  rdma.HandleRxTransaction(std::move(packet1), std::move(cookie1));

  auto packet2 = std::make_unique<Packet>();
  packet2->rdma.opcode = Packet::Rdma::Opcode::kReadRequest;
  packet2->rdma.sgl = {3800};
  packet2->rdma.dest_qp_id = qp.first;
  packet2->rdma.rsn = 1;
  auto cookie2 = std::make_unique<OpaqueCookie>();
  rdma.HandleRxTransaction(std::move(packet2), std::move(cookie2));

  auto packet3 = std::make_unique<Packet>();
  packet3->rdma.opcode = Packet::Rdma::Opcode::kReadRequest;
  packet3->rdma.sgl = {100};
  packet3->rdma.dest_qp_id = qp.first;
  packet3->rdma.rsn = 2;
  auto cookie3 = std::make_unique<OpaqueCookie>();
  rdma.HandleRxTransaction(std::move(packet3), std::move(cookie3));

  // With the pipeline delay between RDMA and Falcon, Falcon will receive the
  // transaction after chip_cycle_time_ns * pipeline_delay_in_cycles.
  {
    InSequence seq;

    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->metadata.scid, qp_options.scid);
          EXPECT_EQ(p->rdma.opcode, Packet::Rdma::Opcode::kReadResponseOnly);
          EXPECT_THAT(p->rdma.sgl, ElementsAre(3800));
          EXPECT_EQ(p->rdma.dest_qp_id, qp.second);
          EXPECT_EQ(p->rdma.rsn, 0);
          EXPECT_EQ(env.ElapsedTime(),
                    absl::Nanoseconds(config.chip_cycle_time_ns()));
        })
        .RetiresOnSaturation();

    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->metadata.scid, qp_options.scid);
          EXPECT_EQ(p->rdma.opcode, Packet::Rdma::Opcode::kReadResponseOnly);
          EXPECT_THAT(p->rdma.sgl, ElementsAre(3800));
          EXPECT_EQ(p->rdma.dest_qp_id, qp.second);
          EXPECT_EQ(p->rdma.rsn, 1);
          EXPECT_EQ(env.ElapsedTime(),
                    absl::Nanoseconds(2 * config.chip_cycle_time_ns()));
        })
        .RetiresOnSaturation();

    // This op should be delayed by the TX rate limiter until it is replenished.
    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->metadata.scid, qp_options.scid);
          EXPECT_EQ(p->rdma.opcode, Packet::Rdma::Opcode::kReadResponseOnly);
          EXPECT_THAT(p->rdma.sgl, ElementsAre(100));
          EXPECT_EQ(p->rdma.dest_qp_id, qp.second);
          EXPECT_EQ(p->rdma.rsn, 2);
          EXPECT_EQ(
              env.ElapsedTime(),
              absl::Nanoseconds(config.tx_rate_limiter().refill_interval_ns()));
        })
        .RetiresOnSaturation();
  }

  EXPECT_OK(env.ScheduleEvent(absl::Microseconds(200), [&]() { env.Stop(); }));
  env.Run();
}

TEST(RdmaFalconModelTest, TestSimultaneousRequestResponseLimits) {
  SimpleEnvironment env;

  RdmaConfig config = DefaultConfigGenerator::DefaultRdmaConfig();
  // Limit global credits such that we can send exactly one request and one
  // response.
  config.mutable_global_credits()->set_tx_packet_request(1);
  config.mutable_global_credits()->set_tx_packet_data(1);

  FakeConnectionManager connection_manager;
  RdmaFalconModel rdma(config, &env,
                       /*stats_collector=*/nullptr, &connection_manager);

  NiceMock<MockFalcon> falcon;
  rdma.ConnectFalcon(&falcon);
  isekai::RNicConfig rnic_config =
      isekai::DefaultConfigGenerator::DefaultRNicConfig();
  auto hifs =
      std::make_unique<std::vector<std::unique_ptr<isekai::MemoryInterface>>>();
  for (const auto& host_intf_config : rnic_config.host_interface_config()) {
    auto hif =
        std::make_unique<isekai::MemoryInterface>(host_intf_config, &env);
    hifs->push_back(std::move(hif));
  }
  rdma.ConnectHostInterface(hifs.get());

  QpOptions qp_options;
  qp_options.dst_ip = "::1";

  std::pair<QpId, QpId> qp = make_pair(1, 100);
  rdma.CreateRcQp(qp.first, qp.second, qp_options,
                  RdmaConnectedMode::kOrderedRc);

  CompletionCallback completion_callback = [](Packet::Syndrome syndrome) {};

  // Post a write request from RDMA. This should be scheduled immediately by the
  // work scheduler.
  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(100), [&]() {
    rdma.PerformOp(/* qp_id = */ qp.first,
                   /* opcode = */ RdmaOpcode::kWrite,
                   /* sgl = */ {100}, /* is_inline = */ false,
                   completion_callback, kInvalidQpId);
  }));
  // Receive a read request from the network (Falcon), which generates a TX read
  // response, and it should be scheduled immediately as well.
  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(200), [&]() {
    auto packet = std::make_unique<Packet>();
    packet->rdma.opcode = Packet::Rdma::Opcode::kReadRequest;
    packet->rdma.sgl = {200};
    packet->rdma.dest_qp_id = qp.first;
    packet->rdma.rsn = 0;
    auto cookie = std::make_unique<OpaqueCookie>();
    rdma.HandleRxTransaction(std::move(packet), std::move(cookie));
  }));

  {
    InSequence seq;

    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->metadata.scid, qp_options.scid);
          EXPECT_EQ(p->rdma.opcode, Packet::Rdma::Opcode::kWriteOnly);
          EXPECT_THAT(p->rdma.sgl, ElementsAre(100));
          EXPECT_EQ(p->rdma.dest_qp_id, qp.second);
          EXPECT_EQ(p->rdma.rsn, 0);
          EXPECT_LE(env.ElapsedTime(),
                    absl::Nanoseconds(100 + config.chip_cycle_time_ns()));
        })
        .RetiresOnSaturation();

    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->metadata.scid, qp_options.scid);
          EXPECT_EQ(p->rdma.opcode, Packet::Rdma::Opcode::kReadResponseOnly);
          EXPECT_THAT(p->rdma.sgl, ElementsAre(200));
          EXPECT_EQ(p->rdma.dest_qp_id, qp.second);
          EXPECT_EQ(p->rdma.rsn, 0);
          EXPECT_LE(env.ElapsedTime(),
                    absl::Nanoseconds(200 + config.chip_cycle_time_ns()));
        })
        .RetiresOnSaturation();
  }

  EXPECT_OK(env.ScheduleEvent(absl::Microseconds(200), [&]() { env.Stop(); }));
  env.Run();
}

TEST(RdmaFalconModelTest, TestOrd) {
  SimpleEnvironment env;

  RdmaConfig config = DefaultConfigGenerator::DefaultRdmaConfig();
  config.set_outbound_read_queue_depth(4);
  FakeConnectionManager connection_manager;
  RdmaFalconModel rdma(config, &env,
                       /*stats_collector=*/nullptr, &connection_manager);

  NiceMock<MockFalcon> falcon;
  rdma.ConnectFalcon(&falcon);
  isekai::RNicConfig rnic_config =
      isekai::DefaultConfigGenerator::DefaultRNicConfig();
  auto hifs =
      std::make_unique<std::vector<std::unique_ptr<isekai::MemoryInterface>>>();
  for (const auto& host_intf_config : rnic_config.host_interface_config()) {
    auto hif =
        std::make_unique<isekai::MemoryInterface>(host_intf_config, &env);
    hifs->push_back(std::move(hif));
  }
  rdma.ConnectHostInterface(hifs.get());

  QpOptions qp_options;
  qp_options.dst_ip = "::1";

  std::pair<QpId, QpId> qp1 = make_pair(1, 100);
  rdma.CreateRcQp(qp1.first, qp1.second, qp_options,
                  RdmaConnectedMode::kOrderedRc);
  std::pair<QpId, QpId> qp2 = make_pair(2, 101);
  rdma.CreateRcQp(qp2.first, qp2.second, qp_options,
                  RdmaConnectedMode::kOrderedRc);

  CompletionCallback completion_callback = [](Packet::Syndrome syndrome) {};
  // With the quanta-based round-robin, QP 1's ops should be first scheduled,
  // as long as the request size does not exceed the work_scheduler_quanta.
  rdma.PerformOp(/* qp_id = */ qp1.first,
                 /* opcode = */ RdmaOpcode::kRead,
                 /* sgl = */ {32}, /* is_inline = */ false, completion_callback,
                 kInvalidQpId);
  rdma.PerformOp(/* qp_id = */ qp1.first,
                 /* opcode = */ RdmaOpcode::kRead,
                 /* sgl = */ {32}, /* is_inline = */ false, completion_callback,
                 kInvalidQpId);
  rdma.PerformOp(/* qp_id = */ qp1.first,
                 /* opcode = */ RdmaOpcode::kRead,
                 /* sgl = */ {32}, /* is_inline = */ false, completion_callback,
                 kInvalidQpId);
  rdma.PerformOp(/* qp_id = */ qp1.first,
                 /* opcode = */ RdmaOpcode::kRead,
                 /* sgl = */ {32}, /* is_inline = */ false, completion_callback,
                 kInvalidQpId);
  rdma.PerformOp(/* qp_id = */ qp1.first,
                 /* opcode = */ RdmaOpcode::kRead,
                 /* sgl = */ {32}, /* is_inline = */ false, completion_callback,
                 kInvalidQpId);
  rdma.PerformOp(/* qp_id = */ qp2.first,
                 /* opcode = */ RdmaOpcode::kRead,
                 /* sgl = */ {32}, /* is_inline = */ false, completion_callback,
                 kInvalidQpId);

  // Send a read response after a specific time.
  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(1000), [&]() {
    auto response = std::make_unique<Packet>();
    response->rdma.opcode = Packet::Rdma::Opcode::kReadResponseOnly;
    response->rdma.dest_qp_id = qp1.first;
    response->rdma.rsn = 0;
    auto cookie = std::make_unique<OpaqueCookie>();
    rdma.HandleRxTransaction(std::move(response), std::move(cookie));
  }));

  {
    InSequence seq;

    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.dest_qp_id, qp1.second);
          EXPECT_EQ(p->rdma.rsn, 0);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(10));
        })
        .RetiresOnSaturation();

    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.dest_qp_id, qp1.second);
          EXPECT_EQ(p->rdma.rsn, 1);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(20));
        })
        .RetiresOnSaturation();

    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.dest_qp_id, qp1.second);
          EXPECT_EQ(p->rdma.rsn, 2);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(30));
        })
        .RetiresOnSaturation();

    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.dest_qp_id, qp1.second);
          EXPECT_EQ(p->rdma.rsn, 3);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(40));
        })
        .RetiresOnSaturation();

    // ORD limit on qp1 should not impact
    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.dest_qp_id, qp2.second);
          EXPECT_EQ(p->rdma.rsn, 0);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(50));
        })
        .RetiresOnSaturation();

    // The last packet from qp1 should be sent to Falcon after a response
    // decrements ORD by 1 at 1000ns.
    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> p) {
          EXPECT_EQ(p->rdma.dest_qp_id, qp1.second);
          EXPECT_EQ(p->rdma.rsn, 4);
          EXPECT_GE(env.ElapsedTime(), absl::Nanoseconds(1000));
        })
        .RetiresOnSaturation();
  }

  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(1050), [&]() { env.Stop(); }));
  env.Run();
}

TEST(RdmaFalconModelTest, TestIrd) {
  constexpr int kScid1 = 1;
  constexpr int kScid2 = 2;

  SimpleEnvironment env;

  RdmaConfig config = DefaultConfigGenerator::DefaultRdmaConfig();
  config.set_inbound_read_queue_depth(2);
  // Set response tx credits to 0 to prevent requests from being sent out, and
  // build up the IRRQ.
  config.mutable_per_qp_credits()->set_tx_packet_data(0);
  FakeConnectionManager connection_manager;
  RdmaFalconModel rdma(config, &env,
                       /*stats_collector=*/nullptr, &connection_manager);

  NiceMock<MockFalcon> falcon;
  rdma.ConnectFalcon(&falcon);
  isekai::RNicConfig rnic_config =
      isekai::DefaultConfigGenerator::DefaultRNicConfig();
  auto hifs =
      std::make_unique<std::vector<std::unique_ptr<isekai::MemoryInterface>>>();
  for (const auto& host_intf_config : rnic_config.host_interface_config()) {
    auto hif =
        std::make_unique<isekai::MemoryInterface>(host_intf_config, &env);
    hifs->push_back(std::move(hif));
  }
  rdma.ConnectHostInterface(hifs.get());

  QpOptions qp_options;
  qp_options.dst_ip = "::1";
  qp_options.scid = kScid1;

  std::pair<QpId, QpId> qp1 = make_pair(1, 100);
  rdma.CreateRcQp(qp1.first, qp1.second, qp_options,
                  RdmaConnectedMode::kOrderedRc);

  qp_options.scid = kScid2;
  std::pair<QpId, QpId> qp2 = make_pair(2, 101);
  rdma.CreateRcQp(qp2.first, qp2.second, qp_options,
                  RdmaConnectedMode::kOrderedRc);

  CompletionCallback completion_callback = [](Packet::Syndrome syndrome) {};

  // Send multiple read requests on qp1.
  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(10), [&]() {
    auto request = std::make_unique<Packet>();
    request->rdma.opcode = Packet::Rdma::Opcode::kReadRequest;
    request->rdma.dest_qp_id = qp1.first;
    request->rdma.rsn = 0;
    auto cookie = std::make_unique<OpaqueCookie>();
    rdma.HandleRxTransaction(std::move(request), std::move(cookie));
  }));
  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(20), [&]() {
    auto request = std::make_unique<Packet>();
    request->rdma.opcode = Packet::Rdma::Opcode::kReadRequest;
    request->rdma.dest_qp_id = qp1.first;
    request->rdma.rsn = 1;
    auto cookie = std::make_unique<OpaqueCookie>();
    rdma.HandleRxTransaction(std::move(request), std::move(cookie));
  }));
  // This request should result in a NACK.
  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(30), [&]() {
    auto request = std::make_unique<Packet>();
    request->rdma.opcode = Packet::Rdma::Opcode::kReadRequest;
    request->rdma.dest_qp_id = qp1.first;
    request->rdma.rsn = 2;
    auto cookie = std::make_unique<OpaqueCookie>();
    rdma.HandleRxTransaction(std::move(request), std::move(cookie));
  }));
  // Request on another qp should not be blocked.
  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(40), [&]() {
    auto request = std::make_unique<Packet>();
    request->rdma.opcode = Packet::Rdma::Opcode::kReadRequest;
    request->rdma.dest_qp_id = qp2.first;
    request->rdma.rsn = 0;
    auto cookie = std::make_unique<OpaqueCookie>();
    rdma.HandleRxTransaction(std::move(request), std::move(cookie));
  }));

  {
    InSequence seq;

    EXPECT_CALL(falcon, AckTransaction(_, _, _, _, _))
        .WillOnce([&](uint32_t scid, uint32_t rsn, Packet::Syndrome ack_code,
                      absl::Duration timeout,
                      std::unique_ptr<OpaqueCookie> cookie) {
          EXPECT_EQ(scid, kScid1);
          EXPECT_EQ(rsn, 0);
          EXPECT_EQ(ack_code, Packet::Syndrome::kAck);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(10));
        })
        .RetiresOnSaturation();

    EXPECT_CALL(falcon, AckTransaction(_, _, _, _, _))
        .WillOnce([&](uint32_t scid, uint32_t rsn, Packet::Syndrome ack_code,
                      absl::Duration timeout,
                      std::unique_ptr<OpaqueCookie> cookie) {
          EXPECT_EQ(scid, kScid1);
          EXPECT_EQ(rsn, 1);
          EXPECT_EQ(ack_code, Packet::Syndrome::kAck);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(20));
        })
        .RetiresOnSaturation();

    EXPECT_CALL(falcon, AckTransaction(_, _, _, _, _))
        .WillOnce([&](uint32_t scid, uint32_t rsn, Packet::Syndrome ack_code,
                      absl::Duration timeout,
                      std::unique_ptr<OpaqueCookie> cookie) {
          EXPECT_EQ(scid, kScid1);
          EXPECT_EQ(rsn, 2);
          EXPECT_EQ(ack_code, Packet::Syndrome::kRnrNak);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(30));
        })
        .RetiresOnSaturation();

    EXPECT_CALL(falcon, AckTransaction(_, _, _, _, _))
        .WillOnce([&](uint32_t scid, uint32_t rsn, Packet::Syndrome ack_code,
                      absl::Duration timeout,
                      std::unique_ptr<OpaqueCookie> cookie) {
          EXPECT_EQ(scid, kScid2);
          EXPECT_EQ(rsn, 0);
          EXPECT_EQ(ack_code, Packet::Syndrome::kAck);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(40));
        })
        .RetiresOnSaturation();
  }

  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(100), [&]() { env.Stop(); }));
  env.Run();
}

TEST(RdmaFalconModelTest, TestPipelineDelayConst) {
  constexpr int kScid1 = 1;

  SimpleEnvironment env;

  MemoryInterfaceConfig hif_config =
      DefaultConfigGenerator::DefaultHostInterfaceConfig();
  hif_config.mutable_write_queue_config()
      ->set_memory_interface_queue_size_packets(UINT64_MAX);
  hif_config.mutable_write_queue_config()->set_delay_distribution(
      MemoryInterfaceConfig_MemoryDelayDistribution_CONST);
  hif_config.mutable_write_queue_config()->set_bandwidth_bps(4.70588235e11);
  hif_config.mutable_write_queue_config()->set_memory_delay_const_ns(0);
  std::vector<std::unique_ptr<MemoryInterface>> hifs;
  hifs.push_back(std::make_unique<MemoryInterface>(hif_config, &env));

  RdmaConfig config = DefaultConfigGenerator::DefaultRdmaConfig();
  // Set response tx credits to 0 to prevent requests from being sent out, and
  // build up the IRRQ.
  config.mutable_per_qp_credits()->set_tx_packet_data(0);
  FakeConnectionManager connection_manager;
  RdmaFalconModel rdma(config, &env,
                       /*stats_collector=*/nullptr, &connection_manager);

  NiceMock<MockFalcon> falcon;
  rdma.ConnectFalcon(&falcon);
  rdma.ConnectHostInterface(&hifs);

  QpOptions qp_options;
  qp_options.dst_ip = "::1";
  qp_options.scid = kScid1;

  std::pair<QpId, QpId> qp1 = make_pair(1, 100);
  rdma.CreateRcQp(qp1.first, qp1.second, qp_options,
                  RdmaConnectedMode::kOrderedRc);

  CompletionCallback completion_callback = [](Packet::Syndrome syndrome) {};

  int n_pkts = 100;
  std::vector<absl::Duration> completion_times;
  for (int i = 0; i < n_pkts; i++) {
    EXPECT_OK(env.ScheduleEvent(absl::ZeroDuration(), [&rdma, &qp1, i]() {
      auto request = std::make_unique<Packet>();
      request->rdma.opcode = Packet::Rdma::Opcode::kWriteOnly;
      request->rdma.dest_qp_id = qp1.first;
      request->rdma.rsn = i;
      request->falcon.payload_length = 1000;
      auto cookie = std::make_unique<OpaqueCookie>();
      rdma.HandleRxTransaction(std::move(request), std::move(cookie));
    }));
  }
  EXPECT_CALL(falcon, AckTransaction(_, _, _, _, _))
      .WillRepeatedly([&](uint32_t scid, uint32_t rsn,
                          Packet::Syndrome ack_code, absl::Duration timeout,
                          std::unique_ptr<OpaqueCookie> cookie) {
        EXPECT_EQ(scid, kScid1);
        EXPECT_EQ(ack_code, Packet::Syndrome::kAck);
        completion_times.push_back(env.ElapsedTime());
      })
      .RetiresOnSaturation();

  env.RunFor(absl::Nanoseconds(100000));
  EXPECT_EQ(completion_times.size(), n_pkts);

  std::vector<uint64_t> inter_completion_times;
  for (int i = 1; i < n_pkts; i++)
    inter_completion_times.push_back(absl::ToInt64Nanoseconds(
        completion_times[i] - completion_times[i - 1]));
  for (auto t : inter_completion_times)
    EXPECT_EQ(t, hif_config.write_queue_config().memory_delay_const_ns() +
                     config.chip_cycle_time_ns());
}

TEST(RdmaFalconModelTest, TestRxWriteAckDelay) {
  constexpr int kScid1 = 1;
  constexpr int kRequestAckDelayNs = 3000;  // 3us.
  constexpr int kResponseDelayNs = 3000;    // 3us.
  SimpleEnvironment env;
  MemoryInterfaceConfig hif_config =
      DefaultConfigGenerator::DefaultHostInterfaceConfig();
  hif_config.mutable_write_queue_config()->set_delay_distribution(
      MemoryInterfaceConfig_MemoryDelayDistribution_CONST);
  hif_config.mutable_write_queue_config()->set_bandwidth_bps(4.70588235e11);
  hif_config.mutable_write_queue_config()->set_memory_delay_const_ns(0);
  std::vector<std::unique_ptr<MemoryInterface>> hifs;
  hifs.push_back(std::make_unique<MemoryInterface>(hif_config, &env));

  RdmaConfig config = DefaultConfigGenerator::DefaultRdmaConfig();
  config.set_max_qp_oprate_million_per_sec(200);
  config.set_ack_nack_latency_ns(kRequestAckDelayNs);
  config.set_response_latency_ns(kResponseDelayNs);
  FakeConnectionManager connection_manager;
  RdmaFalconModel rdma(config, &env,
                       /*stats_collector=*/nullptr, &connection_manager);

  NiceMock<MockFalcon> falcon;
  rdma.ConnectFalcon(&falcon);
  rdma.ConnectHostInterface(&hifs);

  QpOptions qp_options;
  qp_options.dst_ip = "::1";
  qp_options.scid = kScid1;

  std::pair<QpId, QpId> qp1 = make_pair(1, 100);
  rdma.CreateRcQp(qp1.first, qp1.second, qp_options,
                  RdmaConnectedMode::kOrderedRc);

  // Send a request at t = 0.
  EXPECT_OK(env.ScheduleEvent(absl::ZeroDuration(), [&]() {
    auto request = std::make_unique<Packet>();
    request->rdma.opcode = Packet::Rdma::Opcode::kWriteOnly;
    request->rdma.dest_qp_id = qp1.first;
    request->rdma.rsn = 0;
    auto cookie = std::make_unique<OpaqueCookie>();
    rdma.HandleRxTransaction(std::move(request), std::move(cookie));
  }));

  {
    InSequence seq;

    // Request ACK should arrive at kRequestAckDelayNs.
    EXPECT_CALL(falcon, AckTransaction(_, _, _, _, _))
        .WillOnce([&](uint32_t scid, uint32_t rsn, Packet::Syndrome ack_code,
                      absl::Duration timeout,
                      std::unique_ptr<OpaqueCookie> cookie) {
          EXPECT_EQ(scid, kScid1);
          EXPECT_EQ(rsn, 0);
          EXPECT_EQ(ack_code, Packet::Syndrome::kAck);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(kRequestAckDelayNs));
        })
        .RetiresOnSaturation();
  }

  env.RunFor(absl::Nanoseconds(100000));
}

TEST(RdmaFalconModelTest, TestRxRequestAckResponseDelay) {
  constexpr int kScid1 = 1;
  constexpr int kRequestAckDelayNs = 3000;  // 3us.
  constexpr int kResponseDelayNs = 3000;    // 3us.
  SimpleEnvironment env;
  MemoryInterfaceConfig hif_config =
      DefaultConfigGenerator::DefaultHostInterfaceConfig();
  hif_config.mutable_write_queue_config()->set_delay_distribution(
      MemoryInterfaceConfig_MemoryDelayDistribution_CONST);
  hif_config.mutable_write_queue_config()->set_bandwidth_bps(4.70588235e11);
  hif_config.mutable_write_queue_config()->set_memory_delay_const_ns(0);
  std::vector<std::unique_ptr<MemoryInterface>> hifs;
  hifs.push_back(std::make_unique<MemoryInterface>(hif_config, &env));

  RdmaConfig config = DefaultConfigGenerator::DefaultRdmaConfig();
  config.set_max_qp_oprate_million_per_sec(200);
  config.set_ack_nack_latency_ns(kRequestAckDelayNs);
  config.set_response_latency_ns(kResponseDelayNs);
  FakeConnectionManager connection_manager;
  RdmaFalconModel rdma(config, &env,
                       /*stats_collector=*/nullptr, &connection_manager);

  NiceMock<MockFalcon> falcon;
  rdma.ConnectFalcon(&falcon);
  rdma.ConnectHostInterface(&hifs);

  QpOptions qp_options;
  qp_options.dst_ip = "::1";
  qp_options.scid = kScid1;

  std::pair<QpId, QpId> qp1 = make_pair(1, 100);
  rdma.CreateRcQp(qp1.first, qp1.second, qp_options,
                  RdmaConnectedMode::kOrderedRc);

  // Send 2 request back to back at the same time.
  EXPECT_OK(env.ScheduleEvent(absl::ZeroDuration(), [&]() {
    auto request = std::make_unique<Packet>();
    request->rdma.opcode = Packet::Rdma::Opcode::kReadRequest;
    request->rdma.dest_qp_id = qp1.first;
    request->rdma.rsn = 0;
    auto cookie = std::make_unique<OpaqueCookie>();
    rdma.HandleRxTransaction(std::move(request), std::move(cookie));
  }));
  EXPECT_OK(env.ScheduleEvent(absl::ZeroDuration(), [&]() {
    auto request = std::make_unique<Packet>();
    request->rdma.opcode = Packet::Rdma::Opcode::kReadRequest;
    request->rdma.dest_qp_id = qp1.first;
    request->rdma.rsn = 1;
    auto cookie = std::make_unique<OpaqueCookie>();
    rdma.HandleRxTransaction(std::move(request), std::move(cookie));
  }));

  {
    InSequence seq;

    // First request ACK should arrive at kRequestAckDelayNs.
    EXPECT_CALL(falcon, AckTransaction(_, _, _, _, _))
        .WillOnce([&](uint32_t scid, uint32_t rsn, Packet::Syndrome ack_code,
                      absl::Duration timeout,
                      std::unique_ptr<OpaqueCookie> cookie) {
          EXPECT_EQ(scid, kScid1);
          EXPECT_EQ(rsn, 0);
          EXPECT_EQ(ack_code, Packet::Syndrome::kAck);
          EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(kRequestAckDelayNs));
        })
        .RetiresOnSaturation();

    // Second request ACK should arrive at kRequestAckDelayNs + chip_cycle_time.
    EXPECT_CALL(falcon, AckTransaction(_, _, _, _, _))
        .WillOnce([&](uint32_t scid, uint32_t rsn, Packet::Syndrome ack_code,
                      absl::Duration timeout,
                      std::unique_ptr<OpaqueCookie> cookie) {
          EXPECT_EQ(scid, kScid1);
          EXPECT_EQ(rsn, 1);
          EXPECT_EQ(ack_code, Packet::Syndrome::kAck);
          EXPECT_EQ(env.ElapsedTime(),
                    absl::Nanoseconds(kRequestAckDelayNs +
                                      config.chip_cycle_time_ns()));
        })
        .RetiresOnSaturation();

    // First response should arrive kResponseDelayNs after first ACK.
    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> packet) {
          EXPECT_EQ(packet->rdma.opcode,
                    Packet::Rdma::Opcode::kReadResponseOnly);
          EXPECT_EQ(packet->rdma.rsn, 0);
          EXPECT_EQ(env.ElapsedTime(),
                    absl::Nanoseconds(kRequestAckDelayNs + kResponseDelayNs));
        })
        .RetiresOnSaturation();

    // Second response should arrive kResponseDelayNs after second ACK.
    EXPECT_CALL(falcon, InitiateTransaction(_))
        .WillOnce([&](std::unique_ptr<Packet> packet) {
          EXPECT_EQ(packet->rdma.opcode,
                    Packet::Rdma::Opcode::kReadResponseOnly);
          EXPECT_EQ(packet->rdma.rsn, 1);
          EXPECT_EQ(env.ElapsedTime(),
                    absl::Nanoseconds(kRequestAckDelayNs +
                                      config.chip_cycle_time_ns() +
                                      kResponseDelayNs));
        })
        .RetiresOnSaturation();
  }

  env.RunFor(absl::Nanoseconds(100000));
}

TEST(RdmaFalconModelTest, TestRxWriteReadAckOrdering) {
  constexpr int kScid1 = 1;
  constexpr int kRequestAckDelayNs = 3000;  // 3us.
  constexpr int kResponseDelayNs = 3000;    // 3us.
  SimpleEnvironment env;
  MemoryInterfaceConfig hif_config =
      DefaultConfigGenerator::DefaultHostInterfaceConfig();
  hif_config.mutable_write_queue_config()->set_delay_distribution(
      MemoryInterfaceConfig_MemoryDelayDistribution_CONST);
  // Set a very small BPS to slow down write request processing.
  hif_config.mutable_write_queue_config()->set_bandwidth_bps(32768);
  hif_config.mutable_write_queue_config()->set_memory_delay_const_ns(1000);
  std::vector<std::unique_ptr<MemoryInterface>> hifs;
  hifs.push_back(std::make_unique<MemoryInterface>(hif_config, &env));

  RdmaConfig config = DefaultConfigGenerator::DefaultRdmaConfig();
  config.set_ack_nack_latency_ns(kRequestAckDelayNs);
  config.set_response_latency_ns(kResponseDelayNs);
  FakeConnectionManager connection_manager;
  RdmaFalconModel rdma(config, &env,
                       /*stats_collector=*/nullptr, &connection_manager);

  NiceMock<MockFalcon> falcon;
  rdma.ConnectFalcon(&falcon);
  rdma.ConnectHostInterface(&hifs);

  QpOptions qp_options;
  qp_options.dst_ip = "::1";
  qp_options.scid = kScid1;

  std::pair<QpId, QpId> qp1 = make_pair(1, 100);
  rdma.CreateRcQp(qp1.first, qp1.second, qp_options,
                  RdmaConnectedMode::kOrderedRc);

  // Send 2 large write requests back to back at the same time, followed by a
  // read request.
  EXPECT_OK(env.ScheduleEvent(absl::ZeroDuration(), [&]() {
    auto request = std::make_unique<Packet>();
    request->rdma.opcode = Packet::Rdma::Opcode::kWriteOnly;
    request->rdma.dest_qp_id = qp1.first;
    request->rdma.rsn = 0;
    request->falcon.payload_length = 4096;
    auto cookie = std::make_unique<OpaqueCookie>();
    rdma.HandleRxTransaction(std::move(request), std::move(cookie));
  }));
  EXPECT_OK(env.ScheduleEvent(absl::ZeroDuration(), [&]() {
    auto request = std::make_unique<Packet>();
    request->rdma.opcode = Packet::Rdma::Opcode::kWriteOnly;
    request->rdma.dest_qp_id = qp1.first;
    request->rdma.rsn = 1;
    request->falcon.payload_length = 4096;
    auto cookie = std::make_unique<OpaqueCookie>();
    rdma.HandleRxTransaction(std::move(request), std::move(cookie));
  }));
  EXPECT_OK(env.ScheduleEvent(absl::ZeroDuration(), [&]() {
    auto request = std::make_unique<Packet>();
    request->rdma.opcode = Packet::Rdma::Opcode::kWriteOnly;
    request->rdma.dest_qp_id = qp1.first;
    request->rdma.rsn = 2;
    request->falcon.payload_length = 4096;
    auto cookie = std::make_unique<OpaqueCookie>();
    rdma.HandleRxTransaction(std::move(request), std::move(cookie));
  }));
  EXPECT_OK(env.ScheduleEvent(absl::ZeroDuration(), [&]() {
    auto request = std::make_unique<Packet>();
    request->rdma.opcode = Packet::Rdma::Opcode::kReadRequest;
    request->rdma.dest_qp_id = qp1.first;
    request->rdma.rsn = 3;
    auto cookie = std::make_unique<OpaqueCookie>();
    rdma.HandleRxTransaction(std::move(request), std::move(cookie));
  }));

  {
    InSequence seq;

    // Acks should arrive in RSN sequence.
    EXPECT_CALL(falcon, AckTransaction(_, _, _, _, _))
        .WillOnce([&](uint32_t scid, uint32_t rsn, Packet::Syndrome ack_code,
                      absl::Duration timeout,
                      std::unique_ptr<OpaqueCookie> cookie) {
          EXPECT_EQ(scid, kScid1);
          EXPECT_EQ(rsn, 0);
          EXPECT_EQ(ack_code, Packet::Syndrome::kAck);
        })
        .RetiresOnSaturation();

    EXPECT_CALL(falcon, AckTransaction(_, _, _, _, _))
        .WillOnce([&](uint32_t scid, uint32_t rsn, Packet::Syndrome ack_code,
                      absl::Duration timeout,
                      std::unique_ptr<OpaqueCookie> cookie) {
          EXPECT_EQ(scid, kScid1);
          EXPECT_EQ(rsn, 1);
          EXPECT_EQ(ack_code, Packet::Syndrome::kAck);
        })
        .RetiresOnSaturation();

    EXPECT_CALL(falcon, AckTransaction(_, _, _, _, _))
        .WillOnce([&](uint32_t scid, uint32_t rsn, Packet::Syndrome ack_code,
                      absl::Duration timeout,
                      std::unique_ptr<OpaqueCookie> cookie) {
          EXPECT_EQ(scid, kScid1);
          EXPECT_EQ(rsn, 2);
          EXPECT_EQ(ack_code, Packet::Syndrome::kAck);
        })
        .RetiresOnSaturation();

    EXPECT_CALL(falcon, AckTransaction(_, _, _, _, _))
        .WillOnce([&](uint32_t scid, uint32_t rsn, Packet::Syndrome ack_code,
                      absl::Duration timeout,
                      std::unique_ptr<OpaqueCookie> cookie) {
          EXPECT_EQ(scid, kScid1);
          EXPECT_EQ(rsn, 3);
          EXPECT_EQ(ack_code, Packet::Syndrome::kAck);
        })
        .RetiresOnSaturation();
  }

  env.RunFor(absl::Seconds(10));
}

// Tests that a Gen2OpaqueCookie enters and exits the RDMA model while keeping
// all its fields unchanged.
TEST(RdmaFalconModelTest, TestGen2OpaqueCookie) {
  SimpleEnvironment env;

  RdmaConfig config = DefaultConfigGenerator::DefaultRdmaConfig();
  FakeConnectionManager connection_manager;
  RdmaFalconModel rdma(config, &env, /*stats_collector=*/nullptr,
                       &connection_manager);

  NiceMock<MockFalcon> falcon;
  rdma.ConnectFalcon(&falcon);
  isekai::RNicConfig rnic_config =
      isekai::DefaultConfigGenerator::DefaultRNicConfig();
  auto hifs =
      std::make_unique<std::vector<std::unique_ptr<isekai::MemoryInterface>>>();
  for (const auto& host_intf_config : rnic_config.host_interface_config()) {
    auto hif =
        std::make_unique<isekai::MemoryInterface>(host_intf_config, &env);
    hifs->push_back(std::move(hif));
  }
  rdma.ConnectHostInterface(hifs.get());

  QpOptions qp_options;
  qp_options.dst_ip = "::1";

  std::pair<QpId, QpId> qp = make_pair(1, 100);
  rdma.CreateRcQp(qp.first, qp.second, qp_options,
                  RdmaConnectedMode::kOrderedRc);

  MockFunction<void(Packet::Syndrome syndrome)> mock_completion_callback;

  rdma.PerformOp(/* qp_id = */ qp.first, /* opcode = */ RdmaOpcode::kRecv,
                 /* sgl = */ {10}, /* is_inline = */ false,
                 mock_completion_callback.AsStdFunction(), kInvalidQpId);
  MockFunction<void(Packet::Syndrome sydrome)> mock_completion_callback2;

  rdma.PerformOp(/* qp_id = */ qp.first, /* opcode = */ RdmaOpcode::kRecv,
                 /* sgl = */ {5000}, /* is_inline = */ false,
                 mock_completion_callback2.AsStdFunction(), kInvalidQpId);

  // Gen2OpaqueCookie initialization as it enters the RDMA model from Falcon.
  uint8_t flow_id = 2;

  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(50), [&rdma, &qp, flow_id]() {
    auto packet = std::make_unique<Packet>();
    packet->rdma.opcode = Packet::Rdma::Opcode::kSendOnly;
    packet->rdma.dest_qp_id = qp.first;
    packet->rdma.rsn = 0;
    packet->rdma.inline_payload_length = 10;
    auto cookie = std::make_unique<Gen2OpaqueCookie>(flow_id);
    rdma.HandleRxTransaction(std::move(packet), std::move(cookie));
  }));

  EXPECT_CALL(falcon, AckTransaction(_, _, _, _, _))
      .WillOnce([&](uint32_t scid, uint32_t rsn, Packet::Syndrome ack_code,
                    absl::Duration timeout,
                    std::unique_ptr<OpaqueCookie> cookie) {
        // Make sure that the cookie retains its underlying Gen2OpaqueCookie
        // type and its initial values as it is sent back to the Falcon model.
        auto gen2_cookie = dynamic_cast<const Gen2OpaqueCookie*>(cookie.get());
        EXPECT_EQ(gen2_cookie->flow_id, flow_id);
      });
  EXPECT_CALL(mock_completion_callback, Call(Packet::Syndrome::kAck));

  EXPECT_OK(env.ScheduleEvent(absl::Nanoseconds(100), [&]() { env.Stop(); }));
  env.Run();
}

}  // namespace
}  // namespace isekai
