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

#ifndef ISEKAI_HOST_RNIC_OMNEST_PACKET_BUILDER_H_
#define ISEKAI_HOST_RNIC_OMNEST_PACKET_BUILDER_H_

#include <stddef.h>
#include <sys/types.h>

#include <cstdint>
#include <memory>
#include <queue>
#include <string>
#include <vector>

#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "absl/types/variant.h"
#include "gtest/gtest_prod.h"
#include "inet/common/IntrusivePtr.h"
#include "inet/common/packet/Packet.h"
#include "inet/linklayer/common/MacAddress.h"
#include "inet/networklayer/common/L3Address.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/constants.h"
#include "isekai/common/environment.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/common/packet.h"
#include "isekai/common/packet_m.h"
#include "isekai/host/rnic/network_channel_manager.h"
#include "isekai/host/rnic/omnest_environment.h"
#include "omnetpp/cdataratechannel.h"
#include "omnetpp/cmessage.h"
#include "omnetpp/cpacket.h"
#include "omnetpp/csimplemodule.h"
#include "omnetpp/simtime_t.h"

namespace isekai {

constexpr int kTtl = 255;
constexpr char kDefaultMacAddress[] = "02:00:00:00:00:01";
constexpr uint32_t kDefaultScid = 99;  // For testing only.
// The default value for UDP destination port.
constexpr uint16_t kDefaultUdpPort = 1000;
constexpr uint32_t kDefaultFlowLabel = 9;  // For testing only.

// Packet builder Tx queue has three states: idle, active and pause.
// When the state is idle, it means that packet builder queue can process and
// transmit packets to the network.
// When the state is active, it means that packet builder queue is processing /
// transmitting a packet to the network.
// When the state is pause, it means that packet builder queue is paused by PFC
// to send packets out.
enum class PacketBuilderQueueState {
  kIdle,
  kActive,
  kPaused,
};

// The class creates OMNest compatible packets. It also extracts FALCON packet
// from the received OMNest packets.
class OmnestPacketBuilder : public PacketBuilderInterface {
 public:
  OmnestPacketBuilder(omnetpp::cSimpleModule* host_module,
                      FalconInterface* falcon, Environment* env,
                      StatisticCollectionInterface* stats_collector,
                      const std::string& ip_address,
                      omnetpp::cDatarateChannel* transmission_channel,
                      const std::string& host_id);

  // First Encapsulates the FALCON packets from Tx queue (UDP, IPv6 and frame
  // headers will be inserted). Then sends the encapsulated packet out to the
  // network.
  void EncapsulateAndSendTransportPacket(uint16_t priority) override;

  // Decapsulates the received network packets and extract the FALCON packets.
  void ExtractAndHandleTransportPacket(
      omnetpp::cMessage* network_packet) override;

  // Enqueues a packet from traffic shaper. If the state is idle, call
  // EncapsulateAndSendFalconPacket() to process a packet from Tx queue.
  void EnqueuePacket(std::unique_ptr<Packet> packet) override;

  // Creates an INET packet with UDP and IPv6 headers. This function is ONLY
  // used network model to create packets for routing pipeline testing.
  std::unique_ptr<inet::Packet> CreatePacketWithUdpAndIpv6Headers(
      const std::string& src_ip_address, const std::string& dest_ip_address,
      uint32_t src_port, uint32_t dest_port,
      uint32_t flow_label = kDefaultFlowLabel);

  // Get Falcon packet length with a public function
  size_t GetFalconPacketLength(const Packet* falcon_packet) const;

 private:
  FRIEND_TEST(OmnestPacketBuilderTest, TestUdpHeader);
  FRIEND_TEST(OmnestPacketBuilderTest, TestIpHeader);
  FRIEND_TEST(OmnestPacketBuilderTest, TestFrameHeader);
  FRIEND_TEST(OmnestPacketBuilderTest, TestPacketExtraction);
  FRIEND_TEST(PacketUtilTest, TestGenerateHashValueForPacket);
  FRIEND_TEST(PacketUtilTest, TestGetPacketIpProtocol);
  FRIEND_TEST(PacketUtilTest, TestGetPacketUdpPorts);
  FRIEND_TEST(PacketUtilTest, TestGetPacketIpAddresses);
  FRIEND_TEST(PacketUtilTest, TestChangePacketTtl);
  FRIEND_TEST(PacketUtilTest, TestCommunicationBetweenHostAndRouter);
  FRIEND_TEST(PacketUtilTest, TestEcnMark);
  FRIEND_TEST(FlowRoundRobinQueueTest, Test);
  FRIEND_TEST(OmnestPacketBuilderTest, TestPfcHandling);
  FRIEND_TEST(OmnestPacketBuilderTest, TestEcnTag);
  FRIEND_TEST(OmnestPacketBuilderTest, TestWrongPacketSize);
  FRIEND_TEST(OmnestPacketBuilderTest, TestTriggerXonAndXoff);
  FRIEND_TEST(OmnestPacketBuilderTest, TestFlowLabel);
  FRIEND_TEST(OmnestPacketBuilderTest, TestRandomDrop);
  FRIEND_TEST(OmnestPacketBuilderTest, TestHashedPort);

  void EncapsulateAndSendFalconPacket(uint16_t priority);

  void InsertUdpHeader(inet::Packet* packet,
                       const inet::L3Address& source_address,
                       const inet::L3Address& destination_address,
                       unsigned int source_port,
                       unsigned int destination_port = kDefaultUdpPort);
  // Adds IPv6 header.
  void InsertIpv6Header(inet::Packet* packet, uint16_t priority);
  // Adds frame header.
  void InsertFrameHeader(inet::Packet* packet);
  // Sends out the packet and return the duration for the next packet build
  // time.
  absl::Duration SendoutPacket(omnetpp::cPacket* packet);

  // Processes frame header. Returns true if it is a meaningful L2 packet
  // (i.e. 1. it has ethernet header; 2. it is not a ethernet flow control
  // packet), otherwise false.
  bool RemoveFrameHeader(inet::Packet* packet);
  // Processes IPv6 header.
  bool RemoveIpv6Header(inet::Packet* packet);
  // Processes UDP header. Returns true if it has UDP header, otherwise false.
  bool RemoveUdpHeader(inet::Packet* packet);
  // Processes FALCON packet.
  void HandleFalconPacket(inet::Packet* packet, uint8_t forward_hops);

  // Updates the packet queueing delay stats.
  void UpdatePacketQueueingDelayStats(const omnetpp::cMessage* msg);

  std::string GetDestinationIpAddress(const Packet* falcon_packet) const {
    return falcon_packet->metadata.destination_ip_address.ToString();
  }

  // Enforces the packet rate in PB.
  absl::Duration GetNextPacketBuildEventTime(omnetpp::cPacket* packet) const {
    auto packet_trans_delay = GetPacketTransmissionDelay(packet);
    auto next_build_event_time = packet_trans_delay > packet_build_interval_
                                     ? packet_trans_delay
                                     : packet_build_interval_;
    return next_build_event_time;
  }

  absl::Duration GetPacketTransmissionDelay(omnetpp::cPacket* packet) const {
    return absl::Nanoseconds(
        transmission_channel_->calculateDuration(packet).inUnit(
            kOmnestTimeResolution));
  }

  // Creates a packet with all layer headers. This function is used ONLY in unit
  // tests.
  inet::Packet* CreateFalconPacket(const std::string& src_ip_address,
                                   const std::string& dest_ip_address,
                                   uint32_t src_port, uint32_t dest_port,
                                   uint16_t packet_payload = 1000,
                                   uint32_t dest_cid = 0);

  // Once PFC is received, set the packet builder state to PAUSE and stop
  // sending packets.
  void HandlePfc(inet::Packet* packet);
  // Sets the unpause time of the corresponding tx queue.
  void SetPfcPaused(uint32_t priority, absl::Duration pause_duration);
  // Wakes up the corresponding tx queue to send packets after the PFC pause end
  // time expires.
  void WakeUpTxQueue(uint priority, absl::Duration unpaused_time);
  // Triggers Xoff if the TX queue threshold is crossed. Triggers Xon if TX
  // queue becomes available.
  void SetFalconFlowControl(bool xoff);
  // Gets the flow label from FALCON packet metadata after UDP header is
  // inserted.
  uint32_t GetFlowLabelFromFalconMetadata(const inet::Packet* packet) const;
  // Set the random drop probability.
  void SetRandomDropProbability(double drop_probability);
  // Decide if random drop or not.
  bool IsRandomDrop(const omnetpp::cMessage* packet) const;
  // Gets the FALCON packet from the built OMNest cPacket, which includes
  // headers of all layers (from PHY to FALCON).
  inet::IntrusivePtr<const FalconPacket> GetFalconPacket(
      const omnetpp::cMessage* packet) const;
  // Implementation of CRC-16-CCITT (See
  // https://en.wikipedia.org/wiki/Computation_of_cyclic_redundancy_checks#Bit_ordering_(endianness))
  // to generate a hashed port.
  uint16_t GetHashedPort(absl::string_view str) const;

  omnetpp::cSimpleModule* const host_module_;
  absl::variant<FalconInterface*> transport_;
  // Holds a pointer to the OMNest environment.
  Environment* const env_;
  StatisticCollectionInterface* const stats_collection_;
  static StatisticsCollectionConfig::PacketBuilderFlags stats_collection_flags_;
  // Stores the source ip address.
  const std::string source_ip_address_;
  // The transmission channel of the packet builder.
  omnetpp::cDatarateChannel* const transmission_channel_;
  // Stores the unique MAC address.
  inet::MacAddress unique_mac_{kDefaultMacAddress};
  // Records the totoal amount of received packets in bits.
  double total_received_packet_size_ = 0;
  // Records the dropped packets due to wrong destination ip address.
  uint64_t wrong_destination_ip_address_discards_ = 0;
  // Records the dropped packets due to the size exceeding MTU.
  uint64_t wrong_outgoing_packet_size_discards_ = 0;
  uint64_t random_drops_ = 0;
  // Records the total number of dropped packets.
  uint64_t total_discards_ = 0;
  // Records the total number of tx/rx packets.
  uint64_t cumulative_tx_packets_ = 0;
  uint64_t cumulative_rx_packets_ = 0;
  uint64_t cumulative_tx_bytes_ = 0;
  uint64_t cumulative_rx_bytes_ = 0;
  // Tracks the state of the packet builder per Tx queue.
  std::vector<PacketBuilderQueueState> packet_builder_queue_states_;
  // This is a simplification; In hardware, the traffic shaper passes the packet
  // metadata to the Packet Builder which builds the complete packet onto which
  // ICE performs encryption, which finally puts it into one of 8 egress queues
  // drained by the ETH/GNET egress arbiter.
  //
  // schedule packets onto the wire.
  std::queue<std::unique_ptr<Packet>> tx_queue_;
  // The unpause_time_ (in nanoseconds) represents the PFC pause end time of a
  // tx queue if PFC is supported.
  std::vector<absl::Duration> tx_queue_unpause_times_;
  bool ecn_enabled_ = true;
  bool support_pfc_ = false;
  // The delay to handle PFC frame in nanoseconds.
  absl::Duration rx_pfc_delay_ = absl::Nanoseconds(100);
  std::string const host_id_;
  std::vector<uint64_t> pfc_pause_start_ids_;
  std::vector<uint64_t> pfc_pause_end_ids_;
  // Maximum network MTU supported by the packet builder. If a packet larger
  // than this is sent, it will result in an error. This value re-computed if
  // RDMA MTU is provided in the simulation config.
  uint32_t mtu_size_ = kDefaultRdmaMtu + kEthernetHeader + kIpv6Header +
                       kUdpHeader + kPspHeader + kFalconHeader +
                       kFalconOpHeader + kRdmaHeader + kPspTrailer +
                       kEthernetPreambleFCS;
  // The number of packets that PB can build per second (in million packets per
  // second). 0 means unlimited rate.
  uint32_t packet_rate_mpps_ = 200;
  // The packet build interval is 1 / packet_rate_mpps_.
  absl::Duration packet_build_interval_ =
      absl::Microseconds(1.0 / packet_rate_mpps_);
  // If the number of packets hold by Tx queue is equal or larger than the
  // threshold, PB should send Xoff to FALCON to stop sending packets.
  uint32_t tx_queue_length_threshold_ = -1;
  // Random drop probability.
  double drop_probability_ = 0;
  // Random drop burst size.
  int drop_burst_size_ = 1;
  // Count the number of drops remains in a burst of drop.
  uint32_t remaining_drop_in_burst_ = 0;
  // Random drop ACK/NACK packets or not.
  bool drop_ack_ = false;
  // If recording T1 after packet builder.
  bool record_t1_after_packet_builder_ = false;
  absl::Duration falcon_xoff_triggered_time_;
  double falcon_xoff_duration_us_ = 0;
  // Enforces any added delays set in the packet metadata and responsible for
  // actually pushing all the packets on the wire.
  std::unique_ptr<NetworkChannelManager> network_channel_manager_;
};

}  // namespace isekai

#endif  // ISEKAI_HOST_RNIC_OMNEST_PACKET_BUILDER_H_
