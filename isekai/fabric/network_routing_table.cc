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

#include "isekai/fabric/network_routing_table.h"

#include <linux/if_ether.h>
#include <net/ethernet.h>

#include <cstdint>

#include "absl/status/statusor.h"
#include "glog/logging.h"
#include "isekai/common/file_util.h"
#include "isekai/common/net_address.h"

namespace isekai {

// Default VRF id. Used when src_mac_to_vrf_map_ in class NetworkRoutingTable is
// empty.
const uint32_t kDefaultVrfId = 1;

absl::StatusOr<std::unique_ptr<NetworkRoutingTable>>
NetworkRoutingTable::Create(std::string_view routing_table_proto) {
  RoutingConfig routing_config;
  ReadProtoFromRecordioFile(routing_table_proto, routing_config);

  return absl::WrapUnique(new NetworkRoutingTable(routing_config));
}

NetworkRoutingTable::NetworkRoutingTable(const RoutingConfig& routing_config) {
  routing_table_name_ = routing_config.node_id();
  // Populates src_mac_to_vrf_map_.
  for (const auto& src_mac_to_vrf_entry :
       routing_config.src_mac_to_vrf_entries()) {
    // The src mac is stored in bytes format. So we need to convert it.
    ether_addr mac_addr;
    mempcpy(mac_addr.ether_addr_octet, src_mac_to_vrf_entry.src_mac().c_str(),
            ETH_ALEN);
    src_mac_to_vrf_map_[MacAddress(
        mac_addr.ether_addr_octet[0], mac_addr.ether_addr_octet[1],
        mac_addr.ether_addr_octet[2], mac_addr.ether_addr_octet[3],
        mac_addr.ether_addr_octet[4], mac_addr.ether_addr_octet[5])] =
        src_mac_to_vrf_entry.vrf_id();
  }

  // Populates ndv6_ip_table_.
  absl::flat_hash_map<IPRange, std::vector<OutputInfo>> ndv6_ip_to_output_info;
  for (const auto& flow : routing_config.ip_to_port_index().flows()) {
    auto ip_range =
        IPRangeFromHexIPv6Address(flow.ip_address(), flow.ip_mask());
    if (ip_range.ok()) {
      ndv6_ip_to_output_info[ip_range.value()].push_back(
          OutputInfo(kDefaultWeightForNdv6OutputPort, flow.port_index()));
    }
  }
  for (const auto& entry : ndv6_ip_to_output_info) {
    ndv6_ip_table_.insert(entry);
  }

  // Populates vrfs_.
  for (const auto& vrfs_entry : routing_config.vrfs()) {
    for (const auto& dst_ip_to_group_id : vrfs_entry.second.flows()) {
      auto ip_range = IPRangeFromHexIPv6Address(dst_ip_to_group_id.ip_address(),
                                                dst_ip_to_group_id.ip_mask());
      if (ip_range.ok()) {
        CHECK(ip_range.ok());
        vrfs_[vrfs_entry.first].insert(
            std::make_pair(ip_range.value(), dst_ip_to_group_id.group_id()));
      }
    }
  }

  // Populates groups_.
  for (const auto& group_entry : routing_config.groups()) {
    ether_addr mac_addr;
    mempcpy(mac_addr.ether_addr_octet,
            group_entry.second.output_src_mac().c_str(), ETH_ALEN);
    GroupInfo group_info;
    group_info.set_src_mac =
        MacAddress(mac_addr.ether_addr_octet[0], mac_addr.ether_addr_octet[1],
                   mac_addr.ether_addr_octet[2], mac_addr.ether_addr_octet[3],
                   mac_addr.ether_addr_octet[4], mac_addr.ether_addr_octet[5]);
    for (const auto& group_table_entry :
         group_entry.second.group_table_entries()) {
      group_info.output_options.push_back(OutputInfo(
          group_table_entry.weight(), group_table_entry.port_index()));
    }
    groups_[group_entry.first] = group_info;
  }
}

absl::StatusOr<IPRange> IPRangeFromHexIPv6Address(
    const std::string& hex_ipv6_address,
    const std::string& hex_ipv6_address_mask) {
  auto ip_address = HexIPv6AddressToBinaryFormat(hex_ipv6_address);
  auto ip_address_mask = HexIPv6AddressToBinaryFormat(hex_ipv6_address_mask);
  return IPRange(ip_address, ip_address_mask);
}

absl::StatusOr<const TableOutputOptions*>
NetworkRoutingTable::GetOutputOptionsAndModifyPacket(
    omnetpp::cMessage* packet) const {
  const auto src_mac = GetPacketSourceMac(packet);
  const auto dst_ip = StringToIPAddressOrDie(GetPacketDestinationIp(packet));

  // Orders for VRF matching:
  //   1. It tries to match with NDv6 flow. If matched, return.
  //   2. If not match in ndv6_ip_table_, it tries to use the default VRF ID
  //   (80).
  //   3. If the src_mac_to_vrf_map_ is not empty, it tries to update the
  //   vrf_id.
  if (!ndv6_ip_table_.empty()) {
    auto matched_element = ndv6_ip_table_.find_longest(dst_ip);
    if (matched_element.ok()) {
      SetPacketSourceMac(packet, kDefaultNdv6FlowSourceMacAddress);
      return matched_element.value();
    }
  }

  // If the src_mac_to_vrf_map_ is empty, it uses the kDefaultVrfId (80); else,
  // it tries to get the vrf_id from the src_mac in src_mac_to_vrf_map_.
  uint32_t vrf_id = 0;
  if (src_mac_to_vrf_map_.empty()) {
    VLOG(2) << "src_mac_to_vrf_map_ is empty. Used kDefaultVrfId.\n";
    vrf_id = kDefaultVrfId;
  } else {
    const auto& src_mac_to_vrf_iter = src_mac_to_vrf_map_.find(src_mac);
    if (src_mac_to_vrf_iter == src_mac_to_vrf_map_.end()) {
      return absl::InternalError(absl::StrCat(
          "Not find src_mac ", src_mac.ToString(), " for packet."));
    }
    VLOG(2) << "src_mac_to_vrf_map_ is not empty. vrf_id = "
            << src_mac_to_vrf_iter->second << std::endl;
    vrf_id = src_mac_to_vrf_iter->second;
  }
  const auto& vrf_iter = vrfs_.find(vrf_id);
  if (vrf_iter == vrfs_.end()) {
    return absl::InternalError(
        absl::StrCat("Not find vrf_id ", vrf_id, " for packet."));
  }
  const auto& ip_table = vrf_iter->second;

  // If there are IPv6 addresses in ip_table matched in net mask, find_longest()
  // returns the iterator with longest prefix match; else returns
  // ip_table.end(). If ip_table.find_longest() returns ip_table.end(),
  // GetOutputOptionsAndModifyPacket returns error.
  auto matched_element = ip_table.find_longest(dst_ip);
  if (!matched_element.ok()) {
    return absl::InternalError("Not find route for packet with DIP.");
  }
  uint32_t group_id = *matched_element.value();
  const auto& group = groups_.find(group_id);
  if (group == groups_.end()) {
    return absl::InternalError(
        absl::StrCat("Failed to find group_id ", group_id, "."));
  }
  const auto& group_info = group->second;
  SetPacketSourceMac(packet, group_info.set_src_mac);

  return &(group_info.output_options);
}

}  // namespace isekai
