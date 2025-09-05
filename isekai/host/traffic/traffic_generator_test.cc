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

#include "isekai/host/traffic/traffic_generator.h"

#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/functional/any_invocable.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "glog/logging.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/default_config_generator.h"
#include "isekai/common/environment.h"
#include "isekai/common/file_util.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/common/simple_environment.h"
#include "isekai/common/testing.h"
#include "isekai/host/rdma/rdma_falcon_model.h"

namespace isekai {

namespace {

static constexpr char kConfigDataDir[] = "isekai/test_data/";

class FakeConnectionManager : public ConnectionManagerInterface {
 public:
  FakeConnectionManager() {
    // Set the VerifyFalconConnectionOptions callback to an empty callback.
    VerifyFalconConnectionOptions =
        [](const FalconConnectionOptions& connection_options) {};
  }
  absl::StatusOr<std::tuple<QpId, QpId>> CreateConnection(
      absl::string_view host_id1, absl::string_view host_id2,
      uint8_t source_bifurcation_id, uint8_t destination_bifurcation_id,
      QpOptions& qp_options, FalconConnectionOptions& connection_options,
      RdmaConnectedMode rc_mode) override {
    static uint32_t qp_id1 = 1;
    static uint32_t qp_id2 = 100;
    VerifyFalconConnectionOptions(connection_options);
    // Always create a unique QP.
    return std::make_tuple(qp_id1++, qp_id2++);
  }
  absl::StatusOr<std::tuple<QpId, QpId>> GetOrCreateBidirectionalConnection(
      absl::string_view host_id1, absl::string_view host_id2,
      uint8_t source_bifurcation_id, uint8_t destination_bifurcation_id,
      QpOptions& options, FalconConnectionOptions& connection_options,
      RdmaConnectedMode rc_mode) override {
    static uint32_t qp_id1 = 1;
    static uint32_t qp_id2 = 100;
    VerifyFalconConnectionOptions(connection_options);
    return std::make_tuple(qp_id1++, qp_id2++);
  }
  absl::StatusOr<std::tuple<QpId, QpId>> CreateUnidirectionalConnection(
      absl::string_view host_id1, absl::string_view host_id2,
      uint8_t source_bifurcation_id, uint8_t destination_bifurcation_id,
      QpOptions& options, FalconConnectionOptions& connection_options,
      RdmaConnectedMode rc_mode) override {
    static uint32_t qp_id1 = 1;
    static uint32_t qp_id2 = 100;
    VerifyFalconConnectionOptions(connection_options);
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
  // Callback function that allows a test to verify the
  // FalconConnectionOptions passed to the ConnectionManager when initializing a
  // new Falcon connection.
  absl::AnyInvocable<void(const FalconConnectionOptions& connection_options)>
      VerifyFalconConnectionOptions;
};

class FakeRdmaFalconModel : public RdmaFalconModel {
 public:
  FakeRdmaFalconModel(const RdmaConfig& config, Environment* env,
                      ConnectionManagerInterface* connection_manager)
      : RdmaFalconModel(config, env,
                        /*stats_collector=*/nullptr, connection_manager) {}

  // Initializes a QP in RC mode.
  void CreateRcQp(QpId local_qp_id, QpId remote_qp_id, QpOptions& options,
                  RdmaConnectedMode rc_mode) override {
    qp_mode_ = QpType::kRC;
    conn_type_ = rc_mode;
  }

  inline QpType GetQpMode() { return qp_mode_; }
  inline RdmaConnectedMode GetConnectionType() { return conn_type_; }

  void PerformOp(QpId qp_id, RdmaOpcode opcode, std::vector<uint32_t> sgl,
                 bool is_inline, CompletionCallback completion_callback,
                 QpId dest_qp_id) override {
    op_count_++;
    op_issue_time_.push_back(env_->ElapsedTime());
    // The op size is in bytes, and the generated load is in bits.
    double op_size = 0;
    for (const auto& n : sgl) {
      op_size += n;
      generated_load_ += n * 8;
    }

    collect_destination_statistics(dest_qp_id);
    collect_op_code_statistics(opcode);
    collect_op_size_statistics(op_size);
  }

  inline uint32_t GetOpCount() const { return op_count_; }

  inline double GetGenerateLoad() const { return generated_load_ / 1e9; }

  inline const std::vector<absl::Duration>& GetOpIssueTime() const {
    return op_issue_time_;
  }

  std::vector<std::pair<QpId, double>> GetDestinationDistribution() const {
    std::vector<std::pair<QpId, double>> res;
    for (auto& it : destination_qp_id_vector_) {
      res.push_back(std::make_pair(it.first, it.second * 1.0 / op_count_));
    }
    return res;
  }

  inline absl::flat_hash_map<RdmaOpcode, uint32_t> GetOpCodeDistribution()
      const {
    return op_code_vector_;
  }

  inline absl::flat_hash_map<double, uint32_t> GetOpSizeDistribution() const {
    return op_size_vector_;
  }

 private:
  void collect_destination_statistics(QpId dest_qp_id) {
    auto it = destination_qp_id_vector_.find(dest_qp_id);
    if (it != destination_qp_id_vector_.end()) {
      destination_qp_id_vector_[dest_qp_id]++;
    } else {
      destination_qp_id_vector_[dest_qp_id] = 1;
    }
  }

  void collect_op_code_statistics(RdmaOpcode opcode) {
    auto it = op_code_vector_.find(opcode);
    if (it != op_code_vector_.end()) {
      op_code_vector_[opcode]++;
    } else {
      op_code_vector_[opcode] = 1;
    }
  }

  void collect_op_size_statistics(double op_size) {
    auto it = op_size_vector_.find(op_size);
    if (it != op_size_vector_.end()) {
      op_size_vector_[op_size]++;
    } else {
      op_size_vector_[op_size] = 1;
    }
  }

  QpType qp_mode_;
  RdmaConnectedMode conn_type_;
  uint32_t op_count_ = 0;
  double generated_load_ = 0;
  std::map<QpId, uint32_t> destination_qp_id_vector_;
  absl::flat_hash_map<RdmaOpcode, uint32_t> op_code_vector_;
  std::vector<absl::Duration> op_issue_time_;
  absl::flat_hash_map<double, uint32_t> op_size_vector_;
};

}  // namespace

TEST(RdmaAppTest, DISABLED_TestCreateUdQp) {
  //
  int falcon_version = 1;
  SimpleEnvironment env;

  FakeConnectionManager connection_manager;
  FakeRdmaFalconModel rdma(DefaultConfigGenerator::DefaultRdmaConfig(), &env,
                           &connection_manager);

  SimulationConfig simulation;
  EXPECT_OK(ReadTextProtoFromFile(absl::StrCat(kConfigDataDir, "config.pb.txt"),
                                  &simulation));
  simulation.mutable_traffic_pattern()
      ->mutable_explicit_pattern()
      ->mutable_traffic_characteristics()
      ->set_conn_type(TrafficCharacteristics::UD);

  TrafficGenerator app(&connection_manager, &rdma, &env,
                       /*stats_collector=*/nullptr, "host1",
                       simulation.traffic_pattern(), falcon_version);

  ASSERT_EQ(app.traffic_pattern_info_.size(), falcon_version);
  ASSERT_EQ(app.get_traffic_characteristics(0).connection_type,
            TrafficCharacteristics::UD);
  ASSERT_EQ(true, simulation.traffic_pattern().has_explicit_pattern());
  ASSERT_EQ(TrafficPatternType::kExplicitPattern,
            app.get_traffic_pattern_type(0));
  ASSERT_EQ(QpType::kUD, rdma.GetQpMode());
}

TEST(RdmaAppTest, TestCreateRcUnorderedQp) {
  //
  int falcon_version = 1;
  SimpleEnvironment env;

  FakeConnectionManager connection_manager;
  FakeRdmaFalconModel rdma(DefaultConfigGenerator::DefaultRdmaConfig(), &env,
                           &connection_manager);

  SimulationConfig simulation;
  EXPECT_OK(ReadTextProtoFromFile(absl::StrCat(kConfigDataDir, "config.pb.txt"),
                                  &simulation));
  simulation.mutable_traffic_pattern()
      ->mutable_explicit_pattern()
      ->mutable_traffic_characteristics()
      ->set_conn_type(TrafficCharacteristics::UNORDERED_RC);

  TrafficGenerator app(&connection_manager, &rdma, &env,
                       /*stats_collector=*/nullptr, "host1",
                       simulation.traffic_pattern(), falcon_version);

  ASSERT_EQ(app.traffic_pattern_info_.size(), falcon_version);
  ASSERT_EQ(app.get_traffic_characteristics(0).connection_type,
            TrafficCharacteristics::UNORDERED_RC);
  ASSERT_EQ(true, simulation.traffic_pattern().has_explicit_pattern());
}

TEST(RdmaAppTest, TestCreateRcOrderedQp) {
  //
  int falcon_version = 1;
  SimpleEnvironment env;

  FakeConnectionManager connection_manager;
  FakeRdmaFalconModel rdma(DefaultConfigGenerator::DefaultRdmaConfig(), &env,
                           &connection_manager);

  SimulationConfig simulation;
  EXPECT_OK(ReadTextProtoFromFile(absl::StrCat(kConfigDataDir, "config.pb.txt"),
                                  &simulation));
  simulation.mutable_traffic_pattern()
      ->mutable_explicit_pattern()
      ->mutable_traffic_characteristics()
      ->set_conn_type(TrafficCharacteristics::ORDERED_RC);

  TrafficGenerator app(&connection_manager, &rdma, &env,
                       /*stats_collector=*/nullptr, "host1",
                       simulation.traffic_pattern(), falcon_version);

  ASSERT_EQ(app.traffic_pattern_info_.size(), falcon_version);
  ASSERT_EQ(app.get_traffic_characteristics(0).connection_type,
            TrafficCharacteristics::ORDERED_RC);
  ASSERT_EQ(true, simulation.traffic_pattern().has_explicit_pattern());
}

TEST(RdmaAppTestWithParameters, TestGenerateCustomOpSizeDist) {
  //
  int falcon_version = 1;
  SimpleEnvironment env;

  FakeConnectionManager connection_manager;
  FakeRdmaFalconModel rdma(DefaultConfigGenerator::DefaultRdmaConfig(), &env,
                           &connection_manager);

  SimulationConfig simulation;
  EXPECT_OK(ReadTextProtoFromFile(absl::StrCat(kConfigDataDir, "config.pb.txt"),
                                  &simulation));
  simulation.mutable_traffic_pattern()
      ->mutable_explicit_pattern()
      ->mutable_traffic_characteristics()
      ->mutable_conn_config(0)
      ->set_msg_size_distribution(TrafficCharacteristics::CUSTOM_SIZE_DIST);
  std::vector<double> test_quantiles = {4096, 4096,  4096,  8192,
                                        8192, 16384, 65536, 262144};
  absl::flat_hash_map<uint32_t, double> ground_truth_probabilities = {
      {4096, 0.375},
      {8192, 0.25},
      {16384, 0.125},
      {65536, 0.125},
      {262144, 0.125}};
  for (auto quantile : test_quantiles) {
    simulation.mutable_traffic_pattern()
        ->mutable_explicit_pattern()
        ->mutable_traffic_characteristics()
        ->mutable_conn_config(0)
        ->mutable_size_distribution_params()
        ->mutable_cdf()
        ->add_quantiles(quantile);
  }

  TrafficGenerator app(&connection_manager, &rdma, &env,
                       /*stats_collector=*/nullptr, "host1",
                       simulation.traffic_pattern(), falcon_version);

  auto* parsed_traffic_pattern =
      &std::get<TrafficGenerator::ExplicitPatternInfo>(
          app.traffic_pattern_info_[0]);

  absl::flat_hash_map<uint32_t, uint64_t> generated_op_size_count;
  uint64_t total_count = 1000000;
  for (auto i = 0; i < total_count; i++) {
    auto size = app.GetMessageSize(parsed_traffic_pattern);
    if (!generated_op_size_count.contains(size)) {
      generated_op_size_count[size] = 1;
    } else {
      generated_op_size_count[size]++;
    }
  }

  for (auto [size, probability] : ground_truth_probabilities) {
    EXPECT_NEAR(generated_op_size_count[size] * 1.0 / total_count, probability,
                0.001);
  }
}

namespace {

TEST(RdmaAppTest, TestGenerateRcOp) {
  //
  int falcon_version = 1;
  SimpleEnvironment env;

  FakeConnectionManager connection_manager;
  FakeRdmaFalconModel rdma(DefaultConfigGenerator::DefaultRdmaConfig(), &env,
                           &connection_manager);

  SimulationConfig simulation;
  EXPECT_OK(ReadTextProtoFromFile(absl::StrCat(kConfigDataDir, "config.pb.txt"),
                                  &simulation));
  simulation.mutable_traffic_pattern()
      ->mutable_explicit_pattern()
      ->mutable_traffic_characteristics()
      ->set_conn_type(TrafficCharacteristics::ORDERED_RC);

  TrafficGenerator app(&connection_manager, &rdma, &env,
                       /*stats_collector=*/nullptr, "host1",
                       simulation.traffic_pattern(), falcon_version);
  app.GenerateRdmaOp();

  EXPECT_OK(env.ScheduleEvent(absl::Milliseconds(1), [&]() { env.Stop(); }));
  env.Run();

  std::vector<std::pair<QpId, double>> destination_distribution =
      rdma.GetDestinationDistribution();
  ASSERT_EQ(1, destination_distribution.size());
  ASSERT_EQ(1, destination_distribution[0].second);
}

TEST(RdmaAppTest, DISABLED_TestGenerateUdOp) {
  //
  int falcon_version = 1;
  SimpleEnvironment env;

  FakeConnectionManager connection_manager;
  FakeRdmaFalconModel rdma(DefaultConfigGenerator::DefaultRdmaConfig(), &env,
                           &connection_manager);

  SimulationConfig simulation;
  EXPECT_OK(ReadTextProtoFromFile(absl::StrCat(kConfigDataDir, "config.pb.txt"),
                                  &simulation));
  simulation.mutable_traffic_pattern()
      ->mutable_explicit_pattern()
      ->mutable_traffic_characteristics()
      ->set_conn_type(TrafficCharacteristics::UD);

  TrafficGenerator app(&connection_manager, &rdma, &env,
                       /*stats_collector=*/nullptr, "host1",
                       simulation.traffic_pattern(), falcon_version);
  app.GenerateRdmaOp();

  EXPECT_OK(env.ScheduleEvent(absl::Milliseconds(1), [&]() { env.Stop(); }));
  env.Run();

  std::vector<std::pair<QpId, double>> destination_distribution =
      rdma.GetDestinationDistribution();
  ASSERT_EQ(1, destination_distribution.size());
  ASSERT_EQ(1, destination_distribution[0].second);
}

TEST(RdmaAppTest, TestReceiver) {
  //
  int falcon_version = 1;
  SimpleEnvironment env;

  FakeConnectionManager connection_manager;
  FakeRdmaFalconModel rdma(DefaultConfigGenerator::DefaultRdmaConfig(), &env,
                           &connection_manager);

  SimulationConfig simulation;
  EXPECT_OK(ReadTextProtoFromFile(absl::StrCat(kConfigDataDir, "config.pb.txt"),
                                  &simulation));

  TrafficGenerator app(&connection_manager, &rdma, &env,
                       /*stats_collector=*/nullptr, "host2",
                       simulation.traffic_pattern(), falcon_version);
  app.GenerateRdmaOp();

  absl::Duration duration = absl::Seconds(0.01);
  EXPECT_OK(env.ScheduleEvent(duration, [&]() { env.Stop(); }));
  env.Run();

  // Receiver should only create qp but not proactively send out traffic.
  ASSERT_EQ(0, rdma.GetOpCount());
}

TEST(RdmaAppTest, TestGenerateUniformLoad) {
  //
  int falcon_version = 1;
  SimpleEnvironment env;

  FakeConnectionManager connection_manager;
  FakeRdmaFalconModel rdma(DefaultConfigGenerator::DefaultRdmaConfig(), &env,
                           &connection_manager);

  SimulationConfig simulation;
  EXPECT_OK(ReadTextProtoFromFile(absl::StrCat(kConfigDataDir, "config.pb.txt"),
                                  &simulation));
  simulation.mutable_traffic_pattern()
      ->mutable_explicit_pattern()
      ->mutable_traffic_characteristics()
      ->mutable_conn_config(0)
      ->set_msg_size_distribution(TrafficCharacteristics::UNIFORM_SIZE_DIST);
  simulation.mutable_traffic_pattern()
      ->mutable_explicit_pattern()
      ->mutable_traffic_characteristics()
      ->mutable_conn_config(0)
      ->mutable_size_distribution_params()
      ->set_min(100);
  simulation.mutable_traffic_pattern()
      ->mutable_explicit_pattern()
      ->mutable_traffic_characteristics()
      ->mutable_conn_config(0)
      ->mutable_size_distribution_params()
      ->set_max(1900);
  simulation.mutable_traffic_pattern()
      ->mutable_explicit_pattern()
      ->mutable_traffic_characteristics()
      ->mutable_conn_config(0)
      ->mutable_size_distribution_params()
      ->set_mean(1000);

  TrafficGenerator app(&connection_manager, &rdma, &env,
                       /*stats_collector=*/nullptr, "host1",
                       simulation.traffic_pattern(), falcon_version);
  app.GenerateRdmaOp();

  absl::Duration duration = absl::Seconds(0.01);
  EXPECT_OK(env.ScheduleEvent(duration, [&]() { env.Stop(); }));
  env.Run();

  ASSERT_NEAR(rdma.GetGenerateLoad() / absl::ToDoubleSeconds(duration),
              simulation.traffic_pattern()
                  .explicit_pattern()
                  .traffic_characteristics()
                  .offered_load(),
              0.1);
}

TEST(RdmaAppTest, TestGenerateFixedLoad) {
  //
  int falcon_version = 1;
  SimpleEnvironment env;

  FakeConnectionManager connection_manager;
  FakeRdmaFalconModel rdma(DefaultConfigGenerator::DefaultRdmaConfig(), &env,
                           &connection_manager);

  SimulationConfig simulation;
  EXPECT_OK(ReadTextProtoFromFile(absl::StrCat(kConfigDataDir, "config.pb.txt"),
                                  &simulation));
  simulation.mutable_traffic_pattern()
      ->mutable_explicit_pattern()
      ->mutable_traffic_characteristics()
      ->mutable_conn_config(0)
      ->set_msg_size_distribution(TrafficCharacteristics::FIXED_SIZE);

  TrafficGenerator app(&connection_manager, &rdma, &env,
                       /*stats_collector=*/nullptr, "host1",
                       simulation.traffic_pattern(), falcon_version);
  app.GenerateRdmaOp();

  absl::Duration duration = absl::Seconds(0.01);
  EXPECT_OK(env.ScheduleEvent(duration, [&]() { env.Stop(); }));
  env.Run();

  ASSERT_NEAR(rdma.GetGenerateLoad() / absl::ToDoubleSeconds(duration),
              simulation.traffic_pattern()
                  .explicit_pattern()
                  .traffic_characteristics()
                  .offered_load(),
              0.1);
}

TEST(RdmaAppTest, TestGenerateNormalLoad) {
  //
  int falcon_version = 1;
  SimpleEnvironment env;

  FakeConnectionManager connection_manager;
  FakeRdmaFalconModel rdma(DefaultConfigGenerator::DefaultRdmaConfig(), &env,
                           &connection_manager);

  SimulationConfig simulation;
  EXPECT_OK(ReadTextProtoFromFile(absl::StrCat(kConfigDataDir, "config.pb.txt"),
                                  &simulation));
  simulation.mutable_traffic_pattern()
      ->mutable_explicit_pattern()
      ->mutable_traffic_characteristics()
      ->mutable_conn_config(0)
      ->set_msg_size_distribution(TrafficCharacteristics::NORMAL_SIZE_DIST);
  simulation.mutable_traffic_pattern()
      ->mutable_explicit_pattern()
      ->mutable_traffic_characteristics()
      ->mutable_conn_config(0)
      ->mutable_size_distribution_params()
      ->set_mean(5000);
  simulation.mutable_traffic_pattern()
      ->mutable_explicit_pattern()
      ->mutable_traffic_characteristics()
      ->mutable_conn_config(0)
      ->mutable_size_distribution_params()
      ->set_stddev(1000);

  TrafficGenerator app(&connection_manager, &rdma, &env,
                       /*stats_collector=*/nullptr, "host1",
                       simulation.traffic_pattern(), falcon_version);
  app.GenerateRdmaOp();

  absl::Duration duration = absl::Seconds(0.01);
  EXPECT_OK(env.ScheduleEvent(duration, [&]() { env.Stop(); }));
  env.Run();

  ASSERT_NEAR(rdma.GetGenerateLoad() / absl::ToDoubleSeconds(duration),
              simulation.traffic_pattern()
                  .explicit_pattern()
                  .traffic_characteristics()
                  .offered_load(),
              0.1);
}

TEST(RdmaAppTest, TestPoissonArrival) {
  //
  int falcon_version = 1;
  SimpleEnvironment env;

  FakeConnectionManager connection_manager;
  FakeRdmaFalconModel rdma(DefaultConfigGenerator::DefaultRdmaConfig(), &env,
                           &connection_manager);

  SimulationConfig simulation;
  EXPECT_OK(ReadTextProtoFromFile(absl::StrCat(kConfigDataDir, "config.pb.txt"),
                                  &simulation));
  simulation.mutable_traffic_pattern()
      ->mutable_explicit_pattern()
      ->mutable_traffic_characteristics()
      ->mutable_conn_config(0)
      ->set_arrival_time_distribution(TrafficCharacteristics::POISSON_PROCESS);

  TrafficGenerator app(&connection_manager, &rdma, &env,
                       /*stats_collector=*/nullptr, "host1",
                       simulation.traffic_pattern(), falcon_version);
  app.GenerateRdmaOp();

  absl::Duration duration = absl::Seconds(0.01);
  EXPECT_OK(env.ScheduleEvent(duration, [&]() { env.Stop(); }));
  env.Run();

  ASSERT_NEAR(rdma.GetGenerateLoad() / absl::ToDoubleSeconds(duration),
              simulation.traffic_pattern()
                  .explicit_pattern()
                  .traffic_characteristics()
                  .offered_load(),
              0.1);
}

TEST(RdmaAppTest, TestRcRandomOpCode) {
  //
  int falcon_version = 1;
  SimpleEnvironment env;

  FakeConnectionManager connection_manager;
  FakeRdmaFalconModel rdma(DefaultConfigGenerator::DefaultRdmaConfig(), &env,
                           &connection_manager);

  SimulationConfig simulation;
  EXPECT_OK(ReadTextProtoFromFile(absl::StrCat(kConfigDataDir, "config.pb.txt"),
                                  &simulation));
  simulation.mutable_traffic_pattern()
      ->mutable_explicit_pattern()
      ->mutable_traffic_characteristics()
      ->set_conn_type(TrafficCharacteristics::ORDERED_RC);
  simulation.mutable_traffic_pattern()
      ->mutable_explicit_pattern()
      ->mutable_traffic_characteristics()
      ->mutable_conn_config(0)
      ->set_op_code(TrafficCharacteristics::RANDOM_OP);
  auto op_config = simulation.mutable_traffic_pattern()
                       ->mutable_explicit_pattern()
                       ->mutable_traffic_characteristics()
                       ->mutable_conn_config(0)
                       ->mutable_op_config();
  op_config->set_write_ratio(0.4);
  op_config->set_read_ratio(0.3);
  op_config->set_send_ratio(0.2);
  op_config->set_recv_ratio(0.1);

  TrafficGenerator app(&connection_manager, &rdma, &env,
                       /*stats_collector=*/nullptr, "host1",
                       simulation.traffic_pattern(), falcon_version);
  app.GenerateRdmaOp();

  absl::Duration duration = absl::Seconds(0.01);
  EXPECT_OK(env.ScheduleEvent(duration, [&]() { env.Stop(); }));
  env.Run();

  absl::flat_hash_map<RdmaOpcode, uint32_t> op_statistics =
      rdma.GetOpCodeDistribution();
  ASSERT_EQ(op_statistics.size(), 4);
  ASSERT_NEAR(op_statistics[RdmaOpcode::kRead] * 1.0 / rdma.GetOpCount(),
              op_config->read_ratio(), 0.05);
  ASSERT_NEAR(op_statistics[RdmaOpcode::kRecv] * 1.0 / rdma.GetOpCount(),
              op_config->recv_ratio(), 0.05);
  ASSERT_NEAR(op_statistics[RdmaOpcode::kSend] * 1.0 / rdma.GetOpCount(),
              op_config->send_ratio(), 0.05);
  ASSERT_NEAR(op_statistics[RdmaOpcode::kWrite] * 1.0 / rdma.GetOpCount(),
              op_config->write_ratio(), 0.05);
}

TEST(RdmaAppTestWithParameters, TestMixedReadAndWriteOpCode) {
  //
  int falcon_version = 1;
  SimpleEnvironment env;

  FakeConnectionManager connection_manager;
  FakeRdmaFalconModel rdma(DefaultConfigGenerator::DefaultRdmaConfig(), &env,
                           &connection_manager);

  SimulationConfig simulation;
  EXPECT_OK(ReadTextProtoFromFile(absl::StrCat(kConfigDataDir, "config.pb.txt"),
                                  &simulation));

  // Makes the config have 50% READ and 50% WRITE.
  simulation.mutable_traffic_pattern()
      ->mutable_explicit_pattern()
      ->mutable_traffic_characteristics()
      ->mutable_conn_config(0)
      ->set_offered_load_ratio(0.5);
  auto new_connection_config = simulation.mutable_traffic_pattern()
                                   ->mutable_explicit_pattern()
                                   ->mutable_traffic_characteristics()
                                   ->add_conn_config();
  *new_connection_config = simulation.traffic_pattern()
                               .explicit_pattern()
                               .traffic_characteristics()
                               .conn_config(0);
  new_connection_config->set_op_code(TrafficCharacteristics::WRITE);

  TrafficGenerator app(&connection_manager, &rdma, &env,
                       /*stats_collector=*/nullptr, "host1",
                       simulation.traffic_pattern(), falcon_version);
  app.GenerateRdmaOp();

  absl::Duration duration = absl::Seconds(0.01);
  EXPECT_OK(env.ScheduleEvent(duration, [&]() { env.Stop(); }));
  env.Run();

  absl::flat_hash_map<RdmaOpcode, uint32_t> op_statistics =
      rdma.GetOpCodeDistribution();
  ASSERT_EQ(op_statistics.size(), 2);
  ASSERT_NEAR(op_statistics[RdmaOpcode::kRead] * 1.0 / rdma.GetOpCount(), 0.5,
              0.05);
  ASSERT_NEAR(op_statistics[RdmaOpcode::kWrite] * 1.0 / rdma.GetOpCount(), 0.5,
              0.05);
}

TEST(RdmaAppTest, TestMixedOpSize) {
  //
  int falcon_version = 1;
  SimpleEnvironment env;

  FakeConnectionManager connection_manager;
  FakeRdmaFalconModel rdma(DefaultConfigGenerator::DefaultRdmaConfig(), &env,
                           &connection_manager);

  SimulationConfig simulation;
  EXPECT_OK(ReadTextProtoFromFile(
      absl::StrCat(kConfigDataDir, "config_mixed_op_size.pb.txt"),
      &simulation));

  TrafficGenerator app(&connection_manager, &rdma, &env,
                       /*stats_collector=*/nullptr, "host1",
                       simulation.traffic_pattern(), falcon_version);
  app.GenerateRdmaOp();

  absl::Duration duration = absl::Seconds(0.01);
  EXPECT_OK(env.ScheduleEvent(duration, [&]() { env.Stop(); }));
  env.Run();

  absl::flat_hash_map<double, uint32_t> op_statistics =
      rdma.GetOpSizeDistribution();
  // In the config, we have the offered load for op size 10, 100, 1000, and
  // 10000 are 1Gbps, 2Gbps, 3Gbps, and 4Gbps, respectively. Moreover, the
  // arrival_time_distribution is Poisson process.
  ASSERT_EQ(op_statistics.size(), 4);
  int size_10_op_count = 1e9 / 8 * 0.01 / 10;
  ASSERT_NEAR(op_statistics[10], size_10_op_count, size_10_op_count * 0.1);
  int size_100_op_count = 2e9 / 8 * 0.01 / 100;
  ASSERT_NEAR(op_statistics[100], size_100_op_count, size_100_op_count * 0.1);
  int size_1000_op_count = 3e9 / 8 * 0.01 / 1000;
  ASSERT_NEAR(op_statistics[1000], size_1000_op_count,
              size_1000_op_count * 0.1);
  int size_10000_op_count = 4e9 / 8 * 0.01 / 10000;
  ASSERT_NEAR(op_statistics[10000], size_10000_op_count,
              size_10000_op_count * 0.1);
}

TEST(RdmaAppTest, TestMixedOpSizeAndOpCode) {
  //
  int falcon_version = 1;
  SimpleEnvironment env;

  FakeConnectionManager connection_manager;
  FakeRdmaFalconModel rdma(DefaultConfigGenerator::DefaultRdmaConfig(), &env,
                           &connection_manager);

  // In the traffic pattern, 50% READ and 50% WRITE. The op size for READ is
  // 2KB, while the op size for WRITE is 128KB.
  SimulationConfig simulation;
  EXPECT_OK(ReadTextProtoFromFile(
      absl::StrCat(kConfigDataDir, "config_mixed_op_size_and_op_code.pb.txt"),
      &simulation));

  TrafficGenerator app(&connection_manager, &rdma, &env,
                       /*stats_collector=*/nullptr, "host1",
                       simulation.traffic_pattern(), falcon_version);
  app.GenerateRdmaOp();

  absl::Duration duration = absl::Seconds(0.01);
  EXPECT_OK(env.ScheduleEvent(duration, [&]() { env.Stop(); }));
  env.Run();

  absl::flat_hash_map<double, uint32_t> op_size_statistics =
      rdma.GetOpSizeDistribution();
  ASSERT_EQ(op_size_statistics.size(), 2);

  absl::flat_hash_map<RdmaOpcode, uint32_t> op_code_statistics =
      rdma.GetOpCodeDistribution();
  ASSERT_EQ(op_code_statistics.size(), 2);

  ASSERT_EQ(op_size_statistics[2000], op_code_statistics[RdmaOpcode::kRead]);
  ASSERT_EQ(op_size_statistics[128000], op_code_statistics[RdmaOpcode::kWrite]);

  // Since 50% load is READ with 2KB op size, 50% load is WRITE with 128KB op
  // size, the number of ops for READ should be 64 times larger than that for
  // WRITE.
  ASSERT_NEAR(op_size_statistics[2000] / op_size_statistics[128000], 64, 1);
}

TEST(RdmaAppTest, DISABLED_TestUdRandomOpCode) {
  //
  int falcon_version = 1;
  SimpleEnvironment env;

  FakeConnectionManager connection_manager;
  FakeRdmaFalconModel rdma(DefaultConfigGenerator::DefaultRdmaConfig(), &env,
                           &connection_manager);

  SimulationConfig simulation;
  EXPECT_OK(ReadTextProtoFromFile(absl::StrCat(kConfigDataDir, "config.pb.txt"),
                                  &simulation));
  simulation.mutable_traffic_pattern()
      ->mutable_explicit_pattern()
      ->mutable_traffic_characteristics()
      ->set_conn_type(TrafficCharacteristics::UD);
  simulation.mutable_traffic_pattern()
      ->mutable_explicit_pattern()
      ->mutable_traffic_characteristics()
      ->mutable_conn_config(0)
      ->set_op_code(TrafficCharacteristics::RANDOM_OP);

  TrafficGenerator app(&connection_manager, &rdma, &env,
                       /*stats_collector=*/nullptr, "host1",
                       simulation.traffic_pattern(), falcon_version);
  app.GenerateRdmaOp();

  absl::Duration duration = absl::Seconds(0.01);
  EXPECT_OK(env.ScheduleEvent(duration, [&]() { env.Stop(); }));
  env.Run();

  absl::flat_hash_map<RdmaOpcode, uint32_t> op_statistics =
      rdma.GetOpCodeDistribution();
  ASSERT_EQ(op_statistics.size(), 2);
  ASSERT_NEAR(op_statistics[RdmaOpcode::kRecv] * 1.0 / rdma.GetOpCount(),
              1.0 / 2, 0.05);
  ASSERT_NEAR(op_statistics[RdmaOpcode::kSend] * 1.0 / rdma.GetOpCount(),
              1.0 / 2, 0.05);
}

TEST(RdmaAppTest, TestUniformRandom) {
  //
  int falcon_version = 1;
  SimpleEnvironment env;

  FakeConnectionManager connection_manager;
  FakeRdmaFalconModel rdma(DefaultConfigGenerator::DefaultRdmaConfig(), &env,
                           &connection_manager);

  SimulationConfig simulation;
  EXPECT_OK(ReadTextProtoFromFile(
      absl::StrCat(kConfigDataDir, "config_uniform_random.pb.txt"),
      &simulation));

  TrafficGenerator app(&connection_manager, &rdma, &env,
                       /*stats_collector=*/nullptr, "host1",
                       simulation.traffic_pattern(), falcon_version);
  app.GenerateRdmaOp();

  absl::Duration duration = absl::Seconds(0.01);
  EXPECT_OK(env.ScheduleEvent(duration, [&]() { env.Stop(); }));
  env.Run();

  std::vector<std::pair<QpId, double>> destination_distribution =
      rdma.GetDestinationDistribution();
  ASSERT_EQ(simulation.network().hosts_size() - 1,
            destination_distribution.size());
  for (auto& it : destination_distribution) {
    ASSERT_NEAR(1.0 / destination_distribution.size(), it.second, 0.1);
  }
  ASSERT_NEAR(rdma.GetGenerateLoad() / absl::ToDoubleSeconds(duration),
              simulation.traffic_pattern()
                  .uniform_random()
                  .traffic_characteristics()
                  .offered_load(),
              0.1);
}

TEST(RdmaAppTest, TestFixedIncastWrite) {
  //
  int falcon_version = 1;
  SimpleEnvironment env;
  FakeConnectionManager connection_manager;

  FakeRdmaFalconModel rdma1(DefaultConfigGenerator::DefaultRdmaConfig(), &env,
                            &connection_manager);

  FakeRdmaFalconModel rdma2(DefaultConfigGenerator::DefaultRdmaConfig(), &env,
                            &connection_manager);

  FakeRdmaFalconModel rdma3(DefaultConfigGenerator::DefaultRdmaConfig(), &env,
                            &connection_manager);

  FakeRdmaFalconModel rdma4(DefaultConfigGenerator::DefaultRdmaConfig(), &env,
                            &connection_manager);

  SimulationConfig simulation;
  EXPECT_OK(ReadTextProtoFromFile(
      absl::StrCat(kConfigDataDir, "config_incast.pb.txt"), &simulation));

  TrafficGenerator app1(&connection_manager, &rdma1, &env,
                        /*stats_collector=*/nullptr, "host1",
                        simulation.traffic_pattern(), falcon_version);
  app1.GenerateRdmaOp();
  TrafficGenerator app2(&connection_manager, &rdma2, &env,
                        /*stats_collector=*/nullptr, "host2",
                        simulation.traffic_pattern(), falcon_version);
  app2.GenerateRdmaOp();
  TrafficGenerator app3(&connection_manager, &rdma3, &env,
                        /*stats_collector=*/nullptr, "host3",
                        simulation.traffic_pattern(), falcon_version);
  app3.GenerateRdmaOp();
  TrafficGenerator app4(&connection_manager, &rdma4, &env,
                        /*stats_collector=*/nullptr, "host4",
                        simulation.traffic_pattern(), falcon_version);
  app4.GenerateRdmaOp();

  constexpr double kSimulationDurationSeconds = 0.01;
  absl::Duration duration = absl::Seconds(kSimulationDurationSeconds);
  EXPECT_OK(env.ScheduleEvent(duration, [&]() { env.Stop(); }));
  env.Run();

  std::vector<std::pair<QpId, double>> destination_distribution1 =
      rdma1.GetDestinationDistribution();
  auto op_count1 = rdma1.GetOpCount();
  ASSERT_EQ(1, destination_distribution1.size());
  ASSERT_EQ(1, destination_distribution1[0].second);

  std::vector<std::pair<QpId, double>> destination_distribution2 =
      rdma2.GetDestinationDistribution();
  auto op_count2 = rdma1.GetOpCount();
  ASSERT_EQ(1, destination_distribution2.size());
  ASSERT_EQ(1, destination_distribution2[0].second);

  std::vector<std::pair<QpId, double>> destination_distribution3 =
      rdma3.GetDestinationDistribution();
  auto op_count3 = rdma1.GetOpCount();
  ASSERT_EQ(1, destination_distribution3.size());
  ASSERT_EQ(1, destination_distribution3[0].second);

  ASSERT_EQ(0, rdma4.GetOpCount());
  auto total_op_count = op_count1 + op_count2 + op_count3;
  // We have in total 3 Initiator candidates. Each candidate should have the
  // same chance to be the sender over the time. Therefore, each candidate
  // should issue the same number of ops.
  ASSERT_NEAR(1.0 / 3, op_count1 * 1.0 / total_op_count, 0.05);
  ASSERT_NEAR(1.0 / 3, op_count2 * 1.0 / total_op_count, 0.05);
  ASSERT_NEAR(1.0 / 3, op_count3 * 1.0 / total_op_count, 0.05);

  // Total op count should be equal to the expected total op count from config.
  double expected_total_op_count =
      (simulation.traffic_pattern()
           .incast()
           .traffic_characteristics()
           .offered_load() *
       1e9 / 8) *
      simulation.traffic_pattern().incast().incast_degree() *
      kSimulationDurationSeconds /
      simulation.traffic_pattern()
          .incast()
          .traffic_characteristics()
          .conn_config(0)
          .size_distribution_params()
          .mean();

  ASSERT_NEAR(static_cast<double>(total_op_count) / expected_total_op_count,
              1.0, 0.05);
}

TEST(RdmaAppTest, TestFixedIncastRead) {
  //
  int falcon_version = 1;
  SimpleEnvironment env;
  FakeConnectionManager connection_manager;
  FakeRdmaFalconModel rdma(DefaultConfigGenerator::DefaultRdmaConfig(), &env,
                           &connection_manager);

  SimulationConfig simulation;
  EXPECT_OK(ReadTextProtoFromFile(
      absl::StrCat(kConfigDataDir, "config_incast.pb.txt"), &simulation));
  simulation.mutable_traffic_pattern()
      ->mutable_incast()
      ->mutable_traffic_characteristics()
      ->mutable_conn_config(0)
      ->set_op_code(TrafficCharacteristics::READ);

  // In this fixed incast read test, there are 3 senders (host1, host2 and
  // host3), and host4 is the victim (i.e., the initiator). The incast degree is
  // 2, meaning that each time, host4 will randomly select 2 out of 3 senders to
  // send packets to itself.
  TrafficGenerator app(&connection_manager, &rdma, &env,
                       /*stats_collector=*/nullptr, "host4",
                       simulation.traffic_pattern(), falcon_version);
  app.GenerateRdmaOp();

  absl::Duration duration = absl::Seconds(0.01);
  EXPECT_OK(env.ScheduleEvent(duration, [&]() { env.Stop(); }));
  env.Run();

  std::vector<std::pair<QpId, double>> destination_distribution =
      rdma.GetDestinationDistribution();
  // Each of the 3 senders has the same chance to be selected to perform incast.
  EXPECT_EQ(destination_distribution.size(), 3);
  for (const auto& dist : destination_distribution) {
    ASSERT_NEAR(1.0 / 3, dist.second, 0.05);
  }
}

TEST(RdmaAppTest, TestMultiQps) {
  //
  int falcon_version = 1;
  SimpleEnvironment env;

  FakeConnectionManager connection_manager;
  FakeRdmaFalconModel rdma(DefaultConfigGenerator::DefaultRdmaConfig(), &env,
                           &connection_manager);

  SimulationConfig simulation;
  EXPECT_OK(ReadTextProtoFromFile(absl::StrCat(kConfigDataDir, "config.pb.txt"),
                                  &simulation));

  TrafficPatternConfig_ExplicitPattern_Flow* new_flow;
  new_flow = simulation.mutable_traffic_pattern()
                 ->mutable_explicit_pattern()
                 ->add_flows();
  new_flow->set_initiator_host_id("host1_0");
  new_flow->set_target_host_id("host2_0");

  new_flow = simulation.mutable_traffic_pattern()
                 ->mutable_explicit_pattern()
                 ->add_flows();
  new_flow->set_initiator_host_id("host1_0");
  new_flow->set_target_host_id("host3_0");

  simulation.mutable_traffic_pattern()
      ->mutable_explicit_pattern()
      ->mutable_traffic_characteristics()
      ->mutable_conn_config(0)
      ->set_arrival_time_distribution(TrafficCharacteristics::POISSON_PROCESS);

  TrafficGenerator app(&connection_manager, &rdma, &env,
                       /*stats_collector=*/nullptr, "host1",
                       simulation.traffic_pattern(), falcon_version);
  app.GenerateRdmaOp();

  absl::Duration duration = absl::Seconds(0.01);
  EXPECT_OK(env.ScheduleEvent(duration, [&]() { env.Stop(); }));
  env.Run();

  std::vector<std::pair<QpId, double>> destination_distribution =
      rdma.GetDestinationDistribution();
  ASSERT_EQ(simulation.traffic_pattern().explicit_pattern().flows_size(),
            destination_distribution.size());
  for (auto& it : destination_distribution) {
    ASSERT_DOUBLE_EQ(1.0 / destination_distribution.size(), it.second);
  }
  ASSERT_NEAR(rdma.GetGenerateLoad() / absl::ToDoubleSeconds(duration),
              simulation.traffic_pattern()
                  .explicit_pattern()
                  .traffic_characteristics()
                  .offered_load(),
              0.1);
}

// We have in total 4 hosts with incast degree = 3.
TEST(RdmaAppTest, TestRevolvingIncastWrite) {
  //
  int falcon_version = 1;
  SimpleEnvironment env;
  FakeConnectionManager connection_manager;

  FakeRdmaFalconModel rdma1(DefaultConfigGenerator::DefaultRdmaConfig(), &env,
                            &connection_manager);

  FakeRdmaFalconModel rdma2(DefaultConfigGenerator::DefaultRdmaConfig(), &env,
                            &connection_manager);

  FakeRdmaFalconModel rdma3(DefaultConfigGenerator::DefaultRdmaConfig(), &env,
                            &connection_manager);

  FakeRdmaFalconModel rdma4(DefaultConfigGenerator::DefaultRdmaConfig(), &env,
                            &connection_manager);

  SimulationConfig simulation;
  EXPECT_OK(ReadTextProtoFromFile(
      absl::StrCat(kConfigDataDir, "config_revolving_incast.pb.txt"),
      &simulation));

  TrafficGenerator app1(&connection_manager, &rdma1, &env,
                        /*stats_collector=*/nullptr, "host1",
                        simulation.traffic_pattern(), falcon_version);
  app1.GenerateRdmaOp();
  TrafficGenerator app2(&connection_manager, &rdma2, &env,
                        /*stats_collector=*/nullptr, "host2",
                        simulation.traffic_pattern(), falcon_version);
  app2.GenerateRdmaOp();
  TrafficGenerator app3(&connection_manager, &rdma3, &env,
                        /*stats_collector=*/nullptr, "host3",
                        simulation.traffic_pattern(), falcon_version);
  app3.GenerateRdmaOp();
  TrafficGenerator app4(&connection_manager, &rdma4, &env,
                        /*stats_collector=*/nullptr, "host4",
                        simulation.traffic_pattern(), falcon_version);
  app4.GenerateRdmaOp();

  absl::Duration duration = absl::Seconds(0.02);
  EXPECT_OK(env.ScheduleEvent(duration, [&]() { env.Stop(); }));
  env.Run();

  auto destination_distribution1 = rdma1.GetDestinationDistribution();
  // The other 3 hosts should have equal chance to be selected as the incast
  // victim.
  ASSERT_EQ(3, destination_distribution1.size());
  for (const auto& dist : destination_distribution1) {
    ASSERT_NEAR(1.0 / 3, dist.second, 0.05);
  }

  auto destination_distribution2 = rdma2.GetDestinationDistribution();
  ASSERT_EQ(3, destination_distribution2.size());
  for (const auto& dist : destination_distribution2) {
    ASSERT_NEAR(1.0 / 3, dist.second, 0.05);
  }

  auto destination_distribution3 = rdma3.GetDestinationDistribution();
  ASSERT_EQ(3, destination_distribution3.size());
  for (const auto& dist : destination_distribution3) {
    ASSERT_NEAR(1.0 / 3, dist.second, 0.05);
  }

  auto destination_distribution4 = rdma4.GetDestinationDistribution();
  ASSERT_EQ(3, destination_distribution4.size());
  for (const auto& dist : destination_distribution4) {
    ASSERT_NEAR(1.0 / 3, dist.second, 0.05);
  }
}

TEST(RdmaAppTest, TestRevolvingIncastRead) {
  //
  int falcon_version = 1;
  SimpleEnvironment env;
  FakeConnectionManager connection_manager;

  FakeRdmaFalconModel rdma1(DefaultConfigGenerator::DefaultRdmaConfig(), &env,
                            &connection_manager);

  FakeRdmaFalconModel rdma2(DefaultConfigGenerator::DefaultRdmaConfig(), &env,
                            &connection_manager);

  FakeRdmaFalconModel rdma3(DefaultConfigGenerator::DefaultRdmaConfig(), &env,
                            &connection_manager);

  FakeRdmaFalconModel rdma4(DefaultConfigGenerator::DefaultRdmaConfig(), &env,
                            &connection_manager);

  SimulationConfig simulation;
  EXPECT_OK(ReadTextProtoFromFile(
      absl::StrCat(kConfigDataDir, "config_revolving_incast.pb.txt"),
      &simulation));
  simulation.mutable_traffic_pattern()
      ->mutable_incast()
      ->mutable_traffic_characteristics()
      ->mutable_conn_config(0)
      ->set_op_code(TrafficCharacteristics::READ);
  simulation.mutable_traffic_pattern()->mutable_incast()->set_incast_degree(2);

  TrafficGenerator app1(&connection_manager, &rdma1, &env,
                        /*stats_collector=*/nullptr, "host1",
                        simulation.traffic_pattern(), falcon_version);
  app1.GenerateRdmaOp();
  TrafficGenerator app2(&connection_manager, &rdma2, &env,
                        /*stats_collector=*/nullptr, "host2",
                        simulation.traffic_pattern(), falcon_version);
  app2.GenerateRdmaOp();
  TrafficGenerator app3(&connection_manager, &rdma3, &env,
                        /*stats_collector=*/nullptr, "host3",
                        simulation.traffic_pattern(), falcon_version);
  app3.GenerateRdmaOp();
  TrafficGenerator app4(&connection_manager, &rdma4, &env,
                        /*stats_collector=*/nullptr, "host4",
                        simulation.traffic_pattern(), falcon_version);
  app4.GenerateRdmaOp();

  absl::Duration duration = absl::Seconds(0.01);
  EXPECT_OK(env.ScheduleEvent(duration, [&]() { env.Stop(); }));
  env.Run();

  // There are 4 hosts in the revolving incast read with incast degree 2. At
  // each time, each host has the equal chance to be selected as the initiator
  // (i.e., the victim) who randomly selects 2 out of the rest 3 hosts as the
  // senders to perform incast.
  unsigned int total_op_count = 0;
  auto destination_distribution1 = rdma1.GetDestinationDistribution();
  auto op_count1 = rdma1.GetOpCount();
  total_op_count += op_count1;
  // The other 3 hosts should have equal chance to be selected as the incast
  // senders.
  ASSERT_EQ(3, destination_distribution1.size());
  for (const auto& dist : destination_distribution1) {
    ASSERT_NEAR(1.0 / 3, dist.second, 0.05);
  }

  auto destination_distribution2 = rdma2.GetDestinationDistribution();
  auto op_count2 = rdma2.GetOpCount();
  total_op_count += op_count2;
  ASSERT_EQ(3, destination_distribution2.size());
  for (const auto& dist : destination_distribution2) {
    ASSERT_NEAR(1.0 / 3, dist.second, 0.05);
  }

  auto destination_distribution3 = rdma3.GetDestinationDistribution();
  auto op_count3 = rdma3.GetOpCount();
  total_op_count += op_count3;
  ASSERT_EQ(3, destination_distribution3.size());
  for (const auto& dist : destination_distribution3) {
    ASSERT_NEAR(1.0 / 3, dist.second, 0.05);
  }

  auto destination_distribution4 = rdma4.GetDestinationDistribution();
  auto op_count4 = rdma4.GetOpCount();
  total_op_count += op_count4;
  ASSERT_EQ(3, destination_distribution4.size());
  for (const auto& dist : destination_distribution4) {
    ASSERT_NEAR(1.0 / 3, dist.second, 0.05);
  }

  // Each host has the same chance to be selected as the initiator.
  ASSERT_NEAR(1.0 / 4, op_count1 * 1.0 / total_op_count, 0.05);
  ASSERT_NEAR(1.0 / 4, op_count2 * 1.0 / total_op_count, 0.05);
  ASSERT_NEAR(1.0 / 4, op_count3 * 1.0 / total_op_count, 0.05);
  ASSERT_NEAR(1.0 / 4, op_count4 * 1.0 / total_op_count, 0.05);
}

TEST(RdmaAppTest, TestCompositePattern) {
  //
  int falcon_version = 1;
  SimpleEnvironment env;
  FakeConnectionManager connection_manager;
  SimulationConfig simulation;
  EXPECT_OK(ReadTextProtoFromFile(
      absl::StrCat(kConfigDataDir, "config_composite.pb.txt"), &simulation));
  int host_num = simulation.network().hosts_size();

  std::vector<FakeRdmaFalconModel> rdmas;
  for (int i = 0; i < host_num; i++) {
    rdmas.emplace_back(DefaultConfigGenerator::DefaultRdmaConfig(), &env,
                       &connection_manager);
  }

  std::vector<TrafficGenerator> apps;
  std::vector<std::string> hosts_name(host_num);
  for (int i = 0; i < host_num; i++) {
    hosts_name[i] = absl::StrCat("host", i + 1);
    apps.emplace_back(&connection_manager, &rdmas[i], &env,
                      /*stats_collector=*/nullptr, hosts_name[i],
                      simulation.traffic_pattern(), falcon_version);
  }
  for (int i = 0; i < host_num; i++) {
    apps[i].GenerateRdmaOp();
  }

  absl::Duration duration = absl::Seconds(0.01);
  EXPECT_OK(env.ScheduleEvent(duration, [&]() { env.Stop(); }));
  env.Run();

  // host_info stores the number of destinations and issued op count of each
  // host.
  std::vector<std::pair<unsigned int, unsigned int>> host_info(host_num);
  for (int i = 0; i < host_num; i++) {
    host_info[i].first = rdmas[i].GetDestinationDistribution().size();
    host_info[i].second = rdmas[i].GetOpCount();
  }

  // All hosts are in uniform random. Therefore, the number of desinations of
  // each host is host_num - 1.
  // host1 - host5 are in fixed incast (incast degree = 3), where host5 is the
  // victim. Therefore, the number of destinations of host1 - host4 should be
  // host_num - 1 + 1. In addition, host1 - host4 should issue about the same
  // amount of ops.
  unsigned int total_op_count = 0;
  for (int i = 0; i < 4; i++) {
    ASSERT_EQ(host_num - 1 + 1, host_info[i].first);
    total_op_count += host_info[i].second;
  }
  for (int i = 0; i < 4; i++) {
    ASSERT_NEAR(1.0 / 4, host_info[i].second * 1.0 / total_op_count, 0.05);
  }
  // host6 - host10 are in revolving incast (incast degree = 3). Therefore,
  // the number of destinations of host6 - host10 should be host_num - 1 + 4. In
  // addition, host6 - host10 should issue about the same amount of ops.
  total_op_count = 0;
  for (int i = 5; i < 10; i++) {
    ASSERT_EQ(host_num - 1 + 4, host_info[i].first);
    total_op_count += host_info[i].second;
  }
  for (int i = 5; i < 10; i++) {
    ASSERT_NEAR(1.0 / 5, host_info[i].second * 1.0 / total_op_count, 0.05);
  }
  // host11 - host12 are in explicit pattern (host11 sends packets to host12).
  // Therefore, the number of destinations of host11 should be host_num - 1 + 1.
  // In addition, the number of ops issued by host11 should be twice as large as
  // that of host12.
  ASSERT_EQ(host_num - 1 + 1, host_info[10].first);
  ASSERT_EQ(host_info[10].second, host_info[11].second * 2);
  // host11 should issue more ops that host1 - host4, since each time only 3 out
  // of 4 hosts from host1 - host4 are selected as the initiators. Moreover,
  // host1 and host4 should issue more ops than host5 - host10. Although the
  // incast degree is 3 in bot cases, the number of initiator candidats is 4 in
  // the fixed incast, while that value is 5 in the revolving incast. Therefore,
  // intiator candidates in fixed incast have higher chance to send packets than
  // those in revolving incast.
  ASSERT_GT(host_info[10].second, host_info[0].second);
  ASSERT_GT(host_info[0].second, host_info[5].second);
}

TEST(RdmaAppTest, FirstOpIssueTime) {
  //
  int falcon_version = 1;
  SimpleEnvironment env;
  FakeConnectionManager connection_manager;
  FakeRdmaFalconModel rdma1(DefaultConfigGenerator::DefaultRdmaConfig(), &env,
                            &connection_manager);
  FakeRdmaFalconModel rdma2(DefaultConfigGenerator::DefaultRdmaConfig(), &env,
                            &connection_manager);

  SimulationConfig simulation;
  EXPECT_OK(ReadTextProtoFromFile(
      absl::StrCat(kConfigDataDir, "config_uniform_random.pb.txt"),
      &simulation));
  // Given that the arrival process is poisson and only generates 1 op, we test
  // if the generated time of the op is different for different traffic
  // generator.
  simulation.mutable_traffic_pattern()
      ->mutable_uniform_random()
      ->mutable_traffic_characteristics()
      ->mutable_conn_config(0)
      ->set_max_generated_packets(1);
  simulation.mutable_traffic_pattern()
      ->mutable_uniform_random()
      ->mutable_traffic_characteristics()
      ->mutable_conn_config(0)
      ->set_arrival_time_distribution(TrafficCharacteristics::POISSON_PROCESS);

  TrafficGenerator app1(&connection_manager, &rdma1, &env,
                        /*stats_collector=*/nullptr, "host1",
                        simulation.traffic_pattern(), falcon_version);
  app1.GenerateRdmaOp();
  TrafficGenerator app2(&connection_manager, &rdma2, &env,
                        /*stats_collector=*/nullptr, "host2",
                        simulation.traffic_pattern(), falcon_version);
  app2.GenerateRdmaOp();

  absl::Duration duration = absl::Seconds(0.01);
  EXPECT_OK(env.ScheduleEvent(duration, [&]() { env.Stop(); }));
  env.Run();

  auto op_issue_time_1 = rdma1.GetOpIssueTime().at(0);
  auto op_issue_time_2 = rdma2.GetOpIssueTime().at(0);
  EXPECT_NE(op_issue_time_1, op_issue_time_2);
}

}  // namespace

}  // namespace isekai
