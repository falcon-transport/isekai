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

#ifndef ISEKAI_FABRIC_NETWORK_ROUTING_TABLE_H_
#define ISEKAI_FABRIC_NETWORK_ROUTING_TABLE_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "glog/logging.h"
#include "gtest/gtest_prod.h"
#include "isekai/common/ipv6_trie.h"
#include "isekai/common/net_address.h"
#include "isekai/fabric/packet_util.h"
#include "isekai/fabric/routing.pb.h"
#include "omnetpp/cmessage.h"

namespace isekai {

// Default weight for NDv6 output port.
const uint32_t kDefaultWeightForNdv6OutputPort = 1;

// The source MAC address for packet matched with NDv6 flow.
const MacAddress kDefaultNdv6FlowSourceMacAddress(0x02, 0x00, 0x00, 0x00, 0x00,
                                                  0x10);

// Tuple defined struct for output port and its weight. It contains ([weight,
// port_index], ..., [weight, port_index]).
struct OutputInfo {
  uint32_t weight;
  uint32_t port_index;

  template <typename H>
  friend H AbslHashValue(H h, const OutputInfo& s) {
    return H::combine(std::move(h), s.weight, s.port_index);
  }

  const inline bool operator==(const OutputInfo& s) const {
    return (weight == s.weight && port_index == s.port_index);
  }

  const inline bool operator<(const OutputInfo& s) const {
    if (weight < s.weight) {
      return true;
    } else {
      return false;
    }
  }

  // Custom constructors to facilitate the creation of OutputInfo instance.
  OutputInfo(uint32_t weight, uint32_t port_index)
      : weight(weight), port_index(port_index) {}
  OutputInfo() {}
};

// Tuple defined struct for ProgrammedGroup. It contains (group_src_mac,
// ([weight, port_index], ..., [weight, port_index])).
struct GroupInfo {
  MacAddress set_src_mac;
  std::vector<OutputInfo> output_options;

  // Custom constructors to facilitate the creation of GroupInfo instance.
  GroupInfo(const std::string& src_mac,
            const std::vector<OutputInfo>& output_options)
      : output_options(output_options) {
    auto mac_addr = MacAddress::OfString(src_mac);
    CHECK(mac_addr.ok());
    set_src_mac = mac_addr.value();
  }
  GroupInfo() {}
};

// Generates IPRange from hex_ipv6_address and
// hex_ipv6_address_mask.
absl::StatusOr<IPRange> IPRangeFromHexIPv6Address(
    const std::string& hex_ipv6_address,
    const std::string& hex_ipv6_address_mask);

using TableOutputOptions = std::vector<OutputInfo>;

class NetworkRoutingTable {
 public:
  // Initializes the routing table based on the routing_table_proto (a RecordIO
  // format file contains RoutingConfig).
  static absl::StatusOr<std::unique_ptr<NetworkRoutingTable>> Create(
      std::string_view routing_table_proto);

  // Gets the output options from routing table, and modify the MAC address of
  // the packet.  If this function returns error, the packet will be dropped due
  // to lack of flows.
  absl::StatusOr<const TableOutputOptions*> GetOutputOptionsAndModifyPacket(
      omnetpp::cMessage* packet) const;

 private:
  FRIEND_TEST(NetworkRoutingTableTest, Create);

  explicit NetworkRoutingTable(const RoutingConfig& routing_config);

  // {src_mac} -> {vrf_id}.
  absl::flat_hash_map<MacAddress, uint32_t> src_mac_to_vrf_map_;

  // {vrf_id} -> {dst_ip} -> {group_id}. dst_ip is with type IPRange,
  // group_id uint32.
  absl::flat_hash_map<uint32_t, IPTable<uint32_t>> vrfs_;

  // {dst_ip} -> ([/* weight = */ 1, port_index]), used for NDv6 flow.
  IPTable<TableOutputOptions> ndv6_ip_table_;

  // group_id -> (group_src_mac, ([weight, port_index], ..., [weight,
  // port_index])).
  absl::flat_hash_map<uint32_t, GroupInfo> groups_;

  // Routing table name.
  std::string routing_table_name_;
};

};  // namespace isekai

#endif  // ISEKAI_FABRIC_NETWORK_ROUTING_TABLE_H_
