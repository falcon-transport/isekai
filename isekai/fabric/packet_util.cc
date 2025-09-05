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

#include "isekai/common/packet_m.h"

// Undef the macros in mac_address.h that conflict with the definitions in INET.
#undef ETH_ALEN
#undef ETHER_TYPE_LEN
#undef ETHERTYPE_ARP

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "glog/logging.h"
#include "inet/common/IntrusivePtr.h"
#include "inet/common/Protocol.h"
#include "inet/common/ProtocolGroup.h"
#include "inet/common/ProtocolTag_m.h"
#include "inet/common/Ptr.h"
#include "inet/common/Units.h"
#include "inet/common/packet/Packet.h"
#include "inet/linklayer/common/EtherType_m.h"
#include "inet/linklayer/common/FcsMode_m.h"
#include "inet/linklayer/common/InterfaceTag_m.h"
#include "inet/linklayer/common/MacAddress.h"
#include "inet/linklayer/common/MacAddressTag_m.h"
#include "inet/linklayer/ethernet/EtherEncap.h"
#include "inet/linklayer/ethernet/EtherFrame_m.h"
#include "inet/linklayer/ethernet/EtherPhyFrame_m.h"
#include "inet/networklayer/common/L3Tools.h"
#include "inet/networklayer/contract/ipv6/Ipv6Address.h"
#include "inet/networklayer/ipv6/Ipv6Header_m.h"
#include "inet/transportlayer/udp/UdpHeader_m.h"
#include "isekai/common/common_util.h"  // IWYU pragma: keep
#include "isekai/common/hash_5_tuple_tag_m.h"
#include "isekai/common/net_address.h"
#include "isekai/common/packet.h"
#include "isekai/fabric/constants.h"
#include "isekai/fabric/ingress_packet_tag_m.h"
#include "isekai/host/falcon/falcon.h"
#include "isekai/host/rnic/omnest_packet_builder.h"
#include "omnetpp/checkandcast.h"
#include "omnetpp/cmessage.h"
#include "omnetpp/csimulation.h"
#include "omnetpp/simtime.h"
#include "omnetpp/simtime_t.h"

namespace isekai {
namespace {

// Gets the UDP header from the received packet (at L3).
auto GetUdpHeader(const omnetpp::cMessage* packet) {
  auto inet_packet = omnetpp::check_and_cast<const inet::Packet*>(packet);
  const auto& ipv6_header = inet_packet->peekAtFront<inet::Ipv6Header>();
  DCHECK(inet_packet->hasDataAt<inet::UdpHeader>(ipv6_header->getChunkLength()))
      << "no udp header.";
  const auto& udp_header =
      inet_packet->peekDataAt<inet::UdpHeader>(ipv6_header->getChunkLength());

  return udp_header;
}

// Gets the Falcon packet from the received packet (at L3).
auto GetFalconPacket(const omnetpp::cMessage* packet) {
  auto inet_packet = omnetpp::check_and_cast<const inet::Packet*>(packet);
  const auto& ipv6_header = inet_packet->peekAtFront<inet::Ipv6Header>();
  DCHECK(inet_packet->hasDataAt<inet::UdpHeader>(ipv6_header->getChunkLength()))
      << "no udp header.";
  const auto& udp_header =
      inet_packet->peekDataAt<inet::UdpHeader>(ipv6_header->getChunkLength());

  DCHECK(inet_packet->hasDataAt<FalconPacket>(ipv6_header->getChunkLength() +
                                              udp_header->getChunkLength()))
      << "no falcon packet.";
  return inet_packet->peekDataAt<FalconPacket>(ipv6_header->getChunkLength() +
                                               udp_header->getChunkLength());
}

}  // namespace

MacAddress GetPacketSourceMac(omnetpp::cMessage* packet) {
  auto inet_packet = omnetpp::check_and_cast<inet::Packet*>(packet);
  const auto& source_mac_address =
      inet_packet->getTag<inet::MacAddressInd>()->getSrcAddress();
  unsigned char bytes[6];
  source_mac_address.getAddressBytes(bytes);
  return MacAddress(bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5]);
}

MacAddress GetPacketDestinationMac(omnetpp::cMessage* packet) {
  auto inet_packet = omnetpp::check_and_cast<inet::Packet*>(packet);
  const auto& destination_mac_address =
      inet_packet->getTag<inet::MacAddressInd>()->getDestAddress();
  unsigned char bytes[6];
  destination_mac_address.getAddressBytes(bytes);
  return MacAddress(bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5]);
}

std::string GetPacketSourceIp(omnetpp::cMessage* packet) {
  auto inet_packet = omnetpp::check_and_cast<inet::Packet*>(packet);
  auto ipv6_header = inet_packet->peekAtFront<inet::Ipv6Header>();
  return ipv6_header->getSrcAddress().str();
}

std::string GetPacketDestinationIp(omnetpp::cMessage* packet) {
  auto inet_packet = omnetpp::check_and_cast<inet::Packet*>(packet);
  auto ipv6_header = inet_packet->peekAtFront<inet::Ipv6Header>();
  return ipv6_header->getDestAddress().str();
}

void SetPacketSourceMac(omnetpp::cMessage* packet,
                        const MacAddress& mac_address) {
  auto inet_packet = omnetpp::check_and_cast<inet::Packet*>(packet);
  auto mac_address_req = inet_packet->addTagIfAbsent<inet::MacAddressReq>();
  mac_address_req->setSrcAddress(
      inet::MacAddress(mac_address.ToString().c_str()));
}

void SetPacketDestinationMac(omnetpp::cMessage* packet,
                             const MacAddress& mac_address) {
  auto inet_packet = omnetpp::check_and_cast<inet::Packet*>(packet);
  auto mac_address_req = inet_packet->addTagIfAbsent<inet::MacAddressReq>();
  mac_address_req->setDestAddress(
      inet::MacAddress(mac_address.ToString().c_str()));
}

uint32_t GetPacketInputPortId(omnetpp::cMessage* packet) {
  auto inet_packet = omnetpp::check_and_cast<inet::Packet*>(packet);
  return inet_packet->getTag<inet::InterfaceInd>()->getInterfaceId();
}

std::string GetPacketIpProtocol(omnetpp::cMessage* packet) {
  auto inet_packet = omnetpp::check_and_cast<inet::Packet*>(packet);
  return inet_packet->getTag<inet::PacketProtocolTag>()->getProtocol()->str();
}

uint32_t GetPacketUdpSourcePort(const omnetpp::cMessage* packet) {
  return GetUdpHeader(packet)->getSourcePort();
}

uint32_t GetPacketUdpDestinationPort(const omnetpp::cMessage* packet) {
  return GetUdpHeader(packet)->getDestinationPort();
}

uint32_t GenerateHashValueForPacket(omnetpp::cMessage* packet) {
  // Fetch the 5-tuple hash from packet tag instead of computing it again.
  auto inet_packet = omnetpp::check_and_cast<inet::Packet*>(packet);
  auto hash = inet_packet->getTag<Hash5TupleTag>();
  return hash->getHash5tuple();
}

absl::Status DecrementPacketTtl(omnetpp::cMessage* packet) {
  if (GetPacketTtl(packet) == 0) {
    return absl::InternalError("Reach TTL limit.");
  }

  auto inet_packet = omnetpp::check_and_cast<inet::Packet*>(packet);
  const auto& new_ipv6_header =
      inet::removeNetworkProtocolHeader<inet::Ipv6Header>(inet_packet);
  new_ipv6_header->setHopLimit(new_ipv6_header->getHopLimit() - 1);
  inet::insertNetworkProtocolHeader(inet_packet, inet::Protocol::ipv6,
                                    new_ipv6_header);
  return absl::OkStatus();
}

int GetPacketTtl(omnetpp::cMessage* packet) {
  auto inet_packet = omnetpp::check_and_cast<inet::Packet*>(packet);
  auto ipv6_header = inet_packet->peekAtFront<inet::Ipv6Header>();
  return ipv6_header->getHopLimit();
}

omnetpp::cMessage* DecapsulateEthernetSignal(omnetpp::cMessage* packet) {
  auto ethernet_signal = omnetpp::check_and_cast<inet::EthernetSignal*>(packet);
  auto received_packet =
      omnetpp::check_and_cast<inet::Packet*>(ethernet_signal->decapsulate());
  delete ethernet_signal;
  return received_packet;
}

void RemovePhyHeader(omnetpp::cMessage* packet) {
  auto received_packet = omnetpp::check_and_cast<inet::Packet*>(packet);
  received_packet->popAtFront<inet::EthernetPhyHeader>();
}

omnetpp::cMessage* EncapsulateEthernetSignal(omnetpp::cMessage* packet) {
  const auto& phy_header = inet::makeShared<inet::EthernetPhyHeader>();
  phy_header->setSrcMacFullDuplex(true);

  auto inet_packet = omnetpp::check_and_cast<inet::Packet*>(packet);
  inet_packet->insertAtFront(phy_header);
  auto old_packet_protocol_tag =
      inet_packet->removeTagIfPresent<inet::PacketProtocolTag>();
  auto old_hash_tag = inet_packet->removeTagIfPresent<Hash5TupleTag>();
  inet_packet->clearTags();
  // We do not attach the PacketProtocolTag to PFC frame.
  if (old_packet_protocol_tag) {
    auto new_packet_protocol_tag =
        inet_packet->addTag<inet::PacketProtocolTag>();
    *new_packet_protocol_tag = *old_packet_protocol_tag;
    delete old_packet_protocol_tag;
  }
  if (old_hash_tag) {
    auto new_hash_tag = inet_packet->addTag<Hash5TupleTag>();
    *new_hash_tag = *old_hash_tag;
    delete old_hash_tag;
  }

  auto signal = new inet::EthernetSignal();
  signal->encapsulate(omnetpp::check_and_cast<inet::Packet*>(inet_packet));
  return signal;
}

void DecapsulateMacHeader(omnetpp::cMessage* packet) {
  inet::EtherEncap::decapsulateMacHeader(
      omnetpp::check_and_cast<inet::Packet*>(packet));
}

void EncapsulateMacHeader(omnetpp::cMessage* packet) {
  auto inet_packet = omnetpp::check_and_cast<inet::Packet*>(packet);
  // Sets the front and back offsets to 0 so that the packet can be encapsulated
  // properly.
  inet_packet->trim();
  int ethernet_type =
      inet::ProtocolGroup::ethertype.findProtocolNumber(&inet::Protocol::ipv6);
  auto mac_address_req = inet_packet->getTag<inet::MacAddressReq>();
  const auto& ethernet_header = inet::makeShared<inet::EthernetMacHeader>();
  ethernet_header->setSrc(mac_address_req->getSrcAddress());
  ethernet_header->setDest(mac_address_req->getDestAddress());
  ethernet_header->setTypeOrLength(ethernet_type);
  inet_packet->insertAtFront(ethernet_header);
  inet::EtherEncap::addPaddingAndFcs(inet_packet, inet::FCS_DECLARED_CORRECT);
}

bool IsFlowControlPacket(const omnetpp::cMessage* packet) {
  auto inet_packet = omnetpp::check_and_cast<const inet::Packet*>(packet);
  const auto& frame = inet_packet->peekAtFront<inet::EthernetMacHeader>();
  return (frame->getTypeOrLength() == inet::ETHERTYPE_FLOW_CONTROL) ? true
                                                                    : false;
}

omnetpp::cMessage* CreatePfcPacket(int priority, int16_t pause_units) {
  CHECK_LT(priority, kClassesOfService);
  int16_t class_enable = static_cast<int16_t>(1) << priority;

  auto pfc_packet = new inet::Packet("PFC");
  const auto& frame = inet::makeShared<inet::EthernetPfcFrame>();
  const auto& mac_header = inet::makeShared<inet::EthernetMacHeader>();
  mac_header->setDest(inet::MacAddress::MULTICAST_PAUSE_ADDRESS);

  frame->setClassEnable(class_enable);
  frame->setTimeClass(priority, pause_units);

  pfc_packet->insertAtFront(frame);
  mac_header->setTypeOrLength(inet::ETHERTYPE_FLOW_CONTROL);
  pfc_packet->insertAtFront(mac_header);
  inet::EtherEncap::addPaddingAndFcs(pfc_packet, inet::FCS_DECLARED_CORRECT);

  return pfc_packet;
}

void PfcFrameUpdate(omnetpp::cMessage* pfc_frame, int priority,
                    int16_t pause_units) {
  auto inet_packet = omnetpp::check_and_cast<inet::Packet*>(pfc_frame);
  inet_packet->popAtFront<inet::EthernetMacHeader>();
  auto existing_frame_header =
      inet_packet->popAtFront<inet::EthernetPfcFrame>();
  // Creates new pfc frame header.
  const auto& new_frame_header = inet::makeShared<inet::EthernetPfcFrame>();
  // Updates the class_enable.
  int16_t class_enable = existing_frame_header->getClassEnable();
  class_enable |= 1 << priority;
  new_frame_header->setClassEnable(class_enable);
  // Updates the class time.
  for (int i = 0; i < existing_frame_header->getTimeClassArraySize(); i++) {
    new_frame_header->setTimeClass(i, existing_frame_header->getTimeClass(i));
  }
  new_frame_header->setTimeClass(priority, pause_units);
  // Installs the new pfc frame header.
  inet_packet->trim();
  inet_packet->insertAtFront(new_frame_header);
  // Reinstalls the mac header.
  const auto& mac_header = inet::makeShared<inet::EthernetMacHeader>();
  mac_header->setDest(inet::MacAddress::MULTICAST_PAUSE_ADDRESS);
  mac_header->setTypeOrLength(inet::ETHERTYPE_FLOW_CONTROL);
  inet_packet->insertAtFront(mac_header);
}

EcnCode PacketGetEcn(omnetpp::cMessage* packet) {
  auto inet_packet = omnetpp::check_and_cast<inet::Packet*>(packet);
  const auto& ipv6_header = inet_packet->peekAtFront<inet::Ipv6Header>();
  return static_cast<EcnCode>(ipv6_header->getExplicitCongestionNotification());
}

void PacketSetEcn(omnetpp::cMessage* packet, EcnCode ecn_code) {
  auto inet_packet = omnetpp::check_and_cast<inet::Packet*>(packet);
  const auto& new_ipv6_header =
      inet::removeNetworkProtocolHeader<inet::Ipv6Header>(inet_packet);
  new_ipv6_header->setExplicitCongestionNotification(
      static_cast<int16_t>(ecn_code));
  inet::insertNetworkProtocolHeader(inet_packet, inet::Protocol::ipv6,
                                    new_ipv6_header);
}

uint64_t GetPacketOccupancy(const omnetpp::cMessage* packet) {
  auto inet_packet = omnetpp::check_and_cast<const inet::Packet*>(packet);
  return static_cast<uint64_t>(
      std::ceil(static_cast<double>(inet_packet->getBitLength()) / kCellSize));
}

void AddIngressTag(omnetpp::cMessage* packet, uint32_t port_id, int priority,
                   uint64_t minimal_guarantee_occupancy,
                   uint64_t shared_pool_occupancy,
                   uint64_t headroom_occupancy) {
  auto inet_packet = omnetpp::check_and_cast<inet::Packet*>(packet);
  auto ingress_tag = inet_packet->addTag<IngressPacketTag>();
  ingress_tag->setPortId(port_id);
  ingress_tag->setPriority(priority);
  ingress_tag->setMinimalGuaranteeOccupancy(minimal_guarantee_occupancy);
  ingress_tag->setSharedPoolOccupancy(shared_pool_occupancy);
  ingress_tag->setHeadroomOccupancy(headroom_occupancy);
}

void GetAndRemoveIngressTag(omnetpp::cMessage* packet, uint32_t& port_id,
                            int& priority,
                            uint64_t& minimal_guarantee_occupancy,
                            uint64_t& shared_pool_occupancy,
                            uint64_t& headroom_occupancy) {
  auto inet_packet = omnetpp::check_and_cast<inet::Packet*>(packet);
  auto ingress_tag = inet_packet->removeTagIfPresent<IngressPacketTag>();
  port_id = ingress_tag->getPortId();
  priority = ingress_tag->getPriority();
  minimal_guarantee_occupancy = ingress_tag->getMinimalGuaranteeOccupancy();
  shared_pool_occupancy = ingress_tag->getSharedPoolOccupancy();
  headroom_occupancy = ingress_tag->getHeadroomOccupancy();
  delete ingress_tag;
}

uint32_t GetPacketIngressPort(omnetpp::cMessage* packet) {
  auto inet_packet = omnetpp::check_and_cast<inet::Packet*>(packet);
  return inet_packet->getTag<IngressPacketTag>()->getPortId();
}

uint32_t GetPacketIngressPriority(omnetpp::cMessage* packet) {
  auto inet_packet = omnetpp::check_and_cast<inet::Packet*>(packet);
  return inet_packet->getTag<IngressPacketTag>()->getPriority();
}

int GetPacketPriority(const omnetpp::cMessage* packet) { return 0; }

uint32_t GetFalconPacketScid(const omnetpp::cMessage* packet) {
  auto falcon_packet = GetFalconPacket(packet);
  return falcon_packet->getFalconContent().metadata.scid;
}
uint32_t GetFalconPacketDestCid(const omnetpp::cMessage* packet) {
  auto falcon_packet = GetFalconPacket(packet);
  return falcon_packet->getFalconContent().falcon.dest_cid;
}
bool IsFalconAck(const omnetpp::cMessage* packet) {
  auto falcon_packet = GetFalconPacket(packet);
  return falcon_packet->getFalconContent().packet_type ==
         falcon::PacketType::kAck;
}

uint32_t GetPacketFlowLabel(const omnetpp::cMessage* packet) {
  return GetFalconPacket(packet)->getFalconContent().metadata.flow_label;
}

absl::StatusOr<uint32_t> GetPacketStaticRoutingPort(
    const omnetpp::cMessage* packet) {
  const uint32_t static_routing_port_index =
      GetFalconPacket(packet)
          ->getFalconContent()
          .metadata.static_route.current_port_index;
  auto static_routing_port_list = GetFalconPacket(packet)
                                      ->getFalconContent()
                                      .metadata.static_route.port_list;
  if (!static_routing_port_list.has_value()) {
    return absl::NotFoundError("No static routing port specified.");
  }
  if (static_routing_port_list.value().size() <= static_routing_port_index) {
    LOG(FATAL)
        << "The static routing port index should not be larger than the static "
           "routing port list. Please check your static routing list "
           "configuration.";
  }
  return static_routing_port_list.value()[static_routing_port_index];
}

void IncrementPacketStaticRoutingIndex(omnetpp::cMessage* packet) {
  inet::Ptr<const FalconPacket> falcon_packet = GetFalconPacket(packet);
  const isekai::Packet& falcon_content = falcon_packet->getFalconContent();
  isekai::Packet::Metadata& metadata =
      const_cast<isekai::Packet::Metadata&>(falcon_content.metadata);
  metadata.static_route.current_port_index++;
}

// Sets static port list to a Falcon packet. This function is only used for
// test.
void SetFalconPacketStaticPortListForTesting(omnetpp::cMessage* packet,
                                             std::vector<uint32_t> port_list) {
  inet::Ptr<const FalconPacket> falcon_packet = GetFalconPacket(packet);
  const isekai::Packet& falcon_content = falcon_packet->getFalconContent();
  isekai::Packet::Metadata& metadata =
      const_cast<isekai::Packet::Metadata&>(falcon_content.metadata);
  metadata.static_route.port_list = port_list;
  metadata.static_route.current_port_index = 0;
}

}  // namespace isekai
