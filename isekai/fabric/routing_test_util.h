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

#ifndef ISEKAI_FABRIC_ROUTING_TEST_UTIL_H_
#define ISEKAI_FABRIC_ROUTING_TEST_UTIL_H_

#include <stddef.h>

#include <cstdint>
#include <memory>
#include <string>

#include "absl/status/status.h"
#include "absl/strings/string_view.h"
#include "inet/common/packet/Packet.h"
#include "isekai/common/net_address.h"
#include "omnetpp/cmessage.h"

namespace isekai {

// The IPv6 address stored in IPAddress is subject to zero compression
// (See IPAddress::ToString() for more details). Default source IPv6
// address for test.
constexpr char kDefaultSrcIpv6Address[] = "2001:db8:85a3::8a2e:370:7002";

// Destination IPv6 address for testing VRF 80.
constexpr char kTestDstIpv6Address1[] = "2001:db8:85a3::8a2e:370:7334";

// Number of bytes for IPv6 address.
constexpr size_t kIpv6AddressLength = 16;

// Destination IPv6 address in hex for testing VRF 80.
constexpr uint8_t kTestDstIpv6AddressHex1[] = {
    0x20, 0x01, 0x0d, 0xb8, 0x85, 0xa3, 0x00, 0x00,
    0x00, 0x00, 0x8a, 0x2e, 0x03, 0x70, 0x73, 0x34};

// Destination IPv6 address for testing VRF 112.
constexpr char kTestDstIpv6Address2[] = "2001:db8:85a4::8a2e:370:7334";

// Destination IPv6 address in hex for testing VRF 112.
constexpr uint8_t kTestDstIpv6AddressHex2[] = {
    0x20, 0x01, 0x0d, 0xb8, 0x85, 0xa4, 0x00, 0x00,
    0x00, 0x00, 0x8a, 0x2e, 0x03, 0x70, 0x73, 0x34};

// Destination IPv6 address for testing net_util::IPTable.find_longest()
// unmatched case.
constexpr char kTestDstIpv6Address3[] = "2001:db8:85a4:1000::8a2e:370:7334";

// /52 IPv6 network mask.
constexpr uint8_t kTestIpv6Mask52[] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                       0xf0, 0x00, 0x00, 0x00, 0x00, 0x00,
                                       0x00, 0x00, 0x00, 0x00};

// Destination IPv6 address for NDv6 flow.
constexpr uint8_t kTestDstIpv6AddressNdv6_1[] = {
    0x20, 0x01, 0x0d, 0xb8, 0x85, 0xa3, 0x00, 0x0a,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};

// Destination IPv6 address for NDv6 flow.
constexpr uint8_t kTestDstIpv6AddressNdv6_2[] = {
    0x20, 0x01, 0x0d, 0xb8, 0x85, 0xa3, 0x00, 0x0b,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};

// /64 IPv6 network mask.
constexpr uint8_t kTestIpv6Mask64[] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                       0xff, 0xff, 0x00, 0x00, 0x00, 0x00,
                                       0x00, 0x00, 0x00, 0x00};

// Destination IPv6 address for host.
constexpr char kTestDstIpv6AddressForHost1[] = "2001:db8:85a3:a::1";

// Destination IPv6 address for host.
constexpr char kTestDstIpv6AddressForHost2[] = "2001:db8:85a3:b::1";

// Source MAC for flow with VRF UP.
const MacAddress kTestFlowSrcMacVrfUp(0x02, 0x00, 0x00, 0x00, 0x00, 0x01);
// Source MAC for flow with VRF DOWN.
const MacAddress kTestFlowSrcMacVrfDown(0x02, 0x00, 0x00, 0x00, 0x00, 0x02);

// Config for testing NetworkRoutingTable::GetOutputOptionsAndModifyPacket();
constexpr char kTestRoutingConfigWithUpAndDownVrf[] =
    R"pb(src_mac_to_vrf_entries {
           src_mac: "\x02\x00\x00\x00\x00\x01"
           vrf_id: 80
         }
         src_mac_to_vrf_entries {
           src_mac: "\x02\x00\x00\x00\x00\x02"
           vrf_id: 112
         }
         vrfs {
           key: 80
           value {
             flows {
               ip_address: "\x20\x01\x0d\xb8\x85\xa3\x00\x00\x00\x00\x8a\x2e\x03\x70\x73\x34"
               ip_mask: "\xff\xff\xff\xff\xff\xff\xf0\x00\x00\x00\x00\x00\x00\x00\x00\x00"
               group_id: 12
             }
           }
         }
         vrfs {
           key: 112
           value {
             flows {
               ip_address: "\x20\x01\x0d\xb8\x85\xa4\x00\x00\x00\x00\x8a\x2e\x03\x70\x73\x34"
               ip_mask: "\xff\xff\xff\xff\xff\xff\xf0\x00\x00\x00\x00\x00\x00\x00\x00\x00"
               group_id: 22
             }
           }
         }
         groups {
           key: 12
           value {
             output_src_mac: "\x02\x00\x00\x00\x00\x00"
             group_table_entries { weight: 14 port_index: 0 }
             group_table_entries { weight: 15 port_index: 1 }
           }
         }
         groups {
           key: 22
           value {
             output_src_mac: "\x02\x00\x00\x00\x00\x00"
             group_table_entries { weight: 24 port_index: 2 }
             group_table_entries { weight: 25 port_index: 3 }
           }
         }
         ip_to_port_index {
           flows {
             ip_address: "\x20\x01\x0d\xb8\x85\xa3\x00\x0a\x00\x00\x00\x00\x00\x00\x00\x00"
             ip_mask: "\xff\xff\xff\xff\xff\xff\xff\xff\x00\x00\x00\x00\x00\x00\x00\x00"
             port_index: 2
           }
           flows {
             ip_address: "\x20\x01\x0d\xb8\x85\xa3\x00\x0b\x00\x00\x00\x00\x00\x00\x00\x00"
             ip_mask: "\xff\xff\xff\xff\xff\xff\xff\xff\x00\x00\x00\x00\x00\x00\x00\x00"
             port_index: 3
           }
         })pb";

// Util functions for writing NIBEntity to output_file in RecordIO format.
absl::Status WriteTestRoutingConfigToFile(absl::string_view output_file);

// Helper function for setting the source MAC address in inet::MacAddressInd
// tag for packet.
void SetPacketSourceMacInd(const MacAddress& mac_address,
                           omnetpp::cMessage* packet);

// SetupDummyOmnestSimulation, and CloseOmnestSimulation refer to
// SetupDummyOmnestSimulation, and CloseOmnestSimulation in
// isekai/omnest_environment_test.cc.
omnetpp::cSimulation* SetupDummyOmnestSimulation();
void CloseOmnestSimulation(omnetpp::cSimulation* sim);

// Helper function for generating test INET packet.
std::unique_ptr<inet::Packet> GenerateTestInetPacket(
    const std::string& src_ipv6_address = kDefaultSrcIpv6Address,
    const std::string& dst_ipv6_address = kTestDstIpv6Address1,
    const MacAddress& flow_src_mac = kTestFlowSrcMacVrfUp,
    uint32_t src_port = 1000, uint32_t dest_port = 1000);

}  // namespace isekai

#endif  // ISEKAI_FABRIC_ROUTING_TEST_UTIL_H_
