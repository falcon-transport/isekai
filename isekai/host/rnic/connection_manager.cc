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

#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/meta/type_traits.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "absl/strings/string_view.h"
#include "absl/strings/strip.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/common/status_util.h"

namespace isekai {

absl::StatusOr<std::tuple<QpId, QpId>> ConnectionManager::CreateConnection(
    absl::string_view host_id1, absl::string_view host_id2,
    uint8_t source_bifurcation_id, uint8_t destination_bifurcation_id,
    QpOptions& qp_options, FalconConnectionOptions& connection_options,
    RdmaConnectedMode rc_mode) {
  uint32_t cid1, cid2;
  ASSIGN_OR_RETURN(
      std::tie(cid1, cid2),
      CreateFalconConnection(host_id1, host_id2, source_bifurcation_id,
                             destination_bifurcation_id, connection_options,
                             rc_mode));

  ASSIGN_OR_RETURN(QpId qp_id_1, GenerateLocalQpId(host_id1));
  ASSIGN_OR_RETURN(QpId qp_id_2, GenerateLocalQpId(host_id2));
  const auto& host_1_iter = host_id_to_host_info_.find(host_id1);
  if (host_1_iter == host_id_to_host_info_.end()) {
    return absl::InternalError(
        absl::StrCat("Can not find info for the host with id ", host_id1));
  }
  const auto& host_2_iter = host_id_to_host_info_.find(host_id2);
  if (host_2_iter == host_id_to_host_info_.end()) {
    return absl::InternalError(
        absl::StrCat("Can not find info for the host with id ", host_id2));
  }
  // Setup host1.
  qp_options.scid = cid1;
  qp_options.dst_ip = host_2_iter->second.ip_address;
  //

  host_1_iter->second.rdma->CreateRcQp(qp_id_1, qp_id_2, qp_options, rc_mode);
  host_1_iter->second.rdma->Associate(qp_id_1, qp_id_2, cid1);
  // Setup host2.
  qp_options.scid = cid2;
  qp_options.dst_ip = host_1_iter->second.ip_address;

  host_2_iter->second.rdma->CreateRcQp(qp_id_2, qp_id_1, qp_options, rc_mode);
  host_2_iter->second.rdma->Associate(qp_id_2, qp_id_1, cid2);

  return std::make_tuple(qp_id_1, qp_id_2);
}

absl::StatusOr<std::tuple<QpId, QpId>>
ConnectionManager::GetOrCreateBidirectionalConnection(
    absl::string_view host_id1, absl::string_view host_id2,
    uint8_t source_bifurcation_id, uint8_t destination_bifurcation_id,
    QpOptions& qp_options, FalconConnectionOptions& connection_options,
    RdmaConnectedMode rc_mode) {
  auto full_host_id1 = absl::StrCat(host_id1, "_", source_bifurcation_id);
  auto full_host_id2 = absl::StrCat(host_id2, "_", destination_bifurcation_id);
  auto& conn = unused_connections_[full_host_id1][full_host_id2][rc_mode];
  std::tuple<QpId, QpId> qp;
  if (conn.empty()) {
    ASSIGN_OR_RETURN(qp,
                     CreateConnection(host_id1, host_id2, source_bifurcation_id,
                                      destination_bifurcation_id, qp_options,
                                      connection_options, rc_mode));
    // The reverse direction of this qp is not used yet.
    unused_connections_[full_host_id2][full_host_id1][rc_mode].push_back(
        std::make_pair(std::get<1>(qp), std::get<0>(qp)));
  } else {
    qp = conn.back();
    conn.pop_back();
  }
  return qp;
}

absl::StatusOr<std::tuple<QpId, QpId>>
ConnectionManager::CreateUnidirectionalConnection(
    absl::string_view host_id1, absl::string_view host_id2,
    uint8_t source_bifurcation_id, uint8_t destination_bifurcation_id,
    QpOptions& qp_options, FalconConnectionOptions& connection_options,
    RdmaConnectedMode rc_mode) {
  std::tuple<QpId, QpId> qp;
  ASSIGN_OR_RETURN(qp,
                   CreateConnection(host_id1, host_id2, source_bifurcation_id,
                                    destination_bifurcation_id, qp_options,
                                    connection_options, rc_mode));
  return qp;
}

void ConnectionManager::RegisterRdma(absl::string_view host_id,
                                     RdmaBaseInterface* rdma) {
  host_id_to_host_info_[host_id].rdma = rdma;
}

void ConnectionManager::RegisterFalcon(absl::string_view host_id,
                                       FalconInterface* falcon) {
  host_id_to_host_info_[host_id].falcon = falcon;
}

void ConnectionManager::PopulateHostInfo(
    const NetworkConfig& simulation_network) {
  for (const auto& host : simulation_network.hosts()) {
    auto& host_info = host_id_to_host_info_[host.id()];
    host_info.ip_address = host.ip_address();
    host_info.next_local_cid = 0;
    host_info.next_local_qp_id = kInitialQpId;
  }

  // Installs the static routing entries.
  if (simulation_network.has_connection_routing_configs()) {
    for (const auto& static_routing_rule_entry :
         simulation_network.connection_routing_configs()
             .static_routing_rule_entries()) {
      InstallStaticRoutingRule(static_routing_rule_entry);
    }
  }
}

// Installs the static routing entry to the corresponding host from
// the StaticRoutingRuleEntry.
void ConnectionManager::InstallStaticRoutingRule(
    const StaticRoutingRuleEntry& static_routing_rule_entry) {
  auto& src_host_id = static_routing_rule_entry.src_host_id();
  auto& dst_host_id = static_routing_rule_entry.dst_host_id();
  //
  // supports one destination host for each source host. In the future, we might
  // want to support specifying different destination hosts for the same source
  // host.
  if (host_id_to_host_info_[src_host_id].dst_host_id.empty()) {
    host_id_to_host_info_[src_host_id].dst_host_id = dst_host_id;

    for (const auto& static_routing_port_list_in_string :
         static_routing_rule_entry.path_static_routing_port_list()) {
      std::string forward_routing_port_list_in_string =
          static_routing_port_list_in_string.forward_routing_port_list();
      ParseAndStoreRoutingPortList(
          forward_routing_port_list_in_string,
          host_id_to_host_info_[src_host_id].forward_static_routing_port_lists);

      std::string reverse_routing_port_list_in_string =
          static_routing_port_list_in_string.reverse_routing_port_list();
      ParseAndStoreRoutingPortList(
          reverse_routing_port_list_in_string,
          host_id_to_host_info_[src_host_id].reverse_static_routing_port_lists);
    }
  } else {
    // A static routing rule to another dst_host_id for this host already
    // exists.
    if (host_id_to_host_info_[src_host_id].dst_host_id != dst_host_id) {
      LOG(FATAL)
          << "A static routing rule to another dst_host_id for this host "
             "already exists.";
    }
  }
}

// Parses and stores the static routing port list to the corresponding host
// info.
void ConnectionManager::ParseAndStoreRoutingPortList(
    std::string static_routing_port_list_in_string,
    std::optional<std::vector<std::vector<uint32_t>>>&
        static_routing_port_lists) {
  // Creates a vector for storing the static routing port list if there is none.
  if (!static_routing_port_lists.has_value()) {
    static_routing_port_lists = std::vector<std::vector<uint32_t>>();
  }

  std::vector<uint32_t> routing_port_list;

  static_routing_port_list_in_string =
      absl::StripPrefix(static_routing_port_list_in_string, "[");
  static_routing_port_list_in_string =
      absl::StripSuffix(static_routing_port_list_in_string, "]");
  std::vector<std::string> port_string_tokens = absl::StrSplit(
      static_routing_port_list_in_string, ',', absl::SkipWhitespace());
  for (const auto& token : port_string_tokens) {
    uint32_t value;
    CHECK(absl::SimpleAtoi(token, &value));
    routing_port_list.push_back(value);
  }
  static_routing_port_lists.value().push_back(routing_port_list);
}

absl::StatusOr<std::string> ConnectionManager::GetIpAddress(
    absl::string_view host_id) {
  const auto& host_iter = host_id_to_host_info_.find(host_id);
  if (host_iter == host_id_to_host_info_.end()) {
    return absl::InternalError(
        absl::StrCat("Can not find info for the host with id ", host_id));
  }
  return host_iter->second.ip_address;
}

absl::StatusOr<std::tuple<uint32_t, uint32_t>>
ConnectionManager::CreateFalconConnection(
    absl::string_view local_host_id, absl::string_view remote_host_id,
    uint8_t source_bifurcation_id, uint8_t destination_bifurcation_id,
    FalconConnectionOptions& connection_options, RdmaConnectedMode rc_mode) {
  ASSIGN_OR_RETURN(uint32_t local_cid, GenerateLocalCid(local_host_id));
  ASSIGN_OR_RETURN(uint32_t remote_cid, GenerateLocalCid(remote_host_id));

  const auto& local_host_iter = host_id_to_host_info_.find(local_host_id);
  if (local_host_iter == host_id_to_host_info_.end()) {
    return absl::InternalError(
        absl::StrCat("Can not find info for the host with id ", local_host_id));
  }
  const auto& remote_host_iter = host_id_to_host_info_.find(remote_host_id);
  if (remote_host_iter == host_id_to_host_info_.end()) {
    return absl::InternalError(absl::StrCat(
        "Can not find info for the host with id ", remote_host_id));
  }

  OrderingMode ordering_mode = rc_mode == RdmaConnectedMode::kOrderedRc
                                   ? OrderingMode::kOrdered
                                   : OrderingMode::kUnordered;

  // If a destination host ID is specified in a previous static route
  // configuration for this local host and is equal to the current destination
  // (remote) host, pass the corresponding static routing list to the
  // connection_options.
  bool static_routing = false;
  if (local_host_iter->second.dst_host_id == remote_host_id) {
    static_routing = true;
  }

  if (local_host_iter->second.falcon) {
    if (static_routing) {
      // The lists of forward static routing ports will be passed to the local
      // host on this connection.
      connection_options.static_routing_port_lists =
          local_host_iter->second.forward_static_routing_port_lists;
    }
    RETURN_IF_ERROR(local_host_iter->second.falcon->EstablishConnection(
        local_cid, remote_cid, source_bifurcation_id,
        destination_bifurcation_id, remote_host_iter->second.ip_address,
        ordering_mode, connection_options));
  }
  if (remote_host_iter->second.falcon) {
    if (static_routing) {
      // The lists of reverse static routing ports will be passed to the remote
      // host on this connection.
      connection_options.static_routing_port_lists =
          local_host_iter->second.reverse_static_routing_port_lists;
    }
    RETURN_IF_ERROR(remote_host_iter->second.falcon->EstablishConnection(
        remote_cid, local_cid, destination_bifurcation_id,
        source_bifurcation_id, local_host_iter->second.ip_address,
        ordering_mode, connection_options));
  }

  return std::make_tuple(local_cid, remote_cid);
}

absl::StatusOr<uint32_t> ConnectionManager::GenerateLocalCid(
    absl::string_view host_id) {
  // Assumes that a CID should be released before 4294967295 (2^32 - 1) calls of
  // GetConnectionId(), so that no collision happens.
  const auto& host_iter = host_id_to_host_info_.find(host_id);
  if (host_iter == host_id_to_host_info_.end()) {
    return absl::InternalError(
        absl::StrCat("Not find ", host_id, " in host_id_to_next_local_cid_."));
  }

  return host_iter->second.next_local_cid++;
}

absl::StatusOr<QpId> ConnectionManager::GenerateLocalQpId(
    absl::string_view host_id) {
  // Assumes that a QP ID should be released before 4294967295 (2^32 - 2) calls
  // of GetQpId(), so that no collision happens.
  const auto& host_iter = host_id_to_host_info_.find(host_id);
  if (host_iter == host_id_to_host_info_.end()) {
    return absl::InternalError(absl::StrCat(
        "Not find ", host_id, " in host_id_to_next_local_qp_id_."));
  }

  QpId new_qp_id = host_iter->second.next_local_qp_id++;
  if (host_iter->second.next_local_qp_id == 0) {
    // 0 is not a valid QP ID.
    host_iter->second.next_local_qp_id++;
  }
  return new_qp_id;
}

}  // namespace isekai
