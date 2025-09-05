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

#include <netinet/in.h>

#include <filesystem>

#include "absl/strings/str_cat.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "inet/networklayer/common/L3Tools.h"
#include "inet/networklayer/ipv6/Ipv6Header.h"
#include "isekai/common/testing.h"  // IWYU pragma: keep
#include "isekai/common/testing.h"
#include "isekai/fabric/routing_test_util.h"
#include "omnetpp.h"
#include "omnetpp/cmessage.h"

namespace isekai {

namespace {

// Test for method GetOutputOptionsAndModifyPacket in class NetworkRoutingTable.
TEST(NetworkRoutingTableTest, GetOutputOptionsAndModifyPacket) {
  omnetpp::cStaticFlag dummy;
  omnetpp::cSimulation* dummy_simulator = SetupDummyOmnestSimulation();

  const std::string input_path = absl::StrCat(testing::TempDir(), "input_path");
  if (!std::filesystem::exists(input_path)) {
    ASSERT_TRUE(std::filesystem::create_directories(input_path));
  }
  const std::string input_file =
      absl::StrCat(input_path, "/routing_0.recordio");
  ASSERT_OK(WriteTestRoutingConfigToFile(input_file));

  ASSERT_OK_AND_ASSIGN(auto network_routing_table,
                       NetworkRoutingTable::Create(input_file));

  // Test for VRF 80.
  auto test_packet1 = GenerateTestInetPacket();
  TableOutputOptions expected_table_output_options1{{14, 0}, {15, 1}};
  ASSERT_OK_AND_ASSIGN(const auto* table_output_options1,
                       network_routing_table->GetOutputOptionsAndModifyPacket(
                           test_packet1.get()));
  TableOutputOptions test_table_output_options1(*table_output_options1);
  std::sort(test_table_output_options1.begin(),
            test_table_output_options1.end());
  EXPECT_EQ(test_table_output_options1, expected_table_output_options1);

  // Test for VRF 112.
  auto test_packet2 = GenerateTestInetPacket(
      kDefaultSrcIpv6Address, kTestDstIpv6Address2, kTestFlowSrcMacVrfDown);
  TableOutputOptions expected_table_output_options2{{24, 2}, {25, 3}};
  ASSERT_OK_AND_ASSIGN(const auto* table_output_options2,
                       network_routing_table->GetOutputOptionsAndModifyPacket(
                           test_packet2.get()));
  TableOutputOptions test_table_output_options2(*table_output_options2);
  std::sort(test_table_output_options2.begin(),
            test_table_output_options2.end());
  EXPECT_EQ(test_table_output_options2, expected_table_output_options2);

  // Test for NDv6.
  auto test_packet3 = GenerateTestInetPacket(kDefaultSrcIpv6Address,
                                             kTestDstIpv6AddressForHost1);
  TableOutputOptions expected_table_output_options3{
      {kDefaultWeightForNdv6OutputPort, 2}};
  ASSERT_OK_AND_ASSIGN(const auto* table_output_options3,
                       network_routing_table->GetOutputOptionsAndModifyPacket(
                           test_packet3.get()));
  TableOutputOptions test_table_output_options3(*table_output_options3);
  std::sort(test_table_output_options3.begin(),
            test_table_output_options3.end());
  EXPECT_EQ(test_table_output_options3, expected_table_output_options3);

  auto test_packet4 = GenerateTestInetPacket(kDefaultSrcIpv6Address,
                                             kTestDstIpv6AddressForHost2);
  TableOutputOptions expected_table_output_options4{{1, 3}};
  ASSERT_OK_AND_ASSIGN(const auto* table_output_options4,
                       network_routing_table->GetOutputOptionsAndModifyPacket(
                           test_packet4.get()));
  TableOutputOptions test_table_output_options4(*table_output_options4);
  std::sort(test_table_output_options4.begin(),
            test_table_output_options4.end());
  EXPECT_EQ(test_table_output_options4, expected_table_output_options4);

  // Test with unmatched case for net_util::IPTable.find_longest() for
  // src_mac_to_vrf_map_. There is no IPv6 address in ip_table matched in net
  // mask for kTestDstIpv6Address3.
  auto test_packet5 =
      GenerateTestInetPacket(kDefaultSrcIpv6Address, kTestDstIpv6Address3);
  EXPECT_FALSE(
      network_routing_table->GetOutputOptionsAndModifyPacket(test_packet5.get())
          .ok());

  CloseOmnestSimulation(dummy_simulator);
}

}  // namespace

// Test for function Create in class NetworkRoutingTable.
TEST(NetworkRoutingTableTest, Create) {
  const std::string input_path = absl::StrCat(testing::TempDir(), "input_path");
  if (!std::filesystem::exists(input_path)) {
    ASSERT_TRUE(std::filesystem::create_directories(input_path));
  }
  const std::string input_file =
      absl::StrCat(input_path, "/routing_0.recordio");
  ASSERT_OK(WriteTestRoutingConfigToFile(input_file));
  ASSERT_OK_AND_ASSIGN(auto network_routing_table,
                       NetworkRoutingTable::Create(input_file));

  // Test for src_mac_to_vrf_map_.
  EXPECT_EQ(network_routing_table->src_mac_to_vrf_map_.size(), 2);

  EXPECT_TRUE(network_routing_table->src_mac_to_vrf_map_.contains(
      kTestFlowSrcMacVrfUp));
  EXPECT_EQ(network_routing_table->src_mac_to_vrf_map_[kTestFlowSrcMacVrfUp],
            80);
  EXPECT_TRUE(network_routing_table->src_mac_to_vrf_map_.contains(
      kTestFlowSrcMacVrfDown));
  EXPECT_EQ(network_routing_table->src_mac_to_vrf_map_[kTestFlowSrcMacVrfDown],
            112);

  // Test for groups_.
  EXPECT_TRUE(network_routing_table->groups_.contains(12));
  const MacAddress group_src_mac(0x02, 0x00, 0x00, 0x00, 0x00, 0x00);
  EXPECT_EQ(network_routing_table->groups_[12].set_src_mac, group_src_mac);
  EXPECT_THAT(
      network_routing_table->groups_[12].output_options,
      testing::UnorderedElementsAre(OutputInfo(14, 0), OutputInfo(15, 1)));

  EXPECT_TRUE(network_routing_table->groups_.contains(22));
  EXPECT_EQ(network_routing_table->groups_[22].set_src_mac, group_src_mac);
  EXPECT_THAT(
      network_routing_table->groups_[22].output_options,
      testing::UnorderedElementsAre(OutputInfo(24, 2), OutputInfo(25, 3)));
}

}  // namespace isekai
