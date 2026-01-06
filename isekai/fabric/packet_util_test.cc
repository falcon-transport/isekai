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

#include "isekai/fabric/packet_util.h"

#include <cstdint>
#include <memory>

#include "inet/networklayer/ipv6/Ipv6Header_m.h"
#include "inet/transportlayer/udp/UdpHeader_m.h"
#include "isekai/common/packet_m.h"
#include "isekai/fabric/constants.h"
#undef ETH_ALEN
#undef ETHER_TYPE_LEN
#undef ETHERTYPE_ARP
#include "absl/hash/hash.h"
#include "absl/strings/str_join.h"
#include "crc32c/crc32c.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "inet/common/ProtocolTag_m.h"
#include "inet/linklayer/ethernet/EtherFrame_m.h"
#include "inet/linklayer/ethernet/EtherPhyFrame_m.h"
#include "isekai/common/testing.h"
#include "isekai/fabric/routing_test_util.h"
#include "isekai/host/rnic/omnest_packet_builder.h"
#include "omnetpp/simtime.h"
#include "omnetpp/simtime_t.h"

namespace isekai {

// Test for method GenerateHashValueForPacket in class PacketUtil.
TEST(PacketUtilTest, TestGenerateHashValueForPacket) {
  // Refers to isekai/omnest_environment_test.cc.
  omnetpp::cStaticFlag dummy;
  omnetpp::cSimulation* dummy_simulator = SetupDummyOmnestSimulation();

  OmnestPacketBuilder packet_builder(
      /* host_module = */ nullptr,
      /* falcon = */ nullptr,
      /* env = */ nullptr,
      /* stats_collector = */ nullptr,
      /* ip_address = */ "", /* transmission_channel = */ nullptr,
      /* host_id = */ "");

  // The created ethernet_signal contains all layer headers.
  auto packet = packet_builder.CreateFalconPacket(
      /* src_ip_address = */ kDefaultSrcIpv6Address,
      /* dest_ip_address = */ kTestDstIpv6Address1, /* src_port = */ 100,
      /* dest_port = */ 1000);
  DecapsulateMacHeader(packet);
  auto packet_hash_value = GenerateHashValueForPacket(packet);

  // Calculates the expected hash value.
  std::string hash_input_str = absl::StrJoin(
      std::tuple{std::string(kDefaultSrcIpv6Address),
                 std::string(kTestDstIpv6Address1), std::string("ipv6(27)"),
                 100, 1000, kDefaultFlowLabel},
      "|");

  uint32_t expected_hash_value = crc32c::Crc32c(hash_input_str);
  EXPECT_EQ(packet_hash_value, expected_hash_value);

  delete packet;
  CloseOmnestSimulation(dummy_simulator);
}

TEST(PacketUtilTest, TestGetPacketIpProtocol) {
  omnetpp::cStaticFlag dummy;
  omnetpp::cSimulation* dummy_simulator = SetupDummyOmnestSimulation();

  OmnestPacketBuilder packet_builder(
      /* host_module = */ nullptr,
      /* falcon = */ nullptr,
      /* env = */ nullptr,
      /* stats_collector = */ nullptr,
      /* ip_address = */ "", /* transmission_channel = */ nullptr,
      /* host_id = */ "");

  auto packet = packet_builder.CreateFalconPacket(
      /* src_ip_address = */ kDefaultSrcIpv6Address,
      /* dest_ip_address = */ kTestDstIpv6Address1, /* src_port = */ 100,
      /* dest_port = */ 1000);
  DecapsulateMacHeader(packet);
  EXPECT_EQ(GetPacketIpProtocol(packet), "ipv6(27)");

  delete packet;
  CloseOmnestSimulation(dummy_simulator);
}

TEST(PacketUtilTest, TestGetPacketUdpPorts) {
  omnetpp::cStaticFlag dummy;
  omnetpp::cSimulation* dummy_simulator = SetupDummyOmnestSimulation();

  OmnestPacketBuilder packet_builder(
      /* host_module = */ nullptr,
      /* falcon = */ nullptr,
      /* env = */ nullptr,
      /* stats_collector = */ nullptr,
      /* ip_address = */ "", /* transmission_channel = */ nullptr,
      /* host_id = */ "");

  auto packet = packet_builder.CreateFalconPacket(
      /* src_ip_address = */ kDefaultSrcIpv6Address,
      /* dest_ip_address = */ kTestDstIpv6Address1, /* src_port = */ 100,
      /* dest_port = */ 1000);
  DecapsulateMacHeader(packet);
  EXPECT_EQ(GetPacketUdpSourcePort(packet), 100);
  EXPECT_EQ(GetPacketUdpDestinationPort(packet), 1000);

  delete packet;
  CloseOmnestSimulation(dummy_simulator);
}

TEST(PacketUtilTest, TestGetPacketIpAddresses) {
  omnetpp::cStaticFlag dummy;
  omnetpp::cSimulation* dummy_simulator = SetupDummyOmnestSimulation();

  OmnestPacketBuilder packet_builder(
      /* host_module = */ nullptr,
      /* falcon = */ nullptr,
      /* env = */ nullptr,
      /* stats_collector = */ nullptr,
      /* ip_address = */ "", /* transmission_channel = */ nullptr,
      /* host_id = */ "");

  auto packet = packet_builder.CreateFalconPacket(
      /* src_ip_address = */ kDefaultSrcIpv6Address,
      /* dest_ip_address = */ kTestDstIpv6Address1, /* src_port = */ 100,
      /* dest_port = */ 1000);
  DecapsulateMacHeader(packet);
  EXPECT_EQ(GetPacketSourceIp(packet), kDefaultSrcIpv6Address);
  EXPECT_EQ(GetPacketDestinationIp(packet), kTestDstIpv6Address1);

  delete packet;
  CloseOmnestSimulation(dummy_simulator);
}

TEST(PacketUtilTest, TestChangePacketTtl) {
  omnetpp::cStaticFlag dummy;
  omnetpp::cSimulation* dummy_simulator = SetupDummyOmnestSimulation();

  OmnestPacketBuilder packet_builder(
      /* host_module = */ nullptr,
      /* falcon = */ nullptr,
      /* env = */ nullptr,
      /* stats_collector = */ nullptr,
      /* ip_address = */ "", /* transmission_channel = */ nullptr,
      /* host_id = */ "");

  auto packet = packet_builder.CreateFalconPacket(
      /* src_ip_address = */ kDefaultSrcIpv6Address,
      /* dest_ip_address = */ kTestDstIpv6Address1, /* src_port = */ 100,
      /* dest_port = */ 1000);
  DecapsulateMacHeader(packet);
  auto original_ttl = GetPacketTtl(packet);
  EXPECT_OK(DecrementPacketTtl(packet));
  EXPECT_EQ(original_ttl - 1, GetPacketTtl(packet));

  // Decrements the TTL until reaching 0.
  for (int i = 0; i < kTtl - 1; i++) {
    EXPECT_OK(DecrementPacketTtl(packet));
  }
  EXPECT_FALSE(DecrementPacketTtl(packet).ok());

  delete packet;
  CloseOmnestSimulation(dummy_simulator);
}

TEST(PacketUtilTest, TestCommunicationBetweenHostAndRouter) {
  omnetpp::cStaticFlag dummy;
  omnetpp::cSimulation* dummy_simulator = SetupDummyOmnestSimulation();

  OmnestPacketBuilder packet_builder(
      /* host_module = */ nullptr,
      /* falcon = */ nullptr,
      /* env = */ nullptr,
      /* stats_collector = */ nullptr,
      /* ip_address = */ kTestDstIpv6Address1,
      /* transmission_channel = */ nullptr, /* host_id = */ "");

  // step1: Host generates the packet.
  auto packet = packet_builder.CreateFalconPacket(
      /* src_ip_address = */ kDefaultSrcIpv6Address,
      /* dest_ip_address = */ kTestDstIpv6Address1, /* src_port = */ 100,
      /* dest_port = */ 1000);
  // step2: Router decapsulates the packet.
  DecapsulateMacHeader(packet);
  // step3: Router modifies the packet.
  SetPacketSourceMac(packet, kTestFlowSrcMacVrfUp);
  // step4: Router encapsulates the packet and inserts the phy headers.
  EncapsulateMacHeader(packet);
  const auto& phy_header = inet::makeShared<inet::EthernetPhyHeader>();
  phy_header->setSrcMacFullDuplex(true);
  packet->insertAtFront(phy_header);
  auto old_packet_protocol_tag = packet->removeTag<inet::PacketProtocolTag>();
  packet->clearTags();
  auto new_packet_protocol_tag = packet->addTag<inet::PacketProtocolTag>();
  *new_packet_protocol_tag = *old_packet_protocol_tag;
  delete old_packet_protocol_tag;
  // step5: Host extracts FALCON content from the packet.
  packet_builder.RemoveFrameHeader(packet);
  packet_builder.RemoveIpv6Header(packet);
  packet_builder.RemoveUdpHeader(packet);
  const auto& falcon_content_received =
      packet->peekAtFront<FalconPacket>()->getFalconContent();
  auto falcon_packet_received =
      std::make_unique<Packet>(falcon_content_received);
  EXPECT_EQ(3, falcon_packet_received->metadata.traffic_class);

  delete packet;
  CloseOmnestSimulation(dummy_simulator);
}

TEST(PacketUtilTest, TestPfcFrame) {
  omnetpp::cStaticFlag dummy;
  omnetpp::cSimulation* dummy_simulator = SetupDummyOmnestSimulation();

  int16_t class_enable = 0;
  int16_t pause_uints = 10;

  auto pfc_frame = CreatePfcPacket(0, pause_uints);
  class_enable |= 1 << 0;
  EXPECT_TRUE(IsFlowControlPacket(pfc_frame));

  PfcFrameUpdate(pfc_frame, 2, pause_uints);
  class_enable |= 1 << 2;
  EXPECT_TRUE(IsFlowControlPacket(pfc_frame));

  PfcFrameUpdate(pfc_frame, 4, 0);
  class_enable |= 1 << 4;
  EXPECT_TRUE(IsFlowControlPacket(pfc_frame));

  auto inet_packet = omnetpp::check_and_cast<inet::Packet*>(pfc_frame);
  inet_packet->popAtFront<inet::EthernetMacHeader>();
  const auto& control_frame =
      inet_packet->peekAtFront<inet::EthernetPfcFrame>();

  EXPECT_EQ(control_frame->getClassEnable(), class_enable);
  EXPECT_EQ(control_frame->getTimeClass(0), pause_uints);
  EXPECT_EQ(control_frame->getTimeClass(2), pause_uints);
  EXPECT_EQ(control_frame->getTimeClass(4), 0);

  delete pfc_frame;
  CloseOmnestSimulation(dummy_simulator);
}

TEST(PacketUtilTest, TestEcnMark) {
  omnetpp::cStaticFlag dummy;
  omnetpp::cSimulation* dummy_simulator = SetupDummyOmnestSimulation();

  OmnestPacketBuilder packet_builder(
      /* host_module = */ nullptr,
      /* falcon = */ nullptr,
      /* env = */ nullptr,
      /* stats_collector = */ nullptr,
      /* ip_address = */ "",
      /* transmission_channel = */ nullptr, /* host_id = */ "");
  packet_builder.ecn_enabled_ = false;

  auto packet_0 = packet_builder.CreateFalconPacket(
      /* src_ip_address = */ kDefaultSrcIpv6Address,
      /* dest_ip_address = */ kTestDstIpv6Address1, /* src_port = */ 100,
      /* dest_port = */ 1000);
  DecapsulateMacHeader(packet_0);
  EXPECT_EQ(EcnCode::kNonEct, PacketGetEcn(packet_0));

  PacketSetEcn(packet_0, EcnCode::kCongestionEncountered);
  EXPECT_EQ(EcnCode::kCongestionEncountered, PacketGetEcn(packet_0));

  packet_builder.ecn_enabled_ = true;
  auto packet_1 = packet_builder.CreateFalconPacket(
      /* src_ip_address = */ kDefaultSrcIpv6Address,
      /* dest_ip_address = */ kTestDstIpv6Address1, /* src_port = */ 100,
      /* dest_port = */ 1000);
  DecapsulateMacHeader(packet_1);
  EXPECT_EQ(EcnCode::kEcnCapableTransport0, PacketGetEcn(packet_1));

  delete packet_0;
  delete packet_1;
  CloseOmnestSimulation(dummy_simulator);
}

namespace {

TEST(PacketUtilTest, TestPacketOccupancy) {
  omnetpp::cStaticFlag dummy;
  omnetpp::cSimulation* dummy_simulator = SetupDummyOmnestSimulation();

  const auto& falcon = inet::makeShared<FalconPacket>();
  falcon->setChunkLength(inet::units::values::b(1000));
  inet::Packet* packet = new inet::Packet("FALCON");
  packet->insertAtBack(falcon);
  EXPECT_EQ(
      static_cast<uint64_t>(std::ceil(static_cast<double>(1000) / kCellSize)),
      GetPacketOccupancy(packet));

  delete packet;
  CloseOmnestSimulation(dummy_simulator);
}

TEST(PacketUtilTest, TestIngressTag) {
  omnetpp::cStaticFlag dummy;
  omnetpp::cSimulation* dummy_simulator = SetupDummyOmnestSimulation();

  const auto& falcon = inet::makeShared<FalconPacket>();
  falcon->setChunkLength(inet::units::values::b(1000));
  inet::Packet* packet = new inet::Packet("FALCON");
  packet->insertAtBack(falcon);

  AddIngressTag(packet, 10, 10, 1, 2, 3);
  uint32_t port_id;
  int priority;
  uint64_t minimal_guarantee_occupancy;
  uint64_t shared_pool_occupancy;
  uint64_t headroom_occupancy;
  GetAndRemoveIngressTag(packet, port_id, priority, minimal_guarantee_occupancy,
                         shared_pool_occupancy, headroom_occupancy);
  EXPECT_EQ(port_id, 10);
  EXPECT_EQ(priority, 10);
  EXPECT_EQ(minimal_guarantee_occupancy, 1);
  EXPECT_EQ(shared_pool_occupancy, 2);
  EXPECT_EQ(headroom_occupancy, 3);

  delete packet;
  CloseOmnestSimulation(dummy_simulator);
}

TEST(PacketUtilTest, TestGetScid) {
  omnetpp::cStaticFlag dummy;
  omnetpp::cSimulation* dummy_simulator = SetupDummyOmnestSimulation();

  OmnestPacketBuilder packet_builder(
      /* host_module = */ nullptr,
      /* falcon = */ nullptr,
      /* env = */ nullptr,
      /* stats_collector = */ nullptr,
      /* ip_address = */ "", /* transmission_channel = */ nullptr,
      /* host_id = */ "");

  std::unique_ptr<inet::Packet> packet =
      packet_builder.CreatePacketWithUdpAndIpv6Headers(
          /* src_ip_address = */ kDefaultSrcIpv6Address,
          /* dest_ip_address = */ kTestDstIpv6Address1, /* src_port = */ 100,
          /* dest_port = */ 1000);
  EXPECT_EQ(GetFalconPacketScid(packet.get()), kDefaultScid);

  CloseOmnestSimulation(dummy_simulator);
}

}  // namespace

}  // namespace isekai
