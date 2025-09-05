/*
 * Copyright 2024 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ISEKAI_HOST_RNIC_CONNECTION_MANAGER_H_
#define ISEKAI_HOST_RNIC_CONNECTION_MANAGER_H_

#include <cstdint>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "gtest/gtest_prod.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/model_interfaces.h"

namespace isekai {

//
// Related Parts.

// Initial QP ID.
constexpr QpId kInitialQpId = 1;

// Tuple defined struct for host information.
struct TupleDefinedHostInfo {
  RdmaBaseInterface* rdma;
  FalconInterface* falcon;
  std::string ip_address;
  uint32_t next_local_cid;
  QpId next_local_qp_id;
  // The following fields are for configuring static routing.
  // We support configuring a list of forward and reverse router ports between
  // this host and dst_host.
  std::string dst_host_id = "";
  // These two vectors are used to store the forward and reverse static
  // routing port lists for every path.
  // The index of each vector element corresponds to the path ID. Each list
  // specifies the router ports that packets on this path should traverse
  // sequentially, either in the forward or reverse direction. The number of
  // elements in the outer vector must be equal to the degree of multipathing.
  // The number of elements in the nested vector must be equal to the number
  // of routers experienced.
  std::optional<std::vector<std::vector<uint32_t>>>
      forward_static_routing_port_lists;
  std::optional<std::vector<std::vector<uint32_t>>>
      reverse_static_routing_port_lists;
};

// Assumes no concurrent accessing.
class ConnectionManager : public ConnectionManagerInterface {
  friend class FakeConnectionManager;

 public:
  static class ConnectionManager* GetConnectionManager() {
    static class ConnectionManager* const connection_manager =
        new class ConnectionManager();
    return connection_manager;
  }

  // Creates a connection between host_id1 and host_id2, returns the
  // local_qp_id, remote_qp_id. It should be called after
  // PopulateHostInfo(), RegisterRdma(), RegisterFalcon().
  absl::StatusOr<std::tuple<QpId, QpId>> CreateConnection(
      absl::string_view host_id1, absl::string_view host_id2,
      uint8_t source_bifurcation_id, uint8_t destination_bifurcation_id,
      QpOptions& qp_options, FalconConnectionOptions& connection_options,
      RdmaConnectedMode rc_mode) override;
  // Returns a bidirectional connection from host_id1 to host_id2. The
  // connection is created, or the reverse of a previously created bidirectional
  // connection.
  absl::StatusOr<std::tuple<QpId, QpId>> GetOrCreateBidirectionalConnection(
      absl::string_view host_id1, absl::string_view host_id2,
      uint8_t source_bifurcation_id, uint8_t destination_bifurcation_id,
      QpOptions& qp_options, FalconConnectionOptions& connection_options,
      RdmaConnectedMode rc_mode) override;
  // Create a unidirectional connection from host_id1 to host_id2.
  absl::StatusOr<std::tuple<QpId, QpId>> CreateUnidirectionalConnection(
      absl::string_view host_id1, absl::string_view host_id2,
      uint8_t source_bifurcation_id, uint8_t destination_bifurcation_id,
      QpOptions& qp_options, FalconConnectionOptions& connection_options,
      RdmaConnectedMode rc_mode) override;
  // Registers RDMA module. It should be called by
  // RdmaBaseModel::RegisterRdmaToConnectionManager() before CreateConnection().
  void RegisterRdma(absl::string_view host_id,
                    RdmaBaseInterface* rdma) override;
  // Registers FALCON module. It should be called by
  // FalconModel::RegisterFalconToConnectionManager() before CreateConnection().
  void RegisterFalcon(absl::string_view host_id,
                      FalconInterface* falcon) override;
  // Populates the host information from simulation_network. It should be called
  // before CreateConnection().
  void PopulateHostInfo(const NetworkConfig& simulation_network) override;
  // Get the IP address of a host.
  absl::StatusOr<std::string> GetIpAddress(absl::string_view host_id) override;
  // Installs the static routing entry to the corresponding host from
  // the StaticRoutingRuleEntry.
  void InstallStaticRoutingRule(
      const StaticRoutingRuleEntry& static_routing_rule_entry);
  // Parses and stores the static routing port list to the corresponding host
  // info.
  void ParseAndStoreRoutingPortList(
      std::string static_routing_port_list_in_string,
      std::optional<std::vector<std::vector<uint32_t>>>&
          static_routing_port_lists);

 protected:
  //
  FRIEND_TEST(ConnectionManagerTest, PopulateHostInfo);
  FRIEND_TEST(ConnectionManagerTest, PopulateStaticRouteInfo);
  FRIEND_TEST(ConnectionManagerTest, CreateFalconConnection);
  FRIEND_TEST(ConnectionManagerTest, CreateConnection);
  FRIEND_TEST(ConnectionManagerTest, PassStaticRouteInfoToFalcon);

  ConnectionManager() {}

  // Creates the FALCON connection, returns (local_cid, remote_cid). It should
  // be called after PopulateHostInfo(), RegisterRdma(), RegisterFalcon() being
  // called.
  absl::StatusOr<std::tuple<uint32_t, uint32_t>> CreateFalconConnection(
      absl::string_view local_host_id, absl::string_view remote_host_id,
      uint8_t source_bifurcation_id, uint8_t destination_bifurcation_id,
      FalconConnectionOptions& connection_options, RdmaConnectedMode rc_mode);
  // Creates CID on the host for the new connection.
  virtual absl::StatusOr<uint32_t> GenerateLocalCid(absl::string_view host_id);
  // Creates QP ID on the host for the new connection.
  absl::StatusOr<QpId> GenerateLocalQpId(absl::string_view host_id);

  // Host ID to host information (TupleDefinedHostInfo).
  absl::flat_hash_map<std::string, TupleDefinedHostInfo> host_id_to_host_info_;

  // After creating a bidirectional connection, one direction is returned while
  // the other drection is temporarily unused. We should remember the unused
  // direction, so later we can return it to the host on the other end of the
  // connection. The stored connections are indexed bysource host ID and dest
  // host ID.
  absl::flat_hash_map<
      std::string,
      absl::flat_hash_map<
          std::string, absl::flat_hash_map<RdmaConnectedMode,
                                           std::vector<std::pair<QpId, QpId>>>>>
      unused_connections_;
};

};  // namespace isekai

#endif  // ISEKAI_HOST_RNIC_CONNECTION_MANAGER_H_
