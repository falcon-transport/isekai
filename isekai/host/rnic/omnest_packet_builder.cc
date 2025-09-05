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

#include "isekai/host/rnic/omnest_packet_builder.h"

#include <stddef.h>
#include <sys/types.h>

#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <queue>
#include <random>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/string_view.h"
#include "absl/strings/substitute.h"
#include "absl/time/time.h"
#include "crc32c/crc32c.h"
#include "glog/logging.h"
#include "inet/common/IntrusivePtr.h"
#include "isekai/common/common_util.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/constants.h"
#include "isekai/common/default_config_generator.h"
#include "isekai/common/environment.h"
#include "isekai/common/file_util.h"
#include "isekai/common/hash_5_tuple_tag_m.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/common/packet.h"
#include "isekai/common/packet_m.h"
#include "isekai/fabric/constants.h"
#include "isekai/fabric/memory_management_config.pb.h"
#include "isekai/host/falcon/falcon.h"
#include "isekai/host/falcon/falcon_histograms.h"
#include "isekai/host/rnic/network_channel_manager.h"
#include "omnetpp/cdataratechannel.h"
#include "omnetpp/csimplemodule.h"
#include "omnetpp/simtime.h"
#include "omnetpp/simtime_t.h"

// The macros below are conflicted with those defined in OMNest. To solve the
// problem, we have to undefine them.
#undef ETHERTYPE_ARP
#undef ETH_ALEN
#undef ETHER_TYPE_LEN
#include "inet/common/Protocol.h"
#include "inet/common/ProtocolGroup.h"
#include "inet/common/ProtocolTag_m.h"
#include "inet/common/Ptr.h"
#include "inet/common/Units.h"
#include "inet/common/packet/Packet.h"
#include "inet/common/packet/chunk/Chunk.h"
#include "inet/linklayer/common/EtherType_m.h"
#include "inet/linklayer/common/FcsMode_m.h"
#include "inet/linklayer/common/MacAddressTag_m.h"
#include "inet/linklayer/ethernet/EtherEncap.h"
#include "inet/linklayer/ethernet/EtherFrame_m.h"
#include "inet/linklayer/ethernet/EtherPhyFrame_m.h"
#include "inet/networklayer/common/IpProtocolId_m.h"
#include "inet/networklayer/common/L3Address.h"
#include "inet/networklayer/common/L3AddressTag_m.h"
#include "inet/networklayer/common/L3Tools.h"
#include "inet/networklayer/contract/ipv6/Ipv6Address.h"
#include "inet/networklayer/ipv6/Ipv6Header_m.h"
#include "inet/transportlayer/common/CrcMode_m.h"
#include "inet/transportlayer/common/L4Tools.h"
#include "inet/transportlayer/udp/UdpHeader_m.h"
#include "isekai/common/ecn_mark_tag_m.h"
#include "isekai/common/status_util.h"
#include "omnetpp/checkandcast.h"
#include "omnetpp/cmessage.h"
#include "omnetpp/cpacket.h"
#include "omnetpp/csimulation.h"

namespace isekai {
namespace {

// Flag: enable_scalar_packet_delay
constexpr std::string_view kStatScalarRxPacketDelay =
    "packet_builder.scalar_rx_packet_delay";
constexpr std::string_view kStatScalarTxPacketDelay =
    "packet_builder.scalar_tx_packet_delay";

// Flag: enable_vector_packet_delay
constexpr std::string_view kStatVectorRxPacketDelay =
    "packet_builder.rx_packet_delay";
constexpr std::string_view kStatVectorTxPacketDelay =
    "packet_builder.tx_packet_delay";

// Flag: enable_queue_length
constexpr std::string_view kStatVectorTxQueueLength =
    "packet_builder.priority$0.tx_queue_length";

// Flag: enable_discard_and_drops
constexpr std::string_view kStatScalarWrongDestinationIpAddressDiscards =
    "packet_builder.wrong_destination_ip_address_discards";
constexpr std::string_view kStatScalarWrongOutgoingPacketSizeDiscards =
    "packet_builder.wrong_outgoing_packet_size_discards";
constexpr std::string_view kStatScalarRandomDrops =
    "packet_builder.random_drops";
constexpr std::string_view kStatScalarHostAggregatedDiscards =
    "packet_builder.total_discards";

// Flag: enable_scalar_tx_rx_packets_bytes
constexpr std::string_view kStatScalarTxPackets =
    "packet_builder.total_tx_packets";
constexpr std::string_view kStatScalarRxPackets =
    "packet_builder.total_rx_packets";
constexpr std::string_view kStatScalarTxBytes = "packet_builder.total_tx_bytes";
constexpr std::string_view kStatScalarRxBytes = "packet_builder.total_rx_bytes";

// Flag: enable_vector_tx_rx_bytes
constexpr std::string_view kStatVectorTxBytes = "packet_builder.tx_bytes";
constexpr std::string_view kStatVectorRxBytes = "packet_builder.rx_bytes";

// Flag: enable_pfc
constexpr std::string_view kStatVectorPfcPauseStart =
    "packet_builder.priority$0.pfc_pause_start";
constexpr std::string_view kStatVectorPfcPauseEnd =
    "packet_builder.priority$0.pfc_pause_end";

// Flag: enable_xoff_duration
constexpr std::string_view kStatScalarTotalXoffFalcon =
    "packet_builder.total_xoff_duration";

uint32_t GenerateHashValueForPacket(std::string_view src_ip,
                                    std::string_view dst_ip, uint32_t src_port,
                                    uint32_t dst_port, uint32_t flow_label) {
  // Adds packet source and destination ip addresses, packet ip protocol, and
  // packet L4 source and destination ports to the input of the hash function
  // for packet in WCMP. Uses "|" to separate the 5 components of the hash input
  // string. The ip_proto string is the one returned by GetPacketIpProtocol(..)
  // in fabric/packet_util.cc. Since we only use ipv6 in our simulations, it is
  // okay to hardcode it here.
  std::string hash_input_str =
      absl::StrJoin(std::tuple{src_ip, dst_ip, /*ip_proto*/ "ipv6(27)",
                               src_port, dst_port, flow_label},
                    "|");
  return crc32c::Crc32c(hash_input_str);
}

}  // namespace

StatisticsCollectionConfig::PacketBuilderFlags
    OmnestPacketBuilder::stats_collection_flags_;

uint16_t OmnestPacketBuilder::GetHashedPort(absl::string_view str) const {
  uint16_t remainder = 0xFFFF;
  for (const char& c : str) {
    remainder ^= c;
    for (int j = 0; j < 8; j++) {
      if (remainder & 0x0001) {
        remainder = (remainder >> 1) ^ 0x8408;
      } else {
        remainder = remainder >> 1;
      }
    }
  }
  return remainder;
}

// Get the FALCON packet out of a built packet with headers of all layers (from
// PHY to FALCON).
inet::IntrusivePtr<const FalconPacket> OmnestPacketBuilder::GetFalconPacket(
    const omnetpp::cMessage* packet) const {
  auto inet_packet = omnetpp::check_and_cast<const inet::Packet*>(packet);
  // Phy header
  const auto& phy_header = inet_packet->peekAtFront<inet::EthernetPhyHeader>();
  auto header_len = phy_header->getChunkLength();

  // ETH header
  DCHECK(inet_packet->hasDataAt<inet::EthernetMacHeader>(header_len))
      << "no eth header.";
  const auto& ethernet_header =
      inet_packet->peekDataAt<inet::EthernetMacHeader>(header_len);
  header_len += ethernet_header->getChunkLength();

  // IP header
  DCHECK(inet_packet->hasDataAt<inet::Ipv6Header>(header_len))
      << "no ipv6 header.";
  const auto& ipv6_header =
      inet_packet->peekDataAt<inet::Ipv6Header>(header_len);
  header_len += ipv6_header->getChunkLength();

  // UDP header
  DCHECK(inet_packet->hasDataAt<inet::UdpHeader>(header_len))
      << "no udp header.";
  const auto& udp_header = inet_packet->peekDataAt<inet::UdpHeader>(header_len);
  header_len += udp_header->getChunkLength();
  // FALCON packet
  DCHECK(inet_packet->hasDataAt<FalconPacket>(header_len))
      << "no falcon packet.";
  return inet_packet->peekDataAt<FalconPacket>(header_len);
}

OmnestPacketBuilder::OmnestPacketBuilder(
    omnetpp::cSimpleModule* host_module, FalconInterface* falcon,
    Environment* env, StatisticCollectionInterface* stats_collector,
    const std::string& ip_address,
    omnetpp::cDatarateChannel* transmission_channel, const std::string& host_id)
    : host_module_(host_module),
      env_(env),
      stats_collection_(stats_collector),
      source_ip_address_(ip_address),
      transmission_channel_(transmission_channel),
      packet_builder_queue_states_(kClassesOfService,
                                   PacketBuilderQueueState::kIdle),
      tx_queue_unpause_times_(kClassesOfService, absl::ZeroDuration()),
      host_id_(host_id),
      pfc_pause_start_ids_(kClassesOfService, 0),
      pfc_pause_end_ids_(kClassesOfService, 0),
      network_channel_manager_(
          std::make_unique<NetworkChannelManager>(host_module, env)) {
  // The default value for transport_ might be optimized away, so manually sets
  // it up.
  transport_ = static_cast<FalconInterface*>(nullptr);
  if (falcon != nullptr) transport_ = falcon;
  if (host_module_) {
    //
    // like support_pfc_. Eventually the NIC and router will share the MMU code,
    // in which all the parameters are configured in an unified manner.
    isekai::SimulationConfig simulation;
    std::string simulation_config_file =
        host_module_->getSystemModule()->par("simulation_config").stringValue();
    CHECK(ReadTextProtoFromFile(simulation_config_file, &simulation).ok());
    auto host_config = GetHostConfigProfile(host_id_, simulation.network());
    if (host_config.has_mmu_config()) {
      if (host_config.mmu_config().has_pfc_config()) {
        support_pfc_ = true;
        if (host_config.mmu_config().pfc_config().has_rx_pfc_delay()) {
          rx_pfc_delay_ = absl::Nanoseconds(
              host_config.mmu_config().pfc_config().rx_pfc_delay());
        }
      } else {
        support_pfc_ = false;
      }
    }
    // Calculate the maximum allowed network MTU from RDMA MTU.
    if (host_config.has_rdma_configuration() &&
        host_config.rdma_configuration().has_mtu()) {
      mtu_size_ = host_config.rdma_configuration().mtu() + kEthernetHeader +
                  kIpv6Header + kUdpHeader + kPspHeader + kFalconHeader +
                  kFalconOpHeader + kRdmaHeader + kPspTrailer +
                  kEthernetPreambleFCS;
    }
    if (host_config.has_packet_builder_configuration()) {
      if (host_config.packet_builder_configuration()
              .has_packet_rate_million_pps()) {
        packet_rate_mpps_ = host_config.packet_builder_configuration()
                                .packet_rate_million_pps();
        if (packet_rate_mpps_ == 0) {
          packet_build_interval_ = absl::ZeroDuration();
        } else {
          packet_build_interval_ = absl::Microseconds(1.0 / packet_rate_mpps_);
        }
      }
      if (host_config.packet_builder_configuration()
              .has_tx_queue_length_threshold()) {
        tx_queue_length_threshold_ = host_config.packet_builder_configuration()
                                         .tx_queue_length_threshold();
      }
      if (host_config.packet_builder_configuration()
              .has_random_drop_probability()) {
        SetRandomDropProbability(host_config.packet_builder_configuration()
                                     .random_drop_probability());
      }
      if (host_config.packet_builder_configuration()
              .has_random_drop_burst_size()) {
        drop_burst_size_ =
            host_config.packet_builder_configuration().random_drop_burst_size();
      }
      if (host_config.packet_builder_configuration().has_random_drop_ack()) {
        drop_ack_ =
            host_config.packet_builder_configuration().random_drop_ack();
      }
      if (host_config.packet_builder_configuration()
              .has_record_t1_after_packet_builder()) {
        record_t1_after_packet_builder_ =
            host_config.packet_builder_configuration()
                .record_t1_after_packet_builder();
      }
    }
  }

  // Initializes the statistics.
  if (stats_collection_ &&
      stats_collection_->GetConfig().has_packet_builder_flags()) {
    stats_collection_flags_ =
        stats_collection_->GetConfig().packet_builder_flags();
  } else {
    stats_collection_flags_ =
        DefaultConfigGenerator::DefaultPacketBuilderStatsFlags();
  }

  if (stats_collection_ && stats_collection_flags_.enable_discard_and_drops()) {
    CHECK_OK(stats_collection_->UpdateStatistic(
        kStatScalarWrongDestinationIpAddressDiscards,
        wrong_destination_ip_address_discards_,
        StatisticsCollectionConfig::SCALAR_MAX_STAT));
    CHECK_OK(stats_collection_->UpdateStatistic(
        kStatScalarWrongOutgoingPacketSizeDiscards,
        wrong_outgoing_packet_size_discards_,
        StatisticsCollectionConfig::SCALAR_MAX_STAT));
    CHECK_OK(stats_collection_->UpdateStatistic(
        kStatScalarHostAggregatedDiscards, total_discards_,
        StatisticsCollectionConfig::SCALAR_MAX_STAT));
  }
  if (stats_collection_ &&
      stats_collection_flags_.enable_scalar_tx_rx_packets_bytes()) {
    CHECK_OK(stats_collection_->UpdateStatistic(
        kStatScalarTxPackets, cumulative_tx_packets_,
        StatisticsCollectionConfig::SCALAR_MAX_STAT));
    CHECK_OK(stats_collection_->UpdateStatistic(
        kStatScalarRxPackets, cumulative_rx_packets_,
        StatisticsCollectionConfig::SCALAR_MAX_STAT));
    CHECK_OK(stats_collection_->UpdateStatistic(
        kStatScalarTxBytes, cumulative_tx_bytes_,
        StatisticsCollectionConfig::SCALAR_MAX_STAT));
    CHECK_OK(stats_collection_->UpdateStatistic(
        kStatScalarRxBytes, cumulative_rx_bytes_,
        StatisticsCollectionConfig::SCALAR_MAX_STAT));
  }
}

void OmnestPacketBuilder::SetFalconFlowControl(bool xoff) {
  if (auto falcon = std::get_if<FalconInterface*>(&transport_)) {
    (*falcon)->SetXoffByPacketBuilder(xoff);

    if (!xoff) {
      (*falcon)->get_histogram_collector()->Add(
          XoffTypes::kFalcon,
          absl::ToDoubleNanoseconds(env_->ElapsedTime() -
                                    falcon_xoff_triggered_time_));
    }
  }
}

void OmnestPacketBuilder::EnqueuePacket(std::unique_ptr<Packet> packet) {
  // Records the enqueue time in nanoseconds.
  packet->metadata.timestamp = env_->ElapsedTime() / absl::Nanoseconds(1);
  //
  // the priority by packet->metadata.traffic_class. For now, we only support
  // one tx queue, so use the fixed priority (0).
  int priority = 0;
  //
  tx_queue_.push(std::move(packet));
  // The TX queue threshold is crossed, PB should trigger Xoff in FALCON.
  if (tx_queue_.size() == tx_queue_length_threshold_) {
    SetFalconFlowControl(true);
    falcon_xoff_triggered_time_ = env_->ElapsedTime();
  }

  // Updates the queue length after enqueueing a packet.
  if (stats_collection_flags_.enable_queue_length()) {
    CHECK_OK(stats_collection_->UpdateStatistic(
        absl::Substitute(kStatVectorTxQueueLength, priority), tx_queue_.size(),
        StatisticsCollectionConfig::TIME_SERIES_STAT));
  }

  if (packet_builder_queue_states_[priority] ==
      PacketBuilderQueueState::kIdle) {
    packet_builder_queue_states_[priority] = PacketBuilderQueueState::kActive;
    EncapsulateAndSendTransportPacket(priority);
  }
}

void OmnestPacketBuilder::EncapsulateAndSendTransportPacket(uint16_t priority) {
  // The packet is queued and will be transmitted when the corresponding Tx
  // queue is unpaused.
  if (packet_builder_queue_states_[priority] ==
      PacketBuilderQueueState::kPaused) {
    return;
  }

  //
  // and send packet.
  if (tx_queue_.empty()) {
    packet_builder_queue_states_[priority] = PacketBuilderQueueState::kIdle;
    return;
  }
  EncapsulateAndSendFalconPacket(priority);
}

void OmnestPacketBuilder::EncapsulateAndSendFalconPacket(uint16_t priority) {
  // Pops the FALCON packet from tx queue for processing.
  auto falcon_packet = std::move(tx_queue_.front());
  tx_queue_.pop();

  // Updates the queue length after dequeueing a packet.
  if (stats_collection_flags_.enable_queue_length()) {
    CHECK_OK(stats_collection_->UpdateStatistic(
        absl::Substitute(kStatVectorTxQueueLength, priority), tx_queue_.size(),
        StatisticsCollectionConfig::TIME_SERIES_STAT));
  }

  // TX queue length becomes smaller than the threshold, then sends Xon to
  // FALCON.
  if (tx_queue_.size() + 1 == tx_queue_length_threshold_) {
    SetFalconFlowControl(false);
    falcon_xoff_duration_us_ += absl::ToDoubleMicroseconds(
        env_->ElapsedTime() - falcon_xoff_triggered_time_);
    if (stats_collection_flags_.enable_xoff_duration()) {
      CHECK_OK(stats_collection_->UpdateStatistic(
          kStatScalarTotalXoffFalcon, falcon_xoff_duration_us_,
          StatisticsCollectionConfig::SCALAR_MAX_STAT));
    }
  }

  auto enqueue_time = falcon_packet->metadata.timestamp;
  // Randomly generates the UDP source port per connection.
  auto qp_ids = absl::StrCat(falcon_packet->metadata.rdma_src_qp_id, ":",
                             falcon_packet->rdma.dest_qp_id);
  uint16_t udp_src_port = GetHashedPort(qp_ids);
  // Consturcuts the OMNest FALCON packet.
  const auto& falcon = inet::makeShared<FalconPacket>();
  if (record_t1_after_packet_builder_) {
    falcon_packet->timestamps.sent_timestamp = env_->ElapsedTime();
  } else {
    falcon_packet->timestamps.sent_timestamp = absl::Nanoseconds(enqueue_time);
  }
  falcon->setFalconContent(*falcon_packet);
  falcon->setChunkLength(inet::units::values::B(
      GetFalconPacketLength(falcon_packet.get()) + PspHeaderLength +
      PspIntegrityChecksumValueLength));
  // Insert FALCON packet into a INET packet for transmission.
  inet::Packet* packet = new inet::Packet("FALCON");
  packet->insertAtBack(falcon);
  packet->setTimestamp(omnetpp::simtime_t(enqueue_time, omnetpp::SIMTIME_NS));

  inet::L3Address src_ip_address(source_ip_address_.c_str());
  inet::L3Address dest_ip_address(
      GetDestinationIpAddress(falcon_packet.get()).c_str());

  // The process is: 1.insert UDP header --> 2.insert IP header --> 3.insert
  // Eth frame header --> 4.send packets out.
  InsertUdpHeader(/*packet=*/packet, /*source_address=*/src_ip_address,
                  /*destination_address=*/dest_ip_address,
                  /*source_port=*/udp_src_port,
                  /*destination_port=*/kDefaultUdpPort);
  InsertIpv6Header(packet, 0);
  InsertFrameHeader(packet);

  // Compute and add 5-tuple hash tag to the packet.
  auto hash_tag = packet->addTag<Hash5TupleTag>();
  hash_tag->setHash5tuple(GenerateHashValueForPacket(
      source_ip_address_,
      falcon_packet->metadata.destination_ip_address.ToString(), udp_src_port,
      kDefaultUdpPort, falcon_packet->metadata.flow_label));

  absl::Duration next_build_duration = SendoutPacket(packet);

  // Schedule an event to process the next packet if there has any.
  CHECK_OK(env_->ScheduleEvent(next_build_duration, [this, priority]() {
    EncapsulateAndSendTransportPacket(priority);
  }));
}

void OmnestPacketBuilder::InsertUdpHeader(
    inet::Packet* packet, const inet::L3Address& source_address,
    const inet::L3Address& destination_address, unsigned int source_port,
    unsigned int destination_port) {
  auto ip_address_req = packet->addTagIfAbsent<inet::L3AddressReq>();
  ip_address_req->setSrcAddress(source_address);
  ip_address_req->setDestAddress(destination_address);

  auto udp_header = inet::makeShared<inet::UdpHeader>();
  udp_header->setSourcePort(source_port);
  udp_header->setDestinationPort(destination_port);
  udp_header->setTotalLengthField(udp_header->getChunkLength() +
                                  packet->getTotalLength());
  udp_header->setCrcMode(inet::CRC_DISABLED);
  udp_header->setCrc(0x0000);
  inet::insertTransportProtocolHeader(packet, inet::Protocol::udp, udp_header);
}

void OmnestPacketBuilder::InsertIpv6Header(inet::Packet* packet,
                                           uint16_t priority) {
  auto ipv6_header = inet::makeShared<inet::Ipv6Header>();
  inet::L3AddressReq* ip_addresses = packet->removeTag<inet::L3AddressReq>();
  inet::Ipv6Address src = ip_addresses->getSrcAddress().toIpv6();
  inet::Ipv6Address dest = ip_addresses->getDestAddress().toIpv6();
  delete ip_addresses;

  ipv6_header->setPayloadLength(packet->getDataLength());
  ipv6_header->setSrcAddress(src);
  ipv6_header->setDestAddress(dest);
  ipv6_header->setHopLimit(kTtl);
  ipv6_header->setProtocolId(static_cast<inet::IpProtocolId>(
      inet::ProtocolGroup::ipprotocol.getProtocolNumber(
          &inet::Protocol::ipv6)));
  ipv6_header->setExplicitCongestionNotification(
      static_cast<int16_t>(ecn_enabled_));
  ipv6_header->setFlowLabel(GetFlowLabelFromFalconMetadata(packet));

  ipv6_header->setChunkLength(
      inet::units::values::B(ipv6_header->calculateHeaderByteLength()));
  packet->trimFront();
  inet::insertNetworkProtocolHeader(packet, inet::Protocol::ipv6, ipv6_header);

  auto mac_address_req = packet->addTagIfAbsent<inet::MacAddressReq>();
  mac_address_req->setDestAddress(unique_mac_);
  mac_address_req->setSrcAddress(unique_mac_);

  packet->addTagIfAbsent<inet::PacketProtocolTag>()->setProtocol(
      &inet::Protocol::ipv6);
}

void OmnestPacketBuilder::InsertFrameHeader(inet::Packet* packet) {
  int ethernet_type =
      inet::ProtocolGroup::ethertype.findProtocolNumber(&inet::Protocol::ipv6);
  auto mac_address_req = packet->getTag<inet::MacAddressReq>();
  const auto& ethernet_header = inet::makeShared<inet::EthernetMacHeader>();
  ethernet_header->setSrc(mac_address_req->getSrcAddress());
  ethernet_header->setDest(mac_address_req->getDestAddress());
  ethernet_header->setTypeOrLength(ethernet_type);
  packet->insertAtFront(ethernet_header);
  inet::EtherEncap::addPaddingAndFcs(packet, inet::FCS_DECLARED_CORRECT);

  const auto& phy_header = inet::makeShared<inet::EthernetPhyHeader>();
  phy_header->setSrcMacFullDuplex(true);
  packet->insertAtFront(phy_header);

  auto old_packet_protocol_tag = packet->removeTag<inet::PacketProtocolTag>();
  packet->clearTags();
  auto new_packet_protocol_tag = packet->addTag<inet::PacketProtocolTag>();
  *new_packet_protocol_tag = *old_packet_protocol_tag;
  delete old_packet_protocol_tag;
}

absl::Duration OmnestPacketBuilder::SendoutPacket(omnetpp::cPacket* packet) {
  absl::Duration next_build_duration = packet_build_interval_;
  // Test random drop.
  if (IsRandomDrop(packet)) {
    // A new burst of drops.
    remaining_drop_in_burst_ += drop_burst_size_;
  }
  // If there is an ongoing drop burst.
  if (remaining_drop_in_burst_) {
    if (stats_collection_flags_.enable_discard_and_drops()) {
      CHECK_OK(stats_collection_->UpdateStatistic(
          kStatScalarRandomDrops, ++random_drops_,
          StatisticsCollectionConfig::SCALAR_MAX_STAT));
      CHECK_OK(stats_collection_->UpdateStatistic(
          kStatScalarHostAggregatedDiscards, ++total_discards_,
          StatisticsCollectionConfig::SCALAR_MAX_STAT));
    }
    remaining_drop_in_burst_--;
    delete packet;
    return next_build_duration;
  }
  // Records the transmission size.
  auto packet_size =
      omnetpp::check_and_cast<inet::Packet*>(packet)->getByteLength();
  // Checks against MTU size.
  if (packet_size > mtu_size_) {
    LOG(ERROR) << "Sending packet size is larger than MTU: " << packet_size;
    if (stats_collection_flags_.enable_discard_and_drops()) {
      CHECK_OK(stats_collection_->UpdateStatistic(
          kStatScalarWrongOutgoingPacketSizeDiscards,
          ++wrong_outgoing_packet_size_discards_,
          StatisticsCollectionConfig::SCALAR_MAX_STAT));
      CHECK_OK(stats_collection_->UpdateStatistic(
          kStatScalarHostAggregatedDiscards, ++total_discards_,
          StatisticsCollectionConfig::SCALAR_MAX_STAT));
    }
    delete packet;
    return next_build_duration;
  }

  std::optional<uint32_t> added_delay_ns;
  const auto falcon_packet = GetFalconPacket(packet);
  if (falcon_packet != nullptr &&
      std::get<FalconInterface*>(transport_) != nullptr) {
    const auto& falcon_content = falcon_packet->getFalconContent();
    added_delay_ns = falcon_content.metadata.added_delay_ns;
    if (stats_collection_flags_.enable_per_connection_traffic_stats()) {
      std::get<FalconInterface*>(transport_)
          ->UpdateTxBytes(std::make_unique<Packet>(falcon_content),
                          packet_size);
    }
  }
  cumulative_tx_bytes_ += packet_size;
  if (stats_collection_flags_.enable_scalar_tx_rx_packets_bytes()) {
    CHECK_OK(stats_collection_->UpdateStatistic(
        kStatScalarTxBytes, cumulative_tx_bytes_,
        StatisticsCollectionConfig::SCALAR_MAX_STAT));
    CHECK_OK(stats_collection_->UpdateStatistic(
        kStatScalarTxPackets, ++cumulative_tx_packets_,
        StatisticsCollectionConfig::SCALAR_MAX_STAT));
  }
  if (stats_collection_flags_.enable_vector_tx_rx_bytes()) {
    CHECK_OK(stats_collection_->UpdateStatistic(
        kStatVectorTxBytes, cumulative_tx_bytes_,
        StatisticsCollectionConfig::TIME_SERIES_STAT));
  }
  // Updates the packet queueing delay.
  UpdatePacketQueueingDelayStats(packet);

  if (host_module_) {
    // Converts the packet into OMNest ethernet signal and sends it out to the
    // network.
    auto signal = new inet::EthernetSignal();
    signal->encapsulate(packet);
    next_build_duration = GetNextPacketBuildEventTime(signal);
    absl::Duration send_delay = added_delay_ns.has_value()
                                    ? absl::Nanoseconds(added_delay_ns.value())
                                    : absl::ZeroDuration();
    network_channel_manager_->ScheduleSend(signal, send_delay,
                                           GetPacketTransmissionDelay(signal));
  } else {
    delete packet;
    packet = nullptr;
  }

  VLOG(2) << "---->" << host_id_
          << " sends packet @time: " << omnetpp::simTime().str();
  return next_build_duration;
}

void OmnestPacketBuilder::ExtractAndHandleTransportPacket(
    omnetpp::cMessage* network_packet) {
  auto ethernet_signal =
      omnetpp::check_and_cast<inet::EthernetSignal*>(network_packet);
  auto received_packet =
      omnetpp::check_and_cast<inet::Packet*>(ethernet_signal->decapsulate());
  delete ethernet_signal;

  auto received_packet_length = received_packet->getByteLength();
  cumulative_rx_bytes_ += received_packet_length;
  if (stats_collection_flags_.enable_scalar_tx_rx_packets_bytes()) {
    CHECK_OK(stats_collection_->UpdateStatistic(
        kStatScalarRxBytes, cumulative_rx_bytes_,
        StatisticsCollectionConfig::SCALAR_MAX_STAT));
  }
  if (stats_collection_flags_.enable_vector_tx_rx_bytes()) {
    CHECK_OK(stats_collection_->UpdateStatistic(
        kStatVectorRxBytes, cumulative_rx_bytes_,
        StatisticsCollectionConfig::TIME_SERIES_STAT));
  }

  // The process is: 1.remove frame header --> 2.remove ip header --> 3.remove
  // UDP header --> 4.extract falcon packet.
  if (RemoveFrameHeader(received_packet)) {
    uint8_t forward_hops =
        kTtl - received_packet->peekAtFront<inet::Ipv6Header>()->getHopLimit();
    if (RemoveIpv6Header(received_packet)) {
      uint32_t dst_port =
          received_packet->peekAtFront<inet::UdpHeader>()->getDestinationPort();
      if (RemoveUdpHeader(received_packet)) {
        {
          if (stats_collection_flags_.enable_per_connection_traffic_stats() &&
              std::get<FalconInterface*>(transport_) != nullptr) {
            const auto& falcon_packet =
                received_packet->peekAtFront<FalconPacket>();
            const auto& falcon_content = falcon_packet->getFalconContent();
            auto received_falcon_packet =
                std::make_unique<Packet>(falcon_content);
            std::get<FalconInterface*>(transport_)
                ->UpdateRxBytes(std::move(received_falcon_packet),
                                received_packet_length);
          }
          HandleFalconPacket(received_packet, forward_hops);
        }
      }
    }
  }

  delete received_packet;
}

bool OmnestPacketBuilder::RemoveFrameHeader(inet::Packet* packet) {
  auto phy_header = packet->popAtFront<inet::EthernetPhyHeader>();
  packet->addTagIfAbsent<inet::PacketProtocolTag>()->setProtocol(
      &inet::Protocol::ethernetMac);
  const auto& frame = packet->peekAtFront<inet::EthernetMacHeader>();
  if (frame->getTypeOrLength() != inet::ETHERTYPE_FLOW_CONTROL) {
    auto ethernet_header = inet::EtherEncap::decapsulateMacHeader(packet);
    if (inet::isEth2Header(*ethernet_header)) {
      return true;
    }
  } else {
    if (support_pfc_) {
      HandlePfc(packet);
    }
  }

  return false;
}

bool OmnestPacketBuilder::RemoveIpv6Header(inet::Packet* packet) {
  delete packet->removeControlInfo();
  auto ipv6_header = packet->popAtFront<inet::Ipv6Header>();
  auto ecn_code =
      static_cast<EcnCode>(ipv6_header->getExplicitCongestionNotification());
  auto ecn_mark_tag = packet->addTag<EcnMarkTag>();
  if (ecn_code == EcnCode::kCongestionEncountered) {
    ecn_mark_tag->setEcnMarked(true);
  } else {
    ecn_mark_tag->setEcnMarked(false);
  }

  // If receiving a packet whose destination ip does not match the ip address of
  // this FALCON host, drop the packet.
  if (ipv6_header->getDestinationAddress() !=
      inet::L3Address(source_ip_address_.c_str())) {
    if (stats_collection_flags_.enable_discard_and_drops()) {
      CHECK_OK(stats_collection_->UpdateStatistic(
          kStatScalarWrongDestinationIpAddressDiscards,
          ++wrong_destination_ip_address_discards_,
          StatisticsCollectionConfig::SCALAR_MAX_STAT));
      CHECK_OK(stats_collection_->UpdateStatistic(
          kStatScalarHostAggregatedDiscards, ++total_discards_,
          StatisticsCollectionConfig::SCALAR_MAX_STAT));
    }
    return false;
  }

  inet::units::values::B payload_length = ipv6_header->getPayloadLength();
  if (payload_length != inet::units::values::B(0) &&
      payload_length < packet->getDataLength())
    packet->setBackOffset(packet->getFrontOffset() + payload_length);

  return true;
}

bool OmnestPacketBuilder::RemoveUdpHeader(inet::Packet* packet) {
  delete packet->removeTagIfPresent<inet::PacketProtocolTag>();
  inet::units::values::b udp_header_pop_position = packet->getFrontOffset();

  // Uses try-catch here to filter non-udp packet
  // (e.g., ND packets from router supporting IPv6).
  try {
    auto udp_header = packet->popAtFront<inet::UdpHeader>(
        inet::units::values::b(-1), inet::Chunk::PF_ALLOW_INCORRECT);
    auto total_length =
        inet::units::values::B(udp_header->getTotalLengthField());

    if (total_length < packet->getDataLength())
      packet->setBackOffset(udp_header_pop_position + total_length);
  } catch (const std::exception& e) {
    LOG(INFO) << "Non-UDP packet received. " << e.what();
    return false;
  }

  return true;
}

void OmnestPacketBuilder::HandleFalconPacket(inet::Packet* packet,
                                             uint8_t forward_hops) {
  // Updates the Rx packet counter.
  if (stats_collection_flags_.enable_scalar_tx_rx_packets_bytes()) {
    CHECK_OK(stats_collection_->UpdateStatistic(
        kStatScalarRxPackets, ++cumulative_rx_packets_,
        StatisticsCollectionConfig::SCALAR_MAX_STAT));
  }

  const auto& falcon_packet = packet->peekAtFront<FalconPacket>();
  const auto& falcon_content = falcon_packet->getFalconContent();

  auto received_falcon_packet = std::make_unique<Packet>(falcon_content);
  received_falcon_packet->timestamps.received_timestamp = env_->ElapsedTime();
  // Updates the packet delay.
  auto delay = absl::ToDoubleSeconds(
      received_falcon_packet->timestamps.received_timestamp -
      received_falcon_packet->timestamps.sent_timestamp);
  if (stats_collection_flags_.enable_vector_packet_delay()) {
    CHECK_OK(stats_collection_->UpdateStatistic(
        kStatVectorRxPacketDelay, delay,
        StatisticsCollectionConfig::TIME_SERIES_STAT));
  }
  if (stats_collection_flags_.enable_scalar_packet_delay()) {
    CHECK_OK(stats_collection_->UpdateStatistic(
        kStatScalarRxPacketDelay, delay,
        StatisticsCollectionConfig::SCALAR_MEAN_STAT));
  }
  received_falcon_packet->metadata.forward_hops = forward_hops;
  std::get<FalconInterface*>(transport_)
      ->TransferRxPacket(std::move(received_falcon_packet));
}

void OmnestPacketBuilder::UpdatePacketQueueingDelayStats(
    const omnetpp::cMessage* msg) {
  const omnetpp::simtime_t& enqueue_time = msg->getTimestamp();
  omnetpp::simtime_t dequeue_time = omnetpp::simTime();
  omnetpp::simtime_t queueing_delay = dequeue_time - enqueue_time;
  if (stats_collection_flags_.enable_vector_packet_delay()) {
    CHECK_OK(stats_collection_->UpdateStatistic(
        kStatVectorTxPacketDelay, SIMTIME_DBL(queueing_delay),
        StatisticsCollectionConfig::TIME_SERIES_STAT));
  }
  if (stats_collection_flags_.enable_scalar_packet_delay()) {
    CHECK_OK(stats_collection_->UpdateStatistic(
        kStatScalarTxPacketDelay, SIMTIME_DBL(queueing_delay),
        StatisticsCollectionConfig::SCALAR_MEAN_STAT));
  }
}

size_t OmnestPacketBuilder::GetFalconPacketLength(
    const Packet* falcon_packet) const {
  size_t packet_length;
  switch (falcon_packet->packet_type) {
    case falcon::PacketType::kPullRequest:
      // There are 16 reserved bits in FALCON pull request, push request, push
      // grant, and push unsolicited packet headers.
      packet_length = kFalconBaseHeaderSize + kFalconRequestLength +
                      kFalconGniMetadataSize + kFalconTransactionReservedBits;
      break;
    case falcon::PacketType::kPushRequest:
      packet_length = kFalconBaseHeaderSize + kFalconRequestLength +
                      kFalconVendorDefinedSize + kFalconTransactionReservedBits;
      break;
    case falcon::PacketType::kPushGrant:
      packet_length = kFalconBaseHeaderSize + kFalconRequestLength +
                      kFalconGniMetadataSize + kFalconVendorDefinedSize +
                      kFalconTransactionReservedBits;
      break;
    case falcon::PacketType::kPullData:
    case falcon::PacketType::kPushSolicitedData:
      packet_length = kFalconBaseHeaderSize;
      break;
    case falcon::PacketType::kPushUnsolicitedData:
      packet_length = kFalconBaseHeaderSize + kFalconRequestLength +
                      kFalconTransactionReservedBits;
      break;
    case falcon::PacketType::kResync:
      // There are 20 reserved bits in FALCON resync packet header.
      packet_length = kFalconBaseHeaderSize + kFalconResyncCodeSize +
                      kFalconResyncPacketTypeSize + kFalconVendorDefinedSize +
                      kFalconResyncReservedBits;
      break;
    case falcon::PacketType::kAck:
      if (falcon_packet->ack.ack_type == Packet::Ack::kEack) {
        packet_length = kFalconEackSize;
      } else {
        packet_length = kFalconAckSize;
      }
      break;
    case falcon::PacketType::kNack:
      packet_length = kFalconNackSize;
      break;
    case falcon::PacketType::kBack:
    case falcon::PacketType::kEack:
      //
      LOG(FATAL) << "Isekai doesn't use EACK/BACK packet type yet.";
      break;
    case falcon::PacketType::kInvalid:
      LOG(FATAL) << "unset FALCON packet type.";
  }
  // The payload_length carries the RDMA header length for each FALCON
  // transaction.
  return (packet_length + falcon_packet->falcon.payload_length * 8) / 8;
}

std::unique_ptr<inet::Packet>
OmnestPacketBuilder::CreatePacketWithUdpAndIpv6Headers(
    const std::string& src_ip_address, const std::string& dest_ip_address,
    uint32_t src_port, uint32_t dest_port, uint32_t flow_label) {
  // Creates falcon_packet.
  auto falcon_packet = std::make_unique<Packet>();
  falcon_packet->metadata.scid = kDefaultScid;
  falcon_packet->metadata.flow_label = flow_label;
  falcon_packet->packet_type = falcon::PacketType::kPushSolicitedData;
  falcon_packet->falcon.payload_length = 1000;
  // Create OMNest packet and put the content of falcon_packet into it.
  auto packet = std::make_unique<inet::Packet>("FALCON");
  const auto& falcon_content_original = inet::makeShared<FalconPacket>();
  falcon_content_original->setChunkLength(
      inet::units::values::B(GetFalconPacketLength(falcon_packet.get())));
  falcon_content_original->setFalconContent(*falcon_packet);
  packet->insertAtBack(falcon_content_original);

  InsertUdpHeader(packet.get(), inet::L3Address(src_ip_address.c_str()),
                  inet::L3Address(dest_ip_address.c_str()), src_port,
                  dest_port);
  InsertIpv6Header(packet.get(), 0);

  // Compute and add 5-tuple hash tag to the packet.
  auto hash_tag = packet->addTag<Hash5TupleTag>();
  hash_tag->setHash5tuple(GenerateHashValueForPacket(
      src_ip_address, dest_ip_address, src_port, dest_port,
      falcon_packet->metadata.flow_label));

  return packet;
}

inet::Packet* OmnestPacketBuilder::CreateFalconPacket(
    const std::string& src_ip_address, const std::string& dest_ip_address,
    uint32_t src_port, uint32_t dest_port, uint16_t packet_payload,
    uint32_t dest_cid) {
  auto falcon_packet = std::make_unique<Packet>();
  // Sets the FALCON packet fields for testing the FALCON packet extraction.
  falcon_packet->metadata.traffic_class = 3;
  falcon_packet->metadata.flow_label = kDefaultFlowLabel;
  falcon_packet->falcon.psn = 100;
  falcon_packet->timestamps.sent_timestamp = absl::Nanoseconds(10);
  falcon_packet->packet_type = falcon::PacketType::kPushSolicitedData;
  falcon_packet->falcon.payload_length = packet_payload;
  falcon_packet->falcon.dest_cid = dest_cid;
  // Consturcuts the OMNest FALCON packet.
  const auto& falcon = inet::makeShared<FalconPacket>();
  falcon->setFalconContent(*falcon_packet);
  falcon->setChunkLength(inet::units::values::B(
      GetFalconPacketLength(falcon_packet.get()) + PspHeaderLength +
      PspIntegrityChecksumValueLength));
  // Insert FALCON packet into a INET packet for transmission.
  auto packet = new inet::Packet("FALCON");
  packet->insertAtBack(falcon);

  InsertUdpHeader(packet, inet::L3Address(src_ip_address.c_str()),
                  inet::L3Address(dest_ip_address.c_str()), src_port,
                  dest_port);
  InsertIpv6Header(packet, 0);
  InsertFrameHeader(packet);

  // Compute and add 5-tuple hash tag to the packet.
  auto hash_tag = packet->addTag<Hash5TupleTag>();
  hash_tag->setHash5tuple(GenerateHashValueForPacket(
      src_ip_address, dest_ip_address, src_port, kDefaultUdpPort,
      falcon_packet->metadata.flow_label));

  // No phy header in the packet.
  packet->popAtFront<inet::EthernetPhyHeader>();
  return packet;
}

void OmnestPacketBuilder::HandlePfc(inet::Packet* packet) {
  VLOG(2) << "<<<<<Receive PFC: " << host_id_
          << " @time: " << omnetpp::simTime().str()
          << " with queue len: " << tx_queue_.size();
  packet->popAtFront<inet::EthernetMacHeader>();
  const auto& control_frame = packet->peekAtFront<inet::EthernetPfcFrame>();

  int16_t class_enable = control_frame->getClassEnable();
  for (int i = 0; i < kClassesOfService; i++) {
    if ((class_enable >> i) & 1) {
      auto pause_duration =
          absl::Seconds((control_frame->getTimeClass(i) * kPauseUnitBits) /
                        transmission_channel_->getDatarate());
      CHECK_OK(env_->ScheduleEvent(rx_pfc_delay_, [this, i, pause_duration]() {
        SetPfcPaused(i, pause_duration);
      }));
    }
  }
}

void OmnestPacketBuilder::SetPfcPaused(uint32_t priority,
                                       absl::Duration pause_duration) {
  // Records a new PFC pause start event (check if stats_collection_ is nullptr
  // for unit test).
  if (stats_collection_ && pause_duration != absl::ZeroDuration() &&
      packet_builder_queue_states_[priority] !=
          PacketBuilderQueueState::kPaused) {
    if (stats_collection_flags_.enable_pfc()) {
      CHECK_OK(stats_collection_->UpdateStatistic(
          absl::Substitute(kStatVectorPfcPauseStart, priority),
          pfc_pause_start_ids_[priority],
          StatisticsCollectionConfig::TIME_SERIES_STAT));
    }
    pfc_pause_start_ids_[priority] += 1;
  }
  auto unpaused_time = env_->ElapsedTime() + pause_duration;
  tx_queue_unpause_times_[priority] = unpaused_time;
  packet_builder_queue_states_[priority] = PacketBuilderQueueState::kPaused;
  CHECK_OK(
      env_->ScheduleEvent(pause_duration, [this, priority, unpaused_time]() {
        WakeUpTxQueue(priority, unpaused_time);
      }));
}

void OmnestPacketBuilder::WakeUpTxQueue(uint priority,
                                        absl::Duration unpaused_time) {
  if (unpaused_time == tx_queue_unpause_times_[priority]) {
    VLOG(2) << ">>>>>>Resume from PFC: " << host_id_
            << " @time: " << omnetpp::simTime().str()
            << " with queue len: " << tx_queue_.size();
    // Records the PFC end event (check if stats_collection_ is nullptr for unit
    // test).
    if (stats_collection_) {
      if (stats_collection_flags_.enable_pfc()) {
        CHECK_OK(stats_collection_->UpdateStatistic(
            absl::Substitute(kStatVectorPfcPauseEnd, priority),
            pfc_pause_end_ids_[priority],
            StatisticsCollectionConfig::TIME_SERIES_STAT));
        pfc_pause_end_ids_[priority] += 1;
      }
    }
    packet_builder_queue_states_[priority] = PacketBuilderQueueState::kActive;
    EncapsulateAndSendTransportPacket(priority);
  }
}

uint32_t OmnestPacketBuilder::GetFlowLabelFromFalconMetadata(
    const inet::Packet* packet) const {
  const auto& udp_header = packet->peekAtFront<inet::UdpHeader>();
  const auto& falcon_packet =
      packet->peekDataAt<FalconPacket>(udp_header->getChunkLength());
  return falcon_packet->getFalconContent().metadata.flow_label;
}

void OmnestPacketBuilder::SetRandomDropProbability(double drop_probability) {
  drop_probability_ = drop_probability;
}

bool OmnestPacketBuilder::IsRandomDrop(const omnetpp::cMessage* packet) const {
  std::uniform_real_distribution<> dist(0, 1.0);
  if (dist(*env_->GetPrng()) >= (drop_probability_ / drop_burst_size_))
    return false;

  const auto& content = GetFalconPacket(packet)->getFalconContent();
  // If this is ACK/NACK and we don't drop them, return false.
  if (!drop_ack_ && (content.packet_type == falcon::PacketType::kAck ||
                     content.packet_type == falcon::PacketType::kNack))
    return false;
  return true;
}

}  // namespace isekai
