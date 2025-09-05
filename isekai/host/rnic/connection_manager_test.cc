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

#include "isekai/host/rnic/connection_manager.h"

#include <stddef.h>

#include <cstdint>
#include <tuple>
#include <type_traits>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "glog/logging.h"
#include "gmock/gmock.h"
#include "google/protobuf/text_format.h"
#include "gtest/gtest.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/default_config_generator.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/common/simple_environment.h"
#include "isekai/common/testing.h"  // IWYU pragma: keep
#include "isekai/fabric/constants.h"
#include "isekai/host/falcon/falcon_connection_state.h"
#include "isekai/host/falcon/gen1/falcon_model.h"
#include "isekai/host/falcon/gen2/falcon_model.h"
#include "isekai/host/rdma/rdma_falcon_model.h"

namespace isekai {

namespace {

// Text proto for NetworkConfig for test.
constexpr char kHostConfig[] = R"(
  hosts {
    id: "host_1"
    ip_address: "fc00:0100:500:18f::1"
  }
  hosts {
    id: "host_2"
    ip_address: "fc00:0100:500:823::1"
  }
  hosts {
    id: "host_3"
    ip_address: "fc00:0100:500:1e9::1"
  }

  connection_routing_configs {
    static_routing_rule_entries {
      src_host_id: "host_1"
      dst_host_id: "host_2"
      path_static_routing_port_list {
        forward_routing_port_list: "[1, 2, 3, 4]"
        reverse_routing_port_list: "[5, 6, 7, 8]"
      }
      path_static_routing_port_list {
        forward_routing_port_list: "[9, 10, 11, 12]"
        reverse_routing_port_list: "[13, 14 ,15, 16]"
      }
      path_static_routing_port_list {
        forward_routing_port_list: "[13, 14 ,15, 16]"
        reverse_routing_port_list: "[9, 10, 11, 12]"
      }
      path_static_routing_port_list {
        forward_routing_port_list: "[1, 2, 3, 4]"
        reverse_routing_port_list: "[5, 6, 7, 8]"
      }
      path_static_routing_port_list {
        forward_routing_port_list: "[1, 2, 3, 4]"
        reverse_routing_port_list: "[5, 6, 7, 8]"
      }
      path_static_routing_port_list {
        forward_routing_port_list: "[9, 10, 11, 12]"
        reverse_routing_port_list: "[13, 14 ,15, 16]"
      }
      path_static_routing_port_list {
        forward_routing_port_list: "[13, 14 ,15, 16]"
        reverse_routing_port_list: "[9, 10, 11, 12]"
      }
      path_static_routing_port_list {
        forward_routing_port_list: "[1, 2, 3, 4]"
        reverse_routing_port_list: "[5, 6, 7, 8]"
      }
    }
    static_routing_rule_entries {
      src_host_id: "host_2"
      dst_host_id: "host_3"
      path_static_routing_port_list {
        forward_routing_port_list: "[1, 2, 3, 4]"
        reverse_routing_port_list: "[5, 6, 7, 8]"
      }
      path_static_routing_port_list {
        forward_routing_port_list: "[9, 10, 11, 12]"
        reverse_routing_port_list: "[13, 14 ,15, 16]"
      }
      path_static_routing_port_list {
        forward_routing_port_list: "[13, 14 ,15, 16]"
        reverse_routing_port_list: "[9, 10, 11, 12]"
      }
      path_static_routing_port_list {
        forward_routing_port_list: "[1, 2, 3, 4]"
        reverse_routing_port_list: "[5, 6, 7, 8]"
      }
    }
  }
)";

constexpr uint32_t kFalconVersion1 = 1;
constexpr uint32_t kFalconVersion2 = 2;
constexpr uint32_t kFalconVersion3 = 3;

// Number of hosts for test.
constexpr size_t kNumHosts = 3;

// IDs of the hosts for test.
constexpr absl::string_view kHostIds[] = {"host_1", "host_2", "host_3"};

// Prepares the ConnectionManager, RdmaFalconModel, FalconModel for test.
void PrepareModules(ConnectionManager* connection_manager,
                    SimpleEnvironment* env,
                    std::vector<RdmaFalconModel*>* rdma_vector,
                    std::vector<FalconModel*>* falcon_vector,
                    uint32_t falcon_version) {
  RdmaConfig rdma_config = DefaultConfigGenerator::DefaultRdmaConfig();
  FalconConfig falcon_config =
      DefaultConfigGenerator::DefaultFalconConfig(falcon_version);
  for (size_t i = 0; i < kNumHosts; i++) {
    RdmaFalconModel* rdma =
        new RdmaFalconModel(rdma_config, env,
                            /*stats_collector=*/nullptr, connection_manager);
    FalconModel* falcon;
    switch (falcon_version) {
      case kFalconVersion1:
        falcon =
            new FalconModel(falcon_config, env, /*stats_collector=*/nullptr,
                            connection_manager, "falcon-host",
                            /* number of hosts */ 4);
        break;
      case kFalconVersion2:
        falcon =
            new Gen2FalconModel(falcon_config, env, /*stats_collector=*/nullptr,
                                connection_manager, "falcon-host",
                                /* number of hosts */ 4);
        break;
      default:
        LOG(FATAL) << "Unknown falcon_version: " << i;
    }
    rdma->ConnectFalcon(falcon);
    connection_manager->RegisterRdma(kHostIds[i], rdma);
    connection_manager->RegisterFalcon(kHostIds[i], falcon);
    rdma_vector->push_back(rdma);
    falcon_vector->push_back(falcon);
  }
  NetworkConfig simulation_network;
  CHECK(google::protobuf::TextFormat::ParseFromString(kHostConfig,
                                                      &simulation_network))
      << "fail to parse proto string.";
  connection_manager->PopulateHostInfo(simulation_network);
}

}  // namespace

// Test for method PopulateHostInfo in class ConnectionManager.
TEST(ConnectionManagerTest, PopulateHostInfo) {
  ConnectionManager connection_manager;
  NetworkConfig simulation_network;
  CHECK(google::protobuf::TextFormat::ParseFromString(kHostConfig,
                                                      &simulation_network))
      << "fail to parse proto string.";
  connection_manager.PopulateHostInfo(simulation_network);

  EXPECT_EQ(connection_manager.host_id_to_host_info_.size(), 3);
  EXPECT_TRUE(connection_manager.host_id_to_host_info_.contains("host_1"));
  EXPECT_EQ(connection_manager.host_id_to_host_info_["host_1"].ip_address,
            "fc00:0100:500:18f::1");
  EXPECT_TRUE(connection_manager.host_id_to_host_info_.contains("host_2"));
  EXPECT_EQ(connection_manager.host_id_to_host_info_["host_2"].ip_address,
            "fc00:0100:500:823::1");
  EXPECT_TRUE(connection_manager.host_id_to_host_info_.contains("host_3"));
  EXPECT_EQ(connection_manager.host_id_to_host_info_["host_3"].ip_address,
            "fc00:0100:500:1e9::1");
}

// Test for method PopulateHostInfo in class ConnectionManager.
TEST(ConnectionManagerTest, PopulateStaticRouteInfo) {
  ConnectionManager connection_manager;
  NetworkConfig simulation_network;
  CHECK(google::protobuf::TextFormat::ParseFromString(kHostConfig,
                                                      &simulation_network))
      << "fail to parse proto string.";
  connection_manager.PopulateHostInfo(simulation_network);

  EXPECT_EQ(connection_manager.host_id_to_host_info_["host_1"].dst_host_id,
            "host_2");
  // Tests if the static routing lists equal to the one specified in the
  // configuration file.
  std::vector<std::vector<uint32_t>> expected_forward_static_port_list1 = {
      {1, 2, 3, 4}, {9, 10, 11, 12}, {13, 14, 15, 16}, {1, 2, 3, 4},
      {1, 2, 3, 4}, {9, 10, 11, 12}, {13, 14, 15, 16}, {1, 2, 3, 4}};
  std::vector<std::vector<uint32_t>> expected_reverse_static_port_list1 = {
      {5, 6, 7, 8}, {13, 14, 15, 16}, {9, 10, 11, 12}, {5, 6, 7, 8},
      {5, 6, 7, 8}, {13, 14, 15, 16}, {9, 10, 11, 12}, {5, 6, 7, 8}};
  EXPECT_THAT(
      connection_manager.host_id_to_host_info_["host_1"]
          .forward_static_routing_port_lists.value(),
      testing::UnorderedElementsAreArray(expected_forward_static_port_list1));
  EXPECT_THAT(
      connection_manager.host_id_to_host_info_["host_1"]
          .reverse_static_routing_port_lists.value(),
      testing::UnorderedElementsAreArray(expected_reverse_static_port_list1));

  EXPECT_EQ(connection_manager.host_id_to_host_info_["host_2"].dst_host_id,
            "host_3");
  // Tests if the static routing lists equal to the one specified in the
  // configuration file.
  std::vector<std::vector<uint32_t>> expected_forward_static_port_list2 = {
      /*Path0=*/{1, 2, 3, 4},
      /*Path1=*/{9, 10, 11, 12},
      /*Path2=*/{13, 14, 15, 16},
      /*Path3=*/{1, 2, 3, 4}};
  std::vector<std::vector<uint32_t>> expected_reverse_static_port_list2 = {
      /*Path0=*/{5, 6, 7, 8},
      /*Path1=*/{13, 14, 15, 16},
      /*Path2=*/{9, 10, 11, 12},
      /*Path3=*/{5, 6, 7, 8}};
  EXPECT_THAT(
      connection_manager.host_id_to_host_info_["host_2"]
          .forward_static_routing_port_lists.value(),
      testing::UnorderedElementsAreArray(expected_forward_static_port_list2));
  EXPECT_THAT(
      connection_manager.host_id_to_host_info_["host_2"]
          .reverse_static_routing_port_lists.value(),
      testing::UnorderedElementsAreArray(expected_reverse_static_port_list2));
}

// Test for method CreateFalconConnection in class ConnectionManager.
TEST(ConnectionManagerTest, CreateFalconConnection) {
  std::vector<RdmaFalconModel*> rdma_vector;
  std::vector<FalconModel*> falcon_vector;
  ConnectionManager connection_manager;
  SimpleEnvironment env;
  FalconConnectionOptions connection_options;

  PrepareModules(&connection_manager, &env, &rdma_vector, &falcon_vector,
                 kFalconVersion1);

  uint32_t local_cid, remote_cid;
  ASSERT_OK_AND_ASSIGN(std::tie(local_cid, remote_cid),
                       connection_manager.CreateFalconConnection(
                           "host_1", "host_2", 0, 0, connection_options,
                           RdmaConnectedMode::kOrderedRc));
  EXPECT_EQ(local_cid, 0);
  EXPECT_EQ(remote_cid, 0);

  ASSERT_OK_AND_ASSIGN(std::tie(local_cid, remote_cid),
                       connection_manager.CreateFalconConnection(
                           "host_1", "host_3", 0, 0, connection_options,
                           RdmaConnectedMode::kOrderedRc));
  EXPECT_EQ(local_cid, 1);
  EXPECT_EQ(remote_cid, 0);

  for (size_t i = 0; i < kNumHosts; i++) {
    delete rdma_vector[i];
    delete falcon_vector[i];
  }
}

// Tests if the static route information is passed to Falcon successfully.
TEST(ConnectionManagerTest, PassStaticRouteInfoToFalcon) {
  std::vector<RdmaFalconModel*> rdma_vector;
  std::vector<FalconModel*> falcon_vector;
  ConnectionManager connection_manager;
  SimpleEnvironment env;
  FalconConnectionOptions connection_options;
  PrepareModules(&connection_manager, &env, &rdma_vector, &falcon_vector,
                 kFalconVersion1);

  uint32_t local_cid, remote_cid;
  ASSERT_OK_AND_ASSIGN(std::tie(local_cid, remote_cid),
                       connection_manager.CreateFalconConnection(
                           "host_1", "host_2", 0, 0, connection_options,
                           RdmaConnectedMode::kOrderedRc));
  EXPECT_EQ(local_cid, 0);
  EXPECT_EQ(remote_cid, 0);

  // Tests the parameter is_initiator is set correctly.
  ASSERT_OK_AND_ASSIGN(
      auto connection_state1,
      falcon_vector[0]->get_state_manager()->PerformDirectLookup(local_cid));
  ASSERT_OK_AND_ASSIGN(
      auto connection_state2,
      falcon_vector[1]->get_state_manager()->PerformDirectLookup(remote_cid));

  // Tests if the static routing lists equal to the one specified in the
  // configuration file.
  std::vector<std::vector<uint32_t>> expected_forward_static_port_list1 = {
      {1, 2, 3, 4}, {9, 10, 11, 12}, {13, 14, 15, 16}, {1, 2, 3, 4},
      {1, 2, 3, 4}, {9, 10, 11, 12}, {13, 14, 15, 16}, {1, 2, 3, 4}};
  std::vector<std::vector<uint32_t>> expected_reverse_static_port_list1 = {
      {5, 6, 7, 8}, {13, 14, 15, 16}, {9, 10, 11, 12}, {5, 6, 7, 8},
      {5, 6, 7, 8}, {13, 14, 15, 16}, {9, 10, 11, 12}, {5, 6, 7, 8}};
  EXPECT_THAT(
      connection_state1->connection_metadata->static_routing_port_lists.value(),
      testing::UnorderedElementsAreArray(expected_forward_static_port_list1));

  EXPECT_THAT(
      connection_state2->connection_metadata->static_routing_port_lists.value(),
      testing::UnorderedElementsAreArray(expected_reverse_static_port_list1));

  for (size_t i = 0; i < kNumHosts; i++) {
    delete rdma_vector[i];
    delete falcon_vector[i];
  }
}

// Test for method CreateConnection in class ConnectionManager.
TEST(ConnectionManagerTest, CreateConnection) {
  std::vector<RdmaFalconModel*> rdma_vector;
  std::vector<FalconModel*> falcon_vector;
  ConnectionManager connection_manager;
  SimpleEnvironment env;
  PrepareModules(&connection_manager, &env, &rdma_vector, &falcon_vector,
                 kFalconVersion1);

  QpId local_qp_id, remote_qp_id;
  QpOptions qp_options;
  FalconConnectionOptions connection_options;
  RdmaConnectedMode rc_mode = RdmaConnectedMode::kOrderedRc;
  ASSERT_OK_AND_ASSIGN(
      std::tie(local_qp_id, remote_qp_id),
      connection_manager.CreateConnection("host_1", "host_2", 0, 0, qp_options,
                                          connection_options, rc_mode));
  EXPECT_EQ(local_qp_id, 1);
  EXPECT_EQ(remote_qp_id, 1);

  ASSERT_OK_AND_ASSIGN(
      std::tie(local_qp_id, remote_qp_id),
      connection_manager.CreateConnection("host_2", "host_1", 0, 0, qp_options,
                                          connection_options, rc_mode));
  EXPECT_EQ(local_qp_id, 2);
  EXPECT_EQ(remote_qp_id, 2);

  ASSERT_OK_AND_ASSIGN(
      std::tie(local_qp_id, remote_qp_id),
      connection_manager.CreateConnection("host_1", "host_3", 0, 0, qp_options,
                                          connection_options, rc_mode));
  EXPECT_EQ(local_qp_id, 3);
  EXPECT_EQ(remote_qp_id, 1);

  for (size_t i = 0; i < kNumHosts; i++) {
    delete rdma_vector[i];
    delete falcon_vector[i];
  }
}

namespace {

// Test for method GetOrCreateBidirectionalConnection in class
// ConnectionManager.
TEST(ConnectionManagerTest, GetOrCreateBidirectionalConnection) {
  std::vector<RdmaFalconModel*> rdma_vector;
  std::vector<FalconModel*> falcon_vector;
  ConnectionManager* connection_manager =
      ConnectionManager::GetConnectionManager();
  SimpleEnvironment env;
  PrepareModules(connection_manager, &env, &rdma_vector, &falcon_vector,
                 kFalconVersion1);

  QpId local_qp_id, remote_qp_id;
  QpOptions qp_options;
  FalconConnectionOptions connection_options;
  RdmaConnectedMode rc_mode = RdmaConnectedMode::kOrderedRc;
  ASSERT_OK_AND_ASSIGN(
      std::tie(local_qp_id, remote_qp_id),
      connection_manager->GetOrCreateBidirectionalConnection(
          "host_1", "host_2", 0, 0, qp_options, connection_options, rc_mode));
  EXPECT_EQ(local_qp_id, 1);
  EXPECT_EQ(remote_qp_id, 1);

  ASSERT_OK_AND_ASSIGN(
      std::tie(local_qp_id, remote_qp_id),
      connection_manager->GetOrCreateBidirectionalConnection(
          "host_2", "host_1", 0, 0, qp_options, connection_options, rc_mode));
  EXPECT_EQ(local_qp_id, 1);
  EXPECT_EQ(remote_qp_id, 1);

  // CreateConnection shall create a new one, without creating for the reverse
  // direction.
  ASSERT_OK_AND_ASSIGN(
      std::tie(local_qp_id, remote_qp_id),
      connection_manager->CreateUnidirectionalConnection(
          "host_1", "host_3", 0, 0, qp_options, connection_options, rc_mode));
  EXPECT_EQ(local_qp_id, 2);
  EXPECT_EQ(remote_qp_id, 1);

  // This bidirectional connection should create a new one, since the previous
  // CreateConnection does not create for reverse direction.
  ASSERT_OK_AND_ASSIGN(
      std::tie(local_qp_id, remote_qp_id),
      connection_manager->GetOrCreateBidirectionalConnection(
          "host_3", "host_1", 0, 0, qp_options, connection_options, rc_mode));
  EXPECT_EQ(local_qp_id, 2);
  EXPECT_EQ(remote_qp_id, 3);

  // This should get an existing one created above.
  ASSERT_OK_AND_ASSIGN(
      std::tie(local_qp_id, remote_qp_id),
      connection_manager->GetOrCreateBidirectionalConnection(
          "host_1", "host_3", 0, 0, qp_options, connection_options, rc_mode));
  EXPECT_EQ(local_qp_id, 3);
  EXPECT_EQ(remote_qp_id, 2);

  // This should create a new one.
  ASSERT_OK_AND_ASSIGN(
      std::tie(local_qp_id, remote_qp_id),
      connection_manager->GetOrCreateBidirectionalConnection(
          "host_2", "host_3", 0, 0, qp_options, connection_options, rc_mode));
  EXPECT_EQ(local_qp_id, 2);
  EXPECT_EQ(remote_qp_id, 3);

  // This should create a new one.
  ASSERT_OK_AND_ASSIGN(
      std::tie(local_qp_id, remote_qp_id),
      connection_manager->CreateUnidirectionalConnection(
          "host_2", "host_3", 0, 0, qp_options, connection_options, rc_mode));
  EXPECT_EQ(local_qp_id, 3);
  EXPECT_EQ(remote_qp_id, 4);

  // This should get the one created by the previous
  // GetOrCreateBidirectionalConnection.
  ASSERT_OK_AND_ASSIGN(
      std::tie(local_qp_id, remote_qp_id),
      connection_manager->GetOrCreateBidirectionalConnection(
          "host_3", "host_2", 0, 0, qp_options, connection_options, rc_mode));
  EXPECT_EQ(local_qp_id, 3);
  EXPECT_EQ(remote_qp_id, 2);

  for (size_t i = 0; i < kNumHosts; i++) {
    delete rdma_vector[i];
    delete falcon_vector[i];
  }
}

// Test for method GetOrCreateBidirectionalConnection in class
// ConnectionManager.
TEST(ConnectionManagerTest, DifferentRcMode) {
  std::vector<RdmaFalconModel*> rdma_vector;
  std::vector<FalconModel*> falcon_vector;
  ConnectionManager* connection_manager =
      ConnectionManager::GetConnectionManager();
  SimpleEnvironment env;
  PrepareModules(connection_manager, &env, &rdma_vector, &falcon_vector,
                 kFalconVersion1);

  QpId local_qp_id, remote_qp_id;
  QpOptions qp_options;
  FalconConnectionOptions connection_options;
  ASSERT_OK_AND_ASSIGN(std::tie(local_qp_id, remote_qp_id),
                       connection_manager->GetOrCreateBidirectionalConnection(
                           "host_1", "host_2", 0, 0, qp_options,
                           connection_options, RdmaConnectedMode::kOrderedRc));
  EXPECT_EQ(local_qp_id, 1);
  EXPECT_EQ(remote_qp_id, 1);

  ASSERT_OK_AND_ASSIGN(
      std::tie(local_qp_id, remote_qp_id),
      connection_manager->GetOrCreateBidirectionalConnection(
          "host_2", "host_1", 0, 0, qp_options, connection_options,
          RdmaConnectedMode::kUnorderedRc));
  EXPECT_EQ(local_qp_id, 2);
  EXPECT_EQ(remote_qp_id, 2);

  for (size_t i = 0; i < kNumHosts; i++) {
    delete rdma_vector[i];
    delete falcon_vector[i];
  }
}

}  // namespace

}  // namespace isekai
